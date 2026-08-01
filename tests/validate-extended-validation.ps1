param(
    [string]$ScriptPath = (
        Join-Path $PSScriptRoot '..\tools\Invoke-HaloExtendedValidation.ps1')
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
        'Extended validation script has parser errors: ' +
        (($parseErrors | ForEach-Object {
            "line $($_.Extent.StartLineNumber): $($_.Message)"
        }) -join '; '))
}

$source = Get-Content -Raw -LiteralPath $resolved
foreach ($required in @(
    'HMD-01',
    'HAND-01',
    'TWO-01',
    'INP-01',
    'openxr_set_head_pose',
    'openxr_set_controller_pose',
    'openxr_set_controller_input',
    'two_hand_hold_active',
    'locomotion_bridge_observed',
    'right_wrist_determinant_error',
    'left_wrist_determinant_error',
    'weapon_position_formula_error_m',
    'reticle_hide_marker',
    'unavailable_abi_gates',
    'Get-NetTCPConnection -State Listen -LocalPort',
    '$ExpectedGamePid = 0',
    '$ExpectedGamePid = [int]$owners[0]',
    'Set-ExplicitRig $original.Head.Position')) {
    if (-not $source.Contains($required)) {
        throw "Extended validation script is missing required token '$required'."
    }
}

if ($source.Contains('[int]$ExpectedGamePid = 11856')) {
    throw 'Extended validation still contains a stale session-specific PID.'
}

$plan = & powershell -NoProfile -ExecutionPolicy Bypass `
    -File $resolved -PlanOnly 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) {
    throw "Extended validation plan failed:`n$plan"
}
foreach ($id in @('HMD-01','HAND-01','TWO-01','INP-01')) {
    if (-not $plan.Contains($id)) {
        throw "Extended validation plan omitted $id."
    }
}

Write-Output 'Halo extended-validation harness source checks passed.'
