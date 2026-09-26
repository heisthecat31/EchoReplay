// W2-H's check ledger, and the ONE statement of what probe_audio's artifact is.
//
// Three consumers read this header and none of them keeps a copy of its rules: probe_audio.cpp fills
// the ledger in vivo and writes its record through FormatRecord; audio_selftest replays recorded runs
// through the same functions; and audio_ledgerdump prints the table below so tests/audio/check.ps1
// can hold real artifacts against it. A second statement of these rules anywhere else would certify
// itself -- the check's earlier revision parsed this file twice with two regexes and called that two
// sources, and an audit shrank the enumeration by one and watched every arm stay green.
//
// ==============================================================================================
// WHY A LEDGER AND NOT A COUNTER
// ==============================================================================================
//
// The counter this replaces was `size_t g_checked` and it could express two outcomes -- asked and
// passed, asked and failed. On 2026-08-30 this probe met the other two. Its sink proofs COULD NOT BE
// ASKED (CAkSinkXAudio2 had not been constructed, so both globals the proof reads were still at
// their .data zero-init value) and its rate ladder was NEVER ASKED (it sits behind `proven > 0`).
// The first was scored as a failure and the second left no trace at all: `checked: 4` against a
// ceiling of eight, with nothing in the artifact saying eight.
//
// So: every check is named up front, starts NotAsked, and carries a reason when it is deliberately
// excluded. `Asked() + NotAsked() == kCheckCeiling` always, and the ceiling is declared here rather
// than counted from what ran -- a denominator supplied by the work being measured cannot fail.
//
// ★ THE CEILING IS THIS PACKET'S DECLARED CONSTANT, NOT THE SPEC'S. The design does not
// enumerate these eight; an earlier revision of this file said it did, and that claim was false. The
// enumeration lives here and nowhere else, which means a change that removes a check from this enum,
// its name table, the probe and the offline suite together is indistinguishable from an intended
// change of scope -- until the design carries the list, that residual is stated rather than hidden.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace replay {
namespace audio {
namespace ledger {

enum CheckId {
    kCheckExportsBound = 0,
    kCheckSinkProofs,
    kCheckModuleAgrees,
    kCheckRateBaseline,
    kCheckRate050,
    kCheckRate025,
    kCheckRate010,
    kCheckOfflineLock,
    kCheckCeiling,   // the count, and every artifact carries it
};

// Unsized on purpose: the enum is the enumeration and this table must match it entry for entry.
inline constexpr const char* kCheckNames[] = {
    "ak-exports-bound", "sink-proofs", "a6-module-agrees", "rate-baseline",
    "rate-0.5x",        "rate-0.25x",  "rate-0.1x",        "offline-bit-lock",
};
static_assert(sizeof(kCheckNames) / sizeof(kCheckNames[0]) == kCheckCeiling,
              "the CheckId enum and kCheckNames disagree: a check was added to one and not the other");

// The three verdict words a final record may carry are the harness's (the test harness
// $script:VerdictWords). This fourth word is deliberately NOT one of them: a provisional record is
// not a verdict, and a word the harness does not recognise as final cannot end a launch or be
// tallied even if the `provisional` flag were ever ignored.
inline constexpr const char* kInProgressVerdict = "IN-PROGRESS";

// A check that has simply not been reached yet, on a record written mid-run. Only a provisional
// record may carry it; on a final record it is a violation, because a final record has finished
// asking and a check it still calls pending is one it forgot.
inline constexpr const char* kPendingReason = "provisional: not yet reached";

// A check nobody scored and nobody excluded, on a final record. Rendered this loudly so an accepted
// exclusion and a forgotten check cannot look alike.
inline constexpr const char* kForgottenReason =
    "NO REASON WAS RECORDED: this check was forgotten, not excluded";

// The point at which this probe's answer is valid, per tests/audio/W2-H.probe.json.
inline constexpr const char* kCompletesAt = "match";

enum class CheckState : unsigned char { NotAsked = 0, Passed, Failed };

struct Ledger {
    CheckState state[kCheckCeiling] = {};
    const char* reason[kCheckCeiling] = {};

    void Check(CheckId id, bool ok) { state[id] = ok ? CheckState::Passed : CheckState::Failed; }

    // A check this run will not perform, and why. The reason is what makes the exclusion
    // reviewable. The first one wins: the nearest cause is the most specific, and a later blanket
    // exclusion must not overwrite it.
    void Skip(CheckId id, const char* why) {
        if (state[id] == CheckState::NotAsked && reason[id] == nullptr) {
            reason[id] = why;
        }
    }

    size_t Count(CheckState want) const {
        size_t n = 0;
        for (int i = 0; i < kCheckCeiling; ++i) {
            if (state[i] == want) {
                ++n;
            }
        }
        return n;
    }
    size_t Asked() const { return Count(CheckState::Passed) + Count(CheckState::Failed); }
    size_t Failed() const { return Count(CheckState::Failed); }
    size_t NotAsked() const { return Count(CheckState::NotAsked); }
};

// Everything downstream of a sink that was never constructed, excluded together and for one reason.
// The probe and the offline replay both call this, so the set cannot drift between them.
inline void SkipSinkDependents(Ledger& l, const char* reason) {
    const CheckId ids[] = {kCheckSinkProofs, kCheckModuleAgrees, kCheckRateBaseline,
                           kCheckRate050,    kCheckRate025,      kCheckRate010};
    for (CheckId id : ids) {
        l.Skip(id, reason);
    }
}

// ★ THREE-VALUED, AND THE ORDER IS THE ARGUMENT. PASS with the sink never seen would read as "the
// mechanism works"; FAIL would read as "the sink proofs ran and did not hold". Both are false, and
// the second is the one that cost a launch. A check that WAS asked and failed still outranks a
// could-not-ask: the offline-bit lock is a property of the launch, and a launch that ran
// unprotected is a real failure whatever the audio engine did.
//
// PASS means every check was asked. It is derived from the ledger, not from a zero failure count,
// because zero failures is also what a run that asked nothing reports.
inline const char* Verdict(const Ledger& l, bool couldNotAsk) {
    if (l.Failed() > 0) {
        return "FAIL";
    }
    if (couldNotAsk || l.Asked() < kCheckCeiling) {
        return "NOT-RUN";
    }
    return "PASS";
}

// TIER-A-CONFIRMED asserts the ladder ran, at the point the descriptor declares. Eight asked with
// none failed, at a live match, is the only state it is true of.
inline const char* Token(const Ledger& l, bool atDeclaredPoint) {
    return (atDeclaredPoint && l.Asked() == kCheckCeiling && l.Failed() == 0) ? "TIER-A-CONFIRMED"
                                                                               : "none";
}

// The `unasked` array, as JSON objects, for the artifact.
//
// ★ A check excluded because the sink never existed and a check nobody remembered to score both sit
// at NotAsked; the reason is the only thing that separates them, so its absence is printed as
// loudly as its presence. On a provisional record a reason-less check has not been forgotten, it has
// not been reached, and it says so in words the check can hold a final record against.
inline void RenderUnasked(const Ledger& l, bool provisional, char* out, size_t n) {
    out[0] = '\0';
    for (int i = 0; i < kCheckCeiling; ++i) {
        if (l.state[i] != CheckState::NotAsked) {
            continue;
        }
        const char* reason = l.reason[i];
        if (reason == nullptr) {
            reason = provisional ? kPendingReason : kForgottenReason;
        }
        char one[600];
        _snprintf_s(one, sizeof one, _TRUNCATE, "%s{ \"check\": \"%s\", \"reason\": \"%s\" }",
                    out[0] != '\0' ? ", " : "", kCheckNames[i], reason);
        strncat_s(out, n, one, _TRUNCATE);
    }
}

// Everything a record carries besides the ledger. Times are milliseconds from the probe's own first
// tick; a negative time means "never", which is printed as null because -1 is not a time.
struct Record {
    const char* probeId = "";
    bool provisional = false;
    bool couldNotAsk = false;
    bool completionReached = false;
    double wwiseUpMs = -1.0;
    double sinkWaitMs = 0.0;
    uint32_t sinkPolls = 0;
    double selectorFirstMs = -1.0;
    double sinkListFirstMs = -1.0;
    double matchSeenMs = -1.0;
    const char* detail = "";
};

inline const char* MsJson(double ms, char* buf, size_t n) {
    if (ms < 0.0) {
        return "null";
    }
    _snprintf_s(buf, n, _TRUNCATE, "%.0f", ms);
    return buf;
}

// ★ ONE WRITER FOR BOTH RECORDS, AND THE PROVISIONAL ONE IS NOT A VERDICT. It carries the in-progress
// word, every unreached check with the pending reason, no token, and truncated as null because
// whether the launch will reach its declared point is not yet decidable. The final record derives
// its verdict from the ledger and from whether the declared completion point was reached: a
// measurement taken before the match is not the measurement that was queued, and that is a
// could-not-ask, not a pass.
inline void FormatRecord(const Ledger& l, const Record& r, char* out, size_t n) {
    const char* verdict =
        r.provisional ? kInProgressVerdict : Verdict(l, r.couldNotAsk || !r.completionReached);
    const char* token = r.provisional ? "none" : Token(l, r.completionReached);
    const char* truncated = r.provisional ? "null" : (r.completionReached ? "false" : "true");

    char unasked[3072];
    RenderUnasked(l, r.provisional, unasked, sizeof unasked);

    char wwiseAt[32];
    char selectorAt[32];
    char listAt[32];
    char matchAt[32];
    _snprintf_s(out, n, _TRUNCATE,
                "{ \"probe\": \"%s\", \"verdict\": \"%s\", \"checked\": %llu,\n"
                "  \"provisional\": %s,\n"
                "  \"completesAt\": \"%s\", \"completionReached\": %s, \"truncated\": %s, "
                "\"matchSeenMs\": %s,\n"
                "  \"checkCeiling\": %d, \"asked\": %llu, \"notAsked\": %llu,\n"
                "  \"unasked\": [%s],\n"
                "  \"wwiseUpMs\": %s, \"sinkWaitMs\": %.0f, \"sinkPolls\": %u,\n"
                "  \"selectorFirstNonZeroMs\": %s, \"sinkListFirstNonNullMs\": %s,\n"
                "  \"token\": \"%s\",\n"
                "  \"detail\": \"%s\" }\n",
                r.probeId, verdict, (unsigned long long)l.Asked(), r.provisional ? "true" : "false",
                kCompletesAt, r.completionReached ? "true" : "false", truncated,
                MsJson(r.matchSeenMs, matchAt, sizeof matchAt), static_cast<int>(kCheckCeiling),
                (unsigned long long)l.Asked(), (unsigned long long)l.NotAsked(), unasked,
                MsJson(r.wwiseUpMs, wwiseAt, sizeof wwiseAt), r.sinkWaitMs, r.sinkPolls,
                MsJson(r.selectorFirstMs, selectorAt, sizeof selectorAt),
                MsJson(r.sinkListFirstMs, listAt, sizeof listAt), token, r.detail);
}

}  // namespace ledger
}  // namespace audio
}  // namespace replay
