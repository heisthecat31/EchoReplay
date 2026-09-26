// W2-I. The parts of the oracles that are ordinary functions with ordinary answers: the angular
// metric, the source tag, the domain scoring policy. The arm table in verify_harness.cpp covers the
// comparators end to end; this file covers the pieces the arms rest on, so a wrong metric shows up
// here as a wrong number rather than there as a mysteriously green gate.

#include <doctest.h>

#include "corpus.h"
#include "verify/o2.h"
#include "verify/o3.h"

#include <math.h>
#include <string.h>

using namespace replay::verify;

namespace {

void AxisAngle(double x, double y, double z, double deg, float q[4]) {
    const double n = sqrt(x * x + y * y + z * z);
    const double half = deg * 3.14159265358979323846 / 360.0;
    const double s = sin(half) / n;
    q[0] = float(x * s);
    q[1] = float(y * s);
    q[2] = float(z * s);
    q[3] = float(cos(half));
}

}  // namespace

TEST_CASE("QuatAngleDeg measures the rotation between two orientations") {
    float a[4], b[4];
    AxisAngle(0, 1, 0, 0.0, a);

    SUBCASE("identity is exactly zero") {
        CHECK(QuatAngleDeg(a, a) == 0.0f);
    }
    SUBCASE("a known angle comes back") {
        static const double kDeg[] = {0.05, 0.5, 5.0, 90.0, 179.0};
        for (double deg : kDeg) {
            AxisAngle(0, 1, 0, deg, b);
            CHECK(QuatAngleDeg(a, b) == doctest::Approx(deg).epsilon(0.002));
        }
    }
    SUBCASE("q and -q are one rotation") {
        AxisAngle(0.3, 0.4, 0.5, 40.0, b);
        float neg[4] = {-b[0], -b[1], -b[2], -b[3]};
        CHECK(QuatAngleDeg(b, neg) == doctest::Approx(0.0).epsilon(1e-4));
    }
    SUBCASE("a dot product slightly past one does not produce a NaN") {
        // Normalisation slop can push |dot| above 1; acos of that is NaN, and a NaN compares false
        // against every tolerance, which would make an outlier invisible rather than red.
        float p[4] = {0.0f, 0.0f, 0.0f, 1.0000001f};
        float q[4] = {0.0f, 0.0f, 0.0f, 1.0000001f};
        const float d = QuatAngleDeg(p, q);
        CHECK(d == d);
        CHECK(d == 0.0f);
    }
}

TEST_CASE("SourceTag refuses to be zero and separates producers") {
    const SourceTag a = MintSourceTag("live-capture", 1);
    const SourceTag b = MintSourceTag("replay-pass", 1);
    const SourceTag c = MintSourceTag("live-capture", 2);
    CHECK(SourceTagValid(a));
    CHECK(a.v != b.v);
    CHECK(a.v != c.v);
    SourceTag zero;
    zero.v = 0;
    CHECK_FALSE(SourceTagValid(zero));
}

TEST_CASE("the published domain is reported and never asserted") {
    // S12.2 calls the publish-path comparison close to tautological. If it were scored, an oracle
    // that only ever read back its own write would report a perfect percentage.
    CHECK_FALSE(O2DomainIsAsserted(O2Domain::Published));
    CHECK(O2DomainIsAsserted(O2Domain::RenderSlot));
    CHECK(O2DomainIsAsserted(O2Domain::PhysSlot));
    CHECK(O2DomainIsAsserted(O2Domain::PhysBody));
    CHECK(strcmp(O2DomainName(O2Domain::PhysSlot), "phys-slot") == 0);
}

TEST_CASE("the corpus carries a per-player bone count rather than one constant") {
    // If O3 ever hardcoded 23, this fixture is what would catch it: the three players do not agree
    // on how many bones they have, and the builder's own loop takes its trip count at run time.
    static w2i::Corpus live, replay;
    w2i::BuildGoldenPair(live, replay);
    CHECK(live.frames[0][0].boneCount != live.frames[0][1].boneCount);
    CHECK(live.frames[0][1].boneCount != live.frames[0][2].boneCount);

    const O3Result r = O3Compare(live.stream, replay.stream, O3Defaults());
    CHECK(r.verdict == Verdict::Pass);

    uint64_t expected = 0;
    for (uint32_t p = 0; p < w2i::Corpus::kPlayers; ++p) {
        expected += uint64_t(w2i::Corpus::kBones[p]) * w2i::Corpus::kInstants;
    }
    CHECK(r.comparedBones == expected);
    // The negative control is only a control if it measured something: the golden pair differs on
    // every bone, so a zero max outlier would mean the metric is not running.
    CHECK(r.maxOri.value > 0.0f);
    CHECK(r.maxPos.value > 0.0f);
    CHECK(r.bitIdenticalBones == 0);
}

TEST_CASE("O3 reports the outlier rather than averaging it away") {
    static w2i::Corpus live, replay;
    w2i::BuildGoldenPair(live, replay);
    replay.pos[7][1][3 * 5 + 2] += 0.031f;

    const O3Result r = O3Compare(live.stream, replay.stream, O3Defaults());
    CHECK(r.maxPos.seen);
    CHECK(r.maxPos.value == doctest::Approx(0.031).epsilon(0.01));
    CHECK(r.maxPos.bone == 5);
    CHECK(r.maxPos.player == live.frames[7][1].playerId);
    CHECK(r.maxPos.sourceTimeUs == live.instants[7].sourceTimeUs);
    CHECK(r.posViolations == 1);
    // 39 of 40 instants clean is still above SC2's 95%, so one outlier does not fail the gate --
    // and it is still named. Averaging is what would have hidden it.
    CHECK(r.verdict == Verdict::Pass);
    CHECK(r.passedInstants == w2i::Corpus::kInstants - 1);
}
