# Capture through an Electron launcher

Some Electron launchers start the rendering application from their utility process
(`--type=utility --utility-sub-type=node.mojom.NodeService`). Loading the capture
library in the main launcher alone can leave this helper uncaptured. Attach to the
helper with child capture enabled **before** starting the application instance
you want to capture. Some launchers require an initial normal application launch
before their helper accepts attachment, followed by restarting that application.

The GUI makes an initial helper attachment attempt for a local Windows Electron
launcher when **Capture Child Processes** is enabled. The monitor starts with the
launcher's live connection, waits until its NodeService is at least ten seconds
old, and calls the same directory's `gfxcapcmd.exe inject` if this build's library
is absent. It preserves the capture prefix and all selected capture options.
Failed attachments are retried at most twice, with five seconds between attempts.
If all three attempts fail with a readable, absent library, the GUI shows
**Close application, then retry attachment** and **Retry launcher attachment**.
Only clicking this button permits one additional round of up to three attempts.
Application startup and elapsed time do not trigger the extra round.

The button targets only the verified launcher helper. It does not inspect the
application's windows or graphics modules, or attach to an already running
application. Start the application once normally if the launcher requires it,
close the application, then click the button. Start it again after attachment
succeeds. The GUI does not close or restart the application for you.
The button cannot enable another round after success, timeout, an unverified
loaded capture library, or exhaustion of the extra round.
Discovery requires `LICENSE.electron.txt` beside the launcher and a single direct
child with the same actual executable path and the NodeService role. It skips
unverifiable executable identities, unreadable helper module lists, another
capture build, and ambiguous matches. Command lines are used for role detection
and are not logged. Attachment failures are logged and shown in Connection Status.

`Attach-ElectronLauncher.ps1` remains available for manual discovery and attachment
using this branch's existing `gfxcapcmd.exe inject --opt-hook-children` command.
It selects the helper by executable path, Electron role, and launcher ancestry,
then verifies that this build's `gfxcap.dll` loaded. It requires PowerShell 7
(recommended) or Windows PowerShell 5.1;
attachment requires a 64-bit shell. For an elevated launcher, use an administrator
shell and administrator GfxCore so process inspection and attachment can succeed.

## Usage

1. Build the x64 targets. Use `GfxCore.exe`, `gfxcapcmd.exe`, and `gfxcap.dll` from
   the same build directory (default: `x64\Release`).
2. Start the launcher through GfxCore's **Launch Application** window. Set its
   working directory to the launcher directory and enable **Capture Child
   Processes**. Enter these settings manually or load a saved `.cap` settings
   file; both configure the same launch operation.
3. If initial attachment fails, start the application once normally from the
   launcher, then exit only the application. Keep GfxCore and the launcher open.
   **Established** alone confirms only the main launcher connection. If the
   application already shows frame information, it is ready to capture.
4. Click **Retry launcher attachment** in the live capture window's Status group.
   Wait for **Launcher helper attached; start application**, then start the
   application again from the retained launcher. If initial attachment already
   succeeded, start the application directly. Helper attachment does not
   retroactively capture a child that was already running.
5. Press F12 in the rendering application. Opening its child connection is
   optional: the `.rdc` file can also be opened directly from the capture directory.
   For the default temporary capture prefix, this is `%TEMP%\GfxCore`. Confirm
   the capture opens and replays.

Rebuild and restart GfxCore to use the GUI integration. Earlier builds and a GUI
already running before the rebuild still require the manual helper step.

## Manual attachment

Run the following from a 64-bit PowerShell shell, elevated if the launcher is
elevated. For the installation used in the original verification:

```powershell
$launcher = 'D:\TapTap\PC Games\243110\王者万象棋.exe'
& .\util\capture\Attach-ElectronLauncher.ps1 -LauncherPath $launcher -ListOnly
& .\util\capture\Attach-ElectronLauncher.ps1 -LauncherPath $launcher -Verbose
```

The path above is an example, not a script default. Use the launcher's path rather
than the rendering child's path (`WXQ.exe` in this example). No session PID is
stored. If several helpers match, the script stops and prints the candidate IDs.
Use `-LauncherProcessId` and/or `-HelperProcessId` from discovery to select one:

```powershell
& .\util\capture\Attach-ElectronLauncher.ps1 -LauncherPath $launcher `
    -LauncherProcessId $selectedLauncherId -HelperProcessId $selectedHelperId `
    -BuildDirectory .\x64\Release `
    -CaptureFile .\x64\Release\captures\wxq -Verbose
```

`CaptureFile` is a prefix; captures receive a frame suffix and `.rdc` extension.
The default is `x64\Release\captures\electron-launcher`. Build output directories
are already ignored by Git. `-ListOnly` does not attach or create directories.
The script does not start, exit, or restart the launcher/application itself.

## Results and troubleshooting

- `Status = Attached` means both CLI success and the expected helper module were
  verified. It does not establish that a rendering child has loaded the library
  or that a frame has been captured; verify those in GfxCore after launching it.
- The CLI prints `Launched as ID <id>` and returns that connection ID as its exit
  code. A nonzero exit code alone does not mean failure. The script validates both
  the message and the matching positive exit code.
- `Status = AlreadyLoaded` skips repeated attachment. Existing capture options
  and the capture prefix are unknown; restart the launcher to apply new settings.
  A capture library from another build causes an error.
- The GUI shows waiting, attaching, retrying, or failed helper status in
  **Connection Status**. **Launcher helper attached** verifies the CLI result,
  matching positive connection ID, and this build's loaded module. It does not
  verify a rendering child's capture. **Launcher helper library already loaded**
  means attachment was skipped and its existing options have not been verified.
- In the verified game session, all helper attachments before starting the game
  failed, including an independent CLI attempt after eleven minutes of uptime.
  After a normal game launch, the same helper accepted the same standard CLI and
  unchanged library. Restarting only the game produced a capture that replayed
  successfully. Consequently, elapsed time alone is not a readiness test for this
  launcher, and a failure before the first game launch is not a final result.
- In a historical automatic GUI test, the running game's WMI executable path was unavailable
  and module enumeration returned Windows access-denied error 5 even from an
  administrator process. The earlier monitor therefore could not trigger the
  automatic application-start round. A later automatic version checked the main
  window and verified the actual executable. The current button workflow removes
  application-start detection entirely; helper identity and module verification
  still apply.
- Missing or unreadable metadata, ambiguous helpers, a changed process identity,
  and unavailable module enumeration cause errors. Empty module enumeration is
  treated as unknown, rather than evidence that the library is absent. If the
  launcher requires administrator privileges, run the shell at the same level;
  Windows launch error 740 means elevation is required.
- Process selection is checked again before and after attachment. The CLI takes
  a PID, so a process can still exit while the command runs. Rerun discovery after
  any launcher/helper restart. If its role or process tree changes in a future
  launcher version, inspect the new tree before adapting this workflow.
- Only PIDs, timestamps, capture paths, and connection results are returned. Raw
  launcher command lines are not logged because they can contain login tokens.

The manual CLI workflow was verified on 2026-10-28 with the modified 1.47 build and 王者万象棋
v1.7.2: the new `WXQ.exe` loaded `gfxcap.dll`, captured a D3D11 title-screen frame
at 1920×1080, and replayed it successfully in this build. Battle scenes and
continuous capture stability have not been verified. The reusable script still
needs validation against future launcher versions.

## Validation

The runtime results below include historical versions. The current GUI retains
initial attachment and the explicit button, and removes the automatic
application-start round. A final automatic-build session reported an ACE runtime
exception (`1-1812-4331003`) on the first game restart; another start then showed
frame information. The exact cause remains unconfirmed. Changing to the button
workflow does not establish that this startup exception is resolved.

The current button workflow was tested with the production monitor and an owned
launcher/helper tree. After three synthetic initial failures, an uncaptured D3D11
application ran for over ten seconds without triggering another attachment.
After that application exited, the explicit request attached the helper. A new
child verified library loading before its entry point, preserved nondefault
capture options, and produced a capture. Repeated requests after success were
ignored. A separate exhaustion run verified that all six failed attempts prevent
further requests. The latest exit-before-button game workflow was not retested.

Run the regression checks without attaching to any live process:

```powershell
& .\util\capture\Test-ElectronLauncher.ps1
```

The checks cover launcher/helper selection, stale PIDs and ancestry, ambiguous
matches, module enumeration, and the CLI connection-ID return convention. They
load only the script's function definitions and use synthetic process/module
records; no game installation or capture build is needed.

Validation also exercised the full script against an owned launcher/helper tree:
helper attachment succeeded, repeated attachment was skipped, and a newly started
D3D11 test child had the library loaded before its entry point, captured a frame,
and replayed successfully. Runtime checks were run with PowerShell 7. Windows
PowerShell 5.1 passed syntax checks; full runtime compatibility remains unverified.

The native GUI monitor was also exercised against an owned Windows launcher and
NodeService tree, using its production class without a Qt event loop in its
connection worker. Discovery matched Windows process paths to Qt paths, the
asynchronous CLI attachment succeeded, and repeated attachment was skipped. A
second run simulated a first-attempt CLI failure, then verified a successful
retry. A new D3D11 child required the library to be preloaded before its entry
point and verified nondefault vsync, resource-reference, and memory-limit options.
The child captured a frame. Diagnostic fixtures are ignored build outputs.

The application-start fallback fixture separates the launcher executable from
its rendering child's executable. It forces three initial CLI failures, starts
an uncaptured D3D11 child, and checks that this event enables one additional
attachment round. A subsequently started child must inherit the library and
selected options and produce a frame; the already running child is not treated
as captured. Diagnostic fixtures currently remain in the ignored build directory
because automated deletion was blocked.

The explicit application-start request was tested with the production monitor
after three synthetic failures, without relying on rendering-child discovery.
Attachment succeeded, the subsequent D3D11 child inherited the library and
selected options, and its capture replayed successfully. Requests before the
initial failure round, after successful attachment, and after all six failed
attempts did not enable further attempts. The automatic rendering-child flow was
also rerun after adding the explicit request.

The visible-window fallback was tested with the production monitor while the
owned application's WMI path was omitted and its module snapshots returned
access denied. No manual retry was requested: attachment succeeded and the next
child inherited the capture options and captured a frame. Separate checks rejected
hidden, tool, owned, and ambiguous application windows, mismatched or missing
process names, and a visible child using the launcher's own executable.

The automatic GUI workflow was verified in a fresh 王者万象棋 session on
2026-10-28. Without clicking the retry button, the GUI detected the first game
process after approximately ten seconds and verified helper attachment. Restarting
only the game then displayed frame information, and F12 produced a 1920×1080
D3D11 capture that replayed successfully with no HIGH validation messages. The
diagnostic build used for this run added branch logging; the capture strategy
was unchanged. Earlier failed sessions do not establish a precise failure cause.
