[CmdletBinding()]
param(
    [string]$OutputDirectory = (
        Join-Path $PSScriptRoot `
            '..\diagnostics\validation\final-reticle-sweep-20260731'),

    [string]$InvokeToolPath = (
        'E:\Github\UEVRMetaXROperator\dist\release\' +
        'UEVR-Meta-XR-Operator-205.1-nightly-01139-full-controls-v3\' +
        'Invoke-MetaXROperatorTool.ps1'),

    [string]$AuthoredReference = (
        Join-Path $PSScriptRoot `
            ('..\diagnostics\validation\final-depth-reticle-20260731\' +
             'reticle-authored-final.exr')),

    [int]$HaloPid = 11856
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$InvokeToolPath = (Resolve-Path -LiteralPath $InvokeToolPath).Path
$AuthoredReference = (Resolve-Path -LiteralPath $AuthoredReference).Path
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$analyzer = Join-Path $repoRoot 'tests\analyze-rendered-reticle.py'
$montageBuilder = Join-Path $repoRoot 'tools\Build-HaloReticleSweepMontage.py'
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
Copy-Item -LiteralPath $AuthoredReference `
    -Destination (Join-Path $OutputDirectory 'authored-reference.exr') `
    -Force

$neutralGripPosition = @(0.30, -0.33, -0.46)
$neutralAimPosition = @(0.30, -0.30, -0.555)
$identity = @(0.0, 0.0, 0.0, 1.0)
$referenceWidth = 840.0
$referenceHeight = 880.0
$neutralRightPoint = @(316.5, 447.5)
$horizontalFocalPixels = 379.6
$verticalFocalPixels = 376.9
$posePositionTolerance = 0.005
$poseAngleToleranceDegrees = 1.0

function Invoke-OperatorJson {
    param([string]$ToolName, [hashtable]$Arguments = @{})
    $response = & $InvokeToolPath `
        -ToolName $ToolName `
        -ArgumentsJson ($Arguments | ConvertTo-Json -Depth 20 -Compress)
    $text = @(
        $response.result.content |
            Where-Object { $_.type -eq 'text' } |
            Select-Object -First 1
    ).text
    if (-not $text) {
        throw "Operator tool '$ToolName' returned no JSON text."
    }
    $value = $text | ConvertFrom-Json
    if ($value.PSObject.Properties['error'] -and $value.error) {
        throw "Operator tool '$ToolName' failed: $text"
    }
    return $value
}

function Invoke-EyeCapture {
    param([string]$Eye, [string]$Path)
    $null = & $InvokeToolPath `
        -ToolName 'openxr_capture_composited_image' `
        -ArgumentsJson (@{ eye = $Eye } | ConvertTo-Json -Compress) `
        -OutputImage $Path
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Operator did not write $Eye-eye capture '$Path'."
    }
}

function New-AxisQuaternion {
    param([ValidateSet('Yaw','Pitch','Roll')]$Axis, [double]$Degrees)
    $half = $Degrees * [Math]::PI / 360.0
    $s = [Math]::Sin($half)
    $c = [Math]::Cos($half)
    switch ($Axis) {
        'Yaw' { return @(0.0, $s, 0.0, $c) }
        'Pitch' { return @($s, 0.0, 0.0, $c) }
        'Roll' { return @(0.0, 0.0, $s, $c) }
    }
}

function Multiply-Quaternion {
    param([double[]]$Left, [double[]]$Right)
    $lx,$ly,$lz,$lw = $Left
    $rx,$ry,$rz,$rw = $Right
    $value = @(
        ($lw*$rx + $lx*$rw + $ly*$rz - $lz*$ry),
        ($lw*$ry - $lx*$rz + $ly*$rw + $lz*$rx),
        ($lw*$rz + $lx*$ry - $ly*$rx + $lz*$rw),
        ($lw*$rw - $lx*$rx - $ly*$ry - $lz*$rz))
    $length = [Math]::Sqrt(($value | ForEach-Object { $_ * $_ } |
        Measure-Object -Sum).Sum)
    return @($value | ForEach-Object { $_ / $length })
}

function New-CombinedQuaternion {
    param([double]$Yaw, [double]$Pitch, [double]$Roll)
    $value = $identity
    foreach ($axis in @(
        @{ Name='Yaw'; Degrees=$Yaw },
        @{ Name='Pitch'; Degrees=$Pitch },
        @{ Name='Roll'; Degrees=$Roll })) {
        $value = Multiply-Quaternion `
            -Left $value `
            -Right (New-AxisQuaternion $axis.Name $axis.Degrees)
    }
    return $value
}

function Get-VectorError {
    param($Actual, [double[]]$Expected)
    return [Math]::Sqrt(
        [Math]::Pow([double]$Actual.x - $Expected[0], 2) +
        [Math]::Pow([double]$Actual.y - $Expected[1], 2) +
        [Math]::Pow([double]$Actual.z - $Expected[2], 2))
}

function Get-QuaternionErrorDegrees {
    param($Actual, [double[]]$Expected)
    $dot = [Math]::Abs(
        [double]$Actual.x*$Expected[0] +
        [double]$Actual.y*$Expected[1] +
        [double]$Actual.z*$Expected[2] +
        [double]$Actual.w*$Expected[3])
    return 2.0 * [Math]::Acos([Math]::Min(1.0, $dot)) * 180.0 / [Math]::PI
}

function Set-RightPose {
    param($Case, [uint32]$AfterSequence)
    $null = Invoke-OperatorJson `
        -ToolName 'openxr_set_controller_pose' `
        -Arguments @{
            hand='right'; pose_type='grip'; base_space='local'
            position=$neutralGripPosition; orientation=$identity
            duration_seconds=0.0
        }
    $null = Invoke-OperatorJson `
        -ToolName 'openxr_set_controller_pose' `
        -Arguments @{
            hand='right'; pose_type='aim'; base_space='local'
            position=$neutralAimPosition; orientation=$Case.Orientation
            duration_seconds=0.0
        }

    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.Elapsed.TotalSeconds -lt 12.0) {
        $status = Invoke-OperatorJson -ToolName 'UEVR_Status'
        $diagnostics = $status.halo_motion_controls.pose_diagnostics
        $positionError = Get-VectorError `
            -Actual $diagnostics.right_aim.position `
            -Expected $neutralAimPosition
        $angleError = Get-QuaternionErrorDegrees `
            -Actual $diagnostics.right_aim.rotation `
            -Expected $Case.Orientation
        if ([uint32]$diagnostics.sequence -gt $AfterSequence -and
            $positionError -le $posePositionTolerance -and
            $angleError -le $poseAngleToleranceDegrees) {
            return [pscustomobject]@{
                Status = $status
                PositionErrorMeters = $positionError
                AngleErrorDegrees = $angleError
                WaitMilliseconds = $watch.Elapsed.TotalMilliseconds
            }
        }
        Start-Sleep -Milliseconds 50
    }
    throw "Timed out waiting for pose '$($Case.Name)'."
}

function Restore-NeutralPose {
    foreach ($pose in @(
        @{ type='grip'; position=$neutralGripPosition },
        @{ type='aim'; position=$neutralAimPosition })) {
        $null = Invoke-OperatorJson `
            -ToolName 'openxr_set_controller_pose' `
            -Arguments @{
                hand='right'; pose_type=$pose.type; base_space='local'
                position=$pose.position; orientation=$identity
                duration_seconds=0.0
            }
    }
}

$cases = @(
    [pscustomobject]@{ Name='neutral'; Yaw=0.0; Pitch=0.0; Roll=0.0 },
    [pscustomobject]@{ Name='yaw_left_15'; Yaw=15.0; Pitch=0.0; Roll=0.0 },
    [pscustomobject]@{ Name='yaw_right_15'; Yaw=-15.0; Pitch=0.0; Roll=0.0 },
    [pscustomobject]@{ Name='pitch_up_12'; Yaw=0.0; Pitch=12.0; Roll=0.0 },
    [pscustomobject]@{ Name='pitch_down_12'; Yaw=0.0; Pitch=-12.0; Roll=0.0 },
    [pscustomobject]@{
        Name='diagonal_up_right_roll'; Yaw=-10.0; Pitch=10.0; Roll=15.0
    })
foreach ($case in $cases) {
    $case | Add-Member NoteProperty Orientation (
        New-CombinedQuaternion $case.Yaw $case.Pitch $case.Roll)
}

$startedUtc = [DateTime]::UtcNow
$results = [Collections.Generic.List[object]]::new()
try {
    $haloProcess = Get-Process -Id $HaloPid -ErrorAction Stop
    if ($haloProcess.ProcessName -ne 'HaloCampaignEvolved') {
        throw "PID $HaloPid is '$($haloProcess.ProcessName)', not Halo."
    }
    $initial = Invoke-OperatorJson -ToolName 'UEVR_Status'
    if (-not $initial.ok -or -not $initial.openxr.session_ready -or
        -not $initial.halo_motion_controls.visual_weapon_attached) {
        throw 'The active Halo/OpenXR session is not ready for reticle capture.'
    }
    foreach ($case in $cases) {
        $before = Invoke-OperatorJson -ToolName 'UEVR_Status'
        $sample = Set-RightPose `
            -Case $case `
            -AfterSequence ([uint32](
                $before.halo_motion_controls.pose_diagnostics.sequence))
        $leftPath = Join-Path $OutputDirectory "$($case.Name)-left.png"
        $rightPath = Join-Path $OutputDirectory "$($case.Name)-right.png"
        Invoke-EyeCapture -Eye left -Path $leftPath
        Invoke-EyeCapture -Eye right -Path $rightPath

        $expectedX = $neutralRightPoint[0] -
            $horizontalFocalPixels * [Math]::Tan($case.Yaw * [Math]::PI / 180.0)
        $expectedY = $neutralRightPoint[1] -
            $verticalFocalPixels * [Math]::Tan($case.Pitch * [Math]::PI / 180.0)
        $expectedNormalized = @(
            ($expectedX / ($referenceWidth - 1.0)),
            ($expectedY / ($referenceHeight - 1.0)))
        $analysisPath = Join-Path $OutputDirectory "$($case.Name)-analysis.json"
        & py -3 $analyzer `
            --left $leftPath `
            --right $rightPath `
            --authored (Join-Path $OutputDirectory 'authored-reference.exr') `
            --expected-right $expectedNormalized[0] $expectedNormalized[1] `
            --maximum-projection-error 6.0 `
            --maximum-stereo-error 6.0 `
            --json $analysisPath | Out-Null
        $analyzerExitCode = $LASTEXITCODE
        $analysis = Get-Content -LiteralPath $analysisPath -Raw |
            ConvertFrom-Json
        $results.Add([pscustomobject][ordered]@{
            name = $case.Name
            yaw_degrees = $case.Yaw
            pitch_degrees = $case.Pitch
            roll_degrees = $case.Roll
            commanded_orientation = @($case.Orientation)
            expected_right_point_reference_pixels = @($expectedX, $expectedY)
            expected_right_point_normalized = @($expectedNormalized)
            pose_sequence = [uint32](
                $sample.Status.halo_motion_controls.pose_diagnostics.sequence)
            pose_position_error_meters = $sample.PositionErrorMeters
            pose_angle_error_degrees = $sample.AngleErrorDegrees
            pose_wait_milliseconds = $sample.WaitMilliseconds
            left_image = $leftPath
            right_image = $rightPath
            left_image_sha256 = (Get-FileHash `
                -LiteralPath $leftPath -Algorithm SHA256).Hash.ToLowerInvariant()
            right_image_sha256 = (Get-FileHash `
                -LiteralPath $rightPath -Algorithm SHA256).Hash.ToLowerInvariant()
            analysis_file = $analysisPath
            analyzer_exit_code = $analyzerExitCode
            passed = [bool]$analysis.passed
            analysis = $analysis
        })
    }
} finally {
    Restore-NeutralPose
}

$summary = [pscustomobject][ordered]@{
    schema_version = 1
    started_utc = $startedUtc.ToString('o')
    completed_utc = [DateTime]::UtcNow.ToString('o')
    halo_pid = $HaloPid
    halo_pid_verified = $true
    operator_pid_requested = 8720
    operator_pid_present_at_start = [bool](Get-Process -Id 8720 -ErrorAction SilentlyContinue)
    invoke_tool_path = $InvokeToolPath
    output_directory = $OutputDirectory
    calibration = [pscustomobject]@{
        source = (
            'Prior source-preserving 840x880 neutral/yaw +/-15/pitch +/-15 ' +
            'composited captures under final-visible-fire-20260731')
        reference_resolution = @([int]$referenceWidth, [int]$referenceHeight)
        neutral_right_point = $neutralRightPoint
        horizontal_focal_pixels = $horizontalFocalPixels
        vertical_focal_pixels = $verticalFocalPixels
        projection = 'x=cx-fx*tan(yaw); y=cy-fy*tan(pitch)'
        maximum_projection_error_pixels = 6.0
        maximum_stereo_error_pixels = 6.0
    }
    authored_reference = Join-Path $OutputDirectory 'authored-reference.exr'
    source_preservation = [pscustomobject]@{
        source_images_untouched = $true
        montage_uses_annotated_copies = $true
        per_image_sha256 = $true
    }
    session_constraints = [pscustomobject]@{
        relaunched = $false
        foreground_activation_used = $false
        shots_fired = 0
        composited_eye_capture = $true
    }
    restored_pose = [pscustomobject]@{
        grip_position = $neutralGripPosition
        aim_position = $neutralAimPosition
        grip_orientation = $identity
        aim_orientation = $identity
    }
    case_count = $results.Count
    passed_count = @($results | Where-Object passed).Count
    failed_count = @($results | Where-Object { -not $_.passed }).Count
    passed = @($results | Where-Object { -not $_.passed }).Count -eq 0
    cases = @($results)
}
$summaryPath = Join-Path $OutputDirectory 'reticle-sweep-summary.json'
$summary | ConvertTo-Json -Depth 30 |
    Set-Content -LiteralPath $summaryPath -Encoding utf8
& py -3 $montageBuilder `
    --summary $summaryPath `
    --output (Join-Path $OutputDirectory 'reticle-sweep-montage.png')
if ($LASTEXITCODE -ne 0) {
    throw "Montage builder failed with exit code $LASTEXITCODE."
}

$summary
if (-not $summary.passed) {
    exit 1
}
