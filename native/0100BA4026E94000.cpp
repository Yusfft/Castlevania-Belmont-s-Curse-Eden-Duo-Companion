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
#include <random>
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

#include "bc_cards.h"
#include "bc_levelmap.h"
#include "bc_live.h"
#include "bc_mapdraw.h"
#include "bc_facts.h"
#include "bc_menupage.h"
#include "bc_render.h"
#include "bc_ui.h"
#include "bc_world.h"
#include "core/mods/modules/dsmod_module_sdk.h"

namespace {

using namespace dsmod_sdk::int_types;

constexpr u64 kTitleId = 0x0100BA4026E94000ULL;
constexpr char kBuildHex[] = "0F1169AA7D0FA4529C861F96ADEC306E"
                             "00000000000000000000000000000000";
constexpr const char* kVersion = "0.9.2";

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
// Pixels per world unit at each zoom level, farthest first. (The whole-area view and the
// 5 px/unit one were dropped in 0.6.1: too small to read on the second screen.)
constexpr double kZoomScale[] = {8.0, 13.0, 20.0};
constexpr int kZoomLevels = bc_ui::ZoomBarLayout::kLevels;
static_assert(sizeof(kZoomScale) / sizeof(kZoomScale[0]) == kZoomLevels);
constexpr int kZoomButtonW = 230, kZoomButtonH = 64;
constexpr int kPinCell = 84;
// Pins are the game's own map markers (they show on the game's map too and are saved with the
// game): three of its marker symbols are offered. A tap this close to a marker touches it.
constexpr const char* kPinSprites[] = {"Map_MarkerSet_Cross", "Map_MarkerSet_Chest", "Map_MarkerSet_Eye"};
constexpr int kPinTypes = 3;
constexpr int kPinTools = kPinTypes;
constexpr double kPinTouchPx = 52.0; // a tap this close to a marker takes it away
// Dragging the map moves the view off the player; it glides back after kPanIdleSeconds without
// a drag (or at once with the player button).
constexpr double kPanIdleSeconds = 8.0;
constexpr double kPanHomePx = 24.0; // panned this far (canvas px): the player button shows
// The player marker: the game draws Map_PlayerIcon 5 units tall (the page sizes it per zoom
// level), 0.7 units above the player.
constexpr double kPlayerLift = 0.7;
// Map drawing runs beside the emulator: few threads, low priority, and a new window at most
// every kRedrawSeconds for exploration changes alone.
constexpr int kRenderThreads = 2;
// Position glitches: a move further than the player can go since the last good position
// (kJumpUnits plus kMaxSpeed units a second) is taken only once the new place has held, moving
// no faster than that, for kJumpHoldSeconds and kJumpSamples samples (a real teleport shows a
// moment later; a stray value never shows). A missing or implausible position keeps the last
// good one for kHoldSeconds. A change of world must also hold kWorldSamples samples.
constexpr double kJumpUnits = 3.0;
constexpr double kMaxSpeed = 45.0;
constexpr double kJumpHoldSeconds = 0.3;
constexpr int kJumpSamples = 4;
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
constexpr double kTitleFallbackSeconds = 45.0; // title page without the title screen recognised
// The loading page: the biome's card, and "Loading, be patient" with 1-2-3 dots under it (a new
// dot every kLoadingDotMs). The game's loading state shows it once it has held
// kLoadingHoldSeconds, so a quick room change does not flash it.
constexpr int kLoadingW = 900, kLoadingH = 80;
// A Castlevania fact under the biome's card while loading (bc_facts.h), once the game's menu
// (whose art and info box it is drawn with) is read.
constexpr int kFactW = 1180, kFactH = 430;
constexpr int kLoadingDotMs = 420;
constexpr double kLoadingHoldSeconds = 0.4;
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

/// The bundles the game menu page reads (bc_menupage), as named in this build.
const std::vector<std::string>& MenuBundles() {
    static const std::vector<std::string> names = {
#include "bc_menu_bundles.inc"
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

struct Title {
    std::string title, subtitle;
    bc_shade::Vec4 color{0.22f, 0.35f, 0.26f, 1.0f};
};

/// A request to the guest helper (mailbox layout and commands: native/helper/bc_guest_helper.S).
struct HelperRequest {
    u32 cmd = 0;
    u64 target = 0, o1 = 0, o2 = 0, o3 = 0;
    u64 c1 = 0; // the class object 1 must have (through its parents); 0 = its own class
    s32 i = 0;
    float x = 0, y = 0;
    std::string done_msg; // shown on the equipment page when it is done ("" = none)
    int retries = 0;
    bool refresh = false; // the weapon equipped again (see Instance::wref_at)
};
enum : u32 {
    kCmdEquipWeapon = 1, kCmdEquipSpell, kCmdEquipCharm, kCmdUnequipCharm,
    kCmdAddMarker, kCmdRemoveMarker, kCmdChangeMarker,
};
constexpr u32 kHelperMagic = 0x31484342; // 'BCH1'

/// A cell of the equipment page: an item (or slot) name, a line under it, its state.
struct Cell {
    std::string title, sub;
    int state = 0; // bit 0 equipped, bit 1 selected
};

struct Instance;

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
    // Readers of `assets` after the first load (level maps, cards) take turns.
    std::mutex assets_use;
    // The title page's card, picked when the module starts.
    int title_card = 0;

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

    // Pin mode and symbol; `pins_rev` changes when the game's markers changed through the page
    // (the window on screen is redrawn at once).
    std::mutex pin_mutex;
    u64 pins_rev = 1;
    bool pin_mode = false;
    int pin_type = 0;
    // The level map on screen (marker navigation zones for taps).
    std::shared_ptr<const bc_levelmap::LevelMap> tap_lm;

    // Guest helper (the load plan's code in the game, see native/helper): its mailbox, the
    // requests waiting for it, the one it is serving. Tick thread (sample and actions).
    EdenDsmodHostExtensions hx{};
    std::atomic<bool> hx_ok{false};
    std::deque<HelperRequest> hq;
    HelperRequest h_cur;
    bool h_inflight = false, h_init = false, h_seen = false;
    u32 h_req = 0, h_frames = 0;
    std::chrono::steady_clock::time_point h_frames_at{}, h_sent_at{};
    std::string h_status = "-";
    bool force_slow = false;     // read inventory and markers on the next sample
    bool await_markers = false;  // redraw the map once the changed markers are read
    // A weapon equipped from the page is equipped once more a moment later (the game unequips
    // and equips it again), once the assets its first equip asked for have loaded: its attack
    // and attachment are then set up from loaded assets (some weapons were left without an attack).
    HelperRequest wref;
    std::chrono::steady_clock::time_point wref_at{};
    bool wref_due = false;
    // The answer to the last equipment change, shown for a few seconds (tick thread).
    std::string eq_msg;
    std::chrono::steady_clock::time_point eq_msg_at{};
    std::deque<std::pair<u64, Cell>> cells; // under ex_mutex
    // The game's equipment menu (bc_menupage), with assets of its own (the menu's bundles). It
    // is read by menu_thread once a save is loaded and drawn on the asset worker, both under
    // menu_mutex; `menu` is set once (menu_state 2) and read-only for the tick thread after that.
    std::mutex menu_mutex;
    std::shared_ptr<bc_levelmap::Assets> menu_assets;
    std::unique_ptr<bc_levelmap::Codecs> menu_codecs;
    std::unique_ptr<bc_menupage::Menu> menu;
    std::shared_ptr<const bc_levelmap::Names> menu_names;
    std::shared_ptr<const bc_levelmap::Common> menu_common;
    std::string menu_log; // under menu_mutex
    std::string menu_log_line; // its first lines, for the debug page (tick thread)
    std::thread menu_thread;
    std::atomic<int> menu_state{0}; // 0 not started, 1 loading, 2 ready, 3 failed
    std::atomic<int> menu_ms{0};    // load time, then the last draw's
    // The menu page's state (tick thread): the view (0 equipment, 1 weapons, 2 relics,
    // 3 arcanas), the relic slot picked, the item inspected, the relics page.
    int mv_view = 0, mv_slot = 0, mv_page = 0;
    std::string mv_inspect;
    // Page snapshots by id (the image key), and each drawn one's tap targets (ex_mutex).
    std::deque<std::pair<u64, bc_menupage::Snapshot>> menu_snaps;
    std::deque<std::pair<u64, std::vector<bc_menupage::Target>>> menu_targets;
    std::atomic<u64> menu_drawn{0}; // the last snapshot drawn
    u64 menu_key = 0;               // the snapshot on the page (tick thread)
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
    // `cur` is the window wanted, `shown` the newest one whose picture has been drawn (the page
    // shows it until `cur` is ready: the host would otherwise draw the old picture at the new
    // window's place for a moment), `prev` the one under it.
    Window cur, prev, shown;
    std::deque<Window> asked; // the last windows wanted, oldest first
    std::deque<std::string> map_ready; // map keys drawn (ex_mutex)
    std::string last_world;
    double last_wx = 0, last_wy = 0;
    // Glitch filter (tick thread).
    std::string shown_world;
    int world_votes = 0;
    bool pos_ok = false;
    std::string pos_world;
    double pos_x = 0, pos_y = 0, cand_x = 0, cand_y = 0;
    int cand_n = 0;
    std::chrono::steady_clock::time_point pos_time{}, cand_since{}, cand_time{};
    // The view dragged off the player (world units), when last dragged, in which world.
    double pan_wx = 0, pan_wy = 0;
    std::chrono::steady_clock::time_point pan_at{};
    std::string pan_world;
    int pan_biome = -1;
    u64 glitches = 0;
    // Game over and pause (tick thread).
    bool died_known = false, prev_died = false;
    std::chrono::steady_clock::time_point game_over_at{};
    bool game_over_seen = false;
    std::string blur_key;
    // Loading page (tick thread).
    bool game_loading = false;
    // Title page: the game's title screen seen since the last time in game; when the module
    // started (or the game was last in a room).
    bool title_seen = false;
    std::chrono::steady_clock::time_point boot_since = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point game_loading_since{};
    int loading_card = -1;
    // The fact on the loading page: a new one each time the page comes up, in a shuffled order.
    bool load_shown = false;
    int fact = -1;
    std::vector<int> fact_order;
    std::size_t fact_next = 0;
    // Debug and inventory texts, rebuilt when the slower reads run (once a second) and
    // published from the cache in between.
    std::vector<std::pair<std::string, std::string>> slow_texts;
    std::vector<std::pair<std::string, s64>> slow_ints;
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
        if (menu_thread.joinable())
            menu_thread.join();
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

    /// Reads a romfs file (the plain path, then with a leading slash).
    bc_levelmap::RomfsRead Romfs() {
        return [this](const std::string& path, std::uint64_t off, void* out, std::size_t size) -> std::size_t {
            if (!host.read_romfs)
                return 0;
            std::size_t n = host.read_romfs(host.userdata, path.c_str(), off, out, size);
            if (n == 0)
                n = host.read_romfs(host.userdata, ("/" + path).c_str(), off, out, size);
            return n;
        };
    }

    /// Reads the game's menu in the background (once).
    void StartMenu() {
        menu_state = 1;
        menu_thread = std::thread([this] {
            std::shared_ptr<const bc_levelmap::Names> n;
            std::shared_ptr<const bc_levelmap::Common> c;
            {
                std::lock_guard lock(world_mutex);
                n = names;
                c = common;
            }
            const auto t0 = std::chrono::steady_clock::now();
            std::string log;
            bool ok = false;
            try {
                if (n && c && !stopping) {
                    auto a = std::make_shared<bc_levelmap::Assets>(Romfs(), MenuBundles());
                    auto codecs = std::make_unique<bc_levelmap::Codecs>(Codecs());
                    auto m = std::make_unique<bc_menupage::Menu>(*a, *codecs);
                    std::lock_guard lock(menu_mutex);
                    ok = m->Load(*n, c->font, &log);
                    if (ok) {
                        menu_assets = std::move(a);
                        menu_codecs = std::move(codecs);
                        menu = std::move(m);
                        menu_names = n;
                        menu_common = c;
                    }
                } else {
                    log = "map assets missing\n";
                }
            } catch (...) {
                log += "menu load failed (exception)\n";
                ok = false;
            }
            {
                std::lock_guard lock(menu_mutex);
                menu_log = log;
            }
            menu_ms = static_cast<int>(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            menu_state = ok ? 2 : 3;
        });
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
                const bc_levelmap::RomfsRead rr = Romfs();
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
                std::lock_guard use(assets_use);
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
        inst->title_card = static_cast<int>(
            static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count()) / 1000 % bc_cards::kCount);
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

/// The exploration summary for the debug page (walks every explored room).
void BuildFogTexts(const bc_live::State& s, std::vector<std::pair<std::string, std::string>>& texts,
                   std::vector<std::pair<std::string, s64>>& ints) {
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
    ints.emplace_back("bc.fog.zones", static_cast<s64>(zones));
    texts.emplace_back("bc.fog.keys", std::move(keys));
    texts.emplace_back("bc.fog.cur", std::move(cur));
}

/// The inventory, level and stats, as text for the inventory debug page.
void BuildInventoryTexts(Instance* inst, const bc_live::State& s, std::vector<std::pair<std::string, std::string>>& texts,
                         std::vector<std::pair<std::string, s64>>& ints) {
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
    ints.emplace_back("bc.inv.ok", inv.ok);
    char b[160];
    std::snprintf(b, sizeof b, "LVL %.0f  XP %.0f / %.0f%s", inv.level, inv.xp, inv.xp_max, inv.level_ok ? "" : "  (?)");
    texts.emplace_back("bc.inv.level", b);
    texts.emplace_back("bc.inv.eq", "Weapon: " + name(inv.weapon) + "  |  Arcana: " + name(inv.spell) + "  |  Skin: " + name(inv.skin));
    std::string relics;
    for (const auto& c : inv.charms)
        relics += (relics.empty() ? "" : ", ") + name(c);
    texts.emplace_back("bc.inv.relics", relics.empty() ? "-" : relics);
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
        texts.emplace_back(kKeys[t], std::to_string(counts[t]) + ": " + lists[t]);
    std::string stats;
    for (const auto& st : inv.stats) {
        char sb[96];
        std::snprintf(sb, sizeof sb, "%s %.4g%s", st.name.empty() ? "?" : st.name.c_str(), st.value,
                      st.skipped ? "*" : "");
        if (stats.size() < 900)
            stats += (stats.empty() ? "" : ", ") + std::string(sb);
    }
    texts.emplace_back("bc.inv.stats", std::to_string(inv.stats.size()) + ": " + stats);
}

void PublishPins(Instance* inst, const EdenDsmodHostApi* h) {
    std::lock_guard lock(inst->pin_mutex);
    PublishI(h, "bc.pin.mode", inst->pin_mode ? 1 : 0);
    PublishI(h, "bc.pin.plain", inst->pin_mode ? 0 : 1);
    char key[48];
    for (int t = 0; t < kPinTools; ++t) {
        std::snprintf(key, sizeof key, "module:bc:ui:pin:%d:%d", t, t == inst->pin_type ? 1 : 0);
        char name[24];
        std::snprintf(name, sizeof name, "bc.pin.k%d", t);
        PublishT(h, name, key);
    }
    PublishT(h, "bc.pin.btn", inst->pin_mode ? "module:bc:ui:btn:pin:1" : "module:bc:ui:btn:pin:0");
    PublishT(h, "bc.pin.hint", "module:bc:ui:pinhint:0");
    PublishI(h, "bc.pin.n", static_cast<s64>(inst->live.markers.size()));
}

// ---- guest helper client

void EquipMessage(Instance* inst, std::string msg) {
    inst->eq_msg = std::move(msg);
    inst->eq_msg_at = std::chrono::steady_clock::now();
}

/// What a helper result means to the player ("" for done).
const char* HelperError(u32 result) {
    switch (result) {
    case 0: return "";
    case 3: return "The game was paused: try again";
    case 4: return "The game is loading: try again";
    case 5: return "That item is not in the inventory";
    case 6: return "No such relic slot";
    default: return "The game refused the change";
    }
}

/// Serves the helper: collects the answer to the request in flight, sends the next one. The
/// helper runs once a frame inside the game's ApplicationFlowManager::Update.
void HelperStep(Instance* inst, const bc_live::Memory& mem) {
    if (!inst->hx_ok) {
        inst->h_status = "off (no load plan)";
        inst->hq.clear();
        return;
    }
    const auto& hx = inst->hx;
    const auto load32 = [&](u32 off, u32& v) { return hx.load_u32 && hx.load_u32(hx.userdata, off, &v) != 0; };
    const auto load64 = [&](u32 off, u64& v) { return hx.load_u64 && hx.load_u64(hx.userdata, off, &v) != 0; };
    const auto store32 = [&](u32 off, u32 v) { return hx.store_u32 && hx.store_u32(hx.userdata, off, v) != 0; };
    const auto store64 = [&](u32 off, u64 v) { return hx.store_u64 && hx.store_u64(hx.userdata, off, v) != 0; };
    const auto now = std::chrono::steady_clock::now();
    const auto since = [&](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double>(now - t).count();
    };
    u32 frames = 0;
    if (!load32(0x04, frames)) {
        inst->h_status = "no mailbox";
        return;
    }
    if (frames != inst->h_frames) {
        if (inst->h_frames != 0 || inst->h_init)
            inst->h_seen = true;
        inst->h_frames = frames;
        inst->h_frames_at = now;
    }
    const bool alive = inst->h_seen && since(inst->h_frames_at) < 2.0;
    if (!inst->h_init) {
        u32 done = 0;
        if (!load32(0x60, done) || !store32(0x00, kHelperMagic))
            return;
        inst->h_req = done;
        inst->h_init = true;
    }
    inst->h_status = alive ? (inst->h_inflight ? "busy" : "ready") : "waiting for the game";
    if (inst->h_inflight) {
        u32 done = 0, result = 0;
        u64 value = 0;
        if (load32(0x60, done) && done == inst->h_req && load32(0x64, result) && load64(0x68, value)) {
            inst->h_inflight = false;
            HelperRequest r = std::move(inst->h_cur);
            if ((result == 3 || result == 4) && r.retries < 3) {
                ++r.retries; // paused or loading in between: once more
                inst->hq.push_front(std::move(r));
                return;
            }
            inst->force_slow = true;
            if (r.cmd >= kCmdAddMarker)
                inst->await_markers = true;
            if (result == 0) {
                if (!r.done_msg.empty())
                    EquipMessage(inst, r.done_msg);
                if (r.cmd == kCmdEquipWeapon && !r.refresh) {
                    inst->wref = r;
                    inst->wref.refresh = true;
                    inst->wref.done_msg.clear();
                    inst->wref.retries = 0;
                    inst->wref_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
                    inst->wref_due = true;
                }
            } else {
                EquipMessage(inst, HelperError(result));
            }
        } else if (since(inst->h_sent_at) > 3.0) {
            inst->h_inflight = false; // not served (the game stopped running its update)
            EquipMessage(inst, "The game did not answer");
        }
        return;
    }
    if (inst->wref_due && inst->hq.empty() && std::chrono::steady_clock::now() >= inst->wref_at) {
        inst->wref_due = false;
        // Only while that weapon is still the one equipped.
        if (inst->live.inv.ok && inst->live.inv.weapon.ptr == inst->wref.o1)
            inst->hq.push_back(inst->wref);
    }
    if (inst->hq.empty())
        return;
    if (!alive) {
        inst->hq.clear();
        EquipMessage(inst, "The game is not ready");
        return;
    }
    HelperRequest r = std::move(inst->hq.front());
    inst->hq.pop_front();
    const auto class_of = [&](u64 obj) -> u64 {
        u64 k = 0;
        return obj && mem.read(obj, &k, sizeof k) ? k : 0;
    };
    u32 token = 0;
    float x = r.x, y = r.y;
    u32 xb = 0, yb = 0;
    std::memcpy(&xb, &x, 4);
    std::memcpy(&yb, &y, 4);
    const u64 tcls = class_of(r.target);
    const u64 c1 = r.c1 ? r.c1 : class_of(r.o1), c2 = class_of(r.o2), c3 = class_of(r.o3);
    if (!tcls || !load32(0xFFC, token))
        return;
    const bool ok = store32(0x0C, r.cmd) && store32(0x10, token) && store32(0x14, static_cast<u32>(r.i)) &&
                    store32(0x18, xb) && store32(0x1C, yb) && store64(0x20, r.target) && store64(0x28, r.o1) &&
                    store64(0x30, r.o2) && store64(0x38, r.o3) && store64(0x40, tcls) && store64(0x48, c1) &&
                    store64(0x50, c2) && store64(0x58, c3);
    if (!ok)
        return;
    u32 next = inst->h_req + 1;
    if (!store32(0x08, next))
        return;
    inst->h_req = next;
    inst->h_inflight = true;
    inst->h_cur = std::move(r);
    inst->h_sent_at = now;
}

// ---- the game's equipment menu

constexpr int kEqMsgW = 560, kEqMsgH = 56;
constexpr int kTabW = 220, kTabH = 64;

std::string CellKey(Instance* inst, const Cell& c) {
    const u64 id = Fnv(Fnv(Fnv(0xcbf29ce484222325ULL, c.title), "|" + c.sub), std::to_string(c.state));
    {
        std::lock_guard lock(inst->ex_mutex);
        Instance::Remember(inst->cells, id, c, 96);
    }
    char key[48];
    std::snprintf(key, sizeof key, "module:bc:ui:cell:%llx", static_cast<unsigned long long>(id));
    return key;
}

std::string MenuKey(u64 id) {
    char key[48];
    std::snprintf(key, sizeof key, "module:bc:menu:%016llx", static_cast<unsigned long long>(id));
    return key;
}

/// What the menu page shows now: the page state and the live inventory.
bc_menupage::Snapshot MenuSnapshot(const Instance* inst, const bc_live::State& s) {
    bc_menupage::Snapshot sn;
    sn.view = inst->mv_view;
    sn.slot = inst->mv_slot;
    sn.inspect = inst->mv_inspect;
    sn.page = inst->mv_page;
    const auto& inv = s.inv;
    sn.ok = inv.ok;
    sn.weapon = inv.weapon.ptr ? inv.weapon.id : std::string{};
    sn.spell = inv.spell.ptr ? inv.spell.id : std::string{};
    for (const auto& c : inv.charms)
        sn.charms.push_back(c.ptr ? c.id : std::string{});
    for (const auto& it : inv.items)
        if (it.ptr && it.type >= 1 && it.type <= 3)
            sn.owned.push_back(it.id);
    for (const auto& st : inv.stats)
        sn.stats[st.name] = st.value;
    if (inv.level_ok) {
        sn.level = inv.level, sn.xp = inv.xp, sn.xp_max = inv.xp_max;
        sn.stats.emplace("Level", inv.level);
        sn.stats.emplace("ExperienceCurrent", inv.xp);
        sn.stats.emplace("ExperienceMax", inv.xp_max);
    }
    return sn;
}

void PublishMenu(Instance* inst, const EdenDsmodHostApi* h, const bc_live::State& s, bool ingame, bool map_ready) {
    // Read once a save is loaded and its map is up (so the map comes first).
    if (ingame && map_ready && inst->menu_state == 0)
        inst->StartMenu();
    const int st = inst->menu_state;
    const bool ok = ingame && st == 2 && s.inv.ok;
    PublishI(h, "bc.menu.state", st);
    PublishI(h, "bc.menu.ms", inst->menu_ms);
    if (st >= 2) {
        std::unique_lock lock(inst->menu_mutex, std::try_to_lock); // a draw may hold it
        if (lock.owns_lock()) {
            std::string first = inst->menu_log.substr(0, inst->menu_log.find('\n'));
            if (inst->menu_log.find('\n') != std::string::npos && inst->menu_log.find('\n') + 1 < inst->menu_log.size())
                first += " | " + inst->menu_log.substr(inst->menu_log.find('\n') + 1, 80);
            for (auto& c : first)
                if (c == '\n')
                    c = ' ';
            inst->menu_log_line = first;
        }
    }
    PublishT(h, "bc.menu.log", inst->menu_log_line);
    PublishI(h, "bc.menu.ok", ok);
    PublishI(h, "bc.menu.wait", ingame && (st <= 1 || (st == 2 && !s.inv.ok)));
    PublishI(h, "bc.menu.none", !ingame);
    PublishI(h, "bc.menu.fail", ingame && st == 3);
    PublishI(h, "bc.menu.plain", !ok);
    if (ok) {
        const auto sn = MenuSnapshot(inst, s);
        const u64 id = sn.Hash();
        {
            std::lock_guard lock(inst->ex_mutex);
            Instance::Remember(inst->menu_snaps, id, sn, 8);
        }
        inst->menu_key = id;
        PublishT(h, "bc.menu.key", MenuKey(id));
        const u64 drawn = inst->menu_drawn;
        PublishT(h, "bc.menu.pkey", drawn ? MenuKey(drawn) : "");
        PublishI(h, "bc.menu.tabs", sn.view == 0);
        PublishI(h, "bc.menu.back", sn.view != 0);
    } else {
        PublishT(h, "bc.menu.key", "");
        PublishT(h, "bc.menu.pkey", "");
        PublishI(h, "bc.menu.tabs", 0);
        PublishI(h, "bc.menu.back", 0);
    }
    // The last answer of the game, for a few seconds.
    const bool msg = !inst->eq_msg.empty() &&
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - inst->eq_msg_at).count() < 4.0;
    PublishI(h, "bc.eq.msgshow", msg);
    PublishT(h, "bc.eq.msg", msg ? CellKey(inst, {inst->eq_msg, "", 4}) : "");
    PublishI(h, "bc.eq.helper", inst->hx_ok && inst->h_seen);
}

/// An item's name for messages.
std::string MenuItemName(Instance* inst, const std::string& id) {
    std::string n;
    if (inst->menu_state == 2 && inst->menu)
        n = inst->menu->Name(id);
    return n.empty() ? id : n;
}

/// Equips an owned item through the game (the helper): a weapon, an arcana, or a relic into
/// relic slot `slot` (a relic worn in another slot moves there).
/// The game changes equipment only from its pause menu: a weapon swapped while the player acts
/// (its attachment rebuilt between the game's systems) can leave the attack button dead. The
/// page does the same: changes wait for the game to be paused.
bool EquipAllowed(Instance*) {
    return true; // any time, as before 0.9.0 (the game need not be paused)
}

bool MenuEquip(Instance* inst, const std::string& id, int slot) {
    const auto& s = inst->live;
    const auto& inv = s.inv;
    if (!inv.ok || !inv.inventory || !inst->hx_ok || !EquipAllowed(inst))
        return false;
    const bc_live::Item* item = nullptr;
    for (const auto& it : inv.items)
        if (it.ptr && it.id == id)
            item = &it;
    if (!item)
        return false;
    const std::string nm = MenuItemName(inst, id);
    HelperRequest r;
    r.target = inv.inventory;
    r.o1 = item->ptr;
    r.done_msg = "Equipped " + nm;
    if (item->type == 1) {
        if (item->ptr == inv.weapon.ptr || !s.weapon_class)
            return false;
        r.cmd = kCmdEquipWeapon;
        r.c1 = s.weapon_class;
    } else if (item->type == 2) {
        if (item->ptr == inv.spell.ptr || !s.spell_class)
            return false;
        r.cmd = kCmdEquipSpell;
        r.c1 = s.spell_class;
    } else if (item->type == 3) {
        if (!s.charm_class || slot < 0 || slot >= static_cast<int>(inv.charms.size()))
            return false;
        for (std::size_t j = 0; j < inv.charms.size(); ++j) {
            if (inv.charms[j].ptr != item->ptr)
                continue;
            if (static_cast<int>(j) == slot)
                return false; // already there
            // Worn in another slot: taken off there first.
            HelperRequest u;
            u.cmd = kCmdUnequipCharm;
            u.target = inv.inventory;
            u.i = static_cast<s32>(j);
            inst->hq.push_back(std::move(u));
        }
        r.cmd = kCmdEquipCharm;
        r.c1 = s.charm_class;
        r.i = slot;
    } else {
        return false;
    }
    inst->hq.push_back(std::move(r));
    return true;
}

bool MenuUnequipRelic(Instance* inst, int slot) {
    const auto& inv = inst->live.inv;
    if (!inv.ok || !inv.inventory || !inst->hx_ok || slot < 0 || slot >= static_cast<int>(inv.charms.size()) ||
        !inv.charms[static_cast<std::size_t>(slot)].ptr || !EquipAllowed(inst))
        return false;
    HelperRequest u;
    u.cmd = kCmdUnequipCharm;
    u.target = inv.inventory;
    u.i = slot;
    u.done_msg = "Removed " + MenuItemName(inst, inv.charms[static_cast<std::size_t>(slot)].id);
    inst->hq.push_back(std::move(u));
    return true;
}

/// A tap on the menu page (canvas point packed x high, y low): what is under it in the picture
/// on screen, the way the game's own menu answers its buttons.
bool MenuTap(Instance* inst, int64_t packed) {
    const float cx = static_cast<float>(static_cast<std::int32_t>(static_cast<u64>(packed) >> 32));
    const float cy = static_cast<float>(static_cast<std::int32_t>(static_cast<u64>(packed) & 0xFFFFFFFFu));
    std::optional<std::vector<bc_menupage::Target>> targets;
    {
        std::lock_guard lock(inst->ex_mutex);
        targets = Instance::Lookup(inst->menu_targets, inst->menu_key);
        if (!targets)
            targets = Instance::Lookup(inst->menu_targets, inst->menu_drawn.load());
    }
    if (!targets)
        return false;
    const bc_menupage::Target* hit = nullptr;
    for (const auto& t : *targets)
        if (t.Contains(cx, cy)) {
            hit = &t;
            break;
        }
    if (!hit) {
        // A finger just beside a small slot: the nearest slot within reach (the screen is small).
        constexpr float kReach = 36.0f;
        float best = kReach;
        for (const auto& t : *targets) {
            if (t.kind == bc_menupage::Target::Close || t.kind == bc_menupage::Target::Tab)
                continue;
            const float dx = std::max({t.x - cx, 0.0f, cx - (t.x + t.w)});
            const float dy = std::max({t.y - cy, 0.0f, cy - (t.y + t.h)});
            const float d = std::hypot(dx, dy);
            if (d < best)
                best = d, hit = &t;
        }
    }
    if (!hit)
        return false;
    using T = bc_menupage::Target;
    const auto& inv = inst->live.inv;
    const int slots = static_cast<int>(inv.charms.size());
    const auto charm = [&](int i) {
        return i >= 0 && i < slots && inv.charms[static_cast<std::size_t>(i)].ptr ? inv.charms[static_cast<std::size_t>(i)].id
                                                                              : std::string{};
    };
    switch (hit->kind) {
    case T::Tab:
        return false; // Map has its own button; Tarot, Journey and System are not on this page
    case T::OpenWeapons:
        inst->mv_view = 1, inst->mv_inspect = inv.weapon.ptr ? inv.weapon.id : "";
        return true;
    case T::OpenArcanas:
        inst->mv_view = 3, inst->mv_inspect = inv.spell.ptr ? inv.spell.id : "";
        return true;
    case T::OpenRelics:
        inst->mv_view = 2, inst->mv_page = 0, inst->mv_inspect = charm(inst->mv_slot);
        return true;
    case T::RelicSlot:
        if (hit->index >= slots)
            return false; // not unlocked yet
        if (inst->mv_view == 0) {
            inst->mv_view = 2, inst->mv_page = 0, inst->mv_slot = hit->index, inst->mv_inspect = charm(hit->index);
            return true;
        }
        if (inst->mv_slot == hit->index && !charm(hit->index).empty())
            return MenuUnequipRelic(inst, hit->index); // the picked slot again: take its relic off
        inst->mv_slot = hit->index;
        return true;
    case T::Item:
        if (inst->mv_inspect != hit->item) {
            inst->mv_inspect = hit->item; // first tap: shown in the card
            return true;
        }
        if (inst->mv_view == 2 && charm(inst->mv_slot) == hit->item)
            return MenuUnequipRelic(inst, inst->mv_slot);
        return MenuEquip(inst, hit->item, inst->mv_slot); // second tap: equipped
    case T::PrevPage:
        inst->mv_page = std::max(0, inst->mv_page - 1);
        return true;
    case T::NextPage:
        inst->mv_page = std::min(1, inst->mv_page + 1);
        return true;
    case T::Close:
        if (hit->index < 0)
            return false; // the inventory's own panel
        inst->mv_view = 0, inst->mv_inspect.clear();
        return true;
    }
    return false;
}

bool LoadFact(Instance* inst, int index, void* receiver, EdenDsmodImageSink sink) {
    if (inst->menu_state != 2 || index < 0 || index >= bc_facts::kCount)
        return false;
    const auto& f = bc_facts::kFacts[index];
    std::vector<u8> px;
    {
        std::lock_guard lock(inst->menu_mutex);
        if (!inst->menu)
            return false;
        px = inst->menu->RenderFact(f.title, f.text, f.art, f.item, kFactW, kFactH);
    }
    if (px.size() != static_cast<std::size_t>(kFactW) * kFactH * 4)
        return false;
    sink(receiver, kFactW, kFactH, px.data(), px.size());
    return true;
}

bool LoadMenu(Instance* inst, u64 id, void* receiver, EdenDsmodImageSink sink) {
    if (inst->menu_state != 2)
        return false;
    std::optional<bc_menupage::Snapshot> sn;
    {
        std::lock_guard lock(inst->ex_mutex);
        sn = Instance::Lookup(inst->menu_snaps, id);
    }
    if (!sn)
        return false;
    std::vector<bc_menupage::Target> targets;
    std::vector<u8> px;
    {
        std::lock_guard lock(inst->menu_mutex);
        if (!inst->menu)
            return false;
        const auto t0 = std::chrono::steady_clock::now();
        px = inst->menu->Render(*sn, kCanvasW, kCanvasH, &targets);
        inst->menu_ms = static_cast<int>(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    if (px.size() != static_cast<std::size_t>(kCanvasW) * kCanvasH * 4)
        return false;
    {
        std::lock_guard lock(inst->ex_mutex);
        Instance::Remember(inst->menu_targets, id, std::move(targets), 8);
    }
    sink(receiver, kCanvasW, kCanvasH, px.data(), px.size());
    inst->menu_drawn = id;
    return true;
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
        const bool with_fog = inst->samples % 60 == 1 || inst->force_slow;
        inst->force_slow = false;
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
            fresh.markers_comp = inst->live.markers_comp;
            fresh.markers_limit_mode = inst->live.markers_limit_mode;
            fresh.markers_limit = inst->live.markers_limit;
            fresh.marker_types = std::move(inst->live.marker_types);
            fresh.weapon_class = inst->live.weapon_class;
            fresh.spell_class = inst->live.spell_class;
            fresh.charm_class = inst->live.charm_class;
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
        HelperStep(inst, mem);
        const bc_live::State& s = inst->live;
        const auto world = inst->World();
        const bool world_ready = world && !world->biomes.empty();
        bool legacy_mode;
        {
            std::lock_guard lock(inst->lm_mutex);
            legacy_mode = inst->lm_failed.count(s.world_name) != 0;
        }
        if (with_fog && inst->await_markers) {
            // The markers the page changed are read: redraw the map with them now.
            inst->await_markers = false;
            std::lock_guard lock(inst->pin_mutex);
            ++inst->pins_rev;
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
            const auto reach = [](std::chrono::steady_clock::time_point since,
                                  std::chrono::steady_clock::time_point at) {
                const double dt = std::clamp(std::chrono::duration<double>(at - since).count(), 0.0, 1.0);
                return kJumpUnits + kMaxSpeed * dt;
            };
            if (pos_mode == 1) {
                bool take = true;
                if (inst->pos_ok && inst->pos_world == s.world_name &&
                    std::hypot(pwx - inst->pos_x, pwy - inst->pos_y) > reach(inst->pos_time, now)) {
                    // A jump: wait until the new place holds (moving as the player can).
                    if (inst->cand_n > 0 &&
                        std::hypot(pwx - inst->cand_x, pwy - inst->cand_y) <= reach(inst->cand_time, now)) {
                        ++inst->cand_n;
                    } else {
                        inst->cand_n = 1;
                        inst->cand_since = now;
                    }
                    inst->cand_x = pwx, inst->cand_y = pwy;
                    inst->cand_time = now;
                    take = inst->cand_n >= kJumpSamples &&
                           std::chrono::duration<double>(now - inst->cand_since).count() >= kJumpHoldSeconds;
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

        bool follow = false, marker = false;
        double mk_dx = 0, mk_dy = 0;
        int marker_zoom = std::clamp(inst->zoom, 0, kZoomLevels - 1);
        bool panned = false;
        std::string top_key; // the window picture on the page
        bool top_legacy = false;
        const int zoom = std::clamp(inst->zoom, 0, kZoomLevels - 1);
        if ((lm || legacy) && number >= 0) {
            if (pos_mode) {
                follow = true;
                const double scale = kZoomScale[zoom];
                const auto now = std::chrono::steady_clock::now();
                // Where the view looks: the player, or where the map was dragged to.
                if (inst->pan_world != s.world_name || inst->pan_biome != number || !lm) {
                    inst->pan_wx = inst->pan_wy = 0;
                    inst->pan_world = s.world_name;
                    inst->pan_biome = number;
                }
                if (std::chrono::duration<double>(now - inst->pan_at).count() > kPanIdleSeconds) {
                    // Gliding back to the player.
                    inst->pan_wx *= 0.82, inst->pan_wy *= 0.82;
                    if (std::hypot(inst->pan_wx, inst->pan_wy) * scale < 1.0)
                        inst->pan_wx = inst->pan_wy = 0;
                }
                double fx = pwx + inst->pan_wx, fy = pwy + inst->pan_wy;
                if (lm) {
                    fx = std::clamp(fx, std::min<double>(lm->x0, pwx), std::max<double>(lm->x1, pwx));
                    fy = std::clamp(fy, std::min<double>(lm->y0, pwy), std::max<double>(lm->y1, pwy));
                    inst->pan_wx = fx - pwx, inst->pan_wy = fy - pwy;
                }
                panned = std::hypot(inst->pan_wx, inst->pan_wy) * scale > kPanHomePx;
                Window& cur = inst->cur;
                const bool same_view = cur.valid && cur.biome == number && cur.zoom == zoom && cur.legacy == !lm;
                const bool far = same_view && (std::fabs(fx - cur.cx) * scale > kRecentreX ||
                                               std::fabs(fy - cur.cy) * scale > kRecentreY);
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
                    cur.valid = true;
                    cur.legacy = !lm;
                    cur.biome = number;
                    cur.zoom = zoom;
                    if (!same_view || far) {
                        cur.cx = static_cast<int>(std::lround(fx));
                        cur.cy = static_cast<int>(std::lround(fy));
                    }
                    cur.explore = inst->explore_id;
                    cur.pins = pins_rev;
                    cur.key = MapKey(cur);
                    inst->asked.push_back(cur);
                    while (inst->asked.size() > 6)
                        inst->asked.pop_front();
                }
                // The window on the page: the wanted one once its picture is drawn, until then
                // the last drawn one (at its own place and scale). The wanted one waits under it
                // (the host draws a picture only once it is asked for it).
                // (While the view is dragged a newer window may be wanted before the last one is
                // drawn: the newest drawn one is shown.)
                {
                    std::lock_guard lock(inst->ex_mutex);
                    for (auto it = inst->asked.rbegin(); it != inst->asked.rend(); ++it) {
                        if (it->biome != number || it->zoom != zoom || it->legacy != !lm)
                            continue;
                        if (std::find(inst->map_ready.begin(), inst->map_ready.end(), it->key) ==
                            inst->map_ready.end())
                            continue;
                        if (it->key != inst->shown.key) {
                            inst->prev = inst->shown;
                            inst->shown = *it;
                        }
                        break;
                    }
                }
                const bool shown_ok = inst->shown.valid && inst->shown.biome == number && inst->shown.legacy == !lm;
                const Window& top = shown_ok ? inst->shown : cur;
                // Canvas offset of a window so the view centre (fx, fy) is mid-screen.
                const auto offset = [&](const Window& w, double& ox, double& oy) {
                    const double ws = kZoomScale[std::clamp(w.zoom, 0, kZoomLevels - 1)];
                    const auto v = bc_mapdraw::CenteredView(w.cx, w.cy, ws, kWindowW, kWindowH);
                    double px, py;
                    v.WorldToPixel(fx, fy, px, py);
                    ox = kCanvasW / 2.0 - px;
                    oy = kCanvasH / 2.0 - py;
                };
                double ox, oy;
                offset(top, ox, oy);
                const double top_scale = kZoomScale[std::clamp(top.zoom, 0, kZoomLevels - 1)];
                {
                    // Canvas pixel -> world, as the window is shown (rounded offsets).
                    const auto v = bc_mapdraw::CenteredView(top.cx, top.cy, top_scale, kWindowW, kWindowH);
                    std::lock_guard lock(inst->pin_mutex);
                    inst->tap_view = v;
                    inst->tap_view.x0 = v.x0 - static_cast<double>(std::lround(ox)) / top_scale;
                    inst->tap_view.y1 = v.y1 + static_cast<double>(std::lround(oy)) / top_scale;
                    inst->tap_view.w = kCanvasW, inst->tap_view.h = kCanvasH;
                    inst->tap_view_ok = !top.legacy && top.zoom == zoom;
                    inst->tap_biome = number;
                    inst->tap_lm = lm;
                }
                top_key = top.key, top_legacy = top.legacy;
                PublishT(h, "bc.map.key", top.key);
                PublishI(h, "bc.map.ox", std::lround(ox));
                PublishI(h, "bc.map.oy", std::lround(oy));
                // Under it: the wanted window while it is not drawn yet (the host asks the module
                // for a picture only when a widget on the page shows it; the one on top covers
                // it), else the one before (it fills the edges while the view moves).
                const Window* under = nullptr;
                if (cur.key != top.key)
                    under = &cur;
                else if (inst->prev.valid && inst->prev.biome == number && inst->prev.zoom == top.zoom &&
                         inst->prev.legacy == top.legacy && inst->prev.key != top.key)
                    under = &inst->prev;
                PublishI(h, "bc.map.show", 1);
                if (under) {
                    double pox, poy;
                    offset(*under, pox, poy);
                    PublishT(h, "bc.map.pkey", under->key);
                    PublishI(h, "bc.map.pox", std::lround(pox));
                    PublishI(h, "bc.map.poy", std::lround(poy));
                }
                PublishI(h, "bc.map.pshow", under != nullptr);
                // The player marker, where the player is on the window shown.
                marker = true;
                marker_zoom = std::clamp(top.zoom, 0, kZoomLevels - 1);
                mk_dx = (pwx - fx) * top_scale;
                mk_dy = -(pwy - fy) * top_scale - kPlayerLift * top_scale;
            }
        }
        if (!follow) {
            PublishI(h, "bc.map.pshow", 0);
            PublishI(h, "bc.map.show", 0);
        }
        PublishI(h, "bc.pan.home", follow && panned);
        if (!follow) {
            std::lock_guard lock(inst->pin_mutex);
            inst->tap_view_ok = false;
        }
        PublishPins(inst, h);
        if (with_fog || inst->slow_texts.empty()) {
            // The exploration and inventory reads ran this sample (or nothing is cached yet).
            inst->slow_texts.clear();
            inst->slow_ints.clear();
            BuildFogTexts(s, inst->slow_texts, inst->slow_ints);
            BuildInventoryTexts(inst, s, inst->slow_texts, inst->slow_ints);
        }
        for (const auto& [k, v] : inst->slow_texts)
            PublishT(h, k.c_str(), v);
        for (const auto& [k, v] : inst->slow_ints)
            PublishI(h, k.c_str(), v);
        {
            std::lock_guard lock(inst->pin_mutex);
            PublishT(h, "bc.savepins", s.last_save_room + "  | helper " + inst->h_status);
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
            const bool paused = s.paused && !game_over && follow;
            if (follow && !top_legacy && top_key.rfind("module:bc:", 0) == 0)
                inst->blur_key = "module:bc:blur:" + top_key.substr(10);
            PublishT(h, "bc.map.bkey", inst->blur_key);
            PublishI(h, "bc.paused", paused);
            PublishI(h, "bc.paused.f", paused && follow && !inst->blur_key.empty());
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
            PublishI(h, "bc.ui.waitplain", !ui_ok && !follow);

            // Title page (no save loaded) and loading page (the game loading a scene, or the map
            // not shown yet in game). Their image keys appear only once the art can be drawn.
            const auto now = std::chrono::steady_clock::now();
            if (s.gs_loading && !inst->game_loading)
                inst->game_loading_since = now;
            inst->game_loading = s.gs_loading;
            const bool game_loading =
                s.gs_loading && std::chrono::duration<double>(now - inst->game_loading_since).count() >= kLoadingHoldSeconds;
            const bool loading = ui_ok && (game_loading || (s.room_ok && !follow));
            // The title page waits for the game's title screen: the main menu pushes the menu
            // game state (it pauses the game and takes UI input) while no room is loaded. Seen
            // once, the page stays until a save is in game; before it (logos, intro) it is
            // black. If the state is never recognised the page shows after kTitleFallbackSeconds.
            if (s.room_ok)
                inst->title_seen = false, inst->boot_since = now;
            else if (s.gs_ok && s.gs_pause && s.gs_ui)
                inst->title_seen = true;
            const bool title_fallback =
                std::chrono::duration<double>(now - inst->boot_since).count() >= kTitleFallbackSeconds;
            const bool title = ui_ok && !s.room_ok && !loading && (inst->title_seen || title_fallback);
            const bool boot = ui_ok && !s.room_ok && !loading && !title;
            if (s.room_ok) {
                if (const int card = bc_cards::ForWorld(s.world_name); card >= 0)
                    inst->loading_card = card;
            }
            const int load_card = inst->loading_card >= 0 ? inst->loading_card : inst->title_card;
            // The game's menu is read early (title or loading page): the facts are drawn with it.
            if ((title || loading) && ui_ok && inst->menu_state == 0)
                inst->StartMenu();
            if (loading && !inst->load_shown) {
                if (inst->fact_order.empty() || inst->fact_next >= inst->fact_order.size()) {
                    inst->fact_order.resize(bc_facts::kCount);
                    for (int i = 0; i < bc_facts::kCount; ++i)
                        inst->fact_order[static_cast<std::size_t>(i)] = i;
                    std::mt19937 rng(static_cast<u32>(now.time_since_epoch().count()));
                    std::shuffle(inst->fact_order.begin(), inst->fact_order.end(), rng);
                    inst->fact_next = 0;
                }
                inst->fact = inst->fact_order[inst->fact_next++];
            }
            inst->load_shown = loading;
            const bool fact = loading && inst->fact >= 0 && inst->menu_state == 2;
            char key[48];
            std::snprintf(key, sizeof key, "module:bc:ui:menuart:%d", inst->title_card);
            PublishT(h, "bc.ui.art", ui_ok ? key : "");
            std::snprintf(key, sizeof key, fact ? "module:bc:ui:loadartf:%d" : "module:bc:ui:loadart:%d", load_card);
            PublishT(h, "bc.ui.lart", ui_ok ? key : "");
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            std::snprintf(key, sizeof key, "module:bc:ui:loadtxt:%d", static_cast<int>(ms / kLoadingDotMs % 3) + 1);
            PublishT(h, "bc.ui.ltxt", ui_ok ? key : "");
            PublishI(h, "bc.ui.title", title);
            PublishI(h, "bc.ui.wait", title); // 0.5 manifests
            PublishI(h, "bc.ui.load", loading);
            PublishI(h, "bc.ui.fact", fact);
            PublishI(h, "bc.ui.load0", loading && !fact); // the loading line under the big card
            PublishI(h, "bc.ui.load1", loading && fact);  // ... at the bottom, under the fact
            std::snprintf(key, sizeof key, "module:bc:fact:%d", std::max(inst->fact, 0));
            PublishT(h, "bc.ui.factkey", key);
            const bool ingame = ui_ok && !loading && !title && !boot;
            PublishI(h, "bc.ui.ingame", ingame);
            PublishI(h, "bc.ui.boot", boot);
            // The frame's ember glow breathes on two beats (the host fades each picture).
            PublishI(h, "bc.ui.glow0", ingame && (ms / 1700) % 2 == 0);
            PublishI(h, "bc.ui.glow1", ingame && (ms / 1100) % 3 == 0);
            PublishMenu(inst, h, s, ingame, follow);
            if (!ingame && ui_ok) {
                // Pins are put on the map only: the symbols and the tap layer wait for it.
                PublishI(h, "bc.pin.mode", 0);
            }
        }
        PublishI(h, "bc.map.follow", follow);
        PublishI(h, "bc.map.ready", follow);
        PublishI(h, "bc.map.loading", lm_state == 1 && !follow);
        // One marker widget per zoom level (their sizes differ).
        PublishI(h, "bc.mk.show", marker);
        PublishI(h, "bc.mk.z0", marker && marker_zoom == 0);
        PublishI(h, "bc.mk.z1", marker && marker_zoom == 1);
        PublishI(h, "bc.mk.z2", marker && marker_zoom == 2);
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
            // The guest helper's mailbox (only with the package's load plan applied).
            inst->hx = *host;
            inst->hx_ok = host->mailbox_address != 0 && host->mailbox_size >= 0x1000 && host->load_u32 &&
                          host->store_u32 && host->load_u64 && host->store_u64;
        }
        inst->configured = true;
    }
    inst->codec_cv.notify_all();
}

/// The marker guid of a pin symbol (the game's MapMarkerTypeID whose icon it is), "" if unknown.
std::string PinGuid(Instance* inst, int type) {
    std::shared_ptr<const bc_levelmap::Common> common;
    {
        std::lock_guard lock(inst->world_mutex);
        common = inst->common;
    }
    if (!common || type < 0 || type >= kPinTypes)
        return {};
    for (const auto& [guid, sprite] : common->marker_types)
        if (sprite == kPinSprites[type])
            return guid;
    return {};
}

/// A tap on the map in pin mode: `packed` is the tap point on the canvas (x high, y low 32
/// bits). Near a marker of the game: another symbol changes it, the same symbol takes it away.
/// Elsewhere: a new marker of the selected symbol. All through the game's own marker functions
/// (the guest helper), so the game's map shows them and the game saves them.
bool PinTap(Instance* inst, int64_t packed, bool remove_only) {
    const double cx = static_cast<double>(static_cast<std::int32_t>(static_cast<u64>(packed) >> 32));
    const double cy = static_cast<double>(static_cast<std::int32_t>(static_cast<u64>(packed) & 0xFFFFFFFFu));
    const auto& s = inst->live;
    int tool;
    {
        std::lock_guard lock(inst->pin_mutex);
        tool = inst->pin_type;
    }
    const std::string guid = PinGuid(inst, tool);
    std::lock_guard lock(inst->pin_mutex);
    const auto lm = inst->tap_lm;
    if ((!remove_only && !inst->pin_mode) || !inst->tap_view_ok || inst->tap_biome < 0 || !lm || !s.markers_ok ||
        !s.markers_comp || !s.level_ptr || lm->level_guid != s.level_guid || !inst->hx_ok)
        return false;
    const bc_live::MarkerType* type = nullptr;
    for (const auto& t : s.marker_types)
        if (t.guid == guid)
            type = &t;
    const auto& v = inst->tap_view;
    const double wx = v.x0 + cx / v.scale, wy = v.y1 - cy / v.scale;
    const bc_live::MapMarker* best = nullptr;
    double best_d = kPinTouchPx;
    for (const auto& m : s.markers) {
        if (m.level_guid != s.level_guid)
            continue;
        double px, py;
        v.WorldToPixel(m.x, m.y, px, py);
        const double d = std::hypot(px - cx, py - cy);
        if (d < best_d)
            best_d = d, best = &m;
    }
    // A tap on a marker takes it away (with or without the Pin button on); elsewhere, with the
    // Pin button on, a new marker of the picked symbol goes there.
    if (!best && (remove_only || !type))
        return false;
    HelperRequest r;
    r.target = s.markers_comp;
    r.o1 = best && best->level_ptr ? best->level_ptr : s.level_ptr;
    if (best) {
        r.cmd = kCmdRemoveMarker;
        r.i = best->id;
    } else {
        // The game's own limit (MapMarkersComponent.markersCountLimit, counted its way).
        if (s.markers_limit > 0) {
            int count = 0;
            for (const auto& m : s.markers) {
                const bool same_type = m.type_guid == guid, same_map = m.level_guid == s.level_guid;
                switch (s.markers_limit_mode) {
                case 0: ++count; break;
                case 1: count += same_type; break;
                case 2: count += same_type && same_map; break;
                default: count += same_map; break;
                }
            }
            if (count >= s.markers_limit)
                return false; // full: the refused haptic says so
        }
        int zone = lm->NavZoneAt(static_cast<float>(wx), static_cast<float>(wy));
        int index = zone >= 0 ? lm->nav_zones[static_cast<std::size_t>(zone)].game_index : -1;
        if (index < 0 && lm->main_zone >= 0)
            index = lm->nav_zones[static_cast<std::size_t>(lm->main_zone)].game_index;
        if (index < 0)
            return false;
        r.cmd = kCmdAddMarker;
        r.o2 = type->guid_str;
        r.x = static_cast<float>(wx);
        r.y = static_cast<float>(wy);
        r.i = index;
    }
    inst->hq.push_back(std::move(r));
    return true;
}

EdenDsmodBool OnAction(void* p, const char* action, int64_t argument) {
    auto* inst = static_cast<Instance*>(p);
    if (!inst || !action)
        return EDEN_DSMOD_FALSE;
    try {
        const std::string a = action;
        // Close -> near -> far -> close.
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
            inst->pin_type = static_cast<int>(std::clamp<int64_t>(argument, 0, kPinTools - 1));
        } else if (a == "map_pan") {
            // A drag on the map (dx high, dy low 32 bits, canvas px): the map follows the finger.
            const double dx = static_cast<double>(static_cast<std::int32_t>(static_cast<u64>(argument) >> 32));
            const double dy = static_cast<double>(static_cast<std::int32_t>(static_cast<u64>(argument) & 0xFFFFFFFFu));
            const double scale = kZoomScale[std::clamp(inst->zoom, 0, kZoomLevels - 1)];
            inst->pan_wx -= dx / scale;
            inst->pan_wy += dy / scale;
            inst->pan_at = std::chrono::steady_clock::now();
        } else if (a == "map_home") {
            inst->pan_wx = inst->pan_wy = 0;
        } else if (a == "pin_tap") {
            return PinTap(inst, argument, false) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        } else if (a == "map_tap") {
            return PinTap(inst, argument, true) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        } else if (a == "menu_tap") {
            return MenuTap(inst, argument) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        } else if (a == "menu_back") {
            inst->mv_view = 0, inst->mv_inspect.clear();
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
    } else if (int g = 0; std::sscanf(what.c_str(), "glow:%d", &g) == 1) {
        w = kCanvasW, h = kCanvasH;
        px = bc_ui::FrameGlow(*common, w, h, g == 0 ? 0.7f : 0.45f);
    } else if (std::sscanf(what.c_str(), "zoom:%d", &zoom) == 1) {
        w = kZoomBarW, h = kZoomBarH;
        px = bc_ui::ZoomBar(*common, zoom, w, h);
    } else if (std::sscanf(what.c_str(), "zbtn:%d", &zoom) == 1) {
        w = kZoomButtonW, h = kZoomButtonH;
        px = bc_ui::ZoomButton(*common, zoom, w, h);
    } else if (what == "btn:debug" || what == "btn:map" || what == "btn:back") {
        w = kButtonW, h = kButtonH;
        px = bc_ui::Button(*common, what == "btn:debug" ? "Debug" : what == "btn:map" ? "Map" : "Back", w, h);
    } else if (unsigned long long cid = 0; std::sscanf(what.c_str(), "cell:%llx", &cid) == 1) {
        std::optional<Cell> c;
        {
            std::lock_guard lock(inst->ex_mutex);
            c = Instance::Lookup(inst->cells, static_cast<u64>(cid));
        }
        if (!c)
            return false;
        // A message line under the menu (the game's answer to a change).
        w = kEqMsgW, h = kEqMsgH;
        px = bc_ui::Label(*common, c->title, 28, w, h);
    } else if (int tab_on = 0; std::sscanf(what.c_str(), "tab:eq:%d", &tab_on) == 1) {
        w = kTabW, h = kTabH;
        px = bc_ui::Button(*common, "Equipment", w, h, tab_on != 0);
    } else if (int map_on = 0; std::sscanf(what.c_str(), "tab:map:%d", &map_on) == 1) {
        w = kTabW, h = kTabH;
        px = bc_ui::Button(*common, "Map", w, h, map_on != 0);
    } else if (what == "menunone" || what == "menuwait" || what == "menufail") {
        w = kLabelW, h = kLabelH;
        px = bc_ui::Label(*common,
                          what == "menunone"   ? "Load a save to see your equipment"
                          : what == "menuwait" ? "Reading the game's menu"
                                               : "The game's menu could not be read",
                          34, w, h);
    } else if (what == "btn:pin:0" || what == "btn:pin:1") {
        w = kButtonW, h = kButtonH;
        px = bc_ui::Button(*common, "Pin", w, h, what == "btn:pin:1");
    } else if (int t = 0, pin_on = 0; std::sscanf(what.c_str(), "pin:%d:%d", &t, &pin_on) == 2) {
        if (t < 0 || t >= kPinTools)
            return false;
        w = h = kPinCell;
        px = bc_ui::IconCell(*common, kPinSprites[t], pin_on != 0, w, h);
    } else if (what == "home") {
        // Back to the player (shown while the map is dragged off).
        w = h = kPinCell;
        px = bc_ui::IconCell(*common, "Map_PlayerIcon", false, w, h);
    } else if (what == "wait") {
        w = kLabelW, h = kLabelH;
        px = bc_ui::Label(*common, "Waiting for the game", 40, w, h);
    } else if (int f = 0; std::sscanf(what.c_str(), "gameover:%d", &f) == 1) {
        w = kGameOverW, h = kGameOverH;
        px = bc_ui::GameOver(*common, kGameOverGlow[std::clamp(f, 0, kGameOverFrames - 1)], w, h);
    } else if (int card = -1; what == "menuart" || std::sscanf(what.c_str(), "menuart:%d", &card) == 1 ||
                                std::sscanf(what.c_str(), "loadart:%d", &card) == 1 ||
                                std::sscanf(what.c_str(), "loadartf:%d", &card) == 1) {
        // A page of the game's painted cards: the title page (big card) or the loading page
        // (the card higher up, the loading line under it; smaller with a fact under it).
        const bool load = what.rfind("loadart:", 0) == 0, load_fact = what.rfind("loadartf:", 0) == 0;
        if (card < 0)
            card = inst->title_card;
        std::shared_ptr<bc_levelmap::Assets> assets;
        {
            std::lock_guard lock(inst->world_mutex);
            assets = inst->assets;
        }
        if (!assets)
            return false;
        std::optional<bc_levelmap::Sprite> sprite;
        std::string err;
        {
            std::lock_guard use(inst->assets_use);
            sprite = bc_cards::Load(*assets, inst->Codecs(), card, &err);
        }
        if (!sprite) {
            // Shown on the debug page (world log), once per reason.
            std::lock_guard lock(inst->world_mutex);
            if (inst->world_log.find(err) == std::string::npos)
                inst->world_log += "card " + std::to_string(card) + ": " + err + "\n";
            return false;
        }
        w = kCanvasW, h = kCanvasH;
        px = load_fact ? bc_ui::MenuArt(*sprite, w, h, 0.44f, 0.035f)
             : load    ? bc_ui::MenuArt(*sprite, w, h, 0.68f, 0.07f)
                       : bc_ui::MenuArt(*sprite, w, h);
        if (px.empty())
            return false;
    } else if (int dots = 0; std::sscanf(what.c_str(), "loadtxt:%d", &dots) == 1) {
        w = kLoadingW, h = kLoadingH;
        px = bc_ui::Loading(*common, dots, 40, w, h);
    } else if (what == "paused") {
        w = kPausedW, h = kPausedH;
        px = bc_ui::Paused(*common, w, h);
    } else if (what == "prompt") {
        w = kPromptW, h = kPromptH;
        px = bc_ui::Prompt(*common, "Press", "A", "to confirm", 30, w, h);
    } else if (what == "version") {
        w = kVersionW, h = kVersionH;
        px = bc_ui::Label(*common, std::string("Dual Screen Mod ") + kVersion, 26, w, h, 1, {0.62f, 0.58f, 0.52f, 1});
    } else if (what == "pinhint" || what == "pinhint:0" || what == "pinhint:1") {
        w = kLabelW, h = kLabelH;
        px = bc_ui::Label(*common, "Tap to place a pin, tap a pin to remove it", 32, w, h);
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
                                             const bc_mapdraw::Exploration& ex) {
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
    (void)pins_rev; // in the key only: a changed marker set redraws the window
    if (common)
        opt.extra = MapExtras(*lm, *common, *ex);
    std::vector<u8> px;
    int w, h;
    // Blurred (pause screen): the same view drawn kBlurDiv times smaller and softened; the page
    // stretches it back over the sharp one.
    const int div = blur ? kBlurDiv : 1;
    {
        w = kWindowW / div;
        h = kWindowH / div;
        const double scale = kZoomScale[std::clamp(zoom, 0, kZoomLevels - 1)];
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
    prm.zoom = zoom <= 0 ? 1 : 2; // the 0.2 renderer: 1 medium, 2 close
    // Window centre: world units -> biome tiles.
    prm.center_x = cx - static_cast<double>(biome->min_cx) * biome->chunk_w;
    prm.center_y = cy - static_cast<double>(biome->min_cy) * biome->chunk_h;
    prm.out_w = kWindowW;
    prm.out_h = kWindowH;
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
        if (int fact = -1; std::sscanf(key, "module:bc:fact:%d", &fact) == 1)
            return LoadFact(inst, fact, receiver, sink) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        if (std::sscanf(key, "module:bc:menu:%llx", &id) == 1)
            return LoadMenu(inst, static_cast<u64>(id), receiver, sink) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        if (std::sscanf(key, "module:bc:title:%llx", &id) == 1)
            return LoadTitle(inst, id, receiver, sink) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        int number = 0, zoom = 0, cx = 0, cy = 0;
        unsigned long long pins = 0;
        if (std::sscanf(key, "module:bc:blur:lm:4:%d:%d:%d:%d:%llx:%llx", &number, &zoom, &cx, &cy, &id, &pins) == 6)
            return LoadLevelMapImage(inst, number, zoom, cx, cy, id, pins, receiver, sink, true) ? EDEN_DSMOD_TRUE
                                                                                               : EDEN_DSMOD_FALSE;
        // A map window drawn: the page may show it now (see Window).
        const auto drawn = [inst, key](bool ok) {
            if (ok) {
                std::lock_guard lock(inst->ex_mutex);
                inst->map_ready.emplace_back(key);
                while (inst->map_ready.size() > 8)
                    inst->map_ready.pop_front();
            }
            return ok ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        };
        if (std::sscanf(key, "module:bc:lm:4:%d:%d:%d:%d:%llx:%llx", &number, &zoom, &cx, &cy, &id, &pins) == 6)
            return drawn(LoadLevelMapImage(inst, number, zoom, cx, cy, id, pins, receiver, sink));
        if (std::sscanf(key, "module:bc:map:2:%d:%d:%d:%d:%llx", &number, &zoom, &cx, &cy, &id) == 5)
            return drawn(LoadLegacyImage(inst, number, zoom, cx, cy, id, receiver, sink));
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
