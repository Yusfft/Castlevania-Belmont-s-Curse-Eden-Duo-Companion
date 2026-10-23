// SPDX-License-Identifier: GPL-3.0-or-later
// Live state reader. See bc_live.h for every offset and where it comes from.

#include "bc_live.h"

#include <cmath>
#include <cstring>

namespace bc_live {
namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// ---- build-pinned locations (main-relative) ----
constexpr u64 kMetadataRegistration = 0x6A95908; // Il2CppMetadataRegistration
constexpr u64 kSystemManagerTypeInfo = 0x6D47368;
constexpr u64 kSystemManagerEncoded = 0x200117D1; // unresolved metadata-usage token (TypeInfo #35816)

// Il2CppMetadataRegistration counts for this build (entries 0, 2, 6, 10, 12).
constexpr u64 kGenericClassesCount = 14149;
constexpr u64 kGenericInstsCount = 10043;
constexpr u64 kTypesCount = 54324;
constexpr u64 kTypeDefinitionsCount = 20122;

// ---- IL2CPP runtime layout (Unity 2022.3, metadata v31) ----
constexpr u64 kClassName = 0x10;
constexpr u64 kClassNamespace = 0x18;
constexpr int kStaticFieldsCandidates[] = {0xB8, 0xB0, 0xC0, 0xA8, 0xC8, 0xA0};
constexpr u64 kArrayLength = 0x18;
constexpr u64 kArrayData = 0x20;
constexpr u64 kStringLength = 0x10;
constexpr u64 kStringChars = 0x14;
constexpr u64 kListItems = 0x10;
constexpr u64 kListSize = 0x18;
constexpr u64 kDictEntries = 0x18;
constexpr u64 kDictCount = 0x20;
constexpr u64 kDictEntryStride = 0x18; // {int hashCode, int next, string key, object value}

// ---- game classes ----
constexpr u64 kSysMgrSystems[] = {0x28, 0x30, 0x38};
constexpr u64 kRmPrevChunk = 0x78;
constexpr u64 kRmCurChunk = 0x80;
constexpr u64 kRmCurHandle = 0x90;
constexpr u64 kHandleWorldRoomData = 0x18;
constexpr u64 kExExplorer = 0x48;
constexpr u64 kExCurrentWorld = 0x58;
constexpr u64 kExCurrentRoomInstance = 0x60;
constexpr u64 kRoomInstanceWorldRoomData = 0x20;
constexpr u64 kWrdWorldName = 0x10;
constexpr u64 kWrdPosition = 0x20;
constexpr u64 kWrdRuntimeNumber = 0x28;
constexpr u64 kWrdInstanceName = 0x38;
constexpr u64 kWrdId = 0x48;
constexpr u64 kWorldDataRooms = 0x50;
constexpr u64 kExplorerStates = 0x30;
constexpr u64 kExplorerLastMapPos = 0x90;
constexpr u64 kExplorerLastSaveRoom = 0x88;
constexpr u64 kExplorerLevelId = 0xA0;
constexpr u64 kExplorerSelfEntity = 0x20;
constexpr u64 kSavableGuid = 0x18;
constexpr u64 kEntityComponentCache = 0x30;
constexpr u64 kMarkersLevels = 0x28;
constexpr u64 kLevelMarkersLevel = 0x10;
constexpr u64 kLevelMarkersList = 0x18;
constexpr u64 kMarkerId = 0x10;
constexpr u64 kMarkerPosition = 0x14;
constexpr u64 kMarkerTypeGuid = 0x20;
constexpr u64 kDeathPosition = 0x24;
constexpr u64 kDeathMainMapPosition = 0x30;
constexpr u64 kDeathInFastTravelRoom = 0x49;
constexpr u64 kDeathLevelId = 0x58;
constexpr u32 kMaxComponents = 256;
constexpr u32 kMaxMarkerLevels = 64;
constexpr u32 kMaxMarkers = 512;
constexpr u64 kStateDims = 0x10;
constexpr u64 kStateChunks = 0x18;
constexpr u64 kStateDiscoveredZDoorZones = 0x28;
constexpr u64 kStateEnteredZDoors = 0x38;
constexpr u64 kStateDiscoveredSecrets = 0x48;
constexpr u64 kStateEnteredZones = 0x50;
constexpr u64 kStateActivated = 0x58;
constexpr u32 kMaxListItems = 4096;

constexpr u32 kMaxSystems = 512;
constexpr u32 kMaxDictEntries = 4096;
constexpr u32 kMaxWorldRooms = 1024;
constexpr u32 kMaxChunks = 64 * 64;

struct Io {
    const Memory& m;
    bool Bytes(u64 at, void* out, std::size_t n) const {
        return at != 0 && at <= UINT64_MAX - n && m.read && m.read(at, out, n);
    }
    template <typename T>
    bool Get(u64 at, T& out) const {
        return Bytes(at, &out, sizeof(T));
    }
    /// A plausible object pointer: non-null, 8-aligned and readable.
    bool Ptr(u64 at, u64& out) const {
        if (!Get(at, out) || out == 0 || (out & 7) != 0)
            return false;
        u64 probe;
        return Get(out, probe);
    }
    std::string CStr(u64 at, std::size_t max = 96) const {
        std::string s;
        char buf[32];
        while (s.size() < max) {
            std::size_t got = sizeof buf;
            if (!Bytes(at + s.size(), buf, sizeof buf)) {
                // The string may end just before an unmapped page: fall back to single bytes.
                if (!Bytes(at + s.size(), buf, 1))
                    return {};
                got = 1;
            }
            for (std::size_t k = 0; k < got; ++k) {
                const char c = buf[k];
                if (c == '\0')
                    return s;
                if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E)
                    return {};
                s.push_back(c);
            }
        }
        return {};
    }
    /// List<int>; a null list is empty.
    bool IntList(u64 list, std::vector<s32>& out) const {
        out.clear();
        if (list == 0)
            return true;
        u64 items = 0;
        s32 size = 0;
        if (!Get(list + kListSize, size) || size < 0 || static_cast<u32>(size) > kMaxListItems ||
            !Get(list + kListItems, items))
            return false;
        if (size == 0)
            return true;
        u64 cap = 0;
        if (!items || !Get(items + kArrayLength, cap) || cap < static_cast<u64>(size))
            return false;
        out.resize(static_cast<std::size_t>(size));
        return Bytes(items + kArrayData, out.data(), out.size() * 4);
    }
    /// List<string>; a null list is empty.
    bool StringList(u64 list, std::vector<std::string>& out) const {
        out.clear();
        if (list == 0)
            return true;
        u64 items = 0, cap = 0;
        s32 size = 0;
        if (!Get(list + kListSize, size) || size < 0 || static_cast<u32>(size) > kMaxListItems ||
            !Get(list + kListItems, items))
            return false;
        if (size == 0)
            return true;
        if (!items || !Get(items + kArrayLength, cap) || cap < static_cast<u64>(size))
            return false;
        std::vector<u64> ptrs(static_cast<std::size_t>(size));
        if (!Bytes(items + kArrayData, ptrs.data(), ptrs.size() * 8))
            return false;
        for (const u64 p : ptrs) {
            std::string v;
            if (!String(p, v, 128))
                return false;
            out.push_back(std::move(v));
        }
        return true;
    }
    /// System.String (UTF-16) as UTF-8; empty when absent or implausible.
    bool String(u64 obj, std::string& out, s32 max = 256) const {
        out.clear();
        if (obj == 0)
            return true; // null string
        s32 len = 0;
        if (!Get(obj + kStringLength, len) || len < 0 || len > max)
            return false;
        std::vector<std::uint16_t> chars(static_cast<std::size_t>(len));
        if (len && !Bytes(obj + kStringChars, chars.data(), chars.size() * 2))
            return false;
        for (const std::uint16_t c : chars) {
            if (c < 0x80) {
                out.push_back(static_cast<char>(c));
            } else if (c < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (c >> 6)));
                out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xE0 | (c >> 12)));
                out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            }
        }
        return true;
    }
};

u64 Fnv(u64 h, const void* p, std::size_t n) {
    const auto* b = static_cast<const u8*>(p);
    for (std::size_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 0x100000001b3ULL;
    return h;
}

} // namespace

bool Reader::ResolveDelta(const Memory& mem, State& out) {
    if (delta_known_ && last_main_base_ == mem.main_base)
        return true;
    if (samples_ < next_delta_try_)
        return false;
    next_delta_try_ = samples_ + 600; // a full search is ~40k small reads: retry every 10 s
    delta_known_ = false;
    last_main_base_ = mem.main_base;
    Io io{mem};
    const auto matches = [&](s64 delta) {
        u64 v[13];
        const u64 at = mem.main_base + kMetadataRegistration + static_cast<u64>(delta);
        return io.Bytes(at, v, sizeof v) && v[0] == kGenericClassesCount &&
               v[2] == kGenericInstsCount && v[6] == kTypesCount &&
               v[10] == kTypeDefinitionsCount && v[12] == kTypeDefinitionsCount;
    };
    // The shift is page-sized; try 0 first, then pages on both sides (up to +128 / -16 MiB).
    if (matches(0)) {
        delta_ = 0;
        delta_known_ = true;
    } else {
        for (s64 k = 1; k <= 0x8000 && !delta_known_; ++k) {
            const s64 d = k * 0x1000;
            if (matches(d)) {
                delta_ = d;
                delta_known_ = true;
            } else if (k <= 0x1000 && matches(-d)) {
                delta_ = -d;
                delta_known_ = true;
            }
        }
    }
    if (!delta_known_) {
        out.status = "IL2CPP registration not found (wrong build or not loaded yet)";
        return false;
    }
    sysmgr_class_ = 0;
    sysmgr_ = 0;
    rm_ = ex_ = 0;
    return true;
}

bool Reader::ResolveSystemManager(const Memory& mem, State& out) {
    Io io{mem};
    u64 klass = 0;
    const u64 slot = mem.main_base + kSystemManagerTypeInfo + static_cast<u64>(delta_);
    if (!io.Get(slot, klass)) {
        out.status = "SystemManager TypeInfo slot unreadable";
        return false;
    }
    if (klass == kSystemManagerEncoded || (klass & 1)) {
        out.status = "SystemManager not initialised yet";
        return false;
    }
    if (klass != sysmgr_class_) {
        u64 name_ptr = 0, ns_ptr = 0;
        if (!io.Get(klass + kClassName, name_ptr) || !io.Get(klass + kClassNamespace, ns_ptr) ||
            io.CStr(name_ptr) != "SystemManager" ||
            io.CStr(ns_ptr) != "MotherBase.EntityFramework") {
            out.status = "TypeInfo slot does not hold SystemManager";
            return false;
        }
        sysmgr_class_ = klass;
        sf_offset_ = 0;
        sysmgr_ = 0;
    }
    out.stage = 2;
    // static_fields: the candidate whose first static (Instance) is an object of this class.
    const auto try_offset = [&](int off) -> u64 {
        u64 sf = 0, inst = 0, inst_class = 0;
        if (io.Ptr(klass + static_cast<u64>(off), sf) && io.Get(sf, inst) && inst != 0 &&
            io.Get(inst, inst_class) && inst_class == klass)
            return inst;
        return 0;
    };
    u64 inst = sf_offset_ ? try_offset(sf_offset_) : 0;
    if (!inst) {
        for (const int off : kStaticFieldsCandidates) {
            inst = try_offset(off);
            if (inst) {
                sf_offset_ = off;
                break;
            }
        }
    }
    if (!inst) {
        out.status = "SystemManager.Instance not set yet";
        return false;
    }
    if (inst != sysmgr_) {
        sysmgr_ = inst;
        rm_ = ex_ = 0;
    }
    out.stage = 3;
    out.static_fields_offset = sf_offset_;
    return true;
}

bool Reader::FindSystems(const Memory& mem, State& out) {
    Io io{mem};
    const auto still = [&](u64 obj, u64 klass) {
        u64 k = 0;
        return obj && io.Get(obj, k) && k == klass;
    };
    if (still(rm_, rm_class_) && still(ex_, ex_class_)) {
        out.stage = 4;
        out.system_count = system_count_;
        out.system_names = system_names_;
        return true;
    }
    rm_ = ex_ = 0;
    std::string names;
    int count = 0;
    for (const u64 field : kSysMgrSystems) {
        u64 arr = 0, len = 0;
        if (!io.Ptr(sysmgr_ + field, arr) || !io.Get(arr + kArrayLength, len) || len > kMaxSystems)
            continue;
        for (u64 i = 0; i < len; ++i) {
            u64 obj = 0, klass = 0;
            if (!io.Get(arr + kArrayData + 8 * i, obj) || obj == 0 || !io.Get(obj, klass))
                continue;
            auto it = class_names_.find(klass);
            if (it == class_names_.end()) {
                u64 np = 0, nsp = 0;
                std::string full;
                if (io.Get(klass + kClassName, np) && io.Get(klass + kClassNamespace, nsp)) {
                    const std::string ns = io.CStr(nsp), nm = io.CStr(np);
                    full = ns.empty() ? nm : ns + "." + nm;
                }
                it = class_names_.emplace(klass, full).first;
            }
            ++count;
            const std::string& full = it->second;
            if (!full.empty()) {
                const auto dot = full.rfind('.');
                if (!names.empty())
                    names += ", ";
                names += dot == std::string::npos ? full : full.substr(dot + 1);
            }
            if (full == "RoomManager.RoomManagerSystem") {
                rm_ = obj;
                rm_class_ = klass;
            } else if (full == "ExplorationSystem") {
                ex_ = obj;
                ex_class_ = klass;
            }
        }
    }
    system_count_ = count;
    system_names_ = names;
    out.system_count = count;
    out.system_names = names;
    if (!rm_ && !ex_) {
        out.status = count ? "systems found, but not RoomManagerSystem / ExplorationSystem"
                           : "no systems registered yet";
        return false;
    }
    out.stage = 4;
    return true;
}

const std::string& Reader::ClassOf(const Memory& mem, u64 obj) {
    static const std::string kNone;
    Io io{mem};
    u64 klass = 0;
    if (!obj || !io.Get(obj, klass) || !klass || (klass & 7) != 0)
        return kNone;
    auto it = class_names_.find(klass);
    if (it == class_names_.end()) {
        u64 np = 0, nsp = 0;
        std::string full;
        if (io.Get(klass + kClassName, np) && io.Get(klass + kClassNamespace, nsp)) {
            const std::string ns = io.CStr(nsp), nm = io.CStr(np);
            full = ns.empty() ? nm : ns + "." + nm;
        }
        if (full.empty())
            return kNone; // not a class (or not readable now): do not cache
        if (class_names_.size() > 4096)
            class_names_.clear();
        it = class_names_.emplace(klass, full).first;
    }
    return it->second;
}

namespace {
/// Entries of a Dictionary<K, V> whose key and value are references or ints: {int hashCode,
/// int next, key at +8, value at +0x10}, stride 0x18. Freed entries have a null value.
bool DictValues(const Io& io, u64 dict, u32 max, std::vector<std::pair<u64, u64>>& out) {
    out.clear();
    u64 entries = 0, cap = 0;
    s32 count = 0;
    if (!dict || !io.Get(dict + kDictCount, count) || count < 0 || static_cast<u32>(count) > max)
        return false;
    if (count == 0)
        return true;
    if (!io.Ptr(dict + kDictEntries, entries) || !io.Get(entries + kArrayLength, cap) ||
        cap < static_cast<u64>(count))
        return false;
    std::vector<u64> raw(static_cast<std::size_t>(count) * 3);
    if (!io.Bytes(entries + kArrayData, raw.data(), raw.size() * 8))
        return false;
    for (s32 i = 0; i < count; ++i) {
        const u64 key = raw[static_cast<std::size_t>(i) * 3 + 1];
        const u64 value = raw[static_cast<std::size_t>(i) * 3 + 2];
        const s32 next = static_cast<s32>(raw[static_cast<std::size_t>(i) * 3] >> 32);
        if (value && next >= -1)
            out.emplace_back(key, value);
    }
    return true;
}
} // namespace

void Reader::ReadPlayerComponents(const Memory& mem, u64 explorer, State& out) {
    Io io{mem};
    u64 entity = 0, cache = 0;
    if (!io.Ptr(explorer + kExplorerSelfEntity, entity) ||
        ClassOf(mem, entity) != "MotherBase.EntityFramework.Entity" ||
        !io.Ptr(entity + kEntityComponentCache, cache))
        return;
    std::vector<std::pair<u64, u64>> comps;
    if (!DictValues(io, cache, kMaxComponents, comps))
        return;
    const auto guid_of = [&](u64 level_id, std::string& g) {
        u64 s = 0;
        return level_id && io.Get(level_id + kSavableGuid, s) && io.String(s, g, 64) && !g.empty();
    };
    for (const auto& [key, comp] : comps) {
        const std::string& cls = ClassOf(mem, comp);
        if (cls == "MapMarkersComponent") {
            u64 levels = 0;
            std::vector<std::pair<u64, u64>> by_level;
            if (!io.Get(comp + kMarkersLevels, levels))
                continue;
            if (levels && !DictValues(io, levels, kMaxMarkerLevels, by_level))
                continue;
            std::vector<MapMarker> markers;
            bool ok = true;
            for (const auto& [level_key, data] : by_level) {
                u64 level = 0, list = 0, items = 0;
                s32 size = 0;
                std::string guid;
                if (!io.Get(data + kLevelMarkersLevel, level) || !guid_of(level ? level : level_key, guid) ||
                    !io.Get(data + kLevelMarkersList, list)) {
                    ok = false;
                    break;
                }
                if (!list)
                    continue;
                if (!io.Get(list + kListSize, size) || size < 0 || static_cast<u32>(size) > kMaxMarkers ||
                    !io.Get(list + kListItems, items) || (size && !items)) {
                    ok = false;
                    break;
                }
                for (s32 i = 0; i < size && markers.size() < kMaxMarkers; ++i) {
                    u64 m = 0, type = 0;
                    MapMarker mk;
                    float pos[2];
                    if (!io.Get(items + kArrayData + 8 * static_cast<u64>(i), m) || !m ||
                        !io.Get(m + kMarkerId, mk.id) || !io.Get(m + kMarkerPosition, pos) ||
                        !io.Get(m + kMarkerTypeGuid, type) || !io.String(type, mk.type_guid, 64)) {
                        ok = false;
                        break;
                    }
                    if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]))
                        continue;
                    mk.level_guid = guid;
                    mk.x = pos[0];
                    mk.y = pos[1];
                    markers.push_back(std::move(mk));
                }
                if (!ok)
                    break;
            }
            if (ok) {
                out.markers = std::move(markers);
                out.markers_ok = true;
            }
        } else if (cls == "DeathResultComponent") {
            u64 level = 0;
            float p[3], mp[3];
            u8 fast = 0;
            DeathSpot d;
            if (io.Get(comp + kDeathLevelId, level) && io.Get(comp + kDeathPosition, p) &&
                io.Get(comp + kDeathMainMapPosition, mp) && io.Get(comp + kDeathInFastTravelRoom, fast) &&
                std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(mp[0]) && std::isfinite(mp[1])) {
                // No level: the player has not died yet (the game shows no death spot).
                d.known = level != 0 && guid_of(level, d.level_guid);
                d.x = p[0], d.y = p[1];
                d.main_x = mp[0], d.main_y = mp[1];
                d.in_fast_travel_room = fast != 0;
                out.death = std::move(d);
            }
        }
    }
}

void Reader::Sample(const Memory& mem, bool with_fog, State& out) {
    ++samples_;
    out = State{};
    Io io{mem};
    if (!ResolveDelta(mem, out))
        return;
    out.stage = 1;
    out.delta = delta_;
    if (!ResolveSystemManager(mem, out) || !FindSystems(mem, out))
        return;
    out.status = "ok";

    // Room manager: current chunk and room. Read twice; a change between reads is a transition.
    if (rm_) {
        s32 c1[2], c2[2], p[2];
        u64 h1 = 0, h2 = 0;
        if (io.Get(rm_ + kRmCurChunk, c1) && io.Get(rm_ + kRmPrevChunk, p) &&
            io.Get(rm_ + kRmCurHandle, h1) && io.Get(rm_ + kRmCurChunk, c2) &&
            io.Get(rm_ + kRmCurHandle, h2) && c1[0] == c2[0] && c1[1] == c2[1] && h1 == h2) {
            out.rm_ok = true;
            out.chunk_x = c1[0];
            out.chunk_y = c1[1];
            out.prev_x = p[0];
            out.prev_y = p[1];
            u64 wrd = 0, name = 0, id = 0, inst = 0;
            s32 pos[2];
            s32 uid = -1;
            if (h1 && io.Ptr(h1 + kHandleWorldRoomData, wrd) && io.Get(wrd + kWrdWorldName, name) &&
                io.Get(wrd + kWrdPosition, pos) && io.Get(wrd + kWrdRuntimeNumber, uid) &&
                io.Get(wrd + kWrdId, id) && io.Get(wrd + kWrdInstanceName, inst) &&
                io.String(name, out.world_name) && io.String(id, out.room_id) &&
                io.String(inst, out.room_instance)) {
                out.room_ok = true;
                out.room_uid = uid;
                out.room_x = pos[0];
                out.room_y = pos[1];
                out.stage = 5;
            }
        }
    }

    // Exploration system: the room it tracks, the explorer's last map position and the fog.
    if (ex_) {
        u64 ri = 0, wrd = 0, explorer = 0;
        s32 uid = -1;
        if (io.Ptr(ex_ + kExCurrentRoomInstance, ri) &&
            io.Ptr(ri + kRoomInstanceWorldRoomData, wrd) && io.Get(wrd + kWrdRuntimeNumber, uid)) {
            out.ex_ok = true;
            out.ex_room_uid = uid;
        }
        if (io.Ptr(ex_ + kExExplorer, explorer)) {
            float v[3];
            if (io.Get(explorer + kExplorerLastMapPos, v)) {
                out.map_x = v[0];
                out.map_y = v[1];
                out.map_z = v[2];
            }
            u64 last_save = 0;
            if (io.Get(explorer + kExplorerLastSaveRoom, last_save))
                io.String(last_save, out.last_save_room, 160);
            if (u64 level = 0, gs = 0; io.Get(explorer + kExplorerLevelId, level) && level &&
                                        io.Get(level + kSavableGuid, gs))
                io.String(gs, out.level_guid, 64);
            if (with_fog)
                ReadPlayerComponents(mem, explorer, out);
            u64 dict = 0, entries = 0, cap = 0;
            s32 count = 0;
            if (with_fog && io.Ptr(explorer + kExplorerStates, dict) &&
                io.Get(dict + kDictCount, count) && count >= 0 &&
                static_cast<u32>(count) <= kMaxDictEntries && io.Ptr(dict + kDictEntries, entries) &&
                io.Get(entries + kArrayLength, cap) && cap >= static_cast<u64>(count)) {
                // Strings of the current world's rooms -> uid, to name the dictionary's keys.
                std::unordered_map<std::string, s32> uid_by_key;
                u64 world = 0, list = 0, items = 0;
                s32 size = 0;
                if (io.Ptr(ex_ + kExCurrentWorld, world) && io.Ptr(world + kWorldDataRooms, list) &&
                    io.Get(list + kListSize, size) && size >= 0 &&
                    static_cast<u32>(size) <= kMaxWorldRooms && io.Ptr(list + kListItems, items)) {
                    for (s32 i = 0; i < size; ++i) {
                        u64 r = 0, sid = 0, sinst = 0;
                        s32 ruid = -1;
                        if (!io.Get(items + kArrayData + 8 * static_cast<u64>(i), r) || !r ||
                            !io.Get(r + kWrdRuntimeNumber, ruid))
                            continue;
                        std::string s;
                        if (io.Get(r + kWrdId, sid) && io.String(sid, s) && !s.empty())
                            uid_by_key.emplace(s, ruid);
                        if (io.Get(r + kWrdInstanceName, sinst) && io.String(sinst, s) && !s.empty())
                            uid_by_key.emplace(s, ruid);
                    }
                }
                u64 hash = 0xcbf29ce484222325ULL;
                u64 ehash = 0xcbf29ce484222325ULL;
                bool ok = true;
                for (s32 i = 0; i < count && ok; ++i) {
                    const u64 e = entries + kArrayData + kDictEntryStride * static_cast<u64>(i);
                    u64 key = 0, value = 0;
                    if (!io.Get(e + 8, key) || !io.Get(e + 16, value)) {
                        ok = false;
                        break;
                    }
                    if (!key || !value)
                        continue; // a freed entry
                    FogRoom fr;
                    s32 dims[2];
                    u64 arr = 0, n = 0;
                    if (!io.String(key, fr.key) || !io.Get(value + kStateDims, dims) ||
                        !io.Get(value + kStateChunks, arr)) {
                        ok = false;
                        break;
                    }
                    fr.w = dims[0];
                    fr.h = dims[1];
                    // Exploration zones, Z-doors, secrets and activated elements. A list that
                    // cannot be read leaves that list empty; the chunk states still count.
                    {
                        u64 l = 0;
                        if (io.Get(value + kStateEnteredZones, l))
                            io.IntList(l, fr.entered_zones);
                        if (io.Get(value + kStateEnteredZDoors, l))
                            io.IntList(l, fr.entered_zdoors);
                        if (io.Get(value + kStateDiscoveredZDoorZones, l))
                            io.IntList(l, fr.discovered_zdoor_zones);
                        if (io.Get(value + kStateDiscoveredSecrets, l))
                            io.IntList(l, fr.discovered_secrets);
                        if (io.Get(value + kStateActivated, l))
                            io.StringList(l, fr.activated);
                    }
                    if (arr && io.Get(arr + kArrayLength, n) && n <= kMaxChunks) {
                        std::vector<s32> raw(static_cast<std::size_t>(n));
                        if (n && !io.Bytes(arr + kArrayData, raw.data(), raw.size() * 4)) {
                            ok = false;
                            break;
                        }
                        fr.states.reserve(raw.size());
                        for (const s32 s : raw)
                            fr.states.push_back(static_cast<u8>(s < 0 || s > 255 ? 0 : s));
                    }
                    if (const auto it = uid_by_key.find(fr.key); it != uid_by_key.end()) {
                        fr.uid = it->second;
                        ++out.fog_matched;
                    }
                    hash = Fnv(hash, fr.key.data(), fr.key.size());
                    hash = Fnv(hash, fr.states.data(), fr.states.size());
                    ehash = Fnv(ehash, fr.key.data(), fr.key.size() + 1);
                    for (const auto* ids : {&fr.entered_zones, &fr.entered_zdoors, &fr.discovered_zdoor_zones,
                                            &fr.discovered_secrets}) {
                        const u32 n32 = static_cast<u32>(ids->size());
                        hash = Fnv(hash, &n32, 4);
                        hash = Fnv(hash, ids->data(), ids->size() * 4);
                        ehash = Fnv(ehash, &n32, 4);
                        ehash = Fnv(ehash, ids->data(), ids->size() * 4);
                    }
                    for (const auto& a : fr.activated) {
                        hash = Fnv(hash, a.data(), a.size() + 1);
                        ehash = Fnv(ehash, a.data(), a.size() + 1);
                    }
                    out.fog.push_back(std::move(fr));
                }
                if (ok) {
                    hash = Fnv(hash, out.last_save_room.data(), out.last_save_room.size());
                    ehash = Fnv(ehash, out.last_save_room.data(), out.last_save_room.size());
                    // Markers and the death spot are drawn into the map too.
                    for (const auto& m : out.markers) {
                        ehash = Fnv(ehash, m.level_guid.data(), m.level_guid.size() + 1);
                        ehash = Fnv(ehash, m.type_guid.data(), m.type_guid.size() + 1);
                        ehash = Fnv(ehash, &m.x, 4);
                        ehash = Fnv(ehash, &m.y, 4);
                    }
                    if (out.death.known) {
                        const auto& d = out.death;
                        ehash = Fnv(ehash, d.level_guid.data(), d.level_guid.size() + 1);
                        ehash = Fnv(ehash, &d.x, 4);
                        ehash = Fnv(ehash, &d.y, 4);
                        ehash = Fnv(ehash, &d.in_fast_travel_room, 1);
                    }
                    out.fog_ok = true;
                    out.fog_entries = count;
                    out.fog_hash = hash;
                    out.explore_hash = ehash;
                } else {
                    out.fog.clear();
                    out.fog_matched = 0;
                }
            }
        }
    }
}

} // namespace bc_live
