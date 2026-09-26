// DISC-BIND: the recorded tag-1 disc track is bound through the engine's own statement of which
// physics component is a disc, claimed, driven from the recording, and handed back.

#include "doctest.h"

#include <stdint.h>
#include <string.h>

#include "capture/streamcodec.h"
#include "fake_disc_entity.h"
#include "playback/core/apply.h"
#include "playback/core/window.h"
#include "playback/core/windowbuild.h"

using namespace replay;
using namespace replay::playback;
using disctest::DiscEntity;

namespace {

constexpr uint64_t kT0 = 1'000'000;
constexpr uint64_t kDt = 22'222;
constexpr size_t   kSamples = 20;
constexpr uint32_t kRecordedDisc = 0x00000000u;   // SComponentID{index 0, generation 0}
constexpr uint16_t kDiscHandle = 5;
constexpr bool     kDriveDisc = true;

BodyIdentity TaggedId(uint64_t value, uint32_t tag) {
    BodyIdentity id;
    id.value = value;
    id.tag = tag;
    id.fill = capture::kFillBodyIdValue | capture::kFillBodyIdTag;
    return id;
}

// x is the track's signature and moves with time, so a ring holding another track's pose, or a
// stale one, reads differently.
void FillTrack(Window& w, const BodyIdentity& id, float base) {
    for (size_t i = 0; i < kSamples; ++i) {
        BodyState s;
        memset(&s, 0, sizeof s);
        s.com[0] = base + (float)i;
        s.ori[3] = 1.0f;
        REQUIRE(w.AddBodySample(id, kT0 + i * kDt, s));
    }
}

// The disc and the bodies of recorded users 1 and 2, provisioned into live slots 3 and 4.
void* const kNetGame = reinterpret_cast<void*>(0xBEEF);

struct Scene {
    Window w;
    DiscEntity ent;
    uint16_t slot_map[PawnResolver::kSlots];
    Scene() {
        FillTrack(w, TaggedId(kRecordedDisc, 1), 100.0f);
        FillTrack(w, TaggedId(1, 0), 200.0f);
        FillTrack(w, TaggedId(2, 0), 300.0f);
        for (uint16_t& s : slot_map) s = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        slot_map[1] = 3;
        slot_map[2] = 4;
        // Replayed entrants are remote on this host already; only the disc is the host's own.
        ent.owner[DiscEntity::kPlayerBodyBase + 3] = 0x00010020u;
        ent.owner[DiscEntity::kPlayerBodyBase + 4] = 0x00010021u;
    }
    BindResult Bind(bool drive_disc = kDriveDisc) {
        if (!w.Finalized()) w.Finalize();
        return BindBodyHandles(ent, w, slot_map, PawnResolver::kSlots, drive_disc, kNetGame);
    }
    BodyTrack& Disc() {
        for (size_t i = 0; i < w.BodyCount(); ++i) {
            if (w.BodyAt(i).id.tag == 1) return w.BodyAt(i);
        }
        FAIL("no disc track");
        return w.BodyAt(0);
    }
};

}  // namespace

TEST_CASE("DISC-BIND: with the drive off the tag-1 track stays unbound, exactly as before") {
    Scene sc;
    sc.ent.PlaceDisc(kRecordedDisc, kDiscHandle);
    const BindResult r = sc.Bind(/*drive_disc=*/false);
    CHECK(r.bound == 2);
    CHECK(r.not_a_net_id == 1);
    CHECK(r.bound_disc == 0);
    CHECK(r.disc_verdict == BindResult::DiscVerdict::NotAsked);
    char line[64] = "x";
    CHECK_FALSE(FormatDiscBind(r, line, sizeof line));
    CHECK_FALSE(sc.Disc().handle_valid);
    CHECK_FALSE(sc.Disc().claim_owner);
}

TEST_CASE("DISC-BIND: a recorded id the live frisbee names binds, and players keep their own bodies") {
    Scene sc;
    sc.ent.PlaceDisc(kRecordedDisc, kDiscHandle);
    const BindResult r = sc.Bind();
    CHECK(r.bound == 3);
    CHECK(r.bound_by_slot == 2);
    CHECK(r.bound_disc == 1);
    CHECK(r.disc_refused == 0);
    CHECK(r.not_a_net_id == 0);
    CHECK(r.disc_verdict == BindResult::DiscVerdict::Agreed);
    CHECK(r.disc_bound_id == kRecordedDisc);

    for (size_t i = 0; i < sc.w.BodyCount(); ++i) {
        const BodyTrack& t = sc.w.BodyAt(i);
        REQUIRE(t.handle_valid);
        if (t.id.tag == 1) {
            CHECK(t.handle.v == kDiscHandle);
            CHECK(t.claim_owner);
            CHECK_FALSE(t.bound_by_slot);
        } else {
            CHECK(t.handle.v == DiscEntity::kPlayerBodyBase + sc.slot_map[t.id.value]);
            CHECK_FALSE(t.claim_owner);
            CHECK(t.bound_by_slot);
        }
    }

    char line[512] = {};
    REQUIRE(FormatDiscBind(r, line, sizeof line));
    INFO(line);
    CHECK(strstr(line, "agreed: recorded cid=0x00000000 live cid=0x00000000 (1 live) bound "
                       "cid=0x00000000") != nullptr);
    CHECK(strstr(line, "ci+4=0x00000800, physCs=") != nullptr);
    CHECK(strstr(line, "shortcut=agreed, 1 bound 0 refused") != nullptr);
    // With the bind line's longest prefix ("disc bind ") it still fits one log line untruncated.
    CHECK(strlen(line) + 64 < 512);
}

TEST_CASE("DISC-BIND: a recorded id the live chain does not name is re-aimed at the live one") {
    Scene sc;
    const uint32_t live = 0x00030007u;   // another index AND another generation
    sc.ent.PlaceDisc(live, 9);
    // The recorded id still resolves to SOME physics component on this host: not the disc.
    sc.ent.components[kRecordedDisc] = 2;

    const BindResult r = sc.Bind();
    CHECK(r.disc_verdict == BindResult::DiscVerdict::ReaimedToLive);
    CHECK(r.disc_recorded_id == kRecordedDisc);
    CHECK(r.disc_bound_id == live);
    CHECK(r.bound_disc == 1);
    REQUIRE(sc.Disc().handle_valid);
    CHECK(sc.Disc().handle.v == 9);

    char line[512] = {};
    REQUIRE(FormatDiscBind(r, line, sizeof line));
    INFO(line);
    CHECK(strstr(line, "reaimed-to-live: recorded cid=0x00000000 live cid=0x00030007") != nullptr);
    CHECK(strstr(line, "bound cid=0x00030007") != nullptr);
}

TEST_CASE("DISC-BIND: an unreadable chain means no drive, and the line says why") {
    Scene sc;
    sc.ent.components[kRecordedDisc] = kDiscHandle;   // the id would resolve; nothing vouches for it
    const BindResult r = sc.Bind();
    CHECK(r.disc_verdict == BindResult::DiscVerdict::OracleUnreadable);
    CHECK(r.bound_disc == 0);
    CHECK(r.disc_refused == 1);
    CHECK(r.bound == 2);
    CHECK_FALSE(sc.Disc().handle_valid);

    char line[512] = {};
    REQUIRE(FormatDiscBind(r, line, sizeof line));
    CHECK(strstr(line, "oracle-unreadable: recorded") != nullptr);
    CHECK(strstr(line, "(0 live: no frisbee in this fixture)") != nullptr);
}

TEST_CASE("DISC-BIND: a disagreement with no single answer is refused") {
    SUBCASE("two component tracks, one live disc, neither id its own") {
        Scene sc;
        FillTrack(sc.w, TaggedId(0x00000001u, 1), 400.0f);
        sc.ent.PlaceDisc(0x00000004u, 9);
        const BindResult r = sc.Bind();
        CHECK(r.disc_verdict == BindResult::DiscVerdict::Ambiguous);
        CHECK(r.disc_refused == 2);
        CHECK(r.bound_disc == 0);
    }
    SUBCASE("two component tracks and the live id is one of them: that one binds, the other does not") {
        Scene sc;
        FillTrack(sc.w, TaggedId(0x00000004u, 1), 400.0f);
        sc.ent.PlaceDisc(kRecordedDisc, kDiscHandle);
        const BindResult r = sc.Bind();
        CHECK(r.bound_disc == 1);
        CHECK(r.disc_refused == 1);
        // The line names the LAST track it looked at, which is the refused one.
        CHECK(r.disc_verdict == BindResult::DiscVerdict::Ambiguous);
        CHECK(sc.Disc().handle.v == kDiscHandle);
        CHECK(sc.Disc().claim_owner);
    }
}

TEST_CASE("DISC-BIND: the disc is bound in its OWN pool, never the players") {
    Scene sc;
    sc.ent.PlaceDisc(kRecordedDisc, kDiscHandle);
    const BindResult r = sc.Bind();
    REQUIRE(r.bound_disc == 1);
    CHECK(sc.Disc().handle.pool == PhysHandle::kPoolDisc);
    CHECK(r.disc_phys_cs != r.disc_player_phys_cs);
    for (size_t i = 0; i < sc.w.BodyCount(); ++i) {
        const BodyTrack& t = sc.w.BodyAt(i);
        if (t.id.tag != 1) CHECK(t.handle.pool == PhysHandle::kPoolPlayers);
    }
    char line[512] = {};
    REQUIRE(FormatDiscBind(r, line, sizeof line));
    INFO(line);
    CHECK(strstr(line, "separate-pool") != nullptr);
}

TEST_CASE("DISC-BIND: a handle a player drives is in use only inside the same pool") {
    Scene sc;
    // The players pool hands out kPlayerBodyBase + 3; the disc pool numbers its own elements, so
    // the same number there is a different body and must not read as in use.
    sc.ent.PlaceDisc(kRecordedDisc, DiscEntity::kPlayerBodyBase + 3);
    const BindResult r = sc.Bind();
    CHECK(r.disc_verdict == BindResult::DiscVerdict::Agreed);
    CHECK(r.bound_disc == 1);
    CHECK(sc.Disc().handle.pool == PhysHandle::kPoolDisc);
}

TEST_CASE("DISC-BIND: one shared pool still refuses a handle a player drives") {
    Scene sc;
    sc.ent.disc_phys_cs = sc.ent.phys_cs;   // the frisbee resolved the SAME CR15NetPhysicsCS
    sc.ent.PlaceDisc(kRecordedDisc, DiscEntity::kPlayerBodyBase + 3);
    const BindResult r = sc.Bind();
    CHECK(r.disc_verdict == BindResult::DiscVerdict::HandleInUse);
    CHECK(r.bound == 2);
    CHECK_FALSE(sc.Disc().handle_valid);
    CHECK_FALSE(sc.Disc().claim_owner);
    char line[512] = {};
    REQUIRE(FormatDiscBind(r, line, sizeof line));
    CHECK(strstr(line, "SAME-POOL") != nullptr);
}

TEST_CASE("DISC-BIND: the bind is retried, and binds once the level space answers") {
    Scene sc;
    BindResult r = sc.Bind();
    REQUIRE(r.disc_verdict == BindResult::DiscVerdict::OracleUnreadable);
    REQUIRE(r.bound_disc == 0);
    const size_t players_bound = r.bound;

    // The level game space fills after map load; the retry asks again without disturbing the
    // player binds this window already holds.
    sc.ent.PlaceDisc(kRecordedDisc, kDiscHandle);
    BindDiscTracks(sc.ent, kNetGame, sc.w, r);
    CHECK(r.disc_verdict == BindResult::DiscVerdict::Agreed);
    CHECK(r.bound_disc == 1);
    CHECK(r.disc_refused == 0);
    CHECK(r.bound == players_bound);   // BindDiscTracks leaves `bound` to its caller
    REQUIRE(sc.Disc().handle_valid);
    CHECK(sc.Disc().claim_owner);
    CHECK(sc.Disc().disc_component_id == kRecordedDisc);
    for (size_t i = 0; i < sc.w.BodyCount(); ++i) {
        if (sc.w.BodyAt(i).id.tag != 1) CHECK(sc.w.BodyAt(i).handle_valid);
    }
}

TEST_CASE("DISC-BIND: a re-bind that refuses hands the claimed disc back before dropping it") {
    Scene sc;
    sc.ent.PlaceDisc(kRecordedDisc, kDiscHandle);
    sc.Bind();
    StateApplier ap;
    ap.Bind(&sc.ent, nullptr, nullptr, nullptr);
    ap.Apply(sc.w, (int64_t)(kT0 + 4 * kDt), ApplyOptions());
    REQUIRE(sc.ent.owner[kDiscHandle] == DiscEntity::kSyntheticOwner);

    // What every rebind site does now: release, then re-bind. The re-bind refuses because the
    // chain went stale, and the disc must already be back on the host own simulation.
    CHECK(ap.ReleaseSyntheticOwners(sc.w) == 1);
    sc.ent.chain_ok = false;
    sc.ent.chain_refused = "the frisbee ref is stale";
    const BindResult r = sc.Bind();
    CHECK(r.bound_disc == 0);
    CHECK(sc.ent.owner[kDiscHandle] == DiscEntity::kHostOwner);
    CHECK_FALSE(sc.Disc().handle_valid);
    CHECK_FALSE(sc.Disc().synthetic_owner_set);
}

TEST_CASE("DISC-BIND: a release whose handle was already dropped still restores the owner") {
    Scene sc;
    sc.ent.PlaceDisc(kRecordedDisc, kDiscHandle);
    sc.Bind();
    StateApplier ap;
    ap.Bind(&sc.ent, nullptr, nullptr, nullptr);
    ap.Apply(sc.w, (int64_t)(kT0 + 4 * kDt), ApplyOptions());
    REQUIRE(sc.ent.owner[kDiscHandle] == DiscEntity::kSyntheticOwner);

    // The shape that froze a claimed disc in mid-air: the handle flag cleared by a rebind while
    // the claim was still stamped on the engine object.
    sc.Disc().handle_valid = false;
    CHECK(ap.ReleaseSyntheticOwners(sc.w) == 1);
    CHECK(sc.ent.owner[kDiscHandle] == DiscEntity::kHostOwner);
}

TEST_CASE("DISC-BIND: an id the physics pool refuses, or a body a player drives, is not bound") {
    SUBCASE("generation or bound refused by PhysOfComponentId") {
        Scene sc;
        sc.ent.PlaceDisc(kRecordedDisc, kDiscHandle);
        sc.ent.components.clear();
        const BindResult r = sc.Bind();
        CHECK(r.disc_verdict == BindResult::DiscVerdict::NoSuchComponent);
        CHECK_FALSE(sc.Disc().handle_valid);
    }
}

TEST_CASE("DISC-BIND: the bound disc is claimed, driven from its own track, and handed back") {
    Scene sc;
    sc.ent.PlaceDisc(kRecordedDisc, kDiscHandle);
    sc.Bind();
    StateApplier ap;
    ap.Bind(&sc.ent, nullptr, nullptr, nullptr);
    REQUIRE(sc.ent.IsLocallyControlled(PhysHandle(kDiscHandle)));   // the host simulates it

    const ApplyOptions opt;
    ApplyStats st = ap.Apply(sc.w, (int64_t)(kT0 + 4 * kDt), opt);
    // POLISH-2 put the level publish AFTER the install, so the disc is claimed by the ordinary
    // claim block again -- with the out-of-table value, which reads as nobody. The level then
    // names the holder against a node that already carries his held bit. This fixture carries no
    // player tracks, so the recorded holder maps to nobody and the publish changes nothing.
    CHECK(st.bodies_owner_claimed == 1);
    CHECK(st.disc_owner_stamps == 1);
    CHECK(st.disc_owner_unmapped == 1);
    CHECK(st.bodies_locally_controlled == 0);
    CHECK(st.bodies_installed == 3);
    CHECK(st.bodies_read_back == 3);
    CHECK(st.disc_install_refused.why == InstallRefusal::None);
    CHECK(sc.ent.owner[kDiscHandle] == DiscEntity::kSyntheticOwner);
    CHECK(sc.ent.ring[kDiscHandle].s.com[0] == 104.0f);
    CHECK(sc.ent.ring[kDiscHandle].t == kT0 + 4 * kDt);
    CHECK(sc.ent.ring[DiscEntity::kPlayerBodyBase + 3].s.com[0] == 204.0f);

    st = ap.Apply(sc.w, (int64_t)(kT0 + 11 * kDt), opt);
    CHECK(st.bodies_owner_claimed == 0);             // once per claim, not once per frame
    CHECK(sc.ent.ring[kDiscHandle].s.com[0] == 111.0f);
    CHECK(sc.ent.ring[kDiscHandle].installs == 2);

    // Unload and session end both come through here.
    CHECK(ap.ReleaseSyntheticOwners(sc.w) == 1);
    CHECK(sc.ent.owner[kDiscHandle] == DiscEntity::kHostOwner);
    CHECK(sc.ent.IsLocallyControlled(PhysHandle(kDiscHandle)));
    CHECK(ap.ReleaseSyntheticOwners(sc.w) == 0);
}

TEST_CASE("DISC-BIND: a refused disc install is named, and an unclaimed disc is never installed over") {
    Scene sc;
    sc.ent.PlaceDisc(kRecordedDisc, kDiscHandle);
    sc.Bind();
    StateApplier ap;
    ap.Bind(&sc.ent, nullptr, nullptr, nullptr);
    const ApplyOptions opt;

    SUBCASE("no history container") {
        sc.ent.refuse_install = InstallRefusal::NoHistoryList;
        const ApplyStats st = ap.Apply(sc.w, (int64_t)(kT0 + 4 * kDt), opt);
        CHECK(st.disc_install_refused.why == InstallRefusal::NoHistoryList);
        CHECK(sc.ent.ring[kDiscHandle].installs == 0);
    }
    SUBCASE("the track lost its claim flag: the host still simulates it, so nothing is installed") {
        sc.Disc().claim_owner = false;
        const ApplyStats st = ap.Apply(sc.w, (int64_t)(kT0 + 4 * kDt), opt);
        CHECK(st.bodies_locally_controlled == 1);
        CHECK(st.bodies_owner_claimed == 0);
        CHECK(sc.ent.owner[kDiscHandle] == DiscEntity::kHostOwner);
        CHECK(sc.ent.ring[kDiscHandle].installs == 0);
    }
}
