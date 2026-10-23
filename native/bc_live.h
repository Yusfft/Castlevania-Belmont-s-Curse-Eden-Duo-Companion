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
//     lastUsedSaveRoomId string +0x88
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

    bool fog_ok = false;
    std::int32_t fog_entries = 0, fog_matched = 0;
    std::vector<FogRoom> fog;
    std::uint64_t fog_hash = 0;
};

class Reader {
public:
    /// One sample. `with_fog` also walks the exploration dictionary (heavier).
    void Sample(const Memory& mem, bool with_fog, State& out);

private:
    bool ResolveDelta(const Memory& mem, State& out);
    bool ResolveSystemManager(const Memory& mem, State& out);
    bool FindSystems(const Memory& mem, State& out);

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
