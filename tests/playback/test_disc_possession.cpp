// POSSESSION: the disc's recorded holder, published as a LEVEL into the one dword the engine's
// holder gates read. Nothing here synthesises a grab or a release -- CR15NetTouchInteractCS derives
// both by diffing that level against its own latch -- so what these cases pin is the level: the
// value composed for a mapped player, the value for an unowned span, the refusal to guess for an
// owner this session never admitted, and the fact that it is republished only when it changes.

#include "doctest.h"

#include <stdint.h>
#include <string.h>

#include "bootstrap/livesafety.h"
#include "capture/streamcodec.h"
#include "fake_disc_entity.h"
#include "playback/core/apply.h"
#include "playback/core/window.h"

using namespace replay;
using namespace replay::playback;
using disctest::DiscEntity;

namespace {

constexpr uint64_t kT0 = 1'000'000;
constexpr uint64_t kDt = 22'222;          // ~45 Hz, the disc's recorded rate
constexpr uint16_t kDiscHandle = 5;
constexpr uint16_t kRecordedSlot = 1;     // the capture's user slot for the player who catches
// ★ THREE DIFFERENT NUMBERS, DELIBERATELY. The capture's slot, the CR15NetGame table INDEX this
// session admitted that player at (the low word of an SR15NetUserId), and the live user SLOT that
// index resolves to -- the value the engine's band gates actually compare. They were all one
// number here, which made every assertion below true by construction: production composes
// `index | gen << 16`, so a fixture whose index IS its slot cannot fail when the two are confused.
constexpr uint16_t kEngineIndex = 4;
constexpr uint16_t kEngineLiveSlot = 11;
constexpr uint16_t kBandLo = 9;           // netGame+224: the first REMOTE slot
constexpr uint16_t kBandHi = 16;          // netGame+220: one past the last
constexpr uint16_t kEngineGen = 7;

BodyIdentity DiscId() {
    BodyIdentity id;
    id.value = 0;
    id.tag = 1;
    id.fill = capture::kFillBodyIdValue | capture::kFillBodyIdTag;
    return id;
}

// owners[i] is the recorded holder of sample i: a capture user slot, or kBodyOwnerNone.
struct Scene {
    Window w;
    DiscEntity ent;
    StateApplier applier;

    static void Build(Window& w, const uint32_t* owners, size_t n) {
        uint32_t seq = 0;
        for (size_t i = 0; i < n; ++i) {
            BodyState s;
            memset(&s, 0, sizeof s);
            s.ori[3] = 1.0f;
            s.com[0] = (float)i;
            s.owner_slot = owners[i];
            // The reference capture's own word shape, not a convenient one: the low nibble is the
            // ownership sequence and bit 9 is held, and the sample that carries a new sender
            // carries the new held bit WITH it -- 000A->020B, 000E->0200, 020D->020E at every
            // hand-over in 2026-09-18_03-33-45. So a fixture that holds them apart is not the file.
            if (i > 0 && owners[i] != owners[i - 1]) seq = (seq + 1) & 0xFu;
            s.bits11 = (owners[i] == kBodyOwnerNone ? 0u : 0x200u) | seq;
            REQUIRE(w.AddBodySample(DiscId(), kT0 + i * kDt, s));
        }
        PlayerState ps;
        memset(&ps, 0, sizeof ps);
        for (size_t i = 0; i < n; ++i) REQUIRE(w.AddPlayerSample(kRecordedSlot, kT0 + i * kDt, ps));
        w.Finalize();

        BodyTrack& disc = w.BodyAt(0);
        disc.handle = PhysHandle(kDiscHandle);
        disc.handle_valid = true;
        disc.claim_owner = true;

        PlayerTrack& pt = w.PlayerAt(0);
        pt.user_id = kEngineIndex;
        pt.user_id_valid = true;
    }

    Scene(const uint32_t* owners, size_t n) {
        Build(w, owners, n);
        ent.band_lo = kBandLo;
        ent.band_hi = kBandHi;
        ent.generation[kEngineIndex] = kEngineGen;
        ent.live_slot[kEngineIndex] = kEngineLiveSlot;   // an admitted entrant is a REMOTE user
        applier.Bind(&ent, nullptr, nullptr, nullptr);
    }

    ApplyStats At(int64_t t, bool reversed = false, bool holdDisc = false) {
        ApplyOptions opt;
        opt.reversed = reversed;
        opt.hold_disc_pose = holdDisc;
        return applier.Apply(w, t, opt);
    }
    uint32_t Owner() const { return ent.owner[kDiscHandle % DiscEntity::kBodies]; }
    BodyTrack& Disc() { return w.BodyAt(0); }
};

constexpr uint32_t kHeldId = (uint32_t)kEngineIndex | ((uint32_t)kEngineGen << 16);

}  // namespace

TEST_CASE("POSSESSION: a mapped recorded owner becomes the engine's own {slot, generation}") {
    const uint32_t owners[] = {kRecordedSlot, kRecordedSlot, kRecordedSlot};
    Scene sc(owners, 3);

    const ApplyStats st = sc.At((int64_t)kT0);
    CHECK(st.disc_owner_stamps == 1);
    CHECK(st.disc_owner_unmapped == 0);
    CHECK(st.disc_owner_fallbacks == 0);
    CHECK(sc.Owner() == kHeldId);
    CHECK(st.disc_owner_engine_slot == kEngineIndex);
    // The whole point of a real id rather than the out-of-table one: the engine still refuses to
    // simulate the body, so the drive keeps working while possession reads as held.
    CHECK_FALSE(sc.ent.IsLocallyControlled(PhysHandle(kDiscHandle)));
    CHECK(st.bodies_installed == 1);
}

TEST_CASE("POSSESSION: an unowned span publishes the out-of-table value, not a user id") {
    const uint32_t owners[] = {kBodyOwnerNone, kBodyOwnerNone};
    Scene sc(owners, 2);

    const ApplyStats st = sc.At((int64_t)kT0);
    CHECK(st.disc_owner_stamps == 1);
    CHECK(st.disc_owner_unmapped == 0);   // "nobody" is an answer, not a failed lookup
    CHECK(sc.Owner() == DiscEntity::kSyntheticOwner);
    CHECK_FALSE(sc.ent.IsLocallyControlled(PhysHandle(kDiscHandle)));
}

TEST_CASE("POSSESSION: an owner this session never admitted is published as unowned, never guessed") {
    const uint32_t owners[] = {9, 9};       // a spectator, or a slot the roster never mapped
    Scene sc(owners, 2);

    const ApplyStats st = sc.At((int64_t)kT0);
    CHECK(st.disc_owner_unmapped == 1);
    CHECK(sc.Owner() == DiscEntity::kSyntheticOwner);
}

TEST_CASE("POSSESSION: a mapped slot whose live generation is gone is published as unowned") {
    const uint32_t owners[] = {kRecordedSlot};
    Scene sc(owners, 1);
    sc.ent.generation[kEngineIndex] = 0xFFFFu;   // the entrant left; the slot holds nobody  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids

    const ApplyStats st = sc.At((int64_t)kT0);
    CHECK(st.disc_owner_unmapped == 1);
    CHECK(sc.Owner() == DiscEntity::kSyntheticOwner);
}

TEST_CASE("POSSESSION: the level is republished only when the recording says it changed") {
    const uint32_t owners[] = {kBodyOwnerNone, kBodyOwnerNone, kRecordedSlot,
                               kRecordedSlot,  kRecordedSlot,  kBodyOwnerNone};
    Scene sc(owners, 6);

    uint32_t stamps = 0;
    for (size_t i = 0; i < 6; ++i) stamps += sc.At((int64_t)(kT0 + i * kDt)).disc_owner_stamps;
    // Six ticks, two possession changes plus the opening publish. The engine derives the grab and
    // the release from those; a count near the tick count would mean the level is not sticking.
    // The closing "nobody" keeps the holder named, so it is not a publish.
    CHECK(stamps == 2);
    // One STORE for two publishes: the span opens unowned, and the claim that takes the body
    // already put the out-of-table value there, so the opening publish has nothing to change.
    CHECK(sc.ent.recorded_stamps == 1);
    CHECK(sc.Owner() == kHeldId);
}

TEST_CASE("POSSESSION: a seek across a catch shows the new holder, and reverse shows the old one") {
    const uint32_t owners[] = {kBodyOwnerNone, kBodyOwnerNone, kRecordedSlot, kRecordedSlot};
    Scene sc(owners, 4);

    sc.At((int64_t)kT0);
    CHECK(sc.Owner() == DiscEntity::kSyntheticOwner);
    // A jump straight past the catch, with no tick in between.
    sc.Disc().cursor.Invalidate();
    sc.At((int64_t)(kT0 + 3 * kDt));
    CHECK(sc.Owner() == kHeldId);
    // ...and back the other way: the node drops its held bit, and the dword keeps the last real
    // holder rather than handing the host's tracker a team-0xFFFF "holder".
    sc.Disc().cursor.Invalidate();
    const ApplyStats back = sc.At((int64_t)kT0, /*reversed=*/true);
    CHECK(sc.Owner() == kHeldId);
    CHECK_FALSE(back.disc_holder.held_bit);
}

TEST_CASE("POSSESSION: paused on one frame, the level is published once and never rewritten") {
    const uint32_t owners[] = {kRecordedSlot, kRecordedSlot};
    Scene sc(owners, 2);

    for (int i = 0; i < 8; ++i) sc.At((int64_t)kT0);
    CHECK(sc.ent.recorded_stamps == 1);
    CHECK(sc.Owner() == kHeldId);
}

TEST_CASE("POSSESSION: the release hands the engine's ORIGINAL owner back, not one of ours") {
    const uint32_t owners[] = {kRecordedSlot, kBodyOwnerNone, kRecordedSlot};
    Scene sc(owners, 3);
    const uint32_t before = sc.Owner();
    REQUIRE(before == DiscEntity::kHostOwner);

    for (size_t i = 0; i < 3; ++i) sc.At((int64_t)(kT0 + i * kDt));
    REQUIRE(sc.Owner() == kHeldId);
    REQUIRE(sc.ent.recorded_stamps == 1);

    CHECK(sc.applier.ReleaseSyntheticOwners(sc.w) == 1);
    CHECK(sc.Owner() == before);
    CHECK_FALSE(sc.Disc().owner_level_published);
    // And the next tick republishes, because nothing is stamped any more.
    sc.At((int64_t)kT0);
    CHECK(sc.Owner() == kHeldId);
}

TEST_CASE("POSSESSION: a store the engine refuses records no claim and no level") {
    const uint32_t owners[] = {kRecordedSlot};
    Scene sc(owners, 1);
    sc.ent.refuse_recorded_stamp = true;

    const ApplyStats st = sc.At((int64_t)kT0);
    CHECK(st.disc_owner_refused == 1);
    CHECK(st.disc_owner_stamps == 0);
    CHECK_FALSE(sc.Disc().owner_level_published);
    // The pre-existing claim still protects the drive: the host was simulating the disc, so the
    // out-of-table value goes on anyway. Possession is lost; the replay is not.
    CHECK(sc.Owner() == DiscEntity::kSyntheticOwner);
}

TEST_CASE("POSSESSION: a recorded owner that lands in the LOCAL band falls back and is counted") {
    const uint32_t owners[] = {kRecordedSlot};
    Scene sc(owners, 1);
    sc.ent.live_slot[kEngineIndex] = 2;   // this build resolved the entrant into the LOCAL band

    const ApplyStats st = sc.At((int64_t)kT0);
    CHECK(st.disc_owner_fallbacks == 1);
    CHECK(sc.Owner() == DiscEntity::kSyntheticOwner);
    CHECK_FALSE(sc.Disc().owner_level_real);
    // The body is still ours to drive, which is the whole reason the fallback exists.
    CHECK_FALSE(sc.ent.IsLocallyControlled(PhysHandle(kDiscHandle)));
}

TEST_CASE("POSSESSION: a track that is not the disc is never stamped from the recording") {
    const uint32_t owners[] = {kRecordedSlot, kRecordedSlot};
    Scene sc(owners, 2);
    sc.Disc().claim_owner = false;

    const ApplyStats st = sc.At((int64_t)kT0);
    CHECK(st.disc_owner_stamps == 0);
    CHECK(sc.ent.recorded_stamps == 0);
    // Neither claimed nor slot-bound, so the applier leaves the host's own owner alone, exactly as
    // it did before this change.
    CHECK(sc.Owner() == DiscEntity::kHostOwner);
    CHECK(st.bodies_locally_controlled == 1);
}

// ---- P-1: a transport discontinuity must not pass the level through an intermediate value ------
//
// The engine does not read our stamps, it reads the dword, and CR15NetTouchInteractCS derives grab
// and release by diffing it. So what these cases assert is the SEQUENCE of values the engine's CI
// sees: a seek with the same holder must write nothing at all, and a seek across a catch must
// write exactly once. A count of stamps could not tell the two apart from a release-and-republish.

namespace {

// Every transport event the battery fires for -- seek, micro-seek, gap cross, direction flip and
// (since UI-2 P1) every scrub step -- reaches the applier as exactly this pair.
void TransportJump(Scene& sc) {
    sc.Disc().cursor.Invalidate();
    sc.applier.ReleaseSyntheticOwners(sc.w, StateApplier::OwnerRelease::KeepDiscLevel);
}

}  // namespace

TEST_CASE("POSSESSION2: seeks with the same holder write the owner dword once and never again") {
    uint32_t owners[64];
    for (uint32_t& o : owners) o = kRecordedSlot;
    Scene sc(owners, 64);

    sc.At((int64_t)kT0);
    REQUIRE(sc.Owner() == kHeldId);
    // Two writes, and the order is POLISH-2's: the claim that makes the body ours reads as NOBODY,
    // then the level names the holder once the node carrying his held bit is installed. The dword
    // never names a holder the node cannot back.
    REQUIRE(sc.ent.owner_writes.size() == 2);
    REQUIRE(sc.ent.owner_writes[0] == DiscEntity::kSyntheticOwner);
    REQUIRE(sc.ent.owner_writes[1] == kHeldId);

    // Forward seek, backward micro-seek, a direction flip, then twenty scrub steps -- all inside
    // one holder's span. Before this packet each of them released to DiscEntity::kHostOwner and
    // republished, which the engine reads as a release followed by a grab.
    for (int i = 0; i < 20; ++i) {
        TransportJump(sc);
        sc.At((int64_t)(kT0 + (int64_t)(40 - i) * (int64_t)kDt), /*reversed=*/(i % 2) == 0);
    }
    CHECK(sc.ent.owner_writes.size() == 2);
    CHECK(sc.Owner() == kHeldId);
}

TEST_CASE("POSSESSION2: a seek that changes the holder is exactly one transition, old to new") {
    uint32_t owners[8];
    for (size_t i = 0; i < 8; ++i) owners[i] = i < 4 ? kBodyOwnerNone : kRecordedSlot;
    Scene sc(owners, 8);

    sc.At((int64_t)kT0);
    REQUIRE(sc.Owner() == DiscEntity::kSyntheticOwner);
    TransportJump(sc);
    sc.At((int64_t)(kT0 + 7 * kDt));

    REQUIRE(sc.ent.owner_writes.size() == 2);
    CHECK(sc.ent.owner_writes[0] == DiscEntity::kSyntheticOwner);
    CHECK(sc.ent.owner_writes[1] == kHeldId);   // no DiscEntity::kHostOwner in between
}

TEST_CASE("POSSESSION2: the level survives the window rebuild a long seek performs") {
    const uint32_t owners[] = {kRecordedSlot, kRecordedSlot, kRecordedSlot};
    Scene sc(owners, 3);
    sc.At((int64_t)kT0);
    REQUIRE(sc.ent.owner_writes.size() == 2);   // the claim, then the level (POLISH-2's order)

    // A rebuild releases ahead of the build and then throws every BodyTrack away. The fresh window
    // is the same recording at a different centre -- same disc, same handle, same holder.
    sc.applier.ReleaseSyntheticOwners(sc.w, StateApplier::OwnerRelease::KeepDiscLevel);
    Window fresh;
    Scene::Build(fresh, owners, 3);
    REQUIRE(sc.applier.DiscLevelHeld());
    CHECK(sc.applier.ReconcileDiscLevel(fresh) == 0);
    sc.applier.Apply(fresh, (int64_t)(kT0 + 2 * kDt), ApplyOptions());

    CHECK(sc.ent.owner_writes.size() == 2);   // the rebuild adds none: the level was seeded, not restamped
    CHECK(sc.Owner() == kHeldId);
}

TEST_CASE("POSSESSION2: a teardown still hands the engine's original owner back exactly") {
    const uint32_t owners[] = {kRecordedSlot, kRecordedSlot};
    Scene sc(owners, 2);
    sc.At((int64_t)kT0);
    REQUIRE(sc.Owner() == kHeldId);

    CHECK(sc.applier.ReleaseSyntheticOwners(sc.w, StateApplier::OwnerRelease::All) == 1);
    CHECK(sc.Owner() == DiscEntity::kHostOwner);
    CHECK_FALSE(sc.applier.DiscLevelHeld());
    CHECK(sc.ent.owner_writes.back() == DiscEntity::kHostOwner);
}

TEST_CASE("POSSESSION2: a teardown whose track the rebuild destroyed still releases the level") {
    const uint32_t owners[] = {kRecordedSlot, kRecordedSlot};
    Scene sc(owners, 2);
    sc.At((int64_t)kT0);
    REQUIRE(sc.Owner() == kHeldId);

    // The unload path runs after a build that kept the level but left no track naming it.
    sc.applier.ReleaseSyntheticOwners(sc.w, StateApplier::OwnerRelease::KeepDiscLevel);
    Window empty;
    empty.Finalize();
    CHECK(sc.applier.ReleaseSyntheticOwners(empty, StateApplier::OwnerRelease::All) == 1);
    CHECK(sc.Owner() == DiscEntity::kHostOwner);
}

TEST_CASE("POSSESSION2: a re-bind that lands a different disc releases the old one") {
    const uint32_t owners[] = {kRecordedSlot, kRecordedSlot};
    Scene sc(owners, 2);
    sc.At((int64_t)kT0);
    REQUIRE(sc.Owner() == kHeldId);

    sc.applier.ReleaseSyntheticOwners(sc.w, StateApplier::OwnerRelease::KeepDiscLevel);
    sc.Disc().handle = PhysHandle(kDiscHandle + 1);   // the rules chain named another body
    CHECK(sc.applier.ReconcileDiscLevel(sc.w) == 1);
    CHECK(sc.Owner() == DiscEntity::kHostOwner);
    CHECK_FALSE(sc.applier.DiscLevelHeld());
}

// ---- P-2/P-3: the engine's own holder answer, read back ----------------------------------------

TEST_CASE("POSSESSION2: the engine reads the stamped id as a HOLDER, on its remote arm") {
    const uint32_t owners[] = {kRecordedSlot};
    Scene sc(owners, 1);

    const ApplyStats st = sc.At((int64_t)kT0);
    REQUIRE(st.disc_holder.read);
    CHECK(st.disc_holder.held);
    CHECK(st.disc_holder.remote_arm);
    CHECK_FALSE(st.disc_holder.local_arm);
    CHECK(st.disc_holder.gen_ok);
    // The value production NEVER composes: the band the gates compare is slotTable[index], and an
    // id built out of the wrong namespace cannot reach it.
    CHECK(st.disc_holder.live_slot == kEngineLiveSlot);
    CHECK(st.disc_holder.owner == kHeldId);
}

TEST_CASE("POSSESSION2: a body the grab event never reached reads as nobody, and says which term") {
    const uint32_t owners[] = {kRecordedSlot};
    Scene sc(owners, 1);
    // The recording names a holder but its state word never carried 0x200 -- a grab the capture
    // missed, not a namespace fault. Written into the SAMPLE, because that is the only place the
    // node's word can come from.
    const_cast<BodyState&>(sc.w.BodyFrames(sc.w.BodyAt(0))[0]).bits11 = 0x000Fu;

    const ApplyStats st = sc.At((int64_t)kT0);
    REQUIRE(st.disc_holder.read);
    CHECK_FALSE(st.disc_holder.held);
    CHECK_FALSE(st.disc_holder.held_bit);
    CHECK(st.disc_holder.gen_ok);       // so it is NOT a namespace or generation fault
    CHECK(st.disc_owner_engine_slot == kEngineIndex);   // we stamped a real id; the engine disagrees
}

TEST_CASE("POSSESSION2: an id composed out of the wrong namespace reads as nobody") {
    const uint32_t owners[] = {kRecordedSlot};
    Scene sc(owners, 1);
    // What a slot-for-index confusion produces: the LIVE SLOT where the table INDEX belongs.
    sc.w.PlayerAt(0).user_id = kEngineLiveSlot;

    const ApplyStats st = sc.At((int64_t)kT0);
    // The roster row resolves to nothing at all, which is the honest outcome: no guess is stamped.
    CHECK(st.disc_owner_unmapped == 1);
    CHECK(sc.Owner() == DiscEntity::kSyntheticOwner);
    REQUIRE(st.disc_holder.read);
    CHECK_FALSE(st.disc_holder.held);
}

TEST_CASE("POSSESSION2: an unowned span reads as nobody with no arm and no mismatch") {
    const uint32_t owners[] = {kBodyOwnerNone};
    Scene sc(owners, 1);

    const ApplyStats st = sc.At((int64_t)kT0);
    REQUIRE(st.disc_holder.read);
    CHECK_FALSE(st.disc_holder.held);
    CHECK_FALSE(st.disc_holder.remote_arm);
    CHECK_FALSE(st.disc_holder.local_arm);
}

// ---------------------------------------------------------------------------------------------
// POLISH-2. The holder gate is a PAIR -- PhysCI+0x1C and the node's 0x200 -- and the recording
// moves both on one sample. Publishing the owner before installing the node left a window where
// the dword named the new holder against the PREVIOUS tick's state word, which is exactly what the
// launch measured: every disagreeing hand-over read node 003F/001F, every agreeing one 023F, and
// the split was whether the sample BEFORE the change already had bit 9 up.

TEST_CASE("POLISH2: a hand-over publishes the owner against the node that carries its held bit") {
    // The capture's shape at 886.46: unheld sample, then the sender changes and bit 9 rises with it.
    const uint32_t owners[] = {kBodyOwnerNone, kRecordedSlot, kRecordedSlot};
    Scene sc(owners, 3);

    sc.At((int64_t)kT0);
    const ApplyStats handover = sc.At((int64_t)(kT0 + kDt));

    CHECK(handover.disc_owner_stamps == 1);
    REQUIRE(handover.disc_holder.read);
    CHECK(handover.disc_holder.node_read);
    CHECK(handover.disc_holder.held_bit);
    CHECK(handover.disc_holder.held);            // the engine agrees ON the hand-over tick
    CHECK((handover.disc_holder.node_state & 0x200u) != 0);
}

TEST_CASE("POLISH2: the first tick after a bind reads the node rather than answering unread") {
    const uint32_t owners[] = {kRecordedSlot, kRecordedSlot};
    Scene sc(owners, 2);

    const ApplyStats first = sc.At((int64_t)kT0);
    REQUIRE(first.disc_holder.read);
    CHECK(first.disc_holder.node_read);          // the launch's `unread:0000` warm-up
    CHECK(first.disc_holder.held);
}

TEST_CASE("POLISH2: a release drops the held bit and the dword keeps naming the last holder") {
    const uint32_t owners[] = {kRecordedSlot, kBodyOwnerNone};
    Scene sc(owners, 2);

    sc.At((int64_t)kT0);
    REQUIRE(sc.Owner() == kHeldId);
    const ApplyStats release = sc.At((int64_t)(kT0 + kDt));

    CHECK(sc.Owner() == kHeldId);
    CHECK_FALSE(release.disc_holder.held);
    CHECK_FALSE(release.disc_holder.held_bit);
    CHECK(release.disc_holder.node_read);
}

// The recorded holder goes "nobody" only at a goal or a round start. Publishing the
// out-of-table value there made the host's UpdateTrackingStatsArena see a change to a team-0xFFFF
// holder and voice "Interception!" as the scored disc settled in the goal.
TEST_CASE("A goal's recorded 'nobody' keeps the scorer named, and the next grab is one change") {
    const uint32_t owners[] = {kRecordedSlot, kRecordedSlot, kBodyOwnerNone, kBodyOwnerNone,
                               kRecordedSlot};
    Scene sc(owners, 5);

    uint32_t stamps = 0, held_over = 0;
    size_t writes_before_goal = 0;
    for (size_t i = 0; i < 5; ++i) {
        INFO("tick ", i);
        if (i == 2) writes_before_goal = sc.ent.owner_writes.size();
        const ApplyStats st = sc.At((int64_t)(kT0 + i * kDt));
        stamps += st.disc_owner_stamps;
        held_over += st.disc_owner_held_over;
        CHECK(sc.Owner() == kHeldId);
        const bool nobody = owners[i] == kBodyOwnerNone;
        CHECK(sc.Disc().owner_level_held_over == nobody);
        // Read back on the ticks that change something: the goal tick must read as nobody.
        if (st.disc_holder.node_read) CHECK(st.disc_holder.held == !nobody);
        if (i == 2) CHECK(st.disc_holder.node_read);
    }
    CHECK(stamps == 1);
    CHECK(held_over == 1);
    CHECK(sc.ent.recorded_stamps == 1);
    for (size_t k = writes_before_goal; k < sc.ent.owner_writes.size(); ++k)
        CHECK(sc.ent.owner_writes[k] != DiscEntity::kSyntheticOwner);
}

TEST_CASE("A 'nobody' before anyone has held the disc still publishes the out-of-table value") {
    const uint32_t owners[] = {kBodyOwnerNone, kBodyOwnerNone, kRecordedSlot};
    Scene sc(owners, 3);

    const ApplyStats st = sc.At((int64_t)kT0);
    CHECK(st.disc_owner_held_over == 0);
    CHECK_FALSE(sc.Disc().owner_level_held_over);
    CHECK(sc.Owner() == DiscEntity::kSyntheticOwner);
}

TEST_CASE("POLISH2: an install the engine refuses publishes no holder at all") {
    const uint32_t owners[] = {kRecordedSlot};
    Scene sc(owners, 1);
    sc.ent.refuse_install = replay::InstallRefusal::CiIdentityMismatch;

    const ApplyStats st = sc.At((int64_t)kT0);
    CHECK(st.disc_owner_stamps == 0);
    CHECK(sc.Owner() != kHeldId);                // naming a holder on a body we cannot drive
    CHECK_FALSE(sc.Disc().owner_level_published);
}

// ★ DISC-FLIGHT. The engine does not leave the disc alone once a possession level is published:
// CR15NetTouchInteractCS::Update diffs the holder answer against its own latch, fires the grab or
// the release itself, and its handler transfers the body to this host -- owner dword back to the
// host's, history nodes freed. A claim made ONCE per track therefore held only until the first
// hand-over, after which the engine simulated the disc and the replay's install was skipped by the
// INV-OWN1 guard for the rest of the load: `drive=locallyControlled`, `read=badSlot`, and a disc
// parked at arena centre while the plugin's own `driven=` traced the whole recorded flight.
TEST_CASE("DISC-FLIGHT: the drive survives the engine taking the body back on every possession edge") {
    const uint32_t owners[] = {kRecordedSlot, kRecordedSlot, kBodyOwnerNone, kBodyOwnerNone,
                               kBodyOwnerNone, kRecordedSlot, kRecordedSlot};
    Scene sc(owners, 7);
    sc.ent.disc_handle = kDiscHandle;
    sc.ent.takes_back_on_possession_edge = true;

    uint32_t installs = 0;
    for (size_t i = 0; i < 7; ++i) {
        INFO("tick ", i);
        if (i > 0) sc.ent.RunEngineUpdate();   // the engine's own frame, between two of ours
        const ApplyStats st = sc.At((int64_t)(kT0 + i * kDt));
        CHECK(st.disc_drive == DiscDrive::Installed);
        CHECK(st.bodies_installed == 1);
        CHECK(st.bodies_locally_controlled == 0);
        CHECK(st.bodies_read_back == 1);
        // The pose in the engine's ring is THIS tick's, not the one before the take-back.
        CHECK(sc.ent.ring[kDiscHandle % DiscEntity::kBodies].s.com[0] == doctest::Approx((float)i));
        installs += st.bodies_installed;
    }
    CHECK(installs == 7);
    // The fixture has to have fought back, or the case proves nothing. One dword edge in this
    // sequence: the opening publish. The recorded "nobody" keeps the holder named and the catch is
    // the same holder, so neither is an edge any more.
    CHECK(sc.ent.take_backs == 1);
    // Possession is still the recording's. The reclaim re-asserts the level that IS published; it
    // never invents one, so the holder at the end is the player who caught it.
    CHECK(sc.Owner() == kHeldId);
}

// PREROLL-2. Before its first launch the offline host's arena script parks the frisbee at the
// arena origin every frame. Our install goes in at engine stage 0, the script's pass runs later in
// the frame, and neither write goes through the other's path -- so both land and the disc renders
// alternately at the recorded pose and at the origin. The fix is one writer at a time: while the
// host is still pre-launch the pose AND the owner dword are left to it (DESIGN-HOST-SYNC P3 -- a
// claim re-asserted under hold is what alternated the holder with the host's park value), and the
// recorded holder is published on the first tick after the hold ends.
TEST_CASE("PREROLL-2: a held disc is not installed, its owner is the host's, and both resume on the next tick") {
    const uint32_t owners[] = {kRecordedSlot, kRecordedSlot, kRecordedSlot};
    Scene sc(owners, 3);
    sc.ent.disc_handle = kDiscHandle;
    const uint32_t hostOwner = sc.Owner();
    REQUIRE(hostOwner != kHeldId);        // otherwise "untouched" and "claimed" read the same
    const size_t writesBefore = sc.ent.owner_writes.size();

    const ApplyStats first = sc.At((int64_t)kT0, false, true);
    CHECK(first.disc_drive == DiscDrive::HostHeld);
    CHECK(first.bodies_installed == 0);
    CHECK(first.disc_sampled);            // sampled, and deliberately not written
    CHECK(first.disc_owner_stamps == 0);
    CHECK(sc.Owner() == hostOwner);       // ...and possession is not published under the hold

    // A second held tick writes no more than the first: the host keeps the disc, undisturbed.
    const ApplyStats second = sc.At((int64_t)(kT0 + kDt), false, true);
    CHECK(second.disc_drive == DiscDrive::HostHeld);
    CHECK(second.bodies_installed == 0);
    CHECK(sc.Owner() == hostOwner);
    CHECK(sc.ent.owner_writes.size() == writesBefore);

    // The host launches. The very next tick drives the recorded pose, at THIS tick's time, and
    // publishes the recorded holder.
    const ApplyStats after = sc.At((int64_t)(kT0 + 2 * kDt));
    CHECK(after.disc_drive == DiscDrive::Installed);
    CHECK(after.bodies_installed == 1);
    CHECK(sc.ent.ring[kDiscHandle % DiscEntity::kBodies].s.com[0] == doctest::Approx(2.0f));
    CHECK(sc.Owner() == kHeldId);
}

// ---------------------------------------------------------------------------------------------
// PRESENT-A. The two fields beside the owner dword that make the disc read as TOUCHED: PhysCI+6
// bit1 (CR15FrisbeeCS::IsHeld) and PhysCI+0x20 (LastOwner). Both are pure functions of the
// playhead, which is what makes the owner's "seek back before the first touch" case free.

namespace {
constexpr size_t kDiscBody = kDiscHandle % DiscEntity::kBodies;

struct TouchGuard {
    TouchGuard() { replay::livesafety::SetForTest(false, true); }
    ~TouchGuard() { replay::livesafety::ResetForTest(); }
};
}  // namespace

TEST_CASE("the disc's held bit and last owner follow the recorded holder as levels") {
    TouchGuard guard;
    const uint32_t owners[] = {kBodyOwnerNone, kRecordedSlot, kRecordedSlot, kBodyOwnerNone};
    Scene sc(owners, 4);
    // A bit the engine owns, to prove the write preserves the other fifteen.
    sc.ent.ci_flags6[kDiscBody] = 0x8001u;

    sc.At((int64_t)kT0);
    CHECK_FALSE(sc.ent.IsHeldAt(kDiscBody));
    CHECK(sc.ent.ci_prev_owner[kDiscBody] == sc.ent.SyntheticOwnerValue());   // never touched

    sc.At((int64_t)(kT0 + kDt));
    CHECK(sc.ent.IsHeldAt(kDiscBody));
    CHECK(sc.ent.ci_prev_owner[kDiscBody] == kHeldId);
    CHECK((sc.ent.ci_flags6[kDiscBody] & 0x8001u) == 0x8001u);

    // Released. IsHeld goes false and LastOwner STAYS: that is what "has been touched" means.
    sc.At((int64_t)(kT0 + 3 * kDt));
    CHECK_FALSE(sc.ent.IsHeldAt(kDiscBody));
    CHECK(sc.ent.ci_prev_owner[kDiscBody] == kHeldId);

    SUBCASE("a seek back before the first touch shows the ring again") {
        sc.At((int64_t)kT0);
        CHECK_FALSE(sc.ent.IsHeldAt(kDiscBody));
        CHECK(sc.ent.ci_prev_owner[kDiscBody] == sc.ent.SyntheticOwnerValue());
    }

    SUBCASE("a reverse step through the release is the same two levels") {
        sc.At((int64_t)(kT0 + 2 * kDt), /*reversed=*/true);
        CHECK(sc.ent.IsHeldAt(kDiscBody));
        CHECK(sc.ent.ci_prev_owner[kDiscBody] == kHeldId);
    }
}

TEST_CASE("a steady possession writes the touch pair once, not once per tick") {
    TouchGuard guard;
    const uint32_t owners[] = {kRecordedSlot, kRecordedSlot, kRecordedSlot};
    Scene sc(owners, 3);
    sc.At((int64_t)kT0);
    const size_t after_first = sc.ent.touch_writes;
    sc.At((int64_t)(kT0 + kDt));
    sc.At((int64_t)(kT0 + 2 * kDt));
    CHECK(sc.ent.touch_writes == after_first);
}

TEST_CASE("a refused touch write is counted and claims nothing") {
    TouchGuard guard;
    const uint32_t owners[] = {kRecordedSlot};
    Scene sc(owners, 1);
    sc.ent.touch_refuses = true;
    const ApplyStats st = sc.At((int64_t)kT0);
    CHECK(st.disc_touch_refused == 1u);
    CHECK_FALSE(sc.ent.IsHeldAt(kDiscBody));

    sc.ent.touch_refuses = false;
    sc.At((int64_t)kT0);
    CHECK(sc.ent.IsHeldAt(kDiscBody));   // the retry is not suppressed by the failed attempt
}

TEST_CASE("nothing is written when the session is not contained") {
    const uint32_t owners[] = {kRecordedSlot};
    Scene sc(owners, 1);
    replay::livesafety::SetForTest(true, false);   // a LIVE online session
    sc.At((int64_t)kT0);
    replay::livesafety::ResetForTest();
    CHECK(sc.ent.touch_writes == 0u);
    CHECK_FALSE(sc.ent.IsHeldAt(kDiscBody));
}

// ---- DESIGN-HOST-SYNC P3: no holder writes while the host parks the disc -----------------------

TEST_CASE("HOST-SYNC P3: under hold_disc_pose the owner dword is the host's alone, and release re-asserts") {
    const uint32_t owners[] = {kRecordedSlot, kRecordedSlot, kRecordedSlot};
    Scene sc(owners, 3);
    uint32_t& dword = sc.ent.owner[kDiscHandle % DiscEntity::kBodies];
    const size_t writesBefore = sc.ent.owner_writes.size();

    for (int tick = 0; tick < 8; ++tick) {
        dword = DiscEntity::kHostOwner;   // the host re-parks the disc every update
        const ApplyStats st = sc.At((int64_t)kT0, false, true);
        CHECK(st.disc_drive == DiscDrive::HostHeld);
        CHECK(st.disc_owner_stamps == 0);
        CHECK(dword == DiscEntity::kHostOwner);
    }
    CHECK(sc.ent.owner_writes.size() == writesBefore);

    const ApplyStats released = sc.At((int64_t)kT0, false, false);
    CHECK(released.disc_drive != DiscDrive::HostHeld);
    CHECK(sc.Owner() == kHeldId);
}
