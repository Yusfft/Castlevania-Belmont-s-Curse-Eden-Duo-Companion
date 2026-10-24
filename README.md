# Castlevania: Belmont's Curse – Eden Duo map companion (test build 0.5.5)

A second-screen companion for [Eden Duo](https://github.com/igawa6/eden-duo) that shows the
castle map of Castlevania: Belmont's Curse on the lower screen of a dual-screen Android
handheld (developed on an AYN Thor).

| | |
|---|---|
| Game | Castlevania: Belmont's Curse, title ID `0100BA4026E94000` |
| Game version | 1.1.0 (build `0F1169AA7D0FA4529C861F96ADEC306E`) |
| Eden Duo | 1.3.x (package declares `min_runtime` 22) |

The package ships **no game assets**. The map, its icons and the title font are read at runtime
from the player's own romfs and drawn by the native module.

## Features

- **The game's own map.** Each biome is drawn from the game's level-map tiles with the game's
  map material re-implemented on the CPU: the rooms, their outlines, water, the castle
  silhouette and the parchment, and the torn gold edge of the fog of war. Only what the player
  has explored is revealed, zone by zone, exactly as the game reveals it.
- **Map icons.** Mirrors of Life (save points), Z-doors (seen / used), biome exits, elevators,
  barricades and landmarks, shown under the same conditions as in the game.
- **Your map markers and the death spot.** The markers you place on the game's map (cross,
  chest, sun, eye, cup, stick, coin, sword, moon) and the red skull where you last died, with
  the game's own icons, on the map they belong to.
- **Player marker.** The game's gold star, at the player's exact position every frame. The map
  slides under it; a new section is drawn in the background when needed.
- **Area title.** The biome and district names in the game's own font (English).
- **Rooms in the map's vivid palette**, the one the game uses under its map cursor.
- **The game's map-screen interface.** The map screen's frame (darkened, so it does not draw the
  eye), the level banner in the level's colour, the zoom scale and buttons, all from the game's
  own art, with text in the game's font.
- **Zoom.** Five levels: the whole area the player is in (the main map, a tower, a Z-room), then
  four follow views from 5 to 20 pixels per world unit. The small zoom button shows the level;
  tap it for the next level out (wrapping to the closest), hold it for the scale with its − / +
  buttons and the levels' names. The page opens in the closest view, where the map's outlines
  are drawn sharper than the game's 4-texels-per-unit map texture would give.
- **Pins.** The Pin button opens three symbols (the game's marker art) under it; with one picked,
  a tap on the map puts a pin there. Tapping a pin with another symbol picked changes it, with the
  same symbol takes it away. Up to 24 pins, kept across sessions in a small text file of the
  module's own (`dualscreen/persist/0100BA4026E94000/belmonts-curse-map-pins.txt` in the
  emulator's data folder; on Android the app's private files folder). They are the page's own:
  the game's map does not show them.
- **Game over.** When the player dies the page darkens and a card with the game's death icon and
  ornament glows up and fades (`DeathResultComponent.hasJustDied`, set by
  `DeathManagementSystem` the moment of death).
- **Pause.** While a menu or the pause screen is open (the game's ECS context paused by a game state
  that takes UI input), the map is blurred under "Paused".
- **Before a save is loaded** (the game's own menus): one of the game's painted biome cards (the
  save screen's `T_UI_Save_BiomeCards` atlas, one picked at random each session) over a blurred
  copy of itself, "Press A to confirm" at the bottom and the mod's version at the bottom right.
- **Inventory page** (Debug → Inventory): what the module reads of the inventory, level and stats, the first step of the game-menu pages.
- **Debug page.** The Debug button (or hold the area name): every step of the live memory chain
  and the map loader's log.

## How it works

### Map data (romfs, at runtime)

The level map of biome `<NN>` lives in its map scene
`Data/StreamingAssets/aa/Switch/maps_scenes_<NN>_<name>_map_1_<hash>.bundle`:

- `LevelMapRenderer.tileRenderers`: one `LevelMapTileRenderer` per 128 x 128 world-unit tile
  (`bounds`), listed row by row from the top, which is also the slice order of the biome's
  `LevelMap_<NN>_img` texture array (512 x 512 per slice, 4 texels per world unit;
  R = outside-room mask, G = distance to rooms, B = outlines, A = water lines).
- The tiles' material `LevelMap_<NN>_mat` (shader `RedWine/RW_LevelMap`, in the biome's `art-`
  bundle or `ui-gamemenus`): its textures (the map array, the biome silhouette array, the
  tileable parchment, a noise texture) and its ~60 shading values. The project renders in
  linear colour space; colour values are converted like Unity does (except `[HDR]` ones).
- `LevelMapFogOfWarRoomStatesHolder`: for every room, its exploration zones and their shapes
  (boxes and quads, as managed references). `SDGTextureFromShapes.smoothness` gives the
  smooth-union radius.
- `LevelMapDistrict` / `LevelMapDistrictsModel`: district chunk masks; names come from
  `DistrictDisplayData` and the English string tables.
- The map icons are the scene's `SpriteRenderer`s under `LevelMap*Item` components (save point,
  Z-door, biome transition, elevator, activable, landmark); the Z-door display's state lists
  (`discoveredUnenteredObjects`, `enteredUncompletedObjects`, ...) say which sprite shows when.
- `LevelMapNavigationZone.cameraBounds`: the parts of the map the whole-area view fits.

Readers: `native/bc_unity.*` (bounds-checked UnityFS / SerializedFile / type-tree reader with
on-demand LZ4 block reads and managed references), `bc_tex.*` (RGBA, Alpha8, BC1/BC3/BC4 and
the Switch block-linear layout; ASTC through Eden's decoder), `bc_levelmap.*` (the loader).
Drawing: `bc_shade.h` (the map shading), `bc_fog.h` (fog distance field), `bc_mapdraw.*`
(tiles, fog, icons, marker), `bc_ui.*` (frame, banner, zoom scale, buttons and text from the
map screen's own sprites: `Map_Background`, `Map_InfoParchment`, `cartouche`, `Map_Scale*`). The shading and fog maths were checked numerically against
the game's own shaders. If a biome's level map cannot be read, the room layout from the
`data-world_biome_*` bundles is drawn instead (`bc_world.*`, `bc_render.*`, the 0.2 renderer).

Bundle file names are pinned for this build, with a fallback that scans `aa/catalog.json`.

### Live state (guest memory)

All locations come from the game's own `global-metadata.dat` (IL2CPP v31) and `main` NSO:

| What | Where |
|---|---|
| Root | `MotherBase.EntityFramework.SystemManager` TypeInfo slot `main+0x6D47368`, static `Instance` |
| Systems | `SystemManager._fixedUpdateSystems / _dynamicUpdateSystems / _lateUpdateSystems` (`+0x28/0x30/0x38`) |
| Current chunk | `RoomManager.RoomManagerSystem._currentChunk` `+0x80` (`_previousChunk` `+0x78`) |
| Current room | `RoomManagerSystem._currentRoomHandle` `+0x90` → `worldRoomData` `+0x18` (`worldName`, `positionCoord`, `runtimeRoomNumber`, `_instanceName`, `_id`) |
| Player position | `ExplorerComponent.lastMainMapPosition` `+0x90` (Vector3, world units), written every frame by `ExplorationSystem.TryUpdateMainMapPosition` from the player's transform |
| Exploration | `ExplorationSystem._mcExplorerComponent` `+0x48` → `worldRoomExplorationStates` (`Dictionary<string, WorldRoomExplorationState>`) |
| Per room | `enteredZonesIds` `+0x50`, `enteredZDoorIDs` `+0x38`, `discoveredZDoorExplorationZoneIDs` `+0x28`, `discoveredSecretZones` `+0x48`, `gpeSpecialActivated` `+0x58` (all `List`) |
| Pause | `SystemManager._context` `+0x20` → `EntityContext._contextPaused` `+0x68`; the active state: `GameStateManager` (TypeInfo slot `main+0x6BDBA10`) `._instance._states` `+0xA0` top → `+0x18` → `EditorParameteredGameState` (`_pauseEntityContext` `+0x18`, `_activeActionMaps` `+0x28`) |
| Inventory | `InventoryComponent.inventory` `+0x28` → `Inventory` (`_itemsInInventory` `+0x20`, `_equippedWeapon` `+0x28`, `_equippedSkin` `+0x30`, `_equippedSpell` `+0x38`, `_equippedCharms` `+0x40`); `ItemData` `itemID` `+0x20`, `itemType` `+0x28`, `itemDisplayName` `+0x30` → key `+0x30` |
| Level and stats | `ExperienceComponent._currentLevel` `+0x78` (StatRef → `_statObject` `+0x18`); `StatsStateComponent.CurrentStats` `+0x28` (`Dictionary<Type, StatObject>`; `baseValue` `+0x18`, `modifiers` `+0x20`); stat names from the type's definition in the loaded metadata |
| Last save room | `ExplorerComponent.lastUsedSaveRoomId` `+0x88` |
| Current map | `ExplorerComponent.lastMainMapLevelId` `+0xA0` → `LevelID._guid` `+0x18` (matched with the map scene's `LevelMapContext.level`) |
| Player components | `ExplorerComponent.selfEntity` `+0x20` → `Entity.componentCache` `+0x30` (`Dictionary<int, Component>`, filled in `Awake`) |
| Map markers | `MapMarkersComponent._levelsMarkersData` `+0x28` (`Dictionary<LevelID, LevelMarkersData>`) → `markersData` `+0x18` → `MapMarkerData` (`position` `+0x14`, `markerTypeGuid` `+0x20`); the type guid is a `MapMarker_<Name>_ID` and its icon is `Map_MarkerSet_<Name>` |
| Death spot | `DeathResultComponent.levelIdOfLastDeath` `+0x58`, `positionOfLastDeath` `+0x24` (`lastMainMapDeathPosition` `+0x30` when `lastDeathInFastTravelRoom` `+0x49`), shown like `LevelMapDeathSpotItem.Refresh` |

The position is read twice per sample and taken only when both reads agree (the game writes it from another core). A jump of more than 12 units is shown only once it holds for 6 samples, a missing position keeps the last good one for 3 s, and a change of world must hold 10 samples, so a stray value never moves the map.

Classes are checked by name before their fields are read, every read is bounded, and each step
fails closed. Under NCE the data segment may sit at a constant shift from `main_base`; the
module learns it once from `Il2CppMetadataRegistration` (`main+0x6A95908`), whose counts are fixed
for this build.

## Building

The GitHub Actions workflow (`.github/workflows/build.yml`) builds the Android arm64 and Linux
modules, runs the tests and produces `0100BA4026E94000.dsmod.zip` as an artifact (Actions tab →
`build` → the run's **Artifacts**).

Locally (Linux):

```sh
cmake -S native -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build                                    # live reader against a fake guest
build/bc_host_sim build/0100BA4026E94000.so <romfs> out   # drive the module through its C ABI
python3 tools/gen_manifest.py                             # regenerate package/dualscreen/manifest.json
```

`bc_host_sim` plays the host: it needs an ASTC decoder for the parchment, silhouettes and
icons, which Eden provides on the device. For local runs, point `-DBC_TEST_ASTC_DIR=<dir>` at a
directory holding an `astc.cpp` and an `astc_rgba.h` that defines
`bool TestAstcDecode(const uint8_t* in, size_t in_size, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, uint8_t* out)`.

Android arm64 (NDK r28c):

```sh
cmake -S native -B build-android -G Ninja -DCMAKE_BUILD_TYPE=Release -DBC_BUILD_TOOLS=OFF \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_static
cmake --build build-android
```

Package with `tools/build_dualscreen_package.py` (see the workflow for the exact command).

## Installing

1. Copy `0100BA4026E94000.dsmod.zip` to the device. Do not rename it.
2. In Eden Duo, long-press the game → **Add-ons** → **Install** → **Dual screen mods** → pick the file.
3. Start the game and load a save. The map page appears on the second screen.

## Known limits

- Z-doors whose target is fully completed show the "visited" icon, not the game's gold
  "completed" one, and districts are not tinted gold when completed (completion is not read yet).
- Secret rooms stay hidden on the map even after they are found.
- Names are the English ones.

## Testing feedback

Useful reports: a photo of the **Map** page next to the game's own map at the same spot, and a
photo of the **Debug** page. The key rows are `STATUS`, `ROOM UID`, `FOG OK N MATCH ZONES`,
`FOG HERE`, `LEVEL MAP  LOAD MS  DRAW MS` and `LEVEL MAP LOG`.

## License

GPL-3.0-or-later. The ABI headers in `native/third_party/eden` and
`tools/build_dualscreen_package.py` are copied unchanged from Eden Duo and eden-duo-companions
(GPL-3.0-or-later).
