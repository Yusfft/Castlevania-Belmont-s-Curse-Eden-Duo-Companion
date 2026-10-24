// SPDX-License-Identifier: GPL-3.0-or-later
//
// Draws the game's level map on the CPU: the map tiles shaded like the game's RW_LevelMap
// material (bc_shade), the fog of war from the explored zones (bc_fog), the map icons, the
// player marker and the area title, all from the data bc_levelmap read from the player's romfs.
//
// Coordinates: world units (Unity), y up. A view maps them to output pixels (y down).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bc_fog.h"
#include "bc_levelmap.h"

namespace bc_mapdraw {

/// What the player has explored, as the game keeps it per room.
struct Exploration {
    struct Room {
        std::unordered_set<std::int32_t> zones, zdoors, zdoor_zones, secrets;
        std::unordered_set<std::string> activated;
    };
    /// By room id ("World_Biome_01_..._uid59").
    std::unordered_map<std::string, Room> rooms;
    std::string last_save_room;
    /// Markers the player placed, on every map ("level guid" says which).
    struct Marker {
        std::string level_guid, type_guid;
        float x = 0, y = 0;
    };
    std::vector<Marker> markers;
    /// The last death spot (the map whose level guid matches shows it).
    bool has_death = false;
    std::string death_level_guid;
    float death_x = 0, death_y = 0;
    /// No exploration data: reveal this room (the player is in it) and nothing else.
    std::string current_room_only;
    bool known = false; // false: nothing is revealed

    /// By id; a room id from another source with the same world and uid also matches.
    const Room* Find(const std::string& room_id) const;
};

/// The revealed part of one level map: the fog field and which icons show.
struct Reveal {
    bc_fog::Field field;
    std::vector<std::uint8_t> icon_visible;            // parallel to LevelMap::icons
    std::vector<const bc_levelmap::Secret*> hidden;     // secret zones not found yet
    std::size_t zones = 0;
};

std::shared_ptr<const Reveal> BuildReveal(const bc_levelmap::LevelMap& map, const Exploration& ex);

struct View {
    double x0 = 0;    // world x at the left edge
    double y1 = 0;    // world y at the top edge
    double scale = 1; // output pixels per world unit
    int w = 0, h = 0;

    void WorldToPixel(double wx, double wy, double& px, double& py) const {
        px = (wx - x0) * scale;
        py = (y1 - wy) * scale;
    }
};

/// A view of `w` x `h` pixels centred on a world point.
View CenteredView(double cx, double cy, double scale, int w, int h);
/// A view showing the whole map in `w` x `h` pixels.
View FitView(const bc_levelmap::LevelMap& map, int w, int h, double margin = 24);
/// A view showing navigation zone `zone` (the whole map when it is not one).
View FitZone(const bc_levelmap::LevelMap& map, int zone, int w, int h, double margin = 24);

/// An icon that is not part of the map's own items (a marker, the death spot).
struct ExtraIcon {
    const bc_levelmap::Sprite* sprite = nullptr;
    bc_levelmap::Icon icon; // position and scale; the rule is ignored
};

struct RenderOptions {
    int threads = 3;
    /// Drawn over the map's icons, in order.
    std::vector<ExtraIcon> extra;
    /// Icons are drawn at least this many pixels tall (the whole-area view is small).
    float min_icon_px = 0;
    /// Rooms in the game's "selected" palette (the vivid purple the map uses under its cursor)
    /// instead of the muted standard one.
    float selected = 1.0f;
};

/// Straight RGBA8, view.w * view.h * 4 bytes, top row first; opaque (black outside the map).
std::vector<std::uint8_t> Render(const bc_levelmap::LevelMap& map, const Reveal& reveal, const View& view,
                                 const RenderOptions& opt = {});

/// A sprite drawn into `w` x `h` pixels (fit, centred), straight RGBA8.
std::vector<std::uint8_t> RenderSprite(const bc_levelmap::Sprite& sprite, int w, int h);

/// The area title: `title` large, `subtitle` below it, small caps in the game's font, gold
/// with a dark shadow, left aligned, on a transparent `w` x `h` image.
std::vector<std::uint8_t> RenderTitle(const bc_levelmap::Font& font, const std::string& title,
                                      const std::string& subtitle, int w, int h);

} // namespace bc_mapdraw
