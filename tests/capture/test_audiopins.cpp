// KB_AUDIO_PINS, from the tick thread's publish to the byte the playback reader decodes.
//
// The pins are gathered on the tick thread and the keyframe is written by the chapter worker, so
// the seam between them is where this block can be lost. What is asserted here is the whole path
// this packet owns: publish -> the worker's copy -> the block in a real keyframe -> the SHIPPED
// decoder (audio::DecodePins, the same call playback/core/playback_module.cpp makes) giving back
// the pins that were published. A second copy of the pin layout here would certify nothing, so the
// codec is linked rather than reimplemented.

#include "doctest.h"

#include <windows.h>

#include <string.h>

#include <vector>

#include "audio/audiopins.h"
#include "capture/capture_facts.h"
#include "capture/capture_types.h"
#include "capture/chapterbuilder.h"
#include "capture/keyframe.h"
#include "capture/manifestbuild.h"
#include "capture/streamcodec.h"
#include "foundation/bitio/enginewire.h"
#include "foundation/demofile/demofile.h"
#include "generated/facts_generated.h"

using replay::audio::AudioPins;
using replay::audio::DecodePins;
using replay::audio::EncodePins;
using replay::audio::PinDecode;
using replay::audio::PinDecodeReport;
using namespace replay::capture;

namespace {

AudioPins MakePins() {
    AudioPins p{};
    p.stateCount = 2;
    p.states[0].group = 11;
    p.states[0].value = 22;
    p.states[1].group = 33;
    p.states[1].value = 44;
    p.switchCount = 1;
    p.switches[0].gameObj = 7;
    p.switches[0].group = 8;
    p.switches[0].value = 9;
    p.loopCount = 2;
    p.loops[0].gameObj = 100;
    p.loops[0].eventId = 200;
    p.loops[0].positionMs = 1500;
    p.loops[0].sustainedMs = 4000;
    p.loops[1].gameObj = 101;
    p.loops[1].eventId = 201;
    p.loops[1].positionMs = -1;
    p.loops[1].sustainedMs = 9000;
    p.seed = 0xC0FFEEu;
    p.seedValid = true;
    p.historyBytes = 5;
    memcpy(p.history, "hist", 5);
    p.totals.statesSeen = 2;
    p.totals.statesStored = 2;
    // Seen and stored deliberately disagree: a cap that truncated has to survive the round trip,
    // or a file that restored a subset nobody could name would read as complete.
    p.totals.switchesSeen = 900;
    p.totals.switchesStored = 1;
    p.totals.loopsSeen = 2;
    p.totals.loopsStored = 2;
    return p;
}

// Walks a finished keyframe for one block id. Returns its length, 0 when the keyframe does not
// carry it -- which is the state this whole packet exists to change.
uint32_t FindBlock(const uint8_t* blob, size_t bytes, uint16_t wanted, const uint8_t** out) {
    KeyframeReader r;
    REQUIRE(r.Open(blob, bytes));
    uint16_t id = 0;
    const uint8_t* data = nullptr;
    uint32_t len = 0;
    while (r.Next(id, data, len)) {
        if (id == wanted) {
            *out = data;
            return len;
        }
    }
    *out = nullptr;
    return 0;
}

}  // namespace

TEST_CASE("the pin block published by the tick thread is what the worker reads back") {
    ResetAudioPinsForTest();
    uint8_t encoded[replay::audio::MaxPinBytes()];
    const size_t n = EncodePins(MakePins(), encoded, sizeof encoded);
    REQUIRE(n > 0);

    PublishAudioPins(encoded, static_cast<uint32_t>(n));

    uint8_t got[replay::audio::MaxPinBytes()];
    const uint32_t back = AudioPinsNow(got, sizeof got);
    CHECK(back == n);
    CHECK(memcmp(got, encoded, n) == 0);
}

TEST_CASE("a keyframe carries AUDIO_PINS and the shipped decoder gets the pins back") {
    ResetAudioPinsForTest();
    const AudioPins src = MakePins();
    uint8_t encoded[replay::audio::MaxPinBytes()];
    const size_t n = EncodePins(src, encoded, sizeof encoded);
    REQUIRE(n > 0);
    PublishAudioPins(encoded, static_cast<uint32_t>(n));

    // The worker's half, in the order chapterbuilder does it: copy what was published, then write
    // it as one block among the others so the reader has to find it by id and not by position.
    uint8_t pins[replay::audio::MaxPinBytes()];
    const uint32_t pinBytes = AudioPinsNow(pins, sizeof pins);
    REQUIRE(pinBytes == n);

    static uint8_t blob[64 * 1024];
    KeyframeBlobWriter k(blob, sizeof blob, 1234567u);
    k.Block(kKeyBlockTick);
    k.Put(uint8_t(0));
    k.Block(kKeyBlockAudioPins);
    k.Raw(pins, pinBytes);
    k.Block(kKeyBlockWorldOrigin);
    k.Put(float(0));
    const size_t total = k.Finish();
    REQUIRE(total > 0);

    const uint8_t* data = nullptr;
    const uint32_t len = FindBlock(blob, total, kKeyBlockAudioPins, &data);
    REQUIRE(len == pinBytes);

    // The block's payload is EncodePins' output verbatim: the reader hands the WHOLE block to
    // DecodePins (playback/core/windowbuild.cpp copies it raw and playback_module.cpp decodes it),
    // so any framing of our own would make every pin block undecodable.
    AudioPins out{};
    PinDecodeReport report{};
    REQUIRE(DecodePins(data, len, &out, &report) == PinDecode::Ok);
    CHECK(report.unknownSections == 0);
    CHECK(report.droppedRecords == 0);

    CHECK(out.stateCount == src.stateCount);
    CHECK(out.states[1].group == src.states[1].group);
    CHECK(out.states[1].value == src.states[1].value);
    CHECK(out.switchCount == src.switchCount);
    CHECK(out.switches[0].value == src.switches[0].value);
    CHECK(out.loopCount == src.loopCount);
    CHECK(out.loops[0].positionMs == src.loops[0].positionMs);
    CHECK(out.loops[1].positionMs == -1);          // a phase the capture could not read stays -1
    CHECK(out.loops[1].sustainedMs == 9000u);      // the loop-vs-one-shot discriminator survives
    CHECK(out.seed == src.seed);
    CHECK(out.seedValid);
    CHECK(out.historyBytes == src.historyBytes);
    CHECK(memcmp(out.history, src.history, out.historyBytes) == 0);
    CHECK(out.totals.switchesSeen == 900u);        // the truncation is still visible in the file
    CHECK(out.totals.switchesStored == 1u);
}

TEST_CASE("a keyframe written before anything published carries no pin block at all") {
    ResetAudioPinsForTest();
    uint8_t pins[replay::audio::MaxPinBytes()];
    CHECK(AudioPinsNow(pins, sizeof pins) == 0);

    // An EMPTY pin block would be a claim -- "the file says nothing is sustained here" -- so a
    // capture with no audio state bound must write no block, not a zero-length one.
    static uint8_t blob[4096];
    KeyframeBlobWriter k(blob, sizeof blob, 1u);
    k.Block(kKeyBlockTick);
    k.Put(uint8_t(0));
    const size_t total = k.Finish();
    REQUIRE(total > 0);
    const uint8_t* data = nullptr;
    CHECK(FindBlock(blob, total, kKeyBlockAudioPins, &data) == 0);
    CHECK(data == nullptr);
}

TEST_CASE("a publish of nothing clears the pins rather than leaving the last ones standing") {
    ResetAudioPinsForTest();
    uint8_t encoded[replay::audio::MaxPinBytes()];
    const size_t n = EncodePins(MakePins(), encoded, sizeof encoded);
    PublishAudioPins(encoded, static_cast<uint32_t>(n));
    uint8_t got[replay::audio::MaxPinBytes()];
    REQUIRE(AudioPinsNow(got, sizeof got) == n);

    // The audio state went away. Keyframes after that must not keep pinning the last state the
    // session happened to be in -- a seek would restore sounds the file does not claim.
    PublishAudioPins(nullptr, 0);
    CHECK(AudioPinsNow(got, sizeof got) == 0);
}

TEST_CASE("a block too large to hold whole is refused, never published short") {
    ResetAudioPinsForTest();
    uint8_t big[replay::audio::MaxPinBytes() + 1] = {};
    PublishAudioPins(big, sizeof big);
    uint8_t got[replay::audio::MaxPinBytes()];
    CHECK(AudioPinsNow(got, sizeof got) == 0);
}

TEST_CASE("the block table no longer calls AUDIO_PINS unwritten") {
    CHECK(strcmp(KeyBlockName(kKeyBlockAudioPins), "AUDIO_PINS") == 0);
    // The three that really are still reserved keep saying so, or the word stops meaning anything.
    CHECK(strstr(KeyBlockName(kKeyBlockAnimPhase), "reserved") != nullptr);
}

// ---- the FILE-LEVEL arm ------------------------------------------------------------------------
//
// Everything above drives the two halves of the writer directly. None of it drives
// ChapterBuilder::WriteKeyframe into a file and then asks whether the keyframe carried block 16, so
// W2A_MUTATE_NO_PIN_BLOCK reddened nothing: a mutant no assertion can see is not a mutant. This case
// writes a real .echodemo through the real container and reads the blocks back off the bytes, which
// is also the only arm that proves the pins survive the record framing and the chapter codec.

namespace w2a_pinfile {

using namespace replay;

struct TempDemoFile {
    wchar_t path[512];
    explicit TempDemoFile(const wchar_t* tag) {
        wchar_t dir[400];
        const DWORD n = GetTempPathW(400, dir);
        if (!n) { dir[0] = L'.'; dir[1] = 0; }
        _snwprintf_s(path, 512, _TRUNCATE, L"%lsw2a_pins_%ls_%lu.echodemo", dir, tag,
                     (unsigned long)GetCurrentProcessId());
    }
    ~TempDemoFile() { DeleteFileW(path); }
};

void FeedTick(ChapterBuilder& b, uint64_t t, uint32_t f, uint32_t dt) {
    TickRec tr{};
    tr.sourceTimeUs = t;
    tr.simTimeUs = t;
    tr.frameIndex = f + 1;
    tr.frameDeltaUs = dt;
    tr.scaledDeltaUs = dt;
    uint8_t buf[sizeof(RecHeader) + sizeof(TickRec)];
    RecHeader h;
    h.kind = kRecTick;
    h.bytes = uint16_t(sizeof tr);
    h.producer = 0;
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, &tr, sizeof tr);
    b.Consume(buf, uint32_t(sizeof buf));
}

struct FileScan {
    uint32_t records = 0;      // keyframes the file holds
    uint32_t refused = 0;      // keyframes the reader could not open
    uint32_t pinBlocks = 0;    // of those, the ones carrying AUDIO_PINS
    uint32_t lastPinLen = 0;
    uint8_t lastPin[replay::audio::MaxPinBytes()] = {};
};

// Walked the way a reader with no access to this build's source would: DemoReader, the S_KEY
// stream's own record framing, then the SHIPPED KeyframeReader finding the block by id.
bool ScanFile(const wchar_t* path, FileScan& s) {
    DemoReader r;
    if (!r.Open(path)) return false;
    std::vector<uint8_t> body(64 * 1024);
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        DecodedChapter c{};
        if (!r.DecodeChapter(i, c)) continue;
        const StreamView* v = c.Find(kStreamKey);
        if (!v || v->bytes < kBlockPrefixBytes) continue;
        bitio::BitReader br(v->records + kBlockPrefixBytes, v->bytes - kBlockPrefixBytes);
        for (uint32_t k = 0; k < v->storedCount; ++k) {
            uint16_t n = 0;
            if (!DecodeRawRecord(br, body.data(), body.size(), n)) { ++s.refused; break; }
            ++s.records;
            KeyframeReader kr;
            if (!kr.Open(body.data(), n)) { ++s.refused; continue; }
            uint16_t id = 0;
            const uint8_t* data = nullptr;
            uint32_t len = 0;
            while (kr.Next(id, data, len)) {
                if (id != kKeyBlockAudioPins) continue;
                ++s.pinBlocks;
                if (len <= sizeof s.lastPin) {
                    s.lastPinLen = len;
                    memcpy(s.lastPin, data, len);
                }
            }
        }
    }
    return true;
}

}  // namespace w2a_pinfile

TEST_CASE("every keyframe in a written file carries AUDIO_PINS, decodable off the bytes") {
    using namespace w2a_pinfile;
    ResetAudioPinsForTest();

    const AudioPins src = MakePins();
    uint8_t encoded[replay::audio::MaxPinBytes()];
    const size_t n = EncodePins(src, encoded, sizeof encoded);
    REQUIRE(n > 0);
    PublishAudioPins(encoded, static_cast<uint32_t>(n));

    const uint64_t kT0 = 19166475ull;
    const uint32_t kDtUs = uint32_t(facts::CLOCK_FIXEDSTEP_30HZ_US);
    const uint32_t kKeyIntervalUs = 2000000;
    const uint32_t kTicks = 400;          // ~13 s, so several keyframe intervals elapse
    ChapterBuilder b;
    BuilderStats stats{};
    TempDemoFile tmp(L"file");
    {
        ManifestBuilder mb;
        DemoWriter writer;
        mb.Begin(kT0, 0, 0x40);
        const uint8_t md5[16] = {0xca, 0xa0, 0x5e, 0xe3};
        mb.SetBuild(md5, 0x676F6C646D617374ull, 0x0102030405060708ull);
        mb.SetLevel(6300205991959903307ull, 0xF00Dull, 1780177787ull);
        mb.SetKeyframeIntervalUs(kKeyIntervalUs);
        mb.SetUuid(1, 2);
        REQUIRE(mb.Refusal() == nullptr);
        REQUIRE(writer.Open(tmp.path, mb.View()));
        BuilderConfig cfg;
        cfg.keyframeIntervalUs = kKeyIntervalUs;
        for (uint16_t i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;
        for (uint16_t i = 0; i < kSectionCount; ++i) cfg.sectionEnabled[i] = true;
        REQUIRE(b.Open(&writer, mb.View(), cfg));
        b.EmitMatchBegin(kT0);
        for (uint32_t f = 0; f < kTicks; ++f) FeedTick(b, kT0 + uint64_t(f) * kDtUs, f, kDtUs);
        b.EmitMatchEnd(kT0 + uint64_t(kTicks) * kDtUs);
        stats = b.Stats();
        b.Close();
        writer.Close();
    }

    FileScan s;
    REQUIRE(ScanFile(tmp.path, s));
    REQUIRE(s.records > 1);
    CHECK(s.refused == 0);

    // EVERY keyframe, not "at least one". A block present on some of them is the tick/worker seam
    // failing intermittently, and a seek landing on a keyframe without pins restores nothing.
    CHECK(s.pinBlocks == s.records);
    CHECK(stats.audioPinBlocks == s.records);
    CHECK(stats.audioPinsAbsent == 0);

    // And the bytes off disk are still the codec's, not a plausible-looking remnant: the loop with
    // positionMs == -1 is the field a width mistake destroys first.
    REQUIRE(s.lastPinLen == n);
    AudioPins out{};
    PinDecodeReport report{};
    REQUIRE(DecodePins(s.lastPin, s.lastPinLen, &out, &report) == PinDecode::Ok);
    CHECK(report.unknownSections == 0);
    CHECK(out.stateCount == src.stateCount);
    CHECK(out.switchCount == src.switchCount);
    CHECK(out.loopCount == src.loopCount);
    CHECK(out.loops[1].positionMs == -1);
    CHECK(out.totals.switchesSeen == 900);
    CHECK(out.totals.switchesStored == 1);
    CHECK(out.seedValid);
    CHECK(out.seed == 0xC0FFEEu);
}
