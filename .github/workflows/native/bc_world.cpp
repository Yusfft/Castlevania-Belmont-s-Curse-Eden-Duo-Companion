// SPDX-License-Identifier: GPL-3.0-or-later
// Room layout loader. See bc_world.h.

#include "bc_world.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

#include "bc_unity.h"

namespace bc_world {

using bc_unity::Value;

const Room* Biome::FindUid(std::int32_t uid) const {
    for (const auto& r : rooms) {
        if (r.uid == uid)
            return &r;
    }
    return nullptr;
}

const Biome* World::FindWorld(std::string_view world_name) const {
    for (const auto& b : biomes) {
        if (b.world_name == world_name)
            return &b;
    }
    return nullptr;
}

std::size_t World::RoomCount() const {
    std::size_t n = 0;
    for (const auto& b : biomes)
        n += b.rooms.size();
    return n;
}

const std::vector<std::string>& PinnedBiomeBundles() {
    // Build 0F1169AA7D0FA452 (game version 1.1.0). Names only: the files are the player's own.
    static const std::vector<std::string> names = {
        "data-world_biome_01_streets_poor_assets_all_23b02b049ba03fe6ba07c15c0344bb8f.bundle",
        "data-world_biome_02_innocentscemetery_assets_all_fcba54839881790227496919b552d0cd.bundle",
        "data-world_biome_03_catacombs_assets_all_2ba40ce82d86fa44171542115000c5df.bundle",
        "data-world_biome_05_notredame_assets_all_e299b6a2add0b52db3c5cd4da139b416.bundle",
        "data-world_biome_06_mainhall_assets_all_ab8d489b546cd4880035d15480269905.bundle",
        "data-world_biome_08_library_assets_all_88d6a474162457ce1cafbc4c60c3f25a.bundle",
        "data-world_biome_09_clocktower_assets_all_ef10956e8d0a4d38bf34bb4c87bafe78.bundle",
        "data-world_biome_10_hallofmirrors_assets_all_799695d4b97ad4de039c2fa4abcd0696.bundle",
        "data-world_biome_11_masterquarter_assets_all_dc5705e02c1166b8b182a02f2d869663.bundle",
        "data-world_biome_12_mqbadending_assets_all_fadf140969c1464afd57eee9d998d80f.bundle",
    };
    return names;
}

namespace {

std::int64_t Int(const Value* v, std::int64_t fallback = 0) {
    return v ? v->AsInt(fallback) : fallback;
}

std::int64_t PathId(const Value* pptr) {
    if (!pptr)
        return 0;
    if (Int(pptr->Get("m_FileID"), -1) != 0)
        return 0; // only same-file references are followed
    return Int(pptr->Get("m_PathID"));
}

void SetError(std::string* e, std::string what) {
    if (e)
        *e = std::move(what);
}

} // namespace

std::optional<Biome> LoadBiome(const std::vector<std::uint8_t>& bytes, std::string bundle_name,
                               std::string* error) {
    std::string err;
    auto bundle = bc_unity::Bundle::Parse(bytes.data(), bytes.size(), &err);
    if (!bundle) {
        SetError(error, "bundle: " + err);
        return std::nullopt;
    }
    std::optional<bc_unity::SerializedFile> sf;
    for (const auto& node : bundle->nodes) {
        if (node.path.find(".res") != std::string::npos)
            continue;
        auto nb = bundle->NodeBytes(node);
        if (!nb)
            continue;
        sf = bc_unity::SerializedFile::Parse(nb->first, nb->second, &err);
        if (sf)
            break;
    }
    if (!sf) {
        SetError(error, "serialized file: " + err);
        return std::nullopt;
    }

    // Read every MonoBehaviour once; keep the three kinds this map needs.
    std::unordered_map<std::int64_t, Value> by_id;
    const Value* world = nullptr;
    std::int64_t world_id = 0;
    for (const auto& o : sf->Objects()) {
        if (o.class_id != 114)
            continue;
        auto v = sf->ReadObject(o);
        if (!v)
            continue;
        auto [it, _] = by_id.emplace(o.path_id, std::move(*v));
        if (it->second.Get("_rooms") && it->second.Get("worldSettings")) {
            world_id = o.path_id;
        }
    }
    if (auto it = by_id.find(world_id); world_id != 0 && it != by_id.end())
        world = &it->second;
    if (!world) {
        SetError(error, "no WorldData object");
        return std::nullopt;
    }

    Biome biome;
    biome.bundle = std::move(bundle_name);
    if (const Value* name = world->Get("m_Name"))
        biome.world_name = name->s;
    const Value* rooms = world->Get("_rooms");
    if (!rooms || !rooms->IsList()) {
        SetError(error, "WorldData has no room list");
        return std::nullopt;
    }
    bool first = true;
    for (const Value& wr : rooms->list) {
        Room room;
        room.uid = static_cast<std::int32_t>(Int(wr.Get("runtimeRoomNumber"), -1));
        if (const Value* pc = wr.Get("positionCoord")) {
            room.pos_x = static_cast<std::int32_t>(Int(pc->Get("x")));
            room.pos_y = static_cast<std::int32_t>(Int(pc->Get("y")));
        }
        if (const Value* sc = wr.Get("sceneAddressableEntryAddress"))
            room.scene = sc->s;
        const auto rd_it = by_id.find(PathId(wr.Get("roomData")));
        if (rd_it == by_id.end())
            continue; // a room whose data lives elsewhere is left off the map
        const Value& rd = rd_it->second;
        if (const Value* n = rd.Get("m_Name"))
            room.name = n->s;
        room.w = static_cast<std::int32_t>(Int(rd.Get("_width")));
        room.h = static_cast<std::int32_t>(Int(rd.Get("_height")));
        if (const Value* cs = rd.Get("chunkSize")) {
            room.chunk_w = static_cast<std::int32_t>(Int(cs->Get("x")));
            room.chunk_h = static_cast<std::int32_t>(Int(cs->Get("y")));
        }
        const Value* chunks = rd.Get("chunks");
        if (room.w <= 0 || room.h <= 0 || room.w > 64 || room.h > 64 || room.chunk_w <= 0 ||
            room.chunk_h <= 0 || room.chunk_w > 256 || room.chunk_h > 256 || !chunks ||
            !chunks->IsList() ||
            chunks->list.size() != static_cast<std::size_t>(room.w) * static_cast<std::size_t>(room.h))
            continue;
        const std::size_t tiles = static_cast<std::size_t>(room.chunk_w) * room.chunk_h;
        for (const Value& cv : chunks->list) {
            Chunk ch;
            ch.enabled = Int(cv.Get("enabled")) != 0;
            ch.doors = static_cast<std::uint32_t>(Int(cv.Get("doorsBitField")));
            if (const Value* col = cv.Get("_collisions"); col && col->IsList()) {
                ch.collisions.reserve(col->list.size());
                for (const Value& t : col->list)
                    ch.collisions.push_back(static_cast<std::uint8_t>(t.AsInt()));
            }
            if (ch.collisions.size() != tiles)
                ch.collisions.assign(tiles, 1); // unknown layout: draw the chunk as solid
            room.chunks.push_back(std::move(ch));
        }
        if (auto cfg = by_id.find(PathId(rd.Get("configuration"))); cfg != by_id.end()) {
            room.save_room = Int(cfg->second.Get("isSaveRoom")) != 0;
            room.boss_room = Int(cfg->second.Get("isBossRoom")) != 0;
            room.fast_travel_room = Int(cfg->second.Get("isFastTravelRoom")) != 0;
        }
        for (std::int32_t i = 0; i < room.w * room.h; ++i) {
            if (!room.chunks[static_cast<std::size_t>(i)].enabled)
                continue;
            const std::int32_t cx = room.pos_x + i % room.w;
            const std::int32_t cy = room.pos_y + i / room.w;
            if (first) {
                biome.min_cx = biome.max_cx = cx;
                biome.min_cy = biome.max_cy = cy;
                biome.chunk_w = room.chunk_w;
                biome.chunk_h = room.chunk_h;
                first = false;
            } else {
                biome.min_cx = std::min(biome.min_cx, cx);
                biome.max_cx = std::max(biome.max_cx, cx);
                biome.min_cy = std::min(biome.min_cy, cy);
                biome.max_cy = std::max(biome.max_cy, cy);
            }
        }
        biome.rooms.push_back(std::move(room));
    }
    if (biome.rooms.empty()) {
        SetError(error, "no readable rooms");
        return std::nullopt;
    }
    return biome;
}

std::shared_ptr<const World> LoadWorld(const ReadFile& read, std::string* log) {
    return LoadWorld(read, PinnedBiomeBundles(), log);
}

std::vector<std::string> BiomeBundlesFromCatalog(std::string_view catalog) {
    std::vector<std::string> out;
    constexpr std::string_view kPrefix = "data-world_biome_";
    constexpr std::string_view kSuffix = ".bundle";
    std::size_t at = 0;
    while ((at = catalog.find(kPrefix, at)) != std::string_view::npos) {
        std::size_t end = at;
        while (end < catalog.size() && end - at < 200) {
            const char c = catalog[end];
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.'))
                break;
            ++end;
        }
        std::string_view name = catalog.substr(at, end - at);
        if (name.size() > kSuffix.size() &&
            name.substr(name.size() - kSuffix.size()) == kSuffix &&
            std::find(out.begin(), out.end(), name) == out.end())
            out.emplace_back(name);
        at = end;
    }
    return out;
}

std::shared_ptr<const World> LoadWorld(const ReadFile& read, const std::vector<std::string>& names,
                                       std::string* log) {
    auto world = std::make_shared<World>();
    for (const auto& name : names) {
        auto bytes = read(std::string(kBundleDir) + name);
        if (!bytes) {
            if (log)
                *log += "missing " + name + "\n";
            continue;
        }
        std::string err;
        auto biome = LoadBiome(*bytes, name, &err);
        if (!biome) {
            if (log)
                *log += name + ": " + err + "\n";
            continue;
        }
        world->biomes.push_back(std::move(*biome));
    }
    return world;
}

} // namespace bc_world
