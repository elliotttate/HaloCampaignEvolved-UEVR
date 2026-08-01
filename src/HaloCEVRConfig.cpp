#include "HaloCEVRConfig.hpp"

#include <Windows.h>

#include <array>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace halo_cevr {

namespace {

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string{value.substr(first, last - first + 1)};
}

std::unordered_map<std::string, std::string> parse_pairs(std::string_view text) {
    std::unordered_map<std::string, std::string> values{};
    std::istringstream lines{std::string{text}};
    for (std::string line; std::getline(lines, line);) {
        const auto comment = line.find_first_of("#;");
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        auto key = trim(std::string_view{line}.substr(0, separator));
        auto value = trim(std::string_view{line}.substr(separator + 1));
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (!key.empty()) {
            values.insert_or_assign(std::move(key), std::move(value));
        }
    }
    return values;
}

bool as_bool(const std::unordered_map<std::string, std::string>& values,
             std::string_view key, bool fallback) {
    const auto found = values.find(std::string{key});
    if (found == values.end()) {
        return fallback;
    }
    auto value = found->second;
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        return false;
    }
    return fallback;
}

float as_float(const std::unordered_map<std::string, std::string>& values,
               std::string_view key, float fallback, float minimum, float maximum) {
    const auto found = values.find(std::string{key});
    if (found == values.end()) {
        return fallback;
    }
    char* end{};
    const auto value = std::strtof(found->second.c_str(), &end);
    if (end == found->second.c_str() || !std::isfinite(value)) {
        return fallback;
    }
    return std::clamp(value, minimum, maximum);
}

std::uint32_t as_uint(const std::unordered_map<std::string, std::string>& values,
                      std::string_view key, std::uint32_t fallback) {
    const auto found = values.find(std::string{key});
    if (found == values.end()) {
        return fallback;
    }
    char* end{};
    const auto value = std::strtoul(found->second.c_str(), &end, 0);
    return end == found->second.c_str()
        ? fallback
        : static_cast<std::uint32_t>(value);
}

std::string as_string(const std::unordered_map<std::string, std::string>& values,
                      std::string_view key, std::string fallback) {
    const auto found = values.find(std::string{key});
    if (found == values.end()) {
        return fallback;
    }
    auto result = found->second;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

PoseOffset read_offset(
    const std::unordered_map<std::string, std::string>& values,
    const std::string& prefix) {
    PoseOffset result{};
    result.translation.x = as_float(values, prefix + ".tx", 0.0f, -2.0f, 2.0f);
    result.translation.y = as_float(values, prefix + ".ty", 0.0f, -2.0f, 2.0f);
    result.translation.z = as_float(values, prefix + ".tz", 0.0f, -2.0f, 2.0f);
    result.rotation.x = as_float(values, prefix + ".qx", 0.0f, -1.0f, 1.0f);
    result.rotation.y = as_float(values, prefix + ".qy", 0.0f, -1.0f, 1.0f);
    result.rotation.z = as_float(values, prefix + ".qz", 0.0f, -1.0f, 1.0f);
    result.rotation.w = as_float(values, prefix + ".qw", 1.0f, -1.0f, 1.0f);
    result.rotation = normalized(result.rotation);
    return result;
}

void write_offset(std::ostream& output, std::string_view prefix,
                  const PoseOffset& offset) {
    output << prefix << ".tx=" << offset.translation.x << '\n'
           << prefix << ".ty=" << offset.translation.y << '\n'
           << prefix << ".tz=" << offset.translation.z << '\n'
           << prefix << ".qx=" << offset.rotation.x << '\n'
           << prefix << ".qy=" << offset.rotation.y << '\n'
           << prefix << ".qz=" << offset.rotation.z << '\n'
           << prefix << ".qw=" << offset.rotation.w << '\n';
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return {};
    }
    return {std::istreambuf_iterator<char>{input}, {}};
}

bool write_file(const std::filesystem::path& path, std::string_view text) {
    const auto temporary = path.wstring() + L".tmp";
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        if (!output) {
            return false;
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            return false;
        }
    }
    return MoveFileExW(temporary.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

} // namespace

RuntimeConfig parse_runtime_config(std::string_view text) {
    RuntimeConfig result{};
    const auto values = parse_pairs(text);
    result.enabled = as_bool(values, "enabled", result.enabled);
    result.floating_hands = as_bool(values, "floating_hands", result.floating_hands);
    result.two_hand_hold = as_bool(values, "two_hand_hold", result.two_hand_hold);
    result.dominant_hand = as_string(values, "dominant_hand", "right") == "left"
        ? DominantHand::Left : DominantHand::Right;
    result.head_relative_movement = as_bool(
        values, "head_relative_movement", result.head_relative_movement);
    result.movement_deadzone = as_float(
        values, "movement_deadzone", result.movement_deadzone, 0.0f, 0.9f);
    const auto turn_mode = as_string(values, "turn_mode", "snap");
    result.turn_mode = turn_mode == "smooth" ? TurnMode::Smooth
        : turn_mode == "off" ? TurnMode::Off : TurnMode::Snap;
    result.snap_turn_degrees = as_float(
        values, "snap_turn_degrees", result.snap_turn_degrees, 1.0f, 180.0f);
    result.turn_deadzone = as_float(
        values, "turn_deadzone", result.turn_deadzone, 0.05f, 0.95f);
    result.smooth_turn_degrees_per_second = as_float(
        values, "smooth_turn_degrees_per_second",
        result.smooth_turn_degrees_per_second, 10.0f, 360.0f);
    result.dpad_shift = as_bool(values, "dpad_shift", result.dpad_shift);
    result.dpad_deadzone = as_float(
        values, "dpad_deadzone", result.dpad_deadzone, 0.1f, 0.95f);
    result.right_stick_down_crouch = as_bool(
        values, "right_stick_down_crouch", result.right_stick_down_crouch);
    result.menu_button_is_back = as_bool(
        values, "menu_button_is_back", result.menu_button_is_back);
    result.cutscene_comfort = as_bool(
        values, "cutscene_comfort", result.cutscene_comfort);
    result.logical_aim_follower = as_bool(
        values, "logical_aim_follower", result.logical_aim_follower);
    result.logical_aim_deadzone_degrees = as_float(
        values, "logical_aim_deadzone_degrees",
        result.logical_aim_deadzone_degrees, 0.0f, 10.0f);
    result.logical_aim_full_scale_degrees = as_float(
        values, "logical_aim_full_scale_degrees",
        result.logical_aim_full_scale_degrees, 0.5f, 45.0f);
    result.logical_aim_minimum_output = as_float(
        values, "logical_aim_minimum_output",
        result.logical_aim_minimum_output, 0.0f, 1.0f);
    result.reticle_distance_meters = as_float(
        values, "reticle_distance_meters", result.reticle_distance_meters,
        0.5f, 100.0f);
    result.reticle_extent_meters = as_float(
        values, "reticle_extent_meters", result.reticle_extent_meters,
        0.02f, 20.0f);
    result.controller_calibration_key = as_uint(
        values, "controller_calibration_key", result.controller_calibration_key);
    result.weapon_calibration_key = as_uint(
        values, "weapon_calibration_key", result.weapon_calibration_key);
    result.kill_switch_key = as_uint(
        values, "kill_switch_key", result.kill_switch_key);
    return result;
}

CalibrationState parse_calibration(std::string_view text) {
    CalibrationState result{};
    const auto values = parse_pairs(text);
    result.controller = read_offset(values, "controller");
    for (const auto& [key, unused] : values) {
        constexpr std::string_view prefix{"weapon."};
        if (!key.starts_with(prefix)) {
            continue;
        }
        const auto field = key.find('.', prefix.size());
        if (field == std::string::npos) {
            continue;
        }
        const auto weapon = key.substr(prefix.size(), field - prefix.size());
        if (!weapon.empty() && !result.weapons.contains(weapon)) {
            result.weapons.emplace(weapon, read_offset(values, "weapon." + weapon));
        }
    }
    return result;
}

std::string default_runtime_config_text() {
    return
        "# Halo Campaign Evolved UEVR - live settings (reloaded every 2 seconds)\n"
        "# Ctrl+Home also toggles enabled without taking off the headset.\n"
        "enabled=true\n"
        "floating_hands=true\n"
        "two_hand_hold=true\n"
        "dominant_hand=right\n\n"
        "head_relative_movement=true\n"
        "movement_deadzone=0.12\n"
        "turn_mode=snap\n"
        "snap_turn_degrees=30\n"
        "turn_deadzone=0.55\n"
        "smooth_turn_degrees_per_second=90\n"
        "dpad_shift=true\n"
        "dpad_deadzone=0.55\n"
        "right_stick_down_crouch=true\n"
        "menu_button_is_back=true\n\n"
        "cutscene_comfort=true\n"
        "# Optional low-bandwidth game-aim synchronization. Native projectile aim remains authoritative.\n"
        "logical_aim_follower=false\n"
        "logical_aim_deadzone_degrees=0.8\n"
        "logical_aim_full_scale_degrees=7.0\n"
        "logical_aim_minimum_output=0.26\n\n"
        "reticle_distance_meters=10.0\n"
        "reticle_extent_meters=2.5\n\n"
        "# Windows virtual-key values: End, PageDown, Home.\n"
        "controller_calibration_key=0x23\n"
        "weapon_calibration_key=0x22\n"
        "kill_switch_key=0x24\n";
}

std::string serialize_calibration(const CalibrationState& state) {
    std::ostringstream output{};
    output << std::setprecision(9)
           << "# Generated by HaloCEMotionControls. Values are metres and quaternions.\n";
    write_offset(output, "controller", state.controller);
    std::vector<std::string> keys{};
    keys.reserve(state.weapons.size());
    for (const auto& [key, unused] : state.weapons) {
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    for (const auto& key : keys) {
        output << '\n';
        write_offset(output, "weapon." + key, state.weapons.at(key));
    }
    return output.str();
}

bool ConfigStore::initialize() {
    std::array<wchar_t, 32768> appdata{};
    const auto length = GetEnvironmentVariableW(
        L"APPDATA", appdata.data(), static_cast<DWORD>(appdata.size()));
    auto root = length > 0 && length < appdata.size()
        ? std::filesystem::path{appdata.data()}
        : std::filesystem::current_path();
    root /= L"UnrealVRMod";
    root /= L"HaloCampaignEvolved";
    std::error_code error{};
    std::filesystem::create_directories(root, error);
    if (error) {
        return false;
    }
    m_config_path = root / L"halo_ce_vr.cfg";
    m_calibration_path = root / L"halo_ce_vr_calibration.cfg";
    if (!std::filesystem::exists(m_config_path)) {
        if (!write_file(m_config_path, default_runtime_config_text())) {
            return false;
        }
    }
    if (!std::filesystem::exists(m_calibration_path)) {
        if (!write_file(m_calibration_path, serialize_calibration({}))) {
            return false;
        }
    }
    return reload_if_changed(true);
}

bool ConfigStore::reload_if_changed(bool force) {
    std::error_code error{};
    const auto config_time = std::filesystem::last_write_time(m_config_path, error);
    if (error) {
        return false;
    }
    const auto calibration_time = std::filesystem::last_write_time(
        m_calibration_path, error);
    if (error) {
        return false;
    }
    bool changed = false;
    if (force || config_time != m_config_write_time) {
        m_runtime = parse_runtime_config(read_file(m_config_path));
        m_config_write_time = config_time;
        changed = true;
    }
    if (force || calibration_time != m_calibration_write_time) {
        m_calibration = parse_calibration(read_file(m_calibration_path));
        m_calibration_write_time = calibration_time;
        changed = true;
    }
    return changed;
}

bool ConfigStore::save_calibration() const {
    return write_file(m_calibration_path, serialize_calibration(m_calibration));
}

PoseOffset ConfigStore::weapon_calibration(std::string_view key) const {
    const auto found = m_calibration.weapons.find(std::string{key});
    return found == m_calibration.weapons.end() ? PoseOffset{} : found->second;
}

} // namespace halo_cevr
