// O1 -- the codec round-trip oracle. Offline, no game required.
//
// WHAT O1 ESTABLISHES
//   * quant(dequant(i)) == i over EVERY index in every WIRE_Q field's whole quantisation domain, and
//     over every one of the 2^32 smallest-three quaternion codes;
//   * decode(encode(x)) == x bit-exactly for every one of the 2^32 f32 bit patterns -- NaNs, both zeros,
//     denormals and infinities included -- and for every one of the 2^32 XOR-delta codes;
//   * encode(decode(bytes)) == bytes over real PC wire bytes the retail engine itself produced;
//   * refusal on unpinned fields, wrong storage classes, out-of-range values and truncating widths;
//   * agreement with a second, independently transcribed decoder (refdecode.h).
//
// WHAT O1 DOES NOT ESTABLISH
//   That our constants match the engine.  Every arm here is offline, so the engine's own value is
//   unobtainable; W2-I's O2 owns that by comparing our decode of real wire bytes against the PC engine's
//   in-memory f32 through IEntity.  The design records that the previous version of O1 claimed three
//   independent sources and had none, and this file does not repeat the claim.
//
// MEASURED SENSITIVITY FLOOR -- what O1 is shown to MISS
//   * a field that is correctly encoded but semantically wrong.  Record hand orientation into the head
//     slot and every arm here passes.  That is O3's job, and stating the gap is the point.
//   * a wrong READ of the binary.  Both the shipping codec and refdecode.h trace to one engineer's one
//     reading of echovr.exe; together they catch a transcription slip, not a misunderstanding.
//   * mutations this harness reports as EQUIVALENT: a perturbation that provably changes no encoded bit
//     anywhere in the field's whole domain.  Those are not missed defects -- they are not defects.  The
//     battery separates them from genuinely MISSED mutations by counting divergences, and a mutation that
//     diverges without turning an arm red fails the gate.
//
// COVERAGE OF THE STORED CORPUS IS STATED IN DISTINCT PAYLOADS, never record counts.  The only capture on
// this machine is idle: 17 distinct payloads across two symbols -- 1 PlayerState and 16 SimState -- and
// all 17 re-encode to the identical bits.  No compression ratio is published from it: idle data
// compresses at a ratio that measures the corpus, not the codec.
//
// ★ AND THE RECORD-COUNT TRAP CAUGHT THE AUTHOR OF THIS FILE.  An earlier version stripped a 16-byte
// message header that had been checked against PlayerState and then applied unchanged to SimState, whose
// header is 24 bytes.  The SimState decode began 64 bits early, read the framing word 0x50 as the
// record's 11-bit `bits` field, and produced an angular velocity outside its own range -- and W1-A
// reported that the SYMBOL was mislabelled rather than that the harness held an untested constant.  A
// decode failure has two causes, the wrong symbol or the wrong layout for the right symbol, and the one
// that kept this test green was the wrong one.  At the corrected offset every field lands on its exact
// midpoint, component.netid carries the hi dword of 1 that 5.4.5 requires, and 284 and 370 bits are
// confirmed against the shipping engine's own bytes for the third time.
#include "foundation/bitio/bitio.h"
#include "foundation/bitio/engineconst.h"
#include "foundation/bitio/enginewire.h"
#include "foundation/bitio/wirefields.h"

#include "refdecode.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace bitio;

namespace {

std::string Num(long long v) { return std::to_string(v); }

std::string Hex32(uint32_t v) {
    char b[16];
    snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

std::string Cat(std::initializer_list<std::string> parts) {
    std::string s;
    for (const auto& p : parts) s += p;
    return s;
}

struct Report {
    long long checked = 0;      // size of the domain GATE-RESULT counts
    long long failures = 0;
    long long divergences = 0;  // encoded outputs the mutation actually changed
    std::vector<std::string> messages;

    void Fail(const std::string& what) {
        ++failures;
        if (messages.size() < 40) messages.push_back(what);
    }
    bool Expect(bool cond, const std::string& what) {
        if (!cond) Fail(what);
        return cond;
    }
};

// ---------------------------------------------------------------- mutation
//
// Generated from each field's own shape, never hand-picked.  Every mutation perturbs
// OUR ENCODER only: the corpus bytes, the decoders and refdecode.h stay pristine.  A mutation applied to
// both sides of a round trip is invisible to it, which is exactly how the previous O1 was defeated.
enum class MutKind {
    None, MinUp, MinDown, MaxUp, MaxDown, StepDouble, StepHalf, WidthUp, WidthDown,
    QuatWidthUp, QuatWidthDown, QuatSelectorInvert, BitOrderFlip,
};

struct Mutation {
    MutKind     kind = MutKind::None;
    std::string field;
    std::string id;
};

const char* MutName(MutKind k) {
    switch (k) {
        case MutKind::None:               return "none";
        case MutKind::MinUp:              return "min+ulp";
        case MutKind::MinDown:            return "min-ulp";
        case MutKind::MaxUp:              return "max+ulp";
        case MutKind::MaxDown:            return "max-ulp";
        case MutKind::StepDouble:         return "step-x2";
        case MutKind::StepHalf:           return "step-half";
        case MutKind::WidthUp:            return "width+1";
        case MutKind::WidthDown:          return "width-1";
        case MutKind::QuatWidthUp:        return "quatwidth+1";
        case MutKind::QuatWidthDown:      return "quatwidth-1";
        case MutKind::QuatSelectorInvert: return "quatsel-invert";
        case MutKind::BitOrderFlip:       return "bitorder-flip";
    }
    return "?";
}

float NextUlp(float f, int dir) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    const bool up = (dir > 0) == (f >= 0.0f);
    u = up ? (u + 1u) : (u - 1u);
    memcpy(&f, &u, sizeof f);
    return f;
}

// ---------------------------------------------------------------- fields

struct Field {
    std::string name;
    FieldSpec   spec;
    bool        vector3;
};

std::vector<Field> ShippedFields(const std::string& dropped) {
    std::vector<Field> out;
    uint16_t id = 1;
    auto add = [&](const WireField* table, size_t n, const char* prefix) {
        for (size_t i = 0; i < n; ++i) {
            const std::string name = std::string(prefix) + "." + table[i].name;
            if (name == dropped) continue;
            out.push_back(Field{name, SpecOf(table[i], id), IsVectorField(table[i])});
            ++id;
        }
    };
    add(kSimStateWire, kSimStateWireCount, "sim");
    add(kPlayerStateWire, kPlayerStateWireCount, "player");
    return out;
}

std::vector<Mutation> GenerateMutations(const std::vector<Field>& fields) {
    std::vector<Mutation> out;
    auto push = [&](MutKind k, const std::string& f) {
        out.push_back(Mutation{k, f, f.empty() ? std::string(MutName(k)) : f + ":" + MutName(k)});
    };
    for (const Field& f : fields) {
        if (f.spec.cls == FieldClass::WireQ) {
            push(MutKind::MinUp, f.name);
            push(MutKind::MinDown, f.name);
            push(MutKind::MaxUp, f.name);
            push(MutKind::MaxDown, f.name);
            push(MutKind::StepDouble, f.name);
            push(MutKind::StepHalf, f.name);
            push(MutKind::WidthUp, f.name);
            push(MutKind::WidthDown, f.name);
        } else if (f.spec.cls == FieldClass::WireQuat) {
            push(MutKind::QuatWidthUp, f.name);
            push(MutKind::QuatWidthDown, f.name);
            push(MutKind::QuatSelectorInvert, f.name);
        }
    }
    push(MutKind::BitOrderFlip, std::string());
    return out;
}

// The encoder's view of a field once the mutation is applied. Decoders never see this.
struct EncodeView {
    FieldSpec spec;
    unsigned  bits;        // WireQ: field width. WireQuat: per-component width.
    bool      flipSelector = false;
};

EncodeView ViewFor(const Field& f, const Mutation& m) {
    EncodeView v;
    v.spec = f.spec;
    v.bits = (f.spec.cls == FieldClass::WireQ) ? WireFieldBits(f.spec) : f.spec.rawBits;
    if (m.field != f.name) return v;
    switch (m.kind) {
        case MutKind::MinUp:         v.spec.min = NextUlp(f.spec.min, +1); break;
        case MutKind::MinDown:       v.spec.min = NextUlp(f.spec.min, -1); break;
        case MutKind::MaxUp:         v.spec.max = NextUlp(f.spec.max, +1); break;
        case MutKind::MaxDown:       v.spec.max = NextUlp(f.spec.max, -1); break;
        case MutKind::StepDouble:    v.spec.step = f.spec.step * 2.0f; break;
        case MutKind::StepHalf:      v.spec.step = f.spec.step * 0.5f; break;
        case MutKind::WidthUp:       ++v.bits; break;
        case MutKind::WidthDown:     if (v.bits > 1) --v.bits; break;
        case MutKind::QuatWidthUp:   v.spec.rawBits = static_cast<uint8_t>(f.spec.rawBits + 1); ++v.bits; break;
        case MutKind::QuatWidthDown: v.spec.rawBits = static_cast<uint8_t>(f.spec.rawBits - 1); --v.bits; break;
        case MutKind::QuatSelectorInvert: v.flipSelector = true; break;
        default: break;
    }
    if (v.spec.cls == FieldClass::WireQ && m.kind != MutKind::WidthUp && m.kind != MutKind::WidthDown &&
        m.kind != MutKind::None) {
        v.bits = WireFieldBits(v.spec);
    }
    return v;
}

// ---------------------------------------------------------------- bit buffer

struct BitVec {
    std::vector<uint8_t> bytes;
    size_t bits = 0;

    void Put(uint64_t v, unsigned n) {
        for (unsigned i = 0; i < n; ++i) {
            if ((bits & 7) == 0) bytes.push_back(0);
            if ((v >> i) & 1u) bytes[bits >> 3] = static_cast<uint8_t>(bytes[bits >> 3] | (1u << (bits & 7)));
            ++bits;
        }
    }
    bool Get(size_t i) const { return ((bytes[i >> 3] >> (i & 7)) & 1u) != 0; }
    void FlipByteOrder() {
        for (uint8_t& b : bytes) {
            uint8_t r = 0;
            for (int i = 0; i < 8; ++i) r = static_cast<uint8_t>(r | (((b >> i) & 1u) << (7 - i)));
            b = r;
        }
    }
};

// ---------------------------------------------------------------- A1 framing

long long ArmFraming(const std::vector<Field>& fields, const Mutation& mut, Report& rep) {
    long long n = 0;
    for (const Field& f : fields) {
        if (f.spec.cls != FieldClass::WireQ && f.spec.cls != FieldClass::WireQuat) { ++n; continue; }
        ++n;
        if (const char* why = FieldRefusal(f.spec)) {
            rep.Fail(Cat({"A1 ", f.name, ": shipped field is refused: ", why}));
            continue;
        }
        const EncodeView v = ViewFor(f, mut);
        if (f.spec.cls == FieldClass::WireQuat) {
            if (v.bits != f.spec.rawBits || v.flipSelector) ++rep.divergences;
            rep.Expect(v.bits == f.spec.rawBits && !v.flipSelector,
                       Cat({"A1 ", f.name, ": encoder does not use the wire's quaternion encoding"}));
            continue;
        }
        // The engine derives this width TWICE with different arithmetic -- (max-min)*(1/step) in its
        // writer, (max-min)/step in its reader. Nothing else in the project checks that they agree.
        const uint64_t wSteps = RealStepsWriter(f.spec.min, f.spec.max, f.spec.step);
        const uint64_t rSteps = RealStepsReader(f.spec.min, f.spec.max, f.spec.step);
        rep.Expect(wSteps == rSteps, Cat({"A1 ", f.name, ": the engine's writer frames ", Num((long long)wSteps),
                                          " steps and its reader frames ", Num((long long)rSteps)}));
        const unsigned refBits = o1ref::RefWidth(o1ref::RefSteps(f.spec.min, f.spec.max, f.spec.step));
        if (v.bits != refBits) ++rep.divergences;
        rep.Expect(v.bits == refBits,
                   Cat({"A1 ", f.name, ": encoder writes ", Num(v.bits), " bits, the reference decoder reads ",
                        Num(refBits)}));
    }
    return n;
}

// ---------------------------------------------------------------- A2 whole-domain index sweep

struct SweepStats {
    long long indices = 0;
    long long outOfDomainRefused = 0;
    long long engineWriterRejects = 0;   // indices the engine's own writer would refuse to re-emit
};

SweepStats ArmWireSweep(const std::vector<Field>& fields, const Mutation& mut, Report& rep) {
    SweepStats st;
    for (const Field& f : fields) {
        if (f.spec.cls != FieldClass::WireQ) continue;
        if (FieldRefusal(f.spec)) continue;
        const EncodeView v = ViewFor(f, mut);
        const uint64_t steps = RealStepsWriter(f.spec.min, f.spec.max, f.spec.step);
        const unsigned pristineBits = WireFieldBits(f.spec);

        long long broken = 0, refMismatch = 0, diverged = 0, engineRejects = 0;
        for (uint64_t i = 0; i <= steps; ++i) {
            const float value = DequantReal(i, f.spec.min, f.spec.step);
            uint64_t back = 0;
            const WireStatus s = QuantRealStored(value, v.spec.min, v.spec.max, v.spec.step, back);
            if (s != WireStatus::Ok || back != i || v.bits != pristineBits) { ++broken; ++diverged; }

            // ★ The engine's own two halves disagree at the top of some domains: an index its reader
            // accepts can decode to a value its writer refuses (cbitwriter.cpp:215). Counted rather than
            // hidden, because a caller that mirrors the engine exactly will drop those records.
            uint64_t mirror = 0;
            if (QuantReal(value, f.spec.min, f.spec.max, f.spec.step, mirror) != WireStatus::Ok) ++engineRejects;

            // The reference decoder rebuilds the value from a real bit stream with its own constants.
            // Writing the same expression out here instead would have compared DequantReal against a
            // copy of itself: an arm that cannot fail, which is the shape this project keeps finding.
            uint8_t refBuf[16] = {0};
            EngineBitWriter ew(refBuf, sizeof refBuf);
            ew.Write(i, pristineBits);
            ew.Finish();
            o1ref::Cursor rc{refBuf, sizeof refBuf, 0, false};
            const float refValue = o1ref::RefBitPackReal(rc, f.spec.min, f.spec.max, f.spec.step);
            if (rc.bad || memcmp(&refValue, &value, sizeof value) != 0) ++refMismatch;
        }
        st.indices += static_cast<long long>(steps) + 1;
        st.engineWriterRejects += engineRejects;
        rep.divergences += diverged;
        rep.Expect(broken == 0, Cat({"A2 ", f.name, ": ", Num(broken), " of ", Num((long long)steps + 1),
                                     " indices do not survive quant(dequant(i))"}));
        rep.Expect(refMismatch == 0, Cat({"A2 ", f.name, ": ", Num(refMismatch),
                                          " indices disagree with the reference decoder"}));

        // Every code above the step count must be refused, not silently decoded. The engine's own reader
        // raises cbitreader.cpp:193 there, so a decoder that accepts one is inventing a value.
        const uint64_t top = (pristineBits >= 64) ? ~0ull : ((1ull << pristineBits) - 1ull);
        long long leaked = 0;
        const FieldSpec asRaw{f.spec.id, FieldClass::Raw, 0, 0, 0, static_cast<uint8_t>(pristineBits),
                              FieldProv::Pc};
        for (uint64_t i = steps + 1; i <= top && i <= steps + 4096; ++i) {
            uint8_t buf[16] = {0};
            BitWriter w(buf, sizeof buf);
            w.PackRaw(asRaw, i);
            BitReader r(buf, sizeof buf);
            (void)r.UnpackWire(f.spec);
            if (!r.Refused()) ++leaked;
            ++st.outOfDomainRefused;
        }
        rep.Expect(leaked == 0, Cat({"A2 ", f.name, ": ", Num(leaked),
                                     " out-of-domain codes decoded instead of refusing"}));
    }
    return st;
}

// ---------------------------------------------------------------- A3 every quaternion code
//
// Factored, and the factoring is exact rather than a sample.  The decode of (i0,i1,i2) does not depend on
// the selector; and when the encoder picks the same selector back, the three transmitted components are
// always the other three slots in ascending order, so the three re-quantisations are computed once per
// triple and the selector only decides an argmax.  ArmQuatSpot re-runs a strided slice of the same domain
// through the UNFACTORED public API, which is what proves the factoring faithful.
//
// This arm exercises the algorithm, not the field table, so it is mutation-independent by construction
// and the battery skips it.
struct QuatStats {
    std::atomic<long long> total{0};
    std::atomic<long long> unreachable{0};
    std::atomic<long long> exact{0};
    std::atomic<long long> mismatch{0};
    std::atomic<long long> selectorNotArgmax{0};
};

void QuatSweepRange(unsigned bits, uint32_t lo, uint32_t hi, QuatStats& st) {
    const uint32_t mask = (1u << bits) - 1u;
    long long total = 0, unreachable = 0, exact = 0, mismatch = 0, notArgmax = 0;

    for (uint32_t i0 = lo; i0 < hi; ++i0) {
        for (uint32_t i1 = 0; i1 <= mask; ++i1) {
            for (uint32_t i2 = 0; i2 <= mask; ++i2) {
                for (unsigned sel = 0; sel < 4; ++sel) {
                    ++total;
                    Quat10 code;
                    code.selector = static_cast<uint8_t>(sel);
                    code.component[0] = i0;
                    code.component[1] = i1;
                    code.component[2] = i2;

                    float q[4];
                    if (DequantQuat(code, bits, q) != WireStatus::Ok || !(q[sel] >= 0.0f)) {
                        ++unreachable;   // the remainder went negative: no quaternion decodes to this
                        continue;
                    }
                    Quat10 back;
                    if (QuantQuat(q, bits, back) != WireStatus::Ok) { ++mismatch; continue; }
                    // A code whose declared selector is not the one the encoder picks cannot have come
                    // from the encoder, which always drops the largest component. Unreachable, not a
                    // failure -- and the verdict comes from QuantQuat itself rather than from a copy of
                    // its argmax written out here, which would have tested the copy.
                    if (back.selector != sel) { ++notArgmax; ++unreachable; continue; }
                    if (back.component[0] == i0 && back.component[1] == i1 && back.component[2] == i2) {
                        ++exact;
                    } else {
                        ++mismatch;
                    }
                }
            }
        }
    }
    st.total += total;
    st.unreachable += unreachable;
    st.exact += exact;
    st.mismatch += mismatch;
    st.selectorNotArgmax += notArgmax;
}

void ArmQuatSweep(unsigned bits, QuatStats& st) {
    const uint32_t count = (bits >= 32) ? 0xFFFFFFFFu : (1u << bits);
    unsigned nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 4;
    if (nthreads > count) nthreads = count;
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < nthreads; ++t) {
        const uint32_t lo = static_cast<uint32_t>(static_cast<uint64_t>(count) * t / nthreads);
        const uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(count) * (t + 1) / nthreads);
        pool.emplace_back([=, &st] { QuatSweepRange(bits, lo, hi, st); });
    }
    for (auto& th : pool) th.join();
}

// Prove the A3 counter can move before believing the zero it reports. Decoding at one component width
// and re-encoding at another is precisely the disagreement a wrong width causes, so if this cannot
// produce a mismatch then neither can a real defect.
long long ArmQuatProbe(unsigned bits, Report& rep) {
    long long seen = 0, mismatches = 0;
    const uint32_t mask = (1u << bits) - 1u;
    uint64_t rng = 0x2545F4914F6CDD1Dull;
    for (int trial = 0; trial < 20000; ++trial) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        Quat10 code;
        code.selector = static_cast<uint8_t>(rng & 3);
        code.component[0] = static_cast<uint32_t>((rng >> 8) & mask);
        code.component[1] = static_cast<uint32_t>((rng >> 24) & mask);
        code.component[2] = static_cast<uint32_t>((rng >> 40) & mask);
        float q[4];
        if (DequantQuat(code, bits, q) != WireStatus::Ok || !(q[code.selector] >= 0.0f)) continue;
        Quat10 back;
        if (QuantQuat(q, bits - 1, back) != WireStatus::Ok) { ++mismatches; ++seen; continue; }
        ++seen;
        if (back.selector != code.selector || back.component[0] != code.component[0] ||
            back.component[1] != code.component[1] || back.component[2] != code.component[2]) {
            ++mismatches;
        }
    }
    rep.Expect(seen > 0, "A3 probe: covered no codes");
    rep.Expect(mismatches > 0,
               "A3 probe: re-encoding at a different component width produced no mismatch, so the sweep's "
               "mismatch counter cannot move and its zero means nothing");
    return mismatches;
}

long long ArmQuatSpot(unsigned bits, uint64_t stride, Report& rep) {
    const uint32_t mask = (1u << bits) - 1u;
    const uint64_t codes = 4ull << (3 * bits);
    long long tried = 0, bad = 0;
    for (uint64_t code = 0; code < codes; code += stride) {
        Quat10 in;
        in.selector = static_cast<uint8_t>(code & 3);
        in.component[0] = static_cast<uint32_t>((code >> 2) & mask);
        in.component[1] = static_cast<uint32_t>((code >> (2 + bits)) & mask);
        in.component[2] = static_cast<uint32_t>((code >> (2 + 2 * bits)) & mask);

        float q[4];
        if (DequantQuat(in, bits, q) != WireStatus::Ok) continue;
        if (!std::isfinite(q[in.selector])) continue;   // no encoder can produce this code
        unsigned pick = 0;
        for (unsigned k = 1; k < 4; ++k) {
            if (fabsf(q[k]) > fabsf(q[pick])) pick = k;
        }
        if (pick != in.selector) continue;              // nor this one: the selector is not the argmax
        Quat10 back;
        if (QuantQuat(q, bits, back) != WireStatus::Ok) { ++bad; ++tried; continue; }
        if (back.selector != in.selector || back.component[0] != in.component[0] ||
            back.component[1] != in.component[1] || back.component[2] != in.component[2]) {
            ++bad;
        }
        ++tried;
    }
    rep.Expect(bad == 0, Cat({"A3 spot: ", Num(bad), " of ", Num(tried),
                              " reachable quaternion codes did not survive the public API"}));
    return tried;
}

// ---------------------------------------------------------------- A4 every f32 pattern and delta code

struct F32Stats {
    std::atomic<long long> patterns{0};
    std::atomic<long long> deltas{0};
    std::atomic<long long> bad{0};
};

void F32Range(uint64_t lo, uint64_t hi, F32Stats& st) {
    const FieldSpec spec{1, FieldClass::F32, 0, 0, 0, 0, FieldProv::Pc};
    long long bad = 0, n = 0, d = 0;
    uint8_t buf[16];
    const float base = 1.0f;
    uint32_t baseBits;
    memcpy(&baseBits, &base, sizeof baseBits);

    for (uint64_t u = lo; u < hi; ++u) {
        const uint32_t pattern = static_cast<uint32_t>(u);
        float f;
        memcpy(&f, &pattern, sizeof f);

        BitWriter w(buf, sizeof buf);
        w.PackF32(spec, f);
        BitReader r(buf, sizeof buf);
        const float got = r.UnpackF32(spec);
        uint32_t gotBits;
        memcpy(&gotBits, &got, sizeof gotBits);
        if (w.Refused() || r.Refused() || r.Overrun() || gotBits != pattern) ++bad;
        ++n;

        // The XOR-delta code's whole domain IS the XOR pattern, so sweeping every u sweeps every code.
        const uint32_t curBits = baseBits ^ pattern;
        float cur;
        memcpy(&cur, &curBits, sizeof cur);
        BitWriter w2(buf, sizeof buf);
        w2.PackDelta(spec, base, cur);
        BitReader r2(buf, sizeof buf);
        const float got2 = r2.UnpackDelta(spec, base);
        uint32_t got2Bits;
        memcpy(&got2Bits, &got2, sizeof got2Bits);
        if (w2.Refused() || r2.Refused() || r2.Overrun() || got2Bits != curBits) ++bad;
        ++d;
    }
    st.patterns += n;
    st.deltas += d;
    st.bad += bad;
}

void ArmF32(F32Stats& st) {
    unsigned nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 4;
    std::vector<std::thread> pool;
    const uint64_t total = 1ull << 32;
    for (unsigned t = 0; t < nthreads; ++t) {
        const uint64_t lo = total * t / nthreads;
        const uint64_t hi = total * (t + 1) / nthreads;
        pool.emplace_back([=, &st] { F32Range(lo, hi, st); });
    }
    for (auto& th : pool) th.join();
}

// ---------------------------------------------------------------- A8 the grid check, both ways
//
// PackWire and PackQuat10 refuse a value that is not on their field's wire grid, which is what makes a
// silent re-quantisation unrepresentable.  A guard like that fails in two directions and only one of them
// is visible in normal use: if it never refuses it is decoration, and if it always refuses the codec
// cannot store anything.  This arm measures both ends -- every sampled on-grid value must be ACCEPTED and
// every off-grid neighbour must be REFUSED -- so neither failure can hide.
struct GridStats {
    long long accepted = 0;
    long long refusedOffGrid = 0;
    long long neighbourMoved = 0;   // one ulp away lands on another grid point, so it IS representable
};

GridStats ArmGrid(const std::vector<Field>& fields, Report& rep) {
    GridStats st;
    uint8_t buf[64];
    for (const Field& f : fields) {
        if (FieldRefusal(f.spec)) continue;
        if (f.spec.cls == FieldClass::WireQ) {
            const uint64_t steps = RealStepsWriter(f.spec.min, f.spec.max, f.spec.step);
            const uint64_t stride = (steps / 512) + 1;
            for (uint64_t i = 0; i <= steps; i += stride) {
                const float onGrid = DequantReal(i, f.spec.min, f.spec.step);
                BitWriter w(buf, sizeof buf);
                w.PackWire(f.spec, onGrid);
                if (!rep.Expect(!w.Refused(), Cat({"A8 ", f.name, ": refused an on-grid value at index ",
                                                   Num((long long)i), ": ",
                                                   w.RefusalReason() ? w.RefusalReason() : ""}))) break;
                ++st.accepted;

                const float off = NextUlp(onGrid, +1);
                uint64_t offIndex = 0;
                if (QuantRealStored(off, f.spec.min, f.spec.max, f.spec.step, offIndex) != WireStatus::Ok ||
                    offIndex != i) {
                    ++st.neighbourMoved;
                    continue;
                }
                BitWriter w2(buf, sizeof buf);
                w2.PackWire(f.spec, off);
                if (!rep.Expect(w2.Refused(),
                                Cat({"A8 ", f.name, ": accepted an off-grid value at index ",
                                     Num((long long)i), ", which would re-quantise it"}))) break;
                ++st.refusedOffGrid;
            }
        } else if (f.spec.cls == FieldClass::WireQuat) {
            const unsigned bits = f.spec.rawBits;
            const uint32_t mask = (1u << bits) - 1u;
            uint64_t rng = 0xD1B54A32D192ED03ull;
            for (int trial = 0; trial < 20000; ++trial) {
                rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                Quat10 code;
                code.selector = static_cast<uint8_t>(rng & 3);
                code.component[0] = static_cast<uint32_t>((rng >> 8) & mask);
                code.component[1] = static_cast<uint32_t>((rng >> 24) & mask);
                code.component[2] = static_cast<uint32_t>((rng >> 40) & mask);
                float q[4];
                if (DequantQuat(code, bits, q) != WireStatus::Ok) continue;
                if (!std::isfinite(q[code.selector])) continue;
                unsigned pick = 0;
                for (unsigned k = 1; k < 4; ++k) {
                    if (fabsf(q[k]) > fabsf(q[pick])) pick = k;
                }
                if (pick != code.selector) continue;   // not a code any encoder produces

                BitWriter w(buf, sizeof buf);
                w.PackQuat10(f.spec, q);
                if (!rep.Expect(!w.Refused(),
                                Cat({"A8 ", f.name, ": refused a quaternion that came off the wire: ",
                                     w.RefusalReason() ? w.RefusalReason() : ""}))) break;
                ++st.accepted;

                // Nudge a TRANSMITTED component. The dropped slot is rebuilt from the other three, so
                // perturbing that one alone would be undetectable by construction.
                float nudged[4];
                memcpy(nudged, q, sizeof nudged);
                const unsigned slot = (code.selector == 0) ? 1u : 0u;
                nudged[slot] = NextUlp(nudged[slot], +1);
                Quat10 recheck;
                if (QuantQuat(nudged, bits, recheck) != WireStatus::Ok) continue;
                if (recheck.selector != code.selector || recheck.component[0] != code.component[0] ||
                    recheck.component[1] != code.component[1] || recheck.component[2] != code.component[2]) {
                    ++st.neighbourMoved;
                    continue;
                }
                BitWriter w2(buf, sizeof buf);
                w2.PackQuat10(f.spec, nudged);
                if (!rep.Expect(w2.Refused(),
                                Cat({"A8 ", f.name, ": accepted an off-grid quaternion, which would "
                                     "re-quantise it"}))) break;
                ++st.refusedOffGrid;
            }
        }
    }
    rep.Expect(st.accepted > 0, "A8: no on-grid value was accepted; the arm covered nothing");
    rep.Expect(st.refusedOffGrid > 0, "A8: no off-grid value was refused; the grid check is decoration");
    return st;
}


// ---------------------------------------------------------------- A5 refusal

long long ArmRefusal(Report& rep) {
    long long n = 0;
    uint8_t buf[64];
    const FieldSpec wire{1, FieldClass::WireQ, -250.0f, 250.0f, Kf(0x3A83126Fu), 0, FieldProv::Pc};
    const FieldSpec f32{2, FieldClass::F32, 0, 0, 0, 0, FieldProv::Pc};
    const FieldSpec raw{3, FieldClass::Raw, 0, 0, 0, 8, FieldProv::Pc};
    const FieldSpec quat{4, FieldClass::WireQuat, 0, 0, 0, 10, FieldProv::Pc};
    const float unit[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    auto refuses = [&](const char* what, void (*body)(BitWriter&, const FieldSpec&, const float*),
                       const FieldSpec& s) {
        BitWriter w(buf, sizeof buf);
        body(w, s, unit);
        rep.Expect(w.Refused(), std::string("A5 ") + what + ": encoder did not refuse");
        ++n;
    };

    // An under-initialised spec carries FieldProv::Unknown and must refuse rather than encode against
    // parameters nobody derived.
    FieldSpec unpinnedWire = wire;
    unpinnedWire.prov = FieldProv::Unknown;
    FieldSpec unpinnedF32 = f32;
    unpinnedF32.prov = FieldProv::Unknown;
    refuses("unpinned WIRE_Q field", [](BitWriter& w, const FieldSpec& s, const float*) { w.PackWire(s, 0.0f); },
            unpinnedWire);
    refuses("unpinned F32 field", [](BitWriter& w, const FieldSpec& s, const float*) { w.PackF32(s, 1.0f); },
            unpinnedF32);

    // ★ The class guards.  The design names only PackWire; a quaternion we sampled at full precision
    // pushed through the 32-bit smallest-three encoder would have satisfied that rule while losing 96 bits.
    refuses("PackWire on an F32 field", [](BitWriter& w, const FieldSpec& s, const float*) { w.PackWire(s, 1.0f); }, f32);
    refuses("PackF32 on a WIRE_Q field", [](BitWriter& w, const FieldSpec& s, const float*) { w.PackF32(s, 1.0f); }, wire);
    refuses("PackQuat10 on an F32 field", [](BitWriter& w, const FieldSpec& s, const float* q) { w.PackQuat10(s, q); }, f32);
    refuses("PackQuatF32 on a WireQuat field", [](BitWriter& w, const FieldSpec& s, const float* q) { w.PackQuatF32(s, q); }, quat);
    refuses("PackRaw on a WIRE_Q field", [](BitWriter& w, const FieldSpec& s, const float*) { w.PackRaw(s, 1); }, wire);
    refuses("PackDelta on a Raw field", [](BitWriter& w, const FieldSpec& s, const float*) { w.PackDelta(s, 0.0f, 1.0f); }, raw);
    refuses("PackDelta on a WireQuat field", [](BitWriter& w, const FieldSpec& s, const float*) { w.PackDelta(s, 0.0f, 1.0f); }, quat);

    // Values the engine itself refuses.
    refuses("value above max", [](BitWriter& w, const FieldSpec& s, const float*) { w.PackWire(s, 251.0f); }, wire);
    refuses("value below min", [](BitWriter& w, const FieldSpec& s, const float*) { w.PackWire(s, -251.0f); }, wire);
    refuses("NaN into a WIRE_Q field", [](BitWriter& w, const FieldSpec& s, const float*) { w.PackWire(s, NAN); }, wire);
    // One past the widest value the field can hold, derived from the field rather than written down: a
    // bare 2^8 here collided with an unrelated registry fact the moment another packet added one.
    refuses("raw value wider than its field", [](BitWriter& w, const FieldSpec& s, const float*) {
        w.PackRaw(s, 1ull << s.rawBits);
    }, raw);
    refuses("non-unit quaternion", [](BitWriter& w, const FieldSpec& s, const float*) {
        const float big[4] = {2.0f, 2.0f, 2.0f, 2.0f};
        w.PackQuat10(s, big);
    }, quat);

    {
        uint8_t tiny[2] = {0, 0};
        BitWriter w(tiny, sizeof tiny);
        for (int i = 0; i < 8; ++i) w.PackF32(f32, 1.0f);
        rep.Expect(w.Refused(), "A5 buffer overflow: encoder did not refuse");
        ++n;
    }
    {
        uint8_t tiny[2] = {0, 0};
        BitReader r(tiny, sizeof tiny);
        for (int i = 0; i < 8; ++i) (void)r.UnpackF32(f32);
        rep.Expect(r.Overrun(), "A5 buffer underrun: reader did not report an overrun");
        ++n;
    }

    // The delta path must never lose a bit. prev + (cur - prev) does not reproduce cur in f32, and a
    // round trip through one encoder cannot see that; these are the values where it bites hardest.
    {
        const float pairs[][2] = {
            {1.0f, 1.0000001192092896f},          // one ulp apart
            {1e30f, 1.0f},                        // catastrophic cancellation for a float delta
            {0.0f, -0.0f},                        // distinct bit patterns, equal as floats
            {NAN, 1.0f},
            {1.0f, NAN},
            {INFINITY, -INFINITY},
            {5.877472e-39f, 1.401298e-45f},       // denormals
        };
        for (const auto& p : pairs) {
            BitWriter w(buf, sizeof buf);
            w.PackDelta(f32, p[0], p[1]);
            BitReader r(buf, sizeof buf);
            const float got = r.UnpackDelta(f32, p[0]);
            uint32_t a, b;
            memcpy(&a, &got, 4);
            memcpy(&b, &p[1], 4);
            rep.Expect(!w.Refused() && !r.Refused() && a == b,
                       Cat({"A5 f32 delta: ", Hex32(b), " came back as ", Hex32(a)}));
            ++n;
        }
    }
    return n;
}

// ---------------------------------------------------------------- A6 reference decoder

long long ArmReference(const std::vector<Field>& fields, const Mutation& mut, Report& rep) {
    long long n = 0;
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    auto next = [&] {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };
    std::vector<uint8_t> buf(64);
    for (const Field& f : fields) {
        if (f.spec.cls != FieldClass::WireQ && f.spec.cls != FieldClass::WireQuat) continue;
        if (FieldRefusal(f.spec)) continue;
        // Ours mutated, theirs pristine. Handing both sides the same spec would make this arm blind to
        // every mutation in the battery, which is the one arm that most needs to see them.
        const EncodeView v = ViewFor(f, mut);
        if (FieldRefusal(v.spec)) continue;
        for (int trial = 0; trial < 4096; ++trial) {
            for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<uint8_t>(next() >> 24);
            BitReader r(buf.data(), buf.size());
            o1ref::Cursor c{buf.data(), buf.size(), 0, false};
            ++n;
            if (f.spec.cls == FieldClass::WireQ) {
                const float mine = r.UnpackWire(v.spec);
                const float theirs = o1ref::RefBitPackReal(c, f.spec.min, f.spec.max, f.spec.step);
                if (!rep.Expect(r.Refused() == c.bad,
                                Cat({"A6 ", f.name, ": refusal disagrees with the reference decoder"}))) break;
                if (!r.Refused() && memcmp(&mine, &theirs, sizeof mine) != 0) {
                    rep.Fail(Cat({"A6 ", f.name, ": value disagrees with the reference decoder"}));
                    break;
                }
            } else {
                float mine[4], theirs[4];
                r.UnpackQuat10(v.spec, mine);
                o1ref::RefBitPackQuat(c, f.spec.rawBits, theirs);
                if (r.Refused() || c.bad) continue;
                bool same = true;
                for (int k = 0; k < 4; ++k) {
                    // A component rebuilt from a negative remainder is NaN on both sides, and NaN never
                    // compares equal, so compare bit patterns.
                    uint32_t a, b;
                    memcpy(&a, &mine[k], 4);
                    memcpy(&b, &theirs[k], 4);
                    if (a != b) same = false;
                }
                if (!rep.Expect(same, Cat({"A6 ", f.name, ": quaternion disagrees with the reference decoder"}))) break;
            }
        }
    }
    return n;
}

// ---------------------------------------------------------------- A7 stored corpus

struct CorpusRow {
    std::string symbol;
    std::string what;
    long long   startBits = 0;   // where the record begins inside the message body
    long long   records = 0;
    std::vector<uint8_t> body;
};

std::vector<CorpusRow> LoadCorpus(const std::string& path, std::string& err) {
    std::vector<CorpusRow> out;
    std::ifstream in(path);
    if (!in) { err = "cannot open " + path; return out; }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        CorpusRow r;
        std::string hex;
        if (!(ss >> r.symbol >> r.what >> r.startBits >> r.records >> hex)) {
            err = "malformed row: " + line;
            return {};
        }
        if (r.startBits % 8 != 0) { err = "record start is not byte-aligned in " + r.symbol; return {}; }
        if (hex.size() % 2) { err = "odd hex length in " + r.symbol; return {}; }
        for (size_t i = 0; i < hex.size(); i += 2) {
            r.body.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
        }
        out.push_back(std::move(r));
    }
    return out;
}

// Decode a record with the PC schedule and re-encode it. Decoding always uses the pristine spec; only the
// re-encode sees the mutation, which is what makes a mutation visible to a round trip at all.
bool RunSchedule(const std::vector<Field>& schedule, const CorpusRow& row, const Mutation& mut, BitVec& out,
                 std::string& why) {
    const size_t skip = static_cast<size_t>(row.startBits) / 8;
    if (skip >= row.body.size()) { why = "record start is past the end of the body"; return false; }
    BitReader r(row.body.data() + skip, row.body.size() - skip);
    for (const Field& f : schedule) {
        const EncodeView v = ViewFor(f, mut);
        const unsigned reps = f.vector3 ? 3u : 1u;
        for (unsigned k = 0; k < reps; ++k) {
            switch (f.spec.cls) {
                case FieldClass::Raw: {
                    const uint64_t x = r.UnpackRaw(f.spec);
                    if (r.Refused() || r.Overrun()) { why = "raw " + f.name; return false; }
                    out.Put(x, f.spec.rawBits);
                    break;
                }
                case FieldClass::WireQ: {
                    const float value = r.UnpackWire(f.spec);
                    if (r.Refused() || r.Overrun()) { why = "wire " + f.name; return false; }
                    uint64_t idx = 0;
                    if (QuantReal(value, v.spec.min, v.spec.max, v.spec.step, idx) != WireStatus::Ok) {
                        why = "requantise " + f.name;
                        return false;
                    }
                    out.Put(idx, v.bits);
                    break;
                }
                case FieldClass::WireQuat: {
                    float q[4];
                    r.UnpackQuat10(f.spec, q);
                    if (r.Refused() || r.Overrun()) { why = "quat " + f.name; return false; }
                    Quat10 p;
                    if (QuantQuat(q, v.spec.rawBits, p) != WireStatus::Ok) { why = "requantise " + f.name; return false; }
                    out.Put(v.flipSelector ? (3u - p.selector) : p.selector, 2);
                    for (int i = 0; i < 3; ++i) out.Put(p.component[i], v.bits);
                    break;
                }
                default:
                    why = "unexpected class on " + f.name;
                    return false;
            }
        }
    }
    if (mut.kind == MutKind::BitOrderFlip) out.FlipByteOrder();
    return true;
}

struct CorpusResult {
    long long payloads = 0;
    long long roundTripped = 0;
    long long wrongScheduleRejected = 0;
};

CorpusResult ArmCorpus(const std::vector<CorpusRow>& corpus, const std::vector<Field>& player,
                       const std::vector<Field>& sim, const Mutation& mut, Report& rep) {
    CorpusResult res;
    for (const CorpusRow& row : corpus) {
        ++res.payloads;
        const std::vector<Field>& schedule = (row.what == "PlayerState") ? player : sim;
        BitVec out;
        std::string why;
        if (!rep.Expect(RunSchedule(schedule, row, mut, out, why),
                        Cat({"A7 ", row.symbol, " ", row.what, ": the PC schedule refused at ", why}))) {
            ++rep.divergences;
            continue;
        }
        const size_t startBit = static_cast<size_t>(row.startBits);
        bool exact = startBit + out.bits <= row.body.size() * 8;
        for (size_t i = 0; exact && i < out.bits; ++i) {
            const size_t j = startBit + i;
            if (out.Get(i) != (((row.body[j >> 3] >> (j & 7)) & 1u) != 0)) exact = false;
        }
        if (!exact) ++rep.divergences;
        rep.Expect(exact, Cat({"A7 ", row.symbol, " ", row.what,
                               ": encode(decode(bytes)) != bytes over real PC wire bytes"}));
        // Trailing bits that are not zero padding mean we consumed the wrong number of them and merely
        // round-tripped the part we did read.
        bool pad = exact;
        for (size_t i = startBit + out.bits; pad && i < row.body.size() * 8; ++i) {
            if ((row.body[i >> 3] >> (i & 7)) & 1u) pad = false;
        }
        rep.Expect(pad, Cat({"A7 ", row.symbol, " ", row.what, ": ",
                             Num((long long)(row.body.size() * 8) - (long long)(startBit + out.bits)),
                             " trailing bits are not zero padding"}));
        if (exact && pad) ++res.roundTripped;
    }

    // ★ The negative control, and it is here on purpose rather than by accident.
    //
    // An earlier version of this arm asserted that the SimState schedule REFUSED sixteen payloads. It did
    // refuse them -- because the harness started the decode 64 bits early -- and the green assertion was
    // the reason the defect survived: the wrong-symbol reading kept the test passing while the right one
    // would have meant sixteen broken decodes. Now that every payload round-trips, nothing would notice a
    // schedule that had stopped rejecting anything, so the rejection is exercised deliberately: each
    // payload is run through the OTHER symbol's schedule and must fail.
    for (const CorpusRow& row : corpus) {
        const std::vector<Field>& wrong = (row.what == "PlayerState") ? sim : player;
        BitVec out;
        std::string why;
        const Mutation none;
        bool rejected = !RunSchedule(wrong, row, none, out, why);
        if (!rejected) {
            // It parsed. It must still not reproduce the bytes, or the two schedules are indistinguishable.
            const size_t startBit = static_cast<size_t>(row.startBits);
            bool same = startBit + out.bits <= row.body.size() * 8;
            for (size_t i = 0; same && i < out.bits; ++i) {
                const size_t j = startBit + i;
                if (out.Get(i) != (((row.body[j >> 3] >> (j & 7)) & 1u) != 0)) same = false;
            }
            rejected = !same;
        }
        rep.Expect(rejected, Cat({"A7 negative control ", row.symbol,
                                  ": the other symbol's schedule reproduced these bytes, so the arm "
                                  "cannot tell the two layouts apart"}));
        if (rejected) ++res.wrongScheduleRejected;
    }
    return res;
}

// ---------------------------------------------------------------- driver

struct Options {
    std::string corpusPath;
    std::string dropField;
    std::string mutation;
    bool        listMutations = false;
    bool        fast = false;
    unsigned    quatBits = 10;
};

int Run(const Options& opt) {
    Report rep;
    const std::vector<Field> all = ShippedFields(opt.dropField);

    if (opt.listMutations) {
        for (const Mutation& m : GenerateMutations(all)) printf("%s\n", m.id.c_str());
        return 0;
    }

    Mutation mut;
    if (!opt.mutation.empty()) {
        bool found = false;
        for (const Mutation& m : GenerateMutations(all)) {
            if (m.id == opt.mutation) { mut = m; found = true; break; }
        }
        if (!found) {
            printf("O1-RESULT status=FAIL checked=0 failures=1 divergences=0 mutation=unknown:%s\n",
                   opt.mutation.c_str());
            return 2;
        }
    }

    std::vector<Field> sim, player;
    for (const Field& f : all) {
        (f.name.rfind("sim.", 0) == 0 ? sim : player).push_back(f);
    }

    std::string err;
    const std::vector<CorpusRow> corpus = LoadCorpus(opt.corpusPath, err);
    if (corpus.empty()) {
        printf("O1-RESULT status=FAIL checked=0 failures=1 divergences=0 reason=corpus:%s\n",
               err.empty() ? "empty" : err.c_str());
        return 2;
    }

    const long long framed = ArmFraming(all, mut, rep);
    const SweepStats sweep = ArmWireSweep(all, mut, rep);
    const GridStats grid = ArmGrid(all, rep);
    const long long refusals = ArmRefusal(rep);
    const long long refChecks = ArmReference(all, mut, rep);
    const CorpusResult cor = ArmCorpus(corpus, player, sim, mut, rep);

    // The engine constants the codec computes with, against the table they came from. constcheck.py
    // compares that table with echovr.exe itself; this proves nothing silently dropped them in between.
    long long constants = 0;
    for (size_t i = 0; i < kEngineConstantCount; ++i) {
        const float f = EngineConst(i);
        uint32_t bits32;
        memcpy(&bits32, &f, sizeof bits32);
        rep.Expect(bits32 == kEngineConstants[i].bits,
                   Cat({"C ", kEngineConstants[i].id, ": compiled ", Hex32(bits32), " != table ",
                        Hex32(kEngineConstants[i].bits)}));
        ++constants;
    }

    QuatStats quat;
    F32Stats f32s;
    long long quatSpot = 0;
    long long quatProbe = 0;
    if (!opt.fast) {
        ArmQuatSweep(opt.quatBits, quat);
        quatSpot = ArmQuatSpot(opt.quatBits, 4099, rep);
        quatProbe = ArmQuatProbe(opt.quatBits, rep);
        rep.Expect(quat.mismatch.load() == 0,
                   Cat({"A3 ", Num(quat.mismatch.load()), " reachable quaternion codes of ",
                        Num(quat.total.load()), " do not survive quant(dequant(code))"}));
        ArmF32(f32s);
        rep.Expect(f32s.bad.load() == 0,
                   Cat({"A4 ", Num(f32s.bad.load()), " f32 patterns or delta codes did not round-trip"}));
    }

    rep.checked = framed + refusals + cor.payloads + constants +
                  static_cast<long long>(kWireConstantCount);

    printf("O1-COVER wire_fields=%lld\n", framed);
    printf("O1-COVER wire_indices_swept=%lld out_of_domain_refused=%lld engine_writer_rejects=%lld\n",
           sweep.indices, sweep.outOfDomainRefused, sweep.engineWriterRejects);
    printf("O1-COVER quat_codes=%lld reachable=%lld unreachable_nan=%lld unreachable_selector=%lld "
           "exact=%lld mismatch=%lld spot=%lld\n",
           quat.total.load(), quat.exact.load() + quat.mismatch.load(),
           quat.unreachable.load() - quat.selectorNotArgmax.load(), quat.selectorNotArgmax.load(),
           quat.exact.load(), quat.mismatch.load(), quatSpot);
    printf("O1-COVER quat_probe_mismatches=%lld\n", quatProbe);
    printf("O1-COVER f32_patterns=%lld f32_delta_codes=%lld\n", f32s.patterns.load(), f32s.deltas.load());
    printf("O1-COVER refusal_cases=%lld reference_comparisons=%lld\n", refusals, refChecks);
    printf("O1-COVER grid_on_grid_accepted=%lld off_grid_refused=%lld neighbour_moved=%lld\n",
           grid.accepted, grid.refusedOffGrid, grid.neighbourMoved);
    printf("O1-COVER corpus_distinct_payloads=%lld round_tripped=%lld wrong_schedule_rejected=%lld\n",
           cor.payloads, cor.roundTripped, cor.wrongScheduleRejected);

    // ★ Whether a mutation could change ANY encoded bit, decided from the arithmetic rather than from an
    // arm staying quiet.  "No arm failed" and "there was nothing to fail on" are different statements, and
    // reporting the second as the first is how a battery gets a clean number it has not earned.
    //
    // For a WIRE_Q field `max` reaches the encoding through exactly one expression -- the step count
    // ceil((max-min) * (1/step)) that sets the field width.  It does not appear in the index formula
    // floor((v-min)*(1/step) + 0.5), and the storage quantiser's only acceptance test is index <= steps.
    // So an unchanged step count and an unchanged width PROVE the output is bit-identical and the battery
    // may call the mutation equivalent.  Anything else has to turn an arm red.
    long long stepsBefore = -1, stepsAfter = -1, widthBefore = -1, widthAfter = -1;
    for (const Field& f : all) {
        if (f.name != mut.field || f.spec.cls != FieldClass::WireQ) continue;
        const EncodeView v = ViewFor(f, mut);
        stepsBefore = static_cast<long long>(RealStepsWriter(f.spec.min, f.spec.max, f.spec.step));
        stepsAfter = static_cast<long long>(RealStepsWriter(v.spec.min, v.spec.max, v.spec.step));
        widthBefore = WireFieldBits(f.spec);
        widthAfter = v.bits;
    }
    printf("O1-COVER mutation_steps=%lld->%lld mutation_width=%lld->%lld\n",
           stepsBefore, stepsAfter, widthBefore, widthAfter);

    for (const std::string& m : rep.messages) printf("O1-FAIL %s\n", m.c_str());
    printf("O1-RESULT status=%s checked=%lld failures=%lld divergences=%lld mutation=%s\n",
           rep.failures ? "FAIL" : "PASS", rep.checked, rep.failures, rep.divergences,
           mut.kind == MutKind::None ? "none" : mut.id.c_str());
    return rep.failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&a](const char* key) -> std::string {
            const std::string k = std::string(key) + "=";
            return a.rfind(k, 0) == 0 ? a.substr(k.size()) : std::string();
        };
        if (!value("--corpus").empty()) opt.corpusPath = value("--corpus");
        else if (!value("--drop-field").empty()) opt.dropField = value("--drop-field");
        else if (!value("--mutate").empty()) opt.mutation = value("--mutate");
        else if (a == "--list-mutations") opt.listMutations = true;
        else if (a == "--fast") opt.fast = true;
        else {
            fprintf(stderr, "o1: unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    if (opt.corpusPath.empty()) {
        fprintf(stderr, "o1: --corpus=<path to arena_idle.tsv> is required\n");
        return 2;
    }
    return Run(opt);
}
