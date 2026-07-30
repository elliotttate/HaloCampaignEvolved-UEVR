# Halo Campaign Evolved UEVR motion controls

This project gives `HaloCampaignEvolved.exe` a standalone right-hand 6DOF
weapon and native Blam projectile path. The matching UEVR build and the two
profile files in this project are sufficient at runtime; the UEVR MCP and
Meta XR Operator are development and validation tools, not runtime
dependencies.

The matching UEVR source, launchers, and packaged standalone release are in
[UEVRMetaXROperator](https://github.com/elliotttate/UEVRMetaXROperator).

## What is redirected

The native plugin makes the right controller authoritative in three layers:

1. UEVR's global controller-to-camera aim is forced off (`GAME` aim mode).
   The plugin reads the raw right-hand grip and aim poses directly, so moving
   the weapon cannot rotate Halo's camera or make fixed HUD elements
   counter-move. UEVR's decoupled-pitch and UI-compensation modes are also
   forced off because this plugin owns weapon pitch independently.
2. Halo's native 76-node first-person arm/weapon palette is transformed from
   the right grip and aim poses. This moves the rendered weapon without relying
   on the Unreal presentation actor alone. The visual hook also normalizes
   Halo's authored `primaryweapon` attachment (`+Y` barrel direction) to the
   `primary_trigger` ballistic basis (`+X`), so the visible barrel and native
   projectile ray share the same controller-forward axis.
3. The local Blam fire path in `HaloSimulation_tag_release.dll` is hooked at
   trigger creation, muzzle-marker lookup, final projectile creation, and the
   authoritative first-tick collision sweep. The plugin:
   - moves the native muzzle marker to the controller-derived muzzle;
   - rotates final projectile direction, up vector, inherited velocity, and
     tag-derived post-spawn velocity; and
   - rotates the first collision sweep while preserving its original length;
     and
   - rotates every source-authored conical/spread ray by the same complete
     source-to-controller basis delta, preserving pellet separation instead
     of collapsing a shotgun cone onto one center direction.

This final sweep correction is important for instant first-tick hits. Merely
moving the gun or rotating the spawned object's velocity does not redirect the
collision query Halo has already prepared.

## Independent hands and locomotion

The first-person palette now uses the exact 76-node hierarchy read from the
shipped Campaign Evolved tags. The weapon subtree (`7/8/22`) remains owned by
the right aim pose, while the right shoulder/elbow/wrist (`5/16/19`) and left
shoulder/elbow/wrist (`6/9/25`) are solved independently with reach-clamped
two-bone IK. Each wrist rotation is then applied only to its exact hand and
finger descendants. Losing the left controller leaves the stock left arm
alone; it does not invalidate right-hand weapon aiming.

The split path is deliberately fail-open. If a palette contains unreasonable
matrices or either IK solve becomes degenerate, the plugin restores the
untouched stock palette and applies the previously validated rigid right-hand
transform for that frame. Set `UEVR_HALO_TWO_HAND_IK=0` before launch to force
that legacy path for diagnosis.

The plugin also copies UEVR's left OpenXR joystick action into Halo's ordinary
left XInput stick with a small per-axis deadzone. It does not write guessed
Blam player-control offsets, so keyboard and physical-gamepad movement remain
intact. The runtime status flags expose left tracking (bit 5), enabled
two-hand IK (bit 6), and whether the XInput locomotion callback has run
(bit 7) after a nonzero OpenXR stick value has actually been written into
Halo's XInput state.

`scripts/halo_motion_reticle.lua` creates a stereo world-space reticle ten
metres down the right-controller ray. The distance keeps the marker beyond
Halo's unusually large first-person viewmodels and minimizes grip/muzzle
parallax. It intentionally has no profile-library dependencies.
Do not copy this repository's experimental `scripts/libs` directory: those
general profile libraries register an incompatible XInput callback in the
current Operator-enabled UEVR build.

The reticle activates UObjectHook before creating its dynamic anchor and mesh
so constructor/destructor tracking includes both objects; activating only
after creation makes `exists()` reject them forever. Its controller state uses
UEVR's `permanent` transform mode so UObjectHook does not enqueue a
post-stereo restore to the old transform. This flag does not retain the UObject
across destruction. The script explicitly removes the controller state and
hides/releases the mesh when gameplay ends or either tracked component becomes
invalid, then recreates both objects in the next gameplay world.

This package also fixes UEVR's controller-attachment view compensation.
With decoupled pitch disabled, hand-attached components now use the complete
inverse HMD view rotation. UEVR previously flattened that rotation
unconditionally, which let the reticle follow controller yaw but made it miss
controller/head pitch. Flattening is retained only when UEVR's decoupled-pitch
mode is deliberately enabled. Attachment interpolation is disabled for this
profile so the reticle and weapon do not visibly trail rapid controller moves.

Reticle creation is also gated by
`data\halo_motion_gameplay.active`. The native plugin writes `ready` only
after it resolves a valid local Blam weapon index, and writes `inactive` at
startup, menus, and other non-gameplay transitions. The Lua script therefore
does not create controller-owned scene components against the temporary menu
world or while a save is replacing the Unreal shell.

Deferred creation runs from `on_pre_viewport_client_draw` and retries every 60
rendered frames. This UEVR build does not dispatch Lua
`on_pre_engine_tick`, while viewport draw is also the callback used by the
validated native crosshair path. Moving reticle creation back to the engine
tick callback leaves the gameplay marker at `ready` but never creates the
reticle or publishes a diagnostic.

When the world reticle reports ready, the native plugin hides Halo's stock
center crosshair only while unzoomed, controller tracking is valid, and the
native projectile hook is active. Halo rewrites the crosshair visibility
inside viewport drawing after UEVR's pre-draw callback, so the plugin hooks
that late CHUD writer and reapplies the same scope-safe policy after the stock
function returns. Scope/zoom restores the exact stock values.

## Supported game build and fail-open behavior

The current native offsets and signatures target
`HaloSimulation_tag_release.dll` SHA-256:

`82B8A3A006BA3F981D6857DC7F4E4E929AE5282587F31F92F77A3FA78F4B2DAC`

The plugin refuses to arm outside `HaloCampaignEvolved.exe`. Inside Halo it
validates every fixed-RVA signature and direct-call target before installing
the native fire hooks:

| Native path | RVA |
| --- | ---: |
| Trigger projectile creation | `0x5CF460` |
| Muzzle-marker lookup | `0x5A43C0` |
| Final projectile creation | `0x5A0FB0` |
| Projectile collision sweep | `0x2C69F0` |
| Authoritative sweep callsite | `0x6485C7` |
| Conical/spread sweep callsite | `0x648E40` |
| First-person palette builder | `0x46EC10` |
| CHUD crosshair writer | `0x1EA950` |

If the core fire signatures or call targets do not match, no native projectile
hooks are installed and Halo keeps its stock fire path. The visual palette hook
is independently fail-open: a visual signature failure does not disable an
already validated projectile redirect. Crosshair hiding is also independent;
if the CHUD/zoom layouts fail validation, tracking is lost, the reticle is not
ready, or projectile redirection is unavailable, the stock crosshair remains
visible or is restored.

## Standalone installation

Copy exactly these two files into the per-game UEVR profile:

```text
%APPDATA%\UnrealVRMod\HaloCampaignEvolved\
|-- plugins\
|   `-- HaloCEMotionControls.dll
`-- scripts\
    `-- halo_motion_reticle.lua
```

The profile must contain these values in
`%APPDATA%\UnrealVRMod\HaloCampaignEvolved\config.txt`:

```ini
VR_ControllersAllowed=true
VR_ForceMotionControlsActive=true
VR_DecoupledPitch=false
VR_DecoupledPitchUIAdjust=false
VR_AimMethod=0
VR_AimModifyPlayerControlRotation=false
VR_AimUsePawnControlRotation=false
UObjectHook_AttachLerpEnabled=false
VR_MetaXROperatorEnabled=false
```

The packaged launchers perform the two file copies, verify both SHA-256 hashes,
and upsert and verify all nine values automatically.
`VR_AimMethod=0` is UEVR's `GAME` mode: controller tracking remains available
to the plugin without rotating Halo's game camera.

For the normal standalone path, run:

```powershell
.\Start-HaloCEVR-Standalone.ps1
```

That path selects and starts Meta XR Simulator, launches Halo through Steam,
and starts the official auto-attach injector. It deliberately removes this
package's Meta XR Operator explicit-layer registration and neither opens nor
waits for an MCP port. It also persists
`VR_MetaXROperatorEnabled=false`; the backend strips Operator from
`XR_ENABLE_API_LAYERS` and skips discovery even if a different Operator
package remains registered. Runtime behavior comes only from UEVR, the native
profile plugin, and the Lua reticle.

`Start-UEVRMetaXROperator.ps1` remains the development/validation launcher. It
loads the same standalone files but additionally enables Meta XR Operator so a
developer can inspect and drive the simulated session through MCP. That path
persists `VR_MetaXROperatorEnabled=true`.

Both launchers also rewrite all three saved UEVR camera presets so their
`decoupled_pitch` and `decoupled_pitch_ui_adjust` values remain false after a
preset switch. This is intentional: an old preset must not restore the
pitch-coupled HUD behavior.

For development, `UEVR_HALO_DEV_PLUGIN` or `UEVR_HALO_DEV_PLUGIN2` can name an
explicit DLL. The Operator-enabled standalone backend otherwise loads only
`HaloCEMotionControls.dll` from this game's profile and does not load arbitrary
global plugins or require `uevr_mcp.dll`.

## Build

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Override `UEVR_SDK_INCLUDE` at configure time if
`../uevr-mcp/plugin/include` is not the local UEVR plugin SDK path:

```powershell
cmake -S . -B build -A x64 `
  -DUEVR_SDK_INCLUDE="C:\path\to\uevr\plugin\include"
```

The release DLL is
`build\Release\HaloCEMotionControls.dll`.
