// SPDX-License-Identifier: GPL-3.0-or-later
// Test tool: loads every pinned biome from a directory that mirrors romfs
// (<root>/Data/StreamingAssets/aa/Switch/<bundle>, or a flat directory of bundles), prints a
// JSON summary per room, and writes PPM previews of the renderer output.
//   bc_world_tool <romfs-or-bundle-dir> <out-dir>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "bc_render.h"
#include "bc_world.h"

static std::optional<std::vector<std::uint8_t>> ReadPath(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return std::nullopt;
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

static void WritePpm(const std::string& path, const std::vector<std::uint8_t>& rgba, int w, int h) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) {
        const std::uint8_t* p = &rgba[static_cast<std::size_t>(i) * 4];
        const int a = p[3];
        // Composite over the page background #0C0A10.
        const std::uint8_t px[3] = {
            static_cast<std::uint8_t>((p[0] * a + 12 * (255 - a)) / 255),
            static_cast<std::uint8_t>((p[1] * a + 10 * (255 - a)) / 255),
            static_cast<std::uint8_t>((p[2] * a + 16 * (255 - a)) / 255)};
        std::fwrite(px, 1, 3, f);
    }
    std::fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <romfs-or-bundle-dir> <out-dir>\n", argv[0]);
        return 2;
    }
    const std::string root = argv[1], out = argv[2];
    const auto read = [&](const std::string& path) -> std::optional<std::vector<std::uint8_t>> {
        if (auto r = ReadPath(root + "/" + path))
            return r;
        const auto slash = path.rfind('/');
        return ReadPath(root + "/" + path.substr(slash + 1));
    };
    std::string log;
    const auto t0 = std::chrono::steady_clock::now();
    auto world = bc_world::LoadWorld(read, &log);
    const auto t1 = std::chrono::steady_clock::now();
    std::fprintf(stderr, "loaded %zu biomes, %zu rooms in %.1f ms\n%s", world->biomes.size(),
                 world->RoomCount(),
                 std::chrono::duration<double, std::milli>(t1 - t0).count(), log.c_str());
    for (const auto& b : world->biomes) {
        for (const auto& r : b.rooms) {
            std::uint64_t h = 1469598103934665603ULL;
            int enabled = 0;
            for (const auto& c : r.chunks) {
                enabled += c.enabled;
                for (auto t : c.collisions)
                    h = (h ^ t) * 1099511628211ULL;
            }
            std::printf("{\"world\":\"%s\",\"uid\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
                        "\"name\":\"%s\",\"enabled\":%d,\"hash\":\"%016llx\",\"save\":%d,"
                        "\"boss\":%d,\"travel\":%d}\n",
                        b.world_name.c_str(), r.uid, r.pos_x, r.pos_y, r.w, r.h, r.name.c_str(),
                        enabled, static_cast<unsigned long long>(h), r.save_room, r.boss_room,
                        r.fast_travel_room);
        }
        bc_render::Fog fog; // unknown: draw everything
        bc_render::Params p;
        p.zoom = 0;
        p.current_uid = b.rooms.front().uid;
        const auto r0 = std::chrono::steady_clock::now();
        auto img = bc_render::Render(b, fog, p);
        const auto r1 = std::chrono::steady_clock::now();
        std::fprintf(stderr, "%s: bbox %d..%d x %d..%d, render %.1f ms\n", b.world_name.c_str(),
                     b.min_cx, b.max_cx, b.min_cy, b.max_cy,
                     std::chrono::duration<double, std::milli>(r1 - r0).count());
        WritePpm(out + "/" + b.world_name + "_z0.ppm", img, p.out_w, p.out_h);
        // A close view centred on the first save room (or the first room).
        const bc_world::Room* focus = &b.rooms.front();
        for (const auto& r : b.rooms) {
            if (r.save_room) {
                focus = &r;
                break;
            }
        }
        p.zoom = 2;
        p.center_cx = focus->pos_x;
        p.center_cy = focus->pos_y;
        p.current_uid = focus->uid;
        img = bc_render::Render(b, fog, p);
        WritePpm(out + "/" + b.world_name + "_z2.ppm", img, p.out_w, p.out_h);
    }
    return 0;
}
