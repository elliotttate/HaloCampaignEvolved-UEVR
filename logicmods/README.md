# Halo reticle color LogicMod

The release installer copies `HaloCEReticleColor.pak`, `.utoc`, and `.ucas`
to `Meteorite/Content/Paks/LogicMods`.

The IoStore container mounts exactly these two UE 5.6 packages:

- `/Engine/VREditor/UI/WidgetVRPassThrough`
- `/Engine/VREditor/UI/WidgetVRPassThrough_Translucent_OneSided`

The small legacy PAK carries the same four cooked package files as a fallback
for builds that resolve package files through `FPakPlatformFile`. The IoStore
container supplies the equivalent export bundles for Halo's normal package
store.

Do not add the cook project's generated `global.utoc` or `global.ucas` here.
They contain a standalone project's script-object table and would collide with
Halo's game-global IoStore container. Halo's own global container already
provides every engine script object imported by these materials.
