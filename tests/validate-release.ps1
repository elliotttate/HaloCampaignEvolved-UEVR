param(
    [Parameter(Mandatory)][string]$ReleaseRoot,
    [Parameter(Mandatory)][string]$PackagerPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$scripts = @(
    'Install-HaloCEVR.ps1',
    'Install-OfficialUEVR.ps1',
    'Start-HaloCEVR.ps1',
    'Uninstall-HaloCEVR.ps1',
    'Verify-Package.ps1'
)
foreach ($name in $scripts) {
    $path = Join-Path $ReleaseRoot $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing release script: $name"
    }
    $tokens = $null
    $parseErrors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile(
        $path, [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count -gt 0) {
        throw "PowerShell syntax error in ${name}: $($parseErrors[0].Message)"
    }
}

$bootstrap = Get-Content -Raw -LiteralPath (
    Join-Path $ReleaseRoot 'Install-OfficialUEVR.ps1')
foreach ($token in @(
        'https://github.com/praydog/UEVR-nightly/releases/download/',
        'nightly-01139-74b76bc9428a906cbdc69de3ebc1905fd0e9cc57',
        'FA6F590926F3622222969BDDD5C128D9FCB57B8EDB38CFAA550C4A3A50FA721B',
        'Get-FileHash',
        'UEVRInjector.dll.config')) {
    if (-not $bootstrap.Contains($token)) {
        throw "Official UEVR bootstrap is missing: $token"
    }
}

$start = Get-Content -Raw -LiteralPath (
    Join-Path $ReleaseRoot 'Start-HaloCEVR.ps1')
foreach ($token in @(
        "Join-Path `$PSScriptRoot 'uevr'",
        '--attach=HaloCampaignEvolved.exe',
        'Install-OfficialUEVR.ps1')) {
    if (-not $start.Contains($token)) {
        throw "Official UEVR launcher is missing: $token"
    }
}

$packager = Get-Content -Raw -LiteralPath $PackagerPath
if ($packager.Contains("'UEVRBackend.dll' =") -or
    $packager.Contains("'UEVRInjector.exe' =")) {
    throw 'Release packager must not redistribute upstream UEVR binaries.'
}
foreach ($token in @(
        'Install-OfficialUEVR.ps1',
        'upstream_uevr_zip_sha256',
        'plugin_required_api')) {
    if (-not $packager.Contains($token)) {
        throw "Release packager is missing: $token"
    }
}

Write-Output 'Official UEVR release integration validation passed.'
