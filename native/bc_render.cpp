// SPDX-License-Identifier: GPL-3.0-or-later
// Map raster. See bc_render.h.

#include "bc_render.h"

#include <algorithm>
#include <cmath>

namespace bc_render {

using bc_world::Biome;
using bc_world::Room;

namespace {

// Window widths, in chunks, per zoom level (zoom 0 fits the whole biome).
constexpr int kWindowChunks[] = {0, 14, 8};

struct Rgba {
    std::uint8_t r, g, b, a;
};

constexpr Rgba kAir{58, 49, 82, 255};
constexpr Rgba kAirCurrent{92, 78, 132, 255};
constexpr Rgba kAirSave{44, 76, 118, 255};
constexpr Rgba kAirBoss{112, 44, 48, 255};
constexpr Rgba kAirTravel{40, 96, 78, 255};
constexpr Rgba kEdge{204, 166, 86, 255};
constexpr Rgba kEdgeCurrent{246, 214, 132, 255};
constexpr Rgba kAirFog{44, 42, 52, 150};
constexpr Rgba kEdgeFog{96, 88, 70, 150};

// One tile of the biome grid: 0 = outside every room, else room index + 1 in the low 12 bits,
// bit 14 = explored (filled per fog), bit 15 = open (air).
constexpr std::uint16_t kAirBit = 0x8000;
constexpr std::uint16_t kSeenBit = 0x4000;
constexpr std::uint16_t kRoomMask = 0x0FFF;

struct Grid {
    int w = 0, h = 0;
    std::vector<std::uint16_t> cells;
    std::uint16_t At(int x, int y) const {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return 0;
        return cells[static_cast<std::size_t>(y) * w + x];
    }
};

Grid BuildGrid(const Biome& b, const Fog& fog, bool show_unexplored) {
    Grid g;
    const int cw = b.chunk_w, chh = b.chunk_h;
    if (b.max_cx < b.min_cx || cw <= 0 || chh <= 0)
        return g;
    g.w = (b.max_cx - b.min_cx + 1) * cw;
    g.h = (b.max_cy - b.min_cy + 1) * chh;
    g.cells.assign(static_cast<std::size_t>(g.w) * g.h, 0);
    for (std::size_t ri = 0; ri < b.rooms.size() && ri < kRoomMask; ++ri) {
        const Room& r = b.rooms[ri];
        if (r.chunk_w != cw || r.chunk_h != chh)
            continue;
        for (int i = 0; i < r.w * r.h; ++i) {
            const auto& ch = r.chunks[static_cast<std::size_t>(i)];
            if (!ch.enabled)
                continue;
            const bool seen = ChunkExplored(fog, r.uid, static_cast<std::size_t>(i));
            if (!seen && !show_unexplored)
                continue;
            const int ox = (r.pos_x + i % r.w - b.min_cx) * cw;
            const int oy = (r.pos_y + i / r.w - b.min_cy) * chh;
            for (int ty = 0; ty < chh; ++ty) {
                for (int tx = 0; tx < cw; ++tx) {
                    const int gx = ox + tx, gy = oy + ty;
                    if (gx < 0 || gy < 0 || gx >= g.w || gy >= g.h)
                        continue;
                    std::uint16_t cell = static_cast<std::uint16_t>(ri + 1);
                    if (ch.collisions[static_cast<std::size_t>(ty) * cw + tx] == 0)
                        cell |= kAirBit;
                    if (seen)
                        cell |= kSeenBit;
                    g.cells[static_cast<std::size_t>(gy) * g.w + gx] = cell;
                }
            }
        }
    }
    return g;
}

// Priority of what a tile contributes to an output pixel: higher wins when several tiles fall
// into one pixel (zoomed-out views).
int Classify(const Grid& g, int x, int y, std::uint16_t& cell_out, bool& edge_out) {
    const std::uint16_t c = g.At(x, y);
    cell_out = c;
    edge_out = false;
    if (c == 0)
        return 0;
    if (c & kAirBit)
        return 2;
    // A solid tile is outline when an open tile of any room touches it.
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if ((dx || dy) && (g.At(x + dx, y + dy) & kAirBit)) {
                edge_out = true;
                return 3;
            }
        }
    }
    return 0; // solid interior: transparent
}

} // namespace

bool ChunkExplored(const Fog& fog, std::int32_t uid, std::size_t index) {
    if (!fog.known)
        return true;
    const auto it = fog.by_uid.find(uid);
    if (it == fog.by_uid.end() || index >= it->second.size())
        return false;
    return it->second[index] >= 2;
}

View ComputeView(const Biome& b, const Params& p) {
    View v;
    const int cw = b.chunk_w, chh = b.chunk_h;
    const double gw = (b.max_cx - b.min_cx + 1) * cw;
    const double gh = (b.max_cy - b.min_cy + 1) * chh;
    if (gw <= 0 || gh <= 0 || p.out_w <= 0 || p.out_h <= 0)
        return v;
    const int zoom = std::clamp(p.zoom, 0, 2);
    if (zoom == 0) {
        // Whole biome with a one-chunk margin, centred.
        const double mw = gw + 2.0 * cw, mh = gh + 2.0 * chh;
        v.scale = std::min(p.out_w / mw, p.out_h / mh);
        v.x0 = gw / 2.0 - p.out_w / (2.0 * v.scale);
        v.y1 = gh / 2.0 + p.out_h / (2.0 * v.scale);
        return v;
    }
    const double tiles_w = kWindowChunks[zoom] * cw;
    v.scale = p.out_w / tiles_w;
    const double cx = (p.center_cx - b.min_cx + 0.5) * cw;
    const double cy = (p.center_cy - b.min_cy + 0.5) * chh;
    v.x0 = cx - p.out_w / (2.0 * v.scale);
    v.y1 = cy + p.out_h / (2.0 * v.scale);
    return v;
}

std::vector<std::uint8_t> Render(const Biome& b, const Fog& fog, const Params& p) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(p.out_w) * p.out_h * 4, 0);
    const Grid g = BuildGrid(b, fog, p.show_unexplored);
    if (g.w == 0)
        return px;
    const View v = ComputeView(b, p);
    if (v.scale <= 0)
        return px;
    int current_index = -1;
    for (std::size_t i = 0; i < b.rooms.size(); ++i) {
        if (b.rooms[i].uid == p.current_uid)
            current_index = static_cast<int>(i);
    }
    const double inv = 1.0 / v.scale;
    for (int oy = 0; oy < p.out_h; ++oy) {
        const double ty_top = v.y1 - oy * inv;
        const double ty_bot = v.y1 - (oy + 1) * inv;
        int y_hi = static_cast<int>(std::floor(ty_top - 1e-9));
        int y_lo = static_cast<int>(std::floor(ty_bot));
        if (y_lo > y_hi)
            y_lo = y_hi;
        for (int ox = 0; ox < p.out_w; ++ox) {
            const double tx_l = v.x0 + ox * inv;
            const double tx_r = v.x0 + (ox + 1) * inv;
            int x_lo = static_cast<int>(std::floor(tx_l));
            int x_hi = static_cast<int>(std::floor(tx_r - 1e-9));
            if (x_hi < x_lo)
                x_hi = x_lo;
            if (x_hi < 0 || y_hi < 0 || x_lo >= g.w || y_lo >= g.h)
                continue;
            int best = 0;
            std::uint16_t best_cell = 0;
            bool best_edge = false;
            for (int ty = std::max(y_lo, 0); ty <= std::min(y_hi, g.h - 1); ++ty) {
                for (int tx = std::max(x_lo, 0); tx <= std::min(x_hi, g.w - 1); ++tx) {
                    std::uint16_t cell;
                    bool edge;
                    const int pr = Classify(g, tx, ty, cell, edge);
                    if (pr > best) {
                        best = pr;
                        best_cell = cell;
                        best_edge = edge;
                    }
                }
            }
            if (best == 0)
                continue;
            const bool seen = (best_cell & kSeenBit) != 0;
            const int room = static_cast<int>(best_cell & kRoomMask) - 1;
            const bool current = room == current_index;
            Rgba c;
            if (best_edge) {
                c = !seen ? kEdgeFog : current ? kEdgeCurrent : kEdge;
            } else {
                // An outline tile next to an open tile of another room may carry that room's
                // index; colour the fill by the room the open tile belongs to.
                const Room& r = b.rooms[static_cast<std::size_t>(room)];
                if (!seen)
                    c = kAirFog;
                else if (current)
                    c = kAirCurrent;
                else if (r.boss_room)
                    c = kAirBoss;
                else if (r.save_room)
                    c = kAirSave;
                else if (r.fast_travel_room)
                    c = kAirTravel;
                else
                    c = kAir;
            }
            std::uint8_t* d = &px[(static_cast<std::size_t>(oy) * p.out_w + ox) * 4];
            d[0] = c.r;
            d[1] = c.g;
            d[2] = c.b;
            d[3] = c.a;
        }
    }
    return px;
}

} // namespace bc_render
