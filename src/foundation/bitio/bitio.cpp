#include "foundation/bitio/bitio.h"

#include "foundation/bitio/enginewire.h"

#include <cmath>
#include <cstring>

namespace bitio {
namespace {

inline uint64_t LowMask(unsigned bits) { return bits >= 64 ? ~0ull : ((1ull << bits) - 1ull); }

inline uint32_t FloatBits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

inline float BitsFloat(uint32_t u) {
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

inline uint64_t ZigZag(int64_t d) { return (static_cast<uint64_t>(d) << 1) ^ static_cast<uint64_t>(d >> 63); }
inline int64_t  UnZigZag(uint64_t z) { return static_cast<int64_t>(z >> 1) ^ -static_cast<int64_t>(z & 1); }

inline unsigned BitLength(uint64_t v) {
    unsigned n = 0;
    while (v) { ++n; v >>= 1; }
    return n;
}

// The delta code: one bit for "unchanged", then a 2-bit size class.  Class 3 is the escape and is wide
// enough for any difference of two in-range indices, so the code is total.
constexpr unsigned kDeltaClassBits[3] = {5, 9, 14};

inline unsigned DeltaEscapeWidth(unsigned fieldBits) { return fieldBits + 1; }

inline unsigned PickDeltaClass(uint64_t zz, unsigned fieldBits) {
    const unsigned need = BitLength(zz);
    for (unsigned k = 0; k < 3; ++k) {
        if (need <= kDeltaClassBits[k]) return k;
    }
    return need <= DeltaEscapeWidth(fieldBits) ? 3u : 4u;   // 4 == does not fit, which cannot happen
}

inline unsigned DeltaWidth(unsigned k, unsigned fieldBits) {
    return k < 3 ? kDeltaClassBits[k] : DeltaEscapeWidth(fieldBits);
}

inline unsigned CountLeadingZeros32(uint32_t v) {
    unsigned n = 0;
    for (int i = 31; i >= 0 && ((v >> i) & 1u) == 0u; --i) ++n;
    return n;
}

inline unsigned CountTrailingZeros32(uint32_t v) {
    unsigned n = 0;
    while (n < 32 && ((v >> n) & 1u) == 0u) ++n;
    return n;
}

const char* kNotPinned  = "field parameters are not pinned to a binary (FieldProv::Unknown)";
const char* kClassWrong = "field class does not permit this encoder";

}  // namespace

unsigned WireFieldBits(const FieldSpec& f) {
    if (f.cls != FieldClass::WireQ) return 0;
    return RealBits(RealStepsWriter(f.min, f.max, f.step));
}

const char* FieldRefusal(const FieldSpec& f) {
    if (f.prov == FieldProv::Unknown) return kNotPinned;
    switch (f.cls) {
        case FieldClass::WireQ: {
            if (!std::isfinite(f.min) || !std::isfinite(f.max) || !std::isfinite(f.step)) {
                return "WireQ field has a non-finite parameter";
            }
            if (!(f.step > 0.0f) || !(f.max > f.min)) return "WireQ field has an empty or inverted range";
            if (!RealFramingAgrees(f.min, f.max, f.step)) {
                return "the engine's own writer and reader frame this field at different widths";
            }
            const unsigned bits = WireFieldBits(f);
            if (bits == 0 || bits > 48) return "WireQ field width is outside 1..48 bits";
            return nullptr;
        }
        case FieldClass::F32:
            return nullptr;
        case FieldClass::Raw:
            if (f.rawBits == 0 || f.rawBits > 64) return "Raw field width is outside 1..64 bits";
            return nullptr;
        case FieldClass::WireQuat:
            if (f.rawBits == 0 || f.rawBits > 32) return "quaternion component width is outside 1..32 bits";
            return nullptr;
    }
    return "unknown field class";
}

// ---------------------------------------------------------------------------- writer

BitWriter::BitWriter(void* dst, size_t bytes) : m_dst(static_cast<uint8_t*>(dst)), m_cap(bytes) {}

void BitWriter::Refuse(const char* why) {
    if (!m_reason) m_reason = why;
}

bool BitWriter::Guard(const FieldSpec& f, FieldClass expect) {
    if (m_reason) return false;
    if (f.cls != expect) { Refuse(kClassWrong); return false; }
    if (const char* why = FieldRefusal(f)) { Refuse(why); return false; }
    return true;
}

void BitWriter::Put(uint64_t value, unsigned bits) {
    if (m_reason || bits == 0) return;
    if (bits > 64) { Refuse("field width above 64 bits"); return; }
    if ((m_bits + bits + 7) / 8 > m_cap) { Refuse("destination buffer is full"); return; }

    value &= LowMask(bits);
    size_t   pos = m_bits >> 3;
    unsigned off = static_cast<unsigned>(m_bits & 7);
    while (bits) {
        const unsigned take = (bits < 8 - off) ? bits : (8 - off);
        const uint8_t chunk = static_cast<uint8_t>((value & LowMask(take)) << off);
        if (off == 0) {
            m_dst[pos] = chunk;
        } else {
            m_dst[pos] = static_cast<uint8_t>(m_dst[pos] | chunk);
        }
        value >>= take;
        bits -= take;
        m_bits += take;
        off += take;
        if (off == 8) { off = 0; ++pos; }
    }
}

void BitWriter::PackFlag(bool v) { Put(v ? 1u : 0u, 1); }

// ★ THE GRID CHECK, and it is what makes a downgrade unrepresentable rather than merely discouraged.
//
// The design permits WIRE_Q only for a value that ARRIVED OVER THE WIRE ALREADY QUANTISED -- the
// client's own float is then dequant(index), so storing the index is bit-exact.  5.3.3 enforces that by
// which function a stream's schema lets you call, and nothing checks the value itself.  A value the
// capture sampled or interpolated, handed to PackWire by a stream that is wired up wrongly, would be
// snapped to the grid silently: a re-quantisation to fewer bits than the client held, which the ruling
// forbids outright.
//
// So the encoder verifies its own round trip.  If dequant(quant(v)) is not v BIT FOR BIT, then v was not
// received quantised through this field, and the encoder refuses instead of losing the difference.  It
// costs one multiply-add per field.
namespace {
bool OnGrid(float value, uint64_t index, const FieldSpec& f) {
    const float back = DequantReal(index, f.min, f.step);
    return memcmp(&back, &value, sizeof value) == 0;
}
}  // namespace

void BitWriter::PackWire(const FieldSpec& f, float value) {
    if (!Guard(f, FieldClass::WireQ)) return;
    uint64_t index = 0;
    const WireStatus s = QuantRealStored(value, f.min, f.max, f.step, index);
    if (s != WireStatus::Ok) { Refuse(WireStatusName(s)); return; }
    if (!OnGrid(value, index, f)) {
        Refuse("value is not on this field's wire grid, so WIRE_Q would re-quantise it");
        return;
    }
    Put(index, WireFieldBits(f));
}

void BitWriter::PackF32(const FieldSpec& f, float value) {
    if (!Guard(f, FieldClass::F32)) return;
    Put(FloatBits(value), 32);
}

void BitWriter::PackRaw(const FieldSpec& f, uint64_t bits) {
    if (!Guard(f, FieldClass::Raw)) return;
    if (f.rawBits < 64 && bits >= (1ull << f.rawBits)) {
        Refuse("raw value does not fit its declared width");
        return;
    }
    Put(bits, f.rawBits);
}

// The quaternion's grid check, and it carries more weight than the scalar one.  Smallest-three drops a
// whole component and rebuilds it from a square root, so re-encoding a quaternion the capture SAMPLED at
// full precision loses 96 of its 128 bits AND can move the dropped slot.  The design names only
// PackWire, so that violation would have passed every stated check.
namespace {
bool QuatOnGrid(const float q[4], const Quat10& packed, unsigned bits) {
    float back[4];
    if (DequantQuat(packed, bits, back) != WireStatus::Ok) return false;
    return memcmp(back, q, 4 * sizeof(float)) == 0;
}
}  // namespace

void BitWriter::PackQuat10(const FieldSpec& f, const float q[4]) {
    if (!Guard(f, FieldClass::WireQuat)) return;
    Quat10 packed;
    const WireStatus s = QuantQuat(q, f.rawBits, packed);
    if (s != WireStatus::Ok) { Refuse(WireStatusName(s)); return; }
    if (!QuatOnGrid(q, packed, f.rawBits)) {
        Refuse("quaternion is not on this field's wire grid, so quat10 would re-quantise it");
        return;
    }
    Put(packed.selector, 2);
    for (int i = 0; i < 3; ++i) Put(packed.component[i], f.rawBits);
}

void BitWriter::PackQuatF32(const FieldSpec& f, const float q[4]) {
    if (!Guard(f, FieldClass::F32)) return;
    for (int i = 0; i < 4; ++i) Put(FloatBits(q[i]), 32);
}

void BitWriter::PackDelta(const FieldSpec& f, float prev, float cur) {
    if (m_reason) return;
    if (const char* why = FieldRefusal(f)) { Refuse(why); return; }

    if (f.cls == FieldClass::F32) {
        // XOR of the bit patterns, then Gorilla-style trimming.  Never a float subtraction: prev + (cur -
        // prev) does not reproduce cur, and the loss is invisible to a same-encoder round trip.
        const uint32_t x = FloatBits(cur) ^ FloatBits(prev);
        if (x == 0) { Put(0, 1); return; }
        Put(1, 1);
        const unsigned lz = CountLeadingZeros32(x);
        const unsigned tz = CountTrailingZeros32(x);
        const unsigned meaningful = 32 - lz - tz;
        Put(lz, 5);
        Put(meaningful - 1, 5);
        Put(x >> tz, meaningful);
        return;
    }
    if (f.cls != FieldClass::WireQ) { Refuse(kClassWrong); return; }

    uint64_t ip = 0, ic = 0;
    WireStatus s = QuantRealStored(prev, f.min, f.max, f.step, ip);
    if (s != WireStatus::Ok) { Refuse(WireStatusName(s)); return; }
    s = QuantRealStored(cur, f.min, f.max, f.step, ic);
    if (s != WireStatus::Ok) { Refuse(WireStatusName(s)); return; }
    // Both ends, because the decoder reconstructs dequant(ip) when the value is unchanged: an off-grid
    // `prev` would come back as its nearest grid point instead of itself.
    if (!OnGrid(prev, ip, f) || !OnGrid(cur, ic, f)) {
        Refuse("value is not on this field's wire grid, so WIRE_Q would re-quantise it");
        return;
    }

    const unsigned fieldBits = WireFieldBits(f);
    const uint64_t zz = ZigZag(static_cast<int64_t>(ic) - static_cast<int64_t>(ip));
    if (zz == 0) { Put(0, 1); return; }
    Put(1, 1);
    const unsigned k = PickDeltaClass(zz, fieldBits);
    if (k > 3) { Refuse("index delta does not fit the escape width"); return; }
    Put(k, 2);
    Put(zz, DeltaWidth(k, fieldBits));
}

void BitWriter::PackQuatDelta(const FieldSpec& f, const float prev[4], const float cur[4]) {
    if (!Guard(f, FieldClass::WireQuat)) return;
    Quat10 p, c;
    WireStatus s = QuantQuat(prev, f.rawBits, p);
    if (s != WireStatus::Ok) { Refuse(WireStatusName(s)); return; }
    s = QuantQuat(cur, f.rawBits, c);
    if (s != WireStatus::Ok) { Refuse(WireStatusName(s)); return; }
    if (!QuatOnGrid(prev, p, f.rawBits) || !QuatOnGrid(cur, c, f.rawBits)) {
        Refuse("quaternion is not on this field's wire grid, so quat10 would re-quantise it");
        return;
    }

    const bool same = p.selector == c.selector && p.component[0] == c.component[0] &&
                      p.component[1] == c.component[1] && p.component[2] == c.component[2];
    if (same) { Put(0, 1); return; }
    Put(1, 1);
    if (p.selector != c.selector) {
        Put(0, 1);
        Put(c.selector, 2);
        for (int i = 0; i < 3; ++i) Put(c.component[i], f.rawBits);
        return;
    }
    Put(1, 1);
    for (int i = 0; i < 3; ++i) {
        const uint64_t zz = ZigZag(static_cast<int64_t>(c.component[i]) - static_cast<int64_t>(p.component[i]));
        const unsigned k = PickDeltaClass(zz, f.rawBits);
        if (k > 3) { Refuse("quaternion component delta does not fit the escape width"); return; }
        Put(k, 2);
        Put(zz, DeltaWidth(k, f.rawBits));
    }
}

// ---------------------------------------------------------------------------- reader

BitReader::BitReader(const void* src, size_t bytes)
    : m_src(static_cast<const uint8_t*>(src)), m_bytes(bytes) {}

void BitReader::Refuse(const char* why) {
    if (!m_reason) m_reason = why;
}

bool BitReader::Guard(const FieldSpec& f, FieldClass expect) {
    if (m_reason || m_overrun) return false;
    if (f.cls != expect) { Refuse(kClassWrong); return false; }
    if (const char* why = FieldRefusal(f)) { Refuse(why); return false; }
    return true;
}

uint64_t BitReader::Take(unsigned bits) {
    if (m_reason || m_overrun || bits == 0) return 0;
    if (bits > 64) { Refuse("field width above 64 bits"); return 0; }
    if (m_bits + bits > m_bytes * 8) { m_overrun = true; return 0; }

    uint64_t out = 0;
    unsigned got = 0;
    size_t   pos = m_bits >> 3;
    unsigned off = static_cast<unsigned>(m_bits & 7);
    while (bits) {
        const unsigned take = (bits < 8 - off) ? bits : (8 - off);
        const uint64_t chunk = (static_cast<uint64_t>(m_src[pos]) >> off) & LowMask(take);
        out |= chunk << got;
        got += take;
        bits -= take;
        m_bits += take;
        off += take;
        if (off == 8) { off = 0; ++pos; }
    }
    return out;
}

bool BitReader::UnpackFlag() { return Take(1) != 0; }

float BitReader::UnpackWire(const FieldSpec& f) {
    if (!Guard(f, FieldClass::WireQ)) return 0.0f;
    const uint64_t steps = RealStepsReader(f.min, f.max, f.step);
    const uint64_t index = Take(RealBits(steps));
    if (m_overrun) return 0.0f;
    if (index > steps) { Refuse("index above the field's step count"); return 0.0f; }
    return DequantReal(index, f.min, f.step);
}

float BitReader::UnpackF32(const FieldSpec& f) {
    if (!Guard(f, FieldClass::F32)) return 0.0f;
    return BitsFloat(static_cast<uint32_t>(Take(32)));
}

uint64_t BitReader::UnpackRaw(const FieldSpec& f) {
    if (!Guard(f, FieldClass::Raw)) return 0;
    return Take(f.rawBits);
}

void BitReader::UnpackQuat10(const FieldSpec& f, float q[4]) {
    q[0] = q[1] = q[2] = q[3] = 0.0f;
    if (!Guard(f, FieldClass::WireQuat)) return;
    Quat10 packed;
    packed.selector = static_cast<uint8_t>(Take(2));
    for (int i = 0; i < 3; ++i) packed.component[i] = static_cast<uint32_t>(Take(f.rawBits));
    if (m_overrun) return;
    const WireStatus s = DequantQuat(packed, f.rawBits, q);
    if (s != WireStatus::Ok) Refuse(WireStatusName(s));
}

void BitReader::UnpackQuatF32(const FieldSpec& f, float q[4]) {
    q[0] = q[1] = q[2] = q[3] = 0.0f;
    if (!Guard(f, FieldClass::F32)) return;
    for (int i = 0; i < 4; ++i) q[i] = BitsFloat(static_cast<uint32_t>(Take(32)));
}

float BitReader::UnpackDelta(const FieldSpec& f, float prev) {
    if (m_reason || m_overrun) return 0.0f;
    if (const char* why = FieldRefusal(f)) { Refuse(why); return 0.0f; }

    if (f.cls == FieldClass::F32) {
        if (!UnpackFlag()) return prev;
        const unsigned lz = static_cast<unsigned>(Take(5));
        const unsigned meaningful = static_cast<unsigned>(Take(5)) + 1;
        const uint64_t payload = Take(meaningful);
        if (m_overrun) return 0.0f;
        if (lz + meaningful > 32) { Refuse("malformed float delta code"); return 0.0f; }
        const uint32_t x = static_cast<uint32_t>(payload) << (32 - lz - meaningful);
        return BitsFloat(FloatBits(prev) ^ x);
    }
    if (f.cls != FieldClass::WireQ) { Refuse(kClassWrong); return 0.0f; }

    uint64_t ip = 0;
    const WireStatus s = QuantRealStored(prev, f.min, f.max, f.step, ip);
    if (s != WireStatus::Ok) { Refuse(WireStatusName(s)); return 0.0f; }
    if (!UnpackFlag()) return DequantReal(ip, f.min, f.step);

    const unsigned fieldBits = WireFieldBits(f);
    const unsigned k = static_cast<unsigned>(Take(2));
    const uint64_t zz = Take(DeltaWidth(k, fieldBits));
    if (m_overrun) return 0.0f;
    const int64_t ic = static_cast<int64_t>(ip) + UnZigZag(zz);
    if (ic < 0 || static_cast<uint64_t>(ic) > RealStepsReader(f.min, f.max, f.step)) {
        Refuse("index delta lands outside the field's step count");
        return 0.0f;
    }
    return DequantReal(static_cast<uint64_t>(ic), f.min, f.step);
}

void BitReader::UnpackQuatDelta(const FieldSpec& f, const float prev[4], float cur[4]) {
    cur[0] = cur[1] = cur[2] = cur[3] = 0.0f;
    if (!Guard(f, FieldClass::WireQuat)) return;
    Quat10 p;
    const WireStatus s = QuantQuat(prev, f.rawBits, p);
    if (s != WireStatus::Ok) { Refuse(WireStatusName(s)); return; }

    Quat10 c = p;
    if (UnpackFlag()) {
        if (UnpackFlag()) {
            for (int i = 0; i < 3; ++i) {
                const unsigned k = static_cast<unsigned>(Take(2));
                const uint64_t zz = Take(DeltaWidth(k, f.rawBits));
                const int64_t v = static_cast<int64_t>(p.component[i]) + UnZigZag(zz);
                if (v < 0 || static_cast<uint64_t>(v) > LowMask(f.rawBits)) {
                    Refuse("quaternion component delta lands outside the component range");
                    return;
                }
                c.component[i] = static_cast<uint32_t>(v);
            }
        } else {
            c.selector = static_cast<uint8_t>(Take(2));
            for (int i = 0; i < 3; ++i) c.component[i] = static_cast<uint32_t>(Take(f.rawBits));
        }
    }
    if (m_overrun) return;
    const WireStatus d = DequantQuat(c, f.rawBits, cur);
    if (d != WireStatus::Ok) Refuse(WireStatusName(d));
}

}  // namespace bitio
