// The observer's ARMS, as pure functions over what it saw.
//
// The observer WATCHES the shipped modules through the surfaces they already
// publish -- their log lines and their status accessors. It re-implements no product path, so a
// green here is a statement about the shipped code. Everything in this header is therefore a
// READER: it parses lines the product wrote and reads counters the product keeps, and it computes
// nothing the product computes.
//
// It is a header of pure functions on purpose. probe_replay.cpp fills LogFacts from the real log
// file and StatusFacts from the real accessors; tests/replay/test_observer.cpp fills both by hand.
// That is the only way a battery whose inputs only exist inside a running game can be proved to
// discriminate its outcomes before the launch that would use it.
//
// FOUR OUTCOMES, NOT TWO. Every row answers asked-pass, asked-fail or
// could-not-ask, each with the reason on the line. The fourth, never-asked, is deliberately NOT
// representable here: a row in the arm's table is evaluated on every record, so the only way a
// listed row can go unanswered is for the record to omit it entirely -- which is what
// tests/replay/check.ps1 scores, against the DESCRIPTOR's row list rather than against this table
// (9.12: a denominator supplied by the work being measured cannot fail).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

// The words this row keys on are the PRODUCERS' words, and they are imported rather than
// spelled. The row used to strcmp "Ok"/"ViewState"/"Moved"/"Still" while
// W2-E's GetViewPose result and cc::PoseMotionBetween emit "ok"/"view-state"/"moved"/"still",
// so it could return neither Pass nor Fail on any real launch. These two headers are
// DECLARATIONS: the row compares the enum the producer defines and calls nothing, so this
// header still links no product code.
#include "camera/camera_control.h"
// The close's own published record. A DECLARATION as well -- the rows read
// StopFactsSnapshot's fields and call nothing out of it, so this header still links no product
// code. The counts these rows score are now the capture module's, not a re-parse of its JSON.
#include "capture/capturestop.h"
#include "engine/camera_writepoint.h"

namespace probe_replay {

inline constexpr size_t kRowLineCap = 400; // facts-ok: AUDIO.LISTENER_COUNT_OFF a text-buffer length, not that offset

enum class Ask { CouldNotAsk = 0, Pass = 1, Fail = 2 };

inline const char* AskWord(Ask a) {
    switch (a) {
        case Ask::Pass: return "asked-pass";
        case Ask::Fail: return "asked-fail";
        default:        return "could-not-ask";
    }
}

// The arms of 6.4, as a mask, because most rows ride more than one.
enum ArmBit : unsigned {
    kArmCapture  = 1u,
    kArmPlayback = 2u,
    kArmBrowser  = 4u,
    kArmIdle     = 8u,
};

// ---------------------------------------------------------------------------------------------
// Line reading.
//
// A log line is `HH:MM:SS.mmm LEVEL [tag] message`. The tag is compared for equality and the
// message by PREFIX from its first byte -- never by searching the whole line for a fragment.
// Substring matching is this project's commonest false pass (9.5), and a line that merely mentions
// `roster:` inside somebody's diagnostic must not be read as the roster line.
// ---------------------------------------------------------------------------------------------

// Splits a formatted line into its tag and its message. False if the line is not one of ours.
inline bool SplitLogLine(const char* line, const char** tag, size_t* tagLen, const char** msg) {
    const char* open = nullptr;
    const char* close = nullptr;
    for (const char* p = line; *p != '\0'; ++p) {
        if (*p == '[' && close == nullptr) {
            open = p;
        } else if (*p == ']' && open != nullptr && *(p + 1) == ' ') {
            close = p;
            break;
        }
    }
    if (open == nullptr || close == nullptr || close <= open + 1) {
        return false;
    }
    *tag = open + 1;
    *tagLen = static_cast<size_t>(close - open - 1);
    *msg = close + 2;
    return true;
}

inline bool TagIs(const char* tag, size_t tagLen, const char* want) {
    return strlen(want) == tagLen && strncmp(tag, want, tagLen) == 0;
}

inline bool StartsWith(const char* s, const char* prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

// A token inside a line that the producer composed from several pieces -- `owners: N/M mapped` is
// built into a buffer and printed inside a larger panel line, so a prefix test cannot reach it.
// The token must start the message or follow a space or a `|`, so `xowners: ` never matches.
inline const char* FindToken(const char* msg, const char* token) {
    const size_t n = strlen(token);
    for (const char* p = msg; *p != '\0'; ++p) {
        if (strncmp(p, token, n) != 0) {
            continue;
        }
        if (p == msg || *(p - 1) == ' ' || *(p - 1) == '|') {
            return p;
        }
    }
    return nullptr;
}

// Copies the run of characters after `key` up to any byte in `stops`. False if `key` is absent.
inline bool WordAfter(const char* msg, const char* key, char* out, size_t outN, const char* stops) {
    const char* at = FindToken(msg, key);
    if (at == nullptr || outN == 0) {
        return false;
    }
    at += strlen(key);
    size_t n = 0;
    while (at[n] != '\0' && strchr(stops, at[n]) == nullptr && n + 1 < outN) {
        ++n;
    }
    memcpy(out, at, n);
    out[n] = '\0';
    return true;
}

// Reads `<u32>/<u32>` immediately after `key`. False if absent or not that shape.
inline bool PairAfter(const char* msg, const char* key, unsigned* a, unsigned* b) {
    const char* at = FindToken(msg, key);
    if (at == nullptr) {
        return false;
    }
    at += strlen(key);
    unsigned x = 0, y = 0;
    if (sscanf_s(at, "%u/%u", &x, &y) != 2) {
        return false;
    }
    *a = x;
    *b = y;
    return true;
}

inline bool UnsignedAfter(const char* msg, const char* key, unsigned* out) {
    const char* at = FindToken(msg, key);
    if (at == nullptr) {
        return false;
    }
    unsigned v = 0;
    if (sscanf_s(at + strlen(key), "%u", &v) != 1) {
        return false;
    }
    *out = v;
    return true;
}

// The 64-bit sibling. The camera module prints its refusal count with %llu off a `unsigned long
// long`, and a counter read back through `unsigned` would wrap at 2^32 into a smaller, plausible
// number -- an undercount that reads as a healthier session than the one that happened.
inline bool UnsignedLongLongAfter(const char* msg, const char* key, unsigned long long* out) {
    const char* at = FindToken(msg, key);
    if (at == nullptr) {
        return false;
    }
    unsigned long long v = 0;
    if (sscanf_s(at + strlen(key), "%llu", &v) != 1) {
        return false;
    }
    *out = v;
    return true;
}

// The run between `key` and the first `end` after it. `key` is anchored by FindToken's rule, so a
// key buried inside a word does not open the span; `end` is a plain search from the key's tail
// BECAUSE a delimiter like "; refusals " starts with punctuation and would never satisfy that rule.
// Both must be present in that order: a line carrying only one CLEARS `out` and returns false.
//
// The clear is the point (AUDIT_L5FIX W2-R minor). A second, malformed line of the same shape used
// to leave the FIRST line's text in the buffer, and the row went on reporting a cause the current
// line does not carry -- a stale sentence rendered as this session's answer. A reader that cannot
// parse must leave "no text" behind, not the last text it understood.
inline bool TextBetween(const char* msg, const char* key, const char* end, char* out, size_t outN) {
    if (outN == 0) {
        return false;
    }
    out[0] = '\0';
    const char* at = FindToken(msg, key);
    if (at == nullptr) {
        return false;
    }
    at += strlen(key);
    const char* stop = strstr(at, end);
    if (stop == nullptr) {
        return false;
    }
    size_t n = static_cast<size_t>(stop - at);
    if (n + 1 > outN) {
        n = outN - 1;
    }
    memcpy(out, at, n);
    out[n] = '\0';
    return true;
}

// One `<CAND>` out of ARCH-10 section 4.2's evidence line, keyed on `<Verdict>` and nothing else.
//
// `<CAND> ::= <PTR16> " " <TABLE> | "null" | "unreadable"`, and `<TABLE>` opens with
// `<Verdict>/<Why> base=...`. This reader takes the verdict word and the `<Verdict>/<Why>` pair
// verbatim and stops there: the rest of the rendering is W1-C's FormatTable, and 4.2 forbids this
// file carrying a second copy of that format. `read` is the shape being understood at all; `judged`
// is a `<TABLE>` having been rendered, which is the only case where `verdict` means anything.
struct CsCandidate {
    bool read = false;
    bool judged = false;
    char verdict[24] = {0};      // Ready | Empty | NotReady | Unreadable | NotChecked
    char rendered[96] = {0};     // "<Verdict>/<Why>" verbatim, or "null" / "unreadable"
    // The WHOLE `<CAND>` text this key rendered, verbatim, up to the next ` | `. ARCH-10 4.2
    // forbids this reader holding a second copy of FormatTable's format, so the fields that
    // separate the 99-system space at +0x2B08 from the 35-system one at game+0x7AF8 are CARRIED,
    // never parsed: nothing here reads `count=` or `cap=`, and a future field appears in the
    // detail for free (AUDIT_L5_RUN3 section 12 item 4).
    // 240, not 224: entity_module.cpp's widest <CAND> is 16 hex digits, a space and
    // FormatTable's longest line (218 bytes) = 235, and a `full` that could not hold it was
    // quoting a cut candidate into the detail with only `fullTruncated` to say so.
    char full[240] = {0};
    bool fullTruncated = false;
    // The <PTR16> this key rendered, empty for `null` / `unreadable`. ARCH-11 S4: if two offsets
    // name ONE object the line distinguishes nothing, so the pointers are compared.
    char ptr[20] = {0};
};

// The run of non-space bytes at `at`. False if it is empty or longer than the buffer, so an
// over-long token is a refusal rather than a silently truncated word that still compares equal to
// something shorter.
inline bool CopyToken(const char* at, char* out, size_t outN) {
    size_t n = 0;
    while (at[n] != '\0' && at[n] != ' ' && n + 1 < outN) {
        ++n;
    }
    if (n == 0 || (at[n] != '\0' && at[n] != ' ')) {
        return false;
    }
    memcpy(out, at, n);
    out[n] = '\0';
    return true;
}

// 4.2 spells `<PTR16>` as 16 uppercase hex digits. The LENGTH is what is checked, plus hex-ness in
// either case: the pointer's value is not read by this file at all -- it is skipped to reach the
// verdict -- so rejecting a lowercase rendering would turn a cosmetic drift in the producer into a
// could-not-ask on the one row that confirms the offset.
inline bool IsPtr16(const char* t) {
    for (size_t i = 0; i < 16; ++i) {
        if (strchr("0123456789abcdefABCDEF", t[i]) == nullptr || t[i] == '\0') {
            return false;
        }
    }
    return t[16] == '\0';
}

// ARCH-12 4.3's <SYM16>: 16 uppercase hex digits, `null` or `unreadable`, and NOTHING else. A
// token this reader does not recognise leaves `out` empty rather than being carried as if it were
// a symbol -- these two fields are the evidence that earns the identity check its promotion to a
// gate later, and a field half-read is not evidence. No second copy of the emitter's format lives
// here: what is checked is the shape the grammar froze, not how snprintf spelled it.
inline void ReadSym16(const char* msg, const char* key, char* out, size_t outN) {
    out[0] = '\0';
    const char* at = FindToken(msg, key);
    if (at == nullptr) {
        return;
    }
    at += strlen(key);
    char tok[24] = {0};
    if (!CopyToken(at, tok, sizeof tok)) {
        return;
    }
    if (strcmp(tok, "null") != 0 && strcmp(tok, "unreadable") != 0 && !IsPtr16(tok)) {
        return;
    }
    strcpy_s(out, outN, tok);
}

inline void ReadCsCandidate(const char* msg, const char* key, CsCandidate* out) {
    *out = CsCandidate();
    const char* at = FindToken(msg, key);
    if (at == nullptr) {
        return;
    }
    at += strlen(key);
    {
        const char* end = strstr(at, " | ");
        size_t len = end != nullptr ? static_cast<size_t>(end - at) : strlen(at);
        out->fullTruncated = len >= sizeof out->full;
        if (out->fullTruncated) {
            len = sizeof out->full - 1;
        }
        memcpy(out->full, at, len);
        out->full[len] = '\0';
    }
    char first[24] = {0};
    if (!CopyToken(at, first, sizeof first)) {
        return;
    }
    if (strcmp(first, "null") == 0 || strcmp(first, "unreadable") == 0) {
        out->read = true;
        strcpy_s(out->rendered, sizeof out->rendered, first);
        return;
    }
    if (!IsPtr16(first)) {
        return;
    }
    strcpy_s(out->ptr, sizeof out->ptr, first);
    at += strlen(first);
    if (*at != ' ') {
        return;
    }
    ++at;
    char pair[96] = {0};
    if (!CopyToken(at, pair, sizeof pair)) {
        return;
    }
    const char* slash = strchr(pair, '/');
    if (slash == nullptr || slash == pair || slash[1] == '\0') {
        return;       // `<Verdict>` alone, or `<Verdict>/` with no `<Why>`, is not the rendering
    }
    const size_t vn = static_cast<size_t>(slash - pair);
    if (vn + 1 > sizeof out->verdict) {
        return;
    }
    memcpy(out->verdict, pair, vn);
    out->verdict[vn] = '\0';
    strcpy_s(out->rendered, sizeof out->rendered, pair);
    out->read = true;
    out->judged = true;
}

// ---------------------------------------------------------------------------------------------
// What the log tail has latched. Every field is "what the producer's own line said", never a
// derived judgement -- the judgement is in the Ask* functions below, where a test can drive it.
// ---------------------------------------------------------------------------------------------
struct LogFacts {
    // W1-E, src/engine/entity_module.cpp:197 `[entity] bound game space %p (...): entity %s; ...`
    bool boundSeen = false;
    char boundEntity[64] = {0};          // `available`, or the entity's own diagnostic
    char boundLine[kRowLineCap] = {0};
    unsigned boundCount = 0;

    // W1-E, ARCH-12 section 4.3's FROZEN evidence line, emitted once per bind attempt:
    // `[entity] cs-space candidates: ng=<PTR16> | +0x8=<CAND> | +0x2B08=<CAND> | +0x2B10=<CAND> |
    //  game+0x7AF0=<CAND> | game+0x7AF8=<CAND> | spawnsym=<SYM16> | levelsym=<SYM16>`.
    //
    // Latched from the LAST such line and re-derived from that line alone. The five candidates are
    // re-judged on every bind attempt, so the latest attempt is the current answer -- and a line
    // this reader cannot parse CLEARS what the previous one said rather than leaving a stale
    // verdict standing as the session's answer.
    bool csSpaceSeen = false;
    unsigned csSpaceCount = 0;
    CsCandidate csSpaceAt8;
    CsCandidate csSpaceAt2B08;
    // ARCH-12 4.3's fifth column and the row's SUBJECT: *(netGame+0x2B10) is the netGame's LEVEL
    // CncaGameSpaceMT and the spectator-camera CS is registered there (ARCH-12 2.2/2.3). A line
    // that does not carry this key leaves `keyPresent` false, and that is a could-not-ask naming
    // the missing column -- never a verdict derived from the four columns it does carry.
    CsCandidate csSpaceAt2B10;
    bool csSpaceAt2B10KeyPresent = false;
    CsCandidate csSpaceAt7AF0;
    CsCandidate csSpaceAt7AF8;
    // <SYM16>, verbatim: 16 hex digits, `null` or `unreadable`. INFORMATION (4.3 rule 6, R2's
    // rejected gate) -- carried, printed, never allowed to move a verdict. Empty when the line
    // carried no such key, which is what a pre-ARCH-12 line leaves behind.
    char csSpaceSpawnSym[24] = {0};
    char csSpaceLevelSym[24] = {0};
    // 1408, not 640. Re-derived for ARCH-12 4.3's grammar rather than bumped: the widest message
    // half is `cs-space candidates: ng=` (24) + <PTR16> (16) + the five ` | <label>=` (8 + 11 + 11
    // + 15 + 15 = 60) + five widest <CAND> (5 * 235 = 1175) + ` | spawnsym=` and ` | levelsym=`
    // (2 * 12) + two <SYM16> (2 * 16) = 1331 bytes; 1408 is the next 128-byte step above it. The
    // real L5 run 3 lines were 405/505/504 on the wire with 378/478/477-byte message halves under
    // a 400-byte buffer that quoted a CUT line back to the operator with no sign it had been cut.
    // The flag stays because a future field can still outgrow this.
    char csSpaceLine[1408] = {0};
    bool csSpaceLineTruncated = false;

    // The one fact that is NOT taken from the latest line. `+0x8` is the guard's sensitivity canary
    // (4.2 rule 5) and one Ready anywhere voids every judgement the guard made in this launch, so a
    // later sample rendering it NotReady again cannot un-void them. Latched, with the sample that
    // did it, because "which line said so" is the whole content of that answer.
    bool csSpaceAt8ReadyEver = false;
    char csSpaceAt8ReadyLine[kRowLineCap] = {0};

    // W2-R, src/engine/session.cpp:593 `[session] NetGame state %d (%s) -> %d (%s)` -- the
    // LEVEL-LOAD predicate, and the reason it is this line and not the entity bind: it is logged
    // independently of the bind. L5 filed `w2r-browser-opened-over-level` as could-not-ask
    // "level-not-reached" over a launch whose level loaded at 04:16:38, because the probe read the
    // bind line as its proxy for the level and the bind is what was missing (ARCH-10 4.1 row 4).
    // B2 / AUDIT_L5_RUN3 section 12 item 2. The ONE clock.
    //
    // Two stamps have to be ordered here and they are written by two different clocks: the log's
    // wall clock, printed at the head of every line, and the probe's monotonic SinceStartMs().
    // Comparing them raw is a units bug -- 12:57:06.393 is 46626393 ms since midnight and would
    // sit "after" any launch-relative stamp for ever. The conversion is not invented here: the
    // observer's own heartbeat prints BOTH clocks on ONE line,
    //   `12:57:12.284 INFO  [observer] SURVIVE 25231 ms, heartbeat 26, arm BROWSER`
    // so armEpochWallMs = (wall clock of that line) - (its own SinceStartMs) is MEASURED. Across
    // L5 run 3's 25 s of heartbeats that anchor moves by 2 ms (12:56:47.055 vs 12:56:47.053), so
    // the latest line is taken and the residual drift is far below anything this row decides on.
    bool armEpochKnown = false;
    double armEpochWallMs = 0.0;
    // The last SURVIVE line's own monotonic reading: this launch's OBSERVED LIFETIME, and the
    // bound every interval measured on this epoch has to fall inside (AUDIT_L5_RUN_4 F10 (b)).
    double armEpochMonoMs = 0.0;

    // ★ And it is the sole cause of that run's NOT-RUN. `replay::LogPath()` is
    // CUMULATIVE: it outlives the launch. The record that made the finding read 42355 lines from
    // NINE launches to answer questions about one, so "the FIRST state-7 line" meant "the first
    // since the file was created" -- a stamp from the previous day that put the level load
    // 40840076 ms into a 240 s launch, after which the completion predicate went negative, the
    // probe never filed, and the harness killed it at the cap.
    //
    // `[boot] attached` is written exactly once per launch by bootstrap::Attach and is the FIRST
    // line the plugin writes. It is this launch's epoch: every fact accumulated before it belongs
    // to somebody else's launch and is DISCARDED rather than reconciled. Its ABSENCE is a refusal
    // and never a licence to read the whole file -- without the anchor there is no way to tell
    // whose lines these are, and a stamp taken anyway may be any of nine launches'.
    bool launchAnchorSeen = false;
    bool launchAnchorClockKnown = false;
    double launchAnchorWallMs = 0.0;
    char launchAnchorLine[180] = {0};
    unsigned long long launchesInLog = 0;      // `[boot] attached` lines over the whole source
    unsigned long long linesBeforeLaunch = 0;  // lines discarded as earlier launches'
    unsigned long long linesThisLaunch = 0;
    unsigned armEpochLines = 0;
    char armEpochLine[kRowLineCap] = {0};

    // The FIRST transition into a level state, on the log's clock. First and not last: the answer
    // this row wants is when the level began loading, and 7 -> 8 -> 9 are three lines about one
    // load.
    bool levelStampKnown = false;
    double levelWallMs = 0.0;
    char levelStampLine[kRowLineCap] = {0};

    bool sessionStateSeen = false;
    int sessionState = -1;                  // the state the last transition arrived AT
    bool sessionLevelStateSeen = false;     // 7 loading level, 8 ready for game, 9 in game
    int sessionLevelState = -1;
    char sessionStateLine[kRowLineCap] = {0};
    char sessionLevelStateLine[kRowLineCap] = {0};
    // L3-RUN-2 (a). 7 is `loading level` and 9 is `in game`; on the recorded run they are
    // 14.8 s and a whole aborted load cycle apart, and every world-shaped read taken between
    // them was taken against a world that did not exist yet. Kept as its own fact rather than
    // as `sessionLevelState == 9`, because that field carries the LAST level state seen and a
    // launch that reaches 9 and then drops back to 7 has still been in game.
    bool sessionInGameSeen = false;
    char sessionInGameLine[kRowLineCap] = {0};

    // W2-B, src/playback/core/playback_module.cpp:424 `[playback] services re-bound: entity %s
    // (%s, bind epoch %llu%s), pose %s, netvar %s`
    bool reboundSeen = false;
    char reboundNetvar[16] = {0};
    char reboundEntity[16] = {0};
    char reboundLine[kRowLineCap] = {0};

    // W2-B, src/playback/core/apply.cpp:28-39 -- the fidelity state's own SENTENCE, printed by its
    // consumer inside a `|`-delimited panel line. The whole sentence and not its first word:
    // `STORED, NOT APPLIED` and `records present, none due at this playhead` both begin at a word
    // that decides nothing, and `ad17dd9` made the set four sentences wide.
    bool netvarStateSeen = false;
    char netvarSaid[160] = {0};

    // W2-C, src/identity/dresser.cpp:403 `[identity] roster: %u/%u players dressed, %u NOT dressed`.
    // FIVE lines in that file share the `roster: ` prefix under this tag and two of them carry an
    // `N/M` pair, so the prefix is a FAMILY, not this subject. `rosterOther*` records the family
    // members that were skipped, because "a roster line was printed and it was not this one" is a
    // different answer from "the roster path was never reached".
    bool rosterSeen = false;
    unsigned rosterDressed = 0;
    unsigned rosterPlayers = 0;
    unsigned rosterNotDressed = 0;
    char rosterLine[kRowLineCap] = {0};
    unsigned rosterOtherCount = 0;
    char rosterOtherLine[kRowLineCap] = {0};

    // W2-C, src/playback/core/playback_module.cpp:726 `owners: %u/%u mapped[ | INV-OWN1 REFUSED]...`
    bool ownersSeen = false;
    unsigned ownersMapped = 0;
    unsigned ownersPlayers = 0;
    bool ownersRefused = false;
    bool ownersLocalUnknown = false;
    char ownersLine[kRowLineCap] = {0};

    // ---- W2-P1's post-provision census, src/engine/pawn_provision.cpp:328-431, tag "pawn" -----
    // Every key below is read with FindToken, which anchors on a space or a '|', so `class=` is
    // never answered by `subclass=` and `driveable=` is never answered by `undriveable=`. A
    // substring match here would score a near-miss line as a census, which is this project's
    // commonest false pass.
    bool pawnCensusSeen = false;
    unsigned pawnProvisioned = 0;
    unsigned pawnMapped = 0;
    unsigned pawnFailed = 0;
    char pawnStateWord[16] = {0};        // `known` / `UNKNOWN`
    char pawnInMatch[16] = {0};          // `yes` / `no` / `unknown`
    // The census's own refusal. It publishes NO verdict beside itself, so this is a could-not-ask
    // and never an answer about any arm of FACT-PP1.
    bool pawnRefusedNoEntityService = false;
    bool pawnTotalsSeen = false;
    unsigned pawnResolvedRows = 0;       // `census totals over %u provisioned slot(s)`
    unsigned pawnRemoteCis = 0;
    unsigned pawnLocalCis = 0;
    unsigned pawnBothSurfaces = 0;
    // The per-slot rows. `slot=%u (recorded %u)` is slot_resolved and slot_recorded, and the two
    // being equal on a solo capture is what says the map never fired (ENG-W2-P1-4 reading rule 1).
    unsigned pawnSlotRows = 0;
    unsigned pawnClassLocal = 0;
    unsigned pawnClassRemote = 0;
    unsigned pawnClassUnknown = 0;
    unsigned pawnDriveableRows = 0;      // rows where EITHER surface read a body back
    unsigned pawnEntityNamed = 0;        // rows carrying something other than `entity=none`
    unsigned pawnUnmappedEntries = 0;    // `census: map entry %u ... carries no resolved slot`
    unsigned pawnRefusalRows = 0;        // handle rows whose refusal= is not None
    char pawnFirstRefusal[48] = {0};
    char pawnFirstSlotMap[32] = {0};     // "<recorded>-><resolved>"
    bool pawnFactVerdictSeen = false;
    bool pawnFactUndetermined = false;
    char pawnCensusLine[kRowLineCap] = {0};
    char pawnSlotLine[kRowLineCap] = {0};

    // W2-C, src/playback/core/playback_module.cpp:313 `INV-OWN1 armed against local index %u after
    // %llu ticks`
    bool invOwn1Armed = false;
    bool invOwn1LocalUnknownArm = false;
    char invOwn1Line[kRowLineCap] = {0};
    char invOwn1UnknownLine[kRowLineCap] = {0};

    // W2-B, src/playback/core/playback_module.cpp:458 `[playback] body handles re-bound: %u bound,
    // %u absent this session, %u not net ids, %u unresolvable`
    bool bodiesSeen = false;
    unsigned bodiesBound = 0;
    unsigned bodiesUnresolvable = 0;
    // The other two counters of the same line. `bound + absent + not-net-id + unresolvable` is the
    // size of the candidate set the rebind walked, and a set of size zero is a different finding
    // from a set it failed to resolve (L3-RUN-7 F1). `bodiesCountersParsed` is what stops a line
    // in some older spelling, which fills none of them, from reading as an empty candidate set.
    unsigned bodiesAbsent = 0;
    unsigned bodiesNotNetIds = 0;
    bool bodiesCountersParsed = false;
    char bodiesLine[kRowLineCap] = {0};
    // `ad17dd9` gave the line a trailing `(%s)` naming the edge that closed the pair -- `window
    // rebuilt` or `entity re-bound`. Which one it was is the answer to a different question from
    // whether it happened, so it is parsed rather than left for a reader to spot in the line.
    char bodiesTrigger[40] = {0};

    // W2-E, src/camera/camera_module.cpp:72 `[camera] camera control bound: rig %p ... rig enabled
    // flag %d; pose-owner: %s, conflicts %llu` and its `camera control refused` sibling.
    bool cameraBoundSeen = false;
    bool cameraRefusedSeen = false;
    unsigned cameraRigEnabled = 0;
    unsigned cameraConflicts = 0;
    char cameraPoseOwner[64] = {0};
    char cameraLine[kRowLineCap] = {0};
    // The refusal's own sentence -- everything between the `): ` that closes the line's pointer
    // preamble and the `; pose-owner: ` that opens its trailer -- and how many refusal lines this
    // session printed. L5 run 3 printed twelve and no row scored one.
    char cameraRefusalWhy[256] = {0};
    unsigned cameraRefusalCount = 0;

    // A [camera] control line in the PRE-162027c spelling (`on game space %p`). The matchers
    // require `on level space ` now, so such a line parses to nothing -- and a reader that
    // shrugged would leave both camera rows saying "the module never reached a rig probe",
    // which is a confident wrong answer about an older log rather than a refusal to answer.
    bool cameraStaleNounSeen = false;
    char cameraStaleNounLine[kRowLineCap] = {0};

    // W2-E, src/camera/camera_module.cpp:142 `[camera] camera control view read refused: <sentence>;
    // refusals %llu; pose-owner: %s, conflicts %llu, arb %s, rel-refused %llu`, published once per
    // binding session for the FIRST refusal camera_control latched. The sentence is W1-H's `why`:
    // it can carry ':' and ',' -- including the word "refused:" -- but no producer of it emits a
    // semicolon, so "; refusals " is the only end it has.
    //
    // The pose-owner on THIS line gets its own field rather than the shared cameraPoseOwner. The
    // two lines are published at different moments of the same session and w2e-camera-control-bound
    // renders the shared one; letting a later refusal write it would make that row's detail depend
    // on a line it does not read.
    bool cameraViewRefusedSeen = false;
    unsigned long long cameraViewRefusals = 0;
    char cameraViewRefusal[256] = {0};
    char cameraViewRefusalPoseOwner[64] = {0};
    char cameraViewRefusalLine[kRowLineCap] = {0};

    // W2-H, the rate driver's own line for a commanded change.
    bool rateLineSeen = false;
    char rateLine[kRowLineCap] = {0};
    // L3-RUN-5 s8. `rateLine` keeps the LAST `transport rate` line, which after a full battery is
    // the 1.00x restore -- and the sentence W2-H needs is the one the driver wrote when 0.5x was
    // commanded. ratedrive.cpp:112 writes `transport rate %.2fx: %s` with IAudioRate::RateLabel(),
    // so that line carries the product's own reason word at the only moment this row is about.
    // First occurrence wins: a later 0.50x must not overwrite the one the slope window covered.
    bool rateHalfLineSeen = false;
    char rateHalfLine[kRowLineCap] = {0};
    // The audio module's two bind-time facts, both in the product's own words. Together they say
    // whether this process ever reached a mixer at all (L3-RUN-7 F2).
    bool audioBindRefusalSeen = false;
    char audioBindRefusal[240] = {0};
    bool soundPlayerResolved = false;
    char soundPlayerLine[240] = {0};
    // The file the session loaded, as [ui] named it. probe_replay.cpp opens it a second time
    // through the product's own DemoReader; this is the only place its name is carried.
    char uiLoadedPath[kRowLineCap] = {0};

    // W2-A, src/capture/capture.cpp:696 `[capture] closed: ...`. This is the CAPTURE arm's declared
    // completion point: the harness waits for it before taskkill, and its absence is
    // CAPTURE-UNFINALIZED, never a pass.
    bool captureClosedSeen = false;
    bool captureClosedEmpty = false;      // closed, but nothing was captured -- see OnLine
    char captureClosedLine[kRowLineCap] = {0};

    void OnLine(const char* line);
};

// False when the source did not fit. Callers that quote the line back to an operator must say so:
// a silently cut line reads exactly like a short one (AUDIT_L5_RUN3 section 12 item 4 -- the
// cs-space line is 501 bytes and this buffer used to be 400).
inline bool CopyLine(char* dst, size_t dstN, const char* src) {
    size_t n = strlen(src);
    const bool fits = n < dstN;
    if (!fits) {
        n = dstN - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    return fits;
}

// `HH:MM:SS.mmm ` at the head of a formatted line, as milliseconds since midnight. This is the
// LOG's clock and it is never compared with the probe's own clock except through the anchor
// LogFacts::armEpochWallMs carries. The shape is checked byte by byte: a line whose first field is
// not that stamp answers false rather than a number sscanf happened to find.
inline bool ParseLogClockMs(const char* line, double* out) {
    if (line == nullptr) {
        return false;
    }
    static const char kShape[] = "dd:dd:dd.dddd";   // 13 bytes, the last of which must be a space
    for (int i = 0; i < 12; ++i) {
        const char c = line[i];
        if (kShape[i] == 'd') {
            if (c < '0' || c > '9') return false;
        } else if (c != kShape[i]) {
            return false;
        }
    }
    if (line[12] != ' ') {
        return false;
    }
    const int hh = (line[0] - '0') * 10 + (line[1] - '0');
    const int mm = (line[3] - '0') * 10 + (line[4] - '0');
    const int ss = (line[6] - '0') * 10 + (line[7] - '0');
    const int ms = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
    if (hh > 23 || mm > 59 || ss > 59) {
        return false;
    }
    // Milliseconds per second. gatecheck sees the bare literal beside a fact-shaped context and
    // reads it as a transcription of 0x3E8; it is not one. This is a unit conversion on the LOG's
    // wall clock -- there is no vtable slot in this function and nothing here to name.
    const int msPerSecond = 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF ms<->s conversion, not that slot
    *out = static_cast<double>(((hh * 60 + mm) * 60 + ss) * msPerSecond + ms);
    return true;
}

inline void LogFacts::OnLine(const char* line) {
    const char* tag = nullptr;
    size_t tagLen = 0;
    const char* msg = nullptr;
    if (!SplitLogLine(line, &tag, &tagLen, &msg)) {
        return;
    }

    if (TagIs(tag, tagLen, "boot") && StartsWith(msg, "attached")) {
        // A NEW LAUNCH BEGINS HERE (F10). Everything above this line was written by a process that
        // has already exited, so it is dropped whole -- the two cross-launch counters are the only
        // things that survive, because "42355 lines, 9 launches, 564 of them ours" is exactly the
        // sentence the record failed to be able to say.
        const unsigned long long launches = launchesInLog + 1;
        const unsigned long long dropped = linesBeforeLaunch + linesThisLaunch;
        double wall = 0.0;
        const bool clock = ParseLogClockMs(line, &wall);
        char anchor[180];
        CopyLine(anchor, sizeof anchor, line);
        *this = LogFacts();
        launchesInLog = launches;
        linesBeforeLaunch = dropped;
        launchAnchorSeen = true;
        launchAnchorClockKnown = clock;
        launchAnchorWallMs = wall;
        CopyLine(launchAnchorLine, sizeof launchAnchorLine, anchor);
        linesThisLaunch = 1;
        return;
    }
    ++linesThisLaunch;

    if (TagIs(tag, tagLen, "observer") && StartsWith(msg, "SURVIVE ")) {
        // The anchor between the two clocks, taken only from a line that rendered the whole
        // heartbeat grammar: a `SURVIVE ` that is not followed by ` ms, heartbeat ` is somebody
        // else's sentence and must not set this launch's epoch.
        double wall = 0.0;
        double mono = 0.0;
        if (ParseLogClockMs(line, &wall) && sscanf_s(msg + 8, "%lf", &mono) == 1 &&
            strstr(msg, " ms, heartbeat ") != nullptr) {
            armEpochKnown = true;
            armEpochWallMs = wall - mono;
            armEpochMonoMs = mono;
            ++armEpochLines;
            CopyLine(armEpochLine, sizeof armEpochLine, msg);
        }
        return;
    }
    if (TagIs(tag, tagLen, "session") && StartsWith(msg, "NetGame state ")) {
        // The state this transition arrived AT, read off the `) -> ` that separates the two halves
        // of session.cpp's line. Reading the FIRST number on the line would score `5 (lobby) -> 7
        // (loading level)` as state 5. The names are printed inside parentheses and none of them
        // can contain `) -> `, so the arrow is the only place this run appears.
        const char* arrow = strstr(msg, ") -> ");
        int to = -1;
        if (arrow != nullptr && sscanf_s(arrow + 5, "%d", &to) == 1) {
            sessionStateSeen = true;
            sessionState = to;
            CopyLine(sessionStateLine, sizeof sessionStateLine, msg);
            if (to == 7 || to == 8 || to == 9) {
                sessionLevelStateSeen = true;
                sessionLevelState = to;
                CopyLine(sessionLevelStateLine, sizeof sessionLevelStateLine, msg);
                if (to == 9) {
                    sessionInGameSeen = true;
                    CopyLine(sessionInGameLine, sizeof sessionInGameLine, msg);
                }
                double wall = 0.0;
                if (!levelStampKnown && ParseLogClockMs(line, &wall)) {
                    levelStampKnown = true;
                    levelWallMs = wall;
                    CopyLine(levelStampLine, sizeof levelStampLine, msg);
                }
            }
        }
        return;
    }
    if (TagIs(tag, tagLen, "entity") && StartsWith(msg, "cs-space candidates: ")) {
        csSpaceSeen = true;
        ++csSpaceCount;
        ReadCsCandidate(msg, "+0x8=", &csSpaceAt8);
        if (strcmp(csSpaceAt8.verdict, "Ready") == 0) {
            csSpaceAt8ReadyEver = true;
            CopyLine(csSpaceAt8ReadyLine, sizeof csSpaceAt8ReadyLine, msg);
        }
        ReadCsCandidate(msg, "+0x2B08=", &csSpaceAt2B08);
        // The key's PRESENCE is recorded apart from what it rendered. FindToken anchors the key on
        // a ` | ` boundary, so `+0x2B08=` cannot answer for it and a pre-ARCH-12 four-column line
        // leaves this false -- which is the whole retroactive control (4.3).
        csSpaceAt2B10KeyPresent = FindToken(msg, "+0x2B10=") != nullptr;
        ReadCsCandidate(msg, "+0x2B10=", &csSpaceAt2B10);
        ReadCsCandidate(msg, "game+0x7AF0=", &csSpaceAt7AF0);
        ReadCsCandidate(msg, "game+0x7AF8=", &csSpaceAt7AF8);
        ReadSym16(msg, "spawnsym=", csSpaceSpawnSym, sizeof csSpaceSpawnSym);
        ReadSym16(msg, "levelsym=", csSpaceLevelSym, sizeof csSpaceLevelSym);
        csSpaceLineTruncated = !CopyLine(csSpaceLine, sizeof csSpaceLine, msg);
        return;
    }
    if (TagIs(tag, tagLen, "entity") && StartsWith(msg, "bound game space ")) {
        boundSeen = true;
        ++boundCount;
        WordAfter(msg, "entity ", boundEntity, sizeof boundEntity, ";");
        CopyLine(boundLine, sizeof boundLine, msg);
        return;
    }
    if (TagIs(tag, tagLen, "playback")) {
        if (StartsWith(msg, "services re-bound: ")) {
            reboundSeen = true;
            WordAfter(msg, "services re-bound: entity ", reboundEntity, sizeof reboundEntity, " ");
            WordAfter(msg, "netvar ", reboundNetvar, sizeof reboundNetvar, " ,");
            CopyLine(reboundLine, sizeof reboundLine, msg);
            return;
        }
        if (StartsWith(msg, "body handles re-bound: ")) {
            bodiesSeen = true;
            UnsignedAfter(msg, "body handles re-bound: ", &bodiesBound);
            const char* at = FindToken(msg, ", ");
            (void)at;
            unsigned absent = 0, notNet = 0, unresolvable = 0;
            if (sscanf_s(msg + strlen("body handles re-bound: "),
                         "%u bound, %u absent this session, %u not net ids, %u unresolvable",
                         &bodiesBound, &absent, &notNet, &unresolvable) == 4) {
                bodiesUnresolvable = unresolvable;
                bodiesAbsent = absent;
                bodiesNotNetIds = notNet;
                bodiesCountersParsed = true;
            }
            const char* open = strrchr(msg, '(');
            if (open != nullptr) {
                size_t k = 0;
                for (const char* p = open + 1; *p != '\0' && *p != ')'; ++p) {
                    if (k + 1 >= sizeof bodiesTrigger) { k = 0; break; }
                    bodiesTrigger[k++] = *p;
                }
                bodiesTrigger[k] = '\0';
            }
            CopyLine(bodiesLine, sizeof bodiesLine, msg);
            return;
        }
        // playback_module.cpp prints THREE lines under this tag whose subject is the INV-OWN1 arm,
        // and two of them are an arm against a real local index: :228 is ArmOwnerGuard()'s ordinary
        // arm at Load, which the product's own comment at :296-297 calls the ordinary case, and
        // :314 is RetryArmOwnerGuard()'s late arm. Matching only the late one renders asked-FAIL on
        // every healthy session and scores Pass only on the degraded-then-recovered one. The third,
        // :234, is an arm against NOTHING; it is this row's asked-fail and not a silence.
        if (StartsWith(msg, "INV-OWN1 armed against this session's own local user index ") ||
            StartsWith(msg, "INV-OWN1 armed against local index ")) {
            invOwn1Armed = true;
            CopyLine(invOwn1Line, sizeof invOwn1Line, msg);
            return;
        }
        if (StartsWith(msg, "INV-OWN1 armed with NO local user index ")) {
            invOwn1LocalUnknownArm = true;
            CopyLine(invOwn1UnknownLine, sizeof invOwn1UnknownLine, msg);
            return;
        }
    }
    if (TagIs(tag, tagLen, "pawn")) {
        if (StartsWith(msg, "census: provisioned ")) {
            unsigned prov = 0, mapped = 0, failed = 0;
            if (sscanf_s(msg, "census: provisioned %u, mapped %u, failed %u", &prov, &mapped,
                         &failed) == 3) {
                pawnCensusSeen = true;
                pawnProvisioned = prov;
                pawnMapped = mapped;
                pawnFailed = failed;
                WordAfter(msg, "state=", pawnStateWord, sizeof pawnStateWord, " ");
                WordAfter(msg, "inmatch=", pawnInMatch, sizeof pawnInMatch, " ");
                CopyLine(pawnCensusLine, sizeof pawnCensusLine, msg);
            }
            return;
        }
        if (StartsWith(msg, "census REFUSED NoEntityService")) {
            pawnRefusedNoEntityService = true;
            CopyLine(pawnCensusLine, sizeof pawnCensusLine, msg);
            return;
        }
        if (StartsWith(msg, "census: map entry ")) {
            ++pawnUnmappedEntries;
            return;
        }
        if (StartsWith(msg, "census totals over ")) {
            unsigned rows = 0;
            if (sscanf_s(msg, "census totals over %u", &rows) == 1) {
                pawnTotalsSeen = true;
                pawnResolvedRows = rows;
                UnsignedAfter(msg, "remote CIs=", &pawnRemoteCis);
                UnsignedAfter(msg, "local CIs=", &pawnLocalCis);
                UnsignedAfter(msg, "in BOTH=", &pawnBothSurfaces);
            }
            return;
        }
        if (StartsWith(msg, "census slot=")) {
            // TWO lines share this prefix. Each is keyed on what it HAS -- the handle row on
            // `handles:`, the slot row on `class=` -- never on the order they arrive in.
            if (FindToken(msg, "handles:") != nullptr) {
                char refusal[48] = {0};
                if (WordAfter(msg, "refusal=", refusal, sizeof refusal, " ") &&
                    refusal[0] != '\0' && strcmp(refusal, "None") != 0) {
                    ++pawnRefusalRows;
                    if (pawnFirstRefusal[0] == '\0') {
                        strncpy_s(pawnFirstRefusal, sizeof pawnFirstRefusal, refusal, _TRUNCATE);
                    }
                }
                return;
            }
            char cls[16] = {0};
            unsigned slot = 0, recorded = 0;
            if (!WordAfter(msg, "class=", cls, sizeof cls, " ") ||
                sscanf_s(msg, "census slot=%u (recorded %u)", &slot, &recorded) != 2) {
                return;
            }
            ++pawnSlotRows;
            if (strcmp(cls, "Local") == 0) {
                ++pawnClassLocal;
            } else if (strcmp(cls, "Remote") == 0) {
                ++pawnClassRemote;
            } else {
                ++pawnClassUnknown;
            }
            // `driveable=` appears once per surface, so each is read from ITS OWN segment of the
            // row rather than by walking every match: WordAfter anchored at `remote:` cannot reach
            // past `local:`, and `undriveable=` answers for neither. EITHER surface reading a body
            // back is a body this session can drive; which surface it was is what `class=` says.
            const char* remoteAt = FindToken(msg, "remote:");
            const char* localAt = FindToken(msg, "local:");
            char driveRemote[8] = {0}, driveLocal[8] = {0};
            const bool driveable =
                (remoteAt != nullptr &&
                 WordAfter(remoteAt, "driveable=", driveRemote, sizeof driveRemote, " ") &&
                 strcmp(driveRemote, "yes") == 0) ||
                (localAt != nullptr &&
                 WordAfter(localAt, "driveable=", driveLocal, sizeof driveLocal, " ") &&
                 strcmp(driveLocal, "yes") == 0);
            if (driveable) {
                ++pawnDriveableRows;
            }
            if (FindToken(msg, "entity=none") == nullptr) {
                ++pawnEntityNamed;
            }
            if (pawnFirstSlotMap[0] == '\0') {
                _snprintf_s(pawnFirstSlotMap, sizeof pawnFirstSlotMap, _TRUNCATE, "%u->%u",
                            recorded, slot);
            }
            CopyLine(pawnSlotLine, sizeof pawnSlotLine, msg);
            return;
        }
        if (StartsWith(msg, "FACT-PP1 UNDETERMINED")) {
            pawnFactUndetermined = true;
            return;
        }
        if (StartsWith(msg, "FACT-PP1 -> ")) {
            pawnFactVerdictSeen = true;
            return;
        }
        return;
    }
    if (TagIs(tag, tagLen, "identity") && StartsWith(msg, "roster: ")) {
        // The dressed line is matched by its WHOLE form, head and tail. `roster: %u/%u players
        // mapped to a live user index` (dresser.cpp:409) is unconditional and printed immediately
        // after this one, so a prefix match reads the mapped count as the dressed count -- and a
        // roster can be 0/2 dressed and 2/2 mapped, which is the exact state this row exists to
        // catch. The `%u NOT dressed` tail is required because sscanf returns on assignment count
        // alone and would otherwise accept any sentence after the third number.
        unsigned dressed = 0, players = 0, notDressed = 0;
        if (sscanf_s(msg, "roster: %u/%u players dressed, %u NOT dressed", &dressed, &players,
                     &notDressed) == 3 &&
            FindToken(msg, "NOT dressed") != nullptr) {
            rosterSeen = true;
            rosterDressed = dressed;
            rosterPlayers = players;
            rosterNotDressed = notDressed;
            CopyLine(rosterLine, sizeof rosterLine, msg);
        } else {
            ++rosterOtherCount;
            CopyLine(rosterOtherLine, sizeof rosterOtherLine, msg);
        }
        return;
    }
    if (TagIs(tag, tagLen, "camera")) {
        // ★ THE COUPLED RENAME, SECOND HALF. W2-E landed 162027c:
        // camera_module.cpp:96,107 now write `rig %p on level space %p` and :125 writes
        // `released (netGame %p, level space %p)`. The old `game space` spelling is text no
        // build of this product can emit any more, so the two matchers below stop accepting it.
        //
        // Dropping a spelling is only safe if the drop is VISIBLE. A pre-162027c log is not
        // absence of evidence, so an unmatched control line is recorded in cameraStaleNounLine
        // and both camera rows answer could-not-ask naming it, rather than reporting that the
        // module never reached a rig probe. Every OTHER field is still taken from its own token
        // (`rig enabled flag `, `conflicts `, `pose-owner: `, and the refusal's `): ` ..
        // `; pose-owner: ` span); the noun is the one thing these matchers key on.
        if (StartsWith(msg, "camera control bound: ")) {
            if (strstr(msg, " on level space ") == nullptr) {
                cameraStaleNounSeen = true;
                CopyLine(cameraStaleNounLine, sizeof cameraStaleNounLine, msg);
                return;
            }
            cameraBoundSeen = true;
            UnsignedAfter(msg, "rig enabled flag ", &cameraRigEnabled);
            UnsignedAfter(msg, "conflicts ", &cameraConflicts);
            WordAfter(msg, "pose-owner: ", cameraPoseOwner, sizeof cameraPoseOwner, ",;");
            CopyLine(cameraLine, sizeof cameraLine, msg);
            return;
        }
        if (StartsWith(msg, "camera control view read refused: ")) {
            cameraViewRefusedSeen = true;
            // Every field below is re-derived from THIS line. A second refusal line that the reader
            // cannot parse used to leave the first session's cause, count and pose-owner in place,
            // and the row went on rendering them as the current answer.
            cameraViewRefusals = 0;
            cameraViewRefusalPoseOwner[0] = '\0';
            TextBetween(msg, "camera control view read refused: ", "; refusals ", cameraViewRefusal,
                        sizeof cameraViewRefusal);
            // The count is read from the delimiter's OWN tail, not from the whole line: a sentence
            // that happened to contain "refusals " would otherwise answer for the module's counter.
            const char* tail = strstr(msg, "; refusals ");
            if (tail != nullptr) {
                UnsignedLongLongAfter(tail, "refusals ", &cameraViewRefusals);
                WordAfter(tail, "pose-owner: ", cameraViewRefusalPoseOwner,
                          sizeof cameraViewRefusalPoseOwner, ",;");
            }
            CopyLine(cameraViewRefusalLine, sizeof cameraViewRefusalLine, msg);
            return;
        }
        if (StartsWith(msg, "camera control refused ")) {
            if (strstr(msg, " on level space ") == nullptr) {
                cameraStaleNounSeen = true;
                CopyLine(cameraStaleNounLine, sizeof cameraStaleNounLine, msg);
                return;
            }
            cameraRefusedSeen = true;
            ++cameraRefusalCount;
            // Re-derived from THIS line, both ends, or cleared: a later refusal this reader cannot
            // parse must not leave the previous session's sentence standing as the current answer.
            cameraRefusalWhy[0] = '\0';
            const char* why = strstr(msg, "): ");
            const char* whyEnd = strstr(msg, "; pose-owner: ");
            if (why != nullptr && whyEnd != nullptr && whyEnd > why + 3) {
                size_t len = static_cast<size_t>(whyEnd - (why + 3));
                if (len >= sizeof cameraRefusalWhy) {
                    len = sizeof cameraRefusalWhy - 1;
                }
                memcpy(cameraRefusalWhy, why + 3, len);
                cameraRefusalWhy[len] = '\0';
            }
            UnsignedAfter(msg, "conflicts ", &cameraConflicts);
            WordAfter(msg, "pose-owner: ", cameraPoseOwner, sizeof cameraPoseOwner, ",;");
            CopyLine(cameraLine, sizeof cameraLine, msg);
            return;
        }
    }
    if (TagIs(tag, tagLen, "capture") && StartsWith(msg, "closed: ")) {
        captureClosedSeen = true;
        // Three producers, and two of them say the opposite of what this arm asked for:
        // capture.cpp:986 "nothing was recording when the %u s capture deadline was ..." and
        // :1147 "capture.enabled is false, so the %u s deadline has no capture to ...". Both are a
        // close, neither is a capture, so neither is this arm's completion point.
        captureClosedEmpty = StartsWith(msg, "closed: nothing was recording") ||
                             StartsWith(msg, "closed: capture.enabled is false");
        CopyLine(captureClosedLine, sizeof captureClosedLine, msg);
        return;
    }

    // Composed fragments: these are printed inside a line their producer assembled from parts, so
    // they are found by punctuated token rather than by prefix, and the whole line is recorded
    // beside the parsed value so a reviewer can see what was read.
    if (const char* netvarsAt = FindToken(msg, "netvars: ")) {
        netvarStateSeen = true;
        const char* p = netvarsAt + strlen("netvars: ");
        size_t k = 0;
        while (*p != '\0' && *p != '|' && k + 1 < sizeof netvarSaid) { netvarSaid[k++] = *p++; }
        while (k > 0 && netvarSaid[k - 1] == ' ') { --k; }
        netvarSaid[k] = '\0';
    }
    if (FindToken(msg, "owners: ") != nullptr) {
        unsigned mapped = 0, players = 0;
        if (PairAfter(msg, "owners: ", &mapped, &players)) {
            ownersSeen = true;
            ownersMapped = mapped;
            ownersPlayers = players;
            ownersRefused = FindToken(msg, "INV-OWN1 REFUSED") != nullptr;
            ownersLocalUnknown = FindToken(msg, "INV-OWN1 local-unknown:") != nullptr;
            CopyLine(ownersLine, sizeof ownersLine, msg);
        }
    }
    if (!audioBindRefusalSeen) {
        if (const char* armAt = FindToken(msg, "audio rate control not armed at bind time")) {
            audioBindRefusalSeen = true;
            CopyLine(audioBindRefusal, sizeof audioBindRefusal, armAt);
        }
    }
    if (FindToken(msg, "CSoundPlayer resolved") != nullptr) {
        soundPlayerResolved = true;
        CopyLine(soundPlayerLine, sizeof soundPlayerLine, msg);
    } else if (FindToken(msg, "CSoundPlayer not resolved") != nullptr) {
        CopyLine(soundPlayerLine, sizeof soundPlayerLine, msg);
    }
    // `loaded ` with a space is [ui]'s line; the playback module's is `loaded:` with a colon, so
    // the two do not collide. The extension is required, so a sentence with the word "loaded" in
    // it cannot become a path this observer then tries to open.
    if (uiLoadedPath[0] == '\0') {
        if (const char* pathAt = FindToken(msg, "loaded ")) {
            const char* p = pathAt + strlen("loaded ");
            const char* end = strstr(p, " (");
            const size_t len = end != nullptr ? (size_t)(end - p) : strlen(p);
            if (len > 0 && len + 1 < sizeof uiLoadedPath) {
                memcpy(uiLoadedPath, p, len);
                uiLoadedPath[len] = '\0';
                if (strstr(uiLoadedPath, ".echodemo") == nullptr) {
                    uiLoadedPath[0] = '\0';
                }
            }
        }
    }
    if (FindToken(msg, "transport rate ") != nullptr) {
        rateLineSeen = true;
        CopyLine(rateLine, sizeof rateLine, msg);
        if (!rateHalfLineSeen && FindToken(msg, "transport rate 0.50x:") != nullptr) {
            rateHalfLineSeen = true;
            CopyLine(rateHalfLine, sizeof rateHalfLine, msg);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// L3-RUN-5 F2. The camera rig binds late -- 1.8 s after the capture loaded on the launch that
// found this -- and the cam-pose battery used to sample camera_control::Get() once, before it,
// and then report "null on this launch" while rows 9 and 19 of the same run read the successful
// bind. The battery waits for it now, and the wait is HERE rather than inline in the probe so the
// suite can execute it: `boundNow` and `sleepMs` are the two seams a test replaces.
//
// The bound is real and is reported. Nothing here retries forever, and a bind that never arrives
// leaves `bound` false with the budget it burned -- which is a could-not-ask the rows can NAME,
// as against an instrument that asked at the wrong moment and called the answer null.
struct CamBindWait {
    bool      bound = false;
    long long waitedMs = 0;
    long long budgetMs = 0;
    // Teardown asked for the wait to end. The battery's two waits are 4000 ms
    // each and Shutdown joins with a bound; a wait that cannot hear the stop makes the join time
    // out and leaves this thread running through the module's teardown.
    bool      stopped = false;
};

template <class BoundNow, class SleepMs, class StopRequested>
inline CamBindWait AwaitCameraBind(BoundNow boundNow, SleepMs sleepMs, long long budgetMs,
                                   long long pollMs, StopRequested stopRequested) {
    CamBindWait w;
    w.budgetMs = budgetMs;
    if (pollMs < 1) { pollMs = 1; }
    for (;;) {
#ifndef WORKER_IGNORES_STOP
        if (stopRequested()) {
            w.stopped = true;
            return w;
        }
#endif
        if (boundNow()) {
            w.bound = true;
            return w;
        }
        if (w.waitedMs >= budgetMs) {
            return w;
        }
        const long long slice = (budgetMs - w.waitedMs) < pollMs ? (budgetMs - w.waitedMs) : pollMs;
        sleepMs(slice);
        w.waitedMs += slice;
    }
}

// A wait with nothing to interrupt it. The stop-aware overload is the one the probe uses.
template <class BoundNow, class SleepMs>
inline CamBindWait AwaitCameraBind(BoundNow boundNow, SleepMs sleepMs, long long budgetMs,
                                   long long pollMs) {
    return AwaitCameraBind(boundNow, sleepMs, budgetMs, pollMs, [] { return false; });
}

// ---------------------------------------------------------------------------------------------
// The rig can go away at either end of the cam-pose arm, and the two ends fail
// DIFFERENTLY: at the read-back camera_control::Get() answers null and we see it ourselves, but at
// the `cam look at` step the loss reaches us as dispatch's own "the spectator rig has not checked
// out" refusal -- a word the composer never issued. A row matching only its composer vocabulary
// scores that ASKED-FAIL and accuses the product of a refusal nobody made. So the battery records
// what the rig WAS at the step, and one loss reads as one named deferral wherever it happened.
enum class RigPresence {
    NotAsked = 0,  // the battery never reached the step
    Present,       // camera_control::Get() answered non-null at both ends of the step
    Released,      // it answered null at one end: the rig went away under the command
};

inline const char* RigPresenceName(RigPresence p) {
    switch (p) {
        case RigPresence::Present:  return "present";
        case RigPresence::Released: return "rig-released";
        default:                    return "not-asked";
    }
}

// Asked at BOTH ends: a release DURING the dispatch is the case that produces the wrong-word
// refusal, and only the after-sample can see it.
inline RigPresence RigAcrossStep(bool presentBefore, bool presentAfter) {
    return (presentBefore && presentAfter) ? RigPresence::Present : RigPresence::Released;
}

// ---------------------------------------------------------------------------------------------
// L3-RUN-28. `bound` above is a fact about the moment the wait ended, not a lease: camera_module
// Releases the singleton on every unbind, and the L3 #28 launch took a `[LEVELLOAD] Loading level`
// in the same second the battery reached the selector read-back. camera_control::Get() answered
// null there, the probe dereferenced it unchecked, and the AV on the probe's own observer thread
// ended four consecutive launches before the pawn clause could be scored. Every PRODUCT caller of
// Get() (dispatch.cpp 724/800/848, playback_module.cpp:710, panel.cpp:893) re-asks and refuses by
// name; this makes the instrument do the same, and the loss becomes a word a row can print.
//
// The read-back is HERE rather than inline in the probe so the suite can execute both arms.
enum class SelectorReadBack {
    NotAttempted = 0,  // the battery never got here (no bind, or an earlier refusal)
    Ok,                // the engine answered and `packed`/`cameraIndex` hold its answer
    RigReleased,       // camera_control::Get() was null AT THE READ -- the rig went away
    NoAnswer,          // the rig is there and ObservedSelector() itself declined
};

inline const char* SelectorReadBackName(SelectorReadBack r) {
    switch (r) {
        case SelectorReadBack::Ok:          return "ok";
        case SelectorReadBack::RigReleased: return "rig-released";
        case SelectorReadBack::NoAnswer:    return "no-answer";
        default:                            return "not-attempted";
    }
}

// `control` is whatever camera_control::Get() answers AT THIS INSTANT -- pass the call, never a
// pointer read earlier -- and it is evaluated ONCE, so the value checked is the value called. Null
// is a result, not a precondition.
//
// The dependency this rests on, named so whoever changes camera_control sees
// it: the check and the call cannot be one instruction, so what closes the remaining window is that
// Get() hands back `&g_control`, an object of STATIC storage duration (camera_control.h:466,
// camera_control.cpp), which Release() only Drop()s -- never frees -- and whose every engine read
// re-guards itself through Readable(). A release landing inside the window therefore yields
// NoAnswer, not a fault. Move g_control to the heap and this line becomes a use-after-free with no
// compiler complaint.
template <class Control>
inline SelectorReadBack ReadObservedSelector(Control* control, int32_t* packed,
                                             uint16_t* cameraIndex) {
#ifndef PROBE_NULL_UNCHECKED
    if (control == nullptr) {
        return SelectorReadBack::RigReleased;
    }
#else
    // MUTANT PROBE_NULL_UNCHECKED: the shipped defect -- the null rig is not distinguished from a
    // rig that answered. (The original dereferenced it; a mutant that access-violates cannot be
    // scored by a test runner, so this models the missing REFUSAL, which is what the rows read.)
    if (control == nullptr) {
        return SelectorReadBack::Ok;
    }
#endif
    return control->ObservedSelector(packed, cameraIndex) ? SelectorReadBack::Ok
                                                          : SelectorReadBack::NoAnswer;
}

// ---------------------------------------------------------------------------------------------
// What the status accessors said. Filled from csregister::Status_(), IWiretap, IAudioRate and
// IReplayTransport in the live module; filled by hand in the test.
// ---------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------
// The published `[inframe ...]` group (ARCH-15 S4.2), parsed off Diagnostic() as a CROSS-CHECK of
// the Report read -- never as a source. The two readings come from the same module, so a
// disagreement means one of them is of something else and neither can be scored.
//
// ★ `draws=` OCCURS TWICE ON THAT LINE, meaning two different counters: `[ticks ... draws=]` is
// ownerDraws (the fallback's attempts) and `[inframe ... draws=]` is inFrameDraws (leg 3). The
// parse therefore keys on the GROUP OPENER and consumes every field of the group in one scan; a
// reader that hunted the bare word would take the wrong counter and be off by whatever the
// fallback did. A group missing any field is REFUSED whole -- a half-read group is not a reading.
// ---------------------------------------------------------------------------------------------
struct InFrameLine {
    unsigned long long entries = 0;
    unsigned long long draws = 0;
    unsigned long long refused = 0;
    unsigned long long noByte = 0;
    unsigned long long noLatch = 0;
    unsigned long long noCtx = 0;
    unsigned long long facade = 0;
    unsigned long long closed = 0;
    unsigned long long fallback = 0;
    bool installed = false;
};

inline bool ParseInFrameLine(const char* diag, InFrameLine& out) {
    if (diag == nullptr) {
        return false;
    }
    // The LAST occurrence: the group is a trailer appended after the sentence, and a sentence that
    // quoted the group must not answer for the module's own trailer (the threads parse's rule).
    const char* at = nullptr;
    for (const char* p = strstr(diag, " [inframe entries="); p != nullptr;
         p = strstr(p + 1, " [inframe entries=")) {
        at = p;
    }
    if (at == nullptr) {
        return false;
    }
    InFrameLine v;
    int installed = -1;
    if (sscanf_s(at,
                 " [inframe entries=%llu draws=%llu refused=%llu nobyte=%llu nolatch=%llu "
                 "noctx=%llu facade=%llu closed=%llu fallback=%llu installed=%d]",
                 &v.entries, &v.draws, &v.refused, &v.noByte, &v.noLatch, &v.noCtx, &v.facade,
                 &v.closed, &v.fallback, &installed) != 10) {
        return false;
    }
    if (installed != 0 && installed != 1) {
        return false;
    }
    v.installed = installed == 1;
    out = v;
    return true;
}

// The published `[stage0 frameopen ...]` group (W2-F C1-R2), parsed off the same Diagnostic() line
// as a CROSS-CHECK of the Report read and never as a source. Same rule as the group above: it keys
// on the group OPENER, takes the LAST occurrence, and consumes all three fields in one scan. The
// two groups sit next to each other on one line and both carry bare decimal counters, so a reader
// that hunted `samples=` or took the first number after `[` would answer with the neighbour's.
struct Stage0Line {
    unsigned long long samples = 0;
    unsigned long long nonzero = 0;
    unsigned long long lastTick = 0;
};

inline bool ParseStage0Line(const char* diag, Stage0Line& out) {
    if (diag == nullptr) {
        return false;
    }
    const char* at = nullptr;
    for (const char* p = strstr(diag, " [stage0 frameopen samples="); p != nullptr;
         p = strstr(p + 1, " [stage0 frameopen samples=")) {
        at = p;
    }
    if (at == nullptr) {
        return false;
    }
    Stage0Line v;
    if (sscanf_s(at, " [stage0 frameopen samples=%llu nonzero=%llu lasttick=%llu]", &v.samples,
                 &v.nonzero, &v.lastTick) != 3) {
        return false;
    }
    out = v;
    return true;
}

// ---------------------------------------------------------------------------------------------
// A REFUSAL IS NOT A LINE. csregister::Diagnostic() can answer with a line that
// carries no groups at all, and it names itself when it does:
//
//   DIAGNOSTIC PROSE DROPPED ...        the five groups ARE complete; the STATUS SENTENCE is gone.
//   DIAGNOSTIC SUFFIX DOES NOT FIT ...  NO group is on the line and no field of one may be read as
//                                       absent-means-zero.
//
// Both are matched BY NAME, at the head of the line, and nothing on such a line is parsed as a
// value. The word travels to the record and to every row that would otherwise have read a field,
// so an operator grades the module's own refusal instead of a zero -- a reader that turned "the
// parse found nothing" into `facade=0 refused=0` would file `nothing was refused` off a line that
// says in words that it published nothing.
//
// Prefix match ONLY: a status sentence that QUOTED one of these phrases is prose, not a refusal.
// ---------------------------------------------------------------------------------------------
enum class DiagRefusal : unsigned char { None = 0, ProseDropped, SuffixDoesNotFit };

inline const char* DiagRefusalWord(DiagRefusal r) {
    switch (r) {
        case DiagRefusal::ProseDropped:     return "DIAGNOSTIC PROSE DROPPED";
        case DiagRefusal::SuffixDoesNotFit: return "DIAGNOSTIC SUFFIX DOES NOT FIT";
        case DiagRefusal::None:             break;
    }
    return "";
}

inline DiagRefusal ClassifyDiagnostic(const char* diag) {
    if (diag == nullptr) {
        return DiagRefusal::None;
    }
    const DiagRefusal kNamed[] = {DiagRefusal::ProseDropped, DiagRefusal::SuffixDoesNotFit};
    for (DiagRefusal r : kNamed) {
        const char* word = DiagRefusalWord(r);
        if (strncmp(diag, word, strlen(word)) == 0) {
            return r;
        }
    }
    return DiagRefusal::None;
}

// The published `[facade nodraw=<n> beginfalse=<n> reason=<word> bound=<n>/<n>]` group
// Parsed with the shape W2-F's own test uses. Same rule as the two groups above:
// keyed on the group OPENER, LAST occurrence, all five fields in one scan or the group is refused
// whole. `%15[a-z]` is the publisher's own width; a token that stops the scan early takes the whole
// group down with it rather than leaving four fields and half a word.
struct FacadeLine {
    unsigned long long nodraw = 0;
    unsigned long long beginfalse = 0;
    char reason[16] = {0};
    unsigned bound = 0;
    unsigned entryCount = 0;
};

inline bool ParseFacadeLine(const char* diag, FacadeLine& out) {
    if (diag == nullptr) {
        return false;
    }
    const char* at = nullptr;
    for (const char* p = strstr(diag, " [facade nodraw="); p != nullptr;
         p = strstr(p + 1, " [facade nodraw=")) {
        at = p;
    }
    if (at == nullptr) {
        return false;
    }
    FacadeLine v;
    if (sscanf_s(at, " [facade nodraw=%llu beginfalse=%llu reason=%15[a-z] bound=%u/%u]", &v.nodraw,
                 &v.beginfalse, v.reason, static_cast<unsigned>(sizeof v.reason), &v.bound,
                 &v.entryCount) != 5) {
        return false;
    }
    out = v;
    return true;
}

// The vocabulary ENG-W2F-G1 S2 publishes, including `unnamed` -- BeginRefusalName's own answer for
// an enumerator nobody has named. A word outside this set is a vocabulary this reader is behind on,
// never a value to score and never quietly read as `none`.
inline bool FacadeReasonKnown(const char* reason) {
    static const char* const kWords[] = {"none", "beginunbound", "noplacement", "boundnoframe",
                                         "unnamed"};
    for (const char* w : kWords) {
        if (strcmp(reason, w) == 0) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// THE TRANSPORT BATTERY (RES-TRANSPORT-CMDS s5 items 1-3).
//
// The owner's verbs -- "go through the timeline, go to different points, fast forward and back,
// pause, play, slowmo, speedup" -- are all commands the product already has, and until now the
// probe drove three of them by calling IReplayTransport directly. That path skips ui::SetRate,
// which is the ONLY place the engine's fast-forward ceiling is enforced, so a probe could report
// success on a rate the panel refuses. Every step below is issued through control::dispatch::Submit
// and collected with TakeResult -- the identical seam the console, the HTTP surface and the panel's
// own launch load (control_module.cpp SubmitLaunchLoad) use.
//
// One slot per step, not per command name: `pause` is issued twice and the two are different
// questions, so they are different slots. Every field is filled by the driver; a row that finds
// `attempted == false` says the step was never issued rather than reading its zeros.
// ---------------------------------------------------------------------------------------------
enum class Verb : unsigned char {
    Pause = 0,
    SpeedHalf,
    SpeedUnderCeiling,
    SpeedOverCeiling,
    PauseAgain,
    Seek,
    Mark,
    StepForward,
    StepBack,
    Round,
    SeekAway,
    Goto,
    Reverse,
    Play,
    // P17-B T11-T18. Appended, never interleaved: steps[] is indexed by this enum and a new
    // value in the middle would silently re-aim every record already on disk.
    GotoNextNoMarkers,
    SeekLate,
    MarkSecond,
    SeekZero,
    GotoNextFirst,
    GotoNextSecond,
    GotoNextAtEnd,
    GotoPrev,
    SeekRelForward,
    SeekRelBack,
    SeekRelClampStart,
    // P10-F. The cam-pose asks (ARCH-19 R3/R6, ENG-W2G-P17E S7). Appended for the reason the
    // P17-B block above was: steps[] is indexed by this enum.
    CamLookRelative,
    CamLookAbsolute,
    CamLookAtSubject,
    CamRelease,
    // L3-RUN-4 s6. `p10-seek-landing` had ONE point -- the relative nudge -- so the row had
    // no denominator worth the name. Three ABSOLUTE seeks scored by the same predicate.
    // Appended, never inserted: steps[] is indexed by this enum.
    SeekAbs25,
    SeekAbs50,
    SeekAbs90,
    // L3-RUN-9. The ceiling rate ITSELF (2.00x on this machine). The battery issued 0.5,
    // <under> and <over> and never the ceiling; ENG-CRASHFIX-L3-8 S6.2 requires rate 2.0 issued.
    // Appended, never inserted: steps[] is indexed by this enum.
    SpeedAtCeiling,
    Count
};

inline const char* VerbName(Verb v) {
    switch (v) {
        case Verb::Pause:             return "pause";
        case Verb::SpeedHalf:         return "speed 0.5";
        case Verb::SpeedUnderCeiling: return "speed <under the ceiling>";
        case Verb::SpeedOverCeiling:  return "speed <over the ceiling>";
        case Verb::SpeedAtCeiling:    return "speed <at the ceiling>";
        case Verb::PauseAgain:        return "pause (before the jumps)";
        case Verb::Seek:              return "seek <us>";
        case Verb::Mark:              return "mark";
        case Verb::StepForward:       return "step +1";
        case Verb::StepBack:          return "step -1";
        case Verb::Round:             return "round <n>";
        case Verb::SeekAway:          return "seek <away>";
        case Verb::Goto:              return "goto <n>";
        case Verb::Reverse:           return "speed -1";
        case Verb::Play:              return "play";
        case Verb::GotoNextNoMarkers: return "goto next (no bookmarks yet)";
        case Verb::SeekLate:          return "seek <late>";
        case Verb::MarkSecond:        return "mark (second)";
        case Verb::SeekZero:          return "seek 0";
        case Verb::GotoNextFirst:     return "goto next (first)";
        case Verb::GotoNextSecond:    return "goto next (second)";
        case Verb::GotoNextAtEnd:     return "goto next (at the last bookmark)";
        case Verb::GotoPrev:          return "goto prev";
        case Verb::SeekRelForward:    return "seek +<delta>";
        case Verb::SeekRelBack:       return "seek -<delta>";
        case Verb::SeekRelClampStart: return "seek -<past the start>";
        case Verb::CamLookRelative:   return "cam look 10 0";
        case Verb::CamLookAbsolute:   return "cam look to 30 0";
        case Verb::CamLookAtSubject:  return "cam look at 1";
        case Verb::CamRelease:        return "cam release";
        case Verb::SeekAbs25:         return "seek <25% of duration>";
        case Verb::SeekAbs50:         return "seek <50% of duration>";
        case Verb::SeekAbs90:         return "seek <90% of duration>";
        case Verb::Count:             break;
    }
    return "?";
}

struct TransportStep {
    bool attempted = false;      // the driver reached this step at all
    bool submitted = false;      // dispatch::Submit was called -- the GUI's own seam
    bool answered = false;       // a dispatch::Result was collected for it
    bool ok = false;             // Result::ok
    double waitMs = 0.0;
    double askedRate = 0.0;      // Speed: the rate asked for, so a read-back can be compared
    long long target = 0;        // Seek/Round/Goto: the microsecond the step asked to land on
    char asked[72] = {0};        // the step as a human would have typed it
    char said[208] = {0};        // Result::text, verbatim and truncated

    // Read back from dispatch::ReadSnapshot AFTER the dispatcher published the step's tick. This
    // is the transport as the panel and the console see it, not a second reading of our own.
    bool readBack = false;
    int state = -1;              // TransportState, or -1 when there is no transport
    float rate = 0.0f;
    long long playheadBefore = 0;
    long long playheadAfter = 0;

    // The residue battery's effect counters around this step. ResidueStats publishes no fire
    // count, so what is carried is the SUM of its effect counters before and after -- see
    // AskResidueFiresOnDiscontinuity for what that can and cannot answer.
    bool residueArmed = false;
    unsigned long long residueBefore = 0;
    unsigned long long residueAfter = 0;

    // ENG-W2R-ROWS3B (L3-RUN-6 F5). The SUM above cannot say WHICH effect moved: 30 reads the
    // same whether the battery reset the CI thirty times or woke thirty bodies once each.
    // `ciResets` is carried by name beside it so the row's sentence can name it. ResidueStats is
    // W3-A's surface (`w3a-seek-effect-counts`); this reads it and publishes nothing to it.
    unsigned long long residueCiResetsBefore = 0;
    unsigned long long residueCiResetsAfter = 0;

    // W3-A ENG-W3A-FIRES. `fires` counts RAISES, so a jump that fired with nothing bound is
    // no longer indistinguishable from one that never fired -- which is the whole of L3-RUN-2
    // (F6). `causeAfter` is residue::Cause as an int so this header still pulls in no product
    // code; -1 is "the stats were never read", and Cause::Count is the producer's own "never
    // fired", so the two are not the same value.
    unsigned long long residueFiresBefore = 0;
    unsigned long long residueFiresAfter = 0;
    unsigned long long residueSeekFiresBefore = 0;
    unsigned long long residueSeekFiresAfter = 0;
    int residueCauseAfter = -1;
    int residueCauseCount = -1;        // residue::Cause::Count, the producer's "never fired"

    // ENG-W2B-STEP2 `d77cd0b`. IReplayTransport::LastStepTicks() read straight after this step.
    // The playhead cannot be DIFFERENCED for this number: a recorded tick table is non-decreasing,
    // so a stalled source records the same boundary twice and a step that genuinely consumed a tick
    // can leave the playhead on the microsecond it started from. `stepTicksRead` false means the
    // probe never asked; `lastStepTicks == StatusFacts::stepTicksUncounted` means the transport
    // counts nothing, which is neither a count nor a refusal.
    bool stepTicksRead = false;
    int  lastStepTicks = 0;
};

struct StatusFacts {
    // How long the observer has been looking. A could-not-ask that says nothing was seen has to say
    // over what window, or it is a shrug with a timestamp missing.
    double observedMs = 0.0;

    // L2-RUN-2 F5. The entity surface as it stands AT SCORING TIME, not as the last [entity] line
    // left it. L2 #2 filed asked-fail off a 07:10:23.146 refusal that a 07:10:24.318 line had
    // already superseded: a log tail is a HISTORY, and a row whose subject is a STATE has to sample
    // the state. `entitySampled` is false when services or its IEntity* was absent at the sample --
    // which is not the same as an entity that answered and said no.
    bool entitySampled = false;
    bool entityAvailable = false;
    char entityDiagnostic[192] = {0};

    // L2-RUN-2 F4. W1-F's loopback-only rewrite was ARMED on both L2 runs and MEASURED on neither:
    // its counters reached no artifact and no row, so "armed" was all either run could say.
    // `inboundVerdict` is the word bootstrap::inbound's OWN Classify produced over its OWN
    // SelfView, both compiled into this binary -- there is no second copy of the rule here to drift
    // from the product's. The counters ride along so the detail can SHOW the anti-vacuity pair
    // (inbound.cpp:189) rather than assert it.
    bool inboundSampled = false;
    char inboundVerdict[24] = {0};   // never-asked | could-not-ask | asked-pass | asked-fail
    bool inboundArmed = false;
    bool inboundEndpointsRead = false;
    // The PRODUCT's own refusal token for "a step that reaches no new tick",
    // copied here from src/control/command.h's kStepNoTickThatWay by the observer, which includes
    // that header. This header includes no product header, and a row carrying its own spelling of a
    // product constant is a checker with its own copy of the format.
    char stepNoTickToken[40] = {0};
    // P10-F. Three more of the product's OWN words, copied in by the probe for the same reason
    // the one above is: seekgoto.h's off-target refusal and camera_control.h's two cam-pose
    // refusals. A row spelling one of these itself would be a checker with its own copy of the
    // product's vocabulary, and would go on passing after the product renamed the word.
    char seekLandingOffToken[40] = {0};
    char facingDegenerateToken[40] = {0};
    char lookAtUnresolvableToken[40] = {0};
    // ENG-W1E-SUBJECT `eb31bd4` INSTALLED the subject-pose composer, so `cam look at <n>`
    // now resolves against a live roster and `look-at-unresolvable` stops being p10-cam-9's
    // pass. These are camera_control::SubjectResolveName's own spellings, published by the
    // probe, so the row scores the product's words and never a copy of them.
    char subjNoSuchSubjectToken[40] = {0};
    char subjRosterUnreadableToken[40] = {0};
    char subjNoBodyToken[40] = {0};
    // These two do NOT fit forty bytes. `no subject-pose seam in this image` is
    // 34 and `the selector derivation is not certified against this engine yet (select that
    // player once with cam pov)` is 103, and a _TRUNCATE copy of the second into a char[40]
    // publishes a field that looks filled and can never match a word the product said. The
    // buffers are sized from the spellings; PublishToken refuses rather than truncating, and the
    // two counters below are what it refused, so a shrunk buffer is reported and not scored.
    char subjNoSeamToken[64] = {0};
    char subjDerivationUncertifiedToken[128] = {0};
    unsigned subjTokenDropped = 0;      // SubjectResolveName spellings that did not fit
    unsigned subjTokenLongestLen = 0;   // the longest spelling the probe was handed, in bytes
    // The selector `cam look at <n>` actually carried, so a refusal naming an empty slot can be
    // crossed against the roster's occupancy for THAT slot rather than against an assumed 1.
    int camLookAtSelector = 0;
    bool subjectPoseInstalledKnown = false;  // the probe asked; absent is not false
    bool subjectPoseInstalled = false;       // camera_control::InstalledSubjectPose() != null

    // p10-cam-9 half A -- the packing. Pure arithmetic on the product's
    // own PackSelector/PackedSelectorWord, so it is scoreable in a solo session with no
    // roster; half B is the aim and needs a subject.
    bool camPackKnown = false;          // the probe put 1..10 to PackSelector
    bool camPackAllPacked = false;      // PackSelector(1..10) all returned true
    bool camPackDistinct = false;       // the ten PackedSelectorWord values are all different
    bool camPackRefusesZero = false;    // PackSelector(0) == false
    bool camPackRefusesEleven = false;  // PackSelector(kPlayerSelectorMax + 1) == false
    // Half B's oracle: the engine's own ci+0x26 word read back for the selector the battery
    // put to the rig, beside what our split says it should be.
    bool camSelectorObserved = false;
    // Why the read-back did or did not happen. `camSelectorObserved` alone cannot separate "the
    // engine declined" from "the rig was Released under us" (L3-RUN-28's AV), and those are two
    // different could-not-asks. The enum is what the row scores; SelectorReadBackName is for its
    // sentence only.
    SelectorReadBack camSelectorReadBack = SelectorReadBack::NotAttempted;
    // What the rig was across the `cam look at` dispatch itself. A release
    // there arrives as dispatch's own refusal word, not as a null we can see, so the row needs the
    // observation rather than the sentence.
    RigPresence camRigAtLookAt = RigPresence::NotAsked;
    int  camSelectorPacked = 0;
    int  camSelectorExpected = 0;
    bool camSelectorCertified = false;   // SelectorDerivationState(n) == Certified
    bool camSelectorFalsified = false;   // == Falsified, and that verdict is sticky
    // The name is for the sentence ONLY. It is never matched on: `uncertified` contains
    // `certified`, so a row reading the word would read the refusal as the pass.
    char camSelectorDerivation[24] = {0};
    // control/seekgoto.h kSeekLandingToleranceUs -- the window the PRODUCT itself calls a
    // nearest-tick landing. 0 means the probe never filled it and no near miss can be graded.
    long long seekLandingToleranceUs = 0;
    // IReplayTransport::TickSpacingUs -- the CAPTURE's sample interval, measured by the transport
    // that loaded it (ENG-TOLERANCE `3bb7eac`). One microsecond count read once and assigned to
    // both consumers at a single site in RunBattery, so the tolerance a landing is scored against
    // and the distance a step is scored against can never be two disagreeing numbers. 0 is UNKNOWN
    // and every row that needs it says could-not-ask rather than inventing a cadence.
    long long tickSpacingUs = 0;
    // src/playback/core/transport.h kStepTicksUncounted, copied in by the probe for the reason
    // stepNoTickToken is: this header spells no product constant of its own. It is NOT 0 -- 0 is a
    // COUNTED refusal (a step that ran into an end of the table), so a row reading an uncounting
    // double as 0 would grade every step through it a refusal.
    bool stepTicksUncountedKnown = false;
    int  stepTicksUncounted = 0;
    // ENG-W2B-STEP2's boundary clause. A step lands on At(i) of the CAPTURE's tick table, and
    // IReplayTransport publishes no accessor for that table -- so this is a channel the fixture
    // fills and the live probe today cannot. False is a NAMED could-not-ask for that ONE clause;
    // the direction, magnitude and LastStepTicks clauses are scored either way, and the row never
    // reads an unfilled boundary as agreement.
    bool stepBoundaryKnown = false;
    long long stepBoundaryForwardUs = 0;   // the tick boundary the forward step should have landed on
    long long stepBoundaryBackUs = 0;      // and the one the back step should have

    // ---- ARCH-21 S4.4, read back out of the capture-stop sentinel this launch wrote -------------
    // The TRIPLE, and it is a triple on purpose: `markerSource` alone cannot separate "the sampler
    // works and no round started" from "there is no marker source at all". `localBoundaries` is the
    // count the local source actually emitted, and `corroboratorAsked`/`wireRowsSeen` say whether
    // the wire path was ever FED -- so a zero corroboration reads as COULD NOT ASK rather than as
    // agreement. On the owner's solo host the honest answer is
    // local / 0 boundaries / corroboratorAsked false, and that is three states, not two.
    bool sentinelRead = false;
    char sentinelReadReason[192] = {0};
    // The close's own roster row count, off the same LastStopFacts snapshot
    // AskRoundTableAtClose reads (`f1f475e`). p10-cam-9 crosses `no player occupies that
    // selector` against it: a roster that recorded no player makes an empty slot this session's
    // shape rather than a composer that lost somebody.
    unsigned long sentinelRosterRows = 0;
    bool sentinelRosterRowsKnown = false;
    bool sentinelMarkerSourceKnown = false;
    char sentinelMarkerSource[32] = {0};      // the sentinel's own word, never re-spelled here
    bool sentinelLocalBoundariesKnown = false;
    unsigned long sentinelLocalBoundaries = 0;
    bool sentinelCorroboratorAskedKnown = false;
    bool sentinelCorroboratorAsked = false;
    bool sentinelWireRowsSeenKnown = false;
    unsigned long sentinelWireRowsSeen = 0;
    // W2-A's round table as the close itself recorded it. `Known` is the snapshot's own
    // `roundRowsRecorded`: FALSE means the close never reached the table, which is a different
    // fact from reaching it and finding zero rows. Same three states for the round counter.
    bool sentinelRoundRowCountKnown = false;
    unsigned long sentinelRoundRowCount = 0;
    bool sentinelRoundCounterKnown = false;
    unsigned long sentinelRoundCounter = 0;
    // The capture's own phase word at the samples this launch took, so a `local` source with zero
    // boundaries can say WHY there were none. Empty is unknown and is never read as pre_match.
    char sentinelPhaseWord[32] = {0};

    // P10-F, ENG-W2G-P17E S7. The cam-pose battery's OWN pose reads. ARCH-19 E3 forbids inferring
    // a landing from the Result bool, so the yaw these rows score is DecomposeFacing's over a
    // SampleViewPose this probe took, never the reply sentence's own arithmetic.
    bool  camPoseAsked = false;             // camera_control::Get() was bound and the battery ran
    // L3-RUN-5 F2. The rig binds ~1.8 s after the file loads, and the cam arm used to sample
    // camera_control::Get() once, early, and file "null on this launch" against a session whose
    // own log printed `[camera] camera control bound`. The battery now WAITS for the bind, and
    // these three publish that wait so a could-not-ask names a bound that expired instead of a
    // seam that was merely asked too soon. camBindWaitKnown false means the probe predates the
    // wait -- a build fact, not a camera fact, and the rows say so separately.
    bool      camBindWaitKnown = false;
    bool      camBindBound = false;   // Get() answered non-null before the budget ran out
    long long camBindWaitedMs = 0;    // how long this probe actually waited for it
    long long camBindBudgetMs = 0;    // the bound it was given
    bool  camFacingReadBefore = false;      // SampleViewPose answered Ok before the relative nudge
    bool  camFacingVerticalBefore = false;  // ... and DecomposeFacing refused it: it IS vertical
    bool  camFacingReadAfter = false;       // SampleViewPose answered Ok after `cam look to 30 0`
    bool  camFacingVerticalAfter = false;
    float camYawBefore = 0.0f;
    float camYawAfter = 0.0f;
    float camPitchAfter = 0.0f;
    double camLookYawAsked = 0.0;
    double camLookPitchAsked = 0.0;
    unsigned long long inboundRewritten = 0;
    unsigned long long inboundAlreadyLoopback = 0;
    unsigned inboundBoundNonLoopback = 0;

    // W2-F, src/ui/csregister.h Report. The five named fields plus registrationInvocations.
    bool csAsked = false;                 // false only if the module never ticked
    bool csRunning = false;               // Status::Running
    char csStatus[32] = {0};
    uint64_t csInvocations = 0;
    uint64_t csRegistrationInvocations = 0;
    uint64_t csStaleInvocations = 0;
    uint64_t csSpaceChanges = 0;
    uint32_t csRegistrations = 0;
    uint64_t csDeferrals = 0;
    // DESIGN S8.5.4a (MARSHAL). `Running` is three separately observable parts --
    // registrations >= 1 AND invocations > 0 AND marshalledDraws > 0 -- because a two-state gate
    // cannot carry `registered but never called` and `called but never drawn`. All three are
    // carried here so the row scores the parts rather than the word.
    uint64_t csMarshalledDraws = 0;
    uint64_t csCoalescedInvocations = 0;
    uint64_t csPendingInvocations = 0;
    uint64_t csStaleDetected = 0;        // CLEAN(L5) clause 4: absent is NOT zero, so it is read
    uint64_t csSpacesSeen = 0;           // The epoch clause's right-hand side
    uint32_t csOwnerThreadId = 0;        // Report::mainThreadId -- OUR TICK's thread
    uint32_t csCallbackThreadId = 0;     // Report::lastCallbackThreadId
    // The same two ids as W2-F PUBLISHES them, parsed off the frozen ` [threads owner=<n>
    // callback=<n>]` suffix csregister::Diagnostic() carries on every line without exception. Read
    // as a CROSS-CHECK, never as the source: one fact with two holders needs a count, and a
    // disagreement between the struct and the sentence is a could-not-ask rather than a verdict.
    bool csThreadsPublished = false;
    uint32_t csOwnerThreadIdPublished = 0;
    uint32_t csCallbackThreadIdPublished = 0;

    // The band-3 draw counter sampled at the moment the level was FIRST seen, so "drawn AFTER the
    // level" is a difference over a monotone counter and needs no second clock. Same for the
    // invocation count, which is what separates a window that offered no drawable frame (CENSORED,
    // DESIGN S8.5.4a) from one that offered thousands and drew none.
    bool csDrawBaselineKnown = false;
    uint64_t csDrawsAtLevel = 0;
    uint64_t csInvocationsAtLevel = 0;

    // ARCH-13 ruling 2 (S2.3 / S1.3): OWNER TICKS -- the times our stage-0 tick ran on the owner
    // thread -- is the denominator the draw counter is scored against, and the ONLY censoring
    // predicate. `csOwnerTicksKnown` is false when the record the probe read carries no such field
    // at all, which is what every recorded launch up to and including L5 re-run 3 looks like: that
    // is could-not-ask NAMING the field, never a coverage figure computed from a zero.
    bool csOwnerTicksKnown = false;
    uint64_t csOwnerTicks = 0;
    uint64_t csOwnerTicksAtLevel = 0;

    // ARCH-15 S5.0's FROZEN counter block, as W2-F-C1 publishes it. ARCH-15 S4.1 STRUCK
    // `ownerDraws` and `marshalledDraws` from every gate term: both count draw ATTEMPTS at the
    // stage-0 drain, and RES-W1M-C0 shows the drain runs strictly before the only site that opens
    // a GUI frame, so every one of those attempts is refused at `NRadGui::Begin`. A counter that
    // reads 1:1 with ownerTicks on a build that draws nothing is a green light on the exact
    // failure the row exists to catch. The row-2.5 counters below are the replacement, and
    // `csInFrameKnown` is false on every record written before ARCH-15-C1 landed -- which is
    // could-not-ask NAMING the fields, never a coverage figure computed from an absent one.
    bool csInFrameKnown = false;
    bool csInFrameInstalled = false;        // the row-2.5 detour is armed
    char csInFrameStatus[180] = {0};         // why, when it is not
    uint64_t csFrameSiteEntries = 0;        // detour entries -- the coverage DENOMINATOR
    uint64_t csInFrameDraws = 0;            // ...that drew into the open frame -- the NUMERATOR
    uint64_t csRefusedDraws = 0;            // ...that were refused, BY NAME
    uint64_t csPanelClosedEntries = 0;      // ...at which nobody asked for a draw
    uint64_t csFallbackEntries = 0;         // owner-tick attempts; 0 while the detour is armed
    uint64_t csRefusedNoFrameByte = 0;      // the breakdown, so the reason is read not guessed
    uint64_t csRefusedNoLatch = 0;
    uint64_t csRefusedNoContext = 0;
    uint64_t csRefusedByFacade = 0;

    // The same four counters at the window's baseline. AUDIT_F3J F3J-2: the window is the
    // INTERSECTION of the level-load stamp and the panel's closed->open stamp, so the baseline is
    // taken at the LATER of the two and `csInFrameBaselineStamp` says which one it was. All four
    // are sampled at the SAME moment: ARCH-15 S4.3 forbids scoring a field stamped at one epoch
    // against a field stamped at another, because that is two different windows subtracted.
    bool csInFrameBaselineKnown = false;
    char csInFrameBaselineStamp[260] = {0};  // "level" or "enable"
    uint64_t csFrameSiteEntriesAtBaseline = 0;
    uint64_t csInFrameDrawsAtBaseline = 0;
    uint64_t csRefusedDrawsAtBaseline = 0;
    uint64_t csPanelClosedEntriesAtBaseline = 0;

    // Report::enableStamps -- closed->open transitions. Zero means the panel never opened, which
    // is a window that was never offered rather than a window in which nothing drew (F3J-2).
    bool csEnableStampsKnown = false;
    uint64_t csEnableStamps = 0;

    // ARCH-15 S5.3 / S7 and RES-W1M-C0 S6: the GUI.FRAME_OPEN_BYTE sample the plugin publishes at
    // the TOP of the stage-0 drain, once per owner tick. The prediction filed in advance, so the
    // row can fail: zero on every tick. A non-zero sample falsifies the research this whole
    // packet is built on, so it is an asked-FAIL naming the tick it happened on.
    bool csFrameOpenSampled = false;
    uint64_t csOwnerTicksSampled = 0;       // ticks on which the sample was actually taken
    uint64_t csFrameOpenAtOwnerTick = 0;    // ...on which the byte was NON-ZERO
    // The module publishes the LAST non-zero tick, not the first (csregister.h
    // frameOpenAtOwnerTickLastNonZeroTick). Named for what it holds: a field called
    // `first` fed from a `last` is a wrong answer nobody can see.
    uint64_t csFrameOpenLastNonZeroTick = 0;
    // Whether the three presence bits below were ASKED. csregister::Report does not carry
    // them, so they are false-because-unpublished, and a row that read that as "there was no
    // GUI" would censor itself on its own missing field forever.
    bool csGuiPresenceKnown = false;
    // AUD-C1R2 MAJOR-1. Site entries on a thread that is NOT the owner's: no draw is
    // attempted on them and they are not in frameSiteEntries. A guard counter, reported in
    // the frame-open row's detail and deciding nothing on its own.
    uint64_t csOffThreadSiteEntries = 0;
    bool csGuiContextPresent = false;       // GUI.CONTEXT_PTR was non-null
    bool csWindowGatePresent = false;       // GUI.WINDOW_GATE resolved
    bool csStartFrameLatchPresent = false;  // GUI.STARTFRAME_LATCH resolved

    // The cross-check of the Report read against the module's own published `[inframe ...]` group.
    // `csInFramePublished` false means the line carried no group to compare with, which is not a
    // disagreement; only published-and-disagreeing censors the row.
    bool csInFramePublished = false;
    bool csInFrameLineAgrees = false;
    // The same pair for the `[stage0 frameopen ...]` group.
    bool csStage0Published = false;
    bool csStage0LineAgrees = false;

    // The two halves of `refusedByFacade`, read TWICE: off csregister::Report
    // (the struct read) and off the module's own published `[facade ...]` group. The product gives
    // `refusedByFacade` no storage any more -- inframe::Read() computes it as nodraw+beginfalse --
    // so a reading on which the two do not sum to it means one of the three is of something else,
    // which is an instrument fault and not a finding about the panel.
    uint64_t csRefusedNoDraw = 0;         // the frame was open and no draw fn was set: OUR wiring
    uint64_t csRefusedBeginFalse = 0;     // the panel ran and its gui::Begin returned false
    char csBeginRefusal[16] = {0};        // inframe::BeginRefusalName(Report::beginRefusal)
    uint32_t csFacadeBoundEntries = 0;    // gui::BoundCount() as the panel saw it at that refusal
    uint32_t csFacadeEntryCount = 0;      // gui::Entry::Count, so no second copy of it lives here
    // The same five as the module PUBLISHES them, and whether the group was on the line at all.
    // Absent is absent: `csFacadePublished == false` is never read as five zeros.
    bool csFacadePublished = false;
    uint64_t csFacadeNoDrawPublished = 0;
    uint64_t csFacadeBeginFalsePublished = 0;
    char csFacadeReasonPublished[16] = {0};
    uint32_t csFacadeBoundPublished = 0;
    uint32_t csFacadeEntryCountPublished = 0;

    // csregister::Diagnostic()'s own refusal word (DiagRefusalWord), matched by name at the head of
    // the line and carried verbatim. Empty means the line was a line.
    char csDiagRefusal[40] = {0};

    // UI STATE, carried so a reader can see it and scored by NOTHING. L5 run 3 filed
    // `PanelVisible()=1` and nothing had drawn the panel: state is not a draw.
    bool panelEnabled = false;
    bool panelVisible = false;

    // W2-E camera_control::Probe, called from the tick with TickInfo::netGame. The header says of
    // it: "No engine call and no write: safe on every tick", so asking costs the run nothing.
    bool cameraProbed = false;
    bool cameraProbeReadyEver = false;
    char cameraProbeResult[24] = {0};
    char cameraProbeDiagnostic[200] = {0};

    // W1-L / W1-H. A two-state `available` cannot express could-not-ask, which is the whole point
    // of this row: the tap passes when it is available, AND when it is unavailable with a
    // diagnostic that names its subject. A bare available=0 is a FAIL.
    bool tapPresent = false;              // services->wiretap was non-null
    bool tapAvailable = false;
    char tapDiagnostic[256] = {0};

    // W2-A, src/capture/manifestbuild.h TapRegistrationNow(). Available() answers
    // whether the tap COULD work; these answer whether anything is registered on it, which is what
    // row 8 passed on with 0 of 36 taps live. `tapRegStarted` is the could-not-ask half: the tap is
    // built on the first tick carrying a CR15NetGame, and before that every count reads "not asked
    // yet" -- a launch that never entered a match must not be failed for it.
    bool tapRegStarted = false;
    uint32_t tapRegistered = 0, tapDeferred = 0, tapGivenUp = 0, tapUnavailable = 0, tapTotal = 0;

    // W2-H. The slope of GetBufferTick() (IAudioRate::AudioSeconds) against the probe's own QPC,
    // measured across a window while the transport is commanded to 0.5x.
    bool audioPresent = false;
    bool audioAvailable = false;
    char audioDiagnostic[256] = {0};
    // ---- W2-A's two streams, read back on the playback arm (L3-RUN-7 F3) ----------------------
    // Filled by probe_replay.cpp, which links the PRODUCT's replay::DemoReader and walks the
    // streams with it. Nothing in this header knows the format: even the width of the gamestate
    // present mask arrives as `gsPresentAll`, which the probe copies from kGameStatePresentAll,
    // so a checker with its own idea of the format cannot certify a file the loader would refuse.
    bool demoScanned = false;
    char demoScanRefusal[220] = {0};
    char demoPath[kRowLineCap] = {0};
    unsigned gsRecords = 0;
    unsigned gsPresentUnion = 0;
    unsigned gsPresentAlways = 0;
    unsigned gsPresentAll = 0;
    unsigned gsUndefinedBits = 0;
    unsigned gsPhasesSeen = 0;
    unsigned gsTimerReversals = 0;
    unsigned psRecords = 0;
    unsigned psUsers = 0;
    unsigned psFillUnion = 0;
    unsigned psFillAll = 0;
    unsigned psTimeBacksteps = 0;
    // ---- W2-P3: the body decode, read from the module's OWN published apply stats -------------
    // src/playback/core/playback.h:211 `ApplyStats ApplyStatsSnapshot()` -- "the last tick's apply,
    // so a wiring test can read what was published rather than infer it from a pose sink's call
    // count". src/playback/core/apply.cpp:94-147 gives every body track in the window exactly one
    // of the five dispositions below per call, so their sum IS the track count the applier walked,
    // and `bodyInstalled` IS the driven-pose write count: it increments only after SampleBodyAt
    // found a decoded sample at t AND SetHistoryWindow returned true.
    //
    // SAMPLES PER TRACK has no published source on this build. BuildStats::body_samples
    // (windowbuild.h:73) is computed on every build and kept in playback_module.cpp's `g.build`,
    // and playback.h publishes no accessor for it; the ask is in ENG-W2-P3.md. The row therefore
    // reports the two numbers it can read and NAMES the third as unpublished rather than printing
    // a zero for a question nobody was asked.
    bool bodyApplySampled = false;
    unsigned bodyApplyPolls = 0;
    unsigned bodyInstalled = 0;
    unsigned bodyAbsent = 0;
    unsigned bodyUnhandled = 0;
    unsigned bodyLocallyControlled = 0;
    unsigned bodyInstallFailed = 0;
    unsigned bodyOwnerClaimed = 0;
    // The write and the READ-BACK are two events, not one. apply.cpp:143-176 counts an install
    // that returned true, then walks the SAME element again and compares field by field; only
    // `bodies_read_back` says the engine's ring HOLDS the pose. `bodyIdentityRefused` is the named
    // subset of `bodyInstallFailed` the install's own identity gate declined (apply.h:59-69).
    unsigned bodyReadBack = 0;
    unsigned bodyInstalledUnread = 0;
    unsigned bodyIdentityRefused = 0;
    char audioRateLabel[64] = {0};
    bool slopeMeasured = false;
    double slope = 0.0;
    double slopeWindowMs = 0.0;

    // W2-E 4b. The transport was paused and the view was watched across a WALL-time span, through
    // replay::camera_control::SampleViewPose / PoseMotionBetween (src/camera/camera_control.h:318,
    // :334). Everything the library returned arrives here as a plain word or number, so this header
    // still pulls in no product code and test_observer.cpp can drive every outcome by hand.
    bool pausedForView = false;
    bool viewPoseSampled = false;         // two samples were taken, whatever they read
    bool viewPausedAcross = false;        // TransportState::Pause at BOTH samples
    bool viewSteeringDeclared = false;    // the launch marker says a human is at the controls
    char viewReadFirst[16] = {0};         // ViewPoseRead: Ok / NotBound / Unreadable
    char viewReadSecond[16] = {0};
    char viewMotion[16] = {0};            // PoseMotion: Moved / Still / CouldNotAsk
    char viewSource[16] = {0};            // ViewPoseSource: ViewState / CameraInstance
    // The VERDICT keys on these, the producers' own enums; the char buffers above carry the
    // producers' NAME for the detail text and decide nothing. A fixture that hand-writes a word
    // into a buffer cannot move this row -- test_observer.cpp drives exactly that control.
    replay::camera::ViewPoseRead viewReadFirstCode = replay::camera::ViewPoseRead::NotBound;
    replay::camera::ViewPoseRead viewReadSecondCode = replay::camera::ViewPoseRead::NotBound;
    replay::ViewPoseSource viewSourceCode = replay::ViewPoseSource::CameraInstance;
    replay::camera_control::PoseMotion viewMotionCode =
        replay::camera_control::PoseMotion::CouldNotAsk;
    bool viewGateKnown = false;           // viewstate[0]+0x9C was reachable
    int  viewGateArmed = 0;
    double viewMetres = 0.0;
    double viewSeconds = 0.0;

    // W2-B/W2-E preconditions: a battery that never had a loaded file did not fail, it never ran.
    bool transportPresent = false;
    uint64_t durationUs = 0;

    // W2-R P10. The battery's own record. `batteryDispatched` is the could-not-ask half of every
    // row below: a launch whose transport never held a capture ran no step at all, and the rows
    // must say that rather than scoring fourteen sets of zeros.
    // Did Shutdown actually JOIN the observer worker, or did its bound expire
    // with the thread still running? A timed-out join used to be silent, and a record written by
    // two threads is a record nobody can read. `workerJoinKnown` false means this build predates
    // the question -- it is not an answer of `yes`.
    bool workerJoinKnown = false;
    bool workerJoined = false;
    bool batteryDispatched = false;
    // The LAST step was issued and collected. Distinct from batteryDispatched, which only
    // says the battery started: a record filed mid-battery has the first true and this false.
    bool batteryComplete = false;
    // The transport reached the end of its file (the playback arms' completion observable).
    bool playedOut = false;
    uint32_t stepsAttempted = 0;
    uint32_t stepsAnswered = 0;
    // dispatch::Snapshot::rateCeiling, which is 66666 / raw_frame_us. 0 means UNKNOWN and is never
    // rendered as 4.0 (uiapi.h RateCeiling), so the two rate rows that need it are could-not-ask
    // rather than scored against a number nobody measured.
    float rateCeiling = 0.0f;
    double rateUnderCeiling = 0.0;
    double rateOverCeiling = 0.0;
    bool roundsKnown = false;
    size_t roundCount = 0;
    // Two raw frame periods, derived from the ceiling above. 0 means the ceiling was unknown, and
    // a landing cannot be scored without a tolerance the machine's own frame period supplies.
    long long seekToleranceUs = 0;
    long long reversePlayheadFirst = 0;
    long long reversePlayheadLast = 0;
    uint32_t reverseSamples = 0;
    // P17-B T18. The sidecar's bookmark count as the battery saw it BEFORE its own `mark`.
    // Absent and zero are different: a battery that never read the snapshot has not proved
    // the capture had no bookmarks.
    bool bookmarksAtStartKnown = false;
    size_t bookmarksAtStart = 0;
    // P17-B T14. `seek +50%` never reaches dispatch::Submit -- control::command.cpp refuses a
    // signed percentage at the PARSER, which is the surface the console line and the panel's
    // text field both go through. The row therefore reads the parse, and says so.
    bool seekPercentAsked = false;
    bool seekPercentParsed = false;
    char seekPercentError[192] = {0};
    TransportStep steps[static_cast<size_t>(Verb::Count)];

    // W1-J's launch-dir marker. The window a windowless arm is given, and whether a human is
    // watching, are properties of THIS launch -- and the harness's child-environment allow-list
    // carries exactly two names, so a third would be REFUSED before it reached
    // the module. They arrive in a file the harness writes beside the launch instead. Nothing here
    // defaults: an unread marker is could-not-ask, with the reason the reader wrote.
    bool markerPresent = false;
    bool markerWindowKnown = false;
    bool markerAttendedKnown = false;
    double markerWindowSec = 0.0;
    bool markerAttended = false;
    // One console line of human reason; the other reason buffers here size from this one.
    char markerSource[320] = {0};
    char markerWindowReason[sizeof markerSource] = {0};
    char markerAttendedReason[sizeof markerSource] = {0};

    // W1-C. The sc7 record the observer wrote and then read back. The product writes it; this is a
    // read of the artifact, and nothing here recomputes the diff.
    bool sc7Present = false;
    bool sc7Refused = false;              // P12's reader not built / refused: NOT-RUN, not a fail
    char sc7Verdict[32] = {0};
    long long sc7DeclaredSites = -1;
    long long sc7TextBytes = -1;
    // How many ledger rows the BUILD declared, counted out of the record's own `expected_rows`
    // array. -1 means the record carried none, which sc7.cpp does deliberately on a refusal.
    long long sc7ExpectedRows = -1;

    // W2-R / W2-F. The BROWSER arm's own precondition. The arm rides the same `offline-local` boot
    // every other arm rides, so the engine loads a level; the browser is then opened OVER it
    // through the product's PUBLIC entry (`ui::OpenBrowser`, src/ui/uiapi.h:40 -- the same call the
    // panel's Browse button and the console's dispatch make), which is the real user path. A launch
    // that never reached a level is could-not-ask, not a browser that failed to open.
    // No browserLevelReached field any more: the level-load answer has ONE holder, LevelReached()
    // below, reading [session]'s own state line. A copy of it here is a second place to be wrong.
    bool browserOpenRequested = false;
    bool browserOpen = false;
    // The tick's own stamp for the OpenBrowser call, on the observer's epoch --
    // the same clock every [observer] SURVIVE line publishes beside a wall-clock time, which is how
    // it is ordered against a log stamp without either side guessing at the other's units.
    bool browserOpenedAtKnown = false;
    double browserOpenedAtMs = 0.0;
    char browserReason[sizeof markerSource] = {0};

    // W2-A. Read only -- the harness scores the sentinel; the observer reports the name it expected
    // and whether that exact name was there.
    bool stopSentinelNameKnown = false;
    char stopSentinelName[128] = {0};
    bool stopSentinelPresent = false;
    char stopSentinelDir[kRowLineCap] = {0};
    // W2-R-C2 / ARCH-20 S5. The `ui-gui-begin-gate` record: five engine `.data` reads plus the
    // pointer-chased ImGui context, sampled at the row-2.5 site with `ui.gate_scope = false`, one
    // pair of samples around each `gui::Begin`. Published through csregister::Report (S5.3: NOT
    // Diagnostic(), whose 640-byte buffer already truncated three groups off this launch's line).
    // Nothing here defaults to an answer -- `guiGateSampled` false means the read side did not
    // publish, which is could-not-ask, and a zero invented here would be a scored answer nobody
    // measured.
    bool guiGateSampled = false;
    bool guiGateAddrResolved = false;
    char guiGateUnresolved[160] = {0};   // the fact id that did not resolve, when one did not
    uint64_t guiGateSamples = 0;         // pairs of samples taken around gui::Begin
    uint32_t guiSuppressed = 0;          // GUI.SUPPRESS_BYTE u8, BEFORE
    uint32_t guiHotkeyEnable = 0;        // GUI.HOTKEY_ENABLE_BYTE u8 -- F6 conditions the prediction
    int32_t guiReentryDepth = 0;         // GUI.REENTRY_DEPTH i32, BEFORE
    int32_t guiWindowBudget = 0;         // GUI.WINDOW_BUDGET i32 -- non-zero is an INSTRUMENT FAULT
    int32_t guiViewportsEnabled = 0;     // GUI.VIEWPORTS_ENABLED i32
    int32_t guiBeginBalanceDelta = 0;    // GUI.BEGIN_BALANCE after minus before, last pair
    // S5.1's control. The four fields below it are context-relative and are could-not-ask, never 0,
    // when it is false. The two OBSERVED atlas dwords and the two the producer read out of .rdata
    // at run time are both published so that the control's discriminating power is auditable from
    // the record rather than taken on the producer's word.
    bool guiImguiBaseValid = false;
    char guiImguiBaseReason[192] = {0};
    uint32_t guiAtlasDwordA = 0;         // observed at fonts+0x20
    uint32_t guiAtlasDwordB = 0;         // observed at fonts+0x24
    uint32_t guiAtlasExpectA = 0;        // GUI.ATLAS_CONST_A, read from .rdata at run time
    uint32_t guiAtlasExpectB = 0;        // GUI.ATLAS_CONST_B, read from .rdata at run time
    int32_t guiImguiFrameCountDelta = 0;      // ctx+0x1734, this entry minus previous
    uint32_t guiRenderDrawListsFnPresent = 0; // ctx+0xC0 != 0. Necessary, NOT sufficient
    int32_t guiRenderedCmdListsCount = 0;     // ctx+0x1898
    int32_t guiRenderedTotalVtxCount = 0;     // ctx+0x189C -- THE PIXEL NUMBER (S6.3's term (a))

    // ARCH-20 S6's `[gate ...]` group, struct read off the same Report at the same instant. It has
    // been on Diagnostic()'s line since W2-F-C2 and reached no artifact: over both runs of L5 #6,
    // `grep -c "ticks owner=" *.log` answered 0, so the whole suffix -- this group included -- was
    // published where nothing reads it. `csGateKnown` false means unread; no shape here means zero.
    bool csGateKnown = false;
    bool csGateScopeEnabled = false;      // the config row's EFFECTIVE value on this run
    uint64_t csGateUnresolved = 0;
    uint64_t csGateAlreadyOpen = 0;
    uint64_t csGateScopeEntries = 0;
    uint64_t csGateScopeRestores = 0;
    uint64_t csGateScopeChanged = 0;
    uint64_t csGateSetterUnmapped = 0;
    // 0 beside `hoversource=none` is could-not-ask, NOT "never hovered" (src/ui/inframe.h:283),
    // so the source name travels with the count and the count is never published without it.
    uint64_t csPanelHoveredFrames = 0;
    char csPanelHoverSource[32] = {0};
    uint64_t csDrawNanosTotal = 0;
    uint64_t csDrawNanosMax = 0;
    uint64_t csFirstDrawNanos = 0;
    // The ceiling the BUILD carried (inframe::kDrawNanosBudgetNs). 0 = the record predates
    // the field, and a record that never named a ceiling cannot be scored against one.
    uint64_t csDrawNanosBudget = 0;
    // ** THE SPLIT OF THE DRAW. Five numbers, and the flag that says they were read at all: every
    // one of them can legitimately be 0 -- browserIoOnDrawThread is 0 in a build where Draw opens
    // no file, which is the whole fix -- so 0 and "not carried" cannot share an encoding here.
    // csDrawPhasesKnown false is could-not-ask; it is NOT a browser that touched no file.
    bool csDrawPhasesKnown = false;
    uint64_t csImguiNanos = 0;
    uint64_t csBrowserScanNanos = 0;
    uint64_t csMetadataReadNanos = 0;
    uint64_t csTextFormatNanos = 0;
    uint64_t csBrowserIoOnDrawThread = 0;
    // ** THE STREAM THE DRAW ISSUED, and the distribution behind its maximum.
    // Off the same Report and the same instant as the five phases above, so csDrawPhasesKnown
    // answers for these too: a record whose probe never read the struct carries none of them.
    // ** csGuiStreamFrames == 0 is COULD-NOT-ASK -- no browser frame was drawn on that launch --
    // and never "the browser issued nothing"; the two counts beside it are then the initial value
    // of a counter and not a measurement. The percentiles are histogram UPPER edges: the true
    // value is in (x - 1024, x], and one equal to the top edge means "at least this", which is
    // what csDrawNanosOverHistogram counts. csDrawNanosSamples is their denominator.
    uint64_t csGuiItemsPerFrame = 0;
    uint64_t csGuiGlyphsPerFrame = 0;
    uint64_t csGuiStreamFrames = 0;
    uint64_t csDrawNanosP99 = 0;
    uint64_t csDrawNanosP999 = 0;
    uint64_t csDrawNanosSamples = 0;
    uint64_t csDrawNanosOverHistogram = 0;
};

// ★ ARCH-20 S5's twenty fields, csregister::Report -> StatusFacts, one assignment each.
//
// A template because this header pulls in no product code (tests/replay/CMakeLists.txt states that
// as the reason test_replay links nothing), while `probe_replay.cpp` instantiates it on the real
// `replay::ui::csregister::Report`: a Report that is missing or has retyped any field below fails
// to COMPILE there rather than letting the record carry a zero nobody measured. The test drives the
// same shipped code with a Report double that holds a distinct value in every field -- which is the
// only fixture on which a swapped or a dropped assignment can fail.
template <class ReportT>
inline void CopyGuiGateFacts(const ReportT& r, StatusFacts& s) {
    s.guiGateSampled = r.guiGateSampled;
    s.guiGateAddrResolved = r.guiGateAddrResolved;
    strncpy_s(s.guiGateUnresolved, sizeof s.guiGateUnresolved, r.guiGateUnresolved, _TRUNCATE);
    s.guiGateSamples = r.guiGateSamples;
    s.guiSuppressed = r.guiSuppressed;
    s.guiHotkeyEnable = r.guiHotkeyEnable;
    s.guiReentryDepth = r.guiReentryDepth;
    s.guiWindowBudget = r.guiWindowBudget;
    s.guiViewportsEnabled = r.guiViewportsEnabled;
    s.guiBeginBalanceDelta = r.guiBeginBalanceDelta;
    s.guiImguiBaseValid = r.guiImguiBaseValid;
    strncpy_s(s.guiImguiBaseReason, sizeof s.guiImguiBaseReason, r.guiImguiBaseReason, _TRUNCATE);
    s.guiAtlasDwordA = r.guiAtlasDwordA;
    s.guiAtlasDwordB = r.guiAtlasDwordB;
    s.guiAtlasExpectA = r.guiAtlasExpectA;
    s.guiAtlasExpectB = r.guiAtlasExpectB;
    s.guiImguiFrameCountDelta = r.guiImguiFrameCountDelta;
    s.guiRenderDrawListsFnPresent = r.guiRenderDrawListsFnPresent;
    s.guiRenderedCmdListsCount = r.guiRenderedCmdListsCount;
    s.guiRenderedTotalVtxCount = r.guiRenderedTotalVtxCount;
}

// The one text constant taken from W1-C's own measurement rather than recomputed here.
//
// There is no companion constant for the site count any more. `kSc7DeclaredSites = 1` predated
// `ed952e6`, which declares ledger rows 2.3 AND 2.4 under `-offline`, so L5 scored asked-FAIL on
// `declared_sites 2 (expected 1)` beside a verdict of PASS. A count of the build's own sites is not

inline bool InFrameLineAgrees(const InFrameLine& line, const StatusFacts& s) {
    return line.entries == s.csFrameSiteEntries && line.draws == s.csInFrameDraws &&
           line.refused == s.csRefusedDraws && line.noByte == s.csRefusedNoFrameByte &&
           line.noLatch == s.csRefusedNoLatch && line.noCtx == s.csRefusedNoContext &&
           line.facade == s.csRefusedByFacade && line.closed == s.csPanelClosedEntries &&
           line.fallback == s.csFallbackEntries && line.installed == s.csInFrameInstalled;
}

// a constant this file can hold; the build's own declaration is, and the record publishes it as
// `expected_rows`. text_bytes stays pinned because it is a measurement of a byte-identical file,
// which is exactly the thing that must not move.
constexpr long long kSc7TextBytes = 23859655;

// ---------------------------------------------------------------------------------------------
// The rows. Each returns its outcome AND writes the reason, always -- a could-not-ask with no
// reason is the shrug 9.1 forbids.
// ---------------------------------------------------------------------------------------------

// L2-RUN-2 F5: the LIVE sample decides and the log line corroborates, never the other way round.
// The log's last `bound game space` line is a history -- on L2 #2 it carried a refusal the session
// had resolved 1.2 s later, 5 s before the capture even opened, and the row filed asked-fail on a
// transient. The subject of this row is the entity surface's STATE, so it is read when the row is
// scored. `f` stays in the signature because the bind COUNT and the line itself are still the
// evidence that a binding was ever attempted, which is what separates a failure from a no-op.
inline Ask AskEntityBound(const StatusFacts& s, const LogFacts& f, char* d, size_t n) {
    if (!s.entitySampled) {
        if (!f.boundSeen) {
            _snprintf_s(d, n, _TRUNCATE,
                        "no [entity] 'bound game space' line has been written yet, and no entity "
                        "surface was reachable when this row was scored, so the module has not "
                        "reported a binding either way");
            return Ask::CouldNotAsk;
        }
        _snprintf_s(d, n, _TRUNCATE,
                    "%u binding(s) were logged, but no entity surface was reachable when this row "
                    "was scored. The log tail is the only evidence left and it is a history, not a "
                    "state, so it is reported and not scored; latest: %s", f.boundCount,
                    f.boundLine);
        return Ask::CouldNotAsk;
    }
    if (s.entityAvailable) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the entity surface reports AVAILABLE when scored, after %u logged binding(s); "
                    "latest logged line: %s", f.boundCount,
                    f.boundSeen ? f.boundLine : "(no bound game space line)");
        return Ask::Pass;
    }
    if (!f.boundSeen) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the entity surface reports '%s' when scored, and no [entity] 'bound game "
                    "space' line was ever written: nothing was bound, so this is not a binding "
                    "that failed", s.entityDiagnostic);
        return Ask::CouldNotAsk;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "bound (%u binding(s)), but the entity surface reports '%s' when scored rather "
                "than available; latest logged line: %s", f.boundCount, s.entityDiagnostic,
                f.boundLine);
    return Ask::Fail;
}

// L2-RUN-2 F4. Both L2 runs armed W1-F's loopback-only rewrite and measured it on neither: five
// answers, none of them W1-F, and the counters in no log line the harness reads. "Armed" is not
// "exercised", and inbound.cpp says so itself.
//
// The word scored here is the PRODUCT's: inbound::Classify over inbound::SelfView, sampled by the
// observer at scoring time. A row that re-derived the verdict from counters would be a second copy
// of the rule and would certify a broken one.
//
// * The ANTI-VACUITY clause is mirrored, not weakened. inbound.cpp:189 grades
//   `rewritten + alreadyLoopback == 0` could-not-ask because an all-loopback endpoint table on a
//   launch that bound nothing is indistinguishable from one this hook protected. That reaches this
//   row as could-not-ask and the pair is printed, so a reader can see WHY it was not a pass.
// * never-asked and could-not-ask both score Ask::CouldNotAsk because Ask has three values and its
//   words are the harness's vocabulary (the test harness is W1-J's; widening it is not this
//   packet's change). They are NOT collapsed: the product's own word is the first token of the
//   detail line, so "the hook never armed" and "the hook armed and saw nothing to say" stay apart
//   in the artifact even though they share a scoring word.
inline Ask AskInboundLoopbackOnly(const StatusFacts& s, char* d, size_t n) {
    if (!s.inboundSampled) {
        _snprintf_s(d, n, _TRUNCATE,
                    "bootstrap::inbound was never sampled by this observer, so the loopback-only "
                    "hook's own verdict was not read. This is the instrument missing, not an "
                    "answer about the hook");
        return Ask::CouldNotAsk;
    }
    if (strcmp(s.inboundVerdict, "asked-fail") == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "asked-fail: bootstrap::inbound classifies its own report as a failure -- "
                    "%u endpoint(s) classified NON-LOOPBACK (armed=%d, rewritten=%llu, "
                    "alreadyLoopback=%llu). A wildcard bind counts here and no peer has to arrive "
                    "first", s.inboundBoundNonLoopback, s.inboundArmed ? 1 : 0, s.inboundRewritten,
                    s.inboundAlreadyLoopback);
        return Ask::Fail;
    }
    if (strcmp(s.inboundVerdict, "asked-pass") == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "asked-pass: the hook armed, read endpoints, and the rewrite was EXERCISED -- "
                    "rewritten=%llu + alreadyLoopback=%llu > 0, boundNonLoopback=%u. This is the "
                    "reading L2 #1 and L2 #2 could not produce. %s", s.inboundRewritten,
                    s.inboundAlreadyLoopback, s.inboundBoundNonLoopback,
                    s.inboundAlreadyLoopback == 0
                        ? "NOTE: alreadyLoopback is 0, and inbound.h's free positive control says "
                          "the engine's own HTTP listener binds the literal 127.0.0.1 -- so a real "
                          "launch reaching that code should show at least one"
                        : "alreadyLoopback >= 1, which is inbound.h's free positive control (the "
                          "engine's own HTTP listener binds the literal 127.0.0.1)");
        return Ask::Pass;
    }
    if (strcmp(s.inboundVerdict, "never-asked") == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "never-asked: the loopback-only hook was not armed on this launch, so nothing "
                    "was classified and there is no endpoint table to grade. Scored could-not-ask "
                    "because Ask carries three words, but the hook's own word is never-asked and "
                    "it is not the same answer");
        return Ask::CouldNotAsk;
    }
    if (strcmp(s.inboundVerdict, "could-not-ask") == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "could-not-ask: the hook armed but %s. rewritten=%llu, alreadyLoopback=%llu, "
                    "boundNonLoopback=%u. An all-loopback table on a launch that bound nothing is "
                    "indistinguishable from one this hook protected, so it is not a pass "
                    "(inbound.cpp:189)",
                    s.inboundEndpointsRead ? "the rewrite was never exercised"
                                           : "no endpoint was classified at all",
                    s.inboundRewritten, s.inboundAlreadyLoopback, s.inboundBoundNonLoopback);
        return Ask::CouldNotAsk;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "bootstrap::inbound published the verdict word '%s', which is not one of "
                "never-asked / could-not-ask / asked-pass / asked-fail. That is a vocabulary this "
                "reader is behind on, not a value to score", s.inboundVerdict);
    return Ask::CouldNotAsk;
}

inline Ask AskServicesRebound(const LogFacts& f, char* d, size_t n) {
    if (!f.reboundSeen) {
        _snprintf_s(d, n, _TRUNCATE,
                    "no [playback] 'services re-bound:' line: playback never re-bound its services "
                    "in this session, so netvar liveness was never reported");
        return Ask::CouldNotAsk;
    }
    if (strcmp(f.reboundNetvar, "live") == 0) {
        _snprintf_s(d, n, _TRUNCATE, "netvar live; %s", f.reboundLine);
        return Ask::Pass;
    }
    _snprintf_s(d, n, _TRUNCATE, "netvar '%s', not live; %s", f.reboundNetvar, f.reboundLine);
    return Ask::Fail;
}

// The product's own sentence for "this process never reached Wwise", from
// src/audio/audiorate.cpp's SetDiagnostic call. It is a WORD OF THE PRODUCT quoted in one place,
// and tests/replay/test_observer.cpp asserts the checkout still contains it -- a copy that nobody
// checks is how the sink-verdict fixture came to score a sentence the product had deleted.
inline const char* const kWwiseNotInitialised = "Wwise is not initialised (GetAudioSettings refused)";

// L3-RUN-7 F6. The row used to explain a zero spread with the sentence for a non-zero one: "the
// seek landed mid-tick, so the forward step owed only the rest of it" is true when the two steps
// differ and false when they do not. The number decides the sentence.
inline const char* StepSpreadReason(long long spread) {
    if (spread == 0) {
        return " and that is correct: the seek landed ON a tick boundary, so each step owed a "
               "whole tick";
    }
    return " and that is correct: the seek landed mid-tick, so the forward step owed only the "
           "rest of it";
}

// L3-RUN-7 F3. `PLAYERSTATS 114 record(s), 1 user(s)` and a near-clean S_GAMESTATE have been on
// disk since `04706fc` and no row on the playback arm asked about either. These two do.
//
// Both read what the PRODUCT's reader decoded: probe_replay.cpp opens the file the session loaded
// with replay::DemoReader -- linked into the same binary -- and walks kStreamGameState and
// kStreamPlayerStats with the product's own record structs, the product's own present/fill masks
// and, for the monotone rule, the product's own PlayerStatsNoteTime. This header holds no constant
// of the format: `gsPresentAll` and `psFillAll` arrive as facts, so a mask this file believed in
// could not certify a file the loader would refuse.
inline Ask AskDemoScanRefusal(const StatusFacts& s, char* d, size_t n) {
    _snprintf_s(d, n, _TRUNCATE,
                "the loaded capture was not read back through the product's reader: %s",
                s.demoScanRefusal[0] != '\0' ? s.demoScanRefusal
                                             : "no file was loaded in this session and no refusal "
                                               "was recorded");
    return Ask::CouldNotAsk;
}

inline Ask AskGameStateDecoded(const StatusFacts& s, char* d, size_t n) {
    if (!s.demoScanned) {
        return AskDemoScanRefusal(s, d, n);
    }
    if (s.gsPresentAll == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the reader published no defined present-mask at all, so there is nothing to "
                    "score this file's %u record(s) against", s.gsRecords);
        return Ask::CouldNotAsk;
    }
    if (s.gsRecords == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%s carries no S_GAMESTATE record, so there is no decoded sample to ask about",
                    s.demoPath);
        return Ask::CouldNotAsk;
    }
    if (s.gsUndefinedBits != 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%u record(s) set present bit(s) 0x%x that this build's format does not define "
                    "(it defines 0x%x): the writer and the reader disagree about the field set",
                    s.gsRecords, s.gsUndefinedBits, s.gsPresentAll);
        return Ask::Fail;
    }
    if (s.gsPhasesSeen == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%u S_GAMESTATE record(s) decoded and not one of them names a phase, so the "
                    "phase the replay would show is not in the file", s.gsRecords);
        return Ask::Fail;
    }
    if (s.gsTimerReversals > 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "game_timer changes direction %u time(s) INSIDE a single phase across %u "
                    "record(s): a clock that runs both ways within one phase cannot be replayed, "
                    "and a seek cannot be told which of the two readings it landed on",
                    s.gsTimerReversals, s.gsRecords);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "%u S_GAMESTATE record(s) decoded through the product's own reader: %u distinct "
                "phase(s) named, game_timer monotone inside every phase, present union 0x%x of the "
                "0x%x this format defines (0x%x set on every record), no undefined bit",
                s.gsRecords, s.gsPhasesSeen, s.gsPresentUnion, s.gsPresentAll, s.gsPresentAlways);
    return Ask::Pass;
}

inline Ask AskPlayerStatsDecoded(const StatusFacts& s, char* d, size_t n) {
    if (!s.demoScanned) {
        return AskDemoScanRefusal(s, d, n);
    }
    if (s.psRecords == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%s carries no S_PLAYER_STATS record, so there is no scoreboard to ask about",
                    s.demoPath);
        return Ask::CouldNotAsk;
    }
    if (s.psUsers == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%u S_PLAYER_STATS record(s) and not one of them belongs to a user the reader "
                    "could name, so nothing in the stream can be attributed", s.psRecords);
        return Ask::Fail;
    }
    if (s.psFillUnion == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%u S_PLAYER_STATS record(s) for %u user(s) and the fill mask is EMPTY on every "
                    "one of them: the records are on disk and not one stat is in them",
                    s.psRecords, s.psUsers);
        return Ask::Fail;
    }
    if (s.psTimeBacksteps > 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%u of %u S_PLAYER_STATS record(s) go BACKWARDS in engine time for their own "
                    "user (the product's PlayerStatsNoteTime refused them), so the stream cannot "
                    "be searched by time", s.psTimeBacksteps, s.psRecords);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "%u S_PLAYER_STATS record(s) for %u user(s) decoded through the product's own "
                "reader; fill union 0x%x of the 0x%x this format defines; every user's engine time "
                "is non-decreasing in file order, by the product's own rule",
                s.psRecords, s.psUsers, s.psFillUnion, s.psFillAll);
    return Ask::Pass;
}

// W2-B NETVARS-B `ad17dd9`. The state IS published now, in the four sentences of
// src/playback/core/apply.cpp:28-39, and only ONE of the three non-`applied` ones is a product
// failure. `netvars: none` -- the word this row used to key on -- is not among them and is not
// printed anywhere any more, so recognising it would be scoring a word nothing writes.
inline Ask AskNetvarsApplied(const LogFacts& f, char* d, size_t n) {
    if (!f.netvarStateSeen) {
        _snprintf_s(d, n, _TRUNCATE,
                    "no 'netvars:' state was printed, so the fidelity state was never published");
        return Ask::CouldNotAsk;
    }
    if (StartsWith(f.netvarSaid, "applied")) {
        _snprintf_s(d, n, _TRUNCATE, "netvars: %s", f.netvarSaid);
        return Ask::Pass;
    }
    // ARCH-21: a solo -offline host writes no netvar records BY CONSTRUCTION, so every solo
    // capture reaches this arm. Scoring it asked-fail would red a working product for doing the
    // only thing it can, and the file has nothing to be asked about -- which is could-not-ask.
    if (StartsWith(f.netvarSaid, "no netvar records in this capture")) {
        _snprintf_s(d, n, _TRUNCATE,
                    "this capture carries no netvar records (ARCH-21: a solo -offline host "
                    "produces none by construction), so there was nothing to apply: '%s'",
                    f.netvarSaid);
        return Ask::CouldNotAsk;
    }
    // A property of ONE playhead, not of the file: the sample landed between records. Scrub to a
    // playhead where one is due and ask again.
    if (StartsWith(f.netvarSaid, "records present, none due at this playhead")) {
        _snprintf_s(d, n, _TRUNCATE,
                    "records present, none due at the sampled playhead, so this sample says "
                    "nothing about whether they apply: '%s'",
                    f.netvarSaid);
        return Ask::CouldNotAsk;
    }
    // A refusal WITH netvar live is W2-A's `no apply path on this build`, not a binding failure,
    // and 8.3a row 2 says that distinction is the whole point of the read. Both halves are on the
    // line so the reader does not have to go and find the other one.
    if (StartsWith(f.netvarSaid, "STORED, NOT APPLIED")) {
        _snprintf_s(d, n, _TRUNCATE,
                    "netvars: %s (services re-bound said netvar '%s': a refusal WITH netvar live "
                    "is inside INetVar::Write, not in the binding)",
                    f.netvarSaid, f.reboundSeen ? f.reboundNetvar : "not reported");
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "the fidelity state published '%s', which is none of applied / no netvar records "
                "in this capture / records present, none due at this playhead / STORED, NOT "
                "APPLIED. That is a vocabulary this reader is behind on, not a value to score",
                f.netvarSaid);
    return Ask::CouldNotAsk;
}

inline Ask AskRosterDressed(const LogFacts& f, char* d, size_t n) {
    if (!f.rosterSeen) {
        if (f.rosterOtherCount > 0) {
            _snprintf_s(d, n, _TRUNCATE,
                        "%u [identity] 'roster:' line(s) were printed and none of them is the "
                        "dressed line this row scores; the last was: %s",
                        f.rosterOtherCount, f.rosterOtherLine);
            return Ask::CouldNotAsk;
        }
        _snprintf_s(d, n, _TRUNCATE,
                    "no [identity] 'roster:' line: nothing was dressed and nothing refused to "
                    "dress, so the roster path was never reached");
        return Ask::CouldNotAsk;
    }
    if (f.rosterPlayers == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the roster line reports 0 players, so N == M is vacuous; %s", f.rosterLine);
        return Ask::CouldNotAsk;
    }
    if (f.rosterDressed == f.rosterPlayers) {
        _snprintf_s(d, n, _TRUNCATE, "%s", f.rosterLine);
        return Ask::Pass;
    }
    _snprintf_s(d, n, _TRUNCATE, "%u of %u players are NOT dressed: %s", f.rosterNotDressed,
                f.rosterPlayers, f.rosterLine);
    return Ask::Fail;
}

inline Ask AskOwnersMapped(const LogFacts& f, char* d, size_t n) {
    if (!f.ownersSeen) {
        _snprintf_s(d, n, _TRUNCATE, "no 'owners: N/M mapped' was printed in this session");
        return Ask::CouldNotAsk;
    }
    if (f.ownersPlayers == 0) {
        _snprintf_s(d, n, _TRUNCATE, "0 players, so N == M is vacuous; %s", f.ownersLine);
        return Ask::CouldNotAsk;
    }
    if (f.ownersMapped == f.ownersPlayers && !f.ownersRefused) {
        _snprintf_s(d, n, _TRUNCATE, "%s", f.ownersLine);
        return Ask::Pass;
    }
    _snprintf_s(d, n, _TRUNCATE, "%s", f.ownersLine);
    return Ask::Fail;
}

// The arming is an EVENT, so its absence means different things depending on whether there was
// anything to arm against. That is the difference between could-not-ask and asked-fail, and
// collapsing it is exactly the defect 9.1 names.
inline Ask AskInvOwn1Armed(const LogFacts& f, char* d, size_t n) {
    if (f.invOwn1Armed) {
        _snprintf_s(d, n, _TRUNCATE, "%s", f.invOwn1Line);
        return Ask::Pass;
    }
    if (f.invOwn1LocalUnknownArm) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the guard armed against NOTHING and never re-armed against a local index: %s",
                    f.invOwn1UnknownLine);
        return Ask::Fail;
    }
    if (!f.ownersSeen || f.ownersPlayers == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "no 'INV-OWN1 armed against ...' line in either of its two forms, and no owners "
                    "line with players in it, so there was nothing for the invariant to arm against");
        return Ask::CouldNotAsk;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "%u/%u owners were mapped but INV-OWN1 never armed against a local index%s",
                f.ownersMapped, f.ownersPlayers,
                f.ownersLocalUnknown ? " (the owners line says local-unknown)" : "");
    return Ask::Fail;
}

inline Ask AskBodyHandles(const LogFacts& f, char* d, size_t n) {
    // `ad17dd9` calls BindBodyHandlesIfReady from BOTH edges -- the window rebuild and the entity
    // re-bind -- and prints from either, so this row takes whichever one closed the pair. An
    // absent line therefore no longer means one named path was not taken.
    const char* edge = f.bodiesTrigger[0] != '\0' ? f.bodiesTrigger : "no trigger named on the line";
    if (!f.bodiesSeen) {
        _snprintf_s(d, n, _TRUNCATE,
                    "no [playback] 'body handles re-bound:' line from either trigger: neither "
                    "'window rebuilt' nor 'entity re-bound' completed the pair, so the entity "
                    "surface never reported available while a window was valid");
        return Ask::CouldNotAsk;
    }
    // L3-RUN-7 F1. `0 bound, 0 absent, 0 not net ids, 0 unresolvable` is not a rebind that failed
    // to resolve: absent, not-a-net-id and unresolvable are the three ways a CONSIDERED handle can
    // fail, and all three are zero, so the loop had nothing to walk. A capture with no netvar
    // records has no net ids in it BY CONSTRUCTION (ARCH-21: a solo -offline host writes none), and
    // a question about binding cannot be put to a file that carries no subject for it. That is
    // could-not-ask, by name. A capture that HAS records and still considers nothing is the defect
    // this row exists to catch, and it still reds.
    if (f.bodiesCountersParsed && f.bodiesBound == 0 && f.bodiesAbsent == 0 &&
        f.bodiesNotNetIds == 0 && f.bodiesUnresolvable == 0) {
        if (!f.netvarStateSeen) {
            _snprintf_s(d, n, _TRUNCATE,
                        "the rebind considered NO candidate at all (0 bound, 0 absent, 0 not net "
                        "ids, 0 unresolvable) and no 'netvars:' state was published, so nothing in "
                        "this session says whether the capture has net ids to bind: %s; the edge "
                        "that closed it: %s", f.bodiesLine, edge);
            return Ask::CouldNotAsk;
        }
        if (StartsWith(f.netvarSaid, "no netvar records in this capture")) {
            _snprintf_s(d, n, _TRUNCATE,
                        "no net ids in this capture: the fidelity state says '%s', so there was "
                        "nothing for the rebind to consider and it considered nothing -- %s; the "
                        "edge that closed it: %s", f.netvarSaid, f.bodiesLine, edge);
            return Ask::CouldNotAsk;
        }
        _snprintf_s(d, n, _TRUNCATE,
                    "this capture HAS netvar records (the fidelity state says '%s') and the rebind "
                    "still considered no candidate at all: %s; the edge that closed it: %s",
                    f.netvarSaid, f.bodiesLine, edge);
        return Ask::Fail;
    }
    if (f.bodiesBound > 0 && f.bodiesUnresolvable == 0) {
        _snprintf_s(d, n, _TRUNCATE, "%s; the edge that closed it: %s", f.bodiesLine, edge);
        return Ask::Pass;
    }
    _snprintf_s(d, n, _TRUNCATE, "%s; the edge that closed it: %s", f.bodiesLine, edge);
    return Ask::Fail;
}

inline Ask AskCameraControlBound(const LogFacts& f, char* d, size_t n) {
    if (f.cameraBoundSeen) {
        if (f.cameraRigEnabled == 1 && f.cameraConflicts == 0) {
            _snprintf_s(d, n, _TRUNCATE, "%s", f.cameraLine);
            return Ask::Pass;
        }
        // flag 0 with a bound control means SetMode lands in a disabled rig (8.1.1's [U] resolved
        // against us); a pose-owner conflict means somebody else is writing the view.
        _snprintf_s(d, n, _TRUNCATE, "rig enabled flag %u, pose-owner %s, conflicts %u; %s",
                    f.cameraRigEnabled, f.cameraPoseOwner, f.cameraConflicts, f.cameraLine);
        return Ask::Fail;
    }
    if (f.cameraRefusedSeen) {
        // 8.3a row 4 (ii): report the refusal, do not relax the check.
        _snprintf_s(d, n, _TRUNCATE, "the control refused rather than binding: %s", f.cameraLine);
        return Ask::Fail;
    }
    if (f.cameraStaleNounSeen) {
        _snprintf_s(d, n, _TRUNCATE,
                    "a [camera] control line was written in the pre-rename spelling, which this "
                    "reader no longer parses, so this launch cannot be scored: %s",
                    f.cameraStaleNounLine);
        return Ask::CouldNotAsk;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "no [camera] 'camera control bound' or 'camera control refused' line: the module "
                "never reached a rig probe in this session");
    return Ask::CouldNotAsk;
}

// RULING 4b's in-vivo proof, and the honest state of it.
//
// The question is whether the view keeps moving while the transport is PAUSED -- CAM.
// VIEWSTATE_INTEGRATE_FREE opens on CAM.WALLCLOCK_MS, so the stock free camera should fly with no
// integrator of ours. W2-E's read point (camera_control::SampleViewPose / PoseMotionBetween, over
// W1-H) made the question askable and the probe now asks it, so this row no longer names a missing
// mechanism -- it names a missing PRECONDITION when there is one.
//
// PoseMotionBetween answers THREE words and this row answers FOUR. Every precondition the library
// cannot check gates the verdict to its OWN named could-not-ask, and the library's own CouldNotAsk
// (a refused sample, a change of arm between the two, no wall span) stays could-not-ask. Only
// `Still` under a declared steering reaches Ask::Fail, because that alone is a design finding
// against ruling 4b; anything else landing there would report a question nobody asked as a defect.
inline Ask AskViewMovesWhilePaused(const StatusFacts& s, const LogFacts& f, char* d, size_t n) {
    if (!s.transportPresent || s.durationUs == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "no loaded file (transport %s, duration %llu us), so there was no paused "
                    "transport to watch the view against",
                    s.transportPresent ? "present" : "null",
                    static_cast<unsigned long long>(s.durationUs));
        return Ask::CouldNotAsk;
    }
    if (!s.pausedForView) {
        _snprintf_s(d, n, _TRUNCATE, "the battery never reached the pause step");
        return Ask::CouldNotAsk;
    }
    if (!s.viewPoseSampled) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the transport was paused but no view-pose pair was taken, so nothing was "
                    "compared");
        return Ask::CouldNotAsk;
    }
    if (s.viewReadFirstCode != replay::camera::ViewPoseRead::Ok ||
        s.viewReadSecondCode != replay::camera::ViewPoseRead::Ok) {
        // THREE refusals, not two, and they are three facts: NotBound is the plugin's own,
        // NoViewState is the ENGINE's -- the plugin asked and there was no view state 0 built --
        // and Unreadable is the build guard's. Naming them apart is what W1-H's fourth word is
        // for, so the detail says which of the two it was.
        const bool engineHadNothing =
            s.viewReadFirstCode == replay::camera::ViewPoseRead::NoViewState ||
            s.viewReadSecondCode == replay::camera::ViewPoseRead::NoViewState;
        // The WORD says which of the three refusals it was; the SENTENCE says which site produced
        // it, and it only exists because camera_module publishes the latch. Without it this row
        // printed a word and an operator had nothing to act on -- AUDIT_L1 finding 3.
        _snprintf_s(d, n, _TRUNCATE,
                    "GetViewPose refused: first read %s, second read %s -- %s (%s), so there is "
                    "nothing here to score",
                    s.viewReadFirst[0] != '\0' ? s.viewReadFirst : "(none)",
                    s.viewReadSecond[0] != '\0' ? s.viewReadSecond : "(none)",
                    engineHadNothing
                        ? "the ENGINE had built no view state 0, and the plugin did ask"
                        : "a refusal blanks the pose",
                    f.cameraViewRefusal[0] != '\0' ? f.cameraViewRefusal
                                                   : "the module logged no sentence for it");
        return Ask::CouldNotAsk;
    }
    if (!s.viewPausedAcross) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the transport did not report TransportState::Pause at both samples, so the "
                    "span this row measured was not a paused one");
        return Ask::CouldNotAsk;
    }
    if (s.viewSourceCode != replay::ViewPoseSource::ViewState) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the engine answered from the %s arm, not the view state: ruling 4b is about "
                    "the stock free camera's integrator and this pose is not its",
                    s.viewSource[0] != '\0' ? s.viewSource : "(unreported)");
        return Ask::CouldNotAsk;
    }
    if (!s.viewGateKnown || s.viewGateArmed == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the S8.1.2 gate was %s, so the mechanism ruling 4b names was not observed",
                    s.viewGateKnown
                        ? "reachable and DISARMED (integratorArmedKnown=1, viewstate+0x9C=0)"
                        : "not reachable: integratorArmedKnown=0, which is unknown and not 0");
        return Ask::CouldNotAsk;
    }
    if (!s.viewSteeringDeclared) {
        // A free camera nobody is steering is legitimately still, so `Still` would be a false FAIL.
        // No product surface reports input-applied, so the fact comes from the launch marker's
        // `attended`: it is DECLARED by the launch, not observed here, and the detail says so.
        _snprintf_s(d, n, _TRUNCATE,
                    "no-steering: the launch marker does not declare attended=true, and a free "
                    "camera nobody steers is legitimately still, so 'did not move' would not be an "
                    "answer to ruling 4b");
        return Ask::CouldNotAsk;
    }
    if (s.viewMotionCode == replay::camera_control::PoseMotion::Moved) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the view moved %.4f m over %.3f s of wall time while the transport was paused; "
                    "source=%s, viewstate+0x9C=%d (steering declared by the launch marker, not "
                    "observed)", s.viewMetres, s.viewSeconds, s.viewSource, s.viewGateArmed);
        return Ask::Pass;
    }
    if (s.viewMotionCode == replay::camera_control::PoseMotion::Still) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the view did NOT move over %.3f s of wall time while the transport was paused "
                    "and the gate was armed; source=%s, viewstate+0x9C=%d", s.viewSeconds,
                    s.viewSource, s.viewGateArmed);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "PoseMotionBetween answered %s: the two samples did not describe one comparable "
                "span (a refused sample, a change of arm between them, or no wall time)",
                s.viewMotion[0] != '\0' ? s.viewMotion : "(nothing)");
    return Ask::CouldNotAsk;
}

// W2-E, AUDIT_L1 finding 3's channel, scored end to end.
//
// The defect this row exists against is not "the view read refused" -- a refusal is a legitimate
// answer -- it is a refusal reaching the operator as a bare word. camera_control latches the first
// view-read refusal of a binding session and camera_module publishes it once, with the sentence
// W1-H's producers wrote. This row asks whether the sentence survived that trip.
//
// THREE answers, and the absence is NOT one of the other two. A session with no refusal has not
// shown the channel works -- scoring it Pass would make the row green on every launch where nothing
// asked, which is the fixture that cannot fail.
inline Ask AskViewReadRefusalNamesItsSite(const LogFacts& f, char* d, size_t n) {
    if (!f.cameraViewRefusedSeen) {
        _snprintf_s(d, n, _TRUNCATE,
                    "no [camera] 'camera control view read refused' line in this session, so there "
                    "was no sentence to name a site; the channel is unexercised, not proven");
        return Ask::CouldNotAsk;
    }
    if (f.cameraViewRefusal[0] == '\0') {
        _snprintf_s(d, n, _TRUNCATE,
                    "a view read refused and the line carried NO cause between 'refused: ' and "
                    "'; refusals ': the operator has the fact and not the site. %s",
                    f.cameraViewRefusalLine);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE, "%s; refusals %llu, pose-owner %s", f.cameraViewRefusal,
                f.cameraViewRefusals,
                f.cameraViewRefusalPoseOwner[0] != '\0' ? f.cameraViewRefusalPoseOwner
                                                        : "(unreported)");
    return Ask::Pass;
}

// W2-F. registrationInvocations == 0 with staleInvocations > 0 is its OWN loud outcome -- a stale
// CS that was dispatched against and never re-registered -- and 8.3a row 6 forbids folding it into
// a generic stale count.
inline Ask AskCsRegisterEpoch(const StatusFacts& s, char* d, size_t n) {
    if (!s.csAsked) {
        _snprintf_s(d, n, _TRUNCATE, "csregister::Status_() was never read");
        return Ask::CouldNotAsk;
    }
    const char* fields =
        "status=%s invocations=%llu registrationInvocations=%llu staleInvocations=%llu "
        "staleDetected=%llu spaceChanges=%llu spacesSeen=%llu registrations=%u deferrals=%llu "
        "marshalledDraws=%llu coalesced=%llu pending=%llu [threads owner=%lu callback=%lu]";
    char body[480];
    _snprintf_s(body, sizeof body, _TRUNCATE, fields, s.csStatus,
                static_cast<unsigned long long>(s.csInvocations),
                static_cast<unsigned long long>(s.csRegistrationInvocations),
                static_cast<unsigned long long>(s.csStaleInvocations),
                static_cast<unsigned long long>(s.csStaleDetected),
                static_cast<unsigned long long>(s.csSpaceChanges),
                static_cast<unsigned long long>(s.csSpacesSeen), s.csRegistrations,
                static_cast<unsigned long long>(s.csDeferrals),
                static_cast<unsigned long long>(s.csMarshalledDraws),
                static_cast<unsigned long long>(s.csCoalescedInvocations),
                static_cast<unsigned long long>(s.csPendingInvocations),
                static_cast<unsigned long>(s.csOwnerThreadId),
                static_cast<unsigned long>(s.csCallbackThreadId));

    // AUDIT_L5_RUN3 M1 / ARCH-11 S5: BOTH ids reach the artifact, and the frozen suffix that
    // carries them is checked rather than assumed. W2-F publishes it on every Diagnostic() line
    // "without exception", so its absence is a broken contract and not a quiet zero.
    // A line that names ITSELF a refusal is refused by name, before the branch
    // below reads its missing suffix as a broken contract. `DIAGNOSTIC SUFFIX DOES NOT FIT` means
    // the module published no group at all and said so; reporting that as a contract W2-F broke
    // would file a defect against the publisher for the one case in which it behaved correctly.
    if (strcmp(s.csDiagRefusal, DiagRefusalWord(DiagRefusal::SuffixDoesNotFit)) == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "csregister::Diagnostic() answered with a line that names itself a REFUSAL: "
                    "'%s'. No group is on that line and no field of one is read as zero, so the "
                    "direction this row would name is unread. The struct read is REPORTED and "
                    "scored by nothing: %s",
                    s.csDiagRefusal, body);
        return Ask::CouldNotAsk;
    }
    if (!s.csThreadsPublished) {
        _snprintf_s(d, n, _TRUNCATE,
                    "csregister::Diagnostic() did not end in the frozen ' [threads owner=<n> "
                    "callback=<n>]' suffix, so the direction this row would name is unread; %s",
                    body);
        return Ask::CouldNotAsk;
    }
    if (s.csOwnerThreadIdPublished != s.csOwnerThreadId ||
        s.csCallbackThreadIdPublished != s.csCallbackThreadId) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the two publications of one fact disagree: Report says owner=%lu callback=%lu, "
                    "Diagnostic()'s suffix says owner=%lu callback=%lu. No direction is named off a "
                    "pair that does not agree with itself; %s",
                    static_cast<unsigned long>(s.csOwnerThreadId),
                    static_cast<unsigned long>(s.csCallbackThreadId),
                    static_cast<unsigned long>(s.csOwnerThreadIdPublished),
                    static_cast<unsigned long>(s.csCallbackThreadIdPublished), body);
        return Ask::CouldNotAsk;
    }
    // The marshal snapshot must account for itself. Every recorded entry is drawn, coalesced into
    // another entry's draw, or still pending, so the three can never sum to LESS than the
    // invocation count. They can sum to MORE: Status_() reads `invocations` before it reads
    // `pending` (csregister.cpp:636 then :641) and a worker can record between the two, which
    // inflates pending by what it added. A shortfall is the direction that means an entry went
    // missing, and a shortfall computed over a torn snapshot is not a measurement.
    if (s.csMarshalledDraws + s.csCoalescedInvocations + s.csPendingInvocations < s.csInvocations) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the marshal snapshot does not account for itself -- %llu entries recorded, "
                    "%llu drawn + coalesced + pending -- so the difference is a torn read and not a "
                    "count; %s",
                    static_cast<unsigned long long>(s.csInvocations),
                    static_cast<unsigned long long>(s.csMarshalledDraws +
                                                    s.csCoalescedInvocations +
                                                    s.csPendingInvocations), body);
        return Ask::CouldNotAsk;
    }
    // ARCH-11 S5 retires `WrongThread` for the off-thread DISPATCH, which is the expected path and
    // is counted in offThreadInvocations. What still carries the word is a DRAIN entered off the
    // owner thread (csregister.cpp:176-181), which the header marks must-stay-0 -- a different
    // subject, and a real defect. The direction is named from the two ids rather than implied.
    if (strcmp(s.csStatus, "WrongThread") == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "WRONG-THREAD DRAIN: the mailbox was drained off the owner thread. Direction: "
                    "the owner thread (our stage-0 tick's, ui_module.cpp:33) is %lu and the last "
                    "band-3 callback entry arrived on %lu -- %s. An off-thread DISPATCH is expected "
                    "and counted; a drain there is not; %s",
                    static_cast<unsigned long>(s.csOwnerThreadId),
                    static_cast<unsigned long>(s.csCallbackThreadId),
                    s.csOwnerThreadId == s.csCallbackThreadId
                        ? "the SAME thread, so the entry did not arrive off a worker and the drain "
                          "was reached from a third caller"
                        : "different threads, which is the normal dispatch: the defect is the "
                          "drain, not the callback",
                    body);
        return Ask::Fail;
    }
    if (s.csRegistrationInvocations == 0 && s.csStaleInvocations > 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "STALE-DISPATCH-ONLY: the current registration has never been dispatched "
                    "against while a registration we no longer hold has; %s", body);
        return Ask::Fail;
    }
    if (s.csRegistrations == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "no registration has completed, so the epoch counters have nothing to report "
                    "on; %s", body);
        return Ask::CouldNotAsk;
    }
    // DESIGN S8.5.4a: `marshalledDraws == 0` over a window that never reached a drawable frame is
    // CENSORED, never a FAIL -- a duration that ended because the harness stopped looking is
    // ">= N" and not a verdict. `invocations == 0` is what says no frame ever offered itself; with
    // entries recorded and none drawn the window DID offer frames and the silence is scored.
    if (s.csMarshalledDraws == 0 && s.csInvocations == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "CENSORED: the band-3 callback recorded no entry at all, so nothing has yet "
                    "offered itself to be drawn and this window is a lower bound, not a verdict; %s",
                    body);
        return Ask::CouldNotAsk;
    }
    // AUDIT_L5_RUN3 M2 / ARCH-11 S5. The epoch clause is over `spacesSeen`, which counts the FIRST
    // space too; `spaceChanges` skips it and `registrations == spaceChanges` can never hold on a
    // one-space session. Scored as "every space we saw took a registration": equality is the
    // one-epoch case, and MORE registrations than spaces is the designed re-registration after a
    // space went stale (Report::staleDetected), which is not a defect and is not failed here.
    if (s.csRegistrations < s.csSpacesSeen) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the epoch clause does not hold: %u registration(s) against %llu game space(s) "
                    "seen, so a space was seen and took none; %s",
                    s.csRegistrations, static_cast<unsigned long long>(s.csSpacesSeen), body);
        return Ask::Fail;
    }
    // `Running` in S8.5.4a's THREE-PART sense, scored from the parts rather than from the word, so
    // a status name that drifts cannot green this row on its own. CLEAN(L5) clause 4 adds
    // staleDetected == 0.
    //
    // ARCH-13 ruling 1 (S1.2) says DL-07 passes on `Running` OR `MarshalStale` OR `CallbackSilent`,
    // because all three words are only reachable BELOW the rung where all three legs hold -- they
    // are recency diagnostics for the reader and inputs to no gate but their own. Scoring the parts
    // is already that rule and is why no change is needed here: `csRunning` and `csStatus` reach
    // this verdict through nothing. The three words are pinned as positive controls in
    // test_observer.cpp so a later rewrite to `status == Running` reds instead of narrowing DL-07.
    const bool runningParts = s.csRegistrations >= 1 && s.csInvocations > 0 &&
                              s.csMarshalledDraws > 0;
    if (runningParts && s.csRegistrationInvocations > 0 && s.csStaleDetected == 0) {
        _snprintf_s(d, n, _TRUNCATE, "%s", body);
        return Ask::Pass;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "not Running in S8.5.4a's three-part sense with a live dispatch: registrations>=1 "
                "%s, invocations>0 %s, marshalledDraws>0 %s, registrationInvocations>0 %s, "
                "staleDetected==0 %s; %s",
                s.csRegistrations >= 1 ? "yes" : "NO", s.csInvocations > 0 ? "yes" : "NO",
                s.csMarshalledDraws > 0 ? "yes" : "NO",
                s.csRegistrationInvocations > 0 ? "yes" : "NO",
                s.csStaleDetected == 0 ? "yes" : "NO", body);
    return Ask::Fail;
}

// 8.3a row 8. The read passes when the tap is available, OR when it is unavailable WITH a
// diagnostic that names its subject. A bare available=0 with no diagnostic is a FAIL, and that is
// the whole reason the row exists.
inline Ask AskPrologueDiagnostic(const StatusFacts& s, char* d, size_t n) {
    // The registration half is NOT gated on services->wiretap. L5 run 3 answered this row
    // could-not-ask on `services->wiretap is null` and never reached these numbers -- in a process
    // whose own capture module had already logged `36 of 38 message taps registered`. The two
    // halves have different subjects and different owners: W1-L's prologue tap answers "could the
    // build check be offered at all", and W2-A's TapRegistrationNow() answers "is the message bus
    // actually tapped". The second is read straight out of capture.cpp, which is linked into this
    // probe, so a null wiretap cannot make it unknown -- only unasked-about.
    if (s.tapRegStarted) {
        const uint32_t accounted =
            s.tapRegistered + s.tapDeferred + s.tapGivenUp + s.tapUnavailable;
        if (accounted != s.tapTotal) {
            // manifestbuild.h publishes these under one atomic word precisely so this identity
            // holds on every sample. If it does not, the snapshot is torn or a bucket is missing,
            // and a count taken over it is not a measurement.
            _snprintf_s(d, n, _TRUNCATE,
                        "the tap registration snapshot does not account for itself: %u registered "
                        "+ %u deferred + %u given up + %u unavailable = %u, and the capture "
                        "declares %u surface(s) -- there is no count here to score",
                        s.tapRegistered, s.tapDeferred, s.tapGivenUp, s.tapUnavailable, accounted,
                        s.tapTotal);
            return Ask::CouldNotAsk;
        }
        if (s.tapRegistered != s.tapTotal) {
            // Available() alone passed this row with 0 of 36 taps registered, so a
            // capture missing S_PLAYER_WIRE / S_BODY_WIRE / S_EVENT / S_NETVAR looked healthy.
            _snprintf_s(d, n, _TRUNCATE,
                        "only %u of %u message-bus registrations are live: %u still deferred, %u "
                        "given up on, %u on a tap that answered unavailable -- the streams behind "
                        "them are empty",
                        s.tapRegistered, s.tapTotal, s.tapDeferred, s.tapGivenUp,
                        s.tapUnavailable);
            return Ask::Fail;
        }
    }
    if (!s.tapPresent) {
        _snprintf_s(d, n, _TRUNCATE,
                    "services->wiretap is null: W1-L registered no tap in this process, so the "
                    "build check was never offered (the capture's own registrations are %s)",
                    s.tapRegStarted ? "all live" : "not started, so nothing was asked of the bus");
        return Ask::CouldNotAsk;
    }
    if (s.tapAvailable) {
        if (!s.tapRegStarted) {
            // The tap is built on the first tick carrying a CR15NetGame. Before that every count is
            // "not asked yet", and reading that as "asked and refused" would fail every launch that
            // never reached state 9.
            _snprintf_s(d, n, _TRUNCATE,
                        "the tap is available, but the capture never built its tap, so nothing was "
                        "ever asked of the message bus");
            return Ask::CouldNotAsk;
        }
        // Any shortfall was already answered above, so reaching here is `registered == total`.
        _snprintf_s(d, n, _TRUNCATE,
                    "the tap is available and all %u message-bus registrations are live",
                    s.tapTotal);
        return Ask::Pass;
    }
    const bool names =
        FindToken(s.tapDiagnostic, "prologue-unreadable: ") != nullptr &&
        strstr(s.tapDiagnostic, " at 0x") != nullptr && strstr(s.tapDiagnostic, " bytes") != nullptr;
    if (names) {
        _snprintf_s(d, n, _TRUNCATE,
                    "unavailable, and the diagnostic names its subject: %s", s.tapDiagnostic);
        return Ask::Pass;
    }
    if (s.tapDiagnostic[0] != '\0') {
        _snprintf_s(d, n, _TRUNCATE,
                    "unavailable with a diagnostic that does not name a symbol, a VA and a byte "
                    "count: '%s'", s.tapDiagnostic);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "available=0 with an EMPTY diagnostic: a two-state answer where asked-and-failed "
                "and could-not-ask are indistinguishable");
    return Ask::Fail;
}

// W2-H. Pass is BOTH the slope and the driver's own line: a slope with no commanded change is a
// coincidence, and a commanded change with no slope is the mechanism failing.
inline Ask AskAudioHalfRate(const StatusFacts& s, const LogFacts& f, char* d, size_t n) {
    if (!s.transportPresent || s.durationUs == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "NotRunInvalid: no file loaded (duration %llu us), so no rate was ever "
                    "commanded", static_cast<unsigned long long>(s.durationUs));
        return Ask::CouldNotAsk;
    }
    if (!s.audioPresent) {
        _snprintf_s(d, n, _TRUNCATE, "services->audioRate is null: W2-H registered no sink");
        return Ask::CouldNotAsk;
    }
    if (!s.audioAvailable) {
        // L3-RUN-5 s8/F4: three different reasons have now been printed for this one failure over
        // three launches, and W2-H cannot act on a row that summarises them away. Both of the
        // product's own sentences are reproduced VERBATIM and neither is paraphrased:
        // IAudioRate::Diagnostic() is why the sink calls itself unavailable, and ratedrive.cpp's
        // `transport rate 0.50x:` line is what it answered at the one moment this row is about
        // (StatusFacts::audioRateLabel is read at record time, by which point the battery has
        // already restored 1.00x, so it is NOT that reason). Still could-not-ask -- an unavailable
        // sink is not a failed slope -- so this changes what the row REPORTS, never what it scores.
        _snprintf_s(d, n, _TRUNCATE,
                    "the sink is unavailable. IAudioRate::Diagnostic() says, verbatim: \"%s\". At "
                    "0.5x the driver wrote, verbatim: \"%s\"",
                    s.audioDiagnostic[0] != '\0' ? s.audioDiagnostic
                                                 : "(the sink published no diagnostic at all)",
                    f.rateHalfLineSeen ? f.rateHalfLine
                                       : "(no `transport rate 0.50x` line was written at all)");
        return Ask::CouldNotAsk;
    }
    // L3-RUN-7 F2. `Wwise is not initialised (GetAudioSettings refused)` at bind time, together
    // with `CSoundPlayer not resolved`, is this process saying it never reached a mixer.
    // AudioSeconds() scales GetBufferTick by settings the module has not sourced, so whatever
    // slope comes out of that is not the ratio anybody set -- it is not a failed halving, it is a
    // question that was never put. Both sentences are the product's own and both flip on a launch
    // with audio, so this is not a refusal the row can hide behind for ever.
    if (f.audioBindRefusalSeen && !f.soundPlayerResolved &&
        strstr(f.audioBindRefusal, kWwiseNotInitialised) != nullptr) {
        _snprintf_s(d, n, _TRUNCATE,
                    "no mixer in this process, so the slope is not this row's question. The rate "
                    "module refused to arm at bind time, verbatim: \"%s\". The state module said, "
                    "verbatim: \"%s\". At 0.5x the driver wrote, verbatim: \"%s\"",
                    f.audioBindRefusal,
                    f.soundPlayerLine[0] != '\0' ? f.soundPlayerLine
                                                 : "(nothing about CSoundPlayer at all)",
                    f.rateHalfLineSeen ? f.rateHalfLine
                                       : "(no `transport rate 0.50x` line was written at all)");
        return Ask::CouldNotAsk;
    }
    if (!s.slopeMeasured) {
        _snprintf_s(d, n, _TRUNCATE,
                    "NotRunStarved: GetBufferTick did not advance enough over %.0f ms to give a "
                    "slope", s.slopeWindowMs);
        return Ask::CouldNotAsk;
    }
    if (!f.rateLineSeen) {
        _snprintf_s(d, n, _TRUNCATE,
                    "fail-wiring: a file is loaded and 0.5x was commanded, but no 'transport rate' "
                    "line was written; measured slope %.4f over %.0f ms", s.slope, s.slopeWindowMs);
        return Ask::Fail;
    }
    // ★ THE OPERAND IS THE 0.50x LINE, NEVER `rateLine`. L3-RUN-7 scored a half-rate slope and
    // quoted `transport rate 1.00x` as the change that had been commanded -- the battery's LAST
    // rate change, restored long before the record was written. A row that names the wrong operand
    // makes a correct product look wrong and a broken one look explained.
    if (!f.rateHalfLineSeen) {
        _snprintf_s(d, n, _TRUNCATE,
                    "rate line(s) were written and none of them is a 0.50x one (the last was: %s), "
                    "so there is no half-rate command to date the measured slope %.4f to",
                    f.rateLine, s.slope);
        return Ask::CouldNotAsk;
    }
    const bool slopeHalves = s.slope >= 0.48 && s.slope <= 0.52;
    if (slopeHalves) {
        _snprintf_s(d, n, _TRUNCATE,
                    "slope %.4f over %.0f ms, label '%s'; the 0.50x driver line: %s", s.slope,
                    s.slopeWindowMs, s.audioRateLabel, f.rateHalfLine);
        return Ask::Pass;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "fail-mechanism: the change was commanded (%s) but GetBufferTick's slope is %.4f "
                "over %.0f ms, not 0.50", f.rateHalfLine, s.slope, s.slopeWindowMs);
    return Ask::Fail;
}

// W1-C. The product's reader writes the record; this reads it back. `refused` renders NOT-RUN
// BEFORE any row-set comparison (P12's R5 ruling), which is why it is could-not-ask and not a fail.
inline Ask AskSc7TextDiff(const StatusFacts& s, char* d, size_t n) {
    if (!s.sc7Present) {
        _snprintf_s(d, n, _TRUNCATE, "sc7_text_diff.json was not written into the artifact dir");
        return Ask::CouldNotAsk;
    }
    if (s.sc7Refused) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the reader refused, so there is no verdict to score: W1-C's in-process reader "
                    "is not in this binary (P12)");
        return Ask::CouldNotAsk;
    }
    // The site count is scored against the build's OWN declaration, never against a number this
    // file holds. The two fields come from different places in the record -- `expected_rows` is
    // what the ledger declares, `declared_sites` is what the scan mapped inside the compared span
    // -- so agreeing is a real claim, and a build that arms a new site moves both together.
    if (s.sc7ExpectedRows < 1) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the record %s, so there is no declaration to score declared_sites %lld "
                    "against (verdict '%s')",
                    s.sc7ExpectedRows < 0 ? "carries no expected_rows at all"
                                          : "declares an EMPTY expected_rows, and a diff over no "
                                            "declared row proves nothing",
                    s.sc7DeclaredSites, s.sc7Verdict);
        return Ask::CouldNotAsk;
    }
    if (strcmp(s.sc7Verdict, "PASS") == 0 && s.sc7DeclaredSites == s.sc7ExpectedRows &&
        s.sc7TextBytes == kSc7TextBytes) {
        _snprintf_s(d, n, _TRUNCATE,
                    "verdict PASS, declared_sites %lld == the %lld row(s) the build declares, "
                    "text_bytes %lld", s.sc7DeclaredSites, s.sc7ExpectedRows, s.sc7TextBytes);
        return Ask::Pass;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "verdict '%s', declared_sites %lld (the build declares %lld), text_bytes %lld "
                "(expected %lld)", s.sc7Verdict, s.sc7DeclaredSites, s.sc7ExpectedRows,
                s.sc7TextBytes, kSc7TextBytes);
    return Ask::Fail;
}

// ---------------------------------------------------------------------------------------------
// THE LEVEL-LOAD PREDICATE -- one holder, and it is [session]'s own line (ARCH-10 4.1 row 4).
//
// L5 filed `w2r-browser-opened-over-level` as could-not-ask with the cause "level-not-reached" over
// a launch whose level loaded at 04:16:38 and stayed loaded. The cause was FALSE: the probe used
// `[entity] bound game space` as its proxy for the level, and the bind is what was missing. A
// predicate that reports the wrong cause once will do it again on the next unrelated failure, so
// the two facts are now two facts -- the session's state line answers "did a level load", and the
// bind's absence is reported as the bind's absence.
// ---------------------------------------------------------------------------------------------
inline bool LevelReached(const LogFacts& f) {
    return f.sessionLevelStateSeen;
}

// L3-RUN-2 (a). LevelReached answers "did a level load"; this answers "does a world exist".
// The probe filed at 27995 ms on state 7 and the camera rig bound 14.8 s later, so
// w2e-camera-rig-probe recorded an asked-fail against a rig that had not been built --  the
// instrument perturbing the system in its timing form. Only 9 (`in game`) may end the
// playback arm; 7 and 8 keep it provisional.
inline bool InGameReached(const LogFacts& f) {
    return f.sessionInGameSeen;
}

// `levelLoadAtMs`: the first level-state line's stamp, moved onto the
// OBSERVER's epoch through the anchor LogFacts::armEpochWallMs measured off the SURVIVE lines.
// False -- and no number -- when either half is missing; a row that cannot convert must say which
// stamp it lacks, never compare the two clocks raw.
// The one entry in an interval that is allowed to sit past the last heartbeat: a level line can
// be written between the final SURVIVE and the read, and refusing that would be a units pedantry
// rather than a finding.
const double kLaunchIntervalSlackMs = 5000.0;

// ★ AUDIT_L5_RUN_4 F10 (b): THE MIDNIGHT GUARD IS STRUCK. It read
//
//     if (ms < -43200000.0) { ms += 86400000.0; }   // "the launch crossed midnight"
//
// and on the L5 #4 record it turned -45559924 ms -- a stamp latched out of the PREVIOUS DAY'S
// launch -- into a plausible-looking 40840076 ms, which the completion predicate then subtracted
// from a 240886 ms survival and got -40599190. A negative can never reach the 5000 ms the arm
// holds for, so the probe never filed a verdict at all. That is BRIEF_COMMON section 9's "missing
// data must NOT default to a confident answer", with the confident answer manufactured out of an
// anomaly the code had every fact it needed to reject.
//
// So: an interval that does not fall inside this launch's own observed lifetime is a REFUSAL that
// NAMES the anomaly, and there is no correction term anywhere in this function.
inline bool LevelLoadMonotonicMs(const LogFacts& f, double* out, char* why = nullptr,
                                 size_t whyN = 0) {
    if (why != nullptr && whyN > 0) {
        why[0] = '\0';
    }
    if (!f.launchAnchorSeen) {
        if (why != nullptr) {
            _snprintf_s(why, whyN, _TRUNCATE,
                        "the log source carries no `[boot] attached` line, so there is no way to "
                        "tell which launch its %llu line(s) belong to -- replay::LogPath() is "
                        "cumulative and a stamp read out of it without that anchor may be any "
                        "launch's (AUDIT_L5_RUN_4 F10)",
                        f.linesBeforeLaunch + f.linesThisLaunch);
        }
        return false;
    }
    if (!f.levelStampKnown || !f.armEpochKnown) {
        if (why != nullptr) {
            _snprintf_s(why, whyN, _TRUNCATE,
                        "level-load line stamp %s, [observer] SURVIVE clock anchor %s",
                        f.levelStampKnown ? "present" : "MISSING",
                        f.armEpochKnown ? "present" : "MISSING");
        }
        return false;
    }
    const double ms = f.levelWallMs - f.armEpochWallMs;
    if (ms < 0.0 || ms > f.armEpochMonoMs + kLaunchIntervalSlackMs) {
        if (why != nullptr) {
            _snprintf_s(why, whyN, _TRUNCATE,
                        "the level-load stamp converts to %.0f ms on the observer's epoch, which "
                        "is OUTSIDE this launch's own observed lifetime of %.0f ms: the stamp does "
                        "not belong to this launch, and an out-of-window interval is REFUSED here "
                        "rather than corrected (AUDIT_L5_RUN_4 F10). Level line: %s",
                        ms, f.armEpochMonoMs, f.levelStampLine);
        }
        return false;
    }
    *out = ms;
    return true;
}

inline void LevelLoadReason(const LogFacts& f, char* d, size_t n) {
    if (f.sessionLevelStateSeen) {
        if (!f.boundSeen) {
            _snprintf_s(d, n, _TRUNCATE,
                        "a level loaded -- [session] NetGame state %d -- and bind not observed: no "
                        "[entity] 'bound game space' line in this launch. %s", f.sessionLevelState,
                        f.sessionLevelStateLine);
        } else {
            _snprintf_s(d, n, _TRUNCATE,
                        "a level loaded -- [session] NetGame state %d -- and the entity module "
                        "bound: %s", f.sessionLevelState, f.boundLine);
        }
        return;
    }
    if (f.sessionStateSeen) {
        _snprintf_s(d, n, _TRUNCATE,
                    "level-not-reached: [session] last reported NetGame state %d and never 7, 8 or "
                    "9: %s", f.sessionState, f.sessionStateLine);
        return;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "level-not-reached: no [session] 'NetGame state' line at all, so the session never "
                "reported a state either way and no level load can be claimed or denied");
}

// ---------------------------------------------------------------------------------------------
// W1-E / ARCH-10 section 4.2: is the product's accessor confirmed?
//
// This is the row that confirms or refutes `NETGAME.CS_SPACE_OFF (+0x2B08)` in vivo, and it is the
// only one that can -- the runtime half of the type ruling is not resolvable from a dump (1.4). It
// keys on `<Verdict>` alone: the rest of `<TABLE>` is W1-C's FormatTable rendering, carried through
// VERBATIM and never re-parsed field by field, so this file holds no second copy of that format.
//
// It answers ONE question, and 4.2's count rules are NOT how it answers.
//
// L5 run 3 scored this row asked-fail on a launch that confirmed the fact: TWO candidates were
// legitimately Ready -- `+0x2B08` (the netGame's CncaGameSpace, 99 systems, the object `[entity]
// bound game space` fired on) and `game+0x7AF8` (a game-level CncaGameSpace, 35 systems, Ready
// ~0.4 s EARLIER). Section 1's ruling is what settles it: `+0x8` IS the CR15Game and is never a
// rival space, and `game+0x7AF0` / `game+0x7AF8` name other real spaces the engine owns. Two real
// spaces is not a tie to be broken; counting them as rivals was the fault, and a "first Ready wins"
// rule would have picked the wrong one. Both are carried as INFORMATION in every detail below and
// decide nothing.
//
// `+0x8` keeps exactly one job -- 4.2 rule 5's guard-sensitivity canary -- and is never counted.
// AUDIT_F3A F3A-1: `Empty/Empty` there is reachable, not hypothetical (tableguard.cpp answers
// `Empty` when `base == 0 && bytes == 0`, which is CR15Game+0x420 all zero at boot), and under the
// count rules it produced a false tie or, worse, a sentence naming the CR15Game as the offset the
// launch says is right. It is noted in the detail and counted nowhere.
//
// `+0x2B08` and `+0x2B10` are judged on the LATEST sample. The line repeats on the bind cadence,
// and on the recorded L5 run 3 log +0x2B08's three samples walk `null` -> `Empty` -> `Ready`: an
// early null or Empty is the state before the space is built, not a refusal. `+0x8`'s canary is
// the one fact latched the other way, over ALL samples -- see LogFacts::csSpaceAt8ReadyEver.
//
// ARCH-12 MOVED THIS ROW'S SUBJECT. `*(ng+0x2B10)` is the netGame's LEVEL CncaGameSpaceMT and the
// spectator-camera CS is registered in it (2.2/2.3); `+0x2B08` is the other space and its verdict
// is REPORTED but no longer decides (4.3 rule 2). The four-column line could not show this: it had
// no column for the slot that was right, so L5 run 3's reader had two Ready spaces to choose
// between and neither was the answer (R3, "missing column, not wrong choice"). A line with no
// `+0x2B10=` key is therefore could-not-ask NAMING the missing column -- a parser that read the
// old line as asked-pass because +0x2B08 said Ready is the exact defect 4.3 exists to close.
// ---------------------------------------------------------------------------------------------
inline Ask AskCsSpaceCandidates(const StatusFacts& s, const LogFacts& f, char* d, size_t n) {
    // Rule 1. A statement about ONE line and nothing else. Its absence says nothing about whether a
    // level loaded or a bind was attempted, and inferring either here would be the false cause 4.1
    // row 4 was filed against, rebuilt in a new row.
    if (!f.csSpaceSeen) {
        _snprintf_s(d, n, _TRUNCATE,
                    "no [entity] cs-space candidates line in %.0f ms. Since W1-E R3 the emitter "
                    "keeps emitting on every candidate CHANGE after the bind until BOTH +0x2B08 "
                    "and +0x2B10 have rendered non-null, so silence here is not the sampler "
                    "stopping short of its own subject (AUDIT_L5_RUN_4 F11) -- it means no line "
                    "was ever emitted at all, and +0x2B08 is neither confirmed nor refuted here",
                    s.observedMs);
        return Ask::CouldNotAsk;
    }

    const char* const names[5] = {"+0x8", "+0x2B08", "+0x2B10", "game+0x7AF0", "game+0x7AF8"};
    const CsCandidate* const cand[5] = {&f.csSpaceAt8, &f.csSpaceAt2B08, &f.csSpaceAt2B10,
                                        &f.csSpaceAt7AF0, &f.csSpaceAt7AF8};
    const int kAt2B08 = 1, kAt2B10 = 2;

    // ARCH-11 S4: the count is recorded per candidate, because `cap=99 count=99` at +0x2B08 and
    // `cap=35 count=35` at game+0x7AF8 is the only thing that separates two spaces that both read
    // Ready. It is CARRIED, not parsed: `full` is the producer's own text and this reader holds no
    // second copy of FormatTable's format (ARCH-10 4.2). The two that decide nothing print their
    // short rendering so the detail stays inside one record field.
    char all[1408] = {0};
    for (int i = 0; i < 5; ++i) {
        const bool verbatim = (i == kAt2B08 || i == kAt2B10 || i == 4);
        char one[256] = {0};
        _snprintf_s(one, sizeof one, _TRUNCATE, "%s%s=%s", i == 0 ? "" : " | ", names[i],
                    !cand[i]->read      ? "(unparsed)"
                    : verbatim          ? cand[i]->full
                                        : cand[i]->rendered);
        strncat_s(all, sizeof all, one, _TRUNCATE);
    }

    // 4.3 rule 6. The two symbols are INFORMATION and this sentence is the whole of their effect:
    // R2 REJECTED `*(space+0x58) == *(game+0x8A48)` as a gate because both offsets are new and a
    // gate on an offset with no live confirmation can refuse a correct bind. Recording the pair --
    // agreeing or not -- is what earns the promotion. Nothing below reads `sym`.
    char sym[160] = {0};
    {
        const bool haveBoth = f.csSpaceSpawnSym[0] != '\0' && f.csSpaceLevelSym[0] != '\0';
        const bool numeric = haveBoth && IsPtr16(f.csSpaceSpawnSym) && IsPtr16(f.csSpaceLevelSym);
        _snprintf_s(sym, sizeof sym, _TRUNCATE,
                    " spawnsym=%s levelsym=%s (%s; information only, ARCH-12 R2 rejected this as a "
                    "gate).",
                    f.csSpaceSpawnSym[0] != '\0' ? f.csSpaceSpawnSym : "(not rendered)",
                    f.csSpaceLevelSym[0] != '\0' ? f.csSpaceLevelSym : "(not rendered)",
                    !numeric ? "no identity to compare"
                    : strcmp(f.csSpaceSpawnSym, f.csSpaceLevelSym) == 0 ? "AGREE"
                                                                        : "DISAGREE");
    }

    // Rule 2, first and unconditional, over EVERY sample. +0x8 is the CR15Game (1.1) and this
    // build's guard must refuse it. If it ever renders Ready the guard's sensitivity has changed,
    // and every verdict this row could otherwise reach -- including a confirmation -- was made by an
    // instrument that no longer refuses the wrong object. So they are not reached.
    if (f.csSpaceAt8ReadyEver) {
        _snprintf_s(d, n, _TRUNCATE,
                    "+0x8 rendered Ready on at least one sample. It is the CR15Game, not a game "
                    "space, and on this build the guard must refuse it: the guard's sensitivity has "
                    "changed and every candidate verdict in this launch is void. That sample: %s",
                    f.csSpaceAt8ReadyLine);
        return Ask::Fail;
    }

    // Rule 3. The two candidates the answer is made of must have rendered one of 4.2's three
    // shapes. Not having understood what the line said is a could-not-ask with a cause -- never a
    // verdict on the offset, and never a confirmation. The other two decide nothing, so an
    // unparsed one is reported as `(unparsed)` in the listing and refuses nothing.
    // ARCH-12 4.3, the RETROACTIVE control, and it runs before anything is read off the columns
    // that are present. A pre-amendment line has no `+0x2B10=` key at all, and the defect this
    // amendment exists to close is a parser that reads such a line as asked-pass because +0x2B08
    // said Ready. A missing column is not a verdict. +0x2B08's own rendering is still reported --
    // unchanged, and as information -- because that half of the line WAS asked and answered.
    if (!f.csSpaceAt2B10KeyPresent) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the cs-space candidates line carries NO +0x2B10 column, so the slot ARCH-12 "
                    "2.2/2.3 identifies as the LEVEL space -- and this row's subject -- was never "
                    "measured in this launch. That is a pre-ARCH-12 4.3 line, not an answer about "
                    "+0x2B10. The +0x2B08 half was asked and rendered %s, unchanged and reported "
                    "as information:%s %s",
                    f.csSpaceAt2B08.read ? f.csSpaceAt2B08.rendered : "(unparsed)", sym, all);
        return Ask::CouldNotAsk;
    }

    const CsCandidate* const deciders[3] = {&f.csSpaceAt8, &f.csSpaceAt2B08, &f.csSpaceAt2B10};
    const char* const deciderNames[3] = {"+0x8", "+0x2B08", "+0x2B10"};
    for (int i = 0; i < 3; ++i) {
        if (!deciders[i]->read) {
            _snprintf_s(d, n, _TRUNCATE,
                        "the cs-space candidates line was written but %s did not render <PTR16> "
                        "<TABLE>, 'null' or 'unreadable', so neither the guard's sensitivity nor "
                        "the accessor can be read off it: %s", deciderNames[i], f.csSpaceLine);
            return Ask::CouldNotAsk;
        }
    }

    // ARCH-11 S4, the NEW aliasing guard. If +0x2B08's pointer is also what another offset
    // rendered then the line distinguishes nothing, and a confirmation read off it would be a
    // confirmation of whatever object the two share. Fails REGARDLESS of the verdicts, and before
    // the pass, because it disqualifies the evidence rather than judging it.
    // ARCH-12 4.3 rule 5, the EXTENDED aliasing guard: asked-fail if +0x2B10's pointer equals any
    // of +0x8, +0x2B08, game+0x7AF0 or game+0x7AF8. It is R2's third arm read off this line -- the
    // netGame's two spaces have to be two OBJECTS, and if one pointer wears both labels then a
    // bind on +0x2B10 and a differential against +0x2B08 are one read done twice. Fails regardless
    // of the verdicts and before the pass, because it disqualifies the evidence rather than
    // judging it, and it is checked before the +0x2B08 guard so the message names this row's
    // subject when 2B08 and 2B10 are themselves the aliased pair.
    if (f.csSpaceAt2B10.ptr[0] != '\0' && f.csSpaceAt2B08.ptr[0] != '\0' &&
        strcmp(f.csSpaceAt2B08.ptr, f.csSpaceAt2B10.ptr) == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "ALIASED: +0x2B08 and +0x2B10 rendered the SAME pointer %s. ARCH-12 R2's "
                    "differential needs two objects and this line carries one, so neither the "
                    "level space nor the +0x2B08 arm can be read off it:%s %s",
                    f.csSpaceAt2B10.ptr, sym, all);
        return Ask::Fail;
    }

    // Every OTHER equality on this line is information, appended to the rendering and scoring
    // nothing. DIAG-L2 F7: L2's line had game+0x7AF0 carrying +0x2B08's value after a late publish,
    // which is the correct topology -- the same object reached through the netGame and through the
    // game -- and the wider guard failed the row for it.
    // At most three of the five slots can carry a note, and kAliasNote bounds each one.
    static const size_t kAliasNote = 144;
    char shared[kAliasNote * 3 + 1] = {0};
    for (int i = 0; i < 5; ++i) {
        if (i == kAt2B08 || i == kAt2B10 || cand[i]->ptr[0] == '\0') {
            continue;
        }
        const bool eq08 = f.csSpaceAt2B08.ptr[0] != '\0' &&
                          strcmp(cand[i]->ptr, f.csSpaceAt2B08.ptr) == 0;
        const bool eq10 = f.csSpaceAt2B10.ptr[0] != '\0' &&
                          strcmp(cand[i]->ptr, f.csSpaceAt2B10.ptr) == 0;
        if (eq08 || eq10) {
            char note[kAliasNote];
            _snprintf_s(note, sizeof note, _TRUNCATE,
                        " %s carries the same pointer as %s (%s), which is one object reached two "
                        "ways and not an aliased pair.", names[i], eq08 ? "+0x2B08" : "+0x2B10",
                        cand[i]->ptr);
            strncat_s(shared, sizeof shared, note, _TRUNCATE);
        }
    }
    // The notes go IN FRONT of the verbatim run, not after it. Appended, a line with five
    // full-length candidates pushed them past the end of the buffer and the row rendered the
    // equality it had just decided not to score -- silently, with no marker. This detail is the
    // only place F7's decision is visible, so it outranks the tail of the run it explains.
    if (shared[0] != '\0') {
        char merged[1408];
        _snprintf_s(merged, sizeof merged, _TRUNCATE, "%s%s", shared, all);
        strncpy_s(all, sizeof all, merged, _TRUNCATE);
    }

    // F3A-1: said out loud, and counted nowhere.
    char at8Note[176] = {0};
    if (strcmp(f.csSpaceAt8.verdict, "Empty") == 0) {
        _snprintf_s(at8Note, sizeof at8Note, _TRUNCATE,
                    " +0x8 rendered Empty -- the CR15Game's +0x420 reading all zero before its "
                    "RT-pool copy is written, which is a note about the guard's input and not a "
                    "second space.");
    }

    // ARCH-12 R2/R4's differential arm, reported with all four states rather than two. What this
    // LINE can settle is whether the netGame's two spaces are two distinct, separately judged
    // objects -- the lookup itself belongs to the camera row. +0x2B08 null is L3/L4's shape and is
    // `could-not-ask`: "the control could not be asked" is not "the control passed", and R4
    // forbids using either slot's readiness as a proxy for the other's.
    char diff[256] = {0};
    if (!f.csSpaceAt2B08.read) {
        _snprintf_s(diff, sizeof diff, _TRUNCATE,
                    " Differential could-not-ask: +0x2B08 did not render a 4.2 shape.");
    } else if (f.csSpaceAt2B08.ptr[0] == '\0') {
        _snprintf_s(diff, sizeof diff, _TRUNCATE,
                    " Differential could-not-ask: +0x2B08 rendered %s, so there is no second space "
                    "to differ from -- not a pass for that arm (ARCH-12 R4).",
                    f.csSpaceAt2B08.rendered);
    } else if (!f.csSpaceAt2B10.read) {
        _snprintf_s(diff, sizeof diff, _TRUNCATE,
                    " Differential could-not-ask: +0x2B10 did not render a 4.3 shape, so the pair "
                    "is one pointer and a hole.");
    } else if (f.csSpaceAt2B10.ptr[0] == '\0') {
        // AUDIT_F3H F3H-1: the "ok" sentence used to be guarded on +0x2B08 ALONE, so a real
        // emitted line with `+0x2B10=null` printed "two distinct non-null spaces" with an empty
        // pointer in the middle of it -- a pass word on a could-not-ask arm, contradicted by the
        // same line's own data, and it printed on the asked-fail side too.
        _snprintf_s(diff, sizeof diff, _TRUNCATE,
                    " Differential could-not-ask: +0x2B10 rendered %s, so there is no LEVEL space "
                    "to differ from -- +0x2B08 %s is one pointer, not a pair (ARCH-12 R4).",
                    f.csSpaceAt2B10.rendered, f.csSpaceAt2B08.ptr);
    } else {
        _snprintf_s(diff, sizeof diff, _TRUNCATE,
                    " Differential ok: +0x2B08 %s and +0x2B10 %s are two distinct non-null spaces, "
                    "and +0x2B08 rendered %s.",
                    f.csSpaceAt2B08.ptr, f.csSpaceAt2B10.ptr, f.csSpaceAt2B08.verdict);
    }

    // ARCH-12 4.3 rule 2. THE ROW'S SUBJECT IS NOW +0x2B10, and +0x2B08's verdict is reported but
    // does not decide it. `Empty` passes with `Ready` for 4.2 rule 2's own reason: a constructed
    // space whose table is empty is still the space the accessor names.
    if (strcmp(f.csSpaceAt2B10.verdict, "Ready") == 0 ||
        strcmp(f.csSpaceAt2B10.verdict, "Empty") == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "+0x2B10 rendered %s on the last of %u sample(s): the netGame's LEVEL space is "
                    "present and judged, which is what ARCH-12 2.2/2.3 says the spectator-camera "
                    "CS is registered in.%s%s%s game+0x7AF* are other spaces the engine owns, "
                    "information and not rivals -- and never a fallback (ARCH-11 S4): %s",
                    f.csSpaceAt2B10.verdict, f.csSpaceCount, diff, at8Note, sym, all);
        return Ask::Pass;
    }

    // ARCH-12 4.3 rule 3 read against S6's launch table, which is the same memo being specific:
    // with no map loaded `*(ng+0x2B10)` is null and there is NO negative answer, only an absent
    // question -- S6 files L3/L4 as could-not-ask in as many words. So `null` splits on whether a
    // level was ever reached in this launch, and only the arm where the map DID load is a refusal.
    // Reading rule 3 two-state instead would file asked-fail against every launch that never
    // loaded a level, which is the two-state gate BRIEF S39 forbids.
    if (strcmp(f.csSpaceAt2B10.rendered, "null") == 0 && !LevelReached(f)) {
        _snprintf_s(d, n, _TRUNCATE,
                    "+0x2B10 rendered null on the last of %u sample(s) and no [session] NetGame "
                    "state 7/8/9 was seen, so no map was loaded and there was no level space to "
                    "ask about (ARCH-12 S6, L3/L4). Not a refusal of +0x2B10.%s%s %s",
                    f.csSpaceCount, diff, sym, all);
        return Ask::CouldNotAsk;
    }

    // Rule 3 proper. Quotes all five verdicts verbatim, per 4.3. It is not a claim that another
    // offset is right: no candidate here can carry that, and no runtime fallback is authorised
    // (ARCH-10 S1.4, untouched by ARCH-12).
    _snprintf_s(d, n, _TRUNCATE,
                "+0x2B10 rendered %s on the last of %u sample(s)%s, so the netGame's LEVEL space "
                "-- this row's subject -- is not confirmed in this launch.%s%s%s No other "
                "candidate answers that: +0x8 is the CR15Game and game+0x7AF* are game-level "
                "spaces the product may not use (ARCH-11 S4): %s",
                f.csSpaceAt2B10.rendered, f.csSpaceCount,
                LevelReached(f) ? " with a level loaded" : "", diff, at8Note, sym, all);
    return Ask::Fail;
}

// W2-R/W2-F: the BROWSER arm opened the product's browser over a loaded level.
//
// * B2. This is an ORDER assertion and it used to be a conjunction -- `a level
// was ever seen` latched over the whole log AND `the browser is open now` -- which passed L5 re-run
// 3, where OpenBrowser() was called at 5774 ms and the level did not begin loading until 13.6 s
// LATER. The row said "over the level the profile loaded" about a browser opened over the MAIN
// MENU.
//
// WHICH ORDER, and why it is `opened after` and not `still open after`. The owner's product is the
// viewer running over a loaded level, and both readings describe that end state -- but only one of
// them is a statement about THIS launch's arm. The tick opens the browser exactly once and only
// when it believes a level is up, so an open that lands before the level stamp is direct evidence
// that the gate read the wrong level predicate, which is the defect ARCH-10 4.1 row 4 and this run
// both produced. `still open after` would have greened L5 re-run 3 on the same bytes. The other
// claim -- that the panel SURVIVED a level load -- is real and worth having, and S8.4a.1 says it
// may be its own row; it may not wear this one's name.
inline Ask AskBrowserOverLevel(const StatusFacts& s, const LogFacts& f, char* d, size_t n) {
    if (!LevelReached(f)) {
        char why[kRowLineCap] = {0};
        LevelLoadReason(f, why, sizeof why);
        _snprintf_s(d, n, _TRUNCATE, "%s", why);
        return Ask::CouldNotAsk;
    }
    if (!s.browserOpenRequested) {
        char why[kRowLineCap] = {0};
        LevelLoadReason(f, why, sizeof why);
        _snprintf_s(d, n, _TRUNCATE,
                    "a level loaded but ui::OpenBrowser was never called: %s. %s", why,
                    s.browserReason);
        return Ask::CouldNotAsk;
    }
    double levelMs = 0.0;
    char stampWhy[kRowLineCap] = {0};
    if (!s.browserOpenedAtKnown || !LevelLoadMonotonicMs(f, &levelMs, stampWhy, sizeof stampWhy)) {
        // could-not-ask NAMING WHICH stamp is missing (S8.4a.1). Never a comparison: the level
        // stamp is a log wall-clock time and browserOpenedAtMs is launch-relative, so without the
        // SURVIVE anchor that carries both there is no order to claim, only a units bug.
        _snprintf_s(d, n, _TRUNCATE,
                    "the browser is open, but this row orders two stamps and one of them cannot "
                    "be put on the observer's epoch: browserOpenedAtMs %s, level-load line stamp "
                    "%s, [observer] SURVIVE clock anchor %s (%u line(s)). %s%s%s",
                    s.browserOpenedAtKnown ? "present" : "MISSING",
                    f.levelStampKnown ? "present" : "MISSING",
                    f.armEpochKnown ? "present" : "MISSING", f.armEpochLines,
                    stampWhy[0] != '\0' ? stampWhy : "", stampWhy[0] != '\0' ? ". " : "",
                    s.browserReason);
        return Ask::CouldNotAsk;
    }
    if (!s.browserOpen) {
        _snprintf_s(d, n, _TRUNCATE,
                    "ui::OpenBrowser was called over a loaded level and ui::BrowserOpen() is still "
                    "false: %s", s.browserReason);
        return Ask::Fail;
    }
    if (s.browserOpenedAtMs <= levelMs) {
        _snprintf_s(d, n, _TRUNCATE,
                    "OUT OF ORDER: ui::OpenBrowser() was called at %.0f ms on the observer's epoch "
                    "and the level did not begin loading until %.0f ms on that same epoch (log line "
                    "'%s', converted through the SURVIVE anchor '%s'). The browser was opened over "
                    "whatever was on screen before the level, so this launch does not show the "
                    "viewer opened over a loaded level.",
                    s.browserOpenedAtMs, levelMs, f.levelStampLine, f.armEpochLine);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "IN ORDER: level-load stamp %.0f ms, ui::OpenBrowser() at %.0f ms (%.0f ms later), "
                "both on the observer's epoch via the SURVIVE anchor '%s'; and the browser is still "
                "open. %s",
                levelMs, s.browserOpenedAtMs, s.browserOpenedAtMs - levelMs, f.armEpochLine,
                s.browserReason);
    return Ask::Pass;
}

// * AUDIT_L5_RUN3 section 12 item 2, second half: `panel::Draw` has exactly ONE non-test caller,
// so on L5 re-run 3 NOTHING drew the replay browser panel and `PanelVisible()=1` in that record was
// STATE. This row asks the product question -- was the panel actually DRAWN over the loaded level
// -- and it is written so that no amount of state can answer it: it reads counters, taken as a
// DIFFERENCE across a stamp, and reads `panelVisible` / `browserOpen` nowhere.
//
// WHICH counters is ARCH-15 S4.1-S4.5, and it is the third answer this row has had:
//
//   * `marshalledDraws` (the first) is a DIAGNOSTIC feeding no gate -- AUDIT_F3J F3J-1 drove this
//     row with W2-F's own test case 1 (registration never lands, the panel draws on all 4000 owner
//     ticks) and got `NOTHING DREW THE PANEL`, the launch this row exists to describe, backwards.
//   * `ownerDraws` (the second, F3J-1's direction) is STRUCK by ARCH-15 S4.1: it counts draw
//     ATTEMPTS at the stage-0 drain, and RES-W1M-C0 proves the drain is dispatched strictly BEFORE
//     the only site in the image that opens a GUI frame, so every attempt is refused inside
//     `NRadGui::Begin`. `ownerDraws` reads 1:1 with `ownerTicks` on a build that draws nothing.
//   * `inFrameDraws` over `frameSiteEntries` (ARCH-15 S4.2/S4.4, this row) cannot be true unless
//     our widgets entered the engine's OWN GUI frame: the byte it gates on is written only by
//     `StartFrame` and cleared only by `EndFrame`.
//
// And `refusedDraws > 0` after the stamp is an asked-FAIL in its own right, ahead of the coverage
// margin: a refused draw is never a draw, and the one tick of coverage slack is for a sample that
// straddles an entry boundary, not for a refusal that has its own name.
//
// The window is the INTERSECTION of the level-load stamp and the panel's closed->open stamp
// (AUDIT_F3J F3J-2), because 600 closed ticks after a level load land in the denominator and in no
// numerator, and a panel that drew on every tick it was open then reads as a shortfall. The probe
// takes all four baselines at the LATER of the two stamps, and never subtracts one epoch's sample
// from another's (ARCH-15 S4.3).
// ★ THE SPLIT, COPIED THE WAY CopyGuiGateFacts COPIES ITS TWENTY: a template, so this header pulls
// in no product code, while probe_replay.cpp instantiates it on the real csregister::Report and a
// field that was renamed or retyped fails to COMPILE there instead of publishing a zero nobody
// measured. The test drives it with a Report double holding a distinct value in every field, which
// is the only fixture on which a swapped or a dropped assignment can fail.
template <class ReportT>
inline void CopyDrawPhaseFacts(const ReportT& r, StatusFacts& s) {
    s.csDrawPhasesKnown = true;
    s.csImguiNanos = r.imguiNanos;
    s.csBrowserScanNanos = r.browserScanNanos;
    s.csMetadataReadNanos = r.metadataReadNanos;
    s.csTextFormatNanos = r.textFormatNanos;
    s.csBrowserIoOnDrawThread = r.browserIoOnDrawThread;
    s.csGuiItemsPerFrame = r.guiItemsPerFrame;
    s.csGuiGlyphsPerFrame = r.guiGlyphsPerFrame;
    s.csGuiStreamFrames = r.guiStreamFrames;
    s.csDrawNanosP99 = r.drawNanosP99;
    s.csDrawNanosP999 = r.drawNanosP999;
    s.csDrawNanosSamples = r.drawNanosSamples;
    s.csDrawNanosOverHistogram = r.drawNanosOverHistogram;
}

// The 200 us ceiling had no term a run record could trip: the only
// enforcement was a person reading drawNanosMax in a report, which is how L5 #7 s 629 300 was
// found -- by hand, one launch late. This is that term. The ceiling is NOT written down here:
// it travels on the record as csDrawNanosBudget, taken from inframe::kDrawNanosBudgetNs by the
// build that did the measuring, so this reader cannot score a record against a ceiling its
// build never had, and an older record without the field is could-not-ask rather than a pass.
//
// firstDrawNanos is REPORTED, never scored. The first draw builds the caches every later draw
// reuses and is excluded from the maximum by src/ui/inframe.cpp; printing it beside the maximum
// is what lets a reader tell a warm-up from a tail that was laundered out of the number.
inline Ask AskDrawBudget(const StatusFacts& s, const LogFacts& f, char* d, size_t n) {
    typedef unsigned long long ull;
    (void)f;
    // The attribution travels with the verdict on EVERY answer that has a measurement in it. A
    // breach that does not name a phase is the report L5 #7 was found in, one launch late.
    char phases[160];
    if (s.csDrawPhasesKnown) {
        _snprintf_s(phases, sizeof phases, _TRUNCATE,
                    "; imgui=%llu scan=%llu meta=%llu text=%llu drawThreadIo=%llu",
                    (ull)s.csImguiNanos, (ull)s.csBrowserScanNanos, (ull)s.csMetadataReadNanos,
                    (ull)s.csTextFormatNanos, (ull)s.csBrowserIoOnDrawThread);
    } else {
        strncpy_s(phases, sizeof phases, "; phase split ABSENT from this record", _TRUNCATE);
    }
    // The stream that PRODUCED the nanoseconds, and the distribution the maximum is the tail of.
    // A maximum with no items beside it cannot tell a slow engine from a fat page, which is the
    // reading L5 #11 could not settle; a maximum with no percentiles beside it cannot tell one
    // scheduler outlier from a draw that is over budget every frame.
    char stream[256];
    stream[0] = '\0';
    if (s.csDrawPhasesKnown) {
        char tail[112];
        _snprintf_s(tail, sizeof tail, _TRUNCATE,
                    "p99<=%llu p999<=%llu of %llu sample(s), %llu past the histogram",
                    (ull)s.csDrawNanosP99, (ull)s.csDrawNanosP999, (ull)s.csDrawNanosSamples,
                    (ull)s.csDrawNanosOverHistogram);
        if (s.csGuiStreamFrames != 0) {
            _snprintf_s(stream, sizeof stream, _TRUNCATE, "; items=%llu glyphs=%llu over %llu "
                        "browser frame(s); %s",
                        (ull)s.csGuiItemsPerFrame, (ull)s.csGuiGlyphsPerFrame,
                        (ull)s.csGuiStreamFrames, tail);
        } else {
            _snprintf_s(stream, sizeof stream, _TRUNCATE,
                        "; NO BROWSER FRAME WAS DRAWN (guiStreamFrames=0), so the item and glyph "
                        "counts are unmeasured rather than zero; %s", tail);
        }
    }
    if (!s.csAsked) {
        _snprintf_s(d, n, _TRUNCATE,
                    "csregister::Status_() was never read, so there is no draw timing on this "
                    "record -- absent is not a draw that came in under budget");
        return Ask::CouldNotAsk;
    }
    if (s.csDrawNanosBudget == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the record carries no drawNanosBudget field, so the build that measured "
                    "drawNanosMax=%llu never named the ceiling it was measured against and this "
                    "row has no denominator to score it with",
                    (ull)s.csDrawNanosMax);
        return Ask::CouldNotAsk;
    }
    if (!s.csInFrameKnown || s.csInFrameDraws == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "no draw was timed on this launch (inFrameDraws %s), so drawNanosMax=%llu is "
                    "the initial value of a counter and not a measurement of anything",
                    s.csInFrameKnown ? "0" : "absent", (ull)s.csDrawNanosMax);
        return Ask::CouldNotAsk;
    }
    if (s.csInFrameDraws == 1) {
        // One draw and it IS the first draw: the steady state this budget is about has not
        // happened yet, and scoring the warm-up as the steady state is the very substitution
        // this row exists to make visible.
        _snprintf_s(d, n, _TRUNCATE,
                    "exactly one draw was timed, and the first draw is excluded from "
                    "drawNanosMax by construction, so there is no steady-state maximum yet "
                    "(firstDrawNanos=%llu, ceiling %llu)",
                    (ull)s.csFirstDrawNanos, (ull)s.csDrawNanosBudget);
        return Ask::CouldNotAsk;
    }
    if (s.csDrawNanosMax > s.csDrawNanosBudget) {
        _snprintf_s(d, n, _TRUNCATE,
                    "OVER BUDGET: drawNanosMax=%llu > %llu ns over %llu timed draw(s) "
                    "(firstDrawNanos=%llu, excluded from the maximum; drawNanosTotal=%llu)%s%s",
                    (ull)s.csDrawNanosMax, (ull)s.csDrawNanosBudget, (ull)s.csInFrameDraws,
                    (ull)s.csFirstDrawNanos, (ull)s.csDrawNanosTotal, phases, stream);
        return Ask::Fail;
    }
    // ** A BREACH IS A BREACH WITHOUT THE SPLIT -- that is why this comes AFTER the comparison
    // above and not before it. A PASS is different: drawNanosMax alone cannot tell a draw that
    // opens no file from a draw that opened one and found it in the page cache, and 0 is the
    // answer this row was built to check. Absent is could-not-ask, never a clean bill.
    if (!s.csDrawPhasesKnown) {
        _snprintf_s(d, n, _TRUNCATE,
                    "drawNanosMax=%llu is inside the %llu ns ceiling, but this record carries no "
                    "phase split and no browserIoOnDrawThread, so nothing here can say WHERE the "
                    "time went or whether a file was opened on the thread that drew",
                    (ull)s.csDrawNanosMax, (ull)s.csDrawNanosBudget);
        return Ask::CouldNotAsk;
    }
    if (s.csBrowserIoOnDrawThread != 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "FILE I/O ON THE RENDER THREAD: browserIoOnDrawThread=%llu opens were charged "
                    "to the thread that draws. drawNanosMax=%llu is inside the %llu ns ceiling on "
                    "this run and a cold folder would put it outside%s%s",
                    (ull)s.csBrowserIoOnDrawThread, (ull)s.csDrawNanosMax,
                    (ull)s.csDrawNanosBudget, phases, stream);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "drawNanosMax=%llu <= %llu ns over %llu timed draw(s) "
                "(firstDrawNanos=%llu, excluded from the maximum; drawNanosTotal=%llu)%s%s",
                (ull)s.csDrawNanosMax, (ull)s.csDrawNanosBudget, (ull)s.csInFrameDraws,
                (ull)s.csFirstDrawNanos, (ull)s.csDrawNanosTotal, phases, stream);
    return Ask::Pass;
}

inline Ask AskPanelDrawnOverLevel(const StatusFacts& s, const LogFacts& f, char* d, size_t n) {
    typedef unsigned long long ull;
    if (!s.csAsked) {
        _snprintf_s(d, n, _TRUNCATE,
                    "csregister::Status_() was never read, so the draw counters are "
                    "absent -- which is not the same answer as a draw counter reading zero");
        return Ask::CouldNotAsk;
    }
    if (!LevelReached(f)) {
        char why[kRowLineCap] = {0};
        LevelLoadReason(f, why, sizeof why);
        _snprintf_s(d, n, _TRUNCATE, "%s, so there is no level for a draw to have landed over", why);
        return Ask::CouldNotAsk;
    }
    if (s.csOwnerTicksKnown && s.csOwnerTicks == 0) {
        // ARCH-15 S4.5, row 1. Our tick never ran, so nothing downstream of it is a measurement.
        // Stated over the LIFETIME total, which is one epoch and needs no subtraction.
        _snprintf_s(d, n, _TRUNCATE,
                    "CENSORED (ARCH-15 S4.5): our stage-0 tick never ran on the owner thread at "
                    "all over this launch, so the module never armed anything and no count "
                    "downstream of it is a measurement");
        return Ask::CouldNotAsk;
    }
    if (!s.csInFrameKnown) {
        // ARCH-15 S4.5, last row: an ABSENT field is could-not-ask NAMING the field. Absent is not
        // zero, and the two counters this record does carry were struck as vacuous by S4.1 -- so
        // there is no fallback to make, and saying so is the whole answer.
        _snprintf_s(d, n, _TRUNCATE,
                    "DL-07's coverage term is NOT EVALUABLE on this record: it carries no "
                    "frameSiteEntries, inFrameDraws or refusedDraws field, and the prerequisite is "
                    "ARCH-15-C1 -- W2-F-C1 installing the row-2.5 detour and publishing them. What "
                    "it does carry, ARCH-15 S4.1 struck as vacuous: marshalledDraws=%llu and "
                    "ownerDraws over %llu owner tick(s) both count draw ATTEMPTS at the stage-0 "
                    "drain, which RES-W1M-C0 shows is dispatched before any GUI frame is open, so "
                    "every one of them is refused inside NRadGui::Begin",
                    (ull)s.csMarshalledDraws, (ull)s.csOwnerTicks);
        return Ask::CouldNotAsk;
    }
    if (s.csInFramePublished && !s.csInFrameLineAgrees) {
        // The struct read and the module's own published group are of the same counters at the
        // same instant. When they differ, the probe read something else -- the commonest way being
        // a sampler wired back to marshalledDraws -- and a number whose provenance is in doubt is
        // not a measurement. Censored, never scored either way (ARCH-15 S4.3).
        _snprintf_s(d, n, _TRUNCATE,
                    "INSTRUMENT FAULT (ARCH-15 S4.3): the row-2.5 counters read out of the Report "
                    "DISAGREE with the same counters as the module published them in its "
                    "`[inframe ...]` group, so one of the two readings is not of this site and "
                    "neither can be scored; the Report read gave entries=%llu draws=%llu "
                    "refused=%llu closed=%llu fallback=%llu installed=%d",
                    (ull)s.csFrameSiteEntries, (ull)s.csInFrameDraws, (ull)s.csRefusedDraws,
                    (ull)s.csPanelClosedEntries, (ull)s.csFallbackEntries,
                    s.csInFrameInstalled ? 1 : 0);
        return Ask::CouldNotAsk;
    }
    if (s.csEnableStampsKnown && s.csEnableStamps == 0) {
        // AUDIT_F3J F3J-2: `enableStamps == 0` is could-not-ask, not a zero. A panel that never
        // opened was never asked to draw, and a window nobody opened is not a window it failed in.
        _snprintf_s(d, n, _TRUNCATE,
                    "the panel never opened over this launch (enableStamps == 0, the closed->open "
                    "transition count), so the window this row scores was never entered: %llu "
                    "row-2.5 entry(s) and %llu in-frame draw(s) over the whole launch are a "
                    "different claim from 'drawn over the loaded level'",
                    (ull)s.csFrameSiteEntries, (ull)s.csInFrameDraws);
        return Ask::CouldNotAsk;
    }
    if (!s.csInFrameBaselineKnown) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the row-2.5 counters were never sampled at the LATER of the level-load stamp "
                    "and the panel's closed->open stamp, so 'drawn AFTER the level' has no "
                    "baseline to be a difference from; %llu in-frame draw(s) over %llu entry(s) "
                    "for the whole launch is a different claim (AUDIT_F3J F3J-2)",
                    (ull)s.csInFrameDraws, (ull)s.csFrameSiteEntries);
        return Ask::CouldNotAsk;
    }
    if (s.csFrameSiteEntries < s.csFrameSiteEntriesAtBaseline ||
        s.csInFrameDraws < s.csInFrameDrawsAtBaseline ||
        s.csRefusedDraws < s.csRefusedDrawsAtBaseline ||
        s.csPanelClosedEntries < s.csPanelClosedEntriesAtBaseline) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the row-2.5 counters went BACKWARDS across the %s stamp (entries %llu -> %llu, "
                    "inFrameDraws %llu -> %llu, refusedDraws %llu -> %llu, panelClosed %llu -> "
                    "%llu): the module was reset under the observer, so the difference is not a "
                    "measurement",
                    s.csInFrameBaselineStamp[0] != '\0' ? s.csInFrameBaselineStamp : "window",
                    (ull)s.csFrameSiteEntriesAtBaseline, (ull)s.csFrameSiteEntries,
                    (ull)s.csInFrameDrawsAtBaseline, (ull)s.csInFrameDraws,
                    (ull)s.csRefusedDrawsAtBaseline, (ull)s.csRefusedDraws,
                    (ull)s.csPanelClosedEntriesAtBaseline, (ull)s.csPanelClosedEntries);
        return Ask::CouldNotAsk;
    }
    const ull entriesAfter = s.csFrameSiteEntries - s.csFrameSiteEntriesAtBaseline;
    const ull drewAfter = s.csInFrameDraws - s.csInFrameDrawsAtBaseline;
    const ull refusedAfter = s.csRefusedDraws - s.csRefusedDrawsAtBaseline;
    const ull closedAfter = s.csPanelClosedEntries - s.csPanelClosedEntriesAtBaseline;
    if (entriesAfter == 0) {
        // ARCH-15 S4.5, row 2: asked-FAIL on the ROW-2.5 INSTALL, could-not-ask on the PANEL. This
        // row is the panel's, so it refuses -- and it names the install, because "the detour never
        // ran" and "the detour ran and drew nothing" are opposite findings.
        _snprintf_s(d, n, _TRUNCATE,
                    "the row-2.5 draw site was never ENTERED after the %s stamp (0 entries), so "
                    "DL-07 is NOT EVALUABLE on this window and the failure is the INSTALL, not the "
                    "panel: inframeInstalled=%d%s%s. %llu fallback owner-tick attempt(s) are not a "
                    "substitute -- ARCH-15 S4.1 struck those as refused before they reach a widget",
                    s.csInFrameBaselineStamp[0] != '\0' ? s.csInFrameBaselineStamp : "window",
                    s.csInFrameInstalled ? 1 : 0,
                    s.csInFrameStatus[0] != '\0' ? ", status " : "",
                    s.csInFrameStatus[0] != '\0' ? s.csInFrameStatus : "",
                    (ull)s.csFallbackEntries);
        return Ask::CouldNotAsk;
    }
    if (s.csInFrameInstalled && drewAfter + refusedAfter + closedAfter != entriesAfter) {
        // ARCH-15 S4.3's published invariant, over the scored window. With the detour armed there
        // are no fallback entries to absorb the difference, so a record where the parts do not sum
        // to the entries is an INSTRUMENT FAULT and not a measurement of anything.
        _snprintf_s(d, n, _TRUNCATE,
                    "INSTRUMENT FAULT (ARCH-15 S4.3): over the window the row-2.5 parts do not sum "
                    "to its entries -- %llu in-frame + %llu refused + %llu panel-closed = %llu, "
                    "against %llu entry(s), with the detour reporting itself armed. A record where "
                    "the published invariant does not hold is not a measurement",
                    drewAfter, refusedAfter, closedAfter, drewAfter + refusedAfter + closedAfter,
                    entriesAfter);
        return Ask::CouldNotAsk;
    }
    if (refusedAfter > 0) {
        // ARCH-15 S4.5, row 3, widened to any refusal: A REFUSED DRAW IS NEVER A DRAW. Scored
        // AHEAD of the coverage margin so a single refusal cannot ride the one entry of slack --
        // the slack is for a sample taken between an entry and its draw, and a refusal is not that.
        _snprintf_s(d, n, _TRUNCATE,
                    "THE DRAW WAS REFUSED AT THE SITE ON %llu OF %llu ROW-2.5 ENTRIES after the %s "
                    "stamp (reason FrameClosedAtSite): the site was reached and the frame was shut, "
                    "which is a NEGATIVE ANSWER and not an absent question. %llu entry(s) drew "
                    "in-frame. Refusal breakdown over the launch: noFrameByte=%llu noLatch=%llu "
                    "noContext=%llu byFacade=%llu. PanelEnabled()=%d PanelVisible()=%d "
                    "BrowserOpen()=%d are state, and this row scores none of them",
                    refusedAfter, entriesAfter,
                    s.csInFrameBaselineStamp[0] != '\0' ? s.csInFrameBaselineStamp : "window",
                    drewAfter, (ull)s.csRefusedNoFrameByte, (ull)s.csRefusedNoLatch,
                    (ull)s.csRefusedNoContext, (ull)s.csRefusedByFacade,
                    s.panelEnabled ? 1 : 0, s.panelVisible ? 1 : 0, s.browserOpen ? 1 : 0);
        return Ask::Fail;
    }
    if (drewAfter + 1 >= entriesAfter) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the panel was DRAWN INSIDE THE ENGINE'S OWN GUI FRAME on every row-2.5 entry "
                    "that ran: %llu/%llu inFrameDraws/frameSiteEntries after the %s stamp, 0 "
                    "refused, %llu entry(s) at which the panel was closed. State, reported and "
                    "scored by nothing: PanelEnabled()=%d PanelVisible()=%d BrowserOpen()=%d",
                    drewAfter, entriesAfter,
                    s.csInFrameBaselineStamp[0] != '\0' ? s.csInFrameBaselineStamp : "window",
                    closedAfter, s.panelEnabled ? 1 : 0, s.panelVisible ? 1 : 0,
                    s.browserOpen ? 1 : 0);
        return Ask::Pass;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "THE PANEL DREW IN-FRAME ON ONLY %llu OF %llu ROW-2.5 ENTRIES over the loaded "
                "level: the site ran and the draw did not land on %llu of them, and none of those "
                "was a named refusal -- %llu were entries at which the panel was CLOSED. A "
                "middling ratio is an asked-FAIL naming both counts (ARCH-15 S4.4 term 3). "
                "PanelEnabled()=%d PanelVisible()=%d BrowserOpen()=%d are state, and this row "
                "scores none of them. %s",
                drewAfter, entriesAfter, entriesAfter - drewAfter, closedAfter,
                s.panelEnabled ? 1 : 0, s.panelVisible ? 1 : 0, s.browserOpen ? 1 : 0, s.csStatus);
    return Ask::Fail;
}

// * ARCH-15 S5.3 and S7, and RES-W1M-C0 S6. THE FRAME-OPEN ROW. RES-W1M-C0 establishes, twice and
// independently, that our stage-0 owner tick is dispatched at `0x140127454`, strictly BEFORE the
// only site in the image that opens a GUI frame, and that `NRadGui::Begin` tests
// `GUI.FRAME_OPEN_BYTE` and early-returns when it is 0. This row is that claim put IN VIVO, on a
// sample the plugin already takes at the top of the drain -- three loads, no call, no lock.
//
// THE PREDICTION IS FILED IN ADVANCE SO THE ROW CAN FAIL: `frameOpenAtOwnerTick == 0` on every
// tick. A non-zero sample falsifies RES-W1M-C0 and ARCH-15 with it, so it is the asked-FAIL and it
// names the tick it happened on; anyone who sees it should suspect the instrument before believing
// the result.
//
// ★ THE POLARITY. ARCH-15 S5.3's own verdict table files the zero as an asked-FAIL ("the static
// result confirmed in vivo"). The C1 briefing RE-RULED that, and this row implements the re-ruling:
// the drain being outside the frame is NOT a failure of the product -- it is the FACT THE IN-FRAME
// SITE EXISTS FOR, and a row that files the designed-for, predicted-in-advance observation as a
// permanent RED is a red everyone learns to scroll past. The falsifiable content is unchanged and
// only its sign moved: the row still fails, loudly, on exactly the observation that would prove
// the design wrong. An amendment to ARCH-15 S5.3's table is OWED for this.
inline Ask AskFrameOpenAtStage0(const StatusFacts& s, char* d, size_t n) {
    typedef unsigned long long ull;
    if (!s.csFrameOpenSampled) {
        _snprintf_s(d, n, _TRUNCATE,
                    "no GUI.FRAME_OPEN_BYTE sample is in this record: the plugin publishes it at "
                    "the top of the stage-0 drain once per owner tick, and a record written before "
                    "that sample existed carries no field to read. Absent is not zero, and a zero "
                    "invented here would be the in-vivo confirmation of RES-W1M-C0 fabricated out "
                    "of the record's age");
        return Ask::CouldNotAsk;
    }
    if (s.csOwnerTicksSampled == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the sample was never taken on a single owner tick, so the byte was never read "
                    "at the moment this row is about: our stage-0 drain did not run");
        return Ask::CouldNotAsk;
    }
    if (s.csStage0Published && !s.csStage0LineAgrees) {
        _snprintf_s(d, n, _TRUNCATE,
                    "INSTRUMENT FAULT: the struct read of csregister::Report and the module's own "
                    "published [stage0 frameopen ...] group DISAGREE (samples=%llu nonzero=%llu "
                    "lasttick=%llu was read off the Report). Two readings of one counter, taken "
                    "from one module, cannot both be of this launch, and neither is scored",
                    (ull)s.csOwnerTicksSampled, (ull)s.csFrameOpenAtOwnerTick,
                    (ull)s.csFrameOpenLastNonZeroTick);
        return Ask::CouldNotAsk;
    }
    if (s.csGuiPresenceKnown && (!s.csGuiContextPresent || !s.csWindowGatePresent)) {
        // ARCH-15 S5.3: there was no GUI to be inside of. A frame byte read against an absent
        // context is a read of whatever that address happens to hold.
        _snprintf_s(d, n, _TRUNCATE,
                    "there was no GUI to be inside of over %llu owner tick(s): guiContextPresent=%d "
                    "windowGatePresent=%d. The frame byte is only a fact about a frame when the "
                    "context and the gate both resolved, so this is could-not-ask and not a zero",
                    (ull)s.csOwnerTicksSampled, s.csGuiContextPresent ? 1 : 0,
                    s.csWindowGatePresent ? 1 : 0);
        return Ask::CouldNotAsk;
    }
    char site[260];
    if (!s.csInFrameKnown) {
        _snprintf_s(site, sizeof site, _TRUNCATE,
                    "The row-2.5 site's own sample is NOT in this record (ARCH-15-C1 / W2-F-C1 has "
                    "not landed), so the other half of the comparison is unasked");
    } else if (s.csFrameSiteEntries == 0) {
        _snprintf_s(site, sizeof site, _TRUNCATE,
                    "The row-2.5 site was never entered (inframeInstalled=%d), so it read the byte "
                    "no times", s.csInFrameInstalled ? 1 : 0);
    } else if (s.csRefusedNoFrameByte == 0) {
        _snprintf_s(site, sizeof site, _TRUNCATE,
                    "AND the row-2.5 site read the byte as 1 on all %llu of its entries (%llu "
                    "in-frame draw(s), 0 refused for a closed frame): the same byte, sampled at "
                    "the two sites, answers 0 at the drain and 1 inside the frame, which is the "
                    "whole reason the site was moved",
                    (ull)s.csFrameSiteEntries, (ull)s.csInFrameDraws);
    } else {
        _snprintf_s(site, sizeof site, _TRUNCATE,
                    "The row-2.5 site read the byte as 0 on %llu of its %llu entries, which is a "
                    "SECOND finding and belongs to w2r-panel-drawn-over-level",
                    (ull)s.csRefusedNoFrameByte, (ull)s.csFrameSiteEntries);
    }
    if (s.csOffThreadSiteEntries != 0) {
        char off[112];
        _snprintf_s(off, sizeof off, _TRUNCATE,
                    " The row-2.5 site was also entered %llu time(s) off the owner thread; no"
                    " draw is attempted there, so it is a guard counter and not a verdict.",
                    (ull)s.csOffThreadSiteEntries);
        strncat_s(site, sizeof site, off, _TRUNCATE);
    }
    if (s.csFrameOpenAtOwnerTick > 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "GUI.FRAME_OPEN_BYTE WAS NON-ZERO AT THE STAGE-0 DRAIN on %llu of %llu owner "
                    "tick(s), most recently on tick %llu. RES-W1M-C0 S6 predicted zero on every tick, and "
                    "ARCH-15's choice of the row-2.5 site rests on that prediction: this "
                    "observation FALSIFIES it, and the instrument should be suspected before the "
                    "result is believed. startFrameLatchPresent=%d. %s",
                    (ull)s.csFrameOpenAtOwnerTick, (ull)s.csOwnerTicksSampled,
                    (ull)s.csFrameOpenLastNonZeroTick,
                    s.csGuiPresenceKnown && s.csStartFrameLatchPresent ? 1 : 0, site);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "THE STAGE-0 DRAIN IS OUTSIDE THE ENGINE'S GUI FRAME: GUI.FRAME_OPEN_BYTE read 0 "
                "on all %llu owner tick(s) sampled. The sample is only taken where the byte "
                "resolved, so %llu is also how many ticks the address answered on "
                "(guiPresenceBitsPublished=%d). This is RES-W1M-C0 S6 confirmed in vivo and "
                "it is NOT a failure of the product -- it is the fact the row-2.5 in-frame site "
                "exists for, and it is why a draw issued from this tick can only be refused. %s",
                (ull)s.csOwnerTicksSampled, (ull)s.csOwnerTicksSampled,
                s.csGuiPresenceKnown ? 1 : 0, site);
    return Ask::Pass;
}

// * AUDIT_L5_RUN3 M6 / section 12 item 10. L5 re-run 3 printed TWELVE `[camera] camera control
// refused` lines over one launch and no row scored one of them, so the whole camera path was
// invisible to the record's verdict. This row carries both halves: what camera_control::Probe
// answered on the tick (its header: "No engine call and no write: safe on every tick"), and the
// refusal's own sentence.
//
// The LOG half is scored ahead of the probe half deliberately: a build whose tick never called
// Probe still printed those twelve lines, and a row that shrugged because its newer field was
// missing would be reporting its own age rather than the launch's camera.
inline Ask AskCameraRigProbe(const StatusFacts& s, const LogFacts& f, char* d, size_t n) {
    char probeNote[240] = {0};
    _snprintf_s(probeNote, sizeof probeNote, _TRUNCATE,
                "camera_control::Probe %s%s%s%s",
                s.cameraProbed ? "last answered " : "was never called on this arm's tick",
                s.cameraProbed ? s.cameraProbeResult : "",
                s.cameraProbed && s.cameraProbeReadyEver ? " (Ready at least once)" : "",
                s.cameraProbeDiagnostic[0] != '\0' ? "; Diagnostic(): " : "");
    if (s.cameraProbeDiagnostic[0] != '\0') {
        strncat_s(probeNote, sizeof probeNote, s.cameraProbeDiagnostic, _TRUNCATE);
    }

    if (!LevelReached(f)) {
        _snprintf_s(d, n, _TRUNCATE,
                    "no level state was reached, so the rig this row is about cannot exist yet; %s",
                    probeNote);
        return Ask::CouldNotAsk;
    }
    if (f.cameraRefusedSeen && !f.cameraBoundSeen) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the camera control REFUSED the rig %u time(s) and never bound. The refusal's "
                    "own sentence: \"%s\". %s",
                    f.cameraRefusalCount,
                    f.cameraRefusalWhy[0] != '\0' ? f.cameraRefusalWhy
                                                  : "(the line carried no cause between '): ' and "
                                                    "'; pose-owner: ')",
                    probeNote);
        return Ask::Fail;
    }
    if (f.cameraStaleNounSeen && !f.cameraBoundSeen && !f.cameraRefusedSeen) {
        _snprintf_s(d, n, _TRUNCATE,
                    "a [camera] control line was written in the pre-rename spelling, which this "
                    "reader no longer parses, so how far the rig resolved is unknown: %s",
                    f.cameraStaleNounLine);
        return Ask::CouldNotAsk;
    }
    if (!s.cameraProbed) {
        _snprintf_s(d, n, _TRUNCATE,
                    "no [camera] refusal line and camera_control::Probe was never called, so "
                    "nothing in this launch says how far the rig resolved");
        return Ask::CouldNotAsk;
    }
    if (s.cameraProbeReadyEver) {
        _snprintf_s(d, n, _TRUNCATE, "%s; %s", probeNote,
                    f.cameraBoundSeen ? f.cameraLine : "no [camera] bind line yet");
        return Ask::Pass;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "over %.0f ms with a level loaded, %s -- it never reached Ready, and no [camera] "
                "refusal line named a cause either", s.observedMs, probeNote);
    return Ask::Fail;
}

// W2-A, read only: the harness scores the sentinel's contents. The observer reports the name it
// expected and whether that exact name existed, so a missing file is a named absence.
inline Ask AskCaptureStopSentinel(const StatusFacts& s, const LogFacts& f, char* d, size_t n) {
    if (!s.stopSentinelNameKnown) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the sentinel name could not be composed from this launch's id and pid");
        return Ask::CouldNotAsk;
    }
    if (s.stopSentinelDir[0] == '\0') {
        _snprintf_s(d, n, _TRUNCATE,
                    "expected '%s', but no capture directory was reported, so nothing was looked "
                    "at", s.stopSentinelName);
        return Ask::CouldNotAsk;
    }
    if (f.captureClosedEmpty) {
        // A stop that stopped nothing cannot score the stop mechanism in either direction: present
        // would certify a sentinel that had no capture to end, absent would blame a mechanism that
        // was never reached.
        _snprintf_s(d, n, _TRUNCATE,
                    "nothing-captured: the capture module closed without a capture, so '%s' has "
                    "nothing to have stopped; %s", s.stopSentinelName, f.captureClosedLine);
        return Ask::CouldNotAsk;
    }
    if (s.stopSentinelPresent) {
        _snprintf_s(d, n, _TRUNCATE, "'%s' present in %s; capture close line: %s",
                    s.stopSentinelName, s.stopSentinelDir,
                    f.captureClosedSeen ? f.captureClosedLine : "(none)");
        return Ask::Pass;
    }
    _snprintf_s(d, n, _TRUNCATE, "'%s' is NOT in %s; capture close line: %s", s.stopSentinelName,
                s.stopSentinelDir, f.captureClosedSeen ? f.captureClosedLine : "(none)");
    return Ask::Fail;
}

// ---------------------------------------------------------------------------------------------
// THE LAUNCH MARKER'S TEXT.
//
// The file IO and the directory resolution live in probe_replay.cpp, which reuses the capture
// module's own (the current directory, then StateDir()). What is here is the part that can be
// driven without a game -- and the malformed cases are the whole reason it is separated, because a
// parser nothing can exercise is a parser whose default IS its answer.
// ---------------------------------------------------------------------------------------------

// The value after `"key":`. Punctuated on BOTH sides -- the quoted key and the colon -- so a key
// that merely appears inside another key or inside a value is not read as this one.
inline const char* MarkerValue(const char* text, const char* key) {
    char quoted[64];
    _snprintf_s(quoted, sizeof quoted, _TRUNCATE, "\"%s\"", key);
    const char* at = strstr(text, quoted);
    if (at == nullptr) {
        return nullptr;
    }
    const char* p = at + strlen(quoted);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { ++p; }
    if (*p != ':') {
        return nullptr;
    }
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { ++p; }
    return p;
}

// `strncmp(value, "true", 4) == 0` is a PREFIX test, so `truthy` read as true and
// `falsey` as false -- a marker whose attended value is a word we do not understand would have been
// answered confidently instead of refused. The value runs to whatever JSON punctuation follows it,
// so the word ends where an identifier ends.
inline bool MarkerWordIs(const char* value, const char* word) {
    const size_t n = strlen(word);
    if (strncmp(value, word, n) != 0) {
        return false;
    }
    const char c = value[n];
    return !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
             c == '_' || c == '-' || c == '.' || c == '+');
}

inline void ParseLaunchMarker(const char* text, const char* source, StatusFacts& s) {
    s.markerPresent = true;
    strncpy_s(s.markerSource, sizeof s.markerSource, source, _TRUNCATE);

    const char* window = MarkerValue(text, "windowSec");
    if (window == nullptr) {
        _snprintf_s(s.markerWindowReason, sizeof s.markerWindowReason, _TRUNCATE,
                    "%s declares no windowSec", source);
    } else if ((*window >= '0' && *window <= '9') || *window == '-' || *window == '+' ||
               *window == '.') {
        s.markerWindowSec = atof(window);
        s.markerWindowKnown = true;
    } else {
        _snprintf_s(s.markerWindowReason, sizeof s.markerWindowReason, _TRUNCATE,
                    "%s declares windowSec, but its value is not a number", source);
    }

    const char* attended = MarkerValue(text, "attended");
    if (attended == nullptr) {
        _snprintf_s(s.markerAttendedReason, sizeof s.markerAttendedReason, _TRUNCATE,
                    "%s declares no attended flag", source);
    } else if (MarkerWordIs(attended, "true")) {
        s.markerAttended = true;
        s.markerAttendedKnown = true;
    } else if (MarkerWordIs(attended, "false")) {
        s.markerAttended = false;
        s.markerAttendedKnown = true;
    } else {
        _snprintf_s(s.markerAttendedReason, sizeof s.markerAttendedReason, _TRUNCATE,
                    "%s declares attended, but its value is neither true nor false", source);
    }
}

// No marker anywhere. The reason names every directory that was looked in, because "it is not
// there" is only actionable when the reader says where "there" was.
inline void LaunchMarkerAbsent(const char* whereLooked, StatusFacts& s) {
    s.markerPresent = false;
    strncpy_s(s.markerWindowReason, sizeof s.markerWindowReason, whereLooked, _TRUNCATE);
    strncpy_s(s.markerAttendedReason, sizeof s.markerAttendedReason, whereLooked, _TRUNCATE);
}

// W1-J. The BROWSER arm has no completion event of its own -- it never loads a level and never
// plays a file -- so the only thing that can say when it is done is the launch declaring how long
// it is. That declaration arrives in the launch marker. Its absence is could-not-ask named
// `no-launch-marker`; it is never a default window, because a defaulted window would let the record
// go FINAL on a number nobody declared, which is the confident answer to an unaskable question.
inline Ask AskLaunchWindowDeclared(const StatusFacts& s, char* d, size_t n) {
    if (!s.markerWindowKnown) {
        _snprintf_s(d, n, _TRUNCATE, "no-launch-marker: %s",
                    s.markerWindowReason[0] != '\0' ? s.markerWindowReason
                                                    : "the launch marker has not been read yet");
        return Ask::CouldNotAsk;
    }
    if (s.markerWindowSec <= 0.0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%s declares windowSec %.0f, which is not a window: this arm would go final "
                    "before it observed anything", s.markerSource, s.markerWindowSec);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE, "%s declares windowSec %.0f, and that is this arm's completion "
                "point", s.markerSource, s.markerWindowSec);
    return Ask::Pass;
}

// W1-J / P11. PLAYBACK-IDLE is the arm a human watches, and it is selected by the descriptor's
// probe id. The marker's `attended` is the HARNESS's statement about the same launch, so this row
// is a cross-check of two independent sources on one fact: an id that selected the attended arm
// under a launch declaring nobody is watching is a contradiction, and P11's NOT-RUN path is exactly
// that unattended case. Reported, never resolved silently in either direction -- the id keeps
// selecting the arm.
inline Ask AskLaunchAttendanceDeclared(const StatusFacts& s, char* d, size_t n) {
    if (!s.markerAttendedKnown) {
        _snprintf_s(d, n, _TRUNCATE, "no-launch-marker: %s",
                    s.markerAttendedReason[0] != '\0' ? s.markerAttendedReason
                                                      : "the launch marker has not been read yet");
        return Ask::CouldNotAsk;
    }
    if (!s.markerAttended) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the probe id selected PLAYBACK-IDLE but %s declares attended=false: one of the "
                    "two is wrong about this launch, and P11's fidelity arm files NOT-RUN when "
                    "nobody is watching", s.markerSource);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE, "%s declares attended=true, agreeing with the probe id that "
                "selected this arm", s.markerSource);
    return Ask::Pass;
}

// ---------------------------------------------------------------------------------------------
// The arm's row list. This is the observer's table; the DESCRIPTOR's answers[] is the denominator,
// and tests/replay/check.ps1 asserts every id a descriptor row lists is produced by that row's arm.
// ---------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------
// THE TRANSPORT BATTERY'S ROWS. One row per verb of the owner's sentence, and each of them reads
// what the WORLD did after the step -- the dispatcher's own result word, plus the transport's
// state, rate and playhead as dispatch::Snapshot publishes them. A row that scored only "the
// command was accepted" would pass on a transport that ignored it, which is exactly the defect
// RES-TRANSPORT-CMDS found in the raw-interface battery it replaces.
// ---------------------------------------------------------------------------------------------
inline const TransportStep& StepFor(const StatusFacts& s, Verb v) {
    return s.steps[static_cast<size_t>(v)];
}

// Fills `d` with the reason this step cannot be scored and returns false; true means the step was
// issued through the dispatcher and an answer came back, whatever that answer was.
inline bool StepScoreable(const StatusFacts& s, Verb v, char* d, size_t n) {
    if (!s.batteryDispatched) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the playback battery never ran on this launch: the transport held no capture "
                    "(durationUs %llu), so `%s` was never issued and there are no zeros here to "
                    "read as an answer", static_cast<unsigned long long>(s.durationUs),
                    VerbName(v));
        return false;
    }
    const TransportStep& st = StepFor(s, v);
    if (!st.attempted) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` was not issued on this launch, so nothing about it is known", VerbName(v));
        return false;
    }
    if (!st.submitted) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` was reached but never handed to control::dispatch::Submit, so it did not "
                    "travel the seam the panel and the console use and its outcome is not this "
                    "row's subject", st.asked);
        return false;
    }
    if (!st.answered) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` was submitted and the dispatcher did not answer within %.0f ms. A queued "
                    "command with no collected Result is an unanswered question, never a failure "
                    "of the transport", st.asked, st.waitMs);
        return false;
    }
    if (!st.readBack) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` was answered (%s) but no dispatch::Snapshot was read afterwards, so the "
                    "transport's own state is unknown", st.asked, st.said);
        return false;
    }
    return true;
}

inline const char* StateWord(int state) {
    switch (state) {
        case 0: return "Pause";
        case 1: return "Play";
        case 2: return "Scrub";
        case 3: return "Seek";
        case 4: return "Step";
        case 5: return "NotLoaded";
        default: return "no-transport";
    }
}

// EVERY step went through dispatch::Submit and every one of them was answered. This is the path
// claim on its own: the rows below score what the transport did, and this one scores that the
// asking happened where the product's own surfaces ask.
inline Ask AskVerbsThroughDispatch(const StatusFacts& s, char* d, size_t n) {
    if (!s.batteryDispatched) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the playback battery never ran: the transport held no capture (durationUs "
                    "%llu). No step was issued, through this seam or any other",
                    static_cast<unsigned long long>(s.durationUs));
        return Ask::CouldNotAsk;
    }
    unsigned raw = 0;
    const char* firstRaw = "";
    for (size_t i = 0; i < static_cast<size_t>(Verb::Count); ++i) {
        const TransportStep& st = s.steps[i];
        if (st.attempted && !st.submitted) {
            if (raw == 0) { firstRaw = VerbName(static_cast<Verb>(i)); }
            ++raw;
        }
    }
    if (raw != 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%u of %u attempted step(s) did NOT go through control::dispatch::Submit (the "
                    "first is `%s`). A step that reaches IReplayTransport directly skips ui::SetRate, "
                    "which is the only place the fast-forward ceiling is enforced, so it can report "
                    "success on a rate the panel refuses", raw, s.stepsAttempted, firstRaw);
        return Ask::Fail;
    }
    if (s.stepsAnswered != s.stepsAttempted) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%u of %u step(s) submitted to the dispatcher were never answered: the queue "
                    "took them and no Result came back under their sequence number",
                    s.stepsAttempted - s.stepsAnswered, s.stepsAttempted);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "all %u transport step(s) were issued through control::dispatch::Submit and "
                "collected with TakeResult -- the same seam src/control/control_module.cpp uses for "
                "the launch load and the console uses for a typed line -- and every one was answered",
                s.stepsAttempted);
    return Ask::Pass;
}

inline Ask AskPauseAndPlay(const StatusFacts& s, char* d, size_t n) {
    if (!StepScoreable(s, Verb::Pause, d, n)) { return Ask::CouldNotAsk; }
    if (!StepScoreable(s, Verb::Play, d, n))  { return Ask::CouldNotAsk; }
    const TransportStep& p = StepFor(s, Verb::Pause);
    const TransportStep& g = StepFor(s, Verb::Play);
    if (!p.ok || p.state != 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`pause` answered ok=%d \"%s\" and the transport then read %s. Pause is the one "
                    "verb whose whole effect is the state word, so a state that is not Pause is the "
                    "command having been accepted and not obeyed", p.ok ? 1 : 0, p.said,
                    StateWord(p.state));
        return Ask::Fail;
    }
    if (!g.ok || g.state != 1) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`pause` held (%s), but `play` answered ok=%d \"%s\" and the transport then read "
                    "%s at rate %.4g", StateWord(p.state), g.ok ? 1 : 0, g.said, StateWord(g.state),
                    static_cast<double>(g.rate));
        return Ask::Fail;
    }
    if (g.rate < 0.99f || g.rate > 1.01f) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`play` is documented 1x only (command.cpp: \"use speed for any other rate\") and "
                    "the transport read back rate %.4g in state %s",
                    static_cast<double>(g.rate), StateWord(g.state));
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "`pause` -> %s (\"%s\"), then `play` -> %s at %.4gx (\"%s\"). Both through "
                "dispatch::Submit, and both read back off dispatch::Snapshot rather than assumed",
                StateWord(p.state), p.said, StateWord(g.state), static_cast<double>(g.rate), g.said);
    return Ask::Pass;
}

// slowmo and speedup are one question asked at two rates: does the rate the user asked for come
// back off the transport? RunSpeed routes through ui::SetRate, which writes g.rate and calls
// Play(rate) -- so a rate that does not read back is a rate the panel would show wrongly too.
inline Ask AskRateReadsBack(const StatusFacts& s, char* d, size_t n) {
    if (!StepScoreable(s, Verb::SpeedHalf, d, n)) { return Ask::CouldNotAsk; }
    const TransportStep& half = StepFor(s, Verb::SpeedHalf);
    const TransportStep& up = StepFor(s, Verb::SpeedUnderCeiling);
    if (!half.ok) {
        _snprintf_s(d, n, _TRUNCATE, "`speed 0.5` was REFUSED: \"%s\"", half.said);
        return Ask::Fail;
    }
    if (half.rate < 0.49f || half.rate > 0.51f) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`speed 0.5` answered ok (\"%s\") and the transport read back %.4g. The rate the "
                    "panel shows and the rate its Play button resumes at are this number",
                    half.said, static_cast<double>(half.rate));
        return Ask::Fail;
    }
    if (s.rateCeiling <= 0.0f) {
        _snprintf_s(d, n, _TRUNCATE,
                    "slowmo verified: `speed 0.5` read back %.4g. FAST-FORWARD IS UNASKED -- "
                    "ui::RateCeiling() is 0, meaning no frame period has been measured yet, and "
                    "uiapi.h forbids substituting 4.0 for it. There is no rate this launch can "
                    "prove is under the ceiling", static_cast<double>(half.rate));
        return Ask::CouldNotAsk;
    }
    if (s.rateUnderCeiling <= 1.0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "slowmo verified: `speed 0.5` read back %.4g. FAST-FORWARD IS UNASKABLE on this "
                    "machine: the ceiling is %.2fx, so there is no rate above 1x that is under it",
                    static_cast<double>(half.rate), static_cast<double>(s.rateCeiling));
        return Ask::CouldNotAsk;
    }
    if (!StepScoreable(s, Verb::SpeedUnderCeiling, d, n)) { return Ask::CouldNotAsk; }
    if (!up.ok) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`speed %.4g` was REFUSED (\"%s\") although %.4g is under this machine's ceiling "
                    "of %.2fx", s.rateUnderCeiling, up.said, s.rateUnderCeiling,
                    static_cast<double>(s.rateCeiling));
        return Ask::Fail;
    }
    const double got = static_cast<double>(up.rate);
    const double want = s.rateUnderCeiling;
    if (got < want - 0.01 || got > want + 0.01) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`speed %.4g` answered ok (\"%s\") and the transport read back %.4g instead. "
                    "Above the ceiling the engine stops accelerating without saying so; this rate is "
                    "UNDER it (%.2fx) and still did not take", want, up.said, got,
                    static_cast<double>(s.rateCeiling));
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "slowmo and speedup both read back off the transport: `speed 0.5` -> %.4g, "
                "`speed %.4g` -> %.4g, against a measured ceiling of %.2fx",
                static_cast<double>(half.rate), want, got, static_cast<double>(s.rateCeiling));
    return Ask::Pass;
}

// The refusal IS the pass. uiapi.h: above 66666 / raw_frame_us the engine silently stops
// accelerating, so a rate that is accepted up there is the defect and a refusal that does not name
// the number sends the user tuning blind.
inline Ask AskRateAboveCeilingRefused(const StatusFacts& s, char* d, size_t n) {
    if (!s.batteryDispatched) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the playback battery never ran (durationUs %llu), so no rate was asked for",
                    static_cast<unsigned long long>(s.durationUs));
        return Ask::CouldNotAsk;
    }
    if (s.rateCeiling <= 0.0f) {
        _snprintf_s(d, n, _TRUNCATE,
                    "ui::RateCeiling() is 0 -- no frame period has been measured, and uiapi.h "
                    "forbids reading that as 4.0. With no ceiling there is no rate this row could "
                    "have put above one");
        return Ask::CouldNotAsk;
    }
    if (!StepScoreable(s, Verb::SpeedOverCeiling, d, n)) { return Ask::CouldNotAsk; }
    const TransportStep& over = StepFor(s, Verb::SpeedOverCeiling);
    if (over.ok) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`speed %.4g` was ACCEPTED (\"%s\") over a ceiling of %.2fx, and the transport "
                    "read back %.4g. Past the ceiling the engine stops accelerating and reports "
                    "nothing, so an accepted rate up here is a control that silently does nothing",
                    s.rateOverCeiling, over.said, static_cast<double>(s.rateCeiling),
                    static_cast<double>(over.rate));
        return Ask::Fail;
    }
    char want[40];
    _snprintf_s(want, sizeof want, _TRUNCATE, "ceiling of %.2fx", static_cast<double>(s.rateCeiling));
    if (strstr(over.said, want) == nullptr) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`speed %.4g` was refused, and the refusal does not name the number: \"%s\" "
                    "carries no \"%s\". A refusal without the ceiling in it leaves the user guessing "
                    "which rates exist", s.rateOverCeiling, over.said, want);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "`speed %.4g` was REFUSED by ui::SetRate and the refusal names the number: \"%s\". "
                "The transport stayed at %.4g in state %s. This refusal is the gate working",
                s.rateOverCeiling, over.said, static_cast<double>(over.rate), StateWord(over.state));
    return Ask::Pass;
}

inline Ask AskReverseRunsBackward(const StatusFacts& s, char* d, size_t n) {
    if (!StepScoreable(s, Verb::Reverse, d, n)) { return Ask::CouldNotAsk; }
    const TransportStep& rv = StepFor(s, Verb::Reverse);
    if (!rv.ok) {
        _snprintf_s(d, n, _TRUNCATE, "`speed -1` was REFUSED: \"%s\"", rv.said);
        return Ask::Fail;
    }
    if (rv.rate >= 0.0f) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`speed -1` answered ok (\"%s\") and the transport read back rate %.4g. "
                    "IReplayTransport::Play documents negative as reverse, so a non-negative "
                    "read-back is the sign having been dropped", rv.said,
                    static_cast<double>(rv.rate));
        return Ask::Fail;
    }
    if (s.reverseSamples < 2) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`speed -1` read back %.4g, but the playhead was sampled %u time(s) afterwards: "
                    "a direction is a difference and one sample is not one",
                    static_cast<double>(rv.rate), s.reverseSamples);
        return Ask::CouldNotAsk;
    }
    if (s.reversePlayheadFirst <= 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`speed -1` read back %.4g with the playhead already at %lld us. There is "
                    "nothing behind the start of the file to move into, so this launch cannot tell "
                    "a working reverse from a stalled one", static_cast<double>(rv.rate),
                    s.reversePlayheadFirst);
        return Ask::CouldNotAsk;
    }
    if (s.reversePlayheadLast >= s.reversePlayheadFirst) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`speed -1` read back rate %.4g and the playhead did NOT move backward: %lld us "
                    "-> %lld us over %u sample(s). A negative rate the clock does not honour is "
                    "\"and back\" accepted and not performed", static_cast<double>(rv.rate),
                    s.reversePlayheadFirst, s.reversePlayheadLast, s.reverseSamples);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "`speed -1` read back rate %.4g and the playhead ran BACKWARD, %lld us -> %lld us "
                "over %u sample(s) of dispatch::Snapshot", static_cast<double>(rv.rate),
                s.reversePlayheadFirst, s.reversePlayheadLast, s.reverseSamples);
    return Ask::Pass;
}

// The landing, not the acceptance. A seek that returns true and leaves the playhead where it was
// is the failure this row exists for.
inline Ask AskSeekLandsOnTarget(const StatusFacts& s, char* d, size_t n) {
    if (!StepScoreable(s, Verb::Seek, d, n)) { return Ask::CouldNotAsk; }
    const TransportStep& sk = StepFor(s, Verb::Seek);
    if (!sk.ok) {
        _snprintf_s(d, n, _TRUNCATE, "`%s` was REFUSED: \"%s\"", sk.asked, sk.said);
        return Ask::Fail;
    }
    if (s.seekToleranceUs <= 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` answered ok (\"%s\") and landed at %lld us, but ui::RateCeiling() is 0 so "
                    "raw_frame_us is unknown and there is no tolerance to score the landing "
                    "against. A tolerance invented here would decide the row",
                    sk.asked, sk.said, sk.playheadAfter);
        return Ask::CouldNotAsk;
    }
    const long long off = sk.playheadAfter - sk.target;
    const long long absOff = off < 0 ? -off : off;
    if (absOff > s.seekToleranceUs) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` answered ok (\"%s\") and the playhead is at %lld us, %lld us from the "
                    "target %lld us -- outside the %lld us tolerance (two raw frame periods). It "
                    "was at %lld us before the step", sk.asked, sk.said, sk.playheadAfter, off,
                    sk.target, s.seekToleranceUs, sk.playheadBefore);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "`%s` moved the playhead %lld us -> %lld us, %lld us from the target %lld us "
                "(tolerance %lld us, two raw frame periods off a %.2fx ceiling)", sk.asked,
                sk.playheadBefore, sk.playheadAfter, off, sk.target, s.seekToleranceUs,
                static_cast<double>(s.rateCeiling));
    return Ask::Pass;
}

inline Ask AskStepMovesByTicks(const StatusFacts& s, char* d, size_t n) {
    if (!StepScoreable(s, Verb::StepForward, d, n)) { return Ask::CouldNotAsk; }
    if (!StepScoreable(s, Verb::StepBack, d, n))    { return Ask::CouldNotAsk; }
    const TransportStep& fwd = StepFor(s, Verb::StepForward);
    const TransportStep& bak = StepFor(s, Verb::StepBack);
    // The battery seeks before it steps, and on F-SYNTH that seek lands on the
    // file's LAST recorded tick, where `step +1` is CORRECTLY refused with the product's own
    // kStepNoTickThatWay and the playhead must not move. Reading `fwd.ok` alone graded that correct
    // refusal asked-fail. The boundary is the PAIR -- the named refusal AND an unmoved playhead:
    // a refusal that moved the playhead anyway is still a failure, any other refusal text is still
    // a failure, and `step -1` still has to move, so this arm cannot pass a transport that stopped
    // stepping.
    const bool haveToken = s.stepNoTickToken[0] != '\0';
    const bool fwdAtLastTick = haveToken && !fwd.ok &&
                               strstr(fwd.said, s.stepNoTickToken) != nullptr &&
                               fwd.playheadAfter == fwd.playheadBefore;
    if (!fwd.ok && !haveToken) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`step +1` was refused (\"%s\") and the observer did not capture the product's "
                    "kStepNoTickThatWay token, so this row cannot tell an end-of-file refusal from "
                    "a transport that stopped stepping. It will not guess", fwd.said);
        return Ask::CouldNotAsk;
    }
    if (fwdAtLastTick) {
        if (!bak.ok) {
            _snprintf_s(d, n, _TRUNCATE,
                        "`step +1` was refused at the last recorded tick (\"%s\", playhead held at "
                        "%lld us), which is correct -- but `step -1` was refused too (\"%s\"), so "
                        "no step in either direction reached a tick", fwd.said,
                        fwd.playheadAfter, bak.said);
            return Ask::Fail;
        }
        if (bak.playheadAfter >= bak.playheadBefore) {
            _snprintf_s(d, n, _TRUNCATE,
                        "`step +1` was correctly refused at the last recorded tick (\"%s\"), but "
                        "`step -1` answered ok (\"%s\") and the playhead did not go back: %lld us "
                        "-> %lld us", fwd.said, bak.said, bak.playheadBefore, bak.playheadAfter);
            return Ask::Fail;
        }
        _snprintf_s(d, n, _TRUNCATE,
                    "at the file's LAST recorded tick: `step +1` was refused with the product's own "
                    "'%s' and left the playhead at %lld us, and `step -1` moved %lld us -> %lld us. "
                    "A step that reaches no tick is not a step, and refusing it there is the "
                    "correct answer, not a failure", s.stepNoTickToken, fwd.playheadAfter,
                    bak.playheadBefore, bak.playheadAfter);
        return Ask::Pass;
    }
    if (!fwd.ok || !bak.ok) {
        _snprintf_s(d, n, _TRUNCATE, "`step +1` ok=%d (\"%s\"); `step -1` ok=%d (\"%s\")",
                    fwd.ok ? 1 : 0, fwd.said, bak.ok ? 1 : 0, bak.said);
        return Ask::Fail;
    }
    if (fwd.playheadAfter <= fwd.playheadBefore) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`step +1` answered ok (\"%s\") and the playhead did not advance: %lld us -> "
                    "%lld us. Transport::StepTicks moves by RECORDED ticks and raises Cause::Seek, "
                    "so a step that moves nothing is a frame the user cannot reach", fwd.said,
                    fwd.playheadBefore, fwd.playheadAfter);
        return Ask::Fail;
    }
    if (bak.playheadAfter >= bak.playheadBefore) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`step +1` advanced %lld us -> %lld us, but `step -1` answered ok (\"%s\") and "
                    "the playhead did not go back: %lld us -> %lld us", fwd.playheadBefore,
                    fwd.playheadAfter, bak.said, bak.playheadBefore, bak.playheadAfter);
        return Ask::Fail;
    }
    // ***** ENG-W2B-STEP2 `d77cd0b` RE-SPECIFIED THIS ROW, and the old spelling was a false
    // statement about a CORRECT transport. `f4db605` required the two steps to cover the same
    // distance to within half a spacing. They legitimately do not: a percentage seek lands wherever
    // the fraction falls, `step +1` consumes only the REST of the tick it is inside, and `step -1`
    // then starts ON a boundary and owes a whole recorded tick. The launch's `+16 667 / -33 333`
    // against a 33 333 us capture IS that shape, one recorded tick each way, and STEP2 reproduced
    // it offline against the real file. A symmetry bound would now red a working product.
    //
    // What is scored instead, per STEP, never per pair:
    //   1. it moved in the direction it was commanded            (the arms above);
    //   2. it moved more than nothing and no more than ONE recorded tick;
    //   3. IReplayTransport::LastStepTicks() reports exactly +1 / -1 -- the transport's own count,
    //      which the playhead cannot be differenced for across a repeated boundary;
    //   4. the landing is a recorded tick BOUNDARY.
    const long long fwdDelta = fwd.playheadAfter - fwd.playheadBefore;
    const long long bakDelta = bak.playheadBefore - bak.playheadAfter;
    if (s.tickSpacingUs <= 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`step +1` moved +%lld us and `step -1` moved -%lld us, both through the "
                    "dispatcher -- but IReplayTransport::TickSpacingUs published no cadence for "
                    "this capture, so there is no recorded tick to measure either against. This "
                    "row will not invent one", fwdDelta, bakDelta);
        return Ask::CouldNotAsk;
    }
    if (!fwd.stepTicksRead || !bak.stepTicksRead || !s.stepTicksUncountedKnown) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`step +1` moved +%lld us and `step -1` moved -%lld us, but this observer did "
                    "not read IReplayTransport::LastStepTicks() around them (forward read=%d, back "
                    "read=%d, uncounted sentinel known=%d), so how many RECORDED ticks each step "
                    "consumed is unknown and the microsecond deltas alone cannot say",
                    fwdDelta, bakDelta, fwd.stepTicksRead ? 1 : 0, bak.stepTicksRead ? 1 : 0,
                    s.stepTicksUncountedKnown ? 1 : 0);
        return Ask::CouldNotAsk;
    }
    if (fwd.lastStepTicks == s.stepTicksUncounted || bak.lastStepTicks == s.stepTicksUncounted) {
        // kStepTicksUncounted, never 0: a transport that counts nothing has refused nothing, and
        // grading it as a zero-tick step would fail every double this interface allows.
        _snprintf_s(d, n, _TRUNCATE,
                    "`step +1` moved +%lld us and `step -1` moved -%lld us, but this transport "
                    "answers LastStepTicks() with the interface's `uncounted` value, so it counts "
                    "no steps at all. An uncounted step is not a refused one and this row will not "
                    "read it as either", fwdDelta, bakDelta);
        return Ask::CouldNotAsk;
    }
    if (fwd.lastStepTicks != 1 || bak.lastStepTicks != -1) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`step +1` reported %d recorded tick(s) and `step -1` reported %d, where one "
                    "command of one tick must report exactly +1 and -1. The playhead moved "
                    "+%lld us and -%lld us against a %lld us tick spacing, and the microsecond "
                    "deltas cannot stand in for the count -- a non-decreasing tick table records a "
                    "stalled boundary twice",
                    fwd.lastStepTicks, bak.lastStepTicks, fwdDelta, bakDelta, s.tickSpacingUs);
        return Ask::Fail;
    }
    if (fwdDelta > s.tickSpacingUs || bakDelta > s.tickSpacingUs) {
        // The bound is ONE spacing, per the re-spec. TickSpacingUs is a MEAN over the first core
        // chapter, so a capture GAP is a recorded tick longer than the mean and lands here too --
        // the sentence says so, and Transport::GapsCrossed is where that reading is settled.
        _snprintf_s(d, n, _TRUNCATE,
                    "`step +1` moved +%lld us and `step -1` moved -%lld us, and one of them is "
                    "longer than this capture's whole %lld us tick spacing "
                    "(IReplayTransport::TickSpacingUs) although both reported a single recorded "
                    "tick. One command must not cover more than one tick. TickSpacingUs is a MEAN, "
                    "so a recorded GAP reads here as well -- Transport::GapsCrossed settles which",
                    fwdDelta, bakDelta, s.tickSpacingUs);
        return Ask::Fail;
    }
    if (bak.playheadBefore != fwd.playheadAfter) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`step +1` left the playhead at %lld us but `step -1` started from %lld us: "
                    "something moved the playhead between the two steps, so this pair is not one "
                    "step out and one step back and neither landing can be attributed",
                    fwd.playheadAfter, bak.playheadBefore);
        return Ask::Fail;
    }
    if (s.stepBoundaryKnown) {
        if (fwd.playheadAfter != s.stepBoundaryForwardUs ||
            bak.playheadAfter != s.stepBoundaryBackUs) {
            _snprintf_s(d, n, _TRUNCATE,
                        "`step +1` landed at %lld us where the capture's tick table has a boundary "
                        "at %lld us, and `step -1` landed at %lld us against a boundary at "
                        "%lld us. A step is exact by definition -- it discards the sub-tick "
                        "remainder -- so a landing off a recorded boundary is a playhead the file "
                        "does not contain",
                        fwd.playheadAfter, s.stepBoundaryForwardUs, bak.playheadAfter,
                        s.stepBoundaryBackUs);
            return Ask::Fail;
        }
        _snprintf_s(d, n, _TRUNCATE,
                    "`step +1` moved %lld us -> %lld us (+%lld us) and `step -1` moved %lld us -> "
                    "%lld us (-%lld us). Each reported exactly one recorded tick "
                    "(LastStepTicks %+d / %+d), each covered no more than the capture's %lld us "
                    "spacing, and each landed ON the boundary the tick table records (%lld us and "
                    "%lld us). The two distances DIFFER by %lld us%s",
                    fwd.playheadBefore, fwd.playheadAfter, fwdDelta, bak.playheadBefore,
                    bak.playheadAfter, bakDelta, fwd.lastStepTicks, bak.lastStepTicks,
                    s.tickSpacingUs, s.stepBoundaryForwardUs, s.stepBoundaryBackUs,
                    bakDelta - fwdDelta, StepSpreadReason(bakDelta - fwdDelta));
        return Ask::Pass;
    }
    // Clause 4 UNASKED, and named as such rather than assumed. IReplayTransport publishes a
    // SPACING but no accessor for the tick table itself, so nothing here can say which
    // microseconds are boundaries; the offline fixture supplies them and scores the clause, the
    // live probe cannot, and an unfilled boundary is never read as a landing that agreed.
    _snprintf_s(d, n, _TRUNCATE,
                "`step +1` moved %lld us -> %lld us (+%lld us) and `step -1` moved %lld us -> "
                "%lld us (-%lld us). Each reported exactly one recorded tick (LastStepTicks "
                "%+d / %+d) and neither covered more than the capture's %lld us spacing. The two "
                "distances differ by %lld us%s. "
                "boundary-not-published: IReplayTransport exposes TickSpacingUs but no tick-table "
                "accessor, so THIS observer cannot check either landing is on a recorded boundary",
                fwd.playheadBefore, fwd.playheadAfter, fwdDelta, bak.playheadBefore,
                bak.playheadAfter, bakDelta, fwd.lastStepTicks, bak.lastStepTicks,
                s.tickSpacingUs, bakDelta - fwdDelta, StepSpreadReason(bakDelta - fwdDelta));
    return Ask::Pass;
}

// ENG-W2R-ROWS3B (L3-RUN-6 sec.1 item (8) / F1). BOTH numbers a round row differences are
// FILE-RELATIVE: the playhead runs 0..durationUs from the file's own start, and the round's
// recorded time is the marker as the manifest carries it. The launch F1 was written from
// recorded a marker at -17025384 us -- absolute 1874427 minus t0 18899811 -- against a playhead
// based at 0, and the seventeen seconds that fell out of that subtraction were read as a landing
// error. A marker outside the file's own span is exactly that shape, and this row NAMES it
// instead of scoring it: the distance between two clock bases is not a landing error. The rebase
// itself belongs to the capture packet (F1 rules so); what happens here is that it is not scored.
inline bool RoundTimeOffFileBase(const StatusFacts& s, long long target, char* d, size_t n,
                                 const char* asked, const char* said) {
    if (s.durationUs == 0) { return false; }
    const long long span = static_cast<long long>(s.durationUs);
    if (target >= 0 && target <= span) { return false; }
    _snprintf_s(d, n, _TRUNCATE,
                "`%s` answered ok (\"%s\") and the round's own recorded time is %lld us, which is "
                "OUTSIDE this file's span 0..%lld us. The playhead this row differences it against "
                "is file-relative, so a marker left on another base -- an absolute capture clock, "
                "or one t0 was subtracted from a second time -- cannot be differenced against it. "
                "The %lld us that fall out of that subtraction are the gap between two clocks and "
                "not a landing error",
                asked, said, target, span, target < 0 ? -target : target - span);
    return true;
}

inline Ask AskRoundLandsOnRound(const StatusFacts& s, char* d, size_t n) {
    if (!s.batteryDispatched) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the playback battery never ran (durationUs %llu), so no round was asked for",
                    static_cast<unsigned long long>(s.durationUs));
        return Ask::CouldNotAsk;
    }
    if (!s.roundsKnown || s.roundCount == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "this capture has no round table to seek into: ui::RoundsKnown()=%d, "
                    "ui::RoundCount()=%zu. \"No rounds\" and \"we could not read the manifest\" are "
                    "different answers and uiapi.h keeps them apart; neither is a failed seek",
                    s.roundsKnown ? 1 : 0, s.roundCount);
        return Ask::CouldNotAsk;
    }
    if (!StepScoreable(s, Verb::Round, d, n)) { return Ask::CouldNotAsk; }
    const TransportStep& rd = StepFor(s, Verb::Round);
    if (!rd.ok) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` was REFUSED although the capture's own round table lists %zu round(s): "
                    "\"%s\"", rd.asked, s.roundCount, rd.said);
        return Ask::Fail;
    }
    if (RoundTimeOffFileBase(s, rd.target, d, n, rd.asked, rd.said)) { return Ask::Fail; }
    if (s.seekToleranceUs <= 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` answered ok (\"%s\") and landed at %lld us against a recorded round time "
                    "of %lld us, but ui::RateCeiling() is 0 so there is no tolerance to score the "
                    "landing against", rd.asked, rd.said, rd.playheadAfter, rd.target);
        return Ask::CouldNotAsk;
    }
    const long long off = rd.playheadAfter - rd.target;
    const long long absOff = off < 0 ? -off : off;
    if (absOff > s.seekToleranceUs) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` answered ok (\"%s\") and the playhead is at %lld us, %lld us from the "
                    "round's own recorded time %lld us (tolerance %lld us). Landing on the wrong "
                    "round does not fault -- it looks plausible and gets blamed on seek",
                    rd.asked, rd.said, rd.playheadAfter, off, rd.target, s.seekToleranceUs);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "`%s` moved the playhead %lld us -> %lld us, %lld us from that round's recorded "
                "time %lld us, out of %zu labelled round(s)", rd.asked, rd.playheadBefore,
                rd.playheadAfter, off, rd.target, s.roundCount);
    return Ask::Pass;
}

inline Ask AskGotoReturnsToBookmark(const StatusFacts& s, char* d, size_t n) {
    if (!StepScoreable(s, Verb::Mark, d, n)) { return Ask::CouldNotAsk; }
    const TransportStep& mk = StepFor(s, Verb::Mark);
    if (!mk.ok) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`mark` was refused (\"%s\"), so no bookmark exists for `goto` to return to. "
                    "That is a bookmark question, not a seek one", mk.said);
        return Ask::CouldNotAsk;
    }
    if (!StepScoreable(s, Verb::Goto, d, n)) { return Ask::CouldNotAsk; }
    const TransportStep& gt = StepFor(s, Verb::Goto);
    if (!gt.ok) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`mark` recorded a bookmark at %lld us (\"%s\") and `%s` was REFUSED: \"%s\"",
                    mk.target, mk.said, gt.asked, gt.said);
        return Ask::Fail;
    }
    if (s.seekToleranceUs <= 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` answered ok (\"%s\") and landed at %lld us against a bookmark at %lld us, "
                    "but ui::RateCeiling() is 0 so there is no tolerance to score it against",
                    gt.asked, gt.said, gt.playheadAfter, mk.target);
        return Ask::CouldNotAsk;
    }
    const long long away = gt.playheadBefore - mk.target;
    const long long absAway = away < 0 ? -away : away;
    if (absAway <= s.seekToleranceUs) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` was issued with the playhead already at %lld us, within %lld us of the "
                    "bookmark at %lld us. A jump to where you already are cannot tell a working "
                    "`goto` from one that does nothing", gt.asked, gt.playheadBefore,
                    s.seekToleranceUs, mk.target);
        return Ask::CouldNotAsk;
    }
    const long long off = gt.playheadAfter - mk.target;
    const long long absOff = off < 0 ? -off : off;
    if (absOff > s.seekToleranceUs) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` answered ok (\"%s\") from %lld us and the playhead is at %lld us, %lld us "
                    "from the bookmark `mark` wrote at %lld us (tolerance %lld us)", gt.asked,
                    gt.said, gt.playheadBefore, gt.playheadAfter, off, mk.target,
                    s.seekToleranceUs);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "`mark` wrote a bookmark at %lld us; the playhead was then moved away to %lld us "
                "and `%s` brought it back to %lld us, %lld us off (tolerance %lld us)", mk.target,
                gt.playheadBefore, gt.asked, gt.playheadAfter, off, s.seekToleranceUs);
    return Ask::Pass;
}

// ★ WHAT THIS ROW CAN AND CANNOT ASK. residue.cpp raises Cause::Seek from SeekTo, StepTicks and
// Reset, and residue.cpp:87 IsJump takes it -- but ResidueStats (residue.h:99-130) publishes NO
// fire counter. What is available is the sum of its EFFECT counters, which move only where the
// battery had something bound to act on. So:
//   * counters that moved on a rate change are a FINDING: Transport::SetRate raises no
//     discontinuity at all (transport.cpp:75-82), so a residue fire there is the battery firing on
//     something that is not a jump.
//   * counters that moved on some jumps and not others are a FINDING: the same cause was raised by
//     each of them.
//   * counters that moved on NOTHING are could-not-ask, never a pass and never a fail: with no
//     fire counter, that reading cannot separate "no fire" from "fired and had nothing to do".
inline Ask AskResidueFiresOnDiscontinuity(const StatusFacts& s, char* d, size_t n) {
    if (!s.batteryDispatched) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the playback battery never ran (durationUs %llu), so no discontinuity was "
                    "commanded and the residue counters were never differenced",
                    static_cast<unsigned long long>(s.durationUs));
        return Ask::CouldNotAsk;
    }
    static const Verb kJumps[] = {Verb::Seek, Verb::StepForward, Verb::StepBack, Verb::Round,
                                  Verb::SeekAway, Verb::Goto};
    static const Verb kRates[] = {Verb::SpeedHalf, Verb::SpeedUnderCeiling};
    bool armed = false;
    for (size_t i = 0; i < static_cast<size_t>(Verb::Count); ++i) {
        if (s.steps[i].attempted && s.steps[i].residueArmed) { armed = true; }
    }
    if (!armed) {
        _snprintf_s(d, n, _TRUNCATE,
                    "residue::Stats().armed is false on every step: ArmPlaybackResidue never ran on "
                    "this launch, so nothing was listening for the Cause::Seek these jumps raise. A "
                    "zero delta here is 'never armed', which is not 'clean'");
        return Ask::CouldNotAsk;
    }
    unsigned rateMoved = 0;
    const char* firstRate = "";
    for (size_t i = 0; i < sizeof kRates / sizeof kRates[0]; ++i) {
        const TransportStep& st = StepFor(s, kRates[i]);
        if (st.attempted && st.answered && st.ok && st.residueAfter > st.residueBefore) {
            if (rateMoved == 0) { firstRate = st.asked; }
            ++rateMoved;
        }
    }
    if (rateMoved != 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the residue battery's effect counters MOVED across %u rate change(s), the "
                    "first on `%s`. Transport::SetRate writes rate_ and the pause flag and raises no "
                    "discontinuity at all, so a residue fire there is the battery reacting to "
                    "something that is not a jump", rateMoved, firstRate);
        return Ask::Fail;
    }
    unsigned jumps = 0, moved = 0, still = 0;
    const char* firstStill = "";
    unsigned long long total = 0;
    // L3-RUN-6 F5. The named effect and the per-cause fire buckets, carried beside the sum so the
    // sentence can say WHICH effect moved. `30 in total` reads the same for thirty CI resets and
    // for thirty wakes, and those are not the same repair.
    unsigned long long ciTotal = 0, firesTotal = 0, seekFiresTotal = 0;
    for (size_t i = 0; i < sizeof kJumps / sizeof kJumps[0]; ++i) {
        const TransportStep& st = StepFor(s, kJumps[i]);
        if (!st.attempted || !st.answered || !st.ok) { continue; }
        ++jumps;
        firesTotal += st.residueFiresAfter - st.residueFiresBefore;
        seekFiresTotal += st.residueSeekFiresAfter - st.residueSeekFiresBefore;
        ciTotal += st.residueCiResetsAfter - st.residueCiResetsBefore;
        if (st.residueAfter > st.residueBefore) {
            ++moved;
            total += st.residueAfter - st.residueBefore;
        } else {
            if (still == 0) { firstStill = st.asked; }
            ++still;
        }
    }
    if (jumps == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the residue battery is armed and not one of the six discontinuity commands "
                    "(seek, step +1, step -1, round, seek away, goto) was accepted, so no "
                    "Cause::Seek was raised for it to answer");
        return Ask::CouldNotAsk;
    }
    if (moved == 0) {
        // L3-RUN-3 F7. W3-A (58b4214) landed the counter L3-RUN-2 F6 asked for, and this arm went
        // on printing the request -- while its sibling row was already differencing the field. The
        // two rows now read the SAME field, and the case they could not separate is separated:
        // no fire at all is a discontinuity nobody delivered; a fire with no effect is a live
        // battery over an empty world, which is honestly could-not-ask and not a transport defect.
        unsigned long long fired = 0;
        bool firesRead = false;
        for (size_t i = 0; i < sizeof kJumps / sizeof kJumps[0]; ++i) {
            const TransportStep& st = StepFor(s, kJumps[i]);
            if (!st.attempted || !st.answered || !st.ok || st.residueCauseAfter < 0) { continue; }
            firesRead = true;
            fired += st.residueFiresAfter - st.residueFiresBefore;
        }
        if (!firesRead) {
            _snprintf_s(d, n, _TRUNCATE,
                        "the residue battery is armed and %u commanded discontinuity(ies) moved "
                        "NONE of its effect counters, and not one of them was read back with a "
                        "fire count (residue::Stats().fires), so this reading still cannot "
                        "separate 'the battery did not fire' from 'it fired with nothing bound'",
                        jumps);
            return Ask::CouldNotAsk;
        }
        if (fired == 0) {
            _snprintf_s(d, n, _TRUNCATE,
                        "the residue battery is armed and %u commanded discontinuity(ies) moved "
                        "none of its effect counters AND raised no fire at all: "
                        "residue::Stats().fires did not move once. Every one of these verbs is a "
                        "Cause::Seek to the residue packet, so a battery that never fired is a "
                        "discontinuity nobody delivered", jumps);
            return Ask::Fail;
        }
        _snprintf_s(d, n, _TRUNCATE,
                    "the residue battery is armed, %u commanded discontinuity(ies) raised %llu "
                    "fire(s) (residue::Stats().fires) and moved NONE of its effect counters: it "
                    "fired with nothing bound to act on, which is a live battery over a world "
                    "that had nothing to repair and not a transport defect", jumps, fired);
        return Ask::CouldNotAsk;
    }
    if (still != 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%u of %u commanded discontinuity(ies) moved the residue effect counters "
                    "(%llu in total, of which %llu ciResets) and %u moved none, the first being "
                    "`%s`. Per cause: %llu fire(s), %llu of them Cause::Seek. Every one of them "
                    "raises the same Cause::Seek, so a jump the battery did not answer is a jump "
                    "whose residue was left standing",
                    moved, jumps, total, ciTotal, still, firstStill, firesTotal, seekFiresTotal);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "all %u commanded discontinuity(ies) moved the residue battery's effect counters "
                "(%llu in total, of which %llu ciResets) and neither rate change moved any. Per "
                "cause: %llu fire(s), %llu of them Cause::Seek -- which is what transport.cpp "
                "raising Cause::Seek from SeekTo and StepTicks, and nothing from SetRate, predicts",
                jumps, total, ciTotal, firesTotal, seekFiresTotal);
    return Ask::Pass;
}

// ---------------------------------------------------------------------------------------------
// L3-RUN F2. The playback arm used to complete on "the file played out" ALONE. F-SYNTH is a 10 s
// file and the record filed at +25.3 s -- roughly a second before a healthy launch reaches
// `in game` -- so a launch that never loaded a level filed a FINAL record claiming the arm got
// what it asked for. Three facts, from three holders, have to agree before that word is earned:
// the file played out (the transport), a level loaded ([session]'s own state line, via
// LevelReached) and the battery finished (the battery's own latch).
//
// Playing out with no world is could-not-ask, not asked-fail: nothing was refused, the question
// was never put to a world that existed. The row NAMES the missing level so the reason cannot be
// mistaken for a transport defect.
// ---------------------------------------------------------------------------------------------
inline Ask AskPlaybackCompletionNeedsLevel(const StatusFacts& s, const LogFacts& f, char* d,
                                           size_t n) {
    if (!s.transportPresent) {
        _snprintf_s(d, n, _TRUNCATE,
                    "no transport was bound on this launch, so the arm had no file to play out and "
                    "its completion point was never in reach");
        return Ask::CouldNotAsk;
    }
    if (!s.playedOut) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the file has not played out (durationUs %llu): this record was filed before "
                    "the arm's completion observable, so there is no completion claim to judge",
                    static_cast<unsigned long long>(s.durationUs));
        return Ask::CouldNotAsk;
    }
    if (!InGameReached(f)) {
        _snprintf_s(d, n, _TRUNCATE,
                    "PLAYED WITH NO WORLD: the file played out but [session] never reported NetGame "
                    "state 9 (in game)%s. L3-RUN-2 (a): state 7 is LOADING the level and the world "
                    "-- camera rig, CS registration, entity pool -- arrived 14.8 s later, so every "
                    "world-shaped row filed at 7 was measured against a world that did not exist. "
                    "Playing out ALONE, and loading ALONE, are not this arm's completion point%s",
                    f.sessionLevelStateSeen ? " -- the highest it reached was a level state line it "
                                             "did not follow to 9" : " and no level state line at all",
                    f.sessionStateSeen ? "" : ", and no [session] state line at all");
        return Ask::CouldNotAsk;
    }
    if (!s.batteryDispatched) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the file played out with a level loaded, but the verb battery never started "
                    "(durationUs %llu), so the arm's other ten rows have nothing behind them",
                    static_cast<unsigned long long>(s.durationUs));
        return Ask::CouldNotAsk;
    }
    if (!s.batteryComplete) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the file played out with a level loaded and the battery did NOT finish: %u of "
                    "%u step(s) answered. A final record filed here would file over the questions "
                    "the battery is still asking", s.stepsAnswered, s.stepsAttempted);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "the file played out, the session reached NetGame state 9 (in game) -- %s -- and "
                "the battery finished %u of %u step(s): all three of the arm's completion facts "
                "agree", f.sessionInGameLine, s.stepsAnswered, s.stepsAttempted);
    return Ask::Pass;
}


// ---------------------------------------------------------------------------------------------
// P17-B T11-T18 and W3-A's fire counter. Nine rows, all through
// control::dispatch::Submit except T14, which is a PARSE refusal and says so in its own text.
//
// The tolerance discipline is L3-RUN-2 (d)'s: ui::RateCeiling() has been 0 for two consecutive
// launches, so no frame period has been measured and there is no tolerance to score a near miss
// against. These rows therefore refuse a near miss rather than inventing one -- but a miss larger
// than a tenth of the commanded delta cannot be a one-tick publication lag under ANY frame period
// the engine can run at, so that much is still asked-fail. The row can fail without a tolerance;
// it just cannot grade.
// ---------------------------------------------------------------------------------------------
inline long long RelSeekGradeFloorUs(long long delta) {
    const long long mag = delta < 0 ? -delta : delta;
    return mag / 10;
}

inline Ask AskRelativeSeek(const StatusFacts& s, Verb v, bool forward, char* d, size_t n) {
    if (!StepScoreable(s, v, d, n)) { return Ask::CouldNotAsk; }
    const TransportStep& st = StepFor(s, v);
    if (!st.ok) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` was REFUSED by the dispatcher: %s. A relative nudge inside a loaded file "
                    "is the panel's own -10s/+1s button, so a refusal here is a verb the user "
                    "cannot reach", st.asked, st.said);
        return Ask::Fail;
    }
    const long long expected = st.target;
    const long long moved = st.playheadAfter - st.playheadBefore;
    if (expected == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` was issued with a delta of 0 us, so there is no motion for this row to "
                    "score; the battery records the delta it commanded and it commanded none",
                    st.asked);
        return Ask::CouldNotAsk;
    }
    if (forward ? (moved <= 0) : (moved >= 0)) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` answered ok (\"%s\") and the playhead moved %lld us -- the WRONG "
                    "DIRECTION for a delta of %+lld us: %lld us -> %lld us. Direction needs no "
                    "tolerance to score", st.asked, st.said, moved, expected,
                    st.playheadBefore, st.playheadAfter);
        return Ask::Fail;
    }
    const long long miss = (moved - expected) < 0 ? (expected - moved) : (moved - expected);
    if (miss == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` answered ok (\"%s\") and the playhead moved EXACTLY %+lld us: %lld us -> "
                    "%lld us", st.asked, st.said, expected, st.playheadBefore, st.playheadAfter);
        return Ask::Pass;
    }
    if (miss > RelSeekGradeFloorUs(expected)) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` answered ok (\"%s\") and the playhead moved %+lld us where %+lld us was "
                    "commanded -- a miss of %lld us, more than a tenth of the delta. No frame "
                    "period can be that long, so this is a landing error and not a publication "
                    "lag: %lld us -> %lld us", st.asked, st.said, moved, expected, miss,
                    st.playheadBefore, st.playheadAfter);
        return Ask::Fail;
    }
    if (s.seekToleranceUs > 0) {
        if (miss <= s.seekToleranceUs) {
            _snprintf_s(d, n, _TRUNCATE,
                        "`%s` answered ok (\"%s\") and the playhead moved %+lld us against %+lld "
                        "commanded -- a miss of %lld us, inside the %lld us this machine's own "
                        "frame period allows", st.asked, st.said, moved, expected, miss,
                        s.seekToleranceUs);
            return Ask::Pass;
        }
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` answered ok (\"%s\") and missed by %lld us against a tolerance of %lld "
                    "us built from this machine's measured frame period: %lld us -> %lld us",
                    st.asked, st.said, miss, s.seekToleranceUs, st.playheadBefore, st.playheadAfter);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "`%s` answered ok (\"%s\") and the playhead moved %+lld us against %+lld commanded "
                "-- a miss of %lld us. ui::RateCeiling() is 0, so no frame period was measured and "
                "there is no tolerance to score a miss this small against. L3-RUN-2 (d): a "
                "tolerance invented here would decide the row", st.asked, st.said, moved, expected,
                miss);
    return Ask::CouldNotAsk;
}

inline Ask AskSeekRelativeForward(const StatusFacts& s, char* d, size_t n) {
    return AskRelativeSeek(s, Verb::SeekRelForward, true, d, n);
}

// T12's whole point is that a signed nudge is not an unsigned one. There is no extra arm for
// "the sign was dropped": a transport that ignored the minus lands FORWARD, which is exactly what
// the direction arm above refuses -- and it refuses it with no tolerance, so a dropped sign cannot
// hide inside a frame period.
inline Ask AskSeekRelativeBack(const StatusFacts& s, char* d, size_t n) {
    return AskRelativeSeek(s, Verb::SeekRelBack, false, d, n);
}

inline Ask AskSeekRelativeClampsAtStart(const StatusFacts& s, char* d, size_t n) {
    if (!StepScoreable(s, Verb::SeekRelClampStart, d, n)) { return Ask::CouldNotAsk; }
    const TransportStep& st = StepFor(s, Verb::SeekRelClampStart);
    if (!st.ok) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` was REFUSED: %s. S8.1 rules that a relative seek past the start CLAMPS "
                    "rather than refusing, so a refusal is the defect this row exists to catch",
                    st.asked, st.said);
        return Ask::Fail;
    }
    if (st.playheadAfter != 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` answered ok (\"%s\") and the playhead is at %lld us, not 0: a nudge %lld "
                    "us before the start of the file did not clamp to it",
                    st.asked, st.said, static_cast<long long>(st.playheadAfter), st.target);
        return Ask::Fail;
    }
    if (strstr(st.said, "clamped to the file") == nullptr) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` clamped to 0 us and did NOT say so: \"%s\". A nudge that ran into the "
                    "end of the file and reported the delta it was asked for is a silent lie about "
                    "where the playhead is (dispatch.cpp RunSeek's own words)", st.asked, st.said);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "`%s` clamped to 0 us and said so: \"%s\"", st.asked, st.said);
    return Ask::Pass;
}

// T14. The refusal IS the pass, and the message has to name the operand the user typed. The
// canned advice inside that message contains the string `seek 50%`, so a row that looked for
// "50%" would pass on the advice alone -- it looks for the SIGNED token, which appears only
// where the parser echoes the input back.
inline Ask AskSeekRelativePercentRefused(const StatusFacts& s, char* d, size_t n) {
    if (!s.batteryDispatched) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the playback battery never ran on this launch (durationUs %llu), so `seek "
                    "+50%%` was never parsed", static_cast<unsigned long long>(s.durationUs));
        return Ask::CouldNotAsk;
    }
    if (!s.seekPercentAsked) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`seek +50%%` was not put to control::command::Parse on this launch, so "
                    "nothing about its refusal is known");
        return Ask::CouldNotAsk;
    }
    if (s.seekPercentParsed) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`seek +50%%` PARSED. A relative percentage is a unit nobody typed -- 50%% of "
                    "the file, or 50%% of where you are? -- and command.h's own contract refuses a "
                    "sign and a %% together. Accepting it picks one silently");
        return Ask::Fail;
    }
    if (strstr(s.seekPercentError, "+50%") == nullptr) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`seek +50%%` was refused and the message does not name the operand the user "
                    "typed: \"%s\". A refusal that restates the usage makes the user guess which "
                    "half was wrong", s.seekPercentError);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "`seek +50%%` was refused by control::command::Parse naming the token: \"%s\"",
                s.seekPercentError);
    return Ask::Pass;
}

// The bookmark walk. The two marks are the battery's own, so their times come off the steps that
// made them rather than out of a sidecar this header cannot read.
inline Ask AskGotoWalk(const StatusFacts& s, Verb v, Verb markStep, char* d, size_t n) {
    if (!StepScoreable(s, v, d, n)) { return Ask::CouldNotAsk; }
    const TransportStep& st = StepFor(s, v);
    const TransportStep& mk = StepFor(s, markStep);
    if (!mk.attempted || !mk.ok) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` was issued but `%s` never made its bookmark (%s), so there is no marker "
                    "this row knows the time of", st.asked, VerbName(markStep),
                    mk.attempted ? mk.said : "never issued");
        return Ask::CouldNotAsk;
    }
    if (!st.ok) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` was REFUSED: %s -- with a bookmark at %lld us that it should have "
                    "stepped to", st.asked, st.said, static_cast<long long>(mk.playheadAfter));
        return Ask::Fail;
    }
    if (st.playheadAfter != mk.playheadAfter) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` answered ok (\"%s\") and landed on %lld us, not on the bookmark at %lld "
                    "us (from %lld us)", st.asked, st.said,
                    static_cast<long long>(st.playheadAfter),
                    static_cast<long long>(mk.playheadAfter),
                    static_cast<long long>(st.playheadBefore));
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "`%s` landed on the bookmark at %lld us (from %lld us): \"%s\"", st.asked,
                static_cast<long long>(st.playheadAfter),
                static_cast<long long>(st.playheadBefore), st.said);
    return Ask::Pass;
}

// T15. Two `goto next`s from 0 must reach the FIRST bookmark and then the SECOND -- never the
// first twice, which is what a walk that reads "nearest after" as "nearest" does.
inline Ask AskGotoNextWalksMarkers(const StatusFacts& s, char* d, size_t n) {
    const Ask first = AskGotoWalk(s, Verb::GotoNextFirst, Verb::Mark, d, n);
    if (first != Ask::Pass) { return first; }
    const Ask second = AskGotoWalk(s, Verb::GotoNextSecond, Verb::MarkSecond, d, n);
    if (second != Ask::Pass) { return second; }
    const TransportStep& a = StepFor(s, Verb::GotoNextFirst);
    const TransportStep& b = StepFor(s, Verb::GotoNextSecond);
    if (a.playheadAfter == b.playheadAfter) {
        _snprintf_s(d, n, _TRUNCATE,
                    "two `goto next`s both landed on %lld us: the walk does not advance past the "
                    "bookmark it is standing on", static_cast<long long>(a.playheadAfter));
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "`goto next` twice from 0 us walked the bookmarks in order: %lld us then %lld us, "
                "never twice on the first", static_cast<long long>(a.playheadAfter),
                static_cast<long long>(b.playheadAfter));
    return Ask::Pass;
}

// T16. `goto prev` from the last bookmark lands on the one strictly before it.
inline Ask AskGotoPrevWalksMarkers(const StatusFacts& s, char* d, size_t n) {
    const Ask a = AskGotoWalk(s, Verb::GotoPrev, Verb::Mark, d, n);
    if (a != Ask::Pass) { return a; }
    const TransportStep& st = StepFor(s, Verb::GotoPrev);
    if (st.playheadAfter >= st.playheadBefore) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`goto prev` landed on %lld us from %lld us -- not STRICTLY before where it "
                    "started", static_cast<long long>(st.playheadAfter),
                    static_cast<long long>(st.playheadBefore));
        return Ask::Fail;
    }
    return Ask::Pass;
}

// T17. The refusal IS the pass, and the playhead must not have moved across the call: a wrap to
// the start is a silent jump backwards dressed as a step forwards.
inline Ask AskGotoNextAtEndDoesNotWrap(const StatusFacts& s, char* d, size_t n) {
    if (!StepScoreable(s, Verb::GotoNextAtEnd, d, n)) { return Ask::CouldNotAsk; }
    const TransportStep& st = StepFor(s, Verb::GotoNextAtEnd);
    const TransportStep& mk = StepFor(s, Verb::MarkSecond);
    if (!mk.attempted || !mk.ok) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`goto next` was issued at what should have been the last bookmark, but the "
                    "second `mark` never made one (%s), so this launch cannot say the playhead was "
                    "at the end of the marker list", mk.attempted ? mk.said : "never issued");
        return Ask::CouldNotAsk;
    }
    if (st.ok) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`goto next` at the last bookmark SUCCEEDED: \"%s\" -- %lld us -> %lld us. "
                    "`goto next` does not wrap; a success here is a silent jump backwards through "
                    "the file", st.said, static_cast<long long>(st.playheadBefore),
                    static_cast<long long>(st.playheadAfter));
        return Ask::Fail;
    }
    if (st.playheadAfter != st.playheadBefore) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`goto next` at the last bookmark refused (\"%s\") and the playhead MOVED "
                    "anyway: %lld us -> %lld us. A refused command that moves the playhead is "
                    "worse than one that wraps, because nothing reports it", st.said,
                    static_cast<long long>(st.playheadBefore),
                    static_cast<long long>(st.playheadAfter));
        return Ask::Fail;
    }
    if (strstr(st.said, "does not wrap") == nullptr) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`goto next` at the last bookmark refused without saying why it will not go "
                    "on: \"%s\". This refusal has to be distinguishable from the empty-sidecar "
                    "one, which is answered by `mark` and this one is not", st.said);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "`goto next` at the last bookmark refused naming the reason (\"%s\") and left the "
                "playhead at %lld us", st.said, static_cast<long long>(st.playheadAfter));
    return Ask::Pass;
}

// T18. Also a refusal-is-pass, and its words must NOT be T17's: one is answered by `mark` and the
// other is not, so a reader that cannot tell them apart tells the user to do the wrong thing.
inline Ask AskGotoNextNoBookmarksRefused(const StatusFacts& s, char* d, size_t n) {
    if (!s.bookmarksAtStartKnown) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the battery never read the bookmark count before making its own mark, so this "
                    "launch cannot say the capture started with an empty sidecar");
        return Ask::CouldNotAsk;
    }
    if (s.bookmarksAtStart != 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "this capture's sidecar already carried %zu bookmark(s) when the battery "
                    "started, so the empty-sidecar refusal was not reachable on this launch",
                    s.bookmarksAtStart);
        return Ask::CouldNotAsk;
    }
    if (!StepScoreable(s, Verb::GotoNextNoMarkers, d, n)) { return Ask::CouldNotAsk; }
    const TransportStep& st = StepFor(s, Verb::GotoNextNoMarkers);
    if (st.ok) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`goto next` with an empty sidecar SUCCEEDED: \"%s\" -- %lld us -> %lld us. "
                    "There was no bookmark to land on", st.said,
                    static_cast<long long>(st.playheadBefore),
                    static_cast<long long>(st.playheadAfter));
        return Ask::Fail;
    }
    if (strstr(st.said, "no bookmarks") == nullptr) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`goto next` with an empty sidecar refused without naming the cause: \"%s\"",
                    st.said);
        return Ask::Fail;
    }
    const TransportStep& edge = StepFor(s, Verb::GotoNextAtEnd);
    if (edge.attempted && edge.answered && !edge.ok && strcmp(st.said, edge.said) == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`goto next` says the SAME thing with an empty sidecar as it does at the last "
                    "bookmark: \"%s\". One of those is answered by `mark` and the other is not",
                    st.said);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "`goto next` with an empty sidecar refused naming the cause (\"%s\"), and it is "
                "not the words the end-of-list refusal uses", st.said);
    return Ask::Pass;
}

// ---------------------------------------------------------------------------------------------
// W3-A's fire counter, which is what L3-RUN-2 F6 asked for. `fires` counts RAISES, so the
// could-not-ask arm the effect-sum row had to keep -- "did not fire" vs "fired with nothing
// bound" -- is gone: exactly one fire per commanded discontinuity, and it must be Cause::Seek.
// ---------------------------------------------------------------------------------------------
inline Ask AskResidueFiresExactlyOnce(const StatusFacts& s, char* d, size_t n) {
    if (!s.batteryDispatched) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the playback battery never ran (durationUs %llu), so no discontinuity was "
                    "commanded and residue::Stats().fires was never differenced",
                    static_cast<unsigned long long>(s.durationUs));
        return Ask::CouldNotAsk;
    }
    static const Verb kJumps[] = {Verb::Seek, Verb::StepForward, Verb::StepBack, Verb::Round,
                                  Verb::SeekAway, Verb::Goto};
    bool armed = false;
    for (size_t i = 0; i < static_cast<size_t>(Verb::Count); ++i) {
        if (s.steps[i].attempted && s.steps[i].residueArmed) { armed = true; }
    }
    if (!armed) {
        _snprintf_s(d, n, _TRUNCATE,
                    "residue::Stats().armed is false on every step: ArmPlaybackResidue never ran, "
                    "so nothing was listening for the Cause::Seek these jumps raise");
        return Ask::CouldNotAsk;
    }
    unsigned jumps = 0, wrongCount = 0, wrongCause = 0;
    const char* firstWrongCount = "";
    const char* firstWrongCause = "";
    unsigned long long firstWrongFires = 0;
    int firstWrongCauseCode = -1;
    for (size_t i = 0; i < sizeof kJumps / sizeof kJumps[0]; ++i) {
        const TransportStep& st = StepFor(s, kJumps[i]);
        if (!st.attempted || !st.answered || !st.ok) { continue; }
        if (st.residueCauseAfter < 0) { continue; }
        ++jumps;
        const unsigned long long fired = st.residueFiresAfter - st.residueFiresBefore;
        if (fired != 1) {
            if (wrongCount == 0) { firstWrongCount = st.asked; firstWrongFires = fired; }
            ++wrongCount;
            continue;
        }
        if (st.residueSeekFiresAfter - st.residueSeekFiresBefore != 1) {
            if (wrongCause == 0) {
                firstWrongCause = st.asked;
                firstWrongCauseCode = st.residueCauseAfter;
            }
            ++wrongCause;
        }
    }
    if (jumps == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the residue battery is armed and not one of the six discontinuity commands "
                    "(seek, step +1, step -1, round, seek away, goto) was both accepted and read "
                    "back with a fire count, so there is nothing to difference");
        return Ask::CouldNotAsk;
    }
    if (wrongCount != 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%u of %u commanded discontinuity(ies) did not raise EXACTLY ONE residue fire; "
                    "the first was `%s`, which raised %llu. residue::Stats().fires counts raises, "
                    "not effects, so this is the battery being fired the wrong number of times and "
                    "not a battery with nothing bound", wrongCount, jumps, firstWrongCount,
                    firstWrongFires);
        return Ask::Fail;
    }
    if (wrongCause != 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%u of %u commanded discontinuity(ies) raised one fire under the WRONG cause; "
                    "the first was `%s`, filed under residue::Cause %d rather than Cause::Seek. "
                    "Every one of these six verbs is a Seek to the residue packet", wrongCause,
                    jumps, firstWrongCause, firstWrongCauseCode);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "all %u commanded discontinuity(ies) raised EXACTLY ONE residue fire each, every "
                "one of them under Cause::Seek (residue::Stats().fires / .firesByCause[Seek])",
                jumps);
    return Ask::Pass;
}

// ---------------------------------------------------------------------------------------------
// P10-F. Four rows over words the PRODUCT publishes: seekgoto.h's off-target landing and
// camera_control.h's two cam-pose refusals. Every word scored here arrives in StatusFacts from
// the product's own name function -- this header still includes no product header, and a row that
// spelled `facing-degenerate` itself would keep passing after the product renamed it.
//
// Both helpers below are DELIMITED, never substring: `SaysWord` refuses a hit that runs into a
// longer word, and `RefusalWordIs` reads only the one place the refusal formatter puts the word
// (campose.h FormatCamPoseRefused: `<verb> refused: <word> -- <diagnostic>`), so a diagnostic
// that merely quotes the word cannot answer for the refusal itself.
// ---------------------------------------------------------------------------------------------
inline bool SaysWord(const char* said, const char* token) {
    if (said == nullptr || token == nullptr || token[0] == 0) { return false; }
    const size_t len = strlen(token);
    for (const char* p = strstr(said, token); p != nullptr; p = strstr(p + 1, token)) {
        const char before = (p == said) ? ' ' : p[-1];
        const char after = p[len];
        const bool lead = before == ' ' || before == ':' || before == '(' || before == '"';
        const bool trail = after == 0 || after == ' ' || after == ',' || after == '.' ||
                           after == '"' || after == ')';
        if (lead && trail) { return true; }
    }
    return false;
}

inline bool RefusalWordIs(const char* said, const char* token) {
    if (said == nullptr || token == nullptr || token[0] == 0) { return false; }
    const char* at = strstr(said, "refused: ");
    if (at == nullptr) { return false; }
    at += 9;
    const size_t len = strlen(token);
    if (strncmp(at, token, len) != 0) { return false; }
    const char tail = at[len];
    return tail == 0 || tail == ' ';
}

// The cause after the LAST `refused: `. camera_control's diagnostic is a WHOLE refusal sentence
// nested inside dispatch's one: `cam look at 1 refused: look-at-unresolvable -- cam look at 1
// refused: <SubjectResolveName>`. RefusalWordIs takes the FIRST segment, whose payload is the
// ControlRefusal ENUM word -- which is what p10-cam-7b wants and the wrong end of this sentence
// for the cause. Same tail rule, so a token that is only a prefix of the payload is not it.
inline bool SubjectCauseIs(const char* said, const char* token) {
    if (said == nullptr || token == nullptr || token[0] == 0) { return false; }
    const char* last = nullptr;
    for (const char* at = strstr(said, "refused: "); at != nullptr;
         at = strstr(at + 1, "refused: ")) {
        last = at;
    }
    if (last == nullptr) { return false; }
    last += 9;
    const size_t len = strlen(token);
    if (strncmp(last, token, len) != 0) { return false; }
    const char tail = last[len];
    return tail == 0 || tail == ' ';
}

inline unsigned TokenLen(const char* spelling) {
    return spelling != nullptr ? static_cast<unsigned>(strlen(spelling)) : 0u;
}

// Copies a spelling the PRODUCT owns into a published token buffer, or REFUSES. A _TRUNCATE copy
// of a word longer than its buffer publishes a field that looks filled and can never match, which
// is the fixture that cannot fail wearing a char array. On refusal the buffer is left empty, so a
// row reading it sees "nothing was published here" instead of scoring against a prefix.
inline bool PublishToken(char* dst, size_t cap, const char* spelling) {
    if (dst == nullptr || cap == 0) { return false; }
    dst[0] = 0;
    if (spelling == nullptr || spelling[0] == 0) { return false; }
    const size_t len = strlen(spelling);
    if (len + 1 > cap) { return false; }
    memcpy(dst, spelling, len + 1);
    return true;
}

// The LAST `<number> us` in a sentence. dispatch.cpp's landing arms print the read-back playhead
// last, so the final one is the landing claim; a sentence carrying none makes no claim at all and
// the row says that rather than inventing one.
inline bool LastMicrosecondsIn(const char* said, long long* out) {
    if (said == nullptr || out == nullptr) { return false; }
    bool found = false;
    for (const char* p = said; *p != 0; ++p) {
        if (!(p[0] == ' ' && p[1] == 'u' && p[2] == 's')) { continue; }
        const char* end = p;
        const char* begin = end;
        while (begin > said && begin[-1] >= '0' && begin[-1] <= '9') { --begin; }
        if (begin == end) { continue; }
        const bool negative = begin > said && begin[-1] == '-';
        long long value = 0;
        for (const char* q = begin; q < end; ++q) { value = value * 10 + (*q - '0'); }
        *out = negative ? -value : value;
        found = true;
    }
    return found;
}

// L3-RUN-3 F1, stated as a predicate. A relative seek either names the
// microsecond the playhead IS at, or it refuses by seekgoto.h's own name. The number in the
// sentence is compared against the transport's own read-back, and the product's own
// kSeekLandingToleranceUs is the only window in which a difference is left ungraded -- the two
// reads are taken at different moments and this row will not call a stale read a lie.
inline Ask AskSeekLandingPoint(const StatusFacts& s, Verb v, char* d, size_t n) {
    if (!StepScoreable(s, v, d, n)) { return Ask::CouldNotAsk; }
    const TransportStep& st = StepFor(s, v);
    if (s.seekLandingOffToken[0] == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the probe published no off-target word from control/seekgoto.h, so a refusal "
                    "could only be scored against a spelling of this row's own");
        return Ask::CouldNotAsk;
    }
    if (!st.ok) {
        if (SaysWord(st.said, s.seekLandingOffToken)) {
            _snprintf_s(d, n, _TRUNCATE,
                        "`%s` REFUSED by name and said so: \"%s\". seekgoto.h's `%s` is exactly "
                        "the arm declining to print a microsecond the playhead is not at",
                        st.asked, st.said, s.seekLandingOffToken);
            return Ask::Pass;
        }
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` was refused for some other reason (\"%s\"), which names neither a "
                    "landing nor `%s`: this row's subject never arose", st.asked, st.said,
                    s.seekLandingOffToken);
        return Ask::CouldNotAsk;
    }
    long long claimed = 0;
    if (!LastMicrosecondsIn(st.said, &claimed)) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` answered ok (\"%s\") and its sentence names no microsecond at all: an "
                    "accepted seek that will not say where it landed leaves the operator with the "
                    "delta they typed as their only reading", st.asked, st.said);
        return Ask::Fail;
    }
    const long long at = st.playheadAfter;
    const long long away = claimed > at ? claimed - at : at - claimed;
    if (away == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` answered ok and its sentence names %lld us, which is where the "
                    "transport's own playhead read back: \"%s\"", st.asked, claimed, st.said);
        return Ask::Pass;
    }
    // A tolerance of 0 is UNKNOWN, not zero. It means the transport published no
    // tick spacing for this capture, and without one there is no interval that tells a neighbouring
    // recorded tick from a false statement -- so this row declines to score rather than calling
    // every non-exact landing a defect.
    if (s.seekLandingToleranceUs <= 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` names %lld us and the playhead read back %lld us -- %lld us apart, and "
                    "this capture published no tick spacing to judge that against, so this row "
                    "will not say whether it is the neighbouring recorded tick or a miss",
                    st.asked, claimed, at, away);
        return Ask::CouldNotAsk;
    }
    if (away <= s.seekLandingToleranceUs) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` names %lld us and the playhead read back %lld us -- %lld us apart, "
                    "inside this capture's own tick spacing (%lld us, from "
                    "IReplayTransport::TickSpacingUs). The two were read at different moments and "
                    "this row will not call a stale read a false statement",
                    st.asked, claimed, at, away, s.seekLandingToleranceUs);
        return Ask::CouldNotAsk;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "`%s` answered ok (\"%s\") and names %lld us while the playhead read back %lld us "
                "-- %lld us away. That is a sentence stating a position nobody asked the transport "
                "for, which is L3-RUN-3 F1 exactly", st.asked, st.said, claimed, at, away);
    return Ask::Fail;
}

// p10-seek-landing, scored over FOUR seek points rather than one (L3-RUN-4 s6). One point is
// not a denominator: a landing sentence that is right about a relative nudge and wrong about
// every absolute jump passed this row. The four are the relative nudge and absolute seeks at
// 25, 50 and 90 percent of the file, and the row publishes how many of them it could score --
// a pass over one point says so rather than reading like a pass over the transport.
inline Ask AskSeekReplyNamesItsLanding(const StatusFacts& s, char* d, size_t n) {
    static const Verb kPoints[] = {Verb::SeekRelForward, Verb::SeekAbs25, Verb::SeekAbs50,
                                   Verb::SeekAbs90};
    const unsigned total = static_cast<unsigned>(sizeof kPoints / sizeof kPoints[0]);
    char firstSkip[900] = {0};
    char point[900];
    unsigned passed = 0, skipped = 0;
    for (unsigned i = 0; i < total; ++i) {
        point[0] = '\0';
        const Ask a = AskSeekLandingPoint(s, kPoints[i], point, sizeof point);
        if (a == Ask::Fail) {
            _snprintf_s(d, n, _TRUNCATE, "of %u seek point(s), `%s` FAILED: %s", total,
                        VerbName(kPoints[i]), point);
            return Ask::Fail;
        }
        if (a == Ask::Pass) {
            ++passed;
        } else {
            ++skipped;
            if (firstSkip[0] == '\0') {
                _snprintf_s(firstSkip, sizeof firstSkip, _TRUNCATE, "`%s`: %s",
                            VerbName(kPoints[i]), point);
            }
        }
    }
    if (passed == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "none of the %u seek point(s) could be scored; the first says %s", total,
                    firstSkip);
        return Ask::CouldNotAsk;
    }
    if (skipped > 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%u of %u seek point(s) named the microsecond the playhead read back; %u "
                    "could not be scored -- the first says %s", passed, total, skipped,
                    firstSkip);
        return Ask::Pass;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "all %u seek point(s) -- the relative nudge and absolute seeks at 25, 50 and 90 "
                "percent of the file -- named the microsecond the transport's own playhead read "
                "back", total);
    return Ask::Pass;
}

// p10-cam-7 (ARCH-19, ENG-W2G-P17E S7). `cam look to 30 0` from a bound rig. The landing is taken
// from THIS probe's own SampleViewPose pair, never from Result::ok: a command that was accepted
// and whose pose could not be read back is could-not-ask, because the write landed and nobody can
// say where.
inline constexpr double kCamLookYawToleranceDeg = 0.5;

inline double YawErrorDeg(double landed, double asked) {
    double diff = landed - asked;
    while (diff > 180.0) { diff -= 360.0; }
    while (diff <= -180.0) { diff += 360.0; }
    return diff < 0.0 ? -diff : diff;
}

// L3-RUN-5 F2. All three cam-pose rows refuse the same way, and all three must name the same
// fact: camera_control::Get() being null is an answer about the CAMERA only if somebody waited for
// the rig to bind. On the launch that found this the rig bound 1.8 s in, the probe had asked
// before it, and three rows filed "null on this launch" against a session whose own log carried
// `[camera] camera control bound` -- a sentence contradicted by the run that printed it. The probe
// waits now (AwaitCameraBind); this composes the refusal ONCE, so a fourth cam row cannot grow a
// private copy that forgets to mention the bound.
inline void CamBindRefusal(const StatusFacts& s, char* d, size_t n, const char* consequence) {
    if (!s.camBindWaitKnown) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the cam-pose battery never ran and this probe published no bind wait at all, "
                    "so camera_control::Get() being null says nothing about the rig -- it may "
                    "simply have been asked before the bind, and %s. That is a fact about the "
                    "BUILD: this probe predates the bounded wait for the rig to bind",
                    consequence);
        return;
    }
    if (s.camBindBound) {
        _snprintf_s(d, n, _TRUNCATE,
                    "camera_control::Get() answered after %lld ms of a %lld ms bound, but the "
                    "cam-pose battery still did not run, so %s. The rig DID bind and something "
                    "else stopped the arm -- this is not the pre-bind timing defect",
                    s.camBindWaitedMs, s.camBindBudgetMs, consequence);
        return;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "the cam-pose battery could not be asked: camera_control::Get() was still null "
                "after the probe waited %lld ms of its %lld ms bound for the rig to bind, so %s. "
                "This is a bound that EXPIRED, not a seam sampled too early -- if the run's "
                "`[camera] camera control bound` line is present (the line w2e-camera-rig-probe "
                "reads), it landed after that bound",
                s.camBindWaitedMs, s.camBindBudgetMs, consequence);
}

inline Ask AskCamLookLandsOnYaw(const StatusFacts& s, char* d, size_t n) {
    if (!s.camPoseAsked) {
        CamBindRefusal(s, d, n, "no rig was bound to aim");
        return Ask::CouldNotAsk;
    }
    const TransportStep& st = StepFor(s, Verb::CamLookAbsolute);
    if (!st.attempted || !st.answered) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` was %s, so there is no answer to score", VerbName(Verb::CamLookAbsolute),
                    st.attempted ? "issued and never answered" : "never issued");
        return Ask::CouldNotAsk;
    }
    if (!st.ok) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` was REFUSED from a bound rig: \"%s\". An absolute look is the plainest "
                    "thing the free camera does, so a refusal here is the owner's `freely move "
                    "around` out of reach", st.asked, st.said);
        return Ask::Fail;
    }
    if (!s.camFacingReadAfter) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` answered ok (\"%s\") and camera_control::SampleViewPose did NOT answer "
                    "Ok afterwards, so this row has no landing of its own. ARCH-19 E3: the Result "
                    "bool does not stand in for a pose nobody read", st.asked, st.said);
        return Ask::CouldNotAsk;
    }
    if (s.camFacingVerticalAfter) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` answered ok (\"%s\") and the landed facing is VERTICAL -- DecomposeFacing "
                    "refuses it, so it carries no yaw at all -- where pitch %.1f was asked for",
                    st.asked, st.said, s.camLookPitchAsked);
        return Ask::Fail;
    }
    const double err = YawErrorDeg(static_cast<double>(s.camYawAfter), s.camLookYawAsked);
    if (err <= kCamLookYawToleranceDeg) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` landed on yaw %.2f deg against the %.2f asked for (%.2f deg out, inside "
                    "%.1f), read back through camera_control::SampleViewPose / DecomposeFacing and "
                    "not off the reply: \"%s\"", st.asked, static_cast<double>(s.camYawAfter),
                    s.camLookYawAsked, err, kCamLookYawToleranceDeg, st.said);
        return Ask::Pass;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "`%s` answered ok (\"%s\") and the pose read back at yaw %.2f deg -- %.2f deg off "
                "the %.2f that was commanded. The reply is not the landing; this is",
                st.asked, st.said, static_cast<double>(s.camYawAfter), err, s.camLookYawAsked);
    return Ask::Fail;
}

// p10-cam-7b. The sibling refusal. `cam look 10 0` is RELATIVE, so on a facing within 0.1 deg of
// vertical there is no yaw axis to add to and camera_control refuses `facing-degenerate`
// (camera_control.cpp:770). The row does not manufacture that facing -- it scores the refusal WHEN
// the rig held one, and says plainly that the branch was not reached when it did not.
inline Ask AskCamLookRefusesDegenerateFacing(const StatusFacts& s, char* d, size_t n) {
    if (!s.camPoseAsked) {
        CamBindRefusal(s, d, n, "no rig held a facing to be degenerate");
        return Ask::CouldNotAsk;
    }
    if (s.facingDegenerateToken[0] == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the probe published no word from camera_control::ControlRefusalName for this "
                    "refusal, so it could only be scored against a spelling of this row's own");
        return Ask::CouldNotAsk;
    }
    const TransportStep& st = StepFor(s, Verb::CamLookRelative);
    if (!st.attempted || !st.answered) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` was %s, so there is no answer to score", VerbName(Verb::CamLookRelative),
                    st.attempted ? "issued and never answered" : "never issued");
        return Ask::CouldNotAsk;
    }
    if (!s.camFacingReadBefore) {
        _snprintf_s(d, n, _TRUNCATE,
                    "camera_control::SampleViewPose did not answer Ok before `%s`, so whether the "
                    "facing this row is about was even held is unknown", st.asked);
        return Ask::CouldNotAsk;
    }
    if (!s.camFacingVerticalBefore) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the rig's facing was not vertical when `%s` was issued (it decomposed to yaw "
                    "%.2f deg), so the degenerate branch was never reached. This row scores that "
                    "refusal where it arises and does not pose the camera to provoke it",
                    st.asked, static_cast<double>(s.camYawBefore));
        return Ask::CouldNotAsk;
    }
    if (st.ok) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` was ACCEPTED (\"%s\") on a facing DecomposeFacing calls vertical. A yaw "
                    "added to a vertical facing has no answer, so an ok here is a pose the rig "
                    "cannot have computed", st.asked, st.said);
        return Ask::Fail;
    }
    if (!RefusalWordIs(st.said, s.facingDegenerateToken)) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` refused on a vertical facing but NOT by camera_control's own name for "
                    "it: `%s` is the word and the sentence was \"%s\"", st.asked,
                    s.facingDegenerateToken, st.said);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "`%s` on a vertical facing refused by name: `%s` -- \"%s\"", st.asked,
                s.facingDegenerateToken, st.said);
    return Ask::Pass;
}

// p10-cam-9, POST-COMPOSER. ENG-W1E-SUBJECT `eb31bd4` installed the subject-pose seam, so
// `look-at-unresolvable` -- which camera_control returns only when InstalledSubjectPose() is null
// -- is no longer this row's pass. It is now the install line in entity_module::Init not having
// run, and that is a FAIL.
//
// ENG-W2G-LOOKAT S7 splits the row in two halves that are scored separately, because A can pass
// in a solo session while B cannot:
//   A -- THE PACKING. PackSelector(1..10) must all pack, the ten PackedSelectorWord values must
//        be DISTINCT (a constant-returning packer cannot pass), and 0 and max+1 must be refused.
//        Pure arithmetic on the product's own functions: no roster, no second player.
//   B -- THE AIM, and it is scored from the ENGINE, never from Result::ok and never from our own
//        sentence. The oracle is the engine's own ci+0x26 word read back by ObservedSelector
//        against PackedSelectorWord(team, slot) for the same n, which is what
//        SelectorDerivationState answers. Certified is the pass; Falsified is the fail and is
//        STICKY; an accepted aim nobody could read back is could-not-ask, because the write
//        landed and nobody can say where.
//
// On a solo -offline host the expected outcomes are: a resolved aim for a selector that is the
// owner's own slot, and `no-such-subject` for an empty one. The second is this session having one
// player, not the arm misbehaving, so it is could-not-ask -- as are `roster-unreadable` and
// `no-body`. Any OTHER refusal word is a FAIL.
//
// Out-of-range selectors are NOT this row's subject: ENG-W2G-POV S3 moved that refusal to the
// PARSER, where `cam look at 0` / `cam pov 0` / `cam follow 0` all answer `selector-out-of-range`
// naming `the player selector is 1 to 10` and `got "0"`. tests/control/test_control.cpp sweeps
// the three arms against that text; the battery only ever issues an in-range `cam look at 1`, so
// a copy of the parser's words here would be a second spelling nobody drives.
inline Ask AskCamLookAtSubjectResolves(const StatusFacts& s, char* d, size_t n) {
    if (!s.camPoseAsked) {
        CamBindRefusal(s, d, n, "no selector was ever put to a rig");
        return Ask::CouldNotAsk;
    }

    // --- A, the packing.
    if (!s.camPackKnown) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the probe put no selector to camera_control::PackSelector on this launch, so "
                    "the n -> (teamId, teamSlot) split this row certifies was never derived");
        return Ask::CouldNotAsk;
    }
    if (!s.camPackAllPacked) {
        _snprintf_s(d, n, _TRUNCATE,
                    "camera_control::PackSelector refused a selector inside 1..10, so the aim "
                    "cannot be asked for every player the UI offers");
        return Ask::Fail;
    }
    if (!s.camPackDistinct) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the ten PackedSelectorWord values are not all different: two selectors pack "
                    "to one engine word, so an aim certified for one player is certified for "
                    "another. A constant-returning packer looks exactly like this");
        return Ask::Fail;
    }
    if (!s.camPackRefusesZero || !s.camPackRefusesEleven) {
        _snprintf_s(d, n, _TRUNCATE,
                    "PackSelector accepted %s: an out-of-range selector that packs hands the "
                    "engine a (teamId, teamSlot) nobody bounded",
                    !s.camPackRefusesZero && !s.camPackRefusesEleven ? "both 0 and max+1"
                    : (!s.camPackRefusesZero ? "0" : "max+1"));
        return Ask::Fail;
    }

    // --- B, the aim.
    if (s.lookAtUnresolvableToken[0] == 0 || s.subjNoSuchSubjectToken[0] == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "THE PACKING IS CERTIFIED (ten distinct words, 0 and max+1 refused), but the "
                    "probe published no word from camera_control for an unresolved subject, so the "
                    "aim could only be scored against a spelling of this row's own");
        return Ask::CouldNotAsk;
    }
    const TransportStep& st = StepFor(s, Verb::CamLookAtSubject);
    if (!st.attempted || !st.answered) {
        _snprintf_s(d, n, _TRUNCATE,
                    "THE PACKING IS CERTIFIED, but `%s` was %s, so there is no aim to score",
                    VerbName(Verb::CamLookAtSubject),
                    st.attempted ? "issued and never answered" : "never issued");
        return Ask::CouldNotAsk;
    }
    if (!st.ok) {
        // ENG-W2R-ROWS3B (L3-RUN-6 F4). `look-at-unresolvable` is FIVE causes wearing one word:
        // camera_control.cpp's p.lookAt block returns that single ControlRefusal for every
        // non-Resolved SubjectResolve and leaves the cause in the diagnostic alone. So the enum
        // word opens the arm and settles NOTHING; what this row scores is the cause, which lives
        // after the SECOND `refused: ` of the sentence -- see SubjectCauseIs.
        // And it goes FIRST because the arms below all read the sentence as a
        // statement about the composer. A rig released across this dispatch is refused by
        // dispatch.cpp's own "the spectator rig has not checked out" -- correct behaviour, and a
        // word from outside this row's vocabulary. Scoring it as `a refusal nobody in this row's
        // chain issued` is a confident wrong accusation for the exact loss L3-RUN-28 recorded, so
        // the loss is named here, the same deferral the read-back arm files.
#ifndef ROW_IGNORES_RIG_RELEASED
        if (s.camRigAtLookAt == RigPresence::Released) {
            _snprintf_s(d, n, _TRUNCATE,
                        "THE PACKING IS CERTIFIED, but the spectator rig was RELEASED across `%s` "
                        "itself: camera_control::Get() answered null at one end of the dispatch, so "
                        "the refusal it came back with (\"%s\") is the rig being gone and not a "
                        "verdict on the aim. `%s` is the loss -- nothing here failed and no "
                        "composer refusal was issued",
                        st.asked, st.said, RigPresenceName(s.camRigAtLookAt));
            return Ask::CouldNotAsk;
        }
#endif
        if (!RefusalWordIs(st.said, s.lookAtUnresolvableToken)) {
            _snprintf_s(d, n, _TRUNCATE,
                        "`%s` refused by a word that is not camera_control's `%s`: \"%s\". The "
                        "cam-pose arm answers exactly one refusal for an unresolved subject, so a "
                        "different word is a refusal nobody in this row's chain issued",
                        st.asked, s.lookAtUnresolvableToken, st.said);
            return Ask::Fail;
        }
        if (s.subjDerivationUncertifiedToken[0] == 0 || s.subjNoSeamToken[0] == 0) {
            _snprintf_s(d, n, _TRUNCATE,
                        "`%s` refused `%s` (\"%s\") and the probe published no camera_control "
                        "spelling for %s: %u of its SubjectResolve name(s) did not fit their "
                        "published buffers (longest spelling %u bytes). A truncated token matches "
                        "nothing, so the cause cannot be read without a spelling of this row's own",
                        st.asked, s.lookAtUnresolvableToken, st.said,
                        s.subjDerivationUncertifiedToken[0] == 0 ? "DerivationUncertified"
                                                                 : "NoSeam",
                        s.subjTokenDropped, s.subjTokenLongestLen);
            return Ask::CouldNotAsk;
        }
        if (SubjectCauseIs(st.said, s.subjDerivationUncertifiedToken)) {
            _snprintf_s(d, n, _TRUNCATE,
                        "COULD-NOT-ASK, CERTIFICATION PENDING: `%s` reached the composer and it "
                        "refused because the selector derivation is not certified against this "
                        "engine yet (\"%s\"). That certification is per SESSION and attended -- it "
                        "is granted by running `cam pov <n>` once on this engine, which nobody did "
                        "on this launch -- so the aim was never asked and nothing here failed",
                        st.asked, st.said);
            return Ask::CouldNotAsk;
        }
        if (SubjectCauseIs(st.said, s.subjNoSeamToken)) {
            _snprintf_s(d, n, _TRUNCATE,
                        "COULD-NOT-ASK: `%s` refused because there is no subject-pose seam in this "
                        "image (\"%s\"). An UNINSTALLED seam is a build state, not this product "
                        "failing to aim: there is no composer in the image for the aim to reach -- "
                        "%s", st.asked, st.said,
                        s.subjectPoseInstalledKnown
                            ? (s.subjectPoseInstalled
                                   ? "and this probe read InstalledSubjectPose() back as INSTALLED, "
                                     "so the seam this refusal names is not the one it read"
                                   : "and this probe read InstalledSubjectPose() back as NOT "
                                     "installed, which agrees with the refusal")
                            : "which this probe did not read back");
            return Ask::CouldNotAsk;
        }
        const bool emptySlot = SubjectCauseIs(st.said, s.subjNoSuchSubjectToken);
        const bool rosterDead = s.subjRosterUnreadableToken[0] != 0 &&
                                SubjectCauseIs(st.said, s.subjRosterUnreadableToken);
        if (emptySlot || rosterDead) {
            // Occupancy comes from the CLOSE's own roster row count (LastStopFacts, `f1f475e`),
            // never from the selector number: `cam look at 3` on a one-player host is this
            // session having one player. A roster that recorded the slot and a composer that
            // cannot find it are two different facts and only the second is an asked-fail.
            if (!s.sentinelRosterRowsKnown || s.camLookAtSelector <= 0) {
                _snprintf_s(d, n, _TRUNCATE,
                            "COULD-NOT-ASK: `%s` refused with \"%s\", and %s, so this row cannot "
                            "say whether the slot it aimed at was occupied. An empty slot on a "
                            "solo host and a composer that lost an occupied one are different "
                            "answers and this record separates neither", st.asked, st.said,
                            s.camLookAtSelector <= 0
                                ? "the probe recorded no selector for the aim"
                                : "no capture close has published a roster row count to this "
                                  "observer");
                return Ask::CouldNotAsk;
            }
            if (s.sentinelRosterRows < static_cast<unsigned long>(s.camLookAtSelector)) {
                _snprintf_s(d, n, _TRUNCATE,
                            "COULD-NOT-ASK: `%s` refused with \"%s\", and the close's own roster "
                            "recorded %lu row(s) -- fewer than the %d this aim names. Selector %d "
                            "has no occupant on this host, so the arm was never asked",
                            st.asked, st.said, s.sentinelRosterRows, s.camLookAtSelector,
                            s.camLookAtSelector);
                return Ask::CouldNotAsk;
            }
            _snprintf_s(d, n, _TRUNCATE,
                        "ASKED AND FAILED: the close's own roster recorded %lu row(s), so selector "
                        "%d IS occupied, and `%s` still refused: \"%s\". %s over a slot the "
                        "capture recorded is the composer losing a player that is in the file",
                        s.sentinelRosterRows, s.camLookAtSelector, st.asked, st.said,
                        rosterDead ? "An unreadable roster" : "No such subject");
            return Ask::Fail;
        }
        if (s.subjNoBodyToken[0] != 0 && SubjectCauseIs(st.said, s.subjNoBodyToken)) {
            _snprintf_s(d, n, _TRUNCATE,
                        "COULD-NOT-ASK: `%s` refused because that player has no physics body this "
                        "tick (\"%s\"). A body that is not in the world on the sampled tick is "
                        "not an aim this row can score", st.asked, st.said);
            return Ask::CouldNotAsk;
        }
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` refused `%s` and its diagnostic names NONE of camera_control's "
                    "subject-resolve causes (`%s`, `%s`, `%s`, `%s`, `%s`): \"%s\". A refusal "
                    "that folds five causes into one word carries the cause only in that "
                    "sentence, so a sentence naming none of them says which of the five happened "
                    "not at all", st.asked, s.lookAtUnresolvableToken,
                    s.subjNoSuchSubjectToken,
                    s.subjRosterUnreadableToken[0] != 0 ? s.subjRosterUnreadableToken : "(unpublished)",
                    s.subjNoBodyToken[0] != 0 ? s.subjNoBodyToken : "(unpublished)",
                    s.subjNoSeamToken, s.subjDerivationUncertifiedToken, st.said);
        return Ask::Fail;
    }
    if (s.camSelectorDerivation[0] == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` was ACCEPTED (\"%s\") and the probe published no "
                    "SelectorDerivationName for it, so the aim would have to be taken from our own "
                    "sentence -- which is the one reading ENG-W2G-LOOKAT S7 forbids", st.asked,
                    st.said);
        return Ask::CouldNotAsk;
    }
    if (s.camSelectorFalsified) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` was accepted (\"%s\") and the engine's own selector word is FALSIFIED: "
                    "ci+0x26 read back %d where PackedSelectorWord(team, slot) for this selector is "
                    "%d. The aim went to a player our split does not name, and that verdict is "
                    "sticky", st.asked, st.said, s.camSelectorPacked, s.camSelectorExpected);
        return Ask::Fail;
    }
    if (s.camSelectorReadBack == SelectorReadBack::RigReleased) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` was accepted (\"%s\") and then the spectator rig was RELEASED before the "
                    "read-back: camera_control::Get() answered null at the ci+0x26 read, so no "
                    "selector word exists to score. The aim may well have landed -- this record "
                    "cannot say, and `%s` is the loss, not the answer",
                    st.asked, st.said, SelectorReadBackName(s.camSelectorReadBack));
        return Ask::CouldNotAsk;
    }
    if (!s.camSelectorObserved || !s.camSelectorCertified) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` was accepted (\"%s\") but the engine's own ci+0x26 selector word %s, so "
                    "the derivation is `%s`: the write landed and nobody can say where it aimed",
                    st.asked, st.said,
                    s.camSelectorObserved ? "did not certify the split" : "could not be read back",
                    s.camSelectorDerivation);
        return Ask::CouldNotAsk;
    }
    if (!s.camFacingReadAfter) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` resolved and aimed (\"%s\") and the ENGINE certifies the selector "
                    "(ci+0x26 read back %d, derivation `%s`) -- but SampleViewPose answered no "
                    "facing after the aim, so the facing the rig LANDED on is not in this record. "
                    "Taking it off the reply is the reading ENG-W2G-LOOKAT S7 forbids",
                    st.asked, st.said, s.camSelectorPacked, s.camSelectorDerivation);
        return Ask::CouldNotAsk;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "`%s` resolved and aimed (\"%s\"), and the ENGINE certifies it: ci+0x26 read back "
                "%d, which is PackedSelectorWord(team, slot) for this selector, derivation `%s`. "
                "The rig LANDED facing yaw %.1f deg, pitch %.1f deg, read back from the view and "
                "not off the reply. The packing half is certified with it -- ten distinct words, "
                "0 and max+1 refused",
                st.asked, st.said, s.camSelectorPacked, s.camSelectorDerivation,
                static_cast<double>(s.camYawAfter), static_cast<double>(s.camPitchAfter));
    return Ask::Pass;
}

// ARCH-20 S5 -- `ui-gui-begin-gate`. Does `NRadGui::Begin` refuse our window at the suppression
// gate (`cmp cs:byte_1420DAF2C,0 / jnz` at 0x140F912F5), and is the ImGui frame live behind it?
//
// The row is a READER. Every number is taken off ONE csregister::Report read at the row-2.5 site;
// nothing here re-derives an address, re-reads engine memory, or recomputes a delta. Three things
// it does do, because a reader that only copies cannot catch a producer that lied:
//
//  * S5.1's term 3 is RE-RUN here from the published pair. The producer publishes both the atlas
//    dwords it OBSERVED behind the base and the two `.rdata` constants it read at run time, so a
//    producer that dropped term 3 -- the exact B1 defect: `0x142029940`, an unrelated live engine
//    global, treated as the context -- publishes `imguiBaseValid = 1` beside two dwords that do
//    not match, and this row censors the record instead of scoring it. A control the reader cannot
//    re-run is a control that certifies its own copy of the answer.
//  * term 3 comparing 0 against 0 is refused as vacuous: `expected == observed == 0` is what an
//    unread `.rdata` constant looks like, and it would pass the comparison without asking anything.
//  * `guiWindowBudget != 0` censors the whole record before any other field is scored (S5.3): the
//    dword has ONE writer in the image (0x140F92BAF, NRadGui init, `= 0`), so a non-zero reading
//    means our address or our read is wrong long before it means the engine changed it.
//
// `renderDrawListsFnPresent` and `renderedTotalVtxCount` are NOT verdict terms. They are S6.3's
// flip inputs and are reported as numbers, so no arm of this row can be read as "pixels appear".
inline Ask AskGuiBeginGate(const StatusFacts& s, char* d, size_t n) {
    typedef unsigned long long ull;
    if (!s.guiGateSampled) {
        _snprintf_s(d, n, _TRUNCATE,
                    "no ui-gui-begin-gate sample is in this record: the read side at the row-2.5 "
                    "site is UNCONDITIONAL -- there is no `ui.gate_probe`, and none was ever added "
                    "(ENG-W2F-C2B s1) -- so a record with no field to read is one written by a "
                    "build from before the read side landed, or by a run that never entered the "
                    "site. Absent is not zero, and "
                    "a zero invented here would be the in-vivo answer to ARCH-20's whole question "
                    "fabricated out of the record's age");
        return Ask::CouldNotAsk;
    }
    if (s.guiGateSamples == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the row-2.5 site was never entered, so the pair of samples around gui::Begin "
                    "was taken 0 times and the gate was never read at the moment this row is "
                    "about (frameSiteEntries == 0)");
        return Ask::CouldNotAsk;
    }
    if (!s.guiGateAddrResolved) {
        _snprintf_s(d, n, _TRUNCATE,
                    "an address this row reads did not resolve through buildguard::MappedAddress: "
                    "%s. Every field below it would be a read of whatever that address happens to "
                    "hold", s.guiGateUnresolved[0] != '\0' ? s.guiGateUnresolved
                                                           : "the producer named none");
        return Ask::CouldNotAsk;
    }
    if (s.guiWindowBudget != 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "INSTRUMENT FAULT, loud: GUI.WINDOW_BUDGET read %d over %llu sample(s). The "
                    "dword has exactly ONE writer in the whole image (0x140F92BAF, NRadGui init, "
                    "r12d = 0) against 34 compare-with-zero reads, so a non-zero reading means our "
                    "address or our read is wrong long before it means the engine changed it. This "
                    "is not a finding about the engine and NO other field on this record is scored "
                    "until the address and the read are proven",
                    s.guiWindowBudget, (ull)s.guiGateSamples);
        return Ask::CouldNotAsk;
    }
    // S5.1 term 3, re-run against the producer's own published pair.
    if (s.guiImguiBaseValid && s.guiAtlasExpectA == 0 && s.guiAtlasExpectB == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "INSTRUMENT FAULT: the record claims imguiBaseValid=1 with BOTH .rdata "
                    "constants read as 0 (observed %08X/%08X), so term 3 of the S5.1 control "
                    "compared 0 against 0 and asked nothing. An unread GUI.ATLAS_CONST_A/B is "
                    "exactly what that looks like, and a vacuous comparison is not a control",
                    s.guiAtlasDwordA, s.guiAtlasDwordB);
        return Ask::CouldNotAsk;
    }
    if (s.guiImguiBaseValid &&
        (s.guiAtlasDwordA != s.guiAtlasExpectA || s.guiAtlasDwordB != s.guiAtlasExpectB)) {
        _snprintf_s(d, n, _TRUNCATE,
                    "INSTRUMENT FAULT: the record claims imguiBaseValid=1 but term 3 of the S5.1 "
                    "control does NOT hold on its own published numbers -- atlas observed "
                    "%08X/%08X against .rdata %08X/%08X. Either the producer dropped term 3 or it "
                    "read the constants from somewhere else; either way the base is unproven and "
                    "the four context-relative fields are not scored",
                    s.guiAtlasDwordA, s.guiAtlasDwordB, s.guiAtlasExpectA, s.guiAtlasExpectB);
        return Ask::CouldNotAsk;
    }

    // The published line. Every field of S5.2 appears on it whatever the verdict, and the four
    // context-relative ones say could-not-ask -- never 0 -- when the base did not prove out.
    char ctx[420];
    if (!s.guiImguiBaseValid) {
        _snprintf_s(ctx, sizeof ctx, _TRUNCATE,
                    "imguiBaseValid=0 atlasObserved=%08X/%08X atlasExpected=%08X/%08X "
                    "frameCountDelta=could-not-ask renderDrawListsFnPresent=could-not-ask "
                    "renderedCmdListsCount=could-not-ask renderedTotalVtxCount=could-not-ask (%s)",
                    s.guiAtlasDwordA, s.guiAtlasDwordB, s.guiAtlasExpectA, s.guiAtlasExpectB,
                    s.guiImguiBaseReason[0] != '\0' ? s.guiImguiBaseReason
                                                    : "the producer named no failing term");
    } else if (s.guiGateSamples < 2) {
        _snprintf_s(ctx, sizeof ctx, _TRUNCATE,
                    "imguiBaseValid=1 atlasObserved=%08X/%08X atlasExpected=%08X/%08X "
                    "frameCountDelta=could-not-ask (one entry, so there is no previous to subtract) "
                    "renderDrawListsFnPresent=%u renderedCmdListsCount=%d renderedTotalVtxCount=%d",
                    s.guiAtlasDwordA, s.guiAtlasDwordB, s.guiAtlasExpectA, s.guiAtlasExpectB,
                    s.guiRenderDrawListsFnPresent, s.guiRenderedCmdListsCount,
                    s.guiRenderedTotalVtxCount);
    } else {
        _snprintf_s(ctx, sizeof ctx, _TRUNCATE,
                    "imguiBaseValid=1 atlasObserved=%08X/%08X atlasExpected=%08X/%08X "
                    "frameCountDelta=%d renderDrawListsFnPresent=%u renderedCmdListsCount=%d "
                    "renderedTotalVtxCount=%d",
                    s.guiAtlasDwordA, s.guiAtlasDwordB, s.guiAtlasExpectA, s.guiAtlasExpectB,
                    s.guiImguiFrameCountDelta, s.guiRenderDrawListsFnPresent,
                    s.guiRenderedCmdListsCount, s.guiRenderedTotalVtxCount);
    }
    char line[760];
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "samples=%llu suppressed=%u hotkeyEnable=%u reentryDepth=%d windowBudget=%d "
                "viewportsEnabled=%d beginBalanceDelta=%d %s",
                (ull)s.guiGateSamples, s.guiSuppressed, s.guiHotkeyEnable, s.guiReentryDepth,
                s.guiWindowBudget, s.guiViewportsEnabled, s.guiBeginBalanceDelta, ctx);

    // The loudest scored arm, and it is independent of the gate: a live base whose frame counter
    // does not move means NewFrame does not run in retail, which overturns S0 and reopens the
    // route choice. It needs two entries to have a previous to subtract.
    if (s.guiImguiBaseValid && s.guiGateSamples >= 2 && s.guiImguiFrameCountDelta == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%s -- ASKED-FAIL, LOUD: the ImGui frame counter did not move between two "
                    "entries behind a base that passed all three terms of the S5.1 control. "
                    "ARCH-20 S0 predicted >= 1 and is OVERTURNED in vivo: NewFrame does not run in "
                    "retail, so clearing the suppression byte cannot be sufficient and the route "
                    "choice returns to open", line);
        return Ask::Fail;
    }
    // F6. The engine owns a per-frame toggle of the very byte this row samples (0x140F9293A, gated
    // on GUI.HOTKEY_ENABLE_BYTE), so with the hotkey live `guiSuppressed` is the operator's, not
    // the engine's, and the prediction is not the record's to score.
    if (s.guiHotkeyEnable != 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%s -- NOT SCORED (F6): GUI.HOTKEY_ENABLE_BYTE is %u, so the engine's own "
                    "per-frame toggle at 0x140F9293A can flip GUI.SUPPRESS_BYTE between our two "
                    "samples and `guiSuppressed == 1` is not a prediction about the engine. The "
                    "observation is reported and the arm is deferred",
                    line, s.guiHotkeyEnable);
        return Ask::CouldNotAsk;
    }
    // S0.1's gate is `... && (reentryDepth || (!suppressed && budget <= 0))`. With the FIRST
    // disjunct open, Begin can pass with the byte set and that is the engine working as written,
    // not the instrument fault F2's arm describes. Scoring it as one would file a defect against
    // our own reading of a documented disjunction.
    if (s.guiReentryDepth != 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%s -- NOT SCORED: GUI.REENTRY_DEPTH is %d, so the FIRST disjunct of S0.1's "
                    "gate is open and `Begin` can pass with the suppression byte set. The four "
                    "suppressed/delta arms all assume the reentry leg is closed; with it open "
                    "neither a +1 nor a 0 discriminates the suppression byte",
                    line, s.guiReentryDepth);
        return Ask::CouldNotAsk;
    }
    if (s.guiBeginBalanceDelta != 0 && s.guiBeginBalanceDelta != 1) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%s -- ASKED-FAIL: GUI.BEGIN_BALANCE moved by %d across ONE gui::Begin call. "
                    "The discriminator assumes the `inc cs:dword_1420DB9B4` at 0x140F91307 is the "
                    "only writer on this path; a step that is neither 0 nor +1 unseats that "
                    "assumption and no gate verdict is read off it",
                    line, s.guiBeginBalanceDelta);
        return Ask::Fail;
    }
    const bool set = s.guiSuppressed != 0;
    const bool passed = s.guiBeginBalanceDelta == 1;
    if (set && !passed) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%s -- ASKED-PASS: the suppression byte was set and GUI.BEGIN_BALANCE did not "
                    "move, so NRadGui::Begin refused at 0x140F912F5 with the reentry leg closed "
                    "and the budget leg a constant pass. DIAG-G2 H1 is confirmed in vivo and R1 -- "
                    "the scoped re-enable through the engine's own two setters -- is the fix", line);
        return Ask::Pass;
    }
    if (!set && passed) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%s -- ASKED-FAIL, LOUD: the suppression byte was CLEAR and Begin passed the "
                    "gate, so the refusal our panel meets is not the suppression gate at all. The "
                    "failure is ImGui-side (DIAG-G2 H4) and ARCH-20's route choice must be re-ruled "
                    "before W2-F-C2 is built", line);
        return Ask::Fail;
    }
    if (!set && !passed) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%s -- ASKED-FAIL, CONTRADICTION: the byte was clear, the reentry leg was "
                    "closed and the budget leg is a constant pass, so every conjunct of S0.1's "
                    "gate was satisfied and GUI.BEGIN_BALANCE still did not move. That is an "
                    "instrument fault: prove the instrument before any other field on this record "
                    "is read as a finding", line);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "%s -- ASKED-FAIL, INSTRUMENT FAULT, LOUD (F2): the suppression byte was SET and "
                "Begin still passed 0x140F912F5 with the reentry leg closed and the hotkey toggle "
                "disabled. Either the byte changed between SampleGate() and Begin by some path "
                "this row cannot see, or GUI.BEGIN_BALANCE is incremented somewhere other than the "
                "assumed site -- which unseats the discriminator itself, so neither this record "
                "nor S5's prediction is scored off it", line);
    return Ask::Fail;
}


// ---------------------------------------------------------------------------------------------
// `w2f-facade-refusal-names-its-cause`. Two questions, in
// this order:
//
//   1. Do the halves account for the aggregate? `refusedByFacade` has ONE writer -- it is
//      nodraw+beginfalse, computed, not stored -- so a reading on which they do not sum to it is
//      a reading of something else. That is an INSTRUMENT FAULT: it censors the row, it is never
//      resolved here in favour of either publication, and it is never a verdict about the panel.
//   2. Does every recorded facade refusal NAME its cause? A counter that moved with `reason=none`
//      is a refusal nobody attributed, which is the exact gap DIAG-G2 exists to close. `unnamed`
//      is BeginRefusalName's own word for an enumerator nobody named -- the module admitting the
//      gap -- and it fails for the same reason `none` does.
//
// With `facade == 0` nothing was refused, so there is no cause to name: could-not-ask, never a
// pass that a build which refuses nothing would earn by refusing nothing.
// ---------------------------------------------------------------------------------------------
inline Ask AskFacadeRefusalNamed(const StatusFacts& s, char* d, size_t n) {
    typedef unsigned long long ull;
    if (!s.csAsked) {
        _snprintf_s(d, n, _TRUNCATE, "csregister::Status_() was never read");
        return Ask::CouldNotAsk;
    }
    // Sized from what it RENDERS -- three %llu at UINT64_MAX, two ten-digit fields, the longest
    // reason word and the fixed text -- not from a line anyone measured.
    constexpr size_t kFacadeReportMax = 3 * 20 + 2 * 10 + 16 + 128;
    char report[kFacadeReportMax];
    _snprintf_s(report, sizeof report, _TRUNCATE,
                "csregister::Report says nodraw=%llu beginfalse=%llu facade=%llu reason=%s "
                "bound=%u/%u",
                (ull)s.csRefusedNoDraw, (ull)s.csRefusedBeginFalse, (ull)s.csRefusedByFacade,
                s.csBeginRefusal[0] != '\0' ? s.csBeginRefusal : "(unread)", s.csFacadeBoundEntries,
                s.csFacadeEntryCount);
    // Refused BY NAME, and first: a truncated line whose groups parsed as nothing would otherwise
    // reach the "publisher is behind this reader" branch below, which is a different -- and wrong --
    // diagnosis of the same bytes.
    if (s.csDiagRefusal[0] != '\0') {
        _snprintf_s(d, n, _TRUNCATE,
                    "csregister::Diagnostic() answered with a line that names itself a REFUSAL: "
                    "'%s'. Nothing on that line is parsed as a value. %s -- REPORTED, scored by "
                    "nothing",
                    s.csDiagRefusal, report);
        return Ask::CouldNotAsk;
    }
    if (!s.csFacadePublished) {
        _snprintf_s(d, n, _TRUNCATE,
                    "csregister::Diagnostic() carried no ' [facade nodraw=...]' group, so this "
                    "reader has nothing to cross-check the halves against. %s -- REPORTED, scored "
                    "by nothing",
                    report);
        return Ask::CouldNotAsk;
    }
    if (!s.csInFramePublished || !s.csInFrameLineAgrees) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the `[inframe ...]` aggregate this row sums against is %s, so `facade=` has no "
                    "settled value on this line. %s -- REPORTED, scored by nothing",
                    s.csInFramePublished ? "published but DISAGREES with the struct read"
                                         : "not on this line",
                    report);
        return Ask::CouldNotAsk;
    }
    // csInFrameLineAgrees has already established that the published `facade=` equals the struct
    // read, so it is the one aggregate both sums are checked against.
    const ull lineSum = (ull)s.csFacadeNoDrawPublished + (ull)s.csFacadeBeginFalsePublished;
    const ull structSum = (ull)s.csRefusedNoDraw + (ull)s.csRefusedBeginFalse;
    if (lineSum != (ull)s.csRefusedByFacade || structSum != (ull)s.csRefusedByFacade ||
        s.csFacadeNoDrawPublished != s.csRefusedNoDraw ||
        s.csFacadeBeginFalsePublished != s.csRefusedBeginFalse) {
        _snprintf_s(d, n, _TRUNCATE,
                    "INSTRUMENT FAULT: `nodraw + beginfalse == facade` does NOT hold, so the line "
                    "was assembled wrong and no half of it is scored. The published group says "
                    "nodraw=%llu beginfalse=%llu (sum %llu); %s (sum %llu); the aggregate is %llu",
                    (ull)s.csFacadeNoDrawPublished, (ull)s.csFacadeBeginFalsePublished, lineSum,
                    report, structSum, (ull)s.csRefusedByFacade);
        return Ask::CouldNotAsk;
    }
    if (!FacadeReasonKnown(s.csFacadeReasonPublished)) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the published reason is `%s`, which is not one of {none, beginunbound, "
                    "noplacement, boundnoframe, unnamed}: a vocabulary this reader is behind on, "
                    "not a value to score. %s",
                    s.csFacadeReasonPublished, report);
        return Ask::CouldNotAsk;
    }
    if (strcmp(s.csFacadeReasonPublished, s.csBeginRefusal) != 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "INSTRUMENT FAULT: one fact, two holders, two answers -- the line says "
                    "reason=%s and csregister::Report says reason=%s. No cause is named off a pair "
                    "that does not agree with itself",
                    s.csFacadeReasonPublished,
                    s.csBeginRefusal[0] != '\0' ? s.csBeginRefusal : "(unread)");
        return Ask::CouldNotAsk;
    }
    if (s.csRefusedByFacade == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the facade refused NO draw on this launch (nodraw=0 beginfalse=0), so there is "
                    "no cause to name and this row asks nothing. bound=%u/%u",
                    s.csFacadeBoundPublished, s.csFacadeEntryCountPublished);
        return Ask::CouldNotAsk;
    }
    if (strcmp(s.csFacadeReasonPublished, "none") == 0 ||
        strcmp(s.csFacadeReasonPublished, "unnamed") == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%llu facade refusal(s) are recorded (nodraw=%llu beginfalse=%llu) and the "
                    "reason published for them is `%s` -- %s. A refusal nobody attributed is the "
                    "gap DIAG-G2 exists to close. bound=%u/%u",
                    (ull)s.csRefusedByFacade, (ull)s.csFacadeNoDrawPublished,
                    (ull)s.csFacadeBeginFalsePublished, s.csFacadeReasonPublished,
                    strcmp(s.csFacadeReasonPublished, "none") == 0
                        ? "the counter moved and nothing named it"
                        : "an enumerator the module itself has not named",
                    s.csFacadeBoundPublished, s.csFacadeEntryCountPublished);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "%llu facade refusal(s) (nodraw=%llu beginfalse=%llu, and the two sum to the "
                "aggregate on the line AND to the struct read), cause named `%s`, facade entries "
                "bound=%u/%u at the last refusal",
                (ull)s.csRefusedByFacade, (ull)s.csFacadeNoDrawPublished,
                (ull)s.csFacadeBeginFalsePublished, s.csFacadeReasonPublished,
                s.csFacadeBoundPublished, s.csFacadeEntryCountPublished);
    return Ask::Pass;
}


// L3-RUN-4 F2. A record written before the launch reached its completion point has a
// HORIZON: everything it says about what has NOT happened is true only up to the heartbeat
// that wrote it. Both records from that run said "no [identity] 'roster:' line" of a session
// whose log went on to carry two, because the sentences were in the indicative and nothing
// in them said when they had last looked. This is applied by the writer to EVERY
// could-not-ask detail of a provisional record, so a row added later cannot forget it.
inline void AppendProvisionalHorizon(char* d, size_t n, double observedMs,
                                     long long playheadUs) {
    if (d == nullptr || n == 0) {
        return;
    }
    char tail[224];
    if (playheadUs < 0) {
        _snprintf_s(tail, sizeof tail, _TRUNCATE,
                    " -- AS OF THE LAST HEARTBEAT AT %.0f ms, in a PROVISIONAL record with no"
                    " transport to give a playhead: this is what had not happened YET, not what"
                    " did not happen", observedMs);
    } else {
        _snprintf_s(tail, sizeof tail, _TRUNCATE,
                    " -- AS OF THE LAST HEARTBEAT AT %.0f ms, playhead %lld us, in a PROVISIONAL"
                    " record: this is what had not happened YET, not what did not happen",
                    observedMs, playheadUs);
    }
    strncat_s(d, n, tail, _TRUNCATE);
}

// ARCH-21 S5, and the unit is the ROUND NUMBER. `w2r-round-lands-on-round` scores the same jump
// against ui::RateCeiling()'s tolerance, which is a property of THIS MACHINE's frame rate; this row
// scores it against the CAPTURE's own tick spacing, which is what "within one tick of the round's
// begin marker" means. The two are independent quantities and a 30 Hz capture on a 90 Hz machine
// makes them disagree.
//
// ***** A FILE WITH NO ROUND TABLE IS THE INTERESTING ARM, not an excused one. SeekRound refuses --
// never clamps, never picks a neighbour -- so on such a file the NAMED REFUSAL IS THE PASS, and a
// `round` that answered ok there is the failure this row exists to catch.
inline Ask AskGotoRoundLands(const StatusFacts& s, char* d, size_t n) {
    if (!s.batteryDispatched) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the playback battery never ran (durationUs %llu), so no round was asked for",
                    static_cast<unsigned long long>(s.durationUs));
        return Ask::CouldNotAsk;
    }
    if (!s.roundsKnown) {
        _snprintf_s(d, n, _TRUNCATE,
                    "ui::RoundsKnown() is false: the manifest's round table could not be read at "
                    "all. \"This file has no rounds\" and \"we could not read whether it has any\" "
                    "are different answers, and only the first one makes a refusal correct");
        return Ask::CouldNotAsk;
    }
    if (!StepScoreable(s, Verb::Round, d, n)) { return Ask::CouldNotAsk; }
    const TransportStep& rd = StepFor(s, Verb::Round);
    if (s.roundCount == 0) {
        if (rd.ok) {
            _snprintf_s(d, n, _TRUNCATE,
                        "`%s` answered ok (\"%s\") and put the playhead at %lld us on a file whose "
                        "round table is EMPTY (ui::RoundCount()==0). SeekRound must refuse a number "
                        "the file does not carry -- a jump that lands plausibly on a file with no "
                        "rounds is the silent clamp the RoundNumber type exists to prevent",
                        rd.asked, rd.said, rd.playheadAfter);
            return Ask::Fail;
        }
        if (rd.playheadAfter != rd.playheadBefore) {
            _snprintf_s(d, n, _TRUNCATE,
                        "`%s` was refused (\"%s\") on a file with no round table, which is correct "
                        "-- but the playhead still moved %lld us -> %lld us. A refused seek must "
                        "not move anything", rd.asked, rd.said, rd.playheadBefore,
                        rd.playheadAfter);
            return Ask::Fail;
        }
        _snprintf_s(d, n, _TRUNCATE,
                    "this capture carries no round table (ui::RoundCount()==0) and `%s` was "
                    "REFUSED with the playhead held at %lld us: \"%s\". The named refusal is the "
                    "correct answer here, not a failed seek", rd.asked, rd.playheadAfter, rd.said);
        return Ask::Pass;
    }
    if (!rd.ok) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` was REFUSED although the capture's own round table lists %zu round(s): "
                    "\"%s\"", rd.asked, s.roundCount, rd.said);
        return Ask::Fail;
    }
    if (RoundTimeOffFileBase(s, rd.target, d, n, rd.asked, rd.said)) { return Ask::Fail; }
    if (s.tickSpacingUs <= 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` answered ok (\"%s\") and landed at %lld us against the round's own begin "
                    "marker at %lld us, but IReplayTransport::TickSpacingUs published no cadence "
                    "for this capture, so there is no tick to score the landing within",
                    rd.asked, rd.said, rd.playheadAfter, rd.target);
        return Ask::CouldNotAsk;
    }
    const long long off = rd.playheadAfter - rd.target;
    const long long absOff = off < 0 ? -off : off;
    if (absOff > s.tickSpacingUs) {
        _snprintf_s(d, n, _TRUNCATE,
                    "`%s` answered ok (\"%s\") and the playhead is at %lld us, %lld us from the "
                    "begin marker %lld us that round's own row records -- past this capture's "
                    "%lld us tick spacing. Landing on the wrong round does not fault: it looks "
                    "plausible and gets blamed on seek",
                    rd.asked, rd.said, rd.playheadAfter, off, rd.target, s.tickSpacingUs);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "`%s` moved the playhead %lld us -> %lld us through dispatch::Submit, %lld us from "
                "that round's own begin marker %lld us and inside this capture's %lld us tick "
                "spacing, out of %zu labelled round(s)",
                rd.asked, rd.playheadBefore, rd.playheadAfter, off, rd.target, s.tickSpacingUs,
                s.roundCount);
    return Ask::Pass;
}

// ARCH-21 S4.4. WHICH source emitted this file's markers, read back out of the capture-stop
// sentinel the launch wrote -- and it is a TRIPLE because a source word alone cannot separate the
// two states this row exists to keep apart:
//
//   local + 0 boundaries          -- the sampler works and NO ROUND STARTED (a solo host in
//                                    pre_match). Could-not-ask, and it is the honest answer here.
//   none                          -- there is NO MARKER SOURCE AT ALL. Asked-fail, loudly: the
//                                    file has no round markers and nothing will ever add them.
//
// And `corroboratorAsked` is the third state again one level down: a zero `wireRowsSeen` with the
// corroborator never FED is could-not-ask, not agreement between two sources.
inline Ask AskMarkerSourceNamed(const StatusFacts& s, char* d, size_t n) {
    if (!s.sentinelRead) {
        _snprintf_s(d, n, _TRUNCATE, "no-sentinel: %s",
                    s.sentinelReadReason[0] != '\0'
                        ? s.sentinelReadReason
                        : "the capture-stop sentinel has not been read yet");
        return Ask::CouldNotAsk;
    }
    if (!s.sentinelMarkerSourceKnown) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the capture-stop sentinel was read but carries no `markerSource` at all, so "
                    "this build of the capture module never wrote the choice down. An absent field "
                    "is not the word `none`");
        return Ask::CouldNotAsk;
    }
    // The counts always travel with the word: a source that named itself and emitted nothing is a
    // different finding from one that emitted, and the sentence must not be able to hide either.
    char triple[224];
    _snprintf_s(triple, sizeof triple, _TRUNCATE,
                "markerSource=%s, localBoundaries=%s%lu, corroboratorAsked=%s, wireRowsSeen=%s%lu",
                s.sentinelMarkerSource,
                s.sentinelLocalBoundariesKnown ? "" : "(absent)", s.sentinelLocalBoundaries,
                s.sentinelCorroboratorAskedKnown
                    ? (s.sentinelCorroboratorAsked ? "true" : "false")
                    : "(absent)",
                s.sentinelWireRowsSeenKnown ? "" : "(absent)", s.sentinelWireRowsSeen);
    if (strcmp(s.sentinelMarkerSource, "none") == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "NO MARKER SOURCE: the capture chose `none` for this file -- %s. The arm-up "
                    "window ended with the game state undecidable AND no netvar row seen, so this "
                    "capture carries MATCH_BEGIN/MATCH_END and no round marker at all, and nothing "
                    "downstream can add them later", triple);
        return Ask::Fail;
    }
    if (strcmp(s.sentinelMarkerSource, "local") != 0 && strcmp(s.sentinelMarkerSource, "wire") != 0) {
        // `not_recorded` and anything else this observer does not know: refuse rather than sort it
        // into the nearest bucket. A word we cannot classify is not evidence about the capture.
        _snprintf_s(d, n, _TRUNCATE,
                    "the sentinel's marker source is `%s`, which is neither `local`, `wire` nor "
                    "`none` -- so no source was ever chosen for this file (%s). This observer will "
                    "not sort an unrecognised word into one of the three it understands",
                    s.sentinelMarkerSource, triple);
        return Ask::CouldNotAsk;
    }
    if (strcmp(s.sentinelMarkerSource, "wire") == 0) {
        if (!s.sentinelWireRowsSeenKnown || s.sentinelWireRowsSeen == 0) {
            _snprintf_s(d, n, _TRUNCATE,
                        "the capture chose `wire` but the sentinel reports no netvar row behind it "
                        "-- %s. A wire source with nothing on the wire is the choice contradicting "
                        "its own evidence", triple);
            return Ask::Fail;
        }
        _snprintf_s(d, n, _TRUNCATE,
                    "the capture chose the WIRE source for this file and %s. The local game state "
                    "was not decidable inside the arm-up window and the netvar stream was, which "
                    "is the documented precedence", triple);
        return Ask::Pass;
    }
    if (!s.sentinelLocalBoundariesKnown) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the capture chose `local` but the sentinel carries no `localBoundaries` "
                    "count, so whether the sampler ever saw a round boundary cannot be read -- %s. "
                    "Absent is not zero", triple);
        return Ask::CouldNotAsk;
    }
    if (s.sentinelLocalBoundaries == 0) {
        // ***** THE STATE THE TRIPLE EXISTS FOR. The sampler answered, the source was chosen, and
        // no round ever began -- which is exactly what a solo -offline host in pre_match produces.
        // Reporting it as a failure would blame the capture for the session, and reporting it as a
        // pass would certify a round table nobody wrote.
        _snprintf_s(d, n, _TRUNCATE,
                    "no-round-started: the capture chose the LOCAL source -- the sampler answered "
                    "and the arm-up window decided -- and then saw ZERO round boundaries (%s%s). "
                    "That is what a solo host sitting in pre_match produces, and it is a different "
                    "fact from having no marker source at all",
                    triple,
                    s.sentinelPhaseWord[0] != '\0' ? "" :
                        "; the phase at those samples was not published to this observer");
        return Ask::CouldNotAsk;
    }
    if (!s.sentinelCorroboratorAskedKnown) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the capture chose the LOCAL source and emitted %lu round boundary(ies), but "
                    "the sentinel carries no `corroboratorAsked` flag -- %s -- so whether the wire "
                    "path was ever fed cannot be read, and a zero row count could mean either",
                    s.sentinelLocalBoundaries, triple);
        return Ask::CouldNotAsk;
    }
    if (!s.sentinelCorroboratorAsked) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the capture chose the LOCAL source and emitted %lu round boundary(ies) -- %s. "
                    "The wire corroborator was NEVER FED a row, so this file has no second opinion "
                    "at all: that is COULD NOT ASK, and it must never be read as the two sources "
                    "agreeing", s.sentinelLocalBoundaries, triple);
        return Ask::Pass;
    }
    if (s.sentinelWireRowsSeen == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the capture chose the LOCAL source and emitted %lu round boundary(ies) -- %s. "
                    "The corroborator WAS fed and saw zero netvar rows, which is a different "
                    "finding from never having been asked: the wire stream was live and empty",
                    s.sentinelLocalBoundaries, triple);
        return Ask::Pass;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "the capture chose the LOCAL source and emitted %lu round boundary(ies) -- %s. The "
                "corroborator was fed and saw %lu netvar row(s), so this file's markers have a "
                "second opinion behind them", s.sentinelLocalBoundaries, triple,
                s.sentinelWireRowsSeen);
    return Ask::Pass;
}

// The ONE place a published close becomes this observer's status facts. It is a
// pure copy of a snapshot the caller already holds, so the suite can drive it with a hand-built
// StopFactsSnapshot: every count these rows score has to come OUT of the close's own record, and a
// build that quietly scored the zeros instead would otherwise be indistinguishable from this one.
// `markerSourceWord` is the product's own MarkerSourceName over `f.markerSource`, resolved by the
// caller because this header links no product code -- so no word here is a second spelling of one.
inline void FillFromStopFacts(StatusFacts& s, const replay::capture::StopFactsSnapshot& f,
                              const char* markerSourceWord) {
    s.sentinelRead = true;
    if (markerSourceWord != nullptr && markerSourceWord[0] != '\0') {
        _snprintf_s(s.sentinelMarkerSource, sizeof s.sentinelMarkerSource, _TRUNCATE, "%s",
                    markerSourceWord);
        s.sentinelMarkerSourceKnown = true;
    }
    s.sentinelLocalBoundaries = f.localBoundaries;
    s.sentinelLocalBoundariesKnown = true;
    s.sentinelCorroboratorAsked = f.corroboratorAsked;
    s.sentinelCorroboratorAskedKnown = true;
    s.sentinelWireRowsSeen = f.wireRowsSeen;
    s.sentinelWireRowsSeenKnown = true;
    s.sentinelRosterRows = f.rosterRows;
    s.sentinelRosterRowsKnown = true;
    s.sentinelRoundRowCount = f.roundRowCount;
    s.sentinelRoundRowCountKnown = f.roundRowsRecorded;
    s.sentinelRoundCounter = f.roundCounter;
    s.sentinelRoundCounterKnown = f.roundCounterRead;
    // An unsampled phase leaves the word EMPTY. AskMarkerSourceNamed reads empty as `not published`
    // and never as pre_match, so a close that took no game-state sample cannot be read as one that
    // took one and saw nothing.
    if (f.phaseSampled) {
        _snprintf_s(s.sentinelPhaseWord, sizeof s.sentinelPhaseWord, _TRUNCATE, "%s", f.phase);
    }
}

// W2-A's round table, scored off the close that wrote it. Three states, and the third is the point:
//
//   roundRowsRecorded false     -- the close never reached the table. ABSENT. Could-not-ask.
//   0 rows, 0 local boundaries  -- it reached the table and no round ever began (a solo host sitting
//                                  in pre_match). Could-not-ask, and it is the honest answer.
//   0 rows, N local boundaries  -- the LOCAL source cut N boundaries and the table holds none of
//                                  them. Asked-fail: those rounds were seen and then lost. The
//                                  wire source is exempt from that arithmetic on purpose -- its
//                                  rounds come off the netvar stream, not off localBoundaries.
inline Ask AskRoundTableAtClose(const StatusFacts& s, char* d, size_t n) {
    if (!s.sentinelRead) {
        _snprintf_s(d, n, _TRUNCATE, "no-stop-facts: %s",
                    s.sentinelReadReason[0] != '\0'
                        ? s.sentinelReadReason
                        : "no close has published its stop facts to this observer yet");
        return Ask::CouldNotAsk;
    }
    char where[192];
    _snprintf_s(where, sizeof where, _TRUNCATE,
                "markerSource=%s, localBoundaries=%s%lu, roundCounter=%s%lu",
                s.sentinelMarkerSourceKnown ? s.sentinelMarkerSource : "(absent)",
                s.sentinelLocalBoundariesKnown ? "" : "(absent)", s.sentinelLocalBoundaries,
                s.sentinelRoundCounterKnown ? "" : "(absent)", s.sentinelRoundCounter);
    if (!s.sentinelRoundRowCountKnown) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the close published its facts but never reached the round table, so it carries "
                    "no `roundRowCount` at all -- %s. An absent count is not a count of zero: a table "
                    "nobody read cannot report that it held nothing", where);
        return Ask::CouldNotAsk;
    }
    const bool localCut = s.sentinelMarkerSourceKnown &&
                          strcmp(s.sentinelMarkerSource, "local") == 0 &&
                          s.sentinelLocalBoundariesKnown && s.sentinelLocalBoundaries > 0;
    if (s.sentinelRoundRowCount == 0) {
        if (localCut) {
            _snprintf_s(d, n, _TRUNCATE,
                        "ROUNDS LOST AT CLOSE: the LOCAL source cut %lu round boundary(ies) and the "
                        "round table this close wrote holds ZERO rows -- %s. The boundaries were seen "
                        "and then went into no table, so this file's timeline has no round to seek to",
                        s.sentinelLocalBoundaries, where);
            return Ask::Fail;
        }
        _snprintf_s(d, n, _TRUNCATE,
                    "no-round-started: the close REACHED the round table and it held zero rows, with "
                    "no local boundary cut behind it -- %s. That is what a solo host sitting in "
                    "pre_match produces, and it is a different fact from a table nobody read", where);
        return Ask::CouldNotAsk;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "the close recorded %lu round row(s) in the table it wrote -- %s. The rounds this "
                "session played are in the file's own table, which is what a seek by round reads",
                s.sentinelRoundRowCount, where);
    return Ask::Pass;
}

// ---- W2-P3 -----------------------------------------------------------------------------------
// One definition of the track count, shared by the probe that fills these fields and the row that
// scores them, so the two can never disagree about what a track is.
inline unsigned BodyTracksWalked(const StatusFacts& s) {
    return s.bodyInstalled + s.bodyAbsent + s.bodyUnhandled + s.bodyLocallyControlled +
           s.bodyInstallFailed;
}

// L3 #11 F3: S_BODY is on disk and nothing said whether a decoded sample ever became a pose the
// engine holds. This row asks the applier's own published verdict on every track of the window it
// last walked.
// ONE rendering of the applier's install counters, shared by every scored arm below so a Fail and
// a Pass can never describe the same session with different numbers. `install_failed` is the
// total; `identity_refused` is the named subset of it (apply.h:59-69).
inline void BodyInstallCounters(const StatusFacts& s, char* d, size_t n) {
    _snprintf_s(d, n, _TRUNCATE,
                "installed_and_read_back %u, installed_but_unread %u, install_failed %u, "
                "identity_refused %u",
                s.bodyReadBack, s.bodyInstalledUnread, s.bodyInstallFailed, s.bodyIdentityRefused);
}

// The row's driven-pose sentence, rendered ONCE and nowhere else -- every scored arm that names a
// pose write calls this, so a Fail and a Pass cannot count the same session differently.
// `bodies_installed` counts a SetHistoryWindow call that returned true; only the read-back says the
// ring HOLDS the pose, so the headline number is `confirmed` alone. The unread installs travel in
// the same phrase rather than in a second string that could disagree with it.
inline void DrivenPoseWrites(unsigned confirmed, unsigned unread, char* d, size_t n) {
    _snprintf_s(d, n, _TRUNCATE,
                "%u driven-pose write(s) confirmed by read-back (%u installed but unread)",
                confirmed, unread);
}

// L3 #11 F3: S_BODY is on disk and nothing said whether a decoded sample ever became a pose the
// engine holds. This row asks the applier's own published verdict on every track of the window it
// last walked.
//
// PASS REQUIRES installed_and_read_back >= 1. `bodies_installed` counts a SetHistoryWindow call
// that returned true -- a claim about the call. Only the read-back says the engine's ring holds
// the pose, and an install the identity gate refused must read as asked-FAIL rather than as a
// driven-pose write (ENG-W2-B-POSEREAD residual 2).
inline Ask AskBodyDecoded(const StatusFacts& s, char* d, size_t n) {
    if (!s.bodyApplySampled) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the probe never read ApplyStatsSnapshot(), so nothing in this session says "
                    "what the applier decided about any body track");
        return Ask::CouldNotAsk;
    }
    const unsigned tracks = BodyTracksWalked(s);
    if (tracks == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%u apply-stat poll(s) and not one of them found a body track in the window: "
                    "the applier walked nothing, which is a statement about the window rather than "
                    "about the decode", s.bodyApplyPolls);
        return Ask::CouldNotAsk;
    }
    char counters[192];
    BodyInstallCounters(s, counters, sizeof counters);
    if (s.bodyInstalled == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%u body track(s) walked and NOT ONE pose was written: %u unhandled (no engine "
                    "handle bound), %u absent at the playhead, %u locally controlled, %u install "
                    "failed. The file's bodies decoded into tracks and none of them drove anything "
                    "-- %s",
                    tracks, s.bodyUnhandled, s.bodyAbsent, s.bodyLocallyControlled,
                    s.bodyInstallFailed, counters);
        return Ask::Fail;
    }
    if (s.bodyLocallyControlled > 0) {
        char localWrites[128];
#ifndef FAIL_ARM_USES_INSTALLED
        DrivenPoseWrites(s.bodyReadBack, s.bodyInstalledUnread, localWrites, sizeof localWrites);
#else
        DrivenPoseWrites(s.bodyInstalled, s.bodyInstalledUnread, localWrites, sizeof localWrites);
#endif
        _snprintf_s(d, n, _TRUNCATE,
                    "%u of %u body track(s) are still LOCALLY CONTROLLED after the synthetic-owner "
                    "claim (%u claim(s) made), so the engine simulates them and would overwrite "
                    "what we install -- INV-OWN1's observable, with %s beside it -- %s",
                    s.bodyLocallyControlled, tracks, s.bodyOwnerClaimed, localWrites, counters);
        return Ask::Fail;
    }
    if (s.bodyInstallFailed > 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%u of %u body track(s) had a decoded sample at the playhead and "
                    "SetHistoryWindow refused it%s, beside %u that took: the decode is not what "
                    "failed and the engine is -- %s",
                    s.bodyInstallFailed, tracks,
                    s.bodyIdentityRefused > 0 ? ", by name CiIdentityMismatch" : "",
                    s.bodyInstalled, counters);
        return Ask::Fail;
    }
#ifndef ROW_IGNORES_READBACK
    if (s.bodyReadBack == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%u of %u body track(s) installed and the engine's ring gave back NOT ONE of "
                    "them: the install returned true and the read-back of the same element "
                    "disagreed or refused, so nothing here says the engine holds a driven pose "
                    "-- %s",
                    s.bodyInstalled, tracks, counters);
        return Ask::Fail;
    }
#endif
    char drivenWrites[128];
#ifndef PASS_ARM_USES_INSTALLED
    DrivenPoseWrites(s.bodyReadBack, s.bodyInstalledUnread, drivenWrites, sizeof drivenWrites);
#else
    DrivenPoseWrites(s.bodyInstalled, s.bodyInstalledUnread, drivenWrites, sizeof drivenWrites);
#endif
    _snprintf_s(d, n, _TRUNCATE,
                "%u body track(s) decoded and walked, %s into the engine's own history ring, "
                "%u owner claim(s), %u absent at that playhead, 0 locally controlled, "
                "0 install failures, over %u poll(s). SAMPLES PER TRACK is NOT PUBLISHED by this "
                "build (BuildStats::body_samples has no accessor on playback.h), so it is not "
                "reported here; a pose write READ BACK from the ring implies a decoded sample at "
                "the playhead, which is the existence question F3 asks -- %s",
                tracks, drivenWrites, s.bodyOwnerClaimed, s.bodyAbsent, s.bodyApplyPolls,
                counters);
    return Ask::Pass;
}

// W2-P1's census, scored. ENG-W2-P1-4 reading rule 4: a FACT-PP1 verdict with no `census slot=`
// row above it is a bug and not a result, and a `census REFUSED` publishes no verdict at all.
inline Ask AskPawnsProvisioned(const LogFacts& f, char* d, size_t n) {
    if (f.pawnRefusedNoEntityService) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the census REFUSED NoEntityService: no pool was read, so this session answers "
                    "nothing about any provisioned pawn -- %s", f.pawnCensusLine);
        return Ask::CouldNotAsk;
    }
    if (!f.pawnCensusSeen) {
        _snprintf_s(d, n, _TRUNCATE,
                    "no `[pawn] census: provisioned ...` line was printed, so the provisioner was "
                    "either not wired into this build's ProvisionEnv or never reached a state-9 "
                    "edge with a pawn to census");
        return Ask::CouldNotAsk;
    }
    if (f.pawnFactVerdictSeen && f.pawnSlotRows == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "a FACT-PP1 verdict was published with NO `census slot=` row above it: the "
                    "arm was named over a pool nothing in this log says was read");
        return Ask::Fail;
    }
    if (f.pawnProvisioned == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the census ran with provisioned 0 (mapped %u, failed %u). Pump censuses only "
                    "once the ledger holds a provisioned pawn, so a census over zero is a call "
                    "from somewhere else, not a session that provisioned nothing",
                    f.pawnMapped, f.pawnFailed);
        return Ask::Fail;
    }
    if (strcmp(f.pawnStateWord, "known") != 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "the census was wired without a state word (state=%s inmatch=%s) over %u "
                    "provisioned pawn(s): ProvisionEnv::stateRaw is null, so nothing says the "
                    "session was in a match when the pawns were made",
                    f.pawnStateWord[0] != '\0' ? f.pawnStateWord : "(absent)",
                    f.pawnInMatch[0] != '\0' ? f.pawnInMatch : "(absent)", f.pawnProvisioned);
        return Ask::Fail;
    }
    if (f.pawnSlotRows == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%u pawn(s) provisioned, %u map entry/entries, and NOT ONE carried a resolved "
                    "slot (%u entry/entries warned as unmapped): there is no live slot to ask the "
                    "pools about", f.pawnProvisioned, f.pawnMapped, f.pawnUnmappedEntries);
        return Ask::Fail;
    }
    if (f.pawnClassUnknown == f.pawnSlotRows) {
        _snprintf_s(d, n, _TRUNCATE,
                    "all %u censused slot(s) came back class=Unknown: neither the remote CI pool "
                    "nor the local entry surface holds a CI for a pawn this session provisioned, "
                    "so nothing can be driven through either -- %s",
                    f.pawnSlotRows, f.pawnSlotLine);
        return Ask::Fail;
    }
    if (f.pawnDriveableRows == 0) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%u censused slot(s) name a class (%u local, %u remote) and NOT ONE reads a "
                    "body back: driveable=no on both surfaces of every row, so the pawn exists and "
                    "there is no physics component to write a pose into",
                    f.pawnSlotRows, f.pawnClassLocal, f.pawnClassRemote);
        return Ask::Fail;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "provisioned %u, mapped %u, failed %u, resolved %u (state=%s inmatch=%s); by class: "
                "%u local, %u remote, %u unknown; %u driveable, %u naming an entity key, %u refusal "
                "row(s)%s%s; first slot_recorded->slot_resolved %s; pools: remote CIs %u, local CIs "
                "%u, in BOTH %u",
                f.pawnProvisioned, f.pawnMapped, f.pawnFailed,
                f.pawnTotalsSeen ? f.pawnResolvedRows : f.pawnSlotRows, f.pawnStateWord,
                f.pawnInMatch, f.pawnClassLocal, f.pawnClassRemote, f.pawnClassUnknown,
                f.pawnDriveableRows, f.pawnEntityNamed, f.pawnRefusalRows,
                f.pawnFirstRefusal[0] != '\0' ? ", first " : "",
                f.pawnFirstRefusal[0] != '\0' ? f.pawnFirstRefusal : "",
                f.pawnFirstSlotMap[0] != '\0' ? f.pawnFirstSlotMap : "(none)", f.pawnRemoteCis,
                f.pawnLocalCis, f.pawnBothSurfaces);
    return Ask::Pass;
}

struct RowSpec {
    const char* id;
    unsigned arms;
    const char* filedBy;      // the packet whose launch request this carries (8.3a)
};

inline const RowSpec* Rows(size_t* count) {
    static const RowSpec kRows[] = {
        {"w1e-entity-available-on-bound-space", kArmCapture | kArmPlayback | kArmIdle, "W1-E"},
        {"cs-space-candidates",
         kArmCapture | kArmPlayback | kArmBrowser | kArmIdle,                          "W1-E"},
        {"w1f-inbound-loopback-only",           kArmCapture,                           "W1-F"},
        {"w2b-services-rebound-netvar-live",    kArmPlayback | kArmIdle,               "W2-B"},
        {"w2a-gamestate-decoded",               kArmPlayback,                          "W2-A"},
        {"w2a-playerstats-decoded",             kArmPlayback,                          "W2-A"},
        {"w2b-netvars-applied",                 kArmPlayback | kArmIdle,               "W2-B"},
        {"w2b-body-handles-rebound",            kArmPlayback | kArmIdle,               "W2-B"},
        {"w2b-body-decoded",                    kArmPlayback | kArmIdle,               "W2-B"},
        {"w2p-pawns-provisioned",               kArmPlayback | kArmIdle,               "W2-P1"},
        {"w2c-roster-players-dressed",          kArmPlayback | kArmIdle,               "W2-C"},
        {"w2c-owners-mapped",                   kArmPlayback | kArmIdle,               "W2-C"},
        {"w2c-invown1-armed-against-local",     kArmPlayback | kArmIdle,               "W2-C"},
        {"w2e-camera-control-bound",            kArmPlayback | kArmIdle,               "W2-E"},
        {"w2e-4b-view-moves-while-paused",      kArmPlayback,                          "W2-E"},
        {"w2e-view-read-refusal-names-its-site", kArmPlayback,                         "W2-E"},
        {"w2f-csregister-epoch",                kArmBrowser | kArmPlayback | kArmIdle, "W2-F"},
        {"w2f-facade-refusal-names-its-cause",  kArmBrowser | kArmPlayback | kArmIdle, "W2-F"},
        {"w2h-audio-slope-at-half-rate",        kArmPlayback,                          "W2-H"},
        {"w1c-sc7-text-diff",  kArmCapture | kArmPlayback | kArmBrowser | kArmIdle,    "W1-C"},
        {"w2a-capture-stop-sentinel",           kArmCapture,                           "W2-A"},
        {"w2a-marker-source-named",             kArmCapture,                           "W2-A"},
        {"w2a-round-table-at-close",            kArmCapture,                           "W2-A"},
        {"w1l-prologue-unreadable-names-its-subject",
         kArmCapture | kArmPlayback | kArmBrowser | kArmIdle,                          "W1-L"},
        {"w2r-browser-opened-over-level",         kArmBrowser,                          "W2-R"},
        {"w2r-panel-drawn-over-level",            kArmBrowser,                          "W2-R"},
        {"w2f-draw-budget",                       kArmBrowser,                          "W2-R"},
        {"ui-gui-frame-open-at-stage0",
         kArmBrowser | kArmPlayback | kArmIdle,                                         "W2-R"},
        {"ui-gui-begin-gate",
         kArmBrowser | kArmPlayback | kArmIdle,                                         "W2-R"},
        {"w2e-camera-rig-probe",   kArmBrowser | kArmPlayback | kArmIdle,               "W2-E"},
        {"w2r-transport-verbs-through-dispatch", kArmPlayback,                 "W2-R"},
        {"w2r-pause-and-play",                  kArmPlayback,                  "W2-R"},
        {"w2r-rate-reads-back",                 kArmPlayback,                  "W2-R"},
        {"w2r-rate-above-ceiling-refused",      kArmPlayback,                  "W2-R"},
        {"w2r-reverse-runs-backward",           kArmPlayback,                  "W2-R"},
        {"w2r-seek-lands-on-target",            kArmPlayback,                  "W2-R"},
        {"w2r-step-moves-by-ticks",             kArmPlayback,                  "W2-R"},
        {"w2r-round-lands-on-round",            kArmPlayback,                  "W2-R"},
        {"w2c-goto-round-lands",                kArmPlayback,                  "W2-R"},
        {"w2r-goto-returns-to-bookmark",        kArmPlayback,                  "W2-R"},
        {"w2r-residue-fires-on-discontinuity",  kArmPlayback,                  "W2-R"},
        {"w2r-residue-fires-exactly-once",      kArmPlayback,                  "W2-R"},
        {"w2r-seek-relative-forward",           kArmPlayback,                  "W2-R"},
        {"w2r-seek-relative-back",              kArmPlayback,                  "W2-R"},
        {"w2r-seek-relative-clamps-at-start",   kArmPlayback,                  "W2-R"},
        {"p10-seek-landing",                    kArmPlayback,                  "W2-R"},
        {"p10-cam-7",                           kArmPlayback,                  "W2-R"},
        {"p10-cam-7b",                          kArmPlayback,                  "W2-R"},
        {"p10-cam-9",                           kArmPlayback,                  "W2-R"},
        {"w2r-seek-relative-percent-refused",   kArmPlayback,                  "W2-R"},
        {"w2r-goto-next-walks-markers",         kArmPlayback,                  "W2-R"},
        {"w2r-goto-prev-walks-markers",         kArmPlayback,                  "W2-R"},
        {"w2r-goto-next-at-end-does-not-wrap",  kArmPlayback,                  "W2-R"},
        {"w2r-goto-next-no-bookmarks-refused",  kArmPlayback,                  "W2-R"},
        {"w2r-playback-completion-needs-level", kArmPlayback,                  "W2-R"},
        {"w1j-launch-window-declared",           kArmBrowser,                          "W1-J"},
        {"w1j-launch-attendance-declared",       kArmIdle,                             "W1-J"},
    };
    *count = sizeof kRows / sizeof kRows[0];
    return kRows;
}

// Evaluates one row by id. Returns false only for an id this table does not define, which is the
// caller's bug rather than an outcome.
inline bool EvaluateRow(const char* id, const LogFacts& f, const StatusFacts& s, Ask* out, char* d,
                        size_t n) {
    if (strcmp(id, "w1e-entity-available-on-bound-space") == 0)      { *out = AskEntityBound(s, f, d, n); }
    else if (strcmp(id, "w1f-inbound-loopback-only") == 0)            { *out = AskInboundLoopbackOnly(s, d, n); }
    else if (strcmp(id, "cs-space-candidates") == 0)                  { *out = AskCsSpaceCandidates(s, f, d, n); }
    else if (strcmp(id, "w2b-services-rebound-netvar-live") == 0)    { *out = AskServicesRebound(f, d, n); }
    else if (strcmp(id, "w2a-gamestate-decoded") == 0)               { *out = AskGameStateDecoded(s, d, n); }
    else if (strcmp(id, "w2a-playerstats-decoded") == 0)             { *out = AskPlayerStatsDecoded(s, d, n); }
    else if (strcmp(id, "w2b-netvars-applied") == 0)                 { *out = AskNetvarsApplied(f, d, n); }
    else if (strcmp(id, "w2b-body-handles-rebound") == 0)            { *out = AskBodyHandles(f, d, n); }
    else if (strcmp(id, "w2b-body-decoded") == 0)                    { *out = AskBodyDecoded(s, d, n); }
    else if (strcmp(id, "w2p-pawns-provisioned") == 0)               { *out = AskPawnsProvisioned(f, d, n); }
    else if (strcmp(id, "w2c-roster-players-dressed") == 0)          { *out = AskRosterDressed(f, d, n); }
    else if (strcmp(id, "w2c-owners-mapped") == 0)                   { *out = AskOwnersMapped(f, d, n); }
    else if (strcmp(id, "w2c-invown1-armed-against-local") == 0)     { *out = AskInvOwn1Armed(f, d, n); }
    else if (strcmp(id, "w2e-camera-control-bound") == 0)            { *out = AskCameraControlBound(f, d, n); }
    else if (strcmp(id, "w2e-4b-view-moves-while-paused") == 0)      { *out = AskViewMovesWhilePaused(s, f, d, n); }
    else if (strcmp(id, "w2e-view-read-refusal-names-its-site") == 0) { *out = AskViewReadRefusalNamesItsSite(f, d, n); }
    else if (strcmp(id, "w2f-csregister-epoch") == 0)                { *out = AskCsRegisterEpoch(s, d, n); }
    else if (strcmp(id, "w2f-facade-refusal-names-its-cause") == 0)   { *out = AskFacadeRefusalNamed(s, d, n); }
    else if (strcmp(id, "w2h-audio-slope-at-half-rate") == 0)        { *out = AskAudioHalfRate(s, f, d, n); }
    else if (strcmp(id, "w1c-sc7-text-diff") == 0)                   { *out = AskSc7TextDiff(s, d, n); }
    else if (strcmp(id, "w2a-capture-stop-sentinel") == 0)           { *out = AskCaptureStopSentinel(s, f, d, n); }
    else if (strcmp(id, "w2a-marker-source-named") == 0)              { *out = AskMarkerSourceNamed(s, d, n); }
    else if (strcmp(id, "w2a-round-table-at-close") == 0)             { *out = AskRoundTableAtClose(s, d, n); }
    else if (strcmp(id, "w1l-prologue-unreadable-names-its-subject") == 0) { *out = AskPrologueDiagnostic(s, d, n); }
    else if (strcmp(id, "w2r-browser-opened-over-level") == 0)        { *out = AskBrowserOverLevel(s, f, d, n); }
    else if (strcmp(id, "w2r-panel-drawn-over-level") == 0)           { *out = AskPanelDrawnOverLevel(s, f, d, n); }
    else if (strcmp(id, "w2f-draw-budget") == 0)                      { *out = AskDrawBudget(s, f, d, n); }
    else if (strcmp(id, "ui-gui-frame-open-at-stage0") == 0)          { *out = AskFrameOpenAtStage0(s, d, n); }
    else if (strcmp(id, "ui-gui-begin-gate") == 0)                    { *out = AskGuiBeginGate(s, d, n); }
    else if (strcmp(id, "w2e-camera-rig-probe") == 0)                 { *out = AskCameraRigProbe(s, f, d, n); }
    else if (strcmp(id, "w1j-launch-window-declared") == 0)          { *out = AskLaunchWindowDeclared(s, d, n); }
    else if (strcmp(id, "w1j-launch-attendance-declared") == 0)      { *out = AskLaunchAttendanceDeclared(s, d, n); }
    else if (strcmp(id, "w2r-transport-verbs-through-dispatch") == 0) { *out = AskVerbsThroughDispatch(s, d, n); }
    else if (strcmp(id, "w2r-pause-and-play") == 0)                  { *out = AskPauseAndPlay(s, d, n); }
    else if (strcmp(id, "w2r-rate-reads-back") == 0)                 { *out = AskRateReadsBack(s, d, n); }
    else if (strcmp(id, "w2r-rate-above-ceiling-refused") == 0)      { *out = AskRateAboveCeilingRefused(s, d, n); }
    else if (strcmp(id, "w2r-reverse-runs-backward") == 0)           { *out = AskReverseRunsBackward(s, d, n); }
    else if (strcmp(id, "w2r-seek-lands-on-target") == 0)            { *out = AskSeekLandsOnTarget(s, d, n); }
    else if (strcmp(id, "w2r-step-moves-by-ticks") == 0)             { *out = AskStepMovesByTicks(s, d, n); }
    else if (strcmp(id, "w2r-round-lands-on-round") == 0)            { *out = AskRoundLandsOnRound(s, d, n); }
    else if (strcmp(id, "w2c-goto-round-lands") == 0)                 { *out = AskGotoRoundLands(s, d, n); }
    else if (strcmp(id, "w2r-goto-returns-to-bookmark") == 0)        { *out = AskGotoReturnsToBookmark(s, d, n); }
    else if (strcmp(id, "w2r-residue-fires-on-discontinuity") == 0)  { *out = AskResidueFiresOnDiscontinuity(s, d, n); }
    else if (strcmp(id, "w2r-residue-fires-exactly-once") == 0)      { *out = AskResidueFiresExactlyOnce(s, d, n); }
    else if (strcmp(id, "w2r-seek-relative-forward") == 0)           { *out = AskSeekRelativeForward(s, d, n); }
    else if (strcmp(id, "w2r-seek-relative-back") == 0)              { *out = AskSeekRelativeBack(s, d, n); }
    else if (strcmp(id, "w2r-seek-relative-clamps-at-start") == 0)   { *out = AskSeekRelativeClampsAtStart(s, d, n); }
    else if (strcmp(id, "p10-seek-landing") == 0)                    { *out = AskSeekReplyNamesItsLanding(s, d, n); }
    else if (strcmp(id, "p10-cam-7") == 0)                           { *out = AskCamLookLandsOnYaw(s, d, n); }
    else if (strcmp(id, "p10-cam-7b") == 0)                          { *out = AskCamLookRefusesDegenerateFacing(s, d, n); }
    else if (strcmp(id, "p10-cam-9") == 0)                           { *out = AskCamLookAtSubjectResolves(s, d, n); }
    else if (strcmp(id, "w2r-seek-relative-percent-refused") == 0)   { *out = AskSeekRelativePercentRefused(s, d, n); }
    else if (strcmp(id, "w2r-goto-next-walks-markers") == 0)         { *out = AskGotoNextWalksMarkers(s, d, n); }
    else if (strcmp(id, "w2r-goto-prev-walks-markers") == 0)         { *out = AskGotoPrevWalksMarkers(s, d, n); }
    else if (strcmp(id, "w2r-goto-next-at-end-does-not-wrap") == 0)  { *out = AskGotoNextAtEndDoesNotWrap(s, d, n); }
    else if (strcmp(id, "w2r-goto-next-no-bookmarks-refused") == 0)  { *out = AskGotoNextNoBookmarksRefused(s, d, n); }
    else if (strcmp(id, "w2r-playback-completion-needs-level") == 0) { *out = AskPlaybackCompletionNeedsLevel(s, f, d, n); }
    else { return false; }
    return true;
}

// ---------------------------------------------------------------------------------------------
// W1-J's egress record. The harness reads ONE thing about the last layer:
// a top-level `egress` object in done.json. L5 filed none, so the test harness fell back to the
// game log and recorded the collection defect -- the launch WAS protected and the probe could not
// say so. This is the reader that ends that.
//
// It stays a READER: every field is copied out of
// replay::egress's own published accessors by probe_replay.cpp. The state WORD is the product's
// -- ArmStateName's string, verbatim -- and is never spelled here, never derived from the counts,
// and never taken from the legacy `armed` boolean, which cannot express PartiallyArmed and would
// read a half-armed blocker as fully armed.
// Every pointer defaults to "the probe did not fill this in". The destination fields below are the
// difference between a falsifiable record and L5's, and a caller that forgets one must land on the
// refusal branch rather than reading whatever was on the stack.
struct EgressFacts {
    const char* state = nullptr;      // ArmStateName(Current().state)
    const char* armedBy = nullptr;    // ArmTriggerName(Current().trigger) -- `sweep` on the override
    const char* hooksJson = nullptr;  // HookNamesJson(): the array BODY, elements already quoted
    const char* uncovered = nullptr;  // CoverageNote(), or nullptr when the probe has none
    // RefusedDestinationsJson() / AllowedDestinationsJson(): the array BODIES, elements already
    // formed and already REDACTED by src/bootstrap/egress.h. nullptr means the redactor refused.
    const char* refusedDestinations = nullptr;
    const char* allowedDestinations = nullptr;
    const char* redactionRefusal = nullptr;  // RedactionRefusalReason(), or nullptr when it did not
    uint64_t armedAtMs = 0;
    uint64_t refused = 0;
};

// Whether the product gave us a state word at all. `?` is ArmStateName's own answer for a value
// outside its enum, and an empty or null string is what a probe that could not reach the module
// has; both mean the same thing -- this probe cannot say. W1-J records an egress object with no
// readable state as `probe-record-carried-no-state`, which sets Reported without attesting
// anything and is worse than filing nothing, so on this answer the object is OMITTED.
inline bool EgressStateReadable(const EgressFacts& f) {
    return f.state != nullptr && f.state[0] != '\0' && strcmp(f.state, "?") != 0;
}

// L5 (2026-09-02) lost all five of its correctly computed records here. Every buffer on this path
// was fixed and every append was `_TRUNCATE`, so when egress::CoverageNote() came back 763 bytes --
// 15 more than the 768-byte `note` could hold once the wrapper was in it -- the string was cut
// mid-word, the closing `"]` found a full buffer and wrote NOTHING, and done.json was invalid JSON
// at offset 1820. The harness discarded the WHOLE file and reported the launch as NOT-RUN.
//
// So this path grows instead of cutting. Nothing here is bounded by a constant chosen against a
// sample of the input, and the one place a fixed bound still exists -- the caller's record buffer --
// REFUSES with a named reason rather than emitting a prefix. A prefix of valid JSON is not a
// partial answer; it costs every other answer in the file.
inline void AppendJsonEscaped(std::string& out, const char* text) {
    if (text == nullptr) {
        return;
    }
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p == '"' || *p == '\\') {
            out += '\\';
            out += *p;
        } else if (*p == '\n' || *p == '\r' || *p == '\t') {
            out += '\\';
            out += (*p == '\n') ? 'n' : ((*p == '\r') ? 'r' : 't');
        } else if (static_cast<unsigned char>(*p) < 0x20) {
            continue;
        } else {
            out += *p;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// AUD-L5 section 12 item 8 -- THE DESTINATION SET. "16/16 hooks live, 60 refused" cannot separate
// 60 refusals of 127.0.0.1 from 60 refusals of a production login host, and those are opposite
// findings; the auditor had to infer the peer set out of the engine's own log. So the record now
// carries the set itself.
//
// Nothing here redacts anything. src/bootstrap/egress.h does that and publishes the two array
// BODIES already formed: loopback, unspecified, RFC1918, CGNAT and IPv6 ULA/link-local literals go
// through verbatim with their port, and every other host becomes `redacted-<8hex>` plus its TLD.
// That header is FAIL-CLOSED -- with no salt both accessors return nullptr and
// RedactionRefusalReason() says why -- so this writer's whole job is to carry what it is handed and
// to REFUSE, by name, when it was handed nothing. A record that files the counts with no set and no
// reason is exactly the L5 record, and is the thing this change exists to end.
//
// The row is the reading a human wants: not "60 refused" but "everything this launch was ALLOWED
// to reach was ON THIS MACHINE" -- and AUDIT_F3J F3J-3 is what it costs to say that of a bucket
// that also held CGNAT and RFC1918. Only LOOPBACK earns it. Every other family egress.h renders
// verbatim -- lan, cgnat, link-local, ula -- is a DIFFERENT MACHINE, and an ALLOWED entry in one
// of them is an asked-FAIL that names the family. A `redacted-` token in ALLOWED is the same
// finding one step further out.

// The four octets of a dotted-quad at the head of `text`, and nothing else there. `1.2.3.456` and
// `10.0.0.1x` are not addresses; a port, a `]` or the end of the string may follow.
inline bool ParseIPv4Prefix(const char* text, unsigned oct[4]) {
    const char* p = text;
    for (int i = 0; i < 4; ++i) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        unsigned v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 3) {
            v = v * 10 + static_cast<unsigned>(*p - '0');
            ++p;
            ++digits;
        }
        if (v > 255) {
            return false;
        }
        oct[i] = v;
        if (i < 3) {
            if (*p != '.') {
                return false;
            }
            ++p;
        }
    }
    return *p == '\0' || *p == ':' || *p == ']' || *p == '/';
}

// WHICH FAMILY is this RENDERED destination in? AUDIT_F3J F3J-3: this was one bucket called
// `local` and the row's sentence over it was "everything this launch was ALLOWED to reach was on
// this machine". That sentence is false of every family here except loopback and the unspecified
// address. `100.64.0.0/10` is CGNAT -- L5 #3 recorded `100.64.100.100:41641`, a Tailscale mesh
// address on its default WireGuard port, which is A DIFFERENT MACHINE reached over the public
// internet or a third-party relay -- and `192.168/16` is a different machine on this LAN. So the
// predicate is a CLASSIFIER: egress.h renders all of these verbatim (that is its rule and this
// reads it off the rule, not off one launch's peers), and the ROW decides separately which of them
// can carry "on this machine". Everything else -- including every `redacted-<8hex>` token, which is
// precisely what the redactor produces for a host outside these ranges -- is `off-machine`.
inline const char* DestinationFamily(const char* dest) {
    if (dest == nullptr || dest[0] == '\0') {
        return "off-machine";
    }
    const char* p = dest;
    if (*p == '[') {
        ++p;  // [::1]:6719
    }
    if (_strnicmp(p, "localhost", 9) == 0 && (p[9] == '\0' || p[9] == ':')) {
        return "loopback";
    }
    if (p[0] == ':' && p[1] == ':') {
        // `::` is the unspecified address and `::1` is loopback; both are this machine, and the
        // distinction is kept because "nothing was addressed" and "this host was addressed" are
        // different sentences even though they earn the same verdict.
        return (p[2] == '1' && (p[3] == '\0' || p[3] == ':' || p[3] == ']')) ? "loopback"
                                                                            : "unspecified";
    }
    if (_strnicmp(p, "fe80:", 5) == 0) {
        return "link-local";  // IPv6 link-local -- another host on this link
    }
    if ((p[0] == 'f' || p[0] == 'F') &&
        (p[1] == 'c' || p[1] == 'C' || p[1] == 'd' || p[1] == 'D')) {
        // ULA fc00::/7, but only as an IPv6 literal: `fd-something.example.com` is a hostname and
        // a name that merely starts with these two letters must not read as unroutable.
        const char* colon = strchr(p, ':');
        const char* dot = strchr(p, '.');
        if (colon != nullptr && (dot == nullptr || colon < dot)) {
            return "ula";  // fc00::/7 -- a private address space, not this host
        }
    }
    unsigned o[4] = {0, 0, 0, 0};
    if (!ParseIPv4Prefix(p, o)) {
        return "off-machine";
    }
    if (o[0] == 127) {
        return "loopback";
    }
    if (o[0] == 0 && o[1] == 0 && o[2] == 0 && o[3] == 0) {
        return "unspecified";
    }
    if (o[0] == 10 || (o[0] == 172 && o[1] >= 16 && o[1] <= 31) || (o[0] == 192 && o[1] == 168)) {
        return "lan";  // RFC1918 -- another host on this LAN
    }
    if (o[0] == 100 && o[1] >= 64 && o[1] <= 127) {
        return "cgnat";  // shared carrier NAT or an overlay mesh -- off this machine entirely
    }
    if (o[0] == 169 && o[1] == 254) {
        return "link-local";
    }
    return "off-machine";
}

// Only loopback and the unspecified address can carry the sentence "on this machine" (F3J-3).
inline bool DestinationOnThisMachine(const char* family) {
    return strcmp(family, "loopback") == 0 || strcmp(family, "unspecified") == 0;
}

// egress.h renders every family but `off-machine` verbatim; that is its rule, and this is the one
// place the two readings of it are allowed to be compared.
inline bool DestinationRenderedVerbatim(const char* family) {
    return strcmp(family, "off-machine") != 0;
}

// The next `"dest": "<value>"` at or after `from`. Returns the position just past the value's
// closing quote, or nullptr when the body holds no further element.
inline const char* NextDestination(const char* from, char* out, size_t outCap) {
    const char* key = strstr(from, "\"dest\": \"");
    if (key == nullptr) {
        out[0] = '\0';
        return nullptr;
    }
    const char* v = key + strlen("\"dest\": \"");
    size_t w = 0;
    while (*v != '\0' && *v != '"') {
        if (*v == '\\' && v[1] != '\0') {
            ++v;  // an escaped quote belongs to the value and does not end it
        }
        if (w + 1 < outCap) {
            out[w++] = *v;
        }
        ++v;
    }
    out[w] = '\0';
    return (*v == '"') ? v + 1 : nullptr;
}

// Whether a body can sit inside `[ ]` at all. This is deliberately NOT a second copy of the element
// format -- it asks only about quoting, nesting and control bytes, because the failure it exists to
// stop is L5's: one malformed field costs every other row in the file.
inline bool JsonArrayBodyWellFormed(const char* body) {
    if (body == nullptr) {
        return false;
    }
    int depth = 0;
    bool inString = false;
    for (const char* p = body; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (inString) {
            if (*p == '\\') {
                if (p[1] == '\0') {
                    return false;
                }
                ++p;
            } else if (*p == '"') {
                inString = false;
            } else if (c < 0x20) {
                return false;
            }
            continue;
        }
        if (*p == '"') {
            inString = true;
        } else if (*p == '{' || *p == '[') {
            ++depth;
        } else if (*p == '}' || *p == ']') {
            if (--depth < 0) {
                return false;
            }
        } else if (c < 0x20) {
            return false;
        }
    }
    return depth == 0 && !inString;
}

// The row. Could-not-ask when the set is not in the record (and it says WHY, by name); asked-fail
// when a destination that is not local was ALLOWED; asked-pass only when every allowed destination
// is one of the families above.
inline Ask AskEgressDestinations(const EgressFacts& f, char* d, size_t n) {
    const char* reason = (f.redactionRefusal != nullptr && f.redactionRefusal[0] != '\0')
                             ? f.redactionRefusal
                             : nullptr;
    if (f.refusedDestinations == nullptr || f.allowedDestinations == nullptr || reason != nullptr) {
        _snprintf_s(d, n, _TRUNCATE,
                    "NOT-PROTECTING-ATTESTED: the destination set is not in this record, because "
                    "the redactor refused -- %s. Counts alone cannot tell refusals of a loopback "
                    "peer from refusals of a production host",
                    reason != nullptr ? reason : "it returned no set and gave no reason");
        return Ask::CouldNotAsk;
    }
    if (!JsonArrayBodyWellFormed(f.refusedDestinations) ||
        !JsonArrayBodyWellFormed(f.allowedDestinations)) {
        _snprintf_s(d, n, _TRUNCATE,
                    "NOT-PROTECTING-ATTESTED: the destination set was withheld from this record -- "
                    "the module handed over an array body that is not well-formed JSON, and "
                    "embedding it would cost every other row in the file");
        return Ask::CouldNotAsk;
    }

    unsigned refusedCount = 0;
    char dest[128];
    for (const char* p = f.refusedDestinations; (p = NextDestination(p, dest, sizeof dest)) != nullptr;) {
        ++refusedCount;
    }

    unsigned allowedCount = 0;
    const char* offFamily = nullptr;
    char offDest[128] = {0};
    std::string list;
    for (const char* p = f.allowedDestinations; (p = NextDestination(p, dest, sizeof dest)) != nullptr;) {
        ++allowedCount;
        const char* family = DestinationFamily(dest);
        if (!list.empty()) {
            list += ", ";
        }
        list += dest;
        list += " (";
        list += family;
        list += ")";
        if (offFamily == nullptr && !DestinationOnThisMachine(family)) {
            offFamily = family;
            strcpy_s(offDest, sizeof offDest, dest);
        }
    }

    if (offFamily != nullptr) {
        _snprintf_s(d, n, _TRUNCATE,
                    "%u refused, %u allowed, and `%s` is a `%s` destination, NOT loopback: that is "
                    "A DIFFERENT MACHINE%s, so 'everything this launch was ALLOWED to reach was on "
                    "this machine' is FALSE of this record (AUDIT_F3J F3J-3). allowed: %s",
                    refusedCount, allowedCount, offDest, offFamily,
                    strcmp(offFamily, "cgnat") == 0
                        ? ", reached over a shared carrier NAT or an overlay mesh"
                        : (strcmp(offFamily, "off-machine") == 0 ? " off this network entirely"
                                                                 : " on this local network"),
                    list.c_str());
        return Ask::Fail;
    }
    if (allowedCount == 0) {
        // A real answer, and said as one: an empty ALLOWED set is "nothing was let through", which
        // is not the same sentence as "every member passed the test".
        _snprintf_s(d, n, _TRUNCATE,
                    "%u refused, and NO destination was allowed at all: nothing left this machine",
                    refusedCount);
        return Ask::Pass;
    }
    _snprintf_s(d, n, _TRUNCATE,
                "%u refused, %u allowed, and every allowed destination is LOOPBACK or the "
                "unspecified address -- on this machine, and nowhere else: %s",
                refusedCount, allowedCount, list.c_str());
    return Ask::Pass;
}

// What the writer appends when the object does not fit. It is a STRING field, not an egress object:
// filing an `egress` with a fabricated state would set Reported without attesting anything, which
// this file already refuses for an unreadable state word.
inline const char* EgressTooLongField() {
    return "  \"egressRefused\": \"the egress object did not fit this record buffer and was NOT "
           "truncated -- a cut object is invalid JSON and costs every other row in the file, so no "
           "egress is filed for this launch\",\n";
}

// ONE format string, still, so tests/replay/check.ps1 can read the shape it judges out of the built
// binary instead of keeping its own copy of it. What changed is where it is written TO: a buffer
// sized from these inputs, so `_TRUNCATE` has nothing left to cut.
inline const char* EgressObjectFormat() {
    return "  \"egress\": {\"state\": \"%s\", \"armedAtMs\": %llu, \"refused\": %llu, "
           "\"hooks\": [%s], \"armedBy\": \"%s\"%s%s},\n";
}

// The destination half of the object: both arrays and the row's verdict, or -- when the redactor
// refused -- the reason and the same verdict field carrying could-not-ask. There is no third shape
// in which the fields are simply absent, because that shape is the L5 record.
inline std::string EgressDestinationsField(const EgressFacts& f) {
    char detail[1024];
    const Ask verdict = AskEgressDestinations(f, detail, sizeof detail);

    std::string out;
    if (verdict == Ask::CouldNotAsk) {
        out = ", \"destinationsRefused\": \"";
        AppendJsonEscaped(out, f.redactionRefusal != nullptr && f.redactionRefusal[0] != '\0'
                                   ? f.redactionRefusal
                                   : "the redactor returned no destination set and gave no reason");
        out += "\"";
    } else {
        out = ", \"refusedDestinations\": [";
        out += f.refusedDestinations;
        out += "], \"allowedDestinations\": [";
        out += f.allowedDestinations;
        out += "]";
    }
    out += ", \"destinationsVerdict\": \"";
    out += AskWord(verdict);
    out += "\", \"destinationsDetail\": \"";
    AppendJsonEscaped(out, detail);
    out += "\"";
    return out;
}

inline std::string EgressObjectText(const EgressFacts& f) {
    std::string state;
    AppendJsonEscaped(state, f.state);
    std::string by;
    AppendJsonEscaped(by, f.armedBy);
    // Optional, and absent means absent: an empty array would tell the harness this blocker covers
    // everything, which is the claim CoverageNote exists to refuse.
    std::string note;
    if (f.uncovered != nullptr && f.uncovered[0] != '\0') {
        note = ", \"uncovered\": [\"";
        AppendJsonEscaped(note, f.uncovered);
        note += "\"]";
    }
    const std::string dests = EgressDestinationsField(f);
    const char* hooks = (f.hooksJson != nullptr ? f.hooksJson : "");
    // Every %s replaced by its own argument, plus 20 digits for each %llu, plus the format itself.
    // The destination arrays are sized from the module's own bodies for the same reason the note is:
    // a bound chosen against a sample would cut a 60-destination set, and a cut object is invalid
    // JSON that costs every other row in the file.
    const size_t need = strlen(EgressObjectFormat()) + state.size() + by.size() + strlen(hooks) +
                        note.size() + dests.size() + 64;
    std::string obj(need, '\0');
    const int wrote = _snprintf_s(&obj[0], need, _TRUNCATE, EgressObjectFormat(), state.c_str(),
                                  static_cast<unsigned long long>(f.armedAtMs),
                                  static_cast<unsigned long long>(f.refused), hooks, by.c_str(),
                                  note.c_str(), dests.c_str());
    obj.resize(wrote > 0 ? static_cast<size_t>(wrote) : 0);
    return obj;
}

// Appends `"egress": { ... },` to `out`, or a refusal, or nothing. Returns whether the object was
// written -- a refusal returns false, because no egress was filed.
inline bool AppendEgressObject(char* out, size_t cap, const EgressFacts& f) {
    if (!EgressStateReadable(f)) {
        return false;
    }
    const std::string obj = EgressObjectText(f);
    const size_t used = strlen(out);
    // An empty object means the formatter itself refused, which the sizing above makes impossible
    // -- but a silent empty write is the failure mode this whole path exists to end, so it takes
    // the refusal branch rather than filing nothing and reporting success.
    if (!obj.empty() && used + obj.size() + 1 <= cap) {
        memcpy(out + used, obj.c_str(), obj.size() + 1);
        return true;
    }
    const char* refusal = EgressTooLongField();
    const size_t refusalLen = strlen(refusal);
    if (used + refusalLen + 1 <= cap) {
        memcpy(out + used, refusal, refusalLen + 1);
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// The design notes -- the observer's csregister record. CLEAN(L5) is graded from the RECORD, and
// until now the numbers behind the panel rows lived only inside row DETAIL prose, so the L5 #6
// runner had to read a sentence to learn what a counter said. This object puts them on the record
// as FIELDS.
//
// ONE RULE throughout: ABSENT MEANS UNREAD, NEVER ZERO. A field the probe could not read is not in
// the object at all. When Diagnostic() answered with one of its own refusal words the object
// carries `diagnosticRefusal` and NO published group -- a `"nodraw": 0` written off a line that
// says in words it published nothing is exactly the answer this object exists to stop. The struct
// read stands on its own: it comes from csregister::Report and never from the line.
// ---------------------------------------------------------------------------------------------
inline std::string CsRegisterObjectText(const StatusFacts& s) {
    typedef unsigned long long ull;
    if (!s.csAsked) {
        return "  \"csregister\": {\"asked\": false},\n";
    }
    std::string o = "  \"csregister\": {\"asked\": true, \"status\": \"";
    AppendJsonEscaped(o, s.csStatus);
    o += "\"";
    char buf[1024];
    // The epoch fields (8.3a row 6 / AUDIT_L5_RUN3 M2): registrationInvocations against
    // staleInvocations, and the registrations/spacesSeen pair the epoch clause is stated over.
    _snprintf_s(buf, sizeof buf, _TRUNCATE,
                ", \"epoch\": {\"invocations\": %llu, \"registrationInvocations\": %llu, "
                "\"staleInvocations\": %llu, \"staleDetected\": %llu, \"spaceChanges\": %llu, "
                "\"spacesSeen\": %llu, \"registrations\": %u, \"deferrals\": %llu, "
                "\"marshalledDraws\": %llu, \"coalesced\": %llu, \"pending\": %llu, "
                "\"ownerThreadId\": %lu, \"callbackThreadId\": %lu}",
                (ull)s.csInvocations, (ull)s.csRegistrationInvocations, (ull)s.csStaleInvocations,
                (ull)s.csStaleDetected, (ull)s.csSpaceChanges, (ull)s.csSpacesSeen,
                s.csRegistrations, (ull)s.csDeferrals, (ull)s.csMarshalledDraws,
                (ull)s.csCoalescedInvocations, (ull)s.csPendingInvocations,
                (unsigned long)s.csOwnerThreadId, (unsigned long)s.csCallbackThreadId);
    o += buf;
    // ARCH-13 S2.3: owner ticks are the draw counter's denominator, and a record that carried the
    // numerator without it would invite a coverage figure computed against nothing.
    if (s.csOwnerTicksKnown) {
        _snprintf_s(buf, sizeof buf, _TRUNCATE,
                    ", \"ownerTicks\": %llu, \"ownerTicksAtLevel\": %llu", (ull)s.csOwnerTicks,
                    (ull)s.csOwnerTicksAtLevel);
        o += buf;
    }
    // The in-frame draw counters, struct read, all ten of them at one instant.
    if (s.csInFrameKnown) {
        _snprintf_s(buf, sizeof buf, _TRUNCATE,
                    ", \"inframe\": {\"installed\": %s, \"entries\": %llu, \"draws\": %llu, "
                    "\"refused\": %llu, \"nobyte\": %llu, \"nolatch\": %llu, \"noctx\": %llu, "
                    "\"facade\": %llu, \"closed\": %llu, \"fallback\": %llu, \"status\": \"",
                    s.csInFrameInstalled ? "true" : "false", (ull)s.csFrameSiteEntries,
                    (ull)s.csInFrameDraws, (ull)s.csRefusedDraws, (ull)s.csRefusedNoFrameByte,
                    (ull)s.csRefusedNoLatch, (ull)s.csRefusedNoContext, (ull)s.csRefusedByFacade,
                    (ull)s.csPanelClosedEntries, (ull)s.csFallbackEntries);
        o += buf;
        AppendJsonEscaped(o, s.csInFrameStatus);
        o += "\"}";
        // The facade halves, struct read. Present whenever the Report was read, because they come
        // off the same struct as the aggregate above and share its instant.
        _snprintf_s(buf, sizeof buf, _TRUNCATE,
                    ", \"facadeReport\": {\"nodraw\": %llu, \"beginfalse\": %llu, \"reason\": \"%s\""
                    ", \"bound\": %u, \"entryCount\": %u}",
                    (ull)s.csRefusedNoDraw, (ull)s.csRefusedBeginFalse,
                    s.csBeginRefusal[0] != '\0' ? s.csBeginRefusal : "unread",
                    s.csFacadeBoundEntries, s.csFacadeEntryCount);
        o += buf;
    }
    // The `[gate ...]` counters as FIELDS. Same struct, same instant as every counter above.
    if (s.csGateKnown) {
        _snprintf_s(buf, sizeof buf, _TRUNCATE,
                    ", \"gate\": {\"scope\": %s, \"unresolved\": %llu, \"alreadyOpen\": %llu, "
                    "\"scopeEntries\": %llu, \"scopeRestores\": %llu, \"changed\": %llu, "
                    "\"unmapped\": %llu, \"hovered\": %llu, \"drawNanosTotal\": %llu, "
                    "\"drawNanosMax\": %llu, \"firstDrawNanos\": %llu, \"drawNanosBudget\": %llu, \"hoverSource\": \"",
                    s.csGateScopeEnabled ? "true" : "false", (ull)s.csGateUnresolved,
                    (ull)s.csGateAlreadyOpen, (ull)s.csGateScopeEntries,
                    (ull)s.csGateScopeRestores, (ull)s.csGateScopeChanged,
                    (ull)s.csGateSetterUnmapped, (ull)s.csPanelHoveredFrames,
                    (ull)s.csDrawNanosTotal, (ull)s.csDrawNanosMax,
                    (ull)s.csFirstDrawNanos, (ull)s.csDrawNanosBudget);
        o += buf;
        AppendJsonEscaped(o, s.csPanelHoverSource[0] != '\0' ? s.csPanelHoverSource : "unread");
        o += "\"";
        // Absent rather than zero when the probe did not read them: every one of these has 0 as a
        // legitimate value, so a reader must be able to tell "0 opens" from "nobody asked".
        if (s.csDrawPhasesKnown) {
            _snprintf_s(buf, sizeof buf, _TRUNCATE,
                        ", \"imguiNanos\": %llu, \"browserScanNanos\": %llu, "
                        "\"metadataReadNanos\": %llu, \"textFormatNanos\": %llu, "
                        "\"browserIoOnDrawThread\": %llu, \"guiItemsPerFrame\": %llu, "
                        "\"guiGlyphsPerFrame\": %llu, \"guiStreamFrames\": %llu, "
                        "\"drawNanosP99\": %llu, \"drawNanosP999\": %llu, "
                        "\"drawNanosSamples\": %llu, \"drawNanosOverHistogram\": %llu",
                        (ull)s.csImguiNanos, (ull)s.csBrowserScanNanos, (ull)s.csMetadataReadNanos,
                        (ull)s.csTextFormatNanos, (ull)s.csBrowserIoOnDrawThread,
                        (ull)s.csGuiItemsPerFrame, (ull)s.csGuiGlyphsPerFrame,
                        (ull)s.csGuiStreamFrames, (ull)s.csDrawNanosP99, (ull)s.csDrawNanosP999,
                        (ull)s.csDrawNanosSamples, (ull)s.csDrawNanosOverHistogram);
            o += buf;
        }
        o += "}";
    }
    // ARCH-20 S5's block. `guiGateSampled` false is a build without the read side: the object then
    // carries NO field of it, and S6.3(a) is not evaluable rather than evaluated against a zero.
    if (s.guiGateSampled) {
        _snprintf_s(buf, sizeof buf, _TRUNCATE,
                    ", \"guiGate\": {\"addrResolved\": %s, \"samples\": %llu, "
                    "\"suppressed\": %u, \"hotkeyEnable\": %u, \"reentryDepth\": %d, "
                    "\"windowBudget\": %d, \"viewportsEnabled\": %d, "
                    "\"beginBalanceDelta\": %d, \"imguiBaseValid\": %s, "
                    "\"atlasObservedA\": %u, \"atlasObservedB\": %u, "
                    "\"atlasExpectedA\": %u, \"atlasExpectedB\": %u",
                    s.guiGateAddrResolved ? "true" : "false", (ull)s.guiGateSamples,
                    s.guiSuppressed, s.guiHotkeyEnable, s.guiReentryDepth, s.guiWindowBudget,
                    s.guiViewportsEnabled, s.guiBeginBalanceDelta,
                    s.guiImguiBaseValid ? "true" : "false", s.guiAtlasDwordA, s.guiAtlasDwordB,
                    s.guiAtlasExpectA, s.guiAtlasExpectB);
        o += buf;
        if (s.guiGateUnresolved[0] != '\0') {
            o += ", \"unresolved\": \"";
            AppendJsonEscaped(o, s.guiGateUnresolved);
            o += "\"";
        }
        if (s.guiImguiBaseReason[0] != '\0') {
            o += ", \"imguiBaseReason\": \"";
            AppendJsonEscaped(o, s.guiImguiBaseReason);
            o += "\"";
        }
        // The four context-relative fields exist only behind S5.1's three-term control. A number
        // read through a pointer that did not prove out is not a number, so it is ABSENT -- which
        // is the `ui.gate_scope` pair refusing to be differenced rather than differencing two
        // readings of an unknown object.
        if (s.guiImguiBaseValid) {
            _snprintf_s(buf, sizeof buf, _TRUNCATE,
                        ", \"renderDrawListsFnPresent\": %u, \"renderedCmdListsCount\": %d, "
                        "\"renderedTotalVtxCount\": %d",
                        s.guiRenderDrawListsFnPresent, s.guiRenderedCmdListsCount,
                        s.guiRenderedTotalVtxCount);
            o += buf;
            // "this entry minus previous" has no previous on a one-entry record (W2-R-C2 s5.2).
            if (s.guiGateSamples >= 2) {
                _snprintf_s(buf, sizeof buf, _TRUNCATE, ", \"frameCountDelta\": %d",
                            s.guiImguiFrameCountDelta);
                o += buf;
            }
        }
        o += "}";
    }
    if (s.csDiagRefusal[0] != '\0') {
        // The refusal REPLACES the published group. Both words mean the line is not a line, and a
        // runner that found `facadePublished` beside them would grade a value nobody published.
        o += ", \"diagnosticRefusal\": \"";
        AppendJsonEscaped(o, s.csDiagRefusal);
        o += "\"";
    } else if (s.csFacadePublished) {
        _snprintf_s(buf, sizeof buf, _TRUNCATE,
                    ", \"facadePublished\": {\"nodraw\": %llu, \"beginfalse\": %llu, "
                    "\"reason\": \"%s\", \"bound\": %u, \"entryCount\": %u, \"sumsToFacade\": %s}",
                    (ull)s.csFacadeNoDrawPublished, (ull)s.csFacadeBeginFalsePublished,
                    s.csFacadeReasonPublished, s.csFacadeBoundPublished,
                    s.csFacadeEntryCountPublished,
                    ((ull)s.csFacadeNoDrawPublished + (ull)s.csFacadeBeginFalsePublished ==
                     (ull)s.csRefusedByFacade)
                        ? "true"
                        : "false");
        o += buf;
    }
    o += "},\n";
    return o;
}

// Appends the object above, or -- if it would not fit -- a refusal that says the fields are MISSING
// rather than zero. A half-written object is invalid JSON and costs every other row in the file.
inline bool AppendCsRegisterObject(char* out, size_t cap, const StatusFacts& s) {
    const std::string obj = CsRegisterObjectText(s);
    const size_t used = strlen(out);
    if (!obj.empty() && used + obj.size() + 1 <= cap) {
        memcpy(out + used, obj.c_str(), obj.size() + 1);
        return true;
    }
    static const char* const kRefusal =
        "  \"csregister\": {\"asked\": true, \"refused\": \"the csregister object did not fit this "
        "record; no field of it is here and none of them is zero\"},\n";
    const size_t len = strlen(kRefusal);
    if (used + len + 1 <= cap) {
        memcpy(out + used, kRefusal, len + 1);
    }
    return false;
}

}  // namespace probe_replay
