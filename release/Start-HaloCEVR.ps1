[CmdletBinding()]
param(
    [string]$SteamAppId = '2806050',
    [int]$GameStartupTimeoutSeconds = 180,
    [switch]$Install,
    [switch]$RefreshOfficialUEVR
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($Install) {
    & (Join-Path $PSScriptRoot 'Install-HaloCEVR.ps1')
}

$uevrRoot = Join-Path $PSScriptRoot 'uevr'
$injector = Join-Path $uevrRoot 'UEVRInjector.exe'
if ($RefreshOfficialUEVR -or
    -not (Test-Path -LiteralPath $injector -PathType Leaf)) {
    & (Join-Path $PSScriptRoot 'Install-OfficialUEVR.ps1') `
        -Destination $uevrRoot -ForceRefresh:$RefreshOfficialUEVR
}

$halo = @(Get-Process -Name HaloCampaignEvolved -ErrorAction SilentlyContinue)
if ($halo.Count -gt 1) {
    throw 'More than one HaloCampaignEvolved process is running; close stale sessions first.'
}

if ($halo.Count -eq 0) {
    Start-Process "steam://rungameid/$SteamAppId"
    $deadline = [DateTime]::UtcNow.AddSeconds($GameStartupTimeoutSeconds)
    do {
        Start-Sleep -Milliseconds 500
        $halo = @(Get-Process -Name HaloCampaignEvolved -ErrorAction SilentlyContinue)
    } while ($halo.Count -eq 0 -and [DateTime]::UtcNow -lt $deadline)
}

if ($halo.Count -ne 1) {
    throw "Halo did not reach one running process within $GameStartupTimeoutSeconds seconds."
}

if (-not (Test-Path -LiteralPath $injector -PathType Leaf)) {
    throw "Official UEVR injector is missing: $injector"
}

Write-Output "Halo PID $($halo[0].Id) is running; starting official UEVR auto-attach."
Start-Process -FilePath $injector `
    -ArgumentList '--attach=HaloCampaignEvolved.exe' `
    -WorkingDirectory $uevrRoot `
    -WindowStyle Hidden
