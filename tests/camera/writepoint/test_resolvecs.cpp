// W1-H. camera::detail::ResolveSpectatorCs -- which of the CR15NetGame's pointers is handed to
// CS.LOOKUP_BY_SYMBOL, and every refusal that stops it being handed anything.
//
// ARCH-12 R1: the netGame carries TWO spaces. `*(ng+0x2B08)` (NETGAME.CS_SPACE_OFF) is the
// CncaGameSpace; `*(ng+0x2B10)` (NETGAME.SPECTATORCAM_CS_OFF -- a misnomer, ARCH-12 R5) is the
// LEVEL space built at map load, and the spectator camera CS is registered in THAT one. Both of the
// engine's own camera consumers hand +0x2B10, unchanged, to the lookup. The +0x8 slot is the
// CR15Game back-pointer and is not a space at all.
//
// So there are three wrong answers to discriminate against, and the rig makes each one WRONG rather
// than merely unanswered:
//
//   - the level space at +0x2B10 carries CAM.SPECTATOR_SYMBOL and 35 systems;
//   - the CncaGameSpace at +0x2B08 is equally constructed and equally Ready, carries 99 systems,
//     and does NOT carry the symbol -- so an accessor that reads +0x2B08 gets a null answer where
//     this one resolves. That is the recorded L5 #3 state, and it is the must-fail control;
//   - +0x8 holds an object whose +0x420 is all zeroes, so an accessor reading it refuses the table
//     check where this one resolves.
//
// The two spaces differ in CONTENT, not only in count (ARCH-12 S5.3): FakeLookup below actually
// searches the table it is handed, so "the level space answers and the csSpace does not" is a
// property of the rig's memory rather than of a global answer variable a test could set either way.
//
// The netGame buffer is asserted to cover +0x2B10 explicitly. A rig shorter than that which still
// passes is a rig that is not reading the field (ARCH-10 S3.4).
//
// ARCH-11 rule 8 (DESIGN S13.1.1): ownership of a PATH is the wave table's, ownership of an
// ASSERTION is its SUBJECT's. The case analysis of ResolveSpectatorCs belongs with
// src/engine/camera_writepoint.* whatever else calls it, so the TEST_CASE that was at
// tests/camera/test_camera.cpp:735 -- W2-E's path, W1-H's subject -- is here (W2-E deleted the
// original in b933647), rewritten against the ARCH-12 contract rather than deleted: same recorded
// inputs, and the +0x2B08 lookup answering null is now the differential arm PASSING.

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include <doctest.h>

#include "bootstrap/tableguard.h"
#include "engine/camera_writepoint.h"
#include "engine/camera_writepoint_facts.h"
#include "generated/facts_generated.h"

namespace {

namespace cf = replay::camera_facts;
namespace det = replay::camera::detail;
namespace tg = replay::tableguard;
using replay::camera::Refusal;

int      g_lookupCalls = 0;
void*    g_lookupFirstSpace = nullptr;
uint64_t g_lookupSawSymbol = 0;

// A stand-in for CS.LOOKUP_BY_SYMBOL that SEARCHES. The element layout below -- an 8-byte symbol
// then an 8-byte object, 16 bytes to a record -- is this fixture's own; the product does not depend
// on it and nothing here claims it is the engine's. What it buys is that the answer comes out of
// the space it was handed, so a case can make two spaces answer differently without a test-only
// switch deciding which one is asked.
void* __fastcall FakeLookup(void* space, uint64_t symbol) {
    if (g_lookupCalls == 0) g_lookupFirstSpace = space;
    ++g_lookupCalls;
    g_lookupSawSymbol = symbol;
    if (!space) return nullptr;

    const auto* table = static_cast<const uint8_t*>(space) + cf::kSpaceCsTableOff;
    uint64_t base = 0, count = 0;
    memcpy(&base, table + tg::kBaseOff, sizeof base);
    memcpy(&count, table + tg::kCountOff, sizeof count);
    const auto* elements = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(base));
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t key = 0, value = 0;
        memcpy(&key, elements + i * 16, sizeof key);
        memcpy(&value, elements + i * 16 + 8, sizeof value);
        if (key == symbol) return reinterpret_cast<void*>(static_cast<uintptr_t>(value));
    }
    return nullptr;
}

uint64_t Addr(const std::vector<uint8_t>& v) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(v.data()));
}
void Put64(std::vector<uint8_t>& v, size_t off, uint64_t value) {
    memcpy(v.data() + off, &value, sizeof value);
}

// One constructed CncaGameSpace: the object, its element block, and the CS the symbol resolves to.
// The header it writes is tableguard.h's stated postcondition for a constructed CTable -- a real
// base, bytes covering capacity elements, a count inside the capacity, flags bit 0 clear.
struct Space {
    std::vector<uint8_t> obj;
    std::vector<uint8_t> elements;
    std::vector<uint8_t> cs;

    Space(size_t systems, uint64_t symbol)
        : obj(cf::kSpaceCsTableOff + tg::kHeaderBytes, 0),
          elements(systems * 16, 0),
          cs(cf::kCsLocalIndexOff + sizeof(uint16_t), 0) {
        for (size_t i = 0; i < systems; ++i) {
            Put64(elements, i * 16, 0x1000ull + i);  // some other system's symbol
        }
        if (symbol) {
            // Not element zero and not the last: an accessor that only ever read an end of the
            // block would still find it there.
            const size_t slot = systems / 2;
            Put64(elements, slot * 16, symbol);
            Put64(elements, slot * 16 + 8, Addr(cs));
        }
        const size_t t = cf::kSpaceCsTableOff;
        Put64(obj, t + tg::kBaseOff, Addr(elements));
        Put64(obj, t + tg::kBytesOff, elements.size());
        Put64(obj, t + tg::kAllocatorOff, 0);
        Put64(obj, t + tg::kGrowOff, 4);
        Put64(obj, t + tg::kCapacityOff, systems);
        Put64(obj, t + tg::kCountOff, systems);
        Put64(obj, t + tg::kFlagsOff, 0);
    }
};

// ARCH-12 S5.3's shared rig contract. A CR15NetGame with both spaces populated and DISTINCT, plus
// the CR15Game the +0x8 slot really holds. Each case below breaks exactly one thing, so the refusal
// it gets can only have come from that one thing.
struct Rig {
    Space levelSpace{35, cf::kSpectatorSymbol};  // space L: 35 systems, HAS the spectator symbol
    Space csSpace{99, 0};                        // space C: 99 systems, has NOT -- the L5 #3 state
    std::vector<uint8_t> netGame;
    std::vector<uint8_t> game;

    Rig()
        : netGame(facts::NETGAME_SPECTATORCAM_CS_OFF + sizeof(uint64_t), 0),
          game(cf::kSpaceCsTableOff + tg::kHeaderBytes, 0) {
        Put64(netGame, facts::NETGAME_SPECTATORCAM_CS_OFF, Addr(levelSpace.obj));
        Put64(netGame, facts::NETGAME_CS_SPACE_OFF, Addr(csSpace.obj));
        Put64(netGame, facts::IDENT_NETGAME_GAMESPACE_OFF, Addr(game));
        g_lookupCalls = 0;
        g_lookupFirstSpace = nullptr;
        g_lookupSawSymbol = 0;
    }

    det::Resolution Resolve() { return det::ResolveSpectatorCs(netGame.data(), &FakeLookup); }
};

}  // namespace

// The alias camera_writepoint_facts.h now takes from the registry. Read back as literals here on
// purpose: a fixture that imports the product's constant cannot fail when the constant is wrong
// (ARCH-10 S3.3).
static_assert(cf::kSpaceCsTableOff == 0x420u, "CS.TABLE_OFF drifted");
static_assert(facts::NETGAME_SPECTATORCAM_CS_OFF == 0x2B10u, "the level space slot moved");
static_assert(facts::NETGAME_CS_SPACE_OFF == 0x2B08u, "NETGAME.CS_SPACE_OFF drifted");
static_assert(facts::IDENT_NETGAME_GAMESPACE_OFF == 0x8u, "the CR15Game back-pointer moved");
static_assert(facts::NETGAME_SPECTATORCAM_CS_OFF != facts::NETGAME_CS_SPACE_OFF,
              "two slots that are one slot cannot discriminate between two spaces");

TEST_CASE("resolve: the lookup runs on the level space at +0x2B10, not the CncaGameSpace at +0x2B08") {
    Rig rig;
    REQUIRE(rig.netGame.size() >= 0x2B18u);
    CHECK_MESSAGE(rig.netGame.size() >= facts::NETGAME_SPECTATORCAM_CS_OFF + sizeof(uint64_t),
                  "a netGame buffer that does not cover +0x2B10 is not reading the field");
    // Both spaces are Ready. If only one of them were, the case would be discriminating on
    // constructedness rather than on which slot the product reads.
    REQUIRE(tg::CheckSpaceTable(rig.levelSpace.obj.data(), cf::kSpaceCsTableOff,
                                cf::kSpaceCsTableStride, false).verdict == tg::TableVerdict::Ready);
    REQUIRE(tg::CheckSpaceTable(rig.csSpace.obj.data(), cf::kSpaceCsTableOff,
                                cf::kSpaceCsTableStride, false).verdict == tg::TableVerdict::Ready);

    SUBCASE("passing input: both spaces present, and only the level space carries the symbol") {
        const det::Resolution r = rig.Resolve();

        CHECK(r.refusal == Refusal::None);
        CHECK(r.levelSpace == Addr(rig.levelSpace.obj));
        CHECK(r.csSpace == Addr(rig.csSpace.obj));
        CHECK(r.levelSpace != r.csSpace);
        CHECK(r.spaceTable.verdict == tg::TableVerdict::Ready);
        CHECK(r.csFromLookup == Addr(rig.levelSpace.cs));
        CHECK_MESSAGE(g_lookupFirstSpace == rig.levelSpace.obj.data(),
                      "CS.LOOKUP_BY_SYMBOL was handed something other than the +0x2B10 level space");
        CHECK(g_lookupFirstSpace != rig.csSpace.obj.data());
        CHECK(g_lookupFirstSpace != rig.game.data());
        CHECK(g_lookupSawSymbol == cf::kSpectatorSymbol);

        // The differential ran and answered: this is the arm that makes the bind falsifiable.
        CHECK(g_lookupCalls == 2);
        CHECK(r.csSpaceTable.verdict == tg::TableVerdict::Ready);
        CHECK(r.csFromCsSpace == 0);
        CHECK(strstr(r.why, "the differential passed") != nullptr);
    }

    SUBCASE("the must-fail control: the recorded L5 #3 state, the lookup asked on the csSpace") {
        // Exactly what an accessor on +0x2B08 sees, and exactly what L5 #3 recorded: a Ready space
        // of 99 systems with no R14NetSpectatorCamera among them. Reached here by putting that
        // space in the slot the product reads, so the refusal comes from the space's content.
        Put64(rig.netGame, facts::NETGAME_SPECTATORCAM_CS_OFF, Addr(rig.csSpace.obj));
        Put64(rig.netGame, facts::NETGAME_CS_SPACE_OFF, 0);

        const det::Resolution r = rig.Resolve();

        CHECK(r.refusal == Refusal::LookupNull);
        CHECK(r.csFromLookup == 0);
        CHECK(r.spaceTable.verdict == tg::TableVerdict::Ready);
        CHECK_MESSAGE(g_lookupCalls == 1, "the space was searched more than once");
        CHECK(strcmp(replay::camera::RefusalName(r.refusal), "lookup-null") == 0);
        CHECK(strstr(r.why, "not registered in this level space") != nullptr);
    }

    SUBCASE("failing input: the constructed space is at +0x2B08 and +0x2B10 is null") {
        // The inverted arm. Before ARCH-12 this rig resolved; now the only space present is the one
        // the product must NOT use, so it refuses and never calls the engine.
        Put64(rig.netGame, facts::NETGAME_SPECTATORCAM_CS_OFF, 0);

        const det::Resolution r = rig.Resolve();

        CHECK(r.refusal == Refusal::NoLevelSpace);
        CHECK(r.levelSpace == 0);
        CHECK(r.csSpace == Addr(rig.csSpace.obj));
        CHECK_MESSAGE(g_lookupCalls == 0, "a space reached the engine routine with +0x2B10 null");
        CHECK(strcmp(replay::camera::RefusalName(r.refusal), "no-level-space") == 0);
        CHECK_MESSAGE(strstr(r.why, "no map loaded") != nullptr,
                      "L3/L4 must read as an absent question, not a negative answer");
    }

    SUBCASE("failing input: the constructed space is at +0x8, the slot that never was a space") {
        Put64(rig.netGame, facts::NETGAME_SPECTATORCAM_CS_OFF, 0);
        Put64(rig.netGame, facts::NETGAME_CS_SPACE_OFF, 0);
        Put64(rig.netGame, facts::IDENT_NETGAME_GAMESPACE_OFF, Addr(rig.levelSpace.obj));

        const det::Resolution r = rig.Resolve();

        CHECK(r.refusal == Refusal::NoLevelSpace);
        CHECK(g_lookupCalls == 0);
    }

    SUBCASE("the object at +0x8 is not a space: it does not survive the table check") {
        const tg::TableCheck asSpace = tg::CheckSpaceTable(
            rig.game.data(), cf::kSpaceCsTableOff, cf::kSpaceCsTableStride, false);
        CHECK(asSpace.verdict != tg::TableVerdict::Ready);
    }
}

// AUD-F3B F3B-2. Three answers, three values. The level-space slot is 11 KiB into the netGame --
// +0x8 is readable behind essentially any non-null pointer and +0x2B10 is not readable behind a
// stale, truncated or wrong-type one, which is exactly what a level transition hands us. Answering
// that with NoNetGame tells the operator to wait for a netGame that already exists.
//
// The narrow page is a real committed page with nothing committed after it, so the refusal comes
// from the memory manager rather than from a table verdict or from a null test.
static_assert(facts::NETGAME_SPECTATORCAM_CS_OFF + sizeof(uint64_t) > 0x1000u,
              "a one-page netGame must not be able to carry the level-space slot");

TEST_CASE("resolve: a netGame we could not read is not a netGame that does not exist") {
    g_lookupCalls = 0;
    g_lookupFirstSpace = nullptr;

    SUBCASE("could not ask: one committed page, and the level-space slot is past its end") {
        void* const narrow =
            ::VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        REQUIRE(narrow != nullptr);

        const det::Resolution r = det::ResolveSpectatorCs(narrow, &FakeLookup);

        CHECK(r.refusal == Refusal::NetGameUnreadable);
        CHECK_MESSAGE(r.refusal != Refusal::NoNetGame,
                      "could-not-ask was filed as never-was: the operator waits for state 9");
        CHECK(r.levelSpace == 0);
        CHECK(r.csSpace == 0);
        CHECK(r.spaceTable.verdict == tg::TableVerdict::NotChecked);
        CHECK_MESSAGE(g_lookupCalls == 0, "the engine routine ran off an unreadable netGame");
        CHECK(strcmp(replay::camera::RefusalName(r.refusal), "netgame-unreadable") == 0);
        CHECK(strcmp(replay::camera::RefusalName(r.refusal),
                     replay::camera::RefusalName(Refusal::NoNetGame)) != 0);

        ::VirtualFree(narrow, 0, MEM_RELEASE);
    }

    SUBCASE("never asked: a null netGame still answers NoNetGame, so the two stay separable") {
        const det::Resolution r = det::ResolveSpectatorCs(nullptr, &FakeLookup);
        CHECK(r.refusal == Refusal::NoNetGame);
        CHECK(r.levelSpace == 0);
        CHECK(g_lookupCalls == 0);
    }

    SUBCASE("a small non-null integer is a pointer we HELD and could not follow") {
        const det::Resolution r = det::ResolveSpectatorCs(reinterpret_cast<void*>(0x10ull),
                                                          &FakeLookup);
        CHECK(r.refusal == Refusal::NetGameUnreadable);
        CHECK(g_lookupCalls == 0);
    }

    SUBCASE("the width is the cause: a buffer that does cover +0x2B10 resolves on the same code") {
        Rig rig;
        const det::Resolution r = rig.Resolve();
        CHECK(r.refusal == Refusal::None);
        CHECK(g_lookupCalls == 2);
    }

    SUBCASE("could not read the space itself: non-null pointer, no object behind it") {
        Rig rig;
        Put64(rig.netGame, facts::NETGAME_SPECTATORCAM_CS_OFF, 0x10ull);

        const det::Resolution r = rig.Resolve();

        CHECK(r.refusal == Refusal::LevelSpaceUnreadable);
        CHECK_MESSAGE(r.refusal != Refusal::SpaceTableNotReady,
                      "could-not-reach-the-table was filed as the table answering no");
        CHECK(r.levelSpace == 0x10ull);
        CHECK(r.spaceTable.verdict == tg::TableVerdict::Unreadable);
        CHECK(g_lookupCalls == 0);
        CHECK(strcmp(replay::camera::RefusalName(r.refusal), "level-space-unreadable") == 0);
    }
}

// ARCH-12 R2: the two-space differential, which is what keeps R1 falsifiable. +0x2B10 cannot
// cross-check itself, so the control is the SAME symbol asked of the other space. Four-state, not
// two: a csSpace that is absent or not indexable is a control that could not be asked, and R4
// forbids reading either slot's readiness as a proxy for the other's.
TEST_CASE("resolve: the two-space differential, and the four states its arm can be in") {
    SUBCASE("failing input: the csSpace answers the spectator symbol too") {
        Rig rig;
        Space ambiguous{99, cf::kSpectatorSymbol};
        Put64(rig.netGame, facts::NETGAME_CS_SPACE_OFF, Addr(ambiguous.obj));

        const det::Resolution r = rig.Resolve();

        CHECK(r.refusal == Refusal::SpaceAmbiguous);
        CHECK(r.csFromLookup == Addr(rig.levelSpace.cs));
        CHECK(r.csFromCsSpace == Addr(ambiguous.cs));
        CHECK(r.csFromLookup != r.csFromCsSpace);
        CHECK(g_lookupCalls == 2);
        CHECK(strcmp(replay::camera::RefusalName(r.refusal), "space-ambiguous") == 0);
    }

    SUBCASE("failing input: one pointer in both slots") {
        Rig rig;
        Put64(rig.netGame, facts::NETGAME_CS_SPACE_OFF, Addr(rig.levelSpace.obj));

        const det::Resolution r = rig.Resolve();

        CHECK(r.refusal == Refusal::SpaceAliased);
        CHECK(r.levelSpace == r.csSpace);
        CHECK_MESSAGE(g_lookupCalls == 0,
                      "an aliased pair reached the engine routine before it was refused");
        CHECK(strcmp(replay::camera::RefusalName(r.refusal), "space-aliased") == 0);
    }

    SUBCASE("could not ask: no csSpace at all -- the L3/L4 order, and NOT a camera refusal") {
        Rig rig;
        Put64(rig.netGame, facts::NETGAME_CS_SPACE_OFF, 0);

        const det::Resolution r = rig.Resolve();

        CHECK_MESSAGE(r.refusal == Refusal::None,
                      "the csSpace's readiness gated the camera path -- ARCH-12 R4");
        CHECK(r.csSpace == 0);
        CHECK(r.csFromCsSpace == 0);
        CHECK(g_lookupCalls == 1);
        CHECK_MESSAGE(strstr(r.why, "could not be asked") != nullptr,
                      "a control that was never asked was recorded as one that passed");
        CHECK(strstr(r.why, "the differential passed") == nullptr);
    }

    SUBCASE("could not ask: a csSpace whose own table is not indexable, still not a refusal") {
        Rig rig;
        Put64(rig.csSpace.obj, cf::kSpaceCsTableOff + tg::kCountOff, 0x5AA5ull);

        const det::Resolution r = rig.Resolve();

        CHECK(r.refusal == Refusal::None);
        CHECK(r.csSpaceTable.verdict == tg::TableVerdict::NotReady);
        CHECK(r.csFromCsSpace == 0);
        CHECK_MESSAGE(g_lookupCalls == 1,
                      "the engine routine was called on a csSpace that is not indexable");
        CHECK(strstr(r.why, "could not be asked") != nullptr);
    }
}

// The level space's own table guard. Every arm here is on +0x2B10, because that is the pointer the
// engine routine is handed; the csSpace's table decides only whether the differential can be asked.
TEST_CASE("resolve: the level space's table is checked before the engine routine is called") {
    SUBCASE("failing input: the table is allocated but not constructed -- the AVFIX crash class") {
        Rig rig;
        // count > capacity: two fields the constructor writes together, now disagreeing. The engine
        // lookup would have read the count, believed it, and indexed the base unguarded.
        Put64(rig.levelSpace.obj, cf::kSpaceCsTableOff + tg::kCountOff, 0x5AA5ull);
        const det::Resolution r = rig.Resolve();
        CHECK(r.refusal == Refusal::SpaceTableNotReady);
        CHECK(r.spaceTable.verdict == tg::TableVerdict::NotReady);
        CHECK(r.spaceTable.why == tg::TableWhy::CountOverCapacity);
        CHECK_MESSAGE(g_lookupCalls == 0, "the space reached the engine routine anyway");
        CHECK(strstr(r.why, "CountOverCapacity") != nullptr);
    }

    SUBCASE("failing input: the table's base does not cover the elements it claims") {
        Rig rig;
        Put64(rig.levelSpace.obj, cf::kSpaceCsTableOff + tg::kBaseOff, 0x10ull);
        const det::Resolution r = rig.Resolve();
        CHECK(r.refusal == Refusal::SpaceTableNotReady);
        CHECK(r.spaceTable.verdict == tg::TableVerdict::NotReady);
        CHECK(r.spaceTable.why == tg::TableWhy::ArrayUnreadable);
        CHECK(g_lookupCalls == 0);
    }
}

// AUD-F3B F3B-1, the positive control -- kept, and moved onto the level space with the accessor.
// src/ui/csregister.cpp has carried the rule since it was written: never call the engine's lookup
// on a table with flags bit 0 set, because it sorts IN PLACE before searching and on a stale block
// that is a write into freed memory. The audit proved the gap by flipping one byte of this file's
// own fixture and watching every assertion stay green.
//
// W1-C's f5a7191 made the bit a VERDICT rather than a field each caller must remember to read: a
// table that would otherwise be Ready or Empty answers NotReady/SortPending while the bit is set.
// So this is a control on the shipping resolve with nothing added to it, and it is positive in both
// halves -- the engine routine is not called, AND the deferral names the reason.
TEST_CASE("resolve: an unsorted component-system table defers, and the engine is never called") {
    SUBCASE("failing input: bit 0 set on a level-space table that is Ready otherwise") {
        Rig rig;
        Put64(rig.levelSpace.obj, cf::kSpaceCsTableOff + tg::kFlagsOff, 1);

        const det::Resolution r = rig.Resolve();

        CHECK(r.refusal == Refusal::SpaceTableNotReady);
        CHECK(r.spaceTable.verdict == tg::TableVerdict::NotReady);
        CHECK(r.spaceTable.why == tg::TableWhy::SortPending);
        CHECK(strcmp(tg::WhyName(r.spaceTable.why), "SortPending") == 0);
        CHECK_MESSAGE(g_lookupCalls == 0,
                      "CS.LOOKUP_BY_SYMBOL would have sorted this table in place");
        CHECK(r.csFromLookup == 0);
        // The bit survives into the diagnosis. A deferral whose raw fields do not show the flag
        // cannot be told from any other NotReady by whoever reads the log.
        CHECK((r.spaceTable.raw.flags & 1u) != 0);
        CHECK(strstr(r.why, "SortPending") != nullptr);
    }

    SUBCASE("the bit is the cause: the same table with bit 0 clear resolves and calls the lookup") {
        Rig rig;

        const det::Resolution r = rig.Resolve();

        CHECK(r.refusal == Refusal::None);
        CHECK(r.spaceTable.verdict == tg::TableVerdict::Ready);
        CHECK(r.spaceTable.why == tg::TableWhy::Ready);
        CHECK(g_lookupCalls == 2);
    }

    SUBCASE("bit 0 and not merely non-zero flags: the other bits of the dword do not defer") {
        Rig rig;
        Put64(rig.levelSpace.obj, cf::kSpaceCsTableOff + tg::kFlagsOff, ~1ull);

        const det::Resolution r = rig.Resolve();

        CHECK(r.refusal == Refusal::None);
        CHECK(r.spaceTable.verdict == tg::TableVerdict::Ready);
        CHECK(r.csFromLookup == Addr(rig.levelSpace.cs));
        // kFlagsOff is a DWORD: the top half of the qword written here is not part of the field.
        // A guard reading eight bytes would carry 0xFFFFFFFFFFFFFFFE instead.
        CHECK(r.spaceTable.raw.flags == 0xFFFFFFFEull);
    }

    SUBCASE("the csSpace's own sort-pending bit defers the differential, not the bind") {
        Rig rig;
        Put64(rig.csSpace.obj, cf::kSpaceCsTableOff + tg::kFlagsOff, 1);

        const det::Resolution r = rig.Resolve();

        CHECK(r.refusal == Refusal::None);
        CHECK(r.csSpaceTable.why == tg::TableWhy::SortPending);
        CHECK_MESSAGE(g_lookupCalls == 1,
                      "the differential sorted the csSpace's table in place");
    }
}
