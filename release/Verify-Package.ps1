[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$required = @(
    'UEVRInjector.exe',
    'UEVRBackend.dll',
    'LuaVR.dll',
    'openvr_api.dll',
    'openxr_loader.dll',
    'UEVRPluginNullifier.dll',
    'UEVRInjector.dll.config',
    'Start-HaloCEVR-Standalone.ps1',
    'Start-HaloCEVR-Core.ps1',
    'Install-HaloCEVR.ps1',
    'Uninstall-HaloCEVR.ps1',
    'plugins\HaloCEMotionControls.dll',
    'scripts\halo_motion_reticle.lua',
    'logicmods\HaloCEReticleColor.pak',
    'logicmods\HaloCEReticleColor.utoc',
    'logicmods\HaloCEReticleColor.ucas',
    'SHA256SUMS.txt'
)
foreach ($relativePath in $required) {
    $path = Join-Path $PSScriptRoot $relativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required package file is missing: $relativePath"
    }
}

foreach ($forbiddenGlobal in @('global.utoc', 'global.ucas')) {
    $match = Get-ChildItem -LiteralPath $PSScriptRoot -Recurse -File |
        Where-Object Name -EQ $forbiddenGlobal |
        Select-Object -First 1
    if ($match) {
        throw "Package must not contain game-global IoStore payload: $($match.FullName)"
    }
}

foreach ($forbidden in @(
        'uevr_mcp.dll',
        'meta-xr-operator-mcp-proxy.exe',
        'XrApiLayer_METAX_operator.dll')) {
    $match = Get-ChildItem -LiteralPath $PSScriptRoot -Recurse -File |
        Where-Object Name -EQ $forbidden |
        Select-Object -First 1
    if ($match) {
        throw "Standalone package unexpectedly contains $($match.FullName)"
    }
}

$checked = 0
foreach ($line in Get-Content -LiteralPath (Join-Path $PSScriptRoot 'SHA256SUMS.txt')) {
    if (-not $line.Trim()) { continue }
    if ($line -notmatch '^([0-9A-Fa-f]{64})\s{2}(.+)$') {
        throw "Malformed checksum line: $line"
    }
    $expected = $Matches[1].ToUpperInvariant()
    $relativePath = $Matches[2]
    if ([System.IO.Path]::IsPathRooted($relativePath) -or
        $relativePath.Split(@('\', '/')).Contains('..')) {
        throw "Unsafe checksum path: $relativePath"
    }
    $path = Join-Path $PSScriptRoot $relativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Checksum target is missing: $relativePath"
    }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if ($actual -ne $expected) {
        throw "Hash mismatch for ${relativePath}: expected $expected, got $actual"
    }
    $checked++
}

Write-Output "Halo CE UEVR package verification passed ($checked files)."
