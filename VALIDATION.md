# Halo Campaign Evolved standalone validation

Validated on 2026-07-29 against Meta XR Simulator 205.0 and the installed
`HaloSimulation_tag_release.dll` SHA-256
`82B8A3A006BA3F981D6857DC7F4E4E929AE5282587F31F92F77A3FA78F4B2DAC`.

## Final payload

- `UEVRBackend.dll`:
  `1187BDD4678F3A23152DF14C9EC53E54993C3E14443AA20B9A2FCE9706CD7293`
- `plugins\HaloCEMotionControls.dll`:
  `31C9575483AE3AD25F44801ABD0A2B6CA83CCF98D035A37E7BAD093BBA956F9B`
- `scripts\halo_motion_reticle.lua`:
  `BEA023003492D00ED8EAF1B577D40B5E3E8B1A803C13D6883B7DA29C60BA4C8D`

The Lua file passed a Lua 5.4 `loadfile` parse and a mocked lifecycle test
covering creation, ready publication, pose reporting, gameplay teardown,
recreation, and partial-component-loss cleanup.

## Controller, reticle, and native projectile test

The final backend was launched with Meta XR Operator only as a validation
surface. A fixed right-hand position and instant controller orientations were
used. The rendered 76-node first-person weapon and world reticle visibly moved
together in yaw and pitch. The native first-tick Blam sweep produced these
controller-directed unit vectors:

| Pose | Native direction | Target dot |
| --- | --- | ---: |
| Up 30 degrees | `(-0.454, 0.723, 0.521)` | `1.000` |
| Right 30 degrees | `(-0.090, 0.995, -0.030)` | `1.000` |
| Down 30 degrees | `(-0.539, 0.695, -0.475)` | `1.000` |
| Left 30 degrees | `(-0.902, 0.423, 0.083)` | `1.000` |
| Up-right diagonal | `(-0.140, 0.933, 0.331)` | `1.000` |

Earlier cardinal and four-diagonal sweeps likewise produced distinct
directions with `target-dot=1.000`. Weapon translation, weapon rotation, and
the ten-metre world reticle were all driven from the same right-controller
pose. `VR_AimMethod=0` and both decoupled-pitch settings remained disabled, so
the game/HMD camera and fixed HUD did not follow weapon motion.

## Cold standalone test

The same final files were then cold-launched through
`Start-HaloCEVR-Standalone.ps1`. The launcher selected Meta XR Simulator,
launched Steam app 2806050, and started the official injector with
`--attach=HaloCampaignEvolved.exe`.

Live process and log inspection proved:

- `launch_mode` was `standalone`;
- `VR_MetaXROperatorEnabled=false` was loaded before OpenXR instance creation;
- `XrApiLayer_METAX_operator.dll` was absent from the game process;
- no `uevr_mcp` module was loaded;
- there was no listener on TCP port 8720;
- no agentic-tool extension or UEVR Operator bridge registration appeared in
  the UEVR log;
- the exact packaged backend was loaded from this release directory;
- the active profile contained only the exact native plugin and Lua script
  listed above;
- the native fire hooks and 76-node first-person palette hook installed;
- controller tracking, gameplay readiness, and the world-reticle readiness
  markers all became ready; and
- a real standalone shot completed its authoritative first-tick sweep with
  `source-dot=1.000` and `target-dot=1.000`.

This proves Meta XR Operator and MCP were useful for automated validation but
are not runtime dependencies of the shipped Halo motion controls.
