// SPDX-License-Identifier: GPL-3.0-or-later
// Unit test for bc_live against a fake guest laid out as documented in bc_live.h.
#include <cstdio>

#include "bc_live.h"
#include "fake_guest.h"

static int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static void RunCase(std::int64_t delta) {
    FakeGuest g;
    const auto expect = g.Build(delta);
    bc_live::Memory mem;
    mem.main_base = g.main_base;
    mem.main_size = g.main_size;
    mem.read = [&g](std::uint64_t at, void* out, std::size_t n) { return g.Read(at, out, n); };
    bc_live::Reader reader;
    bc_live::State s;
    reader.Sample(mem, true, s);
    std::printf("delta %lld: stage %d status '%s' sf 0x%X systems [%s] chunk %d,%d room %d '%s' "
                "id '%s' ex %d fog %d/%d cur ",
                static_cast<long long>(delta), s.stage, s.status.c_str(), s.static_fields_offset,
                s.system_names.c_str(), s.chunk_x, s.chunk_y, s.room_uid, s.world_name.c_str(),
                s.room_id.c_str(), s.ex_room_uid, s.fog_matched, s.fog_entries);
    for (const auto& f : s.fog)
        for (auto st : f.states)
            std::printf("%d", st);
    std::printf("\n");
    CHECK(s.stage == 5);
    CHECK(s.delta == expect.delta);
    CHECK(s.static_fields_offset == 0xB8);
    CHECK(s.system_count == 3);
    CHECK(s.rm_ok && s.chunk_x == expect.chunk_x && s.chunk_y == expect.chunk_y);
    CHECK(s.room_ok && s.room_uid == expect.uid && s.world_name == expect.world);
    CHECK(s.room_id == expect.id);
    CHECK(s.ex_ok && s.ex_room_uid == expect.uid);
    CHECK(s.map_x == 1.5f && s.map_y == -2.25f);
    CHECK(s.fog_ok && s.fog_entries == 2 && s.fog_matched == 1 && s.fog.size() == 1);
    CHECK(!s.fog.empty() && s.fog[0].uid == expect.uid && s.fog[0].w == 2 &&
          s.fog[0].states == std::vector<std::uint8_t>({3, 2, 1, 0}));
    // A second sample reuses the cached resolution and gives the same answer.
    bc_live::State s2;
    reader.Sample(mem, false, s2);
    CHECK(s2.stage == 5 && s2.room_uid == expect.uid && !s2.fog_ok);
}

int main() {
    RunCase(0);
    RunCase(0x5000);   // data segment shifted up (NCE)
    RunCase(-0x3000);  // and down
    // A wrong class in the TypeInfo slot must fail closed.
    {
        FakeGuest g;
        g.Build(0);
        const std::uint64_t bogus = g.Klass("Other", "NotSystemManager");
        g.Put<std::uint64_t>(g.main_base + 0x6D47368, bogus);
        bc_live::Memory mem{g.main_base, g.main_size,
                            [&g](std::uint64_t at, void* out, std::size_t n) { return g.Read(at, out, n); }};
        bc_live::Reader reader;
        bc_live::State s;
        reader.Sample(mem, true, s);
        std::printf("bogus class: stage %d status '%s'\n", s.stage, s.status.c_str());
        CHECK(s.stage == 1 && !s.room_ok);
    }
    // An unresolved (still encoded) slot reports "not initialised" without reading further.
    {
        FakeGuest g;
        g.Build(0);
        g.Put<std::uint64_t>(g.main_base + 0x6D47368, 0x200117D1);
        bc_live::Memory mem{g.main_base, g.main_size,
                            [&g](std::uint64_t at, void* out, std::size_t n) { return g.Read(at, out, n); }};
        bc_live::Reader reader;
        bc_live::State s;
        reader.Sample(mem, true, s);
        std::printf("encoded slot: stage %d status '%s'\n", s.stage, s.status.c_str());
        CHECK(s.stage == 1);
    }
    std::printf(failures ? "FAILED (%d)\n" : "ALL PASSED\n", failures);
    return failures ? 1 : 0;
}
