param(
    [string]$PoseMatrixPath = (
        Join-Path $PSScriptRoot '..\tools\Invoke-HaloPoseMatrixTest.ps1'),

    [string]$InvokeToolPath = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$resolvedScript = (Resolve-Path -LiteralPath $PoseMatrixPath).Path
$source = Get-Content -LiteralPath $resolvedScript -Raw
$tokens = $null
$parseErrors = $null
[void][System.Management.Automation.Language.Parser]::ParseFile(
    $resolvedScript,
    [ref]$tokens,
    [ref]$parseErrors)
if ($parseErrors.Count -ne 0) {
    throw "Pose-matrix script has $($parseErrors.Count) parse error(s)."
}

$scriptAst = [System.Management.Automation.Language.Parser]::ParseFile(
    $resolvedScript,
    [ref]$tokens,
    [ref]$parseErrors)
$shotGateAst = @(
    $scriptAst.FindAll(
        {
            param($node)
            $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
                $node.Name -eq 'Test-ShotMetrics'
        },
        $true)
) | Select-Object -First 1
if ($null -eq $shotGateAst) {
    throw 'Could not find Test-ShotMetrics in pose-matrix script.'
}
if ($shotGateAst.Extent.Text -match 'Reticle|CrossSpace') {
    throw 'Shot gate must not compare values across viewmodel/native spaces.'
}
Invoke-Expression $shotGateAst.Extent.Text

$PosePositionToleranceMeters = 0.005
$PoseAngleToleranceDegrees = 1.0
$WeaponPositionToleranceMeters = 0.02
$MinimumDirectionDot = 0.98
$MinimumBallisticDot = 0.9995
$MaximumMuzzleRayMissMeters = 0.01
$MaximumProjectileSpreadDegrees = 3.75
$MinimumTargetForwardDistanceMeters = 1.0
$DeterminantTolerance = 0.05
$passingShot = [pscustomobject][ordered]@{
    ShotSampleSequenceDelta = 1
    ShotSampleMarkerCountDelta = 1
    ShotSampleProjectileCountDelta = 1
    ShotSampleGripPositionErrorMeters = 0.0
    ShotSampleAimPositionErrorMeters = 0.0
    ShotSampleGripAngleErrorDegrees = 0.0
    ShotSampleAimAngleErrorDegrees = 0.0
    ShotSampleWeaponPositionErrorMeters = 0.0
    ShotSampleWeaponForwardExpectedDot = 1.0
    ShotSampleWeaponBarrelAimDot = 1.0
    ShotSampleWeaponUpExpectedDot = 1.0
    ShotSampleWeaponDeterminantError = 0.0
    ShotSampleWeaponOrthogonalityMaxError = 0.0
    ShotSampleRightWristDeterminantError = 0.0
    ShotSampleRightWristOrthogonalityMaxError = 0.0
    ShotSampleRightHandForwardWeaponBarrelDot = 1.0
    MuzzleTargetDot = 1.0
    MuzzleTargetForwardDistanceMeters = 10.0
    MuzzleTargetRayMissMeters = 0.0
    ProjectileTargetAngleDegrees = 3.0
    ProjectileTargetForwardDistanceMeters = 9.0
    ProjectileTargetRayMissMeters = 0.5
    ProjectileTargetRayMissLimitMeters = 0.6
    ProjectileForwardUpAbsDot = 0.0
    MuzzleDeterminantError = 0.0
    MuzzleOrthogonalityMaxError = 0.0
}
if (-not (Test-ShotMetrics -Metrics $passingShot)) {
    throw 'Known-good synthetic shot did not pass the shot gate.'
}
foreach ($failure in @(
    @{ Field = 'ShotSampleSequenceDelta'; Value = 0 },
    @{ Field = 'ShotSampleMarkerCountDelta'; Value = 2 },
    @{ Field = 'ShotSampleProjectileCountDelta'; Value = 0 },
    @{ Field = 'ShotSampleWeaponBarrelAimDot'; Value = 0.90 },
    @{ Field = 'MuzzleTargetDot'; Value = 0.99 },
    @{ Field = 'MuzzleTargetForwardDistanceMeters'; Value = 0.5 },
    @{ Field = 'MuzzleTargetRayMissMeters'; Value = 0.02 },
    @{ Field = 'ProjectileTargetAngleDegrees'; Value = 4.0 },
    @{ Field = 'ProjectileTargetForwardDistanceMeters'; Value = 0.0 },
    @{ Field = 'ProjectileTargetRayMissMeters'; Value = 0.7 })) {
    $candidateValues = [ordered]@{}
    foreach ($property in $passingShot.PSObject.Properties) {
        $candidateValues[$property.Name] = $property.Value
    }
    $candidateValues[$failure.Field] = $failure.Value
    if (Test-ShotMetrics -Metrics ([pscustomobject]$candidateValues)) {
        throw "Synthetic bad metric passed: $($failure.Field)"
    }
}

$requiredGateTokens = @(
    'Shot association became ambiguous',
    '$markerDelta -eq 1',
    '$projectileDelta -eq 1',
    '$Metrics.ShotSampleSequenceDelta -gt 0',
    '$Metrics.ShotSampleMarkerCountDelta -eq 1',
    '$Metrics.ShotSampleProjectileCountDelta -eq 1',
    '$Metrics.ShotSampleWeaponBarrelAimDot -ge $MinimumDirectionDot',
    '$Metrics.MuzzleTargetDot -ge $MinimumBallisticDot',
    '$Metrics.MuzzleTargetForwardDistanceMeters -ge',
    '$Metrics.MuzzleTargetRayMissMeters -le',
    '$Metrics.ProjectileTargetAngleDegrees -le',
    '$Metrics.ProjectileTargetForwardDistanceMeters -ge',
    '$Metrics.ProjectileTargetRayMissMeters -le',
    'ControllerReticleComparisonAvailable = $false',
    'CrossSpaceReticleAimDotDiagnostic',
    'a provably event-atomic native fire snapshot.'
)
foreach ($token in $requiredGateTokens) {
    if (-not $source.Contains($token)) {
        throw "Missing same-sample shot gate token: $token"
    }
}

$testRoot = Join-Path `
    ([System.IO.Path]::GetTempPath()) `
    ('halo-pose-matrix-plan-' + [Guid]::NewGuid().ToString('N'))
try {
    $arguments = @{
        Suite = 'Both'
        PlanOnly = $true
        OutputDirectory = $testRoot
    }
    if (-not [string]::IsNullOrWhiteSpace($InvokeToolPath)) {
        $arguments.InvokeToolPath = $InvokeToolPath
    }
    $null = & $resolvedScript @arguments

    $summaryPath = Join-Path $testRoot 'pose-matrix-summary.json'
    $csvPath = Join-Path $testRoot 'pose-matrix-summary.csv'
    $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
    if ($summary.schema_version -ne 2) {
        throw "Expected schema version 2, got $($summary.schema_version)."
    }
    if ($summary.case_count -ne 32) {
        throw "Expected 32 planned cases, got $($summary.case_count)."
    }
    if ($summary.shot_sample_coherence.exact_marker_delta -ne 1 -or
        $summary.shot_sample_coherence.exact_projectile_delta -ne 1 -or
        -not $summary.shot_sample_coherence.requires_newer_visual_sequence) {
        throw 'Shot-sample coherence metadata is incomplete.'
    }
    if ($summary.parameters.maximum_muzzle_ray_miss_meters -ne 0.01 -or
        $summary.parameters.minimum_target_forward_distance_meters -ne 1.0) {
        throw 'Native ray-distance defaults were not serialized.'
    }
    if ($summary.controller_reticle_same_shot_gate.available -or
        $summary.controller_reticle_same_shot_gate.required_native_telemetry.Count -ne 3) {
        throw 'Controller/reticle telemetry limitation is not explicit.'
    }

    $csvHeader = Get-Content -LiteralPath $csvPath -TotalCount 1
    foreach ($column in @(
        'shot_sample_sequence_delta',
        'shot_sample_weapon_barrel_aim_dot',
        'controller_reticle_comparison_available',
        'cross_space_reticle_aim_dot_diagnostic',
        'muzzle_target_forward_distance_m',
        'muzzle_target_ray_miss_m',
        'projectile_target_forward_distance_m',
        'projectile_target_ray_miss_m',
        'projectile_target_ray_miss_limit_m')) {
        if ($csvHeader -notmatch ('"' + [regex]::Escape($column) + '"')) {
            throw "Missing CSV metric column: $column"
        }
    }
} finally {
    if (Test-Path -LiteralPath $testRoot -PathType Container) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}

Write-Output 'Halo pose-matrix validation passed.'
