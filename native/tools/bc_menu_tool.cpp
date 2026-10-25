// SPDX-License-Identifier: GPL-3.0-or-later
// Test tool: renders the game menu (bc_menu) from a romfs directory to PPM files.
//   bc_menu_tool <romfs-dir> <out-prefix> [screen_w screen_h [list]]
// The menu reads the bundles the module pins (bc_menu_bundles.inc) that the directory has.
#include <dirent.h>
#include <malloc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "bc_facts.h"
#include "bc_levelmap.h"
#include "bc_menu.h"
#include "bc_menupage.h"
#ifdef BC_TEST_ASTC
#include "astc_rgba.h"
#endif

namespace {
long Rss() {
    long pages = 0, rss = 0;
    if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &pages, &rss) != 2)
            rss = 0;
        std::fclose(f);
    }
    return rss * 4096 / (1024 * 1024);
}
std::string g_root;
std::size_t ReadRomfs(const std::string& p, std::uint64_t off, void* out, std::size_t size) {
    std::ifstream in(g_root + "/" + p, std::ios::binary);
    if (!in) {
        const auto slash = p.rfind('/');
        in.open(g_root + "/" + p.substr(slash + 1), std::ios::binary);
        if (!in)
            return 0;
    }
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
void Save(const std::string& path, const std::vector<std::uint8_t>& px, int w, int h) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
        const int a = px[i + 3];
        const std::uint8_t c[3] = {static_cast<std::uint8_t>((px[i] * a + 12 * (255 - a)) / 255),
                                   static_cast<std::uint8_t>((px[i + 1] * a + 10 * (255 - a)) / 255),
                                   static_cast<std::uint8_t>((px[i + 2] * a + 16 * (255 - a)) / 255)};
        std::fwrite(c, 1, 3, f);
    }
    std::fclose(f);
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <romfs-dir> <out-prefix> [w h [list]]\n", argv[0]);
        return 2;
    }
    g_root = argv[1];
    const std::string out = argv[2];
    const int sw = argc > 4 ? std::atoi(argv[3]) : 1240, sh = argc > 4 ? std::atoi(argv[4]) : 1080;
    std::vector<std::string> names;
    const std::string dir = g_root + "/Data/StreamingAssets/aa/Switch";
    if (DIR* d = opendir(dir.c_str())) {
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
    const auto t0 = std::chrono::steady_clock::now();
    const auto ms = [&] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    auto names_data = bc_levelmap::LoadNames(assets);
    auto common = bc_levelmap::LoadCommon(assets, codecs);
    std::printf("names+common %.0f ms (rss %ld MB)\n", ms(), Rss());
    static const std::vector<std::string> pinned = {
#include "bc_menu_bundles.inc"
    };
    std::vector<std::string> menu_names;
    for (const auto& n : pinned)
        if (std::find(names.begin(), names.end(), n) != names.end())
            menu_names.push_back(n);
    std::printf("menu bundles: %zu of %zu pinned here\n", menu_names.size(), pinned.size());
    bc_levelmap::Assets menu_assets(ReadRomfs, menu_names);
    bc_menupage::Menu menu(menu_assets, codecs);
    std::string log;
    const bool ok = menu.Load(*names_data, common->font, &log);
    std::printf("menu %.0f ms (rss %ld MB, sprites %zu MB): %s", ms(), Rss(), menu.SpriteBytes() >> 20, log.c_str());
    malloc_trim(0);
    std::printf("after trim rss %ld MB\n", Rss());
    if (!ok)
        return 1;
    if (std::getenv("BC_FACTS")) {
        // The loading page's fact cards.
        for (int i = 0; i < bc_facts::kCount; ++i) {
            const auto& f = bc_facts::kFacts[i];
            const auto t1 = ms();
            const auto px = menu.RenderFact(f.title, f.text, f.art, f.item, 1180, 430);
            std::printf("fact %d: %.0f ms %s\n", i, ms() - t1, px.empty() ? "EMPTY" : "");
            Save(out + "_fact" + std::to_string(i) + ".ppm", px, 1180, 430);
        }
        return 0;
    }
    // A made-up player.
    bc_menupage::Snapshot s;
    s.ok = true;
    s.weapon = "WeaponWhipTier4";
    s.spell = "BloodSpikes";
    s.charms = {"Charm_BatPet", "Charm_ChestRecover"};
    s.owned = {"WeaponWhipTier1", "WeaponWhipTier4", "WeaponOneHandedTier1", "WeaponSpearTier2", "WeaponTwoHandedTier1",
               "WeaponFistTier1", "BloodSpikes", "BoomerangCross", "FireSeismicWave", "GlassArmor", "Charm_BatPet",
               "Charm_ChestRecover", "Charm_BoostDamageAfterSpell", "Charm_PotionGiveMana", "Charm_KillMobBoostSpeed",
               "Charm_BoostDamageFullLife", "Charm_ParryGiveMana", "Charm_BoostDamageBackHit"};
    s.charms.push_back("");
    s.stats = {{"PowerAttack", 37}, {"PowerDefense", 12}, {"HealthCurrent", 180}, {"HealthMax", 240},
               {"SpellManaCurrent", 60}, {"SpellManaMax", 90}, {"HealthUpgradeCount", 3},
               {"ManaUpgradeCount", 2}, {"HealthUpgradeFragmentUnmergedCount", 1}, {"Level", 17},
               {"ExperienceCurrent", 300}, {"ExperienceMax", 800}, {"PowerAttackBonus", 5}};
    s.level = 17, s.xp = 300, s.xp_max = 800;
    for (const auto& id : argc > 5 ? menu.Ids() : std::vector<std::string>{}) {
        const auto* it = menu.Info(id);
        std::printf("  item %s type %d icon %s illus %d char %s name '%s'\n", id.c_str(), it->type, it->icon.c_str(),
                    it->illustration ? 1 : 0, it->character.c_str(), menu.Name(id).c_str());
    }
    std::printf("weapon %s spell %s charms %s, %s\n", s.weapon.c_str(), s.spell.c_str(), s.charms[0].c_str(),
                s.charms[1].c_str());
    const char* const views[] = {"equipment", "weapons", "relics", "arcanas"};
    for (int rep = 0; rep < 2; ++rep)
        for (int v = 0; v < 4; ++v) {
            s.view = v;
            const auto t1 = ms();
            menu.Render(s, sw, sh, nullptr);
            std::printf("warm-up %d %s: %.0f ms\n", rep, views[v], ms() - t1);
        }
    for (int v = 0; v < 4; ++v) {
        s.view = v;
        s.slot = v == 0 ? 0 : v == 2 ? 1 : -1;
        s.inspect = v == 1 ? "WeaponSpearTier2" : v == 2 ? "Charm_PotionGiveMana" : v == 3 ? "FireSeismicWave" : "";
        std::vector<bc_menupage::Target> targets;
        const auto t1 = ms();
        const auto px = menu.Render(s, sw, sh, &targets);
        std::printf("%s: %.0f ms, %zu targets\n", views[v], ms() - t1, targets.size());
        for (const auto& t : targets)
            std::printf("  kind %d index %d item %s at %.0f,%.0f %.0fx%.0f\n", static_cast<int>(t.kind), t.index,
                            t.item.c_str(), t.x, t.y, t.w, t.h);
        Save(out + "_" + views[v] + ".ppm", px, sw, sh);
        std::printf("  rss %ld MB\n", Rss());
    }
    return 0;
}
