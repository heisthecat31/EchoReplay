// CAMERA-STRIDE-GUARD. SPECTATOR.CI_STRIDE is derived from the two factors the engine multiplies,
// not transcribed -- camera_writepoint_facts.h decodes them out of SPECTATOR.CI_STRIDE_MUL_SITE and
// SPECTATOR.CI_STRIDE_SHL. This file is the OUTSIDE record that pins the product, because a value
// derived from the registry cannot also be its own expectation: the 640 below is stated here, from
// facts.tsv:756's evidence, and the derivation has to arrive at it.
//
// The runtime case is the one the header cannot make: a wrong stride is not a wrong number, it is a
// read landing on the wrong instance. The buffer is laid out at a literal 640 with two DECOYS
// planted at the spacings a misread SIB scale would produce (384 and 1152, from scale bits 01 and 11
// against the file's 10), so an off constant does not merely mismatch -- it returns a decoy's
// marker, which is exactly the failure mode in the field.

#include <doctest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "engine/camera_writepoint_facts.h"
#include "generated/facts_generated.h"

namespace {

// The hatch is the point of the file, not an escape from the ratchet: the row is withheld, so there
// is no constant to name, and naming one derived from the same two factors would make the
// expectation restate the derivation it is here to check.
constexpr uint32_t kEngineStride = 640u;  // facts-ok: SPECTATOR.CI_STRIDE PC_PRESENT, so gen_facts emits no value

// The two decoys are not arbitrary: they are the strides a misread of the SIB scale field produces.
// Scale bits 01 give a factor of 3 and bits 11 give 9, against the file's 10 (factor 5), so each
// decoy is one bit away from the real derivation rather than merely a different number.
constexpr uint32_t kDecoyStrideFactor3 = 3u << 7;
constexpr uint32_t kDecoyStrideFactor9 = 9u << 7;

// The marker written into every instance the fixture plants, real or decoy. It is never zero and
// never repeats across a (stride, index) pair, so "read the base" and "read a decoy" are both
// distinguishable from a correct read.
uint32_t MarkerFor(uint32_t stride, uint32_t index) { return (stride + 1u) * (index + 1u); }

}  // namespace

TEST_CASE("CAMERA-STRIDE-GUARD: the derived stride is the engine's 640, and its two factors") {
    using namespace replay::camera_facts;
    CHECK(kCiStride == kEngineStride);
    CHECK(kCiStrideLeaFactor * (1u << facts::SPECTATOR_CI_STRIDE_SHL) == kEngineStride);
}

TEST_CASE("CAMERA-STRIDE-GUARD: the derived stride is the element spacing, and decoys prove it") {
    using namespace replay::camera_facts;

    // Four instances at the engine's spacing, then the two decoy ladders written on top of the
    // same buffer. Decoy markers land where a wrong stride would read and nowhere else.
    const uint32_t kInstances = 4;
    std::vector<uint8_t> pool(static_cast<size_t>(kEngineStride) * kInstances +
                              kDecoyStrideFactor9 * kInstances + kEngineStride, 0);
    uint8_t* base = pool.data();

    for (uint32_t i = 0; i < kInstances; ++i) {
        const uint32_t m = MarkerFor(kEngineStride, i);
        std::memcpy(base + static_cast<size_t>(kEngineStride) * i + kCiModeOff, &m, sizeof m);
    }
    for (uint32_t decoy : {kDecoyStrideFactor3, kDecoyStrideFactor9}) {
        for (uint32_t i = 1; i < kInstances; ++i) {   // i == 0 is the shared instance 0
            const size_t at = static_cast<size_t>(decoy) * i + kCiModeOff;
            const uint32_t m = MarkerFor(decoy, i);
            uint32_t already = 0;
            std::memcpy(&already, base + at, sizeof already);
            if (already == 0) std::memcpy(base + at, &m, sizeof m);
        }
    }

    // The read under test is the engine's own: base + index * SPECTATOR.CI_STRIDE.
    for (uint32_t i = 0; i < kInstances; ++i) {
        uint32_t got = 0;
        std::memcpy(&got, base + static_cast<size_t>(kCiStride) * i + kCiModeOff, sizeof got);
        CHECK(got == MarkerFor(kEngineStride, i));
        CHECK(got != MarkerFor(kDecoyStrideFactor3, i));
        CHECK(got != MarkerFor(kDecoyStrideFactor9, i));
    }

    // The decoys are really there. Without this the loop above would pass on a buffer of zeroes,
    // which is the state a wrong stride reaches by walking off the pool rather than into a
    // neighbour: every decoy slot holds a planted marker, and never the one the right stride
    // returns for that index. (No decoy slot can coincide with a real one here: 384i == 640j and
    // 1152i == 640j both need i == 5.)
    for (uint32_t decoy : {kDecoyStrideFactor3, kDecoyStrideFactor9}) {
        for (uint32_t i = 1; i < kInstances; ++i) {
            uint32_t got = 0;
            std::memcpy(&got, base + static_cast<size_t>(decoy) * i + kCiModeOff, sizeof got);
            CHECK(got != 0u);
            CHECK(got != MarkerFor(kEngineStride, i));
        }
    }
}
