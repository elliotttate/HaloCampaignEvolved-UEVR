param(
    [string]$ScriptPath = (
        Join-Path $PSScriptRoot '..\tools\Invoke-HaloModValidation.ps1')
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
        'Top-level validation script has parser errors: ' +
        (($parseErrors | ForEach-Object {
            "line $($_.Extent.StartLineNumber): $($_.Message)"
        }) -join '; '))
}

$source = Get-Content -Raw -LiteralPath $resolved
foreach ($required in @(
    "'Extended'",
    'Invoke-ExtendedTier',
    'Invoke-HaloExtendedValidation.ps1',
    "`$Suite -in @('Extended', 'Full')",
    'RET-01',
    'RET-02',
    'RET-03',
    'RET-04',
    'Invoke-HaloReticleSweep.ps1',
    'reticle-sweep-summary.json',
    'partial_pass_abi_gaps',
    'Manual follow-ups')) {
    if (-not $source.Contains($required)) {
        throw "Top-level validation is missing required token '$required'."
    }
}
if ($source.Contains('--allow-geometry-only')) {
    throw 'Top-level release validation must not permit geometry-only reticles.'
}

$plan = & powershell -NoProfile -ExecutionPolicy Bypass `
    -File $resolved -Suite Plan 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) {
    throw "Top-level validation plan failed:`n$plan"
}
foreach ($id in @(
    'OFF-01', 'LIVE-01', 'POSE-01', 'POSE-02',
    'RET-01', 'RET-02', 'RET-03', 'RET-04',
    'HMD-01', 'HAND-01', 'TWO-01', 'INP-01',
    'WPN-01', 'LIFE-01', 'PERF-01', 'DEP-01', 'HMD-02')) {
    if (-not $plan.Contains($id)) {
        throw "Top-level validation plan omitted '$id'."
    }
}

Write-Output 'Halo top-level validation harness source checks passed.'
