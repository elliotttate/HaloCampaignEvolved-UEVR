[CmdletBinding()]
param(
    [string]$ProfileRoot = (
        Join-Path $env:APPDATA 'UnrealVRMod\HaloCampaignEvolved')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Set-KeyValueFile {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][System.Collections.IDictionary]$Settings
    )
    $utf8 = [System.Text.UTF8Encoding]::new($false)
    $text = if (Test-Path -LiteralPath $Path -PathType Leaf) {
        [System.IO.File]::ReadAllText($Path)
    } else { '' }
    $newline = if ($text -match '\r\n') { "`r`n" } else { "`n" }
    foreach ($entry in $Settings.GetEnumerator()) {
        $line = "$($entry.Key)=$($entry.Value)"
        $pattern = ('(?im)^[\t ]*' +
            [regex]::Escape([string]$entry.Key) + '[\t ]*=[^\r\n]*')
        if ([regex]::IsMatch($text, $pattern)) {
            $text = [regex]::Replace($text, $pattern, $line)
        } else {
            if ($text.Length -gt 0 -and -not $text.EndsWith("`n") -and
                -not $text.EndsWith("`r")) {
                $text += $newline
            }
            $text += $line + $newline
        }
    }
    $parent = Split-Path -Parent $Path
    $null = New-Item -ItemType Directory -Path $parent -Force
    [System.IO.File]::WriteAllText($Path, $text, $utf8)
}

function Backup-ProfileFile {
    param([string]$Name, [string]$BackupRoot)
    $source = Join-Path $ProfileRoot $Name
    $backup = Join-Path $BackupRoot $Name
    $missing = "$backup.missing"
    if ((Test-Path -LiteralPath $backup) -or (Test-Path -LiteralPath $missing)) {
        return
    }
    if (Test-Path -LiteralPath $source -PathType Leaf) {
        Copy-Item -LiteralPath $source -Destination $backup
    } else {
        New-Item -ItemType File -Path $missing | Out-Null
    }
}

$pluginSource = Join-Path $PSScriptRoot 'plugins\HaloCEMotionControls.dll'
$luaSource = Join-Path $PSScriptRoot 'scripts\halo_motion_reticle.lua'
foreach ($path in @($pluginSource, $luaSource)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Package payload is missing: $path"
    }
}

$ProfileRoot = [System.IO.Path]::GetFullPath($ProfileRoot)
$null = New-Item -ItemType Directory -Path $ProfileRoot -Force
$backupRoot = Join-Path $ProfileRoot '.halo-cevr-backup'
$null = New-Item -ItemType Directory -Path $backupRoot -Force
Backup-ProfileFile -Name 'config.txt' -BackupRoot $backupRoot
Backup-ProfileFile -Name 'cameras.txt' -BackupRoot $backupRoot

$pluginDestination = Join-Path $ProfileRoot 'plugins\HaloCEMotionControls.dll'
$luaDestination = Join-Path $ProfileRoot 'scripts\halo_motion_reticle.lua'
$null = New-Item -ItemType Directory -Path (Split-Path -Parent $pluginDestination) -Force
$null = New-Item -ItemType Directory -Path (Split-Path -Parent $luaDestination) -Force
Copy-Item -LiteralPath $pluginSource -Destination $pluginDestination -Force
Copy-Item -LiteralPath $luaSource -Destination $luaDestination -Force

$configPath = Join-Path $ProfileRoot 'config.txt'
Set-KeyValueFile -Path $configPath -Settings ([ordered]@{
    Frontend_RequestedRuntime = 'openxr_loader.dll'
    VR_ControllersAllowed = 'true'
    VR_ForceMotionControlsActive = 'true'
    VR_AimMethod = '0'
    VR_AimModifyPlayerControlRotation = 'false'
    VR_AimUsePawnControlRotation = 'false'
    VR_DecoupledPitch = 'false'
    VR_DecoupledPitchUIAdjust = 'false'
    VR_SwapControllerInputs = 'false'
    VR_WorldScale = '1.000000'
    UObjectHook_AttachLerpEnabled = 'false'
    VR_MetaXROperatorEnabled = 'false'
})

$cameraSettings = [ordered]@{}
foreach ($index in 0..2) {
    $cameraSettings["decoupled_pitch$index"] = 'false'
    $cameraSettings["decoupled_pitch_ui_adjust$index"] = 'false'
    $cameraSettings["world_scale$index"] = '1.000000'
}
$camerasPath = Join-Path $ProfileRoot 'cameras.txt'
Set-KeyValueFile -Path $camerasPath -Settings $cameraSettings

$pluginHash = (Get-FileHash -LiteralPath $pluginDestination -Algorithm SHA256).Hash
$luaHash = (Get-FileHash -LiteralPath $luaDestination -Algorithm SHA256).Hash
if ($pluginHash -ne (Get-FileHash -LiteralPath $pluginSource -Algorithm SHA256).Hash -or
    $luaHash -ne (Get-FileHash -LiteralPath $luaSource -Algorithm SHA256).Hash) {
    throw 'Installed payload hashes do not match the package.'
}

$record = [ordered]@{
    schema_version = 1
    installed_utc = [DateTime]::UtcNow.ToString('o')
    plugin_sha256 = $pluginHash
    lua_sha256 = $luaHash
    config_sha256 = (Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash
    cameras_sha256 = (Get-FileHash -LiteralPath $camerasPath -Algorithm SHA256).Hash
}
$record | ConvertTo-Json | Set-Content -LiteralPath (
    Join-Path $backupRoot 'install-record.json') -Encoding utf8

Write-Output "Halo CE UEVR installed and verified in $ProfileRoot"
Write-Output "Plugin SHA-256: $pluginHash"
Write-Output "Lua SHA-256: $luaHash"
