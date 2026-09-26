#pragma once

// Normalise and Infer (DESIGN-FORMATS §6.4, §7.4; packet P3). Both are pure: no I/O, no engine.
// Everything here is at source rate on the repaired source time axis; Emit resamples it (§7.4).

#include <stdint.h>
#include <vector>

#include "import/import_facts.h"
#include "import/report.h"
#include "import/result.h"
#include "import/sourcemodel.h"

namespace replay::import {

// In place: repairs time, drops isolated lead-in/tail frames, marks gaps, keeps the chosen session,
// gates the map (sets mapSymbol), rebuilds the roster in first-appearance order so that
// SrcPlayer::rosterIndex is the output file slot, fills firstSeenUs/lastSeenUs, and refuses on the
// §10 thresholds (TooShort, Roster, Basis, UnsupportedMap, Corrupt).
Result Normalise(SourceMatch& match, const ConvertOptions& options, ConvertReport& report);

// Per SrcPlayer sample, parallel to SourceMatch::players.
struct SampleInference {
    uint32_t bits28;                   // I-1, I-2, I-5, and the enabled kinematic rules
    uint16_t holdLSlot, holdRSlot;     // I-3: file slot of the held player, kNoSlot when none
    float trigL, trigR;                // I-4: 0 or 1
};

enum StunFlag : uint8_t {
    kStunPuncherGuessed = 1 << 0,      // no stats or proximity candidate; nearest opponent taken
    kStunDurationTruncated = 1 << 1,   // span ran off the end of the file
};

struct StunRow {
    int64_t tUs;
    uint16_t victimSlot, puncherSlot;
    float durationS;                   // (0, 30]
    uint8_t flags;                     // StunFlag
};

enum GoalFlag : uint8_t {
    kGoalFromEdgeOnly = 1 << 0,        // points rose with no matching last_score/GoalScored
    kGoalScorerUnresolved = 1 << 1,
};

struct GoalRow {
    int64_t tUs;
    uint8_t team;                      // the team whose points rose (own goals credit it too)
    uint8_t points;
    char goalTypeText[32];             // source spelling; Emit maps it through EmitFacts::goalTypes (O-1)
    uint16_t scorerSlot, assistSlot;   // kNoSlot when none
    float speedMps, distanceM;
    uint8_t flags;                     // GoalFlag
};

struct StatRow {
    int64_t tUs;
    uint16_t slot;
    uint16_t stats[kStatCount];
    float possessionTime;
};

struct TeamStatRow {
    int64_t tUs;
    uint8_t team;
    uint16_t stats[kStatCount];
    float possessionTime;
};

// Row lists are sorted by tUs, emitted only on change.
struct InferredTracks {
    std::vector<SampleInference> samples;  // parallel to SourceMatch::players
    std::vector<uint16_t> discOwner;       // parallel to SourceMatch::frames; file slot or kNoSlot (H5)
    std::vector<StunRow> stuns;
    std::vector<GoalRow> goals;
    std::vector<StatRow> stats;
    std::vector<TeamStatRow> teamStats;
};

// Requires a normalised match. Rules with a false RuleGate::enabled contribute nothing.
Result Infer(const SourceMatch& match, const InferFacts& facts, InferredTracks& out, ConvertReport& report);

}  // namespace replay::import
