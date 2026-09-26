// A 1:1 mirror of NRadEngine::CBitWriter / CBitReader, reproduced from echovr.exe.
//
// This layer exists to decode and re-encode bytes the ENGINE produced, bit-identically.  Every arithmetic
// step -- including the association order of the multiplies, which differs between the engine's own writer
// and its own reader -- is reproduced as decompiled, not as an algebraically equivalent rewrite.  An
// equivalent rewrite that rounds one ulp differently produces a stream the engine rejects, or worse, one it
// accepts with a different value.
//
// Provenance of every constant and every formula in this file: the design and the PC functions
//   CBitWriter::Write        0x1415281B0      CBitReader::Read          0x1415278B0
//   CBitWriter::BitPackReal  0x141527F10      CBitReader::BitPackReal   0x141527630
//   CBitWriter::BitPackVec   0x141528060      CBitReader::BitPackVec    (three BitPackReal calls)
//   CBitWriter::BitPackQuat  0x141527C40      CBitReader::BitPackQuat   0x141527350
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace bitio {

// What the engine's own bit primitives can refuse.  The engine raises these through CErrMsg and returns a
// non-zero status; we carry the same distinctions so a caller can tell a malformed stream from a bug.
enum class WireStatus : uint8_t {
    Ok = 0,
    ValueOutOfRange,   // cbitwriter.cpp:37  Write(v,n) with v >= 2^n
    RealOutOfRange,    // cbitwriter.cpp:215 BitPackReal with value outside [min,max]
    IndexOutOfRange,   // cbitreader.cpp:193 BitPackReal read an index above (max-min)/step
    SelectorInvalid,   // cbitreader.cpp:241 BitPackQuat read a largest-component index >= 4
    NoMoreData,        // cbitreader.cpp:69   the stream ran out mid-field
    BufferFull,        // ours: the destination cannot hold another word
    BadParameter,      // ours: a width above 64, or a step that is not finite and positive
};

const char* WireStatusName(WireStatus s);

// (max - min) / step, rounded up, is the highest legal index; the field is that many bits wide.
//
// The engine computes this quantity TWICE with different arithmetic: its writer forms (max-min) * (1/step)
// and its reader forms (max-min) / step.  Those are not the same float.  Both are exposed so a caller can
// assert they agree for a given field -- a field where they disagree is unusable, because the two sides of
// the engine would frame it at different widths.
uint64_t RealStepsWriter(float min, float max, float step);
uint64_t RealStepsReader(float min, float max, float step);
unsigned RealBits(uint64_t steps);   // bit_length: 0 for 0, else bsr+1

// True when the engine's own writer and reader frame this field identically. Every shipped WIRE_Q field
// must satisfy it; tests/o1 asserts it for all of them and refuses any field that does not.
bool RealFramingAgrees(float min, float max, float step);

class EngineBitWriter {
public:
    EngineBitWriter(void* dst, size_t bytes);

    WireStatus Write(uint64_t value, unsigned bits);
    WireStatus BitPackReal(float value, float min, float max, float step);
    WireStatus BitPackVec(const float v[3], float min, float max, float step);
    WireStatus BitPackQuat(const float q[4], unsigned bits);

    // Emits the partial accumulator, so the payload is always a whole number of 64-bit words -- which is
    // what CBitWriter::End does. Idempotent.
    WireStatus Finish();

    size_t     BitsWritten() const { return m_bits; }
    size_t     BytesWritten() const { return m_out; }
    WireStatus Status() const { return m_status; }

private:
    WireStatus Fail(WireStatus s);
    WireStatus Emit(uint64_t word);

    uint8_t*   m_dst;
    size_t     m_cap;
    size_t     m_out = 0;
    uint64_t   m_acc = 0;
    unsigned   m_pos = 0;
    size_t     m_bits = 0;
    WireStatus m_status = WireStatus::Ok;
};

class EngineBitReader {
public:
    EngineBitReader(const void* src, size_t bytes);

    WireStatus Read(uint64_t& out, unsigned bits);
    WireStatus BitPackReal(float& out, float min, float max, float step);
    WireStatus BitPackVec(float out[3], float min, float max, float step);
    WireStatus BitPackQuat(float out[4], unsigned bits);

    size_t     BitsRead() const { return m_bits; }
    WireStatus Status() const { return m_status; }

private:
    WireStatus Fail(WireStatus s);
    uint64_t   WordAt(size_t index) const;

    const uint8_t* m_src;
    size_t         m_bytes;
    size_t         m_word = 0;
    unsigned       m_pos = 0;
    size_t         m_bits = 0;
    WireStatus     m_status = WireStatus::Ok;
};

// The two halves of the quantisation, exposed because the storage codec and the O1 sweep both need them
// without going through a stream.  QuantReal mirrors the engine's writer, DequantReal its reader.
WireStatus QuantReal(float value, float min, float max, float step, uint64_t& index);
float      DequantReal(uint64_t index, float min, float step);

// ★ The engine's two halves do not agree at the top of some domains.  For `com` and `lastheldcom`
// (+/-250 m, 1 mm) the highest legal index 500000 decodes to 250.00003, and feeding THAT back to the
// engine's own writer raises cbitwriter.cpp:215 "Value is out of range" -- so a value the engine's reader
// legitimately produced is a value its writer refuses.  O1's whole-domain sweep is what found it.
//
// The engine mirror above keeps the engine's behaviour, refusal included.  Storage cannot: refusing means
// dropping a record whose value the client really held, and clamping it would change the value, which the
// LOSSLESS rule forbids outright.  So the storage quantiser accepts anything at or above `min` and lets
// the step count be the only ceiling -- exactly the set of values that map into the domain, no wider.
// Nothing is re-quantised and no value is altered; only the input test differs.
WireStatus QuantRealStored(float value, float min, float max, float step, uint64_t& index);

// The smallest-three quaternion codec, split the same way.  `bits` is the per-component width (10 on the
// wire); the encoded field is 2 + 3*bits.
struct Quat10 {
    uint8_t  selector;     // which component was dropped, 0..3
    uint32_t component[3]; // the three transmitted indices, in ascending slot order
};

WireStatus QuantQuat(const float q[4], unsigned bits, Quat10& out);
WireStatus DequantQuat(const Quat10& in, unsigned bits, float q[4]);

}  // namespace bitio
