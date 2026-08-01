[CmdletBinding()]
param(
    [ValidatePattern('^https?://')]
    [string]$UevrMcpBaseUri = 'http://127.0.0.1:8899',

    [ValidateRange(5, 600)]
    [int]$McpReadyTimeoutSeconds = 120,

    [ValidateRange(5, 900)]
    [int]$ResumeAvailableTimeoutSeconds = 180,

    [ValidateRange(5, 900)]
    [int]$GameplayReadyTimeoutSeconds = 180,

    [ValidateRange(100, 5000)]
    [int]$PollIntervalMilliseconds = 1000,

    [string]$SavePath = (
        Join-Path $env:LOCALAPPDATA 'Meteorite\Saved\SaveGames\CoreSave_0.sav'),

    [string]$GameplayMarkerPath = (
        Join-Path $env:APPDATA (
            'UnrealVRMod\HaloCampaignEvolved\data\' +
            'halo_motion_gameplay.active'))
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$baseUri = $UevrMcpBaseUri.TrimEnd('/')
$statusUri = "$baseUri/api/status"
$luaUri = "$baseUri/api/lua/exec"

$resumeLua = @'
local cls = mcp.find_class("/Script/Meteorite.MeteoriteUIStatics")
assert(cls, "MeteoriteUIStatics class not registered yet")

local cdo = cls:get_class_default_object()
assert(
    cdo and uevr.uobject_hook.exists(cdo),
    "MeteoriteUIStatics CDO invalid")

local pc = uevr.api.get_player_controller(0)
assert(
    pc and pc:get_address() ~= 0,
    "player controller not ready")

local flows = mcp.objects_of_class(
    "/Script/BlamEngine.BlamCampaignFlowGameSubsystem")
local flow = flows and flows[1]
assert(
    flow and uevr.uobject_hook.exists(flow),
    "BlamCampaignFlowGameSubsystem not ready")

local campaign = uevr.api.find_uobject(
    "BlamCampaignDataAsset " ..
    "/Game/Blueprints/Campaign/DA_FirstPlayableCampaign." ..
    "DA_FirstPlayableCampaign")
assert(
    campaign and uevr.uobject_hook.exists(campaign),
    "first playable campaign asset not ready")

local current_campaign = mcp.read_property(
    flow:get_address(),
    "CurrentCampaign")
if current_campaign ~= campaign:get_address() then
    flow:call("SetActiveCampaign", campaign)
    return "campaign_activated"
end

local can_resume = cdo:call("CanResumeCampaignSave", pc)
if can_resume and not halo_headless_resume_requested then
    halo_headless_resume_requested = true
    cdo:call("ResumeCampaignSave", pc)
    return "resume_requested"
end

return can_resume and "already_requested" or "save_not_ready"
'@

function Invoke-JsonPost {
    param(
        [Parameter(Mandatory)]
        [string]$Uri,

        [Parameter(Mandatory)]
        [hashtable]$Body,

        [ValidateRange(1, 60)]
        [int]$TimeoutSeconds = 15
    )

    $json = $Body | ConvertTo-Json -Depth 8 -Compress
    return Invoke-RestMethod `
        -Method Post `
        -Uri $Uri `
        -ContentType 'application/json' `
        -Body $json `
        -TimeoutSec $TimeoutSeconds
}

function Wait-UevrMcp {
    $deadline = (Get-Date).AddSeconds($McpReadyTimeoutSeconds)
    $lastError = $null

    do {
        try {
            return Invoke-RestMethod -Uri $statusUri -TimeoutSec 3
        } catch {
            $lastError = $_.Exception.Message
        }
        Start-Sleep -Milliseconds $PollIntervalMilliseconds
    } while ((Get-Date) -lt $deadline)

    throw (
        "UEVR MCP did not become ready at $statusUri within " +
        "$McpReadyTimeoutSeconds seconds. Last error: $lastError")
}

if (-not (Test-Path -LiteralPath $SavePath -PathType Leaf)) {
    throw "Campaign save was not found: $SavePath"
}

$scriptStartedUtc = [DateTime]::UtcNow
[void](Wait-UevrMcp)

$resumeDeadline = (Get-Date).AddSeconds($ResumeAvailableTimeoutSeconds)
$resumeResult = $null
$lastLuaError = $null

do {
    try {
        $response = Invoke-JsonPost `
            -Uri $luaUri `
            -Body @{
                code = $resumeLua
                timeout = 10000
            }

        if (-not $response.success) {
            $lastLuaError = $response.error
        } else {
            $resumeResult = [string]$response.result
            if (
                $resumeResult -eq 'resume_requested' -or
                $resumeResult -eq 'already_requested'
            ) {
                break
            }
        }
    } catch {
        $lastLuaError = $_.Exception.Message
    }

    Start-Sleep -Milliseconds $PollIntervalMilliseconds
} while ((Get-Date) -lt $resumeDeadline)

if (
    $resumeResult -ne 'resume_requested' -and
    $resumeResult -ne 'already_requested'
) {
    throw (
        'The game never reported that the campaign flow and save were safe ' +
        'to resume. ' +
        "Last Lua result: $resumeResult. Last error: $lastLuaError. " +
        'The script deliberately did not call ResumeCampaignSave until ' +
        'CurrentCampaign matched DA_FirstPlayableCampaign and ' +
        'CanResumeCampaignSave returned true.')
}

$gameplayDeadline = (Get-Date).AddSeconds($GameplayReadyTimeoutSeconds)
$markerState = 'missing'
$markerWriteUtc = [DateTime]::MinValue

do {
    if (Test-Path -LiteralPath $GameplayMarkerPath -PathType Leaf) {
        $marker = Get-Item -LiteralPath $GameplayMarkerPath
        $markerWriteUtc = $marker.LastWriteTimeUtc
        $markerState = (
            Get-Content -LiteralPath $GameplayMarkerPath -Raw).Trim()

        if (
            $markerState -eq 'ready' -and
            $markerWriteUtc -ge $scriptStartedUtc
        ) {
            break
        }
    }

    Start-Sleep -Milliseconds $PollIntervalMilliseconds
} while ((Get-Date) -lt $gameplayDeadline)

if (
    $markerState -ne 'ready' -or
    $markerWriteUtc -lt $scriptStartedUtc
) {
    throw (
        'ResumeCampaignSave was requested, but the motion-controls gameplay ' +
        "marker did not transition to a fresh 'ready' state within " +
        "$GameplayReadyTimeoutSeconds seconds. Marker: " +
        "$GameplayMarkerPath; state: $markerState; " +
        "last write UTC: $markerWriteUtc")
}

[pscustomobject]@{
    Result = $resumeResult
    SavePath = (Resolve-Path -LiteralPath $SavePath).Path
    GameplayMarkerPath = $GameplayMarkerPath
    GameplayMarkerState = $markerState
    GameplayMarkerWriteUtc = $markerWriteUtc
    UevrMcpBaseUri = $baseUri
}
