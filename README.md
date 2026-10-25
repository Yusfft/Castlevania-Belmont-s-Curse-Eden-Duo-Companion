# Castlevania: Belmont's Curse – Eden Duo map companion (test build 0.8.0)

A second-screen companion for [Eden Duo](https://github.com/igawa6/eden-duo) that shows the
castle map of Castlevania: Belmont's Curse on the lower screen of a dual-screen Android
handheld (developed on an AYN Thor).

| | |
|---|---|
| Game | Castlevania: Belmont's Curse, title ID `0100BA4026E94000` |
| Game version | 1.1.0 (build `0F1169AA7D0FA4529C861F96ADEC306E`) |
| Eden Duo | 1.3.x (package declares `min_runtime` 22) |

The package ships **no game assets**. The map, its icons and the title font are read at runtime
from the player's own romfs and drawn by the native module. Since 0.7.0 the package also carries
a small guest helper of its own (a load plan, see below) so the second screen can change
equipment and place the game's map markers.

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
- **Zoom.** Three follow views (Far, Near, Close: 8, 13 and 20 pixels per world unit). The small
  zoom button shows the level; tap it for the next level out (wrapping to the closest), hold it
  for the scale with its − / + buttons and the levels' names. The page opens in the closest view, where the map's outlines
  are drawn sharper than the game's 4-texels-per-unit map texture would give.
- **Pins are the game's own map markers.** The Pin button opens three symbols (cross, chest,
  eye; the game's marker art) under it; with one picked, a tap on the map places that marker in
  the game itself (`MapMarkersComponent.AddMarker`), in the navigation zone under the tap, so it
  shows on the game's map screen and is kept in the save. Tapping a marker with another symbol
  picked changes it, with the same symbol removes it; this works on any marker on the map,
  including ones placed in the game. The game's marker limit is respected. (The 0.6 page-only
  pins file is no longer read.)
- **The game's own Equipment menu** (the Equipment button at the bottom left of the map). The
  game's pause menu is rebuilt from its scene in the romfs and laid out the way Unity lays it out:
  the tab bar, the equipped arcana card (with its gold leaf), the weapon's illustration, the
  relic slots and the picked relic's description, ATK / DEF / HP / MP, the heart and mana
  upgrades, level and experience bar, with the player's own items. Tap the weapon card for the
  Armoury, the arcana card for the Arcanas, a relic slot for the Relics (each the game's own
  inventory screen, with its cards and descriptions). In an inventory the first tap on an item
  shows it in the card, a second tap equips it; in the Relics, tap a slot to choose where a relic
  goes and tap the chosen slot again to take its relic off. Back (bottom left) returns to the
  equipment screen, the Map tab to the map. The change is made by the game's own
  `Inventory.EquipWeapon / EquipSpell / EquipCharmAtSlot / UnequipCharmAtSlot`, so stats and the
  in-game menus follow, and a line at the bottom says what the game answered.
- **Game over.** When the player dies the page darkens and a card with the game's death icon and
  ornament glows up and fades (`DeathResultComponent.hasJustDied`, set by
  `DeathManagementSystem` the moment of death).
- **Pause.** While a menu or the pause screen is open (the game's ECS context paused by a game state
  that takes UI input), the map is blurred under "Paused".
- **Before a save is loaded** (the game's own menus): one of the game's painted biome cards (the
  save screen's `T_UI_Save_BiomeCards` atlas, one picked at random each session) over a blurred
  copy of itself, "Press A to confirm" at the bottom and the mod's version at the bottom right.
- **Loading.** While the game loads a scene (its loading state on top for 0.4 s or more) or the
  map is not ready yet, the card of the current biome and "Loading, be patient" with its dots
  running 1-2-3. The zoom and pin buttons show only over the map.
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
- `LevelMapNavigationZone.cameraBounds`: the parts of the map (main map, towers, Z-rooms) the game's own map screen fits.

Readers: `native/bc_unity.*` (bounds-checked UnityFS / SerializedFile / type-tree reader with
on-demand LZ4 block reads and managed references), `bc_tex.*` (RGBA, Alpha8, BC1/BC3/BC4 and
the Switch block-linear layout; ASTC through Eden's decoder, in tiles of at most 2048 x 2048
texels, its limit), `bc_levelmap.*` (the loader), `bc_cards.*` (the save screen's biome cards, read on
demand for the title and loading pages).
Drawing: `bc_shade.h` (the map shading), `bc_fog.h` (fog distance field), `bc_mapdraw.*`
(tiles, fog, icons, marker), `bc_ui.*` (frame, banner, zoom scale, buttons and text from the
map screen's own sprites: `Map_Background`, `Map_InfoParchment`, `cartouche`, `Map_Scale*`). The shading and fog maths were checked numerically against
the game's own shaders. If a biome's level map cannot be read, the room layout from the
`data-world_biome_*` bundles is drawn instead (`bc_world.*`, `bc_render.*`, the 0.2 renderer).

Bundle file names are pinned for this build, with a fallback that scans `aa/catalog.json`.

### Game menu (romfs, at runtime)

The game's pause menu is the uGUI / TextMeshPro hierarchy `GameMenuView` of
`afm_scenes_scenes_gamemenu`. `bc_menu.*` reads it (GameObjects, RectTransforms, Images and their
sprites in the game's sprite atlases, TextMeshPro texts with their English strings, layout groups,
layout elements, content size and aspect ratio fitters, canvas groups, masks, Doozy start
positions), lays it out with Unity's rules for a canvas matched to the page's width (the game's
3840 x 2160 reference) and draws it with the game's art and font. `bc_menupage.*` fills it in the
way the game's own display components do: each slot's `EquipmentViewEquippedItemController`,
`InventoryItemDisplay*`, `ArcanaDisplay`, `IntStatValueDisplay`, `ExperienceBarDisplay`,
`FragmentedCollectibleCountDisplay` and the inventories' `Inventory*Checker` components name the
images, texts and stats they show, and their UnityEvents (which objects they show or hide) are run
as the game runs them. Item names, descriptions (with their smart-string values), icons,
illustrations, mana costs and notions come from the item assets, found by `itemID`. An absorbed
arcana's art is drawn by the game with a gold-leaf material (`RedWine/UI/Image/Goldleaf`); the
module approximates its still look (the painting a little darker, warmed to gold where the
texture's alpha marks gold leaf). The menu's bundles are pinned in `native/bc_menu_bundles.inc`.
The page is read in the background once a save is loaded (a few seconds) and each state is drawn
on the asset worker (about 0.2-0.5 s); the tap targets come from the same layout.

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
| Pause | `SystemManager._context` `+0x20` → `EntityContext._contextPaused` `+0x68`; the active state: `GameStateManager` (TypeInfo slot `main+0x6D42540`; `main+0x6BDBA10` only holds that slot's address) `._instance._states` `+0xA0` top → `+0x18` → `EditorParameteredGameState` (`_pauseEntityContext` `+0x18`, `_activeActionMaps` `+0x28`) |
| Loading | the top state is `GameStateManager.loadingState` `+0x68` |
| Inventory | `InventoryComponent.inventory` `+0x28` → `Inventory` (`_itemsInInventory` `+0x20`, `_equippedWeapon` `+0x28`, `_equippedSkin` `+0x30`, `_equippedSpell` `+0x38`, `_equippedCharms` `+0x40`); `ItemData` `itemID` `+0x20`, `itemType` `+0x28`, `itemDisplayName` `+0x30` → key `+0x30` |
| Level and stats | `ExperienceComponent._currentLevel` `+0x78` (StatRef → `_statObject` `+0x18`); `StatsStateComponent.CurrentStats` `+0x28` (`Dictionary<Type, StatObject>`; `baseValue` `+0x18`, `modifiers` `+0x20`); stat names from the type's definition in the loaded metadata |
| Last save room | `ExplorerComponent.lastUsedSaveRoomId` `+0x88` |
| Current map | `ExplorerComponent.lastMainMapLevelId` `+0xA0` → `LevelID._guid` `+0x18` (matched with the map scene's `LevelMapContext.level`) |
| Player components | `ExplorerComponent.selfEntity` `+0x20` → `Entity.componentCache` `+0x30` (`Dictionary<int, Component>`, filled in `Awake`) |
| Map markers | `MapMarkersComponent._levelsMarkersData` `+0x28` (`Dictionary<LevelID, LevelMarkersData>`) → `markersData` `+0x18` → `MapMarkerData` (`position` `+0x14`, `markerTypeGuid` `+0x20`); the type guid is a `MapMarker_<Name>_ID` and its icon is `Map_MarkerSet_<Name>` |
| Death spot | `DeathResultComponent.levelIdOfLastDeath` `+0x58`, `positionOfLastDeath` `+0x24` (`lastMainMapDeathPosition` `+0x30` when `lastDeathInFastTravelRoom` `+0x49`), shown like `LevelMapDeathSpotItem.Refresh` |

The position is read twice per sample and taken only when both reads agree (the game writes it from another core). A jump of more than 12 units is shown only once it holds for 6 samples, a missing position keeps the last good one for 3 s, and a change of world must hold 10 samples, so a stray value never moves the map.

### Guest helper (load plan)

Changing equipment and markers needs code running on the game's own thread. Eden Duo 1.3
(runtime 19+) can load a *format-2 load plan* from the package:
`package/dualscreen/guest/<build>/load-plan.json`, written by `tools/gen_load_plan.py` from
`native/helper/bc_guest_helper.S` (my own code; the game's bytes appear only as the 4-byte
`expected` value of the one patch).

- The patch turns the first instruction of `ApplicationFlowManager.Update` (`main+0x40D4F00`,
  once per frame on the main thread) into a branch to the helper, which runs the displaced
  instruction and returns.
- The module and the helper talk through Eden's guest mailbox (4 KiB): the module writes a
  command (equip weapon / arcana / relic, unequip relic, add / remove / change marker), its
  objects and the classes it saw, then bumps the request number; the helper checks everything
  again (exact classes, the item in `_itemsInInventory`, the relic slot within
  `_equippedCharms`, not while `ApplicationFlowManager._isLoading`, the request made after the
  last emulator pause/resume via Eden's epoch word at `+0xFFC`), calls the game's method and
  writes the result. Anything unexpected is refused, never called.
- `tools/test_guest_helper.py <main image>` runs the patched `Update` and the helper in
  Unicorn with stubbed game methods (needs `unicorn`, `pyelftools`, `clang`).

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
build/bc_menu_tool <romfs> out/menu                       # draw the menu's four views from a romfs
python3 tools/gen_manifest.py                             # regenerate package/dualscreen/manifest.json
python3 tools/gen_load_plan.py                            # regenerate the guest helper's load plan
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
- The menu's Tarot, Journey and System tabs are not there yet (only Map and Equipment work).
  The arcanas' talents and tasks and the inventories' gamepad prompts are left out; button
  glyphs inside descriptions are dropped.
- The guest helper (since 0.7.0) changes the game's state: keep a backup of your save while testing.

## Testing feedback

Useful reports: a photo of the **Map** page next to the game's own map at the same spot, and a
photo of the **Debug** page. The key rows are `STATUS`, `ROOM UID`, `FOG OK N MATCH ZONES`,
`FOG HERE`, `LEVEL MAP  LOAD MS  DRAW MS` and `LEVEL MAP LOG`.

## License

GPL-3.0-or-later. The ABI headers in `native/third_party/eden` and
`tools/build_dualscreen_package.py` are copied unchanged from Eden Duo and eden-duo-companions
(GPL-3.0-or-later).
