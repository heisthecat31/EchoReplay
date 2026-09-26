// W2-E. The spectator-CS row's outcome mapping, in a header so the probe and test_camera drive the
// SAME function -- a probe that scores itself with a private rule is the defect AUDIT_WIRING_C H2
// found here, and a mapping only the probe can reach is one nothing can mutate.
//
// The probe does not decide WHY the resolve refused: camera::detail::ResolveSpectatorCs does, and
// this maps its five refusals onto the four outcomes a probe row can carry.
//
//     asked-pass     -> "ok"
//     asked-fail     -> "failed"          the question was put to the engine and it answered no
//     could-not-ask  -> "undetermined"    the engine was never in a state to be asked
//     never-asked    -> "undetermined"    no match was reached, so nothing was read at all
//
// ★ could-not-ask and never-asked share the WORD and are separated by the detail (and by
// doneJson's csRefusal), because "ok"/"failed"/"undetermined" are the only three words the verdict
// loop in probe_teamgate.cpp recognises. A fourth word would match neither strcmp there, count as
// neither a failure nor an undetermined, and turn a run that could not ask its central question
// into a PASS. The distinction belongs in the reader's existing channel.
//
// The harness has no row vocabulary at all: it never reads these words. It reads the record's
// counts (`checked`, `undetermined`) and the top-level verdict word, which must be one of
// PASS/FAIL/NOT-RUN/REFUSED -- see WriteVerdict in probe_teamgate.cpp.
#ifndef REPLAY_TESTS_CAMERA_PROBE_TEAMGATE_OUTCOME_H
#define REPLAY_TESTS_CAMERA_PROBE_TEAMGATE_OUTCOME_H

#include <cstdint>

#include "engine/camera_writepoint.h"

namespace replay {
namespace probe_teamgate {

// Whether CS.LOOKUP_BY_SYMBOL itself could be trusted before ResolveSpectatorCs was handed it.
// Unreadable and Mismatch are BOTH could-not-ask for this row: on a build whose bytes are not the
// registry's, the spectator CS was never asked anything. The mismatch is a defect, and it is
// reported as one -- by the sites-verify row, once, where it belongs.
enum class CsSite : uint8_t {
    Unreadable,  // the site is not mapped in this image
    Mismatch,    // mapped, but holding bytes other than the ones facts.tsv recorded
    Verified,
};

inline const char* CsSiteName(CsSite s) {
    switch (s) {
        case CsSite::Unreadable: return "not-mapped";
        case CsSite::Mismatch:   return "prologue-mismatch";
        case CsSite::Verified:   return "verified";
    }
    return "unknown";
}

struct Outcome {
    const char* word;    // "ok" | "failed" | "undetermined"
    const char* detail;
};

// The record's VERDICT word, here rather than inside the probe so test_camera drives the same rule
// the DLL files with. Only PASS/FAIL/NOT-RUN/REFUSED are read: Read-ProbeVerdict rejects anything
// else and Get-VerdictFinality calls the record `nonverdict`, which is how a record saying
// COULD-NOT-DETERMINE filed no verdict at all and never reached the sweep tally.
//
// FAIL beats NOT-RUN: something that could be evaluated was wrong, and that is the more urgent
// fact. Every row is asked-and-answered or could-not-ask, so a run that asked NOTHING is a run
// whose rows are all undetermined -- the asked==0 case is this second clause, not a third one.
inline const char* VerdictWord(uint32_t failed, uint32_t undetermined) {
    if (failed != 0) return "FAIL";
    if (undetermined != 0) return "NOT-RUN";
    return "PASS";
}

inline Outcome SpectatorCsOutcome(bool reachedMatch, CsSite site, camera::Refusal refusal) {
    if (!reachedMatch) {
        return {"undetermined", "never asked: no netGame at state 9, so nothing was read"};
    }
    if (site == CsSite::Unreadable) {
        return {"undetermined",
                "could not ask: CS.LOOKUP_BY_SYMBOL is not mapped in this image"};
    }
    if (site == CsSite::Mismatch) {
        return {"undetermined",
                "could not ask: CS.LOOKUP_BY_SYMBOL holds bytes other than the registry's -- "
                "the defect is reported by sites-verify, not accused again here"};
    }

    switch (refusal) {
        case camera::Refusal::None:
            return {"ok",
                    "the spectator component system has a local instance (W1-H S9 item 2)"};

        // Could-not-ask: the engine has not built the thing the question is about.
        case camera::Refusal::NoNetGame:
            return {"undetermined",
                    "could not ask: there is no CR15NetGame pointer to read a slot off, so no "
                    "slot was read"};
        // Split from NoNetGame by AUD-F3B F3B-2 and kept split here: "there is no netGame yet" is
        // a claim about the engine's progress, this one is a claim about our own read.
        case camera::Refusal::NetGameUnreadable:
            return {"undetermined",
                    "could not ask: a CR15NetGame was held and its level-space slot is not "
                    "committed memory, so the slot was never read"};
        case camera::Refusal::LevelSpaceUnreadable:
            return {"undetermined",
                    "could not ask: the level-space pointer is non-null and its component-system "
                    "table could not be reached at all, so the table was never asked"};
        case camera::Refusal::NoLevelSpace:
            return {"undetermined",
                    "could not ask: CR15NetGame+0x2B10 is null -- no map is loaded, so the level "
                    "space the lookup runs on does not exist yet"};
        case camera::Refusal::SpaceTableNotReady:
            return {"undetermined",
                    "could not ask: the space is allocated but its component-system table is not "
                    "safely indexable, so the lookup was never called"};

        // Asked-fail: the lookup ran and its answer is wrong or absent.
        case camera::Refusal::LookupNull:
            return {"failed",
                    "asked: the lookup returned no R14NetSpectatorCamera component system for "
                    "the level space"};
        // The two-space differential (ARCH-12 R2). Both are asked-fail, not undetermined: the
        // question was put and the answer was wrong, which is a different thing from not asking.
        case camera::Refusal::SpaceAmbiguous:
            return {"failed",
                    "asked: CAM.SPECTATOR_SYMBOL resolves in the +0x2B08 space as well as in the "
                    "level space, so which object would be bound is not decided"};
        case camera::Refusal::SpaceAliased:
            return {"failed",
                    "asked: CR15NetGame+0x2B08 and +0x2B10 hold the SAME pointer -- the two "
                    "spaces cannot be told apart, so the field being read is not the field meant"};
        case camera::Refusal::NoLocalInstance:
            return {"failed",
                    "asked: the component system has no instance at its local index"};

        default:
            // A refusal this mapping has not been taught. Never scored, never accused.
            return {"undetermined",
                    "could not ask: the resolve refused with a reason this probe does not map -- "
                    "see csRefusal in doneJson"};
    }
}

}  // namespace probe_teamgate
}  // namespace replay

#endif  // REPLAY_TESTS_CAMERA_PROBE_TEAMGATE_OUTCOME_H
