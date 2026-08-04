-- Halo: Campaign Evolved - first-person projection scale fix
-- Vanilla UEVR edition: only stock LuaLoader APIs are used.
--
-- Install this file directly in:
--   %APPDATA%\UnrealVRMod\HaloCampaignEvolved\scripts\
--
-- Halo's UE 5.5 camera enables the engine's first-person primitive scale with
-- FirstPersonScale=0.15. Disabling bEnableFirstPersonScale makes
-- UCameraComponent::GetCameraView use the engine default scale of 1.0.

local CHECK_INTERVAL_SECONDS = 0.5
local REMOVE_FIRST_PERSON_FOV = true

local elapsed_seconds = CHECK_INTERVAL_SECONDS
local last_camera_address = 0
local last_error = nil
local camera_component_class = nil
local current_stage = "startup"

local function get_camera()
    current_stage = "get local pawn"
    local pawn = uevr.api:get_local_pawn(0)
    if pawn == nil then
        last_camera_address = 0
        return nil
    end

    -- This is the same stock-UEVR component lookup pattern used by the
    -- upstream hello_world.lua example. It avoids depending on the inherited
    -- BP_MeteoritePawn_C CameraComponent property lookup.
    if camera_component_class == nil then
        current_stage = "find CameraComponent class"
        camera_component_class =
            uevr.api:find_uobject("Class /Script/Engine.CameraComponent")
    end

    if camera_component_class == nil then
        return nil
    end

    current_stage = "GetComponentByClass"
    return pawn:GetComponentByClass(camera_component_class)
end

local function apply_if_needed()
    local camera = get_camera()
    if camera == nil then
        return false
    end

    current_stage = "read camera address"
    local camera_address = camera:get_address()
    local camera_changed = camera_address ~= last_camera_address
    current_stage = "read bEnableFirstPersonScale"
    local scale_enabled = camera:get_property("bEnableFirstPersonScale")

    -- Do not rewrite properties continuously. Apply once for each new camera,
    -- then only if game logic explicitly re-enables the projection scale.
    if camera_changed or scale_enabled then
        current_stage = "read FirstPersonScale"
        local old_scale = camera:get_property("FirstPersonScale")

        current_stage = "write FirstPersonScale"
        camera:set_property("FirstPersonScale", 1.0)
        current_stage = "write bEnableFirstPersonScale"
        camera:set_property("bEnableFirstPersonScale", false)

        print(string.format(
            "[Halo FP projection UEVR-only] camera=0x%X scale=%s enabled=%s -> 1.0/false",
            camera_address,
            tostring(old_scale),
            tostring(scale_enabled)
        ))
    end

    if REMOVE_FIRST_PERSON_FOV then
        current_stage = "read bEnableFirstPersonFieldOfView"
        local fov_enabled =
            camera:get_property("bEnableFirstPersonFieldOfView")

        if camera_changed or fov_enabled then
            current_stage = "read FieldOfView"
            local world_fov = camera:get_property("FieldOfView")
            current_stage = "write FirstPersonFieldOfView"
            camera:set_property("FirstPersonFieldOfView", world_fov)
            current_stage = "write bEnableFirstPersonFieldOfView"
            camera:set_property("bEnableFirstPersonFieldOfView", false)
        end
    end

    last_camera_address = camera_address
    current_stage = "idle"
    return true
end

local function guarded_apply()
    local ok, err = pcall(apply_if_needed)
    if ok then
        if err then
            last_error = nil
        elseif current_stage ~= last_error then
            print("[Halo FP projection UEVR-only] waiting at " .. current_stage)
            last_error = current_stage
        end
    else
        local diagnostic = current_stage .. ": " .. tostring(err)
        if diagnostic ~= last_error then
            print("[Halo FP projection UEVR-only] " .. diagnostic)
            last_error = diagnostic
        end
    end
end

print("[Halo FP projection UEVR-only] loaded")
guarded_apply()

uevr.sdk.callbacks.on_pre_engine_tick(function(engine, delta)
    elapsed_seconds = elapsed_seconds + delta
    if elapsed_seconds < CHECK_INTERVAL_SECONDS then
        return
    end

    elapsed_seconds = 0.0
    guarded_apply()
end)
