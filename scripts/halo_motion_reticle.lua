-- Halo Campaign Evolved: minimal stereo world-space controller reticle.
--
-- Keep this script dependency-free. The general UEVR profile libraries
-- register an XInput callback that is not compatible with this UEVR build.

local api = uevr.api
local reticle = nil
local reticle_light = nil
local anchor = nil
local retry_frames = 0
local pose_frames = 0
local last_error = nil
local last_release_reason = nil

-- UObjectHook.exists() only consults objects observed after activation.
-- Activate before creating either dynamic component so the constructor and
-- destructor hooks can track their lifetime across menu/save transitions.
UEVR_UObjectHook.activate()

local marker_path = "halo_motion_reticle.active"
local diagnostic_path = "halo_motion_reticle.error"
local gameplay_path = "halo_motion_gameplay.active"
local pose_path = "halo_motion_reticle.pose"

local function publish_diagnostic(message)
    fs.write(diagnostic_path, tostring(message) .. "\n")
end

local function publish_ready(ready)
    fs.write(
        marker_path,
        ready and "right-controller world reticle ready\n" or "inactive\n")
    if ready then
        fs.write(diagnostic_path, "")
    end
end

local function valid(object)
    return object ~= nil and UEVR_UObjectHook.exists(object)
end

local function release_reticle(reason)
    local old_anchor = anchor
    local old_reticle = reticle
    local old_reticle_light = reticle_light
    anchor = nil
    reticle = nil
    reticle_light = nil

    -- Remove the attachment before dropping the Lua references. This matters
    -- when only the child mesh disappears: otherwise UObjectHook continues
    -- ticking an invisible, orphaned anchor until its owner is destroyed.
    if valid(old_anchor) then
        UEVR_UObjectHook.remove_motion_controller_state(old_anchor)
    end

    -- Do not destroy either component here. The Unreal shell may already be
    -- tearing down its world on this callback; hiding the still-valid child
    -- and letting its AActor own destruction avoids racing that teardown.
    if valid(old_reticle) then
        pcall(function()
            old_reticle:SetVisibility(false)
            old_reticle:SetHiddenInGame(true)
        end)
    end
    if valid(old_reticle_light) then
        pcall(function()
            old_reticle_light:SetVisibility(false)
            old_reticle_light:SetHiddenInGame(true)
        end)
    end

    publish_ready(false)
    if reason ~= nil and reason ~= last_release_reason then
        publish_diagnostic("released: " .. tostring(reason))
        print("[Halo motion reticle] released: " .. tostring(reason))
        last_release_reason = reason
    end
end

local function create_reticle()
    if fs.read(gameplay_path):sub(1, 5) ~= "ready" then
        return false
    end

    -- Prefer the visible local pawn as the render owner. PlayerController is
    -- persistent, but Halo marks it hidden and that owner flag suppresses the
    -- StaticMeshComponent's scene proxy even when the component itself is
    -- visible. The lifetime checks below safely recreate the pawn-owned
    -- components after a menu or save transition.
    local owner = api:get_local_pawn(0)
    if owner == nil then
        owner = api:get_player_controller(0)
    end
    if owner == nil then
        return false
    end

    local scene_class =
        api:find_uobject("Class /Script/Engine.SceneComponent")
    local mesh_class =
        api:find_uobject("Class /Script/Engine.StaticMeshComponent")
    local point_light_class =
        api:find_uobject("Class /Script/Engine.PointLightComponent")
    local sphere =
        api:find_uobject(
            "StaticMesh /Engine/EngineMeshes/Sphere.Sphere")
    local material =
        api:find_uobject(
            "Material /Engine/EngineMaterials/" ..
            "DefaultSpriteMaterial.DefaultSpriteMaterial")
    if scene_class == nil or mesh_class == nil or
        point_light_class == nil or sphere == nil then
        return false
    end

    anchor = api:add_component_by_class(owner, scene_class, false)
    reticle = api:add_component_by_class(owner, mesh_class, false)
    reticle_light =
        api:add_component_by_class(owner, point_light_class, false)
    if anchor == nil or reticle == nil or reticle_light == nil then
        anchor = nil
        reticle = nil
        reticle_light = nil
        return false
    end

    local controller_state =
        UEVR_UObjectHook.get_or_add_motion_controller_state(anchor)
    if controller_state == nil then
        anchor = nil
        reticle = nil
        reticle_light = nil
        return false
    end

    controller_state:set_hand(1)
    -- "Permanent" means keep the controller transform instead of enqueueing
    -- restoration to the component's old transform after each stereo update;
    -- it does not make the UObject or state survive destruction. UObjectHook's
    -- destructor hook removes the state when this synthetic anchor is freed.
    controller_state:set_permanent(true)

    reticle:SetStaticMesh(sphere)
    if material ~= nil then
        -- DefaultSpriteMaterial is unlit, so the controller marker remains
        -- visible in Halo's very dark interiors and night scenes.
        reticle:SetMaterial(0, material)
    end
    reticle:SetCollisionEnabled(0, false)
    reticle:SetVisibility(true)
    reticle:SetHiddenInGame(false)
    reticle.BoundsScale = 10.0

    -- DefaultSpriteMaterial is not guaranteed to have been loaded into the
    -- game's UObject array when this standalone script starts. Keep a tiny
    -- white point light just behind the sphere as a dependency-free fallback
    -- so the stock lit sphere is still unmistakable in Halo's dark scenes.
    -- Its 40 cm radius is intentionally local to the marker.
    reticle_light:SetIntensity(5000.0)
    reticle_light:SetAttenuationRadius(40.0)
    reticle_light:SetVisibility(true)
    reticle_light:SetHiddenInGame(false)

    -- UEVR's right-hand anchor uses Unreal's +X forward axis. Set the
    -- relative transform before attachment so K2_AttachTo performs Unreal's
    -- normal component-to-world update instead of relying on raw property
    -- writes to dirty a transform that is already attached.
    -- Keep the marker beyond Halo's unusually long/large first-person
    -- viewmodel. At two metres the controller ray was correct, but the marker
    -- sat between the hand and the rendered sniper muzzle and looked
    -- misaligned because of parallax. Ten metres puts it beyond every tested
    -- weapon while remaining near enough to survive world transitions.
    reticle.RelativeLocation = Vector3d.new(1000.0, 0.0, 0.0)
    reticle_light.RelativeLocation = Vector3d.new(990.0, 0.0, 0.0)
    -- Preserve the original angular size: the engine sphere has a 50 cm
    -- radius, so 0.10 gives a 5 cm radius marker at ten metres.
    reticle.RelativeScale3D = Vector3d.new(0.10, 0.10, 0.10)
    reticle:K2_AttachTo(anchor, "", 0, false)
    reticle_light:K2_AttachTo(anchor, "", 0, false)

    publish_ready(true)
    last_release_reason = nil
    print("[Halo motion reticle] right-controller reticle created")
    return true
end

publish_ready(false)
fs.write(diagnostic_path, "")
print("[Halo motion reticle] loaded (dependency-free)")

local function attempt_create()
    local ok, result = pcall(create_reticle)
    if ok and result then
        last_error = nil
        return true
    end

    local diagnostic =
        ok and "required Unreal objects are not ready" or tostring(result)
    if diagnostic ~= last_error then
        print("[Halo motion reticle] waiting: " .. diagnostic)
        publish_diagnostic(diagnostic)
        last_error = diagnostic
    end
    return false
end

uevr.sdk.callbacks.on_pre_viewport_client_draw(
    function(viewport_client, viewport, canvas)
    if fs.read(gameplay_path):sub(1, 5) ~= "ready" then
        if anchor ~= nil or reticle ~= nil or reticle_light ~= nil then
            release_reticle("gameplay inactive")
        end
        return
    end

    if valid(anchor) and valid(reticle) and valid(reticle_light) then
        pose_frames = pose_frames - 1
        if pose_frames <= 0 then
            pose_frames = 60
            pcall(function()
                local anchor_location = anchor:K2_GetComponentLocation()
                local anchor_rotation = anchor:K2_GetComponentRotation()
                local reticle_location = reticle:K2_GetComponentLocation()
                fs.write(
                    pose_path,
                    string.format(
                        "anchor %.6f %.6f %.6f rot %.6f %.6f %.6f\n" ..
                        "reticle %.6f %.6f %.6f\n",
                        anchor_location.X,
                        anchor_location.Y,
                        anchor_location.Z,
                        anchor_rotation.Pitch,
                        anchor_rotation.Yaw,
                        anchor_rotation.Roll,
                        reticle_location.X,
                        reticle_location.Y,
                        reticle_location.Z))
            end)
        end
        return
    end

    if anchor ~= nil or reticle ~= nil or reticle_light ~= nil then
        local anchor_state = valid(anchor) and "valid" or "invalid"
        local reticle_state = valid(reticle) and "valid" or "invalid"
        local light_state =
            valid(reticle_light) and "valid" or "invalid"
        release_reticle(
            "component lifetime lost (anchor=" .. anchor_state ..
            ", reticle=" .. reticle_state ..
            ", light=" .. light_state .. ")")
    else
        publish_ready(false)
    end

    retry_frames = retry_frames - 1
    if retry_frames > 0 then
        return
    end
    retry_frames = 60

    attempt_create()
end)
