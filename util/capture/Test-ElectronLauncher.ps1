#requires -Version 5.1
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Load functions only. Do not execute the production script's attachment path.
$sourcePath = Join-Path $PSScriptRoot 'Attach-ElectronLauncher.ps1'
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($sourcePath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -gt 0) { throw ($parseErrors | Out-String) }
$definitions = $ast.FindAll({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst]
}, $false)
foreach ($definition in $definitions) {
    . ([scriptblock]::Create($definition.Extent.Text))
}

$script:checks = 0
function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "FAIL: $Message" }
    $script:checks++
}

function Assert-Throws {
    param([scriptblock]$Action, [string]$Pattern)
    $message = $null
    try { & $Action | Out-Null }
    catch { $message = $_.Exception.Message }
    Assert-True ($null -ne $message -and $message -match $Pattern) "Expected error matching '$Pattern'; got '$message'"
}

function New-ProcessRecord {
    param([int]$Id, [int]$ParentId, [string]$Role = 'Launcher', [int]$Seconds = 0,
        [string]$Path = 'C:\launcher path\launcher.exe')
    $command = '"C:\launcher path\launcher.exe" --login-token=TEST_SECRET'
    if ($Role -eq 'NodeService') {
        $command += ' --type=utility --utility-sub-type=node.mojom.NodeService'
    }
    elseif ($Role -eq 'Renderer') { $command += ' --type=renderer' }
    elseif ($Role -eq 'Unknown') { $command = $null }
    [pscustomobject]@{
        ProcessId = $Id
        ParentProcessId = $ParentId
        ExecutablePath = $Path
        CommandLine = $command
        CreationDate = ([datetime]'2026-01-01T00:00:00').AddSeconds($Seconds)
    }
}

Assert-True ((Get-ElectronRole 'app --type=utility --utility-sub-type=node.mojom.NodeService') -eq 'NodeService') 'Normal NodeService flags'
Assert-True ((Get-ElectronRole 'app --type "utility" "--utility-sub-type=node.mojom.NodeService"') -eq 'NodeService') 'Quoted and separate flag values'
Assert-True ((Get-ElectronRole 'app --type=renderer --utility-sub-type=node.mojom.NodeService') -eq 'ElectronChild') 'Both NodeService role flags are required'
Assert-True ((Get-ElectronRole 'app --type=utility --utility-sub-type=node.mojom.NodeServiceExtra') -eq 'ElectronChild') 'Role values must match whole tokens'
Assert-True ((Get-ElectronRole $null) -eq 'Unknown') 'Unreadable command line is unknown'

$launcher = New-ProcessRecord 100 1
$helper = New-ProcessRecord 101 100 'NodeService' 1
$renderer = New-ProcessRecord 102 100 'Renderer' 1
$otherRoot = New-ProcessRecord 200 1
$otherHelper = New-ProcessRecord 201 200 'NodeService' 1
$path = $launcher.ExecutablePath
$records = @($launcher, $helper, $renderer)
$found = @(Find-LauncherHelpers $records $path 0 0)
$selected = Select-LauncherHelper $found
Assert-True ($found.Count -eq 1 -and $selected.Helper.ProcessId -eq 101) 'Select only the launcher NodeService'
$nested = New-ProcessRecord 103 102 'NodeService' 2
$found = @(Find-LauncherHelpers @($launcher, $renderer, $nested) $path 0 0)
Assert-True ($found.Count -eq 1 -and $found[0].Helper.ProcessId -eq 103) 'Follow launcher ancestry beyond direct children'
$found = @(Find-LauncherHelpers @($launcher, $helper, $otherRoot, $otherHelper) $path 0 0)
Assert-Throws { Select-LauncherHelper $found } 'Multiple launchers'
$found = @(Find-LauncherHelpers @($launcher, $helper, $otherRoot, $otherHelper) $path 200 0)
Assert-True ((Select-LauncherHelper $found).Helper.ProcessId -eq 201) 'Explicit launcher selection'
$found = @(Find-LauncherHelpers @($launcher, $helper, $renderer, $nested) $path 100 0)
Assert-Throws { Select-LauncherHelper $found } 'Multiple NodeService'
$found = @(Find-LauncherHelpers @($launcher, $helper, $renderer, $nested) $path 100 103)
Assert-True ((Select-LauncherHelper $found).Helper.ProcessId -eq 103) 'Explicit helper selection'
Assert-Throws { Select-LauncherHelper @() } 'No NodeService'
Assert-Throws { Find-LauncherHelpers $records $path 999 0 } 'No matching launcher'
$inaccessible = New-ProcessRecord 100 1 'Unknown'
Assert-Throws { Find-LauncherHelpers @($inaccessible, $helper) $path 0 0 } 'No matching launcher'
$wrongPath = New-ProcessRecord 104 100 'NodeService' 1 'C:\other\launcher.exe'
Assert-True (@(Find-LauncherHelpers @($launcher, $wrongPath) $path 0 0).Count -eq 0) 'Exclude another executable'
$reusedRoot = New-ProcessRecord 100 1 'Launcher' 2
Assert-True (@(Find-LauncherHelpers @($reusedRoot, $helper) $path 0 0).Count -eq 0) 'Reject a parent PID reused after helper creation'
$cycleA = New-ProcessRecord 301 302 'NodeService' 1
$cycleB = New-ProcessRecord 302 301 'Renderer' 1
Assert-True (@(Find-LauncherHelpers @($launcher, $cycleA, $cycleB) $path 0 0).Count -eq 0) 'Ancestry cycles terminate without matching'
Assert-True (@(Find-LauncherHelpers @($launcher, $otherHelper) $path 0 0).Count -eq 0) 'Missing ancestry does not match'

Assert-ProcessIdentity $helper (New-ProcessRecord 101 100 'NodeService' 1)
$script:checks++
Assert-Throws { Assert-ProcessIdentity $helper $null } 'changed identity'
Assert-Throws { Assert-ProcessIdentity $helper (New-ProcessRecord 101 100 'NodeService' 2) } 'changed identity'
Assert-Throws { Assert-ProcessIdentity $helper (New-ProcessRecord 101 200 'NodeService' 1) } 'changed identity'
Assert-Throws { Assert-ProcessIdentity $helper (New-ProcessRecord 101 100 'Renderer' 1) } 'changed identity'
Assert-Throws { Assert-LauncherChain $selected @($reusedRoot, $helper) } 'changed identity'

$corePath = 'C:\build path\gfxcap.dll'
$baseModule = [pscustomobject]@{ ModuleName = 'kernel32.dll'; FileName = 'C:\Windows\System32\kernel32.dll' }
$captureModule = [pscustomobject]@{ ModuleName = 'gfxcap.dll'; FileName = $corePath }
Assert-True ($null -eq (Get-CaptureModule @($baseModule) $corePath)) 'Readable module list without capture library'
Assert-True ((Get-CaptureModule @($null, $baseModule, $captureModule) $corePath).FileName -eq $corePath) 'Verify expected library path while filtering nulls'
Assert-Throws { Get-CaptureModule @() $corePath } 'loading state is unknown'
Assert-Throws { Get-CaptureModule @($null) $corePath } 'loading state is unknown'
$differentBuild = [pscustomobject]@{ ModuleName = 'gfxcap.dll'; FileName = 'C:\old\gfxcap.dll' }
Assert-Throws { Get-CaptureModule @($baseModule, $differentBuild) $corePath } 'different capture library'
$oldLibrary = [pscustomobject]@{ ModuleName = 'renderdoc.dll'; FileName = 'C:\old\renderdoc.dll' }
Assert-Throws { Get-CaptureModule @($baseModule, $oldLibrary) $corePath } 'different capture library'

Assert-True ((Get-InjectConnectionId @('Injecting into PID 101', 'Launched as ID 38921') 38921) -eq 38921) 'Positive connection ID means success'
Assert-Throws { Get-InjectConnectionId @('Failed to inject: test failure') 3 } 'did not report successful'
Assert-Throws { Get-InjectConnectionId @('Launched as ID 38921') 3 } 'inconsistent connection ID'
Assert-Throws { Get-InjectConnectionId @('Launched as ID 0') 0 } 'inconsistent connection ID'
Assert-Throws { Get-InjectConnectionId @('No success message') 0 } 'did not report successful'
Assert-Throws { Get-InjectConnectionId @('Launched as ID 38921', 'Failed to inject: test failure') 38921 } 'did not report successful'

if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
    # A disposable native command exercises stderr, spaces in arguments, and the
    # real nonzero exit-code behavior in both supported PowerShell versions.
    $fixtureDirectory = Join-Path ([IO.Path]::GetTempPath()) ('gfxcap cli test ' + [guid]::NewGuid())
    $fixturePath = Join-Path $fixtureDirectory 'capture fixture.cmd'
    $PSNativeCommandUseErrorActionPreference = $true
    New-Item -ItemType Directory -Path $fixtureDirectory | Out-Null
    try {
        $fixture = @'
@echo off
if not "%~1"=="inject" exit /b 2
if not "%~2"=="--PID" exit /b 2
if not "%~3"=="42" exit /b 2
if not "%~4"=="--opt-hook-children" exit /b 2
if not "%~5"=="--capture-file" exit /b 2
if not "%~6"=="C:\capture path\frame" exit /b 2
echo Injecting into PID 42
echo Launched as ID 38921 1>&2
exit /b 38921
'@
        [IO.File]::WriteAllText($fixturePath, $fixture, [Text.Encoding]::ASCII)
        $result = Invoke-HelperInjection $fixturePath 42 'C:\capture path\frame'
        Assert-True ($result.ConnectionId -eq 38921 -and $result.ExitCode -eq 38921) 'Native stderr and connection exit code, with spaces in paths'
        Assert-Throws { Invoke-HelperInjection $fixturePath 43 'C:\capture path\frame' } 'did not report successful'
    }
    finally {
        if (Test-Path -LiteralPath $fixturePath) { Remove-Item -LiteralPath $fixturePath }
        Remove-Item -LiteralPath $fixtureDirectory
    }
}

Write-Output "Passed $script:checks checks. No live process was attached."
