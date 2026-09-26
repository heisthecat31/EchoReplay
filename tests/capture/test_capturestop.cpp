// W2-A. The design notes' second half: the capture stop closes the file, RE-OPENS it with the shipped
// reader, and writes a sentinel carrying that reader's verdict.
//
// ---------------------------------------------------------------------------------------------
// ★ WHAT THIS ARM IS FOR. F8: no launch in this project's history has ever run `Close()`, and
// `taskkill /F` never will. The fix is in-process, and the trap in a fix like this is that the writer
// certifies its own work -- `Close()` returned, so the file must be good. These cases put a REAL
// container on disk through the REAL writer and then damage it, because the only interesting question
// is whether the verdict can say no.
//
// ★ THE ARM ON THE ARM. Delete any one of VerifyClosedFile's refusals -- the recovery-scan test, the
// footer test, the truncated-tail test, the decode of the last chapter -- and "a tail truncated by
// seven bytes is refused" goes red. Verified by mutation, not by assumption.
// ---------------------------------------------------------------------------------------------

#include "doctest.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <string>

#include "capture/capture_types.h"
#include "capture/capturestop.h"
#include "capture/costmeter.h"
#include "capture/manifestbuild.h"
#include "foundation/demofile/demofile.h"

using namespace replay;
using namespace replay::capture;

namespace {

// A directory of its own per case, under the process temp path. The suite must never write beside the
// user's real captures, and two cases sharing one sentinel name would hide an overwrite.
class ScratchDir {
public:
    explicit ScratchDir(const wchar_t* tag) {
        wchar_t base[MAX_PATH] = {};
        ::GetTempPathW(MAX_PATH, base);
        _snwprintf_s(dir_, MAX_PATH, _TRUNCATE, L"%sreplay_stop_%s_%lu", base, tag,
                     ::GetCurrentProcessId());
        ::CreateDirectoryW(dir_, nullptr);
    }
    ~ScratchDir() {
        wchar_t pattern[MAX_PATH];
        _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\*", dir_);
        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    continue;
                }
                wchar_t victim[MAX_PATH];
                _snwprintf_s(victim, MAX_PATH, _TRUNCATE, L"%s\\%s", dir_, fd.cFileName);
                ::DeleteFileW(victim);
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
        }
        // The archive subdirectory the stop path now writes into. Left behind it would litter the
        // temp path with one directory per case, per run.
        wchar_t archive[MAX_PATH];
        _snwprintf_s(archive, MAX_PATH, _TRUNCATE, L"%s\\%s", dir_, kStopArchiveDirName);
        _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\*", archive);
        h = ::FindFirstFileW(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    continue;
                }
                wchar_t victim[MAX_PATH];
                _snwprintf_s(victim, MAX_PATH, _TRUNCATE, L"%s\\%s", archive, fd.cFileName);
                ::DeleteFileW(victim);
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
        }
        ::RemoveDirectoryW(archive);
        ::RemoveDirectoryW(dir_);
    }
    const wchar_t* Path() const { return dir_; }
    void File(const wchar_t* name, wchar_t* out) const {
        _snwprintf_s(out, MAX_PATH, _TRUNCATE, L"%s\\%s", dir_, name);
    }

private:
    wchar_t dir_[MAX_PATH] = {};
};

// A capture written through the shipping container. `endLastChapter` false leaves a chapter open at
// Close, which is the mid-stop case: the deadline does not wait for a chapter boundary.
void WriteCapture(const wchar_t* path, uint32_t chapters, bool endLastChapter) {
    ManifestBuilder mb;
    mb.Begin(1700000000000000ull, 0, 0x40);
    mb.SetLevel(6300205991959903307ull, 0xF00Dull, 0);
    mb.SetKeyframeIntervalUs(2000000);

    DemoWriter w;
    REQUIRE(w.Open(path, mb.View()));
    uint8_t payload[64];
    for (uint32_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = static_cast<uint8_t>(i * 7 + 1);
    }
    for (uint32_t c = 0; c < chapters; ++c) {
        w.BeginChapter(kSectionCore, 1000000ull * c);
        w.WriteStreamBlock(kStreamGap, kSchemaGap, payload, sizeof(payload), 1, 1);
        if (endLastChapter || c + 1 < chapters) {
            w.EndChapter(1000000ull * (c + 1));
        }
    }
    w.Close();
}

uint64_t FileSize(const wchar_t* path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
        return 0;
    }
    return (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

void TruncateBy(const wchar_t* path, uint64_t bytes) {
    const uint64_t size = FileSize(path);
    REQUIRE(size > bytes);
    HANDLE h = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    LARGE_INTEGER at{};
    at.QuadPart = static_cast<LONGLONG>(size - bytes);
    REQUIRE(::SetFilePointerEx(h, at, nullptr, FILE_BEGIN));
    REQUIRE(::SetEndOfFile(h));
    ::CloseHandle(h);
}

std::string ReadAll(const wchar_t* path) {
    HANDLE h = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return std::string();
    }
    std::string out;
    char buf[4096];
    DWORD got = 0;
    while (::ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) {
        out.append(buf, got);
    }
    ::CloseHandle(h);
    return out;
}

// The value after `"key": ` on its own line, unquoted. Empty means the key is ABSENT, which is a
// different finding from a present-but-empty value and is treated as one below.
std::string RawField(const std::string& doc, const char* key) {
    const std::string needle = std::string("\"") + key + "\": ";
    const size_t at = doc.find(needle);
    if (at == std::string::npos) {
        return std::string();
    }
    const size_t from = at + needle.size();
    size_t to = doc.find('\n', from);
    if (to == std::string::npos) {
        to = doc.size();
    }
    std::string v = doc.substr(from, to - from);
    while (!v.empty() && (v[v.size() - 1] == ',' || v[v.size() - 1] == '\r')) {
        v.erase(v.size() - 1);
    }
    if (v.size() >= 2 && v[0] == '"' && v[v.size() - 1] == '"') {
        v = v.substr(1, v.size() - 2);
    }
    return v;
}

bool Has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// How many `capture_stop.*` documents are in `dir`. The sweep's subject, counted independently of the
// sweep's own return value: a delete that reports 1 and leaves the file is the failure that matters.
uint32_t CountSentinels(const wchar_t* dir) {
    wchar_t pattern[MAX_PATH];
    _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, kStopSentinelPattern);
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    uint32_t n = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            ++n;
        }
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return n;
}

// A launch's facts, as StopCapture fills them.
StopFacts FactsFor(uint64_t launchFileTime, uint32_t pid, const wchar_t* file) {
    StopFacts f{};
    f.launchFileTime = launchFileTime;
    f.pid = pid;
    f.file = file;
    f.reason = StopReason::Deadline;
    // These facts describe a close that closed a file on a launch whose opens all worked, which is
    // what every case in this file writes.
    f.fileClosed = true;
    f.openFailState = "none";
    return f;
}

bool SentinelExists(const wchar_t* dir, uint64_t launchFileTime, uint32_t pid) {
    wchar_t name[64];
    if (!StopSentinelNameFor(launchFileTime, pid, name, 64)) {
        return false;
    }
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, name);
    return ::GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace

TEST_CASE("a closed capture re-opens through the shipped reader and verifies") {
    ScratchDir dir(L"good");
    wchar_t path[MAX_PATH];
    dir.File(L"good.echodemo", path);
    WriteCapture(path, 3, true);

    ReopenVerdict v{};
    VerifyClosedFile(path, &v);
    INFO("detail: ", v.detail);
    CHECK(v.verified);
    CHECK(v.bytes > 0);
    CHECK(v.bytes == FileSize(path));
    CHECK(v.chapters == 3);   // the container's own MANIFEST and INDEX chapters are not the capture's
    CHECK(v.detail[0] != 0);
}

TEST_CASE("a stop that lands mid-chapter still closes the chapter") {
    ScratchDir dir(L"mid");
    wchar_t open[MAX_PATH];
    wchar_t closed[MAX_PATH];
    dir.File(L"midchapter.echodemo", open);
    dir.File(L"whole.echodemo", closed);
    WriteCapture(open, 3, false);   // the third chapter is never EndChapter'd
    WriteCapture(closed, 3, true);

    ReopenVerdict mid{};
    ReopenVerdict whole{};
    VerifyClosedFile(open, &mid);
    VerifyClosedFile(closed, &whole);
    INFO("mid: ", mid.detail);
    CHECK(mid.verified);
    // The chapter that was open at the stop is IN the file, not dropped: E-C12. Counting it against
    // the file that ended the same chapter normally is what makes that a claim about the content
    // rather than about the container being parseable.
    CHECK(mid.chapters == whole.chapters);
}

TEST_CASE("a tail truncated by seven bytes is refused") {
    ScratchDir dir(L"trunc");
    wchar_t path[MAX_PATH];
    dir.File(L"trunc.echodemo", path);
    WriteCapture(path, 3, true);

    ReopenVerdict before{};
    VerifyClosedFile(path, &before);
    REQUIRE(before.verified);   // the damage below is the only difference between the two verdicts

    TruncateBy(path, 7);
    ReopenVerdict after{};
    VerifyClosedFile(path, &after);
    INFO("detail: ", after.detail);
    CHECK_FALSE(after.verified);
    CHECK(after.detail[0] != 0);
    CHECK(after.bytes == before.bytes - 7);
}

TEST_CASE("a capture cut off well before its footer is refused") {
    ScratchDir dir(L"half");
    wchar_t path[MAX_PATH];
    dir.File(L"half.echodemo", path);
    WriteCapture(path, 4, true);

    TruncateBy(path, FileSize(path) / 3);
    ReopenVerdict v{};
    VerifyClosedFile(path, &v);
    INFO("detail: ", v.detail);
    CHECK_FALSE(v.verified);
}

TEST_CASE("a file that is not there is refused, and says so") {
    ScratchDir dir(L"absent");
    wchar_t path[MAX_PATH];
    dir.File(L"never-written.echodemo", path);

    ReopenVerdict v{};
    VerifyClosedFile(path, &v);
    CHECK_FALSE(v.verified);
    CHECK(v.bytes == 0);
    CHECK(v.detail[0] != 0);

    // Nothing was recording at all: a different answer from a file that is damaged, and the sentinel
    // has to be able to tell them apart.
    ReopenVerdict none{};
    VerifyClosedFile(L"", &none);
    CHECK_FALSE(none.verified);
    CHECK(std::string(none.detail).find("nothing to re-open") != std::string::npos);
}

// `refused` is a count and `refusedBy` names the arm; neither carries the
// numbers behind that arm, so a reader cannot tell a slot we never got to ask about from one the
// engine itself would refuse. The FIRST refusal of the capture is published whole -- and ONLY when
// something refused, because a present block of zeros would be read as a measurement while an
// absent key can only mean nothing refused.
TEST_CASE("the stop line publishes the first body refusal whole, and only when one happened") {
    ScratchDir dir(L"firstrefusal");
    wchar_t path[MAX_PATH];
    dir.File(L"firstrefusal.echodemo", path);
    WriteCapture(path, 1, true);

    StopFacts f = FactsFor(0x01DC000000000003ull, 4242, path);
    f.reason = StopReason::MatchEnd;
    f.bodiesSampled = 17;
    f.bodiesRefused = 2;
    f.bodyRefusals[unsigned(replay::BodyRefusal::NoPrimaryBody)] = 2;
    f.bodyFirstRefusalSeen = true;
    f.bodyFirstRefusal.source = replay::BodySource::None;
    f.bodyFirstRefusal.refusedAt = replay::BodyRefusal::NoPrimaryBody;
    f.bodyFirstRefusal.diag.filled = replay::kBodyDiagSlot | replay::kBodyDiagLiveCount |
                                     replay::kBodyDiagBypass | replay::kBodyDiagCiEntity |
                                     replay::kBodyDiagCiIndex | replay::kBodyDiagRevHandle;
    f.bodyFirstRefusal.diag.slot = 11;
    f.bodyFirstRefusal.diag.liveCount = 5;
    f.bodyFirstRefusal.diag.bypass = replay::BypassRead::Unreadable;
    f.bodyFirstRefusal.diag.ciEntityId = 7;
    f.bodyFirstRefusal.diag.ciIndex = 9;
    f.bodyFirstRefusal.diag.revHandle = 12;
    VerifyClosedFile(path, &f.verdict);

    char err[192] = {};
    REQUIRE(WriteStopSentinel(dir.Path(), f, err, sizeof(err)));
    wchar_t name[64];
    REQUIRE(StopSentinelNameFor(f.launchFileTime, f.pid, name, 64));
    wchar_t sentinel[MAX_PATH];
    dir.File(name, sentinel);
    const std::string doc = ReadAll(sentinel);
    REQUIRE(!doc.empty());

    // VERBATIM against the ENGINE'S OWN DESCRIBER run on the same planted diag -- not against a
    // transcription of W1-E's format, which this suite pinned and went red for without a capture
    // behaviour having changed. The claim is the sentinel WRITER's: the gate name, the source word,
    // and the describer's line reaching the document whole.
    char expectText[replay::kBodyDiagTextMax];
    const std::string expectDiag(
        replay::DescribeDiag(f.bodyFirstRefusal.diag, expectText, sizeof expectText));
    // The control: an empty or truncated describer would make the comparison below pass against an
    // empty document field, so the expectation is proven to carry the planted values first.
    REQUIRE(expectDiag.find("slot=11") != std::string::npos);
    REQUIRE(expectDiag.find("liveCount=5") != std::string::npos);
    REQUIRE(expectDiag.find("bypass=unreadable") != std::string::npos);
    REQUIRE(expectDiag.find("bodyId=?") != std::string::npos);
    const std::string expectRefusal = std::string("\"firstRefusal\": { \"gate\": \"noPrimaryBody\", "
                                                  "\"source\": \"none\", \"diag\": \"") +
                                      expectDiag + "\" }";
    CHECK(Has(doc, expectRefusal.c_str()));

    // The same close with nothing refused: the key is ABSENT, and its absence is the statement.
    StopFacts clean = f;
    clean.pid = 4243;
    clean.bodiesRefused = 0;
    clean.bodyRefusals[unsigned(replay::BodyRefusal::NoPrimaryBody)] = 0;
    clean.bodyFirstRefusalSeen = false;
    clean.bodyFirstRefusal = replay::BodyReading{};
    REQUIRE(WriteStopSentinel(dir.Path(), clean, err, sizeof(err)));
    wchar_t cleanName[64];
    REQUIRE(StopSentinelNameFor(clean.launchFileTime, clean.pid, cleanName, 64));
    wchar_t cleanSentinel[MAX_PATH];
    dir.File(cleanName, cleanSentinel);
    const std::string cleanDoc = ReadAll(cleanSentinel);
    REQUIRE(!cleanDoc.empty());
    CHECK_FALSE(Has(cleanDoc, "firstRefusal"));
    CHECK(Has(cleanDoc, "\"refused\": 0"));

    // The in-process snapshot carries the same reading the document does -- one fact, two readers.
    StopFactsSnapshot snap{};
    REQUIRE(LastStopFacts(snap));
    CHECK_FALSE(snap.bodyFirstRefusalSeen);
}

// L5 run 3 closed a real 3287-record capture (echoreplay.log:377) and the harness
// recorded CAPTURE-NOT-ASKED, because the ordinary close -- the match ended, nobody asked for a stop
// -- wrote no sentinel at all. Capture is always on, so that close IS how a capture normally
// finishes. It now carries its own reason, and the registration accounting with it: a harness that
// never opens the file can still say how many of the capture's surfaces were live when it closed.
TEST_CASE("a match-end close writes a sentinel of its own, carrying the registration accounting") {
    ScratchDir dir(L"matchend");
    wchar_t path[MAX_PATH];
    dir.File(L"matchend.echodemo", path);
    WriteCapture(path, 2, true);

    StopFacts f = FactsFor(0x01DC000000000002ull, 909, path);
    f.records = 3287;   // L5 run 3's own count
    f.chapters = 2;
    f.reason = StopReason::MatchEnd;
    f.matchStillLive = false;
    f.taps.registered = 36;
    f.taps.deferred = 0;
    f.taps.givenUp = 1;
    f.taps.unavailable = 0;
    f.taps.excluded = 1;
    f.taps.total = 37;
    f.taps.tableRows = 38;
    f.taps.started = true;
    // L2's own marker numbers -- the watch table was never built, so nine names were
    // watched by the type and none was ever seen while 4400 rows went past. That combination is the
    // one the harness must be able to read off the document.
    f.markers.namesWatched = 9;
    f.markers.namesSeenMask = 0;
    f.markers.rowsSeen = 4400;
    f.markers.emitted = 0;
    f.markers.timeRefused = 3;
    // L3-RUN-13 F3. A close the capture SURVIVED, carrying the tail rung's own accounting -- the
    // reading that used to exist only inside a kill line. RUN-12's window is the shape: the rung
    // was asked on every window and read over its line on every one of them, and the amount term
    // is the only reason this file is still open. `chargedMeanUs` is derived one microsecond under
    // the line it is judged against, so the fixture cannot pass by sitting nowhere near it.
    f.costTail.measured = 8;
    f.costTail.notRun = 0;
    f.costTail.overBudget = f.costTail.measured;
    f.costTail.kills = 0;
    f.costTail.chargedMeanUs = replay::capture::CostMeter::kKillTailMeanUs - 1;
    VerifyClosedFile(path, &f.verdict);

    char err[192] = {};
    REQUIRE(WriteStopSentinel(dir.Path(), f, err, sizeof(err)));
    wchar_t name[64];
    REQUIRE(StopSentinelNameFor(f.launchFileTime, f.pid, name, 64));
    wchar_t sentinel[MAX_PATH];
    dir.File(name, sentinel);
    const std::string doc = ReadAll(sentinel);
    REQUIRE(!doc.empty());

    // The word the reader gets. "match-end" is its own answer: a capture that closed because the
    // match ended is not a capture nobody asked for.
    CHECK(Has(doc, "\"reason\": \"match-end\""));

    // The cost block, on a document no kill line accompanies. Five counters: a rung that ran
    // (`measured`), one that could not be asked (`notRun`), one that read over its line
    // (`overBudget`), and one that met the whole kill condition (`kills`), beside the mean the
    // amount term is read against. Asserted on the SURVIVOR because that is the run L3-RUN-13
    // could not grade either way.
    CHECK(RawField(doc, "measured") == "8");
    CHECK(RawField(doc, "notRun") == "0");
    CHECK(RawField(doc, "overBudget") == "8");
    CHECK(RawField(doc, "kills") == "0");
    CHECK(RawField(doc, "chargedMeanUs") ==
          std::to_string(replay::capture::CostMeter::kKillTailMeanUs - 1));
    CHECK(std::string(StopReasonName(StopReason::MatchEnd)) == "match-end");
    CHECK(Has(doc, "\"records\": 3287"));
    CHECK(Has(doc, "\"matchStillLive\": false"));
    // The denominator. Present, and it closes: without it "the file holds no events"
    // cannot be told from "no tap was ever registered".
    CHECK(Has(doc, "\"registered\": 36"));
    CHECK(Has(doc, "\"deferred\": 0"));
    // ★ The same argument one level down: "this match had no rounds" and "the netvar
    // symbols we watch for are not this build's" are different faults, and until now only the log
    // could tell them apart. namesSeen is DERIVED from the mask here, so the two cannot disagree.
    CHECK(Has(doc, "\"namesWatched\": 9"));
    CHECK(Has(doc, "\"namesSeen\": 0"));
    CHECK(Has(doc, "\"rowsSeen\": 4400"));
    CHECK(Has(doc, "\"timeRefused\": 3"));
    {
        // The healthy shape of the same fault, so the zero above is not the only value the field can
        // hold: three of the nine names arrived. A count that ignored the mask would read 0 here.
        StopFacts seen = f;
        seen.markers.namesSeenMask = 0x105;   // bits 0, 2 and 8
        REQUIRE(WriteStopSentinel(dir.Path(), seen, err, sizeof(err)));
        const std::string doc2 = ReadAll(sentinel);
        CHECK(Has(doc2, "\"namesSeen\": 3"));
        CHECK(Has(doc2, "\"namesSeenMask\": 261"));
    }
    CHECK(Has(doc, "\"givenUp\": 1"));
    CHECK(Has(doc, "\"unavailable\": 0"));
    CHECK(Has(doc, "\"excluded\": 1"));
    CHECK(Has(doc, "\"total\": 37"));
    CHECK(Has(doc, "\"tableRows\": 38"));
    CHECK(Has(doc, "\"started\": true"));
    CHECK(f.taps.registered + f.taps.deferred + f.taps.givenUp + f.taps.unavailable == f.taps.total);
    CHECK(f.taps.total + f.taps.excluded == f.taps.tableRows);

    // And every reason the enum can carry has a word of its own: a stop that cannot name why it
    // happened must not be reported as one that can.
    CHECK(std::string(StopReasonName(StopReason::Deadline)) == "deadline");
    CHECK(std::string(StopReasonName(StopReason::Request)) == "request");
    CHECK(std::string(StopReasonName(StopReason::Shutdown)) == "shutdown");
    CHECK(std::string(StopReasonName(StopReason::CostKill)) == "cost-kill");
}

// ★ `closeRequested` has two setters and the close used to re-derive the cause, so the
// cost meter's rung-5 kill wrote `match-end` for a match that was still being played. The word is the
// finding; the second-order half is that the same close silently stopped a reopen, which is a change
// to always-on capture that nothing in the artefact said out loud.
TEST_CASE("a cost kill names the cost meter and declares what it suppressed") {
    ScratchDir dir(L"costkill");
    wchar_t path[MAX_PATH];
    dir.File(L"killed.echodemo", path);
    WriteCapture(path, 2, true);

    StopFacts f = FactsFor(0x01DC000000000101ull, 909, path);
    f.reason = StopReason::CostKill;
    f.closeSeq = 1;
    f.matchStillLive = true;    // the kill rung fires DURING a match; that is the whole finding
    f.reopenSuppressed = true;
    VerifyClosedFile(path, &f.verdict);

    char err[192] = {};
    REQUIRE(WriteStopSentinelForClose(dir.Path(), f, err, sizeof(err)));
    wchar_t name[64];
    REQUIRE(StopSentinelNameForClose(f.launchFileTime, f.pid, f.closeSeq, name, 64));
    wchar_t sentinel[MAX_PATH];
    dir.File(name, sentinel);
    const std::string doc = ReadAll(sentinel);
    REQUIRE(!doc.empty());

    CHECK(Has(doc, "\"reason\": \"cost-kill\""));
    CHECK_FALSE(Has(doc, "\"reason\": \"match-end\""));
    CHECK(Has(doc, "\"matchStillLive\": true"));
    // Not inferable from matchStillLive: a match-end close can also find the match live. The document
    // states it.
    CHECK(Has(doc, "\"reopenSuppressed\": true"));
    CHECK(Has(doc, "\"closeSeq\": 1"));

    // Every word the enum carries is distinct: a borrowed word is exactly the defect above.
    const StopReason all[] = {StopReason::Deadline, StopReason::Request, StopReason::Shutdown,
                              StopReason::MatchEnd, StopReason::CostKill};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        CHECK(std::string(StopReasonName(all[i])) != "unknown");
        for (size_t k = i + 1; k < sizeof(all) / sizeof(all[0]); ++k) {
            CHECK(std::string(StopReasonName(all[i])) != std::string(StopReasonName(all[k])));
        }
    }
}

// ★ Retroactively against the artefact it was written for. L5 run 3's own log closed a
// 3287-record file at 12:57:43.987 (`echoreplay.log:377`) and re-opened 47 ms later at 12:57:44.034
// (`:379`), so a launch that runs past a match closes MORE THAN ONCE -- and under one launch-scoped
// name the second close overwrote the first, leaving the match that was actually captured with no
// sentinel. Two closes, two sentinels, both still on disk, each naming its own file and reason.
TEST_CASE("an earlier close's sentinel survives every later close") {
    ScratchDir dir(L"twocloses");
    wchar_t matchPath[MAX_PATH];
    wchar_t stubPath[MAX_PATH];
    dir.File(L"the-match.echodemo", matchPath);
    dir.File(L"the-stub.echodemo", stubPath);
    WriteCapture(matchPath, 3, true);
    WriteCapture(stubPath, 1, true);

    const uint64_t launch = 0x01DC000000000202ull;
    const uint32_t pid = 1717;

    StopFacts first = FactsFor(launch, pid, matchPath);
    first.reason = StopReason::MatchEnd;
    first.closeSeq = 1;
    first.records = 3287;
    VerifyClosedFile(matchPath, &first.verdict);

    StopFacts second = FactsFor(launch, pid, stubPath);
    second.reason = StopReason::Shutdown;
    second.closeSeq = 2;
    second.records = 4;
    VerifyClosedFile(stubPath, &second.verdict);

    char err[192] = {};
    REQUIRE(WriteStopSentinelForClose(dir.Path(), first, err, sizeof(err)));
    REQUIRE(WriteStopSentinelForClose(dir.Path(), second, err, sizeof(err)));

    wchar_t firstName[64], secondName[64];
    REQUIRE(StopSentinelNameForClose(launch, pid, 1, firstName, 64));
    REQUIRE(StopSentinelNameForClose(launch, pid, 2, secondName, 64));
    CHECK(std::wstring(firstName) != std::wstring(secondName));

    wchar_t firstPath[MAX_PATH], secondPath[MAX_PATH];
    dir.File(firstName, firstPath);
    dir.File(secondName, secondPath);
    const std::string a = ReadAll(firstPath);
    const std::string b = ReadAll(secondPath);
    REQUIRE(!a.empty());
    REQUIRE(!b.empty());

    // Each names ITS file -- the field the harness correlates on -- and its own reason and ordinal.
    CHECK(Has(a, "the-match.echodemo"));
    CHECK(Has(a, "\"reason\": \"match-end\""));
    CHECK(Has(a, "\"records\": 3287"));
    CHECK(Has(a, "\"closeSeq\": 1"));
    CHECK(Has(b, "the-stub.echodemo"));
    CHECK(Has(b, "\"reason\": \"shutdown\""));
    CHECK(Has(b, "\"closeSeq\": 2"));
    CHECK_FALSE(Has(b, "the-match.echodemo"));

    // The archive accumulates on the same rule: it is the copy collected into the run directory.
    REQUIRE(ArchiveStopSentinel(dir.Path(), first, err, sizeof(err)));
    REQUIRE(ArchiveStopSentinel(dir.Path(), second, err, sizeof(err)));
    wchar_t archived[MAX_PATH];
    _snwprintf_s(archived, MAX_PATH, _TRUNCATE, L"%s\\%s\\%s", dir.Path(), kStopArchiveDirName,
                 firstName);
    CHECK(Has(ReadAll(archived), "the-match.echodemo"));

    // The launch-scoped name stays a POINTER at the latest close for a reader that polls one path --
    // and it says which close it is, so nobody mistakes it for the launch's only sentinel.
    REQUIRE(WriteStopSentinel(dir.Path(), first, err, sizeof(err)));
    REQUIRE(WriteStopSentinel(dir.Path(), second, err, sizeof(err)));
    wchar_t pointerName[64], pointerPath[MAX_PATH];
    REQUIRE(StopSentinelNameFor(launch, pid, pointerName, 64));
    dir.File(pointerName, pointerPath);
    const std::string pointer = ReadAll(pointerPath);
    CHECK(Has(pointer, "\"closeSeq\": 2"));
    CHECK(Has(pointer, "the-stub.echodemo"));
    // ...and the earlier close's own document is STILL what it was. This is the invariant.
    CHECK(ReadAll(firstPath) == a);
}

// The refusal is the mechanism, not a nicety: without it the invariant above holds only while the
// ordinal happens to be unique.
TEST_CASE("a per-close sentinel is refused, never replaced, when its name is already on disk") {
    ScratchDir dir(L"noreplace");
    wchar_t path[MAX_PATH];
    dir.File(L"once.echodemo", path);
    WriteCapture(path, 2, true);

    StopFacts f = FactsFor(0x01DC000000000303ull, 55, path);
    f.reason = StopReason::MatchEnd;
    f.closeSeq = 7;
    f.records = 11;
    VerifyClosedFile(path, &f.verdict);

    char err[192] = {};
    REQUIRE(WriteStopSentinelForClose(dir.Path(), f, err, sizeof(err)));
    wchar_t name[64], full[MAX_PATH];
    REQUIRE(StopSentinelNameForClose(f.launchFileTime, f.pid, f.closeSeq, name, 64));
    // The grammar W1-J reads: prefix, launch FILETIME as 16 upper-case hex, pid, close ordinal in
    // three digits, .json.
    CHECK(std::wstring(name) == L"capture_stop.01DC000000000303.55.007.json");
    dir.File(name, full);
    const std::string original = ReadAll(full);
    REQUIRE(!original.empty());

    StopFacts impostor = f;
    impostor.records = 99999;
    CHECK_FALSE(WriteStopSentinelForClose(dir.Path(), impostor, err, sizeof(err)));
    CHECK(err[0] != 0);   // and it says which file it would not overwrite
    CHECK(std::string(err).find("already") != std::string::npos);
    CHECK(ReadAll(full) == original);
    CHECK_FALSE(Has(ReadAll(full), "99999"));

    // The temporary it wrote on the way is not left behind for the Init sweep to puzzle over.
    wchar_t temp[MAX_PATH];
    _snwprintf_s(temp, MAX_PATH, _TRUNCATE, L"%s.tmp", full);
    CHECK(::GetFileAttributesW(temp) == INVALID_FILE_ATTRIBUTES);

    // The launch-scoped pointer, by contrast, is MEANT to be replaced.
    REQUIRE(WriteStopSentinel(dir.Path(), f, err, sizeof(err)));
    REQUIRE(WriteStopSentinel(dir.Path(), impostor, err, sizeof(err)));
    wchar_t pointerName[64], pointerPath[MAX_PATH];
    REQUIRE(StopSentinelNameFor(f.launchFileTime, f.pid, pointerName, 64));
    dir.File(pointerName, pointerPath);
    CHECK(Has(ReadAll(pointerPath), "99999"));

    // A name that cannot be built is not a name that is written.
    wchar_t tiny[8];
    CHECK_FALSE(StopSentinelNameForClose(f.launchFileTime, f.pid, 1, tiny, 8));
}

TEST_CASE("the sentinel carries the reader's verdict, not the writer's intention") {
    ScratchDir dir(L"sentinel");
    wchar_t path[MAX_PATH];
    dir.File(L"real.echodemo", path);
    WriteCapture(path, 2, true);

    StopFacts f = FactsFor(0x01DC000000000001ull, 4321, path);
    f.records = 1234;
    f.chapters = 2;
    f.matchStillLive = true;
    VerifyClosedFile(path, &f.verdict);
    REQUIRE(f.verdict.verified);

    char err[192] = {};
    REQUIRE(WriteStopSentinel(dir.Path(), f, err, sizeof(err)));
    CHECK(err[0] == 0);

    wchar_t name[64];
    REQUIRE(StopSentinelNameFor(f.launchFileTime, f.pid, name, 64));
    CHECK(std::wstring(name) == L"capture_stop.01DC000000000001.4321.json");
    wchar_t sentinel[MAX_PATH];
    dir.File(name, sentinel);
    const std::string doc = ReadAll(sentinel);
    REQUIRE(!doc.empty());
    // D-4: the launch is named in the document as well as in the file name, so a consumer that copied
    // the file somewhere still knows whose stop it is describing.
    CHECK(Has(doc, "\"launchFileTime\": \"01DC000000000001\""));
    CHECK(Has(doc, "\"pid\": 4321"));
    // startedAt is launchFileTime in the readable form. The expected value comes from outside this build
    // (`[datetime]::FromFileTimeUtc(0x01DC000000000001)`), not from the same API the writer used, so a
    // stamp derived from the wrong end of the FILETIME cannot agree with it.
    CHECK(Has(doc, "\"startedAt\": \"2025-07-28T20:41:31Z\""));
    CHECK(Has(doc, "\"verified\": true"));
    CHECK(Has(doc, "\"reason\": \"deadline\""));
    CHECK(Has(doc, "\"records\": 1234"));
    CHECK(Has(doc, "\"matchStillLive\": true"));
    CHECK(Has(doc, "\"chapters\": 2"));
    // bytes is the size on disk, and L2's expectation line reads it: a zero here is a stop that
    // produced nothing while reporting a file.
    char bytes[64];
    _snprintf_s(bytes, sizeof(bytes), _TRUNCATE, "\"bytes\": %llu",
                static_cast<unsigned long long>(FileSize(path)));
    CHECK(Has(doc, bytes));
    // The path is JSON, so every separator in it is doubled. An unescaped `C:\Users` makes the
    // document unparseable at exactly the machine that has to read it.
    CHECK(Has(doc, "\\\\"));
    CHECK_FALSE(Has(doc, ":\\r"));
}

TEST_CASE("a damaged capture writes a sentinel that says verified false") {
    ScratchDir dir(L"bad");
    wchar_t path[MAX_PATH];
    dir.File(L"bad.echodemo", path);
    WriteCapture(path, 3, true);
    TruncateBy(path, 7);

    StopFacts f = FactsFor(0x01DC000000000002ull, 777, path);
    f.records = 99;
    f.chapters = 3;
    f.reason = StopReason::Request;
    VerifyClosedFile(path, &f.verdict);
    REQUIRE_FALSE(f.verdict.verified);

    char err[192] = {};
    REQUIRE(WriteStopSentinel(dir.Path(), f, err, sizeof(err)));

    wchar_t name[64];
    REQUIRE(StopSentinelNameFor(f.launchFileTime, f.pid, name, 64));
    wchar_t sentinel[MAX_PATH];
    dir.File(name, sentinel);
    const std::string doc = ReadAll(sentinel);
    CHECK(Has(doc, "\"verified\": false"));
    CHECK(Has(doc, "\"reason\": \"request\""));
    // The writer's own count is kept beside the reader's. A stop that lost chapters must not be able
    // to report the number it meant to write as the number that is there.
    CHECK(Has(doc, "\"writerChapters\": 3"));
    CHECK(Has(doc, "\"verifyDetail\": \""));
    CHECK_FALSE(Has(doc, "\"verifyDetail\": \"\""));

    // The sentinel is written into place, not appended to: a second stop must not leave two documents
    // concatenated in one file.
    REQUIRE(WriteStopSentinel(dir.Path(), f, err, sizeof(err)));
    const std::string again = ReadAll(sentinel);
    CHECK(again.size() == doc.size());
}

TEST_CASE("the stop request is a positive, fresh signal") {
    ScratchDir dir(L"request");
    FILETIME now{};
    ::GetSystemTimeAsFileTime(&now);
    const uint64_t launch =
        (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;

    CHECK_FALSE(StopRequestPresent(dir.Path(), launch));
    CHECK_FALSE(StopRequestPresent(L"", launch));
    CHECK_FALSE(StopRequestPresent(nullptr, launch));

    wchar_t request[MAX_PATH];
    dir.File(kStopRequestName, request);
    HANDLE h = ::CreateFileW(request, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    ::CloseHandle(h);
    CHECK(StopRequestPresent(dir.Path(), launch));

    // RK-11's hazard in a second carrier: a stop-capture left behind by an earlier run would
    // otherwise close the capture of every launch after it, in the first second, silently.
    const uint64_t hourFromNow = launch + 36000000000ull;
    CHECK_FALSE(StopRequestPresent(dir.Path(), hourFromNow));
    ::DeleteFileW(request);
}

TEST_CASE("the sentinel goes beside the capture it describes") {
    wchar_t out[MAX_PATH] = {};
    CHECK(DirectoryOfFile(L"D:\\data\\replays\\a.echodemo", out, MAX_PATH));
    CHECK(std::wstring(out) == L"D:\\data\\replays");
    CHECK(DirectoryOfFile(L"D:/data/replays/a.echodemo", out, MAX_PATH));
    CHECK(std::wstring(out) == L"D:/data/replays");
    CHECK_FALSE(DirectoryOfFile(L"a.echodemo", out, MAX_PATH));
    CHECK_FALSE(DirectoryOfFile(L"", out, MAX_PATH));
    CHECK_FALSE(DirectoryOfFile(nullptr, out, MAX_PATH));
    CHECK_FALSE(DirectoryOfFile(L"D:\\data\\replays\\a.echodemo", out, 4));

    CHECK(std::string(StopReasonName(StopReason::Deadline)) == "deadline");
    CHECK(std::string(StopReasonName(StopReason::Request)) == "request");
    CHECK(std::string(StopReasonName(StopReason::Shutdown)) == "shutdown");
}

TEST_CASE("a sentinel that cannot be written is a refusal, not a silence") {
    StopFacts f{};
    f.file = L"D:\\nowhere\\x.echodemo";
    char err[192] = {};
    CHECK_FALSE(WriteStopSentinel(L"", f, err, sizeof(err)));
    CHECK(err[0] != 0);

    char err2[192] = {};
    CHECK_FALSE(WriteStopSentinel(L"Z:\\no-such-volume\\nowhere", f, err2, sizeof(err2)));
    CHECK(err2[0] != 0);
}

// ---------------------------------------------------------------------------------------------
// AUDIT_D_SC7 D-4. The stop directory is shared by every launch, so a sentinel with no owner is a
// document about the PREVIOUS launch that reads as this one's. These two cases are the pair that
// matters: the sweep removes what an earlier launch left, and a launch that is killed before its stop
// leaves NOTHING behind rather than something stale.
// ---------------------------------------------------------------------------------------------

TEST_CASE("a second launch in the same directory sweeps the first launch's sentinel away") {
    ScratchDir dir(L"twolaunch");
    wchar_t path[MAX_PATH];
    dir.File(L"first.echodemo", path);
    WriteCapture(path, 2, true);

    // Launch one runs to its stop and leaves a good sentinel.
    const uint64_t launchOne = 0x01DC000000000010ull;
    const uint64_t launchTwo = 0x01DC000000000020ull;
    StopFacts one = FactsFor(launchOne, 100, path);
    VerifyClosedFile(path, &one.verdict);
    REQUIRE(one.verdict.verified);
    char err[192] = {};
    REQUIRE(WriteStopSentinel(dir.Path(), one, err, sizeof(err)));
    REQUIRE(SentinelExists(dir.Path(), launchOne, 100));
    // The old fixed name is swept too: a tree that ran the previous build left one of these.
    wchar_t legacy[MAX_PATH];
    dir.File(L"capture_stop.json", legacy);
    HANDLE h = ::CreateFileW(legacy, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    ::CloseHandle(h);
    REQUIRE(CountSentinels(dir.Path()) == 2);

    // Launch two's Init.
    uint32_t failed = 99;
    const uint32_t swept = DeleteStaleStopSentinels(dir.Path(), &failed);
    CHECK(swept == 2);
    CHECK(failed == 0);
    CHECK(CountSentinels(dir.Path()) == 0);
    CHECK_FALSE(SentinelExists(dir.Path(), launchOne, 100));

    // ...and its own stop writes a document nobody can confuse with launch one's.
    StopFacts two = FactsFor(launchTwo, 200, path);
    VerifyClosedFile(path, &two.verdict);
    REQUIRE(WriteStopSentinel(dir.Path(), two, err, sizeof(err)));
    CHECK(SentinelExists(dir.Path(), launchTwo, 200));
    CHECK(CountSentinels(dir.Path()) == 1);
}

TEST_CASE("a launch killed before its stop leaves no sentinel at all") {
    ScratchDir dir(L"killed");
    wchar_t path[MAX_PATH];
    dir.File(L"prev.echodemo", path);
    WriteCapture(path, 2, true);

    const uint64_t previous = 0x01DC000000000030ull;
    StopFacts done = FactsFor(previous, 300, path);
    VerifyClosedFile(path, &done.verdict);
    REQUIRE(done.verdict.verified);
    char err[192] = {};
    REQUIRE(WriteStopSentinel(dir.Path(), done, err, sizeof(err)));

    // The next launch inits (sweep) and is then killed: no WriteStopSentinel ever runs. What is left
    // in the directory is the whole evidence a reader has, and it must be nothing -- not the previous
    // launch's `verified:true`, which is exactly what CLEAN would be read off.
    DeleteStaleStopSentinels(dir.Path(), nullptr);
    CHECK(CountSentinels(dir.Path()) == 0);
    CHECK_FALSE(SentinelExists(dir.Path(), previous, 300));
    CHECK_FALSE(SentinelExists(dir.Path(), 0x01DC000000000040ull, 400)); // facts-ok: AUDIO.LISTENER_COUNT_OFF a process id, not that offset
}

TEST_CASE("the launch id is this process, and two launches cannot share one") {
    const uint64_t id = LaunchFileTime();
    CHECK(id != 0);
    CHECK(id == LaunchFileTime());   // an identity that changed per call would name nothing

    FILETIME now{};
    ::GetSystemTimeAsFileTime(&now);
    const uint64_t nowFt = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    // The process started before now and not in the last microsecond of the epoch: this is the process
    // CREATION time, not a constant and not a stamp taken here.
    CHECK(id <= nowFt);
    CHECK(id > 130000000000000000ull);   // 2013; a zeroed or garbage FILETIME fails this

    wchar_t name[64];
    REQUIRE(StopSentinelNameFor(id, ::GetCurrentProcessId(), name, 64));
    CHECK(std::wstring(name).find(L"capture_stop.") == 0);
    CHECK(std::wstring(name).find(L".json") != std::wstring::npos);
    CHECK_FALSE(StopSentinelNameFor(id, 1, name, 8));
}

// ---------------------------------------------------------------------------------------------
// ★ THE PLAN'S READER RULE, MADE EXECUTABLE (the design notes §5.4; AUDIT_G G-7).
//
// W1-J's harness half does not exist yet, so the five outcomes the plan freezes have nowhere to be
// exercised and nothing keeps the writer from making one of them unreachable. `ClassifyLaunch` is
// that rule, in the order the plan states it, and every case below drives it with a document the
// SHIPPED writer produced rather than a literal -- a field the writer stops emitting, or emits under
// another name, turns a case red here instead of at L2.
//
// Its input is the LOGGED launch directory and never the archive copy: the archive outlives the Init
// sweep on purpose, and reading it would resurrect exactly the stale-document read D-4 closed.
// ---------------------------------------------------------------------------------------------
namespace {

// §9 rule 4: a bare `closed:` prefix is refused. The record-count form is the one the stop path
// writes; capture.cpp's empty-capture line begins with the same eight characters and means the
// opposite.
bool IsRecordCountClosedLine(const char* line) {
    if (!line) {
        return false;
    }
    const std::string s(line);
    return s.compare(0, 8, "closed: ") == 0 && s.find(" records, ") != std::string::npos &&
           s.find(" chapters, ") != std::string::npos;
}

std::string ClassifyLaunch(const wchar_t* dir, uint64_t launchFileTime, uint32_t pid,
                           const char* closedLine, std::string* detail) {
    if (detail) {
        detail->clear();
    }
    char stamp[32];
    _snprintf_s(stamp, sizeof(stamp), _TRUNCATE, "%016llX",
                static_cast<unsigned long long>(launchFileTime));
    char pidText[32];
    _snprintf_s(pidText, sizeof(pidText), _TRUNCATE, "%lu", static_cast<unsigned long>(pid));

    wchar_t pattern[MAX_PATH];
    _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\%s%s", dir, kStopSentinelPrefix, L"*.json");
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(pattern, &fd);
    std::string mine;
    bool sawForeign = false;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                continue;
            }
            wchar_t path[MAX_PATH];
            _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);
            const std::string doc = ReadAll(path);
            if (RawField(doc, "launchFileTime") == stamp && RawField(doc, "pid") == pidText) {
                mine = doc;
            } else {
                sawForeign = true;
            }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }

    if (mine.empty()) {
        // A document whose stamp does not match -- including one that carries no stamp at all, which
        // is what a reader written against this rule sees if the writer renames the field -- is a
        // loud finding, never a quiet "nothing finalised".
        return sawForeign ? "CAPTURE-STALE-SENTINEL" : "CAPTURE-UNFINALIZED";
    }
    const std::string verified = RawField(mine, "verified");
    if (verified != "true") {
        if (detail) {
            *detail = verified.empty() ? std::string("the document does not state 'verified'")
                                       : RawField(mine, "verifyDetail");
        }
        return "CAPTURE-UNVERIFIED";
    }
    // D-6: the PAIR. `matchStillLive` alone is a real file whose match was still running.
    if (RawField(mine, "reason") == "deadline" && RawField(mine, "matchStillLive") == "true") {
        return "CAPTURE-PARTIAL";
    }
    const bool bytes = !RawField(mine, "bytes").empty() && RawField(mine, "bytes") != "0";
    const bool chapters = !RawField(mine, "chapters").empty() && RawField(mine, "chapters") != "0";
    if (bytes && chapters && IsRecordCountClosedLine(closedLine)) {
        return "CAPTURE-CLEAN";
    }
    if (detail) {
        *detail = "verified, but the CLEAN conditions are not all met";
    }
    return "CAPTURE-UNFINALIZED";
}

// The two log lines the classifier is fed, copied from capture.cpp's own format strings with the
// runtime values filled in.
const char* kRecordCountClosed = "closed: 1234 records, 2 chapters, 1 keyframes, 0 verbatim, 0 refusals";
const char* kEmptyCaptureClosed =
    "closed: nothing was recording when the 300 s capture deadline was reached";

// A good capture, closed and verified, as launch `id`/`pid` would leave it.
StopFacts GoodStop(const wchar_t* path, uint64_t id, uint32_t pid) {
    StopFacts f = FactsFor(id, pid, path);
    f.records = 1234;
    f.chapters = 2;
    f.reason = StopReason::Request;
    VerifyClosedFile(path, &f.verdict);
    return f;
}

}  // namespace

TEST_CASE("the document carries the plan's field names, and only those") {
    ScratchDir dir(L"fields");
    wchar_t path[MAX_PATH];
    dir.File(L"fields.echodemo", path);
    WriteCapture(path, 2, true);

    StopFacts f = GoodStop(path, 0x01DC000000000001ull, 4321);
    REQUIRE(f.verdict.verified);
    char err[192] = {};
    REQUIRE(WriteStopSentinel(dir.Path(), f, err, sizeof(err)));
    wchar_t name[64];
    REQUIRE(StopSentinelNameFor(f.launchFileTime, f.pid, name, 64));
    wchar_t sentinel[MAX_PATH];
    dir.File(name, sentinel);
    const std::string doc = ReadAll(sentinel);
    REQUIRE(!doc.empty());

    // The design notes freeze the stamp as `launchFileTime` + `pid`, in the name AND in the
    // document. W1-J's reader is being written against those names; anything else is absent to it.
    CHECK(RawField(doc, "launchFileTime") == "01DC000000000001");
    CHECK(RawField(doc, "pid") == "4321");
    CHECK_FALSE(Has(doc, "launchId"));
    CHECK(std::wstring(name) == L"capture_stop.01DC000000000001.4321.json");

    // The rest of the plan's vocabulary, each read the way the reader reads it.
    CHECK(RawField(doc, "verified") == "true");
    CHECK(RawField(doc, "reason") == "request");
    CHECK(RawField(doc, "matchStillLive") == "false");
    CHECK(RawField(doc, "chapters") == "2");
    CHECK(RawField(doc, "records") == "1234");
    CHECK_FALSE(RawField(doc, "bytes").empty());
    CHECK_FALSE(RawField(doc, "verifyDetail").empty());

    // ★ AUDIT_M6 F5 / F2. The two facts a reader cannot derive from anything else in the set: that
    // this close closed a file at all, and whether the launch could open what it tried to.
    CHECK(RawField(doc, "fileClosed") == "true");
    CHECK(RawField(doc, "openFailures") == "0");
    CHECK(RawField(doc, "openFailState") == "none");

    // ★ This document is the launch-scoped POINTER at the latest close, and it is the
    // one document that says so. The per-close document written from the same facts must not:
    // that field is what stops a reader that opened one of them counting it as a close of its own.
    CHECK(RawField(doc, "latestClosePointer") == "true");

    f.closeSeq = 7;
    REQUIRE(WriteStopSentinelForClose(dir.Path(), f, err, sizeof(err)));
    wchar_t perClose[64];
    REQUIRE(StopSentinelNameForClose(f.launchFileTime, f.pid, f.closeSeq, perClose, 64));
    wchar_t perClosePath[MAX_PATH];
    dir.File(perClose, perClosePath);
    const std::string evidence = ReadAll(perClosePath);
    REQUIRE(!evidence.empty());
    CHECK_FALSE(Has(evidence, "latestClosePointer"));
    CHECK(RawField(evidence, "closeSeq") == "7");
    CHECK(RawField(evidence, "fileClosed") == "true");
}

TEST_CASE("a close that closed no file says so, and names the open failures behind it") {
    ScratchDir dir(L"nofile");
    wchar_t path[MAX_PATH];
    dir.File(L"nofile.echodemo", path);
    WriteCapture(path, 2, true);

    // The deadline that met no recording: the document is still written -- the harness needs to know
    // the stop happened -- and `file` still names the launch's previous capture, which is exactly the
    // reading a `fileClosed: false` document has to rule out.
    StopFacts f = GoodStop(path, 0x01DC0000000000F1ull, 4242);
    f.fileClosed = false;
    f.openFailures = 6;
    f.openFailState = "writer-open";
    f.closeSeq = 3;
    char err[192] = {};
    REQUIRE(WriteStopSentinelForClose(dir.Path(), f, err, sizeof(err)));
    wchar_t name[64];
    REQUIRE(StopSentinelNameForClose(f.launchFileTime, f.pid, f.closeSeq, name, 64));
    wchar_t sentinel[MAX_PATH];
    dir.File(name, sentinel);
    const std::string doc = ReadAll(sentinel);
    REQUIRE(!doc.empty());
    CHECK(RawField(doc, "fileClosed") == "false");
    CHECK(RawField(doc, "openFailures") == "6");
    CHECK(RawField(doc, "openFailState") == "writer-open");
    // The same document still names the file it reported on, so the two facts are separable rather
    // than one being inferred from the other.
    CHECK_FALSE(RawField(doc, "file").empty());
}

TEST_CASE("the five outcomes of the plan's reader rule, each produced by a stop") {
    SUBCASE("CAPTURE-CLEAN") {
        ScratchDir dir(L"o_clean");
        wchar_t path[MAX_PATH];
        dir.File(L"clean.echodemo", path);
        WriteCapture(path, 2, true);
        StopFacts f = GoodStop(path, 0x01DC000000000101ull, 11);
        REQUIRE(f.verdict.verified);
        char err[192] = {};
        REQUIRE(WriteStopSentinel(dir.Path(), f, err, sizeof(err)));

        CHECK(ClassifyLaunch(dir.Path(), 0x01DC000000000101ull, 11, kRecordCountClosed, nullptr) ==
              "CAPTURE-CLEAN");
        // Retroactive control (plan item 5, second control): the SAME good document, read against
        // the line capture.cpp writes when nothing was recording, is not CLEAN.
        CHECK(ClassifyLaunch(dir.Path(), 0x01DC000000000101ull, 11, kEmptyCaptureClosed, nullptr) ==
              "CAPTURE-UNFINALIZED");
        // §9 rule 4: the bare prefix is not the record-count form.
        CHECK(ClassifyLaunch(dir.Path(), 0x01DC000000000101ull, 11, "closed:", nullptr) ==
              "CAPTURE-UNFINALIZED");
    }

    SUBCASE("CAPTURE-UNFINALIZED - killed before the stop") {
        ScratchDir dir(L"o_unfin");
        CHECK(ClassifyLaunch(dir.Path(), 0x01DC000000000102ull, 12, kRecordCountClosed, nullptr) ==
              "CAPTURE-UNFINALIZED");
    }

    SUBCASE("CAPTURE-PARTIAL - the deadline caught a live match") {
        ScratchDir dir(L"o_part");
        wchar_t path[MAX_PATH];
        dir.File(L"part.echodemo", path);
        WriteCapture(path, 2, true);
        StopFacts f = GoodStop(path, 0x01DC000000000103ull, 13);
        f.reason = StopReason::Deadline;
        f.matchStillLive = true;
        REQUIRE(f.verdict.verified);
        char err[192] = {};
        REQUIRE(WriteStopSentinel(dir.Path(), f, err, sizeof(err)));
        CHECK(ClassifyLaunch(dir.Path(), 0x01DC000000000103ull, 13, kRecordCountClosed, nullptr) ==
              "CAPTURE-PARTIAL");

        // D-6, the other half of the pair: a live match closed by a REQUEST is not partial.
        f.reason = StopReason::Request;
        REQUIRE(WriteStopSentinel(dir.Path(), f, err, sizeof(err)));
        CHECK(ClassifyLaunch(dir.Path(), 0x01DC000000000103ull, 13, kRecordCountClosed, nullptr) ==
              "CAPTURE-CLEAN");
    }

    SUBCASE("CAPTURE-UNVERIFIED - the file would not re-open") {
        ScratchDir dir(L"o_unver");
        wchar_t path[MAX_PATH];
        dir.File(L"unver.echodemo", path);
        WriteCapture(path, 3, true);
        TruncateBy(path, 7);
        StopFacts f = FactsFor(0x01DC000000000104ull, 14, path);
        f.records = 99;
        f.chapters = 3;
        VerifyClosedFile(path, &f.verdict);
        REQUIRE_FALSE(f.verdict.verified);
        char err[192] = {};
        REQUIRE(WriteStopSentinel(dir.Path(), f, err, sizeof(err)));

        std::string detail;
        CHECK(ClassifyLaunch(dir.Path(), 0x01DC000000000104ull, 14, kRecordCountClosed, &detail) ==
              "CAPTURE-UNVERIFIED");
        // It carries the reader's finding verbatim, and it never falls through to CLEAN even though
        // the record-count `closed:` line is present.
        CHECK_FALSE(detail.empty());
    }

    SUBCASE("CAPTURE-STALE-SENTINEL - a previous launch's verified:true left in place") {
        ScratchDir dir(L"o_stale");
        wchar_t path[MAX_PATH];
        dir.File(L"prev.echodemo", path);
        WriteCapture(path, 2, true);
        StopFacts prev = GoodStop(path, 0x01DC000000000105ull, 15);
        REQUIRE(prev.verdict.verified);
        char err[192] = {};
        REQUIRE(WriteStopSentinel(dir.Path(), prev, err, sizeof(err)));

        // Retroactive control (plan item 5, first control): this is the exact artefact that read as
        // CLEAN before D-4. THIS launch is 0x...106/16, and it gets a loud finding, not a pass and
        // not a quiet UNFINALIZED.
        CHECK(ClassifyLaunch(dir.Path(), 0x01DC000000000106ull, 16, kRecordCountClosed, nullptr) ==
              "CAPTURE-STALE-SENTINEL");
    }

    SUBCASE("CAPTURE-STALE-SENTINEL - a document written under the pre-G-6 field name") {
        ScratchDir dir(L"o_oldname");
        wchar_t path[MAX_PATH];
        dir.File(L"old.echodemo", path);
        WriteCapture(path, 2, true);
        StopFacts f = GoodStop(path, 0x01DC000000000107ull, 17);
        REQUIRE(f.verdict.verified);
        char err[192] = {};
        REQUIRE(WriteStopSentinel(dir.Path(), f, err, sizeof(err)));
        wchar_t name[64];
        REQUIRE(StopSentinelNameFor(f.launchFileTime, f.pid, name, 64));
        wchar_t sentinel[MAX_PATH];
        dir.File(name, sentinel);
        std::string doc = ReadAll(sentinel);
        const size_t at = doc.find("\"launchFileTime\"");
        REQUIRE(at != std::string::npos);
        doc.replace(at, strlen("\"launchFileTime\""), "\"launchId\"       ");
        HANDLE h = ::CreateFileW(sentinel, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        REQUIRE(h != INVALID_HANDLE_VALUE);
        DWORD wrote = 0;
        ::WriteFile(h, doc.data(), static_cast<DWORD>(doc.size()), &wrote, nullptr);
        ::CloseHandle(h);

        // G-6's whole point: the reader is written against `launchFileTime`, so the R2 document does
        // not state its origin at all. It must be a finding, never CLEAN.
        CHECK(ClassifyLaunch(dir.Path(), 0x01DC000000000107ull, 17, kRecordCountClosed, nullptr) ==
              "CAPTURE-STALE-SENTINEL");
    }
}

TEST_CASE("the archived copy survives the next launch's sweep, and is not the reader's input") {
    ScratchDir dir(L"archive");
    wchar_t path[MAX_PATH];
    dir.File(L"arch.echodemo", path);
    WriteCapture(path, 2, true);

    const uint64_t one = 0x01DC000000000201ull;
    StopFacts f = GoodStop(path, one, 21);
    f.closeSeq = 1;
    REQUIRE(f.verdict.verified);
    char err[192] = {};
    REQUIRE(WriteStopSentinel(dir.Path(), f, err, sizeof(err)));
    REQUIRE(ArchiveStopSentinel(dir.Path(), f, err, sizeof(err)));
    CHECK(err[0] == 0);

    // The archived copy carries the PER-CLOSE name: the archive is what gets collected
    // into the run directory, so it is the copy that has to hold every close of the launch.
    wchar_t name[64];
    REQUIRE(StopSentinelNameForClose(one, 21, f.closeSeq, name, 64));
    wchar_t archived[MAX_PATH];
    _snwprintf_s(archived, MAX_PATH, _TRUNCATE, L"%s\\%s\\%s", dir.Path(), kStopArchiveDirName,
                 name);
    REQUIRE(::GetFileAttributesW(archived) != INVALID_FILE_ATTRIBUTES);

    // Launch two's Init sweeps the launch directory. G-8: the swept path is gone, the archive is not
    // -- the harness can still collect launch one's evidence after launch two has started.
    uint32_t failed = 99;
    const uint32_t swept = DeleteStaleStopSentinels(dir.Path(), &failed, nullptr, nullptr);
    CHECK(swept == 1);
    CHECK(failed == 0);
    CHECK(CountSentinels(dir.Path()) == 0);
    CHECK(::GetFileAttributesW(archived) != INVALID_FILE_ATTRIBUTES);
    const std::string doc = ReadAll(archived);
    CHECK(RawField(doc, "launchFileTime") == "01DC000000000201");
    CHECK(RawField(doc, "verified") == "true");

    // ...and the archive is NOT read as this launch's evidence: after the sweep the launch directory
    // is empty, which is what makes an absent sentinel mean "this capture never finalised".
    CHECK(ClassifyLaunch(dir.Path(), one, 21, kRecordCountClosed, nullptr) == "CAPTURE-UNFINALIZED");
}

TEST_CASE("the sweep names every file it tried") {
    ScratchDir dir(L"sweeplog");
    wchar_t path[MAX_PATH];
    dir.File(L"sw.echodemo", path);
    WriteCapture(path, 2, true);
    StopFacts f = GoodStop(path, 0x01DC000000000301ull, 31);
    char err[192] = {};
    REQUIRE(WriteStopSentinel(dir.Path(), f, err, sizeof(err)));
    wchar_t legacy[MAX_PATH];
    dir.File(L"capture_stop.json", legacy);
    HANDLE h = ::CreateFileW(legacy, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    ::CloseHandle(h);

    // G-11 / plan item 2: each deletion is reported, not just counted, because when one will not
    // delete the reader needs to know WHICH file survived.
    std::wstring seen;
    const uint32_t swept = DeleteStaleStopSentinels(
        dir.Path(), nullptr,
        [](const wchar_t* n, bool deleted, void* ctx) {
            *static_cast<std::wstring*>(ctx) += std::wstring(n) + (deleted ? L"=gone;" : L"=stays;");
        },
        &seen);
    CHECK(swept == 2);
    wchar_t name[64];
    REQUIRE(StopSentinelNameFor(0x01DC000000000301ull, 31, name, 64));
    CHECK(seen.find(std::wstring(name) + L"=gone;") != std::wstring::npos);
    CHECK(seen.find(L"capture_stop.json=gone;") != std::wstring::npos);
}

// ---------------------------------------------------------------------------------------------
// L2-RUN-5 F2. Every sentinel of every launch was unparseable, because each rosterRefusals entry was
// built with _snprintf_s(_TRUNCATE) into 96 bytes and two of the engine's refusal sentences are
// longer than that: the value was cut mid-word, with no closing quote and no closing brace, and the
// harness answered CAPTURE-UNREADABLE for a document that was otherwise complete.
//
// The oracle is a PARSE, not a substring: a scanner that walks the document as JSON tokens catches
// both halves of the bug -- the unterminated string AND an escape that was not written -- and it
// catches them in whatever field they happen to land in.
namespace {

// Minimal, and deliberately not clever: strings must terminate, braces must balance, and a quote or
// a backslash inside a string must be escaped. That is exactly the class of defect F2 was.
bool JsonWellFormed(const std::string& doc, std::string& why) {
    int depth = 0;
    bool inString = false;
    for (size_t i = 0; i < doc.size(); ++i) {
        const char c = doc[i];
        if (inString) {
            if (c == '\\') {
                if (i + 1 >= doc.size()) { why = "a backslash escape runs off the end"; return false; }
                ++i;
                continue;
            }
            if (c == '"') { inString = false; continue; }
            if (c == '\n' || c == '\r') { why = "a raw newline inside a string"; return false; }
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == '{' || c == '[') ++depth;
        if (c == '}' || c == ']') {
            if (--depth < 0) { why = "a closing brace with nothing open"; return false; }
        }
    }
    if (inString) { why = "the document ends inside a string"; return false; }
    if (depth != 0) { why = "the document ends with an unclosed object"; return false; }
    return true;
}

std::string ReadOnlySentinel(const wchar_t* dir) {
    std::wstring pattern = std::wstring(dir) + L"\\capture_stop.*.json";
    WIN32_FIND_DATAW fd = {};
    HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    const std::wstring path = std::wstring(dir) + L"\\" + fd.cFileName;
    ::FindClose(h);
    return ReadAll(path.c_str());
}

}  // namespace

TEST_CASE("a refusal reason longer than the entry buffer leaves the sentinel PARSEABLE") {
    ScratchDir dir(L"refusalwords");

    StopFacts f = {};
    f.launchFileTime = 0x01DD3C814E1BCB24ull;
    f.pid = 4321;
    f.reason = StopReason::CostKill;
    f.fileClosed = true;

    // The engine's real word for the refusal L2-RUN-5 saw, verbatim from identity_access.cpp -- the
    // one that did not fit -- and a second word carrying the two characters JSON cannot hold raw.
    // A test that supplied a short reason could not fail against the shipped bug.
    const char* kLong =
        "a node inside the entrant's profile document is not in committed memory";
    const char* kQuoted =
        "that index is inside the replay band and would be renamed \"Replayer %u\" C:\\Users\\x";
    f.rosterRefusals.Note(RosterRefusal::EntrantJson, kLong);
    f.rosterRefusals.Note(RosterRefusal::Platform, kQuoted);

    char err[192] = {};
    REQUIRE(WriteStopSentinel(dir.Path(), f, err, sizeof(err)));

    const std::string doc = ReadOnlySentinel(dir.Path());
    REQUIRE(doc.size() > 0);

    std::string why;
    INFO("document: " << doc);
    CHECK_MESSAGE(JsonWellFormed(doc, why), why);

    // The WHOLE reason is in the document, not a prefix of it: the harness reads this text to know
    // which field refused and why, and 71 of 71 characters is the difference between a repair and a
    // shrug.
    CHECK(doc.find(kLong) != std::string::npos);
    // And the two characters that make a document unparseable are escaped rather than dropped: the
    // raw forms must NOT be present, the escaped forms must.
    CHECK(doc.find("\\\"Replayer %u\\\"") != std::string::npos);
    CHECK(doc.find("C:\\\\Users\\\\x") != std::string::npos);
}

// The refusedBy block of the sentinel. The histogram existing on StopFacts and reaching the FILE
// are two claims: this asserts the document text, because the harness and the owner read the
// document and nothing else. Non-zero arms only, in enum order, and an arm this build has no name
// for is published as "unknown" rather than dropped -- a refusal nobody can spell is still a
// refusal, and dropping it would make the block read as a healthier capture than the one taken.
TEST_CASE("the sentinel names the body refusal arms that fired, and only those") {
    ScratchDir dir(L"refusedby");
    StopFacts f{};
    f.launchFileTime = 0x01DD3C814E1BCB24ull;
    f.pid = 4321;
    f.reason = StopReason::CostKill;
    f.fileClosed = true;
    f.bodiesSampled = 30;
    f.bodiesRefused = 10;
    f.bodyRefusals[unsigned(replay::BodyRefusal::None)] = 30;
    f.bodyRefusals[unsigned(replay::BodyRefusal::NoPrimaryBody)] = 7;
    f.bodyRefusals[unsigned(replay::BodyRefusal::BadZone)] = 2;
    f.bodyRefusalUnknown = 1;

    char err[192] = {};
    REQUIRE(WriteStopSentinel(dir.Path(), f, err, sizeof(err)));
    const std::string doc = ReadOnlySentinel(dir.Path());
    REQUIRE(doc.size() > 0);
    std::string why;
    INFO("document: " << doc);
    CHECK_MESSAGE(JsonWellFormed(doc, why), why);

    // The whole block, matched as one string: this pins the names, the counts, the enum order and
    // the ABSENCE of the None arm at once. A block listing "none": 30 would report the 30 healthy
    // reads as refusals.
    CHECK(doc.find("\"refusedBy\": {\"noPrimaryBody\": 7, \"badZone\": 2, \"unknown\": 1}") !=
          std::string::npos);
}

// A capture where nothing refused must publish an EMPTY block, not an absent one: the field is how
// a reader tells "no gate refused" from "this build does not report gates".
TEST_CASE("the sentinel publishes an empty refusedBy when no gate refused") {
    ScratchDir dir(L"refusedbyempty");
    StopFacts f{};
    f.launchFileTime = 0x01DD3C814E1BCB25ull;
    f.pid = 4322;
    f.reason = StopReason::CostKill;
    f.fileClosed = true;
    f.bodiesSampled = 40;
    f.bodyRefusals[unsigned(replay::BodyRefusal::None)] = 40;

    char err[192] = {};
    REQUIRE(WriteStopSentinel(dir.Path(), f, err, sizeof(err)));
    const std::string doc = ReadOnlySentinel(dir.Path());
    std::string why;
    INFO("document: " << doc);
    CHECK_MESSAGE(JsonWellFormed(doc, why), why);
    CHECK(doc.find("\"refusedBy\": {}") != std::string::npos);
}

// The angVelBy block. A body read through the ncaTransform arm carries a STRUCTURAL zero omega --
// there is no CPhBody, so nothing anywhere holds a rate -- and one read off a history node carries
// a stored measurement; both are three zero-ish floats in the file. Non-zero arms only, in enum
// order, and index 0 IS published: unlike refusedBy's `none`, notAsked is a statement about
// bodies that reached no arm.
TEST_CASE("the sentinel names which arm answered angular velocity, and only the arms that fired") {
    ScratchDir dir(L"angvelby");
    StopFacts f{};
    f.launchFileTime = 0x01DD3C814E1BCB26ull;
    f.pid = 4323;
    f.reason = StopReason::CostKill;
    f.fileClosed = true;
    f.bodiesSampled = 12;
    f.angVelBy[unsigned(replay::AngVelSource::NotAsked)] = 2;
    f.angVelBy[unsigned(replay::AngVelSource::NodeStored)] = 5;
    f.angVelBy[unsigned(replay::AngVelSource::Unavailable)] = 7;
    f.angVelUnknown = 1;

    char err[192] = {};
    REQUIRE(WriteStopSentinel(dir.Path(), f, err, sizeof(err)));
    const std::string doc = ReadOnlySentinel(dir.Path());
    REQUIRE(doc.size() > 0);
    std::string why;
    INFO("document: " << doc);
    CHECK_MESSAGE(JsonWellFormed(doc, why), why);

    // The whole block as one string: the names, the counts, the enum order and the ABSENCE of
    // the arms that never answered, all at once.
    CHECK(doc.find("\"angVelBy\": {\"notAsked\": 2, \"nodeStored\": 5, \"unavailable\": 7, "
                   "\"unknown\": 1}") != std::string::npos);
}

// The producer's counters and refusals were write-only: a run whose log showed no
// census line had five possible causes and the sentinel named none of them. This drives the REAL
// sentinel writer and reads the document back, because "the field exists in a struct" is not the
// claim -- "a harness that never opened the file can tell which of the five it was" is.
TEST_CASE("the sentinel names why an identity block was not written") {
    ScratchDir dir(L"identitywords");

    StopFacts f = {};
    f.launchFileTime = 0x01DD3C814E1BCB24ull;
    f.pid = 4321;
    f.reason = StopReason::CostKill;
    f.fileClosed = true;
    f.rosterRows = 3;
    f.identityBlocks = 0;
    f.identityRefusals[size_t(IdentityEmitRefusal::SinkRefused)] = 7;
    f.identityRefusals[size_t(IdentityEmitRefusal::StreamDisabled)] = 2;
    f.identityJsonRefused = 1;
    f.identityDeferred = 4;
    f.identityTruncationUnknown = 3;
    f.identityLastJsonWhy = "a node inside the entrant's profile document is not in committed memory";

    char err[192] = {};
    REQUIRE(WriteStopSentinel(dir.Path(), f, err, sizeof(err)));
    const std::string doc = ReadOnlySentinel(dir.Path());
    REQUIRE(doc.size() > 0);
    std::string why;
    INFO("document: " << doc);
    CHECK_MESSAGE(JsonWellFormed(doc, why), why);

    // Three rows with three players and ZERO blocks, and the document says which reasons fired --
    // by the same words IdentityEmitRefusalText prints, so the log and the sentinel agree.
    CHECK(doc.find("\"identity\"") != std::string::npos);
    CHECK(doc.find("\"blocks\": 0") != std::string::npos);
    CHECK(doc.find("IdentityEmitSinkRefused\": 7") != std::string::npos);
    CHECK(doc.find("IdentityEmitStreamDisabled\": 2") != std::string::npos);
    CHECK(doc.find("\"deferredToNextScan\": 4") != std::string::npos);
    CHECK(doc.find("\"slotsTruncationUnknown\": 3") != std::string::npos);
    // The engine's whole word for the profile refusal, not a prefix and not a count.
    CHECK(doc.find(f.identityLastJsonWhy) != std::string::npos);

    // A reason that did NOT fire is absent rather than printed as zero: a list of every word with a
    // zero beside it is what makes a reader stop reading the list.
    CHECK(doc.find("IdentityEmitNoTapType") == std::string::npos);
}

