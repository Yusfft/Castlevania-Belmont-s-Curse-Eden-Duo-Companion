// SPDX-License-Identifier: GPL-3.0-or-later
// End-to-end check of the built module through its C ABI, the way Eden Duo drives it:
// dlopen, eden_dsmod_get_module / eden_dsmod_get_extensions, create, sample, on_action,
// load_image, destroy. Guest memory is a FakeGuest; romfs is a directory.
//   bc_host_sim <module.so> <romfs-or-bundle-dir> <out-dir>
#include <dirent.h>
#include <sys/stat.h>
#include <dlfcn.h>

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
    if (out_size < static_cast<std::size_t>(w) * h * 4)
        return EDEN_DSMOD_FALSE;
    return TestAstcDecode(in, in_size, w, h, bw, bh, out) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
#else
    (void)w, (void)h, (void)bw, (void)bh, (void)in, (void)in_size, (void)out, (void)out_size;
    return EDEN_DSMOD_FALSE;
#endif
}
double GetF(void*, const char*, double fb) { return fb; }
const char* GetT(void*, const char*) { return nullptr; }

struct Png {
    std::string path;
};
void Sink(void* receiver, std::uint32_t w, std::uint32_t h, const std::uint8_t* px, std::size_t size) {
    auto* out = static_cast<Png*>(receiver);
    std::FILE* f = std::fopen(out->path.c_str(), "wb");
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
    sim.romfs = argv[2];
    const std::string out = argv[3];
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
    {
        // The module's pin file, with one chest at (50, 44) on map 1.
        const char* xdg = std::getenv("XDG_DATA_HOME");
        if (xdg && *xdg) {
            std::string dir = std::string(xdg) + "/eden";
            for (const char* part : {"", "/dualscreen", "/persist", "/0100BA4026E94000"}) {
                dir += part;
                ::mkdir(dir.c_str(), 0700);
            }
            const long long chest = (1LL << 48) | (1LL << 43) | (1LL << 40) |
                                    ((50LL * 4 + (1 << 19)) << 20) | (44LL * 4 + (1 << 19));
            if (std::FILE* f = std::fopen((dir + "/belmonts-curse-map-pins.txt").c_str(), "wb")) {
                std::fprintf(f, "%lld\n", chest);
                std::fclose(f);
            }
        }
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
            std::printf("sim: level %s\n", lm->level_guid.c_str());
            std::printf("sim: %zu explored rooms\n", rooms.size());
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 600; ++i) {
        sim.ints.clear();
        sim.texts.clear();
        api->sample(inst, &host);
        api->tick(inst, &host);
        if (sim.ints["bc.world.ready"] && i > 40)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::printf("after %.0f ms:\n",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    for (const auto& [k, v] : sim.ints)
        std::printf("  %-18s %lld\n", k.c_str(), static_cast<long long>(v));
    for (const auto& [k, v] : sim.texts)
        std::printf("  %-18s '%s'\n", k.c_str(), v.substr(0, k == "bc.world.log" ? 4000 : 120).c_str());

    const auto sample_n = [&](int n) {
        for (int i = 0; i < n; ++i) {
            sim.ints.clear();
            sim.texts.clear();
            api->sample(inst, &host);
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
        sim.guest.Put<float>(sim.guest.explorer_ + 0x90, -134.0f + step);
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
        sample_n(10);
        std::printf("teleport held 10 samples: %s key %s\n", mk().c_str(), sim.texts["bc.map.key"].c_str());
        sim.guest.Put<float>(sim.guest.explorer_ + 0x90, -94.0f);
        sample_n(10);
    }
    ext->on_action(inst, "zoom", 0); // close -> medium
    sample_n(2);
    std::printf("medium: key=%s\n", sim.texts["bc.map.key"].c_str());
    save(sim.texts["bc.map.key"], "sim_medium");
    ext->on_action(inst, "zoom", 0); // medium -> area
    sample_n(2);
    std::printf("area: akey=%s area=%lld mk=%lld,%lld\n", sim.texts["bc.map.akey"].c_str(),
                static_cast<long long>(sim.ints["bc.map.area"]), static_cast<long long>(sim.ints["bc.mk.dx"]),
                static_cast<long long>(sim.ints["bc.mk.dy"]));
    save(sim.texts["bc.map.akey"], "sim_area");
    std::printf("title '%s' key %s img %lld\n", sim.texts["bc.title"].c_str(), sim.texts["bc.title.key"].c_str(),
                static_cast<long long>(sim.ints["bc.title.img"]));
    save(sim.texts["bc.title.key"], "sim_title");
    save("module:bc:player:70", "sim_player70");
    for (const char* k : {"frame", "zoom:0", "zoom:2", "btn:debug", "btn:map", "wait", "debug", "menuart", "prompt", "version"})
        save(std::string("module:bc:ui:") + k, std::string("sim_ui_") + (k[0] == 'z' ? "zoom" + std::string(k + 5) : k[0] == 'b' ? std::string(k + 4) : std::string(k)));
    std::printf("zoom_set 1 accepted: %u\n", ext->on_action(inst, "zoom_set", 1));
    sample_n(2);
    std::printf("ui zoom key %s ok %lld\n", sim.texts["bc.ui.zoom"].c_str(), static_cast<long long>(sim.ints["bc.ui.ok"]));
    ext->on_action(inst, "zoom_set", 0);
    // Into the main map, near (40, 40): close view.
    ext->on_action(inst, "zoom", 0); // area -> close
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
    // Pins: read from the module's pin file (written below before create: a chest at (50, 44)
    // on map 1), then pin mode, symbol 2 (eye), taps.
    {
        sample_n(2);
        std::printf("pins n=%lld store=%s\n", static_cast<long long>(sim.ints["bc.pin.n"]),
                    sim.texts["bc.pin.store"].c_str());
        ext->on_action(inst, "pin_mode", 0);
        ext->on_action(inst, "pin_type", 2);
        sample_n(2);
        const auto tap = [&](int x, int y) {
            const long long packed = (static_cast<long long>(x) << 32) | static_cast<unsigned>(y);
            const auto ok = ext->on_action(inst, "pin_tap", packed);
            sample_n(2);
            std::printf("tap %d,%d -> %u n=%lld store=%s key=%s\n", x, y, ok,
                        static_cast<long long>(sim.ints["bc.pin.n"]), sim.texts["bc.pin.store"].c_str(),
                        sim.texts["bc.map.key"].c_str());
        };
        std::printf("pin mode=%lld btn=%s k2=%s\n", static_cast<long long>(sim.ints["bc.pin.mode"]),
                    sim.texts["bc.pin.btn"].c_str(), sim.texts["bc.pin.k2"].c_str());
        tap(820, 460); // on the chest: becomes an eye
        tap(420, 600); // a new eye
        tap(300, 300); // another
        tap(300, 300); // same symbol again: removed
        save(sim.texts["bc.map.key"], "sim_pins_close");
        for (const char* k : {"zbtn:4", "zbtn:0", "zoom:3", "btn:pin:0", "btn:pin:1", "pin:0:0", "pin:2:1", "pinhint"})
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
        ext->on_action(inst, "zoom_set", 3);
        sample_n(2);
        std::printf("after zoom_set 3: zbar=%lld zoom=%lld key=%s\n", static_cast<long long>(sim.ints["bc.zbar"]),
                    static_cast<long long>(sim.ints["bc.zoom"]), sim.texts["bc.map.key"].c_str());
        ext->on_action(inst, "zoom_set", 4);
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
    ext->on_action(inst, "zoom", 0);
    ext->on_action(inst, "zoom", 0); // -> area
    sample_n(2);
    save(sim.texts["bc.map.akey"], "sim_main_area");
    std::printf("main area: akey=%s mk=%lld,%lld\n", sim.texts["bc.map.akey"].c_str(),
                static_cast<long long>(sim.ints["bc.mk.dx"]), static_cast<long long>(sim.ints["bc.mk.dy"]));
    std::printf("lm log:\n%s\nrender ms %lld\n", sim.texts["bc.lm.log"].c_str(),
                static_cast<long long>(sim.ints["bc.render.ms"]));
    std::printf("unknown action accepted: %u\n", ext->on_action(inst, "nope", 0));
    std::printf("bad key accepted: %u\n", ext->load_image(inst, &host, "module:bc:other", nullptr, Sink));
    api->destroy(inst);
    dlclose(lib);
    std::printf("done\n");
    return 0;
}
