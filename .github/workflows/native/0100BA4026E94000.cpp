// SPDX-License-Identifier: GPL-3.0-or-later
//
// Castlevania: Belmont's Curse (0100BA4026E94000), game version 1.1.0, build 0F1169AA7D0FA452.
// Eden Duo second-screen map companion, first test version.
//
//   * Map: the castle's rooms are read at runtime from the player's own romfs (bc_world) and
//     drawn by the module (bc_render) as `module:bc:map:...` images. Nothing from the game is
//     shipped in the package.
//   * Live: the current room, chunk and chunk exploration are read from guest memory (bc_live)
//     and published as plain values; the debug page shows every step so the chain can be
//     checked on a device.
//
// Outputs (all published every sample):
//   bc.ver, bc.stage, bc.status, bc.delta, bc.sf, bc.nsys, bc.systems
//   bc.chunk.x/y, bc.prev.x/y, bc.chunk.mode (1 room-local, 2 world, 0 unknown)
//   bc.room.ok, bc.room.uid, bc.room.x/y, bc.room.world, bc.room.id, bc.room.inst, bc.ex.uid
//   bc.mm.x/y/z (ExplorerComponent.lastMainMapPosition, x100)
//   bc.fog.ok, bc.fog.n, bc.fog.matched, bc.fog.cur, bc.fog.keys
//   bc.world.ready, bc.world.rooms, bc.world.biomes, bc.world.log
//   bc.map.key, bc.map.ready, bc.biome.label, bc.zoom, bc.unexp, bc.follow
//   bc.mk.show, bc.mk.x, bc.mk.y (marker widget offsets, canvas px)
//   bc.us (average sample cost in microseconds)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "bc_live.h"
#include "bc_render.h"
#include "bc_world.h"
#include "core/mods/modules/dsmod_module_sdk.h"

namespace {

using namespace dsmod_sdk::int_types;

constexpr u64 kTitleId = 0x0100BA4026E94000ULL;
constexpr char kBuildHex[] = "0F1169AA7D0FA4529C861F96ADEC306E"
                             "00000000000000000000000000000000";
constexpr const char* kVersion = "0.1.0";

// Map image size; manifest.json places it at (0, 96) on the 1240 x 1080 canvas.
constexpr int kMapW = 1240;
constexpr int kMapH = 888;
constexpr int kMarkerHalf = 16;

constexpr const char* kCatalog = "Data/StreamingAssets/aa/catalog.json";

std::optional<std::vector<u8>> ReadRomfs(const EdenDsmodHostApi& host, const std::string& path) {
    if (!host.read_romfs)
        return std::nullopt;
    for (const std::string& p : {path, "/" + path}) {
        const std::size_t size = host.read_romfs(host.userdata, p.c_str(), 0, nullptr, 0);
        if (size == 0 || size > (64u << 20))
            continue;
        std::vector<u8> out(size);
        if (host.read_romfs(host.userdata, p.c_str(), 0, out.data(), size) == size)
            return out;
    }
    return std::nullopt;
}

std::string BiomeLabel(const std::string& world_name) {
    std::string s = world_name;
    const std::string prefix = "World_Biome_";
    if (s.rfind(prefix, 0) == 0)
        s = s.substr(prefix.size());
    for (char& c : s) {
        if (c == '_')
            c = ' ';
    }
    return s;
}

struct Instance {
    EdenDsmodHostApi host{};

    // World model, built once on a background thread.
    std::mutex world_mutex;
    std::condition_variable world_cv;
    std::shared_ptr<const bc_world::World> world;
    std::string world_log;
    bool world_done = false;
    std::thread loader;

    // Fog snapshots referenced by published image keys (load_image runs on the asset worker).
    std::mutex fog_mutex;
    std::deque<std::pair<u64, std::shared_ptr<const bc_render::Fog>>> fogs;

    // Tick-thread state.
    bc_live::Reader reader;
    bc_live::State live;
    u64 samples = 0;
    int zoom = 0;
    bool show_unexplored = false;
    int manual_biome = -1; // >= 0: browsing a biome by hand instead of following the player
    std::shared_ptr<const bc_render::Fog> fog = std::make_shared<bc_render::Fog>();
    u64 fog_id = 0;
    double cost_us = 0;

    void StartLoader() {
        loader = std::thread([this] {
            std::string log;
            std::shared_ptr<const bc_world::World> w;
            try {
                const bc_world::ReadFile read = [this](const std::string& path) {
                    return ReadRomfs(host, path);
                };
                w = bc_world::LoadWorld(read, &log);
                if (w->biomes.empty()) {
                    // Pinned names missing (another build?): look the names up in the catalog.
                    log += "pinned biome bundles not found; scanning the catalog\n";
                    if (auto cat = ReadRomfs(host, kCatalog)) {
                        const std::string_view text(reinterpret_cast<const char*>(cat->data()),
                                                    cat->size());
                        const auto names = bc_world::BiomeBundlesFromCatalog(text);
                        log += std::to_string(names.size()) + " names in the catalog\n";
                        w = bc_world::LoadWorld(read, names, &log);
                    } else {
                        log += "catalog.json not readable\n";
                    }
                }
            } catch (const std::exception& e) {
                log += std::string("world load failed: ") + e.what() + "\n";
                w.reset();
            } catch (...) {
                log += "world load failed\n";
                w.reset();
            }
            {
                std::lock_guard lock(world_mutex);
                world = std::move(w);
                world_log = std::move(log);
                world_done = true;
            }
            world_cv.notify_all();
        });
    }

    std::shared_ptr<const bc_world::World> World() {
        std::lock_guard lock(world_mutex);
        return world;
    }

    void RememberFog(u64 id, std::shared_ptr<const bc_render::Fog> f) {
        std::lock_guard lock(fog_mutex);
        for (const auto& [k, v] : fogs) {
            if (k == id)
                return;
        }
        fogs.emplace_back(id, std::move(f));
        while (fogs.size() > 8)
            fogs.pop_front();
    }

    std::shared_ptr<const bc_render::Fog> FindFog(u64 id) {
        std::lock_guard lock(fog_mutex);
        for (const auto& [k, v] : fogs) {
            if (k == id)
                return v;
        }
        return nullptr;
    }
};

void PublishI(const EdenDsmodHostApi* h, const char* name, s64 v) {
    if (h->publish_i64)
        h->publish_i64(h->userdata, name, v);
}
void PublishT(const EdenDsmodHostApi* h, const char* name, const std::string& v) {
    if (h->publish_text)
        h->publish_text(h->userdata, name, v.c_str());
}

std::string Hex(s64 v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s0x%llX", v < 0 ? "-" : "",
                  static_cast<unsigned long long>(v < 0 ? -v : v));
    return buf;
}

EdenDsmodBool SupportsBuild(const char* build_hex) {
    if (!build_hex)
        return EDEN_DSMOD_FALSE;
    const std::string b = build_hex;
    // The loader passes the running build in upper-case hex; accept the full id, or the id with
    // its zero tail trimmed (never shorter than 16 characters).
    const std::string full = kBuildHex;
    if (b.size() < 16 || b.size() > full.size())
        return EDEN_DSMOD_FALSE;
    return full.compare(0, b.size(), b) == 0 &&
                   full.find_first_not_of('0', std::max<std::size_t>(b.size(), 32)) ==
                       std::string::npos
               ? EDEN_DSMOD_TRUE
               : EDEN_DSMOD_FALSE;
}

void* Create(const EdenDsmodHostApi* host, const char*) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != kTitleId)
            return nullptr;
        char hex[65];
        for (int i = 0; i < 32; ++i)
            std::snprintf(hex + i * 2, 3, "%02X", host->build_id[i]);
        if (std::strncmp(hex, kBuildHex, 64) != 0)
            return nullptr;
        auto* inst = new Instance();
        inst->host = *host;
        inst->StartLoader();
        if (host->log)
            host->log(host->userdata, EDEN_DSMOD_LOG_INFO, "BelmontsCurse map: module created");
        return inst;
    } catch (...) {
        return nullptr;
    }
}

void Destroy(void* p) {
    auto* inst = static_cast<Instance*>(p);
    if (!inst)
        return;
    try {
        if (inst->loader.joinable())
            inst->loader.join();
    } catch (...) {
    }
    delete inst;
}

std::shared_ptr<bc_render::Fog> BuildFog(const bc_live::State& s) {
    auto f = std::make_shared<bc_render::Fog>();
    if (!s.fog_ok || s.fog_matched == 0)
        return f; // unknown: the map shows every room
    f->known = true;
    for (const auto& r : s.fog) {
        if (r.uid >= 0)
            f->by_uid[r.uid] = r.states;
    }
    return f;
}

void Sample(void* p, const EdenDsmodHostApi* h) {
    auto* inst = static_cast<Instance*>(p);
    if (!inst || !h)
        return;
    try {
        const auto t0 = std::chrono::steady_clock::now();
        ++inst->samples;
        bc_live::Memory mem;
        mem.main_base = h->main_base;
        mem.main_size = h->main_size;
        mem.read = [h](u64 at, void* out, std::size_t n) {
            return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::AssumeMapped>(*h, at, out, n);
        };
        // The exploration dictionary is walked twice a second.
        const bool with_fog = inst->samples % 30 == 1;
        bc_live::State fresh;
        inst->reader.Sample(mem, with_fog, fresh);
        if (!with_fog && inst->live.fog_ok) {
            fresh.fog_ok = inst->live.fog_ok;
            fresh.fog_entries = inst->live.fog_entries;
            fresh.fog_matched = inst->live.fog_matched;
            fresh.fog = std::move(inst->live.fog);
            fresh.fog_hash = inst->live.fog_hash;
        }
        inst->live = std::move(fresh);
        const bc_live::State& s = inst->live;
        if (with_fog) {
            const u64 id = s.fog_ok && s.fog_matched ? s.fog_hash : 0;
            if (id != inst->fog_id) {
                inst->fog_id = id;
                inst->fog = BuildFog(s);
            }
            inst->RememberFog(inst->fog_id, inst->fog);
        }

        PublishT(h, "bc.ver", kVersion);
        PublishI(h, "bc.stage", s.stage);
        PublishT(h, "bc.status", s.status);
        PublishT(h, "bc.delta", Hex(s.delta));
        PublishI(h, "bc.sf", s.static_fields_offset);
        PublishI(h, "bc.nsys", s.system_count);
        PublishT(h, "bc.systems", s.system_names);
        PublishI(h, "bc.rm.ok", s.rm_ok);
        PublishI(h, "bc.chunk.x", s.chunk_x);
        PublishI(h, "bc.chunk.y", s.chunk_y);
        PublishI(h, "bc.prev.x", s.prev_x);
        PublishI(h, "bc.prev.y", s.prev_y);
        PublishI(h, "bc.room.ok", s.room_ok);
        PublishI(h, "bc.room.uid", s.room_uid);
        PublishI(h, "bc.room.x", s.room_x);
        PublishI(h, "bc.room.y", s.room_y);
        PublishT(h, "bc.room.world", s.world_name);
        PublishT(h, "bc.room.id", s.room_id);
        PublishT(h, "bc.room.inst", s.room_instance);
        PublishI(h, "bc.ex.uid", s.ex_room_uid);
        PublishI(h, "bc.mm.x", static_cast<s64>(s.map_x * 100));
        PublishI(h, "bc.mm.y", static_cast<s64>(s.map_y * 100));
        PublishI(h, "bc.mm.z", static_cast<s64>(s.map_z * 100));
        PublishI(h, "bc.fog.ok", s.fog_ok);
        PublishI(h, "bc.fog.n", s.fog_entries);
        PublishI(h, "bc.fog.matched", s.fog_matched);
        {
            std::string keys, cur;
            int shown = 0;
            for (const auto& r : s.fog) {
                if (shown < 3) {
                    keys += (shown ? " | " : "") + r.key;
                    ++shown;
                }
                if (r.uid == s.room_uid && s.room_uid >= 0) {
                    cur = std::to_string(r.w) + "x" + std::to_string(r.h) + ":";
                    for (const u8 st : r.states)
                        cur += static_cast<char>('0' + std::min<int>(st, 9));
                }
            }
            PublishT(h, "bc.fog.keys", keys);
            PublishT(h, "bc.fog.cur", cur);
        }

        // World model status.
        const auto world = inst->World();
        std::string world_log;
        bool world_done = false;
        {
            std::lock_guard lock(inst->world_mutex);
            world_log = inst->world_log;
            world_done = inst->world_done;
        }
        PublishI(h, "bc.world.ready", world && !world->biomes.empty());
        PublishI(h, "bc.world.done", world_done);
        PublishI(h, "bc.world.biomes", world ? static_cast<s64>(world->biomes.size()) : 0);
        PublishI(h, "bc.world.rooms", world ? static_cast<s64>(world->RoomCount()) : 0);
        PublishT(h, "bc.world.log", world_log);
        PublishI(h, "bc.zoom", inst->zoom);
        PublishI(h, "bc.unexp", inst->show_unexplored);
        PublishI(h, "bc.follow", inst->manual_biome < 0);

        // Which biome to draw, and where the player is on it.
        int biome_index = -1;
        int mode = 0;
        int pcx = 0, pcy = 0;
        bool have_player = false;
        if (world && !world->biomes.empty()) {
            if (inst->manual_biome >= 0) {
                biome_index = inst->manual_biome % static_cast<int>(world->biomes.size());
            } else if (s.room_ok) {
                for (std::size_t i = 0; i < world->biomes.size(); ++i) {
                    if (world->biomes[i].world_name == s.world_name)
                        biome_index = static_cast<int>(i);
                }
            }
            if (biome_index >= 0 && s.room_ok && s.rm_ok) {
                const auto& b = world->biomes[static_cast<std::size_t>(biome_index)];
                if (b.world_name == s.world_name) {
                    const bc_world::Room* room = b.FindUid(s.room_uid);
                    if (room && s.chunk_x >= 0 && s.chunk_y >= 0 && s.chunk_x < room->w &&
                        s.chunk_y < room->h) {
                        mode = 1; // room-local chunk
                        pcx = room->pos_x + s.chunk_x;
                        pcy = room->pos_y + s.chunk_y;
                        have_player = true;
                    } else if (s.chunk_x >= b.min_cx && s.chunk_x <= b.max_cx &&
                               s.chunk_y >= b.min_cy && s.chunk_y <= b.max_cy) {
                        mode = 2; // already a biome chunk
                        pcx = s.chunk_x;
                        pcy = s.chunk_y;
                        have_player = true;
                    } else if (room) {
                        pcx = room->pos_x + room->w / 2;
                        pcy = room->pos_y + room->h / 2;
                    }
                }
            }
        }
        PublishI(h, "bc.chunk.mode", mode);

        if (biome_index >= 0) {
            const auto& b = world->biomes[static_cast<std::size_t>(biome_index)];
            bc_render::Params prm;
            prm.zoom = inst->zoom;
            prm.out_w = kMapW;
            prm.out_h = kMapH;
            prm.show_unexplored = inst->show_unexplored;
            prm.current_uid = s.room_ok && b.world_name == s.world_name ? s.room_uid : -1;
            if (!have_player && prm.current_uid < 0) {
                pcx = (b.min_cx + b.max_cx) / 2;
                pcy = (b.min_cy + b.max_cy) / 2;
            }
            prm.center_cx = prm.zoom ? pcx : 0;
            prm.center_cy = prm.zoom ? pcy : 0;
            char key[256];
            std::snprintf(key, sizeof key, "module:bc:map:1:%d:%d:%d:%d:%d:%llx:%d", biome_index,
                          prm.zoom, prm.center_cx, prm.center_cy, prm.current_uid,
                          static_cast<unsigned long long>(inst->fog_id), prm.show_unexplored);
            inst->RememberFog(inst->fog_id, inst->fog);
            PublishT(h, "bc.map.key", key);
            PublishI(h, "bc.map.ready", 1);
            PublishT(h, "bc.biome.label", BiomeLabel(b.world_name) +
                                              (inst->manual_biome >= 0 ? "  (browse)" : ""));
            if (have_player) {
                const bc_render::View v = bc_render::ComputeView(b, prm);
                double px, py;
                bc_render::TileToPixel(v, (pcx - b.min_cx + 0.5) * b.chunk_w,
                                       (pcy - b.min_cy + 0.5) * b.chunk_h, px, py);
                const bool inside = px >= 0 && py >= 0 && px < kMapW && py < kMapH;
                PublishI(h, "bc.mk.show", inside);
                PublishI(h, "bc.mk.x", static_cast<s64>(px) - kMarkerHalf);
                PublishI(h, "bc.mk.y", static_cast<s64>(py) - kMarkerHalf);
            } else {
                PublishI(h, "bc.mk.show", 0);
            }
        } else {
            PublishI(h, "bc.map.ready", 0);
            PublishI(h, "bc.mk.show", 0);
            PublishT(h, "bc.biome.label", world_done ? "Waiting for the game..." : "Reading map...");
        }

        const auto t1 = std::chrono::steady_clock::now();
        const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        inst->cost_us = inst->cost_us * 0.95 + us * 0.05;
        PublishI(h, "bc.us", static_cast<s64>(inst->cost_us));
    } catch (...) {
        // Never let an exception cross the C ABI; the next sample starts fresh.
    }
}

void Tick(void*, const EdenDsmodHostApi*) {}

void Configure(void*, const EdenDsmodHostExtensions*) {}

EdenDsmodBool OnAction(void* p, const char* action, int64_t argument) {
    auto* inst = static_cast<Instance*>(p);
    if (!inst || !action)
        return EDEN_DSMOD_FALSE;
    try {
        const std::string a = action;
        const auto world = inst->World();
        const int biomes = world ? static_cast<int>(world->biomes.size()) : 0;
        if (a == "zoom") {
            inst->zoom = (inst->zoom + 1) % 3;
        } else if (a == "zoom_in") {
            inst->zoom = std::min(inst->zoom + 1, 2);
        } else if (a == "zoom_out") {
            inst->zoom = std::max(inst->zoom - 1, 0);
        } else if (a == "unexp") {
            inst->show_unexplored = !inst->show_unexplored;
        } else if (a == "biome_next" || a == "biome_prev") {
            if (biomes == 0)
                return EDEN_DSMOD_FALSE;
            int cur = inst->manual_biome;
            if (cur < 0) {
                cur = 0;
                for (int i = 0; i < biomes; ++i) {
                    if (world->biomes[static_cast<std::size_t>(i)].world_name ==
                        inst->live.world_name)
                        cur = i;
                }
            }
            cur += a == "biome_next" ? 1 : biomes - 1;
            inst->manual_biome = cur % biomes;
        } else if (a == "follow") {
            inst->manual_biome = -1;
        } else {
            (void)argument;
            return EDEN_DSMOD_FALSE;
        }
        return EDEN_DSMOD_TRUE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

EdenDsmodBool LoadImage(void* p, const EdenDsmodHostApi*, const char* key, void* receiver,
                        EdenDsmodImageSink sink) {
    auto* inst = static_cast<Instance*>(p);
    if (!inst || !key || !sink)
        return EDEN_DSMOD_FALSE;
    try {
        int biome = 0, zoom = 0, cx = 0, cy = 0, uid = -1, unexp = 0, version = 0;
        unsigned long long fog_id = 0;
        if (std::sscanf(key, "module:bc:map:%d:%d:%d:%d:%d:%d:%llx:%d", &version, &biome, &zoom,
                        &cx, &cy, &uid, &fog_id, &unexp) != 8 ||
            version != 1)
            return EDEN_DSMOD_FALSE;
        std::shared_ptr<const bc_world::World> world;
        {
            std::unique_lock lock(inst->world_mutex);
            inst->world_cv.wait_for(lock, std::chrono::seconds(5), [inst] { return inst->world_done; });
            world = inst->world;
        }
        if (!world || biome < 0 || biome >= static_cast<int>(world->biomes.size()))
            return EDEN_DSMOD_FALSE;
        auto fog = inst->FindFog(fog_id);
        if (!fog)
            fog = std::make_shared<bc_render::Fog>(); // unknown snapshot: draw every room
        bc_render::Params prm;
        prm.zoom = zoom;
        prm.center_cx = cx;
        prm.center_cy = cy;
        prm.current_uid = uid;
        prm.show_unexplored = unexp != 0;
        prm.out_w = kMapW;
        prm.out_h = kMapH;
        const auto pixels = bc_render::Render(world->biomes[static_cast<std::size_t>(biome)], *fog, prm);
        sink(receiver, static_cast<u32>(prm.out_w), static_cast<u32>(prm.out_h), pixels.data(),
             pixels.size());
        return EDEN_DSMOD_TRUE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

const EdenDsmodModuleApi kModule{
    EDEN_DSMOD_MODULE_ABI_VERSION,
    sizeof(EdenDsmodModuleApi),
    0,
    EDEN_DSMOD_MODULE_ABI_HASH,
    kTitleId,
    "Castlevania: Belmont's Curse map",
    EDEN_DSMOD_CAP_ROMFS_READ | EDEN_DSMOD_CAP_EXTENSIONS,
    SupportsBuild,
    Create,
    Destroy,
    Sample,
    Tick,
};

const EdenDsmodModuleExtensions kExtensions{
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions), EDEN_DSMOD_EXT_HASH,
    Configure,              OnAction,                          LoadImage,
};

} // namespace

DSMOD_SDK_EXPORT_MODULE(kModule)
DSMOD_SDK_EXPORT_EXTENSIONS(kExtensions)
