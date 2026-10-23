// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game's own level map, read at runtime from the player's romfs.
//
// Each biome has a map scene (maps_scenes_<NN>_<name>_map_1_*.bundle). Its LevelMapRenderer
// lists one LevelMapTileRenderer per 128 x 128 world-unit tile; every tile draws one slice of
// the biome's LevelMap_<NN>_img texture array (4 texels per world unit) with the biome's map
// material (shader RedWine/RW_LevelMap). The material's textures and values, the fog-of-war
// exploration zones (LevelMapFogOfWarRoomStatesHolder), the districts and the map icons
// (SpriteRenderers under the LevelMap*Item components) all come from that scene and the bundles
// it references. Names come from the English string tables.
//
// Nothing read here is stored or shipped; the module only draws with it.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "bc_fog.h"
#include "bc_shade.h"
#include "bc_tex.h"
#include "bc_unity.h"

namespace bc_levelmap {

/// Romfs access: copies up to `size` bytes at `offset` of `path` and returns the count; with
/// `out` null it returns the file size (0 when missing).
using RomfsRead = std::function<std::size_t(const std::string& path, std::uint64_t offset, void* out,
                                            std::size_t size)>;

/// RGBA8 texels, `slices` images of w x h one after the other, stored row order (row 0 = v 0).
struct Texture {
    std::uint32_t w = 0, h = 0, slices = 0;
    bool srgb = false;
    bool repeat = false;
    std::vector<std::uint8_t> rgba;
    bool Empty() const { return w == 0 || h == 0 || slices == 0; }
};

/// One opened serialized file (the main CAB of a bundle).
struct File {
    std::string bundle_name;
    std::string cab;
    std::shared_ptr<bc_unity::LazyBundle> bundle;
    bc_unity::BundleNode node;
    std::optional<bc_unity::SerializedFile> sf;

    /// Reads a streamed resource ("archive:/CAB-x/CAB-x.resS").
    bool ReadStream(const std::string& path, std::uint64_t offset, std::size_t size, std::uint8_t* out) const;
    /// The CAB name an external FileID points to ("" when none).
    std::string ExternalCab(std::int64_t file_id) const;
};

/// The game's bundles in romfs, opened on demand.
class Assets {
public:
    Assets(RomfsRead read, std::vector<std::string> bundle_names);

    /// The main serialized file of a bundle, by file name (cached).
    std::shared_ptr<File> Open(const std::string& bundle_name);
    /// The serialized file named `cab`, looked for in the bundles whose names start with one of
    /// `prefixes` (cached).
    std::shared_ptr<File> OpenCab(const std::string& cab, const std::vector<std::string>& prefixes);
    /// The first bundle name starting with `prefix`, or "".
    std::string Find(std::string_view prefix) const;
    std::vector<std::string> FindAll(std::string_view prefix) const;
    /// Closes every opened file (their block caches and metadata); they reopen on demand.
    void Trim();

    /// Resolves a PPtr {FileID, PathID} seen in `from`.
    struct Ref {
        std::shared_ptr<File> file;
        const bc_unity::ObjectInfo* obj = nullptr;
        explicit operator bool() const { return file && obj; }
    };
    Ref Resolve(const std::shared_ptr<File>& from, const bc_unity::Value* pptr,
                const std::vector<std::string>& prefixes);

private:
    RomfsRead read_;
    std::vector<std::string> names_;
    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<File>> by_bundle_;
    std::unordered_map<std::string, std::shared_ptr<File>> by_cab_;
    std::unordered_map<std::string, std::vector<std::string>> nodes_; // bundle -> node paths
};

/// Decoders the loader needs from the host.
struct Codecs {
    bc_tex::AstcFn astc;
};

/// A sprite, cropped to its rect, with its geometry in world units at scale 1.
struct Sprite {
    Texture tex;              // the sprite rect, straight alpha
    float width = 0, height = 0; // world units at scale 1 (rect size / pixels per unit)
    float pivot_x = 0.5f, pivot_y = 0.5f;
    float border[4]{}; // 9-slice border in pixels: left, bottom, right, top
};

struct Tile {
    float x = 0, y = 0, w = 128, h = 128; // world bounds
    std::uint32_t slice = 0;
};

struct Zone {
    std::string room_id; // "World_Biome_01_..._uid59"
    std::int32_t room_uid = -1;
    std::int32_t zone_id = 0;
    bool culled = false;
    std::vector<bc_fog::Shape> shapes;
};

struct District {
    std::string name; // display name ("" when unknown)
    std::int32_t x = 0, y = 0, w = 0, h = 0; // world chunk bounds
    std::vector<std::uint8_t> active;         // w * h, row-major from the bottom row
};

/// When an icon is drawn.
enum class IconRule : std::uint8_t {
    Always,            // decoration
    ZoneEntered,       // the item's exploration zone was entered
    ZDoorDiscovered,   // the Z-door was seen but not used
    ZDoorEntered,      // the Z-door was used
    LastSave,          // the item's room is the last save room
    NotActivated,      // the gameplay element was not activated yet (e.g. a barricade)
};

struct Icon {
    std::string sprite;
    float x = 0, y = 0, z = 0;     // world position of the sprite's pivot
    float sx = 1, sy = 1;          // world scale (negative = mirrored)
    IconRule rule = IconRule::ZoneEntered;
    std::string room_id;
    std::int32_t room_uid = -1, zone_id = 0, zdoor_id = -1;
    std::string guid; // activable element
    bool unrevealed_ok = false; // visibleInUnrevealedMap
};

/// A secret zone: until the player finds it, the game covers its rect with the biome's
/// "_secret" map texture (which draws the secret room as wall). `tex` holds that texture's
/// texels over the rect plus a margin, on the tiles' texel grid.
struct Secret {
    std::string room_id;
    std::int32_t secret_id = 0;
    float x = 0, y = 0, w = 0, h = 0; // the covered rect (world)
    float tx = 0, ty = 0;             // world position of the patch's corner (texel grid)
    float texel = 0.25f;              // world units per texel
    Texture tex;
};

/// A part of the map the game's map camera stays in (the main map, a tower, Z-rooms...).
struct NavZone {
    std::string name;
    float x = 0, y = 0, w = 0, h = 0;
};

struct LevelMap {
    int number = 0;          // biome number (1 = Streets Poor)
    std::string world_name;  // "World_Biome_01_Streets_Poor"
    std::string level_name;  // "Paris Streets"
    std::string level_guid;  // the map's LevelID (LevelMapContext.level), to match live data
    std::vector<Tile> tiles;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // union of the tiles
    Texture data;       // LevelMap_<NN>_img slices
    Texture silhouette; // biome shape slices (may be empty)
    Texture parchment;  // tileable background
    Texture noise;      // noise (repeat)
    bc_shade::Params params;
    std::vector<Zone> zones;
    float smoothness = 3.0f;
    std::vector<District> districts;
    std::int32_t chunk_w = 24, chunk_h = 13;
    std::vector<Icon> icons;
    std::map<std::string, Sprite> sprites; // the icons' sprites
    std::vector<NavZone> nav_zones;
    int main_zone = -1;
    std::vector<Secret> secrets;
    std::string log;

    const Tile* TileAt(float wx, float wy) const;
    /// The smallest navigation zone holding the point, else the main one (-1 when none).
    int NavZoneAt(float wx, float wy) const;
    /// The district whose active chunk holds the world point, or null.
    const District* DistrictAt(float wx, float wy) const;
};

/// Shared textures (parchment, noise), the player marker and the title font.
struct Glyph {
    std::uint32_t code = 0;
    float x = 0, y = 0, w = 0, h = 0;       // atlas rect (pixels)
    float bx = 0, by = 0, advance = 0;       // bearing and advance (pixels at the face size)
};
struct Font {
    Texture atlas; // Alpha8 SDF atlas, alpha channel
    float point_size = 0, line_height = 0, ascender = 0, padding = 0;
    std::unordered_map<std::uint32_t, Glyph> glyphs;
    bool Empty() const { return atlas.Empty() || glyphs.empty(); }
};

struct Common {
    Sprite player;
    std::map<std::string, Sprite> ui; // map screen frame, banner, buttons, zoom scale, markers
    /// Map marker types: MapMarkerTypeID._guid -> its icon sprite in `ui` ("Map_MarkerSet_Cross").
    std::map<std::string, std::string> marker_types;
    Font font;
    std::string log;
};

/// Display names from the English string tables: key id -> text.
class Strings {
public:
    void Load(Assets& assets);
    std::string Get(std::int64_t key) const;
    bool Empty() const { return map_.empty(); }

private:
    std::unordered_map<std::int64_t, std::string> map_;
};

/// Level and district display names.
struct Names {
    Strings strings;
    std::unordered_map<std::string, std::string> levels;    // "World_Biome_01_Streets_Poor" -> "Paris Streets"
    std::unordered_map<std::string, bc_shade::Vec4> banner_colors; // the level's map banner tint (sRGB)
    std::unordered_map<std::string, std::string> districts; // DistrictData name -> display name
};

/// The map scene bundle of a biome ("World_Biome_01_Streets_Poor" -> 1).
int BiomeNumber(std::string_view world_name);

std::shared_ptr<const Names> LoadNames(Assets& assets);
std::shared_ptr<const Common> LoadCommon(Assets& assets, const Codecs& codecs);
/// Null on failure; `log` (optional) receives the loader's log either way.
std::shared_ptr<const LevelMap> LoadLevelMap(Assets& assets, const Codecs& codecs, const Names& names,
                                             std::string_view world_name, std::string* log = nullptr);

} // namespace bc_levelmap
