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
                 std::int32_t dims_h = 2) {
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
        Put<float>(explorer + 0x90, 1.5f);
        Put<float>(explorer + 0x94, -2.25f);
        Put<std::uint64_t>(ex + 0x48, explorer);

        const std::uint64_t state = Obj(Klass("", "WorldRoomExplorationState"), 0x80);
        Put<std::int32_t>(state + 0x10, dims_w);
        Put<std::int32_t>(state + 0x14, dims_h);
        Put<std::uint64_t>(state + 0x18, Array<std::int32_t>(karray, states));
        // Dictionary<string, WorldRoomExplorationState>: one freed entry, one live entry.
        const std::uint64_t entries = Obj(karray, 0x20 + 3 * 0x18);
        Put<std::uint64_t>(entries + 0x18, 3);
        Put<std::int32_t>(entries + 0x20 + 0x18 + 0, 12345);
        Put<std::uint64_t>(entries + 0x20 + 0x18 + 8, String(kstring, "room-guid-" + std::to_string(uid)));
        Put<std::uint64_t>(entries + 0x20 + 0x18 + 16, state);
        const std::uint64_t dict = Obj(Klass("System.Collections.Generic", "Dictionary`2"), 0x50);
        Put<std::uint64_t>(dict + 0x18, entries);
        Put<std::int32_t>(dict + 0x20, 2);
        Put<std::uint64_t>(explorer + 0x30, dict);

        const std::uint64_t worlddata = Obj(Klass("MotherBase.RoomDesigner", "WorldData"), 0x58);
        const std::uint64_t list = Obj(Klass("System.Collections.Generic", "List`1"), 0x28);
        Put<std::uint64_t>(list + 0x10, Array<std::uint64_t>(karray, {wrd}));
        Put<std::int32_t>(list + 0x18, 1);
        Put<std::uint64_t>(worlddata + 0x50, list);
        Put<std::uint64_t>(ex + 0x58, worlddata);
        return Expect{delta, chunk_x, chunk_y, uid, world, "room-guid-" + std::to_string(uid)};
    }
};
