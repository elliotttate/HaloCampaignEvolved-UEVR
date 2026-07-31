-- Halo Campaign Evolved: minimal stereo world-space controller reticle.
--
-- Keep this script dependency-free. The general UEVR profile libraries
-- register an XInput callback that is not compatible with this UEVR build.

local previous_runtime = rawget(_G, "HaloMotionReticleRuntime")
if previous_runtime ~= nil and previous_runtime.cleanup ~= nil then
    pcall(previous_runtime.cleanup)
end
local runtime = { active = true }
_G.HaloMotionReticleRuntime = runtime

local api = uevr.api
local reticle = nil
local reticle_light = nil
local world_reticle = nil
local world_reticle_widget = nil
local world_reticle_parent = nil
local screen_reticle_image = nil
local world_reticle_image = nil
local screen_reticle_original_opacity = 1.0
local screen_reticle_suppressed = false
local anchor = nil
local retry_frames = 0
local pose_frames = 0
local last_error = nil
local last_release_reason = nil
local proof_frames = 0
local restore_widget_to_screen = nil

-- UObjectHook.exists() only consults objects observed after activation.
-- Activate before creating either dynamic component so the constructor and
-- destructor hooks can track their lifetime across menu/save transitions.
UEVR_UObjectHook.activate()

local marker_path = "halo_motion_reticle.active"
local diagnostic_path = "halo_motion_reticle.error"
local gameplay_path = "halo_motion_gameplay.active"
local pose_path = "halo_motion_reticle.pose"
local widget_path = "halo_motion_reticle.widget"
-- Written by the C++ plugin on transitions. The ball hides whenever its ray
-- would mislead: during a two-handed hold (shots follow the hand-to-hand
-- line instead of the right controller ray the ball is anchored to) and
-- while zoomed (scoped shots keep Halo's stock screen-center aim).
local hide_path = "halo_motion_reticle_hide.active"
local reticle_hidden = false
local marker_poll_frames = 0
local gameplay_ready = false
local replacement_hidden = false

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

local function live_in_object_array(target)
    if target == nil then
        return false
    end

    local objects = uevr.types.FUObjectArray.get()
    if objects == nil then
        return false
    end
    for index = objects:get_object_count() - 1, 0, -1 do
        if objects:get_object(index) == target then
            return true
        end
    end
    return false
end

local function set_reticle_image_opacity(image, opacity)
    if not live_in_object_array(image) then
        return false
    end

    return pcall(function()
        if image.SetRenderOpacity ~= nil then
            image:SetRenderOpacity(opacity)
        elseif image.call ~= nil then
            -- HaloUILazyImage does not expose inherited UWidget methods as
            -- direct Lua members in this build. UObject:call resolves the
            -- inherited UFunction and executes it through ProcessEvent,
            -- which also invalidates the cached Slate widget.
            image:call("SetRenderOpacity", opacity)
        else
            image.RenderOpacity = opacity
        end
    end)
end

local function restore_screen_reticle_image()
    if screen_reticle_suppressed and
        live_in_object_array(screen_reticle_image) then
        set_reticle_image_opacity(
            screen_reticle_image,
            screen_reticle_original_opacity)
    end
    screen_reticle_suppressed = false
end

local function release_reticle(reason)
    local old_anchor = anchor
    local old_reticle = reticle
    local old_reticle_light = reticle_light
    local old_world_reticle = world_reticle
    local old_world_reticle_widget = world_reticle_widget
    local old_world_reticle_parent = world_reticle_parent
    restore_screen_reticle_image()
    anchor = nil
    reticle = nil
    reticle_light = nil
    world_reticle = nil
    world_reticle_widget = nil
    world_reticle_parent = nil
    screen_reticle_image = nil
    world_reticle_image = nil
    screen_reticle_original_opacity = 1.0
    proof_frames = 0

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
    if live_in_object_array(old_world_reticle) then
        pcall(function()
            old_world_reticle:SetVisibility(false)
            old_world_reticle:SetHiddenInGame(true)
            old_world_reticle:SetWidget(nil)
        end)
    end
    if live_in_object_array(old_world_reticle_widget) and
        live_in_object_array(old_world_reticle_parent) and
        restore_widget_to_screen ~= nil then
        restore_widget_to_screen(
            old_world_reticle_widget,
            old_world_reticle_parent)
    end

    publish_ready(false)
    fs.write(widget_path, "inactive\n")
    if reason ~= nil and reason ~= last_release_reason then
        publish_diagnostic("released: " .. tostring(reason))
        print("[Halo motion reticle] released: " .. tostring(reason))
        last_release_reason = reason
    end
end

-- UEVR keeps registered frame callbacks across a script hot reload. Retire the
-- old callback and restore its hosted HUD widget before the replacement script
-- creates another anchor. This also makes iterative profile testing safe.
runtime.cleanup = function()
    if runtime.active then
        runtime.active = false
        release_reticle("hot reload")
    end
end

local function new_vector2(x, y)
    local vector_class =
        api:find_uobject("ScriptStruct /Script/CoreUObject.Vector2D")
    if vector_class == nil then
        return nil
    end

    local vector = StructObject.new(vector_class)
    vector.X = x
    vector.Y = y
    return vector
end

local function new_identity_transform()
    local transform_class =
        api:find_uobject(
            "ScriptStruct /Script/CoreUObject.Transform")
    if transform_class == nil then
        return nil
    end

    local transform = StructObject.new(transform_class)
    transform.Rotation.X = 0.0
    transform.Rotation.Y = 0.0
    transform.Rotation.Z = 0.0
    transform.Rotation.W = 1.0
    transform.Translation.X = 0.0
    transform.Translation.Y = 0.0
    transform.Translation.Z = 0.0
    transform.Scale3D.X = 1.0
    transform.Scale3D.Y = 1.0
    transform.Scale3D.Z = 1.0
    return transform
end

local function find_live_first_person_reticle()
    local target_class =
        api:find_uobject(
            "WidgetBlueprintGeneratedClass " ..
            "/Game/UI/Hud/Reticle/WBP_FirstPersonReticle." ..
            "WBP_FirstPersonReticle_C")
    local objects = uevr.types.FUObjectArray.get()
    if target_class == nil or objects == nil then
        return nil
    end

    -- Halo constructs WBP_HUD_Main before the profile scripts run, so the
    -- authoritative GUObjectArray is required; UObjectHook's observed-object
    -- table does not contain this pre-existing widget.
    for index = objects:get_object_count() - 1, 0, -1 do
        local object = objects:get_object(index)
        if object ~= nil and object:is_a(target_class) then
            local full_name = object:get_full_name()
            if string.find(
                    full_name,
                    "WBP_FirstPersonReticle_C /Engine/Transient.",
                    1,
                    true) ~= nil and
                string.find(
                    full_name,
                    ".WBP_HUD_Main_C_",
                    1,
                    true) ~= nil then
                local has_parent = false
                if object.GetParent ~= nil then
                    pcall(function()
                        has_parent = object:GetParent() ~= nil
                    end)
                end
                if has_parent then
                    return object
                end
            end
        end
    end
    return nil
end

local function find_reticle_image(owner_widget)
    if owner_widget == nil then
        return nil
    end

    local image_class =
        api:find_uobject("Class /Script/UMG.Image")
    local objects = uevr.types.FUObjectArray.get()
    if image_class == nil or objects == nil then
        return nil
    end

    local owner_path =
        string.match(owner_widget:get_full_name(), "^[^ ]+ (.+)$")
    if owner_path == nil then
        return nil
    end
    local image_prefix = owner_path .. ".WidgetTree_"
    for index = objects:get_object_count() - 1, 0, -1 do
        local object = objects:get_object(index)
        if object ~= nil and object:is_a(image_class) then
            local object_path =
                string.match(object:get_full_name(), "^[^ ]+ (.+)$")
            if object_path ~= nil and
                string.find(object_path, image_prefix, 1, true) == 1 and
                string.sub(object_path, -14) == ".Reticle_Image" then
                return object
            end
        end
    end
    return nil
end

local function set_screen_reticle_suppressed(suppressed)
    if screen_reticle_suppressed == suppressed then
        return true
    end
    if not live_in_object_array(screen_reticle_image) then
        return false
    end
    local opacity =
        suppressed and 0.0 or screen_reticle_original_opacity
    if not set_reticle_image_opacity(screen_reticle_image, opacity) then
        return false
    end
    screen_reticle_suppressed = suppressed
    return true
end

restore_widget_to_screen = function(widget, parent)
    if widget == nil then
        return
    end

    if parent ~= nil and parent.AddChild ~= nil then
        local ok = pcall(function()
            local slot = parent:AddChild(widget)
            if slot ~= nil then
                if slot.SetHorizontalAlignment ~= nil then
                    slot:SetHorizontalAlignment(2)
                else
                    slot.HorizontalAlignment = 2
                end
                if slot.SetVerticalAlignment ~= nil then
                    slot:SetVerticalAlignment(2)
                else
                    slot.VerticalAlignment = 2
                end
            end
        end)
        if ok then
            return
        end
    end

    if widget.AddToPlayerScreen ~= nil then
        pcall(function()
            widget:AddToPlayerScreen(100)
        end)
    elseif widget.AddToViewport ~= nil then
        pcall(function()
            widget:AddToViewport(100)
        end)
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
    local widget_component_class =
        api:find_uobject("Class /Script/UMG.WidgetComponent")
    local sphere =
        api:find_uobject(
            "StaticMesh /Engine/EngineMeshes/Sphere.Sphere")
    local material =
        api:find_uobject(
            "Material /Engine/EngineMaterials/" ..
            "DefaultSpriteMaterial.DefaultSpriteMaterial")
    local authored_reticle = find_live_first_person_reticle()
    local component_transform = new_identity_transform()
    if scene_class == nil or mesh_class == nil or
        point_light_class == nil or widget_component_class == nil or
        sphere == nil or authored_reticle == nil or
        component_transform == nil then
        return false
    end

    anchor = api:add_component_by_class(owner, scene_class, false)
    reticle = api:add_component_by_class(owner, mesh_class, false)
    reticle_light =
        api:add_component_by_class(owner, point_light_class, false)
    -- Defer registration so WidgetClass is populated before
    -- UWidgetComponent::OnRegister calls InitWidget. SetWidget cannot safely
    -- share Halo's already-parented live HUD instance: RemoveFromParent
    -- invalidates that nested widget and leaves the component with a stale
    -- UObject pointer. A component-owned instance is the canonical Unreal
    -- ownership model and still runs the exact WBP_FirstPersonReticle_C
    -- Blueprint, including its weapon-specific dynamic material and events.
    world_reticle =
        api:add_component_by_class(owner, widget_component_class, true)
    if anchor == nil or reticle == nil or reticle_light == nil or
        world_reticle == nil then
        release_reticle("component creation incomplete")
        return false
    end

    local player_controller = api:get_player_controller(0)
    local local_player = nil
    if player_controller ~= nil then
        pcall(function()
            local_player = player_controller.Player
        end)
    end

    local widget_registered = pcall(function()
        world_reticle.WidgetClass = authored_reticle:get_class()
        -- BlendMode selects the Widget3D material during OnRegister, so it
        -- must be set before FinishAddComponent. The authored cyan reticle
        -- has translucent pixels that a default masked material can clip.
        world_reticle.BlendMode = 2
        if local_player ~= nil then
            if world_reticle.SetOwnerPlayer ~= nil then
                world_reticle:SetOwnerPlayer(local_player)
            elseif world_reticle.call ~= nil then
                world_reticle:call("SetOwnerPlayer", local_player)
            end
        end
        owner:FinishAddComponent(
            world_reticle,
            false,
            component_transform)
    end)
    if not widget_registered then
        release_reticle("component-owned UMG registration failed")
        return false
    end

    local owned_reticle = nil
    pcall(function()
        owned_reticle = world_reticle.Widget
    end)
    -- Blueprint-created widget-tree objects are not guaranteed to pass
    -- UObjectHook.exists(): that table tracks observed constructors, not the
    -- authoritative live object set. GUObjectArray membership is the correct
    -- lifetime proof for the component-owned UserWidget and its children.
    if not live_in_object_array(owned_reticle) then
        release_reticle("component-owned UMG instance unavailable")
        return false
    end

    -- A component-created UserWidget has no owning player by default.
    -- WBP_FirstPersonReticle's Construct therefore falls back to its
    -- NonCombat state and never binds the weapon presenter. Supply the same
    -- player context as the live HUD, then rerun the Blueprint's public
    -- reticle initialization entry point.
    local initialized = pcall(function()
        if player_controller ~= nil and owned_reticle.call ~= nil then
            owned_reticle:call(
                "SetOwningPlayer",
                player_controller)
            owned_reticle:call("SetNewReticle")
            owned_reticle:call("ApplyAllCrosshairSettings")
            owned_reticle:call("ForceLayoutPrepass")
        end
    end)
    if not initialized then
        release_reticle("component-owned reticle initialization failed")
        return false
    end

    screen_reticle_image = find_reticle_image(authored_reticle)
    world_reticle_image = find_reticle_image(owned_reticle)
    if not live_in_object_array(screen_reticle_image) or
        not live_in_object_array(world_reticle_image) then
        release_reticle("authored reticle image discovery failed")
        return false
    end
    pcall(function()
        if screen_reticle_image.GetRenderOpacity ~= nil then
            screen_reticle_original_opacity =
                screen_reticle_image:GetRenderOpacity()
        elseif screen_reticle_image.call ~= nil then
            screen_reticle_original_opacity =
                screen_reticle_image:call("GetRenderOpacity")
        elseif screen_reticle_image.RenderOpacity ~= nil then
            screen_reticle_original_opacity =
                screen_reticle_image.RenderOpacity
        end
    end)
    if screen_reticle_original_opacity == nil then
        screen_reticle_original_opacity = 1.0
    end

    local controller_state =
        UEVR_UObjectHook.get_or_add_motion_controller_state(anchor)
    if controller_state == nil then
        release_reticle("controller state unavailable")
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
    -- This mesh and light were useful while proving the controller ray, but
    -- they must remain hidden now that the authored UMG ring owns the marker.
    -- Leaving the sphere enabled can obscure the cyan Slate texture and make
    -- the result look like a grey cylinder in stereo.
    reticle:SetVisibility(false)
    reticle:SetHiddenInGame(true)
    reticle.BoundsScale = 10.0

    -- DefaultSpriteMaterial is not guaranteed to have been loaded into the
    -- game's UObject array when this standalone script starts. Keep a tiny
    -- white point light just behind the sphere as a dependency-free fallback
    -- so the stock lit sphere is still unmistakable in Halo's dark scenes.
    -- Its 40 cm radius is intentionally local to the marker.
    reticle_light:SetIntensity(0.0)
    reticle_light:SetAttenuationRadius(40.0)
    reticle_light:SetVisibility(false)
    reticle_light:SetHiddenInGame(true)

    -- The synthetic controller anchor's local +X is the forward scene ray.
    -- Live verification showed that its world rotation exactly equals the
    -- Unreal camera rotation at the neutral controller pose; -X therefore
    -- placed the widget ten metres behind the camera. Halo's Blam weapon
    -- matrix uses node 8's `left` axis for its rendered barrel/projectiles,
    -- but that native convention must not be applied to this Unreal component.
    -- Set the relative transform before attachment so K2_AttachTo performs
    -- Unreal's normal component-to-world update instead of relying on raw
    -- property writes to dirty a transform that is already attached.
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

    -- Render a second, component-owned instance of Halo's exact reticle
    -- Blueprint. The native CHUD hook hides only the original screen-space
    -- copy after publish_ready(true), leaving this world-space instance.
    local pivot = new_vector2(0.5, 0.5)
    -- Reticle_Image is center-anchored. The native plugin trims this clone's
    -- overlay to that exact 250x250 image and calls the reflected
    -- SetDrawSize(FVector2D) setter. Do not write DrawSize from Lua:
    -- WidgetComponent stores the backing value as FIntPoint even though the
    -- UE 5.6 reflected setter takes FVector2D, and confusing the two structs
    -- silently zeroes the render target in this build.
    local configured = pcall(function()
        if world_reticle.SetDrawAtDesiredSize ~= nil then
            -- WBP_FirstPersonReticle is canvas-rooted; relying on desired size
            -- can produce a zero-sized render target. Its reticle is
            -- center-anchored, so use an explicit cropped virtual window.
            world_reticle:SetDrawAtDesiredSize(false)
        elseif world_reticle.call ~= nil then
            world_reticle:call("SetDrawAtDesiredSize", false)
        end
        if pivot ~= nil and world_reticle.SetPivot ~= nil then
            world_reticle:SetPivot(pivot)
        end
        if world_reticle.SetWidgetSpace ~= nil then
            world_reticle:SetWidgetSpace(0)
        else
            world_reticle.Space = 0
        end
        if world_reticle.SetTwoSided ~= nil then
            -- Configure this before registration/material creation. Toggling
            -- it later leaves WidgetComponent's existing MID parented to the
            -- two-sided pass material, whose back-face branch replaces the
            -- SlateUI RGB with BackColor. The native transform points the
            -- authored -X front face at the viewer, so a one-sided pass keeps
            -- the live cyan/red reticle texture and removes that ambiguity.
            world_reticle:SetTwoSided(false)
        end
        if world_reticle.SetTickWhenOffscreen ~= nil then
            world_reticle:SetTickWhenOffscreen(false)
        end
        if world_reticle.SetManuallyRedraw ~= nil then
            world_reticle:SetManuallyRedraw(false)
        elseif world_reticle.call ~= nil then
            world_reticle:call("SetManuallyRedraw", false)
        end
        -- Reticle material parameters (enemy/friendly color and recoil)
        -- remain live, but repainting this 250x250 Slate tree at the full
        -- engine tick rate is unnecessary. Transform tracking still updates
        -- natively every tick; only the WidgetComponent render target is
        -- capped at 30 Hz.
        if world_reticle.SetRedrawTime ~= nil then
            world_reticle:SetRedrawTime(1.0 / 30.0)
        elseif world_reticle.call ~= nil then
            world_reticle:call("SetRedrawTime", 1.0 / 30.0)
        end
        world_reticle.BlendMode = 2
        world_reticle.BoundsScale = 100.0
        world_reticle:SetCollisionEnabled(0, false)
        world_reticle:SetVisibility(true)
        world_reticle:SetHiddenInGame(false)
        world_reticle.RelativeLocation =
            Vector3d.new(1000.0, 0.0, 0.0)
        -- The controller and HMD are behind the ten-metre target; face the
        -- WidgetComponent back toward them while inheriting all aim rotation.
        local relative_rotation = world_reticle.RelativeRotation
        relative_rotation.Pitch = 0.0
        relative_rotation.Yaw = 180.0
        relative_rotation.Roll = 0.0
        -- Only the centered 30x30 authored ring has alpha inside this
        -- 250x250 render target. At 0.10 scale it became sub-pixel at the
        -- ten-metre convergence distance. Scale 1 preserves a small readable
        -- HUD-ring appearance while remaining visible in both eyes.
        world_reticle.RelativeScale3D =
            Vector3d.new(1.0, 1.0, 1.0)
        -- The native plugin updates this component from the same
        -- HMD-relative basis used by the weapon and projectile hooks. Do not
        -- attach it to UObjectHook's generic controller transform, which
        -- double-counts head motion for this game's Blam viewmodel path.
    end)
    if not configured then
        release_reticle("component-owned UMG configuration failed")
        return false
    end

    local hosted = false
    pcall(function()
        hosted =
            world_reticle.Widget == owned_reticle and
            live_in_object_array(owned_reticle)
    end)
    if not hosted then
        release_reticle("component-owned UMG host verification failed")
        return false
    end

    world_reticle_widget = owned_reticle
    world_reticle_parent = nil
    fs.write(widget_path, "weapon reticle material pending native sync\n")
    if not set_screen_reticle_suppressed(true) then
        release_reticle("screen reticle suppression failed")
        return false
    end
    -- Keep the old sphere only through a short ownership proof window.
    proof_frames = 0

    publish_ready(true)
    last_release_reason = nil
    print(
        "[Halo motion reticle] component-owned authored UMG reticle " ..
        "created in right-controller world space")
    return true
end

publish_ready(false)
fs.write(diagnostic_path, "")
fs.write(widget_path, "inactive\n")
print("[Halo motion reticle] loaded (dependency-free)")

local function attempt_create()
    local ok, result = pcall(create_reticle)
    if ok and result then
        -- Components are created visible; resync against the hide marker
        -- on the next frame if a hold or zoom is already active.
        reticle_hidden = false
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
    if not runtime.active then
        return
    end

    marker_poll_frames = marker_poll_frames - 1
    if marker_poll_frames <= 0 then
        marker_poll_frames = 12
        gameplay_ready =
            fs.read(gameplay_path):sub(1, 5) == "ready"
        replacement_hidden =
            fs.read(hide_path):sub(1, 2) == "on"
    end

    if not gameplay_ready then
        if anchor ~= nil or reticle ~= nil or reticle_light ~= nil or
            world_reticle ~= nil then
            release_reticle("gameplay inactive")
        end
        return
    end

    if valid(anchor) and valid(reticle) and valid(reticle_light) and
        valid(world_reticle) then
        local hide = replacement_hidden
        if hide ~= reticle_hidden then
            reticle_hidden = hide
            pcall(function()
                world_reticle:SetVisibility(not hide)
                world_reticle:SetHiddenInGame(hide)
                reticle:SetVisibility(false)
                reticle:SetHiddenInGame(true)
                reticle_light:SetVisibility(false)
                reticle_light:SetHiddenInGame(true)
            end)
            -- Halo keeps its scope and two-hand fallback screen-relative.
            -- Restore the original image only while the controller-hosted
            -- replacement is deliberately hidden.
            set_screen_reticle_suppressed(not hide)
        end

        -- The native plugin roots the hosted widget and validates its exact
        -- Reticle_Image/MID at 4 Hz. Do not rescan the entire GUObjectArray
        -- for those same pointers every viewport draw: this game carries
        -- roughly 300,000 UObjects and several full scans per frame reduced
        -- the XR session to single-digit FPS. A pointer transition is the
        -- only event that needs the authoritative image search here.
        local structurally_ready = false
        pcall(function()
            local hosted_widget = world_reticle.Widget
            if hosted_widget ~= nil and
                hosted_widget ~= world_reticle_widget then
                -- The native plugin replaces a component-owned widget if
                -- Unreal's GC clears the Lua-created instance. Adopt the new
                -- strongly retained instance and its authored image.
                world_reticle_widget = hosted_widget
                world_reticle_image =
                    find_reticle_image(hosted_widget)
            end
            local same_widget =
                hosted_widget == world_reticle_widget
            structurally_ready =
                same_widget and
                world_reticle_widget ~= nil and
                world_reticle_image ~= nil
        end)

        if structurally_ready then
            if proof_frames < 2 then
                proof_frames = proof_frames + 1
                if proof_frames == 2 then
                    pcall(function()
                        reticle:SetVisibility(false)
                        reticle:SetHiddenInGame(true)
                        reticle_light:SetVisibility(false)
                        reticle_light:SetHiddenInGame(true)
                    end)
                    print(
                        "[Halo motion reticle] authored UMG reticle " ..
                        "verified; sphere fallback hidden")
                end
            end
        else
            pcall(function()
                reticle:SetVisibility(false)
                reticle:SetHiddenInGame(true)
                reticle_light:SetVisibility(false)
                reticle_light:SetHiddenInGame(true)
            end)
            proof_frames = 0
        end

        pose_frames = pose_frames - 1
        if pose_frames <= 0 then
            pose_frames = 60
            pcall(function()
                -- The native plugin deliberately clears this marker during
                -- reload/init. Republish while the owned world widget remains
                -- valid so the original centered CHUD reticle cannot return
                -- underneath the replacement.
                publish_ready(structurally_ready)
                local anchor_location = anchor:K2_GetComponentLocation()
                local anchor_rotation = anchor:K2_GetComponentRotation()
                local reticle_location =
                    world_reticle:K2_GetComponentLocation()
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

    if anchor ~= nil or reticle ~= nil or reticle_light ~= nil or
        world_reticle ~= nil then
        local anchor_state = valid(anchor) and "valid" or "invalid"
        local reticle_state = valid(reticle) and "valid" or "invalid"
        local light_state =
            valid(reticle_light) and "valid" or "invalid"
        local world_state =
            valid(world_reticle) and "valid" or "invalid"
        release_reticle(
            "component lifetime lost (anchor=" .. anchor_state ..
            ", reticle=" .. reticle_state ..
            ", light=" .. light_state ..
            ", world=" .. world_state .. ")")
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
