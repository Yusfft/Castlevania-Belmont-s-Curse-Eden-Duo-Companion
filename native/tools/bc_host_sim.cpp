// SPDX-License-Identifier: GPL-3.0-or-later
// End-to-end check of the built module through its C ABI, the way Eden Duo drives it:
// dlopen, eden_dsmod_get_module / eden_dsmod_get_extensions, create, sample, on_action,
// load_image, destroy. Guest memory is a FakeGuest; romfs is a directory.
//   bc_host_sim <module.so> <romfs-or-bundle-dir> <out-dir>
#include <dirent.h>
#include <sys/stat.h>
#include <dlfcn.h>

#include <array>
#include <cmath>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <thread>

#include "bc_levelmap.h"
#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "fake_guest.h"
#ifdef BC_TEST_ASTC
#include "astc_rgba.h" // test-only ASTC decoder (not part of the module)
#endif

namespace {

struct Sim {
    FakeGuest guest;
    std::string romfs;
    std::map<std::string, std::int64_t> ints;
    std::map<std::string, std::string> texts;
};
Sim* g_sim = nullptr;

std::uint64_t GetTick(void*) { return 0; }
std::uint64_t HeapBegin(void*) { return 0x20000000; }
std::uint64_t HeapEnd(void*) { return 0x40000000; }
EdenDsmodBool IsMapped(void*, std::uint64_t a, std::uint64_t n) {
    std::vector<std::uint8_t> tmp(static_cast<std::size_t>(std::min<std::uint64_t>(n, 1 << 20)));
    return n <= tmp.size() && g_sim->guest.Read(a, tmp.data(), static_cast<std::size_t>(n));
}
EdenDsmodBool ReadMem(void*, std::uint64_t a, void* out, std::size_t n) {
    return g_sim->guest.Read(a, out, n);
}
void Log(void*, std::uint32_t, const char* m) { std::fprintf(stderr, "[module] %s\n", m); }
void Nop(void*) {}
void PubI(void*, const char* n, std::int64_t v) { g_sim->ints[n] = v; }
void PubF(void*, const char* n, double v) { g_sim->ints[n] = static_cast<std::int64_t>(v); }
void PubT(void*, const char* n, const char* v) { g_sim->texts[n] = v; }
void PubA(void*, const char* n, std::uint64_t v) { g_sim->ints[n] = static_cast<std::int64_t>(v); }
std::size_t ReadRomfs(void*, const char* path, std::uint64_t off, void* out, std::size_t size) {
    std::string p = path;
    if (!p.empty() && p[0] == '/')
        return 0; // the module must find the files with the plain relative path
    std::ifstream in(g_sim->romfs + "/" + p, std::ios::binary);
    if (!in) {
        const auto slash = p.rfind('/');
        in.open(g_sim->romfs + "/" + p.substr(slash + 1), std::ios::binary);
        if (!in)
            return 0;
    }
    in.seekg(0, std::ios::end);
    const std::size_t total = static_cast<std::size_t>(in.tellg());
    if (!out)
        return total;
    if (off > total)
        return 0;
    const std::size_t n = std::min<std::size_t>(size, total - off);
    in.seekg(static_cast<std::streamoff>(off));
    in.read(static_cast<char*>(out), static_cast<std::streamsize>(n));
    return n;
}
std::int64_t GetI(void*, const char*, std::int64_t fb) { return fb; }
EdenDsmodBool DecodeAstc(void*, std::uint32_t w, std::uint32_t h, std::uint32_t bw, std::uint32_t bh,
                         const std::uint8_t* in, std::size_t in_size, std::uint8_t* out, std::size_t out_size) {
#ifdef BC_TEST_ASTC
    // The same limits as Eden Duo's decoder (mod_module_services.cpp): at most 2048 x 2048, the
    // input exactly the block count, the output exactly w * h * 4.
    if (w == 0 || h == 0 || w > 2048 || h > 2048 ||
        in_size != static_cast<std::size_t>((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16 ||
        out_size != static_cast<std::size_t>(w) * h * 4)
        return EDEN_DSMOD_FALSE;
    return TestAstcDecode(in, in_size, w, h, bw, bh, out) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
#else
    (void)w, (void)h, (void)bw, (void)bh, (void)in, (void)in_size, (void)out, (void)out_size;
    return EDEN_DSMOD_FALSE;
#endif
}
double GetF(void*, const char*, double fb) { return fb; }
const char* GetT(void*, const char*) { return nullptr; }

// The guest mailbox and a stand-in for the guest helper (native/helper/bc_guest_helper.S; the
// real one is tested by tools/test_guest_helper.py): once a game frame it counts, and serves a
// new request by doing to the fake guest what the game's method would.
struct FakeHelper {
    std::array<std::uint8_t, 4096> mb{};
    std::vector<std::string> log;
    template <typename T>
    T Get(std::uint32_t o) const {
        T v{};
        std::memcpy(&v, mb.data() + o, sizeof v);
        return v;
    }
    template <typename T>
    void Put(std::uint32_t o, T v) {
        std::memcpy(mb.data() + o, &v, sizeof v);
    }
    void Frame(FakeGuest& g) {
        Put<std::uint32_t>(4, Get<std::uint32_t>(4) + 1);
        const auto req = Get<std::uint32_t>(8);
        if (Get<std::uint32_t>(0) != 0x31484342 || req == Get<std::uint32_t>(0x60))
            return;
        const auto cmd = Get<std::uint32_t>(0xC);
        const auto i = Get<std::int32_t>(0x14);
        const auto o1 = Get<std::uint64_t>(0x28), o2 = Get<std::uint64_t>(0x30), o3 = Get<std::uint64_t>(0x38);
        std::uint32_t result = 0;
        std::int64_t value = 0;
        const auto relic_slot = [&](std::uint64_t put) {
            const std::uint64_t arr = g.Get<std::uint64_t>(g.inv_ + 0x40);
            if (i < 0 || static_cast<std::uint64_t>(i) >= g.Get<std::uint64_t>(arr + 0x18)) {
                result = 6;
                return;
            }
            g.Put<std::uint64_t>(arr + 0x20 + 8 * static_cast<std::uint64_t>(i), put);
        };
        if (Get<std::uint32_t>(0x10) != Get<std::uint32_t>(0xFFC))
            result = 3;
        else if (cmd == 1)
            g.Put<std::uint64_t>(g.inv_ + 0x28, o1);
        else if (cmd == 2)
            g.Put<std::uint64_t>(g.inv_ + 0x38, o1);
        else if (cmd == 3)
            relic_slot(o1);
        else if (cmd == 4)
            relic_slot(0);
        else if (cmd == 5)
            value = 99;
        else if (cmd != 6 && cmd != 7)
            result = 2;
        char line[256];
        std::snprintf(line, sizeof line, "cmd %u i %d x %.1f y %.1f o1 %llx o2 %llx o3 %llx -> %u", cmd, i,
                      static_cast<double>(Get<float>(0x18)), static_cast<double>(Get<float>(0x1C)),
                      static_cast<unsigned long long>(o1), static_cast<unsigned long long>(o2),
                      static_cast<unsigned long long>(o3), result);
        log.push_back(line);
        Put<std::uint32_t>(0x64, result);
        Put<std::int64_t>(0x68, value);
        Put<std::uint32_t>(0x60, req);
    }
};
FakeHelper* g_helper = nullptr;
template <typename T>
EdenDsmodBool MbLoad(void*, std::uint32_t o, T* v) {
    if (o % sizeof(T) != 0 || o > 4096 - sizeof(T))
        return EDEN_DSMOD_FALSE;
    *v = g_helper->Get<T>(o);
    return EDEN_DSMOD_TRUE;
}
template <typename T>
EdenDsmodBool MbStore(void*, std::uint32_t o, T v) {
    if (o % sizeof(T) != 0 || o > 4096 - sizeof(T))
        return EDEN_DSMOD_FALSE;
    g_helper->Put<T>(o, v);
    return EDEN_DSMOD_TRUE;
}

struct Png {
    std::string path;
};
void Sink(void* receiver, std::uint32_t w, std::uint32_t h, const std::uint8_t* px, std::size_t size) {
    auto* out = static_cast<Png*>(receiver);
    if (out->path.empty())
        return; // drawn, not kept (the host asking for the next map window)
    std::FILE* f = std::fopen(out->path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", out->path.c_str());
        return;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (std::size_t i = 0; i + 3 < size; i += 4) {
        const int a = px[i + 3];
        const std::uint8_t c[3] = {static_cast<std::uint8_t>((px[i] * a + 12 * (255 - a)) / 255),
                                   static_cast<std::uint8_t>((px[i + 1] * a + 10 * (255 - a)) / 255),
                                   static_cast<std::uint8_t>((px[i + 2] * a + 16 * (255 - a)) / 255)};
        std::fwrite(c, 1, 3, f);
    }
    std::fclose(f);
    // The same with alpha (u32 w, u32 h, RGBA), for page previews.
    if (std::FILE* r = std::fopen((out->path + ".rgba").c_str(), "wb")) {
        std::fwrite(&w, 4, 1, r);
        std::fwrite(&h, 4, 1, r);
        std::fwrite(px, 1, size, r);
        std::fclose(r);
    }
    std::fprintf(stderr, "image %ux%u -> %s\n", w, h, out->path.c_str());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <module.so> <romfs-dir> <out-dir>\n", argv[0]);
        return 2;
    }
    Sim sim;
    g_sim = &sim;
    FakeHelper helper;
    g_helper = &helper;
    helper.Put<std::uint32_t>(0xFFC, 1); // Eden's epoch after its first Run
    sim.romfs = argv[2];
    const std::string out = argv[3];
    ::mkdir(out.c_str(), 0755);
    // Streets Poor room uid 59 at (-7, 15), 2x2 chunks, chunk 0 disabled; player in chunk (1,0).
    // Room 59 (StreetsPoor_Room21): positionCoord (-7, 15) + editionOffset (0, -1) = chunk origin
    // (-7, 14). The player stands in its chunk (1, 0): world x = -6 * 24 + 10, y = 14 * 13 + 4.
    sim.guest.Build(0x4000, 59, -7, 15, 1, 0, "World_Biome_01_Streets_Poor", {0, 3, 3, 1}, 2, 2,
                    -134.0f, 186.0f);

    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    auto get_module = reinterpret_cast<EdenDsmodGetModuleFn>(dlsym(lib, "eden_dsmod_get_module"));
    auto get_ext = reinterpret_cast<EdenDsmodGetExtensionsFn>(dlsym(lib, "eden_dsmod_get_extensions"));
    if (!get_module || !get_ext) {
        std::fprintf(stderr, "missing exports\n");
        return 1;
    }
    const EdenDsmodModuleApi* api = get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    const EdenDsmodModuleExtensions* ext = get_ext(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    if (!api || api->struct_size != sizeof(EdenDsmodModuleApi) || !ext ||
        ext->struct_size != sizeof(EdenDsmodModuleExtensions) || !ext->configure || !ext->on_action ||
        !ext->load_image) {
        std::fprintf(stderr, "bad tables\n");
        return 1;
    }
    std::printf("module '%s' title %016llX caps 0x%llx\n", api->name,
                static_cast<unsigned long long>(api->title_id),
                static_cast<unsigned long long>(api->capabilities));
    std::printf("supports full build: %u, BUILD16: %u, other: %u\n",
                api->supports_build("0F1169AA7D0FA4529C861F96ADEC306E00000000000000000000000000000000"),
                api->supports_build("0F1169AA7D0FA452"),
                api->supports_build("D4B150B29A931CD381E2A30318FC299F2B0EE36D000000000000000000000000"));

    EdenDsmodHostApi host{};
    host.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    host.struct_size = sizeof(host);
    host.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    host.capabilities = EDEN_DSMOD_CAP_ROMFS_READ | EDEN_DSMOD_CAP_MAP_OUTPUT |
                        EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_WRITE_MEMORY;
    host.title_id = 0x0100BA4026E94000ULL;
    const std::uint8_t build[16] = {0x0F, 0x11, 0x69, 0xAA, 0x7D, 0x0F, 0xA4, 0x52,
                                    0x9C, 0x86, 0x1F, 0x96, 0xAD, 0xEC, 0x30, 0x6E};
    std::memcpy(host.build_id, build, 16);
    host.main_base = sim.guest.main_base;
    host.main_size = sim.guest.main_size;
    host.get_tick = GetTick;
    host.get_heap_begin = HeapBegin;
    host.get_heap_end = HeapEnd;
    host.is_mapped = IsMapped;
    host.read_memory = ReadMem;
    host.log = Log;
    host.begin_output = Nop;
    host.end_output = Nop;
    host.publish_i64 = PubI;
    host.publish_f64 = PubF;
    host.publish_text = PubT;
    host.publish_address = PubA;
    host.read_romfs = ReadRomfs;
    host.get_i64 = GetI;
    host.get_f64 = GetF;
    host.get_text = GetT;

    if ((api->capabilities & ~host.capabilities) != 0) {
        std::fprintf(stderr, "module wants capabilities the host lacks\n");
        return 1;
    }
    void* inst = api->create(&host, "{}");
    if (!inst) {
        std::fprintf(stderr, "create failed\n");
        return 1;
    }
    EdenDsmodHostExtensions hx{};
    hx.version = EDEN_DSMOD_EXT_VERSION;
    hx.struct_size = sizeof(hx);
    hx.abi_hash = EDEN_DSMOD_EXT_HASH;
    hx.decode_astc = DecodeAstc;
    hx.mailbox_address = 0x7F000000;
    hx.mailbox_size = 4096;
    hx.load_u32 = MbLoad<std::uint32_t>;
    hx.load_u64 = MbLoad<std::uint64_t>;
    hx.store_u32 = MbStore<std::uint32_t>;
    hx.store_u64 = MbStore<std::uint64_t>;
    ext->configure(inst, &hx);
    // Explored zones: every zone of the rooms near the start and near (40, 40), from the same
    // level map the module reads (the sim plays the game's part here).
    {
        std::vector<std::string> names;
        const std::string dir = sim.romfs + "/Data/StreamingAssets/aa/Switch";
        if (DIR* d = opendir(dir.c_str())) {
            while (auto* e = readdir(d))
                if (e->d_name[0] != '.')
                    names.push_back(e->d_name);
            closedir(d);
        }
        bc_levelmap::Assets assets(
            [](const std::string& p, std::uint64_t off, void* out, std::size_t n) { return ReadRomfs(nullptr, p.c_str(), off, out, n); },
            names);
        bc_levelmap::Codecs codecs;
        codecs.astc = [](const std::uint8_t* in, std::size_t in_size, std::uint32_t w, std::uint32_t h, std::uint32_t bw,
                         std::uint32_t bh, std::uint8_t* px) {
            return DecodeAstc(nullptr, w, h, bw, bh, in, in_size, px, static_cast<std::size_t>(w) * h * 4) != 0;
        };
        bc_levelmap::Names nm;
        if (auto lm = bc_levelmap::LoadLevelMap(assets, codecs, nm, "World_Biome_01_Streets_Poor")) {
            std::map<std::string, std::vector<std::int32_t>> rooms;
            for (const auto& z : lm->zones) {
                for (const auto& sh : z.shapes) {
                    const float cx = (sh.x0 + sh.x1) / 2, cy = (sh.y0 + sh.y1) / 2;
                    if (std::hypot(cx + 134.0f, cy - 186.0f) < 60.0f || std::hypot(cx - 40.0f, cy - 40.0f) < 90.0f) {
                        rooms[z.room_id].push_back(z.zone_id);
                        break;
                    }
                }
            }
            for (const auto& [k, zs] : rooms)
                sim.guest.AddRoom(k, zs, {27, 29});
            sim.guest.SetLastSave("World_Biome_01_Streets_Poor_StreetsPoor_SaveRoom02_uid78");
            // Two markers (cross, chest) and a death spot near (40, 40).
            sim.guest.SetMapState(lm->level_guid,
                                  {{"3fbe385e6658a484daabb64abc8b5d03", 52.0f, 46.0f},
                                   {"f4bef205f62005a41822af84291076f0", 28.0f, 34.0f}},
                                  true, 46.0f, 30.0f);
            if (const auto common = bc_levelmap::LoadCommon(assets, codecs)) {
                std::vector<std::string> guids;
                for (const auto& [g, sprite] : common->marker_types)
                    guids.push_back(g);
                sim.guest.SetMarkerTypes(guids);
                std::printf("sim: %zu marker types\n", guids.size());
            }
            std::printf("sim: level %s\n", lm->level_guid.c_str());
            std::printf("sim: %zu explored rooms\n", rooms.size());
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1500; ++i) {
        sim.ints.clear();
        sim.texts.clear();
        api->sample(inst, &host);
        helper.Frame(sim.guest);
        api->tick(inst, &host);
        if (sim.ints["bc.world.ready"] && sim.ints["bc.lm.state"] >= 2 && i > 40)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::printf("after %.0f ms:\n",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    for (const auto& [k, v] : sim.ints)
        std::printf("  %-18s %lld\n", k.c_str(), static_cast<long long>(v));
    for (const auto& [k, v] : sim.texts)
        std::printf("  %-18s '%s'\n", k.c_str(), v.substr(0, k == "bc.world.log" ? 4000 : 120).c_str());

    // Like the host: the next map window the page asks for is drawn (then the page shows it).
    int windows_drawn = 0;
    const auto sample_n = [&](int n) {
        for (int i = 0; i < n; ++i) {
            sim.ints.clear();
            sim.texts.clear();
            api->sample(inst, &host);
            helper.Frame(sim.guest);
            if (sim.ints["bc.map.reqshow"] && !sim.texts["bc.map.req"].empty()) {
                Png none;
                ext->load_image(inst, &host, sim.texts["bc.map.req"].c_str(), &none, Sink);
                ++windows_drawn;
            }
        }
    };
    // Samples at about the game's pace (a jump in position is taken once it has held 0.3 s).
    const auto sample_paced = [&](int n) {
        for (int i = 0; i < n; ++i) {
            sample_n(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    };
    const auto save = [&](const std::string& key, const std::string& name) {
        Png png{out + "/" + name + ".ppm"};
        const auto ok = ext->load_image(inst, &host, key.c_str(), &png, Sink);
        std::printf("load_image(%s) = %u\n", key.c_str(), ok);
    };
    std::printf("pos.mode=%lld follow=%lld key=%s ox=%lld oy=%lld\n",
                static_cast<long long>(sim.ints["bc.pos.mode"]),
                static_cast<long long>(sim.ints["bc.map.follow"]), sim.texts["bc.map.key"].c_str(),
                static_cast<long long>(sim.ints["bc.map.ox"]), static_cast<long long>(sim.ints["bc.map.oy"]));
    save(sim.texts["bc.map.key"], "sim_follow");
    save("module:bc:icon:player:48", "sim_icon");
    // Walk right 40 tiles: the map slides every sample, the window is re-rendered once.
    std::string last_key = sim.texts["bc.map.key"];
    int key_changes = 0;
    for (int step = 1; step <= 40; ++step) {
        sim.guest.Put<float>(sim.guest.explorer_ + 0x90, -134.0f + static_cast<float>(step));
        sample_n(1);
        if (sim.texts["bc.map.key"] != last_key) {
            ++key_changes;
            std::printf("  step %d: new window %s (prev shown %lld)\n", step,
                        sim.texts["bc.map.key"].c_str(), static_cast<long long>(sim.ints["bc.map.pshow"]));
            last_key = sim.texts["bc.map.key"];
        }
        if (step % 10 == 0)
            std::printf("  step %d: ox=%lld oy=%lld pox=%lld\n", step,
                        static_cast<long long>(sim.ints["bc.map.ox"]), static_cast<long long>(sim.ints["bc.map.oy"]),
                        static_cast<long long>(sim.ints["bc.map.pox"]));
    }
    std::printf("window changes while walking 40 tiles: %d\n", key_changes);
    // A one-off bad position (two samples far away) must not move the view; one that holds must.
    {
        const auto mk = [&] { return std::to_string(sim.ints["bc.map.ox"]) + "," + std::to_string(sim.ints["bc.map.oy"]); };
        const std::string before = mk();
        sim.guest.Put<float>(sim.guest.explorer_ + 0x90, 300.0f);
        sample_n(2);
        const std::string during = mk();
        sim.guest.Put<float>(sim.guest.explorer_ + 0x90, -94.0f);
        sample_n(2);
        std::printf("glitch: before %s during %s after %s glitches %lld\n", before.c_str(), during.c_str(), mk().c_str(),
                    static_cast<long long>(sim.ints["bc.pos.glitch"]));
        sim.guest.Put<float>(sim.guest.explorer_ + 0x90, -60.0f);
        sample_paced(30);
        std::printf("teleport held 0.5 s: %s key %s\n", mk().c_str(), sim.texts["bc.map.key"].c_str());
        sim.guest.Put<float>(sim.guest.explorer_ + 0x90, -94.0f);
        sample_paced(30);
    }
    ext->on_action(inst, "zoom", 0); // close -> near
    sample_n(2);
    std::printf("near: key=%s zoom=%lld\n", sim.texts["bc.map.key"].c_str(), static_cast<long long>(sim.ints["bc.zoom"]));
    save(sim.texts["bc.map.key"], "sim_near");
    ext->on_action(inst, "zoom", 0); // near -> far
    sample_n(2);
    std::printf("far: key=%s zoom=%lld mk=%lld,%lld\n", sim.texts["bc.map.key"].c_str(),
                static_cast<long long>(sim.ints["bc.zoom"]), static_cast<long long>(sim.ints["bc.mk.dx"]),
                static_cast<long long>(sim.ints["bc.mk.dy"]));
    save(sim.texts["bc.map.key"], "sim_far");
    std::printf("title '%s' key %s img %lld\n", sim.texts["bc.title"].c_str(), sim.texts["bc.title.key"].c_str(),
                static_cast<long long>(sim.ints["bc.title.img"]));
    save(sim.texts["bc.title.key"], "sim_title");
    save("module:bc:player:70", "sim_player70");
    save("module:bc:ui:glow:0", "sim_ui_glow0");
    save("module:bc:ui:glow:1", "sim_ui_glow1");
    for (const char* k : {"frame", "zoom:0", "zoom:2", "btn:debug", "btn:map", "wait", "debug", "menuart", "prompt", "version"})
        save(std::string("module:bc:ui:") + k, std::string("sim_ui_") + (k[0] == 'z' ? "zoom" + std::string(k + 5) : k[0] == 'b' ? std::string(k + 4) : std::string(k)));
    std::printf("zoom_set 1 accepted: %u\n", ext->on_action(inst, "zoom_set", 1));
    sample_n(2);
    std::printf("ui zoom key %s ok %lld\n", sim.texts["bc.ui.zoom"].c_str(), static_cast<long long>(sim.ints["bc.ui.ok"]));
    ext->on_action(inst, "zoom_set", 0);
    // Into the main map, near (40, 40): close view.
    ext->on_action(inst, "zoom", 0); // far -> close
    sim.guest.Put<float>(sim.guest.explorer_ + 0x90, 40.0f);
    sim.guest.Put<float>(sim.guest.explorer_ + 0x94, 40.0f);
    sample_n(32); // the exploration walk runs twice a second
    std::printf("inv ok=%lld level='%s' eq='%s' relics='%s' w='%s' stats='%s'\n",
                static_cast<long long>(sim.ints["bc.inv.ok"]), sim.texts["bc.inv.level"].c_str(),
                sim.texts["bc.inv.eq"].c_str(), sim.texts["bc.inv.relics"].c_str(), sim.texts["bc.inv.t1"].c_str(),
                sim.texts["bc.inv.stats"].c_str());
    std::printf("markers ok=%lld n=%lld death='%s' level='%s'\n", static_cast<long long>(sim.ints["bc.markers.ok"]),
                static_cast<long long>(sim.ints["bc.markers.n"]), sim.texts["bc.death"].c_str(),
                sim.texts["bc.level"].c_str());
    std::printf("main: key=%s title '%s' mk.dy=%lld lm.state=%lld\n", sim.texts["bc.map.key"].c_str(),
                sim.texts["bc.title"].c_str(), static_cast<long long>(sim.ints["bc.mk.dy"]),
                static_cast<long long>(sim.ints["bc.lm.state"]));
    save(sim.texts["bc.map.key"], "sim_main_close");
    // The equipment page: the game's menu, read in the background once the map is up, then
    // taps on it (positions as the menu lays out at 1240 x 1080) and changes through the (fake)
    // guest helper.
    const auto helper_log = [&](const char* what) {
        sample_n(3);
        std::printf("%s -> %s | msg %s show %lld | helper %s\n", what,
                    helper.log.empty() ? "(no request)" : helper.log.back().c_str(), sim.texts["bc.eq.msg"].c_str(),
                    static_cast<long long>(sim.ints["bc.eq.msgshow"]), sim.texts["bc.savepins"].c_str());
        helper.log.clear();
    };
    {
        const auto t_menu = std::chrono::steady_clock::now();
        for (int i = 0; i < 600 && !sim.ints["bc.menu.ok"]; ++i) {
            sample_n(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::printf("menu ok=%lld wait=%lld none=%lld fail=%lld after %.0f ms, key %s\n",
                    static_cast<long long>(sim.ints["bc.menu.ok"]), static_cast<long long>(sim.ints["bc.menu.wait"]),
                    static_cast<long long>(sim.ints["bc.menu.none"]), static_cast<long long>(sim.ints["bc.menu.fail"]),
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_menu).count(),
                    sim.texts["bc.menu.key"].c_str());
        int shot = 0;
        const auto show = [&](const char* what) {
            sample_n(1);
            const std::string key = sim.texts["bc.menu.key"];
            const auto t1 = std::chrono::steady_clock::now();
            save(key, "sim_menu" + std::to_string(shot++) + "_" + what);
            sample_n(1);
            std::printf("  %s: key %s tabs %lld pkey %s (%.0f ms)\n", what, key.c_str(),
                        static_cast<long long>(sim.ints["bc.menu.tabs"]), sim.texts["bc.menu.pkey"].c_str(),
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count());
        };
        const auto tap = [&](int x, int y, const char* what) {
            const long long packed = (static_cast<long long>(x) << 32) | static_cast<unsigned>(y);
            const auto ok = ext->on_action(inst, "menu_tap", packed);
            std::printf("menu tap %d,%d (%s) accepted %u\n", x, y, what, ok);
        };
        // Changes are made while the game is paused (as the game's own menu does).
        sim.guest.SetPause(true);
        sample_n(2);
        show("equipment");
        tap(140, 360, "arcana card");
        show("arcanas");
        tap(395, 168, "Infernal Pyre");
        show("pyre inspected");
        tap(395, 168, "Infernal Pyre again");
        helper_log("equip the pyre");
        std::printf("back accepted %u\n", ext->on_action(inst, "menu_back", 0));
        show("back");
        tap(526, 360, "weapon card");
        show("armoury");
        ext->on_action(inst, "menu_back", 0);
        show("equipment");
        tap(946, 290, "relic slot 2");
        show("relics, slot 2");
        tap(94, 365, "Crypt Key");
        show("crypt key inspected");
        tap(94, 365, "Crypt Key again");
        helper_log("crypt key into slot 2");
        sample_n(32);
        show("after equip");
        std::printf("  worn now: %d\n", sim.guest.Get<std::uint64_t>(sim.guest.Get<std::uint64_t>(sim.guest.inv_ + 0x40) + 0x28) == sim.guest.items_[5]);
        tap(355, 167, "slot 2 again");
        helper_log("take it off");
        sample_n(32);
        show("relic off");
        tap(540, 167, "relic slot 3 (locked)");
        ext->on_action(inst, "menu_back", 0);
        sample_n(32);
        show("equipment again");
        sim.guest.SetPause(false);
        tap(140, 360, "arcana card (not paused)");
        show("arcanas, playing");
        tap(395, 168, "Infernal Pyre");
        show("pyre, playing");
        tap(395, 168, "Infernal Pyre again");
        helper_log("equip while playing: refused");
        ext->on_action(inst, "menu_back", 0);
        sample_n(2);
    }
    // Dragging the map: the view moves with the finger, the player marker the same way, and
    // the player button shows; it goes back at once with the button.
    {
        sample_n(2);
        const auto st = [&](const char* when) {
            std::printf("%s: ox=%lld oy=%lld mk=%lld,%lld home=%lld show=%lld req=%lld key=%s\n", when,
                        static_cast<long long>(sim.ints["bc.map.ox"]), static_cast<long long>(sim.ints["bc.map.oy"]),
                        static_cast<long long>(sim.ints["bc.mk.dx"]), static_cast<long long>(sim.ints["bc.mk.dy"]),
                        static_cast<long long>(sim.ints["bc.pan.home"]), static_cast<long long>(sim.ints["bc.map.show"]),
                        static_cast<long long>(sim.ints["bc.map.reqshow"]), sim.texts["bc.map.key"].c_str());
        };
        st("before drag");
        const auto drag = [&](int dx, int dy) {
            const long long packed = (static_cast<long long>(dx) << 32) | static_cast<unsigned>(dy);
            ext->on_action(inst, "map_pan", packed);
        };
        for (int i = 0; i < 10; ++i)
            drag(-30, 12), sample_n(1);
        st("dragged -300,+120");
        save(sim.texts["bc.map.key"], "sim_pan");
        for (int i = 0; i < 40; ++i)
            drag(-40, 0), sample_n(1);
        st("dragged 1600 more");
        ext->on_action(inst, "map_home", 0);
        sample_n(3);
        st("home");
        std::printf("windows drawn so far %d\n", windows_drawn);
    }
    // Pins: the game's markers (a cross at (52, 46), a chest at (28, 34)). Standing on the cross
    // puts it at the canvas centre (the view keeps the player's point there), so the taps below
    // know where it is.
    {
        sim.guest.Put<float>(sim.guest.explorer_ + 0x90, 52.0f);
        sim.guest.Put<float>(sim.guest.explorer_ + 0x94, 46.0f);
        sample_paced(40);
        const int px = 620, py = 540;
        ext->on_action(inst, "pin_mode", 0);
        ext->on_action(inst, "pin_type", 2);
        sample_n(2);
        std::printf("pin mode=%lld btn=%s k2=%s markers=%lld cross at %d,%d\n", static_cast<long long>(sim.ints["bc.pin.mode"]),
                    sim.texts["bc.pin.btn"].c_str(), sim.texts["bc.pin.k2"].c_str(),
                    static_cast<long long>(sim.ints["bc.markers.n"]), px, py);
        const auto tap = [&](int x, int y, const char* what) {
            const long long packed = (static_cast<long long>(x) << 32) | static_cast<unsigned>(y);
            const auto ok = ext->on_action(inst, "pin_tap", packed);
            std::printf("tap %d,%d accepted %u: ", x, y, ok);
            helper_log(what);
        };
        tap(px + 3, py - 2, "eye on the cross: change");
        tap(px + 300, py, "eye on empty map: add");
        ext->on_action(inst, "pin_type", 0);
        tap(px, py, "cross on the cross: remove");
        ext->on_action(inst, "pin_type", 3);
        sample_n(2);
        std::printf("eraser: hint %s k3 %s\n", sim.texts["bc.pin.hint"].c_str(), sim.texts["bc.pin.k3"].c_str());
        tap(px - 480 + 15, py + 240 - 10, "eraser near the chest: remove");
        tap(px - 400, py - 300, "eraser on nothing: refused");
        ext->on_action(inst, "pin_type", 0);
        save(sim.texts["bc.map.key"], "sim_pins_close");
        for (const char* k : {"zbtn:2", "zbtn:0", "zoom:2", "btn:pin:0", "btn:pin:1", "pin:0:0", "pin:2:1", "pin:3:0",
                             "pin:3:1", "pinhint:0", "pinhint:1", "home"})
            {
                std::string n = k;
                for (char& c : n)
                    if (c == ':')
                        c = '_';
                save(std::string("module:bc:ui:") + k, "sim_ui2_" + n);
            }
        ext->on_action(inst, "zbar_open", 0);
        sample_n(1);
        std::printf("zbar=%lld zbtn=%s\n", static_cast<long long>(sim.ints["bc.zbar"]), sim.texts["bc.ui.zbtn"].c_str());
        ext->on_action(inst, "zoom_set", 1);
        sample_n(2);
        std::printf("after zoom_set 1: zbar=%lld zoom=%lld key=%s\n", static_cast<long long>(sim.ints["bc.zbar"]),
                    static_cast<long long>(sim.ints["bc.zoom"]), sim.texts["bc.map.key"].c_str());
        ext->on_action(inst, "zoom_set", 2);
        sample_n(2);
    }
    // Pause, then a death.
    {
        sim.guest.SetPause(true);
        sample_n(2);
        std::printf("paused=%lld f=%lld bkey=%s marks='%s'\n", static_cast<long long>(sim.ints["bc.paused"]),
                    static_cast<long long>(sim.ints["bc.paused.f"]), sim.texts["bc.map.bkey"].c_str(),
                    sim.texts["bc.marks"].c_str());
        save(sim.texts["bc.map.bkey"], "sim_blur");
        save("module:bc:ui:paused", "sim_ui3_paused");
        sim.guest.SetPause(false);
        sample_n(2);
        std::printf("unpaused=%lld\n", static_cast<long long>(sim.ints["bc.paused"]));
        sim.guest.SetDied(false);
        sample_n(2);
        sim.guest.SetDied(true);
        sample_n(2);
        std::printf("died: go.show=%lld key=%s\n", static_cast<long long>(sim.ints["bc.go.show"]), sim.texts["bc.go.key"].c_str());
        for (int f : {0, 3, 7})
            save("module:bc:ui:gameover:" + std::to_string(f), "sim_ui3_gameover" + std::to_string(f));
        save("module:bc:ui:prompt", "sim_ui3_prompt");
        save("module:bc:ui:version", "sim_ui3_version");
        save("module:bc:ui:menuart", "sim_ui4_menuart");
    }
    // Title and loading pages: the game's loading state on top (shown once it holds), the cards.
    {
        sample_n(2);
        const auto flags = [&](const char* when) {
            std::printf("%s: title=%lld load=%lld ingame=%lld art=%s lart=%s ltxt=%s\n", when,
                        static_cast<long long>(sim.ints["bc.ui.title"]), static_cast<long long>(sim.ints["bc.ui.load"]),
                        static_cast<long long>(sim.ints["bc.ui.ingame"]), sim.texts["bc.ui.art"].c_str(),
                        sim.texts["bc.ui.lart"].c_str(), sim.texts["bc.ui.ltxt"].c_str());
        };
        flags("in game");
        sim.guest.SetLoading(true);
        sample_n(1);
        flags("loading at once");
        std::this_thread::sleep_for(std::chrono::milliseconds(450));
        sample_n(1);
        flags("loading held");
        std::printf("fact=%lld key=%s load0=%lld load1=%lld\n", static_cast<long long>(sim.ints["bc.ui.fact"]),
                    sim.texts["bc.ui.factkey"].c_str(), static_cast<long long>(sim.ints["bc.ui.load0"]),
                    static_cast<long long>(sim.ints["bc.ui.load1"]));
        save(sim.texts["bc.ui.factkey"], "sim_ui5_fact");
        save(sim.texts["bc.ui.lart"], "sim_ui5_loadart");
        for (int d = 1; d <= 3; ++d)
            save("module:bc:ui:loadtxt:" + std::to_string(d), "sim_ui5_loadtxt" + std::to_string(d));
        sim.guest.SetLoading(false);
        sample_n(1);
        flags("loaded");
        for (int c = 0; c < 9; ++c)
            save("module:bc:ui:menuart:" + std::to_string(c), "sim_ui5_card" + std::to_string(c));
    }
    save(sim.texts["bc.title.key"], "sim_main_title");
    ext->on_action(inst, "zoom_set", 0); // far
    sample_n(2);
    save(sim.texts["bc.map.key"], "sim_main_far");
    std::printf("lm log:\n%s\nrender ms %lld\n", sim.texts["bc.lm.log"].c_str(),
                static_cast<long long>(sim.ints["bc.render.ms"]));
    std::printf("unknown action accepted: %u\n", ext->on_action(inst, "nope", 0));
    std::printf("bad key accepted: %u\n", ext->load_image(inst, &host, "module:bc:other", nullptr, Sink));
    api->destroy(inst);
    dlclose(lib);
    std::printf("done\n");
    return 0;
}
