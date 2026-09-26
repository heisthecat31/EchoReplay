#include "corpus.h"

#include <math.h>
#include <string.h>

namespace w2i {

namespace {

constexpr double kPi = 3.14159265358979323846;

void Normalise(float q[4]) {
    const double n = sqrt(double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2] +
                          double(q[3]) * q[3]);
    for (int i = 0; i < 4; ++i) {
        q[i] = float(double(q[i]) / n);
    }
}

// Rotate `q` by `deg` about `axis`.  Shared with the perturbation helpers so a test asking for
// 1.0 degree gets 1.0 degree and not an approximation of one.
void RotateBy(float q[4], const float axis[3], double deg) {
    double n = sqrt(double(axis[0]) * axis[0] + double(axis[1]) * axis[1] + double(axis[2]) * axis[2]);
    if (n <= 0.0) {
        n = 1.0;
    }
    const double half = deg * kPi / 360.0;
    const double s = sin(half) / n;
    const double dx = axis[0] * s, dy = axis[1] * s, dz = axis[2] * s, dw = cos(half);
    const double qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    q[0] = float(dw * qx + dx * qw + dy * qz - dz * qy);
    q[1] = float(dw * qy - dx * qz + dy * qw + dz * qx);
    q[2] = float(dw * qz + dx * qy - dy * qx + dz * qw);
    q[3] = float(dw * qw - dx * qx - dy * qy - dz * qz);
    Normalise(q);
}

// ~44 Hz, in microseconds.  A property of the fixture, not of the engine: the corpus only needs a
// monotonic stamp whose spacing is the same on both sides.
constexpr uint64_t kStepUs = 22727;
// The engine's sim clock and the recorded source time are two clocks, and in a real recording
// they do not advance by identical integers.  Making them identical here would leave
// O3Config::maxClockDriftFrac unreachable by any mutation -- the tightest possible bound would
// still pass -- so the fixture carries the small honest disagreement a real one would.
constexpr uint64_t kSimStepUs = 22730;

}  // namespace

void Corpus::Rebuild(const char* producer, uint64_t nonce) {
    stream.tag = replay::verify::MintSourceTag(producer, nonce);
    stream.instants = instants;
    stream.count = kInstants;

    // The SHAPE is seeded identically whoever the producer is: a difference between two
    // corpora must only ever be one a test asked for.
    Lcg rng(0x5EEDull);
    for (uint32_t t = 0; t < kInstants; ++t) {
        for (uint32_t p = 0; p < kPlayers; ++p) {
            for (uint32_t b = 0; b < kBones[p]; ++b) {
                float* q = ori[t][p] + 4 * b;
                q[0] = rng.Unit();
                q[1] = rng.Unit();
                q[2] = rng.Unit();
                q[3] = rng.Unit() + 1.5f;  // keep w dominant so the smallest-three selector is stable
                Normalise(q);
                float* v = pos[t][p] + 3 * b;
                v[0] = rng.Unit() * 10.0f;
                v[1] = rng.Unit() * 3.0f;
                v[2] = rng.Unit() * 20.0f;
            }
            frames[t][p].playerId = 100 + p;
            frames[t][p].boneCount = kBones[p];
            frames[t][p].boneO = ori[t][p];
            frames[t][p].boneT = pos[t][p];
        }
        const uint64_t at = 1000000ull + uint64_t(t) * kStepUs;
        const uint64_t sim = 3000000ull + uint64_t(t) * kSimStepUs;
        instants[t].sourceTimeUs = at;
        instants[t].simTimeBeforeUs = sim;
        instants[t].simTimeAfterUs = sim;
        instants[t].frameIndexBefore = 7000 + t;
        instants[t].frameIndexAfter = 7000 + t;
        instants[t].playerCount = kPlayers;
        instants[t].players = frames[t];
    }
}

void BuildGoldenPair(Corpus& live, Corpus& replay) {
    live.Rebuild("live-capture", 1);
    replay.Rebuild("replay-pass", 2);

    // A sub-tolerance difference on every bone, so the golden PASS is a real measurement against
    // the thresholds rather than a memcmp that no tolerance can reach.  0.02 deg and 0.1 mm sit
    // comfortably under SC2's 0.1 deg and 0.5 mm.
    static const float kAxis[3] = {0.3f, 0.8f, 0.5f};
    for (uint32_t t = 0; t < Corpus::kInstants; ++t) {
        for (uint32_t p = 0; p < Corpus::kPlayers; ++p) {
            for (uint32_t b = 0; b < Corpus::kBones[p]; ++b) {
                RotateBy(replay.ori[t][p] + 4 * b, kAxis, 0.02);
                replay.pos[t][p][3 * b + 1] += 0.0001f;
            }
        }
    }
}

}  // namespace w2i
