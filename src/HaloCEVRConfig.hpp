#pragma once

#include "HaloCEVRCore.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace halo_cevr {

enum class DominantHand { Right, Left };
enum class TurnMode { Off, Snap, Smooth };

struct RuntimeConfig {
    bool enabled{true};
    bool floating_hands{true};
    bool two_hand_hold{true};
    DominantHand dominant_hand{DominantHand::Right};

    bool head_relative_movement{true};
    float movement_deadzone{0.12f};
    TurnMode turn_mode{TurnMode::Snap};
    float snap_turn_degrees{30.0f};
    float turn_deadzone{0.55f};
    float smooth_turn_degrees_per_second{90.0f};
    bool dpad_shift{true};
    float dpad_deadzone{0.55f};
    bool right_stick_down_crouch{true};
    bool menu_button_is_back{true};

    bool cutscene_comfort{true};
    bool logical_aim_follower{false};
    float logical_aim_deadzone_degrees{0.8f};
    float logical_aim_full_scale_degrees{7.0f};
    float logical_aim_minimum_output{0.26f};

    float reticle_distance_meters{10.0f};
    float reticle_extent_meters{2.5f};

    std::uint32_t controller_calibration_key{0x23}; // End
    std::uint32_t weapon_calibration_key{0x22}; // Page Down
    std::uint32_t kill_switch_key{0x24}; // Ctrl+Home
};

struct CalibrationState {
    PoseOffset controller{};
    std::unordered_map<std::string, PoseOffset> weapons{};
};

RuntimeConfig parse_runtime_config(std::string_view text);
CalibrationState parse_calibration(std::string_view text);
std::string default_runtime_config_text();
std::string serialize_calibration(const CalibrationState& state);

class ConfigStore {
public:
    bool initialize();
    bool reload_if_changed(bool force = false);
    bool save_calibration() const;

    const RuntimeConfig& runtime() const { return m_runtime; }
    const CalibrationState& calibration() const { return m_calibration; }
    CalibrationState& calibration() { return m_calibration; }
    PoseOffset weapon_calibration(std::string_view key) const;

    const std::filesystem::path& config_path() const { return m_config_path; }
    const std::filesystem::path& calibration_path() const { return m_calibration_path; }

private:
    std::filesystem::path m_config_path{};
    std::filesystem::path m_calibration_path{};
    std::filesystem::file_time_type m_config_write_time{};
    std::filesystem::file_time_type m_calibration_write_time{};
    RuntimeConfig m_runtime{};
    CalibrationState m_calibration{};
};

} // namespace halo_cevr
