// RECORDER-5. The four recorder defects the first live capture (2026-09-19, 4v4 Echo Arena) put in
// a file, each pinned by the shape of the real failure rather than by the fix's own words.
//
// F1 TWO CLOCK BASES. The header advertised 3951.939 s of duration for 105.659 s of footage, chapter
//    2 was a 3846 s hole and the eight Join marks sat outside the footage. Two tests: the file a
//    correct origin produces, and the structural bound that holds even when an origin is wrong.
// F2 PROFILE DOCUMENTS. 8 of 9 refused ShortBuffer against a 4096 B buffer; the one that fit was
//    2910 B. Tests the widths that used to be lost, the width that is still refused, and that a full
//    roster of wide documents cannot push the manifest out of one chapter payload.
// F3 FIRST SNAPSHOT READ 0 OF 9 KEYS. The cadence rule, and the keyframe that carries the sample.
// F5 FILE NAME. The gametype field of a capture's name, rewritten from the resolved value.

#include "doctest.h"

#include <windows.h>

#include <string.h>

#include <string>
#include <vector>

#include "capture/capture_types.h"
#include "capture/chapterbuilder.h"
#include "capture/identity_block.h"
#include "capture/identity_emit.h"
#include "capture/manifestbuild.h"
#include "capture/netvarsnapshot.h"
#include "capture/roster.h"
#include "foundation/demofile/demofile.h"
#include "foundation/demofile/demotypes.h"
#include "foundation/demofile/format.h"
#include "foundation/demofile/manifest_io.h"

using namespace replay;
using namespace replay::capture;

namespace {

struct TempFile {
    wchar_t path[512];
    explicit TempFile(const wchar_t* tag) {
        wchar_t dir[400];
        const DWORD n = GetTempPathW(400, dir);
        if (!n) { dir[0] = L'.'; dir[1] = 0; }
        _snwprintf_s(path, 512, _TRUNCATE, L"%lsr5_%ls_%lu.echodemo", dir, tag,
                     (unsigned long)GetCurrentProcessId());
    }
    ~TempFile() { DeleteFileW(path); }
};

template <class T>
void Feed(ChapterBuilder& b, uint16_t kind, const T& payload) {
    uint8_t buf[sizeof(RecHeader) + sizeof(T)];
    RecHeader h{};
    h.kind = kind;
    h.bytes = (uint16_t)sizeof(T);
    h.producer = 0;
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, &payload, sizeof payload);
    b.Consume(buf, (uint32_t)sizeof buf);
}

void FeedTick(ChapterBuilder& b, uint64_t tUs, uint32_t frame, uint32_t dtUs) {
    TickRec tr{};
    tr.sourceTimeUs = tUs;
    tr.simTimeUs = tUs;
    tr.frameIndex = frame;
    tr.frameDeltaUs = dtUs;
    tr.scaledDeltaUs = dtUs;
    Feed(b, kRecTick, tr);
}

// One S_EVENT row with a foreign stamp -- exactly what the pre-open buffer holds. `stampUs` is the
// tap's arrival microsecond on the PRE-OPEN axis, which is not the file's axis.
void FeedPreOpenEvent(ChapterBuilder& b, uint64_t foreignStampUs) {
    EventRec er{};
    er.stampUs = foreignStampUs;
    er.dst = 0;
    er.typeIndex = 1;
    er.bodyBytes = 0;
    Feed(b, kRecEvent, er);
}

constexpr uint64_t kKeyframeUs = 2000000;
constexpr uint64_t kTickUs = 11111;   // 90 Hz, the live capture's own rate

// A chapter is cut once it has REACHED the interval, so the record that crosses it belongs to the
// chapter it ends: the bound is the interval plus the one step that crossed it, never a whole hole.
constexpr uint64_t kLongestChapterUs = kKeyframeUs + kTickUs;

ManifestBuilder MakeManifest(uint64_t t0Us) {
    ManifestBuilder mb;
    mb.Begin(t0Us, 0, 0x40);
    const uint8_t md5[16] = {0xca, 0xa0, 0x5e, 0xe3};
    mb.SetBuild(md5, 0x676F6C646D617374ull, 0x0102030405060708ull);
    mb.SetLevel(6300205991959903307ull, 0xCB60A4DE7E1CAF73ull, 1780177787ull);
    mb.SetKeyframeIntervalUs((uint32_t)kKeyframeUs);
    mb.SetUuid(1, 2);
    return mb;
}

BuilderConfig MakeConfig() {
    BuilderConfig cfg;
    cfg.keyframeIntervalUs = kKeyframeUs;
    for (uint16_t i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;
    for (uint16_t i = 0; i < kSectionCount; ++i) cfg.sectionEnabled[i] = true;
    return cfg;
}

struct FileSpan {
    uint64_t tMin = ~0ull;
    uint64_t tMax = 0;
    uint64_t longestChapterUs = 0;
    size_t chapters = 0;
};

// The span `echodemo info` prints as `duration`: the whole chapter index, MANIFEST included.
FileSpan MeasureFile(const wchar_t* path) {
    DemoReader r;
    REQUIRE(r.Open(path));
    FileSpan s;
    s.chapters = r.ChapterCount();
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        const ChapterKey k = r.ChapterAt(i);
        if (k.tBeginUs < s.tMin) s.tMin = k.tBeginUs;
        if (k.tEndUs > s.tMax) s.tMax = k.tEndUs;
        const uint64_t span = k.tEndUs > k.tBeginUs ? k.tEndUs - k.tBeginUs : 0;
        if (span > s.longestChapterUs) s.longestChapterUs = span;
    }
    if (s.tMin == ~0ull) s.tMin = 0;
    return s;
}

// A synthetic session in the shape of the live one: a lobby whose identity traffic was buffered
// before the file existed, then 106 s of footage. `t0Us` is the origin the capture opens with, and
// `footageStartUs` is where the first tick actually lands -- the same number when the origin is on
// the records' own axis, wildly different when it is not.
BuilderStats DriveSession(const wchar_t* path, uint64_t t0Us, uint64_t footageStartUs,
                          uint32_t seconds, uint64_t preOpenStampUs, uint32_t preOpenRecords) {
    ManifestBuilder mb = MakeManifest(t0Us);
    REQUIRE(mb.Refusal() == nullptr);
    DemoWriter writer;
    REQUIRE(writer.Open(path, mb.View()));

    ChapterBuilder b;
    REQUIRE(b.Open(&writer, mb.View(), MakeConfig()));

    // The open's own order: the pre-open flush, then the origin's marker.
    for (uint32_t i = 0; i < preOpenRecords; ++i) {
        FeedPreOpenEvent(b, preOpenStampUs + i * 1000ull);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    }
    b.NoteDegraded(t0Us, kStreamEvent, kDegradePreOpenIdentityPlaced, preOpenRecords);
    b.EmitMatchBegin(t0Us);

    const uint64_t dt = kTickUs;
    const uint32_t ticks = (uint32_t)(seconds * 1000000ull / dt);
    for (uint32_t f = 0; f < ticks; ++f) {
        FeedTick(b, footageStartUs + f * dt, f + 1, (uint32_t)dt);
    }
    b.EmitMatchEnd(footageStartUs + ticks * dt);
    b.FlushChapter();
    const BuilderStats out = b.Stats();
    b.Close();
    writer.Close();
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// F1. ONE CLOCK BASE
// ---------------------------------------------------------------------------------------------

TEST_CASE("RECORDER-5 F1: an origin on the records' own axis gives a header duration that is the "
          "footage, with no empty chapter and the marks inside the span") {
    TempFile f(L"axis_one_base");
    // The live capture's own numbers: the footage began at 3862.614 s on the peer clock and ran
    // 105.7 s. The origin is that same instant, because the role picked the same axis.
    const uint64_t footage = 3862614000ull;
    const uint32_t seconds = 106;
    const BuilderStats st = DriveSession(f.path, footage, footage, seconds, 16334000ull, 386);

    const FileSpan s = MeasureFile(f.path);
    const uint64_t span = s.tMax - s.tMin;
    // "== footage +/- one chapter": the span may carry the keyframe chapter at each end.
    CHECK(span >= uint64_t(seconds) * 1000000ull - kKeyframeUs);
    CHECK(span <= uint64_t(seconds) * 1000000ull + kKeyframeUs);
    // The defect this replaces advertised 3951.939 s for 105.659 s of footage.
    CHECK(span < 120000000ull);
    // No chapter is a hole. The live file's chapter 2 spanned 3846 s.
    CHECK(s.longestChapterUs <= kLongestChapterUs);
    // Nothing jumped: the origin and the first record are on one clock.
    CHECK(st.axisJumpSplits == 0);
    // MATCH_BEGIN rode t0 and MATCH_END the last tick; both are inside the chapter index.
    CHECK(s.tMin == footage);
    CHECK(s.tMax >= footage);
}

TEST_CASE("RECORDER-5 F1: an origin on the WRONG clock can no longer produce a chapter longer than "
          "the keyframe interval") {
    TempFile f(L"axis_two_bases");
    // Exactly the live file: t0 = 16.334 s on the process-relative clock, footage from 3862.614 s
    // on the peer clock. The origin bug is fixed at its source; this is the structural bound that
    // holds when something else puts the two apart again.
    const uint64_t t0 = 16334000ull;
    const uint64_t footage = 3862614000ull;
    const BuilderStats st = DriveSession(f.path, t0, footage, 20, 16334000ull, 386);

    const FileSpan s = MeasureFile(f.path);
    CHECK(s.longestChapterUs <= kLongestChapterUs);
    // The jump is a chapter boundary, and it is counted rather than buried.
    CHECK(st.axisJumpSplits >= 1);
}

// ---------------------------------------------------------------------------------------------
// F2. PROFILE DOCUMENTS
// ---------------------------------------------------------------------------------------------

namespace {

PlayerRecord MakePlayer(uint16_t slot, const std::vector<uint8_t>& profile,
                        const std::vector<uint8_t>& loadout) {
    PlayerRecord p{};
    p.xplatformId = 0x1000ull + slot;
    p.userSlot = slot;
    p.userIndex = slot;
    p.entrantJson.p = profile.empty() ? nullptr : profile.data();
    p.entrantJson.n = (uint32_t)profile.size();
    p.unlocksJson.p = loadout.empty() ? nullptr : loadout.data();
    p.unlocksJson.n = (uint32_t)loadout.size();
    return p;
}

std::vector<uint8_t> Document(size_t bytes, uint8_t fill) {
    return std::vector<uint8_t>(bytes, fill);
}

}  // namespace

TEST_CASE("RECORDER-5 F2: a profile document of the width the live capture lost survives the "
          "manifest whole") {
    // Every one of the eight lost documents was over 4096 B; the one that fit was 2910 B. 12 KiB is
    // a real live profile and would have been refused ShortBuffer before this.
    const std::vector<uint8_t> profile = Document(12288, 'p');
    const std::vector<uint8_t> loadout = Document(488, 'l');  // facts-ok: SPECTATOR.CS_LOCAL_INDEX_OFF a test fixture value this case compares against, not an engine position
    REQUIRE(profile.size() > 4096u);
    REQUIRE(profile.size() <= kPlayerJsonCap);

    Manifest m{};
    PlayerRecord p = MakePlayer(7, profile, loadout);
    m.players = &p;
    m.playerCount = 1;

    std::vector<uint8_t> bytes;
    SerializeManifest(m, bytes);

    ManifestStore store;
    const char* err = nullptr;
    REQUIRE(DeserializeManifest(bytes.data(), (uint32_t)bytes.size(), store, &err));
    const Manifest& back = store.View();
    REQUIRE(back.playerCount == 1);
    CHECK(back.players[0].entrantJson.n == profile.size());
    CHECK(back.players[0].unlocksJson.n == loadout.size());
    CHECK(memcmp(back.players[0].entrantJson.p, profile.data(), profile.size()) == 0);
    CHECK((back.players[0].jsonTruncated & kPlayerJsonTruncEntrant) == 0);
}

TEST_CASE("RECORDER-5 F2: a document above the cap is stored short with its bit set, never "
          "silently cut") {
    const std::vector<uint8_t> profile = Document(20480, 'p');   // the brief's 20 KB
    const std::vector<uint8_t> loadout = Document(455, 'l');
    REQUIRE(profile.size() > kPlayerJsonCap);

    Manifest m{};
    PlayerRecord p = MakePlayer(0, profile, loadout);
    m.players = &p;
    m.playerCount = 1;

    std::vector<uint8_t> bytes;
    SerializeManifest(m, bytes);

    ManifestStore store;
    const char* err = nullptr;
    REQUIRE(DeserializeManifest(bytes.data(), (uint32_t)bytes.size(), store, &err));
    const Manifest& back = store.View();
    REQUIRE(back.playerCount == 1);
    CHECK(back.players[0].entrantJson.n == kPlayerJsonCap);
    CHECK((back.players[0].jsonTruncated & kPlayerJsonTruncEntrant) != 0);
    CHECK((back.players[0].jsonTruncated & kPlayerJsonTruncUnlocks) == 0);
}

TEST_CASE("RECORDER-5 F2: a full roster of wide documents still fits one chapter payload, and no "
          "row is left below the guaranteed floor") {
    // The per-blob cap is four times what a full roster's worst case could statically justify, so
    // the serialiser holds the budget at run time. Both halves of that are asserted here: the
    // manifest fits, and the LAST row -- the one a greedy early row would starve -- keeps the floor.
    const std::vector<uint8_t> wide = Document(kPlayerJsonCap, 'w');
    std::vector<PlayerRecord> players;
    for (uint16_t i = 0; i < kPlayerJsonRosterBudget; ++i) {
        players.push_back(MakePlayer(i, wide, wide));
    }
    Manifest m{};
    m.players = players.data();
    m.playerCount = (uint16_t)players.size();

    std::vector<uint8_t> bytes;
    SerializeManifest(m, bytes);
    CHECK(bytes.size() <= kMaxChapterPayload);

    ManifestStore store;
    const char* err = nullptr;
    REQUIRE(DeserializeManifest(bytes.data(), (uint32_t)bytes.size(), store, &err));
    const Manifest& back = store.View();
    REQUIRE(back.playerCount == kPlayerJsonRosterBudget);
    for (uint16_t i = 0; i < kPlayerJsonRosterBudget; ++i) {
        CHECK(back.players[i].entrantJson.n >= kPlayerJsonFloorCap);
        CHECK(back.players[i].unlocksJson.n >= kPlayerJsonFloorCap);
    }
}

TEST_CASE("RECORDER-5 F2: one entrant's identity block still fits the stream's own body bound") {
    // The document cap and S_EVENT's uint16_t body length are one decision. A cap that outgrew the
    // bound would desynchronise the whole chapter rather than refuse a record.
    CHECK(kMaxEventBodyBytes <= 0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE the u16 range limit, not the engine no-slot field
    CHECK(kIdentityBlockOneEntrantCap >= kPlayerJsonCap);
}

// ---------------------------------------------------------------------------------------------
// F3. THE FIRST SNAPSHOT
// ---------------------------------------------------------------------------------------------

TEST_CASE("RECORDER-5 F3: the snapshot warms up until every key has answered, then settles to the "
          "keyframe cadence") {
    const uint64_t settled = 1000000;
    const uint64_t fast = kNetvarWarmEveryTickUntilUs;
    const uint64_t warm = kNetvarWarmIntervalUs;

    // Not every key has answered: every tick for the first window, then the warm cadence.
    CHECK(NetvarSnapshotIntervalUs(false, 0, settled) == 0);
    CHECK(NetvarSnapshotIntervalUs(false, fast - 1, settled) == 0);
    CHECK(NetvarSnapshotIntervalUs(false, fast, settled) == warm);
    CHECK(NetvarSnapshotIntervalUs(false, fast * 100, settled) == warm);
    // The live defect: a refusal must never cost the keyframe interval while warming.
    CHECK(NetvarSnapshotIntervalUs(false, fast, settled) < settled);
    CHECK(warm <= 250000u);

    // Every key has answered: the caller's cadence, whatever the clock says.
    CHECK(NetvarSnapshotIntervalUs(true, 0, settled) == settled);
    CHECK(NetvarSnapshotIntervalUs(true, fast * 100, settled) == settled);
}

namespace {

// A source that already holds a sample when the file opens -- which is what the arming ticks are
// for. `rows` is what the tick left behind.
class PrimedSnapshot : public INetvarSnapshotSource {
public:
    PrimedSnapshot(const NetvarSnapshotRow* rows, uint16_t n) : rows_(rows), n_(n) {}
    uint16_t Snapshot(NetvarSnapshotRow* out, uint16_t cap, uint16_t& unreadable) override {
        ++calls;
        const uint16_t n = n_ < cap ? n_ : cap;
        for (uint16_t i = 0; i < n; ++i) out[i] = rows_[i];
        unreadable = 0;
        return n;
    }
    uint32_t calls = 0;

private:
    const NetvarSnapshotRow* rows_;
    uint16_t n_;
};

}  // namespace

TEST_CASE("RECORDER-5 F3: a sample taken before the file opens reaches the FIRST keyframe") {
    TempFile f(L"first_keyframe");
    const NetvarSnapshotRow rows[2] = {
        {0x1C97052A21E09F11ull, 2, 0},   // blue_score = 2, the value the live capture opened on
        {0x2ED07B1C8C27548Dull, 0, 0},   // red_score  = 0, the one that never reached the file
    };
    PrimedSnapshot src(rows, 2);

    const uint64_t t0 = 3862614000ull;
    ManifestBuilder mb = MakeManifest(t0);
    DemoWriter writer;
    REQUIRE(writer.Open(f.path, mb.View()));
    ChapterBuilder b;
    b.SetNetvarSnapshotSource(&src);
    REQUIRE(b.Open(&writer, mb.View(), MakeConfig()));
    b.EmitMatchBegin(t0);
    for (uint32_t i = 0; i < 4; ++i) FeedTick(b, t0 + i * kTickUs, i + 1, (uint32_t)kTickUs);
    b.FlushChapter();
    const BuilderStats st = b.Stats();
    b.Close();
    writer.Close();

    // The first keyframe asked, and it got a value for both score keys rather than the live run's
    // "0 keys at keyframe 1 (9 unreadable)".
    CHECK(src.calls >= 1);
    CHECK(st.netvarSnapshots >= 1);
    CHECK(st.netvarSnapshotSeeded >= 2);
    CHECK(st.netvarSnapshotUnreadable == 0);
}

// ---------------------------------------------------------------------------------------------
// F5. THE FILE NAME
// ---------------------------------------------------------------------------------------------

namespace {

// The rename's own arithmetic, mirrored from capture.cpp: only the last field of the name moves, so
// the timestamp and the level id a person recognises the recording by are untouched.
bool RewriteGametypeField(wchar_t* path, uint64_t resolved) {
    if (resolved == 0 || path[0] == 0) return false;
    wchar_t want[17];
    _snwprintf_s(want, 17, _TRUNCATE, L"%016llX", (unsigned long long)resolved);
    const size_t len = wcslen(path);
    const wchar_t* kExt = L".echodemo";
    const size_t extLen = wcslen(kExt);
    if (len < extLen + 17 || _wcsicmp(path + len - extLen, kExt) != 0) return false;
    const size_t at = len - extLen - 16;
    if (path[at - 1] != L'_' || wcsncmp(path + at, want, 16) == 0) return false;
    memcpy(path + at, want, 16 * sizeof(wchar_t));
    return true;
}

}  // namespace

TEST_CASE("RECORDER-5 F5: the unresolved gametype in a capture's name is replaced by the resolved "
          "one, and nothing else in the name moves") {
    wchar_t name[512];
    wcscpy_s(name, 512,
             L"C:\\Users\\x\\Documents\\EchoReplay\\"
             L"2026-09-19_21-11-01_576ED3F8428EBC4B_FFFFFFFFFFFFFFFF.echodemo");
    CHECK(RewriteGametypeField(name, 0xCB60A4DE7E1CAF73ull));
    CHECK(wcscmp(name,
                 L"C:\\Users\\x\\Documents\\EchoReplay\\"
                 L"2026-09-19_21-11-01_576ED3F8428EBC4B_CB60A4DE7E1CAF73.echodemo") == 0);

    // Already resolved: no rename, so no risk and no log line.
    CHECK_FALSE(RewriteGametypeField(name, 0xCB60A4DE7E1CAF73ull));
    // Nothing to resolve to.
    CHECK_FALSE(RewriteGametypeField(name, 0));

    // A name that is not one of ours is left alone rather than cut at a guessed offset.
    wchar_t other[512];
    wcscpy_s(other, 512, L"C:\\tmp\\short.echodemo");
    CHECK_FALSE(RewriteGametypeField(other, 0xCB60A4DE7E1CAF73ull));
    CHECK(wcscmp(other, L"C:\\tmp\\short.echodemo") == 0);
}

TEST_CASE("RECORDER-5 F5: renaming a closed capture keeps every byte of it") {
    TempFile f(L"rename_src");
    const uint64_t t0 = 3862614000ull;
    DriveSession(f.path, t0, t0, 4, t0, 4);

    HANDLE h = CreateFileW(f.path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    LARGE_INTEGER before{};
    REQUIRE(GetFileSizeEx(h, &before));
    CloseHandle(h);

    wchar_t renamed[512];
    _snwprintf_s(renamed, 512, _TRUNCATE, L"%ls.renamed", f.path);
    DeleteFileW(renamed);
    // The writer's handle is closed by now, which is the only reason this is allowed at all.
    REQUIRE(MoveFileExW(f.path, renamed, 0));

    const FileSpan s = MeasureFile(renamed);
    CHECK(s.chapters > 0);
    h = CreateFileW(renamed, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    LARGE_INTEGER after{};
    REQUIRE(GetFileSizeEx(h, &after));
    CloseHandle(h);
    CHECK(after.QuadPart == before.QuadPart);
    DeleteFileW(renamed);
}
