#include <uevr/Plugin.hpp>

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
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
constexpr std::uint32_t kFirstPersonNodeCount = 76;

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
    Quat right_aim_rotation{};
    Vec3 left_grip_position{};
    Quat left_grip_rotation{};
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
std::atomic_bool g_visual_weapon_attached{};
std::atomic_uintptr_t g_attached_component{};
std::atomic_bool g_two_hand_ik_enabled{true};
std::atomic_bool g_two_hand_ik_fallback_logged{};
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
struct LocalFireMarkerCorrection {
    BlamMatrix4x3 source{};
    BlamMatrix4x3 desired{};
};
thread_local std::array<LocalFireMarkerCorrection, 64>
    g_local_fire_marker_corrections{};
thread_local std::uint16_t g_local_fire_marker_correction_count{};
struct LocalProjectileDirectionOverride {
    std::uint32_t object_index{};
    Vec3 source_direction{};
    Vec3 desired_direction{};
    Mat3 delta_basis{};
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
                std::memcmp(contents.data(), "right", 5) == 0;
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
        const bool want_hidden =
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

bool build_controller_marker(
    const MarkerResult& source,
    const TrackingSnapshot& tracking,
    BlamMatrix4x3& destination) {
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

    const auto controller_basis =
        openxr_rotation_to_blam_basis(aim_relative_xr);
    if (!valid_basis(controller_basis)) {
        return false;
    }

    const auto desired_root_basis = multiply(root_basis, controller_basis);
    const auto grip_delta_blam =
        openxr_to_blam(grip_delta_xr) / kMetersPerBlamUnit;
    const auto desired_root_position =
        root_position + transform_vector(root_basis, grip_delta_blam);

    destination = source.world;
    destination.forward = normalized(
        transform_vector(desired_root_basis, source.local.forward));
    destination.left = normalized(
        transform_vector(desired_root_basis, source.local.left));
    destination.up = normalized(
        transform_vector(desired_root_basis, source.local.up));
    destination.position =
        desired_root_position +
        transform_vector(desired_root_basis, source.local.position);

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
    const auto shoulder_position = palette[shoulder_node].position;
    const auto elbow_position = palette[elbow_node].position;
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

    const auto controller_basis =
        openxr_rotation_to_blam_basis(aim_relative_xr);
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

    const auto source_right_wrist_basis =
        orthonormal_basis(palette[19]);
    const auto right_wrist_target =
        delta_position +
        transform_vector(delta_basis, palette[19].position);
    const auto right_wrist_basis =
        multiply(delta_basis, source_right_wrist_basis);
    if (!solve_two_bone_arm(
            palette,
            5,
            16,
            19,
            right_shoulder_nodes,
            right_elbow_nodes,
            right_wrist_nodes,
            right_wrist_target,
            right_wrist_basis,
            root_basis.up)) {
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
        const auto stock_left_wrist_basis =
            orthonormal_basis(palette[25]);
        const auto stock_left_wrist_relative =
            multiply(transpose(root_basis), stock_left_wrist_basis);
        const auto desired_left_wrist_basis = multiply(
            multiply(root_basis, left_controller_basis),
            stock_left_wrist_relative);
        const auto left_grip_delta_blam =
            openxr_to_blam(left_grip_delta_xr) / kMetersPerBlamUnit;
        const auto left_wrist_target =
            palette[0].position +
            transform_vector(root_basis, left_grip_delta_blam);
        if (!finite(left_grip_delta_xr) ||
            !valid_basis(left_controller_basis) ||
            !solve_two_bone_arm(
                palette,
                6,
                9,
                25,
                left_shoulder_nodes,
                left_elbow_nodes,
                left_wrist_nodes,
                left_wrist_target,
                desired_left_wrist_basis,
                root_basis.up)) {
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
    const auto controller_basis =
        openxr_rotation_to_blam_basis(aim_relative_xr);
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
    if (g_two_hand_ik_enabled.load(std::memory_order_relaxed) &&
        apply_split_controllers_to_first_person_palette(
            palette,
            count,
            tracking)) {
        return true;
    }

    if (g_two_hand_ik_enabled.load(std::memory_order_relaxed) &&
        !g_two_hand_ik_fallback_logged.exchange(
            true,
            std::memory_order_relaxed)) {
        API::get()->log_warn(
            "HaloCEMotionControls: split arm IK rejected this palette; "
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

    if (!g_native_override_enabled.load(std::memory_order_relaxed) ||
        g_local_fire_depth == 0 || return_address != g_primary_marker_return ||
        maximum_count != 64 || output == nullptr || count <= 0 ||
        count > static_cast<std::int16_t>(maximum_count)) {
        return count;
    }

    TrackingSnapshot tracking{};
    {
        const std::scoped_lock lock{g_tracking_mutex};
        tracking = g_tracking_snapshot;
    }
    if (!tracking.valid) {
        return count;
    }

    std::uint32_t rewritten{};
    BlamMatrix4x3 first_source{};
    BlamMatrix4x3 first_desired{};
    bool have_first_rewrite{};
    for (std::int16_t index = 0; index < count; ++index) {
        BlamMatrix4x3 desired{};
        if (!build_controller_marker(output[index], tracking, desired)) {
            continue;
        }

        if (!have_first_rewrite) {
            first_source = output[index].world;
            first_desired = desired;
            have_first_rewrite = true;
        }
        if (g_local_fire_marker_correction_count <
            g_local_fire_marker_corrections.size()) {
            g_local_fire_marker_corrections
                [g_local_fire_marker_correction_count++] = {
                    output[index].world,
                    desired};
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
        if (valid_basis(source_basis) && valid_basis(desired_basis) &&
            valid_basis(delta_basis) && finite(data->position) &&
             finite(data->forward) && finite(data->up) &&
             finite(data->velocity) && finite(corrected_forward) &&
             finite(desired_up) && finite(desired_velocity) &&
             length_squared(corrected_forward) > 0.8f &&
             length_squared(desired_up) > 0.8f) {
            source_forward = data->forward;
            const auto source_up = data->up;
            const auto source_velocity = data->velocity;
            desired_forward = corrected_forward;
            applied_delta = delta_basis;
            have_applied_delta = true;

            data->forward = corrected_forward;
            data->up = desired_up;
            // Some projectile types already carry inherited velocity in the
            // placement. Rotate it here, then also correct the spawned
            // object's tag-derived velocity after object_new returns.
            data->velocity = desired_velocity;

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
                    normalized(desired_forward),
                    applied_delta};
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
        const LocalProjectileDirectionOverride* direction_override = nullptr;
        for (std::uint16_t index = 0;
             index < g_local_projectile_direction_override_count;
             ++index) {
            const auto& candidate =
                g_local_projectile_direction_overrides[index];
            if (candidate.object_index == projectile_object_index) {
                direction_override = &candidate;
                break;
            }
        }

        if (direction_override != nullptr && finite(*start) && finite(*end) &&
            finite(direction_override->source_direction) &&
            finite(direction_override->desired_direction) &&
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

                // projectile_new often makes the primary first-tick sweep
                // controller-aligned before it reaches this hook. Leave that
                // ray alone. Halo's conical multi-ray path (shotgun spread)
                // can still build its vectors around the original game aim;
                // rotate each of those vectors by the complete source-to-hand
                // basis delta so the cone and each ray's length survive.
                auto corrected_direction = original_direction;
                bool rotated = false;
                if (source_alignment > 0.70f &&
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

    TrackingSnapshot tracking{};
    {
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
        std::array<wchar_t, 16> two_hand_setting{};
        const auto two_hand_setting_length = GetEnvironmentVariableW(
            L"UEVR_HALO_TWO_HAND_IK",
            two_hand_setting.data(),
            static_cast<DWORD>(two_hand_setting.size()));
        if (two_hand_setting_length > 0 &&
            two_hand_setting_length < two_hand_setting.size()) {
            const auto first = static_cast<wchar_t>(
                std::towlower(two_hand_setting.front()));
            g_two_hand_ik_enabled.store(
                first != L'0' && first != L'f' &&
                first != L'n' && first != L'o',
                std::memory_order_release);
        }
        const auto marker = reticle_active_marker_path();
        if (!marker.empty()) {
            DeleteFileW(marker.c_str());
        }
        publish_gameplay_ready(false, true);
        g_replacement_reticle_active.store(false, std::memory_order_release);
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
            "muzzle are owned by the right controller; two-hand IK=%d; "
            "game camera aim and pitch/UI compensation remain disabled",
            g_two_hand_ik_enabled.load(std::memory_order_relaxed) ? 1 : 0);
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

        refresh_replacement_reticle_state();

        m_discovery_timer -= std::max(delta, 0.0f);
        if (m_discovery_timer > 0.0f) {
            return;
        }
        m_discovery_timer = 0.25f;

        maintain_weapon_attachment();
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
        TrackingSnapshot snapshot{};
        if (API::VR::is_runtime_ready() && API::VR::is_hmd_active()) {
            const auto hmd_index = API::VR::get_hmd_index();
            const auto right_index = API::VR::get_right_controller_index();
            const auto left_index = API::VR::get_left_controller_index();
            if (hmd_index < 0 || hmd_index >= 64 || right_index < 0 ||
                right_index >= 64) {
                const std::scoped_lock lock{g_tracking_mutex};
                g_tracking_snapshot = snapshot;
                g_tracking_valid.store(false, std::memory_order_release);
                g_left_tracking_valid.store(
                    false,
                    std::memory_order_release);
                return;
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
                finite(snapshot.right_aim_rotation);

            if (left_index >= 0 && left_index < 64) {
                const auto left_grip =
                    API::VR::get_grip_pose(left_index);
                snapshot.left_grip_position = {
                    left_grip.position.x,
                    left_grip.position.y,
                    left_grip.position.z};
                snapshot.left_grip_rotation = {
                    left_grip.rotation.x,
                    left_grip.rotation.y,
                    left_grip.rotation.z,
                    left_grip.rotation.w};
                snapshot.left_valid =
                    finite(snapshot.left_grip_position) &&
                    finite(snapshot.left_grip_rotation);
            }
        }

        const std::scoped_lock lock{g_tracking_mutex};
        g_tracking_snapshot = snapshot;
        g_tracking_valid.store(snapshot.valid, std::memory_order_release);
        g_left_tracking_valid.store(
            snapshot.left_valid,
            std::memory_order_release);
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
        inspect_pawn(api->get_local_pawn(0));

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
        if (weapon_index != kInvalidBlamObjectIndex &&
            weapon_index != previous_index) {
            api->log_info(
                "HaloCEMotionControls: local Blam weapon index is 0x%08x",
                static_cast<std::uint32_t>(weapon_index));
        }
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
};

std::unique_ptr<HaloCEMotionControls> g_plugin{
    std::make_unique<HaloCEMotionControls>()};
