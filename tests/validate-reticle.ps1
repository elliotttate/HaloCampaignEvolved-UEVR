param(
    [Parameter(Mandatory = $true)]
    [string]$SourcePath,

    [Parameter(Mandatory = $true)]
    [string]$LuaPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$source = Get-Content -LiteralPath $SourcePath -Raw
$lua = Get-Content -LiteralPath $LuaPath -Raw
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$validationWrapper = Join-Path $repoRoot 'tools\Invoke-HaloModValidation.ps1'
$renderedAnalyzer = Join-Path $PSScriptRoot 'analyze-rendered-reticle.py'
$renderedAnalyzerTests = Join-Path $PSScriptRoot `
    'test-rendered-reticle-analyzer.py'
$rawAnalyzerTests = Join-Path $PSScriptRoot 'test-reticle-analyzer.py'

function Require-Token {
    param(
        [Parameter(Mandatory)]
        [string]$Text,
        [Parameter(Mandatory)]
        [string]$Token,
        [Parameter(Mandatory)]
        [string]$Description
    )

    if (-not $Text.Contains($Token)) {
        throw "Missing $Description token: $Token"
    }
}

function Reject-Token {
    param(
        [Parameter(Mandatory)]
        [string]$Text,
        [Parameter(Mandatory)]
        [string]$Token,
        [Parameter(Mandatory)]
        [string]$Description
    )

    if ($Text.Contains($Token)) {
        throw "Forbidden $Description token remains: $Token"
    }
}

foreach ($entry in @(
    @('bind_world_reticle_render_target', 'render-target binding helper'),
    @('find_active_world_reticle_component', 'live WidgetComponent ownership selector'),
    @('object->get_bool_property(L"bHiddenInGame")', 'stale hidden WidgetComponent rejection'),
    @('L"GetRenderTarget"', 'current WidgetComponent render target getter'),
    @('L"GetMaterialInstance"', 'current Widget3D material getter'),
    @('L"SetTextureParameterValue"', 'SlateUI texture setter'),
    @('L"K2_GetTextureParameterValue"', 'SlateUI binding verifier'),
    @('L"SlateUI"', 'Widget3D texture parameter'),
    @('kWorldReticleExposureCompensatedGain', 'exposure-compensated world-reticle gain'),
    @('kWorldReticleFallbackEmissiveGain', 'stock-material fallback gain'),
    @('WidgetVRPassThrough', 'EyeAdaptationInverse widget material detection'),
    @('L"SetTintColorAndOpacity"', 'Widget3D tint setter'),
    @('disable_world_reticle_depth_test', 'always-visible world-reticle depth override'),
    @('restore_world_reticle_depth_test', 'scoped depth-override restoration'),
    @('L"bDisableDepthTest"', 'Widget3D pass depth-test property'),
    @('set_world_reticle_draw_size(250, 250)', '250 by 250 authored draw size'),
    @('set_world_reticle_scale(kWorldReticleScale)', 'native scale enforcement'),
    @('get_dynamic_material(m_screen_reticle_image)', 'stock live reticle MID source'),
    @('get_brush_resource_object(m_world_reticle_image)', 'world brush identity verifier'),
    @('set_reticle_image_material(', 'world brush material setter'),
    @('m_world_reticle_image,', 'world Reticle_Image destination'),
    @('source_material))', 'exact source MID handoff'),
    @('set_reticle_image_opacity(m_screen_reticle_image, 0.0f)', 'screen-space source suppression'),
    @('m_shared_reticle_material = source_material', 'shared stock MID cache'),
    @('state.texture = m_world_reticle_render_target->to_handle()', 'compositor render-target publication'),
    @('weapon-specific world reticle retained', 'weapon material retention'),
    @('reticle_position - destination.position', 'muzzle convergence'),
    @('direction_override->reticle_position - *start', 'first sweep convergence')
)) {
    Require-Token -Text $source -Token $entry[0] -Description $entry[1]
}

# Halo's original WBP_FirstPersonReticle remains the only targeting/color
# authority. The plugin must share that exact live MID with the isolated world
# image, hide only the source pixels, and then publish the world render target.
$handoffTokens = @(
    'get_dynamic_material(m_screen_reticle_image)',
    'get_brush_resource_object(m_world_reticle_image)',
    'set_reticle_image_material(',
    'set_reticle_image_opacity(m_screen_reticle_image, 0.0f)',
    'm_shared_reticle_material = source_material',
    'state.texture = m_world_reticle_render_target->to_handle()')
$previousIndex = -1
foreach ($token in $handoffTokens) {
    $index = $source.IndexOf($token, $previousIndex + 1)
    if ($index -lt 0) {
        throw "Stock-reticle handoff token is missing or out of order: $token"
    }
    $previousIndex = $index
}

foreach ($entry in @(
    @('LineTraceSingle', 'duplicate Unreal targeting trace'),
    @('BreakHitResult', 'duplicate Unreal hit-result decoder'),
    @('ActorTeamIsFriendly', 'duplicate team classifier'),
    @('ActorTeamIsEnemy', 'duplicate team classifier'),
    @('OnChangedAimAssistTarget', 'synthetic stock-reticle event injection'),
    @('CurrentReticleTargetActor', 'synthetic target-state mutation'),
    @('update_world_reticle_target_color', 'post-tick reticle color override'),
    @('controller reticle target=', 'custom target/color diagnostic path')
)) {
    Reject-Token -Text $source -Token $entry[0] -Description $entry[1]
}

$sourceScaleMatch = [regex]::Match(
    $source,
    'constexpr\s+double\s+kWorldReticleScale\s*=\s*([0-9.]+)\s*;')
$luaScaleMatch = [regex]::Match(
    $lua,
    'world_reticle\.RelativeScale3D\s*=\s*' +
    'Vector3d\.new\(([0-9.]+),\s*([0-9.]+),\s*([0-9.]+)\)')
if (-not $sourceScaleMatch.Success -or -not $luaScaleMatch.Success) {
    throw 'Could not parse the native and Lua world-reticle scale contract.'
}
$sourceScale = [double]$sourceScaleMatch.Groups[1].Value
$luaScale = @(
    [double]$luaScaleMatch.Groups[1].Value,
    [double]$luaScaleMatch.Groups[2].Value,
    [double]$luaScaleMatch.Groups[3].Value)
if ($sourceScale -le 0.0 -or
    @($luaScale | Where-Object { $_ -le 0.0 }).Count -gt 0 -or
    @($luaScale | Where-Object {
        [Math]::Abs($_ - $sourceScale) -gt 0.0001
    }).Count -gt 0) {
    throw (
        'Native/Lua world-reticle scales disagree: native=' +
        "$sourceScale, Lua=$($luaScale -join ',').")
}

foreach ($entry in @(
    @('right-controller world reticle ready', 'production ready marker'),
    @('halo_motion_gameplay.active', 'gameplay lifetime gate'),
    @('halo_motion_reticle_hide.active', 'zoom/two-hand hide gate'),
    @('on_pre_viewport_client_draw', 'validated deferred-creation callback'),
    @('world_reticle:SetVisibility(not hide)', 'world widget visibility policy'),
    @('set_screen_reticle_suppressed(not hide)', 'stock reticle restoration policy'),
    @('old_world_reticle:SetWidget(nil)', 'widget teardown'),
    @('UEVR_UObjectHook.remove_motion_controller_state(old_anchor)', 'controller-state teardown'),
    @('reticle:SetVisibility(false)', 'sphere fallback suppression'),
    @('reticle_light:SetIntensity(0.0)', 'fallback light suppression'),
    @('reticle_light:SetVisibility(false)', 'fallback light visibility suppression')
)) {
    Require-Token -Text $lua -Token $entry[0] -Description $entry[1]
}

foreach ($entry in @(
    @('sphere fallback restored', 'sphere recovery path'),
    @('show_fallback', 'conditional fallback visibility')
)) {
    Reject-Token -Text $lua -Token $entry[0] -Description $entry[1]
}

if ($lua -match '(?m)^\s*reticle:SetVisibility\(true\)\s*$') {
    throw 'A visible sphere fallback path remains.'
}
if ($lua -match '(?m)^\s*reticle_light:SetVisibility\(true\)\s*$') {
    throw 'A visible fallback-light path remains.'
}

foreach ($path in @($validationWrapper, $renderedAnalyzer,
    $renderedAnalyzerTests, $rawAnalyzerTests)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing rendered-reticle validation file: $path"
    }
}
$wrapper = Get-Content -LiteralPath $validationWrapper -Raw
foreach ($entry in @(
    @('analyze-rendered-reticle.py', 'rendered-reticle analyzer invocation'),
    @('reticle-composited-analysis.json', 'rendered-reticle JSON artifact'),
    @('--left', 'left-eye analyzer argument'),
    @('--right', 'right-eye analyzer argument'),
    @('Rendered ring passed in both eyes', 'rendered-reticle pass marker')
)) {
    Require-Token -Text $wrapper -Token $entry[0] -Description $entry[1]
}

& py -3 $renderedAnalyzerTests
if ($LASTEXITCODE -ne 0) {
    throw "Rendered-reticle analyzer tests failed with exit code $LASTEXITCODE."
}
& py -3 $rawAnalyzerTests
if ($LASTEXITCODE -ne 0) {
    throw "Raw-reticle analyzer tests failed with exit code $LASTEXITCODE."
}

Write-Output 'Halo world-reticle source validation passed.'
