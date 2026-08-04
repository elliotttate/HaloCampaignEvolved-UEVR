[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^\d+$')]
    [string]$AppId,
    [Parameter(Mandatory)]
    [ValidateRange(1, [int]::MaxValue)]
    [int]$GameProcessId,
    [Parameter(Mandatory)]
    [string]$GameCreationDateUtc,
    [Parameter(Mandatory)]
    [string]$GameExe,
    [Parameter(Mandatory)]
    [string]$SteamExe,
    [Parameter(Mandatory)]
    [string]$ConsoleLogPath,
    [int]$RuntimeFrontendProcessId,
    [string]$RuntimeFrontendCreationDateUtc,
    [string]$RuntimeFrontendExe,
    [ValidateRange(1, 120)]
    [int]$RuntimeExitGameCloseSeconds = 15,
    [ValidateRange(1, 300)]
    [int]$RemovalGraceSeconds = 15,
    [ValidateRange(1, 300)]
    [int]$SteamShutdownTimeoutSeconds = 45,
    [ValidateRange(1, 600)]
    [int]$SteamStartupTimeoutSeconds = 180,
    [ValidateRange(1, 3600)]
    [int]$OtherGamePollSeconds = 5,
    [switch]$ForceSteamRecovery,
    [ValidateSet('Recover', 'ReportOnly')]
    [string]$RecoveryMode = 'Recover',
    [string]$LogPath,
    [string]$ResultPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Write-WatchdogLog {
    param([Parameter(Mandatory)][string]$Message)

    $line = '[{0}] {1}' -f [DateTime]::UtcNow.ToString('o'), $Message
    if ($LogPath) {
        $directory = Split-Path -Parent $LogPath
        if ($directory) {
            New-Item -ItemType Directory -Path $directory -Force | Out-Null
        }
        Add-Content -LiteralPath $LogPath -Value $line -Encoding UTF8
    }
    Write-Output $line
}

function Complete-Watchdog {
    param(
        [Parameter(Mandatory)][string]$Status,
        [Parameter(Mandatory)][string]$Details
    )

    $result = [ordered]@{
        completed_at = [DateTime]::UtcNow.ToString('o')
        status = $Status
        details = $Details
        app_id = $AppId
        game_pid = $GameProcessId
    }
    if ($ResultPath) {
        $directory = Split-Path -Parent $ResultPath
        if ($directory) {
            New-Item -ItemType Directory -Path $directory -Force | Out-Null
        }
        $result | ConvertTo-Json -Depth 3 |
            Set-Content -LiteralPath $ResultPath -Encoding UTF8
    }
    Write-WatchdogLog "$Status - $Details"
    return [pscustomobject]$result
}

function Get-ExactProcess {
    param(
        [Parameter(Mandatory)][int]$ProcessId,
        [Parameter(Mandatory)][string]$ExecutablePath,
        [Parameter(Mandatory)][DateTime]$CreationDateUtc
    )

    $record = Get-CimInstance -ClassName Win32_Process `
        -Filter "ProcessId = $ProcessId" -ErrorAction SilentlyContinue
    if ($null -eq $record -or -not $record.ExecutablePath) {
        return $null
    }

    $actualPath = [System.IO.Path]::GetFullPath([string]$record.ExecutablePath)
    $expectedPath = [System.IO.Path]::GetFullPath($ExecutablePath)
    $actualCreationUtc = ([DateTime]$record.CreationDate).ToUniversalTime()
    if (
        -not $actualPath.Equals(
            $expectedPath,
            [System.StringComparison]::OrdinalIgnoreCase) -or
        $actualCreationUtc -ne $CreationDateUtc
    ) {
        return $null
    }
    return $record
}

function Get-SteamProcessEvents {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return @()
    }

    $pattern =
        '^\[(?<timestamp>[^\]]+)\].*Game process ' +
        '(?<verb>added|updated|removed)\s*:\s*AppID\s+' +
        '(?<app_id>\d+).*?,\s*ProcID\s+(?<process_id>\d+)(?:,|\s|$)'
    $events = [System.Collections.Generic.List[object]]::new()
    $stream = [System.IO.FileStream]::new(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        ([System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
    )
    $reader = [System.IO.StreamReader]::new(
        $stream,
        [System.Text.Encoding]::UTF8,
        $true
    )
    try {
        while (-not $reader.EndOfStream) {
            $line = $reader.ReadLine()
            $match = [System.Text.RegularExpressions.Regex]::Match(
                $line,
                $pattern,
                [System.Text.RegularExpressions.RegexOptions]::IgnoreCase
            )
            if (-not $match.Success) {
                continue
            }
            $eventTimestamp = [DateTime]::Parse(
                $match.Groups['timestamp'].Value,
                [System.Globalization.CultureInfo]::InvariantCulture,
                [System.Globalization.DateTimeStyles]::AssumeLocal
            ).ToUniversalTime()
            $events.Add([pscustomobject]@{
                Timestamp = $eventTimestamp
                Verb = $match.Groups['verb'].Value.ToLowerInvariant()
                AppId = $match.Groups['app_id'].Value
                ProcessId = [int]$match.Groups['process_id'].Value
            })
        }
    } finally {
        $reader.Dispose()
        $stream.Dispose()
    }
    return $events.ToArray()
}

function Test-MatchingSteamRemoval {
    param(
        [Parameter(Mandatory)][object[]]$Events,
        [Parameter(Mandatory)][string]$ExpectedAppId,
        [Parameter(Mandatory)][int]$ExpectedProcessId,
        [Parameter(Mandatory)][DateTime]$NotBefore
    )

    return [bool]($Events | Where-Object {
        $_.Verb -eq 'removed' -and
        $_.AppId -eq $ExpectedAppId -and
        $_.ProcessId -eq $ExpectedProcessId -and
        $_.Timestamp -ge $NotBefore
    } | Select-Object -First 1)
}

function Get-OtherLiveSteamGameRegistrations {
    param(
        [Parameter(Mandatory)][object[]]$Events,
        [Parameter(Mandatory)][string]$ExpectedAppId,
        [Parameter(Mandatory)][int]$ExpectedProcessId
    )

    $states = @{}
    foreach ($event in $Events) {
        $key = '{0}:{1}' -f $event.AppId, $event.ProcessId
        if ($event.Verb -eq 'removed') {
            $states.Remove($key)
        } else {
            $states[$key] = $event
        }
    }

    $live = [System.Collections.Generic.List[object]]::new()
    foreach ($event in $states.Values) {
        if (
            ($event.AppId -eq $ExpectedAppId -and
                $event.ProcessId -eq $ExpectedProcessId) -or
            $event.AppId -eq '250820'
        ) {
            continue
        }
        $process = Get-CimInstance -ClassName Win32_Process `
            -Filter "ProcessId = $($event.ProcessId)" `
            -ErrorAction SilentlyContinue
        if ($null -ne $process) {
            $live.Add([pscustomobject]@{
                AppId = $event.AppId
                ProcessId = $event.ProcessId
                Name = $process.Name
                ExecutablePath = $process.ExecutablePath
            })
        }
    }
    return $live.ToArray()
}

function Wait-SteamReady {
    param([Parameter(Mandatory)][int]$TimeoutSeconds)

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $steam = Get-Process -Name steam -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($steam) {
            return $steam
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Steam did not restart within $TimeoutSeconds seconds."
}

function Restart-SteamAfterStaleExit {
    $steamProcesses = Get-Process -Name @('steam', 'steamwebhelper') `
        -ErrorAction SilentlyContinue
    if (-not $steamProcesses) {
        return 'Steam was already closed, so its stale app state is cleared.'
    }

    Write-WatchdogLog `
        'Requesting a clean Steam shutdown to clear stale app state.' | Out-Null
    Start-Process -FilePath $SteamExe -ArgumentList '-shutdown' `
        -WindowStyle Hidden | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds($SteamShutdownTimeoutSeconds)
    do {
        Start-Sleep -Milliseconds 250
        $steamProcesses = Get-Process -Name @('steam', 'steamwebhelper') `
            -ErrorAction SilentlyContinue
    } while ($steamProcesses -and [DateTime]::UtcNow -lt $deadline)

    if ($steamProcesses) {
        if (-not $ForceSteamRecovery) {
            throw (
                'Steam did not exit cleanly; automatic force recovery is disabled. ' +
                'Use -ForceSteamRecovery to permit terminating only Steam client helpers.')
        }
        Write-WatchdogLog `
            'Steam did not exit cleanly; force-closing client helpers.' | Out-Null
        $steamProcesses |
            Where-Object ProcessName -in @('steam', 'steamwebhelper') |
            Stop-Process -Force
        Start-Sleep -Seconds 2
        if (Get-Process -Name @('steam', 'steamwebhelper') -ErrorAction SilentlyContinue) {
            throw 'Steam client helpers remained after forced recovery.'
        }
    }

    Write-WatchdogLog `
        'Restarting Steam silently after stale-state cleanup.' | Out-Null
    Start-Process -FilePath $SteamExe -ArgumentList '-silent' `
        -WindowStyle Hidden | Out-Null
    Wait-SteamReady -TimeoutSeconds $SteamStartupTimeoutSeconds | Out-Null
    return 'Steam was restarted and the stale app state was cleared.'
}

$expectedCreationUtc = ([DateTime]::Parse(
    $GameCreationDateUtc,
    [System.Globalization.CultureInfo]::InvariantCulture,
    [System.Globalization.DateTimeStyles]::RoundtripKind)).ToUniversalTime()
$watchStartedUtc = [DateTime]::UtcNow
$initialSteam = Get-Process -Name steam -ErrorAction SilentlyContinue |
    Select-Object -First 1
$initialSteamId = if ($initialSteam) { $initialSteam.Id } else { $null }
$runtimeIdentitySpecified = (
    $RuntimeFrontendProcessId -gt 0 -or
    -not [string]::IsNullOrWhiteSpace($RuntimeFrontendCreationDateUtc) -or
    -not [string]::IsNullOrWhiteSpace($RuntimeFrontendExe)
)
$runtimeCreationUtc = $null
if ($runtimeIdentitySpecified) {
    if (
        $RuntimeFrontendProcessId -le 0 -or
        [string]::IsNullOrWhiteSpace($RuntimeFrontendCreationDateUtc) -or
        [string]::IsNullOrWhiteSpace($RuntimeFrontendExe)
    ) {
        throw (
            'Runtime frontend monitoring requires process ID, creation date, ' +
            'and executable path together.')
    }
    $runtimeCreationUtc = ([DateTime]::Parse(
        $RuntimeFrontendCreationDateUtc,
        [System.Globalization.CultureInfo]::InvariantCulture,
        [System.Globalization.DateTimeStyles]::RoundtripKind)).ToUniversalTime()
}

Write-WatchdogLog (
    "Watching Steam AppID $AppId, game PID $GameProcessId, " +
    "created $($expectedCreationUtc.ToString('o')).")

if (-not (Get-ExactProcess -ProcessId $GameProcessId `
        -ExecutablePath $GameExe -CreationDateUtc $expectedCreationUtc)) {
    Complete-Watchdog -Status 'identity_not_live' `
        -Details 'The exact launched game process was not live when monitoring began.'
    return
}

while (Get-ExactProcess -ProcessId $GameProcessId `
        -ExecutablePath $GameExe -CreationDateUtc $expectedCreationUtc) {
    if (
        $runtimeIdentitySpecified -and
        -not (Get-ExactProcess -ProcessId $RuntimeFrontendProcessId `
            -ExecutablePath $RuntimeFrontendExe `
            -CreationDateUtc $runtimeCreationUtc)
    ) {
        Write-WatchdogLog (
            'The exact OpenXR frontend exited while the game remained live; ' +
            'requesting a graceful game close.')
        $game = Get-Process -Id $GameProcessId -ErrorAction SilentlyContinue
        if ($game) {
            $null = $game.CloseMainWindow()
        }
        $closeDeadline = [DateTime]::UtcNow.AddSeconds(
            $RuntimeExitGameCloseSeconds)
        do {
            Start-Sleep -Milliseconds 250
            $exactGame = Get-ExactProcess -ProcessId $GameProcessId `
                -ExecutablePath $GameExe `
                -CreationDateUtc $expectedCreationUtc
        } while ($exactGame -and [DateTime]::UtcNow -lt $closeDeadline)

        if ($exactGame) {
            Write-WatchdogLog (
                "The game ignored close for $RuntimeExitGameCloseSeconds " +
                'seconds; terminating only the verified original game process.')
            $termination = Invoke-CimMethod -InputObject $exactGame `
                -MethodName Terminate
            if ($null -eq $termination -or [int]$termination.ReturnValue -ne 0) {
                throw (
                    "Targeted game termination failed with return value " +
                    "$($termination.ReturnValue).")
            }
        }
        break
    }
    Start-Sleep -Milliseconds 500
}
Write-WatchdogLog 'The exact game process exited; waiting for Steam removal bookkeeping.'

$graceDeadline = [DateTime]::UtcNow.AddSeconds($RemovalGraceSeconds)
do {
    $events = @(Get-SteamProcessEvents -Path $ConsoleLogPath)
    if (Test-MatchingSteamRemoval -Events $events `
            -ExpectedAppId $AppId -ExpectedProcessId $GameProcessId `
            -NotBefore $watchStartedUtc.AddMinutes(-5)) {
        Complete-Watchdog -Status 'clean_exit' `
            -Details 'Steam recorded the matching game-process removal.'
        return
    }
    Start-Sleep -Milliseconds 250
} while ([DateTime]::UtcNow -lt $graceDeadline)

if ($RecoveryMode -eq 'ReportOnly') {
    Complete-Watchdog -Status 'stale_detected' `
        -Details 'The game PID is absent and Steam did not record its removal.'
    return
}

while ($true) {
    $currentSteam = Get-Process -Name steam -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $currentSteam) {
        Complete-Watchdog -Status 'cleared_by_steam_exit' `
            -Details 'Steam exited independently, clearing the stale registration.'
        return
    }
    if ($null -ne $initialSteamId -and $currentSteam.Id -ne $initialSteamId) {
        Complete-Watchdog -Status 'cleared_by_steam_restart' `
            -Details 'Steam restarted independently, clearing the stale registration.'
        return
    }

    $events = @(Get-SteamProcessEvents -Path $ConsoleLogPath)
    if (Test-MatchingSteamRemoval -Events $events `
            -ExpectedAppId $AppId -ExpectedProcessId $GameProcessId `
            -NotBefore $watchStartedUtc.AddMinutes(-5)) {
        Complete-Watchdog -Status 'late_clean_exit' `
            -Details 'Steam eventually recorded the matching process removal.'
        return
    }

    $otherGames = @(Get-OtherLiveSteamGameRegistrations -Events $events `
        -ExpectedAppId $AppId -ExpectedProcessId $GameProcessId)
    if ($otherGames.Count -eq 0) {
        break
    }
    $summary = ($otherGames | ForEach-Object {
        'AppID {0} PID {1} ({2})' -f $_.AppId, $_.ProcessId, $_.Name
    }) -join ', '
    Write-WatchdogLog (
        "Deferring stale-state recovery while another Steam game is live: $summary")
    Start-Sleep -Seconds $OtherGamePollSeconds
}

try {
    $details = Restart-SteamAfterStaleExit
    Complete-Watchdog -Status 'recovered' -Details $details
} catch {
    Complete-Watchdog -Status 'recovery_failed' -Details $_.Exception.Message
}
