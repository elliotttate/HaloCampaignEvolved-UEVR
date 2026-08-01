# Halo Campaign Evolved UEVR standalone @VERSION@

This is the self-contained, no-MCP release of the Halo Campaign Evolved 6DOF
motion-control mod. It includes the paired UEVR API 2.43 backend and injector,
the native `HaloCEMotionControls.dll`, and the authored world-reticle Lua
script. Source commit: `@SOURCE_COMMIT@`.

Meta XR Operator and the UEVR MCP were used for development and automated
validation, but neither is required at runtime and neither is included here.

## Fast install

1. Extract the entire ZIP to a normal writable folder. Do not run it from
   inside the ZIP.
2. In PowerShell, verify the download:

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File .\Verify-Package.ps1
   ```

3. Install the Halo profile files, reticle LogicMod, and required UEVR settings:

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File .\Install-HaloCEVR.ps1
   ```

The installer copies only these runtime mod files:

```text
%APPDATA%\UnrealVRMod\HaloCampaignEvolved\
|-- plugins\HaloCEMotionControls.dll
`-- scripts\halo_motion_reticle.lua

<Halo game directory>\Meteorite\Content\Paks\LogicMods\
|-- HaloCEReticleColor.pak
|-- HaloCEReticleColor.utoc
`-- HaloCEReticleColor.ucas
```

The game directory is discovered from a running Halo process or Steam's
library manifests. If discovery cannot find a non-default Steam library, pass
it explicitly:

```powershell
.\Install-HaloCEVR.ps1 `
  -GameRoot 'D:\SteamLibrary\steamapps\common\Halo Campaign Evolved'
```

The LogicMod contains only the two UE 5.6 VR-widget material packages used to
keep Halo's authored cyan/red reticle color stable under eye adaptation. It
deliberately does not ship `global.utoc` or `global.ucas`; those files belong
to the game and must never be replaced by a mod.

It also updates the existing `config.txt` and three camera presets with the
motion-control-safe aim, pitch, interpolation, hand, and world-scale values.
The original configuration is backed up on the first install.

## Run on a real OpenXR headset

1. Select SteamVR, Meta Quest Link, or your preferred headset runtime as the
   active system OpenXR runtime.
2. Launch Halo Campaign Evolved through Steam and load a campaign level.
3. Run:

   ```powershell
   .\UEVRInjector.exe --attach=HaloCampaignEvolved.exe
   ```

The injector watches for the Steam-launched process and loads the paired UEVR
backend. No MCP connection is necessary.

## Run with Meta XR Simulator

Install Meta XR Simulator, close any existing Halo process, then run:

```powershell
.\Start-HaloCEVR-Standalone.ps1
```

If Halo is installed somewhere other than the launcher's default Steam
library, pass its executable explicitly:

```powershell
.\Start-HaloCEVR-Standalone.ps1 `
  -GameExe 'D:\SteamLibrary\steamapps\common\Halo Campaign Evolved\Meteorite\Binaries\Win64\HaloCampaignEvolved.exe'
```

The standalone launcher selects Meta XR Simulator, stages and verifies the
profile payload, launches Steam AppID 2806050, and uses the official
`--attach=HaloCampaignEvolved.exe` injection path. It explicitly disables Meta
XR Operator for the shipping session.

## Controls and behavior

- Right controller: independent 6DOF weapon aim and authoritative projectile
  direction.
- Left stick: Halo locomotion through the OpenXR-to-XInput bridge.
- Left grip near the barrel: two-handed hold.
- Zoom: returns to Halo's stock centered aiming path.
- Reticle: Halo's weapon-specific authored ring, moved into stereo world space
  at the controller/projectile convergence point.

See `HALO_MOTION_CONTROLS.md` for implementation details and environment
switches, and `TESTING.md` for the validation matrix.

## Uninstall

Close Halo and run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Uninstall-HaloCEVR.ps1
```

The uninstaller removes the profile and LogicMod files installed by this
package and restores first-install backups when it can do so without
overwriting later user changes. The recorded game directory is used
automatically. Use `-Force` only if you intentionally want to restore backups
over subsequently edited files.

## Troubleshooting

- **Steam says Halo is already running:** check Task Manager for
  `HaloCampaignEvolved.exe` or its crash reporter and exit the stale process
  before retrying. The launcher will not inject into an ambiguous old session.
- **No controller movement:** confirm the desired OpenXR runtime is active and
  that the profile contains `VR_ControllersAllowed=true` and
  `VR_ForceMotionControlsActive=true`. Re-run `Install-HaloCEVR.ps1` to repair
  the package-owned values.
- **Hands move opposite the head:** controller poses are tracking-space poses.
  In Meta XR Simulator use **Controllers Follow: Head** for a carried-rig test,
  and end any old Operator-owned XR session before returning to simulator input.
- **Weapon moves but shots do not:** this plugin supports the shipping
  `HaloSimulation_tag_release.dll` SHA-256
  `82B8A3A006BA3F981D6857DC7F4E4E929AE5282587F31F92F77A3FA78F4B2DAC`.
  On another build the native hooks deliberately fail open.
- **Reticle is missing or duplicated:** make sure only this package's
  `halo_motion_reticle.lua` is present. Do not copy the repository's general
  `scripts\libs` experiments into the active profile.
- **Verify or install reports a hash mismatch:** delete the extracted folder,
  download the ZIP again, and re-extract it. Do not mix files from older
  releases.

## Integrity

`SHA256SUMS.txt` covers every distributed file except itself. The downloadable
ZIP also has a sibling `.sha256` file on the GitHub release page.
