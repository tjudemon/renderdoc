/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2016-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "LiveCapture.h"
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QHash>
#include <QHostInfo>
#include <QMenu>
#include <QMetaProperty>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QScrollBar>
#include <QSet>
#include <QStyledItemDelegate>
#include <QToolBar>
#include <QToolButton>
#include "Code/QRDUtils.h"
#include "Code/Resources.h"
#include "Code/qprocessinfo.h"
#include "Widgets/Extended/RDLabel.h"
#include "Windows/MainWindow.h"
#include "toolwindowmanager/ToolWindowManager.h"
#include "ui_LiveCapture.h"

#if defined(Q_OS_WIN32)
// These SDK headers require windows.h to have been included first.
// clang-format off
#include <windows.h>
#include <tlhelp32.h>
#include <wbemidl.h>
// clang-format on

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "wbemuuid.lib")

// Child injection happens before application entry. An Electron NodeService may
// fail to load the library at that point but accept ordinary attachment after
// initialisation. This monitor is scoped to the launcher and capture options
// chosen in the GUI; it never attaches to an unrelated rendering application.
class ElectronLauncherMonitor
{
public:
  ElectronLauncherMonitor(uint32_t rootPID, const QString &path, const QString &capturefile,
                          const CaptureOptions &options)
      : m_RootPID(rootPID),
        m_Path(QFileInfo(path).absoluteFilePath()),
        m_CaptureFile(capturefile),
        m_Options(options)
  {
    m_Clock.start();
    if(!options.hookIntoChildren || path.isEmpty() ||
       !QFileInfo(QFileInfo(path).absolutePath() + lit("/LICENSE.electron.txt")).exists())
      return;

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    m_Uninitialise = SUCCEEDED(hr);
    if(FAILED(hr) && hr != RPC_E_CHANGED_MODE)
      return;

    IWbemLocator *locator = NULL;
    hr = CoCreateInstance(CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                          (void **)&locator);
    if(FAILED(hr))
      return;
    BSTR ns = SysAllocString(L"ROOT\\CIMV2");
    hr = locator->ConnectServer(ns, NULL, NULL, NULL, 0, NULL, NULL, &m_Services);
    SysFreeString(ns);
    locator->Release();
    if(SUCCEEDED(hr))
      hr = CoSetProxyBlanket(m_Services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                             RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    if(FAILED(hr))
    {
      if(m_Services)
        m_Services->Release();
      m_Services = NULL;
      qWarning() << "Electron launcher helper discovery unavailable:" << uint32_t(hr);
      SetStatus("Launcher helper discovery unavailable");
    }
    m_Timer.start();
    if(m_Services)
    {
      qInfo() << "Monitoring Electron launcher helper for PID" << m_RootPID;
      SetStatus("Waiting for launcher helper");
    }
  }

  ~ElectronLauncherMonitor()
  {
    if(m_Command.state() != QProcess::NotRunning)
    {
      m_Command.kill();
      m_Command.waitForFinished(1000);
    }
    if(m_HelperProcess)
      CloseHandle(m_HelperProcess);
    if(m_Services)
      m_Services->Release();
    if(m_Uninitialise)
      CoUninitialize();
  }

  const QString &StatusText() const { return m_Status; }

  bool CanRetryAfterApplication() const
  {
    return m_WaitingForApplication.contains(m_HelperIdentity);
  }

  void RetryAfterApplicationStart()
  {
    // Only the button permits one additional round. Application startup and
    // elapsed time never trigger attachment. Helper checks still apply.
    if(CanRetryAfterApplication())
      m_RequestedRetry = m_HelperIdentity;
  }

  // Called only by the connection worker. A successful result is added to the
  // normal child list so the helper and its new children can be opened in the UI.
  uint32_t Poll(uint32_t &helperPID)
  {
    if(!m_Services || (m_Timer.isValid() && m_Timer.elapsed() < 1000))
      return 0;
    m_Timer.restart();

    if(m_HelperProcess)
      return FinishAttachment(helperPID);

    QString query = lit("SELECT ProcessId,ExecutablePath,CommandLine FROM Win32_Process "
                        "WHERE ParentProcessId = %1")
                        .arg(m_RootPID);
    BSTR language = SysAllocString(L"WQL");
    BSTR statement = SysAllocString((const wchar_t *)query.utf16());
    IEnumWbemClassObject *enumerator = NULL;
    HRESULT hr = m_Services->ExecQuery(language, statement,
                                       WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL,
                                       &enumerator);
    SysFreeString(language);
    SysFreeString(statement);
    if(FAILED(hr) || !enumerator)
      return 0;

    QVector<uint32_t> candidates;
    while(true)
    {
      IWbemClassObject *process = NULL;
      ULONG count = 0;
      hr = enumerator->Next(1000, 1, &process, &count);
      if(FAILED(hr) || count == 0)
        break;
      QString path = QFileInfo(StringProperty(process, L"ExecutablePath")).absoluteFilePath();
      QString cmd = StringProperty(process, L"CommandLine");
      // Do not log command lines: launchers may pass authentication tokens.
      if(path.compare(m_Path, Qt::CaseInsensitive) == 0 &&
         QRegularExpression(lit("(?:^|\\s)\"?--type(?:=|\\s+)\"?utility\"?(?=\\s|$)"))
             .match(cmd)
             .hasMatch() &&
         QRegularExpression(lit("(?:^|\\s)\"?--utility-sub-type(?:=|\\s+)\"?"
                                "node\\.mojom\\.NodeService\"?(?=\\s|$)"))
             .match(cmd)
             .hasMatch())
      {
        VARIANT value;
        VariantInit(&value);
        if(SUCCEEDED(process->Get(L"ProcessId", 0, &value, NULL, NULL)))
        {
          if(value.vt == VT_I4)
            candidates.push_back(uint32_t(value.lVal));
          else if(value.vt == VT_UI4)
            candidates.push_back(uint32_t(value.ulVal));
        }
        VariantClear(&value);
      }
      process->Release();
    }
    enumerator->Release();
    if(hr != WBEM_S_FALSE)
      return 0;
    // Never guess when multiple utility services match.
    if(candidates.size() != 1)
      return 0;

    uint32_t pid = candidates[0];
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if(!process)
      return 0;
    wchar_t executable[32768] = {};
    DWORD executableLength = DWORD(ARRAY_COUNT(executable));
    if(!QueryFullProcessImageNameW(process, 0, executable, &executableLength) ||
       QFileInfo(QString::fromWCharArray(executable, int(executableLength)))
               .absoluteFilePath()
               .compare(m_Path, Qt::CaseInsensitive) != 0)
    {
      CloseHandle(process);
      return 0;
    }
    FILETIME created = {}, exited = {}, kernel = {}, user = {}, now = {};
    bool timed = GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
    GetSystemTimeAsFileTime(&now);
    uint64_t birth = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    uint64_t current = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    QString identity = QFormatStr("%1:%2").arg(pid).arg(qulonglong(birth));
    // Give the initial child hook time to finish. Failed ordinary attachments
    // get at most three attempts, spaced out while the launcher initialises.
    Attempt &attempt = m_Attempts[identity];
    if(!timed || current < birth || current - birth < 100000000 || m_Clock.elapsed() < attempt.nextTry)
    {
      CloseHandle(process);
      return 0;
    }
    int moduleState = ModuleState(pid);
    if(m_Attempted.contains(identity))
    {
      if(!m_WaitingForApplication.contains(identity) || moduleState != 0 ||
         m_RequestedRetry != identity)
      {
        if(m_WaitingForApplication.contains(identity) && moduleState == 1)
        {
          m_WaitingForApplication.remove(identity);
          SetStatus("Launcher helper library already loaded");
        }
        CloseHandle(process);
        return 0;
      }
      // The button permits one additional bounded round. Waiting alone never
      // resets attempts, and a successful helper is never attached again.
      m_WaitingForApplication.remove(identity);
      m_PostLaunchRetried.insert(identity);
      m_Attempted.remove(identity);
      m_RequestedRetry.clear();
      attempt.count = 0;
      attempt.nextTry = 0;
      qInfo() << "User requested Electron launcher helper retry:" << pid;
    }
    if(moduleState != 0)
    {
      CloseHandle(process);
      if(moduleState == 1)
        SetStatus("Launcher helper library already loaded");
      return 0;
    }
    DWORD exitCode = 0;
    if(!GetExitCodeProcess(process, &exitCode) || exitCode != STILL_ACTIVE)
    {
      CloseHandle(process);
      return 0;
    }
    QString tool = QCoreApplication::applicationDirPath() + lit("/gfxcapcmd.exe");
    if(!QFileInfo(tool).isFile())
    {
      CloseHandle(process);
      m_Attempted.insert(identity);
      SetStatus("Launcher helper attachment failed");
      qWarning() << "Electron launcher helper attachment unavailable: missing" << tool;
      return 0;
    }

    ++attempt.count;
    m_HelperProcess = process;
    m_HelperPID = pid;
    m_HelperIdentity = identity;
    m_AttachmentTimer.start();
    m_Command.setProcessChannelMode(QProcess::MergedChannels);
    m_Command.setWorkingDirectory(QFileInfo(m_Path).absolutePath());
    m_Command.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
    m_Command.start(tool, CaptureArguments(pid));
    SetStatus("Attaching launcher helper");
    qInfo() << "Attaching Electron launcher helper via gfxcapcmd:" << pid << "attempt"
            << attempt.count;
    m_Command.waitForStarted(1000);
    return 0;
  }

private:
  void SetStatus(const char *text) { m_Status = QCoreApplication::translate("LiveCapture", text); }

  QStringList CaptureArguments(uint32_t pid) const
  {
    QStringList args = {lit("inject"), lit("--PID"), QString::number(pid), lit("--capture-file"),
                        m_CaptureFile};
    if(!m_Options.allowVSync)
      args << lit("--opt-disallow-vsync");
    if(!m_Options.allowFullscreen)
      args << lit("--opt-disallow-fullscreen");
    if(m_Options.apiValidation)
      args << lit("--opt-api-validation");
    if(!m_Options.debugOutputMute)
      args << lit("--opt-api-validation-unmute");
    if(m_Options.captureCallstacks)
      args << lit("--opt-capture-callstacks");
    if(m_Options.captureCallstacksOnlyActions)
      args << lit("--opt-capture-callstacks-only-actions");
    if(m_Options.verifyBufferAccess)
      args << lit("--opt-verify-buffer-access");
    if(m_Options.hookIntoChildren)
      args << lit("--opt-hook-children");
    if(m_Options.refAllResources)
      args << lit("--opt-ref-all-resources");
    if(m_Options.captureAllCmdLists)
      args << lit("--opt-capture-all-cmd-lists");
    args << lit("--opt-delay-for-debugger") << QString::number(m_Options.delayForDebugger);
    args << lit("--opt-soft-memory-limit") << QString::number(m_Options.softMemoryLimit);
    return args;
  }

  uint32_t FinishAttachment(uint32_t &helperPID)
  {
    m_Command.waitForFinished(0);
    bool timedOut = m_Command.state() != QProcess::NotRunning &&
                    m_AttachmentTimer.elapsed() > 30000 + qint64(m_Options.delayForDebugger) * 1000;
    if(m_Command.state() != QProcess::NotRunning && !timedOut)
      return 0;
    if(timedOut)
    {
      // Stop only our CLI. Never terminate the launcher or its application.
      m_Command.kill();
      m_Command.waitForFinished(1000);
    }

    QString output = QString::fromLocal8Bit(m_Command.readAll());
    QRegularExpression ids(lit("(?:^|[\\r\\n])Launched as ID ([0-9]+)(?=[\\r\\n]|$)"));
    QRegularExpressionMatchIterator matches = ids.globalMatch(output);
    uint32_t ident = 0;
    if(matches.hasNext())
      ident = matches.next().captured(1).toUInt();
    DWORD exitCode = 0;
    bool alive = GetExitCodeProcess(m_HelperProcess, &exitCode) && exitCode == STILL_ACTIVE;
    int moduleState = alive ? ModuleState(m_HelperPID) : -1;
    bool loaded = !timedOut && m_Command.exitStatus() == QProcess::NormalExit && ident != 0 &&
                  !matches.hasNext() && ident == uint32_t(m_Command.exitCode()) &&
                  !output.contains(lit("Failed to inject:")) && moduleState == 1;
    CloseHandle(m_HelperProcess);
    m_HelperProcess = NULL;
    if(loaded)
    {
      m_Attempted.insert(m_HelperIdentity);
      SetStatus("Launcher helper attached; start application");
      qInfo() << "Electron launcher helper attached:" << m_HelperPID << "connection" << ident;
      helperPID = m_HelperPID;
      return ident;
    }

    Attempt &attempt = m_Attempts[m_HelperIdentity];
    // A timed-out injector may have left a pending remote call. Do not retry it,
    // or reattach when the library is present but configuration is unverified.
    bool retry = !timedOut && moduleState == 0 && attempt.count < 3;
    if(retry)
    {
      attempt.nextTry = m_Clock.elapsed() + 5000;
      SetStatus("Preparing launcher capture; retrying");
    }
    else
    {
      m_Attempted.insert(m_HelperIdentity);
      if(!timedOut && moduleState == 0 && !m_PostLaunchRetried.contains(m_HelperIdentity))
      {
        m_WaitingForApplication.insert(m_HelperIdentity);
        SetStatus("Close application, then retry attachment");
      }
      else
        SetStatus("Launcher helper attachment failed");
    }
    qWarning() << "Electron launcher helper attachment failed:" << m_HelperPID << "attempt"
               << attempt.count << "exit" << m_Command.exitCode() << "timeout" << timedOut
               << (output.isEmpty() ? m_Command.errorString() : output.trimmed());
    return 0;
  }

  static QString StringProperty(IWbemClassObject *object, const wchar_t *name)
  {
    VARIANT value;
    VariantInit(&value);
    QString result;
    if(SUCCEEDED(object->Get(name, 0, &value, NULL, NULL)) && value.vt == VT_BSTR && value.bstrVal)
      result = QString::fromWCharArray(value.bstrVal, int(SysStringLen(value.bstrVal)));
    VariantClear(&value);
    return result;
  }

  // -1 = unknown or another build, 0 = readable and absent, 1 = this build loaded.
  static int ModuleState(uint32_t pid)
  {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if(snapshot == INVALID_HANDLE_VALUE)
      return -1;
    MODULEENTRY32W module = {};
    module.dwSize = sizeof(module);
    if(!Module32FirstW(snapshot, &module))
    {
      CloseHandle(snapshot);
      return -1;
    }
    int state = 0;
    QString expected = QCoreApplication::applicationDirPath() + lit("/gfxcap.dll");
    do
    {
      QString name = QString::fromWCharArray(module.szModule);
      if(name.compare(lit("gfxcap.dll"), Qt::CaseInsensitive) == 0 ||
         name.compare(lit("renderdoc.dll"), Qt::CaseInsensitive) == 0)
      {
        QString path = QFileInfo(QString::fromWCharArray(module.szExePath)).absoluteFilePath();
        if(path.compare(expected, Qt::CaseInsensitive) != 0)
        {
          CloseHandle(snapshot);
          return -1;
        }
        state = 1;
      }
    } while(Module32NextW(snapshot, &module));
    if(GetLastError() != ERROR_NO_MORE_FILES)
      state = -1;
    CloseHandle(snapshot);
    return state;
  }

  uint32_t m_RootPID;
  QString m_Path, m_CaptureFile;
  CaptureOptions m_Options;
  bool m_Uninitialise = false;
  IWbemServices *m_Services = NULL;
  QElapsedTimer m_Timer;
  QElapsedTimer m_Clock, m_AttachmentTimer;
  QProcess m_Command;
  HANDLE m_HelperProcess = NULL;
  uint32_t m_HelperPID = 0;
  QString m_HelperIdentity, m_Status;
  QString m_RequestedRetry;
  struct Attempt
  {
    int count = 0;
    qint64 nextTry = 0;
  };
  QHash<QString, Attempt> m_Attempts;
  QSet<QString> m_Attempted;
  QSet<QString> m_WaitingForApplication, m_PostLaunchRetried;
};
#endif

static const int PIDRole = Qt::UserRole + 1;
static const int IdentRole = Qt::UserRole + 2;
static const int CapPtrRole = Qt::UserRole + 3;

class NameEditOnlyDelegate : public QStyledItemDelegate
{
public:
  LiveCapture *live;
  NameEditOnlyDelegate(LiveCapture *l) : live(l) {}
  void setEditorData(QWidget *editor, const QModelIndex &index) const override
  {
    QByteArray n = editor->metaObject()->userProperty().name();
    QListWidgetItem *item = live->ui->captures->item(index.row());

    if(!n.isEmpty() && item)
    {
      LiveCapture::Capture *cap = live->GetCapture(item);
      if(cap)
        editor->setProperty(n, cap->name);
    }
  }

  void setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const override
  {
    QByteArray n = editor->metaObject()->userProperty().name();
    QListWidgetItem *item = live->ui->captures->item(index.row());

    if(!n.isEmpty() && item)
    {
      LiveCapture::Capture *cap = live->GetCapture(item);
      if(cap)
      {
        cap->name = editor->property(n).toString();
        item->setText(live->MakeText(cap));
      }
    }
  }
};

LiveCapture::LiveCapture(ICaptureContext &ctx, const QString &hostname, const QString &friendlyname,
                         uint32_t ident, MainWindow *main, QWidget *parent)
    : QFrame(parent),
      ui(new Ui::LiveCapture),
      m_Ctx(ctx),
      m_Hostname(hostname),
      m_HostFriendlyname(friendlyname),
      m_RemoteIdent(ident),
      m_Main(main)
{
  ui->setupUi(this);

  m_Disconnect.release();

  QObject::connect(&childUpdateTimer, &QTimer::timeout, this, &LiveCapture::childUpdate);
  childUpdateTimer.setSingleShot(false);
  childUpdateTimer.setInterval(1000);
  childUpdateTimer.start();

  QObject::connect(&countdownTimer, &QTimer::timeout, this, &LiveCapture::captureCountdownTick);
  countdownTimer.setSingleShot(true);
  countdownTimer.setInterval(1000);

  childUpdate();

  ui->previewSplit->setCollapsible(1, true);
  ui->previewSplit->setSizes({1, 0});

  QObject::connect(ui->preview, &RDLabel::clicked, this, &LiveCapture::preview_mouseClick);
  QObject::connect(ui->preview, &RDLabel::mouseMoved, this, &LiveCapture::preview_mouseMove);

  ui->preview->setMouseTracking(true);

  setTitle(tr("Connecting"));
  ui->connectionStatus->setText(tr("Connecting"));
  ui->connectionIcon->setPixmap(Pixmaps::hourglass(ui->connectionIcon));

  ui->apiIcon->setVisible(false);

  ui->triggerDelayedCapture->setEnabled(false);
  ui->triggerImmediateCapture->setEnabled(false);
  ui->queueCap->setEnabled(false);
  ui->cycleActiveWindow->setEnabled(false);

  ui->target->setText(QString());

  ui->progressLabel->setVisible(false);
  ui->progressBar->setVisible(false);

  m_LauncherRetryButton = new QToolButton(ui->statusGroup);
  m_LauncherRetryButton->setText(tr("Retry launcher attachment"));
  m_LauncherRetryButton->setToolTip(
      tr("If needed, start the application once from this launcher to initialise it. Close the "
         "application, click here, then start it again after attachment succeeds. Keep this "
         "launcher open."));
  m_LauncherRetryButton->setVisible(false);
  ui->gridLayout_2->addWidget(m_LauncherRetryButton, 5, 2, Qt::AlignLeft);
  QObject::connect(m_LauncherRetryButton, &QToolButton::clicked, this, [this]() {
    m_LauncherRetryButton->setEnabled(false);
    m_LauncherRetry.release();
  });

  ui->captures->setItemDelegate(new NameEditOnlyDelegate(this));

  ui->captures->verticalScrollBar()->setSingleStep(20);

  {
    QToolBar *bottomTools = new QToolBar(this);

    QWidget *rightAlign = new QWidget(this);
    rightAlign->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    bottomTools->addWidget(rightAlign);

    previewToggle = new QAction(tr("Preview"), this);
    previewToggle->setCheckable(true);

    bottomTools->addAction(previewToggle);

    QMenu *openMenu = new QMenu(tr("&Open in..."), this);
    QAction *thisAction = new QAction(tr("This instance"), this);
    newWindowAction = new QAction(tr("New instance"), this);

    openMenu->addAction(thisAction);
    openMenu->addAction(newWindowAction);

    openButton = new QToolButton(this);
    openButton->setText(tr("Open"));
    openButton->setPopupMode(QToolButton::MenuButtonPopup);
    openButton->setMenu(openMenu);

    bottomTools->addWidget(openButton);

    saveAction = new QAction(tr("Save"), this);
    deleteAction = new QAction(tr("Delete"), this);

    bottomTools->addAction(saveAction);
    bottomTools->addAction(deleteAction);

    QObject::connect(previewToggle, &QAction::toggled, this, &LiveCapture::previewToggle_toggled);
    QObject::connect(openButton, &QToolButton::clicked, this, &LiveCapture::openCapture_triggered);
    QObject::connect(thisAction, &QAction::triggered, this, &LiveCapture::openCapture_triggered);
    QObject::connect(newWindowAction, &QAction::triggered, this,
                     &LiveCapture::openNewWindow_triggered);
    QObject::connect(saveAction, &QAction::triggered, this, &LiveCapture::saveCapture_triggered);
    QObject::connect(deleteAction, &QAction::triggered, this, &LiveCapture::deleteCapture_triggered);

    QObject::connect(ui->captures, &RDListWidget::keyPress, this, &LiveCapture::captures_keyPress);

    ui->mainLayout->addWidget(bottomTools);
  }
}

LiveCapture::~LiveCapture()
{
  for(ClosedCallback cb : m_CloseCallbacks)
    cb(this);

  m_Main->LiveCaptureClosed(this);

  cleanItems();
  killThread();

  delete ui;
}

void LiveCapture::SetLauncherCapture(const QString &executable, const QString &capturefile,
                                     const CaptureOptions &options)
{
  if(m_Hostname.isEmpty() && options.hookIntoChildren)
  {
    m_LauncherExecutable = executable;
    m_LauncherCaptureFile = capturefile;
    m_LauncherOptions = options;
  }
}

bool LiveCapture::IsConnected()
{
  return m_Connected.available();
}

rdcarray<rdcstr> LiveCapture::GetAPIs()
{
  rdcarray<rdcstr> ret;
  for(QString api : m_APIs.keys())
    ret.push_back(api);
  return ret;
}

void LiveCapture::QueueCapture(int frameNumber, int numFrames)
{
  m_QueueCaptureFrameNum = frameNumber;
  m_CaptureNumFrames = numFrames;
  m_QueueCapture.release();
}

void LiveCapture::TimedCapture(float secondsDelay, int numFrames)
{
  ui->captureDelay->setValue(qMax(0.0f, secondsDelay));
  ui->numFrames->setValue(qMax(0.0f, float(numFrames)));

  on_triggerDelayedCapture_clicked();
}

void LiveCapture::CycleActiveWindow()
{
  on_cycleActiveWindow_clicked();
}

void LiveCapture::Close(bool discardUnsaved)
{
  if(!discardUnsaved)
  {
    if(!checkAllowClose())
      return;
  }

  cleanItems();
}

rdcarray<ConnectedTempCapture> LiveCapture::GetCaptures()
{
  rdcarray<ConnectedTempCapture> ret;
  ret.reserve(m_Captures.size());
  for(Capture *src : m_Captures)
  {
    ConnectedTempCapture dst;
    dst.captureID = src->remoteID;
    dst.api = src->api;
    dst.timestamp = src->timestamp;
    dst.frameNumber = src->frameNumber;
    ret.push_back(dst);
  }
  return ret;
}

void LiveCapture::OpenCapture(uint32_t ID)
{
  for(Capture *c : m_Captures)
  {
    if(c->remoteID == ID)
    {
      openCapture(c);
      return;
    }
  }
}

void LiveCapture::DeleteCapture(uint32_t ID, bool promptForSave)
{
  for(Capture *c : m_Captures)
  {
    if(c->remoteID == ID)
    {
      deleteCapture(c);
      return;
    }
  }
}

void LiveCapture::SaveCapture(uint32_t ID, rdcstr filename)
{
  for(Capture *c : m_Captures)
  {
    if(c->remoteID == ID)
    {
      saveCapture(c, filename);
      return;
    }
  }
}

rdcarray<uint32_t> LiveCapture::GetChildProcesses()
{
  rdcarray<uint32_t> ret;

  QMutexLocker l(&m_ChildrenLock);
  ret.reserve(m_Children.size());
  for(const ChildProcess &c : m_Children)
    ret.push_back(c.PID);

  return ret;
}

ICaptureConnection *LiveCapture::ConnectToChild(uint32_t pid)
{
  QMutexLocker l(&m_ChildrenLock);

  for(const ChildProcess &c : m_Children)
  {
    if(c.PID == pid)
    {
      LiveCapture *live =
          new LiveCapture(m_Ctx, m_Hostname, m_HostFriendlyname, c.ident, m_Main, m_Main);
      m_Main->ShowLiveCapture(live);
      return live;
    }
  }

  return NULL;
}

void LiveCapture::showEvent(QShowEvent *event)
{
  if(!m_ConnectThread)
  {
    m_ConnectThread = new LambdaThread([this]() { this->connectionThreadEntry(); });
    m_ConnectThread->start();
  }

  on_captures_itemSelectionChanged();
}

void LiveCapture::on_captures_mouseClicked(QMouseEvent *e)
{
  if(e->buttons() & Qt::RightButton && !ui->captures->selectedItems().empty())
  {
    QMenu contextMenu(this);

    QMenu contextOpenMenu(tr("&Open in..."), this);
    QAction thisAction(tr("This instance"), this);
    QAction newAction(tr("New instance"), this);

    contextOpenMenu.addAction(&thisAction);
    contextOpenMenu.addAction(&newAction);

    QAction contextRenameAction(tr("&Rename capture"), this);
    QAction contextSaveAction(tr("&Save"), this);
    QAction contextDeleteAction(tr("&Delete"), this);

    contextMenu.addAction(contextOpenMenu.menuAction());
    contextMenu.addAction(&contextRenameAction);
    contextMenu.addAction(&contextSaveAction);
    contextMenu.addAction(&contextDeleteAction);

    if(ui->captures->selectedItems().size() == 1)
    {
      newAction.setEnabled(GetCapture(ui->captures->selectedItems()[0])->local);
    }
    else
    {
      contextOpenMenu.setEnabled(false);
      contextRenameAction.setEnabled(false);
      contextSaveAction.setText(tr("&Save All"));
      contextDeleteAction.setText(tr("&Delete All"));
    }

    QObject::connect(&thisAction, &QAction::triggered, this, &LiveCapture::openCapture_triggered);
    QObject::connect(&newAction, &QAction::triggered, this, &LiveCapture::openNewWindow_triggered);
    QObject::connect(&contextRenameAction, &QAction::triggered,
                     [this]() { ui->captures->editItem(ui->captures->selectedItems()[0]); });
    QObject::connect(&contextSaveAction, &QAction::triggered, this,
                     &LiveCapture::saveCapture_triggered);
    QObject::connect(&contextDeleteAction, &QAction::triggered, this,
                     &LiveCapture::deleteCapture_triggered);

    m_ContextMenu = &contextMenu;
    RDDialog::show(&contextMenu, QCursor::pos());
    m_ContextMenu = NULL;
  }
}

void LiveCapture::on_captures_itemActivated(QListWidgetItem *item)
{
  openCapture_triggered();
}

void LiveCapture::on_childProcesses_itemActivated(QListWidgetItem *item)
{
  QList<QListWidgetItem *> sel = ui->childProcesses->selectedItems();
  if(sel.count() == 1)
  {
    uint32_t ident = sel[0]->data(IdentRole).toUInt();
    if(ident > 0)
    {
      LiveCapture *live =
          new LiveCapture(m_Ctx, m_Hostname, m_HostFriendlyname, ident, m_Main, m_Main);
      m_Main->ShowLiveCapture(live);
    }
  }
}

void LiveCapture::on_queueCap_clicked()
{
  m_CaptureNumFrames = (int)ui->numFrames->value();
  m_QueueCaptureFrameNum = (int)ui->captureFrame->value();
  m_QueueCapture.release();
}

void LiveCapture::on_triggerImmediateCapture_clicked()
{
  m_CaptureNumFrames = (int)ui->numFrames->value();
  m_TriggerCapture.release();
}

void LiveCapture::on_cycleActiveWindow_clicked()
{
  m_CycleWindow.release();
}

void LiveCapture::on_triggerDelayedCapture_clicked()
{
  if(ui->captureDelay->value() == 0.0)
  {
    on_triggerImmediateCapture_clicked();
  }
  else
  {
    m_CaptureCounter = (int)ui->captureDelay->value();
    countdownTimer.start();
    ui->triggerDelayedCapture->setEnabled(false);
    ui->triggerDelayedCapture->setText(tr("Triggering in %1s").arg(m_CaptureCounter));
  }
}

void LiveCapture::openCapture_triggered()
{
  if(ui->captures->selectedItems().size() == 1)
    openCapture(GetCapture(ui->captures->selectedItems()[0]));
}

void LiveCapture::openNewWindow_triggered()
{
  if(ui->captures->selectedItems().size() == 1)
  {
    Capture *cap = GetCapture(ui->captures->selectedItems()[0]);

    QString temppath = m_Ctx.TempCaptureFilename(lit("newwindow"));

    if(!cap->local)
    {
      RDDialog::critical(this, tr("Cannot open new instance"),
                         tr("Can't open capture in new instance with remote server in use"));
      return;
    }

    QFile f(cap->path);

    if(!f.copy(temppath))
    {
      RDDialog::critical(this, tr("Cannot save temporary capture"),
                         tr("Couldn't save capture to temporary location\n%1").arg(f.errorString()));
      return;
    }

    QStringList args;
    args << lit("--tempfile") << temppath;
    QProcess::startDetached(qApp->applicationFilePath(), args);
  }
}

void LiveCapture::saveCapture_triggered()
{
  if(ui->captures->selectedItems().size() == 1)
  {
    saveCapture(GetCapture(ui->captures->selectedItems()[0]), QString());
  }
  else
  {
    QString path = m_Main->GetSavePath(tr("Save All Captures As"));

    if(!path.isEmpty())
    {
      if(path.endsWith(lit(".rdc")))
        path.chop(4);

      // don't save duplicates if we have multiple captures from the same frame (possible if the
      // application is not presenting at all and using the API to capture)
      QMap<uint32_t, uint32_t> existingFiles;

      for(QListWidgetItem *item : ui->captures->selectedItems())
      {
        Capture *cap = GetCapture(item);

        QString filename = QFormatStr("%1-frame%2").arg(path).arg(cap->frameNumber);
        if(cap->frameNumber == ~0U)
          filename = QFormatStr("%1-capture").arg(path);

        if(existingFiles.contains(cap->frameNumber))
        {
          filename += QFormatStr("_%1").arg(existingFiles[cap->frameNumber]);
          existingFiles[cap->frameNumber]++;
        }
        else
        {
          // start on 2 next time
          existingFiles[cap->frameNumber] = 2;
        }

        saveCapture(cap, QFormatStr("%1.rdc").arg(filename));
      }
    }
  }
}

void LiveCapture::deleteCapture_triggered()
{
  bool allow = checkAllowDelete();

  if(!allow)
    return;

  QList<QListWidgetItem *> sel = ui->captures->selectedItems();

  for(QListWidgetItem *item : sel)
  {
    Capture *cap = GetCapture(item);

    deleteCapture(cap);
  }
}

void LiveCapture::childUpdate()
{
  // first do a small lock and check if the list is currently empty
  {
    QMutexLocker l(&m_ChildrenLock);

    if(m_Children.empty())
    {
      ui->childProcessLabel->setVisible(false);
      ui->childProcesses->setVisible(false);
    }
  }

  // We only compare the child processes for a local context
  const bool local = isLocal();

  // enumerate processes outside of the lock
  QProcessList processes;
  if(local)
  {
    processes = QProcessInfo::enumerate(false);
  }

  // now since we're adding and removing, we lock around the whole rest of the function. It won't be
  // too slow.
  {
    QMutexLocker l(&m_ChildrenLock);

    if(!m_Children.empty())
    {
      // remove any stale processes
      for(int i = 0; i < m_Children.size(); i++)
      {
        bool found = false;

        for(QProcessInfo &p : processes)
        {
          if(p.pid() == m_Children[i].PID)
          {
            found = true;
            break;
          }
        }

        if(!found && local)
        {
          if(m_Children[i].added)
          {
            for(int c = 0; c < ui->childProcesses->count(); c++)
            {
              QListWidgetItem *item = ui->childProcesses->item(c);
              if(item->data(PIDRole).toUInt() == m_Children[i].PID)
                delete ui->childProcesses->takeItem(c);
            }
          }

          // process expired/doesn't exist anymore
          m_Children.removeAt(i);

          // don't increment i, check the next element at i (if we weren't at the end
          i--;
        }
      }

      for(int i = 0; i < m_Children.size(); i++)
      {
        if(!m_Children[i].added)
        {
          QString name = tr("Unknown Process");

          // find the name
          for(QProcessInfo &p : processes)
          {
            if(p.pid() == m_Children[i].PID)
            {
              name = p.name();
              break;
            }
          }

          QString text = QFormatStr("%1 [PID %2]").arg(name).arg(m_Children[i].PID);

          m_Children[i].added = true;
          QListWidgetItem *item = new QListWidgetItem(text, ui->childProcesses);
          item->setData(PIDRole, QVariant(m_Children[i].PID));
          item->setData(IdentRole, QVariant(m_Children[i].ident));
          ui->childProcesses->addItem(item);
        }
      }
    }

    if(!m_Children.empty())
    {
      ui->childProcessLabel->setVisible(true);
      ui->childProcesses->setVisible(true);
    }
    else
    {
      ui->childProcessLabel->setVisible(false);
      ui->childProcesses->setVisible(false);
    }
  }
}

void LiveCapture::captureCountdownTick()
{
  m_CaptureCounter--;

  if(m_CaptureCounter == 0)
  {
    m_CaptureNumFrames = (int)ui->numFrames->value();
    ui->triggerDelayedCapture->setEnabled(true);
    ui->triggerDelayedCapture->setText(tr("Trigger After Delay"));
    m_TriggerCapture.release();
  }
  else
  {
    countdownTimer.start();
    ui->triggerDelayedCapture->setText(tr("Triggering in %1s").arg(m_CaptureCounter));
  }
}

void LiveCapture::killThread()
{
  if(m_ConnectThread)
  {
    m_Disconnect.acquire();
    m_ConnectThread->wait();
    m_ConnectThread->deleteLater();
  }
}

void LiveCapture::setTitle(const QString &title)
{
  setWindowTitle((!m_HostFriendlyname.isEmpty() ? (m_HostFriendlyname + lit(" - ")) : QString()) +
                 title);
}

LiveCapture::Capture *LiveCapture::GetCapture(QListWidgetItem *item)
{
  return (Capture *)item->data(CapPtrRole).value<void *>();
}

void LiveCapture::AddCapture(QListWidgetItem *item, Capture *cap)
{
  item->setData(CapPtrRole, QVariant::fromValue<void *>(cap));
}

QImage LiveCapture::MakeThumb(const QImage &screenshot)
{
  const QSizeF thumbSize(ui->captures->iconSize());
  const QSizeF imSize(screenshot.size());

  float x = 0, y = 0;
  float width = 0, height = 0;

  const float srcaspect = imSize.width() / imSize.height();
  const float dstaspect = thumbSize.width() / thumbSize.height();

  if(srcaspect > dstaspect)
  {
    width = thumbSize.width();
    height = width / srcaspect;

    y = (thumbSize.height() - height) / 2;
  }
  else
  {
    height = thumbSize.height();
    width = height * srcaspect;

    x = (thumbSize.width() - width) / 2;
  }

  QImage ret(thumbSize.width(), thumbSize.height(), QImage::Format_RGBA8888);
  ret.fill(Qt::transparent);
  QPainter paint(&ret);
  paint.drawImage(QRectF(x, y, width, height),
                  screenshot.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
  return ret;
}

bool LiveCapture::checkAllowDelete()
{
  bool needcheck = false;

  for(int i = 0; i < ui->captures->count(); i++)
  {
    Capture *cap = GetCapture(ui->captures->item(i));
    needcheck |= !cap->saved;
  }

  if(!needcheck || ui->captures->selectedItems().empty())
    return true;

  ToolWindowManager::raiseToolWindow(this);

  QMessageBox::StandardButton res =
      RDDialog::question(this, tr("Unsaved capture(s)", "", ui->captures->selectedItems().size()),
                         tr("Are you sure you wish to delete the capture(s)?\nAny capture "
                            "currently opened will be closed",
                            "", ui->captures->selectedItems().size()),
                         RDDialog::YesNoCancel);

  return (res == QMessageBox::Yes);
}

void LiveCapture::updateAPIStatus()
{
  QString apiStatus;

  bool nonpresenting = false;

  // add any fully working APIs first in the list.
  for(QString api : m_APIs.keys())
  {
    if(m_APIs[api].supported && m_APIs[api].presenting)
      apiStatus += lit(", <b>%1 (Active)</b>").arg(api);
  }

  // then add any problem APIs
  for(QString api : m_APIs.keys())
  {
    if(!m_APIs[api].supported)
    {
      apiStatus += tr(", %1 (Unsupported)").arg(api);
      if(!m_APIs[api].supportMessage.isEmpty())
        apiStatus += lit("\n") + m_APIs[api].supportMessage;
    }
    else if(!m_APIs[api].presenting)
    {
      apiStatus += tr(", %1 (Not Presenting)").arg(api);
      nonpresenting = true;
    }
  }

  // remove the redundant starting ", "
  apiStatus.remove(0, 2);

  apiStatus.replace(QLatin1Char('\n'), lit("<br>"));

  ui->apiStatus->setText(apiStatus);

  ui->apiIcon->setVisible(nonpresenting);
}

QString LiveCapture::MakeText(Capture *cap)
{
  QString text = cap->name;
  if(!cap->local)
    text += tr(" (Remote)");

  text += lit("\n") + cap->api;
  if(!cap->title.isEmpty())
    text += QFormatStr("\n%1").arg(cap->title);
  else if(cap->frameNumber == ~0U)
    text += tr("\nUser-defined Capture");
  else
    text += tr("\nFrame #%1").arg(cap->frameNumber);

  if(cap->byteSize > 0)
    text += QFormatStr(" (%1 MB)").arg(double(cap->byteSize) / 1000000.0, 0, 'f', 2);

  text += cap->timestamp.toString(lit("\nyyyy-MM-dd HH:mm:ss"));

  return text;
}

bool LiveCapture::checkAllowClose(int totalUnsavedCaptures, bool &noToAll)
{
  m_IgnoreThreadClosed = true;

  bool suppressRemoteWarning = false;

  QMessageBox::StandardButtons msgFlags = RDDialog::YesNoCancel;

  const int unsavedCaptures = unsavedCaptureCount();
  const bool multipleClosures = totalUnsavedCaptures > unsavedCaptures;

  if(unsavedCaptures > 1 || multipleClosures)
    msgFlags |= QMessageBox::NoToAll;

  for(int i = 0; i < ui->captures->count(); i++)
  {
    QListWidgetItem *item = ui->captures->item(i);
    Capture *cap = GetCapture(ui->captures->item(i));

    if(cap->saved)
      continue;

    ui->captures->clearSelection();
    ToolWindowManager::raiseToolWindow(this);
    ui->captures->setFocus();
    item->setSelected(true);

    QMessageBox::StandardButton res = QMessageBox::No;

    if(!suppressRemoteWarning && !noToAll)
    {
      QString frameName = tr("Frame #%1").arg(cap->frameNumber);
      if(cap->frameNumber == ~0U)
        frameName = tr("User-defined Capture");

      res = RDDialog::question(this, tr("Unsaved capture"),
                               tr("Save this capture '%1 %2' at %3?")
                                   .arg(cap->name)
                                   .arg(frameName)
                                   .arg(cap->timestamp.toString(lit("HH:mm:ss"))),
                               msgFlags);

      if(res == QMessageBox::NoToAll)
      {
        // if we're closing multiple connections make sure the user is sure of what they're doing
        if(multipleClosures)
        {
          QMessageBox::StandardButton res2 = RDDialog::question(
              this, tr("Discarding all captures"),
              tr("Multiple connections open have potentially unsaved captures, "
                 "this will discard all captures in all connections, are you sure?"));

          // if the user is sure, apply the no to all
          if(res2 == QMessageBox::Yes)
          {
            noToAll = true;
          }
          else
          {
            // otherwise if the user changed their mind at this stage, cancel everything rather than
            // trying to continue, to keep the flow simple and ensure the user is clear what is
            // happening at all points. We do not support discarding all captures in one connection
            // then individually filtering another.
            m_IgnoreThreadClosed = false;
            return false;
          }
        }
        else
        {
          // if we're not closing multiple, we can just immediately accept the 'no to all'
          noToAll = true;
        }

        res = QMessageBox::No;
      }
    }

    if(res == QMessageBox::Cancel)
    {
      m_IgnoreThreadClosed = false;
      return false;
    }

    // we either have to save or delete the capture. Make sure that if it's remote that we are able
    // to by having an active connection or replay context on that host.
    if(suppressRemoteWarning == false && !m_Connected.available() && !cap->local &&
       m_Ctx.Replay().CurrentRemote().Hostname() != rdcstr(m_Hostname))
    {
      QMessageBox::StandardButton res2 = RDDialog::question(
          this, tr("No active replay context"),
          tr("This capture is on remote host %1 and there is no active replay context on that "
             "host.\n")
                  .arg(m_HostFriendlyname) +
              tr("Without an active replay context the capture cannot be %1.\n\n")
                  .arg(tr(res == QMessageBox::Yes ? "saved" : "deleted")) +
              tr("Would you like to continue and discard this capture and any others, to be left "
                 "in the temporary folder on the remote machine?"),
          RDDialog::YesNoCancel);

      if(res2 == QMessageBox::Yes)
      {
        suppressRemoteWarning = true;
        res = QMessageBox::No;
      }
      else
      {
        m_IgnoreThreadClosed = false;
        return false;
      }
    }

    if(res == QMessageBox::Yes)
    {
      bool success = saveCapture(cap, QString());

      if(!success)
      {
        m_IgnoreThreadClosed = false;
        return false;
      }
    }
  }

  m_IgnoreThreadClosed = false;
  return true;
}

bool LiveCapture::checkAllowClose()
{
  bool dummy = false;
  return checkAllowClose(unsavedCaptureCount(), dummy);
}

void LiveCapture::openCapture(Capture *cap)
{
  cap->opened = true;

  if(!cap->local && m_Ctx.Replay().CurrentRemote().Hostname() != rdcstr(m_Hostname))
  {
    RDDialog::critical(
        this, tr("No active replay context"),
        tr("This capture is on remote host %1 and there is no active replay context on that "
           "host.\nYou can either save the capture locally, or switch to a replay context on %1.")
            .arg(m_HostFriendlyname));
    return;
  }

  m_Main->LoadCapture(cap->path, m_Ctx.Config().DefaultReplayOptions, !cap->saved, cap->local);
}

bool LiveCapture::saveCapture(Capture *cap, QString path)
{
  // if this is the current capture, do the save through the main window
  if(QString(m_Ctx.GetCaptureFilename()) == cap->path)
  {
    // if there's no target path, let the main window prompt for save.
    if(path.isEmpty())
      return m_Main->PromptSaveCaptureAs();
    else
      return m_Main->SaveCurrentCapture(path);
  }

  if(path.isEmpty())
  {
    path = m_Main->GetSavePath();

    if(path.isEmpty())
      return false;
  }

  if(QString(m_Ctx.GetCaptureFilename()) == path)
  {
    RDDialog::critical(this, tr("Cannot save"),
                       tr("Can't overwrite currently open capture at %1\n"
                          "Close the capture or save to another location.")
                           .arg(path));
    return false;
  }

  // we copy the temp capture to the desired path, but the capture item remains referring to the
  // temp path.
  // This ensures that if the user deletes the saved path we can still open or re-save it.
  if(cap->local)
  {
    QFile src(cap->path);
    QFile dst(path);

    // remove any existing file, the user was already prompted to overwrite
    if(dst.exists())
    {
      if(!dst.remove())
      {
        RDDialog::critical(this, tr("Cannot save"),
                           tr("Couldn't remove file at %1\n%2").arg(path).arg(dst.errorString()));
        return false;
      }
    }

    if(!src.copy(path))
    {
      RDDialog::critical(this, tr("Cannot save"),
                         tr("Couldn't copy file to %1\n%2").arg(path).arg(src.errorString()));
      return false;
    }
  }
  else if(m_Connected.available())
  {
    // if we have a current live connection, prefer using it
    m_CopyCaptureLocalPath = path;
    m_CopyCaptureID = cap->remoteID;

    m_CopyCapture.release();
  }
  else
  {
    if(m_Ctx.Replay().CurrentRemote().Hostname() != rdcstr(m_Hostname))
    {
      RDDialog::critical(this, tr("No active replay context"),
                         tr("This capture is on remote host %1 and there is no active replay "
                            "context on that host.\n")
                                 .arg(m_Hostname) +
                             tr("Without an active replay context the capture cannot be saved, "
                                "try switching to a replay context on %1.")
                                 .arg(m_Hostname));
      return false;
    }

    m_Ctx.Replay().CopyCaptureFromRemote(cap->path, path, this);

    if(!QFile::exists(path))
    {
      RDDialog::critical(this, tr("Cannot save"),
                         tr("File couldn't be transferred from remote host"));
      return false;
    }

    m_Ctx.Replay().DeleteCapture(cap->path, false);
  }

  // delete the temporary copy
  if(!cap->saved)
    m_Ctx.Replay().DeleteCapture(cap->path, cap->local);

  m_Main->RemoveRecentCapture(cap->path);
  cap->saved = true;
  cap->path = path;
  AddRecentFile(m_Ctx.Config().RecentCaptureFiles, path);
  m_Main->PopulateRecentCaptureFiles();
  return true;
}

void LiveCapture::deleteCapture(Capture *cap)
{
  if(!cap->saved)
  {
    if(cap->path == m_Ctx.GetCaptureFilename())
    {
      m_Main->takeCaptureOwnership();
      m_Ctx.CloseCapture();
    }
    else
    {
      // if connected, prefer using the live connection
      if(m_Connected.available() && !cap->local)
      {
        QMutexLocker l(&m_DeleteCapturesLock);
        m_DeleteCaptures.push_back(cap->remoteID);
      }
      else
      {
        m_Ctx.Replay().DeleteCapture(cap->path, cap->local);
      }

      if(cap->local)
      {
        m_Main->RemoveRecentCapture(cap->path);
      }
    }
  }

  m_Captures.removeOne(cap);
  delete cap;

  for(int row = 0; row < ui->captures->count(); row++)
  {
    QListWidgetItem *item = ui->captures->item(row);

    if(GetCapture(item) == cap)
    {
      delete ui->captures->takeItem(row);
      break;
    }
  }
}

void LiveCapture::cleanItems()
{
  for(int i = 0; i < ui->captures->count(); i++)
  {
    Capture *cap = GetCapture(ui->captures->item(i));

    if(!cap->saved)
    {
      if(cap->path == m_Ctx.GetCaptureFilename())
      {
        m_Main->takeCaptureOwnership();
      }
      else
      {
        // if connected, prefer using the live connection
        if(m_Connected.available() && !cap->local)
        {
          QMutexLocker l(&m_DeleteCapturesLock);
          m_DeleteCaptures.push_back(cap->remoteID);
        }
        else
        {
          m_Ctx.Replay().DeleteCapture(cap->path, cap->local);
        }

        if(cap->local)
        {
          m_Main->RemoveRecentCapture(cap->path);
        }
      }
    }

    m_Captures.removeOne(cap);
    delete cap;
  }
  ui->captures->clear();
}

void LiveCapture::fileSaved(QString from, QString to)
{
  for(int i = 0; i < ui->captures->count(); i++)
  {
    Capture *cap = GetCapture(ui->captures->item(i));

    if(cap->path == from)
    {
      cap->path = to;
      cap->saved = true;
      cap->local = true;
    }
  }
}

int LiveCapture::unsavedCaptureCount()
{
  int ret = 0;

  for(int i = 0; i < ui->captures->count(); i++)
  {
    Capture *cap = GetCapture(ui->captures->item(i));

    if(!cap->saved)
      ret++;
  }

  return ret;
}

void LiveCapture::previewToggle_toggled(bool checked)
{
  if(m_IgnorePreviewToggle)
    return;

  if(checked)
    ui->previewSplit->setSizes({1, 1});
  else
    ui->previewSplit->setSizes({1, 0});
}

void LiveCapture::on_previewSplit_splitterMoved(int pos, int index)
{
  m_IgnorePreviewToggle = true;

  QList<int> sizes = ui->previewSplit->sizes();

  previewToggle->setChecked(sizes[1] != 0);

  m_IgnorePreviewToggle = false;
}

void LiveCapture::on_apiIcon_clicked(QMouseEvent *event)
{
  QDesktopServices::openUrl(QUrl(lit("https://renderdoc.org/docs/in_application_api.html")));
}

void LiveCapture::captures_keyPress(QKeyEvent *e)
{
  if(e->key() == Qt::Key_Delete)
  {
    deleteCapture_triggered();
  }
}

void LiveCapture::preview_mouseClick(QMouseEvent *e)
{
  QPoint mouse = QCursor::pos();
  if(e->buttons() & Qt::LeftButton)
  {
    previewDragStart = mouse;
    ui->preview->setCursor(QCursor(Qt::SizeAllCursor));
  }
}

void LiveCapture::preview_mouseMove(QMouseEvent *e)
{
  QPoint mouse = QCursor::pos();
  if(e->buttons() & Qt::LeftButton)
  {
    QScrollBar *h = ui->previewScroll->horizontalScrollBar();
    QScrollBar *v = ui->previewScroll->verticalScrollBar();

    h->setValue(h->value() + previewDragStart.x() - mouse.x());
    v->setValue(v->value() + previewDragStart.y() - mouse.y());

    previewDragStart = mouse;
  }
  else
  {
    ui->preview->unsetCursor();
  }
}

void LiveCapture::on_captures_itemSelectionChanged()
{
  int numSelected = ui->captures->selectedItems().size();

  openButton->setEnabled(numSelected == 1);
  saveAction->setEnabled(numSelected != 0);
  deleteAction->setEnabled(numSelected != 0);

  if(ui->captures->selectedItems().size() == 1)
  {
    QListWidgetItem *item = ui->captures->selectedItems()[0];
    Capture *cap = GetCapture(item);

    newWindowAction->setEnabled(cap->local);

    if(cap->thumb.width() > 0)
    {
      ui->preview->setPixmap(QPixmap::fromImage(cap->thumb));
      ui->preview->setMinimumSize(cap->thumb.size());
      ui->preview->setMaximumSize(cap->thumb.size());
    }
    else
    {
      ui->preview->setPixmap(QPixmap());
      ui->preview->setMinimumSize(QSize(16, 16));
      ui->preview->setMaximumSize(QSize(16, 16));
    }
  }
}

void LiveCapture::captureCopied(uint32_t ID, const QString &localPath)
{
  for(int i = 0; i < ui->captures->count(); i++)
  {
    QListWidgetItem *item = ui->captures->item(i);
    Capture *cap = GetCapture(ui->captures->item(i));

    if(cap && cap->remoteID == ID)
    {
      cap->local = true;
      cap->path = localPath;
      QFont f = item->font();
      f.setItalic(false);
      item->setFont(f);
      item->setText(MakeText(cap));
    }
  }
}

void LiveCapture::captureAdded(const QString &name, const NewCaptureData &newCapture)
{
  Capture *cap = new Capture();

  cap->name = name;

  cap->api = newCapture.api;

  cap->timestamp =
      QDateTime(QDate(1970, 1, 1), QTime(0, 0, 0), Qt::UTC).addSecs(newCapture.timestamp).toLocalTime();
  cap->byteSize = newCapture.byteSize;

  cap->thumb = QImage(newCapture.thumbnail.data(), newCapture.thumbWidth, newCapture.thumbHeight,
                      newCapture.thumbWidth * 3, QImage::Format_RGB888)
                   .copy(0, 0, newCapture.thumbWidth, newCapture.thumbHeight);

  cap->remoteID = newCapture.captureId;
  cap->saved = false;
  cap->path = newCapture.path;
  cap->local = newCapture.local;
  cap->frameNumber = newCapture.frameNumber;
  cap->title = newCapture.title;

  QListWidgetItem *item = new QListWidgetItem();
  item->setFlags(item->flags() | Qt::ItemIsEditable);
  item->setText(MakeText(cap));
  item->setIcon(QIcon(QPixmap::fromImage(MakeThumb(cap->thumb))));
  if(!newCapture.local)
  {
    QFont f = item->font();
    f.setItalic(true);
    item->setFont(f);
  }

  AddCapture(item, cap);

  ui->captures->addItem(item);

  m_Captures.push_back(cap);
}

void LiveCapture::connectionClosed()
{
  ui->progressLabel->setVisible(false);
  ui->progressBar->setVisible(false);

  if(m_IgnoreThreadClosed)
    return;

  if(ui->captures->count() <= 1)
  {
    if(ui->captures->count() == 1)
    {
      Capture *cap = GetCapture(ui->captures->item(0));

      // only auto-open a non-local capture if we are successfully connected
      // to this machine as a remote context
      if(!cap->local)
      {
        if(m_Ctx.Replay().CurrentRemote().Hostname() != rdcstr(m_Hostname))
          return;
      }

      // don't close if a dialog is open
      if(QApplication::activeModalWidget() || QApplication::activePopupWidget())
        return;

      if(cap->opened)
        return;

      openCapture(cap);

      // if the capture failed to load, don't proceed.
      if(!m_Ctx.IsCaptureLoaded())
        return;

      if(!cap->saved)
      {
        cap->saved = true;
        m_Main->takeCaptureOwnership();
      }
    }

    int childCount = 0;
    uint32_t ident0 = 0;
    {
      QMutexLocker l(&m_ChildrenLock);
      childCount = m_Children.count();
      if(childCount > 0)
        ident0 = m_Children[0].ident;
    }

    // auto-close and load capture if we got a capture. If we
    // don't have any captures but DO have child processes,
    // then don't close just yet.
    if(ui->captures->count() == 1 || childCount == 0)
    {
      // raise the texture viewer if it exists, instead of falling back to most likely the capture
      // executable dialog which is not useful.
      if(ui->captures->count() == 1 && m_Ctx.HasTextureViewer())
        m_Ctx.ShowTextureViewer();
      selfClose();
      return;
    }

    // if we have no captures and only one child, close and
    // open up a connection to it (similar to behaviour with
    // only one capture
    if(ui->captures->count() == 0 && childCount == 1)
    {
      LiveCapture *live = new LiveCapture(m_Ctx, m_Hostname, m_HostFriendlyname, ident0, m_Main);
      m_Main->ShowLiveCapture(live);
      selfClose();
      return;
    }
  }
}

void LiveCapture::selfClose()
{
  if(!m_SelfClosing)
    return;

  if(m_ContextMenu)
  {
    qInfo() << "preventing race";
    // hide the menu and close our window shortly after
    m_ContextMenu->close();
    QTimer *timer = new QTimer(this);
    QObject::connect(timer, &QTimer::timeout, [this]() { ToolWindowManager::closeToolWindow(this); });
    timer->setSingleShot(true);
    timer->start(250);
  }
  else
  {
    ToolWindowManager::closeToolWindow(this);
  }
}

void LiveCapture::connectionThreadEntry()
{
  ITargetControl *conn = GC_CreateTargetControl(m_Hostname, m_RemoteIdent, GetSystemUsername(), true);
  m_Connected.release();

  if(!conn || !conn->Connected())
  {
    if(conn)
      conn->Shutdown();

    GUIInvoke::call(this, [this]() {
      setTitle(tr("Connection failed"));
      ui->connectionStatus->setText(tr("Failed"));
      ui->connectionIcon->setPixmap(Pixmaps::del(ui->connectionIcon));

      connectionClosed();
    });

    m_Connected.acquire();
    return;
  }

  uint32_t pid = conn->GetPID();
  QString target = conn->GetTarget();

#if defined(Q_OS_WIN32)
  ElectronLauncherMonitor launcherMonitor(pid, m_LauncherExecutable, m_LauncherCaptureFile,
                                          m_LauncherOptions);
  QString launcherStatus;
  bool launcherCanRetry = false;
#endif

  GUIInvoke::call(this, [this, pid, target]() {
    if(!m_Connected.available())
      return;

    if(pid)
      setTitle(QFormatStr("%1 [PID %2]").arg(target).arg(pid));
    else
      setTitle(target);

    m_Target = target;

    ui->target->setText(windowTitle());
    ui->connectionIcon->setPixmap(Pixmaps::connect(ui->connectionIcon));
    ui->connectionStatus->setText(tr("Established"));
  });

  while(conn && conn->Connected())
  {
    if(m_TriggerCapture.tryAcquire())
    {
      conn->TriggerCapture((uint)m_CaptureNumFrames);
      m_CaptureNumFrames = 1;
    }

    if(m_QueueCapture.tryAcquire())
    {
      conn->QueueCapture((uint32_t)m_QueueCaptureFrameNum, (uint32_t)m_CaptureNumFrames);
      m_QueueCaptureFrameNum = 0;
      m_CaptureNumFrames = 1;
    }

    if(m_CopyCapture.tryAcquire())
    {
      conn->CopyCapture(m_CopyCaptureID, m_CopyCaptureLocalPath);
      m_CopyCaptureLocalPath = QString();
      m_CopyCaptureID = ~0U;
    }

    if(m_CycleWindow.tryAcquire())
    {
      conn->CycleActiveWindow();
    }

    QVector<uint32_t> dels;
    {
      QMutexLocker l(&m_DeleteCapturesLock);
      dels.swap(m_DeleteCaptures);
    }

    for(uint32_t del : dels)
      conn->DeleteCapture(del);

    if(!m_Disconnect.available())
    {
      conn->Shutdown();
      conn = NULL;
      m_Connected.acquire();
      return;
    }

#if defined(Q_OS_WIN32)
    if(m_LauncherRetry.tryAcquire())
      launcherMonitor.RetryAfterApplicationStart();
    uint32_t helperPID = 0;
    uint32_t helperIdent = launcherMonitor.Poll(helperPID);
    if(helperIdent)
    {
      ChildProcess child;
      child.PID = helperPID;
      child.ident = helperIdent;
      {
        QMutexLocker lock(&m_ChildrenLock);
        m_Children.push_back(child);
      }
      GUIInvoke::call(this, [this]() { childUpdate(); });
    }
    bool canRetry = launcherMonitor.CanRetryAfterApplication();
    if(!launcherMonitor.StatusText().isEmpty() &&
       (launcherMonitor.StatusText() != launcherStatus || canRetry != launcherCanRetry))
    {
      launcherStatus = launcherMonitor.StatusText();
      launcherCanRetry = canRetry;
      GUIInvoke::call(this, [this, launcherStatus, canRetry]() {
        ui->connectionStatus->setText(launcherStatus);
        m_LauncherRetryButton->setVisible(canRetry);
        m_LauncherRetryButton->setEnabled(canRetry);
      });
    }
#endif

    TargetControlMessage msg = conn->ReceiveMessage([this](float progress) {
      GUIInvoke::call(this, [this, progress]() {
        if(progress >= 0.0f && progress < 1.0f)
        {
          ui->progressLabel->setText(tr("Copy in Progress:"));
          ui->progressLabel->setVisible(true);
          ui->progressBar->setVisible(true);
          ui->progressBar->setMaximum(1000);
          ui->progressBar->setValue(1000 * progress);
        }
        else
        {
          ui->progressLabel->setVisible(false);
          ui->progressBar->setVisible(false);
        }
      });
    });

    if(msg.type == TargetControlMessageType::RegisterAPI)
    {
      GUIInvoke::call(this, [this, msg]() {
        m_APIs[msg.apiUse.name] =
            APIStatus(msg.apiUse.presenting, msg.apiUse.supported, msg.apiUse.supportMessage);

        if(msg.apiUse.presenting && msg.apiUse.supported)
        {
          ui->triggerImmediateCapture->setEnabled(true);
          ui->triggerDelayedCapture->setEnabled(true);
          ui->queueCap->setEnabled(true);
        }

        updateAPIStatus();
      });
    }

    if(msg.type == TargetControlMessageType::CaptureProgress)
    {
      float progress = msg.capProgress;
      GUIInvoke::call(this, [this, progress]() {
        if(progress >= 0.0f && progress < 1.0f)
        {
          ui->progressLabel->setText(tr("Capture in Progress:"));
          ui->progressLabel->setVisible(true);
          ui->progressBar->setVisible(true);
          ui->progressBar->setMaximum(1000);
          ui->progressBar->setValue(1000 * progress);
        }
        else
        {
          ui->progressLabel->setVisible(false);
          ui->progressBar->setVisible(false);
        }
      });
    }

    if(msg.type == TargetControlMessageType::NewCapture)
    {
      NewCaptureData cap = msg.newCapture;
      if(cap.api.isEmpty())
        cap.api = conn->GetAPI();
      QString name = conn->GetTarget();
      GUIInvoke::call(this, [this, name, cap]() { captureAdded(name, cap); });
    }

    if(msg.type == TargetControlMessageType::CaptureCopied)
    {
      uint32_t capID = msg.newCapture.captureId;
      QString path = msg.newCapture.path;

      GUIInvoke::call(this, [this, capID, path]() { captureCopied(capID, path); });
    }

    if(msg.type == TargetControlMessageType::NewChild)
    {
      if(msg.newChild.processId != 0)
      {
        ChildProcess c;
        c.PID = (int)msg.newChild.processId;
        c.ident = msg.newChild.ident;

        {
          QMutexLocker l(&m_ChildrenLock);
          m_Children.push_back(c);
        }

        // force a child update immediately, don't wait for the tick which is intended for decaying
        // processes that exit
        GUIInvoke::call(this, [this]() { childUpdate(); });
      }
    }

    if(msg.type == TargetControlMessageType::CapturableWindowCount)
    {
      uint32_t windows = msg.capturableWindowCount;
      GUIInvoke::call(this, [this, windows]() { ui->cycleActiveWindow->setEnabled(windows > 1); });
    }

    if(msg.type == TargetControlMessageType::RequestShow)
    {
      GUIInvoke::call(this, [this]() { m_Main->BringToFront(); });
    }
  }

  if(conn)
  {
    conn->Shutdown();
    conn = NULL;
    m_Connected.acquire();
  }

  GUIInvoke::call(this, [this]() {
    ui->connectionStatus->setText(tr("Closed"));
    m_LauncherRetryButton->setVisible(false);
    ui->connectionIcon->setPixmap(Pixmaps::disconnect(ui->connectionIcon));

    ui->numFrames->setEnabled(false);
    ui->captureDelay->setEnabled(false);
    ui->captureFrame->setEnabled(false);
    ui->triggerDelayedCapture->setEnabled(false);
    ui->triggerImmediateCapture->setEnabled(false);
    ui->queueCap->setEnabled(false);
    ui->cycleActiveWindow->setEnabled(false);

    ui->apiStatus->setText(tr("None"));
    ui->apiIcon->setVisible(false);

    connectionClosed();
  });
}

bool LiveCapture::isLocal() const
{
  return m_Hostname.isEmpty() || QHostInfo::localHostName() == m_Hostname ||
         QLatin1String("0.0.0.0") == m_Hostname || QHostAddress(m_Hostname).isLoopback();
}
