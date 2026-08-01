# Halo extended headless validation

- Generated UTC: 2026-08-01T01:33:43.6846060Z
- Halo PID: 41660 (verified owner of port 8720)
- Fatal error: none

## Gate summary

- HMD-01: passed, 7 passed, 0 failed
- HAND-01: partial_pass_abi_gaps, 5 passed, 0 failed
- TWO-01: partial_pass_abi_gaps, 4 passed, 0 failed
- INP-01: failed, 6 passed, 1 failed

## Cases

- [PASS] HMD-01 rig_translate_xyz
- [PASS] HMD-01 rig_yaw_pos_20
- [PASS] HMD-01 rig_pitch_neg_15
- [PASS] HMD-01 rig_compound
- [PASS] HMD-01 head_only_translate_x
- [PASS] HMD-01 head_only_yaw_pos_20
- [PASS] HMD-01 head_only_pitch_neg_15
- [PASS] HAND-01 close_to_face
- [PASS] HAND-01 cross_body
- [PASS] HAND-01 fully_extended
- [PASS] HAND-01 above_head
- [PASS] HAND-01 behind_shoulder
- [PASS] TWO-01 outside_zone_no_latch
- [PASS] TWO-01 acquire_and_basis
- [PASS] TWO-01 retained_outside_zone
- [PASS] TWO-01 release
- [PASS] INP-01 dpad_mode_true
- [PASS] INP-01 dpad_mode_false
- [PASS] INP-01 inside_deadzone_x
- [FAIL] INP-01 outside_deadzone_x: nonzero above-deadzone stick never reached Halo XInput callback
- [PASS] INP-01 half_positive_y
- [PASS] INP-01 full_negative_y
- [PASS] INP-01 right_trigger_release

## ABI gaps

- shoulder, elbow, clavicle and per-vertex stretch geometry
- controller tracking-valid override for deterministic loss/reacquire
- runtime ARM_IK enable/disable without plugin reload
- two-hand pause, zoom, disable and grenade-consumption state
- final XINPUT_GAMEPAD stick/button state and physical-gamepad merge
- Operator 205.1 scalar thumbstick writes clear the other axis, preventing diagonal injection
