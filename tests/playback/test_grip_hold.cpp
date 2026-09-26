// GRIP: the per-hand grab target, and the instrument that reports it.
//
// Two things are pinned here. First RemapHoldTarget, which ENG-PLAYBACK-POLISH shipped with no
// apply-level test at all: a recorded hold id names a player in the RECORDING's slot numbering and
// publishing it verbatim points a live hand at whoever holds that live slot. Second the log line,
// which until DIAG-DISC-FROZEN-REGRESSION §3.3b was throttled by ONE function-level `static` shared
// by every track and both hands -- about one line per second for the whole session, so a grab
// shorter than a second could not be sampled, and a launch read "823 of 823 sentinel" as "nobody
// ever holds the disc".

#include "doctest.h"

#include <windows.h>

#include <stdint.h>
#include <string.h>

#include <fstream>
#include <sstream>
#include <string>

#include "bootstrap/log.h"
#include "capture/streamcodec.h"
#include "fake_disc_entity.h"
#include "playback/core/apply.h"
#include "playback/core/window.h"

using namespace replay;
using namespace replay::playback;
using disctest::DiscEntity;

namespace griptest {
namespace {

constexpr uint64_t kT0 = 1'000'000;
constexpr uint64_t kDt = 22'222;   // ~45 Hz

// Two entrants, recorded slots 0 and 1, admitted at DIFFERENT engine indices with DIFFERENT
// generations: a fixture whose recorded slot is also its engine index cannot fail when the two are
// confused, which is the whole subject of RemapHoldTarget.
constexpr uint16_t kRecA = 0, kRecB = 1;
constexpr uint16_t kIndexA = 4, kIndexB = 6;
constexpr uint16_t kGenA = 7, kGenB = 9;

uint32_t LiveIdOf(uint16_t index, uint16_t gen) {
    return (uint32_t)index | ((uint32_t)gen << 16);
}

struct Pose : IPosePublisher {
    PlayerState last[2];
    size_t published = 0;
    void Publish(PlayerHandle h, const PlayerState& render, const PlayerState&) override {
        if (h.v < 2) last[h.v] = render;
        ++published;
    }
    const char* Strategy() const override { return "PP-B"; }
    bool NeedsLateTick() const override { return false; }
};

// Everything log::Direct wrote while this lives.
struct LogCapture {
    std::wstring path;
    LogCapture() {
        wchar_t dir[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, dir);
        path = std::wstring(dir) + L"griphold_selftest_" + std::to_wstring(GetCurrentProcessId()) +
               L".log";
        DeleteFileW(path.c_str());
        log::RedirectForTest(path.c_str());
        log::SetLevel(LogLevel::Info);
    }
    ~LogCapture() {
        log::EndRedirectForTest();
        DeleteFileW(path.c_str());
    }
    std::string Text() const {
        std::ifstream in(path, std::ios::binary);
        std::stringstream buf;
        buf << in.rdbuf();
        return buf.str();
    }
    static size_t Count(const std::string& text, const std::string& needle) {
        size_t n = 0;
        for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
            ++n;
        return n;
    }
};

// `holds[i]` is the recorded holdL of track A's sample i; track B never grabs anything.
struct Scene {
    Window w;
    DiscEntity ent;
    Pose pose;
    StateApplier applier;

    Scene(const uint32_t* holds, size_t n) {
        PlayerState ps;
        memset(&ps, 0, sizeof ps);
        ps.holdR = kHoldNobody;
        for (size_t i = 0; i < n; ++i) {
            ps.holdL = holds[i];
            ps.trigL = holds[i] == kHoldNobody ? 0.0f : 0.9f;
            REQUIRE(w.AddPlayerSample(kRecA, kT0 + i * kDt, ps));
        }
        ps.holdL = kHoldNobody;
        ps.trigL = 0.0f;
        for (size_t i = 0; i < n; ++i) REQUIRE(w.AddPlayerSample(kRecB, kT0 + i * kDt, ps));
        w.Finalize();

        for (size_t i = 0; i < w.PlayerCount(); ++i) {
            PlayerTrack& tr = w.PlayerAt(i);
            tr.handle = PlayerHandle((uint16_t)i);
            tr.handle_valid = true;
            tr.user_id = tr.user_slot == kRecA ? kIndexA : kIndexB;
            tr.user_id_valid = true;
        }
        ent.generation[kIndexA] = kGenA;
        ent.generation[kIndexB] = kGenB;
        applier.Bind(&ent, &pose, nullptr, nullptr);
    }

    ApplyStats At(size_t sample) {
        ApplyOptions opt;
        opt.grip_inputs = true;
        return applier.Apply(w, (int64_t)(kT0 + sample * kDt), opt);
    }
    // The pose the track with recorded slot `rec` was published with.
    const PlayerState& Published(uint16_t rec) {
        for (size_t i = 0; i < w.PlayerCount(); ++i)
            if (w.PlayerAt(i).user_slot == rec) return pose.last[w.PlayerAt(i).handle.v];
        FAIL("no track for that recorded slot");
        return pose.last[0];
    }
};

}  // namespace
}  // namespace griptest

using griptest::LiveIdOf;
using griptest::LogCapture;
using griptest::Scene;

TEST_CASE("grip: a recorded hold target is republished in THIS session's numbering") {
    // Track A's left hand holds recorded slot 1, which this session admitted at index 6 gen 9.
    const uint32_t holds[] = {kHoldNobody, griptest::kRecB, griptest::kRecB};
    Scene sc(holds, 3);

    sc.At(1);
    const uint32_t live = LiveIdOf(griptest::kIndexB, griptest::kGenB);
    CHECK(sc.Published(griptest::kRecA).holdL == live);
    CHECK(sc.Published(griptest::kRecA).holdL != griptest::kRecB);   // not the recorded number
    CHECK(sc.Published(griptest::kRecA).holdR == kHoldNobody);
    // The trigger blend is what the engine's own grab machine runs on, and it was published as a
    // constant 0 in every build before PLAYBACK-POLISH.
    CHECK(sc.Published(griptest::kRecA).trigL == doctest::Approx(0.9f));
}

TEST_CASE("grip: a slot this window never resolved becomes the sentinel, never a guess") {
    const uint32_t unadmitted = 12;   // no track carries recorded slot 12
    const uint32_t holds[] = {kHoldNobody, unadmitted};
    Scene sc(holds, 2);

    sc.At(1);
    CHECK(sc.Published(griptest::kRecA).holdL == kHoldNobody);
}

TEST_CASE("grip: a track whose entrant the engine has no generation for becomes the sentinel") {
    const uint32_t holds[] = {kHoldNobody, griptest::kRecB};
    Scene sc(holds, 2);
    sc.ent.generation[griptest::kIndexB] = 0xFFFFu;   // admitted here, not live over there  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids

    sc.At(1);
    CHECK(sc.Published(griptest::kRecA).holdL == kHoldNobody);
}

TEST_CASE("grip: the engine's own invalid id passes through byte-identical") {
    const uint32_t holds[] = {kHoldNobody, kHoldNobody};
    Scene sc(holds, 2);

    sc.At(1);
    // sub_140CCE580 requires 0xFFFFFFFF on both sides of a frame to raise the geo-attach
    // delegates at all, so a remap that "cleaned it up" would silence every hand attach.
    CHECK(sc.Published(griptest::kRecA).holdL == 0xFFFFFFFFu);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    CHECK(sc.Published(griptest::kRecA).holdR == 0xFFFFFFFFu);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
}

// The goal feed spells "unmapped" 0xFFFF0010 because 0xFFFFFFFF means "reuse the
// previous scorer" to that handler. The two conventions live one file apart and must not be ported
// into each other: here 0xFFFFFFFF is the pass-through and 0xFFFF0010 is an ordinary value whose
// low word is a slot number -- slot 16, which no window resolves.
TEST_CASE("grip: the goal feed's unmapped word is NOT this feed's sentinel") {
    constexpr uint32_t kGoalUnmapped = 0xFFFF0010u;
    CHECK(kGoalUnmapped != kHoldNobody);
    const uint32_t holds[] = {kHoldNobody, kGoalUnmapped};
    Scene sc(holds, 2);

    sc.At(1);
    CHECK(sc.Published(griptest::kRecA).holdL == kHoldNobody);
}

// The instrument. A grab and a release inside the same second must both print; the tracks that did
// not change must not print again in that second.
TEST_CASE("grip: a sub-second grab prints both edges, and only the track that moved") {
    const uint32_t holds[] = {kHoldNobody, griptest::kRecB, kHoldNobody};
    Scene sc(holds, 3);
    LogCapture captured;

    sc.At(0);   // first sight of each track: one line each
    sc.At(1);   // A grabs
    sc.At(2);   // A releases, all inside one millisecond of wall time

    const std::string text = captured.Text();
    const size_t a = LogCapture::Count(text, "grip: rec slot 0 ");
    const size_t b = LogCapture::Count(text, "grip: rec slot 1 ");
    // Three lines for the hand that moved twice, one for the hand that did not: with the old
    // shared `static` the whole scene printed one line.
    CHECK(a == 3);
    CHECK(b == 1);
    CHECK(LogCapture::Count(text, "EDGE") == 4);
    // recorded slot 1 -> this session's {index 6, generation 9}, composed low word first.
    CHECK(text.find("grabTargetL 00000001->00090006") != std::string::npos);
    // The remap runs before the line, so the release prints the RECORDED sentinel on both sides.
    CHECK(LogCapture::Count(text, "grabTargetL none->none") == 3);
    // The field name a launch agent reads. `holdL` was read as disc possession for a whole launch.
    CHECK(text.find("holdL") == std::string::npos);
    CHECK(text.find("not disc possession") != std::string::npos);
}

TEST_CASE("grip: an unchanged track prints at most once a second") {
    const uint32_t holds[] = {kHoldNobody, kHoldNobody, kHoldNobody, kHoldNobody};
    Scene sc(holds, 4);
    LogCapture captured;

    for (size_t i = 0; i < 4; ++i) sc.At(i);

    const std::string text = captured.Text();
    CHECK(LogCapture::Count(text, "grip: rec slot 0 ") == 1);
    CHECK(LogCapture::Count(text, "grip: rec slot 1 ") == 1);
}
