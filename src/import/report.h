#pragma once

// Conversion options and the provenance report (DESIGN-FORMATS §7.7, R6). The in-file mark is the
// header's IMPORTED bit (set by Emit); the per-rule detail lives only in the `<out>.import.json` sidecar.

#include <stdint.h>
#include <vector>

#include "import/import_facts.h"
#include "import/result.h"
#include "import/sourcemodel.h"

namespace replay::import {

struct ConvertOptions {
    int32_t session = -1;              // index into the report's session list; -1 = the longest
    uint32_t rateHz = 45;              // output tick rate; 45 is the native design cadence (D3)
    const wchar_t* reportPath = nullptr;  // null = `<out>.import.json`; not owned
    // Ceiling on the source model the reader builds, which every later stage scales with; 0 is
    // unlimited. The viewer sets one because it converts inside the game's process.
    uint64_t maxModelBytes = 0;
};

struct SourceGap {
    int64_t atUs;  // from the first kept frame
    int64_t lengthUs;
};

struct ConvertReport {
    SourceKind kind;
    uint64_t sourceHash;           // xxh64 of the source bytes
    SourceStats source;

    uint32_t sessionsFound;        // Normalise
    uint32_t sessionChosen;
    uint32_t framesKept;
    uint32_t repairedStamps;       // t' = max(t, prev + 1 ms)
    bool retimedWhole;             // > 1% repaired: re-timed on the nominal rate
    uint32_t leadInDropped, tailDropped;
    uint32_t gaps;                 // > 250 ms mid-file, emitted as tick GAPs
    bool requiresAllowGaps;
    std::vector<SourceGap> gapList;  // the holes Emit writes as GAP records

    uint32_t inferredRows[kRuleCount];  // Infer, by rule
    uint32_t stunPuncherGuessed;
    uint32_t unknownGoalTypes;

    uint32_t ticks;                // Emit
    uint32_t playerRows, bodyRows, eventRows, netvarRows;
    uint32_t handOffsetClamped;    // |hand - head| > 2 m
    uint32_t comRejected;          // outside the +-250 m wire range
    int64_t durationUs;
};

// Writes the sidecar: source identity, converter version, the facts tables used, counts and repairs.
Result WriteReportJson(const ConvertReport& report, const InferFacts& infer, const EmitFacts& emit,
                       const wchar_t* sourcePath, const wchar_t* path);

}  // namespace replay::import
