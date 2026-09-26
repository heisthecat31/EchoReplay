// W2-D's offline gate. Everything here runs on a machine with no game.
//
// ---------------------------------------------------------------------------------------------
// ★ WHAT THIS SUITE ESTABLISHES AND WHAT IT CANNOT.
//
// ESTABLISHES  (a) that every difference this module takes is CLASSIFIED before it is used, and
//              that each of the four classifications is separately constructible -- a regression, a
//              gap, a duplicate and a normal step all arrive and are all told apart;
//              (b) that a discontinuity in the instrument never becomes a rate reading, over a
//              60-second simulated sweep with resets injected on a schedule, with the reset count
//              asserted EXACTLY rather than "greater than zero";
//              (c) that the timescale written for a reverse rate is POSITIVE -- the one arithmetic
//              mistake here that would make the engine take its largest forward step every frame;
//              (d) that the meter reports UNKNOWN rather than a stale value once its samples stop,
//              and RECOVERS when they resume.
//
// CANNOT       say anything about the ENGINE. No CClock is written here, no address is
//              dereferenced, and the offline derivation of CLOCK.MINELAPSED_OFF == 0 is checked
//              against the bytes by tests/clock/check.ps1's factcheck arm, not by this file.
//              ★ THIS SUITE PASSING IS NOT THE CLOCK WORKING. It is the half that needs no launch.
//
// ★ EVERY RED ARM IS COMPILED AND RUN. NaiveMeter below is the module as it would be written
// without the classification -- unsigned differences, everything fed -- and it is asserted to
// DIVERGE on the same input. A guard whose red is unconstructible is not a guard, and this project
// has shipped three of those.
// ---------------------------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "engine/clock_facts.h"
#include "engine/clock_policy.h"
#include "generated/facts_generated.h"
#include "release_check.h"

using namespace replay::clock;
using replay::TransportState;
using replay::clock_facts::kFloorUs;

namespace {

// The module as it would be written by someone who took the differences without asking. It is the
// exact shape of the four defects the audio packet found in an hour, and it exists so the gate can
// prove it tells the two implementations apart.
struct NaiveMeter {
    bool started = false;
    uint64_t prevSim = 0, prevWall = 0;
    double simSum = 0.0, wallSum = 0.0;

    void Sample(uint64_t sim, uint64_t wallUs) {
        if (started) {
            simSum += static_cast<double>(sim - prevSim);      // unsigned. A reset reads as 1.8e19
            wallSum += static_cast<double>(wallUs - prevWall);
        }
        prevSim = sim;
        prevWall = wallUs;
        started = true;
    }
    double Rate() const { return wallSum > 0.0 ? simSum / wallSum : 0.0; }
};

// The registry, not a transcription of it: this is the number the engine clamps every
// heartbeat to, and taking it from anywhere else would let the suite agree with itself.
const uint64_t kMaxElapsed = facts::CLOCK_MAXELAPSED_US;

SimBounds Bounds() {
    SimBounds b;
    b.maxElapsedUs = kMaxElapsed;
    b.maxBeatsPerFrame = 3;
    return b;
}

}  // namespace

TEST_CASE("ClassifySim separates the four things a difference can mean") {
    const SimBounds b = Bounds();
    uint64_t d = 12345;

    // Normal.
    CHECK(ClassifySim(1200, 1750, 1, b, d) == SampleKind::Normal);
    CHECK(d == 550);

    // Equal is a REAL reading -- it is what a paused clock looks like -- and it carries a delta of
    // zero rather than being an error.
    d = 7;
    CHECK(ClassifySim(1200, 1200, 1, b, d) == SampleKind::Stalled);
    CHECK(d == 0);

    // ★★★ THE ONE THAT MATTERS. A regression is not a small reading and it is not a large one; the
    // naive difference turns it into the largest reading representable, and the two are asserted
    // here to be different answers rather than described as different.
    d = 7;
    CHECK(ClassifySim(1750, 1200, 1, b, d) == SampleKind::Regressed);
    CHECK(d == 0);
    const uint64_t naive = 1200ull - 1750ull;
    CHECK(naive > 1.8e19);
    CHECK(naive != d);

    // Beyond what three clamped heartbeats can produce in one frame.
    d = 7;
    CHECK(ClassifySim(0, 3 * kMaxElapsed + 1, 1, b, d) == SampleKind::Implausible);
    CHECK(d == 0);

    // The boundary, pinned from both sides: a bound nobody has stood on is a bound nobody has
    // checked, and off-by-one here would silently reject or silently admit every hitch.
    CHECK(ClassifySim(0, 3 * kMaxElapsed, 1, b, d) == SampleKind::Normal);
    CHECK(d == 3 * kMaxElapsed);
    CHECK(ClassifySim(0, 3 * kMaxElapsed + 1, 1, b, d) == SampleKind::Implausible);

    // The bound scales with the frames spanned, and stops meaning anything past maxFramesSpanned.
    CHECK(ClassifySim(0, 6 * kMaxElapsed, 2, b, d) == SampleKind::Normal);
    CHECK(ClassifySim(0, 6 * kMaxElapsed + 1, 2, b, d) == SampleKind::Implausible);
    CHECK(ClassifySim(0, 1200, b.maxFramesSpanned + 1, b, d) == SampleKind::Implausible);

    // A zero ceiling makes every bound vacuous, so it is refused rather than dividing the world
    // into "everything is implausible" without saying which.
    SimBounds broken = b;
    broken.maxElapsedUs = 0;
    CHECK(ClassifySim(0, 1, 1, broken, d) == SampleKind::Implausible);
    broken = b;
    broken.maxBeatsPerFrame = 0;
    CHECK(ClassifySim(0, 1, 1, broken, d) == SampleKind::Implausible);
}

TEST_CASE("ClassifyWall converts without overflowing and refuses the impossible") {
    const uint64_t freq = 10000000;   // 10 MHz, a typical QPC  // facts-ok: CLOCK.CTOR_MAXELAPSED_INIT 10,000,000 in tick arithmetic, not the clock constructor's initialiser
    uint64_t us = 0;

    CHECK(ClassifyWall(0, freq / 60, freq, 0, us) == SampleKind::Normal);
    CHECK(us == 16666);

    CHECK(ClassifyWall(100, 100, freq, 0, us) == SampleKind::Stalled);
    CHECK(us == 0);
    CHECK(ClassifyWall(200, 100, freq, 0, us) == SampleKind::Regressed);
    CHECK(us == 0);
    CHECK(ClassifyWall(0, 100, 0, 0, us) == SampleKind::Implausible);

    // ★ The overflow the split conversion exists to prevent. `ticks * 1000000` wraps a u64 at about
    // 1.8e13 ticks -- three weeks at 10 MHz -- and a wrapped duration is a small, plausible number
    // that nothing downstream would question. 40 days of ticks must still convert exactly.
    const uint64_t bigTicks = 40ull * 24 * 3600 * freq;
    CHECK(ClassifyWall(0, bigTicks, freq, 0, us) == SampleKind::Normal);
    CHECK(us == 40ull * 24 * 3600 * 1000000ull);
    const uint64_t wrapped = (bigTicks * 1000000ull) / freq;
    CHECK(wrapped != us);   // the naive form really does produce a different, wrong answer

    // Exact for a frequency that does not divide evenly, which is what the remainder term is for.
    CHECK(ClassifyWall(0, 3, 7, 0, us) == SampleKind::Normal);
    CHECK(us == 428571);

    CHECK(ClassifyWall(0, freq * 61, freq, 60000000, us) == SampleKind::Implausible);
}

TEST_CASE("RateMeter reports the ratio, and reports UNKNOWN rather than a stale value") {
    RateMeter m;
    m.Reset();

    double r = 0.0;
    CHECK_FALSE(m.Achieved(r));   // nothing yet is not zero

    for (int i = 0; i < 200; ++i) {
        m.Feed(2 * 16666, 16666);   // the engine advanced 2 us of sim per 1 us of wall
    }
    REQUIRE(m.Achieved(r));
    CHECK(r == doctest::Approx(2.0).epsilon(0.001));

    double p = 0.0;
    REQUIRE(m.FramePeriodUs(p));
    CHECK(p == doctest::Approx(16666.0).epsilon(0.001));

    // ★★ THE LATCH THAT MUST NOT BE PERMANENT. Once every sample starts failing, a meter that keeps
    // returning its last good answer looks healthy forever -- the audio packet's tracker frozen
    // after saturation, exactly. It must go UNKNOWN...
    for (uint32_t i = 0; i < RateMeter::kStaleAfter - 1; ++i) {
        m.Reject();
    }
    CHECK(m.Achieved(r));         // still live: the run has not reached the threshold
    m.Reject();
    CHECK_FALSE(m.Achieved(r));   // and now it is unknown, not stale
    CHECK_FALSE(m.FramePeriodUs(p));
    CHECK(m.RejectRun() == RateMeter::kStaleAfter);

    // ...and it must COME BACK. A latch that cannot clear is the same defect wearing a different
    // hat: the reading would be unknown forever after one bad patch.
    m.Feed(2 * 16666, 16666);
    CHECK(m.Achieved(r));
    CHECK(m.RejectRun() == 0);
    CHECK(r == doctest::Approx(2.0).epsilon(0.01));

    // A zero denominator never divides.
    RateMeter z;
    z.Reset();
    z.Feed(1200, 0);
    CHECK_FALSE(z.Achieved(r));
}

TEST_CASE("Ceiling reproduces the design's own numbers") {
    // These two are the oracle: DESIGN and the packet brief both state 4.0x at 60 Hz and 6.0x at
    // 90 Hz for CLOCK.MAXELAPSED_US, so a formula that does not produce them is the wrong formula.
    CHECK(Ceiling(kMaxElapsed, 1000000.0 / 60.0) == doctest::Approx(4.0).epsilon(0.001));
    CHECK(Ceiling(kMaxElapsed, 1000000.0 / 90.0) == doctest::Approx(6.0).epsilon(0.001));
    CHECK(Ceiling(kMaxElapsed, 1000000.0 / 144.0) == doctest::Approx(9.6).epsilon(0.001));

    // Unknown is 0, and 0 is not 4.0: assuming the 60 Hz answer on every display is how a ceiling
    // becomes wrong on the machine it matters on.
    CHECK(Ceiling(kMaxElapsed, 0.0) == 0.0);
    CHECK(Ceiling(0, 16666.0) == 0.0);
}

TEST_CASE("PlanTimescale writes a magnitude, never a sign, never a zero, never a NaN") {
    // ★★★ THE LOAD-BEARING CASE. TICK.HEARTBEAT_BEFORE converts the scaled product with the
    // unsigned cvttss2si idiom at 0x1400CF695, so a negative product reads as a huge u64 and is
    // then pinned to CLOCK.MAXELAPSED_US -- the engine's LARGEST forward step, every frame, from a
    // reverse rate. Every negative rate must come out positive.
    for (double r = -8.0; r < 0.0; r += 0.25) {
        const TimescalePlan p = PlanTimescale(r, 0.0);
        CHECK(p.value > 0.0f);
        CHECK(p.value == doctest::Approx(static_cast<float>(-r)).epsilon(0.0001));
    }

    // Zero is the +inf hazard: a zero scale rounds the scaled delta to zero and CLOCK.RECIP_OFF
    // becomes 1.0f/0.0f.
    CHECK(PlanTimescale(0.0, 0.0).value > 0.0f);
    CHECK(PlanTimescale(0.0, 0.0).cappedByFloor);

    // NaN passes every ordered comparison silently. It must not reach the float store.
    const double nan = sqrt(-1.0);
    const TimescalePlan pn = PlanTimescale(nan, 4.0);
    CHECK(pn.value == pn.value);
    CHECK(pn.value == doctest::Approx(1.0f));

    // Ordinary rates pass through untouched when no ceiling is known.
    CHECK(PlanTimescale(1.0, 0.0).value == doctest::Approx(1.0f));
    CHECK(PlanTimescale(0.1, 0.0).value == doctest::Approx(0.1f));
    CHECK_FALSE(PlanTimescale(0.1, 0.0).cappedByFloor);
    CHECK(PlanTimescale(8.0, 0.0).value == doctest::Approx(8.0f));

    // The ceiling caps and SAYS it capped. Writing 8.0 above a 4.0 ceiling behaves identically to
    // writing 4.0, so the register would silently disagree with the label.
    const TimescalePlan pc = PlanTimescale(8.0, 4.0);
    CHECK(pc.value == doctest::Approx(4.0f));
    CHECK(pc.cappedByCeiling);
    CHECK_FALSE(PlanTimescale(4.0, 4.0).cappedByCeiling);
    CHECK_FALSE(PlanTimescale(2.0, 4.0).cappedByCeiling);
    // Reverse is capped by magnitude too, and stays positive through the cap.
    const TimescalePlan pr = PlanTimescale(-8.0, 4.0);
    CHECK(pr.value == doctest::Approx(4.0f));
    CHECK(pr.cappedByCeiling);
}

TEST_CASE("ClampPlayhead is signed, and clamps at both ends") {
    CHECK(ClampPlayhead(-1, 1200) == 0);
    CHECK(ClampPlayhead(-1000000, 1200) == 0);
    CHECK(ClampPlayhead(500, 1200) == 500);
    CHECK(ClampPlayhead(1200, 1200) == 1200);
    CHECK(ClampPlayhead(1201, 1200) == 1200);
    CHECK(ClampPlayhead(12345, 0) == 0);
    // A duration wider than int64 cannot silently become negative.
    CHECK(ClampPlayhead(500, 0xFFFFFFFFFFFFFFFFull) == 500);  // facts-ok: LOBBY.JOIN_FREE_SENTINEL an all-ones u64 "none" local to this code, not the lobby join sentinel
    CHECK(ClampPlayhead(0x7FFFFFFFFFFFFFFFll, 0xFFFFFFFFFFFFFFFFull) == 0x7FFFFFFFFFFFFFFFll);  // facts-ok: LOBBY.JOIN_FREE_SENTINEL an all-ones u64 "none" local to this code, not the lobby join sentinel
}

TEST_CASE("ClassifyPlayheadMove tells a step from a seek from a reverse") {
    uint64_t step = 99;
    CHECK(ClassifyPlayheadMove(1200, 1200, kMaxElapsed, step) == MoveKind::None);
    CHECK(step == 0);

    CHECK(ClassifyPlayheadMove(1200, 1750, kMaxElapsed, step) == MoveKind::ForwardStep);
    CHECK(step == 550);

    // Reverse is not a forward step of the same size: the engine's dt is always positive, so the
    // honest answer is to arm nothing rather than to integrate the wrong way.
    step = 99;
    CHECK(ClassifyPlayheadMove(1750, 1200, kMaxElapsed, step) == MoveKind::Reverse);
    CHECK(step == 0);

    // A jump longer than one clamped beat is a seek. Commanding it would have the engine clamp to
    // the maximum and discard the rest silently, with the playhead insisting it moved.
    step = 99;
    CHECK(ClassifyPlayheadMove(0, static_cast<int64_t>(kMaxElapsed) + 1, kMaxElapsed, step) ==
          MoveKind::Jump);
    CHECK(step == 0);
    CHECK(ClassifyPlayheadMove(0, static_cast<int64_t>(kMaxElapsed), kMaxElapsed, step) ==
          MoveKind::ForwardStep);
    CHECK(step == kMaxElapsed);

    // A seek back to the very start, which is the case B.5's signed note exists for: an unsigned
    // playhead would underflow to 1.8e19 and be read as an enormous FORWARD move.
    CHECK(ClassifyPlayheadMove(60000000, 0, kMaxElapsed, step) == MoveKind::Reverse);
}

// ---------------------------------------------------------------------------------------------
// ★★★ THE SWEEP. A discontinuity in the instrument must never become a rate reading.
//
// 60 simulated seconds per cell, at three render rates and six commanded rates, with the sim clock
// RESET on a schedule -- which is what a level load, a second CClock construction or an engine
// state change does to CClock+0x28. The classified meter must report the commanded rate throughout;
// the naive one is asserted to be wrong, which is what proves the classification is load-bearing
// rather than decorative.
// ---------------------------------------------------------------------------------------------
TEST_CASE("a reset clock never masquerades as a rate") {
    const double renderHz[] = {60.0, 90.0, 144.0};
    const double rates[] = {0.1, 0.25, 1.0, 2.0, 4.0, 6.0};

    for (size_t h = 0; h < 3; ++h) {
        for (size_t k = 0; k < 6; ++k) {
            const double periodUs = 1000000.0 / renderHz[h];
            const double rate = rates[k];
            const uint64_t frames = static_cast<uint64_t>(60.0 * renderHz[h]);
            const uint64_t resetEvery = 173;   // prime, so it lands at a different phase each cell

            RateMeter meter;
            meter.Reset();
            NaiveMeter naive;

            uint64_t sim = 500000;
            uint64_t wallTicks = 0;
            const uint64_t freq = 10000000;  // facts-ok: CLOCK.CTOR_MAXELAPSED_INIT 10,000,000 in tick arithmetic, not the clock constructor's initialiser
            uint64_t prevSim = 0, prevWall = 0;
            uint32_t prevFrame = 0;
            bool started = false;
            uint64_t resets = 0, seenRegressions = 0;

            for (uint64_t f = 1; f <= frames; ++f) {
                // The engine advances sim by rate x the real frame, clamped as
                // TICK.HEARTBEAT_BEFORE clamps it. Beats 2 and 3 fire microseconds after our tick
                // and carry almost no raw time, so beat 1 carries the frame and the clamp that
                // binds is ONE CLOCK.MAXELAPSED_US -- which is why the ceiling is 4.0x at 60 Hz and
                // not 12.0x, and is the premise Ceiling() is built on.
                const uint64_t advance = static_cast<uint64_t>(rate * periodUs + 0.5);
                const uint64_t clamped = advance > kMaxElapsed ? kMaxElapsed : advance;
                sim += clamped;
                wallTicks += static_cast<uint64_t>(periodUs * (double)freq / 1000000.0);

                if (f % resetEvery == 0) {
                    sim = 0;   // the instrument, not the thing being measured
                    ++resets;
                }

                const uint32_t frame = static_cast<uint32_t>(f);
                if (!started) {
                    started = true;
                } else {
                    uint64_t simDelta = 0, wallDelta = 0;
                    const SampleKind sk =
                        ClassifySim(prevSim, sim, frame - prevFrame, Bounds(), simDelta);
                    const SampleKind wk = ClassifyWall(prevWall, wallTicks, freq, 60000000, wallDelta);
                    if (sk == SampleKind::Regressed) {
                        ++seenRegressions;
                    }
                    if (Usable(sk) && wk == SampleKind::Normal && wallDelta > 0) {
                        meter.Feed(simDelta, wallDelta);
                    } else {
                        meter.Reject();
                    }
                    naive.Sample(sim, (wallTicks * 1000000ull) / freq);
                }
                prevSim = sim;
                prevWall = wallTicks;
                prevFrame = frame;
            }

            // ★ EXACT, not "greater than zero". Every injected reset must be seen as exactly one
            // regression; a count that only has to be positive passes for an implementation that
            // notices one of them.
            CHECK(seenRegressions == resets);
            CHECK(resets > 0);

            // What the engine can actually deliver at this rate on this display: the commanded
            // rate, or the ceiling, whichever is smaller. At 60 Hz that caps 6.0x at 4.0x, which is
            // the silent-saturation case the whole ceiling exists for.
            const double want = rate < Ceiling(kMaxElapsed, periodUs)
                                    ? rate
                                    : Ceiling(kMaxElapsed, periodUs);
            double got = 0.0;
            REQUIRE(meter.Achieved(got));
            CHECK(got == doctest::Approx(want).epsilon(0.02));

            // The red arm, run rather than described. Without the classification the resets enter
            // the sum as ~1.8e19 apiece and the reported rate is not merely imprecise -- it is off
            // by fifteen orders of magnitude, which is what a discontinuity looks like when it is
            // mistaken for a reading.
            CHECK(naive.Rate() > want * 1e6);
        }
    }
}

TEST_CASE("a stalled clock and a reset clock are not the same reading") {
    // Both produce a frame delta of zero once the difference has been taken -- and W1-D's own
    // frameDeltaUs reports a regression AS zero, which is why this module keeps its own previous
    // sample rather than consuming that field. Three causes, one value, and only the classification
    // separates them.
    uint64_t d = 1;
    const SampleKind stalled = ClassifySim(1200, 1200, 1, Bounds(), d);
    CHECK(stalled == SampleKind::Stalled);
    CHECK(d == 0);

    d = 1;
    const SampleKind reset = ClassifySim(1200, 0, 1, Bounds(), d);
    CHECK(reset == SampleKind::Regressed);
    CHECK(d == 0);

    CHECK(stalled != reset);
    CHECK(Usable(stalled));
    CHECK_FALSE(Usable(reset));

    // And a paused engine is a legitimate, feedable measurement of a rate of zero -- not a rejection.
    RateMeter m;
    m.Reset();
    for (int i = 0; i < 50; ++i) {
        m.Feed(0, 16666);
    }
    double r = 1.0;
    REQUIRE(m.Achieved(r));
    CHECK(r == doctest::Approx(0.0).epsilon(0.0001));
}

TEST_CASE("every kind has a name, and the names are distinct") {
    const SampleKind ks[] = {SampleKind::First, SampleKind::Normal, SampleKind::Stalled,
                             SampleKind::Regressed, SampleKind::Implausible};
    for (size_t i = 0; i < 5; ++i) {
        CHECK(SampleKindName(ks[i])[0] != '?');
        for (size_t j = i + 1; j < 5; ++j) {
            CHECK(SampleKindName(ks[i]) != SampleKindName(ks[j]));
        }
    }
    const MoveKind ms[] = {MoveKind::None, MoveKind::ForwardStep, MoveKind::Reverse, MoveKind::Jump};
    for (size_t i = 0; i < 4; ++i) {
        CHECK(MoveKindName(ms[i])[0] != '?');
    }
}

// ---------------------------------------------------------------------------------------------
// ★★★ THE RELEASE CHECKS, RUN AGAINST THE READINGS THAT FAILED IN THE SWEEP.
//
// runs/20260830-022305Z/launch-10/probes/probe_clock/done.json is the only FAIL this probe has
// produced: restored-on-release, with release.flags "0x2" and minElapsed {atTakeUs 0,
// afterReleaseUs 1}. Those two numbers are the inputs below, so these are not assertions about a
// check that has never met the thing it is for -- S9.18, the strongest evidence this project has.
//
// The FIX is in src/engine/clock.cpp (Release now latches, so the next tick cannot re-take), and
// these cases do NOT prove the fix: an offline suite writes no CClock. They prove the CHECK -- that
// the reading which was scored FAIL is scored FAIL by this code, that a restored clock is scored
// OK, that neither reading can be reached by accident, and that the detail line changes with the
// reading, which is the specific way the old check misreported the sweep.
// ---------------------------------------------------------------------------------------------

namespace {

using replay::clockcheck::Outcome;
using replay::clockcheck::ReleaseReading;

// Launch 10 AS RECORDED: the fields done.json carries. release.flags 0x2, minElapsed.afterReleaseUs
// 1 against atTakeUs 0 (the engine's own minimum, which is what makes a 1 here OUR floor rather
// than the engine's value), and status {read, held: true} from phase 5, before the release -- which
// is the "ever held" witness. The post-release status fields are NOT here: launch 10's probe did
// not read Status() after Release(), so the check that needs them is fed below from a substitute
// and says so.
ReleaseReading LaunchTenAsRecorded() {
    ReleaseReading r;
    r.everHeldRead = true;
    r.everHeld = true;
    r.released = true;
    r.registersRead = true;
    r.flags = 0x2u;
    r.minElapsedUs = 1u;
    r.minElapsedAtTakeRead = true;
    r.minElapsedAtTakeUs = 0u;
    r.statusRead = false;
    r.refusal = replay::ClockRefusal::None;
    r.held = false;
    r.latched = false;
    return r;
}

// Launch 10 plus a SUBSTITUTED post-release status: the phase-5 reading (held=true, refusal=none)
// standing in for the one the probe did not take. The fix's own diagnosis says the module had
// re-taken by then, so held=true is what that read would have returned; a module with no latch
// reports released=false. Reconstructed, not recorded -- the S9.18 claim for StoppedAfterRelease is
// exactly that much weaker than for the other two.
ReleaseReading LaunchTenWithSubstitutedStatus() {
    ReleaseReading r = LaunchTenAsRecorded();
    r.statusRead = true;
    r.held = true;
    r.latched = false;
    r.refusal = replay::ClockRefusal::None;
    return r;
}

// What the same probe must report once the module stays released: our two bits clear with the
// engine's own bit 0 set, the engine's minimum back, and the module saying it has stopped.
ReleaseReading Restored() {
    ReleaseReading r = LaunchTenWithSubstitutedStatus();
    r.flags = 0x1u;
    r.minElapsedUs = 0u;
    r.refusal = replay::ClockRefusal::Released;
    r.held = false;
    r.latched = true;
    return r;
}

// A run in which Take() refused and the probe walked to phase 6 anyway: the registers read exactly
// as a perfect release would, and the module reports the reason it never took the clock.
ReleaseReading NeverTaken() {
    ReleaseReading r = Restored();
    r.everHeld = false;
    r.flags = 0x1u;
    r.minElapsedUs = 0u;
    r.minElapsedAtTakeUs = 0u;
    r.refusal = replay::ClockRefusal::NoFloor;
    r.held = false;
    r.latched = true;
    return r;
}

}  // namespace

TEST_CASE("the two register checks FAIL on the readings launch 10 recorded") {
    const ReleaseReading r = LaunchTenAsRecorded();
    char d[256];

    CHECK(replay::clockcheck::FlagsRestored(r, d, sizeof d) == Outcome::Failed);
    CHECK(strstr(d, "0x2") != nullptr);        // it prints what it saw
    CHECK(strstr(d, "PAUSE bit 1") != nullptr);
    CHECK(strstr(d, "STEPONCE") == nullptr);   // and only what it saw: bit 2 was clear

    CHECK(replay::clockcheck::FloorRestored(r, d, sizeof d) == Outcome::Failed);
    CHECK(strstr(d, "OUR FLOOR IS STILL INSTALLED") != nullptr);

    // Launch 10 did not read Status() after Release(), and this check does not pretend it did.
    CHECK(replay::clockcheck::StoppedAfterRelease(r, d, sizeof d) == Outcome::Undetermined);
    CHECK(strstr(d, "Status() was not read after Release()") != nullptr);
}

TEST_CASE("the stop check FAILS on launch 10 with the phase-5 status standing in") {
    // The cause, not the effect -- fed a reconstruction, which the fixture's own comment discloses.
    const ReleaseReading r = LaunchTenWithSubstitutedStatus();
    char d[256];
    CHECK(replay::clockcheck::StoppedAfterRelease(r, d, sizeof d) == Outcome::Failed);
    CHECK(strstr(d, "taken the clock again") != nullptr);
    CHECK(strstr(d, "held=true") != nullptr);
}

TEST_CASE("a clock nobody took is NOT-RUN in all three, and says why") {
    // Every register reads as a perfect release would, and the module reads as stopped. Without
    // the engaged precondition all three of these are Ok for a run in which nothing was written.
    const ReleaseReading r = NeverTaken();
    char d[256];
    CHECK(replay::clockcheck::FlagsRestored(r, d, sizeof d) == Outcome::NotRun);
    CHECK(strstr(d, "never held the clock") != nullptr);
    CHECK(strstr(d, "refusal=7") != nullptr);   // NoFloor, printed as read
    CHECK(replay::clockcheck::FloorRestored(r, d, sizeof d) == Outcome::NotRun);
    CHECK(strstr(d, "never held the clock") != nullptr);
    CHECK(replay::clockcheck::StoppedAfterRelease(r, d, sizeof d) == Outcome::NotRun);
    CHECK(strstr(d, "never held the clock") != nullptr);
    CHECK(strcmp(replay::clockcheck::OutcomeName(Outcome::NotRun), "not-run") == 0);

    // And "ever held" unknown is could-not-ask, not not-run and not ok.
    ReleaseReading u = Restored();
    u.everHeldRead = false;
    CHECK(replay::clockcheck::FlagsRestored(u, d, sizeof d) == Outcome::Undetermined);
    CHECK(replay::clockcheck::FloorRestored(u, d, sizeof d) == Outcome::Undetermined);
    CHECK(replay::clockcheck::StoppedAfterRelease(u, d, sizeof d) == Outcome::Undetermined);
    CHECK(strstr(d, "never read") != nullptr);
}

TEST_CASE("a refusal that survives the release is what tells never-taken from released") {
    // The module keeps NoFloor (or any other reason) through Release(); if it stamped Released
    // over it, NeverTaken() would carry refusal=Released and the reader could not tell the two
    // runs apart from the status line. The check reads that refusal into its detail.
    ReleaseReading r = NeverTaken();
    char d[256];
    replay::clockcheck::FlagsRestored(r, d, sizeof d);
    char e[256];
    r.refusal = replay::ClockRefusal::SiteMismatch;
    replay::clockcheck::FlagsRestored(r, e, sizeof e);
    CHECK(strcmp(d, e) != 0);
}

TEST_CASE("the release checks PASS on a clock that was actually put back") {
    const ReleaseReading r = Restored();
    char d[256];
    CHECK(replay::clockcheck::FlagsRestored(r, d, sizeof d) == Outcome::Ok);
    CHECK(replay::clockcheck::FloorRestored(r, d, sizeof d) == Outcome::Ok);
    CHECK(replay::clockcheck::StoppedAfterRelease(r, d, sizeof d) == Outcome::Ok);
}

TEST_CASE("the detail changes with the reading") {
    // The defect in the check the sweep ran: one sentence for both outcomes, so a FAIL was reported
    // in words asserting the opposite. A detail that cannot change cannot be read.
    char pass[256], fail[256];
    replay::clockcheck::FlagsRestored(Restored(), pass, sizeof pass);
    replay::clockcheck::FlagsRestored(LaunchTenAsRecorded(), fail, sizeof fail);
    CHECK(strcmp(pass, fail) != 0);
    replay::clockcheck::FloorRestored(Restored(), pass, sizeof pass);
    replay::clockcheck::FloorRestored(LaunchTenAsRecorded(), fail, sizeof fail);
    CHECK(strcmp(pass, fail) != 0);
}

TEST_CASE("the flags check owns two bits and no others") {
    ReleaseReading r = Restored();
    char d[256];

    // The near-miss control. TICK.UPDATE_CLOCK read-modify-writes this qword every frame, so every
    // bit EXCEPT ours may legitimately differ from anything we snapshotted.
    r.flags = ~(replay::clock_facts::kPauseBit | replay::clock_facts::kStepOnceBit);
    CHECK(replay::clockcheck::FlagsRestored(r, d, sizeof d) == Outcome::Ok);

    r.flags = replay::clock_facts::kStepOnceBit;
    CHECK(replay::clockcheck::FlagsRestored(r, d, sizeof d) == Outcome::Failed);
    CHECK(strstr(d, "STEPONCE bit 2") != nullptr);
    CHECK(strstr(d, "PAUSE") == nullptr);

    r.flags = replay::clock_facts::kPauseBit | replay::clock_facts::kStepOnceBit;
    CHECK(replay::clockcheck::FlagsRestored(r, d, sizeof d) == Outcome::Failed);
    CHECK(strstr(d, "PAUSE bit 1 and STEPONCE bit 2") != nullptr);
}

TEST_CASE("a floor that equals the engine's own value is UNDETERMINED, not OK") {
    // Equal-valued fields are untestable: with 1 us at take, a restore and a leaked floor produce
    // the same reading, and calling that a pass is how a check comes to certify the defect.
    ReleaseReading r = Restored();
    r.minElapsedAtTakeUs = replay::clock_facts::kFloorUs;
    r.minElapsedUs = replay::clock_facts::kFloorUs;
    char d[256];
    CHECK(replay::clockcheck::FloorRestored(r, d, sizeof d) == Outcome::Undetermined);

    // A third value is neither ours nor the engine's, and is still a failure.
    r.minElapsedAtTakeUs = 0u;
    r.minElapsedUs = 7u;
    CHECK(replay::clockcheck::FloorRestored(r, d, sizeof d) == Outcome::Failed);
    CHECK(strstr(d, "neither ours nor the engine's") != nullptr);
}

TEST_CASE("could-not-ask is its own outcome in all three") {
    char d[256];
    ReleaseReading r = Restored();
    r.released = false;
    CHECK(replay::clockcheck::FlagsRestored(r, d, sizeof d) == Outcome::Undetermined);
    CHECK(replay::clockcheck::FloorRestored(r, d, sizeof d) == Outcome::Undetermined);
    CHECK(replay::clockcheck::StoppedAfterRelease(r, d, sizeof d) == Outcome::Undetermined);

    r = Restored();
    r.registersRead = false;
    CHECK(replay::clockcheck::FlagsRestored(r, d, sizeof d) == Outcome::Undetermined);
    CHECK(replay::clockcheck::FloorRestored(r, d, sizeof d) == Outcome::Undetermined);

    r = Restored();
    r.minElapsedAtTakeRead = false;
    CHECK(replay::clockcheck::FloorRestored(r, d, sizeof d) == Outcome::Undetermined);

    r = Restored();
    r.statusRead = false;
    CHECK(replay::clockcheck::StoppedAfterRelease(r, d, sizeof d) == Outcome::Undetermined);
}

TEST_CASE("not-held is not the same as released") {
    // A module between takes also reports held=false. Only the refusal says it will not take again,
    // which is the whole content of the fix.
    ReleaseReading r = Restored();
    r.refusal = replay::ClockRefusal::None;
    char d[256];
    CHECK(replay::clockcheck::StoppedAfterRelease(r, d, sizeof d) == Outcome::Failed);

    // A held clock that was released must say Released; any other reason with everHeld is a
    // module whose refusal and latch disagree.
    r.refusal = replay::ClockRefusal::NoGlobalClock;
    CHECK(replay::clockcheck::StoppedAfterRelease(r, d, sizeof d) == Outcome::Failed);

    // The latch is the third field, and it is read on its own: held=false and refusal=released
    // with released=false is a module that will take the clock again.
    r = Restored();
    r.latched = false;
    CHECK(replay::clockcheck::StoppedAfterRelease(r, d, sizeof d) == Outcome::Failed);
    CHECK(strstr(d, "released=false") != nullptr);
}

// ---------------------------------------------------------------------------------------------
// ★ THE FLOOR READ-BACK, PER CLOCK. clock.cpp's Take() read the floor back from the global only
// and TopUpClocks() read nothing back, while Apply() set the pause bit on every clock in the
// array -- so a late clock whose +0x58 write did not stick would have been paused into +inf. The
// install now goes through one policy function for every clock; these cases drive it with a field
// that refuses writes, which is the only way to watch the refusal arm run.
// ---------------------------------------------------------------------------------------------

namespace {

struct StickyField {
    uint64_t value = 0;
    void Store(uint64_t v) { value = v; }
    uint64_t Load() const { return value; }
};

// A write that does not take: the field keeps whatever it held. Every store is counted so the
// restore can be seen to have been ATTEMPTED even though it cannot land either.
struct RefusingField {
    uint64_t value = 0;
    int stores = 0;
    uint64_t lastStored = 0;
    void Store(uint64_t v) { ++stores; lastStored = v; }
    uint64_t Load() const { return value; }
};

// A write that lands but reads back as something else: the field is not the one we think it is.
struct AliasedField {
    uint64_t value = 0;
    uint64_t shadow = 0;
    void Store(uint64_t v) { shadow = v; }
    uint64_t Load() const { return value; }
};

}  // namespace

TEST_CASE("release keeps the reason a clock was never taken, and stamps Released only over a take") {
    using replay::ClockRefusal;
    using replay::clock::RefusalAfterRelease;

    // The six ways an enabled module ends up without the clock all survive a Release().
    const ClockRefusal kept[] = {ClockRefusal::NoModule,      ClockRefusal::BuildGuard,
                                 ClockRefusal::SiteMismatch,  ClockRefusal::NoGlobalClock,
                                 ClockRefusal::NotACClock,    ClockRefusal::NoFloor};
    for (ClockRefusal before : kept) {
        CHECK(RefusalAfterRelease(false, before) == before);
    }
    // A take that was released says so, whatever it read before.
    CHECK(RefusalAfterRelease(true, ClockRefusal::None) == ClockRefusal::Released);
    CHECK(RefusalAfterRelease(true, ClockRefusal::NoFloor) == ClockRefusal::Released);
    // A release before the first attempt has no reason to keep.
    CHECK(RefusalAfterRelease(false, ClockRefusal::None) == ClockRefusal::Released);
}

TEST_CASE("the floor install reports what it read back and restores on a mismatch") {
    uint64_t back = ~0ull;

    StickyField ok;
    ok.value = 0;
    CHECK(replay::clock::InstallFloor(ok, replay::clock_facts::kFloorUs, 0, back));
    CHECK(back == replay::clock_facts::kFloorUs);
    CHECK(ok.value == replay::clock_facts::kFloorUs);

    RefusingField refuses;
    refuses.value = 0;
    back = ~0ull;
    CHECK_FALSE(replay::clock::InstallFloor(refuses, replay::clock_facts::kFloorUs, 0, back));
    CHECK(back == 0);                     // it reports the read-back, not the write
    CHECK(refuses.stores == 2);           // the floor, then the snapshot
    CHECK(refuses.lastStored == 0);       // and the snapshot is what was put back

    // The snapshot restored is the caller's, not a constant: a clock found at 5 goes back to 5.
    AliasedField aliased;
    aliased.value = 5;
    back = ~0ull;
    CHECK_FALSE(replay::clock::InstallFloor(aliased, replay::clock_facts::kFloorUs, 5, back));
    CHECK(back == 5);
    CHECK(aliased.shadow == 5);
}

// ---------------------------------------------------------------------------------------------
// ★★★ THE DECLARED COMPLETION POINT, RUN AGAINST THE LAUNCH THAT SKIPPED IT.
//
// runs/20260830-022305Z/launch-10/probes/probe_clock/done.json: ticks 953, LogSpanSec 14, the
// harness recording ReachedLevelLoad false, and the probe filing at 16.1 s of a 300 s budget under
// a descriptor that declares completesAt "match". The verdict carried nothing about any of that,
// so the sweep's headline clock result was read as a match reading. These cases are the retroactive
// proof (S9.18): the run that was scored without the caveat is scored undetermined by this code,
// and the detail names the condition it never reached.
// ---------------------------------------------------------------------------------------------

TEST_CASE("launch 10's truncated run is undetermined, and says which condition it missed") {
    char d[512];
    CHECK(replay::clockcheck::CompletionReached(false, 0, 953, d, sizeof d) == Outcome::Undetermined);
    CHECK(strstr(d, "TRUNCATED") != nullptr);
    CHECK(strstr(d, "completesAt") != nullptr);
    CHECK(strstr(d, "953") != nullptr);          // it prints what it saw, not a constant
    CHECK(strstr(d, "no match was ever entered") != nullptr);
}

TEST_CASE("a run that did reach a match is ok, and reports where it saw one") {
    char d[512];
    CHECK(replay::clockcheck::CompletionReached(true, 412, 953, d, sizeof d) == Outcome::Ok);
    CHECK(strstr(d, "TRUNCATED") == nullptr);
    CHECK(strstr(d, "412") != nullptr);
    // The two readings cannot produce the same sentence, which is how the old release checks
    // misreported a FAIL in words that asserted a PASS.
    char e[512];
    replay::clockcheck::CompletionReached(false, 0, 953, e, sizeof e);
    CHECK(strcmp(d, e) != 0);
}

// ---------------------------------------------------------------------------------------------
// ★★★ THE ENGAGEMENT GATE, RUN AGAINST THE LAUNCH IT WAS WRITTEN FOR.
//
// runs/20260902-071934Z/launch-01: `[clock] took 2 CClock(s); CLOCK.MINELAPSED_OFF was 0, now 1`
// at 03:19:41.932, with no replay ever loaded -- the transport is published by playback::Init with
// no file and reports Pause from birth. The engine's game log stops at 03:19:42 and the process
// then runs 234 more seconds with no level load. These cases are the retroactive proof (S9.18):
// the exact reading of that launch -- a transport present, DurationUs() == 0 -- must not produce
// Drive, and removing the `durationUs > 0` term from GateFor turns the first case back into it.
// ---------------------------------------------------------------------------------------------

TEST_CASE("launch 1's fileless transport is Idle, not Drive") {
    // The reading at 03:19:41.932: transport published, no file, nothing held, nothing released.
    CHECK(GateFor(true, 0, false, false) == ClockGate::Idle);
    // And it stays Idle however long the session runs: Idle writes nothing, so nothing changes.
    CHECK(GateFor(true, 0, false, false) == ClockGate::Idle);
    // No transport at all is the same answer for a different reason, and neither is Drive.
    CHECK(GateFor(false, 0, false, false) == ClockGate::Idle);
    // ★ The duration is the whole difference. One microsecond of loaded replay engages.
    CHECK(GateFor(true, 1, false, false) == ClockGate::Drive);
}

TEST_CASE("a loaded replay drives, an unloaded one hands the clock back") {
    CHECK(GateFor(true, 900000000ull, false, false) == ClockGate::Drive);
    CHECK(GateFor(true, 900000000ull, true, false) == ClockGate::Drive);
    // Unload while holding: restore, and do it once -- the next tick is Idle, not Disengage again.
    CHECK(GateFor(true, 0, true, false) == ClockGate::Disengage);
    CHECK(GateFor(true, 0, false, false) == ClockGate::Idle);
    // ★ AND A SECOND FILE RE-TAKES. Disengage is not Release: if an unload latched, every replay
    // after the first would run with the engine's clock and look like a transport that pauses
    // nothing.
    CHECK(GateFor(true, 5, false, false) == ClockGate::Drive);
}

TEST_CASE("Release outranks a loaded replay, and stays outranking it") {
    CHECK(GateFor(true, 900000000ull, true, true) == ClockGate::Stopped);
    CHECK(GateFor(true, 900000000ull, false, true) == ClockGate::Stopped);
    CHECK(GateFor(false, 0, false, true) == ClockGate::Stopped);
    // The four gate answers are four distinct answers; a gate collapsing any pair is the shrug
    // this enum exists to prevent.
    CHECK(GateFor(true, 1, false, false) != GateFor(true, 0, false, false));
    CHECK(GateFor(true, 0, true, false) != GateFor(true, 0, false, false));
    CHECK(GateFor(true, 1, false, true) != GateFor(true, 1, false, false));
}

// ---------------------------------------------------------------------------------------------
// Launch 07:37 (2026-09-18): the owner quit with a replay loaded, the clock still held. Its
// restore ran from DLL_PROCESS_DETACH, 2 s after LogOut, and stored the saved 1.0f timescale into
// a CClock the engine had already freed -- unhandled 0xC0000005 at [rcx+0x38].
// ---------------------------------------------------------------------------------------------

TEST_CASE("a session that ends under a loaded replay hands the clock back, once") {
    replay::SessionWatch w;
    // Loading: a replay is open and the match has not started. Not an ended session.
    CHECK_FALSE(w.Ended(false));
    CHECK(GateForSession(true, 900000000ull, false, false, false) == ClockGate::Drive);
    CHECK_FALSE(w.Ended(true));
    CHECK_FALSE(w.Ended(true));
    // The owner leaves with the file still loaded and the clock held.
    CHECK(w.Ended(false));
    CHECK(GateForSession(true, 900000000ull, true, false, true) == ClockGate::Disengage);
    CHECK(GateForSession(true, 900000000ull, false, false, true) == ClockGate::Idle);
    // A second match under the same file takes it again.
    CHECK_FALSE(w.Ended(true));
    CHECK(GateForSession(true, 900000000ull, false, false, false) == ClockGate::Drive);
    CHECK(w.Ended(false));
    // Release still outranks everything.
    CHECK(GateForSession(true, 900000000ull, true, true, true) == ClockGate::Stopped);
}

TEST_CASE("the release a session owes is owed again by the next session in the same process") {
    replay::SessionWatch w;
    CHECK_FALSE(w.Ended(false));      // loading
    CHECK_FALSE(w.Ended(true));       // in match: claims are made here
    CHECK(w.Ended(false));            // it ended
    CHECK(w.ReleaseDue());            // ...and the release runs
    CHECK_FALSE(w.ReleaseDue());      // once, not once per tick
    CHECK_FALSE(w.ReleaseDue());

    // A private-lobby rematch in the SAME process. Without the rising edge clearing the latch the
    // second match claims every body and the release is never owed again -- they stay net-driven
    // with nothing driving them until the module is torn down.
    // ReleaseDue is only ever asked under an ended session; the rising edge is what re-arms it.
    CHECK_FALSE(w.Ended(true));
    CHECK(w.Ended(false));
    CHECK(w.ReleaseDue());
    CHECK_FALSE(w.ReleaseDue());

    // And a third, to prove it is a cycle and not a one-off reset.
    CHECK_FALSE(w.Ended(true));
    CHECK(w.Ended(false));
    CHECK(w.ReleaseDue());
}

TEST_CASE("a second replay in the same process loads its level as the first did") {
    replay::SessionWatch w;
    CHECK_FALSE(w.Ended(1u, false));   // first load, level loading
    CHECK_FALSE(w.Ended(1u, true));
    CHECK(w.Ended(1u, false));         // its session ended: the clock is handed back
    CHECK(w.Ended(1u, false));
    // The owner opens another file. Out of match again, and not an ended session: the clock is
    // driven through this load exactly as it was through the first.
    CHECK_FALSE(w.Ended(2u, false));
    CHECK(GateForSession(true, 900000000ull, false, false, w.Ended(2u, false)) == ClockGate::Drive);
    CHECK_FALSE(w.Ended(2u, true));
    CHECK(w.Ended(2u, false));
}

namespace {

constexpr RegisterLayout kTestLayout = {0x38, 0x58, 0x50, 0x6};
constexpr size_t kFakeClockBytes = 0x70;

const void* g_aliveAsked = nullptr;
bool ClockIsAlive(const void* clock) { g_aliveAsked = clock; return true; }
bool ClockIsGone(const void* clock) { g_aliveAsked = clock; return false; }

void FillHeldClock(uint8_t* clock) {
    memset(clock, 0xA5, kFakeClockBytes);
    const uint64_t flags = 0xF0F6;   // our pause and step-once bits set among the engine's own
    memcpy(clock + kTestLayout.flagsOff, &flags, sizeof flags);
}

}  // namespace

TEST_CASE("a live clock gets its three registers back and nothing else is touched") {
    uint8_t clock[kFakeClockBytes];
    uint8_t before[kFakeClockBytes];
    FillHeldClock(clock);
    memcpy(before, clock, sizeof before);
    g_aliveAsked = nullptr;

    const SavedRegisters saved = {0x3F800000u, 0x1122334455667788ull};  // facts-ok: AUDIO.MAXFREQRATIO_BITS a test fixture value this case compares against, not an engine position
    REQUIRE(RestoreRegisters(clock, kTestLayout, saved, &ClockIsAlive));
    CHECK(g_aliveAsked == clock);

    uint32_t timescale = 0;
    uint64_t minElapsed = 0, flags = 0;
    memcpy(&timescale, clock + 0x38, sizeof timescale);
    memcpy(&minElapsed, clock + 0x58, sizeof minElapsed);
    memcpy(&flags, clock + 0x50, sizeof flags);
    CHECK(timescale == 0x3F800000u);  // facts-ok: AUDIO.MAXFREQRATIO_BITS a test fixture value this case compares against, not an engine position
    CHECK(minElapsed == 0x1122334455667788ull);
    CHECK(flags == 0xF0F0);   // our two bits cleared, the engine's kept

    // Outside the three registers the object is byte-identical.
    for (size_t i = 0; i < kFakeClockBytes; ++i) {
        const bool ours = (i >= 0x38 && i < 0x3C) || (i >= 0x50 && i < 0x60);
        if (!ours) {
            CHECK(clock[i] == before[i]);
        }
    }
}

TEST_CASE("a clock the engine has freed is asked about and then left alone") {
    uint8_t clock[kFakeClockBytes];
    uint8_t before[kFakeClockBytes];
    FillHeldClock(clock);
    memcpy(before, clock, sizeof before);
    g_aliveAsked = nullptr;

    const SavedRegisters saved = {0x3F800000u, 0x1122334455667788ull};  // facts-ok: AUDIO.MAXFREQRATIO_BITS a test fixture value this case compares against, not an engine position
    CHECK_FALSE(RestoreRegisters(clock, kTestLayout, saved, &ClockIsGone));
    CHECK(g_aliveAsked == clock);
    CHECK(memcmp(clock, before, sizeof before) == 0);

    // No probe is no evidence of life, and a null clock is not asked about at all.
    CHECK_FALSE(RestoreRegisters(clock, kTestLayout, saved, nullptr));
    CHECK(memcmp(clock, before, sizeof before) == 0);
    g_aliveAsked = nullptr;
    CHECK_FALSE(RestoreRegisters(nullptr, kTestLayout, saved, &ClockIsAlive));
    CHECK(g_aliveAsked == nullptr);
}

// =================================================================================================
// The unstepped-clock warning. Launch 5's echoreplay.log is 33 279 lines and ~33 000 of them are
// one warning, re-emitted by TopUpClocks every tick for the life of the process. The diagnosis has
// to survive; the volume must not. AddClock itself cannot run here (IsCClock refuses any image
// that is not echovr.exe), so the throttle lives in clock_policy.h and is driven directly.
// =================================================================================================

TEST_CASE("the same unstepped clock is reported once and counted thereafter") {
    UnsteppedReporter r;
    const void* third = reinterpret_cast<const void*>(0x27193BE7C08ull);

    CHECK(r.ShouldLog("third", third));   // the transition: this is the line that is logged
    int logged = 0;
    for (int i = 0; i < 33000; ++i) {
        if (r.ShouldLog("third", third)) ++logged;
    }
    CHECK(logged == 0);                   // ... and these are the 33 000 that were not
    CHECK(r.Reports() == 33001);
    CHECK(r.Suppressed() == 33000);

    SUBCASE("a different object under the same name is a new fact, so it is said") {
        const void* other = reinterpret_cast<const void*>(0x27193BE8000ull);
        CHECK(r.ShouldLog("third", other));
        CHECK(r.Suppressed() == 33000);
        CHECK_FALSE(r.ShouldLog("third", other));
        CHECK(r.Suppressed() == 33001);
    }

    SUBCASE("a name that was accepted since it last refused is re-armed") {
        r.Accepted("third");
        CHECK(r.ShouldLog("third", third));
        CHECK(r.Suppressed() == 33000);
    }

    SUBCASE("accepting one name does not un-silence another") {
        CHECK(r.ShouldLog("game", third));
        r.Accepted("game");
        CHECK_FALSE(r.ShouldLog("third", third));
    }
}

TEST_CASE("two unstepped clocks alternating do not defeat the throttle") {
    // The failure a single-slot latch would have: global and third refused on the same tick, each
    // one displacing the other's record, and the whole 33 000 lines come back.
    UnsteppedReporter r;
    const void* a = reinterpret_cast<const void*>(0x1000ull);
    const void* b = reinterpret_cast<const void*>(0x2000ull);  // facts-ok: PHYS.CI_PAUSED_BIT a test fixture value this case compares against, not an engine position

    CHECK(r.ShouldLog("global", a));
    CHECK(r.ShouldLog("third", b));
    int logged = 0;
    for (int i = 0; i < 500; ++i) {
        if (r.ShouldLog("global", a)) ++logged;
        if (r.ShouldLog("third", b)) ++logged;
    }
    CHECK(logged == 0);
    CHECK(r.Reports() == 1002);
    CHECK(r.Suppressed() == 1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
}

TEST_CASE("a name the reporter cannot track is never silenced") {
    // Four slots for three clocks. If the engine ever grows a fourth name, running out of slots
    // must degrade to logging everything -- losing the warning entirely is the one outcome that
    // is worse than the volume.
    UnsteppedReporter r;
    const void* obj = reinterpret_cast<const void*>(0x3000ull);
    const char* names[] = {"global", "third", "game", "spare", "overflow"};
    for (int i = 0; i < 5; ++i) CHECK(r.ShouldLog(names[i], obj));
    for (int i = 0; i < 4; ++i) CHECK_FALSE(r.ShouldLog(names[i], obj));
    CHECK(r.ShouldLog("overflow", obj));   // no slot to remember it by, so it is still reported
    CHECK(r.Suppressed() == 4);
}

// -------------------------------------------------------------------------------------------
// SCRUB-BODIES. The owner dragged the timeline and the hands and the held disc followed while the
// player BODIES stood still.
//
// ★ THE FAKE HAS A GATE. Every body double in this project until now returned whatever was last
// installed into it, which is why the suite was green while the game disagreed: the engine
// re-evaluates a body's RENDERED transform out of its history window only on a tick its clock
// BEAT on, and a drag armed no beats. FakeBody below is that gate and nothing else, so a version
// of the policy that arms nothing leaves `rendered` at the pose it started on -- visibly, in a
// CHECK -- instead of quietly agreeing with the install.
// -------------------------------------------------------------------------------------------
namespace {

struct FakeBody {
    int64_t rendered = 0;    // what the renderer would draw, named by the playhead it came from
    int64_t installed = 0;   // the one-node window the plugin replaces whole every tick

    void Install(int64_t playheadUs) { installed = playheadUs; }
    void Beat(uint64_t armUs) {
        if (armUs != 0) {
            rendered = installed;
        }
    }
};

// One frame, in the order the shipped code runs it: the module installs and pushes its per-frame
// step, then the clock mirrors the transport (clock.cpp MirrorTransport) and writes (Apply).
struct MirrorSim {
    uint64_t pendingUs = 0;
    bool armedLastFrame = false;
    bool havePrev = false;
    int64_t prevPlayhead = 0;
    uint64_t armed = 0;
    uint64_t refusedBusy = 0;

    uint64_t Frame(TransportState state, float rate, int64_t playheadUs, uint64_t pushUs,
                   bool preRoll, FakeBody& body) {
        body.Install(playheadUs);
        if (pushUs != 0) {
            pendingUs = pushUs;   // playback_module's SetStepUs, suppressed during a drag (F4)
        }
        const bool paused = preRoll ? false : EngineHeld(state, rate);
        if (!preRoll && havePrev && paused) {
            uint64_t step = 0;
            const MoveKind mk = ClassifyPlayheadMove(prevPlayhead, playheadUs, kMaxElapsed, step);
            const uint64_t beat = StepForMove(mk, step, kFloorUs);
            if (beat != 0) {
                pendingUs = beat;
            }
        }
        prevPlayhead = playheadUs;
        havePrev = true;
        const StepArmPlan sp = PlanStepArm(true, paused, pendingUs, armedLastFrame, kMaxElapsed);
        if (sp.refused) {
            ++refusedBusy;
        }
        if (!sp.carry) {
            pendingUs = 0;
        }
        armedLastFrame = sp.armUs != 0;
        if (sp.armUs != 0) {
            ++armed;
        }
        body.Beat(sp.armUs);
        return sp.armUs;
    }
};

// A drag as the owner makes one: the bar is dragged, so every frame carries a new target and the
// per-frame 1 us push is suppressed. Returns the last target the body actually latched.
int64_t Drag(MirrorSim& m, FakeBody& body, int64_t fromUs, int64_t deltaUs, int frames) {
    int64_t latched = body.rendered;
    int64_t ph = fromUs;
    for (int i = 0; i < frames; ++i) {
        ph += deltaUs;
        if (m.Frame(TransportState::Scrub, 1.0f, ph, 0, false, body) != 0) {
            CHECK(body.rendered == ph);   // the beat lands on the window installed THIS frame
            latched = ph;
        }
    }
    return latched;
}

}  // namespace

TEST_CASE("scrub_step_arms_a_beat_like_a_paused_seek") {
    // The owner's own cadence: 105 ms of playhead per step, which is LARGER than the engine's
    // max elapsed, so a drag step is a Jump and not a ForwardStep. Both need a beat.
    const int64_t kStep = 105041;
    const int64_t kStart = 861962787;

    SUBCASE("a forward drag moves the bodies, step after step") {
        MirrorSim m;
        FakeBody body;
        body.rendered = kStart;
        m.Frame(TransportState::Pause, 1.0f, kStart, 1, false, body);   // the drag's first frame
        const int64_t last = Drag(m, body, kStart, kStep, 12);
        CHECK(m.armed >= 5);            // the every-other-frame guard halves it, and may not more
        CHECK(body.rendered == last);
        CHECK(body.rendered >= kStart + 10 * kStep);
        CHECK(m.pendingUs == 0);        // nothing left owed at the end of the drag
    }

    SUBCASE("a backward drag moves them too -- the engine has no backwards dt, it needs a beat") {
        MirrorSim m;
        FakeBody body;
        body.rendered = kStart;
        m.Frame(TransportState::Pause, 1.0f, kStart, 1, false, body);
        const int64_t last = Drag(m, body, kStart, -kStep, 12);
        CHECK(m.armed >= 5);
        CHECK(body.rendered == last);
        CHECK(body.rendered <= kStart - 10 * kStep);
    }

    SUBCASE("a drag made while PLAYING is held exactly like one made while paused") {
        // BeginScrub pauses the transport but keeps the pre-drag signed rate, so +1.00x here is
        // the state the mirror actually sees -- the shape that read as "playing" before.
        CHECK(EngineHeld(TransportState::Scrub, 1.0f));
        MirrorSim m;
        FakeBody body;
        body.rendered = kStart;
        m.Frame(TransportState::Scrub, 1.0f, kStart, 0, false, body);
        CHECK(Drag(m, body, kStart, kStep, 6) > kStart);
    }

    SUBCASE("-1f: one backward move while paused arms one floor-sized beat") {
        MirrorSim m;
        FakeBody body;
        body.rendered = kStart;
        m.Frame(TransportState::Pause, 1.0f, kStart, 0, false, body);
        const int64_t back = kStart - 33333;  // facts-ok: CLOCK.FIXEDSTEP_30HZ_US the 30 Hz step period restated as this case's own cadence fixture
        // The 1 us per-frame push is present here: -1f is not a drag, so F4 does not suppress it.
        CHECK(m.Frame(TransportState::Pause, 1.0f, back, 1, false, body) == kFloorUs);
        CHECK(body.rendered == back);
    }

    SUBCASE("a step the busy guard refuses is carried, not dropped") {
        MirrorSim m;
        FakeBody body;
        body.rendered = kStart;
        m.Frame(TransportState::Pause, 1.0f, kStart, 0, false, body);
        const int64_t a = kStart + kStep;
        const int64_t b = a + kStep;
        CHECK(m.Frame(TransportState::Scrub, 1.0f, a, 0, false, body) != 0);
        CHECK(body.rendered == a);
        CHECK(m.Frame(TransportState::Scrub, 1.0f, b, 0, false, body) == 0);   // guard says no
        CHECK(m.refusedBusy == 1);
        CHECK(m.pendingUs != 0);                                               // ... and keeps it
        // The drag stops here. The carried step still lands, on the window installed at b.
        CHECK(m.Frame(TransportState::Scrub, 1.0f, b, 0, false, body) != 0);
        CHECK(body.rendered == b);
    }

    SUBCASE("a drag during the pre-roll arms nothing, and the drag after it arms again") {
        MirrorSim m;
        FakeBody body;
        body.rendered = kStart;
        for (int i = 1; i <= 4; ++i) {
            CHECK(m.Frame(TransportState::Scrub, 1.0f, kStart + i * kStep, 0, true, body) == 0);
        }
        CHECK(body.rendered == kStart);      // the pre-roll owns the clock; the mirror imposes nothing
        CHECK(m.armed == 0);
        CHECK(Drag(m, body, kStart + 4 * kStep, kStep, 4) > kStart);
    }

    SUBCASE("EndScrub hands the pacing back: Play at 1.00x is not held and arms nothing") {
        CHECK_FALSE(EngineHeld(TransportState::Play, 1.0f));
        CHECK(EngineHeld(TransportState::Play, -1.0f));   // reverse stays engine-paused
        CHECK(EngineHeld(TransportState::Pause, 1.0f));
        MirrorSim m;
        FakeBody body;
        body.rendered = kStart;
        for (int i = 1; i <= 6; ++i) {
            CHECK(m.Frame(TransportState::Play, 1.0f, kStart + i * 16666, 0, false, body) == 0);
        }
        CHECK(m.armed == 0);
        CHECK(body.rendered == kStart);   // free-running play needs no step-once from us
    }

    SUBCASE("regression: a paused forward seek still arms exactly the move's own size") {
        MirrorSim m;
        FakeBody body;
        m.Frame(TransportState::Pause, 1.0f, kStart, 0, false, body);
        CHECK(m.Frame(TransportState::Pause, 1.0f, kStart + 50000, 0, false, body) == 50000);
        // And a seek too large to step is a Jump, which is a beat of the floor, never a clamp of
        // ten seconds into the engine's max elapsed.
        MirrorSim big;
        FakeBody body2;
        big.Frame(TransportState::Pause, 1.0f, kStart, 0, false, body2);
        CHECK(big.Frame(TransportState::Pause, 1.0f, kStart + 10000000, 0, false, body2) ==  // facts-ok: CLOCK.CTOR_MAXELAPSED_INIT 10,000,000 in tick arithmetic, not the clock constructor's initialiser
              kFloorUs);
        CHECK(body2.rendered == kStart + 10000000);  // facts-ok: CLOCK.CTOR_MAXELAPSED_INIT 10,000,000 in tick arithmetic, not the clock constructor's initialiser
    }
}
