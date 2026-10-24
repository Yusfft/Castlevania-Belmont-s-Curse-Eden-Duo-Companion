// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fog of war distance field for the level map.
//
// The game reveals the map by exploration zones; each zone is a list of shapes (axis-aligned
// boxes and quads, in world units). The fog value of a point is the smooth union of the signed
// distances to the shapes of every revealed zone near it (negative or zero inside, growing
// outside), so the boundary of the explored area is round and soft. The map shading turns that
// distance into the torn gold edge bands.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace bc_fog {

struct Shape {
    enum Kind : std::uint8_t { Box, Quad } kind = Box;
    // Box: centre (v[0], v[1]) and half size (v[2], v[3]).
    // Quad: four corners (v[0], v[1]) .. (v[6], v[7]).
    float v[8]{};
    // Bounds, for culling.
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

inline Shape MakeBox(float cx, float cy, float hx, float hy) {
    Shape s;
    s.kind = Shape::Box;
    s.v[0] = cx, s.v[1] = cy, s.v[2] = hx, s.v[3] = hy;
    s.x0 = cx - hx, s.x1 = cx + hx, s.y0 = cy - hy, s.y1 = cy + hy;
    return s;
}

inline Shape MakeQuad(const float* xy) {
    Shape s;
    s.kind = Shape::Quad;
    for (int i = 0; i < 8; ++i)
        s.v[i] = xy[i];
    s.x0 = std::min({xy[0], xy[2], xy[4], xy[6]});
    s.x1 = std::max({xy[0], xy[2], xy[4], xy[6]});
    s.y0 = std::min({xy[1], xy[3], xy[5], xy[7]});
    s.y1 = std::max({xy[1], xy[3], xy[5], xy[7]});
    return s;
}

/// Signed distance to an axis-aligned box.
inline float BoxDistance(const Shape& s, float px, float py) {
    const float qx = std::fabs(px - s.v[0]) - s.v[2];
    const float qy = std::fabs(py - s.v[1]) - s.v[3];
    const float m = std::max(qx, qy);
    if (m > 0.0f) {
        const float ax = std::max(qx, 0.0f), ay = std::max(qy, 0.0f);
        return std::sqrt(ax * ax + ay * ay);
    }
    return m;
}

/// Distance to a quad (corners in order). The unsigned part is the distance to the nearest
/// edge; the sign comes from the edge side tests, normalised by the winding of the corners.
/// A quad with a repeated corner (a triangle) gives 0 inside.
inline float QuadDistance(const Shape& s, float px, float py) {
    const float* c = s.v;
    float best = std::numeric_limits<float>::max();
    float side = -std::numeric_limits<float>::max();
    const float e0x = c[2] - c[0], e0y = c[3] - c[1];
    const float e3x = c[0] - c[6], e3y = c[1] - c[7];
    const float orient = e0y * e3x - e0x * e3y;
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) & 3;
        const float ax = c[2 * i], ay = c[2 * i + 1];
        const float ex = c[2 * j] - ax, ey = c[2 * j + 1] - ay;
        const float vx = px - ax, vy = py - ay;
        const float ee = ex * ex + ey * ey;
        const float t = ee > 0.0f ? std::clamp((vx * ex + vy * ey) / ee, 0.0f, 1.0f) : 0.0f;
        const float qx = vx - ex * t, qy = vy - ey * t;
        best = std::min(best, qx * qx + qy * qy);
        side = std::max(side, orient * (vx * ey - vy * ex));
    }
    const float sign = side > 0.0f ? 1.0f : (side < 0.0f ? -1.0f : 0.0f);
    return std::sqrt(best) * sign;
}

/// Polynomial smooth minimum (radius k).
inline float SmoothMin(float a, float b, float k) {
    const float h = std::max(k - std::fabs(a - b), 0.0f);
    return std::min(a, b) - h * h * 0.25f / k;
}

/// A set of revealed shapes with a coarse grid for fast lookups.
class Field {
public:
    /// `smoothness` is the union radius; `reach` is how far from a shape its distance matters.
    void Build(std::vector<Shape> shapes, float smoothness, float reach) {
        shapes_ = std::move(shapes);
        k_ = smoothness > 0 ? smoothness : 1.0f;
        reach_ = reach;
        cells_.clear();
        if (shapes_.empty())
            return;
        gx0_ = shapes_[0].x0, gy0_ = shapes_[0].y0;
        float gx1 = shapes_[0].x1, gy1 = shapes_[0].y1;
        for (const Shape& s : shapes_) {
            gx0_ = std::min(gx0_, s.x0), gy0_ = std::min(gy0_, s.y0);
            gx1 = std::max(gx1, s.x1), gy1 = std::max(gy1, s.y1);
        }
        gx0_ -= reach + kCell, gy0_ -= reach + kCell;
        gx1 += reach + kCell, gy1 += reach + kCell;
        gw_ = std::max(1, static_cast<int>((gx1 - gx0_) / kCell) + 1);
        gh_ = std::max(1, static_cast<int>((gy1 - gy0_) / kCell) + 1);
        if (static_cast<long long>(gw_) * gh_ > 4'000'000) {
            gw_ = gh_ = 0;
            return;
        }
        cells_.assign(static_cast<std::size_t>(gw_) * gh_, {});
        for (std::uint32_t i = 0; i < shapes_.size(); ++i) {
            const Shape& s = shapes_[i];
            const int cx0 = Cell(s.x0 - reach, gx0_), cx1 = Cell(s.x1 + reach, gx0_);
            const int cy0 = Cell(s.y0 - reach, gy0_), cy1 = Cell(s.y1 + reach, gy0_);
            for (int y = std::max(cy0, 0); y <= std::min(cy1, gh_ - 1); ++y)
                for (int x = std::max(cx0, 0); x <= std::min(cx1, gw_ - 1); ++x)
                    cells_[static_cast<std::size_t>(y) * gw_ + x].push_back(i);
        }
        // Boxes first, then quads, each in zone order: the order the game evaluates them in.
        for (auto& c : cells_)
            std::stable_sort(c.begin(), c.end(), [&](std::uint32_t a, std::uint32_t b) {
                return shapes_[a].kind < shapes_[b].kind;
            });
    }

    bool Empty() const { return shapes_.empty(); }

    /// Smooth-union distance at a point; `far` when nothing revealed is within reach.
    float Distance(float px, float py, float far = 100.0f) const {
        if (cells_.empty())
            return far;
        const int cx = Cell(px, gx0_), cy = Cell(py, gy0_);
        if (cx < 0 || cy < 0 || cx >= gw_ || cy >= gh_)
            return far;
        const auto& list = cells_[static_cast<std::size_t>(cy) * gw_ + cx];
        if (list.empty())
            return far;
        float d = std::numeric_limits<float>::max();
        for (const std::uint32_t i : list) {
            const Shape& s = shapes_[i];
            const float sd = s.kind == Shape::Box ? BoxDistance(s, px, py) : QuadDistance(s, px, py);
            d = SmoothMin(d, sd, k_);
        }
        return std::min(d, far);
    }

private:
    static constexpr float kCell = 16.0f;
    static int Cell(float v, float origin) { return static_cast<int>(std::floor((v - origin) / kCell)); }

    std::vector<Shape> shapes_;
    std::vector<std::vector<std::uint32_t>> cells_;
    float k_ = 3.0f, reach_ = 12.0f;
    float gx0_ = 0, gy0_ = 0;
    int gw_ = 0, gh_ = 0;
};

} // namespace bc_fog
