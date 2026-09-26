// The PC wire schedule for the two BitPack serialisers, in wire order, with every quantisation
// parameter as the IEEE-754 bit pattern read out of echovr.exe.
//
// Derived by decompiling the PC serialisers, NOT carried over from the ARM64 build.  The design
// records that every WIRE_Q parameter was previously ARM64-only and that "F10 is a live demonstration
// that ARM->PC carry-over is exactly how this project gets bitten"; these are PC reads, so that
// containment is not needed for them.  Registry rows: facts\pending\W1-A.tsv (W0-2 merges).
//
//   SR15NetPhysicsSimState::BitPack<CBitWriter>  0x140C515F0   284 bits
//   SR15NetPlayerState::BitPack<CBitWriter>      0x140CB09E0   370 bits
//                                    (inner)     0x140C51A00   251 bits, called first
//
// ★ What this table is NOT.  It states the field ORDER and the PARAMETERS the PC serialisers use.  It
// does not state what this project stores, at what cadence, or with what retention -- the design is
// the only place that does (5.4.8, and it is normative).  A stream schema cites these fields; it does
// not restate a policy.
#pragma once

#include "foundation/bitio/bitio.h"
#include "generated/facts_generated.h"

#include <bit>

namespace bitio {

inline constexpr float Kf(uint32_t bits) { return std::bit_cast<float>(bits); }

// Every distinct .rdata slot the two serialisers pass to BitPackReal / BitPackVec.  tests\o1\constcheck.py
// reads each VA back out of echovr.exe and fails on a disagreement, so a mistyped digit cannot survive.
struct WireConstant {
    const char* id;
    uint64_t    va;
    uint32_t    bits;
    uint64_t    loadSite;   // the serialiser instruction that RIP-addresses that slot
};

// clang-format off
inline constexpr WireConstant kWireConstants[] = {
    { "BITIO.WQ_POS250",   0x1416E6B28ull, 0x437A0000u, 0x140C5170Aull },  //  250.0
    { "BITIO.WQ_NEG250",   0x141CBB620ull, 0xC37A0000u, 0x140C51716ull },  // -250.0
    { "BITIO.WQ_MM",       0x1416CCEF0ull, 0x3A83126Fu, 0x140C51726ull },  //  0.001
    { "BITIO.WQ_POS48",    0x1416D5A54ull, 0x42400000u, 0x140C51839ull },  //  48.0
    { "BITIO.WQ_NEG48",    0x141CBB618ull, 0xC2400000u, 0x140C51844ull },  // -48.0
    { "BITIO.WQ_VSTEP",    0x141C39E4Cull, 0x3D000000u, 0x140C5182Dull },  //  0.03125
    { "BITIO.WQ_POS144",   0x141CBB5D0ull, 0x43100000u, 0x140C518D4ull },  //  144.0
    { "BITIO.WQ_NEG144",   0x141CBB61Cull, 0xC3100000u, 0x140C518DFull },  // -144.0
    { "BITIO.WQ_POS2",     0x1416E6AF8ull, 0x40000000u, 0x140CB0A02ull },  //  2.0
    { "BITIO.WQ_NEG2",     0x1416E8200ull, 0xC0000000u, 0x140CB0A0Aull },  // -2.0
    { "BITIO.WQ_POSPI",    0x1416E6B00ull, 0x40490FDBu, 0x140C51EFEull },  //  pi
    { "BITIO.WQ_NEGPI",    0x14170F128ull, 0xC0490FDBu, 0x140C51F09ull },  // -pi
    { "BITIO.WQ_DEG",      0x1416E80B0ull, 0x3C8EFA35u, 0x140C51F1Bull },  //  1 degree in radians
    { "BITIO.WQ_RESTMAX",  0x141C689B8ull, 0x3FE33333u, 0x140C52036ull },  //  1.775
    { "BITIO.WQ_RESTSTEP", 0x1416E809Cull, 0x3BA3D70Au, 0x140C5202Bull },  //  0.005
    // 1.0, and the same folded dword as AUDIO.MAXFREQRATIO -- see engineconst.h.
    { "AUDIO.MAXFREQRATIO", facts::AUDIO_MAXFREQRATIO_VA, facts::AUDIO_MAXFREQRATIO_BITS, 0x1415273F8ull },
    { "BITIO.WQ_BLENDSTEP",0x1416E6AE0ull, 0x3DCCCCCDu, 0x140CB0CFCull },  //  0.1
};
// clang-format on

inline constexpr size_t kWireConstantCount = sizeof(kWireConstants) / sizeof(kWireConstants[0]);

// restposeheight's minimum and every trigger blend's minimum are +0.0f materialised by `xorps`, so they
// have no .rdata slot to cite. The instruction is the evidence: 0x140C52033 and 0x140CB0CF9.
inline constexpr float kZeroMin = 0.0f;

struct WireField {
    const char* name;
    FieldClass  cls;
    float       min, max, step;   // WireQ only
    uint8_t     bits;             // Raw width, or WireQuat component width
};

// clang-format off
inline constexpr WireField kSimStateWire[] = {
    { "bits",              FieldClass::Raw,      0, 0, 0,                                  11 },
    { "component.netid",   FieldClass::Raw,      0, 0, 0,                                  64 },
    { "com",               FieldClass::WireQ,    Kf(0xC37A0000), Kf(0x437A0000), Kf(0x3A83126F), 0 },
    { "ori",               FieldClass::WireQuat, 0, 0, 0,                                  10 },
    { "v",                 FieldClass::WireQ,    Kf(0xC2400000), Kf(0x42400000), Kf(0x3D000000), 0 },
    { "w",                 FieldClass::WireQ,    Kf(0xC3100000), Kf(0x43100000), Kf(facts::AUDIO_MAXFREQRATIO_BITS), 0 },
    { "lastheldcom",       FieldClass::WireQ,    Kf(0xC37A0000), Kf(0x437A0000), Kf(0x3A83126F), 0 },
};

inline constexpr WireField kPlayerStateWire[] = {
    { "ori",                   FieldClass::WireQuat, 0, 0, 0,                                  10 },
    { "lefthandori",           FieldClass::WireQuat, 0, 0, 0,                                  10 },
    { "righthandori",          FieldClass::WireQuat, 0, 0, 0,                                  10 },
    { "refvelocity",           FieldClass::WireQ,    Kf(0xC2400000), Kf(0x42400000), Kf(0x3D000000), 0 },
    { "bits",                  FieldClass::Raw,      0, 0, 0,                                  28 },
    { "holding.left.slotidx",  FieldClass::Raw,      0, 0, 0,                                  16 },
    { "holding.left.genidx",   FieldClass::Raw,      0, 0, 0,                                  16 },
    { "holding.right.slotidx", FieldClass::Raw,      0, 0, 0,                                  16 },
    { "holding.right.genidx",  FieldClass::Raw,      0, 0, 0,                                  16 },
    { "clippingbackbendang",   FieldClass::WireQ,    Kf(0xC0490FDB), Kf(0x40490FDB), Kf(0x3C8EFA35), 0 },
    { "clippingforwardbendang",FieldClass::WireQ,    Kf(0xC0490FDB), Kf(0x40490FDB), Kf(0x3C8EFA35), 0 },
    { "restposeheight",        FieldClass::WireQ,    kZeroMin,       Kf(0x3FE33333), Kf(0x3BA3D70A), 0 },
    { "lefthandoffset",        FieldClass::WireQ,    Kf(0xC0000000), Kf(0x40000000), Kf(0x3A83126F), 0 },
    { "righthandoffset",       FieldClass::WireQ,    Kf(0xC0000000), Kf(0x40000000), Kf(0x3A83126F), 0 },
    { "lefthandtriggerblend",  FieldClass::WireQ,    kZeroMin,       Kf(facts::AUDIO_MAXFREQRATIO_BITS), Kf(0x3DCCCCCD), 0 },
    { "righthandtriggerblend", FieldClass::WireQ,    kZeroMin,       Kf(facts::AUDIO_MAXFREQRATIO_BITS), Kf(0x3DCCCCCD), 0 },
    { "anchor",                FieldClass::Raw,      0, 0, 0,                                   3 },
    { "refvelocity.dup",       FieldClass::WireQ,    Kf(0xC2400000), Kf(0x42400000), Kf(0x3D000000), 0 },
};
// clang-format on

inline constexpr size_t kSimStateWireCount = sizeof(kSimStateWire) / sizeof(kSimStateWire[0]);
inline constexpr size_t kPlayerStateWireCount = sizeof(kPlayerStateWire) / sizeof(kPlayerStateWire[0]);

// com, v, w, lastheldcom, refvelocity and the two hand offsets are three-axis: BitPackVec writes the same
// field three times.
inline bool IsVectorField(const WireField& f) {
    const char* n = f.name;
    auto eq = [n](const char* s) {
        const char* a = n;
        while (*a && *a == *s) { ++a; ++s; }
        return *a == 0 && *s == 0;
    };
    return eq("com") || eq("v") || eq("w") || eq("lastheldcom") || eq("refvelocity") ||
           eq("refvelocity.dup") || eq("lefthandoffset") || eq("righthandoffset");
}

inline FieldSpec SpecOf(const WireField& f, uint16_t id) {
    return FieldSpec{id, f.cls, f.min, f.max, f.step, f.bits, FieldProv::Pc};
}

}  // namespace bitio
