// The five float constants NRadEngine::CBitWriter / CBitReader compute with, as the exact IEEE-754
// bit patterns read out of echovr.exe (md5 caa05ee3bca2f5df0809994e93e2cc05).
//
// They are bit patterns rather than decimal literals for the reason the design gives: a decimal
// rendering of a float is a lossy restatement, and a compiler is free to round it differently than the
// one that built the engine.  tests\o1\constcheck.py reads each VA back out of the shipped binary and
// fails if a value here disagrees, so a typo cannot survive a gate run.
//
// The 1.0f is CITED from the registry, not written down again: it is the same .rdata dword as
// AUDIO.MAXFREQRATIO.  The other four have no registry row yet -- they are .rdata f32 slots and the guards
// that cover that shape live in factsnchors.tsv, which is W0-2's file.
// facts\pending\W1-A.tsv hands W0-2 the rows and the anchor lines ready to merge; until then
// tests\o1\constcheck.py is their guard and reports how many have landed, so nought reads as nought.
#pragma once

#include "generated/facts_generated.h"

#include <stdint.h>
#include <string.h>

namespace bitio {

struct EngineConstant {
    const char* id;        // registry id once merged
    uint64_t    va;        // the .rdata slot
    uint32_t    bits;      // the f32 bit pattern
    uint64_t    loadSite;  // the instruction in the decompiled codec that RIP-addresses that slot
};

// clang-format off
inline constexpr EngineConstant kEngineConstants[] = {
    // 1.0f, the numerator of 1/step.  This is the SAME .rdata dword the registry already pins as
    // AUDIO.MAXFREQRATIO: the compiler folded Wwise's MaxFrequencyRatio initialiser and CBitReader's
    // numerator onto one slot.  Cited rather than duplicated -- and see
    // the W1-A fact drop for what the sharing means for 10.4's rate ladder.
    { "AUDIO.MAXFREQRATIO",  facts::AUDIO_MAXFREQRATIO_VA, facts::AUDIO_MAXFREQRATIO_BITS, 0x1415273F8ull },
    { "BITIO.REAL_HALF",     0x1416CCEF4ull, 0x3F000000u, 0x141527D82ull },  //  0.5f   round-to-nearest bias
    { "BITIO.QUAT_SCALE",    0x141C3EAD4ull, 0x3FB504F3u, 0x141527430ull },  //  sqrt(2)      decode span
    { "BITIO.QUAT_BIAS_POS", 0x141C44C74ull, 0x3F3504F3u, 0x141527448ull },  //  1/sqrt(2)    decode offset, encode gain
    { "BITIO.QUAT_BIAS_NEG", 0x141C44C98ull, 0xBF3504F3u, 0x141527D6Eull },  // -1/sqrt(2)    encode offset
};
// clang-format on

constexpr size_t kEngineConstantCount = sizeof(kEngineConstants) / sizeof(kEngineConstants[0]);

inline float AsFloat(uint32_t bits) {
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

inline float EngineConst(size_t i) { return AsFloat(kEngineConstants[i].bits); }

inline const float kOne        = EngineConst(0);
inline const float kHalf       = EngineConst(1);
inline const float kQuatScale  = EngineConst(2);
inline const float kQuatBiasP  = EngineConst(3);
inline const float kQuatBiasN  = EngineConst(4);

}  // namespace bitio
