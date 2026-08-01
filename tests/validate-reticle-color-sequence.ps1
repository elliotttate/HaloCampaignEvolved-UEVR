param(
    [string]$ScriptPath = (
        Join-Path $PSScriptRoot '..\tools\Test-HaloReticleColorSequence.ps1')
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
        'Reticle-color runner has parser errors: ' +
        (($parseErrors | ForEach-Object {
            "line $($_.Extent.StartLineNumber): $($_.Message)"
        }) -join '; '))
}

$source = Get-Content -Raw -LiteralPath $resolved
foreach ($required in @(
    'CyanImage',
    'HostileRedImage',
    'RecoveredCyanImage',
    "Color = 'cyan'",
    "Color = 'red'",
    '--reference-mask',
    '--minimum-mask-iou',
    "transition = @('cyan', 'red', 'cyan')",
    'reticle-color-summary.json')) {
    if (-not $source.Contains($required)) {
        throw "Reticle-color runner is missing required token '$required'."
    }
}

Write-Output 'Halo reticle-color sequence harness source checks passed.'
