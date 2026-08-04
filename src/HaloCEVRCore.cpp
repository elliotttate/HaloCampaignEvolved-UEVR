#include "HaloCEVRCore.hpp"

#include <array>
#include <cstdio>
#include <limits>

namespace halo_cevr {

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kEpsilon = 1.0e-7f;

Quaternion scaled_rotation(Quaternion rotation, float amount) {
    amount = std::clamp(amount, 0.0f, 1.0f);
    rotation = normalized(rotation);
    if (rotation.w < 0.0f) {
        rotation = {
            -rotation.x, -rotation.y, -rotation.z, -rotation.w};
    }
    const auto half_angle = std::acos(std::clamp(rotation.w, -1.0f, 1.0f));
    const auto sine = std::sin(half_angle);
    if (amount <= 0.0f || std::abs(sine) < 1.0e-6f) {
        return {};
    }
    const auto scale = std::sin(half_angle * amount) / sine;
    return normalized({
        rotation.x * scale, rotation.y * scale, rotation.z * scale,
        std::cos(half_angle * amount)});
}
}

Vector3 operator+(const Vector3& a, const Vector3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vector3 operator-(const Vector3& a, const Vector3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vector3 operator*(const Vector3& value, float scale) {
    return {value.x * scale, value.y * scale, value.z * scale};
}

Quaternion operator*(const Quaternion& a, const Quaternion& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

float dot(const Vector3& a, const Vector3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float length(const Vector3& value) {
    return std::sqrt(dot(value, value));
}

Vector3 normalized(const Vector3& value) {
    const auto magnitude = length(value);
    return magnitude > kEpsilon ? value * (1.0f / magnitude) : Vector3{};
}

Quaternion normalized(const Quaternion& value) {
    const auto magnitude = std::sqrt(
        value.x * value.x + value.y * value.y +
        value.z * value.z + value.w * value.w);
    if (magnitude <= kEpsilon || !std::isfinite(magnitude)) {
        return {};
    }
    const auto inverse = 1.0f / magnitude;
    return {
        value.x * inverse, value.y * inverse,
        value.z * inverse, value.w * inverse};
}

Quaternion conjugate(const Quaternion& value) {
    return {-value.x, -value.y, -value.z, value.w};
}

Vector3 rotate(const Quaternion& rotation, const Vector3& value) {
    const auto q = normalized(rotation);
    const Quaternion v{value.x, value.y, value.z, 0.0f};
    const auto result = q * v * conjugate(q);
    return {result.x, result.y, result.z};
}

Quaternion quaternion_between(const Vector3& source, const Vector3& destination) {
    const auto from = normalized(source);
    const auto to = normalized(destination);
    const auto alignment = std::clamp(dot(from, to), -1.0f, 1.0f);
    if (alignment > 1.0f - 1.0e-6f) {
        return {};
    }
    if (alignment < -1.0f + 1.0e-6f) {
        const auto axis = std::abs(from.x) < 0.8f
            ? normalized(Vector3{0.0f, from.z, -from.y})
            : normalized(Vector3{-from.z, 0.0f, from.x});
        return {axis.x, axis.y, axis.z, 0.0f};
    }
    const Vector3 axis{
        from.y * to.z - from.z * to.y,
        from.z * to.x - from.x * to.z,
        from.x * to.y - from.y * to.x};
    return normalized(Quaternion{axis.x, axis.y, axis.z, 1.0f + alignment});
}

Pose apply_offset(const Pose& pose, const PoseOffset& offset) {
    return {
        pose.position + rotate(pose.rotation, offset.translation),
        normalized(pose.rotation * offset.rotation)};
}

PoseOffset compose_offsets(const PoseOffset& first, const PoseOffset& second) {
    return {
        first.translation + rotate(first.rotation, second.translation),
        normalized(first.rotation * second.rotation)};
}

PoseOffset inverse_offset(const PoseOffset& offset) {
    const auto inverse_rotation = conjugate(normalized(offset.rotation));
    return {
        rotate(inverse_rotation, offset.translation * -1.0f),
        inverse_rotation};
}

PoseOffset solve_offset(const Pose& raw, const Pose& desired) {
    const auto inverse_raw = conjugate(normalized(raw.rotation));
    return {
        rotate(inverse_raw, desired.position - raw.position),
        normalized(inverse_raw * desired.rotation)};
}

StickVector radial_deadzone(StickVector value, float deadzone) {
    deadzone = std::clamp(deadzone, 0.0f, 0.95f);
    const auto magnitude = std::hypot(value.x, value.y);
    if (!std::isfinite(magnitude) || magnitude <= deadzone) {
        return {};
    }
    const auto clamped = std::min(magnitude, 1.0f);
    const auto scaled = (clamped - deadzone) / (1.0f - deadzone);
    const auto multiplier = scaled / magnitude;
    return {value.x * multiplier, value.y * multiplier};
}

StickVector rotate_stick(StickVector value, float degrees) {
    const auto radians = degrees * kPi / 180.0f;
    const auto cosine = std::cos(radians);
    const auto sine = std::sin(radians);
    return {
        value.x * cosine - value.y * sine,
        value.x * sine + value.y * cosine};
}

float wrap_degrees(float degrees) {
    if (!std::isfinite(degrees)) {
        return 0.0f;
    }
    auto result = std::fmod(degrees + 180.0f, 360.0f);
    if (result < 0.0f) {
        result += 360.0f;
    }
    return result - 180.0f;
}

float signed_angle_degrees(float from, float to) {
    return wrap_degrees(to - from);
}

float shaped_aim_axis(float error_degrees, float deadzone_degrees,
                      float full_scale_degrees, float minimum_output) {
    const auto magnitude = std::abs(error_degrees);
    deadzone_degrees = std::max(deadzone_degrees, 0.0f);
    full_scale_degrees = std::max(full_scale_degrees, deadzone_degrees + 0.01f);
    minimum_output = std::clamp(minimum_output, 0.0f, 1.0f);
    if (!std::isfinite(magnitude) || magnitude <= deadzone_degrees) {
        return 0.0f;
    }
    const auto progress = std::clamp(
        (magnitude - deadzone_degrees) /
            (full_scale_degrees - deadzone_degrees),
        0.0f, 1.0f);
    const auto shaped = minimum_output + (1.0f - minimum_output) * progress;
    return std::copysign(shaped, error_degrees);
}

bool physical_left_for_visual_hand(bool visual_right_hand,
                                   bool dominant_hand_left) {
    return visual_right_hand ? dominant_hand_left : !dominant_hand_left;
}

HandChannels visual_hand_channels(const HandChannels& physical_left,
                                  const HandChannels& physical_right,
                                  bool visual_right_hand,
                                  bool dominant_hand_left,
                                  bool support_hold_active) {
    auto result = physical_left_for_visual_hand(
        visual_right_hand, dominant_hand_left)
        ? physical_left
        : physical_right;
    result.index = std::clamp(result.index, 0.0f, 1.0f);
    result.grip = std::clamp(result.grip, 0.0f, 1.0f);
    result.thumb = std::clamp(result.thumb, 0.0f, 1.0f);
    if (visual_right_hand) {
        // The visual right hand is always the weapon hand, and a hand
        // holding a weapon is holding it: keep it wrapped around the grip
        // regardless of the physical grip button, with the index at least
        // resting on the trigger. The live trigger pull still closes the
        // index further.
        result.grip = 1.0f;
        result.thumb = 1.0f;
        result.index = std::max(result.index, 0.6f);
    }
    if (!visual_right_hand && support_hold_active) {
        // Two-hand hold: the support hand is wrapped around the barrel, so
        // the whole fist closes, index included.
        result.grip = 1.0f;
        result.thumb = 1.0f;
        result.index = std::max(result.index, 0.9f);
    }
    result.thumb = std::max(result.thumb, result.grip);
    return result;
}

float smooth_hand_channel(float current, float target, float delta_seconds,
                          float closing_time_seconds,
                          float opening_time_seconds) {
    current = std::clamp(current, 0.0f, 1.0f);
    target = std::clamp(target, 0.0f, 1.0f);
    delta_seconds = std::clamp(delta_seconds, 0.0f, 0.25f);
    closing_time_seconds = std::max(closing_time_seconds, 1.0e-4f);
    opening_time_seconds = std::max(opening_time_seconds, 1.0e-4f);
    const auto time_constant = target > current
        ? closing_time_seconds
        : opening_time_seconds;
    const auto alpha = 1.0f - std::exp(-delta_seconds / time_constant);
    return current + (target - current) * alpha;
}

FingerOpeningResult solve_finger_opening(
    const Vector3& wrist_position,
    const std::array<Vector3, 4>& closed_positions,
    float curl) {
    FingerOpeningResult result{};
    result.positions = closed_positions;
    const auto open_amount = 1.0f - std::clamp(curl, 0.0f, 1.0f);
    const auto extension = normalized(
        closed_positions.front() - wrist_position);
    if (length(extension) < 0.9f) {
        return result;
    }

    for (std::size_t joint = 0; joint < result.joint_rotations.size(); ++joint) {
        const auto current = normalized(
            result.positions[joint + 1] - result.positions[joint]);
        if (length(current) < 0.9f) {
            return result;
        }
        const auto rotation = scaled_rotation(
            quaternion_between(current, extension), open_amount);
        result.joint_rotations[joint] = rotation;
        const auto pivot = result.positions[joint];
        for (auto index = joint; index < result.positions.size(); ++index) {
            result.positions[index] =
                pivot + rotate(rotation, result.positions[index] - pivot);
        }
    }
    result.valid = true;
    return result;
}

std::string stable_weapon_key(std::wstring_view object_name) {
    // FNV-1a over normalized UTF-16 code units is deterministic across runs,
    // unlike a UObject address or Blam datum index.
    std::uint64_t hash = 14695981039346656037ULL;
    for (auto value : object_name) {
        if (value >= L'A' && value <= L'Z') {
            value = static_cast<wchar_t>(value - L'A' + L'a');
        }
        hash ^= static_cast<std::uint16_t>(value);
        hash *= 1099511628211ULL;
    }
    std::array<char, 17> text{};
    std::snprintf(text.data(), text.size(), "%016llx",
                  static_cast<unsigned long long>(hash));
    return text.data();
}

} // namespace halo_cevr
