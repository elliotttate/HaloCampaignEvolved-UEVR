[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Version,
    [string]$BuildDirectory = '',
    [string]$OperatorPackageRoot = (
        'E:\Github\UEVRMetaXROperator\dist\release\' +
        'UEVR-Meta-XR-Operator-205.1-nightly-01139-analog-hands-v1'),
    [string]$OutputRoot = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $repoRoot 'build-next' }
if (-not $OutputRoot) { $OutputRoot = Join-Path $repoRoot 'dist\release' }
$packageName = "HaloCampaignEvolved-UEVR-Standalone-$Version"
$stageRoot = Join-Path $OutputRoot $packageName
$zipPath = Join-Path $OutputRoot "$packageName.zip"
$zipChecksumPath = "$zipPath.sha256"

foreach ($path in @($stageRoot, $zipPath, $zipChecksumPath)) {
    if (Test-Path -LiteralPath $path) {
        throw "Refusing to overwrite existing release output: $path"
    }
}
$null = New-Item -ItemType Directory -Path $stageRoot -Force
$null = New-Item -ItemType Directory -Path (Join-Path $stageRoot 'plugins') -Force
$null = New-Item -ItemType Directory -Path (Join-Path $stageRoot 'scripts') -Force
$null = New-Item -ItemType Directory -Path (Join-Path $stageRoot 'logicmods') -Force

$copyMap = [ordered]@{
    'LuaVR.dll' = 'LuaVR.dll'
    'openvr_api.dll' = 'openvr_api.dll'
    'openxr_loader.dll' = 'openxr_loader.dll'
    'revision.txt' = 'revision.txt'
    'UEVRBackend.dll' = 'UEVRBackend.dll'
    'UEVRInjector.dll.config' = 'UEVRInjector.dll.config'
    'UEVRInjector.exe' = 'UEVRInjector.exe'
    'UEVRPluginNullifier.dll' = 'UEVRPluginNullifier.dll'
    'Start-HaloCEVR-Standalone.ps1' = 'Start-HaloCEVR-Standalone.ps1'
    'Start-UEVRMetaXROperator.ps1' = 'Start-HaloCEVR-Core.ps1'
}
foreach ($entry in $copyMap.GetEnumerator()) {
    $source = Join-Path $OperatorPackageRoot $entry.Key
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Paired UEVR package is missing $($entry.Key): $source"
    }
    Copy-Item -LiteralPath $source -Destination (
        Join-Path $stageRoot $entry.Value)
}

$plugin = Join-Path $BuildDirectory 'Release\HaloCEMotionControls.dll'
if (-not (Test-Path -LiteralPath $plugin -PathType Leaf)) {
    throw "Release plugin was not built: $plugin"
}
Copy-Item -LiteralPath $plugin -Destination (
    Join-Path $stageRoot 'plugins\HaloCEMotionControls.dll')
Copy-Item -LiteralPath (Join-Path $repoRoot 'scripts\halo_motion_reticle.lua') `
    -Destination (Join-Path $stageRoot 'scripts\halo_motion_reticle.lua')
foreach ($name in @('HaloCEReticleColor.pak', 'HaloCEReticleColor.utoc',
        'HaloCEReticleColor.ucas')) {
    $source = Join-Path $repoRoot "logicmods\$name"
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Reticle LogicMod payload is missing: $source"
    }
    Copy-Item -LiteralPath $source -Destination (
        Join-Path $stageRoot "logicmods\$name")
}

foreach ($name in @('Install-HaloCEVR.ps1', 'Uninstall-HaloCEVR.ps1',
        'Verify-Package.ps1')) {
    Copy-Item -LiteralPath (Join-Path $repoRoot "release\$name") `
        -Destination (Join-Path $stageRoot $name)
}
Copy-Item -LiteralPath (Join-Path $repoRoot 'README.md') `
    -Destination (Join-Path $stageRoot 'HALO_MOTION_CONTROLS.md')
Copy-Item -LiteralPath (Join-Path $repoRoot 'TESTING.md') `
    -Destination (Join-Path $stageRoot 'TESTING.md')

$sourceCommit = (& git -C $repoRoot rev-parse HEAD).Trim()
$readme = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'release\README.md')
$readme = $readme.Replace('@VERSION@', $Version).Replace(
    '@SOURCE_COMMIT@', $sourceCommit)
[System.IO.File]::WriteAllText(
    (Join-Path $stageRoot 'README.md'),
    $readme,
    [System.Text.UTF8Encoding]::new($false))

$manifest = [ordered]@{
    schema_version = 2
    package = $packageName
    version = $Version
    source_repository = 'https://github.com/elliotttate/HaloCampaignEvolved-UEVR'
    source_commit = $sourceCommit
    paired_uevr_package = Split-Path -Leaf $OperatorPackageRoot
    runtime_model = 'standalone UEVR API 2.43; no MCP or Meta XR Operator dependency'
    plugin_sha256 = (Get-FileHash -LiteralPath (
        Join-Path $stageRoot 'plugins\HaloCEMotionControls.dll') -Algorithm SHA256).Hash
    reticle_lua_sha256 = (Get-FileHash -LiteralPath (
        Join-Path $stageRoot 'scripts\halo_motion_reticle.lua') -Algorithm SHA256).Hash
    uevr_backend_sha256 = (Get-FileHash -LiteralPath (
        Join-Path $stageRoot 'UEVRBackend.dll') -Algorithm SHA256).Hash
    reticle_logicmod_sha256 = [ordered]@{
        pak = (Get-FileHash -LiteralPath (
            Join-Path $stageRoot 'logicmods\HaloCEReticleColor.pak') -Algorithm SHA256).Hash
        utoc = (Get-FileHash -LiteralPath (
            Join-Path $stageRoot 'logicmods\HaloCEReticleColor.utoc') -Algorithm SHA256).Hash
        ucas = (Get-FileHash -LiteralPath (
            Join-Path $stageRoot 'logicmods\HaloCEReticleColor.ucas') -Algorithm SHA256).Hash
    }
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (
    Join-Path $stageRoot 'RELEASE-MANIFEST.json') -Encoding utf8

$checksumLines = Get-ChildItem -LiteralPath $stageRoot -Recurse -File |
    Where-Object Name -NE 'SHA256SUMS.txt' |
    Sort-Object FullName |
    ForEach-Object {
        $relative = $_.FullName.Substring($stageRoot.Length + 1)
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        "$hash  $relative"
    }
$checksumLines | Set-Content -LiteralPath (
    Join-Path $stageRoot 'SHA256SUMS.txt') -Encoding ascii

& powershell -NoProfile -ExecutionPolicy Bypass -File (
    Join-Path $stageRoot 'Verify-Package.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Staged package verification failed.' }

Compress-Archive -Path (Join-Path $stageRoot '*') `
    -DestinationPath $zipPath -CompressionLevel Optimal
$zipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
"$zipHash  $(Split-Path -Leaf $zipPath)" |
    Set-Content -LiteralPath $zipChecksumPath -Encoding ascii

Write-Output "Package: $zipPath"
Write-Output "SHA-256: $zipHash"
Write-Output "Checksum: $zipChecksumPath"
