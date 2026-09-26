#include "capture/streamcodec.h"

#include <climits>
#include <string.h>

#include "foundation/bitio/enginewire.h"
#include "foundation/bitio/wirefields.h"

namespace replay {
namespace capture {
namespace {

using bitio::BitReader;
using bitio::BitWriter;
using bitio::FieldClass;
using bitio::FieldProv;
using bitio::FieldSpec;

// ---------------------------------------------------------------------------------------------
// ★ A NOTE ON `FieldProv` FOR THE CONTAINER'S OWN FRAMING FIELDS, because the honest answer is not one
// of the three values the enum offers.
//
// bitio::FieldProv records which binary a field's parameters were read out of, and refuses Unknown so
// that an under-initialised spec cannot encode. That is exactly right for a WIRE_Q field, whose
// (min,max,step) IS a measurement. It has no meaning for a Raw field whose width this file CHOSE --
// a stamp delta class, an entity index, a record count. There is no `Ours` value, and bitio.h belongs
// to W1-A, so these are spelled `Pc` and the discrepancy is recorded here and in W2-A's handoff rather
// than silently accepted. Nothing reads the column for these fields; FieldRefusal only checks the width.
//
// Raw fields that DO carry an engine value at an engine-derived width -- bits28, component.netid, the
// SValue qwords, the holding ids -- take their widths from bitio::wirefields.h, which is a PC read.
// ---------------------------------------------------------------------------------------------
constexpr FieldSpec RawSpec(uint16_t id, uint8_t bits) {
    return FieldSpec{id, FieldClass::Raw, 0.0f, 0.0f, 0.0f, bits, FieldProv::Pc};
}
constexpr FieldSpec F32Spec(uint16_t id) {
    return FieldSpec{id, FieldClass::F32, 0.0f, 0.0f, 0.0f, 0, FieldProv::Pc};
}

// Framing widths. The class selector is two bits and the escape is total, so every value is
// representable and the code never has to refuse a legal input.
constexpr uint8_t kVarClassBits[4] = {8, 16, 24, 64};
constexpr FieldSpec kVarClassSpec = RawSpec(0xF000, 2);
const FieldSpec kVarSpec[4] = {RawSpec(0xF001, 8), RawSpec(0xF002, 16), RawSpec(0xF003, 24),
                               RawSpec(0xF004, 64)};
constexpr FieldSpec kU64Spec = RawSpec(0xF005, 64);
constexpr FieldSpec kU32Spec = RawSpec(0xF006, 32);
constexpr FieldSpec kU16Spec = RawSpec(0xF007, 16);
constexpr FieldSpec kU8Spec = RawSpec(0xF008, 8);
constexpr FieldSpec kLen6Spec = RawSpec(0xF009, 6);
constexpr FieldSpec kDict7Spec = RawSpec(0xF00A, 7);
// The manifest row for a stored-not-decoded 64-bit word.
constexpr FieldSpec kU64Field = RawSpec(0xF00C, 64);
// The per-row fill mask (streamcodec.h): which fields of this row were actually sourced.
constexpr FieldSpec kFillField = RawSpec(0xF00D, 16);
constexpr FieldSpec kU32Field = RawSpec(0xF00E, 32);

inline uint64_t ZigZag(int64_t d) {
    return (static_cast<uint64_t>(d) << 1) ^ static_cast<uint64_t>(d >> 63);
}
inline int64_t UnZigZag(uint64_t z) {
    return static_cast<int64_t>(z >> 1) ^ -static_cast<int64_t>(z & 1);
}
inline unsigned BitLength(uint64_t v) {
    unsigned n = 0;
    while (v) { ++n; v >>= 1; }
    return n;
}
inline unsigned Clz64(uint64_t v) { return v ? 64u - BitLength(v) : 64u; }
inline unsigned Ctz64(uint64_t v) {
    if (!v) return 64u;
    unsigned n = 0;
    while (((v >> n) & 1u) == 0u) ++n;
    return n;
}

unsigned PickVarClass(uint64_t zz) {
    const unsigned need = BitLength(zz);
    for (unsigned k = 0; k < 3; ++k) {
        if (need <= kVarClassBits[k]) return k;
    }
    return 3;
}

// ---------------------------------------------------------------------------------------------
// The two codecs. Same method names, same order of operations; the field walks below are written once
// and instantiated for each. A field added to one is added to both, because there is only one listing.
// ---------------------------------------------------------------------------------------------

// Defined below, beside the body specs whose ids it reads.
const char* BodyFieldName(uint16_t id);

struct WriteCodec {
    BitWriter& w;
    RefusedField* note = nullptr;  // the first field that could not be stored, when a caller wants it
    static constexpr bool kWriting = true;

    void Flag(bool& v) { w.PackFlag(v); }
    void FlagOut(bool v) { w.PackFlag(v); }

    void Raw(const FieldSpec& f, uint64_t& v) { PackRawN(f, v); }
    void Raw32(const FieldSpec& f, uint32_t& v) {
        uint64_t t = v;
        PackRawN(f, t);
    }
    void Raw16(const FieldSpec& f, uint16_t& v) {
        uint64_t t = v;
        PackRawN(f, t);
    }

    // Unchanged flag, then the value. This is S5.4.5's "1-bit unchanged flag" for the static fields.
    void RawKeyed(const FieldSpec& f, uint32_t& v, uint32_t prev, bool absolute) {
        if (absolute) {
            uint64_t t = v;
            PackRawN(f, t);
            return;
        }
        const bool changed = v != prev;
        w.PackFlag(changed);
        if (changed) {
            uint64_t t = v;
            PackRawN(f, t);
        }
    }

    // A 64-bit value against a predecessor, XOR-trimmed. Never an arithmetic difference: for a value
    // that is really float bits, `cur - prev` is meaningless, and for a symbol it is noise.
    void U64Keyed(uint64_t& v, uint64_t prev, bool absolute) {
        if (absolute) {
            PackRawN(kU64Spec, v);
            return;
        }
        const uint64_t x = v ^ prev;
        if (x == 0) {
            w.PackFlag(false);
            return;
        }
        w.PackFlag(true);
        const unsigned lz = Clz64(x);
        const unsigned tz = Ctz64(x);
        const unsigned meaningful = 64u - lz - tz;
        uint64_t a = lz, b = meaningful - 1u, c = x >> tz;
        PackRawN(kLen6Spec, a);
        PackRawN(kLen6Spec, b);
        const FieldSpec m = RawSpec(0xF00B, static_cast<uint8_t>(meaningful));
        PackRawN(m, c);
    }

    // ★ A clock, coded against a PREDICTION rather than against its predecessor. A time advancing by a
    // near-constant amount every frame has a first-order delta of ~11,111 -- 18 bits with the class
    // selector -- and a second-order delta of zero, which the flag below spends one bit on. S5.6.1
    // names this exactly: "a second-order delta is a handful of bits per sample", and it is why every
    // sample of a 118 Hz ramp can be stored rather than decimated.
    //
    // Measured, uncompressed bytes per S_ASSEEN record on the 900-frame 8-player synthetic:
    //     XOR against the predecessor              44.85
    //     first-order arithmetic delta             35.51
    //     second-order, no exact-prediction flag   25.62
    //     second-order + the flag                  11.15
    // ★ AND THE HONEST OTHER HALF: after zstd the whole file moved from 73,422 to 74,332 bytes, i.e.
    // NOT AT ALL. On a synthetic whose clocks tick at exactly 11,111 us, zstd was already finding this
    // redundancy. The coding is kept because a real capture's frame times are the measured 72/80/90 Hz
    // interval and vary frame to frame (S5.4.3), which is the case zstd handles worst -- but nobody
    // should read the uncompressed column above as a saving on disk. S5.6's "measure before
    // hand-rolling" applies to this file's own optimisations too.
    void VarPredicted(uint64_t& v, uint64_t prev, uint64_t prevDelta, bool absolute) {
        if (absolute) {
            PackRawN(kU64Spec, v);
            return;
        }
        const uint64_t predicted = static_cast<uint64_t>(static_cast<int64_t>(prev) +
                                                         static_cast<int64_t>(prevDelta));
        // One bit for "the prediction was exact", which is what a clock running at a steady rate does
        // on almost every frame. Without it the smallest a clock field can cost is the class selector
        // plus its narrowest class -- ten bits to say nothing happened.
        const bool exact = v == predicted;
        w.PackFlag(exact);
        if (!exact) {
            VarKeyed(v, predicted, false);
        }
    }

    // A monotone-ish counter: zigzag delta in one of four width classes.
    void VarKeyed(uint64_t& v, uint64_t prev, bool absolute) {
        if (absolute) {
            PackRawN(kU64Spec, v);
            return;
        }
        const uint64_t zz =
            ZigZag(static_cast<int64_t>(v) - static_cast<int64_t>(prev));
        const unsigned k = PickVarClass(zz);
        uint64_t kk = k;
        PackRawN(kVarClassSpec, kk);
        uint64_t t = zz;
        PackRawN(kVarSpec[k], t);
    }

    void Scalar(const FieldSpec& f, float& v, float prev, bool absolute) {
        if (absolute) {
            if (f.cls == FieldClass::WireQ) PackWireN(f, v);
            else PackF32N(f, v);
        } else {
            PackDeltaN(f, prev, v);
        }
    }
    void Vec3(const FieldSpec& f, float v[3], const float prev[3], bool absolute) {
        for (int i = 0; i < 3; ++i) Scalar(f, v[i], prev[i], absolute);
    }
    void Quat(const FieldSpec& f, float q[4], const float prev[4], bool absolute) {
        if (f.cls == FieldClass::WireQuat) {
            if (absolute) PackQuat10N(f, q);
            else PackQuatDeltaN(f, prev, q);
            return;
        }
        for (int i = 0; i < 4; ++i) Scalar(f, q[i], prev[i], absolute);
    }

    void Bytes(const uint8_t* src, uint8_t* /*dst*/, uint16_t n) {
        for (uint16_t i = 0; i < n; ++i) {
            uint64_t t = src[i];
            PackRawN(kU8Spec, t);
        }
    }


    // ---- the refusing field, named -------------------------------------------------------------
    // A refusal is sticky, so the FIRST field to set it is the offending one and this latches once.
    // Sampled either side of each pack rather than inspected at the end of the walk: `Refused()`
    // carries no field, and once it is set every later field looks equally guilty.
    void NoteAfter(const FieldSpec& f, uint64_t value, uint32_t bits, bool was) {
        if (was || !note || note->identified || !w.Refused()) return;
        note->name = BodyFieldName(f.id);
        note->value = value;
        note->width = bits;
        note->identified = true;
    }
    static uint64_t FloatBits(float v) {
        uint32_t t = 0;
        memcpy(&t, &v, sizeof t);
        return t;
    }
    // A quantised field is stored at its wire width; an F32 field at the width of a float.
    static uint32_t ScalarBits(const FieldSpec& f) {
        return f.cls == FieldClass::WireQ ? bitio::WireFieldBits(f)
                                          : static_cast<uint32_t>(sizeof(float) * CHAR_BIT);
    }
    void PackRawN(const FieldSpec& f, uint64_t& v) {
        const bool was = w.Refused();
        w.PackRaw(f, v);
        NoteAfter(f, v, f.rawBits, was);
    }
    void PackWireN(const FieldSpec& f, float v) {
        const bool was = w.Refused();
        w.PackWire(f, v);
        NoteAfter(f, FloatBits(v), ScalarBits(f), was);
    }
    void PackF32N(const FieldSpec& f, float v) {
        const bool was = w.Refused();
        w.PackF32(f, v);
        NoteAfter(f, FloatBits(v), ScalarBits(f), was);
    }
    void PackDeltaN(const FieldSpec& f, float prev, float cur) {
        const bool was = w.Refused();
        w.PackDelta(f, prev, cur);
        NoteAfter(f, FloatBits(cur), ScalarBits(f), was);
    }
    // Smallest-three: three components at the field's own width, plus the two bits that say which
    // one was dropped. A quaternion we sampled ourselves is four floats instead.
    static uint32_t QuatBits(const FieldSpec& f) {
        return f.cls == FieldClass::WireQuat
                   ? 3u * f.rawBits + 2u
                   : 4u * static_cast<uint32_t>(sizeof(float) * CHAR_BIT);
    }
    void PackQuat10N(const FieldSpec& f, const float q[4]) {
        const bool was = w.Refused();
        w.PackQuat10(f, q);
        NoteAfter(f, FloatBits(q[0]), QuatBits(f), was);
    }
    void PackQuatF32N(const FieldSpec& f, const float q[4]) {
        const bool was = w.Refused();
        w.PackQuatF32(f, q);
        NoteAfter(f, FloatBits(q[0]), QuatBits(f), was);
    }
    void PackQuatDeltaN(const FieldSpec& f, const float prev[4], const float cur[4]) {
        const bool was = w.Refused();
        w.PackQuatDelta(f, prev, cur);
        NoteAfter(f, FloatBits(cur[0]), QuatBits(f), was);
    }

    bool Bad() const { return w.Refused(); }
};

struct ReadCodec {
    BitReader& r;
    static constexpr bool kWriting = false;

    void Flag(bool& v) { v = r.UnpackFlag(); }
    void FlagOut(bool /*v*/) { (void)r.UnpackFlag(); }

    void Raw(const FieldSpec& f, uint64_t& v) { v = r.UnpackRaw(f); }
    void Raw32(const FieldSpec& f, uint32_t& v) { v = static_cast<uint32_t>(r.UnpackRaw(f)); }
    void Raw16(const FieldSpec& f, uint16_t& v) { v = static_cast<uint16_t>(r.UnpackRaw(f)); }

    void RawKeyed(const FieldSpec& f, uint32_t& v, uint32_t prev, bool absolute) {
        if (absolute) {
            v = static_cast<uint32_t>(r.UnpackRaw(f));
            return;
        }
        v = r.UnpackFlag() ? static_cast<uint32_t>(r.UnpackRaw(f)) : prev;
    }

    void U64Keyed(uint64_t& v, uint64_t prev, bool absolute) {
        if (absolute) {
            v = r.UnpackRaw(kU64Spec);
            return;
        }
        if (!r.UnpackFlag()) {
            v = prev;
            return;
        }
        const unsigned lz = static_cast<unsigned>(r.UnpackRaw(kLen6Spec));
        const unsigned meaningful = static_cast<unsigned>(r.UnpackRaw(kLen6Spec)) + 1u;
        if (lz + meaningful > 64u) {
            v = prev;
            return;   // a malformed block; Overrun()/BitsRead() is what the caller checks
        }
        const FieldSpec m = RawSpec(0xF00B, static_cast<uint8_t>(meaningful));
        const uint64_t c = r.UnpackRaw(m);
        const unsigned tz = 64u - lz - meaningful;
        v = prev ^ (c << tz);
    }

    void VarPredicted(uint64_t& v, uint64_t prev, uint64_t prevDelta, bool absolute) {
        if (absolute) {
            v = r.UnpackRaw(kU64Spec);
            return;
        }
        const uint64_t predicted = static_cast<uint64_t>(static_cast<int64_t>(prev) +
                                                         static_cast<int64_t>(prevDelta));
        if (r.UnpackFlag()) {
            v = predicted;
            return;
        }
        VarKeyed(v, predicted, false);
    }

    void VarKeyed(uint64_t& v, uint64_t prev, bool absolute) {
        if (absolute) {
            v = r.UnpackRaw(kU64Spec);
            return;
        }
        const unsigned k = static_cast<unsigned>(r.UnpackRaw(kVarClassSpec));
        const uint64_t zz = r.UnpackRaw(kVarSpec[k]);
        v = static_cast<uint64_t>(static_cast<int64_t>(prev) + UnZigZag(zz));
    }

    void Scalar(const FieldSpec& f, float& v, float prev, bool absolute) {
        if (absolute) {
            v = (f.cls == FieldClass::WireQ) ? r.UnpackWire(f) : r.UnpackF32(f);
        } else {
            v = r.UnpackDelta(f, prev);
        }
    }
    void Vec3(const FieldSpec& f, float v[3], const float prev[3], bool absolute) {
        for (int i = 0; i < 3; ++i) Scalar(f, v[i], prev[i], absolute);
    }
    void Quat(const FieldSpec& f, float q[4], const float prev[4], bool absolute) {
        if (f.cls == FieldClass::WireQuat) {
            if (absolute) r.UnpackQuat10(f, q);
            else r.UnpackQuatDelta(f, prev, q);
            return;
        }
        for (int i = 0; i < 4; ++i) Scalar(f, q[i], prev[i], absolute);
    }

    void Bytes(const uint8_t* /*src*/, uint8_t* dst, uint16_t n) {
        for (uint16_t i = 0; i < n; ++i) dst[i] = static_cast<uint8_t>(r.UnpackRaw(kU8Spec));
    }

    bool Bad() const { return r.Refused() || r.Overrun(); }
};

// ---------------------------------------------------------------------------------------------
// Field specs, derived from bitio::wirefields.h rather than retyped. The SAMPLED set is generated from
// the WIRE set by class substitution, so the two lists cannot drift apart and no sampled field can be
// WireQ -- which is the static half of S5.3.3's guard, held by construction rather than by a checker.
// ---------------------------------------------------------------------------------------------

struct PlayerSpecs {
    FieldSpec ori, lHandOri, rHandOri, refVel, bits28, holdSlot, holdGen, bend, restPose, handOff,
        trigger, anchor;
};

PlayerSpecs MakePlayerSpecs(bool sampled) {
    using bitio::kPlayerStateWire;
    auto pick = [&](size_t i, uint16_t id) {
        FieldSpec s = bitio::SpecOf(kPlayerStateWire[i], id);
        if (!sampled) return s;
        if (s.cls == FieldClass::WireQ || s.cls == FieldClass::WireQuat) {
            s.cls = FieldClass::F32;
            s.rawBits = 0;
        }
        return s;
    };
    PlayerSpecs p{};
    p.ori = pick(0, 1);
    p.lHandOri = pick(1, 2);
    p.rHandOri = pick(2, 3);
    p.refVel = pick(3, 4);
    p.bits28 = bitio::SpecOf(kPlayerStateWire[4], 5);      // Raw in both classes
    p.holdSlot = bitio::SpecOf(kPlayerStateWire[5], 6);
    p.holdGen = bitio::SpecOf(kPlayerStateWire[6], 7);
    p.bend = pick(9, 8);
    p.restPose = pick(11, 9);
    p.handOff = pick(12, 10);
    p.trigger = pick(14, 11);
    p.anchor = bitio::SpecOf(kPlayerStateWire[16], 12);
    return p;
}

struct BodySpecs {
    FieldSpec bits11, netId, com, ori, vel, angVel, lastHeldCom;
};

BodySpecs MakeBodySpecs(bool sampled) {
    using bitio::kSimStateWire;
    auto pick = [&](size_t i, uint16_t id) {
        FieldSpec s = bitio::SpecOf(kSimStateWire[i], id);
        if (!sampled) return s;
        if (s.cls == FieldClass::WireQ || s.cls == FieldClass::WireQuat) {
            s.cls = FieldClass::F32;
            s.rawBits = 0;
        }
        return s;
    };
    BodySpecs b{};
    b.bits11 = bitio::SpecOf(kSimStateWire[0], 20);
    b.netId = bitio::SpecOf(kSimStateWire[1], 21);
    b.com = pick(2, 22);
    b.ori = pick(3, 23);
    b.vel = pick(4, 24);
    b.angVel = pick(5, 25);
    b.lastHeldCom = pick(6, 26);
    return b;
}

// The refusing field by NAME. The ids come from MakeBodySpecs above rather than from a second list
// of numbers, so a renumbered field renames itself here and cannot drift. nullptr for a field this
// walk does not own -- the entity head, or a stream that is not a body.
const char* BodyFieldName(uint16_t id) {
    static const BodySpecs kIds = MakeBodySpecs(false);
    struct Row {
        uint16_t id;
        const char* name;
    };
    const Row rows[] = {
        {kIds.bits11.id, "bits11"}, {kIds.netId.id, "netId"}, {kIds.com.id, "com"},
        {kIds.ori.id, "ori"},       {kIds.vel.id, "vel"},     {kIds.angVel.id, "angVel"},
        {kIds.lastHeldCom.id, "lastHeldCom"},
    };
    for (const Row& r : rows) {
        if (r.id == id) return r.name;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// The field walks. One listing each, used by the encoder and the decoder.
// ---------------------------------------------------------------------------------------------

template <class C>
void WalkPlayer(C& c, const PlayerSpecs& s, PlayerFrame& f, const PlayerFrame& prev, bool absolute,
                bool sampled) {
    // The fill mask leads, so a decoder knows which of the fields below are measurements before it
    // reads them. It is delta-coded like everything else and is constant in practice, so it costs one
    // bit per record after the first.
    c.RawKeyed(kU16Spec, f.fill, prev.fill, absolute);
    c.Quat(s.ori, f.hmdOri, prev.hmdOri, absolute);
    c.Quat(s.lHandOri, f.lHandOri, prev.lHandOri, absolute);
    c.Quat(s.rHandOri, f.rHandOri, prev.rHandOri, absolute);
    c.Vec3(s.refVel, f.refVel, prev.refVel, absolute);
    c.RawKeyed(s.bits28, f.bits28, prev.bits28, absolute);
    c.RawKeyed(s.holdSlot, f.holdLSlot, prev.holdLSlot, absolute);
    c.RawKeyed(s.holdGen, f.holdLGen, prev.holdLGen, absolute);
    c.RawKeyed(s.holdSlot, f.holdRSlot, prev.holdRSlot, absolute);
    c.RawKeyed(s.holdGen, f.holdRGen, prev.holdRGen, absolute);
    c.Scalar(s.bend, f.bendBack, prev.bendBack, absolute);
    c.Scalar(s.bend, f.bendFwd, prev.bendFwd, absolute);
    c.Scalar(s.restPose, f.restPoseHeight, prev.restPoseHeight, absolute);
    c.Vec3(s.handOff, f.lHandOff, prev.lHandOff, absolute);
    c.Vec3(s.handOff, f.rHandOff, prev.rHandOff, absolute);
    c.Scalar(s.trigger, f.trigL, prev.trigL, absolute);
    c.Scalar(s.trigger, f.trigR, prev.trigR, absolute);
    c.RawKeyed(s.anchor, f.anchor, prev.anchor, absolute);
    // The wire carries refvelocity a second time, and precedes the record with a header word. A sampled
    // record has neither, and inventing them would put values in the file that nothing measured.
    if (!sampled) {
        c.Vec3(s.refVel, f.refVelDup, prev.refVelDup, absolute);
        c.U64Keyed(f.hdr0, prev.hdr0, absolute);
    }
}

template <class C>
void WalkBody(C& c, const BodySpecs& s, BodyFrame& f, const BodyFrame& prev, bool absolute,
              bool sampled) {
    // Leads, like the player row's mask, so a decoder knows whether the net id below is an identity
    // or a placeholder before it reads it.
    c.RawKeyed(kU16Spec, f.fill, prev.fill, absolute);
    c.RawKeyed(s.bits11, f.bits11, prev.bits11, absolute);
    c.U64Keyed(f.idValue, prev.idValue, absolute);
    // The tag rides beside the value it explains. Delta-coded like everything else, so it costs a bit
    // once it settles -- and separating them in the record would recreate the very hazard of a value
    // whose meaning went missing.
    c.RawKeyed(kU32Spec, f.idTag, prev.idTag, absolute);
    c.Vec3(s.com, f.com, prev.com, absolute);
    c.Quat(s.ori, f.ori, prev.ori, absolute);
    c.Vec3(s.vel, f.vel, prev.vel, absolute);
    c.Vec3(s.angVel, f.angVel, prev.angVel, absolute);
    c.Vec3(s.lastHeldCom, f.lastHeldCom, prev.lastHeldCom, absolute);
    if (!sampled) {
        c.U64Keyed(f.hdr0, prev.hdr0, absolute);
        c.U64Keyed(f.hdr2, prev.hdr2, absolute);
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Schemas
// ---------------------------------------------------------------------------------------------
namespace {

// One FieldDef row per logical field, in walk order. The manifest carries this so a reader that meets
// an unknown schema can say what it does not understand instead of guessing (S5.9).
struct SchemaStore {
    FieldSpec player[21];
    FieldSpec playerSampled[19];
    FieldSpec body[11];
    FieldSpec bodySampled[9];
    FieldSpec netvar[5];
    FieldSpec event[4];
    FieldSpec tick[7];
    FieldSpec asSeen[11];
    FieldSpec raw[1];
    SchemaDef defs[kSchemaCount];
    uint16_t defCount;
    const char* refusal;
};

SchemaStore& Store() {
    static SchemaStore s = [] {
        SchemaStore t{};
        const PlayerSpecs pw = MakePlayerSpecs(false);
        const PlayerSpecs ps = MakePlayerSpecs(true);
        const BodySpecs bw = MakeBodySpecs(false);
        const BodySpecs bs = MakeBodySpecs(true);

        auto fillPlayer = [](FieldSpec* out, const PlayerSpecs& p, bool sampled) {
            int n = 0;
            out[n++] = kFillField;
            out[n++] = p.ori;
            out[n++] = p.lHandOri;
            out[n++] = p.rHandOri;
            out[n++] = p.refVel;
            out[n++] = p.bits28;
            out[n++] = p.holdSlot;
            out[n++] = p.holdGen;
            out[n++] = p.holdSlot;
            out[n++] = p.holdGen;
            out[n++] = p.bend;
            out[n++] = p.bend;
            out[n++] = p.restPose;
            out[n++] = p.handOff;
            out[n++] = p.handOff;
            out[n++] = p.trigger;
            out[n++] = p.trigger;
            out[n++] = p.anchor;
            if (!sampled) {
                out[n++] = p.refVel;
                out[n++] = kU64Field;   // the message header word, stored not decoded
            }
            return n;
        };
        auto fillBody = [](FieldSpec* out, const BodySpecs& b, bool sampled) {
            int n = 0;
            out[n++] = kFillField;
            out[n++] = b.bits11;
            out[n++] = b.netId;
            out[n++] = kU32Field;   // the identity tag beside the identity value
            out[n++] = b.com;
            out[n++] = b.ori;
            out[n++] = b.vel;
            out[n++] = b.angVel;
            out[n++] = b.lastHeldCom;
            if (!sampled) {
                out[n++] = kU64Field;
                out[n++] = kU64Field;
            }
            return n;
        };

        const int npw = fillPlayer(t.player, pw, false);
        const int nps = fillPlayer(t.playerSampled, ps, true);
        const int nbw = fillBody(t.body, bw, false);
        const int nbs = fillBody(t.bodySampled, bs, true);

        t.netvar[0] = RawSpec(40, 16);  // slot
        t.netvar[1] = RawSpec(41, 16);  // gen
        t.netvar[2] = RawSpec(42, 64);  // name
        t.netvar[3] = RawSpec(43, 64);  // raw SValue, never re-encoded
        // ★ The field count is the COMPATIBILITY GATE. A file records its own schema table, so a
        // capture written before this field declares four and a reader must decode it as four --
        // see playback's NetvarStampsPresent. Adding the field at the END is what keeps the first
        // four meaning what they meant.
        t.netvar[4] = RawSpec(44, 64);  // source time, the axis chapter spans are on
        t.event[0] = RawSpec(50, 64);   // stamp
        t.event[1] = RawSpec(51, 64);   // dst
        t.event[2] = RawSpec(52, 16);   // type index into the manifest tap table
        t.event[3] = RawSpec(53, 8);    // body byte
        t.tick[0] = RawSpec(60, 64);    // source time
        t.tick[1] = RawSpec(61, 64);    // sim time
        t.tick[2] = RawSpec(62, 64);    // peer time
        t.tick[3] = RawSpec(63, 32);    // frame index
        t.tick[4] = RawSpec(64, 32);    // scaled delta
        t.tick[5] = RawSpec(65, 32);    // frame delta
        t.tick[6] = RawSpec(66, 32);    // as-seen valid mask
        t.asSeen[0] = RawSpec(70, 64);
        t.asSeen[1] = RawSpec(71, 32);
        t.asSeen[2] = RawSpec(72, 64);
        t.asSeen[3] = RawSpec(73, 64);
        t.asSeen[4] = RawSpec(74, 64);
        t.asSeen[5] = F32Spec(75);
        t.asSeen[6] = RawSpec(76, 32);
        t.asSeen[7] = RawSpec(77, 32);
        t.asSeen[8] = RawSpec(78, 32);
        t.asSeen[9] = RawSpec(79, 32);
        t.asSeen[10] = RawSpec(80, 16);
        t.raw[0] = RawSpec(90, 8);

        auto add = [&](SchemaId id, bool required, uint16_t n, const FieldSpec* f) {
            t.defs[t.defCount].id = id;
            t.defs[t.defCount].retention = Retention::All;
            t.defs[t.defCount].required = required;
            t.defs[t.defCount].fieldCount = n;
            t.defs[t.defCount].fields = f;
            ++t.defCount;
        };
        add(kSchemaTick, true, 7, t.tick);
        add(kSchemaPlayerWire, true, static_cast<uint16_t>(npw), t.player);
        add(kSchemaPlayerSampled, true, static_cast<uint16_t>(nps), t.playerSampled);
        add(kSchemaBodyWire, true, static_cast<uint16_t>(nbw), t.body);
        add(kSchemaBodySampled, true, static_cast<uint16_t>(nbs), t.bodySampled);
        add(kSchemaNetvar, true, 5, t.netvar);
        add(kSchemaEvent, true, 4, t.event);
        add(kSchemaKey, true, 1, t.raw);
        add(kSchemaMark, false, 1, t.raw);
        add(kSchemaAsSeen, false, 11, t.asSeen);
        add(kSchemaVoip, false, 1, t.raw);
        add(kSchemaOracle, false, 1, t.raw);
        add(kSchemaGap, false, 1, t.raw);
        add(kSchemaDegraded, false, 1, t.raw);
        add(kSchemaManifest, true, 1, t.raw);
        add(kSchemaIndex, true, 1, t.raw);

        // The table checks itself at construction: an unpinned field, a bad width, a WireQ field that
        // survived the sampled substitution, or a retention that is not All refuses here rather than
        // producing a file nothing can read.
        for (uint16_t i = 0; i < t.defCount && !t.refusal; ++i) {
            const SchemaDef& d = t.defs[i];
            if (d.retention != Retention::All) {
                t.refusal = "a schema declares a retention other than ALL";
                break;
            }
            const bool sampled = d.id == kSchemaPlayerSampled || d.id == kSchemaBodySampled;
            for (uint16_t k = 0; k < d.fieldCount; ++k) {
                if (const char* why = bitio::FieldRefusal(d.fields[k])) {
                    t.refusal = why;
                    break;
                }
                if (sampled && (d.fields[k].cls == FieldClass::WireQ ||
                                d.fields[k].cls == FieldClass::WireQuat)) {
                    t.refusal = "a sampled stream declares a WIRE_Q field";
                    break;
                }
            }
        }
        return t;
    }();
    return s;
}

}  // namespace

const SchemaDef* Schemas() { return Store().defs; }
uint16_t SchemaCount() { return Store().defCount; }

const SchemaDef* SchemaById(SchemaId id) {
    const SchemaStore& s = Store();
    for (uint16_t i = 0; i < s.defCount; ++i) {
        if (s.defs[i].id == id) return &s.defs[i];
    }
    return nullptr;
}

const char* SchemaTableRefusal() { return Store().refusal; }

// ---------------------------------------------------------------------------------------------
// Player
// ---------------------------------------------------------------------------------------------
namespace {

// The entity id and the stamp, shared by both entity streams. `absolute` is the answer to "has this
// entity appeared in this chapter yet", which is what makes a chapter independently decodable.
//
// The entity is coded BEFORE the stamp so that both sides can index the same per-entity predecessor:
// the reader does not know which entity's stamp it is decoding until it has read the id, and taking
// the array here rather than a single value is what keeps one listing serving both directions.
template <class C>
void WalkEntityHead(C& c, uint16_t& entity, uint16_t lastEntity, uint64_t& stamp,
                    const uint64_t* prevStamps, uint16_t mod, bool& absolute) {
    bool same = entity == lastEntity;
    c.Flag(same);
    if (!same) {
        c.Raw16(kU16Spec, entity);
    } else if (!C::kWriting) {
        entity = lastEntity;
    }
    c.Flag(absolute);
    c.VarKeyed(stamp, prevStamps[entity % mod], absolute);
}

}  // namespace

void PlayerEncoder::Reset() {
    memset(seen_, 0, sizeof seen_);
    memset(prev_, 0, sizeof prev_);
    memset(prevStamp_, 0, sizeof prevStamp_);
    lastEntity_ = 0;
}

bool PlayerEncoder::Encode(BitWriter& w, uint16_t entity, uint64_t stampUs, const PlayerFrame& f) {
    const uint16_t slot = entity % kMaxEntities;
    bool absolute = !seen_[slot];
    WriteCodec c{w};
    c.FlagOut(false);   // not verbatim
    uint16_t e = entity;
    uint64_t s = stampUs;
    WalkEntityHead(c, e, lastEntity_, s, prevStamp_, kMaxEntities, absolute);

    static const PlayerSpecs kWire = MakePlayerSpecs(false);
    static const PlayerSpecs kSampled = MakePlayerSpecs(true);
    PlayerFrame local = f;
    WalkPlayer(c, sampled_ ? kSampled : kWire, local, prev_[slot], absolute, sampled_);
    if (w.Refused()) {
        return false;
    }
    prev_[slot] = f;
    prevStamp_[slot] = stampUs;
    seen_[slot] = true;
    lastEntity_ = entity;
    return true;
}

bool PlayerEncoder::EncodeVerbatim(BitWriter& w, uint16_t entity, uint64_t stampUs, const void* body,
                                   uint16_t bodyBytes) {
    const uint16_t slot = entity % kMaxEntities;
    WriteCodec c{w};
    c.FlagOut(true);
    uint16_t e = entity;
    uint64_t s = stampUs;
    bool absolute = !seen_[slot];
    WalkEntityHead(c, e, lastEntity_, s, prevStamp_, kMaxEntities, absolute);
    if (!EncodeRawRecord(w, body, bodyBytes)) {
        return false;
    }
    // prev_ is deliberately NOT updated. There is no proved frame here, and deltaing the next record
    // against one we could not verify would spread a single unexplained record into all of them.
    //
    // ★ Which means the NEXT record for this entity must be absolute, so `seen_` is CLEARED rather than
    // set. The suite caught the alternative: leaving it set made the next record delta against a
    // zero-initialised frame, whose all-zero quaternion is not on the unit sphere, and the encoder
    // refused the record. A refusal is the codec being right -- but the record would have been lost.
    prevStamp_[slot] = stampUs;
    seen_[slot] = false;
    lastEntity_ = entity;
    return true;
}

void PlayerDecoder::Reset() {
    memset(seen_, 0, sizeof seen_);
    memset(prev_, 0, sizeof prev_);
    memset(prevStamp_, 0, sizeof prevStamp_);
    lastEntity_ = 0;
}

bool PlayerDecoder::Decode(BitReader& r, DecodedPlayer& out, uint8_t* bodyBuf, size_t bodyCap) {
    ReadCodec c{r};
    out = DecodedPlayer{};
    c.Flag(out.verbatim);
    uint16_t e = 0;
    uint64_t s = 0;
    bool absolute = false;
    WalkEntityHead(c, e, lastEntity_, s, prevStamp_, kMaxEntities, absolute);
    const uint16_t slot = e % kMaxEntities;

    if (out.verbatim) {
        if (!DecodeRawRecord(r, bodyBuf, bodyCap, out.bodyBytes)) {
            return false;
        }
        out.body = bodyBuf;
        prevStamp_[slot] = s;
        seen_[slot] = true;
        lastEntity_ = e;
        out.entity = e;
        out.stampUs = s;
        return true;
    }

    static const PlayerSpecs kWire = MakePlayerSpecs(false);
    static const PlayerSpecs kSampled = MakePlayerSpecs(true);
    out.frame = absolute ? PlayerFrame{} : prev_[slot];
    WalkPlayer(c, sampled_ ? kSampled : kWire, out.frame, prev_[slot], absolute, sampled_);
    out.frame.hasDup = !sampled_;
    if (c.Bad()) {
        return false;
    }
    prev_[slot] = out.frame;
    prevStamp_[slot] = s;
    seen_[slot] = true;
    lastEntity_ = e;
    out.entity = e;
    out.stampUs = s;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Body
// ---------------------------------------------------------------------------------------------

void BodyEncoder::Reset() {
    memset(seen_, 0, sizeof seen_);
    memset(prev_, 0, sizeof prev_);
    memset(prevStamp_, 0, sizeof prevStamp_);
    lastEntity_ = 0;
}

bool BodyEncoder::Encode(BitWriter& w, uint16_t entity, uint64_t stampUs, const BodyFrame& f) {
    const uint16_t slot = entity % kMaxBodies;
    bool absolute = !seen_[slot];
    lastRefusal_ = RefusedField{};
    WriteCodec c{w, &lastRefusal_};
    c.FlagOut(false);
    uint16_t e = entity;
    uint64_t s = stampUs;
    WalkEntityHead(c, e, lastEntity_, s, prevStamp_, kMaxBodies, absolute);

    static const BodySpecs kWire = MakeBodySpecs(false);
    static const BodySpecs kSampled = MakeBodySpecs(true);
    BodyFrame local = f;
    WalkBody(c, sampled_ ? kSampled : kWire, local, prev_[slot], absolute, sampled_);
    if (w.Refused()) {
        return false;
    }
    prev_[slot] = f;
    prevStamp_[slot] = stampUs;
    seen_[slot] = true;
    lastEntity_ = entity;
    return true;
}

bool BodyEncoder::EncodeVerbatim(BitWriter& w, uint16_t entity, uint64_t stampUs, const void* body,
                                 uint16_t bodyBytes) {
    const uint16_t slot = entity % kMaxBodies;
    WriteCodec c{w};
    c.FlagOut(true);
    uint16_t e = entity;
    uint64_t s = stampUs;
    bool absolute = !seen_[slot];
    WalkEntityHead(c, e, lastEntity_, s, prevStamp_, kMaxBodies, absolute);
    if (!EncodeRawRecord(w, body, bodyBytes)) {
        return false;
    }
    // See PlayerEncoder::EncodeVerbatim: the next record for this entity has to be absolute, because
    // there is no verified predecessor to delta it against.
    prevStamp_[slot] = stampUs;
    seen_[slot] = false;
    lastEntity_ = entity;
    return true;
}

void BodyDecoder::Reset() {
    memset(seen_, 0, sizeof seen_);
    memset(prev_, 0, sizeof prev_);
    memset(prevStamp_, 0, sizeof prevStamp_);
    lastEntity_ = 0;
}

bool BodyDecoder::Decode(BitReader& r, DecodedBody& out, uint8_t* bodyBuf, size_t bodyCap) {
    ReadCodec c{r};
    out = DecodedBody{};
    c.Flag(out.verbatim);
    uint16_t e = 0;
    uint64_t s = 0;
    bool absolute = false;
    WalkEntityHead(c, e, lastEntity_, s, prevStamp_, kMaxBodies, absolute);
    const uint16_t slot = e % kMaxBodies;

    if (out.verbatim) {
        if (!DecodeRawRecord(r, bodyBuf, bodyCap, out.bodyBytes)) {
            return false;
        }
        out.body = bodyBuf;
        prevStamp_[slot] = s;
        seen_[slot] = true;
        lastEntity_ = e;
        out.entity = e;
        out.stampUs = s;
        return true;
    }

    static const BodySpecs kWire = MakeBodySpecs(false);
    static const BodySpecs kSampled = MakeBodySpecs(true);
    out.frame = absolute ? BodyFrame{} : prev_[slot];
    WalkBody(c, sampled_ ? kSampled : kWire, out.frame, prev_[slot], absolute, sampled_);
    if (c.Bad()) {
        return false;
    }
    out.frame.hasHeader = !sampled_;
    prev_[slot] = out.frame;
    prevStamp_[slot] = s;
    seen_[slot] = true;
    lastEntity_ = e;
    out.entity = e;
    out.stampUs = s;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------------------------
namespace {

template <class C>
void WalkTick(C& c, TickRec& t, const TickRec& prev, const TickRec& delta, bool absolute) {
    uint64_t src = t.sourceTimeUs, sim = t.simTimeUs, peer = t.peerTimeUs;
    c.VarPredicted(src, prev.sourceTimeUs, delta.sourceTimeUs, absolute);
    c.VarPredicted(sim, prev.simTimeUs, delta.simTimeUs, absolute);
    c.VarPredicted(peer, prev.peerTimeUs, delta.peerTimeUs, absolute);
    t.sourceTimeUs = src;
    t.simTimeUs = sim;
    t.peerTimeUs = peer;

    uint64_t fi = t.frameIndex;
    c.VarPredicted(fi, prev.frameIndex, delta.frameIndex, absolute);
    t.frameIndex = static_cast<uint32_t>(fi);

    c.RawKeyed(kU32Spec, t.scaledDeltaUs, prev.scaledDeltaUs, absolute);
    c.RawKeyed(kU32Spec, t.frameDeltaUs, prev.frameDeltaUs, absolute);
    c.RawKeyed(kU32Spec, t.asSeenValid, prev.asSeenValid, absolute);
}

}  // namespace

void TickEncoder::Reset() {
    started_ = false;
    prev_ = TickRec{};
    delta_ = TickRec{};
}

bool TickEncoder::Encode(BitWriter& w, const TickRec& t) {
    WriteCodec c{w};
    TickRec local = t;
    WalkTick(c, local, prev_, delta_, !started_);
    if (w.Refused()) {
        return false;
    }
    if (started_) {
        delta_.sourceTimeUs = t.sourceTimeUs - prev_.sourceTimeUs;
        delta_.simTimeUs = t.simTimeUs - prev_.simTimeUs;
        delta_.peerTimeUs = t.peerTimeUs - prev_.peerTimeUs;
        delta_.frameIndex = t.frameIndex - prev_.frameIndex;
    }
    prev_ = t;
    started_ = true;
    return true;
}

void TickDecoder::Reset() {
    started_ = false;
    prev_ = TickRec{};
    delta_ = TickRec{};
}

bool TickDecoder::Decode(BitReader& r, TickRec& out) {
    ReadCodec c{r};
    out = TickRec{};
    WalkTick(c, out, prev_, delta_, !started_);
    if (c.Bad()) {
        return false;
    }
    if (started_) {
        delta_.sourceTimeUs = out.sourceTimeUs - prev_.sourceTimeUs;
        delta_.simTimeUs = out.simTimeUs - prev_.simTimeUs;
        delta_.peerTimeUs = out.peerTimeUs - prev_.peerTimeUs;
        delta_.frameIndex = out.frameIndex - prev_.frameIndex;
    }
    prev_ = out;
    started_ = true;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Netvar
// ---------------------------------------------------------------------------------------------
//
// The name dictionary is a coding, not a retention policy: every row of every message is written. It
// exists because 79 distinct CSymbol64s repeat across thousands of messages and a 7-bit index is 57
// bits cheaper than the qword. It resets at each chapter, so a chapter still decodes on its own.

namespace {

int DictFind(const NetvarKnown* d, uint16_t count, uint64_t name, uint16_t slot) {
    for (uint16_t i = 0; i < count; ++i) {
        if (d[i].name == name && d[i].slot == slot) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

void NetvarEncoder::Reset() {
    started_ = false;
    owner_ = 0;
    prevStamp_ = 0;
    prevSeq_ = 0;
    prevSlot_ = 0;
    dictCount_ = 0;
    memset(dict_, 0, sizeof dict_);
}

bool NetvarEncoder::Encode(BitWriter& w, const NetvarMessage& m) {
    if (m.varCount > NetvarMessage::kMaxRows) {
        return false;
    }
    WriteCodec c{w};
    const bool absolute = !started_;

    uint64_t owner = m.owner;
    c.U64Keyed(owner, owner_, absolute);

    uint16_t slot = m.slot;
    bool sameSlot = !absolute && slot == prevSlot_;
    c.FlagOut(sameSlot);
    if (!sameSlot) c.Raw16(kU16Spec, slot);

    uint16_t gen = m.gen;
    c.Raw16(kU16Spec, gen);

    uint64_t seq = m.pktSeq;
    c.VarKeyed(seq, prevSeq_, absolute);

    // Delta-keyed against the previous message exactly as pkt_seq above is: the axis is
    // non-decreasing and the tap runs a little faster than the tick, so most deltas are one tick or
    // zero and the class selector spends 10-18 bits on them rather than 64.
    uint64_t stamp = m.stampUs;
    c.VarKeyed(stamp, prevStamp_, absolute);

    c.FlagOut(m.hasRowset);
    if (m.hasRowset) {
        // rowset_seq equalled pkt_seq in all 14,794 real messages that carry both; when it does not,
        // the escape stores it in full. Nothing is inferred either way.
        const bool same = m.rowsetSeq == m.pktSeq;
        c.FlagOut(same);
        if (!same) {
            uint32_t rs = m.rowsetSeq;
            c.Raw32(kU32Spec, rs);
        }
        uint16_t n = m.varCount;
        c.Raw16(kU16Spec, n);
        for (uint16_t i = 0; i < m.varCount; ++i) {
            const int at = DictFind(dict_, dictCount_, m.rows[i].name, m.rows[i].slot);
            const bool known = at >= 0;
            c.FlagOut(known);
            uint64_t value = m.rows[i].value;
            if (known) {
                uint64_t idx = static_cast<uint64_t>(at);
                c.Raw(kDict7Spec, idx);
                // ★ The two clock ramps are f32 bit patterns that walk almost linearly in INTEGER
                // space within one exponent, so an arithmetic delta beats a trimmed XOR on them and
                // loses on a CSymbol64 that changes wholesale. One selector bit picks per row, which
                // is what lets every sample of a 118 Hz ramp be stored rather than decimated (S5.6.1).
                const uint64_t prevValue = dict_[at].value;
                const uint64_t x = value ^ prevValue;
                const uint64_t zz = ZigZag(static_cast<int64_t>(value) -
                                           static_cast<int64_t>(prevValue));
                const unsigned xorCost =
                    x == 0 ? 1u : 1u + 6u + 6u + (64u - Clz64(x) - Ctz64(x));
                const unsigned varCost = 2u + kVarClassBits[PickVarClass(zz)];
                const bool useVar = varCost < xorCost;
                c.FlagOut(useVar);
                if (useVar) {
                    c.VarKeyed(value, prevValue, false);
                } else {
                    c.U64Keyed(value, prevValue, false);
                }
                dict_[at].value = m.rows[i].value;
            } else {
                uint64_t name = m.rows[i].name;
                c.Raw(kU64Spec, name);
                uint16_t rowSlot = m.rows[i].slot;
                c.Raw16(kU16Spec, rowSlot);
                c.Raw(kU64Spec, value);
                if (dictCount_ < kNameDictMax) {
                    dict_[dictCount_].name = m.rows[i].name;
                    dict_[dictCount_].slot = m.rows[i].slot;
                    dict_[dictCount_].value = m.rows[i].value;
                    ++dictCount_;
                }
            }
        }
    }
    if (w.Refused()) {
        return false;
    }
    owner_ = m.owner;
    prevSlot_ = m.slot;
    prevSeq_ = m.pktSeq;
    prevStamp_ = m.stampUs;
    started_ = true;
    return true;
}

void NetvarDecoder::Reset() {
    started_ = false;
    owner_ = 0;
    prevStamp_ = 0;
    prevSeq_ = 0;
    prevSlot_ = 0;
    dictCount_ = 0;
    memset(dict_, 0, sizeof dict_);
    // stampPresent_ is not cleared: it describes the FILE, and Reset runs per block within one.
}

bool NetvarDecoder::Decode(BitReader& r, NetvarMessage& out) {
    ReadCodec c{r};
    const bool absolute = !started_;
    out = NetvarMessage{};

    c.U64Keyed(out.owner, owner_, absolute);

    bool sameSlot = false;
    c.Flag(sameSlot);
    if (sameSlot && !absolute) {
        out.slot = prevSlot_;
    } else {
        c.Raw16(kU16Spec, out.slot);
    }
    c.Raw16(kU16Spec, out.gen);

    uint64_t seq = 0;
    c.VarKeyed(seq, prevSeq_, absolute);
    out.pktSeq = static_cast<uint32_t>(seq);

    if (stampPresent_) {
        c.VarKeyed(out.stampUs, prevStamp_, absolute);
    }

    c.Flag(out.hasRowset);
    if (out.hasRowset) {
        bool same = false;
        c.Flag(same);
        if (same) {
            out.rowsetSeq = out.pktSeq;
        } else {
            c.Raw32(kU32Spec, out.rowsetSeq);
        }
        c.Raw16(kU16Spec, out.varCount);
        if (out.varCount > NetvarMessage::kMaxRows) {
            return false;
        }
        for (uint16_t i = 0; i < out.varCount; ++i) {
            bool known = false;
            c.Flag(known);
            if (known) {
                uint64_t idxRaw = 0;
                c.Raw(kDict7Spec, idxRaw);
                const uint16_t idx = static_cast<uint16_t>(idxRaw);
                if (idx >= dictCount_) {
                    return false;
                }
                out.rows[i].name = dict_[idx].name;
                out.rows[i].slot = dict_[idx].slot;
                bool useVar = false;
                c.Flag(useVar);
                if (useVar) {
                    c.VarKeyed(out.rows[i].value, dict_[idx].value, false);
                } else {
                    c.U64Keyed(out.rows[i].value, dict_[idx].value, false);
                }
                dict_[idx].value = out.rows[i].value;
            } else {
                c.Raw(kU64Spec, out.rows[i].name);
                c.Raw16(kU16Spec, out.rows[i].slot);
                c.Raw(kU64Spec, out.rows[i].value);
                if (dictCount_ < kNameDictMax) {
                    dict_[dictCount_].name = out.rows[i].name;
                    dict_[dictCount_].slot = out.rows[i].slot;
                    dict_[dictCount_].value = out.rows[i].value;
                    ++dictCount_;
                }
            }
        }
    }
    if (c.Bad()) {
        return false;
    }
    owner_ = out.owner;
    prevSlot_ = out.slot;
    prevSeq_ = out.pktSeq;
    prevStamp_ = out.stampUs;
    started_ = true;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Event
// ---------------------------------------------------------------------------------------------

void EventEncoder::Reset() {
    started_ = false;
    prevStamp_ = 0;
    prevDst_ = 0;
}

bool EventEncoder::Encode(BitWriter& w, const EventMessage& e) {
#ifndef W2A2_MUTATE_BODY_BOUND_UNCHECKED
    // Refused before the FIRST bit is written -- ahead of the stamp and the destination, not merely
    // ahead of the body -- so a body past the format's bound cannot leave half a record in a stream
    // no decoder can walk past.  The producer's own stage is sized from the same
    // constant, so this can only fire on a caller that invented a bigger record.
    if (e.bodyBytes > kMaxEventBodyBytes) {
        return false;
    }
#endif
    WriteCodec c{w};
    const bool absolute = !started_;
    uint64_t stamp = e.stampUs, dst = e.dst;
    c.VarKeyed(stamp, prevStamp_, absolute);
    c.U64Keyed(dst, prevDst_, absolute);
    uint16_t type = e.typeIndex, n = e.bodyBytes;
    c.Raw16(kU16Spec, type);
    c.Raw16(kU16Spec, n);
    c.Bytes(e.body, nullptr, e.bodyBytes);
    if (w.Refused()) {
        return false;
    }
    prevStamp_ = e.stampUs;
    prevDst_ = e.dst;
    started_ = true;
    return true;
}

void EventDecoder::Reset() {
    started_ = false;
    prevStamp_ = 0;
    prevDst_ = 0;
}

bool EventDecoder::Decode(BitReader& r, EventMessage& out, uint8_t* bodyBuf, size_t bodyCap) {
    ReadCodec c{r};
    const bool absolute = !started_;
    uint64_t stamp = 0, dst = 0;
    c.VarKeyed(stamp, prevStamp_, absolute);
    c.U64Keyed(dst, prevDst_, absolute);
    c.Raw16(kU16Spec, out.typeIndex);
    c.Raw16(kU16Spec, out.bodyBytes);
    if (c.Bad()) {
        return false;
    }
    if (out.bodyBytes > kMaxEventBodyBytes) {
        ++overBound_;
        return false;
    }
    if (out.bodyBytes > bodyCap) {
        // CONSUME it anyway and report: the caller loses this record, the stream keeps its boundary.
        ++tooSmall_;
        c.Bytes(nullptr, skip_, out.bodyBytes);
        if (c.Bad()) {
            return false;
        }
        out.body = nullptr;
        out.stampUs = stamp;
        out.dst = dst;
        prevStamp_ = stamp;
        prevDst_ = dst;
        started_ = true;
        return false;
    }
    c.Bytes(nullptr, bodyBuf, out.bodyBytes);
    if (c.Bad()) {
        return false;
    }
    out.stampUs = stamp;
    out.dst = dst;
    out.body = bodyBuf;
    prevStamp_ = stamp;
    prevDst_ = dst;
    started_ = true;
    return true;
}

// ---------------------------------------------------------------------------------------------
// As-seen
// ---------------------------------------------------------------------------------------------
namespace {

template <class C>
void WalkAsSeen(C& c, AsSeenFrame& f, const AsSeenFrame& prev, const AsSeenFrame& delta,
                bool absolute) {
    uint64_t src = f.sourceTimeUs;
    c.VarPredicted(src, prev.sourceTimeUs, delta.sourceTimeUs, absolute);
    f.sourceTimeUs = src;
    uint64_t fi = f.frameIndex;
    c.VarPredicted(fi, prev.frameIndex, delta.frameIndex, absolute);
    f.frameIndex = static_cast<uint32_t>(fi);

    // ★ Clocks are delta-coded ARITHMETICALLY and against a PREDICTION, never by XOR. A value that
    // advances by ~11,111 us a frame has a tiny arithmetic difference and a large XOR -- crossing a
    // binary carry flips a long run of high bits, so a trimmed XOR is near full width exactly when the
    // clock is behaving normally. The measurements are on VarPredicted above.
    c.VarPredicted(f.sample.peerTimeUs, prev.sample.peerTimeUs, delta.sample.peerTimeUs, absolute);
    c.VarPredicted(f.sample.serverTimeEstUs, prev.sample.serverTimeEstUs,
                   delta.sample.serverTimeEstUs, absolute);
    c.VarPredicted(f.sample.serverTimeSlew, prev.sample.serverTimeSlew, delta.sample.serverTimeSlew,
                   absolute);

    static const FieldSpec kDelay = F32Spec(75);
    c.Scalar(kDelay, f.sample.renderDelaySec, prev.sample.renderDelaySec, absolute);

    c.RawKeyed(kU32Spec, f.sample.lodThrottleEnable, prev.sample.lodThrottleEnable, absolute);
    c.RawKeyed(kU32Spec, f.sample.lodPerFrameBudget, prev.sample.lodPerFrameBudget, absolute);
    c.RawKeyed(kU32Spec, f.sample.lodCursor, prev.sample.lodCursor, absolute);
    c.RawKeyed(kU32Spec, f.sample.valid, prev.sample.valid, absolute);

    c.Raw16(kU16Spec, f.playerCount);
    const uint16_t n = f.playerCount < AsSeenFrame::kMaxPlayers ? f.playerCount
                                                               : AsSeenFrame::kMaxPlayers;
    for (uint16_t i = 0; i < n; ++i) {
        const AsSeenPerPlayer& p = prev.players[i];
        c.VarPredicted(f.players[i].peerTimeUs, p.peerTimeUs, delta.players[i].peerTimeUs, absolute);
        uint32_t rtt = f.players[i].rttMs;
        c.RawKeyed(kU16Spec, rtt, p.rttMs, absolute);
        f.players[i].rttMs = static_cast<uint16_t>(rtt);
        uint32_t flags = (f.players[i].occluded ? 1u : 0u) | (f.players[i].bodyUpdatedThisFrame ? 2u : 0u);
        const uint32_t prevFlags = (p.occluded ? 1u : 0u) | (p.bodyUpdatedThisFrame ? 2u : 0u);
        static const FieldSpec kFlags = RawSpec(81, 2);
        c.RawKeyed(kFlags, flags, prevFlags, absolute);
        f.players[i].occluded = (flags & 1u) != 0;
        f.players[i].bodyUpdatedThisFrame = (flags & 2u) != 0;
    }
}

}  // namespace

void AsSeenEncoder::Reset() {
    started_ = false;
    prev_ = AsSeenFrame{};
    delta_ = AsSeenFrame{};
}

namespace {
void UpdateAsSeenDelta(AsSeenFrame& delta, const AsSeenFrame& cur, const AsSeenFrame& prev) {
    delta.sourceTimeUs = cur.sourceTimeUs - prev.sourceTimeUs;
    delta.frameIndex = cur.frameIndex - prev.frameIndex;
    delta.sample.peerTimeUs = cur.sample.peerTimeUs - prev.sample.peerTimeUs;
    delta.sample.serverTimeEstUs = cur.sample.serverTimeEstUs - prev.sample.serverTimeEstUs;
    delta.sample.serverTimeSlew = cur.sample.serverTimeSlew - prev.sample.serverTimeSlew;
    for (uint16_t i = 0; i < AsSeenFrame::kMaxPlayers; ++i) {
        delta.players[i].peerTimeUs = cur.players[i].peerTimeUs - prev.players[i].peerTimeUs;
    }
}
}  // namespace

bool AsSeenEncoder::Encode(BitWriter& w, const AsSeenFrame& f) {
    if (f.playerCount > AsSeenFrame::kMaxPlayers) {
        return false;
    }
    WriteCodec c{w};
    AsSeenFrame local = f;
    WalkAsSeen(c, local, prev_, delta_, !started_);
    if (w.Refused()) {
        return false;
    }
    if (started_) {
        UpdateAsSeenDelta(delta_, f, prev_);
    }
    prev_ = f;
    started_ = true;
    return true;
}

void AsSeenDecoder::Reset() {
    started_ = false;
    prev_ = AsSeenFrame{};
    delta_ = AsSeenFrame{};
}

bool AsSeenDecoder::Decode(BitReader& r, AsSeenFrame& out) {
    ReadCodec c{r};
    const bool absolute = !started_;
    out = absolute ? AsSeenFrame{} : prev_;
    WalkAsSeen(c, out, prev_, delta_, absolute);
    if (c.Bad() || out.playerCount > AsSeenFrame::kMaxPlayers) {
        return false;
    }
    if (started_) {
        UpdateAsSeenDelta(delta_, out, prev_);
    }
    prev_ = out;
    started_ = true;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Raw records
// ---------------------------------------------------------------------------------------------

bool EncodeRawRecord(BitWriter& w, const void* bytes, uint16_t n) {
    WriteCodec c{w};
    uint16_t len = n;
    c.Raw16(kU16Spec, len);
    c.Bytes(static_cast<const uint8_t*>(bytes), nullptr, n);
    return !w.Refused();
}

bool DecodeRawRecord(BitReader& r, uint8_t* out, size_t cap, uint16_t& n) {
    ReadCodec c{r};
    c.Raw16(kU16Spec, n);
    const size_t usable = cap > 0xFFFFu ? 0xFFFFu : cap;
    if (c.Bad() || n > usable) {
        return false;
    }
    c.Bytes(nullptr, out, n);
    return !c.Bad();
}

// ---------------------------------------------------------------------------------------------
// The wire path -- decode, then PROVE the decode
// ---------------------------------------------------------------------------------------------

const char* BodyFillName(uint32_t bit) {
    switch (bit) {
        case kFillBodyTransform: return "transform";
        case kFillBodyLastHeld: return "lastheldcom";
        case kFillBodyBits: return "bits11";
        case kFillBodyIdValue: return "id.value";
        case kFillBodyIdTag: return "id.tag";
        case kFillBodyIdNetActor: return "id.from-net-actor-table";
        case kFillBodyHeader: return "header";
        default: return "?";
    }
}

const char* PlayerFillName(uint32_t bit) {
    switch (bit) {
        case kFillPose: return "pose";
        case kFillRefVel: return "refvel";
        case kFillBody: return "body";
        case kFillTrigger: return "trigger";
        case kFillAnchor: return "anchor";
        case kFillActionWord: return "actionword";
        case kFillHolding: return "holding";
        case kFillHeader: return "header";
        case kFillRefVelDup: return "refvel.dup";
        default: return "?";
    }
}

const char* WireDecodeName(WireDecode d) {
    switch (d) {
        case WireDecode::Ok: return "ok";
        case WireDecode::TooShort: return "body too short for a record at the pinned offset";
        case WireDecode::ReadFailed: return "the engine mirror refused mid-record";
        case WireDecode::ReencodeDiffers: return "re-encoding did not reproduce the source body";
        case WireDecode::PaddingNonZero: return "the bits after the record are not zero padding";
    }
    return "unknown";
}

namespace {

using bitio::EngineBitReader;
using bitio::EngineBitWriter;
using bitio::WireStatus;

// A scratch buffer big enough for either record. 370 bits rounds to 6 qwords; the engine's writer emits
// whole qwords, so the comparison is over whole qwords too.
constexpr size_t kWireScratch = 64;

bool BitsEqual(const uint8_t* a, const uint8_t* b, size_t bits) {
    const size_t whole = bits / 8;
    if (whole && memcmp(a, b, whole) != 0) {
        return false;
    }
    const unsigned rest = static_cast<unsigned>(bits % 8);
    if (rest) {
        const uint8_t mask = static_cast<uint8_t>((1u << rest) - 1u);
        if ((a[whole] & mask) != (b[whole] & mask)) return false;
    }
    return true;
}

// Every bit of `body` from `fromBit` to the end must be zero. W1-A accepted a record offset only under
// this condition, and it is what stops a shifted decode looking like a clean one.
bool TailIsZero(const uint8_t* body, size_t bytes, size_t fromBit) {
    const size_t byteAt = fromBit / 8;
    const unsigned bitIn = static_cast<unsigned>(fromBit % 8);
    if (byteAt >= bytes) {
        return true;
    }
    if (bitIn) {
        const uint8_t mask = static_cast<uint8_t>(0xFFu << bitIn);
        if (body[byteAt] & mask) return false;
        for (size_t i = byteAt + 1; i < bytes; ++i) {
            if (body[i]) return false;
        }
        return true;
    }
    for (size_t i = byteAt; i < bytes; ++i) {
        if (body[i]) return false;
    }
    return true;
}

// The body's record starts at a whole byte on both messages (128 and 192 bits), which is what lets the
// mirror read from `body + start/8` instead of needing a bit-offset reader.
static_assert(kPlayerRecordStartBits % 8 == 0, "the player record starts on a byte");
static_assert(kBodyRecordStartBits % 8 == 0, "the body record starts on a byte");

}  // namespace

WireDecode DecodePlayerBody(const void* bodyPtr, size_t bytes, PlayerFrame& out) {
    const uint8_t* body = static_cast<const uint8_t*>(bodyPtr);
    const size_t at = kPlayerRecordStartBits / 8;
    if (!body || bytes <= at) {
        return WireDecode::TooShort;
    }

    const bitio::WireField* W = bitio::kPlayerStateWire;
    EngineBitReader r(body + at, bytes - at);
    out = PlayerFrame{};
    out.hasDup = true;
    // Every field below comes off the wire schedule, and the re-encode at the foot of this function
    // proves it reproduces the source bits. Nothing here is unsourced.
    out.fill = kFillWireAll;
    // The header word ahead of the record, kept whole. It is not decoded here and must not be: see
    // PlayerFrame::hdr0. [8:16) is the source stamp, which the record carries in its own right.
    memcpy(&out.hdr0, body, sizeof out.hdr0);

    auto q = [&](float dst[4], size_t i) { r.BitPackQuat(dst, W[i].bits); };
    auto v = [&](float dst[3], size_t i) { r.BitPackVec(dst, W[i].min, W[i].max, W[i].step); };
    auto s = [&](float& dst, size_t i) { r.BitPackReal(dst, W[i].min, W[i].max, W[i].step); };
    auto u = [&](uint32_t& dst, size_t i) {
        uint64_t t = 0;
        r.Read(t, W[i].bits);
        dst = static_cast<uint32_t>(t);
    };

    q(out.hmdOri, 0);
    q(out.lHandOri, 1);
    q(out.rHandOri, 2);
    v(out.refVel, 3);
    u(out.bits28, 4);
    u(out.holdLSlot, 5);
    u(out.holdLGen, 6);
    u(out.holdRSlot, 7);
    u(out.holdRGen, 8);
    s(out.bendBack, 9);
    s(out.bendFwd, 10);
    s(out.restPoseHeight, 11);
    v(out.lHandOff, 12);
    v(out.rHandOff, 13);
    s(out.trigL, 14);
    s(out.trigR, 15);
    u(out.anchor, 16);
    v(out.refVelDup, 17);
    if (r.Status() != WireStatus::Ok) {
        return WireDecode::ReadFailed;
    }
    const size_t bits = r.BitsRead();
    // Before the re-encode, so ReencodeDiffers still means a whole record at the pinned offset:
    // playback decodes such a body (RecoverWireVerbatim) and relies on that.
    if (!TailIsZero(body, bytes, kPlayerRecordStartBits + bits)) {
        return WireDecode::PaddingNonZero;
    }

    uint8_t back[kWireScratch] = {};
    EngineBitWriter w(back, sizeof back);
    w.BitPackQuat(out.hmdOri, W[0].bits);
    w.BitPackQuat(out.lHandOri, W[1].bits);
    w.BitPackQuat(out.rHandOri, W[2].bits);
    w.BitPackVec(out.refVel, W[3].min, W[3].max, W[3].step);
    w.Write(out.bits28, W[4].bits);
    w.Write(out.holdLSlot, W[5].bits);
    w.Write(out.holdLGen, W[6].bits);
    w.Write(out.holdRSlot, W[7].bits);
    w.Write(out.holdRGen, W[8].bits);
    w.BitPackReal(out.bendBack, W[9].min, W[9].max, W[9].step);
    w.BitPackReal(out.bendFwd, W[10].min, W[10].max, W[10].step);
    w.BitPackReal(out.restPoseHeight, W[11].min, W[11].max, W[11].step);
    w.BitPackVec(out.lHandOff, W[12].min, W[12].max, W[12].step);
    w.BitPackVec(out.rHandOff, W[13].min, W[13].max, W[13].step);
    w.BitPackReal(out.trigL, W[14].min, W[14].max, W[14].step);
    w.BitPackReal(out.trigR, W[15].min, W[15].max, W[15].step);
    w.Write(out.anchor, W[16].bits);
    w.BitPackVec(out.refVelDup, W[17].min, W[17].max, W[17].step);
    // ★ Finish() is not tidiness, it is the whole tail of the record. CBitWriter buffers a partial
    // 64-bit accumulator and only emits it at End(); without this the last 50 bits of a PlayerState
    // and the last 28 of a SimState never reach the buffer, and the comparison below then agrees with
    // itself over a range that excludes the last four fields. The suite caught exactly that: trigR,
    // anchor and refvelocity.dup decoded as zero while the check reported the decode proved.
    w.Finish();
    if (w.Status() != WireStatus::Ok || w.BitsWritten() != bits) {
        return WireDecode::ReencodeDiffers;
    }
    if (!BitsEqual(body + at, back, bits)) {
        return WireDecode::ReencodeDiffers;
    }
    return WireDecode::Ok;
}

WireDecode DecodeBodyBody(const void* bodyPtr, size_t bytes, BodyFrame& out) {
    const uint8_t* body = static_cast<const uint8_t*>(bodyPtr);
    const size_t at = kBodyRecordStartBits / 8;
    if (!body || bytes <= at) {
        return WireDecode::TooShort;
    }

    const bitio::WireField* W = bitio::kSimStateWire;
    EngineBitReader r(body + at, bytes - at);
    out = BodyFrame{};
    out.hasHeader = true;
    // Everything on the SimState schedule, net id included: the message carries it and the re-encode
    // below proves the whole record reproduces the source bits.
    out.fill = kFillBodyWireAll;
    memcpy(&out.hdr0, body, sizeof out.hdr0);
    memcpy(&out.hdr2, body + 16, sizeof out.hdr2);

    uint64_t t = 0;
    r.Read(t, W[0].bits);
    out.bits11 = static_cast<uint32_t>(t);
    // The wire's `component.netid` field. S5.4.5 notes its high dword "must be 1", which is
    // suggestive next to tag==1 meaning a component id -- but suggestive is not established, so the
    // 64 bits are stored whole as the identity VALUE and no tag is claimed for them. A reader gets
    // kFillBodyIdValue without kFillBodyIdTag and BodyIdentityResolvable says no.
    r.Read(out.idValue, W[1].bits);
    r.BitPackVec(out.com, W[2].min, W[2].max, W[2].step);
    r.BitPackQuat(out.ori, W[3].bits);
    r.BitPackVec(out.vel, W[4].min, W[4].max, W[4].step);
    r.BitPackVec(out.angVel, W[5].min, W[5].max, W[5].step);
    r.BitPackVec(out.lastHeldCom, W[6].min, W[6].max, W[6].step);
    if (r.Status() != WireStatus::Ok) {
        return WireDecode::ReadFailed;
    }
    const size_t bits = r.BitsRead();
    if (!TailIsZero(body, bytes, kBodyRecordStartBits + bits)) {
        return WireDecode::PaddingNonZero;
    }

    uint8_t back[kWireScratch] = {};
    EngineBitWriter w(back, sizeof back);
    w.Write(out.bits11, W[0].bits);
    w.Write(out.idValue, W[1].bits);
    w.BitPackVec(out.com, W[2].min, W[2].max, W[2].step);
    w.BitPackQuat(out.ori, W[3].bits);
    w.BitPackVec(out.vel, W[4].min, W[4].max, W[4].step);
    w.BitPackVec(out.angVel, W[5].min, W[5].max, W[5].step);
    w.BitPackVec(out.lastHeldCom, W[6].min, W[6].max, W[6].step);
    w.Finish();   // see DecodePlayerBody: without it the tail of the record is outside the comparison
    if (w.Status() != WireStatus::Ok || w.BitsWritten() != bits) {
        return WireDecode::ReencodeDiffers;
    }
    if (!BitsEqual(body + at, back, bits)) {
        return WireDecode::ReencodeDiffers;
    }
    return WireDecode::Ok;
}

}  // namespace capture
}  // namespace replay
