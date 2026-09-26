// W2-I's oracle harness.  Not a doctest binary: it reports machine-readable coverage counters --
// arms run, floor rungs measured, oracle mutations classified -- which is what check.ps1 turns into
// GATE-RESULT, and none of that fits doctest's assertion model.  The doctest half lives in
// test_verify.cpp and covers the pieces that ARE plain unit tests.
//
// ============================================================================================
// HOW THIS HARNESS AVOIDS BEING THE THING IT IS TESTING FOR
//
// An arm is a triple: an input, an expected verdict, and an expected refusal reason.  An arm that
// merely runs and does not disagree with anything is not an arm.  Three consequences:
//
//   * EVERY ARM ASSERTS BOTH the verdict AND the reason.  An oracle that returned NOT-RUN for the
//     wrong reason -- refusing on shape when it should have refused on skew -- passes a
//     verdict-only check and is a different oracle.
//
//   * ★ EVERY GATE PARAMETER MUST HAVE AN ARM THAT SEES IT.  `--mutate-oracle` perturbs one knob of
//     O3Config and reports which arms changed verdict.  A knob no arm reacts to is a knob nothing is
//     testing, and check.ps1 fails on it.  This is the direct countermeasure to the arm that no
//     mutation could reach -- here the mutation comes first and the arm has to answer it.
//
//   * THE PASS ARMS ARE THE NEGATIVE CONTROL, and they are not trivial.  The golden pair differs on
//     every bone by 0.02 deg and 0.1 mm, so a PASS is a measurement against the threshold, and the
//     max outlier it reports is non-zero.  A gate that failed everything would fail them.
// ============================================================================================

#include "corpus.h"

#include "engine/oracletap.h"
#include "verify/floor.h"
#include "verify/o2.h"
#include "verify/o3.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

using namespace replay;
using namespace replay::verify;

namespace {

// ---------------------------------------------------------------- arm plumbing

struct ArmOutcome {
    Verdict      verdict;
    NotRunReason reason;
    char         note[192];
};

using ArmFn = ArmOutcome (*)(const O3Config&);

// A verdict alone is too coarse to tell whether a knob is being tested: two different
// refusals are both NOT-RUN.  Arms are compared on the pair.
struct Decision {
    Verdict      verdict;
    NotRunReason reason;
    bool operator!=(const Decision& o) const {
        return verdict != o.verdict || reason != o.reason;
    }
};

struct Arm {
    const char*  name;
    const char*  oracle;   // "O3" | "O2" | "TAP"
    Verdict      expect;
    NotRunReason expectReason;
    // A substring the arm's own report must contain, or null. This is what separates "it went red"
    // from "it went red in the right place": without it, an O2 mutation that reddened the WRONG
    // domain, or a tap that refused with the wrong status, still satisfies a verdict-only arm.
    const char*  expectNote;
    ArmFn        fn;
    const char*  what;
};

ArmOutcome FromO3(const O3Result& r) {
    ArmOutcome o;
    o.verdict = r.verdict;
    o.reason = r.reason;
    snprintf(o.note, sizeof(o.note), "%s", r.detail);
    return o;
}

ArmOutcome FromO2(const O2Result& r) {
    ArmOutcome o;
    o.verdict = r.verdict;
    o.reason = r.reason;
    snprintf(o.note, sizeof(o.note), "%s", r.detail);
    return o;
}

// ---------------------------------------------------------------- O3 arms

ArmOutcome ArmO3Golden(const O3Config& c) {
    static w2i::Corpus live, replay;
    w2i::BuildGoldenPair(live, replay);
    return FromO3(O3Compare(live.stream, replay.stream, c));
}

ArmOutcome ArmO3SameTag(const O3Config& c) {
    static w2i::Corpus live, replay;
    w2i::BuildGoldenPair(live, replay);
    replay.stream.tag = live.stream.tag;  // one producer wearing two names
    return FromO3(O3Compare(live.stream, replay.stream, c));
}

ArmOutcome ArmO3Untagged(const O3Config& c) {
    static w2i::Corpus live, replay;
    w2i::BuildGoldenPair(live, replay);
    replay.stream.tag.v = 0;
    return FromO3(O3Compare(live.stream, replay.stream, c));
}

ArmOutcome ArmO3AliasedStream(const O3Config& c) {
    static w2i::Corpus live, replay;
    w2i::BuildGoldenPair(live, replay);
    replay.stream.instants = live.stream.instants;  // the "100%, every cell" shape
    return FromO3(O3Compare(live.stream, replay.stream, c));
}

ArmOutcome ArmO3AliasedBones(const O3Config& c) {
    static w2i::Corpus live, replay;
    w2i::BuildGoldenPair(live, replay);
    replay.frames[3][1].boneO = live.frames[3][1].boneO;
    return FromO3(O3Compare(live.stream, replay.stream, c));
}

ArmOutcome ArmO3SkewClock(const O3Config& c) {
    static w2i::Corpus live, replay;
    w2i::BuildGoldenPair(live, replay);
    replay.instants[5].simTimeAfterUs += 1;  // the sim clock ticked between the two builders
    return FromO3(O3Compare(live.stream, replay.stream, c));
}

// The second domain, and it is the reason there are two.  A frame boundary that the clock did not
// register still means the two builders straddled a frame; one reading cannot see this.
ArmOutcome ArmO3SkewFrame(const O3Config& c) {
    static w2i::Corpus live, replay;
    w2i::BuildGoldenPair(live, replay);
    replay.instants[5].frameIndexAfter += 1;
    return FromO3(O3Compare(live.stream, replay.stream, c));
}

// The producer integrated scaledDeltaUs -- beat 1 of three -- so its source time runs slow against
// its own sim clock.  W1-D measured that shape; this is O3 refusing to publish numbers built on it.
ArmOutcome ArmO3ClockDrift(const O3Config& c) {
    static w2i::Corpus live, replay;
    w2i::BuildGoldenPair(live, replay);
    for (uint32_t t = 0; t < w2i::Corpus::kInstants; ++t) {
        const uint64_t base = replay.instants[0].sourceTimeUs;
        const uint64_t span = replay.instants[t].sourceTimeUs - base;
        replay.instants[t].sourceTimeUs = base + span / 3;
    }
    return FromO3(O3Compare(live.stream, replay.stream, c));
}

ArmOutcome ArmO3Empty(const O3Config& c) {
    static w2i::Corpus live, replay;
    w2i::BuildGoldenPair(live, replay);
    replay.stream.count = 0;
    return FromO3(O3Compare(live.stream, replay.stream, c));
}

ArmOutcome ArmO3BoneCountDiffers(const O3Config& c) {
    static w2i::Corpus live, replay;
    w2i::BuildGoldenPair(live, replay);
    replay.frames[2][0].boneCount -= 1;
    return FromO3(O3Compare(live.stream, replay.stream, c));
}

ArmOutcome ArmO3NonMonotonic(const O3Config& c) {
    static w2i::Corpus live, replay;
    w2i::BuildGoldenPair(live, replay);
    replay.instants[9].sourceTimeUs = replay.instants[8].sourceTimeUs;
    return FromO3(O3Compare(live.stream, replay.stream, c));
}

// S12.3's mutation: feed poses one frame late.  Matching by index would pass this; matching by
// source time makes every matched pair compare frame N against frame N+1.
ArmOutcome ArmO3OneFrameLate(const O3Config& c) {
    static w2i::Corpus live, replay;
    w2i::BuildGoldenPair(live, replay);
    for (uint32_t t = 0; t + 1 < w2i::Corpus::kInstants; ++t) {
        for (uint32_t p = 0; p < w2i::Corpus::kPlayers; ++p) {
            memcpy(replay.ori[t][p], live.ori[t + 1][p], sizeof(replay.ori[t][p]));
            memcpy(replay.pos[t][p], live.pos[t + 1][p], sizeof(replay.pos[t][p]));
        }
    }
    return FromO3(O3Compare(live.stream, replay.stream, c));
}

// One hand bone, one degree.  S12.5.6 M1 in O3's domain.
ArmOutcome ArmO3Hand1Deg(const O3Config& c) {
    static w2i::Corpus live, replay;
    w2i::BuildGoldenPair(live, replay);
    static const float kAxis[3] = {0.0f, 1.0f, 0.0f};
    for (uint32_t t = 0; t < w2i::Corpus::kInstants; ++t) {
        float* q = replay.ori[t][0] + 4 * 11;
        float dq[4];
        const double half = 1.0 * 3.14159265358979323846 / 360.0;
        const double s = sin(half);
        dq[0] = float(kAxis[0] * s);
        dq[1] = float(kAxis[1] * s);
        dq[2] = float(kAxis[2] * s);
        dq[3] = float(cos(half));
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        q[0] = dq[3] * x + dq[0] * w + dq[1] * z - dq[2] * y;
        q[1] = dq[3] * y - dq[0] * z + dq[1] * w + dq[2] * x;
        q[2] = dq[3] * z + dq[0] * y - dq[1] * x + dq[2] * w;
        q[3] = dq[3] * w - dq[0] * x - dq[1] * y - dq[2] * z;
    }
    return FromO3(O3Compare(live.stream, replay.stream, c));
}

// The disc displaced 2 cm, in the position domain.
ArmOutcome ArmO3Pos2cm(const O3Config& c) {
    static w2i::Corpus live, replay;
    w2i::BuildGoldenPair(live, replay);
    for (uint32_t t = 0; t < w2i::Corpus::kInstants; ++t) {
        replay.pos[t][2][3 * 4 + 0] += 0.02f;
    }
    return FromO3(O3Compare(live.stream, replay.stream, c));
}

// Exactly at SC2's 95%: 2 of 40 instants dirty.  A gate written with >= where it meant > , or the
// other way round, moves here and nowhere else.
ArmOutcome ArmO3AtRatio(const O3Config& c) {
    static w2i::Corpus live, replay;
    w2i::BuildGoldenPair(live, replay);
    for (uint32_t t = 0; t < 2; ++t) {
        replay.pos[t][0][0] += 0.02f;
    }
    return FromO3(O3Compare(live.stream, replay.stream, c));
}

// One instant past it: 3 of 40 = 92.5%.
ArmOutcome ArmO3BelowRatio(const O3Config& c) {
    static w2i::Corpus live, replay;
    w2i::BuildGoldenPair(live, replay);
    for (uint32_t t = 0; t < 3; ++t) {
        replay.pos[t][0][0] += 0.02f;
    }
    return FromO3(O3Compare(live.stream, replay.stream, c));
}

// Instants present on one side only are reported and skipped, not silently matched to a neighbour.
ArmOutcome ArmO3Unmatched(const O3Config& c) {
    static w2i::Corpus live, replay;
    w2i::BuildGoldenPair(live, replay);
    for (uint32_t t = 0; t < w2i::Corpus::kInstants; ++t) {
        replay.instants[t].sourceTimeUs += 3;
    }
    O3Result r = O3Compare(live.stream, replay.stream, c);
    ArmOutcome o = FromO3(r);
    snprintf(o.note, sizeof(o.note), "liveOnly=%u replayOnly=%u matched=%u", r.liveOnlyInstants,
             r.replayOnlyInstants, r.matchedInstants);
    return o;
}

// ---------------------------------------------------------------- O2 arms

const char* const kBodyNames[] = {"com.x", "com.y", "com.z", "ori.x", "ori.y", "ori.z", "ori.w"};
constexpr uint32_t kBodyFields = 7;

struct O2Fixture {
    float file[4][kBodyFields];
    float eng[4][kBodyFields];
    O2Sample samples[8];
    O2Streams streams;

    void Build() {
        for (uint32_t d = 0; d < 4; ++d) {
            for (uint32_t i = 0; i < kBodyFields; ++i) {
                file[d][i] = 1.0f + float(d) * 0.25f + float(i);
                eng[d][i] = file[d][i];
            }
        }
        for (uint32_t k = 0; k < 8; ++k) {
            samples[k].sourceTimeUs = 1000 + k * 100;
            samples[k].entity = 4;
            for (uint32_t d = 0; d < 4; ++d) {
                samples[k].domains[d].expected = StateView{kBodyNames, file[d], kBodyFields};
                samples[k].domains[d].observed = StateView{kBodyNames, eng[d], kBodyFields};
                samples[k].domains[d].valid = true;
            }
        }
        streams.fileTag = MintSourceTag("demo-decoder", 11);
        streams.engineTag = MintSourceTag("ientity-readback", 12);
        streams.samples = samples;
        streams.count = 8;
    }
};

ArmOutcome ArmO2Golden(const O3Config&) {
    static O2Fixture f;
    f.Build();
    return FromO2(O2Compare(f.streams));
}

ArmOutcome ArmO2SameTag(const O3Config&) {
    static O2Fixture f;
    f.Build();
    f.streams.engineTag = f.streams.fileTag;
    return FromO2(O2Compare(f.streams));
}

ArmOutcome ArmO2Aliased(const O3Config&) {
    static O2Fixture f;
    f.Build();
    for (uint32_t k = 0; k < 8; ++k) {
        f.samples[k].domains[uint8_t(O2Domain::RenderSlot)].observed.values =
            f.samples[k].domains[uint8_t(O2Domain::RenderSlot)].expected.values;
    }
    return FromO2(O2Compare(f.streams));
}

// Only the domain we wrote ourselves was readable.  S12.2 says this comparison is close to
// tautological, so agreeing with ourselves eight times is NOT a pass.
ArmOutcome ArmO2PublishedOnly(const O3Config&) {
    static O2Fixture f;
    f.Build();
    for (uint32_t k = 0; k < 8; ++k) {
        for (uint32_t d = 1; d < 4; ++d) {
            f.samples[k].domains[d].valid = false;
        }
    }
    return FromO2(O2Compare(f.streams));
}

// S12.2's mutation: write ci+168 but not ci+320.  Red on the physics-aligned comparison ONLY, which
// is also the proof that the two slots are scored independently rather than summed.
ArmOutcome ArmO2PhysSlotOnly(const O3Config&) {
    static O2Fixture f;
    f.Build();
    f.eng[uint8_t(O2Domain::PhysSlot)][2] += 0.5f;
    O2Result r = O2Compare(f.streams);
    ArmOutcome o = FromO2(r);
    snprintf(o.note, sizeof(o.note), "render=%llu/%llu phys=%llu/%llu body=%llu/%llu",
             (unsigned long long)r.byDomain[uint8_t(O2Domain::RenderSlot)].mismatched,
             (unsigned long long)r.byDomain[uint8_t(O2Domain::RenderSlot)].compared,
             (unsigned long long)r.byDomain[uint8_t(O2Domain::PhysSlot)].mismatched,
             (unsigned long long)r.byDomain[uint8_t(O2Domain::PhysSlot)].compared,
             (unsigned long long)r.byDomain[uint8_t(O2Domain::PhysBody)].mismatched,
             (unsigned long long)r.byDomain[uint8_t(O2Domain::PhysBody)].compared);
    return o;
}

// The owner write was skipped, so physics kept authoring the body: the engine's own state diverges
// from the file while both pose slots still echo what we published.
ArmOutcome ArmO2OwnerWriteSkipped(const O3Config&) {
    static O2Fixture f;
    f.Build();
    for (uint32_t i = 0; i < kBodyFields; ++i) {
        f.eng[uint8_t(O2Domain::PhysBody)][i] += 0.01f;
    }
    return FromO2(O2Compare(f.streams));
}

// Bit-exact and not epsilon-equal.  -0.0 compares equal to +0.0 under ==, and a lossless claim that
// cannot tell them apart is not a lossless claim.
ArmOutcome ArmO2NegativeZero(const O3Config&) {
    static O2Fixture f;
    f.Build();
    for (uint32_t d = 0; d < 4; ++d) {
        f.file[d][0] = 0.0f;
        f.eng[d][0] = -0.0f;
    }
    return FromO2(O2Compare(f.streams));
}

ArmOutcome ArmO2NoSamples(const O3Config&) {
    static O2Fixture f;
    f.Build();
    f.streams.count = 0;
    return FromO2(O2Compare(f.streams));
}

// ---------------------------------------------------------------- E-7 tap arms

struct TapFixture {
    uint8_t  sessionBuf[64];
    uint8_t  bonesBuf[64];
    uint64_t sim;
    uint32_t frame;
    uint64_t simStep;
    uint32_t frameStep;
    bool     clockOk;
    uint32_t bonesLen;
};
TapFixture gTap;

bool TapClock(uint64_t* sim, uint32_t* frame) {
    if (!gTap.clockOk) {
        return false;
    }
    *sim = gTap.sim;
    *frame = gTap.frame;
    gTap.sim += gTap.simStep;
    gTap.frame += gTap.frameStep;
    return true;
}

uint32_t TapSessionBuilder(void*, uint8_t* dst, uint32_t cap) {
    const uint32_t n = cap < 8 ? cap : 8;
    memset(dst, 'S', n);
    return n;
}

uint32_t TapBonesBuilder(void*, uint8_t* dst, uint32_t cap) {
    const uint32_t n = gTap.bonesLen < cap ? gTap.bonesLen : cap;
    memset(dst, 'B', n);
    return n;
}

void TapReset() {
    gTap.sim = 5000;
    gTap.frame = 900;
    gTap.simStep = 0;
    gTap.frameStep = 0;
    gTap.clockOk = true;
    gTap.bonesLen = 16;
}

ArmOutcome RunTap(bool bind) {
    IOracleTap* tap = oracletap::Create(gTap.sessionBuf, sizeof(gTap.sessionBuf), gTap.bonesBuf,
                                        sizeof(gTap.bonesBuf));
    oracletap::BindClock(tap, &TapClock);
    if (bind) {
        oracletap::BindBuilders(tap, &TapSessionBuilder, nullptr, &TapBonesBuilder, nullptr);
    }
    OracleSample s;
    const bool ok = tap->Sample(s);
    ArmOutcome o;
    o.verdict = ok ? Verdict::Pass : Verdict::Fail;
    o.reason = NotRunReason::None;
    snprintf(o.note, sizeof(o.note), "status=%s sim %llu->%llu frame %u->%u sessionLen=%u bonesLen=%u",
             TapStatusName(tap->Status()), (unsigned long long)s.bracket.simTimeBeforeUs,
             (unsigned long long)s.bracket.simTimeAfterUs, s.bracket.frameIndexBefore,
             s.bracket.frameIndexAfter, s.sessionLen, s.bonesLen);
    oracletap::Destroy(tap);
    return o;
}

ArmOutcome ArmTapNotBound(const O3Config&) {
    TapReset();
    return RunTap(false);
}
ArmOutcome ArmTapOk(const O3Config&) {
    TapReset();
    return RunTap(true);
}
ArmOutcome ArmTapSkewClock(const O3Config&) {
    TapReset();
    gTap.simStep = 1;
    return RunTap(true);
}
ArmOutcome ArmTapSkewFrame(const O3Config&) {
    TapReset();
    gTap.frameStep = 1;
    return RunTap(true);
}
ArmOutcome ArmTapNoClock(const O3Config&) {
    TapReset();
    gTap.clockOk = false;
    return RunTap(true);
}
ArmOutcome ArmTapEmptyBones(const O3Config&) {
    TapReset();
    gTap.bonesLen = 0;
    return RunTap(true);
}

// ---------------------------------------------------------------- the arm table

const Arm kArms[] = {
    {"o3-golden", "O3", Verdict::Pass, NotRunReason::None, "40/40 instants clean", ArmO3Golden,
     "two producers, sub-tolerance difference on every bone: the negative control"},
    {"o3-same-tag", "O3", Verdict::NotRun, NotRunReason::SameSource, nullptr, ArmO3SameTag,
     "one producer wearing two names"},
    {"o3-untagged", "O3", Verdict::NotRun, NotRunReason::SameSource, nullptr, ArmO3Untagged,
     "a stream with no SourceTag at all"},
    {"o3-aliased-stream", "O3", Verdict::NotRun, NotRunReason::AliasedBuffers, nullptr, ArmO3AliasedStream,
     "both sides handed one instant array"},
    {"o3-aliased-bones", "O3", Verdict::NotRun, NotRunReason::AliasedBuffers, "player 101 shares a bone array", ArmO3AliasedBones,
     "one player's bone array shared between the passes"},
    {"o3-skew-clock", "O3", Verdict::NotRun, NotRunReason::Skew, "1 of 40 source-time matches", ArmO3SkewClock,
     "the sim clock moved between the two builders"},
    {"o3-skew-frame", "O3", Verdict::NotRun, NotRunReason::Skew, "1 of 40 source-time matches", ArmO3SkewFrame,
     "the frame counter moved while the clock did not: the second domain"},
    {"o3-clock-drift", "O3", Verdict::NotRun, NotRunReason::ClockDrift, "66.7%, over the 10.0% bound", ArmO3ClockDrift,
     "source time integrated from beat-1 deltas runs a third slow"},
    {"o3-empty", "O3", Verdict::NotRun, NotRunReason::NoSamples, nullptr, ArmO3Empty,
     "a zero denominator is not a pass"},
    {"o3-bonecount", "O3", Verdict::NotRun, NotRunReason::ShapeMismatch, "with 23/22 bones", ArmO3BoneCountDiffers,
     "the two passes disagree on how many bones a player has"},
    {"o3-nonmonotonic", "O3", Verdict::NotRun, NotRunReason::ShapeMismatch, "not strictly increasing at index 9", ArmO3NonMonotonic,
     "two instants at one source time cannot be matched unambiguously"},
    {"o3-one-frame-late", "O3", Verdict::Fail, NotRunReason::None, nullptr, ArmO3OneFrameLate,
     "S12.3 mutation: poses fed one frame late"},
    {"o3-hand-1deg", "O3", Verdict::Fail, NotRunReason::None, nullptr, ArmO3Hand1Deg,
     "one bone's orientation off by 1 degree"},
    {"o3-pos-2cm", "O3", Verdict::Fail, NotRunReason::None, nullptr, ArmO3Pos2cm,
     "one bone displaced 2 cm"},
    {"o3-at-ratio", "O3", Verdict::Pass, NotRunReason::None, "38/40 instants clean", ArmO3AtRatio,
     "exactly 95% of instants clean: the SC2 boundary from the passing side"},
    {"o3-below-ratio", "O3", Verdict::Fail, NotRunReason::None, "37/40 instants clean", ArmO3BelowRatio,
     "92.5% of instants clean: the SC2 boundary from the failing side"},
    {"o3-unmatched", "O3", Verdict::NotRun, NotRunReason::NoSamples, "liveOnly=40 replayOnly=40 matched=0", ArmO3Unmatched,
     "no source time appears on both sides; nothing is matched to a neighbour"},

    {"o2-golden", "O2", Verdict::Pass, NotRunReason::None, nullptr, ArmO2Golden,
     "file and engine agree in all four domains"},
    {"o2-same-tag", "O2", Verdict::NotRun, NotRunReason::SameSource, nullptr, ArmO2SameTag,
     "the decoder and the read-back are one producer"},
    {"o2-aliased", "O2", Verdict::NotRun, NotRunReason::AliasedBuffers, "render-slot: sample 0 hands one buffer", ArmO2Aliased,
     "one buffer handed to both sides of a domain"},
    {"o2-published-only", "O2", Verdict::NotRun, NotRunReason::Unavailable, "no engine-authored field was readable", ArmO2PublishedOnly,
     "only the tautological domain was readable"},
    {"o2-phys-slot-only", "O2", Verdict::Fail, NotRunReason::None, "render=0/56 phys=8/56 body=0/56", ArmO2PhysSlotOnly,
     "S12.2 mutation: +168 written, +320 not; exactly one slot goes red"},
    {"o2-owner-write-skipped", "O2", Verdict::Fail, NotRunReason::None, "asserted 56/168", ArmO2OwnerWriteSkipped,
     "physics kept authoring the body"},
    {"o2-negative-zero", "O2", Verdict::Fail, NotRunReason::None, "asserted 24/168", ArmO2NegativeZero,
     "-0.0 against +0.0: equal under ==, not bit-exact"},
    {"o2-no-samples", "O2", Verdict::NotRun, NotRunReason::NoSamples, nullptr, ArmO2NoSamples,
     "a zero denominator is not a pass"},

    {"tap-not-bound", "TAP", Verdict::Fail, NotRunReason::None, "status=not-bound", ArmTapNotBound,
     "no builders bound: says so rather than reporting an empty snapshot"},
    {"tap-ok", "TAP", Verdict::Pass, NotRunReason::None, "status=ok sim 5000->5000 frame 900->900 sessionLen=8 bonesLen=16", ArmTapOk,
     "both builders inside one instant; the bracket holds in both domains"},
    {"tap-skew-clock", "TAP", Verdict::Fail, NotRunReason::None, "status=skewed sim 5000->5001 frame 900->900", ArmTapSkewClock,
     "positive control: the sim clock CAN move between the builders"},
    {"tap-skew-frame", "TAP", Verdict::Fail, NotRunReason::None, "status=skewed sim 5000->5000 frame 900->901", ArmTapSkewFrame,
     "positive control in the second domain: the frame counter CAN move"},
    {"tap-no-clock", "TAP", Verdict::Fail, NotRunReason::None, "status=no-clock", ArmTapNoClock,
     "no clock, so nothing is invoked and nothing is claimed"},
    {"tap-empty-bones", "TAP", Verdict::Fail, NotRunReason::None, "status=bones-builder-failed", ArmTapEmptyBones,
     "a builder that wrote nothing is a failure, not an empty skeleton"},
};
constexpr size_t kArmCount = sizeof(kArms) / sizeof(kArms[0]);

// ---------------------------------------------------------------- oracle mutations
//
// Each perturbs ONE knob of the gate.  A knob no arm reacts to is a knob nothing is testing.

struct OracleMutation {
    const char* name;
    void (*apply)(O3Config&);
    const char* what;
};

void MutTolOriHuge(O3Config& c) { c.oriToleranceDeg = 1.0e9f; }
void MutTolOriZero(O3Config& c) { c.oriToleranceDeg = 0.0f; }
void MutTolPosHuge(O3Config& c) { c.posToleranceM = 1.0e9f; }
void MutTolPosZero(O3Config& c) { c.posToleranceM = 0.0f; }
void MutRatioZero(O3Config& c) { c.minPassRatio = 0.0f; }
void MutRatioOne(O3Config& c) { c.minPassRatio = 1.0f; }
void MutDriftOff(O3Config& c) { c.maxClockDriftFrac = 0.0f; }
void MutDriftTight(O3Config& c) { c.maxClockDriftFrac = 1.0e-9f; }

const OracleMutation kMutations[] = {
    {"tol-ori-huge", MutTolOriHuge, "orientation tolerance to 1e9 deg"},
    {"tol-ori-zero", MutTolOriZero, "orientation tolerance to 0 deg"},
    {"tol-pos-huge", MutTolPosHuge, "position tolerance to 1e9 m"},
    {"tol-pos-zero", MutTolPosZero, "position tolerance to 0 m"},
    {"ratio-zero", MutRatioZero, "pass ratio to 0"},
    {"ratio-one", MutRatioOne, "pass ratio to 1.0"},
    {"drift-off", MutDriftOff, "clock-drift check disabled"},
    {"drift-tight", MutDriftTight, "clock-drift bound to 1e-9"},
};
constexpr size_t kMutationCount = sizeof(kMutations) / sizeof(kMutations[0]);

// ---------------------------------------------------------------- driver

bool StrEq(const char* a, const char* b) {
    return strcmp(a, b) == 0;
}

int RunArms(const O3Config& cfg, const char* dropArm, bool verbose, uint32_t* checked,
            uint32_t* failures, Decision* decisionOut) {
    uint32_t n = 0, bad = 0;
    for (size_t i = 0; i < kArmCount; ++i) {
        if (dropArm != nullptr && StrEq(kArms[i].name, dropArm)) {
            continue;
        }
        const ArmOutcome o = kArms[i].fn(cfg);
        if (decisionOut != nullptr) {
            decisionOut[i] = Decision{o.verdict, o.reason};
        }
        ++n;
        const bool noteOk =
            kArms[i].expectNote == nullptr || strstr(o.note, kArms[i].expectNote) != nullptr;
        const bool ok =
            (o.verdict == kArms[i].expect) && (o.reason == kArms[i].expectReason) && noteOk;
        if (!ok) {
            ++bad;
            printf("O3-FAIL arm=%s oracle=%s expected=%s/%s%s%s got=%s/%s detail=%s\n", kArms[i].name,
                   kArms[i].oracle, VerdictName(kArms[i].expect),
                   NotRunReasonName(kArms[i].expectReason),
                   kArms[i].expectNote != nullptr ? " containing " : "",
                   kArms[i].expectNote != nullptr ? kArms[i].expectNote : "", VerdictName(o.verdict),
                   NotRunReasonName(o.reason), o.note);
        } else if (verbose) {
            printf("O3-ARM %s %s %s/%s %s\n", kArms[i].oracle, kArms[i].name,
                   VerdictName(o.verdict), NotRunReasonName(o.reason), o.note);
        }
    }
    *checked = n;
    *failures = bad;
    return int(bad);
}

// ★ THE FLOOR IS NOT ONE NUMBER, so it is not measured at one place.
//
// The smallest rotation the wire can represent depends on where on the unit sphere the reference
// sits and which way the perturbation goes -- the smallest-three encoding drops a different
// component in each octant, and its resolution is worst where two components are nearly equal and
// the selector is close to changing.  Reporting a single step measured at a single convenient
// orientation would be the S12.4 defect wearing a measurement's clothes: one setting's number
// published as a property of the system.  So the ladder runs at four references chosen to span that
// variation, and the RANGE is what is published.
struct FloorSite {
    const char* name;
    float       ref[4];
    float       axis[3];
};

const FloorSite kFloorSites[] = {
    {"near-identity", {0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
    {"generic", {0.1f, 0.2f, 0.05f, 0.97f}, {0.0f, 1.0f, 0.0f}},
    // Two components nearly equal: the selector is close to switching, which is where the encoding
    // is coarsest and where a floor measured only at a convenient orientation is most optimistic.
    {"selector-edge", {0.7071f, 0.7069f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
    // A perturbation axis that is not a basis vector, so the change is spread across components.
    {"oblique-axis", {0.3f, 0.4f, 0.5f, 0.7f}, {0.577f, 0.577f, 0.577f}},
};
constexpr size_t kFloorSiteCount = sizeof(kFloorSites) / sizeof(kFloorSites[0]);

int MeasureFloors(const O3Config& cfg, uint32_t* checked, uint32_t* failures) {
    uint32_t n = 0, bad = 0;
    float stepMin = 0.0f, stepMax = 0.0f;
    uint32_t sites = 0, sitesMissingSomething = 0, sitesSeeingSomething = 0;

    for (size_t s = 0; s < kFloorSiteCount; ++s) {
        const OriFloorResult f =
            MeasureOrientationFloor(kFloorSites[s].ref, kFloorSites[s].axis, "lefthandori", cfg);
        if (!f.valid) {
            printf("O3-FAIL floor=orientation site=%s refusal=%s\n", kFloorSites[s].name, f.refusal);
            ++bad;
            continue;
        }
        ++sites;
        if (sites == 1 || f.measuredStepDeg < stepMin) {
            stepMin = f.measuredStepDeg;
        }
        if (sites == 1 || f.measuredStepDeg > stepMax) {
            stepMax = f.measuredStepDeg;
        }
        printf("FLOOR-SITE name=%s measured_step_deg=%.6f baseline_deg=%.9f gate_tol_deg=%.4f\n",
               kFloorSites[s].name, double(f.measuredStepDeg), double(f.baselineDeg),
               double(f.gateToleranceDeg));
        for (uint32_t i = 0; i < f.rungCount; ++i) {
            printf("FLOOR-RUNG site=%s commanded_deg=%.4f observed_deg=%.6f gate_red=%d\n",
                   kFloorSites[s].name, double(f.rungs[i].commandedDeg),
                   double(f.rungs[i].observedDeg), f.rungs[i].gateRed ? 1 : 0);
            ++n;
        }
        printf("FLOOR-RESULT site=%s first_red_deg=%.4f missed_rungs=%u of %u\n",
               kFloorSites[s].name, double(f.firstRedDeg), f.missedRungs, f.rungCount);
        if (f.missedRungs > 0) {
            ++sitesMissingSomething;
        }
        if (f.firstRedDeg != 0.0f) {
            ++sitesSeeingSomething;
        }
    }

    if (sites == 0) {
        printf("O3-FAIL floor=orientation no site produced a measurement\n");
        ++bad;
    } else {
        printf("O3-COVER ori_step_range_deg=%.6f..%.6f sites=%u\n", double(stepMin), double(stepMax),
               sites);
        // S12.3: "a run where nothing is missed means the gate is mis-scaled and is itself a
        // finding". Both directions are enforced, because a ladder that sees everything has not
        // bracketed the floor and a ladder that sees nothing cannot show the gate works at all.
        if (sitesMissingSomething == 0) {
            printf("O3-FAIL floor=orientation every rung was seen at every site; the ladder does not "
                   "bracket the floor, so no blind spot has been measured\n");
            ++bad;
        }
        if (sitesSeeingSomething == 0) {
            printf("O3-FAIL floor=orientation no rung went red at any site; the ladder cannot show "
                   "the gate sees anything\n");
            ++bad;
        }
    }

    // The position side, and it varies for the same reason: how far a displacement has to travel to
    // change the encoded index depends on where in its cell the reference already sits. One
    // reference exactly on a grid point measures half a step and would report it as the step.
    static const float kPosRefs[][3] = {
        {0.0f, 0.0f, 0.0f},      // on a grid point by construction
        {1.25f, 0.0f, 0.0f},     // a round number, which is also on the grid at a 1 mm step
        {1.2503f, 0.0f, 0.0f},   // deliberately inside a cell
        {-7.31719f, 0.0f, 0.0f}, // arbitrary, negative, nowhere near a boundary
    };
    float pMin = 0.0f, pMax = 0.0f;
    uint32_t pSites = 0, pBelow = 0;
    for (size_t s = 0; s < sizeof(kPosRefs) / sizeof(kPosRefs[0]); ++s) {
        const PosFloorResult p = MeasurePositionFloor(kPosRefs[s], "com", cfg);
        if (!p.valid) {
            printf("O3-FAIL floor=position ref=%.5f refusal=%s\n", double(kPosRefs[s][0]), p.refusal);
            ++bad;
            continue;
        }
        ++pSites;
        ++n;
        if (pSites == 1 || p.measuredStepM < pMin) {
            pMin = p.measuredStepM;
        }
        if (pSites == 1 || p.measuredStepM > pMax) {
            pMax = p.measuredStepM;
        }
        if (p.gateBelowWireStep) {
            ++pBelow;
        }
        printf("FLOOR-POS ref_m=%.5f measured_step_m=%.6f gate_tol_m=%.6f gate_below_wire_step=%d\n",
               double(kPosRefs[s][0]), double(p.measuredStepM), double(p.gateToleranceM),
               p.gateBelowWireStep ? 1 : 0);
    }
    if (pSites == 0) {
        printf("O3-FAIL floor=position no reference produced a measurement\n");
        ++bad;
    } else {
        printf("O3-COVER pos_step_range_m=%.6f..%.6f sites=%u tol_below_step_at=%u\n", double(pMin),
               double(pMax), pSites, pBelow);
    }

    *checked = n;
    *failures = bad;
    return int(bad);
}

int Usage() {
    printf("usage: verify_harness [--verbose] [--drop-arm=NAME] [--list-arms] "
           "[--list-oracle-mutations] [--mutate-oracle=NAME]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    bool verbose = false;
    const char* dropArm = nullptr;
    const char* mutate = nullptr;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (StrEq(a, "--verbose")) {
            verbose = true;
        } else if (StrEq(a, "--list-arms")) {
            for (size_t k = 0; k < kArmCount; ++k) {
                printf("%s\t%s\t%s/%s\t%s\n", kArms[k].name, kArms[k].oracle,
                       VerdictName(kArms[k].expect), NotRunReasonName(kArms[k].expectReason),
                       kArms[k].what);
            }
            return 0;
        } else if (StrEq(a, "--list-oracle-mutations")) {
            for (size_t k = 0; k < kMutationCount; ++k) {
                printf("%s\t%s\n", kMutations[k].name, kMutations[k].what);
            }
            return 0;
        } else if (strncmp(a, "--drop-arm=", 11) == 0) {
            dropArm = a + 11;
        } else if (strncmp(a, "--mutate-oracle=", 16) == 0) {
            mutate = a + 16;
        } else {
            return Usage();
        }
    }

    const O3Config base = O3Defaults();

    if (mutate != nullptr) {
        // Run every arm twice -- pristine and mutated -- and report which arms saw the knob move.
        // The verdicts are compared, not the numbers: an arm that changed its VERDICT is an arm
        // that is actually testing this parameter.
        static Decision pristine[kArmCount];
        static Decision mutated[kArmCount];
        uint32_t n = 0, bad = 0;
        RunArms(base, nullptr, false, &n, &bad, pristine);

        O3Config c = base;
        bool known = false;
        for (size_t k = 0; k < kMutationCount; ++k) {
            if (StrEq(kMutations[k].name, mutate)) {
                kMutations[k].apply(c);
                known = true;
                break;
            }
        }
        if (!known) {
            printf("MUTATION-RESULT name=%s status=UNKNOWN moved=0\n", mutate);
            return 2;
        }
        // Failures under a mutation are expected, so they are not reported as arm failures.
        uint32_t mn = 0;
        for (size_t i = 0; i < kArmCount; ++i) {
            const ArmOutcome o = kArms[i].fn(c);
            mutated[i] = Decision{o.verdict, o.reason};
            ++mn;
        }
        uint32_t moved = 0;
        for (size_t i = 0; i < kArmCount; ++i) {
            if (pristine[i] != mutated[i]) {
                ++moved;
                printf("MUTATION-ARM %s %s/%s -> %s/%s\n", kArms[i].name,
                       VerdictName(pristine[i].verdict), NotRunReasonName(pristine[i].reason),
                       VerdictName(mutated[i].verdict), NotRunReasonName(mutated[i].reason));
            }
        }
        printf("MUTATION-RESULT name=%s status=%s moved=%u arms=%u\n", mutate,
               moved > 0 ? "CAUGHT" : "UNREACHED", moved, mn);
        return moved > 0 ? 0 : 1;
    }

    uint32_t armChecked = 0, armFail = 0, floorChecked = 0, floorFail = 0;
    RunArms(base, dropArm, verbose, &armChecked, &armFail, nullptr);
    MeasureFloors(base, &floorChecked, &floorFail);

    const uint32_t checked = armChecked + floorChecked;
    const uint32_t failures = armFail + floorFail;
    printf("O3-COVER arms=%u floor_units=%u oracle_mutations=%u\n", armChecked, floorChecked,
           unsigned(kMutationCount));
    printf("O3-RESULT status=%s checked=%u failures=%u\n", failures == 0 ? "PASS" : "FAIL", checked,
           failures);
    return failures == 0 ? 0 : 1;
}
