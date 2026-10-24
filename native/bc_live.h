// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live state of Castlevania: Belmont's Curse 1.1.0 (build 0F1169AA7D0FA452...), read from guest
// memory. Everything below was derived offline from the game's own global-metadata.dat (v31) and
// main NSO; nothing here is a guess about values, only about where the game keeps them:
//
//   MotherBase.EntityFramework.SystemManager (TypeInfo slot main+0x6D47368, data segment)
//     static Instance                          (static_fields + 0x0)
//     _fixedUpdateSystems / _dynamicUpdateSystems / _lateUpdateSystems  SystemBase[] +0x28/30/38
//   RoomManager.RoomManagerSystem  (one of the systems)
//     _previousChunk Vector2Int +0x78, _currentChunk Vector2Int +0x80
//     _currentRoomHandle RoomStreamingHandle +0x90 -> worldRoomData +0x18
//   ExplorationSystem
//     _mcExplorerComponent +0x48, _currentWorld WorldData +0x58, _currentRoomInstance +0x60
//   MotherBase.RoomDesigner.WorldRoomData
//     worldName +0x10, positionCoord +0x20, runtimeRoomNumber +0x28, _instanceName +0x38, _id +0x48
//   MotherBase.RoomDesigner.WorldData  _rooms List<WorldRoomData> +0x50
//   RoomInstance  worldRoomData +0x20
//   ExplorerComponent
//     worldRoomExplorationStates Dictionary<string, WorldRoomExplorationState> +0x30
//     lastMainMapPosition Vector3 +0x90
//     lastUsedSaveRoomId string +0x88, lastMainMapLevelId LevelID +0xA0, selfEntity Entity +0x20
//   LevelID (SavableScriptableObject)  _guid string +0x18
//   MotherBase.EntityFramework.Entity  componentCache Dictionary<int, Component> +0x30 (every
//     component of the entity, filled in Awake)
//   MapMarkersComponent  _levelsMarkersData Dictionary<LevelID, LevelMarkersData> +0x28
//   LevelMarkersData  level LevelID +0x10, markersData List<MapMarkerData> +0x18
//   MapMarkerData  id int +0x10, position Vector2 +0x14, markerTypeGuid string +0x20
//   DeathResultComponent  positionOfLastDeath Vector3 +0x24, lastMainMapDeathPosition Vector3
//     +0x30, lastDeathInFastTravelRoom bool +0x49, levelIdOfLastDeath LevelID +0x58
//   WorldRoomExplorationState  roomDimensions Vector2Int +0x10, chunksExplorationState int[] +0x18,
//     discoveredZDoorExplorationZoneIDs List<int> +0x28, enteredZDoorIDs List<int> +0x38,
//     discoveredSecretZones List<int> +0x48, enteredZonesIds List<int> +0x50,
//     gpeSpecialActivated List<string> +0x58
//
// The data segment can sit at a constant shift from main_base + offset (NCE on Android); that
// shift is learned once from Il2CppMetadataRegistration (main+0x6A95908), whose counts are fixed
// for this build. Every class is checked by name before its fields are read, every pointer read
// is bounded, and a failed step only hides that step's values.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bc_live {

struct Memory {
    std::uint64_t main_base = 0;
    std::uint64_t main_size = 0;
    /// Copies `size` guest bytes at `address`; false when any byte is unmapped.
    std::function<bool(std::uint64_t address, void* out, std::size_t size)> read;
};

struct FogRoom {
    std::string key;       // the room's id, e.g. "World_Biome_01_Streets_Poor_..._uid59"
    std::int32_t uid = -1; // resolved through the current world's room list, -1 if unknown
    std::int32_t w = 0, h = 0;
    std::vector<std::uint8_t> states;
    std::vector<std::int32_t> entered_zones;          // exploration zones the player entered
    std::vector<std::int32_t> entered_zdoors;         // Z-doors the player went through
    std::vector<std::int32_t> discovered_zdoor_zones; // zones of Z-doors seen
    std::vector<std::int32_t> discovered_secrets;     // secret zones found
    std::vector<std::string> activated;               // GUIDs of activated gameplay elements
};

/// A marker the player placed on the map (MapMarkersComponent).
struct MapMarker {
    std::string level_guid; // LevelID._guid of the map it is on
    std::string type_guid;  // MapMarkerTypeID._guid (cross, chest, sun, ...)
    std::int32_t id = 0;
    float x = 0, y = 0;     // world position on that map
};

/// Where the player last died (DeathResultComponent), as the map's death spot shows it.
struct DeathSpot {
    bool known = false;
    std::string level_guid; // levelIdOfLastDeath
    float x = 0, y = 0;     // positionOfLastDeath
    bool in_fast_travel_room = false;
    float main_x = 0, main_y = 0; // lastMainMapDeathPosition
};

/// An item (ItemData, a ScriptableObject) as the inventory holds it.
struct Item {
    std::uint64_t ptr = 0;  // the object (identity)
    std::string id;         // itemID
    std::int32_t type = 0;  // ItemType: 1 weapon, 2 arcana, 3 charm (relic), 4 vania move, 5 codex clue, 6 quest, 7 skin
    std::int64_t name_key = 0; // itemDisplayName's string-table key
};

/// A stat of the player (StatsStateComponent.CurrentStats): its class name and value.
struct Stat {
    std::string name;
    float base = 0, value = 0;
    int skipped = 0; // modifiers whose value depends on a condition (not applied)
};

/// What the player carries and wears, and their stats.
struct Inventory {
    bool ok = false;
    std::vector<Item> items; // Inventory._itemsInInventory
    Item weapon, skin, spell; // the equipped ones (ptr 0 when none)
    std::vector<Item> charms; // equipped relics, one per slot (ptr 0 = empty slot)
    std::uint64_t inventory = 0; // the Inventory object, for writes
    bool level_ok = false;
    float level = 0, xp = 0, xp_max = 0;
    bool stats_ok = false;
    std::vector<Stat> stats;
};

struct State {
    // Resolve progress: 0 nothing, 1 data shift, 2 SystemManager class, 3 instance,
    // 4 systems found, 5 room read.
    int stage = 0;
    std::string status;
    std::int64_t delta = 0;
    int static_fields_offset = 0;
    int system_count = 0;
    std::string system_names;

    bool rm_ok = false;
    std::int32_t chunk_x = 0, chunk_y = 0, prev_x = 0, prev_y = 0;

    bool room_ok = false;
    std::string world_name, room_id, room_instance;
    std::int32_t room_uid = -1, room_x = 0, room_y = 0;

    bool ex_ok = false;
    std::int32_t ex_room_uid = -1;
    float map_x = 0, map_y = 0, map_z = 0;

    std::string last_save_room; // id of the last save room used
    std::string level_guid;     // ExplorerComponent.lastMainMapLevelId._guid

    // The player's components, found through the explorer's entity (with the exploration walk).
    bool markers_ok = false;
    std::vector<MapMarker> markers;
    DeathSpot death;
    Inventory inv;
    // DeathResultComponent.hasJustDied (+0x20), read every sample: set by
    // DeathManagementSystem the moment the player dies.
    bool death_flag_ok = false;
    bool just_died = false;

    // Pause: SystemManager._context (+0x20) -> EntityContext._contextPaused (+0x68), which
    // GameStateManager.TransitionState sets for states that pause the game; and the active
    // state (GameStateManager._instance._states top +0x18, an EditorParameteredGameState:
    // _pauseEntityContext +0x18, _pausePlaytimeCount +0x19, _activeActionMaps string[] +0x28).
    bool ctx_ok = false, ctx_paused = false;
    bool gs_ok = false, gs_pause = false, gs_pause_playtime = false;
    std::string gs_maps; // the active state's action maps, comma separated
    /// A menu or pause screen is open: the game is paused and the state takes UI input.
    bool paused = false;

    bool fog_ok = false;
    std::int32_t fog_entries = 0, fog_matched = 0;
    std::vector<FogRoom> fog;
    std::uint64_t fog_hash = 0;
    // The same without the chunk states: changes only when a zone, Z-door, secret, activated
    // element or the last save room changes (what the level map shows).
    std::uint64_t explore_hash = 0;
};

class Reader {
public:
    /// One sample. `with_fog` also walks the exploration dictionary (heavier).
    void Sample(const Memory& mem, bool with_fog, State& out);

private:
    bool ResolveDelta(const Memory& mem, State& out);
    bool ResolveSystemManager(const Memory& mem, State& out);
    bool FindSystems(const Memory& mem, State& out);
    /// "namespace.Name" of an object's class ("" when unreadable), cached by class pointer.
    const std::string& ClassOf(const Memory& mem, std::uint64_t obj);
    /// Markers and death spot from the components of the explorer's entity.
    void ReadPlayerComponents(const Memory& mem, std::uint64_t explorer, State& out);
    /// Inventory, level and stats of the player.
    void ReadInventory(const Memory& mem, std::uint64_t comp, State& out);
    void ReadExperience(const Memory& mem, std::uint64_t comp, State& out);
    void ReadStats(const Memory& mem, std::uint64_t comp, State& out);
    bool ReadItem(const Memory& mem, std::uint64_t ptr, Item& out);
    float StatValue(const Memory& mem, std::uint64_t stat_object, float* base, int* skipped);
    /// The name of a class from an Il2CppType* (through the game's metadata in memory).
    std::string TypeName(const Memory& mem, std::uint64_t il2cpp_type);
    std::uint64_t meta_base_ = 0;
    bool meta_searched_ = false;
    std::unordered_map<std::uint64_t, std::string> type_names_;
    /// The pause state (context flag and the active game state).
    void ReadPause(const Memory& mem, State& out);
    std::uint64_t death_comp_ = 0; // DeathResultComponent found by ReadPlayerComponents
    std::uint64_t gsm_class_ = 0;

    bool delta_known_ = false;
    std::int64_t delta_ = 0;
    std::uint64_t last_main_base_ = 0;
    std::uint64_t next_delta_try_ = 0;
    std::uint64_t samples_ = 0;

    std::uint64_t sysmgr_class_ = 0;
    int sf_offset_ = 0;
    std::uint64_t sysmgr_ = 0;
    std::uint64_t rm_ = 0, rm_class_ = 0;
    std::uint64_t ex_ = 0, ex_class_ = 0;
    std::string system_names_;
    int system_count_ = 0;

    std::unordered_map<std::uint64_t, std::string> class_names_; // klass -> "ns.name"
};

} // namespace bc_live
