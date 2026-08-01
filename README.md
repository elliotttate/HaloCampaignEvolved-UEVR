# Halo Campaign Evolved UEVR motion controls

This project gives `HaloCampaignEvolved.exe` a standalone right-hand 6DOF
weapon and native Blam projectile path. Current official praydog UEVR and the
two profile files in this project are sufficient at runtime; the UEVR MCP and
Meta XR Operator are development and validation tools, not runtime
dependencies.

The repeatable build, simulator, reticle, ballistics, lifecycle, performance,
standalone, and headset acceptance suites are documented in
[`TESTING.md`](TESTING.md). Run `tools\Invoke-HaloModValidation.ps1 -Suite
Plan` to list them without changing or connecting to a live session.
The steady-state and recovery-path cost audit is documented in
[`PERFORMANCE.md`](PERFORMANCE.md).

The optional extended debugging backend lives in
[UEVRMetaXROperator](https://github.com/elliotttate/UEVRMetaXROperator), but the
formal release runs on official [praydog/UEVR](https://github.com/praydog/UEVR).

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
   - moves the native muzzle marker to the controller-derived muzzle and aims
     it at the exact ten-metre controller-reticle point, removing the otherwise
     permanent grip-to-muzzle parallax;
   - rotates final projectile direction, up vector, inherited velocity, and
     tag-derived post-spawn velocity; and
   - recomputes the first normal collision sweep from Halo's actual sweep start
     to that stored reticle point while preserving its original length, then
     leaves later impact/substep sweeps alone; and
   - rotates every source-authored conical/spread ray by the same complete
     source-to-controller basis delta, preserving pellet separation instead
     of collapsing a shotgun cone onto one center direction.

This final sweep correction is important for instant first-tick hits. Merely
moving the gun or rotating the spawned object's velocity does not redirect the
collision query Halo has already prepared. One tracking snapshot is latched at
the outer fire call, so muzzle lookup, projectile creation, and the collision
sweep cannot accidentally mix controller samples from adjacent frames.

While the local player is zoomed, the native redirect is suspended entirely:
the marker rewrite is skipped, which leaves no corrections for the projectile
or sweep hooks, so scoped shots use Halo's stock screen-center aim. This
matches the stock crosshair, which is restored during zoom, and the floating
aim ball, which is hidden during zoom.

## Independent hands and locomotion

The first-person palette uses the exact 76-node hierarchy read from the shipped
Campaign Evolved tags. The weapon subtree (`7/8/22`) remains owned by the right
aim pose. By default, the visible arms use a torso-anchored solve ported from
RoboquestVR's arm rig: each shoulder hangs from a yaw-only head frame at fixed
human-proportioned offsets (behind, below, and beside the view root), so head
pitch or roll never swings the shoulders into view — Halo's stock viewmodel
shoulders are camera-glued and would otherwise pitch with the player's face.
When a tracked hand is beyond the authored arm's reach, the arm root slides
toward the target by up to 12 cm (clavicle assist) before the remainder is
clamped. Two-bone IK then arranges the upper and lower arms on a temporary
palette, and each wrist with its exact hand/finger descendants receives a final
rigid transform that places the wrist exactly at its tracked target. The left
wrist target additionally sits a fixed offset behind the grip pose, so the
knuckles rather than the wrist bone land where the controller is held, and the
left hand's controller-to-bone axis mapping is latched once from the stock
palette instead of resampled per frame, which kept stock animation bleeding
into the tracked hand. The arm remains visual-only: its reach, shoulders, and
elbows can never pull, rotate, or limit either floating hand. Halo's authored
arm is about 0.635 m long, so extreme poses can still visually stretch the
connecting sleeve, but the controller-owned hand remains exact. Losing left
tracking leaves the stock left arm alone and does not invalidate right-hand
weapon aiming.

The split path is deliberately fail-open. If a palette contains unreasonable
matrices or either hand placement becomes degenerate, the plugin restores the
untouched stock palette and applies the previously validated rigid right-hand
transform for that frame. If the cosmetic IK solve alone fails, the exact
floating-wrist transform still runs. Set `UEVR_HALO_ARM_IK=0` before launch to
hide the arms entirely (floating hands: every non-hand arm bone collapses into
the tracked wrist). Note the collapse can show stretched dark triangles on
vertices skinned across both an arm bone and a preserved hand bone; the
anchored-arm default avoids this. The older
`UEVR_HALO_TWO_HAND_IK` name remains accepted for package compatibility.

Two-handed hold follows Halo-MCC-VR's headset-tuned barrel grab. Holding the
left grip button while the left palm is inside a thin cylinder along the
right aim ray (8–80 cm forward of the right grip, within 9 cm of the ray)
engages the hold; releasing the grip button ends it, and the zone only gates
acquisition. While engaged, the weapon's forward axis eases (0.15 s blend)
onto the line from the right grip to the left grip, with roll still taken
from the right controller. The same blended basis feeds the palette build and
the native marker/projectile/sweep hooks, so the rendered barrel, muzzle ray,
and collision sweep stay on one line. The two-hand influence fades out
smoothly across an agreement band (~60-70 degrees off the aim ray), so
cross-body poses ease back to one-handed aim instead of snapping, and if
left tracking drops mid-hold the blend tail eases out along the last tracked
line. Engaging pulses the left controller, and the hold only latches during
actual unpaused gameplay. The floating aim ball is hidden while two-handed
(shots no longer follow the right-controller ray it is anchored to) and
while zoomed. UEVR maps the left grip to the LEFT_SHOULDER gamepad button
(grenade throw in Halo), so that button is masked from Halo's XInput state
while the support hand is inside the grab zone or holding the grip. Set
`UEVR_HALO_TWO_HAND_HOLD=0` before launch to disable the feature.

The plugin also copies UEVR's left OpenXR joystick action into Halo's ordinary
left XInput stick with a small per-axis deadzone. It does not write guessed
Blam player-control offsets, so keyboard and physical-gamepad movement remain
intact. The runtime status flags expose left tracking (bit 5), enabled
two-hand IK (bit 6), whether the XInput locomotion callback has run
(bit 7) after a nonzero OpenXR stick value has actually been written into
Halo's XInput state, and an engaged two-handed hold (bit 8).

## Controller latency

On the optional API 2.40+ development backend, UEVR exposes one on-demand
OpenXR tracking snapshot containing the HMD plus both controllers'
grip and aim poses, all located at the same current predicted display time.
The native first-person palette hook requests this snapshot immediately before
rewriting Halo's render palette instead of reusing controller matrices cached
during the earlier game tick. The outermost local-fire hook requests the same
late source before latching its immutable shot sample, so the weapon, reticle,
muzzle, projectile, and collision sweep remain coherent.

Official praydog UEVR uses the public grip/aim pose API as the fully supported
fallback. OpenXR compositor timewarp
still corrects head motion only; this late-location path addresses the separate
controller latency that compositor reprojection cannot repair after the weapon
has already been drawn.

`scripts/halo_motion_reticle.lua` moves the live
`WBP_FirstPersonReticle_C` widget out of the flat HUD and into a stereo
world-space `WidgetComponent` ten metres down the right-controller ray. This
preserves Halo's authored reticle material, error cone, target colours,
firing/heat animation, and hitmarker bindings while removing the duplicate
screen-space copy. The distance keeps it beyond Halo's unusually large
first-person viewmodels. The native muzzle ray converges on this exact point
rather than merely running parallel to the grip-origin ray. The script has no
profile-library dependencies.
Do not copy this repository's experimental `scripts/libs` directory: those
general profile libraries register an incompatible XInput callback in the
current Operator-enabled UEVR build.

The reticle activates UObjectHook before creating its dynamic anchor,
hidden diagnostic sphere/light, and `WidgetComponent` so constructor/destructor
tracking includes every object; activating only after creation makes
`exists()` reject them forever. Its controller state uses UEVR's `permanent`
transform mode so UObjectHook does not enqueue a post-stereo restore to the old
transform. This flag does not retain the UObject across destruction.

The authored widget is detached only after its original HUD parent is cached.
The old sphere and light stay hidden for their entire lifetime; they are never
a runtime fallback reticle. The replacement proves the hosted widget for two
rendered frames before declaring its structure stable. Any setup failure
restores the authored widget to its original centered HUD slot;
gameplay/world teardown also removes the controller state and releases the
dynamic components before the next world is allowed to recreate them.

This package also fixes UEVR's controller-attachment view compensation.
With decoupled pitch disabled, hand-attached components now use the complete
inverse HMD view rotation. UEVR previously flattened that rotation
unconditionally, which let the reticle follow controller yaw but made it miss
controller/head pitch. Flattening is retained only when UEVR's decoupled-pitch
mode is deliberately enabled. Attachment interpolation is disabled for this
profile so the reticle and weapon do not visibly trail rapid controller moves.

Both UEVR and the native plugin intentionally calculate controller poses
relative to the HMD. In Meta XR Simulator, use **Plugins → Input →
Controllers Follow: Head** for a head-carried player-rig test. **Body**, or
controller poses still owned by Meta XR Operator from the current XR session,
keeps the controllers fixed in tracking space; moving only the simulated head
then correctly makes the hands move oppositely in the view. End the XR session
before switching from Operator-owned poses back to the Input plugin.

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

When the world reticle publishes the production
`right-controller world reticle ready` marker, the native plugin hides Halo's
stock centered CHUD crosshair only while unzoomed, controller tracking is
valid, and the native projectile hook is active. Halo rewrites the crosshair
visibility inside viewport drawing after UEVR's pre-draw callback, so the
plugin hooks that late CHUD writer and reapplies the same scope-safe policy
after the stock function returns. Scope/zoom restores the exact stock values.

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

The [GitHub Releases](https://github.com/elliotttate/HaloCampaignEvolved-UEVR/releases)
package is one-step: extract it, run `Verify-Package.ps1`, then run
`Install-HaloCEVR.ps1`. The installer downloads official praydog UEVR nightly
01139 directly from praydog, verifies its published SHA-256, and installs the
native plugin, Lua reticle, and profile settings. Meta XR Operator and MCP are
deliberately excluded from the runtime package.

For a real headset, select its OpenXR runtime and run `Start-HaloCEVR.ps1`.
It automates the Steam launch and official
`uevr\UEVRInjector.exe --attach=HaloCampaignEvolved.exe` path.

For a manual developer install, build the project and copy exactly these two
files into the per-game UEVR profile:

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
VR_MotionControlsInactivityTimer=100.000000
VR_DecoupledPitch=false
VR_DecoupledPitchUIAdjust=false
UI_ExternalCompositorQuad=true
VR_AimMethod=0
VR_AimModifyPlayerControlRotation=false
VR_AimUsePawnControlRotation=false
UObjectHook_AttachLerpEnabled=false
VR_MetaXROperatorEnabled=false
```

`prevent_controller_sleep=true` in the live `halo_ce_vr.cfg` is the
default. Meta XR Operator then keeps UEVR controller mode active indefinitely;
stock praydog UEVR is kept at its supported 100-second maximum. Halo's native
weapon-pose path reads OpenXR tracking independently of that stock input gate,
so the weapon continues tracking during a long stationary interval and the next
button or axis action wakes UEVR input immediately. A 0.5 Hz drift-only watchdog
repairs these settings if a profile loads late or another plugin changes them.
Operator's indefinite mode also makes UEVR prefer the VR controllers over a
physical gamepad for input arbitration and haptics. Set
`prevent_controller_sleep=false` to stop runtime enforcement; values changed by
the plugin are then restored if the user or another plugin has not superseded
them. Values already present in the UEVR profile remain profile-owned.
With `cutscene_comfort=true` in the live `halo_ce_vr.cfg`, the
native Halo cinematic detector also switches UEVR to 2D screen mode for the
duration of a cinematic and restores the user's prior display mode afterward.

The packaged launchers download and checksum the pinned official UEVR nightly,
perform the two profile file copies, verify both SHA-256 hashes, and upsert and
verify the required profile and camera values automatically.
`VR_AimMethod=0` is UEVR's `GAME` mode: controller tracking remains available
to the plugin without rotating Halo's game camera.

For the normal standalone path, run:

```powershell
.\Start-HaloCEVR.ps1
```

That path launches Halo through Steam and starts praydog's official auto-attach
injector without selecting or changing the system OpenXR runtime. Runtime
behavior comes only from official UEVR, the native profile plugin, and the Lua
reticle. Meta XR Operator remains an optional development and validation tool.

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
ctest --test-dir build -C Release --output-on-failure
```

The default configure uses an adjacent `UEVR-1.05-sdk` checkout or fetches the
official UEVR 1.05 tag only as the oldest plugin SDK ABI. Building against API
2.34 keeps the DLL loadable by current official UEVR builds; API 2.40-2.43
enhancements are detected at runtime and never raise the minimum version. Halo
itself requires a current UEVR scanner; the formal release pins the verified
official nightly 01139 rather than the old 1.05 runtime.

Override `UEVR_SDK_INCLUDE` only with the official UEVR 1.05 include directory:

```powershell
cmake -S . -B build -A x64 `
  -DUEVR_SDK_INCLUDE="C:\path\to\UEVR-1.05\include"
```

The release DLL is
`build\Release\HaloCEMotionControls.dll`.
