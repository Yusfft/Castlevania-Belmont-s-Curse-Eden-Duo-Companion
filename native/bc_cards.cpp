// SPDX-License-Identifier: GPL-3.0-or-later
#include "bc_cards.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bc_cards {
namespace {

using bc_unity::Value;

constexpr int kClassTexture2D = 28;
constexpr int kClassSpriteAtlas = 687078895;
constexpr char kAtlas[] = "T_UI_Save_BiomeCards";

/// The atlas's packed sprite names, in card order (build 1.1.0: the render data map lists the
/// cards in the same order).
constexpr const char* kNames[kCount] = {
    "T_UI_Save_BiomeCard_HoM",     "T_UI_Save_BiomeCard_MasterQuarter", "T_UI_Save_BiomeCard_Library",
    "T_UI_Save_BiomeCard_Catacombs", "T_UI_Save_BiomeCard_Cemetery",   "T_UI_Save_BiomeCard_MainHall",
    "T_UI_Save_BiomeCard_ClockTower", "T_UI_Save_BiomeCard_StreetPoor", "T_UI_Save_BiomeCard_ND",
};

std::int64_t Int(const Value* v, std::int64_t fb = 0) {
    return v ? v->AsInt(fb) : fb;
}
float Flt(const Value* v) {
    return v ? static_cast<float>(v->AsFloat(0)) : 0.0f;
}
const std::string& Str(const Value* v) {
    static const std::string empty;
    return v && v->kind == Value::Kind::String ? v->s : empty;
}

/// The atlas page `obj` decoded (the Switch layout when it has a platform blob).
std::optional<bc_levelmap::Texture> LoadPage(const bc_levelmap::File& f, const bc_unity::ObjectInfo& obj,
                                             const bc_levelmap::Codecs& codecs, std::string* err) {
    const auto v = f.sf->ReadObject(obj);
    if (!v) {
        *err = "card page unreadable";
        return std::nullopt;
    }
    const auto w = static_cast<std::uint32_t>(Int(v->Get("m_Width")));
    const auto h = static_cast<std::uint32_t>(Int(v->Get("m_Height")));
    const auto fmt = bc_tex::FromTextureFormat(static_cast<int>(Int(v->Get("m_TextureFormat"))));
    if (!fmt || w == 0 || h == 0) {
        *err = "card page format " + std::to_string(Int(v->Get("m_TextureFormat")));
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
            *err = "card page without data";
            return std::nullopt;
        }
        data.resize(size);
        if (!f.ReadStream(Str(sd->Get("path")), static_cast<std::uint64_t>(Int(sd->Get("offset"))), size,
                          data.data())) {
            *err = "card page stream unreadable";
            return std::nullopt;
        }
    }
    auto im = bc_tex::Decode(*fmt, w, h, data.data(), data.size(), shift, codecs.astc);
    if (!im) {
        *err = codecs.astc ? "card page decode failed" : "card page decode failed (no ASTC decoder yet)";
        return std::nullopt;
    }
    bc_levelmap::Texture t;
    t.w = w;
    t.h = h;
    t.slices = 1;
    t.srgb = Int(v->Get("m_ColorSpace")) == 1;
    t.rgba = std::move(im->rgba);
    return t;
}

} // namespace

int ForWorld(const std::string& world_name) {
    // Biome number -> card: 01 Streets, 02 Cemetery, 03 Catacombs, 05 Notre Dame, 06 Main Hall,
    // 08 Library, 09 Clock Tower, 10 Hall of Mirrors, 11 and 12 Master's Quarter.
    static constexpr int kByBiome[] = {-1, 7, 4, 3, -1, 8, 5, -1, 2, 6, 0, 1, 1};
    const int n = bc_levelmap::BiomeNumber(world_name);
    if (n < 0 || n >= static_cast<int>(std::size(kByBiome)))
        return -1;
    return kByBiome[n];
}

std::optional<bc_levelmap::Sprite> Load(bc_levelmap::Assets& assets, const bc_levelmap::Codecs& codecs, int index,
                                        std::string* err) {
    if (index < 0 || index >= kCount) {
        *err = "no such card";
        return std::nullopt;
    }
    const auto ca = assets.Open(assets.Find("ui-commonsalltime_assets_all"));
    if (!ca || !ca->sf) {
        *err = "ui-commonsalltime bundle missing";
        return std::nullopt;
    }
    for (const auto& o : ca->sf->Objects()) {
        if (o.class_id != kClassSpriteAtlas || ca->sf->PeekName(o) != kAtlas)
            continue;
        const auto v = ca->sf->ReadObject(o);
        const Value* map = v ? v->Get("m_RenderDataMap") : nullptr;
        if (!map || map->list.empty()) {
            *err = "card atlas empty";
            return std::nullopt;
        }
        // The packed name's index picks the render data entry.
        std::size_t pick = static_cast<std::size_t>(index);
        if (const Value* names = v->Get("m_PackedSpriteNamesToIndex")) {
            for (std::size_t i = 0; i < names->list.size(); ++i) {
                if (Str(&names->list[i]) == kNames[index]) {
                    pick = i;
                    break;
                }
            }
        }
        if (pick >= map->list.size()) {
            *err = "card missing from the atlas";
            return std::nullopt;
        }
        const Value& entry = map->list[pick];
        const Value* data = entry.Get("second");
        if (!data && entry.list.size() == 2)
            data = &entry.list[1];
        const Value* rect = data ? data->Get("textureRect") : nullptr;
        const auto ref = data ? assets.Resolve(ca, data->Get("texture"), {"ui-"}) : bc_levelmap::Assets::Ref{};
        if (!rect || !ref || ref.obj->class_id != kClassTexture2D) {
            *err = "card page not found";
            return std::nullopt;
        }
        auto tex = LoadPage(*ref.file, *ref.obj, codecs, err);
        if (!tex)
            return std::nullopt;
        const int x0 = std::clamp(static_cast<int>(std::lround(Flt(rect->Get("x")))), 0, static_cast<int>(tex->w) - 1);
        const int y0 = std::clamp(static_cast<int>(std::lround(Flt(rect->Get("y")))), 0, static_cast<int>(tex->h) - 1);
        const int cw = std::clamp(static_cast<int>(std::lround(Flt(rect->Get("width")))), 1, static_cast<int>(tex->w) - x0);
        const int ch = std::clamp(static_cast<int>(std::lround(Flt(rect->Get("height")))), 1, static_cast<int>(tex->h) - y0);
        bc_levelmap::Sprite card;
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
        return card;
    }
    *err = "card atlas not found";
    return std::nullopt;
}

} // namespace bc_cards
