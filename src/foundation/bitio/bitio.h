// IFieldCodec -- the storage codec for .echodemo streams.  Appendix B.1, owned by W1-A, consumed by
// W1-B, W2-A and W2-B.
//
// ============================================================================================
// THE LOSSLESS RULE, AND WHAT THIS FILE DOES ABOUT IT
//
// Ruling: lossless means bit-exact reproduction of the values the client actually held.
// A value that arrived over the wire already quantised is stored at its wire precision, because that IS
// the client's value.  A value we sampled ourselves at full precision is stored at full precision.
// Re-quantising a value to fewer bits than the client held is forbidden, and 5.3.3 claims the format
// makes that unrepresentable.  This file is where the claim is true or false, so:
//
//   * every entry point takes a FieldSpec and refuses if its class does not match.  There is no call
//     that reduces the precision of an F32-class field, because none of the narrowing entry points
//     accepts one;
//   * PackQuat10 -- 32 bits for a whole orientation -- refuses anything but a WireQuat field.  A
//     quaternion we sampled ourselves goes through PackQuatF32 at 4 x 32 bits.  5.3.3 names only
//     PackWire, and a sampled quaternion squeezed into quat10 would have satisfied it;
//   * PackDelta never subtracts floats.  For a WIRE_Q field it deltas the integer index; for an F32
//     field it XORs the bit patterns.  Both are exactly invertible for every input including NaN,
//     signed zeros, denormals and infinities.  `cur - prev` stored as a real is a silent precision loss
//     that a round-trip test over the same encoder cannot see;
//   * a field whose provenance is not pinned is REFUSED, not guessed.  FieldProv::Unknown is the zero
//     value, so a spec that forgot to declare provenance refuses rather than encoding.
//
// A refusal is sticky and fatal to the record.  Nothing is written, BitsWritten() stops being
// meaningful, and the caller must abandon the record -- silently skipping one field would desynchronise
// every field after it.
// ============================================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace bitio {

// WireQ / F32 / Raw are the design's three storage classes.  WireQuat is a WIRE_Q-class encoding
// whose parameter is a per-component bit width rather than (min,max,step): the engine's smallest-three
// BitPackQuat.  It is a separate class so that a sampled orientation cannot reach it.
enum class FieldClass : uint8_t { WireQ = 0, F32 = 1, Raw = 2, WireQuat = 3 };

// Which binary the field's parameters were read out of.  Unknown is deliberately the zero value: an
// under-initialised FieldSpec refuses instead of encoding against parameters nobody derived.
// O1 does not establish that our constants match the engine -- W2-I's O2 does -- so Arm64
// is permitted and reported, not silently treated as PC.
enum class FieldProv : uint8_t { Unknown = 0, Pc = 1, Arm64 = 2 };

struct FieldSpec {
    uint16_t   id;
    FieldClass cls;
    float      min, max, step;   // WireQ only
    uint8_t    rawBits;          // Raw: field width. WireQuat: per-component width (10 on the wire).
    FieldProv  prov;             // appended after rawBits so existing aggregate initialisers still compile
};

// Non-null when the field cannot be encoded, naming why.  Checked on every entry point; exposed so a
// schema loader can reject a bad table before any capture starts.
const char* FieldRefusal(const FieldSpec&);

// Field width in bits for a WireQ field, 0 if it is unusable.
unsigned WireFieldBits(const FieldSpec&);

class BitWriter {
public:
    BitWriter(void* dst, size_t bytes);

    void PackWire(const FieldSpec&, float value);
    void PackF32(const FieldSpec&, float value);
    void PackRaw(const FieldSpec&, uint64_t bits);
    void PackQuat10(const FieldSpec&, const float q[4]);
    void PackQuatF32(const FieldSpec&, const float q[4]);

    // `prev` must be the value the DECODER will hold at this point -- the previously decoded sample, not
    // the previously sampled one.  Both sides quantise `prev` with the same function, so the pair is
    // exactly invertible whatever the quantiser does.
    void PackDelta(const FieldSpec&, float prev, float cur);
    void PackQuatDelta(const FieldSpec&, const float prev[4], const float cur[4]);

    void PackFlag(bool);

    size_t      BitsWritten() const { return m_bits; }
    size_t      BytesWritten() const { return (m_bits + 7) / 8; }  // the tail byte is zero-padded already
    bool        Refused() const { return m_reason != nullptr; }
    const char* RefusalReason() const { return m_reason; }

private:
    void Put(uint64_t value, unsigned bits);
    bool Guard(const FieldSpec&, FieldClass expect);
    void Refuse(const char* why);

    uint8_t*    m_dst;
    size_t      m_cap;
    size_t      m_bits = 0;
    const char* m_reason = nullptr;
};

class BitReader {
public:
    BitReader(const void* src, size_t bytes);

    float    UnpackWire(const FieldSpec&);
    float    UnpackF32(const FieldSpec&);
    uint64_t UnpackRaw(const FieldSpec&);
    void     UnpackQuat10(const FieldSpec&, float q[4]);
    void     UnpackQuatF32(const FieldSpec&, float q[4]);
    float    UnpackDelta(const FieldSpec&, float prev);
    void     UnpackQuatDelta(const FieldSpec&, const float prev[4], float cur[4]);
    bool     UnpackFlag();

    size_t      BitsRead() const { return m_bits; }
    bool        Overrun() const { return m_overrun; }
    bool        Refused() const { return m_reason != nullptr; }
    const char* RefusalReason() const { return m_reason; }

private:
    uint64_t Take(unsigned bits);
    bool     Guard(const FieldSpec&, FieldClass expect);
    void     Refuse(const char* why);

    const uint8_t* m_src;
    size_t         m_bytes;
    size_t         m_bits = 0;
    bool           m_overrun = false;
    const char*    m_reason = nullptr;
};

}  // namespace bitio
