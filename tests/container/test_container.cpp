#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include <doctest.h>

#include "foundation/demofile/demofile.h"
#include "foundation/demofile/format.h"
#include "foundation/demofile/manifest_io.h"
#include "foundation/ring/alloc_trap.h"
#include "foundation/ring/ring.h"
#include "synthcap.h"
#include "../local_inputs.h"

using namespace replay;

namespace {

std::wstring ScratchDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, buf);
    std::wstring dir(buf, n);
    dir += L"echodemo_w1b\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring Scratch(const wchar_t* name) {
    static const std::wstring dir = ScratchDir();
    return dir + name;
}

std::wstring SiblingExe(const wchar_t* name) {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    const size_t cut = p.find_last_of(L"\\/");
    return p.substr(0, cut + 1) + name;
}

bool ReadWhole(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    const bool ok = out.empty() || (ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
    CloseHandle(h);
    return ok;
}

bool WriteWhole(const std::wstring& path, const uint8_t* p, size_t n) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD done = 0;
    const bool ok = n == 0 || (WriteFile(h, p, (DWORD)n, &done, nullptr) && done == n);
    CloseHandle(h);
    return ok;
}

struct FrameSpan {
    uint64_t offset;
    ChapterHeadDisk head;
};

// Walks the raw bytes rather than the reader, so a test can damage a chapter the reader has not seen.
std::vector<FrameSpan> WalkFrames(const std::vector<uint8_t>& file) {
    std::vector<FrameSpan> out;
    uint64_t at = kFileHeaderBytes;
    while (at + kChapterHeadBytes <= file.size()) {
        ChapterHeadDisk h{};
        memcpy(&h, file.data() + at, sizeof(h));
        if (h.magic != kChapterMagic || Crc32(&h, kChapterHeadCrcCover) != h.headCrc32 ||
            at + kChapterHeadBytes + h.payloadLen > file.size()) {
            break;
        }
        out.push_back(FrameSpan{at, h});
        at += kChapterHeadBytes + h.payloadLen;
    }
    return out;
}

void RewriteHead(std::vector<uint8_t>& file, const FrameSpan& f, ChapterHeadDisk h) {
    h.headCrc32 = Crc32(&h, kChapterHeadCrcCover);
    memcpy(file.data() + f.offset, &h, sizeof(h));
}

struct OpenOutcome {
    bool opened = false;
    size_t chapters = 0;
    uint32_t decodable = 0;
    uint32_t resyncs = 0;
    std::vector<ChapterFault> faults;
};

OpenOutcome Probe(const std::wstring& path) {
    OpenOutcome o;
    DemoReader r;
    if (!r.Open(path.c_str())) {
        return o;
    }
    o.opened = true;
    o.chapters = r.ChapterCount();
    o.resyncs = r.Stats().resyncs;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        DecodedChapter c{};
        if (r.DecodeChapter(i, c)) {
            ++o.decodable;
            o.faults.push_back(ChapterFault::None);
        } else {
            o.faults.push_back(r.LastFault());
        }
    }
    return o;
}

synth::Config SmallConfig() {
    synth::Config c;
    c.chapters = 8;
    c.ticksPerChapter = 44;
    c.playerCount = 4;
    return c;
}

}  // namespace

TEST_CASE("on-disk layout is pinned by size and offset") {
    CHECK(sizeof(FileHeaderDisk) == 128);
    CHECK(sizeof(ChapterHeadDisk) == 48);
    CHECK(offsetof(ChapterHeadDisk, headCrc32) == 44);
    CHECK(sizeof(StreamBlockDisk) == 16);
    CHECK(sizeof(IndexEntryDisk) == 32);
    CHECK(sizeof(FileFooterDisk) == 48);
    CHECK(kMaxChapterPayload == (1u << 20));
    CHECK(static_cast<uint8_t>(FieldClass::WireQ) == 0);
    CHECK(static_cast<uint8_t>(FieldClass::WireQuat) == 3);
    CHECK(static_cast<uint8_t>(Retention::All) == 0);
    CHECK(static_cast<uint8_t>(Retention::Edge) == 1);
}

TEST_CASE("crc32 matches the known IEEE vector") {
    CHECK(Crc32("123456789", 9) == 0xCBF43926u);
    CHECK(Crc32("", 0) == 0u);
}

// ---- the SPSC ring -------------------------------------------------------------------------------

TEST_CASE("ring frames records and preserves order across the wrap") {
    std::vector<uint8_t> mem(1024);
    SpscRing ring;
    REQUIRE(ring.Init(mem.data(), 1024));
    CHECK_FALSE(ring.Init(mem.data(), 1000));  // not a power of two
    CHECK_FALSE(ring.Init(nullptr, 1024));

    uint8_t rec[100];
    uint8_t got[128];
    uint32_t n = 0;
    for (int round = 0; round < 40; ++round) {
        for (uint8_t i = 0; i < sizeof(rec); ++i) {
            rec[i] = static_cast<uint8_t>(round * 7 + i);
        }
        REQUIRE(ring.Push(rec, sizeof(rec)));
        CHECK(ring.PeekSize() == sizeof(rec));
        REQUIRE(ring.Pop(got, sizeof(got), &n));
        CHECK(n == sizeof(rec));
        CHECK(memcmp(rec, got, sizeof(rec)) == 0);
    }
    CHECK(ring.BytesUsed() == 0);
    CHECK(ring.PeekSize() == 0);
    CHECK(ring.DroppedRecords() == 0);
    CHECK(ring.PushedRecords() == 40);
}

TEST_CASE("a full ring refuses whole records and counts them") {
    std::vector<uint8_t> mem(256);
    SpscRing ring;
    REQUIRE(ring.Init(mem.data(), 256));

    uint8_t rec[60] = {};
    uint32_t pushed = 0;
    while (ring.Push(rec, sizeof(rec))) {
        ++pushed;
    }
    CHECK(pushed == 4);  // 4 x (60 + 4) == 256, filled exactly
    CHECK(ring.BytesUsed() == 256);
    CHECK(ring.DroppedRecords() == 1);
    CHECK(ring.DroppedBytes() == sizeof(rec));

    uint8_t got[64];
    uint32_t n = 0;
    CHECK(ring.Pop(got, sizeof(got), &n));
    CHECK(ring.Push(rec, sizeof(rec)));

    // A record that can never fit is refused rather than truncated.
    std::vector<uint8_t> huge(300);
    CHECK_FALSE(ring.Push(huge.data(), (uint32_t)huge.size()));
    CHECK(ring.DroppedRecords() == 2);
}

TEST_CASE("Pop reports the size it could not deliver instead of looking empty") {
    std::vector<uint8_t> mem(256);
    SpscRing ring;
    REQUIRE(ring.Init(mem.data(), 256));
    uint8_t rec[40] = {1, 2, 3};
    REQUIRE(ring.Push(rec, sizeof(rec)));

    uint8_t small[8];
    uint32_t n = 0;
    CHECK_FALSE(ring.Pop(small, sizeof(small), &n));
    CHECK(n == sizeof(rec));  // still queued, and the caller knows how big it is

    uint8_t big[64];
    CHECK(ring.Pop(big, sizeof(big), &n));
    CHECK(n == sizeof(rec));
}

TEST_CASE("the operator-new half of the trap is measured on its own, not asserted") {
    // Installed alone, so nothing else in the trap can account for what fires. This is the control the
    // full-trap test cannot give: with kTrapHeap armed, `new` would count an allocation through the
    // HeapAlloc detour even if the operator-new replacement were absent from the image.
    const uint32_t live = TrapInstall(kTrapNew);
    REQUIRE(live == kTrapNew);

    TrapReport fromNew;
    {
        TrapGuard guard;
        int* p = new int(3);
        delete p;
        fromNew = TrapTake();
    }
    CHECK(fromNew.allocations == 1);
    REQUIRE(fromNew.firstSite != nullptr);
    CHECK(std::string(fromNew.firstSite) == "operator new");

    // malloc reaches the heap, not operator new, and no heap detour is installed: it must be invisible.
    TrapReport fromMalloc;
    {
        TrapGuard guard;
        void* q = malloc(32);
        free(q);
        fromMalloc = TrapTake();
    }
    CHECK(fromMalloc.allocations == 0);
    CHECK(fromMalloc.firstSite == nullptr);

    TrapUninstall();
    CHECK(TrapLiveSites() == 0);
}

TEST_CASE("the ring's producer path allocates nothing, locks nothing and makes no syscall") {
    const uint32_t want = kTrapNew | kTrapHeap | kTrapLock | kTrapFile;
    const uint32_t live = TrapInstall(want);
    // A trap with a site missing is a trap that silently cannot fire, so the verdict is refused rather
    // than weakened.
    REQUIRE(live == want);

    std::vector<uint8_t> mem(1 << 16);
    SpscRing ring;
    REQUIRE(ring.Init(mem.data(), 1 << 16));
    uint8_t rec[68];
    memset(rec, 0xA5, sizeof(rec));

    TrapReport clean;
    {
        TrapGuard guard;
        for (int i = 0; i < 5000; ++i) {
            ring.Push(rec, sizeof(rec));
            uint32_t n = 0;
            uint8_t out[80];
            ring.Pop(out, sizeof(out), &n);
        }
        clean = TrapTake();
    }
    CHECK(clean.allocations == 0);
    CHECK(clean.locks == 0);
    CHECK(clean.fileOps == 0);
    CHECK(clean.firstSite == nullptr);

    // The mutation: the same guarded region with one deliberate allocation in it. A trap that cannot be
    // shown firing is worth nothing.
    TrapReport dirty;
    {
        TrapGuard guard;
        ring.Push(rec, sizeof(rec));
        volatile void* leak = malloc(64);
        free(const_cast<void*>(leak));
        dirty = TrapTake();
    }
    CHECK(dirty.allocations >= 1);
    REQUIRE(dirty.firstSite != nullptr);
    CHECK(std::string(dirty.firstSite) == "HeapAlloc");  // malloc reaches the detour, not operator new

    // ... and once more for the C++ half, which is a link-time replacement rather than a detour.
    TrapReport dirtyNew;
    {
        TrapGuard guard;
        int* p = new int(7);
        delete p;
        dirtyNew = TrapTake();
    }
    CHECK(dirtyNew.allocations >= 1);
    REQUIRE(dirtyNew.firstSite != nullptr);
    CHECK(std::string(dirtyNew.firstSite) == "operator new");  // the replacement, not the detour

    // Disarmed threads are unaffected, or the trap would be unusable in a real process.
    CHECK_FALSE(TrapArmedHere());
    void* q = malloc(16);
    free(q);
    const TrapReport idle = TrapTake();
    CHECK(idle.allocations == 0);

    TrapUninstall();
}

// ---- round trip ----------------------------------------------------------------------------------

TEST_CASE("a clean capture round-trips byte for byte") {
    const std::wstring path = Scratch(L"roundtrip.echodemo");
    const synth::Config cfg = SmallConfig();
    WriterStats stats{};
    uint32_t chapters = 0;
    REQUIRE(synth::Write(path.c_str(), cfg, &stats, &chapters));
    CHECK(stats.observedBelowStored == 0);
    CHECK(stats.unknownSchemaBlocks == 0);
    CHECK(stats.oversizeBlocksRefused == 0);
    CHECK(stats.blocksOutsideChapter == 0);
    CHECK(stats.gapRecordsEmitted == 0);

    std::string err;
    const uint64_t records = synth::VerifyRoundTrip(path.c_str(), cfg, &err);
    INFO(err);
    CHECK(err.empty());
    CHECK(records > 0);

    DemoReader r;
    REQUIRE(r.Open(path.c_str()));
    CHECK_FALSE(r.WasRecovered());
    CHECK(r.Stats().fastPath);
    CHECK(r.Stats().footerPresent);
    CHECK((r.FileFlags() & kFlagOpen) == 0);
    CHECK((r.FileFlags() & kFlagHasVoip) != 0);
    CHECK(r.GetManifest().roundCount == 2);  // one S_MARK every fourth chapter of eight

    DecodedChapter past{};
    CHECK_FALSE(r.DecodeChapter(r.ChapterCount(), past));
    CHECK(r.LastFault() == ChapterFault::OutOfRange);
}

TEST_CASE("the index seeks by time within a section") {
    const std::wstring path = Scratch(L"seek.echodemo");
    const synth::Config cfg = SmallConfig();
    REQUIRE(synth::Write(path.c_str(), cfg, nullptr, nullptr));

    DemoReader r;
    REQUIRE(r.Open(path.c_str()));
    const uint64_t mid = cfg.t0Us + 3 * cfg.chapterSpanUs + 1000;
    const size_t core = r.FindChapter(mid, kSectionCore);
    REQUIRE(core != (size_t)-1);
    const ChapterKey k = r.ChapterAt(core);
    CHECK(k.section == kSectionCore);
    CHECK(k.tBeginUs <= mid);
    CHECK(k.tEndUs >= mid);

    CHECK(r.FindChapter(cfg.t0Us - 1, kSectionCore) == (size_t)-1);
    CHECK(r.FindChapter(mid, 999) == (size_t)-1);
    const size_t voip = r.FindChapter(mid, kSectionVoip);
    REQUIRE(voip != (size_t)-1);
    CHECK(r.ChapterAt(voip).section == kSectionVoip);
}

// ---- corruption and truncation (S5.8) --------------------------------------------------------------

TEST_CASE("a corrupted chapter payload skips exactly that chapter") {
    const std::wstring src = Scratch(L"corrupt_src.echodemo");
    const std::wstring dst = Scratch(L"corrupt.echodemo");
    REQUIRE(synth::Write(src.c_str(), SmallConfig(), nullptr, nullptr));

    std::vector<uint8_t> file;
    REQUIRE(ReadWhole(src, file));
    const std::vector<FrameSpan> frames = WalkFrames(file);
    REQUIRE(frames.size() > 6);

    const OpenOutcome base = Probe(src);
    REQUIRE(base.opened);

    const size_t victim = 4;
    file[static_cast<size_t>(frames[victim].offset) + kChapterHeadBytes + 3] ^= 0xFF;
    REQUIRE(WriteWhole(dst, file.data(), file.size()));

    const OpenOutcome o = Probe(dst);
    REQUIRE(o.opened);
    CHECK(o.chapters == base.chapters);           // not fewer: the index still lists it
    CHECK(o.decodable == base.decodable - 1);     // not more: exactly one chapter is lost
    CHECK(o.faults[victim] == ChapterFault::PayloadCrc);
    for (size_t i = 0; i < o.faults.size(); ++i) {
        if (i != victim) {
            CHECK(o.faults[i] == ChapterFault::None);
        }
    }
}

TEST_CASE("a corrupted chapter head costs one chapter, not the rest of the file") {
    const std::wstring src = Scratch(L"head_src.echodemo");
    const std::wstring dst = Scratch(L"head.echodemo");
    REQUIRE(synth::Write(src.c_str(), SmallConfig(), nullptr, nullptr));

    std::vector<uint8_t> file;
    REQUIRE(ReadWhole(src, file));
    const std::vector<FrameSpan> frames = WalkFrames(file);
    REQUIRE(frames.size() > 6);
    const OpenOutcome base = Probe(src);

    // Damage the head and blind the trailer, so the reader must scan and meet the damage head-on.
    const size_t victim = 5;
    file[static_cast<size_t>(frames[victim].offset) + 6] ^= 0x5A;
    FileHeaderDisk h{};
    memcpy(&h, file.data(), sizeof(h));
    h.flags |= kFlagOpen;
    h.trailerOffset = 0;
    h.headerCrc32 = HeaderCrc(h);
    memcpy(file.data(), &h, sizeof(h));
    REQUIRE(WriteWhole(dst, file.data(), file.size()));

    const OpenOutcome o = Probe(dst);
    REQUIRE(o.opened);
    CHECK(o.resyncs == 1);
    CHECK(o.chapters == base.chapters - 1);
    CHECK(o.decodable == o.chapters);
}

TEST_CASE("a file whose close never happened opens by scan with every complete chapter") {
    const std::wstring src = Scratch(L"open_src.echodemo");
    const std::wstring dst = Scratch(L"open.echodemo");
    REQUIRE(synth::Write(src.c_str(), SmallConfig(), nullptr, nullptr));

    std::vector<uint8_t> file;
    REQUIRE(ReadWhole(src, file));
    const std::vector<FrameSpan> frames = WalkFrames(file);
    REQUIRE(!frames.empty());

    // Cut everything from the trailer onward and put the header back into the state a crash leaves it.
    size_t cut = file.size();
    for (const FrameSpan& f : frames) {
        if (f.head.section == kSectionIndex) {
            cut = static_cast<size_t>(f.offset);
            break;
        }
    }
    file.resize(cut);
    FileHeaderDisk h{};
    memcpy(&h, file.data(), sizeof(h));
    h.flags |= kFlagOpen;
    h.trailerOffset = 0;
    h.headerCrc32 = HeaderCrc(h);
    memcpy(file.data(), &h, sizeof(h));
    REQUIRE(WriteWhole(dst, file.data(), file.size()));

    DemoReader r;
    REQUIRE(r.Open(dst.c_str()));
    CHECK(r.WasRecovered());
    CHECK_FALSE(r.Stats().fastPath);
    CHECK(r.ChapterCount() == frames.size() - 1);  // every frame but the trailer we removed
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        DecodedChapter c{};
        CHECK(r.DecodeChapter(i, c));
    }
    CHECK(r.GetManifest().playerCount == SmallConfig().playerCount);
}

TEST_CASE("truncation at every frame boundary class loses at most the torn chapter") {
    const std::wstring src = Scratch(L"trunc_src.echodemo");
    const std::wstring dst = Scratch(L"trunc.echodemo");
    REQUIRE(synth::Write(src.c_str(), SmallConfig(), nullptr, nullptr));

    std::vector<uint8_t> file;
    REQUIRE(ReadWhole(src, file));
    const std::vector<FrameSpan> frames = WalkFrames(file);
    REQUIRE(frames.size() > 4);

    uint32_t cases = 0;
    for (const FrameSpan& f : frames) {
        if (f.head.section == kSectionIndex) {
            continue;
        }
        const size_t base = static_cast<size_t>(f.offset);
        const size_t cuts[] = {
            base,                                            // exactly at the frame start
            base + 1,                                        // one byte into the head
            base + 24,                                       // mid-head
            base + kChapterHeadBytes - 1,                    // head all but its last byte
            base + kChapterHeadBytes,                        // head complete, payload absent
            base + kChapterHeadBytes + f.head.payloadLen / 2,  // mid-payload
            base + kChapterHeadBytes + f.head.payloadLen - 1,  // payload all but its last byte
        };
        for (size_t cut : cuts) {
            if (cut < kFileHeaderBytes || cut > file.size()) {
                continue;
            }
            REQUIRE(WriteWhole(dst, file.data(), cut));
            const OpenOutcome o = Probe(dst);
            ++cases;
            if (!o.opened) {
                // Only legal while the MANIFEST chapter is itself incomplete.
                CHECK(cut <= frames[0].offset + kChapterHeadBytes + frames[0].head.payloadLen);
                continue;
            }
            CHECK(o.decodable == o.chapters);
            // Every frame that ended before the cut must still be there.
            size_t whole = 0;
            for (const FrameSpan& g : frames) {
                if (g.head.section == kSectionIndex) {
                    continue;
                }
                if (g.offset + kChapterHeadBytes + g.head.payloadLen <= cut) {
                    ++whole;
                }
            }
            CHECK(o.chapters == whole);
        }
    }
    CHECK(cases >= 40);
    MESSAGE("truncation cases exercised: " << cases);
}

TEST_CASE("a head whose lengths a chapter could never have is refused before it sizes anything") {
    const std::wstring src = Scratch(L"len_src.echodemo");
    const std::wstring dst = Scratch(L"len.echodemo");
    REQUIRE(synth::Write(src.c_str(), SmallConfig(), nullptr, nullptr));

    std::vector<uint8_t> file;
    REQUIRE(ReadWhole(src, file));
    const std::vector<FrameSpan> frames = WalkFrames(file);
    REQUIRE(frames.size() > 5);
    const OpenOutcome base = Probe(src);
    const size_t victim = 3;

    // head_crc32 only proves the head agrees with itself, and it is recomputed here exactly as an
    // attacker would. The lengths must be bounded on their own.
    SUBCASE("uncompressed_len beyond the chapter cap") {
        std::vector<uint8_t> copy = file;
        ChapterHeadDisk h = frames[victim].head;
        h.uncompressedLen = 0xFFFFFFFFu;
        RewriteHead(copy, frames[victim], h);
        REQUIRE(WriteWhole(dst, copy.data(), copy.size()));
        const OpenOutcome o = Probe(dst);
        REQUIRE(o.opened);
        CHECK(o.chapters == base.chapters);
        CHECK(o.decodable == base.decodable - 1);
        CHECK(o.faults[victim] == ChapterFault::ShortRead);
    }
    SUBCASE("a codec nothing in the format defines") {
        std::vector<uint8_t> copy = file;
        ChapterHeadDisk h = frames[victim].head;
        h.codec = 0x4242;
        RewriteHead(copy, frames[victim], h);
        REQUIRE(WriteWhole(dst, copy.data(), copy.size()));
        const OpenOutcome o = Probe(dst);
        REQUIRE(o.opened);
        CHECK(o.decodable == base.decodable - 1);
        CHECK(o.faults[victim] == ChapterFault::ShortRead);
    }
}

TEST_CASE("a malformed manifest is a named refusal at every truncation") {
    synth::Config cfg = SmallConfig();
    Manifest m{};
    synth::BuildManifest(cfg, m);
    std::vector<uint8_t> good;
    SerializeManifest(m, good);
    REQUIRE(good.size() > 1000);

    ManifestStore store;
    const char* err = nullptr;
    REQUIRE(DeserializeManifest(good.data(), (uint32_t)good.size(), store, &err));
    CHECK(store.View().playerCount == cfg.playerCount);

    // Every prefix of a valid manifest is an invalid one: something it needs was cut off.
    uint32_t refusals = 0;
    for (uint32_t n = 0; n < good.size(); ++n) {
        err = nullptr;
        if (DeserializeManifest(good.data(), n, store, &err)) {
            FAIL("a truncated manifest was accepted at " << n << " bytes");
        }
        CHECK(err != nullptr);
        ++refusals;
    }
    CHECK(refusals == good.size());
    MESSAGE("manifest truncation refusals: " << refusals);

    // A count field that the payload could never justify is refused where it is read, not where it
    // would have overrun. playerCount follows the u32 fixed-block size and the 32-byte fixed block.
    std::vector<uint8_t> huge = good;
    uint16_t at36 = 0;
    memcpy(&at36, huge.data() + 36, sizeof(at36));
    REQUIRE(at36 == cfg.playerCount);
    at36 = 0xFFFF;
    memcpy(huge.data() + 36, &at36, sizeof(at36));
    err = nullptr;
    CHECK_FALSE(DeserializeManifest(huge.data(), (uint32_t)huge.size(), store, &err));
    REQUIRE(err != nullptr);
    CHECK(std::string(err).find("player count") != std::string::npos);
}

TEST_CASE("an index entry that disagrees with its frame is caught, not followed") {
    const std::wstring src = Scratch(L"idx_src.echodemo");
    const std::wstring dst = Scratch(L"idx.echodemo");
    REQUIRE(synth::Write(src.c_str(), SmallConfig(), nullptr, nullptr));

    std::vector<uint8_t> file;
    REQUIRE(ReadWhole(src, file));
    const std::vector<FrameSpan> frames = WalkFrames(file);
    REQUIRE(frames.size() > 4);

    // Relabel one frame's section and re-crc its head. The trailer index still says what it was, so the
    // derived index and the frame it points at now describe different chapters.
    const size_t victim = 3;
    ChapterHeadDisk h = frames[victim].head;
    h.section = (h.section == kSectionCore) ? kSectionVoip : kSectionCore;
    RewriteHead(file, frames[victim], h);
    REQUIRE(WriteWhole(dst, file.data(), file.size()));

    DemoReader r;
    REQUIRE(r.Open(dst.c_str()));
    REQUIRE(r.Stats().fastPath);
    DecodedChapter c{};
    CHECK_FALSE(r.DecodeChapter(victim, c));
    CHECK(r.LastFault() == ChapterFault::IndexMismatch);
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        if (i == victim) {
            continue;
        }
        DecodedChapter ok{};
        CHECK(r.DecodeChapter(i, ok));
    }
}

TEST_CASE("zstd checksum_wrong and dstSize_tooSmall are not the same diagnosis") {
    const std::wstring src = Scratch(L"zstd_src.echodemo");
    const std::wstring dst = Scratch(L"zstd.echodemo");
    REQUIRE(synth::Write(src.c_str(), SmallConfig(), nullptr, nullptr));

    std::vector<uint8_t> file;
    REQUIRE(ReadWhole(src, file));
    std::vector<FrameSpan> frames = WalkFrames(file);
    size_t victim = 0;
    uint32_t zstdChapters = 0, rawChapters = 0;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].head.codec == kCodecZstd) {
            ++zstdChapters;
            if (victim == 0 && i > 0 && frames[i].head.section == kSectionCore &&
                frames[i].head.payloadLen > 64) {
                victim = i;
            }
        } else {
            ++rawChapters;
        }
    }
    // Both storage paths must be present, or half of this is untested.
    CHECK(zstdChapters > 0);
    CHECK(rawChapters > 0);
    REQUIRE(victim != 0);

    SUBCASE("damaged content that our crc32 was made to accept is zstd's checksum") {
        std::vector<uint8_t> copy = file;
        const size_t payload = static_cast<size_t>(frames[victim].offset) + kChapterHeadBytes;
        copy[payload + frames[victim].head.payloadLen / 2] ^= 0x01;
        ChapterHeadDisk h = frames[victim].head;
        h.payloadCrc32 = Crc32(copy.data() + payload, h.payloadLen);
        RewriteHead(copy, frames[victim], h);
        REQUIRE(WriteWhole(dst, copy.data(), copy.size()));

        DemoReader r;
        REQUIRE(r.Open(dst.c_str()));
        DecodedChapter c{};
        CHECK_FALSE(r.DecodeChapter(victim, c));
        const ChapterFault f = r.LastFault();
        CHECK((f == ChapterFault::ZstdChecksum || f == ChapterFault::ZstdCorrupt));
        CHECK(f != ChapterFault::ZstdSizeMismatch);
    }

    SUBCASE("an undersized destination is reported as our bug, not as damage") {
        std::vector<uint8_t> copy = file;
        ChapterHeadDisk h = frames[victim].head;
        h.uncompressedLen = h.uncompressedLen / 2;  // exactly the mistake dstSize_tooSmall names
        RewriteHead(copy, frames[victim], h);
        REQUIRE(WriteWhole(dst, copy.data(), copy.size()));

        DemoReader r;
        REQUIRE(r.Open(dst.c_str()));
        DecodedChapter c{};
        CHECK_FALSE(r.DecodeChapter(victim, c));
        CHECK(r.LastFault() == ChapterFault::ZstdSizeMismatch);
    }
}

TEST_CASE("a header the reader cannot trust is a named refusal, never a guess") {
    const std::wstring src = Scratch(L"refuse_src.echodemo");
    const std::wstring dst = Scratch(L"refuse.echodemo");
    REQUIRE(synth::Write(src.c_str(), SmallConfig(), nullptr, nullptr));
    std::vector<uint8_t> file;
    REQUIRE(ReadWhole(src, file));

    SUBCASE("wrong magic") {
        std::vector<uint8_t> copy = file;
        copy[0] = 'X';
        REQUIRE(WriteWhole(dst, copy.data(), copy.size()));
        DemoReader r;
        CHECK_FALSE(r.Open(dst.c_str()));
        CHECK(std::string(r.RefusalReason()).find("magic") != std::string::npos);
    }
    SUBCASE("header crc") {
        std::vector<uint8_t> copy = file;
        copy[40] ^= 0x11;
        REQUIRE(WriteWhole(dst, copy.data(), copy.size()));
        DemoReader r;
        CHECK_FALSE(r.Open(dst.c_str()));
        CHECK(std::string(r.RefusalReason()).find("crc32") != std::string::npos);
    }
    SUBCASE("unknown format_major") {
        std::vector<uint8_t> copy = file;
        FileHeaderDisk h{};
        memcpy(&h, copy.data(), sizeof(h));
        h.formatMajor = kFormatMajor + 1;
        h.headerCrc32 = HeaderCrc(h);
        memcpy(copy.data(), &h, sizeof(h));
        REQUIRE(WriteWhole(dst, copy.data(), copy.size()));
        DemoReader r;
        CHECK_FALSE(r.Open(dst.c_str()));
        CHECK(std::string(r.RefusalReason()) == "unknown format_major");
    }
    SUBCASE("a damaged manifest chapter with no trailer") {
        std::vector<uint8_t> copy = file;
        const std::vector<FrameSpan> frames = WalkFrames(copy);
        REQUIRE(frames[0].head.section == kSectionManifest);
        copy[static_cast<size_t>(frames[0].offset) + kChapterHeadBytes + 2] ^= 0xFF;
        FileHeaderDisk h{};
        memcpy(&h, copy.data(), sizeof(h));
        h.flags |= kFlagOpen;
        h.trailerOffset = 0;
        h.headerCrc32 = HeaderCrc(h);
        memcpy(copy.data(), &h, sizeof(h));
        REQUIRE(WriteWhole(dst, copy.data(), copy.size()));
        DemoReader r;
        CHECK_FALSE(r.Open(dst.c_str()));
        CHECK(std::string(r.RefusalReason()).find("MANIFEST") != std::string::npos);
    }
}

// ---- completeness (S5.6.2) -------------------------------------------------------------------------

TEST_CASE("an engine-sourced observed count turns a halved sampler into a recorded GAP") {
    synth::Config cfg = SmallConfig();
    cfg.sampleDivisor = 2;
    cfg.honestObserved = true;
    const std::wstring path = Scratch(L"gap_honest.echodemo");
    WriterStats stats{};
    REQUIRE(synth::Write(path.c_str(), cfg, &stats, nullptr));

    // The writer never had to be told: the deficit between the engine's numbering and what reached the
    // file is itself the signal, and the file says so.
    CHECK(stats.gapRecordsEmitted == cfg.chapters);

    DemoReader r;
    REQUIRE(r.Open(path.c_str()));
    uint64_t gapped = 0, stored = 0, observed = 0;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        DecodedChapter c{};
        REQUIRE(r.DecodeChapter(i, c));
        for (uint16_t b = 0; b < c.streamCount; ++b) {
            const StreamView& v = c.streams[b];
            if (v.id == kStreamGap) {
                for (uint32_t g = 0; g < v.bytes / sizeof(GapRecordDisk); ++g) {
                    GapRecordDisk rec{};
                    memcpy(&rec, v.records + g * sizeof(GapRecordDisk), sizeof(rec));
                    CHECK(rec.streamId == kStreamNetvar);
                    gapped += rec.count;
                }
            } else if (v.id == kStreamNetvar) {
                stored += v.storedCount;
                observed += v.observedCount;
            }
        }
    }
    CHECK(observed == cfg.chapters * cfg.ticksPerChapter * 2);
    CHECK(stored == observed / 2);
    CHECK(stored + gapped == observed);
}

TEST_CASE("a writer-sourced observed count cannot see its own under-sampling") {
    // This is the measured blind spot, recorded rather than hidden: when `observed` comes from the
    // sampler's own loop, halving the sample rate produces a file whose counters balance perfectly.
    // Nothing in the container can distinguish it from a complete capture, which is exactly why
    // Appendix B.2 requires `observed` to come from the engine's numbering.
    synth::Config cfg = SmallConfig();
    cfg.sampleDivisor = 2;
    cfg.honestObserved = false;
    const std::wstring path = Scratch(L"gap_blind.echodemo");
    WriterStats stats{};
    REQUIRE(synth::Write(path.c_str(), cfg, &stats, nullptr));
    CHECK(stats.gapRecordsEmitted == 0);

    DemoReader r;
    REQUIRE(r.Open(path.c_str()));
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        DecodedChapter c{};
        REQUIRE(r.DecodeChapter(i, c));
        for (uint16_t b = 0; b < c.streamCount; ++b) {
            CHECK(c.streams[b].id != kStreamGap);
        }
    }
}

TEST_CASE("a mid-capture rate change is visible in the file's own timestamps") {
    // What the container CAN still see when the counters lie: the stored records per second of source
    // time, compared against the same stream in the neighbouring chapter. No rate is assumed anywhere.
    synth::Config cfg = SmallConfig();
    cfg.chapters = 8;
    cfg.sampleDivisor = 3;
    cfg.divisorFromChapter = 4;
    cfg.honestObserved = false;
    const std::wstring path = Scratch(L"rate_step.echodemo");
    REQUIRE(synth::Write(path.c_str(), cfg, nullptr, nullptr));

    DemoReader r;
    REQUIRE(r.Open(path.c_str()));
    double before = 0, after = 0;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        const ChapterKey k = r.ChapterAt(i);
        if (k.section != kSectionCore) {
            continue;
        }
        DecodedChapter c{};
        REQUIRE(r.DecodeChapter(i, c));
        const StreamView* v = c.Find(kStreamNetvar);
        REQUIRE(v != nullptr);
        const double hz = v->storedCount / ((k.tEndUs - k.tBeginUs) / 1e6);
        if (k.tBeginUs < cfg.t0Us + 4 * cfg.chapterSpanUs) {
            before = hz;
        } else if (after == 0) {
            after = hz;
        }
    }
    CHECK(before > 0);
    CHECK(after > 0);
    CHECK(before / after > 2.0);
}

TEST_CASE("the writer refuses counters it can prove impossible") {
    const std::wstring path = Scratch(L"impossible.echodemo");
    synth::Config cfg = SmallConfig();
    Manifest m{};
    synth::BuildManifest(cfg, m);

    DemoWriter w;
    REQUIRE(w.Open(path.c_str(), m));
    uint8_t rec[16] = {};
    w.BeginChapter(kSectionCore, cfg.t0Us);
    w.WriteStreamBlock(kStreamNetvar, synth::kSchemaNetvar, rec, sizeof(rec), 1, 5);  // stored > observed
    w.WriteStreamBlock(kStreamNetvar, 4242, rec, sizeof(rec), 1, 1);                  // unknown schema
    w.EndChapter(cfg.t0Us + 1);
    w.Close();

    const WriterStats s = w.Stats();
    CHECK(s.observedBelowStored == 1);
    CHECK(s.unknownSchemaBlocks == 1);
}

TEST_CASE("an impossible counter pair reaches the file instead of agreeing with itself") {
    // The writer used to clamp `observed` up to `stored` here. The block then said observed == stored,
    // which is what a healthy block says, so the fault lived only in this process's WriterStats and
    // `echodemo verify` -- the tool whose job is to find it in a file someone hands you -- could not
    // fail on it. This is the injection the verify-side check is red against: observed = stored - 1.
    const std::wstring path = Scratch(L"observed_below_stored.echodemo");
    synth::Config cfg = SmallConfig();
    Manifest m{};
    synth::BuildManifest(cfg, m);

    const uint32_t stored = 5;
    const uint32_t observed = stored - 1;

    DemoWriter w;
    REQUIRE(w.Open(path.c_str(), m));
    uint8_t rec[16] = {};
    w.BeginChapter(kSectionCore, cfg.t0Us);
    w.WriteStreamBlock(kStreamNetvar, synth::kSchemaNetvar, rec, sizeof(rec), observed, stored);
    w.EndChapter(cfg.t0Us + 1);
    w.Close();
    CHECK(w.Stats().observedBelowStored == 1);

    DemoReader r;
    REQUIRE(r.Open(path.c_str()));
    bool found = false;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        DecodedChapter c{};
        if (!r.DecodeChapter(i, c)) {
            continue;
        }
        const StreamView* v = c.Find(kStreamNetvar);
        if (v == nullptr) {
            continue;
        }
        found = true;
        CHECK(v->storedCount == stored);
        CHECK(v->observedCount == observed);
    }
    CHECK(found);
}

// ---- strip (S5.4.6) ------------------------------------------------------------------------------

TEST_CASE("a schema that licenses sample dropping cannot open a writer") {
    // S5.6.2 rule 1 requires a sample-dropping transform to be inexpressible. It was expressible in one
    // field of SchemaDef, and everything else the design demanded was present while half the records
    // went missing: engine-sourced observed, a real deficit, and a file that printed it. The container
    // cannot police it after the fact, because the format carries no per-record index and no run
    // boundary -- so it is refused before a capture can start.
    const std::wstring path = Scratch(L"edge.echodemo");
    synth::Config cfg = SmallConfig();
    Manifest m{};
    synth::BuildManifest(cfg, m);

    std::vector<SchemaDef> schemas(m.schemas, m.schemas + m.schemaCount);
    bool found = false;
    for (SchemaDef& d : schemas) {
        if (d.id == synth::kSchemaNetvar) {
            d.retention = Retention::Edge;
            found = true;
        }
    }
    REQUIRE(found);
    m.schemas = schemas.data();

    DemoWriter w;
    CHECK_FALSE(w.Open(path.c_str(), m));
    REQUIRE(w.LastError() != nullptr);
    CHECK(std::string(w.LastError()).find("retention") != std::string::npos);
    CHECK_FALSE(w.IsOpen());
    // Nothing was created, so nothing has to be cleaned up or half-trusted later.
    CHECK(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES);
}

TEST_CASE("a deficit on any stream authors a GAP, with no retention qualifier") {
    // The old law only ran for retention=ALL. Every sample-carrying stream is subject to it now, which
    // is what S5.6.2 rule 3 says: "verify fails if any chapter's counters disagree" names no retention.
    const std::wstring path = Scratch(L"deficit_any.echodemo");
    synth::Config cfg = SmallConfig();
    Manifest m{};
    synth::BuildManifest(cfg, m);

    DemoWriter w;
    REQUIRE(w.Open(path.c_str(), m));
    uint8_t rec[24] = {};
    w.BeginChapter(kSectionCore, cfg.t0Us);
    // S_EVENT and S_WORLD used to declare EDGE, which excluded them from the law entirely.
    w.WriteStreamBlock(kStreamEvent, synth::kSchemaEvent, rec, sizeof(rec), 9, 1);
    w.WriteStreamBlock(kStreamWorld, synth::kSchemaWorld, rec, 12, 4, 1);
    w.EndChapter(cfg.t0Us + 1000);
    w.Close();
    CHECK(w.Stats().gapRecordsEmitted == 2);

    DemoReader r;
    REQUIRE(r.Open(path.c_str()));
    const size_t core = r.FindChapter(cfg.t0Us, kSectionCore);
    REQUIRE(core != (size_t)-1);
    DecodedChapter c{};
    REQUIRE(r.DecodeChapter(core, c));
    const StreamView* gaps = c.Find(kStreamGap);
    REQUIRE(gaps != nullptr);
    REQUIRE(gaps->bytes == 2 * sizeof(GapRecordDisk));
    uint64_t missing = 0;
    for (uint32_t g = 0; g < 2; ++g) {
        GapRecordDisk rec2{};
        memcpy(&rec2, gaps->records + g * sizeof(GapRecordDisk), sizeof(rec2));
        CHECK((rec2.streamId == kStreamEvent || rec2.streamId == kStreamWorld));
        missing += rec2.count;
    }
    CHECK(missing == 11);  // (9 - 1) + (4 - 1)
}

TEST_CASE("two decoded chapters may be live at once, and that is the whole window") {
    // The contract W2-A and W2-B build against: a DecodedChapter survives exactly one further decode.
    const std::wstring path = Scratch(L"lifetime.echodemo");
    REQUIRE(synth::Write(path.c_str(), SmallConfig(), nullptr, nullptr));

    DemoReader r;
    REQUIRE(r.Open(path.c_str()));
    size_t a = (size_t)-1, b = (size_t)-1;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        if (r.ChapterAt(i).section != kSectionCore) {
            continue;
        }
        if (a == (size_t)-1) {
            a = i;
        } else {
            b = i;
            break;
        }
    }
    REQUIRE(b != (size_t)-1);

    DecodedChapter first{}, second{};
    REQUIRE(r.DecodeChapter(a, first));
    const StreamView* firstKey = first.Find(kStreamKey);
    REQUIRE(firstKey != nullptr);
    std::vector<uint8_t> snapshot(firstKey->records, firstKey->records + firstKey->bytes);

    REQUIRE(r.DecodeChapter(b, second));
    // One decode later the first result is still entirely intact, pointers included.
    const StreamView* stillFirst = first.Find(kStreamKey);
    REQUIRE(stillFirst != nullptr);
    CHECK(memcmp(stillFirst->records, snapshot.data(), snapshot.size()) == 0);
    CHECK(first.key.tBeginUs != second.key.tBeginUs);
    const StreamView* secondKey = second.Find(kStreamKey);
    REQUIRE(secondKey != nullptr);
    CHECK(memcmp(secondKey->records, snapshot.data(), snapshot.size()) != 0);
}

TEST_CASE("strip --voip removes the voice and proves it did") {
    const std::wstring src = Scratch(L"voice.echodemo");
    const std::wstring dst = Scratch(L"novoice.echodemo");
    const synth::Config cfg = SmallConfig();
    REQUIRE(synth::Write(src.c_str(), cfg, nullptr, nullptr));

    const StripResult res = StripSections(src.c_str(), dst.c_str(), 1u << kSectionVoip);
    INFO((res.error ? res.error : ""));
    REQUIRE(res.error == nullptr);
    CHECK(res.verified);
    CHECK(res.chaptersDropped == cfg.chapters);
    CHECK(res.chaptersUndecodable == 0);
    CHECK(res.bytesRemoved > 0);

    DemoReader r;
    REQUIRE(r.Open(dst.c_str()));
    CHECK((r.FileFlags() & kFlagHasVoip) == 0);
    CHECK((r.FileFlags() & kFlagHasAsSeen) != 0);
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        CHECK(r.ChapterAt(i).section != kSectionVoip);
        DecodedChapter c{};
        REQUIRE(r.DecodeChapter(i, c));
        CHECK(c.Find(kStreamVoip) == nullptr);
    }

    // The bytes really are gone, not merely unreferenced.
    std::vector<uint8_t> before, after;
    REQUIRE(ReadWhole(src, before));
    REQUIRE(ReadWhole(dst, after));
    CHECK(after.size() < before.size());

    // Everything else survives, including the markers the trailer mirrors.
    CHECK(r.GetManifest().roundCount == 2);
    CHECK(r.GetManifest().playerCount == cfg.playerCount);
    std::string err;
    // The kept sections are still exactly what the plan wrote; only the VOIP chapters are absent.
    synth::Config noVoip = cfg;
    noVoip.voip = false;
    CHECK(synth::VerifyRoundTrip(dst.c_str(), noVoip, &err) > 0);
    INFO(err);
    CHECK(err.empty());
}

TEST_CASE("strip refuses to produce a file that cannot play") {
    const std::wstring src = Scratch(L"voice2.echodemo");
    const std::wstring dst = Scratch(L"nope.echodemo");
    REQUIRE(synth::Write(src.c_str(), SmallConfig(), nullptr, nullptr));
    CHECK(StripSections(src.c_str(), dst.c_str(), 1u << kSectionCore).error != nullptr);
    CHECK(StripSections(src.c_str(), dst.c_str(), 1u << kSectionManifest).error != nullptr);
    CHECK(StripSections(src.c_str(), dst.c_str(), 0).error != nullptr);
}

// ---- SC8 -----------------------------------------------------------------------------------------

namespace {

struct KillResult {
    bool opened = false;
    bool recoveredByScan = false;
    size_t chapters = 0;
    uint32_t decodable = 0;
    uint64_t sizeAtKill = 0;
    uint64_t target = 0;
};

KillResult RunAndKill(const std::wstring& victimExe, const std::wstring& out, uint64_t targetBytes,
                      uint32_t chapters) {
    KillResult k;
    k.target = targetBytes;
    DeleteFileW(out.c_str());

    wchar_t cmd[1024];
    _snwprintf_s(cmd, _TRUNCATE, L"\"%ls\" \"%ls\" --chapters %u --sleep-ms 1", victimExe.c_str(),
                 out.c_str(), chapters);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return k;
    }

    HANDLE probe = INVALID_HANDLE_VALUE;
    const ULONGLONG deadline = GetTickCount64() + 30000;
    while (GetTickCount64() < deadline) {
        if (probe == INVALID_HANDLE_VALUE) {
            probe = CreateFileW(out.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
        }
        if (probe != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz{};
            if (GetFileSizeEx(probe, &sz) && static_cast<uint64_t>(sz.QuadPart) >= targetBytes) {
                k.sizeAtKill = static_cast<uint64_t>(sz.QuadPart);
                break;
            }
        }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            break;  // it finished before we could reach the target
        }
    }
    TerminateProcess(pi.hProcess, 9);
    WaitForSingleObject(pi.hProcess, 5000);
    if (probe != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz{};
        if (GetFileSizeEx(probe, &sz)) {
            k.sizeAtKill = static_cast<uint64_t>(sz.QuadPart);
        }
        CloseHandle(probe);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    DemoReader r;
    if (r.Open(out.c_str())) {
        k.opened = true;
        k.recoveredByScan = r.WasRecovered();
        k.chapters = r.ChapterCount();
        for (size_t i = 0; i < r.ChapterCount(); ++i) {
            DecodedChapter c{};
            if (r.DecodeChapter(i, c)) {
                ++k.decodable;
            }
        }
    }
    return k;
}

}  // namespace

TEST_CASE("SC8: twenty TerminateProcess kills each leave a file that opens and plays") {
    const std::wstring victim = SiblingExe(L"container_mkdemo.exe");
    REQUIRE(GetFileAttributesW(victim.c_str()) != INVALID_FILE_ATTRIBUTES);

    // A reference run gives the byte range a kill can meaningfully land in: after the MANIFEST chapter
    // is on disk, and before the capture would have finished on its own.
    const std::wstring reference = Scratch(L"sc8_ref.echodemo");
    synth::Config refCfg = SmallConfig();
    refCfg.chapters = 240;
    refCfg.sleepMsPerChapter = 1;
    REQUIRE(synth::Write(reference.c_str(), refCfg, nullptr, nullptr));
    std::vector<uint8_t> refFile;
    REQUIRE(ReadWhole(reference, refFile));
    const std::vector<FrameSpan> refFrames = WalkFrames(refFile);
    REQUIRE(refFrames.size() > 20);
    const uint64_t low = refFrames[0].offset + kChapterHeadBytes + refFrames[0].head.payloadLen + 1;
    // Stop short of the end: a target the writer only reaches after it has finished would be killed
    // after a clean close, and a clean file proves nothing about recovery.
    const uint64_t high = low + (refFile.size() - low) * 4 / 5;
    REQUIRE(high > low);

    const std::wstring out = Scratch(L"sc8_kill.echodemo");
    uint64_t state = 0x5DEECE66Dull;
    uint32_t recovered = 0;
    uint32_t byScan = 0;
    uint32_t killedAfterFinish = 0;
    for (int i = 0; i < 20; ++i) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        const uint64_t target = low + (state >> 33) % (high - low);
        const KillResult k = RunAndKill(victim, out, target, refCfg.chapters);
        CAPTURE(i);
        CAPTURE(target);
        CAPTURE(k.sizeAtKill);
        CHECK(k.opened);
        CHECK(k.chapters > 0);
        CHECK(k.decodable == k.chapters);
        if (k.opened && k.decodable == k.chapters && k.chapters > 0) {
            ++recovered;
        }
        if (k.recoveredByScan) {
            ++byScan;
        }
        if (k.sizeAtKill >= refFile.size()) {
            ++killedAfterFinish;
        }
    }
    MESSAGE("SC8: 20 kills / " << recovered << " recovered; " << byScan
                               << " needed the recovery scan; " << killedAfterFinish
                               << " landed after the capture had already finished");
    CHECK(recovered == 20);
    // A kill that arrived after a clean close proves nothing about recovery, so the run is only valid
    // if every file went through the scan.
    CHECK(killedAfterFinish == 0);
    CHECK(byScan == 20);
}

// ---- ARCH-21: S_GAMESTATE and the MANIFEST round table -------------------------------------------

namespace {

// The pre-ARCH-21 capture the L2 #2 run left on disk. It carries no round table and no S_GAMESTATE
// stream, which is the state of every capture taken so far, and it is read-only here.
const std::wstring kL2Capture =
    replay_test::CaptureFileW(L"2026-09-04_07-10-29_0000000000000000_0000000000000000.echodemo");

enum : uint16_t { kSchemaGameState = 11 };

RoundRow MakeRow(uint16_t number, uint64_t beginUs) {
    RoundRow r{};
    r.number = number;
    r.phaseAtBegin = 5;  // RoundStart
    r.phaseAtEnd = 8;    // RoundOver
    r.bluePointsAtEnd = static_cast<uint16_t>(number * 3 + 1);
    r.redPointsAtEnd = static_cast<uint16_t>(number + 2);
    r.blueMatchScoreAtEnd = static_cast<uint16_t>(number * 7);
    r.redMatchScoreAtEnd = static_cast<uint16_t>(number * 2 + 5);
    r.beginUs = beginUs;
    r.endUs = beginUs + 90000000ull;
    r.beginEngineUs = beginUs + 1111;
    r.endEngineUs = r.endUs + 1111;
    return r;
}

GameStateRecordDisk MakeSample(uint32_t roundCounter, uint64_t tUs) {
    GameStateRecordDisk g{};
    g.engineTimeUs = tUs + 7;
    g.captureTimeUs = tUs;
    g.phaseSymbol = 0x0102030405060708ull;
    g.pausedSymbol = 0x090A0B0C0D0E0F10ull;
    g.roundCounter = roundCounter;
    g.bluePoints = 3;
    g.redPoints = 2;
    g.blueMatchScore = 11;
    g.redMatchScore = 9;
    g.frisbeeOwnerId = UINT32_MAX;
    g.gameTimer = 123.5f;
    g.countdownTimer = 0.25f;
    g.firstLaunchOfRound = 1;
    g.roundWinningTeam = -1;
    g.lastScoreTeam = 1;
    g.lastScorePoints = 2;
    g.present = kGameStatePresentAll;
    g.phase = 6;  // Playing
    g.availability = 1;
    return g;
}

// Where the round-table block starts inside a serialized manifest. Derived rather than searched for:
// the block is appended, so the same manifest without it is exactly the prefix that precedes it, and a
// scan for the tag could match a byte pattern inside a player blob.
size_t RoundTableAt(const Manifest& m) {
    Manifest without = m;
    without.markerSource = MarkerSource::NotRecorded;
    without.roundRowCount = 0;
    without.roundRows = nullptr;
    std::vector<uint8_t> prefix;
    SerializeManifest(without, prefix);
    return prefix.size();
}

// A manifest carrying `rows` under `source`, plus the bytes it serialises to.
struct TabledManifest {
    synth::Config cfg = SmallConfig();
    Manifest m{};
    std::vector<RoundRow> rows;
    std::vector<uint8_t> bytes;
    size_t headAt = 0;  // offset of the RoundTableHeadDisk, past the 8-byte tag and length

    TabledManifest(std::initializer_list<uint16_t> numbers, MarkerSource source) {
        synth::BuildManifest(cfg, m);
        uint64_t t = cfg.t0Us;
        for (uint16_t n : numbers) {
            rows.push_back(MakeRow(n, t));
            t += 120000000ull;
        }
        m.roundRowCount = static_cast<uint16_t>(rows.size());
        m.roundRows = rows.empty() ? nullptr : rows.data();
        m.markerSource = source;
        headAt = RoundTableAt(m) + 8;
        SerializeManifest(m, bytes);
    }
};

const char* RefusalFor(std::vector<uint8_t>& bytes) {
    ManifestStore store;
    const char* err = nullptr;
    if (DeserializeManifest(bytes.data(), static_cast<uint32_t>(bytes.size()), store, &err)) {
        return nullptr;
    }
    return err ? err : "(refused with no reason)";
}
// A malformed round table no longer costs the FILE: the manifest parses, the ROUNDS block is refused
// BY NAME and carries no rows. This returns that name, and asserts the degradation is whole.
std::string RoundsRefusalFor(std::vector<uint8_t>& bytes) {
    ManifestStore store;
    const char* err = nullptr;
    REQUIRE(DeserializeManifest(bytes.data(), static_cast<uint32_t>(bytes.size()), store, &err));
    CHECK(store.View().roundRowCount == 0);
    CHECK(store.View().markerSource == MarkerSource::NotRecorded);
    REQUIRE(store.View().roundsRefused != nullptr);
    return store.View().roundsRefused;
}

}  // namespace

TEST_CASE("the ARCH-21 round table round-trips through the manifest byte for byte") {
    TabledManifest t({3, 4, 5}, MarkerSource::Local);

    ManifestStore store;
    const char* err = nullptr;
    REQUIRE(DeserializeManifest(t.bytes.data(), (uint32_t)t.bytes.size(), store, &err));
    const Manifest& got = store.View();
    CHECK(got.markerSource == MarkerSource::Local);
    REQUIRE(got.roundRowCount == 3);
    for (uint16_t i = 0; i < 3; ++i) {
        const RoundRow& a = t.rows[i];
        const RoundRow& b = got.roundRows[i];
        CHECK(b.number == a.number);
        CHECK(b.flags == a.flags);
        CHECK(b.phaseAtBegin == a.phaseAtBegin);
        CHECK(b.phaseAtEnd == a.phaseAtEnd);
        CHECK(b.bluePointsAtEnd == a.bluePointsAtEnd);
        CHECK(b.redPointsAtEnd == a.redPointsAtEnd);
        CHECK(b.blueMatchScoreAtEnd == a.blueMatchScoreAtEnd);
        CHECK(b.redMatchScoreAtEnd == a.redMatchScoreAtEnd);
        CHECK(b.beginUs == a.beginUs);
        CHECK(b.endUs == a.endUs);
        CHECK(b.beginEngineUs == a.beginEngineUs);
        CHECK(b.endEngineUs == a.endEngineUs);
    }

    // Byte-exact: what the reader handed back re-serialises to the bytes it was read from.
    std::vector<uint8_t> again;
    SerializeManifest(got, again);
    REQUIRE(again.size() == t.bytes.size());
    CHECK(memcmp(again.data(), t.bytes.data(), again.size()) == 0);
}

TEST_CASE("a manifest with no round table says NOT_RECORDED, not zero rounds") {
    synth::Config cfg = SmallConfig();
    Manifest m{};
    synth::BuildManifest(cfg, m);
    std::vector<uint8_t> bytes;
    SerializeManifest(m, bytes);

    // The block is absent, not empty: a pre-ARCH-21 writer produced exactly these bytes.
    for (size_t i = 0; i + sizeof(uint32_t) <= bytes.size(); ++i) {
        uint32_t tag = 0;
        memcpy(&tag, bytes.data() + i, sizeof(tag));
        REQUIRE(tag != kRoundTableTag);
    }

    ManifestStore store;
    const char* err = nullptr;
    REQUIRE(DeserializeManifest(bytes.data(), (uint32_t)bytes.size(), store, &err));
    CHECK(store.View().markerSource == MarkerSource::NotRecorded);
    CHECK(store.View().roundRowCount == 0);

    // A writer that chose and found nothing is a DIFFERENT file from one that never asked.
    TabledManifest chose({}, MarkerSource::None);
    ManifestStore store2;
    REQUIRE(DeserializeManifest(chose.bytes.data(), (uint32_t)chose.bytes.size(), store2, &err));
    CHECK(store2.View().markerSource == MarkerSource::None);
    CHECK(store2.View().roundRowCount == 0);
    CHECK(chose.bytes.size() == bytes.size() + 8 + sizeof(RoundTableHeadDisk));
}

TEST_CASE("a malformed round table is a named refusal of the ROUNDS block") {
    SUBCASE("a row count one past what the block holds") {
        TabledManifest t({3, 4, 5}, MarkerSource::Local);
        RoundTableHeadDisk h{};
        memcpy(&h, t.bytes.data() + t.headAt, sizeof(h));
        h.rowCount = 4;
        memcpy(t.bytes.data() + t.headAt, &h, sizeof(h));
        CHECK(RoundsRefusalFor(t.bytes) ==
              "round table size disagrees with its row count");
    }
    SUBCASE("a row count one short of what the block holds") {
        TabledManifest t({3, 4, 5}, MarkerSource::Local);
        RoundTableHeadDisk h{};
        memcpy(&h, t.bytes.data() + t.headAt, sizeof(h));
        h.rowCount = 2;
        memcpy(t.bytes.data() + t.headAt, &h, sizeof(h));
        CHECK(RoundsRefusalFor(t.bytes) ==
              "round table size disagrees with its row count");
    }
    SUBCASE("a marker source outside the vocabulary") {
        TabledManifest t({3}, MarkerSource::Local);
        RoundTableHeadDisk h{};
        memcpy(&h, t.bytes.data() + t.headAt, sizeof(h));
        h.markerSource = kMaxMarkerSource + 1;
        memcpy(t.bytes.data() + t.headAt, &h, sizeof(h));
        CHECK(RoundsRefusalFor(t.bytes) ==
              "round table declares a marker source outside the vocabulary");
    }
    SUBCASE("a block that claims NOT_RECORDED, which only absence can say") {
        TabledManifest t({3}, MarkerSource::Local);
        RoundTableHeadDisk h{};
        memcpy(&h, t.bytes.data() + t.headAt, sizeof(h));
        h.markerSource = static_cast<uint8_t>(MarkerSource::NotRecorded);
        memcpy(t.bytes.data() + t.headAt, &h, sizeof(h));
        CHECK(RoundsRefusalFor(t.bytes) == "round table declares no marker source");
    }
    SUBCASE("a row stride narrower than one row") {
        TabledManifest t({3}, MarkerSource::Local);
        RoundTableHeadDisk h{};
        memcpy(&h, t.bytes.data() + t.headAt, sizeof(h));
        h.rowStride = sizeof(RoundRowDisk) - 1;
        memcpy(t.bytes.data() + t.headAt, &h, sizeof(h));
        CHECK(RoundsRefusalFor(t.bytes) ==
              "round table row stride is smaller than one row");
    }
    SUBCASE("a phase outside the vocabulary") {
        TabledManifest t({3}, MarkerSource::Local);
        const size_t rowAt = t.headAt + sizeof(RoundTableHeadDisk);
        RoundRowDisk d{};
        memcpy(&d, t.bytes.data() + rowAt, sizeof(d));
        d.phaseAtEnd = kMaxGameStatePhase + 1;
        memcpy(t.bytes.data() + rowAt, &d, sizeof(d));
        CHECK(RoundsRefusalFor(t.bytes) ==
              "round row declares a phase outside the vocabulary");
    }
    SUBCASE("a flag bit the format does not define") {
        TabledManifest t({3}, MarkerSource::Local);
        const size_t rowAt = t.headAt + sizeof(RoundTableHeadDisk);
        RoundRowDisk d{};
        memcpy(&d, t.bytes.data() + rowAt, sizeof(d));
        d.flags = static_cast<uint16_t>(kAllRoundRowFlags + 1u);
        memcpy(t.bytes.data() + rowAt, &d, sizeof(d));
        CHECK(RoundsRefusalFor(t.bytes) ==
              "round row sets a flag bit the format does not define");
    }
    SUBCASE("numbers that do not ascend") {
        TabledManifest t({3, 4, 5}, MarkerSource::Local);
        const size_t rowAt = t.headAt + sizeof(RoundTableHeadDisk) + sizeof(RoundRowDisk);
        RoundRowDisk d{};
        memcpy(&d, t.bytes.data() + rowAt, sizeof(d));
        d.number = 3;  // a duplicate makes SeekRound(3) ambiguous
        memcpy(t.bytes.data() + rowAt, &d, sizeof(d));
        CHECK(RoundsRefusalFor(t.bytes) == "round table numbers do not ascend: row 1 is 3 after 3");
    }
    SUBCASE("a block cut short by one byte") {
        TabledManifest t({3, 4, 5}, MarkerSource::Local);
        t.bytes.pop_back();
        CHECK(std::string(RefusalFor(t.bytes)) == "MANIFEST: trailing block overruns the payload");
    }
    SUBCASE("the table appended twice") {
        TabledManifest t({3}, MarkerSource::Local);
        const size_t tagAt = t.headAt - 8;
        t.bytes.insert(t.bytes.end(), t.bytes.begin() + tagAt, t.bytes.end());
        CHECK(RoundsRefusalFor(t.bytes) == "the round table appears twice");
    }
    SUBCASE("a trailing block header with nothing behind it") {
        TabledManifest t({3}, MarkerSource::Local);
        t.bytes.push_back(0);
        CHECK(std::string(RefusalFor(t.bytes)) == "MANIFEST: trailing block header is short");
    }
    SUBCASE("a valid table is not refused by any of the above") {
        TabledManifest t({3, 4, 5}, MarkerSource::Local);
        CHECK(RefusalFor(t.bytes) == nullptr);
    }
}

TEST_CASE("a numberless row is legal and is seekable by time only") {
    TabledManifest t({}, MarkerSource::Local);
    std::vector<RoundRow> rows;
    rows.push_back(MakeRow(kRoundNumberNone, t.cfg.t0Us));
    rows[0].flags = kRoundNumberRefused | kRoundPartialStart;
    rows.push_back(MakeRow(7, t.cfg.t0Us + 200000000ull));
    Manifest m = t.m;
    m.roundRowCount = 2;
    m.roundRows = rows.data();
    std::vector<uint8_t> bytes;
    SerializeManifest(m, bytes);

    ManifestStore store;
    const char* err = nullptr;
    REQUIRE(DeserializeManifest(bytes.data(), (uint32_t)bytes.size(), store, &err));
    REQUIRE(store.View().roundRowCount == 2);
    CHECK(store.View().roundRows[0].number == kRoundNumberNone);
    CHECK((store.View().roundRows[0].flags & kRoundNumberRefused) != 0);
    CHECK(store.View().roundRows[1].number == 7);
}

TEST_CASE("round_counter narrows to a round number by refusal, never by truncation") {
    // ARCH-21 C4 / M5. A build that masks instead of refusing calls round 65537 "round 1", and
    // SeekRound(1) then lands in the wrong round with nothing in the file saying so.
    uint16_t out = 0xAAAA;
    CHECK(RoundNumberFromCounter(0, out));
    CHECK(out == 0);
    CHECK(RoundNumberFromCounter(kMaxRoundNumber, out));
    CHECK(out == kMaxRoundNumber);
    CHECK_FALSE(RoundNumberFromCounter(kRoundNumberNone, out));
    CHECK(out == kRoundNumberNone);
    CHECK_FALSE(RoundNumberFromCounter(0x10001u, out));
    CHECK(out == kRoundNumberNone);
    CHECK_FALSE(RoundNumberFromCounter(UINT32_MAX, out));
    CHECK(out == kRoundNumberNone);

    // The widths the refusal exists to protect.
    CHECK(sizeof(RoundRowDisk::number) == 2);
    CHECK(sizeof(MarkRecordDisk::index) == 2);
    CHECK(sizeof(GameStateRecordDisk::roundCounter) == 4);
    CHECK(sizeof(GameStateRecordDisk) == 84);
    CHECK(sizeof(RoundRowDisk) == 48);
}

TEST_CASE("an S_GAMESTATE stream and a round table survive a whole file, byte for byte") {
    const std::wstring path = Scratch(L"gamestate.echodemo");
    synth::Config cfg = SmallConfig();
    Manifest m{};
    synth::BuildManifest(cfg, m);

    // S_GAMESTATE is fixed-width and raw, like S_MARK, but it still cites a schema the manifest carries.
    FieldDef raw{};
    raw.id = 1;
    raw.cls = FieldClass::Raw;
    raw.rawBits = 8;
    raw.prov = FieldProv::Pc;
    std::vector<SchemaDef> schemas(m.schemas, m.schemas + m.schemaCount);
    SchemaDef gs{};
    gs.id = kSchemaGameState;
    gs.retention = Retention::All;
    gs.required = false;
    gs.fieldCount = 1;
    gs.fields = &raw;
    schemas.push_back(gs);
    m.schemaCount = static_cast<uint16_t>(schemas.size());
    m.schemas = schemas.data();

    std::vector<GameStateRecordDisk> samples;
    samples.push_back(MakeSample(3, cfg.t0Us));
    samples.push_back(MakeSample(0x10001u, cfg.t0Us + 1111));  // past what a round number can hold
    samples.push_back(MakeSample(4, cfg.t0Us + 2222));
    samples[1].present = kGsPhaseSymbol | kGsRoundCounter;  // the rest did not answer
    samples[1].phase = 2;                                   // Unknown
    samples[1].availability = 4;                            // NoScoreStore
    const uint32_t sampleBytes = static_cast<uint32_t>(samples.size() * sizeof(GameStateRecordDisk));

    std::vector<RoundRow> rows;
    rows.push_back(MakeRow(3, cfg.t0Us));
    rows.push_back(MakeRow(4, cfg.t0Us + 120000000ull));

    DemoWriter w;
    REQUIRE(w.Open(path.c_str(), m));
    w.BeginChapter(kSectionCore, cfg.t0Us);
    w.WriteStreamBlock(kStreamGameState, kSchemaGameState, samples.data(), sampleBytes,
                       (uint32_t)samples.size(), (uint32_t)samples.size());
    w.EndChapter(cfg.t0Us + 3333);
    REQUIRE(w.SetRoundTable(rows.data(), rows.size(), MarkerSource::Local));
    w.Close();
    CHECK(w.LastError() == nullptr);

    DemoReader r;
    REQUIRE(r.Open(path.c_str()));
    const Manifest& got = r.GetManifest();
    CHECK(got.markerSource == MarkerSource::Local);
    REQUIRE(got.roundRowCount == 2);
    CHECK(got.roundRows[0].number == 3);
    CHECK(got.roundRows[1].number == 4);
    CHECK(got.roundRows[1].blueMatchScoreAtEnd == rows[1].blueMatchScoreAtEnd);

    bool found = false;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        DecodedChapter c{};
        if (!r.DecodeChapter(i, c)) {
            continue;
        }
        if (const StreamView* v = c.Find(kStreamGameState)) {
            found = true;
            REQUIRE(v->bytes == sampleBytes);
            CHECK(v->storedCount == static_cast<uint32_t>(samples.size()));
            CHECK(memcmp(v->records, samples.data(), sampleBytes) == 0);
            GameStateRecordDisk back{};
            memcpy(&back, v->records + sizeof(GameStateRecordDisk), sizeof(back));
            CHECK(back.roundCounter == 0x10001u);  // the full engine width survived the file
            break;
        }
    }
    CHECK(found);
    CHECK(std::string(StreamName(kStreamGameState)) == "S_GAMESTATE");
}

TEST_CASE("a file whose round table is malformed opens, and the ROUNDS block is refused by name") {
    // The wiring, not just the parser: the reader prefers the TRAILER manifest and falls back to the
    // opening MANIFEST chapter when it is absent. That chapter is written before the table exists, so a
    // fallback on a malformed table would answer "not_recorded" for a file that carries a broken one.
    // Rows with no source is the caller bug this can actually be produced by -- W2-R filling the rows
    // and leaving markerSource at its zero -- so it is the fixture rather than a byte patch.
    const std::wstring path = Scratch(L"badtable.echodemo");
    synth::Config cfg = SmallConfig();
    Manifest m{};
    synth::BuildManifest(cfg, m);
    std::vector<RoundRow> rows;
    rows.push_back(MakeRow(1, cfg.t0Us));
    rows.push_back(MakeRow(2, cfg.t0Us + 120000000ull));
    m.roundRowCount = 2;
    m.roundRows = rows.data();
    m.markerSource = MarkerSource::NotRecorded;

    DemoWriter w;
    REQUIRE(w.Open(path.c_str(), m));
    w.BeginChapter(kSectionCore, cfg.t0Us);
    uint8_t rec[8] = {};
    w.WriteStreamBlock(kStreamTick, synth::kSchemaTick, rec, sizeof(rec), 1, 1);
    w.EndChapter(cfg.t0Us + 1111);
    w.Close();

    // The FILE is not the round table. It opens, it plays, and the ROUNDS block alone is refused.
    DemoReader r;
    REQUIRE(r.Open(path.c_str()));
    CHECK(r.RefusalReason() == nullptr);
    REQUIRE(r.GetManifest().roundsRefused != nullptr);
    CHECK(std::string(r.GetManifest().roundsRefused) == "round table declares no marker source");
    CHECK(r.GetManifest().roundRowCount == 0);
    CHECK(r.GetManifest().markerSource == MarkerSource::NotRecorded);
}

TEST_CASE("the writer refuses an S_GAMESTATE block that is not a whole number of records") {
    const std::wstring path = Scratch(L"gsragged.echodemo");
    synth::Config cfg = SmallConfig();
    Manifest m{};
    synth::BuildManifest(cfg, m);
    DemoWriter w;
    REQUIRE(w.Open(path.c_str(), m));
    w.BeginChapter(kSectionCore, cfg.t0Us);
    uint8_t ragged[sizeof(GameStateRecordDisk) + 1] = {};
    w.WriteStreamBlock(kStreamGameState, synth::kSchemaMark, ragged, sizeof(ragged), 1, 1);
    w.EndChapter(cfg.t0Us + 1);
    w.Close();
    REQUIRE(w.LastError() != nullptr);
    CHECK(std::string(w.LastError()) == "an S_GAMESTATE block is not a whole number of records");
}

TEST_CASE("the capture already on disk still opens, and its markers are unchanged") {
    DemoReader r;
    if (!r.Open(kL2Capture.c_str())) {
        MESSAGE("NOT RUN: the L2 capture is not on this machine at "
                << "<captures>/2026-09-04_07-10-29_*.echodemo (set REPLAY_CAPTURE_DIR)");
        return;
    }
    const Manifest& m = r.GetManifest();
    // Written before ARCH-21: it carries no round table, and that is a state, not a failure.
    CHECK(m.markerSource == MarkerSource::NotRecorded);
    CHECK(m.roundRowCount == 0);
    MESSAGE("L2 capture: " << r.ChapterCount() << " chapters, " << m.roundCount << " S_MARK rows");

    // MarkRec is binary-compatible: the file's own markers re-serialise to the same bytes and read back
    // identical. A change to MarkRecordDisk would move them.
    std::vector<uint8_t> a;
    SerializeManifest(m, a);
    ManifestStore store;
    const char* err = nullptr;
    REQUIRE(DeserializeManifest(a.data(), (uint32_t)a.size(), store, &err));
    REQUIRE(store.View().roundCount == m.roundCount);
    for (uint16_t i = 0; i < m.roundCount; ++i) {
        // Field by field: RoundMarker is an in-memory struct with padding, and only the 20-byte disk
        // record is byte-exact -- which the buffer comparison below is what actually proves.
        const RoundMarker& a2 = store.View().rounds[i];
        const RoundMarker& b2 = m.rounds[i];
        CHECK(a2.tUs == b2.tUs);
        CHECK(a2.kind == b2.kind);
        CHECK(a2.index == b2.index);
        CHECK(a2.payload == b2.payload);
    }
    std::vector<uint8_t> b;
    SerializeManifest(store.View(), b);
    REQUIRE(a.size() == b.size());
    CHECK(memcmp(a.data(), b.data(), a.size()) == 0);
}

// --- W1-B: the extended PlayerRecord ------------------------------------------------------------
//
// The disk record lives in manifest_io.cpp's anonymous namespace, so these tests pin it from OUTSIDE:
// the record's own size field, read out of the serialised bytes, is the only number a mutant that
// changes the layout cannot also change here.

namespace {

constexpr size_t kFirstPlayerRecBytesAt = 4 + 32 + 2;  // [u32 fixedBytes][32 fixed][u16 playerCount]
constexpr uint32_t kPlayerFixedV0OnDisk = 254;
constexpr uint32_t kPlayerExtV1OnDisk = 102;
constexpr uint32_t kPlayerExtV2OnDisk = 4;   // RECORDER-3: the extension fill word
constexpr uint32_t kPlayerFixedTotalBytes =
    kPlayerFixedV0OnDisk + kPlayerExtV1OnDisk + kPlayerExtV2OnDisk;

const std::wstring kL2Capture0801 =
    replay_test::CaptureFileW(L"2026-09-04_08-01-50_0000000000000000_0000000000000000.echodemo");

PlayerRecord SynthPlayer(const char* name, uint16_t team, uint16_t align) {
    PlayerRecord p{};
    p.xplatformId = 0x1122334455667788ull;
    strncpy_s(p.displayNameFull, name, _TRUNCATE);
    strncpy_s(p.platformStrFull, "OVR-ORG-4242", _TRUNCATE);
    strncpy_s(p.displayName, name, _TRUNCATE);
    strncpy_s(p.platformStr, "OVR-ORG-4242", _TRUNCATE);
    p.userSlot = 3;
    p.userIndex = 1;
    p.generation = 7;
    p.teamId = 1;
    p.teamSlot = 2;
    p.alignment = 0;
    p.loadoutNumber = 11;
    p.loadoutNumberBody = 12;
    for (int i = 0; i < 20; ++i) {
        p.loadoutSlots[i] = 0x5000ull + i;
    }
    p.teamId16 = team;
    // A team slot without a team is not a fact, and the writer normalises it away: a row's team fields
    // are all sourced or all absent.
    p.teamSlot16 = team == kPlayerNoTeam16 ? kPlayerNoTeam16 : 2;
    p.alignment16 = align;
    p.firstSeenUs = 10;
    p.lastSeenUs = 20;
    p.fill = kPlayerFillSlot | kPlayerFillName | kPlayerFillPlatform | kPlayerFillXplat |
             kPlayerFillGen | kPlayerFillLoadout;
    if (team != kPlayerNoTeam16) {
        p.fill |= kPlayerFillTeam;
    }
    if (align != kPlayerNoAlignment16) {
        p.fill |= kPlayerFillAlign;
    }
    return p;
}

// One player's manifest, rewritten with a fixed block of `fixedBytes` -- 254 is what a pre-v1 writer
// emitted, and anything else is a version that never existed or one from the future.
std::vector<uint8_t> WithFixedBlockOf(const std::vector<uint8_t>& v1, uint32_t fixedBytes,
                                      uint32_t blobBytes) {
    std::vector<uint8_t> out(v1.begin(), v1.begin() + kFirstPlayerRecBytesAt);
    const uint32_t rec = fixedBytes + blobBytes;
    const uint8_t* recSize = reinterpret_cast<const uint8_t*>(&rec);
    out.insert(out.end(), recSize, recSize + 4);
    const uint8_t* fixed = v1.data() + kFirstPlayerRecBytesAt + 4;
    if (fixedBytes <= kPlayerFixedTotalBytes) {
        out.insert(out.end(), fixed, fixed + fixedBytes);
    } else {
        out.insert(out.end(), fixed, fixed + kPlayerFixedTotalBytes);
        out.insert(out.end(), fixedBytes - kPlayerFixedTotalBytes, 0xAA);
    }
    out.insert(out.end(), v1.begin() + kFirstPlayerRecBytesAt + 4 + kPlayerFixedTotalBytes, v1.end());
    return out;
}

}  // namespace

TEST_CASE("a player row round-trips with its full-width fields, and its record size is pinned") {
    PlayerRecord in[2] = {SynthPlayer("Kaiser Wilhelm II", 1, 5),
                          SynthPlayer("spectator", kPlayerNoTeam16, kPlayerNoAlignment16)};
    const char entrant[] = "{\"account\":1,\"level\":42}";
    in[0].entrantJson.p = reinterpret_cast<const uint8_t*>(entrant);
    in[0].entrantJson.n = sizeof(entrant) - 1;
    in[0].fill |= kPlayerFillProfile;

    Manifest m{};
    m.playerCount = 2;
    m.players = in;
    std::vector<uint8_t> bytes;
    SerializeManifest(m, bytes);

    // The mutant this catches is a record whose size moved: a field dropped, added or reordered.
    uint32_t recBytes = 0;
    memcpy(&recBytes, bytes.data() + kFirstPlayerRecBytesAt, sizeof(recBytes));
    CHECK(recBytes == kPlayerFixedTotalBytes + (sizeof(entrant) - 1));

    ManifestStore store;
    const char* err = nullptr;
    REQUIRE(DeserializeManifest(bytes.data(), static_cast<uint32_t>(bytes.size()), store, &err));
    const Manifest& got = store.View();
    REQUIRE(got.playerCount == 2);
    for (uint16_t i = 0; i < 2; ++i) {
        const PlayerRecord& a = in[i];
        const PlayerRecord& b = got.players[i];
        CHECK(b.extended == 1);
        CHECK(memcmp(b.displayNameFull, a.displayNameFull, sizeof(a.displayNameFull)) == 0);
        CHECK(memcmp(b.platformStrFull, a.platformStrFull, sizeof(a.platformStrFull)) == 0);
        CHECK(b.teamId16 == a.teamId16);
        CHECK(b.teamSlot16 == a.teamSlot16);
        CHECK(b.alignment16 == a.alignment16);
        CHECK(b.firstSeenUs == a.firstSeenUs);
        CHECK(b.lastSeenUs == a.lastSeenUs);
        CHECK(b.fill == a.fill);
        CHECK(b.jsonTruncated == 0);
        CHECK(b.xplatformId == a.xplatformId);
        CHECK(b.userSlot == a.userSlot);
        CHECK(b.generation == a.generation);
        CHECK(b.loadoutNumber == a.loadoutNumber);
        CHECK(b.loadoutNumberBody == a.loadoutNumberBody);
        CHECK(memcmp(b.loadoutSlots, a.loadoutSlots, sizeof(a.loadoutSlots)) == 0);
    }
    // A player with no team is not a player on team 0.
    CHECK(got.players[1].teamId16 == kPlayerNoTeam16);
    CHECK((got.players[1].fill & kPlayerFillTeam) == 0);
    REQUIRE(got.players[0].entrantJson.n == sizeof(entrant) - 1);
    CHECK(memcmp(got.players[0].entrantJson.p, entrant, sizeof(entrant) - 1) == 0);

    std::vector<uint8_t> again;
    SerializeManifest(got, again);
    REQUIRE(again.size() == bytes.size());
    CHECK(memcmp(again.data(), bytes.data(), again.size()) == 0);
}

TEST_CASE("a record written before the v1 block still reads, and says it was never asked") {
    PlayerRecord in = SynthPlayer("Old Capture", 2, 0);
    in.teamId = 2;
    in.teamSlot = 2;
    in.alignment = 0;
    Manifest m{};
    m.playerCount = 1;
    m.players = &in;
    std::vector<uint8_t> v1;
    SerializeManifest(m, v1);

    const std::vector<uint8_t> v0 = WithFixedBlockOf(v1, kPlayerFixedV0OnDisk, 0);
    ManifestStore store;
    const char* err = nullptr;
    REQUIRE(DeserializeManifest(v0.data(), static_cast<uint32_t>(v0.size()), store, &err));
    const PlayerRecord& p = store.View().players[0];
    CHECK(p.extended == 0);
    CHECK(p.fill == 0);
    CHECK(p.firstSeenUs == kPlayerTimeNone);
    CHECK(p.lastSeenUs == kPlayerTimeNone);
    CHECK(p.teamId16 == 2);  // widened out of the 8-bit field, not invented
    CHECK(p.teamSlot16 == 2);
    CHECK(p.alignment16 == 0);
    CHECK(memcmp(p.displayNameFull, "Old Capture", 12) == 0);  // seeded from the 32-byte field
    CHECK(memcmp(p.platformStrFull, "OVR-ORG-4242", 13) == 0);

    // 0xFF is the only absence B.0 could express, and it is the only value that widens to "no team".
    in.teamId = 0xFF;
    in.teamSlot = 0xFF;
    in.alignment = 0xFF;
    std::vector<uint8_t> v1b;
    SerializeManifest(m, v1b);
    const std::vector<uint8_t> v0b = WithFixedBlockOf(v1b, kPlayerFixedV0OnDisk, 0);
    ManifestStore store2;
    REQUIRE(DeserializeManifest(v0b.data(), static_cast<uint32_t>(v0b.size()), store2, &err));
    const PlayerRecord& q = store2.View().players[0];
    CHECK(q.teamId16 == kPlayerNoTeam16);
    CHECK(q.teamSlot16 == kPlayerNoTeam16);
    CHECK(q.alignment16 == kPlayerNoAlignment16);
}

TEST_CASE("a json blob over the cap is stored at the cap and flagged, never silently cut") {
    std::vector<uint8_t> big(kPlayerJsonCap + 777);
    for (size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<uint8_t>('a' + (i % 26));
    }
    PlayerRecord in = SynthPlayer("Loud Profile", 1, 1);
    in.fill |= kPlayerFillProfile;
    in.entrantJson.p = big.data();
    in.entrantJson.n = static_cast<uint32_t>(big.size());
    in.unlocksJson.p = big.data();
    in.unlocksJson.n = 12;
    Manifest m{};
    m.playerCount = 1;
    m.players = &in;
    std::vector<uint8_t> bytes;
    SerializeManifest(m, bytes);

    // The record is sized by what was STORED, not by what was offered.
    uint32_t recBytes = 0;
    memcpy(&recBytes, bytes.data() + kFirstPlayerRecBytesAt, sizeof(recBytes));
    CHECK(recBytes == kPlayerFixedTotalBytes + kPlayerJsonCap + 12);

    ManifestStore store;
    const char* err = nullptr;
    REQUIRE(DeserializeManifest(bytes.data(), static_cast<uint32_t>(bytes.size()), store, &err));
    const PlayerRecord& p = store.View().players[0];
    CHECK(p.entrantJson.n == kPlayerJsonCap);
    CHECK((p.jsonTruncated & kPlayerJsonTruncEntrant) != 0);
    CHECK((p.jsonTruncated & kPlayerJsonTruncUnlocks) == 0);
    CHECK(memcmp(p.entrantJson.p, big.data(), kPlayerJsonCap) == 0);  // the FIRST cap bytes
    CHECK(p.unlocksJson.n == 12);
}

TEST_CASE("a longer extension is skipped and a shorter one is a named refusal") {
    PlayerRecord in = SynthPlayer("Future Writer", 1, 3);
    Manifest m{};
    m.playerCount = 1;
    m.players = &in;
    std::vector<uint8_t> v1;
    SerializeManifest(m, v1);

    // A later minor's longer fixed block: its tail is skipped and every field this version knows is
    // read where it always was.
    const std::vector<uint8_t> longer = WithFixedBlockOf(v1, kPlayerFixedTotalBytes + 24, 0);
    ManifestStore store;
    const char* err = nullptr;
    REQUIRE(DeserializeManifest(longer.data(), static_cast<uint32_t>(longer.size()), store, &err));
    const PlayerRecord& p = store.View().players[0];
    CHECK(p.extended == 1);
    CHECK(p.teamId16 == 1);
    CHECK(p.alignment16 == 3);
    CHECK(p.fill == in.fill);

    // A block between v0 and v1 is a record no version ever wrote: reading it would mean guessing
    // which fields arrived, so it is refused by name.
    const std::vector<uint8_t> partial = WithFixedBlockOf(v1, kPlayerFixedV0OnDisk + 40, 0);
    ManifestStore store2;
    const char* err2 = nullptr;
    CHECK_FALSE(DeserializeManifest(partial.data(), static_cast<uint32_t>(partial.size()), store2, &err2));
    REQUIRE(err2 != nullptr);
    CHECK(std::string(err2).find("extension block") != std::string::npos);
}

TEST_CASE("the 08-01-50 capture opens with an empty roster and round-trips byte for byte") {
    DemoReader r;
    if (!r.Open(kL2Capture0801.c_str())) {
        MESSAGE("NOT RUN: the capture is not on this machine at " << "2026-09-04_08-01-50_*.echodemo");
        return;
    }
    const Manifest& m = r.GetManifest();
    // ROSTER 0 is an answer: this host had no entrants. It is not a file whose roster is malformed.
    CHECK(m.playerCount == 0);
    CHECK(m.players == nullptr);

    std::vector<uint8_t> a;
    SerializeManifest(m, a);
    ManifestStore store;
    const char* err = nullptr;
    REQUIRE(DeserializeManifest(a.data(), static_cast<uint32_t>(a.size()), store, &err));
    CHECK(store.View().playerCount == 0);
    std::vector<uint8_t> b;
    SerializeManifest(store.View(), b);
    REQUIRE(a.size() == b.size());
    CHECK(memcmp(a.data(), b.data(), a.size()) == 0);
}

// ---------------------------------------------------------------------------------------------
// The pseudonymiser, exercised the way echodemo actually runs it: a child process, a fixture on disk.
//
// It cannot be checked in-process. The decision is taken once from the environment at startup and the
// alias map lives for the life of that ONE process, so "the same value keeps one alias" and "two runs
// with different salts are not comparable" are statements about whole processes, not about a function.
//
// Each child gets an environment built from this one with ECHOREPLAY_HARNESS and
// ECHOREPLAY_PSEUDONYM_SALT removed and only the arm's own values put back, so the no-harness arm still
// means no harness when the suite itself is run under check.ps1 with the harness set.

namespace {

// Distinctive on purpose. A name that shared a substring with anything else echodemo prints -- the
// synthetic roster's own "player_00", say -- would make the absence checks below pass for the wrong
// reason.
const char* const kRosterNames[] = {"Vorpal_Hedgehog_Qz", "Marmalade_Trilobite_Wx",
                                    "Persimmon_Kestrel_Yv"};
const size_t kRosterCount = sizeof(kRosterNames) / sizeof(kRosterNames[0]);

std::wstring RosterArg() {
    std::wstring s;
    for (size_t i = 0; i < kRosterCount; ++i) {
        if (i) {
            s += L',';
        }
        for (const char* c = kRosterNames[i]; *c; ++c) {
            s += static_cast<wchar_t>(*c);
        }
    }
    return s;
}

bool EntryNamed(const wchar_t* entry, const wchar_t* name) {
    const size_t n = wcslen(name);
    return _wcsnicmp(entry, name, n) == 0 && entry[n] == L'=';
}

void AppendVar(std::vector<wchar_t>& env, const wchar_t* name, const wchar_t* value) {
    std::wstring s(name);
    s += L'=';
    s += value;
    env.insert(env.end(), s.c_str(), s.c_str() + s.size() + 1);
}

// harness == nullptr means the variable is absent, which is the only way to say "not under the harness"
// to a tool that keys on presence rather than on value.
std::vector<wchar_t> ChildEnv(const wchar_t* harness, const wchar_t* salt) {
    std::vector<wchar_t> env;
    wchar_t* all = GetEnvironmentStringsW();
    for (wchar_t* p = all; *p;) {
        const size_t n = wcslen(p);
        if (!EntryNamed(p, L"ECHOREPLAY_HARNESS") && !EntryNamed(p, L"ECHOREPLAY_PSEUDONYM_SALT")) {
            env.insert(env.end(), p, p + n + 1);
        }
        p += n + 1;
    }
    FreeEnvironmentStringsW(all);
    if (harness) {
        AppendVar(env, L"ECHOREPLAY_HARNESS", harness);
    }
    if (salt) {
        AppendVar(env, L"ECHOREPLAY_PSEUDONYM_SALT", salt);
    }
    env.push_back(0);
    return env;
}

struct ChildRun {
    bool launched = false;
    DWORD exitCode = STILL_ACTIVE;  // "no exit observed", in the OS's own spelling for it
    std::string out;
};

ChildRun RunChild(const std::wstring& cmdLine, std::vector<wchar_t>& env) {
    ChildRun r;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        return r;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd(cmdLine.begin(), cmdLine.end());
    cmd.push_back(0);
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, env.data(), nullptr, &si,
                        &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return r;
    }
    CloseHandle(wr);
    char buf[2048];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got != 0) {
        r.out.append(buf, got);
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 60000);
    GetExitCodeProcess(pi.hProcess, &r.exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    r.launched = true;
    return r;
}

// Read the fields back out of the row the tool printed, rather than out of a second copy of the format.
struct RosterRow {
    std::string name, plat, xplat;
};

std::string FieldAfter(const std::string& line, const char* key) {
    const size_t at = line.find(key);
    if (at == std::string::npos) {
        return std::string();
    }
    size_t b = at + strlen(key);
    while (b < line.size() && line[b] == ' ') {
        ++b;
    }
    size_t e = b;
    while (e < line.size() && line[e] != ' ') {
        ++e;
    }
    return line.substr(b, e - b);
}

std::vector<RosterRow> ParseRoster(const std::string& out) {
    std::vector<RosterRow> rows;
    for (size_t at = 0; at < out.size();) {
        size_t eol = out.find('\n', at);
        if (eol == std::string::npos) {
            eol = out.size();
        }
        const std::string line = out.substr(at, eol - at);
        at = eol + 1;
        if (line.find("] ") != std::string::npos && line.find(" plat ") != std::string::npos) {
            RosterRow r;
            r.name = FieldAfter(line, "] ");
            r.plat = FieldAfter(line, " plat ");
            r.xplat = FieldAfter(line, " xplat ");
            rows.push_back(r);
        }
    }
    return rows;
}

bool IsAlias(const std::string& s) {
    const std::string prefix("player");
    if (s.size() != prefix.size() + 2 || s.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    for (size_t i = prefix.size(); i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

void CheckNoRawNames(const std::string& out) {
    for (size_t i = 0; i < kRosterCount; ++i) {
        INFO("name ", kRosterNames[i]);
        CHECK(out.find(kRosterNames[i]) == std::string::npos);
    }
}

}  // namespace

TEST_CASE("verify accepts an S_ORACLE stream whose records are not all one width") {
    const std::wstring mkdemo = SiblingExe(L"container_mkdemo.exe");
    const std::wstring tool = SiblingExe(L"echodemo.exe");
    REQUIRE(GetFileAttributesW(mkdemo.c_str()) != INVALID_FILE_ATTRIBUTES);
    REQUIRE(GetFileAttributesW(tool.c_str()) != INVALID_FILE_ATTRIBUTES);

    const std::wstring demo = Scratch(L"oraclewidth.echodemo");
    DeleteFileW(demo.c_str());
    std::vector<wchar_t> plain = ChildEnv(nullptr, nullptr);
    const ChildRun made = RunChild(
        L"\"" + mkdemo + L"\" \"" + demo + L"\" --chapters 12 --oracle-variable", plain);
    REQUIRE(made.launched);
    INFO("mkdemo: ", made.out);
    REQUIRE(made.exitCode == 0);

    ChildRun v = RunChild(L"\"" + tool + L"\" verify \"" + demo + L"\"", plain);
    REQUIRE(v.launched);
    INFO("verify: ", v.out);
    // Not the exit code: this synthetic file trips unrelated checks (it carries no TAP_REGISTRATION),
    // so a two-state pass/fail cannot see the width rule. The width rule fails a block BY NAME, and
    // that named failure is what a stream of variable-width records used to produce on every block
    // wider than the first one verify happened to learn from.
    CHECK(v.out.find("is not the") == std::string::npos);
    CHECK(v.out.find("-byte record this stream has used") == std::string::npos);

    // The row itself: 117 records emitted, 117 stored, nothing lost. Read from the report rather
    // than assumed, so a fixture that stopped carrying S_ORACLE fails here instead of passing.
    const size_t row = v.out.find("S_ORACLE");
    REQUIRE(row != std::string::npos);
    unsigned long long observed = 0, stored = 0, accounted = 1;
    char name[32] = {};
    REQUIRE(sscanf_s(v.out.c_str() + row, "%31s %llu %llu %llu", name, unsigned(sizeof name),
                     &observed, &stored, &accounted) == 4);
    CHECK(stored == 117u);
    CHECK(observed == stored);
    CHECK(accounted == 0u);
}

TEST_CASE("echodemo pseudonymises the roster under the harness") {
    const std::wstring mkdemo = SiblingExe(L"container_mkdemo.exe");
    const std::wstring tool = SiblingExe(L"echodemo.exe");
    REQUIRE(GetFileAttributesW(mkdemo.c_str()) != INVALID_FILE_ATTRIBUTES);
    REQUIRE(GetFileAttributesW(tool.c_str()) != INVALID_FILE_ATTRIBUTES);

    const std::wstring demo = Scratch(L"pseudo.echodemo");
    DeleteFileW(demo.c_str());
    std::vector<wchar_t> plain = ChildEnv(nullptr, nullptr);
    const ChildRun made = RunChild(
        L"\"" + mkdemo + L"\" \"" + demo + L"\" --chapters 2 --roster " + RosterArg(), plain);
    REQUIRE(made.launched);
    INFO("mkdemo: ", made.out);
    REQUIRE(made.exitCode == 0);

    const std::wstring info = L"\"" + tool + L"\" info \"" + demo + L"\"";
    const std::string roster = "ROSTER " + std::to_string(kRosterCount);

    // The control first. Without the harness the raw names ARE printed -- which is what makes their
    // absence in every arm below a fact about the pseudonymiser rather than about the fixture.
    ChildRun bare = RunChild(info, plain);
    REQUIRE(bare.launched);
    CHECK(bare.exitCode == 0);
    CHECK(bare.out.find(roster + " pseudonymised=no") != std::string::npos);
    for (size_t i = 0; i < kRosterCount; ++i) {
        INFO("name ", kRosterNames[i]);
        CHECK(bare.out.find(kRosterNames[i]) != std::string::npos);
    }

    std::vector<wchar_t> harnessA = ChildEnv(L"1", L"0");
    ChildRun a = RunChild(info, harnessA);
    REQUIRE(a.launched);
    CHECK(a.exitCode == 0);
    CHECK(a.out.find(roster + " pseudonymised=yes") != std::string::npos);
    CheckNoRawNames(a.out);

    std::vector<RosterRow> rowsA = ParseRoster(a.out);
    REQUIRE(rowsA.size() == kRosterCount);
    for (size_t i = 0; i < rowsA.size(); ++i) {
        INFO("row ", i, " name ", rowsA[i].name, " plat ", rowsA[i].plat, " xplat ", rowsA[i].xplat);
        CHECK(IsAlias(rowsA[i].name));
        CHECK(IsAlias(rowsA[i].plat));
        CHECK(IsAlias(rowsA[i].xplat));
    }
    // One pseudonym per distinct value, stable for the life of the run: every row of this fixture
    // carries the same platform string, so every row must carry the same alias for it.
    for (size_t i = 1; i < rowsA.size(); ++i) {
        CHECK(rowsA[i].plat == rowsA[0].plat);
    }
    // Never reused: distinct names are distinct aliases, because two players rendered as one name
    // would be a false fact.
    for (size_t i = 0; i < rowsA.size(); ++i) {
        for (size_t j = i + 1; j < rowsA.size(); ++j) {
            CHECK(rowsA[i].name != rowsA[j].name);
        }
    }

    // A second run of the same file under a different salt. The sequence is first-seen order, so the
    // salt is the only thing that can move the numbering -- if it is ignored, these are equal.
    std::vector<wchar_t> harnessB = ChildEnv(L"1", L"41");
    ChildRun b = RunChild(info, harnessB);
    REQUIRE(b.launched);
    CHECK(b.exitCode == 0);
    CheckNoRawNames(b.out);
    std::vector<RosterRow> rowsB = ParseRoster(b.out);
    REQUIRE(rowsB.size() == kRosterCount);
    for (size_t i = 0; i < rowsB.size(); ++i) {
        INFO("row ", i, " A ", rowsA[i].name, " B ", rowsB[i].name);
        CHECK(rowsB[i].name != rowsA[i].name);
    }
    CHECK(rowsB[0].plat != rowsA[0].plat);
    // Same raw platform string, so B is internally stable too -- a mutant that made every alias unique
    // per lookup would satisfy the inequality above and fail here.
    for (size_t i = 1; i < rowsB.size(); ++i) {
        CHECK(rowsB[i].plat == rowsB[0].plat);
    }

    // --raw under the harness is refused BY NAME, not ignored: a caller that asked for personal data in
    // archived output gets an answer.
    ChildRun refused = RunChild(info + L" --raw", harnessA);
    REQUIRE(refused.launched);
    CHECK(refused.exitCode != 0);
    INFO("refusal: ", refused.out);
    CHECK(refused.out.find("REFUSED") != std::string::npos);
    CHECK(refused.out.find("--raw") != std::string::npos);
    CheckNoRawNames(refused.out);
    CHECK(refused.out.find(roster) == std::string::npos);

    DeleteFileW(demo.c_str());
}


// ---- S_PLAYER_STATS (ARCH-21 MAJOR-B2) --------------------------------------------------------
//
// The engine's per-entrant live match block reached no artifact before this stream existed. What is
// asserted here is that it round-trips byte for byte, that the fill mask is load-bearing in both
// directions, and that every way of malforming the stream is refused BY NAME rather than shrugged at.

namespace {

// A record every check below can start from: fully claimed, in range, and refused by nothing.
PlayerStatsRecordDisk GoodStatsRecord(uint64_t id, uint64_t engineUs) {
    PlayerStatsRecord r{};
    r.xplatformId = id;
    r.userIndex = 0;
    r.engineTimeUs = engineUs;
    r.captureTimeUs = engineUs;
    r.possessionTime = 12.5f;
    r.points = 6;
    r.goals = 3;
    r.stuns = 2;
    r.blocks = 1;
    r.assists = 4;
    r.saves = 5;
    r.interceptions = 7;
    r.steals = 8;
    r.catches = 9;
    r.passes = 11;
    r.shotsTaken = 13;
    r.ping = 42;
    r.packetLossRatio = 0.25f;
    r.isEmotePlaying = 1;
    r.keyframe = 1;
    r.fill = kPlayerStatsFillAll;

    PlayerStatsRecordDisk d{};
    PlayerStatsToDisk(r, d);
    return d;
}

const char* CheckOne(const PlayerStatsRecordDisk& d) {
    PlayerStatsCursor cursor{};
    return CheckPlayerStatsBlock(&d, sizeof(d), cursor);
}

}  // namespace

TEST_CASE("a player stats record survives the round trip and the block width is the record size") {
    const std::wstring path = ScratchDir() + L"playerstats.echodemo";
    synth::Config cfg;
    cfg.chapters = 4;
    cfg.ticksPerChapter = 8;
    REQUIRE(synth::Write(path.c_str(), cfg, nullptr, nullptr));

    DemoReader r;
    REQUIRE(r.Open(path.c_str()));

    uint32_t records = 0, keyframes = 0, onChange = 0;
    std::vector<PlayerStatsRecordDisk> all;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        DecodedChapter c{};
        if (!r.DecodeChapter(i, c)) {
            continue;
        }
        for (uint16_t b = 0; b < c.streamCount; ++b) {
            const StreamView& v = c.streams[b];
            if (v.id != kStreamPlayerStats) {
                continue;
            }
            // The width is read off the bytes on disk, not asserted from sizeof: a writer that emitted
            // a differently sized record would divide wrong here, not agree with itself.
            REQUIRE(v.storedCount > 0);
            CHECK(v.bytes % v.storedCount == 0);
            CHECK(v.bytes / v.storedCount == sizeof(PlayerStatsRecordDisk));
            for (uint32_t k = 0; k < v.storedCount; ++k) {
                PlayerStatsRecordDisk d{};
                memcpy(&d, v.records + size_t(k) * sizeof(d), sizeof(d));
                all.push_back(d);
                ++records;
                if (d.keyframe) {
                    ++keyframes;
                } else {
                    ++onChange;
                }
            }
        }
    }

    // Both writer paths ran: a keyframe copy per player per chapter, and at least one record written
    // because a value moved. A fixture that only ever exercised one of them could not tell them apart.
    CHECK(keyframes == cfg.chapters * cfg.statsPlayers);
    CHECK(onChange > 0);
    CHECK(records == keyframes + onChange);

    // Every value, against the plan that wrote it.
    for (uint32_t c = 0; c < cfg.chapters; ++c) {
        for (uint32_t who = 0; who < cfg.statsPlayers; ++who) {
            PlayerStatsRecord want{};
            synth::PlanPlayerStats(cfg, c, who, want);
            bool found = false;
            for (const PlayerStatsRecordDisk& d : all) {
                if (d.xplatformId != want.xplatformId || !d.keyframe ||
                    d.captureTimeUs != want.captureTimeUs) {
                    continue;
                }
                found = true;
                PlayerStatsRecord got{};
                PlayerStatsFromDisk(d, got);
                CHECK(got.fill == want.fill);
                CHECK(got.possessionTime == want.possessionTime);
                CHECK(got.points == want.points);
                CHECK(got.goals == want.goals);
                CHECK(got.stuns == want.stuns);
                CHECK(got.blocks == want.blocks);
                CHECK(got.assists == want.assists);
                CHECK(got.saves == want.saves);
                CHECK(got.interceptions == want.interceptions);
                CHECK(got.steals == want.steals);
                CHECK(got.catches == want.catches);
                CHECK(got.passes == want.passes);
                CHECK(got.shotsTaken == want.shotsTaken);
                CHECK(got.ping == want.ping);
                CHECK(got.userIndex == want.userIndex);
                CHECK(got.isEmotePlaying == want.isEmotePlaying);
                // The one field the fixture never claims: it must read back as absent, not as 0.0.
                if ((want.fill & kPsFillPacketLoss) == 0) {
                    CHECK(got.packetLossRatio == kPlayerStatFloatNone);
                } else {
                    CHECK(got.packetLossRatio == want.packetLossRatio);
                }
                break;
            }
            INFO("chapter ", c, " player ", who);
            CHECK(found);
        }
    }

    // S_PLAYER_STATS is composed by the writer, so it stays out of the completeness law -- a stream
    // whose `observed` could only be our own loop count must not be given a fabricated denominator.
    CHECK(StreamCarriesSamples(kStreamPlayerStats) == false);
    CHECK(std::string(StreamName(kStreamPlayerStats)) == "S_PLAYER_STATS");

    r.Close();
    DeleteFileW(path.c_str());
}

TEST_CASE("the player stats fill mask decides what reaches the disk") {
    // A producer that filled nothing: every value must land as its absence value, so a zero-initialised
    // record is read as "not recorded" and never as a scoreboard of zeroes.
    PlayerStatsRecord blank{};
    blank.xplatformId = 0x77;
    blank.userIndex = 3;
    blank.engineTimeUs = 10;
    blank.goals = 9;  // set, but unclaimed: the writer must drop it
    blank.possessionTime = 3.5f;
    blank.packetLossRatio = 0.5f;
    blank.isEmotePlaying = 1;
    blank.fill = 0;

    PlayerStatsRecordDisk d{};
    PlayerStatsToDisk(blank, d);
    CHECK(d.goals == kPlayerStatNone);
    CHECK(d.points == kPlayerStatNone);
    CHECK(d.ping == kPlayerStatNone);
    CHECK(d.possessionTime == kPlayerStatFloatNone);
    CHECK(d.packetLossRatio == kPlayerStatFloatNone);
    CHECK(d.isEmotePlaying == kPlayerStatEmoteNone);
    CHECK(CheckOne(d) == nullptr);

    // One bit set: exactly that value survives and the rest are still absent.
    blank.fill = kPsFillGoals;
    PlayerStatsToDisk(blank, d);
    CHECK(d.goals == 9);
    CHECK(d.points == kPlayerStatNone);
    CHECK(d.possessionTime == kPlayerStatFloatNone);
    CHECK(CheckOne(d) == nullptr);

    // And the other direction, which is the half a writer cannot enforce for a hand-built record: a
    // value present with no bit claiming it is a number nothing vouches for.
    d.points = 4;
    const std::string unclaimed(CheckOne(d) ? CheckOne(d) : "(accepted)");
    INFO(unclaimed);
    CHECK(std::string(CheckOne(d)) ==
          "an S_PLAYER_STATS record carries a stat its fill mask does not claim");

    // A claimed stat holding the absence value is the same fact read the other way round.
    PlayerStatsRecordDisk good = GoodStatsRecord(0x77, 10);
    CHECK(CheckOne(good) == nullptr);
    good.goals = kPlayerStatNone;
    CHECK(std::string(CheckOne(good)) ==
          "an S_PLAYER_STATS record claims a stat whose value is the absence value");
}

TEST_CASE("player stats engine time never goes backwards for a user") {
    PlayerStatsRecordDisk two[2] = {GoodStatsRecord(0x11, 140), GoodStatsRecord(0x11, 70)};
    PlayerStatsCursor cursor{};
    const char* why = CheckPlayerStatsBlock(two, sizeof(two), cursor);
    REQUIRE(why != nullptr);
    CHECK(std::string(why).find("engine time went backwards") != std::string::npos);

    // Equal times are not backwards: two records at one instant is what a keyframe copy plus an
    // on-change record in the same tick looks like.
    PlayerStatsRecordDisk same[2] = {GoodStatsRecord(0x11, 140), GoodStatsRecord(0x11, 140)};
    PlayerStatsCursor c2{};
    CHECK(CheckPlayerStatsBlock(same, sizeof(same), c2) == nullptr);

    // The rule is PER USER: another user's earlier time is not a violation, and a check that compared
    // against one global clock would refuse this valid block.
    PlayerStatsRecordDisk twoUsers[2] = {GoodStatsRecord(0x11, 140), GoodStatsRecord(0x22, 70)};
    PlayerStatsCursor c3{};
    CHECK(CheckPlayerStatsBlock(twoUsers, sizeof(twoUsers), c3) == nullptr);
    CHECK(c3.n == 2);

    // The cursor spans blocks, so an out-of-order record in a LATER chapter is caught too.
    PlayerStatsRecordDisk later = GoodStatsRecord(0x11, 110);
    CHECK(CheckPlayerStatsBlock(&later, sizeof(later), c3) != nullptr);

    // And the writer refuses to produce such a file in the first place.
    const std::wstring path = ScratchDir() + L"statsbackwards.echodemo";
    synth::Config cfg;
    cfg.chapters = 1;
    cfg.ticksPerChapter = 4;
    Manifest m{};
    synth::BuildManifest(cfg, m);
    DemoWriter w;
    REQUIRE(w.Open(path.c_str(), m));
    w.BeginChapter(kSectionCore, 70);
    PlayerStatsRecord r{};
    PlayerStatsFromDisk(GoodStatsRecord(0x11, 140), r);
    CHECK(w.WritePlayerStats(r));
    r.engineTimeUs = 70;
    CHECK_FALSE(w.WritePlayerStats(r));
    const std::string backwards(w.LastError() ? w.LastError() : "(no error)");
    INFO(backwards);
    CHECK(backwards.find("went backwards") != std::string::npos);
    w.EndChapter(210);
    w.Close();
    DeleteFileW(path.c_str());
}

TEST_CASE("a malformed player stats block is refused by name") {
    const PlayerStatsRecordDisk good = GoodStatsRecord(0x33, 500);
    PlayerStatsCursor control{};
    // The control: nothing below fires on a record this format did write. Without it every refusal
    // could be a check that refuses everything.
    CHECK(CheckPlayerStatsBlock(&good, sizeof(good), control) == nullptr);

    uint8_t cut[sizeof(PlayerStatsRecordDisk) - 1];
    memcpy(cut, &good, sizeof(cut));
    PlayerStatsCursor c1{};
    CHECK(std::string(CheckPlayerStatsBlock(cut, sizeof(cut), c1)) ==
          "an S_PLAYER_STATS block is not a whole number of records");

    PlayerStatsRecordDisk d = good;
    d.fill |= (kPlayerStatsFillAll + 1u);
    CHECK(std::string(CheckOne(d)) == "an S_PLAYER_STATS record's fill mask has an undefined bit");

    d = good;
    d.xplatformId = 0;
    d.userIndex = kPlayerStatsNoIndex;
    CHECK(std::string(CheckOne(d)) == "an S_PLAYER_STATS record identifies no player");

    d = good;
    d.keyframe = 2;
    CHECK(std::string(CheckOne(d)) ==
          "an S_PLAYER_STATS record's keyframe flag is neither set nor clear");

    d = good;
    d.isEmotePlaying = 7;
    CHECK(std::string(CheckOne(d)) ==
          "an S_PLAYER_STATS record's emote flag is neither true nor false");

    // An unclaimed emote flag holding anything but its absence value is the same defect the numeric
    // stats have, and it is named the same way.
    d = good;
    d.fill &= ~kPsFillEmote;
    CHECK(std::string(CheckOne(d)) ==
          "an S_PLAYER_STATS record carries a stat its fill mask does not claim");
    d.isEmotePlaying = kPlayerStatEmoteNone;
    CHECK(CheckOne(d) == nullptr);

    // A record naming its player by index alone is legal: a capture that could not source an account
    // id still knows which roster row it read.
    d = good;
    d.xplatformId = 0;
    d.userIndex = 2;
    CHECK(CheckOne(d) == nullptr);

    // More users than a match can hold: the cursor refuses rather than forgetting the oldest, which
    // would silently stop checking the monotone rule for whoever fell out.
    PlayerStatsCursor full{};
    for (uint16_t i = 0; i < kPlayerStatsMaxUsers; ++i) {
        const PlayerStatsRecordDisk one = GoodStatsRecord(0x70 + i, 100);
        INFO("user ", i);
        REQUIRE(CheckPlayerStatsBlock(&one, sizeof(one), full) == nullptr);
    }
    const PlayerStatsRecordDisk overflow = GoodStatsRecord(0x9999, 100);
    CHECK(CheckPlayerStatsBlock(&overflow, sizeof(overflow), full) != nullptr);
}

TEST_CASE("R15: a round table that does not ascend degrades the ROUNDS block, not the file") {
    // L5-RUN-14 run 2, off the file on disk: `ROUNDS refused: round table numbers do not ascend:
    // row 1 is 0 after 0`. The shipped reader refused the whole capture over that pair and 55 099
    // bytes of footage went with it. The rounds feature is not the recording.
    TabledManifest t({0, 7}, MarkerSource::Local);
    const size_t rowAt = t.headAt + sizeof(RoundTableHeadDisk) + sizeof(RoundRowDisk);
    RoundRowDisk d{};
    memcpy(&d, t.bytes.data() + rowAt, sizeof(d));
    d.number = 0;
    memcpy(t.bytes.data() + rowAt, &d, sizeof(d));

    ManifestStore store;
    const char* err = nullptr;
    REQUIRE(DeserializeManifest(t.bytes.data(), static_cast<uint32_t>(t.bytes.size()), store, &err));
    const Manifest& got = store.View();
    REQUIRE(got.roundsRefused != nullptr);
    CHECK(std::string(got.roundsRefused) == "round table numbers do not ascend: row 1 is 0 after 0");
    CHECK(got.roundRowCount == 0);
    CHECK(got.markerSource == MarkerSource::NotRecorded);

    // Everything the file is actually FOR survived the refusal.
    CHECK(got.playerCount == t.m.playerCount);
    CHECK(got.schemaCount == t.m.schemaCount);
    CHECK(got.levelId == t.m.levelId);

    // A good table in the same shape is not refused, so the check above is not passing on everything.
    TabledManifest ok({0, 7}, MarkerSource::Local);
    ManifestStore store2;
    REQUIRE(DeserializeManifest(ok.bytes.data(), static_cast<uint32_t>(ok.bytes.size()), store2, &err));
    CHECK(store2.View().roundsRefused == nullptr);
    CHECK(store2.View().roundRowCount == 2);
    CHECK(store2.View().markerSource == MarkerSource::Local);
}

// ---- L2-RUN-8 F10: the present mask on the file that actually exists ---------------------------
//
// The numbering lives in demotypes.h and nowhere else, so this test reads the real capture through
// the format's own names. It is the retroactive proof the remedy is measured against: under the
// 13-bit numbering the header carried until 2026-09-05 this file decoded as gameTimer ABSENT and
// countdownTimer/firstLaunchOfRound PRESENT, which is the exact inversion of what the sampler wrote.

namespace {

// F-REAL-3, md5 4baf2ec11acaf6d05eee1317d30d0a53. Read-only, and outside the repository.
const std::wstring kRealF3 =
    replay_test::CaptureFileW(L"2026-09-05_14-36-55_0000000000000000_0000000000000000.echodemo");

}  // namespace

TEST_CASE("the real capture's S_GAMESTATE present mask decodes through the format's own numbering") {
    DemoReader r;
    if (!r.Open(kRealF3.c_str())) {
        MESSAGE("NOT RUN: F-REAL-3 is not on this machine at "
                << "<captures>/2026-09-05_14-36-55_*.echodemo (set REPLAY_CAPTURE_DIR)");
        return;
    }

    uint32_t records = 0;
    uint32_t presentUnion = 0;
    uint32_t presentAlways = UINT32_MAX;
    uint32_t countdownNonZero = 0;
    uint32_t firstLaunchNonZero = 0;
    float timerMin = 1e30f;
    float timerMax = -1e30f;
    std::vector<float> timers;

    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        DecodedChapter c{};
        if (!r.DecodeChapter(i, c)) {
            continue;
        }
        for (uint16_t b = 0; b < c.streamCount; ++b) {
            const StreamView& v = c.streams[b];
            if (v.id != kStreamGameState) {
                continue;
            }
            REQUIRE(v.bytes % sizeof(GameStateRecordDisk) == 0);
            const uint32_t n = v.bytes / static_cast<uint32_t>(sizeof(GameStateRecordDisk));
            for (uint32_t g = 0; g < n; ++g) {
                GameStateRecordDisk d{};
                memcpy(&d, v.records + size_t(g) * sizeof(d), sizeof(d));
                ++records;
                presentUnion |= d.present;
                presentAlways &= d.present;
                if (d.countdownTimer != 0.0f) {
                    ++countdownNonZero;
                }
                if (d.firstLaunchOfRound != 0) {
                    ++firstLaunchNonZero;
                }
                if (d.present & kGsGameTimer) {
                    timers.push_back(d.gameTimer);
                    if (d.gameTimer < timerMin) {
                        timerMin = d.gameTimer;
                    }
                    if (d.gameTimer > timerMax) {
                        timerMax = d.gameTimer;
                    }
                }
            }
        }
    }

    // A run that found nothing would satisfy every mask assertion below vacuously.
    REQUIRE(records > 0);
    MESSAGE("F-REAL-3: " << records << " S_GAMESTATE record(s), present union=" << presentUnion);

    // The mask is spelled out of the format's names, never as the literal 0x67D: a bit that moves
    // moves this expectation with it, and the two lines below are then the only thing that can fail.
    const uint32_t expected = kGsPhaseSymbol | kGsRoundCounter | kGsRoundPoints | kGsMatchScore |
                              kGsFrisbeeOwnerId | kGsGameTimer | kGsRoundWinningTeam | kGsLastScore;
    CHECK(presentUnion == expected);
    CHECK(presentAlways == expected);
    CHECK((presentUnion & ~kGameStatePresentAll) == 0);

    // gameTimer is CARRIED: the sampler reads it every tick and this file holds a live match clock.
    CHECK((presentAlways & kGsGameTimer) != 0);
    CHECK(timers.size() == records);
    CHECK(timerMin >= 0.0f);
    CHECK(timerMax > 100.0f);
    CHECK(timerMin < timerMax);
    MESSAGE("F-REAL-3 gameTimer: " << timerMax << " s down to " << timerMin << " s");

    // These three the engine could not answer on this build. A clear bit is the only honest report,
    // and a field whose bit is clear was never written -- so it is still the zero it was born with.
    CHECK((presentUnion & kGsCountdownTimer) == 0);
    CHECK((presentUnion & kGsFirstLaunchOfRound) == 0);
    CHECK((presentUnion & kGsPausedSymbol) == 0);
    CHECK(countdownNonZero == 0);
    CHECK(firstLaunchNonZero == 0);
}

// =================================================================================================
// RECORDER-3 F5 -- the v2 player extension. The `fill` word could not carry "the jersey numbers
// were sourced", so a row that never had them wrote 0, which is also a real jersey 0.
// =================================================================================================

TEST_CASE("RECORDER-3: the v2 player extension round-trips, and a v1 record still reads as v1") {
    Manifest m{};
    m.levelId = 7;
    PlayerRecord rows[2]{};
    rows[0].userSlot = 1;
    rows[0].loadoutNumber = 0;
    rows[0].fill = kPlayerFillSlot;
    rows[0].fillExt = kPlayerFillNumbers | kPlayerFillProfileFromEntrantRec;
    rows[1].userSlot = 2;
    rows[1].loadoutNumber = 0;
    rows[1].fill = kPlayerFillSlot;
    rows[1].fillExt = 0;
    m.playerCount = 2;
    m.players = rows;

    std::vector<uint8_t> bytes;
    SerializeManifest(m, bytes);
    ManifestStore store;
    const char* err = nullptr;
    REQUIRE(DeserializeManifest(bytes.data(), (uint32_t)bytes.size(), store, &err));
    REQUIRE(store.View().playerCount == 2);
    const PlayerRecord& a = store.View().players[0];
    const PlayerRecord& b = store.View().players[1];
    CHECK(a.extendedV2 == 1);
    CHECK((a.fillExt & kPlayerFillNumbers) != 0);
    CHECK((a.fillExt & kPlayerFillProfileFromEntrantRec) != 0);
    // Same zero in the field, and the file now says the two zeros are different facts.
    CHECK(b.extendedV2 == 1);
    CHECK((b.fillExt & kPlayerFillNumbers) == 0);
    CHECK(a.loadoutNumber == b.loadoutNumber);
    // The v2 word never leaks into v1's: an old reader takes `fill` and sees what it always saw.
    CHECK((a.fill & ~kPlayerFillAll) == 0u);

    // A FILE WRITTEN BEFORE THIS EXISTED. The record is cut back to its v1 length, which is
    // byte-for-byte what the previous build wrote, and the reader must say "this file has no
    // opinion" rather than "the numbers were not sourced".
    std::vector<uint8_t> v1 = bytes;
    // The file leads with [u32 fixed-block size][fixed block][u16 playerCount], so the first
    // player record's length word starts there. The size word is read back rather than assumed.
    uint32_t fixedBytes = 0;
    memcpy(&fixedBytes, v1.data(), sizeof(fixedBytes));
    REQUIRE(fixedBytes == 32u);
    const uint32_t at = 4 + fixedBytes + 2;
    uint32_t recBytes = 0;
    memcpy(&recBytes, v1.data() + at, sizeof(recBytes));
    const uint32_t shorter = recBytes - 4;
    memcpy(v1.data() + at, &shorter, sizeof(shorter));
    v1.erase(v1.begin() + at + 4 + shorter, v1.begin() + at + 4 + recBytes);

    ManifestStore old;
    REQUIRE(DeserializeManifest(v1.data(), (uint32_t)v1.size(), old, &err));
    CHECK(old.View().players[0].extendedV2 == 0);
    CHECK(old.View().players[0].fillExt == 0u);
    CHECK(old.View().players[0].extended == 1);   // v1 is still v1, and still fully read
    CHECK(old.View().players[0].fill == a.fill);
}
