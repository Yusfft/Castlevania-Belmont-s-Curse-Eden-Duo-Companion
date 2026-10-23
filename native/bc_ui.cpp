// SPDX-License-Identifier: GPL-3.0-or-later
// Interface drawing with the game's map-screen art. See bc_ui.h.

#include "bc_ui.h"

#include <algorithm>
#include <cmath>

namespace bc_ui {

using bc_levelmap::Font;
using bc_levelmap::Sprite;
using bc_shade::Vec4;

namespace {

float SrgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float LinearToSrgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
const float* Lut() {
    static float lut[256];
    static const bool init = [] {
        for (int i = 0; i < 256; ++i)
            lut[i] = SrgbToLinear(static_cast<float>(i) / 255.0f);
        return true;
    }();
    (void)init;
    return lut;
}

/// Bilinear texel fetch, premultiplied linear. (x, y) in texels, y down from the sprite's top.
Vec4 Fetch(const Sprite& s, float x, float y) {
    const auto& t = s.tex;
    const int w = static_cast<int>(t.w), h = static_cast<int>(t.h);
    const float fx = x - 0.5f, fy = (static_cast<float>(h) - y) - 0.5f; // stored rows run bottom-up
    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    const float ax = fx - static_cast<float>(x0), ay = fy - static_cast<float>(y0);
    const float* L = t.srgb ? Lut() : nullptr;
    Vec4 o{};
    for (int k = 0; k < 4; ++k) {
        const int xx = std::clamp(x0 + (k & 1), 0, w - 1), yy = std::clamp(y0 + (k >> 1), 0, h - 1);
        const float wgt = ((k & 1) ? ax : 1 - ax) * ((k >> 1) ? ay : 1 - ay);
        const std::uint8_t* p = &t.rgba[(static_cast<std::size_t>(yy) * w + xx) * 4];
        const float a = p[3] / 255.0f;
        const float r = L ? L[p[0]] : p[0] / 255.0f, g = L ? L[p[1]] : p[1] / 255.0f, b = L ? L[p[2]] : p[2] / 255.0f;
        o.x += r * a * wgt, o.y += g * a * wgt, o.z += b * a * wgt, o.w += a * wgt;
    }
    return o;
}

std::uint32_t Upper(std::uint32_t c) {
    if (c >= 'a' && c <= 'z')
        return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7)
        return c - 32;
    return c;
}

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

Vec4 Lin(const Vec4& srgb) {
    return {SrgbToLinear(srgb.x), SrgbToLinear(srgb.y), SrgbToLinear(srgb.z), srgb.w};
}

} // namespace

Canvas::Canvas(int w, int h) : w_(std::max(w, 1)), h_(std::max(h, 1)), px_(static_cast<std::size_t>(w_) * h_ * 4, 0.0f) {}

void Canvas::Over(int x, int y, float r, float g, float b, float a) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_ || a <= 0)
        return;
    float* p = &px_[(static_cast<std::size_t>(y) * w_ + x) * 4];
    p[0] = r + p[0] * (1 - a);
    p[1] = g + p[1] * (1 - a);
    p[2] = b + p[2] * (1 - a);
    p[3] = a + p[3] * (1 - a);
}

void Canvas::Blit(const Sprite& s, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh,
                  const Vec4& tint) {
    if (s.tex.Empty() || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
        return;
    const int x0 = std::max(0, static_cast<int>(std::floor(dx))), x1 = std::min(w_, static_cast<int>(std::ceil(dx + dw)));
    const int y0 = std::max(0, static_cast<int>(std::floor(dy))), y1 = std::min(h_, static_cast<int>(std::ceil(dy + dh)));
    const float kx = sw / dw, ky = sh / dh;
    const int ss = std::clamp(static_cast<int>(std::ceil(std::max(kx, ky))), 1, 4);
    const Vec4 tl = Lin(tint);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            Vec4 acc{};
            int n = 0;
            for (int j = 0; j < ss; ++j) {
                for (int i = 0; i < ss; ++i) {
                    const float px = static_cast<float>(x) + (static_cast<float>(i) + 0.5f) / static_cast<float>(ss);
                    const float py = static_cast<float>(y) + (static_cast<float>(j) + 0.5f) / static_cast<float>(ss);
                    if (px < dx || px >= dx + dw || py < dy || py >= dy + dh)
                        continue;
                    const Vec4 c = Fetch(s, sx + (px - dx) * kx, sy + (py - dy) * ky);
                    acc.x += c.x, acc.y += c.y, acc.z += c.z, acc.w += c.w;
                    ++n;
                }
            }
            if (n == 0)
                continue;
            const float inv = 1.0f / static_cast<float>(ss * ss); // partial pixels at the rect edge stay partial
            Over(x, y, acc.x * inv * tl.x * tl.w, acc.y * inv * tl.y * tl.w, acc.z * inv * tl.z * tl.w,
                 acc.w * inv * tl.w);
        }
    }
}

void Canvas::Blit(const Sprite& s, float dx, float dy, float dw, float dh, const Vec4& tint) {
    Blit(s, 0, 0, static_cast<float>(s.tex.w), static_cast<float>(s.tex.h), dx, dy, dw, dh, tint);
}

void Canvas::Nine(const Sprite& s, float l, float t, float r, float b, float dx, float dy, float dw, float dh,
                  float scale, bool center, const Vec4& tint) {
    const float tw = static_cast<float>(s.tex.w), th = static_cast<float>(s.tex.h);
    const float sxs[4] = {0, l, tw - r, tw}, sys[4] = {0, t, th - b, th};
    const float dxs[4] = {dx, dx + l * scale, dx + dw - r * scale, dx + dw};
    const float dys[4] = {dy, dy + t * scale, dy + dh - b * scale, dy + dh};
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            if (i == 1 && j == 1 && !center)
                continue;
            Blit(s, sxs[i], sys[j], sxs[i + 1] - sxs[i], sys[j + 1] - sys[j], dxs[i], dys[j], dxs[i + 1] - dxs[i],
                 dys[j + 1] - dys[j], tint);
        }
    }
}

float Canvas::TextWidth(const Font& font, const std::string& text, float size, bool small_caps) {
    if (font.point_size <= 0)
        return 0;
    float pen = 0;
    for (const std::uint32_t raw : Utf8(text)) {
        const bool small = small_caps && raw != Upper(raw);
        const auto it = font.glyphs.find(small_caps ? Upper(raw) : raw);
        const float k = size * (small ? 0.8f : 1.0f) / font.point_size;
        pen += it == font.glyphs.end() ? 0.3f * size : it->second.advance * k;
    }
    return pen;
}

float Canvas::Text(const Font& font, const std::string& text, float x, float baseline, float size, const Vec4& color,
                   bool small_caps, float shadow) {
    if (font.Empty() || font.point_size <= 0)
        return 0;
    const auto& atlas = font.atlas;
    const float spread = 2.0f * (font.padding + 1.0f);
    const float pad = font.padding;
    const Vec4 cl = Lin(color);
    const auto sdf = [&](float ax, float ay) {
        const float fx = ax - 0.5f, fy = ay - 0.5f;
        const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
        const float tx = fx - static_cast<float>(ix), ty = fy - static_cast<float>(iy);
        const auto at = [&](int u, int v) -> float {
            if (u < 0 || v < 0 || u >= static_cast<int>(atlas.w) || v >= static_cast<int>(atlas.h))
                return 0.0f;
            return atlas.rgba[(static_cast<std::size_t>(v) * atlas.w + u) * 4 + 3] / 255.0f;
        };
        return (at(ix, iy) * (1 - tx) + at(ix + 1, iy) * tx) * (1 - ty) + (at(ix, iy + 1) * (1 - tx) + at(ix + 1, iy + 1) * tx) * ty;
    };
    const auto glyphs = Utf8(text);
    for (int pass = shadow > 0 ? 0 : 1; pass < 2; ++pass) {
        float pen = x;
        for (const std::uint32_t raw : glyphs) {
            const bool small = small_caps && raw != Upper(raw);
            const auto it = font.glyphs.find(small_caps ? Upper(raw) : raw);
            const float k = size * (small ? 0.8f : 1.0f) / font.point_size;
            if (it == font.glyphs.end()) {
                pen += 0.3f * size;
                continue;
            }
            const auto& g = it->second;
            const float off = pass == 0 ? std::max(1.5f, size * 0.06f) : 0.0f;
            const float qx0 = pen + (g.bx - pad) * k + off, qx1 = pen + (g.bx + g.w + pad) * k + off;
            const float qy0 = baseline - (g.by + pad) * k + off, qy1 = baseline - (g.by - g.h - pad) * k + off;
            const float ax0 = g.x - pad, ay1 = g.y + g.h + pad;
            for (int yy = std::max(0, static_cast<int>(qy0)); yy < std::min(h_, static_cast<int>(std::ceil(qy1))); ++yy) {
                for (int xx = std::max(0, static_cast<int>(qx0)); xx < std::min(w_, static_cast<int>(std::ceil(qx1))); ++xx) {
                    const float a = sdf((static_cast<float>(xx) + 0.5f - qx0) / k + ax0, ay1 - (static_cast<float>(yy) + 0.5f - qy0) / k);
                    const float dist = (a - 0.5f) * spread * k; // output px, + inside
                    if (pass == 0) {
                        const float cov = std::clamp((dist + 3.0f) / 4.0f, 0.0f, 1.0f) * shadow;
                        Over(xx, yy, 0, 0, 0, cov);
                    } else {
                        const float cov = std::clamp(dist + 0.5f, 0.0f, 1.0f) * cl.w;
                        Over(xx, yy, cl.x * cov, cl.y * cov, cl.z * cov, cov);
                    }
                }
            }
            pen += g.advance * k;
        }
        if (pass == 1)
            return pen - x;
    }
    return 0;
}

std::vector<std::uint8_t> Canvas::Encode() const {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w_) * h_ * 4);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w_) * h_; ++i) {
        const float a = std::clamp(px_[i * 4 + 3], 0.0f, 1.0f);
        const float inv = a > 0 ? 1.0f / a : 0;
        for (int k = 0; k < 3; ++k)
            out[i * 4 + k] = static_cast<std::uint8_t>(std::lround(LinearToSrgb(px_[i * 4 + k] * inv) * 255.0f));
        out[i * 4 + 3] = static_cast<std::uint8_t>(std::lround(a * 255.0f));
    }
    return out;
}

namespace {
const Sprite* Ui(const bc_levelmap::Common& c, const char* name) {
    const auto it = c.ui.find(name);
    return it == c.ui.end() || it->second.tex.Empty() ? nullptr : &it->second;
}
const Vec4 kCream{0.92f, 0.88f, 0.80f, 1.0f};
const Vec4 kGold{0.91f, 0.78f, 0.47f, 1.0f};
} // namespace

std::vector<std::uint8_t> Frame(const bc_levelmap::Common& c, int w, int h) {
    Canvas cv(w, h);
    // A soft dark vignette towards the edges, as the game's map screen has.
    const float band = 90.0f;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float d = static_cast<float>(std::min(std::min(x, w - 1 - x), std::min(y, h - 1 - y)));
            if (d < band) {
                const float t = 1.0f - d / band;
                cv.Darken(x, y, 0.55f * t * t);
            }
        }
    }
    if (const Sprite* bg = Ui(c, "Map_Background")) {
        // The panel's gold border and corner ornaments; its dark inside is left out.
        cv.Nine(*bg, 48, 48, 48, 48, 0, 0, static_cast<float>(w), static_cast<float>(h), 0.8f, false);
    }
    return cv.Encode();
}

std::vector<std::uint8_t> TitleBanner(const bc_levelmap::Common& c, const std::string& title,
                                      const std::string& subtitle, const Vec4& color, int w, int h) {
    Canvas cv(w, h);
    if (const Sprite* b = Ui(c, "Map_InfoParchment")) {
        // The banner's dark cloth takes the level's colour; its gold edge stays gold.
        Sprite tinted = *b;
        auto& px = tinted.tex.rgba;
        for (std::size_t i = 0; i < px.size(); i += 4) {
            const float r = px[i] / 255.0f, g = px[i + 1] / 255.0f, bl = px[i + 2] / 255.0f;
            const float mx = std::max({r, g, bl});
            const float gold = std::clamp((mx - 0.22f) / 0.25f, 0.0f, 1.0f);
            const float lum = 0.3f * r + 0.6f * g + 0.1f * bl;
            const float k = 0.30f + 1.3f * lum;
            const float nr = color.x * k, ng = color.y * k, nb = color.z * k;
            px[i] = static_cast<std::uint8_t>(std::clamp((nr + (r - nr) * gold) * 255.0f, 0.0f, 255.0f));
            px[i + 1] = static_cast<std::uint8_t>(std::clamp((ng + (g - ng) * gold) * 255.0f, 0.0f, 255.0f));
            px[i + 2] = static_cast<std::uint8_t>(std::clamp((nb + (bl - nb) * gold) * 255.0f, 0.0f, 255.0f));
        }
        const float sc = static_cast<float>(w) / static_cast<float>(tinted.tex.w);
        cv.Blit(tinted, 0, 0, static_cast<float>(w), static_cast<float>(tinted.tex.h) * sc);
        // Names on the ribbon: its band runs from ~6% to ~76% of the width, ~9% to ~30% of the height.
        const float ribbon_w = 0.66f * static_cast<float>(w);
        const float bh = static_cast<float>(tinted.tex.h) * sc;
        float big = 0.105f * bh, small = 0.062f * bh;
        big = std::min(big, big * ribbon_w / std::max(1.0f, Canvas::TextWidth(c.font, title, big)));
        small = std::min(small, small * ribbon_w / std::max(1.0f, Canvas::TextWidth(c.font, subtitle, small)));
        const float x = 0.08f * static_cast<float>(w);
        cv.Text(c.font, title, x, 0.205f * bh, big, kCream);
        if (!subtitle.empty())
            cv.Text(c.font, subtitle, x + 0.02f * static_cast<float>(w), 0.205f * bh + small * 1.25f, small, kCream);
    } else {
        cv.Text(c.font, title, 8, 0.45f * static_cast<float>(h), 0.42f * static_cast<float>(h), kGold);
        if (!subtitle.empty())
            cv.Text(c.font, subtitle, 10, 0.8f * static_cast<float>(h), 0.26f * static_cast<float>(h), kCream);
    }
    return cv.Encode();
}

std::vector<std::uint8_t> ZoomBar(const bc_levelmap::Common& c, int zoom, int w, int h) {
    using L = ZoomBarLayout;
    Canvas cv(w, h);
    const float sx = static_cast<float>(w) / L::kW, sy = static_cast<float>(h) / L::kH;
    const float bar_y = L::kBarY * sy;
    if (const Sprite* bar = Ui(c, "Map_Scale"))
        cv.Blit(*bar, (L::kBarX0 - 30) * sx, bar_y - 8 * sy, (L::kBarX1 - L::kBarX0 + 60) * sx, 16 * sy);
    const float btn = L::kButton * sx;
    if (const Sprite* out = Ui(c, "Map_ScaleNavigationDeZoom"))
        cv.Blit(*out, 0, bar_y - btn / 2, btn, btn);
    if (const Sprite* in = Ui(c, "Map_ScaleNavigationZoom"))
        cv.Blit(*in, static_cast<float>(w) - btn, bar_y - btn / 2, btn, btn);
    static const char* const kNames[3] = {"Area", "Sub-area", "Close"};
    for (int z = 0; z < 3; ++z) {
        const float tx = static_cast<float>(L::Tick(z)) * sx;
        const float size = 24 * sy;
        const float tw = Canvas::TextWidth(c.font, kNames[z], size);
        cv.Text(c.font, kNames[z], tx - tw / 2, bar_y + 58 * sy, size, z == zoom ? kGold : Vec4{0.70f, 0.66f, 0.60f, 1});
    }
    if (const Sprite* cur = Ui(c, "Map_ScaleCursor")) {
        const float cs = 64 * sy;
        const float tx = static_cast<float>(L::Tick(std::clamp(zoom, 0, 2))) * sx;
        cv.Blit(*cur, tx - cs / 2, bar_y - cs / 2, cs, cs);
    }
    return cv.Encode();
}

std::vector<std::uint8_t> Button(const bc_levelmap::Common& c, const std::string& text, int w, int h) {
    Canvas cv(w, h);
    if (const Sprite* f = Ui(c, "cartouche"))
        cv.Nine(*f, 17, 17, 17, 17, 0, 0, static_cast<float>(w), static_cast<float>(h), 1.0f, true);
    const float size = 0.46f * static_cast<float>(h);
    const float tw = Canvas::TextWidth(c.font, text, size);
    cv.Text(c.font, text, (static_cast<float>(w) - tw) / 2, 0.66f * static_cast<float>(h), size, kCream);
    return cv.Encode();
}

std::vector<std::uint8_t> Label(const bc_levelmap::Common& c, const std::string& text, float size, int w, int h) {
    Canvas cv(w, h);
    const float tw = Canvas::TextWidth(c.font, text, size);
    cv.Text(c.font, text, (static_cast<float>(w) - tw) / 2, 0.5f * static_cast<float>(h) + 0.35f * size, size,
            {0.80f, 0.75f, 0.66f, 1});
    return cv.Encode();
}

} // namespace bc_ui
