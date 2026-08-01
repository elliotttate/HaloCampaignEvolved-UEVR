#pragma once

#include <cstdint>

// Shared, append-only C ABI used by the optional Meta XR Operator diagnostics
// bridge.  This header lives with the Halo plugin so a stock praydog UEVR SDK
// is sufficient to build the mod.
struct HaloCEVR_DiagnosticVec3 {
    float x;
    float y;
    float z;
};

struct HaloCEVR_DiagnosticQuat {
    float x;
    float y;
    float z;
    float w;
};

struct HaloCEVR_DiagnosticPose {
    HaloCEVR_DiagnosticVec3 position;
    HaloCEVR_DiagnosticQuat rotation;
};

struct HaloCEVR_DiagnosticMatrix {
    float scale;
    HaloCEVR_DiagnosticVec3 forward;
    HaloCEVR_DiagnosticVec3 left;
    HaloCEVR_DiagnosticVec3 up;
    HaloCEVR_DiagnosticVec3 position;
};

struct HaloCEVR_PoseDiagnostics {
    std::uint32_t size;
    std::uint32_t version;
    std::uint32_t sequence;
    std::uint32_t flags;
    std::int64_t predicted_display_time;
    std::int64_t predicted_display_period;
    HaloCEVR_DiagnosticPose hmd;
    HaloCEVR_DiagnosticPose right_grip;
    HaloCEVR_DiagnosticPose right_aim;
    HaloCEVR_DiagnosticPose left_grip;
    HaloCEVR_DiagnosticPose left_aim;
    HaloCEVR_DiagnosticMatrix root;
    HaloCEVR_DiagnosticMatrix weapon;
    HaloCEVR_DiagnosticMatrix right_wrist;
    HaloCEVR_DiagnosticMatrix left_wrist;
    HaloCEVR_DiagnosticMatrix muzzle_marker;
    HaloCEVR_DiagnosticVec3 reticle_position;
    HaloCEVR_DiagnosticVec3 projectile_position;
    HaloCEVR_DiagnosticVec3 projectile_forward;
    HaloCEVR_DiagnosticVec3 projectile_up;
    HaloCEVR_DiagnosticVec3 right_hand_forward;
    HaloCEVR_DiagnosticVec3 right_hand_thumb_side;
    HaloCEVR_DiagnosticVec3 right_hand_palm_normal;
    HaloCEVR_DiagnosticVec3 right_hand_grip_forward;
    HaloCEVR_DiagnosticVec3 left_hand_forward;
    HaloCEVR_DiagnosticVec3 left_hand_thumb_side;
    HaloCEVR_DiagnosticVec3 left_hand_palm_normal;
    HaloCEVR_DiagnosticVec3 left_hand_grip_forward;
    float right_index_curl;
    float right_grip_curl;
    float right_thumb_curl;
    float left_index_curl;
    float left_grip_curl;
    float left_thumb_curl;
    float right_index_tip_distance_blam;
    float left_index_tip_distance_blam;
    std::int32_t final_xinput_left_x;
    std::int32_t final_xinput_left_y;
    std::int32_t final_xinput_right_x;
    std::int32_t final_xinput_right_y;
    std::uint32_t final_xinput_buttons;
    std::uint32_t xinput_callback_count;
    std::uint32_t visual_override_count;
    std::uint32_t marker_override_count;
    std::uint32_t projectile_override_count;
    std::uint32_t reserved;
};

static_assert(sizeof(HaloCEVR_DiagnosticVec3) == 12);
static_assert(sizeof(HaloCEVR_DiagnosticQuat) == 16);
static_assert(sizeof(HaloCEVR_DiagnosticPose) == 28);
static_assert(sizeof(HaloCEVR_DiagnosticMatrix) == 52);
static_assert(sizeof(HaloCEVR_PoseDiagnostics) == 648);
