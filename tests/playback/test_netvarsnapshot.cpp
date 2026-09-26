// RECORDER-4 / SPEC-RECORDER-2 S2.3. THE KEYFRAME NETVAR SNAPSHOT, END TO END.
//
// The recorder's netvar tap is a listener on the DELTA message, so a key that does not change during
// a capture is in no record and in no keyframe, and playback's NetVarPrimer -- which is where the
// HUD's value-at-T0 comes from -- has nothing for it. The scoreboard then opens on the offline
// host's own residual numbers and stays there until the first goal.
//
// Every case goes through the REAL ChapterBuilder, the REAL container and the REAL ScanNetVarPrimer.
// Only the engine read is a stand-in, because these must build on a machine with no game.
//
// The merge has two rules and each has its case:
//   1. a key no recorded delta has spoken for gets the engine's value, and
//   2. a key a recorded delta HAS spoken for keeps the delta's.
// Rule 2 is not a nicety: the delta stream carries that row, and a keyframe holding a value no
// record in the file agrees with is the file disagreeing with itself.

#include "doctest.h"

#include <windows.h>

#include <stdint.h>
#include <string.h>

#include <vector>

#include "capture/capture_types.h"
#include "capture/chapterbuilder.h"
#include "capture/manifestbuild.h"
#include "foundation/demofile/demofile.h"
#include "playback/core/netvarhud.h"
#include "playback/core/windowbuild.h"

using namespace replay;
using namespace replay::playback;

namespace nvsnap {

struct TempDemo {
    wchar_t path[512];
    explicit TempDemo(const wchar_t* tag) {
        wchar_t dir[400];
        const DWORD n = GetTempPathW(400, dir);
        if (!n) { dir[0] = L'.'; dir[1] = 0; }
        _snwprintf_s(path, 512, _TRUNCATE, L"%lsnvsnap_%ls_%lu.echodemo", dir, tag,
                     (unsigned long)GetCurrentProcessId());
    }
    ~TempDemo() { DeleteFileW(path); }
};

// The recorded slot every row in these files carries. One value, because the snapshot and the delta
// tap must land on the SAME shadow entry for rule 2 to have anything to be about.
const uint16_t kSlot = 1;

// What the stand-in engine reports for key i. Distinct from the delta value below, so a primer entry
// names which of the two sources it came from.
uint64_t EngineValue(size_t i) { return 0x5100ull + i; }

// `blindCalls` is a store that is not bound yet: the first N keyframes get nothing and every key is
// reported unreadable, which is the fresh-spectate-join case.
struct FakeSnapshot : capture::INetvarSnapshotSource {
    uint16_t calls = 0;
    uint16_t blindCalls = 0;

    uint16_t Snapshot(capture::NetvarSnapshotRow* out, uint16_t cap, uint16_t& unreadable) override {
        ++calls;
        if (calls <= blindCalls) {
            unreadable = static_cast<uint16_t>(kHudVarCount);
            return 0;
        }
        uint16_t n = 0;
        for (size_t i = 0; i < kHudVarCount && n < cap; ++i) {
            out[n].name = kHudVars[i].symbol;
            out[n].value = EngineValue(i);
            out[n].slot = kSlot;
            ++n;
        }
        unreadable = 0;
        return n;
    }
};

// The DUV wire body the netvar tap delivers: one row, header exactly as duv.h describes it.
std::vector<uint8_t> DuvBody(uint32_t seq, uint64_t name, uint64_t value) {
    std::vector<uint8_t> b(46, 0);
    const uint64_t owner = 0x1111ull;
    memcpy(b.data(), &owner, 8);
    memcpy(b.data() + 8, &kSlot, 2);
    const uint32_t one = 1;
    memcpy(b.data() + 12, &one, 4);
    memcpy(b.data() + 16, &seq, 4);
    memcpy(b.data() + 24, &seq, 4);
    const uint16_t rows = 1;
    memcpy(b.data() + 28, &rows, 2);
    memcpy(b.data() + 30, &name, 8);
    memcpy(b.data() + 38, &value, 8);
    return b;
}

void FeedNetvar(capture::ChapterBuilder& b, uint32_t seq, uint64_t name, uint64_t value) {
    using namespace replay::capture;
    const std::vector<uint8_t> duv = DuvBody(seq, name, value);
    std::vector<uint8_t> rec(sizeof(RecHeader) + sizeof(NetvarRec) + duv.size());
    RecHeader h{};
    h.kind = kRecNetvar;
    h.bytes = static_cast<uint16_t>(sizeof(NetvarRec) + duv.size());
    NetvarRec nr{};
    nr.arrivalUs = 1;
    nr.bodyBytes = static_cast<uint16_t>(duv.size());
    memcpy(rec.data(), &h, sizeof h);
    memcpy(rec.data() + sizeof h, &nr, sizeof nr);
    memcpy(rec.data() + sizeof h + sizeof nr, duv.data(), duv.size());
    b.Consume(rec.data(), static_cast<uint32_t>(rec.size()));
}

void FeedTick(capture::ChapterBuilder& b, uint64_t tUs, uint32_t frame, uint64_t dt) {
    using namespace replay::capture;
    TickRec tr{};
    tr.sourceTimeUs = tUs;
    tr.simTimeUs = tUs;
    tr.frameIndex = frame;
    tr.frameDeltaUs = static_cast<uint32_t>(dt);
    tr.scaledDeltaUs = static_cast<uint32_t>(dt);
    uint8_t buf[sizeof(RecHeader) + sizeof(TickRec)];
    RecHeader h{};
    h.kind = kRecTick;
    h.bytes = static_cast<uint16_t>(sizeof tr);
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, &tr, sizeof tr);
    b.Consume(buf, static_cast<uint32_t>(sizeof buf));
}

const uint64_t kT0 = 5000000;
const uint64_t kDt = 8333;
const uint32_t kTicks = 600;   // 5 s at 120 Hz over a 2 s keyframe interval: several core chapters
const uint32_t kNoDelta = 0xFFFFFFFFu;  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
const uint64_t kDeltaValue = 0x9999ull;

// One capture. `deltaFromTick` is when the one changing key starts being reported; kNoDelta writes a
// capture with not a single netvar record in it, which is what a scoreless spectate produces.
void WriteDemo(const wchar_t* path, capture::INetvarSnapshotSource* src, uint64_t deltaKey,
               uint32_t deltaFromTick) {
    using namespace replay::capture;

    ManifestBuilder mb;
    mb.Begin(kT0, 0, 0x40);
    const uint8_t md5[16] = {0xca, 0xa0, 0x5e, 0xe3};
    mb.SetBuild(md5, 0x676F6C646D617374ull, 0x0102030405060708ull);
    mb.SetLevel(6300205991959903307ull, 0xF00Dull, 1780177787ull);
    mb.SetKeyframeIntervalUs(2000000);
    mb.SetUuid(1, 2);
    REQUIRE(mb.Refusal() == nullptr);

    DemoWriter writer;
    REQUIRE(writer.Open(path, mb.View()));

    BuilderConfig cfg;
    cfg.keyframeIntervalUs = 2000000;
    for (uint16_t i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;
    for (uint16_t i = 0; i < kSectionCount; ++i) cfg.sectionEnabled[i] = true;

    ChapterBuilder b;
    b.SetNetvarSnapshotSource(src);
    REQUIRE(b.Open(&writer, mb.View(), cfg));
    b.EmitMatchBegin(kT0);

    for (uint32_t f = 0; f < kTicks; ++f) {
        FeedTick(b, kT0 + f * kDt, f + 1, kDt);
        if (deltaFromTick != kNoDelta && f >= deltaFromTick) {
            FeedNetvar(b, 1000 + f, deltaKey, kDeltaValue);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
        }
    }
    b.Close();
    writer.Close();
}

NetVarPrimer PrimerOf(const wchar_t* path) {
    DemoReader reader;
    REQUIRE(reader.Open(path));
    NetVarPrimer p;
    ScanNetVarPrimer(reader, p);
    reader.Close();
    return p;
}

bool PrimerHas(const NetVarPrimer& p, uint64_t key, uint64_t value, uint16_t slot) {
    const size_t i = p.IndexOf(key);
    return i != NetVarPrimer::kMax && p.value[i] == value && p.slot[i] == slot;
}

const size_t kBlueScore = 3;   // kHudVars[3], the key the goal test moves

}  // namespace nvsnap

// ★ THE CASE THE LIVE RUN IS FOR. Not one netvar record in the whole capture -- which is exactly
// what a spectate of a scoreless stretch produces -- and every HUD key still has a value at T0.
TEST_CASE("SnapshotSeedsEveryKeyAtT0") {
    using namespace nvsnap;
    TempDemo demo(L"t0");
    FakeSnapshot src;
    WriteDemo(demo.path, &src, 0, kNoDelta);

    const NetVarPrimer p = PrimerOf(demo.path);
    CHECK(p.keyframes > 0);
    for (size_t i = 0; i < kHudVarCount; ++i) {
        INFO("key ", kHudVars[i].name);
        CHECK(PrimerHas(p, kHudVars[i].symbol, EngineValue(i), kSlot));
    }
}

// Without a source the file is exactly what it was before this existed -- the delta shadow alone --
// and the same zero-delta capture primes NOTHING, which is the defect stated as a test.
TEST_CASE("NoSourceRecordsOnlyDeltas") {
    using namespace nvsnap;
    {
        TempDemo demo(L"none");
        WriteDemo(demo.path, nullptr, 0, kNoDelta);
        const NetVarPrimer p = PrimerOf(demo.path);
        CHECK(p.keyframes > 0);
        CHECK(p.count == 0);
    }
    // And a delta-carrying capture round-trips its block unchanged: one key, the delta's value.
    TempDemo demo(L"nonedelta");
    WriteDemo(demo.path, nullptr, kHudVars[kBlueScore].symbol, 0);
    const NetVarPrimer p = PrimerOf(demo.path);
    CHECK(p.count == 1);
    CHECK(PrimerHas(p, kHudVars[kBlueScore].symbol, kDeltaValue, kSlot));
}

// A key that changes MID-capture opens on its PRE-change value: the primer reads T0's keyframe, and
// at T0 the engine still held the old number.
TEST_CASE("PreChangeValueComesFromT0") {
    using namespace nvsnap;
    TempDemo demo(L"prechange");
    FakeSnapshot src;
    WriteDemo(demo.path, &src, kHudVars[kBlueScore].symbol, 480);   // ~4 s in

    const NetVarPrimer p = PrimerOf(demo.path);
    for (size_t i = 0; i < kHudVarCount; ++i) {
        INFO("key ", kHudVars[i].name);
        CHECK(PrimerHas(p, kHudVars[i].symbol, EngineValue(i), kSlot));
    }
}

// RULE 2, and the store-unbound retry in the same capture. The first keyframe is blind, so the
// primer's first sighting of every key is the SECOND keyframe -- which is written after a delta has
// already been recorded for blue_score. That keyframe must carry the DELTA's value for it and the
// engine's for the other six. A snapshot that refreshed every key would put the engine's stale
// number in the file beside an S_NETVAR row that says otherwise.
TEST_CASE("DeltaBeatsALaterSnapshot") {
    using namespace nvsnap;
    TempDemo demo(L"deltawins");
    FakeSnapshot src;
    src.blindCalls = 1;
    WriteDemo(demo.path, &src, kHudVars[kBlueScore].symbol, 0);

    CHECK(src.calls > 1);   // the builder asked again: a one-shot snapshot would never fill
    const NetVarPrimer p = PrimerOf(demo.path);
    CHECK(PrimerHas(p, kHudVars[kBlueScore].symbol, kDeltaValue, kSlot));
    for (size_t i = 0; i < kHudVarCount; ++i) {
        if (i == kBlueScore) continue;
        INFO("key ", kHudVars[i].name);
        CHECK(PrimerHas(p, kHudVars[i].symbol, EngineValue(i), kSlot));
    }
}
