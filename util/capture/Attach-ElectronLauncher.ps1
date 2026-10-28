#requires -Version 5.1
<#
.SYNOPSIS
Enables child capture in the Electron NodeService that starts an application.
.DESCRIPTION
Selects a helper belonging to the specified launcher and uses this build's
gfxcapcmd inject command. Run before starting the application from its launcher.
Use -ListOnly to inspect candidates without attaching or creating files.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$LauncherPath,
    [ValidateRange(0, 2147483647)]
    [int]$LauncherProcessId = 0,
    [ValidateRange(0, 2147483647)]
    [int]$HelperProcessId = 0,
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\..\x64\Release'),
    [string]$CaptureFile,
    [switch]$ListOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-ElectronRole {
    param([string]$CommandLine)

    if ([string]::IsNullOrWhiteSpace($CommandLine)) { return 'Unknown' }
    $typeFlag = '(?:^|\s)"?--type(?:=|\s|"|$)'
    $utilityFlag = '(?:^|\s)"?--type(?:=|\s+)"?utility"?(?=\s|$)'
    $nodeFlag = '(?:^|\s)"?--utility-sub-type(?:=|\s+)"?node\.mojom\.NodeService"?(?=\s|$)'
    if ($CommandLine -match $utilityFlag -and $CommandLine -match $nodeFlag) {
        return 'NodeService'
    }
    if ($CommandLine -match $typeFlag) { return 'ElectronChild' }
    return 'Launcher'
}

function Test-LauncherDescendant {
    param($Process, $Launcher, [hashtable]$ProcessesById)

    $seen = @{}
    $current = $Process
    while ($current.ProcessId -ne $Launcher.ProcessId) {
        if ($seen.ContainsKey([int]$current.ProcessId)) { return $false }
        $seen[[int]$current.ProcessId] = $true
        $parent = $ProcessesById[[int]$current.ParentProcessId]
        if ($null -eq $parent -or $null -eq $parent.CreationDate -or
            $null -eq $current.CreationDate -or $parent.CreationDate -gt $current.CreationDate) {
            return $false
        }
        $current = $parent
    }
    return $Process.ProcessId -ne $Launcher.ProcessId
}

function Find-LauncherHelpers {
    param([object[]]$Processes, [string]$Path, [int]$RootId, [int]$HelperId)

    $byId = @{}
    foreach ($process in $Processes) { $byId[[int]$process.ProcessId] = $process }
    $launchers = @($Processes | Where-Object {
        $_.ExecutablePath -ieq $Path -and $null -ne $_.CreationDate -and
        (Get-ElectronRole $_.CommandLine) -eq 'Launcher' -and
        ($RootId -eq 0 -or $_.ProcessId -eq $RootId)
    })
    if ($launchers.Count -eq 0) {
        throw 'No matching launcher found. Start it first, check -LauncherPath/-LauncherProcessId, and use an elevated shell if its metadata is inaccessible.'
    }

    $candidates = @($Processes | Where-Object {
        $_.ExecutablePath -ieq $Path -and (Get-ElectronRole $_.CommandLine) -eq 'NodeService' -and
        ($HelperId -eq 0 -or $_.ProcessId -eq $HelperId)
    })
    foreach ($launcher in $launchers) {
        foreach ($helper in $candidates) {
            if (Test-LauncherDescendant $helper $launcher $byId) {
                [pscustomobject]@{ Launcher = $launcher; Helper = $helper }
            }
        }
    }
}

function Select-LauncherHelper {
    param([object[]]$HelperMatches)

    if ($HelperMatches.Count -eq 0) {
        throw 'No NodeService helper found under the selected launcher. Wait for it to finish starting, or check the selected process IDs with -ListOnly.'
    }
    $rootIds = @($HelperMatches | ForEach-Object { $_.Launcher.ProcessId } | Sort-Object -Unique)
    if ($rootIds.Count -ne 1) {
        throw "Multiple launchers have matching helpers (PIDs: $($rootIds -join ', ')). Select one with -LauncherProcessId."
    }
    if ($HelperMatches.Count -ne 1) {
        $helperIds = @($HelperMatches | ForEach-Object { $_.Helper.ProcessId })
        throw "Multiple NodeService helpers found (PIDs: $($helperIds -join ', ')). Select one with -HelperProcessId."
    }
    return $HelperMatches[0]
}

function Assert-ProcessIdentity {
    param($Expected, $Current)

    if ($null -eq $Current -or $null -eq $Current.CreationDate -or
        $Current.ProcessId -ne $Expected.ProcessId -or
        $Current.ParentProcessId -ne $Expected.ParentProcessId -or
        $Current.CreationDate -ne $Expected.CreationDate -or
        $Current.ExecutablePath -ine $Expected.ExecutablePath -or
        (Get-ElectronRole $Current.CommandLine) -ne (Get-ElectronRole $Expected.CommandLine)) {
        throw "Process $($Expected.ProcessId) exited, changed identity, or became inaccessible. Run discovery again."
    }
}

function Assert-LauncherChain {
    param($Match, [object[]]$Processes)

    $byId = @{}
    foreach ($process in $Processes) { $byId[[int]$process.ProcessId] = $process }
    Assert-ProcessIdentity $Match.Launcher $byId[[int]$Match.Launcher.ProcessId]
    Assert-ProcessIdentity $Match.Helper $byId[[int]$Match.Helper.ProcessId]
    if (-not (Test-LauncherDescendant $byId[[int]$Match.Helper.ProcessId] $Match.Launcher $byId)) {
        throw 'The NodeService no longer belongs to the selected launcher. Run discovery again.'
    }
}

function Get-CaptureModule {
    param([object[]]$Modules, [string]$ExpectedPath)

    $available = @($Modules | Where-Object { $null -ne $_ })
    if ($available.Count -eq 0) {
        throw 'Module enumeration returned no modules; loading state is unknown. Use a 64-bit shell with permission to inspect the launcher.'
    }
    $captureModules = @($available | Where-Object {
        $_.ModuleName -ieq 'gfxcap.dll' -or $_.ModuleName -ieq 'renderdoc.dll'
    })
    foreach ($module in $captureModules) {
        if ($module.FileName -ine $ExpectedPath) {
            throw "A different capture library is already loaded ($($module.FileName)). Restart the launcher and select the intended build."
        }
    }
    if ($captureModules.Count -gt 0) { return $captureModules[0] }
    return $null
}

function Read-CaptureModule {
    param([int]$ProcessId, [string]$ExpectedPath)

    $process = Get-Process -Id $ProcessId -ErrorAction Stop
    try {
        # Filter null entries: @($process.Modules) can contain a single null.
        $modules = @($process.Modules | Where-Object { $null -ne $_ })
        return Get-CaptureModule $modules $ExpectedPath
    }
    catch {
        throw "Cannot verify modules in PID ${ProcessId}: $($_.Exception.Message) If the launcher is elevated, run this script from an administrator shell."
    }
    finally { $process.Dispose() }
}

function Get-InjectConnectionId {
    param([string[]]$Output, [int]$ExitCode)

    $lines = @($Output | ForEach-Object { $_.Trim() })
    $success = @($lines | Where-Object { $_ -match '^Launched as ID ([0-9]+)$' })
    if ($success.Count -ne 1 -or @($lines | Where-Object { $_ -match '^Failed to inject:' }).Count -gt 0) {
        throw "gfxcapcmd did not report successful attachment (exit $ExitCode): $($Output -join [Environment]::NewLine)"
    }
    $connectionId = [uint32]($success[0] -replace '^Launched as ID ', '')
    if ($connectionId -eq 0 -or $connectionId -ne $ExitCode) {
        throw "gfxcapcmd returned an inconsistent connection ID (ID $connectionId, exit $ExitCode)."
    }
    return $connectionId
}

function Invoke-HelperInjection {
    param([string]$ToolPath, [int]$ProcessId, [string]$CapturePrefix)

    # CLI success returns a connection ID, and writes it to stderr. Handle both
    # Windows PowerShell's NativeCommandError records and PowerShell 7's native
    # exit-code handling locally, then validate the tool's output explicitly.
    $ErrorActionPreference = 'Continue'
    $PSNativeCommandUseErrorActionPreference = $false
    $output = @(& $ToolPath inject --PID $ProcessId --opt-hook-children `
        --capture-file $CapturePrefix 2>&1 | ForEach-Object { "$_" })
    $exitCode = $LASTEXITCODE
    $connectionId = Get-InjectConnectionId $output $exitCode
    return [pscustomobject]@{ ConnectionId = $connectionId; ExitCode = $exitCode }
}

function Get-LauncherProcesses {
    # Keep raw command lines in memory for role detection; never include them in
    # public results or write them to disk, since launchers can include tokens.
    return @(Get-CimInstance Win32_Process -Verbose:$false -Property ProcessId, ParentProcessId,
        ExecutablePath, CommandLine, CreationDate)
}

if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'This script requires Windows.'
}
$launcherFile = Get-Item -LiteralPath $LauncherPath
if ($launcherFile.PSIsContainer -or $launcherFile.Extension -ine '.exe') {
    throw '-LauncherPath must name the launcher executable.'
}
$launcherFullPath = $launcherFile.FullName
$matchesFound = @(Find-LauncherHelpers (Get-LauncherProcesses) $launcherFullPath `
    $LauncherProcessId $HelperProcessId)
if ($ListOnly) {
    if ($matchesFound.Count -eq 0) { throw 'No NodeService helper found. Wait for the launcher to finish starting.' }
    foreach ($match in $matchesFound) {
        [pscustomobject]@{
            LauncherProcessId = $match.Launcher.ProcessId
            HelperProcessId = $match.Helper.ProcessId
            HelperStartedAt = $match.Helper.CreationDate
        }
    }
    return
}
$selected = Select-LauncherHelper $matchesFound
if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell with this x64 build.' }
$buildPath = (Get-Item -LiteralPath $BuildDirectory).FullName
$toolPath = Join-Path $buildPath 'gfxcapcmd.exe'
$corePath = Join-Path $buildPath 'gfxcap.dll'
foreach ($required in @($toolPath, $corePath)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing $required. Build this branch's x64 targets or specify -BuildDirectory."
    }
}
Assert-LauncherChain $selected (Get-LauncherProcesses)
$loaded = Read-CaptureModule $selected.Helper.ProcessId $corePath
if ($null -ne $loaded) {
    Write-Warning 'This build is already loaded in the helper. Its child-capture options and capture prefix have not been changed or verified. Connect through GfxCore, or restart the launcher to apply these options.'
    [pscustomobject]@{
        Status = 'AlreadyLoaded'
        LauncherProcessId = $selected.Launcher.ProcessId
        HelperProcessId = $selected.Helper.ProcessId
        ConnectionId = $null
        CliExitCode = $null
        CaptureModulePath = $loaded.FileName
        CaptureFile = $null
    }
    return
}
if ([string]::IsNullOrWhiteSpace($CaptureFile)) {
    $CaptureFile = Join-Path $buildPath 'captures\electron-launcher'
}
$capturePrefix = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($CaptureFile)
$captureDirectory = Split-Path -Parent $capturePrefix
if (-not (Test-Path -LiteralPath $captureDirectory -PathType Container)) {
    New-Item -ItemType Directory -Path $captureDirectory -Force | Out-Null
}
# Revalidate immediately before passing the PID to the existing CLI.
Assert-LauncherChain $selected (Get-LauncherProcesses)
$result = Invoke-HelperInjection $toolPath $selected.Helper.ProcessId $capturePrefix
Assert-LauncherChain $selected (Get-LauncherProcesses)
$loaded = Read-CaptureModule $selected.Helper.ProcessId $corePath
if ($null -eq $loaded) {
    throw "CLI returned connection $($result.ConnectionId), but gfxcap.dll was not found in the helper. Attachment has not been verified."
}
Write-Verbose 'Helper attachment verified. Start or restart the application from the retained launcher, then connect to the child in GfxCore and capture a frame.'
[pscustomobject]@{
    Status = 'Attached'
    LauncherProcessId = $selected.Launcher.ProcessId
    HelperProcessId = $selected.Helper.ProcessId
    ConnectionId = $result.ConnectionId
    CliExitCode = $result.ExitCode
    CaptureModulePath = $loaded.FileName
    CaptureFile = $capturePrefix
}
