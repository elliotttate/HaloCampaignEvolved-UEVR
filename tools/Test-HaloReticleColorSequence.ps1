[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$CyanImage,

    [Parameter(Mandatory)]
    [string]$HostileRedImage,

    [Parameter(Mandatory)]
    [string]$RecoveredCyanImage,

    [string]$OutputDirectory = '',

    [ValidateRange(0.5, 1.0)]
    [double]$MinimumMaskIou = 0.90
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$analyzer = Join-Path $repoRoot 'tests\analyze-reticle.py'
$CyanImage = (Resolve-Path -LiteralPath $CyanImage).Path
$HostileRedImage = (Resolve-Path -LiteralPath $HostileRedImage).Path
$RecoveredCyanImage = (Resolve-Path -LiteralPath $RecoveredCyanImage).Path
if (-not $OutputDirectory) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputDirectory = Join-Path $repoRoot (
        "diagnostics\validation\reticle-color-$stamp")
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

$cases = @(
    [pscustomobject]@{
        Name = 'world-cyan'
        Image = $CyanImage
        Color = 'cyan'
        Reference = ''
    },
    [pscustomobject]@{
        Name = 'hostile-red'
        Image = $HostileRedImage
        Color = 'red'
        Reference = $CyanImage
    },
    [pscustomobject]@{
        Name = 'recovered-cyan'
        Image = $RecoveredCyanImage
        Color = 'cyan'
        Reference = $CyanImage
    })

$results = [Collections.Generic.List[object]]::new()
foreach ($case in $cases) {
    $jsonPath = Join-Path $OutputDirectory "$($case.Name)-analysis.json"
    $arguments = @(
        '-3', $analyzer, $case.Image,
        '--color', $case.Color,
        '--json', $jsonPath)
    if ($case.Reference) {
        $arguments += @(
            '--reference-mask', $case.Reference,
            '--minimum-mask-iou', $MinimumMaskIou)
    }
    $stdout = & py @arguments 2>&1 | Out-String
    $exitCode = $LASTEXITCODE
    $logPath = Join-Path $OutputDirectory "$($case.Name)-analysis.log"
    $stdout | Set-Content -LiteralPath $logPath -Encoding utf8
    if (-not (Test-Path -LiteralPath $jsonPath -PathType Leaf)) {
        throw "Reticle analyzer did not create '$jsonPath'. See '$logPath'."
    }
    $analysis = Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json
    $results.Add([pscustomobject][ordered]@{
        name = $case.Name
        expected_color = $case.Color
        image = $case.Image
        passed = ($exitCode -eq 0 -and [bool]$analysis.passed)
        exit_code = $exitCode
        reference_mask_iou = if (
            $analysis.PSObject.Properties['reference_mask_iou']) {
            [double]$analysis.reference_mask_iou
        } else {
            $null
        }
        failures = @($analysis.failures)
        analysis = $jsonPath
        log = $logPath
    })
}

$failed = @($results | Where-Object { -not $_.passed })
$summary = [pscustomobject][ordered]@{
    schema_version = 1
    generated_utc = [DateTime]::UtcNow.ToString('o')
    passed = $failed.Count -eq 0
    minimum_mask_iou = $MinimumMaskIou
    transition = @('cyan', 'red', 'cyan')
    cases = @($results)
}
$summaryPath = Join-Path $OutputDirectory 'reticle-color-summary.json'
$markdownPath = Join-Path $OutputDirectory 'reticle-color-summary.md'
$summary | ConvertTo-Json -Depth 20 |
    Set-Content -LiteralPath $summaryPath -Encoding utf8

$markdown = @(
    '# Halo reticle color sequence', '',
    "- Passed: $($summary.passed)",
    "- Minimum mask IoU: $MinimumMaskIou", '',
    '| Stage | Expected | Result | Mask IoU | Evidence |',
    '|---|---|---:|---:|---|')
foreach ($result in $results) {
    $label = if ($result.passed) { 'PASS' } else { 'FAIL' }
    $iou = if ($null -eq $result.reference_mask_iou) {
        'baseline'
    } else {
        [Math]::Round($result.reference_mask_iou, 4)
    }
    $markdown += (
        "| $($result.name) | $($result.expected_color) | $label | " +
        "$iou | $($result.analysis) |")
}
$markdown | Set-Content -LiteralPath $markdownPath -Encoding utf8

$summary
if ($failed.Count -gt 0) {
    exit 1
}
