// SPDX-License-Identifier: GPL-3.0-or-later
// Test tool: reads every biome's level map from a romfs directory and prints its log.
//   bc_lm_tool <romfs-dir>
#include <dirent.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "bc_levelmap.h"
#ifdef BC_TEST_ASTC
#include "astc_rgba.h"
#endif

namespace {
std::string g_root;
std::size_t ReadRomfs(const std::string& p, std::uint64_t off, void* out, std::size_t size) {
    std::ifstream in(g_root + "/" + p, std::ios::binary);
    if (!in)
        return 0;
    in.seekg(0, std::ios::end);
    const auto total = static_cast<std::size_t>(in.tellg());
    if (!out)
        return total;
    if (off > total)
        return 0;
    const std::size_t n = std::min<std::size_t>(size, total - off);
    in.seekg(static_cast<std::streamoff>(off));
    in.read(static_cast<char*>(out), static_cast<std::streamsize>(n));
    return n;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2)
        return 2;
    g_root = argv[1];
    std::vector<std::string> names;
    if (DIR* d = opendir((g_root + "/Data/StreamingAssets/aa/Switch").c_str())) {
        while (auto* e = readdir(d))
            if (e->d_name[0] != '.')
                names.push_back(e->d_name);
        closedir(d);
    }
    bc_levelmap::Assets assets(ReadRomfs, names);
    bc_levelmap::Codecs codecs;
#ifdef BC_TEST_ASTC
    codecs.astc = [](const std::uint8_t* in, std::size_t in_size, std::uint32_t w, std::uint32_t h, std::uint32_t bw,
                     std::uint32_t bh, std::uint8_t* px) { return TestAstcDecode(in, in_size, w, h, bw, bh, px); };
#endif
    const auto nm = bc_levelmap::LoadNames(assets);
    if (!nm)
        return 1;
    for (int b = 1; b <= 12; ++b) {
        char world[64];
        std::snprintf(world, sizeof world, "World_Biome_%02d_X", b);
        std::string log;
        const auto lm = bc_levelmap::LoadLevelMap(assets, codecs, *nm, world, &log);
        std::printf("== biome %d: %s\n%s\n", b, lm ? "ok" : "failed", log.c_str());
    }
    return 0;
}
