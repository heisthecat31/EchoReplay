// ARCH-23 W2-P1 -- the provisioner, driven offline against a fake IIdentity and a fake netgame.
//
// The subject is the SHIPPED provisioner (src/engine/pawn_provision.cpp is linked in, not a copy of
// its reasoning).  What the fake stands in for is the engine's admission insert, and the fake's own
// postcondition is the one that matters: a fake that reports an admission WITHOUT adding an entrant
// is today's AddReplayUser defect, and the L3 #11 case below has to reject it.

#include <doctest.h>

#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <chrono>
#include <string>

#include "bootstrap/log.h"

#include "engine/identity_access.h"
#include "capture/roster.h"
#include "engine/pawn_provision.h"
#include "playback/core/recorded_loadout.h"
#include "generated/facts_generated.h"

using namespace replay;

namespace {

// The session's CR15NetGame, as far as this test is concerned: a state word at NETGAME.STATE_OFF.
uint8_t gNetGame[128];   // NETGAME.LOBBY_OFF is 0x40, so 64 bytes no longer covers the fields read
bool    gNetGamePresent = true;

void* FakeNetGame() { return gNetGamePresent ? gNetGame : nullptr; }

// NETGAME.STATE_OFF is 0 and the registry withholds the row (a zero offset has no encoded
// displacement to anchor), so the state word is the first dword of the netgame, as
// cmp dword ptr [rdi], 9 at 0x14015269A reads it.
void SetState(uint32_t v) { memcpy(gNetGame, &v, sizeof v); }

// A lobby that starts empty and gains entrants only when somebody admits one.  `dressed` is the
// five-predicate answer: an entrant with an unusable profile is PRESENT but does not resolve, which
// is exactly the state L3 #11 found at slot 0.
class FakeIdentity final : public IIdentity {
public:
    struct Entrant {
        bool     present = false;
        bool     dressed = false;   // ResolveReplayUser answers true only for this
        UserIndex index{kSlotUnmapped};
        JoinDesc desc{};
    };

    bool admitReportsSuccessWithoutAdmitting = false;   // the anti-AddReplayUser fake
    uint16_t nextIndex = 3;
    uint32_t inserts = 0;

    // What Arm() picks up when the env carries no surface of its own -- the shipped env is built
    // with an identity and nothing else.
    const PawnProvision::ActorHops* hopSurface = nullptr;
    const PawnProvision::ActorHops* ActorHops() const override { return hopSurface; }

    Entrant slots[facts::ENTITY_USER_SLOT_TABLE_MAX];

    bool AddReplayUser(UserSlot slot, UserIndex& out) override {
        if (slot.v >= facts::ENTITY_USER_SLOT_TABLE_MAX) return false;
        const Entrant& e = slots[slot.v];
        if (!e.present || !e.dressed) return false;
        out = e.index;
        return true;
    }

    // A blocker PAST the session gate -- L3 #15's `entity surface not bound to a game space` --
    // which is the only kind the retry cadence can be measured against: SessionNotInMatch refuses
    // before the cadence is consulted, on one guarded read, and always will.
    PawnProvision::Refusal refuseProvisionWith = PawnProvision::Refusal::None;

    bool ProvisionReplayUser(UserSlot slot, const JoinDesc& desc, UserIndex& out) override {
        ++inserts;
        if (refuseProvisionWith != PawnProvision::Refusal::None) {
            if (engineCompletesAfterInserts != 0 && inserts >= engineCompletesAfterInserts &&
                slot.v < facts::ENTITY_USER_SLOT_TABLE_MAX) {
                Entrant& late = slots[slot.v];
                late.present = true;
                late.dressed = engineCompletesDressed;
                late.index = UserIndex(nextIndex++);
                late.desc = desc;
            }
            why_ = refuseProvisionWith;
            return false;
        }
        if (slot.v >= facts::ENTITY_USER_SLOT_TABLE_MAX) {
            why_ = PawnProvision::Refusal::SlotOutOfRange;
            return false;
        }
        out = UserIndex(nextIndex++);
        if (admitReportsSuccessWithoutAdmitting) {
            why_ = PawnProvision::Refusal::ProfileStillEmpty;   // what the postcondition will find
            return true;
        }
        Entrant& e = slots[slot.v];
        e.present = true;
        e.dressed = true;          // the insert's GenerateBotProfile is what makes it resolvable
        e.index = out;
        e.desc = desc;
        why_ = PawnProvision::Refusal::None;
        return true;
    }

    // The L3 #22 shape.  The reply route parks the request (nothing is published from this phase,
    // so ReplyWrongPhase) and the ENGINE completes the entrant out of band on a later tick -- the
    // run watched it print `Adding remote user to slot 1` / `Player Actor Allocating` while this
    // call had already returned false.  Nothing this fake RETURNS ever says the entrant landed: it
    // appears only in the record, which is exactly where the provisioner has to look.  0 == never.
    uint32_t engineCompletesAfterInserts = 0;

    // WHAT THE ENGINE LEAVES BEHIND WHEN IT COMPLETES ONE.  L3 #24 read the engine's own slot-1
    // dump: `Actor 0x... (disabled)  (pooled)`, `Loadouts : 0`.  That entrant IS in the table --
    // IndexOf answers for it -- and it is NOT dressed, so the five-predicate ResolveReplayUser
    // says no.  A fixture that plants a DRESSED entrant cannot tell those two surfaces apart, and
    // a re-read keyed on the wrong one passes it.  Default true so every case written before this
    // one reads exactly as it did.
    bool engineCompletesDressed = true;

    PawnProvision::Refusal LastProvisionRefusal() const override { return why_; }

    bool SetProfileJson(UserSlot, const Blob&) override { return false; }
    bool SetTeam(UserIndex, uint8_t, uint8_t) override { return false; }
    bool ApplyLoadout(UserIndex, const Symbol64[20], uint16_t, uint16_t) override { return false; }
    bool OverrideAlignment(UserIndex, uint16_t) override { return false; }
    bool ReadAppliedItems(UserIndex, Symbol64[20]) const override { return false; }
    // The engine names a user's slot by the ENTRANT's own record -- every site that asks reads
    // `user[idx].word & IDENT.USER_SLOT_MASK` -- so the fake answers the same way, by scanning the
    // entrants rather than by consulting the per-slot table below.
    // The live shape L3 #13 met, which no other knob here can produce: the side table answers a
    // legal index and the entrant array does not cover it, so the record that would name a slot
    // back is not there. SlotOfIndex bounds on ENTRANT_COUNT_OFF and returns false; the surface
    // has COULD-NOT-SAY, not "held".
    bool entrantRecordsUnreadable = false;

    UserSlot SlotOf(UserIndex index) const override {
        if (entrantRecordsUnreadable) return UserSlot(kSlotUnmapped);
        for (uint16_t i = 0; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i)
            if (slots[i].present && slots[i].index.v == index.v) return UserSlot(i);
        return UserSlot(kSlotUnmapped);
    }

    // The live shape L3 #12 met: the admission path never writes the per-slot side table IndexOf
    // reads, so an untouched entry answers index 0 -- a legal index -- for every slot.
    bool sideTableZeroFilled = false;

    // The live shape L3 #17 recorded (`1=..ffff/0001:unreadable`): the side table answers the
    // slot's own number as a legal index for a slot NO entrant occupies, so the index resolves
    // to no entrant record at all.  This is the empty offline lobby, and nothing else here
    // produces it -- sideTableZeroFilled answers index 0 for every slot, which the viewer's own
    // entrant then names back.
    bool sideTableNamesTheSlot = false;

    // The engine's own slot table, which is what OCCUPANCY means.  An entrant is in it whether or
    // not it is dressed -- the viewer's undressed user is exactly that case -- so this answers on
    // `present` and never on `dressed`.
    UserIndex IndexOf(UserSlot slot) const override {
        if (slot.v >= facts::ENTITY_USER_SLOT_TABLE_MAX) return UserIndex(kSlotUnmapped);
        if (sideTableZeroFilled) return UserIndex(0);
        const Entrant& e = slots[slot.v];
        if (e.present) return e.index;
        if (sideTableNamesTheSlot) return UserIndex(slot.v);
        return UserIndex(kSlotUnmapped);
    }

    // Plant an entrant this session did not create.  present AND an index together: the engine's
    // table never holds one without the other, and a fixture that set only `present` would leave
    // IndexOf answering "empty" and disarm the map's occupancy test without failing anything.
    void Place(uint16_t slot, uint16_t index, bool dressed) {
        Entrant& e = slots[slot];
        e.present = true;
        e.dressed = dressed;
        e.index = UserIndex(index);
    }

private:
    PawnProvision::Refusal why_ = PawnProvision::Refusal::NotAttempted;
};

PlayerRecord OnePlayer(uint64_t id, uint16_t team) {
    PlayerRecord r{};
    r.xplatformId = id;
    r.teamId = uint8_t(team);
    r.teamId16 = team;
    r.extended = 1;
    return r;
}

// The two answers the entrant table's span proof can give.  Neither is wired by Armed(): the
// unwired default is what every other case exercises, and it must read an absent row as FREE.
bool EntrantTableNotProved() { return false; }
bool EntrantTableIsProved()  { return true; }

ProvisionEnv Armed(FakeIdentity& id, uint32_t perFrame = 2) {
    ProvisionEnv env;
    env.identity = &id;
    env.netGame = &FakeNetGame;
    env.perFrameMax = perFrame;
    return env;
}

// The lobby chain, laid out the way the engine lays it out: the CNSLobby carries the connection
// table as a SUBOBJECT at LOBBY.OBJ_OFF, and that subobject holds the array pointer and the count.
// Nothing here is a shortcut around an offset -- every displacement below is a registry row, so a
// consumer that skips one reads a different address and this fixture says so.
alignas(16) uint8_t gLobby[0x100];
alignas(16) uint8_t gConn[facts::ENTITY_USER_SLOT_TABLE_MAX * facts::LOBBY_CONN_STRIDE];

// `held` is the peer id planted in slot 0's record; every other slot carries the join sentinel.
void SetUpLobby(uint64_t held) {
    memset(gLobby, 0, sizeof gLobby);
    for (uint16_t i = 0; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i) {
        const uint64_t peer = i == 0 ? held : facts::LOBBY_JOIN_FREE_SENTINEL;
        memcpy(gConn + i * facts::LOBBY_CONN_STRIDE, &peer, sizeof peer);
    }
    uint8_t* table = gLobby + facts::LOBBY_OBJ_OFF;
    void*    conn  = gConn;
    uint64_t count = facts::ENTITY_USER_SLOT_TABLE_MAX;
    memcpy(table + facts::LOBBY_CONN_ARRAY_OFF, &conn, sizeof conn);
    memcpy(table + facts::LOBBY_CONN_COUNT_OFF, &count, sizeof count);

    void* lobby = gLobby;
    memcpy(gNetGame + facts::NETGAME_LOBBY_OFF, &lobby, sizeof lobby);
}

void Reset() {
    memset(gNetGame, 0, sizeof gNetGame);
    gNetGamePresent = true;
    SetState(facts::NETGAME_STATE_INMATCH);
}

}  // namespace

TEST_CASE("join_desc_is_authored_from_the_record_and_not_from_the_session") {
    const JoinDesc player = JoinDescFromRecord(OnePlayer(0x1122334455667788ull, 1));
    // The id is the one field the descriptor does NOT carry across (DIAG-ENTITY-COLLISION
    // 4): the recorded id is the eviction key, so the admission is authored with ours.
    CHECK(player.xplatformId != 0x1122334455667788ull);
    CHECK(player.team == 1);
    CHECK(player.role == kJoinRolePlayer);
    CHECK(player.useSpawnPoint);

    // E-I6: spectators and moderators are entrants with no pawn, and the insert's own spectator
    // test is `cmp word ptr [rsi+0Ah], 1`.
    CHECK(JoinDescFromRecord(OnePlayer(1, kTeamSpectator)).role == kJoinRoleSpectator);
    CHECK(JoinDescFromRecord(OnePlayer(1, kTeamModerator)).role == kJoinRoleSpectator);

    // A capture that recorded no id is authored with a synthetic one like every other
    // entrant.  It must still never be 0 (a legal id) or -1 (the engine's "supply one",
    // and the viewer's own id64 on an -offline host).
    const uint64_t none = JoinDescFromRecord(OnePlayer(0, 0)).xplatformId;
    CHECK(none != ~uint64_t(0));
    CHECK(none != 0);
}

TEST_CASE("join_rec_lands_every_field_at_the_offset_the_engine_reads") {
    JoinDesc d = JoinDescFromRecord(OnePlayer(0xAABBCCDDull, 3));
    uint8_t rec[kJoinRecBytes];
    REQUIRE(WriteJoinRec(d, rec));

    uint64_t id = 0;
    // Poison, so a field the writer never touched cannot read as a correct value.
    uint16_t team = kSlotUnmapped, role = kSlotUnmapped;
    memcpy(&id, rec, sizeof id);
    memcpy(&team, rec + facts::IDENT_ADMIT_JOINREC_TEAM_OFF, sizeof team);
    memcpy(&role, rec + facts::IDENT_ADMIT_JOINREC_ROLE_OFF, sizeof role);
    // The field lands at +0x00: the value compared is the one the DESCRIPTOR carries, which is
    // ours and not the recording's (DIAG-ENTITY-COLLISION 4).
    CHECK(id == d.xplatformId);
    CHECK(id != 0xAABBCCDDull);
    CHECK(team == 3);
    CHECK(role == kJoinRolePlayer);

    // The sentinel selects the level spawn point.  All three compared floats have to carry it: the
    // engine ANDs the three comparisons, so two out of three is a real transform at the origin.
    uint32_t bits[3] = {0, 0, 0};
    memcpy(bits, rec + facts::IDENT_ADMIT_JOINREC_XFORM_OFF + 0x10, sizeof bits);
    CHECK(bits[0] == facts::IDENT_ADMIT_XFORM_SENTINEL);
    CHECK(bits[1] == facts::IDENT_ADMIT_XFORM_SENTINEL);
    CHECK(bits[2] == facts::IDENT_ADMIT_XFORM_SENTINEL);

    // An authored transform is written verbatim, and nothing past the record's own bytes is touched.
    d.useSpawnPoint = false;
    for (int i = 0; i < 8; ++i) d.xform[i] = float(i + 1);
    REQUIRE(WriteJoinRec(d, rec));
    float xf[8];
    memcpy(xf, rec + facts::IDENT_ADMIT_JOINREC_XFORM_OFF, sizeof xf);
    for (int i = 0; i < 8; ++i) CHECK(xf[i] == float(i + 1));
    CHECK(facts::IDENT_ADMIT_JOINREC_PROFILE_OFF < kJoinRecBytes);
}

TEST_CASE("pawn_provision_maps_the_recorded_slot_off_the_viewers_own") {
    // The L3 #11 shape: one recorded player at capture slot 0, and a session whose slot 0 already
    // holds the VIEWER -- present, EMPTY profile, no pawn.  Undressed is not unoccupied, so the
    // recorded slot is mapped somewhere else rather than written through.
    Reset();
    FakeIdentity id;
    const uint16_t viewerIndex = 1;
    id.Place(0, viewerIndex, /*dressed=*/false);
    // The control: without it the table would read empty at slot 0 and every assertion below would
    // pass just as well against a provisioner that had no map at all.
    REQUIRE(id.IndexOf(UserSlot(0)).v != kSlotUnmapped);

    PawnProvisioner p;
    p.Arm(Armed(id));
    const JoinDesc desc = JoinDescFromRecord(OnePlayer(0x99ull, 1));
    REQUIRE(p.Request(UserSlot(0), desc));
    CHECK(p.Pump() == 1);

    const uint16_t expected = 1;                // slot 0 is held, so the first free slot is 1
    CHECK(p.MappedSlot(UserSlot(0)).v == expected);
    CHECK(p.MappedSlot(UserSlot(0)).v != UserSlot(0).v);
    CHECK(p.Provisioned(UserSlot(0)));

    // The viewer's own entrant is untouched: same index, still undressed, nothing written over it.
    CHECK(id.slots[0].index.v == viewerIndex);
    CHECK_FALSE(id.slots[0].dressed);
    CHECK(id.slots[expected].dressed);          // and the pawn landed on the MAPPED slot
    CHECK(id.inserts == 1);

    const PawnLedger led = p.Ledger();
    CHECK(led.provisioned == 1);
    CHECK(led.adopted == 0);                    // INV-PP1: structurally zero
    CHECK(led.deferred == 0);
    CHECK(led.failed == 0);
    CHECK(led.mapped == 1);
    CHECK(led.map[0].slot_recorded == UserSlot(0).v);
    CHECK(led.map[0].slot_resolved == expected);
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::None);

    // Idempotent per slot per load: the second call answers true, does not re-admit, and does not
    // re-map -- a map that moved between calls could not be read against a roster.
    CHECK(p.Provision(UserSlot(0), desc));
    CHECK(id.inserts == 1);
    CHECK(p.Ledger().provisioned == 1);
    CHECK(p.Ledger().mapped == 1);
    CHECK(p.MappedSlot(UserSlot(0)).v == expected);
}

TEST_CASE("the_slot_map_asks_the_entrant_and_not_the_side_table") {
    // L3 #12's live table, modelled: the viewer holds slot 0 as user index 0, and the per-slot side
    // table IndexOf reads has never been written, so it answers index 0 for ALL SIXTEEN slots.  A
    // map keyed on IndexOf alone reads that as "every slot is held" and refuses -- which is the
    // refusal the run printed.  The engine's own predicate asks the entrant that index names, and
    // only slot 0 agrees that it holds one.
    Reset();
    FakeIdentity id;
    id.Place(0, /*index=*/0, /*dressed=*/false);
    id.sideTableZeroFilled = true;
    // The controls.  Without the first, the fixture is the well-formed table the other cases use
    // and this case cannot fail; without the second, "free" would be the side table agreeing.
    REQUIRE(id.IndexOf(UserSlot(7)).v != kSlotUnmapped);
    REQUIRE(id.SlotOf(id.IndexOf(UserSlot(7))).v == 0);

    PawnProvisioner p;
    p.Arm(Armed(id));
    const JoinDesc desc = JoinDescFromRecord(OnePlayer(0x99ull, 1));
    REQUIRE(p.Request(UserSlot(0), desc));
    CHECK(p.Pump() == 1);

    CHECK(p.MappedSlot(UserSlot(0)).v == 1);   // slot 0 is genuinely held; 1 is the first free one
    CHECK(p.Provisioned(UserSlot(0)));
    CHECK(id.slots[1].dressed);                // the pawn landed on the mapped slot
    CHECK(id.slots[0].index.v == 0);           // and the viewer's own entrant is untouched
    CHECK(id.slots[0].dressed == false);

    const PawnLedger led = p.Ledger();
    CHECK(led.provisioned == 1);
    CHECK(led.mapped == 1);
    CHECK(led.map[0].slot_recorded == 0);
    CHECK(led.map[0].slot_resolved == 1);
}

TEST_CASE("the_slot_map_hands_out_free_slots_and_never_a_held_one") {
    // Two recorded players; a session holding the viewer at slot 0 and somebody else's live player
    // at slot 2.  Each recorded slot gets a DISTINCT free slot and neither held slot is handed out
    // -- a map that answered the recorded number would drive the viewer's own pawn.
    Reset();
    FakeIdentity id;
    id.Place(0, 1, /*dressed=*/false);   // the viewer: present, empty profile
    id.Place(2, 4, /*dressed=*/true);    // somebody else's live player
    REQUIRE(id.IndexOf(UserSlot(0)).v != kSlotUnmapped);
    REQUIRE(id.IndexOf(UserSlot(2)).v != kSlotUnmapped);

    ProvisionEnv env = Armed(id);
    env.perFrameMax = facts::ENTITY_USER_SLOT_TABLE_MAX;
    PawnProvisioner p;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), JoinDescFromRecord(OnePlayer(0x11ull, 1))));
    REQUIRE(p.Request(UserSlot(2), JoinDescFromRecord(OnePlayer(0x22ull, 1))));
    CHECK(p.Pump() == 2);

    const uint16_t a = p.MappedSlot(UserSlot(0)).v;
    const uint16_t b = p.MappedSlot(UserSlot(2)).v;
    CHECK(a != b);
    CHECK(a != UserSlot(0).v);
    CHECK(b != UserSlot(0).v);
    CHECK(a != UserSlot(2).v);
    CHECK(b != UserSlot(2).v);
    CHECK(a == 1);                       // first free ascending
    CHECK(b == 3);                       // first free ascending that `a` has not claimed
    CHECK(p.Ledger().mapped == 2);

    // An unrequested slot carries no mapping, and out of range is absence rather than slot 0.
    CHECK(p.MappedSlot(UserSlot(5)).v == kSlotUnmapped);
    CHECK(p.MappedSlot(UserSlot(facts::ENTITY_USER_SLOT_TABLE_MAX)).v == kSlotUnmapped);
}

TEST_CASE("provision_rejects_an_insert_that_reports_success_without_admitting") {
    // M1's shape from the other side: the postcondition is asserted through the same five
    // predicates the dresser will use, so an admission that leaves the profile empty is caught and
    // NAMED rather than counted as a pawn.
    Reset();
    FakeIdentity id;
    id.admitReportsSuccessWithoutAdmitting = true;

    PawnProvisioner p;
    p.Arm(Armed(id));
    CHECK_FALSE(p.Provision(UserSlot(0), JoinDescFromRecord(OnePlayer(1, 1))));
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::ProfileStillEmpty);
    CHECK(p.Ledger().provisioned == 0);
    CHECK(p.Ledger().failed == 1);
    CHECK(p.Ledger().refusals[size_t(PawnProvision::Refusal::ProfileStillEmpty)] == 1);
}

TEST_CASE("provision_defers_before_state_nine") {
    Reset();
    SetState(facts::NETGAME_STATE_INMATCH - 1);
    FakeIdentity id;
    PawnProvisioner p;
    p.Arm(Armed(id));
    const JoinDesc desc = JoinDescFromRecord(OnePlayer(7, 1));

    CHECK_FALSE(p.Provision(UserSlot(0), desc));
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::SessionNotInMatch);
    CHECK(p.Ledger().deferred == 1);
    CHECK(p.Ledger().failed == 0);      // a deferral is not a failure
    CHECK(id.inserts == 0);             // and nothing was called at the wrong time

    // The next attempt after the edge succeeds: the latch is driven by session state, not by
    // whether it has been asked before.
    SetState(facts::NETGAME_STATE_INMATCH);
    CHECK(p.Provision(UserSlot(0), desc));
    CHECK(p.Ledger().provisioned == 1);
    CHECK(p.Ledger().deferred == 1);

    // No netgame at all is a different word from "not in match".
    gNetGamePresent = false;
    CHECK_FALSE(p.Provision(UserSlot(1), desc));
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::NoNetGame);
}

TEST_CASE("an absent entrant row is a FREE slot, not an unreadable one") {
    // L3 #17's own map line, reproduced: `0=..fffc/0000:held, 1..15=..ffff/000N:unreadable` on an
    // EMPTY offline lobby.  The side table answered a legal index for all sixteen slots and only
    // the viewer's index named a slot back; the other fifteen rows were simply ABSENT.  Scoring
    // those absences `unreadable' meant no slot could EVER be proven free, and `provisioned 1'
    // (RUN-16) became `provisioned 0'.  This case is that recorded input, and it must map.
    Reset();
    SetUpLobby(facts::LOBBY_JOIN_FREE_SENTINEL);
    FakeIdentity id;
    id.Place(0, /*index=*/0, /*dressed=*/false);   // the viewer, and its record names slot 0 back
    id.sideTableNamesTheSlot = true;
    // The controls, without which this passes for the wrong reason: slot 1's side table entry is
    // a LEGAL index (not the empty-table shortcut), no entrant carries it, and the viewer's row
    // is genuinely present -- so the only difference between slot 0 and slot 1 is occupancy.
    REQUIRE(id.IndexOf(UserSlot(1)).v != kSlotUnmapped);
    REQUIRE(id.SlotOf(id.IndexOf(UserSlot(1))).v == kSlotUnmapped);
    REQUIRE(id.SlotOf(id.IndexOf(UserSlot(0))).v == 0);

    PawnProvisioner p;
    p.Arm(Armed(id));
    CHECK(p.Provision(UserSlot(0), JoinDescFromRecord(OnePlayer(5, 1))));
    CHECK(p.MappedSlot(UserSlot(0)).v == 1);      // the first FREE slot that is not the viewer's
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::None);
    CHECK(p.Ledger().provisioned == 1);
    CHECK(p.Ledger().adopted == 0);
    CHECK(p.Ledger().mapped == 1);
    CHECK(p.Ledger().refusals[size_t(PawnProvision::Refusal::SlotOccupancyUnreadable)] == 0);
    CHECK(id.inserts == 1);
    CHECK_FALSE(id.slots[0].dressed);             // the viewer's own pawn was not written over
    CHECK(id.slots[1].dressed);                   // and the recording landed on the mapped slot
}

TEST_CASE("only a table whose own memory could not be proved is UNREADABLE") {
    // The other half of the split.  SlotOf answers kSlotUnmapped for `no entrant carries this
    // index' AND for `the table could not be read', so the discriminator cannot be the row: it is
    // the span proof on the table's memory, which is the surface's to report and not the row's.
    Reset();
    SetUpLobby(facts::LOBBY_JOIN_FREE_SENTINEL);
    FakeIdentity id;
    id.Place(0, /*index=*/0, /*dressed=*/false);
    id.sideTableNamesTheSlot = true;
    id.entrantRecordsUnreadable = true;   // the page faulted: NO row answers, the viewer's included
    REQUIRE(id.IndexOf(UserSlot(0)).v != kSlotUnmapped);
    REQUIRE(id.SlotOf(id.IndexOf(UserSlot(0))).v == kSlotUnmapped);

    ProvisionEnv env = Armed(id);
    env.entrantTableProved = &EntrantTableNotProved;
    PawnProvisioner p;
    p.Arm(env);
    CHECK_FALSE(p.Provision(UserSlot(0), JoinDescFromRecord(OnePlayer(5, 1))));
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::SlotOccupancyUnreadable);
    CHECK(p.Ledger().refusals[size_t(PawnProvision::Refusal::SlotOccupancyUnreadable)] == 1);
    CHECK(p.Ledger().refusals[size_t(PawnProvision::Refusal::SlotAlreadyOccupied)] == 0);
    CHECK(p.Ledger().provisioned == 0);
    CHECK(p.Ledger().adopted == 0);     // INV-PP1 holds through the new refusal too
    CHECK(p.Ledger().mapped == 0);
    CHECK(id.inserts == 0);             // and nothing was written over on the way to saying so

    // The same fixture with the SAME absent rows and a hook that PROVES the table maps instead of
    // refusing, so it is the fault and not the absence that refused above.
    Reset();
    SetUpLobby(facts::LOBBY_JOIN_FREE_SENTINEL);
    FakeIdentity ok;
    ok.Place(0, /*index=*/0, /*dressed=*/false);
    ok.sideTableNamesTheSlot = true;
    ProvisionEnv proved = Armed(ok);
    proved.entrantTableProved = &EntrantTableIsProved;
    PawnProvisioner r;
    r.Arm(proved);
    CHECK(r.Provision(UserSlot(0), JoinDescFromRecord(OnePlayer(5, 1))));
    CHECK(r.MappedSlot(UserSlot(0)).v == 1);
    CHECK(r.Ledger().refusals[size_t(PawnProvision::Refusal::SlotOccupancyUnreadable)] == 0);

    // The discriminator kept from L3 #13: a table that really IS full still refuses by the OTHER
    // name, so the two causes do not collapse back into one word.
    Reset();
    FakeIdentity full;
    for (uint16_t i = 0; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i)
        full.Place(i, uint16_t(i + 1), /*dressed=*/true);
    PawnProvisioner q;
    q.Arm(Armed(full));
    CHECK_FALSE(q.Provision(UserSlot(0), JoinDescFromRecord(OnePlayer(5, 1))));
    CHECK(q.LastProvisionRefusal() == PawnProvision::Refusal::SlotAlreadyOccupied);
    CHECK(q.Ledger().refusals[size_t(PawnProvision::Refusal::SlotOccupancyUnreadable)] == 0);
}

TEST_CASE("provision_refuses_when_the_table_has_nowhere_to_map_into") {
    // (A) refused executably.  A live entrant this session did not create is never adopted and
    // never written over: with every slot held there is nowhere to map to, and the attempt is
    // refused BY NAME rather than landing on somebody's own pawn.
    Reset();
    FakeIdentity id;
    for (uint16_t i = 0; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i)
        id.Place(i, uint16_t(i + 1), /*dressed=*/true);

    PawnProvisioner p;
    p.Arm(Armed(id));
    CHECK_FALSE(p.Provision(UserSlot(0), JoinDescFromRecord(OnePlayer(5, 1))));
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::SlotAlreadyOccupied);
    CHECK(p.Ledger().adopted == 0);
    CHECK(p.Ledger().provisioned == 0);
    CHECK(p.Ledger().mapped == 0);     // nothing was claimed, so nothing is published
    CHECK(id.inserts == 0);            // the engine was not asked to overwrite it either
    CHECK_FALSE(p.Provisioned(UserSlot(0)));
    CHECK(p.MappedSlot(UserSlot(0)).v == kSlotUnmapped);

    // Free exactly one slot and the same request maps into it -- so the refusal above was the
    // table being full, and not the provisioner refusing everything put to it.
    const uint16_t freed = 7;
    id.slots[freed] = FakeIdentity::Entrant{};
    REQUIRE(id.IndexOf(UserSlot(freed)).v == kSlotUnmapped);
    CHECK(p.Provision(UserSlot(0), JoinDescFromRecord(OnePlayer(5, 1))));
    CHECK(p.MappedSlot(UserSlot(0)).v == freed);
}

TEST_CASE("provision_is_sliced_so_a_full_roster_cannot_spike_one_tick") {
    // S7.3: L3 #11's tail rung already fired at N=1, so twelve inserts in one frame is a governor
    // trip waiting to happen.  The slice is per PUMP, and what is not admitted stays queued.
    Reset();
    FakeIdentity id;
    PawnProvisioner p;
    p.Arm(Armed(id, 2));
    for (uint16_t i = 0; i < 5; ++i)
        REQUIRE(p.Request(UserSlot(i), JoinDescFromRecord(OnePlayer(100 + i, 1))));
    CHECK(p.Ledger().requested == 5);

    CHECK(p.Pump() == 2);
    CHECK(id.inserts == 2);
    CHECK(p.Ledger().refusals[size_t(PawnProvision::Refusal::BudgetExceeded)] == 3);
    CHECK(p.Pump() == 2);
    CHECK(p.Pump() == 1);
    CHECK(p.Pump() == 0);
    CHECK(p.Ledger().provisioned == 5);
    CHECK(id.inserts == 5);
    CHECK(p.Ledger().failed == 0);      // a budget deferral never counts as a failure

    for (uint16_t i = 0; i < 5; ++i) {
        CHECK(p.Provisioned(UserSlot(i)));
        // No crossover between queued slots: each carries the id ITS OWN record authors, and
        // the authored id is a function of the record, so the check is still exact.
        CHECK(id.slots[i].desc.xplatformId ==
              JoinDescFromRecord(OnePlayer(100 + i, 1)).xplatformId);
        CHECK(id.slots[i].desc.xplatformId != 100 + i);
    }
}

TEST_CASE("provision_refuses_by_name_before_it_is_armed_or_out_of_range") {
    Reset();
    PawnProvisioner p;
    CHECK_FALSE(p.Armed());
    CHECK_FALSE(p.Provision(UserSlot(0), JoinDesc{}));
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::NotAttempted);

    FakeIdentity id;
    p.Arm(Armed(id));
    const UserSlot past = UserSlot(uint16_t(facts::ENTITY_USER_SLOT_TABLE_MAX));
    CHECK_FALSE(p.Provision(past, JoinDesc{}));
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::SlotOutOfRange);
    CHECK(p.SlotRefusal(past) == PawnProvision::Refusal::SlotOutOfRange);
}

TEST_CASE("every_refusal_in_the_vocabulary_has_its_own_sentence") {
    // A vocabulary whose words share a sentence is a two-state gate with extra enum values.
    for (size_t i = 0; i < PawnProvision::kRefusalCount; ++i) {
        const char* a = PawnProvision::RefusalText(PawnProvision::Refusal(i));
        REQUIRE(a != nullptr);
        CHECK(strcmp(a, "unknown") != 0);
        for (size_t j = i + 1; j < PawnProvision::kRefusalCount; ++j)
            CHECK(strcmp(a, PawnProvision::RefusalText(PawnProvision::Refusal(j))) != 0);
    }
    for (size_t i = 0; i < PawnResolve::kRefusalCount; ++i) {
        const char* a = PawnResolve::RefusalText(PawnResolve::Refusal(i));
        REQUIRE(a != nullptr);
        CHECK(strcmp(a, "unknown") != 0);
        for (size_t j = i + 1; j < PawnResolve::kRefusalCount; ++j)
            CHECK(strcmp(a, PawnResolve::RefusalText(PawnResolve::Refusal(j))) != 0);
    }
}

// L3 #15 F3: the log capped the line and ate the tail of the sentence -- exactly the clause that
// said the refusal was OUR postcondition and not an occupancy claim. The name goes FIRST, so a cap
// can only cost prose. Every value, both vocabularies, and the name is checked as a PREFIX rather
// than as "contains", because a name buried mid-sentence is the thing a cap takes.
TEST_CASE("every refusal line leads with its enum name and fits inside the log's own cap") {
    // The dresser's widest prefix before `reason=`, spelled as the format it stands for: a text
    // that fits kLogLineMax on its own but not behind this prefix is still a truncated line.
    constexpr size_t kDresserPrefix =
        sizeof("player 'xxxxxxxxxxxxxxxx' slot 15 NOT dressed: stage=entrant reason=") - 1;

    for (size_t i = 0; i < PawnProvision::kRefusalCount; ++i) {
        const PawnProvision::Refusal r = PawnProvision::Refusal(i);
        const char* name = PawnProvision::RefusalName(r);
        const char* text = PawnProvision::RefusalText(r);
        REQUIRE(name != nullptr);
        REQUIRE(text != nullptr);
        CHECK(strcmp(name, "Unknown") != 0);
        const size_t n = strlen(name);
        CHECK(strncmp(text, name, n) == 0);
        REQUIRE(strlen(text) > n + 1);
        CHECK(text[n] == ':');
        CHECK(text[n + 1] == ' ');
        CHECK(kDresserPrefix + strlen(text) < kLogLineMax);
        for (size_t j = i + 1; j < PawnProvision::kRefusalCount; ++j)
            CHECK(strcmp(name, PawnProvision::RefusalName(PawnProvision::Refusal(j))) != 0);
    }
    for (size_t i = 0; i < PawnResolve::kRefusalCount; ++i) {
        const PawnResolve::Refusal r = PawnResolve::Refusal(i);
        const char* name = PawnResolve::RefusalName(r);
        const char* text = PawnResolve::RefusalText(r);
        REQUIRE(name != nullptr);
        REQUIRE(text != nullptr);
        CHECK(strcmp(name, "Unknown") != 0);
        const size_t n = strlen(name);
        CHECK(strncmp(text, name, n) == 0);
        CHECK(kDresserPrefix + strlen(text) < kLogLineMax);
        for (size_t j = i + 1; j < PawnResolve::kRefusalCount; ++j)
            CHECK(strcmp(name, PawnResolve::RefusalName(PawnResolve::Refusal(j))) != 0);
    }

    // The names are the enum's, not paraphrases of it: the two the L3 #15 packet added are spelled
    // here so a rename that leaves the table behind cannot pass.
    CHECK(strcmp(PawnProvision::RefusalName(PawnProvision::Refusal::AdmitDidNotCreateUser),
                 "AdmitDidNotCreateUser") == 0);
    CHECK(strcmp(PawnProvision::RefusalName(PawnProvision::Refusal::AdmitDidNotCreatePlayer),
                 "AdmitDidNotCreatePlayer") == 0);
}

// L3 #15 F1. The dress pass asks for the same admission on every tick while it is blocked, and
// each ask walks the lobby chain per candidate slot and calls the engine's insert: the run measured
// ~1 ms a tick and its own capture died of the cost. The bound here is DERIVED -- one attempt for
// the blocker's first reading plus one per cadence period -- so a per-tick retry cannot satisfy it
// by being a little cheaper.
TEST_CASE("a blocked provisioner retries on a cadence, never on every tick") {
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    FakeIdentity id;
    // Blocked PAST the session gate, which is L3 #15's own second blocker: the state word says
    // INMATCH and the admission still refuses. SessionNotInMatch would refuse before the cadence
    // is ever consulted, on one guarded read, so it cannot measure this.
    id.refuseProvisionWith = PawnProvision::Refusal::NoGameSpace;

    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), JoinDesc{}));

    constexpr uint32_t kTicks = 10000;
    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kTicks; ++i) {
        UserSlot  slot(kSlotUnmapped);
        UserIndex index(kSlotUnmapped);
        p.Admit(UserSlot(0), JoinDesc{}, slot, index);   // the dress pass, every tick
        p.Pump();                                        // and the frame's pump behind it
    }
    const long long us = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - t0).count();

    const uint32_t bound = 1 + kTicks / PawnProvisioner::kRetryTicks;
    REQUIRE(PawnProvisioner::kRetryTicks >= 2);
    CHECK(p.Pumps() == kTicks);
    CHECK(p.Attempts() <= bound);
    CHECK(p.Attempts() >= 2);              // bounded, and still retrying
    CHECK(id.inserts == p.Attempts());     // one engine call per attempt -- and none per tick
    CHECK(id.inserts <= bound);
    CHECK(us <= 30ll * kTicks);            // 30 us/tick amortised
    CHECK(p.LastProvisionRefusal() != PawnProvision::Refusal::None);
}

// The cadence must not slow a pass that is making progress: an entrant landing moves every other
// slot's occupancy answer, so the next tick is due immediately and a full roster still fills at
// perFrameMax a tick rather than at perFrameMax a cadence period.
TEST_CASE("a landing entrant reopens the window, so a full roster is not spread over the cadence") {
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    FakeIdentity id;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    p.Arm(env);
    for (uint16_t i = 0; i < 6; ++i) REQUIRE(p.Request(UserSlot(i), JoinDesc{}));

    uint32_t done = 0, pumps = 0;
    while (done < 6 && pumps < PawnProvisioner::kRetryTicks) {
        done += p.Pump();
        ++pumps;
    }
    CHECK(done == 6);
    CHECK(pumps == 3);   // 6 players at perFrameMax 2, one pump each -- not one cadence each
}

TEST_CASE("the_shipped_identity_refuses_admit_not_promoted_rather_than_calling") {
    // ARCH-23 S3.4.  The product's own IIdentity, not a fake: with no session it says NoNetGame,
    // and the default any un-updated implementation inherits is AdmitNotPromoted -- never `true`.
    struct Bare final : IIdentity {
        bool AddReplayUser(UserSlot, UserIndex&) override { return false; }
        bool SetProfileJson(UserSlot, const Blob&) override { return false; }
        bool SetTeam(UserIndex, uint8_t, uint8_t) override { return false; }
        bool ApplyLoadout(UserIndex, const Symbol64[20], uint16_t, uint16_t) override { return false; }
        bool OverrideAlignment(UserIndex, uint16_t) override { return false; }
        bool ReadAppliedItems(UserIndex, Symbol64[20]) const override { return false; }
        UserSlot SlotOf(UserIndex) const override { return UserSlot(kSlotUnmapped); }
        UserIndex IndexOf(UserSlot) const override { return UserIndex(kSlotUnmapped); }
    } bare;
    UserIndex out(0);
    CHECK_FALSE(bare.ProvisionReplayUser(UserSlot(0), JoinDesc{}, out));
    CHECK(bare.LastProvisionRefusal() == PawnProvision::Refusal::AdmitNotPromoted);

    Reset();
    FakeIdentity id;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id);
    env.identity = &bare;
    p.Arm(env);
    CHECK_FALSE(p.Provision(UserSlot(0), JoinDesc{}));
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::AdmitNotPromoted);
    CHECK(p.Ledger().refusals[size_t(PawnProvision::Refusal::AdmitNotPromoted)] == 1);
}

// ===========================================================================================
// THE POST-PROVISION CENSUS (ARCH-23 S9's W2-P0 "after" pass).
//
// The instrument is what settles FACT-PP1, so the thing that must not be true of it is that it
// prints an arm it did not measure.  Every case below plants a surface and asserts the census
// names THAT surface: a census hard-wired to one arm fails all four.

#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "bootstrap/services.h"

namespace {

struct CaptureLog : ILog {
    std::vector<std::string> lines;
    void Write(LogLevel, const char* module, const char* fmt, ...) override {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        lines.push_back(std::string("[") + module + "] " + buf);
    }
    bool Enabled(LogLevel) const override { return true; }
    void DumpLedger() override {}

    size_t Count(const char* needle) const {
        size_t n = 0;
        for (const std::string& l : lines)
            if (l.find(needle) != std::string::npos) ++n;
        return n;
    }
};

// A slot table with both surfaces plantable independently, which is the whole point: FACT-PP1 is
// the question of whether one entrant appears on one surface or on both.
struct CensusEntity : IEntity {
    enum { kN = facts::ENTITY_USER_SLOT_TABLE_MAX };

    bool     available = true;
    // ModelVisible publishes ONE bool for all of its gates, so a fake that always answers cannot
    // exercise the census arm that has to name the refusal.
    bool     modelResolves = true;
    bool     model0Resolves = true;
    uint16_t remoteCi[kN];
    uint16_t physOfCi[kN];
    bool     bodyReadable[kN];
    bool     localHere[kN];
    uint16_t localEcho[kN];

    CensusEntity() {
        for (int i = 0; i < kN; ++i) {
            remoteCi[i] = kSlotUnmapped;
            physOfCi[i] = kSlotUnmapped;
            bodyReadable[i] = false;
            localHere[i] = false;
            localEcho[i] = kSlotUnmapped;
        }
    }

    void PlantRemote(uint16_t slot, uint16_t ci, uint16_t phys, bool driveable) {
        remoteCi[slot] = ci;
        physOfCi[ci] = phys;
        if (phys < kN) bodyReadable[phys] = driveable;
    }
    void PlantLocal(uint16_t slot, uint16_t echo, bool driveable) {
        localHere[slot] = true;
        localEcho[slot] = echo;
        if (echo < kN) bodyReadable[echo] = driveable;
    }

    size_t PhysCount()  const override { return kN; }
    size_t PlayerCount() const override { return kN; }
    size_t LocalPlayerCount() const override { return kN; }

    bool ReadBody(PhysHandle h, BodyState& s) const override {
        s = BodyState();
        return h.v < kN && bodyReadable[h.v];
    }
    PlayerHandle PlayerOf(UserIndex u) const override {
        return PlayerHandle(u.v < kN ? remoteCi[u.v] : kSlotUnmapped);
    }
    PhysHandle PhysOf(PlayerHandle p) const override {
        return PhysHandle(p.v < kN ? physOfCi[p.v] : kSlotUnmapped);
    }
    uint32_t ReadLocalPlayerDetailed(UserIndex u, PlayerState& s,
                                     LocalPlayerReading& r) const override {
        s = PlayerState();
        r = LocalPlayerReading();
        if (u.v >= kN || !localHere[u.v]) {
            r.refusal = LocalRefusal::SlotNotInPool;
            return 0;
        }
        r.refusal = LocalRefusal::NotAsked;
        r.echoComponent = localEcho[u.v] != kSlotUnmapped;
        r.echoHandle = localEcho[u.v];
        return 1u;
    }
    bool ModelVisible(UserIndex u, bool& visible) const override {
        visible = false;
        // User index 0 is the LOCAL viewer and answers on its own switch: the census asks
        // it as the control for `resolved=no`, and a fake that answered one bool for both
        // could never show the control disagreeing with the entrant.
        if (!(u.v == 0 ? model0Resolves : modelResolves)) return false;
        visible = u.v < kN && (localHere[u.v] || remoteCi[u.v] != kSlotUnmapped);
        return true;
    }

    bool        Available()  const override { return available; }
    const char* Diagnostic() const override { return available ? "fake" : "no entity service"; }

    // Nothing below is read by the census; they exist because IEntity requires them.
    bool  IsLocallyControlled(PhysHandle) const override { return false; }
    void  SetSyntheticOwner(PhysHandle) override {}
    void  ClearSyntheticOwner(PhysHandle) override {}
    void  PushHistory(PhysHandle, const BodyState&, uint64_t) override {}
    void  TrimHistory(PhysHandle, uint64_t, uint64_t) override {}
    float GetTimeOffset(PhysHandle) const override { return 0.0f; }
    void  SetTimeOffset(PhysHandle, float) override {}
    bool  ReadPlayer(PlayerHandle, PlayerState& s) const override { s = PlayerState(); return false; }
    void  PushPlayerHistory(PlayerHandle, const PlayerState&, uint64_t) override {}
    void  SetLodThrottle(bool) override {}
};

// One mapped pair, built by hand so the census cases do not depend on the provisioner reaching a
// particular state -- the two are proved separately and joined by the epoch case below.
PawnLedger MappedLedger(uint16_t recorded, uint16_t resolved) {
    PawnLedger l{};
    l.requested = 1;
    l.provisioned = 1;
    l.mapped = 1;
    l.map[0].slot_recorded = recorded;
    l.map[0].slot_resolved = resolved;
    return l;
}

}  // namespace

TEST_CASE("census names the REMOTE surface when only the remote pool holds the pawn") {
    CensusEntity e;
    e.PlantRemote(/*slot*/ 1, /*ci*/ 3, /*phys*/ 4, /*driveable*/ true);
    CaptureLog log;

    CHECK(CensusProvisionedPawns(MappedLedger(0, 1), e, int32_t(facts::NETGAME_STATE_INMATCH),
                                 log) == 1);
    CHECK(log.Count("census slot=1 (recorded 0) class=Remote") == 1);
    CHECK(log.Count("remote: ci=present phys=present driveable=yes") == 1);
    CHECK(log.Count("local: ci=ABSENT") == 1);
    CHECK(log.Count("remote_ci=3 remote_phys=4") == 1);
    CHECK(log.Count("inmatch=yes") == 1);
    CHECK(log.Count("stateRaw=9") == 1);
    CHECK(log.Count("in BOTH=0") == 1);
    // Remote-only is not evidence for either arm of FACT-PP1: the arm is about an entrant the
    // local surface holds. MIXED is the honest word when the two counts disagree.
    CHECK(log.Count("FACT-PP1 -> ARM (1)") == 0);
    CHECK(log.Count("FACT-PP1 -> ARM (2)/(3)") == 0);
    CHECK(log.Count("FACT-PP1 -> MIXED") == 1);
}

TEST_CASE("census reports FACT-PP1 arm (2)/(3) when the provisioned pawn has no remote CI") {
    CensusEntity e;
    e.PlantLocal(/*slot*/ 2, /*echo*/ 7, /*driveable*/ true);
    CaptureLog log;

    CHECK(CensusProvisionedPawns(MappedLedger(0, 2), e, int32_t(facts::NETGAME_STATE_INMATCH),
                                 log) == 1);
    CHECK(log.Count("census slot=2 (recorded 0) class=Local") == 1);
    CHECK(log.Count("local: ci=present echo=present driveable=yes") == 1);
    CHECK(log.Count("FACT-PP1 -> ARM (2)/(3)") == 1);
    CHECK(log.Count("FACT-PP1 -> ARM (1)") == 0);
}

TEST_CASE("census reports FACT-PP1 arm (1) when the pawn is on BOTH surfaces") {
    CensusEntity e;
    e.PlantRemote(3, 5, 6, true);
    e.PlantLocal(3, 8, false);
    CaptureLog log;

    CHECK(CensusProvisionedPawns(MappedLedger(1, 3), e, int32_t(facts::NETGAME_STATE_INMATCH),
                                 log) == 1);
    CHECK(log.Count("FACT-PP1 -> ARM (1)") == 1);
    CHECK(log.Count("in BOTH=1") == 1);
    // The echo handle is planted but its body does not read: driveable is a READ, not a presence.
    CHECK(log.Count("local: ci=present echo=present driveable=no") == 1);
}

TEST_CASE("census refuses by name when the entity service is unavailable") {
    CensusEntity e;
    e.available = false;
    e.PlantLocal(1, 7, true);
    CaptureLog log;

    CHECK(CensusProvisionedPawns(MappedLedger(0, 1), e, int32_t(facts::NETGAME_STATE_INMATCH),
                                 log) == 0);
    CHECK(log.Count("REFUSED NoEntityService") == 1);
    CHECK(log.Count("census slot=") == 0);
    // A refusal must not publish a verdict: an unread pool is not evidence for any arm.
    CHECK(log.Count("FACT-PP1 ->") == 0);
    CHECK(log.Count("FACT-PP1 UNDETERMINED") == 0);
    CHECK(log.Count("answers NOTHING about FACT-PP1") == 1);
}

TEST_CASE("census says UNDETERMINED rather than an arm when neither surface answers") {
    CensusEntity e;
    CaptureLog log;

    CHECK(CensusProvisionedPawns(MappedLedger(0, 1), e, kCensusStateUnknown, log) == 1);
    CHECK(log.Count("class=Unknown") == 1);
    CHECK(log.Count("FACT-PP1 UNDETERMINED") == 1);
    CHECK(log.Count("FACT-PP1 -> ") == 0);
    // No state word offered: say so rather than print a zero that reads like a state.
    CHECK(log.Count("state=UNKNOWN") == 1);
    CHECK(log.Count("stateRaw=") == 0);
}

TEST_CASE("Pump censuses ONCE per provisioning epoch, and only once a pawn exists") {
    Reset();
    FakeIdentity id;
    CensusEntity e;
    CaptureLog log;
    PawnProvisioner p;

    ProvisionEnv env = Armed(id, 1);
    env.entity = &e;
    env.log = &log;
    p.Arm(env);

    // Nothing requested: Pump provisions nothing, so there is nothing to census.
    CHECK(p.Pump() == 0);
    CHECK(log.Count("census: provisioned") == 0);

    CHECK(p.Request(UserSlot(0), JoinDesc{}));
    CHECK(p.Request(UserSlot(1), JoinDesc{}));
    CHECK(p.Pump() == 1);
    CHECK(log.Count("census: provisioned") == 1);
    CHECK(p.Pump() == 1);
    CHECK(log.Count("census: provisioned") == 1);   // second pass, same epoch, no second census

    // A new Arm is a new epoch.
    p.Arm(env);
    CHECK(p.Request(UserSlot(0), JoinDesc{}));
    CHECK(p.Pump() == 1);
    CHECK(log.Count("census: provisioned") == 2);
}

TEST_CASE("an unwired census is skipped, not faked") {
    Reset();
    FakeIdentity id;
    CaptureLog log;
    PawnProvisioner p;

    ProvisionEnv env = Armed(id, 1);
    env.log = &log;                 // entity missing: the census cannot ask, so it must not print
    p.Arm(env);
    CHECK(p.Request(UserSlot(0), JoinDesc{}));
    CHECK(p.Pump() == 1);
    // The CENSUS is the subject: the window line is the pump reporting the table it just
    // walked (L3 #25 F1) and prints on any pass that has work, so this asserts the census is
    // absent rather than that nothing was logged at all.
    CHECK(log.Count("census") == 0);
}

TEST_CASE("the entrant table decides a local entrant's slot; conn[] is a diagnostic") {
    // ENG-W2-P1-7 could not build this: LOBBY.OBJ_OFF -- the CNSLobby subobject step every LOBBY.*
    // row named in prose and none of them filed -- had no registry row, so the connection array was
    // unreachable and the provisioner had only the per-slot side table to infer from.  With the row
    // filed the engine's own predicate is readable, and it is the one sub_1406051C0 uses to pick a
    // slot: conn[slot]'s first qword == LOBBY.JOIN_FREE_SENTINEL means nobody holds it.
    const uint64_t kPeer = 0x00025F1A7C3D1A2Bull;

    SUBCASE("a one-player offline lobby: every conn record free, and slot 0 still held") {
        // The engine's table for an offline/practice lobby with one local player.  Nothing arrived
        // through SNSLobbyAddEntrantRequestv4, so EVERY conn record carries the free sentinel --
        // the viewer's included -- while the entrant table names the viewer at slot 0.  A gate on
        // conn[] maps the recording onto the viewer's own pawn; the entrant table maps it to 1.
        Reset();
        SetUpLobby(facts::LOBBY_JOIN_FREE_SENTINEL);
        FakeIdentity id;
        id.Place(0, /*index=*/3, /*dressed=*/false);

        // Both controls, because either one alone lets the case pass for the wrong reason: the
        // conn record really does read FREE at slot 0, and the entrant table really does name it.
        PawnProvision::LobbyRecord rec{};
        REQUIRE(PawnProvision::ReadLobbySlot(gNetGame, 0, rec));
        REQUIRE(rec.state == PawnProvision::SlotOccupancy::Free);
        REQUIRE(id.IndexOf(UserSlot(0)).v == 3);
        REQUIRE(id.SlotOf(UserIndex(3)).v == 0);

        PawnProvisioner p;
        ProvisionEnv env = Armed(id, 1);
        p.Arm(env);
        CHECK(p.Request(UserSlot(0), JoinDesc{}));
        CHECK(p.Pump() == 1);
        CHECK(p.MappedSlot(UserSlot(0)).v == 1);
        CHECK(p.MappedSlot(UserSlot(0)).v != 0);   // never the viewer's own
        CHECK(id.slots[0].dressed == false);
    }

    SUBCASE("a conn record the net layer calls held does not hold a slot against a local entrant") {
        // The mirror: conn[] says slot 1 is taken by a peer, and no entrant record names it.  The
        // peer is the net layer's business; this insert never writes that array.
        Reset();
        SetUpLobby(kPeer);
        memcpy(gConn + 1 * facts::LOBBY_CONN_STRIDE, &kPeer, sizeof kPeer);
        FakeIdentity id;
        id.Place(0, /*index=*/3, /*dressed=*/false);

        PawnProvision::LobbyRecord rec{};
        REQUIRE(PawnProvision::ReadLobbySlot(gNetGame, 1, rec));
        REQUIRE(rec.state == PawnProvision::SlotOccupancy::Held);

        PawnProvisioner p;
        ProvisionEnv env = Armed(id, 1);
        p.Arm(env);
        CHECK(p.Request(UserSlot(0), JoinDesc{}));
        CHECK(p.Pump() == 1);
        CHECK(p.MappedSlot(UserSlot(0)).v == 1);
    }

    SUBCASE("every entrant slot held is a full lobby, refused by name and printed with peers") {
        Reset();
        SetUpLobby(kPeer);
        for (uint16_t i = 1; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i)
            memcpy(gConn + i * facts::LOBBY_CONN_STRIDE, &kPeer, sizeof kPeer);

        FakeIdentity id;
        // The engine's table is what fills the lobby; conn[] being full too is what puts a peer id
        // in the diagnostic, and the assertions below prove it is redacted there.
        for (uint16_t i = 0; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i)
            id.Place(i, /*index=*/uint16_t(i), /*dressed=*/false);
        CaptureLog   log;
        PawnProvisioner p;
        ProvisionEnv env = Armed(id, 1);
        env.log = &log;
        p.Arm(env);

        CHECK(p.Request(UserSlot(0), JoinDesc{}));
        CHECK(p.Pump() == 0);
        CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::SlotAlreadyOccupied);
        // The line still says which surface answered -- a ..xxxx peer is the lobby's own record,
        // an xxxx/xxxx pair is the side table -- but a peer id is an identifier, and an identifier
        // never lands in a log in full: only its low 16 bits do, beside the state word.
        CHECK(log.Count(",1=..1a2b/0001:held,") == 1);
        CHECK(log.Count(",15=..1a2b/000f:held") == 1);
        CHECK(log.Count("00025f1a7c3d1a2b") == 0);
        CHECK(log.Count("5f1a7c3d") == 0);

        // Independent of the format above: no sixteen-hex-digit run survives in the line at all.
        const std::string* refusal = nullptr;
        for (const std::string& l : log.lines)
            if (l.find("not mapped") != std::string::npos) refusal = &l;
        REQUIRE(refusal != nullptr);
        size_t wide = 0, run = 0;
        for (size_t i = 0; i <= refusal->size(); ++i) {
            const char c = i < refusal->size() ? (*refusal)[i] : ' ';
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
                ++run;
                continue;
            }
            if (run >= 16) ++wide;
            run = 0;
        }
        CHECK(wide == 0);
    }

    SUBCASE("an unreachable lobby is not an obstacle: the entrant table was always the rule") {
        Reset();   // netgame + LOBBY_OFF is zero: no chain, and nothing about it was load-bearing
        FakeIdentity id;
        PawnProvisioner p;
        ProvisionEnv env = Armed(id, 1);
        p.Arm(env);

        CHECK(p.Request(UserSlot(0), JoinDesc{}));
        CHECK(p.Pump() == 1);
        CHECK(p.MappedSlot(UserSlot(0)).v == 0);
    }
}

namespace {

// The engine's insert, as far as this test is concerned -- and it is the LOCAL/bot insert, which
// is what sub_140152210 is: it opens by logging "[NETGAME] Adding bot user to slot %llu, idx %hu"
// and never touches the lobby's connection array.  So `claimsConn` is FALSE by default and a
// successful admission still has to be a provision; what it writes instead is the entrant record
// (the slot the issued index names back) and the actor ref (the index that slot resolves to).
struct AdmitProbe {
    uint16_t sideIndex[facts::ENTITY_USER_SLOT_TABLE_MAX];    // the actor ref resolved per slot
    uint16_t entrantSlot[facts::ENTITY_USER_SLOT_TABLE_MAX];  // the slot each index's record names
    bool     createsUser   = true;   // the entrant table names the slot back for the issued index
    bool     createsPlayer = true;   // an actor component ref resolves for the slot
    bool     claimsConn    = false;  // only the net layer's add-entrant handler ever writes this
    // The engine's OTHER table, and the one L3 #16 found empty: a component instance exists
    // for an entrant's actor only because NETGAME.SET_PLAYER_ACTOR built that actor out of a
    // body template.  The bot insert does not do that, so the probe's admit step does not
    // fill this -- only the actor step does.
    uint16_t actorCi[facts::ENTITY_USER_SLOT_TABLE_MAX];
    bool     actorStepMakesCi = true;   // false models a step that ran and attached nothing
    uint32_t actorSteps       = 0;
    uint16_t steppedIndex     = kSlotUnmapped;

    // L3 #21's hang, modelled as the engine's own answer rather than as a belief: the refresh may
    // be issued only on an entrant that already holds the component instance it refreshes.
    bool     assetsReady      = true;
    // What AdmitHooks::seated reported, and how often.
    uint32_t seatedSlots      = 0;
    uint16_t lastSeatedSlot   = kSlotUnmapped;
    uint16_t lastSeatedIndex  = kSlotUnmapped;
    uint32_t readinessAsks    = 0;

    // THE DRESS SIDE, and the RUN-18 shape is one field of it.  loadouts[slot] is what the engine
    // would print as "Loadouts : N" for that slot; it starts at 0 for every slot because that is
    // what the bot insert leaves behind, and it is the profile apply -- not the insert -- that
    // puts instances there.  instancesAfterProfile = 0 is L3 #18 verbatim: a dress that ran,
    // reported success, and left the entrant with nothing for the customisation walk to find.
    uint64_t loadouts[facts::ENTITY_USER_SLOT_TABLE_MAX];
    uint64_t instancesAfterProfile = 1;
    // The SOURCE container the bot insert (AddBotUser -> GenerateBotProfile)
    // populates -- the entrant's user record -- as distinct from `loadouts[]`, the per-slot live
    // table the guard reads.  `botDefaultLoadout` is the instance count the insert leaves in the
    // source; the publish hook is what copies it across (IDENT.LOADOUT_ASSIGN), and until it runs
    // the per-slot count is 0.  0 models a source the default gen has not filled yet.
    uint64_t userLoadout[facts::ENTITY_USER_SLOT_TABLE_MAX];
    uint64_t botDefaultLoadout = 1;
    uint32_t publishSteps = 0;
    uint32_t publishSeq   = 0;
    // The recorded loadout the SOURCE fill parses in AFTER the insert has
    // clobbered the source (GenerateBotProfile), and BEFORE the publish copies source->live.  0
    // models "the recording carried no loadout"; a non-zero value is the recorded instance count the
    // fill lands in the source for the publish to bridge.
    uint64_t recordedSourceLoadout = 0;
    uint32_t sourceSteps = 0;
    uint32_t sourceSeq   = 0;
    // THE DOCUMENT THE ENTRANT CARRIES.  True is the bot insert's own outcome: the entrant is
    // handed the json db CACHE's document, so nothing can be written into it and the engine
    // cannot answer the recording back.  Only the attach clears it, by replacing the pointer --
    // which is why every success case in this file now depends on that step.
    bool     docCached     = true;
    bool     attachParses  = true;
    bool     attachStores  = true;
    uint32_t attachSteps   = 0;
    uint32_t attachSeq     = 0;
    uint16_t attachedSlot  = kSlotUnmapped;
    uint16_t attachedIndex = kSlotUnmapped;
    bool     profileApplies   = true;
    // The L3 #19 shape, and it is ONE field: the wrapper answered true and the engine's own
    // document does not carry the recording.  58 `json db is cached, read only` lines, a
    // published loadout, and no profile.
    bool     profileReadsBack = true;
    uint32_t readBackSteps    = 0;
    uint32_t readBackSeq      = 0;
    uint16_t readBackSlot     = kSlotUnmapped;
    uint16_t readBackIndex    = kSlotUnmapped;
    uint32_t profileSteps     = 0;
    uint16_t profiledSlot     = kSlotUnmapped;
    uint16_t profiledIndex    = kSlotUnmapped;
    // The ORDER witness.  A ticket taken by each step, so "the profile went on first" is a
    // comparison between two numbers rather than a belief about the source.
    uint32_t seq              = 0;
    uint32_t profileSeq       = 0;
    uint32_t actorSeq         = 0;
    uint16_t answers  = 7;
    uint32_t inserts  = 0;
    uint16_t admitted = kSlotUnmapped;

    // The L3 #14 shape, and it is the shape ENG-W2-P1-5 derived: admission never writes the side
    // table's +0 word, so every entry reads index 0, while the one real entrant -- the viewer --
    // is index 0 holding slot 0 and is the only one whose own record names a slot back.
    AdmitProbe() {
        for (uint16_t i = 0; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i) {
            sideIndex[i]   = 0;
            entrantSlot[i] = kSlotUnmapped;
            actorCi[i]     = kSlotUnmapped;
            loadouts[i]    = 0;
            userLoadout[i] = 0;
        }
        entrantSlot[0] = 0;
    }
};

AdmitProbe* Probe(void* ctx) { return static_cast<AdmitProbe*>(ctx); }

uint16_t ProbeIndexOfSlot(void* ctx, uint16_t slot) { return Probe(ctx)->sideIndex[slot]; }

uint16_t ProbeSlotOfIndex(void* ctx, uint16_t index) {
    return index < facts::ENTITY_USER_SLOT_TABLE_MAX ? Probe(ctx)->entrantSlot[index]
                                                     : kSlotUnmapped;
}

PawnProvision::Refusal ProbeAdmit(void* ctx, uint16_t slot, UserIndex& out) {
    AdmitProbe* p = Probe(ctx);
    ++p->inserts;
    p->admitted = slot;
    if (p->claimsConn) {
        const uint64_t peer = 0x00025F1A7C3D0777ull;
        memcpy(gConn + slot * facts::LOBBY_CONN_STRIDE, &peer, sizeof peer);
    }
    if (p->createsUser)   p->entrantSlot[p->answers] = slot;
    if (p->createsPlayer) p->sideIndex[slot] = p->answers;
    // AddBotUser calls GenerateBotProfile, which writes a default loadout into the entrant's USER
    // RECORD (the source), NOT the per-slot live table -- so the insert leaves the source filled and
    // the per-slot count still 0.  The publish hook is what bridges them.
    if (slot < facts::ENTITY_USER_SLOT_TABLE_MAX) p->userLoadout[slot] = p->botDefaultLoadout;
    out = UserIndex(p->answers);
    return PawnProvision::Refusal::None;
}

// The IDENT.LOADOUT_ASSIGN bridge: copy the source (user record) across to the per-slot live table,
// exactly as the engine's own add-entrant handler does above the insert.  Best-effort like the real
// hook -- an empty source copies 0 and the guard then defers.
bool ProbePublishLoadout(void* ctx, uint16_t slot, uint16_t index) {
    AdmitProbe* p = Probe(ctx);
    ++p->publishSteps;
    p->publishSeq = ++p->seq;
    (void)index;
    if (slot < facts::ENTITY_USER_SLOT_TABLE_MAX) p->loadouts[slot] = p->userLoadout[slot];
    return true;
}

// The recorded-loadout SOURCE fill (HookSourceLoadout -> DressLoadout Source):
// parse the recording's loadout into the SOURCE the insert just cleared, so the publish above copies
// the recording's instances -- not the empty default -- into the live table.  Best-effort like the
// real hook, and it takes a seq ticket so a test can prove it ran BEFORE the publish.
bool ProbeSourceLoadout(void* ctx, uint16_t slot, uint16_t index) {
    AdmitProbe* p = Probe(ctx);
    ++p->sourceSteps;
    p->sourceSeq = ++p->seq;
    (void)index;
    if (slot < facts::ENTITY_USER_SLOT_TABLE_MAX) p->userLoadout[slot] = p->recordedSourceLoadout;
    return p->recordedSourceLoadout != 0;
}

bool ProbeApplyProfile(void* ctx, uint16_t slot, uint16_t index) {
    AdmitProbe* p = Probe(ctx);
    ++p->profileSteps;
    p->profileSeq    = ++p->seq;
    p->profiledSlot  = slot;
    p->profiledIndex = index;
    if (!p->profileApplies) return false;
    if (slot < facts::ENTITY_USER_SLOT_TABLE_MAX) p->loadouts[slot] = p->instancesAfterProfile;
    return true;
}

bool ProbeProfileApplied(void* ctx, uint16_t slot, uint16_t index) {
    AdmitProbe* p = Probe(ctx);
    ++p->readBackSteps;
    p->readBackSeq   = ++p->seq;
    p->readBackSlot  = slot;
    p->readBackIndex = index;
    // A cached document answers the cache's values back, never the recording's.
    return p->profileReadsBack && !p->docCached;
}

uint64_t ProbeLoadoutCount(void* ctx, uint16_t slot) {
    return slot < facts::ENTITY_USER_SLOT_TABLE_MAX ? Probe(ctx)->loadouts[slot] : 0;
}

bool ProbeCreateActor(void* ctx, uint16_t index) {
    AdmitProbe* p = Probe(ctx);
    ++p->actorSteps;
    p->actorSeq = ++p->seq;
    p->steppedIndex = index;
    if (!p->actorStepMakesCi) return false;
    if (index >= facts::ENTITY_USER_SLOT_TABLE_MAX) return false;
    p->actorCi[index] = uint16_t(100 + index);
    return true;
}

uint16_t ProbeActorCi(void* ctx, uint16_t index) {
    return index < facts::ENTITY_USER_SLOT_TABLE_MAX ? Probe(ctx)->actorCi[index]
                                                     : kSlotUnmapped;
}

void ProbeSeated(void* ctx, uint16_t slot, uint16_t index) {
    AdmitProbe* q = Probe(ctx);
    ++q->seatedSlots;
    q->lastSeatedSlot = slot;
    q->lastSeatedIndex = index;
}

bool ProbeActorAssetsReady(void* ctx, uint16_t index) {
    AdmitProbe* p = Probe(ctx);
    ++p->readinessAsks;
    (void)index;
    return p->assetsReady;
}

bool StartsWith(const char* text, const char* prefix) {
    if (text == nullptr || prefix == nullptr) return false;
    while (*prefix != 0) {
        if (*text++ != *prefix++) return false;
    }
    return true;
}

PawnProvision::Refusal ProbeAttachProfile(void* ctx, uint16_t slot, uint16_t index) {
    AdmitProbe* p = Probe(ctx);
    ++p->attachSteps;
    p->attachSeq     = ++p->seq;
    p->attachedSlot  = slot;
    p->attachedIndex = index;
    if (!p->attachParses) return PawnProvision::Refusal::ProfileParseFailed;
    if (!p->attachStores) return PawnProvision::Refusal::ProfileAttachFailed;
    p->docCached = false;   // the replacement: the entrant now carries a document we own
    return PawnProvision::Refusal::None;
}

PawnProvision::AdmitHooks HooksFor(AdmitProbe& p) {
    PawnProvision::AdmitHooks h;
    h.ctx         = &p;
    h.indexOfSlot = &ProbeIndexOfSlot;
    h.slotOfIndex = &ProbeSlotOfIndex;
    h.admit       = &ProbeAdmit;
    h.attachProfile = &ProbeAttachProfile;
    h.applyProfile = &ProbeApplyProfile;
    h.profileApplied = &ProbeProfileApplied;
    h.loadoutCount = &ProbeLoadoutCount;
    h.createActor  = &ProbeCreateActor;
    h.actorCi      = &ProbeActorCi;
    h.actorAssetsReady = &ProbeActorAssetsReady;
    return h;
}

}  // namespace

TEST_CASE("the admission gate asks the lobby, and the side table only names the viewer's own slot") {
    // L3 #14, verbatim: `[pawn] recorded slot 0 -> slot 1 (class=none, ent=?)` -- the map walked
    // the lobby and slot 1 answered FREE -- and twelve lines later identity_access.cpp:1086
    // re-gated the SAME slot with the side table and refused SlotAlreadyOccupied, so the run
    // printed `provisioned 0`. Two occupancy surfaces, one call apart, disagreeing about slot 1.
    const uint64_t kViewer = 0x00025F1A7C3D1A2Bull;

    SUBCASE("the mapper lands on slot 1 and the gate admits that same slot") {
        Reset();
        SetUpLobby(kViewer);
        FakeIdentity id;
        // The viewer, in the surface the mapper actually asks: the entrant table.  Without it the
        // recording maps onto slot 0 and this case never reaches the gate it exists to exercise.
        id.Place(0, /*index=*/0, /*dressed=*/false);
        REQUIRE(id.SlotOf(id.IndexOf(UserSlot(0))).v == 0);
        PawnProvisioner p;
        ProvisionEnv env = Armed(id, 1);
        p.Arm(env);

        CHECK(p.Request(UserSlot(0), JoinDesc{}));
        CHECK(p.Pump() == 1);
        REQUIRE(p.MappedSlot(UserSlot(0)).v == 1);   // the line L3 #14 printed

        AdmitProbe probe;
        // RED BEFORE, and it is the old gate's own predicate spelled out: the side table names an
        // index for slot 1 -- it names index 0 for every slot -- so IndexOfSlot succeeded and the
        // admission refused the slot the engine's own predicate had just called free.
        REQUIRE(probe.sideIndex[1] != kSlotUnmapped);

        UserIndex out(kSlotUnmapped);
        CHECK(PawnProvision::AdmitIntoLobbySlot(gNetGame, 1, HooksFor(probe), out) ==
              PawnProvision::Refusal::None);
        CHECK(probe.inserts == 1);
        CHECK(probe.admitted == 1);
        CHECK(out.v == probe.answers);
    }

    SUBCASE("the viewer's slot is excluded by the entrant's own record, not by the table's zero") {
        Reset();
        SetUpLobby(facts::LOBBY_JOIN_FREE_SENTINEL);   // even slot 0's record reads free...
        AdmitProbe probe;                              // ...and entrant 0 still names slot 0 back
        UserIndex out(kSlotUnmapped);

        CHECK(PawnProvision::AdmitIntoLobbySlot(gNetGame, 0, HooksFor(probe), out) ==
              PawnProvision::Refusal::SlotAlreadyOccupied);
        CHECK(probe.inserts == 0);
        CHECK(out.v == kSlotUnmapped);

        // The same table, on the slot no entrant record names back, does not refuse -- which is
        // the difference between a cross-check and reading the table's bare index word as "held".
        CHECK(PawnProvision::AdmitIntoLobbySlot(gNetGame, 1, HooksFor(probe), out) ==
              PawnProvision::Refusal::None);
        CHECK(probe.admitted == 1);
    }

    SUBCASE("a conn record the lobby calls held does not refuse a local entrant") {
        // conn[1] carries a peer and no entrant record names slot 1.  The peer is the net layer's;
        // the insert this gate guards never writes that array, so it cannot be told no by it.
        Reset();
        SetUpLobby(kViewer);
        memcpy(gConn + 1 * facts::LOBBY_CONN_STRIDE, &kViewer, sizeof kViewer);
        PawnProvision::LobbyRecord rec{};
        REQUIRE(PawnProvision::ReadLobbySlot(gNetGame, 1, rec));
        REQUIRE(rec.state == PawnProvision::SlotOccupancy::Held);

        AdmitProbe probe;
        UserIndex out(kSlotUnmapped);
        CHECK(PawnProvision::AdmitIntoLobbySlot(gNetGame, 1, HooksFor(probe), out) ==
              PawnProvision::Refusal::None);
        CHECK(probe.inserts == 1);
    }

    // L3 #15 F2, and this is the regression: the run refused a CORRECT admission because the
    // postcondition read conn[slot].  sub_1406051C0 is the only writer of that qword and it is the
    // net layer's add-entrant handler; the insert this plugin calls is the engine's bot/local path
    // and leaves the record on the sentinel by design.  A local entrant that claims no connection
    // record is a provision, and the record is read here to say so out loud.
    SUBCASE("a local entrant claims no connection record and is still a provision") {
        Reset();
        SetUpLobby(kViewer);
        AdmitProbe probe;
        REQUIRE(probe.claimsConn == false);
        UserIndex out(kSlotUnmapped);
        CHECK(PawnProvision::AdmitIntoLobbySlot(gNetGame, 1, HooksFor(probe), out) ==
              PawnProvision::Refusal::None);
        CHECK(out.v == probe.answers);

        PawnProvision::LobbyRecord after{};
        REQUIRE(PawnProvision::ReadLobbySlot(gNetGame, 1, after));
        CHECK(after.state == PawnProvision::SlotOccupancy::Free);   // still the sentinel, and fine
    }

    SUBCASE("an insert that answers an index and creates no user record is not a provision") {
        Reset();
        SetUpLobby(kViewer);
        AdmitProbe probe;
        probe.createsUser = false;                  // the entrant table names nothing back
        UserIndex out(kSlotUnmapped);
        const PawnProvision::Refusal why =
            PawnProvision::AdmitIntoLobbySlot(gNetGame, 1, HooksFor(probe), out);

        CHECK(why == PawnProvision::Refusal::AdmitDidNotCreateUser);
        // The postcondition failing is THIS session's failure. Naming it with the word for a slot
        // somebody else holds is what sent L3 #14 looking for a full lobby that was never full.
        CHECK(why != PawnProvision::Refusal::SlotAlreadyOccupied);
        CHECK(probe.inserts == 1);
        CHECK(out.v == kSlotUnmapped);
    }

    SUBCASE("an entrant the engine placed no pawn for is not a provision") {
        Reset();
        SetUpLobby(kViewer);
        AdmitProbe probe;
        probe.createsPlayer = false;                // no actor component ref resolves for the slot
        UserIndex out(kSlotUnmapped);
        const PawnProvision::Refusal why =
            PawnProvision::AdmitIntoLobbySlot(gNetGame, 1, HooksFor(probe), out);

        CHECK(why == PawnProvision::Refusal::AdmitDidNotCreatePlayer);
        CHECK(why != PawnProvision::Refusal::SlotAlreadyOccupied);
        CHECK(probe.inserts == 1);
        CHECK(out.v == kSlotUnmapped);
    }

    SUBCASE("a lobby that cannot be walked admits a local entrant anyway") {
        // An offline session need not have a walkable connection chain at all.  The gate asks the
        // entrant table, which is present whenever the netgame is, so there is nothing to refuse.
        Reset();   // netgame + LOBBY_OFF is zero: there is no chain to ask
        AdmitProbe probe;
        UserIndex out(kSlotUnmapped);
        CHECK(PawnProvision::AdmitIntoLobbySlot(gNetGame, 1, HooksFor(probe), out) ==
              PawnProvision::Refusal::None);
        CHECK(probe.inserts == 1);
    }
}

// L3 #16 provisioned an entrant with `stateRaw=9` printed by the census, so the
// insert's own state-9 arm ran, and the run still read `driveable=no` on both surfaces with
// `remote: ci=ABSENT`.  The bytes say why: the bot insert IDENT.ADMIT_USER_VA registers the new
// user into R14PlayerBroadcaster, R14NetBot and RxAI and never calls NETGAME.SET_PLAYER_ACTOR_VA,
// the only site in the image that attaches R14RemotePlayer and R14NetPhysics to an entrant's
// actor, and one the engine reaches only from its REMOTE entrant admission.  So the admission is
// two steps, and the second one is what these cases pin.
// The bot-body hook set -- no recorded-profile dress (AddBotUser installs the default
// profile+loadout and a visible body itself), botBody flagged so AdmitIntoLobbySlot treats any
// overlay best-effort.  Internal-linkage Probe* fns are visible here in the same translation unit.
static PawnProvision::AdmitHooks BotHooksFor(AdmitProbe& p) {
    PawnProvision::AdmitHooks h;
    h.ctx              = &p;
    h.botBody          = true;
    h.indexOfSlot      = &ProbeIndexOfSlot;
    h.slotOfIndex      = &ProbeSlotOfIndex;
    h.admit            = &ProbeAdmit;
    h.createActor      = &ProbeCreateActor;
    h.actorCi          = &ProbeActorCi;
    h.actorAssetsReady = &ProbeActorAssetsReady;
    h.loadoutCount     = &ProbeLoadoutCount;
    h.publishLoadout   = &ProbePublishLoadout;   // always: bridges the default loadout before the guard
    return h;   // attach/apply/profileApplied deliberately null: the default look ships without them
}

TEST_CASE("The bot path admits and steps the actor without the recorded-profile dress") {
    UserIndex out(kSlotUnmapped);

    SUBCASE("with no profile hooks wired, botBody publishes the default loadout, then steps the actor") {
        AdmitProbe p;
        p.answers = 5;
        // The per-slot table starts EMPTY (the insert filled the user record, not this table); the
        // publish hook is what bridges the default loadout across before the guard.
        CHECK(p.loadouts[1] == 0);
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, BotHooksFor(p), out) ==
              PawnProvision::Refusal::None);
        CHECK(out.v == 5);
        CHECK(p.admitted == 1);
        CHECK(p.publishSteps == 1);              // the default loadout was bridged...
        CHECK(p.loadouts[1] == 1);               // ...so the per-slot count is non-empty at the guard
        CHECK(p.actorSteps == 1);                // SET_PLAYER_ACTOR reached -> phys/remote CI install
        CHECK(p.steppedIndex == 5);
        CHECK(p.attachSteps == 0);               // no recorded-profile dress on the default-look path
        CHECK(p.profileSteps == 0);
        CHECK(p.readBackSteps == 0);
    }

    SUBCASE("Without the publish hook the default loadout never reaches the guard") {
        AdmitProbe p;
        p.answers = 5;
        PawnProvision::AdmitHooks h = BotHooksFor(p);
        h.publishLoadout = nullptr;              // the pre-fix wiring: the bridge is absent
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, h, out) ==
              PawnProvision::Refusal::LoadoutsEmpty);   // exactly the launch's blocker
        CHECK(p.publishSteps == 0);
        CHECK(p.actorSteps == 0);                // the actor step never ran -> no phys CI -> no body
    }

    SUBCASE("botBody defers (LoadoutsEmpty) when the insert's default gen has not filled the source") {
        AdmitProbe p;
        p.answers = 5;
        p.botDefaultLoadout = 0;                 // GenerateBotProfile left the source empty this tick
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, BotHooksFor(p), out) ==
              PawnProvision::Refusal::LoadoutsEmpty);
        CHECK(p.publishSteps == 1);              // the bridge ran and copied an empty source...
        CHECK(p.loadouts[1] == 0);               // ...so the count is still 0
        CHECK(p.actorSteps == 0);                // the crash guard kept the actor step away
    }

    // `seated` is what tells the caller it now OWNS this entrant, and the two deferrals
    // below leave a seated entrant behind. Reported only on the return value, the leave path then
    // refuses to remove its own admission on every load whose admit pass happened to defer -- which
    // is exactly what the first launch of this build measured (NotOurEntrant x3 on the second unload).
    SUBCASE("a deferring pass still reports the slot it seated") {
        for (PawnProvision::Refusal want : {PawnProvision::Refusal::LoadoutsEmpty,
                                            PawnProvision::Refusal::ActorAssetsNotReady}) {
            AdmitProbe p;
            p.answers = 5;
            if (want == PawnProvision::Refusal::LoadoutsEmpty) {
                p.botDefaultLoadout = 0;
            } else {
                p.loadouts[1] = 1;
                p.assetsReady = false;
            }
            PawnProvision::AdmitHooks h = BotHooksFor(p);
            h.seated = &ProbeSeated;
            CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, h, out) == want);
            CHECK(p.seatedSlots == 1);          // reported once, for this slot, before the deferral
            CHECK(p.lastSeatedSlot == 1);
            CHECK(p.lastSeatedIndex == 5);
        }
    }

    SUBCASE("a refused insert reports nothing: there is no entrant to own") {
        AdmitProbe p;
        p.answers = 5;
        p.createsUser = false;               // the postcondition pair does not name the slot back
        PawnProvision::AdmitHooks h = BotHooksFor(p);
        h.seated = &ProbeSeated;
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, h, out) ==
              PawnProvision::Refusal::AdmitDidNotCreateUser);
        CHECK(p.seatedSlots == 0);
    }

    SUBCASE("botBody defers (ActorAssetsNotReady) when the actor component does not resolve") {
        AdmitProbe p;
        p.answers = 5;
        p.loadouts[1] = 1;
        p.assetsReady = false;                   // L3 #21 readiness predicate still gates the step
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, BotHooksFor(p), out) ==
              PawnProvision::Refusal::ActorAssetsNotReady);
        CHECK(p.actorSteps == 0);
    }

    SUBCASE("the recorded-loadout overlay is best-effort: a failing dress never discards the body") {
        AdmitProbe p;
        p.answers = 5;
        p.loadouts[1] = 1;                       // the default look is present regardless of the overlay
        p.attachParses  = false;                 // the overlay attach fails...
        p.profileApplies = false;                // ...and so does the overlay apply
        PawnProvision::AdmitHooks h = BotHooksFor(p);
        h.attachProfile  = &ProbeAttachProfile;
        h.applyProfile   = &ProbeApplyProfile;
        h.profileApplied = &ProbeProfileApplied;
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, h, out) ==
              PawnProvision::Refusal::None);     // the visible default body still admits
        CHECK(p.actorSteps == 1);
        CHECK(p.attachSteps == 1);               // the overlay was attempted...
        CHECK(p.profileSteps == 1);              // ...best-effort, and its failure did not refuse
    }

    SUBCASE("the dress relaxation is scoped to botBody: the remote path still requires the dress hooks") {
        AdmitProbe p;
        PawnProvision::AdmitHooks h = HooksFor(p);
        h.botBody = false;                       // the ordinary remote path...
        h.attachProfile = nullptr;               // ...with a missing dress hook
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, h, out) ==
              PawnProvision::Refusal::AdmitNotPromoted);
        CHECK(p.inserts == 0);                   // nothing was admitted
    }
}

// ENG-BOT-BODY-SOURCE (echo-102).  The working-body experiment: the recorded loadout is parsed into
// the SOURCE AFTER the insert (which clears it), then bridged into the live table, so the L3 #18
// guard passes on REAL recorded instances and the actor step builds a body.  These two cases pin that
// the sourceLoadout hook is LOAD-BEARING (with it: a body; without it: the prior deferral) and that it
// is ORDERED before the publish -- if the fill ran after the copy, the copy would carry the empty
// source and the body would never build.
TEST_CASE("The recorded SOURCE fill is load-bearing and ordered before the bridge") {
    UserIndex out(kSlotUnmapped);

    SUBCASE("flag ON (sourceLoadout wired): recorded fill runs first, count>0, the actor steps") {
        AdmitProbe p;
        p.answers = 5;
        // echo-98 exactly: GenerateBotProfile leaves the SOURCE empty for the recorded-identity bot...
        p.botDefaultLoadout    = 0;
        // ...and the recording DID carry a loadout, which the SOURCE fill parses in.
        p.recordedSourceLoadout = 1;
        PawnProvision::AdmitHooks h = BotHooksFor(p);
        h.sourceLoadout = &ProbeSourceLoadout;   // the flag-ON wiring
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, h, out) == PawnProvision::Refusal::None);
        CHECK(out.v == 5);
        CHECK(p.sourceSteps == 1);               // the recorded fill ran...
        CHECK(p.publishSteps == 1);              // ...and the bridge ran...
        CHECK(p.sourceSeq < p.publishSeq);       // ...with the fill BEFORE the copy (the whole point)
        CHECK(p.loadouts[1] == 1);               // so the recording's instances reached the live table
        CHECK(p.actorSteps == 1);                // guard passed on REAL data -> SET_PLAYER_ACTOR
        CHECK(p.steppedIndex == 5);
    }

    SUBCASE("flag OFF (sourceLoadout null): the same empty source defers, no body -- the shipped default") {
        AdmitProbe p;
        p.answers = 5;
        p.botDefaultLoadout    = 0;              // same empty source...
        p.recordedSourceLoadout = 1;             // ...same recording available, but the hook is absent
        PawnProvision::AdmitHooks h = BotHooksFor(p);   // no sourceLoadout: the default build
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, h, out) ==
              PawnProvision::Refusal::LoadoutsEmpty);
        CHECK(p.sourceSteps == 0);               // nothing filled the source...
        CHECK(p.publishSteps == 1);              // ...the bridge copied the empty source...
        CHECK(p.loadouts[1] == 0);               // ...so the count is still 0...
        CHECK(p.actorSteps == 0);                // ...and the actor step never ran (no deadlock risk)
    }
}

TEST_CASE("a provisioned entrant gets the engine's own actor step, and the CI it leaves is read back") {
    UserIndex out(kSlotUnmapped);

    SUBCASE("the step runs once, on the index the insert issued, and the admission succeeds") {
        AdmitProbe p;
        p.answers = 7;
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, HooksFor(p), out) ==
              PawnProvision::Refusal::None);
        CHECK(out.v == 7);
        CHECK(p.actorSteps == 1);
        CHECK(p.steppedIndex == 7);
        CHECK(p.actorCi[7] == 107);
    }

    SUBCASE("an actor step that attaches nothing is AdmitDidNotCreatePhysics, and out is absence") {
        AdmitProbe p;
        p.answers = 7;
        p.actorStepMakesCi = false;
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, HooksFor(p), out) ==
              PawnProvision::Refusal::AdmitDidNotCreatePhysics);
        CHECK(out.v == kSlotUnmapped);
        CHECK(p.actorSteps == 1);   // it was ASKED and answered no; this is not could-not-ask
    }

    SUBCASE("the refusal carries its own enum name in front of the sentence") {
        CHECK(StartsWith(PawnProvision::RefusalText(
                             PawnProvision::Refusal::AdmitDidNotCreatePhysics),
                         "AdmitDidNotCreatePhysics: "));
        CHECK(StartsWith(PawnProvision::RefusalName(
                             PawnProvision::Refusal::AdmitDidNotCreatePhysics),
                         "AdmitDidNotCreatePhysics"));
    }

    SUBCASE("a build with no actor step bound refuses by name and calls no insert") {
        AdmitProbe p;
        PawnProvision::AdmitHooks h = HooksFor(p);
        h.createActor = nullptr;
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, h, out) ==
              PawnProvision::Refusal::AdmitNotPromoted);
        CHECK(p.inserts == 0);

        PawnProvision::AdmitHooks h2 = HooksFor(p);
        h2.actorCi = nullptr;
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, h2, out) ==
              PawnProvision::Refusal::AdmitNotPromoted);
        CHECK(p.inserts == 0);
    }

    SUBCASE("a refused insert never reaches the actor step") {
        AdmitProbe p;
        p.createsUser = false;
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, HooksFor(p), out) ==
              PawnProvision::Refusal::AdmitDidNotCreateUser);
        CHECK(p.actorSteps == 0);
    }
}

// The state gate is a DEFERRAL and it is the sequencing rule of the whole packet: admitted below
// NETGAME.STATE_INMATCH an entrant gets an actor GUID and no placed pawn, silently.
TEST_CASE("the session blocker is the state word, and nothing else") {
    uint32_t state = facts::NETGAME_STATE_INMATCH;
    CHECK(PawnProvision::SessionBlocker(&state) == PawnProvision::Refusal::None);
    state = facts::NETGAME_STATE_INMATCH - 1;
    CHECK(PawnProvision::SessionBlocker(&state) == PawnProvision::Refusal::SessionNotInMatch);
    state = facts::NETGAME_STATE_INMATCH + 1;
    CHECK(PawnProvision::SessionBlocker(&state) == PawnProvision::Refusal::SessionNotInMatch);
    CHECK(PawnProvision::SessionBlocker(nullptr) == PawnProvision::Refusal::NoNetGame);
}

// L3 #18 F1, and this is that run's own shape: the insert landed into slot 1 ("Adding bot user to
// slot 1, idx 1"), the engine printed "Loadouts : 0" in the same entrant block, and the actor step
// was asked for anyway -- "Player Actor Changing ... (enabled)" -- whereupon "Unknown loadout
// instance name ... User has 0 loadout instances" was followed by "Offset is past the end of the
// stream" at cmemstream.cpp:89, "Deadlock detected!" and 0xC0000005 at +42 s.  The guard is not a
// flag: it is a POSTCONDITION read off the engine's own loadout count, in front of the one call
// that faults on it.
TEST_CASE("an entrant with no loadout instances never reaches the engine's actor step") {
    UserIndex out(kSlotUnmapped);

    SUBCASE("RUN-18 verbatim: the dress ran, the count is still 0, and the step is NOT called") {
        AdmitProbe p;
        p.answers = 1;
        p.instancesAfterProfile = 0;   // "Loadouts : 0"
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, HooksFor(p), out) ==
              PawnProvision::Refusal::LoadoutsEmpty);
        CHECK(out.v == kSlotUnmapped);
        CHECK(p.inserts == 1);         // the insert DID land -- this is not a refusal to admit
        CHECK(p.profileSteps == 1);    // and the dress WAS attempted -- not a could-not-ask
        CHECK(p.actorSteps == 0);      // ...and the call that took the process down was not made
    }

    SUBCASE("a dress that could not be installed is ProfileNotApplied, and the step is NOT called") {
        AdmitProbe p;
        p.answers = 1;
        p.profileApplies = false;
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, HooksFor(p), out) ==
              PawnProvision::Refusal::ProfileNotApplied);
        CHECK(out.v == kSlotUnmapped);
        CHECK(p.actorSteps == 0);
    }

    SUBCASE("L3 #19: the dress REPORTS success, the engine does not carry it, and no actor is asked for") {
        AdmitProbe p;
        p.answers = 1;
        p.profileApplies    = true;    // `SetProfileJson` returned SUCCESS...
        p.profileReadsBack  = false;   // ...over 58 `json db is cached, read only` refusals
        p.instancesAfterProfile = 1;   // and the loadout publish DID leave instances behind,
                                       // so LoadoutsEmpty cannot stand in for this refusal
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, HooksFor(p), out) ==
              PawnProvision::Refusal::ProfileNotApplied);
        CHECK(out.v == kSlotUnmapped);
        CHECK(p.profileSteps  == 1);   // the dress was attempted -- this is not could-not-ask
        CHECK(p.readBackSteps == 1);   // ...and the ENGINE was asked, which is the new fact
        CHECK(p.actorSteps    == 0);   // the call that hung the process was not made
    }

    SUBCASE("the read-back is asked of the entrant that was issued, and BEFORE the actor step") {
        AdmitProbe p;
        p.answers = 1;
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, HooksFor(p), out) ==
              PawnProvision::Refusal::None);
        REQUIRE(p.readBackSeq != 0);
        REQUIRE(p.actorSeq != 0);
        CHECK(p.profileSeq  < p.readBackSeq);
        CHECK(p.readBackSeq < p.actorSeq);
        CHECK(p.readBackSlot  == 1);
        CHECK(p.readBackIndex == 1);
    }

    SUBCASE("one instance is enough: the step runs and the admission succeeds") {
        AdmitProbe p;
        p.answers = 1;
        p.instancesAfterProfile = 1;
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, HooksFor(p), out) ==
              PawnProvision::Refusal::None);
        CHECK(out.v == 1);
        CHECK(p.actorSteps == 1);
    }

    SUBCASE("THE ORDER: the profile is on the entrant before the actor is asked for") {
        AdmitProbe p;
        p.answers = 1;
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, HooksFor(p), out) ==
              PawnProvision::Refusal::None);
        REQUIRE(p.profileSeq != 0);
        REQUIRE(p.actorSeq != 0);
        CHECK(p.profileSeq < p.actorSeq);
        // ...and it dressed the entrant the insert actually issued, at the slot it landed in
        CHECK(p.profiledSlot == 1);
        CHECK(p.profiledIndex == 1);
        CHECK(p.steppedIndex == 1);
    }

    SUBCASE("a build with no dress bound refuses by name and calls no insert") {
        AdmitProbe p;
        PawnProvision::AdmitHooks h = HooksFor(p);
        h.applyProfile = nullptr;
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, h, out) ==
              PawnProvision::Refusal::AdmitNotPromoted);
        CHECK(p.inserts == 0);

        PawnProvision::AdmitHooks h2 = HooksFor(p);
        h2.loadoutCount = nullptr;
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, h2, out) ==
              PawnProvision::Refusal::AdmitNotPromoted);
        CHECK(p.inserts == 0);

        // A build that cannot ASK whether the profile landed must not admit either: the read-back
        // is the decision, so an unbound one is could-not-ask, not permission.
        PawnProvision::AdmitHooks h3 = HooksFor(p);
        h3.profileApplied = nullptr;
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, h3, out) ==
              PawnProvision::Refusal::AdmitNotPromoted);
        CHECK(p.inserts == 0);
        CHECK(p.actorSteps == 0);
    }

    SUBCASE("both refusals lead with their own enum name") {
        CHECK(StartsWith(PawnProvision::RefusalText(PawnProvision::Refusal::LoadoutsEmpty),
                         "LoadoutsEmpty: "));
        CHECK(StartsWith(PawnProvision::RefusalText(PawnProvision::Refusal::ProfileNotApplied),
                         "ProfileNotApplied: "));
    }
}

// The descriptor is the only thing that carries the recording's clothes into the admission, so a
// field it drops is a player who arrives naked at the actor step.  Authored from the record, and
// from nothing else.
TEST_CASE("the join descriptor carries the recorded profile and loadout, not just the identity") {
    PlayerRecord rec{};
    rec.xplatformId = 0x00025F1A7C3D1A2Bull;
    rec.teamId = 1;
    rec.loadoutNumber = 4;
    rec.loadoutNumberBody = 9;
    static const uint8_t kProfile[] = "{\"displayname\":\"KFC\"}";
    static const uint8_t kUnlocks[] = "{\"unlocks\":{}}";
    rec.entrantJson = Blob{kProfile, uint32_t(sizeof kProfile - 1)};
    rec.unlocksJson = Blob{kUnlocks, uint32_t(sizeof kUnlocks - 1)};
    for (size_t i = 0; i < 20; ++i) rec.loadoutSlots[i] = Symbol64(0x1000ull + i);

    const JoinDesc d = JoinDescFromRecord(rec);
    CHECK(d.entrantJson.p == kProfile);
    CHECK(d.entrantJson.n == uint32_t(sizeof kProfile - 1));
    CHECK(d.unlocksJson.p == kUnlocks);
    CHECK(d.loadoutNumber == 4);
    CHECK(d.loadoutNumberBody == 9);
    for (size_t i = 0; i < 20; ++i) CHECK(d.loadoutSlots[i] == Symbol64(0x1000ull + i));
}

// The engine's own write gate, tested over a document this test owns.  IDENT.JSON_SET_STRING and
// IDENT.JSON_SET_INT open `cmp qword ptr [rcx+8], 0` and write only on the zero arm; L3 #19's 58
// `json db is cached, read only` lines are the other arm, and this is the one qword that tells
// them apart before a single key is spent.
TEST_CASE("a cached profile document is not writable, and the predicate says so off one qword") {
    // The flag is planted where the ENGINE's setters read it -- the offset comes from the registry
    // through identity_access, never written down again here.
    const size_t off = identity_access::ProfileDocCachedOffset();
    uint8_t doc[64] = {};
    REQUIRE(off + sizeof(uint64_t) <= sizeof doc);

    CHECK(identity_access::ProfileDocWritable(doc));

    doc[off] = 1;   // the json db owns this document: every set on it is refused read-only
    CHECK_FALSE(identity_access::ProfileDocWritable(doc));

    memset(doc + off, 0xFF, sizeof(uint64_t));
    CHECK_FALSE(identity_access::ProfileDocWritable(doc));

    memset(doc + off, 0, sizeof(uint64_t));
    memset(doc, 0xFF, off);   // the root descriptor is not the flag and must not be read as one
    CHECK(identity_access::ProfileDocWritable(doc));

    CHECK_FALSE(identity_access::ProfileDocWritable(nullptr));
}

TEST_CASE("P1-16: the cached document is REPLACED first, then dressed, read back, and only then acted on") {
    // L3 #19's shape without the fix: the entrant carries the json db's own document, every
    // setter refuses it, and the engine cannot answer the recording back.  The attach is what
    // makes the rest of the ladder possible at all, so it is asked FIRST.
    AdmitProbe p;
    UserIndex out(kSlotUnmapped);
    CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, HooksFor(p), out) ==
          PawnProvision::Refusal::None);
    CHECK(p.attachSteps == 1);
    CHECK(p.attachedSlot == 1);
    CHECK(p.attachedIndex == p.profiledIndex);
    CHECK_FALSE(p.docCached);
    REQUIRE(p.attachSeq != 0);
    CHECK(p.attachSeq < p.profileSeq);
    CHECK(p.profileSeq < p.readBackSeq);
    CHECK(p.readBackSeq < p.actorSeq);
}

TEST_CASE("P1-16: a recording the parser refuses and a store that does not land refuse BY NAME") {
    SUBCASE("the parser refuses the recording") {
        AdmitProbe p;
        p.attachParses = false;
        UserIndex out(kSlotUnmapped);
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, HooksFor(p), out) ==
              PawnProvision::Refusal::ProfileParseFailed);
        CHECK(p.profileSteps == 0);
        CHECK(p.readBackSteps == 0);
        CHECK(p.actorSteps == 0);
    }
    SUBCASE("the store did not read back") {
        AdmitProbe p;
        p.attachStores = false;
        UserIndex out(kSlotUnmapped);
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, HooksFor(p), out) ==
              PawnProvision::Refusal::ProfileAttachFailed);
        CHECK(p.docCached);
        CHECK(p.actorSteps == 0);
    }
    SUBCASE("an unbound attach hook is could-not-ask, and nothing is created") {
        AdmitProbe p;
        PawnProvision::AdmitHooks h = HooksFor(p);
        h.attachProfile = nullptr;
        UserIndex out(kSlotUnmapped);
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, h, out) ==
              PawnProvision::Refusal::AdmitNotPromoted);
        CHECK(p.inserts == 0);
        CHECK(p.attachSteps == 0);
        CHECK(p.actorSteps == 0);
    }
}

TEST_CASE("P1-16: the store REPLACES the entrant document pointer, it does not write through it") {
    // The engine's own store, driven over memory this test owns: 0x14019F613 overwrites the
    // pointer at IDENT.PROFILE_JSON_OFF and the document that was there is abandoned -- not
    // freed, not written into.  Both offsets come from identity_access, so no number is
    // written down twice.
    const uint16_t index = 1;
    const size_t field = replay::identity_access::ProfileDocFieldOffset(index);
    const size_t cachedOff = replay::identity_access::ProfileDocCachedOffset();
    alignas(8) uint8_t ng[4096] = {0};
    alignas(8) uint8_t cached[64] = {0};
    alignas(8) uint8_t mine[64] = {0};
    REQUIRE(field + sizeof(void*) <= sizeof(ng));
    REQUIRE(cachedOff + sizeof(uint64_t) <= sizeof(cached));
    cached[cachedOff] = 1;   // the json db's own document: the qword both setters refuse on
    void** slot = reinterpret_cast<void**>(ng + field);
    *slot = cached;
    REQUIRE_FALSE(replay::identity_access::ProfileDocWritable(*slot));

    CHECK(replay::identity_access::StoreProfileDocument(ng, index, mine) == replay::identity_access::AttachResult::Ok);
    CHECK(*slot == static_cast<void*>(mine));
    CHECK(replay::identity_access::ProfileDocWritable(*slot));
    CHECK(cached[cachedOff] == 1);   // the displaced document is untouched, and still refused
}

// -------------------------------------------------------------------------------------------
// ENG-W2-P1-17 -- the L3 #20 (pid 62356) fault, and the two things that keep it from recurring.
// -------------------------------------------------------------------------------------------

// The minidump's exception record names echovr.exe+0x5EFA36 == IDENT.JSON_ELEM_COUNT+0x16, reached
// from HookAttachProfile: the site table's rows and the Site enum had drifted apart by three, so
// Site::JsonParseText named IDENT.JSON_ELEM_COUNT and the attach called the child-counter with the
// recording's TEXT where a node belongs.  The pairing is spelled here a SECOND time, by hand, so a
// reordering of either list is caught by a test instead of by a launch.  Neither shipped guard can
// do this: the count static_assert compares two lengths, and VerifySite compares row i's recorded
// bytes at row i's own address, which a misordered table satisfies exactly as well.
TEST_CASE("every json entry point the profile path binds resolves to its own recorded fact") {
    using identity_access::JsonEntryPoint;
    using identity_access::JsonEntryPointVa;

    CHECK(JsonEntryPointVa(JsonEntryPoint::NodeKind)   == facts::IDENT_JSON_NODE_KIND);
    CHECK(JsonEntryPointVa(JsonEntryPoint::NodeName)   == facts::IDENT_JSON_NODE_NAME);
    CHECK(JsonEntryPointVa(JsonEntryPoint::NodeString) == facts::IDENT_JSON_NODE_STRING);
    CHECK(JsonEntryPointVa(JsonEntryPoint::NodeInt)    == facts::IDENT_JSON_NODE_INT);
    CHECK(JsonEntryPointVa(JsonEntryPoint::NodeBool)   == facts::IDENT_JSON_NODE_BOOL);
    CHECK(JsonEntryPointVa(JsonEntryPoint::NextElem)   == facts::IDENT_JSON_NEXT_ELEM);
    CHECK(JsonEntryPointVa(JsonEntryPoint::PathElem)   == facts::IDENT_JSON_PATH_ELEM);
    CHECK(JsonEntryPointVa(JsonEntryPoint::ElemCount)  == facts::IDENT_JSON_ELEM_COUNT);
    CHECK(JsonEntryPointVa(JsonEntryPoint::FindNode)   == facts::IDENT_JSON_FIND_NODE);
    CHECK(JsonEntryPointVa(JsonEntryPoint::IsEmpty)    == facts::IDENT_JSON_IS_EMPTY);

    // The three the recording's own attach calls, named against the addresses they were WRONGLY
    // bound to at e615578.  The first of these three is the 62356 fault, verbatim.
    CHECK(JsonEntryPointVa(JsonEntryPoint::ParseText) != facts::IDENT_JSON_ELEM_COUNT);
    CHECK(JsonEntryPointVa(JsonEntryPoint::DocCtor)   != facts::IDENT_JSON_PATH_ELEM);
    CHECK(JsonEntryPointVa(JsonEntryPoint::GetInt)    != facts::IDENT_JSON_FIND_NODE);

    // Distinct entry points stay distinct: a table collapsed onto one row would satisfy every
    // inequality above and none of these.
    CHECK(JsonEntryPointVa(JsonEntryPoint::ParseText) != JsonEntryPointVa(JsonEntryPoint::GetInt));
    CHECK(JsonEntryPointVa(JsonEntryPoint::DocCtor)   != JsonEntryPointVa(JsonEntryPoint::ParseText));
}

// The plugin-side gate: a document that cannot be a json OBJECT is refused BY NAME before any
// engine address is touched, so the answer survives where a fault does not.
TEST_CASE("a recorded profile that cannot be a json object refuses ProfileParseFailed") {
    using identity_access::AttachProfileDocument;
    using identity_access::AttachResult;
    using identity_access::RecordedProfileJsonAcceptable;

    static const uint8_t kGood[]      = "{\"_version\":3,\"loadout\":{\"emote\":\"x\"}} ";
    static const uint8_t kTruncated[] = "{\"_version\":3,\"loadout\":{";
    static const uint8_t kArrayRoot[] = "[1,2,3]";
    static const uint8_t kTrailing[]  = "{\"a\":1} junk";
    static const uint8_t kOpenString[]= "{\"a\":\"unterminated}";
    static const uint8_t kNulInside[] = "{\"a\":1\0,\"b\":2}";
    static const uint8_t kBraceOnly[] = "}";

    CHECK(RecordedProfileJsonAcceptable(Blob{kGood, uint32_t(sizeof kGood - 1)}));
    CHECK_FALSE(RecordedProfileJsonAcceptable(Blob{kTruncated, uint32_t(sizeof kTruncated - 1)}));
    CHECK_FALSE(RecordedProfileJsonAcceptable(Blob{kArrayRoot, uint32_t(sizeof kArrayRoot - 1)}));
    CHECK_FALSE(RecordedProfileJsonAcceptable(Blob{kTrailing, uint32_t(sizeof kTrailing - 1)}));
    CHECK_FALSE(RecordedProfileJsonAcceptable(Blob{kOpenString, uint32_t(sizeof kOpenString - 1)}));
    CHECK_FALSE(RecordedProfileJsonAcceptable(Blob{kNulInside, uint32_t(sizeof kNulInside - 1)}));
    CHECK_FALSE(RecordedProfileJsonAcceptable(Blob{kBraceOnly, uint32_t(sizeof kBraceOnly - 1)}));
    CHECK_FALSE(RecordedProfileJsonAcceptable(Blob{kGood, 0}));
    CHECK_FALSE(RecordedProfileJsonAcceptable(Blob{nullptr, 4}));

    // Through the attach, with a session present so the gate is what answers.  The malformed
    // document is ParseFailed with no engine entry point resolved at all; the well-formed one gets
    // PAST the gate and stops at the could-not-ask answer, which is what a process with no
    // echovr.exe mapped must give.  Two different words for two different facts.
    uint8_t ng[64] = {};
    CHECK(AttachProfileDocument(ng, UserIndex(0), Blob{kTruncated, uint32_t(sizeof kTruncated - 1)})
          == AttachResult::ParseFailed);
    CHECK(AttachProfileDocument(ng, UserIndex(0), Blob{kGood, uint32_t(sizeof kGood - 1)})
          == AttachResult::AttachFailed);
    CHECK(AttachProfileDocument(nullptr, UserIndex(0), Blob{kGood, uint32_t(sizeof kGood - 1)})
          == AttachResult::AttachFailed);
}

// ENG-LOADOUT-GUARD S7's open item: the census now asks CensusLoadout per mapped slot.
TEST_CASE("the census prints a loadout count per entrant and names why when it could not ask") {
    CensusEntity e;
    e.PlantRemote(/*slot*/ 1, /*ci*/ 3, /*phys*/ 4, /*driveable*/ true);
    CaptureLog log;

    CHECK(CensusProvisionedPawns(MappedLedger(0, 1), e, int32_t(facts::NETGAME_STATE_INMATCH),
                                 log) == 1);
    // Unwired, so the field must carry the REASON. `loadouts=0` here would read as an entrant that
    // was asked and answered nothing on, which is a different fact from never having been asked.
    CHECK(log.Count("loadouts=?(NoNetGame)") == 1);
    CHECK(log.Count("loadouts=0") == 0);

    // Wired to a session-shaped block of zeroes: the read is REACHED -- the reason changes -- and
    // it still refuses by name rather than answering a count out of memory nobody wrote.
    CaptureLog wired;
    std::vector<uint8_t> ng(4u << 20, 0u);
    CHECK(CensusProvisionedPawns(MappedLedger(0, 1), e, int32_t(facts::NETGAME_STATE_INMATCH),
                                 wired, ng.data()) == 1);
    CHECK(wired.Count("loadouts=?(NoNetGame)") == 0);
    CHECK(wired.Count("loadouts=?(") == 1);
}

// ENG-RESIDUALS-1 (b). `model: resolved=no` published the verdict and named nothing, so
// DIAG-PAWN-STATIC had to reconstruct H3 -- ComponentOnActor answering kNoComponent -- out of an
// engine dump instead of reading it off our own line.
TEST_CASE("the census names a reason when the model does not resolve, and names none when it does") {
    CensusEntity e;
    e.PlantRemote(/*slot*/ 1, /*ci*/ 3, /*phys*/ 4, /*driveable*/ true);

    // The control, and it is the thing that keeps the case honest: this fixture's ModelVisible
    // ANSWERS, so there is nothing to name and the line carries no reason at all. A reason printed
    // on every row would be naming nothing.
    CaptureLog answered;
    CHECK(CensusProvisionedPawns(MappedLedger(0, 1), e, int32_t(facts::NETGAME_STATE_INMATCH),
                                 answered) == 1);
    CHECK(answered.Count("model: resolved=yes") == 1);
    CHECK(answered.Count("reason=") == 0);

    // The same slot, the same pools, one gate changed.
    e.modelResolves = false;
    CaptureLog refused;
    CHECK(CensusProvisionedPawns(MappedLedger(0, 1), e, int32_t(facts::NETGAME_STATE_INMATCH),
                                 refused) == 1);
    CHECK(refused.Count("model: resolved=no visible=no "
                        "reason=noModelOnActor loadouts=?(NoNetGame)") == 1);
}

// L3 #21: the main thread went into NETGAME.REFRESH_PLAYER_ACTOR from inside the engine's own
// per-frame update and never came back.  The minidump carries no wait primitive of any kind on
// that thread -- Rip was in engine code, the stack ran HookCreateActor+0x70 ->
// sub_14019F1C0+0x3C -> sub_1401B0D80+0x970 -> sub_14050EE50+0x4D5 into the component walk -- so
// the engine's watchdog, not a lock, is what ended the process.  A call that can fail to RETURN
// is not one a tick may make speculatively, and the contract its own two callers satisfy is that
// the entrant is ALREADY a remote player.  These cases pin the order: ask the engine first, and
// when it says no, make no engine call at all.
TEST_CASE("the actor step is issued only on an entrant the engine already holds a component instance for") {
    SUBCASE("readiness false refuses ActorAssetsNotReady and the engine is never called") {
        AdmitProbe p;
        p.assetsReady = false;
        p.loadouts[1] = 2;
        PawnProvision::AdmitHooks h = HooksFor(p);
        UserIndex out(0);
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, h, out) ==
              PawnProvision::Refusal::ActorAssetsNotReady);
        CHECK(out.v == kSlotUnmapped);
        CHECK(p.readinessAsks == 1);
        CHECK(p.actorSteps == 0);
        CHECK(p.steppedIndex == kSlotUnmapped);
        CHECK(p.inserts == 1);   // the entrant WAS admitted: only the actor step was withheld
    }

    SUBCASE("readiness true lets the actor step run and the admission completes") {
        AdmitProbe p;
        p.loadouts[1] = 2;
        PawnProvision::AdmitHooks h = HooksFor(p);
        UserIndex out(kSlotUnmapped);
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, h, out) == PawnProvision::Refusal::None);
        CHECK(out.v != kSlotUnmapped);
        CHECK(p.readinessAsks == 1);
        CHECK(p.actorSteps == 1);
        CHECK(p.steppedIndex == out.v);
    }

    SUBCASE("the refusal carries its own name, and LoadoutsEmpty cannot stand in for it") {
        CHECK(StartsWith(PawnProvision::RefusalText(
                             PawnProvision::Refusal::ActorAssetsNotReady),
                         "ActorAssetsNotReady: "));
        CHECK(StartsWith(PawnProvision::RefusalName(
                             PawnProvision::Refusal::ActorAssetsNotReady),
                         "ActorAssetsNotReady"));
        CHECK(!StartsWith(PawnProvision::RefusalName(PawnProvision::Refusal::LoadoutsEmpty),
                          "ActorAssetsNotReady"));
    }

    SUBCASE("a provisioner armed without the predicate is not promoted, and asks nothing") {
        AdmitProbe p;
        p.loadouts[1] = 2;
        PawnProvision::AdmitHooks h = HooksFor(p);
        h.actorAssetsReady = nullptr;
        UserIndex out(0);
        CHECK(PawnProvision::AdmitIntoLobbySlot(&p, 1, h, out) ==
              PawnProvision::Refusal::AdmitNotPromoted);
        CHECK(p.inserts == 0);
        CHECK(p.actorSteps == 0);
    }
}

// ---------------------------------------------------------------------------------------
// P1-19 -- the engine's own route: publish the entrant-added message the netgame registered a
// handler for, and let the ENGINE do the insert, the profile attach and the actor build.

namespace {

// A fake engine that admits an entrant ONLY when the message it was handed names the slot that was
// asked for, at the size the engine itself publishes.  That is what makes the message a subject a
// test can fail on: a route that publishes the wrong slot, or a short payload the real trampoline
// would drop without calling the handler, leaves the entrant table exactly as empty as no publish.
struct ReplyProbe {
    bool     promoted      = true;
    bool     inPhase       = true;
    bool     dispatchTakes = true;
    PawnProvision::Refusal seedAnswer = PawnProvision::Refusal::None;
    bool     buildsActor   = true;

    uint16_t wantSlot   = 0;
    uint32_t seedCalls  = 0;
    uint32_t dispatches = 0;
    uint64_t lastSlotField = 0;
    uint32_t lastBytes     = 0;
    uint8_t  lastPayload[sizeof(PawnProvision::ReplyMsg)] = {0};

    uint16_t engineSlot  = kSlotUnmapped;   // what the engine's entrant table says AFTERWARDS
    uint16_t engineIndex = kSlotUnmapped;
    uint16_t engineCi    = kSlotUnmapped;
};

bool ReplyProbePromoted(void* c) { return static_cast<ReplyProbe*>(c)->promoted; }
bool ReplyProbeInPhase(void* c)  { return static_cast<ReplyProbe*>(c)->inPhase; }

PawnProvision::Refusal ReplyProbeSeed(void* c, uint16_t) {
    ReplyProbe* p = static_cast<ReplyProbe*>(c);
    ++p->seedCalls;
    return p->seedAnswer;
}

bool ReplyProbeDispatch(void* c, const PawnProvision::ReplyMsg& msg, uint32_t bytes) {
    ReplyProbe* p = static_cast<ReplyProbe*>(c);
    ++p->dispatches;
    p->lastSlotField = msg.slot;
    p->lastBytes     = bytes;
    memcpy(p->lastPayload, &msg, sizeof(msg));
    if (!p->dispatchTakes) return false;
    if (msg.slot == uint64_t(p->wantSlot) && bytes == uint32_t(sizeof(PawnProvision::ReplyMsg))) {
        p->engineSlot  = p->wantSlot;
        p->engineIndex = 7;
        p->engineCi    = p->buildsActor ? uint16_t(3) : kSlotUnmapped;
    }
    return true;
}

uint16_t ReplyProbeIndexOfSlot(void* c, uint16_t slot) {
    ReplyProbe* p = static_cast<ReplyProbe*>(c);
    return (p->engineSlot != kSlotUnmapped && slot == p->engineSlot) ? p->engineIndex : kSlotUnmapped;
}
uint16_t ReplyProbeSlotOfIndex(void* c, uint16_t index) {
    ReplyProbe* p = static_cast<ReplyProbe*>(c);
    return (p->engineIndex != kSlotUnmapped && index == p->engineIndex) ? p->engineSlot : kSlotUnmapped;
}
uint16_t ReplyProbeActorCi(void* c, uint16_t index) {
    ReplyProbe* p = static_cast<ReplyProbe*>(c);
    return (p->engineIndex != kSlotUnmapped && index == p->engineIndex) ? p->engineCi : kSlotUnmapped;
}

PawnProvision::ReplyRoute RouteFor(ReplyProbe& p) {
    PawnProvision::ReplyRoute r;
    r.ctx             = &p;
    r.promoted        = ReplyProbePromoted;
    r.inReceivePhase  = ReplyProbeInPhase;
    r.seedLobbyRecord = ReplyProbeSeed;
    r.dispatch        = ReplyProbeDispatch;
    return r;
}

PawnProvision::AdmitHooks ReplyHooksFor(ReplyProbe& p, const PawnProvision::ReplyRoute& r) {
    PawnProvision::AdmitHooks h;
    h.ctx         = &p;
    h.reply       = &r;
    h.indexOfSlot = ReplyProbeIndexOfSlot;
    h.slotOfIndex = ReplyProbeSlotOfIndex;
    h.actorCi     = ReplyProbeActorCi;
    return h;
}

}  // namespace

TEST_CASE("the reply route publishes the engine's own entrant-added message and reads the engine back") {
    int session = 0;
    void* const netGame = &session;
    const uint16_t kAskedSlot = 1;

    SUBCASE("no route wired: not promoted by name, and nothing was published") {
        ReplyProbe p;
        PawnProvision::AdmitHooks h;
        h.ctx = &p;
        h.indexOfSlot = ReplyProbeIndexOfSlot;
        h.slotOfIndex = ReplyProbeSlotOfIndex;
        h.actorCi     = ReplyProbeActorCi;
        CHECK(h.reply == nullptr);
        UserIndex out{uint16_t(0)};
        CHECK(PawnProvision::ProvisionViaReply(netGame, kAskedSlot, h, out) ==
              PawnProvision::Refusal::ReplyRouteNotPromoted);
        CHECK(out.v == kSlotUnmapped);
        CHECK(p.seedCalls == 0);
        CHECK(p.dispatches == 0);
    }

    SUBCASE("route present but unpromoted: still not promoted, and still nothing published") {
        ReplyProbe p;
        p.promoted = false;
        PawnProvision::ReplyRoute r = RouteFor(p);
        PawnProvision::AdmitHooks h = ReplyHooksFor(p, r);
        UserIndex out{uint16_t(0)};
        CHECK(PawnProvision::ProvisionViaReply(netGame, kAskedSlot, h, out) ==
              PawnProvision::Refusal::ReplyRouteNotPromoted);
        CHECK(p.seedCalls == 0);
        CHECK(p.dispatches == 0);
    }

    SUBCASE("wrong phase: refused by name and NOT ONE engine call was made") {
        ReplyProbe p;
        p.wantSlot = kAskedSlot;
        p.inPhase  = false;
        PawnProvision::ReplyRoute r = RouteFor(p);
        PawnProvision::AdmitHooks h = ReplyHooksFor(p, r);
        UserIndex out{uint16_t(0)};
        CHECK(PawnProvision::ProvisionViaReply(netGame, kAskedSlot, h, out) ==
              PawnProvision::Refusal::ReplyWrongPhase);
        CHECK(out.v == kSlotUnmapped);
        CHECK(p.seedCalls == 0);
        CHECK(p.dispatches == 0);
        CHECK(p.engineSlot == kSlotUnmapped);
    }

    SUBCASE("the authored message, byte for byte") {
        ReplyProbe p;
        p.wantSlot = kAskedSlot;
        PawnProvision::ReplyRoute r = RouteFor(p);
        PawnProvision::AdmitHooks h = ReplyHooksFor(p, r);
        UserIndex out{uint16_t(0)};
        CHECK(PawnProvision::ProvisionViaReply(netGame, kAskedSlot, h, out) ==
              PawnProvision::Refusal::None);
        CHECK(out.v == 7);
        CHECK(p.seedCalls == 1);
        CHECK(p.dispatches == 1);
        // The engine's own publish sites write ONE qword and pass MSG.ENTRANT_ADDED_SIZE.
        CHECK(p.lastBytes == uint32_t(facts::MSG_ENTRANT_ADDED_SIZE));
        CHECK(p.lastSlotField == uint64_t(kAskedSlot));
        const uint8_t expected[8] = {0x01, 0, 0, 0, 0, 0, 0, 0};
        CHECK(memcmp(p.lastPayload, expected, sizeof(expected)) == 0);
    }

    SUBCASE("an unmapped slot is published as the engine's own absence, never as slot 65535") {
        const PawnProvision::ReplyMsg none = PawnProvision::AuthorEntrantAddedMsg(kSlotUnmapped);
        CHECK(none.slot == UINT64_MAX);
        CHECK(none.slot != uint64_t(kSlotUnmapped));
        const PawnProvision::ReplyMsg two = PawnProvision::AuthorEntrantAddedMsg(2);
        CHECK(two.slot == 2u);
    }

    SUBCASE("the lobby seed refuses: its own name is returned and nothing is published") {
        ReplyProbe p;
        p.wantSlot   = kAskedSlot;
        p.seedAnswer = PawnProvision::Refusal::ProfileParseFailed;
        PawnProvision::ReplyRoute r = RouteFor(p);
        PawnProvision::AdmitHooks h = ReplyHooksFor(p, r);
        UserIndex out{uint16_t(0)};
        CHECK(PawnProvision::ProvisionViaReply(netGame, kAskedSlot, h, out) ==
              PawnProvision::Refusal::ProfileParseFailed);
        CHECK(p.seedCalls == 1);
        CHECK(p.dispatches == 0);
        CHECK(out.v == kSlotUnmapped);
    }

    SUBCASE("the engine would not take the message: could-not-ask, not a provision") {
        ReplyProbe p;
        p.wantSlot      = kAskedSlot;
        p.dispatchTakes = false;
        PawnProvision::ReplyRoute r = RouteFor(p);
        PawnProvision::AdmitHooks h = ReplyHooksFor(p, r);
        UserIndex out{uint16_t(0)};
        CHECK(PawnProvision::ProvisionViaReply(netGame, kAskedSlot, h, out) ==
              PawnProvision::Refusal::ReplyNotDispatched);
        CHECK(p.dispatches == 1);
        CHECK(out.v == kSlotUnmapped);
    }

    SUBCASE("the publish returned and the engine built no actor: read off the engine, not the publish") {
        ReplyProbe p;
        p.wantSlot    = kAskedSlot;
        p.buildsActor = false;
        PawnProvision::ReplyRoute r = RouteFor(p);
        PawnProvision::AdmitHooks h = ReplyHooksFor(p, r);
        UserIndex out{uint16_t(0)};
        CHECK(PawnProvision::ProvisionViaReply(netGame, kAskedSlot, h, out) ==
              PawnProvision::Refusal::AdmitDidNotCreatePhysics);
        CHECK(p.dispatches == 1);
        CHECK(out.v == kSlotUnmapped);
    }

    SUBCASE("the three names, and no other refusal can stand in for them") {
        using PawnProvision::Refusal;
        CHECK(strcmp(PawnProvision::RefusalName(Refusal::ReplyRouteNotPromoted),
                     "ReplyRouteNotPromoted") == 0);
        CHECK(strcmp(PawnProvision::RefusalName(Refusal::ReplyWrongPhase), "ReplyWrongPhase") == 0);
        CHECK(strcmp(PawnProvision::RefusalName(Refusal::ReplyNotDispatched),
                     "ReplyNotDispatched") == 0);
        for (size_t i = 0; i < PawnProvision::kRefusalCount; ++i) {
            const char* text = PawnProvision::RefusalText(Refusal(i));
            const char* name = PawnProvision::RefusalName(Refusal(i));
            CHECK(strncmp(text, name, strlen(name)) == 0);
            CHECK(text[strlen(name)] == ':');
        }
        CHECK(strcmp(PawnProvision::RefusalName(Refusal::ReplyWrongPhase),
                     PawnProvision::RefusalName(Refusal::ReplyNotDispatched)) != 0);
    }
}

TEST_CASE("the direct insert is the FALLBACK, and only ReplyRouteNotPromoted reaches it") {
    int session = 0;
    void* const netGame = &session;

    SUBCASE("a promoted route owns the verdict: the insert is never called") {
        AdmitProbe direct;
        ReplyProbe p;
        p.wantSlot = 1;
        PawnProvision::ReplyRoute r = RouteFor(p);
        PawnProvision::AdmitHooks h = HooksFor(direct);
        h.reply = &r;
        // The route's postconditions are asked of the route's own engine, not the direct probe's.
        h.ctx         = &p;
        h.indexOfSlot = ReplyProbeIndexOfSlot;
        h.slotOfIndex = ReplyProbeSlotOfIndex;
        h.actorCi     = ReplyProbeActorCi;
        UserIndex out{uint16_t(0)};
        CHECK(PawnProvision::AdmitIntoLobbySlot(netGame, 1, h, out) ==
              PawnProvision::Refusal::None);
        CHECK(out.v == 7);
        CHECK(p.dispatches == 1);
        CHECK(direct.inserts == 0);
        CHECK(direct.actorSteps == 0);
    }
}

// L3 #22's headline, as a test.  `provisioned` must follow the ENGINE's own records and not the
// return value of whichever route asked: the route deferred, the engine built the entrant a tick
// later, and the ledger said `provisioned 0` over a pawn that existed.  A deferral is re-read on
// the next window, never latched -- and the re-read is what counts it, so the insert is not called
// a second time for a slot the engine has already filled.
TEST_CASE("a provision the ENGINE completed out of band is counted from the engine's record") {
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    FakeIdentity id;
    id.refuseProvisionWith = PawnProvision::Refusal::ReplyWrongPhase;
    id.engineCompletesAfterInserts = 1;   // the engine's own pump lands it after our first ask

    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), JoinDesc{}));

    // Tick N: the route defers.  Nothing was published and nothing is counted -- and the slot must
    // NOT latch, because the request is parked, not refused.
    CHECK(p.Pump() == 0);
    CHECK(p.Ledger().provisioned == 0);
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::ReplyWrongPhase);
    CHECK(id.inserts == 1);

    // Tick N+1..: the engine's record now answers, and the next attempt window reads it.
    uint32_t done = 0;
    uint32_t pumps = 0;
    while (done == 0 && pumps < PawnProvisioner::kRetryTicks + 2) {
        done += p.Pump();
        ++pumps;
    }
    CHECK(done == 1);
    CHECK(p.Ledger().provisioned == 1);
    CHECK(p.Ledger().mapped == 1);
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::None);
    // THE DISCRIMINATOR.  The insert was never called again: the pawn was counted off the engine's
    // record, not off a second ask that happened to succeed.  A provisioner keyed on the route's
    // answer cannot pass this line, because this route never answers anything but ReplyWrongPhase.
    CHECK(id.inserts == 1);

    // Idempotent, and it stays counted once.
    CHECK(p.Pump() == 0);
    CHECK(p.Ledger().provisioned == 1);
}

// The guard that keeps the re-read from being an ADOPTION (INV-PP1): a slot nobody asked the engine
// for is not this session's pawn, however full the engine's table is.
TEST_CASE("the engine-record re-read never adopts an entrant this session did not ask for") {
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    FakeIdentity id;
    // An entrant sitting in every live slot before we ever ask.  Nothing here was requested by us.
    for (uint16_t i = 0; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i) {
        id.slots[i].present = true;
        id.slots[i].dressed = true;
        id.slots[i].index = UserIndex(uint16_t(100 + i));
    }
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), JoinDesc{}));
    CHECK(p.Pump() == 0);
    CHECK(p.Ledger().provisioned == 0);
    CHECK(p.Ledger().adopted == 0);
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::SlotAlreadyOccupied);
    CHECK(id.inserts == 0);
}


// ---------------------------------------------------------------------------------------
// P1-22 -- THE ACTOR HOP CHAIN.  L3 #23 read the whole gap in one engine line,
// `Actor : 0x... (disabled)  (pooled)`: the admission landed and the actor is allocated out of the
// pool and never brought up.  These cases drive every hop between that line and a driveable body,
// in the engine's own order, and each one is decided by an ENGINE RECORD read back by name.

namespace {

struct FakeHopEngine {
    bool inCs = false;
    bool enabled = false;
    bool spawned = false;
    bool physCi = false;
    // What the ENGINE's own records do when the two authored steps run.  Separating "the call was
    // made" from "the record moved" is the whole point: the engine's assign is silent when the
    // session type refuses it, and a chain that believed the call would pass a state it never
    // reached.
    bool spawnWorks = true;
    bool assignWorks = true;
    PawnProvision::SpawnedWrite spawnSays = PawnProvision::SpawnedWrite::Issued;
    unsigned reads = 0;      // postcondition reads -- what proves the re-read keeps happening
    unsigned spawnWrites = 0;
    unsigned assigns = 0;
    uint8_t team = 1;        // the LIVE packed role word, which A3 compares the recorded one to
    uint8_t teamSlot = 3;
    bool teamReadable = true;
    unsigned teamReads = 0;
    bool localUser = false;
    // A0's third answer: the netgame's local-user count would not prove readable, so whether this
    // index is the viewer's own is UNKNOWN -- and the arm below it calls the engine.
    bool localUserUnreadable = false;
    uint16_t sawIndex = kSlotUnmapped;
    uint16_t sawRole = kSlotUnmapped;
    // Set by the re-entrancy case: writeSpawned re-enters the chain from inside the engine call.
    PawnProvision::ActorHops* reenter = nullptr;
    PawnProvision::HopDrive* reenterDrive = nullptr;
    PawnProvision::ParticipateOutcome nested{};
    // What each call saw of the other's work, so an ORDER can be asserted rather than assumed.
    bool spawnedWhenAssigned = false;

    // L3 #29b's arm.  The engine's own late-join re-provision, and the readiness predicate that
    // gates it.  `refreshEnables` false models the call returning and the dispatch enabling
    // nothing, which is the only thing separating a drive from a claim.
    bool refreshEnables = true;
    // The re-seat's own two knobs.  `assignEnables` false models the legs reaching the dispatch
    // and the dispatch enabling nothing, which is what separates a drive from a claim on this arm
    // as much as on the refresh's.  `assignFailOnCall` is 1-based and fails exactly one leg.
    bool assignEnables = true;
    unsigned assignFailOnCall = 0;
    unsigned sameTeamEarlyOuts = 0;
    bool assetsReady = true;
    unsigned refreshes = 0;
    unsigned readinessAsks = 0;
    bool spawnedWhenRefreshed = false;

    // THE GATE THE REFRESH ACTUALLY MEETS, modelled as the engine states it.  sub_1401B0D80:86
    // reads the body-template key of the actor the entrant's SActorGUID resolves to and :90-91
    // returns 0 when it already equals the one the entrant's team asks for.  TRUE by default,
    // because that is the state of every late entrant whose actor the engine itself allocated --
    // L3 #31's three refreshes, and the reason they drove nothing.
    bool actorHasTeamKey = true;
    bool refreshUnavailable = false;
    // sub_14019F1C0's SUCCESS-path rewrite of its own return register
    // (`14019F200 mov rax,[rdi+2DA0h] ... movzx eax,cl ... shr al,2`).  With this set the wrapper
    // answers 0 for a call that allocated at :319 and dispatched at :392-395 -- the state that
    // makes the return unusable on its own, and the reason the arm reads the guid instead.
    bool wrapperReturnsZero = false;
    // ENGINE.ACTOR_IS_ENABLED's own call count -- `reads` is shared with three other hops, so a
    // case that wants a particular ENABLE read needs this one -- and the read index from which it
    // starts answering true.  That is how an actor that is ON at the call site but was OFF at the
    // window's entry read gets in front of the arm that would destroy it.
    unsigned enabledReads = 0;
    unsigned enabledFromRead = 0;
    // The entrant's SActorGUID.  Only sub_1401B0D80:319 -- the ALLOCATE -- moves it.
    uint64_t guidLo = 0x1111;
    uint64_t guidHi = 0x2222;
    bool guidReadable = true;
    unsigned guidReads = 0;

    // THE TEAM-SLOT ALLOCATOR, and it is the half of IDENT.SERVER_ASSIGN_TEAM the old fake did not
    // have.  Every assign frees the entrant's live slot onto its OLD team's min-heap and
    // `--count[old]` (sub_1401AB730:198-217), then takes a slot for the new team: when that team's
    // free heap holds one entry or fewer the engine IGNORES it and grabs `count[dest]` (:227-230,
    // `Grabbing last teamslot`), otherwise it pops the minimum (:237-263, `Dequeing teamslot`).
    // A fake that left `teamSlot` alone made the whole hazard untestable.
    static constexpr unsigned kTeams = 5;
    uint8_t teamCount[kTeams] = {1, 0, 0, 0, 0};
    uint8_t freeSlots[kTeams][8] = {};
    uint8_t freeCount[kTeams] = {0, 0, 0, 0, 0};

    void PushFreeSlot(uint8_t team, uint8_t slot) {
        if (team >= kTeams || freeCount[team] >= 8) return;
        freeSlots[team][freeCount[team]++] = slot;
    }
    uint8_t TakeSlot(uint8_t team) {
        if (team >= kTeams) return 0;
        if (freeCount[team] <= 1) return teamCount[team];   // "Grabbing last teamslot"
        unsigned min = 0;
        for (unsigned i = 1; i < freeCount[team]; ++i)
            if (freeSlots[team][i] < freeSlots[team][min]) min = i;
        const uint8_t taken = freeSlots[team][min];
        for (unsigned i = min + 1; i < freeCount[team]; ++i) freeSlots[team][i - 1] = freeSlots[team][i];
        --freeCount[team];
        return taken;                                       // "Dequeing teamslot"
    }
};

FakeHopEngine* HopEng(void* ctx) { return static_cast<FakeHopEngine*>(ctx); }

bool HopInCs(void* c, uint16_t i) { HopEng(c)->reads++; HopEng(c)->sawIndex = i; return HopEng(c)->inCs; }
bool HopEnabled(void* c, uint16_t i) {
    (void)i;
    FakeHopEngine* e = HopEng(c);
    ++e->reads;
    ++e->enabledReads;
    if (e->enabledFromRead != 0 && e->enabledReads >= e->enabledFromRead) return true;
    return e->enabled;
}
bool HopSpawnedRead(void* c, uint16_t i) { (void)i; HopEng(c)->reads++; return HopEng(c)->spawned; }
bool HopPhys(void* c, uint16_t i) { (void)i; HopEng(c)->reads++; return HopEng(c)->physCi; }
PawnProvision::LocalUserAnswer HopIsLocal(void* c, uint16_t i) {
    (void)i;
    FakeHopEngine* e = HopEng(c);
    if (e->localUserUnreadable) return PawnProvision::LocalUserAnswer::Unreadable;
    return e->localUser ? PawnProvision::LocalUserAnswer::Local
                        : PawnProvision::LocalUserAnswer::Remote;
}

PawnProvision::SpawnedWrite HopWriteSpawnedFake(void* c, uint16_t i) {
    (void)i;
    FakeHopEngine* e = HopEng(c);
    // The shipped binding asks the engine's own consumer first and writes nothing when it already
    // answers true.  A fake that wrote anyway would make an already-set record look like an issue.
    if (e->spawned) return PawnProvision::SpawnedWrite::AlreadySet;
    e->spawnWrites++;
    if (e->reenter != nullptr && e->reenterDrive != nullptr) {
        // A hook of ours firing inside the engine call, which is exactly what S14.6's latch is
        // for: the whole participating arm runs synchronously from A3.
        e->nested = PawnProvision::Participate(*e->reenter, i, *e->reenterDrive);
    }
    if (e->spawnSays == PawnProvision::SpawnedWrite::Issued && e->spawnWorks) e->spawned = true;
    return e->spawnSays;
}

bool HopAssignTeamFake(void* c, uint16_t i, uint16_t role) {
    (void)i;
    FakeHopEngine* e = HopEng(c);
    e->assigns++;
    e->sawRole = role;
    e->spawnedWhenAssigned = e->spawned;
    if (e->assignFailOnCall != 0 && e->assigns == e->assignFailOnCall) return false;
    // The engine's THREE nested same-team early-outs -- sub_1401AB730:73 wraps the whole body,
    // :186 prints `ServerAssignTeam: attempting to assign to same team - early out`, and
    // sub_1401B1FD0:31 tests it again.  A request that already equals the packed word drives
    // NOTHING, and a fake that moved anyway would make the arm's whole hazard untestable.
    if (uint16_t(e->team) == role) {
        e->sameTeamEarlyOuts++;
        return true;
    }
    if (e->assignWorks) {
        // The engine's own participating arm: the word takes the role, and the enable it performs
        // is the engine's, not ours -- through `if ( USER_SPAWNED_GET(ng, idx) ) ENABLE(...)` at
        // sub_1401C9BA0:334-336, so an entrant whose record is still 0 gets the dispatch and no
        // enable.  The SLOT moves too, and it does not come back on its own.
        const uint8_t old = e->team;
        e->PushFreeSlot(old, e->teamSlot);
        if (old < FakeHopEngine::kTeams && e->teamCount[old] > 0) --e->teamCount[old];
        e->team = uint8_t(role);
        e->teamSlot = e->TakeSlot(uint8_t(role));
        if (role < FakeHopEngine::kTeams) ++e->teamCount[role];
        if (e->assignEnables && e->spawned) e->enabled = true;
    }
    return true;
}

bool HopTeamOf(void* c, uint16_t i, uint8_t* team, uint8_t* teamSlot) {
    (void)i;
    FakeHopEngine* e = HopEng(c);
    ++e->teamReads;
    if (!e->teamReadable) return false;
    *team = e->team;
    *teamSlot = e->teamSlot;
    return true;
}

// Hop 4's WHY, off the same fake record `HopPhys` reads, so the two can never disagree here for
// the same reason the shipped pair cannot: one reading answers both.
PawnProvision::PhysicsCiReq HopPhysAwaiting(void* c, uint16_t) {
    return static_cast<FakeHopEngine*>(c)->physCi ? PawnProvision::PhysicsCiReq::None
                                                  : PawnProvision::PhysicsCiReq::R15NetPhysics;
}

PawnProvision::RefreshAnswer HopRefreshFake(void* c, uint16_t i) {
    using PawnProvision::RefreshAnswer;
    (void)i;
    FakeHopEngine* e = HopEng(c);
    ++e->refreshes;
    e->spawnedWhenRefreshed = e->spawned;
    if (e->refreshUnavailable) return RefreshAnswer::Unavailable;
    // sub_1401B0D80:90-91.  The actor already carries the body template its team asks for, so the
    // function returns 0 having allocated nothing and dispatched nothing.  Everything below --
    // the allocate at :319 and the participating dispatch at :392-395 -- is unreachable from here.
    if (e->actorHasTeamKey) return RefreshAnswer::Declined;
    // :319, the ALLOCATE: a fresh SActorGUID, and only then the dispatch.  Its enable site is
    // `if (USER_SPAWNED_GET(ng,idx)) ENABLE(...)`, so an entrant whose `spawned` record is still 0
    // gets the call and no enable -- which is exactly what happened to slot 1 twice.
    e->actorHasTeamKey = true;
    ++e->guidLo;
    if (e->refreshEnables && e->spawned) e->enabled = true;
    // The engine did all of it AND the wrapper still hands back 0.  Nothing above changes: this
    // models the return register being overwritten on the way out, not a different engine path.
    if (e->wrapperReturnsZero) return RefreshAnswer::Declined;
    return RefreshAnswer::Redispatched;
}

bool HopActorGuidFake(void* c, uint16_t i, uint64_t* lo, uint64_t* hi) {
    (void)i;
    FakeHopEngine* e = HopEng(c);
    ++e->guidReads;
    if (!e->guidReadable) return false;
    *lo = e->guidLo;
    *hi = e->guidHi;
    return true;
}

bool HopAssetsReadyFake(void* c, uint16_t i) {
    (void)i;
    FakeHopEngine* e = HopEng(c);
    ++e->readinessAsks;
    return e->assetsReady;
}

PawnProvision::ActorHops HopsFor(FakeHopEngine& e) {
    PawnProvision::ActorHops h;
    h.ctx = &e;
    h.refreshPlayerActor = HopRefreshFake;
    h.actorAssetsReady = HopAssetsReadyFake;
    h.actorGuid = HopActorGuidFake;
    h.inRemotePlayerCs = HopInCs;
    h.enabled = HopEnabled;
    h.placed = HopSpawnedRead;
    h.physicsCi = HopPhys;
    h.physicsCiAwaiting = HopPhysAwaiting;
    h.isLocalUser = HopIsLocal;
    h.writeSpawned = HopWriteSpawnedFake;
    h.assignTeam = HopAssignTeamFake;
    h.teamOf = HopTeamOf;
    return h;
}

// The chain drives at most one transition per epoch per slot, so a case that wants a second one
// opens a new epoch rather than reaching past the gate.
// `entrantCount` defaults to the one population in which the role bounce round-trips the Team
// Slot -- a single recorded player.  Every case that wants the 4v4 population says so.
PawnProvision::HopDrive DriveFor(PawnProvision::DriveGate& g, uint16_t recordedRole = 0,
                                uint16_t entrantCount = 1) {
    PawnProvision::HopDrive d;
    d.gate = &g;
    d.recordedRole = recordedRole;
    d.entrantCount = entrantCount;
    return d;
}

}  // namespace

TEST_CASE("every hop is answered by the engine's own record, by name, in the engine's order") {
    FakeHopEngine e;
    PawnProvision::ActorHops h = HopsFor(e);
    // Fresh per SUBCASE, because doctest re-runs the case body for each one: a gate carried across
    // subcases would latch in one and silently suppress the transition in the next.
    PawnProvision::DriveGate g;

    SUBCASE("an index nobody issued is before the start of the chain, not at the end of it") {
        PawnProvision::HopDrive d = DriveFor(g);
        CHECK(PawnProvision::AdvanceActorHops(h, kSlotUnmapped, &d) ==
              PawnProvision::Hop::RemotePlayerCs);
        CHECK(e.spawnWrites == 0);
        CHECK(e.assigns == 0);
    }

    SUBCASE("hop 1 is waited for, never driven -- the engine registers the actor itself") {
        PawnProvision::HopDrive d = DriveFor(g);
        CHECK(PawnProvision::AdvanceActorHops(h, 7, &d) == PawnProvision::Hop::RemotePlayerCs);
        CHECK(e.sawIndex == 7);
        // Nothing is authored for an actor no component system has been told about.
        CHECK(e.spawnWrites == 0);
        CHECK(e.assigns == 0);
        CHECK(PawnProvision::HopRefusal(PawnProvision::Hop::RemotePlayerCs) ==
              PawnProvision::Refusal::NotInRemotePlayerCs);
    }

    SUBCASE("the four hops come out in the engine's own order and each names its own refusal") {
        e.inCs = true;
        PawnProvision::HopDrive d = DriveFor(g);
        // The participation is authored, the ENGINE enables and the `spawned` record reads back,
        // so one call moves past Enabled and past Placed and stops at the hop nothing may drive.
        CHECK(PawnProvision::AdvanceActorHops(h, 7, &d) == PawnProvision::Hop::PhysicsCi);
        CHECK(d.outcome.state == PawnProvision::Participation::Issued);
        CHECK(e.spawnWrites == 1);
        CHECK(e.assigns == 1);
        CHECK(e.enabled);
        CHECK(e.spawned);
        CHECK(PawnProvision::HopRefusal(PawnProvision::Hop::PhysicsCi) ==
              PawnProvision::Refusal::NoPhysicsCi);

        e.physCi = true;
        CHECK(PawnProvision::AdvanceActorHops(h, 7, &d) == PawnProvision::Hop::Done);
        CHECK(PawnProvision::HopRefusal(PawnProvision::Hop::Done) ==
              PawnProvision::Refusal::None);
    }

    SUBCASE("★ the participation is decided by the engine's record, never by the call's answer") {
        e.inCs = true;
        e.assignWorks = false;   // the call is made and the engine's word does not move
        PawnProvision::HopDrive d = DriveFor(g);
        CHECK(PawnProvision::AdvanceActorHops(h, 7, &d) == PawnProvision::Hop::Enabled);
        CHECK(e.assigns == 1);
        CHECK(d.outcome.state == PawnProvision::Participation::Refused);
        CHECK(d.outcome.why == PawnProvision::ParticipateRefusal::AssignTeamNotApplied);
        CHECK_FALSE(e.enabled);
        // The latch is not spent by a refusal: the epoch stays open for a pass that can take.
        CHECK(g.issuedEpoch != g.epoch);
        CHECK(g.issues == 0);
    }

    SUBCASE("an unwired postcondition is could-not-ask under its own hop's name, never a pass") {
        e.inCs = true; e.enabled = true; e.spawned = true; e.physCi = true; e.team = 0;
        PawnProvision::HopDrive d = DriveFor(g);
        CHECK(PawnProvision::AdvanceActorHops(h, 7, &d) == PawnProvision::Hop::Done);

        PawnProvision::ActorHops n = h;
        n.inRemotePlayerCs = nullptr;
        CHECK(PawnProvision::AdvanceActorHops(n, 7, &d) == PawnProvision::Hop::RemotePlayerCs);
        n = h; n.enabled = nullptr;
        CHECK(PawnProvision::AdvanceActorHops(n, 7, &d) == PawnProvision::Hop::Enabled);
        n = h; n.placed = nullptr;
        CHECK(PawnProvision::AdvanceActorHops(n, 7, &d) == PawnProvision::Hop::Placed);
        n = h; n.physicsCi = nullptr;
        CHECK(PawnProvision::AdvanceActorHops(n, 7, &d) == PawnProvision::Hop::PhysicsCi);
    }

    SUBCASE("every hop's name and refusal text are its own, and no name is a prefix of another") {
        const char* names[PawnProvision::kHopCount];
        for (size_t i = 0; i < PawnProvision::kHopCount; ++i) {
            names[i] = PawnProvision::HopName(PawnProvision::Hop(i));
            CHECK(names[i][0] != 0);
        }
        for (size_t i = 0; i < PawnProvision::kHopCount; ++i)
            for (size_t j = 0; j < PawnProvision::kHopCount; ++j)
                if (i != j) CHECK(strcmp(names[i], names[j]) != 0);
        for (size_t i = 0; i + 1 < PawnProvision::kHopCount; ++i) {
            const PawnProvision::Refusal r =
                PawnProvision::HopRefusal(PawnProvision::Hop(i));
            const char* name = PawnProvision::RefusalName(r);
            const char* text = PawnProvision::RefusalText(r);
            CHECK(strncmp(text, name, strlen(name)) == 0);
            CHECK(text[strlen(name)] == ':');
            CHECK(strlen(text) < 400u);  // facts-ok: AUDIO.LISTENER_COUNT_OFF a character-length bound on the refusal text, not the listener-count offset
        }
    }
}

// ★ THE HOP A SKIPPED PACKET CANNOT HIDE.  `provisioned` counts DRIVEABLE pawns: an admission
// whose actor is still disabled is an entrant, and L3 #23 counted one of those as nothing at all
// because the ledger had no word for it.  Here it is Awaiting, and it is Awaiting by NAME.
TEST_CASE("an admitted entrant is not provisioned until every actor hop reads done") {
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    FakeIdentity id;
    FakeHopEngine e;
    PawnProvision::ActorHops h = HopsFor(e);

    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.hops = &h;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), JoinDesc{}));

    CHECK(p.Pump() == 0);
    CHECK(id.inserts == 1);
    CHECK(p.Ledger().provisioned == 0);
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::NotInRemotePlayerCs);
    CHECK_FALSE(p.Provisioned(UserSlot(0)));
    CHECK(p.Ledger().failed == 0);

    // The engine registers the actor.  The next cadence window authors the participation and the
    // engine's own arm enables; the chain stops at the body, the one hop nothing here may call.
    e.inCs = true;
    uint32_t pumps = 0;
    while (p.LastProvisionRefusal() != PawnProvision::Refusal::NoPhysicsCi &&
           pumps < PawnProvisioner::kRetryTicks + 4) {
        p.Pump();
        ++pumps;
    }
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::NoPhysicsCi);
    CHECK(e.spawnWrites >= 1);
    CHECK(e.assigns >= 1);
    CHECK(p.Ledger().provisioned == 0);
    CHECK(id.inserts == 1);

    e.physCi = true;
    uint32_t done = 0;
    pumps = 0;
    while (done == 0 && pumps < PawnProvisioner::kRetryTicks + 4) {
        done += p.Pump();
        ++pumps;
    }
    CHECK(done == 1);
    CHECK(p.Ledger().provisioned == 1);
    CHECK(p.Ledger().participateIssued == 1);
    CHECK(p.Ledger().adopted == 0);
    CHECK(p.Provisioned(UserSlot(0)));
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::None);
    CHECK(id.inserts == 1);
}

// ★ L3 #23 F3/F4, AS A TEST.  A slot awaiting a hop must be re-read for the LIFE of the session at
// a bounded cadence -- there is no window count at which it stops looking.
TEST_CASE("an awaiting slot is re-read past the edge for the life of the session, never latched") {
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    FakeIdentity id;
    FakeHopEngine e;
    e.inCs = true;                  // admitted and registered; only the body is outstanding
    PawnProvision::ActorHops h = HopsFor(e);

    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.hops = &h;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), JoinDesc{}));
    CHECK(p.Pump() == 0);

    const uint32_t kPeriods = 12;
    unsigned readsAtStart = e.reads;
    unsigned windows = 0;
    unsigned lastReads = e.reads;
    for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks * kPeriods; ++i) {
        p.Pump();
        if (e.reads != lastReads) {
            ++windows;
            lastReads = e.reads;
        }
    }
    CHECK(e.reads > readsAtStart);
    CHECK(windows >= kPeriods - 1);
    CHECK(windows <= kPeriods + 1);
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::NoPhysicsCi);
    CHECK(p.Ledger().provisioned == 0);
    CHECK(p.Ledger().failed == 0);
    CHECK(p.Ledger().refusals[size_t(PawnProvision::Refusal::NoPhysicsCi)] >= kPeriods - 1);
    CHECK(p.Ledger().deferred == 0);
    // ★ And the transition was authored ONCE across all of those windows, not once per window.
    CHECK(e.assigns == 1);
    CHECK(e.spawnWrites == 1);

    e.physCi = true;
    uint32_t done = 0;
    for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks + 4 && done == 0; ++i) done += p.Pump();
    CHECK(done == 1);
    CHECK(p.Ledger().provisioned == 1);
    CHECK(id.inserts == 1);
}

TEST_CASE("no actor-hop surface is could-not-ask, and settles at the admission as it always did") {
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    FakeIdentity id;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    CHECK(env.hops == nullptr);
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), JoinDesc{}));
    CHECK(p.Pump() == 1);
    CHECK(p.Ledger().provisioned == 1);
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::None);
}

TEST_CASE("Arm takes the hop surface from the identity when the env carries none") {
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    FakeHopEngine fromIdentity;
    PawnProvision::ActorHops h = HopsFor(fromIdentity);
    FakeIdentity id;
    id.hopSurface = &h;

    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    REQUIRE(env.hops == nullptr);
    const uint16_t admittedIndex = id.nextIndex;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), JoinDesc{}));

    CHECK(p.Pump() == 0);
    CHECK(fromIdentity.reads > 0);
    CHECK(fromIdentity.sawIndex == admittedIndex);
    CHECK(p.Ledger().provisioned == 0);
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::NotInRemotePlayerCs);
}

TEST_CASE("an env that carries its own hop surface keeps it") {
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    FakeHopEngine fromIdentity;
    FakeHopEngine handedIn;
    PawnProvision::ActorHops fromId = HopsFor(fromIdentity);
    PawnProvision::ActorHops own = HopsFor(handedIn);
    FakeIdentity id;
    id.hopSurface = &fromId;

    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.hops = &own;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), JoinDesc{}));

    CHECK(p.Pump() == 0);
    CHECK(handedIn.reads > 0);
    CHECK(fromIdentity.reads == 0);
}

TEST_CASE("the identity interface refuses a team read by default and leaves the out params alone") {
    FakeIdentity id;
    IIdentity& iface = id;
    uint8_t teamId = 0xAB;
    uint8_t teamSlot = 0xCD;
    CHECK_FALSE(iface.TeamOf(UserIndex(0), teamId, teamSlot));
    CHECK(teamId == 0xAB);
    CHECK(teamSlot == 0xCD);
}

// ===========================================================================================
// ARCH-23 S14.7's acceptance -- every case below either requires an ISSUE or asserts a NAMED
// refusal, and the last two make "shipped with the driver gated off" a RED state.
// ===========================================================================================

// S14.7 test 5.
TEST_CASE("W2-P1-29: participate is ordered -- the spawned record before the team, or not at all") {
    FakeHopEngine e;
    PawnProvision::ActorHops h = HopsFor(e);
    e.inCs = true;

    SUBCASE("the record is written first, and the engine's call sees it") {
        PawnProvision::DriveGate g;
        PawnProvision::HopDrive d = DriveFor(g, 0);
        CHECK(PawnProvision::Participate(h, 7, d).state ==
              PawnProvision::Participation::Issued);
        CHECK(e.spawnWrites == 1);
        CHECK(e.assigns == 1);
        CHECK(e.spawnedWhenAssigned);
    }

    SUBCASE("the team is NOT assigned when the spawned re-read still answers 0") {
        PawnProvision::DriveGate g;
        PawnProvision::HopDrive d = DriveFor(g, 0);
        e.spawnWorks = false;      // the setter runs and the consumer's own read does not move
        const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(h, 7, d);
        CHECK(o.state == PawnProvision::Participation::Refused);
        CHECK(o.why == PawnProvision::ParticipateRefusal::SpawnedRecordNotWritten);
        CHECK(e.spawnWrites == 1);
        CHECK(e.assigns == 0);
        CHECK(g.issues == 0);
    }
}

// S14.7 test 6 -- a recorded spectator stays a spectator, and every hole refuses by name.
TEST_CASE("W2-P1-29: a recorded spectator refuses by name, and nothing at all is written") {
    FakeHopEngine e;
    PawnProvision::ActorHops h = HopsFor(e);
    e.inCs = true;

    struct Row { uint16_t role; PawnProvision::ParticipateRefusal why; };
    const Row rows[] = {
        {2, PawnProvision::ParticipateRefusal::RecordedAsSpectator},
        {4, PawnProvision::ParticipateRefusal::RecordedAsSpectator},
        {PawnProvision::kRoleNotRecorded, PawnProvision::ParticipateRefusal::TeamNotRecorded},
        {6, PawnProvision::ParticipateRefusal::TeamOutOfRange},
    };
    for (const Row& r : rows) {
        PawnProvision::DriveGate g;
        PawnProvision::HopDrive d = DriveFor(g, r.role);
        const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(h, 7, d);
        CHECK(o.state == PawnProvision::Participation::Refused);
        CHECK(o.why == r.why);
    }
    // Not one engine call was made for any of them, and no record was authored.
    CHECK(e.spawnWrites == 0);
    CHECK(e.assigns == 0);
    CHECK_FALSE(e.spawned);

    // The three refusals are three different names, and the roles the engine DOES take are taken.
    CHECK(strcmp(PawnProvision::ParticipateRefusalName(
                     PawnProvision::ParticipateRefusal::RecordedAsSpectator),
                 "RecordedAsSpectator") == 0);
    for (uint16_t role : {uint16_t(0), uint16_t(1), uint16_t(3)}) {
        FakeHopEngine ok;
        ok.inCs = true;
        ok.team = 5;   // deliberately unequal to every role under test
        PawnProvision::ActorHops oh = HopsFor(ok);
        PawnProvision::DriveGate g;
        PawnProvision::HopDrive d = DriveFor(g, role);
        CHECK(PawnProvision::Participate(oh, 7, d).state ==
              PawnProvision::Participation::Issued);
        CHECK(ok.sawRole == role);
    }
}

// S14.7 test 7, rewritten by L3 #29b.  A word that already matches still means A3 is a no-op --
// that part held -- but "the engine has nothing left to do" was NEVER the same statement, and this
// packet's whole defect was reporting the second off the first.
TEST_CASE("W2-P1-32: a word that already matches is a SKIP only when the actor is already ON") {
    SUBCASE("actor already enabled: nothing is driven and nothing is claimed") {
        FakeHopEngine e;
        PawnProvision::ActorHops h = HopsFor(e);
        e.inCs = true;
        e.team = 1;        // the live word already reads the recorded role
        e.spawned = true;  // and the engine's own record is already there
        e.enabled = true;  // and the actor the whole chain exists to turn on is on
        PawnProvision::DriveGate g;
        PawnProvision::HopDrive d = DriveFor(g, 1);

        const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(h, 7, d);
        CHECK(o.state == PawnProvision::Participation::SkippedAlreadyDone);
        CHECK(o.why == PawnProvision::ParticipateRefusal::None);
        // The engine's own early-out would have made the assign a no-op, and the refresh has
        // nothing to re-run, so NEITHER call is made.
        CHECK(e.assigns == 0);
        CHECK(e.refreshes == 0);
        CHECK(e.spawnWrites == 0);
        CHECK(g.issues == 0);
        CHECK(g.via == PawnProvision::ParticipateDrive::None);
    }

    SUBCASE("actor still disabled: L3 #29b's state, and it is a DRIVE, not a skip") {
        // L3 #31 measured the refresh declining this exact state 276 times, so the fake does
        // too: whatever turns the actor on here, it is not NETGAME.REFRESH_PLAYER_ACTOR.
        FakeHopEngine e;
        PawnProvision::ActorHops h = HopsFor(e);
        e.inCs = true;
        e.team = 1;
        e.spawned = true;   // written on an EARLIER pump, so the engine can read it inside the call
        e.enabled = false;  // `Actor : 0x... (disabled)  (pooled)` -- 276 hop attempts of it
        e.teamSlot = 0;
        e.teamCount[0] = 0;
        e.teamCount[1] = 1;
        e.refreshEnables = false;   // whatever turns this actor on, the refresh's dispatch did not
        PawnProvision::DriveGate g;
        PawnProvision::HopDrive d = DriveFor(g, 1);

        // PUMP 1 -- the refresh goes out and the engine DECLINES it, at :91, by key.  Nothing has
        // been allocated and nothing the recording owns has moved -- but the decline is what admits
        // the re-seat next pump.
        const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(h, 7, d);
        CHECK(o.state == PawnProvision::Participation::Refused);
        CHECK(o.why == PawnProvision::ParticipateRefusal::RefreshDeclinedKeyUnchanged);
        CHECK(e.refreshes == 1);
        CHECK(e.assigns == 0);
        CHECK(g.issues == 0);

        // PUMP 2 -- the decline admits the re-seat, so the bounce goes out here.  Its verdict is
        // read on a LATER pump.
        PawnProvision::HopDrive d2 = DriveFor(g, 1);
        CHECK(PawnProvision::Participate(h, 7, d2).why ==
              PawnProvision::ParticipateRefusal::ReSeatAwaitingReRead);
        CHECK(e.assigns == 2);            // away and back, one tick, no engine update between
        CHECK(e.sameTeamEarlyOuts == 0);  // neither leg asked for the role the word already held
        CHECK(e.team == 1);               // and the entrant is back on the recorded role
        CHECK(e.teamSlot == 0);           // on the slot it started on, which is the other half
        CHECK(g.reSeatIssues == 1);
        CHECK(g.issues == 0);

        // PUMP 3 -- the postcondition, and it is what books the issue.
        PawnProvision::HopDrive d3 = DriveFor(g, 1);
        const PawnProvision::ParticipateOutcome o3 = PawnProvision::Participate(h, 7, d3);
        CHECK(o3.state == PawnProvision::Participation::Issued);
        CHECK(o3.why == PawnProvision::ParticipateRefusal::None);
        CHECK(e.assigns == 2);          // nothing was driven twice
        CHECK(e.enabled);               // the postcondition the outcome was decided on
        CHECK(g.issues == 1);
        CHECK(g.via == PawnProvision::ParticipateDrive::AssignTeamReSeat);
        CHECK(strcmp(PawnProvision::ParticipateDriveName(g.via),
                     "roleAlreadyMatched:assignTeam") == 0);
    }
}

// DIAG-PARTICIPATION-REGRESSION / PRIORITY 0.  THE RE-SEAT'S ENABLE IS ASYNC, so its enable lands
// a WINDOW OR MORE after the bounce, not in the window that reads the pending flag.  The pre-fix
// code consumed reSeatPending in the first window it read the actor still off, before the async
// enable could coincide with it -- so a re-seat that DID enable was never booked, participate.state
// never reached Issued, and the slot churned on Enabled:refused / ParticipationNotIssued forever.
// The latch HOLDS the pending flag across a bounded settle budget; the window the enable finally
// lands in still sees it set, and the re-seat books Issued.  Pins RESEAT_PENDING_CLEARED_EARLY.
TEST_CASE("W2-P1-47: a re-seat whose enable lands asynchronously is still booked Issued") {
    FakeHopEngine e;
    PawnProvision::ActorHops h = HopsFor(e);
    e.inCs = true;
    e.team = 1;
    e.spawned = true;          // written on an earlier pump, so the engine reads it inside the call
    e.enabled = false;         // (disabled) (pooled) -- the async enable has not landed yet
    e.teamSlot = 0;
    e.teamCount[0] = 0;
    e.teamCount[1] = 1;
    e.refreshEnables = false;  // the refresh declines, admitting the re-seat
    e.assignEnables = false;   // THE POINT: the legs reach the dispatch but DO NOT enable this window
    PawnProvision::DriveGate g;

    // PUMP 1 -- the refresh declines by key, admitting the bounce next window.
    PawnProvision::HopDrive d1 = DriveFor(g, 1);
    CHECK(PawnProvision::Participate(h, 7, d1).why ==
          PawnProvision::ParticipateRefusal::RefreshDeclinedKeyUnchanged);

    // PUMP 2 -- the bounce goes out; its enable postcondition is read on a LATER pump.  assignEnables
    // is false, so the actor is still off when this returns -- the real async gap.
    PawnProvision::HopDrive d2 = DriveFor(g, 1);
    CHECK(PawnProvision::Participate(h, 7, d2).why ==
          PawnProvision::ParticipateRefusal::ReSeatAwaitingReRead);
    CHECK(g.reSeatIssues == 1);
    CHECK(g.reSeatPending);
    CHECK(e.assigns == 2);     // away and back, one tick
    CHECK_FALSE(e.enabled);

    // PUMPS 3-4 -- the enable STILL has not landed.  The latch HOLDS reSeatPending across the settle
    // budget and drives NOTHING more; the pre-fix code dropped the flag in the first of these.
    for (int w = 0; w < 2; ++w) {
        PawnProvision::HopDrive d = DriveFor(g, 1);
        CHECK(PawnProvision::Participate(h, 7, d).why ==
              PawnProvision::ParticipateRefusal::ReSeatAwaitingReRead);
        CHECK(g.reSeatPending);          // NOT consumed early
        CHECK(e.assigns == 2);           // no re-drive while settling
    }
    CHECK(g.reSeatSettle == 2);

    // THE ASYNC ENABLE LANDS, a few windows after the bounce (the engine dump always shows it does).
    e.enabled = true;

    // PUMP 5 -- the pending flag is STILL set, so the coincidence fires and the re-seat books Issued.
    // Under RESEAT_PENDING_CLEARED_EARLY the flag was dropped at pump 3 and this reads
    // SkippedAlreadyDone instead -- the race the diagnostic named.
    PawnProvision::HopDrive d5 = DriveFor(g, 1);
    const PawnProvision::ParticipateOutcome o5 = PawnProvision::Participate(h, 7, d5);
    CHECK(o5.state == PawnProvision::Participation::Issued);
    CHECK(o5.why == PawnProvision::ParticipateRefusal::None);
    CHECK(g.issues == 1);
    CHECK(g.via == PawnProvision::ParticipateDrive::AssignTeamReSeat);
    CHECK(e.assigns == 2);               // Issued off the enable RE-READ, not a fresh drive
    CHECK_FALSE(g.reSeatPending);        // the latch is cleared on success
    CHECK(g.reSeatSettle == 0);
}

// PRIORITY 0, the OTHER end of the latch: an enable that NEVER lands does not spin forever and does
// not drop silently -- past the settle budget it gives up with a NAMED refusal.
TEST_CASE("W2-P1-47: a re-seat whose enable never lands is bounded and named ReSeatSettleExhausted") {
    FakeHopEngine e;
    PawnProvision::ActorHops h = HopsFor(e);
    e.inCs = true;
    e.team = 1;
    e.spawned = true;
    e.enabled = false;
    e.teamSlot = 0;
    e.teamCount[0] = 0;
    e.teamCount[1] = 1;
    e.refreshEnables = false;
    e.assignEnables = false;   // the enable never lands, this whole case
    PawnProvision::DriveGate g;

    PawnProvision::HopDrive dr1 = DriveFor(g, 1);
    CHECK(PawnProvision::Participate(h, 7, dr1).why ==
          PawnProvision::ParticipateRefusal::RefreshDeclinedKeyUnchanged);        // pump 1
    PawnProvision::HopDrive dr2 = DriveFor(g, 1);
    CHECK(PawnProvision::Participate(h, 7, dr2).why ==
          PawnProvision::ParticipateRefusal::ReSeatAwaitingReRead);               // pump 2: bounce

    // Every settle window defers by name while the budget is unspent, then the give-up is named.
    PawnProvision::ParticipateRefusal last = PawnProvision::ParticipateRefusal::None;
    for (uint16_t w = 0; w < PawnProvision::kReSeatSettleWindows + 1; ++w) {
        PawnProvision::HopDrive dw = DriveFor(g, 1);
        last = PawnProvision::Participate(h, 7, dw).why;
    }
    CHECK(last == PawnProvision::ParticipateRefusal::ReSeatSettleExhausted);
    CHECK_FALSE(g.reSeatPending);        // cleared on exhaustion, not left latched
    CHECK(g.issues == 0);                // and never booked as Issued
}

// DIAG-PARTICIPATION-FLAKINESS, AT THE PROVISION GATE.  The bounce's async enable can land INSIDE
// the same AdvanceActorHops call: Participate reads enabled#1 off at the top, reSeatBack's dispatch
// enables the actor, and hop 2's own re-read -- one step later, same call -- reads it on, so the
// chain reaches hop==Done while participate.state is still Refused(ReSeatAwaitingReRead) and the
// Issued booking is deferred to the next window (the P1-47 latch).  The shipped gate DEFERS
// (ParticipationSettling), keeping the slot Awaiting so that next window's enabled#1 books Issued via
// the re-seat.  The pre-fix gate raised ParticipationNotIssued, which latches the slot Failed between
// the bounce window and the booking -- Pump then skips it forever, so it never provisions and no
// named outcome ever prints.  Pins PARTNOTISSUED_LATCHES_UNDER_PENDING.
TEST_CASE("W2-P1-52: a re-seat whose enable lands mid-chain defers, then books Issued -- never Failed") {
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    FakeIdentity id;
    FakeHopEngine e;
    e.inCs    = true;
    e.team    = 1;             // the LIVE role -- matched to the recorded one below -> the re-seat arm
    e.teamSlot = 0;            // sole member of team 1: the bounce round-trips its own slot (N==1)
    e.teamCount[0] = 0;
    e.teamCount[1] = 1;
    e.spawned = true;          // A2 already ran, so the engine reads it inside the call
    e.enabled = false;         // (disabled) (pooled) -- off when Participate reads enabled#1 at the top
    e.physCi  = true;          // the last hop is ready, so hop==Done the instant the enable lands
    e.refreshEnables = false;  // the refresh declines by key, admitting the bounce next window
    e.assignEnables  = true;   // THE RACE: reSeatBack's dispatch enables the actor DURING the call,
                               // so hop 2's re-read reads it on in the SAME chain the bounce ran in
    PawnProvision::ActorHops h = HopsFor(e);

    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.hops = &h;
    p.Arm(env);
    JoinDesc desc{};
    desc.team = 1;             // RECORDED role == live role: the roleAlreadyMatched (re-seat) arm
    REQUIRE(p.Request(UserSlot(0), desc));

    // Pump window by window until the slot provisions, watching for the two gate outcomes that tell
    // the fix from the defect.
    bool sawSettling  = false;
    bool sawNotIssued = false;
    uint32_t done = 0;
    for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks * 10 && done == 0; ++i) {
        done += p.Pump();
        const PawnProvision::Refusal why = p.LastProvisionRefusal();
        if (why == PawnProvision::Refusal::ParticipationSettling)  sawSettling  = true;
        if (why == PawnProvision::Refusal::ParticipationNotIssued) sawNotIssued = true;
    }

    CHECK(sawSettling);                    // the non-latching deferral fired on the mid-chain window
    CHECK_FALSE(sawNotIssued);             // and the Failed-latch never did
    CHECK(done == 1);                      // the slot provisioned on the next window's booking
    CHECK(p.Ledger().provisioned == 1);
    CHECK(p.Ledger().failed == 0);         // never latched off
    CHECK(p.Ledger().participateIssued == 1);
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::None);
}

// L3 #29b -- THE FIX, driven over the state the launch actually produced.  The engine's own
// `Server Assigning Team ... : 5` auto-pick had already written team 0 into user index 1 when our
// `spawned` write landed a second later, so the participating dispatch had run twice against a
// `spawned` of 0 and A3 could never re-run it.
TEST_CASE("W2-P1-34: the engine pre-set the role -- the refresh NAMES the gate, and nothing frees past it") {
    FakeHopEngine e;
    PawnProvision::ActorHops h = HopsFor(e);
    e.inCs = true;
    e.team = 0;         // the ENGINE's auto-pick, not ours
    e.teamSlot = 0;     // a multi-entrant load (2): the bounce is refused for it
    e.spawned = false;  // our A2 has not run yet: this pump is the one that writes it
    e.enabled = false;
    // The state L3 #31 was in and the default here: the actor the engine allocated at admission
    // already carries the body template team 0 asks for, so the refresh alone can only decline.
    PawnProvision::DriveGate g;

    // PUMP 1 -- A2 writes the record.  The engine reads that record from INSIDE the refresh, so
    // the refresh is NOT issued on the pump that authored it: a deferral, by name, and the gate
    // stays unlatched so the next window asks again.
    PawnProvision::HopDrive p1 = DriveFor(g, 0, 2);
    const PawnProvision::ParticipateOutcome o1 = PawnProvision::Participate(h, 7, p1);
    CHECK(o1.state == PawnProvision::Participation::Refused);
    CHECK(o1.why == PawnProvision::ParticipateRefusal::RefreshDeferredToNextPump);
    CHECK(e.spawnWrites == 1);
    CHECK(e.spawned);
    CHECK(e.refreshes == 0);       // THE ORDERING, and it is the assertion the mutant breaks
    CHECK(e.readinessAsks == 0);   // no call was contemplated, so its guard was not spent
    CHECK(g.issues == 0);
    CHECK(g.issuedEpoch != g.epoch);
    CHECK(!e.enabled);

    // PUMP 2 -- the refresh goes out and the ENGINE ANSWERS.  Its answer is 0: sub_1401B0D80:90
    // found the actor already carrying the body template team 0 asks for, so the function returned
    // at :91 having allocated nothing and dispatched nothing.  That is L3 #31's three calls, and
    // it is now a named refusal instead of an unexplained silence.
    PawnProvision::HopDrive p2 = DriveFor(g, 0, 2);
    const PawnProvision::ParticipateOutcome o2 = PawnProvision::Participate(h, 7, p2);
    CHECK(o2.why == PawnProvision::ParticipateRefusal::RefreshDeclinedKeyUnchanged);
    CHECK(e.spawnWrites == 1);     // nothing was written twice
    CHECK(e.refreshes == 1);
    CHECK(e.spawnedWhenRefreshed); // the record the dispatch reads was there before the call
    CHECK(e.assigns == 0);         // and nothing the recording owns has been touched
    CHECK(g.lastRefresh == PawnProvision::RefreshAnswer::Declined);
    CHECK(g.refreshDeclined);
    CHECK(g.issues == 0);

    // EVERY LATER WINDOW -- LAUNCH-GAMETYPE-FIX's five-entrant load sat exactly here, and the arm
    // that answered it freed three actors and took the game down a frame later.  The bounce is
    // refused for this population and NOTHING else is driven: no second refresh, no assign, and
    // the entrant's SActorGUID still names the actor the engine allocated.
    const uint64_t guidBefore = e.guidLo;
    for (uint16_t w = 0; w < PawnProvision::kRefreshAttemptMax; ++w) {
        PawnProvision::HopDrive d = DriveFor(g, 0, 2);
        const PawnProvision::Hop hop = PawnProvision::AdvanceActorHops(h, 7, &d);
        CHECK(hop == PawnProvision::Hop::Enabled);
        CHECK(d.outcome.state == PawnProvision::Participation::Refused);
        const bool named =
            d.outcome.why == PawnProvision::ParticipateRefusal::BounceUnsafeForMultipleEntrants ||
            d.outcome.why == PawnProvision::ParticipateRefusal::RefreshAttemptsExhausted;
        CHECK(named);
    }
    CHECK(g.refreshAttempts == PawnProvision::kRefreshAttemptMax);   // the budget WAS spent
    CHECK(e.refreshes == 1);
    CHECK(e.assigns == 0);
    CHECK(e.teamSlot == 0);
    CHECK(e.guidLo == guidBefore);
    CHECK(g.issues == 0);
}

// The hop that issued NETGAME.SET_PLAYER_ACTOR(ng, index, -1) is GONE, and this is what keeps it
// gone.  The call releases the actor and leaves the entrant's SActorGUID naming it, and the
// engine's next frame poses every seated entrant through that guid: sub_140D89DD0 calls the bone
// getter sub_1403C4470 on the miss without a null test (L3 #35, LAUNCH-GAMETYPE-FIX).
template <class Hops>
constexpr bool kHasFreeActorHop = requires(Hops hops) { hops.freePlayerActor; };
static_assert(!kHasFreeActorHop<PawnProvision::ActorHops>,
              "no hop may free an entrant's player actor from the tick");

TEST_CASE("W2-P1-32: the readiness guard defers, and the refusal that drove nothing is not Issued") {
    SUBCASE("assets not ready: no engine call, a named deferral, and the next window issues") {
        FakeHopEngine e;
        PawnProvision::ActorHops h = HopsFor(e);
        e.inCs = true;
        e.team = 1;
        e.spawned = true;
        e.assetsReady = false;   // L3 #21's precondition, read off the engine before the call
        PawnProvision::DriveGate g;
        // The refresh runs FIRST now, and these are its own cases, so they are set in the state
        // where it does work: the entrant's actor does NOT already carry the body template its
        // team asks for, so sub_1401B0D80:90 lets the call past its early return.
        e.actorHasTeamKey = false;

        PawnProvision::HopDrive p1 = DriveFor(g, 1);
        const PawnProvision::ParticipateOutcome o1 = PawnProvision::Participate(h, 7, p1);
        CHECK(o1.state == PawnProvision::Participation::Refused);
        CHECK(o1.why == PawnProvision::ParticipateRefusal::ActorAssetsNotReady);
        CHECK(e.readinessAsks == 1);
        CHECK(e.refreshes == 0);   // the call that hung L3 #21 was NOT made
        CHECK(g.issues == 0);

        // The component instance arrives -- the RemotePlayer CS builds it on its own update -- and
        // the very next window drives the thing the last one refused.
        e.assetsReady = true;
        PawnProvision::HopDrive p2 = DriveFor(g, 1);
        CHECK(PawnProvision::Participate(h, 7, p2).state ==
              PawnProvision::Participation::Issued);
        CHECK(e.refreshes == 1);
    }

    SUBCASE("the deferral is BOUNDED: it stops asking and stops calling, by name") {
        FakeHopEngine e;
        PawnProvision::ActorHops h = HopsFor(e);
        e.inCs = true;
        e.team = 1;
        e.spawned = true;
        e.assetsReady = false;
        PawnProvision::DriveGate g;
        // The refresh runs FIRST now, and these are its own cases, so they are set in the state
        // where it does work: the entrant's actor does NOT already carry the body template its
        // team asks for, so sub_1401B0D80:90 lets the call past its early return.
        e.actorHasTeamKey = false;
        for (uint16_t i = 0; i < PawnProvision::kRefreshAttemptMax; ++i) {
            PawnProvision::HopDrive d = DriveFor(g, 1);
            CHECK(PawnProvision::Participate(h, 7, d).why ==
                  PawnProvision::ParticipateRefusal::ActorAssetsNotReady);
        }
        CHECK(e.readinessAsks == PawnProvision::kRefreshAttemptMax);
        PawnProvision::HopDrive last = DriveFor(g, 1);
        CHECK(PawnProvision::Participate(h, 7, last).why ==
              PawnProvision::ParticipateRefusal::RefreshAttemptsExhausted);
        CHECK(e.readinessAsks == PawnProvision::kRefreshAttemptMax);   // it stopped asking too
    }

    SUBCASE("the call returned and the engine enabled nothing: refused, never Issued") {
        FakeHopEngine e;
        PawnProvision::ActorHops h = HopsFor(e);
        e.inCs = true;
        e.team = 1;
        e.spawned = true;
        e.refreshEnables = false;   // the call returns; the dispatch enables nothing
        PawnProvision::DriveGate g;
        // The refresh runs FIRST now, and these are its own cases, so they are set in the state
        // where it does work: the entrant's actor does NOT already carry the body template its
        // team asks for, so sub_1401B0D80:90 lets the call past its early return.
        e.actorHasTeamKey = false;
        PawnProvision::HopDrive d = DriveFor(g, 1);

        const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(h, 7, d);
        CHECK(e.refreshes == 1);    // it WAS called -- this is not the guard refusing
        CHECK(o.state == PawnProvision::Participation::Refused);
        CHECK(o.why == PawnProvision::ParticipateRefusal::ActorNotEnabledAfterRefresh);
        CHECK(g.issues == 0);
        CHECK(g.via == PawnProvision::ParticipateDrive::None);
    }

    SUBCASE("nothing bound to re-run the dispatch: ParticipationNotReRun, never a skip") {
        FakeHopEngine e;
        PawnProvision::ActorHops h = HopsFor(e);
        h.refreshPlayerActor = nullptr;
        e.inCs = true;
        // The ONLY state in which neither re-entry exists: the refresh is unbound and the
        // recorded role is the one sub_1401AB730:68 will not take as a request, so no bounce can
        // be built out of it either.
        e.team = 3;
        e.spawned = true;
        PawnProvision::DriveGate g;
        PawnProvision::HopDrive d = DriveFor(g, 3);

        const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(h, 7, d);
        CHECK(o.state == PawnProvision::Participation::Refused);
        CHECK(o.why == PawnProvision::ParticipateRefusal::ParticipationNotReRun);
        CHECK(g.issues == 0);
    }
}

// AUD-W2-P1-32 MAJOR-1.  The two arms of this branch do not cost the same thing: a deferral makes
// no engine call, while every refresh calls NETGAME.SET_PLAYER_ACTOR unconditionally -- an actor
// rebuild.  One budget for both meant an entrant whose enable never fires took ~63 rebuilds in a
// minute, on exactly the run this packet exists to make readable.
TEST_CASE("W2-P1-32: the CALL budget is separate from the window budget, and far tighter") {
    SUBCASE("a refresh that never enables stops calling, and the DECLINE stops it sooner") {
        FakeHopEngine e;
        PawnProvision::ActorHops h = HopsFor(e);
        e.inCs = true;
        e.team = 1;
        e.teamSlot = 0;
        e.teamCount[0] = 0;
        e.teamCount[1] = 1;
        e.spawned = true;
        e.refreshEnables = false;   // the call returns and the dispatch enables nothing
        e.assignEnables = false;    // and neither does the bounce, so nothing is ever booked
        PawnProvision::DriveGate g;
        // The refresh runs FIRST now, and this case is set in the state where it does work: the
        // entrant's actor does NOT already carry the body template its team asks for, so
        // sub_1401B0D80:90 lets the first call past its early return.  The SECOND call meets the
        // key the first one installed, which is exactly the L3 #31 state.
        e.actorHasTeamKey = false;

        // Far more windows than any budget, and not one of them is silent.
        for (uint16_t w = 0; w < uint16_t(PawnProvision::kRefreshIssueMax * 5 + 7); ++w) {
            PawnProvision::HopDrive d = DriveFor(g, 1);
            const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(h, 7, d);
            CHECK(o.state == PawnProvision::Participation::Refused);
        }
        // THE ASSERTION MAJOR-1 IS ABOUT: actor rebuilds, counted.  TWO, not kRefreshIssueMax
        // windows' worth: one that re-dispatched and one that the engine declined.  The decline
        // is what stops the third rebuild the old budget would have paid for.
        CHECK(e.refreshes == 2);
        CHECK(g.refreshIssues == 2);
        CHECK(g.refreshIssues < PawnProvision::kRefreshIssueMax);
        CHECK(g.issues == 0);
        // ...and the readiness guard is not asked once every arm is spent either.
        CHECK(e.readinessAsks == 2);
    }

    SUBCASE("deferrals are charged to the window budget and NOT to the call budget") {
        FakeHopEngine e;
        PawnProvision::ActorHops h = HopsFor(e);
        e.inCs = true;
        e.team = 1;
        e.spawned = true;
        e.assetsReady = false;   // refuses BEFORE the call, so no rebuild happens
        PawnProvision::DriveGate g;
        // The refresh runs FIRST now, and these are its own cases, so they are set in the state
        // where it does work: the entrant's actor does NOT already carry the body template its
        // team asks for, so sub_1401B0D80:90 lets the call past its early return.
        e.actorHasTeamKey = false;
        for (uint16_t w = 0; w < uint16_t(PawnProvision::kRefreshIssueMax * 4); ++w) {
            PawnProvision::HopDrive d = DriveFor(g, 1);
            CHECK(PawnProvision::Participate(h, 7, d).why ==
                  PawnProvision::ParticipateRefusal::ActorAssetsNotReady);
        }
        CHECK(e.refreshes == 0);
        CHECK(g.refreshIssues == 0);   // a deferral that called nothing spent no call budget
        CHECK(g.refreshAttempts == uint16_t(PawnProvision::kRefreshIssueMax * 4));
        // And the call budget is intact: the moment the guard comes good, the refresh goes out.
        e.assetsReady = true;
        PawnProvision::HopDrive last = DriveFor(g, 1);
        CHECK(PawnProvision::Participate(h, 7, last).state ==
              PawnProvision::Participation::Issued);
        CHECK(e.refreshes == 1);
    }
}


// L3 #31 vs L3 #30, decided from the engine's own text.  The refresh ran three times against an
// equal role and enabled nothing; the assign ran once against a DIFFERENT role and enabled the
// actor in the same block.  What separates them is the role change, so the equal-role arm makes
// one -- through the other participating role and straight back.
// AUD-W2-P1-33 BLOCKER-1.  The bounce is not symmetric in the thing the recording owns:
// IDENT.SERVER_ASSIGN_TEAM reallocates the entrant's Team Slot on every leg, and the leg home
// lands on `count[T]-1`.  So it is the LAST arm, behind the two that touch nothing, and it is
// gated on the one population where `count[T]-1` is the entrant's own slot.
TEST_CASE("W2-P1-34: the bounce is LAST, and it is refused for a population it would corrupt") {
    SUBCASE("refresh declined, then the re-seat is preferred and its Team Slot round-trips (N==1)") {
        FakeHopEngine e;
        PawnProvision::ActorHops h = HopsFor(e);
        e.inCs = true;
        e.team = 1;
        e.teamSlot = 0;            // sole member of team 1
        e.teamCount[0] = 0;
        e.teamCount[1] = 1;
        e.spawned = true;
        e.enabled = false;
        e.refreshEnables = false;  // every dispatch the refresh reaches enables nothing
        PawnProvision::DriveGate g;

        PawnProvision::HopDrive p1 = DriveFor(g, 1);
        CHECK(PawnProvision::Participate(h, 7, p1).why ==
              PawnProvision::ParticipateRefusal::RefreshDeclinedKeyUnchanged);
        CHECK(e.refreshes == 1);
        CHECK(e.assigns == 0);   // the arm that moves a recorded Team Slot has not been reached

        // The decline admits the bounce, and at N==1 it round-trips the recorded Team Slot
        // (count[T]-1 IS the sole member's own slot).
        PawnProvision::HopDrive p2 = DriveFor(g, 1);
        CHECK(PawnProvision::Participate(h, 7, p2).why ==
              PawnProvision::ParticipateRefusal::ReSeatAwaitingReRead);
        CHECK(e.assigns == 2);   // away and back
        CHECK(e.team == 1);
        CHECK(e.teamSlot == 0);  // survives: count[T]-1 is the entrant's own slot
        CHECK(g.reSeatIssues == 1);
    }

    SUBCASE("more than one recorded entrant: the bounce is refused BY NAME, uncalled") {
        // The goal's own case.  A 4v4 entrant that leaves team T and comes back lands on
        // `count[T]-1` -- a slot another player holds -- and leaves its own in the free list.
        FakeHopEngine e;
        PawnProvision::ActorHops h = HopsFor(e);
        e.inCs = true;
        e.team = 1;
        e.teamSlot = 2;
        e.teamCount[1] = 4;
        e.spawned = true;
        e.refreshEnables = false;
        PawnProvision::DriveGate g;

        const uint64_t guidBefore = e.guidLo;
        for (uint16_t w = 0; w < PawnProvision::kReSeatSettleWindows + 6; ++w) {
            PawnProvision::HopDrive d = DriveFor(g, 1, 4);   // four recorded players
            PawnProvision::Participate(h, 7, d);
        }
        PawnProvision::HopDrive last = DriveFor(g, 1, 4);
        const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(h, 7, last);
        CHECK(o.state == PawnProvision::Participation::Refused);
        CHECK(o.why == PawnProvision::ParticipateRefusal::BounceUnsafeForMultipleEntrants);
        CHECK(strcmp(PawnProvision::ParticipateRefusalName(o.why),
                     "BounceUnsafeForMultipleEntrants") == 0);
        // NOT ONE assign reached the engine, so not one Team Slot moved.
        CHECK(e.assigns == 0);
        CHECK(e.teamSlot == 2);
        CHECK(g.reSeatIssues == 0);
        CHECK(g.issues == 0);
        // The one arm that touches nothing the recording owns DID run, once, and the actor the
        // engine allocated is still the entrant's.
        CHECK(e.refreshes == 1);
        CHECK(e.guidLo == guidBefore);
    }

    SUBCASE("an entrant count nobody supplied fails CLOSED under the same name") {
        FakeHopEngine e;
        PawnProvision::ActorHops h = HopsFor(e);
        e.inCs = true;
        e.team = 1;
        e.teamSlot = 0;
        e.spawned = true;
        e.refreshEnables = false;
        PawnProvision::DriveGate g;
        for (uint16_t w = 0; w < 4; ++w) {
            PawnProvision::HopDrive d = DriveFor(g, 1, 0);   // the caller did not say
            const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(h, 7, d);
            CHECK(o.state == PawnProvision::Participation::Refused);
        }
        CHECK(e.assigns == 0);
        CHECK(g.reSeatIssues == 0);
    }

    SUBCASE("the bounce ran and the Team Slot did NOT come back: refused, never Done") {
        // The count said one entrant and the engine's own team population says otherwise, which is
        // the case the count gate cannot see: the allocator grabs `count[T]` on the way home and
        // the entrant returns wearing a slot the recording never gave it.
        FakeHopEngine e;
        PawnProvision::ActorHops h = HopsFor(e);
        e.inCs = true;
        e.team = 1;
        e.teamSlot = 0;
        e.teamCount[1] = 2;        // a second body on team 1 the recorded count did not mention
        e.spawned = true;
        e.refreshEnables = false;
        PawnProvision::DriveGate g;
        g.refreshDeclined = true;   // the refresh has had its turn

        PawnProvision::HopDrive d = DriveFor(g, 1);
        const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(h, 7, d);
        CHECK(o.state == PawnProvision::Participation::Refused);
        CHECK(o.why == PawnProvision::ParticipateRefusal::TeamSlotChangedByBounce);
        CHECK(strcmp(PawnProvision::ParticipateRefusalName(o.why), "TeamSlotChangedByBounce") == 0);
        CHECK(e.assigns == 2);     // both legs went out -- this is the read-back refusing
        CHECK(e.team == 1);        // the ROLE came back
        CHECK(e.teamSlot == 1);    // and the SLOT did not: `count[T]-1`, not the recorded 0
        CHECK(g.issues == 0);
        CHECK(!g.reSeatPending);   // nothing is awaiting a re-read that could book this as Done
    }

    SUBCASE("neither re-entry enables: both budgets are spent, once each, and it says so") {
        FakeHopEngine e;
        PawnProvision::ActorHops h = HopsFor(e);
        e.inCs = true;
        e.team = 1;
        e.teamSlot = 0;
        e.teamCount[0] = 0;
        e.teamCount[1] = 1;
        e.spawned = true;
        e.assignEnables = false;
        e.refreshEnables = false;
        PawnProvision::DriveGate g;
        for (uint16_t w = 0; w < uint16_t(PawnProvision::kRefreshIssueMax * 4 + 3); ++w) {
            PawnProvision::HopDrive d = DriveFor(g, 1);
            CHECK(PawnProvision::Participate(h, 7, d).state ==
                  PawnProvision::Participation::Refused);
        }
        CHECK(e.assigns == 2 * PawnProvision::kReSeatIssueMax);
        CHECK(e.refreshes == 1);   // the one that declined.  NOT one per window
        CHECK(g.issues == 0);
        CHECK(g.via == PawnProvision::ParticipateDrive::None);
    }

    SUBCASE("a recorded role the engine will not take as a request is not bounced through") {
        // sub_1401AB730:68 rewrites a requested 3 to whatever the word already holds, so there is
        // no leg out of it and no leg back into it.  The arm builds no bounce rather than a wrong
        // one, so a declined refresh is the standing fact and it is said by name.
        FakeHopEngine e;
        PawnProvision::ActorHops h = HopsFor(e);
        e.inCs = true;
        e.team = 3;
        e.spawned = true;
        PawnProvision::DriveGate g;
        PawnProvision::HopDrive d1 = DriveFor(g, 3);
        CHECK(PawnProvision::Participate(h, 7, d1).why ==
              PawnProvision::ParticipateRefusal::RefreshDeclinedKeyUnchanged);
        PawnProvision::HopDrive d2 = DriveFor(g, 3);
        CHECK(PawnProvision::Participate(h, 7, d2).why ==
              PawnProvision::ParticipateRefusal::ActorNotEnabledAfterRefresh);
        CHECK(e.assigns == 0);
        CHECK(e.refreshes == 1);   // and the declined call is not issued a second time
        CHECK(g.issues == 0);
        CHECK(g.via == PawnProvision::ParticipateDrive::None);
    }

    SUBCASE("the leg BACK not taking is named, not papered over -- and A3 re-seats it next pump") {
        FakeHopEngine e;
        PawnProvision::ActorHops h = HopsFor(e);
        e.inCs = true;
        e.team = 1;
        e.teamSlot = 0;
        e.teamCount[0] = 0;
        e.teamCount[1] = 1;
        e.spawned = true;
        e.assignFailOnCall = 2;   // away takes, home does not
        PawnProvision::DriveGate g;
        g.refreshDeclined = true;

        PawnProvision::HopDrive p1 = DriveFor(g, 1);
        CHECK(PawnProvision::Participate(h, 7, p1).why ==
              PawnProvision::ParticipateRefusal::ReSeatNotRestored);
        CHECK(e.team == 0);        // and the entrant IS on a role the recording never gave it
        CHECK(g.issues == 0);

        // The live role no longer matches the recorded one, so the next window is the ordinary A3
        // arm -- the drive L3 #30 measured working -- and it puts the entrant back.
        e.assignFailOnCall = 0;
        PawnProvision::HopDrive p2 = DriveFor(g, 1);
        CHECK(PawnProvision::Participate(h, 7, p2).state == PawnProvision::Participation::Issued);
        CHECK(e.team == 1);
        CHECK(g.via == PawnProvision::ParticipateDrive::AssignTeam);
    }
}

// AUD-W2-P1-33 MEDIUM-1.  The refresh's return was discarded at the call site for two packets, so
// "the engine ran its dispatch and nothing came on" and "the engine returned at :91 having run
// nothing" were the same reading.  They are not the same fact and they do not have the same fix.
TEST_CASE("W2-P1-34: the refresh's own return is kept, and it is what selects the next arm") {
    SUBCASE("a refresh that DID re-dispatch never reaches the bounce") {
        FakeHopEngine e;
        PawnProvision::ActorHops h = HopsFor(e);
        e.inCs = true;
        e.team = 1;
        e.spawned = true;
        e.actorHasTeamKey = false;   // the key differs, so :90 lets the call through
        PawnProvision::DriveGate g;

        PawnProvision::HopDrive d = DriveFor(g, 1);
        CHECK(PawnProvision::Participate(h, 7, d).state == PawnProvision::Participation::Issued);
        CHECK(g.lastRefresh == PawnProvision::RefreshAnswer::Redispatched);
        CHECK(!g.refreshDeclined);
        CHECK(e.assigns == 0);
        CHECK(g.via == PawnProvision::ParticipateDrive::RefreshPlayerActor);
    }

    SUBCASE("a call that was NOT issued is not a decline, and does not authorise a rebuild") {
        FakeHopEngine e;
        PawnProvision::ActorHops h = HopsFor(e);
        e.inCs = true;
        e.team = 1;
        e.spawned = true;
        e.refreshUnavailable = true;
        PawnProvision::DriveGate g;

        PawnProvision::HopDrive d = DriveFor(g, 1);
        const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(h, 7, d);
        CHECK(o.state == PawnProvision::Participation::Refused);
        CHECK(o.why == PawnProvision::ParticipateRefusal::ParticipationNotReRun);
        CHECK(!g.refreshDeclined);
        CHECK(g.issues == 0);
    }

    SUBCASE("declined with no bounce to take: the standing fact is named, and it is not silent") {
        FakeHopEngine e;
        PawnProvision::ActorHops h = HopsFor(e);
        e.inCs = true;
        e.team = 3;   // no away role exists, so the bounce cannot stand in for it either
        e.spawned = true;
        PawnProvision::DriveGate g;

        PawnProvision::HopDrive p1 = DriveFor(g, 3);
        CHECK(PawnProvision::Participate(h, 7, p1).why ==
              PawnProvision::ParticipateRefusal::RefreshDeclinedKeyUnchanged);
        PawnProvision::HopDrive p2 = DriveFor(g, 3);
        const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(h, 7, p2);
        CHECK(o.state == PawnProvision::Participation::Refused);
        CHECK(o.why == PawnProvision::ParticipateRefusal::ActorNotEnabledAfterRefresh);
        CHECK(e.refreshes == 1);   // and the declined call is not issued a second time
    }
}
// AUD-W2-P1-32 MINOR-4.  Before this packet, a mis-guarded index 0 reached an arm that called
// nothing.  Now it would reach NETGAME.REFRESH_PLAYER_ACTOR, which performs no index check of its
// own, so A0's partition must fail CLOSED and by name.
TEST_CASE("W2-P1-32: an unreadable local-user count refuses ViewerIndexRefused, before any call") {
    FakeHopEngine e;
    PawnProvision::ActorHops h = HopsFor(e);
    e.inCs = true;
    e.team = 1;
    e.spawned = true;
    e.localUserUnreadable = true;
    PawnProvision::DriveGate g;
    PawnProvision::HopDrive d = DriveFor(g, 1);

    const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(h, 7, d);
    CHECK(o.state == PawnProvision::Participation::Refused);
    CHECK(o.why == PawnProvision::ParticipateRefusal::ViewerIndexRefused);
    CHECK(strcmp(PawnProvision::ParticipateRefusalName(o.why), "ViewerIndexRefused") == 0);
    // Nothing at all was driven, and A2 never even ran.
    CHECK(e.refreshes == 0);
    CHECK(e.readinessAsks == 0);
    CHECK(e.spawnWrites == 0);
    CHECK(e.assigns == 0);
    CHECK(g.issues == 0);
}

// S14.7 test 9 -- one authored transition per epoch per slot.
TEST_CASE("W2-P1-29: the gate admits one participate per epoch per slot, never one per frame") {
    FakeHopEngine e;
    PawnProvision::ActorHops h = HopsFor(e);
    PawnProvision::DriveGate g;
    e.inCs = true;
    e.assignWorks = false;   // the engine's word never moves, so nothing self-limits but the gate

    for (int frame = 0; frame < 10; ++frame) {
        PawnProvision::HopDrive d = DriveFor(g, 0);
        (void)PawnProvision::AdvanceActorHops(h, 7, &d);
    }
    // A refusal does not spend the epoch, so this is bounded by the ATTEMPT rule, not the gate.
    CHECK(e.assigns == 10);
    CHECK(g.issues == 0);

    // With the engine taking the call, the gate closes after exactly one issue per epoch.
    FakeHopEngine ok;
    ok.inCs = true;
    PawnProvision::ActorHops oh = HopsFor(ok);
    PawnProvision::DriveGate og;
    for (int frame = 0; frame < 10; ++frame) {
        PawnProvision::HopDrive d = DriveFor(og, 0);
        (void)PawnProvision::AdvanceActorHops(oh, 7, &d);
    }
    CHECK(ok.assigns == 1);
    CHECK(ok.spawnWrites == 1);
    CHECK(og.issues == 1);
    CHECK(og.issuedEpoch == og.epoch);
    CHECK(og.issuedRole == 0);

    // A new epoch is a new arm -- a level load, a re-provision -- and re-opens exactly one issue.
    og.epoch = 1;
    ok.team = 3;
    ok.spawned = false;
    PawnProvision::HopDrive d2 = DriveFor(og, 0);
    (void)PawnProvision::AdvanceActorHops(oh, 7, &d2);
    CHECK(ok.assigns == 2);
    CHECK(og.issues == 2);
    PawnProvision::HopDrive d3 = DriveFor(og, 0);
    (void)PawnProvision::AdvanceActorHops(oh, 7, &d3);
    CHECK(ok.assigns == 2);
}

// S14.7 test 10 -- the whole participating arm runs synchronously from A3 (S14.6), so a hook of
// ours firing inside it must not be able to start a second transition.
TEST_CASE("W2-P1-29: the re-entrancy latch blocks a nested participate") {
    FakeHopEngine e;
    PawnProvision::ActorHops h = HopsFor(e);
    PawnProvision::DriveGate g;
    PawnProvision::DriveGate nestedGate;
    PawnProvision::HopDrive nested = DriveFor(nestedGate, 0);
    e.inCs = true;
    e.reenter = &h;
    e.reenterDrive = &nested;

    PawnProvision::HopDrive d = DriveFor(g, 0);
    CHECK(PawnProvision::Participate(h, 7, d).state == PawnProvision::Participation::Issued);
    CHECK(e.nested.state == PawnProvision::Participation::Refused);
    CHECK(e.nested.why == PawnProvision::ParticipateRefusal::Reentrant);
    // The nested call authored NOTHING: one write and one assign for the whole outer transition.
    CHECK(e.spawnWrites == 1);
    CHECK(e.assigns == 1);
    CHECK(nestedGate.issues == 0);

    // And the latch is released afterwards, so the next frame is not permanently locked out.
    e.reenter = nullptr;
    e.team = 3;
    e.spawned = false;
    g.epoch = 1;
    PawnProvision::HopDrive d2 = DriveFor(g, 0);
    CHECK(PawnProvision::Participate(h, 7, d2).state == PawnProvision::Participation::Issued);
}

// S14.7 test 11 -- a build that cannot drive must SAY so, and may not report Armed.
TEST_CASE("W2-P1-29: a gated-off build cannot claim the feature") {
    FakeHopEngine e;
    PawnProvision::ActorHops h = HopsFor(e);
    CHECK(PawnProvision::ParticipationFeatureState(h).armed);
    CHECK(strcmp(PawnProvision::ParticipationFeatureState(h).reason, "") == 0);

    struct Row { PawnProvision::ActorHops h; const char* reason; };
    PawnProvision::ActorHops noWrite = h;   noWrite.writeSpawned = nullptr;
    PawnProvision::ActorHops noAssign = h;  noAssign.assignTeam = nullptr;
    PawnProvision::ActorHops noTeam = h;    noTeam.teamOf = nullptr;
    PawnProvision::ActorHops noSpawned = h; noSpawned.placed = nullptr;
    const Row rows[] = {{noWrite, "NoSpawnedWriter"},
                        {noAssign, "NoAssignTeam"},
                        {noTeam, "NoTeamRead"},
                        {noSpawned, "NoSpawnedRead"}};
    for (const Row& r : rows) {
        const PawnProvision::FeatureState fs = PawnProvision::ParticipationFeatureState(r.h);
        CHECK_FALSE(fs.armed);
        CHECK(strcmp(fs.reason, r.reason) == 0);
        // And such a surface issues nothing at all rather than half a transition.
        PawnProvision::DriveGate g;
        PawnProvision::HopDrive d = DriveFor(g, 0);
        FakeHopEngine* eng = static_cast<FakeHopEngine*>(r.h.ctx);
        eng->inCs = true;
        const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(r.h, 7, d);
        CHECK(o.state == PawnProvision::Participation::Refused);
        CHECK(o.why == PawnProvision::ParticipateRefusal::NoDriver);
    }
    CHECK(e.spawnWrites == 0);
    CHECK(e.assigns == 0);

    // ★ A chain handed no gate drives nothing at all -- the state a build ships in when the caller
    // forgets, and it must be a named refusal rather than a quiet reads-only pass.
    PawnProvision::HopDrive noGate;
    noGate.gate = nullptr;
    e.inCs = true;
    CHECK(PawnProvision::Participate(h, 7, noGate).why ==
          PawnProvision::ParticipateRefusal::NoGate);
    CHECK(e.spawnWrites == 0);
    CHECK(PawnProvision::AdvanceActorHops(h, 7, nullptr) == PawnProvision::Hop::Enabled);
    CHECK(e.spawnWrites == 0);
    CHECK(e.assigns == 0);
}

// S14.7 test 12 -- intentions, skips and refusals are counted separately and can never make the
// total.  This is the case that makes a reads-only build RED however green its hops read.
TEST_CASE("W2-P1-29: the census counts issues, not intentions") {
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    FakeIdentity id;
    FakeHopEngine e;
    // Every hop already reads DONE and the entrant is already on the recorded role: this session
    // drove nothing, so it may not claim the pawn however complete the engine's records look.
    e.inCs = true; e.enabled = true; e.spawned = true; e.physCi = true; e.team = 0;
    PawnProvision::ActorHops h = HopsFor(e);

    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.hops = &h;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), JoinDesc{}));

    uint32_t done = 0;
    for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks + 4; ++i) done += p.Pump();
    CHECK(done == 0);
    CHECK(p.Ledger().provisioned == 0);
    CHECK(p.Ledger().participateIssued == 0);
    CHECK(p.Ledger().participateSkipped == 1);
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::ParticipationNotIssued);
    CHECK(e.assigns == 0);

    // A refused participation is its own number too, and it names itself.
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    FakeIdentity id2;
    FakeHopEngine e2;
    e2.inCs = true;
    PawnProvision::ActorHops h2 = HopsFor(e2);
    PawnProvisioner p2;
    ProvisionEnv env2 = Armed(id2, 2);
    env2.hops = &h2;
    p2.Arm(env2);
    JoinDesc spectator{};
    spectator.team = 2;
    REQUIRE(p2.Request(UserSlot(0), spectator));
    for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks + 4; ++i) (void)p2.Pump();
    CHECK(p2.Ledger().provisioned == 0);
    CHECK(p2.Ledger().participateIssued == 0);
    CHECK(p2.Ledger().participateRefused == 1);
    CHECK(p2.Ledger().participateRefusals[size_t(
              PawnProvision::ParticipateRefusal::RecordedAsSpectator)] == 1);
    CHECK(e2.assigns == 0);
    CHECK(e2.spawnWrites == 0);
}

// A0's partition, which is a partition and not a bound: an entrant below the netgame's LOCAL-user
// count is one the engine's own first block already owns.
TEST_CASE("W2-P1-29: a local user is refused by name and nothing is authored for it") {
    FakeHopEngine e;
    PawnProvision::ActorHops h = HopsFor(e);
    PawnProvision::DriveGate g;
    PawnProvision::HopDrive d = DriveFor(g, 0);
    e.inCs = true;
    e.localUser = true;

    const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(h, 7, d);
    CHECK(o.state == PawnProvision::Participation::Refused);
    CHECK(o.why == PawnProvision::ParticipateRefusal::EntrantIsLocalUser);
    CHECK(e.spawnWrites == 0);
    CHECK(e.assigns == 0);
}

// A2's out-of-range side, refused rather than written: the engine's per-slot setter computes
// store[864] + 216*slot with no bounds check of its own.
TEST_CASE("W2-P1-29: an unwritable spawned record refuses by name and issues no team assign") {
    FakeHopEngine e;
    PawnProvision::ActorHops h = HopsFor(e);
    PawnProvision::DriveGate g;
    PawnProvision::HopDrive d = DriveFor(g, 0);
    e.inCs = true;
    e.spawnSays = PawnProvision::SpawnedWrite::Unwritable;

    const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(h, 7, d);
    CHECK(o.state == PawnProvision::Participation::Refused);
    CHECK(o.why == PawnProvision::ParticipateRefusal::SpawnedRecordUnwritable);
    CHECK(e.assigns == 0);
    CHECK(g.issues == 0);
}

// Every refusal name is its own word, and none is a prefix of another: a census line that printed
// two of these would otherwise be ambiguous to the grep that reads it.
TEST_CASE("W2-P1-29: every participate refusal has its own name") {
    const char* names[PawnProvision::kParticipateRefusalCount];
    for (size_t i = 0; i < PawnProvision::kParticipateRefusalCount; ++i) {
        names[i] = PawnProvision::ParticipateRefusalName(PawnProvision::ParticipateRefusal(i));
        CHECK(names[i][0] != 0);
    }
    for (size_t i = 0; i < PawnProvision::kParticipateRefusalCount; ++i)
        for (size_t j = 0; j < PawnProvision::kParticipateRefusalCount; ++j)
            if (i != j) CHECK(strcmp(names[i], names[j]) != 0);
}

namespace {
PawnProvision::PhysicsCiReq g_ciWhy = PawnProvision::PhysicsCiReq::Unknown;
uint32_t g_ciWhyAsks = 0;
PawnProvision::PhysicsCiReq HopCiWhy(void*, uint16_t) {
    ++g_ciWhyAsks;
    return g_ciWhy;
}
}  // namespace

TEST_CASE("HOPS-3-4: hop 4 names the requirement the engine's own InitCI refused on") {
    FakeHopEngine e;
    PawnProvision::ActorHops h = HopsFor(e);
    PawnProvision::DriveGate g;
    e.inCs = true; e.enabled = true; e.spawned = true;
    g_ciWhy = PawnProvision::PhysicsCiReq::R15Body;
    g_ciWhyAsks = 0;

    // Unwired is COULD-NOT-ASK: the census may not print a requirement nobody read.  HopsFor
    // binds both hop-4 hooks now that the shipped surface does, so the unwired shape is built by
    // clearing them here rather than by relying on a default -- and `physicsCi` goes with it,
    // because a surface that answers "built" has no requirement outstanding to report.
    h.physicsCi = nullptr;
    h.physicsCiAwaiting = nullptr;
    CHECK(PawnProvision::PhysicsCiAwaiting(h, 7) == PawnProvision::PhysicsCiReq::Unknown);
    CHECK(g_ciWhyAsks == 0);

    h.physicsCi = HopPhys;
    h.physicsCiAwaiting = HopCiWhy;
    PawnProvision::HopDrive d = DriveFor(g, 0);
    CHECK(PawnProvision::AdvanceActorHops(h, 7, &d) == PawnProvision::Hop::PhysicsCi);
    // The chain DECIDES the requirement on the same read that decided the hop
    // and hands it back on the drive.  One ask, so the caller never puts the question a second
    // time -- which is what let the engine finish InitCI between two reads and print
    // `PhysicsCiAwaiting(none)` beside a hop that had answered false.
    CHECK(d.physicsCiWhy == PawnProvision::PhysicsCiReq::R15Body);
    CHECK(g_ciWhyAsks == 1);
    // The standalone reader still exists for callers holding no drive, and it IS a second ask:
    // that it moves the counter is the evidence the chain's answer did not come from here.
    CHECK(PawnProvision::PhysicsCiAwaiting(h, 7) == PawnProvision::PhysicsCiReq::R15Body);
    CHECK(g_ciWhyAsks == 2);
    // A pass that reaches Done reports no requirement outstanding, rather than leaving the caller
    // to read Unknown and print a could-not-ask beside a finished hop.
    e.physCi = true;
    PawnProvision::HopDrive done = DriveFor(g, 0);
    if (PawnProvision::AdvanceActorHops(h, 7, &done) == PawnProvision::Hop::Done)
        CHECK(done.physicsCiWhy == PawnProvision::PhysicsCiReq::None);
    e.physCi = false;
    // An unwired reader is a COULD-NOT-ASK, and NoPhysicsCi's text is a positive
    // claim about the engine ("has not built an instance for it yet ... this waits for it").
    CHECK(PawnProvision::PhysicsCiRefusal(PawnProvision::PhysicsCiReq::Unknown) ==
          PawnProvision::Refusal::PhysicsCiUnreadable);
    CHECK(PawnProvision::PhysicsCiRefusal(PawnProvision::PhysicsCiReq::R15Body) ==
          PawnProvision::Refusal::NoPhysicsCi);
    CHECK(strcmp(PawnProvision::PhysicsCiReqName(PawnProvision::PhysicsCiReq::R15Body),
                      "R15Body") == 0);

    // An index nobody issued has no requirement outstanding -- it is before the chain, not in it.
    CHECK(PawnProvision::PhysicsCiAwaiting(h, kSlotUnmapped) ==
          PawnProvision::PhysicsCiReq::Unknown);

    // A built CI has no requirement outstanding, and the reason read is not even consulted.
    e.physCi = true;
    g_ciWhyAsks = 0;
    CHECK(PawnProvision::PhysicsCiAwaiting(h, 7) == PawnProvision::PhysicsCiReq::None);
    CHECK(g_ciWhyAsks == 0);
    PawnProvision::HopDrive d2 = DriveFor(g, 0);
    CHECK(PawnProvision::AdvanceActorHops(h, 7, &d2) == PawnProvision::Hop::Done);
}

TEST_CASE("HOPS-3-4: every requirement name is the engine's own spelling, and all nine differ") {
    const char* names[size_t(PawnProvision::PhysicsCiReq::Unknown) + 1];
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        names[i] = PawnProvision::PhysicsCiReqName(PawnProvision::PhysicsCiReq(i));
        REQUIRE(names[i] != nullptr);
        CHECK(names[i][0] != 0);
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        for (size_t j = i + 1; j < sizeof(names) / sizeof(names[0]); ++j) {
            CHECK(strcmp(names[i], names[j]) != 0);
        }
    }
    // The six the engine raises by name, in the order InitCI checks them.
    CHECK(strcmp(names[size_t(PawnProvision::PhysicsCiReq::NcaPhysics)], "ncaPhysics") == 0);
    CHECK(strcmp(names[size_t(PawnProvision::PhysicsCiReq::R15TouchInteract)],
                      "R15TouchInteract") == 0);
    // and the two that are NOT requirements at all, kept distinct from every one that is
    CHECK(strcmp(names[size_t(PawnProvision::PhysicsCiReq::NotAttempted)],
                      "not-attempted") == 0);
    CHECK(strcmp(names[size_t(PawnProvision::PhysicsCiReq::Unknown)], "unknown") == 0);
}


namespace {

// The pump number the LAST line matching `needle` carries, from the line's own text.  It is what
// answers "did the ledger stop at the edge" without trusting a count: a run whose last window line
// is stamped near its last tick did not stop.
uint64_t LastPumpNumber(const CaptureLog& log, const char* needle) {
    uint64_t last = 0;
    for (const std::string& l : log.lines) {
        const size_t at = l.find(needle);
        if (at == std::string::npos) continue;
        last = strtoull(l.c_str() + at + strlen(needle), nullptr, 10);
    }
    return last;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// L3 #24, THE LIVE SEQUENCE, TICK FOR TICK.
//
// RUN-24's two headline findings are one sequence, and this case is that sequence and nothing
// else: the level loads with NETGAME's state word at 0 for a stretch of ticks; the word edges to
// STATE_INMATCH; the viewer's row appears in the entrant table; our insert is attempted and the
// route defers (ReplyWrongPhase, nothing published from this phase) while the ENGINE goes on to
// allocate the actor out of the pool -- `Actor 0x... (disabled)  (pooled)`, `Loadouts : 0`, an
// entrant that is PRESENT and NOT DRESSED -- and then the session runs for four more minutes with
// nothing else changing at all.
//
// F1: the run printed `provisioned 0, adopted 0, deferred 309, resolved 0` to its last line and
//     `hops:` zero times, because the slot never left Pending.
// F2: the last [pawn] line of the whole log is stamped at the state-9 edge, the millisecond the
//     entrant lands -- the observation window closes as the thing to observe appears.
//
// Four claims, and each one is a line RUN-24 could not print:
//   (a) the slot leaves Pending within ONE attempt window of the engine's records showing the
//       entrant, off the record and not off a second ask;
//   (b) a [pawn] window line prints on EVERY cadence window for the whole run -- before the edge,
//       across it, and nine thousand ticks after it;
//   (c) `hops:` prints once the slot is past Pending, on every window from then on;
//   (d) `deferred` stops climbing the moment the state gate opens.
TEST_CASE("L3 #24: the state-9 edge, an undressed entrant, and nine thousand ticks after it") {
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);   // the viewer holds slot 0, as every live lobby has it
    SetState(0);                          // NETGAME.STATE_OFF: the level is still loading

    FakeIdentity id;
    // The viewer's own user, in the engine's own entrant table.  conn[] is a diagnostic and the
    // entrant table is the rule, so without this row slot 0 reads FREE and the recording's player
    // is mapped onto the viewer's own slot -- which is the one thing INV-PP1 forbids.
    id.Place(0, 7, true);
    // The L3 #22/#24 route: nothing is published from this phase, and the ENGINE completes the
    // entrant out of band on its own pump -- leaving it undressed, which is the whole point.
    id.refuseProvisionWith = PawnProvision::Refusal::ReplyWrongPhase;
    id.engineCompletesAfterInserts = 1;
    id.engineCompletesDressed = false;

    // The actor the engine allocated is pooled and disabled and no component instance ever answers
    // for it: RUN-24's bind line read `1 awaiting physics ci` on every summary to the last one.
    FakeHopEngine eng;
    eng.inCs = true;          // the engine registered the actor with the component system
    eng.enabled = false;      // (disabled) -- the engine's own participating arm drives it
    eng.spawned = false;      // and its `spawned` record is what A2 authors
    eng.physCi = false;       // and the body never arrives: the slot stays Awaiting for the run
    PawnProvision::ActorHops hopSurface = HopsFor(eng);

    CaptureLog log;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.log  = &log;
    env.hops = &hopSurface;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), JoinDesc{}));

    // (i) THE LEVEL LOAD.  The state gate refuses on one guarded read: no lobby is walked, no
    //     insert is called, and the slot sits in Pending -- which is the state the ledger has to
    //     be able to report on, because it is the one the live run never left.
    const uint32_t kBeforeEdge = 40;
    for (uint32_t i = 0; i < kBeforeEdge; ++i) p.Pump();
    CHECK(id.inserts == 0);
    CHECK(p.Ledger().provisioned == 0);
    CHECK(p.SlotRefusal(UserSlot(0)) == PawnProvision::Refusal::SessionNotInMatch);
    CHECK(p.Ledger().deferred == kBeforeEdge);   // one per tick, and the state gate is the reason
    CHECK(log.Count("hops:") == 0);   // nothing is past Pending, and nothing pretends to be
    // (b), first half: the ledger is printing while the slot is PENDING.  The old gate printed
    // only for slots that had reached Awaiting, so this whole stretch was silent.
    CHECK(log.Count("[pawn] window ") >= 2);

    // (ii) THE EDGE.  The blocker moves, the window opens, the lobby is walked, the insert is
    //      called once -- and it DEFERS while the engine goes on to build the entrant.
    SetState(facts::NETGAME_STATE_INMATCH);
    p.Pump();
    CHECK(id.inserts == 1);
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::ReplyWrongPhase);
    CHECK(p.Ledger().provisioned == 0);
    const uint16_t live = p.MappedSlot(UserSlot(0)).v;
    CHECK(live == 1);                       // the viewer holds 0; the recording's player lands in 1
    REQUIRE(live < facts::ENTITY_USER_SLOT_TABLE_MAX);
    // The engine's own record, exactly as RUN-24 dumped it: the row is THERE and it is undressed.
    REQUIRE(id.slots[live].present);
    REQUIRE_FALSE(id.slots[live].dressed);
    const uint32_t deferredAtEdge = p.Ledger().deferred;

    // (iii) (a) ONE ATTEMPT WINDOW LATER the engine's own entrant table is re-read and the slot
    //       leaves Pending.  The insert is NOT called a second time: the answer comes off the
    //       record.  A re-read keyed on the DRESS predicate cannot pass this, because this entrant
    //       is never dressed -- which is precisely why RUN-24 sat in Pending for four minutes.
    const uint32_t kSettleTicks = 2 * PawnProvisioner::kRetryTicks + 2;
    for (uint32_t i = 0; i < kSettleTicks; ++i) p.Pump();
    CHECK(log.Count("counted from the record") == 1);
    CHECK(id.inserts == 1);
    // Past Pending and awaiting the LAST hop by name -- the `1 awaiting physics ci` RUN-24 read on
    // every bind summary, now with the slot that owns it.
    CHECK(log.Count("is awaiting hop 'PhysicsCi'") >= 1);
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::RetryNotDue);
    CHECK(p.Ledger().provisioned == 0);   // admitted is not driveable, and the ledger still says so
    CHECK(eng.assigns == 1);              // the participation the admission unblocked was issued
    CHECK(eng.spawnWrites == 1);
    // (c): the hop census prints once the slot is past Pending, and names the hop it is short of.
    CHECK(log.Count("hops:") >= 1);
    CHECK(log.Count("hops: 0->1=PhysicsCi") >= 1);
    // THE LOADOUT FIELD NAMES ITS REFUSAL.  The demo -> JoinDesc path carries the twenty item ids
    // by POSITION and no slot key, so L1 cannot verify anything and says which fact is missing.
    // A field that printed a placeholder ("S2 open") or a bare zero would read, to the next run,
    // as a dress that had nothing to do.
    CHECK(log.Count("loadout: not-attempted") >= 1);   // DIAG-APPLY-KEY sec A: the SESSION reason (parked), not a recording verdict synthesised from a zeroed struct
    CHECK(log.Count("S2 open") == 0);
    CHECK(log.Count("loadout: applied 0/20") == 0);
    // And the window line carries the shutter the session would refuse a dress with RIGHT NOW,
    // beside the cadence's own remembered blocker: `blocker 'NotAttempted'` is an initial value
    // and never a reason (DIAG-NO-REQUEST).
    CHECK(log.Count("shutter '") >= 1);
    const size_t windowsAtSettle = log.Count("[pawn] window ");
    const size_t hopsAtSettle    = log.Count("hops:");

    // (iv) NINE THOUSAND MORE TICKS with nothing changing -- the four minutes RUN-24 spent silent.
    const uint32_t kAfterEdge = 9000;
    for (uint32_t i = 0; i < kAfterEdge; ++i) p.Pump();

    // (b): one window line per cadence window, for the WHOLE run, derived from the cadence rather
    //      than counted off this fixture -- the first window is pump 0, so the count is the number
    //      of cadence periods the run covers.
    const uint64_t totalPumps = kBeforeEdge + 1 + kSettleTicks + kAfterEdge;
    const uint64_t expectedWindows =
        (totalPumps + PawnProvisioner::kRetryTicks - 1) / PawnProvisioner::kRetryTicks;
    CHECK(log.Count("[pawn] window ") == expectedWindows);
    // and the LAST [pawn] line of the run is one of them, not a line stamped at the edge.
    REQUIRE_FALSE(log.lines.empty());
    CHECK(LastPumpNumber(log, "[pawn] window ") >= totalPumps - PawnProvisioner::kRetryTicks);

    // (c): every one of those windows carried a hop reading, because the slot is past Pending.
    CHECK(log.Count("[pawn] window ") - windowsAtSettle > 0);
    CHECK(log.Count("hops:") - hopsAtSettle == log.Count("[pawn] window ") - windowsAtSettle);

    // (d): the state gate opened once and `deferred` has not moved since.  It is the number of
    //      attempts that could not be made, not a frame counter: RUN-24 read 309 of them over a
    //      seven-second load and could not tell a blocked session from a working one.
    CHECK(p.Ledger().deferred == deferredAtEdge);
    CHECK(deferredAtEdge == kBeforeEdge);
    // The cadence is still the cadence: nine thousand ticks did not buy nine thousand attempts.
    CHECK(p.Attempts() <= totalPumps / PawnProvisioner::kRetryTicks + 2);
    CHECK(id.inserts == 1);
}

// ============================ L3 #25: THE CRASH, AND WHAT PRINTED NOTHING ====================

// L3 #25 crashed the engine with 0xC0000005 inside its own loadout apply at the NetGame state
// 8 -> 9 edge, five seconds after this re-read printed `counted from the record` for the first
// time -- and the engine's log carries NO `Entrant ... added to slot 1` anywhere: no insert ever
// completed.  What the re-read believed was the per-slot side table's bare index word, which the
// admission path never writes and which L3 #17 recorded answering a slot's OWN NUMBER as a legal
// index for a slot no entrant occupies.  The rule pawn_provision.cpp already states for that
// table -- believed only when the entrant it names agrees it holds the slot -- is the fix.
TEST_CASE("the engine-record re-read refuses a side-table index no entrant names back") {
    Reset();
    FakeIdentity id;
    id.Place(0, 0, false);              // the viewer: slot 0, index 0, undressed (W2-P0's shape)
    id.sideTableNamesTheSlot = true;    // the empty offline lobby L3 #17 and #25 both read
    id.refuseProvisionWith = PawnProvision::Refusal::ReplyWrongPhase;
    // and engineCompletesAfterInserts stays 0: NOTHING ever lands, which is RUN-25 exactly.

    // The fixture can fail, and this is the pair that makes it RUN-25's: the side table answers a
    // legal index for slot 1, and no entrant carries that index at all.
    REQUIRE(id.IndexOf(UserSlot(1)).v == 1);
    REQUIRE(id.SlotOf(UserIndex(1)).v == kSlotUnmapped);

    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), JoinDesc{}));

    for (uint32_t i = 0; i < 2 * PawnProvisioner::kRetryTicks + 4; ++i) p.Pump();

    // The map itself is sound -- the viewer's own slot was excluded from it (P1-13) --
    REQUIRE(p.MappedSlot(UserSlot(0)).v == 1);
    // -- and the insert really was attempted more than once, so the re-read really was reached.
    CHECK(id.inserts >= 2);

    // THE DISCRIMINATOR.  No entrant exists, so nothing may be counted and nothing may be handed
    // to the dress pass.  Keyed on the bare index word this reads `provisioned 1` over a slot the
    // engine never filled, and the index it publishes is the one the engine crashed applying.
    CHECK(p.Ledger().provisioned == 0);
    CHECK_FALSE(p.Provisioned(UserSlot(0)));
    CHECK(p.LastProvisionRefusal() != PawnProvision::Refusal::None);
}

// The other half of L3 #25's hypothesis, and the one the cross-check above cannot catch: the
// entrant DOES name the slot back, and it is the viewer's own.  The engine re-seats the local user
// across the state 8 -> 9 edge -- the exact millisecond the crash landed -- so a slot proven free
// when it was mapped can be answered by the one entrant this session did not create.  Settled, its
// index goes to the dress pass and a FILE's loadout and profile are applied to the live local
// user.  INV-PP1 forbids adopting; this is the adoption that had no name.
TEST_CASE("the re-read never settles a recorded slot onto the viewers own entrant") {
    Reset();
    FakeIdentity id;
    id.Place(0, 0, false);              // the viewer, holding slot 0 before anything is asked
    id.refuseProvisionWith = PawnProvision::Refusal::ReplyWrongPhase;

    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), JoinDesc{}));
    CHECK(p.Pump() == 0);
    REQUIRE(p.MappedSlot(UserSlot(0)).v == 1);   // mapped off the viewer's own slot, onto a free one
    REQUIRE(id.inserts == 1);

    // The state-9 edge: the engine seats the local user into the slot this load was mapped into.
    id.slots[0].present = false;
    id.Place(1, 0, false);

    // The cross-check above PASSES on this shape -- the entrant names the slot back -- so the
    // exclusion is the only thing that can refuse it, and this pair says so.
    REQUIRE(id.IndexOf(UserSlot(1)).v == 0);
    REQUIRE(id.SlotOf(UserIndex(0)).v == 1);

    for (uint32_t i = 0; i < 2 * PawnProvisioner::kRetryTicks + 4; ++i) p.Pump();

    CHECK(p.Ledger().provisioned == 0);
    CHECK_FALSE(p.Provisioned(UserSlot(0)));
    // Counted, not read off the last line: the cadence refuses RetryNotDue on every pump
    // between windows, so the slot word names the most recent deferral and the ledger names
    // every one.
    CHECK(p.Ledger().refusals[size_t(PawnProvision::Refusal::ViewerSlotExcluded)] >= 1);
}

// L3 #25 F1: `[pawn] window` printed ZERO times in a 450-line plugin log spanning the whole life
// of the session, on a run that had asked for a slot.  The gate was the two non-terminal states,
// and every request this load made was answered inside the pass that made it -- so no pump ever
// saw a Pending or an Awaiting row and the run could report no state at all.  A request that was
// ever made is work the table has to account for.
TEST_CASE("the window line reports an armed pump -- an empty table and a settled one alike") {
    Reset();
    FakeIdentity id;
    CaptureLog log;

    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.log = &log;
    p.Arm(env);

    // ★ THIS CONTROL USED TO ASSERT ZERO, AND IT WAS CODIFYING THE DEFECT L3 #26 THEN SPENT A RUN
    // ON.  "A table nobody has asked anything of prints nothing" is precisely the state an armed
    // pump is in from the tick it arms until the engine seats an entrant -- five seconds in RUN-26,
    // and forever in any run where the entrant never lands.  A silent pump and an absent pump were
    // the same log, so the run could report neither.  The pump now reports ITSELF on the cadence
    // and the counts in the text report the table.
    for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks + 2; ++i) p.Pump();
    REQUIRE(log.Count("[pawn] window") >= 1);
    // And what it says about the empty table is the empty table, not a fabricated slot.
    REQUIRE(log.Count("pending 0, awaiting 0, provisioned 0, failed 0") >= 1);
    REQUIRE(log.Count("requested 0") >= 1);
    // The FIRST pump prints, not the second: a run whose process dies at 51 s must still carry the
    // provisioner's first word.  Pump 0 is the first cadence window.
    REQUIRE(log.Count("[pawn] window 0:") == 1);

    REQUIRE(p.Request(UserSlot(0), JoinDesc{}));
    CHECK(p.Pump() == 1);
    REQUIRE(p.Ledger().provisioned == 1);
    REQUIRE(p.Ledger().requested == 1);
    // Nothing is left non-terminal: this is the state RUN-25's gate printed nothing over.
    for (uint32_t i = 0; i < 2 * PawnProvisioner::kRetryTicks + 2; ++i) p.Pump();

    CHECK(log.Count("[pawn] window") >= 2);
    CHECK(log.Count("provisioned 1") >= 1);
}

// ============================================================================================
// THE CRASH LOGGER AND THE PHASE MARKERS -- L3 #25/#26/#27 died of 0xC0000005 with no stack
// ============================================================================================
//
// REAL CAPTURE, no mock: the logger is pointed at a file in TEMP, a real access violation is taken
// in a child thread, and the assertions are made against the bytes that reached the disk.  Nothing
// here stands in for the writer; a fake writer is exactly the thing that cannot witness a flush.

namespace {

// The subject of the deliberate fault.  Both the pointer and the pointee are volatile: with only
// the pointee volatile, MSVC constant-propagates the null through /O2 and folds the store into an
// intrinsic that never reaches the exception dispatcher.
volatile int* volatile gNullTarget = nullptr;

DWORD WINAPI FaultThread(LPVOID) {
    __try {
        *gNullTarget = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
    return 0;
}

// A SWALLOWED probe fault, standing in for entity.cpp's TryTouchSpan and tick.cpp's
// TryLoadPointer -- our own DLL takes access violations like this one on purpose, on the game
// thread, at twenty-odd sites.  A READ, so the block it produces is distinguishable from the
// write above by the `access=` word alone.
DWORD WINAPI ProbeFaultThread(LPVOID) {
    __try {
        return *gNullTarget == 0 ? 2 : 3;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

DWORD WINAPI RaiseCodeThread(LPVOID param) {
    __try {
        RaiseException(static_cast<DWORD>(reinterpret_cast<uintptr_t>(param)), 0, 0, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
    return 0;
}

// Every fault is taken on its own thread, so the test's own stack is never the one unwound and a
// handler that did continue execution could not corrupt the run.
DWORD RunOnThread(LPTHREAD_START_ROUTINE proc, void* param) {
    HANDLE t = CreateThread(nullptr, 0, proc, param, 0, nullptr);
    REQUIRE(t != nullptr);
    REQUIRE(WaitForSingleObject(t, 10000) == WAIT_OBJECT_0);
    DWORD rc = 0;
    GetExitCodeThread(t, &rc);
    CloseHandle(t);
    return rc;
}

// A file in TEMP named for this test, deleted before each install so what is read back can only
// have been written by the run that just happened.
struct DiagFile {
    wchar_t path[MAX_PATH];

    DiagFile(const wchar_t* leaf) {
        wchar_t dir[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, dir);
        REQUIRE(n != 0);
        REQUIRE(n < MAX_PATH);
        swprintf(path, MAX_PATH, L"%s%s", dir, leaf);
        DeleteFileW(path);
    }
    ~DiagFile() { PawnProvision::RemoveCrashLogger(); DeleteFileW(path); }

    std::string Read() const {
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return std::string();
        std::string out;
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got != 0) out.append(buf, got);
        CloseHandle(h);
        return out;
    }
};

size_t CountOf(const std::string& hay, const char* needle) {
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos;
         at = hay.find(needle, at + strlen(needle)))
        ++n;
    return n;
}

}  // namespace

TEST_CASE("a crash block names the code, the faulting site, the registers and a real stack") {
    DiagFile diag(L"replay_pawn_crash.log");
    REQUIRE(PawnProvision::InstallCrashLogger(diag.path));
    // A second install must not disturb the first -- the tick calls this every Arm.
    CHECK_FALSE(PawnProvision::InstallCrashLogger(diag.path));

    // The last phase marker is echoed into the block; this is the line that will name the engine
    // call a live run died inside.
    int ctxObject = 0;
    PawnProvision::MarkHop(2, "enable", &ctxObject, 1);
    const uint64_t flushesAfterMarker = PawnProvision::CrashLogFlushes();
    CHECK(flushesAfterMarker >= 1);

    CHECK(RunOnThread(FaultThread, nullptr) == 1);   // the __except handled it, as designed
    const std::string log = diag.Read();

    // The exception itself.
    CHECK(CountOf(log, "[crash] block 1 first-chance code=0xc0000005") == 1);
    CHECK(CountOf(log, "[crash] access=write va=0x0000000000000000") == 1);
    CHECK(CountOf(log, "[crash] end block 1") == 1);
    CHECK(PawnProvision::CrashBlocksWritten() == 1);

    // The faulting site resolved to this module, at a non-zero offset inside it.  The name is
    // asserted with the offset attached: `pawn_selftest.exe` alone would also match a line that
    // resolved nothing.
    const size_t site = log.find("[crash] site=pawn_selftest.exe+0x");   // off the snapshot
    REQUIRE(site != std::string::npos);
    CHECK(log.compare(site + strlen("[crash] site=pawn_selftest.exe+0x"), 1, "0") != 0);
    // ... and the IDA VA, which is the module's own preferred base plus that offset.
    CHECK(log.find(" (ida 0x", site) != std::string::npos);

    // The register file: all sixteen GPRs plus the three the fault is read from.
    for (const char* reg : {"rax=0x", "rbx=0x", "rcx=0x", "rdx=0x", "rsi=0x", "rdi=0x", "rbp=0x",
                            "rsp=0x", "r8=0x", "r9=0x", "r10=0x", "r11=0x", "r12=0x", "r13=0x",
                            "r14=0x", "r15=0x"})
        CHECK(CountOf(log, reg) >= 1);
    CHECK(CountOf(log, "[crash] rip=0x") == 1);

    // BOTH walks, and the unwind is the one that matters: RtlCaptureStackBackTrace starts inside
    // the handler, so the faulting frame is only ever on the manual walk.
    CHECK(CountOf(log, "[crash] capture 0:") == 1);
    CHECK(CountOf(log, "[crash] frame 0:") == 1);
    size_t frames = 0;
    for (unsigned i = 0; i < 48; ++i) {
        char needle[32];
        snprintf(needle, sizeof needle, "[crash] frame %u:", i);
        if (log.find(needle) == std::string::npos) break;
        ++frames;
    }
    CHECK(frames >= 3);

    // The last marker, verbatim.
    char expect[128];
    snprintf(expect, sizeof expect, "[crash] lastmarker=hop 2 enable: ctx=0x%016llx index=0x0001",
             (unsigned long long)(uintptr_t)&ctxObject);
    CHECK(CountOf(log, expect) == 1);

    // THE BLOCK MUST SURVIVE A SWALLOWED FAULT, because this process takes those
    // on purpose: a lifetime latch would have spent its one block on the probe and printed nothing
    // for the fault that killed the process.  A REAL swallowed AV, then a REAL second one.
    CHECK(RunOnThread(ProbeFaultThread, nullptr) == 1);
    CHECK(RunOnThread(FaultThread, nullptr) == 1);
    const std::string after = diag.Read();
    CHECK(PawnProvision::CrashBlocksWritten() == 3);
    CHECK(CountOf(after, "[crash] block 2 first-chance code=0xc0000005") == 1);
    CHECK(CountOf(after, "[crash] block 3 first-chance code=0xc0000005") == 1);
    CHECK(CountOf(after, "[crash] end block 3") == 1);
    // Increasing, and the LAST one names the second site: block 2 is the swallowed READ probe,
    // block 3 the write that followed it.  A reader takes the highest-numbered block.
    const size_t b2 = after.find("[crash] block 2 ");
    const size_t b3 = after.find("[crash] block 3 ");
    REQUIRE(b2 != std::string::npos);
    REQUIRE(b3 != std::string::npos);
    CHECK(b2 < b3);
    CHECK(after.find("[crash] access=read va=0x0000000000000000", b2) < b3);
    CHECK(after.find("[crash] access=write va=0x0000000000000000", b3) != std::string::npos);
    // And the cap holds: eight blocks, then the ninth swallowed probe costs nothing.
    for (int i = 0; i < 8; ++i) CHECK(RunOnThread(ProbeFaultThread, nullptr) == 1);
    CHECK(PawnProvision::CrashBlocksWritten() == 8);
    CHECK(CountOf(diag.Read(), "[crash] end block 9") == 0);

    // Every line of the block was flushed, not left in a buffer for a process that is dying.
    CHECK(PawnProvision::CrashLogFlushes() > flushesAfterMarker);
}

TEST_CASE("the unhandled arm is installed and hands the engine's own filter back") {
    DiagFile diag(L"replay_pawn_uef.log");
    // SetUnhandledExceptionFilter answers the filter it replaced, so installing null and putting
    // the answer straight back reads the current one without raising anything.
    LPTOP_LEVEL_EXCEPTION_FILTER before = SetUnhandledExceptionFilter(nullptr);
    SetUnhandledExceptionFilter(before);

    REQUIRE(PawnProvision::InstallCrashLogger(diag.path));
    LPTOP_LEVEL_EXCEPTION_FILTER during = SetUnhandledExceptionFilter(nullptr);
    SetUnhandledExceptionFilter(during);
    CHECK(during != before);   // the commit arm is on

    PawnProvision::RemoveCrashLogger();
    LPTOP_LEVEL_EXCEPTION_FILTER after = SetUnhandledExceptionFilter(nullptr);
    SetUnhandledExceptionFilter(after);
    // CHAINED, not clobbered: whatever the engine had installed is what the process is left with.
    CHECK(after == before);
}

// AUD-W2-P1-28 F2 -- unit-green cannot see wiring.  RemoveCrashLogger had no product caller at
// 5489e1c and the header claimed one, so the claim is now a check: the FreeLibrary route in
// dllmain.cpp is read from the source tree and must still name it.  Deleting the call turns this
// red, which is the whole point -- a vectored handler left in the chain after our pages are
// unmapped kills the process on the next exception of any kind.
TEST_CASE("the FreeLibrary detach route calls RemoveCrashLogger") {
    std::string text;
    {
        FILE* f = fopen(REPLAY_SOURCE_ROOT "/src/plugin/dllmain.cpp", "rb");
        REQUIRE(f != nullptr);
        char buf[4096];
        size_t got = 0;
        while ((got = fread(buf, 1, sizeof buf, f)) != 0) text.append(buf, got);
        fclose(f);
    }
    const size_t call = text.find("RemoveCrashLogger()");
    REQUIRE(call != std::string::npos);
    // Under the FreeLibrary arm specifically -- lpReserved null -- and not on the terminating one.
    const size_t guard = text.rfind("reserved == nullptr", call);
    REQUIRE(guard != std::string::npos);
    CHECK(call - guard < 80);
}

// AUD-DIAG-FIX-HEAP B1 -- the logger had ONE caller, PawnProvisioner::Arm, which the playback
// module reaches only behind `!g.loaded` and in-match.  A CAPTURE process arms no provisioner, so
// it ran with no VEH and no UEF and L2 #17b's 0xC0000374 was swallowed by the OS with nothing
// written.  The install is now a module of its own, run by modreg::InitAll on the bootstrap worker
// in every session mode; this reads that out of the source tree so deleting it goes red.
TEST_CASE("a module of our own installs the crash logger, before every other module") {
    std::string text;
    {
        FILE* f = fopen(REPLAY_SOURCE_ROOT "/src/engine/pawn_provision.cpp", "rb");
        REQUIRE(f != nullptr);
        char buf[4096];
        size_t got = 0;
        while ((got = fread(buf, 1, sizeof buf, f)) != 0) text.append(buf, got);
        fclose(f);
    }
    const size_t reg = text.find("REPLAY_MODULE(\"crashlog\"");
    REQUIRE(reg != std::string::npos);
    // The init it names must be the one that installs, and it must install unconditionally: a
    // second `if` here is how a capture session got left out the first time.
    const size_t init = text.find("bool CrashLogModuleInit(");
    REQUIRE(init != std::string::npos);
    const size_t call = text.find("EnsureCrashLogger(LogPath()", init);
    REQUIRE(call != std::string::npos);
    const size_t close = text.find("}", init);
    REQUIRE(call < close);
    CHECK(text.find("if (", init) > close);
    // Ordered ahead of every other module, which is what makes "before any module can fault" true.
    CHECK(text.find("kModuleOrderCrashLog < kModuleOrderCapture") != std::string::npos);
}

// The wiring the packet exists for, driven rather than read: a CAPTURE-mode process -- no demo
// loaded, no PawnProvisioner ever armed, so `Arm` never runs -- must still take a real access
// violation and write a numbered block.  `first-chance` is the VEH's word: only the vectored
// handler sees an exception a downstream __except goes on to swallow.
TEST_CASE("a capture-mode session logs a swallowed access violation") {
    DiagFile diag(L"replay_pawn_capture_crash.log");
    REQUIRE(PawnProvision::EnsureCrashLogger(diag.path, "capture"));
    // Once per process: Arm's own call, and any second worker, must change nothing and log nothing.
    CHECK_FALSE(PawnProvision::EnsureCrashLogger(diag.path, "capture"));
    CHECK_FALSE(PawnProvision::InstallCrashLogger(diag.path));

    CHECK(RunOnThread(FaultThread, nullptr) == 1);   // the __except swallows it, as a live one would
    const std::string log = diag.Read();
    CHECK(CountOf(log, "[crash] block 1 first-chance code=0xc0000005") == 1);
    CHECK(CountOf(log, "[crash] site=") == 1);
    CHECK(CountOf(log, "[crash] end block 1") == 1);
}

TEST_CASE("the crash filter passes the engine's benign first-chance codes through unlogged") {
    DiagFile diag(L"replay_pawn_filter.log");
    REQUIRE(PawnProvision::InstallCrashLogger(diag.path));

    // 0xE06D7363 is every C++ throw in the engine and 0x406D1388 is every thread that gets named.
    // A logger that wrote a block for these would bury the one block that matters.
    for (uintptr_t code : {(uintptr_t)0xE06D7363u, (uintptr_t)0x406D1388u, (uintptr_t)0x40010006u})
        CHECK(RunOnThread(RaiseCodeThread, reinterpret_cast<void*>(code)) == 1);

    CHECK(CountOf(diag.Read(), "[crash] ") == 0);

    // And the pass-through did not spend the one-block flag: a real fault after them still logs.
    CHECK(RunOnThread(FaultThread, nullptr) == 1);
    CHECK(CountOf(diag.Read(), "[crash] block 1 first-chance code=0xc0000005") == 1);
}

// L2 #17b: a capture session died at +6.3 s with 0xC0000374 and the logger wrote NOTHING, because
// the two NTSTATUS reports had no EXCEPTION_* spelling and so were never put on the list.  A status
// code alone names no module, no thread and no stack.  The heap manager raises 0xC0000374 through
// RtlReportCriticalFailure as an ordinary exception, so the dispatcher does reach a vectored
// handler -- these raises stand in for that dispatch, which is the only part this test can drive.
TEST_CASE("the crash filter logs the heap-corruption and fastfail status codes") {
    CHECK(PawnProvision::CrashCodeIsFatal(0xC0000374u));
    CHECK(PawnProvision::CrashCodeIsFatal(0xC0000409u));
    CHECK(!PawnProvision::CrashCodeIsFatal(0xE06D7363u));

    DiagFile diag(L"replay_pawn_heapfilter.log");
    REQUIRE(PawnProvision::InstallCrashLogger(diag.path));

    CHECK(RunOnThread(RaiseCodeThread, reinterpret_cast<void*>((uintptr_t)0xC0000374u)) == 1);
    CHECK(CountOf(diag.Read(), "[crash] block 1 first-chance code=0xc0000374") == 1);

    CHECK(RunOnThread(RaiseCodeThread, reinterpret_cast<void*>((uintptr_t)0xC0000409u)) == 1);
    CHECK(CountOf(diag.Read(), "[crash] block 2 first-chance code=0xc0000409") == 1);

    // The block is only worth writing if it carries the frames: `site=` is what turns a status code
    // into a module and an offset.
    CHECK(CountOf(diag.Read(), "[crash] site=") == 2);
    CHECK(CountOf(diag.Read(), "[crash] end block 2") == 1);
}

TEST_CASE("every hop call writes exactly one marker, before the call, in the documented format") {
    DiagFile diag(L"replay_pawn_marker.log");
    REQUIRE(PawnProvision::InstallCrashLogger(diag.path));

    struct Surface {
        int  ctxObject = 0;
        bool registered = true;
        bool enabled = false;
        bool placed = false;
        bool ci = true;
        uint8_t team = 1;
        int  enableCalls = 0;
        int  placeCalls = 0;
    } surface;

    PawnProvision::ActorHops h{};
    h.ctx = &surface;
    h.inRemotePlayerCs = [](void* c, uint16_t) { return static_cast<Surface*>(c)->registered; };
    h.enabled = [](void* c, uint16_t) { return static_cast<Surface*>(c)->enabled; };
    h.placed = [](void* c, uint16_t) { return static_cast<Surface*>(c)->placed; };
    h.physicsCi = [](void* c, uint16_t) { return static_cast<Surface*>(c)->ci; };
    h.isLocalUser = [](void*, uint16_t) { return PawnProvision::LocalUserAnswer::Remote; };
    h.writeSpawned = [](void* c, uint16_t) {
        Surface* s = static_cast<Surface*>(c);
        ++s->enableCalls;
        s->placed = true;
        return PawnProvision::SpawnedWrite::Issued;
    };
    h.assignTeam = [](void* c, uint16_t, uint16_t role) {
        Surface* s = static_cast<Surface*>(c);
        ++s->placeCalls;
        s->team = uint8_t(role);
        s->enabled = true;
        return true;
    };
    h.teamOf = [](void* c, uint16_t, uint8_t* id, uint8_t* slot) {
        *id = static_cast<Surface*>(c)->team;
        *slot = 0;
        return true;
    };

    PawnProvision::DriveGate gate;
    gate.epoch = 3;
    PawnProvision::HopDrive drive;
    drive.gate = &gate;
    drive.recordedRole = 0;
    CHECK(PawnProvision::AdvanceActorHops(h, 1, &drive) == PawnProvision::Hop::Done);
    CHECK(drive.outcome.state == PawnProvision::Participation::Issued);
    CHECK(surface.enableCalls == 1);
    CHECK(surface.placeCalls == 1);

    const std::string log = diag.Read();

    // THE FORMAT, character for character, with the arguments the call was actually made with.
    char expect[160];
    snprintf(expect, sizeof expect, "[pawn] hop 1 inRemotePlayerCs: ctx=0x%016llx index=0x0001\r\n",
             (unsigned long long)(uintptr_t)&surface);
    CHECK(CountOf(log, expect) == 1);

    // ONE marker per call and no more: ten calls were made on this pass -- the registration read,
    // A0's partition read, A2's write and its re-read, A3's word read / assign / re-read, and the
    // three postcondition reads the chain answers on.
    CHECK(CountOf(log, "[pawn] hop ") == 10);
    for (const char* call : {"hop 1 inRemotePlayerCs:", "hop 0 a0 isLocalUser:",
                             "hop 0 a2 writeSpawned:", "hop 0 a2 spawned:", "hop 0 a3 teamOf:",
                             "hop 0 a3 assignTeam:", "hop 0 a3 teamOf#2:", "hop 2 enabled:",
                             "hop 3 placed:", "hop 4 physicsCi:"})
        CHECK(CountOf(log, call) == 1);

    // The marker is written BEFORE its call, so the last line names the call in flight rather than
    // the last one that returned.  A second pass over a chain that is already done makes exactly
    // five calls -- the gate has latched, so no engine call is issued -- and leaves `physicsCi`
    // last.
    const uint64_t flushesBefore = PawnProvision::CrashLogFlushes();
    PawnProvision::HopDrive again;
    again.gate = &gate;
    again.recordedRole = 0;
    CHECK(PawnProvision::AdvanceActorHops(h, 1, &again) == PawnProvision::Hop::Done);
    CHECK(again.outcome.state == PawnProvision::Participation::SkippedAlreadyDone);
    CHECK(PawnProvision::CrashLogFlushes() == flushesBefore + 5);
    CHECK(strstr(PawnProvision::LastHopMarker(), "hop 4 physicsCi:") ==
          PawnProvision::LastHopMarker());

    // Every marker line was flushed as it was written.
    CHECK(PawnProvision::CrashLogFlushes() == 15);
}

// And this case is AUD-W2-P1-29 F1's marker count RE-MEASURED after hop 4 was
// bound.  The shipped shape now binds BOTH hop-4 hooks (identity_access.cpp reads the RemotePlayer
// CI's own requirement words and its eight-byte init witness), so a driving pass emits the tenth
// marker the old shape could not and the chain can reach Done.  The number an L3 runner greps is
// measured here rather than predicted, over the shipped SHAPE, so the report's number cannot drift
// away from the build's.
TEST_CASE("The SHIPPED shape emits ten markers on a driving pass, then five") {
    DiagFile diag(L"replay_pawn_marker_shipped.log");
    REQUIRE(PawnProvision::InstallCrashLogger(diag.path));
    PawnProvision::ResetHopMarkerBudget();

    struct Surface {
        bool placed = false;
        bool built = false;
        uint8_t team = 1;
    } surface;

    PawnProvision::ActorHops h{};
    h.ctx = &surface;
    h.inRemotePlayerCs = [](void*, uint16_t) { return true; };
    h.enabled = [](void*, uint16_t) { return true; };
    h.placed = [](void* c, uint16_t) { return static_cast<Surface*>(c)->placed; };
    h.isLocalUser = [](void*, uint16_t) { return PawnProvision::LocalUserAnswer::Remote; };
    h.writeSpawned = [](void* c, uint16_t) {
        static_cast<Surface*>(c)->placed = true;
        return PawnProvision::SpawnedWrite::Issued;
    };
    h.assignTeam = [](void* c, uint16_t, uint16_t role) {
        static_cast<Surface*>(c)->team = uint8_t(role);
        return true;
    };
    h.teamOf = [](void* c, uint16_t, uint8_t* t, uint8_t* s) {
        *t = static_cast<Surface*>(c)->team;
        *s = 0;
        return true;
    };
    // ...and hop 4 is BOUND, which is the whole point of the case: the shipped surface binds both
    // hooks, so this fixture binds both too or it is not driving the shipped shape.  `built` is
    // flipped between the two passes below, which is what makes this a re-read and not a latch.
    h.physicsCi = [](void* c, uint16_t) { return static_cast<Surface*>(c)->built; };
    h.physicsCiAwaiting = [](void* c, uint16_t) {
        return static_cast<Surface*>(c)->built ? PawnProvision::PhysicsCiReq::None
                                               : PawnProvision::PhysicsCiReq::R15NetPhysics;
    };
    REQUIRE(h.physicsCi != nullptr);
    REQUIRE(h.physicsCiAwaiting != nullptr);

    PawnProvision::DriveGate gate;
    gate.epoch = 3;
    PawnProvision::HopDrive drive;
    drive.gate = &gate;
    drive.recordedRole = 0;
    // Pass one: the CI is not built yet, so the chain stops at hop 4 -- and it MARKED hop 4,
    // which the unbound shape could never do.
    CHECK(PawnProvision::AdvanceActorHops(h, 1, &drive) == PawnProvision::Hop::PhysicsCi);
    CHECK(drive.outcome.state == PawnProvision::Participation::Issued);
    // Eleven, not ten: hop 4's requirement word is read HERE now, inside the
    // one pass, instead of by a caller asking the same pool over again afterwards.
    CHECK(CountOf(diag.Read(), "[pawn] hop ") == 11);
    CHECK(CountOf(diag.Read(), "hop 4 physicsCi:") == 1);
    CHECK(CountOf(diag.Read(), "hop 4 physicsCiAwaiting:") == 1);
    CHECK(drive.physicsCiWhy == PawnProvision::PhysicsCiReq::R15NetPhysics);

    // Pass two: the engine finished the init BETWEEN the two reads, and the chain reaches Done off
    // the RE-READ.  A chain that trusted pass one's answer would still be refusing.
    const uint64_t ten = PawnProvision::CrashLogFlushes();
    surface.built = true;
    PawnProvision::HopDrive again;
    again.gate = &gate;
    again.recordedRole = 0;
    CHECK(PawnProvision::AdvanceActorHops(h, 1, &again) == PawnProvision::Hop::Done);
    CHECK(again.outcome.state == PawnProvision::Participation::SkippedAlreadyDone);
    CHECK(PawnProvision::CrashLogFlushes() == ten + 5);
    CHECK(strstr(PawnProvision::LastHopMarker(), "hop 4 physicsCi:") ==
          PawnProvision::LastHopMarker());
}

// `provisioned` could not move in the shipped DLL while hop 4 was unbound -- the
// chain returned Hop::PhysicsCi forever and AdvanceSlot's `++ledger_.provisioned` was unreachable.
// This drives the whole pump over a record that FLIPS BETWEEN READS, which is the only shape that
// can tell a re-read from a latched first answer: the same slot is refused by name on one pass and
// provisioned on the next, with no second insert and no second participation.
TEST_CASE("A CI that arrives between two reads carries the slot to Done") {
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    SetState(facts::NETGAME_STATE_INMATCH);

    FakeIdentity id;
    id.Place(0, 7, true);   // the viewer holds slot 0, as INV-PP1 requires it to

    FakeHopEngine eng;
    eng.inCs = true;
    eng.enabled = true;
    eng.spawned = false;
    eng.physCi = false;          // the component instance has not been built yet
    PawnProvision::ActorHops hopSurface = HopsFor(eng);

    CaptureLog log;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.log  = &log;
    env.hops = &hopSurface;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), JoinDesc{}));

    const uint32_t kSettle = 3 * PawnProvisioner::kRetryTicks + 4;
    for (uint32_t i = 0; i < kSettle; ++i) p.Pump();
    REQUIRE(id.inserts == 1);
    CHECK(p.Ledger().provisioned == 0);
    // The hop-4 refusal is named in the ledger line -- RefusalText leads with the enum's own
    // spelling -- rather than read off SlotRefusal, which carries whatever the LAST tick answered
    // and on a not-due tick that is the cadence, not the hop.
    CHECK(log.Count("NoPhysicsCi:") >= 1);
    CHECK(log.Count("is awaiting hop 'PhysicsCi'") >= 1);
    // The census names the requirement the engine's own record is short of, and says so in the
    // window line's own field.
    CHECK(log.Count("PhysicsCiAwaiting(R15NetPhysics)") >= 1);
    CHECK(log.Count("physicsCi=R15NetPhysics") >= 1);
    CHECK(log.Count("physicsCi=none") == 0);

    // THE ENGINE FINISHES THE INIT.  Nothing else changes -- no new request, no second insert.
    eng.physCi = true;
    const uint32_t insertsBefore = id.inserts;
    const uint32_t assignsBefore = eng.assigns;
    for (uint32_t i = 0; i < 2 * PawnProvisioner::kRetryTicks + 2; ++i) p.Pump();

    CHECK(p.Ledger().provisioned == 1);
    CHECK(p.Provisioned(UserSlot(0)));
    CHECK(p.Ledger().adopted == 0);              // INV-PP1 is unaffected by hop 4 reaching Done
    CHECK(id.inserts == insertsBefore);          // the answer came off the record, not off a call
    CHECK(eng.assigns == assignsBefore);
    CHECK(log.Count("physicsCi=none") >= 1);     // and the census says the CI is built
}

TEST_CASE("the per-tick marker budget bounds a settled loop and says when it runs out") {
    DiagFile diag(L"replay_pawn_budget.log");
    REQUIRE(PawnProvision::InstallCrashLogger(diag.path));
    PawnProvision::ResetHopMarkerBudget();

    // ReopenAttemptWindow makes every slot due again the moment a provision
    // lands, so one tick can drive a dozen hop chains at ~9 flushed writes each -- on the game
    // thread, inside the window RUN-27 narrowed the fault to.
    // The budget is asked of the module, never spelled here: AUD-W2-P1-30 F2 is a budget that had
    // drifted BELOW what one driven pass issues, and a test carrying its own 16 could not see it.
    const unsigned budget = PawnProvision::HopMarkerBudget();
    // It must cover the whole of one driven pass -- the hop chain AND every mark DressLoadout
    // makes -- or the last-marker field freezes on an L1 hash while the fault lands in the apply.
    CHECK(budget >= 10u + replay::identity_access::kLoadoutMaxMarksPerPass);
    const unsigned over = budget + 8;
    for (unsigned i = 0; i < over; ++i)
        PawnProvision::MarkHop(1, "inRemotePlayerCs", nullptr, uint16_t(i));
    const std::string log = diag.Read();
    CHECK(CountOf(log, "[pawn] hop ") == size_t(budget));
    char suppressed[96];
    snprintf(suppressed, sizeof suppressed,
             "markers suppressed for the rest of this tick (budget %u)", budget);
    CHECK(CountOf(log, suppressed) == 1);
    // Every marker plus the one suppression line, each flushed as it was written.
    CHECK(PawnProvision::CrashLogFlushes() == uint64_t(budget) + 1);

    // The pump reopens it, so the budget is per tick and not per session.
    PawnProvision::ResetHopMarkerBudget();
    PawnProvision::MarkHop(1, "inRemotePlayerCs", nullptr, 0);
    CHECK(CountOf(diag.Read(), "[pawn] hop ") == size_t(budget) + 1);
}

TEST_CASE("no marker and no handler until the logger is installed") {
    DiagFile diag(L"replay_pawn_none.log");
    PawnProvision::RemoveCrashLogger();
    // The absence of a HANDLER, not the absence of a counter: a handler left
    // registered by an earlier case would satisfy every other assertion in this case.  A real
    // access violation is taken and the file it would have been written to must not exist.
    CHECK(RunOnThread(FaultThread, nullptr) == 1);
    CHECK(diag.Read().empty());
    CHECK(PawnProvision::CrashBlocksWritten() == 0);
    CHECK(PawnProvision::CrashLogFlushes() == 0);
    PawnProvision::MarkHop(1, "inRemotePlayerCs", nullptr, 0);
    CHECK(PawnProvision::CrashLogFlushes() == 0);
    CHECK(PawnProvision::LastHopMarker()[0] == '\0');
    // An empty path is not an install: a logger pointed at nothing must say so rather than
    // claiming a file it never opened.
    CHECK_FALSE(PawnProvision::InstallCrashLogger(L""));
    CHECK_FALSE(PawnProvision::InstallCrashLogger(nullptr));
}

// =============================================================================================
// W2-P1-30d -- THE DRESS, DRIVEN.  ARCH-23 S14.8/S14.8b/S14.8c.  Until this block nothing in the
// shipped build called DressLoadout: L1..L6 existed and no pass issued them.  These cases drive
// the sequence through PawnProvisioner with fakes of the four engine calls and of the recording's
// own reader, and they assert the two things a green unit test cannot see -- that the dress runs
// at all, and that it runs in the right frame.
// =============================================================================================
namespace loadoutdrive {

using replay::identity_access::LoadoutEngine;
using replay::identity_access::LoadoutPair;
namespace ia = replay::identity_access;

// The engine's name -> Symbol64, faked: any function will do provided the fixture applies the
// SAME one to the document's values and to the recorded item ids, because what L1 verifies is
// that those two agree -- not what the hash of a particular string is.
uint64_t FakeHash(const char* s, uint64_t, int32_t, int32_t, void*) {
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); p && *p; ++p) {
        h ^= *p;
        h *= 1099511628211ull;
    }
    return h ? h : 1;   // never 0: L1 treats 0 as padding and refuses SentinelCollision
}

// The recorded document: one child per slot name, each a String node carrying an item name.
struct DocFake {
    char value[20][40] = {};
    void* node[20] = {};
    uint16_t children = 20;
};
DocFake* g_doc = nullptr;

void* DocPathElem(void*, const char*, void* prev) {
    size_t i = 0;
    if (prev != nullptr) {
        while (i < g_doc->children && g_doc->node[i] != prev) ++i;
        ++i;
    }
    return i < g_doc->children ? g_doc->node[i] : nullptr;
}
const char* DocName(void*, void* node) {
    for (size_t i = 0; i < 20; ++i)
        if (g_doc->node[i] == node) return ia::LoadoutSlotName(uint16_t(i));
    return nullptr;
}
const char* DocString(void*, void* node, const char*) {
    for (size_t i = 0; i < 20; ++i)
        if (g_doc->node[i] == node) return g_doc->value[i];
    return nullptr;
}
uint32_t DocKind(void*, void*) { return facts::IDENT_JSON_KIND_STRING; }
bool DocReadable(const void*, size_t) { return true; }
int64_t DocPathCount(void*, const char*) { return g_doc->children; }

uint64_t InstanceSymbol() {
    char seg[64] = {0};
    if (!ia::LoadoutTemplateInstance(seg, sizeof seg)) return 0;
    return FakeHash(seg, 0, 0, 0, nullptr);
}

// The four engine calls plus the two read-backs, each recording that it ran and in what order.
struct EngineFake {
    bool bind = true;
    std::string order;
    void* attached = nullptr;
    LoadoutPair applied[20] = {};
    ia::AppliedSlotState appliedState[20] = {};
    uint64_t appliedCount = 0;
    unsigned applyItemCalls = 0;
    unsigned setStringCalls = 0;   // must stay 0: L1 verifies and authors nothing
    // L7's own drive, counted apart from L6's alignment re-apply so a test can see the model step
    // re-fire the engine's productive apply exactly once, precondition-gated (+56>0), without
    // confusing it with the dress's L6.  The re-point (DIAG-MODEL-INSTANTIATE Lever A) drives
    // LoadoutApplyAlignment ONCE per undressed-and-ready window, not once per recorded pair.
    unsigned driveModelCalls = 0;
    bool driveModelOk = true;
    bool alignOk = true;
    // The gate's SECOND operand.  `entityAlignBound` false is an image where
    // IDENT.CUSTOMISATION_ALIGNMENT did not resolve; `entityAlignOk` false is one where the
    // component would not answer.  They are different refusals and the rig can plant either.
    uint16_t entityAlign = ia::kSlotAlignmentNone;
    bool entityAlignOk = true;
    bool entityAlignBound = true;
    unsigned entityAlignCalls = 0;
    uint16_t descAlignment = ia::kSlotAlignmentNone;   // what the RECORDING holds
    int unfilledRow = -1;   // a recorded row the player wore NOTHING in (the engine -1)
    unsigned publishNoOps = 0;    // windows on which the publish call leaves the live table alone
    // The engine's own apply took one of its two SILENT arms off the enable: the read-back holds
    // nothing until OUR apply has run.  0 leaves the applied set readable from the start.
    unsigned appliedFromApplyCall = 0;
    unsigned attachBlindWindows = 0;   // windows on which the attach does not read back
    unsigned unusableWindows = 0;      // windows on which the profile is not yet usable
    // THE FOUR PRE-ENABLE GATES this fake hands back.  Unbound by default, which is what an
    // image with no IDENT.ACTOR_REF_IS_VALID_VA answers -- and `Unasked` must never hold a slot.
    bool gatesBound = false;
    ia::LoadoutApplyGates gates{};
    unsigned gateCalls = 0;
    // The re-seat freed the instance the dress installed: the dress gate VERDICT is Ready (the enable
    // issues) but its pair count reads 0 by the model step.  The model step's rebuild re-publishes;
    // the Nth publish (the rebuild, past the dress's own publishes) refills the instance.
    bool rebuildFillsInstance = false;
    unsigned refillAfterPublishCount = 0;
    unsigned publishCalls = 0;
    // Keys the model-step pair count on the READ slot, so a remap (recorded != resolved) test can
    // pin which slot the gate is asked at -- the recorded slot or the mapped one.
    bool gateKeysOnSlot = false;
    uint64_t slotPairs[facts::ENTITY_USER_SLOT_TABLE_MAX] = {};
};
EngineFake* g_eng = nullptr;

void* EngAttach(void*, uint16_t, void* doc) { g_eng->order += "attach;"; g_eng->attached = doc; return doc; }
void* EngAttachedDoc(void*, uint16_t) {
    if (g_eng->attachBlindWindows > 0) {
        --g_eng->attachBlindWindows;
        return nullptr;
    }
    return g_eng->attached;
}
bool  EngDocUsable(void*, uint16_t) {
    if (g_eng->unusableWindows > 0) {
        --g_eng->unusableWindows;
        return false;
    }
    return g_eng->attached != nullptr;
}
void  EngParse(void* container, void*) {
    g_eng->order += "parse;";
    ia::LoadoutHeader h{};
    h.instanceCount = 1;
    h.number = 3;
    h.numberBody = 7;
    memcpy(container, &h, sizeof h);
}
// The parser over a document that carries no loadout instances: the header pair reads back but
// instanceCount is 0, the exact input L3's #18/#21 emptiness gate must still refuse.
void  EngParseEmpty(void* container, void*) {
    g_eng->order += "parse;";
    ia::LoadoutHeader h{};
    h.instanceCount = 0;
    h.number = 3;
    h.numberBody = 7;
    memcpy(container, &h, sizeof h);
}
void* EngPublish(void* dst, const void* src) {
    g_eng->order += "publish;";
    // The call ran and the live table did not change: L4's read-back is what says so, and it is
    // the state the enable must not be issued into.
    if (g_eng->publishNoOps > 0) {
        --g_eng->publishNoOps;
        return dst;
    }
    memcpy(dst, src, sizeof(ia::LoadoutHeader));
    // The rebuild is the Nth publish (past the dress's own): it refills the name-matched instance.
    ++g_eng->publishCalls;
    if (g_eng->rebuildFillsInstance && g_eng->refillAfterPublishCount > 0 &&
        g_eng->publishCalls >= g_eng->refillAfterPublishCount)
        g_eng->gates.pairs = 20;
    return dst;
}
void EngApplyItems(void*, uint16_t) { g_eng->order += "apply;"; ++g_eng->applyItemCalls; }
bool EngApplyAlignment(void*, uint16_t) { g_eng->order += "align;"; return g_eng->alignOk; }
bool EngDriveModel(void*, uint16_t) {
    g_eng->order += "model;";
    ++g_eng->driveModelCalls;
    return g_eng->driveModelOk;
}
bool EngEntityAlignment(void*, uint16_t, uint16_t& out) {
    ++g_eng->entityAlignCalls;
    if (!g_eng->entityAlignOk) return false;
    out = g_eng->entityAlign;
    return true;
}
bool EngReadApplied(void*, uint16_t, LoadoutPair out[20], uint64_t& n,
                    ia::AppliedSlotState state[20]) {
    g_eng->order += "readback;";
    if (g_eng->applyItemCalls < g_eng->appliedFromApplyCall) {
        for (size_t i = 0; i < 20; ++i) {
            out[i] = LoadoutPair{};
            state[i] = ia::AppliedSlotState{};
        }
        n = 0;
        return true;
    }
    for (size_t i = 0; i < 20; ++i) {
        out[i] = g_eng->applied[i];
        state[i] = g_eng->appliedState[i];
    }
    n = g_eng->appliedCount;
    return true;
}

entity_json::CJsonReader g_reader{};

// The four gates, as an image that CAN ask them answers.  A test that wired the real reader here
// would be handed `Unasked` for every case -- the one value the deferrals must not be built on.
ia::LoadoutApplyGate EngGates(void* ng, uint16_t slot, uint16_t index, ia::LoadoutApplyGates& out) {
    (void)ng; (void)index;
    ++g_eng->gateCalls;
    out = g_eng->gates;
    // A slot-blind fake cannot tell the recorded slot from the resolved one; keying the pair
    // count on the read slot pins WHICH slot the model step asks its instance question at.
    if (g_eng->gateKeysOnSlot)
        out.pairs = slot < facts::ENTITY_USER_SLOT_TABLE_MAX ? g_eng->slotPairs[slot] : 0;
    return out.verdict;
}

bool ProvideEngine(LoadoutEngine& e) {
    if (!g_eng->bind) return false;
    e = LoadoutEngine{};
    e.doc = &g_reader;
    e.hash = &FakeHash;
    e.instanceSymbol = &InstanceSymbol;
    e.attach = &EngAttach;
    e.attachedDoc = &EngAttachedDoc;
    e.docUsable = &EngDocUsable;
    e.parse = &EngParse;
    e.publish = &EngPublish;
    e.actorEnabled = nullptr;   // filled below from hop 2's own pointer
    e.applyItems = &EngApplyItems;
    e.applyAlignment = &EngApplyAlignment;
    e.driveModel = &EngDriveModel;
    e.readAppliedPairs = &EngReadApplied;
    e.entityAlignment = g_eng->entityAlignBound ? &EngEntityAlignment : nullptr;
    e.applyGates = g_eng->gatesBound ? &EngGates : nullptr;
    return true;
}

// Hop 2's own read, handed to the surface exactly as BindLoadoutEngine hands it: the same pointer
// and the same ctx the chain uses, so the gate and the hop cannot answer differently.
PawnProvision::ActorHops* g_hops = nullptr;
bool ProvideEngineWithHopGate(LoadoutEngine& e) {
    if (!ProvideEngine(e)) return false;
    e.actorCtx = g_hops->ctx;
    e.actorEnabled = g_hops->enabled;
    return true;
}

// The recording, as playback's reader answers it.
struct RecordedFake {
    replay::identity::RecordedLoadout rl{};
    bool ok = true;
    const char* why = "IdentityBlockNotRecorded";
    unsigned asks = 0;
    uint32_t sawSlot = ~uint32_t(0);   // no slot yet -- derived, never transcribed
    // Which recorded slot the file has a block for.  The sentinel means "every slot", which is
    // what every case written before the census counted entrants assumed.
    uint32_t onlySlot = ~uint32_t(0);
};
RecordedFake* g_rec = nullptr;
bool ProvideRecorded(uint32_t slot, replay::identity::RecordedLoadout& out, const char** why) {
    ++g_rec->asks;
    g_rec->sawSlot = slot;
    if (!g_rec->ok || (g_rec->onlySlot != ~uint32_t(0) && slot != g_rec->onlySlot)) {
        if (why) *why = g_rec->why;
        return false;
    }
    out = g_rec->rl;
    return true;
}

// A netgame with room for every offset L3/L4 address, planted the way BuildRig plants it.  The
// pawn suite's own gNetGame is 128 bytes; the publish writes 64 bytes at IDENT.LOADOUT_TABLE_OFF.
std::string* g_ngMem = nullptr;
uint32_t g_sessionFlags = 0;
void* BigNetGame() { return g_ngMem != nullptr ? &(*g_ngMem)[0] : nullptr; }

void BuildBigNetGame(std::string& mem) {
    const size_t stride = size_t(facts::USER_PROFILE_STRIDE);
    size_t need = size_t(facts::USER_PROFILE_OFF) +
                  stride * size_t(facts::ENTITY_USER_SLOT_TABLE_MAX) +
                  size_t(facts::USER_LOADOUT_OFF) + 256;
    const size_t live = size_t(facts::IDENT_LOADOUT_TABLE_OFF) +
                        (size_t(facts::ENTITY_USER_SLOT_TABLE_MAX)
                             << facts::IDENT_LOADOUT_ENTRY_SHIFT) + 256;
    if (live > need) need = live;
    const size_t flagEnd = stride * size_t(facts::ENTITY_USER_SLOT_TABLE_MAX) +
                           size_t(facts::IDENT_LOADOUT_PUBLISHED_FLAG_OFF) + 256;
    if (flagEnd > need) need = flagEnd;
    mem.assign(need, char(0));
    // The lobby chain and the state word SetUpLobby planted, carried over: this buffer replaces
    // the suite's 128-byte netgame, and a provisioner that could not read the state or the lobby
    // would refuse long before the dress.
    memcpy(&mem[0], gNetGame, sizeof gNetGame);
    g_sessionFlags = 0;
    uint32_t* flags = &g_sessionFlags;
    memcpy(&mem[size_t(facts::NETGAME_UIFLAGS_PTR_OFF)], &flags, sizeof flags);
}

// The document, the recorded pairs and the engine's post-apply truth, all built from ONE source:
// the twenty registry slot names.  A fixture whose recorded side came from the document would be
// comparing the document with itself.
void Seed(DocFake& doc, EngineFake& eng, RecordedFake& rec, uint16_t filled) {
    for (uint16_t i = 0; i < 20; ++i) {
        snprintf(doc.value[i], sizeof doc.value[i], "item_%s", ia::LoadoutSlotName(i));
        doc.node[i] = &doc.value[i][0];
    }
    rec.rl = replay::identity::RecordedLoadout{};
    rec.rl.hasSlotKeys = true;
    rec.rl.profile = replay::identity::RecordedProfileState::Present;
    rec.rl.document = doc.value[0];
    rec.rl.documentBytes = 1;
    rec.rl.filledPairCount = filled;
    for (uint16_t i = 0; i < filled; ++i) {
        rec.rl.pairs[i].slotKey = FakeHash(ia::LoadoutSlotName(i), 0, 0, 0, nullptr);
        rec.rl.pairs[i].itemId  = FakeHash(doc.value[i], 0, 0, 0, nullptr);
    }
    // The engine leaves an entry it applied with no alignment requirement and pending == -1
    // (0x140D0D452), so every slot starts on the arm a working apply produces and a test
    // that wants one of the three silent arms has to plant it.
    for (uint16_t i = 0; i < 20; ++i) {
        eng.appliedState[i] = ia::AppliedSlotState{};
        eng.appliedState[i].pendingItem = Symbol64(~0ull);
        eng.appliedState[i].alignmentReq = ia::kSlotAlignmentNone;
    }
    for (uint16_t i = 0; i < filled; ++i) {
        eng.applied[i].slotKey = rec.rl.pairs[i].slotKey;
        eng.applied[i].itemId  = rec.rl.pairs[i].itemId;
    }
    eng.appliedCount = filled;
    g_reader = entity_json::CJsonReader{};
    g_reader.json = &doc;
    g_reader.kind = &DocKind;
    g_reader.name = &DocName;
    g_reader.stringValue = &DocString;
    g_reader.readable = &DocReadable;
    g_reader.pathElem = &DocPathElem;
    g_reader.pathCount = &DocPathCount;
}

// The JoinDesc the admission is authored from carries the container's own header pair, which L3
// reads back off the parse.  EngParse plants 3/7, so the descriptor must say 3/7.
JoinDesc DescWithNumbers() {
    JoinDesc d{};
    d.loadoutNumber = 3;
    d.loadoutNumberBody = 7;
    return d;
}

struct Wiring {
    DocFake doc;
    EngineFake eng;
    RecordedFake rec;
    std::string ng;
    Wiring() {
        g_doc = &doc;
        g_eng = &eng;
        g_rec = &rec;
        BuildBigNetGame(ng);
        g_ngMem = &ng;
    }
    ~Wiring() { g_doc = nullptr; g_eng = nullptr; g_rec = nullptr; g_ngMem = nullptr; g_hops = nullptr; }
};

}  // namespace loadoutdrive

// An Unasked re-read must not erase a NO the publish half already took.  The
// publish half and the AdvanceSlot re-read run through the same surface in this harness, so the
// merge is pinned directly -- the must-fail control REFUSAL_CLOBBERED_TO_UNASKED reverts it to a
// blind overwrite and the first case reds.
// Authoring a profile document is what gives an entrant a loadout INSTANCE when the
// recorder could not capture one, and each of the four nos has to be able to fail on its own.
TEST_CASE("A document is authored only for a slot that needs one and can have one") {
    constexpr uint16_t kUnknown = replay::identity::kRecordedPairCountUnknown;
    CHECK(PawnProvision::ShouldAuthorDocument(true, false, true, 20));
    CHECK(PawnProvision::ShouldAuthorDocument(true, false, true, 1));
    // playback.dress_loadout off: nothing is authored, which is the whole of the switch.
    CHECK(!PawnProvision::ShouldAuthorDocument(false, false, true, 20));
    // The recorder captured the entrant's own document: it wins, always.
    CHECK(!PawnProvision::ShouldAuthorDocument(true, true, true, 20));
    // A v1 block records no slot keys, so a pair would be a position and a position is a guess.
    CHECK(!PawnProvision::ShouldAuthorDocument(true, false, false, 20));
    // Nothing recorded, and "could not ask" is not the same answer as zero -- neither is dressable.
    CHECK(!PawnProvision::ShouldAuthorDocument(true, false, true, 0));
    CHECK(!PawnProvision::ShouldAuthorDocument(true, false, true, kUnknown));
}

TEST_CASE("W2-P1-45 F4: a re-read that could not ask keeps the publish-time refusal") {
    namespace ia = replay::identity_access;
    using G = ia::LoadoutApplyGate;
    // A refusal in hand, a re-read that could not put the question: the refusal stands.
    CHECK(PawnProvision::MergeGateReread(G::InstanceEmpty, G::Unasked)  == G::InstanceEmpty);
    CHECK(PawnProvision::MergeGateReread(G::ActorUnresolved, G::Unasked) == G::ActorUnresolved);
    // A re-read that actually answered is adopted -- a real yes clears the hold, a real no replaces it.
    CHECK(PawnProvision::MergeGateReread(G::InstanceEmpty, G::Ready)        == G::Ready);
    CHECK(PawnProvision::MergeGateReread(G::InstanceEmpty, G::CsPointerNull) == G::CsPointerNull);
    // No prior answer to preserve: the re-read stands even when it is Unasked.
    CHECK(PawnProvision::MergeGateReread(G::Unasked, G::Unasked) == G::Unasked);
}

// AUD-W2-P1-45 sec B: the publish census names whether the entrant's apply runs inline or is
// parked on the deferred queue.  The reader fills these observe-only fields; here the fake gates
// carry them and the census line must print inline=y and the parked count.
TEST_CASE("W2-P1-45 sec B: the census prints the inline flag and the parked-request count") {
    using namespace loadoutdrive;
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    Wiring w;
    Seed(w.doc, w.eng, w.rec, 20);
    w.eng.gatesBound = true;
    w.eng.gates.instances = 1;
    w.eng.gates.pairs = 20;
    w.eng.gates.actor = reinterpret_cast<const void*>(0x1122334455667788ull);
    w.eng.gates.compCs = reinterpret_cast<const void*>(0x99AABBCCDDEEFF00ull);
    w.eng.gates.compKey = 7;
    w.eng.gates.compValidAsked = true;
    w.eng.gates.compValid = true;
    // OBSERVE-ONLY reads the census must surface: this entrant's apply is parked, three deep.
    w.eng.gates.queueAsked = true;
    w.eng.gates.inlineApply = 0;
    w.eng.gates.queueDepth = 3;
    w.eng.gates.verdict = ia::LoadoutApplyGate::Ready;

    FakeIdentity id;
    FakeHopEngine e;
    e.inCs = true; e.enabled = true; e.physCi = true;
    PawnProvision::ActorHops h = HopsFor(e);
    g_hops = &h;
    CaptureLog log;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.netGame = &BigNetGame;
    env.hops = &h;
    env.log = &log;
    env.recordedLoadout = &ProvideRecorded;
    env.loadoutEngine = &ProvideEngineWithHopGate;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), DescWithNumbers()));
    p.Pump();
    CHECK(log.Count("compValid=1 inline=n queued=3 -> gate=Ready, before the enable") == 1);
}

// The bot Source-fill hook builds a rec with only the header pair and
// documentRecorded -- no slot keys, no pairs -- and drives DressLoadout in the Source phase.  Before
// the fix, L1 ran the recorded-pair/key fidelity (RecordedLoadoutRefusal) FIRST and refused a keyless
// rec (documentRecorded left false -> DocumentNotRecorded; even set, hasSlotKeys false ->
// SlotKeysNotRecorded), so the parser never ran and the SOURCE stayed empty -> the echo-98 empty-
// loadout deadlock.  This case pins the two edits: the Source phase now reaches eng.parse over the
// recording's own document and fills a NON-EMPTY SOURCE, while an empty or absent document is still
// refused -- no false non-empty body.
TEST_CASE("The Source phase reaches the parser and fills a non-empty SOURCE") {
    using namespace loadoutdrive;
    namespace ia = replay::identity_access;
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    Wiring w;
    Seed(w.doc, w.eng, w.rec, 20);   // a valid recorded-profile document reader

    // The engine the bot Source path binds: the document reader plus the parser, and NO hasher or
    // instance symbol.  If L1 reached the recorded-pair verification it would deref those nulls; a
    // Source phase that stops at "a document is bound" never does.
    ia::LoadoutEngine eng{};
    eng.doc   = &g_reader;
    eng.parse = &EngParse;               // plants number 3 / body 7 / instanceCount 1
    void* ng  = BigNetGame();

    // rec exactly as HookSourceLoadout builds it for a PRESENT document: the container header pair
    // and documentRecorded, and nothing else -- no slot keys, no pairs, filledPairCount 0.
    ia::RecordedLoadout recPresent{};
    recPresent.number        = 3;
    recPresent.numberBody    = 7;
    recPresent.documentRecorded = true;   // HookSourceLoadout: entrantJson.n != 0
    // The bot hook sets none of the fidelity fields: no slot keys, and the pair count stays at its
    // could-not-ask default -- exactly the rec that RecordedLoadoutRefusal would reject.
    REQUIRE_FALSE(recPresent.hasSlotKeys);
    REQUIRE(recPresent.filledPairCount == ia::kLoadoutPairCountUnknown);

    SUBCASE("present document: L1 passes without recorded pairs, the parser fills a non-empty SOURCE") {
        ia::LoadoutSteps steps{};
        const ia::LoadoutRefusal why = ia::DressLoadout(
            ng, UserSlot(0), UserIndex(0), recPresent, eng, steps, ia::LoadoutPhase::Source);
        CHECK(why == ia::LoadoutRefusal::None);
        CHECK(steps.instances == 1);   // the recorded instances the bridge copies into the live table
    }

    SUBCASE("empty document: the parser fills nothing and L3's instanceCount==0 gate still refuses") {
        ia::LoadoutEngine engEmpty = eng;
        engEmpty.parse = &EngParseEmpty;   // header pair reads back, instanceCount 0
        ia::LoadoutSteps steps{};
        const ia::LoadoutRefusal why = ia::DressLoadout(
            ng, UserSlot(0), UserIndex(0), recPresent, engEmpty, steps, ia::LoadoutPhase::Source);
        CHECK(why == ia::LoadoutRefusal::ParseDidNotFill);
        CHECK(steps.instances == ia::kLoadoutInstancesUnreadable);   // never a false non-empty SOURCE
    }

    SUBCASE("absent document: documentRecorded false is still refused DocumentNotRecorded") {
        ia::RecordedLoadout recAbsent = recPresent;
        recAbsent.documentRecorded = false;   // HookSourceLoadout: entrantJson.n == 0
        ia::LoadoutSteps steps{};
        const ia::LoadoutRefusal why = ia::DressLoadout(
            ng, UserSlot(0), UserIndex(0), recAbsent, eng, steps, ia::LoadoutPhase::Source);
        CHECK(why == ia::LoadoutRefusal::DocumentNotRecorded);
        CHECK(steps.instances == ia::kLoadoutInstancesUnreadable);
    }
}

// ==== DIAG-MODEL-BUILD fix (b): the entrant's BODY is driven and its arrival is a named fact ====
// One shared setup for the four cases, so each differs only in the postcondition surface's answers
// and the mutant it pins.  The entity is the two-read surface (ComponentOnActor via ModelVisible);
// env.entity null in every OTHER loadout case is why the model step is a no-op there and this is
// the only place it runs.
namespace modeldrive {
struct Rig {
    loadoutdrive::Wiring w;
    FakeIdentity id;
    FakeHopEngine e;
    PawnProvision::ActorHops h;
    CensusEntity ent;
    CaptureLog log;
    PawnProvisioner p;
    Rig() : h(HopsFor(e)) {
        Reset();
        SetUpLobby(0x00025F1A7C3D1A2Bull);
        // Wiring's ctor built the big netgame from gNetGame BEFORE Reset()/SetUpLobby planted the
        // INMATCH state and lobby into it, so re-seed it now.  Without this the first modeldrive
        // case in a run reads SessionNotInMatch and never attempts -- it only "passed" because an
        // earlier test happened to leave gNetGame INMATCH (the cross-test residue AUD-P1-48 found).
        loadoutdrive::BuildBigNetGame(w.ng);
        loadoutdrive::Seed(w.doc, w.eng, w.rec, 20);
        e.inCs = true; e.enabled = true; e.physCi = true;
        loadoutdrive::g_hops = &h;
        // The entrant's body is ABSENT and the viewer's (index 0) answers on its own switch; the
        // default here is undressed so the drive has a precondition to meet.
        ent.model0Resolves = false;
        ent.modelResolves = false;
    }
    ProvisionEnv Arm() {
        ProvisionEnv env = Armed(id, 2);
        env.netGame = &loadoutdrive::BigNetGame;
        env.hops = &h;
        env.entity = &ent;
        env.log = &log;
        env.recordedLoadout = &loadoutdrive::ProvideRecorded;
        env.loadoutEngine = &loadoutdrive::ProvideEngineWithHopGate;
        return env;
    }
    // Pump until `pred` holds or the retry budget is spent; returns the pumps spent.
    // The apply half and each model re-poll happen one retry-cadence window apart, so a hold that
    // resolves or bounds on the SECOND window past the drive needs several cadence windows of
    // headroom.  Bounded, so a predicate that never holds still returns.
    template <class Pred>
    uint32_t PumpUntil(Pred pred) {
        uint32_t n = 0;
        // Headroom for the full 3-rebuild path (dress publish-grace + the bounded rebuilds, each a
        // retry-cadence window apart, + the model grace) WITHOUT leaning on cross-test residue: the
        // old kRetryTicks*5+8 was marginal and only cleared with unrelated tests' process-global
        // state present.  Twelve cadence windows covers the whole path standalone.
        const uint32_t budget = PawnProvisioner::kRetryTicks * 12 + 16;
        for (uint32_t i = 0; i < budget && !pred(); ++i) { p.Pump(); ++n; }
        return n;
    }
};

// THE RE-POINT'S INVARIANT (DIAG-MODEL-INSTANTIATE Lever A): the body drive re-fires the engine's OWN
// productive apply (LoadoutApplyAlignment -> APPLY_CURRENT_LOADOUT) ONCE, gated on the engine-selected
// loadout instance's +56 itemCount being non-empty.  It is NOT once per recorded pair (Apply Chassis
// only skins an existing model-CI and never created one -- RUN-43), so a driven window means exactly
// one dispatch of the productive apply.
constexpr unsigned kDrivenOnce = 1;
}  // namespace modeldrive

// GOLDEN PATH (DIAG-MODEL-INSTANTIATE Lever A).  An entrant enabled with an UNDRESSED record has the
// engine's OWN productive apply (LoadoutApplyAlignment) re-fired ONCE; while the async mesh is in flight
// the slot HOLDS (not provisioned, ModelNotInstantiated); when ComponentOnActor flips present the body
// resolves, the slot provisions, and it is NEVER re-driven.  The gate is off here (getterless image), so
// the +56 precondition is skipped and only the undressed precondition gates the single re-fire.
TEST_CASE("W2-P1-46: undressed entrant re-fires the productive apply once, holds, then resolves") {
    using namespace loadoutdrive;
    modeldrive::Rig r;
    r.p.Arm(r.Arm());
    REQUIRE(r.p.Request(UserSlot(0), DescWithNumbers()));

    // The productive apply re-fired EXACTLY ONCE on the undressed record; the slot HOLDS while the async
    // load is in flight.  Not once per recorded pair -- Apply Chassis only skins and never created a body.
    r.PumpUntil([&] { return r.w.eng.driveModelCalls > 0; });
    CHECK(r.w.eng.driveModelCalls == modeldrive::kDrivenOnce);
    CHECK(r.log.Count("re-fire engine productive apply (LoadoutApplyAlignment)") == modeldrive::kDrivenOnce);
    CHECK(r.log.Count("productive apply dispatched (drove=1)") == modeldrive::kDrivenOnce);
    CHECK(r.p.Ledger().provisioned == 0);
    CHECK(r.p.LastProvisionRefusal() == PawnProvision::Refusal::ModelNotInstantiated);
    CHECK(r.log.Count("InstanceModel present") == 0);       // async: not the same window

    // THE MESH LANDS: ComponentOnActor flips present.  The body resolves and the pawn provisions.
    r.ent.modelResolves = true;
    r.ent.model0Resolves = true;
    r.PumpUntil([&] { return r.p.Ledger().provisioned == 1; });
    CHECK(r.p.Ledger().provisioned == 1);
    CHECK(r.log.Count("InstanceModel present (model_.cs on actor != kNoComponent)") >= 1);
    CHECK(r.log.Count("model: resolved=yes") >= 1);         // the census clause a runner greps
    CHECK(r.w.eng.driveModelCalls == modeldrive::kDrivenOnce);  // NEVER re-driven once present
}

// ENG-W2-P1-54 (DIAG-MODEL-INSTANTIATE Lever A): THE BODY RE-FIRES THE ENGINE'S PRODUCTIVE APPLY, NOT A
// DIRECT PER-PAIR APPLY CHASSIS.  Driving Apply Chassis directly (ENG-W2-P1-51/53) only skins an existing
// model-CI and its readiness gate refused silently on the entrant, so RUN-43 created no body.  The drive
// is now a SINGLE dispatch of LoadoutApplyAlignment, whose worn-record loop runs Load-and-apply +
// Apply Chassis over a non-empty instance.
TEST_CASE("W2-P1-54: the body re-fires the productive apply exactly once, never per recorded pair") {
    using namespace loadoutdrive;
    modeldrive::Rig r;
    r.p.Arm(r.Arm());
    REQUIRE(r.p.Request(UserSlot(0), DescWithNumbers()));

    r.PumpUntil([&] { return r.w.eng.driveModelCalls > 0; });
    CHECK(r.w.eng.driveModelCalls == modeldrive::kDrivenOnce);   // one dispatch, not one-per-pair
    // The old per-pair Apply Chassis marker is gone; the productive-apply marker is what a runner greps.
    CHECK(r.log.Count("undressed -> drive Apply Chassis(item=") == 0);
    CHECK(r.log.Count("re-fire engine productive apply (LoadoutApplyAlignment)") == modeldrive::kDrivenOnce);
}

// THE DRIVE IS SEQUENCED AFTER THE ENABLE.  An entrant whose enable never fires never reaches the
// model step, so nothing is driven on a stale pooled-shell record.  Pins DRIVE_BEFORE_ENABLE,
// which relocates the drive ahead of the hop chain and fires it while the actor is disabled.
TEST_CASE("W2-P1-46: the model is not driven before the entrant's actor chain is up") {
    using namespace loadoutdrive;
    modeldrive::Rig r;
    r.e.inCs = false;                                      // the actor is not even registered yet
    r.p.Arm(r.Arm());
    REQUIRE(r.p.Request(UserSlot(0), DescWithNumbers()));

    r.PumpUntil([&] { return false; });                   // spend the whole budget: the chain stalls
    CHECK(r.w.eng.driveModelCalls == 0);                  // the shipped drive waits for hop Done
    CHECK(r.p.Ledger().provisioned == 0);
}

// A BODY THAT NEVER ARRIVES is a NAMED, BOUNDED verdict, not a silent noModelOnActor.  Past the
// grace the slot provisions BODYLESS with ModelNotInstantiated.  Pins MODEL_NOT_DRIVEN (which
// drives nothing -- driveModelCalls stays 0) and PRESTAMP_CURRENT (which claims the body resolved
// without driving or reading -- no drive, no ModelNotInstantiated, provisions "dressed").
TEST_CASE("W2-P1-46: an entrant whose body never instantiates is bounded and named, then provisions") {
    using namespace loadoutdrive;
    modeldrive::Rig r;                                     // modelResolves stays false forever
    r.p.Arm(r.Arm());
    REQUIRE(r.p.Request(UserSlot(0), DescWithNumbers()));

    r.PumpUntil([&] { return r.p.Ledger().provisioned == 1; });
    CHECK(r.w.eng.driveModelCalls == modeldrive::kDrivenOnce);  // the productive apply fired once
    CHECK(r.log.Count("ModelNotInstantiated after 4 windows") >= 1);
    CHECK(r.p.Ledger().provisioned == 1);                  // bodyless, but provisioned -- not held for ever
}

// AUD-W2-P1-46 MAJOR-1.  THE CENSUS NAMES ASKED-AND-FAILED APART FROM NEVER-DRIVEN.  A slot the
// drive ran on and whose async body never landed provisions bodyless holding ModelNotInstantiated;
// the per-window census line an L3 runner greps must carry THAT word, not the noModelOnActor a
// never-driven / could-not-ask absence prints.  Pins MODEL_WHY_DEAD (which drops the held verdict
// and reverts the census to noModelOnActor).
TEST_CASE("W2-P1-46: the census reason names ModelNotInstantiated for a driven-then-bodyless slot") {
    using namespace loadoutdrive;
    modeldrive::Rig r;                                     // modelResolves stays false forever
    r.p.Arm(r.Arm());
    REQUIRE(r.p.Request(UserSlot(0), DescWithNumbers()));

    r.PumpUntil([&] { return r.p.Ledger().provisioned == 1; });
    for (int i = 0; i < 3; ++i) r.p.Pump();               // a few census windows on the bodyless slot
    CHECK(r.p.Ledger().provisioned == 1);
    CHECK(r.w.eng.driveModelCalls == modeldrive::kDrivenOnce);

    // THE POINT.  `reason=ModelNotInstantiated` rides the census line (distinct from the give-up
    // log's "ModelNotInstantiated after 4 windows").  Under MODEL_WHY_DEAD this reads
    // `reason=noModelOnActor` instead and this count is 0.
    CHECK(r.log.Count("reason=ModelNotInstantiated") >= 1);
}

// AN ALREADY-DRESSED RECORD IS NEVER DRIVEN.  When ComponentOnActor is present from the start the
// precondition (undressed) is false, so the body is neither re-skinned nor pre-stamped: the swap
// that would print from==to is exactly the one this precondition suppresses.
TEST_CASE("W2-P1-46: a record that already carries a body is not re-driven") {
    using namespace loadoutdrive;
    modeldrive::Rig r;
    r.ent.modelResolves = true;                            // the body is present before we look
    r.ent.model0Resolves = true;
    r.p.Arm(r.Arm());
    REQUIRE(r.p.Request(UserSlot(0), DescWithNumbers()));

    r.PumpUntil([&] { return r.p.Ledger().provisioned == 1; });
    CHECK(r.p.Ledger().provisioned == 1);
    CHECK(r.w.eng.driveModelCalls == 0);                   // present -> no drive, no from==to
    CHECK(r.log.Count("InstanceModel present (model_.cs on actor != kNoComponent)") >= 1);
}

// ENG-W2-P1-54 (DIAG-MODEL-INSTANTIATE Lever A): THE +56 GATE HOLDS THE DRIVE UNTIL THE ENGINE-SELECTED
// INSTANCE IS NON-EMPTY, THEN FIRES.  SERVER_ASSIGN_TEAM re-seats the entrant and frees the instance the
// publish installed, so the model step first finds it empty (+56==0).  Unlike the old per-pair drive, the
// productive apply builds NOTHING over an empty instance, so the step HOLDS -- best-effort re-publishes to
// refill the instance -- and drives only once +56 flips non-empty.  Pins NO_REFIRE_ON_PROFILE_SETTLE
// (skip the re-publish -> the instance never fills -> the drive never fires).
TEST_CASE("W2-P1-54: a re-seated entrant holds on the empty instance, refills it, then drives once") {
    using namespace loadoutdrive;
    modeldrive::Rig r;
    r.w.eng.gatesBound = true;
    r.w.eng.gates.pairs = 0;                                // the re-seat freed the published instance
    r.w.eng.gates.verdict = ia::LoadoutApplyGate::Ready;    // ...but the dress VERDICT issued the enable
    r.w.eng.rebuildFillsInstance = true;                    // the model step's re-publish refills it
    r.w.eng.refillAfterPublishCount = 2;                    // dress publish + the single re-publish
    r.p.Arm(r.Arm());
    REQUIRE(r.p.Request(UserSlot(0), DescWithNumbers()));

    r.PumpUntil([&] { return r.w.eng.driveModelCalls > 0; });
    // The drive fired only after the re-publish filled the engine-selected instance (+56 flipped)...
    CHECK(r.w.eng.driveModelCalls == modeldrive::kDrivenOnce);
    CHECK(r.log.Count("re-fire engine productive apply (LoadoutApplyAlignment)") == modeldrive::kDrivenOnce);
    // ...preceded by the best-effort re-publish that refilled it.
    CHECK(r.log.Count("best-effort re-publish") >= 1);      // held+refilled (0 under NO_REFIRE_ON_PROFILE_SETTLE)
    CHECK(r.w.eng.gates.pairs == 20);                       // it FILLED (stays 0 under NO_REFIRE_ON_PROFILE_SETTLE)
}

// ENG-W2-P1-54 (DIAG-MODEL-INSTANTIATE Lever A): AN INSTANCE THAT NEVER REFILLS PROVISIONS BODYLESS, NAMED.
// Same re-seat wipe, but the best-effort re-publish never fills the engine-selected instance.  The
// productive apply cannot build a body over an empty instance, so past the bounded re-publishes the slot
// provisions BODYLESS holding LoadoutInstanceEmpty -- never a drive against an instance the engine would
// enumerate as 0 (the RUN-40 mistake).  Pins DRIVE_APPLY_CHASSIS_ON_ABSENT_MODEL (drive despite +56==0).
TEST_CASE("W2-P1-54: an engine-selected instance that never fills provisions bodyless, never drives empty") {
    using namespace loadoutdrive;
    modeldrive::Rig r;
    r.w.eng.gatesBound = true;
    r.w.eng.gates.pairs = 0;                                // empty and stays empty
    r.w.eng.gates.verdict = ia::LoadoutApplyGate::Ready;
    r.w.eng.rebuildFillsInstance = false;                   // the re-publish never refills it
    r.p.Arm(r.Arm());
    REQUIRE(r.p.Request(UserSlot(0), DescWithNumbers()));

    // The whole way through, the drive NEVER fires against the empty instance.  Under
    // W2P1_MUTATE_DRIVE_APPLY_CHASSIS_ON_ABSENT_MODEL the +56 hold is skipped and it drives on the
    // absent-model entrant -> driveModelCalls>0, exactly HEAD 993a65c / RUN-43.
    r.PumpUntil([&] { return r.p.Ledger().provisioned == 1; });
    CHECK(r.w.eng.driveModelCalls == 0);                   // RED under DRIVE_APPLY_CHASSIS_ON_ABSENT_MODEL
    CHECK(r.log.Count("best-effort re-publish") >= 1);     // it tried, bounded
    CHECK(r.log.Count("still empty") >= 1);                // named give-up after the bounded re-publishes
    CHECK(r.w.eng.gates.pairs == 0);                       // honestly never filled
    CHECK(r.p.Ledger().provisioned == 1);                  // a placed pawn, bodyless and named
}

// ENG-W2-P1-49 BLOCKER-1: THE MODEL STEP'S +56 READ MUST USE THE RESOLVED SLOT, NOT THE RECORDED ONE.
// The recorded slot (0) is held, so the entrant is mapped to slot 1; its loadout instance lives at the
// MAPPED slot.  The fake keys the pair count on the READ slot -- empty at slot 0, filled at slot 1.
// Reading s.mapped (slot 1, filled) sees +56>0 and drives at once with no re-publish; reading slot.v
// (slot 0, empty) would hold and re-publish needlessly.  Pins GATE_READ_RECORDED_SLOT.
TEST_CASE("W2-P1-49: the model step reads the resolved slot's +56, so a remapped entrant drives at once") {
    using namespace loadoutdrive;
    modeldrive::Rig r;
    r.id.Place(0, /*index=*/1, /*dressed=*/false);          // recorded slot 0 is held -> remap to 1
    r.w.eng.gatesBound = true;
    r.w.eng.gateKeysOnSlot = true;                          // the pair count depends on WHICH slot
    r.w.eng.slotPairs[0] = 0;                               // recorded slot: empty (the wrong read)
    r.w.eng.slotPairs[1] = 20;                              // mapped slot: the entrant's own, filled
    r.w.eng.gates.verdict = ia::LoadoutApplyGate::Ready;
    r.p.Arm(r.Arm());
    REQUIRE(r.p.Request(UserSlot(0), DescWithNumbers()));

    r.PumpUntil([&] { return r.w.eng.driveModelCalls > 0; });

    // The remap actually happened -- without it recorded == resolved and the read slot is moot.
    REQUIRE(r.p.MappedSlot(UserSlot(0)).v == 1);
    REQUIRE(r.p.MappedSlot(UserSlot(0)).v != UserSlot(0).v);

    CHECK(r.w.eng.driveModelCalls == modeldrive::kDrivenOnce);   // mapped slot reads filled -> drives
    // The mapped slot 1 reads FILLED, so no hold and no re-publish.  Under GATE_READ_RECORDED_SLOT the
    // +56 read uses slot 0 (empty) and the step holds + re-publishes instead -> count >= 1.
    CHECK(r.log.Count("best-effort re-publish") == 0);
}

// ENG-W2-P1-55 (DIAG-SPAWN-MODEL-CI Lever B): THE ENTRANT'S SPAWN HANDLER IS DRIVEN ONCE, BEFORE THE
// PRODUCTIVE APPLY, SO THE ncaModel/swapper CI IS INSTALLED WHEN THE APPLY DRESSES IT.  Our synthetic
// admission drove the REFRESH half of the entrant's Entrant-Data-Changed callback but never the SPAWN
// half (sub_1401CAC00), so RUN-44 reached the apply with modelCI=absent(kNoComponent) and no body.  The
// model step now drives NETGAME.SPAWN_NOTIFY once, right before the apply, and the section-4 A/B probe
// reads the model CI immediately before and after plus the branch the handler took.  The fake spawn
// models the remote arm INSTALLING the CI (ModelVisible flips present).  Pins SKIP_SPAWN_NOTIFY.
namespace modeldrive {
struct SpawnFake {
    unsigned calls = 0;
    CensusEntity* ent = nullptr;   // flipping its ModelVisible models the remote arm's install
    uint64_t modeWord = 0;         // bit1/bit2/bit6 clear -> the model-installing remote arm
    bool installs = true;
    bool issued = true;
};
SpawnFake* g_spawn = nullptr;
PawnProvision::SpawnNotifyResult SpawnNotifyFake(void*, uint16_t) {
    PawnProvision::SpawnNotifyResult r{};
    ++g_spawn->calls;
    r.issued = g_spawn->issued;
    r.modeRead = true;
    r.modeWord = g_spawn->modeWord;
    r.modeWordEffective = g_spawn->modeWord;   // the fake does not flip: effective == session word
    if (g_spawn->installs && g_spawn->ent != nullptr) g_spawn->ent->modelResolves = true;
    return r;
}
}  // namespace modeldrive

TEST_CASE("W2-P1-55: the spawn handler is driven once before the productive apply, model CI absent->present") {
    using namespace loadoutdrive;
    modeldrive::Rig r;
    modeldrive::SpawnFake spawn;
    spawn.ent = &r.ent;                       // the remote arm installs the CI on this entrant
    modeldrive::g_spawn = &spawn;
    r.h.spawnNotify = modeldrive::SpawnNotifyFake;   // the fake ignores ctx (HopsFor's &e stays)
    r.w.eng.gatesBound = true;
    r.w.eng.gates.pairs = 20;                 // instance non-empty: the apply is reached
    r.w.eng.gates.verdict = ia::LoadoutApplyGate::Ready;
    r.p.Arm(r.Arm());
    REQUIRE(r.p.Request(UserSlot(0), DescWithNumbers()));

    r.PumpUntil([&] { return r.w.eng.driveModelCalls > 0; });

    // Driven exactly once and BEFORE the apply: the A/B probe caught the model CI flipping
    // absent -> present across the call, the handler took the model-installing remote arm, and the
    // productive apply then fired.  Under W2P1_MUTATE_SKIP_SPAWN_NOTIFY the spawn handler is never
    // driven -> calls 0, the A/B line never appears, and the entrant stays undressed exactly as RUN-44.
    CHECK(spawn.calls == 1);                                            // RED under SKIP_SPAWN_NOTIFY
    CHECK(r.log.Count("spawnNotify: issued=1") == 1);                   // RED under SKIP_SPAWN_NOTIFY
    CHECK(r.log.Count("modelCI absent(kNoComponent) -> present") == 1); // the section-4 A/B flip
    CHECK(r.log.Count("arm=remote(install)") == 1);                     // the branch the handler took
    CHECK(r.w.eng.driveModelCalls == modeldrive::kDrivenOnce);          // the apply fired, after the install

    // The install flipped ModelVisible present, so the body resolves and the pawn provisions.
    r.PumpUntil([&] { return r.p.Ledger().provisioned == 1; });
    CHECK(r.p.Ledger().provisioned == 1);
    CHECK(spawn.calls == 1);                  // latched: the spawn handler is never re-driven
    modeldrive::g_spawn = nullptr;
}

// THE SECTION-4 BRANCH PROBE NAMES THE HOST ARM, THE ESCALATION SIGNAL.  When the session
// mode word selects the host arm (bit1 set), sub_1401CAC00 runs the local-user record init and NOT the
// remote arm, so no model CI is installed -- the model stays absent across the call.  The probe records
// arm=host(noInstall) and modelCI absent -> absent, which is the run signal to escalate to an
// architect-level spawn re-point rather than call the focused fix done.
TEST_CASE("W2-P1-55: the branch probe reports the host arm and a still-absent model CI as the escalation signal") {
    using namespace loadoutdrive;
    modeldrive::Rig r;
    modeldrive::SpawnFake spawn;
    spawn.ent = &r.ent;
    spawn.installs = false;                   // host arm: the call installs no model CI
    spawn.modeWord = 0x2;                      // bit1 set -> host arm
    modeldrive::g_spawn = &spawn;
    r.h.spawnNotify = modeldrive::SpawnNotifyFake;   // the fake ignores ctx (HopsFor's &e stays)
    r.w.eng.gatesBound = true;
    r.w.eng.gates.pairs = 20;
    r.w.eng.gates.verdict = ia::LoadoutApplyGate::Ready;
    r.p.Arm(r.Arm());
    REQUIRE(r.p.Request(UserSlot(0), DescWithNumbers()));

    r.PumpUntil([&] { return r.w.eng.driveModelCalls > 0; });

    CHECK(spawn.calls == 1);
    CHECK(r.log.Count("arm=host(noInstall)") == 1);                            // the branch the run must see
    CHECK(r.log.Count("modelCI absent(kNoComponent) -> absent(kNoComponent)") == 1);  // no install: escalate
    modeldrive::g_spawn = nullptr;
}

// ENG-W2-P1-50 (DIAG-SWAP-TRIGGER): THE ENTRANT'S ITEMS COME FROM ITS PROFILE, AND THE PROFILE LANDS
// LATE.  The instance's items derive from the profile document alone; in the recording the entrant's
// profile is empty at the spawn and the real bytes arrive ~a second later (echoreplay: empty 42.286,
// real 45.536).  Driving before it lands reaches the productive apply with a +56==0 instance and skips
// its loop -- no Load and apply, no swap, the RUN-39 bodyless entrant.  The shipped step HOLDS while
// the recorded profile is absent and drives ONCE the profile lands.  Pins DRIVE_BEFORE_PROFILE_LANDS.
TEST_CASE("W2-P1-50: the model drive holds for the entrant's late profile, then fires once it lands") {
    using namespace loadoutdrive;
    modeldrive::Rig r;
    // The +56 gate is off (a getterless image); this isolates the PROFILE precondition.
    r.w.eng.gatesBound = false;
    // The entrant's profile has not landed yet -- the recording answers Absent at the spawn.
    r.w.rec.rl.profile = replay::identity::RecordedProfileState::Absent;
    r.p.Arm(r.Arm());
    REQUIRE(r.p.Request(UserSlot(0), DescWithNumbers()));

    // WHILE THE PROFILE IS ABSENT: the step holds, and drives nothing.  Under DRIVE_BEFORE_PROFILE_LANDS
    // this line never prints (and the drive fires on the empty instance), so both of these red.
    r.PumpUntil([&] { return r.log.Count("entrant profile not yet present") >= 1; });
    CHECK(r.log.Count("entrant profile not yet present") >= 1);   // held, waiting for the profile
    CHECK(r.w.eng.driveModelCalls == 0);                          // NOT driven while the profile is absent
    CHECK(r.log.Count("re-fire engine productive apply") == 0);   // no drive yet

    // THE REAL PROFILE LANDS: the recording now answers Present.  The held drive re-fires ONCE.
    r.w.rec.rl.profile = replay::identity::RecordedProfileState::Present;
    r.PumpUntil([&] { return r.w.eng.driveModelCalls > 0; });
    CHECK(r.w.eng.driveModelCalls == modeldrive::kDrivenOnce);   // driven only after it landed
    CHECK(r.log.Count("re-fire engine productive apply (LoadoutApplyAlignment)") == modeldrive::kDrivenOnce);

    // The instrumentation carries the three camera-independent reads on one line, for RUN-L3-44.
    CHECK(r.log.Count("drivegate: modelCI=") >= 1);
}

// ★ THE DRESS RUNS, AND IT RUNS ONE FRAME AFTER A3.  Both halves matter: a build that never calls
// DressLoadout is exactly the state this packet inherited, and a build that calls it in the frame
// that issued the participation walks a customisation component the actor step has not filled.
TEST_CASE("W2-P1-30d: the loadout is dressed after the hops read done, never in A3's own frame") {
    using namespace loadoutdrive;
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    Wiring w;
    Seed(w.doc, w.eng, w.rec, 20);

    FakeIdentity id;
    FakeHopEngine e;
    // Everything the engine owes is already true EXCEPT the spawned record, so one pass issues the
    // participation and reads Done: the same frame, which is the frame the dress may not use.
    e.inCs = true;
    e.enabled = true;
    e.physCi = true;
    PawnProvision::ActorHops h = HopsFor(e);
    g_hops = &h;

    CaptureLog log;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.netGame = &BigNetGame;
    env.hops = &h;
    env.log = &log;
    env.recordedLoadout = &ProvideRecorded;
    env.loadoutEngine = &ProvideEngineWithHopGate;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), DescWithNumbers()));

    // PASS 1 -- the PUBLISH half runs, before the hop chain issues the enable (DIAG-MODEL-SWAP),
    // the participation is issued and every hop reads done, so the ONLY thing holding this slot
    // is the same-frame rule.  L5 and L6 have not run: the apply half is the half that waits.
    CHECK(p.Pump() == 0);
    CHECK(p.Ledger().participateIssued == 1);
    CHECK(p.Ledger().provisioned == 0);
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::LoadoutSameFrameAsParticipation);
    CHECK(w.eng.applyItemCalls == 0);
    CHECK(w.eng.order == "attach;parse;publish;");
    CHECK(w.rec.asks == 1);
    // ★ THE ORDERING ITSELF.  The publish is recorded BEFORE the enable the participating arm
    // issued in the same pass, which is the whole defect: the engine drives its own loadout apply
    // off that enable and refuses "User has 0 loadout instances" if the table is empty when it
    // fires.  The census line names the count the enable read.
    CHECK(log.Count("loadout publish None -- live instances 1, pairs=unread") == 1);
    CHECK(log.Count("compValid=notAsked inline=unread queued=unread -> gate=Unasked, before the enable") >= 1);

    // PASS 2 -- a later pump: the apply half is driven, once, in the engine's own order.
    uint32_t done = 0;
    for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks + 2 && done == 0; ++i) done += p.Pump();
    CHECK(done == 1);
    CHECK(p.Ledger().provisioned == 1);
    CHECK(w.rec.asks == 2);                    // once per half: the apply half re-verifies L1
    CHECK(w.rec.sawSlot == 0);                 // the RECORDED slot, which is what the block keys on
    // No "enabled;" of its own: L5's gate is hop 2's OWN pointer and ctx, so the read lands on
    // the hop chain's fake, not on this surface.  That the apply ran at all is the proof the ctx
    // was carried -- a dropped ctx makes HopEnabled deref null, answer false, and refuse.
    // NO `apply;` OF OUR OWN.  The container was published before the enable, so the engine has
    // already applied it off that enable and a second apply would re-drive a transition it owns.
    // The read-back and the alignment still run: the read-back IS the acceptance.
    CHECK(w.eng.order == "attach;parse;publish;readback;align;");
    CHECK(w.eng.applyItemCalls == 0);
    CHECK(w.eng.setStringCalls == 0);          // L1 verified and authored nothing

    // THE ACCEPTANCE, on the census line a runner greps: twenty recorded pairs matched twenty the
    // engine held, compared BY SLOT KEY.
    CHECK(log.Count("loadout: applied 1/1 attempted of 1 live (20/20 pairs)") >= 1);
    // And before the dress the same line said, truthfully, that nothing had been authored yet --
    // the census never claimed an applied loadout for a pass that had driven no step.
    CHECK(log.Count("loadout: not-attempted") >= 1);   // DIAG-APPLY-KEY sec A: nothing attempted is a SESSION fact; the recorded-side refusal is DriveLoadout's to name

    // AUD-W2-P1-30-part2 F11/F12.  The budget is checked against the marks a REAL dress issued,
    // not against the constants it is built from: one driven pass asked for `issued` marks, a tick
    // may drive one pass per slot-table row, and the budget has to cover that product or the
    // second entrant to dress in a tick is silenced and g_lastMarker freezes on the first.
    const unsigned issued = PawnProvision::HopMarksRequested();
    CHECK(issued > 0);
    CHECK(issued <= PawnProvision::HopMarkerBudget());
    CHECK(PawnProvision::HopMarkerBudget() >=
          PawnProvision::HopDrivenPassesPerTick() * issued);

    // ONE ATTEMPT PER HALF.  A provisioned slot is not re-dressed on the next window.
    for (uint32_t i = 0; i < 4; ++i) p.Pump();
    CHECK(w.eng.applyItemCalls == 0);
    CHECK(w.rec.asks == 2);
}

// ENG-NATIVE-ADMIT (echo-101).  With env.sourceLoadoutBeforeAdmit set -- the remote reply route --
// the recorded loadout is parsed into the per-user SOURCE container BEFORE admission, so the
// engine's own add-entrant bridge copies a non-empty loadout into the live table and its phase-16
// model-build reads real instances rather than the zero-length array it deadlocked on.  Identical
// setup to W2-P1-30d, and the only delta is the flag: it makes an extra parse (of the SOURCE) run
// AHEAD of the admission's own attach/parse/publish.  The default (flag off) case above proves the
// same setup does NOT run it, so this pins the flag as load-bearing.
TEST_CASE("W2-P1-30d ENG-NATIVE-ADMIT: the SOURCE loadout is parsed before admission on the remote route") {
    using namespace loadoutdrive;
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    Wiring w;
    Seed(w.doc, w.eng, w.rec, 20);

    FakeIdentity id;
    FakeHopEngine e;
    e.inCs = true;
    e.enabled = true;
    e.physCi = true;
    PawnProvision::ActorHops h = HopsFor(e);
    g_hops = &h;

    CaptureLog log;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.netGame = &BigNetGame;
    env.hops = &h;
    env.log = &log;
    env.recordedLoadout = &ProvideRecorded;
    env.loadoutEngine = &ProvideEngineWithHopGate;
    env.sourceLoadoutBeforeAdmit = true;   // the remote reply route
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), DescWithNumbers()));

    CHECK(p.Pump() == 0);
    // ★ THE SOURCE PARSE RAN FIRST.  A leading `parse;` -- the SOURCE fill -- precedes the
    // admission's own publish-half attach/parse/publish.  Without the flag (W2-P1-30d) the order is
    // exactly "attach;parse;publish;", so this leading parse is the change and nothing else.
    CHECK(w.eng.order == "parse;attach;parse;publish;");
    // And the recording was asked once more than the flag-off case: the SOURCE parse asks it too.
    CHECK(w.rec.asks == 2);
    // Admission still proceeded exactly as it does without the flag -- the pre-fill neither blocks
    // nor changes the admission itself; it only leaves a non-empty SOURCE for the engine's bridge.
    CHECK(p.Ledger().participateIssued == 1);

    // The SOURCE parse is a one-shot: later pumps drive the apply half but never re-parse the
    // SOURCE.  The order is W2-P1-30d's pass-2 order with exactly one leading SOURCE parse in front.
    uint32_t done = 0;
    for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks + 2 && done == 0; ++i) done += p.Pump();
    CHECK(done == 1);
    CHECK(w.eng.order == "parse;attach;parse;publish;readback;align;");
}

// ENG-BOTROUTE-FIX (Change A).  The bot Source parse used to bind the manifest entrantJson, which the
// engine parser fills to ZERO loadout instances (DIAG-BOTROUTE-PARSE); the fix carries the
// identity-block document -- the SAME {rl.document, rl.documentBytes} the remote route parses to one
// real instance -- to the admission on JoinDesc::recordedLoadoutDocument, and HookSourceLoadout binds
// THAT.  This case pins the PLUMBING half (the internal HookSourceLoadout/AdmitCtx are file-private, so
// the ProvisionReplayUser seam is the unit-testable one, and the parse-yields-non-empty-SOURCE half is
// pinned by identity's ENG-BOTBODY-LOADOUT-FIX DressLoadout case): the document reaching the admission
// is EXACTLY the recordedLoadout provider's identity-block bytes and NOT entrantJson, and the flag is
// load-bearing -- off, nothing is carried and the shipped default is byte-for-byte unchanged.
TEST_CASE("The bot admission carries the identity-block loadout document, gated") {
    using namespace loadoutdrive;
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    Wiring w;
    Seed(w.doc, w.eng, w.rec, 20);
    // The identity-block document as the recording's reader answers it: the SAME channel the remote
    // route parses (rl.document), and a length standing in for the real ~1671 B block so the plumbed
    // field is unmistakable.
    w.rec.rl.document      = w.doc.value[0];
    w.rec.rl.documentBytes = 1671;

    FakeIdentity id;
    FakeHopEngine e;
    e.inCs = true;
    e.enabled = true;
    e.physCi = true;
    PawnProvision::ActorHops h = HopsFor(e);
    g_hops = &h;
    CaptureLog log;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.netGame = &BigNetGame;
    env.hops = &h;
    env.log = &log;
    env.recordedLoadout = &ProvideRecorded;
    env.loadoutEngine = &ProvideEngineWithHopGate;

    // A DISTINCT manifest entrantJson, so "carried the identity-block bytes" cannot be confused with
    // "carried entrantJson": the two blobs differ in both pointer and length.
    static const uint8_t kManifestBytes[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    JoinDesc desc = DescWithNumbers();
    desc.entrantJson = Blob{kManifestBytes, uint32_t(sizeof kManifestBytes)};

    auto admittedEntrant = [&id]() -> const FakeIdentity::Entrant* {
        for (uint16_t i = 0; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i)
            if (id.slots[i].present) return &id.slots[i];
        return nullptr;
    };

    SUBCASE("flag ON: the identity-block document reaches the admission, distinct from entrantJson") {
        env.botBodySourceLoadout = true;
        p.Arm(env);
        REQUIRE(p.Request(UserSlot(0), desc));
        p.Pump();
        const FakeIdentity::Entrant* admitted = admittedEntrant();
        REQUIRE(admitted != nullptr);
        // The plumbed document is EXACTLY the recordedLoadout provider's identity-block bytes...
        CHECK(admitted->desc.recordedLoadoutDocument.p ==
              reinterpret_cast<const uint8_t*>(w.rec.rl.document));
        CHECK(admitted->desc.recordedLoadoutDocument.n == w.rec.rl.documentBytes);
        // ...and it is NOT the manifest entrantJson (the channel that parses to zero).
        CHECK(admitted->desc.recordedLoadoutDocument.p != admitted->desc.entrantJson.p);
        CHECK(admitted->desc.entrantJson.n == sizeof kManifestBytes);
    }

    SUBCASE("flag OFF: no identity-block document is carried -- shipped default unchanged") {
        env.botBodySourceLoadout = false;
        p.Arm(env);
        REQUIRE(p.Request(UserSlot(0), desc));
        p.Pump();
        const FakeIdentity::Entrant* admitted = admittedEntrant();
        REQUIRE(admitted != nullptr);
        CHECK(admitted->desc.recordedLoadoutDocument.n == 0);
        CHECK(admitted->desc.recordedLoadoutDocument.p == nullptr);
        // entrantJson still travels exactly as before -- the fix added a channel, it removed none.
        CHECK(admitted->desc.entrantJson.n == sizeof kManifestBytes);
    }
}

// ★ THE ORDERING DEFECT ITSELF (DIAG-MODEL-SWAP).  The engine drives its OWN loadout apply off
// the social ENABLE the participating arm issues, and RUN-33 shows that apply refusing "User has
// 0 loadout instances" 1.2 s before our publish landed -- an event that is spent once and never
// re-run, so the entrant wore nothing for the match.  The publish half therefore runs before the
// hop chain, and when the live table still reads 0 the hop chain is HELD rather than issuing an
// enable the engine is certain to refuse.
TEST_CASE("W2-P1-42: a published instance with no item pairs holds the enable, by name") {
    using namespace loadoutdrive;
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    Wiring w;
    Seed(w.doc, w.eng, w.rec, 20);
    // THE PUBLISH TAKES -- `Loadouts : 1` -- AND THE INSTANCE IS STILL EMPTY.  That is exactly
    // RUN-34's entrant: the live count is what the instance SEARCH needs and says nothing about
    // the item pairs the apply iterates, so a gate keyed on the count alone passes this state.
    w.eng.gatesBound = true;
    w.eng.gates.instances = 1;
    w.eng.gates.pairs = 0;
    w.eng.gates.actor = reinterpret_cast<const void*>(0x1122334455667788ull);
    w.eng.gates.compCs = reinterpret_cast<const void*>(0x99AABBCCDDEEFF00ull);
    w.eng.gates.compKey = 7;
    w.eng.gates.compValidAsked = true;
    w.eng.gates.compValid = true;
    w.eng.gates.verdict = ia::LoadoutApplyGate::InstanceEmpty;

    FakeIdentity id;
    FakeHopEngine e;
    e.inCs = true;
    e.enabled = true;
    e.physCi = true;
    PawnProvision::ActorHops h = HopsFor(e);
    g_hops = &h;

    CaptureLog log;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.netGame = &BigNetGame;
    env.hops = &h;
    env.log = &log;
    env.recordedLoadout = &ProvideRecorded;
    env.loadoutEngine = &ProvideEngineWithHopGate;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), DescWithNumbers()));

    // NO ENABLE WENT OUT.  The publish half ran whole -- attach, parse, publish -- and the gate
    // held the hop chain: an enable issued here is spent on an apply with no body to dress.
    CHECK(p.Pump() == 0);
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::LoadoutInstanceEmpty);
    CHECK(p.Ledger().participateIssued == 0);
    CHECK(w.eng.gateCalls >= 1);
    // AND THE READINGS THE VERDICT WAS DECIDED FROM ARE ON THE LINE, each one of them, so the
    // census says WHICH of the four shut rather than only that one did.
    CHECK(log.Count("live instances 1, pairs=0 actor=0x") == 1);
    CHECK(log.Count("compKey=0x00000007 compValid=1 inline=unread queued=unread -> gate=InstanceEmpty, before the enable") == 1);

    // AND IT IS A DEFERRAL.  The pairs arrive on a later window -- the document is re-parsed and
    // the publish re-run -- and the entrant is then provisioned and dressed.
    w.eng.gates.pairs = 20;
    w.eng.gates.verdict = ia::LoadoutApplyGate::Ready;
    uint32_t done = 0;
    for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks * 4 && done == 0; ++i) done += p.Pump();
    CHECK(done == 1);
    CHECK(p.Ledger().provisioned == 1);
    CHECK(p.Ledger().participateIssued == 1);
}

// THE OTHER THREE GATES, one case each: they are four different fixes and a test that only ever
// drove the pairs would let three of them refuse under one name.
TEST_CASE("W2-P1-42: each actor-side gate holds the enable under its own name") {
    struct GateRow { replay::identity_access::LoadoutApplyGate gate; PawnProvision::Refusal why; };
    const GateRow rows[] = {
        {replay::identity_access::LoadoutApplyGate::ActorUnresolved, PawnProvision::Refusal::LoadoutActorUnresolved},
        {replay::identity_access::LoadoutApplyGate::CsPointerNull,   PawnProvision::Refusal::LoadoutCsPointerNull},
        {replay::identity_access::LoadoutApplyGate::CompKeyInvalid,  PawnProvision::Refusal::LoadoutCompKeyInvalid},
        // THE CONTROL, and it is what makes the three above mean anything: the same wiring with
        // every gate open issues the enable.  Without it a gate that always held would pass.
        {replay::identity_access::LoadoutApplyGate::Ready,           PawnProvision::Refusal::None},
        // AND THE COULD-NOT-ASK ARM.  An image with no IDENT.ACTOR_REF_IS_VALID_VA must not hold
        // a single entrant: a question nobody managed to put is not a gate that answered no.
        {replay::identity_access::LoadoutApplyGate::Unasked,         PawnProvision::Refusal::None},
    };
    for (const GateRow& r : rows) {
        using namespace loadoutdrive;
        Reset();
        SetUpLobby(0x00025F1A7C3D1A2Bull);
        Wiring w;
        Seed(w.doc, w.eng, w.rec, 20);
        w.eng.gatesBound = true;
        w.eng.gates.instances = 1;
        w.eng.gates.pairs = 20;
        w.eng.gates.verdict = r.gate;

        FakeIdentity id;
        FakeHopEngine e;
        e.inCs = true;
        e.enabled = true;
        e.physCi = true;
        PawnProvision::ActorHops h = HopsFor(e);
        g_hops = &h;

        CaptureLog log;
        PawnProvisioner p;
        ProvisionEnv env = Armed(id, 2);
        env.netGame = &BigNetGame;
        env.hops = &h;
        env.log = &log;
        env.recordedLoadout = &ProvideRecorded;
        env.loadoutEngine = &ProvideEngineWithHopGate;
        p.Arm(env);
        REQUIRE(p.Request(UserSlot(0), DescWithNumbers()));

        if (r.why == PawnProvision::Refusal::None) {
            uint32_t done = 0;
            for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks * 4 && done == 0; ++i)
                done += p.Pump();
            CHECK(done == 1);
            CHECK(p.Ledger().participateIssued == 1);
        } else {
            // NO ENABLE ON THE WINDOW THE GATE WAS SHUT ON, which is the whole guarantee: an
            // enable issued here is spent on an apply the engine will skip without a word.
            // The hold is BOUNDED on purpose -- a pawn held for ever is worse than one dressed
            // late -- so this asserts the window, never that the slot is refused for the load.
            CHECK(p.Pump() == 0);
            CHECK(p.LastProvisionRefusal() == r.why);
            CHECK(p.Ledger().participateIssued == 0);
        }
    }
}

TEST_CASE("W2-P1-38: a publish that did not take holds the enable, and latches nothing") {
    using namespace loadoutdrive;
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    Wiring w;
    Seed(w.doc, w.eng, w.rec, 20);
    w.eng.publishNoOps = 1;    // the first window's publish call leaves the live table at 0

    FakeIdentity id;
    FakeHopEngine e;
    e.inCs = true;
    e.enabled = true;
    e.physCi = true;
    PawnProvision::ActorHops h = HopsFor(e);
    g_hops = &h;

    CaptureLog log;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.netGame = &BigNetGame;
    env.hops = &h;
    env.log = &log;
    env.recordedLoadout = &ProvideRecorded;
    env.loadoutEngine = &ProvideEngineWithHopGate;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), DescWithNumbers()));

    // PASS 1 -- L1..L4 ran and the live count still reads 0, so the hop chain was NOT run: no
    // participation was issued, which is to say no enable reached the engine.  That is the whole
    // guarantee -- an enable issued here is the one the engine refuses and nothing re-runs.
    CHECK(p.Pump() == 0);
    CHECK(p.LastProvisionRefusal() ==
          PawnProvision::Refusal::LoadoutNotPublishedBeforeEnable);
    CHECK(p.Ledger().participateIssued == 0);
    CHECK(w.eng.order == "attach;parse;publish;");
    CHECK(log.Count("loadout publish LoadoutPublishDidNotTake -- live instances 0, pairs=unread") == 1);

    // AND IT LATCHES NOTHING.  The refusal is a DEFERRAL: the publish is re-asked on a later
    // window, takes, and the slot goes on to be provisioned and dressed.  A latch here would
    // cost the entrant its pawn as well as its clothes.
    uint32_t done = 0;
    for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks * 4 && done == 0; ++i) done += p.Pump();
    CHECK(done == 1);
    CHECK(p.Ledger().provisioned == 1);
    CHECK(p.Ledger().participateIssued == 1);
    CHECK(log.Count("loadout publish None -- live instances 1, pairs=unread") == 1);
    CHECK(log.Count("compValid=notAsked inline=unread queued=unread -> gate=Unasked, before the enable") >= 1);
    CHECK(log.Count("loadout: applied 1/1 attempted of 1 live (20/20 pairs)") >= 1);
}

// The engine's container is fixed width and it writes -1 into BOTH halves of a
// slot the player wore nothing in, so a -1 KEY is an EMPTY PAIR and not a key -- and
// SentinelCollision has already proved no slot name hashes to -1.  Resolved before that is asked,
// one legitimately bare slot refuses LoadoutSlotKeyUnresolved and takes the whole document with
// it: the entrant wears nothing because it was wearing nineteen things instead of twenty.
TEST_CASE("W2-P1-38: an empty engine pair is counted unfilled, never resolved") {
    using namespace loadoutdrive;
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    Wiring w;
    Seed(w.doc, w.eng, w.rec, 20);

    // Slot 3 is bare, exactly as the engine records one.
    w.rec.rl.pairs[3].slotKey = ~uint64_t(0);
    w.rec.rl.pairs[3].itemId  = ~uint64_t(0);
    w.eng.applied[3] = LoadoutPair{};

    FakeIdentity id;
    FakeHopEngine e;
    e.inCs = true;
    e.enabled = true;
    e.physCi = true;
    PawnProvision::ActorHops h = HopsFor(e);
    g_hops = &h;

    CaptureLog log;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.netGame = &BigNetGame;
    env.hops = &h;
    env.log = &log;
    env.recordedLoadout = &ProvideRecorded;
    env.loadoutEngine = &ProvideEngineWithHopGate;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), DescWithNumbers()));

    uint32_t done = 0;
    for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks * 4 && done == 0; ++i) done += p.Pump();
    CHECK(done == 1);
    CHECK(p.Ledger().provisioned == 1);
    // NINETEEN compared and ONE counted bare -- and the two still add up to the recorded row
    // count, which is the rule an empty pair must not be allowed to break.
    CHECK(log.Count("L1 compared 19, unfilled 1, of 20 recorded rows") >= 1);
    CHECK(log.Count("loadout: applied 1/1 attempted of 1 live (19/19 pairs)") >= 1);
    CHECK(log.Count("LoadoutSlotKeyUnresolved") == 0);
}

// ★ THE v1 RECORDING, WHICH IS WHAT SHIPS TODAY.  It has a profile document and no slot keys, and
// the honest answer is a named refusal -- not a positional guess, and not a silent pass.  The pawn
// is still provisioned: a loadout that cannot be authored does not un-place a body.
TEST_CASE("W2-P1-30d: a v1 recording refuses LoadoutSlotKeysNotRecorded and still provisions") {
    using namespace loadoutdrive;
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    Wiring w;
    Seed(w.doc, w.eng, w.rec, 20);
    w.rec.rl.hasSlotKeys = false;      // the v1 identity block: items by POSITION, no keys

    FakeIdentity id;
    FakeHopEngine e;
    e.inCs = true;
    e.enabled = true;
    e.physCi = true;
    PawnProvision::ActorHops h = HopsFor(e);
    g_hops = &h;

    CaptureLog log;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.netGame = &BigNetGame;
    env.hops = &h;
    env.log = &log;
    env.recordedLoadout = &ProvideRecorded;
    env.loadoutEngine = &ProvideEngineWithHopGate;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), DescWithNumbers()));

    uint32_t done = 0;
    for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks + 4 && done == 0; ++i) done += p.Pump();
    CHECK(done == 1);
    CHECK(p.Ledger().provisioned == 1);
    CHECK(log.Count("loadout: not-authored (LoadoutSlotKeysNotRecorded)") >= 1);
    CHECK(log.Count("loadout: applied") == 0);
    // NOTHING reached the engine: L1 refused before a single call was made against its state.
    CHECK(w.eng.order.empty());
    CHECK(w.eng.applyItemCalls == 0);
}

// ★ AN ENGINE SURFACE THAT WILL NOT BIND.  A wrong or absent image is a could-not-ask, and it is
// refused whole rather than driven with a hole in the sequence.
TEST_CASE("W2-P1-30d: an unbindable engine surface refuses LoadoutEngineUnbound and calls nothing") {
    using namespace loadoutdrive;
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    Wiring w;
    Seed(w.doc, w.eng, w.rec, 20);
    w.eng.bind = false;

    FakeIdentity id;
    FakeHopEngine e;
    e.inCs = true;
    e.enabled = true;
    e.physCi = true;
    PawnProvision::ActorHops h = HopsFor(e);
    g_hops = &h;

    CaptureLog log;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.netGame = &BigNetGame;
    env.hops = &h;
    env.log = &log;
    env.recordedLoadout = &ProvideRecorded;
    env.loadoutEngine = &ProvideEngineWithHopGate;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), DescWithNumbers()));

    uint32_t done = 0;
    for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks + 4 && done == 0; ++i) done += p.Pump();
    CHECK(done == 1);
    CHECK(log.Count("loadout: not-authored (LoadoutEngineUnbound)") >= 1);
    CHECK(w.eng.order.empty());
    CHECK(w.eng.applyItemCalls == 0);
}

// ★ NO READER AT ALL.  "The recording was never asked" and "the recording said no" are different
// facts and the census must not spell them the same way.
TEST_CASE("W2-P1-30d: no recorded-identity reader is a could-not-ask, never an empty loadout") {
    using namespace loadoutdrive;
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    Wiring w;
    Seed(w.doc, w.eng, w.rec, 20);
    w.rec.ok = false;
    w.rec.why = "EntrantNotInBlock";

    FakeIdentity id;
    FakeHopEngine e;
    e.inCs = true;
    e.enabled = true;
    e.physCi = true;
    PawnProvision::ActorHops h = HopsFor(e);
    g_hops = &h;

    CaptureLog log;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.netGame = &BigNetGame;
    env.hops = &h;
    env.log = &log;
    env.recordedLoadout = &ProvideRecorded;
    env.loadoutEngine = &ProvideEngineWithHopGate;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), DescWithNumbers()));

    uint32_t done = 0;
    for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks + 4 && done == 0; ++i) done += p.Pump();
    CHECK(done == 1);
    // The reader's OWN word rides the line: no enum of ours can say WHICH block was missing.
    CHECK(log.Count("loadout: not-authored (LoadoutDocumentNotRecorded) -- EntrantNotInBlock") >= 1);
    CHECK(w.eng.order.empty());
}

// ★ THE B.0 ROW'S NO-TEAM SURVIVES THE DESCRIPTOR.  ENG-W2-A-4 made the reader widen a v0 row's
// only absence value (0xFF) into kPlayerNoTeam16; reading the 8-bit field again here threw that
// away, and A3 then refused TeamOutOfRange for a player whose team was never recorded at all.
// One source for both record shapes: the field the READER hands back.
TEST_CASE("W2-P1-30d: no-team reaches the descriptor from a B.0 row and an extended row alike") {
    PlayerRecord b0{};
    b0.xplatformId = 7;
    b0.extended = 0;
    b0.teamId = 0xFF;                       // B.0's only way to say "absent"
    b0.teamId16 = kPlayerNoTeam16;          // what manifest_io's widening leaves behind
    CHECK(JoinDescFromRecord(b0).team == kPlayerNoTeam16);

    PlayerRecord ext{};
    ext.xplatformId = 7;
    ext.extended = 1;
    ext.teamId = 0xFF;
    ext.teamId16 = kPlayerNoTeam16;
    CHECK(JoinDescFromRecord(ext).team == kPlayerNoTeam16);

    // A REAL team is unaffected on both shapes: this fix widens the absence, it does not widen
    // every value into one.
    PlayerRecord b0Blue{};
    b0Blue.extended = 0;
    b0Blue.teamId = 1;
    b0Blue.teamId16 = 1;
    CHECK(JoinDescFromRecord(b0Blue).team == 1);
    CHECK(JoinDescFromRecord(OnePlayer(1, 1)).team == 1);
}

// AUD-W2-P1-30-part2 F10.  THE CENSUS LINE IS THE ACCEPTANCE, so it has to be able to report a
// PARTIAL load.  The old clause took the FIRST attempted slot and reported it for the whole load:
// one entrant dressed 20/20 out of three printed a clean `applied 20/20` over two entrants
// standing in the engine's default kit.  The decision is asked here directly, over a population no
// fixture has to provision, so the count is the thing under test rather than the fixture.
TEST_CASE("W2-P1-30e: the census counts every dressed entrant, not the first one") {
    namespace ia = replay::identity_access;
    PawnProvision::LoadoutOutcome rows[3];
    for (int i = 0; i < 3; ++i) { rows[i].alive = true; rows[i].attempted = true; }
    // Slot 0 dressed and matched every pair it compared; 1 and 2 were asked and refused BY NAME.
    rows[0].compared = 20;
    rows[0].matched = 20;
    rows[1].why = ia::LoadoutRefusal::DocumentNotRecorded;
    rows[1].sourceWhy = "IdentityBlockNotRecorded";
    rows[2].why = ia::LoadoutRefusal::SlotKeyUnresolved;

    const PawnProvision::LoadoutCensus c = PawnProvision::SummariseLoadouts(rows, 3);
    CHECK(c.live == 3);
    CHECK(c.attempted == 3);
    CHECK(c.authored == 1);
    CHECK(c.compared == 20);
    CHECK(c.matched == 20);
    CHECK(c.firstWhy == ia::LoadoutRefusal::DocumentNotRecorded);

    char clause[224] = {0};
    PawnProvision::FormatLoadoutClause(c, clause, sizeof clause);
    CHECK(std::string(clause) ==
          "applied 1/3 attempted of 3 live (20/20 pairs), first refusal "
          "LoadoutDocumentNotRecorded -- IdentityBlockNotRecorded");
    // The reading the old clause produced, which must never appear for this population again.
    CHECK(std::string(clause).find("applied 1/1") == std::string::npos);
    CHECK(std::string(clause).find("applied 3/3") == std::string::npos);

    // ALL THREE REFUSED is not the same statement as ONE OF THREE, and neither is a bare zero.
    rows[0].compared = 0;
    rows[0].matched = 0;
    rows[0].why = ia::LoadoutRefusal::SlotKeysNotRecorded;
    PawnProvision::FormatLoadoutClause(PawnProvision::SummariseLoadouts(rows, 3), clause,
                                       sizeof clause);
    CHECK(std::string(clause) ==
          "not-authored (LoadoutSlotKeysNotRecorded) -- 0/3 attempted of 3 live");

    // AUD-W2-P1-30-part2 F15.  THE DENOMINATOR IS THE LOAD, NOT THE ATTEMPTS.  An entrant parked
    // on a hop is never attempted and its deferral does not latch, so a fraction counted over
    // attempts alone reads a clean 2/2 over a naked pawn for the whole match. Two of three here.
    rows[0].compared = 20;
    rows[0].matched = 20;
    rows[0].why = ia::LoadoutRefusal::None;
    rows[1] = rows[0];
    rows[2] = PawnProvision::LoadoutOutcome{};
    rows[2].alive = true;
    rows[2].deferralWhy = PawnProvision::HopName(PawnProvision::Hop::PhysicsCi);
    const PawnProvision::LoadoutCensus parked = PawnProvision::SummariseLoadouts(rows, 3);
    CHECK(parked.live == 3);
    CHECK(parked.attempted == 2);
    CHECK(parked.authored == 2);
    CHECK(parked.deferred == 1);
    PawnProvision::FormatLoadoutClause(parked, clause, sizeof clause);
    CHECK(std::string(clause) ==
          "applied 2/2 attempted of 3 live (40/40 pairs), deferred: PhysicsCi x1");
    // The reading B1 must NOT close on: two of two, with the third entrant invisible.
    CHECK(std::string(clause).find("of 2 live") == std::string::npos);

    // And a load where nothing was dressed does NOT synthesise a recording verdict from a
    // zero-initialised RecordedLoadout (DIAG-APPLY-KEY sec A): it says nothing was attempted, and
    // the recorded-side refusal is never fabricated here.
    for (int i = 0; i < 3; ++i) rows[i].attempted = false;
    PawnProvision::FormatLoadoutClause(PawnProvision::SummariseLoadouts(rows, 3), clause,
                                       sizeof clause);
    CHECK(std::string(clause).find("not-attempted") == 0);
    CHECK(std::string(clause).find("not-authored") == std::string::npos);
    CHECK(std::string(clause).find("LoadoutSlotKeysNotRecorded") == std::string::npos);
    // RUN-29b F4: the K/N/M tail holds on ALL THREE arms, so the LIVE count is always there.
    CHECK(std::string(clause).find("attempted of 3 live") != std::string::npos);
    CHECK(std::string(clause).find("entrants") == std::string::npos);
}

// DIAG-APPLY-KEY sec A / L3 #35 F4.  A parked entrant's clause held ActorNotEnabled and printed
// LoadoutSlotKeysNotRecorded instead, synthesised from a zeroed RecordedLoadout on a run whose
// recording carried keys.  The held deferral is the one true word.
TEST_CASE("W2-P1-44: a parked entrant names its held deferral, not a synthesised recording verdict") {
    PawnProvision::LoadoutCensus c;
    c.live = 1;
    c.attempted = 0;
    c.deferred = 1;
    c.firstDeferral = "ActorNotEnabled";
    c.anyDocument = true;   // the recording DID carry a document -- the synthesis reads keys as no
    char clause[224] = {0};
    PawnProvision::FormatLoadoutClause(c, clause, sizeof clause);
    CHECK(std::string(clause) ==
          "not-attempted -- 0/0 attempted of 1 live, deferred: ActorNotEnabled x1");
    CHECK(std::string(clause).find("LoadoutSlotKeysNotRecorded") == std::string::npos);
}

// AUD-W2-P1-30-part2 F13.  The contract's PLAYBACK side, which F1 closed only on the engine side.
// `playback_module.cpp` constructs one of these bare and hands it to the reader; a build that
// value-initialises it and then reads it without a true return must see a could-not-ask, never an
// entrant who was asked and answered "nothing".
TEST_CASE("W2-P1-30e: a value-initialised playback RecordedLoadout is a could-not-ask") {
    replay::identity::RecordedLoadout rl{};
    CHECK(rl.filledPairCount == replay::identity::kRecordedPairCountUnknown);
    CHECK(rl.filledPairCount != 0);
    CHECK_FALSE(rl.hasSlotKeys);
    CHECK(rl.profile == replay::identity::RecordedProfileState::NotAsked);
    CHECK(rl.document == nullptr);
    CHECK(rl.documentBytes == 0);
    CHECK(rl.pairs[0].slotKey == 0);
    CHECK(rl.pairs[replay::identity::kRecordedLoadoutPairCap - 1].itemId == 0);
    // The same record the ENGINE side answers for, so the two halves of the contract cannot drift:
    // an unknown count on either side is the same word, and it is not zero.
    replay::identity_access::RecordedLoadout eng{};
    CHECK(eng.filledPairCount == replay::identity_access::kLoadoutPairCountUnknown);
    CHECK(unsigned(rl.filledPairCount) == unsigned(eng.filledPairCount));
}


// F44: THE MODEL CLAUSE IS ON THE SHIPPED PER-WINDOW LINE, NOT ONLY ON THE ONCE-PER-EPOCH CENSUS.
// The per-slot census runs behind `provisioned >= 1` and prints once; every run so far parked its
// entrant in Awaiting, so `model: resolved=... reason=...` -- the clause added for the undressed
// entrant -- could not appear in any shipping log.  This drives the SHIPPED arm+pump path with an
// entity whose ModelVisible declines and asserts the clause rides the window line, by name, on
// every cadence window, with the reason NAMED rather than left to be inferred from `resolved=no`.
TEST_CASE("F44: the window census carries the model clause, every window, with its reason") {
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    SetState(0);

    FakeIdentity id;
    id.Place(0, 7, true);
    id.refuseProvisionWith = PawnProvision::Refusal::ReplyWrongPhase;
    id.engineCompletesAfterInserts = 1;
    id.engineCompletesDressed = false;

    FakeHopEngine eng;
    eng.inCs = true;
    eng.enabled = false;
    eng.spawned = false;
    eng.physCi = false;          // the slot stays Awaiting -- the state the clause exists for
    PawnProvision::ActorHops hopSurface = HopsFor(eng);

    CensusEntity ent;
    ent.modelResolves = false;   // the gate declines, so the clause has something to name

    CaptureLog log;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.log    = &log;
    env.hops   = &hopSurface;
    env.entity = &ent;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), JoinDesc{}));

    SetState(facts::NETGAME_STATE_INMATCH);
    p.Pump();
    const uint16_t live = p.MappedSlot(UserSlot(0)).v;
    REQUIRE(live < facts::ENTITY_USER_SLOT_TABLE_MAX);

    // Nothing is past Pending yet, so there is no hop line and no model clause to ride one.
    CHECK(log.Count(" model: ") == 0);

    for (uint32_t i = 0; i < 4 * PawnProvisioner::kRetryTicks + 4; ++i) p.Pump();

    // The slot is Awaiting -- the once-per-epoch census never ran, and the clause is here anyway.
    CHECK(p.Ledger().provisioned == 0);
    CHECK(log.Count("census slot=") == 0);
    // ON THE WINDOW LINE, not on a line of its own: one formatter, one line.
    CHECK(log.Count("hops: 0->1=PhysicsCi") >= 2);
    for (const std::string& l : log.lines) {
        if (l.find(" model: ") == std::string::npos) continue;
        CHECK(l.find("hops: ") != std::string::npos);
        CHECK(l.find("loadout: ") != std::string::npos);
    }
    // EVERY window carries it, which is the whole difference from the epoch census.
    CHECK(log.Count(" model: resolved=no visible=no ctrl0=yes reason=noModelOnActor") >= 2);
    // DIAG-L3-33 B.5: the control is what turns `resolved=no` into a measurement. The
    // viewer resolves on the SAME surface, so `ctrl0=y` beside `resolved=no` says the probe
    // works and the entrant has no model -- not that the probe is broken.
    CHECK(log.Count(" ctrl0=yes ") >= 2);
    CHECK(log.Count(" ctrl0=no ") == 0);
    CHECK(log.Count(" model: ") == log.Count("hops: "));
    // The reason carries the slot's OWN loadout reading, and a could-not-ask is not a count.
    CHECK(log.Count("reason=noModelOnActor loadouts=") >= 2);

    // THE CONTROL, and it is what keeps the case from passing on a hardcoded string: the same
    // pumped path with a gate that ANSWERS prints the clause with nothing to name.
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    SetState(0);
    FakeIdentity id2;
    id2.Place(0, 7, true);
    id2.refuseProvisionWith = PawnProvision::Refusal::ReplyWrongPhase;
    id2.engineCompletesAfterInserts = 1;
    id2.engineCompletesDressed = false;
    FakeHopEngine eng2;
    eng2.inCs = true;
    eng2.enabled = false;
    eng2.spawned = false;
    eng2.physCi = false;
    PawnProvision::ActorHops hopSurface2 = HopsFor(eng2);
    CensusEntity ent2;           // modelResolves stays true
    CaptureLog answered;
    PawnProvisioner p2;
    ProvisionEnv env2 = Armed(id2, 2);
    env2.log    = &answered;
    env2.hops   = &hopSurface2;
    env2.entity = &ent2;
    p2.Arm(env2);
    REQUIRE(p2.Request(UserSlot(0), JoinDesc{}));
    SetState(facts::NETGAME_STATE_INMATCH);
    for (uint32_t i = 0; i < 4 * PawnProvisioner::kRetryTicks + 5; ++i) p2.Pump();
    CHECK(answered.Count(" model: resolved=yes") >= 2);
    CHECK(answered.Count("reason=") == 0);

    // AND A BUILD WITH NO ENTITY SURFACE SAYS SO rather than reading as a resolved gate.
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    SetState(0);
    FakeIdentity id3;
    id3.Place(0, 7, true);
    id3.refuseProvisionWith = PawnProvision::Refusal::ReplyWrongPhase;
    id3.engineCompletesAfterInserts = 1;
    id3.engineCompletesDressed = false;
    FakeHopEngine eng3;
    eng3.inCs = true;
    eng3.enabled = false;
    eng3.spawned = false;
    eng3.physCi = false;
    PawnProvision::ActorHops hopSurface3 = HopsFor(eng3);
    CaptureLog unasked;
    PawnProvisioner p3;
    ProvisionEnv env3 = Armed(id3, 2);
    env3.log  = &unasked;
    env3.hops = &hopSurface3;    // env3.entity stays null: no surface to ask
    p3.Arm(env3);
    REQUIRE(p3.Request(UserSlot(0), JoinDesc{}));
    SetState(facts::NETGAME_STATE_INMATCH);
    for (uint32_t i = 0; i < 4 * PawnProvisioner::kRetryTicks + 5; ++i) p3.Pump();
    CHECK(unasked.Count(" model: resolved=unasked visible=unasked reason=NoEntitySurface") >= 2);
    CHECK(unasked.Count("resolved=no ") == 0);
}

// AUD-W2-P1-34 MAJOR-1 -- the wrapper's return is not the engine's.  sub_14019F1C0 forwards
// SET_PLAYER_ACTOR's 0 (`14019F1FC test eax,eax / jz 14019F24C`), but on the SUCCESS path it
// overwrites the return register with `(*(u8*)**(u64**)(ng+0x2DA0)) >> 2`, so a session whose
// flags bit is clear makes a completed allocate-and-dispatch answer 0.  Believing that 0 latches
// the one-way gate: it retires the refresh for the epoch off a call that in fact rebuilt the actor.
TEST_CASE("W2-P1-36: a wrapper zero with a MOVED SActorGUID is not a decline") {
    FakeHopEngine e;
    PawnProvision::ActorHops h = HopsFor(e);
    e.inCs = true;
    e.team = 0;
    e.teamSlot = 0;
    e.spawned = false;
    e.enabled = false;
    e.actorHasTeamKey = false;    // :90 lets this call through, so :319 allocates
    e.refreshEnables = true;
    e.wrapperReturnsZero = true;  // ... and the wrapper hands back 0 anyway
    PawnProvision::DriveGate g;

    PawnProvision::HopDrive p1 = DriveFor(g, 0);
    CHECK(PawnProvision::Participate(h, 7, p1).why ==
          PawnProvision::ParticipateRefusal::RefreshDeferredToNextPump);

    const uint64_t before = e.guidLo;
    PawnProvision::HopDrive p2 = DriveFor(g, 0);
    const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(h, 7, p2);
    CHECK(g.lastRefresh == PawnProvision::RefreshAnswer::Declined);  // what the engine ANSWERED
    CHECK(e.guidLo != before);        // what the engine DID -- :319 is the only writer of it
    CHECK(!g.refreshDeclined);        // the two-read decides, and it did not latch the gate
    CHECK(o.state == PawnProvision::Participation::Issued);
    CHECK(o.why == PawnProvision::ParticipateRefusal::None);
    CHECK(e.enabled);                 // and the enable re-read, not the return, booked it
}

// The other half of the same reading: an UNMOVED guid is what names sub_1401B0D80:90-91, and a
// two-read that could not be taken is a third answer -- never the confident one.
TEST_CASE("W2-P1-36: a wrapper zero the guid cannot corroborate latches nothing") {
    FakeHopEngine e;
    PawnProvision::ActorHops h = HopsFor(e);
    e.inCs = true;
    e.team = 0;
    e.teamSlot = 0;
    e.spawned = false;
    e.enabled = false;
    e.actorHasTeamKey = true;   // the genuine :91 state
    e.guidReadable = false;     // ... and the reading that would prove it is not available
    PawnProvision::DriveGate g;

    PawnProvision::HopDrive p1 = DriveFor(g, 0);
    CHECK(PawnProvision::Participate(h, 7, p1).why ==
          PawnProvision::ParticipateRefusal::RefreshDeferredToNextPump);

    PawnProvision::HopDrive p2 = DriveFor(g, 0);
    const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(h, 7, p2);
    CHECK(o.why == PawnProvision::ParticipateRefusal::RefreshDeclineNotCorroborated);
    CHECK(strcmp(PawnProvision::ParticipateRefusalName(o.why),
                 "RefreshDeclineNotCorroborated") == 0);
    CHECK(e.refreshes == 1);
    CHECK(!g.refreshDeclined);  // COULD NOT ASK is not asked-and-failed
}

// DEFERRED CHECK #8.  The refresh is not harmless: sub_14019F1C0:8-9 builds its own third
// argument, so NETGAME.SET_PLAYER_ACTOR can take its `Player Actor Freeing` arm.  The enable is
// read AT the refresh, and not inherited from the read the window opened with.
TEST_CASE("W2-P1-38: an actor that is ENABLED at the refresh site is never refreshed") {
    FakeHopEngine e;
    PawnProvision::ActorHops h = HopsFor(e);
    e.inCs = true;
    e.team = 0;
    e.teamSlot = 0;
    e.spawned = false;
    e.enabled = false;
    e.actorHasTeamKey = true;
    PawnProvision::DriveGate g;

    PawnProvision::HopDrive p1 = DriveFor(g, 0);
    CHECK(PawnProvision::Participate(h, 7, p1).why ==
          PawnProvision::ParticipateRefusal::RefreshDeferredToNextPump);
    CHECK(e.refreshes == 0);

    // The engine turns the actor on between this window's entry read and the refresh: read 1 is
    // `a3 enabled#1` and read 2 is `a3 enabled#beforeRefresh`.
    e.enabledFromRead = e.enabledReads + 2;
    PawnProvision::HopDrive p2 = DriveFor(g, 0);
    const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(h, 7, p2);
    CHECK(o.why == PawnProvision::ParticipateRefusal::ActorEnabledSoNotFreed);
    CHECK(e.refreshes == 0);    // THE GOAL: no call that can free a live actor was issued
}

// AUD-W2-P1-34 MAJOR-3 -- the census may not print a number it does not have.  `filledPairCount`
// carries 0xFFFF for a capture that could not ask, and the line used to print a denominator
// derived from it by subtraction.
TEST_CASE("W2-P1-36: the L1 census renders an unknown recorded count by name") {
    using namespace loadoutdrive;
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    Wiring w;
    Seed(w.doc, w.eng, w.rec, 20);
    w.rec.rl.filledPairCount = replay::identity::kRecordedPairCountUnknown;

    FakeIdentity id;
    FakeHopEngine e;
    e.inCs = true;
    e.enabled = true;
    e.physCi = true;
    PawnProvision::ActorHops h = HopsFor(e);
    g_hops = &h;

    CaptureLog log;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.netGame = &BigNetGame;
    env.hops = &h;
    env.log = &log;
    env.recordedLoadout = &ProvideRecorded;
    env.loadoutEngine = &ProvideEngineWithHopGate;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), DescWithNumbers()));
    for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks + 3; ++i) p.Pump();

    CHECK(log.Count("L1 compared 0, unfilled 0, of unknown recorded rows") >= 1);
    CHECK(log.Count("LoadoutPairCountUnknown") >= 1);
    CHECK(log.Count("65535") == 0);      // the subtraction that printed `0/65535 filled`
    CHECK(log.Count("4294967295") == 0); // and the one that wrapped on the mismatch path
}

// The same line on a recording that CAN be verified: three readings, printed as they were read,
// so `compared + unfilled == filledPairCount` is checkable by adding the two on the line.
TEST_CASE("W2-P1-36: the L1 census prints the recorded row count, not a difference") {
    using namespace loadoutdrive;
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    Wiring w;
    Seed(w.doc, w.eng, w.rec, 20);
    for (uint16_t i = 14; i < 20; ++i) {   // six slots the player genuinely wore nothing in
        w.rec.rl.pairs[i].itemId = ~uint64_t(0);
        w.eng.applied[i].itemId  = ~uint64_t(0);
    }

    FakeIdentity id;
    FakeHopEngine e;
    e.inCs = true;
    e.enabled = true;
    e.physCi = true;
    PawnProvision::ActorHops h = HopsFor(e);
    g_hops = &h;

    CaptureLog log;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.netGame = &BigNetGame;
    env.hops = &h;
    env.log = &log;
    env.recordedLoadout = &ProvideRecorded;
    env.loadoutEngine = &ProvideEngineWithHopGate;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), DescWithNumbers()));
    for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks + 3; ++i) p.Pump();

    CHECK(log.Count("L1 compared 14, unfilled 6, of 20 recorded rows") >= 1);
    CHECK(log.Count("of 14 recorded rows") == 0);   // the derived denominator, which is not a reading
}

// A5.  IDENT.LOAD_AND_APPLY_ITEM has four per-slot outcomes and only one of them moves +0x10, so
// scoring on the item alone reports three different engine decisions as one `mismatched` nobody
// can act on (DIAG-L3-33 A.1: that is what made RUN-33's sixteen unexplainable).  One fixture per
// arm, each planted in the engine side only -- the recording is identical in all four.
TEST_CASE("W2-P1-37: the 20/20 scores each slot by the arm the engine took") {
    using namespace loadoutdrive;

    auto drive = [](void (*plant)(EngineFake&), CaptureLog& log) {
        Reset();
        SetUpLobby(0x00025F1A7C3D1A2Bull);
        Wiring w;
        Seed(w.doc, w.eng, w.rec, 20);
        plant(w.eng);
        // L1 skips a row whose ITEM is the empty-slot sentinel and counts it `unfilled`; the
        // engine still holds a real id there, so a scorer that does not skip the same row
        // books a mismatch for a slot both sides agree is bare (L3 #34).
        if (w.eng.unfilledRow >= 0) w.rec.rl.pairs[w.eng.unfilledRow].itemId = ~uint64_t(0);

        FakeIdentity id;
        FakeHopEngine e;
        e.inCs = true;
        e.enabled = true;
        e.physCi = true;
        PawnProvision::ActorHops h = HopsFor(e);
        g_hops = &h;

        PawnProvisioner p;
        ProvisionEnv env = Armed(id, 2);
        env.netGame = &BigNetGame;
        env.hops = &h;
        env.log = &log;
        env.recordedLoadout = &ProvideRecorded;
        env.loadoutEngine = &ProvideEngineWithHopGate;
        p.Arm(env);
        JoinDesc d = DescWithNumbers();
        d.alignment = w.eng.descAlignment;
        REQUIRE(p.Request(UserSlot(0), d));
        for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks + 3; ++i) p.Pump();
    };

    SUBCASE("ARM 1/3 -- the engine wears it: twenty matched and not one slot line") {
        CaptureLog log;
        drive([](EngineFake&) {}, log);
        CHECK(log.Count("matched 20 pending 0 alignGated 0 mismatch 0 of 20") >= 1);
        CHECK(log.Count("[identity] loadout slot ") == 0);
        CHECK(log.Count("LoadoutAppliedPairsMismatch") == 0);
    }

    SUBCASE("ARM 2 -- the APPLIED_BIT latched our item at +0x20: pending, named, not a refusal") {
        CaptureLog log;
        drive([](EngineFake& eng) {
            // The engine took the value and queued it: +0x10 still holds what was there before.
            eng.appliedState[3].flags = facts::IDENT_CUST_SLOT_APPLIED_BIT;
            eng.appliedState[3].pendingItem = eng.applied[3].itemId;
            eng.applied[3].itemId = 0xD15EA5EDD15EA5EDull;
        }, log);
        CHECK(log.Count("matched 19 pending 1 alignGated 0 mismatch 0 of 20") >= 1);
        CHECK(log.Count("[identity] loadout slot chassis: ") >= 1);
        CHECK(log.Count(" -> pending") >= 1);
        CHECK(log.Count("item=0xD15EA5EDD15EA5ED") >= 1);
        CHECK(log.Count("flags=0x00000001") >= 1);
        CHECK(log.Count("LoadoutAppliedPairsMismatch") == 0);
    }

    SUBCASE("ARM 4 -- the outer alignment gate skipped the slot, and the recording says why") {
        CaptureLog log;
        drive([](EngineFake& eng) {
            eng.appliedState[16].alignmentReq = 1;   // tint_alignment_a's own requirement
            eng.applied[16].itemId = 0;              // nothing was written
            eng.entityAlign = 2;                     // the entrant does not meet it...
            eng.descAlignment = 2;                   // ...and the capture recorded that
        }, log);
        CHECK(log.Count("matched 19 pending 0 alignGated 1 mismatch 0 of 20") >= 1);
        CHECK(log.Count("[identity] loadout slot tint_alignment_a: ") >= 1);
        CHECK(log.Count(" -> alignGated") >= 1);
        CHECK(log.Count("align=0x0001") >= 1);
        CHECK(log.Count("LoadoutAppliedPairsMismatch") == 0);
        CHECK(log.Count("LoadoutAlignmentUnrecorded") == 0);
    }

    // AUD-W2-P1-34 P1-39 F1.  THE SAME SKIP WITH NOTHING TO CORROBORATE IT.  The capture
    // recorded no alignment, so the live value the engine skipped by is uncorroborated and the
    // slot is left visibly empty -- which is a difference, not a pass.  It scores as the
    // mismatch it is, under a word that names what the capture owes.
    SUBCASE("the skip the recording cannot corroborate is a mismatch, by name") {
        CaptureLog log;
        drive([](EngineFake& eng) {
            eng.appliedState[16].alignmentReq = 1;
            eng.applied[16].itemId = 0;
            eng.entityAlign = 2;
            // eng.descAlignment stays at the sentinel: the capture recorded no alignment
        }, log);
        CHECK(log.Count("matched 19 pending 0 alignGated 0 mismatch 1 of 20") >= 1);
        CHECK(log.Count("[identity] loadout slot tint_alignment_a: ") >= 1);
        CHECK(log.Count(" -> mismatch") >= 1);
        CHECK(log.Count("LoadoutAlignmentUnrecorded") >= 1);
    }

    SUBCASE("the residue -- gate passed, nothing latched, wrong item: still a refusal") {
        CaptureLog log;
        drive([](EngineFake& eng) {
            eng.applied[5].itemId = 0xBADBADBADBADBAD1ull;
        }, log);
        CHECK(log.Count("matched 19 pending 0 alignGated 0 mismatch 1 of 20") >= 1);
        CHECK(log.Count("[identity] loadout slot decal_body: ") >= 1);
        CHECK(log.Count(" -> mismatch") >= 1);
        CHECK(log.Count("LoadoutAppliedPairsMismatch") >= 1);
    }

    // The engine's gate has two conjuncts and skips the entry only when the
    // requirement is set AND the entrant does not meet it.  These four say what the shipped rule
    // could not: a requirement the entrant MEETS is not a skip, and an unreadable second operand
    // is not a pass.
    SUBCASE("gated but MATCHING -- the entrant meets it, so the gate passed and nothing was worn") {
        CaptureLog log;
        drive([](EngineFake& eng) {
            eng.appliedState[16].alignmentReq = 1;
            eng.entityAlign = 1;                 // the entrant IS alignment 1
            eng.applied[16].itemId = 0;          // and the slot is still empty
        }, log);
        CHECK(log.Count("matched 19 pending 0 alignGated 0 mismatch 1 of 20") >= 1);
        CHECK(log.Count("[identity] loadout slot tint_alignment_a: ") >= 1);
        CHECK(log.Count(" -> mismatch") >= 1);
        CHECK(log.Count("entityAlign=0x0001") >= 1);
        CHECK(log.Count("LoadoutAppliedPairsMismatch") >= 1);
    }

    SUBCASE("SUBSTITUTED -- gate passed and the engine wrote a cosmetic of its own choosing") {
        CaptureLog log;
        drive([](EngineFake& eng) {
            eng.appliedState[16].alignmentReq = 1;
            eng.entityAlign = 1;
            eng.applied[16].itemId = 0x50B571707E50B51Aull;
        }, log);
        CHECK(log.Count("matched 19 pending 0 alignGated 0 mismatch 1 of 20") >= 1);
        CHECK(log.Count("item=0x50B571707E50B51A") >= 1);
        CHECK(log.Count("LoadoutAppliedPairsMismatch") >= 1);
    }

    SUBCASE("the second operand could not be read: refused by name, never scored as a skip") {
        CaptureLog log;
        drive([](EngineFake& eng) {
            eng.appliedState[16].alignmentReq = 1;
            eng.entityAlignOk = false;
            eng.applied[16].itemId = 0;
        }, log);
        CHECK(log.Count("LoadoutAlignmentUnverified") >= 1);
        CHECK(log.Count("alignGated 1") == 0);
    }

    SUBCASE("the entry point did not bind: the step refuses rather than guessing the operand") {
        CaptureLog log;
        drive([](EngineFake& eng) {
            eng.appliedState[16].alignmentReq = 1;
            eng.entityAlignBound = false;
            eng.applied[16].itemId = 0;
        }, log);
        CHECK(log.Count("LoadoutStepNotBound") >= 1);
        CHECK(log.Count("alignGated 1") == 0);
    }

    SUBCASE("live alignment is not the recorded one: refused before a slot is decided by it") {
        CaptureLog log;
        drive([](EngineFake& eng) {
            eng.appliedState[16].alignmentReq = 1;
            eng.entityAlign = 1;
            eng.descAlignment = 0;               // the recording says this entrant was alignment 0
            eng.applied[16].itemId = 0;
        }, log);
        CHECK(log.Count("LoadoutAlignmentMismatch") >= 1);
        CHECK(log.Count("alignGated 1") == 0);
    }

    SUBCASE("no slot carries a requirement: the operand is never read and the line says so") {
        CaptureLog log;
        drive([](EngineFake& eng) { eng.entityAlignBound = false; }, log);
        CHECK(log.Count("matched 20 pending 0 alignGated 0 mismatch 0 of 20") >= 1);
        CHECK(log.Count("entityAlign=notAsked") >= 1);
    }

    SUBCASE("a row the player wore nothing in is skipped by BOTH loops, so the tallies close") {
        CaptureLog log;
        drive([](EngineFake& eng) { eng.unfilledRow = 7; }, log);
        CHECK(log.Count("L1 compared 19, unfilled 1, of 20 recorded rows") >= 1);
        CHECK(log.Count("matched 19 pending 0 alignGated 0 mismatch 0 of 19") >= 1);
        CHECK(log.Count("LoadoutAppliedPairsMismatch") == 0);
        CHECK(log.Count("[identity] loadout slot ") == 0);
    }

    SUBCASE("no engine entry carried the key: the words say so, and no zero reads as a reading") {
        CaptureLog log;
        drive([](EngineFake& eng) {
            eng.applied[5].slotKey = 0x0F0F0F0F0F0F0F0Full;
        }, log);
        CHECK(log.Count("matched 19 pending 0 alignGated 0 mismatch 1 of 20") >= 1);
        CHECK(log.Count("item=noEngineSlot pending=noEngineSlot flags=- align=-") >= 1);
    }
}

// ===============================================================================================
// AUD-W2-P1-34 P1-38 F1.  THE LIVE FALLBACK.  The publish landed, the enable fired, and the
// engine's own apply took one of the two silent arms DIAG-MODEL-SWAP ranks for this entrant --
// so the read-back holds nothing and, before this, `loadoutAttempted` latched over an entrant
// wearing nothing for the rest of the match.  Our apply is the only one we control on this path.
// ===============================================================================================
TEST_CASE("W2-P1-40: the enable applied nothing, so L5 applies once itself and re-reads") {
    using namespace loadoutdrive;
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    Wiring w;
    Seed(w.doc, w.eng, w.rec, 20);
    w.eng.appliedFromApplyCall = 1;   // nothing is held until an apply has run

    FakeIdentity id;
    FakeHopEngine e;
    e.inCs = true;
    e.enabled = true;
    e.physCi = true;
    PawnProvision::ActorHops h = HopsFor(e);
    g_hops = &h;

    CaptureLog log;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.netGame = &BigNetGame;
    env.hops = &h;
    env.log = &log;
    env.recordedLoadout = &ProvideRecorded;
    env.loadoutEngine = &ProvideEngineWithHopGate;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), DescWithNumbers()));

    uint32_t done = 0;
    for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks + 4 && done == 0; ++i) done += p.Pump();
    CHECK(done == 1);
    CHECK(p.Ledger().provisioned == 1);

    // READ, APPLY, READ AGAIN -- in that order, and exactly once.  The first read-back is what
    // decides there is anything to fall back to; a fallback that applied before reading would be
    // the unconditional apply this ordering was built to remove.
    CHECK(w.eng.order == "attach;parse;publish;readback;apply;readback;align;");
    CHECK(w.eng.applyItemCalls == 1);
    CHECK(log.Count("appliedBy=fallback") == 1);
    CHECK(log.Count("matched 20 pending 0 alignGated 0 mismatch 0 of 20") >= 1);
    CHECK(log.Count("loadout: applied 1/1 attempted of 1 live (20/20 pairs)") >= 1);
    CHECK(log.Count("LoadoutAppliedPairsMismatch") == 0);
}

// AND THE CONTROL: an engine that DID dress the entrant off its own enable is not applied to a
// second time.  Without this the case above passes for a build that simply applies unconditionally
// -- which is the ordering defect W2-P1-38 removed.
TEST_CASE("W2-P1-40: an enable that dressed the entrant gets no second apply from us") {
    using namespace loadoutdrive;
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    Wiring w;
    Seed(w.doc, w.eng, w.rec, 20);

    FakeIdentity id;
    FakeHopEngine e;
    e.inCs = true;
    e.enabled = true;
    e.physCi = true;
    PawnProvision::ActorHops h = HopsFor(e);
    g_hops = &h;

    CaptureLog log;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.netGame = &BigNetGame;
    env.hops = &h;
    env.log = &log;
    env.recordedLoadout = &ProvideRecorded;
    env.loadoutEngine = &ProvideEngineWithHopGate;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), DescWithNumbers()));

    uint32_t done = 0;
    for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks + 4 && done == 0; ++i) done += p.Pump();
    CHECK(done == 1);
    CHECK(w.eng.applyItemCalls == 0);
    CHECK(w.eng.order == "attach;parse;publish;readback;align;");
    CHECK(log.Count("appliedBy=engine") >= 1);
}

// ===============================================================================================
// AUD-W2-P1-34 P1-38 F2.  The publish half runs in the window the entrant is admitted, which is
// the youngest the per-user profile record ever is.  L2's two answers about THAT record are
// engine state, they clear on a later window, and latching on them sent the enable out at
// `Loadouts : 0`.
// ===============================================================================================
TEST_CASE("W2-P1-40: an attach that does not read back on window 1 is re-asked, not latched") {
    using namespace loadoutdrive;
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    Wiring w;
    Seed(w.doc, w.eng, w.rec, 20);
    w.eng.attachBlindWindows = 1;   // the engine has not committed the profile record yet

    FakeIdentity id;
    FakeHopEngine e;
    e.inCs = true;
    e.enabled = true;
    e.physCi = true;
    PawnProvision::ActorHops h = HopsFor(e);
    g_hops = &h;

    CaptureLog log;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.netGame = &BigNetGame;
    env.hops = &h;
    env.log = &log;
    env.recordedLoadout = &ProvideRecorded;
    env.loadoutEngine = &ProvideEngineWithHopGate;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), DescWithNumbers()));

    // WINDOW 1: refused at L2, and NO enable was issued -- the entrant is held instead.
    CHECK(p.Pump() == 0);
    CHECK(p.Ledger().participateIssued == 0);
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::LoadoutNotPublishedBeforeEnable);
    // ...and the count on that line is the one nobody read.  L4 never ran, so printing 0 would
    // report an empty live table for a table this pass never asked about.
    CHECK(log.Count("loadout publish LoadoutDocumentNotAttached -- live instances unread, pairs=unread") == 1);

    // WINDOW 2: the record is there, the publish takes, and the entrant is dressed.
    uint32_t done = 0;
    for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks * 4 && done == 0; ++i) done += p.Pump();
    CHECK(done == 1);
    CHECK(p.Ledger().provisioned == 1);
    CHECK(log.Count("loadout publish None -- live instances 1, pairs=unread") == 1);
    CHECK(log.Count("compValid=notAsked inline=unread queued=unread -> gate=Unasked, before the enable") >= 1);
    CHECK(log.Count("loadout: applied 1/1 attempted of 1 live (20/20 pairs)") >= 1);
}

TEST_CASE("W2-P1-40: a profile that is not yet usable is re-asked on the same rule") {
    using namespace loadoutdrive;
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    Wiring w;
    Seed(w.doc, w.eng, w.rec, 20);
    w.eng.unusableWindows = 1;

    FakeIdentity id;
    FakeHopEngine e;
    e.inCs = true;
    e.enabled = true;
    e.physCi = true;
    PawnProvision::ActorHops h = HopsFor(e);
    g_hops = &h;

    CaptureLog log;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.netGame = &BigNetGame;
    env.hops = &h;
    env.log = &log;
    env.recordedLoadout = &ProvideRecorded;
    env.loadoutEngine = &ProvideEngineWithHopGate;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), DescWithNumbers()));

    CHECK(p.Pump() == 0);
    CHECK(p.Ledger().participateIssued == 0);
    CHECK(log.Count("loadout publish LoadoutDocumentNotUsable -- live instances unread, pairs=unread") == 1);
    uint32_t done = 0;
    for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks * 4 && done == 0; ++i) done += p.Pump();
    CHECK(done == 1);
    CHECK(log.Count("loadout: applied 1/1 attempted of 1 live (20/20 pairs)") >= 1);
}

// AND THE RE-ASKS ARE BOUNDED, BY NAME.  An engine state that has not cleared in the budget is
// not going to, and an entrant held for ever loses its pawn as well as its clothes -- so the
// hold is released, the enable goes out, and the census SAYS the retries ran out rather than
// going quiet in a way that reads like a refusal that was never retryable.
TEST_CASE("W2-P1-40: publish re-asks are bounded and the exhaustion is named") {
    using namespace loadoutdrive;
    Reset();
    SetUpLobby(0x00025F1A7C3D1A2Bull);
    Wiring w;
    Seed(w.doc, w.eng, w.rec, 20);
    w.eng.attachBlindWindows = 99;   // it never clears

    FakeIdentity id;
    FakeHopEngine e;
    e.inCs = true;
    e.enabled = true;
    e.physCi = true;
    PawnProvision::ActorHops h = HopsFor(e);
    g_hops = &h;

    CaptureLog log;
    PawnProvisioner p;
    ProvisionEnv env = Armed(id, 2);
    env.netGame = &BigNetGame;
    env.hops = &h;
    env.log = &log;
    env.recordedLoadout = &ProvideRecorded;
    env.loadoutEngine = &ProvideEngineWithHopGate;
    p.Arm(env);
    REQUIRE(p.Request(UserSlot(0), DescWithNumbers()));

    uint32_t done = 0;
    for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks * 4 && done == 0; ++i) done += p.Pump();
    CHECK(done == 1);
    CHECK(p.Ledger().provisioned == 1);          // the pawn is placed either way
    CHECK(log.Count("loadout publish retries exhausted after 2 re-asks -- "
                    "LoadoutDocumentNotAttached, the enable goes out undressed") == 1);
    // The first attempt plus kPublishRetryLimit re-asks, and not one window more.
    CHECK(log.Count("loadout publish LoadoutDocumentNotAttached") == 3);
}

// ===============================================================================================
// AUD-W2-P1-34 P1-40 F1 + FACTS-VIEWER-ID64.  The census line has to tell a synthetic id from a
// regression that hands the engine the recorded one, and the synthesis has to stay off the ids
// the live session already holds -- the viewer's own among them.
// ===============================================================================================

// A netgame whose entrant table is laid out the way sub_140152D80/sub_140152210 lay it out:
// entrant 0 is the local viewer at the base with NO stride, and every further entrant is one
// NETGAME.PAWN_TABLE_STRIDE step on.  Every displacement below is a registry row.
alignas(16) uint8_t gXplatNetGame[facts::ENTITY_USER_SLOT_TABLE_MAX * facts::NETGAME_PAWN_TABLE_STRIDE +
                                  facts::NETGAME_ENTRANT_XPLAT_ID64_OFF + 64];
void* XplatNetGame() { return gXplatNetGame; }
void PlantEntrantId(uint32_t entrant, uint64_t id) {
    memcpy(gXplatNetGame + size_t(entrant) * facts::NETGAME_PAWN_TABLE_STRIDE +
               facts::NETGAME_ENTRANT_XPLAT_ID64_OFF,
           &id, sizeof id);
}
void ClearEntrantIds() { memset(gXplatNetGame, 0, sizeof gXplatNetGame); }

TEST_CASE("W2-P1-43: the synthesis avoids the viewer's own id64 and every entrant's") {
    ClearEntrantIds();
    XplatIdsInUse empty{};
    REQUIRE(ReadXplatIdsInUse(XplatNetGame(), empty));
    CHECK(empty.known);
    CHECK(empty.count == 0);            // an all-zero table holds no id: 0 is not an id in use

    PlayerRecord rec{};
    rec.xplatformId = 0x0002ABCDEF001234ull;
    uint64_t plain = 0;
    REQUIRE(SyntheticXplatformId(rec.xplatformId, 3, empty, plain));

    // THE VIEWER'S OWN, read at entrant 0 with no stride, is avoided.
    ClearEntrantIds();
    PlantEntrantId(0, plain);
    XplatIdsInUse withViewer{};
    REQUIRE(ReadXplatIdsInUse(XplatNetGame(), withViewer));
    CHECK(withViewer.count == 1);
    CHECK(withViewer.Has(plain));
    uint64_t off = 0;
    REQUIRE(SyntheticXplatformId(rec.xplatformId, 3, withViewer, off));
    CHECK(off != plain);
    CHECK((off & kXplatIndexMask) == 3);

    // AND EVERY OTHER ENTRANT'S, at the stride the spawn path uses.
    ClearEntrantIds();
    PlantEntrantId(5, off);
    XplatIdsInUse withEntrant{};
    REQUIRE(ReadXplatIdsInUse(XplatNetGame(), withEntrant));
    CHECK(withEntrant.Has(off));
    CHECK(!withEntrant.Has(plain));     // the stride is read, not guessed: slot 0 is still clear

    // A set that holds every id the budget can reach REFUSES by name rather than authoring a
    // colliding one.  The salts are enumerated the way the synthesis enumerates them.
    XplatIdsInUse all{};
    all.known = true;
    for (unsigned i = 0; i < kXplatSaltAttempts; ++i) {
        XplatIdsInUse sofar = all;
        uint64_t next = 0;
        REQUIRE(SyntheticXplatformId(rec.xplatformId, 3, sofar, next));
        all.ids[all.count++] = next;
    }
    uint64_t exhausted = 0xFEEDull;
    CHECK(!SyntheticXplatformId(rec.xplatformId, 3, all, exhausted));
    CHECK(exhausted == 0xFEEDull);      // the out parameter is left alone on a refusal

    // One salt short of that is still authorable, so the budget is what refused and not the mix.
    XplatIdsInUse allButOne = all;
    --allButOne.count;
    uint64_t last = 0;
    CHECK(SyntheticXplatformId(rec.xplatformId, 3, allButOne, last));
    ClearEntrantIds();
}

TEST_CASE("W2-P1-43: the entrant census line prints the salt bits, not the entrant index") {
    ClearEntrantIds();
    FakeIdentity id;
    CaptureLog log;
    ProvisionEnv env = Armed(id);
    env.netGame = &XplatNetGame;
    env.log = &log;
    PawnProvisioner p;
    p.Arm(env);

    PlayerRecord rec{};
    rec.xplatformId = 0x0002ABCDEF001234ull;   // a real account id: its high half is the platform's
    rec.teamId16 = 0;
    const JoinDesc d = JoinDescFromRecord(rec, 0);
    REQUIRE(p.Request(UserSlot(0), d));

    // The old line masked the LOW 16 bits, which the synthesis SETS to the entrant index: it read
    // 0x0000 for a correct id and 0x0000 for a regression to the recorded one alike.
    CHECK(log.Count("xplat salt=0x0000 ") == 0);
    REQUIRE(log.Count("entrant 0 xplat salt=") == 1);

    // The id the provisioner authored, recomputed through the same public surface it used: the
    // set is the one on the table right now and the mix is deterministic within a session.
    XplatIdsInUse live{};
    REQUIRE(ReadXplatIdsInUse(XplatNetGame(), live));
    uint64_t authored = 0;
    REQUIRE(SyntheticXplatformId(rec.xplatformId, 0, live, authored));
    char want[96];
    snprintf(want, sizeof want, "entrant 0 xplat salt=0x%04X hi=0x%04X rec=0x%04X inuse=read",
             unsigned((authored >> kXplatSaltShift) & kXplatIndexMask),
             unsigned((authored >> 48) & kXplatIndexMask),
             unsigned((rec.xplatformId >> 48) & kXplatIndexMask));
    CHECK(log.Count(want) == 1);

    // THE DISCRIMINATOR.  `d.xplatformId = rec.xplatformId' makes the authored and recorded high
    // halves equal on every entrant; the synthesis makes them differ.  Both are on the line.
    CHECK(((authored >> 48) & kXplatIndexMask) != ((rec.xplatformId >> 48) & kXplatIndexMask));
    CHECK(authored != rec.xplatformId);
    CHECK(((authored >> kXplatSaltShift) & kXplatIndexMask) !=
          ((rec.xplatformId >> kXplatSaltShift) & kXplatIndexMask));

    // AND THE ID IS LATCHED: a Provision handing the author's descriptor back does not re-author
    // it, so the engine is never told two ids for one entrant.
    p.Provision(UserSlot(0), d);
    CHECK(log.Count("entrant 0 xplat salt=") == 1);
    CHECK(log.Count(want) == 1);
    ClearEntrantIds();
}

TEST_CASE("W2-P1-43: an entrant whose id cannot be authored clear is refused by name") {
    ClearEntrantIds();
    PlayerRecord rec{};
    rec.xplatformId = 0x0002ABCDEF001234ull;
    rec.teamId16 = 0;

    // Plant every id the budget can reach into the entrant table, so the walk finds them all.
    XplatIdsInUse all{};
    all.known = true;
    for (unsigned i = 0; i < kXplatSaltAttempts; ++i) {
        XplatIdsInUse sofar = all;
        uint64_t next = 0;
        REQUIRE(SyntheticXplatformId(rec.xplatformId, 0, sofar, next));
        all.ids[all.count++] = next;
        PlantEntrantId(i, next);
    }

    FakeIdentity id;
    CaptureLog log;
    ProvisionEnv env = Armed(id);
    env.netGame = &XplatNetGame;
    env.log = &log;
    PawnProvisioner p;
    p.Arm(env);
    CHECK(!p.Request(UserSlot(0), JoinDescFromRecord(rec, 0)));
    CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::XplatIdCollision);
    CHECK(std::string(PawnProvision::RefusalName(p.LastProvisionRefusal())) == "XplatIdCollision");
    ClearEntrantIds();
}

// ===============================================================================================
// DIAG-ENTITY-COLLISION 4.  The engine evicts a duplicate entrant on {platform, id64}, and ~0 is
// the viewer's own id64 on a `-offline' host: authoring it asked the engine to remove the person
// watching.  The id we author is ours, unique per entrant per session, and nobody else's.
// ===============================================================================================
TEST_CASE("W2-P1-40: the authored xplatform id is synthetic, per-entrant and never -1") {
    PlayerRecord a{};
    PlayerRecord b{};
    a.xplatformId = 0;                       // a capture that never recorded one
    b.xplatformId = 0x0002DEADBEEF0001ull;   // and one that did -- both are re-authored
    a.teamId16 = b.teamId16 = 0;
    const uint64_t viewer = 0x00025F1A7C3D1A2Bull;
    XplatIdsInUse held{};
    held.known = true;
    held.ids[held.count++] = viewer;

    const JoinDesc da = JoinDescFromRecord(a, 0, &held);
    const JoinDesc db = JoinDescFromRecord(b, 1, &held);

    CHECK(da.xplatformId != ~uint64_t(0));
    CHECK(db.xplatformId != ~uint64_t(0));
    CHECK(da.xplatformId != 0);
    CHECK(db.xplatformId != 0);
    CHECK(da.xplatformId != viewer);
    CHECK(db.xplatformId != viewer);
    // NOT THE RECORDED ONE: replaying a capture on the account that made it is the case the
    // eviction is keyed for.
    CHECK(db.xplatformId != b.xplatformId);
    // DISTINCT BY CONSTRUCTION, not by luck: the entrant index is carried in the low 16 bits.
    CHECK(da.xplatformId != db.xplatformId);
    CHECK((da.xplatformId & kXplatIndexMask) == 0);
    CHECK((db.xplatformId & kXplatIndexMask) == 1);
    // The same entrant asked twice in one session gets one id -- a second admission of the same
    // recorded slot must not read to the engine as a second person.
    CHECK(JoinDescFromRecord(b, 1, &held).xplatformId == db.xplatformId);
    // An id that lands on one already in use is re-salted off it, and stays distinct from every
    // other entrant's while it does.
    XplatIdsInUse none{};
    none.known = true;
    uint64_t collide = 0;
    REQUIRE(SyntheticXplatformId(a.xplatformId, 4, none, collide));
    XplatIdsInUse one{};
    one.known = true;
    one.ids[one.count++] = collide;
    uint64_t moved = 0;
    REQUIRE(SyntheticXplatformId(a.xplatformId, 4, one, moved));
    CHECK(moved != collide);
    CHECK((moved & kXplatIndexMask) == 4);
    // Everything else the descriptor carries is untouched by the substitution.
    CHECK(db.team == b.teamId16);
    CHECK(db.alignment == b.alignment16);
    CHECK(db.loadoutNumber == b.loadoutNumber);
    CHECK(db.loadoutNumberBody == b.loadoutNumberBody);
    CHECK(db.entrantJson.p == b.entrantJson.p);
    CHECK(db.entrantJson.n == b.entrantJson.n);
}

// -------------------------------------------------------------------------------------------
// The sweeps the stage-0 detour runs for OUR entrants, driven on a provisioner
// whose entrant was admitted and whose actor the engine left off -- the state the backstop exists
// for, and the state the model-swap arms used to fire in for recorded spectators too.
// -------------------------------------------------------------------------------------------
#include "engine/identity_access_facts.h"

namespace spawnedseed {
namespace iaf = replay::identity_facts;
namespace ia  = replay::identity_access;

struct PairFake {
    unsigned calls = 0;
    uint16_t sawIndex = kSlotUnmapped;
    bool     issues = true;    // false: the hop refused (wrong phase / entry point moved)
};
PairFake g_pair;
bool HopPairFake(void*, uint16_t index) {
    ++g_pair.calls;
    g_pair.sawIndex = index;
    return g_pair.issues;
}

// A CR15NetPlayerModelSwapperCS at the plugin's own offsets, with one entrant's record and the
// chain down to its current-model qword.
struct FakeSwapper {
    alignas(16) uint8_t cs[0x200] = {};
    uint16_t            handleTable[8] = {};
    alignas(16) uint8_t record[iaf::kSWAPPER_RECORD_STRIDE] = {};
    alignas(16) uint8_t inst[0x200] = {};
    uint16_t            idxTable[8] = {};
    alignas(16) uint8_t pool[0x100] = {};
    alignas(16) uint8_t slotObj[0xB18] = {};

    FakeSwapper() {
        void* t = handleTable;  memcpy(cs + iaf::kSWAPPER_HANDLE_TABLE_OFF, &t, sizeof t);
        void* r = record;       memcpy(cs + iaf::kSWAPPER_RECORD_BASE_OFF, &r, sizeof r);
        void* ip = inst;        memcpy(record + iaf::kSWAPPER_MODELINST_OFF, &ip, sizeof ip);
        void* it = idxTable;    memcpy(inst + iaf::kSWAPPER_INST_INDEX_TABLE_OFF, &it, sizeof it);
        void* pp = pool;        memcpy(inst + iaf::kSWAPPER_INST_POOL_BASE_OFF, &pp, sizeof pp);
        void* so = slotObj;     memcpy(pool + iaf::kSWAPPER_INST_SLOT_PTR_OFF, &so, sizeof so);
    }
    uint64_t& Current() {
        return *reinterpret_cast<uint64_t*>(slotObj + iaf::kSWAPPER_CURRENT_MODEL_OFF);
    }
};

constexpr uint64_t kPooledResidue = 0xDAC6537A23236325ull;
unsigned g_beginCalls = 0;
void FakeBegin(void*, uint32_t, uint64_t) { ++g_beginCalls; }

constexpr uint64_t kChassisSym = 0x00C4A55150000001ull;
uint64_t FakeSlotSymbol(const char* name) { return strcmp(name, "chassis") == 0 ? kChassisSym : 0; }
uint64_t g_recordedChassis = 0;
bool FakeRecordedLoadout(uint32_t, identity::RecordedLoadout& out, const char**) {
    out = identity::RecordedLoadout{};
    out.hasSlotKeys = true;
    out.filledPairCount = 1;
    out.pairs[0] = {kChassisSym, g_recordedChassis};
    return true;
}

// One recorded entrant, admitted, registered, and left DISABLED by the engine: neither the assign
// nor the refresh enables, which is the run DIAG-ENABLE-INVALID-USER measured.
struct Rig {
    FakeIdentity              id;
    FakeHopEngine             eng;
    PawnProvision::ActorHops  hops;
    CensusEntity              ent;
    CaptureLog                log;
    FakeSwapper               swapper;
    PawnProvisioner           p;
    uint16_t                  index = kSlotUnmapped;

    Rig(uint16_t recordedTeam, bool backstop, bool equipmentRecv = false) {
        Reset();
        SetUpLobby(0x00025F1A7C3D1A2Bull);
        g_pair = PairFake{};
        g_beginCalls = 0;
        eng.inCs = true;
        eng.assignEnables = false;
        eng.refreshEnables = false;
        hops = HopsFor(eng);
        hops.participatePair = HopPairFake;
        ent.modelResolves = false;

        ProvisionEnv env = Armed(id, 2);
        env.log    = &log;
        env.hops   = &hops;
        env.entity = &ent;
        env.participateBackstop = backstop;
        env.equipmentRecv       = equipmentRecv;
        env.recordedLoadout     = FakeRecordedLoadout;
        p.Arm(env);

        JoinDesc desc{};
        desc.team = recordedTeam;
        REQUIRE(p.Request(UserSlot(0), desc));
        index = id.nextIndex;
        REQUIRE(index < 8);
        for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks + 4; ++i) p.Pump();
        REQUIRE(id.inserts == 1);
        REQUIRE_FALSE(eng.enabled);

        swapper.handleTable[index] = 0;
        swapper.Current() = kPooledResidue;
        ia::SwapperInjectForTest(swapper.cs, nullptr);
        ia::SwapperInjectBeginForTest(&FakeBegin);
        ia::EquipmentSlotSymbolInjectForTest(&FakeSlotSymbol);
    }
    ~Rig() {
        ia::SwapperClearInjectForTest();
        ia::EquipmentRecvClearInjectForTest();
    }

    // What the detour does every stage-0 frame, in its order.
    void Frame() {
        p.ParticipateBackstopSweep(gNetGame);
        p.EmulateEquipmentRecvSweep(gNetGame);
        p.UndressPooledSwapperSweep(gNetGame);
    }
};

}  // namespace spawnedseed

TEST_CASE("ENG-SPAWNED-SEED backstop: fires once, only armed, only for a spawned, disabled, recorded player") {
    using spawnedseed::g_pair;

    SUBCASE("armed, recorded on team 0, spawned, actor off: one pair, with the entrant's index, never again") {
        spawnedseed::Rig rig(/*team*/ 0, /*backstop*/ true);
        REQUIRE(rig.eng.spawned);
        rig.p.ParticipateBackstopSweep(gNetGame);
        CHECK(g_pair.calls == 1);
        CHECK(g_pair.sawIndex == rig.index);
        // The actor is STILL off -- the enable lands later, or never -- and the pair is not re-issued.
        REQUIRE_FALSE(rig.eng.enabled);
        for (int i = 0; i < 5; ++i) rig.p.ParticipateBackstopSweep(gNetGame);
        CHECK(g_pair.calls == 1);
        CHECK(rig.log.Count("participate backstop idx") == 2);   // the issue, and one enable re-read
    }
    SUBCASE("the flag off: nothing, for the same entrant") {
        spawnedseed::Rig rig(0, /*backstop*/ false);
        REQUIRE(rig.eng.spawned);
        rig.p.ParticipateBackstopSweep(gNetGame);
        CHECK(g_pair.calls == 0);
    }
    SUBCASE("a recorded spectator is never driven, even with a spawned record and the actor off") {
        spawnedseed::Rig rig(/*team*/ 2, true);
        rig.eng.spawned = true;
        rig.p.ParticipateBackstopSweep(gNetGame);
        CHECK(g_pair.calls == 0);
    }
    SUBCASE("an actor the engine already enabled is left alone") {
        spawnedseed::Rig rig(0, true);
        rig.eng.enabled = true;
        rig.p.ParticipateBackstopSweep(gNetGame);
        CHECK(g_pair.calls == 0);
    }
    SUBCASE("a `spawned` record the engine's own reader answers false for is not driven") {
        spawnedseed::Rig rig(0, true);
        rig.eng.spawned = false;
        rig.p.ParticipateBackstopSweep(gNetGame);
        CHECK(g_pair.calls == 0);
    }
    SUBCASE("a local user, or a count that would not prove, is not driven") {
        spawnedseed::Rig rig(0, true);
        rig.eng.localUser = true;
        rig.p.ParticipateBackstopSweep(gNetGame);
        rig.eng.localUser = false;
        rig.eng.localUserUnreadable = true;
        rig.p.ParticipateBackstopSweep(gNetGame);
        CHECK(g_pair.calls == 0);
    }
    SUBCASE("a refused pair is not latched: it is re-asked, and latches on the issue") {
        spawnedseed::Rig rig(0, true);
        g_pair.issues = false;
        rig.p.ParticipateBackstopSweep(gNetGame);
        rig.p.ParticipateBackstopSweep(gNetGame);
        CHECK(g_pair.calls == 2);
        g_pair.issues = true;
        rig.p.ParticipateBackstopSweep(gNetGame);
        rig.p.ParticipateBackstopSweep(gNetGame);
        CHECK(g_pair.calls == 3);
    }
}

TEST_CASE("No model-swap arm fires for a recorded spectator or a sentinel chassis") {
    using spawnedseed::g_beginCalls;
    using spawnedseed::kPooledResidue;

    SUBCASE("CONTROL -- a recorded player with a real chassis: the reset and one BeginModelSwap go out") {
        spawnedseed::g_recordedChassis = 0xCA4CC178F3707385ull;
        spawnedseed::Rig rig(/*team*/ 1, false, /*equipmentRecv*/ true);
        for (int i = 0; i < 4; ++i) rig.Frame();
        CHECK(rig.swapper.Current() == ~uint64_t(0));
        CHECK(g_beginCalls == 1);
        CHECK(rig.log.Count("equipment RECV (ENG-EQUIPMENT-RECV)") >= 1);
    }
    SUBCASE("a recorded spectator: no equipment RECV, no reset, no BeginModelSwap") {
        spawnedseed::g_recordedChassis = 0xCA4CC178F3707385ull;
        spawnedseed::Rig rig(/*team*/ 2, false, true);
        for (int i = 0; i < 4; ++i) rig.Frame();
        CHECK(rig.swapper.Current() == kPooledResidue);
        CHECK(g_beginCalls == 0);
        CHECK(rig.log.Count("equipment RECV") == 0);
        CHECK(rig.log.Count("BeginModelSwap") == 0);
    }
    SUBCASE("a recorded player whose chassis is the unfilled sentinel: RECV latched off, and no swap either") {
        spawnedseed::g_recordedChassis = ~uint64_t(0);
        spawnedseed::Rig rig(/*team*/ 1, false, true);
        for (int i = 0; i < 4; ++i) rig.Frame();
        CHECK(rig.log.Count("equipment RECV not driven") == 1);
        CHECK(rig.swapper.Current() == kPooledResidue);
        CHECK(g_beginCalls == 0);
        CHECK(rig.log.Count("BeginModelSwap") == 0);
    }
}

TEST_CASE("SwapDriveAllowed is the spectator predicate and the sentinel latch, nothing wider") {
    CHECK(PawnProvision::SwapDriveAllowed(0, false));
    CHECK(PawnProvision::SwapDriveAllowed(1, false));
    // A team the recording never carried still gets the swap drives it got before this packet.
    CHECK(PawnProvision::SwapDriveAllowed(PawnProvision::kRoleNotRecorded, false));
    CHECK_FALSE(PawnProvision::SwapDriveAllowed(2, false));
    CHECK_FALSE(PawnProvision::SwapDriveAllowed(4, false));
    CHECK_FALSE(PawnProvision::SwapDriveAllowed(1, true));
}

namespace spawnedseed {
bool g_seededAnswer = false;
unsigned g_seededAsks = 0;
bool HopSeededFake(void*, uint16_t) { ++g_seededAsks; return g_seededAnswer; }
}  // namespace spawnedseed

// The engine enabled the actor during the entrant add because the seed had already written
// `spawned`.  Participate then finds the role matched, the record set and the actor ON -- the
// state that used to be a skip, which the `provisioned` gate refuses ParticipationNotIssued.
TEST_CASE("An actor the seeded add enabled is claimed as an issue, and only then") {
    FakeHopEngine e;
    e.inCs = true;
    e.team = 1;
    e.spawned = true;
    e.enabled = true;
    PawnProvision::ActorHops h = HopsFor(e);
    h.seededBeforeAdd = spawnedseed::HopSeededFake;
    spawnedseed::g_seededAsks = 0;
    PawnProvision::DriveGate g;
    PawnProvision::HopDrive d = DriveFor(g, 1, /*entrantCount*/ 6);

    SUBCASE("the seed wrote it: Issued via the add, and no engine call is made to earn it") {
        spawnedseed::g_seededAnswer = true;
        const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(h, 7, d);
        CHECK(o.state == PawnProvision::Participation::Issued);
        CHECK(g.via == PawnProvision::ParticipateDrive::SeededEntrantAdd);
        CHECK(strcmp(PawnProvision::ParticipateDriveName(g.via), "seededEntrantAdd") == 0);
        CHECK(g.issues == 1);
        CHECK(g.issuedRole == 1);
        CHECK(e.assigns + e.refreshes + e.spawnWrites == 0);
        // Once per epoch, like every other issue.
        CHECK(PawnProvision::Participate(h, 7, d).state == PawnProvision::Participation::SkippedAlreadyDone);
        CHECK(g.issues == 1);
    }
    SUBCASE("the seed did not: the enabled actor stays a skip, never an adoption") {
        spawnedseed::g_seededAnswer = false;
        const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(h, 7, d);
        CHECK(o.state == PawnProvision::Participation::SkippedAlreadyDone);
        CHECK(spawnedseed::g_seededAsks == 1);
        CHECK(g.issues == 0);
        CHECK(g.via == PawnProvision::ParticipateDrive::None);
    }
    SUBCASE("the seed wrote it and the actor is still OFF: nothing is booked off the seed alone") {
        spawnedseed::g_seededAnswer = true;
        e.enabled = false;
        e.refreshEnables = false;
        const PawnProvision::ParticipateOutcome o = PawnProvision::Participate(h, 7, d);
        CHECK(o.state == PawnProvision::Participation::Refused);
        CHECK(g.via != PawnProvision::ParticipateDrive::SeededEntrantAdd);
        CHECK(spawnedseed::g_seededAsks == 0);
    }
}

TEST_CASE("A seeded, engine-enabled entrant reaches Provisioned instead of ParticipationNotIssued") {
    for (const bool seeded : {false, true}) {
        CAPTURE(seeded);
        Reset();
        SetUpLobby(0x00025F1A7C3D1A2Bull);
        FakeIdentity id;
        FakeHopEngine e;
        // What the engine leaves behind when the add ran against spawned == 1.
        e.inCs = true;
        e.team = 1;
        e.spawned = true;
        e.enabled = true;
        e.physCi = true;
        PawnProvision::ActorHops h = HopsFor(e);
        h.seededBeforeAdd = spawnedseed::HopSeededFake;
        spawnedseed::g_seededAnswer = seeded;

        PawnProvisioner p;
        ProvisionEnv env = Armed(id, 2);
        env.hops = &h;
        p.Arm(env);
        JoinDesc desc{};
        desc.team = 1;
        REQUIRE(p.Request(UserSlot(0), desc));
        for (uint32_t i = 0; i < PawnProvisioner::kRetryTicks + 4; ++i) p.Pump();

        CHECK(e.assigns + e.refreshes + e.spawnWrites == 0);
        if (seeded) {
            CHECK(p.Ledger().provisioned == 1);
            CHECK(p.Ledger().participateIssued == 1);
            CHECK(p.Provisioned(UserSlot(0)));
        } else {
            CHECK(p.Ledger().provisioned == 0);
            CHECK(p.LastProvisionRefusal() == PawnProvision::Refusal::ParticipationNotIssued);
        }
    }
}
