// The hand-transcribed reference decoder.
//
// This is a SECOND transcription of NRadEngine::CBitReader, written from the disassembly of
// echovr.exe rather than from src\foundation\bitio.  It shares no header, no constant and no helper
// with the shipping codec; the constants below are typed here as literal IEEE-754 bit patterns and the
// control flow follows the instruction stream, not the shipping code's structure.
//
// ============================================================================================
// WHAT THIS ARM'S INDEPENDENCE ACTUALLY IS -- stated plainly because the design (M9) records that
// the previous version of O1 claimed three independent sources and had none.
//
// Both this decoder and the shipping one trace to ONE reading of echovr.exe, by one engineer.  So this
// arm catches: a transcription slip, a mis-ordered multiply, a wrong bit order, a permuted selector
// mapping, a constant typed with two digits swapped.  It does NOT catch a wrong READ of the binary --
// if the disassembly was misunderstood, both sides are wrong together.
//
// Nothing offline can close that gap.  The design is explicit: O1 does not establish that our
// constants match the engine; W2-I's O2 does, by comparing our decode of real PC wire bytes against the
// PC engine's own in-memory f32.  This file is not evidence of engine agreement and must not be quoted
// as if it were.
// ============================================================================================
//
// Disassembly this was transcribed from, all in echovr.exe md5 caa05ee3bca2f5df0809994e93e2cc05:
//   CBitReader::Read         0x1415278B0   LSB-first into 64-bit little-endian words
//   CBitReader::BitPackReal  0x141527630   divss  at 0x141527656; value = idx*step + min
//   CBitReader::BitPackQuat  0x141527350   mulss/mulss/subss at 0x141527440..0x141527448
#pragma once

#include <stdint.h>
#include <string.h>

#include <cmath>

namespace o1ref {

// Typed from the .rdata dwords, not from a decimal rendering.
//   0x141C3EAD4 = 3F B5 04 F3   sqrt(2)
//   0x141C44C74 = 3F 35 04 F3   1/sqrt(2)
//   0x1416CCEF8 = 3F 80 00 00   1.0
inline float FromBits(uint32_t u) {
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

struct Cursor {
    const uint8_t* data;
    size_t         size;
    size_t         bit;
    bool           bad;
};

// CBitReader::Read.  The engine indexes qwords and shifts right by the in-word bit position; a field
// that straddles a word boundary takes the low part from one word and the high part from the next,
// shifted up by however many bits the first word supplied.
inline uint64_t RefRead(Cursor& c, unsigned n) {
    if (n == 0) return 0;
    if (n > 64 || c.bad || c.bit + n > c.size * 8) { c.bad = true; return 0; }

    auto word = [&](size_t index) -> uint64_t {
        uint64_t w = 0;
        for (int i = 0; i < 8; ++i) {
            const size_t off = index * 8 + static_cast<size_t>(i);
            const uint64_t b = (off < c.size) ? c.data[off] : 0u;
            w |= b << (8 * i);
        }
        return w;
    };

    const size_t   wi = c.bit / 64;
    const unsigned bp = static_cast<unsigned>(c.bit % 64);
    const unsigned take = (64 - bp < n) ? (64 - bp) : n;

    uint64_t v = word(wi) >> bp;
    if (take != 64) v &= (1ull << take) - 1ull;

    const unsigned rest = n - take;
    if (rest) v |= (word(wi + 1) & ((1ull << rest) - 1ull)) << take;

    c.bit += n;
    return v;
}

// The step count both sides of the engine derive the field width from.  The READER forms this with a
// division; the writer forms it with a reciprocal multiply.  This file is the reader, so it divides.
inline uint64_t RefSteps(float mn, float mx, float st) {
    const float span = (mx - mn) / st;
    const float up = std::ceil(span);
    if (!(up >= 0.0f)) return 0;
    return static_cast<uint64_t>(up);
}

inline unsigned RefWidth(uint64_t steps) {
    unsigned w = 0;
    while (steps) { ++w; steps >>= 1; }
    return w;
}

inline float RefBitPackReal(Cursor& c, float mn, float mx, float st) {
    const uint64_t steps = RefSteps(mn, mx, st);
    const uint64_t idx = RefRead(c, RefWidth(steps));
    if (idx > steps) c.bad = true;
    return static_cast<float>(static_cast<int64_t>(idx)) * st + mn;
}

inline void RefBitPackVec(Cursor& c, float mn, float mx, float st, float out[3]) {
    out[0] = RefBitPackReal(c, mn, mx, st);
    out[1] = RefBitPackReal(c, mn, mx, st);
    out[2] = RefBitPackReal(c, mn, mx, st);
}

// CBitReader::BitPackQuat.  Two selector bits, then three n-bit indices.  Each index becomes
// idx * (1/mask) * sqrt2 - 1/sqrt2, and the component named by the selector is rebuilt as the positive
// square root of what is left of one -- which is why the encoder negates the whole quaternion when its
// largest component is negative.
inline void RefBitPackQuat(Cursor& c, unsigned n, float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (n == 0 || n > 32) { c.bad = true; return; }

    const uint64_t largest = RefRead(c, 2);
    const uint64_t i0 = RefRead(c, n);
    const uint64_t i1 = RefRead(c, n);
    const uint64_t i2 = RefRead(c, n);
    if (largest >= 4) { c.bad = true; return; }

    const uint64_t mask = (n >= 64) ? ~0ull : ((1ull << n) - 1ull);
    // Written as a plain 1.0f, not as a bit pattern: it is exactly representable, so there is nothing to
    // transcribe, and this file stays free of the registry the shipping codec cites.
    const float one = 1.0f;
    const float recip = one / static_cast<float>(static_cast<int64_t>(mask));
    const float sq2 = FromBits(0x3FB504F3u);
    const float half = FromBits(0x3F3504F3u);

    const float a = static_cast<float>(static_cast<int64_t>(i0)) * recip * sq2 - half;
    const float b = static_cast<float>(static_cast<int64_t>(i1)) * recip * sq2 - half;
    const float d = static_cast<float>(static_cast<int64_t>(i2)) * recip * sq2 - half;
    const float rebuilt = std::sqrt(one - a * a - b * b - d * d);

    switch (largest) {
        case 0: out[0] = rebuilt; out[1] = a; out[2] = b; out[3] = d; break;
        case 1: out[0] = a; out[1] = rebuilt; out[2] = b; out[3] = d; break;
        case 2: out[0] = a; out[1] = b; out[2] = rebuilt; out[3] = d; break;
        default: out[0] = a; out[1] = b; out[2] = d; out[3] = rebuilt; break;
    }
}

}  // namespace o1ref
