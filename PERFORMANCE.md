# Performance audit

This audit describes the work added by HaloCEVR itself. It does not include the
base cost of Halo, UEVR stereo rendering, the OpenXR runtime, or the headset
compositor. Source inspection proves execution frequency and asymptotic cost;
only an on-headset capture can establish exact milliseconds for a particular
GPU, CPU, runtime, resolution, and level.

## Steady-state work

| Path | Frequency | Cost and reason |
| --- | --- | --- |
| Tracking and controller math | Once per engine tick | Constant-size pose reads, quaternion/vector math, hand smoothing, and two-hand state. No UObject-array scan or filesystem I/O. |
| Native first-person palette detour | Each Halo first-person palette build | The largest CPU path. It transforms and validates the fixed 76-node palette and may repeat for Halo's interpolation capture palette. All storage is fixed-size stack memory; there is no heap allocation or object discovery. This is required for late-tracked weapon and hand rendering. |
| OpenXR reticle publisher | Once per engine tick while visible | Constant-size pose math plus one extension-state update. The 250x250 WidgetComponent remains the texture producer, while its Unreal main-pass mesh is disabled. |
| XInput bridge | Each relevant XInput query | Constant-time button/axis operations. It reads an OpenXR joystick pose only when UEVR supplied no movement. |
| Stock CHUD suppression | Viewport draw and Halo's own crosshair writer | Reads/writes 16 known float fields. It does not search UObjects. |
| Projectile redirection | Local shots only | Bounded marker/projectile arrays and matrix math. Remote/network projectiles fail through without modification. |
| Cached Unreal maintenance | 4 Hz | Local-pawn/weapon checks, reticle property checks, and transition marker polling. Full UObject discovery is skipped after the required component and images are cached. |
| Live config reload | 0.5 Hz | Timestamp/read-if-changed check. Parsing and UEVR setting updates occur only after an actual file change. |
| UEVR compatibility and controller diagnostics | 0.5 Hz | Four low-frequency lookups through UEVR's small mod/option lists plus one constant-time controller-gate query. It writes only when profile drift is detected, including restoring the controller no-sleep policy. No UObject or filesystem scan. |
| Lua reticle lifetime check | Viewport draw | Cached UObjectHook lifetime tests and property comparisons. Marker files are polled every 30 callbacks; diagnostic marker output is refreshed every 180 callbacks. |

## Expensive recovery paths

Halo can carry roughly 300,000 live UObjects. Finding a missing WidgetComponent
or reticle image by walking `FUObjectArray` is therefore expensive even though
each individual test is simple.

The active implementation has two protections:

- Native C++ discovery is pointer-cached. A failed recovery uses progressive
  0.25, 0.5, 1, and 2 second delays instead of rescanning on every 4 Hz
  maintenance pass.
- Lua creation retries use progressive 30, 60, 120, 240, and 480 callback
  delays. A healthy active reticle does not rescan the object array.

A single recovery scan can still produce a visible hitch on a heavily populated
level. That is preferable to retaining stale UObjects or permanently giving up;
the backoff prevents a missing HUD from becoming sustained single-digit FPS.

## Removed recurring work

- UEVR aim, pitch, hand-swap, and inactivity invariants are checked twice per
  second instead of walking UEVR's mod-value collection every engine tick.
  The watchdog performs writes only when a profile or another plugin changes
  an invariant. This also repairs controller no-sleep settings after late
  profile reloads without creating per-frame work.
- Eight OpenXR action-name lookups are refreshed twice per second rather than
  every engine tick. This removes repeated temporary `std::string` construction
  in official UEVR while retaining session-recreation recovery.
- A retired 4 Hz UObject attachment cleanup was removed. The visual weapon is
  owned by the native Halo palette hook; no code populated that old attachment
  list, and the cleanup incorrectly cleared the native attachment status.
- Reticle marker reads and diagnostic writes were reduced to transition-usable
  cadences rather than near-frame cadence.

## GPU and memory cost

The persistent mod-owned rendering resource is a 250x250 world-reticle render
target plus one OpenXR compositor quad when the reticle is visible. The old
debug sphere, cylinder, and point light remain hidden. No per-frame render
target creation, material creation, UObject creation, or unbounded container
growth is present in the steady-state path.

The mod deliberately requests a reticle redraw only when the live weapon
reticle material changes. Requesting it every maintenance pass previously caused
Slate/D3D resource churn.

## Remaining measurement work

For exact cost, capture the same save and headset pose with the mod enabled and
disabled, holding resolution, runtime, reprojection, and level constant. Record
CPU game-thread time, render-thread time, GPU time, missed compositor frames,
and one-percent-low frame time. A source audit can identify risk and eliminate
unbounded work, but it cannot replace that controlled A/B measurement.
