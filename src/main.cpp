#include <uevr/HaloCEVRDiagnostics.h>
#include <uevr/Plugin.hpp>

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

using namespace uevr;

struct HaloCEVR_RuntimeStatus {
    std::uint32_t size;
    std::uint32_t version;
    std::uint32_t flags;
    std::uint32_t override_count;
    std::int32_t local_weapon_index;
    std::uintptr_t attached_component;
};
static_assert(sizeof(HaloCEVR_RuntimeStatus) == 32);

namespace {

constexpr float kMetersPerBlamUnit = 3.048f;
constexpr std::int32_t kInvalidBlamObjectIndex = -1;
constexpr std::size_t kMarkerResultStride = 0x70;
constexpr std::uint32_t kStatusNativeHooksInstalled = 1U << 0;
constexpr std::uint32_t kStatusTrackingValid = 1U << 1;
constexpr std::uint32_t kStatusVisualWeaponAttached = 1U << 2;
constexpr std::uint32_t kStatusNativeVisualHookInstalled = 1U << 3;
constexpr std::uint32_t kStatusNativeProjectileHookInstalled = 1U << 4;
constexpr std::uint32_t kStatusLeftTrackingValid = 1U << 5;
constexpr std::uint32_t kStatusTwoHandIkEnabled = 1U << 6;
constexpr std::uint32_t kStatusLocomotionBridgeObserved = 1U << 7;
constexpr std::uint32_t kStatusTwoHandHoldActive = 1U << 8;
constexpr std::uint32_t kDiagnosticVisualValid = 1U << 0;
constexpr std::uint32_t kDiagnosticMarkerValid = 1U << 1;
constexpr std::uint32_t kDiagnosticProjectileValid = 1U << 2;
constexpr std::uint32_t kDiagnosticLeftWristValid = 1U << 3;
constexpr std::uint32_t kDiagnosticRightHandGeometryValid = 1U << 4;
constexpr std::uint32_t kDiagnosticLeftHandGeometryValid = 1U << 5;
constexpr std::uint32_t kFirstPersonNodeCount = 76;

// Torso anchoring constants, ported from RoboquestVR's arm rig. Roboquest
// attaches its arms mesh to the HMD position but only the HMD yaw, and pushes
// the mesh ~18 cm behind the camera so the shoulders sit where a torso would
// be. The offsets below hang each shoulder from a yaw-only head frame:
// head pitch or roll never swings the arm root, which is the main reason its
// arms never cross the player's face. Distances are in meters (converted to
// Blam units at use); tuned starting values, not calibration.
constexpr float kShoulderBackMeters = 0.16f;
constexpr float kShoulderDownMeters = 0.22f;
constexpr float kShoulderLateralMeters = 0.17f;
// When the tracked hand is beyond the authored arm's reach, slide the arm
// root toward the target by up to this much (Roboquest gets the same effect
// from a FABRIK chain rooted at the clavicle) before clamping the remainder.
constexpr float kClavicleAssistMaxMeters = 0.12f;
// The left wrist bone belongs behind the controller's grip point, not on it.
// The OpenXR grip pose sits at the palm centroid; Roboquest offsets its wrist
// targets a fixed distance back along the hand so knuckles, not the wrist,
// land where the controller is held.
constexpr float kGripToWristBackMeters = 0.08f;
constexpr float kGripToWristDownMeters = 0.02f;

// Two-handed hold, ported from Halo-MCC-VR's headset-tuned barrel grab. The
// grab zone is a thin cylinder along the right-controller aim ray, measured
// from the right grip position (the weapon's rear hand): 8-80 cm forward,
// within 9 cm of the ray. Engaging requires the left grip button while the
// left palm is inside the zone; the hold then persists until the button
// releases, so the zone only gates acquisition, never retention.
constexpr float kTwoHandZoneMinAlongMeters = 0.08f;
constexpr float kTwoHandZoneMaxAlongMeters = 0.80f;
constexpr float kTwoHandZoneRadiusMeters = 0.09f;
// The two-hand influence fades in across this agreement band between the
// hand-to-hand line and the right aim ray (Halo-MCC-VR hard-gates at 0.35;
// a smoothstep band avoids the ~70 degree weapon snap its cutoff produces
// when a latched support hand crosses the boundary). At or below the
// minimum, aim is fully one-handed; at or above the full value, the
// two-hand line has full authority.
constexpr float kTwoHandMinimumAgreement = 0.35f;
constexpr float kTwoHandFullAgreement = 0.50f;
constexpr float kTwoHandBlendSeconds = 0.15f;
constexpr bool kUseNativeChudCrosshairHide = false;

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

struct Quat {
    float x{};
    float y{};
    float z{};
    float w{1.0f};
};

struct UnrealVector {
    double x{};
    double y{};
    double z{};
};

struct UnrealRotator {
    double pitch{};
    double yaw{};
    double roll{};
};

struct UnrealVector2D {
    double x{};
    double y{};
};

struct UnrealIntPoint {
    std::int32_t x{};
    std::int32_t y{};
};

struct UnrealLinearColor {
    float r{};
    float g{};
    float b{};
    float a{};
};

constexpr double kWorldReticleScale = 1.0;
constexpr float kWorldReticleFallbackEmissiveGain = 4.0f;
constexpr float kWorldReticleExposureCompensatedGain = 1.0f;

template <typename T>
bool write_function_parameter(
    API::UFunction* function,
    std::span<std::byte> buffer,
    std::wstring_view name,
    const T& value) {
    if (function == nullptr) {
        return false;
    }
    auto* const property = function->find_property(name);
    if (property == nullptr || property->get_offset() < 0) {
        return false;
    }
    const auto offset = static_cast<std::size_t>(property->get_offset());
    if (offset + sizeof(T) > buffer.size()) {
        return false;
    }
    std::memcpy(buffer.data() + offset, &value, sizeof(T));
    return true;
}

template <typename T>
T read_function_parameter(
    API::UFunction* function,
    std::span<const std::byte> buffer,
    std::wstring_view name) {
    T result{};
    if (function == nullptr) {
        return result;
    }
    auto* const property = function->find_property(name);
    if (property == nullptr || property->get_offset() < 0) {
        return result;
    }
    const auto offset = static_cast<std::size_t>(property->get_offset());
    if (offset + sizeof(T) <= buffer.size()) {
        std::memcpy(&result, buffer.data() + offset, sizeof(T));
    }
    return result;
}

struct Mat3 {
    Vec3 forward{1.0f, 0.0f, 0.0f};
    Vec3 left{0.0f, 1.0f, 0.0f};
    Vec3 up{0.0f, 0.0f, 1.0f};
};

struct BlamMatrix4x3 {
    float scale{1.0f};
    Vec3 forward{};
    Vec3 left{};
    Vec3 up{};
    Vec3 position{};
};
static_assert(sizeof(BlamMatrix4x3) == 0x34);

struct MarkerResult {
    std::int16_t marker_or_node_index{};
    std::uint16_t padding{};
    BlamMatrix4x3 local{};
    BlamMatrix4x3 world{};
    std::uint32_t metadata{};
};
static_assert(offsetof(MarkerResult, local) == 0x04);
static_assert(offsetof(MarkerResult, world) == 0x38);
static_assert(offsetof(MarkerResult, world.forward) == 0x3C);
static_assert(offsetof(MarkerResult, world.position) == 0x60);
static_assert(sizeof(MarkerResult) == kMarkerResultStride);

struct ProjectileNewData {
    std::array<std::uint8_t, 0x1C> prefix{};
    Vec3 position{};
    Vec3 forward{};
    Vec3 up{};
    Vec3 velocity{};
    Vec3 angular_velocity{};
};
static_assert(offsetof(ProjectileNewData, position) == 0x1C);
static_assert(offsetof(ProjectileNewData, forward) == 0x28);
static_assert(offsetof(ProjectileNewData, up) == 0x34);
static_assert(offsetof(ProjectileNewData, velocity) == 0x40);
static_assert(offsetof(ProjectileNewData, angular_velocity) == 0x4C);

struct NativeObjectData {
    std::array<std::uint8_t, 0x44> prefix{};
    Vec3 position{};
    Vec3 forward{};
    Vec3 up{};
    Vec3 velocity{};
};
static_assert(offsetof(NativeObjectData, position) == 0x44);
static_assert(offsetof(NativeObjectData, forward) == 0x50);
static_assert(offsetof(NativeObjectData, up) == 0x5C);
static_assert(offsetof(NativeObjectData, velocity) == 0x68);

struct TrackingSnapshot {
    Vec3 hmd_position{};
    Quat hmd_rotation{};
    Vec3 right_grip_position{};
    Quat right_grip_rotation{};
    Vec3 right_aim_position{};
    Quat right_aim_rotation{};
    Vec3 left_grip_position{};
    Quat left_grip_rotation{};
    Vec3 left_aim_position{};
    Quat left_aim_rotation{};
    std::int64_t predicted_display_time{};
    std::int64_t predicted_display_period{};
    bool late_located{};
    bool left_valid{};
    bool valid{};
};

struct GetPawnViewModeAndWeaponActorsParams {
    std::uint8_t out_view_mode{};
    std::array<std::uint8_t, 7> padding{};
    API::UObject* third_person_weapon{};
    API::UObject* first_person_weapon{};
};
static_assert(sizeof(GetPawnViewModeAndWeaponActorsParams) == 24);

struct GetRootComponentParams {
    API::UObject* return_value{};
};
static_assert(sizeof(GetRootComponentParams) == 8);

struct IsGamePausedParams {
    API::UObject* world_context{};
    bool return_value{};
};
static_assert(offsetof(IsGamePausedParams, return_value) == 8);

struct GetComponentByClassParams {
    API::UClass* component_class{};
    API::UObject* return_value{};
};
static_assert(sizeof(GetComponentByClassParams) == 16);

struct GetComponentsByClassParams {
    API::UClass* component_class{};
    API::TArray<API::UObject*> return_value{};
};

using TriggerCreateProjectilesFn = std::int16_t (*)(
    std::uint32_t weapon_object_index,
    std::int16_t barrel_index,
    const void* network_projectile_records,
    bool from_network);

using GetMarkersFn = std::int16_t (*)(
    std::uint32_t object_index,
    std::int32_t marker_string_id,
    MarkerResult* output,
    std::uint16_t maximum_count,
    std::uint8_t use_exact_object,
    void* unused_context,
    std::uint8_t use_interpolated_transform);

using ProjectileNewFn = std::int64_t (*)(ProjectileNewData* data);

using ProjectileCollisionSweepFn = std::int64_t (*)(
    std::uint32_t projectile_object_index,
    std::int64_t flags,
    std::uint8_t phase,
    Vec3* start,
    Vec3* end,
    std::int32_t arg6,
    std::int32_t arg7,
    std::int32_t arg8,
    void* result);

using FirstPersonWeaponBuildFn = void (*)(
    std::int32_t local_player,
    std::int32_t weapon_slot,
    bool capture_render_palette);

using ChudShowCrosshairFn = std::int64_t (*)(
    std::int16_t controller_index,
    std::int32_t user_index,
    std::uint8_t update_immediately);

std::mutex g_tracking_mutex{};
TrackingSnapshot g_tracking_snapshot{};
std::mutex g_pose_diagnostics_mutex{};
HaloCEVR_PoseDiagnostics g_pose_diagnostics{};
std::atomic_uint32_t g_pose_diagnostic_sequence{};
std::atomic<std::int32_t> g_local_weapon_index{
    kInvalidBlamObjectIndex};
std::atomic_bool g_native_override_enabled{true};
std::atomic_uint32_t g_override_count{};
std::atomic_uint32_t g_marker_override_count{};
std::atomic_bool g_native_hooks_installed{};
std::atomic_bool g_native_projectile_hook_installed{};
std::atomic_bool g_native_visual_hook_installed{};
std::atomic_bool g_native_crosshair_hide_supported{};
std::atomic_bool g_replacement_reticle_active{};
std::atomic_int g_gameplay_ready_published{-1};
std::atomic_uint32_t g_visual_override_count{};
std::atomic_uint32_t g_visual_diagnostic_count{};
std::atomic_uint32_t g_visual_build_entry_count{};
std::atomic_bool g_tracking_valid{};
std::atomic_bool g_left_tracking_valid{};
std::atomic_bool g_late_tracking_active{};
std::atomic_bool g_late_tracking_logged{};
std::atomic_bool g_visual_weapon_attached{};
std::atomic_uintptr_t g_attached_component{};
// Anchored-arm IK is the default: shoulders hang from a yaw-only head frame,
// overreach slides the arm root forward (clavicle assist), and the wrist is
// snapped exactly to the tracked pose afterwards, so the authored 0.635 m arm
// can no longer leave hands behind. UEVR_HALO_ARM_IK=0 restores the previous
// floating-hands mode (arms hidden, wrist-only placement).
std::atomic_bool g_two_hand_ik_enabled{true};
std::atomic_bool g_two_hand_ik_fallback_logged{};
// Fixed controller-to-wrist axis convention for the left hand, latched from
// the stock palette on the first tracked frame after each weapon change.
// Reading it every frame let the running stock animation rotate the tracked
// hand; latching per weapon keeps it the constant mapping it was always
// meant to be. The matrix is written and read only on the first-person
// build thread; the flag is atomic because the tick thread clears it when
// the local weapon index changes.
Mat3 g_left_wrist_stock_relative{};
std::atomic_bool g_left_wrist_stock_latched{};
// Two-handed hold state. The latch is decided once per engine tick from the
// published tracking snapshot plus the left grip action; the blend eases the
// weapon between one- and two-handed aim and is read by both the palette
// build and the native fire path. UEVR_HALO_TWO_HAND_HOLD=0 disables it.
std::atomic_bool g_two_hand_hold_enabled{true};
std::atomic_bool g_two_hand_hold_latched{};
std::atomic_bool g_two_hand_zone_active{};
std::atomic<float> g_two_hand_hold_blend{};
// Last valid HMD-relative two-hand forward (Blam axes). When left tracking
// drops mid-hold, the blend tail eases out along this line instead of
// snapping the weapon back to one-handed aim in a single frame.
std::atomic<Vec3> g_two_hand_last_forward{};
// True while Halo reports the local player zoomed. Sampled on the draw
// thread (where the CHUD TLS is known-good) and consumed by the fire hooks:
// scoped shots keep Halo's stock screen-center aim to match the restored
// stock crosshair.
std::atomic_bool g_local_zoomed{};
std::atomic_bool g_game_paused{};
std::atomic_int g_reticle_hide_published{-1};
std::atomic_bool g_locomotion_bridge_observed{};

TriggerCreateProjectilesFn g_original_trigger_create_projectiles{};
GetMarkersFn g_original_get_markers{};
ProjectileNewFn g_original_projectile_new{};
ProjectileCollisionSweepFn g_original_projectile_collision_sweep{};
FirstPersonWeaponBuildFn g_original_first_person_weapon_build{};
ChudShowCrosshairFn g_original_chud_show_crosshair{};
void* g_primary_marker_return{};
void* g_projectile_return_primary{};
void* g_projectile_return_secondary{};
void* g_projectile_primary_sweep_return{};
void* g_projectile_spread_sweep_return{};
int g_trigger_hook_id{-1};
int g_marker_hook_id{-1};
int g_projectile_hook_id{-1};
int g_projectile_sweep_hook_id{-1};
int g_visual_hook_id{-1};
int g_chud_show_crosshair_hook_id{-1};
std::uint8_t* g_simulation_module{};

constexpr std::size_t kHaloTlsIndexRva = 0xD72730;
constexpr std::size_t kHaloTlsInitializedOffset = 0x14;
constexpr std::size_t kPlayerControlsTlsOffset = 0xB8;
constexpr std::size_t kPlayerControlStride = 0x198;
constexpr std::size_t kPlayerZoomLevelOffset = 0xC6;
constexpr std::size_t kChudBankTlsOffset = 0x428;
constexpr std::size_t kChudRecordStride = 0xC8C;
constexpr std::size_t kChudCrosshairVisibilityOffset = 0x354;
constexpr std::size_t kChudRecordCount = 16;

struct StockCrosshairState {
    std::uint8_t* bank{};
    std::array<float, kChudRecordCount> saved{};
    bool saved_valid{};
    bool hidden{};
};
StockCrosshairState g_stock_crosshair_state{};

thread_local std::uint32_t g_local_fire_depth{};
thread_local TrackingSnapshot g_local_fire_tracking{};
struct LocalFireMarkerCorrection {
    BlamMatrix4x3 source{};
    BlamMatrix4x3 desired{};
    Vec3 reticle_position{};
};
thread_local std::array<LocalFireMarkerCorrection, 64>
    g_local_fire_marker_corrections{};
thread_local std::uint16_t g_local_fire_marker_correction_count{};
struct LocalProjectileDirectionOverride {
    std::uint32_t object_index{};
    Vec3 source_direction{};
    Vec3 desired_direction{};
    Vec3 reticle_position{};
    Mat3 delta_basis{};
    bool primary_sweep_consumed{};
};
thread_local std::array<LocalProjectileDirectionOverride, 64>
    g_local_projectile_direction_overrides{};
thread_local std::uint16_t g_local_projectile_direction_override_count{};

NativeObjectData* get_native_object(std::int32_t object_index) {
    if (object_index == kInvalidBlamObjectIndex ||
        g_simulation_module == nullptr) {
        return nullptr;
    }

#if defined(_MSC_VER)
    __try {
#endif
        constexpr std::size_t tls_index_rva = 0xD72730;
        constexpr std::size_t tls_object_state_offset = 0x20;
        constexpr std::size_t object_table_offset = 0x50;
        constexpr std::size_t object_entry_stride = 0x18;
        constexpr std::size_t object_entry_data_offset = 0x10;

        const auto tls_index = *reinterpret_cast<const DWORD*>(
            g_simulation_module + tls_index_rva);
        auto** const tls_slots =
            reinterpret_cast<void**>(__readgsqword(0x58));
        auto* const tls =
            tls_slots != nullptr
            ? static_cast<std::uint8_t*>(tls_slots[tls_index])
            : nullptr;
        if (tls == nullptr) {
            return nullptr;
        }

        auto* const object_state =
            *reinterpret_cast<std::uint8_t**>(
                tls + tls_object_state_offset);
        if (object_state == nullptr) {
            return nullptr;
        }

        auto* const object_table =
            *reinterpret_cast<std::uint8_t**>(
                object_state + object_table_offset);
        if (object_table == nullptr) {
            return nullptr;
        }

        const auto absolute_index =
            static_cast<std::uint16_t>(object_index);
        auto* const entry =
            object_table +
            static_cast<std::size_t>(absolute_index) *
                object_entry_stride;
        return *reinterpret_cast<NativeObjectData**>(
            entry + object_entry_data_offset);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
#endif
}

std::wstring profile_data_marker_path(const wchar_t* filename) {
    std::array<wchar_t, 32768> appdata{};
    const auto length = GetEnvironmentVariableW(
        L"APPDATA",
        appdata.data(),
        static_cast<DWORD>(appdata.size()));
    if (length == 0 || length >= appdata.size()) {
        return {};
    }

    std::wstring result{appdata.data(), length};
    result += L"\\UnrealVRMod\\HaloCampaignEvolved\\data\\";
    result += filename;
    return result;
}

std::wstring reticle_active_marker_path() {
    return profile_data_marker_path(L"halo_motion_reticle.active");
}

void publish_gameplay_ready(bool ready, bool force = false) {
    const auto desired = ready ? 1 : 0;
    const auto previous = g_gameplay_ready_published.exchange(
        desired,
        std::memory_order_acq_rel);
    if (!force && previous == desired) {
        return;
    }

    const auto marker =
        profile_data_marker_path(L"halo_motion_gameplay.active");
    if (marker.empty()) {
        return;
    }

    const auto separator = marker.find_last_of(L"\\/");
    if (separator != std::wstring::npos) {
        const auto directory = marker.substr(0, separator);
        CreateDirectoryW(directory.c_str(), nullptr);
    }

    const auto file = CreateFileW(
        marker.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    const char* const contents = ready ? "ready\n" : "inactive\n";
    const auto length = static_cast<DWORD>(std::strlen(contents));
    DWORD bytes_written = 0;
    WriteFile(file, contents, length, &bytes_written, nullptr);
    CloseHandle(file);
}

// The Lua reticle polls this marker and hides the floating aim ball while
// its ray would mislead: during a two-handed hold (shots follow the
// hand-to-hand line, not the right-controller ray the ball is anchored to)
// and while zoomed (scoped shots keep Halo's stock screen-center aim).
// Written on transitions only.
void publish_reticle_hide(bool hidden, bool force = false) {
    const auto desired = hidden ? 1 : 0;
    const auto previous = g_reticle_hide_published.exchange(
        desired,
        std::memory_order_acq_rel);
    if (!force && previous == desired) {
        return;
    }

    const auto marker =
        profile_data_marker_path(L"halo_motion_reticle_hide.active");
    if (marker.empty()) {
        return;
    }

    const auto separator = marker.find_last_of(L"\\/");
    if (separator != std::wstring::npos) {
        const auto directory = marker.substr(0, separator);
        CreateDirectoryW(directory.c_str(), nullptr);
    }

    const auto file = CreateFileW(
        marker.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    const char* const contents = hidden ? "on\n" : "off\n";
    const auto length = static_cast<DWORD>(std::strlen(contents));
    DWORD bytes_written = 0;
    WriteFile(file, contents, length, &bytes_written, nullptr);
    CloseHandle(file);
}

void refresh_replacement_reticle_state() {
    const auto marker = reticle_active_marker_path();
    bool active = false;
    if (!marker.empty()) {
        const auto file = CreateFileW(
            marker.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            std::array<char, 6> contents{};
            DWORD bytes_read = 0;
            active =
                ReadFile(
                    file,
                    contents.data(),
                    5,
                    &bytes_read,
                    nullptr) != FALSE &&
                bytes_read == 5 &&
                // Accept the production "right-controller ..." marker and
                // the short-lived "umg-right-controller ..." prototype so
                // an older profile cannot leave Halo's CHUD reticle visible.
                (std::memcmp(contents.data(), "right", 5) == 0 ||
                 std::memcmp(contents.data(), "umg-r", 5) == 0);
            CloseHandle(file);
        }
    }
    g_replacement_reticle_active.store(active, std::memory_order_release);
}

bool read_halo_game_thread_tls(std::uint8_t*& tls) {
    tls = nullptr;
    if (g_simulation_module == nullptr) {
        return false;
    }

#if defined(_MSC_VER)
    __try {
#endif
        const auto tls_index = *reinterpret_cast<const DWORD*>(
            g_simulation_module + kHaloTlsIndexRva);
        if (tls_index >= 1088) {
            return false;
        }

        auto** const tls_slots =
            reinterpret_cast<void**>(__readgsqword(0x58));
        tls = tls_slots != nullptr
            ? static_cast<std::uint8_t*>(tls_slots[tls_index])
            : nullptr;
        return tls != nullptr &&
            tls[kHaloTlsInitializedOffset] != 0;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        tls = nullptr;
        return false;
    }
#endif
}

bool read_local_zoomed(std::uint8_t* tls, bool& zoomed) {
#if defined(_MSC_VER)
    __try {
#endif
        auto* const controls =
            *reinterpret_cast<std::uint8_t**>(
                tls + kPlayerControlsTlsOffset);
        if (controls == nullptr) {
            return false;
        }

        const auto zoom_level =
            *reinterpret_cast<const std::int16_t*>(
                controls + kPlayerZoomLevelOffset);
        if (zoom_level < -1 || zoom_level > 2) {
            return false;
        }

        zoomed = zoom_level != -1;
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

void maintain_stock_crosshair() {
    if (!g_native_crosshair_hide_supported.load(
            std::memory_order_acquire)) {
        return;
    }

    std::uint8_t* tls{};
    if (!read_halo_game_thread_tls(tls)) {
        return;
    }

#if defined(_MSC_VER)
    __try {
#endif
        auto* const hud_bank =
            *reinterpret_cast<std::uint8_t**>(
                tls + kChudBankTlsOffset);
        if (hud_bank == nullptr) {
            return;
        }

        auto& state = g_stock_crosshair_state;
        if (state.bank != hud_bank) {
            state = {};
            state.bank = hud_bank;
        }

        bool zoomed{};
        const bool zoom_known = read_local_zoomed(tls, zoomed);
        g_local_zoomed.store(
            zoom_known && zoomed,
            std::memory_order_release);
        const bool want_hidden =
            kUseNativeChudCrosshairHide &&
            zoom_known &&
            !zoomed &&
            g_replacement_reticle_active.load(
                std::memory_order_acquire) &&
            g_tracking_valid.load(std::memory_order_acquire) &&
            g_native_projectile_hook_installed.load(
                std::memory_order_acquire);

        const auto snapshot_current = [&]() -> bool {
            std::array<float, kChudRecordCount> values{};
            for (std::size_t index = 0;
                 index < kChudRecordCount;
                 ++index) {
                const auto value =
                    *reinterpret_cast<const float*>(
                        hud_bank +
                        index * kChudRecordStride +
                        kChudCrosshairVisibilityOffset);
                if (!std::isfinite(value) || std::abs(value) > 16.0f) {
                    return false;
                }
                values[index] = value;
            }
            state.saved = values;
            state.saved_valid = true;
            return true;
        };

        if (want_hidden) {
            if (!state.hidden && !snapshot_current()) {
                return;
            }

            state.hidden = true;
            for (std::size_t index = 0;
                 index < kChudRecordCount;
                 ++index) {
                *reinterpret_cast<float*>(
                    hud_bank +
                    index * kChudRecordStride +
                    kChudCrosshairVisibilityOffset) = 0.0f;
            }
            return;
        }

        if (state.hidden && state.saved_valid) {
            for (std::size_t index = 0;
                 index < kChudRecordCount;
                 ++index) {
                *reinterpret_cast<float*>(
                    hud_bank +
                    index * kChudRecordStride +
                    kChudCrosshairVisibilityOffset) =
                    state.saved[index];
            }
            state.hidden = false;
            return;
        }

        snapshot_current();
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_stock_crosshair_state = {};
        g_native_crosshair_hide_supported.store(
            false,
            std::memory_order_release);
        // Without the crosshair path there is no zoom sampling either; a
        // stale "zoomed" reading here would silently disable the hand-aim
        // redirect for the rest of the session.
        g_local_zoomed.store(false, std::memory_order_release);
        API::get()->log_error(
            "HaloCEMotionControls: stock crosshair hide faulted and was "
            "disabled; native firing remains active");
    }
#endif
}

std::int64_t hook_chud_show_crosshair(
    std::int16_t controller_index,
    std::int32_t user_index,
    std::uint8_t update_immediately) {
    const auto result = g_original_chud_show_crosshair(
        controller_index,
        user_index,
        update_immediately);

    // chud_show_crosshair is called from inside the viewport draw after the
    // pre-draw callback. Halo rewrites all sixteen visibility records here,
    // so the pre-draw zeroes alone are overwritten before CHUD consumes them.
    // Re-apply the scope-safe policy after the native writer, immediately
    // before its caller continues drawing the HUD.
    maintain_stock_crosshair();
    return result;
}

Vec3 operator+(const Vec3& a, const Vec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 operator-(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 operator*(const Vec3& value, float scale) {
    return {value.x * scale, value.y * scale, value.z * scale};
}

Vec3 operator/(const Vec3& value, float scale) {
    return {value.x / scale, value.y / scale, value.z / scale};
}

float dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x};
}

float length_squared(const Vec3& value) {
    return dot(value, value);
}

bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool finite(const Quat& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

Vec3 normalized(const Vec3& value) {
    const auto squared = length_squared(value);
    if (!std::isfinite(squared) || squared < 1.0e-8f) {
        return {};
    }

    return value / std::sqrt(squared);
}

Quat normalized(const Quat& value) {
    const auto squared = value.x * value.x + value.y * value.y +
                         value.z * value.z + value.w * value.w;
    if (!std::isfinite(squared) || squared < 1.0e-8f) {
        return {};
    }

    const auto inverse = 1.0f / std::sqrt(squared);
    return {
        value.x * inverse,
        value.y * inverse,
        value.z * inverse,
        value.w * inverse};
}

Quat quaternion_between(const Vec3& source, const Vec3& destination) {
    const auto from = normalized(source);
    const auto to = normalized(destination);
    if (length_squared(from) < 0.8f || length_squared(to) < 0.8f) {
        return {};
    }

    const auto alignment = std::clamp(dot(from, to), -1.0f, 1.0f);
    if (alignment > 0.9999f) {
        return {};
    }

    if (alignment < -0.9999f) {
        auto axis = cross(from, {1.0f, 0.0f, 0.0f});
        if (length_squared(axis) < 1.0e-6f) {
            axis = cross(from, {0.0f, 1.0f, 0.0f});
        }
        axis = normalized(axis);
        return {axis.x, axis.y, axis.z, 0.0f};
    }

    const auto axis = cross(from, to);
    return normalized({axis.x, axis.y, axis.z, 1.0f + alignment});
}

Quat conjugate(const Quat& value) {
    return {-value.x, -value.y, -value.z, value.w};
}

Quat operator*(const Quat& a, const Quat& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Vec3 rotate(const Quat& rotation, const Vec3& value) {
    const Quat vector{value.x, value.y, value.z, 0.0f};
    const auto result = rotation * vector * conjugate(rotation);
    return {result.x, result.y, result.z};
}

Vec3 transform_vector(const Mat3& matrix, const Vec3& value) {
    return matrix.forward * value.x + matrix.left * value.y +
           matrix.up * value.z;
}

Mat3 multiply(const Mat3& a, const Mat3& b) {
    return {
        transform_vector(a, b.forward),
        transform_vector(a, b.left),
        transform_vector(a, b.up)};
}

Mat3 transpose(const Mat3& value) {
    return {
        {value.forward.x, value.left.x, value.up.x},
        {value.forward.y, value.left.y, value.up.y},
        {value.forward.z, value.left.z, value.up.z}};
}

Mat3 rotation_basis(const Quat& rotation) {
    return {
        rotate(rotation, {1.0f, 0.0f, 0.0f}),
        rotate(rotation, {0.0f, 1.0f, 0.0f}),
        rotate(rotation, {0.0f, 0.0f, 1.0f})};
}

Mat3 rotation_between(const Vec3& source, const Vec3& destination) {
    const auto from = normalized(source);
    const auto to = normalized(destination);
    if (length_squared(from) < 0.8f || length_squared(to) < 0.8f) {
        return {};
    }

    const auto alignment = std::clamp(dot(from, to), -1.0f, 1.0f);
    if (alignment > 0.9999f) {
        return {};
    }

    Quat rotation{};
    if (alignment < -0.9999f) {
        auto axis = cross(from, {1.0f, 0.0f, 0.0f});
        if (length_squared(axis) < 1.0e-6f) {
            axis = cross(from, {0.0f, 1.0f, 0.0f});
        }
        axis = normalized(axis);
        rotation = {axis.x, axis.y, axis.z, 0.0f};
    } else {
        const auto axis = cross(from, to);
        rotation = normalized(
            {axis.x, axis.y, axis.z, 1.0f + alignment});
    }

    return rotation_basis(rotation);
}

Mat3 orthonormal_basis(const BlamMatrix4x3& value) {
    return {
        normalized(value.forward),
        normalized(value.left),
        normalized(value.up)};
}

bool valid_basis(const Mat3& value) {
    if (!finite(value.forward) || !finite(value.left) || !finite(value.up)) {
        return false;
    }

    const auto forward_length = length_squared(value.forward);
    const auto left_length = length_squared(value.left);
    const auto up_length = length_squared(value.up);
    if (forward_length < 0.8f || forward_length > 1.2f ||
        left_length < 0.8f || left_length > 1.2f ||
        up_length < 0.8f || up_length > 1.2f) {
        return false;
    }

    return std::abs(dot(value.forward, value.left)) < 0.2f &&
           std::abs(dot(value.forward, value.up)) < 0.2f &&
           std::abs(dot(value.left, value.up)) < 0.2f;
}

bool valid_openxr_pose(const UEVR_TrackingPose& pose) {
    constexpr unsigned long long orientation_valid = 1ULL << 0;
    constexpr unsigned long long position_valid = 1ULL << 1;
    constexpr auto required = orientation_valid | position_valid;
    return (pose.location_flags & required) == required &&
           finite(Vec3{
               pose.position.x,
               pose.position.y,
               pose.position.z}) &&
           finite(Quat{
               pose.rotation.x,
               pose.rotation.y,
               pose.rotation.z,
               pose.rotation.w});
}

TrackingSnapshot capture_tracking_snapshot() {
    TrackingSnapshot snapshot{};

    UEVR_LateTrackingSnapshot late{};
    if (API::VR::is_openxr() &&
        API::VR::get_late_tracking_snapshot(late) &&
        valid_openxr_pose(late.hmd) &&
        valid_openxr_pose(late.right_grip) &&
        valid_openxr_pose(late.right_aim)) {
        snapshot.hmd_position = {
            late.hmd.position.x,
            late.hmd.position.y,
            late.hmd.position.z};
        snapshot.hmd_rotation = {
            late.hmd.rotation.x,
            late.hmd.rotation.y,
            late.hmd.rotation.z,
            late.hmd.rotation.w};
        snapshot.right_grip_position = {
            late.right_grip.position.x,
            late.right_grip.position.y,
            late.right_grip.position.z};
        snapshot.right_grip_rotation = {
            late.right_grip.rotation.x,
            late.right_grip.rotation.y,
            late.right_grip.rotation.z,
            late.right_grip.rotation.w};
        snapshot.right_aim_position = {
            late.right_aim.position.x,
            late.right_aim.position.y,
            late.right_aim.position.z};
        snapshot.right_aim_rotation = {
            late.right_aim.rotation.x,
            late.right_aim.rotation.y,
            late.right_aim.rotation.z,
            late.right_aim.rotation.w};
        snapshot.predicted_display_time =
            late.predicted_display_time;
        snapshot.predicted_display_period =
            late.predicted_display_period;
        snapshot.late_located = true;
        snapshot.valid = true;

        if (valid_openxr_pose(late.left_grip) &&
            valid_openxr_pose(late.left_aim)) {
            snapshot.left_grip_position = {
                late.left_grip.position.x,
                late.left_grip.position.y,
                late.left_grip.position.z};
            snapshot.left_grip_rotation = {
                late.left_grip.rotation.x,
                late.left_grip.rotation.y,
                late.left_grip.rotation.z,
                late.left_grip.rotation.w};
            snapshot.left_aim_position = {
                late.left_aim.position.x,
                late.left_aim.position.y,
                late.left_aim.position.z};
            snapshot.left_aim_rotation = {
                late.left_aim.rotation.x,
                late.left_aim.rotation.y,
                late.left_aim.rotation.z,
                late.left_aim.rotation.w};
            snapshot.left_valid = true;
        }

        g_late_tracking_active.store(true, std::memory_order_release);
        if (!g_late_tracking_logged.exchange(
                true,
                std::memory_order_acq_rel)) {
            API::get()->log_info(
                "HaloCEMotionControls: coherent late OpenXR tracking is "
                "active at predicted display time %lld (period %lld ns)",
                static_cast<long long>(
                    snapshot.predicted_display_time),
                static_cast<long long>(
                    snapshot.predicted_display_period));
        }
        return snapshot;
    }

    g_late_tracking_active.store(false, std::memory_order_release);
    if (!API::VR::is_runtime_ready() || !API::VR::is_hmd_active()) {
        return snapshot;
    }

    const auto hmd_index = API::VR::get_hmd_index();
    const auto right_index = API::VR::get_right_controller_index();
    const auto left_index = API::VR::get_left_controller_index();
    if (hmd_index < 0 || hmd_index >= 64 || right_index < 0 ||
        right_index >= 64) {
        return snapshot;
    }

    const auto hmd = API::VR::get_pose(hmd_index);
    const auto grip = API::VR::get_grip_pose(right_index);
    const auto aim = API::VR::get_aim_pose(right_index);
    snapshot.hmd_position = {
        hmd.position.x,
        hmd.position.y,
        hmd.position.z};
    snapshot.hmd_rotation = {
        hmd.rotation.x,
        hmd.rotation.y,
        hmd.rotation.z,
        hmd.rotation.w};
    snapshot.right_grip_position = {
        grip.position.x,
        grip.position.y,
        grip.position.z};
    snapshot.right_grip_rotation = {
        grip.rotation.x,
        grip.rotation.y,
        grip.rotation.z,
        grip.rotation.w};
    snapshot.right_aim_position = {
        aim.position.x,
        aim.position.y,
        aim.position.z};
    snapshot.right_aim_rotation = {
        aim.rotation.x,
        aim.rotation.y,
        aim.rotation.z,
        aim.rotation.w};
    snapshot.valid =
        finite(snapshot.hmd_position) &&
        finite(snapshot.hmd_rotation) &&
        finite(snapshot.right_grip_position) &&
        finite(snapshot.right_grip_rotation) &&
        finite(snapshot.right_aim_position) &&
        finite(snapshot.right_aim_rotation);

    if (left_index >= 0 && left_index < 64) {
        const auto left_grip = API::VR::get_grip_pose(left_index);
        const auto left_aim = API::VR::get_aim_pose(left_index);
        snapshot.left_grip_position = {
            left_grip.position.x,
            left_grip.position.y,
            left_grip.position.z};
        snapshot.left_grip_rotation = {
            left_grip.rotation.x,
            left_grip.rotation.y,
            left_grip.rotation.z,
            left_grip.rotation.w};
        snapshot.left_aim_position = {
            left_aim.position.x,
            left_aim.position.y,
            left_aim.position.z};
        snapshot.left_aim_rotation = {
            left_aim.rotation.x,
            left_aim.rotation.y,
            left_aim.rotation.z,
            left_aim.rotation.w};
        snapshot.left_valid =
            finite(snapshot.left_grip_position) &&
            finite(snapshot.left_grip_rotation) &&
            finite(snapshot.left_aim_position) &&
            finite(snapshot.left_aim_rotation);
    }

    return snapshot;
}

void publish_tracking_snapshot(const TrackingSnapshot& snapshot) {
    const std::scoped_lock lock{g_tracking_mutex};
    g_tracking_snapshot = snapshot;
    g_tracking_valid.store(snapshot.valid, std::memory_order_release);
    g_left_tracking_valid.store(
        snapshot.left_valid,
        std::memory_order_release);
}

HaloCEVR_DiagnosticVec3 diagnostic(const Vec3& value) {
    return {value.x, value.y, value.z};
}

HaloCEVR_DiagnosticQuat diagnostic(const Quat& value) {
    return {value.x, value.y, value.z, value.w};
}

HaloCEVR_DiagnosticPose diagnostic(
    const Vec3& position,
    const Quat& rotation) {
    return {diagnostic(position), diagnostic(rotation)};
}

HaloCEVR_DiagnosticMatrix diagnostic(const BlamMatrix4x3& value) {
    return {
        value.scale,
        diagnostic(value.forward),
        diagnostic(value.left),
        diagnostic(value.up),
        diagnostic(value.position)};
}

bool diagnostic_hand_geometry(
    const BlamMatrix4x3* palette,
    std::uint8_t wrist,
    std::uint8_t index_finger,
    std::uint8_t middle_finger,
    std::uint8_t ring_finger,
    std::uint8_t thumb,
    std::uint8_t grip_end,
    HaloCEVR_DiagnosticVec3& hand_forward,
    HaloCEVR_DiagnosticVec3& thumb_side,
    HaloCEVR_DiagnosticVec3& palm_normal,
    HaloCEVR_DiagnosticVec3& grip_forward) {
    const auto wrist_position = palette[wrist].position;
    const auto finger_centroid =
        (palette[index_finger].position +
         palette[middle_finger].position +
         palette[ring_finger].position) /
        3.0f;
    const auto forward = normalized(finger_centroid - wrist_position);
    const auto thumb_delta = palette[thumb].position - wrist_position;
    const auto side = normalized(
        thumb_delta - forward * dot(forward, thumb_delta));
    const auto normal = normalized(cross(forward, side));
    const auto grip = normalized(
        palette[grip_end].position - wrist_position);
    if (!finite(forward) || !finite(side) || !finite(normal) ||
        !finite(grip) || length_squared(forward) < 0.8f ||
        length_squared(side) < 0.8f || length_squared(normal) < 0.8f ||
        length_squared(grip) < 0.8f) {
        return false;
    }

    hand_forward = diagnostic(forward);
    thumb_side = diagnostic(side);
    palm_normal = diagnostic(normal);
    grip_forward = diagnostic(grip);
    return true;
}

void record_visual_pose_diagnostics(
    const TrackingSnapshot& tracking,
    const BlamMatrix4x3* palette) {
    if (palette == nullptr) {
        return;
    }

    const auto sequence =
        g_pose_diagnostic_sequence.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::scoped_lock lock{g_pose_diagnostics_mutex};
    auto& output = g_pose_diagnostics;
    output.size = sizeof(output);
    output.version = 1;
    output.sequence = sequence;
    output.flags |= kDiagnosticVisualValid;
    if (tracking.left_valid) {
        output.flags |= kDiagnosticLeftWristValid;
    } else {
        output.flags &= ~kDiagnosticLeftWristValid;
    }
    output.predicted_display_time = tracking.predicted_display_time;
    output.predicted_display_period = tracking.predicted_display_period;
    output.hmd = diagnostic(
        tracking.hmd_position,
        tracking.hmd_rotation);
    output.right_grip = diagnostic(
        tracking.right_grip_position,
        tracking.right_grip_rotation);
    output.right_aim = diagnostic(
        tracking.right_aim_position,
        tracking.right_aim_rotation);
    output.left_grip = diagnostic(
        tracking.left_grip_position,
        tracking.left_grip_rotation);
    output.left_aim = diagnostic(
        tracking.left_aim_position,
        tracking.left_aim_rotation);
    output.root = diagnostic(palette[0]);
    output.weapon = diagnostic(palette[8]);
    output.right_wrist = diagnostic(palette[19]);
    output.left_wrist = diagnostic(palette[25]);
    if (diagnostic_hand_geometry(
            palette,
            19,
            41,
            36,
            31,
            37,
            45,
            output.right_hand_forward,
            output.right_hand_thumb_side,
            output.right_hand_palm_normal,
            output.right_hand_grip_forward)) {
        output.flags |= kDiagnosticRightHandGeometryValid;
    } else {
        output.flags &= ~kDiagnosticRightHandGeometryValid;
    }
    if (tracking.left_valid &&
        diagnostic_hand_geometry(
            palette,
            25,
            28,
            33,
            38,
            32,
            50,
            output.left_hand_forward,
            output.left_hand_thumb_side,
            output.left_hand_palm_normal,
            output.left_hand_grip_forward)) {
        output.flags |= kDiagnosticLeftHandGeometryValid;
    } else {
        output.flags &= ~kDiagnosticLeftHandGeometryValid;
    }
    output.visual_override_count =
        g_visual_override_count.load(std::memory_order_acquire) + 1;
    output.marker_override_count =
        g_marker_override_count.load(std::memory_order_acquire);
    output.projectile_override_count =
        g_override_count.load(std::memory_order_acquire);
}

void record_marker_pose_diagnostics(
    const BlamMatrix4x3& marker,
    const Vec3& reticle_position) {
    const std::scoped_lock lock{g_pose_diagnostics_mutex};
    g_pose_diagnostics.size = sizeof(g_pose_diagnostics);
    g_pose_diagnostics.version = 1;
    g_pose_diagnostics.flags |= kDiagnosticMarkerValid;
    g_pose_diagnostics.muzzle_marker = diagnostic(marker);
    g_pose_diagnostics.reticle_position = diagnostic(reticle_position);
    g_pose_diagnostics.marker_override_count =
        g_marker_override_count.load(std::memory_order_acquire) + 1;
}

void record_projectile_pose_diagnostics(const ProjectileNewData& projectile) {
    const std::scoped_lock lock{g_pose_diagnostics_mutex};
    g_pose_diagnostics.size = sizeof(g_pose_diagnostics);
    g_pose_diagnostics.version = 1;
    g_pose_diagnostics.flags |= kDiagnosticProjectileValid;
    g_pose_diagnostics.projectile_position =
        diagnostic(projectile.position);
    g_pose_diagnostics.projectile_forward =
        diagnostic(projectile.forward);
    g_pose_diagnostics.projectile_up = diagnostic(projectile.up);
    g_pose_diagnostics.projectile_override_count =
        g_override_count.load(std::memory_order_acquire) + 1;
}

// OpenXR stage coordinates are +X right, +Y up, -Z forward. Halo CE's
// Blam coordinates are +X forward, +Y left, +Z up.
Vec3 openxr_to_blam(const Vec3& value) {
    return {-value.z, -value.x, value.y};
}

Mat3 openxr_rotation_to_blam_basis(const Quat& rotation) {
    return {
        openxr_to_blam(rotate(rotation, {0.0f, 0.0f, -1.0f})),
        openxr_to_blam(rotate(rotation, {-1.0f, 0.0f, 0.0f})),
        openxr_to_blam(rotate(rotation, {0.0f, 1.0f, 0.0f}))};
}

// The controller basis both the visual weapon and the native fire path aim
// with. One-handed this is the right aim rotation as before. While the
// two-handed hold is engaged, the forward axis eases onto the line from the
// right grip to the left grip (Halo-MCC-VR's two-hand aim), with roll still
// taken from the right controller. Sharing this one function between the
// palette build and the marker/projectile hooks keeps the rendered barrel,
// the muzzle ray, and the collision sweep on the same line while blending.
Mat3 effective_controller_basis(
    const TrackingSnapshot& tracking,
    const Quat& inverse_hmd_rotation,
    const Quat& aim_relative_xr) {
    const auto one_hand = openxr_rotation_to_blam_basis(aim_relative_xr);
    const auto blend =
        g_two_hand_hold_blend.load(std::memory_order_relaxed);
    if (blend <= 0.0f || !valid_basis(one_hand)) {
        return one_hand;
    }

    // Hand-to-hand line in the same HMD-relative frame as the aim rotation.
    // If left tracking drops mid-hold, ease the blend tail out along the
    // last tracked line instead of snapping back to one-handed aim.
    Vec3 two_hand_forward{};
    if (tracking.left_valid) {
        const auto hand_line_xr = rotate(
            inverse_hmd_rotation,
            tracking.left_grip_position - tracking.right_grip_position);
        two_hand_forward = openxr_to_blam(hand_line_xr);
        const auto line_length_squared = length_squared(two_hand_forward);
        if (!finite(two_hand_forward) || line_length_squared < 1.0e-6f) {
            return one_hand;
        }
        two_hand_forward =
            two_hand_forward / std::sqrt(line_length_squared);
        g_two_hand_last_forward.store(
            two_hand_forward,
            std::memory_order_relaxed);
    } else {
        two_hand_forward = g_two_hand_last_forward.load(
            std::memory_order_relaxed);
        if (!finite(two_hand_forward) ||
            length_squared(two_hand_forward) < 0.5f) {
            return one_hand;
        }
    }

    // Fade the two-hand influence in across the agreement band rather than
    // hard-gating: below the minimum aim is exactly one-handed, and the
    // smoothstep keeps the transition continuous when a latched support
    // hand crosses the boundary.
    const auto agreement = dot(two_hand_forward, one_hand.forward);
    if (!std::isfinite(agreement) ||
        agreement < kTwoHandMinimumAgreement) {
        return one_hand;
    }
    const auto band = std::clamp(
        (agreement - kTwoHandMinimumAgreement) /
            (kTwoHandFullAgreement - kTwoHandMinimumAgreement),
        0.0f,
        1.0f);
    const auto weight = blend * band * band * (3.0f - 2.0f * band);

    const auto blended_forward = normalized(
        one_hand.forward +
        (two_hand_forward - one_hand.forward) * weight);
    auto blended_left = cross(one_hand.up, blended_forward);
    if (!finite(blended_forward) ||
        length_squared(blended_left) < 1.0e-6f) {
        return one_hand;
    }
    blended_left = normalized(blended_left);
    const auto blended_up = normalized(
        cross(blended_forward, blended_left));
    const Mat3 result{blended_forward, blended_left, blended_up};
    return valid_basis(result) ? result : one_hand;
}

bool build_controller_marker(
    const MarkerResult& source,
    const TrackingSnapshot& tracking,
    BlamMatrix4x3& destination,
    Vec3& reticle_position) {
    if (!tracking.valid || !finite(source.local.position) ||
        !finite(source.world.position)) {
        return false;
    }

    const auto hmd_rotation = normalized(tracking.hmd_rotation);
    const auto aim_rotation = normalized(tracking.right_aim_rotation);
    if (!finite(hmd_rotation) || !finite(aim_rotation)) {
        return false;
    }

    const auto local_basis = orthonormal_basis(source.local);
    const auto world_basis = orthonormal_basis(source.world);
    if (!valid_basis(local_basis) || !valid_basis(world_basis)) {
        return false;
    }

    // The marker result gives both object-local and world matrices. Removing
    // the local marker transform recovers the current first-person weapon
    // root in Blam world space without relying on an absolute UE/Blam origin.
    const auto root_basis = multiply(world_basis, transpose(local_basis));
    if (!valid_basis(root_basis)) {
        return false;
    }
    const auto root_position =
        source.world.position -
        transform_vector(root_basis, source.local.position);

    const auto inverse_hmd_rotation = conjugate(hmd_rotation);
    const auto grip_delta_xr = rotate(
        inverse_hmd_rotation,
        tracking.right_grip_position - tracking.hmd_position);
    const auto aim_relative_xr =
        normalized(inverse_hmd_rotation * aim_rotation);
    if (!finite(grip_delta_xr) || !finite(aim_relative_xr)) {
        return false;
    }

    const auto controller_basis = effective_controller_basis(
        tracking, inverse_hmd_rotation, aim_relative_xr);
    if (!valid_basis(controller_basis)) {
        return false;
    }

    const auto desired_root_basis = multiply(root_basis, controller_basis);
    const auto grip_delta_blam =
        openxr_to_blam(grip_delta_xr) / kMetersPerBlamUnit;
    const auto desired_root_position =
        root_position + transform_vector(root_basis, grip_delta_blam);

    destination = source.world;
    destination.position =
        desired_root_position +
        transform_vector(desired_root_basis, source.local.position);

    // The visible ball is ten metres along the controller aim ray, but a
    // projectile begins at the weapon's displaced muzzle. Aim the native
    // marker at that exact convergence point instead of keeping the muzzle
    // ray merely parallel to the controller ray. Otherwise the two rays
    // retain their grip-to-muzzle lateral offset and never actually meet.
    constexpr float kReticleDistanceMeters = 10.0f;
    reticle_position =
        desired_root_position +
        desired_root_basis.forward *
            (kReticleDistanceMeters / kMetersPerBlamUnit);
    const auto converged_forward =
        normalized(reticle_position - destination.position);
    const auto converged_left = normalized(
        cross(desired_root_basis.up, converged_forward));
    const auto converged_up =
        normalized(cross(converged_forward, converged_left));
    destination.forward = converged_forward;
    destination.left = converged_left;
    destination.up = converged_up;

    return finite(destination.position) &&
           valid_basis(orthonormal_basis(destination));
}

bool reasonable_palette_matrix(const BlamMatrix4x3& matrix) {
    if (!std::isfinite(matrix.scale) || !finite(matrix.forward) ||
        !finite(matrix.left) || !finite(matrix.up) ||
        !finite(matrix.position)) {
        return false;
    }

    constexpr float kMaximumBasisComponent = 4.0f;
    constexpr float kMaximumPositionMagnitudeSquared = 10000.0f;
    const auto component_reasonable = [](const Vec3& value) {
        return std::abs(value.x) <= kMaximumBasisComponent &&
               std::abs(value.y) <= kMaximumBasisComponent &&
               std::abs(value.z) <= kMaximumBasisComponent;
    };
    return std::abs(matrix.scale) <= 16.0f &&
           component_reasonable(matrix.forward) &&
           component_reasonable(matrix.left) &&
           component_reasonable(matrix.up) &&
           length_squared(matrix.position) <=
               kMaximumPositionMagnitudeSquared;
}

void apply_rigid_delta(
    BlamMatrix4x3* palette,
    std::span<const std::uint8_t> nodes,
    const Mat3& rotation,
    const Vec3& pivot) {
    for (const auto node : nodes) {
        auto& matrix = palette[node];
        matrix.forward =
            normalized(transform_vector(rotation, matrix.forward));
        matrix.left =
            normalized(transform_vector(rotation, matrix.left));
        matrix.up =
            normalized(transform_vector(rotation, matrix.up));
        matrix.position =
            pivot + transform_vector(rotation, matrix.position - pivot);
    }
}

bool place_wrist_subtree(
    BlamMatrix4x3* palette,
    std::uint8_t wrist_node,
    std::span<const std::uint8_t> wrist_nodes,
    const Vec3& requested_wrist_position,
    const Mat3& desired_wrist_basis) {
    if (palette == nullptr || !finite(requested_wrist_position) ||
        !valid_basis(desired_wrist_basis)) {
        return false;
    }

    const auto source_wrist_position = palette[wrist_node].position;
    const auto source_wrist_basis =
        orthonormal_basis(palette[wrist_node]);
    const auto rotation = multiply(
        desired_wrist_basis,
        transpose(source_wrist_basis));
    if (!finite(source_wrist_position) ||
        !valid_basis(source_wrist_basis) ||
        !valid_basis(rotation)) {
        return false;
    }

    const auto translation =
        requested_wrist_position -
        transform_vector(rotation, source_wrist_position);
    if (!finite(translation)) {
        return false;
    }

    for (const auto node : wrist_nodes) {
        auto& matrix = palette[node];
        matrix.forward =
            normalized(transform_vector(rotation, matrix.forward));
        matrix.left =
            normalized(transform_vector(rotation, matrix.left));
        matrix.up =
            normalized(transform_vector(rotation, matrix.up));
        matrix.position =
            translation + transform_vector(rotation, matrix.position);
    }

    return length_squared(
               palette[wrist_node].position - requested_wrist_position) <
           1.0e-8f;
}

bool place_floating_hand_only(
    BlamMatrix4x3* palette,
    std::uint8_t wrist_node,
    std::span<const std::uint8_t> arm_nodes,
    std::span<const std::uint8_t> wrist_nodes,
    const Vec3& requested_wrist_position,
    const Mat3& desired_wrist_basis) {
    if (!place_wrist_subtree(
            palette,
            wrist_node,
            wrist_nodes,
            requested_wrist_position,
            desired_wrist_basis)) {
        return false;
    }

    // Floating-hands mode must not let Halo's authored arm length influence
    // the tracked wrist. Collapse every non-hand arm bone into the wrist
    // instead of solving/clamping an IK chain and stretching the seam back to
    // the exact controller pose. Keeping a tiny finite scale avoids singular
    // palette entries while hiding the upper-arm/forearm geometry inside the
    // controller-owned hand.
    constexpr float kHiddenArmScale = 1.0e-4f;
    for (const auto node : arm_nodes) {
        if (std::find(wrist_nodes.begin(), wrist_nodes.end(), node) !=
            wrist_nodes.end()) {
            continue;
        }

        auto& matrix = palette[node];
        matrix.scale = kHiddenArmScale;
        matrix.forward = desired_wrist_basis.forward;
        matrix.left = desired_wrist_basis.left;
        matrix.up = desired_wrist_basis.up;
        matrix.position = requested_wrist_position;
    }
    return true;
}

bool solve_two_bone_arm(
    BlamMatrix4x3* palette,
    std::uint8_t shoulder_node,
    std::uint8_t elbow_node,
    std::uint8_t wrist_node,
    std::span<const std::uint8_t> shoulder_nodes,
    std::span<const std::uint8_t> elbow_nodes,
    std::span<const std::uint8_t> wrist_nodes,
    const Vec3& requested_wrist_position,
    const Mat3& desired_wrist_basis,
    const Vec3& fallback_pole) {
    auto shoulder_position = palette[shoulder_node].position;
    auto elbow_position = palette[elbow_node].position;
    const auto wrist_position = palette[wrist_node].position;
    const auto upper_length = std::sqrt(
        length_squared(elbow_position - shoulder_position));
    const auto lower_length = std::sqrt(
        length_squared(wrist_position - elbow_position));
    if (!std::isfinite(upper_length) || !std::isfinite(lower_length) ||
        upper_length < 1.0e-4f || lower_length < 1.0e-4f ||
        !finite(requested_wrist_position) ||
        !valid_basis(desired_wrist_basis)) {
        return false;
    }

    auto target_delta = requested_wrist_position - shoulder_position;
    auto target_distance = std::sqrt(length_squared(target_delta));
    if (!std::isfinite(target_distance) || target_distance < 1.0e-4f) {
        return false;
    }

    const auto target_direction = target_delta / target_distance;
    const auto minimum_reach =
        std::abs(upper_length - lower_length) + 1.0e-4f;
    const auto maximum_reach =
        upper_length + lower_length - 1.0e-4f;

    // Clavicle assist: instead of stopping the hand at the reach sphere,
    // slide the whole arm root toward an out-of-reach target, the way
    // Roboquest's clavicle-rooted FABRIK chain rolls the shoulder into an
    // overreach. The remaining shortfall is clamped as before and absorbed
    // by the caller's exact wrist placement.
    const auto overshoot = target_distance - maximum_reach;
    if (overshoot > 0.0f) {
        const auto assist = std::min(
            overshoot, kClavicleAssistMaxMeters / kMetersPerBlamUnit);
        const auto assist_offset = target_direction * assist;
        for (const auto node : shoulder_nodes) {
            palette[node].position = palette[node].position + assist_offset;
        }
        shoulder_position = shoulder_position + assist_offset;
        elbow_position = elbow_position + assist_offset;
        target_distance -= assist;
    }

    target_distance =
        std::clamp(target_distance, minimum_reach, maximum_reach);
    const auto wrist_target =
        shoulder_position + target_direction * target_distance;

    auto pole = elbow_position - shoulder_position;
    pole = pole - target_direction * dot(pole, target_direction);
    if (length_squared(pole) < 1.0e-6f) {
        pole = fallback_pole -
               target_direction * dot(fallback_pole, target_direction);
    }
    if (length_squared(pole) < 1.0e-6f) {
        pole = cross(target_direction, {0.0f, 0.0f, 1.0f});
    }
    pole = normalized(pole);
    if (length_squared(pole) < 0.8f) {
        return false;
    }

    const auto along =
        (target_distance * target_distance +
         upper_length * upper_length -
         lower_length * lower_length) /
        (2.0f * target_distance);
    const auto height_squared =
        std::max(upper_length * upper_length - along * along, 0.0f);
    const auto elbow_target =
        shoulder_position + target_direction * along +
        pole * std::sqrt(height_squared);

    const auto shoulder_rotation = rotation_between(
        elbow_position - shoulder_position,
        elbow_target - shoulder_position);
    if (!valid_basis(shoulder_rotation)) {
        return false;
    }
    apply_rigid_delta(
        palette,
        shoulder_nodes,
        shoulder_rotation,
        shoulder_position);

    const auto moved_elbow = palette[elbow_node].position;
    const auto moved_wrist = palette[wrist_node].position;
    const auto elbow_rotation = rotation_between(
        moved_wrist - moved_elbow,
        wrist_target - moved_elbow);
    if (!valid_basis(elbow_rotation)) {
        return false;
    }
    apply_rigid_delta(
        palette,
        elbow_nodes,
        elbow_rotation,
        moved_elbow);

    const auto final_wrist_position = palette[wrist_node].position;
    const auto current_wrist_basis =
        orthonormal_basis(palette[wrist_node]);
    const auto wrist_rotation = multiply(
        desired_wrist_basis,
        transpose(current_wrist_basis));
    if (!valid_basis(current_wrist_basis) ||
        !valid_basis(wrist_rotation)) {
        return false;
    }
    apply_rigid_delta(
        palette,
        wrist_nodes,
        wrist_rotation,
        final_wrist_position);
    return true;
}

bool solve_visual_arm_for_floating_wrist(
    BlamMatrix4x3* palette,
    std::uint8_t shoulder_node,
    std::uint8_t elbow_node,
    std::uint8_t wrist_node,
    std::span<const std::uint8_t> shoulder_nodes,
    std::span<const std::uint8_t> elbow_nodes,
    std::span<const std::uint8_t> wrist_nodes,
    const Vec3& requested_wrist_position,
    const Mat3& desired_wrist_basis,
    const Vec3& fallback_pole) {
    if (palette == nullptr) {
        return false;
    }

    // The arm solve is cosmetic; the wrist is controller-owned. Solve on a
    // temporary palette so a degenerate or partially-applied arm solution can
    // never disturb the tracked hand. If it succeeds, reapply the exact
    // floating-wrist transform last; this makes shoulder/elbow reach purely
    // visual and keeps the wrist at the tracked pose even beyond Halo's
    // authored arm length (the clavicle assist inside the solver closes most
    // of that gap first).
    std::array<BlamMatrix4x3, kFirstPersonNodeCount> visual_palette{};
    std::copy_n(
        palette,
        kFirstPersonNodeCount,
        visual_palette.begin());
    if (solve_two_bone_arm(
            visual_palette.data(),
            shoulder_node,
            elbow_node,
            wrist_node,
            shoulder_nodes,
            elbow_nodes,
            wrist_nodes,
            requested_wrist_position,
            desired_wrist_basis,
            fallback_pole) &&
        place_wrist_subtree(
            visual_palette.data(),
            wrist_node,
            wrist_nodes,
            requested_wrist_position,
            desired_wrist_basis)) {
        std::copy(
            visual_palette.begin(),
            visual_palette.end(),
            palette);
        return true;
    }

    // The floating hand remains authoritative if the visual arm cannot solve.
    return place_wrist_subtree(
        palette,
        wrist_node,
        wrist_nodes,
        requested_wrist_position,
        desired_wrist_basis);
}

// Yaw-only torso frame derived from the view root, after RoboquestVR's
// UpdateFPSMeshTransform: the arms rig follows the head's position and yaw
// but never its pitch or roll, so looking down does not rotate the shoulders
// down into the player's view.
Mat3 torso_basis_from_root(const Mat3& root_basis) {
    Vec3 flat_forward{
        root_basis.forward.x, root_basis.forward.y, 0.0f};
    if (length_squared(flat_forward) < 1.0e-6f) {
        // Looking straight up or down: the camera up axis carries the yaw.
        const auto sign = root_basis.forward.z <= 0.0f ? 1.0f : -1.0f;
        flat_forward = {
            root_basis.up.x * sign, root_basis.up.y * sign, 0.0f};
    }
    flat_forward = normalized(flat_forward);
    constexpr Vec3 up{0.0f, 0.0f, 1.0f};
    return Mat3{flat_forward, cross(up, flat_forward), up};
}

// Rigidly translate an arm subtree so its shoulder hangs from the yaw-only
// torso frame at a fixed human-proportioned offset behind and below the
// head, instead of wherever Halo's camera-glued stock viewmodel put it this
// frame. Pure translation: the stock pose within the arm is preserved for
// the IK solve that follows.
bool anchor_shoulder_to_torso(
    BlamMatrix4x3* palette,
    std::uint8_t shoulder_node,
    std::span<const std::uint8_t> arm_nodes,
    const Mat3& torso_basis,
    const Vec3& head_position,
    bool left_side) {
    const Vec3 local_offset{
        -kShoulderBackMeters / kMetersPerBlamUnit,
        (left_side ? kShoulderLateralMeters : -kShoulderLateralMeters) /
            kMetersPerBlamUnit,
        -kShoulderDownMeters / kMetersPerBlamUnit};
    const auto anchor =
        head_position + transform_vector(torso_basis, local_offset);
    const auto shift = anchor - palette[shoulder_node].position;
    if (!finite(shift)) {
        return false;
    }
    for (const auto node : arm_nodes) {
        palette[node].position = palette[node].position + shift;
    }
    return true;
}

bool apply_split_controllers_to_first_person_palette(
    BlamMatrix4x3* palette,
    std::int32_t count,
    const TrackingSnapshot& tracking) {
    if (palette == nullptr ||
        count != static_cast<std::int32_t>(kFirstPersonNodeCount) ||
        !tracking.valid) {
        return false;
    }

    // Baboon confirms all 16 shipped Campaign Evolved first-person skeletons
    // use this same 76-node topology. Keep the three sibling trees separate:
    // right shoulder 5, left shoulder 6, and weapon 7. The weapon subtree is
    // only 7/8/22; each wrist has its own exact descendants below.
    for (std::uint8_t node = 5;
         node < static_cast<std::uint8_t>(kFirstPersonNodeCount);
         ++node) {
        if (!reasonable_palette_matrix(palette[node])) {
            return false;
        }
    }
    if (!reasonable_palette_matrix(palette[0])) {
        return false;
    }

    const auto hmd_rotation = normalized(tracking.hmd_rotation);
    const auto aim_rotation = normalized(tracking.right_aim_rotation);
    if (!finite(hmd_rotation) || !finite(aim_rotation)) {
        return false;
    }

    const auto root_basis = orthonormal_basis(palette[0]);
    const auto weapon_basis = orthonormal_basis(palette[8]);
    if (!valid_basis(root_basis) || !valid_basis(weapon_basis)) {
        return false;
    }

    const auto inverse_hmd_rotation = conjugate(hmd_rotation);
    const auto grip_delta_xr = rotate(
        inverse_hmd_rotation,
        tracking.right_grip_position - tracking.hmd_position);
    const auto aim_relative_xr =
        normalized(inverse_hmd_rotation * aim_rotation);
    if (!finite(grip_delta_xr) || !finite(aim_relative_xr)) {
        return false;
    }

    const auto controller_basis = effective_controller_basis(
        tracking, inverse_hmd_rotation, aim_relative_xr);
    if (!valid_basis(controller_basis)) {
        return false;
    }

    // The first-person "primaryweapon" attachment on node 8 is authored with
    // local +Y pointing down the visible barrel, while Blam's
    // "primary_trigger" projectile marker uses local +X. If node 8 receives
    // the controller basis directly, the rendered gun points along the
    // controller's left axis even though projectiles correctly travel along
    // its forward axis. Rotate only the visual attachment basis so node +Y
    // maps to controller +X; the native muzzle/projectile path remains +X.
    const Mat3 visual_weapon_basis{
        controller_basis.left * -1.0f,
        controller_basis.forward,
        controller_basis.up};
    if (!valid_basis(visual_weapon_basis)) {
        return false;
    }

    // The identity "primaryweapon" marker is authored on node 8. Construct
    // its desired pose from the composed view root and HMD-relative
    // controller pose, then derive one rigid left-delta for the whole weapon
    // branch. This preserves stock animation within the branch and never
    // touches the camera-control node.
    const auto desired_weapon_basis =
        multiply(root_basis, visual_weapon_basis);
    const auto grip_delta_blam =
        openxr_to_blam(grip_delta_xr) / kMetersPerBlamUnit;
    const auto desired_weapon_position =
        palette[0].position +
        transform_vector(root_basis, grip_delta_blam);

    const auto delta_basis =
        multiply(desired_weapon_basis, transpose(weapon_basis));
    if (!valid_basis(delta_basis)) {
        return false;
    }
    const auto delta_position =
        desired_weapon_position -
        transform_vector(delta_basis, palette[8].position);

    constexpr std::array<std::uint8_t, 3> weapon_nodes{7, 8, 22};
    for (const auto node : weapon_nodes) {
        auto& matrix = palette[node];
        matrix.forward =
            normalized(transform_vector(delta_basis, matrix.forward));
        matrix.left =
            normalized(transform_vector(delta_basis, matrix.left));
        matrix.up =
            normalized(transform_vector(delta_basis, matrix.up));
        matrix.position =
            delta_position +
            transform_vector(delta_basis, matrix.position);
    }

    constexpr std::array<std::uint8_t, 34> right_shoulder_nodes{
        5, 10, 11, 12, 16, 17, 18, 19, 20, 21, 30, 31, 34, 36, 37, 40,
        41, 43, 44, 45, 48, 49, 53, 55, 56, 59, 60, 63, 65, 66, 69, 70,
        73, 74};
    constexpr std::array<std::uint8_t, 29> right_elbow_nodes{
        16, 17, 18, 19, 20, 30, 31, 34, 36, 37, 40, 41, 43, 44, 45,
        48, 49, 53, 55, 56, 59, 60, 63, 65, 66, 69, 70, 73, 74};
    constexpr std::array<std::uint8_t, 24> right_wrist_nodes{
        19, 30, 31, 36, 37, 40, 41, 43, 44, 45, 48, 49,
        53, 55, 56, 59, 60, 63, 65, 66, 69, 70, 73, 74};
    constexpr std::array<std::uint8_t, 34> left_shoulder_nodes{
        6, 9, 13, 14, 15, 23, 24, 25, 26, 27, 28, 29, 32, 33, 35, 38,
        39, 42, 46, 47, 50, 51, 52, 54, 57, 58, 61, 62, 64, 67, 68, 71,
        72, 75};
    constexpr std::array<std::uint8_t, 29> left_elbow_nodes{
        9, 24, 25, 26, 27, 28, 29, 32, 33, 35, 38, 39, 42, 46, 47,
        50, 51, 52, 54, 57, 58, 61, 62, 64, 67, 68, 71, 72, 75};
    constexpr std::array<std::uint8_t, 24> left_wrist_nodes{
        25, 28, 29, 32, 33, 38, 39, 42, 46, 47, 50, 51,
        52, 54, 57, 58, 61, 62, 64, 67, 68, 71, 72, 75};
    const auto use_arm_ik =
        g_two_hand_ik_enabled.load(std::memory_order_relaxed);
    const auto torso_basis = torso_basis_from_root(root_basis);

    const auto source_right_wrist_basis =
        orthonormal_basis(palette[19]);
    const auto right_wrist_target =
        delta_position +
        transform_vector(delta_basis, palette[19].position);
    const auto right_wrist_basis =
        multiply(delta_basis, source_right_wrist_basis);
    // Anchor after the wrist target is captured from the stock pose: the
    // target belongs to the controller, not to the relocated arm.
    if (use_arm_ik &&
        !anchor_shoulder_to_torso(
            palette,
            5,
            right_shoulder_nodes,
            torso_basis,
            palette[0].position,
            false)) {
        return false;
    }
    const auto right_hand_placed =
        use_arm_ik
        ? solve_visual_arm_for_floating_wrist(
              palette,
              5,
              16,
              19,
              right_shoulder_nodes,
              right_elbow_nodes,
              right_wrist_nodes,
              right_wrist_target,
              right_wrist_basis,
              root_basis.up)
        : place_floating_hand_only(
              palette,
              19,
              right_shoulder_nodes,
              right_wrist_nodes,
              right_wrist_target,
              right_wrist_basis);
    if (!right_hand_placed) {
        return false;
    }

    if (tracking.left_valid) {
        const auto left_grip_rotation =
            normalized(tracking.left_grip_rotation);
        const auto left_grip_delta_xr = rotate(
            inverse_hmd_rotation,
            tracking.left_grip_position - tracking.hmd_position);
        const auto left_relative_xr =
            normalized(inverse_hmd_rotation * left_grip_rotation);
        const auto left_controller_basis =
            openxr_rotation_to_blam_basis(left_relative_xr);
        // The stock wrist-to-view orientation is the constant that maps
        // controller axes onto Halo's left wrist bone convention. Latch it
        // once: re-reading it every frame leaked the running stock animation
        // into the tracked hand as a per-frame rotation wobble.
        if (!g_left_wrist_stock_latched.load(std::memory_order_acquire)) {
            const auto stock_left_wrist_basis =
                orthonormal_basis(palette[25]);
            if (!valid_basis(stock_left_wrist_basis)) {
                return false;
            }
            g_left_wrist_stock_relative =
                multiply(transpose(root_basis), stock_left_wrist_basis);
            g_left_wrist_stock_latched.store(
                true, std::memory_order_release);
        }
        const auto desired_left_wrist_basis = multiply(
            multiply(root_basis, left_controller_basis),
            g_left_wrist_stock_relative);
        const auto left_grip_delta_blam =
            openxr_to_blam(left_grip_delta_xr) / kMetersPerBlamUnit;
        // Place the wrist bone behind the grip point rather than on it: the
        // OpenXR grip pose is the palm centroid, and Roboquest-style fixed
        // offsets in controller space put the knuckles on the controller
        // with no per-user calibration.
        const Vec3 left_wrist_local_offset{
            -kGripToWristBackMeters / kMetersPerBlamUnit,
            0.0f,
            -kGripToWristDownMeters / kMetersPerBlamUnit};
        const auto left_wrist_target =
            palette[0].position +
            transform_vector(root_basis, left_grip_delta_blam) +
            transform_vector(
                multiply(root_basis, left_controller_basis),
                left_wrist_local_offset);
        if (use_arm_ik &&
            !anchor_shoulder_to_torso(
                palette,
                6,
                left_shoulder_nodes,
                torso_basis,
                palette[0].position,
                true)) {
            return false;
        }
        const auto left_hand_placed =
            use_arm_ik
            ? solve_visual_arm_for_floating_wrist(
                  palette,
                  6,
                  9,
                  25,
                  left_shoulder_nodes,
                  left_elbow_nodes,
                  left_wrist_nodes,
                  left_wrist_target,
                  desired_left_wrist_basis,
                  root_basis.up)
            : place_floating_hand_only(
                  palette,
                  25,
                  left_shoulder_nodes,
                  left_wrist_nodes,
                  left_wrist_target,
                  desired_left_wrist_basis);
        if (!finite(left_grip_delta_xr) ||
            !valid_basis(left_controller_basis) ||
            !left_hand_placed) {
            return false;
        }
    }

    for (std::uint8_t node = 5;
         node < static_cast<std::uint8_t>(kFirstPersonNodeCount);
         ++node) {
        if (!reasonable_palette_matrix(palette[node]) ||
            !valid_basis(orthonormal_basis(palette[node]))) {
            return false;
        }
    }
    return true;
}

bool apply_legacy_controller_to_first_person_palette(
    BlamMatrix4x3* palette,
    std::int32_t count,
    const TrackingSnapshot& tracking) {
    if (palette == nullptr ||
        count != static_cast<std::int32_t>(kFirstPersonNodeCount) ||
        !tracking.valid) {
        return false;
    }

    const auto hmd_rotation = normalized(tracking.hmd_rotation);
    const auto aim_rotation = normalized(tracking.right_aim_rotation);
    const auto root_basis = orthonormal_basis(palette[0]);
    const auto weapon_basis = orthonormal_basis(palette[8]);
    if (!finite(hmd_rotation) || !finite(aim_rotation) ||
        !valid_basis(root_basis) || !valid_basis(weapon_basis)) {
        return false;
    }

    const auto inverse_hmd_rotation = conjugate(hmd_rotation);
    const auto grip_delta_xr = rotate(
        inverse_hmd_rotation,
        tracking.right_grip_position - tracking.hmd_position);
    const auto aim_relative_xr =
        normalized(inverse_hmd_rotation * aim_rotation);
    // Share the two-hand-aware basis with the split path and the fire
    // hooks: this fallback must not render a one-handed weapon while the
    // native muzzle keeps following the hand-to-hand line.
    const auto controller_basis = effective_controller_basis(
        tracking, inverse_hmd_rotation, aim_relative_xr);
    const Mat3 visual_weapon_basis{
        controller_basis.left * -1.0f,
        controller_basis.forward,
        controller_basis.up};
    if (!finite(grip_delta_xr) || !finite(aim_relative_xr) ||
        !valid_basis(controller_basis) ||
        !valid_basis(visual_weapon_basis)) {
        return false;
    }

    const auto desired_weapon_basis =
        multiply(root_basis, visual_weapon_basis);
    const auto grip_delta_blam =
        openxr_to_blam(grip_delta_xr) / kMetersPerBlamUnit;
    const auto desired_weapon_position =
        palette[0].position +
        transform_vector(root_basis, grip_delta_blam);
    const auto delta_basis =
        multiply(desired_weapon_basis, transpose(weapon_basis));
    const auto delta_position =
        desired_weapon_position -
        transform_vector(delta_basis, palette[8].position);
    if (!valid_basis(delta_basis) || !finite(delta_position)) {
        return false;
    }

    std::array<BlamMatrix4x3, kFirstPersonNodeCount - 5> transformed{};
    for (std::uint8_t node = 5;
         node < static_cast<std::uint8_t>(kFirstPersonNodeCount);
         ++node) {
        auto matrix = palette[node];
        matrix.forward =
            normalized(transform_vector(delta_basis, matrix.forward));
        matrix.left =
            normalized(transform_vector(delta_basis, matrix.left));
        matrix.up =
            normalized(transform_vector(delta_basis, matrix.up));
        matrix.position =
            delta_position +
            transform_vector(delta_basis, matrix.position);
        if (!reasonable_palette_matrix(matrix) ||
            !valid_basis(orthonormal_basis(matrix))) {
            return false;
        }
        transformed[node - 5] = matrix;
    }
    for (std::uint8_t node = 5;
         node < static_cast<std::uint8_t>(kFirstPersonNodeCount);
         ++node) {
        palette[node] = transformed[node - 5];
    }
    return true;
}

bool apply_controller_to_first_person_palette(
    BlamMatrix4x3* palette,
    std::int32_t count,
    const TrackingSnapshot& tracking) {
    if (palette == nullptr ||
        count != static_cast<std::int32_t>(kFirstPersonNodeCount)) {
        return false;
    }

    std::array<BlamMatrix4x3, kFirstPersonNodeCount> stock{};
    std::copy_n(palette, kFirstPersonNodeCount, stock.begin());
    if (apply_split_controllers_to_first_person_palette(
            palette,
            count,
            tracking)) {
        return true;
    }

    if (!g_two_hand_ik_fallback_logged.exchange(
            true,
            std::memory_order_relaxed)) {
        API::get()->log_warn(
            "HaloCEMotionControls: split hand placement rejected this palette; "
            "restoring it and using the validated rigid right-hand path");
    }
    std::copy(stock.begin(), stock.end(), palette);
    return apply_legacy_controller_to_first_person_palette(
        palette,
        count,
        tracking);
}

std::string current_executable_name() {
    std::array<char, MAX_PATH> path{};
    const auto size = GetModuleFileNameA(nullptr, path.data(), path.size());
    std::string result{path.data(), size};
    const auto separator = result.find_last_of("\\/");
    if (separator != std::string::npos) {
        result.erase(0, separator + 1);
    }

    std::ranges::transform(result, result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

std::uint8_t* validate_signature_at_rva(
    HMODULE module,
    std::size_t rva,
    const std::vector<int>& pattern,
    const char* label) {
    auto& api = API::get();
    if (module == nullptr || pattern.empty()) {
        return nullptr;
    }

    const auto base = reinterpret_cast<std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        api->log_error(
            "HaloCEMotionControls: %s module has no DOS header; "
            "native muzzle override remains disabled",
            label);
        return nullptr;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        rva > nt->OptionalHeader.SizeOfImage ||
        pattern.size() > nt->OptionalHeader.SizeOfImage - rva) {
        api->log_error(
            "HaloCEMotionControls: %s RVA 0x%zx is outside the module; "
            "native muzzle override remains disabled",
            label,
            rva);
        return nullptr;
    }

    bool in_executable_section = false;
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (std::uint16_t index = 0;
         index < nt->FileHeader.NumberOfSections;
         ++index, ++section) {
        const auto section_begin =
            static_cast<std::size_t>(section->VirtualAddress);
        const auto section_size = static_cast<std::size_t>(
            std::max(section->Misc.VirtualSize, section->SizeOfRawData));
        const auto section_end = section_begin + section_size;
        if (rva >= section_begin && rva + pattern.size() <= section_end &&
            (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) {
            in_executable_section = true;
            break;
        }
    }
    if (!in_executable_section) {
        api->log_error(
            "HaloCEMotionControls: %s RVA 0x%zx is not executable; "
            "native muzzle override remains disabled",
            label,
            rva);
        return nullptr;
    }

    auto* candidate = base + rva;
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        const auto expected = pattern[index];
        if (expected >= 0 &&
            candidate[index] != static_cast<std::uint8_t>(expected)) {
            api->log_error(
                "HaloCEMotionControls: %s signature mismatch at "
                "RVA 0x%zx (+0x%zx); native muzzle override remains disabled",
                label,
                rva,
                index);
            return nullptr;
        }
    }

    return candidate;
}

std::int16_t hook_get_markers(
    std::uint32_t object_index,
    std::int32_t marker_string_id,
    MarkerResult* output,
    std::uint16_t maximum_count,
    std::uint8_t use_exact_object,
    void* unused_context,
    std::uint8_t use_interpolated_transform) {
    const auto* return_address = _ReturnAddress();
    const auto count = g_original_get_markers(
        object_index,
        marker_string_id,
        output,
        maximum_count,
        use_exact_object,
        unused_context,
        use_interpolated_transform);

    // While zoomed the stock crosshair is restored, so scoped shots keep
    // Halo's stock screen-center aim: skipping the marker rewrite leaves no
    // corrections for the projectile/sweep hooks to apply either.
    if (!g_native_override_enabled.load(std::memory_order_relaxed) ||
        g_local_zoomed.load(std::memory_order_acquire) ||
        g_local_fire_depth == 0 || return_address != g_primary_marker_return ||
        maximum_count != 64 || output == nullptr || count <= 0 ||
        count > static_cast<std::int16_t>(maximum_count)) {
        return count;
    }

    const auto tracking = g_local_fire_tracking;
    if (!tracking.valid) {
        return count;
    }

    std::uint32_t rewritten{};
    BlamMatrix4x3 first_source{};
    BlamMatrix4x3 first_desired{};
    bool have_first_rewrite{};
    for (std::int16_t index = 0; index < count; ++index) {
        BlamMatrix4x3 desired{};
        Vec3 reticle_position{};
        if (!build_controller_marker(
                output[index],
                tracking,
                desired,
                reticle_position)) {
            continue;
        }

        if (!have_first_rewrite) {
            first_source = output[index].world;
            first_desired = desired;
            record_marker_pose_diagnostics(desired, reticle_position);
            have_first_rewrite = true;
        }
        if (g_local_fire_marker_correction_count <
            g_local_fire_marker_corrections.size()) {
            g_local_fire_marker_corrections
                [g_local_fire_marker_correction_count++] = {
                    output[index].world,
                    desired,
                    reticle_position};
        }
        output[index].world = desired;
        ++rewritten;
    }

    if (rewritten > 0) {
        const auto total = g_marker_override_count.fetch_add(
                               rewritten,
                               std::memory_order_relaxed) +
                           rewritten;
        if (total <= 8) {
            API::get()->log_info(
                "HaloCEMotionControls: redirected %u native muzzle marker(s) "
                "for weapon 0x%08x "
                "pos (%.3f %.3f %.3f)->(%.3f %.3f %.3f) "
                "forward (%.3f %.3f %.3f)->(%.3f %.3f %.3f) "
                "(total %u)",
                rewritten,
                object_index,
                first_source.position.x,
                first_source.position.y,
                first_source.position.z,
                first_desired.position.x,
                first_desired.position.y,
                first_desired.position.z,
                first_source.forward.x,
                first_source.forward.y,
                first_source.forward.z,
                first_desired.forward.x,
                first_desired.forward.y,
                first_desired.forward.z,
                total);
        }
    }

    return count;
}

std::int64_t hook_projectile_new(ProjectileNewData* data) {
    Mat3 applied_delta{};
    Vec3 source_forward{};
    Vec3 desired_forward{};
    Vec3 center_forward{};
    Vec3 reticle_position{};
    bool have_applied_delta = false;

    const auto* return_address = _ReturnAddress();
    if (data != nullptr &&
        g_native_override_enabled.load(std::memory_order_relaxed) &&
        g_local_fire_depth > 0 &&
        g_local_fire_marker_correction_count > 0 &&
        (return_address == g_projectile_return_primary ||
         return_address == g_projectile_return_secondary)) {
        const LocalFireMarkerCorrection* correction = nullptr;
        auto nearest_distance_squared =
            std::numeric_limits<float>::infinity();
        for (std::uint16_t index = 0;
             index < g_local_fire_marker_correction_count;
             ++index) {
            const auto& candidate = g_local_fire_marker_corrections[index];
            const auto distance_squared =
                length_squared(data->position - candidate.desired.position);
            if (std::isfinite(distance_squared) &&
                distance_squared < nearest_distance_squared) {
                correction = &candidate;
                nearest_distance_squared = distance_squared;
            }
        }
        if (correction == nullptr) {
            return g_original_projectile_new(data);
        }
        const auto source_basis =
            orthonormal_basis(correction->source);
        const auto desired_basis = orthonormal_basis(correction->desired);
        const auto delta_basis =
            multiply(desired_basis, transpose(source_basis));
        const auto corrected_forward =
            normalized(transform_vector(delta_basis, data->forward));
        const auto desired_up =
            normalized(transform_vector(delta_basis, data->up));
        const auto desired_velocity =
            transform_vector(delta_basis, data->velocity);
        const auto converged_center_forward = normalized(
            correction->reticle_position - data->position);
        const auto converged_center_left = normalized(
            cross(desired_basis.up, converged_center_forward));
        const auto converged_center_up = normalized(
            cross(converged_center_forward, converged_center_left));
        const Mat3 converged_center_basis{
            converged_center_forward,
            converged_center_left,
            converged_center_up};
        const auto convergence_delta = multiply(
            converged_center_basis,
            transpose(desired_basis));
        const auto final_forward = normalized(
            transform_vector(convergence_delta, corrected_forward));
        const auto final_up = normalized(
            transform_vector(convergence_delta, desired_up));
        const auto final_velocity =
            transform_vector(convergence_delta, desired_velocity);
        const auto final_delta =
            multiply(convergence_delta, delta_basis);
        if (valid_basis(source_basis) && valid_basis(desired_basis) &&
            valid_basis(delta_basis) &&
            valid_basis(converged_center_basis) &&
            valid_basis(convergence_delta) &&
            valid_basis(final_delta) && finite(data->position) &&
             finite(data->forward) && finite(data->up) &&
             finite(data->velocity) && finite(corrected_forward) &&
             finite(desired_up) && finite(desired_velocity) &&
             finite(converged_center_forward) &&
             finite(final_forward) && finite(final_up) &&
             finite(final_velocity) &&
             length_squared(final_forward) > 0.8f &&
             length_squared(converged_center_forward) > 0.8f &&
             length_squared(final_up) > 0.8f) {
            source_forward = data->forward;
            const auto source_up = data->up;
            const auto source_velocity = data->velocity;
            desired_forward = final_forward;
            center_forward = converged_center_forward;
            reticle_position = correction->reticle_position;
            applied_delta = final_delta;
            have_applied_delta = true;

            // Preserve the weapon's authored spread within the controller
            // cone, then recenter that cone from the projectile's real spawn
            // point onto the same ten-metre target used by the world reticle.
            // Previously this convergence was deferred to the first collision
            // sweep, so the visible projectile and its initial velocity could
            // travel several degrees away from the ring.
            data->forward = final_forward;
            data->up = final_up;
            // Some projectile types already carry inherited velocity in the
            // placement. Rotate it here, then also correct the spawned
            // object's tag-derived velocity after object_new returns.
            data->velocity = final_velocity;

            record_projectile_pose_diagnostics(*data);
            const auto total = g_override_count.fetch_add(
                                   1,
                                   std::memory_order_relaxed) +
                               1;
            if (total <= 16) {
                API::get()->log_info(
                    "HaloCEMotionControls: redirected final local projectile "
                    "origin (%.3f %.3f %.3f) "
                    "forward (%.3f %.3f %.3f)->(%.3f %.3f %.3f) "
                    "up (%.3f %.3f %.3f)->(%.3f %.3f %.3f) "
                    "velocity (%.3f %.3f %.3f)->(%.3f %.3f %.3f) "
                    "(total %u)",
                    data->position.x,
                    data->position.y,
                    data->position.z,
                    source_forward.x,
                    source_forward.y,
                    source_forward.z,
                    data->forward.x,
                    data->forward.y,
                    data->forward.z,
                    source_up.x,
                    source_up.y,
                    source_up.z,
                    data->up.x,
                    data->up.y,
                    data->up.z,
                    source_velocity.x,
                    source_velocity.y,
                    source_velocity.z,
                    data->velocity.x,
                    data->velocity.y,
                    data->velocity.z,
                    total);
            }
        }
    }

    const auto result = g_original_projectile_new(data);
    if (have_applied_delta && result != kInvalidBlamObjectIndex) {
        if (g_local_projectile_direction_override_count <
                g_local_projectile_direction_overrides.size() &&
            finite(desired_forward) &&
            length_squared(desired_forward) > 0.8f) {
            g_local_projectile_direction_overrides
                [g_local_projectile_direction_override_count++] = {
                    static_cast<std::uint32_t>(result),
                    normalized(source_forward),
                    normalized(center_forward),
                    reticle_position,
                    applied_delta,
                    false};
        }

        auto* const object =
            get_native_object(static_cast<std::int32_t>(result));
        if (object != nullptr && finite(object->velocity)) {
            const auto spawned_velocity = object->velocity;
            const auto speed_squared = length_squared(spawned_velocity);
            if (std::isfinite(speed_squared) && speed_squared > 0.0001f) {
                const auto spawned_direction =
                    normalized(spawned_velocity);
                const auto source_direction =
                    normalized(source_forward);
                const auto target_direction =
                    normalized(desired_forward);
                const auto source_alignment =
                    dot(spawned_direction, source_direction);
                const auto target_alignment =
                    dot(spawned_direction, target_direction);
                const auto corrected_velocity =
                    transform_vector(applied_delta, spawned_velocity);

                // If Halo already consumed the rewritten placement direction,
                // leave the result alone. Otherwise rotate the final
                // tag-derived velocity before the caller runs the
                // instantaneous first projectile tick.
                const auto needs_post_spawn_rotation =
                    source_alignment > target_alignment + 0.05f;
                if (needs_post_spawn_rotation &&
                    finite(corrected_velocity)) {
                    object->velocity = corrected_velocity;
                }
                const auto total =
                    g_override_count.load(std::memory_order_relaxed);
                if (total <= 32) {
                    API::get()->log_info(
                        "HaloCEMotionControls: post-spawn projectile 0x%08x "
                        "speed %.3f source-dot %.3f target-dot %.3f "
                        "velocity (%.3f %.3f %.3f)->(%.3f %.3f %.3f) "
                        "rotated=%d",
                        static_cast<std::uint32_t>(result),
                        std::sqrt(speed_squared),
                        source_alignment,
                        target_alignment,
                        spawned_velocity.x,
                        spawned_velocity.y,
                        spawned_velocity.z,
                        object->velocity.x,
                        object->velocity.y,
                        object->velocity.z,
                        needs_post_spawn_rotation ? 1 : 0);
                }
            }
        }
    }

    return result;
}

std::int64_t hook_projectile_collision_sweep(
    std::uint32_t projectile_object_index,
    std::int64_t flags,
    std::uint8_t phase,
    Vec3* start,
    Vec3* end,
    std::int32_t arg6,
    std::int32_t arg7,
    std::int32_t arg8,
    void* result) {
    const auto* return_address = _ReturnAddress();
    if (g_native_override_enabled.load(std::memory_order_relaxed) &&
        g_local_fire_depth > 0 &&
        (return_address == g_projectile_primary_sweep_return ||
         return_address == g_projectile_spread_sweep_return) &&
        start != nullptr && end != nullptr) {
        LocalProjectileDirectionOverride* direction_override = nullptr;
        for (std::uint16_t index = 0;
             index < g_local_projectile_direction_override_count;
             ++index) {
            auto& candidate =
                g_local_projectile_direction_overrides[index];
            if (candidate.object_index == projectile_object_index) {
                direction_override = &candidate;
                break;
            }
        }

        if (direction_override != nullptr && finite(*start) && finite(*end) &&
            finite(direction_override->source_direction) &&
            finite(direction_override->desired_direction) &&
            finite(direction_override->reticle_position) &&
            valid_basis(direction_override->delta_basis)) {
            const auto original_delta = *end - *start;
            const auto distance_squared = length_squared(original_delta);
            if (std::isfinite(distance_squared) &&
                distance_squared > 0.0001f) {
                const auto original_end = *end;
                const auto distance = std::sqrt(distance_squared);
                const auto original_direction =
                    original_delta / distance;
                const auto source_alignment = dot(
                    original_direction,
                    direction_override->source_direction);
                const auto desired_alignment = dot(
                    original_direction,
                    direction_override->desired_direction);

                // Only the first normal first-tick sweep is the center ray
                // represented by the visible ball. Halo can call the same
                // site again for post-impact/substep work, so consume it once
                // per projectile. The separate spread callsite keeps its
                // authored cone and receives only the source-to-hand delta.
                auto corrected_direction = original_direction;
                bool rotated = false;
                const auto primary_center_sweep =
                    return_address == g_projectile_primary_sweep_return &&
                    !direction_override->primary_sweep_consumed;
                if (primary_center_sweep) {
                    corrected_direction = normalized(
                        direction_override->reticle_position - *start);
                    direction_override->primary_sweep_consumed = true;
                    rotated =
                        finite(corrected_direction) &&
                        length_squared(corrected_direction) > 0.8f;
                } else if (source_alignment > 0.70f &&
                    source_alignment > desired_alignment + 0.02f) {
                    corrected_direction = normalized(transform_vector(
                        direction_override->delta_basis,
                        original_direction));
                    rotated =
                        finite(corrected_direction) &&
                        length_squared(corrected_direction) > 0.8f;
                }

                if (rotated) {
                    *end = *start + corrected_direction * distance;
                }
                if (primary_center_sweep && rotated) {
                    auto* const object = get_native_object(
                        static_cast<std::int32_t>(projectile_object_index));
                    if (object != nullptr && finite(object->velocity)) {
                        const auto speed_squared =
                            length_squared(object->velocity);
                        if (std::isfinite(speed_squared) &&
                            speed_squared > 0.0001f) {
                            object->velocity =
                                corrected_direction *
                                std::sqrt(speed_squared);
                        }
                        object->forward = corrected_direction;
                    }
                }

                const auto total =
                    g_override_count.load(std::memory_order_relaxed);
                if (total <= 64) {
                    API::get()->log_info(
                        "HaloCEMotionControls: checked authoritative %s "
                        "sweep projectile 0x%08x "
                        "start (%.3f %.3f %.3f) "
                        "end (%.3f %.3f %.3f)->(%.3f %.3f %.3f) "
                        "direction (%.3f %.3f %.3f)->"
                        "(%.3f %.3f %.3f) source-dot %.3f "
                        "target-dot %.3f distance %.3f rotated=%d",
                        return_address == g_projectile_spread_sweep_return
                            ? "spread"
                            : "first-tick",
                        projectile_object_index,
                        start->x,
                        start->y,
                        start->z,
                        original_end.x,
                        original_end.y,
                        original_end.z,
                        end->x,
                        end->y,
                        end->z,
                        original_direction.x,
                        original_direction.y,
                        original_direction.z,
                        corrected_direction.x,
                        corrected_direction.y,
                        corrected_direction.z,
                        source_alignment,
                        desired_alignment,
                        distance,
                        rotated ? 1 : 0);
                }
            }
        }
    }

    return g_original_projectile_collision_sweep(
        projectile_object_index,
        flags,
        phase,
        start,
        end,
        arg6,
        arg7,
        arg8,
        result);
}

std::int16_t hook_trigger_create_projectiles(
    std::uint32_t weapon_object_index,
    std::int16_t barrel_index,
    const void* network_projectile_records,
    bool from_network) {
    const auto is_local =
        !from_network &&
        static_cast<std::int32_t>(weapon_object_index) ==
        g_local_weapon_index.load(std::memory_order_acquire);

    const auto outermost_local_fire = is_local && g_local_fire_depth == 0;
    if (outermost_local_fire) {
        g_local_fire_marker_correction_count = 0;
        g_local_projectile_direction_override_count = 0;
        g_local_fire_tracking = capture_tracking_snapshot();
        if (g_local_fire_tracking.valid) {
            publish_tracking_snapshot(g_local_fire_tracking);
        } else {
            const std::scoped_lock lock{g_tracking_mutex};
            g_local_fire_tracking = g_tracking_snapshot;
        }
    }
    if (is_local) {
        ++g_local_fire_depth;
    }

    const auto result = g_original_trigger_create_projectiles(
        weapon_object_index,
        barrel_index,
        network_projectile_records,
        from_network);

    if (is_local) {
        --g_local_fire_depth;
    }
    if (outermost_local_fire) {
        g_local_fire_marker_correction_count = 0;
        g_local_projectile_direction_override_count = 0;
        g_local_fire_tracking = {};
    }

    return result;
}

void hook_first_person_weapon_build(
    std::int32_t local_player,
    std::int32_t weapon_slot,
    bool capture_render_palette) {
    g_original_first_person_weapon_build(
        local_player,
        weapon_slot,
        capture_render_palette);

    const auto entry_count =
        g_visual_build_entry_count.fetch_add(
            1,
            std::memory_order_relaxed) +
        1;
    if (entry_count <= 8) {
        API::get()->log_info(
            "HaloCEMotionControls: native FP builder detour call "
            "player=%d slot=%d capture=%d installed=%d",
            local_player,
            weapon_slot,
            capture_render_palette ? 1 : 0,
            g_native_visual_hook_installed.load(std::memory_order_relaxed)
                ? 1
                : 0);
    }

    if (!g_native_visual_hook_installed.load(std::memory_order_relaxed) ||
        g_simulation_module == nullptr || local_player < 0 ||
        local_player > 3 || weapon_slot < 0 || weapon_slot > 1) {
        return;
    }

    auto tracking = capture_tracking_snapshot();
    if (tracking.valid) {
        publish_tracking_snapshot(tracking);
    } else {
        const std::scoped_lock lock{g_tracking_mutex};
        tracking = g_tracking_snapshot;
    }
    if (!tracking.valid) {
        return;
    }

    constexpr std::size_t tls_index_rva = 0xD72730;
    constexpr std::size_t shared_capture_pointer_rva = 0x1831220;
    constexpr std::size_t tls_player_bank_offset = 0x4F8;
    constexpr std::size_t tls_capture_context_offset = 0x5B8;
    constexpr std::size_t player_stride = 0x52D8;
    constexpr std::size_t weapon_slot_stride = 0x2908;
    constexpr std::size_t slot_flags_offset = 0x38;
    constexpr std::size_t slot_object_index_offset = 0x44;
    constexpr std::size_t slot_animation_index_offset = 0x58;
    constexpr std::size_t slot_model_tag_offset = 0x194;
    constexpr std::size_t source_node_count_offset = 0x108C;
    constexpr std::size_t final_node_count_offset = 0x1090;
    constexpr std::size_t final_palette_offset = 0x1094;

    const auto tls_index = *reinterpret_cast<const DWORD*>(
        g_simulation_module + tls_index_rva);
    auto** const tls_slots =
        reinterpret_cast<void**>(__readgsqword(0x58));
    auto* const tls =
        tls_slots != nullptr
        ? static_cast<std::uint8_t*>(tls_slots[tls_index])
        : nullptr;
    if (tls == nullptr) {
        return;
    }

    auto* const player_bank = *reinterpret_cast<std::uint8_t**>(
        tls + tls_player_bank_offset);
    if (player_bank == nullptr) {
        return;
    }

    auto* const slot =
        player_bank +
        static_cast<std::size_t>(local_player) * player_stride +
        static_cast<std::size_t>(weapon_slot) * weapon_slot_stride;
    const auto flags =
        *reinterpret_cast<const std::uint32_t*>(slot + slot_flags_offset);
    const auto object_index =
        *reinterpret_cast<const std::int32_t*>(
            slot + slot_object_index_offset);
    const auto animation_index =
        *reinterpret_cast<const std::int32_t*>(
            slot + slot_animation_index_offset);
    const auto model_tag =
        *reinterpret_cast<const std::int32_t*>(slot + slot_model_tag_offset);
    const auto source_count =
        *reinterpret_cast<const std::int32_t*>(
            slot + source_node_count_offset);
    const auto final_count =
        *reinterpret_cast<const std::int32_t*>(
            slot + final_node_count_offset);

    const auto candidate_active =
        (flags & 0x0C) == 0x0C && object_index != -1 &&
        animation_index != -1 && model_tag != -1;
    const auto diagnostic = candidate_active
        ? g_visual_diagnostic_count.fetch_add(
              1,
              std::memory_order_relaxed) +
              1
        : 0;
    if (candidate_active && diagnostic <= 12) {
        API::get()->log_info(
            "HaloCEMotionControls: native FP palette candidate "
            "player=%d slot=%d tracking=%d flags=0x%08x object=%d "
            "animation=%d tag=%d source=%d final=%d",
            local_player,
            weapon_slot,
            tracking.valid ? 1 : 0,
            flags,
            object_index,
            animation_index,
            model_tag,
            source_count,
            final_count);
    }
    if ((flags & 0x0C) != 0x0C || object_index == -1 ||
        animation_index == -1 || model_tag == -1 ||
        source_count != static_cast<std::int32_t>(kFirstPersonNodeCount) ||
        final_count != static_cast<std::int32_t>(kFirstPersonNodeCount)) {
        return;
    }

    auto* const live_palette = reinterpret_cast<BlamMatrix4x3*>(
        slot + final_palette_offset);
    const auto previous_weapon_position = live_palette[8].position;
    if (!apply_controller_to_first_person_palette(
            live_palette,
            final_count,
            tracking)) {
        if (diagnostic <= 12) {
            const auto& root = live_palette[0];
            const auto& weapon = live_palette[8];
            API::get()->log_info(
                "HaloCEMotionControls: palette transform rejected "
                "root_scale=%.3f root_pos=(%.3f %.3f %.3f) "
                "weapon_scale=%.3f weapon_pos=(%.3f %.3f %.3f)",
                root.scale,
                root.position.x,
                root.position.y,
                root.position.z,
                weapon.scale,
                weapon.position.x,
                weapon.position.y,
                weapon.position.z);
        }
        return;
    }

    // The render reader can select a captured interpolation palette instead
    // of the live slot. Mirror the corrected palette into the exact capture
    // record the stock builder just wrote so both paths stay controller-owned.
    auto* const capture_context = *reinterpret_cast<std::uint8_t**>(
        tls + tls_capture_context_offset);
    auto* const shared_capture = *reinterpret_cast<std::uint8_t**>(
        g_simulation_module + shared_capture_pointer_rva);
    if (capture_context != nullptr && capture_context[2] != 0 &&
        capture_context[0] < 2 && shared_capture != nullptr) {
        constexpr std::size_t capture_context_stride = 0x30600;
        constexpr std::size_t capture_player_stride = 0x30D4;
        constexpr std::size_t capture_slot_stride = 0x1868;
        constexpr std::size_t capture_tag_offset = 0x24010;
        constexpr std::size_t capture_count_offset = 0x24014;
        constexpr std::size_t capture_palette_offset = 0x24018;

        auto* const record =
            shared_capture +
            static_cast<std::size_t>(capture_context[0]) *
                capture_context_stride +
            static_cast<std::size_t>(local_player) *
                capture_player_stride +
            static_cast<std::size_t>(weapon_slot) * capture_slot_stride;
        const auto captured_tag =
            *reinterpret_cast<const std::int32_t*>(
                record + capture_tag_offset);
        const auto captured_count =
            *reinterpret_cast<const std::int32_t*>(
                record + capture_count_offset);
        if (captured_tag == model_tag && captured_count == final_count) {
            auto* const captured_palette =
                reinterpret_cast<BlamMatrix4x3*>(
                    record + capture_palette_offset);
            apply_controller_to_first_person_palette(
                captured_palette,
                captured_count,
                tracking);
        }
    }

    record_visual_pose_diagnostics(tracking, live_palette);
    const auto total =
        g_visual_override_count.fetch_add(1, std::memory_order_relaxed) + 1;
    g_visual_weapon_attached.store(true, std::memory_order_release);
    if (total <= 8) {
        const auto& current = live_palette[8].position;
        API::get()->log_info(
            "HaloCEMotionControls: redirected native FP palette "
            "player=%d slot=%d node8 "
            "(%.3f %.3f %.3f)->(%.3f %.3f %.3f), total=%u",
            local_player,
            weapon_slot,
            previous_weapon_position.x,
            previous_weapon_position.y,
            previous_weapon_position.z,
            current.x,
            current.y,
            current.z,
            total);
    }

}

bool install_native_hooks() {
    g_native_hooks_installed.store(false, std::memory_order_release);
    g_native_projectile_hook_installed.store(false, std::memory_order_release);
    g_native_visual_hook_installed.store(false, std::memory_order_release);
    g_native_crosshair_hide_supported.store(
        false,
        std::memory_order_release);

    static const std::vector<int> trigger_create_pattern{
        0x44, 0x88, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18,
        0x66, 0x89, 0x54, 0x24, 0x10, 0x89, 0x4C, 0x24, 0x08, 0x53,
        0x57, 0x41, 0x55, 0x41, 0x57, 0xB8, 0x38, 0x26, 0x00,
        0x00, 0xE8, -1, -1, -1, -1, 0x48, 0x2B, 0xE0};
    static const std::vector<int> get_markers_pattern{
        0x66, 0x44, 0x89, 0x4C, 0x24, 0x20, 0x53, 0x55, 0x56, 0x57,
        0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x68, 0x45, 0x33,
        0xFF, 0x8B, 0xF1, 0x49, 0x8B, 0xD8, 0x44, 0x8B, 0xF2};
    static const std::vector<int> primary_marker_call_pattern{
        0xC6, 0x44, 0x24, 0x30, 0x00, 0x41, 0xB9, 0x40, 0x00, 0x00,
        0x00, 0xC6, 0x44, 0x24, 0x20, 0x00, 0x4C, 0x8D, 0x84, 0x24,
        0x80, 0x09, 0x00, 0x00, 0x8B, 0xD7, 0xE8, -1, -1, -1, -1,
        0x44, 0x0F, 0xB7, 0xC0};
    static const std::vector<int> projectile_new_pattern{
        0x48, 0x89, 0x4C, 0x24, 0x08, 0x41, 0x54, 0x41, 0x55, 0x48,
        0x81, 0xEC, 0x98, 0x04, 0x00, 0x00, 0xF6, 0x41, 0x18, 0x10,
        0x48, 0x8D, 0x41, 0x18, 0x4C, 0x8B, 0xE1, 0xC6, 0x44, 0x24,
        0x50, 0x00, 0x41, 0xBD, 0xFF, 0xFF, 0xFF, 0xFF};
    static const std::vector<int> projectile_collision_sweep_pattern{
        0x4C, 0x89, 0x4C, 0x24, 0x20, 0x44, 0x88, 0x44, 0x24, 0x18,
        0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41,
        0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC,
        0x24, 0x08, 0xE6, 0xFF, 0xFF};
    static const std::vector<int> chud_crosshair_layout_pattern{
        0x41, 0xBE, 0x28, 0x04, 0x00, 0x00,
        0x80, 0x3C, 0x2F, 0x00, 0x75, 0x05,
        0xE8, -1, -1, -1, -1,
        0x4A, 0x8B, 0x04, 0x37,
        0x45, 0x33, 0xC9,
        0x48, 0x69, 0xCB, 0x8C, 0x0C, 0x00, 0x00,
        0xBA, 0xFF, 0xFF, 0xFF, 0xFF,
        0x41, 0xB8, 0x10, 0x00, 0x00, 0x00,
        0xC5, 0xFA, 0x11, 0xB4, 0x01, 0x54, 0x03, 0x00, 0x00,
        0xB9, 0x03, 0x00, 0x00, 0x00};
    static const std::vector<int> chud_show_crosshair_pattern{
        0x40, 0x56, 0x48, 0x83, 0xEC, 0x30,
        0x8B, 0xF2,
        0x48, 0x0F, 0xBF, 0xC1,
        0x45, 0x0F, 0xB6, 0xC8,
        0x48, 0x8D, 0x15, -1, -1, -1, -1,
        0x8B, 0xCE,
        0x48, 0x8B, 0x14, 0xC2};
    static const std::vector<int> player_zoom_layout_pattern{
        0x40, 0x53,
        0x44, 0x8B, 0x05, -1, -1, -1, -1,
        0x44, 0x8B, 0xD2,
        0x65, 0x48, 0x8B, 0x0C, 0x25, 0x58, 0x00, 0x00, 0x00,
        0x33, 0xC0,
        0xBB, 0xB8, 0x00, 0x00, 0x00,
        0x41, 0xBB, 0xFF, 0xFF, 0xFF, 0xFF,
        0x4E, 0x8D, 0x0C, 0xC1,
        0x83, 0xF8, 0x03, 0x77, 0x23,
        0x48, 0x63, 0xC8,
        0x4C, 0x69, 0xC1, 0x98, 0x01, 0x00, 0x00,
        0x49, 0x8B, 0x09,
        0x48, 0x8B, 0x14, 0x0B,
        0x66, 0x46, 0x89, 0x9C, 0x02, 0xC6, 0x00, 0x00, 0x00};
    static const std::vector<int> first_person_weapon_build_pattern{
        0x48, 0x8B, 0xC4, 0x44, 0x88, 0x40, 0x18, 0x89, 0x50, 0x10,
        0x89, 0x48, 0x08, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
        0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xB0, 0x03, 0x00,
        0x00};

    auto& api = API::get();
    const auto module = GetModuleHandleW(L"HaloSimulation_tag_release.dll");
    if (module == nullptr) {
        return false;
    }
    g_simulation_module = reinterpret_cast<std::uint8_t*>(module);

    // These RVAs are from the exact HaloSimulation_tag_release.dll whose
    // native fire path was decompiled. Validate the full signatures before
    // installing either hook so an updated game fails open immediately
    // instead of scanning a very large simulation image on the game thread.
    constexpr std::size_t trigger_create_rva = 0x5CF460;
    constexpr std::size_t get_markers_rva = 0x5A43C0;
    constexpr std::size_t projectile_new_rva = 0x5A0FB0;
    constexpr std::size_t projectile_collision_sweep_rva = 0x2C69F0;
    constexpr std::size_t chud_crosshair_layout_rva = 0x1EA9DA;
    constexpr std::size_t chud_show_crosshair_rva = 0x1EA950;
    constexpr std::size_t players_unzoom_rva = 0x1E0A10;
    constexpr std::size_t primary_marker_call_rva = 0x5CF711;
    constexpr std::size_t projectile_primary_call_rva = 0x5D122E;
    constexpr std::size_t projectile_secondary_call_rva = 0x5D1249;
    constexpr std::size_t projectile_primary_sweep_call_rva = 0x6485C7;
    constexpr std::size_t projectile_spread_sweep_call_rva = 0x648E40;
    constexpr std::size_t first_person_weapon_build_rva = 0x46EC10;

    auto* trigger_create = validate_signature_at_rva(
        module,
        trigger_create_rva,
        trigger_create_pattern,
        "trigger_create_projectiles");
    auto* get_markers = validate_signature_at_rva(
        module,
        get_markers_rva,
        get_markers_pattern,
        "object_get_markers");
    auto* projectile_new = validate_signature_at_rva(
        module,
        projectile_new_rva,
        projectile_new_pattern,
        "projectile_new");
    auto* projectile_collision_sweep = validate_signature_at_rva(
        module,
        projectile_collision_sweep_rva,
        projectile_collision_sweep_pattern,
        "projectile_collision_sweep");
    auto* chud_crosshair_layout = validate_signature_at_rva(
        module,
        chud_crosshair_layout_rva,
        chud_crosshair_layout_pattern,
        "CHUD crosshair layout");
    auto* chud_show_crosshair = validate_signature_at_rva(
        module,
        chud_show_crosshair_rva,
        chud_show_crosshair_pattern,
        "chud_show_crosshair");
    auto* players_unzoom = validate_signature_at_rva(
        module,
        players_unzoom_rva,
        player_zoom_layout_pattern,
        "player zoom layout");
    auto* primary_call = validate_signature_at_rva(
        module,
        primary_marker_call_rva,
        primary_marker_call_pattern,
        "primary fire-marker call");
    auto* first_person_weapon_build = validate_signature_at_rva(
        module,
        first_person_weapon_build_rva,
        first_person_weapon_build_pattern,
        "first_person_weapon_build_node_matrices");
    if (trigger_create == nullptr || get_markers == nullptr ||
        projectile_new == nullptr || projectile_collision_sweep == nullptr ||
        primary_call == nullptr) {
        return false;
    }

    constexpr std::size_t call_instruction_offset = 26;
    constexpr std::size_t return_offset = 31;
    const auto relative = *reinterpret_cast<const std::int32_t*>(
        primary_call + call_instruction_offset + 1);
    auto* resolved_call_target =
        primary_call + return_offset + relative;
    if (resolved_call_target != get_markers) {
        api->log_error(
            "HaloCEMotionControls: primary marker call target does not match "
            "the marker signature; native muzzle override remains disabled");
        return false;
    }

    auto validate_projectile_call = [&](std::size_t rva) -> std::uint8_t* {
        auto* call = reinterpret_cast<std::uint8_t*>(module) + rva;
        if (*call != 0xE8) {
            return nullptr;
        }
        const auto displacement =
            *reinterpret_cast<const std::int32_t*>(call + 1);
        return call + 5 + displacement == projectile_new ? call : nullptr;
    };
    auto* projectile_primary_call =
        validate_projectile_call(projectile_primary_call_rva);
    auto* projectile_secondary_call =
        validate_projectile_call(projectile_secondary_call_rva);
    if (projectile_primary_call == nullptr ||
        projectile_secondary_call == nullptr) {
        api->log_error(
            "HaloCEMotionControls: final projectile callsites do not resolve "
            "to projectile_new; native fire override remains disabled");
        return false;
    }

    auto* projectile_primary_sweep_call =
        reinterpret_cast<std::uint8_t*>(module) +
        projectile_primary_sweep_call_rva;
    auto* projectile_spread_sweep_call =
        reinterpret_cast<std::uint8_t*>(module) +
        projectile_spread_sweep_call_rva;
    if (*projectile_primary_sweep_call != 0xE8 ||
        *projectile_spread_sweep_call != 0xE8) {
        api->log_error(
            "HaloCEMotionControls: one or more authoritative projectile "
            "sweep callsites are not direct calls; native fire override "
            "remains disabled");
        return false;
    }
    const auto projectile_sweep_displacement =
        *reinterpret_cast<const std::int32_t*>(
            projectile_primary_sweep_call + 1);
    const auto projectile_spread_sweep_displacement =
        *reinterpret_cast<const std::int32_t*>(
            projectile_spread_sweep_call + 1);
    if (projectile_primary_sweep_call + 5 +
            projectile_sweep_displacement != projectile_collision_sweep ||
        projectile_spread_sweep_call + 5 +
            projectile_spread_sweep_displacement !=
            projectile_collision_sweep) {
        api->log_error(
            "HaloCEMotionControls: authoritative projectile sweep callsites "
            "do not both resolve to the validated collision function; "
            "native fire override remains disabled");
        return false;
    }

    g_primary_marker_return = primary_call + return_offset;
    g_projectile_return_primary = projectile_primary_call + 5;
    g_projectile_return_secondary = projectile_secondary_call + 5;
    g_projectile_primary_sweep_return =
        projectile_primary_sweep_call + 5;
    g_projectile_spread_sweep_return =
        projectile_spread_sweep_call + 5;

    const auto* functions = api->param()->functions;
    g_trigger_hook_id = functions->register_inline_hook(
        trigger_create,
        reinterpret_cast<void*>(&hook_trigger_create_projectiles),
        reinterpret_cast<void**>(&g_original_trigger_create_projectiles));
    if (g_trigger_hook_id < 0 ||
        g_original_trigger_create_projectiles == nullptr) {
        api->log_error(
            "HaloCEMotionControls: failed to hook trigger_create_projectiles");
        return false;
    }

    g_marker_hook_id = functions->register_inline_hook(
        get_markers,
        reinterpret_cast<void*>(&hook_get_markers),
        reinterpret_cast<void**>(&g_original_get_markers));
    if (g_marker_hook_id < 0 || g_original_get_markers == nullptr) {
        functions->unregister_inline_hook(g_trigger_hook_id);
        g_trigger_hook_id = -1;
        g_original_trigger_create_projectiles = nullptr;
        api->log_error(
            "HaloCEMotionControls: failed to hook object_get_markers");
        return false;
    }

    g_projectile_hook_id = functions->register_inline_hook(
        projectile_new,
        reinterpret_cast<void*>(&hook_projectile_new),
        reinterpret_cast<void**>(&g_original_projectile_new));
    if (g_projectile_hook_id < 0 || g_original_projectile_new == nullptr) {
        functions->unregister_inline_hook(g_marker_hook_id);
        functions->unregister_inline_hook(g_trigger_hook_id);
        g_marker_hook_id = -1;
        g_trigger_hook_id = -1;
        g_original_get_markers = nullptr;
        g_original_trigger_create_projectiles = nullptr;
        g_projectile_hook_id = -1;
        g_original_projectile_new = nullptr;
        api->log_error(
            "HaloCEMotionControls: failed to hook projectile_new");
        return false;
    }

    g_projectile_sweep_hook_id = functions->register_inline_hook(
        projectile_collision_sweep,
        reinterpret_cast<void*>(&hook_projectile_collision_sweep),
        reinterpret_cast<void**>(&g_original_projectile_collision_sweep));
    if (g_projectile_sweep_hook_id < 0 ||
        g_original_projectile_collision_sweep == nullptr) {
        functions->unregister_inline_hook(g_projectile_hook_id);
        functions->unregister_inline_hook(g_marker_hook_id);
        functions->unregister_inline_hook(g_trigger_hook_id);
        g_projectile_sweep_hook_id = -1;
        g_projectile_hook_id = -1;
        g_marker_hook_id = -1;
        g_trigger_hook_id = -1;
        g_original_projectile_collision_sweep = nullptr;
        g_original_projectile_new = nullptr;
        g_original_get_markers = nullptr;
        g_original_trigger_create_projectiles = nullptr;
        api->log_error(
            "HaloCEMotionControls: failed to hook authoritative projectile "
            "collision sweep");
        return false;
    }

    api->log_info(
        "HaloCEMotionControls: native 6DOF fire hooks installed "
        "(trigger RVA 0x%llx, marker RVA 0x%llx, projectile RVA 0x%llx, "
        "first-tick sweep RVA 0x%llx)",
        static_cast<unsigned long long>(
            trigger_create - reinterpret_cast<std::uint8_t*>(module)),
        static_cast<unsigned long long>(
            get_markers - reinterpret_cast<std::uint8_t*>(module)),
        static_cast<unsigned long long>(
            projectile_new - reinterpret_cast<std::uint8_t*>(module)),
        static_cast<unsigned long long>(
            projectile_collision_sweep -
            reinterpret_cast<std::uint8_t*>(module)));
    g_native_projectile_hook_installed.store(
        true,
        std::memory_order_release);
    g_native_hooks_installed.store(true, std::memory_order_release);

    bool crosshair_layout_valid =
        chud_crosshair_layout != nullptr &&
        chud_show_crosshair != nullptr &&
        players_unzoom != nullptr;
    if (crosshair_layout_valid) {
        const auto displacement =
            *reinterpret_cast<const std::int32_t*>(players_unzoom + 5);
        const auto* resolved_tls_index =
            players_unzoom + 9 + displacement;
        const auto* expected_tls_index =
            reinterpret_cast<std::uint8_t*>(module) +
            kHaloTlsIndexRva;
        crosshair_layout_valid =
            resolved_tls_index == expected_tls_index;
    }

    if (crosshair_layout_valid) {
        g_chud_show_crosshair_hook_id =
            functions->register_inline_hook(
                chud_show_crosshair,
                reinterpret_cast<void*>(&hook_chud_show_crosshair),
                reinterpret_cast<void**>(&g_original_chud_show_crosshair));
        if (g_chud_show_crosshair_hook_id >= 0 &&
            g_original_chud_show_crosshair != nullptr) {
            g_native_crosshair_hide_supported.store(
                true,
                std::memory_order_release);
            api->log_info(
                "HaloCEMotionControls: scope-safe stock center crosshair "
                "writer hooked (CHUD RVA 0x%llx, writer RVA 0x%llx, "
                "zoom RVA 0x%llx)",
                static_cast<unsigned long long>(
                    chud_crosshair_layout -
                    reinterpret_cast<std::uint8_t*>(module)),
                static_cast<unsigned long long>(
                    chud_show_crosshair -
                    reinterpret_cast<std::uint8_t*>(module)),
                static_cast<unsigned long long>(
                    players_unzoom -
                    reinterpret_cast<std::uint8_t*>(module)));
        } else {
            g_chud_show_crosshair_hook_id = -1;
            g_original_chud_show_crosshair = nullptr;
            api->log_error(
                "HaloCEMotionControls: stock center crosshair writer hook "
                "failed; crosshair remains visible");
        }
    } else {
        api->log_error(
            "HaloCEMotionControls: stock center crosshair layout did not "
            "validate; crosshair remains visible");
    }

    // Visual correction is independently fail-open: a future first-person
    // builder change must not disable the already-validated muzzle redirect.
    if (first_person_weapon_build == nullptr) {
        api->log_error(
            "HaloCEMotionControls: native FP palette hook signature did not "
            "validate; visual 6DOF remains disabled");
        return true;
    }

    g_visual_hook_id = functions->register_inline_hook(
        first_person_weapon_build,
        reinterpret_cast<void*>(&hook_first_person_weapon_build),
        reinterpret_cast<void**>(&g_original_first_person_weapon_build));
    if (g_visual_hook_id < 0 ||
        g_original_first_person_weapon_build == nullptr) {
        g_visual_hook_id = -1;
        g_original_first_person_weapon_build = nullptr;
        api->log_error(
            "HaloCEMotionControls: failed to hook native first-person "
            "weapon palette builder; visual 6DOF remains disabled");
        return true;
    }

    g_native_visual_hook_installed.store(true, std::memory_order_release);
    api->log_info(
        "HaloCEMotionControls: native FP palette hook installed "
        "(builder RVA 0x%llx, 76-node arm/weapon assembly 5-75)",
        static_cast<unsigned long long>(
            first_person_weapon_build -
            reinterpret_cast<std::uint8_t*>(module)));
    return true;
}

API::UObject* get_component_by_class(
    API::UObject* actor,
    API::UClass* component_class) {
    if (actor == nullptr || component_class == nullptr) {
        return nullptr;
    }

    auto* function =
        actor->get_class()->find_function(L"GetComponentByClass");
    if (function == nullptr) {
        return nullptr;
    }

    GetComponentByClassParams params{};
    params.component_class = component_class;
    actor->process_event(function, &params);
    return params.return_value;
}

std::int32_t get_blam_object_index(
    API::UObject* weapon_actor,
    API::UClass* synchronization_component_class) {
    auto* component =
        get_component_by_class(weapon_actor, synchronization_component_class);
    if (component == nullptr) {
        return kInvalidBlamObjectIndex;
    }

    auto* index =
        component->get_property_data<std::int32_t>(L"BlamObjectIndex");
    return index != nullptr ? *index : kInvalidBlamObjectIndex;
}

} // namespace

extern "C" __declspec(dllexport) bool HaloCEVR_GetStatus(
    HaloCEVR_RuntimeStatus* status) {
    if (status == nullptr || status->size < sizeof(HaloCEVR_RuntimeStatus)) {
        return false;
    }

    std::uint32_t flags{};
    if (g_native_hooks_installed.load(std::memory_order_acquire)) {
        flags |= kStatusNativeHooksInstalled;
    }
    if (g_tracking_valid.load(std::memory_order_acquire)) {
        flags |= kStatusTrackingValid;
    }
    if (g_visual_weapon_attached.load(std::memory_order_acquire)) {
        flags |= kStatusVisualWeaponAttached;
    }
    if (g_native_visual_hook_installed.load(std::memory_order_acquire)) {
        flags |= kStatusNativeVisualHookInstalled;
    }
    if (g_native_projectile_hook_installed.load(std::memory_order_acquire)) {
        flags |= kStatusNativeProjectileHookInstalled;
    }
    if (g_left_tracking_valid.load(std::memory_order_acquire)) {
        flags |= kStatusLeftTrackingValid;
    }
    if (g_two_hand_ik_enabled.load(std::memory_order_acquire)) {
        flags |= kStatusTwoHandIkEnabled;
    }
    if (g_locomotion_bridge_observed.load(std::memory_order_acquire)) {
        flags |= kStatusLocomotionBridgeObserved;
    }
    if (g_two_hand_hold_latched.load(std::memory_order_acquire)) {
        flags |= kStatusTwoHandHoldActive;
    }

    const HaloCEVR_RuntimeStatus snapshot{
        .size = sizeof(HaloCEVR_RuntimeStatus),
        .version = 1,
        .flags = flags,
        .override_count =
            g_override_count.load(std::memory_order_acquire),
        .local_weapon_index =
            g_local_weapon_index.load(std::memory_order_acquire),
        .attached_component =
            g_attached_component.load(std::memory_order_acquire)};
    *status = snapshot;
    return true;
}

extern "C" __declspec(dllexport) bool HaloCEVR_GetPoseDiagnostics(
    HaloCEVR_PoseDiagnostics* diagnostics) {
    if (diagnostics == nullptr ||
        diagnostics->size < sizeof(HaloCEVR_PoseDiagnostics)) {
        return false;
    }

    const std::scoped_lock lock{g_pose_diagnostics_mutex};
    auto snapshot = g_pose_diagnostics;
    snapshot.size = sizeof(snapshot);
    snapshot.version = 1;
    snapshot.visual_override_count =
        g_visual_override_count.load(std::memory_order_acquire);
    snapshot.marker_override_count =
        g_marker_override_count.load(std::memory_order_acquire);
    snapshot.projectile_override_count =
        g_override_count.load(std::memory_order_acquire);
    *diagnostics = snapshot;
    return true;
}

class HaloCEMotionControls final : public Plugin {
public:
    void on_initialize() override {
        auto& api = API::get();
        if (api == nullptr) {
            return;
        }

        if (current_executable_name() != "halocampaignevolved.exe") {
            api->log_error(
                "HaloCEMotionControls: refusing to arm outside "
                "HaloCampaignEvolved.exe");
            return;
        }

        m_armed = true;
        std::array<wchar_t, 16> arm_ik_setting{};
        auto arm_ik_setting_length = GetEnvironmentVariableW(
            L"UEVR_HALO_ARM_IK",
            arm_ik_setting.data(),
            static_cast<DWORD>(arm_ik_setting.size()));
        if (arm_ik_setting_length == 0) {
            // Compatibility with packages made before the mode was renamed.
            arm_ik_setting_length = GetEnvironmentVariableW(
                L"UEVR_HALO_TWO_HAND_IK",
                arm_ik_setting.data(),
                static_cast<DWORD>(arm_ik_setting.size()));
        }
        if (arm_ik_setting_length > 0 &&
            arm_ik_setting_length < arm_ik_setting.size()) {
            const auto first = static_cast<wchar_t>(
                std::towlower(arm_ik_setting.front()));
            g_two_hand_ik_enabled.store(
                first != L'0' && first != L'f' &&
                first != L'n' && first != L'o',
                std::memory_order_release);
        }
        std::array<wchar_t, 16> two_hand_setting{};
        const auto two_hand_setting_length = GetEnvironmentVariableW(
            L"UEVR_HALO_TWO_HAND_HOLD",
            two_hand_setting.data(),
            static_cast<DWORD>(two_hand_setting.size()));
        if (two_hand_setting_length > 0 &&
            two_hand_setting_length < two_hand_setting.size()) {
            const auto first = static_cast<wchar_t>(
                std::towlower(two_hand_setting.front()));
            g_two_hand_hold_enabled.store(
                first != L'0' && first != L'f' &&
                first != L'n' && first != L'o',
                std::memory_order_release);
        }
        const auto marker = reticle_active_marker_path();
        if (!marker.empty()) {
            DeleteFileW(marker.c_str());
        }
        publish_gameplay_ready(false, true);
        publish_reticle_hide(false, true);
        g_replacement_reticle_active.store(false, std::memory_order_release);
        API::VR::clear_openxr_compositor_quad();
        API::UObjectHook::activate();
        // The native weapon/palette/projectile path below reads the raw
        // right-controller pose directly. UEVR's global RIGHT_CONTROLLER aim
        // mode is therefore both redundant and harmful here: it rotates
        // Halo's game camera toward the hand, which makes the HUD appear to
        // move opposite the weapon. Keep the game camera/controller aim
        // decoupled while retaining raw 6DOF tracking for this plugin.
        API::VR::set_aim_method(API::VR::AimMethod::GAME);
        API::VR::set_decoupled_pitch_enabled(false);
        API::VR::set_mod_value("VR_DecoupledPitchUIAdjust", false);
        api->log_info(
            "HaloCEMotionControls: armed; visual weapon and native Blam "
            "muzzle are owned by the right controller; arm mode=%s; "
            "game camera aim and pitch/UI compensation remain disabled",
            g_two_hand_ik_enabled.load(std::memory_order_relaxed)
                ? "anchored-arm IK (yaw-only shoulders, clavicle assist, "
                  "exact wrists)"
                : "floating hands (arms hidden)");
    }

    void on_pre_engine_tick(API::UGameEngine* engine, float delta) override {
        if (!m_armed) {
            return;
        }

        // UEVR loads the per-game config after external plugins initialize.
        // Keep the requested mode authoritative on every launch.
        API::VR::set_aim_method(API::VR::AimMethod::GAME);
        API::VR::set_decoupled_pitch_enabled(false);
        API::VR::set_mod_value("VR_DecoupledPitchUIAdjust", false);

        if (engine == nullptr) {
            return;
        }

        m_engine_uptime += std::max(delta, 0.0f);
        if (m_engine_uptime < 3.0f) {
            return;
        }

        update_tracking_snapshot();
        update_two_hand_hold(delta);
        update_world_reticle_transform();
        update_openxr_reticle_quad();

        if (!m_native_hook_attempted) {
            // Retry while the simulation DLL is still loading, but make a
            // signature failure terminal and fail open to stock firing.
            if (GetModuleHandleW(L"HaloSimulation_tag_release.dll") != nullptr) {
                m_native_hook_attempted = true;
                API::get()->log_info(
                    "HaloCEMotionControls: validating native fire signatures");
                m_native_hooks_installed = install_native_hooks();
            }
        }

        m_discovery_timer -= std::max(delta, 0.0f);
        if (m_discovery_timer > 0.0f) {
            return;
        }
        m_discovery_timer = 0.25f;

        // Marker files are a compatibility bridge between the native plugin
        // and the profile Lua script. Poll them at 4 Hz rather than opening a
        // filesystem handle on every engine tick.
        refresh_replacement_reticle_state();
        maintain_weapon_attachment();
        maintain_world_reticle_widget();
    }

    void on_device_reset() override {
        if (m_armed) {
            API::VR::clear_openxr_compositor_quad();
            m_openxr_reticle_published = false;
        }
    }

    void on_pre_viewport_client_draw(
        UEVR_UGameViewportClientHandle,
        UEVR_FViewportHandle,
        UEVR_FCanvasHandle) override {
        if (m_armed) {
            // This is deliberately later than the simulation tick so Halo
            // cannot immediately overwrite the visibility field. The helper
            // restores the exact stock values for zoom/scopes or if the
            // replacement reticle/tracking/projectile path goes unavailable.
            maintain_stock_crosshair();
        }
    }

    void on_xinput_get_state(
        std::uint32_t* retval,
        std::uint32_t user_index,
        XINPUT_STATE* state) override {
        if (!m_armed || retval == nullptr || state == nullptr ||
            !API::VR::is_runtime_ready() ||
            user_index != API::VR::get_lowest_xinput_index()) {
            return;
        }

        // UEVR maps the left grip to LEFT_SHOULDER, which Halo binds to
        // grenade throw. While the support hand is holding the two-handed
        // grip, or is primed inside the barrel zone, swallow that button so
        // acquiring or holding the weapon never throws a grenade.
        if (g_two_hand_hold_enabled.load(std::memory_order_relaxed) &&
            (g_two_hand_hold_latched.load(std::memory_order_relaxed) ||
             g_two_hand_zone_active.load(std::memory_order_relaxed))) {
            state->Gamepad.wButtons &=
                static_cast<WORD>(~XINPUT_GAMEPAD_LEFT_SHOULDER);
        }

        constexpr WORD dpad_mask =
            XINPUT_GAMEPAD_DPAD_UP |
            XINPUT_GAMEPAD_DPAD_DOWN |
            XINPUT_GAMEPAD_DPAD_LEFT |
            XINPUT_GAMEPAD_DPAD_RIGHT;
        if ((state->Gamepad.wButtons & dpad_mask) != 0) {
            // UEVR's d-pad shifting deliberately converts a stick gesture
            // into buttons and clears that stick before plugin callbacks run.
            // Do not re-inject analog movement on top of the shifted buttons.
            return;
        }

        const auto source = API::VR::get_left_joystick_source();
        const auto raw_axis = API::VR::get_joystick_axis(source);
        if (!std::isfinite(raw_axis.x) || !std::isfinite(raw_axis.y)) {
            return;
        }
        if (std::abs(raw_axis.x) <= 0.001f &&
            std::abs(raw_axis.y) <= 0.001f &&
            (state->Gamepad.sThumbLX != 0 ||
             state->Gamepad.sThumbLY != 0)) {
            // A real gamepad already supplied movement and the OpenXR action
            // is idle. Do not erase that physical input.
            return;
        }

        // UEVR's generic mapping can return a zero XInput stick for interaction
        // profiles it does not recognize even though the OpenXR action itself
        // is valid. Feed the left OpenXR stick into Halo's normal XInput path;
        // this preserves keyboard/gamepad movement and needs no Blam memory
        // write or build-specific movement offset.
        constexpr float deadzone = 0.12f;
        const auto apply_deadzone = [](float value) {
            const auto magnitude = std::abs(value);
            if (magnitude <= deadzone) {
                return 0.0f;
            }
            const auto remapped =
                (magnitude - deadzone) / (1.0f - deadzone);
            return std::copysign(std::min(remapped, 1.0f), value);
        };
        const auto x = apply_deadzone(
            std::clamp(raw_axis.x, -1.0f, 1.0f));
        const auto y = apply_deadzone(
            std::clamp(raw_axis.y, -1.0f, 1.0f));
        state->Gamepad.sThumbLX =
            static_cast<SHORT>(std::lround(x * 32767.0f));
        state->Gamepad.sThumbLY =
            static_cast<SHORT>(std::lround(y * 32767.0f));
        *retval = ERROR_SUCCESS;
        if (x == 0.0f && y == 0.0f) {
            return;
        }

        const auto first_locomotion_callback =
            !g_locomotion_bridge_observed.exchange(
                true,
            std::memory_order_release);

        if (first_locomotion_callback) {
            API::get()->log_info(
                "HaloCEMotionControls: left OpenXR thumbstick is bridged "
                "to Halo XInput locomotion");
        }
    }

private:
    void update_tracking_snapshot() {
        publish_tracking_snapshot(capture_tracking_snapshot());
    }

    static bool has_outer(
        API::UObject* object,
        API::UObject* expected_outer) {
        for (auto* outer = object != nullptr ? object->get_outer() : nullptr;
             outer != nullptr && outer != object;
             outer = outer->get_outer()) {
            if (outer == expected_outer) {
                return true;
            }
        }
        return false;
    }

    API::UObject* find_reticle_object(
        API::UClass* required_class,
        std::wstring_view required_text,
        API::UObject* required_outer = nullptr) {
        auto* const objects = API::FUObjectArray::get();
        if (objects == nullptr || required_class == nullptr) {
            return nullptr;
        }
        for (auto index = objects->get_object_count() - 1;
             index >= 0;
             --index) {
            auto* const object = objects->get_object(index);
            if (object == nullptr || !object->is_a(required_class) ||
                (required_outer != nullptr &&
                 !has_outer(object, required_outer))) {
                continue;
            }
            const auto name = object->get_full_name();
            if (name.find(required_text) != std::wstring::npos) {
                return object;
            }
        }
        return nullptr;
    }

    API::UObject* find_active_world_reticle_component(
        API::UClass* widget_component_class,
        API::UObject* owner) {
        auto* const objects = API::FUObjectArray::get();
        if (objects == nullptr || widget_component_class == nullptr ||
            owner == nullptr) {
            return nullptr;
        }

        // The Lua profile intentionally leaves pawn-owned components hidden
        // during teardown because destroying a component while Unreal is
        // unloading a world can race the engine. Those inert components may
        // remain in GUObjectArray and recycled slots are not ordered by
        // creation time. Selecting merely the first WidgetComponent_ can
        // therefore resurrect an old hidden component while Lua continues to
        // drive the current visible one. Require the component-created widget
        // and the visibility state that Lua establishes before it publishes
        // the ready marker. Once selected, the cached pointer is retained
        // while zoom/two-hand mode temporarily hides it.
        for (auto index = objects->get_object_count() - 1;
             index >= 0;
             --index) {
            auto* const object = objects->get_object(index);
            if (object == nullptr ||
                !object->is_a(widget_component_class) ||
                !has_outer(object, owner) ||
                object->get_full_name().find(L"WidgetComponent_") ==
                    std::wstring::npos ||
                object->get_bool_property(L"bHiddenInGame")) {
                continue;
            }
            auto* const widget =
                object->get_property_data<API::UObject*>(L"Widget");
            if (widget != nullptr && *widget != nullptr) {
                return object;
            }
        }
        return nullptr;
    }

    API::UObject* find_screen_reticle_image(API::UClass* required_class) {
        auto* const objects = API::FUObjectArray::get();
        if (objects == nullptr || required_class == nullptr) {
            return nullptr;
        }
        for (auto index = objects->get_object_count() - 1;
             index >= 0;
             --index) {
            auto* const object = objects->get_object(index);
            if (object == nullptr || !object->is_a(required_class)) {
                continue;
            }
            const auto name = object->get_full_name();
            if (name.find(L".WBP_HUD_Main_C_") != std::wstring::npos &&
                name.find(L".FirstPersonReticle.WidgetTree_") !=
                    std::wstring::npos &&
                name.ends_with(L".Reticle_Image")) {
                return object;
            }
        }
        return nullptr;
    }

    API::UObject* get_dynamic_material(API::UObject* image) {
        if (image == nullptr) {
            return nullptr;
        }
        auto* const function =
            image->get_class()->find_function(L"GetDynamicMaterial");
        if (function == nullptr) {
            return nullptr;
        }
        alignas(16) std::array<std::byte, 64> parameters{};
        image->process_event(function, parameters.data());
        return read_function_parameter<API::UObject*>(
            function,
            parameters,
            L"ReturnValue");
    }

    API::UObject* call_object_return(
        API::UObject* object,
        std::wstring_view function_name) {
        if (object == nullptr) {
            return nullptr;
        }
        auto* const function =
            object->get_class()->find_function(function_name);
        if (function == nullptr) {
            return nullptr;
        }
        alignas(16) std::array<std::byte, 64> parameters{};
        object->process_event(function, parameters.data());
        return read_function_parameter<API::UObject*>(
            function,
            parameters,
            L"ReturnValue");
    }

    bool bind_world_reticle_render_target() {
        auto* const render_target = call_object_return(
            m_world_reticle_component,
            L"GetRenderTarget");
        auto* const material_instance = call_object_return(
            m_world_reticle_component,
            L"GetMaterialInstance");
        if (render_target == nullptr || material_instance == nullptr) {
            return false;
        }

        bool exposure_compensated = false;
        auto* material = material_instance;
        for (int depth = 0; material != nullptr && depth < 8; ++depth) {
            if (material->get_full_name().find(
                    L"WidgetVRPassThrough") != std::wstring::npos) {
                exposure_compensated = true;
                break;
            }
            auto* const parent =
                material->get_property_data<API::UObject*>(L"Parent");
            material = parent == nullptr ? nullptr : *parent;
        }
        if (exposure_compensated !=
            m_world_reticle_exposure_compensated) {
            API::get()->log_info(
                exposure_compensated
                    ? "HaloCEMotionControls: world reticle uses the "
                      "exposure-compensated VR pass-through material"
                    : "HaloCEMotionControls: world reticle fell back to "
                      "the stock Widget3D pass material");
        }
        m_world_reticle_exposure_compensated = exposure_compensated;

        static const API::FName slate_ui{L"SlateUI"};
        auto* current_texture = static_cast<API::UObject*>(nullptr);
        auto* get_texture =
            material_instance->get_class()->find_function(
                L"K2_GetTextureParameterValue");
        if (get_texture != nullptr) {
            alignas(16) std::array<std::byte, 64> get_parameters{};
            if (write_function_parameter(
                    get_texture,
                    get_parameters,
                    L"ParameterName",
                    slate_ui)) {
                material_instance->process_event(
                    get_texture,
                    get_parameters.data());
                current_texture =
                    read_function_parameter<API::UObject*>(
                        get_texture,
                        get_parameters,
                        L"ReturnValue");
            }
        }

        const bool binding_changed =
            material_instance != m_world_reticle_pass_material ||
            render_target != m_world_reticle_render_target ||
            current_texture != render_target;
        if (!binding_changed) {
            return true;
        }

        auto* const set_texture =
            material_instance->get_class()->find_function(
                L"SetTextureParameterValue");
        alignas(16) std::array<std::byte, 64> set_parameters{};
        if (set_texture == nullptr ||
            !write_function_parameter(
                set_texture,
                set_parameters,
                L"ParameterName",
                slate_ui) ||
            !write_function_parameter(
                set_texture,
                set_parameters,
                L"Value",
                render_target)) {
            return false;
        }
        material_instance->process_event(
            set_texture,
            set_parameters.data());

        if (get_texture != nullptr) {
            alignas(16) std::array<std::byte, 64> verify_parameters{};
            if (!write_function_parameter(
                    get_texture,
                    verify_parameters,
                    L"ParameterName",
                    slate_ui)) {
                return false;
            }
            material_instance->process_event(
                get_texture,
                verify_parameters.data());
            if (read_function_parameter<API::UObject*>(
                    get_texture,
                    verify_parameters,
                    L"ReturnValue") != render_target) {
                return false;
            }
        }

        m_world_reticle_pass_material = material_instance;
        m_world_reticle_render_target = render_target;
        API::get()->log_info(
            "HaloCEMotionControls: rebound Widget3D SlateUI to the current "
            "reticle render target");
        return true;
    }

    void restore_world_reticle_depth_test() {
        if (m_world_reticle_base_material == nullptr ||
            !m_world_reticle_depth_override_active) {
            return;
        }
        m_world_reticle_base_material->set_bool_property(
            L"bDisableDepthTest",
            m_world_reticle_depth_test_was_disabled);
        m_world_reticle_base_material = nullptr;
        m_world_reticle_depth_override_active = false;
    }

    bool disable_world_reticle_depth_test() {
        if (m_world_reticle_pass_material == nullptr) {
            return false;
        }

        // UWidgetComponent's pass material is a dynamic instance whose Parent
        // chain ends at /Engine/EngineMaterials/Widget3DPassThrough.  A fixed
        // ten-metre reticle otherwise disappears whenever nearer world
        // geometry crosses the controller ray (most visibly while aiming at
        // the ground).  Disable depth on that pass only while our component is
        // active, and restore the material's original value on teardown.
        auto* material = m_world_reticle_pass_material;
        for (int depth = 0; material != nullptr && depth < 4; ++depth) {
            auto* const disable_depth_property =
                material->get_class()->find_property(L"bDisableDepthTest");
            if (disable_depth_property != nullptr) {
                if (material != m_world_reticle_base_material) {
                    restore_world_reticle_depth_test();
                    m_world_reticle_base_material = material;
                    m_world_reticle_depth_test_was_disabled =
                        material->get_bool_property(L"bDisableDepthTest");
                    m_world_reticle_depth_override_active = true;
                }
                if (!material->get_bool_property(L"bDisableDepthTest")) {
                    material->set_bool_property(L"bDisableDepthTest", true);
                }
                return material->get_bool_property(L"bDisableDepthTest");
            }

            auto* const parent =
                material->get_property_data<API::UObject*>(L"Parent");
            material = parent == nullptr ? nullptr : *parent;
        }
        return false;
    }

    bool set_widget(API::UObject* component, API::UObject* widget) {
        if (component == nullptr) {
            return false;
        }
        auto* const function =
            component->get_class()->find_function(L"SetWidget");
        std::array<std::byte, 64> parameters{};
        if (function == nullptr ||
            (!write_function_parameter(
                 function, parameters, L"Widget", widget) &&
             !write_function_parameter(
                 function, parameters, L"InWidget", widget))) {
            return false;
        }
        component->process_event(function, parameters.data());
        auto* const property =
            component->get_property_data<API::UObject*>(L"Widget");
        return property != nullptr && *property == widget;
    }

    bool set_uobject_rooted(API::UObject* object, bool rooted) {
        if (object == nullptr) {
            return false;
        }
        auto* const objects = API::FUObjectArray::get();
        if (objects == nullptr) {
            return false;
        }

        // UE5's EInternalObjectFlags layout is stable for these GC flags.
        // WidgetComponent's raw Widget property did not keep a dynamically
        // created UserWidget reachable in this game, so explicitly mirror
        // UObject::AddToRoot/RemoveFromRoot on its FUObjectItem.
        constexpr std::int32_t root_set = 0x40000000;
        constexpr std::int32_t invalid_gc_state =
            0x10000000 |  // Unreachable
            0x20000000 |  // PendingKill
            static_cast<std::int32_t>(0x80000000u);  // Garbage
        for (auto index = objects->get_object_count() - 1;
             index >= 0;
             --index) {
            auto* const item = objects->get_item(index);
            if (item == nullptr || item->object != object) {
                continue;
            }
            if (rooted) {
                if ((item->flags & invalid_gc_state) != 0) {
                    return false;
                }
                item->flags |= root_set;
            } else {
                item->flags &= ~root_set;
            }
            return true;
        }
        return false;
    }

    API::UObject* get_brush_resource_object(API::UObject* image) {
        if (image == nullptr) {
            return nullptr;
        }
        auto* const brush_property =
            image->get_class()->find_property(L"Brush");
        if (brush_property == nullptr || brush_property->get_offset() < 0) {
            return nullptr;
        }
        auto* const struct_property =
            reinterpret_cast<API::FStructProperty*>(brush_property);
        auto* const brush_struct = struct_property->get_struct();
        auto* const resource_property =
            brush_struct != nullptr
                ? brush_struct->find_property(L"ResourceObject")
                : nullptr;
        if (resource_property == nullptr ||
            resource_property->get_offset() < 0) {
            return nullptr;
        }
        auto* const address =
            reinterpret_cast<std::byte*>(image) +
            brush_property->get_offset() +
            resource_property->get_offset();
        return *reinterpret_cast<API::UObject**>(address);
    }

    bool set_reticle_image_material(
        API::UObject* image,
        API::UObject* material) {
        if (image == nullptr || material == nullptr) {
            return false;
        }
        auto* const function =
            image->get_class()->find_function(L"SetBrushResourceObject");
        std::array<std::byte, 64> parameters{};
        if (function == nullptr ||
            !write_function_parameter(
                function, parameters, L"ResourceObject", material)) {
            return false;
        }
        image->process_event(function, parameters.data());
        return get_brush_resource_object(image) == material;
    }

    void set_reticle_image_opacity(API::UObject* image, float opacity) {
        if (image == nullptr) {
            return;
        }
        auto* const function =
            image->get_class()->find_function(L"SetRenderOpacity");
        std::array<std::byte, 64> parameters{};
        if (function == nullptr ||
            !write_function_parameter(
                function, parameters, L"InOpacity", opacity)) {
            return;
        }
        image->process_event(function, parameters.data());
    }

    bool set_world_reticle_draw_size(std::int32_t width, std::int32_t height) {
        if (m_world_reticle_component == nullptr) {
            return false;
        }
        auto* const function =
            m_world_reticle_component->get_class()->find_function(
                L"SetDrawSize");
        std::array<std::byte, 64> parameters{};
        // UWidgetComponent stores DrawSize as FIntPoint, but its reflected
        // Blueprint setter deliberately takes FVector2D. UE5's LWC
        // FVector2D is two doubles; writing an 8-byte FIntPoint into this
        // 16-byte parameter produced (0,0), left RenderTarget null, and kept
        // bRedrawRequested latched so the widget retried a failed draw every
        // tick.
        const UnrealVector2D size{
            static_cast<double>(width),
            static_cast<double>(height)};
        if (function == nullptr ||
            !write_function_parameter(
                function, parameters, L"Size", size)) {
            return false;
        }
        m_world_reticle_component->process_event(
            function,
            parameters.data());
        auto* const actual =
            m_world_reticle_component
                ->get_property_data<UnrealIntPoint>(L"DrawSize");
        return actual != nullptr &&
            actual->x == width &&
            actual->y == height;
    }

    bool set_world_reticle_scale(double scale) {
        if (m_world_reticle_component == nullptr) {
            return false;
        }
        auto* const function =
            m_world_reticle_component->get_class()->find_function(
                L"SetRelativeScale3D");
        alignas(16) std::array<std::byte, 64> parameters{};
        const UnrealVector new_scale{scale, scale, scale};
        if (function == nullptr ||
            !write_function_parameter(
                function,
                parameters,
                L"NewScale3D",
                new_scale)) {
            return false;
        }
        m_world_reticle_component->process_event(
            function,
            parameters.data());
        auto* const actual =
            m_world_reticle_component
                ->get_property_data<UnrealVector>(L"RelativeScale3D");
        return actual != nullptr &&
            std::abs(actual->x - scale) < 0.001 &&
            std::abs(actual->y - scale) < 0.001 &&
            std::abs(actual->z - scale) < 0.001;
    }

    bool set_world_reticle_tint(float gain) {
        if (m_world_reticle_component == nullptr) {
            return false;
        }
        auto* const function =
            m_world_reticle_component->get_class()->find_function(
                L"SetTintColorAndOpacity");
        alignas(16) std::array<std::byte, 64> parameters{};
        const UnrealLinearColor tint{gain, gain, gain, 1.0f};
        if (function == nullptr ||
            !write_function_parameter(
                function,
                parameters,
                L"NewTintColorAndOpacity",
                tint)) {
            return false;
        }
        m_world_reticle_component->process_event(
            function,
            parameters.data());
        auto* const actual =
            m_world_reticle_component
                ->get_property_data<UnrealLinearColor>(
                    L"TintColorAndOpacity");
        return actual != nullptr &&
            std::abs(actual->r - gain) < 0.001f &&
            std::abs(actual->g - gain) < 0.001f &&
            std::abs(actual->b - gain) < 0.001f &&
            std::abs(actual->a - 1.0f) < 0.001f;
    }

    bool prune_world_reticle_overlay() {
        if (m_world_reticle_widget == nullptr ||
            m_world_reticle_image == nullptr) {
            return false;
        }
        auto& api = API::get();
        auto* const overlay_class =
            api->find_uobject<API::UClass>(
                L"Class /Script/UMG.Overlay");
        auto* const overlay = find_reticle_object(
            overlay_class,
            L".ReticleOverlay",
            m_world_reticle_widget);
        if (overlay == nullptr) {
            return false;
        }

        auto* const count_function =
            overlay->get_class()->find_function(L"GetChildrenCount");
        auto* const child_function =
            overlay->get_class()->find_function(L"GetChildAt");
        auto* const remove_function =
            overlay->get_class()->find_function(L"RemoveChildAt");
        if (count_function == nullptr || child_function == nullptr ||
            remove_function == nullptr) {
            return false;
        }

        std::array<std::byte, 64> count_parameters{};
        overlay->process_event(count_function, count_parameters.data());
        const auto count = read_function_parameter<std::int32_t>(
            count_function,
            count_parameters,
            L"ReturnValue");
        for (auto index = count - 1; index >= 0; --index) {
            std::array<std::byte, 64> child_parameters{};
            if (!write_function_parameter(
                    child_function,
                    child_parameters,
                    L"Index",
                    index)) {
                return false;
            }
            overlay->process_event(
                child_function,
                child_parameters.data());
            auto* const child =
                read_function_parameter<API::UObject*>(
                    child_function,
                    child_parameters,
                    L"ReturnValue");
            if (child == m_world_reticle_image) {
                continue;
            }
            std::array<std::byte, 64> remove_parameters{};
            if (!write_function_parameter(
                    remove_function,
                    remove_parameters,
                    L"Index",
                    index)) {
                return false;
            }
            overlay->process_event(
                remove_function,
                remove_parameters.data());
        }

        std::array<std::byte, 64> final_count_parameters{};
        overlay->process_event(
            count_function,
            final_count_parameters.data());
        return read_function_parameter<std::int32_t>(
                   count_function,
                   final_count_parameters,
                   L"ReturnValue") == 1;
    }

    API::UObject* create_reticle_widget(API::UObject* player_controller) {
        auto& api = API::get();
        auto* const library_class =
            api->find_uobject<API::UClass>(
                L"Class /Script/UMG.WidgetBlueprintLibrary");
        auto* const widget_class =
            api->find_uobject<API::UClass>(
                L"WidgetBlueprintGeneratedClass "
                L"/Game/UI/Hud/Reticle/WBP_FirstPersonReticle."
                L"WBP_FirstPersonReticle_C");
        if (library_class == nullptr || widget_class == nullptr ||
            player_controller == nullptr) {
            return nullptr;
        }
        auto* const library = library_class->get_class_default_object();
        auto* const function = library_class->find_function(L"Create");
        std::array<std::byte, 128> parameters{};
        if (library == nullptr || function == nullptr ||
            !write_function_parameter(
                function,
                parameters,
                L"WorldContextObject",
                player_controller) ||
            !write_function_parameter(
                function, parameters, L"WidgetType", widget_class) ||
            !write_function_parameter(
                function,
                parameters,
                L"OwningPlayer",
                player_controller)) {
            return nullptr;
        }
        library->process_event(function, parameters.data());
        return read_function_parameter<API::UObject*>(
            function,
            parameters,
            L"ReturnValue");
    }

    void reset_world_reticle_cache(API::UObject* owner) {
        clear_openxr_reticle_quad(true);
        restore_world_reticle_depth_test();
        if (m_world_reticle_widget_rooted) {
            set_uobject_rooted(m_world_reticle_widget, false);
        }
        m_reticle_owner = owner;
        m_world_reticle_component = nullptr;
        m_world_reticle_widget = nullptr;
        m_world_reticle_widget_rooted = false;
        m_world_reticle_image = nullptr;
        m_world_reticle_pruned = false;
        m_world_reticle_pass_material = nullptr;
        m_world_reticle_render_target = nullptr;
        m_world_reticle_exposure_compensated = false;
        m_screen_reticle_image = nullptr;
        m_screen_reticle_suppressed = false;
        m_shared_reticle_material = nullptr;
    }

    bool set_world_reticle_main_pass(bool enabled) {
        if (m_world_reticle_component == nullptr) {
            return false;
        }

        auto* const function =
            m_world_reticle_component->get_class()->find_function(
                L"SetRenderInMainPass");
        std::array<std::byte, 32> parameters{};
        if (function == nullptr ||
            (!write_function_parameter(
                 function,
                 parameters,
                 L"bValue",
                 enabled) &&
             !write_function_parameter(
                 function,
                 parameters,
                 L"bRenderInMainPass",
                 enabled))) {
            return false;
        }

        m_world_reticle_component->process_event(function, parameters.data());
        return true;
    }

    void clear_openxr_reticle_quad(bool restore_world_geometry) {
        if (m_openxr_reticle_published) {
            API::VR::clear_openxr_compositor_quad();
            m_openxr_reticle_published = false;
        }

        if (restore_world_geometry && m_world_reticle_main_pass_disabled) {
            set_world_reticle_main_pass(true);
            m_world_reticle_main_pass_disabled = false;
        }
    }

    void log_reticle_state(int state, const char* message) {
        if (state == m_reticle_state) {
            return;
        }
        m_reticle_state = state;
        if (state == 0) {
            API::get()->log_info("%s", message);
        } else {
            API::get()->log_warn("%s", message);
        }
    }

    void maintain_world_reticle_widget() {
        auto& api = API::get();
        auto* const owner = api->get_local_pawn(0);
        auto* const player_controller = api->get_player_controller(0);
        if (owner == nullptr || player_controller == nullptr) {
            reset_world_reticle_cache(nullptr);
            return;
        }
        if (owner != m_reticle_owner) {
            reset_world_reticle_cache(owner);
        }

        auto* const widget_component_class =
            api->find_uobject<API::UClass>(
                L"Class /Script/UMG.WidgetComponent");
        if (m_world_reticle_component == nullptr) {
            m_world_reticle_component =
                find_active_world_reticle_component(
                    widget_component_class,
                    owner);
        }
        if (m_world_reticle_component == nullptr) {
            log_reticle_state(
                1,
                "HaloCEMotionControls: waiting for Lua WidgetComponent");
            return;
        }

        auto* const widget_property =
            m_world_reticle_component
                ->get_property_data<API::UObject*>(L"Widget");
        auto* hosted_widget =
            widget_property != nullptr ? *widget_property : nullptr;
        if (hosted_widget != nullptr &&
            hosted_widget != m_world_reticle_widget &&
            !set_uobject_rooted(hosted_widget, true)) {
            // A non-null Widget pointer can remain after the object has entered
            // GC. Clear it through the component API so a fresh instance can be
            // created instead of dereferencing the stale UObject.
            set_widget(m_world_reticle_component, nullptr);
            hosted_widget = nullptr;
        }
        if (hosted_widget == nullptr) {
            hosted_widget = create_reticle_widget(player_controller);
            if (hosted_widget == nullptr) {
                log_reticle_state(
                    2,
                    "HaloCEMotionControls: WidgetBlueprintLibrary.Create "
                    "returned null");
                return;
            }
            if (!set_uobject_rooted(hosted_widget, true)) {
                log_reticle_state(
                    7,
                    "HaloCEMotionControls: failed to root created world "
                    "reticle widget");
                return;
            }
            if (!set_widget(m_world_reticle_component, hosted_widget)) {
                set_uobject_rooted(hosted_widget, false);
                log_reticle_state(
                    3,
                    "HaloCEMotionControls: WidgetComponent.SetWidget did "
                    "not retain the created reticle");
                return;
            }
            api->log_info(
                "HaloCEMotionControls: native world reticle widget retained");
        }
        if (hosted_widget != m_world_reticle_widget) {
            if (m_world_reticle_widget_rooted) {
                set_uobject_rooted(m_world_reticle_widget, false);
            }
            m_world_reticle_widget = hosted_widget;
            m_world_reticle_widget_rooted = true;
            m_world_reticle_image = nullptr;
            m_world_reticle_pruned = false;
        }

        auto* const lazy_image_class =
            api->find_uobject<API::UClass>(
                L"Class /Script/HaloUI.HaloUILazyImage");
        if (m_screen_reticle_image == nullptr) {
            m_screen_reticle_image =
                find_screen_reticle_image(lazy_image_class);
        }
        if (m_world_reticle_image == nullptr) {
            m_world_reticle_image = find_reticle_object(
                lazy_image_class,
                L".Reticle_Image",
                m_world_reticle_widget);
        }
        if (m_screen_reticle_image == nullptr ||
            m_world_reticle_image == nullptr) {
            log_reticle_state(
                m_screen_reticle_image == nullptr ? 4 : 5,
                m_screen_reticle_image == nullptr
                    ? "HaloCEMotionControls: waiting for live HUD "
                      "Reticle_Image"
                    : "HaloCEMotionControls: waiting for world "
                      "Reticle_Image");
            return;
        }
        if (!m_world_reticle_pruned) {
            if (!prune_world_reticle_overlay()) {
                log_reticle_state(
                    8,
                    "HaloCEMotionControls: failed to isolate the authored "
                    "250x250 Reticle_Image");
                return;
            }
            m_world_reticle_pruned = true;
            api->log_info(
                "HaloCEMotionControls: world reticle reduced to the authored "
                "250x250 image");
        }
        auto* const draw_size =
            m_world_reticle_component
                ->get_property_data<UnrealIntPoint>(L"DrawSize");
        if (draw_size == nullptr ||
            draw_size->x != 250 || draw_size->y != 250) {
            if (!set_world_reticle_draw_size(250, 250)) {
                log_reticle_state(
                    10,
                    "HaloCEMotionControls: failed to enforce the authored "
                    "250x250 reticle target");
                return;
            }
        }
        auto* const relative_scale =
            m_world_reticle_component
                ->get_property_data<UnrealVector>(L"RelativeScale3D");
        if (relative_scale == nullptr ||
            std::abs(relative_scale->x - kWorldReticleScale) > 0.001 ||
            std::abs(relative_scale->y - kWorldReticleScale) > 0.001 ||
            std::abs(relative_scale->z - kWorldReticleScale) > 0.001) {
            if (!set_world_reticle_scale(kWorldReticleScale)) {
                log_reticle_state(
                    12,
                    "HaloCEMotionControls: failed to enforce world "
                    "reticle scale");
                return;
            }
        }
        // SetDrawSize can replace both the render target and Widget3D MID.
        // Bind first so the parent-chain check below knows whether the cooked
        // EyeAdaptationInverse pass is active. That pass preserves authored
        // cyan/red at unit tint; the stock fallback retains the old gain.
        if (!bind_world_reticle_render_target()) {
            log_reticle_state(
                11,
                "HaloCEMotionControls: failed to bind Widget3D SlateUI "
                "render target");
            return;
        }
        const float expected_gain =
            m_world_reticle_exposure_compensated
                ? kWorldReticleExposureCompensatedGain
                : kWorldReticleFallbackEmissiveGain;
        auto* const tint =
            m_world_reticle_component
                ->get_property_data<UnrealLinearColor>(
                    L"TintColorAndOpacity");
        if (tint == nullptr ||
            std::abs(tint->r - expected_gain) > 0.001f ||
            std::abs(tint->g - expected_gain) > 0.001f ||
            std::abs(tint->b - expected_gain) > 0.001f ||
            std::abs(tint->a - 1.0f) > 0.001f) {
            if (!set_world_reticle_tint(expected_gain)) {
                log_reticle_state(
                    13,
                    "HaloCEMotionControls: failed to enforce world "
                    "reticle emissive gain");
                return;
            }
        }
        // SetDrawSize may allocate a replacement render target after the
        // Widget3D MID has already been created. UWidgetComponent normally
        // refreshes the SlateUI texture parameter as part of its internal
        // material update, but Halo's dynamically added component can retain
        // the old (or null) texture. The render target then contains the
        // correct cyan ring while the world quad samples black. Verify and
        // repair the binding against the component's current objects.
        if (!disable_world_reticle_depth_test()) {
            log_reticle_state(
                14,
                "HaloCEMotionControls: failed to disable world reticle "
                "depth testing");
            return;
        }

        // Copy the live weapon-specific reticle MID (for example the sniper
        // ring) into the component-owned image. This changes the pixels drawn
        // into the WidgetComponent render target; it is separate from the
        // Widget3D SlateUI pass material rebound above.
        auto* const source_material =
            get_dynamic_material(m_screen_reticle_image);
        if (source_material == nullptr) {
            log_reticle_state(
                6,
                "HaloCEMotionControls: live HUD reticle MID unavailable");
            return;
        }
        const bool material_changed =
            m_shared_reticle_material != source_material ||
            get_brush_resource_object(m_world_reticle_image) !=
                source_material;
        if (material_changed &&
            !set_reticle_image_material(
                m_world_reticle_image,
                source_material)) {
            log_reticle_state(
                9,
                "HaloCEMotionControls: world Reticle_Image brush did not "
                "retain the live HUD MID");
            return;
        }
        if (!m_screen_reticle_suppressed) {
            set_reticle_image_opacity(m_screen_reticle_image, 0.0f);
            m_screen_reticle_suppressed = true;
        }
        m_shared_reticle_material = source_material;
        log_reticle_state(
            0,
            "HaloCEMotionControls: weapon-specific world reticle retained");

        // Redraw only after a real brush transition. Requesting a redraw on
        // every maintenance pass causes runaway Slate/D3D resource churn.
        if (material_changed) {
            auto* const redraw =
                m_world_reticle_component->get_class()->find_function(
                    L"RequestRedraw");
            if (redraw == nullptr) {
                return;
            }
            std::array<std::byte, 8> parameters{};
            m_world_reticle_component->process_event(
                redraw,
                parameters.data());
        }
    }

    void update_world_reticle_transform() {
        if (m_world_reticle_component == nullptr ||
            m_world_reticle_widget == nullptr) {
            return;
        }

        TrackingSnapshot tracking{};
        {
            const std::scoped_lock lock{g_tracking_mutex};
            tracking = g_tracking_snapshot;
        }
        if (!tracking.valid) {
            return;
        }

        auto* const player_controller =
            API::get()->get_player_controller(0);
        if (player_controller == nullptr) {
            return;
        }
        auto* const function =
            player_controller->get_class()->find_function(
                L"GetPlayerViewPoint");
        std::array<std::byte, 128> viewpoint_parameters{};
        if (function == nullptr) {
            return;
        }
        player_controller->process_event(
            function,
            viewpoint_parameters.data());
        const auto view_location =
            read_function_parameter<UnrealVector>(
                function,
                viewpoint_parameters,
                L"Location");
        const auto view_rotation =
            read_function_parameter<UnrealRotator>(
                function,
                viewpoint_parameters,
                L"Rotation");

        const auto hmd_rotation = normalized(tracking.hmd_rotation);
        const auto aim_rotation = normalized(tracking.right_aim_rotation);
        const auto aim_relative =
            normalized(conjugate(hmd_rotation) * aim_rotation);
        const auto controller_basis = effective_controller_basis(
            tracking,
            conjugate(hmd_rotation),
            aim_relative);
        if (!valid_basis(controller_basis)) {
            return;
        }

        // Blam basis is +X forward, +Y left, +Z up. Unreal camera-local is
        // +X forward, +Y right, +Z up.
        const Vec3 local_direction{
            controller_basis.forward.x,
            -controller_basis.forward.y,
            controller_basis.forward.z};

        constexpr double radians_per_degree =
            3.14159265358979323846 / 180.0;
        const auto pitch = view_rotation.pitch * radians_per_degree;
        const auto yaw = view_rotation.yaw * radians_per_degree;
        const auto roll = view_rotation.roll * radians_per_degree;
        const auto cp = std::cos(pitch);
        const auto sp = std::sin(pitch);
        const auto cy = std::cos(yaw);
        const auto sy = std::sin(yaw);
        const auto cr = std::cos(roll);
        const auto sr = std::sin(roll);
        const UnrealVector camera_forward{cp * cy, cp * sy, sp};
        const UnrealVector camera_right{
            sr * sp * cy - cr * sy,
            sr * sp * sy + cr * cy,
            -sr * cp};
        const UnrealVector camera_up{
            -(cr * sp * cy + sr * sy),
            cy * sr - cr * sp * sy,
            cr * cp};
        const UnrealVector direction{
            camera_forward.x * local_direction.x +
                camera_right.x * local_direction.y +
                camera_up.x * local_direction.z,
            camera_forward.y * local_direction.x +
                camera_right.y * local_direction.y +
                camera_up.y * local_direction.z,
            camera_forward.z * local_direction.x +
                camera_right.z * local_direction.y +
                camera_up.z * local_direction.z};
        constexpr double reticle_distance_centimeters = 1000.0;
        const UnrealVector target{
            view_location.x + direction.x * reticle_distance_centimeters,
            view_location.y + direction.y * reticle_distance_centimeters,
            view_location.z + direction.z * reticle_distance_centimeters};
        constexpr double degrees_per_radian =
            180.0 / 3.14159265358979323846;
        // The one-sided Widget3D pass uses local +X as the authored visible
        // face. Point +X from the target back toward the camera. Creating the
        // component as one-sided is essential: changing bIsTwoSided after its
        // MID exists leaves the old two-sided parent and its black BackColor
        // branch in place.
        const UnrealRotator face_camera{
            std::atan2(
                -direction.z,
                std::hypot(direction.x, direction.y)) *
                degrees_per_radian,
            std::atan2(-direction.y, -direction.x) *
                degrees_per_radian,
            0.0};

        auto* const set_transform =
            m_world_reticle_component->get_class()->find_function(
                L"K2_SetWorldLocationAndRotation");
        std::array<std::byte, 512> transform_parameters{};
        if (set_transform == nullptr ||
            !write_function_parameter(
                set_transform,
                transform_parameters,
                L"NewLocation",
                target) ||
            !write_function_parameter(
                set_transform,
                transform_parameters,
                L"NewRotation",
                face_camera)) {
            return;
        }
        m_world_reticle_component->process_event(
            set_transform,
            transform_parameters.data());
    }

    void update_openxr_reticle_quad() {
        const bool visible =
            API::VR::is_openxr() &&
            g_replacement_reticle_active.load(std::memory_order_acquire) &&
            !g_two_hand_hold_latched.load(std::memory_order_acquire) &&
            !g_local_zoomed.load(std::memory_order_acquire) &&
            m_world_reticle_component != nullptr &&
            m_world_reticle_render_target != nullptr;
        if (!visible) {
            clear_openxr_reticle_quad(true);
            return;
        }

        TrackingSnapshot tracking{};
        {
            const std::scoped_lock lock{g_tracking_mutex};
            tracking = g_tracking_snapshot;
        }
        if (!tracking.valid) {
            clear_openxr_reticle_quad(true);
            return;
        }

        const auto hmd_rotation = normalized(tracking.hmd_rotation);
        const auto aim_rotation = normalized(tracking.right_aim_rotation);
        const auto inverse_hmd_rotation = conjugate(hmd_rotation);
        const auto aim_relative = normalized(inverse_hmd_rotation * aim_rotation);
        const auto controller_basis = effective_controller_basis(
            tracking,
            inverse_hmd_rotation,
            aim_relative);
        if (!valid_basis(controller_basis)) {
            clear_openxr_reticle_quad(true);
            return;
        }

        // This is the inverse of openxr_to_blam(). Applying it to the shared
        // controller basis gives the exact HMD-local ray used by the visual
        // weapon, native marker hooks, projectile hooks, and world widget.
        const Vec3 local_direction_xr{
            -controller_basis.forward.y,
            controller_basis.forward.z,
            -controller_basis.forward.x};
        const auto stage_direction = normalized(
            rotate(hmd_rotation, local_direction_xr));
        if (!finite(stage_direction) ||
            length_squared(stage_direction) < 0.8f) {
            clear_openxr_reticle_quad(true);
            return;
        }

        constexpr float reticle_distance_meters = 10.0f;
        constexpr float reticle_texture_extent_meters = 2.5f;
        const auto target =
            tracking.hmd_position + stage_direction * reticle_distance_meters;
        // An OpenXR quad's authored front face is +Z. Rotate that normal back
        // toward the HMD so the one-sided alpha surface is visible. The ring
        // is rotationally symmetric, so no controller roll is needed here.
        const auto face_hmd = quaternion_between(
            {0.0f, 0.0f, 1.0f},
            normalized(tracking.hmd_position - target));

        UEVR_OpenXRCompositorQuadState state{};
        state.size = sizeof(state);
        state.flags = UEVR_OPENXR_COMPOSITOR_QUAD_VISIBLE;
        state.texture = m_world_reticle_render_target->to_handle();
        state.position = {target.x, target.y, target.z};
        state.rotation = {
            face_hmd.x,
            face_hmd.y,
            face_hmd.z,
            face_hmd.w};
        state.size_meters = {
            reticle_texture_extent_meters,
            reticle_texture_extent_meters};

        if (!API::VR::set_openxr_compositor_quad(state)) {
            clear_openxr_reticle_quad(true);
            return;
        }

        m_openxr_reticle_published = true;
        // Keep the WidgetComponent rendering its 250x250 target, but remove
        // its mesh from the Unreal main pass. This eliminates the old grey
        // plane/cylinder without suppressing the live authored pixels copied
        // into the compositor swapchain.
        if (!m_world_reticle_main_pass_disabled &&
            set_world_reticle_main_pass(false)) {
            m_world_reticle_main_pass_disabled = true;
            API::get()->log_info(
                "HaloCEMotionControls: dedicated OpenXR reticle quad active; "
                "world WidgetComponent geometry removed from the main pass");
        }
    }

    // Two-hand latch and blend, decided once per engine tick from the
    // published snapshot (Halo-MCC-VR decides its latch once per frame for
    // the same reason: edge detection cannot live in a multi-call getter).
    void update_two_hand_hold(float delta) {
        TrackingSnapshot tracking{};
        {
            const std::scoped_lock lock{g_tracking_mutex};
            tracking = g_tracking_snapshot;
        }

        bool latched = false;
        bool in_zone = false;
        if (g_two_hand_hold_enabled.load(std::memory_order_relaxed) &&
            g_gameplay_ready_published.load(std::memory_order_acquire) == 1 &&
            !g_game_paused.load(std::memory_order_acquire) &&
            tracking.valid && tracking.left_valid) {
            const auto aim_forward = rotate(
                normalized(tracking.right_aim_rotation),
                Vec3{0.0f, 0.0f, -1.0f});
            const auto hand_line =
                tracking.left_grip_position -
                tracking.right_grip_position;
            const auto along = dot(hand_line, aim_forward);
            const auto perpendicular = hand_line - aim_forward * along;
            const auto lateral = std::sqrt(length_squared(perpendicular));
            in_zone =
                std::isfinite(along) && std::isfinite(lateral) &&
                along > kTwoHandZoneMinAlongMeters &&
                along < kTwoHandZoneMaxAlongMeters &&
                lateral < kTwoHandZoneRadiusMeters;

            // Retry the lookup until the runtime has created its action set;
            // caching a permanent null here would silently kill the feature.
            static UEVR_ActionHandle grip_action = nullptr;
            if (grip_action == nullptr) {
                grip_action = API::VR::get_action_handle(
                    "/actions/default/in/Grip");
            }
            const auto grip_held =
                grip_action != nullptr &&
                API::VR::is_action_active(
                    grip_action,
                    API::VR::get_left_joystick_source());

            const auto was_latched = g_two_hand_hold_latched.load(
                std::memory_order_relaxed);
            latched = grip_held && (was_latched || in_zone);
        }
        g_two_hand_zone_active.store(in_zone, std::memory_order_relaxed);

        const auto previous = g_two_hand_hold_latched.exchange(
            latched,
            std::memory_order_acq_rel);
        if (previous != latched) {
            // Acquisition buzz on the support hand. The C ABI takes
            // (seconds_from_now, duration, frequency, amplitude); the C++
            // wrapper's parameter names disagree with that order, so these
            // values are ordered for the ABI, not the wrapper names.
            API::VR::trigger_haptic_vibration(
                0.0f,
                latched ? 0.12f : 0.06f,
                0.0f,
                latched ? 0.7f : 0.35f,
                API::VR::get_left_joystick_source());
        }

        // The floating ball is misleading both while two-handed (shots
        // follow the hand-to-hand line) and while zoomed (shots keep stock
        // screen-center aim). The publisher only writes on transitions.
        publish_reticle_hide(
            latched || g_local_zoomed.load(std::memory_order_acquire));

        auto blend = g_two_hand_hold_blend.load(std::memory_order_relaxed);
        const auto target = latched ? 1.0f : 0.0f;
        const auto step =
            std::clamp(delta, 0.0f, 0.25f) / kTwoHandBlendSeconds;
        if (blend < target) {
            blend = std::min(target, blend + step);
        } else {
            blend = std::max(target, blend - step);
        }
        g_two_hand_hold_blend.store(blend, std::memory_order_relaxed);
    }

    void maintain_weapon_attachment() {
        auto& api = API::get();
        clear_ue_visual_attachment();

        auto* pawn_class =
            api->find_uobject<API::UClass>(L"Class /Script/BlamEngine.BlamPawn");
        auto* synchronization_component_class =
            api->find_uobject<API::UClass>(
                L"Class /Script/BlamSynchronization."
                L"BlamObjectSynchronizationComponent");
        if (pawn_class == nullptr ||
            synchronization_component_class == nullptr) {
            g_local_weapon_index.store(
                kInvalidBlamObjectIndex,
                std::memory_order_release);
            publish_gameplay_ready(false);
            return;
        }

        API::UObject* third_person_weapon = nullptr;
        API::UObject* first_person_weapon = nullptr;
        auto inspect_pawn = [&](API::UObject* pawn) {
            if (pawn == nullptr || !pawn->is_a(pawn_class)) {
                return;
            }

            auto* function =
                pawn->get_class()->find_function(
                    L"GetPawnViewModeAndWeaponActors");
            if (function == nullptr) {
                return;
            }

            GetPawnViewModeAndWeaponActorsParams params{};
            pawn->process_event(function, &params);
            if (params.first_person_weapon != nullptr) {
                first_person_weapon = params.first_person_weapon;
                third_person_weapon = params.third_person_weapon;
            }
        };

        // The local pawn returned by UEVR is a Blueprint subclass and is not
        // guaranteed to be in UObjectHook's exact-class cache. It is already
        // the engine's authoritative local-pawn pointer, so inspect it
        // directly.
        auto* const local_pawn = api->get_local_pawn(0);
        inspect_pawn(local_pawn);
        update_game_paused(local_pawn);

        if (first_person_weapon == nullptr) {
            g_local_weapon_index.store(
                kInvalidBlamObjectIndex,
                std::memory_order_release);
            g_visual_weapon_attached.store(false, std::memory_order_release);
            g_attached_component.store(0, std::memory_order_release);
            publish_gameplay_ready(false);
            return;
        }

        auto weapon_index = get_blam_object_index(
            first_person_weapon,
            synchronization_component_class);
        if (weapon_index == kInvalidBlamObjectIndex) {
            weapon_index = get_blam_object_index(
                third_person_weapon,
                synchronization_component_class);
        }

        const auto previous_index = g_local_weapon_index.exchange(
            weapon_index,
            std::memory_order_acq_rel);
        publish_gameplay_ready(
            weapon_index != kInvalidBlamObjectIndex);
        if (weapon_index != previous_index) {
            // Each weapon's stock left-hand pose differs, so the latched
            // controller-to-wrist convention must be resampled from the new
            // weapon's first tracked frame.
            g_left_wrist_stock_latched.store(
                false, std::memory_order_release);
        }
        if (weapon_index != kInvalidBlamObjectIndex &&
            weapon_index != previous_index) {
            api->log_info(
                "HaloCEMotionControls: local Blam weapon index is 0x%08x",
                static_cast<std::uint32_t>(weapon_index));
        }
    }

    // Two-handing (and its LEFT_SHOULDER masking) is gameplay-only: a grip
    // with the hands coincidentally aligned must not buzz or eat menu input
    // while Halo is paused. Fail open to "not paused" when the statics
    // class, function, or world context is unavailable.
    void update_game_paused(API::UObject* world_context) {
        bool paused = false;
        if (world_context != nullptr) {
            auto* const statics_class =
                API::get()->find_uobject<API::UClass>(
                    L"Class /Script/Engine.GameplayStatics");
            auto* const statics = statics_class != nullptr
                ? statics_class->get_class_default_object()
                : nullptr;
            auto* const function = statics_class != nullptr
                ? statics_class->find_function(L"IsGamePaused")
                : nullptr;
            if (statics != nullptr && function != nullptr) {
                IsGamePausedParams params{};
                params.world_context = world_context;
                statics->process_event(function, &params);
                paused = params.return_value;
            }
        }
        g_game_paused.store(paused, std::memory_order_release);
    }

    void clear_ue_visual_attachment() {
        for (auto* component : m_attached_components) {
            if (component != nullptr) {
                API::UObjectHook::remove_motion_controller_state(component);
            }
        }
        m_attached_components.clear();
        m_attached_weapon_actor = nullptr;

        if (m_attached_component != nullptr &&
            API::UObjectHook::get_motion_controller_state(
                m_attached_component) != nullptr) {
            API::UObjectHook::remove_motion_controller_state(
                m_attached_component);
        }
        m_attached_component = nullptr;
        g_visual_weapon_attached.store(false, std::memory_order_release);
        g_attached_component.store(0, std::memory_order_release);
    }

    bool m_armed{};
    bool m_native_hook_attempted{};
    bool m_native_hooks_installed{};
    float m_engine_uptime{};
    float m_discovery_timer{};
    API::UObject* m_attached_component{};
    API::UObject* m_attached_weapon_actor{};
    std::vector<API::UObject*> m_attached_components{};
    API::UObject* m_reticle_owner{};
    API::UObject* m_world_reticle_component{};
    API::UObject* m_world_reticle_widget{};
    bool m_world_reticle_widget_rooted{};
    API::UObject* m_world_reticle_image{};
    bool m_world_reticle_pruned{};
    API::UObject* m_world_reticle_pass_material{};
    API::UObject* m_world_reticle_render_target{};
    bool m_world_reticle_exposure_compensated{};
    API::UObject* m_world_reticle_base_material{};
    bool m_world_reticle_depth_test_was_disabled{};
    bool m_world_reticle_depth_override_active{};
    bool m_openxr_reticle_published{};
    bool m_world_reticle_main_pass_disabled{};
    API::UObject* m_screen_reticle_image{};
    bool m_screen_reticle_suppressed{};
    API::UObject* m_shared_reticle_material{};
    int m_reticle_state{-1};
};

std::unique_ptr<HaloCEMotionControls> g_plugin{
    std::make_unique<HaloCEMotionControls>()};
