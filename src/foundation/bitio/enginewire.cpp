#include "foundation/bitio/enginewire.h"

#include "foundation/bitio/engineconst.h"

#include <cmath>
#include <cstring>

namespace bitio {
namespace {

// MSVC inlines floorf/ceilf as a 32-bit truncate plus a one-step fixup, which is what the engine's
// disassembly shows; for any argument the library function and that sequence agree exactly.
inline float EngineFloor(float f) { return floorf(f); }
inline float EngineCeil(float f) { return ceilf(f); }

// The engine's float-to-u64 conversion, with the range the cast is defined over made explicit: a float
// outside it must be reported, never folded to zero, or a wildly out-of-range value would encode as the
// field's minimum.
inline bool ToU64(float f, uint64_t& out) {
    if (!(f >= 0.0f) || !(f < 18446744073709551616.0f)) return false;
    out = static_cast<uint64_t>(f);
    return true;
}

inline uint64_t CeilToU64(float f) {
    uint64_t v = 0;
    return ToU64(EngineCeil(f), v) ? v : 0;
}

inline unsigned BitLength(uint64_t v) {
    unsigned n = 0;
    while (v) { ++n; v >>= 1; }
    return n;
}

inline bool StepUsable(float min, float max, float step) {
    return std::isfinite(min) && std::isfinite(max) && std::isfinite(step) && step > 0.0f && max >= min;
}

inline uint64_t LowMask(unsigned bits) {
    return bits >= 64 ? ~0ull : ((1ull << bits) - 1ull);
}

}  // namespace

const char* WireStatusName(WireStatus s) {
    switch (s) {
        case WireStatus::Ok:              return "ok";
        case WireStatus::ValueOutOfRange: return "value does not fit its field width";
        case WireStatus::RealOutOfRange:  return "value outside [min,max]";
        case WireStatus::IndexOutOfRange: return "index above the field's step count";
        case WireStatus::SelectorInvalid: return "quaternion selector >= 4";
        case WireStatus::NoMoreData:      return "stream exhausted";
        case WireStatus::BufferFull:      return "destination full";
        case WireStatus::BadParameter:    return "unusable field parameters";
    }
    return "unknown";
}

uint64_t RealStepsWriter(float min, float max, float step) {
    if (!StepUsable(min, max, step)) return 0;
    const float invStep = kOne / step;
    return CeilToU64((max - min) * invStep);
}

uint64_t RealStepsReader(float min, float max, float step) {
    if (!StepUsable(min, max, step)) return 0;
    return CeilToU64((max - min) / step);
}

unsigned RealBits(uint64_t steps) { return BitLength(steps); }

bool RealFramingAgrees(float min, float max, float step) {
    if (!StepUsable(min, max, step)) return false;
    return RealStepsWriter(min, max, step) == RealStepsReader(min, max, step);
}

namespace {

WireStatus QuantCommon(float value, float min, float step, uint64_t steps, uint64_t& index) {
    const float invStep = kOne / step;
    const float t = EngineFloor(((value - min) * invStep) + kHalf);
    if (!ToU64(t, index)) { index = 0; return WireStatus::RealOutOfRange; }
    if (index > steps) { index = 0; return WireStatus::IndexOutOfRange; }
    return WireStatus::Ok;
}

}  // namespace

WireStatus QuantReal(float value, float min, float max, float step, uint64_t& index) {
    index = 0;
    if (!StepUsable(min, max, step)) return WireStatus::BadParameter;
    // The engine's range test, ahead of the arithmetic and in its order. NaN fails both comparisons and
    // so falls through in the engine; we reject it, because a NaN index is not a value any stream carries.
    if (!(value >= min) || !(value <= max)) return WireStatus::RealOutOfRange;
    return QuantCommon(value, min, step, RealStepsWriter(min, max, step), index);
}

WireStatus QuantRealStored(float value, float min, float max, float step, uint64_t& index) {
    index = 0;
    if (!StepUsable(min, max, step)) return WireStatus::BadParameter;
    if (!(value >= min)) return WireStatus::RealOutOfRange;   // also rejects NaN
    return QuantCommon(value, min, step, RealStepsWriter(min, max, step), index);
}

float DequantReal(uint64_t index, float min, float step) {
    return (static_cast<float>(static_cast<int64_t>(index)) * step) + min;
}

WireStatus QuantQuat(const float q[4], unsigned bits, Quat10& out) {
    out = Quat10{};
    if (bits == 0 || bits > 32) return WireStatus::BadParameter;
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(q[i])) return WireStatus::RealOutOfRange;
    }

    // Largest component by magnitude. Strictly greater, scanning low to high, so a tie keeps the lower
    // slot -- which is what the engine's chain of comiss/jbe does.
    unsigned sel = 0;
    float best = fabsf(q[0]);
    for (unsigned i = 1; i < 4; ++i) {
        const float a = fabsf(q[i]);
        if (a > best) { best = a; sel = i; }
    }

    // The reader rebuilds the dropped component as a positive square root, so a negative largest is
    // carried by negating the whole quaternion -- the same rotation, and the only representable choice.
    const float sign = (q[sel] < 0.0f) ? -1.0f : 1.0f;

    const uint64_t mask = LowMask(bits);
    const float scale = static_cast<float>(static_cast<int64_t>(mask));
    unsigned n = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (i == sel) continue;
        const float x = q[i] * sign;
        const float t = ((x - kQuatBiasN) * kQuatBiasP) * scale + kHalf;
        uint64_t idx = 0;
        if (!ToU64(EngineFloor(t), idx)) return WireStatus::ValueOutOfRange;
        if (idx > mask) return WireStatus::ValueOutOfRange;
        out.component[n++] = static_cast<uint32_t>(idx);
    }
    out.selector = static_cast<uint8_t>(sel);
    return WireStatus::Ok;
}

WireStatus DequantQuat(const Quat10& in, unsigned bits, float q[4]) {
    if (bits == 0 || bits > 32) return WireStatus::BadParameter;
    if (in.selector >= 4) return WireStatus::SelectorInvalid;

    const uint64_t mask = LowMask(bits);
    const float invMask = kOne / static_cast<float>(static_cast<int64_t>(mask));

    float x[3];
    for (int i = 0; i < 3; ++i) {
        if (in.component[i] > mask) return WireStatus::ValueOutOfRange;
        const float f = static_cast<float>(static_cast<int64_t>(in.component[i]));
        x[i] = ((f * invMask) * kQuatScale) - kQuatBiasP;
    }

    int n = 0;
    for (int i = 0; i < 4; ++i) {
        if (i == in.selector) continue;
        q[i] = x[n++];
    }
    // Left to right, exactly as the engine subtracts: ((1 - a^2) - b^2) - c^2.
    const float rest = ((kOne - (x[0] * x[0])) - (x[1] * x[1])) - (x[2] * x[2]);
    q[in.selector] = sqrtf(rest);
    return WireStatus::Ok;
}

// ---------------------------------------------------------------------------- writer

EngineBitWriter::EngineBitWriter(void* dst, size_t bytes)
    : m_dst(static_cast<uint8_t*>(dst)), m_cap(bytes) {}

WireStatus EngineBitWriter::Fail(WireStatus s) {
    if (m_status == WireStatus::Ok) m_status = s;
    return s;
}

WireStatus EngineBitWriter::Emit(uint64_t word) {
    if (m_out + 8 > m_cap) return Fail(WireStatus::BufferFull);
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(word >> (8 * i));
    memcpy(m_dst + m_out, b, 8);
    m_out += 8;
    return WireStatus::Ok;
}

WireStatus EngineBitWriter::Write(uint64_t value, unsigned bits) {
    if (m_status != WireStatus::Ok) return m_status;
    if (bits > 64) return Fail(WireStatus::BadParameter);
    if (bits < 64 && value >= (1ull << bits)) return Fail(WireStatus::ValueOutOfRange);

    m_acc |= value << m_pos;
    const unsigned after = m_pos + bits;
    m_bits += bits;
    if (after < 64) {
        m_pos = after;
        return WireStatus::Ok;
    }
    if (Emit(m_acc) != WireStatus::Ok) return m_status;
    const unsigned leftover = after - 64;
    const unsigned consumed = bits - leftover;
    m_acc = (consumed >= 64) ? 0ull : ((value >> consumed) & LowMask(leftover));
    m_pos = leftover;
    return WireStatus::Ok;
}

WireStatus EngineBitWriter::BitPackReal(float value, float min, float max, float step) {
    if (m_status != WireStatus::Ok) return m_status;
    uint64_t index = 0;
    const WireStatus s = QuantReal(value, min, max, step, index);
    if (s != WireStatus::Ok) return Fail(s);
    return Write(index, RealBits(RealStepsWriter(min, max, step)));
}

WireStatus EngineBitWriter::BitPackVec(const float v[3], float min, float max, float step) {
    for (int i = 0; i < 3; ++i) {
        const WireStatus s = BitPackReal(v[i], min, max, step);
        if (s != WireStatus::Ok) return s;
    }
    return WireStatus::Ok;
}

WireStatus EngineBitWriter::BitPackQuat(const float q[4], unsigned bits) {
    if (m_status != WireStatus::Ok) return m_status;
    Quat10 packed;
    const WireStatus s = QuantQuat(q, bits, packed);
    if (s != WireStatus::Ok) return Fail(s);
    if (Write(packed.selector, 2) != WireStatus::Ok) return m_status;
    for (int i = 0; i < 3; ++i) {
        if (Write(packed.component[i], bits) != WireStatus::Ok) return m_status;
    }
    return WireStatus::Ok;
}

WireStatus EngineBitWriter::Finish() {
    if (m_status != WireStatus::Ok) return m_status;
    if (m_pos == 0) return WireStatus::Ok;
    if (Emit(m_acc) != WireStatus::Ok) return m_status;
    m_acc = 0;
    m_pos = 0;
    return WireStatus::Ok;
}

// ---------------------------------------------------------------------------- reader

EngineBitReader::EngineBitReader(const void* src, size_t bytes)
    : m_src(static_cast<const uint8_t*>(src)), m_bytes(bytes) {}

WireStatus EngineBitReader::Fail(WireStatus s) {
    if (m_status == WireStatus::Ok) m_status = s;
    return s;
}

// The engine indexes a qword array. A capture handed to us as bytes need not be a whole number of words,
// so the tail is zero-extended rather than read out of bounds; a field that reaches into the padding is
// caught by the m_bytes bound in Read, not by reading garbage.
uint64_t EngineBitReader::WordAt(size_t index) const {
    const size_t off = index * 8;
    if (off >= m_bytes) return 0;
    uint8_t b[8] = {0};
    const size_t n = (m_bytes - off < 8) ? (m_bytes - off) : 8;
    memcpy(b, m_src + off, n);
    uint64_t w = 0;
    for (int i = 7; i >= 0; --i) w = (w << 8) | b[i];
    return w;
}

WireStatus EngineBitReader::Read(uint64_t& out, unsigned bits) {
    out = 0;
    if (m_status != WireStatus::Ok) return m_status;
    if (bits == 0) return WireStatus::Ok;
    if (bits > 64) return Fail(WireStatus::BadParameter);
    if (m_bits + bits > m_bytes * 8) return Fail(WireStatus::NoMoreData);

    const unsigned take = (64 - m_pos < bits) ? (64 - m_pos) : bits;
    uint64_t v = WordAt(m_word) >> m_pos;
    if (take != 64) v &= LowMask(take);
    out = v;

    const unsigned rest = bits - take;
    m_pos += take;
    if (m_pos == 64) {
        ++m_word;
        m_pos = 0;
        if (rest) {
            out |= (WordAt(m_word) & LowMask(rest)) << take;
            m_pos = rest;
        }
    }
    m_bits += bits;
    return WireStatus::Ok;
}

WireStatus EngineBitReader::BitPackReal(float& out, float min, float max, float step) {
    out = 0.0f;
    if (m_status != WireStatus::Ok) return m_status;
    if (!StepUsable(min, max, step)) return Fail(WireStatus::BadParameter);

    const uint64_t steps = RealStepsReader(min, max, step);
    uint64_t index = 0;
    if (Read(index, RealBits(steps)) != WireStatus::Ok) return m_status;
    out = DequantReal(index, min, step);
    if (index > steps) return Fail(WireStatus::IndexOutOfRange);
    return WireStatus::Ok;
}

WireStatus EngineBitReader::BitPackVec(float out[3], float min, float max, float step) {
    for (int i = 0; i < 3; ++i) {
        const WireStatus s = BitPackReal(out[i], min, max, step);
        if (s != WireStatus::Ok) return s;
    }
    return WireStatus::Ok;
}

WireStatus EngineBitReader::BitPackQuat(float out[4], unsigned bits) {
    if (m_status != WireStatus::Ok) return m_status;
    if (bits == 0 || bits > 32) return Fail(WireStatus::BadParameter);

    uint64_t sel = 0;
    if (Read(sel, 2) != WireStatus::Ok) return m_status;
    Quat10 packed;
    packed.selector = static_cast<uint8_t>(sel);
    for (int i = 0; i < 3; ++i) {
        uint64_t c = 0;
        if (Read(c, bits) != WireStatus::Ok) return m_status;
        packed.component[i] = static_cast<uint32_t>(c);
    }
    const WireStatus s = DequantQuat(packed, bits, out);
    if (s != WireStatus::Ok) return Fail(s);
    return WireStatus::Ok;
}

}  // namespace bitio
