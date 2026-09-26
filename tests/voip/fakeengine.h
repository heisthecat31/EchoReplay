#pragma once

// A CR15NetVoipReceiverCS and the CR15NetGame behind it, laid out at the offsets
// the W3-C fact drop decoded out of echovr.exe, plus a stand-in for VOIP.RECEIVE that records
// what it was handed.
//
// ★ WHY A SYNTHETIC ENGINE RATHER THAN A MOCK OF OUR OWN CODE. The thing that can be wrong here is
// the WALK -- which field holds the count, which map goes which way, which dword is the join key --
// and a mock built from the same beliefs as the code proves only that the beliefs are consistent.
// So this builds memory AT the offsets and the REAL walk runs over it. Every field a walk reads is
// here and nothing else is, so a walk that reads an offset this fixture does not populate reads a
// zero and refuses, which is the outcome it should have.

#include <string.h>

#include <vector>

#include "audio/voip/voip_facts.h"
#include "audio/voip/voipsite.h"
#include "generated/facts_generated.h"
#include "doctest.h"

namespace w3c {

struct FakeEngine {
    // ★ THE netGame MUST BE BIG ENOUGH TO HOLD THE FIELD THE RESOLVER READS. The game space is at
    // NETGAME.CS_SPACE_OFF (+0x2B08), so a rig smaller than that plus a pointer is a rig that never
    // reads the field and would keep passing after the offset changed under it (ARCH-10 S3.4).
    static constexpr size_t kNetGameSize = 0x2B10;
    static_assert(kNetGameSize >= facts::NETGAME_CS_SPACE_OFF + sizeof(void*),
                  "the netGame rig cannot hold NETGAME.CS_SPACE_OFF");

    static constexpr uint16_t kSlots = 16;
    static constexpr uint16_t kInstances = 4;
    static constexpr uint16_t kPoolSlots = 8;

    // ★ THE TABLE THE LOOKUP SEARCHES, on whatever object it is handed. A LITERAL on purpose
    // (ARCH-10 S3.3): a fixture that imports facts::CS_TABLE_OFF cannot fail when facts is wrong.
    static constexpr size_t kFixtureCsTableOff = 0x420u;  // facts-ok: CS.TABLE_OFF a fixture that imports the product's constant cannot fail when the product's constant is wrong -- ARCH-10 S3.3 keeps the test-side 0x420 literal
    // The one qword L5 judged at +0x420 of the object the plugin was passing -- a CR15Game render
    // budget number, not a table pointer, which is why the decoy below refuses rather than resolves.
    static constexpr uint64_t kRecordedBudgetWord = 0x400000002000ull;

    // ★ THE BLOCK HEADER THE TABLE GUARD JUDGES, written out in full rather than left as a bare
    // pointer, because the release path now REFUSES a table that is not Ready -- and a rig that
    // could never produce a Ready one would make that refusal unfalsifiable, which is the fixture
    // that cannot fail. Literals for the same reason kFixtureCsTableOff is one.
    static constexpr size_t   kFixtureHeaderBytes   = 0x40u;
    static constexpr uint32_t kFixtureBaseOff       = 0x00u;
    static constexpr uint32_t kFixtureBytesOff      = 0x08u;
    static constexpr uint32_t kFixtureAllocOff      = 0x10u;
    static constexpr uint32_t kFixtureGrowOff       = 0x20u;
    static constexpr uint32_t kFixtureCapacityOff   = 0x28u;
    static constexpr uint32_t kFixtureCountOff      = 0x30u;
    static constexpr uint32_t kFixtureFlagsOff      = 0x38u;
    static constexpr uint64_t kFixtureEntryStride   = 16u;
    static constexpr uint64_t kFixtureCapacity      = 2u;

    struct FakeCsTable {
        size_t count = 0;
        uint64_t symbols[2] = {0, 0};
        void* systems[2] = {nullptr, nullptr};
    };
    FakeCsTable table;

    std::vector<uint8_t> netGame = std::vector<uint8_t>(kNetGameSize, 0);
    std::vector<uint8_t> cs = std::vector<uint8_t>(0x400, 0);
    std::vector<uint8_t> ci =
        std::vector<uint8_t>(size_t(replay::audio::voip_facts::kCiStride) * kInstances, 0);
    std::vector<uint16_t> denseMap = std::vector<uint16_t>(kInstances, 0);
    std::vector<uint32_t> pool = std::vector<uint32_t>(kPoolSlots, 0);
    // Both objects are wide enough to CARRY a table pointer at CS.TABLE_OFF, because the lookup
    // below decides by what it reads out of them rather than by which pointer it recognises.
    std::vector<uint8_t> gameSpace = std::vector<uint8_t>(kFixtureCsTableOff + kFixtureHeaderBytes, 0);
    // The CR15Game the netGame points back to at +0x8. It is here so that resolving through +0x8
    // finds a plausible non-null object that is NOT the space: an accessor put back on +0x8 then
    // fails the way it failed in the game, rather than tripping over a null.
    std::vector<uint8_t> gameDecoy =
        std::vector<uint8_t>(kFixtureCsTableOff + kFixtureHeaderBytes, 0xCD);
    uint64_t netGameSub = 0x1234;
    // Somewhere for the header's allocator field to point. Never called: the guard only asks whether
    // an append would have an allocator to grow through, and a null one is its own refusal.
    uint64_t allocatorStub = 0;

    template <class T>
    void Poke(std::vector<uint8_t>& buf, uint32_t off, T v) {
        REQUIRE(off + sizeof(T) <= buf.size());
        memcpy(buf.data() + off, &v, sizeof v);
    }

    uint8_t* Ci(uint16_t dense) {
        return ci.data() + size_t(replay::audio::voip_facts::kCiStride) * dense;
    }

    uint32_t CiOff(uint16_t dense, uint32_t field) {
        return uint32_t(replay::audio::voip_facts::kCiStride) * dense + field;
    }

    // dense -> pool slot, the pool entry's {u16 dense, u16 generation}, and a CONNECTED instance.
    void Bind(uint16_t dense, uint16_t poolSlot, uint16_t generation, uint32_t userId) {
        namespace f = replay::audio::voip_facts;
        denseMap[dense] = poolSlot;
        pool[poolSlot] = uint32_t(dense) | (uint32_t(generation) << 16);
        Poke(ci, CiOff(dense, f::kCiUserIdOff), userId);
        Poke(ci, CiOff(dense, f::kCiDecoderStateOff), 1u);
        Poke(ci, CiOff(dense, f::kCiTalkerAOff), 0x11u);
        Poke(ci, CiOff(dense, f::kCiTalkerBOff), 0x22u);
    }

    void SetSlotGeneration(uint16_t slot, uint16_t generation) {
        namespace f = replay::audio::voip_facts;
        Poke(netGame, f::kGenTableOff + 4u * slot, generation);
        Poke(netGame, f::kSlotTableOff + 4u * slot, uint16_t(slot));
    }

    void Build(uint16_t liveInstances) {
        namespace f = replay::audio::voip_facts;
        Poke(cs, f::kCsCountOff, liveInstances);
        Poke(cs, f::kCsCiBaseOff, ci.data());
        Poke(cs, f::kCsPoolTableOff, reinterpret_cast<uint8_t*>(pool.data()));
        Poke(cs, f::kCsDenseMapOff, reinterpret_cast<uint8_t*>(denseMap.data()));
        Poke(cs, f::kCsNetGameOff, netGame.data());
        Poke(netGame, facts::NETGAME_CS_SPACE_OFF, gameSpace.data());
        Poke(netGame, facts::IDENT_NETGAME_GAMESPACE_OFF, gameDecoy.data());
        // The space carries the receiver in its table; the CR15Game carries the budget word L5 saw,
        // so the two are told apart by what is READ out of them and not by which one this rig is.
        table.count = 1;
        table.symbols[0] = f::kReceiverCsSymbol;
        table.systems[0] = cs.data();
        // The header's BASE is the qword at kFixtureCsTableOff, which is exactly what the lookup
        // seam reads for the table -- so filling in the rest of the header leaves every test that
        // pokes a table pointer here reading precisely what it read before.
        Poke(gameSpace, uint32_t(kFixtureCsTableOff + kFixtureBaseOff), &table);
        Poke(gameSpace, uint32_t(kFixtureCsTableOff + kFixtureBytesOff),
             uint64_t(kFixtureCapacity * kFixtureEntryStride));
        Poke(gameSpace, uint32_t(kFixtureCsTableOff + kFixtureAllocOff), &allocatorStub);
        Poke(gameSpace, uint32_t(kFixtureCsTableOff + kFixtureGrowOff), uint64_t(1));
        Poke(gameSpace, uint32_t(kFixtureCsTableOff + kFixtureCapacityOff), kFixtureCapacity);
        Poke(gameSpace, uint32_t(kFixtureCsTableOff + kFixtureCountOff), uint64_t(1));
        Poke(gameSpace, uint32_t(kFixtureCsTableOff + kFixtureFlagsOff), uint32_t(0));
        Poke(gameDecoy, uint32_t(kFixtureCsTableOff), kRecordedBudgetWord);
        Poke(netGame, f::kNetGameSubOff, &netGameSub);
        // Every slot starts as the engine's own "nobody", so a test that wants an entrant has to
        // say so and a test that forgets gets a refusal rather than slot zero.
        for (uint16_t s = 0; s < kSlots; ++s) {
            Poke(netGame, f::kGenTableOff + 4u * s, uint16_t(0xFFFF));
        }
    }

    // flags bit 0: the engine's lookup would SORT this block in place before searching it, which is
    // an engine write on the calling thread -- so tableguard answers NotReady/SortPending and a
    // caller about to write through what the lookup returns must not enter it.
    void SetTableSortPending(bool on) {
        Poke(gameSpace, uint32_t(kFixtureCsTableOff + kFixtureFlagsOff), uint32_t(on ? 1u : 0u));
    }

    replay::audio::voip::VoipSite Site() {
        return replay::audio::voip::VoipSite{cs.data(), netGame.data()};
    }

    // Makes ResolveVoipSite's one engine call land on this engine. Everything else in that
    // function -- the gamespace read, the null checks, the four field reads -- still runs.
    void InstallLookup();
};

// The stand-in for CncaGameSpace::FindComponentSystem. It searches the table carried by WHATEVER
// object it is handed, at CS.TABLE_OFF, and never compares the pointer against the rig's own space:
// an accessor aimed at the CR15Game then fails for the engine's own reason -- that object's +0x420
// is not a table -- instead of because the fixture recognised a pointer and decided the verdict
// itself. A test that asks for a symbol the table does not carry still gets NoVoipCs.
void* __fastcall FakeComponentLookup(void* gameSpace, uint64_t symbol);
void ClearLookup();

// ★ HOW MANY TIMES THE ENGINE WAS ENTERED, counted at the seam rather than by the code under test.
// The resolve throttle's only honest oracle: a player that re-resolves per frame and one that
// caches are indistinguishable in every frame counter, and differ here by a factor of a thousand.
// Incremented on ENTRY, so a lookup that returns null still counts -- refusals are the expensive
// case, since the engine sorts its table before it can answer at all.
extern int g_lookupCalls;

// Records exactly what VOIP.RECEIVE was handed, so an injection arm can assert the ARGUMENTS
// rather than assert that a bool came back true.
struct RecordedCall {
    int calls = 0;
    void* receiver = nullptr;
    uint32_t handle = 0;
    uint8_t header[16] = {};
    std::vector<uint8_t> payload;
    uint32_t length = 0;
};

extern RecordedCall g_call;

void FakeReceiveVoip(void* receiver, uint32_t handle, const void* header, const void* payload,
                     uint32_t length);

}  // namespace w3c
