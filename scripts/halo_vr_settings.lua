-- Halo Campaign Evolved VR - in-headset settings panel.
--
-- Draws the mod's halo_ce_vr.cfg settings inside UEVR's own overlay menu
-- (LuaLoader on_draw_ui), so hands/turning/reticle options can be toggled
-- without leaving the headset. Edits are written back to halo_ce_vr.cfg by
-- replacing the matching key=value line in place (comments and unknown keys
-- are preserved); the native plugin live-reloads that file within ~2 s.
--
-- No profile-library dependencies; stock LuaLoader APIs only.

-- LuaLoader sandboxes the os library (os.getenv is nil), so the profile
-- directory is derived from this script's own source path instead:
--   <profile>\scripts\halo_vr_settings.lua -> <profile>\halo_ce_vr.cfg
-- If the debug library is sandboxed too, fall back to probing relative
-- paths from the loader's working directory.
local function resolve_config_path()
    local ok, info = pcall(function()
        return debug.getinfo(1, "S")
    end)
    if ok and info ~= nil and type(info.source) == "string" then
        local path = info.source:gsub("^@", "")
        local dir = path:match("^(.*)[\\/][Ss]cripts[\\/][^\\/]+$")
        if dir ~= nil then
            return dir .. "\\halo_ce_vr.cfg"
        end
    end
    local candidates = {
        "..\\halo_ce_vr.cfg",
        "halo_ce_vr.cfg",
    }
    for _, candidate in ipairs(candidates) do
        local probe_ok, f = pcall(io.open, candidate, "rb")
        if probe_ok and f ~= nil then
            f:close()
            return candidate
        end
    end
    return nil
end

local CONFIG_PATH = nil
do
    local ok, resolved = pcall(resolve_config_path)
    if ok then CONFIG_PATH = resolved end
end

-- ---------------------------------------------------------------- config io

local function read_file(path)
    if path == nil or io == nil or io.open == nil then return nil end
    local ok, f = pcall(io.open, path, "rb")
    if not ok or f == nil then return nil end
    local text = f:read("*a")
    f:close()
    return text
end

local function write_file(path, text)
    if path == nil or io == nil or io.open == nil then return false end
    local ok, f = pcall(io.open, path, "wb")
    if not ok or f == nil then return false end
    f:write(text)
    f:close()
    return true
end

-- key=value lines; '#' comments. Returns both the parsed map and the raw
-- text so writes can edit lines in place.
local state = {
    raw = nil,
    values = {},
    status = "not loaded",
    dirty_note_until = 0.0,
}

local function parse_config()
    local raw = read_file(CONFIG_PATH)
    if raw == nil then
        state.status = CONFIG_PATH == nil
            and "config path unresolved (os and debug both sandboxed)"
            or ("halo_ce_vr.cfg not readable at " .. CONFIG_PATH)
        state.raw = nil
        state.values = {}
        return
    end
    local values = {}
    for line in raw:gmatch("[^\r\n]+") do
        local trimmed = line:match("^%s*(.-)%s*$")
        if trimmed ~= "" and trimmed:sub(1, 1) ~= "#" then
            local key, value = trimmed:match("^([%w_]+)%s*=%s*(.-)%s*$")
            if key ~= nil then values[key] = value end
        end
    end
    state.raw = raw
    state.values = values
    state.status = "loaded"
end

local function set_value(key, value)
    if state.raw == nil then return end
    local serialized = tostring(value)
    local replaced = false
    local pattern = "([^\r\n]*)"
    local out = {}
    for line in (state.raw .. "\n"):gmatch(pattern .. "\r?\n") do
        local trimmed = line:match("^%s*(.-)%s*$")
        local line_key = trimmed:match("^([%w_]+)%s*=")
        if line_key == key and trimmed:sub(1, 1) ~= "#" then
            out[#out + 1] = key .. "=" .. serialized
            replaced = true
        else
            out[#out + 1] = line
        end
    end
    -- Drop the trailing empty element the sentinel newline produces.
    if out[#out] == "" then out[#out] = nil end
    if not replaced then
        out[#out + 1] = key .. "=" .. serialized
    end
    local text = table.concat(out, "\n") .. "\n"
    if write_file(CONFIG_PATH, text) then
        state.raw = text
        state.values[key] = serialized
        state.status = "saved " .. key .. " (applies in ~2 s)"
    else
        state.status = "WRITE FAILED: " .. CONFIG_PATH
    end
end

local function get_bool(key, default)
    local v = state.values[key]
    if v == nil then return default end
    v = v:lower()
    return v == "true" or v == "1" or v == "yes" or v == "on"
end

local function get_number(key, default)
    local v = tonumber(state.values[key])
    if v == nil then return default end
    return v
end

local function get_string(key, default)
    local v = state.values[key]
    if v == nil or v == "" then return default end
    return v
end

do
    local ok, err = pcall(parse_config)
    if not ok then
        state.status = "load error: " .. tostring(err)
        pcall(print, "[halo_vr_settings] load error: " .. tostring(err))
    end
end

-- ---------------------------------------------------------------- ui

local function draw_checkbox(label, key, default)
    local changed, value = imgui.checkbox(label, get_bool(key, default))
    if changed then set_value(key, value and "true" or "false") end
end

local function draw_slider(label, key, default, minimum, maximum, format)
    local changed, value = imgui.slider_float(
        label, get_number(key, default), minimum, maximum, format or "%.2f")
    if changed then
        set_value(key, string.format("%.3f", value))
    end
end

local function draw_combo(label, key, default, options)
    local current = get_string(key, default)
    local index = 1
    for i, option in ipairs(options) do
        if option == current then index = i end
    end
    local changed, new_index = imgui.combo(label, index, options)
    if changed and options[new_index] ~= nil then
        set_value(key, options[new_index])
    end
end

local function draw_panel()
    imgui.text("Halo Campaign Evolved VR")
    imgui.text("Status: " .. state.status)
    if imgui.button("Reload from file") then
        parse_config()
    end

    if state.raw == nil then return end

    imgui.text("")
    imgui.text("-- Core --")
    draw_checkbox("Motion controls enabled", "enabled", true)
    draw_checkbox(
        "Floating hands (off = anchored arm IK / VRIK)",
        "floating_hands", true)
    draw_checkbox("Two-handed hold", "two_hand_hold", true)
    draw_combo("Dominant hand", "dominant_hand", "right",
               { "right", "left" })
    draw_checkbox("Cutscene comfort (flat 2D cutscenes)",
                  "cutscene_comfort", true)
    draw_checkbox("Prevent controller sleep",
                  "prevent_controller_sleep", true)

    imgui.text("")
    imgui.text("-- Movement and turning --")
    draw_checkbox("Head-relative movement", "head_relative_movement", true)
    draw_slider("Movement deadzone", "movement_deadzone", 0.12, 0.0, 0.5)
    draw_combo("Turn mode", "turn_mode", "snap",
               { "off", "snap", "smooth" })
    draw_slider("Snap turn degrees", "snap_turn_degrees", 30.0, 15.0, 90.0,
                "%.0f")
    draw_slider("Smooth turn deg/s", "smooth_turn_degrees_per_second",
                90.0, 30.0, 360.0, "%.0f")
    draw_slider("Turn stick deadzone", "turn_deadzone", 0.55, 0.1, 0.95)
    draw_checkbox("D-pad shifting (right stick up)", "dpad_shift", true)
    draw_checkbox("Crouch on right stick click",
                  "right_stick_down_crouch", true)
    draw_checkbox("Menu button acts as Back", "menu_button_is_back", true)

    imgui.text("")
    imgui.text("-- Reticle --")
    draw_slider("Reticle distance (m)", "reticle_distance_meters",
                10.0, 3.0, 50.0, "%.0f")
    draw_slider("Reticle size (m)", "reticle_extent_meters",
                2.5, 0.5, 10.0)

    imgui.text("")
    imgui.text("-- Support hand trim (degrees) --")
    draw_slider("Left hand pitch", "left_hand_pitch_degrees",
                0.0, -180.0, 180.0, "%.0f")
    draw_slider("Left hand yaw", "left_hand_yaw_degrees",
                0.0, -180.0, 180.0, "%.0f")
    draw_slider("Left hand roll", "left_hand_roll_degrees",
                0.0, -180.0, 180.0, "%.0f")

    imgui.text("")
    imgui.text("-- Advanced --")
    draw_checkbox("Waypoints anchored in world (HUD head-compensation)",
                  "hud_waypoint_fix", true)
    draw_checkbox("Logical aim follower (game aim tracks controller)",
                  "logical_aim_follower", false)
end

-- A missing binding or parse problem must not kill the script: show the
-- reason in the menu AND print it once to the UEVR log, so it can be
-- diagnosed from log.txt without the headset on.
local reported = {}
local function report_error(err)
    local text = tostring(err)
    if not reported[text] then
        reported[text] = true
        pcall(print, "[halo_vr_settings] draw error: " .. text)
    end
    pcall(function()
        imgui.text("halo_vr_settings error: " .. text)
    end)
end

local registered, register_error = pcall(function()
    uevr.sdk.callbacks.on_draw_ui(function()
        local ok, err = pcall(draw_panel)
        if not ok then report_error(err) end
    end)
end)
if not registered then
    pcall(print, "[halo_vr_settings] failed to register on_draw_ui: "
        .. tostring(register_error))
end
