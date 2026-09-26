#pragma once

// W2-I.  The synthetic skeleton corpus the O3 arms run on, and the perturbations applied to it.
//
// ★ WHY A SYNTHETIC CORPUS IS THE RIGHT SUBJECT HERE, AND WHERE IT STOPS BEING ONE.
// O3's subject is a COMPARATOR: given two bone streams, does it decide correctly, and can it be
// made to decide wrongly?  That question is answerable with constructed streams and is answerable
// exactly -- the perturbation applied is known to the microdegree, so "the gate saw it" and "the
// gate did not" are facts rather than impressions.  What a synthetic corpus CANNOT establish is
// whether the engine's solvers move bones the way the perturbation assumes; that is the T3 sweep,
// and no arm here claims it.
//
// The golden pair is deliberately NOT identical.  Two byte-identical streams make every tolerance
// in O3Config unreachable -- the comparison passes through memcmp and no threshold is exercised --
// which is the "arm no mutation can reach" shape.  So the replay side carries a sub-tolerance
// difference on every bone, and the PASS arms report a non-zero max outlier.

#include "verify/o3.h"

#include <stdint.h>

namespace w2i {

// Deterministic, seeded, and reproduced identically on both sides of a comparison so that a
// difference in a stream is only ever the difference the test asked for.
struct Lcg {
    uint64_t s;
    explicit Lcg(uint64_t seed) : s(seed ? seed : 1) {}
    uint32_t Next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return uint32_t(s >> 33);
    }
    float Unit() { return float(Next()) / 2147483648.0f - 1.0f; }
};

// One stream's storage.  Owned by value so an arm can build two and know they cannot alias.
struct Corpus {
    static constexpr uint32_t kInstants = 40;
    static constexpr uint32_t kPlayers = 3;
    // Deliberately unequal per player: the oracle must carry the count from the data, and a
    // hardcoded 23 would be caught here rather than in vivo.
    static constexpr uint32_t kBones[kPlayers] = {23, 19, 27};
    static constexpr uint32_t kMaxBones = 27;

    float                       ori[kInstants][kPlayers][kMaxBones * 4];
    float                       pos[kInstants][kPlayers][kMaxBones * 3];
    replay::verify::BoneFrame   frames[kInstants][kPlayers];
    replay::verify::SkeletonInstant instants[kInstants];
    replay::verify::SkeletonStream  stream;

    void Rebuild(const char* producer, uint64_t nonce);
};

// The two corpora an arm starts from: same bones, two producers, sub-tolerance difference.
void BuildGoldenPair(Corpus& live, Corpus& replay);

}  // namespace w2i
