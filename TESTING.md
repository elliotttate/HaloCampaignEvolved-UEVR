# Halo Campaign Evolved UEVR test plan

This plan validates the exact repository build, the installed UEVR profile,
the Meta XR Simulator path, the native Blam hooks, the world-space authored
reticle, motion-controller transforms, firing, lifecycle behavior, and a real
headset. A green screenshot is not sufficient: every automated run records
machine-readable measurements and preserves the relevant images and logs.

## Entry point

Run commands from the repository root:

```powershell
cd E:\Github\HaloCampaignEvolved-UEVR
```

List the complete inventory without building, launching, or connecting:

```powershell
pwsh -NoProfile -File .\tools\Invoke-HaloModValidation.ps1 -Suite Plan
```

Run the fast offline gate after every source change:

```powershell
pwsh -NoProfile -File .\tools\Invoke-HaloModValidation.ps1 -Suite Offline
```

Run a non-firing live simulator smoke test after Halo is in a level:

```powershell
pwsh -NoProfile -File .\tools\Invoke-HaloModValidation.ps1 -Suite LiveSmoke
```

Run the full automated build, 25-case pose, seven-case fire, reticle-capture,
and package validation:

```powershell
pwsh -NoProfile -File .\tools\Invoke-HaloModValidation.ps1 -Suite Full
```

Run a ten-minute stability sample against an already-running session:

```powershell
pwsh -NoProfile -File .\tools\Invoke-HaloModValidation.ps1 `
    -Suite Soak `
    -SoakSeconds 600
```

Each non-plan run writes `validation-summary.json`, CSV, Markdown, command
logs, pose results, and captures beneath a timestamped directory in
`diagnostics\validation`. The command returns non-zero if any executed gate
fails. `SKIP` is explicit and never reported as a pass.

The runner defaults to the v3 Operator package. Use
`-OperatorPackageRoot`, `-ProfileRoot`, `-GameDll`, or `-BuildDirectory` to
pin another exact artifact. Do not let it auto-select an arbitrary nightly for
release evidence.

## Reticle-specific oracle

The world reticle must be Halo's authored `Reticle_Image`, not the old sphere,
light, cylinder, a grey default Widget3D material, or a newly drawn imitation.
Its raw render target is the reliable visual oracle because a composited-eye
screenshot is affected by scene color, depth, exposure, and occlusion.

Export the raw render target as PNG or EXR, then run:

```powershell
py -3 .\tests\analyze-reticle.py E:\Temp\halo-reticle.png --color cyan
```

Or include it in the full report:

```powershell
pwsh -NoProfile -File .\tools\Invoke-HaloModValidation.ps1 `
    -Suite Full `
    -ReticleImage E:\Temp\halo-reticle.png `
    -ReticleColor cyan
```

The oracle requires all of the following:

- exactly 250 by 250 pixels;
- exactly one visible connected component;
- a 28-32 pixel centered ring with 250-450 foreground pixels;
- centroid within one pixel of `(124.5, 124.5)`;
- an empty center rather than a filled ball;
- saturated cyan or red, depending on the requested state; and
- when `--reference-mask` is supplied, at least 0.90 foreground-mask IoU with the
  known-good authored ring.

For target-color validation, capture the same weapon and reticle twice:

1. Point at empty world geometry and require `--color cyan`.
2. Point at a hostile target and require `--color red`.
3. Move off the target and require cyan again within 200 ms.
4. Compare cyan and red masks with `--reference-mask`; color may change but ring
   geometry must not.

The full suite also captures both composited eyes. Review those images for one
small ring at the predicted ten-metre point, identical world placement in the
two eye views, no centered HUD duplicate, no sphere/cylinder, no mirror, and
no grey material substitution. A white tree or bright scene feature does not
count as reticle evidence.

## Test series and pass criteria

### 1. Offline build and compatibility gate

Run before opening the game.

| ID | Test | Pass condition |
|---|---|---|
| OFF-01 | Release build and CTest | Native build succeeds; source and reticle validations pass. |
| OFF-02 | Exact game build | `HaloSimulation_tag_release.dll` SHA-256 is `82B8A3A006BA3F981D6857DC7F4E4E929AE5282587F31F92F77A3FA78F4B2DAC`. |
| OFF-02 | Native hook scanner | Each implemented signature appears exactly once and the primary marker call resolves to `object_get_markers`. |
| OFF-03 | Plugin API | DLL exports `HaloCEVR_GetStatus` and `HaloCEVR_GetPoseDiagnostics`; Operator source exposes two-hand status bit 8. |
| OFF-04 | Profile policy | OpenXR, controllers, forced activity, aim method 0, no pawn/control rotation, no decoupled pitch, no input swap, world scale 1.0. |
| OFF-05 | Deployment parity | Repository build, package DLL, and active-profile DLL hashes match; repository, package, and profile Lua hashes match. |
| OFF-06 | Package integrity | Every entry in `SHA256SUMS.txt` exists and matches its recorded digest. |
| OFF-07 | Image oracle | Analyzer parses; an optional raw reticle image satisfies the strict ring oracle. |

Also run two deliberate negative checks before a release:

- change one byte in a copied game DLL and confirm OFF-02 refuses it;
- remove one copied signature region and confirm the scanner fails rather than
  partially installing native hooks.

Never perform those negative checks on the installed game DLL.

### 2. Cold boot and deterministic level setup

1. Confirm no stale `HaloCampaignEvolved.exe`, injector, Operator proxy, or XR
   session owns the previous run.
2. Launch Halo through Steam, inject the pinned UEVR build headlessly, and
   load the known campaign save.
3. Use `tools\Resume-HaloCampaignSave.ps1` when UEVR MCP is intentionally
   installed for setup. It calls the game's reflected campaign resume path and
   never bypasses `CanResumeCampaignSave`.
4. Wait for `halo_motion_gameplay.active=ready` and
   `halo_motion_reticle.active=right-controller world reticle ready`.
5. Confirm the weapon has ammunition before any firing matrix. A trigger pulse
   that advances neither marker nor projectile counters is an ammo/input setup
   failure, not proof of bad projectile math.

LIVE-01 passes only when the OpenXR session, Operator extension, HMD,
controllers, native hooks, visual attachment, projectile hook, local weapon,
and pose diagnostics are all ready. LIVE-02 passes only when gameplay and
world-reticle ownership markers are current and the Lua error marker is empty.

### 3. Right-controller 6DOF matrix

`Invoke-HaloPoseMatrixTest.ps1 -Suite FullNumeric` runs 25 independent cases:
neutral, positive/negative X/Y/Z translation, and positive/negative
yaw/pitch/roll for both right grip and right aim. Every case resets the other
pose to neutral so state cannot leak between cases.

Pass criteria:

- requested grip and aim position error no greater than 5 mm;
- requested orientation error no greater than 1 degree;
- expected-versus-rendered weapon position no greater than 2 cm;
- weapon/barrel/right-hand direction dot at least 0.98;
- weapon and right-wrist determinant and orthogonality error no greater than
  0.05; and
- diagnostic sequence and visual override counters advance after each command.

Before a release, add eight diagonal aim cases and compound translation plus
rotation sweeps. These catch axis-order or sign errors hidden by cardinal-only
tests.

### 4. HMD/controller independence

This is the regression test for hands moving opposite the player's head.

1. Record HMD, both controller poses, weapon, both wrists, and reticle.
2. Apply the same rigid translation and rotation to the HMD and both
   controllers. View-relative weapon, wrists, and reticle must remain stable
   within 5 mm and 1 degree.
3. Move only the HMD while controllers remain fixed in tracking space. The
   view-relative result must contain exactly one inverse-HMD transform, never
   zero and never twice.
4. Repeat ±yaw, ±pitch, ±roll, ±X/Y/Z, and two compound transforms.
5. Repeat with the simulator's `Controllers Follow: Head` policy to prove a
   carried player rig stays visually stable.

This matrix needs explicit HMD-command orchestration in the Operator test
harness before it can be a release-blocking automatic gate. Until then, run it
as a recorded simulator test and retain numerical status samples alongside the
video.

### 5. Floating hands and arm IK

Run with `UEVR_HALO_ARM_IK=0` first, then with arm IK enabled.

- Right and left wrist matrices must follow their respective grip poses with
  the same axis convention as the controller model.
- With arm IK disabled, wrists and hands remain authoritative floating hands;
  shoulder, elbow, and clavicle animation must not pull them away.
- With arm IK enabled, the wrist result must stay identical; only upstream arm
  nodes may solve toward it.
- Test close-to-face, cross-body, fully extended, behind-shoulder, above-head,
  and below-waist poses.
- No stretched triangles, NaNs, flipped determinant, or forearm scale is
  allowed. Clavicle assist stays below 12 cm.
- Loss of left or right tracking restores the stock subtree cleanly, then
  reacquires without a one-frame origin flash.

Add shoulder/elbow/clavicle matrices and fallback counters to pose diagnostics
before making every geometry assertion automatic. The wrist/basis assertions
are already available.

### 6. Two-hand behavior

Use the newly exposed `two_hand_hold_active` status bit.

1. Approach the support-hand zone from outside and prove no early latch.
2. Cross the acquisition radius and prove one latch transition.
3. Move inside the agreement band; weapon, muzzle, projectile, and sweep must
   use the same hand-to-hand basis.
4. Move just outside and back inside to verify hysteresis does not chatter.
5. Release the support input, lose left tracking, pause, zoom, and disable the
   feature independently; each must release the latch and restore the
   one-handed basis.
6. While latched, the world reticle hides, the stock reticle is restored, and
   the support input cannot also trigger a grenade action.

### 7. Locomotion and input

Command left-stick values at zero, just inside/outside deadzone, half-scale,
full-scale, and all diagonals. Verify sign and magnitude at the XInput bridge.
Repeat with D-pad shifting enabled and disabled. Physical gamepad input must be
preserved rather than doubled, and every synthesized button must return to the
released state after the test. Right trigger fire, grip, reload, zoom, melee,
and grenade actions must not remain stuck after Operator ownership ends.

### 8. Reticle structure, placement, and color

The structural runtime gate is:

- exactly one world `WidgetComponent` and one hosted `Reticle_Image`;
- draw size 250x250, pivot `(0.5,0.5)`, collision disabled, one-sided;
- non-null current render target with the same 250x250 size;
- Widget3D `SlateUI` parameter equals that current render target;
- world image brush uses the live Halo HUD material instance;
- original HUD image opacity is zero while unzoomed;
- old sphere and light are hidden, with light intensity zero;
- reticle position is ten metres down the effective controller ray with a
  direction dot of at least 0.9995;
- widget faces the HMD without mirroring; and
- the world pass disables depth only while the replacement is active, so the
  authored ring remains visible over nearer geometry and restores the shared
  pass material on teardown; and
- no second screen-space copy exists.

Rotate aim through cardinal, diagonal, roll, compound, and figure-eight paths.
At each stop, compare the numeric predicted world point to the rendered ring in
both eyes. Reticle lag may not exceed two display periods. Capture at least 60
consecutive frames and require the ring in at least 59, excluding an explicit
zoom/two-hand hide transition.

### 9. Ballistics and collision sweeps

`VisualFire` fires neutral, ±yaw, ±pitch, roll, and translated-grip cases. A
case passes only if marker and projectile counters both advance and:

- muzzle-to-reticle dot is at least 0.9995;
- projectile direction from its real spawn position remains inside the
  weapon's authored spread cone (3.5 degrees for the current assault-rifle
  tag, with a 0.25-degree measurement margin);
- projectile forward/up absolute dot is no greater than 0.001; and
- muzzle basis determinant and orthogonality remain valid.

The reticle, muzzle center ray, first-tick center sweep, and the center of the
projectile spread cone must all converge on the same stored point. Individual
assault-rifle bullets are expected to vary inside the authored 0.3-3.5 degree
error-angle bounds. Projectile-forward versus marker-forward is diagnostic
only because Halo spawns the projectile roughly a weapon-length away from the
marker and both rays converge on the same ten-metre point. A projectile merely
firing left/right or “not forward” is not a pass. A crosshair that visually
tracks the weapon while the cone center misses the shot is also not a pass.

Add exact corrected sweep vectors/counters to the public diagnostics before
making the sweep assertion independent of the projectile sample. Existing
three-decimal log text is supporting evidence, not a precision oracle.

### 10. Weapon and zoom matrix

At minimum test:

- pistol: hitscan-style single shot;
- assault rifle: automatic fire and reticle heat/spread animation;
- plasma weapon: slow visible projectile;
- shotgun: preserve its authored cone rather than collapsing pellets;
- sniper unzoomed; and
- sniper at every zoom level.

For zoom, the world reticle must hide, the exact stock scope/HUD must restore,
and zoomed shots must follow the intended stock path. Exiting zoom must restore
the authored world ring and native controller convergence. Cycle zoom at least
20 times and verify that component, root, callback, and object counts do not
grow.

### 11. Lifecycle and fail-open behavior

Exercise menu to gameplay, death/respawn, checkpoint reload, save replacement,
weapon switch, dropped weapon, cutscene, pause, zoom, Lua hot reload, and render
target replacement. After each transition:

- there is at most one anchor, sphere/light pair, WidgetComponent, and hosted
  widget;
- stale UObjects are not reused;
- the screen reticle is restored whenever the world replacement is invalid;
- tracking loss restores stock visuals and shot behavior;
- local-player hooks do not alter AI/remote projectiles; and
- callbacks, roots, handles, and private bytes do not grow per transition.

### 12. Performance and soak

Use a paired 30-second control and candidate capture three times in the same
scene and simulator pose. Candidate p95 CPU frame time must be within 10% or
1 ms of control, candidate p99 within 2 ms, and long-frame rate within one
percentage point. This catches a return of the full-UObject-array scan that
previously reduced the XR session to single-digit FPS.

The automated soak additionally requires pose sequences to keep advancing,
private bytes to grow no faster than the configured MB/min limit, and handles
to grow no faster than the configured handles/min limit. Treat an unpaired
soak as a leak screen; use the paired captures for a performance claim.

Process counters are sampled once per second, but the in-process UEVR status
surface is sampled only every 30 seconds by default. Polling status every
counter sample creates short-lived HTTP/plugin handles and can make the test
itself look like a game leak. Use `-SoakStatusSampleSeconds` to change that
cadence, and retain a process-only control when diagnosing a failed handle
trend.

### 13. Standalone dependency test

Cold-launch the standalone package with Meta XR Operator and UEVR MCP absent.
Confirm no Operator API layer in the Halo process, no `uevr_mcp.dll`, and no
listeners on the Operator/MCP ports. Then confirm tracking, weapon movement,
world reticle, and a controller-directed shot still work. The automation tools
are test infrastructure, not runtime dependencies.

### 14. Real-headset release smoke

Perform this last because simulator math cannot prove perceived latency,
comfort, stereo stability, or audio behavior.

- Move each hand along each axis and rotate yaw/pitch/roll; no direction may be
  inverted and hand palms must face forward rather than toward the player.
- Move only the head; hands must not counter-rotate or counter-translate.
- Aim at near/far/cardinal/diagonal targets and fire; the authored ring and
  projectile impact must agree.
- Inspect both hands at extreme reach for stretched geometry.
- Sweep quickly and check controller-to-weapon latency and reticle flicker.
- Test two-hand acquire/release and every zoom level.
- A/B weapon audio with the mod enabled/disabled; perceived level should not
  change, and a measured comparison should remain within 1 dB.

Record headset/runtime/model, refresh rate, UEVR package hash, plugin/Lua
hashes, weapon, level, and the validation-summary path with the result.

## Release rule

A release candidate is acceptable only when:

1. Offline is fully green with no package/profile drift.
2. Full is green with ammunition available and all seven firing cases
   observed.
3. The raw cyan and hostile-red reticle oracles pass.
4. Lifecycle and weapon/zoom matrices have no unresolved failure.
5. The paired performance comparison and soak pass.
6. Standalone and real-headset smoke tests pass.

Any missing diagnostic is recorded as an unautomated requirement; it is never
silently converted into a pass.
