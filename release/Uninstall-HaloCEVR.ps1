[CmdletBinding()]
param(
    [string]$ProfileRoot = (
        Join-Path $env:APPDATA 'UnrealVRMod\HaloCampaignEvolved'),
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProfileRoot = [System.IO.Path]::GetFullPath($ProfileRoot)
$backupRoot = Join-Path $ProfileRoot '.halo-cevr-backup'
$recordPath = Join-Path $backupRoot 'install-record.json'
$record = if (Test-Path -LiteralPath $recordPath -PathType Leaf) {
    Get-Content -Raw -LiteralPath $recordPath | ConvertFrom-Json
} else { $null }

function Remove-OwnedPayload {
    param([string]$RelativePath, [string]$ExpectedHash)
    $path = Join-Path $ProfileRoot $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if (-not $Force -and $ExpectedHash -and $actual -ne $ExpectedHash) {
        Write-Warning "Preserving modified payload: $path"
        return
    }
    Remove-Item -LiteralPath $path -Force
    Write-Output "Removed $path"
}

Remove-OwnedPayload -RelativePath 'plugins\HaloCEMotionControls.dll' `
    -ExpectedHash $(if ($record) { $record.plugin_sha256 } else { '' })
Remove-OwnedPayload -RelativePath 'scripts\halo_motion_reticle.lua' `
    -ExpectedHash $(if ($record) { $record.lua_sha256 } else { '' })

foreach ($name in @('config.txt', 'cameras.txt')) {
    $current = Join-Path $ProfileRoot $name
    $backup = Join-Path $backupRoot $name
    $missing = "$backup.missing"
    $recordProperty = if ($name -eq 'config.txt') {
        'config_sha256'
    } else { 'cameras_sha256' }
    $installedHash = if ($record) { $record.$recordProperty } else { '' }
    $safeToRestore = $Force -or -not (Test-Path -LiteralPath $current) -or
        ($installedHash -and
            (Get-FileHash -LiteralPath $current -Algorithm SHA256).Hash -eq
                $installedHash)
    if (-not $safeToRestore) {
        Write-Warning "Preserving subsequently modified profile file: $current"
        continue
    }
    if (Test-Path -LiteralPath $backup -PathType Leaf) {
        Copy-Item -LiteralPath $backup -Destination $current -Force
        Write-Output "Restored $current"
    } elseif (Test-Path -LiteralPath $missing -PathType Leaf) {
        if (Test-Path -LiteralPath $current -PathType Leaf) {
            Remove-Item -LiteralPath $current -Force
        }
        Write-Output "Removed package-created $current"
    }
}

Write-Output 'Halo CE UEVR uninstall complete.'
