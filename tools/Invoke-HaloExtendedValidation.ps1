[CmdletBinding()]
param(
    [ValidateRange(0, [int]::MaxValue)]
    [int]$ExpectedGamePid = 0,

    [int]$OperatorPort = 8720,

    [string]$OperatorPackageRoot = (
        'E:\Github\UEVRMetaXROperator\dist\release\' +
        'UEVR-Meta-XR-Operator-205.1-nightly-01139-analog-hands-v1'),

    [string]$OutputDirectory = '',

    [ValidateRange(0.001, 0.05)]
    [double]$PosePositionToleranceMeters = 0.005,

    [ValidateRange(0.1, 5.0)]
    [double]$PoseAngleToleranceDegrees = 1.0,

    [switch]$PlanOnly,

    [switch]$InputOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputDirectory) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputDirectory = Join-Path $repoRoot (
        "diagnostics\validation\extended-$stamp")
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)

$inventory = @(
    [pscustomobject]@{ Id='HMD-01'; Cases=7; Scope='rig invariance plus head-only inverse-HMD yaw, pitch, translation' },
    [pscustomobject]@{ Id='HAND-01'; Cases=5; Scope='tracked wrist and hand geometry at extreme poses' },
    [pscustomobject]@{ Id='TWO-01'; Cases=4; Scope='outside-zone, acquire, retained latch, release' },
    [pscustomobject]@{ Id='INP-01'; Cases=7; Scope='stick echo, bridge observation, D-pad modes, release state' }
)
if ($PlanOnly) {
    $inventory | Format-Table -AutoSize
    return
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$proxyPath = Join-Path $OperatorPackageRoot (
    'meta-xr-operator\windows\meta-xr-operator-mcp-proxy.exe')
$proxyPath = (Resolve-Path -LiteralPath $proxyPath).Path

$listeners = @(Get-NetTCPConnection -State Listen -LocalPort $OperatorPort)
$owners = @($listeners | Select-Object -ExpandProperty OwningProcess -Unique)
if ($owners.Count -ne 1) {
    throw (
        "Operator port $OperatorPort does not have exactly one owner; " +
        "observed owners: $($owners -join ',').")
}
if ($ExpectedGamePid -eq 0) {
    $ExpectedGamePid = [int]$owners[0]
} elseif ([int]$owners[0] -ne $ExpectedGamePid) {
    throw (
        "Operator port $OperatorPort is owned by PID $($owners[0]), not " +
        "requested Halo PID $ExpectedGamePid.")
}
$game = Get-CimInstance Win32_Process -Filter "ProcessId=$ExpectedGamePid"
if ($null -eq $game -or $game.Name -ne 'HaloCampaignEvolved.exe') {
    throw (
        "Operator port $OperatorPort owner PID $ExpectedGamePid is not " +
        "HaloCampaignEvolved.exe.")
}

$startInfo = [System.Diagnostics.ProcessStartInfo]::new($proxyPath)
$startInfo.WorkingDirectory = Split-Path -Parent $proxyPath
$startInfo.UseShellExecute = $false
$startInfo.RedirectStandardInput = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$startInfo.CreateNoWindow = $true
$script:proxy = $null
$script:requestId = 0
$script:toolCallsOnProxy = 0

function Read-Response {
    param([int]$Id, [int]$TimeoutSeconds = 20)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $remaining = [Math]::Max(
            1, [int]($deadline - [DateTime]::UtcNow).TotalMilliseconds)
        $read = $script:proxy.StandardOutput.ReadLineAsync()
        if (-not $read.Wait($remaining)) { break }
        $line = $read.Result
        if ($null -eq $line) { break }
        try {
            $message = $line | ConvertFrom-Json -ErrorAction Stop
            if ([int]$message.id -eq $Id) { return $message }
        } catch {
            # Startup diagnostics are not JSON-RPC messages.
        }
    }
    throw "Timed out waiting for Meta XR Operator response $Id."
}

function Send-Request {
    param([string]$Method, $Params)
    $script:requestId++
    $request = [ordered]@{
        jsonrpc = '2.0'
        id = $script:requestId
        method = $Method
        params = $Params
    } | ConvertTo-Json -Depth 30 -Compress
    $script:proxy.StandardInput.WriteLine($request)
    $script:proxy.StandardInput.Flush()
    Read-Response -Id $script:requestId
}

function Start-OperatorClient {
    if ($null -ne $script:proxy -and -not $script:proxy.HasExited) {
        $script:proxy.Kill()
        $script:proxy.WaitForExit(2000) | Out-Null
    }
    $script:proxy = [System.Diagnostics.Process]::new()
    $script:proxy.StartInfo = $startInfo
    $null = $script:proxy.Start()
    $script:requestId = 0
    $script:toolCallsOnProxy = 0
    $null = Send-Request -Method 'initialize' -Params ([ordered]@{
        protocolVersion='2025-06-18'; capabilities=@{}
        clientInfo=@{name='halo-extended-validation';version='1.0'}
    })
    $notification = @{
        jsonrpc='2.0'; method='notifications/initialized'; params=@{}
    } | ConvertTo-Json -Compress
    $script:proxy.StandardInput.WriteLine($notification)
    $script:proxy.StandardInput.Flush()
}

function Invoke-OperatorJson {
    param([string]$Name, [hashtable]$Arguments = @{})
    # Meta's in-process server becomes unreliable after a long stream of calls
    # on one proxy connection. Rotate the hidden proxy after six tool calls;
    # Operator ownership lives in Halo and is unaffected by proxy recycling.
    if ($null -eq $script:proxy -or $script:proxy.HasExited -or
        $script:toolCallsOnProxy -ge 6) {
        Start-OperatorClient
    }
    $script:toolCallsOnProxy++
    $response = Send-Request -Method 'tools/call' -Params ([ordered]@{
        name = $Name
        arguments = $Arguments
    })
    if ([bool]$response.result.isError) {
        throw "Meta XR Operator tool '$Name' reported an error."
    }
    $text = @(
        $response.result.content |
            Where-Object { $_.type -eq 'text' } |
            Select-Object -First 1).text
    if (-not $text) { throw "Meta XR Operator tool '$Name' returned no JSON." }
    $value = $text | ConvertFrom-Json
    if (($null -ne $value.PSObject.Properties['error'] -and $value.error) -or
        ($null -ne $value.PSObject.Properties['success'] -and
         $value.success -eq $false)) {
        throw "Meta XR Operator tool '$Name' failed: $text"
    }
    return $value
}

function Get-Status { Invoke-OperatorJson -Name 'UEVR_Status' }

function ConvertTo-Vector3 {
    param($Value)
    if ($Value -is [System.Array]) {
        return @([double]($Value[0]), [double]($Value[1]), [double]($Value[2]))
    }
    return @([double]$Value.x, [double]$Value.y, [double]$Value.z)
}

function Test-FiniteNumber {
    param([double]$Value)
    return -not [double]::IsNaN($Value) -and
        -not [double]::IsInfinity($Value)
}

function ConvertTo-Quaternion {
    param($Value)
    if ($Value -is [System.Array]) {
        $q = @(
            [double]($Value[0]), [double]($Value[1]),
            [double]($Value[2]), [double]($Value[3]))
    } else {
        $q = @(
            [double]$Value.x, [double]$Value.y,
            [double]$Value.z, [double]$Value.w)
    }
    $qx=[double]($q[0]); $qy=[double]($q[1])
    $qz=[double]($q[2]); $qw=[double]($q[3])
    $length = [Math]::Sqrt(
        $qx*$qx + $qy*$qy + $qz*$qz + $qw*$qw)
    if (-not (Test-FiniteNumber $length) -or $length -lt 1.0e-9) {
        throw 'Quaternion is invalid.'
    }
    return @($q | ForEach-Object { $_ / $length })
}

function Add-Vector { param($A,$B) @(0..2 | ForEach-Object { [double]($A[$_]) + [double]($B[$_]) }) }
function Subtract-Vector { param($A,$B) @(0..2 | ForEach-Object { [double]($A[$_]) - [double]($B[$_]) }) }
function Scale-Vector { param($Value,[double]$Scale) @(0..2 | ForEach-Object { [double]($Value[$_]) * $Scale }) }
function Get-Dot {
    param($A,$B)
    $ax=[double]($A[0]); $ay=[double]($A[1]); $az=[double]($A[2])
    $bx=[double]($B[0]); $by=[double]($B[1]); $bz=[double]($B[2])
    $ax*$bx + $ay*$by + $az*$bz
}
function Get-Length { param($Value) [Math]::Sqrt((Get-Dot $Value $Value)) }
function Get-Distance { param($A,$B) Get-Length (Subtract-Vector $A $B) }
function Normalize-Vector {
    param($Value)
    $length = Get-Length $Value
    if (-not (Test-FiniteNumber $length) -or $length -lt 1.0e-9) {
        throw 'Vector cannot be normalized.'
    }
    Scale-Vector $Value (1.0 / $length)
}

function Multiply-Quaternion {
    param($Left,$Right)
    $l = ConvertTo-Quaternion $Left
    $r = ConvertTo-Quaternion $Right
    $lx=[double]($l[0]); $ly=[double]($l[1])
    $lz=[double]($l[2]); $lw=[double]($l[3])
    $rx=[double]($r[0]); $ry=[double]($r[1])
    $rz=[double]($r[2]); $rw=[double]($r[3])
    ConvertTo-Quaternion @(
        ($lw*$rx + $lx*$rw + $ly*$rz - $lz*$ry),
        ($lw*$ry - $lx*$rz + $ly*$rw + $lz*$rx),
        ($lw*$rz + $lx*$ry - $ly*$rx + $lz*$rw),
        ($lw*$rw - $lx*$rx - $ly*$ry - $lz*$rz))
}

function Get-Conjugate {
    param($Q)
    $v=ConvertTo-Quaternion $Q
    @(
        (-[double]($v[0])),
        (-[double]($v[1])),
        (-[double]($v[2])),
        ([double]($v[3])))
}

function Rotate-Vector {
    param($Quaternion,$Vector)
    $q = ConvertTo-Quaternion $Quaternion
    $qx=[double]($q[0]); $qy=[double]($q[1])
    $qz=[double]($q[2]); $s=[double]($q[3])
    $vx=[double]($Vector[0]); $vy=[double]($Vector[1]); $vz=[double]($Vector[2])
    $u = @($qx,$qy,$qz)
    $dotUV = Get-Dot $u $Vector
    $dotUU = Get-Dot $u $u
    $cross = @(
        ($qy*$vz-$qz*$vy),
        ($qz*$vx-$qx*$vz),
        ($qx*$vy-$qy*$vx))
    @(
        (2*$dotUV*$qx+($s*$s-$dotUU)*$vx+2*$s*[double]($cross[0])),
        (2*$dotUV*$qy+($s*$s-$dotUU)*$vy+2*$s*[double]($cross[1])),
        (2*$dotUV*$qz+($s*$s-$dotUU)*$vz+2*$s*[double]($cross[2])))
}

function New-AxisQuaternion {
    param([ValidateSet('x','y','z')]$Axis,[double]$Degrees)
    $half = $Degrees * [Math]::PI / 360.0
    $s = [Math]::Sin($half)
    switch ($Axis) {
        x { return @($s,0.0,0.0,[Math]::Cos($half)) }
        y { return @(0.0,$s,0.0,[Math]::Cos($half)) }
        z { return @(0.0,0.0,$s,[Math]::Cos($half)) }
    }
}

function Get-QuaternionAngleDegrees {
    param($A,$B)
    $a = ConvertTo-Quaternion $A
    $b = ConvertTo-Quaternion $B
    $dot = [Math]::Abs(
        [double]($a[0]) * [double]($b[0]) +
        [double]($a[1]) * [double]($b[1]) +
        [double]($a[2]) * [double]($b[2]) +
        [double]($a[3]) * [double]($b[3]))
    $dot = [Math]::Min(1.0,[Math]::Max(-1.0,$dot))
    2.0 * [Math]::Acos($dot) * 180.0 / [Math]::PI
}

function Convert-OpenXRToBlam {
    param($Value)
    @(
        (-[double]($Value[2])),
        (-[double]($Value[0])),
        ([double]($Value[1])))
}

function Get-Basis {
    param($Matrix)
    [pscustomobject]@{
        Forward = ConvertTo-Vector3 $Matrix.forward
        Left = ConvertTo-Vector3 $Matrix.left
        Up = ConvertTo-Vector3 $Matrix.up
    }
}

function Transform-BasisVector {
    param($Basis,$Value)
    Add-Vector `
        (Add-Vector `
            (Scale-Vector $Basis.Forward ([double]($Value[0]))) `
            (Scale-Vector $Basis.Left ([double]($Value[1])))) `
        (Scale-Vector $Basis.Up ([double]($Value[2])))
}

function Get-BasisQuality {
    param($Matrix)
    $b = Get-Basis $Matrix
    $lx=[double]($b.Left[0]); $ly=[double]($b.Left[1]); $lz=[double]($b.Left[2])
    $ux=[double]($b.Up[0]); $uy=[double]($b.Up[1]); $uz=[double]($b.Up[2])
    $cross = @(
        ($ly*$uz-$lz*$uy),
        ($lz*$ux-$lx*$uz),
        ($lx*$uy-$ly*$ux))
    $det = Get-Dot $b.Forward $cross
    $orth = [Math]::Max(
        [Math]::Abs((Get-Dot $b.Forward $b.Left)),
        [Math]::Max(
            [Math]::Abs((Get-Dot $b.Forward $b.Up)),
            [Math]::Abs((Get-Dot $b.Left $b.Up))))
    [pscustomobject]@{
        determinant = $det
        determinant_error = [Math]::Abs($det - 1.0)
        orthogonality_error = $orth
        scale_error = [Math]::Abs([double]$Matrix.scale - 1.0)
        finite = @(
            $b.Forward + $b.Left + $b.Up +
            (ConvertTo-Vector3 $Matrix.position) |
            Where-Object { -not (Test-FiniteNumber ([double]$_)) }).Count -eq 0
    }
}

function Get-BasisMinimumDot {
    param($A,$B)
    $aBasis = Get-Basis $A
    $bBasis = Get-Basis $B
    $forwardDot = Get-Dot `
        (Normalize-Vector $aBasis.Forward) `
        (Normalize-Vector $bBasis.Forward)
    $leftDot = Get-Dot `
        (Normalize-Vector $aBasis.Left) `
        (Normalize-Vector $bBasis.Left)
    $upDot = Get-Dot `
        (Normalize-Vector $aBasis.Up) `
        (Normalize-Vector $bBasis.Up)
    [Math]::Min($forwardDot, [Math]::Min($leftDot, $upDot))
}

function Set-HeadPose {
    param($Position,$Orientation)
    $null = Invoke-OperatorJson 'openxr_set_head_pose' @{
        base_space='local'; position=@($Position); orientation=@($Orientation)
        duration_seconds=0.0
    }
}

function Set-ControllerPose {
    param([string]$Hand,[string]$PoseType,$Position,$Orientation)
    $null = Invoke-OperatorJson 'openxr_set_controller_pose' @{
        base_space='local'; hand=$Hand; pose_type=$PoseType
        position=@($Position); orientation=@($Orientation); duration_seconds=0.0
    }
}

function Set-ControllerInput {
    param([string]$Hand,[string]$Component,[double]$Value,[string]$SubComponent='')
    $arguments = @{ hand=$Hand; component=$Component; value=$Value; auto_release=$false }
    if ($SubComponent) { $arguments.sub_component = $SubComponent }
    $null = Invoke-OperatorJson 'openxr_set_controller_input' $arguments
}

function Set-ExplicitRig {
    param($HeadPosition,$HeadOrientation,$RightGrip,$RightAim,$LeftGrip,$LeftAim)
    Set-HeadPose $HeadPosition $HeadOrientation
    Set-ControllerPose right grip $RightGrip.Position $RightGrip.Orientation
    Set-ControllerPose right aim $RightAim.Position $RightAim.Orientation
    Set-ControllerPose left grip $LeftGrip.Position $LeftGrip.Orientation
    Set-ControllerPose left aim $LeftAim.Position $LeftAim.Orientation
}

function New-Pose { param($Position,$Orientation) [pscustomobject]@{ Position=@($Position); Orientation=@($Orientation) } }

function Wait-ForRigSample {
    param($HeadPosition,$HeadOrientation,$RightGrip,$RightAim,$LeftGrip,$LeftAim,[uint32]$AfterSequence)
    $deadline = [DateTime]::UtcNow.AddSeconds(4)
    do {
        $status = Get-Status
        $d = $status.halo_motion_controls.pose_diagnostics
        if ([uint32]$d.sequence -gt $AfterSequence -and
            (Get-Distance (ConvertTo-Vector3 $d.hmd.position) $HeadPosition) -le $PosePositionToleranceMeters -and
            (Get-QuaternionAngleDegrees (ConvertTo-Quaternion $d.hmd.rotation) $HeadOrientation) -le $PoseAngleToleranceDegrees -and
            (Get-Distance (ConvertTo-Vector3 $d.right_grip.position) $RightGrip.Position) -le $PosePositionToleranceMeters -and
            (Get-Distance (ConvertTo-Vector3 $d.right_aim.position) $RightAim.Position) -le $PosePositionToleranceMeters -and
            (Get-Distance (ConvertTo-Vector3 $d.left_grip.position) $LeftGrip.Position) -le $PosePositionToleranceMeters) {
            return $status
        }
        Start-Sleep -Milliseconds 40
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Timed out waiting for the commanded HMD/controller pose sample.'
}

function Get-ExpectedWeaponMetrics {
    param($Diagnostics)
    $hmdPosition = ConvertTo-Vector3 $Diagnostics.hmd.position
    $hmdRotation = ConvertTo-Quaternion $Diagnostics.hmd.rotation
    $gripPosition = ConvertTo-Vector3 $Diagnostics.right_grip.position
    $aimRotation = ConvertTo-Quaternion $Diagnostics.right_aim.rotation
    $rootPosition = ConvertTo-Vector3 $Diagnostics.root.position
    $rootBasis = Get-Basis $Diagnostics.root
    $inverseHmd = Get-Conjugate $hmdRotation
    $relativeGrip = Rotate-Vector $inverseHmd (Subtract-Vector $gripPosition $hmdPosition)
    $relativeAim = Multiply-Quaternion $inverseHmd $aimRotation
    $controllerForward = Convert-OpenXRToBlam (
        Rotate-Vector $relativeAim @(0.0,0.0,-1.0))
    $expectedAim = Normalize-Vector (
        Transform-BasisVector $rootBasis $controllerForward)
    $gripDelta = Scale-Vector (Convert-OpenXRToBlam $relativeGrip) (1.0/3.048)
    $expectedPosition = Add-Vector $rootPosition (
        Transform-BasisVector $rootBasis $gripDelta)
    $weaponPosition = ConvertTo-Vector3 $Diagnostics.weapon.position
    $weaponBasis = Get-Basis $Diagnostics.weapon
    [pscustomobject]@{
        weapon_position_error_m = 3.048 * (Get-Distance $weaponPosition $expectedPosition)
        weapon_barrel_aim_dot = Get-Dot (Normalize-Vector $weaponBasis.Left) $expectedAim
        relative_grip = $relativeGrip
    }
}

function Get-HandGeometryQuality {
    param($Diagnostics,[string]$Side)
    $forward = ConvertTo-Vector3 $Diagnostics."${Side}_hand_forward"
    $thumb = ConvertTo-Vector3 $Diagnostics."${Side}_hand_thumb_side"
    $palm = ConvertTo-Vector3 $Diagnostics."${Side}_hand_palm_normal"
    [pscustomobject]@{
        forward_length_error = [Math]::Abs((Get-Length $forward)-1.0)
        thumb_length_error = [Math]::Abs((Get-Length $thumb)-1.0)
        palm_length_error = [Math]::Abs((Get-Length $palm)-1.0)
        max_abs_axis_dot = [Math]::Max(
            [Math]::Abs((Get-Dot $forward $thumb)),
            [Math]::Max(
                [Math]::Abs((Get-Dot $forward $palm)),
                [Math]::Abs((Get-Dot $thumb $palm))))
    }
}

function Add-CaseResult {
    param([string]$Id,[string]$Name,[hashtable]$Metrics,[string[]]$Failures,[string[]]$Notes=@())
    $script:results.Add([pscustomobject]@{
        id=$Id; name=$Name; passed=($Failures.Count -eq 0)
        metrics=[pscustomobject]$Metrics; failures=@($Failures); notes=@($Notes)
    })
}

function Get-TextMarker { param([string]$Name) (Get-Content -Raw -LiteralPath (Join-Path $env:APPDATA "UnrealVRMod\HaloCampaignEvolved\data\$Name")).Trim() }

$identity = @(0.0,0.0,0.0,1.0)
$baseHeadPosition = @(0.0,0.0,0.0)
$baseRightGrip = New-Pose @(0.30,-0.33,-0.46) $identity
$baseRightAim = New-Pose @(0.30,-0.30,-0.555) $identity
$baseLeftGrip = New-Pose @(-0.30,-0.33,-0.46) $identity
$baseLeftAim = New-Pose @(-0.30,-0.30,-0.555) $identity
$script:results = [System.Collections.Generic.List[object]]::new()
$fatal = $null
$original = $null
$originalDPad = $null

try {
    $head = Invoke-OperatorJson 'openxr_get_head_pose' @{base_space='local'}
    $original = [ordered]@{
        Head = New-Pose $head.pose.position $head.pose.orientation
    }
    foreach ($hand in @('right','left')) {
        foreach ($poseType in @('grip','aim')) {
            $pose = Invoke-OperatorJson 'openxr_get_controller_pose' @{
                base_space='local'; hand=$hand; pose_type=$poseType
            }
            $original["$hand-$poseType"] =
                New-Pose $pose.pose.position $pose.pose.orientation
        }
    }
    $originalDPad = (Invoke-OperatorJson 'UEVR_GetConfig' @{
        key='VR_DPadShifting' }).value

    Set-ControllerInput left Grip 0.0
    Set-ControllerInput right Trigger 0.0
    Set-ControllerInput left Thumbstick 0.0 X
    Set-ControllerInput left Thumbstick 0.0 Y

    if (-not $InputOnly) {
    $before = Get-Status
    Set-ExplicitRig $baseHeadPosition $identity `
        $baseRightGrip $baseRightAim $baseLeftGrip $baseLeftAim
    $baselineStatus = Wait-ForRigSample $baseHeadPosition $identity `
        $baseRightGrip $baseRightAim $baseLeftGrip $baseLeftAim `
        ([uint32]$before.halo_motion_controls.pose_diagnostics.sequence)
    $baseline = $baselineStatus.halo_motion_controls.pose_diagnostics

    $rigCases = @(
        @{Name='rig_translate_xyz';Position=@(0.12,0.08,-0.10);Orientation=$identity},
        @{Name='rig_yaw_pos_20';Position=$baseHeadPosition;Orientation=(New-AxisQuaternion y 20)},
        @{Name='rig_pitch_neg_15';Position=$baseHeadPosition;Orientation=(New-AxisQuaternion x -15)},
        @{Name='rig_compound';Position=@(0.08,0.05,-0.04);Orientation=(Multiply-Quaternion (New-AxisQuaternion y 18) (New-AxisQuaternion x -12))}
    )
    foreach ($case in $rigCases) {
        $q = $case.Orientation
        $hp = $case.Position
        function Move-BasePose($pose) {
            New-Pose (Add-Vector $hp (Rotate-Vector $q $pose.Position)) (
                Multiply-Quaternion $q $pose.Orientation)
        }
        $rg=Move-BasePose $baseRightGrip; $ra=Move-BasePose $baseRightAim
        $lg=Move-BasePose $baseLeftGrip; $la=Move-BasePose $baseLeftAim
        $seq=[uint32](Get-Status).halo_motion_controls.pose_diagnostics.sequence
        Set-ExplicitRig $hp $q $rg $ra $lg $la
        $status=Wait-ForRigSample $hp $q $rg $ra $lg $la $seq
        $d=$status.halo_motion_controls.pose_diagnostics
        $metrics=Get-ExpectedWeaponMetrics $d
        $values=@{
            weapon_position_delta_m=3.048*(Get-Distance (ConvertTo-Vector3 $d.weapon.position) (ConvertTo-Vector3 $baseline.weapon.position))
            right_wrist_position_delta_m=3.048*(Get-Distance (ConvertTo-Vector3 $d.right_wrist.position) (ConvertTo-Vector3 $baseline.right_wrist.position))
            left_wrist_position_delta_m=3.048*(Get-Distance (ConvertTo-Vector3 $d.left_wrist.position) (ConvertTo-Vector3 $baseline.left_wrist.position))
            reticle_position_delta_m=3.048*(Get-Distance (ConvertTo-Vector3 $d.reticle_position) (ConvertTo-Vector3 $baseline.reticle_position))
            weapon_basis_min_dot=Get-BasisMinimumDot $d.weapon $baseline.weapon
            right_wrist_basis_min_dot=Get-BasisMinimumDot $d.right_wrist $baseline.right_wrist
            left_wrist_basis_min_dot=Get-BasisMinimumDot $d.left_wrist $baseline.left_wrist
            weapon_position_formula_error_m=$metrics.weapon_position_error_m
            weapon_barrel_aim_dot=$metrics.weapon_barrel_aim_dot
        }
        $fail=[System.Collections.Generic.List[string]]::new()
        if ($values.weapon_position_delta_m -gt 0.02) { $fail.Add('weapon was not rigid-rig invariant within 2 cm') }
        if ($values.right_wrist_position_delta_m -gt 0.03) { $fail.Add('right wrist was not rigid-rig invariant within 3 cm') }
        if ($values.left_wrist_position_delta_m -gt 0.03) { $fail.Add('left wrist was not rigid-rig invariant within 3 cm') }
        if ($values.reticle_position_delta_m -gt 0.04) { $fail.Add('reticle was not rigid-rig invariant within 4 cm') }
        if ($values.weapon_position_formula_error_m -gt 0.02) { $fail.Add('weapon disagreed with one inverse-HMD transform') }
        if ($values.weapon_barrel_aim_dot -lt 0.98) { $fail.Add('weapon barrel disagreed with HMD-relative aim') }
        Add-CaseResult HMD-01 $case.Name $values $fail.ToArray()
    }

    $headOnlyCases=@(
        @{Name='head_only_translate_x';Position=@(0.12,0,0);Orientation=$identity},
        @{Name='head_only_yaw_pos_20';Position=$baseHeadPosition;Orientation=(New-AxisQuaternion y 20)},
        @{Name='head_only_pitch_neg_15';Position=$baseHeadPosition;Orientation=(New-AxisQuaternion x -15)}
    )
    foreach($case in $headOnlyCases) {
        $seq=[uint32](Get-Status).halo_motion_controls.pose_diagnostics.sequence
        Set-ExplicitRig $case.Position $case.Orientation `
            $baseRightGrip $baseRightAim $baseLeftGrip $baseLeftAim
        $status=Wait-ForRigSample $case.Position $case.Orientation `
            $baseRightGrip $baseRightAim $baseLeftGrip $baseLeftAim $seq
        $d=$status.halo_motion_controls.pose_diagnostics
        $metrics=Get-ExpectedWeaponMetrics $d
        $values=@{
            weapon_position_formula_error_m=$metrics.weapon_position_error_m
            weapon_barrel_aim_dot=$metrics.weapon_barrel_aim_dot
            relative_grip_delta_from_baseline_m=Get-Distance $metrics.relative_grip (ConvertTo-Vector3 $baseline.right_grip.position)
        }
        $fail=[System.Collections.Generic.List[string]]::new()
        if ($values.weapon_position_formula_error_m -gt 0.02) { $fail.Add('weapon disagreed with one inverse-HMD transform') }
        if ($values.weapon_barrel_aim_dot -lt 0.98) { $fail.Add('weapon barrel disagreed with HMD-relative aim') }
        if ($values.relative_grip_delta_from_baseline_m -lt 0.03) { $fail.Add('head-only case did not produce a meaningful relative-pose change') }
        Add-CaseResult HMD-01 $case.Name $values $fail.ToArray()
    }

    $handCases=@(
        @{Name='close_to_face';Right=@(0.18,-0.05,-0.18);Left=@(-0.18,-0.05,-0.18)},
        @{Name='cross_body';Right=@(-0.25,-0.25,-0.45);Left=@(0.25,-0.25,-0.45)},
        @{Name='fully_extended';Right=@(0.75,-0.25,-0.90);Left=@(-0.75,-0.25,-0.90)},
        @{Name='above_head';Right=@(0.30,0.40,-0.46);Left=@(-0.30,0.40,-0.46)},
        @{Name='behind_shoulder';Right=@(0.45,-0.20,0.20);Left=@(-0.45,-0.20,0.20)}
    )
    foreach($case in $handCases) {
        $rg=New-Pose $case.Right $identity
        $ra=New-Pose (Add-Vector $case.Right @(0.0,0.03,-0.095)) $identity
        $lg=New-Pose $case.Left $identity
        $la=New-Pose (Add-Vector $case.Left @(0.0,0.03,-0.095)) $identity
        $seq=[uint32](Get-Status).halo_motion_controls.pose_diagnostics.sequence
        Set-ExplicitRig $baseHeadPosition $identity $rg $ra $lg $la
        $status=Wait-ForRigSample $baseHeadPosition $identity $rg $ra $lg $la $seq
        $d=$status.halo_motion_controls.pose_diagnostics
        $rq=Get-BasisQuality $d.right_wrist; $lq=Get-BasisQuality $d.left_wrist
        $rh=Get-HandGeometryQuality $d right; $lh=Get-HandGeometryQuality $d left
        $rightGripDelta=Get-Distance $case.Right $baseRightGrip.Position
        $leftGripDelta=Get-Distance $case.Left $baseLeftGrip.Position
        $rightWristDelta=3.048*(Get-Distance (ConvertTo-Vector3 $d.right_wrist.position) (ConvertTo-Vector3 $baseline.right_wrist.position))
        $leftWristDelta=3.048*(Get-Distance (ConvertTo-Vector3 $d.left_wrist.position) (ConvertTo-Vector3 $baseline.left_wrist.position))
        $values=@{
            arm_ik_enabled=[bool]$status.halo_motion_controls.two_hand_ik_enabled
            right_wrist_determinant_error=$rq.determinant_error
            right_wrist_orthogonality_error=$rq.orthogonality_error
            right_wrist_scale_error=$rq.scale_error
            left_wrist_determinant_error=$lq.determinant_error
            left_wrist_orthogonality_error=$lq.orthogonality_error
            left_wrist_scale_error=$lq.scale_error
            right_wrist_motion_magnitude_error_m=[Math]::Abs($rightWristDelta-$rightGripDelta)
            left_wrist_motion_magnitude_error_m=[Math]::Abs($leftWristDelta-$leftGripDelta)
            right_hand_max_abs_axis_dot=$rh.max_abs_axis_dot
            left_hand_max_abs_axis_dot=$lh.max_abs_axis_dot
            right_hand_geometry_valid=[bool]$d.right_hand_geometry_valid
            left_hand_geometry_valid=[bool]$d.left_hand_geometry_valid
        }
        $fail=[System.Collections.Generic.List[string]]::new()
        if(-not $rq.finite -or -not $lq.finite){$fail.Add('a wrist matrix contained non-finite values')}
        if($rq.determinant_error -gt 0.05 -or $lq.determinant_error -gt 0.05){$fail.Add('a wrist determinant exceeded 0.05 error')}
        if($rq.orthogonality_error -gt 0.05 -or $lq.orthogonality_error -gt 0.05){$fail.Add('a wrist orthogonality error exceeded 0.05')}
        if($rq.scale_error -gt 0.001 -or $lq.scale_error -gt 0.001){$fail.Add('a wrist scale differed from 1')}
        if($values.right_wrist_motion_magnitude_error_m -gt 0.06 -or $values.left_wrist_motion_magnitude_error_m -gt 0.06){$fail.Add('wrist motion magnitude did not follow grip within 6 cm')}
        if(-not $values.right_hand_geometry_valid -or -not $values.left_hand_geometry_valid){$fail.Add('hand geometry diagnostics became invalid')}
        if($rh.max_abs_axis_dot -gt 0.20 -or $lh.max_abs_axis_dot -gt 0.20){$fail.Add('hand geometry axes were not near-orthogonal')}
        Add-CaseResult HAND-01 $case.Name $values $fail.ToArray() @(
            'Shoulder, elbow, clavicle, triangle stretch, and tracking-loss transitions are not exposed by status ABI v1.')
    }

    Set-ControllerInput left Grip 0.0
    $seq=[uint32](Get-Status).halo_motion_controls.pose_diagnostics.sequence
    Set-ExplicitRig $baseHeadPosition $identity `
        $baseRightGrip $baseRightAim $baseLeftGrip $baseLeftAim
    $outside=Wait-ForRigSample $baseHeadPosition $identity `
        $baseRightGrip $baseRightAim $baseLeftGrip $baseLeftAim $seq
    Set-ControllerInput left Grip 1.0
    Start-Sleep -Milliseconds 250
    $outside=Get-Status
    Add-CaseResult TWO-01 outside_zone_no_latch @{
        two_hand_hold_active=[bool]$outside.halo_motion_controls.two_hand_hold_active
    } @($(if([bool]$outside.halo_motion_controls.two_hand_hold_active){'hold latched outside acquisition zone'}))
    Set-ControllerInput left Grip 0.0

    $insidePosition=@(0.30,-0.33,-0.81)
    $insideGrip=New-Pose $insidePosition $identity
    $insideAim=New-Pose (Add-Vector $insidePosition @(0.0,0.03,-0.095)) $identity
    $seq=[uint32](Get-Status).halo_motion_controls.pose_diagnostics.sequence
    Set-ExplicitRig $baseHeadPosition $identity `
        $baseRightGrip $baseRightAim $insideGrip $insideAim
    $null=Wait-ForRigSample $baseHeadPosition $identity `
        $baseRightGrip $baseRightAim $insideGrip $insideAim $seq
    Set-ControllerInput left Grip 1.0
    Start-Sleep -Milliseconds 350
    $latched=Get-Status
    $expectedTwoHand=Normalize-Vector (Convert-OpenXRToBlam (
        Subtract-Vector $insidePosition $baseRightGrip.Position))
    $weaponBasis=Get-Basis $latched.halo_motion_controls.pose_diagnostics.weapon
    $lineDot=Get-Dot (Normalize-Vector $weaponBasis.Left) $expectedTwoHand
    $hideMarker=Get-TextMarker 'halo_motion_reticle_hide.active'
    $acquireFailures=[System.Collections.Generic.List[string]]::new()
    if(-not [bool]$latched.halo_motion_controls.two_hand_hold_active){$acquireFailures.Add('hold did not latch inside acquisition zone')}
    if($lineDot -lt 0.98){$acquireFailures.Add('weapon barrel did not converge on hand-to-hand basis')}
    if($hideMarker -ne 'on'){$acquireFailures.Add("reticle hide marker was '$hideMarker'")}
    Add-CaseResult TWO-01 acquire_and_basis @{
        two_hand_hold_active=[bool]$latched.halo_motion_controls.two_hand_hold_active
        weapon_hand_line_dot=$lineDot; reticle_hide_marker=$hideMarker
    } $acquireFailures.ToArray()

    Set-ControllerPose left grip $baseLeftGrip.Position $baseLeftGrip.Orientation
    Set-ControllerPose left aim $baseLeftAim.Position $baseLeftAim.Orientation
    Start-Sleep -Milliseconds 200
    $retained=Get-Status
    Add-CaseResult TWO-01 retained_outside_zone @{
        two_hand_hold_active=[bool]$retained.halo_motion_controls.two_hand_hold_active
    } @($(if(-not [bool]$retained.halo_motion_controls.two_hand_hold_active){'latched hold did not persist outside zone while grip remained held'}))

    Set-ControllerInput left Grip 0.0
    Start-Sleep -Milliseconds 250
    $released=Get-Status
    $releaseMarker=Get-TextMarker 'halo_motion_reticle_hide.active'
    $releaseFailures=[System.Collections.Generic.List[string]]::new()
    if([bool]$released.halo_motion_controls.two_hand_hold_active){$releaseFailures.Add('hold remained active after grip release')}
    if($releaseMarker -ne 'off'){$releaseFailures.Add("reticle hide marker remained '$releaseMarker'")}
    Add-CaseResult TWO-01 release @{
        two_hand_hold_active=[bool]$released.halo_motion_controls.two_hand_hold_active
        reticle_hide_marker=$releaseMarker
    } $releaseFailures.ToArray() @(
        'Pause, zoom, disable, tracking-loss release and grenade masking lack objective live ABI outputs.')
    }

    foreach($dpadMode in @($true,$false)) {
        $null=Invoke-OperatorJson 'UEVR_SetConfig' @{
            key='VR_DPadShifting'; value=($dpadMode.ToString().ToLower()); save=$false
        }
        $actual=(Invoke-OperatorJson 'UEVR_GetConfig' @{key='VR_DPadShifting'}).value
        Add-CaseResult INP-01 "dpad_mode_$($dpadMode.ToString().ToLower())" @{
            requested=$dpadMode; observed=$actual
        } @($(if($actual -ne $dpadMode.ToString().ToLower()){'live D-pad shifting config did not update'}))
    }
    $null=Invoke-OperatorJson 'UEVR_SetConfig' @{
        key='VR_DPadShifting'; value='false'; save=$false
    }
    foreach($stickCase in @(
        @{Name='inside_deadzone_x';X=0.11;Y=0.0;Last='X'},
        @{Name='outside_deadzone_x';X=0.13;Y=0.0;Last='X'},
        @{Name='half_positive_y';X=0.0;Y=0.5;Last='Y'},
        @{Name='full_negative_y';X=0.0;Y=-1.0;Last='Y'})) {
        # Operator 205.1 exposes the thumbstick as two scalar writes, but each
        # write clears the other component.  Set the zero axis first so axial
        # values remain objectively testable; diagonal injection is recorded
        # below as an Operator ABI gap rather than misreported as a mod failure.
        if ($stickCase.Last -eq 'X') {
            Set-ControllerInput left Thumbstick $stickCase.Y Y
            Set-ControllerInput left Thumbstick $stickCase.X X
        } else {
            Set-ControllerInput left Thumbstick $stickCase.X X
            Set-ControllerInput left Thumbstick $stickCase.Y Y
        }
        Start-Sleep -Milliseconds 100
        $status=Get-Status
        $observedX=[double]$status.input.left_stick.x
        $observedY=[double]$status.input.left_stick.y
        $finalX=[int]$status.halo_motion_controls.pose_diagnostics.final_xinput_left_x
        $finalY=[int]$status.halo_motion_controls.pose_diagnostics.final_xinput_left_y
        $fail=[System.Collections.Generic.List[string]]::new()
        if([Math]::Abs($observedX-$stickCase.X) -gt 0.01 -or [Math]::Abs($observedY-$stickCase.Y) -gt 0.01){$fail.Add('Operator stick echo differed from command')}
        if($stickCase.Name -eq 'inside_deadzone_x') {
            if([Math]::Abs($finalX) -gt 1 -or [Math]::Abs($finalY) -gt 1){$fail.Add('inside-deadzone stick leaked into final Halo XInput state')}
        } else {
            if(-not [bool]$status.halo_motion_controls.locomotion_bridge_observed){$fail.Add('nonzero above-deadzone stick never reached Halo XInput callback')}
            if($stickCase.X -gt 0 -and $finalX -le 0){$fail.Add('positive X input did not reach final Halo XInput with positive sign')}
            if($stickCase.X -lt 0 -and $finalX -ge 0){$fail.Add('negative X input did not reach final Halo XInput with negative sign')}
            if($stickCase.Y -gt 0 -and $finalY -le 0){$fail.Add('positive Y input did not reach final Halo XInput with positive sign')}
            if($stickCase.Y -lt 0 -and $finalY -ge 0){$fail.Add('negative Y input did not reach final Halo XInput with negative sign')}
        }
        Set-ControllerInput left Thumbstick 0.0 X
        Set-ControllerInput left Thumbstick 0.0 Y
        Start-Sleep -Milliseconds 80
        $releasedStick=Get-Status
        $releasedOk=[Math]::Abs([double]$releasedStick.input.left_stick.x) -le 0.001 -and [Math]::Abs([double]$releasedStick.input.left_stick.y) -le 0.001
        if(-not $releasedOk){$fail.Add('left stick did not return to zero')}
        $releasedFinalX=[int]$releasedStick.halo_motion_controls.pose_diagnostics.final_xinput_left_x
        $releasedFinalY=[int]$releasedStick.halo_motion_controls.pose_diagnostics.final_xinput_left_y
        if([Math]::Abs($releasedFinalX) -gt 1 -or [Math]::Abs($releasedFinalY) -gt 1){$fail.Add('released stick remained nonzero in final Halo XInput state')}
        Add-CaseResult INP-01 $stickCase.Name @{
            commanded=@($stickCase.X,$stickCase.Y); observed=@($observedX,$observedY)
            final_xinput=@($finalX,$finalY)
            released=$releasedOk
            released_final_xinput=@($releasedFinalX,$releasedFinalY)
            locomotion_bridge_observed=[bool]$status.halo_motion_controls.locomotion_bridge_observed
        } $fail.ToArray()
    }

    Set-ControllerInput right Trigger 1.0
    Start-Sleep -Milliseconds 80
    $triggerPressed=Get-Status
    Set-ControllerInput right Trigger 0.0
    Start-Sleep -Milliseconds 100
    $triggerReleased=Get-Status
    $triggerFailures=[System.Collections.Generic.List[string]]::new()
    if(-not [bool]$triggerPressed.input.right_trigger){$triggerFailures.Add('right trigger press was not observed')}
    if([bool]$triggerReleased.input.right_trigger){$triggerFailures.Add('right trigger remained pressed after release')}
    Add-CaseResult INP-01 right_trigger_release @{
        pressed_observed=[bool]$triggerPressed.input.right_trigger
        released_observed=(-not [bool]$triggerReleased.input.right_trigger)
    } $triggerFailures.ToArray() @(
        'A/B/X/Y/Menu and physical-gamepad merge state are not exposed by the current status ABI.')
} catch {
    $fatal = @(
        $_.Exception.ToString(),
        $_.InvocationInfo.PositionMessage,
        $_.ScriptStackTrace) -join [Environment]::NewLine
} finally {
    try { Set-ControllerInput left Grip 0.0 } catch {}
    try { Set-ControllerInput right Trigger 0.0 } catch {}
    try { Set-ControllerInput left Thumbstick 0.0 X } catch {}
    try { Set-ControllerInput left Thumbstick 0.0 Y } catch {}
    if ($originalDPad) {
        try {
            $null=Invoke-OperatorJson 'UEVR_SetConfig' @{
                key='VR_DPadShifting'; value=$originalDPad; save=$false
            }
        } catch {}
    }
    if ($null -ne $original) {
        try {
            Set-ExplicitRig $original.Head.Position $original.Head.Orientation `
                $original['right-grip'] $original['right-aim'] `
                $original['left-grip'] $original['left-aim']
            Start-Sleep -Milliseconds 80
            foreach ($restoreSpec in @(
                @('right','grip',$original['right-grip']),
                @('right','aim',$original['right-aim']),
                @('left','grip',$original['left-grip']),
                @('left','aim',$original['left-aim']))) {
                $restoredPose = Invoke-OperatorJson `
                    'openxr_get_controller_pose' @{
                        base_space='local'; hand=$restoreSpec[0]
                        pose_type=$restoreSpec[1]
                    }
                $positionError = Get-Distance `
                    (ConvertTo-Vector3 $restoredPose.pose.position) `
                    $restoreSpec[2].Position
                $angleError = Get-QuaternionAngleDegrees `
                    (ConvertTo-Quaternion $restoredPose.pose.orientation) `
                    $restoreSpec[2].Orientation
                if ($positionError -gt $PosePositionToleranceMeters -or
                    $angleError -gt $PoseAngleToleranceDegrees) {
                    throw (
                        "restore verification failed for $($restoreSpec[0]) " +
                        "$($restoreSpec[1]): ${positionError}m/${angleError}deg")
                }
            }
        } catch {
            if (-not $fatal) { $fatal = "Restore failed: $($_.Exception)" }
        }
    }
    if ($null -ne $script:proxy -and -not $script:proxy.HasExited) {
        $script:proxy.Kill()
    }
}

$caseArray=@($script:results)
$gates=@()
foreach($id in @('HMD-01','HAND-01','TWO-01','INP-01')) {
    $cases=@($caseArray | Where-Object id -eq $id)
    $failed=@($cases | Where-Object { -not $_.passed })
    $classification = if($fatal){'not_run_fatal'}elseif($cases.Count -eq 0){'not_run'}elseif($failed.Count -gt 0){'failed'}elseif($id -eq 'HMD-01'){'passed'}else{'partial_pass_abi_gaps'}
    $gates += [pscustomobject]@{
        id=$id; classification=$classification
        passed_cases=$cases.Count-$failed.Count; failed_cases=$failed.Count
    }
}
$summary=[ordered]@{
    schema_version=1
    generated_utc=[DateTime]::UtcNow.ToString('o')
    target_game_pid=$ExpectedGamePid
    operator_port=$OperatorPort
    operator_port_owner=@($owners)
    game_executable=$game.ExecutablePath
    operator_package=$OperatorPackageRoot
    original_pose=$original
    fatal_error=$fatal
    gates=$gates
    cases=$caseArray
    unavailable_abi_gates=@(
        'shoulder, elbow, clavicle and per-vertex stretch geometry',
        'controller tracking-valid override for deterministic loss/reacquire',
        'runtime ARM_IK enable/disable without plugin reload',
        'two-hand pause, zoom, disable and grenade-consumption state',
        'final XINPUT_GAMEPAD stick/button state and physical-gamepad merge',
        'Operator 205.1 scalar thumbstick writes clear the other axis, preventing diagonal injection')
}
$jsonPath=Join-Path $OutputDirectory 'extended-validation-summary.json'
$summary | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $jsonPath -Encoding utf8

$markdown=@(
    '# Halo extended headless validation', '',
    "- Generated UTC: $($summary.generated_utc)",
    "- Halo PID: $ExpectedGamePid (verified owner of port $OperatorPort)",
    "- Fatal error: $(if($fatal){$fatal}else{'none'})", '',
    '## Gate summary', '')
foreach($gate in $gates){$markdown += "- $($gate.id): $($gate.classification), $($gate.passed_cases) passed, $($gate.failed_cases) failed"}
$markdown += @('', '## Cases', '')
foreach($case in $caseArray){
    $caseLabel = if ($case.passed) { 'PASS' } else { 'FAIL' }
    $failureText = if ($case.failures.Count) {
        ': ' + ($case.failures -join '; ')
    } else { '' }
    $markdown += "- [$caseLabel] $($case.id) $($case.name)$failureText"
}
$markdown += @('', '## ABI gaps', '')
foreach($gap in $summary.unavailable_abi_gates){$markdown += "- $gap"}
$markdownPath=Join-Path $OutputDirectory 'extended-validation-summary.md'
$markdown | Set-Content -LiteralPath $markdownPath -Encoding utf8

[pscustomobject]@{
    OutputDirectory=$OutputDirectory
    Json=$jsonPath
    Markdown=$markdownPath
    FatalError=$fatal
    PassedCases=@($caseArray|Where-Object passed).Count
    FailedCases=@($caseArray|Where-Object{-not $_.passed}).Count
} | Format-List

if($fatal -or @($caseArray|Where-Object{-not $_.passed}).Count -gt 0){exit 1}
