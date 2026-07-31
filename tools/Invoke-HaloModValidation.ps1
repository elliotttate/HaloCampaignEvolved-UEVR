[CmdletBinding()]
param(
    [ValidateSet('Plan', 'Offline', 'LiveSmoke', 'Full', 'Soak')]
    [string]$Suite = 'Offline',

    [string]$OutputDirectory = '',

    [string]$BuildDirectory = '',

    [string]$GameDll = (
        'E:\SteamLibrary\steamapps\common\Halo Campaign Evolved\' +
        'Meteorite\Binaries\Win64\HaloSimulation_tag_release.dll'),

    [string]$ExpectedGameDllSha256 =
        '82B8A3A006BA3F981D6857DC7F4E4E929AE5282587F31F92F77A3FA78F4B2DAC',

    [string]$ProfileRoot = (
        (Join-Path $env:APPDATA 'UnrealVRMod\HaloCampaignEvolved')),

    [string]$OperatorPackageRoot = (
        'E:\Github\UEVRMetaXROperator\dist\release\' +
        'UEVR-Meta-XR-Operator-205.1-nightly-01139-full-controls-v3'),

    [string]$ReticleImage = '',

    [ValidateSet('cyan', 'red', 'any')]
    [string]$ReticleColor = 'any',

    [ValidateRange(1.0, 75.0)]
    [double]$PoseRotationDegrees = 15.0,

    [ValidateRange(15, 86400)]
    [int]$SoakSeconds = 600,

    [ValidateRange(0.1, 60.0)]
    [double]$SoakSampleSeconds = 1.0,

    [ValidateRange(5.0, 600.0)]
    [double]$SoakStatusSampleSeconds = 30.0,

    [ValidateRange(0.1, 100.0)]
    [double]$MaximumPrivateBytesGrowthMbPerMinute = 4.0,

    [ValidateRange(0.1, 100.0)]
    [double]$MaximumHandleGrowthPerMinute = 5.0,

    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if (-not $BuildDirectory) {
    $BuildDirectory = Join-Path $repoRoot 'build-next'
}
if (-not $OutputDirectory) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputDirectory = Join-Path $repoRoot "diagnostics\validation\$stamp-$Suite"
}

$inventory = @(
    [pscustomobject]@{ Id='OFF-01'; Tier='Offline'; Test='Release build and CTest source/reticle gates'; Automation='automatic' },
    [pscustomobject]@{ Id='OFF-02'; Tier='Offline'; Test='Shipping Halo DLL hash and hook signatures'; Automation='automatic' },
    [pscustomobject]@{ Id='OFF-03'; Tier='Offline'; Test='Plugin status/diagnostic exports'; Automation='automatic' },
    [pscustomobject]@{ Id='OFF-04'; Tier='Offline'; Test='Profile configuration and camera presets'; Automation='automatic' },
    [pscustomobject]@{ Id='OFF-05'; Tier='Offline'; Test='Repository, package, and active-profile parity'; Automation='automatic' },
    [pscustomobject]@{ Id='OFF-06'; Tier='Offline'; Test='Package SHA256SUMS integrity'; Automation='automatic' },
    [pscustomobject]@{ Id='OFF-07'; Tier='Offline'; Test='Reticle analyzer and optional raw image oracle'; Automation='automatic' },
    [pscustomobject]@{ Id='LIVE-01'; Tier='Live'; Test='OpenXR, Operator, tracking, hooks, weapon readiness'; Automation='automatic' },
    [pscustomobject]@{ Id='LIVE-02'; Tier='Live'; Test='Gameplay and world-reticle ownership markers'; Automation='automatic' },
    [pscustomobject]@{ Id='POSE-01'; Tier='Live'; Test='25-case right grip/aim 6DOF numeric matrix'; Automation='automatic' },
    [pscustomobject]@{ Id='POSE-02'; Tier='Live'; Test='Visual-fire matrix and projectile convergence'; Automation='automatic' },
    [pscustomobject]@{ Id='RET-01'; Tier='Live'; Test='Raw 250x250 authored reticle shape/color'; Automation='automatic with -ReticleImage' },
    [pscustomobject]@{ Id='RET-02'; Tier='Live'; Test='Rendered stereo reticle oracle'; Automation='automatic capture and image oracle' },
    [pscustomobject]@{ Id='HMD-01'; Tier='Extended'; Test='Head/controller independence matrix'; Automation='specified; diagnostics extension required' },
    [pscustomobject]@{ Id='HAND-01'; Tier='Extended'; Test='Floating wrists, arm IK, tracking loss, extreme reach'; Automation='specified; diagnostics extension required' },
    [pscustomobject]@{ Id='TWO-01'; Tier='Extended'; Test='Two-hand acquire/latch/release and shot basis'; Automation='bridge bit exposed; scripted matrix pending' },
    [pscustomobject]@{ Id='INP-01'; Tier='Extended'; Test='Locomotion, deadzone, D-pad shift, button release'; Automation='specified; input diagnostics required' },
    [pscustomobject]@{ Id='WPN-01'; Tier='Extended'; Test='Weapon-family ballistic and zoom matrix'; Automation='semi-automatic' },
    [pscustomobject]@{ Id='LIFE-01'; Tier='Extended'; Test='Menu, death, reload, switch, zoom, hot-reload lifecycle'; Automation='semi-automatic' },
    [pscustomobject]@{ Id='PERF-01'; Tier='Soak'; Test='Frame progression, private bytes, handles'; Automation='automatic' },
    [pscustomobject]@{ Id='DEP-01'; Tier='Extended'; Test='Standalone run with Operator/MCP absent'; Automation='manual launch plus automatic status checks' },
    [pscustomobject]@{ Id='HMD-02'; Tier='Hardware'; Test='Real-headset axes, latency, flicker, geometry, audio'; Automation='manual checklist' }
)

if ($Suite -eq 'Plan') {
    $inventory | Format-Table -AutoSize
    return
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$results = [System.Collections.Generic.List[object]]::new()

function Add-Result {
    param(
        [string]$Id,
        [string]$Tier,
        [string]$Name,
        [ValidateSet('PASS', 'FAIL', 'SKIP', 'MANUAL')]
        [string]$Status,
        [double]$DurationMilliseconds,
        [string]$Details,
        [string[]]$Artifacts = @()
    )

    $results.Add([pscustomobject][ordered]@{
        id = $Id
        tier = $Tier
        test = $Name
        status = $Status
        duration_ms = [Math]::Round($DurationMilliseconds, 2)
        details = $Details
        artifacts = @($Artifacts)
    })
}

function Invoke-ValidationCase {
    param(
        [string]$Id,
        [string]$Tier,
        [string]$Name,
        [scriptblock]$Body
    )

    Write-Host "[$Id] $Name"
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $value = & $Body
        $details = if ($null -eq $value) {
            'Completed without an additional message.'
        } elseif ($value -is [string]) {
            $value
        } else {
            ($value | ConvertTo-Json -Depth 12 -Compress)
        }
        $stopwatch.Stop()
        Add-Result -Id $Id -Tier $Tier -Name $Name -Status PASS `
            -DurationMilliseconds $stopwatch.Elapsed.TotalMilliseconds `
            -Details $details
        Write-Host "  PASS: $details" -ForegroundColor Green
    } catch {
        $stopwatch.Stop()
        $details = $_.Exception.Message
        if ($_.ScriptStackTrace) {
            $details += [Environment]::NewLine + $_.ScriptStackTrace
        }
        Add-Result -Id $Id -Tier $Tier -Name $Name -Status FAIL `
            -DurationMilliseconds $stopwatch.Elapsed.TotalMilliseconds `
            -Details $details
        Write-Host "  FAIL: $($_.Exception.Message)" -ForegroundColor Red
    }
}

function Add-SkippedCase {
    param([string]$Id, [string]$Tier, [string]$Name, [string]$Reason)
    Add-Result -Id $Id -Tier $Tier -Name $Name -Status SKIP `
        -DurationMilliseconds 0 -Details $Reason
    Write-Host "[$Id] SKIP: $Reason" -ForegroundColor Yellow
}

function ConvertTo-NativeCommandLineArgument {
    param([AllowEmptyString()][string]$Argument)

    if ($Argument.Length -gt 0 -and $Argument -notmatch '[\s"]') {
        return $Argument
    }

    # CommandLineToArgvW/CRT quoting: backslashes are literal except before a
    # quote or the closing quote, where they must be doubled.
    $builder = [System.Text.StringBuilder]::new()
    $null = $builder.Append('"')
    $backslashes = 0
    foreach ($character in $Argument.ToCharArray()) {
        if ($character -eq '\') {
            $backslashes++
            continue
        }
        if ($character -eq '"') {
            $null = $builder.Append(('\' * ($backslashes * 2 + 1)))
            $null = $builder.Append('"')
            $backslashes = 0
            continue
        }
        if ($backslashes -gt 0) {
            $null = $builder.Append(('\' * $backslashes))
            $backslashes = 0
        }
        $null = $builder.Append($character)
    }
    if ($backslashes -gt 0) {
        $null = $builder.Append(('\' * ($backslashes * 2)))
    }
    $null = $builder.Append('"')
    return $builder.ToString()
}

function Invoke-HeadlessProcess {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [Parameter(Mandatory)][string]$LogName,
        [switch]$AllowFailure
    )

    $resolvedCommand = Get-Command $FilePath -ErrorAction Stop
    $processInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $processInfo.FileName = $resolvedCommand.Source
    $processInfo.WorkingDirectory = $repoRoot
    $processInfo.UseShellExecute = $false
    $processInfo.RedirectStandardOutput = $true
    $processInfo.RedirectStandardError = $true
    $processInfo.CreateNoWindow = $true
    if ($processInfo.PSObject.Properties['ArgumentList']) {
        foreach ($argument in $ArgumentList) {
            $processInfo.ArgumentList.Add([string]$argument)
        }
    } else {
        # Windows PowerShell 5.1 targets .NET Framework, whose
        # ProcessStartInfo predates ArgumentList. Keep the validation runner
        # usable from both the packaged `powershell` commands and pwsh.
        $processInfo.Arguments = (@($ArgumentList | ForEach-Object {
            ConvertTo-NativeCommandLineArgument ([string]$_)
        }) -join ' ')
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $processInfo
    $null = $process.Start()
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    $logPath = Join-Path $OutputDirectory $LogName
    @(
        "command: $($processInfo.FileName) $($ArgumentList -join ' ')"
        "exit_code: $($process.ExitCode)"
        '--- stdout ---'
        $stdout
        '--- stderr ---'
        $stderr
    ) | Set-Content -LiteralPath $logPath -Encoding utf8

    if ($process.ExitCode -ne 0 -and -not $AllowFailure) {
        throw "Command failed with exit code $($process.ExitCode). See '$logPath'."
    }
    return [pscustomobject]@{
        ExitCode = $process.ExitCode
        Stdout = $stdout
        Stderr = $stderr
        Log = $logPath
    }
}

function Read-KeyValueFile {
    param([string]$Path)
    $values = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^([^=]+)=(.*)$') {
            $values[$matches[1]] = $matches[2]
        }
    }
    return $values
}

function Assert-FileHashEqual {
    param([string[]]$Paths)
    $resolved = @($Paths | ForEach-Object {
        (Resolve-Path -LiteralPath $_ -ErrorAction Stop).Path
    })
    $hashes = @($resolved | ForEach-Object {
        (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash
    })
    if (@($hashes | Select-Object -Unique).Count -ne 1) {
        $rows = for ($index = 0; $index -lt $resolved.Count; $index++) {
            "$($hashes[$index])  $($resolved[$index])"
        }
        throw "Artifact hashes differ:`n$($rows -join [Environment]::NewLine)"
    }
    return $hashes[0]
}

function Resolve-Dumpbin {
    $command = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} `
        'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $candidate = & $vswhere -latest -products '*' -requires `
            Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find `
            'VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe' |
            Select-Object -Last 1
        if ($candidate) {
            return $candidate
        }
    }
    return $null
}

function Invoke-OperatorJson {
    param([string]$ToolName, [hashtable]$Arguments = @{})
    $invokeTool = Join-Path $OperatorPackageRoot 'Invoke-MetaXROperatorTool.ps1'
    $response = & $invokeTool -ToolName $ToolName -ArgumentsJson (
        $Arguments | ConvertTo-Json -Depth 20 -Compress)
    if ($null -eq $response -or $null -eq $response.result) {
        throw "Operator tool '$ToolName' returned no result."
    }
    if ($response.result.PSObject.Properties['isError'] -and
        [bool]$response.result.isError) {
        throw "Operator tool '$ToolName' returned an MCP error."
    }
    $text = @($response.result.content |
        Where-Object { $_.type -eq 'text' } |
        Select-Object -First 1).text
    if (-not $text) {
        throw "Operator tool '$ToolName' returned no JSON text."
    }
    $value = $text | ConvertFrom-Json
    if (($value.PSObject.Properties['error'] -and $value.error) -or
        ($value.PSObject.Properties['success'] -and
            $value.success -eq $false)) {
        throw "Operator tool '$ToolName' failed: $text"
    }
    return $value
}

function Invoke-OfflineTier {
    $builtDll = Join-Path $BuildDirectory 'Release\HaloCEMotionControls.dll'
    $packageDll = Join-Path $OperatorPackageRoot 'plugins\HaloCEMotionControls.dll'
    $profileDll = Join-Path $ProfileRoot 'plugins\HaloCEMotionControls.dll'
    $repoLua = Join-Path $repoRoot 'scripts\halo_motion_reticle.lua'
    $packageLua = Join-Path $OperatorPackageRoot 'scripts\halo_motion_reticle.lua'
    $profileLua = Join-Path $ProfileRoot 'scripts\halo_motion_reticle.lua'

    Invoke-ValidationCase OFF-01 Offline `
        'Release build and CTest source/reticle gates' {
        if (-not $SkipBuild) {
            if (-not (Test-Path -LiteralPath (
                Join-Path $BuildDirectory 'CMakeCache.txt'))) {
                $null = Invoke-HeadlessProcess cmake @(
                    '-S', $repoRoot, '-B', $BuildDirectory,
                    '-DBUILD_TESTING=ON') '01-cmake-configure.log'
            }
            $null = Invoke-HeadlessProcess cmake @(
                '--build', $BuildDirectory, '--config', 'Release') `
                '01-cmake-build.log'
        }
        $ctest = Invoke-HeadlessProcess ctest @(
            '--test-dir', $BuildDirectory, '-C', 'Release',
            '--output-on-failure') '01-ctest.log'
        "CTest passed. Log: $($ctest.Log)"
    }

    Invoke-ValidationCase OFF-02 Offline `
        'Shipping Halo DLL hash and hook signatures' {
        $actualHash = (Get-FileHash -LiteralPath $GameDll -Algorithm SHA256).Hash
        if ($actualHash -ne $ExpectedGameDllSha256) {
            throw (
                "Unexpected game DLL SHA-256 $actualHash; expected " +
                "$ExpectedGameDllSha256. Refuse to trust build-specific hooks.")
        }
        $scan = Invoke-HeadlessProcess node @(
            (Join-Path $repoRoot 'tools\scan-native-hook-signatures.mjs'),
            '--json', $GameDll) '02-native-signatures.log'
        $report = $scan.Stdout | ConvertFrom-Json
        if (-not [bool]$report.validationPassed) {
            throw 'Native signature scanner did not validate every direct-call target.'
        }
        $report | ConvertTo-Json -Depth 20 |
            Set-Content (Join-Path $OutputDirectory '02-native-signatures.json')
        "Known shipping DLL and all implemented native signatures validated."
    }

    Invoke-ValidationCase OFF-03 Offline `
        'Plugin status/diagnostic exports and Operator two-hand bit' {
        $dumpbin = Resolve-Dumpbin
        if (-not $dumpbin) {
            throw 'dumpbin.exe was not found; cannot verify binary exports.'
        }
        $dump = Invoke-HeadlessProcess $dumpbin @('/exports', $builtDll) `
            '03-plugin-exports.log'
        foreach ($name in @('HaloCEVR_GetStatus', 'HaloCEVR_GetPoseDiagnostics')) {
            if (-not $dump.Stdout.Contains($name)) {
                throw "Built plugin does not export $name."
            }
        }
        $bridgeSource = Join-Path (
            Split-Path -Parent (Split-Path -Parent $OperatorPackageRoot)) `
            'src\mods\vr\MetaXROperatorBridge.cpp'
        if (-not (Test-Path -LiteralPath $bridgeSource)) {
            $bridgeSource = 'E:\Github\UEVRMetaXROperator\src\mods\vr\MetaXROperatorBridge.cpp'
        }
        $bridgeText = Get-Content -LiteralPath $bridgeSource -Raw
        foreach ($token in @('HALO_CEVR_TWO_HAND_HOLD_ACTIVE',
            '"two_hand_hold_active"')) {
            if (-not $bridgeText.Contains($token)) {
                throw "Operator bridge is missing $token."
            }
        }
        $packagedBackend = Join-Path $OperatorPackageRoot 'UEVRBackend.dll'
        $backendBytes = [System.IO.File]::ReadAllBytes($packagedBackend)
        $backendText = [System.Text.Encoding]::ASCII.GetString($backendBytes)
        if (-not $backendText.Contains('two_hand_hold_active')) {
            throw (
                'Packaged UEVRBackend.dll predates the two-hand status field; ' +
                'rebuild and restage the Operator backend.')
        }
        'Both runtime APIs are exported and the packaged bridge exposes status bit 8.'
    }

    Invoke-ValidationCase OFF-04 Offline `
        'Profile configuration and camera presets' {
        $config = Read-KeyValueFile (Join-Path $ProfileRoot 'config.txt')
        $expected = [ordered]@{
            Frontend_RequestedRuntime = 'openxr_loader.dll'
            VR_ControllersAllowed = 'true'
            VR_ForceMotionControlsActive = 'true'
            VR_AimMethod = '0'
            VR_AimModifyPlayerControlRotation = 'false'
            VR_AimUsePawnControlRotation = 'false'
            VR_DecoupledPitch = 'false'
            VR_DecoupledPitchUIAdjust = 'false'
            VR_SwapControllerInputs = 'false'
            VR_WorldScale = '1.000000'
        }
        foreach ($entry in $expected.GetEnumerator()) {
            if (-not $config.ContainsKey($entry.Key) -or
                $config[$entry.Key] -ne $entry.Value) {
                throw "config.txt expected $($entry.Key)=$($entry.Value)."
            }
        }
        $cameras = Read-KeyValueFile (Join-Path $ProfileRoot 'cameras.txt')
        foreach ($index in 0..2) {
            foreach ($key in @("decoupled_pitch$index",
                "decoupled_pitch_ui_adjust$index")) {
                if ($cameras[$key] -ne 'false') {
                    throw "cameras.txt expected $key=false."
                }
            }
            if ($cameras["world_scale$index"] -ne '1.000000') {
                throw "cameras.txt expected world_scale$index=1.000000."
            }
        }
        'The active UEVR profile cannot recouple pitch, swap hands, or rescale the world.'
    }

    Invoke-ValidationCase OFF-05 Offline `
        'Repository, package, and active-profile parity' {
        $dllHash = Assert-FileHashEqual @($builtDll, $packageDll, $profileDll)
        $luaHash = Assert-FileHashEqual @($repoLua, $packageLua, $profileLua)
        "DLL=$dllHash; Lua=$luaHash"
    }

    Invoke-ValidationCase OFF-06 Offline 'Package SHA256SUMS integrity' {
        $manifest = Join-Path $OperatorPackageRoot 'SHA256SUMS.txt'
        $mismatches = [System.Collections.Generic.List[string]]::new()
        $checked = 0
        foreach ($line in Get-Content -LiteralPath $manifest) {
            if (-not $line.Trim()) { continue }
            if ($line -notmatch '^([0-9A-Fa-f]{64})\s{2}(.+)$') {
                $mismatches.Add("Malformed manifest line: $line")
                continue
            }
            $expectedHash = $matches[1].ToUpperInvariant()
            $relativePath = $matches[2]
            $path = Join-Path $OperatorPackageRoot $relativePath
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                $mismatches.Add("Missing: $relativePath")
                continue
            }
            $checked++
            $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
            if ($actualHash -ne $expectedHash) {
                $mismatches.Add(
                    "$relativePath expected $expectedHash actual $actualHash")
            }
        }
        if ($mismatches.Count -gt 0) {
            throw "SHA256SUMS failed:`n$($mismatches -join [Environment]::NewLine)"
        }
        "Validated $checked packaged files."
    }

    Invoke-ValidationCase OFF-07 Offline `
        'Reticle analyzer syntax and optional raw image oracle' {
        $analyzer = Join-Path $repoRoot 'tests\analyze-reticle.py'
        $renderedAnalyzer = Join-Path $repoRoot `
            'tests\analyze-rendered-reticle.py'
        $syntax = Invoke-HeadlessProcess py @(
            '-3', '-c',
            "import ast,pathlib; " +
            "ast.parse(pathlib.Path(r'$analyzer').read_text(encoding='utf-8')); " +
            "ast.parse(pathlib.Path(r'$renderedAnalyzer').read_text(encoding='utf-8'))") `
            '07-reticle-analyzer-syntax.log'
        if ($ReticleImage) {
            $json = Join-Path $OutputDirectory '07-reticle-image.json'
            $analysis = Invoke-HeadlessProcess py @(
                '-3', $analyzer, $ReticleImage,
                '--color', $ReticleColor, '--json', $json) `
                '07-reticle-image.log'
            return "Raw reticle image passed. $($analysis.Stdout.Trim())"
        }
        return (
            'Analyzer parsed successfully. Pass -ReticleImage with a raw ' +
            '250x250 render-target export to enforce shape and color.')
    }
}

function Invoke-LiveTier {
    $status = $null
    Invoke-ValidationCase LIVE-01 Live `
        'OpenXR, Operator, tracking, hooks, and weapon readiness' {
        $script:liveStatus = Invoke-OperatorJson UEVR_Status
        $status = $script:liveStatus
        $failures = [System.Collections.Generic.List[string]]::new()
        if (-not [bool]$status.openxr.session_ready) {
            $failures.Add('OpenXR session is not ready.')
        }
        if (-not [bool]$status.openxr.operator_extension_enabled) {
            $failures.Add('Meta XR Operator extension is not enabled.')
        }
        if (-not [bool]$status.runtime.hmd_active) {
            $failures.Add('HMD/runtime is not active.')
        }
        if (-not [bool]$status.runtime.using_controllers) {
            $failures.Add('UEVR is not using tracked controllers.')
        }
        $halo = $status.halo_motion_controls
        foreach ($property in @('loaded', 'status_api', 'tracking_valid',
            'native_hooks_installed', 'native_visual_hook_installed',
            'native_projectile_hook_installed', 'visual_weapon_attached')) {
            if (-not [bool]$halo.$property) {
                $failures.Add("halo_motion_controls.$property is false.")
            }
        }
        # Blam object indices are signed 32-bit handles and valid handles can
        # appear negative. Only 0xFFFFFFFF/-1 is the invalid sentinel.
        if ([int]$halo.local_weapon_index -eq -1) {
            $failures.Add('No local first-person weapon is active.')
        }
        if ($failures.Count -gt 0) {
            throw ($failures -join ' ')
        }
        $status | ConvertTo-Json -Depth 30 |
            Set-Content (Join-Path $OutputDirectory 'live-status.json')
        "Runtime $($status.runtime.name); weapon index $($halo.local_weapon_index)."
    }

    Invoke-ValidationCase LIVE-02 Live `
        'Gameplay and world-reticle ownership markers' {
        $dataRoot = Join-Path $ProfileRoot 'data'
        $expected = [ordered]@{
            'halo_motion_gameplay.active' = '^ready'
            'halo_motion_reticle.active' = '^right-controller world reticle ready'
            'halo_motion_reticle.error' = '^\s*$'
        }
        foreach ($entry in $expected.GetEnumerator()) {
            $path = Join-Path $dataRoot $entry.Key
            $contents = Get-Content -LiteralPath $path -Raw
            if ($contents -notmatch $entry.Value) {
                throw "$($entry.Key) contains '$($contents.Trim())'."
            }
        }
        $widgetPath = Join-Path $dataRoot 'halo_motion_reticle.widget'
        $widget = Get-Content -LiteralPath $widgetPath -Raw
        if ($widget -match 'inactive|error|fallback') {
            throw "World-reticle widget is not production-ready: $($widget.Trim())"
        }
        "World-reticle marker is owned by the authored UMG replacement: $($widget.Trim())"
    }

    Invoke-ValidationCase POSE-01 Live `
        '25-case right grip/aim 6DOF numeric matrix' {
        $poseOutput = Join-Path $OutputDirectory 'pose-matrix-numeric'
        $arguments = @(
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
            (Join-Path $repoRoot 'tools\Invoke-HaloPoseMatrixTest.ps1'),
            '-Suite', 'FullNumeric',
            '-OutputDirectory', $poseOutput,
            '-RotationDegrees', $PoseRotationDegrees,
            '-OperatorPackageRoot', $OperatorPackageRoot)
        $run = Invoke-HeadlessProcess pwsh $arguments `
            'pose-matrix-numeric-run.log'
        $summaryPath = Join-Path $poseOutput 'pose-matrix-summary.json'
        $poseSummary = Get-Content -LiteralPath $summaryPath -Raw |
            ConvertFrom-Json
        if ([int]$poseSummary.failed_count -ne 0) {
            throw "Pose matrix reported $($poseSummary.failed_count) failures."
        }
        "Passed $($poseSummary.passed_count)/$($poseSummary.case_count) cases."
    }

    if ($Suite -ne 'LiveSmoke') {
        Invoke-ValidationCase POSE-02 Live `
            'Visual-fire matrix and projectile convergence' {
            $poseOutput = Join-Path $OutputDirectory 'pose-matrix-fire'
            $arguments = @(
                '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                (Join-Path $repoRoot 'tools\Invoke-HaloPoseMatrixTest.ps1'),
                '-Suite', 'VisualFire',
                '-OutputDirectory', $poseOutput,
                '-RotationDegrees', $PoseRotationDegrees,
                '-OperatorPackageRoot', $OperatorPackageRoot)
            $run = Invoke-HeadlessProcess pwsh $arguments `
                'pose-matrix-fire-run.log'
            $summaryPath = Join-Path $poseOutput 'pose-matrix-summary.json'
            $poseSummary = Get-Content -LiteralPath $summaryPath -Raw |
                ConvertFrom-Json
            if ([int]$poseSummary.failed_count -ne 0) {
                throw (
                    "Visual-fire matrix reported " +
                    "$($poseSummary.failed_count) failures.")
            }
            "Passed $($poseSummary.passed_count)/$($poseSummary.case_count) firing cases."
        }

        $invokeTool = Join-Path $OperatorPackageRoot `
            'Invoke-MetaXROperatorTool.ps1'
        Invoke-ValidationCase RET-02 Live `
            'Rendered stereo reticle image oracle' {
            # VisualFire intentionally leaves the controller at its final
            # (pitch-down) case.  The stereo oracle's calibrated search point
            # is the neutral aim pose, so restore that pose before capturing.
            foreach ($pose in @(
                @{
                    hand = 'right'
                    pose_type = 'grip'
                    base_space = 'local_floor'
                    position = @(0.30, -0.33, -0.46)
                    orientation = @(0.0, 0.0, 0.0, 1.0)
                    duration_seconds = 0.0
                },
                @{
                    hand = 'right'
                    pose_type = 'aim'
                    base_space = 'local_floor'
                    position = @(0.30, -0.30, -0.555)
                    orientation = @(0.0, 0.0, 0.0, 1.0)
                    duration_seconds = 0.0
                }
            )) {
                $null = & $invokeTool `
                    -ToolName openxr_set_controller_pose `
                    -ArgumentsJson ($pose | ConvertTo-Json -Compress)
            }
            Start-Sleep -Milliseconds 750

            $artifacts = @()
            foreach ($eye in @('left', 'right')) {
                $path = Join-Path $OutputDirectory "reticle-$eye-eye.png"
                $null = & $invokeTool `
                    -ToolName openxr_capture_composited_image `
                    -ArgumentsJson (@{ eye = $eye } |
                        ConvertTo-Json -Compress) `
                    -OutputImage $path
                if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                    throw "Operator did not capture $eye eye."
                }
                $artifacts += $path
            }
            $analyzer = Join-Path $repoRoot `
                'tests\analyze-rendered-reticle.py'
            $json = Join-Path $OutputDirectory `
                'reticle-composited-analysis.json'
            $arguments = @(
                '-3', $analyzer,
                '--left', $artifacts[0],
                '--right', $artifacts[1],
                '--json', $json)
            if ($ReticleImage) {
                $arguments += @('--authored', $ReticleImage)
            }
            $analysis = Invoke-HeadlessProcess py $arguments `
                'reticle-composited-analysis.log' -AllowFailure
            $report = $analysis.Stdout | ConvertFrom-Json
            if ($analysis.ExitCode -ne 0 -or -not [bool]$report.passed) {
                throw (
                    'Rendered reticle oracle failed: ' +
                    ($report.failures -join ' '))
            }
            return (
                'Rendered ring passed in both eyes. right=' +
                "($($report.right_ring.centroid -join ',')); left=" +
                "($($report.left_ring.centroid -join ',')); stereo_error=" +
                "$([Math]::Round([double]$report.stereo_correspondence_error_px, 3))px; " +
                "analysis=$json")
        }
    } else {
        Add-SkippedCase POSE-02 Live `
            'Visual-fire matrix and projectile convergence' `
            'LiveSmoke is non-firing; use -Suite Full for ballistic validation.'
        Add-SkippedCase RET-02 Live `
            'Rendered stereo reticle image oracle' `
            'LiveSmoke intentionally omits image capture.'
    }
}

function Invoke-SoakTier {
    Invoke-ValidationCase PERF-01 Soak `
        'Frame progression, process memory, and handle growth' {
        $samples = [System.Collections.Generic.List[object]]::new()
        $initialStatus = Invoke-OperatorJson UEVR_Status
        $initialSequence = [uint32](
            $initialStatus.halo_motion_controls.pose_diagnostics.sequence)
        $latestSequence = $initialSequence
        $nextStatusSample = [DateTime]::UtcNow.AddSeconds(
            $SoakStatusSampleSeconds)
        $deadline = [DateTime]::UtcNow.AddSeconds($SoakSeconds)
        while ([DateTime]::UtcNow -lt $deadline) {
            $game = Get-Process -Name HaloCampaignEvolved `
                -ErrorAction Stop | Select-Object -First 1
            if ([DateTime]::UtcNow -ge $nextStatusSample) {
                $status = Invoke-OperatorJson UEVR_Status
                $latestSequence = [uint32](
                    $status.halo_motion_controls.pose_diagnostics.sequence)
                $nextStatusSample = [DateTime]::UtcNow.AddSeconds(
                    $SoakStatusSampleSeconds)
            }
            $samples.Add([pscustomobject]@{
                utc = [DateTime]::UtcNow.ToString('o')
                sequence = $latestSequence
                private_bytes = [int64]$game.PrivateMemorySize64
                handles = [int]$game.HandleCount
            })
            Start-Sleep -Milliseconds ([int]($SoakSampleSeconds * 1000.0))
        }
        $finalStatus = Invoke-OperatorJson UEVR_Status
        $finalSequence = [uint32](
            $finalStatus.halo_motion_controls.pose_diagnostics.sequence)
        if ($samples.Count -lt 2) {
            throw 'Soak collected fewer than two samples.'
        }
        $csv = Join-Path $OutputDirectory 'soak-samples.csv'
        $samples | Export-Csv -LiteralPath $csv -NoTypeInformation
        $first = $samples[0]
        $last = $samples[$samples.Count - 1]
        if ($finalSequence -le $initialSequence) {
            throw 'Pose diagnostic sequence did not advance during soak.'
        }
        $minutes = [Math]::Max($SoakSeconds / 60.0, 0.001)
        $memoryRate = (($last.private_bytes - $first.private_bytes) / 1MB) /
            $minutes
        $handleRate = ($last.handles - $first.handles) / $minutes
        if ($memoryRate -gt $MaximumPrivateBytesGrowthMbPerMinute) {
            throw "Private bytes grew $([Math]::Round($memoryRate,2)) MB/min."
        }
        if ($handleRate -gt $MaximumHandleGrowthPerMinute) {
            throw "Handles grew $([Math]::Round($handleRate,2))/min."
        }
        "Samples=$($samples.Count); status_interval_seconds=$SoakStatusSampleSeconds; sequence_delta=$($finalSequence - $initialSequence); private_bytes_mb_per_min=$([Math]::Round($memoryRate,2)); handles_per_min=$([Math]::Round($handleRate,2)); $csv"
    }
}

$startedUtc = [DateTime]::UtcNow
if ($Suite -in @('Offline', 'Full')) {
    Invoke-OfflineTier
}
if ($Suite -in @('LiveSmoke', 'Full')) {
    Invoke-LiveTier
}
if ($Suite -eq 'Soak') {
    Invoke-SoakTier
}

$completedUtc = [DateTime]::UtcNow
$summary = [pscustomobject][ordered]@{
    schema_version = 1
    suite = $Suite
    repo_root = $repoRoot
    output_directory = $OutputDirectory
    started_utc = $startedUtc.ToString('o')
    completed_utc = $completedUtc.ToString('o')
    passed = @($results | Where-Object status -eq PASS).Count
    failed = @($results | Where-Object status -eq FAIL).Count
    skipped = @($results | Where-Object status -eq SKIP).Count
    manual = @($results | Where-Object status -eq MANUAL).Count
    results = @($results)
    inventory = $inventory
}
$jsonPath = Join-Path $OutputDirectory 'validation-summary.json'
$csvPath = Join-Path $OutputDirectory 'validation-summary.csv'
$markdownPath = Join-Path $OutputDirectory 'validation-summary.md'
$summary | ConvertTo-Json -Depth 30 |
    Set-Content -LiteralPath $jsonPath -Encoding utf8
$results | Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding utf8

$markdown = [System.Collections.Generic.List[string]]::new()
$markdown.Add("# Halo Campaign Evolved UEVR validation: $Suite")
$markdown.Add('')
$markdown.Add("- Started: $($summary.started_utc)")
$markdown.Add("- Passed: $($summary.passed)")
$markdown.Add("- Failed: $($summary.failed)")
$markdown.Add("- Skipped: $($summary.skipped)")
$markdown.Add('')
$markdown.Add('| ID | Tier | Status | Test | Details |')
$markdown.Add('|---|---|---:|---|---|')
foreach ($result in $results) {
    $safeDetails = $result.details.Replace('|', '\|').Replace("`r", ' ').Replace("`n", ' ')
    $markdown.Add("| $($result.id) | $($result.tier) | $($result.status) | $($result.test) | $safeDetails |")
}
$markdown | Set-Content -LiteralPath $markdownPath -Encoding utf8

$results | Format-Table id, tier, status, test -AutoSize
Write-Host "JSON: $jsonPath"
Write-Host "Markdown: $markdownPath"

if ($summary.failed -gt 0) {
    exit 1
}
