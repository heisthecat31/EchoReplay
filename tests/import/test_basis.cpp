#include "doctest.h"

#include <math.h>

#include <random>

#include "fixture.h"
#include "import/basis.h"
#include "import/hypotheses.h"

using namespace replay::import;
using import_test::MatchBuilder;

namespace {

// quat10 resolution plus fp error (DESIGN-FORMATS §5.4).
const float kCosHalfDegree = cosf(0.5f * 3.14159265f / 180.0f);

float Dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void CheckColumns(const float q[4], const float forward[3], const float up[3], const float left[3]) {
    float x[3], y[3], z[3];
    QuatAxis(q, 0, x);
    QuatAxis(q, 1, y);
    QuatAxis(q, 2, z);
    CHECK(Dot3(x, left) > kCosHalfDegree);
    CHECK(Dot3(y, up) > kCosHalfDegree);
    CHECK(Dot3(z, forward) > kCosHalfDegree);
}

struct AxisFixture {
    const char* name;
    float forward[3], up[3], left[3];
    float expected[4];
};

const AxisFixture kFixtures[] = {
    {"yaw +90", {1, 0, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0.70710678f, 0, 0.70710678f}},
    {"pitch down", {0, -1, 0}, {0, 0, 1}, {1, 0, 0}, {0.70710678f, 0, 0, 0.70710678f}},
    {"roll", {0, 0, 1}, {-1, 0, 0}, {0, 1, 0}, {0, 0, 0.70710678f, 0.70710678f}},
};

}  // namespace

TEST_CASE("BasisToQuat: the three one-axis fixtures decode to their quaternion and all three columns") {
    for (const AxisFixture& f : kFixtures) {
        CAPTURE(f.name);
        float q[4] = {9, 9, 9, 9};
        REQUIRE(BasisToQuat(f.forward, f.up, f.left, q) == BasisVerdict::Ok);
        for (int i = 0; i < 4; ++i) CHECK(q[i] == doctest::Approx(f.expected[i]).epsilon(1e-5));
        CheckColumns(q, f.forward, f.up, f.left);
    }
}

TEST_CASE("BasisToQuat: a negated left (the mirror mutant) is refused and leaves q untouched") {
    for (const AxisFixture& f : kFixtures) {
        CAPTURE(f.name);
        const float mirrored[3] = {-f.left[0], -f.left[1], -f.left[2]};
        float q[4] = {1, 2, 3, 4};
        CHECK(BasisToQuat(f.forward, f.up, mirrored, q) == BasisVerdict::Mirrored);
        CHECK(q[0] == 1);
        CHECK(q[3] == 4);
    }
}

TEST_CASE("BasisToQuat: degenerate bases are refused") {
    const float zero[3] = {0, 0, 0}, z[3] = {0, 0, 1}, y[3] = {0, 1, 0}, x[3] = {1, 0, 0};
    const float nan3[3] = {NAN, 0, 0};
    float q[4];
    CHECK(BasisToQuat(zero, y, x, q) == BasisVerdict::Degenerate);
    CHECK(BasisToQuat(z, z, x, q) == BasisVerdict::Degenerate);  // up parallel to forward
    CHECK(BasisToQuat(z, y, zero, q) == BasisVerdict::Degenerate);
    CHECK(BasisToQuat(nan3, y, x, q) == BasisVerdict::Degenerate);
}

TEST_CASE("BasisToQuat: 3-decimal API vectors of random rotations re-orthonormalise within half a degree") {
    std::mt19937 rng(20260925);
    std::normal_distribution<float> gauss;
    for (int n = 0; n < 2000; ++n) {
        float truth[4] = {gauss(rng), gauss(rng), gauss(rng), gauss(rng)};
        const float norm = sqrtf(truth[0] * truth[0] + truth[1] * truth[1] + truth[2] * truth[2] + truth[3] * truth[3]);
        for (float& c : truth) c /= norm;
        float left[3], up[3], forward[3];
        QuatAxis(truth, 0, left);
        QuatAxis(truth, 1, up);
        QuatAxis(truth, 2, forward);
        for (float* v : {left, up, forward}) {
            for (int i = 0; i < 3; ++i) v[i] = roundf(v[i] * 1000.0f) / 1000.0f;
        }
        float q[4];
        REQUIRE(BasisToQuat(forward, up, left, q) == BasisVerdict::Ok);
        CHECK(q[3] >= 0.0f);
        const float dot = fabsf(q[0] * truth[0] + q[1] * truth[1] + q[2] * truth[2] + q[3] * truth[3]);
        CHECK(2.0f * acosf(fminf(dot, 1.0f)) < 0.5f * 3.14159265f / 180.0f);
    }
}

TEST_CASE("AlignHemisphere keeps consecutive quaternions on the short arc") {
    const float previous[4] = {0, 0, 0, 1};
    float far[4] = {0, 0.1f, 0, -0.995f};
    AlignHemisphere(previous, far);
    CHECK(far[1] == doctest::Approx(-0.1f));
    CHECK(far[3] == doctest::Approx(0.995f));
    float near[4] = {0, 0.1f, 0, 0.995f};
    AlignHemisphere(previous, near);
    CHECK(near[3] == doctest::Approx(0.995f));
}

TEST_CASE("pose hypotheses H1-H4 read their fact and nothing else") {
    MatchBuilder b;
    b.Identity(7, "a", kTeamBlue);
    b.Frame(0);
    SrcPlayer& p = b.Player(0, 0, 2.0f, -5.0f);
    p.head.q[1] = 1.0f;  // distinguishable (not normalised; only identity matters here)
    p.head.q[3] = 0.0f;

    EmitFacts facts = kEmitFacts;
    CHECK(CheckPoseFacts(facts).ok);
    float com[3];
    ComFromHead(p.head.pos, facts, com);
    CHECK(com[0] == 2.0f);
    CHECK(com[2] == -5.0f);
    facts.comFromHead.value = {0.0f, -0.25f, 0.1f};
    ComFromHead(p.head.pos, facts, com);
    CHECK(com[1] == doctest::Approx(0.75f));
    CHECK(com[2] == doctest::Approx(-4.9f));

    // P1 measured: hmdOri is a yaw-only frame, nearest the body basis (H2); SimState ori is the head (H3).
    CHECK(HmdQuat(p, facts) == p.body.q);
    CHECK(BodyQuat(p, facts) == p.head.q);
    facts.hmdOri.value = OriSource::HeadBasis;
    CHECK(HmdQuat(p, facts) == p.head.q);
    facts.bodyOri.value = OriSource::SameAsHmd;
    CHECK(BodyQuat(p, facts) == p.head.q);
    facts.hmdOri.value = OriSource::BodyBasis;
    CHECK(BodyQuat(p, facts) == p.body.q);
    CHECK(HandQuat(p.lhand, facts) == p.lhand.q);

    EmitFacts mirrored = kEmitFacts;
    mirrored.worldAxesMatchEngine.value = false;
    CHECK(CheckPoseFacts(mirrored).code == ResultCode::BadArguments);
    EmitFacts rotatedHands = kEmitFacts;
    rotatedHands.handAxesDirect.value = false;
    CHECK(strstr(CheckPoseFacts(rotatedHands).message, "H4") != nullptr);
}

TEST_CASE("H5 disc owner: in hand vs possession, and a contested disc keeps its holder") {
    MatchBuilder b;
    b.Identity(1, "a", kTeamBlue);
    b.Identity(2, "b", kTeamOrange);
    b.Frame(0);
    b.Player(0, 0).possession = true;  // thrown: possession kept, disc not in hand
    b.Player(1, 1);
    const SrcFrame& f = b.match.frames[0];
    CHECK(DiscOwnerSlot(b.match, f, DiscOwnerMeaning::InHand, kNoSlot) == kNoSlot);
    CHECK(DiscOwnerSlot(b.match, f, DiscOwnerMeaning::Possession, kNoSlot) == 0);

    b.match.players[0].holdR = Hold::Disc;
    b.match.players[1].holdL = Hold::Disc;
    CHECK(DiscOwnerSlot(b.match, f, DiscOwnerMeaning::InHand, kNoSlot) == 0);
    CHECK(DiscOwnerSlot(b.match, f, DiscOwnerMeaning::InHand, 1) == 1);

    // Under Possession a holder claims without the flag: the API's holding_*=="disc" is possession.
    b.match.players[0].possession = false;
    b.match.players[0].holdR = Hold::None;
    CHECK(DiscOwnerSlot(b.match, f, DiscOwnerMeaning::Possession, kNoSlot) == 1);
}
