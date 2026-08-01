[CmdletBinding()]
param(
    [string]$Destination = (Join-Path $PSScriptRoot 'uevr'),
    [switch]$ForceRefresh
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$releaseTag =
    'nightly-01139-74b76bc9428a906cbdc69de3ebc1905fd0e9cc57'
$expectedRevision = '74b76bc9428a906cbdc69de3ebc1905fd0e9cc57'
$expectedZipSha256 =
    'FA6F590926F3622222969BDDD5C128D9FCB57B8EDB38CFAA550C4A3A50FA721B'
$downloadUrl =
    "https://github.com/praydog/UEVR-nightly/releases/download/$releaseTag/uevr.zip"

$Destination = [System.IO.Path]::GetFullPath($Destination)
$revisionPath = Join-Path $Destination 'revision.txt'
$injectorPath = Join-Path $Destination 'UEVRInjector.exe'
$backendPath = Join-Path $Destination 'UEVRBackend.dll'
$luaPath = Join-Path $Destination 'LuaVR.dll'
$currentRevision = if (Test-Path -LiteralPath $revisionPath -PathType Leaf) {
    [System.IO.File]::ReadAllText($revisionPath).Trim()
} else { '' }

if (-not $ForceRefresh -and $currentRevision -eq $expectedRevision -and
    (Test-Path -LiteralPath $injectorPath -PathType Leaf) -and
    (Test-Path -LiteralPath $backendPath -PathType Leaf) -and
    (Test-Path -LiteralPath $luaPath -PathType Leaf)) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'UEVRInjector.dll.config') `
        -Destination (Join-Path $Destination 'UEVRInjector.dll.config') -Force
    Write-Output "Official praydog UEVR $expectedRevision is already installed."
    return
}

$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    'HaloCEVR-UEVR-' + [guid]::NewGuid().ToString('N'))
$zipPath = Join-Path $temporaryRoot 'uevr.zip'
$extractRoot = Join-Path $temporaryRoot 'extracted'

try {
    $null = New-Item -ItemType Directory -Path $temporaryRoot
    Write-Output "Downloading official praydog UEVR $releaseTag..."
    Invoke-WebRequest -Uri $downloadUrl -OutFile $zipPath -UseBasicParsing
    $actualZipSha256 =
        (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
    if ($actualZipSha256 -ne $expectedZipSha256) {
        throw "Official UEVR ZIP hash mismatch: expected $expectedZipSha256, got $actualZipSha256"
    }

    Expand-Archive -LiteralPath $zipPath -DestinationPath $extractRoot
    $downloadedRevision = [System.IO.File]::ReadAllText(
        (Join-Path $extractRoot 'revision.txt')).Trim()
    if ($downloadedRevision -ne $expectedRevision) {
        throw "Official UEVR revision mismatch: expected $expectedRevision, got $downloadedRevision"
    }

    $required = @(
        'LuaVR.dll',
        'openvr_api.dll',
        'openxr_loader.dll',
        'revision.txt',
        'UEVRBackend.dll',
        'UEVRInjector.exe',
        'UEVRPluginNullifier.dll'
    )
    foreach ($name in $required) {
        $source = Join-Path $extractRoot $name
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Official UEVR archive is missing $name"
        }
    }

    $null = New-Item -ItemType Directory -Path $Destination -Force
    foreach ($name in $required) {
        Copy-Item -LiteralPath (Join-Path $extractRoot $name) `
            -Destination (Join-Path $Destination $name) -Force
    }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'UEVRInjector.dll.config') `
        -Destination (Join-Path $Destination 'UEVRInjector.dll.config') -Force
    @(
        'Upstream: https://github.com/praydog/UEVR',
        "Nightly: https://github.com/praydog/UEVR-nightly/releases/tag/$releaseTag",
        "Revision: $expectedRevision",
        "Downloaded ZIP SHA-256: $expectedZipSha256"
    ) | Set-Content -LiteralPath (Join-Path $Destination 'UPSTREAM-UEVR.txt') `
        -Encoding utf8

    Write-Output "Official praydog UEVR installed in $Destination"
    Write-Output "Verified ZIP SHA-256: $actualZipSha256"
} finally {
    $resolvedTemporaryRoot = [System.IO.Path]::GetFullPath($temporaryRoot)
    $systemTemporaryRoot = [System.IO.Path]::GetFullPath(
        [System.IO.Path]::GetTempPath())
    if ((Test-Path -LiteralPath $resolvedTemporaryRoot) -and
        $resolvedTemporaryRoot.StartsWith(
            $systemTemporaryRoot,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $resolvedTemporaryRoot -Recurse -Force
    }
}
