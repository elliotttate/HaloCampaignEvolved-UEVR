[CmdletBinding()]
param(
    [string]$ProfileRoot = (
        Join-Path $env:APPDATA 'UnrealVRMod\HaloCampaignEvolved'),
    [string]$GameRoot = '',
    [switch]$SkipOfficialUEVRDownload
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

function Test-HaloGameRoot {
    param([string]$Path)
    if (-not $Path) { return $false }
    $exe = Join-Path $Path (
        'Meteorite\Binaries\Win64\HaloCampaignEvolved.exe')
    return Test-Path -LiteralPath $exe -PathType Leaf
}

function Resolve-HaloGameRoot {
    param([string]$RequestedRoot)

    if ($RequestedRoot) {
        try {
            $requestedFullPath = [System.IO.Path]::GetFullPath($RequestedRoot)
        } catch {
            throw "GameRoot is not a valid path: '$RequestedRoot'."
        }
        if (Test-HaloGameRoot -Path $requestedFullPath) {
            return $requestedFullPath
        }
        throw "Halo Campaign Evolved was not found at GameRoot '$RequestedRoot'."
    }

    $candidates = [System.Collections.Generic.List[string]]::new()

    try {
        $running = Get-Process -Name HaloCampaignEvolved -ErrorAction Stop |
            Where-Object Path | Select-Object -First 1
        if ($running) {
            $candidate = Split-Path -Parent $running.Path
            foreach ($unused in 1..3) { $candidate = Split-Path -Parent $candidate }
            $candidates.Add($candidate)
        }
    } catch {}

    $steamRoots = [System.Collections.Generic.List[string]]::new()
    foreach ($key in @(
            'HKCU:\Software\Valve\Steam',
            'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam')) {
        try {
            $steam = Get-ItemProperty -LiteralPath $key -ErrorAction Stop
            foreach ($property in @('SteamPath', 'InstallPath')) {
                if ($steam.$property) { $steamRoots.Add([string]$steam.$property) }
            }
        } catch {}
    }

    foreach ($steamRoot in @($steamRoots)) {
        $libraryRoots = [System.Collections.Generic.List[string]]::new()
        $libraryRoots.Add($steamRoot)
        $libraryFile = Join-Path $steamRoot 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $libraryFile -PathType Leaf) {
            foreach ($match in [regex]::Matches(
                    [System.IO.File]::ReadAllText($libraryFile),
                    '(?im)"path"\s+"([^"]+)"')) {
                $libraryRoots.Add($match.Groups[1].Value.Replace('\\', '\'))
            }
        }
        foreach ($libraryRoot in @($libraryRoots)) {
            $manifest = Join-Path $libraryRoot 'steamapps\appmanifest_2806050.acf'
            $installDirectory = 'Halo Campaign Evolved'
            if (Test-Path -LiteralPath $manifest -PathType Leaf) {
                $manifestText = [System.IO.File]::ReadAllText($manifest)
                $match = [regex]::Match(
                    $manifestText, '(?im)"installdir"\s+"([^"]+)"')
                if ($match.Success) { $installDirectory = $match.Groups[1].Value }
            }
            $candidates.Add((Join-Path $libraryRoot (
                "steamapps\common\$installDirectory")))
        }
    }

    # Preserve the development machine's established default while still
    # discovering arbitrary Steam libraries first.
    $candidates.Add('E:\SteamLibrary\steamapps\common\Halo Campaign Evolved')

    foreach ($candidate in $candidates) {
        try { $fullPath = [System.IO.Path]::GetFullPath($candidate) } catch { continue }
        if (Test-HaloGameRoot -Path $fullPath) { return $fullPath }
    }
    throw 'Could not find Halo Campaign Evolved. Pass -GameRoot with the Steam game directory.'
}

function Backup-LogicModFile {
    param([string]$Name, [string]$BackupRoot, [string]$DestinationRoot)
    $source = Join-Path $DestinationRoot $Name
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
$logicModNames = @(
    'HaloCEReticleColor.pak',
    'HaloCEReticleColor.utoc',
    'HaloCEReticleColor.ucas'
)
$logicModSources = @($logicModNames | ForEach-Object {
    Join-Path $PSScriptRoot "logicmods\$_"
})
foreach ($path in @($pluginSource, $luaSource) + $logicModSources) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Package payload is missing: $path"
    }
}

if (-not $SkipOfficialUEVRDownload) {
    & (Join-Path $PSScriptRoot 'Install-OfficialUEVR.ps1')
}

$ProfileRoot = [System.IO.Path]::GetFullPath($ProfileRoot)
$GameRoot = Resolve-HaloGameRoot -RequestedRoot $GameRoot
$logicModRoot = Join-Path $GameRoot 'Meteorite\Content\Paks\LogicMods'
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

$logicModBackupRoot = Join-Path $backupRoot 'logicmods'
$null = New-Item -ItemType Directory -Path $logicModBackupRoot -Force
$null = New-Item -ItemType Directory -Path $logicModRoot -Force
$logicModRecords = @()
foreach ($name in $logicModNames) {
    Backup-LogicModFile -Name $name -BackupRoot $logicModBackupRoot `
        -DestinationRoot $logicModRoot
    $source = Join-Path $PSScriptRoot "logicmods\$name"
    $destination = Join-Path $logicModRoot $name
    Copy-Item -LiteralPath $source -Destination $destination -Force
    $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    $installedHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
    if ($installedHash -ne $sourceHash) {
        throw "Installed LogicMod hash does not match the package: $name"
    }
    $logicModRecords += [ordered]@{
        name = $name
        sha256 = $installedHash
    }
}

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
    UI_ExternalCompositorQuad = 'true'
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
    schema_version = 2
    installed_utc = [DateTime]::UtcNow.ToString('o')
    plugin_sha256 = $pluginHash
    lua_sha256 = $luaHash
    config_sha256 = (Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash
    cameras_sha256 = (Get-FileHash -LiteralPath $camerasPath -Algorithm SHA256).Hash
    game_root = $GameRoot
    logicmod_root = $logicModRoot
    logicmod_files = $logicModRecords
}
$record | ConvertTo-Json | Set-Content -LiteralPath (
    Join-Path $backupRoot 'install-record.json') -Encoding utf8

Write-Output "Halo CE UEVR installed and verified in $ProfileRoot"
Write-Output "Plugin SHA-256: $pluginHash"
Write-Output "Lua SHA-256: $luaHash"
Write-Output "Reticle LogicMod: $logicModRoot"
