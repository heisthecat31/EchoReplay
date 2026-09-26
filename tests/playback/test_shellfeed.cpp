// DESIGN-HOST-SYNC P4: the disc shell's level function and its reconciler.

#include "doctest.h"

#include <windows.h>

#include <math.h>
#include <string.h>

#include <vector>

#include "foundation/demofile/demofile.h"
#include "playback/core/windowbuild.h"

#include "engine/scriptgraph_facts.h"
#include "playback/core/shellfeed.h"
#include "playback/core/shellplan.h"
#include "../local_inputs.h"

using namespace replay;
using replay::playback::ScanTrackSources;
using replay::playback::TrackScan;
using shellplan::Colour;

namespace {

constexpr int64_t S(double seconds) { return int64_t(seconds * 1e6); }

// 2026-09-24_12-59-23 as its own marker table decodes (echodemo info, kMarkPhase payloads): the
// goals, and every pre_launch edge plus the non-pre_launch edges around them.
struct Oracle {
    shellplan::GoalIn goals[7] = {{S(15.139), 0},  {S(72.432), 0},  {S(115.389), 0},
                                  {S(198.725), 1}, {S(272.221), 0}, {S(333.073), 0},
                                  {S(432.086), 0}};
    shellplan::PhaseIn phases[40] = {};
    uint32_t nPhases = 0;
    shellplan::Plan plan;

    void Add(double t, bool pre) { phases[nPhases++] = {S(t), pre}; }
    Oracle() {
        const double goalsAt[7] = {15.139, 72.432, 115.389, 198.725, 272.221, 333.073, 432.086};
        const double pre[6] = {34.716, 92.150, 134.963, 218.293, 291.758, 352.664};
        for (int i = 0; i < 7; ++i) {
            Add(goalsAt[i], false);           // point_score_enter
            if (i < 6) {
                Add(goalsAt[i] + 15.0, false);   // post_score_replay / team_room / transition
                Add(pre[i], true);               // pre_launch_enter
                Add(pre[i] + 0.02, true);        // pre_launch_update
                Add(pre[i] + 15.05, false);      // main_loop
            }
        }
        plan.Build(S(0.0), goals, 7, phases, nPhases);
    }
};

}  // namespace

TEST_CASE("SHELLPLAN: the owner's capture gives six restarts in the oracle's colours") {
    Oracle o;
    REQUIRE(o.plan.Count() == 7);   // R[0] plus one restart per goal except the match winner
    const double r[6] = {34.716, 92.150, 134.963, 218.293, 291.758, 352.664};
    const Colour c[6] = {Colour::Red, Colour::Red, Colour::Red, Colour::Blue, Colour::Red, Colour::Red};
    CHECK(o.plan.At(0u).colour == Colour::Blue);
    for (uint32_t k = 1; k < 7; ++k) {
        CHECK(o.plan.At(k).r_us == S(r[k - 1]));
        CHECK(o.plan.At(k).colour == c[k - 1]);
        const double afterGoal = double(o.plan.At(k).r_us - o.goals[k - 1].t_us) / 1e6;
        CHECK(afterGoal > 10.0);
        CHECK(afterGoal < 30.0);
    }
}

TEST_CASE("SHELLPLAN: want is unknown until the grab is scanned, then exact at every boundary") {
    Oracle o;
    CHECK_FALSE(o.plan.WantAt(S(40.0)).known);
    CHECK(o.plan.FirstUnscanned() == 0);
    const double g[7] = {5.0, 51.4, 108.3, 151.5, 235.0, 308.0, 368.8};
    for (uint32_t k = 0; k < 7; ++k) o.plan.SetGrab(k, S(g[k]));
    CHECK(o.plan.FirstUnscanned() == 7);

    const shellplan::Want before = o.plan.WantAt(-1);
    CHECK(before.known);
    CHECK_FALSE(before.show);

    for (uint32_t k = 1; k < 7; ++k) {
        const int64_t rk = o.plan.At(k).r_us;
        const int64_t gk = S(g[k]);
        const shellplan::Want justBefore = o.plan.WantAt(rk - 1);
        CHECK(justBefore.k == int32_t(k - 1));
        CHECK_FALSE(justBefore.show);   // the previous span's grab is long past
        const shellplan::Want atR = o.plan.WantAt(rk);
        CHECK(atR.k == int32_t(k));
        CHECK(atR.show);
        CHECK(atR.colour == o.plan.At(k).colour);
        CHECK(o.plan.WantAt(gk - 1).show);
        CHECK_FALSE(o.plan.WantAt(gk).show);
        CHECK_FALSE(o.plan.WantAt(gk + 1).show);
    }
    // A span with no grab in it shows to its end.
    o.plan.SetNoGrab(6);
    CHECK(o.plan.WantAt(S(454.0)).show);
    CHECK(o.plan.WantAt(S(454.0)).k == 6);
}

TEST_CASE("SHELLPLAN: a capture without goals has only R[0]; a restart needs a goal before it") {
    shellplan::Plan p;
    const shellplan::PhaseIn early[1] = {{S(20.0), true}};   // a pre_launch before any goal
    p.Build(S(10.0), nullptr, 0, early, 1);
    CHECK(p.Count() == 1);
    CHECK(p.At(0u).r_us == S(10.0));

    // 2026-09-19_21-11-01's shape: one goal that did not end the match, and a restart after it.
    const shellplan::GoalIn goal[1] = {{S(79.6), 0}};
    const shellplan::PhaseIn ph[3] = {{S(19.2), true}, {S(79.6), false}, {S(99.2), true}};
    p.Build(S(0.0), goal, 1, ph, 3);
    REQUIRE(p.Count() == 2);
    CHECK(p.At(1u).r_us == S(99.2));
    CHECK(p.At(1u).colour == Colour::Red);
}

// ---- the reconciler -------------------------------------------------------------------------------

namespace {

shellfeed::Input Showing(int32_t k, Colour c, bool on, Colour onColour, uint64_t nowMs,
                         uint32_t epoch = 0) {
    shellfeed::Input in;
    in.want.known = true;
    in.want.k = k;
    in.want.show = true;
    in.want.colour = c;
    in.obs.read = true;
    in.obs.on = on;
    in.obs.colour = onColour;
    in.allowed = true;
    in.epoch = epoch;
    in.now_ms = nowMs;
    return in;
}

}  // namespace

TEST_CASE("SHELLFEED: the words map to on and colour, orange meaning Red") {
    CHECK_FALSE(shellfeed::ObserveWords(true, 0, 0).on);
    CHECK(shellfeed::ObserveWords(true, 0, 1).colour == Colour::Red);
    CHECK(shellfeed::ObserveWords(true, 1, 0).colour == Colour::Blue);
    CHECK_FALSE(shellfeed::ObserveWords(false, 1, 1).read);
}

TEST_CASE("SHELLFEED: one delivery per (span, epoch), up to three retries a second apart, then refused") {
    shellfeed::Reconciler rc;
    shellfeed::Step s = rc.Next(Showing(1, Colour::Red, false, Colour::Blue, 0));
    CHECK(s.action == shellfeed::Action::Show);
    CHECK(s.colour == Colour::Red);
    CHECK_FALSE(s.retry);

    // Sweeps inside the second wait for the graph.
    for (uint64_t ms = 5; ms < shellfeed::kRetryMs; ms += 5) {
        CHECK(rc.Next(Showing(1, Colour::Red, false, Colour::Blue, ms)).action ==
              shellfeed::Action::None);
    }
    for (uint32_t retry = 1; retry <= shellfeed::kMaxRetries; ++retry) {
        s = rc.Next(Showing(1, Colour::Red, false, Colour::Blue, retry * shellfeed::kRetryMs));
        CHECK(s.action == shellfeed::Action::Show);
        CHECK(s.retry);
    }
    s = rc.Next(Showing(1, Colour::Red, false, Colour::Blue, 4000));
    CHECK(s.action == shellfeed::Action::Refused);
    CHECK(rc.Next(Showing(1, Colour::Red, false, Colour::Blue, 9000)).action ==
          shellfeed::Action::None);

    // A seek is a new epoch: the same span may be delivered again.
    s = rc.Next(Showing(1, Colour::Red, false, Colour::Blue, 10000, 1));
    CHECK(s.action == shellfeed::Action::Show);
    CHECK_FALSE(s.retry);
}

TEST_CASE("SHELLFEED: the state reached stops delivery; a paused drag sends once, not once per seek") {
    shellfeed::Reconciler rc;
    CHECK(rc.Next(Showing(2, Colour::Blue, false, Colour::Blue, 0)).action == shellfeed::Action::Show);
    CHECK(rc.Next(Showing(2, Colour::Blue, true, Colour::Blue, 50)).action == shellfeed::Action::None);

    shellfeed::Reconciler paused;
    uint32_t sends = 0;
    for (uint32_t epoch = 0; epoch < 40; ++epoch) {
        shellfeed::Input in = Showing(3, Colour::Red, false, Colour::Blue, epoch * 100, epoch);
        in.clock_running = false;
        in.transport_set = true;
        if (paused.Next(in).action == shellfeed::Action::Show) ++sends;
    }
    CHECK(sends == 1);
}

TEST_CASE("SHELLFEED: a shell in the wrong colour is hidden, then shown in the right one") {
    shellfeed::Reconciler rc;
    shellfeed::Step s = rc.Next(Showing(4, Colour::Blue, true, Colour::Red, 0));
    CHECK(s.action == shellfeed::Action::Hide);
    CHECK_FALSE(s.residual);
    s = rc.Next(Showing(4, Colour::Blue, false, Colour::Blue, 20));
    CHECK(s.action == shellfeed::Action::Show);
    CHECK(s.colour == Colour::Blue);
}

TEST_CASE("SHELLFEED: past the grab, the hide is the engine's at 1x and ours only on a seek-set unowned disc") {
    shellfeed::Input in = Showing(5, Colour::Red, true, Colour::Red, 0);
    in.want.show = false;

    shellfeed::Reconciler play;
    in.transport_set = false;
    in.disc_owned = false;
    CHECK(play.Next(in).action == shellfeed::Action::None);   // forward play: the grab edge comes

    shellfeed::Reconciler owned;
    in.transport_set = true;
    in.disc_owned = true;
    CHECK(owned.Next(in).action == shellfeed::Action::None);  // the level publish makes the grab

    shellfeed::Reconciler seek;
    in.disc_owned = false;
    const shellfeed::Step s = seek.Next(in);
    CHECK(s.action == shellfeed::Action::Hide);
    CHECK(s.residual);
}

TEST_CASE("SHELLFEED: nothing is delivered when live, unknown or unread") {
    shellfeed::Input in = Showing(1, Colour::Red, false, Colour::Blue, 0);
    shellfeed::Reconciler rc;
    in.allowed = false;
    CHECK(rc.Next(in).action == shellfeed::Action::None);
    in.allowed = true;
    in.want.known = false;
    CHECK(rc.Next(in).action == shellfeed::Action::None);
    in.want.known = true;
    in.obs.read = false;
    CHECK(rc.Next(in).action == shellfeed::Action::None);
}

// ---- HOST-SYNC-2: first grabs from the whole file, and the FX graph of the bound disc -----------

TEST_CASE("SHELLPLAN: first grabs come from the possession edges, bounded by each span") {
    Oracle o;
    // Grabs in spans 0-5, span 6 never grabbed; each span ends with the disc released and loose.
    const double grab[6] = {5.0, 51.4, 108.3, 151.5, 235.0, 308.0};
    shellplan::HolderEdgeIn edges[16] = {};
    uint32_t n = 0;
    edges[n++] = {S(1.0), false};
    for (int k = 0; k < 6; ++k) {
        edges[n++] = {S(grab[k]), true};
        edges[n++] = {S(grab[k] + 3.0), false};
    }
    o.plan.FillGrabs(edges, n);

    REQUIRE(o.plan.FirstUnscanned() == 7);
    for (uint32_t k = 0; k < 6; ++k) {
        CHECK_FALSE(o.plan.At(k).grab_none);
        CHECK(o.plan.At(k).g_us == S(grab[k]));
    }
    // Span 6 has only span 5's release before it and nothing after: no grab, and the next span's
    // edges (there are none) cannot leak backwards either.
    CHECK(o.plan.At(6u).grab_none);

    // A seek far past a restart answers from the whole-file index, not from a window.
    const shellplan::Want farPast = o.plan.WantAt(S(300.0));   // span 5, R 291.758, G 308.0
    CHECK(farPast.known);
    CHECK(farPast.k == 5);
    CHECK(farPast.show);
    CHECK_FALSE(o.plan.WantAt(S(340.0)).show);
    CHECK(o.plan.WantAt(S(420.0)).show);   // span 6, never grabbed

    // A grab edge in the NEXT span does not end this one: span 5 grabbed only after R[6].
    shellplan::HolderEdgeIn late[2] = {{S(1.0), false}, {S(360.0), true}};
    o.plan.FillGrabs(late, 2);
    CHECK(o.plan.At(5u).grab_none);
    CHECK(o.plan.At(6u).g_us == S(360.0));

    // Held through a restart: grabbed at R, so that span never shows.
    shellplan::HolderEdgeIn held[2] = {{S(1.0), false}, {S(90.0), true}};
    o.plan.FillGrabs(held, 2);
    CHECK(o.plan.At(1u).g_us == S(90.0));
    CHECK(o.plan.At(2u).g_us == o.plan.At(2u).r_us);
    CHECK_FALSE(o.plan.WantAt(o.plan.At(2u).r_us).show);

    o.plan.ClearGrabs();
    CHECK(o.plan.FirstUnscanned() == 0);
    CHECK_FALSE(o.plan.WantAt(S(300.0)).known);
}

TEST_CASE("SHELLFEED: the FX graph's actor is the census row the disc track drives") {
    IEntity::DiscRingReading ring;
    ring.actorsWritten = 3;
    ring.actors[0].actorId = 0x1111;
    ring.actors[0].physId = 7;
    ring.actors[1].actorId = 0x2222;
    ring.actors[1].physId = 9;
    ring.actors[1].bound = true;
    ring.actors[2].actorId = 0x3333;
    ring.actors[2].physId = 11;
    uint64_t actor = 0;
    const char* why = nullptr;

    REQUIRE(shellfeed::BoundDiscActor(ring, 9, actor, &why));
    CHECK(actor == 0x2222);   // not the first row

    CHECK_FALSE(shellfeed::BoundDiscActor(ring, 7, actor, &why));
    REQUIRE(why != nullptr);
    CHECK(strstr(why, "component id") != nullptr);

    ring.actors[2].bound = true;
    CHECK_FALSE(shellfeed::BoundDiscActor(ring, 9, actor, &why));
    CHECK(strstr(why, "two") != nullptr);

    ring.actors[1].bound = ring.actors[2].bound = false;
    CHECK_FALSE(shellfeed::BoundDiscActor(ring, 9, actor, &why));
    CHECK(strstr(why, "no disc-ring census actor") != nullptr);
}

#define SHELL_WIDEN_(x) L##x
#define SHELL_WIDEN(x) SHELL_WIDEN_(x)
#define SHELL_CAPTURE "2026-09-24_12-59-23_576ED3F8428EBC4B_CB60A4DE7E1CAF73.echodemo"

// The owner's capture, through the shipped load scan: one disc track, and each restart's first grab
// where LIVE-TRUTH 3.1 watched the shell fall.
TEST_CASE("SHELLPLAN: the load scan's possession index gives the owner's capture its six first grabs") {
    const std::wstring candidates[] = {
        SHELL_WIDEN(REPLAY_PLAYBACK_FIXTURE_DIR) L"/" SHELL_WIDEN(SHELL_CAPTURE),
        replay_test::CaptureFileW(SHELL_WIDEN(SHELL_CAPTURE)),
    };
    DemoReader reader;
    bool opened = false;
    for (const std::wstring& p : candidates) {
        if (!p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES && reader.Open(p.c_str())) {
            opened = true;
            break;
        }
    }
    if (!opened) {
        MESSAGE("SKIPPED BY NAME: the 2026-09-24 capture is on neither path");
        return;
    }
    TrackScan scan;
    REQUIRE(ScanTrackSources(reader, scan));
    REQUIRE(scan.have_tick_span);
    CHECK_FALSE(scan.holder_edges_incomplete);
    REQUIRE_FALSE(scan.holder_edges.empty());
    const uint64_t disc = scan.holder_edges.front().id_value;
    for (const TrackScan::HolderEdge& e : scan.holder_edges) CHECK(e.id_value == disc);

    std::vector<shellplan::HolderEdgeIn> edges;
    for (const TrackScan::HolderEdge& e : scan.holder_edges) {
        edges.push_back({e.t_us - scan.first_tick_us, e.owned});
        if (edges.size() > 1) CHECK(edges.back().t_us >= edges[edges.size() - 2].t_us);
    }
    Oracle o;
    o.plan.FillGrabs(edges.data(), uint32_t(edges.size()));
    const double seen[6] = {51.4, 108.3, 151.5, 235.0, 308.0, 368.8};
    for (uint32_t k = 1; k < 7; ++k) {
        const shellplan::Span& sp = o.plan.At(k);
        INFO("span ", k, " R ", sp.r_us / 1e6, " G ", sp.g_us / 1e6);
        REQUIRE_FALSE(sp.grab_none);
        CHECK(fabs(sp.g_us / 1e6 - seen[k - 1]) < 0.5);
    }
}

TEST_CASE("HOST-SYNC-3: a Show sends the state and then the particle arm the state's thread would") {
    namespace sgf = replay::scriptgraph_facts;
    const shellfeed::ShowEvents red = shellfeed::ShowEventsFor(Colour::Red);
    CHECK(red.fxState == sgf::kSymFxStateRed);
    CHECK(red.spawn == sgf::kSymSpawnOrange);   // w6, the orange thread, is the Red state's
    const shellfeed::ShowEvents blue = shellfeed::ShowEventsFor(Colour::Blue);
    CHECK(blue.fxState == sgf::kSymFxStateBlue);
    CHECK(blue.spawn == sgf::kSymSpawnBlue);
}

TEST_CASE("HOST-SYNC-3: the first-grab line is said once per span and seek, not at every catch") {
    shellfeed::GrabClearLatch latch;
    CHECK(latch.Fall(6, 0));
    CHECK_FALSE(latch.Fall(6, 0));   // 370.59, 379.67, 380.41 ... in RUN-VERIFY-4
    CHECK_FALSE(latch.Fall(6, 0));
    CHECK(latch.Fall(6, 1));         // a seek back into the same span is a new first grab
    CHECK(latch.Fall(0, 1));
    CHECK_FALSE(latch.Fall(0, 1));
    latch.Reset();
    CHECK(latch.Fall(0, 1));
}
