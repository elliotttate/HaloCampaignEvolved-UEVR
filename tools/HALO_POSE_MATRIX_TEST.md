# Deterministic Meta XR pose-matrix test

`Invoke-HaloPoseMatrixTest.ps1` drives Meta XR Simulator through the packaged
`Invoke-MetaXROperatorTool.ps1`. It does not load or copy an MCP client and it
does not modify UEVR or the Halo native plugin.

The test independently varies the right grip and right aim poses. The unchanged
pose is reset to neutral before the varied pose is commanded, so stale
simulator state cannot make an aim test accidentally depend on grip state (or
vice versa).

## Suites

`FullNumeric` runs 25 deterministic cases:

- neutral;
- positive and negative X/Y/Z translation for grip and aim; and
- positive and negative yaw, pitch, and roll for grip and aim.

Yaw is rotation around OpenXR `+Y`, pitch around `+X`, and roll around `+Z`.
The default rotation is 30 degrees and translation is 0.15 metres.

`VisualFire` runs a seven-case subset: neutral, positive/negative aim yaw,
positive/negative aim pitch, positive aim roll, and positive grip-X
translation. It saves a composited-eye screenshot and fires one native shot in
each case.

`Both` runs the numeric suite followed by the screenshot/fire subset.

## Runtime requirements

Launch Halo with the Operator-enabled UEVR package, load a first-person level
with a usable weapon, and wait for the native Halo hooks to become ready. The
script requires:

- an active OpenXR session with the Operator extension;
- `HaloCEMotionControls.dll` and its status API;
- valid controller tracking and the native first-person visual hook; and
- for `VisualFire`, the native projectile hook and available ammunition.

By default the script discovers the newest complete package beneath:

`E:\Github\UEVRMetaXROperator\dist\release`

Pass `-OperatorPackageRoot` to pin a particular packaged build. The directory
must contain both `Invoke-MetaXROperatorTool.ps1` and
`meta-xr-operator\windows\meta-xr-operator-mcp-proxy.exe`.

## Usage

Run the full non-firing numeric matrix:

```powershell
pwsh -NoProfile -File .\tools\Invoke-HaloPoseMatrixTest.ps1 `
    -Suite FullNumeric
```

Run the visible screenshot-and-fire subset:

```powershell
pwsh -NoProfile -File .\tools\Invoke-HaloPoseMatrixTest.ps1 `
    -Suite VisualFire
```

Run both and pin the current v3 Operator package:

```powershell
pwsh -NoProfile -File .\tools\Invoke-HaloPoseMatrixTest.ps1 `
    -Suite Both `
    -OperatorPackageRoot `
      'E:\Github\UEVRMetaXROperator\dist\release\UEVR-Meta-XR-Operator-205.1-nightly-01139-analog-hands-v1'
```

Preview the generated case matrix without connecting to a running session:

```powershell
pwsh -NoProfile -File .\tools\Invoke-HaloPoseMatrixTest.ps1 `
    -Suite Both `
    -PlanOnly `
    -OutputDirectory 'E:\Temp\halo-pose-plan'
```

Results default to a timestamped directory under
`tools\pose-matrix-results`. Each run writes:

- `pose-matrix-summary.json`, containing commands, counters, metrics, and
  per-case errors;
- `pose-matrix-summary.csv`, with one flattened row per case; and
- for `VisualFire`, one PNG per case.

The script restores the starting grip and aim values and releases the trigger
in `finally`. Meta XR Operator 205.1 has no API to relinquish simulated pose
ownership during a live session, so restoring a value does not return control
to the simulator UI. End the OpenXR session to release Operator ownership.
Use `-KeepFinalPose` only when the last commanded pose should remain visible.

## What is measured

For every case the harness snapshots `UEVR_Status`, commands the two poses, and
polls until both:

- `pose_diagnostics.sequence` advances; and
- the diagnostic grip and aim poses match the requested positions and
  orientations.

It records diagnostic sequence plus visual, marker, and projectile counters.
Numeric metrics include:

- grip/aim position error, quaternion absolute dot, and angular error;
- expected versus rendered weapon position;
- weapon forward, visible-barrel, and up-vector dots;
- weapon and right-wrist determinant and orthogonality errors; and
- rendered right-hand-forward versus visible weapon-barrel dot; and
- polling count and latency.

Firing cases additionally require marker and projectile counters to advance and
record:

- muzzle-versus-reticle dot;
- projectile-versus-reticle and projectile-versus-muzzle-forward dots;
- viewmodel-relative controller-aim dots for diagnosis only;
- projectile-to-muzzle position error; and
- muzzle basis determinant and orthogonality error.

The pass/fail decision only compares directions expressed in the native Blam
world space. The controller/viewmodel aim and weapon position are recorded for
diagnosis but are not compared directly with the native projectile position:
the first-person palette is viewmodel-relative, while the reticle, muzzle
marker, and projectile diagnostics are native world-space values. A projectile
sample is also captured after creation and can already have advanced away from
the muzzle, so projectile-to-muzzle distance is not a zero-distance assertion.

The expected weapon transform is reconstructed from the exact conventions used
by the Halo plugin: OpenXR `+X right, +Y up, -Z forward`, Blam `+X forward,
+Y left, +Z up`, and `3.048` metres per Blam unit. The visible weapon's authored
`+Y` barrel is compared to controller aim; its matrix `forward` is not
mistakenly treated as the barrel axis.
