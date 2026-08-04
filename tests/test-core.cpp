#include "HaloCEVRConfig.hpp"
#include "HaloCEVRCore.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

bool near(float a, float b, float tolerance = 1.0e-4f) {
    return std::abs(a - b) <= tolerance;
}

bool near(const halo_cevr::Vector3& a, const halo_cevr::Vector3& b,
          float tolerance = 1.0e-4f) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) &&
           near(a.z, b.z, tolerance);
}

bool same_rotation(const halo_cevr::Quaternion& a,
                   const halo_cevr::Quaternion& b,
                   float tolerance = 1.0e-4f) {
    const auto dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    return std::abs(std::abs(dot) - 1.0f) <= tolerance;
}

} // namespace

int main() {
    using namespace halo_cevr;
    constexpr float s = 0.70710678118f;
    const Pose raw{{1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, s, s}};
    const Pose desired{{4.0f, -1.0f, 2.0f}, {s, 0.0f, 0.0f, s}};
    const auto solved = solve_offset(raw, desired);
    const auto reapplied = apply_offset(raw, solved);
    require(near(reapplied.position, desired.position),
            "pose calibration must reproduce desired position");
    require(same_rotation(reapplied.rotation, desired.rotation),
            "pose calibration must reproduce desired rotation");

    const PoseOffset first{{0.1f, -0.2f, 0.3f}, {0.0f, 0.0f, s, s}};
    const PoseOffset second{{-0.4f, 0.5f, 0.2f}, {s, 0.0f, 0.0f, s}};
    const auto sequential = apply_offset(apply_offset(raw, first), second);
    const auto composed = apply_offset(raw, compose_offsets(first, second));
    require(near(sequential.position, composed.position),
            "offset composition position");
    require(same_rotation(sequential.rotation, composed.rotation),
            "offset composition rotation");
    const auto identity = compose_offsets(first, inverse_offset(first));
    require(near(identity.translation, {}), "offset inverse translation");
    require(same_rotation(identity.rotation, {}), "offset inverse rotation");

    require(near(radial_deadzone({0.05f, 0.0f}, 0.12f).x, 0.0f),
            "radial deadzone must suppress idle input");
    const auto diagonal = radial_deadzone({0.6f, 0.8f}, 0.12f);
    require(near(std::hypot(diagonal.x, diagonal.y), 1.0f),
            "radial deadzone must preserve direction and full scale");
    const auto rotated = rotate_stick({0.0f, 1.0f}, 90.0f);
    require(near(rotated.x, -1.0f) && near(rotated.y, 0.0f),
            "head-relative movement rotation");
    require(near(signed_angle_degrees(179.0f, -179.0f), 2.0f),
            "wrapped aim error");
    require(near(shaped_aim_axis(0.5f, 0.8f, 7.0f, 0.26f), 0.0f),
            "aim follower deadband");
    require(shaped_aim_axis(2.0f, 0.8f, 7.0f, 0.26f) >= 0.26f,
            "aim follower minimum actionable output");

    const HandChannels physical_left{0.1f, 0.2f, 0.3f};
    const HandChannels physical_right{0.7f, 0.8f, 0.4f};
    auto hand = visual_hand_channels(
        physical_left, physical_right, true, false, false);
    require(near(hand.index, 0.7f) && near(hand.grip, 1.0f) &&
            near(hand.thumb, 1.0f),
            "weapon hand must stay gripped with the live trigger on top");
    hand = visual_hand_channels(
        physical_left, physical_right, false, false, false);
    require(near(hand.index, 0.1f) && near(hand.grip, 0.2f) &&
            near(hand.thumb, 0.3f),
            "right-dominant support hand must use physical left independently");
    hand = visual_hand_channels(
        physical_left, physical_right, true, true, false);
    require(near(hand.index, 0.6f) && near(hand.grip, 1.0f),
            "left-dominant weapon hand must rest its index on the trigger");
    hand = visual_hand_channels(
        physical_left, physical_right, false, true, true);
    require(near(hand.index, 0.9f) && near(hand.grip, 1.0f) &&
            near(hand.thumb, 1.0f),
            "support hold must close the whole support fist");
    require(physical_left_for_visual_hand(true, true) &&
            !physical_left_for_visual_hand(true, false) &&
            !physical_left_for_visual_hand(false, true) &&
            physical_left_for_visual_hand(false, false),
            "handedness routing truth table");

    float closing = 0.0f;
    float opening = 1.0f;
    for (int frame = 0; frame < 30; ++frame) {
        const auto next_closing = smooth_hand_channel(closing, 1.0f, 1.0f / 90.0f);
        const auto next_opening = smooth_hand_channel(opening, 0.0f, 1.0f / 90.0f);
        require(next_closing >= closing && next_closing <= 1.0f,
                "closing interpolation must be monotonic");
        require(next_opening <= opening && next_opening >= 0.0f,
                "opening interpolation must be monotonic");
        closing = next_closing;
        opening = next_opening;
    }
    require(closing > 0.98f && opening < 0.06f,
            "finger smoothing must converge at a 90 Hz headset cadence");
    const auto reversed = smooth_hand_channel(closing, 0.0f, 1.0f / 90.0f);
    require(reversed < closing,
            "finger smoothing must reverse immediately without a timer latch");

    const Vector3 wrist{};
    const std::array<Vector3, 4> curled_finger{{
        {0.20f, 0.00f, 0.00f},
        {0.20f, 0.10f, 0.00f},
        {0.15f, 0.17f, 0.00f},
        {0.05f, 0.20f, 0.00f}}};
    float previous_tip_distance = -1.0f;
    for (const auto curl : {1.0f, 0.75f, 0.5f, 0.25f, 0.0f}) {
        const auto finger = solve_finger_opening(wrist, curled_finger, curl);
        require(finger.valid,
                "finger solver must accept a valid four-node chain");
        const auto tip_distance = length(finger.positions.back() - wrist);
        require(tip_distance + 1.0e-5f >= previous_tip_distance,
                "fingertip-to-palm distance must increase monotonically as the hand opens");
        previous_tip_distance = tip_distance;
    }
    const auto closed_finger = solve_finger_opening(
        wrist, curled_finger, 1.0f);
    require(near(closed_finger.positions.back(), curled_finger.back()),
            "zero openness must preserve the authored weapon grip exactly");

    const auto config = parse_runtime_config(
        "enabled=no\n"
        "floating_hands=1\n"
        "dominant_hand=LEFT\n"
        "turn_mode=smooth\n"
        "movement_deadzone=9\n"
        "reticle_distance_meters=-3\n"
        "prevent_controller_sleep=false\n"
        "logical_aim_follower=true\n");
    require(!config.enabled && config.floating_hands,
            "typed boolean parsing");
    require(config.dominant_hand == DominantHand::Left,
            "dominant hand parsing");
    require(config.turn_mode == TurnMode::Smooth,
            "turn mode parsing");
    require(near(config.movement_deadzone, 0.9f) &&
            near(config.reticle_distance_meters, 0.5f),
            "numeric settings must clamp safely");
    require(config.logical_aim_follower, "logical follower parsing");
    require(!config.prevent_controller_sleep,
            "controller sleep policy parsing");

    CalibrationState persisted{};
    persisted.controller = first;
    persisted.weapons.emplace("abcdef", second);
    const auto reparsed = parse_calibration(serialize_calibration(persisted));
    require(near(reparsed.controller.translation, first.translation),
            "controller calibration persistence");
    require(reparsed.weapons.contains("abcdef") &&
            near(reparsed.weapons.at("abcdef").translation, second.translation),
            "per-weapon calibration persistence");
    require(stable_weapon_key(L"Weapon_Rifle") ==
            stable_weapon_key(L"weapon_rifle"),
            "weapon keys must be case-stable");

    std::cout << "HaloCEVR core tests passed\n";
    return 0;
}
