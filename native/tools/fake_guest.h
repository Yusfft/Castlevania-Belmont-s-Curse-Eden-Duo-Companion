// SPDX-License-Identifier: GPL-3.0-or-later
// Test helper: a small fake guest address space laid out the way bc_live.h documents the game's
// objects, so the reader and the module glue can be exercised without the game.
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

struct FakeGuest {
    std::map<std::uint64_t, std::vector<std::uint8_t>> regions; // base -> bytes
    std::uint64_t heap_next = 0x20000000;
    std::uint64_t main_base = 0x8000000;
    std::uint64_t main_size = 0x7400000;
    std::uint64_t explorer_ = 0;
    std::uint64_t sysmgr_ = 0, data_ = 0, death_ = 0, ctx_ = 0, state_ = 0; // ExplorerComponent of the last Build, for moving the player
    // The exploration dictionary of the last Build, for adding rooms.
    std::uint64_t dict_entries_ = 0, dict_ = 0, kstring_ = 0, karray_ = 0, klist_ = 0, kstate_ = 0;
    std::int32_t dict_count_ = 0;
    // The game state stack (SetPause / SetLoading).
    std::uint64_t gsm_ = 0, stack_ = 0, menu_entry_ = 0, loading_entry_ = 0, loading_state_ = 0;

    bool Read(std::uint64_t at, void* out, std::size_t n) const {
        auto it = regions.upper_bound(at);
        if (it == regions.begin())
            return false;
        --it;
        const std::uint64_t off = at - it->first;
        if (off > it->second.size() || n > it->second.size() - off)
            return false;
        std::memcpy(out, it->second.data() + off, n);
        return true;
    }
    std::uint8_t* Ptr(std::uint64_t at, std::size_t n) {
        auto it = regions.upper_bound(at);
        --it;
        const std::uint64_t off = at - it->first;
        if (off + n > it->second.size())
            std::abort();
        return it->second.data() + off;
    }
    template <typename T>
    T Get(std::uint64_t at) {
        T v{};
        std::memcpy(&v, Ptr(at, sizeof(T)), sizeof(T));
        return v;
    }
    template <typename T>
    void Put(std::uint64_t at, T v) {
        std::memcpy(Ptr(at, sizeof(T)), &v, sizeof(T));
    }
    std::uint64_t Alloc(std::size_t n) {
        const std::uint64_t at = heap_next;
        heap_next += (n + 0xFFF) & ~std::uint64_t(0xFFF);
        heap_next += 0x1000; // guard gap
        regions[at] = std::vector<std::uint8_t>(n, 0);
        return at;
    }
    std::uint64_t CStr(const std::string& s) {
        const std::uint64_t at = Alloc(s.size() + 1);
        std::memcpy(Ptr(at, s.size()), s.data(), s.size());
        return at;
    }
    std::uint64_t Klass(const std::string& ns, const std::string& name) {
        const std::uint64_t k = Alloc(0x140);
        Put<std::uint64_t>(k + 0x10, CStr(name));
        Put<std::uint64_t>(k + 0x18, CStr(ns));
        return k;
    }
    std::uint64_t Obj(std::uint64_t klass, std::size_t size) {
        const std::uint64_t o = Alloc(size);
        Put<std::uint64_t>(o, klass);
        return o;
    }
    std::uint64_t String(std::uint64_t string_klass, const std::string& s) {
        const std::uint64_t o = Obj(string_klass, 0x14 + 2 * s.size() + 2);
        Put<std::int32_t>(o + 0x10, static_cast<std::int32_t>(s.size()));
        for (std::size_t i = 0; i < s.size(); ++i)
            Put<std::uint16_t>(o + 0x14 + 2 * i, static_cast<unsigned char>(s[i]));
        return o;
    }
    template <typename T>
    std::uint64_t Array(std::uint64_t klass, const std::vector<T>& items) {
        const std::uint64_t a = Obj(klass, 0x20 + sizeof(T) * items.size());
        Put<std::uint64_t>(a + 0x18, items.size());
        for (std::size_t i = 0; i < items.size(); ++i)
            Put<T>(a + 0x20 + sizeof(T) * i, items[i]);
        return a;
    }

    struct Expect {
        std::int64_t delta;
        std::int32_t chunk_x, chunk_y, uid;
        std::string world, id;
    };

    /// Builds the documented object graph. `delta` shifts the data segment (NCE).
    Expect Build(std::int64_t delta, std::int32_t uid = 59, std::int32_t pos_x = -7,
                 std::int32_t pos_y = 15, std::int32_t chunk_x = 1, std::int32_t chunk_y = 0,
                 const std::string& world = "World_Biome_01_Streets_Poor",
                 std::vector<std::int32_t> states = {3, 2, 1, 0}, std::int32_t dims_w = 2,
                 std::int32_t dims_h = 2, float map_x = 1.5f, float map_y = -2.25f) {
        regions[main_base] = std::vector<std::uint8_t>(main_size + 0x100000, 0);
        const std::uint64_t data = main_base + static_cast<std::uint64_t>(delta);
        // Il2CppMetadataRegistration counts.
        const std::uint64_t reg = data + 0x6A95908;
        const std::uint64_t counts[13] = {14149, 1, 10043, 1, 0, 1, 54324, 1, 0, 1, 20122, 1, 20122};
        for (int i = 0; i < 13; ++i)
            Put<std::uint64_t>(reg + 8 * i, counts[i]);
        const std::uint64_t kstring = Klass("System", "String");
        const std::uint64_t karray = Klass("", "Object[]");
        const std::uint64_t ksys = Klass("MotherBase.EntityFramework", "SystemManager");
        Put<std::uint64_t>(data + 0x6D47368, ksys);
        const std::uint64_t statics = Alloc(16);
        Put<std::uint64_t>(ksys + 0xB8, statics);
        const std::uint64_t sysmgr = Obj(ksys, 0x98);
        sysmgr_ = sysmgr;
        data_ = data;
        Put<std::uint64_t>(statics, sysmgr);

        const std::uint64_t krm = Klass("RoomManager", "RoomManagerSystem");
        const std::uint64_t kex = Klass("", "ExplorationSystem");
        const std::uint64_t kother = Klass("", "CameraSystem");
        const std::uint64_t rm = Obj(krm, 0xA8);
        const std::uint64_t ex = Obj(kex, 0xA8);
        const std::uint64_t other = Obj(kother, 0x40);
        Put<std::uint64_t>(sysmgr + 0x30, Array<std::uint64_t>(karray, {other, rm, ex}));

        const std::uint64_t kwrd = Klass("MotherBase.RoomDesigner", "WorldRoomData");
        const std::uint64_t wrd = Obj(kwrd, 0x50);
        Put<std::uint64_t>(wrd + 0x10, String(kstring, world));
        Put<std::int32_t>(wrd + 0x20, pos_x);
        Put<std::int32_t>(wrd + 0x24, pos_y);
        Put<std::int32_t>(wrd + 0x28, uid);
        Put<std::uint64_t>(wrd + 0x38, String(kstring, "StreetsPoor_Room21_uid" + std::to_string(uid)));
        Put<std::uint64_t>(wrd + 0x48, String(kstring, "room-guid-" + std::to_string(uid)));

        const std::uint64_t handle = Obj(Klass("RoomManager", "RoomStreamingHandle"), 0x38);
        Put<std::uint64_t>(handle + 0x18, wrd);
        Put<std::int32_t>(rm + 0x78, chunk_x);
        Put<std::int32_t>(rm + 0x7C, chunk_y);
        Put<std::int32_t>(rm + 0x80, chunk_x);
        Put<std::int32_t>(rm + 0x84, chunk_y);
        Put<std::uint64_t>(rm + 0x90, handle);

        const std::uint64_t ri = Obj(Klass("MotherBase.RoomDesigner", "RoomInstance"), 0x38);
        Put<std::uint64_t>(ri + 0x20, wrd);
        Put<std::uint64_t>(ex + 0x60, ri);

        const std::uint64_t explorer = Obj(Klass("", "ExplorerComponent"), 0xB0);
        Put<float>(explorer + 0x90, map_x);
        Put<float>(explorer + 0x94, map_y);
        explorer_ = explorer;
        Put<std::uint64_t>(ex + 0x48, explorer);

        kstring_ = kstring;
        karray_ = karray;
        klist_ = Klass("System.Collections.Generic", "List`1");
        kstate_ = Klass("", "WorldRoomExplorationState");
        const std::uint64_t state = Obj(kstate_, 0x80);
        Put<std::int32_t>(state + 0x10, dims_w);
        Put<std::int32_t>(state + 0x14, dims_h);
        Put<std::uint64_t>(state + 0x18, Array<std::int32_t>(karray, states));
        // Dictionary<string, WorldRoomExplorationState>: one freed entry, one live entry, room
        // for AddRoom.
        const std::uint64_t entries = Obj(karray, 0x20 + 256 * 0x18);
        Put<std::uint64_t>(entries + 0x18, 256);
        Put<std::int32_t>(entries + 0x20 + 0x18 + 0, 12345);
        Put<std::uint64_t>(entries + 0x20 + 0x18 + 8, String(kstring, "room-guid-" + std::to_string(uid)));
        Put<std::uint64_t>(entries + 0x20 + 0x18 + 16, state);
        const std::uint64_t dict = Obj(Klass("System.Collections.Generic", "Dictionary`2"), 0x50);
        Put<std::uint64_t>(dict + 0x18, entries);
        Put<std::int32_t>(dict + 0x20, 2);
        Put<std::uint64_t>(explorer + 0x30, dict);
        dict_entries_ = entries;
        dict_ = dict;
        dict_count_ = 2;

        const std::uint64_t worlddata = Obj(Klass("MotherBase.RoomDesigner", "WorldData"), 0x58);
        const std::uint64_t list = Obj(Klass("System.Collections.Generic", "List`1"), 0x28);
        Put<std::uint64_t>(list + 0x10, Array<std::uint64_t>(karray, {wrd}));
        Put<std::int32_t>(list + 0x18, 1);
        Put<std::uint64_t>(worlddata + 0x50, list);
        Put<std::uint64_t>(ex + 0x58, worlddata);
        return Expect{delta, chunk_x, chunk_y, uid, world, "room-guid-" + std::to_string(uid)};
    }

    std::uint64_t IntList(const std::vector<std::int32_t>& v) {
        const std::uint64_t list = Obj(klist_, 0x28);
        Put<std::uint64_t>(list + 0x10, Array<std::int32_t>(karray_, v));
        Put<std::int32_t>(list + 0x18, static_cast<std::int32_t>(v.size()));
        return list;
    }

    /// Adds an exploration entry: `key` is the room id, with entered zones and Z-doors.
    void AddRoom(const std::string& key, const std::vector<std::int32_t>& zones,
                 const std::vector<std::int32_t>& zdoors = {}) {
        if (dict_count_ >= 256)
            return;
        const std::uint64_t state = Obj(kstate_, 0x80);
        Put<std::int32_t>(state + 0x10, 1);
        Put<std::int32_t>(state + 0x14, 1);
        Put<std::uint64_t>(state + 0x18, Array<std::int32_t>(karray_, {3}));
        Put<std::uint64_t>(state + 0x38, IntList(zdoors));
        Put<std::uint64_t>(state + 0x50, IntList(zones));
        const std::uint64_t e = dict_entries_ + 0x20 + 0x18 * static_cast<std::uint64_t>(dict_count_);
        Put<std::int32_t>(e, 1000 + dict_count_);
        Put<std::uint64_t>(e + 8, String(kstring_, key));
        Put<std::uint64_t>(e + 16, state);
        ++dict_count_;
        Put<std::int32_t>(dict_ + 0x20, dict_count_);
    }

    void SetDied(bool died) { Put<std::uint8_t>(death_ + 0x20, died ? 1 : 0); }

    /// The game's pause: the ECS context flag and the active game state (a menu state with the
    /// "UI" action map when `menu`).
    void SetPause(bool paused, bool menu = true) {
        if (!ctx_) {
            ctx_ = Obj(Klass("MotherBase.EntityFramework", "EntityContext"), 0x70);
            Put<std::uint64_t>(sysmgr_ + 0x20, ctx_);
            const std::uint64_t kgsm = Klass("", "GameStateManager");
            // As in the game: the TypeInfo slot holds the class; GetInstance's pointer holds
            // the slot's address.
            Put<std::uint64_t>(data_ + 0x6D42540, kgsm);
            Put<std::uint64_t>(data_ + 0x6BDBA10, data_ + 0x6D42540);
            const std::uint64_t statics = Alloc(16);
            Put<std::uint64_t>(kgsm + 0xB8, statics);
            const std::uint64_t gsm = Obj(kgsm, 0xD0);
            gsm_ = gsm;
            Put<std::uint64_t>(statics, gsm);
            state_ = Obj(Klass("", "EditorParameteredGameState"), 0x30);
            const std::uint64_t entry = Obj(Klass("", "GameStateManager.Entry"), 0x20);
            Put<std::uint64_t>(entry + 0x18, state_);
            const std::uint64_t base = Obj(Klass("", "EditorParameteredGameState"), 0x30);
            const std::uint64_t base_entry = Obj(Klass("", "GameStateManager.Entry"), 0x20);
            Put<std::uint64_t>(base_entry + 0x18, base);
            const std::uint64_t stack = Obj(Klass("System.Collections.Generic", "Stack`1"), 0x28);
            loading_state_ = Obj(Klass("", "EditorParameteredGameState"), 0x30);
            loading_entry_ = Obj(Klass("", "GameStateManager.Entry"), 0x20);
            Put<std::uint64_t>(loading_entry_ + 0x18, loading_state_);
            Put<std::uint64_t>(gsm + 0x68, loading_state_);
            stack_ = stack;
            menu_entry_ = entry;
            Put<std::uint64_t>(stack + 0x10, Array<std::uint64_t>(karray_, {base_entry, entry, 0}));
            Put<std::int32_t>(stack + 0x18, 2);
            Put<std::uint64_t>(gsm + 0xA0, stack);
            Put<std::uint64_t>(state_ + 0x28, Array<std::uint64_t>(karray_, {String(kstring_, "UI"), String(kstring_, "Debug")}));
        }
        Put<std::uint8_t>(ctx_ + 0x68, paused ? 1 : 0);
        Put<std::uint8_t>(state_ + 0x18, paused ? 1 : 0);
        Put<std::uint8_t>(state_ + 0x19, paused ? 1 : 0);
        if (!menu)
            Put<std::uint64_t>(state_ + 0x28, Array<std::uint64_t>(karray_, {String(kstring_, "Gameplay")}));
    }

    /// The game's loading state on top of the state stack (SetPause first builds the stack).
    void SetLoading(bool loading) {
        if (!ctx_)
            SetPause(false);
        const std::uint64_t arr = Get<std::uint64_t>(stack_ + 0x10);
        Put<std::uint64_t>(arr + 0x20 + 8, loading ? loading_entry_ : menu_entry_);
    }

    void SetLastSave(const std::string& room_id) { Put<std::uint64_t>(explorer_ + 0x88, String(kstring_, room_id)); }

    struct FakeMarker {
        std::string type_guid;
        float x, y;
    };
    /// The explorer's entity with its MapMarkersComponent (markers on level `level_guid`) and
    /// DeathResultComponent (a death at `death_x, death_y` there when `died`).
    void SetMapState(const std::string& level_guid, const std::vector<FakeMarker>& markers, bool died,
                     float death_x, float death_y) {
        const std::uint64_t level = Obj(Klass("", "LevelID"), 0x28);
        Put<std::uint64_t>(level + 0x18, String(kstring_, level_guid));
        Put<std::uint64_t>(explorer_ + 0xA0, level);
        const std::uint64_t entity = Obj(Klass("MotherBase.EntityFramework", "Entity"), 0x2E0);
        Put<std::uint64_t>(explorer_ + 0x20, entity);
        // MapMarkersComponent -> Dictionary<LevelID, LevelMarkersData> -> List<MapMarkerData>.
        const std::uint64_t kmarker = Klass("", "MapMarkerData");
        std::vector<std::uint64_t> objs;
        for (std::size_t i = 0; i < markers.size(); ++i) {
            const std::uint64_t m = Obj(kmarker, 0x28);
            Put<std::int32_t>(m + 0x10, static_cast<std::int32_t>(i));
            Put<float>(m + 0x14, markers[i].x);
            Put<float>(m + 0x18, markers[i].y);
            Put<std::uint64_t>(m + 0x20, String(kstring_, markers[i].type_guid));
            objs.push_back(m);
        }
        const std::uint64_t list = Obj(klist_, 0x28);
        Put<std::uint64_t>(list + 0x10, Array<std::uint64_t>(karray_, objs));
        Put<std::int32_t>(list + 0x18, static_cast<std::int32_t>(objs.size()));
        const std::uint64_t lmd = Obj(Klass("", "LevelMarkersData"), 0x28);
        Put<std::uint64_t>(lmd + 0x10, level);
        Put<std::uint64_t>(lmd + 0x18, list);
        const auto dict = [&](const std::vector<std::pair<std::uint64_t, std::uint64_t>>& kv) {
            const std::uint64_t entries = Obj(karray_, 0x20 + 0x18 * (kv.size() + 1));
            Put<std::uint64_t>(entries + 0x18, kv.size() + 1);
            // Entry 0 freed (next = -3), then the live ones.
            Put<std::int32_t>(entries + 0x20 + 4, -3);
            for (std::size_t i = 0; i < kv.size(); ++i) {
                const std::uint64_t e = entries + 0x20 + 0x18 * (i + 1);
                Put<std::int32_t>(e, 77 + static_cast<std::int32_t>(i));
                Put<std::int32_t>(e + 4, -1);
                Put<std::uint64_t>(e + 8, kv[i].first);
                Put<std::uint64_t>(e + 16, kv[i].second);
            }
            const std::uint64_t d = Obj(Klass("System.Collections.Generic", "Dictionary`2"), 0x50);
            Put<std::uint64_t>(d + 0x18, entries);
            Put<std::int32_t>(d + 0x20, static_cast<std::int32_t>(kv.size() + 1));
            return d;
        };
        const std::uint64_t mc = Obj(Klass("", "MapMarkersComponent"), 0x40);
        Put<std::uint64_t>(mc + 0x28, dict({{level, lmd}}));
        const std::uint64_t death = Obj(Klass("", "DeathResultComponent"), 0x60);
        death_ = death;
        if (died) {
            Put<float>(death + 0x24, death_x);
            Put<float>(death + 0x28, death_y);
            Put<float>(death + 0x30, death_x);
            Put<float>(death + 0x34, death_y);
            Put<std::uint64_t>(death + 0x40, level);
            Put<std::uint64_t>(death + 0x58, level);
        }
        // Inventory: two weapons owned, one worn, one relic slot of two used.
        const std::uint64_t kitem = Klass("", "WeaponItemData");
        const auto item = [&](const std::string& id, int type, std::int64_t key) {
            const std::uint64_t it = Obj(kitem, 0x80);
            Put<std::uint64_t>(it + 0x20, String(kstring_, id));
            Put<std::int32_t>(it + 0x28, type);
            const std::uint64_t ls = Obj(Klass("UnityEngine.Localization", "LocalizedString"), 0xE0);
            Put<std::int64_t>(ls + 0x30, key);
            Put<std::uint64_t>(it + 0x30, ls);
            return it;
        };
        const std::uint64_t sword = item("Weapon_Sword", 1, 111), whip = item("Weapon_Whip", 1, 222);
        const std::uint64_t relic = item("Charm_Ring", 3, 333), spell = item("Spell_Fire", 2, 444);
        const std::uint64_t inv = Obj(Klass("", "Inventory"), 0x58);
        const std::uint64_t ilist = Obj(klist_, 0x28);
        Put<std::uint64_t>(ilist + 0x10, Array<std::uint64_t>(karray_, {sword, whip}));
        Put<std::int32_t>(ilist + 0x18, 2);
        Put<std::uint64_t>(inv + 0x20, ilist);
        Put<std::uint64_t>(inv + 0x28, whip);
        Put<std::uint64_t>(inv + 0x38, spell);
        Put<std::uint64_t>(inv + 0x40, Array<std::uint64_t>(karray_, {relic, 0}));
        const std::uint64_t invc = Obj(Klass("", "InventoryComponent"), 0x38);
        Put<std::uint64_t>(invc + 0x28, inv);
        // Experience: level 17 (base 16, +1 from a brute modifier).
        const std::uint64_t kso = Klass("", "StatObject");
        const auto stat_object = [&](float base, float add) {
            const std::uint64_t so = Obj(kso, 0x30);
            Put<float>(so + 0x18, base);
            const std::uint64_t mod = Obj(Klass("", "OperationBruteValueStatModifier"), 0x18);
            Put<std::int32_t>(mod + 0x10, 0);
            Put<float>(mod + 0x14, add);
            const std::uint64_t ml = Obj(klist_, 0x28);
            Put<std::uint64_t>(ml + 0x10, Array<std::uint64_t>(karray_, {mod}));
            Put<std::int32_t>(ml + 0x18, 1);
            Put<std::uint64_t>(so + 0x20, ml);
            return so;
        };
        const std::uint64_t exp = Obj(Klass("", "ExperienceComponent"), 0x98);
        const std::uint64_t ref = Obj(Klass("", "StatRef`1"), 0x28);
        Put<std::uint64_t>(ref + 0x18, stat_object(16, 1));
        Put<std::uint64_t>(exp + 0x78, ref);
        // Stats: one stat whose type name comes from a fake metadata blob.
        const std::uint64_t meta = Alloc(0x3000);
        Put<std::uint32_t>(meta, 0xFAB11BAF);
        Put<std::int32_t>(meta + 4, 31);
        Put<std::int32_t>(meta + 8 + 2 * 8, 0x100);      // strings at +0x100
        Put<std::int32_t>(meta + 8 + 2 * 8 + 4, 0x100);
        Put<std::int32_t>(meta + 8 + 19 * 8, 0x1000);    // type definitions at +0x1000
        Put<std::int32_t>(meta + 8 + 19 * 8 + 4, 88 * 4);
        std::memcpy(Ptr(meta + 0x100 + 8, 7), "Attack", 7);
        Put<std::int32_t>(meta + 0x1000 + 88 * 2, 8);     // typedef #2: name "Attack"
        const std::uint64_t il2type = Alloc(16);
        Put<std::uint64_t>(il2type, meta + 0x1000 + 88 * 2);
        const std::uint64_t typeobj = Obj(Klass("System", "RuntimeType"), 0x18);
        Put<std::uint64_t>(typeobj + 0x10, il2type);
        const std::uint64_t statsc = Obj(Klass("", "StatsStateComponent"), 0x38);
        Put<std::uint64_t>(statsc + 0x28, dict({{typeobj, stat_object(10, 8)}}));
        // componentCache: Dictionary<int, Component>, keyed by type hash.
        Put<std::uint64_t>(entity + 0x30, dict({{0x1111, explorer_}, {0x2222, mc}, {0x3333, death}, {0x4444, invc},
                                                {0x5555, exp}, {0x6666, statsc}}));
    }
};
