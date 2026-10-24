// SPDX-License-Identifier: GPL-3.0-or-later
// Interface drawing with the game's map-screen art. See bc_ui.h.

#include "bc_ui.h"

#include <cstring>

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

void Canvas::Paint(int x, int y, const Vec4& srgb, float a) {
    const Vec4 l = Lin(srgb);
    Over(x, y, l.x * a, l.y * a, l.z * a, a);
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
        // Darkened: a bright gold edge around the whole screen draws the eye from the map.
        cv.Nine(*bg, 48, 48, 48, 48, 0, 0, static_cast<float>(w), static_cast<float>(h), 0.8f, false,
                {0.16f, 0.14f, 0.17f, 1.0f});
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

const char* const kZoomNames[] = {"Area", "Far", "Mid", "Near", "Close"};

std::vector<std::uint8_t> ZoomBar(const bc_levelmap::Common& c, int zoom, int w, int h) {
    using L = ZoomBarLayout;
    Canvas cv(w, h);
    const float sx = static_cast<float>(w) / L::kW, sy = static_cast<float>(h) / L::kH;
    const float bar_y = L::kBarY * sy;
    // A dark backing so the bar reads over the map.
    for (int y = 6; y < h - 6; ++y)
        for (int x = 6; x < w - 6; ++x)
            cv.Darken(x, y, 0.82f);
    if (const Sprite* f = Ui(c, "cartouche"))
        cv.Nine(*f, 17, 17, 17, 17, 0, 0, static_cast<float>(w), static_cast<float>(h), 1.0f, true,
                {0.30f, 0.27f, 0.30f, 1.0f});
    if (const Sprite* bar = Ui(c, "Map_Scale"))
        cv.Blit(*bar, (L::kBarX0 - 30) * sx, bar_y - 8 * sy, (L::kBarX1 - L::kBarX0 + 60) * sx, 16 * sy);
    const float btn = L::kButton * sx;
    if (const Sprite* out = Ui(c, "Map_ScaleNavigationDeZoom"))
        cv.Blit(*out, 12 * sx, bar_y - btn / 2 + 6 * sy, btn, btn);
    if (const Sprite* in = Ui(c, "Map_ScaleNavigationZoom"))
        cv.Blit(*in, static_cast<float>(w) - btn - 12 * sx, bar_y - btn / 2 + 6 * sy, btn, btn);
    for (int z = 0; z < L::kLevels; ++z) {
        const float tx = static_cast<float>(L::Tick(z)) * sx;
        const float size = 24 * sy;
        const float tw = Canvas::TextWidth(c.font, kZoomNames[z], size);
        cv.Text(c.font, kZoomNames[z], tx - tw / 2, bar_y + 58 * sy, size,
                z == zoom ? kGold : Vec4{0.70f, 0.66f, 0.60f, 1});
    }
    if (const Sprite* cur = Ui(c, "Map_ScaleCursor")) {
        const float cs = 64 * sy;
        const float tx = static_cast<float>(L::Tick(std::clamp(zoom, 0, L::kLevels - 1))) * sx;
        cv.Blit(*cur, tx - cs / 2, bar_y - cs / 2, cs, cs);
    }
    return cv.Encode();
}

std::vector<std::uint8_t> ZoomButton(const bc_levelmap::Common& c, int zoom, int w, int h) {
    Canvas cv(w, h);
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    if (const Sprite* f = Ui(c, "cartouche"))
        cv.Nine(*f, 17, 17, 17, 17, 0, 0, fw, fh, 1.0f, true);
    const float cs = 0.78f * fh;
    if (const Sprite* cur = Ui(c, "Map_ScaleCursor"))
        cv.Blit(*cur, 0.10f * fh, (fh - cs) / 2, cs, cs);
    const std::string name = kZoomNames[std::clamp(zoom, 0, ZoomBarLayout::kLevels - 1)];
    const float size = 0.44f * fh;
    const float left = 0.10f * fh + cs, room = fw - left - 0.12f * fh;
    const float tw = Canvas::TextWidth(c.font, name, size);
    cv.Text(c.font, name, left + (room - tw) / 2, 0.65f * fh, size, kCream);
    return cv.Encode();
}

std::vector<std::uint8_t> Button(const bc_levelmap::Common& c, const std::string& text, int w, int h, bool active) {
    Canvas cv(w, h);
    if (const Sprite* f = Ui(c, "cartouche"))
        cv.Nine(*f, 17, 17, 17, 17, 0, 0, static_cast<float>(w), static_cast<float>(h), 1.0f, true,
                active ? Vec4{1.35f, 1.15f, 0.70f, 1.0f} : Vec4{1, 1, 1, 1});
    const float size = 0.46f * static_cast<float>(h);
    const float tw = Canvas::TextWidth(c.font, text, size);
    cv.Text(c.font, text, (static_cast<float>(w) - tw) / 2, 0.66f * static_cast<float>(h), size,
            active ? kGold : kCream);
    return cv.Encode();
}

std::vector<std::uint8_t> IconCell(const bc_levelmap::Common& c, const std::string& sprite, bool selected, int w,
                                   int h) {
    Canvas cv(w, h);
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    if (const Sprite* f = Ui(c, "cartouche"))
        cv.Nine(*f, 17, 17, 17, 17, 0, 0, fw, fh, 1.0f, true,
                selected ? Vec4{1.35f, 1.15f, 0.70f, 1.0f} : Vec4{0.55f, 0.52f, 0.56f, 1.0f});
    if (selected) {
        // A gold ring inside the frame.
        const float r0 = 0.5f * std::min(fw, fh) - 9, r1 = r0 - 3;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float dx = std::max(std::fabs(static_cast<float>(x) + 0.5f - fw / 2) - (fw - fh) / 2, 0.0f);
                const float dy = std::fabs(static_cast<float>(y) + 0.5f - fh / 2);
                const float d = std::max(dx, dy);
                if (d <= r0 && d >= r1)
                    cv.Paint(x, y, kGold, 0.9f);
            }
        }
    }
    if (const Sprite* s = Ui(c, sprite.c_str())) {
        const float box = 0.66f * std::min(fw, fh);
        const float tw = static_cast<float>(s->tex.w), th = static_cast<float>(s->tex.h);
        const float k = box / std::max(tw, th);
        const float iw = tw * k, ih = th * k;
        cv.Blit(*s, (fw - iw) / 2, (fh - ih) / 2, iw, ih,
                selected ? Vec4{1, 1, 1, 1} : Vec4{0.75f, 0.75f, 0.75f, 1});
    }
    return cv.Encode();
}

std::vector<std::uint8_t> Label(const bc_levelmap::Common& c, const std::string& text, float size, int w, int h,
                                int align, const Vec4& color) {
    Canvas cv(w, h);
    const float tw = Canvas::TextWidth(c.font, text, size);
    const float x = align == 1 ? static_cast<float>(w) - tw - 0.2f * size : (static_cast<float>(w) - tw) / 2;
    cv.Text(c.font, text, x, 0.5f * static_cast<float>(h) + 0.35f * size, size, color);
    return cv.Encode();
}

std::vector<std::uint8_t> Prompt(const bc_levelmap::Common& c, const std::string& before, const std::string& button,
                                 const std::string& after, float size, int w, int h) {
    Canvas cv(w, h);
    const Vec4 text{0.80f, 0.75f, 0.66f, 1};
    const float r = 0.62f * size; // the button glyph: a gold ring with the letter
    const float gap = 0.35f * size;
    const float w1 = Canvas::TextWidth(c.font, before, size), w2 = Canvas::TextWidth(c.font, after, size);
    const float total = w1 + gap + 2 * r + gap + w2;
    float x = (static_cast<float>(w) - total) / 2;
    const float base = 0.5f * static_cast<float>(h) + 0.35f * size;
    cv.Text(c.font, before, x, base, size, text);
    x += w1 + gap;
    const float cx = x + r, cy = 0.5f * static_cast<float>(h);
    for (int y = static_cast<int>(cy - r - 2); y <= static_cast<int>(cy + r + 2); ++y) {
        for (int px = static_cast<int>(cx - r - 2); px <= static_cast<int>(cx + r + 2); ++px) {
            const float d = std::hypot(static_cast<float>(px) + 0.5f - cx, static_cast<float>(y) + 0.5f - cy);
            const float ring = std::clamp(1.0f - std::fabs(d - (r - 1.5f)) / 1.6f, 0.0f, 1.0f);
            const float fill = std::clamp(r - 2.0f - d, 0.0f, 1.0f);
            if (fill > 0)
                cv.Paint(px, y, {0.10f, 0.08f, 0.07f, 1}, 0.85f * fill);
            if (ring > 0)
                cv.Paint(px, y, kGold, ring);
        }
    }
    const float bs = 1.05f * size;
    const float bw = Canvas::TextWidth(c.font, button, bs, false);
    cv.Text(c.font, button, cx - bw / 2, cy + 0.36f * bs, bs, kGold, false);
    x += 2 * r + gap;
    cv.Text(c.font, after, x, base, size, text);
    return cv.Encode();
}

namespace {
/// A soft disc of light (sRGB colour) for glows.
void Glow(Canvas& cv, float cx, float cy, float rx, float ry, const Vec4& color, float strength) {
    if (strength <= 0)
        return;
    const int x0 = static_cast<int>(cx - rx), x1 = static_cast<int>(cx + rx);
    const int y0 = static_cast<int>(cy - ry), y1 = static_cast<int>(cy + ry);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float dx = (static_cast<float>(x) + 0.5f - cx) / rx, dy = (static_cast<float>(y) + 0.5f - cy) / ry;
            const float d = dx * dx + dy * dy;
            if (d >= 1)
                continue;
            const float t = (1 - d) * (1 - d);
            cv.Paint(x, y, color, strength * t);
        }
    }
}
} // namespace

std::vector<std::uint8_t> GameOver(const bc_levelmap::Common& c, float glow, int w, int h) {
    Canvas cv(w, h);
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    // A soft dark backing (no edges), then the glow behind the skull.
    Glow(cv, fw / 2, fh / 2, 0.50f * fw, 0.50f * fh, {0, 0, 0, 1}, 0.75f);
    Glow(cv, fw / 2, 0.36f * fh, 0.30f * fw, 0.34f * fh, {0.75f, 0.10f, 0.08f, 1}, 0.55f * glow);
    Glow(cv, fw / 2, 0.36f * fh, 0.16f * fw, 0.20f * fh, {1.0f, 0.78f, 0.45f, 1}, 0.35f * glow);
    if (const Sprite* skull = Ui(c, "Map_DeathLocationIcon")) {
        const float s = 0.42f * fh;
        const float k = s / std::max(1.0f, static_cast<float>(skull->tex.h));
        const float sw = static_cast<float>(skull->tex.w) * k;
        cv.Blit(*skull, (fw - sw) / 2, 0.36f * fh - s / 2, sw, s);
    }
    const float size = 0.15f * fh;
    const std::string title = "Game Over";
    const float tw = Canvas::TextWidth(c.font, title, size);
    const Vec4 col{0.92f + 0.08f * glow, 0.80f + 0.12f * glow, 0.55f + 0.25f * glow, 1};
    cv.Text(c.font, title, (fw - tw) / 2, 0.74f * fh, size, col);
    if (const Sprite* orn = Ui(c, "CommonUI_Ornament_01")) {
        // Under the title, kept inside the card.
        float ow = 0.45f * fw;
        float oh = ow * static_cast<float>(orn->tex.h) / std::max(1.0f, static_cast<float>(orn->tex.w));
        const float room = 0.95f * fh - 0.79f * fh;
        if (oh > room)
            ow *= room / oh, oh = room;
        cv.Blit(*orn, (fw - ow) / 2, 0.79f * fh, ow, oh);
    }
    return cv.Encode();
}

std::vector<std::uint8_t> MenuArt(const Sprite& card_sprite, int w, int h, float card_h, float card_top) {
    const Sprite* card = &card_sprite;
    if (card->tex.Empty())
        return {};
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    const float aw = static_cast<float>(card->tex.w), ah = static_cast<float>(card->tex.h);
    // Background: the card covering the page, drawn small, blurred, then stretched back.
    constexpr int kDiv = 8;
    const int sw = std::max(1, w / kDiv), sh = std::max(1, h / kDiv);
    Canvas small(sw, sh);
    {
        const float k = std::max(static_cast<float>(sw) / aw, static_cast<float>(sh) / ah);
        small.Blit(*card, (static_cast<float>(sw) - aw * k) / 2, (static_cast<float>(sh) - ah * k) / 2, aw * k, ah * k);
    }
    std::vector<std::uint8_t> px = small.Encode();
    for (int pass = 0; pass < 3; ++pass) {
        std::vector<std::uint8_t> out(px.size());
        for (int y = 0; y < sh; ++y) {
            for (int x = 0; x < sw; ++x) {
                int acc[4] = {0, 0, 0, 0}, n = 0;
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int xx = std::clamp(x + dx, 0, sw - 1), yy = std::clamp(y + dy, 0, sh - 1);
                        const std::uint8_t* p = &px[(static_cast<std::size_t>(yy) * sw + xx) * 4];
                        for (int k = 0; k < 4; ++k)
                            acc[k] += p[k];
                        ++n;
                    }
                }
                for (int k = 0; k < 4; ++k)
                    out[(static_cast<std::size_t>(y) * sw + x) * 4 + k] = static_cast<std::uint8_t>(acc[k] / n);
            }
        }
        px.swap(out);
    }
    Sprite bg;
    bg.tex.w = static_cast<std::uint32_t>(sw);
    bg.tex.h = static_cast<std::uint32_t>(sh);
    bg.tex.slices = 1;
    bg.tex.srgb = true;
    bg.tex.rgba.resize(px.size());
    for (int y = 0; y < sh; ++y) // stored rows run bottom-up, like the game's textures
        std::memcpy(bg.tex.rgba.data() + static_cast<std::size_t>(sh - 1 - y) * sw * 4,
                    px.data() + static_cast<std::size_t>(y) * sw * 4, static_cast<std::size_t>(sw) * 4);
    Canvas cv(w, h);
    cv.Blit(bg, 0, 0, fw, fh, {0.38f, 0.36f, 0.40f, 1});
    // The card itself, with a soft shadow and a thin gold edge.
    const float ch = card_h * fh, cw = ch * aw / ah;
    const float x0 = (fw - cw) / 2, y0 = card_top * fh;
    for (int y = static_cast<int>(y0 - 30); y < static_cast<int>(y0 + ch + 30); ++y) {
        for (int x = static_cast<int>(x0 - 30); x < static_cast<int>(x0 + cw + 30); ++x) {
            const float dx = std::max({x0 - static_cast<float>(x), static_cast<float>(x) - (x0 + cw), 0.0f});
            const float dy = std::max({y0 - static_cast<float>(y), static_cast<float>(y) - (y0 + ch), 0.0f});
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d > 0 && d < 30)
                cv.Darken(x, y, 0.6f * (1 - d / 30) * (1 - d / 30));
        }
    }
    cv.Blit(*card, x0, y0, cw, ch);
    for (int i = 0; i < 2; ++i) {
        const float a = i == 0 ? 0.9f : 0.35f;
        const int l = static_cast<int>(x0) - 1 - i, r = static_cast<int>(x0 + cw) + i;
        const int t = static_cast<int>(y0) - 1 - i, b = static_cast<int>(y0 + ch) + i;
        for (int x = l; x <= r; ++x) {
            cv.Paint(x, t, kGold, a);
            cv.Paint(x, b, kGold, a);
        }
        for (int y = t; y <= b; ++y) {
            cv.Paint(l, y, kGold, a);
            cv.Paint(r, y, kGold, a);
        }
    }
    // Darker towards the bottom, under the prompt.
    for (int y = static_cast<int>(0.80f * fh); y < h; ++y) {
        const float t = (static_cast<float>(y) - 0.80f * fh) / (0.20f * fh);
        for (int x = 0; x < w; ++x)
            cv.Darken(x, y, 0.75f * t * t);
    }
    return cv.Encode();
}

std::vector<std::uint8_t> Loading(const bc_levelmap::Common& c, int dots, float size, int w, int h) {
    Canvas cv(w, h);
    const std::string text = "Loading, be patient";
    // Placed for the full line, so the text stays put while the dots come and go.
    const float full = Canvas::TextWidth(c.font, text + "...", size);
    const float x = (static_cast<float>(w) - full) / 2;
    const float base = 0.5f * static_cast<float>(h) + 0.35f * size;
    const Vec4 col{0.80f, 0.75f, 0.66f, 1};
    const float tw = cv.Text(c.font, text, x, base, size, col);
    cv.Text(c.font, std::string(static_cast<std::size_t>(std::clamp(dots, 0, 3)), '.'), x + tw, base, size, col);
    return cv.Encode();
}

std::vector<std::uint8_t> Paused(const bc_levelmap::Common& c, int w, int h) {
    Canvas cv(w, h);
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    const float size = 0.42f * fh;
    const std::string text = "Paused";
    const float tw = Canvas::TextWidth(c.font, text, size);
    cv.Text(c.font, text, (fw - tw) / 2, 0.52f * fh, size, kGold);
    if (const Sprite* orn = Ui(c, "CommonUI_Ornament_01")) {
        const float ow = std::min(0.8f * fw, tw * 1.4f);
        const float oh = ow * static_cast<float>(orn->tex.h) / std::max(1.0f, static_cast<float>(orn->tex.w));
        cv.Blit(*orn, (fw - ow) / 2, 0.62f * fh, ow, oh);
    }
    return cv.Encode();
}

} // namespace bc_ui
