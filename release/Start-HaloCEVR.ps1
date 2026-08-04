[CmdletBinding()]
param(
    [string]$SteamAppId = '2806050',
    [string]$UevrRoot = (Join-Path $PSScriptRoot 'uevr'),
    [int]$GameStartupTimeoutSeconds = 180,
    [int]$SteamExitRemovalGraceSeconds = 15,
    [int]$SteamShutdownTimeoutSeconds = 45,
    [int]$SteamStartupTimeoutSeconds = 180,
    [switch]$Install,
    [switch]$RefreshOfficialUEVR,
    [switch]$DisableSteamExitWatchdog,
    [switch]$ForceSteamExitRecovery
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($Install) {
    & (Join-Path $PSScriptRoot 'Install-HaloCEVR.ps1')
}

$defaultUevrRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot 'uevr'))
$uevrRoot = [System.IO.Path]::GetFullPath($UevrRoot)
$usingExternalUevr = -not $uevrRoot.Equals(
    $defaultUevrRoot,
    [System.StringComparison]::OrdinalIgnoreCase)
$injector = Join-Path $uevrRoot 'UEVRInjector.exe'
if ($RefreshOfficialUEVR -and $usingExternalUevr) {
    throw (
        '-RefreshOfficialUEVR cannot overwrite an explicit external ' +
        "UEVR installation: $uevrRoot")
}
if (
    $RefreshOfficialUEVR -or
    ((-not $usingExternalUevr) -and
        -not (Test-Path -LiteralPath $injector -PathType Leaf))
) {
    & (Join-Path $PSScriptRoot 'Install-OfficialUEVR.ps1') `
        -Destination $uevrRoot -ForceRefresh:$RefreshOfficialUEVR
}

foreach ($requiredUevrFile in @(
        'UEVRInjector.exe',
        'UEVRBackend.dll',
        'LuaVR.dll',
        'openxr_loader.dll'
    )) {
    $requiredUevrPath = Join-Path $uevrRoot $requiredUevrFile
    if (-not (Test-Path -LiteralPath $requiredUevrPath -PathType Leaf)) {
        throw "Selected UEVR installation is missing ${requiredUevrFile}: $uevrRoot"
    }
}

$halo = @(Get-Process -Name HaloCampaignEvolved -ErrorAction SilentlyContinue)
if ($halo.Count -gt 1) {
    throw 'More than one HaloCampaignEvolved process is running; close stale sessions first.'
}

if ($halo.Count -eq 0) {
    Start-Process "steam://rungameid/$SteamAppId"
    $deadline = [DateTime]::UtcNow.AddSeconds($GameStartupTimeoutSeconds)
    do {
        Start-Sleep -Milliseconds 500
        $halo = @(Get-Process -Name HaloCampaignEvolved -ErrorAction SilentlyContinue)
    } while ($halo.Count -eq 0 -and [DateTime]::UtcNow -lt $deadline)
}

if ($halo.Count -ne 1) {
    throw "Halo did not reach one running process within $GameStartupTimeoutSeconds seconds."
}

$gameIdentity = Get-CimInstance -ClassName Win32_Process `
    -Filter "ProcessId = $($halo[0].Id)" -ErrorAction SilentlyContinue
if (
    $null -eq $gameIdentity -or
    -not $gameIdentity.ExecutablePath
) {
    throw "Could not establish the exact identity of Halo PID $($halo[0].Id)."
}
$gamePath = [System.IO.Path]::GetFullPath(
    [string]$gameIdentity.ExecutablePath)
$gameCreationUtc = ([DateTime]$gameIdentity.CreationDate).ToUniversalTime()

if (-not (Test-Path -LiteralPath $injector -PathType Leaf)) {
    throw "Official UEVR injector is missing: $injector"
}

if (-not $DisableSteamExitWatchdog) {
    $watchdog = Join-Path $PSScriptRoot 'Watch-SteamAppExit.ps1'
    if (-not (Test-Path -LiteralPath $watchdog -PathType Leaf)) {
        throw "Steam exit watchdog is missing: $watchdog"
    }

    $steam = Get-Process -Name steam -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $steam -or -not $steam.Path) {
        throw 'Steam is not running or its executable path is unavailable.'
    }
    $steamRoot = Split-Path -Parent $steam.Path
    $consoleLog = Join-Path $steamRoot 'logs\console_log.txt'
    $logDirectory = Join-Path $PSScriptRoot 'logs'
    New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
    $watchdogSuffix = '{0}-{1}' -f $SteamAppId, $halo[0].Id
    $watchdogLog = Join-Path `
        $logDirectory "steam-exit-watchdog-$watchdogSuffix.log"
    $watchdogResult = Join-Path `
        $logDirectory "steam-exit-watchdog-$watchdogSuffix.json"

    $quoteLiteral = {
        param([string]$Value)
        return "'" + $Value.Replace("'", "''") + "'"
    }
    $watchdogCommandParts = @(
        '& ' + (& $quoteLiteral $watchdog),
        '-AppId ' + (& $quoteLiteral $SteamAppId),
        '-GameProcessId ' + $halo[0].Id,
        '-GameCreationDateUtc ' +
            (& $quoteLiteral $gameCreationUtc.ToString('o')),
        '-GameExe ' + (& $quoteLiteral $gamePath),
        '-SteamExe ' + (& $quoteLiteral $steam.Path),
        '-ConsoleLogPath ' + (& $quoteLiteral $consoleLog),
        '-RemovalGraceSeconds ' + $SteamExitRemovalGraceSeconds,
        '-SteamShutdownTimeoutSeconds ' + $SteamShutdownTimeoutSeconds,
        '-SteamStartupTimeoutSeconds ' + $SteamStartupTimeoutSeconds,
        '-LogPath ' + (& $quoteLiteral $watchdogLog),
        '-ResultPath ' + (& $quoteLiteral $watchdogResult)
    )
    if ($ForceSteamExitRecovery) {
        $watchdogCommandParts += '-ForceSteamRecovery'
    }
    $watchdogCommand = $watchdogCommandParts -join ' '
    $encodedCommand = [Convert]::ToBase64String(
        [Text.Encoding]::Unicode.GetBytes($watchdogCommand))
    $powerShellHost = (Get-Process -Id $PID).Path
    $watchdogProcess = Start-Process `
        -FilePath $powerShellHost `
        -ArgumentList @(
            '-NoLogo',
            '-NoProfile',
            '-NonInteractive',
            '-ExecutionPolicy', 'Bypass',
            '-EncodedCommand', $encodedCommand
        ) `
        -WindowStyle Hidden `
        -PassThru
    Write-Output (
        "Steam exit watchdog PID $($watchdogProcess.Id) is monitoring " +
        "Halo PID $($halo[0].Id).")
}

Write-Output "Halo PID $($halo[0].Id) is running; starting official UEVR auto-attach."
Write-Output (
    "Using UEVR backend: " + (Join-Path $uevrRoot 'UEVRBackend.dll'))
Start-Process -FilePath $injector `
    -ArgumentList '--attach=HaloCampaignEvolved.exe' `
    -WorkingDirectory $uevrRoot `
    -WindowStyle Hidden
