#pragma once

// Source-time thresholds shared by Normalise, Infer and the tick grid (DESIGN-FORMATS §7.4).

#include <stdint.h>

namespace replay::import {

constexpr int64_t kGapUs = 250000;           // a longer source step is a GAP, not a bracket to interpolate
constexpr int64_t kIsolatedEndUs = 5000000;  // an end frame this far from its neighbour is dropped
constexpr int64_t kMinDurationUs = 2000000;  // shorter after trimming refuses TooShort
constexpr int64_t kRepairStepUs = 1000;      // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (1 ms in microseconds), not the CR15Game vtable slot

}  // namespace replay::import
