// SPDX-License-Identifier: GPL-3.0-or-later
// Level map drawing. See bc_mapdraw.h.

#include "bc_mapdraw.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

#if defined(__linux__) || defined(__ANDROID__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "bc_shade.h"

namespace bc_mapdraw {

using bc_levelmap::Icon;
using bc_levelmap::IconRule;
using bc_levelmap::LevelMap;
using bc_levelmap::Sprite;
using bc_levelmap::Texture;
using bc_shade::Vec4;

namespace {

// ---- colour tables

struct Tables {
    float srgb_to_linear[256];
    float unorm[256];
    std::uint8_t linear_to_srgb[4097];
    Tables() {
        for (int i = 0; i < 256; ++i) {
            const float c = static_cast<float>(i) / 255.0f;
            srgb_to_linear[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
            unorm[i] = c;
        }
        for (int i = 0; i <= 4096; ++i) {
            const float c = static_cast<float>(i) / 4096.0f;
            const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
            linear_to_srgb[i] = static_cast<std::uint8_t>(std::clamp(std::lround(s * 255.0f), 0L, 255L));
        }
    }
};
const Tables& T() {
    static const Tables t;
    return t;
}
std::uint8_t Encode(float linear) {
    const float c = std::clamp(linear, 0.0f, 1.0f);
    return T().linear_to_srgb[static_cast<int>(c * 4096.0f + 0.5f)];
}

// ---- texture sampling (GL conventions: bilinear, texel centres at +0.5)

struct Sampler {
    const std::uint8_t* px = nullptr;
    int w = 0, h = 0;
    int mask_x = -1, mask_y = -1; // power-of-two repeat
    bool repeat = false;
    const float* lut = nullptr; // RGB decode; alpha is always linear

    void Bind(const Texture& t, std::uint32_t slice) {
        if (t.Empty()) {
            px = nullptr;
            return;
        }
        slice = std::min(slice, t.slices - 1);
        px = t.rgba.data() + static_cast<std::size_t>(t.w) * t.h * 4 * slice;
        w = static_cast<int>(t.w);
        h = static_cast<int>(t.h);
        repeat = t.repeat;
        mask_x = repeat && (w & (w - 1)) == 0 ? w - 1 : -1;
        mask_y = repeat && (h & (h - 1)) == 0 ? h - 1 : -1;
        lut = t.srgb ? T().srgb_to_linear : T().unorm;
    }
    int WrapX(int x) const {
        if (mask_x >= 0)
            return x & mask_x;
        if (repeat) {
            x %= w;
            return x < 0 ? x + w : x;
        }
        return x < 0 ? 0 : (x >= w ? w - 1 : x);
    }
    int WrapY(int y) const {
        if (mask_y >= 0)
            return y & mask_y;
        if (repeat) {
            y %= h;
            return y < 0 ? y + h : y;
        }
        return y < 0 ? 0 : (y >= h ? h - 1 : y);
    }
    Vec4 Sample(float u, float v) const {
        if (!px)
            return {};
        float fx = u * static_cast<float>(w) - 0.5f;
        float fy = v * static_cast<float>(h) - 0.5f;
        // Keep the integer conversion in range (clamped textures ignore the excess anyway).
        fx = std::clamp(fx, -1.0e6f, 1.0e6f);
        fy = std::clamp(fy, -1.0e6f, 1.0e6f);
        const float flx = std::floor(fx), fly = std::floor(fy);
        const float ax = fx - flx, ay = fy - fly;
        const int ix = static_cast<int>(flx), iy = static_cast<int>(fly);
        const int x0 = WrapX(ix), x1 = WrapX(ix + 1), y0 = WrapY(iy), y1 = WrapY(iy + 1);
        const std::uint8_t* r0 = px + static_cast<std::size_t>(y0) * w * 4;
        const std::uint8_t* r1 = px + static_cast<std::size_t>(y1) * w * 4;
        const std::uint8_t* a = r0 + x0 * 4;
        const std::uint8_t* b = r0 + x1 * 4;
        const std::uint8_t* c = r1 + x0 * 4;
        const std::uint8_t* d = r1 + x1 * 4;
        const float w00 = (1 - ax) * (1 - ay), w10 = ax * (1 - ay), w01 = (1 - ax) * ay, w11 = ax * ay;
        const float* L = lut;
        constexpr float U = 1.0f / 255.0f;
        return {L[a[0]] * w00 + L[b[0]] * w10 + L[c[0]] * w01 + L[d[0]] * w11,
                L[a[1]] * w00 + L[b[1]] * w10 + L[c[1]] * w01 + L[d[1]] * w11,
                L[a[2]] * w00 + L[b[2]] * w10 + L[c[2]] * w01 + L[d[2]] * w11,
                (a[3] * w00 + b[3] * w10 + c[3] * w01 + d[3] * w11) * U};
    }
};

struct MapTextures {
    Sampler data, silhouette, parchment, noise;
    // Inside a secret zone not found yet, the map data comes from the secret texture.
    const bc_levelmap::Secret* secret = nullptr;
    Sampler secret_s;
    float tile_x = 0, tile_y = 0, tile_w = 1, tile_h = 1;
    Vec4 Data(float u, float v) const {
        if (!secret)
            return data.Sample(u, v);
        const float wx = tile_x + u * tile_w, wy = tile_y + v * tile_h;
        return secret_s.Sample((wx - secret->tx) / (static_cast<float>(secret->tex.w) * secret->texel),
                               (wy - secret->ty) / (static_cast<float>(secret->tex.h) * secret->texel));
    }
    Vec4 Silhouette(float u, float v) const { return silhouette.Sample(u, v); }
    Vec4 Parchment(float u, float v) const { return parchment.Sample(u, v); }
    Vec4 Noise(float u, float v) const { return noise.Sample(u, v); }
};

void LowerPriority() {
#if defined(__linux__) || defined(__ANDROID__)
    // Map drawing must not compete with the emulator: run it at a low priority.
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 19);
#endif
}

template <class F>
void Parallel(int threads, int rows, F&& body) {
    threads = std::clamp(threads, 1, 8);
    std::atomic<int> next{0};
    constexpr int kChunk = 8;
    const auto work = [&] {
        LowerPriority();
        for (;;) {
            const int r0 = next.fetch_add(kChunk);
            if (r0 >= rows)
                break;
            for (int r = r0; r < std::min(rows, r0 + kChunk); ++r)
                body(r);
        }
    };
    std::vector<std::thread> pool;
    for (int i = 1; i < threads; ++i)
        pool.emplace_back(work);
    work();
    for (auto& t : pool)
        t.join();
}

// ---- icons

struct PlacedIcon {
    const Icon* icon = nullptr;
    const Sprite* sprite = nullptr;
    float boost = 1;
    double px0 = 0, py0 = 0, px1 = 0, py1 = 0; // pixel rect
};

/// Blends the icons over one row of linear RGB.
void DrawIconsRow(const std::vector<PlacedIcon>& icons, const View& view, int py, float* row) {
    for (const PlacedIcon& pi : icons) {
        if (py + 1 <= pi.py0 || py >= pi.py1)
            continue;
        const Icon& ic = *pi.icon;
        const Sprite& sp = *pi.sprite;
        Sampler s;
        s.Bind(sp.tex, 0);
        const float sw = sp.width * ic.sx * pi.boost, sh = sp.height * ic.sy * pi.boost;
        const int x0 = std::max(0, static_cast<int>(std::floor(pi.px0)));
        const int x1 = std::min(view.w, static_cast<int>(std::ceil(pi.px1)));
        // Texels per pixel decides the supersampling.
        const double tpp = sp.tex.w / std::max(1e-3, (pi.px1 - pi.px0));
        const int ss = tpp > 1.5 ? 3 : (tpp > 0.75 ? 2 : 1);
        for (int px = x0; px < x1; ++px) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < ss; ++sy) {
                for (int sx = 0; sx < ss; ++sx) {
                    const double fx = px + (sx + 0.5) / ss, fy = py + (sy + 0.5) / ss;
                    const double wx = view.x0 + fx / view.scale, wy = view.y1 - fy / view.scale;
                    const float u = sp.pivot_x + static_cast<float>(wx - ic.x) / sw;
                    const float v = sp.pivot_y + static_cast<float>(wy - ic.y) / sh;
                    if (u < 0 || u > 1 || v < 0 || v > 1)
                        continue;
                    const Vec4 c = s.Sample(u, v);
                    r += c.x * c.w, g += c.y * c.w, b += c.z * c.w, a += c.w;
                }
            }
            const float n = 1.0f / static_cast<float>(ss * ss);
            r *= n, g *= n, b *= n, a *= n;
            if (a <= 0)
                continue;
            float* o = row + px * 3;
            o[0] = r + o[0] * (1 - a);
            o[1] = g + o[1] * (1 - a);
            o[2] = b + o[2] * (1 - a);
        }
    }
}

} // namespace

namespace {
/// "World_..._uid59" -> ("World_...", 59); uid -1 when there is none.
std::pair<std::string_view, int> SplitUid(std::string_view id) {
    const auto p = id.rfind("_uid");
    if (p == std::string_view::npos)
        return {id, -1};
    int v = 0;
    bool any = false;
    for (std::size_t i = p + 4; i < id.size(); ++i) {
        if (id[i] < '0' || id[i] > '9')
            return {id, -1};
        v = v * 10 + (id[i] - '0');
        any = true;
    }
    return any ? std::pair<std::string_view, int>{id.substr(0, p), v} : std::pair<std::string_view, int>{id, -1};
}
} // namespace

const Exploration::Room* Exploration::Find(const std::string& room_id) const {
    const auto it = rooms.find(room_id);
    if (it != rooms.end())
        return &it->second;
    // The game may key a room by its instance name ("StreetsPoor_Room21_uid59", the end of the
    // id) or by an id with the same biome and uid.
    const auto [want_world, want_uid] = SplitUid(room_id);
    if (want_uid < 0)
        return nullptr;
    const auto biome_of = [](std::string_view s) {
        constexpr std::string_view kPrefix = "World_Biome_"; // "World_Biome_01_"
        return s.substr(0, kPrefix.size()) == kPrefix
                   ? s.substr(0, std::min<std::size_t>(s.size(), kPrefix.size() + 3))
                   : std::string_view{};
    };
    const std::string_view want(room_id);
    for (const auto& [k, r] : rooms) {
        const auto [world, uid] = SplitUid(k);
        if (uid != want_uid)
            continue;
        if (k.size() < want.size() && want.substr(want.size() - k.size()) == k && want[want.size() - k.size() - 1] == '_')
            return &r;
        const auto b = biome_of(world);
        if (!b.empty() && b == biome_of(want_world))
            return &r;
    }
    return nullptr;
}

std::shared_ptr<const Reveal> BuildReveal(const LevelMap& map, const Exploration& ex) {
    auto r = std::make_shared<Reveal>();
    std::vector<bc_fog::Shape> shapes;
    if (ex.known) {
        for (const auto& z : map.zones) {
            if (z.culled)
                continue;
            const bool whole_room = !ex.current_room_only.empty() &&
                                    (z.room_id == ex.current_room_only ||
                                     SplitUid(z.room_id).second == SplitUid(ex.current_room_only).second);
            const auto* room = ex.Find(z.room_id);
            if (!whole_room && (!room || !room->zones.count(z.zone_id)))
                continue;
            shapes.insert(shapes.end(), z.shapes.begin(), z.shapes.end());
            ++r->zones;
        }
    }
    // Boxes first, then quads, as the game's fog pass draws them.
    std::stable_sort(shapes.begin(), shapes.end(),
                     [](const bc_fog::Shape& a, const bc_fog::Shape& b) { return a.kind < b.kind; });
    r->field.Build(std::move(shapes), map.smoothness, 16.0f);
    for (const auto& sz : map.secrets) {
        const auto* room = ex.known ? ex.Find(sz.room_id) : nullptr;
        if (!(room && room->secrets.count(sz.secret_id)))
            r->hidden.push_back(&sz);
    }
    r->icon_visible.assign(map.icons.size(), 0);
    for (std::size_t i = 0; i < map.icons.size(); ++i) {
        const Icon& ic = map.icons[i];
        if (ic.rule == IconRule::Always) {
            r->icon_visible[i] = 1;
            continue;
        }
        const auto* room = ex.known ? ex.Find(ic.room_id) : nullptr;
        const bool zone = ic.unrevealed_ok || (room && room->zones.count(ic.zone_id));
        bool show = false;
        switch (ic.rule) {
        case IconRule::ZoneEntered:
            show = zone;
            break;
        case IconRule::ZDoorDiscovered:
            show = room && !room->zdoors.count(ic.zdoor_id) &&
                   (room->zones.count(ic.zone_id) || room->zdoor_zones.count(ic.zone_id));
            break;
        case IconRule::ZDoorEntered:
            show = room && room->zdoors.count(ic.zdoor_id);
            break;
        case IconRule::LastSave:
            show = zone && !ex.last_save_room.empty() && ex.last_save_room == ic.room_id;
            break;
        case IconRule::NotActivated:
            show = zone && !(room && room->activated.count(ic.guid));
            break;
        case IconRule::Always:
            show = true;
            break;
        }
        // The plain save icon gives way to the last-save one.
        if (show && ic.sprite == "Map_SaveRoomIcon" && !ex.last_save_room.empty() &&
            ex.last_save_room == ic.room_id)
            show = false;
        r->icon_visible[i] = show ? 1 : 0;
    }
    return r;
}

View CenteredView(double cx, double cy, double scale, int w, int h) {
    View v;
    v.scale = scale;
    v.w = w;
    v.h = h;
    v.x0 = cx - w / (2.0 * scale);
    v.y1 = cy + h / (2.0 * scale);
    return v;
}

View FitZone(const LevelMap& map, int zone, int w, int h, double margin) {
    if (zone < 0 || static_cast<std::size_t>(zone) >= map.nav_zones.size())
        return FitView(map, w, h, margin);
    const auto& z = map.nav_zones[static_cast<std::size_t>(zone)];
    const double scale = std::min((w - 2 * margin) / std::max(1.0f, z.w), (h - 2 * margin) / std::max(1.0f, z.h));
    return CenteredView(z.x + z.w / 2.0, z.y + z.h / 2.0, scale, w, h);
}

View FitView(const LevelMap& map, int w, int h, double margin) {
    const double mw = std::max(1.0f, map.x1 - map.x0), mh = std::max(1.0f, map.y1 - map.y0);
    const double scale = std::min((w - 2 * margin) / mw, (h - 2 * margin) / mh);
    return CenteredView((map.x0 + map.x1) / 2.0, (map.y0 + map.y1) / 2.0, scale, w, h);
}

std::vector<std::uint8_t> Render(const LevelMap& map, const Reveal& reveal, const View& view,
                                 const RenderOptions& opt) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(view.w) * view.h * 4, 0);
    if (view.w <= 0 || view.h <= 0 || map.tiles.empty())
        return out;
    const bc_shade::Params& P = map.params;
#ifdef BC_SHADE_NO_LIMITS
    const bc_shade::Limits lim{}; // test builds: every pixel through the full shader
#else
    const bc_shade::Limits lim = bc_shade::ComputeLimits(P);
#endif

    // Tile grid (the tiles form a full grid, listed row by row from the top).
    const float tw = map.tiles[0].w, th = map.tiles[0].h;
    const int cols = std::max(1, static_cast<int>(std::lround((map.x1 - map.x0) / tw)));
    const int rows = std::max(1, static_cast<int>(std::lround((map.y1 - map.y0) / th)));
    std::vector<int> grid(static_cast<std::size_t>(cols) * rows, -1);
    for (std::size_t i = 0; i < map.tiles.size(); ++i) {
        const auto& t = map.tiles[i];
        const int c = static_cast<int>(std::lround((t.x - map.x0) / tw));
        const int r = static_cast<int>(std::lround((map.y1 - (t.y + t.h)) / th));
        if (c >= 0 && c < cols && r >= 0 && r < rows)
            grid[static_cast<std::size_t>(r) * cols + c] = static_cast<int>(i);
    }

    // Fog on a coarse world grid, interpolated per pixel (the game also renders its fog field at
    // a low resolution and lets the GPU filter it).
    const double step = std::clamp(4.0 / view.scale, 0.75, 4.0);
    const double fx0 = view.x0 - step, fy1 = view.y1 + step;
    const int fw = static_cast<int>(std::ceil(view.w / view.scale / step)) + 3;
    const int fh = static_cast<int>(std::ceil(view.h / view.scale / step)) + 3;
    std::vector<float> fog(static_cast<std::size_t>(fw) * fh);
    const float far = 100.0f;
    Parallel(opt.threads, fh, [&](int y) {
        for (int x = 0; x < fw; ++x)
            fog[static_cast<std::size_t>(y) * fw + x] =
                reveal.field.Distance(static_cast<float>(fx0 + x * step), static_cast<float>(fy1 - y * step), far);
    });

    // Icons on screen: the map's own, then the extra ones.
    std::vector<PlacedIcon> icons;
    const std::size_t n_icons = map.icons.size() + opt.extra.size();
    for (std::size_t k = 0; k < n_icons; ++k) {
        PlacedIcon pi;
        if (k < map.icons.size()) {
            if (k >= reveal.icon_visible.size() || !reveal.icon_visible[k])
                continue;
            const auto sit = map.sprites.find(map.icons[k].sprite);
            if (sit == map.sprites.end())
                continue;
            pi.icon = &map.icons[k];
            pi.sprite = &sit->second;
        } else {
            const ExtraIcon& e = opt.extra[k - map.icons.size()];
            if (!e.sprite || e.sprite->tex.Empty())
                continue;
            pi.icon = &e.icon;
            pi.sprite = e.sprite;
        }
        const Icon& ic = *pi.icon;
        const float hw = pi.sprite->height * std::fabs(ic.sy);
        if (opt.min_icon_px > 0 && hw <= 8.0f && hw * view.scale < opt.min_icon_px)
            pi.boost = static_cast<float>(opt.min_icon_px / (hw * view.scale));
        const double sw = pi.sprite->width * ic.sx * pi.boost, sh = pi.sprite->height * ic.sy * pi.boost;
        const double ax = ic.x - pi.sprite->pivot_x * sw, bx = ax + sw;
        const double ay = ic.y - pi.sprite->pivot_y * sh, by = ay + sh;
        double px0, py0, px1, py1;
        view.WorldToPixel(std::min(ax, bx), std::max(ay, by), px0, py0);
        view.WorldToPixel(std::max(ax, bx), std::min(ay, by), px1, py1);
        if (px1 < 0 || py1 < 0 || px0 > view.w || py0 > view.h)
            continue;
        pi.px0 = px0, pi.py0 = py0, pi.px1 = px1, pi.py1 = py1;
        icons.push_back(pi);
    }

    Parallel(opt.threads, view.h, [&](int py) {
        std::vector<float> row(static_cast<std::size_t>(view.w) * 3, 0.0f);
        const double wy = view.y1 - (py + 0.5) / view.scale;
        const int tr = static_cast<int>(std::floor((map.y1 - wy) / th));
        const double gy = (fy1 - wy) / step;
        const int gy0 = std::clamp(static_cast<int>(std::floor(gy)), 0, fh - 2);
        const float ay = static_cast<float>(gy - gy0);
        MapTextures tex;
        tex.parchment.Bind(map.parchment, 0);
        tex.noise.Bind(map.noise, 0);
        int bound = -1;
        // Hidden secret zones crossing this row.
        const bc_levelmap::Secret* row_secrets[16];
        int n_secrets = 0;
        for (const auto* sz : reveal.hidden) {
            if (wy >= sz->y && wy < sz->y + sz->h && n_secrets < 16)
                row_secrets[n_secrets++] = sz;
        }
        for (int px = 0; px < view.w; ++px) {
            const double wx = view.x0 + (px + 0.5) / view.scale;
            const int tc = static_cast<int>(std::floor((wx - map.x0) / tw));
            if (tr < 0 || tr >= rows || tc < 0 || tc >= cols)
                continue;
            const int ti = grid[static_cast<std::size_t>(tr) * cols + tc];
            if (ti < 0)
                continue;
            const auto& tile = map.tiles[static_cast<std::size_t>(ti)];
            if (ti != bound) {
                tex.data.Bind(map.data, tile.slice);
                tex.silhouette.Bind(map.silhouette, tile.slice);
                tex.tile_x = tile.x, tex.tile_y = tile.y, tex.tile_w = tile.w, tex.tile_h = tile.h;
                bound = ti;
            }
            const bc_levelmap::Secret* in_secret = nullptr;
            for (int k = 0; k < n_secrets; ++k) {
                if (wx >= row_secrets[k]->x && wx < row_secrets[k]->x + row_secrets[k]->w)
                    in_secret = row_secrets[k];
            }
            if (in_secret != tex.secret) {
                tex.secret = in_secret;
                if (in_secret)
                    tex.secret_s.Bind(in_secret->tex, 0);
            }
            const double gx = (wx - fx0) / step;
            const int gx0 = std::clamp(static_cast<int>(std::floor(gx)), 0, fw - 2);
            const float ax = static_cast<float>(gx - gx0);
            const float* f0 = &fog[static_cast<std::size_t>(gy0) * fw + gx0];
            const float* f1 = f0 + fw;
            const float d = (f0[0] * (1 - ax) + f0[1] * ax) * (1 - ay) + (f1[0] * (1 - ax) + f1[1] * ax) * ay;
            bc_shade::Pixel pix;
            pix.u = static_cast<float>((wx - tile.x) / tile.w);
            pix.v = static_cast<float>((wy - tile.y) / tile.h);
            pix.wx = static_cast<float>(wx);
            pix.wy = static_cast<float>(wy);
            pix.fog = d * 0.1f; // the game's fog pass stores distance / 10
            pix.selected = opt.selected;
            const Vec4 c = bc_shade::Shade(P, tex, pix, lim);
            const float a = std::clamp(c.w, 0.0f, 1.0f);
            float* o = &row[static_cast<std::size_t>(px) * 3];
            o[0] = std::max(0.0f, c.x) * a;
            o[1] = std::max(0.0f, c.y) * a;
            o[2] = std::max(0.0f, c.z) * a;
        }
        DrawIconsRow(icons, view, py, row.data());
        std::uint8_t* dst = out.data() + static_cast<std::size_t>(py) * view.w * 4;
        for (int px = 0; px < view.w; ++px) {
            dst[px * 4 + 0] = Encode(row[static_cast<std::size_t>(px) * 3 + 0]);
            dst[px * 4 + 1] = Encode(row[static_cast<std::size_t>(px) * 3 + 1]);
            dst[px * 4 + 2] = Encode(row[static_cast<std::size_t>(px) * 3 + 2]);
            dst[px * 4 + 3] = 255;
        }
    });
    return out;
}

std::vector<std::uint8_t> RenderSprite(const Sprite& sprite, int w, int h) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4, 0);
    if (sprite.tex.Empty() || w <= 0 || h <= 0)
        return out;
    Sampler s;
    s.Bind(sprite.tex, 0);
    const double k = std::min(static_cast<double>(w) / sprite.tex.w, static_cast<double>(h) / sprite.tex.h);
    const double dw = sprite.tex.w * k, dh = sprite.tex.h * k;
    const double ox = (w - dw) / 2, oy = (h - dh) / 2;
    const int ss = 4;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < ss; ++sy) {
                for (int sx = 0; sx < ss; ++sx) {
                    const double u = (x + (sx + 0.5) / ss - ox) / dw;
                    const double v = 1.0 - (y + (sy + 0.5) / ss - oy) / dh; // top row first
                    if (u < 0 || u > 1 || v < 0 || v > 1)
                        continue;
                    const Vec4 c = s.Sample(static_cast<float>(u), static_cast<float>(v));
                    r += c.x * c.w, g += c.y * c.w, b += c.z * c.w, a += c.w;
                }
            }
            a /= ss * ss;
            std::uint8_t* o = &out[(static_cast<std::size_t>(y) * w + x) * 4];
            if (a > 0) {
                const float n = 1.0f / (a * ss * ss);
                o[0] = Encode(r * n), o[1] = Encode(g * n), o[2] = Encode(b * n);
            }
            o[3] = static_cast<std::uint8_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f));
        }
    }
    return out;
}

// ---- title text

namespace {

std::vector<std::uint32_t> Utf8(const std::string& s) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = c;
        int extra = 0;
        if (c >= 0xF0)
            cp = c & 0x07, extra = 3;
        else if (c >= 0xE0)
            cp = c & 0x0F, extra = 2;
        else if (c >= 0xC0)
            cp = c & 0x1F, extra = 1;
        ++i;
        for (int k = 0; k < extra && i < s.size(); ++k, ++i)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        out.push_back(cp);
    }
    return out;
}

std::uint32_t Upper(std::uint32_t c) {
    if (c >= 'a' && c <= 'z')
        return c - 32;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7))
        return c - 32;
    return c;
}

/// Coverage of one line of small-caps text into an alpha buffer (distance-based, with an
/// outer distance for the shadow). `fill` and `glow` are w*h buffers.
void DrawLine(const bc_levelmap::Font& font, const std::string& text, float size, float x, float baseline, int w,
              int h, std::vector<float>& fill, std::vector<float>& shadow, float shadow_dx, float shadow_dy) {
    const auto& atlas = font.atlas;
    const float spread = 2.0f * (font.padding + 1.0f); // atlas pixels per unit of SDF value
    const float pad = font.padding;
    float pen = x;
    for (const std::uint32_t raw : Utf8(text)) {
        const bool small = raw != Upper(raw);
        const std::uint32_t cp = Upper(raw);
        const auto it = font.glyphs.find(cp);
        const float k = size * (small ? 0.8f : 1.0f) / font.point_size;
        if (it == font.glyphs.end()) {
            pen += 0.3f * size;
            continue;
        }
        const auto& g = it->second;
        if (g.w > 1 && g.h > 1) {
            // Quad in output pixels (y down), atlas rect padded on every side.
            const float qx0 = pen + (g.bx - pad) * k, qx1 = pen + (g.bx + g.w + pad) * k;
            const float qy0 = baseline - (g.by + pad) * k, qy1 = baseline - (g.by - g.h - pad) * k;
            const float ax0 = g.x - pad, ay1 = g.y + g.h + pad; // atlas: y up
            const float px_per_atlas = k;                      // output px per atlas px
            for (int pass = 0; pass < 2; ++pass) {
                const float dx = pass ? shadow_dx : 0, dy = pass ? shadow_dy : 0;
                const int x0 = std::max(0, static_cast<int>(std::floor(qx0 + dx)));
                const int x1 = std::min(w, static_cast<int>(std::ceil(qx1 + dx)));
                const int y0 = std::max(0, static_cast<int>(std::floor(qy0 + dy)));
                const int y1 = std::min(h, static_cast<int>(std::ceil(qy1 + dy)));
                for (int yy = y0; yy < y1; ++yy) {
                    for (int xx = x0; xx < x1; ++xx) {
                        const float ox = (static_cast<float>(xx) + 0.5f - dx - qx0) / px_per_atlas + ax0;
                        const float oy = ay1 - (static_cast<float>(yy) + 0.5f - dy - qy0) / px_per_atlas;
                        // Bilinear on the SDF.
                        const float fx = ox - 0.5f, fy = oy - 0.5f;
                        const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
                        const float tx = fx - static_cast<float>(ix), ty = fy - static_cast<float>(iy);
                        const auto at = [&](int ax, int ay) -> float {
                            if (ax < 0 || ay < 0 || ax >= static_cast<int>(atlas.w) || ay >= static_cast<int>(atlas.h))
                                return 0.0f;
                            return atlas.rgba[(static_cast<std::size_t>(ay) * atlas.w + ax) * 4 + 3] / 255.0f;
                        };
                        const float a = (at(ix, iy) * (1 - tx) + at(ix + 1, iy) * tx) * (1 - ty) +
                                        (at(ix, iy + 1) * (1 - tx) + at(ix + 1, iy + 1) * tx) * ty;
                        const float dist = (a - 0.5f) * spread * px_per_atlas; // output px, + inside
                        float& dst = pass ? shadow[static_cast<std::size_t>(yy) * w + xx]
                                          : fill[static_cast<std::size_t>(yy) * w + xx];
                        if (pass == 0) {
                            dst = std::max(dst, std::clamp(dist + 0.5f, 0.0f, 1.0f));
                        } else {
                            // Soft shadow: full inside, fading over 4 px outside.
                            dst = std::max(dst, std::clamp((dist + 4.0f) / 5.0f, 0.0f, 1.0f));
                        }
                    }
                }
            }
        }
        pen += g.advance * k;
    }
}

} // namespace

std::vector<std::uint8_t> RenderTitle(const bc_levelmap::Font& font, const std::string& title,
                                      const std::string& subtitle, int w, int h) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4, 0);
    if (font.Empty() || w <= 0 || h <= 0 || font.point_size <= 0)
        return out;
    std::vector<float> fill1(static_cast<std::size_t>(w) * h, 0), fill2 = fill1, shadow = fill1;
    const float big = static_cast<float>(h) * 0.42f, small = static_cast<float>(h) * 0.26f;
    const float base1 = big * 0.95f + 4, base2 = base1 + small * 1.35f;
    DrawLine(font, title, big, 6, base1, w, h, fill1, shadow, 2.5f, 3.0f);
    if (!subtitle.empty())
        DrawLine(font, subtitle, small, 8, base2, w, h, fill2, shadow, 2.0f, 2.5f);
    // Pale gold title, warm light subtitle, dark shadow (sRGB).
    const float c1[3] = {0.93f, 0.84f, 0.62f}, c2[3] = {0.86f, 0.80f, 0.68f}, cs[3] = {0.02f, 0.015f, 0.02f};
    for (std::size_t i = 0; i < fill1.size(); ++i) {
        const float a1 = fill1[i], a2 = fill2[i], as = shadow[i] * 0.85f;
        // Shadow under the fills.
        float r = cs[0], g = cs[1], b = cs[2], a = as;
        const auto over = [&](const float* c, float ca) {
            const float na = ca + a * (1 - ca);
            if (na <= 0)
                return;
            r = (c[0] * ca + r * a * (1 - ca)) / na;
            g = (c[1] * ca + g * a * (1 - ca)) / na;
            b = (c[2] * ca + b * a * (1 - ca)) / na;
            a = na;
        };
        over(c1, a1);
        over(c2, a2);
        std::uint8_t* o = &out[i * 4];
        o[0] = static_cast<std::uint8_t>(std::lround(std::clamp(r, 0.0f, 1.0f) * 255));
        o[1] = static_cast<std::uint8_t>(std::lround(std::clamp(g, 0.0f, 1.0f) * 255));
        o[2] = static_cast<std::uint8_t>(std::lround(std::clamp(b, 0.0f, 1.0f) * 255));
        o[3] = static_cast<std::uint8_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255));
    }
    return out;
}

} // namespace bc_mapdraw
