param(
    [string]$ScriptPath = (
        Join-Path $PSScriptRoot '..\tools\Invoke-HaloReticleSweep.ps1')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$resolved = (Resolve-Path -LiteralPath $ScriptPath).Path
$tokens = $null
$parseErrors = $null
[System.Management.Automation.Language.Parser]::ParseFile(
    $resolved,
    [ref]$tokens,
    [ref]$parseErrors) | Out-Null
if ($parseErrors.Count -gt 0) {
    throw (
        'Reticle-sweep script has parser errors: ' +
        (($parseErrors | ForEach-Object {
            "line $($_.Extent.StartLineNumber): $($_.Message)"
        }) -join '; '))
}

$source = Get-Content -Raw -LiteralPath $resolved
foreach ($required in @(
    '[int]$HaloPid = 0',
    '[switch]$PlanOnly',
    'Get-NetTCPConnection -State Listen -LocalPort',
    '$HaloPid = [int]$owners[0]',
    'analyze-rendered-reticle.py',
    'openxr_capture_composited_image',
    '--maximum-projection-error',
    '--maximum-stereo-error',
    'Restore-NeutralPose',
    'Get-FileHash')) {
    if (-not $source.Contains($required)) {
        throw "Reticle-sweep script is missing required token '$required'."
    }
}
if ($source.Contains('[int]$HaloPid = 11856') -or
    $source.Contains('final-reticle-sweep-20260731')) {
    throw 'Reticle sweep still contains stale session-specific defaults.'
}
if ($source.Contains('--allow-geometry-only')) {
    throw 'Release reticle sweep must require verifiable blue/cyan color.'
}

$plan = & powershell -NoProfile -ExecutionPolicy Bypass `
    -File $resolved -PlanOnly 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) {
    throw "Reticle-sweep plan failed:`n$plan"
}
foreach ($name in @(
    'neutral',
    'yaw_left_15',
    'yaw_right_15',
    'pitch_up_12',
    'pitch_down_12',
    'diagonal_up_right_roll')) {
    if (-not $plan.Contains($name)) {
        throw "Reticle-sweep plan omitted '$name'."
    }
}

Write-Output 'Halo reticle-sweep harness source checks passed.'
