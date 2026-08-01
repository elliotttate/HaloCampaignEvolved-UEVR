[CmdletBinding()]
param(
    [ValidateSet('FullNumeric', 'VisualFire', 'Both')]
    [string]$Suite = 'FullNumeric',

    [string]$OutputDirectory = '',

    [string]$OperatorPackageRoot = '',

    [string]$InvokeToolPath = '',

    [ValidateRange(1.0, 90.0)]
    [double]$RotationDegrees = 30.0,

    [ValidateRange(0.01, 0.50)]
    [double]$TranslationMeters = 0.15,

    [double[]]$NeutralGripPosition = @(0.30, -0.33, -0.46),

    [double[]]$NeutralAimPosition = @(0.30, -0.30, -0.555),

    [ValidateRange(0.1, 30.0)]
    [double]$PollTimeoutSeconds = 5.0,

    [ValidateRange(10, 1000)]
    [int]$PollIntervalMilliseconds = 50,

    [ValidateRange(0.0001, 0.10)]
    [double]$PosePositionToleranceMeters = 0.005,

    [ValidateRange(0.01, 10.0)]
    [double]$PoseAngleToleranceDegrees = 1.0,

    [ValidateRange(0.0001, 0.25)]
    [double]$WeaponPositionToleranceMeters = 0.02,

    [ValidateRange(0.5, 1.0)]
    [double]$MinimumDirectionDot = 0.98,

    [ValidateRange(0.98, 1.0)]
    [double]$MinimumBallisticDot = 0.9995,

    [ValidateRange(0.0001, 1.0)]
    [double]$MaximumMuzzleRayMissMeters = 0.01,

    [ValidateRange(0.01, 10.0)]
    [double]$MinimumTargetForwardDistanceMeters = 1.0,

    [ValidateRange(0.0, 45.0)]
    [double]$MaximumProjectileSpreadDegrees = 3.75,

    [ValidateRange(0.0001, 0.50)]
    [double]$DeterminantTolerance = 0.05,

    [ValidateSet('left', 'right')]
    [string]$ScreenshotEye = 'right',

    [ValidateRange(0.02, 2.0)]
    [double]$TriggerHoldSeconds = 0.20,

    [string[]]$CaseName = @(),

    [switch]$KeepFinalPose,

    [switch]$PlanOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($NeutralGripPosition.Count -ne 3) {
    throw 'NeutralGripPosition must contain exactly three values: x, y, z.'
}
if ($NeutralAimPosition.Count -ne 3) {
    throw 'NeutralAimPosition must contain exactly three values: x, y, z.'
}

$script:InvokeToolPathResolved = $null
$script:BlamMetersPerUnit = 3.048

function ConvertTo-DoubleVector {
    param(
        [Parameter(Mandatory)]
        $Value
    )

    if ($Value -is [System.Array]) {
        if ($Value.Count -ne 3) {
            throw 'Expected a three-component vector.'
        }
        return @(
            [double]$Value[0],
            [double]$Value[1],
            [double]$Value[2])
    }

    return @(
        [double]$Value.x,
        [double]$Value.y,
        [double]$Value.z)
}

function Test-FiniteDouble {
    param([double]$Value)
    return -not [double]::IsNaN($Value) -and
        -not [double]::IsInfinity($Value)
}

function ConvertTo-DoubleQuaternion {
    param(
        [Parameter(Mandatory)]
        $Value
    )

    if ($Value -is [System.Array]) {
        if ($Value.Count -ne 4) {
            throw 'Expected a four-component quaternion.'
        }
        $result = @(
            [double]$Value[0],
            [double]$Value[1],
            [double]$Value[2],
            [double]$Value[3])
    } else {
        $result = @(
            [double]$Value.x,
            [double]$Value.y,
            [double]$Value.z,
            [double]$Value.w)
    }

    $length = [Math]::Sqrt(
        $result[0] * $result[0] +
        $result[1] * $result[1] +
        $result[2] * $result[2] +
        $result[3] * $result[3])
    if (-not (Test-FiniteDouble $length) -or $length -lt 1.0e-12) {
        throw 'Quaternion was non-finite or had zero length.'
    }
    return @($result | ForEach-Object { $_ / $length })
}

function Add-Vector {
    param([double[]]$A, [double[]]$B)
    return @(
        ($A[0] + $B[0]),
        ($A[1] + $B[1]),
        ($A[2] + $B[2]))
}

function Subtract-Vector {
    param([double[]]$A, [double[]]$B)
    return @(
        ($A[0] - $B[0]),
        ($A[1] - $B[1]),
        ($A[2] - $B[2]))
}

function Scale-Vector {
    param([double[]]$Value, [double]$Scale)
    return @(
        ($Value[0] * $Scale),
        ($Value[1] * $Scale),
        ($Value[2] * $Scale))
}

function Get-VectorDot {
    param([double[]]$A, [double[]]$B)
    return (
        $A[0] * $B[0] +
        $A[1] * $B[1] +
        $A[2] * $B[2])
}

function Get-VectorCross {
    param([double[]]$A, [double[]]$B)
    return @(
        ($A[1] * $B[2] - $A[2] * $B[1]),
        ($A[2] * $B[0] - $A[0] * $B[2]),
        ($A[0] * $B[1] - $A[1] * $B[0]))
}

function Get-VectorLength {
    param([double[]]$Value)
    return [Math]::Sqrt((Get-VectorDot -A $Value -B $Value))
}

function Normalize-Vector {
    param([double[]]$Value)
    $length = Get-VectorLength -Value $Value
    if (-not (Test-FiniteDouble $length) -or $length -lt 1.0e-12) {
        throw 'Cannot normalize a non-finite or zero-length vector.'
    }
    return Scale-Vector -Value $Value -Scale (1.0 / $length)
}

function Get-VectorDistance {
    param([double[]]$A, [double[]]$B)
    return Get-VectorLength -Value (Subtract-Vector -A $A -B $B)
}

function Multiply-Quaternion {
    param([double[]]$Left, [double[]]$Right)
    $lx, $ly, $lz, $lw = $Left
    $rx, $ry, $rz, $rw = $Right
    return ConvertTo-DoubleQuaternion -Value @(
        ($lw * $rx + $lx * $rw + $ly * $rz - $lz * $ry),
        ($lw * $ry - $lx * $rz + $ly * $rw + $lz * $rx),
        ($lw * $rz + $lx * $ry - $ly * $rx + $lz * $rw),
        ($lw * $rw - $lx * $rx - $ly * $ry - $lz * $rz))
}

function Get-QuaternionConjugate {
    param([double[]]$Value)
    return @(-$Value[0], -$Value[1], -$Value[2], $Value[3])
}

function Rotate-Vector {
    param([double[]]$Quaternion, [double[]]$Vector)
    $q = ConvertTo-DoubleQuaternion -Value $Quaternion
    $u = @($q[0], $q[1], $q[2])
    $s = $q[3]
    $dotUv = Get-VectorDot -A $u -B $Vector
    $dotUu = Get-VectorDot -A $u -B $u
    $cross = Get-VectorCross -A $u -B $Vector
    return Add-Vector `
        -A (Add-Vector `
            -A (Scale-Vector -Value $u -Scale (2.0 * $dotUv)) `
            -B (Scale-Vector `
                -Value $Vector `
                -Scale ($s * $s - $dotUu))) `
        -B (Scale-Vector -Value $cross -Scale (2.0 * $s))
}

function New-AxisQuaternion {
    param(
        [ValidateSet('Yaw', 'Pitch', 'Roll')]
        [string]$Axis,
        [double]$Degrees
    )

    $halfRadians = $Degrees * [Math]::PI / 360.0
    $sine = [Math]::Sin($halfRadians)
    $cosine = [Math]::Cos($halfRadians)
    switch ($Axis) {
        'Yaw' { return @(0.0, $sine, 0.0, $cosine) }
        'Pitch' { return @($sine, 0.0, 0.0, $cosine) }
        'Roll' { return @(0.0, 0.0, $sine, $cosine) }
    }
}

function Get-QuaternionAbsDot {
    param([double[]]$A, [double[]]$B)
    $dot =
        $A[0] * $B[0] +
        $A[1] * $B[1] +
        $A[2] * $B[2] +
        $A[3] * $B[3]
    return [Math]::Min(1.0, [Math]::Abs($dot))
}

function Get-QuaternionAngleDegrees {
    param([double[]]$A, [double[]]$B)
    return (
        2.0 *
        [Math]::Acos((Get-QuaternionAbsDot -A $A -B $B)) *
        180.0 /
        [Math]::PI)
}

function Convert-OpenXRVectorToBlam {
    param([double[]]$Value)
    return @(-$Value[2], -$Value[0], $Value[1])
}

function Get-MatrixBasis {
    param($Matrix)
    return [pscustomobject]@{
        Forward = ConvertTo-DoubleVector -Value $Matrix.forward
        Left = ConvertTo-DoubleVector -Value $Matrix.left
        Up = ConvertTo-DoubleVector -Value $Matrix.up
    }
}

function Transform-BasisVector {
    param($Basis, [double[]]$Value)
    return Add-Vector `
        -A (Add-Vector `
            -A (Scale-Vector -Value $Basis.Forward -Scale $Value[0]) `
            -B (Scale-Vector -Value $Basis.Left -Scale $Value[1])) `
        -B (Scale-Vector -Value $Basis.Up -Scale $Value[2])
}

function Get-BasisDeterminant {
    param($Basis)
    return Get-VectorDot `
        -A $Basis.Forward `
        -B (Get-VectorCross -A $Basis.Left -B $Basis.Up)
}

function Get-BasisMaximumOrthogonalityError {
    param($Basis)
    return @(
        [Math]::Abs((Get-VectorDot -A $Basis.Forward -B $Basis.Left)),
        [Math]::Abs((Get-VectorDot -A $Basis.Forward -B $Basis.Up)),
        [Math]::Abs((Get-VectorDot -A $Basis.Left -B $Basis.Up)),
        [Math]::Abs((Get-VectorLength -Value $Basis.Forward) - 1.0),
        [Math]::Abs((Get-VectorLength -Value $Basis.Left) - 1.0),
        [Math]::Abs((Get-VectorLength -Value $Basis.Up) - 1.0)
    ) | Measure-Object -Maximum | Select-Object -ExpandProperty Maximum
}

function Resolve-PackagedInvokeTool {
    if ($InvokeToolPath) {
        return (Resolve-Path -LiteralPath $InvokeToolPath).Path
    }

    if ($OperatorPackageRoot) {
        $explicit = Join-Path `
            $OperatorPackageRoot `
            'Invoke-MetaXROperatorTool.ps1'
        return (Resolve-Path -LiteralPath $explicit).Path
    }

    $releaseRoot = 'E:\Github\UEVRMetaXROperator\dist\release'
    if (-not (Test-Path -LiteralPath $releaseRoot -PathType Container)) {
        throw (
            'Could not find the packaged Meta XR Operator release root at ' +
            "'$releaseRoot'. Pass -OperatorPackageRoot or -InvokeToolPath.")
    }

    $candidate = Get-ChildItem -LiteralPath $releaseRoot -Directory |
        Where-Object {
            (Test-Path -LiteralPath (
                Join-Path $_.FullName 'Invoke-MetaXROperatorTool.ps1') `
                -PathType Leaf) -and
            (Test-Path -LiteralPath (
                Join-Path $_.FullName (
                    'meta-xr-operator\windows\' +
                    'meta-xr-operator-mcp-proxy.exe')) `
                -PathType Leaf)
        } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -eq $candidate) {
        throw (
            "No complete packaged Operator build was found under '$releaseRoot'.")
    }
    return Join-Path $candidate.FullName 'Invoke-MetaXROperatorTool.ps1'
}

function Invoke-OperatorJson {
    param(
        [Parameter(Mandatory)]
        [string]$ToolName,
        [hashtable]$Arguments = @{}
    )

    $response = & $script:InvokeToolPathResolved `
        -ToolName $ToolName `
        -ArgumentsJson ($Arguments | ConvertTo-Json -Depth 20 -Compress)
    if ($null -eq $response -or $null -eq $response.result) {
        throw "Operator tool '$ToolName' returned no MCP result."
    }
    if ($response.result.PSObject.Properties['isError'] -and
        [bool]$response.result.isError) {
        throw "Operator tool '$ToolName' returned an MCP error."
    }

    $text = @(
        $response.result.content |
            Where-Object { $_.type -eq 'text' } |
            Select-Object -First 1
    ).text
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

function Invoke-OperatorScreenshot {
    param(
        [Parameter(Mandatory)]
        [string]$Path
    )

    $null = & $script:InvokeToolPathResolved `
        -ToolName 'openxr_capture_composited_image' `
        -ArgumentsJson (@{
            eye = $ScreenshotEye
        } | ConvertTo-Json -Compress) `
        -OutputImage $Path
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Operator did not write screenshot '$Path'."
    }
}

function Get-UEVRStatus {
    return Invoke-OperatorJson -ToolName 'UEVR_Status'
}

function Get-PoseDiagnostics {
    param($Status)
    if ($null -eq $Status.halo_motion_controls -or
        $null -eq $Status.halo_motion_controls.pose_diagnostics -or
        -not [bool]$Status.halo_motion_controls.pose_diagnostics.available) {
        throw 'UEVR_Status.pose_diagnostics is unavailable.'
    }
    return $Status.halo_motion_controls.pose_diagnostics
}

function Assert-ReadyStatus {
    param($Status, [bool]$RequireFire)

    $failures = [System.Collections.Generic.List[string]]::new()
    if (-not [bool]$Status.openxr.session_ready) {
        $failures.Add('OpenXR session is not ready.')
    }
    if (-not [bool]$Status.openxr.operator_extension_enabled) {
        $failures.Add('Meta XR Operator extension is not enabled.')
    }
    $halo = $Status.halo_motion_controls
    if (-not [bool]$halo.loaded) {
        $failures.Add('HaloCEMotionControls.dll is not loaded.')
    }
    if (-not [bool]$halo.status_api) {
        $failures.Add('HaloCEMotionControls status API is unavailable.')
    }
    if (-not [bool]$halo.tracking_valid) {
        $failures.Add('Halo controller tracking is not valid.')
    }
    if (-not [bool]$halo.native_visual_hook_installed) {
        $failures.Add('Halo native visual hook is not installed.')
    }
    if (-not [bool]$halo.visual_weapon_attached) {
        $failures.Add('Halo first-person weapon is not attached.')
    }
    if ($RequireFire -and
        -not [bool]$halo.native_projectile_hook_installed) {
        $failures.Add('Halo native projectile hook is not installed.')
    }
    try {
        $null = Get-PoseDiagnostics -Status $Status
    } catch {
        $failures.Add($_.Exception.Message)
    }
    if ($failures.Count -gt 0) {
        throw "Runtime readiness check failed:`n - $($failures -join "`n - ")"
    }
}

function New-PoseCase {
    param(
        [string]$Name,
        [string]$ChangedPose,
        [double[]]$GripPosition,
        [double[]]$GripOrientation,
        [double[]]$AimPosition,
        [double[]]$AimOrientation,
        [bool]$CaptureAndFire = $false
    )

    return [pscustomobject]@{
        Name = $Name
        ChangedPose = $ChangedPose
        GripPosition = @($GripPosition)
        GripOrientation = @(ConvertTo-DoubleQuaternion $GripOrientation)
        AimPosition = @($AimPosition)
        AimOrientation = @(ConvertTo-DoubleQuaternion $AimOrientation)
        CaptureAndFire = $CaptureAndFire
    }
}

function New-OffsetPosition {
    param([double[]]$Base, [int]$Axis, [double]$Amount)
    $result = @($Base[0], $Base[1], $Base[2])
    $result[$Axis] += $Amount
    return $result
}

function Get-FullNumericCases {
    $identity = @(0.0, 0.0, 0.0, 1.0)
    $cases = [System.Collections.Generic.List[object]]::new()
    $cases.Add((New-PoseCase `
        -Name 'neutral' `
        -ChangedPose 'none' `
        -GripPosition $NeutralGripPosition `
        -GripOrientation $identity `
        -AimPosition $NeutralAimPosition `
        -AimOrientation $identity))

    foreach ($poseName in @('grip', 'aim')) {
        foreach ($axis in @(
            @{ Name = 'x'; Index = 0 },
            @{ Name = 'y'; Index = 1 },
            @{ Name = 'z'; Index = 2 })) {
            foreach ($direction in @(
                @{ Name = 'pos'; Sign = 1.0 },
                @{ Name = 'neg'; Sign = -1.0 })) {
                $gripPosition = @($NeutralGripPosition)
                $aimPosition = @($NeutralAimPosition)
                if ($poseName -eq 'grip') {
                    $gripPosition = New-OffsetPosition `
                        -Base $NeutralGripPosition `
                        -Axis $axis.Index `
                        -Amount ($TranslationMeters * $direction.Sign)
                } else {
                    $aimPosition = New-OffsetPosition `
                        -Base $NeutralAimPosition `
                        -Axis $axis.Index `
                        -Amount ($TranslationMeters * $direction.Sign)
                }
                $cases.Add((New-PoseCase `
                    -Name "$($poseName)_translate_$($axis.Name)_$($direction.Name)" `
                    -ChangedPose $poseName `
                    -GripPosition $gripPosition `
                    -GripOrientation $identity `
                    -AimPosition $aimPosition `
                    -AimOrientation $identity))
            }
        }

        foreach ($rotationAxis in @('Yaw', 'Pitch', 'Roll')) {
            foreach ($direction in @(
                @{ Name = 'pos'; Sign = 1.0 },
                @{ Name = 'neg'; Sign = -1.0 })) {
                $gripOrientation = $identity
                $aimOrientation = $identity
                $orientation = New-AxisQuaternion `
                    -Axis $rotationAxis `
                    -Degrees ($RotationDegrees * $direction.Sign)
                if ($poseName -eq 'grip') {
                    $gripOrientation = $orientation
                } else {
                    $aimOrientation = $orientation
                }
                $cases.Add((New-PoseCase `
                    -Name (
                        "$($poseName)_$($rotationAxis.ToLower())_" +
                        $direction.Name) `
                    -ChangedPose $poseName `
                    -GripPosition $NeutralGripPosition `
                    -GripOrientation $gripOrientation `
                    -AimPosition $NeutralAimPosition `
                    -AimOrientation $aimOrientation))
            }
        }
    }
    return $cases
}

function Get-VisualFireCases {
    $wanted = @(
        'neutral',
        'aim_yaw_pos',
        'aim_yaw_neg',
        'aim_pitch_pos',
        'aim_pitch_neg',
        'aim_roll_pos',
        'grip_translate_x_pos')
    $fullByName = @{}
    foreach ($case in (Get-FullNumericCases)) {
        $fullByName[$case.Name] = $case
    }
    return @(
        foreach ($name in $wanted) {
            $source = $fullByName[$name]
            New-PoseCase `
                -Name $source.Name `
                -ChangedPose $source.ChangedPose `
                -GripPosition $source.GripPosition `
                -GripOrientation $source.GripOrientation `
                -AimPosition $source.AimPosition `
                -AimOrientation $source.AimOrientation `
                -CaptureAndFire $true
        })
}

function Set-RightControllerCase {
    param($Case)

    $neutralGrip = @{
        hand = 'right'
        pose_type = 'grip'
        base_space = 'local'
        position = @($Case.GripPosition)
        orientation = @($Case.GripOrientation)
        duration_seconds = 0.0
    }
    $neutralAim = @{
        hand = 'right'
        pose_type = 'aim'
        base_space = 'local'
        position = @($Case.AimPosition)
        orientation = @($Case.AimOrientation)
        duration_seconds = 0.0
    }

    # Apply the unchanged pose first and the independently varied pose last.
    if ($Case.ChangedPose -eq 'aim') {
        $null = Invoke-OperatorJson `
            -ToolName 'openxr_set_controller_pose' `
            -Arguments $neutralGrip
        $null = Invoke-OperatorJson `
            -ToolName 'openxr_set_controller_pose' `
            -Arguments $neutralAim
    } else {
        $null = Invoke-OperatorJson `
            -ToolName 'openxr_set_controller_pose' `
            -Arguments $neutralAim
        $null = Invoke-OperatorJson `
            -ToolName 'openxr_set_controller_pose' `
            -Arguments $neutralGrip
    }
}

function Get-PoseEchoErrors {
    param($Case, $Diagnostics)

    $actualGripPosition =
        ConvertTo-DoubleVector -Value $Diagnostics.right_grip.position
    $actualAimPosition =
        ConvertTo-DoubleVector -Value $Diagnostics.right_aim.position
    $actualGripRotation =
        ConvertTo-DoubleQuaternion -Value $Diagnostics.right_grip.rotation
    $actualAimRotation =
        ConvertTo-DoubleQuaternion -Value $Diagnostics.right_aim.rotation

    return [pscustomobject]@{
        GripPositionErrorMeters = Get-VectorDistance `
            -A $Case.GripPosition `
            -B $actualGripPosition
        AimPositionErrorMeters = Get-VectorDistance `
            -A $Case.AimPosition `
            -B $actualAimPosition
        GripQuaternionAbsDot = Get-QuaternionAbsDot `
            -A $Case.GripOrientation `
            -B $actualGripRotation
        AimQuaternionAbsDot = Get-QuaternionAbsDot `
            -A $Case.AimOrientation `
            -B $actualAimRotation
        GripAngleErrorDegrees = Get-QuaternionAngleDegrees `
            -A $Case.GripOrientation `
            -B $actualGripRotation
        AimAngleErrorDegrees = Get-QuaternionAngleDegrees `
            -A $Case.AimOrientation `
            -B $actualAimRotation
    }
}

function Wait-ForPoseSample {
    param(
        $Case,
        [uint32]$AfterSequence,
        [uint32]$AfterVisualCount
    )

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $pollCount = 0
    $lastStatus = $null
    while ($stopwatch.Elapsed.TotalSeconds -lt $PollTimeoutSeconds) {
        $pollCount++
        $lastStatus = Get-UEVRStatus
        $diagnostics = Get-PoseDiagnostics -Status $lastStatus
        $echo = Get-PoseEchoErrors -Case $Case -Diagnostics $diagnostics
        if ([uint32]$diagnostics.sequence -gt $AfterSequence -and
            [uint32]$diagnostics.visual_override_count -gt
                $AfterVisualCount -and
            $echo.GripPositionErrorMeters -le
                $PosePositionToleranceMeters -and
            $echo.AimPositionErrorMeters -le
                $PosePositionToleranceMeters -and
            $echo.GripAngleErrorDegrees -le
                $PoseAngleToleranceDegrees -and
            $echo.AimAngleErrorDegrees -le
                $PoseAngleToleranceDegrees) {
            $stopwatch.Stop()
            return [pscustomobject]@{
                Status = $lastStatus
                Diagnostics = $diagnostics
                Echo = $echo
                PollCount = $pollCount
                WaitMilliseconds = $stopwatch.Elapsed.TotalMilliseconds
            }
        }
        Start-Sleep -Milliseconds $PollIntervalMilliseconds
    }

    $stopwatch.Stop()
    $lastSequence = if ($null -ne $lastStatus) {
        (Get-PoseDiagnostics -Status $lastStatus).sequence
    } else {
        'none'
    }
    throw (
        "Timed out waiting for pose '$($Case.Name)' after sequence " +
        "$AfterSequence and visual count $AfterVisualCount; last sequence " +
        "was $lastSequence.")
}

function Wait-ForShotSample {
    param(
        [uint32]$AfterSequence,
        [uint32]$AfterMarkerCount,
        [uint32]$AfterProjectileCount
    )

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $pollCount = 0
    $lastStatus = $null
    while ($stopwatch.Elapsed.TotalSeconds -lt $PollTimeoutSeconds) {
        $pollCount++
        $lastStatus = Get-UEVRStatus
        $diagnostics = Get-PoseDiagnostics -Status $lastStatus
        $markerDelta =
            [uint32]$diagnostics.marker_override_count - $AfterMarkerCount
        $projectileDelta =
            [uint32]$diagnostics.projectile_override_count -
                $AfterProjectileCount
        if ($markerDelta -gt 1 -or $projectileDelta -gt 1) {
            throw (
                'Shot association became ambiguous: the single-shot test ' +
                "observed marker/projectile deltas $markerDelta/" +
                "$projectileDelta instead of 1/1.")
        }
        if ([bool]$diagnostics.marker_valid -and
            [bool]$diagnostics.projectile_valid -and
            [uint32]$diagnostics.sequence -gt $AfterSequence -and
            $markerDelta -eq 1 -and
            $projectileDelta -eq 1) {
            $stopwatch.Stop()
            return [pscustomobject]@{
                Status = $lastStatus
                Diagnostics = $diagnostics
                PollCount = $pollCount
                WaitMilliseconds = $stopwatch.Elapsed.TotalMilliseconds
            }
        }
        Start-Sleep -Milliseconds $PollIntervalMilliseconds
    }

    $stopwatch.Stop()
    $lastMarker = 'none'
    $lastProjectile = 'none'
    if ($null -ne $lastStatus) {
        $last = Get-PoseDiagnostics -Status $lastStatus
        $lastMarker = $last.marker_override_count
        $lastProjectile = $last.projectile_override_count
    }
    throw (
        'Timed out waiting for one unambiguous native shot and a newer visual ' +
        "sample; expected sequence above $AfterSequence and marker/projectile " +
        "counts $($AfterMarkerCount + 1)/$($AfterProjectileCount + 1), " +
        "observed $lastMarker/$lastProjectile.")
}

function Get-NumericMetrics {
    param($Case, $Sample)

    $diagnostics = $Sample.Diagnostics
    $echo = $Sample.Echo
    $hmdPosition = ConvertTo-DoubleVector -Value $diagnostics.hmd.position
    $hmdRotation =
        ConvertTo-DoubleQuaternion -Value $diagnostics.hmd.rotation
    $gripPosition =
        ConvertTo-DoubleVector -Value $diagnostics.right_grip.position
    $aimRotation =
        ConvertTo-DoubleQuaternion -Value $diagnostics.right_aim.rotation
    $rootPosition = ConvertTo-DoubleVector -Value $diagnostics.root.position
    $rootBasis = Get-MatrixBasis -Matrix $diagnostics.root
    $weaponPosition =
        ConvertTo-DoubleVector -Value $diagnostics.weapon.position
    $weaponBasis = Get-MatrixBasis -Matrix $diagnostics.weapon
    $wristBasis = Get-MatrixBasis -Matrix $diagnostics.right_wrist
    $rightHandForward =
        ConvertTo-DoubleVector -Value $diagnostics.right_hand_forward

    $inverseHmd = Get-QuaternionConjugate -Value $hmdRotation
    $relativeAim = Multiply-Quaternion `
        -Left $inverseHmd `
        -Right $aimRotation
    $relativeGrip = Rotate-Vector `
        -Quaternion $inverseHmd `
        -Vector (Subtract-Vector -A $gripPosition -B $hmdPosition)

    $controllerForward = Convert-OpenXRVectorToBlam (
        Rotate-Vector -Quaternion $relativeAim -Vector @(0.0, 0.0, -1.0))
    $controllerLeft = Convert-OpenXRVectorToBlam (
        Rotate-Vector -Quaternion $relativeAim -Vector @(-1.0, 0.0, 0.0))
    $controllerUp = Convert-OpenXRVectorToBlam (
        Rotate-Vector -Quaternion $relativeAim -Vector @(0.0, 1.0, 0.0))

    $expectedAimWorld = Normalize-Vector (
        Transform-BasisVector -Basis $rootBasis -Value $controllerForward)
    $expectedWeaponForward = Normalize-Vector (
        Transform-BasisVector `
            -Basis $rootBasis `
            -Value (Scale-Vector -Value $controllerLeft -Scale -1.0))
    $expectedWeaponLeft = $expectedAimWorld
    $expectedWeaponUp = Normalize-Vector (
        Transform-BasisVector -Basis $rootBasis -Value $controllerUp)
    $gripDeltaBlam = Scale-Vector `
        -Value (Convert-OpenXRVectorToBlam $relativeGrip) `
        -Scale (1.0 / $script:BlamMetersPerUnit)
    $expectedWeaponPosition = Add-Vector `
        -A $rootPosition `
        -B (Transform-BasisVector -Basis $rootBasis -Value $gripDeltaBlam)

    $weaponDeterminant = Get-BasisDeterminant -Basis $weaponBasis
    $wristDeterminant = Get-BasisDeterminant -Basis $wristBasis
    return [pscustomobject]@{
        GripPositionErrorMeters = $echo.GripPositionErrorMeters
        AimPositionErrorMeters = $echo.AimPositionErrorMeters
        GripQuaternionAbsDot = $echo.GripQuaternionAbsDot
        AimQuaternionAbsDot = $echo.AimQuaternionAbsDot
        GripAngleErrorDegrees = $echo.GripAngleErrorDegrees
        AimAngleErrorDegrees = $echo.AimAngleErrorDegrees
        WeaponPositionErrorMeters = (
            (Get-VectorDistance `
                -A $weaponPosition `
                -B $expectedWeaponPosition) *
            $script:BlamMetersPerUnit)
        WeaponForwardExpectedDot = Get-VectorDot `
            -A (Normalize-Vector $weaponBasis.Forward) `
            -B $expectedWeaponForward
        WeaponBarrelAimDot = Get-VectorDot `
            -A (Normalize-Vector $weaponBasis.Left) `
            -B $expectedWeaponLeft
        WeaponUpExpectedDot = Get-VectorDot `
            -A (Normalize-Vector $weaponBasis.Up) `
            -B $expectedWeaponUp
        WeaponDeterminant = $weaponDeterminant
        WeaponDeterminantError = [Math]::Abs($weaponDeterminant - 1.0)
        WeaponOrthogonalityMaxError =
            Get-BasisMaximumOrthogonalityError -Basis $weaponBasis
        RightWristDeterminant = $wristDeterminant
        RightWristDeterminantError = [Math]::Abs($wristDeterminant - 1.0)
        RightWristOrthogonalityMaxError =
            Get-BasisMaximumOrthogonalityError -Basis $wristBasis
        RightHandForwardWeaponBarrelDot = Get-VectorDot `
            -A (Normalize-Vector $rightHandForward) `
            -B (Normalize-Vector $weaponBasis.Left)
        ExpectedAimWorld = $expectedAimWorld
        ExpectedWeaponPosition = $expectedWeaponPosition
    }
}

function Get-ShotMetrics {
    param($Case, $Sample, $BaselineDiagnostics)

    $Diagnostics = $Sample.Diagnostics
    $shotEcho = Get-PoseEchoErrors -Case $Case -Diagnostics $Diagnostics
    $shotNumeric = Get-NumericMetrics `
        -Case $Case `
        -Sample ([pscustomobject]@{
            Diagnostics = $Diagnostics
            Echo = $shotEcho
        })
    $muzzlePosition =
        ConvertTo-DoubleVector -Value $Diagnostics.muzzle_marker.position
    $muzzleBasis = Get-MatrixBasis -Matrix $Diagnostics.muzzle_marker
    $reticlePosition =
        ConvertTo-DoubleVector -Value $Diagnostics.reticle_position
    $projectilePosition =
        ConvertTo-DoubleVector -Value $Diagnostics.projectile_position
    $projectileForward = Normalize-Vector (
        ConvertTo-DoubleVector -Value $Diagnostics.projectile_forward)
    $projectileUp = Normalize-Vector (
        ConvertTo-DoubleVector -Value $Diagnostics.projectile_up)
    $muzzleToTarget = Normalize-Vector (
        Subtract-Vector -A $reticlePosition -B $muzzlePosition)
    $projectileToTarget = Normalize-Vector (
        Subtract-Vector -A $reticlePosition -B $projectilePosition)
    $controllerToTargetVector = Subtract-Vector `
        -A $reticlePosition `
        -B $shotNumeric.ExpectedWeaponPosition
    $controllerToTarget = Normalize-Vector $controllerToTargetVector
    $muzzleToTargetVector = Subtract-Vector `
        -A $reticlePosition `
        -B $muzzlePosition
    $projectileToTargetVector = Subtract-Vector `
        -A $reticlePosition `
        -B $projectilePosition
    $muzzleDeterminant = Get-BasisDeterminant -Basis $muzzleBasis
    $muzzleTargetDot = Get-VectorDot `
        -A (Normalize-Vector $muzzleBasis.Forward) `
        -B $muzzleToTarget
    $projectileTargetDot = Get-VectorDot `
        -A $projectileForward `
        -B $projectileToTarget
    $projectileMuzzleForwardDot = Get-VectorDot `
        -A $projectileForward `
        -B (Normalize-Vector $muzzleBasis.Forward)
    $dotToDegrees = {
        param([double]$Dot)
        return [Math]::Acos([Math]::Clamp($Dot, -1.0, 1.0)) *
            180.0 / [Math]::PI
    }
    $rayMissMeters = {
        param([double[]]$ToTarget, [double]$DirectionDot)
        $sinSquared = [Math]::Max(
            0.0,
            1.0 - [Math]::Pow(
                [Math]::Clamp($DirectionDot, -1.0, 1.0),
                2.0))
        return (Get-VectorLength $ToTarget) *
            [Math]::Sqrt($sinSquared) * $script:BlamMetersPerUnit
    }
    # This is intentionally retained as a diagnostic only. The pose/root data
    # are first-person viewmodel-relative while reticle_position is native Blam
    # world space, so neither its direction nor distance may be release-gated.
    $crossSpaceReticleAimDot = Get-VectorDot `
        -A $controllerToTarget `
        -B $shotNumeric.ExpectedAimWorld
    $crossSpaceControllerTargetDistanceMeters =
        (Get-VectorLength $controllerToTargetVector) *
        $script:BlamMetersPerUnit
    $muzzleTargetDistanceMeters =
        (Get-VectorLength $muzzleToTargetVector) *
        $script:BlamMetersPerUnit
    $projectileTargetDistanceMeters =
        (Get-VectorLength $projectileToTargetVector) *
        $script:BlamMetersPerUnit

    return [pscustomobject]@{
        # These controller/weapon values are recalculated from the exact bridge
        # object returned with the first unambiguous post-trigger sample. The
        # native ABI does not yet event-stamp these visual fields to the shot.
        ShotSampleGripPositionErrorMeters =
            $shotNumeric.GripPositionErrorMeters
        ShotSampleAimPositionErrorMeters =
            $shotNumeric.AimPositionErrorMeters
        ShotSampleGripAngleErrorDegrees =
            $shotNumeric.GripAngleErrorDegrees
        ShotSampleAimAngleErrorDegrees =
            $shotNumeric.AimAngleErrorDegrees
        ShotSampleWeaponPositionErrorMeters =
            $shotNumeric.WeaponPositionErrorMeters
        ShotSampleWeaponForwardExpectedDot =
            $shotNumeric.WeaponForwardExpectedDot
        ShotSampleWeaponBarrelAimDot = $shotNumeric.WeaponBarrelAimDot
        ShotSampleWeaponUpExpectedDot = $shotNumeric.WeaponUpExpectedDot
        ShotSampleWeaponDeterminantError =
            $shotNumeric.WeaponDeterminantError
        ShotSampleWeaponOrthogonalityMaxError =
            $shotNumeric.WeaponOrthogonalityMaxError
        ShotSampleRightWristDeterminantError =
            $shotNumeric.RightWristDeterminantError
        ShotSampleRightWristOrthogonalityMaxError =
            $shotNumeric.RightWristOrthogonalityMaxError
        ShotSampleRightHandForwardWeaponBarrelDot =
            $shotNumeric.RightHandForwardWeaponBarrelDot
        ShotSampleSequenceDelta =
            ([uint32]$Diagnostics.sequence -
                [uint32]$BaselineDiagnostics.sequence)
        ShotSampleMarkerCountDelta =
            ([uint32]$Diagnostics.marker_override_count -
                [uint32]$BaselineDiagnostics.marker_override_count)
        ShotSampleProjectileCountDelta =
            ([uint32]$Diagnostics.projectile_override_count -
                [uint32]$BaselineDiagnostics.projectile_override_count)
        ControllerReticleComparisonAvailable = $false
        ControllerReticleComparisonReason = (
            'Controller/weapon diagnostics are first-person ' +
            'viewmodel-relative; reticle, muzzle, and projectile diagnostics ' +
            'are native Blam world space. The v1 ABI exposes no shot-latched ' +
            'native controller ray or common event id.')
        CrossSpaceReticleAimDotDiagnostic = $crossSpaceReticleAimDot
        CrossSpaceReticleAimAngleDegreesDiagnostic =
            & $dotToDegrees $crossSpaceReticleAimDot
        CrossSpaceReticleOriginDistanceMetersDiagnostic =
            $crossSpaceControllerTargetDistanceMeters
        MuzzleTargetDot = $muzzleTargetDot
        MuzzleTargetAngleDegrees = & $dotToDegrees $muzzleTargetDot
        MuzzleTargetDistanceMeters = $muzzleTargetDistanceMeters
        MuzzleTargetForwardDistanceMeters =
            $muzzleTargetDistanceMeters * $muzzleTargetDot
        MuzzleTargetRayMissMeters =
            & $rayMissMeters $muzzleToTargetVector $muzzleTargetDot
        ProjectileTargetDot = $projectileTargetDot
        ProjectileTargetAngleDegrees = & $dotToDegrees $projectileTargetDot
        ProjectileTargetDistanceMeters = $projectileTargetDistanceMeters
        ProjectileTargetForwardDistanceMeters =
            $projectileTargetDistanceMeters * $projectileTargetDot
        ProjectileTargetRayMissMeters =
            & $rayMissMeters $projectileToTargetVector $projectileTargetDot
        ProjectileTargetRayMissLimitMeters =
            $projectileTargetDistanceMeters *
            [Math]::Sin(
                $MaximumProjectileSpreadDegrees * [Math]::PI / 180.0)
        ProjectileAimDot = Get-VectorDot `
            -A $projectileForward `
            -B $shotNumeric.ExpectedAimWorld
        ProjectileMuzzleForwardDot = $projectileMuzzleForwardDot
        ProjectileMuzzleForwardAngleDegrees =
            & $dotToDegrees $projectileMuzzleForwardDot
        ProjectileMuzzlePositionErrorMeters = (
            (Get-VectorDistance -A $projectilePosition -B $muzzlePosition) *
            $script:BlamMetersPerUnit)
        ProjectileForwardUpAbsDot = [Math]::Abs(
            (Get-VectorDot -A $projectileForward -B $projectileUp))
        MuzzleDeterminant = $muzzleDeterminant
        MuzzleDeterminantError = [Math]::Abs($muzzleDeterminant - 1.0)
        MuzzleOrthogonalityMaxError =
            Get-BasisMaximumOrthogonalityError -Basis $muzzleBasis
    }
}

function Test-NumericMetrics {
    param($Metrics)
    return (
        $Metrics.GripPositionErrorMeters -le
            $PosePositionToleranceMeters -and
        $Metrics.AimPositionErrorMeters -le
            $PosePositionToleranceMeters -and
        $Metrics.GripAngleErrorDegrees -le
            $PoseAngleToleranceDegrees -and
        $Metrics.AimAngleErrorDegrees -le
            $PoseAngleToleranceDegrees -and
        $Metrics.WeaponPositionErrorMeters -le
            $WeaponPositionToleranceMeters -and
        $Metrics.WeaponForwardExpectedDot -ge $MinimumDirectionDot -and
        $Metrics.WeaponBarrelAimDot -ge $MinimumDirectionDot -and
        $Metrics.WeaponUpExpectedDot -ge $MinimumDirectionDot -and
        $Metrics.WeaponDeterminantError -le $DeterminantTolerance -and
        $Metrics.WeaponOrthogonalityMaxError -le $DeterminantTolerance -and
        $Metrics.RightWristDeterminantError -le $DeterminantTolerance -and
        $Metrics.RightWristOrthogonalityMaxError -le
            $DeterminantTolerance -and
        $Metrics.RightHandForwardWeaponBarrelDot -ge $MinimumDirectionDot)
}

function Test-ShotMetrics {
    param($Metrics)
    return (
        $Metrics.ShotSampleSequenceDelta -gt 0 -and
        $Metrics.ShotSampleMarkerCountDelta -eq 1 -and
        $Metrics.ShotSampleProjectileCountDelta -eq 1 -and
        $Metrics.ShotSampleGripPositionErrorMeters -le
            $PosePositionToleranceMeters -and
        $Metrics.ShotSampleAimPositionErrorMeters -le
            $PosePositionToleranceMeters -and
        $Metrics.ShotSampleGripAngleErrorDegrees -le
            $PoseAngleToleranceDegrees -and
        $Metrics.ShotSampleAimAngleErrorDegrees -le
            $PoseAngleToleranceDegrees -and
        $Metrics.ShotSampleWeaponPositionErrorMeters -le
            $WeaponPositionToleranceMeters -and
        $Metrics.ShotSampleWeaponForwardExpectedDot -ge
            $MinimumDirectionDot -and
        $Metrics.ShotSampleWeaponBarrelAimDot -ge $MinimumDirectionDot -and
        $Metrics.ShotSampleWeaponUpExpectedDot -ge $MinimumDirectionDot -and
        $Metrics.ShotSampleWeaponDeterminantError -le
            $DeterminantTolerance -and
        $Metrics.ShotSampleWeaponOrthogonalityMaxError -le
            $DeterminantTolerance -and
        $Metrics.ShotSampleRightWristDeterminantError -le
            $DeterminantTolerance -and
        $Metrics.ShotSampleRightWristOrthogonalityMaxError -le
            $DeterminantTolerance -and
        $Metrics.ShotSampleRightHandForwardWeaponBarrelDot -ge
            $MinimumDirectionDot -and
        $Metrics.MuzzleTargetDot -ge $MinimumBallisticDot -and
        $Metrics.MuzzleTargetForwardDistanceMeters -ge
            $MinimumTargetForwardDistanceMeters -and
        $Metrics.MuzzleTargetRayMissMeters -le
            $MaximumMuzzleRayMissMeters -and
        $Metrics.ProjectileTargetAngleDegrees -le
            $MaximumProjectileSpreadDegrees -and
        $Metrics.ProjectileTargetForwardDistanceMeters -ge
            $MinimumTargetForwardDistanceMeters -and
        $Metrics.ProjectileTargetRayMissMeters -le
            $Metrics.ProjectileTargetRayMissLimitMeters -and
        $Metrics.ProjectileForwardUpAbsDot -le $DeterminantTolerance -and
        $Metrics.MuzzleDeterminantError -le $DeterminantTolerance -and
        $Metrics.MuzzleOrthogonalityMaxError -le $DeterminantTolerance)
}

function Convert-CaseResultToCsvRow {
    param($Result)
    $numeric = $Result.NumericMetrics
    $shot = $Result.ShotMetrics
    return [pscustomobject][ordered]@{
        suite = $Result.Suite
        case = $Result.Name
        changed_pose = $Result.ChangedPose
        passed = $Result.Passed
        error = $Result.Error
        sequence_before = $Result.SequenceBefore
        sequence_after = $Result.SequenceAfter
        visual_count_before = $Result.VisualCountBefore
        visual_count_after = $Result.VisualCountAfter
        marker_count_before = $Result.MarkerCountBefore
        marker_count_after = $Result.MarkerCountAfter
        projectile_count_before = $Result.ProjectileCountBefore
        projectile_count_after = $Result.ProjectileCountAfter
        poll_count = $Result.PollCount
        wait_ms = $Result.WaitMilliseconds
        screenshot = $Result.Screenshot
        grip_position_error_m = $numeric.GripPositionErrorMeters
        aim_position_error_m = $numeric.AimPositionErrorMeters
        grip_quaternion_abs_dot = $numeric.GripQuaternionAbsDot
        aim_quaternion_abs_dot = $numeric.AimQuaternionAbsDot
        grip_angle_error_deg = $numeric.GripAngleErrorDegrees
        aim_angle_error_deg = $numeric.AimAngleErrorDegrees
        weapon_position_error_m = $numeric.WeaponPositionErrorMeters
        weapon_forward_expected_dot = $numeric.WeaponForwardExpectedDot
        weapon_barrel_aim_dot = $numeric.WeaponBarrelAimDot
        weapon_up_expected_dot = $numeric.WeaponUpExpectedDot
        weapon_determinant = $numeric.WeaponDeterminant
        weapon_determinant_error = $numeric.WeaponDeterminantError
        weapon_orthogonality_max_error =
            $numeric.WeaponOrthogonalityMaxError
        right_wrist_determinant = $numeric.RightWristDeterminant
        right_wrist_determinant_error =
            $numeric.RightWristDeterminantError
        right_wrist_orthogonality_max_error =
            $numeric.RightWristOrthogonalityMaxError
        right_hand_forward_weapon_barrel_dot =
            $numeric.RightHandForwardWeaponBarrelDot
        shot_sample_sequence_delta = $shot.ShotSampleSequenceDelta
        shot_sample_marker_count_delta = $shot.ShotSampleMarkerCountDelta
        shot_sample_projectile_count_delta =
            $shot.ShotSampleProjectileCountDelta
        shot_sample_grip_position_error_m =
            $shot.ShotSampleGripPositionErrorMeters
        shot_sample_aim_position_error_m =
            $shot.ShotSampleAimPositionErrorMeters
        shot_sample_grip_angle_error_deg =
            $shot.ShotSampleGripAngleErrorDegrees
        shot_sample_aim_angle_error_deg =
            $shot.ShotSampleAimAngleErrorDegrees
        shot_sample_weapon_position_error_m =
            $shot.ShotSampleWeaponPositionErrorMeters
        shot_sample_weapon_forward_expected_dot =
            $shot.ShotSampleWeaponForwardExpectedDot
        shot_sample_weapon_barrel_aim_dot =
            $shot.ShotSampleWeaponBarrelAimDot
        shot_sample_weapon_up_expected_dot =
            $shot.ShotSampleWeaponUpExpectedDot
        shot_sample_weapon_determinant_error =
            $shot.ShotSampleWeaponDeterminantError
        shot_sample_weapon_orthogonality_max_error =
            $shot.ShotSampleWeaponOrthogonalityMaxError
        shot_sample_right_wrist_determinant_error =
            $shot.ShotSampleRightWristDeterminantError
        shot_sample_right_wrist_orthogonality_max_error =
            $shot.ShotSampleRightWristOrthogonalityMaxError
        shot_sample_right_hand_forward_weapon_barrel_dot =
            $shot.ShotSampleRightHandForwardWeaponBarrelDot
        controller_reticle_comparison_available =
            $shot.ControllerReticleComparisonAvailable
        controller_reticle_comparison_reason =
            $shot.ControllerReticleComparisonReason
        cross_space_reticle_aim_dot_diagnostic =
            $shot.CrossSpaceReticleAimDotDiagnostic
        cross_space_reticle_aim_angle_deg_diagnostic =
            $shot.CrossSpaceReticleAimAngleDegreesDiagnostic
        cross_space_reticle_origin_distance_m_diagnostic =
            $shot.CrossSpaceReticleOriginDistanceMetersDiagnostic
        muzzle_target_dot = $shot.MuzzleTargetDot
        muzzle_target_angle_deg = $shot.MuzzleTargetAngleDegrees
        muzzle_target_distance_m = $shot.MuzzleTargetDistanceMeters
        muzzle_target_forward_distance_m =
            $shot.MuzzleTargetForwardDistanceMeters
        muzzle_target_ray_miss_m = $shot.MuzzleTargetRayMissMeters
        projectile_target_dot = $shot.ProjectileTargetDot
        projectile_target_angle_deg = $shot.ProjectileTargetAngleDegrees
        projectile_target_distance_m = $shot.ProjectileTargetDistanceMeters
        projectile_target_forward_distance_m =
            $shot.ProjectileTargetForwardDistanceMeters
        projectile_target_ray_miss_m =
            $shot.ProjectileTargetRayMissMeters
        projectile_target_ray_miss_limit_m =
            $shot.ProjectileTargetRayMissLimitMeters
        projectile_aim_dot = $shot.ProjectileAimDot
        projectile_muzzle_forward_dot =
            $shot.ProjectileMuzzleForwardDot
        projectile_muzzle_forward_angle_deg =
            $shot.ProjectileMuzzleForwardAngleDegrees
        projectile_muzzle_position_error_m =
            $shot.ProjectileMuzzlePositionErrorMeters
        projectile_forward_up_abs_dot = $shot.ProjectileForwardUpAbsDot
        muzzle_determinant = $shot.MuzzleDeterminant
        muzzle_determinant_error = $shot.MuzzleDeterminantError
        muzzle_orthogonality_max_error =
            $shot.MuzzleOrthogonalityMaxError
    }
}

function New-EmptyMetrics {
    return [pscustomobject]@{
        GripPositionErrorMeters = $null
        AimPositionErrorMeters = $null
        GripQuaternionAbsDot = $null
        AimQuaternionAbsDot = $null
        GripAngleErrorDegrees = $null
        AimAngleErrorDegrees = $null
        WeaponPositionErrorMeters = $null
        WeaponForwardExpectedDot = $null
        WeaponBarrelAimDot = $null
        WeaponUpExpectedDot = $null
        WeaponDeterminant = $null
        WeaponDeterminantError = $null
        WeaponOrthogonalityMaxError = $null
        RightWristDeterminant = $null
        RightWristDeterminantError = $null
        RightWristOrthogonalityMaxError = $null
        RightHandForwardWeaponBarrelDot = $null
        ExpectedAimWorld = $null
        ExpectedWeaponPosition = $null
    }
}

function New-EmptyShotMetrics {
    return [pscustomobject]@{
        ShotSampleGripPositionErrorMeters = $null
        ShotSampleAimPositionErrorMeters = $null
        ShotSampleGripAngleErrorDegrees = $null
        ShotSampleAimAngleErrorDegrees = $null
        ShotSampleWeaponPositionErrorMeters = $null
        ShotSampleWeaponForwardExpectedDot = $null
        ShotSampleWeaponBarrelAimDot = $null
        ShotSampleWeaponUpExpectedDot = $null
        ShotSampleWeaponDeterminantError = $null
        ShotSampleWeaponOrthogonalityMaxError = $null
        ShotSampleRightWristDeterminantError = $null
        ShotSampleRightWristOrthogonalityMaxError = $null
        ShotSampleRightHandForwardWeaponBarrelDot = $null
        ShotSampleSequenceDelta = $null
        ShotSampleMarkerCountDelta = $null
        ShotSampleProjectileCountDelta = $null
        ControllerReticleComparisonAvailable = $false
        ControllerReticleComparisonReason = $null
        CrossSpaceReticleAimDotDiagnostic = $null
        CrossSpaceReticleAimAngleDegreesDiagnostic = $null
        CrossSpaceReticleOriginDistanceMetersDiagnostic = $null
        MuzzleTargetDot = $null
        MuzzleTargetAngleDegrees = $null
        MuzzleTargetDistanceMeters = $null
        MuzzleTargetForwardDistanceMeters = $null
        MuzzleTargetRayMissMeters = $null
        ProjectileTargetDot = $null
        ProjectileTargetAngleDegrees = $null
        ProjectileTargetDistanceMeters = $null
        ProjectileTargetForwardDistanceMeters = $null
        ProjectileTargetRayMissMeters = $null
        ProjectileTargetRayMissLimitMeters = $null
        ProjectileAimDot = $null
        ProjectileMuzzleForwardDot = $null
        ProjectileMuzzleForwardAngleDegrees = $null
        ProjectileMuzzlePositionErrorMeters = $null
        ProjectileForwardUpAbsDot = $null
        MuzzleDeterminant = $null
        MuzzleDeterminantError = $null
        MuzzleOrthogonalityMaxError = $null
    }
}

$script:InvokeToolPathResolved = Resolve-PackagedInvokeTool
if (-not $OutputDirectory) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputDirectory = Join-Path `
        $PSScriptRoot `
        "pose-matrix-results\$stamp"
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

$suiteCases = [System.Collections.Generic.List[object]]::new()
if ($Suite -in @('FullNumeric', 'Both')) {
    foreach ($case in (Get-FullNumericCases)) {
        $suiteCases.Add([pscustomobject]@{
            Suite = 'FullNumeric'
            Case = $case
        })
    }
}
if ($Suite -in @('VisualFire', 'Both')) {
    foreach ($case in (Get-VisualFireCases)) {
        $suiteCases.Add([pscustomobject]@{
            Suite = 'VisualFire'
            Case = $case
        })
    }
}
if ($CaseName.Count -gt 0) {
    $requestedNames = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($name in $CaseName) {
        if (-not [string]::IsNullOrWhiteSpace($name)) {
            $null = $requestedNames.Add($name)
        }
    }
    $filteredCases = [System.Collections.Generic.List[object]]::new()
    foreach ($entry in $suiteCases) {
        if ($requestedNames.Contains($entry.Case.Name)) {
            $filteredCases.Add($entry)
        }
    }
    $missingNames = @(
        $requestedNames |
            Where-Object {
                $candidate = $_
                -not ($filteredCases | Where-Object {
                    $_.Case.Name -eq $candidate
                })
            })
    if ($missingNames.Count -gt 0) {
        throw "Unknown pose case name(s): $($missingNames -join ', ')."
    }
    $suiteCases = $filteredCases
}

$startedUtc = [DateTime]::UtcNow
$results = [System.Collections.Generic.List[object]]::new()
$originalGrip = $null
$originalAim = $null

try {
    if (-not $PlanOnly) {
        $initialStatus = Get-UEVRStatus
        Assert-ReadyStatus `
            -Status $initialStatus `
            -RequireFire ($Suite -in @('VisualFire', 'Both'))
        $originalGrip = Invoke-OperatorJson `
            -ToolName 'openxr_get_controller_pose' `
            -Arguments @{
                hand = 'right'
                pose_type = 'grip'
                base_space = 'local'
            }
        $originalAim = Invoke-OperatorJson `
            -ToolName 'openxr_get_controller_pose' `
            -Arguments @{
                hand = 'right'
                pose_type = 'aim'
                base_space = 'local'
            }
    }

    $caseIndex = 0
    foreach ($entry in $suiteCases) {
        $caseIndex++
        $case = $entry.Case
        Write-Progress `
            -Activity 'Halo deterministic pose matrix' `
            -Status "$($entry.Suite): $($case.Name)" `
            -PercentComplete (
                100.0 * $caseIndex / [Math]::Max(1, $suiteCases.Count))

        if ($PlanOnly) {
            $results.Add([pscustomobject][ordered]@{
                Suite = $entry.Suite
                Name = $case.Name
                ChangedPose = $case.ChangedPose
                Passed = $null
                Error = ''
                Command = $case
                SequenceBefore = $null
                SequenceAfter = $null
                VisualCountBefore = $null
                VisualCountAfter = $null
                MarkerCountBefore = $null
                MarkerCountAfter = $null
                ProjectileCountBefore = $null
                ProjectileCountAfter = $null
                PollCount = $null
                WaitMilliseconds = $null
                Screenshot = ''
                NumericMetrics = New-EmptyMetrics
                ShotMetrics = New-EmptyShotMetrics
            })
            continue
        }

        $result = [ordered]@{
            Suite = $entry.Suite
            Name = $case.Name
            ChangedPose = $case.ChangedPose
            Passed = $false
            Error = ''
            Command = $case
            SequenceBefore = $null
            SequenceAfter = $null
            VisualCountBefore = $null
            VisualCountAfter = $null
            MarkerCountBefore = $null
            MarkerCountAfter = $null
            ProjectileCountBefore = $null
            ProjectileCountAfter = $null
            PollCount = 0
            WaitMilliseconds = 0.0
            Screenshot = ''
            NumericMetrics = New-EmptyMetrics
            ShotMetrics = New-EmptyShotMetrics
        }
        try {
            $beforeStatus = Get-UEVRStatus
            $before = Get-PoseDiagnostics -Status $beforeStatus
            $result.SequenceBefore = [uint32]$before.sequence
            $result.VisualCountBefore = [uint32]$before.visual_override_count
            $result.MarkerCountBefore = [uint32]$before.marker_override_count
            $result.ProjectileCountBefore =
                [uint32]$before.projectile_override_count

            Set-RightControllerCase -Case $case
            $sample = Wait-ForPoseSample `
                -Case $case `
                -AfterSequence ([uint32]$before.sequence) `
                -AfterVisualCount ([uint32]$before.visual_override_count)
            $result.SequenceAfter = [uint32]$sample.Diagnostics.sequence
            $result.VisualCountAfter =
                [uint32]$sample.Diagnostics.visual_override_count
            $result.MarkerCountAfter =
                [uint32]$sample.Diagnostics.marker_override_count
            $result.ProjectileCountAfter =
                [uint32]$sample.Diagnostics.projectile_override_count
            $result.PollCount = $sample.PollCount
            $result.WaitMilliseconds = $sample.WaitMilliseconds
            $result.NumericMetrics = Get-NumericMetrics `
                -Case $case `
                -Sample $sample
            $numericPassed = Test-NumericMetrics -Metrics $result.NumericMetrics

            $shotPassed = $true
            if ($case.CaptureAndFire) {
                $imageName = (
                    '{0:D2}-{1}-{2}.png' -f
                    $caseIndex,
                    $entry.Suite,
                    $case.Name)
                $imagePath = Join-Path $OutputDirectory $imageName
                Invoke-OperatorScreenshot -Path $imagePath
                $result.Screenshot = $imagePath

                $shotBaseline = $sample.Diagnostics
                $null = Invoke-OperatorJson `
                    -ToolName 'openxr_set_controller_input' `
                    -Arguments @{
                        hand = 'right'
                        component = 'Trigger'
                        value = 1.0
                        auto_release = $true
                        hold_duration = $TriggerHoldSeconds
                    }
                $shot = Wait-ForShotSample `
                    -AfterSequence ([uint32]$shotBaseline.sequence) `
                    -AfterMarkerCount (
                        [uint32]$shotBaseline.marker_override_count) `
                    -AfterProjectileCount (
                        [uint32]$shotBaseline.projectile_override_count)
                $result.PollCount += $shot.PollCount
                $result.WaitMilliseconds += $shot.WaitMilliseconds
                $result.SequenceAfter = [uint32]$shot.Diagnostics.sequence
                $result.MarkerCountAfter =
                    [uint32]$shot.Diagnostics.marker_override_count
                $result.ProjectileCountAfter =
                    [uint32]$shot.Diagnostics.projectile_override_count
                $result.ShotMetrics = Get-ShotMetrics `
                    -Case $case `
                    -Sample $shot `
                    -BaselineDiagnostics $shotBaseline
                $shotPassed = Test-ShotMetrics -Metrics $result.ShotMetrics
            }
            $result.Passed = $numericPassed -and $shotPassed
        } catch {
            $result.Error = (
                $_.Exception.Message + [Environment]::NewLine +
                $_.ScriptStackTrace)
        }
        $results.Add([pscustomobject]$result)
    }
} finally {
    Write-Progress -Activity 'Halo deterministic pose matrix' -Completed
    if (-not $PlanOnly) {
        try {
            $null = Invoke-OperatorJson `
                -ToolName 'openxr_set_controller_input' `
                -Arguments @{
                    hand = 'right'
                    component = 'Trigger'
                    value = 0.0
                }
        } catch {
            Write-Warning "Could not release right trigger: $($_.Exception.Message)"
        }

        if (-not $KeepFinalPose -and
            $null -ne $originalGrip -and
            $null -ne $originalAim) {
            try {
                $null = Invoke-OperatorJson `
                    -ToolName 'openxr_set_controller_pose' `
                    -Arguments @{
                        hand = 'right'
                        pose_type = 'grip'
                        base_space = 'local'
                        position = @($originalGrip.pose.position)
                        orientation = @($originalGrip.pose.orientation)
                        duration_seconds = 0.0
                    }
                $null = Invoke-OperatorJson `
                    -ToolName 'openxr_set_controller_pose' `
                    -Arguments @{
                        hand = 'right'
                        pose_type = 'aim'
                        base_space = 'local'
                        position = @($originalAim.pose.position)
                        orientation = @($originalAim.pose.orientation)
                        duration_seconds = 0.0
                    }
            } catch {
                Write-Warning (
                    'Could not restore the starting right-controller poses: ' +
                    $_.Exception.Message)
            }
        }
    }
}

$completedUtc = [DateTime]::UtcNow
$failedCount = @($results | Where-Object { $_.Passed -eq $false }).Count
$passedCount = @($results | Where-Object { $_.Passed -eq $true }).Count
$summary = [pscustomobject][ordered]@{
    schema_version = 2
    suite = $Suite
    plan_only = [bool]$PlanOnly
    started_utc = $startedUtc.ToString('o')
    completed_utc = $completedUtc.ToString('o')
    invoke_tool_path = $script:InvokeToolPathResolved
    output_directory = $OutputDirectory
    parameters = [pscustomobject]@{
        rotation_degrees = $RotationDegrees
        translation_meters = $TranslationMeters
        neutral_grip_position = @($NeutralGripPosition)
        neutral_aim_position = @($NeutralAimPosition)
        pose_position_tolerance_meters = $PosePositionToleranceMeters
        pose_angle_tolerance_degrees = $PoseAngleToleranceDegrees
        weapon_position_tolerance_meters = $WeaponPositionToleranceMeters
        minimum_direction_dot = $MinimumDirectionDot
        minimum_ballistic_dot = $MinimumBallisticDot
        maximum_muzzle_ray_miss_meters = $MaximumMuzzleRayMissMeters
        minimum_target_forward_distance_meters =
            $MinimumTargetForwardDistanceMeters
        maximum_projectile_spread_degrees =
            $MaximumProjectileSpreadDegrees
        determinant_tolerance = $DeterminantTolerance
        screenshot_eye = $ScreenshotEye
        trigger_hold_seconds = $TriggerHoldSeconds
    }
    shot_sample_coherence = [pscustomobject]@{
        level = 'correlated first post-trigger bridge snapshot'
        exact_marker_delta = 1
        exact_projectile_delta = 1
        requires_newer_visual_sequence = $true
        limitation = (
            'The native v1 diagnostics ABI mutex-protects each returned ' +
            'object, but visual, marker, and projectile fields are written by ' +
            'separate hooks and carry no shared shot event id. Controller and ' +
            'weapon gates therefore use the same returned bridge object, not ' +
            'a provably event-atomic native fire snapshot.')
    }
    controller_reticle_same_shot_gate = [pscustomobject]@{
        available = $false
        reason = (
            'The v1 ABI reports controller/weapon data in first-person ' +
            'viewmodel space and reticle/muzzle/projectile data in native ' +
            'Blam world space. No common transform or fire event id is ' +
            'exported.')
        required_native_telemetry = @(
            'shot event id copied into controller, marker, and projectile data',
            'shot-latched controller ray origin in native Blam world space',
            'shot-latched controller ray forward in native Blam world space'
        )
    }
    case_count = $results.Count
    passed_count = $passedCount
    failed_count = $failedCount
    cases = @($results)
}

$jsonPath = Join-Path $OutputDirectory 'pose-matrix-summary.json'
$csvPath = Join-Path $OutputDirectory 'pose-matrix-summary.csv'
$summary |
    ConvertTo-Json -Depth 30 |
    Set-Content -LiteralPath $jsonPath -Encoding utf8
@($results | ForEach-Object { Convert-CaseResultToCsvRow -Result $_ }) |
    Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding utf8

[pscustomobject]@{
    Suite = $Suite
    PlanOnly = [bool]$PlanOnly
    Cases = $results.Count
    Passed = $passedCount
    Failed = $failedCount
    Json = $jsonPath
    Csv = $csvPath
}

if (-not $PlanOnly -and $failedCount -gt 0) {
    exit 1
}
