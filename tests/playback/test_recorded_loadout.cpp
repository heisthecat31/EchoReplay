// The identity block, decoded OFF DISK at playback.
//
// The block, its writer and its reader all existed and nothing on the playback side had ever opened
// one: every recorded loadout key stopped at the container, and the provisioner dressed entrants
// from positions. So these cases run the real chain -- IdentityBlockWriter -> an S_EVENT row ->
// ChapterBuilder -> zstd -> the container -> DemoReader -> ScanRecordedIdentity -> the contract --
// rather than handing the store bytes it never had to find.
//
// The v1 and v2 fixtures are laid out from the constants THOSE versions wrote (kIdentityHeaderSizeV1,
// kIdentityEntrantSizeV1/V2, kIdentitySlotStrideV1), never from this build's sizeof: both the header
// and the entrant grew, and a fixture built from today's widths would move with a defect instead of
// catching it. The v3 fixture is written by the SHIPPED writer, because that direction has a producer.

#include "doctest.h"

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "capture/chapterbuilder.h"
#include "capture/identity_block.h"
#include "capture/manifestbuild.h"
#include "foundation/demofile/demofile.h"
#include "playback/core/recorded_loadout.h"

using namespace replay;
using namespace replay::capture;
using replay::identity::kRecordedLoadoutPairCap;
using replay::identity::kRecordedPairCountUnknown;
using replay::identity::RecordedLoadout;
using replay::identity::RecordedIdentitySource;
using replay::identity::RecordedProfileState;
using replay::identity::ReadRecordedLoadout;
using replay::identity::ScanRecordedIdentity;

namespace {

const uint64_t kSlotKeyBase = 0x5EED000000000000ull;
// The instant the standalone block fixtures are stamped at -- the same one the written demo
// starts from, so this file carries one fixture clock rather than a second bare number.
const uint64_t kFixtureT0 = 5000000;
const uint64_t kItemBase    = 0x1701000000000000ull;

IdentityEntrant Person(uint16_t userSlot, const char* name) {
    IdentityEntrant e{};
    e.xplatformId = 0xDEC0DE0000000000ull + userSlot;
    e.userSlot = userSlot;
    e.userIndex = userSlot;
    e.generation = 1;
    e.alignment = 3;
    e.teamId = 1;
    e.teamSlot = uint8_t(userSlot);
    e.bodyType = 2;
    e.loadoutNumber = 11;
    e.loadoutNumberBody = 12;
    e.loadoutHeader3C = 0x3C3C3C3Cu;
    _snprintf_s(e.displayName, sizeof e.displayName, _TRUNCATE, "%s", name);
    _snprintf_s(e.platformStr, sizeof e.platformStr, _TRUNCATE, "PC");
    e.flags = kIdentityEntrantProfileAsked;
    return e;
}

// v3, through the writer this build ships.
std::vector<uint8_t> WriteV3Block(uint64_t tUs, const IdentityEntrant& fields, const std::string& json,
                                  uint16_t filledPairs) {
    IdentityEntrant f = fields;
    f.filledPairCount = filledPairs;
    IdentityLoadoutSlot slots[kIdentityBlockSlotCount] = {};
    for (uint16_t i = 0; i < kIdentityBlockSlotCount; ++i) {
        slots[i].slotIndex = i;
        slots[i].itemId = kItemBase + i + fields.userSlot;
        slots[i].slotKey = kSlotKeyBase + i + fields.userSlot;
    }
    std::vector<uint8_t> buf(kMaxEventBodyBytes);
    IdentityBlockWriter w(buf.data(), buf.size(), tUs, 0xC0FFEEull);
    IdentityBlockEntrantIn in{};
    in.fields = f;
    in.slots = slots;
    Blob b{};
    b.p = reinterpret_cast<const uint8_t*>(json.data());
    b.n = uint32_t(json.size());
    in.profileJson = b;
    REQUIRE(w.Add(in));
    size_t bytes = 0;
    REQUIRE(w.Finish(bytes));
    buf.resize(bytes);
    return buf;
}

// v1: 28-byte header, 92-byte entrant, 16-byte slot records, and no key anywhere in them.
std::vector<uint8_t> BuildV1Block(uint64_t tUs, const IdentityEntrant& fields, const std::string& json) {
    std::vector<uint8_t> b;
    auto put = [&](const void* p, size_t n) {
        const uint8_t* q = static_cast<const uint8_t*>(p);
        b.insert(b.end(), q, q + n);
    };
    IdentitySnapshotHeader h{};
    h.magic = kIdentityBlockMagic;
    h.version = kIdentityBlockVersionV1;
    h.tUs = tUs;
    h.entrantCount = 1;
    h.contentHash = 0xC0FFEEull;
    put(&h, kIdentityHeaderSizeV1);
    put(&fields, kIdentityEntrantSizeV1);
    for (uint16_t s = 0; s < kIdentitySlotCount; ++s) {
        uint8_t rec[kIdentitySlotStrideV1] = {};
        const uint64_t itemId = kItemBase + s + fields.userSlot;
        memcpy(rec + 0, &s, sizeof s);
        memcpy(rec + 8, &itemId, sizeof itemId);
        put(rec, sizeof rec);
    }
    const uint32_t n = uint32_t(json.size());
    put(&n, sizeof n);
    put(json.data(), n);
    return b;
}

// v2: the key exists, the per-entrant count does not.
std::vector<uint8_t> BuildV2Block(uint64_t tUs, const IdentityEntrant& fields, const std::string& json) {
    std::vector<uint8_t> b;
    auto put = [&](const void* p, size_t n) {
        const uint8_t* q = static_cast<const uint8_t*>(p);
        b.insert(b.end(), q, q + n);
    };
    const uint16_t kV2SlotStride = 24;
    IdentitySnapshotHeader h{};
    h.magic = kIdentityBlockMagic;
    h.version = kIdentityBlockVersionV2;
    h.tUs = tUs;
    h.entrantCount = 1;
    h.contentHash = 0xC0FFEEull;
    h.loadoutSlotStride = kV2SlotStride;
    h.loadoutSlotCount = kIdentitySlotCount;
    put(&h, kIdentityHeaderSizeV2);
    put(&fields, kIdentityEntrantSizeV2);
    for (uint16_t s = 0; s < kIdentitySlotCount; ++s) {
        uint8_t rec[kV2SlotStride] = {};
        const uint64_t itemId = kItemBase + s + fields.userSlot;
        const uint64_t slotKey = kSlotKeyBase + s + fields.userSlot;
        memcpy(rec + 0, &s, sizeof s);
        memcpy(rec + 8, &itemId, sizeof itemId);
        memcpy(rec + 16, &slotKey, sizeof slotKey);
        put(rec, sizeof rec);
    }
    const uint32_t n = uint32_t(json.size());
    put(&n, sizeof n);
    put(json.data(), n);
    return b;
}

struct TempDemo {
    wchar_t path[512];
    TempDemo() {
        wchar_t dir[400];
        const DWORD n = GetTempPathW(400, dir);
        if (!n) { dir[0] = L'.'; dir[1] = 0; }
        _snwprintf_s(path, 512, _TRUNCATE, L"%lsw3e_idn_%lu.echodemo", dir,
                     (unsigned long)GetCurrentProcessId());
    }
    ~TempDemo() { DeleteFileW(path); }
};

template <class T>
void Feed(ChapterBuilder& b, uint16_t kind, const T& payload) {
    uint8_t buf[sizeof(RecHeader) + sizeof(T)];
    RecHeader h{};
    h.kind = kind;
    h.bytes = uint16_t(sizeof(T));
    h.producer = 0;
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, &payload, sizeof payload);
    b.Consume(buf, uint32_t(sizeof buf));
}

// The event row exactly as capture::PushIdentityBlock frames it: the record, then the block VERBATIM.
void FeedEventBody(ChapterBuilder& b, uint64_t stampUs, uint16_t typeIndex,
                   const std::vector<uint8_t>& body) {
    std::vector<uint8_t> buf(sizeof(RecHeader) + sizeof(EventRec) + body.size());
    RecHeader h{};
    h.kind = kRecEvent;
    h.bytes = uint16_t(sizeof(EventRec) + body.size());
    h.producer = 0;
    EventRec r{};
    r.stampUs = stampUs;
    r.dst = 0;
    r.typeIndex = typeIndex;
    r.bodyBytes = uint16_t(body.size());
    memcpy(buf.data(), &h, sizeof h);
    memcpy(buf.data() + sizeof h, &r, sizeof r);
    memcpy(buf.data() + sizeof h + sizeof r, body.data(), body.size());
    b.Consume(buf.data(), uint32_t(buf.size()));
}

// One recording carrying the four blocks below, written through the real container.
//   slot 3: v1 at t0+1s, then v3 at t0+3s     -- the later block must win once the playhead reaches it
//   slot 5: v2 at t0+1s                        -- keys recorded, count not
struct SynthTimes {
    uint64_t t0, early, late, end;
};

SynthTimes WriteSynthDemo(const wchar_t* path) {
    SynthTimes t{};
    t.t0 = kFixtureT0;
    t.early = t.t0 + 1000000;
    t.late = t.t0 + 3000000;
    t.end = t.t0 + 5000000;

    ManifestBuilder mb;
    mb.Begin(t.t0, 0, 0x40);
    const uint8_t md5[16] = {0xca, 0xa0, 0x5e, 0xe3};
    mb.SetBuild(md5, 0x676F6C646D617374ull, 0x0102030405060708ull);
    mb.SetLevel(6300205991959903307ull, 0xF00Dull, 1780177787ull);
    mb.SetKeyframeIntervalUs(2000000);
    mb.SetUuid(1, 2);
    PlayerRecord p{};
    p.xplatformId = 0x1000;
    p.userSlot = 3;
    p.userIndex = 3;
    mb.AddPlayer(p);
    REQUIRE(mb.Refusal() == nullptr);

    DemoWriter writer;
    REQUIRE(writer.Open(path, mb.View()));

    BuilderConfig cfg;
    cfg.keyframeIntervalUs = 2000000;
    for (uint16_t i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;
    for (uint16_t i = 0; i < kSectionCount; ++i) cfg.sectionEnabled[i] = true;

    // The row's index comes from the SAME table the file publishes, so a reordering moves both.
    const uint16_t tapIndex = TapTypeIndex(kIdentitySnapshotTapName);
    REQUIRE(tapIndex != kTapTypeIndexNone);

    ChapterBuilder b;
    REQUIRE(b.Open(&writer, mb.View(), cfg));
    b.EmitMatchBegin(t.t0);

    const std::string jsonA = "{\"profile\":\"A\"}";
    const std::string jsonB = "{\"profile\":\"B\"}";
    const IdentityEntrant a = Person(3, "ALPHA");
    const IdentityEntrant c = Person(5, "BRAVO");

    const uint64_t dt = 11111;
    for (uint32_t f = 0; f * dt <= t.end - t.t0; ++f) {
        const uint64_t st = t.t0 + f * dt;
        TickRec tr{};
        tr.sourceTimeUs = st;
        tr.simTimeUs = 1000000 + f * dt;
        tr.frameIndex = f + 1;
        tr.frameDeltaUs = uint32_t(dt);
        tr.scaledDeltaUs = uint32_t(dt);
        Feed(b, kRecTick, tr);

        if (f == 1) {
            FeedEventBody(b, st, tapIndex, BuildV1Block(t.early, a, jsonA));
            FeedEventBody(b, st, tapIndex, BuildV2Block(t.early, c, jsonB));
        }
        if (st >= t.late && st < t.late + dt) {
            FeedEventBody(b, st, tapIndex, WriteV3Block(t.late, a, jsonA, kIdentityBlockSlotCount));
        }
    }
    b.EmitMatchEnd(t.end);
    b.FlushChapter();
    b.Close();
    writer.Close();
    return t;
}

// The store is 160 KB of arena; one per case, and never two on the stack at once.
RecordedIdentitySource& Store() {
    static RecordedIdentitySource s;
    s.Reset();
    return s;
}

}  // namespace

TEST_CASE("a written .echodemo's identity blocks decode into the recorded loadout contract") {
    TempDemo tmp;
    const SynthTimes t = WriteSynthDemo(tmp.path);

    DemoReader reader;
    REQUIRE(reader.Open(tmp.path));

    RecordedIdentitySource& src = Store();
    const uint32_t stored = ScanRecordedIdentity(reader, src);
    // Three blocks reached disk and three came back: a count of two would mean the row a chapter
    // boundary split was dropped, and reading one entrant's fields would never have shown it.
    CHECK(stored == 3);
    CHECK(src.Decoded() == 3);
    CHECK(src.Refused() == 0);
    CHECK(src.SlotCount() == 2);

    SUBCASE("the v2 block: keys recorded, per-entrant count NOT -- and the two are separate facts") {
        src.SetHorizonUs(t.late + 1);
        RecordedLoadout rec;
        const char* why = "unset";
        REQUIRE(ReadRecordedLoadout(src, 5, rec, &why));
        CHECK(why == nullptr);
        CHECK(rec.hasSlotKeys);
        CHECK(rec.filledPairCount == kRecordedPairCountUnknown);
        CHECK(rec.filledPairCount != 0);
        for (uint16_t i = 0; i < kRecordedLoadoutPairCap; ++i) {
            CHECK(rec.pairs[i].slotKey == kSlotKeyBase + i + 5);
            CHECK(rec.pairs[i].itemId == kItemBase + i + 5);
        }
        CHECK(rec.profile == RecordedProfileState::Present);
        REQUIRE(rec.document != nullptr);
        CHECK(rec.documentBytes == 15);
        CHECK(memcmp(rec.document, "{\"profile\":\"B\"}", rec.documentBytes) == 0);
    }

    SUBCASE("the v1 block: no key is recorded, and none is invented from a position") {
        // The playhead sits between the two blocks slot 3 has, so the v1 one is the live answer.
        src.SetHorizonUs(t.late - 1);
        RecordedLoadout rec;
        const char* why = "unset";
        REQUIRE(ReadRecordedLoadout(src, 3, rec, &why));
        CHECK_FALSE(rec.hasSlotKeys);
        CHECK(rec.filledPairCount == kRecordedPairCountUnknown);
        for (uint16_t i = 0; i < kRecordedLoadoutPairCap; ++i) {
            // Not the position, and not the item either: a v1 record's pairs are simply absent.
            CHECK(rec.pairs[i].slotKey == 0);
            CHECK(rec.pairs[i].itemId == 0);
        }
        CHECK(rec.profile == RecordedProfileState::Present);
    }

    SUBCASE("the later block wins once the playhead reaches it, and only then") {
        RecordedLoadout before, after;
        const char* why = nullptr;
        src.SetHorizonUs(t.late - 1);
        REQUIRE(ReadRecordedLoadout(src, 3, before, &why));
        src.SetHorizonUs(t.late);
        REQUIRE(ReadRecordedLoadout(src, 3, after, &why));

        CHECK_FALSE(before.hasSlotKeys);                                  // v1
        CHECK(after.hasSlotKeys);                                         // v3
        CHECK(after.filledPairCount == kIdentityBlockSlotCount);
        CHECK(before.filledPairCount != after.filledPairCount);
        CHECK(after.pairs[0].slotKey == kSlotKeyBase + 3);
        CHECK(after.pairs[19].slotKey == kSlotKeyBase + 19 + 3);
        CHECK(after.pairs[0].itemId == kItemBase + 3);

        // And BACKWARDS: a seek before the change answers the old block again, because the read is
        // a search over what the file holds and not a state that was advanced past.
        RecordedLoadout back;
        src.SetHorizonUs(t.late - 1);
        REQUIRE(ReadRecordedLoadout(src, 3, back, &why));
        CHECK_FALSE(back.hasSlotKeys);
    }

    SUBCASE("an entrant recorded only LATER is not the same absence as one never recorded") {
        src.SetHorizonUs(t.t0);
        RecordedLoadout rec;
        const char* why = nullptr;
        CHECK_FALSE(ReadRecordedLoadout(src, 3, rec, &why));
        REQUIRE(why != nullptr);
        CHECK(strcmp(why, "RecordedBlockNotYetReached") == 0);
        // ... and the entrant the file never carried still says THAT, at the same instant.
        const char* other = nullptr;
        CHECK_FALSE(ReadRecordedLoadout(src, 9, rec, &other));
        REQUIRE(other != nullptr);
        CHECK(strcmp(other, "EntrantNotInBlock") == 0);
    }

    SUBCASE("an entrant the file never carried is not a silent empty loadout") {
        src.SetHorizonUs(t.end);
        RecordedLoadout rec;
        const char* why = nullptr;
        CHECK_FALSE(ReadRecordedLoadout(src, 9, rec, &why));
        REQUIRE(why != nullptr);
        CHECK(strcmp(why, "EntrantNotInBlock") == 0);
        CHECK(rec.filledPairCount == kRecordedPairCountUnknown);
        CHECK_FALSE(rec.hasSlotKeys);
    }
}

TEST_CASE("a block laid out by a NEWER build is refused whole, and the refusal reaches the caller") {
    const IdentityEntrant a = Person(7, "FUTURE");
    std::vector<uint8_t> block = WriteV3Block(kFixtureT0, a, "{}", kIdentityBlockSlotCount);
    const uint32_t newer = kIdentityBlockVersion + 1;
    memcpy(block.data() + offsetof(IdentitySnapshotHeader, version), &newer, sizeof newer);

    RecordedIdentitySource& src = Store();
    CHECK_FALSE(src.Offer(block.data(), uint32_t(block.size())));
    CHECK(src.Decoded() == 0);
    CHECK(src.Refused() == 1);
    REQUIRE(src.FirstRefusal() != nullptr);
    CHECK(strcmp(src.FirstRefusal(), "IdentityBlockUnknownVersion") == 0);

    src.SetHorizonUs(kFixtureT0 + 1);
    RecordedLoadout rec;
    const char* why = nullptr;
    CHECK_FALSE(ReadRecordedLoadout(src, 7, rec, &why));
    REQUIRE(why != nullptr);
    CHECK(strcmp(why, "IdentityBlockRefused:IdentityBlockUnknownVersion") == 0);
    CHECK_FALSE(rec.hasSlotKeys);
    CHECK(rec.filledPairCount == kRecordedPairCountUnknown);
}

TEST_CASE("a file with no identity block answers IdentityBlockNotRecorded, not an empty loadout") {
    RecordedIdentitySource& src = Store();
    src.SetHorizonUs(kFixtureT0);
    RecordedLoadout rec;
    const char* why = nullptr;
    CHECK_FALSE(ReadRecordedLoadout(src, 0, rec, &why));
    REQUIRE(why != nullptr);
    CHECK(strcmp(why, "IdentityBlockNotRecorded") == 0);
}

// ---------------------------------------------------------------------------------------------
// The real captures. `.echodemo` is gitignored (recorded sessions carry the owner's own config), so
// the cases say where they looked when a file is not there instead of passing quietly.
//
// A ZERO HERE HAS THREE CAUSES and they are three different repairs: the file carries no identity
// block, its tap table never named the row, or its event stream did not decode. The scan records all
// three, and the control below asserts the SECOND and THIRD before reading anything into the first --
// otherwise "no blocks" is an absence of evidence about this reader dressed as a fact about the file.
// ---------------------------------------------------------------------------------------------
namespace {

const wchar_t kL3Capture[] = L"2026-09-05_23-52-34_576ED3F8428EBC4B_000000007E1CAF73.echodemo";

void ReportCapture(const wchar_t* path, const char* label) {
    DemoReader reader;
    if (!reader.Open(path)) {
        WARN_MESSAGE(false, "capture not on this machine; not exercised against real bytes");
        return;
    }
    RecordedIdentitySource& src = Store();
    const uint32_t stored = ScanRecordedIdentity(reader, src);
    printf("[identity] %s: %u event row(s), %u chapter(s) with a tap table, identity tap index %d, "
           "%u block(s) offered, %u stored, %u refused, %u slot(s)\n",
           label, src.EventRows(), src.ChaptersWithTapTable(),
           src.TapIndex() == replay::identity::kRecordedTapIndexNone ? -1 : (int)src.TapIndex(),
           src.Offered(), stored, src.Refused(), (unsigned)src.SlotCount());

    // The probe, proven before its zero is trusted: real bytes reached the point at which an
    // identity row would have been recognised.
    CHECK(src.EventRows() > 0);
    CHECK(src.ChaptersWithTapTable() > 0);
    CHECK(src.TapIndex() != replay::identity::kRecordedTapIndexNone);
    CHECK(src.Refused() == 0);

    if (src.SlotCount() == 0) {
        printf("[identity] %s carries no identity block: its %u event rows are other tap types\n",
               label, src.EventRows());
        return;
    }
    src.SetHorizonUs(UINT64_MAX);
    for (uint16_t i = 0; i < src.SlotCount(); ++i) {
        const uint32_t slot = src.SlotAt(i);
        RecordedLoadout rec;
        const char* why = nullptr;
        REQUIRE(ReadRecordedLoadout(src, slot, rec, &why));
        const replay::identity::RecordedIdentityEntry* e = src.Current(slot);
        REQUIRE(e != nullptr);
        char pairs[16];
        if (rec.filledPairCount == kRecordedPairCountUnknown) {
            _snprintf_s(pairs, sizeof pairs, _TRUNCATE, "UNKNOWN");
        } else {
            _snprintf_s(pairs, sizeof pairs, _TRUNCATE, "%u", (unsigned)rec.filledPairCount);
        }
        printf("[identity] recorded loadout slot %u: v%u pairs %s/%u keys %s profile %s\n",
               slot, (unsigned)e->version, pairs, (unsigned)kRecordedLoadoutPairCap,
               rec.hasSlotKeys ? "yes" : "no",
               rec.profile == RecordedProfileState::Present    ? "present"
               : rec.profile == RecordedProfileState::Absent   ? "absent"
               : rec.profile == RecordedProfileState::Refused  ? "refused"
                                                               : "not asked");
        // The two facts are asserted against the version the FILE carries, never against the one
        // this case hoped for.
        CHECK(rec.hasSlotKeys == (e->version >= kIdentityBlockVersionSlotKeys));
        CHECK((rec.filledPairCount != kRecordedPairCountUnknown) ==
              (e->version >= kIdentityBlockVersionPairCount));
    }
}

// The most recent recording in the owner's capture directory: the one written by the newest build,
// and therefore the only one that can carry a block from an emitter that landed after the pinned
// file was recorded.
bool NewestCapture(wchar_t* out, size_t chars) {
    const wchar_t* home = _wgetenv(L"USERPROFILE");
    if (!home) return false;
    wchar_t glob[1024];
    _snwprintf_s(glob, _TRUNCATE, L"%ls/Documents/EchoReplay/*.echodemo", home);
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(glob, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    wchar_t best[MAX_PATH] = {};
    do {
        if (wcscmp(fd.cFileName, best) > 0) wcscpy_s(best, fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!best[0]) return false;
    _snwprintf_s(out, chars, _TRUNCATE, L"%ls/Documents/EchoReplay/%ls", home, best);
    return true;
}

}  // namespace

TEST_CASE("the L3 capture is read for identity blocks, and the reader's reach is proven first") {
    const wchar_t* home = _wgetenv(L"USERPROFILE");
    REQUIRE(home != nullptr);
    wchar_t path[1024];
    _snwprintf_s(path, _TRUNCATE, L"%ls/Documents/EchoReplay/%ls", home, kL3Capture);
    ReportCapture(path, "L3 2026-09-05_23-52-34");
}

TEST_CASE("the newest real capture is read for identity blocks") {
    wchar_t path[1024];
    if (!NewestCapture(path, 1024)) {
        WARN_MESSAGE(false, "no .echodemo in the capture directory");
        return;
    }
    ReportCapture(path, "newest capture");
}

// ---------------------------------------------------------------------------------------------
// F3, F5: the census that can tell a blind reader from an empty file, the per-entrant
// bounds that refuse instead of going stale, and the refusal that does not speak for everybody.
// ---------------------------------------------------------------------------------------------
namespace {

void OfferBlocks(RecordedIdentitySource& src, uint16_t slot, uint32_t n, uint64_t firstUs) {
    for (uint32_t i = 0; i < n; ++i) {
        const std::vector<uint8_t> b =
            WriteV3Block(firstUs + i, Person(slot, "PLAYER"), "{}", kIdentityBlockSlotCount);
        src.Offer(b.data(), uint32_t(b.size()));
    }
}

}  // namespace

TEST_CASE("the census tells a reader that saw nothing from a file that holds nothing") {
    RecordedIdentitySource& src = Store();
    char blind[RecordedIdentitySource::kCensusChars];
    char sighted[RecordedIdentitySource::kCensusChars];

    // A reader that never resolved the tap row: no rows scanned, no table, no index.
    src.NoteScan(0, 0, replay::identity::kRecordedTapIndexNone);
    replay::identity::RecordedIdentityCensusLine(src, blind, sizeof blind);
    // A reader that walked 8488 real rows in 70 chapters and found the row at index 37 -- and the
    // file still carried no identity block. Same zero, completely different repair.
    src.NoteScan(8488, 70, 37);
    replay::identity::RecordedIdentityCensusLine(src, sighted, sizeof sighted);

    CHECK(strcmp(blind, sighted) != 0);
    CHECK(strstr(blind, "rows 0") != nullptr);
    CHECK(strstr(blind, "TapRowAbsent") != nullptr);
    CHECK(strstr(sighted, "rows 8488") != nullptr);
    CHECK(strstr(sighted, "tap index 37") != nullptr);
    // And the scan leaves it on the source, so a shipped caller has one string to print.
    CHECK(src.EventRows() == 8488);
}

TEST_CASE("forty blocks over three entrants: the latest wins, evictions are named, none goes stale") {
    RecordedIdentitySource& src = Store();
    OfferBlocks(src, 0, 13, kFixtureT0 + 1);
    OfferBlocks(src, 1, 13, kFixtureT0 + 100);
    OfferBlocks(src, 2, 14, kFixtureT0 + 200);
    CHECK(src.Offered() == 40);
    CHECK(src.Decoded() == 40);
    CHECK(src.Refused() == 0);
    CHECK(src.SlotCount() == 3);
    CHECK(src.Evicted() == (13 - replay::identity::kRecordedBlocksPerEntrant) * 2 +
                               (14 - replay::identity::kRecordedBlocksPerEntrant));

    src.SetHorizonUs(UINT64_MAX);
    RecordedLoadout rec;
    const char* why = "unset";
    REQUIRE(ReadRecordedLoadout(src, 2, rec, &why));
    CHECK(why == nullptr);
    const replay::identity::RecordedIdentityEntry* e = src.Current(2);
    REQUIRE(e != nullptr);
    CHECK(e->tUs == kFixtureT0 + 200 + 13);   // the LAST block offered for that entrant

    // A seek back past the evicted blocks refuses by name rather than answering with a block that is
    // no longer the latest one before the playhead.
    src.SetHorizonUs(kFixtureT0);
    const char* back = nullptr;
    CHECK_FALSE(ReadRecordedLoadout(src, 2, rec, &back));
    REQUIRE(back != nullptr);
    CHECK(strcmp(back, "IdentityBlockRefused:RecordedBlockEvicted") == 0);
}

TEST_CASE("eight entrants, nine changes each: every one serves its latest at every held instant") {
    RecordedIdentitySource& src = Store();
    const uint16_t people = 8;
    const uint32_t changes = replay::identity::kRecordedBlocksPerEntrant + 1;
    for (uint16_t s = 0; s < people; ++s) OfferBlocks(src, s, changes, kFixtureT0 + s * 100);

    // THE POOL IS entrants x depth, so nobody is denied a cell an entrant with room will never get
    // back: every block is decoded and the only losses are the ring's own, counted, evictions.
    CHECK(src.Decoded() == people * changes);
    CHECK(src.Refused() == 0);
    CHECK(src.SlotCount() == people);
    CHECK(src.Evicted() == people * (changes - replay::identity::kRecordedBlocksPerEntrant));

    for (uint16_t p = 0; p < people; ++p) {
        const uint64_t base = kFixtureT0 + p * 100;
        for (uint32_t k = 1; k < changes; ++k) {
            src.SetHorizonUs(base + k);
            RecordedLoadout rec;
            const char* why = "unset";
            REQUIRE(ReadRecordedLoadout(src, p, rec, &why));
            CHECK(why == nullptr);
            const replay::identity::RecordedIdentityEntry* e = src.Current(p);
            REQUIRE(e != nullptr);
            CHECK(e->tUs == base + k);
            CHECK(rec.hasSlotKeys);
        }
        // The one block the ring evicted is the only instant that refuses, and it says which.
        src.SetHorizonUs(base);
        RecordedLoadout rec;
        const char* why = nullptr;
        CHECK_FALSE(ReadRecordedLoadout(src, p, rec, &why));
        REQUIRE(why != nullptr);
        CHECK(strcmp(why, "IdentityBlockRefused:RecordedBlockEvicted") == 0);
    }
}

TEST_CASE("past the entrant table a dropped entrant is NAMED, not reported as never recorded") {
    RecordedIdentitySource& src = Store();
    const uint16_t cap = replay::identity::kRecordedEntrantCap;
    for (uint16_t s = 0; s < cap; ++s) OfferBlocks(src, s, 1, kFixtureT0 + s);
    CHECK(src.Refused() == 0);
    CHECK(src.SlotCount() == cap);

    OfferBlocks(src, cap, 1, kFixtureT0 + cap);   // one entrant more than the format can hold
    CHECK(src.Refused() == 1);
    REQUIRE(src.FirstRefusal() != nullptr);
    CHECK(strcmp(src.FirstRefusal(), "RecordedEntrantTableFull") == 0);
    CHECK(src.EntrantTableFull());

    src.SetHorizonUs(UINT64_MAX);
    RecordedLoadout rec;
    const char* why = nullptr;
    CHECK_FALSE(ReadRecordedLoadout(src, cap, rec, &why));
    REQUIRE(why != nullptr);
    CHECK(strcmp(why, "IdentityBlockRefused:RecordedEntrantTableFull") == 0);

    // Before the table filled, an unknown slot is still simply not in any block.
    src.SetHorizonUs(kFixtureT0);
    const char* early = nullptr;
    CHECK_FALSE(ReadRecordedLoadout(src, cap, rec, &early));
    REQUIRE(early != nullptr);
    CHECK(strcmp(early, "EntrantNotInBlock") == 0);
}

TEST_CASE("one refused block does not relabel an entrant the file simply never carried") {
    RecordedIdentitySource& src = Store();
    OfferBlocks(src, 3, 1, kFixtureT0 + 1);
    std::vector<uint8_t> bad = WriteV3Block(kFixtureT0 + 100, Person(4, "FUTURE"), "{}", kIdentityBlockSlotCount);
    const uint32_t newer = kIdentityBlockVersion + 1;
    memcpy(bad.data() + offsetof(IdentitySnapshotHeader, version), &newer, sizeof newer);
    CHECK_FALSE(src.Offer(bad.data(), uint32_t(bad.size())));
    CHECK(src.Decoded() == 1);
    CHECK(src.Refused() == 1);

    src.SetHorizonUs(UINT64_MAX);
    RecordedLoadout rec;
    const char* why = nullptr;
    CHECK_FALSE(ReadRecordedLoadout(src, 9, rec, &why));
    REQUIRE(why != nullptr);
    // Blocks for other entrants decoded fine, so the bad one is not this entrant's story.
    CHECK(strcmp(why, "EntrantNotInBlock") == 0);
}
