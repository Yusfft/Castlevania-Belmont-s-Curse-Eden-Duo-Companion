// SPDX-License-Identifier: GPL-3.0-or-later
// End-to-end check of the built module through its C ABI, the way Eden Duo drives it:
// dlopen, eden_dsmod_get_module / eden_dsmod_get_extensions, create, sample, on_action,
// load_image, destroy. Guest memory is a FakeGuest; romfs is a directory.
//   bc_host_sim <module.so> <romfs-or-bundle-dir> <out-dir>
#include <dlfcn.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <thread>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "fake_guest.h"

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
    sim.guest.Build(0x4000, 59, -7, 15, 1, 0, "World_Biome_01_Streets_Poor", {0, 3, 3, 1}, 2, 2);

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
    ext->configure(inst, nullptr);
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
        std::printf("  %-18s '%s'\n", k.c_str(), v.substr(0, 120).c_str());

    for (int step = 0; step < 3; ++step) {
        Png png{out + "/sim_zoom" + std::to_string(step) + ".ppm"};
        const std::string key = sim.texts["bc.map.key"];
        const auto ok = ext->load_image(inst, &host, key.c_str(), &png, Sink);
        std::printf("load_image(%s) = %u  marker show=%lld x=%lld y=%lld\n", key.c_str(), ok,
                    static_cast<long long>(sim.ints["bc.mk.show"]),
                    static_cast<long long>(sim.ints["bc.mk.x"]),
                    static_cast<long long>(sim.ints["bc.mk.y"]));
        ext->on_action(inst, "zoom", 0);
        for (int i = 0; i < 31; ++i)
            api->sample(inst, &host);
    }
    std::printf("unknown action accepted: %u\n", ext->on_action(inst, "nope", 0));
    std::printf("bad key accepted: %u\n", ext->load_image(inst, &host, "module:bc:other", nullptr, Sink));
    api->destroy(inst);
    dlclose(lib);
    std::printf("done\n");
    return 0;
}
