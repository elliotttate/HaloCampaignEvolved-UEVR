# Headless campaign resume

`Resume-HaloCampaignSave.ps1` moves a freshly launched, UEVR-injected Halo
session from the front end into the existing campaign save without mouse or
keyboard automation.

It uses UEVR MCP's game-thread Lua endpoint to resolve
`/Script/Meteorite.MeteoriteUIStatics` by reflection, obtains its class default
object and the current player controller, then calls:

1. `CanResumeCampaignSave(PlayerController)`
2. `ResumeCampaignSave(PlayerController)` only after the first call returns
   true

The script does not synthesize keyboard, mouse, or gamepad input. It waits for
the game's own campaign-flow safety checks and calls the reflected resume
function only after the save is available.

## Prerequisites

- Halo was launched through Steam and UEVR was injected.
- `uevr_mcp.dll` is loaded and listening on port 8899.
- `CoreSave_0.sav` exists in the standard Meteorite save directory.
- `HaloCEMotionControls.dll` is installed, so the gameplay marker can prove
  that the Blam level and local player are ready.

## Usage

```powershell
.\tools\Resume-HaloCampaignSave.ps1
```

Success requires
`%APPDATA%\UnrealVRMod\HaloCampaignEvolved\data\halo_motion_gameplay.active`
to be written as `ready` after the script starts. A stale marker cannot produce
a false success.

The safety gate is intentional. A forced `ResumeCampaignSave` call while
`CanResumeCampaignSave` was false was immediately followed by an access
violation in the current CU3 build. A later session reached the same crash
address without the forced call, so that crash does not prove causality, but it
also provides no basis for bypassing the game's own guard. The script times out
with an explanation instead of forcing the call.
