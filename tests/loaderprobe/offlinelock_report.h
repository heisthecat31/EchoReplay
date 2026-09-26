// The done.json this packet's offline-local probe files, as a pure function of what it observed.
//
// ★ WHY THIS IS A HEADER AND NOT A LUMP INSIDE THE PROBE. BLOCK-1 shipped a done.json that no parser
// would accept, and it shipped because nothing offline could produce the document the game produces:
// the probe DLL refuses to load outside echovr.exe, so its artifact had never once been parsed before a
// launch depended on it. Serialisation that only runs in the game is serialisation nobody has checked.
//
// Everything here is a pure function of a Reads and a Report. tests/bootstrap drives it with synthetic
// inputs - including ones that overflow the buffer - and asserts the result parses.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdint.h>

#include "bootstrap/offlinelock.h"

namespace w1c {

constexpr size_t kMaxSeries = 64;

// One timescale reading and when it was taken. A single sample of a LIVE value is a sample from a moment
// nobody chose; the trajectory is what can be read honestly afterwards.
struct TimescaleSample {
    uint64_t atMs;
    float    value;
};

// ★ ONE READ ATTEMPT, RECORDED IN FULL.
//
// Three launches reported globalPtrSeen=false and it meant nothing, because one boolean was covering
// two opposite facts: "the address was not mapped, so we refused to touch it" and "the address was
// mapped and the pointer in it was null". The first is our bug; the second is the engine not having
// built the object yet. Collapsing them cost three launches, so nothing here collapses.
enum class ReadOutcome : uint8_t {
    NotAttempted,
    QueryFailed,     // VirtualQuery itself said nothing
    NotCommitted,    // the page is reserved or free: the address is wrong
    NotReadable,     // committed but no read access
    GuardPage,
    ReadNull,        // ★ read fine, and the value was zero
    ReadOk,
};

inline const char* ReadOutcomeName(ReadOutcome o) {
    switch (o) {
        case ReadOutcome::NotAttempted: return "not-attempted";
        case ReadOutcome::QueryFailed:  return "query-failed";
        case ReadOutcome::NotCommitted: return "not-committed";
        case ReadOutcome::NotReadable:  return "not-readable";
        case ReadOutcome::GuardPage:    return "guard-page";
        case ReadOutcome::ReadNull:     return "read-null";
        case ReadOutcome::ReadOk:       return "read-ok";
    }
    return "?";
}

struct ReadSite {
    char        name[28];
    uint64_t    preferredVa;   // what the registry says
    uint64_t    absoluteVa;    // what we actually dereferenced
    uint32_t    state;
    uint32_t    protect;
    uint64_t    regionSize;
    uint64_t    value;
    uint32_t    attempts;
    ReadOutcome outcome;
};

constexpr size_t kMaxReadSites = 12;

struct Reads {
    bool     clockSeen;
    uint64_t clockPtr;
    uint64_t minElapsedUs;      // CLOCK.MINELAPSED_OFF - decides whether pause can ship at all
    bool     minElapsedRead;
    uint64_t maxElapsedUs;      // CLOCK.MAXELAPSED_OFF
    bool     maxElapsedRead;
    uint64_t firstSeenAtMs;

    // The frame delta the global clock hands to Update. maxElapsedUs alone does not give the
    // fast-forward ceiling - that is maxElapsedUs / raw_frame_us - so the read is useless without it.
    bool     deltaSeen;
    uint64_t deltaMinUs;
    uint64_t deltaMaxUs;
    uint64_t deltaLastUs;
    uint64_t deltaSamples;

    bool     gameSeen;
    bool     gameClockSeen;
    TimescaleSample series[kMaxSeries];
    size_t   seriesLen;
    uint32_t seriesDropped;

    // ★ The lobby capacity read, which decides whether playback can CREATE N recorded players or only
    // redress the ones already present. The offsets are facts::candidate:: - proposed, not derived - so
    // this records the trajectory rather than one sample: a candidate offset that reads a plausible
    // constant at one instant and garbage at the next has been tested, and one read at one moment has
    // not. `lobbySeen` says the pointer chain resolved at all, which is a separate fact from the values.
    bool     lobbySeen;
    uint64_t lobbyPtr;
    uint64_t memberCountFirst;
    uint64_t memberCountLast;
    uint64_t memberCountMax;
    uint64_t connCountFirst;
    uint64_t connCountLast;
    uint64_t connCountMax;
    uint64_t lobbySamples;
    uint64_t lobbyFirstSeenAtMs;
    uint32_t memberCountChanges;
    uint32_t connCountChanges;

    // The rebase evidence, so a wrong base is visible rather than inferred.
    uint64_t moduleBase;
    uint64_t imageBase;
    ReadSite sites[kMaxReadSites];
    size_t   siteCount;
};

// ★ One refused destination. The COUNT was never the finding: "refused 47" cannot tell an expected
// lobby endpoint from something surprising, cannot be turned into a layer-1 rule, and cannot say what
// the offline path is reaching for. Host, port and the hooked API travel together or the number is
// undiagnosable.
struct Destination {
    char     text[64];    // host:port, or the WININET host string
    char     via[24];     // the hooked entry point that saw it
    uint64_t count;
    bool     local;
};

constexpr size_t kMaxDestinations = 128;

// What the blocker saw, gathered by the probe so this builder stays pure and testable.
struct EgressView {
    bool        reported;
    char        state[24];
    char        trigger[24];   // WHY it is armed, which is not the same fact as whether
    char        hooks[384];
    uint64_t    armedAtMs;
    uint64_t    armDurationMs;
    uint32_t    hooksLive;
    uint32_t    hooksRequested;
    uint64_t    refused;
    uint64_t    allowedLocal;
    uint32_t    distinct;
    uint32_t    overflow;
    uint64_t    extensionsRefused;
    uint64_t    socketsCreated;
    Destination dest[kMaxDestinations];
    uint32_t    destCount;
};

// Held back from the first byte so the document can always be closed. A size limit costs a FIELD; the
// object always parses, and `truncated` says whether anything was lost.
constexpr size_t kTailReserve = 48;

struct Json {
    char*  out;
    size_t cap;
    size_t len;
    size_t safe;       // end of the last COMPLETE member, always a valid place to close from
    size_t reserve;    // bytes held back; raised while a member needs room to close itself
    bool   truncated;
};

// ★ Marks a point the document can be closed at. Bounding each append is not enough on its own: a
// string that runs out of room stops mid-value, and the closing quote that would repair it is refused by
// the same bound. The first version of this file did exactly that and produced an unterminated string -
// the BLOCK-1 shape again, one level down. Truncation therefore rewinds to the last complete member
// rather than trying to patch up a half-written one.
inline void JsonMark(Json& j) {
    if (!j.truncated) {
        j.safe = j.len;
    }
}

inline void JsonAppendN(Json& j, const char* text, size_t n) {
    if (j.len + n + j.reserve > j.cap) {
        j.truncated = true;
        return;
    }
    std::memcpy(j.out + j.len, text, n);
    j.len += n;
    j.out[j.len] = '\0';
}

inline void JsonAppend(Json& j, const char* text) { JsonAppendN(j, text, std::strlen(text)); }

inline void JsonFmt(Json& j, const char* fmt, ...) {
    char field[1024];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(field, sizeof field, fmt, args);
    va_end(args);
    if (n < 0) {
        j.truncated = true;
        return;
    }
    size_t len = static_cast<size_t>(n);
    if (len >= sizeof field) {
        len = sizeof field - 1;
        j.truncated = true;
    }
    JsonAppendN(j, field, len);
}

// Escapes into the buffer a character at a time. Safe to stop short: the caller has already written the
// opening quote and always writes the closing one, so a clipped string still parses.
inline void JsonEscaped(Json& j, const char* text) {
    for (const char* p = text; *p != '\0'; ++p) {
        char esc[2];
        size_t n = 0;
        switch (*p) {
            case '"':  esc[0] = '\\'; esc[1] = '"';  n = 2; break;
            case '\\': esc[0] = '\\'; esc[1] = '\\'; n = 2; break;
            case '\n': esc[0] = '\\'; esc[1] = 'n';  n = 2; break;
            case '\r': esc[0] = '\\'; esc[1] = 'r';  n = 2; break;
            case '\t': esc[0] = '\\'; esc[1] = 't';  n = 2; break;
            default:
                if (static_cast<unsigned char>(*p) >= 0x20u) {
                    esc[0] = *p;
                    n = 1;
                }
                break;
        }
        if (n == 0) {
            continue;
        }
        if (j.len + n + j.reserve > j.cap) {
            j.truncated = true;
            return;
        }
        std::memcpy(j.out + j.len, esc, n);
        j.len += n;
        j.out[j.len] = '\0';
    }
}

// Returns the document length. `out` must have room for at least kTailReserve bytes.
inline size_t BuildDoneJson(char* out, size_t cap, const char* probeId, const char* verdict,
                            const char* note, unsigned checked, unsigned failed,
                            const replay::offlinelock::Report& lk,
                            replay::offlinelock::Exercise ex, const Reads& r,
                            const EgressView& eg) {
    if (out == nullptr || cap <= kTailReserve) {
        return 0;
    }
    Json j{out, cap, 0, 0, kTailReserve, false};
    out[0] = '\0';

    // Each member is written whole and then marked. The finaliser closes from the last mark, so a
    // document that ran out of room loses trailing FIELDS and still parses.
    JsonAppend(j, "{\n  \"probe\": \"");
    JsonEscaped(j, probeId);
    JsonAppend(j, "\"");
    JsonMark(j);
    JsonAppend(j, ",\n  \"verdict\": \"");
    JsonEscaped(j, verdict);
    JsonAppend(j, "\"");
    JsonMark(j);
    JsonAppend(j, ",\n  \"note\": \"");
    JsonEscaped(j, note);
    JsonAppend(j, "\"");
    JsonMark(j);

    // The substance floor reads these. An artifact with perfect content and no `checked` records
    // NOT-RUN, which is how a good run of this probe nearly went in the bin.
    JsonFmt(j, ",\n  \"checked\": %u,\n  \"failed\": %u", checked, failed);
    JsonMark(j);

    JsonFmt(j, ",\n  \"offlineLock\": { \"state\": \"%s\", \"exercise\": \"%s\", "
               "\"installedHere\": %s, \"instrumentShared\": %s, \"sitesFound\": %u, "
               "\"sitesExpected\": %u, \"functionVa\": %llu, \"scanMs\": %llu, \"scanBytes\": %u, "
               "\"foreignPrologue\": %s, \"selfTestRan\": %s, \"selfTestClearedByOriginal\": %s, "
               "\"selfTestHeldWhenPreserved\": %s, \"calls\": %llu, \"clearsPrevented\": %llu, "
               "\"callsBitAlreadyClear\": %llu, \"callsBitUntouched\": %llu }",
            replay::offlinelock::LockStateName(lk.state), replay::offlinelock::ExerciseName(ex),
            lk.installedHere ? "true" : "false", lk.instrumentShared ? "true" : "false",
            lk.sitesFound, lk.sitesExpected, static_cast<unsigned long long>(lk.functionVa),
            static_cast<unsigned long long>(lk.scanMs), lk.scanBytes,
            lk.foreignPrologue ? "true" : "false", lk.selfTestRan ? "true" : "false",
            lk.selfTestClearedByOriginal ? "true" : "false",
            lk.selfTestHeldWhenPreserved ? "true" : "false",
            static_cast<unsigned long long>(lk.calls),
            static_cast<unsigned long long>(lk.clearsPrevented),
            static_cast<unsigned long long>(lk.callsBitAlreadyClear),
            static_cast<unsigned long long>(lk.callsBitUntouched));
    JsonMark(j);

    // ★ Each read carries whether it was TAKEN, not just its value. An unread field and a field that
    // read zero are different facts, and zero is a meaningful value for both of these.
    JsonFmt(j, ",\n  \"clock\": { \"globalPtrSeen\": %s, \"globalPtr\": %llu, "
               "\"firstSeenAtMs\": %llu, \"minElapsedUsRead\": %s, \"minElapsedUs\": %llu, "
               "\"maxElapsedUsRead\": %s, \"maxElapsedUs\": %llu, \"deltaSeen\": %s, "
               "\"deltaMinUs\": %llu, \"deltaMaxUs\": %llu, \"deltaLastUs\": %llu, "
               "\"deltaSamples\": %llu, \"gameSeen\": %s, \"gameClockSeen\": %s, "
               "\"timescaleSamples\": %llu, \"timescaleDropped\": %u, \"timescale\": [",
            r.clockSeen ? "true" : "false", static_cast<unsigned long long>(r.clockPtr),
            static_cast<unsigned long long>(r.firstSeenAtMs),
            r.minElapsedRead ? "true" : "false",
            static_cast<unsigned long long>(r.minElapsedUs),
            r.maxElapsedRead ? "true" : "false",
            static_cast<unsigned long long>(r.maxElapsedUs),
            r.deltaSeen ? "true" : "false", static_cast<unsigned long long>(r.deltaMinUs),
            static_cast<unsigned long long>(r.deltaMaxUs),
            static_cast<unsigned long long>(r.deltaLastUs),
            static_cast<unsigned long long>(r.deltaSamples),
            r.gameSeen ? "true" : "false", r.gameClockSeen ? "true" : "false",
            static_cast<unsigned long long>(r.seriesLen), r.seriesDropped);

    size_t emitted = 0;
    for (size_t i = 0; i < r.seriesLen && i < kMaxSeries; ++i) {
        const size_t before = j.len;
        JsonFmt(j, "%s{ \"atMs\": %llu, \"value\": %.6f }", emitted != 0 ? ", " : "",
                static_cast<unsigned long long>(r.series[i].atMs),
                static_cast<double>(r.series[i].value));
        // A sample that did not fit must not leave a dangling separator behind it.
        if (j.len != before) {
            ++emitted;
        } else {
            break;
        }
    }
    JsonFmt(j, "], \"timescaleEmitted\": %llu }", static_cast<unsigned long long>(emitted));
    JsonMark(j);

    // ★ Every read attempt, with the base it was rebased against, the address actually dereferenced,
    // and the page state found there. This exists because three launches reported four booleans that
    // could not distinguish "we refused an unmapped address" from "the engine had not built it yet".
    JsonFmt(j, ",\n  \"reads\": { \"moduleBase\": %llu, \"imageBase\": %llu, \"relocated\": %s, "
               "\"sites\": [",
            static_cast<unsigned long long>(r.moduleBase),
            static_cast<unsigned long long>(r.imageBase),
            (r.moduleBase != r.imageBase) ? "true" : "false");
    for (size_t i = 0; i < r.siteCount && i < kMaxReadSites; ++i) {
        const ReadSite& rs = r.sites[i];
        JsonFmt(j, "%s{ \"name\": \"", i != 0 ? ", " : "");
        JsonEscaped(j, rs.name);
        JsonFmt(j, "\", \"outcome\": \"%s\", \"preferredVa\": %llu, \"absoluteVa\": %llu, "
                   "\"state\": %u, \"protect\": %u, \"regionSize\": %llu, \"value\": %llu, "
                   "\"attempts\": %u }",
                ReadOutcomeName(rs.outcome),
                static_cast<unsigned long long>(rs.preferredVa),
                static_cast<unsigned long long>(rs.absoluteVa), rs.state, rs.protect,
                static_cast<unsigned long long>(rs.regionSize),
                static_cast<unsigned long long>(rs.value), rs.attempts);
    }
    JsonAppend(j, "] }");
    JsonMark(j);

    // The offsets here are facts::candidate:: - proposed numbers this launch exists to settle - so the
    // shape of the value over time is the evidence, not the value at one instant.
    JsonFmt(j, ",\n  \"lobby\": { \"seen\": %s, \"ptr\": %llu, \"firstSeenAtMs\": %llu, "
               "\"samples\": %llu, \"memberCountFirst\": %llu, \"memberCountLast\": %llu, "
               "\"memberCountMax\": %llu, \"memberCountChanges\": %u, \"connCountFirst\": %llu, "
               "\"connCountLast\": %llu, \"connCountMax\": %llu, \"connCountChanges\": %u }",
            r.lobbySeen ? "true" : "false", static_cast<unsigned long long>(r.lobbyPtr),
            static_cast<unsigned long long>(r.lobbyFirstSeenAtMs),
            static_cast<unsigned long long>(r.lobbySamples),
            static_cast<unsigned long long>(r.memberCountFirst),
            static_cast<unsigned long long>(r.memberCountLast),
            static_cast<unsigned long long>(r.memberCountMax), r.memberCountChanges,
            static_cast<unsigned long long>(r.connCountFirst),
            static_cast<unsigned long long>(r.connCountLast),
            static_cast<unsigned long long>(r.connCountMax), r.connCountChanges);
    JsonMark(j);

    // ★ The blocker's own report, and the destinations BY NAME. A refused count says the offline path
    // is contained; only the list says what it was reaching for, and that is the difference between a
    // number to worry about and a rule layer 1 can enforce. Emitted last on purpose: it is the largest
    // member, so if anything is going to be lost to a size limit it should be the tail of this list and
    // not another field - and `destinationsEmitted` beside `distinct` makes any such loss visible.
    JsonFmt(j, ",\n  \"egress\": { \"reported\": %s, \"state\": \"%s\", \"trigger\": \"%s\", "
               "\"armedAtMs\": %llu, "
               "\"armDurationMs\": %llu, \"hooks\": [%s], \"hooksLive\": %u, \"hooksRequested\": %u, "
               "\"refused\": %llu, \"allowedLocal\": %llu, \"distinct\": %u, \"overflow\": %u, "
               "\"extensionsRefused\": %llu, \"socketsCreated\": %llu, \"destinations\": [",
            eg.reported ? "true" : "false", eg.state, eg.trigger,
            static_cast<unsigned long long>(eg.armedAtMs),
            static_cast<unsigned long long>(eg.armDurationMs), eg.hooks, eg.hooksLive,
            eg.hooksRequested, static_cast<unsigned long long>(eg.refused),
            static_cast<unsigned long long>(eg.allowedLocal), eg.distinct, eg.overflow,
            static_cast<unsigned long long>(eg.extensionsRefused),
            static_cast<unsigned long long>(eg.socketsCreated));

    // Losing the TAIL of this list must not cost the whole egress member. The document-level flag is
    // therefore held back until the array is closed and marked - otherwise the finaliser, which rewinds
    // to the last mark, would discard every destination that did fit along with the block around them.
    // ★ The array must always be able to close itself. Without this the loop can spend the last bytes
    // on a destination and leave no room for `], "destinationsEmitted": N }` - the close then fails, the
    // member is marked truncated, and the finaliser rewinds past the entire egress block, discarding
    // every destination that did fit. Reserving the close is what keeps a partial list a partial list.
    const size_t closeNeeds = 48;
    j.reserve = kTailReserve + closeNeeds;

    size_t destEmitted = 0;
    bool destCut = false;
    for (uint32_t i = 0; i < eg.destCount && i < kMaxDestinations; ++i) {
        const size_t before = j.len;
        const bool wasTruncated = j.truncated;
        JsonFmt(j, "%s{ \"to\": \"", destEmitted != 0 ? ", " : "");
        JsonEscaped(j, eg.dest[i].text);
        JsonAppend(j, "\", \"via\": \"");
        JsonEscaped(j, eg.dest[i].via);
        JsonFmt(j, "\", \"calls\": %llu, \"local\": %s }",
                static_cast<unsigned long long>(eg.dest[i].count),
                eg.dest[i].local ? "true" : "false");
        if (j.truncated && !wasTruncated) {
            j.len = before;             // a destination is written whole or not at all
            out[j.len] = '\0';
            j.truncated = wasTruncated;
            destCut = true;
            break;
        }
        ++destEmitted;
    }
    j.reserve = kTailReserve;
    JsonFmt(j, "], \"destinationsEmitted\": %llu }",
            static_cast<unsigned long long>(destEmitted));
    JsonMark(j);
    if (destCut) {
        j.truncated = true;
    }

    // Close from the last complete member. The reserve exists for exactly this and nothing else, so the
    // close cannot itself be the thing that does not fit.
    const bool cut = j.truncated;
    j.len = j.safe;
    out[j.len] = '\0';
    const int tail = std::snprintf(out + j.len, cap - j.len, ",\n  \"truncated\": %s\n}\n",
                                   cut ? "true" : "false");
    if (tail > 0) {
        j.len += static_cast<size_t>(tail);
    }
    return j.len;
}

}  // namespace w1c
