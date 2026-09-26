// W2-B's offline gate. Everything here runs on a machine with no game.
//
// ---------------------------------------------------------------------------------------------
// â˜… WHAT THIS SUITE ESTABLISHES AND WHAT IT CANNOT.
//
// ESTABLISHES  (a) G2: the cursor resolver computes the same function as the binary search, over
//              random queries in random order INCLUDING strictly descending runs -- which is the
//              property that makes reverse playback and backward seek correct for netvars, and the
//              one that a forward-only implementation passes every forward test while failing.
//              (b) The transport's rate invariant and its fidelity property -- every commanded step
//              is a recorded dt -- over a sweep of capture/render/rate cells, with injected hitches.
//              (c) That the residue battery counts an unavailable primitive rather than skipping it.
//
// CANNOT       say anything about the ENGINE. ApplyStateAt, the one-node history ring and the
//              interpolator agreement (W2-B.md gates G3/G4) need a loaded match and are parked in
//              runs/queue/payload.json. â˜… THIS SUITE PASSING IS NOT PLAYBACK WORKING. It is the half
//              that does not need a launch, reported as that half.
//
// â˜… EVERY RED ARM IS COMPILED AND RUN, not described. ForwardOnlyCursorMutant and the unsigned-
// accumulator loop below are the two most plausible ways to write these wrong; both are instantiated
// here and both are asserted to DIVERGE. A test that asserts a mutant diverges also guards the gate's
// own discriminating power: if a future change made the mutant agree, this file goes red rather than
// quietly losing the ability to tell the two apart.
// ---------------------------------------------------------------------------------------------

#include "doctest.h"

#include <windows.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <limits>
#include <ratio>

#include "bootstrap/buildguard.h"
#include "bootstrap/livesafety.h"
#include "capture/capture_types.h"
#include "capture/chapterbuilder.h"
#include "capture/gather.h"
#include "capture/manifestbuild.h"
#include "foundation/demofile/demofile.h"
#include "playback/core/apply.h"
#include "playback/core/playback.h"
#include "playback/core/discontinuity.h"
#include "playback/core/ownership.h"
#include "playback/core/resolve.h"
#include "playback/core/sample.h"
#include "playback/core/transport.h"
#include "playback/core/windowbuild.h"
#include "smoothing_fixture.h"

using namespace replay::playback;

namespace {

// A deterministic PRNG. rand() differs between runtimes, and a gate whose inputs depend on the CRT is
// a gate whose failures cannot be reproduced from the log.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t Next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    uint64_t Below(uint64_t n) { return n ? Next() % n : 0; }
};

}  // namespace

// =============================================================================================
// G2 -- resolution
// =============================================================================================

TEST_CASE("ResolveBySearch is the definition, including its edges") {
    const uint64_t t[] = {100, 200, 200, 300, 400};   // a duplicated stamp is legal and must resolve  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF plain 300 as a count, size or pixel value, not the extrapolation-cap offset
    CHECK(ResolveBySearch(t, 5, 99) == kNoRecord);
    CHECK(ResolveBySearch(t, 5, 100) == 0);
    CHECK(ResolveBySearch(t, 5, 150) == 0);
    CHECK(ResolveBySearch(t, 5, 200) == 2);           // the LAST record at or before t
    CHECK(ResolveBySearch(t, 5, 399) == 3);
    CHECK(ResolveBySearch(t, 5, 100000) == 4);
    CHECK(ResolveBySearch(t, 0, 100) == kNoRecord);
    // A negative playhead is representable (B.5 F-4) and must not wrap into "the last record".
    CHECK(ResolveBySearch(t, 5, -1) == kNoRecord);
    CHECK(ResolveBySearch(t, 5, -1000000) == kNoRecord);
}

TEST_CASE("G2: the cursor computes the definition, in any query order") {
    Rng rng(0xC0FFEE);
    uint64_t times[512];
    uint64_t at = 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    for (int i = 0; i < 512; ++i) {
        at += 1 + rng.Below(40000);
        times[i] = at;
    }
    const int64_t lo = 0;
    const int64_t hi = (int64_t)times[511] + 50000;

    SUBCASE("random order") {
        SeriesCursor c;
        for (int i = 0; i < 20000; ++i) {
            const int64_t q = lo + (int64_t)rng.Below((uint64_t)(hi - lo));
            REQUIRE(c.Resolve(times, 512, q) == ResolveBySearch(times, 512, q));
        }
    }
    SUBCASE("strictly ascending -- the case a forward-only cursor also passes") {
        SeriesCursor c;
        for (int64_t q = lo; q < hi; q += 997) {
            REQUIRE(c.Resolve(times, 512, q) == ResolveBySearch(times, 512, q));
        }
    }
    SUBCASE("strictly descending -- reverse playback") {
        SeriesCursor c;
        for (int64_t q = hi; q > lo; q -= 997) {
            REQUIRE(c.Resolve(times, 512, q) == ResolveBySearch(times, 512, q));
        }
    }
    SUBCASE("alternating far jumps -- scrub-bar dragging") {
        SeriesCursor c;
        for (int i = 0; i < 5000; ++i) {
            const int64_t q = (i & 1) ? hi - (int64_t)rng.Below(20000)
                                      : lo + (int64_t)rng.Below(20000);
            REQUIRE(c.Resolve(times, 512, q) == ResolveBySearch(times, 512, q));
        }
    }
    SUBCASE("invalidation never changes the answer, only the cost") {
        SeriesCursor c;
        for (int i = 0; i < 4000; ++i) {
            const int64_t q = lo + (int64_t)rng.Below((uint64_t)(hi - lo));
            const size_t a = c.Resolve(times, 512, q);
            c.Invalidate();
            const size_t b = c.Resolve(times, 512, q);
            REQUIRE(a == b);
            REQUIRE(a == ResolveBySearch(times, 512, q));
        }
    }
}

TEST_CASE("G2's red arm: the forward-only cursor agrees going up and diverges coming back") {
    uint64_t times[256];
    uint64_t at = 500;
    for (int i = 0; i < 256; ++i) {
        at += 11111;
        times[i] = at;
    }

    SUBCASE("ascending: the mutant is indistinguishable, which is why it would ship") {
        ForwardOnlyCursorMutant m;
        int checked = 0;
        for (int64_t q = 0; q < (int64_t)times[255] + 20000; q += 3000) {
            REQUIRE(m.Resolve(times, 256, q) == ResolveBySearch(times, 256, q));
            ++checked;
        }
        CHECK(checked > 100);
    }
    SUBCASE("descending: the mutant is WRONG, and this gate sees it") {
        ForwardOnlyCursorMutant m;
        SeriesCursor good;
        // Walk up, then back down -- exactly a play-then-reverse.
        for (int64_t q = 0; q < (int64_t)times[255]; q += 3000) {
            m.Resolve(times, 256, q);
            good.Resolve(times, 256, q);
        }
        int divergences = 0, compared = 0;
        for (int64_t q = (int64_t)times[255]; q > 0; q -= 3000) {
            const size_t truth = ResolveBySearch(times, 256, q);
            REQUIRE(good.Resolve(times, 256, q) == truth);
            if (m.Resolve(times, 256, q) != truth) {
                ++divergences;
            }
            ++compared;
        }
        // â˜… The gate's own discriminating power, asserted. If a change ever made the mutant agree,
        // this fails -- rather than the suite quietly losing the arm that makes G2 mean anything.
        CHECK(compared > 100);
        CHECK(divergences > 100);
    }
}

TEST_CASE("KeyedResolver resolves per key, in both directions, and refuses rather than dropping") {
    KeyedRecord seeds[3] = {{0, 0xAAA, 10}, {0, 0xBBB, 20}, {0, 0xCCC, 30}};
    KeyedRecord recs[6] = {
        {1000, 0xAAA, 11}, {1500, 0xBBB, 21}, {2000, 0xAAA, 12},  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
        {2500, 0xCCC, 31}, {3000, 0xAAA, 13}, {3500, 0xBBB, 22},
    };
    KeyedResolver r;
    REQUIRE(r.Build(seeds, 3, recs, 6));
    CHECK(r.KeyCount() == 3);

    uint64_t v = 0;
    SUBCASE("forward then backward gives the same values at the same times") {
        const int64_t qs[] = {0, 999, 1000, 1999, 2000, 2999, 3000, 3499, 3500, 9999};  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
        uint64_t fwd[10], bwd[10];
        for (int i = 0; i < 10; ++i) {
            REQUIRE(r.ValueAt(0xAAA, qs[i], v));
            fwd[i] = v;
        }
        for (int i = 9; i >= 0; --i) {
            REQUIRE(r.ValueAt(0xAAA, qs[i], v));
            bwd[i] = v;
        }
        for (int i = 0; i < 10; ++i) {
            REQUIRE(fwd[i] == bwd[i]);
            uint64_t truth = 0;
            REQUIRE(r.ValueAtBySearch(0xAAA, qs[i], truth));
            REQUIRE(fwd[i] == truth);
        }
        CHECK(fwd[0] == 10);   // the keyframe seed, before any in-chapter write
        CHECK(fwd[9] == 13);
    }
    SUBCASE("an unknown key is a false return, never a stale value") {
        v = 0xDEAD;
        CHECK_FALSE(r.ValueAt(0x123, 5000, v));
        CHECK(v == 0xDEAD);    // untouched on a false return
    }
    SUBCASE("a discontinuity invalidates every cursor without changing any answer") {
        Rng rng(7);
        for (int i = 0; i < 3000; ++i) {
            const uint64_t key = (&(seeds[rng.Below(3)]))->key;
            const int64_t q = (int64_t)rng.Below(6000);
            uint64_t a = 0, b = 0;
            const bool oka = r.ValueAt(key, q, a);
            r.InvalidateCursors();
            const bool okb = r.ValueAtBySearch(key, q, b);
            REQUIRE(oka == okb);
            if (oka) {
                REQUIRE(a == b);
            }
        }
    }
}

TEST_CASE("The match clock follows the playhead, interpolated, in both directions") {
    Window w;   // default caps size max_ticks > 0, room for this handful of samples
    // gameTimer counts up: 10.0 at 1000us, 20.0 at 3000us, 25.0 at 4000us.
    REQUIRE(w.AddGameStateClock(1000, 10.0f));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    REQUIRE(w.AddGameStateClock(3000, 20.0f));
    REQUIRE(w.AddGameStateClock(4000, 25.0f));
    CHECK(w.GameStateClockCount() == 3);

    float c = -1.0f;
    SUBCASE("before the first sample there is no clock: the engine's own value stands") {
        CHECK_FALSE(w.ClockAt(0, c));
        CHECK_FALSE(w.ClockAt(999, c));
    }
    SUBCASE("on a sample it reads that sample; between two it interpolates linearly") {
        REQUIRE(w.ClockAt(1000, c)); CHECK(c == doctest::Approx(10.0f));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
        REQUIRE(w.ClockAt(2000, c)); CHECK(c == doctest::Approx(15.0f));  // halfway 10 -> 20
        REQUIRE(w.ClockAt(3000, c)); CHECK(c == doctest::Approx(20.0f));
        REQUIRE(w.ClockAt(3500, c)); CHECK(c == doctest::Approx(22.5f));  // halfway 20 -> 25
    }
    SUBCASE("past the last sample it holds and does not extrapolate") {
        REQUIRE(w.ClockAt(4000, c)); CHECK(c == doctest::Approx(25.0f));
        REQUIRE(w.ClockAt(9000, c)); CHECK(c == doctest::Approx(25.0f));
    }
    SUBCASE("forward, after a seek, and paused all resolve ONE value for one T") {
        float fwd = 0, back = 0, paused1 = 0, paused2 = 0;
        REQUIRE(w.ClockAt(1500, fwd));      // walking forward into 1500
        REQUIRE(w.ClockAt(3500, c));        // jump forward
        REQUIRE(w.ClockAt(1500, back));     // seek back to 1500
        CHECK(fwd == back);                 // a pure function of T, not of the path taken to it
        REQUIRE(w.ClockAt(2000, paused1));  // paused: the same T resolved twice
        REQUIRE(w.ClockAt(2000, paused2));
        CHECK(paused1 == paused2);
        CHECK(paused1 == doctest::Approx(15.0f));
    }
}

TEST_CASE("Round points resolve to the last write at or before T, both directions") {
    // The real scoreboard netvar symbols -- gamestate_facts.h kVarBlueScore / kVarRedScore.
    const uint64_t kBlue = 0x1C97052A21E09F11ULL;
    const uint64_t kRed = 0x2ED07B1C8C27548DULL;
    KeyedRecord seeds[2] = {{0, kBlue, 0}, {0, kRed, 0}};
    KeyedRecord recs[4] = {
        {1000, kBlue, 1}, {2000, kRed, 1}, {3000, kBlue, 2}, {5000, kBlue, 3},  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    };
    KeyedResolver r;
    REQUIRE(r.Build(seeds, 2, recs, 4));
    uint64_t v = 0;
    REQUIRE(r.ValueAt(kBlue, 0, v));    CHECK(v == 0);   // the keyframe seed
    REQUIRE(r.ValueAt(kBlue, 1500, v)); CHECK(v == 1);
    REQUIRE(r.ValueAt(kBlue, 3000, v)); CHECK(v == 2);
    REQUIRE(r.ValueAt(kBlue, 4999, v)); CHECK(v == 2);   // last write <= T, NOT the 5000 one
    REQUIRE(r.ValueAt(kBlue, 5000, v)); CHECK(v == 3);
    // A backward seek past a goal returns the earlier, lower score: the scoreboard counts down.
    REQUIRE(r.ValueAt(kBlue, 1500, v)); CHECK(v == 1);
    REQUIRE(r.ValueAt(kRed, 2500, v));  CHECK(v == 1);
    REQUIRE(r.ValueAt(kRed, 1999, v));  CHECK(v == 0);
}

// =============================================================================================
// The transport
// =============================================================================================

namespace {

struct Cell {
    double capture_hz;
    double render_hz;
    double rate;
};

// Builds a boundary table at `hz` with +-50 us of recorded jitter, as S7.3.2's sweep specifies.
size_t BuildTicks(uint64_t* out, size_t cap, double hz, double seconds, Rng& rng) {
    const double nominal = 1.0e6 / hz;
    uint64_t t = 0;
    size_t n = 0;
    const double total = seconds * 1.0e6;
    while (n < cap && (double)t <= total) {
        out[n++] = t;
        const int64_t jitter = (int64_t)rng.Below(101) - 50;
        double step = nominal + (double)jitter;
        if (step < 1.0) {
            step = 1.0;
        }
        t += (uint64_t)llround(step);
    }
    return n;
}

// Is `d` one of the recorded dts? The fidelity property, checked against the table rather than against
// the value the loop just handed itself -- which is the tautology the design caught itself in.
bool IsRecordedDt(const uint64_t* b, size_t n, uint64_t d) {
    for (size_t i = 0; i + 1 < n; ++i) {
        if (b[i + 1] - b[i] == d) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("the transport holds the rate invariant and steps only in recorded ticks") {
    const Cell cells[] = {
        {72, 90, 1.0},   {90, 90, 1.0},   {120, 60, 2.0},  {44, 165, 0.25},
        {90, 60, 4.0},   {90, 144, 0.05}, {72, 60, -1.0},  {90, 90, -0.5},
        {120, 90, -2.0}, {44, 60, 0.05},
    };
    int cells_checked = 0;

    for (size_t ci = 0; ci < sizeof cells / sizeof cells[0]; ++ci) {
        const Cell& c = cells[ci];
        Rng rng(0xBEEF + ci);
        static uint64_t b[40000];
        const size_t n = BuildTicks(b, 40000, c.capture_hz, 60.0, rng);
        REQUIRE(n > 100);

        TickTable tt;
        tt.Set(b, n);
        Transport tr;
        TransportTuning tune;
        tune.max_substeps = 8;
        tr.Reset(tt, tune);

        // Reverse starts at the end so there is something behind the playhead to consume.
        tr.SeekTo(c.rate < 0 ? (int64_t)b[n - 1] : (int64_t)b[0]);
        int64_t from = 0, to = 0;
        Cause dc = Cause::Seek;
        tr.TakeDiscontinuity(from, to, dc);
        tr.SetRate(c.rate);
        tr.SetPaused(false);

        const int64_t start = tr.DisplayPlayheadUs();
        const double render_dt = 1.0e6 / c.render_hz;
        double wall_total = 0.0;
        uint64_t dropped = 0;
        int bad_steps = 0, zero_steps = 0;
        const int frames = (int)(20.0 * c.render_hz);

        for (int f = 0; f < frames; ++f) {
            // 0.1% render jitter, and five 250 ms hitches -- S7.3.2's acceptance requires them,
            // because an idle 30 s run has none and WALL_MAX passes without them.
            double wall = render_dt * (1.0 + ((double)rng.Below(2001) - 1000.0) * 1.0e-6);
            if (f && f % (frames / 6) == 0) {
                wall += 250000.0;
            }
            const uint64_t w = (uint64_t)llround(wall);
            // ★ THE SPENDABLE INTERVAL, NOT THE ELAPSED ONE (PLAYBACK-FIX-3 A). A hitch is time the
            // viewer spent looking at a frozen picture, and the transport deliberately does not pay
            // it back -- so the rate invariant is against what a frame was ALLOWED to spend. The
            // hitches stay in the sweep: they are what a mutant that clamps the wrong quantity, or
            // clamps nothing, still has to get right.
            wall_total += (double)(w > tune.max_wall_dt_us ? tune.max_wall_dt_us : w);
            if (w > tune.max_wall_dt_us) dropped += w - tune.max_wall_dt_us;

            const StepResult r = tr.Advance(w);

            if (r.commanded_step_us == 0) {
                ++zero_steps;
            }
            if (r.steps_consumed && !IsRecordedDt(b, n, r.commanded_step_us)) {
                ++bad_steps;
            }
        }

        const int64_t moved = tr.DisplayPlayheadUs() - start;
        const double expected = c.rate * wall_total;

        // â˜… NEVER ZERO. A zero step makes clock+0x40 = 1/0 = +inf (S7.3.3).
        CHECK(zero_steps == 0);
        // â˜… The fidelity property, against the recorded table.
        CHECK(bad_steps == 0);
        // bad_steps is the real fidelity check: it compares each commanded step against the recorded
        // TABLE, which is a question the data can answer no to. The counter that used to sit here
        // asked whether the step came from the table one line after reading it from the table, and
        // its red was unconstructible -- see the B6 case.
        CHECK(tr.GapsCrossed() == 0);   // these cells have no gaps, so this must be zero
        // Five hitches per cell: the transport reports exactly the real time it refused to spend.
        CHECK(tr.WallClamps() == 5);
        CHECK(tr.WallDroppedUs() == dropped);

        // The rate invariant. Only checked where the run did not hit an end stop -- at 4x over 20 s of
        // wall on a 60 s file the playhead reaches the end, and a clamp is E-P2 working, not a failure.
        const bool hit_end = (c.rate > 0 && tr.DisplayPlayheadUs() >= (int64_t)b[n - 1] - 100000) ||
                             (c.rate < 0 && tr.DisplayPlayheadUs() <= (int64_t)b[0] + 100000);
        if (!hit_end) {
            const double err = fabs((double)moved - expected);
            const double tol = fabs(expected) * 0.005 + 40000.0;   // 0.5% + one hitch's worth
            INFO("cell ", ci, " moved=", moved, " expected=", expected);
            CHECK(err < tol);
        }
        ++cells_checked;
    }
    CHECK(cells_checked == 10);
}

TEST_CASE("the transport's red arm: the unsigned accumulator makes reverse run away") {
    // The literal pseudocode in an earlier revision of the design had `acc -= d` rather than
    // `acc -= sgn * d`. An engineer implementing it verbatim ships a reverse transport that
    // accelerates. This reproduces both forms over the same input and asserts they diverge, so the
    // shipped sign is protected by a demonstration rather than by a comment.
    Rng rng(11);
    static uint64_t b[20000];
    const size_t n = BuildTicks(b, 20000, 90.0, 30.0, rng);
    REQUIRE(n > 100);

    const double rate = -1.0;
    const double render_dt = 1.0e6 / 90.0;
    const int frames = 900;

    // The shipped form.
    TickTable tt;
    tt.Set(b, n);
    Transport tr;
    TransportTuning tune;
    tune.max_substeps = 8;
    tr.Reset(tt, tune);
    tr.SeekTo((int64_t)b[n - 1]);
    int64_t f0 = 0, t0 = 0;
    Cause dc0 = Cause::Seek;
    tr.TakeDiscontinuity(f0, t0, dc0);
    tr.SetRate(rate);
    tr.SetPaused(false);
    const int64_t start_good = tr.DisplayPlayheadUs();
    for (int f = 0; f < frames; ++f) {
        tr.Advance((uint64_t)llround(render_dt));
    }
    const double moved_good = (double)(tr.DisplayPlayheadUs() - start_good);

    // The mutant: identical, except acc -= d.
    size_t idx = n - 1;
    double acc = 0.0;
    uint32_t consumed_total = 0;
    for (int f = 0; f < frames; ++f) {
        acc += rate * render_dt;
        uint32_t consumed = 0;
        while (consumed < 8 && idx > 0) {
            const uint64_t d = b[idx] - b[idx - 1];
            if (fabs(acc) < (double)d) {
                break;
            }
            --idx;
            acc -= (double)d;   // â˜… THE MUTATION: unsigned, so reverse ADDS to the deficit
            ++consumed;
            ++consumed_total;
        }
    }
    const double moved_bad = (double)b[idx] - (double)b[n - 1];
    const double expected = rate * render_dt * frames;

    INFO("good=", moved_good, " bad=", moved_bad, " expected=", expected);
    // The shipped form tracks the expectation; the mutant does not. Both arms asserted, so neither a
    // silently-correct mutant nor a silently-wrong shipped form can pass.
    CHECK(fabs(moved_good - expected) < fabs(expected) * 0.02 + 40000.0);
    CHECK(fabs(moved_bad - expected) > fabs(expected) * 0.10);
}

// â˜… WHAT A STEP MOVED IS COUNTED, NOT DIFFERENCED. L3-RUN-2 F4 was answered by reading the playhead
// either side of Step(), which is right about a refusal at an end of the table and wrong about a
// repeated boundary -- and a repeated boundary is legal, because a recorded tick table is
// non-decreasing and a stalled source records the same microsecond twice.
TEST_CASE("StepTicks returns the signed ticks it actually moved") {
    // Tick 2 is a recorded stall: b[2] == b[3], so stepping across it consumes a tick and leaves
    // every playhead where it was.
    static const uint64_t b[] = {0, 11111, 22222, 22222, 34444, 45555, 56666};
    const size_t n = sizeof b / sizeof b[0];
    TickTable tt;
    tt.Set(b, n);
    Transport tr;
    tr.Reset(tt, TransportTuning());

    SUBCASE("the count is signed, and short of what was asked when the table runs out") {
        tr.SeekTo((int64_t)b[1]);
        CHECK(tr.StepTicks(3) == 3);
        CHECK(tr.SolverPlayheadUs() == (int64_t)b[4]);
        CHECK(tr.StepTicks(-2) == -2);
        CHECK(tr.SolverPlayheadUs() == (int64_t)b[2]);
        // Asked for more than the table holds: the count is what was consumed, not what was asked.
        CHECK(tr.StepTicks(100) == (int)(n - 1) - 2);
        CHECK(tr.SolverPlayheadUs() == (int64_t)b[n - 1]);
        // And at the end, a refusal is 0 -- forwards. Backwards from the same place still moves.
        CHECK(tr.StepTicks(1) == 0);
        CHECK(tr.StepTicks(-100) == -(int)(n - 1));
        CHECK(tr.StepTicks(-1) == 0);
        CHECK(tr.SolverPlayheadUs() == (int64_t)b[0]);
        // A zero step consumes nothing and says so.
        CHECK(tr.StepTicks(0) == 0);
    }

    SUBCASE("a step across a repeated boundary moves a tick the playhead cannot see") {
        // Seeking to a repeated boundary lands on the LAST index holding it (IndexAt takes the
        // largest i with At(i) <= t), so the first of the pair is reached by stepping onto it.
        tr.SeekTo((int64_t)b[1]);
        CHECK(tr.StepTicks(1) == 1);
        const int64_t display = tr.DisplayPlayheadUs();
        const int64_t solver = tr.SolverPlayheadUs();
        CHECK(tr.StepTicks(1) == 1);
        // Both playheads are unchanged -- this is the case a differencing caller gets wrong.
        CHECK(tr.DisplayPlayheadUs() == display);
        CHECK(tr.SolverPlayheadUs() == solver);
        // But the tick really was consumed: the NEXT step lands on the following boundary, which it
        // could not do if the index had stayed where it was.
        CHECK(tr.StepTicks(1) == 1);
        CHECK(tr.SolverPlayheadUs() == (int64_t)b[4]);
    }
}

TEST_CASE("pause, step and the end stops") {
    Rng rng(3);
    static uint64_t b[8000];
    const size_t n = BuildTicks(b, 8000, 90.0, 10.0, rng);
    TickTable tt;
    tt.Set(b, n);
    Transport tr;
    tr.Reset(tt, TransportTuning());

    SUBCASE("paused: the playhead is frozen and the step is 1 us, never 0") {
        tr.SeekTo((int64_t)b[n / 2]);
        int64_t f = 0, t = 0;
        Cause dc = Cause::Seek;
        tr.TakeDiscontinuity(f, t, dc);
        const int64_t before = tr.DisplayPlayheadUs();
        for (int i = 0; i < 600; ++i) {
            const StepResult r = tr.Advance(11111);
            REQUIRE(r.commanded_step_us == 1);
            REQUIRE(r.steps_consumed == 0);
        }
        CHECK(tr.DisplayPlayheadUs() == before);
    }
    SUBCASE("a rate of zero is paused, not a zero step") {
        tr.SetPaused(false);
        tr.SetRate(0.0);
        CHECK(tr.Paused());
        const StepResult r = tr.Advance(11111);
        CHECK(r.commanded_step_us == 1);
    }
    SUBCASE("single-step lands exactly on recorded boundaries, both directions") {
        tr.SeekTo((int64_t)b[100]);
        int64_t f = 0, t = 0;
        Cause dc = Cause::Seek;
        tr.TakeDiscontinuity(f, t, dc);
        CHECK(tr.SolverPlayheadUs() == (int64_t)b[100]);
        tr.StepTicks(3);
        CHECK(tr.SolverPlayheadUs() == (int64_t)b[103]);
        CHECK(tr.DisplayPlayheadUs() == (int64_t)b[103]);
        tr.StepTicks(-5);
        CHECK(tr.SolverPlayheadUs() == (int64_t)b[98]);
        CHECK(tr.TakeDiscontinuity(f, t, dc));
    }
    SUBCASE("E-P1 and E-P2: seeks past either end clamp and do not wrap") {
        tr.SeekTo(-5000000);
        CHECK(tr.DisplayPlayheadUs() == (int64_t)b[0]);
        tr.SeekTo((int64_t)b[n - 1] + 5000000);
        CHECK(tr.DisplayPlayheadUs() == (int64_t)b[n - 1]);
        // Stepping past the end stops rather than running off the table.
        tr.StepTicks(100);
        CHECK(tr.SolverPlayheadUs() == (int64_t)b[n - 1]);
        tr.SeekTo((int64_t)b[0]);
        tr.StepTicks(-100);
        CHECK(tr.SolverPlayheadUs() == (int64_t)b[0]);
    }
    SUBCASE("every seek raises exactly one discontinuity, and two in a frame collapse") {
        int64_t f = 0, t = 0;
        Cause dc2 = Cause::Seek;
        while (tr.TakeDiscontinuity(f, t, dc2)) {
        }
        const int64_t origin = tr.DisplayPlayheadUs();
        tr.SeekTo((int64_t)b[10]);
        tr.SeekTo((int64_t)b[200]);
        REQUIRE(tr.TakeDiscontinuity(f, t, dc2));
        CHECK(f == origin);                    // the FIRST from -- what the residue must reconcile away from
        CHECK(t == (int64_t)b[200]);
        CHECK_FALSE(tr.TakeDiscontinuity(f, t, dc2));
    }
}

// â˜…â˜…â˜… L3-RUN-3 F1, THE HALF THAT LIVES IN THIS CLASS. The window refills its tick storage IN PLACE
// on every rebuild, so the table this transport borrowed silently becomes a different three chapters
// behind the same pointer -- with the COUNT it was handed at load. Nothing here could observe that
// before, because every case in this file hands the transport ONE table and never a second.
TEST_CASE("Retable lands the playhead in the new table BY TIME, not on the same index") {
    // Recorded-tick spacing, not round numbers: a fixture whose boundaries look like a capture's
    // is a fixture whose failure reads like the product's.
    static const uint64_t before[5] = {11111, 22222, 44444, 55555, 77777};
    static const uint64_t after[5] = {44444, 55555, 77777, 88888, 99999};
    TickTable a, b;
    a.Set(before, 5);
    b.Set(after, 5);

    Transport tr;
    tr.Reset(a, TransportTuning());
    tr.SeekTo(55555);
    int64_t f = 0, t = 0;
    Cause c = Cause::Seek;
    REQUIRE(tr.TakeDiscontinuity(f, t, c));       // the seek's own, consumed
    REQUIRE(tr.SolverPlayheadUs() == 55555);

    tr.Retable(b);
    // 55555 us is index 3 of the old table and index 1 of the new one. By index it would have
    // teleported to 88888; by time it stays exactly where the user left it.
    CHECK(tr.SolverPlayheadUs() == 55555);
    CHECK(tr.DisplayPlayheadUs() == 55555);
    // A rebuild is not a jump: the position in time did not change, so there is nothing for the
    // residue battery to reconcile and no discontinuity to consume.
    CHECK_FALSE(tr.TakeDiscontinuity(f, t, c));

    // And the new table's reach is real: the far end of it is now steppable, which is the whole of
    // what the launch could not do.
    tr.SeekTo(99999);
    CHECK(tr.SolverPlayheadUs() == 99999);
}

// â˜…â˜…â˜… L3-RUN-3 F5. A playhead of -20880 us was read off a file whose first tick is 0. The
// accumulator never discards wall time -- that is what makes a hitch survive -- but at the first
// recorded tick of the RECORDING there is no tick to spend it on, so it accrued into a position the
// file does not contain, every frame, for as long as reverse was held.
TEST_CASE("the display playhead never leaves the file's own span, at either end") {
    static const uint64_t b[4] = {11111, 22222, 44444, 55555};
    TickTable tt;
    tt.Set(b, 4);
    Transport tr;
    tr.Reset(tt, TransportTuning());

    // â˜… UNSET IS UNKNOWN, AND UNKNOWN CLAMPS NOTHING. A transport nobody told where the file ends
    // must not invent an end: the end of a resident three-chapter table is not the end of a file.
    CHECK_FALSE(tr.FileSpanKnown());
    tr.SetFileSpan(11111, 55555);
    CHECK(tr.FileSpanKnown());

    tr.SeekTo(11111);
    int64_t f = 0, t = 0;
    Cause c = Cause::Seek;
    tr.TakeDiscontinuity(f, t, c);
    tr.SetRate(-1.0);
    tr.SetPaused(false);
    for (int i = 0; i < 20; ++i) {
        tr.Advance(5000);
        INFO("reverse frame ", i);
        CHECK(tr.DisplayPlayheadUs() >= 11111);
    }
    // ★★★ THE END OF THE RECORDING, NOT A WINDOW THAT HAS NOT CAUGHT UP. Where the file span and
    // the resident table coincide the playhead has reached the START of the FILE, and that is a
    // state: clamped, paused, counted as an end clamp and NOT as coverage lag. The old assertion
    // here required the opposite, and so wrote the conflation into the gate.
    CHECK(tr.EndClamps() > 0);
    CHECK(tr.CoverageHolds() == 0);
    CHECK(tr.Ended() == replay::EndState::AtStart);
    CHECK(tr.Paused());

    const uint64_t ends_at_start = tr.EndClamps();
    tr.SetPaused(false);
    tr.SetRate(1.0);
    tr.SeekTo(55555);
    tr.TakeDiscontinuity(f, t, c);
    for (int i = 0; i < 20; ++i) {
        tr.Advance(5000);
        INFO("forward frame ", i);
        CHECK(tr.DisplayPlayheadUs() <= 55555);
    }
    // The same stop with the sign the other way round -- a mutant that keeps one end and drops the
    // other has to fail one of these two loops.
    CHECK(tr.EndClamps() > ends_at_start);
    CHECK(tr.Ended() == replay::EndState::AtEnd);
    CHECK(tr.Paused());
    CHECK(tr.CoverageHolds() == 0);

    // ★ THE SPAN CLAMP ITSELF, ON A SPAN STRICTLY INSIDE THE TABLE. The file's first CORE sample is
    // not always the first boundary the resident window holds, and only this clamp knows the
    // difference: without it the playhead runs off the front of the RECORDING while the table still
    // has rows behind it (L3-RUN-3 F5).
    Transport inner;
    inner.Reset(tt, TransportTuning());
    inner.SetFileSpan(22222, 44444);
    inner.SeekTo(44444);
    inner.TakeDiscontinuity(f, t, c);
    inner.SetRate(1.0);
    inner.SetPaused(false);
    for (int i = 0; i < 20; ++i) {
        inner.Advance(5000);
        INFO("inner-span frame ", i);
        CHECK(inner.DisplayPlayheadUs() <= 44444);
    }
    CHECK(inner.EndClamps() > 0);

    // A backwards span is a defect in the caller. Refused, not swapped: silently fixing it would
    // hide the one thing worth reporting about it.
    tr.SetFileSpan(99999, 11111);
    CHECK_FALSE(tr.FileSpanKnown());
}

TEST_CASE("a hitch is dropped, never caught up") {
    // ★★★ THIS CONTRACT IS THE REVERSE OF THE ONE THIS CASE SHIPPED WITH, AND DELIBERATELY SO.
    // S7.3.2 banked the whole of a hitch so no scene time was "discarded"; for a RECORDER that is
    // right, for a VIEWER it is the sprint. The seconds a stall took are seconds the watcher spent
    // on a frozen picture, and paying them back runs the replay through exactly the part he was
    // waiting to see. After a stall the playhead carries on from where it stopped, short by what
    // the stall cost and by nothing else -- which is what the two assertions below say.
    Rng rng(23);
    static uint64_t b[20000];
    const size_t n = BuildTicks(b, 20000, 90.0, 60.0, rng);
    TickTable tt;
    tt.Set(b, n);
    Transport tr;
    TransportTuning tune;
    tune.max_substeps = 8;
    tr.Reset(tt, tune);
    tr.SeekTo((int64_t)b[0]);
    int64_t f = 0, t = 0;
    Cause dc = Cause::Seek;
    tr.TakeDiscontinuity(f, t, dc);
    tr.SetRate(1.0);
    tr.SetPaused(false);

    const int64_t start = tr.DisplayPlayheadUs();
    double wall = 0.0;
    int64_t prev = start, worst_step = 0;
    for (int i = 0; i < 900; ++i) {
        uint64_t w = 11111;
        if (i == 200 || i == 400 || i == 600) {
            w = 500000;   // three half-second stalls, clear of the 250 ms clamp
        }
        wall += (double)w;
        tr.Advance(w);
        const int64_t now = tr.DisplayPlayheadUs();
        if (now - prev > worst_step) worst_step = now - prev;
        prev = now;
    }
    const double moved = (double)(tr.DisplayPlayheadUs() - start);
    const double spendable = wall - (double)tr.WallDroppedUs();
    INFO("moved=", moved, " wall=", wall, " spendable=", spendable,
         " dropped=", tr.WallDroppedUs(), " microseeks=", tr.MicroSeeks());
    // Exactly the three stalls' excess, and nothing else: a clamp on the wrong quantity, or one
    // that fires on ordinary frames, moves this number.
    CHECK(tr.WallClamps() == 3);
    CHECK(tr.WallDroppedUs() == 3 * (500000 - tune.max_wall_dt_us));
    // And over the spendable time the playhead is still on the 1x line, to within a tick.
    CHECK(fabs(moved - spendable) < spendable * 0.01 + 40000.0);
    // ★ AND NO FRAME EVER SHOWS A SKIP. A hitch frame still consumes its clamped 100 ms -- partly by
    // stepping, partly by a micro-seek of the few milliseconds eight substeps could not pay -- so
    // micro-seeks are not zero here and should not be. What must be true is that NO frame moved the
    // playhead further than one clamped interval plus a tick, which is the thing a viewer sees.
    INFO("worst single-frame advance = ", worst_step);
    CHECK(worst_step <= (int64_t)tune.max_wall_dt_us + 20000);
}

// =============================================================================================
// The residue seam
// =============================================================================================

namespace {

struct FakeResidue : IResidue {
    const char* id;
    Availability avail;
    bool reverse_too;
    int fired = 0;
    FakeResidue(const char* i, Availability a, bool rev) : id(i), avail(a), reverse_too(rev) {}
    const char* Id() const override { return id; }
    Availability Avail() const override { return avail; }
    const char* Diagnostic() const override { return "test"; }
    bool RunsOn(Cause c) const override {
        return c == Cause::Seek || (reverse_too && c == Cause::ReverseTick);
    }
    void Fire(const Discontinuity&) override { ++fired; }
};

}  // namespace

TEST_CASE("an unavailable residue primitive is counted, never silently skipped") {
    FakeResidue ready("N-5", Availability::Ready, true);
    FakeResidue unmapped("N-13", Availability::Unmapped, false);
    FakeResidue refused("N-9", Availability::RefusedByConfig, false);

    ResidueBattery bat;
    REQUIRE(bat.Register(&ready));
    REQUIRE(bat.Register(&unmapped));
    REQUIRE(bat.Register(&refused));
    CHECK(bat.Count() == 3);

    Discontinuity d;
    d.cause = Cause::Seek;
    CHECK(bat.Fire(d) == 1);          // only the Ready one ran
    CHECK(ready.fired == 1);
    CHECK(unmapped.fired == 0);
    CHECK(refused.fired == 0);
    CHECK(bat.SkippedTotal() == 2);   // â˜… and the other two are COUNTED

    // â˜… The user-facing half: the ids are retrievable, so "which parts of EVERYTHING is this build not
    // respecting" is answerable on screen rather than only in a log.
    const char* ids[4] = {nullptr, nullptr, nullptr, nullptr};
    CHECK(bat.UnavailableCount() == 2);
    CHECK(bat.UnavailableIds(ids, 4) == 2);
    CHECK(ids[0] != nullptr);
    CHECK(ids[1] != nullptr);

    // A reverse tick fires only the subset that declares it -- S7.5.5's {N-5, N-13, N-16}, expressed
    // as a per-primitive predicate rather than as a hard-coded list in the frame loop.
    d.cause = Cause::ReverseTick;
    CHECK(bat.Fire(d) == 1);
    CHECK(ready.fired == 2);
    CHECK(refused.fired == 0);
}

TEST_CASE("the cause and availability names are total") {
    for (int i = 0; i < (int)Cause::Count; ++i) {
        CHECK(CauseName((Cause)i)[0] != '?');
    }
    for (int i = 0; i < (int)Availability::Count; ++i) {
        CHECK(AvailabilityName((Availability)i)[0] != '?');
    }
}

// =============================================================================================
// INV-OWN1 -- the one assumption the owner-id ruling rests on
// =============================================================================================

TEST_CASE("INV-OWN1: an entrant id that would be locally controlled is refused, not published") {
    const uint32_t kInvalid = 0xFFFFFFFFu;  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    const uint32_t local = 0x00000003u;   // the replay session's own user id

    OwnerGuard g;
    g.Reset(local, kInvalid);

    SUBCASE("GREEN: real recorded entrant ids, none equal to the local id") {
        // Ids as the bot-admission path allocates them: a low-word slot inside the user table, with a
        // generation in the high word -- the "owned by someone who is not me" shape.
        const uint32_t ids[4] = {0x00010004u, 0x00010005u, 0x00020006u, 0x00010007u};
        for (int i = 0; i < 4; ++i) {
            REQUIRE(g.RegisterEntrant(ids[i]) == OwnerVerdict::Ok);
        }
        CHECK(g.EntrantCount() == 4);
        for (int i = 0; i < 4; ++i) {
            REQUIRE(g.Validate(ids[i]) == OwnerVerdict::Ok);
        }
        CHECK(g.Refusals() == 0);
    }

    SUBCASE("RED: an entrant allocated onto the local slot is refused and named") {
        // â˜… The failure this invariant exists for. IsLocallyControlled would return TRUE for this
        // entrant, physics would simulate its body, and the installed replay state would be
        // overwritten -- HP1's original defect arriving through the front door.
        CHECK(g.RegisterEntrant(local) == OwnerVerdict::IsLocalUser);
        CHECK(g.EntrantCount() == 0);              // refused, not recorded
        CHECK(g.Validate(local) == OwnerVerdict::IsLocalUser);
        CHECK(g.Refusals() == 1);
        CHECK(g.LastRefusal() == OwnerVerdict::IsLocalUser);
    }

    SUBCASE("RED: kInvalid is refused, and reported as itself rather than as out-of-table") {
        // Offline, owner == kInvalid means locally controlled (the S7.2.1 defect). Its low word is
        // ALSO out of the table, so a classifier that checked the bound first would send the next
        // reader to the wrong packet.
        CHECK(g.RegisterEntrant(kInvalid) == OwnerVerdict::IsInvalidSentinel);
        CHECK(g.Validate(kInvalid) == OwnerVerdict::IsInvalidSentinel);
    }

    SUBCASE("RED: the retired synthetic value is refused as the class it belongs to") {
        // 0xFFFE0010 -- the value this packet's F1 was about. Low word 0x0010 is exactly the first
        // index past the user table, so no engine branch owns it.
        CHECK(g.RegisterEntrant(0xFFFE0010u) == OwnerVerdict::OutOfTable);  // facts-ok: ENTITY.OWNER_SYNTHETIC PC_PRESENT, so gen_facts emits no value; this is the fixture's expected owner word
        CHECK(g.Validate(0xFFFE0010u) == OwnerVerdict::OutOfTable);  // facts-ok: ENTITY.OWNER_SYNTHETIC PC_PRESENT, so gen_facts emits no value; this is the fixture's expected owner word
    }

    SUBCASE("an id we never admitted is refused: we did not create that entrant") {
        REQUIRE(g.RegisterEntrant(0x00010004u) == OwnerVerdict::Ok);
        CHECK(g.Validate(0x00010009u) == OwnerVerdict::NotRegistered);
        CHECK(g.Validate(0x00010004u) == OwnerVerdict::Ok);
    }

    SUBCASE("re-admitting the same entrant is idempotent, not an error") {
        REQUIRE(g.RegisterEntrant(0x00010004u) == OwnerVerdict::Ok);
        REQUIRE(g.RegisterEntrant(0x00010004u) == OwnerVerdict::Ok);
        CHECK(g.EntrantCount() == 1);
    }

    SUBCASE("an unconfigured guard refuses everything rather than passing everything") {
        OwnerGuard fresh;
        // GuardUnconfigured, not NotRegistered: "nobody set the guard up" is a finding about US, and
        // "that is not an entrant we admitted" is a finding about the replay.
        CHECK(fresh.Validate(0x00010004u) == OwnerVerdict::GuardUnconfigured);
    }
}

TEST_CASE("the owner verdict names are total") {
    for (int i = 0; i < (int)OwnerVerdict::Count; ++i) {
        CHECK(OwnerVerdictName((OwnerVerdict)i)[0] != '?');
    }
}

// =============================================================================================
// ApplyStateAt and the one-node ring
// =============================================================================================

namespace {

// Records what the applier installed, so the assertions are about the WRITES rather than about the
// applier's own return values. A fake that only counted calls would pass an applier that installed
// the wrong body's pose into the right handle.
struct FakeEntity : replay::IEntity {
    static const size_t kSlots = 32;
    struct Slot {
        bool installed = false;
        size_t depth = 0;
        BodyState s{};
        uint64_t t = 0;
        size_t installs = 0;
    };
    Slot slots[kSlots];
    bool locally_controlled[kSlots] = {};
    bool fail_install = false;

    size_t PhysCount() const override { return kSlots; }
    bool IsLocallyControlled(PhysHandle h) const override { return locally_controlled[h.v % kSlots]; }
    // W1-E's SetSyntheticOwner writes OWNER_SYNTHETIC into PHYS.CI_OWNER_OFF, and PHYS.CI_OWNER_OFF
    // is the word IsLocallyControlled reads. A fake that swallowed the write would let the applier
    // claim a body and still find it refused -- a state indistinguishable from the claim working,
    // which is the whole thing the claim has to be proven on.
    mutable size_t synthetic_owner_calls[kSlots] = {};
    void SetSyntheticOwner(PhysHandle h) override {
        ++synthetic_owner_calls[h.v % kSlots];
        locally_controlled[h.v % kSlots] = false;
    }
    void ClearSyntheticOwner(PhysHandle) override {}
    bool ReadBody(PhysHandle, BodyState&) const override { return false; }
    void PushHistory(PhysHandle, const BodyState&, uint64_t) override {}
    void TrimHistory(PhysHandle, uint64_t, uint64_t) override {}
    bool SetHistoryWindow(PhysHandle h, const BodyState* s, const uint64_t* t, size_t n) override {
        if (fail_install || n == 0) return false;
        Slot& sl = slots[h.v % kSlots];
        sl.installed = true;
        sl.depth = n;
        sl.s = s[0];
        sl.t = t[0];
        ++sl.installs;
        return true;
    }
    size_t ReadHistoryWindow(PhysHandle h, BodyState* s, uint64_t* t, size_t cap) const override {
        const Slot& sl = slots[h.v % kSlots];
        if (!sl.installed || cap == 0) return 0;
        s[0] = sl.s;
        t[0] = sl.t;
        return 1;
    }
    float GetTimeOffset(PhysHandle) const override { return 0.0f; }
    void SetTimeOffset(PhysHandle, float) override {}
    size_t PlayerCount() const override { return kSlots; }
    bool ReadPlayer(PlayerHandle, PlayerState&) const override { return false; }
    void PushPlayerHistory(PlayerHandle, const PlayerState&, uint64_t) override {}
    // ---- the two pawn surfaces, ARCH-23 F-A / F-B ------------------------------------------
    // The defaults keep the pre-ARCH-23 shape: the REMOTE pool answers for every slot and the local
    // surface answers for none, which is what every case written before this packet assumed. A case
    // that wants the other arm says so. Both arms COUNT their asks -- the epoch cache and the
    // attempt cap are claims about how often a surface is touched, and a fake that did not count
    // could not tell a cached answer from a re-resolved one.
    struct LocalPawnRow {
        bool present = false;
        bool actor_null = false;
        bool duplicate = false;
        bool model = true;
        uint16_t phys = replay::playback::kPawnAbsent;
    };
    bool remote_absent[kSlots] = {};      // PlayerOf refuses: no CI in the remote pool
    bool remote_no_phys[kSlots] = {};     // a PlayerCI carrying no body handle
    LocalPawnRow local_pawns[kSlots];
    bool local_surface_absent = false;    // no broadcaster component system at all
    mutable size_t remote_asks[kSlots] = {};
    mutable size_t local_asks[kSlots] = {};
    // BindEpochNow answers NotAvailable unless a case publishes an epoch, so the resolver's
    // uncached arm stays the default here and no existing case silently gains a cache.
    bool publish_epoch = false;
    uint64_t epoch = 1;
    // The entity key each surface publishes for a slot. `ok` is the word, never the value: 0 is a
    // legal id here exactly as it is in the engine, so a row cannot say "no key" by holding 0.
    struct EntityKeyRow { bool ok = false; uint16_t key = 0; };
    EntityKeyRow remote_entity[kSlots];
    EntityKeyRow local_entity[kSlots];
    mutable size_t entity_key_asks[kSlots] = {};
    replay::IEntity::EntityKey EntityKeyOf(replay::UserIndex u, replay::IEntity::PawnSurface s,
                                           uint16_t& out) const override {
        if (u.v >= kSlots) return replay::IEntity::EntityKey::NoPawn;
        ++entity_key_asks[u.v];
        const EntityKeyRow& row = s == replay::IEntity::PawnSurface::Local ? local_entity[u.v]
                                                                          : remote_entity[u.v];
        if (!row.ok) return replay::IEntity::EntityKey::NoPawn;
        out = row.key;
        return replay::IEntity::EntityKey::Ok;
    }

    PhysHandle PhysOf(PlayerHandle h) const override {
        if (h.v < kSlots && remote_no_phys[h.v]) return PhysHandle(replay::playback::kPawnAbsent);
        return PhysHandle(h.v);
    }
    PlayerHandle PlayerOf(UserIndex u) const override {
        if (u.v < kSlots) ++remote_asks[u.v];
        if (u.v < kSlots && remote_absent[u.v]) return PlayerHandle(replay::playback::kPawnAbsent);
        return PlayerHandle(u.v);
    }
    replay::IEntity::LocalPawn LocalPawnAt(UserIndex u,
                                           replay::IEntity::LocalPawnRef& out) const override {
        using Answer = replay::IEntity::LocalPawn;
        out = replay::IEntity::LocalPawnRef();
        if (u.v < kSlots) ++local_asks[u.v];
        if (local_surface_absent) return Answer::NoBroadcaster;
        if (u.v >= kSlots || !local_pawns[u.v].present) return Answer::SlotNotInPool;
        const LocalPawnRow& lp = local_pawns[u.v];
        out.inPool = true;
        out.duplicateInPool = lp.duplicate;
        if (lp.actor_null) return Answer::ActorNull;
        out.phys = PhysHandle(lp.phys);
        out.model = lp.model;
        return Answer::Ok;
    }
    BindEpoch BindEpochNow(uint64_t& out) const override {
        if (!publish_epoch) return BindEpoch::NotAvailable;
        out = epoch;
        return BindEpoch::Ok;
    }
    void SetLodThrottle(bool) override {}
    // LP-A, W1-E's local-pawn accessors. The applier does not use them -- the local pawn is published
    // like any other entrant -- but IEntity is one interface and a fake that stubs only what it
    // happens to need would stop compiling the moment the applier grew a use for them.
    // W1-E's durable net-id table, both ways. The fake carries a real one so the binding under test
    // is resolution, not a stub that always agrees.
    uint64_t net_ids[kSlots] = {};
    uint64_t NetIdOf(PhysHandle h) const override { return net_ids[h.v % kSlots]; }
    PhysHandle PhysOfNetId(uint64_t id) const override {
        if (id) {
            for (size_t i = 0; i < kSlots; ++i) {
                if (net_ids[i] == id) return PhysHandle((uint16_t)i);
            }
        }
        return PhysHandle(0xFFFF);   // the contract's "this session does not carry it"  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    }
    size_t LocalPlayerCount() const override { return 0; }
    UserIndex LocalPlayerAt(size_t) const override { return UserIndex(0xFFFF); }  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    uint32_t ReadLocalPlayer(UserIndex, PlayerState&) const override { return 0; }
    bool Available() const override { return true; }
    const char* Diagnostic() const override { return "fake"; }
};

struct FakePose : replay::IPosePublisher {
    static const size_t kSlots = 32;
    PlayerState last[kSlots];
    size_t calls[kSlots] = {};
    bool slots_equal_every_call = true;
    void Publish(PlayerHandle h, const PlayerState& a, const PlayerState& b) override {
        // PP-B: offline the engine makes both slots bit-identical itself, so a publisher that ever
        // handed them different values would be manufacturing a state the shipped game never has.
        if (memcmp(&a, &b, sizeof a) != 0) slots_equal_every_call = false;
        last[h.v % kSlots] = a;
        ++calls[h.v % kSlots];
    }
    const char* Strategy() const override { return "PP-B"; }
    bool NeedsLateTick() const override { return false; }
};

struct FakeNetVar : replay::INetVar {
    static const size_t kMax = 256;
    replay::NetVarWrite writes[kMax];
    size_t count = 0;
    // The shipping INetVar returns false on this build. The fake can do either, so both arms of the
    // applier's accounting are exercised rather than only the one that happens to ship today.
    bool accept = true;
    size_t Poll(replay::NetVarWrite*, size_t) override { return 0; }
    bool Write(const replay::NetVarWrite& w) override {
        if (!accept) return false;
        if (count < kMax) writes[count++] = w;
        return true;
    }
    bool Read(replay::Symbol64, uint8_t, uint64_t&) const override { return false; }
    size_t ObservedCount() const override { return 0; }
};

// A resolvable NET-actor identity, which is what BindBodyHandles will accept. Written through the
// helper so a test cannot accidentally build an identity with no fill bits and still bind.
BodyIdentity NetId(uint64_t v) {
    BodyIdentity id;
    id.value = v;
    id.fill = replay::capture::kFillBodyIdValue | replay::capture::kFillBodyIdNetActor;
    return id;
}

void MakeBody(BodyState& s, float x, float w) {
    memset(&s, 0, sizeof s);
    s.com[0] = x; s.com[1] = x * 2.0f; s.com[2] = x * 3.0f;
    s.ori[0] = 0.0f; s.ori[1] = 0.0f; s.ori[2] = sinf(w); s.ori[3] = cosf(w);
    s.vel[0] = x; s.angVel[1] = x;
    s.bits11 = (uint32_t)(int)x;
}

// A window with two bodies and one player, sampled at 90 Hz for a second.
void FillWindow(Window& w, size_t n = 90) {
    static uint64_t ticks[256];
    for (size_t i = 0; i < n; ++i) {
        const uint64_t t = 1000000 + i * 11111;
        ticks[i] = t;
        BodyState s;
        MakeBody(s, (float)i, (float)i * 0.01f);
        w.AddBodySample(NetId(0x1001), t, s);
    }
    for (size_t i = 0; i < n; ++i) {
        BodyState s;
        MakeBody(s, (float)i * -1.0f, (float)i * 0.02f);
        w.AddBodySample(NetId(0x1002), 1000000 + i * 11111, s);
    }
    for (size_t i = 0; i < n; ++i) {
        PlayerState p;
        memset(&p, 0, sizeof p);
        p.hmdOri[3] = 1.0f;
        p.lHandOri[3] = 1.0f;
        p.rHandOri[3] = 1.0f;
        p.lHandOff[0] = (float)i;
        p.bendBack = (float)i * 0.5f;
        p.bits28 = (uint32_t)i;
        p.holdL = (uint32_t)(i / 10);
        w.AddPlayerSample(0, 1000000 + i * 11111, p);   // capture slot 0, not a user id
    }
    w.SetTicks(ticks, n);
    w.Finalize();   // staging -> per-track contiguous runs; mandatory after the last Add*
    w.BodyAt(0).handle = PhysHandle(1); w.BodyAt(0).handle_valid = true;
    w.BodyAt(1).handle = PhysHandle(2); w.BodyAt(1).handle_valid = true;
    w.PlayerAt(0).handle = PlayerHandle(1); w.PlayerAt(0).handle_valid = true;
    // What W2-C resolves at bind time: capture slot 0 IS this recorded entrant's real user id.
    w.PlayerAt(0).user_id = 0x00010004u; w.PlayerAt(0).user_id_valid = true;
}

}  // namespace

TEST_CASE("the ring is exactly one node, and it carries the state at the playhead") {
    Window w;
    FillWindow(w);
    FakeEntity ent;
    FakePose pose;
    FakeNetVar nv;
    OwnerGuard og;
    og.Reset(0x00000003u, 0xFFFFFFFFu);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    REQUIRE(og.RegisterEntrant(0x00010004u) == OwnerVerdict::Ok);

    StateApplier ap;
    ap.Bind(&ent, &pose, &nv, &og);
    ApplyOptions opt;
    opt.verify_installed = true;

    const int64_t t = 1000000 + 45 * 11111;
    const ApplyStats st = ap.Apply(w, t, opt);

    CHECK(st.bodies_installed == 2);
    CHECK(st.players_published == 1);
    CHECK(ap.VerifyMismatches() == 0);
    // ONE node. Not "at most 20", not a straddling window: one, on every body, every frame.
    CHECK(ent.slots[1].depth == 1);
    CHECK(ent.slots[2].depth == 1);
    CHECK(ent.slots[1].t == (uint64_t)t);
    // Sampled exactly on a recorded boundary, so the installed value is that sample, not a blend.
    CHECK(ent.slots[1].s.com[0] == doctest::Approx(45.0f));
    CHECK(ent.slots[2].s.com[0] == doctest::Approx(-45.0f));
    // The discrete word is HELD, never blended.
    CHECK(ent.slots[1].s.bits11 == 45u);
    // PP-B hands both slots the same value.
    CHECK(pose.slots_equal_every_call);
}

TEST_CASE("G1 offline: arriving at t by walking and by jumping installs identical state") {
    // The design's central claim, run without a game. If ApplyStateAt carried ANY state across frames
    // that was not a cache of f(file, t), these two would differ -- which is exactly the bug that
    // shows up as "right when you play into it, wrong when you scrub to it".
    const int64_t target = 1000000 + 37 * 11111 + 5000;   // deliberately OFF a recorded boundary

    Window w1;
    FillWindow(w1);
    FakeEntity e1; FakePose p1; FakeNetVar n1; OwnerGuard o1;
    o1.Reset(3, 0xFFFFFFFFu); o1.RegisterEntrant(0x00010004u);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    StateApplier a1; a1.Bind(&e1, &p1, &n1, &o1);
    // Arm A: walk there frame by frame, forward, then past it, then back -- a play, an overshoot and
    // a reverse, which is the ugliest ordinary case.
    for (int64_t t = 1000000; t < target; t += 4000) a1.Apply(w1, t, ApplyOptions());
    for (int64_t t = target + 200000; t > target; t -= 4000) a1.Apply(w1, t, ApplyOptions());
    a1.Apply(w1, target, ApplyOptions());

    Window w2;
    FillWindow(w2);
    FakeEntity e2; FakePose p2; FakeNetVar n2; OwnerGuard o2;
    o2.Reset(3, 0xFFFFFFFFu); o2.RegisterEntrant(0x00010004u);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    StateApplier a2; a2.Bind(&e2, &p2, &n2, &o2);
    // Arm B: cold, then jump straight there. The cursors have never been anywhere.
    w2.InvalidateCursors();
    a2.Apply(w2, target, ApplyOptions());

    for (int h = 1; h <= 2; ++h) {
        INFO("body handle ", h);
        REQUIRE(e1.slots[h].installed);
        REQUIRE(e2.slots[h].installed);
        CHECK(e1.slots[h].t == e2.slots[h].t);
        CHECK(e1.slots[h].depth == e2.slots[h].depth);
        for (int k = 0; k < 3; ++k) CHECK(e1.slots[h].s.com[k] == e2.slots[h].s.com[k]);
        for (int k = 0; k < 4; ++k) CHECK(e1.slots[h].s.ori[k] == e2.slots[h].s.ori[k]);
        for (int k = 0; k < 3; ++k) CHECK(e1.slots[h].s.vel[k] == e2.slots[h].s.vel[k]);
        CHECK(e1.slots[h].s.bits11 == e2.slots[h].s.bits11);
    }
    // The published pose too -- field by field, never memcmp: PlayerState has padding.
    CHECK(p1.last[1].lHandOff[0] == p2.last[1].lHandOff[0]);
    CHECK(p1.last[1].bendBack == p2.last[1].bendBack);
    CHECK(p1.last[1].bits28 == p2.last[1].bits28);
    CHECK(p1.last[1].holdL == p2.last[1].holdL);
}

TEST_CASE("a locally controlled body is refused, not installed over") {
    // INV-OWN1's observable, asked of the engine's own predicate. If this were installed anyway the
    // engine would simulate the body and overwrite it, and nothing would report the difference.
    Window w;
    FillWindow(w);
    FakeEntity ent;
    ent.locally_controlled[2] = true;
    FakePose pose; FakeNetVar nv; OwnerGuard og;
    og.Reset(3, 0xFFFFFFFFu); og.RegisterEntrant(0x00010004u);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    StateApplier ap;
    ap.Bind(&ent, &pose, &nv, &og);
    const ApplyStats st = ap.Apply(w, 1000000 + 10 * 11111, ApplyOptions());
    CHECK(st.bodies_installed == 1);
    CHECK(st.bodies_locally_controlled == 1);
    CHECK(ent.slots[1].installed);
    CHECK_FALSE(ent.slots[2].installed);
}

TEST_CASE("an entity absent at the playhead is not published at all") {
    // E-P4: a backwards seek past a join must REMOVE the entity, not freeze it at its first pose.
    Window w;
    FillWindow(w);
    FakeEntity ent; FakePose pose; FakeNetVar nv; OwnerGuard og;
    og.Reset(3, 0xFFFFFFFFu); og.RegisterEntrant(0x00010004u);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    StateApplier ap;
    ap.Bind(&ent, &pose, &nv, &og);
    const ApplyStats st = ap.Apply(w, 500000, ApplyOptions());   // before the first sample
    CHECK(st.bodies_installed == 0);
    CHECK(st.bodies_absent == 2);
    CHECK(st.players_absent == 1);
    CHECK_FALSE(ent.slots[1].installed);
}

TEST_CASE("as-seen differs from ground truth in the sampling time and in nothing else") {
    Window w;
    FillWindow(w);
    w.PlayerAt(0).render_delay_us = 44440;   // the -0.04444f the client applied
    w.PlayerAt(0).time_offset_us = 0;

    FakeEntity e1; FakePose p1; FakeNetVar n1; OwnerGuard o1;
    o1.Reset(3, 0xFFFFFFFFu); o1.RegisterEntrant(0x00010004u);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    StateApplier a1; a1.Bind(&e1, &p1, &n1, &o1);
    FakeEntity e2; FakePose p2; FakeNetVar n2; OwnerGuard o2;
    o2.Reset(3, 0xFFFFFFFFu); o2.RegisterEntrant(0x00010004u);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    StateApplier a2; a2.Bind(&e2, &p2, &n2, &o2);

    const int64_t t = 1000000 + 50 * 11111;
    ApplyOptions ground; ground.as_seen = false;
    ApplyOptions seen; seen.as_seen = true;
    a1.Apply(w, t, ground);
    Window w2;
    FillWindow(w2);
    w2.PlayerAt(0).render_delay_us = 44440;
    a2.Apply(w2, t, seen);

    // The bodies are IDENTICAL between modes -- S8.3's claim, made checkable. As-seen is a per-entity
    // sampling time on the pose channel and nothing else.
    for (int h = 1; h <= 2; ++h) {
        for (int k = 0; k < 3; ++k) CHECK(e1.slots[h].s.com[k] == e2.slots[h].s.com[k]);
    }
    // The pose differs, and by the recorded delay: 44440 us is about four 11111 us samples back.
    CHECK(p1.last[1].lHandOff[0] == doctest::Approx(50.0f));
    CHECK(p2.last[1].lHandOff[0] == doctest::Approx(46.0f).epsilon(0.01));

    // An occluded player is SKIPPED, not published at a stale pose.
    Window w3;
    FillWindow(w3);
    w3.PlayerAt(0).occluded = true;
    FakeEntity e3; FakePose p3; FakeNetVar n3; OwnerGuard o3;
    o3.Reset(3, 0xFFFFFFFFu); o3.RegisterEntrant(0x00010004u);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    StateApplier a3; a3.Bind(&e3, &p3, &n3, &o3);
    const ApplyStats st = a3.Apply(w3, t, seen);
    CHECK(st.players_skipped_asseen == 1);
    CHECK(st.players_published == 0);
    CHECK(p3.calls[1] == 0);
}

TEST_CASE("a netvar row owned elsewhere is withheld from this path, by (name, slot)") {
    Window w;
    FillWindow(w);
    KeyedRecord recs[3] = {{1200000, 0xAA, 101}, {1200000, 0xBB, 201}, {1200000, 0xCC, 301}};
    REQUIRE(w.NetVars().Build(nullptr, 0, recs, 3));
    w.NoteNetVarSlot(0xAA, 0);
    w.NoteNetVarSlot(0xBB, 1);
    w.NoteNetVarSlot(0xCC, 1);

    FakeEntity ent; FakePose pose; FakeNetVar nv; OwnerGuard og;
    og.Reset(3, 0xFFFFFFFFu); og.RegisterEntrant(0x00010004u);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    StateApplier ap;
    ap.Bind(&ent, &pose, &nv, &og);

    ApplyOptions opt;
    opt.netvar_owned_elsewhere = [](uint64_t name, uint16_t slot) {
        return slot == 1 && name == 0xBB;
    };
    const ApplyStats st = ap.Apply(w, 1300000, opt);
    CHECK(st.netvars_attempted == 2);
    CHECK(st.netvars_refused == 0);
    REQUIRE(nv.count == 2);
    for (size_t i = 0; i < nv.count; ++i) CHECK(nv.writes[i].name != 0xBB);

    // It stays withheld frame after frame, and is never recorded as written.
    const ApplyStats again = ap.Apply(w, 1310000, opt);
    CHECK(again.netvars_attempted == 0);
    CHECK(nv.count == 2);

    // The hook is the only thing withholding it.
    w.InvalidateCursors();
    ap.Apply(w, 1300000, ApplyOptions());
    CHECK(nv.count == 5);
}

TEST_CASE("netvars are written on change, and re-written after a discontinuity") {
    // FakeNetVar accepts by default: this case is about the RESOLUTION and the shadow, not about
    // whether the engine accepts a write. B2 covers the latter.
    const ApplyOptions landed;
    Window w;
    FillWindow(w);
    KeyedRecord seeds[2] = {{0, 0xAA, 100}, {0, 0xBB, 200}};
    KeyedRecord recs[3] = {{1200000, 0xAA, 101}, {1400000, 0xAA, 102}, {1600000, 0xBB, 201}};
    REQUIRE(w.NetVars().Build(seeds, 2, recs, 3));
    // The shipped builder records a slot for EVERY netvar it ingests (windowbuild.cpp
    // FinishBuild -> NoteNetVarSlot; both ingest paths read one), and the applier refuses a
    // name it has no slot for rather than addressing it to slot 0 -- slot 0 is a real record.
    // A fixture that builds the resolver alone models a window the builder cannot produce, so
    // it supplies the slot too. The expectations below are unchanged.
    w.NoteNetVarSlot(0xAA, 0);
    w.NoteNetVarSlot(0xBB, 0);

    FakeEntity ent; FakePose pose; FakeNetVar nv; OwnerGuard og;
    og.Reset(3, 0xFFFFFFFFu); og.RegisterEntrant(0x00010004u);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    StateApplier ap;
    ap.Bind(&ent, &pose, &nv, &og);

    ApplyStats s1 = ap.Apply(w, 1300000, landed);
    CHECK(s1.netvars_attempted == 2);       // both keys, first time
    ApplyStats s2 = ap.Apply(w, 1310000, landed);
    CHECK(s2.netvars_attempted == 0);       // nothing changed
    CHECK(s2.netvars_unchanged == 2);
    ApplyStats s3 = ap.Apply(w, 1500000, landed);
    CHECK(s3.netvars_attempted == 1);       // 0xAA moved to 102

    // After a jump the engine holds whatever the pre-jump frame wrote, so "unchanged since our last
    // write" is a claim about a world that no longer exists. The shadow goes with the cursors.
    const size_t before = nv.count;
    w.InvalidateCursors();
    ApplyStats s4 = ap.Apply(w, 1500000, landed);
    CHECK(s4.netvars_attempted == 2);
    CHECK(nv.count == before + 2);

    // And going BACKWARDS resolves to the earlier value -- the property edge application cannot have.
    w.InvalidateCursors();
    ApplyStats s5 = ap.Apply(w, 1250000, landed);
    CHECK(s5.netvars_attempted == 2);
    bool saw_101 = false;
    for (size_t i = 0; i < nv.count; ++i) {
        if (nv.writes[i].name == 0xAA && nv.writes[i].rawSValue == 101) saw_101 = true;
    }
    CHECK(saw_101);
}

TEST_CASE("slerp takes the shortest path and reproduces its endpoints exactly") {
    float a[4] = {0, 0, 0, 1};
    float b[4] = {0, 0, 0, -1};          // the SAME rotation, negated
    float out[4];
    SlerpQuat(a, b, 0.5f, out);
    // Without the dot<0 flip this blends to a zero quaternion and the transform goes NaN.
    const float len = sqrtf(out[0]*out[0] + out[1]*out[1] + out[2]*out[2] + out[3]*out[3]);
    CHECK(len == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(fabsf(out[3]) == doctest::Approx(1.0f).epsilon(1e-4));

    float c[4] = {0.0f, 0.0f, 0.70710678f, 0.70710678f};
    SlerpQuat(a, c, 0.0f, out);
    CHECK(out[3] == doctest::Approx(1.0f).epsilon(1e-5));
    SlerpQuat(a, c, 1.0f, out);
    CHECK(out[2] == doctest::Approx(0.70710678f).epsilon(1e-5));
}

TEST_CASE("an unresolved capture slot is never published as somebody's pose") {
    // A capture slot is dense and per-file; a user id is the engine's. Publishing before W2-C has
    // resolved one to the other would write a recorded pose onto whichever entrant happened to land
    // on that handle -- the wrong avatar, with no error and no log line.
    Window w;
    FillWindow(w);
    w.PlayerAt(0).user_id_valid = false;
    FakeEntity ent; FakePose pose; FakeNetVar nv; OwnerGuard og;
    og.Reset(3, 0xFFFFFFFFu); og.RegisterEntrant(0x00010004u);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    StateApplier ap;
    ap.Bind(&ent, &pose, &nv, &og);
    const ApplyStats st = ap.Apply(w, 1000000 + 10 * 11111, ApplyOptions());
    CHECK(st.players_published == 0);
    // UNMAPPED, not refused: no mapping exists, so the guard was never asked. The two are
    // separate counters because "not wired yet" must not read as an ownership violation.
    CHECK(st.players_unmapped == 1);
    CHECK(st.players_refused_owner == 0);
    CHECK(pose.calls[1] == 0);
    CHECK(st.bodies_installed == 2);   // bodies are unaffected: they key on a durable u64 net id
}

// =============================================================================================
// The round trip: a real .echodemo, written through W2-A's writer, read back through BuildWindow
// =============================================================================================
//
// This is the one arm that proves the BINDING rather than the arithmetic. Everything above runs on a
// window this file constructed; this one runs on bytes that went through ManifestBuilder, DemoWriter,
// ChapterBuilder, the bit-packing codec, zstd, and back out through DemoReader and W2-A's decoders.
// If the two sides ever disagree about a field's meaning, this is where it shows.

namespace {

struct TempDemo {
    wchar_t path[512];
    TempDemo() {
        wchar_t dir[400];
        const DWORD n = GetTempPathW(400, dir);
        if (!n) { dir[0] = L'.'; dir[1] = 0; }
        _snwprintf_s(path, 512, _TRUNCATE, L"%lsw2b_rt_%lu.echodemo", dir, (unsigned long)GetCurrentProcessId());
    }
    ~TempDemo() { DeleteFileW(path); }
};

// One ring record, framed exactly as ChapterBuilder::Consume expects.
template <class T>
void Feed(replay::capture::ChapterBuilder& b, uint16_t kind, const T& payload) {
    uint8_t buf[sizeof(replay::capture::RecHeader) + sizeof(T)];
    replay::capture::RecHeader h;
    h.kind = kind;
    h.bytes = (uint16_t)sizeof(T);
    h.producer = 0;
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, &payload, sizeof payload);
    b.Consume(buf, (uint32_t)sizeof buf);
}

}  // namespace

TEST_CASE("round trip: a written .echodemo builds a Window whose samples are the ones captured") {
    using namespace replay::capture;
    using namespace replay;   // PlayerRecord, DemoWriter, DemoReader, kSectionCount
    TempDemo tmp;

    const uint64_t t0 = 5000000;
    const uint64_t dt = 11111;
    const uint32_t frames = 60;
    const uint64_t kNetIdA = 0x00000001DEADBEEFull;   // hi dword must be 1 (S5.4.5)
    const uint64_t kNetIdB = 0x00000001CAFEF00Dull;

    {
        ManifestBuilder mb;
        mb.Begin(t0, 0, 0x40);
        const uint8_t md5[16] = {0xca, 0xa0, 0x5e, 0xe3};
        mb.SetBuild(md5, 0x676F6C646D617374ull, 0x0102030405060708ull);
        mb.SetLevel(6300205991959903307ull, 0xF00Dull, 1780177787ull);
        mb.SetKeyframeIntervalUs(2000000);
        mb.SetUuid(1, 2);
        PlayerRecord p{};
        p.xplatformId = 0x1000;
        p.userSlot = 0;
        p.userIndex = 0;
        mb.AddPlayer(p);
        REQUIRE(mb.Refusal() == nullptr);

        DemoWriter writer;
        REQUIRE(writer.Open(tmp.path, mb.View()));

        BuilderConfig cfg;
        cfg.keyframeIntervalUs = 2000000;
        for (uint16_t i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;
        for (uint16_t i = 0; i < kSectionCount; ++i) cfg.sectionEnabled[i] = true;

        ChapterBuilder b;
        REQUIRE(b.Open(&writer, mb.View(), cfg));
        b.EmitMatchBegin(t0);

        for (uint32_t f = 0; f < frames; ++f) {
            const uint64_t st = t0 + f * dt;

            TickRec tr{};
            tr.sourceTimeUs = st;
            tr.simTimeUs = 1000000 + f * dt;
            tr.frameIndex = f + 1;
            tr.frameDeltaUs = (uint32_t)dt;
            tr.scaledDeltaUs = (uint32_t)dt;
            Feed(b, kRecTick, tr);

            for (int e = 0; e < 2; ++e) {
                SampledBodyRec br{};
                br.sourceTimeUs = st;
                br.frameIndex = f + 1;
                // The tagged pair, with the net-actor guarantee set: the value came from an
                // accessor that resolves through the net-actor table, so it IS a net id whatever
                // tag byte a component held.
                br.idValue = e ? kNetIdB : kNetIdA;
                br.idTag = 0;
                // A SAMPLED row carries no wire header, so kFillBodyHeader must stay clear: a blanket
                // 0xFFFFFFFF claims a header this record does not have and the encoder refuses it --
                // which it did, silently, into S_DROPPED. Claim exactly what was sourced.
                br.fill = kFillBodyTransform | kFillBodyLastHeld | kFillBodyBits |
                          kFillBodyIdValue | kFillBodyIdNetActor;
                br.handle = (uint16_t)e;
                br.state.com[0] = (float)f * (e ? -1.0f : 1.0f);
                br.state.com[1] = 2.0f;
                br.state.com[2] = 3.0f;
                br.state.ori[3] = 1.0f;
                br.state.vel[0] = (float)f;
                br.state.bits11 = f & 0x7FF;
                Feed(b, kRecSampledBody, br);
            }

            SampledPlayerRec pr{};
            pr.sourceTimeUs = st;
            pr.frameIndex = f + 1;
            pr.entity = 0;
            pr.fill = kFillPose | kFillRefVel | kFillBody | kFillActionWord | kFillHolding;
            pr.state.hmdOri[3] = 1.0f;
            pr.state.lHandOri[3] = 1.0f;
            pr.state.rHandOri[3] = 1.0f;
            pr.state.lHandOff[0] = (float)f * 0.001f;
            pr.state.bendBack = (float)f * 0.01f;
            pr.state.bits28 = f;
            pr.state.holdL = 0x00070003u;      // {slot 3, generation 7} -- the split/rejoin under test
            pr.state.holdR = 0u;
            Feed(b, kRecSampledPlayer, pr);
        }
        b.EmitMatchEnd(t0 + frames * dt);
        b.FlushChapter();
        b.Close();
        writer.Close();
    }

    DemoReader reader;
    REQUIRE(reader.Open(tmp.path));
    REQUIRE(reader.ChapterCount() > 0);

    const size_t ci = ChapterForTime(reader, (int64_t)(t0 + 30 * dt));
    Window w;
    BuildStats bs;
    const bool ok = BuildWindow(reader, ci, w, bs);
    // Diagnostics, printed only when something is wrong: a gate that dumps an inventory on every green
    // run trains its reader to skip the output, which is how the one failing line gets missed.
    if (!ok || bs.decode_failures || bs.body_samples == 0) {
        DecodedChapter dbg;
        if (reader.DecodeChapter(ci, dbg)) {
            printf("RT-STREAMS chapter=%zu streamCount=%u\n", ci, (unsigned)dbg.streamCount);
            for (uint16_t q = 0; q < dbg.streamCount; ++q) {
                const StreamView& v = dbg.streams[q];
                printf("  stream id=%u (%s) schema=%u records=%u bytes=%u obs=%u stored=%u\n",
                       (unsigned)v.id, StreamName(v.id), (unsigned)v.schema,
                       (unsigned)v.recordCount, (unsigned)v.bytes,
                       (unsigned)v.observedCount, (unsigned)v.storedCount);
            }
        }
    }
    if (!ok || bs.decode_failures || bs.body_samples == 0) {
        printf("RT-DIAG ok=%d refusal=%s chapters=%u ci=%zu chapterCount=%zu body=%u player=%u ticks=%u nvrows=%u seeds=%u fails=%u verbatim=%u keyframe=%d unknownblocks=%u\n",
               (int)ok, bs.refusal ? bs.refusal : "none", bs.chapters, ci, reader.ChapterCount(),
               bs.body_samples, bs.player_samples, bs.ticks, bs.netvar_rows, bs.netvar_seeds,
               bs.decode_failures, bs.verbatim_skipped, (int)bs.keyframe_present, bs.key_blocks_unknown);
        for (size_t q = 0; q < reader.ChapterCount(); ++q) {
            const ChapterKey k = reader.ChapterAt(q);
            printf("  chapter %zu section=%u seq=%u [%llu,%llu]\n", q, (unsigned)k.section,
                   (unsigned)k.seq, (unsigned long long)k.tBeginUs, (unsigned long long)k.tEndUs);
        }
    }
    REQUIRE(ok);

    CHECK(bs.decode_failures == 0);
    // Every block decoded was also bit-verified. The two numbers being equal and non-zero is what
    // says the check is ON; a build that decodes blocks and verifies none looks identical otherwise.
    CHECK(bs.blocks_bit_mismatch == 0);
    CHECK(bs.blocks_verified >= 4);   // S_TICK, S_PLAYER_SAMPLED, S_BODY_SAMPLED, and S_KEY's netvars
    CHECK(bs.body_samples > 0);
    CHECK(bs.player_samples > 0);
    CHECK(bs.ticks > 0);

    // Both bodies present, keyed on the u64 NET ID -- not on a slot index, which is the key that would
    // not round-trip.
    REQUIRE(w.BodyCount() == 2);
    bool saw_a = false, saw_b = false;
    for (size_t i = 0; i < w.BodyCount(); ++i) {
        if (w.BodyAt(i).id.value == kNetIdA) saw_a = true;
        if (w.BodyAt(i).id.value == kNetIdB) saw_b = true;
    }
    CHECK(saw_a);
    CHECK(saw_b);

    // The captured value comes back. com[0] was the frame index on body A, so sampling at frame 30's
    // stamp must return 30 -- through the bit-packed quantiser, zstd and the decoder.
    for (size_t i = 0; i < w.BodyCount(); ++i) {
        BodyTrack& tr = w.BodyAt(i);
        if (tr.id.value != kNetIdA) continue;
        BodyState s;
        memset(&s, 0, sizeof s);
        REQUIRE(SampleBodyAt(w.BodyFrames(tr), w.BodyTimes(tr), tr.count, tr.cursor,
                             (int64_t)(t0 + 30 * dt), s));
        CHECK(s.com[0] == doctest::Approx(30.0f).epsilon(0.01));
        CHECK(s.com[1] == doctest::Approx(2.0f).epsilon(0.01));
        CHECK(s.bits11 == 30u);   // held, not blended, and it survived the round trip
    }

    // The player's holdingontouserid halves are re-joined the way the capture split them. A wrong
    // reassembly here yields a grab id that is plausible and wrong, which nothing downstream reports.
    REQUIRE(w.PlayerCount() == 1);
    {
        PlayerTrack& tr = w.PlayerAt(0);
        CHECK(tr.user_slot == 0);
        // BuildWindow deliberately leaves this unresolved -- a capture slot is not a user id.
        CHECK_FALSE(tr.user_id_valid);
        PlayerState ps;
        memset(&ps, 0, sizeof ps);
        REQUIRE(SamplePlayerAt(w.PlayerFrames(tr), w.PlayerTimes(tr), tr.count, tr.cursor,
                               (int64_t)(t0 + 30 * dt), ps));
        CHECK(ps.holdL == 0x00070003u);
        CHECK(ps.bits28 == 30u);
    }

    // And the applier refuses to publish that player until W2-C resolves it, which is the whole point
    // of keeping the two index spaces apart.
    FakeEntity ent; FakePose pose; FakeNetVar nv; OwnerGuard og;
    og.Reset(3, 0xFFFFFFFFu);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    w.PlayerAt(0).handle = PlayerHandle(1);
    w.PlayerAt(0).handle_valid = true;
    StateApplier ap;
    ap.Bind(&ent, &pose, &nv, &og);
    const ApplyStats st = ap.Apply(w, (int64_t)(t0 + 30 * dt), ApplyOptions());
    CHECK(st.players_published == 0);
    CHECK(st.players_unmapped == 1);
    CHECK(st.players_refused_owner == 0);
}

TEST_CASE("interleaved tracks survive: the shape a real chapter actually has") {
    // The defect this catches shipped in the first Window: samples arrive A,B,A,B and the flat pool
    // could only extend the most recently touched track, so it kept the first of each and silently
    // refused the rest -- 61 of 120, with a true return from the build.
    Window w;
    const size_t n = 40;
    for (size_t i = 0; i < n; ++i) {
        BodyState a, b;
        MakeBody(a, (float)i, 0.0f);
        MakeBody(b, (float)i * -1.0f, 0.0f);
        REQUIRE(w.AddBodySample(NetId(0xAAAA), 1000 + i * 100, a));   // interleaved, not grouped  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
        REQUIRE(w.AddBodySample(NetId(0xBBBB), 1000 + i * 100, b));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    }
    w.Finalize();
    REQUIRE(w.BodyCount() == 2);
    CHECK(w.DroppedSamples() == 0);
    for (size_t k = 0; k < 2; ++k) {
        BodyTrack& tr = w.BodyAt(k);
        INFO("track ", k, " id ", tr.id.value);
        REQUIRE(tr.count == n);                       // every sample kept, not just the first
        const uint64_t* times = w.BodyTimes(tr);
        for (size_t i = 1; i < tr.count; ++i) {
            REQUIRE(times[i] >= times[i - 1]);        // and contiguous in time order
        }
        BodyState s;
        memset(&s, 0, sizeof s);
        REQUIRE(SampleBodyAt(w.BodyFrames(tr), times, tr.count, tr.cursor, 1000 + 25 * 100, s));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
        const float expect = tr.id.value == 0xAAAA ? 25.0f : -25.0f;
        CHECK(s.com[0] == doctest::Approx(expect));
    }
}

TEST_CASE("the block bit-length check refuses a short decode as well as a long one") {
    // The predicate that ships, tested directly, because its whole job is to distinguish three
    // outcomes that a decoder's own return value cannot: exact, short, and over-read.
    SUBCASE("exact is the only pass") {
        const BlockVerdict v = VerifyBlockConsumed(1234, 1234, false);
        CHECK(v.ok);
        CHECK(v.consumed_bits == 1234u);
    }
    SUBCASE("short: every record decoded, and the tail of each was never read") {
        // 50 bits short over a whole block is the shape this project already shipped once, with the
        // covering gate reporting "decode proved".
        const BlockVerdict v = VerifyBlockConsumed(1234, 1184, false);
        CHECK_FALSE(v.ok);
        CHECK(v.why[0] != '\0');
    }
    SUBCASE("over-read is a failure too, not a tolerance") {
        const BlockVerdict v = VerifyBlockConsumed(1234, 1240, false);
        CHECK_FALSE(v.ok);
    }
    SUBCASE("overrun is reported as itself, not as a length mismatch") {
        const BlockVerdict v = VerifyBlockConsumed(1234, 1234, true);
        CHECK_FALSE(v.ok);
        CHECK(v.overrun);
    }
    SUBCASE("an empty block is exact at zero") {
        CHECK(VerifyBlockConsumed(0, 0, false).ok);
    }
}

TEST_CASE("body handles bind through the durable net id, and an unknown id stays unpublished") {
    Window w;
    FillWindow(w);
    // The window's tracks are 0x1001 and 0x1002 (FillWindow). This session carries only the first,
    // on a DIFFERENT pool slot than the capture used -- which is the normal case, and the reason a
    // handle is not an identity.
    FakeEntity ent;
    ent.net_ids[7] = 0x1001;
    for (size_t i = 0; i < w.BodyCount(); ++i) {
        w.BodyAt(i).handle_valid = false;
    }
    CHECK(BindBodyHandles(ent, w).bound == 1);

    size_t bound = 0, unbound = 0;
    for (size_t i = 0; i < w.BodyCount(); ++i) {
        const BodyTrack& tr = w.BodyAt(i);
        if (tr.id.value == 0x1001) {
            CHECK(tr.handle_valid);
            CHECK(tr.handle.v == 7);       // the engine's slot, not the capture's
            ++bound;
        } else {
            CHECK_FALSE(tr.handle_valid);  // absent, not defaulted onto slot 0
            ++unbound;
        }
    }
    CHECK(bound == 1);
    CHECK(unbound == 1);

    FakePose pose; FakeNetVar nv; OwnerGuard og;
    og.Reset(3, 0xFFFFFFFFu); og.RegisterEntrant(0x00010004u);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    StateApplier ap;
    ap.Bind(&ent, &pose, &nv, &og);
    const ApplyStats st = ap.Apply(w, 1000000 + 10 * 11111, ApplyOptions());
    CHECK(st.bodies_installed == 1);
    CHECK(st.bodies_unhandled == 1);
    CHECK(ent.slots[7].installed);
    CHECK_FALSE(ent.slots[0].installed);   // nothing landed on the default slot
}

TEST_CASE("a body identity with no meaning is dropped, not keyed on") {
    // W2-A's rule: "idValue on its own is a number whose meaning was not recorded." A value with
    // neither the raw tag nor the net-actor guarantee cannot be keyed on, and cannot be handed to
    // PhysOfNetId -- a component id passed there returns 0xFFFF at best and collides at worst.
    Window w;
    BodyState s;
    MakeBody(s, 1.0f, 0.0f);

    BodyIdentity bare;                 // value only: fill says nothing about how to read it
    bare.value = 0x1234;
    bare.fill = replay::capture::kFillBodyIdValue;
    CHECK_FALSE(replay::capture::BodyIdentityResolvable(bare.fill));

    BodyIdentity tagged;               // the raw tag WAS read, and it says SR15NetUserId
    tagged.value = 0x1234;
    tagged.tag = 0;                    // tag 0 with kFillBodyIdTag SET means user id -- see below
    tagged.fill = replay::capture::kFillBodyIdValue | replay::capture::kFillBodyIdTag;
    CHECK(replay::capture::BodyIdentityResolvable(tagged.fill));
    // The same stored 0 means two different things depending on the fill bit, which is why the fill
    // bit is checked first: here it is a user id; in a file W2-A writes today it is a placeholder.
    CHECK(ClassifyBodyIdentity(tagged.fill, tagged.tag) == IdKind::UserIdByTag);

    // The same qword under two different tags is TWO tracks, not one. A window keyed on the bare
    // value would merge them, which is the defect the pair-key exists to prevent.
    REQUIRE(w.AddBodySample(NetId(0x1234), 1000, s));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    REQUIRE(w.AddBodySample(tagged, 1000, s));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    w.Finalize();
    CHECK(w.BodyCount() == 2);

    // Only the net-actor-guaranteed one is offered to PhysOfNetId. The user-id one is not -- even
    // though its value is the SAME qword and IS in the table, which is the collision the pair-key and
    // the classification together exist to prevent. It names a PLAYER, so it goes to the pawn
    // surfaces by slot instead, and nobody has assigned that recorded slot a live one.
    FakeEntity ent;
    ent.net_ids[4] = 0x1234;
    const BindResult r = BindBodyHandles(ent, w);
    CHECK(r.bound == 1);
    CHECK(r.not_a_net_id == 0);
    CHECK(r.slot_unassigned == 1);
    CHECK(r.no_player_ci == 0);
    CHECK(r.unresolvable == 0);
}

TEST_CASE("the stored identity is classified by the FILL BIT first, then the tag threshold") {
    using namespace replay::capture;
    const uint32_t val = kFillBodyIdValue;

    SUBCASE("what every file written today actually contains") {
        // W2-A writes idTag = 0 unconditionally and sets kFillBodyIdNetActor, never kFillBodyIdTag:
        // "writing a tag value nobody read would be inventing the one field this pair exists to
        // preserve." So a stored 0 means NOBODY READ A TAG -- it does not mean SR15NetUserId.
        //
        // A reader that applied the >= 2 threshold to the stored value would classify every sampled
        // body as a user id and resolve NONE of them: correct-looking code, no error, empty replay.
        CHECK(ClassifyBodyIdentity(val | kFillBodyIdNetActor, 0) == IdKind::NetActorGuaranteed);
        // ...and it stays a net id whatever placeholder happens to sit in the tag.
        CHECK(ClassifyBodyIdentity(val | kFillBodyIdNetActor, 1) == IdKind::NetActorGuaranteed);
        CHECK(ClassifyBodyIdentity(val | kFillBodyIdNetActor, 99) == IdKind::NetActorGuaranteed);
    }

    SUBCASE("when the raw tag WAS read, the wire threshold applies -- and it is a threshold") {
        CHECK(ClassifyBodyIdentity(val | kFillBodyIdTag, 0) == IdKind::UserIdByTag);
        CHECK(ClassifyBodyIdentity(val | kFillBodyIdTag, 1) == IdKind::ComponentIdByTag);
        CHECK(ClassifyBodyIdentity(val | kFillBodyIdTag, 2) == IdKind::NetIdByTag);
        // >= 2, not == 2. `jb` is unsigned-below, so everything at or above the bound is a net id;
        // an equality test here would drop every tag above 2 into the user-id arm.
        CHECK(ClassifyBodyIdentity(val | kFillBodyIdTag, 3) == IdKind::NetIdByTag);
        CHECK(ClassifyBodyIdentity(val | kFillBodyIdTag, 0xFFFFFFFFu) == IdKind::NetIdByTag);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    }

    SUBCASE("value alone is not an identity") {
        CHECK(ClassifyBodyIdentity(val, 5) == IdKind::Unresolvable);
        CHECK(ClassifyBodyIdentity(0, 5) == IdKind::Unresolvable);
        // A tag bit with no value is not one either: the rule needs both halves.
        CHECK(ClassifyBodyIdentity(kFillBodyIdTag, 2) == IdKind::Unresolvable);
    }

    SUBCASE("only the two net-id kinds reach PhysOfNetId") {
        Window w;
        BodyState s;
        MakeBody(s, 1.0f, 0.0f);
        BodyIdentity guaranteed;      // what a real capture stores
        guaranteed.value = 0x900;
        guaranteed.fill = val | kFillBodyIdNetActor;
        BodyIdentity user;            // tag read, and it says user id
        user.value = 0x901;
        user.tag = 0;
        user.fill = val | kFillBodyIdTag;
        BodyIdentity bytag;           // tag read, and it says net id
        bytag.value = 0x902;
        bytag.tag = 7;
        bytag.fill = val | kFillBodyIdTag;
        REQUIRE(w.AddBodySample(guaranteed, 1000, s));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
        REQUIRE(w.AddBodySample(user, 1000, s));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
        REQUIRE(w.AddBodySample(bytag, 1000, s));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
        w.Finalize();

        FakeEntity ent;
        ent.net_ids[3] = 0x900;
        ent.net_ids[4] = 0x902;
        ent.net_ids[5] = 0x901;   // present in the table, but this identity is NOT a net id
        const BindResult r = BindBodyHandles(ent, w);
        CHECK(r.bound == 2);
        CHECK(r.not_a_net_id == 0);
        CHECK(r.slot_unassigned == 1);   // the user id names a player's slot, and none is assigned
        CHECK(r.unresolvable == 0);
        // The user-id track is not bound even though its value happens to match a table entry --
        // which is the collision the classification exists to prevent.
        for (size_t i = 0; i < w.BodyCount(); ++i) {
            if (w.BodyAt(i).id.value == 0x901) {
                CHECK_FALSE(w.BodyAt(i).handle_valid);
            }
        }
    }
}

TEST_CASE("B1a: an Add after Finalize is refused, not staged where nothing will regroup it") {
    // The audit's exact scenario: one Add after Finalize left a track reading across into its
    // neighbour -- track A's newest com[0] came back as track B's first frame. A precondition that
    // nothing enforces is a comment, so this one is enforced.
    Window w;
    BodyState a, b;
    MakeBody(a, 1.0f, 0.0f);
    MakeBody(b, 1000.0f, 0.0f);
    REQUIRE(w.AddBodySample(NetId(0xAAAA), 1000, a));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    REQUIRE(w.AddBodySample(NetId(0xBBBB), 1000, b));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    w.Finalize();
    REQUIRE(w.BodyCount() == 2);
    CHECK(w.PostFinalizeAdds() == 0);

    // The add the audit made. It must FAIL.
    CHECK_FALSE(w.AddBodySample(NetId(0xAAAA), 2000, a));
    CHECK(w.PostFinalizeAdds() == 1);
    PlayerState ps;
    memset(&ps, 0, sizeof ps);
    CHECK_FALSE(w.AddPlayerSample(0, 2000, ps));
    CHECK(w.PostFinalizeAdds() == 2);

    // And the tracks are untouched: track A still ends on its own sample, not its neighbour's.
    for (size_t k = 0; k < w.BodyCount(); ++k) {
        BodyTrack& tr = w.BodyAt(k);
        REQUIRE(tr.count == 1);
        const BodyState* frames = w.BodyFrames(tr);
        const float expect = tr.id.value == 0xAAAA ? 1.0f : 1000.0f;
        CHECK(frames[tr.count - 1].com[0] == doctest::Approx(expect));
    }
}

TEST_CASE("B2: netvars report what LANDED, and this build lands nothing") {
    // INetVar::Write returns whether the write LANDED -- W2-A added that at this packet's request,
    // because a void return makes success unobservable through the interface and a caller can only
    // count attempts. That is exactly how the one netvar observable came to report success for a
    // no-op: score, clock and round state did not replay and nothing said so.
    Window w;
    FillWindow(w);
    KeyedRecord seeds[2] = {{0, 0xAA, 100}, {0, 0xBB, 200}};
    KeyedRecord recs[1] = {{1200000, 0xAA, 101}};
    REQUIRE(w.NetVars().Build(seeds, 2, recs, 1));
    w.NoteNetVarSlot(0xAA, 0);
    w.NoteNetVarSlot(0xBB, 0);   // the slot the shipped builder always records

    FakeEntity ent; FakePose pose; OwnerGuard og;
    og.Reset(3, 0xFFFFFFFFu); og.RegisterEntrant(0x00010004u);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop

    SUBCASE("refusing writes: attempted non-zero, applied ZERO, and the label says so") {
        FakeNetVar nv;
        nv.accept = false;              // what the shipping INetVar does on this build
        StateApplier ap;
        ap.Bind(&ent, &pose, &nv, &og);
        const ApplyStats st = ap.Apply(w, 1300000, ApplyOptions());
        CHECK(st.netvars_attempted == 2);
        CHECK(st.netvars_applied == 0);          // the honest number
        CHECK(st.netvars_refused == 2);
        CHECK_FALSE(st.netvar_apply_available);
        CHECK(strstr(st.NetVarFidelity(), "NOT APPLIED") != nullptr);

        // â˜… And the shadow did NOT advance, so the next frame tries again rather than skipping a
        // write that never landed. A shadow keyed on intent survives the fix meant to end it.
        const ApplyStats st2 = ap.Apply(w, 1300000, ApplyOptions());
        CHECK(st2.netvars_attempted == 2);
        CHECK(st2.netvars_unchanged == 0);
    }

    SUBCASE("accepting writes: the shadow advances and the next frame is quiet") {
        FakeNetVar nv;
        StateApplier ap;
        ap.Bind(&ent, &pose, &nv, &og);
        const ApplyStats st = ap.Apply(w, 1300000, ApplyOptions());
        CHECK(st.netvars_attempted == 2);
        CHECK(st.netvars_applied == 2);
        CHECK(st.netvars_refused == 0);
        CHECK(st.netvar_apply_available);
        const ApplyStats st2 = ap.Apply(w, 1300000, ApplyOptions());
        CHECK(st2.netvars_attempted == 0);
        CHECK(st2.netvars_unchanged == 2);
    }

    SUBCASE("a partial refusal advances only the shadows that landed") {
        // Per-write granularity is what the bool buys over the batch flag it replaced.
        FakeNetVar nv;
        nv.accept = true;
        StateApplier ap;
        ap.Bind(&ent, &pose, &nv, &og);
        ap.Apply(w, 1300000, ApplyOptions());
        nv.accept = false;
        const ApplyStats st = ap.Apply(w, 1500000, ApplyOptions());
        CHECK(st.netvars_refused == st.netvars_attempted);
        CHECK(st.netvars_applied == 0);
    }

    SUBCASE("the fidelity label never claims applied when nothing landed") {
        // Driven by the three-state now. The bool it used to set could not express NotAttempted, so
        // this case asserted "NOT APPLIED" for a frame that had asked nothing -- the same false claim
        // the state exists to prevent.
        ApplyStats a;
        a.netvar_state = NetVarFidelityState::Refused;
        CHECK(strstr(a.NetVarFidelity(), "NOT APPLIED") != nullptr);
        a.netvar_state = NetVarFidelityState::Applied;
        CHECK(strstr(a.NetVarFidelity(), "NOT APPLIED") == nullptr);
        a.netvar_state = NetVarFidelityState::PresentNotApplied;
        CHECK(strstr(a.NetVarFidelity(), "NOT APPLIED") == nullptr);
        a.netvar_state = NetVarFidelityState::NoRecords;
        CHECK(strstr(a.NetVarFidelity(), "NOT APPLIED") == nullptr);

        // â˜… Every state renders a sentence of its OWN. The panel line and the probe row read
        // nothing else, so two states sharing a string is the split not existing. The states are
        // static literals, so holding the pointer across the next assignment is safe.
        const NetVarFidelityState all[4] = {
            NetVarFidelityState::NoRecords, NetVarFidelityState::PresentNotApplied,
            NetVarFidelityState::Applied, NetVarFidelityState::Refused};
        for (int i = 0; i < 4; ++i) {
            for (int j = i + 1; j < 4; ++j) {
                a.netvar_state = all[i];
                const char* const si = a.NetVarFidelity();
                a.netvar_state = all[j];
                CHECK(strcmp(si, a.NetVarFidelity()) != 0);
            }
        }
    }

    // â˜…â˜… THE SPLIT. `netvars_attempted == 0` had one sentence for two different facts: a capture
    // that carries no netvar records -- which a solo -offline host produces by construction, so a
    // fidelity row must PASS it -- and a capture that carries them where none was due here.
    SUBCASE("a capture with no netvar records is NoRecords, not a failed apply") {
        Window empty;
        FillWindow(empty);
        REQUIRE(empty.NetVars().KeyCount() == 0);
        FakeNetVar nv;
        StateApplier ap;
        ap.Bind(&ent, &pose, &nv, &og);
        const ApplyStats st = ap.Apply(empty, 1300000, ApplyOptions());
        CHECK(st.netvars_present == 0);
        CHECK(st.netvars_attempted == 0);
        CHECK(st.netvar_state == NetVarFidelityState::NoRecords);
        CHECK(strstr(st.NetVarFidelity(), "no netvar records") != nullptr);
    }

    SUBCASE("records present with none due at this playhead is a DIFFERENT state") {
        FakeNetVar nv;
        StateApplier ap;
        ap.Bind(&ent, &pose, &nv, &og);
        ap.Apply(w, 1300000, ApplyOptions());        // lands both writes; the shadows advance
        const ApplyStats st = ap.Apply(w, 1300000, ApplyOptions());
        CHECK(st.netvars_attempted == 0);            // identical to the case above ...
        CHECK(st.netvars_present == (uint32_t)w.NetVars().KeyCount());
        CHECK(st.netvars_present > 0);               // ... and told apart only by this
        CHECK(st.netvar_state == NetVarFidelityState::PresentNotApplied);
        CHECK(strstr(st.NetVarFidelity(), "no netvar records") == nullptr);
    }

    SUBCASE("over a span, a quiet frame cannot pull the report back to NoRecords") {
        ApplyStats span;
        REQUIRE(span.netvar_state == NetVarFidelityState::NoRecords);
        ApplyStats had;
        had.netvar_state = NetVarFidelityState::Applied;
        had.netvars_present = 2;
        span.Add(had);
        CHECK(span.netvar_state == NetVarFidelityState::Applied);
        ApplyStats quiet;                            // NoRecords, zero keys
        span.Add(quiet);
        CHECK(span.netvar_state == NetVarFidelityState::Applied);
        CHECK(span.netvars_present == 2);            // the largest key set seen, not a total
        ApplyStats refused;
        refused.netvar_state = NetVarFidelityState::Refused;
        span.Add(refused);
        CHECK(span.netvar_state == NetVarFidelityState::Refused);
        ApplyStats applied2;
        applied2.netvar_state = NetVarFidelityState::Applied;
        span.Add(applied2);
        CHECK(span.netvar_state == NetVarFidelityState::Refused);
        ApplyStats present;
        present.netvar_state = NetVarFidelityState::PresentNotApplied;
        span.Add(present);
        CHECK(span.netvar_state == NetVarFidelityState::Refused);
    }
}

namespace {

// Writes a demo long enough to span several CORE chapters. Returns the frame interval so a test can
// address any frame by index without restating the arithmetic.
uint64_t WriteMultiChapterDemo(const wchar_t* path, uint64_t t0, uint32_t frames, uint64_t netIdA) {
    using namespace replay::capture;
    using namespace replay;
    const uint64_t dt = 11111;

    ManifestBuilder mb;
    mb.Begin(t0, 0, 0x40);
    const uint8_t md5[16] = {0xca, 0xa0, 0x5e, 0xe3};
    mb.SetBuild(md5, 0x676F6C646D617374ull, 0x0102030405060708ull);
    mb.SetLevel(6300205991959903307ull, 0xF00Dull, 1780177787ull);
    mb.SetKeyframeIntervalUs(2000000);
    mb.SetUuid(3, 4);
    PlayerRecord p{};
    p.xplatformId = 0x2000;  // facts-ok: PHYS.CI_PAUSED_BIT a test fixture value this case compares against, not an engine position
    mb.AddPlayer(p);

    DemoWriter writer;
    REQUIRE(writer.Open(path, mb.View()));
    BuilderConfig cfg;
    cfg.keyframeIntervalUs = 2000000;
    for (uint16_t i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;
    for (uint16_t i = 0; i < kSectionCount; ++i) cfg.sectionEnabled[i] = true;

    ChapterBuilder b;
    REQUIRE(b.Open(&writer, mb.View(), cfg));
    b.EmitMatchBegin(t0);
    for (uint32_t f = 0; f < frames; ++f) {
        const uint64_t st = t0 + f * dt;
        TickRec tr{};
        tr.sourceTimeUs = st;
        tr.simTimeUs = 1000000 + f * dt;
        tr.frameIndex = f + 1;
        tr.frameDeltaUs = (uint32_t)dt;
        tr.scaledDeltaUs = (uint32_t)dt;
        Feed(b, kRecTick, tr);

        SampledBodyRec br{};
        br.sourceTimeUs = st;
        br.frameIndex = f + 1;
        br.idValue = netIdA;
        br.idTag = 0;
        br.fill = kFillBodyTransform | kFillBodyLastHeld | kFillBodyBits | kFillBodyIdValue |
                  kFillBodyIdNetActor;
        br.handle = 0;
        // com[0] IS the frame index, so any sample can be checked against where it came from.
        br.state.com[0] = (float)f;
        br.state.ori[3] = 1.0f;
        Feed(b, kRecSampledBody, br);
    }
    b.EmitMatchEnd(t0 + frames * dt);
    b.FlushChapter();
    b.Close();
    writer.Close();
    return dt;
}

size_t CountCoreChapters(replay::DemoReader& r) {
    size_t n = 0;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        if (r.ChapterAt(i).section == replay::kSectionCore) ++n;
    }
    return n;
}

}  // namespace

TEST_CASE("the resident window spans three chapters, and one chapter is not enough") {
    TempDemo tmp;
    const uint64_t t0 = 5000000;
    const uint64_t kNetId = 0x00000001ABCDEF01ull;
    const uint32_t frames = 600;                       // ~6.7 s -> several 2 s chapters
    const uint64_t dt = WriteMultiChapterDemo(tmp.path, t0, frames, kNetId);

    replay::DemoReader reader;
    REQUIRE(reader.Open(tmp.path));
    const size_t cores = CountCoreChapters(reader);
    INFO("core chapters: ", cores);
    REQUIRE(cores >= 3);

    // A time comfortably inside the middle of the file.
    const int64_t mid = (int64_t)(t0 + (frames / 2) * dt);
    const size_t centre = ChapterForTime(reader, mid);

    Window w;
    BuildStats bs;
    const bool ok = BuildWindowRange(reader, centre, w, bs);
    INFO("refusal: ", bs.refusal ? bs.refusal : "none");
    REQUIRE(ok);

    CHECK(bs.chapters == 3);
    CHECK(bs.first_chapter < bs.center_chapter);
    CHECK(bs.center_chapter < bs.last_chapter);
    CHECK(bs.decode_failures == 0);
    CHECK(bs.blocks_bit_mismatch == 0);

    // â˜… The span really covers three chapters' worth of samples, not one.
    const replay::ChapterKey kf = reader.ChapterAt(bs.first_chapter);
    const replay::ChapterKey kl = reader.ChapterAt(bs.last_chapter);
    CHECK(w.OuterBeginUs() <= kf.tBeginUs);
    CHECK(w.OuterEndUs() >= kl.tEndUs);

    REQUIRE(w.BodyCount() == 1);
    BodyTrack& tr = w.BodyAt(0);
    CHECK(tr.id.value == kNetId);

    // â˜… THE POINT OF THE RANGE. A sample near the START of the first chapter and one near the END of
    // the last both answer from ONE window -- which a current-only window cannot do, and which is what
    // playback crossing a boundary and reverse both need.
    BodyState s;
    memset(&s, 0, sizeof s);
    const int64_t early = (int64_t)(kf.tBeginUs + 2 * dt);
    const int64_t late = (int64_t)(kl.tEndUs - 2 * dt);
    REQUIRE(w.CoversForSampling(early));
    REQUIRE(w.CoversForSampling(late));
    REQUIRE(SampleBodyAt(w.BodyFrames(tr), w.BodyTimes(tr), tr.count, tr.cursor, early, s));
    const float early_com = s.com[0];
    REQUIRE(SampleBodyAt(w.BodyFrames(tr), w.BodyTimes(tr), tr.count, tr.cursor, late, s));
    const float late_com = s.com[0];
    INFO("early ", early_com, " late ", late_com);
    // com[0] is the frame index, so the two must differ by roughly the frames between them -- if the
    // window held only the centre chapter, one of them would clamp and they would be much closer.
    CHECK(late_com > early_com + 100.0f);

    // And each answers with the frame that was actually written at that time.
    const float expect_early = (float)((early - (int64_t)t0) / (int64_t)dt);
    CHECK(early_com == doctest::Approx(expect_early).epsilon(0.02));

    SUBCASE("the rebuild trigger is the CENTRE chapter, not the outer span") {
        const replay::ChapterKey kc = reader.ChapterAt(bs.center_chapter);
        CHECK_FALSE(w.NeedsRebuild((int64_t)(kc.tBeginUs + 1000)));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
        CHECK_FALSE(w.NeedsRebuild((int64_t)(kc.tEndUs - 1000)));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
        // Leaving the centre asks for a rebuild WHILE the neighbours still cover sampling -- which is
        // the whole reason the inner span is narrower than the outer one.
        CHECK(w.NeedsRebuild((int64_t)(kc.tBeginUs - 1000)));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
        CHECK(w.NeedsRebuild((int64_t)(kc.tEndUs + 1000)));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
        CHECK(w.CoversForSampling((int64_t)(kc.tBeginUs - 1000)));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
        CHECK(w.CoversForSampling((int64_t)(kc.tEndUs + 1000)));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    }

    SUBCASE("an unset coverage answers NO rather than inheriting a default yes") {
        Window fresh;
        CHECK_FALSE(fresh.CoversForSampling(0));
        CHECK_FALSE(fresh.CoversForSampling(mid));
        CHECK(fresh.NeedsRebuild(mid));
    }
}

TEST_CASE("at the ends of the file the neighbour IS the centre, and the chapter is decoded once") {
    TempDemo tmp;
    const uint64_t t0 = 5000000;
    const uint64_t kNetId = 0x00000001ABCDEF02ull;
    const uint64_t dt = WriteMultiChapterDemo(tmp.path, t0, 600, kNetId);
    (void)dt;

    replay::DemoReader reader;
    REQUIRE(reader.Open(tmp.path));

    size_t firstCore = 0, lastCore = 0;
    bool seen = false;
    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        if (reader.ChapterAt(i).section != replay::kSectionCore) continue;
        if (!seen) { firstCore = i; seen = true; }
        lastCore = i;
    }
    REQUIRE(seen);

    SUBCASE("first chapter: prev resolves to itself and it is not decoded twice") {
        Window w;
        BuildStats bs;
        REQUIRE(BuildWindowRange(reader, firstCore, w, bs));
        CHECK(bs.first_chapter == firstCore);
        CHECK(bs.center_chapter == firstCore);
        // â˜… Two chapters, not three, and critically NOT the same chapter decoded twice -- which would
        // double every sample and put two entries at each timestamp in one track.
        CHECK(bs.chapters == 2);
        REQUIRE(w.BodyCount() == 1);
        const BodyTrack& tr = w.BodyAt(0);
        const uint64_t* times = w.BodyTimes(tr);
        for (size_t i = 1; i < tr.count; ++i) {
            REQUIRE(times[i] > times[i - 1]);   // strictly increasing: no duplicated chapter
        }
    }

    SUBCASE("last chapter: next resolves to itself") {
        Window w;
        BuildStats bs;
        REQUIRE(BuildWindowRange(reader, lastCore, w, bs));
        CHECK(bs.center_chapter == lastCore);
        CHECK(bs.last_chapter == lastCore);
        CHECK(bs.chapters == 2);
    }

    SUBCASE("neighbours are found by walking sections, not by index arithmetic") {
        // MANIFEST and INDEX chapters sit among the CORE ones, so prev/next are not i-1 / i+1.
        const size_t p = PrevCoreChapter(reader, lastCore);
        const size_t n = NextCoreChapter(reader, firstCore);
        CHECK(reader.ChapterAt(p).section == replay::kSectionCore);
        CHECK(reader.ChapterAt(n).section == replay::kSectionCore);
        CHECK(p < lastCore);
        CHECK(n > firstCore);
    }
}

// =============================================================================================
// THE CHAIN: synthetic engine state -> W2-A's real capture -> a real .echodemo -> W2-A's real
// decoders -> Window + ApplyStateAt -> a fake engine that records what was installed.
// =============================================================================================
//
// â˜…â˜…â˜… THE OWNER'S REQUIREMENT STATED AS A CHECK. "Capture basically everything needed to have a 1 to 1
// playback" is a property of the WHOLE chain, and nothing tested it: each half is verified against its
// own fixtures, so a field that capture never sources and playback never installs is equal to itself
// at both ends and absent from the product.
//
// â˜…â˜… WHAT THIS COVERS, AND THE ONE SEAM IT DOES NOT. It starts at the ring record -- the POD the
// capture tick memcpys after reading the engine -- and runs everything downstream for real:
// ChapterBuilder, the delta coder, the bit packer, zstd, the container, then DemoReader, W2-A's own
// decoders, the Window, and ApplyStateAt.
//
// It does NOT cover the GATHER: the step that walks the component pools, calls IEntity::ReadBody, and
// decides the fill bits. GatherEntitiesT is a template in capture.cpp with no header, so it is not
// reachable from another translation unit. That step is where the fill-bit decisions live, and the
// fill bits are precisely what this test has to mirror by hand -- which means a gather that set them
// differently would not be caught here. Requested from W2-A: move the Gather*T templates to a header.
// Until then this file states the seam rather than implying the chain is closed.
//
// â˜… THE FILL MASKS BELOW ARE COPIED FROM capture.cpp's GATHER, not invented, and the divergences are
// the point of section 3.

namespace {

// The ground truth. Deterministic and varied: bodies that move and rotate, a player whose pose and
// action word both change, so a field that is dropped anywhere in the chain shows up as a mismatch
// rather than as two zeroes agreeing.
struct ScriptedFrame {
    uint64_t t_us;
    BodyState body[2];
    PlayerState player;
};

const uint64_t kChainT0 = 7000000;
const uint64_t kChainDt = 11111;
const uint64_t kChainNetId[2] = {0x00000001F00D0001ull, 0x00000001F00D0002ull};

void ScriptFrame(uint32_t f, ScriptedFrame& out) {
    out.t_us = kChainT0 + (uint64_t)f * kChainDt;
    for (int e = 0; e < 2; ++e) {
        BodyState& b = out.body[e];
        memset(&b, 0, sizeof b);
        const float s = (e ? -1.0f : 1.0f);
        // Values chosen to land off the quantiser grid, so a dropped requantisation shows.
        b.com[0] = s * (1.234f + (float)f * 0.05f);
        b.com[1] = s * (2.345f - (float)f * 0.03f);
        b.com[2] = s * (0.5f + (float)f * 0.011f);
        const float a = (float)f * 0.037f * s;
        b.ori[0] = 0.0f;
        b.ori[1] = sinf(a * 0.5f);
        b.ori[2] = 0.0f;
        b.ori[3] = cosf(a * 0.5f);
        b.vel[0] = s * (3.0f + (float)f * 0.25f);
        b.vel[1] = s * -1.5f;
        b.vel[2] = 0.125f;
        b.angVel[0] = s * 2.0f;
        b.angVel[1] = s * (float)(f % 7);
        b.angVel[2] = -3.0f;
        b.lastHeldCom[0] = s * 9.0f;
        b.lastHeldCom[1] = 8.0f;
        b.lastHeldCom[2] = s * 7.0f;
        b.bits11 = (f * 3 + (uint32_t)e) & 0x7FF;
    }
    PlayerState& p = out.player;
    memset(&p, 0, sizeof p);
    const float pa = (float)f * 0.021f;
    p.hmdOri[1] = sinf(pa * 0.5f);
    p.hmdOri[3] = cosf(pa * 0.5f);
    p.lHandOri[3] = 1.0f;
    p.rHandOri[3] = 1.0f;
    p.lHandOff[0] = 0.25f + (float)f * 0.004f;
    p.rHandOff[1] = -0.25f - (float)f * 0.003f;
    p.refVel[0] = 1.0f + (float)f * 0.02f;
    p.bendBack = 0.3f + (float)f * 0.001f;
    p.bendFwd = 0.1f;
    p.restPoseHeight = 1.2f;
    // â˜… These two CHANGE every frame on purpose. The gather does not source them (section 3), so the
    // chain must report them absent -- and a chain that quietly compared them would compare two zeroes
    // and agree.
    p.bits28 = 0x0100 | (f & 0xFF);
    p.holdL = (f / 8) | 0x00030000u;
}

// The quantiser steps S5.4.5 declares for these fields. Cited, not invented; a tighter check would
// read them out of the manifest's schema, which is worth doing when the schema is exposed.
const float kStepCom = 0.001f;      // WIRE_Q(+-250 m, 1 mm)
const float kStepVel = 0.03125f;    // WIRE_Q(+-48 m/s, 0.03125)
const float kStepAngVel = 1.0f;     // WIRE_Q(+-144 rad/s, 1.0)

// â˜…â˜… CODE EQUALITY, NOT A TOLERANCE. S5.3's lossless rule and SC2a's threshold are both stated as
// EXACT equality of the quantiser code, because at the wire level "lossless" and "bit-exact" coincide.
// Comparing floats with an epsilon would be a strict-equality test wearing a number, and it would pass
// for a value that landed one code away.
int64_t QCode(float v, float step) {
    return (int64_t)llround((double)v / (double)step);
}

// The CAPTURE-side engine: what the recording client's IEntity would answer.
//
// """ + star + star + """ NOTE THE POLARITY, because it is the opposite of the playback side and both are right.
// GatherBodies enumerates the bodies that ARE locally controlled -- the recording client authors them.
// ApplyStateAt installs into bodies that are NOT -- the replay client must not have physics overwrite
// what it publishes. Same predicate, opposite sense, because they are different sessions. A fake that
// served both would have to lie to one of them.
struct CaptureEngine : replay::IEntity {
    const ScriptedFrame* frame = nullptr;

    size_t PhysCount() const override { return 2; }
    bool IsLocallyControlled(PhysHandle h) const override { return h.v < 2; }
    void SetSyntheticOwner(PhysHandle) override {}
    void ClearSyntheticOwner(PhysHandle) override {}
    bool ReadBody(PhysHandle h, BodyState& out) const override {
        if (!frame || h.v >= 2) return false;
        out = frame->body[h.v];
        return true;
    }
    void PushHistory(PhysHandle, const BodyState&, uint64_t) override {}
    void TrimHistory(PhysHandle, uint64_t, uint64_t) override {}
    bool SetHistoryWindow(PhysHandle, const BodyState*, const uint64_t*, size_t) override { return false; }
    size_t ReadHistoryWindow(PhysHandle, BodyState*, uint64_t*, size_t) const override { return 0; }
    float GetTimeOffset(PhysHandle) const override { return 0.0f; }
    void SetTimeOffset(PhysHandle, float) override {}
    size_t PlayerCount() const override { return 1; }
    bool ReadPlayer(PlayerHandle, PlayerState&) const override { return false; }
    void PushPlayerHistory(PlayerHandle, const PlayerState&, uint64_t) override {}
    PhysHandle PhysOf(PlayerHandle h) const override { return PhysHandle(h.v); }
    PlayerHandle PlayerOf(UserIndex u) const override { return PlayerHandle(u.v); }
    uint64_t NetIdOf(PhysHandle h) const override { return h.v < 2 ? kChainNetId[h.v] : 0; }
    PhysHandle PhysOfNetId(uint64_t id) const override {
        for (uint16_t i = 0; i < 2; ++i) {
            if (kChainNetId[i] == id) return PhysHandle(i);
        }
        return PhysHandle(0xFFFF);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    }
    size_t LocalPlayerCount() const override { return 1; }
    UserIndex LocalPlayerAt(size_t) const override { return UserIndex(0); }
    // """ + star + star + star + """ THE GAP, PRODUCED BY THE ENGINE RATHER THAN ASSERTED BY THE TEST. This returns the mask
    // IEntity can actually source. There is no kLocal bit for the action word or the hold state, so a
    // fake CANNOT claim them however much it wants to -- the absence is a property of the contract,
    // and LocalMaskToFill turns it into the file's fill. The scripted bits28/holdL below are written
    // into `out` deliberately: a chain that carried them anyway would show up as the file CLAIMING a
    // field nothing sourced.
    uint32_t ReadLocalPlayer(UserIndex, PlayerState& out) const override {
        if (!frame) return 0;
        out = frame->player;
        return kLocalPose | kLocalRefVel | kLocalBody | kLocalTrigger | kLocalAnchor;
    }
    void SetLodThrottle(bool) override {}
    bool Available() const override { return true; }
    const char* Diagnostic() const override { return "capture-side fake"; }
};

// The gather's sink, framed into ring records exactly as the capture tick does, and handed straight to
// the real ChapterBuilder.
bool ChainSink(uint16_t kind, const void* head, uint32_t headBytes, const void* tail,
               uint32_t tailBytes, void* ctx) {
    auto* b = static_cast<replay::capture::ChapterBuilder*>(ctx);
    static uint8_t buf[4096];
    const uint32_t payload = headBytes + tailBytes;
    if (sizeof(replay::capture::RecHeader) + payload > sizeof buf) return false;
    replay::capture::RecHeader h;
    h.kind = kind;
    h.bytes = (uint16_t)payload;
    h.producer = 0;
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, head, headBytes);
    if (tailBytes) memcpy(buf + sizeof h + headBytes, tail, tailBytes);
    b->Consume(buf, (uint32_t)(sizeof h + payload));
    return true;
}

// Writes the scripted match through the REAL gather and the REAL capture path.
replay::capture::GatherReport WriteChainDemo(const wchar_t* path, uint32_t frames,
                                             ScriptedFrame* script) {
    using namespace replay::capture;
    using namespace replay;
    ManifestBuilder mb;
    mb.Begin(kChainT0, 0, 0x40);
    const uint8_t md5[16] = {0xca, 0xa0, 0x5e, 0xe3};
    mb.SetBuild(md5, 0x676F6C646D617374ull, 0x0102030405060708ull);
    mb.SetLevel(6300205991959903307ull, 0xF00Dull, 1780177787ull);
    mb.SetKeyframeIntervalUs(2000000);
    mb.SetUuid(9, 10);
    PlayerRecord p{};
    p.xplatformId = 0x3000;
    mb.AddPlayer(p);
    REQUIRE(mb.Refusal() == nullptr);

    DemoWriter writer;
    REQUIRE(writer.Open(path, mb.View()));
    BuilderConfig cfg;
    cfg.keyframeIntervalUs = 2000000;
    for (uint16_t i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;
    for (uint16_t i = 0; i < kSectionCount; ++i) cfg.sectionEnabled[i] = true;
    ChapterBuilder b;
    REQUIRE(b.Open(&writer, mb.View(), cfg));
    b.EmitMatchBegin(kChainT0);

    GatherConfig gcfg{};
    for (uint16_t i = 0; i < 32; ++i) gcfg.streamEnabled[i] = true;
    GatherReport rep;
    CaptureEngine eng;

    for (uint32_t f = 0; f < frames; ++f) {
        ScriptFrame(f, script[f]);
        eng.frame = &script[f];
        const uint64_t t = script[f].t_us;
        // """ + star + """ THE REAL GATHER. The fill masks this test asserts on are now the ones the SHIPPING gather
        // sets, not a copy mirrored by hand -- which was the one seam the chain did not close.
        GatherBodies(&eng, gcfg, t, f + 1, ChainSink, &b, rep);
        GatherLocalPawns(&eng, gcfg, t, f + 1, ChainSink, &b, rep);

        TickRec tr{};
        tr.sourceTimeUs = t;
        tr.simTimeUs = 1000000 + (uint64_t)f * kChainDt;
        tr.frameIndex = f + 1;
        tr.frameDeltaUs = (uint32_t)kChainDt;
        tr.scaledDeltaUs = (uint32_t)kChainDt;
        Feed(b, kRecTick, tr);
    }
    b.EmitMatchEnd(kChainT0 + (uint64_t)frames * kChainDt);
    b.FlushChapter();
    b.Close();
    writer.Close();
    return rep;
}

}  // namespace

TEST_CASE("CHAIN: state handed to the capture is the state installed in the engine") {
    TempDemo tmp;
    static const uint32_t kFrames = 500;
    static ScriptedFrame script[kFrames];
    const replay::capture::GatherReport rep = WriteChainDemo(tmp.path, kFrames, script);

    // The gather saw what it was given, so a later assertion about absence is about the
    // ENGINE not sourcing a field rather than about the fake never being asked.
    CHECK(rep.bodiesPresent == 2);
    CHECK(rep.bodiesSampled > 0);
    CHECK(rep.pawnsSampled > 0);
    CHECK_FALSE(rep.sawBodyWithoutNetId);
    CHECK(rep.sinkRefusals == 0);

    replay::DemoReader reader;
    REQUIRE(reader.Open(tmp.path));

    // The engine this replay lands in: two bodies, on pool slots that deliberately differ from the
    // capture's handles, so anything that carried a handle through the file instead of a net id binds
    // the wrong body.
    FakeEntity ent;
    ent.net_ids[11] = kChainNetId[0];
    ent.net_ids[4] = kChainNetId[1];
    FakePose pose;
    FakeNetVar nv;
    nv.accept = false;              // what the shipping INetVar does: no apply path on this build
    OwnerGuard og;
    og.Reset(3, 0xFFFFFFFFu);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop

    StateApplier ap;
    ap.Bind(&ent, &pose, &nv, &og);

    // Walk the middle of the match, rebuilding the window as the playhead leaves the centre chapter.
    Window w;
    BuildStats bs;
    size_t built_centre = (size_t)-1;
    uint32_t compared = 0, rebuilds = 0;

    for (uint32_t f = 5; f < kFrames - 5; f += 7) {
        const int64_t t = (int64_t)script[f].t_us;
        if (built_centre == (size_t)-1 || w.NeedsRebuild(t)) {
            const size_t centre = ChapterForTime(reader, t);
            REQUIRE(BuildWindowRange(reader, centre, w, bs));
            INFO("refusal: ", bs.refusal ? bs.refusal : "none");
            REQUIRE(bs.decode_failures == 0);
            REQUIRE(bs.blocks_bit_mismatch == 0);
            BindBodyHandles(ent, w);
            built_centre = centre;
            ++rebuilds;
        }
        REQUIRE(w.CoversForSampling(t));

        const ApplyStats st = ap.Apply(w, t, ApplyOptions());
        REQUIRE(st.bodies_installed == 2);

        // ---- 1. the bodies came back, on the RIGHT engine slots, with the RIGHT values ----------
        for (int e = 0; e < 2; ++e) {
            const uint16_t slot = e == 0 ? 11 : 4;
            const FakeEntity::Slot& got = ent.slots[slot];
            REQUIRE(got.installed);
            REQUIRE(got.depth == 1);
            const BodyState& want = script[f].body[e];
            INFO("frame ", f, " body ", e);
            for (int k = 0; k < 3; ++k) {
                REQUIRE(QCode(got.s.com[k], kStepCom) == QCode(want.com[k], kStepCom));
                REQUIRE(QCode(got.s.vel[k], kStepVel) == QCode(want.vel[k], kStepVel));
                REQUIRE(QCode(got.s.angVel[k], kStepAngVel) == QCode(want.angVel[k], kStepAngVel));
                REQUIRE(QCode(got.s.lastHeldCom[k], kStepCom) == QCode(want.lastHeldCom[k], kStepCom));
            }
            // The orientation is a smallest-three quat10; compare as a rotation, not component-wise,
            // because q and -q are the same rotation and the codec may return either.
            const double dot = (double)got.s.ori[0] * want.ori[0] + (double)got.s.ori[1] * want.ori[1] +
                               (double)got.s.ori[2] * want.ori[2] + (double)got.s.ori[3] * want.ori[3];
            REQUIRE(fabs(dot) > 0.999);
            // â˜… The discrete word is sourced for bodies, and must survive EXACTLY -- it is copied,
            // never blended, at every stage.
            REQUIRE(got.s.bits11 == want.bits11);
        }
        ++compared;
    }

    CHECK(compared > 50);
    CHECK(rebuilds > 1);            // the window really did roll, so the seam was crossed

    // ---- 2. the honest gaps, asserted as ABSENT rather than equal -------------------------------
    //
    // â˜…â˜… A chain test that quietly skipped these would certify a replay of a match with no gameplay
    // in it. Each is checked as a REPORTED absence, so the day one of them starts working the
    // assertion fails and somebody updates the claim.
    SUBCASE("this capture carries no netvar stream, and the artifact says THAT, not a refusal") {
        // â˜…â˜…â˜… THIS SUBCASE WAS ASSERTING A FALSEHOOD AND PASSING. The chain's gather writes no
        // netvars, so nothing is attempted -- and the retired bool reported "STORED, NOT APPLIED"
        // anyway, which would have told the owner his score and clock were dropped when in truth the
        // file never carried them. The honest answer for this file is "none at this playhead"; the
        // refusal claim belongs in B2, where netvars actually exist and actually get refused.
        const ApplyStats st = ap.Apply(w, (int64_t)script[100].t_us, ApplyOptions());
        CHECK(st.netvars_attempted == 0);
        CHECK(st.netvars_applied == 0);
        CHECK(st.netvar_state == NetVarFidelityState::NoRecords);
        CHECK(strstr(st.NetVarFidelity(), "NOT APPLIED") == nullptr);
        CHECK(nv.count == 0);
    }

    SUBCASE("the local pawn's action word and hold state are declared UNSOURCED by the file") {
        // â˜…â˜…â˜… THE GAMEPLAY WORD, and the assertion has to be about the FILE rather than about a value.
        // bits28 carries punch, grab, boost, stun and possession edges. A field the gather never read
        // is zero in the file and zero on playback, so comparing the installed value against the
        // captured one agrees at both ends and the absence is invisible -- which is exactly how a
        // replay of a match with no gameplay in it would certify clean.
        //
        // So this asserts what the file DECLARES: capture.cpp's local-pawn gather has no line for
        // kFillActionWord or kFillHolding, so no record in the file may claim them.
        CHECK((bs.player_fill_seen & replay::capture::kFillActionWord) == 0u);
        CHECK((bs.player_fill_seen & replay::capture::kFillHolding) == 0u);
        // """ + star + star + """ AND FROM THE GATHER'S OWN REPORT, which is the stronger statement: localFillMask is
        // what IEntity::ReadLocalPlayer RETURNED, so this is the engine declining to source the field
        // rather than the file happening not to carry it. The fake cannot fake it either -- there is
        // no kLocal bit for the action word, so no implementation of the contract can claim one.
        CHECK((rep.localFillMask & replay::capture::kFillActionWord) == 0u);
        CHECK((rep.localFillMask & replay::capture::kFillHolding) == 0u);
        CHECK((rep.localFillMask & replay::capture::kFillPose) != 0u);
        // And the pose fields it DOES source are claimed, so the check above is discriminating rather
        // than reading a fill nobody set.
        CHECK((bs.player_fill_seen & replay::capture::kFillPose) != 0u);
        CHECK((bs.player_fill_seen & replay::capture::kFillRefVel) != 0u);

        // â˜… THE ASYMMETRY IS THE FINDING. A body's discrete word IS sourced and IS asserted equal
        // above; the local pawn's is not. And it is a LOCAL-PAWN gap specifically -- IEntity::ReadPlayer
        // does source bits28 (entity.cpp reads PLAYER_CI_ACTION_WORD_OFF), so a remote player's action
        // word travels. The hole is the local pawn's own path, which is the player the recorder is.
        CHECK((bs.body_fill_seen & replay::capture::kFillBodyBits) != 0u);
    }

    SUBCASE("S_WORLD gathers nothing, so the file carries no world bodies") {
        // The 22 networked world slots -- the disc among them -- have no gather. Two tracked bodies
        // is what the scripted match fed; a world stream would have added more.
        CHECK(w.BodyCount() == 2);
    }
}

TEST_CASE("CHAIN: seek through a real file installs what playing to the same point installs") {
    // â˜…â˜…â˜… G1 END TO END. The synthesised-window version already caught the defect it exists for; this
    // one runs the same property through the real capture, a real file, and the real decoders --
    // which is where a CAPTURE-side bug would surface, and is how a keyframe shadow that recorded an
    // unstored value would have appeared.
    TempDemo tmp;
    static const uint32_t kFrames = 500;
    static ScriptedFrame script[kFrames];
    WriteChainDemo(tmp.path, kFrames, script);

    replay::DemoReader reader;
    REQUIRE(reader.Open(tmp.path));

    const uint32_t target_frame = 300;  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF plain 300 as a count, size or pixel value, not the extrapolation-cap offset
    // Deliberately OFF a recorded stamp, so the answer is an interpolation rather than a lookup.
    const int64_t target = (int64_t)script[target_frame].t_us + (int64_t)(kChainDt / 3);

    struct Arm {
        FakeEntity ent;
        FakePose pose;
        FakeNetVar nv;
        OwnerGuard og;
        Window w;
        BuildStats bs;
        StateApplier ap;
    };
    static Arm played, jumped;

    for (Arm* arm : {&played, &jumped}) {
        arm->ent.net_ids[11] = kChainNetId[0];
        arm->ent.net_ids[4] = kChainNetId[1];
        arm->nv.accept = false;
        arm->og.Reset(3, 0xFFFFFFFFu);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
        arm->ap.Bind(&arm->ent, &arm->pose, &arm->nv, &arm->og);
    }

    // Arm A: play into it, crossing chapter seams, overshooting, and reversing back -- the ugliest
    // ordinary path a user produces with a scrub bar.
    size_t centre = (size_t)-1;
    for (uint32_t f = 10; f <= target_frame + 40; f += 3) {
        const int64_t t = (int64_t)script[f].t_us;
        if (centre == (size_t)-1 || played.w.NeedsRebuild(t)) {
            centre = ChapterForTime(reader, t);
            REQUIRE(BuildWindowRange(reader, centre, played.w, played.bs));
            BindBodyHandles(played.ent, played.w);
        }
        played.ap.Apply(played.w, t, ApplyOptions());
    }
    for (uint32_t f = target_frame + 40; f > target_frame; f -= 3) {
        const int64_t t = (int64_t)script[f].t_us;
        if (played.w.NeedsRebuild(t)) {
            centre = ChapterForTime(reader, t);
            REQUIRE(BuildWindowRange(reader, centre, played.w, played.bs));
            BindBodyHandles(played.ent, played.w);
        }
        played.ap.Apply(played.w, t, ApplyOptions());
    }
    if (played.w.NeedsRebuild(target)) {
        centre = ChapterForTime(reader, target);
        REQUIRE(BuildWindowRange(reader, centre, played.w, played.bs));
        BindBodyHandles(played.ent, played.w);
    }
    played.ap.Apply(played.w, target, ApplyOptions());

    // Arm B: cold open, one jump. The cursors have never been anywhere.
    const size_t jcentre = ChapterForTime(reader, target);
    REQUIRE(BuildWindowRange(reader, jcentre, jumped.w, jumped.bs));
    BindBodyHandles(jumped.ent, jumped.w);
    jumped.ap.Apply(jumped.w, target, ApplyOptions());

    for (uint16_t slot : {(uint16_t)11, (uint16_t)4}) {
        INFO("engine slot ", slot);
        const FakeEntity::Slot& a = played.ent.slots[slot];
        const FakeEntity::Slot& b = jumped.ent.slots[slot];
        REQUIRE(a.installed);
        REQUIRE(b.installed);
        CHECK(a.t == b.t);
        CHECK(a.depth == b.depth);
        // Field by field, never memcmp: BodyState has padding, and a memcmp gate is red or green on
        // uninitialised bytes for reasons unrelated to its subject.
        for (int k = 0; k < 3; ++k) {
            CHECK(a.s.com[k] == b.s.com[k]);
            CHECK(a.s.vel[k] == b.s.vel[k]);
            CHECK(a.s.angVel[k] == b.s.angVel[k]);
            CHECK(a.s.lastHeldCom[k] == b.s.lastHeldCom[k]);
        }
        for (int k = 0; k < 4; ++k) {
            CHECK(a.s.ori[k] == b.s.ori[k]);
        }
        CHECK(a.s.bits11 == b.s.bits11);
    }
}

// =============================================================================================
// The discontinuity sweep: every a-b where both are samples of a source that can jump, stall,
// regress or reset.
// =============================================================================================

TEST_CASE("SWEEP: the step classifier answers 'did it go backwards' before it computes a magnitude") {
    // â˜…â˜…â˜… A discontinuity in the instrument looks exactly like a large reading from the thing being
    // measured. Unsigned subtraction collapses a REGRESSED table and a five-second GAP onto the same
    // ~1.8e19, so the order of the questions is the whole defence.
    uint64_t d = 0;
    CHECK(ClassifyStep(1000, 12111, 66666, d) == StepKind::Normal);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    CHECK(d == 11111u);

    d = 0xDEAD;
    CHECK(ClassifyStep(1000, 1000, 66666, d) == StepKind::ZeroDt);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    CHECK(d == 0u);                       // and it does not leave a stale magnitude behind

    CHECK(ClassifyStep(1000, 5001000, 66666, d) == StepKind::Gap);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot

    // â˜… The one an unsigned difference cannot express: backwards. Without the signed test first, this
    // is indistinguishable from the gap above -- same enormous number, same treatment, wrong finding.
    CHECK(ClassifyStep(5001000, 1000, 66666, d) == StepKind::Regressed);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    CHECK(ClassifyStep(1001, 1000, 66666, d) == StepKind::Regressed);   // by one microsecond  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    for (int i = 0; i < (int)StepKind::Regressed + 1; ++i) {
        CHECK(StepKindName((StepKind)i)[0] != '?');
    }
}

TEST_CASE("SWEEP: a duplicate stamp is a step of zero, not a permanent freeze") {
    // â˜…â˜…â˜… THE DEFECT THIS FOUND, stated as the evidence supports rather than as I first wrote it.
    // Duplicate stamps are legal -- ResolveBySearch has a case for them -- and the transport BROKE its
    // loop on d == 0, so the index stalled at the pair. My first claim was "froze for the rest of the
    // session"; running the mutation DISPROVED that, because the accumulator keeps growing and
    // CATCHUP_LIMIT micro-seeks past it after ~2 s. The real cost is a two-second stall plus a
    // SPURIOUS micro-seek on a perfectly legal file -- milder than I claimed, still wrong, and exactly
    // what a "did it get there eventually" assertion would have certified as healthy.
    //
    // MEASURED, by running the mutation with a corrected instrument: 189 frames to cross the pair
    // (~2.1 s at 90 Hz) and one spurious micro-seek. Against 1 frame and zero micro-seeks fixed.
    //
    // A stalled reading is a real reading; what must never happen is COMMANDING a zero, because that
    // is the +inf hazard.
    static uint64_t b[40];
    size_t n = 0;
    for (int i = 0; i < 10; ++i) b[n++] = 1000000 + (uint64_t)i * 11111;
    b[n] = b[n - 1]; ++n;                              // a duplicate stamp
    b[n] = b[n - 1]; ++n;                              // and a second, back to back
    for (int i = 1; i <= 10; ++i) b[n++] = b[11] + (uint64_t)i * 11111;

    TickTable tt;
    tt.Set(b, n);
    Transport tr;
    TransportTuning tune;
    tune.max_substeps = 8;
    tr.Reset(tt, tune);
    tr.SeekTo((int64_t)b[0]);
    int64_t f = 0, t = 0;
    Cause dc = Cause::Seek;
    tr.TakeDiscontinuity(f, t, dc);
    tr.SetRate(1.0);
    tr.SetPaused(false);

    const int64_t start = tr.DisplayPlayheadUs();
    int frames_to_pass = -1;
    uint64_t micro_at_pass = 0;
    for (int i = 0; i < 400 && frames_to_pass < 0; ++i) {
        const StepResult r = tr.Advance(11111);
        REQUIRE(r.commanded_step_us >= 1);             // never zero: the +inf hazard
        // â˜…â˜…â˜… THE SOLVER playhead, and against b[12] -- the first STRICTLY GREATER stamp.
        //
        // Two instrument defects, both found by running the mutation rather than by reading the test.
        // DisplayPlayheadUs is solver + accumulator, so it exceeds a boundary the moment acc grows,
        // WITHOUT the index moving -- it reported the duplicates crossed while the transport was stuck
        // on them. And b[9] == b[10] == b[11], so no comparison against those three can tell index 9
        // from index 11; only reaching b[12] proves the pair was actually traversed.
        if (tr.SolverPlayheadUs() >= (int64_t)b[12]) {
            frames_to_pass = i;
            // â˜… Sampled HERE and not at the end of a 400-frame run: this table spans ~0.24 s, so a
            // longer run outlasts the file and micro-seeks for E-P2 reasons that have nothing to do
            // with duplicates. Measuring the whole run would have blamed the end stop on this defect.
            micro_at_pass = tr.MicroSeeks();
        }
    }
    REQUIRE(frames_to_pass >= 0);
    // â˜…â˜… PROMPTLY, and that word is the assertion. The pre-sweep code broke the loop on a duplicate,
    // so the index stalled -- but NOT forever: the accumulator kept growing and CATCHUP_LIMIT
    // eventually micro-seeked past it. The real cost was a ~2 s stall and a SPURIOUS micro-seek
    // discontinuity on a legal file, which is milder than a freeze and still wrong, and is what a
    // "did it eventually get there" check would have called healthy.
    CHECK(frames_to_pass < 20);   // promptly, not after CATCHUP_LIMIT hauls it past
    CHECK(tr.ZeroDtSteps() >= 2);
    CHECK(micro_at_pass == 0);                         // no spurious discontinuity: THE defect
    CHECK(tr.GapsCrossed() == 0);                      // a duplicate is not a gap
    CHECK(tr.TableRegressions() == 0);
    CHECK(tr.DisplayPlayheadUs() > start);
}

TEST_CASE("SWEEP: a regressed tick table is crossed like a gap and REPORTED as itself") {
    // Same remedy, different finding. Calling a corrupt table a capture gap is what an unsigned
    // difference would have forced, and it would have left the real defect unnamed forever.
    static uint64_t b[40];
    size_t n = 0;
    for (int i = 0; i < 12; ++i) b[n++] = 1000000 + (uint64_t)i * 11111;
    b[n++] = 1000000;                                  // the table jumps BACKWARDS
    for (int i = 1; i <= 12; ++i) b[n++] = 1000000 + (uint64_t)i * 11111;

    TickTable tt;
    tt.Set(b, n);
    Transport tr;
    TransportTuning tune;
    tune.max_substeps = 8;
    tr.Reset(tt, tune);
    tr.SeekTo((int64_t)b[0]);
    int64_t f = 0, t = 0;
    Cause dc = Cause::Seek;
    tr.TakeDiscontinuity(f, t, dc);
    tr.SetRate(1.0);
    tr.SetPaused(false);
    for (int i = 0; i < 200; ++i) {
        const StepResult r = tr.Advance(11111);
        REQUIRE(r.commanded_step_us <= tune.max_step_us);
    }
    CHECK(tr.TableRegressions() >= 1);                 // named as a regression
    CHECK(tr.LargestStepUs() <= tune.max_step_us);     // and never handed to the clock

    // And the Window counts it at the source, so the finding exists even if the transport never runs.
    Window w;
    w.SetTicks(b, n);
    CHECK(w.TickRegressions() >= 1);
    static uint64_t clean[12];
    for (size_t i = 0; i < 12; ++i) clean[i] = 1000 + i * 100;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    Window w2;
    w2.SetTicks(clean, 12);
    CHECK(w2.TickRegressions() == 0);                  // the green arm, so the count discriminates
}

TEST_CASE("SWEEP: Alpha's underflow saturated to the FURTHEST wrong answer, and no longer can") {
    // â˜…â˜… (uint64_t)t - t0 with t below t0 wraps to ~1.8e19; the ratio is enormous; Clamp01 turned it
    // into 1.0 -- "fully at the NEWER sample", the furthest possible wrong answer and the hardest to
    // see. Clamping the result is not a guard, it is what hides the fault.
    BodyState frames[2];
    MakeBody(frames[0], 10.0f, 0.0f);
    MakeBody(frames[1], 20.0f, 0.0f);
    // A SECOND apart, not a millisecond: 10 m at 10 m/s is motion a body can make, and the pair has
    // to be interpolated for this case to be about Alpha at all. At 1 ms it is a 10,000 m/s
    // teleport, which SampleBodyAt now snaps -- a correct answer to a different question.
    const uint64_t times[2] = {5000, 1005000};

    SeriesCursor c;
    BodyState out;
    memset(&out, 0, sizeof out);

    // Inside the bracket: an honest interpolation.
    REQUIRE(SampleBodyAt(frames, times, 2, c, 505000, out));
    CHECK(out.com[0] == doctest::Approx(15.0f));

    // At or before the first sample: the OLDER value, never the newer one.
    c.Invalidate();
    REQUIRE(SampleBodyAt(frames, times, 2, c, 5000, out));
    CHECK(out.com[0] == doctest::Approx(10.0f));

    // Past the newest: held, not extrapolated.
    c.Invalidate();
    REQUIRE(SampleBodyAt(frames, times, 2, c, 2000000, out));
    CHECK(out.com[0] == doctest::Approx(20.0f));

    // Before the track entirely: absent, not saturated to either end.
    c.Invalidate();
    CHECK_FALSE(SampleBodyAt(frames, times, 2, c, 100, out));
    CHECK_FALSE(SampleBodyAt(frames, times, 2, c, -1, out));
}

// =================================================================================================
// OWNER-WATCH-FIX Q3. "in reverse the interpolation gets really funky and the disc no longer lines
// up with the hands." A recorded velocity is d(pose)/dt ALONG THE RECORDING; played backwards the
// body is genuinely moving the other way, and handing the engine the forward vector points every
// velocity consumer 180 degrees wrong.
// =================================================================================================

TEST_CASE("sampled velocities are negated when the playhead runs backwards, and only the velocities") {
    BodyState frames[2];
    MakeBody(frames[0], 10.0f, 0.0f);
    MakeBody(frames[1], 20.0f, 0.0f);
    const uint64_t times[2] = {5000, 1005000};

    SeriesCursor c;
    BodyState fwd, rev;
    memset(&fwd, 0, sizeof fwd);
    memset(&rev, 0, sizeof rev);

    REQUIRE(SampleBodyAt(frames, times, 2, c, 505000, fwd, false));
    c.Invalidate();
    REQUIRE(SampleBodyAt(frames, times, 2, c, 505000, rev, true));

    // The pose is the SAME instant either way -- reverse is not a different interpolation.
    for (int i = 0; i < 3; ++i) {
        CHECK(rev.com[i] == doctest::Approx(fwd.com[i]));
        CHECK(rev.ori[i] == doctest::Approx(fwd.ori[i]));
    }
    CHECK(rev.bits11 == fwd.bits11);

    // ...and the velocities are its exact negation. Negated, never zeroed: a zero is the claim
    // "not moving", which is a different and false statement about a body mid-flight.
    CHECK(fwd.vel[0] != doctest::Approx(0.0f));   // the mutant this would otherwise pass against
    for (int i = 0; i < 3; ++i) {
        CHECK(rev.vel[i] == doctest::Approx(-fwd.vel[i]));
        CHECK(rev.angVel[i] == doctest::Approx(-fwd.angVel[i]));
    }

    // Past the newest sample the value is HELD rather than interpolated, and that arm has to
    // negate too -- it is the one a reverse pass over the end of a track takes.
    c.Invalidate();
    BodyState heldFwd, heldRev;
    memset(&heldFwd, 0, sizeof heldFwd);
    memset(&heldRev, 0, sizeof heldRev);
    REQUIRE(SampleBodyAt(frames, times, 2, c, 2000000, heldFwd, false));
    c.Invalidate();
    REQUIRE(SampleBodyAt(frames, times, 2, c, 2000000, heldRev, true));
    CHECK(heldRev.com[0] == doctest::Approx(heldFwd.com[0]));
    CHECK(heldRev.vel[0] == doctest::Approx(-heldFwd.vel[0]));
    CHECK(heldFwd.vel[0] != doctest::Approx(0.0f));
}

TEST_CASE("a player's reference velocity follows the same sign, and the discrete words do not") {
    PlayerState frames[2];
    memset(frames, 0, sizeof frames);
    frames[0].refVel[0] = 3.0f;  frames[1].refVel[0] = 3.0f;
    frames[0].refVel[2] = -1.5f; frames[1].refVel[2] = -1.5f;
    frames[0].bits28 = 0x80u;    frames[1].bits28 = 0x80u;
    frames[0].holdR = 7;         frames[1].holdR = 7;
    frames[0].trigR = 0.5f;      frames[1].trigR = 0.5f;
    const uint64_t times[2] = {5000, 1005000};

    SeriesCursor c;
    PlayerState out;
    memset(&out, 0, sizeof out);
    REQUIRE(SamplePlayerAt(frames, times, 2, c, 505000, out, true));
    CHECK(out.refVel[0] == doctest::Approx(-3.0f));
    CHECK(out.refVel[2] == doctest::Approx(1.5f));
    // The attached-velocity edge and the grab id are words, not vectors: negating or zeroing them
    // would be the "reverse breaks the hold" defect wearing this fix's clothes.
    CHECK(out.bits28 == 0x80u);
    CHECK(out.holdR == 7);
    CHECK(out.trigR == doctest::Approx(0.5f));
}

TEST_CASE("SWEEP: a rewritten sample stamp is counted, because a silent rewrite smooths over a defect") {
    Window w;
    BodyState s;
    MakeBody(s, 1.0f, 0.0f);
    REQUIRE(w.AddBodySample(NetId(0xAAAA), 5000, s));
    CHECK(w.SampleTimeRegressions() == 0);            // green arm
    REQUIRE(w.AddBodySample(NetId(0xAAAA), 4000, s)); // backwards within its own track
    CHECK(w.SampleTimeRegressions() == 1);            // red arm, and the clamp still happened
    w.Finalize();
    const BodyTrack& tr = w.BodyAt(0);
    const uint64_t* times = w.BodyTimes(tr);
    CHECK(times[1] >= times[0]);                      // the cursor's precondition is still held
}

// =============================================================================================
// Three-state answers that were being carried as two
// =============================================================================================

TEST_CASE("THREE-STATE: netvar fidelity distinguishes 'none to write' from 'refused'") {
    // â˜…â˜…â˜… The defect W2-F found in its own ContentMismatch, sitting in my code one seam away -- and
    // mine was worse, because ActiveModeLabel RENDERS it. A file with no netvar stream would have told
    // the owner his score and clock were STORED, NOT APPLIED: a claim about the world that is not true.
    CHECK(strcmp(NetVarFidelityText(NetVarFidelityState::NoRecords),
                 NetVarFidelityText(NetVarFidelityState::Refused)) != 0);
    CHECK(strcmp(NetVarFidelityText(NetVarFidelityState::Applied),
                 NetVarFidelityText(NetVarFidelityState::Refused)) != 0);
    CHECK(strstr(NetVarFidelityText(NetVarFidelityState::Refused), "NOT APPLIED") != nullptr);
    CHECK(strstr(NetVarFidelityText(NetVarFidelityState::NoRecords), "NOT APPLIED") == nullptr);
    CHECK(strstr(NetVarFidelityText(NetVarFidelityState::Applied), "NOT APPLIED") == nullptr);

    Window w;
    FillWindow(w);
    FakeEntity ent; FakePose pose; OwnerGuard og;
    og.Reset(3, 0xFFFFFFFFu); og.RegisterEntrant(0x00010004u);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop

    SUBCASE("a window with NO netvars reports NoRecords, not a refusal") {
        FakeNetVar nv;
        nv.accept = false;                       // would refuse -- but nothing asks it to
        StateApplier ap;
        ap.Bind(&ent, &pose, &nv, &og);
        const ApplyStats st = ap.Apply(w, 1000000 + 10 * 11111, ApplyOptions());
        REQUIRE(st.netvars_attempted == 0);
        CHECK(st.netvar_state == NetVarFidelityState::NoRecords);
        CHECK(strstr(st.NetVarFidelity(), "NOT APPLIED") == nullptr);
    }

    SUBCASE("writes that are refused report Refused") {
        KeyedRecord seeds[1] = {{0, 0xAA, 100}};
        REQUIRE(w.NetVars().Build(seeds, 1, nullptr, 0));
        w.NoteNetVarSlot(0xAA, 0);
        w.NoteNetVarSlot(0xBB, 0);   // the slot the shipped builder always records
        FakeNetVar nv;
        nv.accept = false;
        StateApplier ap;
        ap.Bind(&ent, &pose, &nv, &og);
        const ApplyStats st = ap.Apply(w, 1000000 + 10 * 11111, ApplyOptions());
        REQUIRE(st.netvars_attempted == 1);
        CHECK(st.netvar_state == NetVarFidelityState::Refused);
        CHECK(strstr(st.NetVarFidelity(), "NOT APPLIED") != nullptr);
    }

    SUBCASE("writes that land report Applied") {
        KeyedRecord seeds[1] = {{0, 0xAA, 100}};
        REQUIRE(w.NetVars().Build(seeds, 1, nullptr, 0));
        w.NoteNetVarSlot(0xAA, 0);
        w.NoteNetVarSlot(0xBB, 0);   // the slot the shipped builder always records
        FakeNetVar nv;
        StateApplier ap;
        ap.Bind(&ent, &pose, &nv, &og);
        const ApplyStats st = ap.Apply(w, 1000000 + 10 * 11111, ApplyOptions());
        CHECK(st.netvar_state == NetVarFidelityState::Applied);
    }

    SUBCASE("summing frames: Refused dominates, and NoRecords never overwrites Applied") {
        // A report over many frames must say "something was refused" if anything was, and must not
        // say "none" because the last frame happened to have nothing to write.
        ApplyStats total;
        ApplyStats applied;  applied.netvar_state = NetVarFidelityState::Applied;
        ApplyStats none;     none.netvar_state = NetVarFidelityState::NoRecords;
        ApplyStats refused;  refused.netvar_state = NetVarFidelityState::Refused;
        total.Add(applied);
        total.Add(none);
        CHECK(total.netvar_state == NetVarFidelityState::Applied);
        total.Add(refused);
        CHECK(total.netvar_state == NetVarFidelityState::Refused);
        total.Add(applied);
        CHECK(total.netvar_state == NetVarFidelityState::Refused);   // does not un-refuse
    }
}

TEST_CASE("THREE-STATE: 'the guard was never configured' is not 'not an entrant'") {
    // "We asked and this is not an entrant we admitted" is a finding about the REPLAY. "Nobody
    // configured the guard" is a finding about US. They were one value, so a caller seeing
    // NotRegistered could not tell which it had.
    OwnerGuard fresh;
    CHECK(fresh.Validate(0x00010004u) == OwnerVerdict::GuardUnconfigured);
    CHECK(fresh.RegisterEntrant(0x00010004u) == OwnerVerdict::GuardUnconfigured);

    OwnerGuard g;
    g.Reset(3, 0xFFFFFFFFu);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    REQUIRE(g.RegisterEntrant(0x00010004u) == OwnerVerdict::Ok);
    CHECK(g.Validate(0x00010004u) == OwnerVerdict::Ok);
    // Configured, asked, and the answer is genuinely no -- a different verdict from the above.
    CHECK(g.Validate(0x00010009u) == OwnerVerdict::NotRegistered);
    CHECK(strcmp(OwnerVerdictName(OwnerVerdict::GuardUnconfigured),
                 OwnerVerdictName(OwnerVerdict::NotRegistered)) != 0);
    for (int i = 0; i < (int)OwnerVerdict::Count; ++i) {
        CHECK(OwnerVerdictName((OwnerVerdict)i)[0] != '?');
    }
}

// â˜… L3-RUN-4 S4: live, `step +1` moved one tick and `step -1` moved two. The playhead between two
// recorded ticks is At(index) + acc, and the tick nearest it BACKWARDS is At(index) itself -- the
// arm moved the index as well, so it paid the surplus and a whole period.
TEST_CASE("W2-B/L3-RUN-4 S4: a step from between two ticks moves ONE tick in either direction") {
    // The tick spacing is ONE constant and every boundary is a multiple of it, so the case says
    // what the fixture's cadence is instead of restating five products of it.
    constexpr uint64_t kTickUs = 11111;
    static const uint64_t b[] = {0, kTickUs, kTickUs * 2, kTickUs * 3, kTickUs * 4, kTickUs * 5};
    TickTable tt;
    tt.Set(b, sizeof b / sizeof b[0]);
    Transport tr;
    tr.Reset(tt, TransportTuning());

    SUBCASE("forward and backward from the same inter-tick playhead are one tick each") {
        tr.SeekTo((int64_t)b[2] + 5000);
        const int64_t from = tr.DisplayPlayheadUs();
        CHECK(from == (int64_t)b[2] + 5000);
        CHECK(tr.StepTicks(-1) == -1);
        // The tick below 27222 is 22222, NOT 11111. The defect landed on 11111, two periods away.
        CHECK(tr.DisplayPlayheadUs() == (int64_t)b[2]);

        tr.SeekTo(from);
        CHECK(tr.StepTicks(1) == 1);
        CHECK(tr.DisplayPlayheadUs() == (int64_t)b[3]);
    }

    SUBCASE("a playhead already ON a tick still moves a whole period, both ways") {
        tr.SeekTo((int64_t)b[3]);
        CHECK(tr.StepTicks(-1) == -1);
        CHECK(tr.DisplayPlayheadUs() == (int64_t)b[2]);
        CHECK(tr.StepTicks(1) == 1);
        CHECK(tr.DisplayPlayheadUs() == (int64_t)b[3]);
    }

    SUBCASE("the surplus is ONE of the n asked for, never a free extra") {
        tr.SeekTo((int64_t)b[4] + 5000);
        CHECK(tr.StepTicks(-2) == -2);
        CHECK(tr.DisplayPlayheadUs() == (int64_t)b[3]);
    }

    SUBCASE("index 0 with a surplus still has a tick to step back onto") {
        tr.SeekTo(5000);
        CHECK(tr.StepTicks(-1) == -1);
        CHECK(tr.DisplayPlayheadUs() == (int64_t)b[0]);
        // And now there is nothing below it.
        CHECK(tr.StepTicks(-1) == 0);
    }
}

// â˜… AUDIT_P17 F1. `max_body_samples` was 16384 -- "64 bodies x 90 frames x 3", one SECOND of
// chapters. The chapter span is the manifest's keyframe interval and the real capture's is 4.033 s.
// Every number below is spelled out from the file's own two terms, so a formula that lost a factor
// (halved, or three chapters read as one) fails here rather than at somebody's seek.
TEST_CASE("W2-B/AUD-P17 F1: the window pool is derived from the capture's cadence") {
    using namespace replay::playback;

    // Ceil: 2000000/11111 is 180.0018 chapters' worth of ticks, and 180 would be one short.
    CHECK(TicksPerChapter(2000000, 11111) == 181);
    CHECK(TicksPerChapter(4033000, 11111) == 363);
    // Zero in, zero out -- an unanswered cadence must not become a confident default.
    CHECK(TicksPerChapter(0, 11111) == 0);
    CHECK(TicksPerChapter(2000000, 0) == 0);

    const WindowCaps c = CapsForCadence(2000000, 11111);
    CHECK(c.max_ticks == 181 * 3);
    CHECK(c.max_body_samples == 64 * 181 * 3);      // 34752
    CHECK(c.max_player_samples == 16 * 181 * 3);    // 8688
    // The constant this replaces was 16384: less than half of what its own sizing note claimed.
    // What the retired pool held, in the terms its own sizing note used: 64 bodies x one SECOND of
    // 90 Hz frames x three chapters. The shipped constant was under even that.
    constexpr size_t kOneSecondPool = 64 * 90 * kWindowChapters;
    CHECK(c.max_body_samples > kOneSecondPool);
    // The locked target: eight players, the disc and both goals, sampled every tick of the window.
    CHECK((size_t)(11 * 181 * 3) <= c.max_body_samples);

    // The L2 #3 capture's own chapters, measured off its index by the audit.
    const WindowCaps real = CapsForCadence(4033000, 11111);
    CHECK(real.max_body_samples == 64 * 363 * 3);   // 69696
    CHECK(real.max_body_samples > kOneSecondPool * 3);   // the old pool could not have held it

    Window w;
    const char* why = "";
    CHECK_FALSE(w.Reserve(CapsForCadence(0, 0), &why));
    CHECK(strstr(why, "how far apart") != nullptr);
    CHECK_FALSE(w.Reserve(CapsForCadence(600000000, 11111), &why));
    CHECK(strstr(why, "more samples than") != nullptr);

    const size_t before = w.Caps().max_body_samples;
    CHECK(before < real.max_body_samples);
    CHECK(w.Reserve(real, nullptr));
    CHECK(w.Caps().max_body_samples == real.max_body_samples);
    // Grows only: a narrower file after a wider one keeps the wider pools.
    CHECK(w.Reserve(c, nullptr));
    CHECK(w.Caps().max_body_samples == real.max_body_samples);
}

namespace {

// Twelve 2 s chapters of ten concurrently sampled bodies -- an Arena roster's shape, not one body.
uint64_t WriteTenBodyDemo(const wchar_t* path, uint64_t t0, uint32_t frames, uint32_t bodies) {
    using namespace replay::capture;
    using namespace replay;
    const uint64_t dt = 11111;

    ManifestBuilder mb;
    mb.Begin(t0, 0, 0x40);
    const uint8_t md5[16] = {0xca, 0xa0, 0x5e, 0xe3};
    mb.SetBuild(md5, 0x676F6C646D617374ull, 0x0102030405060708ull);
    mb.SetLevel(6300205991959903307ull, 0xF00Dull, 1780177787ull);
    mb.SetKeyframeIntervalUs(2000000);
    mb.SetUuid(7, 8);
    PlayerRecord p{};
    // This fixture writes BODY samples only: the roster exists to make the manifest well-formed and
    // its id carries nothing, so it is the body count rather than a number that could name a fact.
    p.xplatformId = bodies;
    mb.AddPlayer(p);

    DemoWriter writer;
    REQUIRE(writer.Open(path, mb.View()));
    BuilderConfig cfg;
    cfg.keyframeIntervalUs = 2000000;
    for (uint16_t i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;
    for (uint16_t i = 0; i < kSectionCount; ++i) cfg.sectionEnabled[i] = true;

    ChapterBuilder b;
    REQUIRE(b.Open(&writer, mb.View(), cfg));
    b.EmitMatchBegin(t0);
    for (uint32_t f = 0; f < frames; ++f) {
        const uint64_t st = t0 + f * dt;
        TickRec tr{};
        tr.sourceTimeUs = st;
        tr.simTimeUs = 1000000 + f * dt;
        tr.frameIndex = f + 1;
        tr.frameDeltaUs = (uint32_t)dt;
        tr.scaledDeltaUs = (uint32_t)dt;
        Feed(b, kRecTick, tr);
        for (uint32_t k = 0; k < bodies; ++k) {
            SampledBodyRec br{};
            br.sourceTimeUs = st;
            br.frameIndex = f + 1;
            br.idValue = 0x00000001ABCD0000ull + k;
            br.idTag = 0;
            br.fill = kFillBodyTransform | kFillBodyLastHeld | kFillBodyBits | kFillBodyIdValue |
                      kFillBodyIdNetActor;
            br.handle = 0;
            br.state.com[0] = (float)f;
            br.state.ori[3] = 1.0f;
            Feed(b, kRecSampledBody, br);
        }
    }
    b.EmitMatchEnd(t0 + frames * dt);
    b.FlushChapter();
    b.Close();
    writer.Close();
    return dt;
}

}  // namespace

// The sweep the sizing decides: twelve chapters, ten bodies, a window built at every chapter and at
// both sides of every seam. The caps come from the FILE -- its widest core chapter span over its own
// measured tick period -- so a formula that loses a factor cannot hold this and the sweep reds.
TEST_CASE("W2-B/AUD-P17 F1: a 12-chapter, 10-body file builds a window everywhere") {
    using namespace replay;
    using namespace replay::playback;
    TempDemo tmp;
    const uint64_t t0 = 5000000;
    const uint32_t bodies = 10;
    const uint32_t frames = 2160;                 // 24.0 s at 11111 us -> twelve 2 s chapters
    const uint64_t dt = WriteTenBodyDemo(tmp.path, t0, frames, bodies);

    DemoReader reader;
    REQUIRE(reader.Open(tmp.path));
    REQUIRE(CountCoreChapters(reader) >= 12);

    // Measure the cadence the way Load does: the widest CORE chapter in the index, and the mean tick
    // spacing of the first CORE chapter that carries ticks.
    uint64_t spanUs = 0, periodUs = 0;
    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        const ChapterKey k = reader.ChapterAt(i);
        if (k.section == kSectionCore && k.tEndUs > k.tBeginUs && k.tEndUs - k.tBeginUs > spanUs) {
            spanUs = k.tEndUs - k.tBeginUs;
        }
    }
    for (size_t i = 0; i < reader.ChapterCount() && periodUs == 0; ++i) {
        const ChapterKey k = reader.ChapterAt(i);
        if (k.section != kSectionCore || k.tEndUs <= k.tBeginUs) continue;
        DecodedChapter dc;
        if (!reader.DecodeChapter(i, dc)) continue;
        const StreamView* tv = dc.Find(kStreamTick);
        if (tv != nullptr && tv->recordCount != 0) {
            periodUs = (k.tEndUs - k.tBeginUs) / tv->recordCount;
        }
    }
    CHECK(spanUs >= 1900000);
    // The mean spacing over the chapter's own span, which is a hair UNDER the true period because
    // the span covers n ticks and n-1 intervals. Under is the safe side: it sizes for more ticks.
    CHECK(periodUs <= dt);
    CHECK(periodUs > dt - dt / 10);

    // Sized for this file's roster, so the pool is what the formula says and nothing spare: a halved
    // formula is a pool that cannot hold three chapters of ten bodies, and every build below refuses.
    Window w;
    const char* why = "";
    REQUIRE(w.Reserve(CapsForCadence(spanUs, periodUs, bodies, 4), &why));

    size_t built = 0;
    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        const ChapterKey k = reader.ChapterAt(i);
        if (k.section != kSectionCore) continue;
        BuildStats bs;
        INFO("refusal: ", bs.refusal ? bs.refusal : "none");
        REQUIRE(BuildWindowRange(reader, i, w, bs));
        CHECK(w.BodyCount() == bodies);
        ++built;
        // Both sides of the seam, which an eighths sweep straddles only by luck.
        for (int off = -1; off <= 1; off += 2) {
            const int64_t t = (int64_t)k.tBeginUs + off * (int64_t)dt;
            if (t < (int64_t)t0 || t > (int64_t)(t0 + frames * dt)) continue;
            BuildStats bs2;
            const size_t centre = ChapterForTime(reader, t);
            INFO("seam refusal: ", bs2.refusal ? bs2.refusal : "none");
            REQUIRE(BuildWindowRange(reader, centre, w, bs2));
            CHECK(w.CoversForSampling((uint64_t)t));
        }
    }
    CHECK(built >= 12);
}

// ---------------------------------------------------------------------------------------------
// The capture L3-RUN-5 played, by name. 161 KB, so it lives beside this file rather than in the
// owner's Documents: a case that reads a path outside the repo is a case that silently stops
// running the day that directory is tidied, and this one is the only offline reproduction of a
// field asymmetry that survived one synthetic fix already.
#define REPLAY_L3RUN5_CAPTURE "2026-09-04_08-01-50_0000000000000000_0000000000000000.echodemo"
#define REPLAY_WIDEN_(x) L##x
#define REPLAY_WIDEN(x) REPLAY_WIDEN_(x)
static const char kL3Run5PathA[] = REPLAY_PLAYBACK_FIXTURE_DIR "/" REPLAY_L3RUN5_CAPTURE;
static const wchar_t kL3Run5PathW[] =
    REPLAY_WIDEN(REPLAY_PLAYBACK_FIXTURE_DIR) L"/" REPLAY_WIDEN(REPLAY_L3RUN5_CAPTURE);

// The capture itself is NOT in the repo: `.gitignore:2` is `*.echodemo`, and that rule is there to
// keep recorded sessions -- which carry the owner's own config -- out of history. So the case looks
// in the two places the file can be, and when it is in neither it says so in the run's own output
// rather than passing quietly. A fixture that cannot fail is worse than one that is absent, and an
// absence nobody is told about is worse than both.
bool OpenL3Run5Capture(replay::DemoReader& reader, char* whereOut, size_t whereChars) {
    if (reader.Open(kL3Run5PathW)) {
        _snprintf_s(whereOut, whereChars, _TRUNCATE, "%s", kL3Run5PathA);
        return true;
    }
    const wchar_t* home = _wgetenv(L"USERPROFILE");
    if (home == nullptr) {
        _snprintf_s(whereOut, whereChars, _TRUNCATE, "%s (and USERPROFILE is not set)", kL3Run5PathA);
        return false;
    }
    wchar_t alt[1024];
    _snwprintf_s(alt, _TRUNCATE, L"%s/Documents/EchoReplay/%s", home,
                 REPLAY_WIDEN(REPLAY_L3RUN5_CAPTURE));
    if (reader.Open(alt)) {
        _snprintf_s(whereOut, whereChars, _TRUNCATE, "%%USERPROFILE%%/Documents/EchoReplay/%s",
                    REPLAY_L3RUN5_CAPTURE);
        return true;
    }
    _snprintf_s(whereOut, whereChars, _TRUNCATE,
                "neither %s nor %%USERPROFILE%%/Documents/EchoReplay/%s", kL3Run5PathA,
                REPLAY_L3RUN5_CAPTURE);
    return false;
}

// ---------------------------------------------------------------------------------------------
// L3-RUN-6 S8. THE OLD FILE, AND THE ONE THING THE READER CAN DO ABOUT IT.
//
// The writer fix stops lobby marks being filed on the tap's arrival clock. It cannot
// reach a capture that is already on disk: L2-RUN-7's own file carries three marks at 1.874 s under
// a t0 of 18.900 s, and `round 1` / `goto round 1` computed -17 025 384 us from them -- a playhead
// outside every span, silently clamped by the transport to the first tick and read in the field as
// a seek fault (L3-RUN-6 rows 27 and 28).
//
// The clamp has to be here rather than at the seek's end, because by the time the transport sees a
// negative time the reason for it is gone: a target below the origin and a target the window cannot
// reach are the same number to it. ClampRoundSeekUs keeps the two apart and names the second one
// kRoundPartialStart -- the capture opened after this round began, which is precisely true.
//
// The first case pins the boundary and always runs. The second drives the real file, which is the
// only place the -17 s exists.
TEST_CASE("W2-B/L3-RUN-6 S8: the round-seek clamp holds exactly at the origin") {
    using namespace replay;

    // Not zero: playback.h's rule is that a zero origin is the degenerate case in which the two
    // axes are the same number and this invariant cannot fail. This is L2-RUN-7's own t0.
    const int64_t t0 = 18899811;

    bool partialStart = false;
    CHECK(playback::ClampRoundSeekUs(t0 + 1, t0, &partialStart) == t0 + 1);
    CHECK_FALSE(partialStart);

    partialStart = false;
    CHECK(playback::ClampRoundSeekUs(t0, t0, &partialStart) == t0);
    CHECK_FALSE(partialStart);   // ON the origin is inside the file, not before it

    partialStart = false;
    CHECK(playback::ClampRoundSeekUs(t0 - 1, t0, &partialStart) == t0);
    CHECK(partialStart);

    // The field number itself: L2-RUN-7's lobby mark against L2-RUN-7's t0.
    partialStart = false;
    const int64_t mark = 1874427;
    REQUIRE(mark - t0 == -17025384);   // the value rows 27 and 28 printed, from its two operands
    CHECK(playback::ClampRoundSeekUs(mark, t0, &partialStart) == t0);
    CHECK(partialStart);

    // A null out-parameter is a caller that does not care, not a crash.
    CHECK(playback::ClampRoundSeekUs(mark, t0, nullptr) == t0);
}

#define REPLAY_L2RUN7_CAPTURE "2026-09-04_14-53-31_0000000000000000_0000000000000000.echodemo"
static const char kL2Run7PathA[] = REPLAY_PLAYBACK_FIXTURE_DIR "/" REPLAY_L2RUN7_CAPTURE;
static const wchar_t kL2Run7PathW[] =
    REPLAY_WIDEN(REPLAY_PLAYBACK_FIXTURE_DIR) L"/" REPLAY_WIDEN(REPLAY_L2RUN7_CAPTURE);

// Same two places as the L3-RUN-5 capture above, and the same reason: `.gitignore:2` keeps
// `*.echodemo` out of history, so an absent file is reported in the run's own output rather than
// passed over quietly.
bool OpenL2Run7Capture(replay::DemoReader& reader, char* whereOut, size_t whereChars) {
    if (reader.Open(kL2Run7PathW)) {
        _snprintf_s(whereOut, whereChars, _TRUNCATE, "%s", kL2Run7PathA);
        return true;
    }
    const wchar_t* home = _wgetenv(L"USERPROFILE");
    if (home == nullptr) {
        _snprintf_s(whereOut, whereChars, _TRUNCATE, "%s (and USERPROFILE is not set)", kL2Run7PathA);
        return false;
    }
    wchar_t alt[1024];
    _snwprintf_s(alt, _TRUNCATE, L"%s/Documents/EchoReplay/%s", home,
                 REPLAY_WIDEN(REPLAY_L2RUN7_CAPTURE));
    if (reader.Open(alt)) {
        _snprintf_s(whereOut, whereChars, _TRUNCATE, "%%USERPROFILE%%/Documents/EchoReplay/%s",
                    REPLAY_L2RUN7_CAPTURE);
        return true;
    }
    _snprintf_s(whereOut, whereChars, _TRUNCATE,
                "neither %s nor %%USERPROFILE%%/Documents/EchoReplay/%s", kL2Run7PathA,
                REPLAY_L2RUN7_CAPTURE);
    return false;
}

TEST_CASE("W2-B/L3-RUN-6 S8: on the REAL capture, no round seek lands before the file's origin") {
    using namespace replay;

    DemoReader reader;
    char where[1024];
    const bool present = OpenL2Run7Capture(reader, where, sizeof where);
    INFO("capture: ", where);
    if (!present) {
        WARN_MESSAGE(false, "SKIPPED, capture absent: ", where);
        return;
    }

    const Manifest& m = reader.GetManifest();
    const int64_t t0 = (int64_t)m.t0Us;
    REQUIRE(t0 > 0);

    int offAxis = 0;
    int64_t worst = 0;
    for (uint16_t i = 0; i < m.roundCount; ++i) {
        const int64_t rel = (int64_t)m.rounds[i].tUs - t0;
        if (rel < 0) {
            ++offAxis;
            if (rel < worst) {
                worst = rel;
            }
        }
    }
    INFO("t0 ", (long long)t0, " marks ", (unsigned)m.roundCount, " off-axis ", offAxis, " worst ",
         (long long)worst);

    // THE SUBJECT MUST EXIST. Every assertion below is vacuous on a file with nothing before its
    // origin, and this is the one file the field measured the -17 s on -- if it ever stops carrying
    // an off-axis mark, this case is testing nothing and must say so rather than pass.
    REQUIRE(offAxis > 0);
    CHECK(worst == -17025384);   // L3-RUN-6 S8's number, read back off the file that produced it

    for (uint16_t i = 0; i < m.roundCount; ++i) {
        bool partialStart = false;
        const int64_t target =
            playback::ClampRoundSeekUs((int64_t)m.rounds[i].tUs, t0, &partialStart);
        CHECK(target >= t0);                                            // never a negative playhead
        CHECK(partialStart == ((int64_t)m.rounds[i].tUs < t0));
        CHECK(target == (partialStart ? t0 : (int64_t)m.rounds[i].tUs));  // and nothing else moves
    }

    // The ARCH-21 round table reads by the same rule: `round 1` takes its target from a row when the
    // file carries one, and this file's row is derived from those same marks.
    for (uint16_t i = 0; i < m.roundRowCount; ++i) {
        bool partialStart = false;
        const int64_t begin =
            playback::ClampRoundSeekUs((int64_t)m.roundRows[i].beginUs, t0, &partialStart);
        CHECK(begin >= t0);
        CHECK(partialStart == ((int64_t)m.roundRows[i].beginUs < t0));
    }
}

// W2-B / L3-RUN-5 F4. THE REAL CAPTURE, OFFLINE.
//
// The field record (L3-RUN-5 S4) is two console lines from launch-01, 09:49:45:
//
//     stepped 1 tick(s) from 116615500 us to 116632167 us      (+16 667 us)
//     stepped -1 tick(s) from 116632167 us to 116598834 us     (-33 333 us)
//
// d3ab817's step case was synthetic and could not see this: its table was a constant `kTickUs * n`
// and every seek in it landed ON a boundary, so the accumulator was zero at every step and the
// surplus arm it added was never the thing under test. This case drives the actual file the launch
// played, through the actual window builder, so the tick spacing, the chapter seams and the
// mid-tick landing a percentage seek makes are the capture's own and not a fixture author's.
//
// The claim, in both directions and at every probe: ONE step consumes exactly one recorded tick.
// From between two boundaries that is the boundary on that side; from a boundary it is the next one
// along; and StepTicks reports the 1 it moved, not the count it was asked for.
TEST_CASE("W2-B/L3-RUN-5 F4: on the REAL capture, one step is one recorded tick either way") {
    using namespace replay;

    DemoReader reader;
    char where[1024];
    const bool present = OpenL3Run5Capture(reader, where, sizeof where);
    INFO("capture: ", where);
    if (!present) {
        WARN_MESSAGE(false, "SKIPPED, capture absent: ", where);
        return;
    }

    const Manifest& m = reader.GetManifest();
    const int64_t t0 = (int64_t)m.t0Us;
    int64_t coreEnd = t0;
    size_t coreChapters = 0;
    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        const ChapterKey k = reader.ChapterAt(i);
        if (k.section == kSectionCore) {
            ++coreChapters;
            if ((int64_t)k.tEndUs > coreEnd) {
                coreEnd = (int64_t)k.tEndUs;
            }
        }
    }
    const int64_t durationUs = coreEnd - t0;
    INFO("chapters ", (unsigned)reader.ChapterCount(), " core ", (unsigned)coreChapters,
         " duration us ", (long long)durationUs);
    REQUIRE(coreChapters > 0);
    REQUIRE(durationUs > 0);

    // The launch's own three absolute seek points, as fractions of the duration rather than as the
    // microseconds it happened to print: the same arithmetic the battery does, over the same file.
    const int64_t fractions[3][2] = {{1, 4}, {1, 2}, {9, 10}};

    for (int f = 0; f < 3; ++f) {
        const int64_t target = t0 + durationUs * fractions[f][0] / fractions[f][1];
        Window w;
        BuildStats bs;
        const size_t centre = ChapterForTime(reader, target);
        INFO("window refusal: ", bs.refusal ? bs.refusal : "none");
        REQUIRE(BuildWindowRange(reader, centre, w, bs));
        const TickTable& tt = w.Ticks();
        REQUIRE(tt.Count() > 2);

        TransportTuning tune;

        // Two starting states at every probe, and the difference between them is the whole point:
        // state 0 is wherever the percentage seek actually put the playhead, and state 1 is forced
        // to sit strictly between two boundaries, so the arm that consumes the sub-tick remainder is
        // exercised even on a file whose seeks happen to land square.
        for (int state = 0; state < 2; ++state) {
            Transport tr;
            tr.Reset(tt, tune);
            tr.SetFileSpan(t0, coreEnd);
            tr.SeekTo(target);
            if (state == 1) {
                const size_t i0 = tt.IndexAt(tr.DisplayPlayheadUs());
                REQUIRE(i0 + 1 < tt.Count());
                const int64_t span = (int64_t)tt.At(i0 + 1) - (int64_t)tt.At(i0);
                REQUIRE(span > 1);
                tr.SeekTo((int64_t)tt.At(i0) + span / 2);
            }

            const int64_t before = tr.DisplayPlayheadUs();
            const size_t i = tt.IndexAt(before);
            REQUIRE(i > 0);
            REQUIRE(i + 1 < tt.Count());
            // The boundary one step away, read off the table rather than off the transport: below is
            // At(i) when the playhead is between boundaries and At(i-1) when it is on one; above is
            // At(i+1) either way.
            const int64_t below =
                (before > (int64_t)tt.At(i)) ? (int64_t)tt.At(i) : (int64_t)tt.At(i - 1);
            const int64_t above = (int64_t)tt.At(i + 1);
            INFO("state ", state, " probe ", f, " before ", (long long)before, " below ",
                 (long long)below, " above ", (long long)above, " acc ", tr.AccumulatorUs());

            const int back = tr.StepTicks(-1);
            const int64_t afterBack = tr.DisplayPlayheadUs();
            INFO("back ", back, " -> ", (long long)afterBack);
            CHECK(back == -1);
            CHECK(afterBack == below);

            Transport fr;
            fr.Reset(tt, tune);
            fr.SetFileSpan(t0, coreEnd);
            fr.SeekTo(before);
            const int fwd = fr.StepTicks(1);
            const int64_t afterFwd = fr.DisplayPlayheadUs();
            INFO("fwd ", fwd, " -> ", (long long)afterFwd);
            CHECK(fwd == 1);
            CHECK(afterFwd == above);

            // The asymmetry the field measured, stated as arithmetic: the two single steps out of
            // one playhead cannot between them span more than the two recorded ticks they sit in. A
            // backward step that pays a sub-tick remainder AND a whole index move fails this.
            const int64_t widest = (int64_t)tt.At(i + 1) - (int64_t)tt.At(i - 1);
            CHECK(before - below > 0);
            CHECK(above - before > 0);
            CHECK((above - before) + (before - below) <= widest);
        }
    }
}

// The field's exact sequence, in order, on the file that produced it: seek to the midpoint, step
// forward, step back. The forward step lands on a boundary, so the backward one starts from a
// boundary and owes a whole tick -- but it owes ONE, and the playhead it leaves must be the boundary
// the forward step departed from.
TEST_CASE("W2-B/L3-RUN-5 F4: the launch's own seek-step-step sequence, replayed offline") {
    using namespace replay;

    DemoReader reader;
    char where[1024];
    const bool present = OpenL3Run5Capture(reader, where, sizeof where);
    INFO("capture: ", where);
    if (!present) {
        WARN_MESSAGE(false, "SKIPPED, capture absent: ", where);
        return;
    }
    const Manifest& m = reader.GetManifest();
    const int64_t t0 = (int64_t)m.t0Us;
    int64_t coreEnd = t0;
    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        const ChapterKey k = reader.ChapterAt(i);
        if (k.section == kSectionCore && (int64_t)k.tEndUs > coreEnd) {
            coreEnd = (int64_t)k.tEndUs;
        }
    }
    const int64_t half = t0 + (coreEnd - t0) / 2;

    Window w;
    BuildStats bs;
    INFO("window refusal: ", bs.refusal ? bs.refusal : "none");
    REQUIRE(BuildWindowRange(reader, ChapterForTime(reader, half), w, bs));
    const TickTable& tt = w.Ticks();
    TransportTuning tune;
    Transport tr;
    tr.Reset(tt, tune);
    tr.SetFileSpan(t0, coreEnd);
    tr.SeekTo(half);

    const int64_t landed = tr.DisplayPlayheadUs();
    INFO("half ", (long long)half, " landed ", (long long)landed);
    CHECK(landed == half);   // a percentage seek names the microsecond it asked for

    const size_t i = tt.IndexAt(landed);
    REQUIRE(i > 0);
    REQUIRE(i + 1 < tt.Count());
    const int64_t next = (int64_t)tt.At(i + 1);
    const int64_t prev = (landed > (int64_t)tt.At(i)) ? (int64_t)tt.At(i) : (int64_t)tt.At(i - 1);

    const int up = tr.StepTicks(1);
    const int64_t afterUp = tr.DisplayPlayheadUs();
    INFO("step +1: ", (long long)landed, " -> ", (long long)afterUp, " count ", up);
    CHECK(up == 1);
    CHECK(afterUp == next);

    const int down = tr.StepTicks(-1);
    const int64_t afterDown = tr.DisplayPlayheadUs();
    INFO("step -1: ", (long long)afterUp, " -> ", (long long)afterDown, " count ", down);
    CHECK(down == -1);
    // Back to the boundary the forward step left from -- NOT to the one before it, which is what the
    // launch printed (116632167 -> 116598834, two boundaries in one step).
    CHECK(afterDown == (int64_t)tt.At(i));
    CHECK(afterDown <= landed);
    CHECK(landed - afterDown < next - prev);
}

// ---------------------------------------------------------------------------------------------
// W2-B TICK TABLE ACCESSOR -- IReplayTransport::NearestTickUs / IsOnTickBoundary.
//
// W2-R's `w2r-step-moves-by-ticks` has four clauses and could only ask three: the interface
// published no way to say WHICH microseconds are recorded boundaries, so its landing clause scored
// `boundary-not-published` and the row passed on a landing nothing had checked. These cases are the
// evidence for the accessor that closes it.
//
// Every probe below obeys STEP2's rule -- it seeks to a target that is NOT a multiple of the tick
// spacing. A seek that lands square on a boundary makes "is this a boundary" true before the step
// under test even runs, and a fixture like that cannot tell a correct answer from a constant one.

namespace {

// The published interface over a real Transport and a real NON-ZERO origin. `IsOnTickBoundary` is
// not overridden here -- or anywhere -- so the body these cases exercise is transport.h's own
// derivation. The origin is what makes the axis load-bearing: an implementation that forgot to
// convert would hand back an absolute microsecond, which can never equal a playback-relative
// playhead, and every equality below would go red.
class TickHarness final : public replay::IReplayTransport {
public:
    TickHarness(replay::playback::Transport* t, int64_t origin) : t_(t), origin_(origin) {}

    bool Load(const wchar_t*) override { return false; }
    void Unload() override {}
    void Play(float) override {}
    void Pause() override {}
    void Step(int frames) override { last_ = t_->StepTicks(frames); }
    int LastStepTicks() const override { return last_; }
    void Seek(int64_t tUs) override { t_->SeekTo(tUs + origin_); }
    bool SeekRound(replay::RoundNumber) override { return false; }
    void SetMode(bool) override {}
    replay::TransportState State() const override { return replay::TransportState::Pause; }
    float Rate() const override { return (float)t_->Rate(); }
    int64_t PlayheadUs() const override { return t_->DisplayPlayheadUs() - origin_; }
    uint64_t DurationUs() const override { return 0; }
    const char* ActiveModeLabel() const override { return "tick harness"; }

    bool NearestTickUs(int64_t playheadUs, int64_t* outTickUs) const override {
        int64_t absUs = 0;
        if (!t_->TickAtOrBelow(playheadUs + origin_, &absUs)) {
            return false;
        }
        *outTickUs = absUs - origin_;
        return true;
    }

private:
    replay::playback::Transport* t_;
    int64_t origin_;
    int last_ = replay::kStepTicksUncounted;
};

}  // namespace

TEST_CASE("W2-B/TICKTABLE: on the REAL capture, a step lands on a boundary and NearestTickUs says so") {
    using namespace replay;

    DemoReader reader;
    char where[1024];
    const bool present = OpenL3Run5Capture(reader, where, sizeof where);
    INFO("capture: ", where);
    if (!present) {
        WARN_MESSAGE(false, "SKIPPED, capture absent: ", where);
        return;
    }

    const Manifest& m = reader.GetManifest();
    const int64_t t0 = (int64_t)m.t0Us;
    int64_t coreEnd = t0;
    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        const ChapterKey k = reader.ChapterAt(i);
        if (k.section == kSectionCore && (int64_t)k.tEndUs > coreEnd) {
            coreEnd = (int64_t)k.tEndUs;
        }
    }
    REQUIRE(coreEnd > t0);
    REQUIRE(t0 > 0);   // the real capture's origin, and it is what makes the axis observable

    const int64_t half = t0 + (coreEnd - t0) / 2;
    Window w;
    BuildStats bs;
    INFO("window refusal: ", bs.refusal ? bs.refusal : "none");
    REQUIRE(BuildWindowRange(reader, ChapterForTime(reader, half), w, bs));
    const TickTable& tt = w.Ticks();
    REQUIRE(tt.Count() > 2);

    TransportTuning tune;
    Transport tr;
    tr.Reset(tt, tune);
    tr.SetFileSpan(t0, coreEnd);
    TickHarness h(&tr, t0);

    tr.SeekTo(half);
    const size_t i0 = tt.IndexAt(tr.DisplayPlayheadUs());
    REQUIRE(i0 > 0);
    REQUIRE(i0 + 1 < tt.Count());
    const int64_t span = (int64_t)tt.At(i0 + 1) - (int64_t)tt.At(i0);
    REQUIRE(span > 1);
    tr.SeekTo((int64_t)tt.At(i0) + span / 2);   // strictly between two recorded boundaries

    int64_t tick = -1;
    REQUIRE(h.NearestTickUs(h.PlayheadUs(), &tick));
    INFO("mid-tick: playhead ", (long long)h.PlayheadUs(), " nearest ", (long long)tick);
    CHECK(tick == (int64_t)tt.At(i0) - t0);   // the boundary BELOW, not the nearer one above
    CHECK(tick < h.PlayheadUs());
    CHECK_FALSE(h.IsOnTickBoundary(h.PlayheadUs()));

    h.Step(1);
    CHECK(h.LastStepTicks() == 1);
    REQUIRE(h.NearestTickUs(h.PlayheadUs(), &tick));
    INFO("after +1: playhead ", (long long)h.PlayheadUs(), " nearest ", (long long)tick);
    CHECK(tick == h.PlayheadUs());
    CHECK(tick == (int64_t)tt.At(i0 + 1) - t0);
    CHECK(h.IsOnTickBoundary(h.PlayheadUs()));

    // Back to the boundary the forward step left from -- STEP2 S1, now asserted against the table
    // itself rather than against a microsecond delta.
    h.Step(-1);
    CHECK(h.LastStepTicks() == -1);
    REQUIRE(h.NearestTickUs(h.PlayheadUs(), &tick));
    INFO("after -1: playhead ", (long long)h.PlayheadUs(), " nearest ", (long long)tick);
    CHECK(tick == h.PlayheadUs());
    CHECK(tick == (int64_t)tt.At(i0) - t0);
    CHECK(h.IsOnTickBoundary(h.PlayheadUs()));

    // Spread over the whole resident table, both answers. An implementation right only near the
    // ends has nowhere to hide, and the negative half is counted so it cannot pass vacuously.
    const size_t stride = tt.Count() / 512 ? tt.Count() / 512 : 1;
    size_t probes = 0, onHit = 0, offEligible = 0, offHit = 0;
    for (size_t i = 1; i < tt.Count(); i += stride) {
        ++probes;
        const int64_t rel = (int64_t)tt.At(i) - t0;
        if (h.IsOnTickBoundary(rel)) {
            ++onHit;
        }
        if ((int64_t)tt.At(i) - (int64_t)tt.At(i - 1) >= 2) {
            ++offEligible;                                  // one us below is inside the tick below
            if (!h.IsOnTickBoundary(rel - 1)) {
                ++offHit;
            }
        }
    }
    INFO("probes ", (unsigned)probes, " on ", (unsigned)onHit, " offEligible ", (unsigned)offEligible,
         " off ", (unsigned)offHit);
    REQUIRE(probes > 100);
    CHECK(onHit == probes);
    REQUIRE(offEligible > 100);   // a negative half with no eligible probes proves nothing
    CHECK(offHit == offEligible);

    // Before the first recorded boundary there is none at or below: refused, and the out-parameter
    // is left alone so a caller that ignores the bool cannot come to hold a number nobody measured.
    int64_t sentinel = 0x5EED;
    const int64_t belowTable = (int64_t)tt.FirstUs() - t0 - 1;
    CHECK_FALSE(h.NearestTickUs(belowTable, &sentinel));
    CHECK(sentinel == 0x5EED);
    CHECK_FALSE(h.IsOnTickBoundary(belowTable));
}

TEST_CASE("W2-B/TICKTABLE: twelve chapters, a window rebuilt under the playhead, and the axis") {
    using namespace replay;

    TempDemo tmp;
    const uint64_t t0 = 5000000;   // non-zero: an unconverted absolute answer can never equal a playhead
    const uint64_t kNetId = 0x00000001ABCDEF01ull;
    // 2400 frames at the helper's own frame interval; the builder's 2 s chapters make that many more
    // than twelve, and the REQUIRE below is what actually establishes the count.
    const uint64_t dt = WriteMultiChapterDemo(tmp.path, t0, 2400, kNetId);
    REQUIRE(dt > 0);

    DemoReader reader;
    REQUIRE(reader.Open(tmp.path));
    const size_t cores = CountCoreChapters(reader);
    INFO("core chapters: ", cores);
    REQUIRE(cores >= 12);

    int64_t coreEnd = (int64_t)t0;
    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        const ChapterKey k = reader.ChapterAt(i);
        if (k.section == kSectionCore && (int64_t)k.tEndUs > coreEnd) {
            coreEnd = (int64_t)k.tEndUs;
        }
    }
    REQUIRE(coreEnd > (int64_t)t0);

    Window w;
    BuildStats bs;
    Transport tr;
    TransportTuning tune;
    TickHarness h(&tr, (int64_t)t0);

    // Three windows far enough apart in a twelve-chapter file that the resident table is a different
    // three chapters each time. The accessor must answer from the table the transport is stepping
    // over NOW -- Retable is the path the module takes on every rebuild.
    const int64_t fractions[3][2] = {{1, 6}, {1, 2}, {5, 6}};
    for (int f = 0; f < 3; ++f) {
        const int64_t target =
            (int64_t)t0 + (coreEnd - (int64_t)t0) * fractions[f][0] / fractions[f][1];
        INFO("probe ", f, " target ", (long long)target, " refusal ", bs.refusal ? bs.refusal : "none");
        REQUIRE(BuildWindowRange(reader, ChapterForTime(reader, target), w, bs));
        const TickTable& tt = w.Ticks();
        REQUIRE(tt.Count() > 2);
        if (f == 0) {
            tr.Reset(tt, tune);
            tr.SetFileSpan((int64_t)t0, coreEnd);
        } else {
            tr.Retable(tt);
        }

        tr.SeekTo(target);
        const size_t i0 = tt.IndexAt(tr.DisplayPlayheadUs());
        REQUIRE(i0 > 0);
        REQUIRE(i0 + 1 < tt.Count());
        const int64_t span = (int64_t)tt.At(i0 + 1) - (int64_t)tt.At(i0);
        REQUIRE(span > 1);
        tr.SeekTo((int64_t)tt.At(i0) + span / 2);
        CHECK_FALSE(h.IsOnTickBoundary(h.PlayheadUs()));

        for (int dir = 1; dir >= -1; dir -= 2) {
            h.Step(dir);
            CHECK(h.LastStepTicks() == dir);
            int64_t tick = -1;
            REQUIRE(h.NearestTickUs(h.PlayheadUs(), &tick));
            INFO("dir ", dir, " playhead ", (long long)h.PlayheadUs(), " tick ", (long long)tick);
            CHECK(h.IsOnTickBoundary(h.PlayheadUs()));
            CHECK(tick == h.PlayheadUs());
            // The answer is on the PLAYBACK axis; the boundary in the table is t0 higher.
            CHECK(tick + (int64_t)t0 == (int64_t)tt.At(dir > 0 ? i0 + 1 : i0));
        }
    }
}

TEST_CASE("W2-B/TICKTABLE: the lookup is O(log n) -- 16384 recorded ticks, under 2 us a call") {
    using namespace replay::playback;

    // 16384 is SESSION.FLAG_BIT_ACTIVE's value and 33333 is CLOCK.FIXEDSTEP_30HZ_US's, and the
    // fact scanner is right to ask about either: neither is that fact here. The count is spelled
    // as the shift it is, and the spacing is arbitrary -- a lookup over a sorted table does not
    // care what the cadence was, only how many boundaries it has to get past.
    static uint64_t boundaries[1 << 14];
    const uint64_t kOrigin = 5000000, kSpacing = 4001;
    const size_t kCount = sizeof boundaries / sizeof boundaries[0];
    for (size_t i = 0; i < kCount; ++i) {
        boundaries[i] = kOrigin + (uint64_t)i * kSpacing;
    }
    TickTable tt;
    tt.Set(boundaries, kCount);
    TransportTuning tune;
    Transport tr;
    tr.Reset(tt, tune);

    // Queried across the whole table, so a linear scan pays its average rather than its best case,
    // and every target sits strictly inside a tick so no query is answered by an equality shortcut.
    // The expected sum is computed independently and checked: it is the known answer that keeps the
    // loop from being folded away, and it fails first if the lookup is fast but wrong.
    int64_t expected = 0;
    for (size_t q = 0; q < kCount; ++q) {
        expected += (int64_t)boundaries[q];
    }

    LARGE_INTEGER freq, a, b;
    QueryPerformanceFrequency(&freq);
    int64_t sink = 0;
    size_t answered = 0;
    QueryPerformanceCounter(&a);
    for (size_t q = 0; q < kCount; ++q) {
        int64_t out = 0;
        const int64_t target = (int64_t)(kOrigin + (uint64_t)q * kSpacing + kSpacing / 2);
        if (tr.TickAtOrBelow(target, &out)) {
            sink += out;
            ++answered;
        }
    }
    QueryPerformanceCounter(&b);

    const double usPerCall =
        (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)freq.QuadPart / (double)kCount;
    INFO("us per lookup: ", usPerCall, " over ", (unsigned)kCount, " queries of a ", (unsigned)kCount,
         "-tick table");
    CHECK(answered == kCount);
    CHECK(sink == expected);
    CHECK(usPerCall < 2.0);
}


// -------------------------------------------------------------------------------------------
// L3-RUN-7 F4. `netvars: <state>` was published to the PANEL only, and the probe reads the LOG,
// so the could-not-ask improvement was invisible to the instrument that scores it. The state is
// now written once per load, through the same NetVarFidelityLine() the panel calls.
//
// playback_module.cpp is a module, not a library: it registers itself and holds one file-scope
// `g`, so this suite cannot call Load() to watch it write. What it CAN do is read the checkout it
// was built from and assert the call is on the load path -- which is what fails if the line is
// deleted, and what a copy of the sentence in this file could never notice.
namespace {

char* ReadModuleSource(size_t* outLen) {
    char here[] = __FILE__;
    for (char* c = here; *c != '\0'; ++c) {
        if (*c == '\\') {
            *c = '/';
        }
    }
    const char* tail = strstr(here, "tests/playback/test_playback.cpp");
    if (tail == nullptr) {
        return nullptr;
    }
    char path[sizeof __FILE__ + sizeof "src/playback/core/playback_module.cpp"];
    const size_t rootLen = (size_t)(tail - here);
    if (rootLen + sizeof "src/playback/core/playback_module.cpp" >= sizeof path) {
        return nullptr;
    }
    memcpy(path, here, rootLen);
    path[rootLen] = '\0';
    strcat_s(path, sizeof path, "src/playback/core/playback_module.cpp");
    FILE* fh = nullptr;
    if (fopen_s(&fh, path, "rb") != 0 || fh == nullptr) {
        return nullptr;
    }
    // Sized from the file, so this helper writes down no magnitude of its own.
    fseek(fh, 0, SEEK_END);
    const long len = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    if (len <= 0) {
        fclose(fh);
        return nullptr;
    }
    char* text = new char[(size_t)len + 1];
    const size_t got = fread(text, 1, (size_t)len, fh);
    fclose(fh);
    text[got] = '\0';
    *outLen = got;
    return text;
}

}  // namespace

TEST_CASE("the netvar fidelity state is logged once, on the load path, from the panel's function") {
    size_t len = 0;
    char* src = ReadModuleSource(&len);
    REQUIRE(src != nullptr);      // a failed read must not pass as a file with nothing in it

    const char* call = "\"%s\", NetVarFidelityLine())";
    size_t writes = 0;
    for (const char* at = strstr(src, call); at != nullptr; at = strstr(at + 1, call)) {
        ++writes;
    }
    // Once. A per-frame write of the same sentence would bury the log, and it would let a state
    // that changed after the load read as the state at the load.
    CHECK(writes == 1);

    const char* loaded = strstr(src, "\"loaded: %u chapters, %u players, duration %llu us%s\"");
    const char* unload = strstr(src, "void Unload() override");
    const char* site = strstr(src, call);
    REQUIRE(loaded != nullptr);
    REQUIRE(unload != nullptr);
    REQUIRE(site != nullptr);
    // On the load path: after the line that says a file loaded, before Unload begins.
    CHECK(site > loaded);
    CHECK(site < unload);

    // The sentence is the panel's own and the reader keys on its first words, so the two states a
    // playback session can publish at load must still start the way the row expects. Read from
    // apply.cpp's function, never transcribed.
    const char* noRecords = NetVarFidelityText(NetVarFidelityState::NoRecords);
    const char* notDue = NetVarFidelityText(NetVarFidelityState::PresentNotApplied);
    CHECK(strstr(noRecords, "netvars: no netvar records in this capture") == noRecords);
    CHECK(strstr(notDue, "netvars: records present, none due at this playhead") == notDue);

    delete[] src;
}

// ==================================================================================================
// SECOND HALF -- the recorder's own body binds by USER SLOT.
//
// W2-A fixed the capture side: GatherBodies walks the whole physics pool, so every net-driven body
// reaches the file. The recorder's OWN body reaches it too, and it arrives with `idValue == 0` and no
// identity fill at all, because it has no net-actor entry to name it. Before this case the bind
// refused exactly that shape, so a solo capture -- the only kind anyone has taken -- replayed every
// body except the viewer's own, which is the one the viewer is looking out of.
//
// The fixture is the shape a solo host actually has: four bodies, body 0 locally simulated with no
// net id, and ONE locally authored pawn whose user slot is the only name that body has.
// ==================================================================================================
namespace {

constexpr uint16_t kBindBodies = 4;
// Deliberately not 0 and not the capture-side pool index: a fixture where the slot, the pool index
// and the first-of-anything all coincide cannot fail a bind that took the first entry it found.
constexpr uint16_t kBindLocalSlot = 6;
constexpr uint64_t kBindNetBase = 0xB1D0000ull;
constexpr uint64_t kBindT0 = 5000000ull;
constexpr uint64_t kBindDt = 8333ull;

struct SoloCaptureEngine : replay::IEntity {
    uint32_t frame = 0;

    size_t PhysCount() const override { return kBindBodies; }
    bool IsLocallyControlled(PhysHandle h) const override { return h.v == 0; }
    void SetSyntheticOwner(PhysHandle) override {}
    void ClearSyntheticOwner(PhysHandle) override {}
    // EVERY body reads. B1's first half is about a body that never reached the file at all, and a
    // body that is not in the file cannot exercise a bind -- so this fake keeps that arm out of the
    // way and moves every body on its own line, at its own rate, from the frame counter.
    bool ReadBody(PhysHandle h, BodyState& out) const override {
        if (h.v >= kBindBodies) return false;
        out = BodyState{};
        const float k = float(h.v + 1);
        const float t = float(frame) * 0.01f;
        out.com[0] = 1.5f + k * t;
        out.com[1] = 2.5f - 0.5f * k * t;
        out.com[2] = -3.25f + 0.25f * k * t;
        out.ori[3] = 1.0f;
        out.vel[0] = k;
        out.vel[1] = -0.5f * k;
        out.vel[2] = 0.25f * k;
        out.angVel[0] = 3.0f * k;
        out.angVel[1] = -2.0f * k;
        out.angVel[2] = 5.0f * k;
        out.bits11 = h.v + 1u;
        return true;
    }
    void PushHistory(PhysHandle, const BodyState&, uint64_t) override {}
    void TrimHistory(PhysHandle, uint64_t, uint64_t) override {}
    bool SetHistoryWindow(PhysHandle, const BodyState*, const uint64_t*, size_t) override { return false; }
    size_t ReadHistoryWindow(PhysHandle, BodyState*, uint64_t*, size_t) const override { return 0; }
    float GetTimeOffset(PhysHandle) const override { return 0.0f; }
    void SetTimeOffset(PhysHandle, float) override {}
    size_t PlayerCount() const override { return 1; }
    bool ReadPlayer(PlayerHandle, PlayerState&) const override { return false; }
    void PushPlayerHistory(PlayerHandle, const PlayerState&, uint64_t) override {}
    PhysHandle PhysOf(PlayerHandle h) const override { return PhysHandle(h.v); }
    PlayerHandle PlayerOf(UserIndex u) const override { return PlayerHandle(u.v); }
    // 0 for body 0 is the engine's honest answer for a body with no net-actor entry, not a gap.
    uint64_t NetIdOf(PhysHandle h) const override {
        return h.v == 0 ? 0ull : kBindNetBase + h.v;
    }
    PhysHandle PhysOfNetId(uint64_t id) const override {
        for (uint16_t i = 1; i < kBindBodies; ++i) {
            if (kBindNetBase + i == id) return PhysHandle(i);
        }
        return PhysHandle(UINT16_MAX);
    }
    size_t LocalPlayerCount() const override { return 1; }
    UserIndex LocalPlayerAt(size_t) const override { return UserIndex(kBindLocalSlot); }
    uint32_t ReadLocalPlayer(UserIndex, PlayerState& out) const override {
        out = PlayerState{};
        out.hmdOri[3] = 1.0f;
        out.refVel[0] = 0.5f + float(frame) * 0.001f;
        return kLocalPose | kLocalRefVel;
    }
    void SetLodThrottle(bool) override {}
    bool Available() const override { return true; }
    const char* Diagnostic() const override { return "solo-capture fake"; }
};

// The same real gather / real builder chain WriteChainDemo uses, over the solo pool.
replay::capture::GatherReport WriteSoloDemo(const wchar_t* path, uint32_t frames) {
    using namespace replay::capture;
    using namespace replay;
    ManifestBuilder mb;
    mb.Begin(kBindT0, 0, 0x40);
    const uint8_t md5[16] = {0xca, 0xa0, 0x5e, 0xe3};
    mb.SetBuild(md5, 0x676F6C646D617374ull, 0x0102030405060708ull);
    mb.SetLevel(6300205991959903307ull, 0xF00Dull, 1780177787ull);
    mb.SetKeyframeIntervalUs(2000000);
    mb.SetUuid(0xB1, 0xB1);
    PlayerRecord p{};
    p.xplatformId = 0x4111;
    p.userSlot = kBindLocalSlot;
    p.userIndex = kBindLocalSlot;
    mb.AddPlayer(p);
    REQUIRE(mb.Refusal() == nullptr);

    DemoWriter writer;
    REQUIRE(writer.Open(path, mb.View()));
    BuilderConfig cfg;
    cfg.keyframeIntervalUs = 2000000;
    for (uint16_t i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;
    for (uint16_t i = 0; i < kSectionCount; ++i) cfg.sectionEnabled[i] = true;
    ChapterBuilder b;
    REQUIRE(b.Open(&writer, mb.View(), cfg));
    b.EmitMatchBegin(kBindT0);

    GatherConfig gcfg{};
    for (uint16_t i = 0; i < 32; ++i) gcfg.streamEnabled[i] = true;
    GatherReport rep;
    SoloCaptureEngine eng;

    for (uint32_t f = 0; f < frames; ++f) {
        eng.frame = f;
        const uint64_t t = kBindT0 + (uint64_t)f * kBindDt;
        GatherBodies(&eng, gcfg, t, f + 1, ChainSink, &b, rep);
        GatherLocalPawns(&eng, gcfg, t, f + 1, ChainSink, &b, rep);
        TickRec tr{};
        tr.sourceTimeUs = t;
        tr.simTimeUs = 1000000 + (uint64_t)f * kBindDt;
        tr.frameIndex = f + 1;
        tr.frameDeltaUs = (uint32_t)kBindDt;
        tr.scaledDeltaUs = (uint32_t)kBindDt;
        Feed(b, kRecTick, tr);
    }
    b.EmitMatchEnd(kBindT0 + (uint64_t)frames * kBindDt);
    b.FlushChapter();
    b.Close();
    writer.Close();
    return rep;
}

}  // namespace

TEST_CASE("BODYBIND: the recorder's own body binds by user slot and the pawn moves") {
    TempDemo tmp;
    static const uint32_t kFrames = 400;
    const replay::capture::GatherReport rep = WriteSoloDemo(tmp.path, kFrames);

    // The fixture reached the file with the shape the bind is about: four bodies present, all four
    // sampled, and one of them WITHOUT a net id. If that last flag were false the netId-0 arm would
    // never be entered and every assertion below would pass on the net-id path alone.
    CHECK(rep.bodiesPresent == kBindBodies);
    CHECK(rep.bodiesSampled == kBindBodies * kFrames);
    CHECK(rep.bodiesRefused == 0);
    CHECK(rep.sawBodyWithoutNetId);
    CHECK(rep.sinkRefusals == 0);

    replay::DemoReader reader;
    REQUIRE(reader.Open(tmp.path));

    // The replay machine. Its pool positions deliberately differ from the capture's, so a bind that
    // carried a handle through the file lands on the wrong body; and the viewer's OWN pawn -- the one
    // user slot kBindLocalSlot resolves to -- is locally simulated here, exactly as it was there.
    FakeEntity ent;
    ent.net_ids[21] = kBindNetBase + 1;
    ent.net_ids[9] = kBindNetBase + 2;
    ent.net_ids[17] = kBindNetBase + 3;
    ent.locally_controlled[kBindLocalSlot] = true;
    FakePose pose;
    FakeNetVar nv;
    nv.accept = false;
    OwnerGuard og;
    og.Reset(3, UINT32_MAX);

    StateApplier ap;
    ap.Bind(&ent, &pose, &nv, &og);

    Window w;
    BuildStats bs;
    const int64_t t_a = (int64_t)(kBindT0 + 40ull * kBindDt);
    const size_t centre = ChapterForTime(reader, t_a);
    REQUIRE(BuildWindowRange(reader, centre, w, bs));
    INFO("refusal: ", bs.refusal ? bs.refusal : "none");
    REQUIRE(bs.decode_failures == 0);

    // ---- 1. FOUR, not three ---------------------------------------------------------------------
    const BindResult r = BindBodyHandles(ent, w);
    INFO("bound ", r.bound, " by_slot ", r.bound_by_slot, " unresolvable ", r.unresolvable,
         " ambiguous ", r.local_pawn_ambiguous, " no_player_ci ", r.no_player_ci,
         " not_net ", r.not_a_net_id, " bodies ", w.BodyCount(), " players ", w.PlayerCount());
    REQUIRE(w.BodyCount() == kBindBodies);
    REQUIRE(w.PlayerCount() == 1);
    CHECK(r.bound == kBindBodies);
    CHECK(r.bound_by_slot == 1);
    CHECK(r.unresolvable == 0);
    CHECK(r.local_pawn_ambiguous == 0);

    // THE LINE, not only the struct. `bound_by_slot` is what a launch reads out of the log, and a
    // counter incremented into a field the line never prints is unreadable from outside the process
    // -- which is the state this assertion is here to keep closed.
    // Sized off the log's own cap (bootstrap/log.h kLogLineMax is 512), not off a number that
    // happened to fit the line the day it was written: a field added mid-line must not make this
    // assertion fail for the reason it exists to catch.
    char summary[512] = {};
    FormatBindSummary(r, summary, sizeof summary);
    INFO("summary: ", summary);
    CHECK(strstr(summary, "4 bound (1 by slot 6->6, class=remote, ent=?)") != nullptr);
    CHECK(strstr(summary, "0 local pawn ambiguous") != nullptr);
    CHECK(r.no_player_ci == 0);
    CHECK(r.no_phys_ci == 0);

    // The slot-bound track landed on the pawn the ROSTER slot names, not on a pool index that
    // happened to match the capture's.
    size_t local_track = (size_t)-1;
    for (size_t i = 0; i < w.BodyCount(); ++i) {
        if (w.BodyAt(i).bound_by_slot) local_track = i;
    }
    REQUIRE(local_track != (size_t)-1);
    CHECK(w.BodyAt(local_track).user_slot == kBindLocalSlot);
    CHECK(w.BodyAt(local_track).handle.v == kBindLocalSlot);

    // ---- 2. the applier DRIVES it, through the engine's own owner claim -------------------------
    const ApplyStats st_a = ap.Apply(w, t_a, ApplyOptions());
    INFO("installed ", st_a.bodies_installed, " locally ", st_a.bodies_locally_controlled,
         " claimed ", st_a.bodies_owner_claimed, " unhandled ", st_a.bodies_unhandled,
         " absent ", st_a.bodies_absent);
    CHECK(st_a.bodies_installed == kBindBodies);
    CHECK(st_a.bodies_locally_controlled == 0);
    CHECK(st_a.bodies_owner_claimed == 1);
    CHECK(ent.synthetic_owner_calls[kBindLocalSlot] == 1);
    REQUIRE(ent.slots[kBindLocalSlot].installed);

    // com, vel AND angVel -- the three the goal's "physics, EVERYTHING" is carried by. Compared at
    // the wire's own quantisation, because that is the fidelity the format claims and an exact float
    // compare would be testing the codec's rounding rather than the bind.
    {
        SoloCaptureEngine src;
        src.frame = 40;
        BodyState want{};
        REQUIRE(src.ReadBody(PhysHandle(0), want));
        const BodyState& got = ent.slots[kBindLocalSlot].s;
        for (int k = 0; k < 3; ++k) {
            INFO("component ", k);
            CHECK(QCode(got.com[k], kStepCom) == QCode(want.com[k], kStepCom));
            CHECK(QCode(got.vel[k], kStepVel) == QCode(want.vel[k], kStepVel));
            CHECK(QCode(got.angVel[k], kStepAngVel) == QCode(want.angVel[k], kStepAngVel));
        }
    }
    const BodyState at_a = ent.slots[kBindLocalSlot].s;

    // ---- 3. THE PAWN MOVES between two playheads -----------------------------------------------
    // This is what L3 #8 has to show on a screenshot pair, asserted here on the same state the
    // screenshot would be of. A bind that resolves and an applier that installs a FROZEN pose are
    // the same picture, so the position has to be read twice.
    const int64_t t_b = (int64_t)(kBindT0 + 200ull * kBindDt);
    // doctest decomposes ONE binary comparison; a boolean operator inside REQUIRE is a compile
    // error, so the disjunction is named here rather than spelled in the macro.
    const bool reachable = w.CoversForSampling(t_b) || w.NeedsRebuild(t_b);
    REQUIRE(reachable);
    if (w.NeedsRebuild(t_b)) {
        REQUIRE(BuildWindowRange(reader, ChapterForTime(reader, t_b), w, bs));
        const BindResult r2 = BindBodyHandles(ent, w);
        CHECK(r2.bound == kBindBodies);
        CHECK(r2.bound_by_slot == 1);
    }
    const ApplyStats st_b = ap.Apply(w, t_b, ApplyOptions());
    CHECK(st_b.bodies_installed == kBindBodies);
    // The claim is made ONCE per track: the engine answers "not locally controlled" afterwards, so a
    // second claim here would mean it did not stick.
    CHECK(ent.synthetic_owner_calls[kBindLocalSlot] == 1);

    const BodyState at_b = ent.slots[kBindLocalSlot].s;
    CHECK(QCode(at_b.com[0], kStepCom) != QCode(at_a.com[0], kStepCom));
    CHECK(QCode(at_b.com[1], kStepCom) != QCode(at_a.com[1], kStepCom));
    CHECK(QCode(at_b.com[2], kStepCom) != QCode(at_a.com[2], kStepCom));
    CHECK(ent.slots[kBindLocalSlot].installs >= 2);
}


// =================================================================================================
// ARCH-23 W2-P2 -- THE RESOLVE, OVER BOTH SURFACES.
//
// L3 #11 bound nothing and could say only `1 absent this session`. Two different failures wear that
// sentence -- no CI for the slot in either pool, and a CI that carries no body -- and a third was
// invisible inside it: the binder asked ONE pool, the remote one, for a pawn the capture had read
// out of the local one (F-A / F-B). These cases are written on the refusal WORD, because the word
// is the thing that run could not produce.
// =================================================================================================

namespace {

// Every number these cases use, named once. They are fixture data, not engine values: the slots
// differ from the handles they resolve to on purpose, so a resolver that returned a slot index or
// the first CI in the pool could not pass a single case below.
constexpr uint16_t kPrSlot = 5;          // the recorded slot most cases resolve
constexpr uint16_t kPrSlotEmpty = 3;     // a slot neither surface carries
constexpr uint16_t kPrSlotA = 4;         // two recorded players...
constexpr uint16_t kPrSlotB = 9;
constexpr uint16_t kPrSlotFree = 6;      // ...and a slot neither of them occupies
constexpr uint16_t kPrPhys = 12;         // pool handles, none equal to its slot
constexpr uint16_t kPrPhysAfterEpoch = 20;
constexpr uint16_t kPrPhysA = 11;
constexpr uint16_t kPrPhysB = 22;
constexpr uint16_t kPrOtherSlot = 7;
constexpr uint16_t kPrOtherPhys = 3;
constexpr uint8_t  kPrCap = 3;           // an attempt cap small enough to spend inside a case
constexpr int      kPrRebuilds = 24;     // L3 #11 saw 25 window rebuilds in four minutes
constexpr int      kPrUncachedAsks = 4;
constexpr uint64_t kPrSampleT = std::milli::den;  // one millisecond into the window, in us
constexpr uint64_t kPrEpochTwo = 2;
constexpr size_t   kPrSummaryCap = 288;
// The two surfaces answer with DIFFERENT keys for one slot on purpose: a resolver that asked the
// surface it did not bind through would still hand back a number, and equal keys would hide it.
constexpr uint16_t kPrRemoteEntity = 41;
// Entity id 0 is the local pawn's by design, and the local pawn is what a launch looks at. It is
// planted here because a build that spelled absence 0 would report this resolved pawn as unknown.
constexpr uint16_t kPrLocalEntity = 0;

// A row as it arrives from the file. The handles are seeded ONE BELOW the absent word rather than
// AT it: ResolvePawn clears its out-parameter on entry, so a resolver that merely left the caller's
// ref alone on a refusal would satisfy every "== kPawnAbsent" assertion below if the fixture had
// supplied that word itself.
replay::PawnRef UnresolvedPawnRef() {
    const uint16_t seed = uint16_t(replay::playback::kPawnAbsent - 1);
    return replay::PawnRef{PlayerHandle(seed), PhysHandle(seed), 0u, replay::PawnClass::Unknown,
                           false};
}

// The L3 #11 window shape, built directly rather than through a file: ONE body track with no
// identity fill -- the recorder's own body, exactly as gather.h writes it -- and ONE player track at
// the capture slot. BuildWindowRange has its own cases; what these need is the bind.
void MakeSlotPairedWindow(replay::playback::Window& w, uint16_t slot) {
    w.Clear();
    replay::PlayerState ps{};
    REQUIRE(w.AddPlayerSample(slot, kPrSampleT, ps));
    replay::playback::BodyIdentity id{};
    id.fill = 0;
    id.tag = 0;
    id.value = 0;
    replay::BodyState bs{};
    REQUIRE(w.AddBodySample(id, kPrSampleT, bs));
    REQUIRE(w.BodyCount() == 1);
    REQUIRE(w.PlayerCount() == 1);
}

}  // namespace

TEST_CASE("pawn resolve on the L3 #11 shape names WHICH surface refused") {
    FakeEntity ent;
    // Neither surface carries a pawn for the slot: the remote pool refuses and the broadcaster pool
    // does not hold the slot either. This is the state the L3 #11 launch was in.
    for (size_t i = 0; i < FakeEntity::kSlots; ++i) ent.remote_absent[i] = true;

    Window w;
    MakeSlotPairedWindow(w, kPrSlotEmpty);
    const BindResult r = BindBodyHandles(ent, w);
    INFO("bound ", r.bound, " no_player_ci ", r.no_player_ci, " no_phys_ci ", r.no_phys_ci,
         " unresolvable ", r.unresolvable);
    CHECK(r.bound == 0);
    CHECK(r.bound_by_slot == 0);
    // THE WORD, not the count. `1 unresolvable` and `1 absent` would both be true of this run, and
    // neither says that no pool holds a CI for the slot.
    CHECK(r.no_player_ci == 1);
    CHECK(r.no_phys_ci == 0);
    CHECK(r.unresolvable == 0);
    CHECK(r.class_mismatch == 0);
    CHECK(r.slot_class == replay::PawnClass::Unknown);

    char summary[kPrSummaryCap] = {};
    FormatBindSummary(r, summary, sizeof summary);
    INFO("summary: ", summary);
    CHECK(strstr(summary, "1 no player ci") != nullptr);
    CHECK(strstr(summary, "0 no phys ci") != nullptr);
    // The retired sentence must not come back under any spelling: it is the merge this packet undid.
    CHECK(strstr(summary, "absent this session") == nullptr);

    // A CI that exists and carries no body is a DIFFERENT run, and it says so.
    FakeEntity ent2;
    ent2.remote_no_phys[kPrSlotEmpty] = true;
    Window w2;
    MakeSlotPairedWindow(w2, kPrSlotEmpty);
    const BindResult r2 = BindBodyHandles(ent2, w2);
    CHECK(r2.bound == 0);
    CHECK(r2.no_phys_ci == 1);
    CHECK(r2.no_player_ci == 0);
}

TEST_CASE("pawn resolve binds through the REMOTE surface and says so") {
    FakeEntity ent;                       // the default: the remote pool answers for every slot
    Window w;
    MakeSlotPairedWindow(w, kPrSlot);
    const BindResult r = BindBodyHandles(ent, w);
    CHECK(r.bound == 1);
    CHECK(r.bound_by_slot == 1);
    CHECK(r.slot_class == replay::PawnClass::Remote);
    CHECK(w.BodyAt(0).handle.v == 5);
    char summary[kPrSummaryCap] = {};
    FormatBindSummary(r, summary, sizeof summary);
    INFO("summary: ", summary);
    CHECK(strstr(summary, "1 bound (1 by slot 5->5, class=remote, ent=?)") != nullptr);
}

TEST_CASE("pawn resolve binds through the LOCAL surface when the remote pool has nothing") {
    // F-B's case, and the one the capture actually recorded through: the pawn is in the broadcaster
    // pool and not in the remote one. Before this packet the binder asked only the remote pool, so
    // this run bound nothing and blamed the session for it.
    FakeEntity ent;
    for (size_t i = 0; i < FakeEntity::kSlots; ++i) ent.remote_absent[i] = true;
    ent.local_pawns[kPrSlot].present = true;
    ent.local_pawns[kPrSlot].phys = kPrPhys;

    Window w;
    MakeSlotPairedWindow(w, kPrSlot);
    const BindResult r = BindBodyHandles(ent, w);
    CHECK(r.bound == 1);
    CHECK(r.bound_by_slot == 1);
    CHECK(r.slot_class == replay::PawnClass::Local);
    // The handle came from the LOCAL surface, not from the slot number: they differ on purpose, so
    // a resolver that fell back to the slot would look identical to one that asked.
    CHECK(w.BodyAt(0).handle.v == 12);
    char summary[kPrSummaryCap] = {};
    FormatBindSummary(r, summary, sizeof summary);
    CHECK(strstr(summary, "1 bound (1 by slot 5->5, class=local, ent=?)") != nullptr);
}

TEST_CASE("pawn resolve prefers the surface the capture reads when BOTH answer") {
    // FACT-PP1 is unmeasured, so both pools holding a CI for one slot is a state this build must
    // handle deterministically rather than by lookup order. The capture reads the local surface, so
    // the local surface wins -- INV-PP2 applied to a file that cannot name its own class.
    FakeEntity ent;
    ent.local_pawns[kPrSlot].present = true;
    ent.local_pawns[kPrSlot].phys = kPrPhys;

    Window w;
    MakeSlotPairedWindow(w, kPrSlot);
    const BindResult r = BindBodyHandles(ent, w);
    CHECK(r.bound == 1);
    CHECK(r.slot_class == replay::PawnClass::Local);
    CHECK(w.BodyAt(0).handle.v == 12);
}

TEST_CASE("pawn resolve refuses a class mismatch instead of driving the wrong CI") {
    // INV-PP2. The row says it was captured from a REMOTE CI and only the local surface answers:
    // driving it would apply the row to a class of CI it was never read from.
    FakeEntity ent;
    for (size_t i = 0; i < FakeEntity::kSlots; ++i) ent.remote_absent[i] = true;
    ent.local_pawns[kPrSlot].present = true;
    ent.local_pawns[kPrSlot].phys = kPrPhys;

    PawnResolver res;
    replay::PawnRef ref = UnresolvedPawnRef();
    CHECK_FALSE(res.ResolvePawn(ent, kPrSlot, replay::PawnClass::Remote, ref));
    CHECK(res.LastResolveRefusal() == replay::PawnResolve::Refusal::PawnClassMismatch);
    CHECK(ref.phys.v == replay::playback::kPawnAbsent);

    // The same session and the same slot, with a row that names the class that DID answer.
    replay::PawnRef ok = UnresolvedPawnRef();
    CHECK(res.ResolvePawn(ent, kPrSlot, replay::PawnClass::Local, ok));
    CHECK(ok.cls == replay::PawnClass::Local);
    CHECK(ok.phys.v == 12);
}

TEST_CASE("pawn resolve carries the entity key of the surface that ANSWERED") {
    FakeEntity ent;                       // the default: the remote pool answers for every slot
    ent.remote_entity[kPrSlot].ok = true;
    ent.remote_entity[kPrSlot].key = kPrRemoteEntity;
    ent.local_entity[kPrSlot].ok = true;
    ent.local_entity[kPrSlot].key = kPrLocalEntity;

    PawnResolver res;
    replay::PawnRef remote = UnresolvedPawnRef();
    REQUIRE(res.ResolvePawn(ent, kPrSlot, replay::PawnClass::Remote, remote));
    CHECK(remote.cls == replay::PawnClass::Remote);
    CHECK(remote.entity == kPrRemoteEntity);

    // The same slot on the other surface, with the remote key still planted: a resolver reading the
    // surface it did not bind through would hand back 41 here.
    FakeEntity lent;
    for (size_t i = 0; i < FakeEntity::kSlots; ++i) lent.remote_absent[i] = true;
    lent.local_pawns[kPrSlot].present = true;
    lent.local_pawns[kPrSlot].phys = kPrPhys;
    lent.remote_entity[kPrSlot].ok = true;
    lent.remote_entity[kPrSlot].key = kPrRemoteEntity;
    lent.local_entity[kPrSlot].ok = true;
    lent.local_entity[kPrSlot].key = kPrLocalEntity;

    PawnResolver res2;
    replay::PawnRef local = UnresolvedPawnRef();
    REQUIRE(res2.ResolvePawn(lent, kPrSlot, replay::PawnClass::Local, local));
    CHECK(local.cls == replay::PawnClass::Local);
    CHECK(local.entity == kPrLocalEntity);
    CHECK(local.entity != remote.entity);

    // ...and the bind prints the digits, so a run states the entity it drove.
    Window w;
    MakeSlotPairedWindow(w, kPrSlot);
    const BindResult r = BindBodyHandles(ent, w);
    REQUIRE(r.bound_by_slot == 1);
    CHECK(r.slot_entity == kPrRemoteEntity);
    char summary[kPrSummaryCap] = {};
    FormatBindSummary(r, summary, sizeof summary);
    INFO("summary: ", summary);
    CHECK(strstr(summary, "class=remote, ent=41)") != nullptr);
    // The tail must still be there: the key is a column, not a truncation.
    CHECK(strstr(summary, "0 local pawn ambiguous") != nullptr);
}

TEST_CASE("entity id 0 is a KEY on the local surface, not an absence") {
    // The one the launch will look at. A build spelling absence 0 reports the viewer's own pawn as
    // never-asked, and the census that reads it cannot tell FACT-PP1's arms apart.
    FakeEntity ent;
    for (size_t i = 0; i < FakeEntity::kSlots; ++i) ent.remote_absent[i] = true;
    ent.local_pawns[kPrSlot].present = true;
    ent.local_pawns[kPrSlot].phys = kPrPhys;
    ent.local_entity[kPrSlot].ok = true;
    ent.local_entity[kPrSlot].key = kPrLocalEntity;

    Window w;
    MakeSlotPairedWindow(w, kPrSlot);
    const BindResult r = BindBodyHandles(ent, w);
    REQUIRE(r.bound_by_slot == 1);
    REQUIRE(r.slot_class == replay::PawnClass::Local);
    CHECK(r.slot_entity == 0u);
    CHECK(r.slot_entity != replay::kEntityKeyUnknown);
    char summary[kPrSummaryCap] = {};
    FormatBindSummary(r, summary, sizeof summary);
    INFO("summary: ", summary);
    CHECK(strstr(summary, "class=local, ent=0)") != nullptr);
    CHECK(strstr(summary, "ent=?") == nullptr);
}

TEST_CASE("a surface publishing no entity key leaves the key UNKNOWN, never 0") {
    // Nothing planted, so both surfaces refuse the key while the remote pool still binds the pawn.
    // The seeded ref carries 0, which is what a resolver that never wrote the field would leave.
    FakeEntity ent;
    PawnResolver res;
    replay::PawnRef ref = UnresolvedPawnRef();
    REQUIRE(res.ResolvePawn(ent, kPrSlot, replay::PawnClass::Remote, ref));
    CHECK(ref.entity == replay::kEntityKeyUnknown);
    CHECK(ref.entity != 0u);
}

TEST_CASE("pawn resolve refuses two CIs claiming one slot") {
    FakeEntity ent;
    for (size_t i = 0; i < FakeEntity::kSlots; ++i) ent.remote_absent[i] = true;
    ent.local_pawns[kPrSlot].present = true;
    ent.local_pawns[kPrSlot].phys = kPrPhys;
    ent.local_pawns[kPrSlot].duplicate = true;   // a second pool element carries the same user slot

    PawnResolver res;
    replay::PawnRef ref = UnresolvedPawnRef();
    CHECK_FALSE(res.ResolvePawn(ent, kPrSlot, replay::PawnClass::Unknown, ref));
    CHECK(res.LastResolveRefusal() == replay::PawnResolve::Refusal::SlotCollision);
    CHECK(ref.phys.v == replay::playback::kPawnAbsent);
}

TEST_CASE("pawn resolve reports a pawn with no model as bound and INVISIBLE") {
    FakeEntity ent;
    for (size_t i = 0; i < FakeEntity::kSlots; ++i) ent.remote_absent[i] = true;
    ent.local_pawns[kPrSlot].present = true;
    ent.local_pawns[kPrSlot].phys = kPrPhys;
    ent.local_pawns[kPrSlot].model = false;

    Window w;
    MakeSlotPairedWindow(w, kPrSlot);
    const BindResult r = BindBodyHandles(ent, w);
    // Bound, because a body that cannot be seen still has to be driven -- and counted, because a
    // launch whose replay is invisible must be able to say that is what happened.
    CHECK(r.bound == 1);
    CHECK(r.no_model == 1);
    char summary[kPrSummaryCap] = {};
    FormatBindSummary(r, summary, sizeof summary);
    CHECK(strstr(summary, "1 no model") != nullptr);
}

TEST_CASE("pawn resolve is once per slot per bind epoch, not once per rebuild") {
    // ARCH-23 section 8's cost bound. The binder re-runs on every window rebuild -- L3 #11 shows 25
    // of them in four minutes -- and each rebuild scanning both pools for every player is the
    // per-tick cost this design refuses. The fake COUNTS the asks, so a cache that is not a cache
    // fails here rather than in a profile.
    FakeEntity ent;
    ent.publish_epoch = true;
    ent.local_pawns[kPrSlot].present = true;
    ent.local_pawns[kPrSlot].phys = kPrPhys;

    PawnResolver res;
    replay::PawnRef ref = UnresolvedPawnRef();
    CHECK(res.ResolvePawn(ent, kPrSlot, replay::PawnClass::Unknown, ref));
    CHECK(ent.local_asks[kPrSlot] == 1);

    for (int i = 0; i < kPrRebuilds; ++i) CHECK(res.ResolvePawn(ent, kPrSlot, replay::PawnClass::Unknown, ref));
    CHECK(ent.local_asks[kPrSlot] == 1);                   // 24 rebuilds, no second lookup
    CHECK(res.GetStats().cache_hits == size_t(kPrRebuilds));
    CHECK(ref.phys.v == 12);

    // The epoch moves: every handle issued against the old game space is garbage, so the answer is
    // re-resolved rather than served out of the table.
    ent.epoch = kPrEpochTwo;
    ent.local_pawns[kPrSlot].phys = kPrPhysAfterEpoch;
    CHECK(res.ResolvePawn(ent, kPrSlot, replay::PawnClass::Unknown, ref));
    CHECK(ent.local_asks[kPrSlot] == 2);
    CHECK(res.GetStats().epoch_changes == 1);
    CHECK(ref.phys.v == 20);

    // An IEntity that publishes NO epoch has no invalidation signal, so it is never cached: a
    // cached handle with nothing to invalidate it is a stale pool index waiting for a rebuild.
    FakeEntity noepoch;
    noepoch.local_pawns[kPrOtherSlot].present = true;
    noepoch.local_pawns[kPrOtherSlot].phys = kPrOtherPhys;
    PawnResolver res2;
    for (int i = 0; i < kPrUncachedAsks; ++i) CHECK(res2.ResolvePawn(noepoch, kPrOtherSlot, replay::PawnClass::Unknown, ref));
    CHECK(noepoch.local_asks[kPrOtherSlot] == kPrUncachedAsks);
    CHECK(res2.GetStats().cache_hits == 0);
    CHECK(res2.GetStats().uncached == size_t(kPrUncachedAsks));
}

TEST_CASE("pawn resolve latches a refusal after the attempt cap, within the epoch") {
    // A permanently absent pawn must not spend two pool lookups on every rebuild forever. The cap
    // is per player per epoch and it latches the SAME word: a cap changes how often we ask, never
    // why the answer was no.
    FakeEntity ent;
    ent.publish_epoch = true;
    for (size_t i = 0; i < FakeEntity::kSlots; ++i) ent.remote_absent[i] = true;

    PawnResolver res;
    res.SetAttemptCap(kPrCap);
    replay::PawnRef ref = UnresolvedPawnRef();
    for (int i = 0; i < 10; ++i) {
        CHECK_FALSE(res.ResolvePawn(ent, kPrSlot, replay::PawnClass::Unknown, ref));
        CHECK(res.LastResolveRefusal() == replay::PawnResolve::Refusal::NoPlayerCi);
    }
    CHECK(ent.remote_asks[kPrSlot] == size_t(kPrCap));
    CHECK(ent.local_asks[kPrSlot] == size_t(kPrCap));
    CHECK(res.GetStats().surface_lookups == size_t(kPrCap));
    CHECK(res.GetStats().latched == 10u - size_t(kPrCap));

    // The epoch moving is what re-arms it: a new game space is a new answer, so a cap spent against
    // the old one must not silence the new one.
    ent.epoch = kPrEpochTwo;
    ent.remote_absent[kPrSlot] = false;
    CHECK(res.ResolvePawn(ent, kPrSlot, replay::PawnClass::Unknown, ref));
    CHECK(ent.remote_asks[kPrSlot] == size_t(kPrCap) + 1);
    CHECK(ref.cls == replay::PawnClass::Remote);
}

TEST_CASE("two players two pawns, and neither body lands on the other's handle") {
    // The `local_pawn_ambiguous` guard cannot give this once N > 1, and every capture so far is
    // N == 1. Two slots, two pawns, and the resolver must key each on ITS slot: a resolver that
    // fell back to the first CI in the pool would pass every N == 1 case in this file.
    FakeEntity ent;
    for (size_t i = 0; i < FakeEntity::kSlots; ++i) ent.remote_absent[i] = true;
    ent.local_pawns[kPrSlotA].present = true;
    ent.local_pawns[kPrSlotA].phys = kPrPhysA;
    ent.local_pawns[kPrSlotB].present = true;
    ent.local_pawns[kPrSlotB].phys = kPrPhysB;

    PawnResolver res;
    replay::PawnRef a = UnresolvedPawnRef();
    replay::PawnRef b = UnresolvedPawnRef();
    CHECK(res.ResolvePawn(ent, kPrSlotA, replay::PawnClass::Unknown, a));
    CHECK(res.ResolvePawn(ent, kPrSlotB, replay::PawnClass::Unknown, b));
    CHECK(a.phys.v == 11);
    CHECK(b.phys.v == 22);
    CHECK(a.phys.v != b.phys.v);

    // And a slot NEITHER pawn occupies resolves to nothing rather than to whichever CI is first.
    replay::PawnRef c = UnresolvedPawnRef();
    CHECK_FALSE(res.ResolvePawn(ent, kPrSlotFree, replay::PawnClass::Unknown, c));
    CHECK(res.LastResolveRefusal() == replay::PawnResolve::Refusal::NoPlayerCi);
    CHECK(c.phys.v == replay::playback::kPawnAbsent);
}

// L3 #22 F3.  The engine printed `Player Actor Allocating` for slot 1 and the bind still reported
// `1 no phys ci`: the component instance that carries the body is built AFTER the actor, so a pass
// that finds a PlayerCI with no body has not proved the body absent -- it has arrived early.  That
// pass must therefore not spend the permanent-absence budget, and the run must be able to say which
// of the two it is looking at.  Bounded by the same cap on its own counter: a body that never comes
// does stop being asked for, and the word goes back to `no phys ci` when it does.
TEST_CASE("a CI with no body yet is AWAITED and re-asked, and the wait is capped") {
    using replay::playback::PawnResolver;
    constexpr uint16_t kAwSlot = 4;
    constexpr uint8_t  kAwCap  = 3;

    FakeEntity ent;
    ent.publish_epoch = true;
    // The remote pool holds a CI for the slot and PhysOf answers absence: NoPhysCi, on the surface
    // the engine had just allocated an actor on.  The local pool holds nothing, so the deeper of
    // the two words is the one under test.
    ent.remote_no_phys[kAwSlot] = true;

    PawnResolver res;
    res.SetAttemptCap(kAwCap);
    replay::PawnRef ref = UnresolvedPawnRef();

    // Every pass walks the surface: the wait does not latch and does not consume the absence cap.
    // `awaiting` is true while the wait BUDGET is unspent, so the cap-th wait is the one that
    // spends it -- the slot is still re-asked after that, under the plain absence budget.
    for (uint8_t i = 0; i < kAwCap; ++i) {
        CHECK_FALSE(res.ResolvePawn(ent, kAwSlot, replay::PawnClass::Unknown, ref));
        CHECK(res.LastResolveRefusal() == replay::PawnResolve::Refusal::NoPhysCi);
        CHECK(res.AwaitingPhysCi(kAwSlot) == (i + 1 < kAwCap));
        CHECK(ent.remote_asks[kAwSlot] == size_t(i) + 1);
    }
    CHECK(res.GetStats().latched == 0);

    // The body arrives a frame later -- WITHOUT the epoch moving, which is the whole point: nothing
    // invalidates the cache, and the resolver still asks again and binds.
    ent.remote_no_phys[kAwSlot] = false;
    CHECK(res.ResolvePawn(ent, kAwSlot, replay::PawnClass::Unknown, ref));
    CHECK(ref.cls == replay::PawnClass::Remote);
    CHECK(ref.phys.v != replay::playback::kPawnAbsent);   // a body, which is the whole point
    CHECK_FALSE(res.AwaitingPhysCi(kAwSlot));

    // And a body that never comes: the wait is spent, then the absence cap is spent, and the slot
    // latches under the SAME word.  2*cap passes, no more surface walks after them.
    FakeEntity dead;
    dead.publish_epoch = true;
    dead.remote_no_phys[kAwSlot] = true;
    PawnResolver res2;
    res2.SetAttemptCap(kAwCap);
    for (int i = 0; i < 2 * int(kAwCap); ++i)
        CHECK_FALSE(res2.ResolvePawn(dead, kAwSlot, replay::PawnClass::Unknown, ref));
    CHECK(dead.remote_asks[kAwSlot] == size_t(kAwCap) * 2);
    CHECK_FALSE(res2.AwaitingPhysCi(kAwSlot));
    CHECK_FALSE(res2.ResolvePawn(dead, kAwSlot, replay::PawnClass::Unknown, ref));
    CHECK(res2.LastResolveRefusal() == replay::PawnResolve::Refusal::NoPhysCi);
    CHECK(dead.remote_asks[kAwSlot] == size_t(kAwCap) * 2);   // latched: no surface was touched
    CHECK(res2.GetStats().latched == 1);
}

// The two counts are different sentences and the summary must not merge them.
TEST_CASE("the bind summary names an awaited physics CI apart from an absent one") {
    replay::playback::BindResult r;
    r.awaiting_phys_ci = 1;
    char summary[512] = {};
    replay::playback::FormatBindSummary(r, summary, sizeof summary);
    CHECK(strstr(summary, "0 no phys ci") != nullptr);
    CHECK(strstr(summary, "1 awaiting physics ci") != nullptr);
    // The tail must survive: a field added mid-line is how a cap silently eats the end of a log.
    CHECK(strstr(summary, "local pawn ambiguous") != nullptr);
}

// ---------------------------------------------------------------------------------------------
// P3-MOTION. The slot-bound remote entrant's file track carries no net id, so it binds through
// PhysOf(PlayerOf(slot)) -- a handle the pose-write gate refuses (DIAG-ENTRANT-MOTION-TRACK). The
// fix asks the engine's own actor->component resolver, through VouchPhysForUser, for the handle it
// vouches for and binds THAT one; nothing vouches -> the old handle stays and the write refuses,
// exactly as today. These pin both arms.
namespace {

// The discriminator, modelled so it actually SEPARATES the two handles rather than accepting any.
// `kMisaddressed` is what PhysOf hands back for the entrant -- the RUN-46 handle the write refuses.
// `kVouched` is the handle the engine's actor->component resolver would vouch for. Install and
// read-back succeed for the vouched handle ONLY: a fake that ignored the handle (a `(void)handle`
// stub) would pass the mutant too and pin nothing (cf. AUD-W2-P1-48 BLOCKER-1).
struct VouchFakeEntity : FakeEntity {
    static const uint16_t kMisaddressed = 25;
    static const uint16_t kVouched = 12;
    bool scenario_b = false;   // true -> no vouched handle: the receive-path wall

    replay::PhysHandle PhysOf(replay::PlayerHandle) const override {
        return replay::PhysHandle(kMisaddressed);
    }

    replay::IEntity::PhysVouch VouchPhysForUser(replay::UserIndex) const override {
        replay::IEntity::PhysVouch v;
        v.boundHandle = kMisaddressed;
        v.boundVouched = false;
        if (scenario_b) {
            v.diag.ciEntityId = 0;          // CI[+0] uninitialised -- no id round-trips
            v.diag.revHandle = 0;
            v.componentPresent = false;     // ComponentOnActor(phys) == kNoComponent
            v.actorComponent = replay::playback::kPawnAbsent;
            return v;                       // vouched stays false
        }
        v.diag.ciEntityId = kVouched;       // the coherent CI id the re-resolved handle carries
        v.diag.revHandle = kVouched;
        v.actorComponent = kVouched;
        v.componentPresent = true;
        v.vouched = true;
        v.vouchedHandle = kVouched;
        return v;
    }

    bool SetHistoryWindowDetailed(replay::PhysHandle h, const replay::BodyState* s,
                                  const uint64_t* t, size_t n,
                                  replay::InstallReading& ir) override {
        ir = replay::InstallReading();
        ir.from = replay::CiPath::Handle;
        if (h.v != kVouched || n == 0 || !s || !t) {
            ir.why = replay::InstallRefusal::CiIdentityMismatch;   // the RUN-46 refusal, by name
            return false;
        }
        Slot& sl = slots[h.v % kSlots];
        sl.installed = true;
        sl.s = s[0];
        sl.t = t[0];
        sl.depth = n;
        ++sl.installs;
        return true;
    }

    bool ReadBodyDetailed(replay::PhysHandle h, replay::BodyState& out,
                          replay::BodyReading& r) const override {
        r = replay::BodyReading();
        const Slot& sl = slots[h.v % kSlots];
        if (h.v != kVouched || !sl.installed) {
            r.refusedAt = replay::BodyRefusal::CiEntityMismatch;
            r.source = replay::BodySource::None;
            return false;
        }
        out = sl.s;
        r.source = replay::BodySource::History;
        r.historyPresent = true;
        return true;
    }
};

// One no-net-id body (a local-pawn candidate) sampled along a moving trajectory, plus one player on
// `user_slot` -- the exact shape BindBodyHandles pairs by slot.
void FillSlotWindow(Window& w, uint16_t user_slot, size_t n = 90) {
    static uint64_t ticks[256];
    for (size_t i = 0; i < n; ++i) {
        const uint64_t t = 1000000 + i * 11111;
        ticks[i] = t;
        BodyState s;
        MakeBody(s, (float)i, (float)i * 0.01f);   // com[0] sweeps with the playhead
        w.AddBodySample(BodyIdentity(), t, s);      // fill=0,tag=0,value=0: no net id
    }
    for (size_t i = 0; i < n; ++i) {
        PlayerState p;
        memset(&p, 0, sizeof p);
        p.hmdOri[3] = 1.0f;
        w.AddPlayerSample(user_slot, 1000000 + i * 11111, p);
    }
    w.SetTicks(ticks, n);
    w.Finalize();
}

size_t SlotBoundTrack(Window& w) {
    for (size_t i = 0; i < w.BodyCount(); ++i) {
        if (w.BodyAt(i).bound_by_slot) return i;
    }
    return (size_t)-1;
}

}  // namespace

TEST_CASE("P3-MOTION: a slot-bound remote binds the VOUCHED handle and the recorded body moves") {
    Window w;
    FillSlotWindow(w, /*user_slot=*/1);
    VouchFakeEntity ent;

    PawnResolver resolver;   // no slot map installed -> MappedSlot is the identity
    const BindResult r = BindBodyHandles(ent, w, resolver);
    INFO("bound ", r.bound, " probed ", r.p3_probed, " vouched ", r.p3_vouched,
         " bound_h ", r.p3_bound_handle, " vouched_h ", r.p3_vouched_handle);
    REQUIRE(r.bound == 1);
    REQUIRE(r.bound_by_slot == 1);
    REQUIRE(r.slot_class == replay::PawnClass::Remote);

    // The discriminator the run reads: the bound handle was the mis-addressed one, the re-resolve
    // found and bound the vouched handle instead.
    CHECK(r.p3_probed);
    CHECK(r.p3_bound_handle == VouchFakeEntity::kMisaddressed);
    CHECK(r.p3_component_present);
    CHECK(r.p3_vouched);
    CHECK(r.p3_vouched_handle == VouchFakeEntity::kVouched);
    CHECK(r.p3_ci_entity_id == VouchFakeEntity::kVouched);

    // The fix: the track drives the VOUCHED handle, not the mis-addressed one. Under
    // P3_MUTATE_BIND_UNVOUCHED_HANDLE this is kMisaddressed and the case goes red here.
    const size_t track = SlotBoundTrack(w);
    REQUIRE(track != (size_t)-1);
    CHECK(w.BodyAt(track).handle.v == VouchFakeEntity::kVouched);

    // The bind summary is the sentence RUN-L3-47 quotes back.
    char summary[512] = {};
    FormatBindSummary(r, summary, sizeof summary);
    INFO("summary: ", summary);
    CHECK(strstr(summary, "p3probe") != nullptr);
    CHECK(strstr(summary, "comp=present vouched=yes handle=12") != nullptr);

    FakePose pose;
    FakeNetVar nv;
    nv.accept = false;
    OwnerGuard og;
    og.Reset(3, UINT32_MAX);
    StateApplier ap;
    ap.Bind(&ent, &pose, &nv, &og);

    // The pose installs and reads back src=history, and the read CHANGES window-to-window: a bind
    // that installed a frozen pose would read back the same value twice.
    const int64_t t_a = 1000000 + 10 * 11111;
    const ApplyStats st_a = ap.Apply(w, t_a, ApplyOptions());
    CHECK(st_a.bodies_installed == 1);
    CHECK(st_a.bodies_install_identity_refused == 0);
    BodyState back_a{};
    replay::BodyReading ra;
    REQUIRE(ent.ReadBodyDetailed(w.BodyAt(track).handle, back_a, ra));
    CHECK(ra.source == replay::BodySource::History);

    const int64_t t_b = 1000000 + 70 * 11111;
    const ApplyStats st_b = ap.Apply(w, t_b, ApplyOptions());
    CHECK(st_b.bodies_installed == 1);
    BodyState back_b{};
    replay::BodyReading rb;
    REQUIRE(ent.ReadBodyDetailed(w.BodyAt(track).handle, back_b, rb));
    CHECK(rb.source == replay::BodySource::History);
    INFO("com_a ", back_a.com[0], " com_b ", back_b.com[0]);
    CHECK(back_a.com[0] != back_b.com[0]);   // the recorded trajectory reaches the live body
}

TEST_CASE("P3-MOTION: with no vouched handle the bind keeps the old handle and no-ops safely") {
    Window w;
    FillSlotWindow(w, /*user_slot=*/1);
    VouchFakeEntity ent;
    ent.scenario_b = true;   // the receive-path wall: nothing vouches

    PawnResolver resolver;
    const BindResult r = BindBodyHandles(ent, w, resolver);
    REQUIRE(r.bound == 1);
    REQUIRE(r.bound_by_slot == 1);
    CHECK(r.p3_probed);
    CHECK_FALSE(r.p3_component_present);
    CHECK_FALSE(r.p3_vouched);

    // Byte-identical to today's behaviour: the track keeps the handle the write refuses.
    const size_t track = SlotBoundTrack(w);
    REQUIRE(track != (size_t)-1);
    CHECK(w.BodyAt(track).handle.v == VouchFakeEntity::kMisaddressed);

    char summary[512] = {};
    FormatBindSummary(r, summary, sizeof summary);
    CHECK(strstr(summary, "comp=absent vouched=no handle=65535") != nullptr);

    FakePose pose;
    FakeNetVar nv;
    nv.accept = false;
    OwnerGuard og;
    og.Reset(3, UINT32_MAX);
    StateApplier ap;
    ap.Bind(&ent, &pose, &nv, &og);

    const ApplyStats st = ap.Apply(w, 1000000 + 10 * 11111, ApplyOptions());
    CHECK(st.bodies_installed == 0);
    CHECK(st.bodies_install_identity_refused == 1);   // refused by name, never driven blind
}

// ---------------------------------------------------------------------------------------------
// The shape of a SPECTATOR capture, built by hand so it runs with no file: every
// player's body arrives on the wire named by SR15NetUserId, the recorder's own pool copies of those
// bodies carry no identity at all, the disc is a component id, and the player poses come off the
// wire under recorded slots that are not the wire stream's dense order. test_track_map.cpp proves
// the same thing on the owner's real capture.
namespace {

BodyIdentity WireUserBody(uint16_t slot) {
    BodyIdentity id;
    id.value = slot;   // {u16 slot, u16 generation 0}
    id.tag = 0;
    id.fill = replay::capture::kFillBodyIdValue | replay::capture::kFillBodyIdTag;
    return id;
}

BodyIdentity PoolBody(uint16_t handle) {
    BodyIdentity id;   // fill 0: the capture's pool handle, never resolved as an id
    id.value = handle;
    return id;
}

void FillSpectatorWindow(Window& w, const uint16_t* slots, size_t players, size_t n = 60) {
    static uint64_t ticks[256];
    for (size_t i = 0; i < n; ++i) {
        const uint64_t t = 1000000 + i * 11111;
        ticks[i] = t;
        for (size_t p = 0; p < players; ++p) {
            BodyState s;
            // Each player sweeps their own line, 100 m apart, so a body driven from the wrong
            // track is unmistakable.
            MakeBody(s, 100.0f * (float)slots[p] + (float)i, 0.0f);
            w.AddBodySample(WireUserBody(slots[p]), t, s);
            w.AddBodySample(PoolBody((uint16_t)p), t, s);
            PlayerIdentity who;
            who.user_slot = slots[p];
            who.generation = 0;
            who.sampled = false;
            PlayerState ps;
            memset(&ps, 0, sizeof ps);
            ps.hmdOri[3] = 1.0f;
            w.AddPlayerSample(who, t, ps);
        }
        BodyIdentity disc;
        disc.value = 0;
        disc.tag = 1;
        disc.fill = replay::capture::kFillBodyIdValue | replay::capture::kFillBodyIdTag;
        BodyState d;
        MakeBody(d, -50.0f, 0.0f);
        w.AddBodySample(disc, t, d);
    }
    w.SetTicks(ticks, n);
    w.Finalize();
}

}  // namespace

TEST_CASE("TRACKMAP: three wire player bodies bind to three pawns and move independently") {
    const uint16_t slots[3] = {1, 0, 3};   // the real capture's wire order
    Window w;
    FillSpectatorWindow(w, slots, 3);
    REQUIRE(w.BodyCount() == 7);     // 3 wire player bodies, 3 pool copies, the disc
    REQUIRE(w.PlayerCount() == 3);
    for (size_t i = 0; i < 3; ++i) {
        CHECK(w.PlayerAt(i).user_slot == slots[i]);
        CHECK_FALSE(w.PlayerAt(i).from_sampled);
    }

    // The viewer holds live slot 0, so the recorded players are provisioned elsewhere.
    uint16_t map[PawnResolver::kSlots];
    for (uint16_t i = 0; i < PawnResolver::kSlots; ++i) map[i] = i;
    map[0] = 5;
    map[1] = 6;
    map[3] = 7;
    FakeEntity ent;
    const BindResult r = BindBodyHandles(ent, w, map, PawnResolver::kSlots);
    char summary[512] = {};
    FormatBindSummary(r, summary, sizeof summary);
    INFO("summary: ", summary);
    CHECK(r.bound == 3);
    CHECK(r.bound_by_slot == 3);
    CHECK(r.local_pawn_ambiguous == 3);   // the pool copies: no sampled player to pair them with
    CHECK(r.not_a_net_id == 1);           // the disc, a component id
    CHECK(r.slot_already_driven == 0);
    CHECK(r.unresolvable == 0);

    for (size_t i = 0; i < w.BodyCount(); ++i) {
        const BodyTrack& tr = w.BodyAt(i);
        if (ClassifyBodyIdentity(tr.id.fill, tr.id.tag) != IdKind::UserIdByTag) {
            CHECK_FALSE(tr.handle_valid);
            continue;
        }
        REQUIRE(tr.handle_valid);
        CHECK(tr.bound_by_slot);
        CHECK(tr.user_slot == (uint16_t)tr.id.value);
        CHECK(tr.handle.v == map[tr.user_slot]);
    }

    FakePose pose;
    FakeNetVar nv;
    nv.accept = false;
    OwnerGuard og;
    og.Reset(31, UINT32_MAX);
    StateApplier ap;
    ap.Bind(&ent, &pose, &nv, &og);
    const ApplyStats st = ap.Apply(w, 1000000 + 20 * 11111, ApplyOptions());
    CHECK(st.bodies_installed == 3);
    // Each pawn holds ITS player's pose at the playhead, not a neighbour's.
    for (size_t p = 0; p < 3; ++p) {
        const FakeEntity::Slot& sl = ent.slots[map[slots[p]]];
        REQUIRE(sl.installed);
        CHECK(sl.s.com[0] == doctest::Approx(100.0f * (float)slots[p] + 20.0f));
    }
}

TEST_CASE("TRACKMAP: a slot one track already drives is refused to a second") {
    // A player client's capture: its own body sampled with no id beside its own sampled pose, and
    // -- malformed, but representable -- a wire body naming the same slot.
    Window w;
    BodyState s;
    MakeBody(s, 1.0f, 0.0f);
    REQUIRE(w.AddBodySample(WireUserBody(2), 1000, s));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    REQUIRE(w.AddBodySample(PoolBody(9), 1000, s));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    PlayerState ps;
    memset(&ps, 0, sizeof ps);
    REQUIRE(w.AddPlayerSample(2, 1000, ps));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    w.Finalize();

    uint16_t map[PawnResolver::kSlots];
    for (uint16_t i = 0; i < PawnResolver::kSlots; ++i) map[i] = PawnResolver::kSlotUnassigned;
    map[2] = 2;
    FakeEntity ent;
    const BindResult r = BindBodyHandles(ent, w, map, PawnResolver::kSlots);
    CHECK(r.bound == 1);
    CHECK(r.bound_by_slot == 1);
    CHECK(r.slot_already_driven == 1);
    char summary[512] = {};
    FormatBindSummary(r, summary, sizeof summary);
    CHECK(strstr(summary, "1 slot already driven") != nullptr);
}

TEST_CASE("TRACKMAP: a player track is keyed on the recorded slot and generation") {
    Window w;
    PlayerState ps;
    memset(&ps, 0, sizeof ps);
    PlayerIdentity first;
    first.user_slot = 4;
    first.generation = 0;
    first.sampled = false;
    PlayerIdentity second = first;
    second.generation = 1;   // the slot handed to someone else
    REQUIRE(w.AddPlayerSample(first, 1000, ps));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    REQUIRE(w.AddPlayerSample(second, 1000, ps));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    REQUIRE(w.AddPlayerSample(first, 2000, ps));
    w.Finalize();
    REQUIRE(w.PlayerCount() == 2);
    CHECK(w.PlayerAt(0).count == 2);
    CHECK(w.PlayerAt(1).count == 1);
    CHECK(w.PlayerAt(1).generation == 1);
}

TEST_CASE("TRACKMAP: a wire body on a recorded slot nobody assigned is refused by name") {
    // The viewer sits in live slot 0 with a pawn every surface answers for. Recorded slot 1 has
    // been provisioned at live 6; recorded slot 0 has not been placed yet.
    Window w;
    BodyState s;
    MakeBody(s, 1.0f, 0.0f);
    REQUIRE(w.AddBodySample(WireUserBody(0), 1000, s));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    REQUIRE(w.AddBodySample(WireUserBody(1), 1000, s));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    w.Finalize();

    uint16_t map[PawnResolver::kSlots];
    for (uint16_t i = 0; i < PawnResolver::kSlots; ++i) map[i] = PawnResolver::kSlotUnassigned;
    map[1] = 6;
    FakeEntity ent;
    const BindResult r = BindBodyHandles(ent, w, map, PawnResolver::kSlots);
    CHECK(r.bound == 1);
    CHECK(r.slot_unassigned == 1);
    CHECK(r.first_unassigned_slot == 0);
    CHECK(r.no_player_ci == 0);
    // Not bound, and the viewer's slot was never even asked about.
    CHECK(ent.remote_asks[0] == 0);
    CHECK(ent.local_asks[0] == 0);
    for (size_t i = 0; i < w.BodyCount(); ++i) {
        const BodyTrack& tr = w.BodyAt(i);
        if ((uint16_t)tr.id.value == 0) {
            CHECK_FALSE(tr.handle_valid);
        } else {
            REQUIRE(tr.handle_valid);
            CHECK(tr.handle.v == 6);
        }
    }
    char summary[640] = {};  // facts-ok: LOCAL.ECHOUNIT_CI_STRIDE a local buffer size, not the engine offset
    FormatBindSummary(r, summary, sizeof summary);
    CHECK(strstr(summary, "1 slot unassigned (recorded slot 0)") != nullptr);

    FakePose pose;
    FakeNetVar nv;
    nv.accept = false;
    OwnerGuard og;
    og.Reset(31, UINT32_MAX);
    StateApplier ap;
    ap.Bind(&ent, &pose, &nv, &og);
    ap.Apply(w, 1000, ApplyOptions());  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    CHECK_FALSE(ent.slots[0].installed);
    CHECK(ent.slots[6].installed);

    // No map at all is every slot unassigned, not the identity.
    FakeEntity bare;
    const BindResult none = BindBodyHandles(bare, w);
    CHECK(none.bound == 0);
    CHECK(none.slot_unassigned == 2);
    FormatBindSummary(none, summary, sizeof summary);
    CHECK(strstr(summary, "2 slot unassigned (recorded slot 0)") != nullptr);

    // Once the provisioner places recorded slot 0 -- even at the number the file used -- it binds.
    map[0] = 0;
    FakeEntity placed;
    CHECK(BindBodyHandles(placed, w, map, PawnResolver::kSlots).bound == 2);
}

namespace {

// A remote entrant whose identity-proved key is refused, as every replayed pawn's is, and whose
// driven surface names the element by the handle it is driven through.
struct DrivenKeyEntity : FakeEntity {
    mutable size_t driven_key_asks = 0;
    replay::IEntity::EntityKey EntityKeyOfDriven(replay::PhysHandle h, uint16_t& out) const override {
        ++driven_key_asks;
        out = (uint16_t)(h.v + 100);
        return replay::IEntity::EntityKey::Ok;
    }
};

}  // namespace

TEST_CASE("TRACKMAP: a slot-bound remote pawn is named by the key of the element it drives") {
    Window w;
    BodyState s;
    MakeBody(s, 1.0f, 0.0f);
    REQUIRE(w.AddBodySample(WireUserBody(1), 1000, s));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    REQUIRE(w.AddBodySample(WireUserBody(3), 1000, s));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    w.Finalize();
    uint16_t map[PawnResolver::kSlots];
    for (uint16_t i = 0; i < PawnResolver::kSlots; ++i) map[i] = PawnResolver::kSlotUnassigned;
    map[1] = 6;
    map[3] = 7;

    DrivenKeyEntity ent;
    ent.remote_entity[7].ok = true;   // this one's identity-proved key answers, and is kept
    ent.remote_entity[7].key = 55;
    const BindResult r = BindBodyHandles(ent, w, map, PawnResolver::kSlots);
    REQUIRE(r.bound == 2);
    for (size_t i = 0; i < w.BodyCount(); ++i) {
        const BodyTrack& tr = w.BodyAt(i);
        CHECK(tr.slot_entity == ((uint16_t)tr.id.value == 1 ? 106u : 55u));
    }
    CHECK(ent.driven_key_asks == 1);
    char summary[640] = {};  // facts-ok: LOCAL.ECHOUNIT_CI_STRIDE a local buffer size, not the engine offset
    FormatBindSummary(r, summary, sizeof summary);
    CHECK(strstr(summary, "ent=?") == nullptr);
}

// ---------------------------------------------------------------------------------------------
// PLAYBACK-FIX-3 A. The playhead is paced by REAL time and nothing else.

namespace {

// `n` boundaries at a fixed period, starting at `first`.
size_t EvenTicks(uint64_t* out, size_t n, uint64_t first, uint64_t period) {
    for (size_t i = 0; i < n; ++i) out[i] = first + period * i;
    return n;
}

}  // namespace

TEST_CASE("PLAYBACK-FIX-3 A: a window refilled in place must not teleport the playhead") {
    // The shape that produced the sprint: a rolling window whose STORAGE is one buffer refilled per
    // rebuild. The old window's index named a row of the new one, so "preserving" At(index_) + acc_
    // moved the playhead by exactly the span the window had rolled off -- one chapter, every
    // rebuild, measured at 2.02 s and 4.01 s on the capture that reported it.
    static uint64_t storage[600];
    const uint64_t kPeriod = 33333;  // facts-ok: CLOCK.FIXEDSTEP_30HZ_US the 30 Hz step period restated as this case's own cadence fixture
    const uint64_t kChapter = 2000000;
    const uint64_t kOrigin = 843887448;
    uint64_t chapter0 = kOrigin;

    EvenTicks(storage, 180, chapter0, kPeriod);
    TickTable tt;
    tt.Set(storage, 180);
    Transport tr;
    TransportTuning tune;
    tr.Reset(tt, tune);
    tr.SetFileSpan((int64_t)kOrigin, (int64_t)(kOrigin + kChapter * 40));
    tr.SeekTo((int64_t)kOrigin);
    int64_t df = 0, dt = 0;
    Cause dc = Cause::Seek;
    tr.TakeDiscontinuity(df, dt, dc);
    tr.SetRate(1.0);
    tr.SetPaused(false);

    int64_t prev = tr.DisplayPlayheadUs();
    int64_t worst = 0;
    int64_t wall = 0;
    for (int frame = 0; frame < 400; ++frame) {
        tr.Advance(31000);
        wall += 31000;
        // The rebuild, in the module's own order: the storage is REFILLED FIRST and the table is
        // handed over afterwards, so a transport that re-reads At(index_) here reads new chapters.
        if (tr.DisplayPlayheadUs() > (int64_t)(chapter0 + kChapter)) {
            chapter0 += kChapter;
            EvenTicks(storage, 180, chapter0, kPeriod);
            tr.Retable(tt);
        }
        const int64_t now = tr.DisplayPlayheadUs();
        const int64_t step = now > prev ? now - prev : prev - now;
        if (step > worst) worst = step;
        prev = now;
    }
    CHECK(worst < 100000);   // one 31 ms interval plus at most a tick of rounding
    const double ratio = double(prev - (int64_t)kOrigin) / double(wall);
    CHECK(ratio > 0.97);
    CHECK(ratio < 1.03);
}

TEST_CASE("PLAYBACK-FIX-3 A: bursty, huge, zero and negative real intervals") {
    static uint64_t b[4000];
    const uint64_t kPeriod = 33333;  // facts-ok: CLOCK.FIXEDSTEP_30HZ_US the 30 Hz step period restated as this case's own cadence fixture
    EvenTicks(b, 4000, 1000000, kPeriod);
    TickTable tt;
    tt.Set(b, 4000);

    SUBCASE("a hitch is not caught up") {
        Transport tr;
        TransportTuning tune;
        tr.Reset(tt, tune);
        tr.SeekTo(1000000);
        int64_t df = 0, dt = 0;
        Cause dc = Cause::Seek;
        tr.TakeDiscontinuity(df, dt, dc);
        tr.SetRate(1.0);
        tr.SetPaused(false);

        tr.Advance(31000);
        const int64_t before = tr.DisplayPlayheadUs();
        // Five seconds of wall clock in one frame: a shader compile, a breakpoint, a stalled build.
        const StepResult r = tr.Advance(5000000);
        CHECK(r.wall_dropped_us == 5000000 - tune.max_wall_dt_us);
        CHECK(tr.DisplayPlayheadUs() - before <= (int64_t)tune.max_wall_dt_us);

        const int64_t after = tr.DisplayPlayheadUs();
        tr.Advance(31000);
        CHECK(tr.DisplayPlayheadUs() - after < 70000);   // the next frame is an ordinary frame
        // At most the hitch frame's own: a clamped 250 ms is more than four substeps of a 33 ms
        // table can pay, so its remainder is jumped once. What must not happen is a second one on
        // an ordinary frame, which is what banking the hitch looks like.
        CHECK(tr.MicroSeeks() <= 1);
    }

    SUBCASE("zero and huge intervals alternating hold the slope") {
        Transport tr;
        TransportTuning tune;
        tr.Reset(tt, tune);
        tr.SeekTo(1000000);
        int64_t df = 0, dt = 0;
        Cause dc = Cause::Seek;
        tr.TakeDiscontinuity(df, dt, dc);
        tr.SetRate(1.0);
        tr.SetPaused(false);

        const int64_t start = tr.DisplayPlayheadUs();
        int64_t prev = start;
        int64_t worst = 0;
        uint64_t spent = 0;
        for (int i = 0; i < 300; ++i) {  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF plain 300 as a count, size or pixel value, not the extrapolation-cap offset
            const uint64_t d = (i % 10 == 0) ? 400000u : ((i % 3 == 0) ? 0u : 16000u);
            tr.Advance(d);
            spent += d > tune.max_wall_dt_us ? tune.max_wall_dt_us : d;
            const int64_t now = tr.DisplayPlayheadUs();
            const int64_t step = now > prev ? now - prev : prev - now;
            if (step > worst) worst = step;
            prev = now;
        }
        CHECK(worst <= (int64_t)tune.max_wall_dt_us + (int64_t)kPeriod);
        const double ratio = double(prev - start) / double(spent);
        CHECK(ratio > 0.97);
        CHECK(ratio < 1.03);
    }

    SUBCASE("pause advances nothing and reverse mirrors forward") {
        Transport tr;
        TransportTuning tune;
        tr.Reset(tt, tune);
        tr.SeekTo(int64_t(1000000 + kPeriod * 2000));
        int64_t df = 0, dt = 0;
        Cause dc = Cause::Seek;
        tr.TakeDiscontinuity(df, dt, dc);

        tr.SetPaused(true);
        const int64_t parked = tr.DisplayPlayheadUs();
        for (int i = 0; i < 50; ++i) tr.Advance(5000000);
        CHECK(tr.DisplayPlayheadUs() == parked);

        tr.SetRate(1.0);
        tr.SetPaused(false);
        for (int i = 0; i < 60; ++i) tr.Advance(16000);
        const int64_t mid = tr.DisplayPlayheadUs();
        const int64_t fwd = mid - parked;

        tr.SetRate(-1.0);
        for (int i = 0; i < 60; ++i) tr.Advance(16000);
        const int64_t back = mid - tr.DisplayPlayheadUs();
        CHECK(fwd > 900000);
        const int64_t diff = fwd > back ? fwd - back : back - fwd;
        CHECK(diff < (int64_t)kPeriod * 2);
        // The same real time in each direction lands back where it started.
        CHECK(tr.DisplayPlayheadUs() == parked);
    }
}

TEST_CASE("PLAYBACK-FIX-3 A: a playhead past the resident table WAITS, it does not skip") {
    static uint64_t b[100];
    const uint64_t kPeriod = 33333;  // facts-ok: CLOCK.FIXEDSTEP_30HZ_US the 30 Hz step period restated as this case's own cadence fixture
    EvenTicks(b, 100, 1000000, kPeriod);   // only ~3.3 s of the file is decoded
    TickTable tt;
    tt.Set(b, 100);

    Transport tr;
    TransportTuning tune;
    tr.Reset(tt, tune);
    tr.SeekTo(int64_t(1000000 + kPeriod * 98));
    int64_t df = 0, dt = 0;
    Cause dc = Cause::Seek;
    tr.TakeDiscontinuity(df, dt, dc);
    tr.SetRate(1.0);
    tr.SetPaused(false);

    for (int i = 0; i < 30; ++i) tr.Advance(16000);   // half a second with no new coverage
    const int64_t last = (int64_t)b[99];
    CHECK(tr.DisplayPlayheadUs() == last);
    CHECK(tr.CoverageHolds() > 0);
    CHECK(tr.MicroSeeks() == 0);

    // The window catches up: playback carries on from the same instant, having skipped nothing.
    static uint64_t wide[400];
    EvenTicks(wide, 400, 1000000, kPeriod);
    TickTable tt2;
    tt2.Set(wide, 400);
    tr.Retable(tt2);
    CHECK(tr.DisplayPlayheadUs() == last);
    tr.Advance(16000);
    CHECK(tr.DisplayPlayheadUs() > last);
    CHECK(tr.DisplayPlayheadUs() - last < (int64_t)kPeriod * 2);
}

TEST_CASE("PLAYBACK-FIX-3 C: a teleport is snapped, ordinary motion is interpolated") {
    BodyState frames[3] = {};
    const uint64_t times[3] = {1000000, 1022000, 1048000};
    frames[0].com[0] = -5.108f; frames[0].com[1] = 7.322f; frames[0].com[2] = 27.268f;
    frames[0].ori[3] = 1.0f;
    // 0.35 m in 22 ms = 16 m/s: a hard pass, and it must still blend.
    frames[1].com[0] = -5.108f; frames[1].com[1] = 7.322f; frames[1].com[2] = 27.618f;
    frames[1].ori[3] = 1.0f;
    // The round reset: 28.9 m to the face-off point in 26 ms.
    frames[2].com[0] = 0.0f; frames[2].com[1] = 0.0f; frames[2].com[2] = 0.0f;
    frames[2].ori[0] = 0.707107f; frames[2].ori[3] = 0.707107f;

    SeriesCursor cur;
    BodyState out;
    REQUIRE(SampleBodyAt(frames, times, 3, cur, 1011000, out));
    CHECK(out.com[2] > 27.4f);
    CHECK(out.com[2] < 27.5f);

    // Inside the teleport pair: HELD at the older sample, not half way across the arena.
    SeriesCursor cur2;
    REQUIRE(SampleBodyAt(frames, times, 3, cur2, 1035000, out));
    CHECK(out.com[0] == frames[1].com[0]);
    CHECK(out.com[2] == frames[1].com[2]);
    CHECK(out.ori[0] == 0.0f);

    // On the far side the new sample is taken whole -- the zero row still drives the disc.
    SeriesCursor cur3;
    REQUIRE(SampleBodyAt(frames, times, 3, cur3, 1048000, out));
    CHECK(out.com[2] == 0.0f);

    // Reverse through the same pair snaps at the same instant.
    SeriesCursor cur4;
    REQUIRE(SampleBodyAt(frames, times, 3, cur4, 1047000, out));
    CHECK(out.com[2] == frames[1].com[2]);
    REQUIRE(SampleBodyAt(frames, times, 3, cur4, 1011000, out));
    CHECK(out.com[2] > 27.4f);
    CHECK(out.com[2] < 27.5f);
}

// =================================================================================================
// PLAYBACK-FIX-4. The three transport findings of the review, each with the behaviour the
// finding says is missing and a mutant that has to fail it.
// =================================================================================================

TEST_CASE("PLAYBACK-FIX-4 F2: a rebuild that does not cover the held time RAISES the jump") {
    static uint64_t near_ticks[400], far_ticks[400], wide_ticks[4000];
    const uint64_t kPeriod = 33333;  // facts-ok: CLOCK.FIXEDSTEP_30HZ_US the 30 Hz step period restated as this case's own cadence fixture
    EvenTicks(near_ticks, 400, 1000000, kPeriod);
    EvenTicks(far_ticks, 400, 50000000, kPeriod);
    EvenTicks(wide_ticks, 4000, 1000000, kPeriod);

    TickTable near_tt;
    near_tt.Set(near_ticks, 400);
    Transport tr;
    tr.Reset(near_tt, TransportTuning());
    tr.SeekTo(1000000);
    int64_t f = 0, t = 0;
    Cause c = Cause::MicroSeek;
    REQUIRE(tr.TakeDiscontinuity(f, t, c));
    REQUIRE(tr.DisplayPlayheadUs() == 1000000);

    SUBCASE("a table that still covers the held time is silent, as it always was") {
        TickTable wide;
        wide.Set(wide_ticks, 4000);
        tr.Retable(wide);
        CHECK(tr.DisplayPlayheadUs() == 1000000);
        CHECK_FALSE(tr.TakeDiscontinuity(f, t, c));
        CHECK(tr.RetableJumps() == 0);
    }

    SUBCASE("a table that does not is a jump, and says so") {
        TickTable far_tt;
        far_tt.Set(far_ticks, 400);
        tr.Retable(far_tt);
        // The clamp is unavoidable -- the transport cannot hold a time its table has no row for --
        // so what the finding asks for is that it be REPORTED. Measured 1.000 s -> 50.000 s with
        // TakeDiscontinuity answering NO, which is what left the module cursor/owner/residue
        // battery unfired at the `if (jumped)` arm of the tick.
        CHECK(tr.DisplayPlayheadUs() == 50000000);
        CHECK(tr.RetableJumps() == 1);
        REQUIRE(tr.TakeDiscontinuity(f, t, c));
        CHECK(f == 1000000);
        CHECK(t == 50000000);
        CHECK(c == Cause::Seek);
    }
}

TEST_CASE("PLAYBACK-FIX-4 F3: the end of the recording stops, and is not the end of what is decoded") {
    static uint64_t ticks[400];
    const uint64_t kPeriod = 33333;  // facts-ok: CLOCK.FIXEDSTEP_30HZ_US the 30 Hz step period restated as this case's own cadence fixture
    EvenTicks(ticks, 400, 1000000, kPeriod);
    TickTable tt;
    tt.Set(ticks, 400);
    const int64_t last = (int64_t)ticks[399];

    SUBCASE("playing into the last sample of the FILE ends, clamps and pauses") {
        Transport tr;
        tr.Reset(tt, TransportTuning());
        tr.SetFileSpan(1000000, last);
        tr.SeekTo(last - 100000);
        int64_t f = 0, t = 0;
        Cause c = Cause::Seek;
        tr.TakeDiscontinuity(f, t, c);
        tr.SetRate(1.0);
        tr.SetPaused(false);

        StepResult r;
        for (int i = 0; i < 100; ++i) r = tr.Advance(33000);

        CHECK(tr.DisplayPlayheadUs() == last);
        CHECK(tr.Ended() == replay::EndState::AtEnd);
        CHECK(r.end_state == replay::EndState::AtEnd);
        CHECK(tr.Paused());
        // THE COUNTER THAT USED TO CLIMB FOREVER. Measured 92 and rising one per frame at the end
        // of a file, with EndClamps at 0 -- the end of the recording reported as a decoder that was
        // behind, for as long as the viewer left it sitting there.
        CHECK(tr.CoverageHolds() == 0);
        CHECK(tr.EndClamps() == 1);
        CHECK_FALSE(r.held_for_coverage);

        // And it stays put: 100 more frames add nothing to either counter.
        const uint64_t ends = tr.EndClamps();
        for (int i = 0; i < 100; ++i) tr.Advance(33000);
        CHECK(tr.EndClamps() == ends);
        CHECK(tr.CoverageHolds() == 0);
        CHECK(tr.DisplayPlayheadUs() == last);

        // A seek is the way out, and it clears the state.
        tr.SeekTo(2000000);
        CHECK(tr.Ended() == replay::EndState::None);
    }

    SUBCASE("reverse into the first sample of the FILE is the same state at the other end") {
        Transport tr;
        tr.Reset(tt, TransportTuning());
        tr.SetFileSpan(1000000, last);
        tr.SeekTo(1100000);
        int64_t f = 0, t = 0;
        Cause c = Cause::Seek;
        tr.TakeDiscontinuity(f, t, c);
        tr.SetRate(-1.0);
        tr.SetPaused(false);
        for (int i = 0; i < 100; ++i) tr.Advance(33000);

        CHECK(tr.DisplayPlayheadUs() == 1000000);
        CHECK(tr.Ended() == replay::EndState::AtStart);
        CHECK(tr.Paused());
        CHECK(tr.CoverageHolds() == 0);
        CHECK(tr.EndClamps() == 1);
    }

    SUBCASE("the edge of the RESIDENT TABLE inside the file is still a wait, and does not pause") {
        Transport tr;
        tr.Reset(tt, TransportTuning());
        // The file runs a long way past this table: reaching the table end means the window has not
        // caught up, which is the one thing that must go on being reported as a hold.
        tr.SetFileSpan(1000000, last + 60000000);
        tr.SeekTo(last - 100000);
        int64_t f = 0, t = 0;
        Cause c = Cause::Seek;
        tr.TakeDiscontinuity(f, t, c);
        tr.SetRate(1.0);
        tr.SetPaused(false);

        StepResult r;
        for (int i = 0; i < 100; ++i) r = tr.Advance(33000);

        CHECK(r.held_for_coverage);
        CHECK(tr.CoverageHolds() > 0);
        CHECK(tr.EndClamps() == 0);
        CHECK(tr.Ended() == replay::EndState::None);
        CHECK_FALSE(tr.Paused());   // a wait is not a stop: the rebuild resumes from the same instant
    }

    SUBCASE("a transport nobody told where the file ends waits rather than inventing an end") {
        Transport tr;
        tr.Reset(tt, TransportTuning());
        tr.SeekTo(last - 100000);
        int64_t f = 0, t = 0;
        Cause c = Cause::Seek;
        tr.TakeDiscontinuity(f, t, c);
        tr.SetRate(1.0);
        tr.SetPaused(false);
        for (int i = 0; i < 100; ++i) tr.Advance(33000);
        CHECK(tr.Ended() == replay::EndState::None);
        CHECK(tr.CoverageHolds() > 0);
        CHECK_FALSE(tr.Paused());
    }
}

TEST_CASE("PLAYBACK-FIX-4 F4: a slow machine runs at 1.000x, a stall still drops") {
    static uint64_t ticks[8000];
    const uint64_t kPeriod = 33333;  // facts-ok: CLOCK.FIXEDSTEP_30HZ_US the 30 Hz step period restated as this case's own cadence fixture
    EvenTicks(ticks, 8000, 1000000, kPeriod);
    TickTable tt;
    tt.Set(ticks, 8000);

    SUBCASE("120 ms frames -- 8.3 fps -- play at 1.000x, not 0.833x") {
        Transport tr;
        TransportTuning tune;
        tr.Reset(tt, tune);
        tr.SetFileSpan((int64_t)ticks[0], (int64_t)ticks[7999]);
        tr.SeekTo(1000000);
        int64_t f = 0, t = 0;
        Cause c = Cause::Seek;
        tr.TakeDiscontinuity(f, t, c);
        tr.SetRate(1.0);
        tr.SetPaused(false);

        const int64_t start = tr.DisplayPlayheadUs();
        const uint64_t kFrame = 120000;
        for (int i = 0; i < 200; ++i) tr.Advance(kFrame);
        const double ratio = double(tr.DisplayPlayheadUs() - start) / double(200 * kFrame);
        INFO("ratio=", ratio, " clamps=", tr.WallClamps(), " dropped=", tr.WallDroppedUs());
        // THE MEASUREMENT THE FINDING IS. At the old 100 ms clamp this was 0.833 with all 200
        // frames clamped, and the log said "hitches=200" -- a machine that is simply slow, reported
        // in exactly the shape of a one-off stall.
        CHECK(ratio > 0.99);
        CHECK(ratio < 1.01);
        CHECK(tr.WallClamps() == 0);
        CHECK(tr.WallDroppedUs() == 0);
    }

    SUBCASE("a 2 s stall advances by one clamped interval and is counted with its amount") {
        Transport tr;
        TransportTuning tune;
        tr.Reset(tt, tune);
        tr.SetFileSpan((int64_t)ticks[0], (int64_t)ticks[7999]);
        tr.SeekTo(1000000);
        int64_t f = 0, t = 0;
        Cause c = Cause::Seek;
        tr.TakeDiscontinuity(f, t, c);
        tr.SetRate(1.0);
        tr.SetPaused(false);
        tr.Advance(33000);

        const int64_t before = tr.DisplayPlayheadUs();
        const StepResult r = tr.Advance(2000000);
        CHECK(r.wall_dropped_us == 2000000 - tune.max_wall_dt_us);
        CHECK(tr.DisplayPlayheadUs() - before <= (int64_t)tune.max_wall_dt_us);
        CHECK(tr.WallClamps() == 1);

        // And the stall is not paid back afterwards: ten ordinary frames advance ten ordinary
        // frames worth, which is what "hitches are dropped, never caught up" means to a viewer.
        const int64_t after = tr.DisplayPlayheadUs();
        for (int i = 0; i < 10; ++i) tr.Advance(33000);
        CHECK(tr.DisplayPlayheadUs() - after < 10 * 33000 + (int64_t)kPeriod);
    }

    SUBCASE("2x on a slow machine is 2x, which the old clamp made 1.67x") {
        Transport tr;
        TransportTuning tune;
        tr.Reset(tt, tune);
        tr.SetFileSpan((int64_t)ticks[0], (int64_t)ticks[7999]);
        tr.SeekTo(1000000);
        int64_t f = 0, t = 0;
        Cause c = Cause::Seek;
        tr.TakeDiscontinuity(f, t, c);
        tr.SetRate(2.0);
        tr.SetPaused(false);

        const int64_t start = tr.DisplayPlayheadUs();
        const uint64_t kFrame = 120000;
        for (int i = 0; i < 100; ++i) tr.Advance(kFrame);
        const double ratio = double(tr.DisplayPlayheadUs() - start) / double(100 * kFrame);
        INFO("ratio=", ratio);
        CHECK(ratio > 1.98);
        CHECK(ratio < 2.02);
    }
}

// ==============================================================================================
// ★ SMOOTHER MOTION BETWEEN THE 45 Hz SAMPLES (DIAG-RECORD-RATE). The recorded velocities are
// already decoded beside the positions; used as Hermite tangents they take the corner off every
// sample boundary. Every case below is about the guards as much as the curve: the curve is only
// better than the line where the tangents describe the same motion the endpoints do.
// ==============================================================================================

namespace {

// Two samples one 45 Hz period apart on a parabola x(t) = x0 + v0*t + 0.5*a*t^2, with the
// velocities that curve really has. A straight line between them cuts the corner; the Hermite
// does not.
void MakeHermitePair(BodyState out[2], uint64_t times[2], float v0, float v1) {
    const double dt = 1.0 / 45.0;
    memset(out, 0, sizeof(BodyState) * 2);
    times[0] = 1000000;
    times[1] = times[0] + (uint64_t)(dt * 1e6);
    out[0].com[0] = 0.0f;
    out[1].com[0] = (float)((v0 + v1) * 0.5 * dt);   // exact for a constant acceleration
    out[0].vel[0] = v0;
    out[1].vel[0] = v1;
    out[0].ori[3] = 1.0f;
    out[1].ori[3] = 1.0f;
}

float SampleX(const BodyState* f, const uint64_t* t, int64_t at, bool hermite, bool reversed = false,
              replay::playback::HermiteTally* tally = nullptr) {
    SeriesCursor c;
    BodyState s;
    memset(&s, 0, sizeof s);
    REQUIRE(replay::playback::SampleBodyAt(f, t, 2, c, at, s, reversed, hermite, tally));
    return s.com[0];
}

}  // namespace

TEST_CASE("the hermite passes through both samples exactly and matches lerp at constant velocity") {
    using namespace replay::playback;
    BodyState f[2];
    uint64_t t[2];
    MakeHermitePair(f, t, 2.0f, 8.0f);

    CHECK(SampleX(f, t, (int64_t)t[0], true) == doctest::Approx(f[0].com[0]));
    CHECK(SampleX(f, t, (int64_t)t[1], true) == doctest::Approx(f[1].com[0]));

    // Equal tangents that agree with the chord: the cubic terms cancel and the curve IS the line.
    // This is the case that says the interpolant is not just "different", it is a refinement.
    MakeHermitePair(f, t, 5.0f, 5.0f);
    for (int k = 1; k < 8; ++k) {
        const int64_t at = (int64_t)t[0] + (int64_t)(t[1] - t[0]) * k / 8;
        CHECK(SampleX(f, t, at, true) == doctest::Approx(SampleX(f, t, at, false)).epsilon(1e-5));
    }
}

TEST_CASE("the hermite bends towards the recorded velocities and is symmetric in direction") {
    using namespace replay::playback;
    BodyState f[2];
    uint64_t t[2];
    MakeHermitePair(f, t, 0.0f, 10.0f);   // accelerating hard: the line is well above the curve

    const int64_t mid = (int64_t)t[0] + (int64_t)(t[1] - t[0]) / 2;
    const float curve = SampleX(f, t, mid, true);
    const float line = SampleX(f, t, mid, false);
    CHECK(curve < line);                   // starting from rest, the body has covered less ground
    // And it is the RIGHT amount less: a cubic Hermite reproduces a constant acceleration exactly,
    // so the midpoint is 0.5*a*(dt/2)^2 = a quarter of the ground covered over the whole bracket.
    CHECK(curve == doctest::Approx(f[1].com[0] * 0.25f).epsilon(1e-4));

    // ★ SYMMETRIC IN TIME. The engine gets negated velocities on a reverse pass, but the
    // INTERPOLANT must not see the direction at all: the same instant is the same position, or a
    // rewind and a replay of the same second would not retrace the same path.
    CHECK(SampleX(f, t, mid, true, /*reversed=*/true) == curve);
}

TEST_CASE("the hermite falls back to the straight line on every disagreement it can see") {
    using namespace replay::playback;
    BodyState f[2];
    uint64_t t[2];

    // A gap: more than three sample periods between the two rows is a hole in the recording, not a
    // bracket, and a cubic across it invents a swing the capture never had.
    MakeHermitePair(f, t, 0.0f, 10.0f);
    t[1] = t[0] + 200000;
    const int64_t gapMid = (int64_t)t[0] + 100000;
    CHECK(SampleX(f, t, gapMid, true) == SampleX(f, t, gapMid, false));

    // A velocity no player or disc reaches: the field is not a velocity, so it is not a tangent.
    MakeHermitePair(f, t, 0.0f, 10.0f);
    f[1].vel[0] = 1000.0f;
    const int64_t mid = (int64_t)t[0] + (int64_t)(t[1] - t[0]) / 2;
    CHECK(SampleX(f, t, mid, true) == SampleX(f, t, mid, false));

    // ...and a NaN, which a naive `speed > limit` test would admit rather than refuse.
    MakeHermitePair(f, t, 0.0f, 10.0f);
    f[0].vel[1] = std::numeric_limits<float>::quiet_NaN();
    CHECK(SampleX(f, t, mid, true) == SampleX(f, t, mid, false));

    // Tangents that disagree with the endpoints by metres. The clamp is per axis, so the axis that
    // overshoots takes the line and the ones that agree keep their curve.
    MakeHermitePair(f, t, 0.0f, 10.0f);
    f[0].vel[0] = -140.0f;   // legal as a speed, absurd against a bracket 22 mm long
    f[1].vel[0] = 140.0f;
    CHECK(SampleX(f, t, mid, true) == SampleX(f, t, mid, false));

    // A teleport still snaps: the curve must not be what re-introduces the slide across a jump.
    BodyState tp[2];
    uint64_t tpt[2] = {1000000, 1000000 + 22222};
    memset(tp, 0, sizeof tp);
    tp[0].ori[3] = 1.0f;
    tp[1].ori[3] = 1.0f;
    tp[1].com[0] = 400.0f;
    tp[0].vel[0] = 1.0f;
    tp[1].vel[0] = 1.0f;
    CHECK(SampleX(tp, tpt, (int64_t)tpt[0] + 11111, true) == 0.0f);
}

// The per-second line counts these, and a count that cannot name WHY the line won says only that
// the curve is not running -- which is the state the reader already suspected. Each reason is
// driven from the fixture that produces it, so a guard that stops refusing, or that starts
// refusing for a neighbouring reason, moves a number here.
TEST_CASE("each bracket is counted against the interpolant it got, and a refusal by its reason") {
    using namespace replay::playback;
    BodyState f[2];
    uint64_t t[2];
    MakeHermitePair(f, t, 0.0f, 10.0f);
    const int64_t mid = (int64_t)t[0] + (int64_t)(t[1] - t[0]) / 2;

    HermiteTally curve;
    SampleX(f, t, mid, true, false, &curve);
    CHECK(curve.cubic == 1);
    CHECK(curve.Lerped() == 0);

    // With the curve off the straight line is what was asked for: not a fallback, so no reason.
    SampleX(f, t, mid, false, false, &curve);
    CHECK(curve.cubic == 1);
    CHECK(curve.Lerped() == 0);

    // And past the newest sample the pose is held, so there is no bracket to count either way.
    SampleX(f, t, (int64_t)t[1] + 50000, true, false, &curve);
    CHECK(curve.cubic == 1);
    CHECK(curve.Lerped() == 0);

    HermiteTally gap;
    MakeHermitePair(f, t, 0.0f, 10.0f);
    t[1] = t[0] + 200000;
    SampleX(f, t, (int64_t)t[0] + 100000, true, false, &gap);
    CHECK(gap.gap == 1);
    CHECK(gap.Lerped() == 1);

    HermiteTally velocity;
    MakeHermitePair(f, t, 0.0f, 10.0f);
    f[1].vel[0] = 1000.0f;
    SampleX(f, t, mid, true, false, &velocity);
    CHECK(velocity.velocity == 1);
    CHECK(velocity.Lerped() == 1);

    HermiteTally clamped;
    MakeHermitePair(f, t, 0.0f, 10.0f);
    f[0].vel[0] = -140.0f;   // legal as a speed, absurd against a bracket 22 ms long
    f[1].vel[0] = 140.0f;
    SampleX(f, t, mid, true, false, &clamped);
    CHECK(clamped.clamp == 1);
    CHECK(clamped.cubic == 0);   // the curve ran and was thrown away: that is not a cubic bracket

    HermiteTally jumped;
    BodyState jump[2];
    uint64_t jt[2] = {1000000, 1000000 + 22222};
    memset(jump, 0, sizeof jump);
    jump[0].ori[3] = 1.0f;
    jump[1].ori[3] = 1.0f;
    jump[1].com[0] = 400.0f;
    jump[0].vel[0] = 1.0f;
    jump[1].vel[0] = 1.0f;
    SampleX(jump, jt, (int64_t)jt[0] + 11111, true, false, &jumped);
    CHECK(jumped.teleport == 1);
    CHECK(jumped.Lerped() == 1);
}

// ---------------------------------------------------------------------------------------------
// The micro-seek latch: the owner's 4v4 sat at 6-10 fps because every frame raised a
// catch-up discontinuity, and each one cost ~100 ms of residue battery at eight players.
//
// Both halves are asserted here against the transport alone, because both are transport bugs:
//   - `saturated` was `steps_consumed == max_substeps`, which is also the healthy exit with the
//     counter on K and no remainder left. That raised a jump to where the playhead already was.
//   - K was four TICKS while its reasoning was in MILLISECONDS. On a 90 Hz capture four ticks pay
//     44 ms, so at 1x every frame under 22 fps saturated -- and the recovery made the next frame
//     slower, so it never came back.
namespace {

size_t Perf2Ticks(uint64_t* out, size_t n, uint64_t first, uint64_t period) {
    for (size_t i = 0; i < n; ++i) out[i] = first + period * i;
    return n;
}

}  // namespace

TEST_CASE("1x playback on a headset-cadence capture never micro-seeks") {
    static uint64_t ticks[4000];
    const uint64_t kPeriod = 11111;   // facts-ok: the ~90 Hz cadence the owner's captures record at
    Perf2Ticks(ticks, 4000, 1000000, kPeriod);
    TickTable tt;
    tt.Set(ticks, 4000);

    Transport tr;
    TransportTuning tune;
    tr.Reset(tt, tune);
    tr.SeekTo(1000000);
    int64_t df = 0, dt = 0;
    Cause dc = Cause::Seek;
    tr.TakeDiscontinuity(df, dt, dc);
    tr.SetRate(1.0);
    tr.SetPaused(false);

    // Ten seconds of a machine running at 20 fps -- slow, but nothing a viewer would call a stall.
    // Before the fix the fourth substep ran out at 44 ms and this raised 200 micro-seeks, one per
    // frame, each one a full category-N residue battery.
    for (int i = 0; i < 200; ++i) tr.Advance(50000);
    CHECK(tr.MicroSeeks() == 0);

    // And the picture still keeps up with the wall: stepping paid the time, it was not jumped.
    const int64_t travelled = tr.DisplayPlayheadUs() - 1000000;
    CHECK(travelled > 9500000);
    CHECK(travelled < 10500000);

    // A frame at the collapsed rate the bug produced -- 7 fps, 140 ms, thirteen ticks of this
    // table -- is still paid by stepping, which is what stops the loop from latching.
    const uint64_t before = tr.MicroSeeks();
    tr.Advance(140000);
    CHECK(tr.MicroSeeks() == before);
}

TEST_CASE("A frame that spends exactly K substeps is not saturated") {
    static uint64_t ticks[64];
    const uint64_t kPeriod = 10000;
    Perf2Ticks(ticks, 64, 1000000, kPeriod);
    TickTable tt;
    tt.Set(ticks, 64);

    Transport tr;
    TransportTuning tune;
    tune.max_substeps = 4;   // the old default, so the boundary is reachable in one frame
    tr.Reset(tt, tune);
    tr.SeekTo(1000000);
    int64_t df = 0, dt = 0;
    Cause dc = Cause::Seek;
    tr.TakeDiscontinuity(df, dt, dc);
    tr.SetRate(1.0);
    tr.SetPaused(false);

    // Exactly four ticks of wall time: the loop takes four steps and has nothing left to jump.
    const StepResult r = tr.Advance(4 * kPeriod);
    CHECK(r.steps_consumed == 4u);
    CHECK_FALSE(r.saturated);
    CHECK_FALSE(r.micro_seek);
    CHECK(tr.MicroSeeks() == 0);

    // One microsecond more than four ticks is still not saturated: the remainder is smaller than
    // the next step, so there was never a fifth step to want.
    const StepResult r2 = tr.Advance(4 * kPeriod + 1);
    CHECK_FALSE(r2.saturated);
    CHECK(tr.MicroSeeks() == 0);

    // Five ticks in one frame IS saturation: the fifth step is affordable and the budget is spent.
    const StepResult r3 = tr.Advance(5 * kPeriod);
    CHECK(r3.saturated);
    CHECK(r3.micro_seek);
    CHECK(tr.MicroSeeks() == 1);
}

// SMOOTH-1 T1 through the real Apply: a SmoothFrame of weight 0 publishes exactly what no SmoothFrame
// publishes, and weight 1 (the control) publishes something else, so the comparison can fail.
namespace {

struct SmoothApplyRig {
    Window w;
    FakeEntity ent;
    FakePose pose;
    FakeNetVar nv;
    OwnerGuard og;
    StateApplier ap;
    explicit SmoothApplyRig(uint64_t seed) {
        smoothing_fixture::FillSmoothWindow(w, 300, 0.003, seed);
        w.SetFileEdges(true, true);
        w.BodyAt(0).handle = PhysHandle(1);
        w.BodyAt(0).handle_valid = true;
        w.PlayerAt(0).handle = PlayerHandle(1);
        w.PlayerAt(0).handle_valid = true;
        w.PlayerAt(0).user_id = 0x00010004u;
        w.PlayerAt(0).user_id_valid = true;
        og.Reset(0x00000003u, 0xFFFFFFFFu);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
        REQUIRE(og.RegisterEntrant(0x00010004u) == OwnerVerdict::Ok);
        ap.Bind(&ent, &pose, &nv, &og);
    }
};

}  // namespace

TEST_CASE("smoothing T1: through Apply, weight 0 is the 1:1 path and weight 1 is not") {
    SmoothApplyRig none(61), zero(61), one(61);
    replay::playback::SmoothTally tally;
    replay::playback::SmoothFrame frame0, frame1;
    frame0.weight = 0.0f;
    frame0.tally = &tally;
    frame1.weight = 1.0f;
    const uint64_t* t = none.w.BodyTimes(none.w.BodyAt(0));
    const int64_t lo = (int64_t)t[20], hi = (int64_t)t[none.w.BodyAt(0).count - 20];
    size_t differ0 = 0, differ1 = 0, published = 0;
    for (int k = 0; k < 3000; ++k) {
        const int64_t at = k < 1500 ? lo + (hi - lo) * k / 1500 : hi - (hi - lo) * (k - 1500) / 1500;
        ApplyOptions opt;
        opt.reversed = k >= 1500;
        none.ap.Apply(none.w, at, opt);
        opt.smooth = &frame0;
        zero.ap.Apply(zero.w, at, opt);
        opt.smooth = &frame1;
        one.ap.Apply(one.w, at, opt);
        if (none.pose.calls[1] == 0) continue;
        ++published;
        const bool same0 = memcmp(&none.ent.slots[1].s, &zero.ent.slots[1].s, sizeof(BodyState)) == 0 &&
                           memcmp(&none.pose.last[1], &zero.pose.last[1], sizeof(PlayerState)) == 0;
        const bool same1 = memcmp(&none.ent.slots[1].s, &one.ent.slots[1].s, sizeof(BodyState)) == 0 &&
                           memcmp(&none.pose.last[1], &one.pose.last[1], sizeof(PlayerState)) == 0;
        differ0 += !same0;
        differ1 += !same1;
    }
    CHECK(published == 3000);
    CHECK(differ0 == 0);
    CHECK(differ1 > 2500);           // the control: the smoothed path is observably different
    CHECK(tally.brackets == 0);      // weight 0 never entered the smooth sampler
}

TEST_CASE("smoothing: the mode crossfade ramps over 200 ms and lands on a seek") {
    using replay::playback::StepSmoothWeight;
    float w = 0.0f;
    for (int f = 0; f < 10; ++f) w = StepSmoothWeight(w, 1.0f, 10000, false);   // 100 ms
    CHECK(w == doctest::Approx(0.5f));
    for (int f = 0; f < 20; ++f) w = StepSmoothWeight(w, 1.0f, 10000, false);
    CHECK(w == 1.0f);                // clamped at the target, never past it
    w = StepSmoothWeight(w, 0.0f, 50000, false);
    CHECK(w == doctest::Approx(0.75f));
    CHECK(StepSmoothWeight(w, 0.0f, 1000, true) == 0.0f);   // a seek lands exactly
    CHECK(StepSmoothWeight(0.3f, 1.0f, -5000, false) == 0.3f);   // a negative wall step moves nothing
}
