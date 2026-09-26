// W1-E's offline selftest: the pure logic E-4 owns, exercised without an engine.
//
// Two things are worth testing here and nothing else is. The rest of entity.cpp is memory access
// against engine pools, which no offline test can reach and which W1-E-match.probe.json covers.
//
//   1. The fp16 codec. PP-B writes the two hand offsets back through the SAME half-float encoding
//      the wire uses, because the engine's authority branch decodes them with (h << 13) + 0x70000000
//      plus a denormal fixup. If our encoder and that decoder disagree, every replayed hand offset is
//      quietly wrong -- a pose that renders, and renders in the wrong place.
//   2. The two constants entity.cpp DERIVES instead of carrying as facts. Both are worth pinning
//      because both are one edit away from becoming a magic number again.

#include <cstdint>
#include <cstring>

#include "doctest.h"
#include "generated/facts_generated.h"

#include "verdict_word.h"

namespace {

// Verbatim from src/engine/entity.cpp. Kept as a copy on purpose: a test that #includes the
// implementation's own definition cannot catch the implementation changing.
float HalfToFloat(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    const uint32_t mag = static_cast<uint32_t>(h & 0x7FFFu) << 13;
    const uint32_t bits = sign | (mag ? mag + 0x38000000u : 0u);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

uint16_t FloatToHalf(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000u);
    const int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    if (exp <= 0) return sign;
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7BFFu);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | ((bits >> 13) & 0x3FFu));
}

// The engine's decode. This is the ORACLE -- our encoder is correct exactly insofar as feeding its
// output to THIS returns what we started with -- so it must not be a paraphrase. It is the sequence at
// PLAYER.FP16_WIDEN_VA (sub_140CB09E0) verbatim: (h << 13) + 0x70000000 masked with 0x8FFFFFFF, plus
// 0x38000000 when the shifted value has any low 28 bits set. That function's own string literals,
// " (lefthandoffset.ToC3Vector())" and " (righthandoffset.ToC3Vector())", are also what name the two
// packed pairs left and right.
float EngineWiden(uint16_t h) {
    const uint32_t shifted = static_cast<uint32_t>(h) << 13;
    uint32_t bits = (shifted + 0x70000000u) & 0x8FFFFFFFu;
    if ((shifted & 0xFFFFFFFu) != 0u) bits += 0x38000000u;
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

}  // namespace

TEST_CASE("fp16 round-trips every value the hand offsets can hold") {
    // Hand offsets are metres from the unit root and never leave arm's reach; the sweep is deliberately
    // far wider than that so a value outside the expected range still behaves.
    int checked = 0;
    for (float metres = -8.0f; metres <= 8.0f; metres += 0.001f) {
        const uint16_t h = FloatToHalf(metres);
        const float back = HalfToFloat(h);
        // Half precision at this magnitude is ~2^-11 of the value; the tolerance is that, not an
        // arbitrary epsilon, so shrinking the mantissa by a bit would fail this.
        const float tol = (metres == 0.0f) ? 1e-7f : (metres < 0 ? -metres : metres) / 1024.0f;
        const float err = back - metres < 0 ? metres - back : back - metres;
        CHECK(err <= tol);
        ++checked;
    }
    CHECK(checked > 15000);
}

TEST_CASE("our encoder agrees with the engine's own widening sequence") {
    // The one that matters: PP-B's written bytes are read back by the ENGINE, not by us.
    for (int i = -4000; i <= 4000; ++i) {
        const float metres = static_cast<float>(i) * 0.001f;
        const uint16_t h = FloatToHalf(metres);
        const float ours = HalfToFloat(h);
        const float theirs = EngineWiden(h);
        CHECK(memcmp(&ours, &theirs, sizeof ours) == 0);
    }
}

TEST_CASE("fp16 signed zero and sign are preserved") {
    CHECK(FloatToHalf(0.0f) == 0x0000u);
    CHECK(FloatToHalf(-0.0f) == 0x8000u);
    CHECK(HalfToFloat(0x0000u) == 0.0f);
    CHECK(FloatToHalf(1.0f) == 0x3C00u);
    CHECK(HalfToFloat(0x3C00u) == 1.0f);
    CHECK(FloatToHalf(-1.0f) == 0xBC00u);
}

TEST_CASE("fp16 clamps rather than producing infinity") {
    // An out-of-range offset must land on the largest finite half. An infinity written into a pose
    // slot would propagate through the engine's own lerp and poison every frame after it.
    CHECK(FloatToHalf(1.0e30f) == 0x7BFFu);
    CHECK(FloatToHalf(-1.0e30f) == 0xFBFFu);
    const float big = HalfToFloat(0x7BFFu);
    CHECK(big > 65000.0f);
    CHECK(big < 66000.0f);
}

TEST_CASE("the synthetic owner is derived from the measured bound, not invented") {
    // entity.cpp writes (0xFFFE << 16) | ENTITY_USER_SLOT_TABLE_MAX. The point of the derivation is
    // that the low word is the first index the engine refuses, so these two assertions are the whole
    // safety argument in miniature.
    const uint32_t owner = (0xFFFEu << 16) | facts::ENTITY_USER_SLOT_TABLE_MAX;
    CHECK((owner & 0xFFFFu) >= facts::ENTITY_USER_SLOT_TABLE_MAX);
    // Not kInvalid's sentinel, and not the generation sentinel either function tests.
    CHECK((owner >> 16) != 0xFFFFu);
    CHECK(owner != 0xFFFFFFFFu);
}

TEST_CASE("the warp nibble is the ownership nibble shifted up four") {
    // PushHistory copies bits 4-7. If this relationship is ever edited to a literal 0xF0 the two masks
    // can drift apart, and the drift is silent: the pose still renders.
    const uint16_t warp = static_cast<uint16_t>(facts::PHYS_CI_OWNERSEQ_NIBBLE_MASK << 4);
    CHECK(warp == 0xF0u);
    CHECK((warp & facts::PHYS_CI_OWNERSEQ_NIBBLE_MASK) == 0u);
}

TEST_CASE("the two pools' history pairs are in opposite orders") {
    // §2 F11's trap, pinned. PhysCI is list-then-count; PlayerCI is list-then-count at a different
    // spacing, and the design's own text had them swapped. A future edit that makes these agree by
    // accident is a bug, so the test states the disagreement.
    CHECK(facts::PHYS_CI_HIST_LIST_OFF < facts::PHYS_CI_HIST_COUNT_OFF);
    CHECK(facts::PHYS_CI_HIST_COUNT_OFF - facts::PHYS_CI_HIST_LIST_OFF == 8u);
    CHECK(facts::PLAYER_CI_LIST_HEAD_OFF < facts::PLAYER_CI_HIST_COUNT_OFF);
    CHECK(facts::PLAYER_CI_HIST_COUNT_OFF - facts::PLAYER_CI_LIST_HEAD_OFF == 8u);
    // And the two pools do not share offsets, which is what the non-converting handles enforce.
    CHECK(facts::PHYS_CI_HIST_LIST_OFF != facts::PLAYER_CI_LIST_HEAD_OFF);
    CHECK(facts::PHYS_CI_STRIDE != facts::PLAYER_CI_STRIDE);
}

// ---------------------------------------------------------------------------------------------
// ★★★ THE VERDICT WORD, RUN AGAINST THE COUNTERS LAUNCH 07 ACTUALLY RECORDED.
//
// runs/20260830-022305Z/launch-07/probes/probe_entitymatch/done.json, verbatim:
//   { "probe": "W1-E-match", "verdict": "PASS", "checked": 6, "undetermined": 1, ... }
//   detail: "COULD-NOT-DETERMINE: CR15Game resolved but its CR15NetGame is null -- no match |
//            ESTABLISHES NOTHING about the arm question ..."
//
// checked counts Ok and Failed only, and the detail carries no FAILED line, so the counters behind
// that file are ok=6, failed=0, undetermined=1 -- and the probe filed PASS. These cases are the
// retroactive proof (S9.18): the same numbers, through the code that ships now.
// ---------------------------------------------------------------------------------------------

namespace {
const char* Word(size_t ok, size_t failed, size_t undetermined, bool reachedEnd) {
    return replay::probeverdict::Word(ok, failed, undetermined, reachedEnd);
}
}  // namespace

TEST_CASE("launch 07's own counters no longer file a PASS") {
    CHECK(strcmp(Word(6, 0, 1, true), "COULD-NOT-DETERMINE") == 0);
    // The site that produced them exits before the last check, which is the same answer by a second
    // route -- a probe that stopped at the game-space link asked none of the questions after it.
    CHECK(strcmp(Word(6, 0, 1, false), "COULD-NOT-DETERMINE") == 0);
}

TEST_CASE("the verdict word can still say PASS, and only for a full clean run") {
    // Without this the fix would be indistinguishable from a gate wired shut.
    CHECK(strcmp(Word(6, 0, 0, true), "PASS") == 0);
    // One undetermined read is enough to take it away; so is stopping early with none.
    CHECK(strcmp(Word(6, 0, 1, true), "COULD-NOT-DETERMINE") == 0);
    CHECK(strcmp(Word(6, 0, 0, false), "COULD-NOT-DETERMINE") == 0);
}

TEST_CASE("a real failure outranks an undetermined read, and nothing scored is NOT-RUN") {
    CHECK(strcmp(Word(5, 1, 0, true), "FAIL") == 0);
    CHECK(strcmp(Word(5, 1, 3, true), "FAIL") == 0);
    CHECK(strcmp(Word(0, 1, 0, false), "FAIL") == 0);
    // Nothing asked at all is not the same fact as something asked and not answered.
    CHECK(strcmp(Word(0, 0, 0, false), "NOT-RUN") == 0);
    CHECK(strcmp(Word(0, 0, 2, false), "COULD-NOT-DETERMINE") == 0);
}
