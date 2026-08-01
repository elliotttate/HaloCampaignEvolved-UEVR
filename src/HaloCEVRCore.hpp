#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace halo_cevr {

struct Vector3 {
    float x{};
    float y{};
    float z{};
};

struct Quaternion {
    float x{};
    float y{};
    float z{};
    float w{1.0f};
};

struct Pose {
    Vector3 position{};
    Quaternion rotation{};
};

// A rigid transform expressed in a tracked pose's local frame. Applying two
// offsets is equivalent to multiplying their transforms on the right.
struct PoseOffset {
    Vector3 translation{};
    Quaternion rotation{};
};

Vector3 operator+(const Vector3& a, const Vector3& b);
Vector3 operator-(const Vector3& a, const Vector3& b);
Vector3 operator*(const Vector3& value, float scale);
Quaternion operator*(const Quaternion& a, const Quaternion& b);

float dot(const Vector3& a, const Vector3& b);
float length(const Vector3& value);
Vector3 normalized(const Vector3& value);
Quaternion normalized(const Quaternion& value);
Quaternion conjugate(const Quaternion& value);
Vector3 rotate(const Quaternion& rotation, const Vector3& value);
Quaternion quaternion_between(const Vector3& source, const Vector3& destination);

Pose apply_offset(const Pose& pose, const PoseOffset& offset);
PoseOffset compose_offsets(const PoseOffset& first, const PoseOffset& second);
PoseOffset inverse_offset(const PoseOffset& offset);
PoseOffset solve_offset(const Pose& raw, const Pose& desired);

struct StickVector {
    float x{};
    float y{};
};

StickVector radial_deadzone(StickVector value, float deadzone);
StickVector rotate_stick(StickVector value, float degrees);
float wrap_degrees(float degrees);
float signed_angle_degrees(float from, float to);
float shaped_aim_axis(float error_degrees, float deadzone_degrees,
                      float full_scale_degrees, float minimum_output);

struct HandChannels {
    float index{};
    float grip{};
    float thumb{};
};

// Halo's authored right-hand wrist is the semantic weapon hand and its
// authored left-hand wrist is the support hand. Keep that separate from the
// physical OpenXR controller side so handedness is resolved exactly once.
bool physical_left_for_visual_hand(bool visual_right_hand,
                                   bool dominant_hand_left);
HandChannels visual_hand_channels(const HandChannels& physical_left,
                                  const HandChannels& physical_right,
                                  bool visual_right_hand,
                                  bool dominant_hand_left,
                                  bool support_hold_active);
float smooth_hand_channel(float current, float target, float delta_seconds,
                          float closing_time_seconds = 0.065f,
                          float opening_time_seconds = 0.11f);

struct FingerOpeningResult {
    bool valid{};
    std::array<Quaternion, 3> joint_rotations{};
    std::array<Vector3, 4> positions{};
};

// Solve the exact joint rotations used to extend an authored closed finger.
// Returning the resulting positions also makes the deformation numerically
// verifiable without needing a running renderer.
FingerOpeningResult solve_finger_opening(
    const Vector3& wrist_position,
    const std::array<Vector3, 4>& closed_positions,
    float curl);

std::string stable_weapon_key(std::wstring_view object_name);

} // namespace halo_cevr
