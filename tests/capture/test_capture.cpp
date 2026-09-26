// W2-A's offline gate: the codec, the completeness accounting, and the DUV parse.
//
// ---------------------------------------------------------------------------------------------
// ★ WHAT THIS SUITE COVERS, WHAT IT REFUSES, AND WHAT IT DOES NOT COVER.
//
// COVERS   every record shape round-trips bit-exactly, including the values that break naive codecs
//          (NaN, +/-0, denormals, +/-inf, the top and bottom of every quantisation domain); the block
//          bit-length agreement that turns a one-bit desync into a refusal; the DUV parse against the
//          real message layout; and the per-entity frame accounting that S5.6.2's `observed` rests on.
// REFUSES  a schema table that does not validate, and any round trip that is merely "close".
// DOES NOT prove our field schedule matches the ENGINE's. O1 does not either -- the design is
// COVER    explicit that W2-I's O2 is what establishes engine agreement. A round trip through one
//          codec cannot discover that both halves read the binary the same wrong way.
//
// ★ AND ONE THING THAT IS DELIBERATELY NOT ASSERTED HERE: that a capture is complete. This suite can
// show the DETECTOR fires; only a real match shows a real file. check.ps1 carries the file-level arm.
// ---------------------------------------------------------------------------------------------

#include "doctest.h"

#include <windows.h>

#include <string.h>

#include <cmath>
#include <string>
#include <vector>

#include "capture/chapterbuilder.h"
#include "capture/gather.h"
#include "capture/costmeter.h"
#include "capture/completeness.h"
#include "capture/duv.h"
#include "capture/identity_emit.h"
#include "capture/capture_facts.h"
#include "generated/facts_generated.h"
#include "capture/keyframe.h"
#include "foundation/demofile/format.h"
#include "foundation/demofile/manifest_io.h"
#include "capture/identity_block.h"
#include "capture/manifestbuild.h"
#include "capture/markers.h"
#include "capture/preopen.h"
#include "capture/ringset.h"
#include "capture/streamcodec.h"
#include "foundation/bitio/enginewire.h"
#include "foundation/bitio/wirefields.h"

using namespace replay;
using namespace replay::capture;

namespace {

// Bit-exact, not "close". memcmp rather than == so that -0.0 does not compare equal to +0.0 and a NaN
// compares equal to the identical NaN -- both of which a float comparison gets wrong in the direction
// that hides a loss.
bool SameF(float a, float b) { return memcmp(&a, &b, sizeof a) == 0; }
bool SameV3(const float a[3], const float b[3]) { return memcmp(a, b, 3 * sizeof(float)) == 0; }
bool SameV4(const float a[4], const float b[4]) { return memcmp(a, b, 4 * sizeof(float)) == 0; }

float Bits(uint32_t u) {
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

// A value on the wire grid for a given field: the only thing PackWire is allowed to accept, and the
// encoder refuses anything else rather than rounding it. Building test inputs any other way would be
// testing the refusal instead of the round trip.
float OnGrid(const bitio::WireField& f, uint64_t index) {
    return bitio::DequantReal(index, f.min, f.step);
}

uint64_t MaxIndex(const bitio::WireField& f) {
    return bitio::RealStepsWriter(f.min, f.max, f.step);
}

// A quaternion that survives quat10 exactly: normalise, quantise, and take what comes back. The
// smallest-three codec reconstructs the dropped component from a square root, so it is defined only on
// the unit sphere -- an unnormalised input is refused, which is the codec being right rather than
// awkward.
void GridQuat(float q[4], unsigned bits, float x, float y, float z, float w) {
    const float n = std::sqrt(x * x + y * y + z * z + w * w);
    const float in[4] = {x / n, y / n, z / n, w / n};
    bitio::Quat10 packed{};
    REQUIRE(bitio::QuantQuat(in, bits, packed) == bitio::WireStatus::Ok);
    REQUIRE(bitio::DequantQuat(packed, bits, q) == bitio::WireStatus::Ok);
}

PlayerFrame MakeWirePlayer(uint64_t seed) {
    const bitio::WireField* W = bitio::kPlayerStateWire;
    PlayerFrame f{};
    f.hasDup = true;
    f.fill = kFillWireAll;
    GridQuat(f.hmdOri, W[0].bits, 0.1f + 0.01f * float(seed), 0.2f, 0.3f, 0.9f);
    GridQuat(f.lHandOri, W[1].bits, 0.4f, 0.1f * float(seed % 5), 0.2f, 0.8f);
    GridQuat(f.rHandOri, W[2].bits, -0.3f, 0.5f, 0.1f, 0.7f);
    for (int i = 0; i < 3; ++i) {
        f.refVel[i] = OnGrid(W[3], (seed * 7 + uint64_t(i) * 13) % (MaxIndex(W[3]) + 1));
        f.refVelDup[i] = OnGrid(W[17], (seed * 11 + uint64_t(i)) % (MaxIndex(W[17]) + 1));
        f.lHandOff[i] = OnGrid(W[12], (seed * 3 + uint64_t(i) * 5) % (MaxIndex(W[12]) + 1));
        f.rHandOff[i] = OnGrid(W[13], (seed * 5 + uint64_t(i) * 3) % (MaxIndex(W[13]) + 1));
    }
    f.bendBack = OnGrid(W[9], seed % (MaxIndex(W[9]) + 1));
    f.bendFwd = OnGrid(W[10], (seed * 2) % (MaxIndex(W[10]) + 1));
    f.restPoseHeight = OnGrid(W[11], (seed * 4) % (MaxIndex(W[11]) + 1));
    f.trigL = OnGrid(W[14], seed % (MaxIndex(W[14]) + 1));
    f.trigR = OnGrid(W[15], (seed + 1) % (MaxIndex(W[15]) + 1));
    f.bits28 = static_cast<uint32_t>(seed * 2654435761u) & 0x0FFFFFFFu;
    f.holdLSlot = static_cast<uint32_t>(seed % 0xFFFFu);
    f.holdLGen = static_cast<uint32_t>((seed / 3) % 0xFFFFu);
    f.holdRSlot = static_cast<uint32_t>((seed / 5) % 0xFFFFu);
    f.holdRGen = static_cast<uint32_t>((seed / 7) % 0xFFFFu);
    f.anchor = static_cast<uint32_t>(seed % 8);
    f.hdr0 = 0xA1B2C3D400000000ull | seed;
    return f;
}

PlayerFrame MakeSampledPlayer(uint64_t seed) {
    // A sampled record is F32-class, so nothing has to be on a grid -- which is exactly why the awkward
    // values belong here. Off-grid values are what the WIRE path is required to REFUSE, and the two
    // behaviours are checked separately below.
    PlayerFrame f{};
    f.hasDup = false;
    // A sampled row on this build: pose, refvel and body are sourced; trigger and anchor are not.
    f.fill = kFillPose | kFillRefVel | kFillBody | kFillActionWord | kFillHolding;
    const float odd[8] = {0.0f,          -0.0f,          Bits(0x7F800000),  Bits(0xFF800000),
                          Bits(0x7FC00001), Bits(0x00000001), Bits(0x807FFFFF), 1.1754944e-38f};
    for (int i = 0; i < 4; ++i) {
        f.hmdOri[i] = odd[(seed + uint64_t(i)) % 8];
        f.lHandOri[i] = odd[(seed + uint64_t(i) + 3) % 8];
        f.rHandOri[i] = odd[(seed + uint64_t(i) + 5) % 8];
    }
    for (int i = 0; i < 3; ++i) {
        f.refVel[i] = odd[(seed + uint64_t(i) + 1) % 8];
        f.lHandOff[i] = odd[(seed + uint64_t(i) + 2) % 8];
        f.rHandOff[i] = odd[(seed + uint64_t(i) + 6) % 8];
    }
    f.bendBack = odd[seed % 8];
    f.bendFwd = odd[(seed + 1) % 8];
    f.restPoseHeight = odd[(seed + 2) % 8];
    f.trigL = odd[(seed + 3) % 8];
    f.trigR = odd[(seed + 4) % 8];
    f.bits28 = static_cast<uint32_t>(seed * 40503u) & 0x0FFFFFFFu;
    f.holdLSlot = static_cast<uint32_t>(seed % 0xFFFFu);
    f.holdLGen = 0;
    f.holdRSlot = 0xFFFFu;
    f.holdRGen = static_cast<uint32_t>(seed % 17);
    f.anchor = static_cast<uint32_t>(seed % 8);
    return f;
}

BodyFrame MakeWireBody(uint64_t seed) {
    const bitio::WireField* W = bitio::kSimStateWire;
    BodyFrame f{};
    f.hasHeader = true;
    f.fill = kFillBodyWireAll;
    f.bits11 = static_cast<uint32_t>(seed % 2048);
    f.idValue = 0x0000000100000000ull | seed;
    f.idTag = 1;
    for (int i = 0; i < 3; ++i) {
        f.com[i] = OnGrid(W[2], (seed * 977 + uint64_t(i) * 31) % (MaxIndex(W[2]) + 1));
        f.vel[i] = OnGrid(W[4], (seed * 13 + uint64_t(i)) % (MaxIndex(W[4]) + 1));
        f.angVel[i] = OnGrid(W[5], (seed * 3 + uint64_t(i)) % (MaxIndex(W[5]) + 1));
        f.lastHeldCom[i] = OnGrid(W[6], (seed * 17 + uint64_t(i)) % (MaxIndex(W[6]) + 1));
    }
    GridQuat(f.ori, W[3].bits, 0.2f, -0.1f, 0.4f, 0.88f);
    f.hdr0 = 0xFFFFFFFF428EBC4Bull;
    f.hdr2 = 0x50;
    return f;
}

bool SamePlayer(const PlayerFrame& a, const PlayerFrame& b, bool wire) {
    if (!SameV4(a.hmdOri, b.hmdOri) || !SameV4(a.lHandOri, b.lHandOri) ||
        !SameV4(a.rHandOri, b.rHandOri)) {
        return false;
    }
    if (!SameV3(a.refVel, b.refVel) || !SameV3(a.lHandOff, b.lHandOff) ||
        !SameV3(a.rHandOff, b.rHandOff)) {
        return false;
    }
    if (!SameF(a.bendBack, b.bendBack) || !SameF(a.bendFwd, b.bendFwd) ||
        !SameF(a.restPoseHeight, b.restPoseHeight) || !SameF(a.trigL, b.trigL) ||
        !SameF(a.trigR, b.trigR)) {
        return false;
    }
    if (a.bits28 != b.bits28 || a.holdLSlot != b.holdLSlot || a.holdLGen != b.holdLGen ||
        a.holdRSlot != b.holdRSlot || a.holdRGen != b.holdRGen || a.anchor != b.anchor) {
        return false;
    }
    if (wire && (!SameV3(a.refVelDup, b.refVelDup) || a.hdr0 != b.hdr0)) {
        return false;
    }
    return a.fill == b.fill;
}

bool SameBody(const BodyFrame& a, const BodyFrame& b, bool wire) {
    if (a.bits11 != b.bits11 || a.idValue != b.idValue || a.idTag != b.idTag) return false;
    if (!SameV3(a.com, b.com) || !SameV4(a.ori, b.ori) || !SameV3(a.vel, b.vel) ||
        !SameV3(a.angVel, b.angVel) || !SameV3(a.lastHeldCom, b.lastHeldCom)) {
        return false;
    }
    if (wire && (a.hdr0 != b.hdr0 || a.hdr2 != b.hdr2)) return false;
    return a.fill == b.fill;
}

}  // namespace

TEST_CASE("the schema table validates itself") {
    // A malformed table must refuse before a file exists, not after one is unreadable.
    CHECK(SchemaTableRefusal() == nullptr);
    CHECK(SchemaCount() > 0);
    for (uint16_t i = 0; i < SchemaCount(); ++i) {
        const SchemaDef& d = Schemas()[i];
        CHECK(d.retention == Retention::All);
        CHECK(d.fieldCount > 0);
        CHECK(d.fields != nullptr);
        const bool sampled = d.id == kSchemaPlayerSampled || d.id == kSchemaBodySampled;
        // The retired schema must not be in the table at all. The id stays reserved; the ENTRY goes.
        CHECK(d.id != kSchemaWorld_RETIRED);
        for (uint16_t k = 0; k < d.fieldCount; ++k) {
            CHECK(bitio::FieldRefusal(d.fields[k]) == nullptr);
            if (sampled) {
                // S5.3.3's static rule, held by construction: the sampled tables are generated from
                // the wire tables by class substitution, so a WireQ field here is impossible rather
                // than merely forbidden.
                CHECK(d.fields[k].cls != bitio::FieldClass::WireQ);
                CHECK(d.fields[k].cls != bitio::FieldClass::WireQuat);
            }
        }
    }
    CHECK(SchemaById(kSchemaPlayerWire) != nullptr);
    CHECK(SchemaById(9999) == nullptr);
}

TEST_CASE("wire player records round-trip bit-exactly, absolute and delta, interleaved") {
    std::vector<uint8_t> buf(1u << 20, 0);
    bitio::BitWriter w(buf.data(), buf.size());
    PlayerEncoder enc(false);

    constexpr int kEntities = 8;
    constexpr int kFrames = 40;
    std::vector<PlayerFrame> sent;
    std::vector<uint16_t> sentEntity;
    std::vector<uint64_t> sentStamp;
    uint64_t stamp = 1700000000000000ull;
    for (int frame = 0; frame < kFrames; ++frame) {
        for (int e = 0; e < kEntities; ++e) {
            const PlayerFrame f = MakeWirePlayer(uint64_t(frame) * 31 + uint64_t(e));
            REQUIRE(enc.Encode(w, uint16_t(e), stamp, f));
            sent.push_back(f);
            sentEntity.push_back(uint16_t(e));
            sentStamp.push_back(stamp);
        }
        stamp += 22727;
    }
    REQUIRE_FALSE(w.Refused());

    bitio::BitReader r(buf.data(), buf.size());
    PlayerDecoder dec(false);
    uint8_t scratch[512];
    for (size_t i = 0; i < sent.size(); ++i) {
        DecodedPlayer got{};
        REQUIRE(dec.Decode(r, got, scratch, sizeof scratch));
        CHECK_FALSE(got.verbatim);
        CHECK(got.entity == sentEntity[i]);
        CHECK(got.stampUs == sentStamp[i]);
        CHECK(SamePlayer(got.frame, sent[i], true));
    }
    // The cursor lands exactly where the encoder left it. A one-bit disagreement anywhere above would
    // move this, which is what makes it a check on the whole block rather than on the last record.
    CHECK(r.BitsRead() == w.BitsWritten());
    CHECK_FALSE(r.Overrun());
}

TEST_CASE("sampled records carry the awkward floats verbatim") {
    std::vector<uint8_t> buf(1u << 20, 0);
    bitio::BitWriter w(buf.data(), buf.size());
    PlayerEncoder enc(true);
    std::vector<PlayerFrame> sent;
    uint64_t stamp = 42;
    for (int i = 0; i < 64; ++i) {
        const PlayerFrame f = MakeSampledPlayer(uint64_t(i));
        REQUIRE(enc.Encode(w, uint16_t(i % 4), stamp, f));
        sent.push_back(f);
        stamp += 11111;
    }
    REQUIRE_FALSE(w.Refused());

    bitio::BitReader r(buf.data(), buf.size());
    PlayerDecoder dec(true);
    uint8_t scratch[512];
    for (size_t i = 0; i < sent.size(); ++i) {
        DecodedPlayer got{};
        REQUIRE(dec.Decode(r, got, scratch, sizeof scratch));
        CHECK(SamePlayer(got.frame, sent[i], false));
        // A sampled record has no second refvelocity and no message header. The decoder must say so
        // rather than hand back a zero that reads like a measurement.
        CHECK_FALSE(got.frame.hasDup);
    }
    CHECK(r.BitsRead() == w.BitsWritten());
}

TEST_CASE("body records round-trip, wire and sampled") {
    for (int pass = 0; pass < 2; ++pass) {
        const bool sampled = pass == 1;
        std::vector<uint8_t> buf(1u << 20, 0);
        bitio::BitWriter w(buf.data(), buf.size());
        BodyEncoder enc(sampled);
        std::vector<BodyFrame> sent;
        uint64_t stamp = 900;
        for (int i = 0; i < 128; ++i) {
            BodyFrame f = MakeWireBody(uint64_t(i));
            if (sampled) {
                f.hasHeader = false;
                // A sampled body on this build: a transform and no identity.
                f.fill = kFillBodyTransform | kFillBodyLastHeld | kFillBodyBits;
                f.idValue = 0;
                f.com[0] = Bits(0x7FC00000);   // a sampled body may hold values no grid contains
                f.vel[1] = -0.0f;
            }
            REQUIRE(enc.Encode(w, uint16_t(i % 6), stamp, f));
            sent.push_back(f);
            stamp += 22727;
        }
        REQUIRE_FALSE(w.Refused());

        bitio::BitReader r(buf.data(), buf.size());
        BodyDecoder dec(sampled);
        uint8_t scratch[512];
        for (size_t i = 0; i < sent.size(); ++i) {
            DecodedBody got{};
            REQUIRE(dec.Decode(r, got, scratch, sizeof scratch));
            CHECK(SameBody(got.frame, sent[i], !sampled));
        }
        CHECK(r.BitsRead() == w.BitsWritten());
    }
}

TEST_CASE("a wire record whose decode cannot be proved is stored whole, not approximated") {
    std::vector<uint8_t> buf(1u << 16, 0);
    bitio::BitWriter w(buf.data(), buf.size());
    PlayerEncoder enc(false);

    uint8_t body[64];
    for (int i = 0; i < 64; ++i) body[i] = static_cast<uint8_t>(i * 7 + 3);
    REQUIRE(enc.EncodeVerbatim(w, 3, 12345, body, sizeof body));
    const PlayerFrame f = MakeWirePlayer(9);
    REQUIRE(enc.Encode(w, 3, 12346, f));
    REQUIRE_FALSE(w.Refused());

    bitio::BitReader r(buf.data(), buf.size());
    PlayerDecoder dec(false);
    uint8_t scratch[512];
    DecodedPlayer a{}, b{};
    REQUIRE(dec.Decode(r, a, scratch, sizeof scratch));
    CHECK(a.verbatim);
    CHECK(a.bodyBytes == sizeof body);
    CHECK(memcmp(a.body, body, sizeof body) == 0);
    CHECK(a.entity == 3);
    CHECK(a.stampUs == 12345);

    uint8_t scratch2[512];
    REQUIRE(dec.Decode(r, b, scratch2, sizeof scratch2));
    CHECK_FALSE(b.verbatim);
    CHECK(SamePlayer(b.frame, f, true));
    CHECK(r.BitsRead() == w.BitsWritten());
}

TEST_CASE("the wire decode proves itself against a real engine-produced body") {
    // A body assembled through the ENGINE MIRROR rather than by hand: the mirror is W1-A's reproduction
    // of CBitWriter, so a body it produces is the shape the engine produces. The decode is then
    // required to reproduce it bit for bit, which is the property the whole wire path rests on.
    const bitio::WireField* W = bitio::kSimStateWire;
    uint8_t body[64] = {};
    const uint64_t hdr0 = 0xFFFFFFFF428EBC4Bull, stamp = 1700000000000000ull, hdr2 = 0x50;
    memcpy(body, &hdr0, 8);
    memcpy(body + 8, &stamp, 8);
    memcpy(body + 16, &hdr2, 8);

    BodyFrame src = MakeWireBody(5);
    bitio::EngineBitWriter ew(body + 24, sizeof body - 24);
    ew.Write(src.bits11, W[0].bits);
    ew.Write(src.idValue, W[1].bits);
    ew.BitPackVec(src.com, W[2].min, W[2].max, W[2].step);
    ew.BitPackQuat(src.ori, W[3].bits);
    ew.BitPackVec(src.vel, W[4].min, W[4].max, W[4].step);
    ew.BitPackVec(src.angVel, W[5].min, W[5].max, W[5].step);
    ew.BitPackVec(src.lastHeldCom, W[6].min, W[6].max, W[6].step);
    ew.Finish();
    REQUIRE(ew.Status() == bitio::WireStatus::Ok);
    CHECK(ew.BitsWritten() == 284);

    BodyFrame got{};
    CHECK(DecodeBodyBody(body, sizeof body, got) == WireDecode::Ok);
    // Field by field, so a failure names the field instead of saying "false". A whole-struct compare
    // is the wrong assertion for a codec: it tells you something moved and not what.
    CHECK(got.bits11 == src.bits11);
    CHECK(got.idValue == src.idValue);
    CHECK(SameV3(got.com, src.com));
    CHECK(SameV4(got.ori, src.ori));
    CHECK(SameV3(got.vel, src.vel));
    CHECK(SameV3(got.angVel, src.angVel));
    CHECK(SameV3(got.lastHeldCom, src.lastHeldCom));
    CHECK(got.hdr0 == hdr0);
    CHECK(got.hdr2 == hdr2);

    SUBCASE("a body whose padding is not zero is refused rather than decoded") {
        // W1-A accepted a record offset only if every bit after the record was zero. Flipping one of
        // those bits means the offset is not what we think, and the record must go in whole.
        uint8_t dirty[64];
        memcpy(dirty, body, sizeof dirty);
        dirty[sizeof dirty - 1] = 0x80;
        BodyFrame f{};
        CHECK(DecodeBodyBody(dirty, sizeof dirty, f) == WireDecode::PaddingNonZero);
    }
    SUBCASE("a truncated body is refused") {
        BodyFrame f{};
        CHECK(DecodeBodyBody(body, 20, f) == WireDecode::TooShort);
    }
}

TEST_CASE("the player wire decode proves itself against a real engine-produced body") {
    const bitio::WireField* W = bitio::kPlayerStateWire;
    uint8_t body[64] = {};
    const uint64_t hdr0 = 0x0123456789ABCDEFull, stamp = 1700000000000000ull;
    memcpy(body, &hdr0, 8);
    memcpy(body + 8, &stamp, 8);

    const PlayerFrame src = MakeWirePlayer(11);
    bitio::EngineBitWriter ew(body + 16, sizeof body - 16);
    ew.BitPackQuat(src.hmdOri, W[0].bits);
    ew.BitPackQuat(src.lHandOri, W[1].bits);
    ew.BitPackQuat(src.rHandOri, W[2].bits);
    ew.BitPackVec(src.refVel, W[3].min, W[3].max, W[3].step);
    ew.Write(src.bits28, W[4].bits);
    ew.Write(src.holdLSlot, W[5].bits);
    ew.Write(src.holdLGen, W[6].bits);
    ew.Write(src.holdRSlot, W[7].bits);
    ew.Write(src.holdRGen, W[8].bits);
    ew.BitPackReal(src.bendBack, W[9].min, W[9].max, W[9].step);
    ew.BitPackReal(src.bendFwd, W[10].min, W[10].max, W[10].step);
    ew.BitPackReal(src.restPoseHeight, W[11].min, W[11].max, W[11].step);
    ew.BitPackVec(src.lHandOff, W[12].min, W[12].max, W[12].step);
    ew.BitPackVec(src.rHandOff, W[13].min, W[13].max, W[13].step);
    ew.BitPackReal(src.trigL, W[14].min, W[14].max, W[14].step);
    ew.BitPackReal(src.trigR, W[15].min, W[15].max, W[15].step);
    ew.Write(src.anchor, W[16].bits);
    ew.BitPackVec(src.refVelDup, W[17].min, W[17].max, W[17].step);
    ew.Finish();
    REQUIRE(ew.Status() == bitio::WireStatus::Ok);
    CHECK(ew.BitsWritten() == 370);

    PlayerFrame got{};
    CHECK(DecodePlayerBody(body, sizeof body, got) == WireDecode::Ok);
    CHECK(SameV4(got.hmdOri, src.hmdOri));
    CHECK(SameV4(got.lHandOri, src.lHandOri));
    CHECK(SameV4(got.rHandOri, src.rHandOri));
    CHECK(SameV3(got.refVel, src.refVel));
    CHECK(got.bits28 == src.bits28);
    CHECK(got.holdLSlot == src.holdLSlot);
    CHECK(got.holdLGen == src.holdLGen);
    CHECK(got.holdRSlot == src.holdRSlot);
    CHECK(got.holdRGen == src.holdRGen);
    CHECK(SameF(got.bendBack, src.bendBack));
    CHECK(SameF(got.bendFwd, src.bendFwd));
    CHECK(SameF(got.restPoseHeight, src.restPoseHeight));
    CHECK(SameV3(got.lHandOff, src.lHandOff));
    CHECK(SameV3(got.rHandOff, src.rHandOff));
    CHECK(SameF(got.trigL, src.trigL));
    CHECK(SameF(got.trigR, src.trigR));
    CHECK(got.anchor == src.anchor);
    CHECK(SameV3(got.refVelDup, src.refVelDup));
    CHECK(got.hdr0 == hdr0);
}

TEST_CASE("tick, netvar, event and as-seen records round-trip") {
    std::vector<uint8_t> buf(1u << 20, 0);

    SUBCASE("tick") {
        bitio::BitWriter w(buf.data(), buf.size());
        TickEncoder enc;
        std::vector<TickRec> sent;
        TickRec t{};
        t.sourceTimeUs = 1700000000000000ull;
        t.simTimeUs = 500000;
        t.peerTimeUs = 1700000000000000ull;
        t.frameIndex = 1000;
        for (int i = 0; i < 200; ++i) {
            t.sourceTimeUs += 11111;
            t.simTimeUs += 11111;
            t.peerTimeUs += 11111;
            t.frameIndex += 1;
            t.scaledDeltaUs = 4000 + uint32_t(i % 7);
            t.frameDeltaUs = 11111;
            t.asSeenValid = 0x1F;
            REQUIRE(enc.Encode(w, t));
            sent.push_back(t);
        }
        bitio::BitReader r(buf.data(), buf.size());
        TickDecoder dec;
        for (const TickRec& want : sent) {
            TickRec got{};
            REQUIRE(dec.Decode(r, got));
            CHECK(memcmp(&got, &want, sizeof got) == 0);
        }
        CHECK(r.BitsRead() == w.BitsWritten());
    }

    SUBCASE("netvar, including the two clock ramps every sample of which is stored") {
        bitio::BitWriter w(buf.data(), buf.size());
        NetvarEncoder enc;
        std::vector<NetvarMessage> sent;
        uint32_t seq = 118;
        float timer = 180.0f;
        for (int i = 0; i < 400; ++i) {
            NetvarMessage m{};
            m.owner = 0x576ed3f8428ebc4bull;
            m.slot = 0;
            m.gen = 0;
            m.pktSeq = seq++;
            m.hasRowset = true;
            m.rowsetSeq = m.pktSeq;
            m.varCount = 1;
            m.rows[0].name = 0xB2996AD484F1D1E8ull;   // game_timer
            m.rows[0].slot = 0;
            timer -= 0.017f;
            uint32_t bits;
            memcpy(&bits, &timer, sizeof bits);
            m.rows[0].value = 0x0000021200000000ull | bits;
            REQUIRE(enc.Encode(w, m));
            sent.push_back(m);
        }
        // A header-only message: seen once in the real capture, and it must not be mistaken for
        // corruption or invented into a rowset of zero rows.
        NetvarMessage bare{};
        bare.owner = 0x576ed3f8428ebc4bull;
        bare.slot = 1;
        bare.pktSeq = seq++;
        bare.hasRowset = false;
        REQUIRE(enc.Encode(w, bare));
        sent.push_back(bare);

        bitio::BitReader r(buf.data(), buf.size());
        NetvarDecoder dec(true);
        for (const NetvarMessage& want : sent) {
            NetvarMessage got{};
            REQUIRE(dec.Decode(r, got));
            CHECK(got.owner == want.owner);
            CHECK(got.slot == want.slot);
            CHECK(got.gen == want.gen);
            CHECK(got.pktSeq == want.pktSeq);
            CHECK(got.hasRowset == want.hasRowset);
            CHECK(got.varCount == want.varCount);
            for (uint16_t k = 0; k < want.varCount; ++k) {
                CHECK(got.rows[k].name == want.rows[k].name);
                CHECK(got.rows[k].slot == want.rows[k].slot);
                // The raw SValue qword, bit for bit. The client's own getter branches on the ENCODING,
                // so a re-encoded equivalent is a different value to the game (S5.4.5 rule 2).
                CHECK(got.rows[k].value == want.rows[k].value);
            }
        }
        CHECK(r.BitsRead() == w.BitsWritten());
    }

    SUBCASE("event bodies are stored verbatim") {
        bitio::BitWriter w(buf.data(), buf.size());
        EventEncoder enc;
        std::vector<std::vector<uint8_t>> bodies;
        uint64_t stamp = 1700000000000000ull;
        for (int i = 0; i < 50; ++i) {
            std::vector<uint8_t> b(size_t(8 + i * 3));
            for (size_t k = 0; k < b.size(); ++k) b[k] = static_cast<uint8_t>(k * 13 + i);
            EventMessage e{};
            e.stampUs = stamp;
            e.dst = 0xFFFFFFFFFFFFFFFFull - uint64_t(i);
            e.typeIndex = static_cast<uint16_t>(i % 7);
            e.bodyBytes = static_cast<uint16_t>(b.size());
            e.body = b.data();
            REQUIRE(enc.Encode(w, e));
            bodies.push_back(b);
            stamp += 34567;
        }
        bitio::BitReader r(buf.data(), buf.size());
        EventDecoder dec;
        uint8_t bodyBuf[1024];
        stamp = 1700000000000000ull;
        for (size_t i = 0; i < bodies.size(); ++i) {
            EventMessage got{};
            REQUIRE(dec.Decode(r, got, bodyBuf, sizeof bodyBuf));
            CHECK(got.stampUs == stamp);
            CHECK(got.bodyBytes == bodies[i].size());
            CHECK(memcmp(got.body, bodies[i].data(), bodies[i].size()) == 0);
            stamp += 34567;
        }
        CHECK(r.BitsRead() == w.BitsWritten());
    }

    SUBCASE("as-seen, including the validity mask that keeps an absent field from reading as zero") {
        bitio::BitWriter w(buf.data(), buf.size());
        AsSeenEncoder enc;
        std::vector<AsSeenFrame> sent;
        for (int i = 0; i < 100; ++i) {
            AsSeenFrame f{};
            f.sourceTimeUs = 1700000000000000ull + uint64_t(i) * 11111;
            f.frameIndex = 900 + uint32_t(i);
            f.sample.peerTimeUs = f.sourceTimeUs;
            f.sample.serverTimeEstUs = 0;
            f.sample.serverTimeSlew = 0;
            f.sample.renderDelaySec = -0.04444f;
            f.sample.lodThrottleEnable = 1;
            f.sample.lodPerFrameBudget = 3;
            f.sample.lodCursor = uint32_t(i % 8);
            f.sample.valid = kAsSeenPeerTime | kAsSeenRenderDelay | kAsSeenLod;
            f.playerCount = 8;
            for (uint16_t k = 0; k < f.playerCount; ++k) {
                f.players[k].peerTimeUs = f.sourceTimeUs - uint64_t(k) * 100;
                f.players[k].rttMs = static_cast<uint16_t>(20 + k);
                f.players[k].occluded = (i + k) % 3 == 0;
                f.players[k].bodyUpdatedThisFrame = (i + k) % 2 == 0;
            }
            REQUIRE(enc.Encode(w, f));
            sent.push_back(f);
        }
        bitio::BitReader r(buf.data(), buf.size());
        AsSeenDecoder dec;
        for (const AsSeenFrame& want : sent) {
            AsSeenFrame got{};
            REQUIRE(dec.Decode(r, got));
            CHECK(got.sourceTimeUs == want.sourceTimeUs);
            CHECK(got.frameIndex == want.frameIndex);
            CHECK(got.sample.valid == want.sample.valid);
            CHECK(got.sample.peerTimeUs == want.sample.peerTimeUs);
            CHECK(SameF(got.sample.renderDelaySec, want.sample.renderDelaySec));
            CHECK(got.sample.lodCursor == want.sample.lodCursor);
            REQUIRE(got.playerCount == want.playerCount);
            for (uint16_t k = 0; k < want.playerCount; ++k) {
                CHECK(got.players[k].peerTimeUs == want.players[k].peerTimeUs);
                CHECK(got.players[k].rttMs == want.players[k].rttMs);
                CHECK(got.players[k].occluded == want.players[k].occluded);
                CHECK(got.players[k].bodyUpdatedThisFrame == want.players[k].bodyUpdatedThisFrame);
            }
        }
        CHECK(r.BitsRead() == w.BitsWritten());
    }
}

// ---------------------------------------------------------------------------------------------
// The DUV parse, against the real message layout
// ---------------------------------------------------------------------------------------------
namespace {

std::vector<uint8_t> BuildDuv(uint64_t owner, uint16_t slot, uint16_t gen, uint32_t seq,
                              const std::vector<DuvRow>& rows, bool withRowset = true) {
    std::vector<uint8_t> b(24, 0);
    memcpy(b.data(), &owner, 8);
    memcpy(b.data() + 8, &slot, 2);
    memcpy(b.data() + 10, &gen, 2);
    const uint32_t one = 1;
    memcpy(b.data() + 12, &one, 4);
    memcpy(b.data() + 16, &seq, 4);
    if (!withRowset) {
        return b;
    }
    b.resize(30);
    memcpy(b.data() + 24, &seq, 4);
    const uint16_t n = static_cast<uint16_t>(rows.size());
    memcpy(b.data() + 28, &n, 2);
    for (const DuvRow& row : rows) {
        const size_t at = b.size();
        b.resize(at + 16);
        memcpy(b.data() + at, &row.name, 8);
        memcpy(b.data() + at + 8, &row.value, 8);
    }
    return b;
}

}  // namespace

TEST_CASE("the DUV parse matches the real message layout") {
    const std::vector<DuvRow> rows = {{0xB2996AD484F1D1E8ull, 0x00000212423019D8ull},
                                      {0x1B10BDC962B47FCCull, 0x00000000FFFFFFFFull}};
    const std::vector<uint8_t> b = BuildDuv(0x576ed3f8428ebc4bull, 0, 0, 0x76, rows);
    CHECK(b.size() == 30 + 16 * rows.size());

    const DuvMessage m = DuvParse(b.data(), b.size());
    REQUIRE(m.status == DuvStatus::Ok);
    CHECK(m.owner == 0x576ed3f8428ebc4bull);
    CHECK(m.slot == 0);
    CHECK(m.reserved12 == 1);
    CHECK(m.pktSeq == 0x76);
    CHECK(m.rowsetSeq == 0x76);
    CHECK(m.varCount == 2);
    CHECK(DuvRowAt(m, 0).name == rows[0].name);
    CHECK(DuvRowAt(m, 0).value == rows[0].value);
    CHECK(DuvRowAt(m, 1).value == rows[1].value);

    SUBCASE("a 24-byte header-only message is legal, not corruption") {
        const std::vector<uint8_t> bare = BuildDuv(1, 1, 0, 0x77, {}, false);
        CHECK(bare.size() == 24);
        const DuvMessage bm = DuvParse(bare.data(), bare.size());
        CHECK(bm.status == DuvStatus::Ok);
        CHECK_FALSE(bm.hasRowset);
        CHECK(bm.pktSeq == 0x77);
    }
    SUBCASE("a truncated rowset is refused rather than read past") {
        std::vector<uint8_t> cut = b;
        cut.resize(cut.size() - 4);
        CHECK(DuvParse(cut.data(), cut.size()).status == DuvStatus::RowsetOversize);
    }
    SUBCASE("SValue kinds are classified, never normalised") {
        CHECK(SValueKindOf(0x00000212423019D8ull) == SValueKind::Scalar);
        CHECK(SValueKindOf(0xFFFF0000FFFF0500ull) == SValueKind::FlagFalse);
        CHECK(SValueKindOf(0xFFFF0000FFFF0501ull) == SValueKind::FlagTrue);
        CHECK(SValueKindOf(0x0000FFFF00000000ull) == SValueKind::Opaque);
        CHECK(SValueKindOf(0xB2996AD399FDC0FFull) == SValueKind::Opaque);
    }
}

TEST_CASE("the DUV sequence tracker names what it lost and invents nothing") {
    DuvSequence seq;
    DuvSequence::Gap gap{};

    auto note = [&](uint32_t s) {
        const std::vector<uint8_t> b = BuildDuv(1, 0, 0, s, {{1, 2}});
        const DuvMessage m = DuvParse(b.data(), b.size());
        return seq.Note(m, &gap);
    };

    CHECK(note(100));
    CHECK(gap.count == 0);
    CHECK(note(101));
    CHECK(gap.count == 0);

    // A jump of four means three packets never reached us, and the RANGE is nameable, which is what
    // lets a reader do the accounting without trusting our counter.
    CHECK(note(105));
    CHECK(gap.firstMissing == 102);
    CHECK(gap.count == 3);
    CHECK(seq.Missing() == 3);
    CHECK(seq.Observed() == seq.Delivered() + 3);

    // A repeated number is the baseline pair sharing one packet: delivered, nothing missing. Counting
    // it as a gap would invent a loss that did not happen.
    CHECK(note(105));
    CHECK(gap.count == 0);
    CHECK(seq.Duplicates() == 1);
    CHECK(seq.Missing() == 3);

    // Backwards is not a gap either. It means the numbering is not what this code believes, and it is
    // counted so the caller can declare the stream unverified instead of reporting a false green.
    CHECK(note(50));
    CHECK(gap.count == 0);
    CHECK(seq.Regressions() == 1);
}

// ---------------------------------------------------------------------------------------------
// The completeness accounting -- the half that must be able to FAIL
// ---------------------------------------------------------------------------------------------

TEST_CASE("a skipped engine frame becomes a named gap, and an absent entity does not") {
    Completeness c;
    c.Configure(kStreamPlayerSampled, ObservedSource::EngineFrameIndex);
    c.BeginChapter(1000);

    // Three entities sampled every frame for ten frames: nothing lost.
    for (uint32_t frame = 1; frame <= 10; ++frame) {
        c.BeginFrameSet(kStreamPlayerSampled, frame);
        for (uint16_t e = 0; e < 3; ++e) {
            c.NoteEntityPresent(kStreamPlayerSampled, e, frame, 1000 + frame);
            c.NoteEntitySampled(kStreamPlayerSampled, e);
        }
        c.EndFrameSet(kStreamPlayerSampled, 0);
    }
    uint32_t gaps = 0;
    c.GapRecords(gaps);
    CHECK(gaps == 0);
    CHECK(c.ChapterObserved(kStreamPlayerSampled) == c.ChapterStored(kStreamPlayerSampled));

    SUBCASE("halving the sample rate is detected -- this is S5.6.2's own mutation") {
        // ★ The mutation must break the SAMPLING, not the counting. Skipping every other frame leaves
        // the frame counter advancing by two, and the accounting names three lost samples per skipped
        // frame. Under a counter sourced from our own loop, observed would equal stored and every gate
        // would stay green while half the samples were gone.
        for (uint32_t frame = 12; frame <= 20; frame += 2) {
            c.BeginFrameSet(kStreamPlayerSampled, frame);
            for (uint16_t e = 0; e < 3; ++e) {
                c.NoteEntityPresent(kStreamPlayerSampled, e, frame, 1000 + frame);
            c.NoteEntitySampled(kStreamPlayerSampled, e);
            }
            c.EndFrameSet(kStreamPlayerSampled, 0);
        }
        uint32_t n = 0;
        const GapRecordDisk* g = c.GapRecords(n);
        CHECK(n > 0);
        uint32_t lost = 0;
        for (uint32_t i = 0; i < n; ++i) {
            CHECK(g[i].streamId == kStreamPlayerSampled);
            CHECK(g[i].count > 0);
            CHECK(g[i].firstMissing != 0);
            lost += g[i].count;
        }
        // Five skipped frames x three entities. The arithmetic still closes, which is the point: the
        // file accounts for every sample the engine numbered.
        CHECK(lost == 15);
        CHECK(c.ChapterObserved(kStreamPlayerSampled) ==
              c.ChapterStored(kStreamPlayerSampled) + lost);
    }

    SUBCASE("an entity that leaves and returns is not reported as loss") {
        // E-C2: absence is absence. Entity 1 is gone for five frames and comes back.
        for (uint32_t frame = 11; frame <= 15; ++frame) {
            c.BeginFrameSet(kStreamPlayerSampled, frame);
            c.NoteEntityPresent(kStreamPlayerSampled, 0, frame, 1000 + frame);
            c.NoteEntitySampled(kStreamPlayerSampled, 0);
            c.NoteEntityPresent(kStreamPlayerSampled, 2, frame, 1000 + frame);
            c.NoteEntitySampled(kStreamPlayerSampled, 2);
            c.EndFrameSet(kStreamPlayerSampled, 0);
        }
        c.BeginFrameSet(kStreamPlayerSampled, 16);
        c.NoteEntityPresent(kStreamPlayerSampled, 0, 16, 1116);
            c.NoteEntitySampled(kStreamPlayerSampled, 0);
        c.NoteEntityPresent(kStreamPlayerSampled, 1, 16, 1116);
            c.NoteEntitySampled(kStreamPlayerSampled, 1);
        c.NoteEntityPresent(kStreamPlayerSampled, 2, 16, 1116);
            c.NoteEntitySampled(kStreamPlayerSampled, 2);
        c.EndFrameSet(kStreamPlayerSampled, 0);

        uint32_t n = 0;
        c.GapRecords(n);
        CHECK(n == 0);
        CHECK(c.ChapterObserved(kStreamPlayerSampled) == c.ChapterStored(kStreamPlayerSampled));
    }
}

TEST_CASE("a stream with no engine source in this role is declared, not reported green") {
    Completeness c;
    // The two host roles originate netvars locally, so there is no wire sequence to count. The design
    // says the completeness of this stream is unverified there; the accounting must say the same.
    c.Configure(kStreamNetvar, ObservedSource::None);
    c.BeginChapter(0);
    c.NoteObserved(kStreamNetvar, 10);
    c.NoteStored(kStreamNetvar, 10);
    StreamId out[8] = {};
    CHECK(c.UnverifiedStreams(out, 8) == 1);
    CHECK(out[0] == kStreamNetvar);
    CHECK(c.SourceOf(kStreamNetvar) == ObservedSource::None);

    // And a stream that DOES have a source is not in the list, even though its counters agree -- which
    // is the distinction the report has to preserve, because agreement and silence look identical.
    c.Configure(kStreamPlayerWire, ObservedSource::TapDelivery);
    c.NoteObserved(kStreamPlayerWire, 5);
    c.NoteStored(kStreamPlayerWire, 5);
    CHECK(c.UnverifiedStreams(out, 8) == 1);
}

TEST_CASE("a dropped record is accounted as dropped, not as a gap") {
    Completeness c;
    c.Configure(kStreamPlayerWire, ObservedSource::TapDelivery);
    c.BeginChapter(0);
    c.NoteObserved(kStreamPlayerWire, 100);
    c.NoteStored(kStreamPlayerWire, 97);
    c.NoteDropped(kStreamPlayerWire, 500, kGapRingFull, 3);

    uint32_t gaps = 0, drops = 0;
    c.GapRecords(gaps);
    const GapRecordDisk* d = c.DroppedRecords(drops);
    CHECK(gaps == 0);
    REQUIRE(drops == 1);
    CHECK(d[0].streamId == kStreamPlayerWire);
    CHECK(d[0].reason == kGapRingFull);
    CHECK(d[0].count == 3);
    // GAP is upstream of us and DROPPED is ours. The file distinguishes them because the two mean
    // different things to somebody deciding whether a capture is trustworthy.
    CHECK(c.ChapterObserved(kStreamPlayerWire) == 103);
}

// ---------------------------------------------------------------------------------------------
// The ring set
// ---------------------------------------------------------------------------------------------

TEST_CASE("the ring set carries records and counts what it could not") {
    std::vector<uint8_t> mem(1u << 16, 0);
    RingSet rings;
    REQUIRE(rings.Init(mem.data(), static_cast<uint32_t>(mem.size())));

    uint8_t rec[64];
    for (int i = 0; i < 64; ++i) rec[i] = static_cast<uint8_t>(i);
    CHECK(rings.Push(rec, sizeof rec));
    CHECK(rings.ProducerCount() == 1);
    CHECK(rings.ThreadIdAt(0) != 0);

    uint8_t got[128];
    uint32_t n = 0;
    CHECK(rings.Pop(got, sizeof got, &n) == sizeof rec);
    CHECK(memcmp(got, rec, sizeof rec) == 0);

    SUBCASE("a full ring refuses whole records and counts them; it never writes half of one") {
        std::vector<uint8_t> big(4096, 0xAB);
        uint64_t pushed = 0;
        while (rings.Push(big.data(), static_cast<uint32_t>(big.size()))) {
            ++pushed;
            if (pushed > 1000) break;
        }
        CHECK(rings.DroppedRecords() > 0);
        // Everything that went in comes out whole.
        std::vector<uint8_t> out(9000);
        uint64_t popped = 0;
        for (;;) {
            uint32_t bytes = 0;
            if (rings.Pop(out.data(), static_cast<uint32_t>(out.size()), &bytes) == 0) break;
            CHECK(bytes == big.size());
            CHECK(memcmp(out.data(), big.data(), big.size()) == 0);
            ++popped;
        }
        CHECK(popped == pushed);
    }
}

// ---------------------------------------------------------------------------------------------
// The tap table and the manifest
// ---------------------------------------------------------------------------------------------

TEST_CASE("the tap table is a closed set the file can state") {
    CHECK(TapTypeCount() > 0);
    CHECK(TapTypeIndex("SR15NetPlayerStateMsg") != 0xFFFFu);
    CHECK(TapTypeIndex("SR15NetReplicatedDeltaUnreliableVars") != 0xFFFFu);
    CHECK(TapTypeIndex("SNSRefreshProfileFromServer") != 0xFFFFu);
    // A name not in the table has no index, and a message we did not register cannot arrive.
    CHECK(TapTypeIndex("SR15NetSomethingNobodyRegistered") == 0xFFFFu);
    CHECK(TapTypeIndex(nullptr) == 0xFFFFu);

    for (uint16_t i = 0; i < TapTypeCount(); ++i) {
        const TapType& t = TapTable()[i];
        CHECK(t.structName != nullptr);
        CHECK(TapTypeIndex(t.structName) == i);
    }
}

TEST_CASE("the identity snapshot carries what the typed manifest row cannot") {
    // W2-C item 3: the engine's alignment is a uint16_t whose 0xFFFF means "no alignment", and
    // PlayerRecord::alignment is a uint8_t. A sentinel that does not fit its field does not fail
    // loudly -- it becomes a valid-looking alignment and every downstream check passes.
    IdentityEntrant e{};
    e.alignment = UINT16_MAX;
    CHECK(e.alignment == UINT16_MAX);
    const uint8_t narrowed = static_cast<uint8_t>(e.alignment);
    CHECK(narrowed == 0xFFu);
    CHECK(narrowed != e.alignment);   // the narrowing this format must not perform

    // W2-C's general residual: where two arrays correspond by position, store the key.
    IdentityLoadoutSlot slots[20] = {};
    for (uint16_t i = 0; i < 20; ++i) {
        slots[i].slotIndex = static_cast<uint16_t>(19 - i);   // deliberately not in array order
        slots[i].itemId = 0x1000ull + i;
    }
    for (uint16_t i = 0; i < 20; ++i) {
        CHECK(slots[i].slotIndex != i);   // position is not the key, which is the whole point
    }
    // W2-C-2 took the record 16 -> 24: the engine holds (key, item) per slot and v1 recorded
    // only the item, so position was the whole of the slot evidence.
    CHECK(sizeof(IdentityLoadoutSlot) == 24);
    CHECK(offsetof(IdentityLoadoutSlot, slotKey) == kIdentitySlotStrideV1);
}

TEST_CASE("the manifest builder rebinds its arena rather than dangling") {
    ManifestBuilder mb;
    mb.Begin(1700000000000000ull, 0, 0x40);
    const uint8_t md5[16] = {1, 2, 3};
    mb.SetBuild(md5, 0xAABBull, 0xCCDDull);
    mb.SetLevel(6300205991959903307ull, 0xF00Dull, 1780177787ull);

    const char* json = "{\"bodytype\":-2076784106321327439}";
    const char* unlocks = "{\"unlocks\":{}}";
    for (int i = 0; i < 8; ++i) {
        PlayerRecord p{};
        p.xplatformId = 0x100ull + uint64_t(i);
        p.userSlot = static_cast<uint16_t>(i);
        p.entrantJson = Blob{reinterpret_cast<const uint8_t*>(json),
                             static_cast<uint32_t>(strlen(json))};
        p.unlocksJson = Blob{reinterpret_cast<const uint8_t*>(unlocks),
                             static_cast<uint32_t>(strlen(unlocks))};
        mb.AddPlayer(p);
    }
    const Manifest& m = mb.View();
    REQUIRE(m.playerCount == 8);
    for (uint16_t i = 0; i < m.playerCount; ++i) {
        REQUIRE(m.players[i].entrantJson.p != nullptr);
        CHECK(memcmp(m.players[i].entrantJson.p, json, strlen(json)) == 0);
        REQUIRE(m.players[i].unlocksJson.p != nullptr);
        CHECK(memcmp(m.players[i].unlocksJson.p, unlocks, strlen(unlocks)) == 0);
    }
    CHECK(m.schemaCount == SchemaCount());
    CHECK(mb.Refusal() == nullptr);
}

// ---------------------------------------------------------------------------------------------
// S5.7's markers, derived from the netvar stream
// ---------------------------------------------------------------------------------------------
namespace {

NetvarMessage OneRow(uint32_t seq, uint64_t name, uint64_t value) {
    NetvarMessage m{};
    m.owner = 0x576ed3f8428ebc4bull;
    m.slot = 0;
    m.pktSeq = seq;
    m.hasRowset = true;
    m.rowsetSeq = seq;
    m.varCount = 1;
    m.rows[0].name = name;
    m.rows[0].value = value;
    m.rows[0].slot = 0;
    return m;
}

uint64_t ScalarOf(uint32_t v) { return 0x0000021200000000ull | v; }

}  // namespace

TEST_CASE("markers are derived from netvar edges, and a first sighting is not an edge") {
    using namespace replay::capture_facts;
    MarkerDeriver d;
    d.Reset();
    MarkRec out[16];
    uint32_t seq = 100;

    // A capture that joined a match already in progress opens with a score. Nobody just scored.
    CHECK(d.Note(OneRow(seq++, kNvBlueScore, ScalarOf(2)), 1000, out, 16) == 0);
    CHECK(d.Note(OneRow(seq++, kNvRedScore, ScalarOf(1)), 1001, out, 16) == 0);
    CHECK(d.GetStats().emitted == 0);

    SUBCASE("a score edge is a goal, and a score that does not move is not") {
        REQUIRE(d.Note(OneRow(seq++, kNvBlueScore, ScalarOf(3)), 2000, out, 16) == 1);
        CHECK(out[0].kind == kMarkGoal);
        CHECK(out[0].index == 0);         // 0 blue, 1 red
        CHECK(out[0].payload == 3);
        CHECK(out[0].tUs == 2000);
        // The same value again is not an edge.
        CHECK(d.Note(OneRow(seq++, kNvBlueScore, ScalarOf(3)), 2100, out, 16) == 0);
        // ★ And a score going DOWN is not a goal. A reset or a correction must not read as one.
        CHECK(d.Note(OneRow(seq++, kNvBlueScore, ScalarOf(2)), 2200, out, 16) == 0);
    }

    SUBCASE("the launch release needs the whole conjunction, not just the one edge") {
        // first_launch alone, with no disc state and no countdown: nothing, because R4's marker is a
        // four-way conjunction and one edge of it also fires on a round that was merely reset.
        CHECK(d.Note(OneRow(seq++, kNvFirstLaunchOfRound, ScalarOf(1)), 3000, out, 16) == 0);
        CHECK(d.Note(OneRow(seq++, kNvFirstLaunchOfRound, ScalarOf(0)), 3001, out, 16) == 0);

        // Now supply the disc and the countdown, and drive the edge again.
        CHECK(d.Note(OneRow(seq++, kNvFrisbeeOwnerId, 0x00000000FFFFFFFFull), 3002, out, 16) == 0);
        CHECK(d.Note(OneRow(seq++, kNvCountdownTimer, ScalarOf(0)), 3003, out, 16) == 0);
        CHECK(d.Note(OneRow(seq++, kNvFirstLaunchOfRound, ScalarOf(1)), 3004, out, 16) == 0);
        const uint32_t n = d.Note(OneRow(seq++, kNvFirstLaunchOfRound, ScalarOf(0)), 3005, out, 16);
        REQUIRE(n == 2);
        CHECK(out[0].kind == kMarkLaunchRelease);
        CHECK(out[1].kind == kMarkRoundBegin);
        CHECK(out[1].index == 0);

        // The next round gets the next index, which is what "capture by round played" indexes on.
        CHECK(d.Note(OneRow(seq++, kNvFirstLaunchOfRound, ScalarOf(1)), 4000, out, 16) == 0);
        REQUIRE(d.Note(OneRow(seq++, kNvFirstLaunchOfRound, ScalarOf(0)), 4001, out, 16) == 2);
        CHECK(out[1].index == 1);

        // ★ And the conjunction really is required: hold the disc, and the same edge produces nothing.
        CHECK(d.Note(OneRow(seq++, kNvFrisbeeOwnerId, 0x0000000000000003ull), 5000, out, 16) == 0);
        CHECK(d.Note(OneRow(seq++, kNvFirstLaunchOfRound, ScalarOf(1)), 5001, out, 16) == 0);
        CHECK(d.Note(OneRow(seq++, kNvFirstLaunchOfRound, ScalarOf(0)), 5002, out, 16) == 0);
    }

    SUBCASE("a game_state edge is recorded as PHASE, not asserted to be a round") {
        // Ten of these occur per match. Calling each one ROUND_BEGIN would produce ten round markers
        // that look entirely plausible, which is why the vocabulary this project has not re-derived is
        // carried in the payload instead of guessed at.
        CHECK(d.Note(OneRow(seq++, kNvGameState, 0x1111111111111111ull), 6000, out, 16) == 0);
        REQUIRE(d.Note(OneRow(seq++, kNvGameState, 0x2222222222222222ull), 6001, out, 16) == 1);
        CHECK(out[0].kind == kMarkPhase);
        CHECK(out[0].payload == 0x2222222222222222ull);
    }

    SUBCASE("a non-scalar SValue is never read as a number") {
        // The unset sentinel and a flag are not 0x212-tagged scalars. Reading one as an int is the
        // documented cause of a pause-overlay regression, so the deriver refuses rather than converts.
        CHECK(d.Note(OneRow(seq++, kNvBlueScore, 0x0000FFFF00000000ull), 7000, out, 16) == 0);
        CHECK(d.Note(OneRow(seq++, kNvBlueScore, 0xFFFF0000FFFF0501ull), 7001, out, 16) == 0);
    }

    SUBCASE("the seen-mask distinguishes a quiet match from the wrong symbols") {
        MarkerDeriver e;
        e.Reset();
        for (int i = 0; i < 50; ++i) {
            e.Note(OneRow(seq++, 0xDEADBEEFCAFEF00Dull, ScalarOf(uint32_t(i))), 8000, out, 16);
        }
        // Rows went past and no watched name was ever seen. That is not a quiet match: it is a set of
        // symbols that does not match the stream, and the two must not be reported the same way.
        CHECK(e.GetStats().rowsSeen == 50);
        CHECK(e.GetStats().emitted == 0);
        CHECK(e.GetStats().namesSeenMask == 0);
        CHECK(e.GetStats().namesWatched > 0);
    }
}

// ---------------------------------------------------------------------------------------------
// S_KEY block framing -- the property the framing exists to buy
// ---------------------------------------------------------------------------------------------

TEST_CASE("a keyframe round-trips its blocks, and the table has to land on the end") {
    uint8_t blob[4096];
    KeyframeBlobWriter w(blob, sizeof blob, 12345);
    w.Block(kKeyBlockTick);
    const uint32_t tick = 0xABCDEF01u;
    w.Put(tick);
    w.Block(kKeyBlockNetvar);
    const uint64_t name = 0xB2996AD484F1D1E8ull, value = 0x0000021242301900ull;
    w.Put(name);
    w.Put(value);
    const size_t bytes = w.Finish();
    REQUIRE(bytes > 0);
    REQUIRE(w.Ok());

    KeyframeReader r;
    REQUIRE(r.Open(blob, bytes));
    CHECK(r.TimeUs() == 12345);
    CHECK(r.Version() == kKeyframeVersion);
    CHECK(r.BlockCount() == 2);

    uint16_t id = 0;
    const uint8_t* data = nullptr;
    uint32_t len = 0;
    REQUIRE(r.Next(id, data, len));
    CHECK(id == kKeyBlockTick);
    REQUIRE(len == sizeof tick);
    CHECK(memcmp(data, &tick, sizeof tick) == 0);
    REQUIRE(r.Next(id, data, len));
    CHECK(id == kKeyBlockNetvar);
    REQUIRE(len == sizeof name + sizeof value);
    CHECK(memcmp(data, &name, sizeof name) == 0);
    CHECK(memcmp(data + sizeof name, &value, sizeof value) == 0);
    CHECK_FALSE(r.Next(id, data, len));
    // The declared count landed exactly on the end of the blob. Trailing bytes would mean the table
    // and the payload disagree, and reporting that as a clean parse would be reporting a partial
    // understanding as a complete one.
    CHECK(r.Consistent());
}

TEST_CASE("a reader built today reads a keyframe carrying a block from tomorrow") {
    // THIS IS THE WHOLE REASON THE FRAMING LANDED BEFORE THE FIELDS. The determinism pins S7.4 and
    // rulings Q4 / Q-G require cannot be written yet -- three are blocked on other packets -- and every
    // match recorded in the meantime must still be able to CARRY them. So a future block is planted in
    // the MIDDLE of the blob, where a positional layout would be destroyed by it, and a consumer that
    // knows only the ids of today is required to come through with every known block intact.
    uint8_t blob[4096];
    KeyframeBlobWriter w(blob, sizeof blob, 999);
    const uint32_t tickPayload = 0x11111111u;
    w.Block(kKeyBlockTick);
    w.Put(tickPayload);

    // A pin nobody can write today, in the middle, deliberately larger than its neighbours.
    w.Block(kKeyBlockAudioPins);
    uint8_t future[300];
    for (size_t i = 0; i < sizeof future; ++i) {
        future[i] = static_cast<uint8_t>(i * 7 + 1);
    }
    w.Raw(future, sizeof future);

    // And one nobody has even reserved, to prove the rule is "skip by length", not "skip a known list".
    w.Block(2000);
    const uint64_t unnamed = 0xFEEDFACECAFEBEEFull;
    w.Put(unnamed);

    const uint16_t netvarCount = 3;
    w.Block(kKeyBlockNetvar);
    w.Put(netvarCount);
    const size_t bytes = w.Finish();
    REQUIRE(bytes > 0);

    KeyframeReader r;
    REQUIRE(r.Open(blob, bytes));
    CHECK(r.BlockCount() == 4);

    bool sawTick = false, sawNetvar = false;
    uint32_t gotTick = 0;
    uint16_t gotNetvar = 0;
    uint16_t id = 0;
    const uint8_t* data = nullptr;
    uint32_t len = 0;
    while (r.Next(id, data, len)) {
        switch (id) {
            case kKeyBlockTick:
                REQUIRE(len == sizeof gotTick);
                memcpy(&gotTick, data, sizeof gotTick);
                sawTick = true;
                break;
            case kKeyBlockNetvar:
                REQUIRE(len == sizeof gotNetvar);
                memcpy(&gotNetvar, data, sizeof gotNetvar);
                sawNetvar = true;
                break;
            default:
                // The consumer decides what it does not know, which is why NoteUnknown is its call and
                // not the reader own: a reader that classified for you would be reporting its own
                // coverage rather than the caller's.
                r.NoteUnknown(len);
                break;
        }
    }
    // Both known blocks arrived intact, including the one AFTER the two unknowns.
    CHECK(sawTick);
    CHECK(gotTick == tickPayload);
    CHECK(sawNetvar);
    CHECK(gotNetvar == netvarCount);
    CHECK(r.Consistent());

    // And the skipping was COUNTED. A reader that silently ignored what it could not read could not
    // tell a file that carries nothing from a file it cannot understand -- the same shape as a counter
    // that counts its own polling. A player without the audio pins can now say so.
    CHECK(r.UnknownBlocks() == 2);
    CHECK(r.UnknownBytes() == sizeof future + sizeof unnamed);
}

TEST_CASE("a keyframe a reader cannot decode is refused by version, never misparsed") {
    uint8_t blob[256];
    KeyframeBlobWriter w(blob, sizeof blob, 7);
    w.Block(kKeyBlockTick);
    const uint32_t v = 1;
    w.Put(v);
    const size_t bytes = w.Finish();
    REQUIRE(bytes > 0);

    SUBCASE("a version this build does not know") {
        uint8_t bad[256];
        memcpy(bad, blob, bytes);
        // Version 1 was a positional struct with no block table at all. Reading it with the block loop
        // would manufacture block ids out of pose floats, so the version is checked before the table.
        const uint32_t v1 = 1;
        memcpy(bad + 4, &v1, sizeof v1);
        KeyframeReader r;
        CHECK_FALSE(r.Open(bad, bytes));
        REQUIRE(r.Refusal() != nullptr);
        CHECK(r.BlockCount() == 0);
    }
    SUBCASE("wrong magic") {
        uint8_t bad[256];
        memcpy(bad, blob, bytes);
        bad[0] = 0;
        KeyframeReader r;
        CHECK_FALSE(r.Open(bad, bytes));
        CHECK(r.Refusal() != nullptr);
    }
    SUBCASE("a blob shorter than its own header") {
        KeyframeReader r;
        CHECK_FALSE(r.Open(blob, 8));
        CHECK(r.Refusal() != nullptr);
    }
    SUBCASE("a block claiming more bytes than the blob holds stops the walk, and says so") {
        uint8_t bad[256];
        memcpy(bad, blob, bytes);
        KeyBlockHeaderDisk h{};
        memcpy(&h, bad + sizeof(KeyframeHeaderDisk), sizeof h);
        h.len = 0xFFFFu;
        memcpy(bad + sizeof(KeyframeHeaderDisk), &h, sizeof h);
        KeyframeReader r;
        REQUIRE(r.Open(bad, bytes));
        uint16_t id = 0;
        const uint8_t* data = nullptr;
        uint32_t len = 0;
        CHECK_FALSE(r.Next(id, data, len));
        // Not consistent, and the caller is told rather than handed a short read that looks whole.
        CHECK_FALSE(r.Consistent());
    }
}

TEST_CASE("the reserved pin ids are distinct, so two packets cannot collide on a number") {
    // The ids are claimed by appearing in the enum, not by being written. Three of the five are
    // blocked on other packets today; the point of reserving them now is that whoever unblocks first
    // does not have to ask what number to use.
    const uint16_t reserved[] = {kKeyBlockAudioPins, kKeyBlockParticleRng, kKeyBlockAnimPhase,
                                 kKeyBlockTrailState, kKeyBlockBodyOwnerSleep};
    const uint16_t today[] = {kKeyBlockTick,      kKeyBlockPlayerWire, kKeyBlockPlayerSampled,
                              kKeyBlockBodyWire,  kKeyBlockBodySampled, kKeyBlockRoster,
                              kKeyBlockWorldOrigin, kKeyBlockNetvar};
    for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; ++i) {
        CHECK(reserved[i] >= kKeyBlockFirstReserved);
        CHECK(reserved[i] <= kKeyBlockLastReserved);
        for (size_t k = i + 1; k < sizeof reserved / sizeof reserved[0]; ++k) {
            CHECK(reserved[i] != reserved[k]);
        }
        for (size_t k = 0; k < sizeof today / sizeof today[0]; ++k) {
            CHECK(reserved[i] != today[k]);
            CHECK(today[k] < kKeyBlockFirstReserved);
        }
        CHECK(KeyBlockName(reserved[i]) != nullptr);
    }
}

TEST_CASE("the fill mask survives the round trip, so an absent field is not stored as a zero") {
    // The property: a field that no engine accessor supplied must be distinguishable in the FILE from
    // the same field genuinely measuring zero. Without it, trigL = 0.0 from a source that does not
    // exist on this build reads exactly like a trigger at rest, and a player reproduces the wrong one
    // of those two while every check stays green.
    std::vector<uint8_t> buf(1u << 16, 0);
    bitio::BitWriter w(buf.data(), buf.size());
    PlayerEncoder enc(true);

    PlayerFrame sourced = MakeSampledPlayer(1);
    sourced.trigL = 0.0f;
    sourced.trigR = 0.0f;
    sourced.anchor = 0;
    sourced.fill = kFillPose | kFillRefVel | kFillBody | kFillActionWord | kFillHolding;

    // Bit-identical field values, and the trigger really was measured at rest this time.
    PlayerFrame measured = sourced;
    measured.fill = sourced.fill | kFillTrigger | kFillAnchor;

    REQUIRE(enc.Encode(w, 4, 1000, sourced));
    REQUIRE(enc.Encode(w, 4, 1001, measured));
    REQUIRE_FALSE(w.Refused());

    bitio::BitReader r(buf.data(), buf.size());
    PlayerDecoder dec(true);
    uint8_t scratch[512];
    DecodedPlayer a{}, b{};
    REQUIRE(dec.Decode(r, a, scratch, sizeof scratch));
    REQUIRE(dec.Decode(r, b, scratch, sizeof scratch));
    CHECK(r.BitsRead() == w.BitsWritten());

    // The two rows carry identical VALUES for every field...
    CHECK(SameF(a.frame.trigL, b.frame.trigL));
    CHECK(a.frame.anchor == b.frame.anchor);
    // ...and the file still tells them apart, which is the whole point.
    CHECK(a.frame.fill == sourced.fill);
    CHECK(b.frame.fill == measured.fill);
    CHECK((a.frame.fill & kFillTrigger) == 0);
    CHECK((b.frame.fill & kFillTrigger) != 0);
    CHECK(a.frame.fill != b.frame.fill);
}

TEST_CASE("a wire row declares every field sourced, because the message carries them all") {
    // The distinction matters in both directions: trigger blends and anchor ARE on the wire schedule,
    // so a remote player's row has them even though the sampled path cannot supply them. A mask that
    // said otherwise would understate what a capture holds.
    const bitio::WireField* W = bitio::kPlayerStateWire;
    uint8_t body[64] = {};
    const uint64_t hdr0 = 0x0123456789ABCDEFull, stamp = 1700000000000000ull;
    memcpy(body, &hdr0, 8);
    memcpy(body + 8, &stamp, 8);

    const PlayerFrame src = MakeWirePlayer(3);
    bitio::EngineBitWriter ew(body + 16, sizeof body - 16);
    ew.BitPackQuat(src.hmdOri, W[0].bits);
    ew.BitPackQuat(src.lHandOri, W[1].bits);
    ew.BitPackQuat(src.rHandOri, W[2].bits);
    ew.BitPackVec(src.refVel, W[3].min, W[3].max, W[3].step);
    ew.Write(src.bits28, W[4].bits);
    ew.Write(src.holdLSlot, W[5].bits);
    ew.Write(src.holdLGen, W[6].bits);
    ew.Write(src.holdRSlot, W[7].bits);
    ew.Write(src.holdRGen, W[8].bits);
    ew.BitPackReal(src.bendBack, W[9].min, W[9].max, W[9].step);
    ew.BitPackReal(src.bendFwd, W[10].min, W[10].max, W[10].step);
    ew.BitPackReal(src.restPoseHeight, W[11].min, W[11].max, W[11].step);
    ew.BitPackVec(src.lHandOff, W[12].min, W[12].max, W[12].step);
    ew.BitPackVec(src.rHandOff, W[13].min, W[13].max, W[13].step);
    ew.BitPackReal(src.trigL, W[14].min, W[14].max, W[14].step);
    ew.BitPackReal(src.trigR, W[15].min, W[15].max, W[15].step);
    ew.Write(src.anchor, W[16].bits);
    ew.BitPackVec(src.refVelDup, W[17].min, W[17].max, W[17].step);
    ew.Finish();
    REQUIRE(ew.Status() == bitio::WireStatus::Ok);

    PlayerFrame got{};
    REQUIRE(DecodePlayerBody(body, sizeof body, got) == WireDecode::Ok);
    CHECK(got.fill == kFillWireAll);
    CHECK((got.fill & kFillTrigger) != 0);
    CHECK((got.fill & kFillAnchor) != 0);
    CHECK(SameF(got.trigL, src.trigL));
    CHECK(SameF(got.trigR, src.trigR));
    CHECK(got.anchor == src.anchor);
}

TEST_CASE("a body row without a net id says so, and is not confused with net id zero") {
    // The wire identifies a body by a u64 net id -- SimStateCB resolves one through
    // FindNetActor(netId) before it touches a component. A local pool handle is a per-session number
    // that means nothing to the session replaying the file, so a row that cannot supply the net id
    // has to say that rather than store a zero that reads like an identity. Same class as an
    // alignment sentinel that does not fit its field.
    std::vector<uint8_t> buf(1u << 16, 0);
    bitio::BitWriter w(buf.data(), buf.size());
    BodyEncoder enc(true);

    BodyFrame unidentified = MakeWireBody(2);
    unidentified.hasHeader = false;
    unidentified.idValue = 0;
    unidentified.fill = kFillBodyTransform | kFillBodyLastHeld | kFillBodyBits;

    // A body whose net id genuinely IS zero. Identical bytes in the field, different meaning.
    BodyFrame identified = unidentified;
    identified.fill = unidentified.fill | kFillBodyIdValue;

    REQUIRE(enc.Encode(w, 1, 500, unidentified));
    REQUIRE(enc.Encode(w, 1, 501, identified));
    REQUIRE_FALSE(w.Refused());

    bitio::BitReader r(buf.data(), buf.size());
    BodyDecoder dec(true);
    uint8_t scratch[512];
    DecodedBody a{}, b{};
    REQUIRE(dec.Decode(r, a, scratch, sizeof scratch));
    REQUIRE(dec.Decode(r, b, scratch, sizeof scratch));
    CHECK(r.BitsRead() == w.BitsWritten());

    CHECK(a.frame.idValue == b.frame.idValue);          // same bytes
    CHECK((a.frame.fill & kFillBodyIdValue) == 0);    // and the file still tells them apart
    CHECK((b.frame.fill & kFillBodyIdValue) != 0);
}

TEST_CASE("a wire body row carries its net id as an identity") {
    // The other direction: S_BODY_WIRE rows DO have the key, off the wire, so a capture is not
    // uniformly identity-less -- only the sampled rows are, and only until the offset is derived.
    BodyFrame f = MakeWireBody(9);
    CHECK((f.fill & kFillBodyIdValue) != 0);
    CHECK(f.idValue != 0);
}

TEST_CASE("the manifest blobs survive a serialise and reload, so the file carries them") {
    // S5.4.1's four blobs are raw bytes, not parsed structures, and the capture fills them verbatim
    // from the one-shot messages. This asserts the container actually carries them out the other side:
    // a blob that is set and then lost in serialisation is indistinguishable, from the file, from one
    // that was never captured.
    ManifestBuilder mb;
    mb.Begin(1700000000000000ull, 0, 0x40);
    const uint8_t md5[16] = {1};
    mb.SetBuild(md5, 1, 2);

    std::vector<uint8_t> netIds(218), balance(3334), slot0(926), slot1(398);
    for (size_t i = 0; i < netIds.size(); ++i) netIds[i] = static_cast<uint8_t>(i * 3 + 1);
    for (size_t i = 0; i < balance.size(); ++i) balance[i] = static_cast<uint8_t>(i * 7 + 2);
    for (size_t i = 0; i < slot0.size(); ++i) slot0[i] = static_cast<uint8_t>(i * 11 + 3);
    for (size_t i = 0; i < slot1.size(); ++i) slot1[i] = static_cast<uint8_t>(i * 13 + 5);
    mb.SetNetIdSlotMap(netIds.data(), static_cast<uint32_t>(netIds.size()));
    mb.SetBalanceBlob(balance.data(), static_cast<uint32_t>(balance.size()));
    mb.SetSlot0Baselines(slot0.data(), static_cast<uint32_t>(slot0.size()));
    mb.SetSlot1RuleVars(slot1.data(), static_cast<uint32_t>(slot1.size()));

    std::vector<uint8_t> wire;
    SerializeManifest(mb.View(), wire);
    REQUIRE(wire.size() > 0);

    ManifestStore store;
    const char* err = nullptr;
    REQUIRE(DeserializeManifest(wire.data(), static_cast<uint32_t>(wire.size()), store, &err));
    const Manifest& back = store.View();

    REQUIRE(back.netIdSlotMap.n == netIds.size());
    CHECK(memcmp(back.netIdSlotMap.p, netIds.data(), netIds.size()) == 0);
    REQUIRE(back.balanceBlobZstd.n == balance.size());
    CHECK(memcmp(back.balanceBlobZstd.p, balance.data(), balance.size()) == 0);
    REQUIRE(back.slot0Baselines.n == slot0.size());
    CHECK(memcmp(back.slot0Baselines.p, slot0.data(), slot0.size()) == 0);
    REQUIRE(back.slot1RuleVars.n == slot1.size());
    CHECK(memcmp(back.slot1RuleVars.p, slot1.data(), slot1.size()) == 0);
}

TEST_CASE("the net-id map framing, pinned against the two real messages") {
    // Framing recovered from the reference capture and asserted here so a later decoder starts from a
    // checked shape rather than re-deriving it: size == 8 + n*(8 + 2), a u64 column then a u16 column,
    // n implied by the size because it is not stored.
    //
    // The two real messages: 218 bytes -> n = 21, slots 27..47; 278 bytes -> n = 27, slots 0..26. They
    // are disjoint and contiguous and together cover 48 slots.
    struct Case { uint32_t size; uint32_t n; uint16_t firstSlot; };
    const Case cases[] = {{218, 21, 27}, {278, 27, 0}};
    for (const Case& c : cases) {
        CHECK(c.size == 8 + c.n * (8 + 2));
        CHECK((c.size - 8) % 10 == 0);
        CHECK((c.size - 8) / 10 == c.n);
    }
    CHECK(cases[1].firstSlot + cases[1].n == cases[0].firstSlot);   // contiguous, no overlap
    CHECK(cases[0].firstSlot + cases[0].n == 48);

    // ★ AND THE PART THAT IS NOT ESTABLISHED, asserted as a NEGATIVE so it cannot quietly become an
    // assumption: the u64 column is not a per-actor identity. In the 27-entry message it holds only
    // four distinct values -- one repeated 15 times, one 10 times -- which matches S5.4.5's structural
    // groups, not unique ids. Anyone decoding this must establish what the column is; the capture
    // stores the bytes and reads nothing into them.
    const uint32_t distinctInMessage1 = 4;
    CHECK(distinctInMessage1 < cases[1].n);
}

TEST_CASE("a body identity is a tagged pair, and the value alone is not resolvable") {
    // SR15NetPhysicsSimState's dword at +0x4C selects how its qword at +0x48 is read: 0 is an
    // SR15NetUserId (two u16), 1 is a component id, >=2 is a u64 net id. The same eight bytes mean
    // three different things, so a row that stored the value and dropped the tag has recorded a
    // number whose meaning is gone. This is the rule that stops a reader keying on one anyway.
    CHECK_FALSE(BodyIdentityResolvable(0));
    CHECK_FALSE(BodyIdentityResolvable(kFillBodyTransform | kFillBodyBits));
    // Value with no tag and no accessor guarantee: NOT resolvable, which is the case the wire rows
    // are in today.
    CHECK_FALSE(BodyIdentityResolvable(kFillBodyIdValue));
    // Tag without value is equally useless and equally refused.
    CHECK_FALSE(BodyIdentityResolvable(kFillBodyIdTag));
    // Either witness to the meaning, alongside the value, resolves it.
    CHECK(BodyIdentityResolvable(kFillBodyIdValue | kFillBodyIdTag));
    CHECK(BodyIdentityResolvable(kFillBodyIdValue | kFillBodyIdNetActor));

    SUBCASE("both halves survive the round trip, and the tag distinguishes equal values") {
        std::vector<uint8_t> buf(1u << 16, 0);
        bitio::BitWriter w(buf.data(), buf.size());
        BodyEncoder enc(true);

        // Three rows whose idValue is bit-identical and whose MEANING differs. Under the old
        // single-field shape these were the same row.
        BodyFrame asUserId = MakeWireBody(4);
        asUserId.hasHeader = false;
        // slot 5 at +0x48, generation 2 at +0x4A -- so bits 0-15 and 16-31 of the qword. The first
        // version of this line put the generation at bits 32-47 and the test caught it, which is the
        // same halves-of-a-qword confusion the tag exists to prevent.
        asUserId.idValue = 0x0000000000020005ull;
        asUserId.idTag = 0;
        asUserId.fill = kFillBodyTransform | kFillBodyLastHeld | kFillBodyBits | kFillBodyIdValue |
                        kFillBodyIdTag;

        BodyFrame asComponent = asUserId;
        asComponent.idTag = 1;

        BodyFrame asNetId = asUserId;
        asNetId.idTag = 7;   // any value >= 2 is the net-id arm

        REQUIRE(enc.Encode(w, 2, 100, asUserId));
        REQUIRE(enc.Encode(w, 2, 101, asComponent));
        REQUIRE(enc.Encode(w, 2, 102, asNetId));
        REQUIRE_FALSE(w.Refused());

        bitio::BitReader r(buf.data(), buf.size());
        BodyDecoder dec(true);
        uint8_t scratch[512];
        DecodedBody a{}, b{}, c{};
        REQUIRE(dec.Decode(r, a, scratch, sizeof scratch));
        REQUIRE(dec.Decode(r, b, scratch, sizeof scratch));
        REQUIRE(dec.Decode(r, c, scratch, sizeof scratch));
        CHECK(r.BitsRead() == w.BitsWritten());

        CHECK(a.frame.idValue == b.frame.idValue);
        CHECK(b.frame.idValue == c.frame.idValue);
        CHECK(a.frame.idTag == 0);
        CHECK(b.frame.idTag == 1);
        CHECK(c.frame.idTag == 7);
        // And the tag-0 arm really is two u16, which is why the value alone could not be trusted.
        CHECK(static_cast<uint16_t>(a.frame.idValue) == 5);
        CHECK(static_cast<uint16_t>(a.frame.idValue >> 16) == 2);
    }

    SUBCASE("a wire row gets the value and no tag, so it declares itself unresolvable") {
        // S5.4.5 notes the wire's component.netid has a high dword that "must be 1", which sits
        // suggestively next to tag==1 meaning a component id. Suggestive is not established, so the
        // 64 bits are stored whole and no tag is claimed for them.
        BodyFrame f = MakeWireBody(6);
        CHECK((f.fill & kFillBodyIdValue) != 0);
        CHECK((f.fill & kFillBodyIdTag) == 0);
        CHECK((kFillBodyWireAll & kFillBodyIdTag) == 0);
        CHECK_FALSE(BodyIdentityResolvable(kFillBodyWireAll));
    }
}

// ★ The tap TABLE above says which types the capture COULD have recorded; this block
// says how many were LIVE when the keyframe was written, which is the denominator an L2 verdict needs
// -- "S_EVENT holds 0 records" is not distinguishable from "the tap was never registered" without it.
// The writer and the reader are one pair (keyframe.h) so no consumer keeps a private copy of the
// layout, and the reader REFUSES a block whose counts do not close rather than passing on a
// plausible-looking denominator.
TEST_CASE("the tap-registration block round-trips, and a block that does not add up is refused") {
    TapRegistrationBlock in{};
    in.registered = 30;
    in.deferred = 4;
    in.givenUp = 2;
    in.unavailable = 1;
    in.total = 37;
    in.excluded = 1;
    in.tableRows = 38;
    in.started = 1;

    uint8_t blob[512];
    KeyframeBlobWriter w(blob, sizeof blob, 7);
    PutTapRegistrationBlock(w, in);
    const size_t bytes = w.Finish();
    REQUIRE(bytes > 0);

    KeyframeReader r;
    REQUIRE(r.Open(blob, bytes));
    uint16_t id = 0;
    const uint8_t* data = nullptr;
    uint32_t len = 0;
    REQUIRE(r.Next(id, data, len));
    REQUIRE(id == kKeyBlockTapRegistration);
    REQUIRE(len == kTapRegistrationBlockBytes);

    TapRegistrationBlock out{};
    REQUIRE(ReadTapRegistrationBlock(data, len, out));
    CHECK(out.registered == in.registered);
    CHECK(out.deferred == in.deferred);
    CHECK(out.givenUp == in.givenUp);
    CHECK(out.unavailable == in.unavailable);
    CHECK(out.excluded == in.excluded);
    CHECK(out.total == in.total);
    CHECK(out.tableRows == in.tableRows);
    CHECK(out.started == in.started);

    // A short block is refused, not padded with zeros -- a zero denominator reads as "no tap was ever
    // live", which is a claim about the capture and not about the read.
    TapRegistrationBlock shortRead{};
    CHECK_FALSE(ReadTapRegistrationBlock(data, kTapRegistrationBlockBytes - 1, shortRead));

    // And a block whose own identities do not close is refused on each of them separately.
    uint8_t broken[kTapRegistrationBlockBytes];
    memcpy(broken, data, sizeof broken);
    ++broken[0];   // one more registered than the buckets can account for
    CHECK_FALSE(ReadTapRegistrationBlock(broken, sizeof broken, shortRead));
    memcpy(broken, data, sizeof broken);
    ++broken[12];  // tableRows no longer covers total + excluded
    CHECK_FALSE(ReadTapRegistrationBlock(broken, sizeof broken, shortRead));

    // The name is the reader-facing half: a block printed as "unknown" is one a reader cannot report.
    CHECK(strcmp(KeyBlockName(kKeyBlockTapRegistration), "TAP_REGISTRATION") == 0);
}

TEST_CASE("a keyframe carries the event vocabulary, so a file can say what its own S_EVENT rows mean") {
    // Every S_EVENT row stores a typeIndex. Before this block that index pointed into an array that
    // existed only in the writer's binary: adding, removing or reordering one entry silently repointed
    // every event in every file already recorded, with nothing anywhere disagreeing. A goal would read
    // as a stun. The vocabulary now travels with the recording.
    uint8_t blob[64 * 1024];
    KeyframeBlobWriter w(blob, sizeof blob, 42);
    w.Block(kKeyBlockTapTable);
    const uint16_t count = TapTypeCount();
    w.Put(count);
    for (uint16_t i = 0; i < count; ++i) {
        const TapType& t = TapTable()[i];
        const uint16_t len = static_cast<uint16_t>(strlen(t.structName));
        w.Put(i);
        w.Put(t.stream);
        w.Put(len);
        w.Raw(t.structName, len);
    }
    const size_t bytes = w.Finish();
    REQUIRE(bytes > 0);

    KeyframeReader r;
    REQUIRE(r.Open(blob, bytes));
    uint16_t id = 0;
    const uint8_t* data = nullptr;
    uint32_t len = 0;
    REQUIRE(r.Next(id, data, len));
    REQUIRE(id == kKeyBlockTapTable);

    // Walk it back the way a reader with no access to this build's source would.
    size_t at = 0;
    uint16_t got = 0;
    memcpy(&got, data + at, 2);
    at += 2;
    REQUIRE(got == count);
    uint16_t resolved = 0;
    for (uint16_t i = 0; i < got; ++i) {
        uint16_t idx = 0, stream = 0, nameLen = 0;
        memcpy(&idx, data + at, 2);
        at += 2;
        memcpy(&stream, data + at, 2);
        at += 2;
        memcpy(&nameLen, data + at, 2);
        at += 2;
        REQUIRE(at + nameLen <= len);
        const std::string name(reinterpret_cast<const char*>(data + at), nameLen);
        at += nameLen;
        CHECK(idx == i);
        CHECK(name == TapTable()[i].structName);
        CHECK(stream == TapTable()[i].stream);
        ++resolved;
    }
    CHECK(at == len);
    CHECK(resolved == count);

    // The property, stated as the thing that was broken: an index recorded in an event row resolves to
    // a NAME using only bytes from the file.
    const uint16_t goalIdx = TapTypeIndex("SR15NetComponentEvent");
    REQUIRE(goalIdx != 0xFFFFu);
    at = 2;
    std::string nameAtGoalIdx;
    for (uint16_t i = 0; i < got; ++i) {
        uint16_t idx = 0, stream = 0, nameLen = 0;
        memcpy(&idx, data + at, 2);
        at += 2;
        memcpy(&stream, data + at, 2);
        at += 2;
        memcpy(&nameLen, data + at, 2);
        at += 2;
        if (idx == goalIdx) {
            nameAtGoalIdx.assign(reinterpret_cast<const char*>(data + at), nameLen);
        }
        at += nameLen;
    }
    CHECK(nameAtGoalIdx == "SR15NetComponentEvent");
}

TEST_CASE("a clear identity bit is never read as net id zero") {
    // W1-E's NetIdOf rests on two subsystems sharing a handle space, and is built so that if that is
    // wrong the lookup returns 0 and the fill bit stays clear. That design is only safe if nothing
    // downstream reads the clear bit as an identity of zero, so the rule is asserted here rather than
    // left to reviewers of future code.
    CHECK_FALSE(BodyIdentityResolvable(kFillBodyTransform | kFillBodyLastHeld | kFillBodyBits));

    BodyFrame unresolvable{};
    unresolvable.idValue = 0;
    unresolvable.fill = kFillBodyTransform;
    BodyFrame alsoUnresolvable{};
    alsoUnresolvable.idValue = 0;
    alsoUnresolvable.fill = kFillBodyTransform | kFillBodyBits;

    // Two rows, both with idValue 0, neither resolvable. They are not "the same body with net id 0";
    // they are two rows whose subjects were not recorded, and nothing may key them together.
    CHECK_FALSE(BodyIdentityResolvable(unresolvable.fill));
    CHECK_FALSE(BodyIdentityResolvable(alsoUnresolvable.fill));

    // A genuine net id of zero cannot arise from NetIdOf -- 0 is its "no entry" answer -- so the only
    // way the value 0 is ever resolvable is with an explicitly read tag.
    CHECK(BodyIdentityResolvable(kFillBodyIdValue | kFillBodyIdTag));
    CHECK_FALSE(BodyIdentityResolvable(kFillBodyIdValue));
}

TEST_CASE("a dropped sample and a departed entity are not the same file") {
    // A3. Before presence was reported separately these produced BIT-IDENTICAL output: in both cases
    // no record for the entity reached the chapter, and the accounting had no way to say which had
    // happened. One is a loss the file must name; the other is an entity that simply left.
    auto run = [](bool present, bool sampled) {
        Completeness c;
        c.Configure(kStreamPlayerSampled, ObservedSource::EngineFrameIndex);
        c.BeginChapter(0);
        // Two frames where everything is normal, so there is a baseline to depart from.
        for (uint32_t f = 1; f <= 2; ++f) {
            c.BeginFrameSet(kStreamPlayerSampled, f);
            c.NoteEntityPresent(kStreamPlayerSampled, 3, f, 100 + f);
            c.NoteEntitySampled(kStreamPlayerSampled, 3);
            c.EndFrameSet(kStreamPlayerSampled, 100 + f);
        }
        // The frame under test.
        c.BeginFrameSet(kStreamPlayerSampled, 3);
        if (present) {
            c.NoteEntityPresent(kStreamPlayerSampled, 3, 3, 103);
            if (sampled) {
                c.NoteEntitySampled(kStreamPlayerSampled, 3);
            }
        }
        c.EndFrameSet(kStreamPlayerSampled, 103);
        struct R { uint64_t observed, stored, dropped; };
        Completeness::Totals t[8];
        const uint32_t n = c.Snapshot(t, 8);
        for (uint32_t i = 0; i < n; ++i) {
            if (t[i].stream == kStreamPlayerSampled) {
                return R{t[i].observed, t[i].stored, t[i].dropped};
            }
        }
        return R{0, 0, 0};
    };

    const auto normal = run(true, true);
    const auto departed = run(false, false);
    const auto skipped = run(true, false);

    // The engine held it and we recorded it.
    CHECK(normal.observed == 3);
    CHECK(normal.stored == 3);
    CHECK(normal.dropped == 0);

    // It left. Two observations, two records, nothing lost -- absence is absence (E-C2).
    CHECK(departed.observed == 2);
    CHECK(departed.stored == 2);
    CHECK(departed.dropped == 0);

    // It was there and we did not record it. THREE observations, two records, one named loss.
    CHECK(skipped.observed == 3);
    CHECK(skipped.stored == 2);
    CHECK(skipped.dropped == 1);

    // And the two cases are distinguishable, which is the whole property. Under the old API both
    // produced observed == stored == 2 and the file certified complete.
    CHECK(departed.observed != skipped.observed);
    CHECK(departed.dropped != skipped.dropped);

    // The arithmetic still closes in every case -- the point is not that the mutant fails to balance,
    // it is that the loss is COUNTED rather than never observed.
    CHECK(skipped.observed == skipped.stored + skipped.dropped);
    CHECK(departed.observed == departed.stored + departed.dropped);
}

TEST_CASE("a stream that observed nothing cannot report itself complete") {
    // A3, the other half. 0 == 0 + 0 closes, so an untouched stream passes the arithmetic without ever
    // having been shown to work. EverObserved is what a caller checks so silence is not read as
    // success.
    Completeness c;
    c.Configure(kStreamBodySampled, ObservedSource::EngineFrameIndex);
    c.BeginChapter(0);
    CHECK_FALSE(c.EverObserved(kStreamBodySampled));

    c.BeginFrameSet(kStreamBodySampled, 1);
    c.NoteEntityPresent(kStreamBodySampled, 0, 1, 10);
    c.NoteEntitySampled(kStreamBodySampled, 0);
    c.EndFrameSet(kStreamBodySampled, 10);
    CHECK(c.EverObserved(kStreamBodySampled));

    // A stream nobody configured has observed nothing either, and says so rather than defaulting true.
    CHECK_FALSE(c.EverObserved(kStreamVoip));
}

TEST_CASE("shedding the pre-open buffer does not shed the record that it was shed") {
    // A5. The identity traffic that arrives before the file exists is held in a capped buffer. The
    // version this replaces cleared its record COUNTER along with the bytes, so an overflow destroyed
    // the only evidence the traffic had ever arrived: the manifest log then reported the post-shed
    // count as though it were everything walked, and a genuine loss read exactly like a lobby that
    // sent nothing. The counters are now the buffer's own invariant, which is what this pins.
    PreOpenBuffer b;
    std::vector<uint8_t> rec(4096, 0xAB);

    // Fill to just under the cap. Nothing is shed and nothing is counted as lost.
    const uint32_t fits = static_cast<uint32_t>(PreOpenBuffer::kCapBytes / rec.size());
    for (uint32_t i = 0; i < fits; ++i) {
        b.Push(rec.data(), static_cast<uint32_t>(rec.size()));
    }
    REQUIRE(b.Records() == fits);
    REQUIRE(b.Size() == PreOpenBuffer::kCapBytes);
    CHECK(b.Sheds() == 0);
    CHECK(b.DroppedRecords() == 0);
    CHECK(b.DroppedBytes() == 0);

    // One more crosses the cap. The buffer restarts holding just that record -- and says so.
    b.Push(rec.data(), static_cast<uint32_t>(rec.size()));
    CHECK(b.Sheds() == 1);
    CHECK(b.Records() == 1);
    CHECK(b.Size() == rec.size());
    CHECK(b.DroppedRecords() == fits);
    CHECK(b.DroppedBytes() == PreOpenBuffer::kCapBytes);

    // ★ THE REGRESSION ITSELF. Draining after the flush must not reset the loss either: the degraded
    // record is written AFTER the drain, when there is finally a file to write it into. If Drain()
    // zeroed these, the file would open clean over a loss that had already happened.
    b.Drain();
    CHECK(b.Records() == 0);
    CHECK(b.Size() == 0);
    CHECK(b.DroppedRecords() == fits);
    CHECK(b.DroppedBytes() == PreOpenBuffer::kCapBytes);
    CHECK(b.Sheds() == 1);

    // A second overflow accumulates onto the first rather than replacing it.
    for (uint32_t i = 0; i <= fits; ++i) {
        b.Push(rec.data(), static_cast<uint32_t>(rec.size()));
    }
    CHECK(b.Sheds() == 2);
    CHECK(b.DroppedRecords() == fits * 2);
    CHECK(b.DroppedBytes() == PreOpenBuffer::kCapBytes * 2);

    // A zero-byte push is not a record and must not be counted as one.
    const uint32_t before = b.Records();
    b.Push(rec.data(), 0);
    CHECK(b.Records() == before);
}


// ---------------------------------------------------------------------------------------------
// L5 re-run #4. Two defects that together closed always-on capture 32 s into a healthy
// solo match: a kill rung that read a quantile its window cannot express, and a chapter time axis
// that any foreign stamp could redefine.
// ---------------------------------------------------------------------------------------------

namespace w2a_r7 {

// The killed run's own shape. Its manifest still carries keyframe_us=2000000, so rung 2 never fired,
// so at most one window in that launch had p99 over the 50 us budget -- while three consecutive
// windows were judged to have "p999" over 400 us. The only distribution that is both is a body well
// inside budget with one outlier per window, which is what a wall-clock RDTSC bracket records when
// the OS deschedules the tick thread once.
void FeedHealthyWindow(replay::capture::CostMeter& m) {
    for (uint32_t i = 0; i < replay::capture::CostMeter::kWindowTicks - 1; ++i) {
        m.AddSampleUs(12);
    }
    m.AddSampleUs(4000);
}

void FeedFlatWindow(replay::capture::CostMeter& m, uint32_t us) {
    for (uint32_t i = 0; i < replay::capture::CostMeter::kWindowTicks; ++i) {
        m.AddSampleUs(us);
    }
}

}  // namespace w2a_r7

TEST_CASE("the cost kill cannot be reached by one slow tick per window, and still fires on a runaway") {
    using namespace replay::capture;

    // A 512-sample window cannot express a 99.9th percentile: floor(512*0.999) = 511, so the bucket
    // whose cumulative count first passes 511 is the one holding the 512th -- largest -- sample.
    // Stated as arithmetic rather than as a claim about the code.
    CHECK(static_cast<uint64_t>(double(CostMeter::kWindowTicks) * 0.999) ==
          CostMeter::kWindowTicks - 1);

    SUBCASE("the recorded run's distribution never reaches the kill") {
        CostMeter m;
        m.Init();
        for (uint32_t w = 0; w < 8; ++w) {
            w2a_r7::FeedHealthyWindow(m);
            CostMeter::Rung r = CostMeter::Rung::Nominal;
            m.Evaluate(r);
            CHECK(r != CostMeter::Rung::Killed);
        }
        CHECK(m.CurrentRung() != CostMeter::Rung::Killed);

        // ...and the discriminating property is published, so the next argument about the threshold
        // has numbers in it. The window's max is the outlier; its median is the work we did.
        const CostMeter::WindowStat st = m.LastWindow();
        CHECK(st.windows == 8);
        CHECK(st.maxUs >= 4000);
        CHECK(st.medianUs <= CostMeter::kMedianBudgetUs);
        CHECK(st.p99Us <= CostMeter::kP99BudgetUs);
        CHECK(st.overKillWindows == 0);
    }

    SUBCASE("a genuine runaway is killed, on the third window and not before") {
        CostMeter m;
        m.Init();
        for (uint32_t w = 1; w <= CostMeter::kKillWindows; ++w) {
            w2a_r7::FeedFlatWindow(m, 4 * CostMeter::kP999HardFailUs);
            CostMeter::Rung r = CostMeter::Rung::Nominal;
            m.Evaluate(r);
            CHECK((r == CostMeter::Rung::Killed) == (w == CostMeter::kKillWindows));
        }
        CHECK(m.CurrentRung() == CostMeter::Rung::Killed);
        CHECK(m.LastWindow().medianUs > CostMeter::kKillMedianUs);
    }

    // ENG-PINS-COST "governor gap": the stall L2 #15 measured was 159 ms on one tick in fifteen.
    // The median rung cannot see it and must not -- the median is what it exists to bound -- so
    // the discriminating claim is that the SAME window kills on the tail rung and leaves the
    // median rung's counter at zero. A test that only asserted Killed would pass with the median
    // rung doing the work.
    SUBCASE("a 1-in-15 stall kills on the tail rung and never touches the median rung") {
        CostMeter m;
        m.Init();
        CostMeter::Rung r = CostMeter::Rung::Nominal;
        for (uint32_t w = 1; w <= CostMeter::kKillWindows; ++w) {
            for (uint32_t i = 0; i < CostMeter::kWindowTicks; ++i) {
                m.AddSampleUs(i % 15u == 0u ? 159000u : 5u);
            }
            m.Evaluate(r);
            CHECK((r == CostMeter::Rung::Killed) == (w == CostMeter::kKillWindows));
        }
        const CostMeter::WindowStat st = m.LastWindow();
        CHECK(st.chargedSamples == CostMeter::kWindowTicks);
        CHECK(st.medianUs <= CostMeter::kMedianBudgetUs);
        CHECK(st.medianLoUs < CostMeter::kKillMedianUs);
        CHECK(st.overKillWindows == 0);
        CHECK(st.p99LoUs >= CostMeter::kKillTailUs);
        CHECK(st.overKillTailWindows == CostMeter::kKillWindows);
        // And it kills because the stall COSTS: 1 tick in 15 at 159 ms averages 10.6 ms a tick,
        // two orders over the line a tail has to clear since L3-RUN-12 F4.
        CHECK(st.tailOverBudget);
        CHECK(st.chargedMeanUs >= CostMeter::kKillTailMeanUs);
    }

    SUBCASE("one stalled tick per window reaches neither rung") {
        CostMeter m;
        m.Init();
        CostMeter::Rung r = CostMeter::Rung::Nominal;
        for (uint32_t w = 0; w < 8; ++w) {
            for (uint32_t i = 0; i < CostMeter::kWindowTicks; ++i) {
                m.AddSampleUs(i == 0u ? 159000u : 5u);
            }
            m.Evaluate(r);
        }
        CHECK(m.CurrentRung() != CostMeter::Rung::Killed);
        CHECK(m.LastWindow().maxUs == 159000u);
        CHECK(m.LastWindow().overKillTailWindows == 0);
    }

    // L3-RUN-11 F5. The playback session's always-on capture was closed at window 4 with a MEDIAN of
    // 15-20 us: a few 300 us spikes per window carried the p99 over the tail budget. The design
    // says what this governor protects -- "gather + encode + push" measured under a stated load
    // condition, "machine otherwise idle", so that capture never degrades a live match (G11). The
    // spikes belong to the playback module's own rebuild on the same tick; the capture's charge
    // window must not contain them, and the two halves of this subcase differ ONLY in which channel
    // the identical spikes arrive through.
    SUBCASE("F5: 300 us spikes charged as foreign never kill; the same spikes inside a capture phase do") {
        // 16 spikes in 512 ticks is 3.1 %, comfortably above the 1 % the p99 index sits at, so the
        // spike rate is NOT what distinguishes these two runs. The channel is.
        const auto isSpike = [](uint32_t i) { return i % 32u == 0u; };
        // F5 measured its spikes at six times the S6.4 p99 budget. Derived rather than typed:
        // the bare magnitude collides with a registry fact of the same value.
        const uint32_t kSpikeUs = 6u * CostMeter::kP99BudgetUs;

        CostMeter foreign;
        foreign.Init();
        CostMeter::Rung fr = CostMeter::Rung::Nominal;
        for (uint32_t w = 0; w < 8; ++w) {
            for (uint32_t i = 0; i < CostMeter::kWindowTicks; ++i) {
                foreign.AddSampleUs(17u);                          // the capture's own gather
                foreign.AddUnchargedUs(isSpike(i) ? kSpikeUs : 2u);    // playback's rebuild, same tick
            }
            foreign.Evaluate(fr);
        }
        const CostMeter::WindowStat fw = foreign.LastWindow();
        CHECK(foreign.CurrentRung() != CostMeter::Rung::Killed);
        // Asked and passed -- not passed by default, which is a different statement and would still
        // hold if the rung had been silently skipped.
        CHECK(fw.tailMeasured);
        CHECK(fw.tailNotRunWindows == 0);
        CHECK(fw.chargedSamples == CostMeter::kWindowTicks);
        CHECK(fw.overKillTailWindows == 0);
        CHECK(fw.maxUs == 17u);            // the spike is nowhere in the charged histogram
        CHECK(fw.unchargedMaxUs == kSpikeUs);  // and is reported in full beside it

        CostMeter own;
        own.Init();
        CostMeter::Rung orung = CostMeter::Rung::Nominal;
        for (uint32_t w = 1; w <= CostMeter::kKillWindows; ++w) {
            for (uint32_t i = 0; i < CostMeter::kWindowTicks; ++i) {
                own.AddSampleUs(isSpike(i) ? kSpikeUs : 17u);          // the same spikes, now capture's
                own.AddUnchargedUs(2u);
            }
            own.Evaluate(orung);
        }
        const CostMeter::WindowStat ow = own.LastWindow();
        CHECK(ow.tailMeasured);
        CHECK(ow.maxUs == kSpikeUs);          // charged, so the spike IS in this histogram
        CHECK(ow.tailOverBudget);             // and the tail rung read it
        CHECK(ow.p99LoUs >= CostMeter::kKillTailUs);
        CHECK(!fw.tailOverBudget);            // where through the foreign channel it never saw one
        CHECK(ow.medianUs <= CostMeter::kMedianBudgetUs);
        CHECK(ow.overKillWindows == 0);
        // ...and reading it did NOT close the file. 16 spikes of 300 us on a 17 us body average
        // 26 us a tick, which is not a capture degrading a match (L3-RUN-12 F4): the kill needs the
        // AMOUNT as well as the height, and the 1-in-15 stall subcase above is where a tail that
        // does cost the frame still kills. What this pair discriminates is the CHANNEL, and it now
        // does so on the field the channel decides -- tailOverBudget -- instead of on a latch two
        // terms downstream.
        CHECK(ow.chargedMeanUs < CostMeter::kKillTailMeanUs);
        CHECK(ow.overKillTailWindows == 0);
        CHECK(own.CurrentRung() != CostMeter::Rung::Killed);
    }

    // The mechanism underneath F5: an uncharged tick closes the window too, so a capture that is
    // open on part of the tick stream reaches Evaluate with a charged population far below 512, and
    // p99 over a population that small is the MAXIMUM. The discriminating assertion is the last
    // one -- the number the rung would have read IS over budget, and only the NOT-RUN guard stops it
    // from closing a healthy file.
    SUBCASE("a window truncated by foreign ticks cannot express p99, so the tail rung is NOT-RUN") {
        CostMeter m;
        m.Init();
        CostMeter::Rung r = CostMeter::Rung::Nominal;
        for (uint32_t w = 0; w < 8; ++w) {
            for (uint32_t i = 0; i < CostMeter::kWindowTicks; ++i) {
                if (i % 16u == 0u) {
                    m.AddSampleUs(i % 128u == 0u ? 1231u : 17u);
                }
                m.AddUnchargedUs(2u);
            }
            m.Evaluate(r);
        }
        const CostMeter::WindowStat st = m.LastWindow();
        CHECK(m.CurrentRung() != CostMeter::Rung::Killed);
        CHECK(st.chargedSamples == CostMeter::kWindowTicks / 16u);
        CHECK(st.tailMeasured == false);
        CHECK(st.tailNotRunWindows == 8u);
        CHECK(st.overKillTailWindows == 0);
        CHECK(st.p99LoUs >= CostMeter::kKillTailUs);
    }

    // The boundary the integer solve lands on, pinned so that changing kTailQuantilePct moves it
    // rather than silently widening what the rung will judge.
    // L3-RUN-12 F4, the shape verbatim: the always-on capture of a PLAYBACK session was closed by
    // the tail rung at window 4 -- median 15-20 us, charged mean 51 us, p99 in (750, 1000] us, max
    // 1278 us, read over a FULL 512 charged samples, so the window was askable and 5c70101's guard
    // was never the thing in question. The spikes are capture's OWN phases (roster 20.3/840 us
    // mean/max, stats 10.8/407): ScanRosterTick is change-gated, so it does a full walk on the ~1 %
    // of ticks the engine's entrant table moves on -- which is exactly where the p99 index sits.
    SUBCASE("L3-RUN-12: a change-gated pass above the p99 index does not close a cheap window") {
        // RUN-12's own numbers, derived rather than typed: a body inside the S6.4 median budget,
        // spikes at 25x the p99 budget (1250 us, beside the run's 1278 us max) on 7 of 512 ticks --
        // 1.4 %, deliberately ABOVE the 1 % the p99 index sits at, so the rung is reached and it is
        // the second term that spares the file.
        const uint32_t kBodyUs = CostMeter::kMedianBudgetUs - 3u;
        const uint32_t kSpikeUs = 25u * CostMeter::kP99BudgetUs;
        const uint32_t kSpikeEvery = CostMeter::kWindowTicks / 6u;

        CostMeter m;
        m.Init();
        CostMeter::Rung r = CostMeter::Rung::Nominal;
        const uint32_t kWindows = 8u;
        for (uint32_t w = 0; w < kWindows; ++w) {
            for (uint32_t i = 0; i < CostMeter::kWindowTicks; ++i) {
                m.AddSampleUs(i % kSpikeEvery == 0u ? kSpikeUs : kBodyUs);
                m.AddUnchargedUs(1u);
            }
            m.Evaluate(r);
        }
        const CostMeter::WindowStat st = m.LastWindow();
        // Askable and ASKED -- this is not a NOT-RUN window passing by another route.
        CHECK(st.chargedSamples == CostMeter::kWindowTicks);
        CHECK(st.tailMeasured);
        CHECK(st.tailNotRunWindows == 0);
        // ...and the number it read IS over the tail line, so only the amount term stops the kill.
        CHECK(st.tailOverBudget);
        CHECK(st.p99LoUs >= CostMeter::kKillTailUs);
        CHECK(st.maxUs == kSpikeUs);
        CHECK(st.medianUs <= CostMeter::kMedianBudgetUs);
        CHECK(st.chargedMeanUs < CostMeter::kKillTailMeanUs);
        CHECK(st.overKillWindows == 0);
        CHECK(st.overKillTailWindows == 0);
        CHECK(m.CurrentRung() != CostMeter::Rung::Killed);
        // Eight windows, so this is not "not yet": three consecutive would already have killed.
        CHECK(st.windows == kWindows);
    }

    SUBCASE("p99 becomes a tail at 101 charged samples and not at 100") {
        for (uint32_t charged : {100u, 101u}) {
            CostMeter m;
            m.Init();
            CostMeter::Rung r = CostMeter::Rung::Nominal;
            for (uint32_t i = 0; i < CostMeter::kWindowTicks; ++i) {
                if (i < charged) {
                    m.AddSampleUs(1231u);
                }
                m.AddUnchargedUs(2u);
            }
            m.Evaluate(r);
            const CostMeter::WindowStat st = m.LastWindow();
            CHECK(st.chargedSamples == charged);
            CHECK(st.p99LoUs >= CostMeter::kKillTailUs);
            CHECK(st.tailMeasured == (charged > 100u));
            CHECK(st.overKillTailWindows == (charged > 100u ? 1u : 0u));
        }
    }

    SUBCASE("a run of over-budget windows broken by one healthy window starts the count again") {
        CostMeter m;
        m.Init();
        CostMeter::Rung r = CostMeter::Rung::Nominal;
        w2a_r7::FeedFlatWindow(m, 4 * CostMeter::kP999HardFailUs);
        m.Evaluate(r);
        w2a_r7::FeedFlatWindow(m, 4 * CostMeter::kP999HardFailUs);
        m.Evaluate(r);
        w2a_r7::FeedHealthyWindow(m);
        m.Evaluate(r);
        CHECK(m.LastWindow().overKillWindows == 0);
        w2a_r7::FeedFlatWindow(m, 4 * CostMeter::kP999HardFailUs);
        m.Evaluate(r);
        CHECK(m.CurrentRung() != CostMeter::Rung::Killed);
    }

    SUBCASE("the threshold is a strict boundary, so the budget itself is not a kill") {
        for (int over = 0; over <= 1; ++over) {
            CostMeter m;
            m.Init();
            CostMeter::Rung r = CostMeter::Rung::Nominal;
            for (uint32_t w = 0; w < CostMeter::kKillWindows; ++w) {
                w2a_r7::FeedFlatWindow(m, CostMeter::kKillMedianUs + (over ? 100u : 0u));
                m.Evaluate(r);
            }
            CHECK((m.CurrentRung() == CostMeter::Rung::Killed) == (over == 1));
        }
    }
}

namespace w2a_r7 {

struct TempDemoFile {
    wchar_t path[512];
    explicit TempDemoFile(const wchar_t* tag) {
        wchar_t dir[400];
        const DWORD n = GetTempPathW(400, dir);
        if (!n) { dir[0] = L'.'; dir[1] = 0; }
        _snwprintf_s(path, 512, _TRUNCATE, L"%lsw2a_r7_%ls_%lu.echodemo", dir, tag,
                     (unsigned long)GetCurrentProcessId());
    }
    ~TempDemoFile() { DeleteFileW(path); }
};

template <class T>
void FeedRec(replay::capture::ChapterBuilder& b, uint16_t kind, const T& payload) {
    uint8_t buf[sizeof(replay::capture::RecHeader) + sizeof(T)];
    replay::capture::RecHeader h;
    h.kind = kind;
    h.bytes = (uint16_t)sizeof(T);
    h.producer = 0;
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, &payload, sizeof payload);
    b.Consume(buf, (uint32_t)sizeof buf);
}

// The exact S_EVENT stamps the killed capture recorded, read off its own chapter index with
// `echodemo dump`. None of them is a source time; 0xFFFFFFFFFFFFFFFF is the one that made
// `chapterEndUs >= chapterBeginUs + keyframeIntervalUs` wrap and cut a chapter per record.
const uint64_t kRecordedEventStamps[] = {
    18446744073709551615ull, 6ull, 34ull, 36ull, 65ull, 66ull, 16369959894457074595ull,
};

// Drives one 32 s solo match at the rate the killed run actually ran (S_TICK observed = 962 over
// 32.08 s) and returns the builder's own account of it. `poison` interleaves the recorded S_EVENT
// stamps; with it off the same match is fed with nothing but its ticks.
replay::capture::BuilderStats DriveSoloMatch(const wchar_t* tag, bool poison) {
    using namespace replay::capture;
    using namespace replay;
    TempDemoFile tmp(tag);

    const uint64_t t0 = 19166475ull;      // the killed run's own t0_us
    const uint64_t dt = facts::CLOCK_FIXEDSTEP_30HZ_US;   // 30 Hz, its measured tick rate
    const uint32_t ticks = 962;           // its S_TICK observed count

    ManifestBuilder mb;
    mb.Begin(t0, 0, 0x40);
    const uint8_t md5[16] = {0xca, 0xa0, 0x5e, 0xe3};
    mb.SetBuild(md5, 0x676F6C646D617374ull, 0x0102030405060708ull);
    mb.SetLevel(6300205991959903307ull, 0xF00Dull, 1780177787ull);
    mb.SetKeyframeIntervalUs(2000000);
    mb.SetUuid(1, 2);
    REQUIRE(mb.Refusal() == nullptr);

    DemoWriter writer;
    REQUIRE(writer.Open(tmp.path, mb.View()));

    BuilderConfig cfg;
    cfg.keyframeIntervalUs = 2000000;
    for (uint16_t i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;
    for (uint16_t i = 0; i < kSectionCount; ++i) cfg.sectionEnabled[i] = true;

    ChapterBuilder b;
    REQUIRE(b.Open(&writer, mb.View(), cfg));
    b.EmitMatchBegin(t0);

    for (uint32_t f = 0; f < ticks; ++f) {
        const uint64_t st = t0 + f * dt;
        TickRec tr{};
        tr.sourceTimeUs = st;
        tr.simTimeUs = st;
        tr.frameIndex = f + 1;
        tr.frameDeltaUs = (uint32_t)dt;
        tr.scaledDeltaUs = (uint32_t)dt;
        FeedRec(b, kRecTick, tr);

        if (poison) {
            EventRec er{};
            er.stampUs = kRecordedEventStamps[f % (sizeof kRecordedEventStamps / sizeof(uint64_t))];
            er.dst = 0;
            er.typeIndex = 1;
            er.bodyBytes = 0;
            FeedRec(b, kRecEvent, er);
        }
    }

    b.EmitMatchEnd(t0 + ticks * dt);
    b.FlushChapter();
    const BuilderStats out = b.Stats();
    b.Close();
    writer.Close();
    return out;
}

}  // namespace w2a_r7

TEST_CASE("a 32 s match chapters on its own cadence, and a foreign stamp cannot cut a chapter") {
    using namespace replay::capture;

    const BuilderStats clean = w2a_r7::DriveSoloMatch(L"clean", false);
    const BuilderStats poisoned = w2a_r7::DriveSoloMatch(L"poison", true);

    // The cadence is the whole rule. 962 ticks x 33333 us is 32.06 s of source time at a 2.0 s
    // keyframe interval: sixteen full spans, plus the chapter the match-begin marker opens and the
    // one the match-end marker opens. Anything near the 64 the killed run wrote is the defect.
    CHECK(clean.chaptersWritten >= 16);
    CHECK(clean.chaptersWritten <= 20);
    CHECK(clean.keyframes == clean.chaptersWritten);

    // THE DISCRIMINATING ASSERTION. The poisoned run is the same match with the killed capture's own
    // S_EVENT stamps interleaved. A record whose stamp is not on the source-time axis may not change
    // WHEN a chapter begins or ends, so the two runs must chapter identically. Before this fix the
    // poisoned run cut a chapter almost per record.
    CHECK(poisoned.chaptersWritten == clean.chaptersWritten);
    CHECK(poisoned.keyframes == clean.keyframes);

    // ...and the events did arrive, so the equality above is not the equality of two empty runs.
    CHECK(poisoned.recordsIn == clean.recordsIn + 962);
    CHECK(poisoned.foreignStampsRidden == 962);
    CHECK(clean.foreignStampsRidden == 0);

    // Locked constraint 6: one file per match WITH ROUND MARKERS. Both boundary markers begin their
    // own chapter, so a round boundary is a seek target rather than a row buried mid-chapter.
    CHECK(clean.roundBoundarySplits == 1);
    CHECK(poisoned.roundBoundarySplits == 1);
}

// ---------------------------------------------------------------------------------------------
// AUDIT_M8 F1 (2026-09-03). The round split keyed on a kRecMark record reaching Consume, and nothing
// constructs one: the derived round marker enters the file through AppendMark in DispatchRecord. The
// case above could not see that, because both its boundaries come from EmitMatchBegin/EmitMatchEnd.
// This one drives the PRODUCTION path -- netvar rows in, DUV-parsed, marker derived on the way past.
// ---------------------------------------------------------------------------------------------
namespace w2a_r9 {

// The DUV wire body the netvar tap delivers: one row, header exactly as duv.h describes it.
std::vector<uint8_t> DuvBody(uint32_t seq, uint64_t name, uint64_t value) {
    std::vector<uint8_t> b(46, 0);
    const uint64_t owner = 0x1111ull;
    memcpy(b.data(), &owner, 8);
    const uint16_t slot = 1;
    memcpy(b.data() + 8, &slot, 2);
    const uint32_t one = 1;
    memcpy(b.data() + 12, &one, 4);
    memcpy(b.data() + 16, &seq, 4);
    memcpy(b.data() + 24, &seq, 4);
    const uint16_t n = 1;
    memcpy(b.data() + 28, &n, 2);
    memcpy(b.data() + 30, &name, 8);
    memcpy(b.data() + 38, &value, 8);
    return b;
}

void FeedNetvar(replay::capture::ChapterBuilder& b, uint64_t tUs, uint32_t seq, uint64_t name,
                uint64_t value) {
    using namespace replay::capture;
    const std::vector<uint8_t> duv = DuvBody(seq, name, value);
    std::vector<uint8_t> rec(sizeof(RecHeader) + sizeof(NetvarRec) + duv.size());
    RecHeader h{};
    h.kind = kRecNetvar;
    h.bytes = static_cast<uint16_t>(sizeof(NetvarRec) + duv.size());
    h.producer = 0;
    NetvarRec nr{};
    nr.arrivalUs = tUs;
    nr.bodyBytes = static_cast<uint16_t>(duv.size());
    memcpy(rec.data(), &h, sizeof h);
    memcpy(rec.data() + sizeof h, &nr, sizeof nr);
    memcpy(rec.data() + sizeof h + sizeof nr, duv.data(), duv.size());
    b.Consume(rec.data(), static_cast<uint32_t>(rec.size()));
}

// A three-round match on the real path. `discFree` is R4's conjunction: with the disc UNOWNED the
// first_launch_of_round 1 -> 0 edge is a round boundary, and with it held the identical record
// traffic derives no round at all. That is the control -- same ticks, same netvar rows, same seq
// numbers, one bit of engine state different -- so a difference in chapter count is the split and
// nothing else.
replay::capture::BuilderStats DriveThreeRounds(const wchar_t* tag, bool discFree,
                                               replay::capture::MarkerDeriver::Stats& msOut) {
    using namespace replay::capture;
    using namespace replay;
    using namespace replay::capture_facts;
    w2a_r7::TempDemoFile tmp(tag);

    const uint64_t t0 = 19166475ull;
    const uint64_t dt = facts::CLOCK_FIXEDSTEP_30HZ_US;
    const uint32_t ticks = 400;

    ManifestBuilder mb;
    mb.Begin(t0, 0, 0x40);
    const uint8_t md5[16] = {0xca, 0xa0, 0x5e, 0xe3};
    mb.SetBuild(md5, 0x676F6C646D617374ull, 0x0102030405060708ull);
    mb.SetLevel(6300205991959903307ull, 0xF00Dull, 1780177787ull);
    mb.SetKeyframeIntervalUs(2000000);
    mb.SetUuid(1, 2);
    REQUIRE(mb.Refusal() == nullptr);

    DemoWriter writer;
    REQUIRE(writer.Open(tmp.path, mb.View()));

    BuilderConfig cfg;
    cfg.keyframeIntervalUs = 2000000;
    for (uint16_t i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;
    for (uint16_t i = 0; i < kSectionCount; ++i) cfg.sectionEnabled[i] = true;

    ChapterBuilder b;
    REQUIRE(b.Open(&writer, mb.View(), cfg));
    b.EmitMatchBegin(t0);

    uint32_t seq = 500;
    for (uint32_t f = 0; f < ticks; ++f) {
        const uint64_t st = t0 + f * dt;
        TickRec tr{};
        tr.sourceTimeUs = st;
        tr.simTimeUs = st;
        tr.frameIndex = f + 1;
        tr.frameDeltaUs = (uint32_t)dt;
        tr.scaledDeltaUs = (uint32_t)dt;
        w2a_r7::FeedRec(b, kRecTick, tr);

        if (f == 10) {
            FeedNetvar(b, st, seq++, kNvFrisbeeOwnerId,
                       discFree ? 0x00000000FFFFFFFFull   // facts-ok: PHYS.ID_DEAD the disc's unowned OWNER id, not that offset
                                : 0x0000000000000003ull);
            FeedNetvar(b, st, seq++, kNvCountdownTimer, ScalarOf(0));
        }
        // Three rounds, each armed a tick before it releases. The first 1 is the baseline sighting
        // on round one and a real edge on rounds two and three; the 0 is the release either way.
        if (f == 100 || f == 200 || f == 290) {
            FeedNetvar(b, st, seq++, kNvFirstLaunchOfRound, ScalarOf(1));
        }
        if (f == 101 || f == 201 || f == 291) {
            FeedNetvar(b, st, seq++, kNvFirstLaunchOfRound, ScalarOf(0));
        }
    }

    b.EmitMatchEnd(t0 + ticks * dt);
    msOut = b.MarkerStats();
    b.FlushChapter();
    const BuilderStats out = b.Stats();
    b.Close();
    writer.Close();
    return out;
}

}  // namespace w2a_r9

TEST_CASE("a round derived from the netvar stream begins a chapter, on the path production uses") {
    using namespace replay::capture;

    MarkerDeriver::Stats roundedMs{}, flatMs{};
    const BuilderStats rounded = w2a_r9::DriveThreeRounds(L"r9rounds", true, roundedMs);
    const BuilderStats flat = w2a_r9::DriveThreeRounds(L"r9flat", false, flatMs);

    // The fixture reaches the marker at all: three ROUND_BEGIN in one run and none in the other, off
    // the same rows. Without this the chapter counts below could agree for the wrong reason.
    CHECK(roundedMs.emitted == 6);      // LAUNCH_RELEASE + ROUND_BEGIN, three times
    CHECK(flatMs.emitted == 0);
    CHECK(roundedMs.rowsSeen == flatMs.rowsSeen);   // identical traffic, different engine state

    // THE ASSERTION AUDIT_M8 F1 ASKED FOR. Locked constraint 6 is round markers in the file, and a
    // marker buried mid-chapter is not a seek target. One split for the match end in both runs; three
    // more, and three more chapters, in the run that played three rounds.
    CHECK(flat.roundBoundarySplits == 1);
    CHECK(rounded.roundBoundarySplits == flat.roundBoundarySplits + 3);
    // ...and the cut really opened a chapter rather than only being counted. Keyframes count chapter
    // OPENS, one per chapter. The rise is not +3: a split re-bases the keyframe cadence from the
    // boundary, so two of the three cuts land inside a span the cadence would have cut anyway. The
    // provable statement is that three extra cuts leave strictly more chapters in the file, and the
    // exact number is a property of where the rounds fell rather than of the split.
    CHECK(rounded.keyframes > flat.keyframes);
    CHECK(rounded.chaptersWritten > flat.chaptersWritten);
    MESSAGE("three rounds: splits " << rounded.roundBoundarySplits << " vs " << flat.roundBoundarySplits
            << ", chapters opened " << rounded.keyframes << " vs " << flat.keyframes
            << ", chapter-sections written " << rounded.chaptersWritten << " vs "
            << flat.chaptersWritten);
}

// ---------------------------------------------------------------------------------------------
// W1-E R5, folded in: LocalPlayerAt's absence word is not a player, and the refusal has a name.
// ---------------------------------------------------------------------------------------------
namespace w2a_r9 {

struct SlotPool {
    std::vector<uint16_t> slots;
    uint32_t asked = 0;

    bool Available() const { return true; }
    size_t LocalPlayerCount() const { return slots.size(); }
    replay::UserIndex LocalPlayerAt(size_t i) const { return replay::UserIndex(slots[i]); }

    // Templated on the state and reading types so this fake pins the CALL the gather makes and not
    // the spelling of the engine's structs.
    template <class S, class R>
    uint32_t ReadLocalPlayerDetailed(replay::UserIndex u, S&, R& r) {
        ++asked;
        r.fill = 0;
        // L2's arm: an actor that never resolved. The gather must carry this word out.
        r.refusal = replay::IEntity::LocalRefusal::ActorNull;
        (void)u;
        return 0;
    }
};

struct SlotSinkState {
    uint32_t frameSets = 0;
    uint16_t framedCount = 0;
};

bool SlotSink(uint16_t kind, const void* rec, uint32_t bytes, const void* tail, uint32_t tailBytes,
              void* ctx) {
    using namespace replay::capture;
    SlotSinkState* st = static_cast<SlotSinkState*>(ctx);
    if (kind == kRecFrameSet && bytes >= sizeof(FrameSetRec)) {
        FrameSetRec fs{};
        memcpy(&fs, rec, sizeof fs);
        ++st->frameSets;
        st->framedCount = fs.count;
    }
    (void)tail;
    (void)tailBytes;
    return true;
}

}  // namespace w2a_r9

TEST_CASE("a local-pawn slot the engine could not name is not a present player") {
    using namespace replay::capture;

    w2a_r9::SlotPool pool;
    pool.slots = {kNoUserSlot, 5u, kNoUserSlot};   // two absence words around one real slot

    GatherConfig gc{};
    for (uint16_t i = 0; i < 32; ++i) gc.streamEnabled[i] = true;
    GatherReport rep;
    w2a_r9::SlotSinkState st;
    GatherLocalPawns(&pool, gc, 1234, 1, &w2a_r9::SlotSink, &st, rep);

    // 0xFFFF is LocalPlayerAt's "no such slot" answer. Counted as present it puts a player that does
    // not exist into the frame set, and every frame then reports a DROPPED row for it -- a loss the
    // file states and the session never had.
    CHECK(rep.pawnsPresent == 1);
    CHECK(rep.pawnsUnknownSlot == 2);
    CHECK(st.frameSets == 1);
    CHECK(st.framedCount == 1);
    // ...and the one real slot was the one asked about, so the filter dropped the right two.
    CHECK(pool.asked == 1);

    // The arm, not just the count (DIAG-L2 F4 / W1-E R5). `notAsked` here would mean the gather is
    // still calling the mask-only read and the sentinel cannot say why the rows are missing.
    CHECK(rep.pawnsRefused == 1);
    CHECK(rep.localRefusal == static_cast<uint8_t>(replay::IEntity::LocalRefusal::ActorNull));
    CHECK(std::string(replay::LocalRefusalName(
              static_cast<replay::IEntity::LocalRefusal>(rep.localRefusal))) == "actorNull");
}

// ---------------------------------------------------------------------------------------------
// L2-RUN-2 F2, retroactive on the shape the launch recorded: `EVERY present local pawn refused --
// ReadLocalPlayer returned an EMPTY mask for 1 of 1 present pawn(s) ... Refusal arm as the engine
// named it: ok`. `ok` is set at exactly one place in entity.cpp -- `filled ? Ok : NoSourceAnswered` --
// so the mask the accessor returned was NOT empty, and the only group bit the translator dropped was
// kLocalHolding. That pins the state to reproduce: holding sourced, everything else declined.
// ---------------------------------------------------------------------------------------------
namespace w2a_r10 {

struct HoldingOnlyPool {
    uint32_t engineMask = replay::kLocalHolding;   // what the accessor answers with
    uint32_t asked = 0;

    bool Available() const { return true; }
    size_t LocalPlayerCount() const { return 1; }
    replay::UserIndex LocalPlayerAt(size_t) const { return replay::UserIndex(7); }

    template <class S, class R>
    uint32_t ReadLocalPlayerDetailed(replay::UserIndex, S& st, R& r) {
        ++asked;
        if (engineMask & replay::kLocalHolding) {
            st.holdL = 0x000A0003u;
            st.holdR = 0x000B0004u;
        }
        r.fill = engineMask;
        // The engine's own rule, copied deliberately: a non-empty mask IS `Ok`. That is why the L2
        // line printed `ok` beside a file with no players.
        r.refusal = engineMask ? replay::IEntity::LocalRefusal::Ok
                               : replay::IEntity::LocalRefusal::NoSourceAnswered;
        r.actorSource = replay::IEntity::ActorSource::NetGameUserActor;
        r.echoComponent = false;
        return engineMask;
    }
};

struct RowSink {
    uint32_t rows = 0;
    uint32_t fill = 0;
    uint32_t holdL = 0, holdR = 0;
};

bool CollectRows(uint16_t kind, const void* rec, uint32_t bytes, const void*, uint32_t, void* ctx) {
    using namespace replay::capture;
    RowSink* st = static_cast<RowSink*>(ctx);
    if (kind == kRecSampledPlayer && bytes >= sizeof(SampledPlayerRec)) {
        SampledPlayerRec r{};
        memcpy(&r, rec, sizeof r);
        ++st->rows;
        st->fill = r.fill;
        st->holdL = r.state.holdL;
        st->holdR = r.state.holdR;
    }
    return true;
}

}  // namespace w2a_r10

TEST_CASE("L2-RUN-2 F2 retroactive: a pawn whose ONLY sourced group is holding is RECORDED") {
    using namespace replay::capture;

    w2a_r10::HoldingOnlyPool pool;
    GatherConfig gc{};
    for (uint16_t i = 0; i < 32; ++i) gc.streamEnabled[i] = true;
    GatherReport rep;
    w2a_r10::RowSink st;
    GatherLocalPawns(&pool, gc, 4321, 2, &w2a_r10::CollectRows, &st, rep);

    CHECK(pool.asked == 1);
    CHECK(rep.pawnsPresent == 1);
    // THE L2 SHAPE. Before the translation carried kLocalHolding these four read 0 / 1 / 1 / 0 and
    // the capture wrote no player row for a pawn the engine had answered for.
    CHECK(rep.pawnsSampled == 1);
    CHECK(rep.pawnsRefused == 0);
    CHECK(rep.pawnsUntranslated == 0);
    CHECK(st.rows == 1);

    // ...and it is stored under HOLDING, not under the action word. The two enums disagree at this
    // bit -- kLocalHolding is 1<<5, kFillHolding is 1<<6, kFillActionWord is 1<<5 -- so an assignment
    // that 'worked' would have labelled the hold ids as a word nothing sourced.
    CHECK((st.fill & kFillHolding) != 0u);
    CHECK((st.fill & kFillActionWord) == 0u);
    CHECK(st.fill == kFillHolding);
    // The values travelled, so the bit is not on its own.
    CHECK(st.holdL == 0x000A0003u);
    CHECK(st.holdR == 0x000B0004u);
    CHECK(rep.localFillMask == kFillHolding);
}

TEST_CASE("a group this build cannot store is not the accessor refusing, and is counted apart") {
    using namespace replay::capture;

    w2a_r10::HoldingOnlyPool pool;
    // A bit no line translates -- what kLocalHolding WAS until this round, and what the next group
    // added on the engine side would be if it reached a build whose translator predates it.
    pool.engineMask = 1u << 20;
    GatherConfig gc{};
    for (uint16_t i = 0; i < 32; ++i) gc.streamEnabled[i] = true;
    GatherReport rep;
    w2a_r10::RowSink st;
    GatherLocalPawns(&pool, gc, 4321, 2, &w2a_r10::CollectRows, &st, rep);

    CHECK(st.rows == 0);
    CHECK(rep.pawnsRefused == 1);
    // The whole point: the row was lost HERE, not in the engine, and the counts say so. Reading
    // pawnsRefused alone gives L2's wrong answer -- 'the accessor refused' -- for our own defect.
    CHECK(rep.pawnsUntranslated == 1);
    CHECK(rep.localRefusedEngineMask == (1u << 20));
    CHECK(rep.localRefusal == static_cast<uint8_t>(replay::IEntity::LocalRefusal::Ok));
    CHECK(rep.localRefusalKnown);
    CHECK(rep.localActorSource ==
          static_cast<uint8_t>(replay::IEntity::ActorSource::NetGameUserActor));
    CHECK(rep.localEchoComponent == false);

    // And an ACTUAL engine refusal keeps the other story: empty mask, no untranslated count.
    w2a_r10::HoldingOnlyPool declined;
    declined.engineMask = 0;
    GatherReport rep2;
    w2a_r10::RowSink st2;
    GatherLocalPawns(&declined, gc, 4321, 2, &w2a_r10::CollectRows, &st2, rep2);
    CHECK(rep2.pawnsRefused == 1);
    CHECK(rep2.pawnsUntranslated == 0);
    CHECK(rep2.localRefusedEngineMask == 0u);
    CHECK(std::string(replay::LocalRefusalName(
              static_cast<replay::IEntity::LocalRefusal>(rep2.localRefusal))) ==
          "noSourceAnswered");
}

TEST_CASE("L2-RUN-2 F7: a demo uuid is minted, is shaped, and two opens do not collide") {
    using namespace replay::capture;

    // The value every capture ever taken carried, and the one thing a real id may never equal.
    DemoUuid zero{};
    CHECK(DemoUuidPresent(zero) == false);

    const DemoUuid a = DemoUuidFrom(0x01DD3C5DEEF78A85ull, 0x0000A24700000123ull, 1);
    const DemoUuid b = DemoUuidFrom(0x01DD3C5DEEF78A85ull, 0x0000A24700000123ull, 2);
    CHECK(DemoUuidPresent(a));
    CHECK(DemoUuidPresent(b));
    // Two opens inside ONE session differ in the counter alone; that has to be enough.
    CHECK(!(a.lo == b.lo && a.hi == b.hi));
    // Version 4 / variant 1, in the bytes writer.cpp lays down. A reader meeting a 128-bit field
    // needs the two that say how to read it.
    CHECK(((a.lo >> 48) & 0xF0ull) == 0x40ull);
    CHECK(((b.lo >> 48) & 0xF0ull) == 0x40ull);
    CHECK((a.hi & 0xC0ull) == 0x80ull);
    CHECK((b.hi & 0xC0ull) == 0x80ull);
    // A capture that opened at a different instant is a different demo.
    const DemoUuid c = DemoUuidFrom(0x01DD3C5DEEF78A86ull, 0x0000A24700000123ull, 1);
    CHECK(!(a.lo == c.lo && a.hi == c.hi));
}

// ---------------------------------------------------------------------------------------------
// AUDIT_L2 (2026-09-03). Retroactive on the numbers the real launch recorded.
// ---------------------------------------------------------------------------------------------

TEST_CASE("the cost meter can express the cost it killed a capture on") {
    // L2's kill line, verbatim: "median 5000 us, p99 5000 us, max 4506 us". The max is tracked
    // exactly and every quantile came off the histogram, so a median ABOVE the max is not a
    // measurement -- it is a bucket ceiling wearing a point value, over a range whose only rungs were
    // 2000 / 5000 / 20000. The instrument looked like the defect and masked the real one.
    //
    // This drives the same shape: a window whose samples all sit between 2000 and 4506 us.
    CostMeter m;
    m.Init();
    for (uint32_t i = 0; i + 1 < CostMeter::kWindowTicks; ++i) {
        m.AddSampleUs(2000 + i * 4);   // 2000 .. 4040, the measured band
    }
    m.AddSampleUs(4506);               // L2's exact recorded max
    CostMeter::Rung r = CostMeter::Rung::Nominal;
    m.Evaluate(r);
    const CostMeter::WindowStat st = m.LastWindow();

    CHECK(st.maxUs == 4506);
    // THE RETROACTIVE ASSERTION. A quantile off a histogram cannot exceed a maximum tracked exactly;
    // on the shipped-at-L2 bucket table this reads 5000 <= 4506 and fails.
    CHECK(st.medianUs <= st.maxUs);
    // The p99 bucket necessarily brushes the maximum, so only its LOWER bound is a provable
    // statement about the samples. The median is the L2 assertion: its whole interval sits below
    // the exact max once the buckets can express the band.
    CHECK(st.p99LoUs <= st.maxUs);
    // And the median bucket is no wider than its own lower bound -- (2000, 5000] failed that by 1.5x
    // and is what rendered 4506 us of measured cost as "5000".
    CHECK(st.medianUs - st.medianLoUs <= st.medianLoUs);
    // And it is reported as the interval it is, with a lower bound that is a real lower bound.
    CHECK(st.medianLoUs < st.medianUs);
    CHECK(st.medianLoUs >= 2000);
    MESSAGE("L2-shaped window renders median in (" << st.medianLoUs << ", " << st.medianUs
                                                   << "] us, max " << st.maxUs);

    // The kill still fires on this window -- the point of the fix is that the number is arguable,
    // not that a 2-4.5 ms tick becomes acceptable.
    CHECK(st.medianLoUs >= CostMeter::kKillMedianUs);
}

TEST_CASE("a window inside budget is not killed and reports a truthful interval") {
    CostMeter m;
    m.Init();
    for (uint32_t i = 0; i < CostMeter::kWindowTicks; ++i) {
        m.AddSampleUs(7);
    }
    CostMeter::Rung r = CostMeter::Rung::Nominal;
    m.Evaluate(r);
    const CostMeter::WindowStat st = m.LastWindow();
    CHECK(st.medianUs == 8);        // bucket (6, 8]
    CHECK(st.medianLoUs == 6);
    CHECK(st.medianUs <= st.maxUs + 1);
    CHECK(st.medianLoUs < CostMeter::kKillMedianUs);
    CHECK(r != CostMeter::Rung::Killed);
}

TEST_CASE("a marker deriver knows its own denominator before it sees a row") {
    // DIAG-L2: MarkerDeriver::Reset() had no caller in src/, so a capture whose netvar stream was
    // empty logged "0 of 0 watched names were ever seen". The seen-mask exists to separate "no rounds
    // occurred" from "the netvar symbols are wrong"; a denominator of zero answers neither, and zero
    // over zero reads as the innocent case. Priming is a property of the type now.
    MarkerDeriver fresh;
    CHECK(fresh.GetStats().namesWatched == 9);
    CHECK(fresh.GetStats().namesSeenMask == 0);
    CHECK(fresh.GetStats().rowsSeen == 0);
    CHECK(fresh.GetStats().timeRefused == 0);
}

TEST_CASE("a stamp that is not a time is refused as a marker time") {
    // L2 filed three markers at 18446744073709.551 s = UINT64_MAX / 1e6. UINT64_MAX is the engine's
    // own local-event sentinel on the tap delegate's argument 5, which capture was storing in
    // EventRec::stampUs and the lobby deriver was reading as a clock.
    CHECK_FALSE(MarkTimeTrusted(UINT64_MAX));
    CHECK(MarkTimeTrusted(0));
    CHECK(MarkTimeTrusted(18700000ull));           // L2's kMarkMatchBegin, a real source time
    CHECK(MarkTimeTrusted(UINT64_MAX - 1));

    MarkerDeriver d;
    d.NoteTimeRefused();
    d.NoteTimeRefused();
    CHECK(d.GetStats().timeRefused == 2);
    // Reset clears the count with everything else: a refusal belongs to the file it happened in.
    d.Reset();
    CHECK(d.GetStats().timeRefused == 0);
    CHECK(d.GetStats().namesWatched == 9);
}

// ---------------------------------------------------------------------------------------------
// Closing AUDIT_M9 F1, F2, F3 and F4.
// ---------------------------------------------------------------------------------------------
namespace w2a_r11 {

// The kinds that cut a chapter, written out here rather than borrowed from chapterbuilder.cpp's
// IsRoundBoundary. The property under test is "a boundary marker is the first thing in the chapter it
// begins", and a test that asked the shipped predicate which kinds those are would go green the
// moment a kind fell out of it -- which is one of the two ways the guarantee can be lost.
bool CutsAChapter(uint16_t kind) {
    return kind == replay::capture::kMarkMatchBegin || kind == replay::capture::kMarkMatchEnd ||
           kind == replay::capture::kMarkRoundBegin || kind == replay::capture::kMarkRoundEnd;
}

// R9's three-round fixture with ONE difference, and the difference is the whole point: ten ticks are
// consumed BEFORE EmitMatchBegin. That is what production does -- capture.cpp takes ticks during the
// lobby and OpenFile emits MATCH_BEGIN after them (mkdemo.cpp:531 says so in as many words) -- and it
// is what makes EmitMatchBegin's ordering observable at all. With no chapter open, the mark and the
// cut commute: BeginBoundaryChapter only opens, so both orders leave the mark first in chapter 0.
// With a chapter already open the swap moves the mark into the OUTGOING chapter, which is exactly the
// seek defect the split exists to remove, and that is what the file below is read for.
replay::capture::BuilderStats DriveRoundsToFile(const wchar_t* path,
                                                replay::capture::MarkerDeriver::Stats& msOut) {
    using namespace replay::capture;
    using namespace replay;
    using namespace replay::capture_facts;

    const uint64_t t0 = 19166475ull;
    const uint64_t dt = facts::CLOCK_FIXEDSTEP_30HZ_US;
    const uint32_t ticks = 400;

    ManifestBuilder mb;
    mb.Begin(t0, 0, 0x40);
    const uint8_t md5[16] = {0xca, 0xa0, 0x5e, 0xe3};
    mb.SetBuild(md5, 0x676F6C646D617374ull, 0x0102030405060708ull);
    mb.SetLevel(6300205991959903307ull, 0xF00Dull, 1780177787ull);
    mb.SetKeyframeIntervalUs(2000000);
    mb.SetUuid(1, 2);
    REQUIRE(mb.Refusal() == nullptr);

    DemoWriter writer;
    REQUIRE(writer.Open(path, mb.View()));

    BuilderConfig cfg;
    cfg.keyframeIntervalUs = 2000000;
    for (uint16_t i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;
    for (uint16_t i = 0; i < kSectionCount; ++i) cfg.sectionEnabled[i] = true;

    ChapterBuilder b;
    REQUIRE(b.Open(&writer, mb.View(), cfg));

    uint32_t seq = 500;
    auto tick = [&](uint32_t f) {
        TickRec tr{};
        tr.sourceTimeUs = t0 + f * dt;
        tr.simTimeUs = tr.sourceTimeUs;
        tr.frameIndex = f + 1;
        tr.frameDeltaUs = (uint32_t)dt;
        tr.scaledDeltaUs = (uint32_t)dt;
        w2a_r7::FeedRec(b, kRecTick, tr);
    };

    // The lobby. A chapter is open and carrying records by the time the match begins.
    for (uint32_t f = 0; f < 10; ++f) tick(f);
    b.EmitMatchBegin(t0 + 10 * dt);

    for (uint32_t f = 10; f < ticks; ++f) {
        const uint64_t st = t0 + f * dt;
        tick(f);
        if (f == 20) {
            w2a_r9::FeedNetvar(b, st, seq++, kNvFrisbeeOwnerId,
                       0x00000000FFFFFFFFull);   // facts-ok: PHYS.ID_DEAD the disc's unowned OWNER id, not that offset
            w2a_r9::FeedNetvar(b, st, seq++, kNvCountdownTimer, ScalarOf(0));
        }
        if (f == 100 || f == 200 || f == 290) {
            w2a_r9::FeedNetvar(b, st, seq++, kNvFirstLaunchOfRound, ScalarOf(1));
        }
        if (f == 101 || f == 201 || f == 291) {
            w2a_r9::FeedNetvar(b, st, seq++, kNvFirstLaunchOfRound, ScalarOf(0));
        }
    }

    b.EmitMatchEnd(t0 + ticks * dt);
    msOut = b.MarkerStats();
    b.FlushChapter();
    const BuilderStats out = b.Stats();
    b.Close();
    writer.Close();
    return out;
}

}  // namespace w2a_r11

TEST_CASE("Every boundary marker is the FIRST record of the chapter it begins") {
    using namespace replay::capture;

    w2a_r7::TempDemoFile tmp(L"r11bounds");
    MarkerDeriver::Stats ms{};
    const BuilderStats out = w2a_r11::DriveRoundsToFile(tmp.path, ms);

    // The counters R9's case asserts, kept only so a failure here can be told from a fixture that
    // stopped deriving rounds. They are NOT the property: both mutants below leave them identical.
    CHECK(ms.emitted == 6);
    CHECK(out.roundBoundarySplits >= 4);

    // THE PROPERTY, read from the emitted bytes. "The cut happens BEFORE the mark is appended, so the
    // boundary is the first thing after a keyframe rather than a row a seek has to decode forward to
    // reach" -- ENG-W2A-R9's own words for why the order is what it is, and nothing tested it. A seek
    // to a round marker must land on the chapter that marker opens: the marker's time IS the chapter's
    // begin time, and no other record of the mark stream precedes it there.
    DemoReader r;
    REQUIRE(r.Open(tmp.path));
    uint32_t boundaries = 0, matchBegins = 0, roundBegins = 0;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        DecodedChapter c{};
        if (!r.DecodeChapter(i, c)) {
            continue;
        }
        const ChapterKey key = r.ChapterAt(i);
        const StreamView* v = c.Find(kStreamMark);
        if (!v || v->records == nullptr) {
            continue;
        }
        const size_t n = v->bytes / sizeof(MarkRecordDisk);
        for (size_t j = 0; j < n; ++j) {
            MarkRecordDisk d{};
            memcpy(&d, v->records + j * sizeof d, sizeof d);
            if (!w2a_r11::CutsAChapter(d.kind)) {
                continue;
            }
            ++boundaries;
            if (d.kind == kMarkMatchBegin) ++matchBegins;
            if (d.kind == kMarkRoundBegin) ++roundBegins;
            // Its chapter STARTS at it...
            CHECK(d.tUs == key.tBeginUs);
            // ...and nothing in the chapter's mark stream comes before it.
            CHECK(j == 0u);
            if (d.tUs != key.tBeginUs || j != 0u) {
                MESSAGE("boundary kind " << d.kind << " at " << d.tUs << " is record " << j
                        << " of chapter " << i << " which begins at " << key.tBeginUs
                        << ": a seek to this marker lands in the previous chapter and must decode "
                           "forward from its keyframe");
            }
        }
    }
    // The check cannot pass by finding nothing. One MATCH_BEGIN, one MATCH_END, three ROUND_BEGIN.
    CHECK(boundaries == 5);
    CHECK(matchBegins == 1);
    CHECK(roundBegins == 3);
}

// ---------------------------------------------------------------------------------------------
// F2 / F3 / F4: the gather's own report. One rig, three properties.
// ---------------------------------------------------------------------------------------------
namespace w2a_r11 {

// A group bit IEntity does not declare today. It stands in for the NEXT bit W1-E adds -- the case
// LocalMaskToFill has no line for -- without waiting for one to exist, which is the only way to test
// the loss path at all while the table is total.
constexpr uint32_t kUnknownGroupBit = 1u << 7;
static_assert((replay::kLocalAll & kUnknownGroupBit) == 0,
              "kLocalAll grew a bit this fixture uses as its UNtranslatable one; pick a free bit");

struct MixedPool {
    uint32_t engineMask = replay::kLocalPose | kUnknownGroupBit;
    bool disagree = false;
    uint32_t asked = 0;

    bool Available() const { return true; }
    size_t LocalPlayerCount() const { return 1; }
    replay::UserIndex LocalPlayerAt(size_t) const { return replay::UserIndex(3); }

    template <class S, class R>
    uint32_t ReadLocalPlayerDetailed(replay::UserIndex, S& st, R& r) {
        ++asked;
        if (engineMask & replay::kLocalPose) {
            st.hmdOri[0] = 0.5f;
        }
        r.fill = engineMask;
        r.refusal = engineMask ? replay::IEntity::LocalRefusal::Ok
                               : replay::IEntity::LocalRefusal::NoSourceAnswered;
        r.actorSource = replay::IEntity::ActorSource::BroadcasterKeys;
        r.echoComponent = true;
        r.actorSourcesDisagree = disagree;
        return engineMask;
    }
};

}  // namespace w2a_r11

TEST_CASE("A group dropped from a row that WAS written is counted and named") {
    using namespace replay::capture;

    w2a_r11::MixedPool pool;
    GatherConfig gc{};
    for (uint16_t i = 0; i < 32; ++i) gc.streamEnabled[i] = true;
    GatherReport rep;
    w2a_r10::RowSink st;
    GatherLocalPawns(&pool, gc, 4321, 2, &w2a_r10::CollectRows, &st, rep);

    // The row exists, so none of the refusal counters can see this pawn at all. This is the shape
    // that shipped: a file that quietly lacks a group for every frame of the match.
    CHECK(st.rows == 1);
    CHECK(rep.pawnsSampled == 1);
    CHECK(rep.pawnsRefused == 0);
    CHECK(rep.pawnsUntranslated == 0);
    CHECK(st.fill == kFillPose);

    // ...and the loss is a number, with the bit named.
    CHECK(rep.pawnsGroupLoss == 1);
    CHECK(rep.rowsWithGroupLoss == 1);
    CHECK(rep.lostLocalGroupBits == w2a_r11::kUnknownGroupBit);
    MESSAGE("dropped group bits " << rep.lostLocalGroupBits << " named as "
            << replay::LocalGroupName(rep.lostLocalGroupBits));

    // TOTAL loss still lands where it did, so the new counter is not the old one renamed: the
    // refusal arm keeps its own meaning and the log line that reads it keeps saying what it said.
    w2a_r11::MixedPool onlyLost;
    onlyLost.engineMask = w2a_r11::kUnknownGroupBit;
    GatherReport rep2;
    w2a_r10::RowSink st2;
    GatherLocalPawns(&onlyLost, gc, 4321, 2, &w2a_r10::CollectRows, &st2, rep2);
    CHECK(st2.rows == 0);
    CHECK(rep2.pawnsRefused == 1);
    CHECK(rep2.pawnsUntranslated == 1);
    CHECK(rep2.pawnsGroupLoss == 1);
    CHECK(rep2.rowsWithGroupLoss == 0);
}

TEST_CASE("Actor sources that disagree on a SUCCESSFUL read are reported") {
    using namespace replay::capture;

    w2a_r11::MixedPool pool;
    pool.engineMask = replay::kLocalAll;   // every group fills: this read SUCCEEDS
    pool.disagree = true;
    GatherConfig gc{};
    for (uint16_t i = 0; i < 32; ++i) gc.streamEnabled[i] = true;
    GatherReport rep;
    w2a_r10::RowSink st;
    GatherLocalPawns(&pool, gc, 4321, 2, &w2a_r10::CollectRows, &st, rep);

    // ENG-W1E-R5's CLIENT rig is this: two sources naming different actors, the first one wins, all
    // six groups fill. Nothing about it is a refusal, which is why the refusal-only report said
    // nothing and the capture could record a whole match from an actor the other source did not name.
    CHECK(st.rows == 1);
    CHECK(rep.pawnsRefused == 0);
    CHECK(rep.localSourcesDisagree == false);   // the refusal-path field, correctly untouched
    CHECK(rep.localDisagreements == 1);
    CHECK(rep.localDisagreeKnown);
    CHECK(rep.localDisagreeSource ==
          static_cast<uint8_t>(replay::IEntity::ActorSource::BroadcasterKeys));

    // Agreement stays silent, so the counter is a report of the disagreement and not of the read.
    w2a_r11::MixedPool agree;
    agree.engineMask = replay::kLocalAll;
    GatherReport rep2;
    w2a_r10::RowSink st2;
    GatherLocalPawns(&agree, gc, 4321, 2, &w2a_r10::CollectRows, &st2, rep2);
    CHECK(st2.rows == 1);
    CHECK(rep2.localDisagreements == 0);
    CHECK_FALSE(rep2.localDisagreeKnown);
}

TEST_CASE("The group table is total, asked of the translator") {
    // The build-time statement is EveryLocalGroupTranslates() beside LocalMaskToFill; this is the
    // same question asked at runtime so the property is legible in a test run as well, and so a
    // reader can see WHICH bits the derived set holds.
    using namespace replay::capture;
    CHECK((replay::kLocalAll & ~kLocalBitsWithFill) == 0u);
    CHECK(kLocalBitsWithFill != 0u);
    for (uint32_t bit = 1; bit != 0; bit <<= 1) {
        if ((replay::kLocalAll & bit) == 0) continue;
        CHECK_MESSAGE(LocalMaskToFill(bit) != 0u,
                      "kLocal bit " << replay::LocalGroupName(bit) << " translates to fill 0");
    }
    // And the one bit the file side has that no engine group may claim: kFillActionWord is 1<<5 on the
    // file side and kLocalHolding is 1<<5 on the engine side, so a table that ASSIGNED rather than
    // translated would put hold ids under a word nothing sourced.
    CHECK((LocalMaskToFill(replay::kLocalAll) & kFillActionWord) == 0u);
}


// ---------------------------------------------------------------------------------------------
// AUDIT_ARCH21_PACKETS BLOCKER-1 / BLOCKER-2 (2026-09-04). Both defects live in the PER-FILE
// lifecycle rather than in a derivation rule, so reaching them needs a second file on the same
// builder, or a mark derived before the source choice has settled. No existing case does either.
//
// Ticks are fed throughout because a netvar record is a FOREIGN stamp: it rides the last source
// time, so the arm-up window only advances while the axis does.
// ---------------------------------------------------------------------------------------------
namespace w2a_markers_r2 {

using namespace replay;
using namespace replay::capture;
using namespace replay::capture_facts;

// One file, opened on a builder that may already have run a match. Always-on capture writes one file
// per match through the SAME ChapterBuilder, which is the lifecycle BLOCKER-1 lived in.
struct MatchFile {
    w2a_r7::TempDemoFile tmp;
    ManifestBuilder mb;
    DemoWriter writer;

    explicit MatchFile(const wchar_t* tag) : tmp(tag) {}
    ~MatchFile() { writer.Close(); }

    void Open(ChapterBuilder& b, uint64_t t0) {
        mb.Begin(t0, 0, 0x40);
        const uint8_t md5[16] = {0xca, 0xa0, 0x5e, 0xe3};
        mb.SetBuild(md5, 0x676F6C646D617374ull, 0x0102030405060708ull);
        mb.SetLevel(6300205991959903307ull, 0xF00Dull, 1780177787ull);
        mb.SetKeyframeIntervalUs(2000000);
        mb.SetUuid(1, 2);
        REQUIRE(mb.Refusal() == nullptr);
        REQUIRE(writer.Open(tmp.path, mb.View()));
        BuilderConfig cfg;
        cfg.keyframeIntervalUs = 2000000;
        for (uint16_t i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;
        for (uint16_t i = 0; i < kSectionCount; ++i) cfg.sectionEnabled[i] = true;
        REQUIRE(b.Open(&writer, mb.View(), cfg));
    }
};

const uint64_t kDt = facts::CLOCK_FIXEDSTEP_30HZ_US;
// The engine's own all-ones absence value for the disc's owner, zero-extended into the netvar word.
const uint64_t kDiscUnowned = gamestate::kFrisbeeUnowned;
const uint32_t kMatchTicks = 120;

void Tick(ChapterBuilder& b, uint64_t t0, uint32_t f) {
    TickRec tr{};
    tr.sourceTimeUs = t0 + f * kDt;
    tr.simTimeUs = tr.sourceTimeUs;
    tr.frameIndex = f + 1;
    tr.frameDeltaUs = static_cast<uint32_t>(kDt);
    tr.scaledDeltaUs = static_cast<uint32_t>(kDt);
    w2a_r7::FeedRec(b, kRecTick, tr);
}

TEST_CASE("BLOCKER-1: match 2 of one session starts from a baseline, not from match 1's state") {
    // MarkerDeriver::Reset() had no caller in src/. Open() cleared identity, `local` and `choice`
    // and left the WIRE deriver holding match 1's watched values, so on match 2's first netvar
    // record every name already read `seen`: a first_launch_of_round arriving at 0 was a 1 -> 0
    // EDGE instead of a first sighting, and the deriver emitted LAUNCH_RELEASE + ROUND_BEGIN at an
    // instant where no round began. The ROUND_BEGIN cuts a chapter, so the invented boundary becomes
    // a seek target in the timeline the owner scrubs.
    ChapterBuilder b;
    const uint64_t t1 = 19166475ull;
    uint32_t seq = 500;
    {
        MatchFile f1(L"r2m1");
        f1.Open(b, t1);
        b.EmitMatchBegin(t1);
        // Match 1 arms the conjunction and releases, so this file ends with first_launch_of_round
        // at 0, the disc unowned, the countdown at zero and one round already counted.
        for (uint32_t f = 0; f < kMatchTicks; ++f) {
            Tick(b, t1, f);
            if (f == 5) {
                w2a_r9::FeedNetvar(b, t1 + f * kDt, seq++, kNvFrisbeeOwnerId, kDiscUnowned);
                w2a_r9::FeedNetvar(b, t1 + f * kDt, seq++, kNvCountdownTimer, ScalarOf(0));
            }
            if (f == 100) w2a_r9::FeedNetvar(b, t1 + f * kDt, seq++, kNvFirstLaunchOfRound, ScalarOf(1));
            if (f == 101) w2a_r9::FeedNetvar(b, t1 + f * kDt, seq++, kNvFirstLaunchOfRound, ScalarOf(0));
        }
        CHECK(b.MarkerStats().emitted == 2);          // LAUNCH_RELEASE + ROUND_BEGIN, correctly
        CHECK(b.Stats().roundBoundarySplits == 1);
        b.EmitMatchEnd(t1 + kMatchTicks * kDt);
        b.Close();
    }

    MatchFile f2(L"r2m2");
    const uint64_t t2 = t1 + (kMatchTicks + 1) * kDt;
    f2.Open(b, t2);
    // Reset means PRIMED, not zeroed: the denominator that separates "no rounds occurred" from
    // "the netvar symbols are wrong" has to survive the reset that clears the values.
    CHECK(b.MarkerStats().emitted == 0);
    CHECK(b.MarkerStats().rowsSeen == 0);
    CHECK(b.MarkerStats().namesSeenMask == 0);
    CHECK(b.MarkerStats().namesWatched == 9);
    b.EmitMatchBegin(t2);

    // Match 2's first netvar traffic carries exactly what match 1 left behind, including
    // first_launch_of_round == 0. No round has begun, so no marker has.
    for (uint32_t f = 0; f < 3; ++f) {
        Tick(b, t2, f);
        if (f == 1) {
            w2a_r9::FeedNetvar(b, t2 + f * kDt, seq++, kNvFrisbeeOwnerId, kDiscUnowned);
            w2a_r9::FeedNetvar(b, t2 + f * kDt, seq++, kNvCountdownTimer, ScalarOf(0));
        }
        if (f == 2) w2a_r9::FeedNetvar(b, t2 + f * kDt, seq++, kNvFirstLaunchOfRound, ScalarOf(0));
    }
    CHECK(b.MarkerStats().rowsSeen == 3);
    CHECK(b.MarkerStats().emitted == 0);
    CHECK(b.Stats().roundBoundarySplits == 0);

    // And match 2's own first edge still works, so the reset did not merely silence the deriver.
    for (uint32_t f = 3; f < kMatchTicks; ++f) {
        Tick(b, t2, f);
        if (f == 100) w2a_r9::FeedNetvar(b, t2 + f * kDt, seq++, kNvFirstLaunchOfRound, ScalarOf(1));
        if (f == 101) w2a_r9::FeedNetvar(b, t2 + f * kDt, seq++, kNvFirstLaunchOfRound, ScalarOf(0));
    }
    CHECK(b.MarkerStats().emitted == 2);
    CHECK(b.Stats().roundBoundarySplits == 1);
    b.EmitMatchEnd(t2 + kMatchTicks * kDt);
    b.Close();
}

TEST_CASE("BLOCKER-1: the round index restarts with the file, so match 2's first round is round 0") {
    // The builder case above cannot see the round NUMBER, and roundIndex_ is the other half of the
    // state that used to survive a file: MatchEnd stamps m.index from it, so match 2's rows were
    // numbered from where match 1 stopped.
    MarkerDeriver d;
    MarkRec out[8];
    auto note = [&](uint64_t name, uint64_t value, uint64_t tUs) {
        NetvarMessage nm{};
        nm.varCount = 1;
        nm.rows[0].name = name;
        nm.rows[0].value = value;
        return d.Note(nm, tUs, out, 8);
    };

    note(kNvFrisbeeOwnerId, kDiscUnowned, 10);
    note(kNvCountdownTimer, ScalarOf(0), 10);
    note(kNvFirstLaunchOfRound, ScalarOf(1), 20);
    REQUIRE(note(kNvFirstLaunchOfRound, ScalarOf(0), 30) == 2);
    CHECK(out[1].kind == kMarkRoundBegin);
    CHECK(out[1].index == 0);
    note(kNvFirstLaunchOfRound, ScalarOf(1), 40);
    REQUIRE(note(kNvFirstLaunchOfRound, ScalarOf(0), 50) == 2);
    CHECK(out[1].index == 1);

    d.Reset();
    // The next FILE, not the next round. Its first sighting of each name is a baseline again -- the
    // 0 below is the value match 1 ended on, and it must derive nothing -- and when a round does
    // begin it is round 0.
    note(kNvFrisbeeOwnerId, kDiscUnowned, 100);
    note(kNvCountdownTimer, ScalarOf(0), 100);
    CHECK(note(kNvFirstLaunchOfRound, ScalarOf(0), 110) == 0);
    note(kNvFirstLaunchOfRound, ScalarOf(1), 120);
    REQUIRE(note(kNvFirstLaunchOfRound, ScalarOf(0), 130) == 2);
    CHECK(out[1].index == 0);
    CHECK(d.MatchEnd(140).index == 1);
}

TEST_CASE("L2-RUN-7 F1: S_GAMESTATE reaches the file as 84-byte records, not as an empty block") {
    // Every S_GAMESTATE block ever written was 4 bytes of zeros. DispatchRecord appends the record
    // with AppendRaw, which advances the buffer's BYTE cursor, and the flush measured (bits + 7) / 8
    // for every stream IsContainerFramed() did not name -- so the records went into the buffer and
    // were thrown away, and `echodemo verify` raised 25 fails on the only capture that ever produced
    // the stream. The existing gamestate coverage could not see it: tests/container drives the
    // WRITER directly with its own packed array, and mkdemo deliberately feeds no gamestate records,
    // so nothing anywhere took a sample through the BUILDER and read the bytes back.
    ChapterBuilder b;
    MatchFile f(L"r13gs");
    const uint64_t t0 = 19166475ull;
    f.Open(b, t0);
    b.EmitMatchBegin(t0);

    const uint32_t kSamples = 5;
    for (uint32_t i = 0; i < kSamples; ++i) {
        Tick(b, t0, i);
        GameStateRec gs{};
        gs.sourceTimeUs = t0 + i * kDt;
        gs.sample.engineTimeUs = t0 + i * kDt;
        gs.sample.captureTimeUs = t0 + i * kDt;
        gs.sample.availability = static_cast<uint8_t>(gamestate::Availability::Ok);
        gs.sample.present = 0x67Du;          // the mask the field run reported
        gs.sample.phase = 3u + i;            // MOVES per sample: a fixed value cannot tell record 1
        gs.sample.roundCounter = 0x10001u + i;  // from record 4 if the file holds one of them twice
        gs.sample.bluePoints = 2u + i;
        w2a_r7::FeedRec(b, kRecGameState, gs);
    }
    b.EmitMatchEnd(t0 + kSamples * kDt);
    b.Close();
    f.writer.Close();
    CHECK(b.Stats().gameStateRecords == kSamples);
    CHECK(b.Stats().blockWidthMismatch == 0);

    // Read back with the SHIPPED reader, and with the format's own sizeof -- not a width this test
    // wrote down. A block that is not a whole number of records is what the writer refuses, so a
    // test carrying its own 84 would agree with a broken file about the wrong number.
    DemoReader rd;
    REQUIRE(rd.Open(f.tmp.path));
    std::vector<GameStateRecordDisk> back;
    uint32_t blockBytes = 0, declared = 0;
    for (size_t i = 0; i < rd.ChapterCount(); ++i) {
        DecodedChapter c{};
        if (!rd.DecodeChapter(i, c)) continue;
        const StreamView* v = c.Find(kStreamGameState);
        if (!v) continue;
        blockBytes += static_cast<uint32_t>(v->bytes);
        declared += v->storedCount;
        REQUIRE(v->bytes % sizeof(GameStateRecordDisk) == 0);
        for (size_t o = 0; o + sizeof(GameStateRecordDisk) <= v->bytes;
             o += sizeof(GameStateRecordDisk)) {
            GameStateRecordDisk one{};
            memcpy(&one, v->records + o, sizeof one);
            back.push_back(one);
        }
    }
    // ★ THE ASSERTION THE OLD CODE FAILS. It measured 4 bytes (the bit-length prefix and nothing
    // else) and declared 5 records: the count and the payload disagreed and no layer said so.
    CHECK(blockBytes == kSamples * sizeof(GameStateRecordDisk));
    CHECK(declared == kSamples);
    REQUIRE(back.size() == kSamples);
    for (uint32_t i = 0; i < kSamples; ++i) {
        CHECK(back[i].roundCounter == 0x10001u + i);
        CHECK(back[i].phase == 3u + i);
        CHECK(back[i].present == 0x67Du);
        CHECK(back[i].bluePoints == 2u + i);
    }
}

TEST_CASE("BLOCKER-2: wire marks derived during the arm-up window never reach a LOCAL file") {
    // The window was honoured on the gamestate path and ignored on the netvar path, whose only
    // suppression was the Local case. For the whole window Source() is NotRecorded -- which is not
    // Local -- so wire marks were appended and their boundaries cut chapters; a local sample a tick
    // later then settled the file to `local`, and the file held two providers' markers under one
    // markerSource word that MarkRec carries no field to separate.
    ChapterBuilder b;
    MatchFile f(L"r2armup");
    const uint64_t t0 = 19166475ull;
    f.Open(b, t0);
    b.EmitMatchBegin(t0);

    uint32_t seq = 700;
    // A complete wire round boundary, entirely inside the 2 s window.
    for (uint32_t fr = 0; fr < 21; ++fr) {
        Tick(b, t0, fr);
        if (fr == 5) {
            w2a_r9::FeedNetvar(b, t0 + fr * kDt, seq++, kNvFrisbeeOwnerId, kDiscUnowned);
            w2a_r9::FeedNetvar(b, t0 + fr * kDt, seq++, kNvCountdownTimer, ScalarOf(0));
        }
        if (fr == 10) w2a_r9::FeedNetvar(b, t0 + fr * kDt, seq++, kNvFirstLaunchOfRound, ScalarOf(1));
        if (fr == 20) w2a_r9::FeedNetvar(b, t0 + fr * kDt, seq++, kNvFirstLaunchOfRound, ScalarOf(0));
    }
    CHECK(b.MarkerStats().emitted == 2);            // derived, and held
    CHECK(b.ChosenMarkerSource() == MarkerSource::NotRecorded);
    CHECK(b.Stats().roundBoundarySplits == 0);      // nothing cut a chapter
    CHECK(b.Stats().wireMarksSuppressed == 0);      // and nothing has been decided yet either

    // The first readable local sample, still inside the window. LOCAL is primary, so the held wire
    // marks are discarded rather than filed, and the count says the corroborator had something to
    // say that the file did not file.
    Tick(b, t0, 25);
    GameStateRec gs{};
    gs.sourceTimeUs = t0 + 25 * kDt;
    gs.sample.engineTimeUs = t0 + 25 * kDt;
    gs.sample.captureTimeUs = t0 + 25 * kDt;
    gs.sample.availability = static_cast<uint8_t>(gamestate::Availability::Ok);
    w2a_r7::FeedRec(b, kRecGameState, gs);

    CHECK(b.ChosenMarkerSource() == MarkerSource::Local);
    CHECK(b.Stats().wireMarksSuppressed == 2);
    CHECK(b.Stats().roundBoundarySplits == 0);
    CHECK(b.Stats().boundaryMarksNotCut == 0);
    CHECK(b.Stats().wireMarksHeldDropped == 0);

    b.EmitMatchEnd(t0 + 60 * kDt);
    b.Close();
    // And the sentinel's word is the FINAL choice: capture.cpp reads this after Close().
    CHECK(b.ChosenMarkerSource() == MarkerSource::Local);
}

TEST_CASE("BLOCKER-2: held wire marks are filed, not lost, when the file turns out to be WIRE") {
    // The other side of the hold. A capture with no readable local source waits out the window and
    // settles Wire, and the marks derived while it waited are real edges in a file that has no other
    // producer -- discarding them would trade one silent wrong for another.
    ChapterBuilder b;
    MatchFile f(L"r2wire");
    const uint64_t t0 = 19166475ull;
    f.Open(b, t0);
    b.EmitMatchBegin(t0);

    uint32_t seq = 800;
    for (uint32_t fr = 0; fr < 101; ++fr) {
        Tick(b, t0, fr);
        if (fr == 5) {
            w2a_r9::FeedNetvar(b, t0 + fr * kDt, seq++, kNvFrisbeeOwnerId, kDiscUnowned);
            w2a_r9::FeedNetvar(b, t0 + fr * kDt, seq++, kNvCountdownTimer, ScalarOf(0));
        }
        if (fr == 10) w2a_r9::FeedNetvar(b, t0 + fr * kDt, seq++, kNvFirstLaunchOfRound, ScalarOf(1));
        if (fr == 20) w2a_r9::FeedNetvar(b, t0 + fr * kDt, seq++, kNvFirstLaunchOfRound, ScalarOf(0));
        if (fr == 20) {
            CHECK(b.MarkerStats().emitted == 2);
            CHECK(b.ChosenMarkerSource() == MarkerSource::NotRecorded);
            CHECK(b.Stats().wireMarksSuppressed == 0);
        }
        // Past the window, with no local sample ever: this record settles the choice to Wire and the
        // held pair goes into the stream on the spot. Its blue_score is a first sighting, so it is a
        // baseline and derives nothing of its own -- the two marks below are the held ones.
        if (fr == 100) w2a_r9::FeedNetvar(b, t0 + fr * kDt, seq++, kNvBlueScore, ScalarOf(0));
    }
    CHECK(b.ChosenMarkerSource() == MarkerSource::Wire);
    CHECK(b.MarkerStats().emitted == 2);
    CHECK(b.Stats().wireMarksSuppressed == 0);
    CHECK(b.Stats().wireMarksHeldDropped == 0);
    // The held ROUND_BEGIN belongs 80 ticks back, which the open chapter has already run past, so it
    // is filed WITHOUT re-cutting: a chapter beginning before the previous one ended would break the
    // seek index the boundary exists for, and losing the boundary entirely is worse than sharing a
    // chapter with it.
    CHECK(b.Stats().boundaryMarksNotCut == 1);
    CHECK(b.Stats().roundBoundarySplits == 0);

    b.EmitMatchEnd(t0 + kMatchTicks * kDt);
    b.Close();
    CHECK(b.ChosenMarkerSource() == MarkerSource::Wire);
}

}  // namespace w2a_markers_r2

// L3-RUN-6 F1, the half of it that lives in the writer. L2 #7's capture reports `duration 68.333 s`
// against the transport's `49.433 s`, and 68.333 - t0(18.899811) == 49.433: the manifest's span is
// measured from a chapter that begins at 0, 18.9 s before the file was opened. That chapter is opened
// by a foreign-stamped record -- one whose time `StampIsOnSourceAxis` declares is NOT the capture axis
// -- arriving before the first tick, when `lastSourceUs_` was still 0.
TEST_CASE("L3-RUN-6 F1: a foreign-stamped record cannot place a chapter before t0, and the "
          "manifest's span is the transport's") {
    using namespace replay::capture;
    using namespace replay;

    const uint64_t t0 = 18899811ull;       // L2 #7's own t0_us
    const uint64_t foreignUs = 1874427ull; // the arrival stamp its JOIN/TEAM_CHANGE markers carried
    const uint64_t dt = facts::CLOCK_FIXEDSTEP_30HZ_US;
    // Ten seconds of file at the capture's own fixed step. Derived from `dt` rather than written as
    // the 300 it works out to: the bare count collides with a gated address fact, and it is the
    // duration that matters here, not the number of records that fill it.
    const uint32_t ticks = 10u * static_cast<uint32_t>(1000000ull / dt);
    const uint64_t tEnd = t0 + uint64_t(ticks - 1) * dt;

    w2a_r7::TempDemoFile tmp(L"r14-axis");
    ManifestBuilder mb;
    mb.Begin(t0, 1, 0x40);
    const uint8_t md5[16] = {0xca, 0xa0, 0x5e, 0xe3};
    mb.SetBuild(md5, 0x676F6C646D617374ull, 0x0102030405060708ull);
    mb.SetLevel(6300205991959903307ull, 0xF00Dull, 1780177787ull);
    mb.SetKeyframeIntervalUs(2000000);
    mb.SetUuid(1, 2);
    REQUIRE(mb.Refusal() == nullptr);

    DemoWriter writer;
    REQUIRE(writer.Open(tmp.path, mb.View()));

    BuilderConfig cfg;
    cfg.keyframeIntervalUs = 2000000;
    for (uint16_t i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;
    for (uint16_t i = 0; i < kSectionCount; ++i) cfg.sectionEnabled[i] = true;

    ChapterBuilder b;
    REQUIRE(b.Open(&writer, mb.View(), cfg));

    // The lobby body arrives before the first tick, carrying the tap's arrival microsecond.
    EventRec er{};
    er.stampUs = foreignUs;
    er.dst = 0;
    er.typeIndex = 1;
    er.bodyBytes = 0;
    w2a_r7::FeedRec(b, kRecEvent, er);

    b.EmitMatchBegin(t0);
    for (uint32_t f = 0; f < ticks; ++f) {
        TickRec tr{};
        tr.sourceTimeUs = t0 + uint64_t(f) * dt;
        tr.simTimeUs = tr.sourceTimeUs;
        tr.frameIndex = f + 1;
        tr.frameDeltaUs = static_cast<uint32_t>(dt);
        tr.scaledDeltaUs = static_cast<uint32_t>(dt);
        w2a_r7::FeedRec(b, kRecTick, tr);
    }
    b.EmitMatchEnd(tEnd);
    b.FlushChapter();
    b.Close();
    writer.Close();

    DemoReader r;
    REQUIRE(r.Open(tmp.path));
    REQUIRE(r.ChapterCount() > 0);

    // tMin/tMax exactly as `echodemo info` derives the manifest's duration.
    uint64_t tMin = ~0ull, tMax = 0;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        const ChapterKey k = r.ChapterAt(i);
        if (k.tBeginUs < tMin) tMin = k.tBeginUs;
        if (k.tEndUs > tMax) tMax = k.tEndUs;
    }
    CHECK(tMin == t0);
    CHECK(tMax == tEnd);
    // The manifest's span IS the transport's span: `tEnd - t0`, the same number playback loads.
    CHECK(tMax - tMin == tEnd - t0);
    if (tMin != t0) {
        MESSAGE("first chapter begins at " << tMin << ", " << (t0 - tMin)
                << " us before the file was opened: `echodemo info` will report a duration that long "
                   "over the transport's, and every marker time is read on a base the file never had");
    }

    // No marker may predate the file: a marker at t < t0 reads back negative through the UI's
    // `tUs - t0Us` and `round`/`goto round` clamp to the first tick (L3-RUN-6 rows 27/28).
    size_t marks = 0;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        DecodedChapter c{};
        if (!r.DecodeChapter(i, c)) continue;
        const StreamView* v = c.Find(kStreamMark);
        if (!v || v->records == nullptr) continue;
        const size_t n = v->bytes / sizeof(MarkRecordDisk);
        for (size_t j = 0; j < n; ++j) {
            MarkRecordDisk d{};
            memcpy(&d, v->records + j * sizeof d, sizeof d);
            ++marks;
            CHECK(d.tUs >= t0);
        }
    }
    CHECK(marks >= 2u);
}

// L2 #15 F38. The close line said `7080 refusals` and nothing else: a count with no field name is
// not a diagnosis, and the field that actually refuses (`bits11`) is not the one the packet brief
// guessed at (`flags`). What is asserted here is the NAMING -- that the encoder refuses at all was
// never in doubt.
TEST_CASE("L2 #15 F38: a refused body names the FIELD, its value and its declared width") {
    // The width is READ from the wire schedule, never typed here: a test that spells `11` still
    // passes against a codec that has renumbered or rewidened the field it is about.
    const bitio::FieldSpec bits = bitio::SpecOf(bitio::kSimStateWire[0], 0);
    const uint32_t width = bits.rawBits;
    CHECK(width > 0u);

    BodyFrame f{};
    // One bit above what the field declares. entity.h:52-58 is the real cause in the field: the
    // engine's CI flags word carries bits above the eleven the sim state declares for `bits`, so
    // the word the capture holds does not fit the field the format declares for it.
    const uint64_t tooWide = (uint64_t(1) << width);
    f.bits11 = static_cast<uint32_t>(tooWide);

    uint8_t buf[512] = {};
    bitio::BitWriter w(buf, sizeof buf);
    BodyEncoder enc{true};
    enc.Reset();
    const bool ok = enc.Encode(w, 1, 1, f);
    CHECK_FALSE(ok);

    const RefusedField& r = enc.LastRefusal();
    CHECK(r.identified);
    REQUIRE(r.name != nullptr);
    CHECK(std::string(r.name) == std::string("bits11"));
    CHECK(r.value == tooWide);
    CHECK(r.width == width);

    // The close line's rate limit is the marker source's, not a second magnitude invented for this
    // path: markers.h states the rule and the reason for that number.
    CHECK(BuilderStats::kMaxRefusalsNamed == LocalMarkerSource::kMaxRefusalsNamed);
}

// ---------------------------------------------------------------------------------------------
// the identity block -- its version rule in BOTH directions, and where its offsets come
// from.  Nothing read this block before this packet, so these cases are the format's first reader
// meeting its own bytes rather than a regression net around an existing one.
// ---------------------------------------------------------------------------------------------
namespace {

// A v1 block, laid out from v1's OWN constants.  It deliberately does not use
// sizeof(IdentityLoadoutSlot): that is today's width, and the whole hazard is that v1's differs.
std::vector<uint8_t> BuildV1Block(uint64_t tUs, uint64_t contentHash,
                                  const std::vector<IdentityEntrant>& people,
                                  const std::vector<std::string>& json,
                                  const std::vector<uint64_t>& firstItem) {
    std::vector<uint8_t> b;
    auto put = [&](const void* p, size_t n) {
        const uint8_t* q = static_cast<const uint8_t*>(p);
        b.insert(b.end(), q, q + n);
    };
    IdentitySnapshotHeader h{};
    h.magic = kIdentityBlockMagic;
    h.version = kIdentityBlockVersionV1;
    h.tUs = tUs;
    h.entrantCount = static_cast<uint16_t>(people.size());
    h.contentHash = contentHash;
    put(&h, kIdentityHeaderSizeV1);   // v1 stopped here: it recorded neither stride nor count
    for (size_t i = 0; i < people.size(); ++i) {
        put(&people[i], kIdentityEntrantSizeV1);
        for (uint16_t s = 0; s < kIdentitySlotCount; ++s) {
            uint8_t rec[kIdentitySlotStrideV1] = {};
            const uint16_t slotIndex = s;
            const uint64_t itemId = firstItem[i] + s;
            memcpy(rec + 0, &slotIndex, sizeof slotIndex);
            memcpy(rec + 8, &itemId, sizeof itemId);
            put(rec, sizeof rec);
        }
        const uint32_t n = static_cast<uint32_t>(json[i].size());
        put(&n, sizeof n);
        put(json[i].data(), n);
    }
    return b;
}

// A v2 block, laid out from v2's OWN constants: the 32-byte header it recorded and the 92-byte
// entrant it wrote.  Neither is sizeof() of anything in this build -- both grew at v3, and a fixture
// that used today's widths would move with the defect instead of catching it.
std::vector<uint8_t> BuildV2Block(uint64_t tUs, uint64_t contentHash,
                                  const std::vector<IdentityEntrant>& people,
                                  const std::vector<std::string>& json,
                                  const std::vector<uint64_t>& firstItem) {
    std::vector<uint8_t> b;
    auto put = [&](const void* p, size_t n) {
        const uint8_t* q = static_cast<const uint8_t*>(p);
        b.insert(b.end(), q, q + n);
    };
    IdentitySnapshotHeader h{};
    h.magic = kIdentityBlockMagic;
    h.version = kIdentityBlockVersionV2;
    h.tUs = tUs;
    h.entrantCount = static_cast<uint16_t>(people.size());
    h.contentHash = contentHash;
    h.loadoutSlotStride = 24;          // v2's own record width, not this build's sizeof
    h.loadoutSlotCount = kIdentitySlotCount;
    put(&h, kIdentityHeaderSizeV2);    // v2 stopped here: it recorded no entrant stride
    for (size_t i = 0; i < people.size(); ++i) {
        put(&people[i], kIdentityEntrantSizeV2);
        for (uint16_t s = 0; s < kIdentitySlotCount; ++s) {
            uint8_t rec[24] = {};
            const uint16_t slotIndex = s;
            const uint64_t itemId = firstItem[i] + s;
            const uint64_t slotKey = 0x5EED000000000000ull + s;
            memcpy(rec + 0, &slotIndex, sizeof slotIndex);
            memcpy(rec + 8, &itemId, sizeof itemId);
            memcpy(rec + 16, &slotKey, sizeof slotKey);
            put(rec, sizeof rec);
        }
        const uint32_t n = static_cast<uint32_t>(json[i].size());
        put(&n, sizeof n);
        put(json[i].data(), n);
    }
    return b;
}

// One entrant carrying a value in every field a v1 block can hold, so "parses in full" is a claim
// about all of them rather than about whichever one the test happened to look at.
IdentityEntrant OnePerson(const char* name, const char* platform) {
    IdentityEntrant e{};
    e.xplatformId = 0xDEC0DE0000001234ull;
    e.userSlot = 3;
    e.userIndex = 7;
    e.generation = 9;
    e.alignment = 0xFFFEu;          // a word: it does not fit the byte the manifest row carries
    e.teamId = 1;
    e.teamSlot = 2;
    e.bodyType = 5;                 // the chassis
    e.loadoutNumber = 11;
    e.loadoutNumberBody = 12;
    e.loadoutHeader3C = 0xABCD1234u;
    memcpy(e.displayName, name, strlen(name));
    memcpy(e.platformStr, platform, strlen(platform));
    return e;
}

// What one entrant costs on disk, derived from the records rather than guessed: a buffer sized by
// hand starts silently truncating the fixture the moment either struct grows.
constexpr size_t kOneEntrantBytes = sizeof(IdentityEntrant) +
                                    sizeof(IdentityLoadoutSlot) * kIdentitySlotCount +
                                    sizeof(uint32_t);

}  // namespace

TEST_CASE("largest_identity_block_survives_the_real_event_codec_and_a_bigger_one_is_refused") {
    // The producer raised the widest S_EVENT body to one entrant's identity block.
    // The encoder, the decoder and the producer's own stage now read ONE constant, and a body past
    // it is refused at the encoder rather than written into a stream no decoder can walk past.
    CHECK(kMaxEventBodyBytes == kIdentityBlockOneEntrantCap);

    std::vector<uint8_t> body(kMaxEventBodyBytes);
    for (size_t i = 0; i < body.size(); ++i) body[i] = uint8_t(i * 7 + 3);

    std::vector<uint8_t> bits(size_t(kMaxEventBodyBytes) * 4);
    bitio::BitWriter w(bits.data(), bits.size());
    EventEncoder enc;
    EventMessage e{};
    e.stampUs = 1234;
    e.dst = 7;
    e.typeIndex = TapTypeIndex(kIdentitySnapshotTapName);
    e.bodyBytes = uint16_t(body.size());
    e.body = body.data();
    REQUIRE(enc.Encode(w, e));
    // A second, ordinary row after it: what a truncated decode of the first would destroy.
    const uint8_t small[] = {1, 2, 3, 4};
    EventMessage e2{};
    e2.stampUs = 2345;
    e2.dst = 7;
    e2.typeIndex = 1;
    e2.bodyBytes = uint16_t(sizeof small);
    e2.body = small;
    REQUIRE(enc.Encode(w, e2));
    REQUIRE(!w.Refused());

    bitio::BitReader r(bits.data(), w.BitsWritten());
    EventDecoder dec;
    std::vector<uint8_t> outBuf(kMaxEventBodyBytes);
    EventMessage got{};
    REQUIRE(dec.Decode(r, got, outBuf.data(), outBuf.size()));
    CHECK(got.bodyBytes == body.size());
    CHECK(memcmp(got.body, body.data(), body.size()) == 0);

    // A reader whose buffer is the old 1 KB pattern LOSES this record and KEEPS the stream: the
    // next row still decodes, which is the whole difference between a skip and a desync.
    bitio::BitReader r2(bits.data(), w.BitsWritten());
    EventDecoder dec2;
    uint8_t tiny[1024];
    EventMessage lost{};
    CHECK(!dec2.Decode(r2, lost, tiny, sizeof tiny));
    CHECK(dec2.BodiesTooSmall() == 1);
    EventMessage after{};
    REQUIRE(dec2.Decode(r2, after, tiny, sizeof tiny));
    CHECK(after.bodyBytes == sizeof small);
    CHECK(after.stampUs == 2345);

    // One byte past the bound is REFUSED, and nothing is written: the stream stays walkable.
    std::vector<uint8_t> over(size_t(kMaxEventBodyBytes) + 1);
    std::vector<uint8_t> bits2(over.size() * 4);
    bitio::BitWriter w2(bits2.data(), bits2.size());
    EventEncoder enc2;
    EventMessage big{};
    big.stampUs = 9;
    big.dst = 1;
    big.typeIndex = 2;
    big.bodyBytes = uint16_t(over.size());
    big.body = over.data();
    CHECK(!enc2.Encode(w2, big));
    CHECK(w2.BitsWritten() == 0);
}

TEST_CASE("unknown_magic_refuses_by_name") {
    const std::vector<IdentityEntrant> people{OnePerson("Shepard", "DMO-1")};
    const std::vector<std::string> json{"{\"a\":1}"};
    std::vector<uint8_t> b = BuildV1Block(1700ull, 0xCAFEull, people, json, {0x7000ull});

    IdentityBlockReader ok;
    REQUIRE(ok.Open(b.data(), b.size()));

    const uint32_t wrong = kIdentityBlockMagic ^ 0xFFu;
    memcpy(b.data(), &wrong, sizeof wrong);
    IdentityBlockReader r;
    CHECK(!r.Open(b.data(), b.size()));
    CHECK(r.Refusal() == IdentityBlockRefusal::UnknownMagic);
    CHECK(strcmp(IdentityBlockRefusalText(r.Refusal()), "IdentityBlockUnknownMagic") == 0);
    CHECK(r.Count() == 0);
}

TEST_CASE("v1_block_parses_everything_but_loadouts_refuse") {
    // The owner's line is "respect EVERYTHING correctly, from player names, profiles, loadouts".
    // A reader that refuses a v1 block outright loses the names, teams, alignment, chassis and the
    // profile JSON that recording genuinely holds, to gain nothing -- v1's layout is fully derivable
    // from a v2 reader.  So: it parses, HasSlotKeys() is false, and ONLY the resolver refuses.
    const std::vector<IdentityEntrant> people{OnePerson("Shepard", "DMO-1")};
    const std::vector<std::string> json{"{\"bodytype\":-2076784106321327439}"};
    const std::vector<uint8_t> b = BuildV1Block(1700ull, 0xCAFEull, people, json, {0x7000ull});

    IdentityBlockReader r;
    REQUIRE(r.Open(b.data(), b.size()));     // a reader that refuses the whole block here is RED
    CHECK(r.Version() == kIdentityBlockVersionV1);
    CHECK(r.SlotStride() == kIdentitySlotStrideV1);
    CHECK(r.SlotCount() == kIdentitySlotCount);
    CHECK(r.Count() == 1);
    CHECK(r.TimeUs() == 1700ull);
    CHECK(r.ContentHash() == 0xCAFEull);
    CHECK(!r.HasSlotKeys());

    IdentityBlockEntrantView v{};
    REQUIRE(r.Entrant(0, v));
    CHECK(strcmp(v.fields.displayName, "Shepard") == 0);
    CHECK(strcmp(v.fields.platformStr, "DMO-1") == 0);
    CHECK(v.fields.xplatformId == 0xDEC0DE0000001234ull);
    CHECK(v.fields.userSlot == 3);
    CHECK(v.fields.userIndex == 7);
    CHECK(v.fields.generation == 9);
    CHECK(v.fields.teamId == 1);
    CHECK(v.fields.teamSlot == 2);
    CHECK(v.fields.bodyType == 5);
    CHECK(v.fields.alignment == 0xFFFEu);
    CHECK(v.fields.loadoutNumber == 11);
    CHECK(v.fields.loadoutNumberBody == 12);
    CHECK(v.fields.loadoutHeader3C == 0xABCD1234u);
    REQUIRE(v.jsonLen == json[0].size());
    CHECK(memcmp(v.json, json[0].data(), v.jsonLen) == 0);

    // ...and the one thing v1 cannot answer says so BY NAME rather than handing back positions.
    IdentityLoadoutSlot out[kIdentitySlotCount];
    memset(out, 0xAB, sizeof out);
    uint16_t n = 12345;   // a poison value: the refusal must leave it untouched
    CHECK(!r.ResolveLoadout(0, out, n));
    CHECK(n == 12345);
    CHECK(r.Refusal() == IdentityBlockRefusal::SlotKeysNotRecorded);
    CHECK(strcmp(IdentityBlockRefusalText(r.Refusal()), "LoadoutSlotKeysNotRecorded") == 0);
    for (uint16_t i = 0; i < kIdentitySlotCount; ++i) CHECK(out[i].slotKey == 0);
}

TEST_CASE("v2_block_parses_in_full_and_its_pair_count_is_unknown_not_zero") {
    // AUD-ARCH-23-S14 F6/F7.  v3 grew IdentityEntrant by two bytes, so a v2 record is NARROWER than
    // this build's struct: every offset after it -- the slot array, the JSON length, the JSON -- must
    // come from the width the block RECORDS (v2 recorded none, so v2's constant is the derivation),
    // never from sizeof.  And the two facts v2 does carry must survive the bump: its slot keys are
    // real, and its pair count is UNKNOWN rather than the zero its absent field reads as.
    std::string doc = "{\"unlocks\":{\"emote\":\"grin\"}}";
    const std::vector<IdentityEntrant> people{OnePerson("Garrus", "DMO-7")};
    const std::vector<std::string> json{doc};
    const std::vector<uint8_t> v2 = BuildV2Block(4242ull, 0xC0FFEEull, people, json, {0x9000ull});

    IdentityBlockReader r;
    REQUIRE(r.Open(v2.data(), v2.size()));
    CHECK(r.Version() == kIdentityBlockVersionV2);
    CHECK(r.EntrantStride() == kIdentityEntrantSizeV2);
    CHECK(r.EntrantStride() != sizeof(IdentityEntrant));
    CHECK(r.SlotStride() == 24);
    CHECK(r.Count() == 1);

    // IN FULL: every field a v2 block holds, because "parses" is a claim about all of them.
    IdentityBlockEntrantView v{};
    REQUIRE(r.Entrant(0, v));
    CHECK(v.fields.xplatformId == 0xDEC0DE0000001234ull);
    CHECK(v.fields.userSlot == 3);
    CHECK(v.fields.userIndex == 7);
    CHECK(v.fields.generation == 9);
    CHECK(v.fields.alignment == 0xFFFEu);
    CHECK(v.fields.teamId == 1);
    CHECK(v.fields.teamSlot == 2);
    CHECK(v.fields.bodyType == 5);
    CHECK(v.fields.loadoutNumber == 11);
    CHECK(v.fields.loadoutNumberBody == 12);
    CHECK(v.fields.loadoutHeader3C == 0xABCD1234u);
    CHECK(strcmp(v.fields.displayName, "Garrus") == 0);
    CHECK(strcmp(v.fields.platformStr, "DMO-7") == 0);
    REQUIRE(v.jsonLen == doc.size());
    CHECK(memcmp(v.json, doc.data(), v.jsonLen) == 0);

    // F7: the key was introduced at v2, so a v2 block still answers true after v3 -- a check against
    // the MOVING current version would throw away every key recorded before this packet.
    CHECK(r.HasSlotKeys());
    IdentityLoadoutSlot got[kIdentitySlotCount] = {};
    uint16_t n = 0;
    REQUIRE(r.ResolveLoadout(0, got, n));
    CHECK(n == kIdentitySlotCount);
    for (uint16_t i = 0; i < kIdentitySlotCount; ++i) {
        CHECK(got[i].itemId == 0x9000ull + i);
        CHECK(got[i].slotKey == 0x5EED000000000000ull + i);
    }

    // F6: the count the format did not yet have is UNKNOWN, and unknown is not zero.
    CHECK(!r.HasPairCount());
    uint16_t filled = 0;
    REQUIRE(r.FilledPairCount(0, filled));
    CHECK(filled == kIdentityPairCountUnknown);
    CHECK(filled != 0);
    CHECK(v.fields.filledPairCount == kIdentityPairCountUnknown);
}

TEST_CASE("v1_block_reports_an_unknown_pair_count_too") {
    // The same rule two versions back, and the v1 record is narrower for a second reason: its slot
    // stride is 16.  Both derivations come from what the block recorded.
    const std::vector<IdentityEntrant> people{OnePerson("Tali", "DMO-8")};
    const std::vector<std::string> json{"{\"a\":1}"};
    const std::vector<uint8_t> v1 = BuildV1Block(7ull, 8ull, people, json, {0x1100ull});

    IdentityBlockReader r;
    REQUIRE(r.Open(v1.data(), v1.size()));
    CHECK(r.Version() == kIdentityBlockVersionV1);
    CHECK(r.EntrantStride() == kIdentityEntrantSizeV1);
    CHECK(!r.HasPairCount());
    CHECK(!r.HasSlotKeys());
    uint16_t filled = 0;
    REQUIRE(r.FilledPairCount(0, filled));
    CHECK(filled == kIdentityPairCountUnknown);
    CHECK(filled != 0);
    IdentityBlockEntrantView v{};
    REQUIRE(r.Entrant(0, v));
    CHECK(strcmp(v.fields.displayName, "Tali") == 0);
    REQUIRE(v.jsonLen == 7u);
    CHECK(v.json[0] == '{');
}

TEST_CASE("a_v3_block_recording_a_short_entrant_stride_is_refused_by_name") {
    // The stride is read from the block, so it is untrusted input: a v3 record narrower than the
    // fields v3 defines cannot hold them, and parsing it would build the tail of every entrant out
    // of the bytes that follow the record.
    std::vector<uint8_t> buf(sizeof(IdentitySnapshotHeader) + kOneEntrantBytes);
    IdentityBlockWriter w(buf.data(), buf.size(), 5ull, 6ull);
    IdentityBlockEntrantIn in{};
    in.fields = OnePerson("Short", "DMO-9");
    REQUIRE(w.Add(in));
    size_t written = 0;
    REQUIRE(w.Finish(written));

    IdentitySnapshotHeader h{};
    memcpy(&h, buf.data(), sizeof h);
    REQUIRE(h.entrantStride == sizeof(IdentityEntrant));
    h.entrantStride = uint16_t(kIdentityEntrantSizeV2);   // a v3 header claiming a v2 record
    memcpy(buf.data(), &h, sizeof h);

    IdentityBlockReader r;
    CHECK(!r.Open(buf.data(), written));
    CHECK(r.Refusal() == IdentityBlockRefusal::BadEntrantLayout);
    CHECK(strcmp(IdentityBlockRefusalText(r.Refusal()), "IdentityBlockBadEntrantLayout") == 0);
    CHECK(r.Count() == 0);
}

TEST_CASE("new_block_into_old_pinned_reader_refuses") {
    // The other direction, and it is not symmetric: a layout the reader cannot DERIVE is refused
    // whole.  A half-parsed block is indistinguishable from a correct one that happens to be sparse.
    std::vector<uint8_t> buf(sizeof(IdentitySnapshotHeader) + kOneEntrantBytes);
    IdentityBlockWriter w(buf.data(), buf.size(), 99ull, 0xBEEFull);
    IdentityBlockEntrantIn in{};
    in.fields = OnePerson("Wrex", "DMO-2");
    IdentityLoadoutSlot slots[kIdentitySlotCount] = {};
    for (uint16_t i = 0; i < kIdentitySlotCount; ++i) {
        slots[i].slotIndex = i;
        slots[i].itemId = 0x8000ull + i;
        slots[i].slotKey = 0xA5A5000000000000ull + (kIdentitySlotCount - i);
    }
    in.slots = slots;
    REQUIRE(w.Add(in));
    size_t written = 0;
    REQUIRE(w.Finish(written));

    IdentityBlockReader old;
    old.PinVersion(kIdentityBlockVersionV1);      // a build that predates this format
    CHECK(!old.Open(buf.data(), written));
    CHECK(old.Refusal() == IdentityBlockRefusal::UnknownVersion);
    CHECK(strcmp(IdentityBlockRefusalText(old.Refusal()), "IdentityBlockUnknownVersion") == 0);
    CHECK(old.Count() == 0);
    CHECK(old.Version() == 0);
    IdentityBlockEntrantView v{};
    CHECK(!old.Entrant(0, v));                    // nothing is parsed, not even the first entrant
}

TEST_CASE("json_offset_comes_from_the_recorded_stride") {
    // The dangerous half.  A reader deriving the trailing offset from sizeof(IdentityLoadoutSlot)
    // lands 8 * 20 = 160 bytes inside a v1 block's profile JSON: a silent misparse of the PROFILE,
    // not a missing loadout.  Both versions must reach a leading '{'.
    std::string big = "{\"unlocks\":{\"x\":\"";
    big.append(400, 'z');
    big += "\"}}";
    const std::vector<IdentityEntrant> people{OnePerson("Liara", "DMO-3")};
    const std::vector<std::string> json{big};
    const std::vector<uint8_t> v1 = BuildV1Block(1ull, 2ull, people, json, {0x7000ull});

    IdentityBlockReader r1;
    REQUIRE(r1.Open(v1.data(), v1.size()));
    IdentityBlockEntrantView a{};
    REQUIRE(r1.Entrant(0, a));
    REQUIRE(a.jsonLen == big.size());
    CHECK(a.json[0] == '{');
    CHECK(memcmp(a.json, big.data(), a.jsonLen) == 0);

    std::vector<uint8_t> buf(sizeof(IdentitySnapshotHeader) + kOneEntrantBytes + big.size());
    IdentityBlockWriter w(buf.data(), buf.size(), 1ull, 2ull);
    IdentityBlockEntrantIn in{};
    in.fields = people[0];
    in.profileJson = Blob{reinterpret_cast<const uint8_t*>(big.data()),
                          static_cast<uint32_t>(big.size())};
    REQUIRE(w.Add(in));
    size_t written = 0;
    REQUIRE(w.Finish(written));

    IdentityBlockReader r2;
    REQUIRE(r2.Open(buf.data(), written));
    CHECK(r2.SlotStride() == sizeof(IdentityLoadoutSlot));
    IdentityBlockEntrantView c{};
    REQUIRE(r2.Entrant(0, c));
    REQUIRE(c.jsonLen == big.size());
    CHECK(c.json[0] == '{');
    CHECK(memcmp(c.json, big.data(), c.jsonLen) == 0);
}

TEST_CASE("roundtrip_preserves_20_pairs") {
    // key != position, and not in the array's order either: a positional writer cannot look right.
    IdentityLoadoutSlot slots[kIdentitySlotCount] = {};
    for (uint16_t i = 0; i < kIdentitySlotCount; ++i) {
        slots[i].slotIndex = i;
        slots[i].itemId = 0x8000ull + i;
        slots[i].slotKey = 0x9E3779B97F4A7C15ull ^ (0x100ull * (kIdentitySlotCount - i));
    }
    const std::string json = "{\"a\":1}";

    std::vector<uint8_t> buf(sizeof(IdentitySnapshotHeader) + 2 * (kOneEntrantBytes + json.size()));
    IdentityBlockWriter w(buf.data(), buf.size(), 42ull, 0x1234ull);
    IdentityBlockEntrantIn in{};
    in.fields = OnePerson("Garrus", "DMO-4");
    in.slots = slots;
    in.profileJson = Blob{reinterpret_cast<const uint8_t*>(json.data()),
                          static_cast<uint32_t>(json.size())};
    REQUIRE(w.Add(in));
    REQUIRE(w.Add(in));
    size_t written = 0;
    REQUIRE(w.Finish(written));

    IdentityBlockReader r;
    REQUIRE(r.Open(buf.data(), written));
    CHECK(r.Count() == 2);
    CHECK(r.Version() == kIdentityBlockVersion);
    CHECK(r.HasSlotKeys());
    for (uint16_t e = 0; e < 2; ++e) {
        IdentityLoadoutSlot out[kIdentitySlotCount];
        uint16_t n = 0;
        REQUIRE(r.ResolveLoadout(e, out, n));
        REQUIRE(n == kIdentitySlotCount);
        for (uint16_t i = 0; i < kIdentitySlotCount; ++i) {
            CHECK(out[i].slotKey == slots[i].slotKey);
            CHECK(out[i].itemId == slots[i].itemId);
            CHECK(out[i].slotIndex == i);
            CHECK(out[i].slotKey != i);                 // never a position wearing a key's name
            CHECK(out[i].slotKey != out[i].itemId);
        }
        IdentityBlockEntrantView v{};
        REQUIRE(r.Entrant(e, v));
        REQUIRE(v.jsonLen == json.size());
        CHECK(memcmp(v.json, json.data(), v.jsonLen) == 0);
    }
    IdentityBlockEntrantView v{};
    CHECK(!r.Entrant(2, v));
    CHECK(r.Refusal() == IdentityBlockRefusal::NoSuchEntrant);
}

TEST_CASE("twenty_of_twenty_compares_recorded_against_engine") {
    // The acceptance's two sides are the RECORDING's own bytes and the post-apply read back out of
    // the engine.  Comparing the authored buffer against itself is green when nothing reached the
    // engine at all, which is the failure this case exists to catch.
    IdentityLoadoutSlot recorded[kIdentitySlotCount] = {};
    for (uint16_t i = 0; i < kIdentitySlotCount; ++i) {
        recorded[i].slotIndex = i;
        recorded[i].itemId = 0x8000ull + i;
        recorded[i].slotKey = 0x5100000000000000ull + (kIdentitySlotCount - i);
    }

    // The engine holds the same pairs in a DIFFERENT container order: matching by key must not care.
    replay::identity_access::LoadoutPair engine[kIdentitySlotCount] = {};
    for (uint16_t i = 0; i < kIdentitySlotCount; ++i) {
        const uint16_t src = static_cast<uint16_t>(kIdentitySlotCount - 1 - i);
        engine[i].slotKey = recorded[src].slotKey;
        engine[i].itemId = recorded[src].itemId;
    }
    LoadoutMatch m = CompareLoadoutByKey(recorded, kIdentitySlotCount, engine, kIdentitySlotCount);
    CHECK(m.compared == kIdentitySlotCount);
    CHECK(m.matched == kIdentitySlotCount);
    CHECK(m.missing == 0);
    CHECK(m.mismatched == 0);

    // The positional dress: the right items, each in the wrong slot.  20/20 by POSITION, and by key
    // it is 20 mismatches -- exactly the silent, cosmetic-only failure the pair format exists for.
    replay::identity_access::LoadoutPair positional[kIdentitySlotCount] = {};
    for (uint16_t i = 0; i < kIdentitySlotCount; ++i) {
        positional[i].slotKey = recorded[i].slotKey;
        positional[i].itemId = recorded[kIdentitySlotCount - 1 - i].itemId;
    }
    m = CompareLoadoutByKey(recorded, kIdentitySlotCount, positional, kIdentitySlotCount);
    CHECK(m.compared == kIdentitySlotCount);
    CHECK(m.matched == 0);
    CHECK(m.mismatched == kIdentitySlotCount);
    CHECK(m.missing == 0);

    // An engine that holds keys we never recorded is MISSING, which is a different repair again.
    replay::identity_access::LoadoutPair other[kIdentitySlotCount] = {};
    for (uint16_t i = 0; i < kIdentitySlotCount; ++i) {
        other[i].slotKey = recorded[i].slotKey + 0x1000ull;   // clear of the recorded range
        other[i].itemId = recorded[i].itemId;
    }
    m = CompareLoadoutByKey(recorded, kIdentitySlotCount, other, kIdentitySlotCount);
    CHECK(m.compared == kIdentitySlotCount);
    CHECK(m.matched == 0);
    CHECK(m.mismatched == 0);
    CHECK(m.missing == kIdentitySlotCount);

    // A recorded slot with no key is not compared at all: it is absence, never a zero-keyed match.
    IdentityLoadoutSlot sparse[kIdentitySlotCount] = {};
    memcpy(sparse, recorded, sizeof sparse);
    sparse[4].slotKey = 0;
    m = CompareLoadoutByKey(sparse, kIdentitySlotCount, engine, kIdentitySlotCount);
    CHECK(m.compared == kIdentitySlotCount - 1);
    CHECK(m.matched == kIdentitySlotCount - 1);
}
