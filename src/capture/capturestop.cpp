#include "capture/capturestop.h"

#include <windows.h>

#include <atomic>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bootstrap/paths.h"
#include "foundation/demofile/demofile.h"

namespace replay {
namespace capture {

const wchar_t kStopSentinelPrefix[] = L"capture_stop.";
const wchar_t kStopSentinelSuffix[] = L".json";
const wchar_t kStopSentinelPattern[] = L"capture_stop.*";
const wchar_t kStopArchiveDirName[] = L"capture_stop_archive";
const wchar_t kStopRequestName[] = L"stop-capture";

uint64_t LaunchFileTime(bool* fromProcessCreation) {
    if (fromProcessCreation) {
        *fromProcessCreation = false;
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    if (::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const uint64_t t =
            (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        if (t) {
            if (fromProcessCreation) {
                *fromProcessCreation = true;
            }
            return t;
        }
    }
    FILETIME now{};
    ::GetSystemTimeAsFileTime(&now);
    const uint64_t t = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    // 0 is the one value that would make two launches share an id, and it is also what a caller gets
    // from a zeroed struct, so it is never returned.
    return t ? t : 1;
}

bool StopSentinelNameFor(uint64_t launchFileTime, uint32_t pid, wchar_t* out, size_t cap) {
    if (!out || cap == 0) {
        return false;
    }
    out[0] = 0;
    return _snwprintf_s(out, cap, _TRUNCATE, L"%s%016llX.%lu%s", kStopSentinelPrefix,
                        static_cast<unsigned long long>(launchFileTime), static_cast<unsigned long>(pid),
                        kStopSentinelSuffix) >= 0;
}

bool StopSentinelNameForClose(uint64_t launchFileTime, uint32_t pid, uint32_t closeSeq, wchar_t* out,
                              size_t cap) {
    if (!out || cap == 0) {
        return false;
    }
    out[0] = 0;
    // %03lu, not %lu: the names sort in close order in a directory listing up to 999 closes, and a
    // launch that closes a thousand times has a bigger problem than a listing that stops sorting.
    return _snwprintf_s(out, cap, _TRUNCATE, L"%s%016llX.%lu.%03lu%s", kStopSentinelPrefix,
                        static_cast<unsigned long long>(launchFileTime),
                        static_cast<unsigned long>(pid), static_cast<unsigned long>(closeSeq),
                        kStopSentinelSuffix) >= 0;
}

uint32_t DeleteStaleStopSentinels(const wchar_t* dir, uint32_t* failures, StopSweepReport report,
                                  void* ctx) {
    if (failures) {
        *failures = 0;
    }
    if (!dir || !dir[0]) {
        return 0;
    }
    wchar_t pattern[kPathMax];
    if (_snwprintf_s(pattern, kPathMax, _TRUNCATE, L"%s\\%s", dir, kStopSentinelPattern) < 0) {
        return 0;
    }
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    uint32_t deleted = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        wchar_t victim[kPathMax];
        if (_snwprintf_s(victim, kPathMax, _TRUNCATE, L"%s\\%s", dir, fd.cFileName) < 0) {
            continue;
        }
        const bool gone = ::DeleteFileW(victim) != 0;
        if (gone) {
            ++deleted;
        } else if (failures) {
            ++*failures;
        }
        if (report) {
            report(fd.cFileName, gone, ctx);
        }
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return deleted;
}

const char* StopReasonName(StopReason r) {
    switch (r) {
        case StopReason::Deadline: return "deadline";
        case StopReason::Request:  return "request";
        case StopReason::Shutdown: return "shutdown";
        case StopReason::MatchEnd: return "match-end";
#if defined(W2A_MUTATE_COSTKILL_BORROWS_MATCHEND)
        // Mutation control: the pre-fix shape -- a cost kill reported as the match
        // ending, a cause that did not happen.
        case StopReason::CostKill: return "match-end";
#else
        case StopReason::CostKill: return "cost-kill";
#endif
        case StopReason::Disabled: return "disabled";
        case StopReason::NoOp: return "no-op";
    }
    // Not reachable through the enum, but a stop that cannot name its reason must not be reported as
    // one of the three that can.
    return "unknown";
}

namespace {

// Sized from the input, not from a guess. The longest refusal sentence identity_access can hand
// back is under kRefusalWhyMaxChars characters, and JSON's worst case escapes every one of them to
// a six-byte \uXXXX -- so the cap is that product plus the terminator, written AS the product, so a
// longer sentence is answered by raising the length it is derived from rather than by re-guessing a
// round number. The entry adds the reason's name, a count and the flag. A value that outgrows even
// this is flagged, never cut in a token -- see the refusal list below.
constexpr size_t kRefusalWhyMaxChars = 128;
constexpr size_t kJsonEscapeWorstCaseBytes = 6;   // \uXXXX, the longest form JsonEscapeOne emits
constexpr size_t kRefusalWhyCap = kRefusalWhyMaxChars * kJsonEscapeWorstCaseBytes + 1;
constexpr size_t kRefusalEntryCap = kRefusalWhyCap + 128;
constexpr size_t kRefusalListCap = kRosterRefusalCount * kRefusalEntryCap;

void SetDetail(ReopenVerdict* out, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(out->detail, sizeof(out->detail), _TRUNCATE, fmt, ap);
    va_end(ap);
}

// THE ESCAPE TABLE, in one place. Both writers below go through it, so a mutation that drops an
// escape cannot be true of one string in this document and false of another.
size_t JsonEscapeOne(unsigned char c, char* esc) {
    switch (c) {
        case '"':  memcpy(esc, "\\\"", 2); return 2;
        case '\\': memcpy(esc, "\\\\", 2); return 2;
        case '\b': memcpy(esc, "\\b", 2);  return 2;
        case '\f': memcpy(esc, "\\f", 2);  return 2;
        case '\n': memcpy(esc, "\\n", 2);  return 2;
        case '\r': memcpy(esc, "\\r", 2);  return 2;
        case '\t': memcpy(esc, "\\t", 2);  return 2;
        default: break;
    }
    if (c < 0x20) {
        return static_cast<size_t>(sprintf_s(esc, 8, "\\u%04X", c));
    }
    esc[0] = static_cast<char>(c);
    return 1;
}

// A NARROW string into a JSON string body, length-checked (L2-RUN-5 F2). Returns false when the
// whole value did not fit -- and `out` is still terminated, still whole-token, still a legal string
// body, because the ONE thing this must never do is what it used to do: hand back a value cut in the
// middle of a token so the document ends mid-string. A caller that gets false says so in the
// document; it does not pretend the value is complete.
bool JsonEscapeNarrow(const char* in, char* out, size_t cap) {
    if (cap == 0) {
        return false;
    }
    out[0] = 0;
    if (!in) {
        return true;
    }
    size_t o = 0;
    for (const char* p = in; *p; ++p) {
        char esc[8];
        const size_t len = JsonEscapeOne(static_cast<unsigned char>(*p), esc);
        if (o + len + 1 > cap) {
            out[o] = 0;
            return false;
        }
        memcpy(out + o, esc, len);
        o += len;
    }
    out[o] = 0;
    return true;
}

// UTF-8 with the six escapes JSON requires. A Windows path is mostly backslashes, which is the one
// that matters: an unescaped `\U` in `C:\Users\...` makes the document unparseable.
bool JsonEscapeUtf8(const wchar_t* w, char* out, size_t cap) {
    if (cap == 0) {
        return false;
    }
    out[0] = 0;
    if (!w || !w[0]) {
        return true;
    }
    char utf8[kPathMax * 3] = {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, utf8, sizeof(utf8), nullptr, nullptr);
    if (n <= 0) {
        return false;
    }
    size_t o = 0;
    for (const char* p = utf8; *p; ++p) {
        char esc[8];
        const size_t len = JsonEscapeOne(static_cast<unsigned char>(*p), esc);
        if (o + len + 1 > cap) {
            out[0] = 0;
            return false;
        }
        memcpy(out + o, esc, len);
        o += len;
    }
    out[o] = 0;
    return true;
}

void UtcStamp(char* out, size_t cap) {
    SYSTEMTIME t{};
    ::GetSystemTime(&t);
    _snprintf_s(out, cap, _TRUNCATE, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay,
                t.wHour, t.wMinute, t.wSecond);
}

}  // namespace

void VerifyClosedFile(const wchar_t* path, ReopenVerdict* out) {
    if (!out) {
        return;
    }
    out->verified = false;
    out->chapters = 0;
    out->bytes = 0;
    out->detail[0] = 0;

    if (!path || !path[0]) {
        SetDetail(out, "no capture file was open, so there was nothing to re-open");
        return;
    }

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
        SetDetail(out, "the closed capture is not on disk (GetFileAttributesEx failed, %lu)",
                  ::GetLastError());
        return;
    }
    out->bytes = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;

    DemoReader reader;
    if (!reader.Open(path)) {
        const char* why = reader.RefusalReason();
        SetDetail(out, "the shipped reader refused the closed file: %s",
                  why ? why : "no reason given");
        return;
    }
    const ReaderStats s = reader.Stats();
    const size_t chapters = reader.ChapterCount();
    // The container's own two chapters -- the MANIFEST it opens with and the INDEX Close() appends --
    // are not chapters of the capture, and counting them here would put a number in the sentinel that
    // is two larger than the writer's for every good file. Two counts of "chapters" that always
    // disagree teach whoever reads them to ignore a real disagreement.
    uint32_t content = 0;
    for (size_t i = 0; i < chapters; ++i) {
        const uint16_t section = reader.ChapterAt(i).section;
        if (section != kSectionIndex && section != kSectionManifest) {
            ++content;
        }
    }
    out->chapters = content;

    // A recovery scan SUCCEEDS on a half-written file -- that is what it is for (F8) -- so a reader
    // that merely opened the file proves nothing about whether Close() ran. The fast path is the
    // claim: the trailer index was read, so the writer got as far as writing one.
    if (reader.WasRecovered() || !s.fastPath || !s.trailerPresent) {
        SetDetail(out,
                  "the reader had to recover the file rather than read its index (fastPath=%d, "
                  "trailer=%d, %u resync(s), %llu byte(s) unattributed): Close() did not finish",
                  int(s.fastPath), int(s.trailerPresent), s.resyncs,
                  static_cast<unsigned long long>(s.skippedBytes));
        return;
    }
    if (!s.footerPresent) {
        SetDetail(out, "no footer: the file ends before the record Close() writes last");
        return;
    }
    if (s.truncatedTailBytes) {
        SetDetail(out, "%llu byte(s) hang off the tail past the last whole chapter",
                  static_cast<unsigned long long>(s.truncatedTailBytes));
        return;
    }
    if (chapters == 0) {
        SetDetail(out, "the container is closed but indexes no chapters, so it holds no capture");
        return;
    }
    // The index claiming a chapter and the payload being there are two facts, and the tail is exactly
    // where they come apart: a file truncated after its index was written still has an index naming a
    // chapter whose bytes are gone.
    DecodedChapter last{};
    if (!reader.DecodeChapter(chapters - 1, last)) {
        SetDetail(out, "the last chapter (%u of %u) does not decode: %s", unsigned(chapters - 1),
                  unsigned(chapters), ChapterFaultName(reader.LastFault()));
        return;
    }
    out->verified = true;
    SetDetail(out,
              "re-opened through the shipped reader: index and footer present, no recovery scan, no "
              "truncated tail, %u capture chapter(s) plus the manifest and the index, last chapter decoded",
              content);
}

bool DirectoryOfFile(const wchar_t* path, wchar_t* out, size_t cap) {
    if (!out || cap == 0) {
        return false;
    }
    out[0] = 0;
    if (!path || !path[0]) {
        return false;
    }
    const wchar_t* cut = nullptr;
    for (const wchar_t* p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') {
            cut = p;
        }
    }
    if (!cut || cut == path) {
        return false;
    }
    const size_t len = static_cast<size_t>(cut - path);
    if (len + 1 > cap) {
        return false;
    }
    memcpy(out, path, len * sizeof(wchar_t));
    out[len] = 0;
    return true;
}

bool StopRequestPresent(const wchar_t* dir, uint64_t notBefore) {
    if (!dir || !dir[0]) {
        return false;
    }
    wchar_t path[kPathMax];
    if (_snwprintf_s(path, kPathMax, _TRUNCATE, L"%s\\%s", dir, kStopRequestName) < 0) {
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
        return false;
    }
    const uint64_t written = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                             fad.ftLastWriteTime.dwLowDateTime;
    return written >= notBefore;
}

namespace {

// The one writer. `replaceExisting` is the whole difference between the two public names: the
// launch-scoped path is a pointer at the latest close and is meant to be replaced, and the per-close
// path is evidence and must not be.
bool WriteSentinelAs(const wchar_t* dir, const StopFacts& facts, const wchar_t* name,
                     bool replaceExisting, char* err, size_t errCap) {
    wchar_t finalPath[kPathMax];
    wchar_t tempPath[kPathMax];
    if (_snwprintf_s(finalPath, kPathMax, _TRUNCATE, L"%s\\%s", dir, name) < 0 ||
        _snwprintf_s(tempPath, kPathMax, _TRUNCATE, L"%s\\%s.tmp", dir, name) < 0) {
        if (errCap) {
            strcpy_s(err, errCap, "the sentinel path did not fit");
        }
        return false;
    }

    char file[kPathMax * 3] = {};
    if (!JsonEscapeUtf8(facts.file, file, sizeof(file))) {
        if (errCap) {
            strcpy_s(err, errCap, "the capture path would not encode as JSON");
        }
        return false;
    }
    char detail[sizeof(facts.verdict.detail) * 2] = {};
    {
        wchar_t wide[sizeof(facts.verdict.detail)] = {};
        ::MultiByteToWideChar(CP_UTF8, 0, facts.verdict.detail, -1, wide,
                              int(sizeof(wide) / sizeof(wide[0])));
        JsonEscapeUtf8(wide, detail, sizeof(detail));
    }
    // Every cause that asked for this close, as a JSON array, so a reader gets the whole list and
    // not only the word the close was filed under. The bit-to-name mapping is the
    // enum's own, taken from CloseCauseBit and StopReasonName rather than restated here.
    char causeList[96] = {};
    for (uint8_t r = 0; r <= static_cast<uint8_t>(StopReason::Disabled); ++r) {
        if (!(facts.closeCauses & CloseCauseBit(static_cast<StopReason>(r)))) {
            continue;
        }
        if (causeList[0]) {
            strcat_s(causeList, sizeof(causeList), ", ");
        }
        strcat_s(causeList, sizeof(causeList), "\"");
        strcat_s(causeList, sizeof(causeList), StopReasonName(static_cast<StopReason>(r)));
        strcat_s(causeList, sizeof(causeList), "\"");
    }

    // The roster's refusals, by reason, with the engine's own word for the most recent of each. Only
    // reasons that actually fired are listed: a document naming all twelve at zero reads as a report
    // on twelve things that went wrong.
    //
    // ★ L2-RUN-5 F2. This built each entry with _snprintf_s(_TRUNCATE) into 96 bytes. Two of the
    // engine's refusal sentences are longer than that, so the entry was cut MID-STRING -- no closing
    // quote, no closing brace -- and EVERY sentinel of EVERY launch was unparseable: the harness
    // read `CAPTURE-UNREADABLE`, discarded three intact documents, and then reported that none
    // existed. Size from the input or refuse by name: the buffers below are sized from the longest
    // value that can arrive, a value that still will not fit is escaped and FLAGGED rather than cut
    // inside a token, and an entry that will not fit the list is COUNTED rather than half-appended
    // (strcat_s would have aborted the process instead).
    char rosterRefusalList[kRefusalListCap] = {};
    uint32_t refusalsOmitted = 0;
    for (size_t i = 0; i < kRosterRefusalCount; ++i) {
        if (!facts.rosterRefusals.byReason[i]) {
            continue;
        }
        char why[kRefusalWhyCap];
        const bool whole =
            JsonEscapeNarrow(facts.rosterRefusals.wordByReason[i]
                                 ? facts.rosterRefusals.wordByReason[i]
                                 : "unnamed",
                             why, sizeof(why));
        char one[kRefusalEntryCap];
        _snprintf_s(one, sizeof(one), _TRUNCATE,
                    "%s\"%s\": {\"count\": %u, \"why\": \"%s\"%s}",
                    rosterRefusalList[0] ? ", " : "",
                    RosterRefusalName(static_cast<RosterRefusal>(i)),
                    unsigned(facts.rosterRefusals.byReason[i]), why,
                    whole ? "" : ", \"whyTruncated\": true");
        if (strlen(rosterRefusalList) + strlen(one) + 1 > sizeof(rosterRefusalList)) {
            ++refusalsOmitted;
            continue;
        }
        strcat_s(rosterRefusalList, sizeof(rosterRefusalList), one);
    }
    // The producer's refusals, same shape and same rule: a reason that fired appears
    // by its own name, so `blocks: 0` in the object below always comes with what stopped them.
    char identityRefusalList[kRefusalListCap] = {};
    for (size_t i = 0; i < kIdentityEmitRefusalCount; ++i) {
        if (!facts.identityRefusals[i]) {
            continue;
        }
        char one[kRefusalEntryCap];
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%s\"%s\": %u",
                    identityRefusalList[0] ? ", " : "",
                    IdentityEmitRefusalText(static_cast<IdentityEmitRefusal>(i)),
                    unsigned(facts.identityRefusals[i]));
        if (strlen(identityRefusalList) + strlen(one) + 1 > sizeof(identityRefusalList)) {
            continue;
        }
        strcat_s(identityRefusalList, sizeof(identityRefusalList), one);
    }
    char identityJsonWhy[kRefusalWhyCap];
    JsonEscapeNarrow(facts.identityLastJsonWhy ? facts.identityLastJsonWhy : "none",
                     identityJsonWhy, sizeof(identityJsonWhy));

    // The scoreboard's refusals, same shape and same rule as the roster's above: only reasons that
    // actually fired, each with the engine's own word, and an entry that will not fit is COUNTED.
    char statsRefusalList[kRefusalListCap] = {};
    uint32_t statsRefusalsOmitted = facts.statsRefusals.overflow;
    for (uint8_t i = 0; i < facts.statsRefusals.used; ++i) {
        char why[kRefusalWhyCap];
        const bool whole = JsonEscapeNarrow(
            facts.statsRefusals.word[i] ? facts.statsRefusals.word[i] : "unnamed", why, sizeof(why));
        char one[kRefusalEntryCap];
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%s\"%u\": {\"count\": %u, \"why\": \"%s\"%s}",
                    statsRefusalList[0] ? ", " : "", unsigned(facts.statsRefusals.reason[i]),
                    unsigned(facts.statsRefusals.count[i]), why,
                    whole ? "" : ", \"whyTruncated\": true");
        if (strlen(statsRefusalList) + strlen(one) + 1 > sizeof(statsRefusalList)) {
            ++statsRefusalsOmitted;
            continue;
        }
        strcat_s(statsRefusalList, sizeof(statsRefusalList), one);
    }
    char statsOmittedLine[64] = {};
    if (statsRefusalsOmitted) {
        _snprintf_s(statsOmittedLine, sizeof(statsOmittedLine), _TRUNCATE,
                    "    \"refusalsOmitted\": %u,\n", unsigned(statsRefusalsOmitted));
    }
    // WHICH GATE refused a body read, by name, and only the arms that actually fired -- a block
    // naming all nineteen at zero reads as a report on nineteen things that went wrong.
    // BodyRefusal::None is "nothing refused", i.e. every healthy read, so it is skipped and an
    // EMPTY {} is the healthy shape: any name in here is the diagnosis. The buffer is sized from
    // the input rather than guessed (L2-RUN-5 F2): every arm at its widest is
    // `, "zoneUnreadable": 4294967295` = 30 bytes, so all of them fit and no entry can be dropped
    // or cut mid-token.
    char bodyRefusalList[kBodyRefusalCount * 32 + 1] = {};
    for (unsigned i = 1; i < kBodyRefusalCount; ++i) {
        if (!facts.bodyRefusals[i]) {
            continue;
        }
        char one[64];
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%s\"%s\": %u", bodyRefusalList[0] ? ", " : "",
                    BodyRefusalName(static_cast<BodyRefusal>(i)),
                    unsigned(facts.bodyRefusals[i]));
        strcat_s(bodyRefusalList, sizeof(bodyRefusalList), one);
    }
    if (facts.bodyRefusalUnknown) {
        char one[64];
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%s\"unknown\": %u",
                    bodyRefusalList[0] ? ", " : "", unsigned(facts.bodyRefusalUnknown));
        strcat_s(bodyRefusalList, sizeof(bodyRefusalList), one);
    }

    // Same shape, same buffer arithmetic: the widest entry is `, "notStoredByEngine": 4294967295`
    // at 32 bytes, so 40 per arm fits every one of them whole.
    char angVelList[kAngVelSourceCount * 40 + 1] = {};
    for (unsigned i = 0; i < kAngVelSourceCount; ++i) {
        if (!facts.angVelBy[i]) {
            continue;
        }
        char one[64];
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%s\"%s\": %u", angVelList[0] ? ", " : "",
                    AngVelSourceName(static_cast<AngVelSource>(i)),
                    unsigned(facts.angVelBy[i]));
        strcat_s(angVelList, sizeof(angVelList), one);
    }
    if (facts.angVelUnknown) {
        char one[64];
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%s\"unknown\": %u",
                    angVelList[0] ? ", " : "", unsigned(facts.angVelUnknown));
        strcat_s(angVelList, sizeof(angVelList), one);
    }

    // The FIELD each refusal named, in the order they happened. The widest entry is
    // `, { "stream": "S_BODY_SAMPLED", "field": "lastHeldCom", "value": "0xFFFFFFFFFFFFFFFF",
    // "width": 4294967295 }` at 112 bytes, so this holds every one of them whole. An empty array is
    // a statement: the count above says how many refused, this says how many could be named.
    static const size_t kRefusalEntryMax = 128;
    char refusedFieldList[BuilderStats::kMaxRefusalsNamed * kRefusalEntryMax + 1] = {};
    for (uint32_t i = 0; i < facts.encodeRefusalsNamed; ++i) {
        const EncodeRefusalNote& n = facts.encodeRefusalNotes[i];
        char one[kRefusalEntryMax];
        _snprintf_s(one, sizeof(one), _TRUNCATE,
                    "%s{ \"stream\": \"%s\", \"field\": \"%s\", \"value\": \"0x%llX\", "
                    "\"width\": %u }",
                    refusedFieldList[0] ? ", " : "", n.stream ? n.stream : "?",
                    n.field ? n.field : "?", static_cast<unsigned long long>(n.value),
                    unsigned(n.width));
        strcat_s(refusedFieldList, sizeof(refusedFieldList), one);
    }

    char refusalsOmittedLine[64] = {};
    if (refusalsOmitted) {
        _snprintf_s(refusalsOmittedLine, sizeof(refusalsOmittedLine), _TRUNCATE,
                    "    \"refusalsOmitted\": %u,\n", refusalsOmitted);
    }

    char closedAt[32];
    UtcStamp(closedAt, sizeof(closedAt));
    // `startedAt` is `launchFileTime` in the form a person can read; they are one fact, so it is derived
    // here rather than passed in and allowed to disagree with the id in the file's own name.
    char startedAt[32] = {};
    {
        FILETIME ft{};
        ft.dwHighDateTime = static_cast<DWORD>(facts.launchFileTime >> 32);
        ft.dwLowDateTime = static_cast<DWORD>(facts.launchFileTime & 0xFFFFFFFFull);
        SYSTEMTIME st{};
        if (::FileTimeToSystemTime(&ft, &st)) {
            _snprintf_s(startedAt, sizeof(startedAt), _TRUNCATE,
                        "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour,
                        st.wMinute, st.wSecond);
        }
    }

    // How many of the watched names ever arrived. Counted from the mask rather than carried beside
    // it: two fields holding one fact can disagree, and the mask is the one the deriver sets.
    uint32_t namesSeen = 0;
    for (uint32_t bit = facts.markers.namesSeenMask; bit; bit &= bit - 1) {
        ++namesSeen;
    }

    // `chapters` is the READER's count and `writerChapters` the writer's. They are the same number on
    // a good file, and a stop that lost chapters is exactly the case where reporting one of them as
    // both would hide the loss.
    // THE FIRST BODY REFUSAL OF THE CAPTURE, whole. `refused` counts and `refusedBy` names the
    // arm; neither says what the gates SAW, and L2 #11 read "the bit is CLEAR" off an arm that
    // cannot tell clear from never-asked. Rendered by the ENGINE's own DescribeDiag rather than
    // by a field list copied here: a second copy of that list is a second thing to drift, and
    // its `?` for a field the walk never reached is a distinction JSON has no number for.
    // Emitted ONLY when something refused -- the key's ABSENCE is what says nothing did.
    char firstRefusalLine[replay::kBodyDiagTextMax * 2] = {};
    if (facts.bodiesRefused > 0 && facts.bodyFirstRefusalSeen) {
        char diagText[replay::kBodyDiagTextMax];
        _snprintf_s(firstRefusalLine, sizeof firstRefusalLine, _TRUNCATE,
                    ",\n    \"firstRefusal\": { \"gate\": \"%s\", \"source\": \"%s\", "
                    "\"diag\": \"%s\" }",
                    replay::BodyRefusalName(facts.bodyFirstRefusal.refusedAt),
                    replay::BodySourceName(facts.bodyFirstRefusal.source),
                    replay::DescribeDiag(facts.bodyFirstRefusal.diag, diagText,
                                         sizeof diagText));
    }

    // 9216 + the refusedBy list, which is bounded by its own buffer above, + the line above.
    char json[9984 + 128 + sizeof firstRefusalLine + sizeof angVelList + sizeof refusedFieldList];
    // Built before the format call because an ABSENT field is a missing LINE, not an empty value:
    // a reader that finds "roundRowCount": 0 must be able to trust that the table was reached.
    char roundRows[64] = {};
    if (facts.roundRowsRecorded) {
        _snprintf_s(roundRows, sizeof roundRows, _TRUNCATE, "    \"roundRowCount\": %u,\n",
                    unsigned(facts.roundRowCount));
    }
    char roundCounter[32] = "null";
    if (facts.roundCounterRead) {
        _snprintf_s(roundCounter, sizeof roundCounter, _TRUNCATE, "%u",
                    unsigned(facts.roundCounter));
    }
    const int n = _snprintf_s(json, sizeof(json), _TRUNCATE,
                              "{\n"
                              "  \"launchFileTime\": \"%016llX\",\n"
                              "  \"pid\": %lu,\n"
                              "  \"startedAt\": \"%s\",\n"
                              "  \"file\": \"%s\",\n"
                              "  \"bytes\": %llu,\n"
                              "  \"chapters\": %u,\n"
                              "  \"records\": %llu,\n"
                              "  \"writerChapters\": %llu,\n"
                              "  \"closedAt\": \"%s\",\n"
                              "  \"closeSeq\": %lu,\n"
                              // ★ Only the launch-scoped pointer carries this. The two
                              // documents are otherwise byte-identical in shape -- same writer, same
                              // facts -- so a reader that opens the pointer alone had nothing
                              // telling it this is a duplicate of the latest per-close document and
                              // not a close of its own.
                              "%s"
                              "  \"reason\": \"%s\",\n"
                              "  \"matchStillLive\": %s,\n"
                              "  \"reopenSuppressed\": %s,\n"
                              "  \"fileClosed\": %s,\n"
                              "  \"openFailures\": %u,\n"
                              "  \"openFailTotal\": %u,\n"
                              "  \"openFailState\": \"%s\",\n"
                              "  \"closeCauses\": [%s],\n"
                              "  \"workerJoin\": \"%s\",\n"
                              "  \"verified\": %s,\n"
                              "  \"verifyDetail\": \"%s\",\n"
                              "  \"taps\": {\n"
                              // ★ One vocabulary. These are the TapRegistration struct's
                              // own field names, which is what the keyframe block already writes and
                              // what the log line now says, so a harness correlating the two never
                              // needs a mapping table. `retrying` and `excludedByDesign` were this
                              // document's private spelling of `deferred` and `excluded`.
                              "    \"registered\": %u,\n"
                              "    \"deferred\": %u,\n"
                              "    \"givenUp\": %u,\n"
                              "    \"unavailable\": %u,\n"
                              "    \"excluded\": %u,\n"
                              "    \"total\": %u,\n"
                              "    \"tableRows\": %u,\n"
                              "    \"started\": %s\n"
                              "  },\n"
                              // ★ The marker deriver's own field names, so a harness
                              // correlating this with the `markers:` log line needs no mapping table.
                              // namesWatched 0 means the watch table was never built; namesSeen 0
                              // with rowsSeen > 0 means the symbols are wrong rather than the match
                              // being quiet.
                              "  \"markers\": {\n"
                              "    \"namesWatched\": %u,\n"
                              "    \"namesSeen\": %u,\n"
                              "    \"namesSeenMask\": %u,\n"
                              "    \"rowsSeen\": %u,\n"
                              "    \"emitted\": %u,\n"
                              "    \"timeRefused\": %u,\n"
                              "    \"markerSource\": \"%s\",\n"
                              "    \"localBoundaries\": %u,\n"
                              "    \"corroboratorAsked\": %s,\n"
                              "    \"wireRowsSeen\": %u,\n"
                              "    \"netvarRecords\": %llu,\n"
                              "    \"wireMarksHeldDropped\": %llu,\n"
                              "    \"boundaryMarksNotCut\": %llu\n"
                              "  },\n"
                              // `roundRowCount` is ABSENT when this close
                              // never reached SetRoundTable, and present as 0 when it reached it
                              // with an empty table. A harness reading only this document can now
                              // tell those apart; before, both were silence.
                              "  \"oracle\": {\n"
                              "    \"samples\": %llu,\n"
                              "    \"ok\": %llu,\n"
                              "    \"dropped\": %llu,\n"
                              "    \"status\": \"%s\",\n"
                              "    \"notBound\": \"%s\"\n"
                              "  },\n"
                              "  \"rounds\": {\n"
                              "%s"
                              "    \"refusedNonAscending\": %u,\n"
                              "    \"rebasedAfterReset\": %u,\n"
                              "    \"phase\": \"%s\",\n"
                              "    \"roundCounter\": %s\n"
                              "  },\n"
                              "  \"roster\": {\n"
                              "    \"rows\": %u,\n"
                              "    \"localRows\": %u,\n"
                              "    \"bandRefusals\": %u,\n"
                              "    \"bandClamped\": %u,\n"
                              "    \"unchangedRowScans\": %u,\n"
                              "    \"signalUnchangedTicks\": %u,\n"
                              "%s"
                              "    \"rosterRefusals\": {%s}\n"
                              "  },\n"
                              "  \"identity\": {\n"
                              "    \"blocks\": %u,\n"
                              "    \"profileRefused\": %u,\n"
                              "    \"profileRefusalWhy\": \"%s\",\n"
                              "    \"keyless\": %u,\n"
                              "    \"slotsTruncated\": %u,\n"
                              "    \"slotsTruncationUnknown\": %u,\n"
                              "    \"deferredToNextScan\": %u,\n"
                              "    \"emitRefusals\": {%s}\n"
                              "  },\n"
                              "  \"stats\": {\n"
                              "    \"records\": %llu,\n"
                              "    \"players\": %u,\n"
                              "    \"keyframes\": %llu,\n"
                              "    \"sweeps\": %llu,\n"
                              "    \"signalUnchangedTicks\": %llu,\n"
                              "    \"sourceAbsentTicks\": %llu,\n"
                              "%s"
                              "    \"refusalsByReason\": {%s}\n"
                              "  },\n"
                              // `sampled` is the record count for stream 7 --
                              // the only world position the format carries -- and the four source
                              // counters below are what tell a capture of everyone except the owner
                              // (history only) from a whole one. They sum to the bodies walked.
                              "  \"bodies\": {\n"
                              "    \"sampled\": %u,\n"
                              "    \"refused\": %u,\n"
                              "    \"local\": %u,\n"
                              "    \"history\": %u,\n"
                              "    \"none\": %u,\n"
                              "    \"notAsked\": %u,\n"
                              // WHICH gate refused, non-zero arms only. Empty is healthy; a name
                              // here IS the diagnosis L2 #9 had to guess at.
                              // WHICH ARM answered omega, non-zero arms only. `unavailable` is the
                              // ncaTransform pose arm: those bodies carry no rotation rate at all.
                              "    \"angVelBy\": {%s},\n"
                              "    \"refusedBy\": {%s},\n"
                              // WHICH FIELD the encoder could not store, and what it held. The gate
                              // name above says where the READ stopped; this says what the WRITE
                              // could not fit, which is the diagnosis a bare count cannot carry.
                              "    \"refusedFields\": [%s]%s\n"
                              "  },\n"
                              // L3-RUN-13 F3. The cost block. A close is the only document a SURVIVING launch
                              // writes about the cost meter, and it said nothing: the tail rung's verdict
                              // reached the log only inside the kill line. `measured` beside `notRun` is
                              // what separates a rung that ran and passed from one that was never askable,
                              // which a zero `kills` alone cannot say.
                              "  \"tail\": {\n"
                              "    \"measured\": %u,\n"
                              "    \"notRun\": %u,\n"
                              "    \"overBudget\": %u,\n"
                              "    \"kills\": %u,\n"
                              "    \"chargedMeanUs\": %u\n"
                              "  },\n"
                              "  \"demoUuid\": \"%016llX%016llX\"\n"
                              "}\n",
                              static_cast<unsigned long long>(facts.launchFileTime),
                              static_cast<unsigned long>(facts.pid), startedAt,
                              file, static_cast<unsigned long long>(facts.verdict.bytes),
                              facts.verdict.chapters, static_cast<unsigned long long>(facts.records),
                              static_cast<unsigned long long>(facts.chapters), closedAt,
                              static_cast<unsigned long>(facts.closeSeq),
                              replaceExisting ? "  \"latestClosePointer\": true,\n" : "",
                              StopReasonName(facts.reason), facts.matchStillLive ? "true" : "false",
                              facts.reopenSuppressed ? "true" : "false",
                              facts.fileClosed ? "true" : "false", unsigned(facts.openFailures),
                              unsigned(facts.openFailTotal),
                              facts.openFailState ? facts.openFailState : "none", causeList,
                              facts.workerJoin ? facts.workerJoin : "n/a",
                              facts.verdict.verified ? "true" : "false", detail,
                              unsigned(facts.taps.registered), unsigned(facts.taps.deferred),
                              unsigned(facts.taps.givenUp), unsigned(facts.taps.unavailable),
                              unsigned(facts.taps.excluded), unsigned(facts.taps.total),
                              unsigned(facts.taps.tableRows),
                              facts.taps.started ? "true" : "false",
                              unsigned(facts.markers.namesWatched), unsigned(namesSeen),
                              unsigned(facts.markers.namesSeenMask),
                              unsigned(facts.markers.rowsSeen), unsigned(facts.markers.emitted),
                              unsigned(facts.markers.timeRefused),
                              MarkerSourceName(facts.markerSource),
                              unsigned(facts.localBoundaries),
                              facts.corroboratorAsked ? "true" : "false",
                              unsigned(facts.wireRowsSeen),
                              static_cast<unsigned long long>(facts.netvarRecords),
                              static_cast<unsigned long long>(facts.wireMarksHeldDropped),
                              static_cast<unsigned long long>(facts.boundaryMarksNotCut),
                              static_cast<unsigned long long>(facts.oracleSamples),
                              static_cast<unsigned long long>(facts.oracleOk),
                              static_cast<unsigned long long>(facts.oracleDropped),
                              facts.oracleStatus ? facts.oracleStatus : "not-sampled",
                              facts.oracleNotBound ? facts.oracleNotBound : "",
                              roundRows, unsigned(facts.roundsRefusedNonAscending),
                              unsigned(facts.roundsRebasedAfterReset),
                              facts.phase ? facts.phase : "not-sampled", roundCounter,
                              unsigned(facts.rosterRows), unsigned(facts.rosterLocalRows),
                              unsigned(facts.rosterBandRefusals),
                              unsigned(facts.rosterBandClamped),
                              unsigned(facts.rosterUnchanged),
                              unsigned(facts.rosterSignalUnchanged), refusalsOmittedLine,
                              rosterRefusalList,
                              unsigned(facts.identityBlocks),
                              unsigned(facts.identityJsonRefused), identityJsonWhy,
                              unsigned(facts.identityKeyless),
                              unsigned(facts.identityTruncated),
                              unsigned(facts.identityTruncationUnknown),
                              unsigned(facts.identityDeferred), identityRefusalList,
                              static_cast<unsigned long long>(facts.statsRecords),
                              unsigned(facts.statsPlayers),
                              static_cast<unsigned long long>(facts.statsKeyframes),
                              static_cast<unsigned long long>(facts.statsSweeps),
                              static_cast<unsigned long long>(facts.statsSignalUnchanged),
                              static_cast<unsigned long long>(facts.statsSourceAbsent),
                              statsOmittedLine, statsRefusalList,
                              unsigned(facts.bodiesSampled), unsigned(facts.bodiesRefused),
                              unsigned(facts.bodiesLocal), unsigned(facts.bodiesHistory),
                              unsigned(facts.bodiesNone), unsigned(facts.bodiesNotAsked),
                              angVelList, bodyRefusalList, refusedFieldList, firstRefusalLine,
                              unsigned(facts.costTail.measured), unsigned(facts.costTail.notRun),
                              unsigned(facts.costTail.overBudget), unsigned(facts.costTail.kills),
                              unsigned(facts.costTail.chargedMeanUs),
                              static_cast<unsigned long long>(facts.demoUuidHi),
                              static_cast<unsigned long long>(facts.demoUuidLo));
    if (n < 0) {
        if (errCap) {
            strcpy_s(err, errCap, "the sentinel document did not fit its buffer");
        }
        return false;
    }

    // Written under a temporary name and moved into place: the harness polls for this file, and a
    // reader that arrives between the create and the write must not see an empty document and read it
    // as a stop that recorded nothing.
    HANDLE h = ::CreateFileW(tempPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (errCap) {
            _snprintf_s(err, errCap, _TRUNCATE, "could not create the sentinel (%lu)",
                        ::GetLastError());
        }
        return false;
    }
    DWORD wrote = 0;
    const BOOL ok = ::WriteFile(h, json, static_cast<DWORD>(n), &wrote, nullptr);
    const DWORD writeErr = ::GetLastError();
    ::FlushFileBuffers(h);
    ::CloseHandle(h);
    if (!ok || wrote != static_cast<DWORD>(n)) {
        if (errCap) {
            _snprintf_s(err, errCap, _TRUNCATE, "short write to the sentinel (%lu of %d bytes, %lu)",
                        wrote, n, writeErr);
        }
        ::DeleteFileW(tempPath);
        return false;
    }
#if defined(W2A_MUTATE_SENTINEL_REPLACES)
    // Mutation control: the pre-fix shape -- every close overwrites the last close's
    // document, whatever it is named.
    replaceExisting = true;
#endif
    if (!::MoveFileExW(tempPath, finalPath, replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0)) {
        const DWORD moveErr = ::GetLastError();
        if (errCap) {
            if (!replaceExisting &&
                (moveErr == ERROR_ALREADY_EXISTS || moveErr == ERROR_FILE_EXISTS)) {
                // The refusal IS the invariant. Overwriting here is what lost L5 run 3's match-end
                // sentinel, so a name collision is reported as a failure and the document that is
                // already there is left exactly as it is.
                _snprintf_s(err, errCap, _TRUNCATE,
                            "a sentinel is already on disk at %ls and was NOT replaced", finalPath);
            } else {
                _snprintf_s(err, errCap, _TRUNCATE, "could not move the sentinel into place (%lu)",
                            moveErr);
            }
        }
        ::DeleteFileW(tempPath);
        return false;
    }
    return true;
}

// Both public writers reduce to a name plus a replace rule. `dir` is checked once, here.
bool WriteSentinelNamed(const wchar_t* dir, const StopFacts& facts, bool perClose, char* err,
                        size_t errCap) {
    if (errCap) {
        err[0] = 0;
    }
    if (!dir || !dir[0]) {
        if (errCap) {
            strcpy_s(err, errCap, "no directory to write the sentinel into");
        }
        return false;
    }
    wchar_t name[64];
    const bool named = perClose ? StopSentinelNameForClose(facts.launchFileTime, facts.pid,
                                                           facts.closeSeq, name, 64)
                                : StopSentinelNameFor(facts.launchFileTime, facts.pid, name, 64);
    if (!named) {
        if (errCap) {
            strcpy_s(err, errCap, "the sentinel name did not fit");
        }
        return false;
    }
    return WriteSentinelAs(dir, facts, name, !perClose, err, errCap);
}

}  // namespace

namespace {

SRWLOCK gLastFactsLock = SRWLOCK_INIT;
StopFactsSnapshot gLastFacts = {};
bool gHaveLastFacts = false;

void CopyNarrow(char* out, size_t cap, const char* in) {
    if (!cap) return;
    out[0] = 0;
    if (in) strncpy_s(out, cap, in, _TRUNCATE);
}

void PublishLastStopFacts(const StopFacts& f) {
    StopFactsSnapshot s = {};
    if (f.file) wcsncpy_s(s.file, f.file, _TRUNCATE);
    CopyNarrow(s.openFailState, sizeof(s.openFailState), f.openFailState);
    CopyNarrow(s.workerJoin, sizeof(s.workerJoin), f.workerJoin);
    // A null `phase` is "no game-state sample ever reached the tick", which on a solo host is the
    // honest answer. It becomes its own boolean rather than the word "not-sampled": a reader must
    // not have to compare against a string to learn that nothing was sampled.
    s.phaseSampled = f.phase != nullptr;
    CopyNarrow(s.phase, sizeof(s.phase), f.phase);
    s.launchFileTime = f.launchFileTime;
    s.records = f.records;
    s.chapters = f.chapters;
    s.netvarRecords = f.netvarRecords;
    s.wireMarksHeldDropped = f.wireMarksHeldDropped;
    s.boundaryMarksNotCut = f.boundaryMarksNotCut;
    s.statsRecords = f.statsRecords;
    s.statsKeyframes = f.statsKeyframes;
    s.statsSweeps = f.statsSweeps;
    s.statsSignalUnchanged = f.statsSignalUnchanged;
    s.statsSourceAbsent = f.statsSourceAbsent;
    s.demoUuidLo = f.demoUuidLo;
    s.demoUuidHi = f.demoUuidHi;
    s.pid = f.pid;
    s.closeSeq = f.closeSeq;
    s.openFailures = f.openFailures;
    s.openFailTotal = f.openFailTotal;
    s.localBoundaries = f.localBoundaries;
    s.wireRowsSeen = f.wireRowsSeen;
    s.roundRowCount = f.roundRowCount;
    s.roundCounter = f.roundCounter;
    s.roundsRefusedNonAscending = f.roundsRefusedNonAscending;
    s.roundsRebasedAfterReset = f.roundsRebasedAfterReset;
    s.rosterBandRefusals = f.rosterBandRefusals;
    s.rosterUnchanged = f.rosterUnchanged;
    s.rosterSignalUnchanged = f.rosterSignalUnchanged;
    s.identityBlocks = f.identityBlocks;
    s.identityJsonRefused = f.identityJsonRefused;
    s.identityKeyless = f.identityKeyless;
    s.identityTruncated = f.identityTruncated;
    s.identityTruncationUnknown = f.identityTruncationUnknown;
    s.identityDeferred = f.identityDeferred;
    s.identityLastJsonWhy = f.identityLastJsonWhy;
    for (size_t i = 0; i < kIdentityEmitRefusalCount; ++i) {
        s.identityRefusals[i] = f.identityRefusals[i];
    }
    s.rosterBandClamped = f.rosterBandClamped;
    s.rosterRows = f.rosterRows;
    s.rosterLocalRows = f.rosterLocalRows;
    s.statsPlayers = f.statsPlayers;
    s.markerSource = f.markerSource;
    s.reason = static_cast<uint8_t>(f.reason);
    s.closeCauses = f.closeCauses;
    s.corroboratorAsked = f.corroboratorAsked;
    s.matchStillLive = f.matchStillLive;
    s.reopenSuppressed = f.reopenSuppressed;
    s.fileClosed = f.fileClosed;
    s.roundRowsRecorded = f.roundRowsRecorded;
    s.roundCounterRead = f.roundCounterRead;
    s.verdict = f.verdict;
    s.taps = f.taps;
    s.markers = f.markers;
    s.rosterRefusals = f.rosterRefusals;
    s.statsRefusals = f.statsRefusals;
    s.bodiesSampled = f.bodiesSampled;
    s.bodiesRefused = f.bodiesRefused;
    s.bodiesLocal = f.bodiesLocal;
    s.bodiesHistory = f.bodiesHistory;
    s.bodiesNone = f.bodiesNone;
    s.bodiesNotAsked = f.bodiesNotAsked;
    memcpy(s.bodyRefusals, f.bodyRefusals, sizeof s.bodyRefusals);
    s.bodyRefusalUnknown = f.bodyRefusalUnknown;
    s.bodyFirstRefusalSeen = f.bodyFirstRefusalSeen;
    s.bodyFirstRefusal = f.bodyFirstRefusal;
    s.encodeRefusalsNamed = f.encodeRefusalsNamed;
    for (uint32_t i = 0; i < BuilderStats::kMaxRefusalsNamed; ++i) {
        s.encodeRefusalNotes[i] = f.encodeRefusalNotes[i];
    }

    ::AcquireSRWLockExclusive(&gLastFactsLock);
    gLastFacts = s;
    gHaveLastFacts = true;
    ::ReleaseSRWLockExclusive(&gLastFactsLock);
}

}  // namespace

namespace {
// One holder, one writer (capture.cpp's SetPhase), any number of readers on any thread.
std::atomic<bool> gCaptureRecording{false};
}  // namespace

void PublishCaptureRecording(bool recording) {
    gCaptureRecording.store(recording, std::memory_order_release);
}

bool CaptureIsRecording() { return gCaptureRecording.load(std::memory_order_acquire); }

bool LastStopFacts(StopFactsSnapshot& out) {
    ::AcquireSRWLockShared(&gLastFactsLock);
    const bool have = gHaveLastFacts;
    if (have) out = gLastFacts;
    ::ReleaseSRWLockShared(&gLastFactsLock);
    return have;
}

bool WriteStopSentinel(const wchar_t* dir, const StopFacts& facts, char* err, size_t errCap) {
    // Published whether or not the document reaches disk. A full volume must not also cost the
    // in-process reader its answer, and the write's own success is reported separately.
    PublishLastStopFacts(facts);
    return WriteSentinelNamed(dir, facts, false, err, errCap);
}

bool WriteStopSentinelForClose(const wchar_t* dir, const StopFacts& facts, char* err, size_t errCap) {
    return WriteSentinelNamed(dir, facts, true, err, errCap);
}

bool ArchiveStopSentinel(const wchar_t* dir, const StopFacts& facts, char* err, size_t errCap) {
    if (errCap) {
        err[0] = 0;
    }
    if (!dir || !dir[0]) {
        if (errCap) {
            strcpy_s(err, errCap, "no directory to archive into");
        }
        return false;
    }
    wchar_t archive[kPathMax];
    if (_snwprintf_s(archive, kPathMax, _TRUNCATE, L"%s\\%s", dir, kStopArchiveDirName) < 0) {
        if (errCap) {
            strcpy_s(err, errCap, "the archive path did not fit");
        }
        return false;
    }
    if (!::CreateDirectoryW(archive, nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
        if (errCap) {
            _snprintf_s(err, errCap, _TRUNCATE, "could not create %ls (%lu)", archive,
                        ::GetLastError());
        }
        return false;
    }
    // Per-close, never replaced: the archive is what gets collected into the run directory, so it is
    // the copy that has to hold every close of the launch.
    return WriteStopSentinelForClose(archive, facts, err, errCap);
}

}  // namespace capture
}  // namespace replay
