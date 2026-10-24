// SPDX-License-Identifier: GPL-3.0-or-later
//
// Castlevania: Belmont's Curse (0100BA4026E94000), game version 1.1.0, build 0F1169AA7D0FA452.
// Eden Duo second-screen map companion.
//
//   * Map: the game's own level map, read at runtime from the player's romfs (bc_levelmap) and
//     drawn by the module (bc_mapdraw) as `module:bc:...` images: the map tiles with the game's
//     map material, the fog of war from the exploration zones the player entered, the map icons,
//     the player marker and the area title in the game's font. Nothing from the game is shipped
//     in the package. If a biome's level map cannot be read, the room layout (bc_world) is drawn
//     instead (bc_render, the 0.2 renderer).
//   * Live: room, exploration and the player's world position are read from guest memory
//     (bc_live). The position is ExplorerComponent.lastMainMapPosition, which
//     ExplorationSystem.TryUpdateMainMapPosition sets to the player's transform position every
//     frame (except where the game itself freezes the map position).
//
// Follow view: the module renders a 2048 x 1536 window of the map around the player and the
// page slides it under a fixed marker, so the map follows the player smoothly; a new window is
// rendered only when the player nears its edge or explores something new. The previous window
// stays drawn underneath until the new one arrives, so there is no blank frame.

#include <algorithm>
#include <cstdlib>
#include <sys/stat.h>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "bc_levelmap.h"
#include "bc_live.h"
#include "bc_mapdraw.h"
#include "bc_render.h"
#include "bc_ui.h"
#include "bc_world.h"
#include "core/mods/modules/dsmod_module_sdk.h"

namespace {

using namespace dsmod_sdk::int_types;

constexpr u64 kTitleId = 0x0100BA4026E94000ULL;
constexpr char kBuildHex[] = "0F1169AA7D0FA4529C861F96ADEC306E"
                             "00000000000000000000000000000000";
constexpr const char* kVersion = "0.5.5";

// Canvas and images (must match manifest.json).
constexpr int kCanvasW = 1240;
constexpr int kCanvasH = 1080;
constexpr int kWindowW = 2048; // follow-view window image
constexpr int kWindowH = 1536;
constexpr int kTitleW = 640; // level banner image (the banner art is 512 x 275)
constexpr int kTitleH = 344;
constexpr int kZoomBarW = bc_ui::ZoomBarLayout::kW, kZoomBarH = bc_ui::ZoomBarLayout::kH;
constexpr int kButtonW = 170, kButtonH = 56;
constexpr int kLabelW = 700, kLabelH = 70;
// Re-centre the window when the player is this far (px) from its centre.
constexpr double kRecentreX = (kWindowW - kCanvasW) / 2.0 - 90.0;
constexpr double kRecentreY = (kWindowH - kCanvasH) / 2.0 - 60.0;
// Pixels per world unit at each zoom level (0 = the whole area).
constexpr double kZoomScale[] = {0.0, 5.0, 8.0, 13.0, 20.0};
constexpr int kZoomLevels = bc_ui::ZoomBarLayout::kLevels;
static_assert(sizeof(kZoomScale) / sizeof(kZoomScale[0]) == kZoomLevels);
constexpr int kZoomButtonW = 230, kZoomButtonH = 64;
constexpr int kPinCell = 84;
// The page's own pins: the symbols offered, the slots kept (persisted as runtime flags
// "pin.v<i>", see the manifest), and how close a tap must be to an existing pin to touch it.
constexpr const char* kPinSprites[] = {"Map_MarkerSet_Cross", "Map_MarkerSet_Chest", "Map_MarkerSet_Eye"};
constexpr int kPinTypes = 3;
constexpr int kPinSlots = 24;
constexpr double kPinTouchPx = 40.0;
constexpr const char* kPinFile = "belmonts-curse-map-pins.txt";
// The player marker: the game draws Map_PlayerIcon 5 units tall (the page sizes it per zoom
// level), 0.7 units above the player.
constexpr double kPlayerLift = 0.7;
// Map drawing runs beside the emulator: few threads, low priority, and a new window at most
// every kRedrawSeconds for exploration changes alone.
constexpr int kRenderThreads = 2;
// Position glitches: a jump of more than kJumpUnits from the last position is taken only once
// it holds for kJumpSamples samples in a row (a real teleport shows ~0.1 s later; a one-off bad
// value never shows). A missing or implausible position keeps the last good one for
// kHoldSeconds. A change of world must also hold kWorldSamples samples.
constexpr double kJumpUnits = 12.0;
constexpr int kJumpSamples = 6;
constexpr double kHoldSeconds = 3.0;
constexpr int kWorldSamples = 10;
// Game over: shown kGameOverSeconds after the player dies, its glow running through
// kGameOverGlow (one frame every kGameOverFrameMs), then held at the last.
constexpr double kGameOverSeconds = 2.8;
constexpr int kGameOverFrameMs = 110;
constexpr float kGameOverGlow[] = {0.15f, 0.45f, 0.80f, 1.0f, 0.80f, 0.55f, 0.35f, 0.20f};
constexpr int kGameOverFrames = sizeof(kGameOverGlow) / sizeof(kGameOverGlow[0]);
constexpr int kGameOverW = 760, kGameOverH = 420;
constexpr int kPausedW = 600, kPausedH = 200;
constexpr int kPromptW = 700, kPromptH = 70;
constexpr int kVersionW = 460, kVersionH = 50;
// The pause screen's blurred map: the window drawn this many times smaller, stretched back.
constexpr int kBlurDiv = 6;
constexpr double kRedrawSeconds = 6.0;

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

u64 Fnv(u64 h, std::string_view s) {
    for (const char c : s)
        h = (h ^ static_cast<u8>(c)) * 0x100000001b3ULL;
    return h;
}

/// "World_Biome_01_Streets_Poor" -> "STREETS POOR" (used when the game's names are missing).
std::string BiomeTitle(const std::string& world_name) {
    std::string s = world_name;
    const std::string prefix = "World_Biome_";
    if (s.rfind(prefix, 0) == 0)
        s = s.substr(prefix.size());
    if (s.size() > 3 && std::isdigit(static_cast<unsigned char>(s[0])) &&
        std::isdigit(static_cast<unsigned char>(s[1])) && s[2] == '_')
        s = s.substr(3);
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '_') {
            out += ' ';
        } else {
            if (i > 0 && std::isupper(static_cast<unsigned char>(c)) &&
                std::islower(static_cast<unsigned char>(s[i - 1])))
                out += ' ';
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }
    return out;
}

/// Bundles the level maps read (build 1.1.0). Names only: the files are the player's own.
const std::vector<std::string>& PinnedBundles() {
    static const std::vector<std::string> names = {
        "maps_scenes_01_streets_poor_map_1_d8baa1dae0a4b34f7e696a7a9212e535.bundle",
        "maps_scenes_02_innocentscemetery_map_1_f2e15edf86f4eea7221f0c454c807c8b.bundle",
        "maps_scenes_03_catacombs_map_1_1f83de3ad066abba844f00912fba8449.bundle",
        "maps_scenes_05_notredame_map_1_26f32a40ec5b6d6ac411f1b3985e888b.bundle",
        "maps_scenes_06_mainhall_map_1_7f54316e1cd9f5888f2bf10fc6978f5c.bundle",
        "maps_scenes_08_library_map_1_70a8d7cd2968bb78ee887718a7218274.bundle",
        "maps_scenes_09_clocktower_map_1_2595940dda28ec47f1485f144dca168c.bundle",
        "maps_scenes_10_hallofmirrors_map_1_87e5922a2567cac81ef3f12e18a9d2c1.bundle",
        "maps_scenes_11_masterquarter_map_1_8d214ab1358b12c885e6d2dcd4a97a5e.bundle",
        "maps_scenes_12_mqbadending_map_1_61d3334591f067ba587cc2fe9e198ef6.bundle",
        "art-01_streetspoor_assets_all_e33a704a7ea92e47f2c375f443aacbf1.bundle",
        "art-03_catacombs_assets_all_c055e263f91c9094b04a632b5581848d.bundle",
        "art-05_notredame_assets_all_32ef2b2c715639c5d7e41d803bcb27d1.bundle",
        "art-06_mainhall_assets_all_b8a50ac1737b72382cb0ac62e70bd487.bundle",
        "art-08_library_assets_all_a76b87cc6d1ea3d322f22efc30e0245d.bundle",
        "art-09_clocktower_assets_all_d6bb5abd4c1941cd19cf0e43295e974a.bundle",
        "art-10_hallofmirrors_assets_all_2ee69f349e25b9ac35c3a3220e8b9130.bundle",
        "art-11_masterquarter_assets_all_52b72c61e319c95a97603f4c975125ff.bundle",
        "ui-gamemenus_assets_all_866519f7d6a7057a8ba1c459a5b3f694.bundle",
        "ui-fasttravel_assets_all_89e0a49633a82026de7339a2815e7425.bundle",
        "ui-commonsingame_assets_all_acb23e9b0608a368aad72d2fe488a679.bundle",
        "ui-commonsalltime_assets_all_cbf3732ca9bb007ffe39c0428d68b4e2.bundle",
        "ui-acquisitions_assets_all_401d7a70bb44d71476736dbebe99e9c7.bundle",
        "fonts-latin_assets_all_304d9e1f03b4a2a6d44b17c96032676a.bundle",
        "leveldata_assets_all_940f672e456a1dd57099d3d5c994fbc7.bundle",
        "leveldata-savablescriptableobject_assets_all_ff1d7d702377adb724f6d92b5520502b.bundle",
        "savablescriptableobject_assets_all_5b62e0c70cf11954564852274ab4ce5a.bundle",
        "localization-string-tables-english(en)_assets_all_aa06f0f9074c49a1bdd9e6b274bdec25.bundle",
        "district-districtdata_bosszdoor_assets_all_efd62a1f286ba654c232c9da8083666b.bundle",
        "district-districtdata_catacombs-savablescriptableobject_assets_all_2c5599631df37e00b8a1c0d6e6109564.bundle",
        "district-districtdata_catacombs_assets_all_f8b2592e639239a7e849b0b650ef23bf.bundle",
        "district-districtdata_clocktower-savablescriptableobject_assets_all_3eac144db0bb2f9d6648cad1ffe65d0a.bundle",
        "district-districtdata_clocktower_assets_all_36008c50d01783c19caea477abbf1e42.bundle",
        "district-districtdata_hallofmirrors-savablescriptableobject_assets_all_63d8aaf5423ffd98918b0656f8210244.bundle",
        "district-districtdata_hallofmirrors_assets_all_16d663d40c049744135ff8726d3cee67.bundle",
        "district-districtdata_innocentscemetery-savablescriptableobject_assets_all_ba3ca8f9cf13bff5255ec8c087c84500.bundle",
        "district-districtdata_innocentscemetery_assets_all_75ae6dfb49977b4f36851aef40d2a1f6.bundle",
        "district-districtdata_library-savablescriptableobject_assets_all_9941cb47832135d5007815c99dbb233e.bundle",
        "district-districtdata_library_assets_all_eba90c828280a188e9b40cf8c5750a87.bundle",
        "district-districtdata_mainhall-savablescriptableobject_assets_all_21a44dc94ab1725d1fb79bad78406d32.bundle",
        "district-districtdata_mainhall_assets_all_508e07bff978f5721f37b4330ea27c8c.bundle",
        "district-districtdata_masterquarter-savablescriptableobject_assets_all_bf7724d3a77f668df950f66935bc6663.bundle",
        "district-districtdata_masterquarter_assets_all_150dec9f967c9fec7148ce6522ed5710.bundle",
        "district-districtdata_millersbridge-savablescriptableobject_assets_all_88b1327ac5fc93eb9ff1b4ea85025bb9.bundle",
        "district-districtdata_millersbridge_assets_all_f8730e8095cc97244e22ad10ee0c4ba3.bundle",
        "district-districtdata_mqbadending-savablescriptableobject_assets_all_326541aef871fe71f5f46a6fb8c9fb43.bundle",
        "district-districtdata_mqbadending_assets_all_36a3a6a7c75bfe00b969b2fe7cbcc9ac.bundle",
        "district-districtdata_notredame-savablescriptableobject_assets_all_55764b0987a98f6a32eccd39798ecd2a.bundle",
        "district-districtdata_notredame_assets_all_d95542ff2638c9df7f45627444627b91.bundle",
        "district-districtdata_streets-savablescriptableobject_assets_all_a7360e4b14d252869ffd592c17efe004.bundle",
        "district-districtdata_streets_assets_all_a67fbddd6cfbd9d4b54812a06cb661e6.bundle",
        "district-districtdata_streetsrich-savablescriptableobject_assets_all_0ae6fcf3b8f9fba4f6e203f24dabda7f.bundle",
        "district-districtdata_streetsrich_assets_all_e227455247b19c1d811c9e13a0bfbebd.bundle",
    };
    return names;
}

/// Every "*.bundle" name in an Addressables catalog.
std::vector<std::string> BundlesFromCatalog(std::string_view text) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    constexpr std::string_view kSuffix = ".bundle";
    std::size_t at = 0;
    while ((at = text.find(kSuffix, at)) != std::string_view::npos) {
        std::size_t start = at;
        while (start > 0 && at - start < 240) {
            const char c = text[start - 1];
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '(' ||
                  c == ')'))
                break;
            --start;
        }
        const std::string name(text.substr(start, at + kSuffix.size() - start));
        if (name.size() > kSuffix.size() && seen.insert(name).second)
            out.push_back(name);
        at += kSuffix.size();
    }
    return out;
}

// ---- what the page shows

struct Window {
    bool valid = false;
    bool legacy = false; // drawn by bc_render (the level map could not be read)
    int biome = -1;      // biome number
    int zoom = -1;
    int cx = 0, cy = 0;  // window centre, world units
    u64 explore = 0;
    u64 pins = 0;        // pins revision drawn
    std::string key;
};

/// A pin the player put on the page's map (not in the game).
struct Pin {
    int biome = -1; // level map number; -1 = free slot
    int type = 0;   // index into kPinSprites
    float x = 0, y = 0;
};

/// Pins as stored in the persisted flags: 0 = empty; else bit 48 set, biome in bits 43..47,
/// type in 40..42, x and y in quarter units (offset by 2^19) in 20..39 and 0..19. Under 2^53,
/// so a JSON number keeps it exactly.
s64 EncodePin(const Pin& p) {
    if (p.biome < 0)
        return 0;
    const auto q = [](float v) {
        return static_cast<s64>(std::clamp<long>(std::lround(v * 4.0f), -(1 << 19) + 1, (1 << 19) - 1) + (1 << 19));
    };
    return (s64{1} << 48) | (static_cast<s64>(p.biome & 31) << 43) | (static_cast<s64>(p.type & 7) << 40) |
           (q(p.x) << 20) | q(p.y);
}
Pin DecodePin(s64 v) {
    Pin p;
    if (v <= 0 || !((v >> 48) & 1) || (v >> 49) != 0)
        return p;
    p.biome = static_cast<int>((v >> 43) & 31);
    p.type = static_cast<int>((v >> 40) & 7);
    if (p.type >= kPinTypes)
        p.type = 0;
    p.x = static_cast<float>(((v >> 20) & 0xFFFFF) - (1 << 19)) / 4.0f;
    p.y = static_cast<float>((v & 0xFFFFF) - (1 << 19)) / 4.0f;
    return p;
}

struct Title {
    std::string title, subtitle;
    bc_shade::Vec4 color{0.22f, 0.35f, 0.26f, 1.0f};
};

struct Instance;
void LoadPins(Instance* inst);

struct Instance {
    EdenDsmodHostApi host{};

    // Host ASTC decoder (base extensions); level maps wait for configure().
    std::mutex codec_mutex;
    std::condition_variable codec_cv;
    bool configured = false;
    EdenDsmodAstcDecoder astc = nullptr;
    void* astc_user = nullptr;

    // Loader thread: room layout, assets, names, common art.
    std::mutex world_mutex;
    std::condition_variable world_cv;
    std::shared_ptr<const bc_world::World> world;
    std::string world_log;
    bool world_done = false;  // room layout read (or failed)
    bool assets_done = false; // level map assets, names and font read (or failed)
    std::thread loader, lm_thread;
    std::atomic<bool> stopping{false};

    std::shared_ptr<bc_levelmap::Assets> assets;
    std::shared_ptr<const bc_levelmap::Names> names;
    std::shared_ptr<const bc_levelmap::Common> common;

    // Level maps, loaded on request by the loader thread (two kept).
    std::mutex lm_mutex;
    std::condition_variable lm_cv;
    std::string lm_request;
    std::deque<std::pair<std::string, std::shared_ptr<const bc_levelmap::LevelMap>>> lm_cache;
    std::set<std::string> lm_failed;
    std::string lm_log;
    double lm_ms = 0;

    // Exploration snapshots by id (renders ask for them by the id in the image key).
    std::mutex ex_mutex;
    std::deque<std::pair<u64, std::shared_ptr<const bc_mapdraw::Exploration>>> explorations;
    std::deque<std::pair<u64, std::shared_ptr<const bc_render::Fog>>> fogs;
    std::deque<std::pair<u64, Title>> titles;
    std::atomic<int> last_render_ms{0};

    // Pins: kept in a small text file of the module's own (see PinStorePath), read at start
    // and written on every change.
    std::mutex pin_mutex;
    std::array<Pin, kPinSlots> pins{};
    u64 pins_rev = 1;
    std::deque<std::pair<u64, std::shared_ptr<const std::vector<Pin>>>> pin_snapshots;
    bool pin_mode = false;
    int pin_type = 0;
    std::string pin_store; // "" when no place to keep them was found
    std::string pin_store_status = "-";
    // How a canvas pixel maps to the world in the view on screen (for taps).
    bool tap_view_ok = false;
    int tap_biome = -1;
    bc_mapdraw::View tap_view;

    // Tick-thread state.
    bc_live::Reader reader;
    bc_live::State live;
    u64 samples = 0;
    int zoom = kZoomLevels - 1;
    // The zoom scale is shown (held zoom button) until a level is picked or it times out.
    bool zoom_bar = false;
    std::chrono::steady_clock::time_point zoom_bar_since{};
    u64 explore_id = 1; // never a real id: the first sample builds the snapshot
    std::shared_ptr<const bc_mapdraw::Exploration> exploration;
    std::shared_ptr<const bc_render::Fog> fog;
    Window cur, prev;
    std::string last_world;
    double last_wx = 0, last_wy = 0;
    // Glitch filter (tick thread).
    std::string shown_world;
    int world_votes = 0;
    bool pos_ok = false;
    std::string pos_world;
    double pos_x = 0, pos_y = 0, cand_x = 0, cand_y = 0;
    int cand_n = 0;
    std::chrono::steady_clock::time_point pos_time{};
    u64 glitches = 0;
    // Game over and pause (tick thread).
    bool died_known = false, prev_died = false;
    std::chrono::steady_clock::time_point game_over_at{};
    bool game_over_seen = false;
    std::string blur_key, blur_area_key;
    std::chrono::steady_clock::time_point last_window{};
    double cost_us = 0;

    ~Instance() {
        stopping = true;
        lm_cv.notify_all();
        codec_cv.notify_all();
        if (loader.joinable())
            loader.join();
        lm_cv.notify_all();
        if (lm_thread.joinable())
            lm_thread.join();
    }

    bc_levelmap::Codecs Codecs() {
        std::lock_guard lock(codec_mutex);
        bc_levelmap::Codecs c;
        if (astc) {
            const auto fn = astc;
            void* user = astc_user;
            c.astc = [fn, user](const u8* in, std::size_t in_size, u32 w, u32 h, u32 bw, u32 bh, u8* out) {
                return fn(user, w, h, bw, bh, in, in_size, out, static_cast<std::size_t>(w) * h * 4) != 0;
            };
        }
        return c;
    }

    void StartLoader() {
        // Level map assets first (the map shows as soon as they are read), the room layout
        // (fallback map and position) after them, level maps on request in their own thread.
        loader = std::thread([this] {
            std::string log;
            std::vector<std::string> catalog_names;
            const auto catalog = [&]() -> const std::vector<std::string>& {
                if (catalog_names.empty()) {
                    if (auto cat = ReadRomfs(host, kCatalog))
                        catalog_names =
                            BundlesFromCatalog(std::string_view(reinterpret_cast<const char*>(cat->data()), cat->size()));
                }
                return catalog_names;
            };
            try {
                bc_levelmap::RomfsRead rr = [this](const std::string& path, std::uint64_t off, void* out,
                                                    std::size_t size) -> std::size_t {
                    if (!host.read_romfs)
                        return 0;
                    std::size_t n = host.read_romfs(host.userdata, path.c_str(), off, out, size);
                    if (n == 0)
                        n = host.read_romfs(host.userdata, ("/" + path).c_str(), off, out, size);
                    return n;
                };
                std::vector<std::string> names_list = PinnedBundles();
                const std::string probe = std::string("Data/StreamingAssets/aa/Switch/") + names_list[0];
                if (rr(probe, 0, nullptr, 0) == 0) {
                    names_list = catalog();
                    log += "level map bundles: " + std::to_string(names_list.size()) + " catalog names\n";
                }
                auto a = std::make_shared<bc_levelmap::Assets>(rr, names_list);
                auto n = bc_levelmap::LoadNames(*a);
                {
                    std::unique_lock lock(codec_mutex);
                    codec_cv.wait_for(lock, std::chrono::seconds(10), [this] { return configured || stopping; });
                }
                auto c = bc_levelmap::LoadCommon(*a, Codecs());
                a->Trim();
                log += c->log;
                std::lock_guard lock(world_mutex);
                assets = a;
                names = n;
                common = c;
                assets_done = true;
                world_log = log;
            } catch (...) {
                log += "level map assets failed\n";
                std::lock_guard lock(world_mutex);
                assets_done = true;
            }
            world_cv.notify_all();
            lm_thread = std::thread([this] { LevelMapLoop(); });

            // Room layout.
            std::shared_ptr<const bc_world::World> w;
            const bc_world::ReadFile read = [this](const std::string& path) { return ReadRomfs(host, path); };
            try {
                w = bc_world::LoadWorld(read, &log);
                if (w->biomes.empty()) {
                    log += "pinned biome bundles not found; scanning the catalog\n";
                    std::vector<std::string> biomes;
                    for (const auto& name : catalog()) {
                        if (name.rfind("data-world_biome_", 0) == 0)
                            biomes.push_back(name);
                    }
                    log += std::to_string(biomes.size()) + " biome names in the catalog\n";
                    w = bc_world::LoadWorld(read, biomes, &log);
                }
            } catch (...) {
                log += "room layout load failed\n";
                w.reset();
            }
            {
                std::lock_guard lock(world_mutex);
                world = std::move(w);
                world_log = log;
                world_done = true;
            }
            world_cv.notify_all();
        });
    }

    void LevelMapLoop() {
        for (;;) {
            std::string want;
            {
                std::unique_lock lock(lm_mutex);
                lm_cv.wait(lock, [this] { return stopping || !lm_request.empty(); });
                if (stopping)
                    return;
                want = std::move(lm_request);
                lm_request.clear();
            }
            std::shared_ptr<bc_levelmap::Assets> a;
            std::shared_ptr<const bc_levelmap::Names> n;
            {
                std::lock_guard lock(world_mutex);
                a = assets;
                n = names;
            }
            std::shared_ptr<const bc_levelmap::LevelMap> m;
            const auto t0 = std::chrono::steady_clock::now();
            std::string err;
            std::string load_log;
            try {
                if (a && n)
                    m = bc_levelmap::LoadLevelMap(*a, Codecs(), *n, want, &load_log);
                if (a)
                    a->Trim(); // the map keeps what it needs; drop the bundle caches
                else
                    err = "(assets missing)";
            } catch (...) {
                m.reset();
                err = "exception";
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            {
                std::lock_guard lock(lm_mutex);
                lm_ms = ms;
                if (m) {
                    lm_log = m->log;
                    lm_cache.emplace_back(want, m);
                    while (lm_cache.size() > 1) // one biome in memory
                        lm_cache.pop_front();
                } else {
                    lm_log = "level map " + want + " failed " + err + "\n" + load_log;
                    lm_failed.insert(want);
                }
            }
            lm_cv.notify_all();
        }
    }

    std::shared_ptr<const bc_world::World> World() {
        std::lock_guard lock(world_mutex);
        return world;
    }

    /// The level map of a world if loaded; asks for it otherwise. `state`: 0 none, 1 loading,
    /// 2 ready, 3 failed.
    std::shared_ptr<const bc_levelmap::LevelMap> LevelMap(const std::string& world_name, int* state) {
        std::lock_guard lock(lm_mutex);
        for (const auto& [k, v] : lm_cache) {
            if (k == world_name) {
                *state = 2;
                return v;
            }
        }
        if (lm_failed.count(world_name)) {
            *state = 3;
            return nullptr;
        }
        if (lm_request != world_name) {
            lm_request = world_name;
            lm_cv.notify_all();
        }
        *state = 1;
        return nullptr;
    }

    /// The level map with a biome number, waiting for it to load (render worker).
    std::shared_ptr<const bc_levelmap::LevelMap> WaitLevelMap(int number, int timeout_s) {
        std::unique_lock lock(lm_mutex);
        std::shared_ptr<const bc_levelmap::LevelMap> found;
        lm_cv.wait_for(lock, std::chrono::seconds(timeout_s), [&] {
            for (const auto& [k, v] : lm_cache) {
                if (v && v->number == number) {
                    found = v;
                    return true;
                }
            }
            for (const auto& f : lm_failed) {
                if (bc_levelmap::BiomeNumber(f) == number)
                    return true;
            }
            return stopping.load();
        });
        return found;
    }

    template <class T>
    static void Remember(std::deque<std::pair<u64, T>>& q, u64 id, T v, std::size_t keep) {
        for (const auto& [k, x] : q) {
            if (k == id)
                return;
        }
        q.emplace_back(id, std::move(v));
        while (q.size() > keep)
            q.pop_front();
    }
    template <class T>
    static std::optional<T> Lookup(const std::deque<std::pair<u64, T>>& q, u64 id) {
        for (const auto& [k, x] : q) {
            if (k == id)
                return x;
        }
        return std::nullopt;
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
    std::snprintf(buf, sizeof buf, "%s0x%llX", v < 0 ? "-" : "", static_cast<unsigned long long>(v < 0 ? -v : v));
    return buf;
}

EdenDsmodBool SupportsBuild(const char* build_hex) {
    if (!build_hex)
        return EDEN_DSMOD_FALSE;
    const std::string b = build_hex;
    const std::string full = kBuildHex;
    if (b.size() < 16 || b.size() > full.size())
        return EDEN_DSMOD_FALSE;
    return full.compare(0, b.size(), b) == 0 ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
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
        {
            std::lock_guard lock(inst->pin_mutex);
            LoadPins(inst);
        }
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
        delete inst;
    } catch (...) {
    }
}

// ---- exploration snapshots

/// What the game says the player explored. Without readable exploration data only the
/// current room is revealed: the map must never show what the player has not explored.
std::shared_ptr<bc_mapdraw::Exploration> BuildExploration(const bc_live::State& s) {
    auto ex = std::make_shared<bc_mapdraw::Exploration>();
    ex->known = true;
    ex->last_save_room = s.last_save_room;
    if (s.fog_ok && s.fog_entries > 0) {
        for (const auto& r : s.fog) {
            auto& room = ex->rooms[r.key];
            room.zones.insert(r.entered_zones.begin(), r.entered_zones.end());
            room.zdoors.insert(r.entered_zdoors.begin(), r.entered_zdoors.end());
            room.zdoor_zones.insert(r.discovered_zdoor_zones.begin(), r.discovered_zdoor_zones.end());
            room.secrets.insert(r.discovered_secrets.begin(), r.discovered_secrets.end());
            room.activated.insert(r.activated.begin(), r.activated.end());
        }
    } else if (s.room_ok && !s.room_id.empty()) {
        ex->current_room_only = s.room_id;
    }
    for (const auto& m : s.markers)
        ex->markers.push_back({m.level_guid, m.type_guid, m.x, m.y});
    if (s.death.known) {
        ex->has_death = true;
        ex->death_level_guid = s.death.level_guid;
        // In the fast-travel room the game puts the spot on that room's Z-door; the main-map
        // position at death is where the player left the map for it.
        ex->death_x = s.death.in_fast_travel_room ? s.death.main_x : s.death.x;
        ex->death_y = s.death.in_fast_travel_room ? s.death.main_y : s.death.y;
    }
    return ex;
}

std::shared_ptr<bc_render::Fog> BuildFog(const bc_live::State& s, const bc_world::World* world) {
    auto f = std::make_shared<bc_render::Fog>();
    f->known = true;
    if (s.fog_ok && s.fog_matched > 0) {
        for (const auto& r : s.fog) {
            if (r.uid >= 0)
                f->by_uid[r.uid] = r.states;
        }
    } else if (world && s.room_ok) {
        if (const auto* b = world->FindWorld(s.world_name)) {
            if (const auto* room = b->FindUid(s.room_uid))
                f->by_uid[room->uid] = std::vector<u8>(room->chunks.size(), 3);
        }
    }
    return f;
}

u64 ExploreId(const bc_live::State& s, bool world_ready, bool legacy) {
    // The level map changes only with zones (not with chunk states); the fallback map needs both.
    if (s.fog_ok && s.fog_entries > 0)
        return legacy ? s.fog_hash ^ (s.fog_matched > 0 ? 0 : 0x5A5A) : s.explore_hash;
    return 0x5EED000000000000ULL ^ (world_ready ? 1ULL << 40 : 0) ^ static_cast<u64>(static_cast<u32>(s.room_uid));
}

void PublishDebug(const EdenDsmodHostApi* h, const bc_live::State& s) {
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
    std::string keys, cur;
    int shown = 0;
    std::size_t zones = 0;
    for (const auto& r : s.fog) {
        zones += r.entered_zones.size();
        if (shown < 3) {
            keys += (shown ? " , " : "") + r.key;
            ++shown;
        }
        if (r.key == s.room_id && !s.room_id.empty()) {
            cur = "zones";
            for (const s32 z : r.entered_zones)
                cur += " " + std::to_string(z);
            cur += " doors";
            for (const s32 z : r.entered_zdoors)
                cur += " " + std::to_string(z);
        }
    }
    PublishI(h, "bc.fog.zones", static_cast<s64>(zones));
    PublishT(h, "bc.fog.keys", keys);
    PublishT(h, "bc.fog.cur", cur);
    PublishT(h, "bc.save", s.last_save_room);
    PublishT(h, "bc.level", s.level_guid);
    PublishI(h, "bc.markers.ok", s.markers_ok);
    PublishI(h, "bc.markers.n", static_cast<s64>(s.markers.size()));
    std::string death = "-";
    if (s.death.known) {
        char b[96];
        std::snprintf(b, sizeof b, "%.1f %.1f%s %s", s.death.x, s.death.y, s.death.in_fast_travel_room ? " FT" : "",
                      s.death.level_guid == s.level_guid ? "here" : "other map");
        death = b;
    }
    PublishT(h, "bc.death", death);
    PublishT(h, "bc.marks", (s.markers_ok ? std::to_string(s.markers.size()) : std::string("-")) + "  " + death +
                                "  | died " + (s.death_flag_ok ? std::to_string(s.just_died) : std::string("-")) +
                                " ctx " + (s.ctx_ok ? std::to_string(s.ctx_paused) : std::string("-")) + " state " +
                                (s.gs_ok ? (std::to_string(s.gs_pause) + std::to_string(s.gs_pause_playtime) + " " + s.gs_maps)
                                         : std::string("-")) +
                                (s.paused ? " PAUSED" : ""));
}

std::string MapKey(const Window& w) {
    char key[200];
    if (w.legacy)
        std::snprintf(key, sizeof key, "module:bc:map:2:%d:%d:%d:%d:%llx", w.biome, w.zoom, w.cx, w.cy,
                      static_cast<unsigned long long>(w.explore));
    else
        std::snprintf(key, sizeof key, "module:bc:lm:4:%d:%d:%d:%d:%llx:%llx", w.biome, w.zoom, w.cx, w.cy,
                      static_cast<unsigned long long>(w.explore), static_cast<unsigned long long>(w.pins));
    return key;
}

/// The pins on screen, saved by revision for the renders that ask for it.
std::shared_ptr<const std::vector<Pin>> PinSnapshot(Instance* inst, u64 rev) {
    std::lock_guard lock(inst->pin_mutex);
    if (auto p = Instance::Lookup(inst->pin_snapshots, rev))
        return *p;
    auto v = std::make_shared<std::vector<Pin>>();
    for (const Pin& p : inst->pins)
        if (p.biome >= 0)
            v->push_back(p);
    Instance::Remember(inst->pin_snapshots, rev, std::shared_ptr<const std::vector<Pin>>(v), 8);
    return v;
}

/// The inventory, level and stats, as text for the inventory debug page.
void PublishInventory(Instance* inst, const EdenDsmodHostApi* h, const bc_live::State& s) {
    std::shared_ptr<const bc_levelmap::Names> names;
    {
        std::lock_guard lock(inst->world_mutex);
        names = inst->names;
    }
    const auto& inv = s.inv;
    const auto name = [&](const bc_live::Item& it) -> std::string {
        if (!it.ptr)
            return "-";
        std::string n = names ? names->strings.Get(it.name_key) : std::string{};
        return n.empty() ? it.id : n;
    };
    PublishI(h, "bc.inv.ok", inv.ok);
    char b[160];
    std::snprintf(b, sizeof b, "LVL %.0f  XP %.0f / %.0f%s", inv.level, inv.xp, inv.xp_max, inv.level_ok ? "" : "  (?)");
    PublishT(h, "bc.inv.level", b);
    PublishT(h, "bc.inv.eq", "Weapon: " + name(inv.weapon) + "  |  Arcana: " + name(inv.spell) + "  |  Skin: " + name(inv.skin));
    std::string relics;
    for (const auto& c : inv.charms)
        relics += (relics.empty() ? "" : ", ") + name(c);
    PublishT(h, "bc.inv.relics", relics.empty() ? "-" : relics);
    // Owned items by type.
    static const char* const kKeys[] = {"", "bc.inv.t1", "bc.inv.t2", "bc.inv.t3", "bc.inv.t4", "bc.inv.t5", "bc.inv.t6", "bc.inv.t7"};
    std::string lists[8];
    int counts[8] = {};
    for (const auto& it : inv.items) {
        const int t = it.type >= 1 && it.type <= 7 ? it.type : 0;
        ++counts[t];
        if (lists[t].size() < 600)
            lists[t] += (lists[t].empty() ? "" : ", ") + name(it);
    }
    for (int t = 1; t <= 7; ++t)
        PublishT(h, kKeys[t], std::to_string(counts[t]) + ": " + lists[t]);
    std::string stats;
    for (const auto& st : inv.stats) {
        char sb[96];
        std::snprintf(sb, sizeof sb, "%s %.4g%s", st.name.empty() ? "?" : st.name.c_str(), st.value,
                      st.skipped ? "*" : "");
        if (stats.size() < 900)
            stats += (stats.empty() ? "" : ", ") + std::string(sb);
    }
    PublishT(h, "bc.inv.stats", std::to_string(inv.stats.size()) + ": " + stats);
}

void PublishPins(Instance* inst, const EdenDsmodHostApi* h) {
    std::lock_guard lock(inst->pin_mutex);
    PublishT(h, "bc.pin.store", inst->pin_store_status);
    PublishI(h, "bc.pin.mode", inst->pin_mode ? 1 : 0);
    PublishI(h, "bc.pin.plain", inst->pin_mode ? 0 : 1);
    char key[48];
    for (int t = 0; t < kPinTypes; ++t) {
        std::snprintf(key, sizeof key, "module:bc:ui:pin:%d:%d", t, t == inst->pin_type ? 1 : 0);
        char name[24];
        std::snprintf(name, sizeof name, "bc.pin.k%d", t);
        PublishT(h, name, key);
    }
    PublishT(h, "bc.pin.btn", inst->pin_mode ? "module:bc:ui:btn:pin:1" : "module:bc:ui:btn:pin:0");
    int count = 0;
    for (const Pin& p : inst->pins)
        count += p.biome >= 0;
    PublishI(h, "bc.pin.n", count);
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
        // The exploration dictionary is walked once a second. Debug values are published every
        // sample: the host clears a value that a sample does not publish.
        const bool with_fog = inst->samples % 60 == 1;
        const bool debug = true;
        bc_live::State fresh;
        inst->reader.Sample(mem, with_fog, fresh);
        if (!with_fog && inst->live.fog_ok) {
            fresh.fog_ok = inst->live.fog_ok;
            fresh.fog_entries = inst->live.fog_entries;
            fresh.fog_matched = inst->live.fog_matched;
            fresh.fog = std::move(inst->live.fog);
            fresh.fog_hash = inst->live.fog_hash;
            fresh.explore_hash = inst->live.explore_hash;
            fresh.markers_ok = inst->live.markers_ok;
            fresh.markers = std::move(inst->live.markers);
            fresh.death = std::move(inst->live.death);
            fresh.inv = std::move(inst->live.inv);
        }
        if (!with_fog)
            fresh.last_save_room = inst->live.last_save_room.empty() ? fresh.last_save_room : inst->live.last_save_room;
        inst->live = std::move(fresh);
        {
            // A world change must hold a few samples (a stray read during a transition would
            // swap the map for one frame).
            bc_live::State& l = inst->live;
            if (l.room_ok && !l.world_name.empty()) {
                if (inst->shown_world.empty() || l.world_name == inst->shown_world) {
                    inst->shown_world = l.world_name;
                    inst->world_votes = 0;
                } else if (++inst->world_votes >= kWorldSamples) {
                    inst->shown_world = l.world_name;
                    inst->world_votes = 0;
                } else {
                    l.world_name = inst->shown_world;
                    ++inst->glitches;
                }
            }
        }
        const bc_live::State& s = inst->live;
        const auto world = inst->World();
        const bool world_ready = world && !world->biomes.empty();
        bool legacy_mode;
        {
            std::lock_guard lock(inst->lm_mutex);
            legacy_mode = inst->lm_failed.count(s.world_name) != 0;
        }
        if (const u64 id = ExploreId(s, world_ready, legacy_mode); id != inst->explore_id || !inst->exploration) {
            inst->explore_id = id;
            inst->exploration = BuildExploration(s);
            inst->fog = BuildFog(s, world.get());
            std::lock_guard lock(inst->ex_mutex);
            Instance::Remember(inst->explorations, id, inst->exploration, 8);
            Instance::Remember(inst->fogs, id, inst->fog, 8);
        }
        if (debug)
            PublishDebug(h, s);
        if (debug) {
            std::lock_guard lock(inst->world_mutex);
            PublishT(h, "bc.world.log", inst->world_log);
            PublishI(h, "bc.world.done", inst->world_done);
            PublishI(h, "bc.assets.done", inst->assets_done);
        }
        PublishI(h, "bc.world.ready", world_ready);
        PublishI(h, "bc.world.biomes", world ? static_cast<s64>(world->biomes.size()) : 0);
        PublishI(h, "bc.world.rooms", world ? static_cast<s64>(world->RoomCount()) : 0);
        PublishI(h, "bc.zoom", inst->zoom);

        // The level map of the player's biome.
        int lm_state = 0;
        std::shared_ptr<const bc_levelmap::LevelMap> lm;
        const int number = bc_levelmap::BiomeNumber(s.world_name);
        if (s.room_ok && number >= 0) {
            bool assets_ready;
            {
                std::lock_guard lock(inst->world_mutex);
                assets_ready = inst->assets_done && inst->assets;
            }
            if (assets_ready)
                lm = inst->LevelMap(s.world_name, &lm_state);
        }
        PublishI(h, "bc.lm.state", lm_state);
        if (debug) {
            std::lock_guard lock(inst->lm_mutex);
            PublishT(h, "bc.lm.log", inst->lm_log);
            PublishI(h, "bc.lm.ms", static_cast<s64>(inst->lm_ms));
        }
        PublishI(h, "bc.render.ms", inst->last_render_ms.load());

        // Fallback biome (room layout) when the level map cannot be read.
        const bc_world::Biome* legacy = nullptr;
        if (!lm && lm_state == 3 && world && s.room_ok)
            legacy = world->FindWorld(s.world_name);

        // Where the player is, in world units.
        double pwx = 0, pwy = 0;
        int pos_mode = 0; // 1 exact world position, 2 chunk centre
        {
            const bool finite = std::isfinite(s.map_x) && std::isfinite(s.map_y);
            bool plausible = false;
            if (lm)
                plausible = s.map_x > lm->x0 - 64 && s.map_x < lm->x1 + 64 && s.map_y > lm->y0 - 64 &&
                            s.map_y < lm->y1 + 64;
            else if (legacy)
                plausible = s.map_x > static_cast<double>((legacy->min_cx - 2) * legacy->chunk_w) &&
                            s.map_x < static_cast<double>((legacy->max_cx + 3) * legacy->chunk_w) &&
                            s.map_y > static_cast<double>((legacy->min_cy - 2) * legacy->chunk_h) &&
                            s.map_y < static_cast<double>((legacy->max_cy + 3) * legacy->chunk_h);
            if (s.ex_ok && finite && !(s.map_x == 0 && s.map_y == 0) && plausible) {
                pwx = s.map_x;
                pwy = s.map_y;
                pos_mode = 1;
            } else if (world && s.rm_ok) {
                if (const auto* b = world->FindWorld(s.world_name)) {
                    if (const bc_world::Room* room = b->FindUid(s.room_uid);
                        room && s.chunk_x >= 0 && s.chunk_y >= 0 && s.chunk_x < room->w && s.chunk_y < room->h) {
                        pwx = (room->pos_x + s.chunk_x + 0.5) * b->chunk_w;
                        pwy = (room->pos_y + s.chunk_y + 0.5) * b->chunk_h;
                        pos_mode = 2;
                    }
                }
            }
        }
        {
            const auto now = std::chrono::steady_clock::now();
            if (pos_mode == 1) {
                bool take = true;
                if (inst->pos_ok && inst->pos_world == s.world_name &&
                    std::hypot(pwx - inst->pos_x, pwy - inst->pos_y) > kJumpUnits) {
                    // A jump: wait until the new place holds.
                    if (inst->cand_n > 0 && std::hypot(pwx - inst->cand_x, pwy - inst->cand_y) < 3.0)
                        ++inst->cand_n;
                    else
                        inst->cand_n = 1;
                    inst->cand_x = pwx, inst->cand_y = pwy;
                    take = inst->cand_n >= kJumpSamples;
                    if (!take) {
                        pwx = inst->pos_x, pwy = inst->pos_y;
                        ++inst->glitches;
                    }
                }
                if (take) {
                    inst->pos_ok = true;
                    inst->pos_world = s.world_name;
                    inst->pos_x = pwx, inst->pos_y = pwy;
                    inst->pos_time = now;
                    inst->cand_n = 0;
                }
            } else if (inst->pos_ok && inst->pos_world == s.world_name &&
                       std::chrono::duration<double>(now - inst->pos_time).count() < kHoldSeconds) {
                // A moment without a good position: stay where the player was.
                pwx = inst->pos_x, pwy = inst->pos_y;
                pos_mode = 1;
                ++inst->glitches;
            }
        }
        PublishI(h, "bc.pos.glitch", static_cast<s64>(inst->glitches));
        // No position yet (the game writes it once the player moves): the last known one in this
        // world, else the current room's chunk from the live room data.
        if (pos_mode) {
            inst->last_world = s.world_name;
            inst->last_wx = pwx, inst->last_wy = pwy;
        } else if (s.room_ok && inst->last_world == s.world_name) {
            pwx = inst->last_wx, pwy = inst->last_wy;
            pos_mode = 3;
        } else if (s.room_ok && s.rm_ok && lm) {
            // positionCoord + (0, -1) edition offset is the usual chunk origin.
            pwx = (s.room_x + s.chunk_x + 0.5) * lm->chunk_w;
            pwy = (s.room_y - 1 + s.chunk_y + 0.5) * lm->chunk_h;
            pos_mode = 4;
        }
        PublishI(h, "bc.pos.mode", pos_mode);

        // Title: the game's level and district names.
        {
            std::string title, subtitle;
            if (lm) {
                title = lm->level_name.empty() ? BiomeTitle(s.world_name) : lm->level_name;
                if (pos_mode) {
                    if (const auto* d = lm->DistrictAt(static_cast<float>(pwx), static_cast<float>(pwy)))
                        subtitle = d->name;
                }
            } else if (s.room_ok && number >= 0) {
                title = BiomeTitle(s.world_name);
            }
            PublishT(h, "bc.title", title);
            bool font = false;
            {
                std::lock_guard lock(inst->world_mutex);
                font = inst->common && !inst->common->font.Empty();
            }
            if (font && !title.empty()) {
                Title t{title, subtitle};
                {
                    std::lock_guard lock(inst->world_mutex);
                    if (inst->names) {
                        if (const auto it = inst->names->banner_colors.find(s.world_name);
                            it != inst->names->banner_colors.end())
                            t.color = it->second;
                    }
                }
                char col[48];
                std::snprintf(col, sizeof col, "|%.3f,%.3f,%.3f", t.color.x, t.color.y, t.color.z);
                const u64 id = Fnv(Fnv(Fnv(0xcbf29ce484222325ULL, title), "|" + subtitle), col);
                char key[64];
                std::snprintf(key, sizeof key, "module:bc:title:%llx", static_cast<unsigned long long>(id));
                {
                    std::lock_guard lock(inst->ex_mutex);
                    Instance::Remember(inst->titles, id, t, 16);
                }
                PublishT(h, "bc.title.key", key);
                PublishI(h, "bc.title.img", 1);
            } else {
                PublishI(h, "bc.title.img", 0);
            }
        }

        bool follow = false, area = false, marker = false;
        double mk_dx = 0, mk_dy = 0;
        const int zoom = std::clamp(inst->zoom, 0, kZoomLevels - 1);
        if ((lm || legacy) && number >= 0) {
            if (zoom > 0 && pos_mode) {
                follow = true;
                const double scale = kZoomScale[zoom];
                Window& cur = inst->cur;
                const bool same_view = cur.valid && cur.biome == number && cur.zoom == zoom && cur.legacy == !lm;
                const bool far = same_view && (std::fabs(pwx - cur.cx) * scale > kRecentreX ||
                                               std::fabs(pwy - cur.cy) * scale > kRecentreY);
                const auto now = std::chrono::steady_clock::now();
                const bool redraw_due =
                    cur.explore != inst->explore_id &&
                    std::chrono::duration<double>(now - inst->last_window).count() >= kRedrawSeconds;
                u64 pins_rev;
                {
                    std::lock_guard lock(inst->pin_mutex);
                    pins_rev = inst->pins_rev;
                }
                // A pin put or taken away shows at once.
                const bool pins_changed = same_view && lm && cur.pins != pins_rev;
                if (!same_view || far || redraw_due || pins_changed) {
                    inst->last_window = now;
                    // Keep the old window underneath while the new one loads.
                    inst->prev = same_view ? cur : Window{};
                    cur.valid = true;
                    cur.legacy = !lm;
                    cur.biome = number;
                    cur.zoom = zoom;
                    if (!same_view || far) {
                        cur.cx = static_cast<int>(std::lround(pwx));
                        cur.cy = static_cast<int>(std::lround(pwy));
                    }
                    if (!pins_changed || redraw_due || far || !same_view)
                        cur.explore = inst->explore_id;
                    cur.pins = pins_rev;
                    cur.key = MapKey(cur);
                }
                const auto offset = [&](const Window& w, double& ox, double& oy) {
                    const auto v = bc_mapdraw::CenteredView(w.cx, w.cy, scale, kWindowW, kWindowH);
                    double px, py;
                    v.WorldToPixel(pwx, pwy, px, py);
                    ox = kCanvasW / 2.0 - px;
                    oy = kCanvasH / 2.0 - py;
                };
                double ox, oy;
                offset(cur, ox, oy);
                {
                    // Canvas pixel -> world, as the window is shown (rounded offsets).
                    const auto v = bc_mapdraw::CenteredView(cur.cx, cur.cy, scale, kWindowW, kWindowH);
                    std::lock_guard lock(inst->pin_mutex);
                    inst->tap_view = v;
                    inst->tap_view.x0 = v.x0 - static_cast<double>(std::lround(ox)) / scale;
                    inst->tap_view.y1 = v.y1 + static_cast<double>(std::lround(oy)) / scale;
                    inst->tap_view.w = kCanvasW, inst->tap_view.h = kCanvasH;
                    inst->tap_view_ok = !cur.legacy;
                    inst->tap_biome = number;
                }
                PublishT(h, "bc.map.key", cur.key);
                PublishI(h, "bc.map.ox", std::lround(ox));
                PublishI(h, "bc.map.oy", std::lround(oy));
                const bool prev_ok = inst->prev.valid && inst->prev.biome == number && inst->prev.zoom == zoom;
                if (prev_ok) {
                    double pox, poy;
                    offset(inst->prev, pox, poy);
                    PublishT(h, "bc.map.pkey", inst->prev.key);
                    PublishI(h, "bc.map.pox", std::lround(pox));
                    PublishI(h, "bc.map.poy", std::lround(poy));
                }
                PublishI(h, "bc.map.pshow", prev_ok);
                marker = true;
                mk_dy = -kPlayerLift * scale;
            } else if (zoom == 0) {
                // Whole area on screen; the marker moves instead of the map.
                area = true;
                inst->cur.valid = false;
                inst->prev.valid = false;
                Window w;
                w.legacy = !lm;
                w.biome = number;
                w.zoom = 0;
                w.explore = inst->explore_id;
                {
                    std::lock_guard lock(inst->pin_mutex);
                    w.pins = inst->pins_rev;
                }
                if (lm && pos_mode) {
                    // The navigation zone the player is in (the main map, a tower, a Z-room...).
                    w.cx = lm->NavZoneAt(static_cast<float>(pwx), static_cast<float>(pwy));
                }
                {
                    std::lock_guard lock(inst->pin_mutex);
                    inst->tap_view_ok = lm != nullptr;
                    inst->tap_biome = number;
                    if (lm)
                        inst->tap_view = bc_mapdraw::FitZone(*lm, w.cx, kCanvasW, kCanvasH);
                }
                PublishT(h, "bc.map.akey", MapKey(w));
                if (pos_mode) {
                    double px = 0, py = 0;
                    if (lm) {
                        const auto v = bc_mapdraw::FitZone(*lm, w.cx, kCanvasW, kCanvasH);
                        v.WorldToPixel(pwx, pwy, px, py);
                        py -= kPlayerLift * v.scale;
                    } else {
                        bc_render::Params prm;
                        prm.zoom = 0;
                        prm.out_w = kCanvasW;
                        prm.out_h = kCanvasH;
                        const auto view = bc_render::ComputeView(*legacy, prm);
                        double tx, ty;
                        bc_render::WorldToTile(*legacy, pwx, pwy, tx, ty);
                        bc_render::TileToPixel(view, tx, ty, px, py);
                    }
                    mk_dx = px - kCanvasW / 2.0;
                    mk_dy = py - kCanvasH / 2.0;
                    marker = true;
                }
            }
        } else {
            PublishI(h, "bc.map.pshow", 0);
        }
        if (!follow && !area) {
            std::lock_guard lock(inst->pin_mutex);
            inst->tap_view_ok = false;
        }
        PublishPins(inst, h);
        PublishInventory(inst, h, s);
        {
            std::lock_guard lock(inst->pin_mutex);
            PublishT(h, "bc.savepins", s.last_save_room + "  | pins " + inst->pin_store_status);
        }
        {
            // Game over: from the moment the game marks the player dead.
            const auto now = std::chrono::steady_clock::now();
            if (s.death_flag_ok) {
                if (inst->died_known && s.just_died && !inst->prev_died) {
                    inst->game_over_at = now;
                    inst->game_over_seen = true;
                }
                inst->prev_died = s.just_died;
                inst->died_known = true;
            }
            const double since = std::chrono::duration<double>(now - inst->game_over_at).count();
            const bool game_over = inst->game_over_seen && since < kGameOverSeconds;
            const int frame = std::clamp(static_cast<int>(since * 1000.0 / kGameOverFrameMs), 0, kGameOverFrames - 1);
            char gk[48];
            std::snprintf(gk, sizeof gk, "module:bc:ui:gameover:%d", game_over ? frame : kGameOverFrames - 1);
            PublishT(h, "bc.go.key", gk);
            PublishI(h, "bc.go.show", game_over);
            // Pause: the map blurred under "Paused" while a menu or the pause screen is open.
            const bool paused = s.paused && !game_over && (follow || area);
            if (follow && inst->cur.valid && !inst->cur.legacy && inst->cur.key.rfind("module:bc:", 0) == 0)
                inst->blur_key = "module:bc:blur:" + inst->cur.key.substr(10);
            if (area && lm) {
                Window w;
                w.biome = number;
                w.zoom = 0;
                w.explore = inst->explore_id;
                if (pos_mode)
                    w.cx = lm->NavZoneAt(static_cast<float>(pwx), static_cast<float>(pwy));
                {
                    std::lock_guard lock(inst->pin_mutex);
                    w.pins = inst->pins_rev;
                }
                inst->blur_area_key = "module:bc:blur:" + MapKey(w).substr(10);
            }
            PublishT(h, "bc.map.bkey", inst->blur_key);
            PublishT(h, "bc.map.bakey", inst->blur_area_key);
            PublishI(h, "bc.paused", paused);
            PublishI(h, "bc.paused.f", paused && follow && !inst->blur_key.empty());
            PublishI(h, "bc.paused.a", paused && area && !inst->blur_area_key.empty());
        }
        {
            // The zoom scale closes by itself after a while.
            if (inst->zoom_bar &&
                std::chrono::duration<double>(std::chrono::steady_clock::now() - inst->zoom_bar_since).count() > 6.0)
                inst->zoom_bar = false;
            PublishI(h, "bc.zbar", inst->zoom_bar ? 1 : 0);
            char zb[40];
            std::snprintf(zb, sizeof zb, "module:bc:ui:zbtn:%d", zoom);
            PublishT(h, "bc.ui.zbtn", zb);
        }
        {
            bool ui_ok;
            {
                std::lock_guard lock(inst->world_mutex);
                ui_ok = inst->common && !inst->common->font.Empty() && !inst->common->ui.empty();
            }
            char zk[40];
            std::snprintf(zk, sizeof zk, "module:bc:ui:zoom:%d", zoom);
            PublishT(h, "bc.ui.zoom", zk);
            PublishI(h, "bc.ui.ok", ui_ok);
            PublishI(h, "bc.ui.plain", !ui_ok);
            PublishI(h, "bc.ui.wait", ui_ok && !(follow || area));
            PublishI(h, "bc.ui.waitplain", !ui_ok && !(follow || area));
        }
        PublishI(h, "bc.map.follow", follow);
        PublishI(h, "bc.map.area", area);
        PublishI(h, "bc.map.ready", follow || area);
        PublishI(h, "bc.map.loading", lm_state == 1 && !follow && !area);
        // One marker widget per zoom level (their sizes differ).
        PublishI(h, "bc.mk.show", marker);
        PublishI(h, "bc.mk.z0", marker && zoom == 0);
        PublishI(h, "bc.mk.z1", marker && zoom == 1);
        PublishI(h, "bc.mk.z2", marker && zoom == 2);
        PublishI(h, "bc.mk.z3", marker && zoom == 3);
        PublishI(h, "bc.mk.z4", marker && zoom == 4);
        PublishI(h, "bc.mk.dx", std::lround(mk_dx));
        PublishI(h, "bc.mk.dy", std::lround(mk_dy));

        const auto t1 = std::chrono::steady_clock::now();
        const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        inst->cost_us = inst->cost_us * 0.95 + us * 0.05;
        PublishI(h, "bc.us", static_cast<s64>(inst->cost_us));
    } catch (...) {
        // Never let an exception cross the C ABI; the next sample starts fresh.
    }
}

void Tick(void*, const EdenDsmodHostApi*) {}

void Configure(void* p, const EdenDsmodHostExtensions* host) {
    auto* inst = static_cast<Instance*>(p);
    if (!inst)
        return;
    {
        std::lock_guard lock(inst->codec_mutex);
        if (host && host->struct_size >= sizeof(EdenDsmodHostExtensions)) {
            inst->astc = host->decode_astc;
            inst->astc_user = host->userdata;
        }
        inst->configured = true;
    }
    inst->codec_cv.notify_all();
}

/// Where the pins are kept: the emulator's own data folder, beside the runtime's persisted
/// flags (on Android the app's private files folder, found from the process name; elsewhere the
/// XDG data folder). Created when missing; "" when there is none.
std::string PinStorePath() {
    std::string base;
#if defined(__ANDROID__)
    if (std::FILE* f = std::fopen("/proc/self/cmdline", "rb")) {
        char buf[256] = {};
        const std::size_t n = std::fread(buf, 1, sizeof buf - 1, f);
        std::fclose(f);
        std::string pkg(buf, strnlen(buf, n));
        if (const auto colon = pkg.find(':'); colon != std::string::npos)
            pkg.resize(colon);
        if (!pkg.empty() && pkg.find('/') == std::string::npos)
            base = "/data/data/" + pkg + "/files";
    }
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        base = std::string(xdg) + "/eden";
    else if (const char* home = std::getenv("HOME"); home && *home)
        base = std::string(home) + "/.local/share/eden";
#endif
    if (base.empty())
        return {};
    std::string dir = base;
    for (const char* part : {"/dualscreen", "/persist", "/0100BA4026E94000"}) {
        dir += part;
        ::mkdir(dir.c_str(), 0700); // fails harmlessly when it exists
    }
    struct stat st {};
    if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return {};
    return dir + "/" + kPinFile;
}

/// Reads the pins (one encoded pin per line). Under pin_mutex.
void LoadPins(Instance* inst) {
    inst->pin_store = PinStorePath();
    if (inst->pin_store.empty()) {
        inst->pin_store_status = "no folder";
        return;
    }
    std::FILE* f = std::fopen(inst->pin_store.c_str(), "rb");
    if (!f) {
        inst->pin_store_status = "new";
        return;
    }
    char line[64];
    int i = 0;
    while (i < kPinSlots && std::fgets(line, sizeof line, f)) {
        const Pin p = DecodePin(std::strtoll(line, nullptr, 10));
        if (p.biome >= 0)
            inst->pins[static_cast<std::size_t>(i++)] = p;
    }
    std::fclose(f);
    inst->pin_store_status = "read " + std::to_string(i);
}

/// Writes the pins (a new file renamed over the old one). Under pin_mutex.
void SavePins(Instance* inst) {
    if (inst->pin_store.empty())
        return;
    const std::string tmp = inst->pin_store + ".new";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        inst->pin_store_status = "cannot write";
        return;
    }
    int n = 0;
    for (const Pin& p : inst->pins) {
        if (p.biome >= 0) {
            std::fprintf(f, "%lld\n", static_cast<long long>(EncodePin(p)));
            ++n;
        }
    }
    const bool ok = std::fflush(f) == 0;
    std::fclose(f);
    if (!ok || std::rename(tmp.c_str(), inst->pin_store.c_str()) != 0) {
        inst->pin_store_status = "cannot write";
        return;
    }
    inst->pin_store_status = "saved " + std::to_string(n);
}

/// A tap on the map in pin mode: `packed` is the tap point on the canvas (x high, y low 32
/// bits). Near a pin: a pin of another symbol takes the selected one, a pin of the same symbol
/// goes away. Elsewhere: a new pin.
bool PinTap(Instance* inst, int64_t packed) {
    const double cx = static_cast<double>(static_cast<std::int32_t>(static_cast<u64>(packed) >> 32));
    const double cy = static_cast<double>(static_cast<std::int32_t>(static_cast<u64>(packed) & 0xFFFFFFFFu));
    std::lock_guard lock(inst->pin_mutex);
    if (!inst->pin_mode || !inst->tap_view_ok || inst->tap_biome < 0)
        return false;
    const auto& v = inst->tap_view;
    const double wx = v.x0 + cx / v.scale, wy = v.y1 - cy / v.scale;
    // The pin icons are drawn centred on their point.
    int best = -1;
    double best_d = kPinTouchPx;
    for (int i = 0; i < kPinSlots; ++i) {
        const Pin& p = inst->pins[static_cast<std::size_t>(i)];
        if (p.biome != inst->tap_biome)
            continue;
        double px, py;
        v.WorldToPixel(p.x, p.y, px, py);
        const double d = std::hypot(px - cx, py - cy);
        if (d < best_d)
            best_d = d, best = i;
    }
    if (best >= 0) {
        Pin& p = inst->pins[static_cast<std::size_t>(best)];
        if (p.type != inst->pin_type)
            p.type = inst->pin_type;
        else
            p = Pin{};
    } else {
        int slot = -1;
        for (int i = 0; i < kPinSlots && slot < 0; ++i)
            if (inst->pins[static_cast<std::size_t>(i)].biome < 0)
                slot = i;
        if (slot < 0)
            return false; // full: the refused haptic says so
        Pin& p = inst->pins[static_cast<std::size_t>(slot)];
        p.biome = inst->tap_biome;
        p.type = inst->pin_type;
        p.x = static_cast<float>(wx);
        p.y = static_cast<float>(wy);
    }
    ++inst->pins_rev;
    SavePins(inst);
    return true;
}

EdenDsmodBool OnAction(void* p, const char* action, int64_t argument) {
    auto* inst = static_cast<Instance*>(p);
    if (!inst || !action)
        return EDEN_DSMOD_FALSE;
    try {
        const std::string a = action;
        // Close -> medium -> whole area -> close.
        const auto keep_bar = [inst] { inst->zoom_bar_since = std::chrono::steady_clock::now(); };
        if (a == "zoom") {
            inst->zoom = inst->zoom == 0 ? kZoomLevels - 1 : inst->zoom - 1;
        } else if (a == "zoom_in") {
            inst->zoom = std::min(inst->zoom + 1, kZoomLevels - 1);
            keep_bar();
        } else if (a == "zoom_out") {
            inst->zoom = std::max(inst->zoom - 1, 0);
            keep_bar();
        } else if (a == "zoom_set") {
            inst->zoom = static_cast<int>(std::clamp<int64_t>(argument, 0, kZoomLevels - 1));
            inst->zoom_bar = false;
        } else if (a == "zbar_open") {
            inst->zoom_bar = true;
            keep_bar();
        } else if (a == "zbar_close") {
            inst->zoom_bar = false;
        } else if (a == "pin_mode") {
            std::lock_guard lock(inst->pin_mutex);
            inst->pin_mode = !inst->pin_mode;
        } else if (a == "pin_type") {
            std::lock_guard lock(inst->pin_mutex);
            inst->pin_type = static_cast<int>(std::clamp<int64_t>(argument, 0, kPinTypes - 1));
        } else if (a == "pin_tap") {
            return PinTap(inst, argument) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        } else {
            return EDEN_DSMOD_FALSE;
        }
        return EDEN_DSMOD_TRUE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

bool LoadPlayer(Instance* inst, int size, void* receiver, EdenDsmodImageSink sink) {
    std::shared_ptr<const bc_levelmap::Common> common;
    {
        std::unique_lock lock(inst->world_mutex);
        inst->world_cv.wait_for(lock, std::chrono::seconds(20), [inst] { return inst->assets_done; });
        common = inst->common;
    }
    std::vector<u8> px;
    if (common && !common->player.tex.Empty())
        px = bc_mapdraw::RenderSprite(common->player, size, size);
    else
        px = bc_render::RenderPlayerIcon(size);
    sink(receiver, static_cast<u32>(size), static_cast<u32>(size), px.data(), px.size());
    return true;
}

bool LoadUi(Instance* inst, const std::string& what, void* receiver, EdenDsmodImageSink sink) {
    std::shared_ptr<const bc_levelmap::Common> common;
    {
        std::unique_lock lock(inst->world_mutex);
        inst->world_cv.wait_for(lock, std::chrono::seconds(20), [inst] { return inst->assets_done; });
        common = inst->common;
    }
    if (!common || common->font.Empty())
        return false;
    std::vector<u8> px;
    int w = 0, h = 0;
    int zoom = 0;
    if (what == "frame") {
        w = kCanvasW, h = kCanvasH;
        px = bc_ui::Frame(*common, w, h);
    } else if (std::sscanf(what.c_str(), "zoom:%d", &zoom) == 1) {
        w = kZoomBarW, h = kZoomBarH;
        px = bc_ui::ZoomBar(*common, zoom, w, h);
    } else if (std::sscanf(what.c_str(), "zbtn:%d", &zoom) == 1) {
        w = kZoomButtonW, h = kZoomButtonH;
        px = bc_ui::ZoomButton(*common, zoom, w, h);
    } else if (what == "btn:debug" || what == "btn:map") {
        w = kButtonW, h = kButtonH;
        px = bc_ui::Button(*common, what == "btn:debug" ? "Debug" : "Map", w, h);
    } else if (what == "btn:pin:0" || what == "btn:pin:1") {
        w = kButtonW, h = kButtonH;
        px = bc_ui::Button(*common, "Pin", w, h, what == "btn:pin:1");
    } else if (int t = 0, sel = 0; std::sscanf(what.c_str(), "pin:%d:%d", &t, &sel) == 2) {
        if (t < 0 || t >= kPinTypes)
            return false;
        w = h = kPinCell;
        px = bc_ui::IconCell(*common, kPinSprites[t], sel != 0, w, h);
    } else if (what == "wait") {
        w = kLabelW, h = kLabelH;
        px = bc_ui::Label(*common, "Waiting for the game", 40, w, h);
    } else if (int f = 0; std::sscanf(what.c_str(), "gameover:%d", &f) == 1) {
        w = kGameOverW, h = kGameOverH;
        px = bc_ui::GameOver(*common, kGameOverGlow[std::clamp(f, 0, kGameOverFrames - 1)], w, h);
    } else if (what == "menuart") {
        w = kCanvasW, h = kCanvasH;
        px = bc_ui::MenuArt(*common, w, h);
        if (px.empty())
            return false;
    } else if (what == "paused") {
        w = kPausedW, h = kPausedH;
        px = bc_ui::Paused(*common, w, h);
    } else if (what == "prompt") {
        w = kPromptW, h = kPromptH;
        px = bc_ui::Prompt(*common, "Press", "A", "to confirm", 30, w, h);
    } else if (what == "version") {
        w = kVersionW, h = kVersionH;
        px = bc_ui::Label(*common, std::string("Dual Screen Mod ") + kVersion, 26, w, h, 1, {0.62f, 0.58f, 0.52f, 1});
    } else if (what == "pinhint") {
        w = kLabelW, h = kLabelH;
        px = bc_ui::Label(*common, "Tap the map to place a pin", 34, w, h);
    } else if (what == "debug") {
        w = kLabelW, h = kLabelH;
        px = bc_ui::Label(*common, "Debug", 48, w, h);
    } else {
        return false;
    }
    sink(receiver, static_cast<u32>(w), static_cast<u32>(h), px.data(), px.size());
    return true;
}

bool LoadTitle(Instance* inst, u64 id, void* receiver, EdenDsmodImageSink sink) {
    std::optional<Title> t;
    {
        std::lock_guard lock(inst->ex_mutex);
        t = Instance::Lookup(inst->titles, id);
    }
    std::shared_ptr<const bc_levelmap::Common> common;
    {
        std::lock_guard lock(inst->world_mutex);
        common = inst->common;
    }
    if (!t || !common || common->font.Empty())
        return false;
    const auto px = bc_ui::TitleBanner(*common, t->title, t->subtitle, t->color, kTitleW, kTitleH);
    sink(receiver, kTitleW, kTitleH, px.data(), px.size());
    return true;
}

/// The death spot and the player's markers on this map, as the map screen places them: both
/// prefabs (ui-gamemenus) draw their icon under a "DisplayParent" scaled 5x, the death icon
/// raised by 0.25 inside it.
std::vector<bc_mapdraw::ExtraIcon> MapExtras(const bc_levelmap::LevelMap& lm, const bc_levelmap::Common& c,
                                             const bc_mapdraw::Exploration& ex, const std::vector<Pin>& pins) {
    constexpr float kItemScale = 5.0f, kDeathRaise = 0.25f;
    std::vector<bc_mapdraw::ExtraIcon> out;
    const auto add = [&](const std::string& sprite, float x, float y) {
        const auto it = c.ui.find(sprite);
        if (it == c.ui.end())
            return;
        bc_mapdraw::ExtraIcon e;
        e.sprite = &it->second;
        e.icon.sprite = sprite;
        e.icon.x = x, e.icon.y = y;
        e.icon.sx = e.icon.sy = kItemScale;
        out.push_back(std::move(e));
    };
    if (ex.has_death && ex.death_level_guid == lm.level_guid)
        add("Map_DeathLocationIcon", ex.death_x, ex.death_y + kDeathRaise * kItemScale);
    for (const auto& m : ex.markers) {
        if (m.level_guid != lm.level_guid)
            continue;
        if (const auto t = c.marker_types.find(m.type_guid); t != c.marker_types.end())
            add(t->second, m.x, m.y);
    }
    // The page's own pins, over the game's markers.
    for (const Pin& p : pins) {
        if (p.biome == lm.number && p.type >= 0 && p.type < kPinTypes)
            add(kPinSprites[p.type], p.x, p.y);
    }
    return out;
}

/// Two passes of a (2r+1)-wide box blur over straight RGBA8 (a soft, near-gaussian blur).
void BoxBlur(std::vector<u8>& px, int w, int h, int r) {
    std::vector<u8> tmp(px.size());
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int acc[4] = {0, 0, 0, 0}, n = 0;
                for (int k = -r; k <= r; ++k) {
                    const int xx = std::clamp(x + k, 0, w - 1);
                    const u8* p = &px[(static_cast<std::size_t>(y) * w + xx) * 4];
                    for (int c = 0; c < 4; ++c)
                        acc[c] += p[c];
                    ++n;
                }
                for (int c = 0; c < 4; ++c)
                    tmp[(static_cast<std::size_t>(y) * w + x) * 4 + c] = static_cast<u8>(acc[c] / n);
            }
        }
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int acc[4] = {0, 0, 0, 0}, n = 0;
                for (int k = -r; k <= r; ++k) {
                    const int yy = std::clamp(y + k, 0, h - 1);
                    const u8* p = &tmp[(static_cast<std::size_t>(yy) * w + x) * 4];
                    for (int c = 0; c < 4; ++c)
                        acc[c] += p[c];
                    ++n;
                }
                for (int c = 0; c < 4; ++c)
                    px[(static_cast<std::size_t>(y) * w + x) * 4 + c] = static_cast<u8>(acc[c] / n);
            }
        }
    }
}

bool LoadLevelMapImage(Instance* inst, int number, int zoom, int cx, int cy, u64 explore, u64 pins_rev,
                       void* receiver, EdenDsmodImageSink sink, bool blur = false) {
    const auto lm = inst->WaitLevelMap(number, 30);
    if (!lm)
        return false;
    std::shared_ptr<const bc_mapdraw::Exploration> ex;
    {
        std::lock_guard lock(inst->ex_mutex);
        if (auto e = Instance::Lookup(inst->explorations, explore))
            ex = *e;
    }
    if (!ex) {
        auto empty = std::make_shared<bc_mapdraw::Exploration>();
        empty->known = true; // unknown snapshot: reveal nothing rather than everything
        ex = empty;
    }
    std::shared_ptr<const bc_levelmap::Common> common;
    {
        std::lock_guard lock(inst->world_mutex);
        common = inst->common;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto reveal = bc_mapdraw::BuildReveal(*lm, *ex);
    bc_mapdraw::RenderOptions opt;
    opt.threads = kRenderThreads;
    if (common) {
        const auto pins = PinSnapshot(inst, pins_rev);
        opt.extra = MapExtras(*lm, *common, *ex, *pins);
    }
    std::vector<u8> px;
    int w, h;
    // Blurred (pause screen): the same view drawn kBlurDiv times smaller and softened; the page
    // stretches it back over the sharp one.
    const int div = blur ? kBlurDiv : 1;
    if (zoom == 0) {
        w = kCanvasW / div;
        h = kCanvasH / div;
        opt.min_icon_px = 22.0f / static_cast<float>(div);
        auto v = bc_mapdraw::FitZone(*lm, cx, kCanvasW, kCanvasH);
        v.scale /= div;
        v.w = w, v.h = h;
        px = bc_mapdraw::Render(*lm, *reveal, v, opt);
    } else {
        w = kWindowW / div;
        h = kWindowH / div;
        const double scale = kZoomScale[std::clamp(zoom, 1, kZoomLevels - 1)];
        px = bc_mapdraw::Render(*lm, *reveal, bc_mapdraw::CenteredView(cx, cy, scale / div, w, h), opt);
    }
    if (blur)
        BoxBlur(px, w, h, 2);
    inst->last_render_ms = static_cast<int>(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    sink(receiver, static_cast<u32>(w), static_cast<u32>(h), px.data(), px.size());
    return true;
}

bool LoadLegacyImage(Instance* inst, int number, int zoom, int cx, int cy, u64 explore, void* receiver,
                     EdenDsmodImageSink sink) {
    std::shared_ptr<const bc_world::World> world;
    {
        std::unique_lock lock(inst->world_mutex);
        inst->world_cv.wait_for(lock, std::chrono::seconds(5), [inst] { return inst->world_done; });
        world = inst->world;
    }
    if (!world)
        return false;
    const bc_world::Biome* biome = nullptr;
    for (const auto& b : world->biomes) {
        if (bc_levelmap::BiomeNumber(b.world_name) == number)
            biome = &b;
    }
    if (!biome)
        return false;
    std::shared_ptr<const bc_render::Fog> fog;
    {
        std::lock_guard lock(inst->ex_mutex);
        if (auto f = Instance::Lookup(inst->fogs, explore))
            fog = *f;
    }
    if (!fog) {
        auto empty = std::make_shared<bc_render::Fog>();
        empty->known = true;
        fog = empty;
    }
    bc_render::Params prm;
    prm.zoom = std::clamp(zoom, 0, bc_render::kZoomLevels - 1);
    // Window centre: world units -> biome tiles.
    prm.center_x = cx - static_cast<double>(biome->min_cx) * biome->chunk_w;
    prm.center_y = cy - static_cast<double>(biome->min_cy) * biome->chunk_h;
    prm.out_w = prm.zoom == 0 ? kCanvasW : kWindowW;
    prm.out_h = prm.zoom == 0 ? kCanvasH : kWindowH;
    const auto pixels = bc_render::Render(*biome, *fog, prm);
    sink(receiver, static_cast<u32>(prm.out_w), static_cast<u32>(prm.out_h), pixels.data(), pixels.size());
    return true;
}

EdenDsmodBool LoadImage(void* p, const EdenDsmodHostApi*, const char* key, void* receiver,
                        EdenDsmodImageSink sink) {
    auto* inst = static_cast<Instance*>(p);
    if (!inst || !key || !sink)
        return EDEN_DSMOD_FALSE;
    try {
        int size = 0;
        if (std::sscanf(key, "module:bc:player:%d", &size) == 1 || std::sscanf(key, "module:bc:icon:player:%d", &size) == 1) {
            if (size < 8 || size > 256)
                return EDEN_DSMOD_FALSE;
            return LoadPlayer(inst, size, receiver, sink) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        }
        unsigned long long id = 0;
        if (std::strncmp(key, "module:bc:ui:", 13) == 0)
            return LoadUi(inst, key + 13, receiver, sink) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        if (std::sscanf(key, "module:bc:title:%llx", &id) == 1)
            return LoadTitle(inst, id, receiver, sink) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        int number = 0, zoom = 0, cx = 0, cy = 0;
        unsigned long long pins = 0;
        if (std::sscanf(key, "module:bc:blur:lm:4:%d:%d:%d:%d:%llx:%llx", &number, &zoom, &cx, &cy, &id, &pins) == 6)
            return LoadLevelMapImage(inst, number, zoom, cx, cy, id, pins, receiver, sink, true) ? EDEN_DSMOD_TRUE
                                                                                               : EDEN_DSMOD_FALSE;
        if (std::sscanf(key, "module:bc:lm:4:%d:%d:%d:%d:%llx:%llx", &number, &zoom, &cx, &cy, &id, &pins) == 6)
            return LoadLevelMapImage(inst, number, zoom, cx, cy, id, pins, receiver, sink) ? EDEN_DSMOD_TRUE
                                                                                         : EDEN_DSMOD_FALSE;
        if (std::sscanf(key, "module:bc:map:2:%d:%d:%d:%d:%llx", &number, &zoom, &cx, &cy, &id) == 5)
            return LoadLegacyImage(inst, number, zoom, cx, cy, id, receiver, sink) ? EDEN_DSMOD_TRUE
                                                                                 : EDEN_DSMOD_FALSE;
        return EDEN_DSMOD_FALSE;
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
