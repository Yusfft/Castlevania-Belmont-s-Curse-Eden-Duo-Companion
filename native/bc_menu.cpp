// SPDX-License-Identifier: GPL-3.0-or-later
// The game's pause menu from its own scene. See bc_menu.h.

#include "bc_menu.h"

#include <algorithm>
#include <deque>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>

namespace bc_menu {
namespace {

using bc_levelmap::Assets;
using bc_levelmap::File;
using bc_unity::ObjectInfo;
using bc_unity::Value;

constexpr int kClassGameObject = 1;
constexpr int kClassTransform = 4;
constexpr int kClassTexture2D = 28;
constexpr int kClassMonoBehaviour = 114;
constexpr int kClassSprite = 213;
constexpr int kClassRectTransform = 224;
constexpr int kClassCanvasGroup = 225;
constexpr int kClassSpriteAtlas = 687078895;

// Where the menu's art lives.
const std::vector<std::string> kArtPrefixes = {"atlas-", "ui-", "uidefault", "skins", "charms-", "weapon-",
                                               "arcana-", "item-", "fonts-"};

std::int64_t Int(const Value* v, std::int64_t fb = 0) {
    return v ? v->AsInt(fb) : fb;
}
float Flt(const Value* v, float fb = 0) {
    return v ? static_cast<float>(v->AsFloat(fb)) : fb;
}
const std::string& Str(const Value* v) {
    static const std::string empty;
    return v && v->kind == Value::Kind::String ? v->s : empty;
}
bool PPtr(const Value* v, std::int64_t& fid, std::int64_t& pid) {
    if (!v)
        return false;
    fid = Int(v->Get("m_FileID"));
    pid = Int(v->Get("m_PathID"));
    return pid != 0;
}
void Vec2(const Value* v, float out[2], float fb = 0) {
    out[0] = v ? Flt(v->Get("x"), fb) : fb;
    out[1] = v ? Flt(v->Get("y"), fb) : fb;
}
Rgba Color(const Value* v) {
    Rgba c;
    if (v) {
        c.r = Flt(v->Get("r"), 1), c.g = Flt(v->Get("g"), 1), c.b = Flt(v->Get("b"), 1), c.a = Flt(v->Get("a"), 1);
    }
    return c;
}

/// Structural equality of two values (render data keys).
bool Same(const Value* a, const Value* b) {
    if (!a || !b || a->kind != b->kind || a->list.size() != b->list.size())
        return false;
    switch (a->kind) {
    case Value::Kind::Int: return a->i == b->i;
    case Value::Kind::Float: return a->f == b->f;
    case Value::Kind::String:
    case Value::Kind::Bytes: return a->s == b->s;
    default: break;
    }
    for (std::size_t i = 0; i < a->list.size(); ++i)
        if (!Same(&a->list[i], &b->list[i]))
            return false;
    return true;
}

// ---- UTF-8 and text runs

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

std::string Encode(std::uint32_t cp) {
    std::string o;
    if (cp < 0x80) {
        o += static_cast<char>(cp);
    } else if (cp < 0x800) {
        o += static_cast<char>(0xC0 | (cp >> 6));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o += static_cast<char>(0xE0 | (cp >> 12));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        o += static_cast<char>(0xF0 | (cp >> 18));
        o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return o;
}

std::uint32_t Upper(std::uint32_t c) {
    if (c >= 'a' && c <= 'z')
        return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7)
        return c - 32;
    if (c == 0x153) // oe
        return 0x152;
    return c;
}
std::uint32_t Lower(std::uint32_t c) {
    if (c >= 'A' && c <= 'Z')
        return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
        return c + 32;
    return c;
}

struct Ch {
    std::uint32_t cp;
    float k;   // size multiplier
    float adv; // advance in units (filled by line layout)
};

/// Characters after rich-text tags (<size=N%> kept, others dropped) and the font style.
std::vector<Ch> Runs(const std::string& text, int style) {
    std::vector<Ch> out;
    float mul = 1;
    const auto cps = Utf8(text);
    for (std::size_t i = 0; i < cps.size(); ++i) {
        if (cps[i] == '<') {
            std::size_t j = i + 1;
            while (j < cps.size() && cps[j] != '>' && j - i < 64)
                ++j;
            if (j < cps.size() && cps[j] == '>') {
                std::string tag;
                for (std::size_t k = i + 1; k < j; ++k)
                    tag += static_cast<char>(cps[k] < 0x80 ? cps[k] : '?');
                if (tag.rfind("size=", 0) == 0) {
                    const float v = std::strtof(tag.c_str() + 5, nullptr);
                    mul = tag.back() == '%' ? v / 100.0f : 1.0f;
                } else if (tag == "/size") {
                    mul = 1;
                } else if (tag == "br") {
                    out.push_back({'\n', mul, 0});
                }
                i = j;
                continue;
            }
        }
        std::uint32_t c = cps[i];
        float k = mul;
        if (style & 16)
            c = Upper(c);
        if (style & 8)
            c = Lower(c);
        if ((style & 32) && Upper(c) != c) {
            c = Upper(c);
            k *= 0.8f;
        }
        out.push_back({c, k, 0});
    }
    return out;
}

struct Line {
    std::vector<Ch> chars;
    float width = 0;
};

std::vector<Line> LayoutLines(const bc_levelmap::Font& font, std::vector<Ch> chars, float size, float width, bool wrap) {
    std::vector<Line> lines(1);
    const float pt = font.point_size > 0 ? font.point_size : 48;
    int last_space = -1;
    for (auto c : chars) {
        if (c.cp == '\n') {
            lines.emplace_back();
            last_space = -1;
            continue;
        }
        const auto it = font.glyphs.find(c.cp);
        c.adv = (it == font.glyphs.end() ? pt * 0.3f : it->second.advance) * size * c.k / pt;
        Line& cur = lines.back();
        if (wrap && width > 0 && cur.width + c.adv > width + 0.5f && !cur.chars.empty()) {
            Line next;
            if (last_space >= 0) {
                next.chars.assign(cur.chars.begin() + last_space + 1, cur.chars.end());
                cur.chars.resize(static_cast<std::size_t>(last_space));
                cur.width = 0;
                for (const auto& x : cur.chars)
                    cur.width += x.adv;
                for (const auto& x : next.chars)
                    next.width += x.adv;
            }
            lines.push_back(std::move(next));
            last_space = -1;
        }
        Line& line = lines.back();
        if (c.cp == ' ')
            last_space = static_cast<int>(line.chars.size());
        line.chars.push_back(c);
        line.width += c.adv;
    }
    return lines;
}

void TextMetrics(const bc_levelmap::Font& font, const Text& t, float wrap_width, float& pw, float& ph) {
    // TextMeshPro's preferred size includes the text's margins.
    const float mh = t.margin[0] + t.margin[2], mv = t.margin[1] + t.margin[3];
    if (wrap_width > 0)
        wrap_width = std::max(1.0f, wrap_width - mh);
    const auto lines = LayoutLines(font, Runs(t.text, t.style), t.size, wrap_width, t.wrap && wrap_width > 0);
    const float pt = font.point_size > 0 ? font.point_size : 48;
    const float k = t.size / pt;
    pw = 0;
    for (const auto& l : lines)
        pw = std::max(pw, l.width);
    pw += mh;
    ph = (font.ascender - font.descender) * k + static_cast<float>(lines.size() - 1) * font.line_height * k + mv;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Sprites

std::shared_ptr<const bc_levelmap::Texture> SpriteBank::Texture(const std::shared_ptr<File>& f, const ObjectInfo& obj) {
    const auto key = std::make_pair(f->cab, obj.path_id);
    if (const auto it = textures_.find(key); it != textures_.end())
        return it->second;
    std::string err;
    std::shared_ptr<const bc_levelmap::Texture> t;
    if (auto tex = bc_levelmap::LoadTexture(*f, obj, codecs_, &err))
        t = std::make_shared<const bc_levelmap::Texture>(std::move(*tex));
    textures_[key] = t;
    return t;
}

SpriteP SpriteBank::Load(const std::shared_ptr<File>& f, const ObjectInfo& obj, int max_texels) {
    if (!f || !f->sf || obj.class_id != kClassSprite)
        return nullptr;
    const auto key = std::make_pair(f->cab, obj.path_id);
    if (const auto it = sprites_.find(key); it != sprites_.end())
        return it->second;
    sprites_[key] = nullptr;
    const auto v = f->sf->ReadObject(obj);
    if (!v)
        return nullptr;
    auto ms = std::make_shared<MenuSprite>();
    ms->name = Str(v->Get("m_Name"));
    const Value* rect = v->Get("m_Rect");
    ms->rect_w = Flt(rect ? rect->Get("width") : nullptr);
    ms->rect_h = Flt(rect ? rect->Get("height") : nullptr);
    ms->ppu = Flt(v->Get("m_PixelsToUnits"), 100);
    if (const Value* b = v->Get("m_Border")) {
        ms->border[0] = Flt(b->Get("x")), ms->border[1] = Flt(b->Get("y"));
        ms->border[2] = Flt(b->Get("z")), ms->border[3] = Flt(b->Get("w"));
    }
    if (ms->rect_w < 1 || ms->rect_h < 1 || ms->ppu <= 0)
        return nullptr;
    // Render data: the atlas's when the sprite is packed, else its own.
    const Value* data = v->Get("m_RD");
    std::shared_ptr<File> data_file = f;
    std::int64_t fid = 0, pid = 0;
    if (PPtr(v->Get("m_SpriteAtlas"), fid, pid)) {
        const auto ref = assets_.Resolve(f, v->Get("m_SpriteAtlas"), kArtPrefixes);
        if (ref && ref.obj->class_id == kClassSpriteAtlas) {
            static thread_local std::map<std::pair<std::string, std::int64_t>, std::shared_ptr<Value>> atlases;
            auto& atlas = atlases[std::make_pair(ref.file->cab, ref.obj->path_id)];
            if (!atlas)
                if (auto av = ref.file->sf->ReadObject(*ref.obj))
                    atlas = std::make_shared<Value>(std::move(*av));
            const Value* map = atlas ? atlas->Get("m_RenderDataMap") : nullptr;
            const Value* want = v->Get("m_RenderDataKey");
            if (map && want) {
                for (const auto& e : map->list) {
                    if (e.list.size() == 2 && Same(&e.list[0], want)) {
                        data = &e.list[1];
                        data_file = ref.file;
                        break;
                    }
                }
            }
        }
    }
    if (!data)
        return nullptr;
    const auto tref = assets_.Resolve(data_file, data->Get("texture"), kArtPrefixes);
    if (!tref || tref.obj->class_id != kClassTexture2D)
        return nullptr;
    const auto tex = Texture(tref.file, *tref.obj);
    if (!tex || tex->Empty())
        return nullptr;
    const Value* tr = data->Get("textureRect");
    const Value* off = data->Get("textureRectOffset");
    const int x0 = std::clamp(static_cast<int>(std::lround(Flt(tr ? tr->Get("x") : nullptr))), 0, static_cast<int>(tex->w) - 1);
    const int y0 = std::clamp(static_cast<int>(std::lround(Flt(tr ? tr->Get("y") : nullptr))), 0, static_cast<int>(tex->h) - 1);
    const int tw = std::clamp(static_cast<int>(std::lround(Flt(tr ? tr->Get("width") : nullptr))), 1, static_cast<int>(tex->w) - x0);
    const int th = std::clamp(static_cast<int>(std::lround(Flt(tr ? tr->Get("height") : nullptr))), 1, static_cast<int>(tex->h) - y0);
    const int ox = static_cast<int>(std::lround(Flt(off ? off->Get("x") : nullptr)));
    const int oy = static_cast<int>(std::lround(Flt(off ? off->Get("y") : nullptr)));
    // The full sprite rect, the packed texels at their offset (atlases may trim borders).
    const int W = std::max(static_cast<int>(std::lround(ms->rect_w)), 1), H = std::max(static_cast<int>(std::lround(ms->rect_h)), 1);
    std::vector<std::uint8_t> px(static_cast<std::size_t>(W) * H * 4, 0);
    for (int y = 0; y < th; ++y) {
        const int dy = y + oy;
        if (dy < 0 || dy >= H)
            continue;
        for (int x = 0; x < tw; ++x) {
            const int dx = x + ox;
            if (dx < 0 || dx >= W)
                continue;
            std::memcpy(&px[(static_cast<std::size_t>(dy) * W + dx) * 4],
                        &tex->rgba[(static_cast<std::size_t>(y0 + y) * tex->w + x0 + x) * 4], 4);
        }
    }
    int w = W, h = H;
    float scale = 1;
    int halved = 0;
    while ((std::max(w, h) > max_texels || (halved < reduce_ && std::max(w, h) > 128)) && w >= 2 && h >= 2) {
        ++halved;
        const int nw = w / 2, nh = h / 2;
        std::vector<std::uint8_t> half(static_cast<std::size_t>(nw) * nh * 4);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                float acc[4]{};
                for (int k = 0; k < 4; ++k) {
                    const std::uint8_t* p = &px[(static_cast<std::size_t>(2 * y + (k >> 1)) * w + 2 * x + (k & 1)) * 4];
                    const float a = p[3];
                    acc[0] += p[0] * a, acc[1] += p[1] * a, acc[2] += p[2] * a, acc[3] += a;
                }
                std::uint8_t* q = &half[(static_cast<std::size_t>(y) * nw + x) * 4];
                for (int c = 0; c < 3; ++c)
                    q[c] = static_cast<std::uint8_t>(acc[3] > 0 ? std::lround(acc[c] / acc[3]) : 0);
                q[3] = static_cast<std::uint8_t>(std::lround(acc[3] / 4));
            }
        }
        px.swap(half);
        w = nw, h = nh;
        scale *= 0.5f;
    }
    ms->s.tex.w = static_cast<std::uint32_t>(w);
    ms->s.tex.h = static_cast<std::uint32_t>(h);
    ms->s.tex.slices = 1;
    ms->s.tex.srgb = tex->srgb;
    ms->s.tex.rgba = std::move(px);
    ms->texel_scale = scale;
    SpriteP out = ms;
    sprites_[key] = out;
    return out;
}

SpriteP SpriteBank::Resolve(const std::shared_ptr<File>& from, const Value* pptr, int max_texels) {
    const auto ref = assets_.Resolve(from, pptr, kArtPrefixes);
    if (!ref || ref.obj->class_id != kClassSprite)
        return nullptr;
    return Load(ref.file, *ref.obj, max_texels);
}

SpriteP SpriteBank::ByName(const std::string& name, const std::vector<std::string>& prefixes, int max_texels) {
    for (const auto& prefix : prefixes) {
        if (std::find(indexed_.begin(), indexed_.end(), prefix) != indexed_.end())
            continue;
        indexed_.push_back(prefix);
        for (const auto& b : assets_.FindAll(prefix)) {
            const auto f = assets_.Open(b);
            if (!f || !f->sf)
                continue;
            for (const auto& o : f->sf->Objects())
                if (o.class_id == kClassSprite)
                    names_.emplace(f->sf->PeekName(o), std::make_pair(f, o.path_id));
        }
    }
    const auto it = names_.find(name);
    if (it == names_.end())
        return nullptr;
    const ObjectInfo* o = it->second.first->sf->Find(it->second.second);
    return o ? Load(it->second.first, *o, max_texels) : nullptr;
}

// ---------------------------------------------------------------------------------------------
// Scene

int Scene::Find(const std::string& name, int under) const {
    if (under < 0)
        under = root;
    if (under < 0 || under >= static_cast<int>(nodes.size()))
        return -1;
    std::vector<int> stack{under};
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        if (nodes[static_cast<std::size_t>(i)].name == name)
            return i;
        const auto& ch = nodes[static_cast<std::size_t>(i)].children;
        for (auto it = ch.rbegin(); it != ch.rend(); ++it)
            stack.push_back(*it);
    }
    return -1;
}

int Scene::Clone(int node, int parent) {
    if (node < 0 || parent < 0 || node >= static_cast<int>(nodes.size()) || parent >= static_cast<int>(nodes.size()))
        return -1;
    const int at = static_cast<int>(nodes.size());
    Node copy = nodes[static_cast<std::size_t>(node)];
    const std::vector<int> kids = copy.children;
    copy.children.clear();
    copy.parent = parent;
    nodes.push_back(std::move(copy));
    nodes[static_cast<std::size_t>(parent)].children.push_back(at);
    for (const int k : kids)
        Clone(k, at);
    return at;
}

int Scene::Path(const std::string& path, int under) const {
    int at = under;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto slash = path.find('/', start);
        const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        at = Find(part, at);
        if (at < 0 || slash == std::string::npos)
            return at;
        start = slash + 1;
    }
    return at;
}

int Scene::NodeOf(const Value* pptr) const {
    std::int64_t fid = 0, pid = 0;
    if (!PPtr(pptr, fid, pid) || fid != 0)
        return -1;
    const auto it = node_by_obj.find(pid);
    return it == node_by_obj.end() ? -1 : it->second;
}

const Comp* Scene::CompOf(const Value* pptr) const {
    std::int64_t fid = 0, pid = 0;
    if (!PPtr(pptr, fid, pid) || fid != 0)
        return nullptr;
    const auto it = comp_by_pid.find(pid);
    return it == comp_by_pid.end() ? nullptr : &comps[static_cast<std::size_t>(it->second)];
}

const Comp* Scene::CompOn(int node, const std::string& script) const {
    for (const auto& c : comps)
        if (c.node == node && c.script == script)
            return &c;
    return nullptr;
}

Image* Scene::ImageOf(const Value* pptr) {
    std::int64_t fid = 0, pid = 0;
    const int n = NodeOf(pptr);
    if (n < 0 || !PPtr(pptr, fid, pid))
        return nullptr;
    for (auto& im : nodes[static_cast<std::size_t>(n)].images)
        if (im.pid == pid)
            return &im;
    return nullptr;
}

Text* Scene::TextOf(const Value* pptr) {
    std::int64_t fid = 0, pid = 0;
    const int n = NodeOf(pptr);
    if (n < 0 || !PPtr(pptr, fid, pid))
        return nullptr;
    auto& texts = nodes[static_cast<std::size_t>(n)].texts;
    for (auto& t : texts)
        if (t.pid == pid)
            return &t;
    // The game's UILocalizedString (its text is set through it): the node's text.
    return texts.empty() ? nullptr : &texts.front();
}

void Scene::RunEvent(const Value* ev) {
    const Value* pc = ev ? ev->Get("m_PersistentCalls") : nullptr;
    const Value* calls = pc ? pc->Get("m_Calls") : nullptr;
    if (!calls)
        return;
    for (const auto& c : calls->list) {
        const int n = NodeOf(c.Get("m_Target"));
        if (n < 0 || Int(c.Get("m_CallState"), 2) == 0)
            continue;
        const std::string& m = Str(c.Get("m_MethodName"));
        const std::string& type = Str(c.Get("m_TargetAssemblyTypeName"));
        Node& node = nodes[static_cast<std::size_t>(n)];
        if (m == "SetActive" && type.rfind("UnityEngine.GameObject", 0) == 0) {
            const Value* a = c.Get("m_Arguments");
            node.active = Int(a ? a->Get("m_BoolArgument") : nullptr) != 0;
        } else if ((type.find("UIContainer") != std::string::npos || type.find("UIView") != std::string::npos) &&
                   (m == "Show" || m == "InstantShow")) {
            node.active = true;
            node.alpha = 1;
        } else if ((type.find("UIContainer") != std::string::npos || type.find("UIView") != std::string::npos) &&
                   (m == "Hide" || m == "InstantHide")) {
            node.active = false;
        }
    }
}

namespace {

class SceneLoader {
public:
    SceneLoader(Assets& a, SpriteBank& sprites, const bc_levelmap::Strings& strings, std::shared_ptr<File> f,
                std::vector<std::string> skip)
        : assets_(a), sprites_(sprites), strings_(strings), f_(std::move(f)), skip_(std::move(skip)) {
        for (const auto& n : assets_.FindAll(""))
            if (n.find("_monoscripts") != std::string::npos)
                script_prefixes_.push_back(n);
    }

    int Build(std::int64_t go_pid, int parent, Scene& scene) {
        const ObjectInfo* go = f_->sf->Find(go_pid);
        if (!go || go->class_id != kClassGameObject)
            return -1;
        const auto gv = f_->sf->ReadObject(*go);
        if (!gv)
            return -1;
        const std::string name = Str(gv->Get("m_Name"));
        if (parent >= 0 && std::find(skip_.begin(), skip_.end(), name) != skip_.end())
            return -1;
        const int index = static_cast<int>(scene.nodes.size());
        scene.nodes.emplace_back();
        scene.node_by_obj[go_pid] = index;
        {
            Node& n = scene.nodes.back();
            n.name = name;
            n.parent = parent;
            n.active = Int(gv->Get("m_IsActive"), 1) != 0;
        }
        std::vector<std::int64_t> kids;
        const Value* comps = gv->Get("m_Component");
        if (comps) {
            for (const auto& c : comps->list) {
                std::int64_t fid = 0, pid = 0;
                const Value* ref = c.Get("component");
                if (!PPtr(ref ? ref : &c, fid, pid) || fid != 0)
                    continue;
                const ObjectInfo* co = f_->sf->Find(pid);
                if (!co)
                    continue;
                scene.node_by_obj[pid] = index;
                Component(*co, scene, index, kids);
            }
        }
        for (const auto k : kids) {
            const int ci = Build(k, index, scene);
            if (ci >= 0)
                scene.nodes[static_cast<std::size_t>(index)].children.push_back(ci);
        }
        return index;
    }

private:
    /// The MonoBehaviour's class, from its type (one read per type).
    std::string ScriptOf(const ObjectInfo& co) {
        const auto* t = f_->sf->TypeOf(co);
        if (const auto it = type_scripts_.find(t); it != type_scripts_.end())
            return it->second;
        std::string name;
        if (const auto v = f_->sf->ReadObject(co))
            name = ScriptName(*v);
        type_scripts_[t] = name;
        return name;
    }

    std::string ScriptName(const Value& mb) {
        std::int64_t fid = 0, pid = 0;
        if (!PPtr(mb.Get("m_Script"), fid, pid))
            return {};
        const auto key = std::make_pair(fid, pid);
        if (const auto it = scripts_.find(key); it != scripts_.end())
            return it->second;
        std::string name;
        const auto ref = assets_.Resolve(f_, mb.Get("m_Script"), script_prefixes_);
        if (ref) {
            if (const auto sv = ref.file->sf->ReadObject(*ref.obj))
                name = Str(sv->Get("m_ClassName"));
        }
        scripts_[key] = name;
        return name;
    }

    void Component(const ObjectInfo& co, Scene& scene, int index, std::vector<std::int64_t>& kids) {
        auto& sf = *f_->sf;
        if (co.class_id == kClassRectTransform || co.class_id == kClassTransform) {
            const auto v = sf.ReadObject(co);
            if (!v)
                return;
            Node& n = scene.nodes[static_cast<std::size_t>(index)];
            if (const Value* ch = v->Get("m_Children")) {
                for (const auto& c : ch->list) {
                    std::int64_t fid = 0, pid = 0;
                    if (!PPtr(&c, fid, pid) || fid != 0)
                        continue;
                    const ObjectInfo* t = sf.Find(pid);
                    if (!t)
                        continue;
                    const auto tv = sf.ReadObject(*t);
                    std::int64_t gf = 0, gp = 0;
                    if (tv && PPtr(tv->Get("m_GameObject"), gf, gp) && gf == 0)
                        kids.push_back(gp);
                }
            }
            if (const Value* s = v->Get("m_LocalScale"))
                Vec2(s, n.scale, 1);
            if (co.class_id == kClassRectTransform) {
                n.is_rect = true;
                Vec2(v->Get("m_AnchorMin"), n.amin);
                Vec2(v->Get("m_AnchorMax"), n.amax);
                if (!pos_set_.count(index))
                    Vec2(v->Get("m_AnchoredPosition"), n.pos);
                Vec2(v->Get("m_SizeDelta"), n.size);
                Vec2(v->Get("m_Pivot"), n.pivot, 0.5f);
            }
            return;
        }
        if (co.class_id == kClassCanvasGroup) {
            const auto v = sf.ReadObject(co);
            if (v && Int(v->Get("m_Enabled"), 1))
                scene.nodes[static_cast<std::size_t>(index)].alpha = Flt(v->Get("m_Alpha"), 1);
            return;
        }
        if (co.class_id != kClassMonoBehaviour)
            return;
        const std::string sc = ScriptOf(co);
        static const std::set<std::string> kLayout = {
            "Image", "TextMeshProUGUI", "TextMeshProUGUIForCurvedTexts", "LayoutElement", "HorizontalLayoutGroup",
            "VerticalLayoutGroup", "GridLayoutGroup", "ContentSizeFitter", "AspectRatioFitter", "Mask", "RectMask2D",
            "UILocalizedString", "UIView", "UIContainer"};
        static const std::set<std::string> kKept = {
            "BooleanUIView", "InitAndUpdateBooleanUIView", "InventoryItemDisplayBase", "InventoryItemDisplayWeaponCard",
            "ArcanaDisplay", "InventoryCharmChecker", "InventoryCharmSlotController", "InventoryWeaponSlotController",
            "InventoryWeaponChecker", "InventorySpellSlotController", "InventorySpellChecker", "FragmentedCollectibleDisplay",
            "FragmentedCollectibleCountDisplay", "EquipmentViewEquippedItemController", "IntStatValueDisplay",
            "ExperienceBarDisplay", "InventoryWeaponArchetypeList", "InventoryCharmPage", "InventorySpellDisplay",
            "InventoryItemDisplayNotions", "UIContainer", "UIView", "BasicTab", "UIToggle", "PagesIndexDots",
            "InventoryItemDisplayCollection", "UISelectableColorAnimator", "UISelectableUIAnimator", "NotionDisplay"};
        const bool layout = kLayout.count(sc) != 0, kept = kKept.count(sc) != 0;
        if (!layout && !kept)
            return;
        const auto v = sf.ReadObject(co);
        if (!v)
            return;
        if (kept) {
            Comp c;
            c.script = sc;
            c.node = index;
            c.pid = co.path_id;
            c.v = std::make_shared<Value>(*v);
            // Item data a slot shows (another bundle): its itemID.
            for (const char* fld : {"charmItemData", "spellItemData"}) {
                std::int64_t fid = 0, pid = 0;
                if (PPtr(v->Get(fld), fid, pid)) {
                    const auto ref = assets_.Resolve(f_, v->Get(fld), {"charms-", "arcana-", "item-", "weapon-"});
                    if (ref)
                        if (const auto iv = ref.file->sf->ReadObject(*ref.obj))
                            c.item_id = Str(iv->Get("itemID"));
                }
            }
            if (const Value* id = v->Get("itemID"))
                c.item_id = Str(id);
            scene.comp_by_pid[co.path_id] = static_cast<int>(scene.comps.size());
            scene.comps.push_back(std::move(c));
        }
        if (!layout)
            return;
        Node& n = scene.nodes[static_cast<std::size_t>(index)];
        const bool enabled = Int(v->Get("m_Enabled"), 1) != 0;
        if (sc == "UIView" || sc == "UIContainer") {
            // Doozy: shown at its custom start position.
            if (Int(v->Get("UseCustomStartPosition"))) {
                float p[2];
                Vec2(v->Get("CustomStartPosition"), p);
                n.pos[0] = p[0], n.pos[1] = p[1];
                pos_set_.insert(index);
            }
            return;
        }
        if (sc == "UILocalizedString") {
            const Value* sr = v->Get("m_StringReference");
            const Value* e = sr ? sr->Get("m_TableEntryReference") : nullptr;
            const std::int64_t key = Int(e ? e->Get("m_KeyId") : nullptr);
            if (key != 0)
                loc_[index] = key;
            return;
        }
        if (!enabled)
            return;
        if (sc == "Image") {
            Image im;
            im.pid = co.path_id;
            im.color = Color(v->Get("m_Color"));
            im.type = static_cast<int>(Int(v->Get("m_Type")));
            im.preserve_aspect = Int(v->Get("m_PreserveAspect")) != 0;
            im.fill_center = Int(v->Get("m_FillCenter"), 1) != 0;
            im.fill_amount = Flt(v->Get("m_FillAmount"), 1);
            im.fill_method = static_cast<int>(Int(v->Get("m_FillMethod")));
            im.fill_origin = static_cast<int>(Int(v->Get("m_FillOrigin")));
            im.ppu_multiplier = Flt(v->Get("m_PixelsPerUnitMultiplier"), 1);
            if (im.ppu_multiplier <= 0)
                im.ppu_multiplier = 1;
            std::int64_t fid = 0, pid = 0;
            if (PPtr(v->Get("m_Sprite"), fid, pid))
                im.sprite = sprites_.Resolve(f_, v->Get("m_Sprite"));
            // A white placeholder with no sprite is filled in by the game (item art): not drawn.
            im.runtime = !im.sprite && im.color.r == 1 && im.color.g == 1 && im.color.b == 1 && im.color.a == 1;
            n.images.push_back(std::move(im));
            return;
        }
        if (sc.rfind("TextMeshProUGUI", 0) == 0) {
            Text t;
            t.pid = co.path_id;
            t.text = Str(v->Get("m_text"));
            t.size = Flt(v->Get("m_fontSize"), 36);
            t.size_min = Flt(v->Get("m_fontSizeMin"), 18);
            t.size_max = Flt(v->Get("m_fontSizeMax"), 72);
            t.autosize = Int(v->Get("m_enableAutoSizing")) != 0;
            t.style = static_cast<int>(Int(v->Get("m_fontStyle")));
            t.halign = static_cast<int>(Int(v->Get("m_HorizontalAlignment"), 2));
            t.valign = static_cast<int>(Int(v->Get("m_VerticalAlignment"), 512));
            t.color = Color(v->Get("m_fontColor"));
            t.wrap = Int(v->Get("m_enableWordWrapping"), 1) != 0;
            if (const Value* m = v->Get("m_margin")) {
                t.margin[0] = Flt(m->Get("x")), t.margin[1] = Flt(m->Get("y"));
                t.margin[2] = Flt(m->Get("z")), t.margin[3] = Flt(m->Get("w"));
            }
            n.texts.push_back(std::move(t));
            return;
        }
        if (sc == "LayoutElement") {
            LayoutElement le;
            le.ignore = Int(v->Get("m_IgnoreLayout")) != 0;
            le.min[0] = Flt(v->Get("m_MinWidth"), -1), le.min[1] = Flt(v->Get("m_MinHeight"), -1);
            le.preferred[0] = Flt(v->Get("m_PreferredWidth"), -1);
            le.preferred[1] = Flt(v->Get("m_PreferredHeight"), -1);
            le.flexible[0] = Flt(v->Get("m_FlexibleWidth"), -1);
            le.flexible[1] = Flt(v->Get("m_FlexibleHeight"), -1);
            le.priority = static_cast<int>(Int(v->Get("m_LayoutPriority"), 1));
            n.le = le;
            return;
        }
        if (sc == "HorizontalLayoutGroup" || sc == "VerticalLayoutGroup" || sc == "GridLayoutGroup") {
            Group g;
            if (const Value* p = v->Get("m_Padding")) {
                g.pad_l = Flt(p->Get("m_Left")), g.pad_r = Flt(p->Get("m_Right"));
                g.pad_t = Flt(p->Get("m_Top")), g.pad_b = Flt(p->Get("m_Bottom"));
            }
            g.align = static_cast<int>(Int(v->Get("m_ChildAlignment")));
            if (sc == "GridLayoutGroup") {
                g.kind = Group::Grid;
                Vec2(v->Get("m_CellSize"), g.cell);
                Vec2(v->Get("m_Spacing"), g.spacing_xy);
                g.start_corner = static_cast<int>(Int(v->Get("m_StartCorner")));
                g.start_axis = static_cast<int>(Int(v->Get("m_StartAxis")));
                g.constraint = static_cast<int>(Int(v->Get("m_Constraint")));
                g.constraint_count = std::max<int>(1, static_cast<int>(Int(v->Get("m_ConstraintCount"), 1)));
            } else {
                g.kind = sc == "VerticalLayoutGroup" ? Group::Vertical : Group::Horizontal;
                g.spacing = Flt(v->Get("m_Spacing"));
                g.control[0] = Int(v->Get("m_ChildControlWidth")) != 0;
                g.control[1] = Int(v->Get("m_ChildControlHeight")) != 0;
                g.expand[0] = Int(v->Get("m_ChildForceExpandWidth"), 1) != 0;
                g.expand[1] = Int(v->Get("m_ChildForceExpandHeight"), 1) != 0;
                g.use_scale[0] = Int(v->Get("m_ChildScaleWidth")) != 0;
                g.use_scale[1] = Int(v->Get("m_ChildScaleHeight")) != 0;
            }
            n.group = g;
            return;
        }
        if (sc == "ContentSizeFitter") {
            n.has_fitter = true;
            n.fit[0] = static_cast<int>(Int(v->Get("m_HorizontalFit")));
            n.fit[1] = static_cast<int>(Int(v->Get("m_VerticalFit")));
            return;
        }
        if (sc == "AspectRatioFitter") {
            n.aspect_mode = static_cast<int>(Int(v->Get("m_AspectMode")));
            n.aspect_ratio = Flt(v->Get("m_AspectRatio"), 1);
            return;
        }
        if (sc == "Mask") {
            n.mask = 1;
            n.mask_show = Int(v->Get("m_ShowMaskGraphic"), 1) != 0;
            return;
        }
        if (sc == "RectMask2D") {
            n.mask = 2;
            return;
        }
    }

public:
    void Localize(Scene& scene) {
        for (const auto& [index, key] : loc_) {
            Node& n = scene.nodes[static_cast<std::size_t>(index)];
            const std::string s = strings_.Get(key);
            if (!s.empty())
                for (auto& t : n.texts)
                    t.text = s;
        }
    }

private:
    Assets& assets_;
    SpriteBank& sprites_;
    const bc_levelmap::Strings& strings_;
    std::shared_ptr<File> f_;
    std::vector<std::string> skip_;
    std::vector<std::string> script_prefixes_;
    std::map<std::pair<std::int64_t, std::int64_t>, std::string> scripts_;
    std::map<const bc_unity::SerializedType*, std::string> type_scripts_;
    std::map<int, std::int64_t> loc_;
    std::set<int> pos_set_;
};

} // namespace

std::shared_ptr<Scene> LoadScene(Assets& assets, SpriteBank& sprites, const bc_levelmap::Strings& strings,
                                 const std::string& root_name, const std::vector<std::string>& skip, std::string* log) {
    const auto f = assets.Open(assets.Find("afm_scenes_scenes_gamemenu"));
    if (!f || !f->sf) {
        if (log)
            *log += "game menu scene missing\n";
        return nullptr;
    }
    std::int64_t root = 0;
    for (const auto& o : f->sf->Objects()) {
        if (o.class_id == kClassGameObject && f->sf->PeekName(o) == root_name) {
            root = o.path_id;
            break;
        }
    }
    if (!root) {
        if (log)
            *log += root_name + " not found\n";
        return nullptr;
    }
    auto scene = std::make_shared<Scene>();
    SceneLoader loader(assets, sprites, strings, f, skip);
    scene->root = loader.Build(root, -1, *scene);
    loader.Localize(*scene);
    sprites.DropTextures();
    if (scene->root < 0)
        return nullptr;
    if (log)
        *log += std::to_string(scene->nodes.size()) + " menu nodes\n";
    return scene;
}

// ---------------------------------------------------------------------------------------------
// Layout (Unity's RectTransform, LayoutGroup, LayoutElement and ContentSizeFitter rules)

void CanvasSize(int screen_w, int screen_h, float match, float& cw, float& ch, float& scale) {
    const float lw = std::log2(static_cast<float>(screen_w) / 3840.0f);
    const float lh = std::log2(static_cast<float>(screen_h) / 2160.0f);
    scale = std::exp2(lw + (lh - lw) * match);
    cw = static_cast<float>(screen_w) / scale;
    ch = static_cast<float>(screen_h) / scale;
}

namespace {

struct LayoutCtx {
    Scene& s;
    const bc_levelmap::Font* font;
};

void GroupInput(LayoutCtx& c, const Node& n, int axis, float& tmin, float& tpref, float& tflex);

void Props(LayoutCtx& c, const Node& n, int axis, float& mn, float& pf, float& fl, float wrap_width = -1) {
    struct Cand {
        int pri;
        float v[3];
    };
    std::vector<Cand> cands;
    for (const auto& im : n.images) {
        if (!im.sprite) {
            cands.push_back({0, {0, 0, -1}});
            continue;
        }
        const auto& sp = *im.sprite;
        const float ppu = sp.ppu / 100.0f * im.ppu_multiplier;
        const bool border = sp.border[0] + sp.border[1] + sp.border[2] + sp.border[3] > 0;
        float pref;
        if ((im.type == 1 || im.type == 2) && border)
            pref = (axis == 0 ? sp.border[0] + sp.border[2] : sp.border[1] + sp.border[3]) / ppu;
        else
            pref = (axis == 0 ? sp.rect_w : sp.rect_h) / ppu;
        cands.push_back({0, {0, pref, -1}});
    }
    for (const auto& t : n.texts) {
        float pw = 0, ph = 0;
        if (c.font)
            TextMetrics(*c.font, t, axis == 1 ? wrap_width : -1, pw, ph);
        cands.push_back({0, {0, axis == 0 ? pw : ph, -1}});
    }
    if (n.group) {
        float a, b, d;
        GroupInput(c, n, axis, a, b, d);
        cands.push_back({0, {a, b, d}});
    }
    if (n.le)
        cands.push_back({n.le->priority, {n.le->min[axis], n.le->preferred[axis], n.le->flexible[axis]}});
    const auto prop = [&](int i, float fb) {
        float val = fb;
        int best = -1000000;
        for (const auto& cd : cands) {
            const float v = cd.v[i];
            if (v < 0)
                continue;
            if (cd.pri > best) {
                val = v, best = cd.pri;
            } else if (cd.pri == best && v > val) {
                val = v;
            }
        }
        return val;
    };
    mn = prop(0, 0);
    pf = std::max(mn, prop(1, 0));
    fl = prop(2, 0);
}

std::vector<int> InLayout(const Scene& s, const Node& n) {
    std::vector<int> out;
    for (const int ci : n.children) {
        const Node& c = s.nodes[static_cast<std::size_t>(ci)];
        if (c.active && c.is_rect && !(c.le && c.le->ignore))
            out.push_back(ci);
    }
    return out;
}

float OwnSize(LayoutCtx& c, const Node& n, int axis) {
    if (n.has_fitter && n.fit[axis]) {
        float mn, pf, fl;
        Props(c, n, axis, mn, pf, fl, axis == 1 ? n.size[0] : -1);
        return n.fit[axis] == 1 ? mn : pf;
    }
    return n.size[axis];
}

void ChildSizes(LayoutCtx& c, const Node& ch, int axis, bool control, bool expand, float& mn, float& pf, float& fl,
                float width = -1) {
    if (!control) {
        mn = OwnSize(c, ch, axis);
        pf = mn;
        fl = 0;
    } else {
        Props(c, ch, axis, mn, pf, fl, width);
    }
    if (expand)
        fl = std::max(fl, 1.0f);
}

void GroupInput(LayoutCtx& c, const Node& n, int axis, float& tmin, float& tpref, float& tflex) {
    const Group& g = *n.group;
    const auto kids = InLayout(c.s, n);
    const float padh = g.pad_l + g.pad_r, padv = g.pad_t + g.pad_b;
    if (g.kind == Group::Grid) {
        const int cnt = static_cast<int>(kids.size());
        if (axis == 0) {
            int mc, pc;
            if (g.constraint == 1) {
                mc = pc = g.constraint_count;
            } else if (g.constraint == 2) {
                mc = pc = static_cast<int>(std::ceil(static_cast<float>(cnt) / static_cast<float>(g.constraint_count) - 0.001f));
            } else {
                mc = 1;
                pc = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(cnt))));
            }
            tmin = padh + (g.cell[0] + g.spacing_xy[0]) * static_cast<float>(mc) - g.spacing_xy[0];
            tpref = padh + (g.cell[0] + g.spacing_xy[0]) * static_cast<float>(pc) - g.spacing_xy[0];
            tflex = 0;
        } else {
            int rows;
            if (g.constraint == 1) {
                rows = static_cast<int>(std::ceil(static_cast<float>(cnt) / static_cast<float>(g.constraint_count) - 0.001f));
            } else if (g.constraint == 2) {
                rows = g.constraint_count;
            } else {
                const int cols = std::max(1, static_cast<int>((n.rect.w - padh + g.spacing_xy[0] + 0.001f) /
                                                              (g.cell[0] + g.spacing_xy[0])));
                rows = static_cast<int>(std::ceil(static_cast<float>(cnt) / static_cast<float>(cols)));
            }
            tmin = tpref = padv + (g.cell[1] + g.spacing_xy[1]) * static_cast<float>(rows) - g.spacing_xy[1];
            tflex = 0;
        }
        return;
    }
    const bool vertical = g.kind == Group::Vertical;
    const float pad = axis == 0 ? padh : padv;
    tmin = tpref = pad;
    tflex = 0;
    const bool other = vertical != (axis == 1);
    for (const int ci : kids) {
        const Node& ch = c.s.nodes[static_cast<std::size_t>(ci)];
        float mn, pf, fl;
        ChildSizes(c, ch, axis, g.control[axis], g.expand[axis], mn, pf, fl);
        if (g.use_scale[axis]) {
            const float k = ch.scale[axis];
            mn *= k, pf *= k, fl *= k;
        }
        if (other) {
            tmin = std::max(mn + pad, tmin);
            tpref = std::max(pf + pad, tpref);
            tflex = std::max(fl, tflex);
        } else {
            tmin += mn + g.spacing;
            tpref += pf + g.spacing;
            tflex += fl;
        }
    }
    if (!other && !kids.empty()) {
        tmin -= g.spacing;
        tpref -= g.spacing;
    }
    tpref = std::max(tmin, tpref);
}

Rect Resolve(const Node& n, const Rect& p) {
    const float ax0 = p.x + p.w * n.amin[0], ax1 = p.x + p.w * n.amax[0];
    const float ay0 = p.y + p.h * n.amin[1], ay1 = p.y + p.h * n.amax[1];
    const float w = ax1 - ax0 + n.size[0], h = ay1 - ay0 + n.size[1];
    const float px = ax0 + (ax1 - ax0) * n.pivot[0] + n.pos[0];
    const float py = ay0 + (ay1 - ay0) * n.pivot[1] + n.pos[1];
    return {px - w * n.pivot[0], py - h * n.pivot[1], w, h};
}

Rect OwnRect(LayoutCtx& c, Node& n, const Rect& p) {
    if (n.has_fitter) {
        for (int axis = 0; axis < 2; ++axis) {
            if (!n.fit[axis])
                continue;
            float mn, pf, fl;
            Props(c, n, axis, mn, pf, fl, axis == 1 ? n.size[0] : -1);
            const float v = n.fit[axis] == 1 ? mn : pf;
            const float span = axis == 0 ? p.w * (n.amax[0] - n.amin[0]) : p.h * (n.amax[1] - n.amin[1]);
            n.size[axis] = v - span;
        }
    }
    Rect r = Resolve(n, p);
    if (n.aspect_mode == 1 && n.aspect_ratio > 0) {
        n.size[1] = r.w / n.aspect_ratio - p.h * (n.amax[1] - n.amin[1]);
        r = Resolve(n, p);
    } else if (n.aspect_mode == 2) {
        n.size[0] = r.h * n.aspect_ratio - p.w * (n.amax[0] - n.amin[0]);
        r = Resolve(n, p);
    } else if ((n.aspect_mode == 3 || n.aspect_mode == 4) && n.aspect_ratio > 0 && p.h > 0) {
        float w, h;
        if ((p.w / p.h > n.aspect_ratio) == (n.aspect_mode == 3)) {
            h = p.h;
            w = h * n.aspect_ratio;
        } else {
            w = p.w;
            h = w / n.aspect_ratio;
        }
        n.amin[0] = n.amin[1] = 0;
        n.amax[0] = n.amax[1] = 1;
        n.pos[0] = n.pos[1] = 0;
        n.size[0] = w - p.w;
        n.size[1] = h - p.h;
        r = Resolve(n, p);
    }
    return r;
}

void SetChild(Scene& s, const Node& n, int ci, int axis, float pos, float size) {
    Node& c = s.nodes[static_cast<std::size_t>(ci)];
    c.driven = true;
    if (axis == 0) {
        c.rect.x = n.rect.x + pos;
        c.rect.w = size;
    } else {
        c.rect.h = size;
        c.rect.y = n.rect.y + n.rect.h - pos - size;
    }
}

void PlaceGrid(Scene& s, Node& n, const std::vector<int>& kids) {
    const Group& g = *n.group;
    const float w = n.rect.w, h = n.rect.h;
    const int cnt = static_cast<int>(kids.size());
    int cols, rows;
    if (g.constraint == 1) {
        cols = g.constraint_count;
        rows = static_cast<int>(std::ceil(static_cast<float>(cnt) / static_cast<float>(g.constraint_count) - 0.001f));
    } else if (g.constraint == 2) {
        rows = g.constraint_count;
        cols = static_cast<int>(std::ceil(static_cast<float>(cnt) / static_cast<float>(g.constraint_count) - 0.001f));
    } else {
        cols = std::max(1, static_cast<int>((w - g.pad_l - g.pad_r + g.spacing_xy[0] + 0.001f) / (g.cell[0] + g.spacing_xy[0])));
        rows = std::max(1, static_cast<int>((h - g.pad_t - g.pad_b + g.spacing_xy[1] + 0.001f) / (g.cell[1] + g.spacing_xy[1])));
    }
    cols = std::max(cols, 1);
    rows = std::max(rows, 1);
    int cpa, acols, arows;
    if (g.start_axis == 0) {
        cpa = cols;
        acols = std::clamp(std::min(cols, cnt), 1, cols);
        arows = std::clamp(std::min(rows, static_cast<int>(std::ceil(static_cast<float>(cnt) / static_cast<float>(cpa)))), 1, rows);
    } else {
        cpa = rows;
        arows = std::clamp(std::min(rows, cnt), 1, rows);
        acols = std::clamp(std::min(cols, static_cast<int>(std::ceil(static_cast<float>(cnt) / static_cast<float>(cpa)))), 1, cols);
    }
    const float req[2] = {acols * g.cell[0] + (acols - 1) * g.spacing_xy[0], arows * g.cell[1] + (arows - 1) * g.spacing_xy[1]};
    const float start[2] = {g.pad_l + (w - req[0] - g.pad_l - g.pad_r) * static_cast<float>(g.align % 3) * 0.5f,
                            g.pad_t + (h - req[1] - g.pad_t - g.pad_b) * static_cast<float>(g.align / 3) * 0.5f};
    for (int i = 0; i < cnt; ++i) {
        int px, py;
        if (g.start_axis == 0)
            px = i % cpa, py = i / cpa;
        else
            px = i / cpa, py = i % cpa;
        if (g.start_corner % 2 == 1)
            px = acols - 1 - px;
        if (g.start_corner / 2 == 1)
            py = arows - 1 - py;
        SetChild(s, n, kids[static_cast<std::size_t>(i)], 0, start[0] + (g.cell[0] + g.spacing_xy[0]) * static_cast<float>(px), g.cell[0]);
        SetChild(s, n, kids[static_cast<std::size_t>(i)], 1, start[1] + (g.cell[1] + g.spacing_xy[1]) * static_cast<float>(py), g.cell[1]);
    }
}

void Place(LayoutCtx& c, Node& n) {
    const Group& g = *n.group;
    const auto kids = InLayout(c.s, n);
    if (g.kind == Group::Grid) {
        PlaceGrid(c.s, n, kids);
        return;
    }
    const bool vertical = g.kind == Group::Vertical;
    for (int axis = 0; axis < 2; ++axis) {
        const float size = axis == 0 ? n.rect.w : n.rect.h;
        const bool control = g.control[axis], expand = g.expand[axis];
        const float align = axis == 0 ? static_cast<float>(g.align % 3) * 0.5f : static_cast<float>(g.align / 3) * 0.5f;
        const float pad_start = axis == 0 ? g.pad_l : g.pad_t;
        const float padt = axis == 0 ? g.pad_l + g.pad_r : g.pad_t + g.pad_b;
        const bool other = vertical != (axis == 1);
        float tmin, tpref, tflex;
        GroupInput(c, n, axis, tmin, tpref, tflex);
        const auto start_offset = [&](float required) { return pad_start + (size - (required + padt)) * align; };
        if (other) {
            const float inner = size - padt;
            for (const int ci : kids) {
                const Node& ch = c.s.nodes[static_cast<std::size_t>(ci)];
                float mn, pf, fl;
                ChildSizes(c, ch, axis, control, expand, mn, pf, fl, axis == 1 ? ch.rect.w : -1);
                const float req = std::max(mn, std::min(inner, fl > 0 ? size : pf));
                const float st = start_offset(req);
                if (control) {
                    SetChild(c.s, n, ci, axis, st, req);
                } else {
                    const float own = OwnSize(c, ch, axis);
                    SetChild(c.s, n, ci, axis, st + (req - own) * align, own);
                }
            }
        } else {
            float pos = pad_start, mult = 0;
            const float surplus = size - tpref;
            if (surplus > 0) {
                if (tflex == 0)
                    pos = start_offset(tpref - padt);
                else
                    mult = surplus / tflex;
            }
            float lerp = 0;
            if (tmin != tpref)
                lerp = std::clamp((size - tmin) / (tpref - tmin), 0.0f, 1.0f);
            for (const int ci : kids) {
                const Node& ch = c.s.nodes[static_cast<std::size_t>(ci)];
                float mn, pf, fl;
                ChildSizes(c, ch, axis, control, expand, mn, pf, fl, axis == 1 ? ch.rect.w : -1);
                const float cs = mn + (pf - mn) * lerp + fl * mult;
                if (control) {
                    SetChild(c.s, n, ci, axis, pos, cs);
                } else {
                    const float own = OwnSize(c, ch, axis);
                    SetChild(c.s, n, ci, axis, pos + (cs - own) * align, own);
                }
                pos += cs + g.spacing;
            }
        }
    }
}

void Apply(LayoutCtx& c, int index, const Rect& r) {
    Node& n = c.s.nodes[static_cast<std::size_t>(index)];
    n.rect = r;
    for (const int ci : n.children)
        c.s.nodes[static_cast<std::size_t>(ci)].driven = false;
    if (n.group && n.active)
        Place(c, n);
    for (const int ci : n.children) {
        Node& ch = c.s.nodes[static_cast<std::size_t>(ci)];
        Rect cr = ch.rect;
        if (!ch.driven)
            cr = ch.is_rect ? OwnRect(c, ch, n.rect) : n.rect;
        Apply(c, ci, cr);
    }
}

} // namespace

void Layout(Scene& scene, const bc_levelmap::Font& font, float cw, float ch) {
    if (scene.root < 0)
        return;
    LayoutCtx c{scene, &font};
    Node& root = scene.nodes[static_cast<std::size_t>(scene.root)];
    root.pos[0] = root.pos[1] = 0;
    Apply(c, scene.root, Rect{0, 0, cw, ch});
}

// ---------------------------------------------------------------------------------------------
// Drawing

void PixelRect(const Rect& r, float scale, float ch, float& x, float& y, float& w, float& h) {
    x = r.x * scale;
    y = (ch - (r.y + r.h)) * scale;
    w = r.w * scale;
    h = r.h * scale;
}

namespace {

const bc_levelmap::Sprite& White() {
    static const bc_levelmap::Sprite s = [] {
        bc_levelmap::Sprite w;
        w.tex.w = w.tex.h = 1;
        w.tex.slices = 1;
        w.tex.rgba = {255, 255, 255, 255};
        return w;
    }();
    return s;
}

struct Painter {
    const Scene& s;
    const bc_levelmap::Font& font;
    float k;   // pixels per unit
    float ch;  // canvas height, units
    bc_ui::Canvas& out;
    const std::vector<bool>* hide;
    std::deque<bc_ui::Canvas::ClipMask> clips; // owned clip masks, innermost last
    float ox = 0, oy = 0;                  // pixel origin of `out` in the page

    void Px(const Rect& r, float& x, float& y, float& w, float& h) const {
        PixelRect(r, k, ch, x, y, w, h);
        x -= ox;
        y -= oy;
    }

    bc_shade::Vec4 Tint(const Rgba& c, float alpha) const { return {c.r, c.g, c.b, c.a * alpha}; }

    void DrawImage(const Node& n, const Image& im, float alpha) {
        if (im.color.a * alpha <= 0.003f)
            return;
        float x, y, w, h;
        Px(n.rect, x, y, w, h);
        if (w <= 0 || h <= 0)
            return;
        const auto tint = Tint(im.color, alpha);
        if (!im.sprite) {
            if (!im.runtime)
                out.Blit(White(), x, y, w, h, tint);
            return;
        }
        const MenuSprite& sp = *im.sprite;
        const float tw = static_cast<float>(sp.s.tex.w), th = static_cast<float>(sp.s.tex.h);
        const bool border = sp.border[0] + sp.border[1] + sp.border[2] + sp.border[3] > 0;
        if ((im.type == 1 || im.type == 2) && border) {
            // Sprite pixels -> canvas units: 100 / ppu / multiplier; then -> output pixels.
            float px_per_texel = (100.0f / sp.ppu) / im.ppu_multiplier * k / sp.texel_scale;
            const float l = sp.border[0] * sp.texel_scale, b = sp.border[1] * sp.texel_scale;
            const float r = sp.border[2] * sp.texel_scale, t = sp.border[3] * sp.texel_scale;
            const float fx = (l + r) * px_per_texel > w ? w / ((l + r) * px_per_texel) : 1.0f;
            const float fy = (t + b) * px_per_texel > h ? h / ((t + b) * px_per_texel) : 1.0f;
            px_per_texel *= std::min(fx, fy);
            out.Nine(sp.s, l, t, r, b, x, y, w, h, px_per_texel, im.fill_center, tint);
            return;
        }
        if (im.type == 3 && im.fill_amount < 1) {
            const float a = std::clamp(im.fill_amount, 0.0f, 1.0f);
            if (a <= 0)
                return;
            if (im.fill_method == 0) { // horizontal
                if (im.fill_origin == 0)
                    out.Blit(sp.s, 0, 0, tw * a, th, x, y, w * a, h, tint);
                else
                    out.Blit(sp.s, tw * (1 - a), 0, tw * a, th, x + w * (1 - a), y, w * a, h, tint);
                return;
            }
            if (im.fill_method == 1) { // vertical (origin 0 = bottom)
                if (im.fill_origin == 0)
                    out.Blit(sp.s, 0, th * (1 - a), tw, th * a, x, y + h * (1 - a), w, h * a, tint);
                else
                    out.Blit(sp.s, 0, 0, tw, th * a, x, y, w, h * a, tint);
                return;
            }
        }
        if (im.preserve_aspect && sp.rect_h > 0 && h > 0) {
            const float ar = sp.rect_w / sp.rect_h;
            float dw = w, dh = h;
            if (w / h > ar)
                dw = h * ar;
            else
                dh = w / ar;
            out.Blit(sp.s, x + (w - dw) / 2, y + (h - dh) / 2, dw, dh, tint);
            return;
        }
        out.Blit(sp.s, x, y, w, h, tint);
    }

    void DrawText(const Node& n, const Text& t, float alpha) {
        if (t.color.a * alpha <= 0.003f || t.text.empty())
            return;
        const Rect& r = n.rect;
        const float width = r.w - t.margin[0] - t.margin[2];
        const float pt = font.point_size > 0 ? font.point_size : 48;
        auto runs = Runs(t.text, t.style);
        float size = t.size;
        if (t.autosize) {
            size = t.size_max;
            while (size > t.size_min) {
                const auto lines = LayoutLines(font, runs, size, width, t.wrap);
                float mw = 0;
                for (const auto& l : lines)
                    mw = std::max(mw, l.width);
                const float kk = size / pt;
                const float hh = (font.ascender - font.descender) * kk + static_cast<float>(lines.size() - 1) * font.line_height * kk;
                if (mw <= width + 0.5f && hh <= r.h + 0.5f)
                    break;
                size -= 1;
            }
        }
        const auto lines = LayoutLines(font, runs, size, width, t.wrap);
        const float kk = size / pt;
        const float lh = font.line_height * kk;
        const float total = (font.ascender - font.descender) * kk + static_cast<float>(lines.size() - 1) * lh;
        const float top = r.y + r.h - t.margin[1], bottom = r.y + t.margin[3];
        float base;
        if (t.valign == 256)
            base = top - font.ascender * kk;
        else if (t.valign == 1024)
            base = bottom - font.descender * kk + static_cast<float>(lines.size() - 1) * lh;
        else
            base = (top + bottom) / 2 + total / 2 - font.ascender * kk;
        const bc_shade::Vec4 col{t.color.r, t.color.g, t.color.b, t.color.a * alpha};
        for (const auto& line : lines) {
            float x;
            if (t.halign == 1 || t.halign == 8)
                x = r.x + t.margin[0];
            else if (t.halign == 4)
                x = r.x + r.w - t.margin[2] - line.width;
            else
                x = r.x + t.margin[0] + (width - line.width) / 2;
            for (const auto& c : line.chars) {
                if (c.cp != ' ')
                    out.Text(font, Encode(c.cp), x * k - ox, (ch - base) * k - oy, size * c.k * k, col, false, 0.0f);
                x += c.adv;
            }
            base -= lh;
        }
    }

    void Content(int index, float alpha) {
        const Node& n = s.nodes[static_cast<std::size_t>(index)];
        for (const auto& im : n.images)
            DrawImage(n, im, alpha);
        for (const auto& t : n.texts)
            DrawText(n, t, alpha);
        for (const int ci : n.children)
            Draw(ci, alpha);
    }

    void PushClip(bc_ui::Canvas::ClipMask clip) {
        if (!clips.empty()) {
            // Nested: only where both cover.
            const auto& outer = clips.back();
            for (int v = 0; v < clip.h; ++v)
                for (int u = 0; u < clip.w; ++u)
                    clip.a[static_cast<std::size_t>(v) * clip.w + u] *= outer.At(clip.x + u, clip.y + v);
        }
        clips.push_back(std::move(clip));
        out.SetClip(&clips.back());
    }
    void PopClip() {
        clips.pop_back();
        out.SetClip(clips.empty() ? nullptr : &clips.back());
    }

    void Draw(int index, float alpha) {
        const Node& n = s.nodes[static_cast<std::size_t>(index)];
        if (!n.active || (hide && index < static_cast<int>(hide->size()) && (*hide)[static_cast<std::size_t>(index)]))
            return;
        alpha *= n.alpha;
        if (alpha <= 0.003f)
            return;
        if (n.mask == 1) {
            // Stencil: the mask graphic's coverage clips the subtree. It is drawn alone into a
            // canvas the size of its rect.
            float x, y, w, h;
            Px(n.rect, x, y, w, h);
            const int x0 = std::max(0, static_cast<int>(std::floor(x))), y0 = std::max(0, static_cast<int>(std::floor(y)));
            const int x1 = std::min(out.W(), static_cast<int>(std::ceil(x + w))), y1 = std::min(out.H(), static_cast<int>(std::ceil(y + h)));
            bc_ui::Canvas::ClipMask clip;
            clip.x = x0, clip.y = y0, clip.w = std::max(0, x1 - x0), clip.h = std::max(0, y1 - y0);
            clip.a.assign(static_cast<std::size_t>(clip.w) * clip.h, 0.0f);
            if (x1 > x0 && y1 > y0) {
                bc_ui::Canvas m(x1 - x0, y1 - y0);
                Painter p{s, font, k, ch, m, hide, {}, ox + static_cast<float>(x0), oy + static_cast<float>(y0)};
                for (const auto& im : n.images) {
                    Image solid = im;
                    solid.color.a = 1;
                    solid.runtime = false;
                    p.DrawImage(n, solid, 1.0f);
                }
                const auto a = m.Alpha();
                for (std::size_t i = 0; i < clip.a.size(); ++i)
                    clip.a[i] = std::min(1.0f, a[i] * 4.0f);
            }
            if (n.mask_show)
                for (const auto& im : n.images)
                    DrawImage(n, im, alpha);
            PushClip(std::move(clip));
            for (const auto& t : n.texts)
                DrawText(n, t, alpha);
            for (const int ci : n.children)
                Draw(ci, alpha);
            PopClip();
            return;
        }
        if (n.mask == 2) {
            float x, y, w, h;
            Px(n.rect, x, y, w, h);
            const int x0 = std::max(0, static_cast<int>(std::lround(x))), x1 = std::min(out.W(), static_cast<int>(std::lround(x + w)));
            const int y0 = std::max(0, static_cast<int>(std::lround(y))), y1 = std::min(out.H(), static_cast<int>(std::lround(y + h)));
            bc_ui::Canvas::ClipMask clip;
            clip.x = x0, clip.y = y0, clip.w = std::max(0, x1 - x0), clip.h = std::max(0, y1 - y0);
            clip.a.assign(static_cast<std::size_t>(clip.w) * clip.h, 1.0f);
            PushClip(std::move(clip));
            Content(index, alpha);
            PopClip();
            return;
        }
        Content(index, alpha);
    }
};

} // namespace

std::vector<std::uint8_t> Render(const Scene& scene, const bc_levelmap::Font& font, float scale, int w, int h,
                                 const std::vector<bool>* hide) {
    bc_ui::Canvas canvas(w, h);
    if (scene.root >= 0) {
        Painter p{scene, font, scale, static_cast<float>(h) / scale, canvas, hide, {}, 0, 0};
        p.Draw(scene.root, 1.0f);
    }
    return canvas.Encode();
}

void Fit(Part& part, const Rect& src, float ch, float x, float y, float w, float h, float k_max, bool top) {
    if (src.w <= 0 || src.h <= 0)
        return;
    const float k = std::min({w / src.w, h / src.h, k_max});
    part.k = k;
    // The source's top-left goes to the box's, centred.
    const float dx = x + (w - src.w * k) / 2, dy = top ? y : y + (h - src.h * k) / 2;
    part.ox = src.x * k - dx;
    part.oy = (ch - (src.y + src.h)) * k - dy;
}

Rect Bounds(const Scene& scene, int node, const std::vector<bool>* hide) {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    const std::function<void(int)> walk = [&](int i) {
        const Node& n = scene.nodes[static_cast<std::size_t>(i)];
        if (!n.active || (hide && i < static_cast<int>(hide->size()) && (*hide)[static_cast<std::size_t>(i)]) ||
            n.alpha <= 0.003f)
            return;
        bool drawn = false;
        for (const auto& t : n.texts)
            drawn |= !t.text.empty();
        for (const auto& im : n.images)
            drawn |= (im.sprite != nullptr || !im.runtime) && im.color.a > 0.003f;
        if (drawn && n.rect.w > 0 && n.rect.h > 0) {
            x0 = std::min(x0, n.rect.x), y0 = std::min(y0, n.rect.y);
            x1 = std::max(x1, n.rect.x + n.rect.w), y1 = std::max(y1, n.rect.y + n.rect.h);
        }
        for (int c : n.children)
            walk(c);
    };
    if (node >= 0)
        walk(node);
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

void PartRect(const Part& part, const Rect& r, float ch, float& x, float& y, float& w, float& h) {
    PixelRect(r, part.k, ch, x, y, w, h);
    x -= part.ox;
    y -= part.oy;
}

std::vector<std::uint8_t> RenderParts(const Scene& scene, const bc_levelmap::Font& font, float ch, int w, int h,
                                      const std::vector<Part>& parts, const std::vector<bool>* hide) {
    bc_ui::Canvas canvas(w, h);
    std::vector<bool> mask = hide ? *hide : std::vector<bool>(scene.nodes.size(), false);
    mask.resize(scene.nodes.size(), false);
    for (const auto& part : parts) {
        for (int e : part.exclude)
            if (e >= 0)
                mask[static_cast<std::size_t>(e)] = true;
        Painter p{scene, font, part.k, ch, canvas, &mask, {}, part.ox, part.oy};
        for (int node : part.nodes) {
            if (node < 0)
                continue;
            // The alpha of its parents (canvas groups); nothing when one is off.
            float alpha = 1;
            bool on = true;
            for (int a = scene.nodes[static_cast<std::size_t>(node)].parent; a >= 0;
                 a = scene.nodes[static_cast<std::size_t>(a)].parent) {
                const Node& an = scene.nodes[static_cast<std::size_t>(a)];
                on &= an.active && !mask[static_cast<std::size_t>(a)];
                alpha *= an.alpha;
            }
            if (on)
                p.Draw(node, alpha);
        }
        for (int e : part.exclude)
            if (e >= 0)
                mask[static_cast<std::size_t>(e)] = hide && static_cast<std::size_t>(e) < hide->size() && (*hide)[static_cast<std::size_t>(e)];
    }
    return canvas.Encode();
}

} // namespace bc_menu
