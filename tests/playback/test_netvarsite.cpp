// The netvar LOCATOR over the netGame's global netvar handle, driven over synthetic
// engine memory (fake_netvar_session.h).
//
// What this drives is the SHIPPED bind -- BindNetVarTableOn, the same function playback_module calls
// every tick -- and it asserts on the applier's own state afterwards, not on a copy of the walk. A
// test that re-implemented the hops would stay green with the bind call deleted.

#include "doctest.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fake_netvar_session.h"
#include "playback/core/apply.h"
#include "playback/core/netvarsite.h"

using namespace replay;
using namespace replay::playback;
using fake_netvar::FakeSession;
using fake_netvar::kCaptureLevelId;

namespace {

int FakeDecode(void*, const void*, uint64_t, void*) { return 0; }

struct Fixture {
    FakeSession sess;
    StateApplier applier;
    char line[768] = {};  // facts-ok: WORLD.REORIGIN_FLAG_BITS a local buffer size, not the engine offset

    Fixture() { ResetNetVarSiteForTest(); }
    ~Fixture() { ResetNetVarSiteForTest(); }

    NetVarSite Bind(uint64_t levelId = kCaptureLevelId) {
        return BindNetVarTableOn(applier, sess.NetGame(), levelId, &FakeDecode, line, sizeof line);
    }
};

}  // namespace

TEST_CASE("netvarsite: the global handle binds the component and hands it to the applier") {
    Fixture f;
    const NetVarSite s = f.Bind();

    REQUIRE(s.bind == NetVarSiteBind::Ok);
    CHECK(s.netVarCs == static_cast<void*>(f.sess.cs));
    CHECK(s.game == static_cast<void*>(f.sess.game));
    CHECK(s.space == static_cast<void*>(f.sess.levelSpace));
    CHECK(s.Slot() == FakeSession::kGlobalSlot);
    CHECK(s.records == FakeSession::kRecords);
    // (slot, symbol): the store is the record the HANDLE'S slot maps to, not record 0.
    CHECK(s.store == static_cast<void*>(f.sess.globalStore.header));

    REQUIRE(f.applier.NetVarTable() != nullptr);
    uint64_t owner = 0;
    REQUIRE(f.applier.NetVarTable()->OwnerId(owner));
    CHECK(owner == kCaptureLevelId);

    CHECK(strstr(f.line, "netvar table: bound at") != nullptr);
    CHECK(strstr(f.line, "netgame state 9") != nullptr);
    CHECK(strstr(f.line, "vars=3") != nullptr);
}

// The 2026-09-18 launch: state 9, the handle filled, and the CncaGame's topology counter long past
// the one-shot snapshot on the netGame. Nothing ever makes the pair equal again.
TEST_CASE("netvarsite: a generation that differs from its snapshot does not refuse the bind") {
    Fixture f;
    REQUIRE(fake_netvar::kCachedGeneration != fake_netvar::kLiveGeneration);

    const NetVarSite s = f.Bind();
    REQUIRE(s.bind == NetVarSiteBind::Ok);
    CHECK(f.applier.NetVarTable() != nullptr);
    CHECK(s.cachedGeneration == fake_netvar::kCachedGeneration);
    CHECK(s.liveGeneration == fake_netvar::kLiveGeneration);

    char expect[64];
    snprintf(expect, sizeof expect, "generation cached %llu live %llu",
             (unsigned long long)fake_netvar::kCachedGeneration,
             (unsigned long long)fake_netvar::kLiveGeneration);
    CHECK(strstr(f.line, expect) != nullptr);

    // A spawn between two ticks moves the counter. It is not a level re-entry.
    f.sess.SetLiveGeneration(fake_netvar::kLiveGeneration + 1);
    const NetVarSite next = LocateNetVarSite(f.sess.NetGame());
    REQUIRE(next.StoreOk());
    CHECK(next.liveGeneration == fake_netvar::kLiveGeneration + 1);
    CHECK(next.SameInstanceAs(s));
}

TEST_CASE("netvarsite: a component outside the netGame's current level space is refused") {
    Fixture f;
    f.sess.SetState(7);
    f.sess.SetComponentSpace(f.sess.nextLevelSpace);   // filled from a space the netGame is not in

    const NetVarSite s = f.Bind();
    REQUIRE(s.bind == NetVarSiteBind::SpaceMismatch);
    CHECK(s.netVarCs == nullptr);
    CHECK(f.applier.NetVarTable() == nullptr);
    CHECK(strstr(f.line, "not located (netgame state 7") != nullptr);
    CHECK(strstr(f.line, "current level space") != nullptr);

    f.sess.SetLevelSpace(nullptr);                     // no level loaded, and a component with no space
    f.sess.SetComponentSpace(nullptr);
    CHECK(f.Bind().bind == NetVarSiteBind::SpaceMismatch);
    CHECK(f.applier.NetVarTable() == nullptr);

    f.sess.SetState(9);
    f.sess.SetLevelSpace(f.sess.nextLevelSpace);
    f.sess.SetComponentSpace(f.sess.nextLevelSpace);
    CHECK(f.Bind().bind == NetVarSiteBind::Ok);
    CHECK(f.applier.NetVarTable() != nullptr);
}

TEST_CASE("netvarsite: a level re-entry stops validating, then binds as a different instance") {
    Fixture f;
    const NetVarSite first = f.Bind();
    REQUIRE(first.StoreOk());
    CHECK(first.SameInstanceAs(LocateNetVarSite(f.sess.NetGame())));

    // The netGame moves to the next level's space while the old component's handle is still filled.
    f.sess.SetLevelSpace(f.sess.nextLevelSpace);
    CHECK(LocateNetVarSite(f.sess.NetGame()).bind == NetVarSiteBind::SpaceMismatch);
    f.sess.ClearHandle();
    CHECK(LocateNetVarSite(f.sess.NetGame()).bind == NetVarSiteBind::NoGame);
    UnbindNetVarTable(f.applier);
    CHECK(f.applier.NetVarTable() == nullptr);

    f.sess.FillHandle(f.sess.nextLevelSpace);
    const NetVarSite second = f.Bind();
    REQUIRE(second.StoreOk());
    CHECK(second.space == static_cast<void*>(f.sess.nextLevelSpace));
    CHECK_FALSE(second.SameInstanceAs(first));
    CHECK(f.applier.NetVarTable() != nullptr);
}

TEST_CASE("netvarsite: no session, an empty handle and no component are three different answers") {
    Fixture f;
    NetVarSite s = BindNetVarTableOn(f.applier, nullptr, kCaptureLevelId, &FakeDecode, f.line,
                                     sizeof f.line);
    CHECK(s.bind == NetVarSiteBind::NoNetGame);
    CHECK(strstr(f.line, "not located") != nullptr);

    FakeSession::Put(f.sess.netGame, kNetGameNetVarGameOff, static_cast<void*>(nullptr));
    CHECK(f.Bind().bind == NetVarSiteBind::NoGame);

    FakeSession::Put(f.sess.netGame, kNetGameNetVarGameOff, static_cast<void*>(f.sess.game));
    FakeSession::Put(f.sess.netGame, kNetGameNetVarCsOff, static_cast<void*>(nullptr));
    CHECK(f.Bind().bind == NetVarSiteBind::NoNetVarCs);

    FakeSession::Put(f.sess.netGame, kNetGameNetVarCsOff, static_cast<void*>(f.sess.cs));
    f.sess.SetHandle(0xFFFFu | (5u << 16));   // the unresolved slot under a live-looking upper half  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    CHECK(f.Bind().bind == NetVarSiteBind::NoNetVarCs);
    CHECK(f.applier.NetVarTable() == nullptr);

    f.sess.SetHandle(FakeSession::kGlobalSlot | (5u << 16));
    CHECK(f.Bind().bind == NetVarSiteBind::Ok);
}

TEST_CASE("netvarsite: a component whose CR15NetGame slot is not ours is refused, not bound") {
    Fixture f;
    FakeSession::Put(f.sess.cs, facts::NETVAR_LEVEL_NETGAME_OFF, reinterpret_cast<void*>(0x1234ull));
    const NetVarSite s = f.Bind();

    REQUIRE(s.bind == NetVarSiteBind::NetGameMismatch);
    CHECK(s.netVarCs == nullptr);
    CHECK(f.applier.NetVarTable() == nullptr);
}

// The three launches this replaces: a live CR15NetVarCS carries a pointer at +0x130 and a set bool
// at +0x138, which a CTable-header reading of cs+0x100 takes for `count > capacity` / "unsorted".
// The fake plants both, so every Ok in this file is also this assertion; here it is by name.
TEST_CASE("netvarsite: the members past the pool are not a table header and refuse nothing") {
    Fixture f;
    uint64_t past = 0;
    memcpy(&past, f.sess.cs + 0x130, sizeof past);  // facts-ok: PHYS.CI_TARGET_TIME_OFF a test fixture value this case compares against, not an engine position
    REQUIRE(past > FakeSession::kCapacity);
    uint32_t flag = 0;
    memcpy(&flag, f.sess.cs + 0x138, sizeof flag);  // facts-ok: ENTITY.REMOTEPLAYER_CS_NCAPHYSICS_OFF a test fixture value this case compares against, not an engine position
    REQUIRE((flag & 1u) == 1u);

    const NetVarSite s = f.Bind();
    CHECK(s.RouteOk());
    CHECK(s.StoreOk());
    CHECK(s.records == FakeSession::kRecords);   // u16 [cs+0xFA], not the qword at +0x130
    CHECK(s.pool.capacity == FakeSession::kCapacity);
    CHECK(s.pool.index == FakeSession::kGlobalRecord);
}

TEST_CASE("netvarsite: the route holds without a readable store, and only the decoder is left unbound") {
    Fixture f;
    f.sess.SetStoreOf(FakeSession::kGlobalRecord, reinterpret_cast<void*>(0xDEAD0000DEAD0000ull));
    const NetVarSite s = f.Bind();

    CHECK(s.RouteOk());
    CHECK_FALSE(s.StoreOk());
    REQUIRE(s.bind == NetVarSiteBind::StoreUnreadable);
    CHECK(s.pool.step == NetVarPoolStep::StoreUnreadable);
    CHECK(s.store == nullptr);
    CHECK(f.applier.NetVarTable() == nullptr);
    CHECK(strstr(f.line, "generic store decoder not bound") != nullptr);
    CHECK(strstr(f.line, "the record's store pointer is null or unreadable") != nullptr);
    CHECK(strstr(f.line, "store=DEAD0000DEAD0000") != nullptr);
    CHECK(strstr(f.line, "HUD setter path does not need the store") != nullptr);

    f.sess.SetStoreOf(FakeSession::kGlobalRecord, nullptr);
    CHECK(f.Bind().bind == NetVarSiteBind::StoreUnreadable);

    // The record array itself gone: still a route, because the slot map is what names the record.
    FakeSession::Put(f.sess.cs, 0x100, reinterpret_cast<void*>(0xDEAD0000DEAD0000ull));
    const NetVarSite gone = f.Bind();
    CHECK(gone.RouteOk());
    CHECK(gone.pool.step == NetVarPoolStep::RecordUnreadable);
    CHECK(strstr(f.line, "the slot's 48-byte record is unreadable") != nullptr);
}

TEST_CASE("netvarsite: every pool refusal names its own step and prints the values it read") {
    struct Case {
        const char*    what;
        void         (*plant)(FakeSession&);
        NetVarSiteBind bind;
        NetVarPoolStep step;
        const char*    sentence;
        const char*    value;
    };
    const Case cases[] = {
        {"slot map says 0xFFFF",
         [](FakeSession& s) { s.slotMap[FakeSession::kGlobalSlot][0] = 0xFFFF; },  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
         NetVarSiteBind::NoRecord, NetVarPoolStep::SlotUnmapped,
         "the slot map holds 0xFFFF for the slot", "index=65535"},
        {"index at the record count",
         [](FakeSession& s) { s.slotMap[FakeSession::kGlobalSlot][0] = FakeSession::kRecords; },
         NetVarSiteBind::NoRecord, NetVarPoolStep::IndexOverCount,
         "the slot's record index is not below the record count", "count=3"},
        {"no record appended yet",
         [](FakeSession& s) { s.SetRecordCount(0); },
         NetVarSiteBind::NoRecord, NetVarPoolStep::IndexOverCount,
         "the slot's record index is not below the record count", "count=0"},
        {"slot past the slot map",
         [](FakeSession& s) { s.SetHandle(uint32_t{FakeSession::kCapacity} | (5u << 16)); },
         NetVarSiteBind::PoolUnreadable, NetVarPoolStep::SlotOverCapacity,
         "the slot is past the pool's capacity", "capacity=8"},
        {"slot map pointer unmapped",
         [](FakeSession& s) {
             FakeSession::Put(s.cs, 0xD0, reinterpret_cast<void*>(0xDEAD0000DEAD0000ull));
         },
         NetVarSiteBind::PoolUnreadable, NetVarPoolStep::SlotMapUnreadable,
         "the slot map entry is unreadable", "slotmap=DEAD0000DEAD0000"},
        {"slot recycled",
         [](FakeSession& s) { s.slotMap[FakeSession::kGlobalSlot][1] = 6; },
         NetVarSiteBind::GenerationMismatch, NetVarPoolStep::Walked,
         "the handle's slot was recycled", "generation=6"},
    };
    uint16_t seen[sizeof cases / sizeof cases[0]] = {};
    size_t n = 0;
    for (const Case& c : cases) {
        CAPTURE(c.what);
        Fixture f;
        c.plant(f.sess);
        const NetVarSite s = f.Bind();
        CHECK_FALSE(s.RouteOk());
        CHECK_FALSE(s.StoreOk());
        CHECK(s.bind == c.bind);
        CHECK(s.pool.step == c.step);
        CHECK(f.applier.NetVarTable() == nullptr);
        CHECK(strstr(f.line, c.sentence) != nullptr);
        CHECK(strstr(f.line, c.value) != nullptr);
        CHECK(strstr(f.line, "cannot be walked yet") == nullptr);
        CHECK(strstr(f.line, "needs this route too and is not writing") != nullptr);
        seen[n++] = s.RefusalKey();
    }
    // Once-per-distinct-reason logging compares these, so two reasons must never share one.
    CHECK(seen[0] != seen[1]);
    CHECK(seen[1] == seen[2]);
    CHECK(seen[3] != seen[4]);
    CHECK(seen[4] != seen[5]);
}

TEST_CASE("netvarsite: a capture with no recorded level id is located and left unbound") {
    Fixture f;
    const NetVarSite s = f.Bind(0);

    REQUIRE(s.bind == NetVarSiteBind::NoCaptureLevelId);
    CHECK(f.applier.NetVarTable() == nullptr);
    CHECK(strstr(f.line, "records no level id") != nullptr);
}
