// SPDX-License-Identifier: GPL-3.0-or-later
// Map raster. See bc_render.h.

#include "bc_render.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace bc_render {

using bc_world::Biome;
using bc_world::Room;

namespace {

// Tile kinds in the biome grid.
enum : std::uint8_t {
    kNone = 0,       // outside every room
    kSolid = 1,      // explored, solid
    kAir = 2,        // explored, open
    kHidden = 3,     // a room chunk the player has not explored
};

struct Grid {
    int w = 0, h = 0;
    std::vector<std::uint8_t> cells;
    std::uint8_t At(int x, int y) const {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return kNone;
        return cells[static_cast<std::size_t>(y) * w + x];
    }
    // Interpolation value at a tile centre: 1 for open, 0 otherwise.
    float V(int x, int y) const { return At(x, y) == kAir ? 1.0f : 0.0f; }
};

Grid BuildGrid(const Biome& b, const Fog& fog) {
    Grid g;
    const int cw = b.chunk_w, chh = b.chunk_h;
    if (b.max_cx < b.min_cx || cw <= 0 || chh <= 0)
        return g;
    g.w = (b.max_cx - b.min_cx + 1) * cw;
    g.h = (b.max_cy - b.min_cy + 1) * chh;
    g.cells.assign(static_cast<std::size_t>(g.w) * g.h, kNone);
    for (const Room& r : b.rooms) {
        if (r.chunk_w != cw || r.chunk_h != chh)
            continue;
        for (int i = 0; i < r.w * r.h; ++i) {
            const auto& ch = r.chunks[static_cast<std::size_t>(i)];
            if (!ch.enabled)
                continue;
            const bool seen = ChunkExplored(fog, r.uid, static_cast<std::size_t>(i));
            const int ox = (r.pos_x + i % r.w - b.min_cx) * cw;
            const int oy = (r.pos_y + i / r.w - b.min_cy) * chh;
            for (int ty = 0; ty < chh; ++ty) {
                for (int tx = 0; tx < cw; ++tx) {
                    const int gx = ox + tx, gy = oy + ty;
                    if (gx < 0 || gy < 0 || gx >= g.w || gy >= g.h)
                        continue;
                    std::uint8_t cell = kHidden;
                    if (seen)
                        cell = ch.collisions[static_cast<std::size_t>(ty) * cw + tx] == 0 ? kAir : kSolid;
                    g.cells[static_cast<std::size_t>(gy) * g.w + gx] = cell;
                }
            }
        }
    }
    return g;
}

// Cheap deterministic value noise for the parchment grain.
inline float Hash(int x, int y) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}
float Grain(double u, double v) {
    // Two octaves, in tile space so the grain stays put while the map scrolls.
    float n = 0, amp = 0.6f, freq = 1.5f;
    for (int o = 0; o < 2; ++o) {
        const double x = u * freq, y = v * freq;
        const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
        const float fx = static_cast<float>(x - xi), fy = static_cast<float>(y - yi);
        const float a = Hash(xi, yi), bb = Hash(xi + 1, yi), c = Hash(xi, yi + 1), d = Hash(xi + 1, yi + 1);
        const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
        n += amp * ((a * (1 - sx) + bb * sx) * (1 - sy) + (c * (1 - sx) + d * sx) * sy);
        amp *= 0.5f;
        freq *= 3.1f;
    }
    return n / 0.9f; // ~0..1
}

struct Rgb {
    float r, g, b;
};
constexpr Rgb kFill{40.f, 33.f, 47.f};      // room interior
constexpr Rgb kOutline{150.f, 140.f, 158.f}; // walls
constexpr Rgb kFogEdge{214.f, 168.f, 82.f};  // edge of the explored part

inline void Blend(std::uint8_t* d, const Rgb& c, float a) {
    a = std::clamp(a, 0.0f, 1.0f);
    const float da = d[3] / 255.0f;
    const float oa = a + da * (1 - a);
    if (oa <= 0)
        return;
    const auto mix = [&](int i, float src) {
        const float dst = d[i];
        d[i] = static_cast<std::uint8_t>(std::clamp((src * a + dst * da * (1 - a)) / oa, 0.0f, 255.0f));
    };
    mix(0, c.r);
    mix(1, c.g);
    mix(2, c.b);
    d[3] = static_cast<std::uint8_t>(std::clamp(oa * 255.0f, 0.0f, 255.0f));
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
    const double gw = (b.max_cx - b.min_cx + 1) * static_cast<double>(b.chunk_w);
    const double gh = (b.max_cy - b.min_cy + 1) * static_cast<double>(b.chunk_h);
    if (gw <= 0 || gh <= 0 || p.out_w <= 0 || p.out_h <= 0)
        return v;
    const int zoom = std::clamp(p.zoom, 0, kZoomLevels - 1);
    if (zoom == 0) {
        const double mw = gw + 2.0 * b.chunk_w, mh = gh + 2.0 * b.chunk_h;
        v.scale = std::min(p.out_w / mw, p.out_h / mh);
        v.x0 = gw / 2.0 - p.out_w / (2.0 * v.scale);
        v.y1 = gh / 2.0 + p.out_h / (2.0 * v.scale);
        return v;
    }
    v.scale = kZoomScale[zoom];
    v.x0 = p.center_x - p.out_w / (2.0 * v.scale);
    v.y1 = p.center_y + p.out_h / (2.0 * v.scale);
    return v;
}

std::vector<std::uint8_t> Render(const Biome& b, const Fog& fog, const Params& p) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(p.out_w) * p.out_h * 4, 0);
    const Grid g = BuildGrid(b, fog);
    if (g.w == 0)
        return px;
    const View v = ComputeView(b, p);
    if (v.scale <= 0)
        return px;
    const double inv = 1.0 / v.scale;
    // Line widths in output pixels; thinner when the whole biome is shown.
    const float wall_w = v.scale >= 4 ? 1.6f : 1.0f;
    const float fog_w = v.scale >= 4 ? 2.4f : 1.4f;
    for (int oy = 0; oy < p.out_h; ++oy) {
        const double ty = v.y1 - (oy + 0.5) * inv;
        // Bilinear cell between tile centres: centres sit at k + 0.5.
        const double cy = ty - 0.5;
        const int y0 = static_cast<int>(std::floor(cy));
        const float fy = static_cast<float>(cy - y0);
        if (y0 < -1 || y0 >= g.h)
            continue;
        for (int ox = 0; ox < p.out_w; ++ox) {
            const double tx = v.x0 + (ox + 0.5) * inv;
            const double cx = tx - 0.5;
            const int x0 = static_cast<int>(std::floor(cx));
            if (x0 < -1 || x0 >= g.w)
                continue;
            const float fx = static_cast<float>(cx - x0);
            const float a = g.V(x0, y0), bb = g.V(x0 + 1, y0), c = g.V(x0, y0 + 1), d = g.V(x0 + 1, y0 + 1);
            if (a + bb + c + d == 0)
                continue; // no open tile around: solid, hidden or outside
            const float f = (a * (1 - fx) + bb * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
            // Gradient in tile units, then distance to the f = 0.5 contour in output pixels.
            const float gx = (bb - a) * (1 - fy) + (d - c) * fy;
            const float gy = (c - a) * (1 - fx) + (d - bb) * fx;
            const float gl = std::sqrt(gx * gx + gy * gy);
            const float dist = gl > 1e-4f ? (f - 0.5f) / gl * static_cast<float>(v.scale) : 1e3f;
            std::uint8_t* out = &px[(static_cast<std::size_t>(oy) * p.out_w + ox) * 4];
            // Interior fill, anti-aliased at the contour.
            if (dist > -0.5f) {
                const float grain = Grain(tx, ty);
                const float k = 0.82f + 0.30f * grain;
                Blend(out, Rgb{kFill.r * k, kFill.g * k, kFill.b * k}, std::min(1.0f, dist + 0.5f));
            }
            // Outline: which side is beyond the contour decides wall or fog edge.
            const float edge_w = std::max(wall_w, fog_w);
            if (std::fabs(dist) < edge_w + 1.0f && gl > 1e-4f) {
                // Step one tile against the gradient (towards the closed side).
                const int sx = static_cast<int>(std::floor(tx - gx / gl * 0.9));
                const int sy = static_cast<int>(std::floor(ty - gy / gl * 0.9));
                const bool fog_side = g.At(sx, sy) == kHidden;
                const float w = fog_side ? fog_w : wall_w;
                const float alpha = std::clamp(w + 0.5f - std::fabs(dist), 0.0f, 1.0f);
                if (alpha > 0)
                    Blend(out, fog_side ? kFogEdge : kOutline, alpha);
            }
        }
    }
    return px;
}

std::vector<std::uint8_t> RenderPlayerIcon(int size) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(size) * size * 4, 0);
    const float fs = static_cast<float>(size);
    const float c = fs / 2.0f;
    const float r_out = fs / 2.0f - 1.0f;
    const float ring = std::max(2.0f, fs / 9.0f);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - c, dy = static_cast<float>(y) + 0.5f - c;
            const float d = std::sqrt(dx * dx + dy * dy);
            std::uint8_t* o = &px[(static_cast<std::size_t>(y) * size + x) * 4];
            // Soft dark halo, gold ring, blue disc, light core.
            Blend(o, Rgb{0, 0, 0}, std::clamp((r_out + 1.0f - d) * 0.5f, 0.0f, 0.55f));
            Blend(o, Rgb{226, 182, 92}, std::clamp(r_out - d, 0.0f, 1.0f));
            Blend(o, Rgb{28, 52, 120}, std::clamp(r_out - ring - d, 0.0f, 1.0f));
            Blend(o, Rgb{120, 170, 240}, std::clamp(fs / 7.0f - d, 0.0f, 1.0f));
        }
    }
    return px;
}

} // namespace bc_render
