// SPDX-License-Identifier: GPL-3.0-or-later
//
// The castle's room layout, read at runtime from the player's own romfs.
//
// Each biome bundle (Data/StreamingAssets/aa/Switch/data-world_biome_<NN>_<name>_assets_all_*.bundle)
// holds one WorldData MonoBehaviour whose `_rooms` list places every room on the biome's chunk
// grid (`positionCoord`, `runtimeRoomNumber` = the `uid` in the room's scene name), one RoomData
// per room (`_width` x `_height` chunks of `chunkSize` = 24x13 tiles, each chunk with an
// `enabled` flag and a per-tile `_collisions` byte: 1 = solid, 0 = open), and one
// RoomConfiguration per room (save / boss / fast-travel flags, ZDoor positions).
// Chunk and tile y grow upwards (Unity world space).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bc_world {

struct Chunk {
    bool enabled = false;
    std::uint32_t doors = 0;
    std::vector<std::uint8_t> collisions; // chunk_w * chunk_h, row-major from the bottom row
};

struct Room {
    std::int32_t uid = -1;            // runtimeRoomNumber
    std::int32_t pos_x = 0, pos_y = 0; // positionCoord, in chunks
    std::int32_t w = 0, h = 0;         // size in chunks
    std::int32_t chunk_w = 0, chunk_h = 0;
    std::string name;  // RoomData m_Name
    std::string scene; // sceneAddressableEntryAddress
    bool save_room = false, boss_room = false, fast_travel_room = false;
    std::vector<Chunk> chunks; // w * h, index = x + y * w
};

struct Biome {
    std::string world_name; // "World_Biome_01_Streets_Poor"
    std::string bundle;     // bundle file name
    std::vector<Room> rooms;
    // Bounding box of enabled chunks, in chunks (inclusive).
    std::int32_t min_cx = 0, min_cy = 0, max_cx = -1, max_cy = -1;
    std::int32_t chunk_w = 24, chunk_h = 13;

    const Room* FindUid(std::int32_t uid) const;
};

struct World {
    std::vector<Biome> biomes;
    const Biome* FindWorld(std::string_view world_name) const;
    std::size_t RoomCount() const;
};

/// Reads a whole romfs file; nullopt when it is missing.
using ReadFile = std::function<std::optional<std::vector<std::uint8_t>>(const std::string& path)>;

/// The biome bundles of the supported build (file names carry a content hash).
const std::vector<std::string>& PinnedBiomeBundles();

/// Directory of the Addressables bundles inside romfs.
inline constexpr std::string_view kBundleDir = "Data/StreamingAssets/aa/Switch/";

/// Parses one biome bundle. `error` explains a failure.
std::optional<Biome> LoadBiome(const std::vector<std::uint8_t>& bundle_bytes,
                               std::string bundle_name, std::string* error = nullptr);

/// Loads every pinned biome through `read` (paths are kBundleDir + name). Biomes that fail are
/// reported in `log` and skipped.
std::shared_ptr<const World> LoadWorld(const ReadFile& read, std::string* log = nullptr);
/// The same for an explicit list of bundle file names.
std::shared_ptr<const World> LoadWorld(const ReadFile& read, const std::vector<std::string>& names,
                                       std::string* log = nullptr);

/// Biome bundle names found in an Addressables catalog (raw catalog.json bytes), in order.
std::vector<std::string> BiomeBundlesFromCatalog(std::string_view catalog);

} // namespace bc_world
