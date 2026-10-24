// SPDX-License-Identifier: GPL-3.0-or-later
// Level map loader. See bc_levelmap.h.

#include "bc_levelmap.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <unordered_set>

namespace bc_levelmap {
namespace {

using bc_unity::ObjectInfo;
using bc_unity::SerializedFile;
using bc_unity::Value;

constexpr char kBundleDir[] = "Data/StreamingAssets/aa/Switch/";
constexpr int kClassGameObject = 1;
constexpr int kClassTransform = 4;
constexpr int kClassMaterial = 21;
constexpr int kClassTexture2D = 28;
constexpr int kClassMonoBehaviour = 114;
constexpr int kClassSprite = 213;
constexpr int kClassSpriteRenderer = 212;
constexpr int kClassTexture2DArray = 187;
constexpr int kClassSpriteAtlas = 687078895;

// Where the referenced assets live: map art per biome, shared UI art, fonts, districts.
const std::vector<std::string> kArtPrefixes = {"art-", "ui-", "map-", "shaders_"};
const std::vector<std::string> kUiPrefixes = {"ui-", "map-", "art-"};
const std::vector<std::string> kDistrictPrefixes = {"district-districtdata_"};

void Log(std::string& log, const std::string& line) {
    if (log.size() < 16384)
        log += line + "\n";
}

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
bool PPtr(const Value* v, std::int64_t& file_id, std::int64_t& path_id) {
    if (!v)
        return false;
    file_id = Int(v->Get("m_FileID"));
    path_id = Int(v->Get("m_PathID"));
    return path_id != 0;
}
std::int64_t PathId(const Value* v) {
    std::int64_t f = 0, p = 0;
    PPtr(v, f, p);
    return f == 0 ? p : 0; // same-file references only
}

std::string CabFromPath(std::string_view p) {
    // "archive:/CAB-x/CAB-x" or "archive:/CAB-x/CAB-x.resS"
    const auto slash = p.rfind('/');
    std::string_view last = slash == std::string_view::npos ? p : p.substr(slash + 1);
    const auto dot = last.find('.');
    if (dot != std::string_view::npos)
        last = last.substr(0, dot);
    return std::string(last);
}

bool HasField(const SerializedFile& sf, const ObjectInfo& o, std::string_view name) {
    const auto* t = sf.TypeOf(o);
    return t && SerializedFile::HasField(*t, name);
}

bool EndsWith(std::string_view s, std::string_view suf) {
    return s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf;
}

float SrgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Files and bundles

bool File::ReadStream(const std::string& path, std::uint64_t offset, std::size_t size, std::uint8_t* out) const {
    if (!bundle)
        return false;
    const auto slash = path.rfind('/');
    const std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
    const bc_unity::BundleNode* n = bundle->FindNode(leaf);
    return n && bundle->ReadNodeRange(*n, offset, size, out);
}

std::string File::ExternalCab(std::int64_t file_id) const {
    if (!sf || file_id <= 0 || static_cast<std::size_t>(file_id) > sf->Externals().size())
        return {};
    return CabFromPath(sf->Externals()[static_cast<std::size_t>(file_id - 1)]);
}

Assets::Assets(RomfsRead read, std::vector<std::string> bundle_names)
    : read_(std::move(read)), names_(std::move(bundle_names)) {}

std::string Assets::Find(std::string_view prefix) const {
    for (const auto& n : names_) {
        if (n.size() >= prefix.size() && n.compare(0, prefix.size(), prefix) == 0)
            return n;
    }
    return {};
}

std::vector<std::string> Assets::FindAll(std::string_view prefix) const {
    std::vector<std::string> out;
    for (const auto& n : names_) {
        if (n.size() >= prefix.size() && n.compare(0, prefix.size(), prefix) == 0)
            out.push_back(n);
    }
    return out;
}

namespace {

std::shared_ptr<bc_unity::LazyBundle> OpenBundle(const RomfsRead& read, const std::string& name) {
    const std::string path = std::string(kBundleDir) + name;
    const std::size_t size = read(path, 0, nullptr, 0);
    if (size == 0)
        return nullptr;
    bc_unity::ReadAt at = [read, path](std::uint64_t off, void* out, std::size_t n) {
        return read(path, off, out, n);
    };
    return bc_unity::LazyBundle::Open(std::move(at), size);
}

std::shared_ptr<File> OpenNode(const std::shared_ptr<bc_unity::LazyBundle>& b, const bc_unity::BundleNode& node,
                               const std::string& bundle_name) {
    auto f = std::make_shared<File>();
    f->bundle_name = bundle_name;
    f->bundle = b;
    f->node = node;
    f->cab = CabFromPath(node.path);
    auto bundle = b;
    const bc_unity::BundleNode n = node;
    f->sf = SerializedFile::Open(
        [bundle, n](std::uint64_t off, std::size_t size, std::uint8_t* out) {
            return bundle->ReadNodeRange(n, off, size, out);
        },
        node.size);
    if (!f->sf)
        return nullptr;
    return f;
}

bool IsMainNode(const bc_unity::BundleNode& n) {
    return n.path.find('.') == std::string::npos;
}

} // namespace

void Assets::Trim() {
    std::lock_guard lock(mu_);
    by_bundle_.clear();
    by_cab_.clear();
}

std::shared_ptr<File> Assets::Open(const std::string& bundle_name) {
    if (bundle_name.empty())
        return nullptr;
    {
        std::lock_guard lock(mu_);
        if (const auto it = by_bundle_.find(bundle_name); it != by_bundle_.end())
            return it->second;
    }
    auto b = OpenBundle(read_, bundle_name);
    std::shared_ptr<File> f;
    if (b) {
        for (const auto& n : b->Nodes()) {
            if (IsMainNode(n)) {
                f = OpenNode(b, n, bundle_name);
                break;
            }
        }
    }
    std::lock_guard lock(mu_);
    by_bundle_[bundle_name] = f;
    if (f)
        by_cab_[f->cab] = f;
    return f;
}

std::shared_ptr<File> Assets::OpenCab(const std::string& cab, const std::vector<std::string>& prefixes) {
    if (cab.empty())
        return nullptr;
    {
        std::lock_guard lock(mu_);
        if (const auto it = by_cab_.find(cab); it != by_cab_.end())
            return it->second;
    }
    for (const auto& prefix : prefixes) {
        for (const auto& name : FindAll(prefix)) {
            std::vector<std::string> paths;
            {
                std::lock_guard lock(mu_);
                if (const auto it = nodes_.find(name); it != nodes_.end())
                    paths = it->second;
            }
            std::shared_ptr<bc_unity::LazyBundle> b;
            if (paths.empty()) {
                b = OpenBundle(read_, name);
                if (b) {
                    for (const auto& n : b->Nodes())
                        paths.push_back(n.path);
                }
                std::lock_guard lock(mu_);
                nodes_[name] = paths;
            }
            bool has = false;
            for (const auto& p : paths)
                has |= p == cab;
            if (!has)
                continue;
            if (!b)
                b = OpenBundle(read_, name);
            if (!b)
                continue;
            for (const auto& n : b->Nodes()) {
                if (n.path == cab) {
                    auto f = OpenNode(b, n, name);
                    std::lock_guard lock(mu_);
                    by_cab_[cab] = f;
                    return f;
                }
            }
        }
    }
    std::lock_guard lock(mu_);
    by_cab_[cab] = nullptr;
    return nullptr;
}

Assets::Ref Assets::Resolve(const std::shared_ptr<File>& from, const Value* pptr,
                            const std::vector<std::string>& prefixes) {
    Ref r;
    std::int64_t fid = 0, pid = 0;
    if (!from || !from->sf || !PPtr(pptr, fid, pid))
        return r;
    r.file = fid == 0 ? from : OpenCab(from->ExternalCab(fid), prefixes);
    if (r.file && r.file->sf)
        r.obj = r.file->sf->Find(pid);
    return r;
}

// ---------------------------------------------------------------------------------------------
// Textures

namespace {

std::optional<Texture> LoadTexture2D(const File& f, const ObjectInfo& obj, const Codecs& codecs, std::string* err) {
    const auto v = f.sf->ReadObject(obj);
    if (!v) {
        *err = "texture unreadable";
        return std::nullopt;
    }
    const auto w = static_cast<std::uint32_t>(Int(v->Get("m_Width")));
    const auto h = static_cast<std::uint32_t>(Int(v->Get("m_Height")));
    const auto fmt = bc_tex::FromTextureFormat(static_cast<int>(Int(v->Get("m_TextureFormat"))));
    if (!fmt) {
        *err = "texture format " + std::to_string(Int(v->Get("m_TextureFormat")));
        return std::nullopt;
    }
    std::vector<std::uint8_t> blob;
    if (const Value* pb = v->Get("m_PlatformBlob")) {
        if (pb->kind == Value::Kind::Bytes)
            blob.assign(pb->s.begin(), pb->s.end());
        for (const auto& e : pb->list)
            blob.push_back(static_cast<std::uint8_t>(e.i));
    }
    const int shift = bc_tex::GobShiftFromPlatformBlob(blob.data(), blob.size());
    std::vector<std::uint8_t> data;
    const Value* img = v->Get("image data");
    if (img && !img->s.empty()) {
        data.assign(img->s.begin(), img->s.end());
    } else if (const Value* sd = v->Get("m_StreamData")) {
        const auto size = static_cast<std::size_t>(Int(sd->Get("size")));
        if (size == 0 || size > bc_unity::kMaxAlloc) {
            *err = "texture without data";
            return std::nullopt;
        }
        data.resize(size);
        if (!f.ReadStream(Str(sd->Get("path")), static_cast<std::uint64_t>(Int(sd->Get("offset"))), size,
                          data.data())) {
            *err = "texture stream unreadable";
            return std::nullopt;
        }
    }
    // A platform blob means the Switch block-linear layout, also with one GOB per block (shift 0).
    auto im = bc_tex::Decode(*fmt, w, h, data.data(), data.size(), shift, codecs.astc);
    if (!im) {
        *err = "texture decode failed";
        return std::nullopt;
    }
    Texture t;
    t.w = w;
    t.h = h;
    t.slices = 1;
    t.srgb = Int(v->Get("m_ColorSpace")) == 1;
    if (const Value* ts = v->Get("m_TextureSettings"))
        t.repeat = Int(ts->Get("m_WrapU"), 1) == 0;
    t.rgba = std::move(im->rgba);
    return t;
}

std::optional<Texture> LoadTextureArray(const File& f, const ObjectInfo& obj, const Codecs& codecs, std::string* err) {
    const auto v = f.sf->ReadObject(obj);
    if (!v) {
        *err = "texture array unreadable";
        return std::nullopt;
    }
    const auto w = static_cast<std::uint32_t>(Int(v->Get("m_Width")));
    const auto h = static_cast<std::uint32_t>(Int(v->Get("m_Height")));
    const auto depth = static_cast<std::uint32_t>(Int(v->Get("m_Depth")));
    const int gfmt = static_cast<int>(Int(v->Get("m_Format")));
    const auto fmt = bc_tex::FromGraphicsFormat(gfmt);
    if (!fmt || depth == 0 || depth > 256) {
        *err = "texture array format " + std::to_string(gfmt);
        return std::nullopt;
    }
    std::vector<std::uint8_t> data;
    const Value* img = v->Get("image data");
    if (img && !img->s.empty()) {
        data.assign(img->s.begin(), img->s.end());
    } else if (const Value* sd = v->Get("m_StreamData")) {
        const auto size = static_cast<std::size_t>(Int(sd->Get("size")));
        if (size == 0 || size > bc_unity::kMaxAlloc) {
            *err = "texture array without data";
            return std::nullopt;
        }
        data.resize(size);
        if (!f.ReadStream(Str(sd->Get("path")), static_cast<std::uint64_t>(Int(sd->Get("offset"))), size,
                          data.data())) {
            *err = "texture array stream unreadable";
            return std::nullopt;
        }
    }
    const std::size_t per = data.size() / depth;
    const std::size_t need = bc_tex::LinearSize(*fmt, w, h);
    if (per < need) {
        *err = "texture array too small";
        return std::nullopt;
    }
    Texture t;
    t.w = w;
    t.h = h;
    t.slices = depth;
    t.srgb = fmt->srgb;
    if (const Value* ts = v->Get("m_TextureSettings"))
        t.repeat = Int(ts->Get("m_WrapU"), 1) == 0;
    t.rgba.resize(static_cast<std::size_t>(w) * h * 4 * depth);
    for (std::uint32_t s = 0; s < depth; ++s) {
        auto im = bc_tex::Decode(*fmt, w, h, data.data() + per * s, need, -1, codecs.astc);
        if (!im) {
            *err = "texture array decode failed";
            return std::nullopt;
        }
        std::memcpy(t.rgba.data() + static_cast<std::size_t>(w) * h * 4 * s, im->rgba.data(), im->rgba.size());
    }
    return t;
}

/// A sprite whose texture is its own (the map icons are not atlased).
std::optional<Sprite> LoadSprite(Assets& assets, const std::shared_ptr<File>& f, const ObjectInfo& obj,
                                 const Codecs& codecs, std::string* err) {
    const auto v = f->sf->ReadObject(obj);
    if (!v) {
        *err = "sprite unreadable";
        return std::nullopt;
    }
    const Value* rect = v->Get("m_Rect");
    const Value* rd = v->Get("m_RD");
    const float ppu = Flt(v->Get("m_PixelsToUnits"), 100.0f);
    if (!rect || !rd || ppu <= 0) {
        *err = "sprite without geometry";
        return std::nullopt;
    }
    const auto ref = assets.Resolve(f, rd->Get("texture"), kUiPrefixes);
    if (!ref || ref.obj->class_id != kClassTexture2D) {
        *err = "sprite texture missing";
        return std::nullopt;
    }
    auto tex = LoadTexture2D(*ref.file, *ref.obj, codecs, err);
    if (!tex)
        return std::nullopt;
    // Crop to the sprite rect (in texture pixels, origin bottom-left = stored row 0).
    const float rx = Flt(rect->Get("x")), ry = Flt(rect->Get("y"));
    const float rw = Flt(rect->Get("width")), rh = Flt(rect->Get("height"));
    const int x0 = std::clamp(static_cast<int>(std::lround(rx)), 0, static_cast<int>(tex->w));
    const int y0 = std::clamp(static_cast<int>(std::lround(ry)), 0, static_cast<int>(tex->h));
    const int cw = std::clamp(static_cast<int>(std::lround(rw)), 1, static_cast<int>(tex->w) - x0);
    const int ch = std::clamp(static_cast<int>(std::lround(rh)), 1, static_cast<int>(tex->h) - y0);
    Sprite s;
    s.tex.w = static_cast<std::uint32_t>(cw);
    s.tex.h = static_cast<std::uint32_t>(ch);
    s.tex.slices = 1;
    s.tex.srgb = tex->srgb;
    s.tex.rgba.resize(static_cast<std::size_t>(cw) * ch * 4);
    for (int y = 0; y < ch; ++y)
        std::memcpy(s.tex.rgba.data() + static_cast<std::size_t>(y) * cw * 4,
                    tex->rgba.data() + (static_cast<std::size_t>(y0 + y) * tex->w + x0) * 4,
                    static_cast<std::size_t>(cw) * 4);
    s.width = rw / ppu;
    s.height = rh / ppu;
    if (const Value* pv = v->Get("m_Pivot")) {
        s.pivot_x = Flt(pv->Get("x"), 0.5f);
        s.pivot_y = Flt(pv->Get("y"), 0.5f);
    }
    if (const Value* b = v->Get("m_Border")) {
        s.border[0] = Flt(b->Get("x")), s.border[1] = Flt(b->Get("y"));
        s.border[2] = Flt(b->Get("z")), s.border[3] = Flt(b->Get("w"));
    }
    return s;
}

// ---- Material ----

struct MaterialValues {
    std::unordered_map<std::string, float> floats;
    std::unordered_map<std::string, bc_shade::Vec4> colors;
    std::unordered_map<std::string, const Value*> textures;
};

MaterialValues ReadMaterial(const Value& m) {
    MaterialValues out;
    const Value* sp = m.Get("m_SavedProperties");
    if (!sp)
        return out;
    if (const Value* fl = sp->Get("m_Floats")) {
        for (const auto& p : fl->list) {
            if (p.list.size() == 2)
                out.floats[p.list[0].s] = static_cast<float>(p.list[1].AsFloat());
        }
    }
    if (const Value* cl = sp->Get("m_Colors")) {
        for (const auto& p : cl->list) {
            if (p.list.size() == 2) {
                const Value& c = p.list[1];
                out.colors[p.list[0].s] = {Flt(c.Get("r")), Flt(c.Get("g")), Flt(c.Get("b")), Flt(c.Get("a"))};
            }
        }
    }
    if (const Value* te = sp->Get("m_TexEnvs")) {
        for (const auto& p : te->list) {
            if (p.list.size() == 2)
                out.textures[p.list[0].s] = p.list[1].Get("m_Texture");
        }
    }
    return out;
}

/// The shader's colour properties that are neither [HDR] nor [Gamma]: Unity converts them to
/// linear before the shader sees them (the project renders in linear space).
bool IsLinearizedColor(std::string_view name) {
    static const char* const kNames[] = {"_GradientColor",     "_StandardBackColor", "_CompletedBackColor",
                                         "_SelectedBackColor", "_CompletedEdgeColor", "_WaterColor"};
    for (const char* n : kNames) {
        if (name == n)
            return true;
    }
    return false;
}

bc_shade::Params ParamsFromMaterial(const MaterialValues& m) {
    bc_shade::Params p;
    const auto F = [&](const char* name, float& out) {
        if (const auto it = m.floats.find(name); it != m.floats.end())
            out = it->second;
    };
    const auto V = [&](const char* name, bc_shade::Vec4& out) {
        if (const auto it = m.colors.find(name); it != m.colors.end()) {
            out = it->second;
            if (IsLinearizedColor(name)) {
                out.x = SrgbToLinear(out.x);
                out.y = SrgbToLinear(out.y);
                out.z = SrgbToLinear(out.z);
            }
        }
    };
    V("_SDGMaskNoiseTexST", p.sdg_noise_st);
    V("_SDGMaskNoiseStrength", p.sdg_noise_strength);
    F("_SDGGlobalRange", p.sdg_global_range);
    F("_SDGGlobalContrast", p.sdg_global_contrast);
    F("_MainLineThickness", p.main_line_thickness);
    F("_MainLineAddRange", p.main_line_add_range);
    F("_MainLineContrast", p.main_line_contrast);
    F("_MainLineNoiseMaskStrength", p.main_line_noise_strength);
    F("_MainLineNoiseMaskContrast", p.main_line_noise_contrast);
    F("_SecondLineThickness", p.second_line_thickness);
    F("_SecondLineAddRange", p.second_line_add_range);
    F("_SecondLineContrast", p.second_line_contrast);
    F("_SecondLineNoiseMaskStrength", p.second_line_noise_strength);
    F("_SecondLineNoiseMaskContrast", p.second_line_noise_contrast);
    F("_CloudSDFMaskRange", p.cloud_range);
    F("_CloudSDFMaskContrast", p.cloud_contrast);
    F("_CollisionSDFMaskRange", p.collision_range);
    F("_CollisionSDFMaskContrast", p.collision_contrast);
    F("_AlphaSDFRange", p.alpha_range);
    F("_AlphaSDFContrast", p.alpha_contrast);
    F("_GradientColorRange", p.gradient_range);
    F("_GradientColorContrast", p.gradient_contrast);
    V("_GradientColor", p.gradient_color);
    V("_GradientColorNoiseMask", p.gradient_noise_mask);
    V("_GradientColorPerlinST", p.gradient_perlin_st);
    F("_GradientColorPerlinMaskRange", p.gradient_perlin_range);
    F("_GradientColorPerlinMaskContrast", p.gradient_perlin_contrast);
    V("_LineColor", p.line_color);
    V("_GlobalNoiseST", p.global_noise_st);
    V("_ColorNoiseMask", p.color_noise_mask);
    F("_GlobalMoveNoiseMaskRange", p.move_noise_range);
    F("_GlobalMoveNoiseMaskContrast", p.move_noise_contrast);
    F("_GlobalMoveNoiseMaskBackColorStrength", p.move_noise_back_strength);
    F("_GlobalMoveNoiseMaskLineStrength", p.move_noise_line_strength);
    V("_StandardBackColor", p.standard_back);
    V("_CompletedBackColor", p.completed_back);
    V("_SelectedBackColor", p.selected_back);
    V("_StandardEdgeColor", p.standard_edge);
    V("_CompletedEdgeColor", p.completed_edge);
    V("_SelectedEdgeColor", p.selected_edge);
    F("_EdgeDetectMin", p.edge_min);
    F("_EdgeDetectMax", p.edge_max);
    V("_SelectedEdgeDisplaceST", p.selected_displace_st);
    V("_SelectedEdgeDisplaceStrength", p.selected_displace_strength);
    F("_SelectedEdgeDisplaceRange", p.selected_displace_range);
    F("_SelectedEdgeDisplaceContrast", p.selected_displace_contrast);
    F("_WaterThickness", p.water_thickness);
    F("_WaterContrast", p.water_contrast);
    V("_WaterColor", p.water_color);
    V("_WaterNoiseStrength", p.water_noise_strength);
    F("_WaterLineDisplaceSpeed", p.water_line_speed);
    F("_WaterLineDisplaceNumber", p.water_line_number);
    F("_WaterLineDisplaceStrength", p.water_line_strength);
    F("_WaterSDFMaskRange", p.water_sdf_range);
    F("_WaterSDFMaskContrast", p.water_sdf_contrast);
    V("_BackgroundST", p.background_st);
    V("_BakcgroundNoiseMask", p.background_noise_mask);
    V("_BakgroundTexNoiseStrength", p.background_tex_noise_strength);
    F("_IsDiscovered", p.is_discovered);
    return p;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Strings

void Strings::Load(Assets& assets) {
    map_.clear();
    const std::string name = assets.Find("localization-string-tables-english");
    const auto f = assets.Open(name);
    if (!f || !f->sf)
        return;
    for (const auto& o : f->sf->Objects()) {
        if (o.class_id != kClassMonoBehaviour || !HasField(*f->sf, o, "m_TableData"))
            continue;
        const std::string n = f->sf->PeekName(o);
        if (n.rfind("World_", 0) != 0 && n.rfind("UI_", 0) != 0 && n.rfind("Items_", 0) != 0)
            continue;
        const auto v = f->sf->ReadObject(o);
        if (!v)
            continue;
        if (const Value* td = v->Get("m_TableData")) {
            for (const auto& e : td->list)
                map_.emplace(Int(e.Get("m_Id")), Str(e.Get("m_Localized")));
        }
    }
}

std::string Strings::Get(std::int64_t key) const {
    const auto it = map_.find(key);
    return it == map_.end() ? std::string{} : it->second;
}

// ---------------------------------------------------------------------------------------------
// Level map

int BiomeNumber(std::string_view world_name) {
    constexpr std::string_view kPrefix = "World_Biome_";
    if (world_name.substr(0, kPrefix.size()) != kPrefix || world_name.size() < kPrefix.size() + 2)
        return -1;
    const char a = world_name[kPrefix.size()], b = world_name[kPrefix.size() + 1];
    if (!std::isdigit(static_cast<unsigned char>(a)) || !std::isdigit(static_cast<unsigned char>(b)))
        return -1;
    return (a - '0') * 10 + (b - '0');
}

const Tile* LevelMap::TileAt(float wx, float wy) const {
    for (const auto& t : tiles) {
        if (wx >= t.x && wx < t.x + t.w && wy >= t.y && wy < t.y + t.h)
            return &t;
    }
    return nullptr;
}

int LevelMap::NavZoneAt(float wx, float wy) const {
    int best = -1;
    float best_area = 0;
    for (std::size_t i = 0; i < nav_zones.size(); ++i) {
        const auto& z = nav_zones[i];
        if (wx < z.x || wy < z.y || wx >= z.x + z.w || wy >= z.y + z.h)
            continue;
        const float a = z.w * z.h;
        if (best < 0 || a < best_area) {
            best = static_cast<int>(i);
            best_area = a;
        }
    }
    return best >= 0 ? best : main_zone;
}

const District* LevelMap::DistrictAt(float wx, float wy) const {
    const int cx = static_cast<int>(std::floor(wx / static_cast<float>(chunk_w)));
    const int cy = static_cast<int>(std::floor(wy / static_cast<float>(chunk_h)));
    for (const auto& d : districts) {
        const int lx = cx - d.x, ly = cy - d.y;
        if (lx < 0 || ly < 0 || lx >= d.w || ly >= d.h)
            continue;
        const std::size_t i = static_cast<std::size_t>(lx + ly * d.w);
        if (i < d.active.size() && d.active[i])
            return &d;
    }
    return nullptr;
}

namespace {

std::int32_t UidFromRoomId(std::string_view id) {
    const auto p = id.rfind("_uid");
    if (p == std::string_view::npos)
        return -1;
    std::int32_t v = 0;
    bool any = false;
    for (std::size_t i = p + 4; i < id.size() && std::isdigit(static_cast<unsigned char>(id[i])); ++i) {
        v = v * 10 + (id[i] - '0');
        any = true;
    }
    return any ? v : -1;
}

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};
Quat Mul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
void Rotate(const Quat& q, float& vx, float& vy, float& vz) {
    const float ix = q.w * vx + q.y * vz - q.z * vy;
    const float iy = q.w * vy + q.z * vx - q.x * vz;
    const float iz = q.w * vz + q.x * vy - q.y * vx;
    const float iw = -q.x * vx - q.y * vy - q.z * vz;
    vx = ix * q.w + iw * -q.x + iy * -q.z - iz * -q.y;
    vy = iy * q.w + iw * -q.y + iz * -q.x - ix * -q.z;
    vz = iz * q.w + iw * -q.z + ix * -q.y - iy * -q.x;
}

struct Xform {
    std::int64_t go = 0, father = 0;
    float px = 0, py = 0, pz = 0, sx = 1, sy = 1, sz = 1;
    Quat q;
    // world, filled lazily
    bool done = false;
    float wx = 0, wy = 0, wz = 0, wsx = 1, wsy = 1;
    Quat wq;
};

struct Go {
    std::string name;
    bool active = true;
    std::int64_t transform = 0;
};

/// What a MonoBehaviour on a map item tells about its icons.
struct ItemInfo {
    enum Kind : std::uint8_t { Generic, SavePoint, ZDoor, Activable, Hidden } kind = Generic;
    std::string room_id;
    std::int32_t zone_id = 0, zdoor_id = -1;
    std::string guid;
    bool unrevealed_ok = false;
};

ItemInfo ClassifyItem(const Value& v, const SerializedFile& sf, const ObjectInfo& o) {
    ItemInfo it;
    it.room_id = Str(v.Get("itemWorldRoomId"));
    it.zone_id = static_cast<std::int32_t>(Int(v.Get("itemExplorationZoneId")));
    it.unrevealed_ok = Int(v.Get("visibleInUnrevealedMap")) != 0;
    if (v.Get("zDoorID")) {
        it.kind = ItemInfo::ZDoor;
        it.zdoor_id = static_cast<std::int32_t>(Int(v.Get("zDoorID")));
    } else if (v.Get("activableUniqueGuid")) {
        it.kind = ItemInfo::Activable;
        it.guid = Str(v.Get("activableUniqueGuid"));
    } else if (v.Get("targetExplorerEntity") && it.room_id.empty()) {
        it.kind = ItemInfo::Hidden; // the player and death markers: drawn by the module itself
    } else if (v.Get("linkedCharacter")) {
        it.kind = ItemInfo::Hidden; // character tokens (masked portraits) are left out
    } else if (HasField(sf, o, "display") && !v.Get("transitionID") && !v.Get("landmarkData") &&
               !v.Get("entranceZDoorID") && !v.Get("secretZoneID")) {
        it.kind = ItemInfo::SavePoint;
    }
    return it;
}

} // namespace

std::shared_ptr<const Names> LoadNames(Assets& assets) {
    auto n = std::make_shared<Names>();
    n->strings.Load(assets);
    const auto key_of = [](const Value& v, const char* field) {
        const Value* k = v.Get(field);
        const Value* e = k ? k->Get("m_TableEntryReference") : nullptr;
        return Int(e ? e->Get("m_KeyId") : nullptr);
    };
    // DistrictDisplayData "X_Display" -> name of DistrictData "X".
    for (const auto& bundle : assets.FindAll("district-districtdata_")) {
        if (bundle.find("savablescriptableobject") != std::string::npos)
            continue;
        const auto f = assets.Open(bundle);
        if (!f || !f->sf)
            continue;
        for (const auto& o : f->sf->Objects()) {
            if (o.class_id != kClassMonoBehaviour || !HasField(*f->sf, o, "districtDisplayName"))
                continue;
            std::string name = f->sf->PeekName(o);
            if (!EndsWith(name, "_Display"))
                continue;
            if (const auto v = f->sf->ReadObject(o)) {
                const std::string text = n->strings.Get(key_of(*v, "districtDisplayName"));
                if (!text.empty())
                    n->districts[name.substr(0, name.size() - 8)] = text;
            }
        }
    }
    // LevelUIDisplayData "<world>_UIDisplay".
    if (const auto f = assets.Open(assets.Find("leveldata_assets_all")); f && f->sf) {
        for (const auto& o : f->sf->Objects()) {
            if (o.class_id != kClassMonoBehaviour || !HasField(*f->sf, o, "levelDisplayName"))
                continue;
            std::string name = f->sf->PeekName(o);
            if (!EndsWith(name, "_UIDisplay"))
                continue;
            if (const auto v = f->sf->ReadObject(o)) {
                const std::string text = n->strings.Get(key_of(*v, "levelDisplayName"));
                const std::string world = name.substr(0, name.size() - 10);
                if (!text.empty())
                    n->levels[world] = text;
                if (const Value* c = v->Get("levelMapBannerColor"))
                    n->banner_colors[world] = {Flt(c->Get("r")), Flt(c->Get("g")), Flt(c->Get("b")), Flt(c->Get("a"), 1)};
            }
        }
    }
    return n;
}

namespace {
std::shared_ptr<const LevelMap> LoadLevelMapImpl(Assets& assets, const Codecs& codecs, const Names& names,
                                                 std::string_view world_name, std::string& log_out);
} // namespace

std::shared_ptr<const LevelMap> LoadLevelMap(Assets& assets, const Codecs& codecs, const Names& names,
                                             std::string_view world_name, std::string* log) {
    std::string l;
    auto m = LoadLevelMapImpl(assets, codecs, names, world_name, l);
    if (log)
        *log = l;
    return m;
}

namespace {
std::shared_ptr<const LevelMap> LoadLevelMapImpl(Assets& assets, const Codecs& codecs, const Names& names,
                                                 std::string_view world_name, std::string& log_out) {
    auto lm = std::make_shared<LevelMap>();
    struct LogOut {
        std::string& out;
        const std::string& src;
        ~LogOut() { out = src; }
    } log_out_guard{log_out, lm->log};
    lm->world_name = std::string(world_name);
    lm->number = BiomeNumber(world_name);
    std::string& log = lm->log;
    if (lm->number < 0) {
        Log(log, "not a biome: " + lm->world_name);
        return nullptr;
    }
    char prefix[32];
    std::snprintf(prefix, sizeof prefix, "maps_scenes_%02d_", lm->number);
    const auto scene = assets.Open(assets.Find(prefix));
    if (!scene || !scene->sf) {
        Log(log, std::string("map scene missing: ") + prefix);
        return nullptr;
    }
    const SerializedFile& sf = *scene->sf;

    // ---- one pass over the scene's objects
    std::unordered_map<std::int64_t, Xform> xf;
    std::unordered_map<std::int64_t, Go> gos;
    std::unordered_map<std::int64_t, std::vector<std::int64_t>> go_behaviours; // GO -> MonoBehaviours
    std::vector<const ObjectInfo*> sprite_renderers;
    const ObjectInfo* renderer = nullptr;
    const ObjectInfo* fow = nullptr;
    const ObjectInfo* model = nullptr;
    const ObjectInfo* context = nullptr;
    std::vector<const ObjectInfo*> districts;
    std::vector<const ObjectInfo*> nav_zones;
    std::vector<const ObjectInfo*> secret_items;
    std::unordered_map<std::int64_t, ItemInfo> items;                  // MonoBehaviour -> item
    std::unordered_map<std::int64_t, IconRule> state_group;            // GameObject -> state
    for (const auto& o : sf.Objects()) {
        if (o.class_id == kClassTransform) {
            const auto v = sf.ReadObject(o);
            if (!v)
                continue;
            Xform x;
            x.go = PathId(v->Get("m_GameObject"));
            x.father = PathId(v->Get("m_Father"));
            if (const Value* p = v->Get("m_LocalPosition"))
                x.px = Flt(p->Get("x")), x.py = Flt(p->Get("y")), x.pz = Flt(p->Get("z"));
            if (const Value* s = v->Get("m_LocalScale"))
                x.sx = Flt(s->Get("x"), 1), x.sy = Flt(s->Get("y"), 1), x.sz = Flt(s->Get("z"), 1);
            if (const Value* q = v->Get("m_LocalRotation"))
                x.q = {Flt(q->Get("x")), Flt(q->Get("y")), Flt(q->Get("z")), Flt(q->Get("w"), 1)};
            xf[o.path_id] = x;
        } else if (o.class_id == kClassGameObject) {
            const auto v = sf.ReadObject(o);
            if (!v)
                continue;
            Go g;
            g.name = Str(v->Get("m_Name"));
            g.active = Int(v->Get("m_IsActive"), 1) != 0;
            gos[o.path_id] = std::move(g);
        } else if (o.class_id == kClassSpriteRenderer) {
            sprite_renderers.push_back(&o);
        } else if (o.class_id == kClassMonoBehaviour) {
            if (HasField(sf, o, "tileRenderers")) {
                renderer = &o;
            } else if (HasField(sf, o, "worldRoomStates")) {
                fow = &o;
            } else if (HasField(sf, o, "globalBoundsCoord")) {
                model = &o;
            } else if (HasField(sf, o, "levelMapRenderer") && HasField(sf, o, "fowModel")) {
                context = &o;
            } else if (HasField(sf, o, "districtGenerationData")) {
                districts.push_back(&o);
            } else if (HasField(sf, o, "cameraBounds") && HasField(sf, o, "navigationZoneCollider")) {
                nav_zones.push_back(&o);
            } else if (HasField(sf, o, "smoothness") && HasField(sf, o, "boxRoundness")) {
                if (const auto v = sf.ReadObject(o)) {
                    const float k = Flt(v->Get("smoothness"), 3.0f);
                    if (k > 0 && k < 64)
                        lm->smoothness = k;
                }
            } else if (HasField(sf, o, "itemWorldRoomId")) {
                if (HasField(sf, o, "secretZoneID"))
                    secret_items.push_back(&o);
                if (const auto v = sf.ReadObject(o)) {
                    items[o.path_id] = ClassifyItem(*v, sf, o);
                    go_behaviours[PathId(v->Get("m_GameObject"))].push_back(o.path_id);
                }
            } else if (HasField(sf, o, "discoveredUnenteredObjects")) {
                if (const auto v = sf.ReadObject(o)) {
                    const auto mark = [&](const char* field, IconRule rule) {
                        if (const Value* l = v->Get(field)) {
                            for (const auto& e : l->list)
                                state_group.emplace(PathId(&e), rule);
                        }
                    };
                    // A GameObject listed in several states (a shared parent) keeps the first one
                    // marked: emplace does not overwrite.
                    mark("discoveredUnenteredObjects", IconRule::ZDoorDiscovered);
                    mark("enteredUncompletedObjects", IconRule::ZDoorEntered);
                    mark("enteredCompletedObjects", IconRule::Always);
                    mark("enteredSelectedObjects", IconRule::Always);
                }
            }
        }
    }
    for (auto& [id, x] : xf) {
        if (auto it = gos.find(x.go); it != gos.end())
            it->second.transform = id;
    }
    Log(log, "scene " + scene->bundle_name + ": " + std::to_string(xf.size()) + " transforms, " +
                 std::to_string(items.size()) + " items, " + std::to_string(sprite_renderers.size()) + " sprites");

    // ---- the level this map shows (LevelMapContext.level -> LevelID._guid)
    if (context) {
        if (const auto v = sf.ReadObject(*context)) {
            const auto ref = assets.Resolve(scene, v->Get("level"), {"leveldata-savablescriptableobject", "leveldata"});
            if (ref) {
                if (const auto lv = ref.file->sf->ReadObject(*ref.obj))
                    lm->level_guid = Str(lv->Get("_guid"));
            }
        }
    }
    Log(log, "level " + (lm->level_guid.empty() ? std::string("?") : lm->level_guid));

    // ---- tiles and material
    if (!renderer) {
        Log(log, "no LevelMapRenderer");
        return nullptr;
    }
    std::shared_ptr<File> mat_file;
    const ObjectInfo* mat_obj = nullptr;
    std::shared_ptr<File> main_array_file;
    std::string main_array_name;
    {
        const auto v = sf.ReadObject(*renderer);
        const Value* list = v ? v->Get("tileRenderers") : nullptr;
        if (!list || list->list.empty()) {
            Log(log, "no tiles");
            return nullptr;
        }
        for (std::size_t i = 0; i < list->list.size(); ++i) {
            const ObjectInfo* to = sf.Find(PathId(&list->list[i]));
            const auto tv = to ? sf.ReadObject(*to) : std::nullopt;
            const Value* b = tv ? tv->Get("bounds") : nullptr;
            if (!b)
                continue;
            Tile t;
            t.x = Flt(b->Get("x"));
            t.y = Flt(b->Get("y"));
            t.w = Flt(b->Get("width"), 128);
            t.h = Flt(b->Get("height"), 128);
            t.slice = static_cast<std::uint32_t>(i); // the list runs row by row from the top, like the slices
            lm->tiles.push_back(t);
            if (!mat_obj) {
                const ObjectInfo* mr = sf.Find(PathId(tv->Get("tileRenderer")));
                const auto mv = mr ? sf.ReadObject(*mr) : std::nullopt;
                const Value* mats = mv ? mv->Get("m_Materials") : nullptr;
                if (mats && !mats->list.empty()) {
                    const auto ref = assets.Resolve(scene, &mats->list[0], kArtPrefixes);
                    if (ref && ref.obj->class_id == kClassMaterial) {
                        mat_file = ref.file;
                        mat_obj = ref.obj;
                    }
                }
            }
        }
    }
    if (lm->tiles.empty() || !mat_obj) {
        Log(log, "tiles or material missing");
        return nullptr;
    }
    lm->x0 = lm->x1 = lm->tiles[0].x;
    lm->y0 = lm->y1 = lm->tiles[0].y;
    for (const auto& t : lm->tiles) {
        lm->x0 = std::min(lm->x0, t.x), lm->y0 = std::min(lm->y0, t.y);
        lm->x1 = std::max(lm->x1, t.x + t.w), lm->y1 = std::max(lm->y1, t.y + t.h);
    }
    {
        const auto mv = mat_file->sf->ReadObject(*mat_obj);
        if (!mv) {
            Log(log, "material unreadable");
            return nullptr;
        }
        Log(log, "material " + Str(mv->Get("m_Name")) + " in " + mat_file->bundle_name);
        const MaterialValues mvals = ReadMaterial(*mv);
        lm->params = ParamsFromMaterial(mvals);
        std::string err;
        const auto tex = [&](const char* prop, bool array, Texture& out) {
            const auto it = mvals.textures.find(prop);
            if (it == mvals.textures.end())
                return false;
            const auto ref = assets.Resolve(mat_file, it->second, kArtPrefixes);
            if (!ref)
                return false;
            std::optional<Texture> t;
            if (array && ref.obj->class_id == kClassTexture2DArray)
                t = LoadTextureArray(*ref.file, *ref.obj, codecs, &err);
            else if (!array && ref.obj->class_id == kClassTexture2D)
                t = LoadTexture2D(*ref.file, *ref.obj, codecs, &err);
            if (!t) {
                Log(log, std::string(prop) + ": " + err);
                return false;
            }
            out = std::move(*t);
            return true;
        };
        if (!tex("_MainTexArray", true, lm->data)) {
            Log(log, "map texture missing");
            return nullptr;
        }
        if (const auto it = mvals.textures.find("_MainTexArray"); it != mvals.textures.end()) {
            const auto ref = assets.Resolve(mat_file, it->second, kArtPrefixes);
            if (ref) {
                main_array_file = ref.file;
                main_array_name = ref.file->sf->PeekName(*ref.obj);
            }
        }
        tex("_BackgroundTexArray", true, lm->silhouette);
        if (!tex("_Background", false, lm->parchment))
            Log(log, "parchment missing");
        if (!tex("_SDGMaskNoiseTex", false, lm->noise))
            Log(log, "noise missing");
        lm->parchment.repeat = lm->noise.repeat = true;
        Log(log, "map texture " + std::to_string(lm->data.w) + "x" + std::to_string(lm->data.h) + "x" +
                     std::to_string(lm->data.slices) + (lm->data.srgb ? " srgb" : " linear") + ", silhouette " +
                     std::to_string(lm->silhouette.slices));
    }

    // ---- fog of war zones
    if (fow) {
        bool truncated = false;
        const auto v = sf.ReadObject(*fow, &truncated);
        std::unordered_map<std::int64_t, const Value*> refs;
        if (v) {
            if (const Value* r = v->Get("references")) {
                if (const Value* ids = r->Get("RefIds")) {
                    for (const auto& e : ids->list)
                        refs[Int(e.Get("rid"))] = &e;
                }
            }
        }
        const Value* rooms = v ? v->Get("worldRoomStates") : nullptr;
        std::size_t shapes = 0;
        for (const auto& room : rooms ? rooms->list : std::vector<Value>{}) {
            const Value* zs = room.Get("zoneStates");
            for (const auto& z : zs ? zs->list : std::vector<Value>{}) {
                Zone zone;
                zone.room_id = Str(z.Get("roomId"));
                zone.room_uid = UidFromRoomId(zone.room_id);
                zone.zone_id = static_cast<std::int32_t>(Int(z.Get("zoneId")));
                zone.culled = Int(z.Get("culled")) != 0;
                const Value* sl = z.Get("shapes");
                for (const auto& s : sl ? sl->list : std::vector<Value>{}) {
                    const auto it = refs.find(Int(s.Get("rid")));
                    if (it == refs.end())
                        continue;
                    const Value* type = it->second->Get("type");
                    const Value* data = it->second->Get("data");
                    const std::string cls = type ? Str(type->Get("class")) : std::string{};
                    if (!data)
                        continue;
                    if (cls == "ExplorationShapeBox") {
                        const Value* p = data->Get("worldPosition");
                        const Value* sz = data->Get("size");
                        if (p && sz)
                            zone.shapes.push_back(bc_fog::MakeBox(Flt(p->Get("x")), Flt(p->Get("y")),
                                                                  Flt(sz->Get("x")) * 0.5f, Flt(sz->Get("y")) * 0.5f));
                    } else if (cls == "ExplorationShapeQuad") {
                        const Value* vs = data->Get("vertices");
                        if (vs && vs->list.size() >= 4) {
                            float q[8];
                            for (int k = 0; k < 4; ++k) {
                                q[2 * k] = Flt(vs->list[static_cast<std::size_t>(k)].Get("x"));
                                q[2 * k + 1] = Flt(vs->list[static_cast<std::size_t>(k)].Get("y"));
                            }
                            zone.shapes.push_back(bc_fog::MakeQuad(q));
                        }
                    }
                }
                shapes += zone.shapes.size();
                lm->zones.push_back(std::move(zone));
            }
        }
        Log(log, std::to_string(lm->zones.size()) + " zones, " + std::to_string(shapes) + " shapes" +
                     (truncated ? " (truncated)" : ""));
    } else {
        Log(log, "no fog-of-war holder");
    }

    // ---- districts and names
    if (model) {
        if (const auto v = sf.ReadObject(*model)) {
            if (const Value* cs = v->Get("chunkSize")) {
                lm->chunk_w = static_cast<std::int32_t>(std::lround(Flt(cs->Get("x"), 24)));
                lm->chunk_h = static_cast<std::int32_t>(std::lround(Flt(cs->Get("y"), 13)));
            }
        }
    }
    for (const ObjectInfo* o : districts) {
        const auto v = sf.ReadObject(*o);
        const Value* g = v ? v->Get("districtGenerationData") : nullptr;
        const Value* b = g ? g->Get("districtChunksBounds") : nullptr;
        if (!b)
            continue;
        District d;
        d.x = static_cast<std::int32_t>(Int(b->Get("x")));
        d.y = static_cast<std::int32_t>(Int(b->Get("y")));
        d.w = static_cast<std::int32_t>(Int(b->Get("width")));
        d.h = static_cast<std::int32_t>(Int(b->Get("height")));
        if (const Value* a = g->Get("activeChunks")) {
            if (a->kind == Value::Kind::Bytes)
                d.active.assign(a->s.begin(), a->s.end());
            for (const auto& e : a->list)
                d.active.push_back(static_cast<std::uint8_t>(e.i));
        }
        if (const auto ref = assets.Resolve(scene, g->Get("district"), kDistrictPrefixes)) {
            const auto it = names.districts.find(ref.file->sf->PeekName(*ref.obj));
            if (it != names.districts.end())
                d.name = it->second;
        }
        lm->districts.push_back(std::move(d));
    }
    if (const auto it = names.levels.find(lm->world_name); it != names.levels.end())
        lm->level_name = it->second;
    {
        std::size_t named = 0;
        for (const auto& d : lm->districts)
            named += !d.name.empty();
        Log(log, "level '" + lm->level_name + "', " + std::to_string(lm->districts.size()) + " districts (" +
                     std::to_string(named) + " named)");
    }

    // ---- secret zones: the "_secret" texture's texels over each rect
    if (!secret_items.empty() && main_array_file && !main_array_name.empty()) {
        const ObjectInfo* sec_obj = nullptr;
        for (const auto& o : main_array_file->sf->Objects()) {
            if (o.class_id == kClassTexture2DArray && main_array_file->sf->PeekName(o) == main_array_name + "_secret") {
                sec_obj = &o;
                break;
            }
        }
        std::optional<Texture> sec;
        std::string err;
        if (sec_obj)
            sec = LoadTextureArray(*main_array_file, *sec_obj, codecs, &err);
        if (sec && sec->w == lm->data.w && sec->h == lm->data.h && sec->slices == lm->data.slices) {
            for (const ObjectInfo* o : secret_items) {
                const auto v = sf.ReadObject(*o);
                if (!v)
                    continue;
                const ObjectInfo* dobj = sf.Find(PathId(v->Get("display")));
                const auto dv = dobj ? sf.ReadObject(*dobj) : std::nullopt;
                const Value* r = dv ? dv->Get("secretZoneRect") : nullptr;
                if (!r)
                    continue;
                Secret s;
                s.room_id = Str(v->Get("itemWorldRoomId"));
                s.secret_id = static_cast<std::int32_t>(Int(v->Get("secretZoneID")));
                s.x = Flt(r->Get("x")), s.y = Flt(r->Get("y"));
                s.w = Flt(r->Get("width")), s.h = Flt(r->Get("height"));
                if (s.w <= 0 || s.h <= 0 || s.w > 512 || s.h > 512)
                    continue;
                const float tw = lm->tiles[0].w;
                s.texel = tw / static_cast<float>(sec->w);
                // Patch on the texel grid, 3 texels of margin.
                const auto snap = [&](float v0, float origin) {
                    return origin + std::floor((v0 - origin) / s.texel) * s.texel;
                };
                s.tx = snap(s.x, lm->x0) - 3 * s.texel;
                s.ty = snap(s.y, lm->y0) - 3 * s.texel;
                const int pw = static_cast<int>(std::ceil((s.x + s.w - s.tx) / s.texel)) + 3;
                const int ph = static_cast<int>(std::ceil((s.y + s.h - s.ty) / s.texel)) + 3;
                s.tex.w = static_cast<std::uint32_t>(pw);
                s.tex.h = static_cast<std::uint32_t>(ph);
                s.tex.slices = 1;
                s.tex.srgb = sec->srgb;
                s.tex.rgba.assign(static_cast<std::size_t>(pw) * ph * 4, 0);
                for (int py = 0; py < ph; ++py) {
                    for (int px = 0; px < pw; ++px) {
                        const float wx = s.tx + (static_cast<float>(px) + 0.5f) * s.texel;
                        const float wy = s.ty + (static_cast<float>(py) + 0.5f) * s.texel;
                        const Tile* t = lm->TileAt(wx, wy);
                        if (!t)
                            continue;
                        const int sx = std::clamp(static_cast<int>((wx - t->x) / s.texel), 0, static_cast<int>(sec->w) - 1);
                        const int sy = std::clamp(static_cast<int>((wy - t->y) / s.texel), 0, static_cast<int>(sec->h) - 1);
                        const std::uint8_t* src = sec->rgba.data() +
                                                  ((static_cast<std::size_t>(t->slice) * sec->h + static_cast<std::size_t>(sy)) * sec->w +
                                                   static_cast<std::size_t>(sx)) * 4;
                        std::memcpy(s.tex.rgba.data() + (static_cast<std::size_t>(py) * pw + px) * 4, src, 4);
                    }
                }
                lm->secrets.push_back(std::move(s));
            }
        } else {
            Log(log, "secret texture: " + (sec_obj ? err : std::string("missing")));
        }
        Log(log, std::to_string(lm->secrets.size()) + " secret zones");
    }

    // ---- navigation zones
    for (const ObjectInfo* o : nav_zones) {
        const auto v = sf.ReadObject(*o);
        const Value* b = v ? v->Get("cameraBounds") : nullptr;
        if (!b)
            continue;
        NavZone z;
        z.x = Flt(b->Get("x"));
        z.y = Flt(b->Get("y"));
        z.w = Flt(b->Get("width"));
        z.h = Flt(b->Get("height"));
        if (z.w <= 0 || z.h <= 0)
            continue;
        if (const auto g = gos.find(PathId(v->Get("m_GameObject"))); g != gos.end())
            z.name = g->second.name;
        lm->nav_zones.push_back(std::move(z));
    }
    for (std::size_t i = 0; i < lm->nav_zones.size(); ++i) {
        const auto& z = lm->nav_zones[i];
        const bool named_main = z.name.find("Main") != std::string::npos;
        if (lm->main_zone < 0 || named_main ||
            (lm->nav_zones[static_cast<std::size_t>(lm->main_zone)].name.find("Main") == std::string::npos &&
             z.w * z.h > lm->nav_zones[static_cast<std::size_t>(lm->main_zone)].w *
                             lm->nav_zones[static_cast<std::size_t>(lm->main_zone)].h))
            lm->main_zone = static_cast<int>(i);
    }
    Log(log, std::to_string(lm->nav_zones.size()) + " navigation zones");

    // ---- icons: every sprite under a map item, with the state that shows it
    const auto world_of = [&](std::int64_t tid) {
        // Iterative: collect the chain to the root, then compose downwards.
        std::vector<Xform*> chain;
        for (std::int64_t t = tid; t != 0 && chain.size() < 64;) {
            auto it = xf.find(t);
            if (it == xf.end())
                break;
            chain.push_back(&it->second);
            if (it->second.done)
                break;
            t = it->second.father;
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            Xform& x = **it;
            if (x.done)
                continue;
            const auto pit = xf.find(x.father);
            if (pit == xf.end() || !pit->second.done) {
                x.wx = x.px, x.wy = x.py, x.wz = x.pz, x.wsx = x.sx, x.wsy = x.sy, x.wq = x.q;
            } else {
                const Xform& p = pit->second;
                float vx = x.px * p.wsx, vy = x.py * p.wsy, vz = x.pz;
                Rotate(p.wq, vx, vy, vz);
                x.wx = p.wx + vx, x.wy = p.wy + vy, x.wz = p.wz + vz;
                x.wsx = p.wsx * x.sx, x.wsy = p.wsy * x.sy;
                x.wq = Mul(p.wq, x.q);
            }
            x.done = true;
        }
        return chain.empty() ? nullptr : chain.front();
    };
    std::set<std::string> wanted_sprites;
    std::unordered_map<std::string, std::pair<std::shared_ptr<File>, const ObjectInfo*>> sprite_refs;
    for (const ObjectInfo* o : sprite_renderers) {
        const auto v = sf.ReadObject(*o);
        if (!v || Int(v->Get("m_Enabled"), 1) == 0)
            continue;
        const std::int64_t go = PathId(v->Get("m_GameObject"));
        const auto git = gos.find(go);
        if (git == gos.end())
            continue;
        // Walk up: activity, the state group, the owning item.
        bool active = true;
        bool in_cursor = false;
        IconRule group = IconRule::Always;
        bool has_group = false;
        const ItemInfo* item = nullptr;
        std::int64_t t = git->second.transform;
        for (int depth = 0; t != 0 && depth < 64; ++depth) {
            const auto xit = xf.find(t);
            if (xit == xf.end())
                break;
            const std::int64_t g = xit->second.go;
            const auto sg = state_group.find(g);
            if (sg != state_group.end() && !has_group) {
                has_group = true;
                group = sg->second;
            } else if (const auto gi = gos.find(g); gi != gos.end()) {
                active = active && gi->second.active;
                if (gi->second.name.find("Cursor") != std::string::npos)
                    in_cursor = true;
            }
            if (!item) {
                if (const auto bh = go_behaviours.find(g); bh != go_behaviours.end()) {
                    for (const std::int64_t mb : bh->second) {
                        if (const auto ii = items.find(mb); ii != items.end()) {
                            item = &ii->second;
                            break;
                        }
                    }
                }
            }
            t = xit->second.father;
        }
        // Sprite name and filters.
        const auto ref = assets.Resolve(scene, v->Get("m_Sprite"), kUiPrefixes);
        if (!ref || ref.obj->class_id != kClassSprite)
            continue;
        const std::string sname = ref.file->sf->PeekName(*ref.obj);
        const bool last_save = sname == "Map_LastSaveRoomIcon";
        if (!active && !last_save)
            continue;
        if (in_cursor || sname.empty() || sname == "Circle" || sname.find("Selected") != std::string::npos ||
            EndsWith(sname, "_Hover") || git->second.name == "FastTravelIcon_Locked")
            continue;
        if (has_group && group == IconRule::Always)
            continue; // completed / selected variants
        Icon icon;
        icon.sprite = sname;
        if (item) {
            if (item->kind == ItemInfo::Hidden)
                continue;
            icon.room_id = item->room_id;
            icon.room_uid = UidFromRoomId(item->room_id);
            icon.zone_id = item->zone_id;
            icon.zdoor_id = item->zdoor_id;
            icon.guid = item->guid;
            icon.unrevealed_ok = item->unrevealed_ok;
            if (has_group)
                icon.rule = group;
            else if (last_save)
                icon.rule = IconRule::LastSave;
            else if (item->kind == ItemInfo::Activable)
                icon.rule = IconRule::NotActivated;
            else
                icon.rule = IconRule::ZoneEntered;
        } else {
            if (has_group || last_save)
                continue;
            icon.rule = IconRule::Always;
        }
        const Xform* w = world_of(git->second.transform);
        if (!w)
            continue;
        icon.x = w->wx, icon.y = w->wy, icon.z = w->wz;
        icon.sx = w->wsx, icon.sy = w->wsy;
        icon.z -= 0.001f * static_cast<float>(Int(v->Get("m_SortingOrder")));
        lm->icons.push_back(std::move(icon));
        if (wanted_sprites.insert(sname).second)
            sprite_refs[sname] = {ref.file, ref.obj};
    }
    // Back to front.
    std::stable_sort(lm->icons.begin(), lm->icons.end(), [](const Icon& a, const Icon& b) { return a.z > b.z; });
    for (const auto& [name, ref] : sprite_refs) {
        std::string err;
        if (auto s = LoadSprite(assets, ref.first, *ref.second, codecs, &err))
            lm->sprites[name] = std::move(*s);
        else
            Log(log, "sprite " + name + ": " + err);
    }
    Log(log, std::to_string(lm->icons.size()) + " icons, " + std::to_string(lm->sprites.size()) + " sprites");
    return lm;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Common: player marker and title font

std::shared_ptr<const Common> LoadCommon(Assets& assets, const Codecs& codecs) {
    auto c = std::make_shared<Common>();
    std::string& log = c->log;
    if (const auto ui = assets.Open(assets.Find("ui-gamemenus_assets_all")); ui && ui->sf) {
        // The player marker and the map screen's own frame, banner, legend box, button frame
        // and zoom scale.
        static const char* const kUi[] = {"Map_Background", "Map_InfoParchment", "Map_LegendBox", "cartouche",
                                          "Map_Scale", "Map_ScaleCursor", "Map_ScaleNavigationZoom",
                                          "Map_ScaleNavigationDeZoom"};
        for (const auto& o : ui->sf->Objects()) {
            if (o.class_id != kClassSprite)
                continue;
            const std::string name = ui->sf->PeekName(o);
            const bool player = name == "Map_PlayerIcon";
            // The death spot and the player's own map markers.
            bool wanted = player || name == "Map_DeathLocationIcon" || name.rfind("Map_MarkerSet_", 0) == 0;
            for (const char* k : kUi)
                wanted |= name == k;
            if (!wanted || (!player && c->ui.count(name)))
                continue;
            std::string err;
            auto s = LoadSprite(assets, ui, o, codecs, &err);
            if (!s) {
                Log(log, name + ": " + err);
                continue;
            }
            if (player)
                c->player = std::move(*s);
            else
                c->ui[name] = std::move(*s);
        }
        Log(log, std::to_string(c->ui.size()) + " ui sprites");
    } else {
        Log(log, "ui-gamemenus bundle missing");
    }
    // One of the save screen's painted biome cards (sprite atlas T_UI_Save_BiomeCards), picked
    // at random: the art of the page shown before a save is loaded.
    if (const auto ca = assets.Open(assets.Find("ui-commonsalltime_assets_all")); ca && ca->sf) {
        for (const auto& o : ca->sf->Objects()) {
            if (o.class_id != kClassSpriteAtlas || ca->sf->PeekName(o) != "T_UI_Save_BiomeCards")
                continue;
            const auto v = ca->sf->ReadObject(o);
            const Value* map = v ? v->Get("m_RenderDataMap") : nullptr;
            if (!map || map->list.empty())
                break;
            const auto pick = static_cast<std::size_t>(
                std::chrono::steady_clock::now().time_since_epoch().count() / 1000 % static_cast<long long>(map->list.size()));
            const Value& entry = map->list[pick];
            const Value* data = entry.Get("second");
            if (!data && entry.list.size() == 2)
                data = &entry.list[1];
            const Value* rect = data ? data->Get("textureRect") : nullptr;
            const auto ref = data ? assets.Resolve(ca, data->Get("texture"), {"ui-"}) : Assets::Ref{};
            if (!rect || !ref || ref.obj->class_id != kClassTexture2D)
                break;
            std::string err;
            auto tex = LoadTexture2D(*ref.file, *ref.obj, codecs, &err);
            if (!tex) {
                Log(log, "biome card: " + err);
                break;
            }
            const int x0 = std::clamp(static_cast<int>(std::lround(Flt(rect->Get("x")))), 0, static_cast<int>(tex->w));
            const int y0 = std::clamp(static_cast<int>(std::lround(Flt(rect->Get("y")))), 0, static_cast<int>(tex->h));
            const int cw = std::clamp(static_cast<int>(std::lround(Flt(rect->Get("width")))), 1, static_cast<int>(tex->w) - x0);
            const int ch = std::clamp(static_cast<int>(std::lround(Flt(rect->Get("height")))), 1, static_cast<int>(tex->h) - y0);
            Sprite card;
            card.tex.w = static_cast<std::uint32_t>(cw);
            card.tex.h = static_cast<std::uint32_t>(ch);
            card.tex.slices = 1;
            card.tex.srgb = tex->srgb;
            card.tex.rgba.resize(static_cast<std::size_t>(cw) * ch * 4);
            for (int y = 0; y < ch; ++y)
                std::memcpy(card.tex.rgba.data() + static_cast<std::size_t>(y) * cw * 4,
                            tex->rgba.data() + (static_cast<std::size_t>(y0 + y) * tex->w + x0) * 4,
                            static_cast<std::size_t>(cw) * 4);
            card.width = static_cast<float>(cw) / 100.0f;
            card.height = static_cast<float>(ch) / 100.0f;
            c->ui["menu_card"] = std::move(card);
            Log(log, "biome card " + std::to_string(pick) + " " + std::to_string(cw) + "x" + std::to_string(ch));
            break;
        }
    }
    // The common in-game UI's ornament and vignette (game-over and pause screens).
    if (const auto ci = assets.Open(assets.Find("ui-commonsingame_assets_all")); ci && ci->sf) {
        for (const auto& o : ci->sf->Objects()) {
            if (o.class_id != kClassSprite)
                continue;
            const std::string name = ci->sf->PeekName(o);
            if ((name != "CommonUI_Ornament_01" && name != "T_UI_GEN_Vignette") || c->ui.count(name))
                continue;
            std::string err;
            if (auto sp = LoadSprite(assets, ci, o, codecs, &err))
                c->ui[name] = std::move(*sp);
            else
                Log(log, name + ": " + err);
        }
    }
    // Marker types: "MapMarker_<Name>_ID" (a MapMarkerTypeID, with the guid the save keeps) is
    // shown with "Map_MarkerSet_<Name>" (its MapMarker_<Name>_DisplayData's icon).
    if (const auto so = assets.Open(assets.Find("savablescriptableobject_assets_all")); so && so->sf) {
        for (const auto& o : so->sf->Objects()) {
            if (o.class_id != kClassMonoBehaviour)
                continue;
            const std::string name = so->sf->PeekName(o);
            if (name.rfind("MapMarker_", 0) != 0 || !EndsWith(name, "_ID"))
                continue;
            const std::string sprite = "Map_MarkerSet_" + name.substr(10, name.size() - 13);
            if (!c->ui.count(sprite))
                continue;
            if (const auto v = so->sf->ReadObject(o)) {
                const std::string guid = Str(v->Get("_guid"));
                if (!guid.empty())
                    c->marker_types[guid] = sprite;
            }
        }
        Log(log, std::to_string(c->marker_types.size()) + " marker types");
    } else {
        Log(log, "marker types missing");
    }
    if (const auto ff = assets.Open(assets.Find("fonts-latin_assets_all")); ff && ff->sf) {
        for (const auto& o : ff->sf->Objects()) {
            if (o.class_id != kClassMonoBehaviour || !HasField(*ff->sf, o, "m_GlyphTable"))
                continue;
            if (ff->sf->PeekName(o) != "LatinFont")
                continue;
            const auto v = ff->sf->ReadObject(o);
            if (!v)
                break;
            Font& font = c->font;
            if (const Value* fi = v->Get("m_FaceInfo")) {
                font.point_size = Flt(fi->Get("m_PointSize"));
                font.line_height = Flt(fi->Get("m_LineHeight"));
                font.ascender = Flt(fi->Get("m_AscentLine"));
            }
            font.padding = Flt(v->Get("m_AtlasPadding"), 8);
            std::unordered_map<std::int64_t, Glyph> by_index;
            if (const Value* gt = v->Get("m_GlyphTable")) {
                for (const auto& g : gt->list) {
                    Glyph gl;
                    const Value* m = g.Get("m_Metrics");
                    const Value* r = g.Get("m_GlyphRect");
                    if (!m || !r)
                        continue;
                    gl.x = Flt(r->Get("m_X")), gl.y = Flt(r->Get("m_Y"));
                    gl.w = Flt(r->Get("m_Width")), gl.h = Flt(r->Get("m_Height"));
                    gl.bx = Flt(m->Get("m_HorizontalBearingX"));
                    gl.by = Flt(m->Get("m_HorizontalBearingY"));
                    gl.advance = Flt(m->Get("m_HorizontalAdvance"));
                    by_index[Int(g.Get("m_Index"))] = gl;
                }
            }
            if (const Value* ct = v->Get("m_CharacterTable")) {
                for (const auto& ch : ct->list) {
                    const auto it = by_index.find(Int(ch.Get("m_GlyphIndex")));
                    if (it == by_index.end())
                        continue;
                    Glyph gl = it->second;
                    gl.code = static_cast<std::uint32_t>(Int(ch.Get("m_Unicode")));
                    font.glyphs[gl.code] = gl;
                }
            }
            if (const Value* at = v->Get("m_AtlasTextures"); at && !at->list.empty()) {
                const auto ref = assets.Resolve(ff, &at->list[0], {"fonts-"});
                std::string err;
                if (ref && ref.obj->class_id == kClassTexture2D) {
                    if (auto t = LoadTexture2D(*ref.file, *ref.obj, codecs, &err))
                        font.atlas = std::move(*t);
                    else
                        Log(log, "font atlas: " + err);
                }
            }
            Log(log, "font " + std::to_string(font.glyphs.size()) + " glyphs, atlas " +
                         std::to_string(font.atlas.w) + "x" + std::to_string(font.atlas.h));
            break;
        }
    } else {
        Log(log, "fonts-latin bundle missing");
    }
    return c;
}

} // namespace bc_levelmap
