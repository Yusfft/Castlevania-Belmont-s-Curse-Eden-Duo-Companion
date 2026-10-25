// SPDX-License-Identifier: GPL-3.0-or-later
//
// The page's interface drawn with the game's own map-screen art (read from the player's romfs
// by bc_levelmap): the gold-edged frame of the map screen, the level banner, the zoom scale
// and its buttons, button frames and text in the game's font.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bc_levelmap.h"
#include "bc_shade.h"

namespace bc_ui {

/// Linear-light, premultiplied RGBA canvas, y down.
class Canvas {
public:
    Canvas(int w, int h);
    int W() const { return w_; }
    int H() const { return h_; }

    /// The sprite's texels [sx, sy, sx+sw, sy+sh] (pixels, y down) into the destination rect.
    void Blit(const bc_levelmap::Sprite& s, float sx, float sy, float sw, float sh, float dx, float dy, float dw,
              float dh, const bc_shade::Vec4& tint = {1, 1, 1, 1});
    /// The whole sprite into a rect.
    void Blit(const bc_levelmap::Sprite& s, float dx, float dy, float dw, float dh,
              const bc_shade::Vec4& tint = {1, 1, 1, 1});
    /// 9-slice: borders in texels (left, top, right, bottom), drawn at `scale` px per texel.
    void Nine(const bc_levelmap::Sprite& s, float l, float t, float r, float b, float dx, float dy, float dw,
              float dh, float scale, bool center, const bc_shade::Vec4& tint = {1, 1, 1, 1});
    /// Text in the game's font (small capitals like the game's menus). `color` is sRGB.
    /// Returns the advance width.
    float Text(const bc_levelmap::Font& font, const std::string& text, float x, float baseline, float size,
               const bc_shade::Vec4& color, bool small_caps = true, float shadow = 0.8f);
    static float TextWidth(const bc_levelmap::Font& font, const std::string& text, float size, bool small_caps = true);
    /// Black over one pixel with coverage `a`.
    void Darken(int x, int y, float a) { Over(x, y, 0, 0, 0, a); }
    /// An sRGB colour over one pixel with coverage `a`.
    void Paint(int x, int y, const bc_shade::Vec4& srgb, float a);
    /// Straight sRGB RGBA8.
    std::vector<std::uint8_t> Encode() const;
    /// Coverage for everything drawn next: `a` holds w * h values (0..1) for the rect at
    /// (x, y); outside it nothing is drawn.
    struct ClipMask {
        int x = 0, y = 0, w = 0, h = 0;
        std::vector<float> a;
        float At(int px, int py) const {
            const int u = px - x, v = py - y;
            return u < 0 || v < 0 || u >= w || v >= h ? 0.0f : a[static_cast<std::size_t>(v) * w + u];
        }
    };
    void SetClip(const ClipMask* clip) { clip_ = clip; }
    const ClipMask* Clip() const { return clip_; }
    /// The canvas's alpha (w * h values).
    std::vector<float> Alpha() const;
    /// Clears to transparent.
    void Clear();

private:
    void Over(int x, int y, float r, float g, float b, float a);
    int w_, h_;
    std::vector<float> px_;
    const ClipMask* clip_ = nullptr;
};

/// The map screen's frame around a w x h page (transparent inside).
std::vector<std::uint8_t> Frame(const bc_levelmap::Common& c, int w, int h);
/// An ember glow over the frame's gold (straight sRGB RGBA, mostly transparent); `strength`
/// scales it.
std::vector<std::uint8_t> FrameGlow(const bc_levelmap::Common& c, int w, int h, float strength);
/// The level banner with the level and district names. `color` is the level's banner colour.
std::vector<std::uint8_t> TitleBanner(const bc_levelmap::Common& c, const std::string& title,
                                      const std::string& subtitle, const bc_shade::Vec4& color, int w, int h);
/// The zoom scale: zoom-out button, scale with the cursor at `zoom` (0..kLevels-1), zoom-in
/// button, the levels' names.
std::vector<std::uint8_t> ZoomBar(const bc_levelmap::Common& c, int zoom, int w, int h);
/// The small zoom button: the scale's cursor and the current level's name.
std::vector<std::uint8_t> ZoomButton(const bc_levelmap::Common& c, int zoom, int w, int h);
/// A framed button with a caption; `active` lights it (a mode that is on).
std::vector<std::uint8_t> Button(const bc_levelmap::Common& c, const std::string& text, int w, int h,
                                 bool active = false);
/// An equipment cell: the game's cartouche frame, the item's name and a smaller line under it.
/// `equipped` writes the name in gold, `selected` lights the frame.
std::vector<std::uint8_t> ItemCell(const bc_levelmap::Common& c, const std::string& title, const std::string& sub,
                                   bool equipped, bool selected, int w, int h);
/// A square framed cell holding a sprite (a pin symbol); `selected` lights it.
std::vector<std::uint8_t> IconCell(const bc_levelmap::Common& c, const std::string& sprite, bool selected, int w,
                                   int h);
/// A line of text, centred (`align` 0) or right-aligned (1).
std::vector<std::uint8_t> Label(const bc_levelmap::Common& c, const std::string& text, float size, int w, int h,
                                int align = 0, const bc_shade::Vec4& color = {0.80f, 0.75f, 0.66f, 1});
/// "Press (A) to confirm": text with a button glyph in the game's prompt style, centred.
std::vector<std::uint8_t> Prompt(const bc_levelmap::Common& c, const std::string& before, const std::string& button,
                                 const std::string& after, float size, int w, int h);
/// The game-over card: the death icon, the game's ornament and "Game Over", with a glow of
/// strength `glow` (0..1) around them.
std::vector<std::uint8_t> GameOver(const bc_levelmap::Common& c, float glow, int w, int h);
/// A page with one of the game's painted biome cards (bc_cards): the card centred, `card_h` of
/// the page tall from `card_top` (fractions of the height), over a blurred, darkened copy of
/// itself filling the page. Empty when the card is.
std::vector<std::uint8_t> MenuArt(const bc_levelmap::Sprite& card, int w, int h, float card_h = 0.84f,
                                  float card_top = 0.05f);
/// "Loading, be patient" and `dots` dots (0..3), centred for the full line so it does not move.
std::vector<std::uint8_t> Loading(const bc_levelmap::Common& c, int dots, float size, int w, int h);
/// "Paused" between two ornaments.
std::vector<std::uint8_t> Paused(const bc_levelmap::Common& c, int w, int h);

/// The zoom levels' names, closest last.
extern const char* const kZoomNames[];

/// Layout of the zoom bar image (pixels), shared with the page's tap areas.
struct ZoomBarLayout {
    static constexpr int kLevels = 3;
    static constexpr int kW = 900, kH = 130;
    static constexpr int kButton = 84;               // the two buttons, square, at the ends
    static constexpr int kBarX0 = 150, kBarX1 = 750; // the scale (its ticks)
    static constexpr int kBarY = 44;
    static int Tick(int zoom) { return kBarX0 + (kBarX1 - kBarX0) * zoom / (kLevels - 1); }
};

} // namespace bc_ui
