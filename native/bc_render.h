// SPDX-License-Identifier: GPL-3.0-or-later
//
// Map raster for one biome, in the style of the game's own level map: explored rooms are a
// dark, grained fill with a thin light outline; the edge of the explored part is drawn in
// gold. Shapes come from the rooms' collision grids, interpolated between tile centres
// (marching-squares style), so walls get chamfered corners instead of tile steps and the
// outline stays a constant width at every zoom.
//
// Coordinates: Unity world units are tiles; world = room chunk origin * chunk size + local.
// "Biome tiles" are world tiles shifted so the biome's lowest chunk corner is (0, 0); y is up.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "bc_world.h"

namespace bc_render {

/// Per-room chunk exploration, as the game keeps it (RoomChunkExplorationState):
/// 0 NoChunk, 1 Unexplored, 2 ExploredTemporary, 3 Validated. Index = x + y * room width.
struct Fog {
    bool known = false; // false: no exploration data, draw every room
    std::unordered_map<std::int32_t, std::vector<std::uint8_t>> by_uid;
};

/// Pixels per tile at each zoom level; zoom 0 fits the whole biome instead.
inline constexpr double kZoomScale[] = {0.0, 7.0, 14.0};
inline constexpr int kZoomLevels = 3;

struct Params {
    int zoom = 2;           // 0 whole biome, 1 medium, 2 close
    double center_x = 0;    // view centre in biome tiles (zoom > 0)
    double center_y = 0;
    int out_w = 2048;
    int out_h = 1536;
};

struct View {
    double x0 = 0;    // biome tile x at the left edge
    double y1 = 0;    // biome tile y at the top edge
    double scale = 1; // output px per tile
};

View ComputeView(const bc_world::Biome& biome, const Params& p);

inline void TileToPixel(const View& v, double tx, double ty, double& px, double& py) {
    px = (tx - v.x0) * v.scale;
    py = (v.y1 - ty) * v.scale;
}

/// World position (Unity units) to biome tiles.
inline void WorldToTile(const bc_world::Biome& b, double wx, double wy, double& tx, double& ty) {
    tx = wx - static_cast<double>(b.min_cx) * b.chunk_w;
    ty = wy - static_cast<double>(b.min_cy) * b.chunk_h;
}

/// Straight RGBA8, out_w * out_h * 4 bytes, top row first. Transparent outside the rooms.
std::vector<std::uint8_t> Render(const bc_world::Biome& biome, const Fog& fog, const Params& p);

/// The player marker: a gold ring around a deep blue disc, `size` x `size` RGBA.
std::vector<std::uint8_t> RenderPlayerIcon(int size);

bool ChunkExplored(const Fog& fog, std::int32_t uid, std::size_t index);

} // namespace bc_render
