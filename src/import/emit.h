#pragma once

// Resample and write (DESIGN-FORMATS §7.4, §7.6; packet P4). Output goes through the shipping
// ManifestBuilder / ChapterBuilder / DemoWriter, so a converted file meets the same reader, CRC and
// `echodemo verify` checks as a native one (R8).
//
// Provenance: Emit is the one writer of the header's IMPORTED bit, via Manifest::flags before
// DemoWriter::Open. Playback never branches on it beyond the existing UI label (§7.7).

#include <stdint.h>
#include <vector>

#include "import/import_facts.h"
#include "import/inferred.h"
#include "import/report.h"
#include "import/result.h"
#include "import/sourcemodel.h"

namespace replay::import {

// One output tick. Continuous fields interpolate between frames[older] and frames[newer] at alpha
// (Hermite on position with source velocities, slerp on quaternions); discrete fields step to older.
struct TickFrame {
    int64_t tUs;         // t0 + k * (1e6 / rateHz), source time axis
    uint32_t older, newer;
    float alpha;         // [0, 1]
    bool gapBefore;      // a > 250 ms source gap precedes this tick: frameIndex skips (native GAP)
};

Result BuildTickGrid(const SourceMatch& match, uint32_t rateHz, std::vector<TickFrame>& out);

// Writes `outPath`. On any refusal the partial output is deleted and the writer's text is carried.
Result Emit(const SourceMatch& match, const InferredTracks& tracks, const EmitFacts& facts,
            const ConvertOptions& options, const wchar_t* outPath, ConvertReport& report, Progress* progress);

}  // namespace replay::import
