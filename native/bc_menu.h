// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game's own pause menu (Equipment, Tarot, Journey, System), drawn from its scene.
//
// The menu is a Unity UI (uGUI + TextMeshPro) hierarchy in the game's
// afm_scenes_scenes_gamemenu bundle. This reads that hierarchy at runtime from the player's
// romfs - GameObjects, RectTransforms, Images (their sprites, in the game's sprite atlases),
// TextMeshPro texts (their English strings), layout groups, layout elements, content size and
// aspect ratio fitters, canvas groups and masks - lays it out the way Unity does for the page's
// canvas and draws it with the game's art and font. The module then fills in what the game fills
// in at runtime (the equipped weapon's illustration, the arcana card, the relics, the stats).
//
// Nothing read here is stored or shipped.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "bc_levelmap.h"
#include "bc_ui.h"

namespace bc_menu {

struct Rgba {
    float r = 1, g = 1, b = 1, a = 1;
};

/// A sprite of the menu: its texels (possibly reduced) and its geometry in sprite pixels.
struct MenuSprite {
    bc_levelmap::Sprite s;     // texels (rows bottom-up), border unused
    std::string name;
    float rect_w = 0, rect_h = 0; // the sprite rect, in sprite pixels
    float ppu = 100;              // sprite pixels per unit
    float border[4]{};            // left, bottom, right, top, in sprite pixels
    float texel_scale = 1;        // texels per sprite pixel (< 1 when reduced)
};
using SpriteP = std::shared_ptr<const MenuSprite>;

struct Image {
    std::int64_t pid = 0;        // the Image component (targets of the game's displays)
    SpriteP sprite;              // null: a plain rectangle in `color`
    Rgba color;
    int type = 0;                // 0 simple, 1 sliced, 2 tiled, 3 filled
    bool preserve_aspect = false, fill_center = true;
    float fill_amount = 1;
    int fill_method = 0, fill_origin = 0;
    float ppu_multiplier = 1;
    bool runtime = false;        // a null sprite the game fills in: not drawn until set
};

struct Text {
    std::int64_t pid = 0;        // the TextMeshProUGUI component
    std::string text;            // already localized
    float size = 36, size_min = 18, size_max = 72;
    bool autosize = false;
    int style = 0;               // TMP FontStyles: 16 upper, 8 lower, 32 small caps
    int halign = 2, valign = 512;
    Rgba color;
    bool wrap = true;
    float margin[4]{};           // left, top, right, bottom
};

struct LayoutElement {
    bool ignore = false;
    float min[2]{-1, -1}, preferred[2]{-1, -1}, flexible[2]{-1, -1};
    int priority = 1;
};

struct Group {
    enum Kind { Horizontal, Vertical, Grid } kind = Horizontal;
    float pad_l = 0, pad_r = 0, pad_t = 0, pad_b = 0;
    float spacing = 0;           // horizontal / vertical
    float spacing_xy[2]{};       // grid
    float cell[2]{};             // grid
    int align = 0;               // TextAnchor 0..8
    bool control[2]{}, expand[2]{}, use_scale[2]{};
    int start_corner = 0, start_axis = 0, constraint = 0, constraint_count = 1;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0; // canvas units, y up, (x, y) = bottom left
    bool Contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

struct Node {
    std::string name;
    bool active = true;
    bool is_rect = false;
    float amin[2]{}, amax[2]{1, 1}, pos[2]{}, size[2]{}, pivot[2]{0.5f, 0.5f}, scale[2]{1, 1};
    std::vector<Image> images;
    std::vector<Text> texts;
    std::optional<LayoutElement> le;
    std::optional<Group> group;
    int fit[2]{};                // ContentSizeFitter: 0 none, 1 min, 2 preferred
    bool has_fitter = false;
    int aspect_mode = 0;         // AspectRatioFitter
    float aspect_ratio = 1;
    float alpha = 1;             // CanvasGroup
    int mask = 0;                // 1 Mask (stencil), 2 RectMask2D
    bool mask_show = true;
    std::vector<int> children;
    int parent = -1;
    // Layout results.
    Rect rect;
    bool driven = false;
};

/// A game component kept for its references (the game's display scripts, BooleanUIViews).
struct Comp {
    std::string script;
    int node = -1;
    std::int64_t pid = 0;
    std::shared_ptr<const bc_unity::Value> v; // its fields
    std::string item_id;                      // the item a slot is for (itemID, or its item data's)
};

/// The menu hierarchy under one root (GameMenuView), with its sprites loaded.
struct Scene {
    std::vector<Node> nodes;
    int root = -1;
    std::string log;
    std::vector<Comp> comps;
    std::unordered_map<std::int64_t, int> comp_by_pid; // kept component -> comps index
    std::unordered_map<std::int64_t, int> node_by_obj; // GameObject or component -> node

    /// The node a same-file PPtr (to a GameObject or a component) points to, or -1.
    int NodeOf(const bc_unity::Value* pptr) const;
    /// The kept component a same-file PPtr points to, or null.
    const Comp* CompOf(const bc_unity::Value* pptr) const;
    /// The kept component of class `script` on node `node`, or null.
    const Comp* CompOn(int node, const std::string& script) const;
    /// Runs a UnityEvent's persistent calls the menu cares about: GameObject.SetActive and
    /// UIView / UIContainer Show / Hide on the nodes they target.
    void RunEvent(const bc_unity::Value* unity_event);
    /// A node's image (by its Image component's path id), or null.
    Image* ImageOf(const bc_unity::Value* pptr);
    Text* TextOf(const bc_unity::Value* pptr);

    /// The first node named `name` under `under` (depth first; `under` itself included).
    int Find(const std::string& name, int under = -1) const;
    /// Finds along a path of names ("Weapon/WeaponImage"), each under the previous.
    int Path(const std::string& path, int under = -1) const;
    /// Copies the subtree at `node` as the last child of `parent` (as Instantiate does with a
    /// template); returns the copy's index. Its components keep the template's ids.
    int Clone(int node, int parent);
};

/// The game's sprites by name (item icons, illustrations), and by reference.
class SpriteBank {
public:
    SpriteBank(bc_levelmap::Assets& assets, const bc_levelmap::Codecs& codecs) : assets_(assets), codecs_(codecs) {}
    /// A Sprite object (atlased or not), reduced to at most `max_texels` on its longer side.
    SpriteP Load(const std::shared_ptr<bc_levelmap::File>& f, const bc_unity::ObjectInfo& obj, int max_texels = 512);
    /// The sprite a PPtr in `from` points to.
    SpriteP Resolve(const std::shared_ptr<bc_levelmap::File>& from, const bc_unity::Value* pptr, int max_texels = 512);
    /// A sprite by name, looked for in the bundles starting with one of `prefixes` (indexed once).
    SpriteP ByName(const std::string& name, const std::vector<std::string>& prefixes, int max_texels = 512);
    /// Drops decoded textures kept between loads.
    void DropTextures() { textures_.clear(); }
    /// Halves sprites larger than 128 texels this many times (for a page drawn well below the
    /// game's 4K reference size).
    void SetReduce(int halvings) { reduce_ = halvings; }
    /// Texel bytes held (sprites and decoded textures).
    std::size_t Bytes() const {
        std::size_t n = 0;
        for (const auto& [k, sp] : sprites_)
            n += sp ? sp->s.tex.rgba.size() : 0;
        for (const auto& [k, t] : textures_)
            n += t ? t->rgba.size() : 0;
        return n;
    }

private:
    std::shared_ptr<const bc_levelmap::Texture> Texture(const std::shared_ptr<bc_levelmap::File>& f,
                                                        const bc_unity::ObjectInfo& obj);
    bc_levelmap::Assets& assets_;
    const bc_levelmap::Codecs& codecs_;
    std::map<std::pair<std::string, std::int64_t>, SpriteP> sprites_;
    std::map<std::pair<std::string, std::int64_t>, std::shared_ptr<const bc_levelmap::Texture>> textures_;
    std::map<std::string, std::pair<std::shared_ptr<bc_levelmap::File>, std::int64_t>> names_;
    std::vector<std::string> indexed_;
    int reduce_ = 0;
};

/// Reads the menu hierarchy under `root_name` from the game menu scene. Subtrees whose root's
/// name is in `skip` are left out. Null on failure (`log` says why).
std::shared_ptr<Scene> LoadScene(bc_levelmap::Assets& assets, SpriteBank& sprites,
                                 const bc_levelmap::Strings& strings, const std::string& root_name,
                                 const std::vector<std::string>& skip, std::string* log);

/// The canvas size (units) the game's CanvasScaler gives a screen: reference 3840 x 2160,
/// matching width for `match` 0 and height for 1.
void CanvasSize(int screen_w, int screen_h, float match, float& canvas_w, float& canvas_h, float& scale);

/// Lays the scene out in a canvas of cw x ch units (Unity's RectTransform and layout rules).
void Layout(Scene& scene, const bc_levelmap::Font& font, float cw, float ch);

/// Draws the scene (active nodes, `hide` returning true skips a subtree) at `scale` pixels per
/// unit into a w x h RGBA8 image.
std::vector<std::uint8_t> Render(const Scene& scene, const bc_levelmap::Font& font, float scale, int w, int h,
                                 const std::vector<bool>* hide = nullptr);

/// A part of a page drawn on its own: subtrees of the laid-out scene at their own scale and
/// place (the game's 16:9 screen rearranged for another shape). A page pixel is
/// (unit_x * k - ox, (ch - unit_y) * k - oy).
struct Part {
    std::vector<int> nodes;   // subtrees drawn, in order
    std::vector<int> exclude; // subtrees left out of them
    float k = 1, ox = 0, oy = 0;
};
/// The part's transform putting the rect `src` (units) at page pixels (x, y, w, h): scaled to fit
/// (at most `k_max`), centred in the box (at its top with `top`).
void Fit(Part& part, const Rect& src, float ch, float x, float y, float w, float h, float k_max = 1e9f,
         bool top = false);
/// What a subtree draws (active, not hidden, with an image or a text), as one rect in units;
/// w = 0 when nothing.
Rect Bounds(const Scene& scene, int node, const std::vector<bool>* hide);
/// Draws the parts, in order, into a w x h page (`ch`: the layout's canvas height, units).
std::vector<std::uint8_t> RenderParts(const Scene& scene, const bc_levelmap::Font& font, float ch, int w, int h,
                                      const std::vector<Part>& parts, const std::vector<bool>* hide);
/// A node's rect in page pixels as a part draws it.
void PartRect(const Part& part, const Rect& r, float ch, float& x, float& y, float& w, float& h);

/// A node's rect in pixels (x, y top-left, w, h) for a canvas drawn at `scale`, ch units tall.
void PixelRect(const Rect& r, float scale, float ch, float& x, float& y, float& w, float& h);

} // namespace bc_menu
