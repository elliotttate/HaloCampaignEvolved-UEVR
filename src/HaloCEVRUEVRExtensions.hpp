#pragma once

#include <cstddef>
#include <type_traits>

// API 2.39 is the public praydog/UEVR plugin ABI.  Our development backend
// appends optional functions to UEVR_VRData without changing that prefix.
// Keep the extension description local and inspect the backend's advertised
// version before reading any appended field, allowing one DLL to run on both.
namespace halo_cevr::uevr_extensions {

static_assert(
    UEVR_PLUGIN_VERSION_MAJOR == 2 && UEVR_PLUGIN_VERSION_MINOR == 34,
    "HaloCEMotionControls must be compiled against the official UEVR 1.05 "
    "API 2.34 SDK so the DLL retains the older official plugin ABI.");

struct TrackingPose {
    UEVR_Vector3f position;
    UEVR_Quaternionf rotation;
    unsigned long long location_flags;
};

struct LateTrackingSnapshot {
    unsigned int size;
    long long predicted_display_time;
    long long predicted_display_period;
    TrackingPose hmd;
    TrackingPose left_grip;
    TrackingPose left_aim;
    TrackingPose right_grip;
    TrackingPose right_aim;
};

constexpr unsigned int kCompositorQuadVisible = 1U;
struct OpenXRCompositorQuadState {
    unsigned int size;
    unsigned int flags;
    UEVR_UObjectHandle texture;
    UEVR_Vector3f position;
    UEVR_Quaternionf rotation;
    UEVR_Vector2f size_meters;
};

struct ExtendedVRData {
    UEVR_VRData base;
    bool (*get_late_tracking_snapshot)(LateTrackingSnapshot* out_snapshot);
    bool (*set_openxr_compositor_quad)(
        const OpenXRCompositorQuadState* state);
    void (*clear_openxr_compositor_quad)();
    float (*get_action_float)(
        UEVR_ActionHandle action, UEVR_InputSourceHandle source);
    bool (*get_action_float_value)(
        UEVR_ActionHandle action,
        UEVR_InputSourceHandle source,
        float* out_value);
};

static_assert(std::is_standard_layout_v<ExtendedVRData>);
static_assert(offsetof(ExtendedVRData, get_late_tracking_snapshot) ==
              sizeof(UEVR_VRData));

inline bool backend_at_least(int minor) {
    const auto* const param = uevr::API::get()->param();
    return param != nullptr && param->version != nullptr &&
           (param->version->major > 2 ||
            (param->version->major == 2 &&
             param->version->minor >= minor));
}

inline const ExtendedVRData* data() {
    const auto* const param = uevr::API::get()->param();
    return param != nullptr && param->vr != nullptr
        ? reinterpret_cast<const ExtendedVRData*>(param->vr)
        : nullptr;
}

inline bool get_late_tracking_snapshot(LateTrackingSnapshot& snapshot) {
    if (!backend_at_least(40)) {
        return false;
    }
    const auto* const extended = data();
    if (extended == nullptr || extended->get_late_tracking_snapshot == nullptr) {
        return false;
    }
    snapshot.size = sizeof(snapshot);
    return extended->get_late_tracking_snapshot(&snapshot);
}

inline bool set_openxr_compositor_quad(
    const OpenXRCompositorQuadState& state) {
    if (!backend_at_least(41)) {
        return false;
    }
    const auto* const extended = data();
    return extended != nullptr &&
           extended->set_openxr_compositor_quad != nullptr &&
           extended->set_openxr_compositor_quad(&state);
}

inline void clear_openxr_compositor_quad() {
    if (!backend_at_least(41)) {
        return;
    }
    const auto* const extended = data();
    if (extended != nullptr &&
        extended->clear_openxr_compositor_quad != nullptr) {
        extended->clear_openxr_compositor_quad();
    }
}

inline bool get_action_float_value(
    UEVR_ActionHandle action,
    UEVR_InputSourceHandle source,
    float& value) {
    const auto* const extended = data();
    if (backend_at_least(43) && extended != nullptr &&
        extended->get_action_float_value != nullptr) {
        return extended->get_action_float_value(action, source, &value);
    }
    if (backend_at_least(42) && extended != nullptr &&
        extended->get_action_float != nullptr) {
        value = extended->get_action_float(action, source);
        return true;
    }
    return false;
}

} // namespace halo_cevr::uevr_extensions
