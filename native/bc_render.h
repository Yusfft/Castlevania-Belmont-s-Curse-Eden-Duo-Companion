// SPDX-License-Identifier: GPL-3.0-or-later
//
// Map raster for one biome: rooms drawn from their collision grids (open tiles filled, solid
// tiles next to open ones drawn as the outline), with optional chunk fog.
#pragma once

#include <cstdint>
#include <memory>
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

struct Params {
    int zoom = 0;            // 0 whole biome, 1 wide window, 2 close window
    int center_cx = 0;       // window centre in biome chunks (zoom > 0)
    int center_cy = 0;
    int current_uid = -1;    // highlighted room
    bool show_unexplored = false;
    int out_w = 1240;
    int out_h = 900;
};

/// Biome tile space: x right, y up, origin at the biome's (min_cx, min_cy) chunk corner.
struct View {
    double x0 = 0;    // tile x at the left edge
    double y1 = 0;    // tile y at the top edge
    double scale = 1; // output px per tile
};

View ComputeView(const bc_world::Biome& biome, const Params& p);

/// Output pixel of a biome-space tile coordinate.
inline void TileToPixel(const View& v, double tx, double ty, double& px, double& py) {
    px = (tx - v.x0) * v.scale;
    py = (v.y1 - ty) * v.scale;
}

/// Straight RGBA8, out_w * out_h * 4 bytes, top row first.
std::vector<std::uint8_t> Render(const bc_world::Biome& biome, const Fog& fog, const Params& p);

/// Whether chunk `index` of room `uid` counts as explored under `fog`.
bool ChunkExplored(const Fog& fog, std::int32_t uid, std::size_t index);

} // namespace bc_render
