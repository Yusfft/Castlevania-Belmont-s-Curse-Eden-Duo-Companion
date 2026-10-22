# Castlevania: Belmont's Curse – Eden Duo map companion (test build 0.1.0)

A second-screen companion for [Eden Duo](https://github.com/igawa6/eden-duo) that shows the
castle map of Castlevania: Belmont's Curse on the lower screen of a dual-screen Android
handhelds (specificallu developed to run on an AYN Thor).

| | |
|---|---|
| Game | Castlevania: Belmont's Curse, title ID `0100BA4026E94000` |
| Game version | 1.1.0 (build `0F1169AA7D0FA4529C861F96ADEC306E`) |
| Eden Duo | 1.3.x (package declares `min_runtime` 16) |

The package ships **no game assets**. The room layout is read at runtime from the player's own
romfs and drawn by the native module.

## Features

- **Map page.** The current biome, drawn room by room from the game's own collision grids,
  with chunk-level fog of war and a marker on the player's current chunk.
  - `ZOOM`: whole biome, wide view, close view.
  - `FOG`: show or hide unexplored rooms (dimmed).
  - `- AREA` / `AREA +`: browse the other biomes.
  - `FOLLOW`: go back to following the player.
- **Debug page.** Every step of the live memory chain (resolve stage, current chunk, room,
  exploration state, world loading). This first build is meant to verify that chain on a device.

## How it works

### Map data (romfs, at runtime)

Each biome bundle (`Data/StreamingAssets/aa/Switch/data-world_biome_<NN>_<name>_assets_all_<hash>.bundle`,
plain UnityFS, LZ4HC) contains:

- one `WorldData` MonoBehaviour whose `_rooms` list places every room on the biome's chunk
  grid (`positionCoord`; `runtimeRoomNumber` equals the `uid` in the room's scene name);
- one `RoomData` per room: `_width` x `_height` chunks of `chunkSize` 24x13 tiles, each chunk with
  an `enabled` flag and a per-tile `_collisions` byte (1 solid, 0 open);
- one `RoomConfiguration` per room (save / boss / fast-travel flags, ZDoor positions).

`native/bc_unity.*` is a small bounds-checked UnityFS / SerializedFile / type-tree reader
(its output matches UnityPy 1.25 on all 12,355 MonoBehaviours of the map bundles). `bc_world.*`
builds the room model (384 rooms in 10 biomes for 1.1.0) and `bc_render.*` rasterises it.
Bundle file names are pinned for this build, with a fallback that scans `aa/catalog.json`.

### Live state (guest memory)

All locations come from the game's own `global-metadata.dat` (IL2CPP v31) and `main` NSO:

| What | Where |
|---|---|
| Root | `MotherBase.EntityFramework.SystemManager` TypeInfo slot `main+0x6D47368`, static `Instance` |
| Systems | `SystemManager._fixedUpdateSystems / _dynamicUpdateSystems / _lateUpdateSystems` (`+0x28/0x30/0x38`) |
| Current chunk | `RoomManager.RoomManagerSystem._currentChunk` `+0x80` (`_previousChunk` `+0x78`) |
| Current room | `RoomManagerSystem._currentRoomHandle` `+0x90` → `worldRoomData` `+0x18` (`worldName`, `positionCoord`, `runtimeRoomNumber`, `_instanceName`, `_id`) |
| Exploration | `ExplorationSystem._mcExplorerComponent` `+0x48` → `worldRoomExplorationStates` (`Dictionary<string, WorldRoomExplorationState>`) → `chunksExplorationState` (`NoChunk`, `Unexplored`, `ExploredTemporary`, `Validated`) |

Classes are checked by name before their fields are read, every read is bounded, and each step
fails closed. Under NCE the data segment may sit at a constant shift from `main_base`; the
module learns it once from `Il2CppMetadataRegistration` (`main+0x6A95908`), whose counts are fixed
for this build.

**Not yet verified on a device:** the full live chain, whether `_currentChunk` is room-local or
biome-wide (the debug page shows `CHUNK MODE`), and the exploration-state values.

## Building

The GitHub Actions workflow (`.github/workflows/build.yml`) builds the Android arm64 and Linux
modules, runs the tests and produces `0100BA4026E94000.dsmod.zip` as an artifact (Actions tab →
`build` → the run's **Artifacts**).

Locally (Linux):

```sh
cmake -S native -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build                                    # live reader against a fake guest
build/bc_world_tool <romfs-or-bundle-dir> out             # render every biome to PPM
build/bc_host_sim build/0100BA4026E94000.so <romfs> out   # drive the module through its C ABI
python3 tools/gen_manifest.py                             # regenerate package/dualscreen/manifest.json
```

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

## Testing feedback

Useful reports: a photo of the **Debug** page in one room and again after walking into the next
room, plus a photo of the **Map** page. The key rows are `STAGE`, `STATUS`, `CHUNK X Y`,
`CHUNK MODE`, `ROOM UID`, `FOG OK N MATCH`, `FOG HERE` and `WORLD BIOMES ROOMS`.

## License

GPL-3.0-or-later. The ABI headers in `native/third_party/eden` and
`tools/build_dualscreen_package.py` are copied unchanged from Eden Duo and eden-duo-companions
(GPL-3.0-or-later).
