// The netvar row's time: recorded on new files, spread on old ones.
//
// The defect these cases pin (DIAG-HUD-CLOCK-STAIRCASE): every S_NETVAR row in a chapter was given
// the CHAPTER'S BEGIN TIME, because the wire format carried no per-message time. The resolver
// resolves a tie to its LAST member, so the HUD showed the value at the chapter's END for the whole
// chapter -- frozen for 2-4 s AND leading the playhead by up to one chapter.
//
// Three things have to hold and each has its own case below:
//   1. A file written now carries the time, and a window built from it resolves the value AT the
//      playhead rather than at the chapter's end.
//   2. A file written BEFORE the fifth schema field still decodes. The manifest's own field count is
//      what decides -- never the bits, which cannot tell.
//   3. An old file's rows are spread evenly across the chapter: monotone, strictly inside it, and
//      giving the playhead something that moves. That is an INTERPOLATION and the cases say so.
//
// ★ The old-format arm is driven by a REAL old capture, not by a transcription of the old encoder.
// A hand-written "this is what the four-field form looked like" fixture proves only that the
// transcription and the decoder agree, and drifts silently the day either moves. The bytes on disk
// cannot drift.

#include "doctest.h"

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "capture/capture_facts.h"
#include "capture/capture_types.h"
#include "capture/chapterbuilder.h"
#include "capture/manifestbuild.h"
#include "capture/streamcodec.h"
#include "foundation/bitio/bitio.h"
#include "foundation/demofile/demofile.h"
#include "playback/core/window.h"
#include "playback/core/windowbuild.h"

using namespace replay;
using namespace replay::playback;

namespace nvstamp {

// ---------------------------------------------------------------------------------------------
// A demo written through the real pipeline, with a dense netvar ramp on one key.
// ---------------------------------------------------------------------------------------------

struct TempDemo {
    wchar_t path[512];
    explicit TempDemo(const wchar_t* tag) {
        wchar_t dir[400];
        const DWORD n = GetTempPathW(400, dir);
        if (!n) { dir[0] = L'.'; dir[1] = 0; }
        _snwprintf_s(path, 512, _TRUNCATE, L"%lsnvstamp_%ls_%lu.echodemo", dir, tag,
                     (unsigned long)GetCurrentProcessId());
    }
    ~TempDemo() { DeleteFileW(path); }
};

// The DUV wire body the netvar tap delivers: one row, header exactly as duv.h describes it.
std::vector<uint8_t> DuvBody(uint32_t seq, uint64_t name, uint64_t value) {
    std::vector<uint8_t> b(46, 0);
    const uint64_t owner = 0x1111ull;
    memcpy(b.data(), &owner, 8);
    const uint16_t slot = 1;
    memcpy(b.data() + 8, &slot, 2);
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
    h.producer = 0;
    NetvarRec nr{};
    // ★ Deliberately NOT the axis. NetvarRec::arrivalUs is the engine delegate's argument 5
    // (capture.cpp OnNetvarMessage) and was never a clock -- W1-L found it reads -1 for a locally
    // raised message. A build that stamps the message from this field puts every row at 1 us and the
    // ramp case below fails on the first sample.
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
    h.producer = 0;
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, &tr, sizeof tr);
    b.Consume(buf, static_cast<uint32_t>(sizeof buf));
}

// 5 s at 120 Hz over a 2 s keyframe interval, so the file has several CORE chapters and the netvar
// key is dense inside each one.
const uint64_t kT0 = 5000000;
const uint64_t kDt = 8333;
const uint32_t kTicks = 600;

// The one netvar key counts UP once per tick, so the value at time t is a known function of t: a
// resolver that answers the end of the chapter answers a number this case can name.
void WriteRampDemo(const wchar_t* path) {
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
    REQUIRE(b.Open(&writer, mb.View(), cfg));
    b.EmitMatchBegin(kT0);

    for (uint32_t f = 0; f < kTicks; ++f) {
        FeedTick(b, kT0 + f * kDt, f + 1, kDt);
        FeedNetvar(b, 1000 + f, capture_facts::kNvGameTimer, uint64_t(f) + 1);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    }
    b.Close();
    writer.Close();
}

// ---------------------------------------------------------------------------------------------
// The real four-field capture. `.gitignore` keeps `*.echodemo` out of history, so it is looked for
// where it is, and an absence is REPORTED rather than passed over: a fixture that cannot fail is
// worse than one that is missing, and a missing one nobody is told about is worse than both.
// ---------------------------------------------------------------------------------------------
#define NVSTAMP_WIDEN_(x) L##x
#define NVSTAMP_WIDEN(x) NVSTAMP_WIDEN_(x)
// The real multi-player capture the diagnosis was taken from is first: it is the one with a dense
// `game_timer`, and it is the file the owner actually watches. The bare-level session beside this
// source is the fallback -- it opens on any machine, and a file with no netvar records in it says so
// rather than passing.
const wchar_t* const kOldCaptures[] = {
    L"2026-09-18_03-33-45_576ED3F8428EBC4B_00000000FFFFFFFF.echodemo",
    L"2026-09-04_08-01-50_0000000000000000_0000000000000000.echodemo",
};

bool HasNetvarRecords(DemoReader& reader) {
    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        if (reader.ChapterAt(i).section != kSectionCore) continue;
        DecodedChapter dc;
        if (!reader.DecodeChapter(i, dc)) continue;
        const StreamView* sv = dc.Find(static_cast<StreamId>(kStreamNetvar));
        if (sv && sv->recordCount > 0) return true;
    }
    return false;
}

// Opens the first four-field capture that actually carries netvar rows. `.gitignore` keeps
// `*.echodemo` out of history, so both places a file can be are tried and an absence is REPORTED:
// a fixture that cannot fail is worse than one that is missing, and a missing one nobody is told
// about is worse than both.
bool OpenOldCapture(DemoReader& reader, char* whereOut, size_t whereChars) {
    const wchar_t* home = _wgetenv(L"USERPROFILE");
    for (size_t i = 0; i < sizeof kOldCaptures / sizeof kOldCaptures[0]; ++i) {
        wchar_t path[1024];
        _snwprintf_s(path, _TRUNCATE, NVSTAMP_WIDEN(REPLAY_PLAYBACK_FIXTURE_DIR) L"/%s",
                     kOldCaptures[i]);
        bool opened = reader.Open(path);
        if (!opened && home != nullptr) {
            _snwprintf_s(path, _TRUNCATE, L"%s/Documents/EchoReplay/%s", home, kOldCaptures[i]);
            opened = reader.Open(path);
        }
        if (opened && !NetvarStampsPresent(reader.GetManifest()) && HasNetvarRecords(reader)) {
            _snprintf_s(whereOut, whereChars, _TRUNCATE, "%ls", path);
            return true;
        }
        reader.Close();
    }
    _snprintf_s(whereOut, whereChars, _TRUNCATE,
                "no four-field capture carrying netvar rows, in " REPLAY_PLAYBACK_FIXTURE_DIR
                " or %%USERPROFILE%%/Documents/EchoReplay");
    return false;
}

// The CORE chapter with the most netvar rows in it, which is where a spread has something to say.
size_t DensestCoreChapter(DemoReader& reader, uint32_t& rowsOut) {
    size_t best = 0;
    uint32_t bestRows = 0;
    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        if (reader.ChapterAt(i).section != kSectionCore) continue;
        DecodedChapter dc;
        if (!reader.DecodeChapter(i, dc)) continue;
        const StreamView* sv = dc.Find(static_cast<StreamId>(kStreamNetvar));
        const uint32_t n = sv ? sv->recordCount : 0;
        if (n > bestRows) { bestRows = n; best = i; }
    }
    rowsOut = bestRows;
    return best;
}

// Whether any CORE chapter's keyframe or netvar stream carries this key at all. Asked before the
// clock sweep so a capture without a match clock says so instead of failing on an empty resolver.
bool HasKey(DemoReader& reader, uint64_t key) {
    Window w;
    BuildStats bs;
    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        if (reader.ChapterAt(i).section != kSectionCore) continue;
        if (!BuildWindowRange(reader, i, w, bs)) continue;
        for (size_t k = 0; k < w.NetVars().KeyCount(); ++k) {
            if (w.NetVars().KeyAt(k) == key) return true;
        }
    }
    return false;
}

float AsFloat(uint64_t raw) {
    // What the engine's typed setter stores for a Real: the low dword (netvarhud.cpp AsFloat).
    const uint32_t bits = uint32_t(raw);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

}  // namespace nvstamp

// =============================================================================================

TEST_CASE("A netvar message carries its own time through encode and decode") {
    using namespace replay::capture;

    // A monotone series with a repeat in it: the tap runs a little faster than the tick, so two
    // messages sharing one axis time is the normal case, not an edge one.
    const uint64_t stamps[4] = {877920337ull, 877928670ull, 877928670ull, 881929957ull};
    NetvarMessage in[4] = {};
    for (int i = 0; i < 4; ++i) {
        in[i].owner = 0x1111ull;
        in[i].slot = 1;
        in[i].gen = 1;
        in[i].pktSeq = 500 + i;
        in[i].stampUs = stamps[i];
        in[i].hasRowset = true;
        in[i].rowsetSeq = in[i].pktSeq;
        in[i].varCount = 1;
        in[i].rows[0].name = capture_facts::kNvGameTimer;
        in[i].rows[0].slot = 1;
        in[i].rows[0].value = 0x1CF00000000ull + uint64_t(i);
    }

    uint8_t buf[4096] = {};
    bitio::BitWriter w(buf, sizeof buf);
    NetvarEncoder enc;
    enc.Reset();
    for (int i = 0; i < 4; ++i) {
        REQUIRE(enc.Encode(w, in[i]));
    }
    const size_t bits = w.BitsWritten();

    bitio::BitReader r(buf, sizeof buf);
    NetvarDecoder dec(true);
    dec.Reset();
    for (int i = 0; i < 4; ++i) {
        NetvarMessage out{};
        REQUIRE(dec.Decode(r, out));
        CHECK(out.stampUs == in[i].stampUs);
        CHECK(out.pktSeq == in[i].pktSeq);
        CHECK(out.varCount == 1);
        CHECK(out.rows[0].name == in[i].rows[0].name);
        CHECK(out.rows[0].value == in[i].rows[0].value);
    }
    CHECK(r.BitsRead() == bits);   // read back by exactly the bits it was written with
}

TEST_CASE("The stamp costs a delta, not a field") {
    using namespace replay::capture;

    // What a chapter of the real capture looks like: 722 messages over 4 s, the axis advancing one
    // 120 Hz tick at a time and the tap running a little ahead of it.
    const uint32_t kMsgs = 722;
    const uint64_t kTick = 8333;

    auto encodeWith = [&](bool moving) -> size_t {
        std::vector<uint8_t> buf(1 << 20, 0);
        bitio::BitWriter w(buf.data(), buf.size());
        NetvarEncoder enc;
        enc.Reset();
        for (uint32_t i = 0; i < kMsgs; ++i) {
            NetvarMessage m{};
            m.owner = 0x1111ull;
            m.slot = 1;
            m.gen = 1;
            m.pktSeq = 500 + i;
            m.stampUs = moving ? 877920337ull + uint64_t(i * 2 / 3) * kTick : 877920337ull;
            m.hasRowset = true;
            m.rowsetSeq = m.pktSeq;
            m.varCount = 1;
            m.rows[0].name = capture_facts::kNvGameTimer;
            m.rows[0].slot = 1;
            m.rows[0].value = 0x1CF00000000ull + uint64_t(i) * 37;
            REQUIRE(enc.Encode(w, m));
        }
        return w.BitsWritten();
    };

    const size_t moving = encodeWith(true);
    const size_t still = encodeWith(false);

    // A still stamp is the format's floor for the field: the first message writes it absolute (64
    // bits) and every later one is a 2-bit class selector plus the smallest 8-bit payload. So the
    // whole field costs `still_floor` bits, and a moving series costs that plus the difference.
    const size_t stillFloor = 64 + size_t(kMsgs - 1) * (2 + 8);
    const size_t stampBits = stillFloor + (moving - still);
    const double bytesPerRecord = double(stampBits) / 8.0 / double(kMsgs);

    MESSAGE("stamp costs " << stampBits << " bits over " << kMsgs << " messages = "
            << bytesPerRecord << " bytes/record; block " << (moving / 8) << " bytes");

    // The claim this packet makes about capture cost: a couple of bits short of two bytes a record,
    // not another eight-byte field. Bounded on both sides so a regression in either direction shows.
    CHECK(bytesPerRecord > 1.0);
    CHECK(bytesPerRecord < 2.5);
}

TEST_CASE("The manifest's field count decides, not the bits") {
    SchemaDef schemas[2] = {};
    schemas[0].id = capture::kSchemaTick;
    schemas[0].fieldCount = 7;
    schemas[1].id = capture::kSchemaNetvar;

    Manifest m{};
    m.schemas = schemas;
    m.schemaCount = 2;

    schemas[1].fieldCount = 4;
    CHECK_FALSE(NetvarStampsPresent(m));
    schemas[1].fieldCount = 5;
    CHECK(NetvarStampsPresent(m));

    // A file that declares no netvar schema at all declares no stamp; neither does one whose schema
    // table did not survive the parse.
    m.schemaCount = 1;
    CHECK_FALSE(NetvarStampsPresent(m));
    m.schemaCount = 2;
    m.schemas = nullptr;
    CHECK_FALSE(NetvarStampsPresent(m));
}

TEST_CASE("Reading a stamped block as four fields does not quietly work") {
    using namespace replay::capture;

    // The mirror of the compatibility gate. If a stream written one way could be read the other way
    // and still produce the right rows, the gate would be decorative and the manifest check would be
    // proving nothing.
    uint8_t buf[8192] = {};  // facts-ok: PHYS.CI_PAUSED_BIT a local buffer size, not the engine offset
    bitio::BitWriter w(buf, sizeof buf);
    NetvarEncoder enc;
    enc.Reset();
    const int kN = 8;
    uint64_t values[kN] = {};
    for (int i = 0; i < kN; ++i) {
        NetvarMessage m{};
        m.owner = 0x1111ull;
        m.slot = 1;
        m.gen = 1;
        m.pktSeq = 900 + i;
        m.stampUs = 877920337ull + uint64_t(i) * 8333ull;
        m.hasRowset = true;
        m.rowsetSeq = m.pktSeq;
        m.varCount = 1;
        m.rows[0].name = capture_facts::kNvGameTimer;
        m.rows[0].slot = 1;
        m.rows[0].value = 0x1CF00000000ull + uint64_t(i) * 101;
        values[i] = m.rows[0].value;
        REQUIRE(enc.Encode(w, m));
    }
    const size_t bits = w.BitsWritten();

    bitio::BitReader r(buf, sizeof buf);
    NetvarDecoder dec(false);   // the old four-field reader, against a stamped block
    dec.Reset();
    bool sameRows = true;
    for (int i = 0; i < kN; ++i) {
        NetvarMessage out{};
        if (!dec.Decode(r, out) || out.pktSeq != uint32_t(900 + i) || out.varCount != 1 ||
            out.rows[0].value != values[i]) {
            sameRows = false;
            break;
        }
    }
    const bool sameBits = !r.Overrun() && r.BitsRead() == bits;
    const bool indistinguishable = sameRows && sameBits;
    CHECK_FALSE(indistinguishable);
}

TEST_CASE("The spread is monotone per key and strictly inside the chapter") {
    // Checked on the ROWS, not through the resolver: KeyedResolver::Build clamps each key's series
    // non-decreasing, so a spread that went backwards would be straightened on the way in and a test
    // that looked at ValueAt would agree with a broken spread.
    const uint64_t tBegin = 873907732ull;
    const uint64_t tEnd = 877920337ull;

    // Three keys interleaved at different rates, the way a real chapter arrives: a 120 Hz clock, a
    // score that moves twice, and one key seen exactly once.
    std::vector<KeyedRecord> rows;
    for (int i = 0; i < 480; ++i) {
        KeyedRecord r{};
        r.t_us = tBegin;
        r.key = 0xAAAA;
        r.value = uint64_t(i);
        rows.push_back(r);
        if (i == 100 || i == 300) {  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF plain 300 as a count, size or pixel value, not the extrapolation-cap offset
            KeyedRecord s{};
            s.t_us = tBegin;
            s.key = 0xBBBB;
            s.value = uint64_t(i);
            rows.push_back(s);
        }
        if (i == 7) {
            KeyedRecord o{};
            o.t_us = tBegin;
            o.key = 0xCCCC;
            o.value = 1;
            rows.push_back(o);
        }
    }
    const size_t n = rows.size();
    SpreadRowsOverChapter(rows.data(), n, tBegin, tEnd);

    struct Seen { uint64_t key; uint64_t last; size_t count; };
    std::vector<Seen> seen;
    for (size_t i = 0; i < n; ++i) {
        // Strictly inside: a row ON tBegin would tie with this chapter's keyframe snapshot and the
        // tie resolves to the later one; a row ON tEnd would tie with the NEXT chapter's.
        CHECK(rows[i].t_us > tBegin);
        CHECK(rows[i].t_us < tEnd);
        size_t at = seen.size();
        for (size_t k = 0; k < seen.size(); ++k) {
            if (seen[k].key == rows[i].key) { at = k; break; }
        }
        if (at == seen.size()) {
            Seen s{rows[i].key, rows[i].t_us, 1};
            seen.push_back(s);
        } else {
            CHECK(rows[i].t_us > seen[at].last);   // strictly increasing within one key
            seen[at].last = rows[i].t_us;
            ++seen[at].count;
        }
    }
    REQUIRE(seen.size() == 3);
    for (size_t k = 0; k < seen.size(); ++k) {
        if (seen[k].key == 0xAAAA) CHECK(seen[k].count == 480);
        if (seen[k].key == 0xBBBB) CHECK(seen[k].count == 2);
        if (seen[k].key == 0xCCCC) CHECK(seen[k].count == 1);
    }

    // A single row of a key sits at the chapter's midpoint, which is the only defensible place for
    // "it arrived somewhere in here" -- and not at either edge, where it would tie with a keyframe.
    for (size_t i = 0; i < n; ++i) {
        if (rows[i].key == 0xCCCC) {
            CHECK(rows[i].t_us == tBegin + (tEnd - tBegin) / 2);
        }
    }

    // A degenerate chapter is left alone rather than divided by its own zero width.
    std::vector<KeyedRecord> flat(4);
    for (size_t i = 0; i < flat.size(); ++i) {
        flat[i].t_us = tBegin;
        flat[i].key = 0xDDDD;
        flat[i].value = uint64_t(i);
    }
    SpreadRowsOverChapter(flat.data(), flat.size(), tBegin, tBegin);
    for (size_t i = 0; i < flat.size(); ++i) {
        CHECK(flat[i].t_us == tBegin);
    }
    SpreadRowsOverChapter(nullptr, 0, tBegin, tEnd);   // no rows, no read
}

TEST_CASE("A stamped file resolves the value AT the playhead, not at the chapter end") {
    using namespace nvstamp;
    TempDemo tmp(L"ramp");
    WriteRampDemo(tmp.path);

    DemoReader reader;
    REQUIRE(reader.Open(tmp.path));
    REQUIRE(NetvarStampsPresent(reader.GetManifest()));

    Window w;
    BuildStats bs;
    const size_t centre = ChapterForTime(reader, int64_t(kT0 + (kTicks / 2) * kDt));
    REQUIRE(BuildWindowRange(reader, centre, w, bs));
    CHECK(bs.refusal == nullptr);
    CHECK(bs.netvar_stamps_present);
    CHECK(bs.netvar_rows_spread == 0);          // nothing is invented on a stamped file
    CHECK(bs.netvar_rows_over_budget == 0);
    CHECK(bs.blocks_verified > 0);
    CHECK(bs.blocks_bit_mismatch == 0);

    const ChapterKey key = reader.ChapterAt(centre);
    REQUIRE(key.tEndUs > key.tBeginUs);

    // The value written at tick f is f+1 and that tick is at kT0 + f*kDt, so the resolver's answer
    // at any t names the tick it came from. Sampled across the CENTRE chapter, which is exactly
    // where the pre-fix build answered one single number for two whole seconds.
    size_t distinct = 0;
    uint64_t prev = 0;
    bool first = true;
    int64_t worstErrUs = 0;
    const int kSamples = 40;
    for (int step = 1; step < kSamples; ++step) {
        const int64_t t = int64_t(key.tBeginUs) +
                          int64_t((key.tEndUs - key.tBeginUs) * uint64_t(step) / kSamples);
        uint64_t v = 0;
        REQUIRE(w.NetVars().ValueAt(capture_facts::kNvGameTimer, t, v));
        if (first || v != prev) ++distinct;
        first = false;
        prev = v;
        const int64_t vTime = int64_t(kT0) + int64_t(v - 1) * int64_t(kDt);
        const int64_t err = t - vTime;
        CHECK(err >= 0);                   // never a value from the future -- the old bug's signature
        CHECK(err <= int64_t(kDt) * 2);    // and never more than a tick or two stale
        if (err > worstErrUs) worstErrUs = err;
    }
    // 2 s of a 120 Hz key is ~240 rows; 39 samples of it must land on 39 different values. The
    // pre-fix build answered exactly one.
    CHECK(distinct >= 35);
    MESSAGE("distinct values across the chapter = " << distinct << " of " << (kSamples - 1)
            << " samples, worst staleness = " << worstErrUs << " us");

    // REVERSE. Walked backwards, the incremental cursor must answer what the definition answers --
    // a stamp that only worked forwards shows up here and nowhere else.
    for (int step = kSamples - 1; step >= 1; --step) {
        const int64_t t = int64_t(key.tBeginUs) +
                          int64_t((key.tEndUs - key.tBeginUs) * uint64_t(step) / kSamples);
        uint64_t cursorV = 0, searchV = 0;
        REQUIRE(w.NetVars().ValueAt(capture_facts::kNvGameTimer, t, cursorV));
        REQUIRE(w.NetVars().ValueAtBySearch(capture_facts::kNvGameTimer, t, searchV));
        CHECK(cursorV == searchV);
    }

    // SEEK. A window centred elsewhere and re-centred here answers the same thing: the row's time is
    // a property of the file, not of the order windows were built in.
    Window w2;
    BuildStats bs2;
    REQUIRE(BuildWindowRange(reader, ChapterForTime(reader, int64_t(kT0 + kDt)), w2, bs2));
    REQUIRE(BuildWindowRange(reader, centre, w2, bs2));
    const int64_t mid = int64_t(key.tBeginUs) + int64_t((key.tEndUs - key.tBeginUs) / 2);
    uint64_t a = 0, b = 0;
    REQUIRE(w.NetVars().ValueAt(capture_facts::kNvGameTimer, mid, a));
    REQUIRE(w2.NetVars().ValueAt(capture_facts::kNvGameTimer, mid, b));
    CHECK(a == b);
}

TEST_CASE("A real four-field capture still opens, and its rows are spread") {
    using namespace nvstamp;

    DemoReader reader;
    char where[512] = {};
    if (!OpenOldCapture(reader, where, sizeof where)) {
        MESSAGE("NOT RUN: the four-field capture is absent -- looked in " << where);
        return;
    }
    MESSAGE("four-field capture: " << where);

    // The compatibility statement itself: this file predates the fifth field and says so.
    REQUIRE_FALSE(NetvarStampsPresent(reader.GetManifest()));

    uint32_t msgs = 0;
    const size_t centre = DensestCoreChapter(reader, msgs);
    REQUIRE(msgs > 0);   // OpenOldCapture only returns a file that has them

    Window w;
    BuildStats bs;
    REQUIRE(BuildWindowRange(reader, centre, w, bs));
    REQUIRE(bs.refusal == nullptr);          // an old file DECODES; it is not refused
    CHECK_FALSE(bs.netvar_stamps_present);
    CHECK(bs.netvar_rows_spread > 0);        // and its rows carry an interpolated time
    CHECK(bs.netvar_rows_over_budget == 0);

    // ★ THE BYTE-EXACT STATEMENT, and the only form of it that is not vacuous. Every stream block
    // declares its exact bit length, and the build refuses unless the decode consumed precisely that
    // many. A four-field block read with the stamp branch taken would consume more; one read with it
    // skipped consumes exactly what the old decoder consumed, because that branch is the only thing
    // this packet added to the read path. So a green verify over blocks this size IS the proof that
    // the old file still decodes as it did.
    CHECK(bs.blocks_verified > 0);
    CHECK(bs.blocks_bit_mismatch == 0);

    const ChapterKey key = reader.ChapterAt(centre);
    REQUIRE(key.tEndUs > key.tBeginUs);

    // The spread has to give the playhead something that MOVES, and it has to stay inside the
    // chapter: a row at or past tEnd would cross the next chapter's keyframe snapshot, and a row
    // before tBegin would back-date into the chapter behind it.
    //
    // Sampled on whichever key the file actually carries densely, found rather than assumed: this
    // capture is a bare-level session and need not contain a match clock at all.
    uint64_t denseKey = 0;
    size_t denseDistinct = 0;
    const int kSamples = 24;
    for (size_t k = 0; k < w.NetVars().KeyCount(); ++k) {
        const uint64_t name = w.NetVars().KeyAt(k);
        size_t distinct = 0;
        uint64_t prev = 0;
        bool first = true, ok = true;
        for (int step = 1; step < kSamples; ++step) {
            const int64_t t = int64_t(key.tBeginUs) +
                              int64_t((key.tEndUs - key.tBeginUs) * uint64_t(step) / kSamples);
            uint64_t v = 0;
            if (!w.NetVars().ValueAt(name, t, v)) { ok = false; break; }
            if (first || v != prev) ++distinct;
            first = false;
            prev = v;
        }
        if (ok && distinct > denseDistinct) { denseDistinct = distinct; denseKey = name; }
    }
    REQUIRE(denseKey != 0);
    MESSAGE("densest key " << denseKey << " gave " << denseDistinct
            << " distinct values across the chapter, from " << msgs << " messages");

    // A key the file writes many times in one chapter must resolve to many values across it. One
    // value is the staircase; this is the number that says it is gone.
    CHECK(denseDistinct >= 4);

    // Monotone and bounded, checked through the resolver's own definition: the value at tBegin is
    // the earliest, the value at tEnd-1 is the latest, and the sequence between never goes back.
    uint64_t last = 0;
    bool started = false;
    for (int step = 0; step <= kSamples; ++step) {
        const int64_t t = int64_t(key.tBeginUs) +
                          int64_t((key.tEndUs - key.tBeginUs - 1) * uint64_t(step) / kSamples);
        uint64_t cursorV = 0, searchV = 0;
        const bool a = w.NetVars().ValueAt(denseKey, t, cursorV);
        const bool b = w.NetVars().ValueAtBySearch(denseKey, t, searchV);
        CHECK(a == b);
        if (a) {
            CHECK(cursorV == searchV);
            last = cursorV;
            started = true;
        }
    }
    CHECK(started);
    (void)last;
}

TEST_CASE("The match clock of the real capture ticks at 10 Hz and tracks the playhead") {
    using namespace nvstamp;

    DemoReader reader;
    char where[512] = {};
    if (!OpenOldCapture(reader, where, sizeof where)) {
        MESSAGE("NOT RUN: " << where);
        return;
    }
    if (!HasKey(reader, capture_facts::kNvGameTimer)) {
        MESSAGE("NOT RUN: " << where << " carries no game_timer rows");
        return;
    }

    // ★ THE FIELD MEASUREMENT, OFFLINE. DIAG-HUD-CLOCK-STAIRCASE measured the shipped build reading
    // 107.68 / 103.66 / 99.65 across 8 s of playback -- three values, each held for one chapter,
    // each ahead of the playhead. This walks the same file's last 30 s at the 10 Hz the HUD sweep
    // runs at, rebuilding the window the way the transport does, and asks two things of the answers:
    // they must MOVE, and they must move at the rate a match clock counts down.
    uint64_t tFirst = 0, tLast = 0;
    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        const ChapterKey k = reader.ChapterAt(i);
        if (k.section != kSectionCore) continue;
        if (tFirst == 0 || k.tBeginUs < tFirst) tFirst = k.tBeginUs;
        if (k.tEndUs > tLast) tLast = k.tEndUs;
    }
    REQUIRE(tLast > tFirst);

    const uint64_t kSpanUs = 30000000ull;
    const uint64_t kStepUs = 100000ull;   // 10 Hz, the cadence netvarhud's sweep measures at
    const int64_t begin = int64_t(tLast > tFirst + kSpanUs ? tLast - kSpanUs : tFirst);
    const int64_t end = int64_t(tLast);

    Window w;
    BuildStats bs;
    size_t centre = size_t(-1);
    int samples = 0, changed = 0;
    float firstV = 0.0f, lastV = 0.0f;
    int64_t firstT = 0, lastT = 0;
    bool have = false;
    float maxStep = 0.0f;
    int64_t maxStepAt = 0;
    char table[8192];  // facts-ok: PHYS.CI_PAUSED_BIT a local buffer size, not the engine offset
    size_t used = 0;
    table[0] = 0;

    for (int64_t t = begin; t < end; t += int64_t(kStepUs)) {
        const size_t c = ChapterForTime(reader, t);
        if (c != centre) {
            if (!BuildWindowRange(reader, c, w, bs)) {
                MESSAGE("window build refused at t=" << t << ": "
                        << (bs.refusal ? bs.refusal : "unstated"));
                break;
            }
            centre = c;
        }
        uint64_t raw = 0;
        if (!w.NetVars().ValueAt(capture_facts::kNvGameTimer, t, raw)) {
            continue;
        }
        const float v = AsFloat(raw);
        ++samples;
        if (have) {
            if (v != lastV) ++changed;
            const float step = v > lastV ? v - lastV : lastV - v;
            if (step > maxStep) { maxStep = step; maxStepAt = t; }
        }
        if (!have) { firstV = v; firstT = t; have = true; }
        lastV = v;
        lastT = t;
        if (samples % 10 == 1 && used + 48 < sizeof table) {
            used += size_t(_snprintf_s(table + used, sizeof table - used, _TRUNCATE,
                                       "  t=%.3f s  game_timer=%.3f\n",
                                       double(t) / 1e6, double(v)));
        }
    }

    REQUIRE(samples > 200);          // 30 s at 10 Hz, less whatever the file's tail does not cover
    MESSAGE("game_timer at 10 Hz over the last 30 s of " << where << ":\n" << table);

    // ★ THE STAIRCASE, AS A NUMBER. Every sample but the first must differ from the one before it.
    // Before this packet the same sweep changed only when the playhead crossed a chapter boundary:
    // 10 distinct values in 142 frames, one every 2-4 s.
    const double changeRate = double(changed) / double(samples - 1);
    MESSAGE("samples=" << samples << " changed=" << changed << " (" << (changeRate * 100.0) << "%)");
    CHECK(changeRate > 0.9);

    // ★ AND THE RATE. A match clock counts DOWN one second per second of playhead, so the slope of
    // the resolved value against the playhead is -1. A value stamped at its chapter's beginning
    // still has slope -1 on average -- it is the staircase, not a tilt -- so this is the check that
    // says the samples are on the right line, and the one above is the check that says they are not
    // steps. Neither alone would catch both faults.
    const double slope = double(lastV - firstV) / (double(lastT - firstT) / 1e6);
    MESSAGE("slope = " << slope << " s of clock per s of playhead");
    CHECK(slope < -0.8);
    CHECK(slope > -1.2);

    // ★★★ AND THE LOCAL STEP, WHICH NEITHER OF THE TWO ABOVE CAN SEE. `changed` counts WHETHER a
    // sample moved and the slope is fitted over the whole 30 s, so a 2 s jump inside the sweep
    // passes both -- which is exactly what happened: DIAG-CLOCK-BIND-2 opened against a 2 s clock
    // jump that this file's own data does not contain, and no assertion here could say so. A 10 Hz
    // sweep of a 1 s/s clock moves 0.1 s per sample; 0.3 s is three times that and still an order
    // below the jump it exists to catch. Today's run measures 0.0195 s.
    MESSAGE("largest consecutive change = " << maxStep << " s at t=" << maxStepAt);
    CHECK(maxStep < 0.3f);
}

// =================================================================================================
// PLAYBACK-FIX-4 F1. The keyframe netvar KEY BLOCK, through the real writer and both readers.
//
// The block is `{u16 count}{u16 slot, u64 name, u64 value} x count` (chapterbuilder.cpp's keyframe
// writer). Every reader of it walked from `data` instead of `data + 2` for the life of the project,
// so every seeded name was the low two bytes of the count followed by six bytes of the real name --
// a name no symbol can equal. The defect was invisible because a seed nobody can look up is inert:
// the resolver answered from the S_NETVAR rows instead, and only a key with NO row in the resident
// chapters (the match clock at T0) ever showed it. Which is exactly why the cases below are built on
// windows that carry no row for the key: the seed and the primer are the only answer there, and a
// reader two bytes adrift has nothing to hide behind.
// =================================================================================================

namespace nvstamp {

// 18 s of ticks with the netvar key alive for half a second in the middle. That shape is what makes
// the block observable: the chapters before the key's first row carry neither a seed nor a row for
// it (the primer's job), and the chapters after its last row carry the keyframe SNAPSHOT of it and
// no rows at all (the seed's job).
const uint64_t kLateT0 = 5000000;
const uint32_t kLateTicks = 2160;          // 18 s at 8333 us
const uint32_t kLateFirstRow = 480;        // t0 + 4.000 s, inside the third chapter
const uint32_t kLateRowCount = 60;         // half a second of rows
const uint64_t kLateLastValue = kLateFirstRow + kLateRowCount;   // the ramp's last value
// The ramp value the FIRST keyframe to carry the key snapshots. The chapter roll is not instant --
// the keyframe after the ramp starts is written a few ticks into it -- so this is measured from the
// file rather than assumed to be the ramp's first value, and it moves only if the builder's roll
// timing does, which is worth being told about.
const uint64_t kLateFirstSnapshotValue = 483;

void WriteLateKeyDemo(const wchar_t* path) {
    using namespace replay::capture;

    ManifestBuilder mb;
    mb.Begin(kLateT0, 0, 0x40);
    const uint8_t md5[16] = {0xca, 0xa0, 0x5e, 0xe3};
    mb.SetBuild(md5, 0x676F6C646D617374ull, 0x0102030405060708ull);
    mb.SetLevel(6300205991959903307ull, 0xF00Dull, 1780177787ull);
    mb.SetKeyframeIntervalUs(2000000);
    mb.SetUuid(3, 4);
    REQUIRE(mb.Refusal() == nullptr);

    DemoWriter writer;
    REQUIRE(writer.Open(path, mb.View()));

    BuilderConfig cfg;
    cfg.keyframeIntervalUs = 2000000;
    for (uint16_t i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;
    for (uint16_t i = 0; i < kSectionCount; ++i) cfg.sectionEnabled[i] = true;

    ChapterBuilder b;
    REQUIRE(b.Open(&writer, mb.View(), cfg));
    b.EmitMatchBegin(kLateT0);
    for (uint32_t f = 0; f < kLateTicks; ++f) {
        FeedTick(b, kLateT0 + f * kDt, f + 1, kDt);
        if (f >= kLateFirstRow && f < kLateFirstRow + kLateRowCount) {
            FeedNetvar(b, 1000 + f, capture_facts::kNvGameTimer, uint64_t(f) + 1);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
        }
    }
    b.Close();
    writer.Close();
}

// The first CORE chapter whose keyframe begins at or after `t_us`.
size_t CoreChapterAtOrAfter(DemoReader& reader, uint64_t t_us) {
    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        if (reader.ChapterAt(i).section != kSectionCore) continue;
        if (reader.ChapterAt(i).tBeginUs >= t_us) return i;
    }
    return reader.ChapterCount();
}

}  // namespace nvstamp

TEST_CASE("PLAYBACK-FIX-4: the keyframe's netvar seeds carry the REAL symbol, through the writer") {
    using namespace nvstamp;
    TempDemo demo(L"latekey_primer");
    WriteLateKeyDemo(demo.path);

    DemoReader reader;
    REQUIRE(reader.Open(demo.path));

    NetVarPrimer p;
    ScanNetVarPrimer(reader, p);
    MESSAGE("primer: " << p.count << " key(s) from " << p.keyframes << " keyframe(s) over "
                       << p.chapters << " chapter(s)");
    REQUIRE(p.keyframes > 0);

    // ★★★ THE ASSERTION THE OLD READER CANNOT PASS. The name is the one this file wrote, so a walk
    // that starts two bytes early answers a name built from the count -- and IndexOf refuses it.
    const size_t at = p.IndexOf(capture_facts::kNvGameTimer);
    REQUIRE(at != NetVarPrimer::kMax);
    MESSAGE("primer value = " << p.value[at] << " slot = " << p.slot[at]);
    CHECK(p.value[at] == kLateFirstSnapshotValue);
    CHECK(p.slot[at] == 1);                 // DuvBody's slot; without it netvarhud refuses the key

    // And nothing else got in: a mis-walked block yields names that are not symbols, so a primer
    // holding only keys this file wrote is the other half of the same statement.
    for (size_t i = 0; i < p.count; ++i) {
        CHECK(p.key[i] == capture_facts::kNvGameTimer);
    }
}

TEST_CASE("PLAYBACK-FIX-4: a window with no row for the key answers from the keyframe SEED") {
    using namespace nvstamp;
    TempDemo demo(L"latekey_seed");
    WriteLateKeyDemo(demo.path);

    DemoReader reader;
    REQUIRE(reader.Open(demo.path));

    // Well past the key's last row: these chapters carry its keyframe snapshot and no rows at all.
    const size_t centre = CoreChapterAtOrAfter(reader, kLateT0 + 10000000);  // facts-ok: CLOCK.CTOR_MAXELAPSED_INIT 10,000,000 in tick arithmetic, not the clock constructor's initialiser
    REQUIRE(centre < reader.ChapterCount());

    Window w;
    BuildStats bs;
    REQUIRE(BuildWindowRange(reader, centre, w, bs));
    REQUIRE(bs.netvar_seeds > 0);

    // ★ INSIDE THE WINDOW'S FIRST CHAPTER, which is where the seed is the only answer. Only that
    // chapter's keyframe becomes a SEED; the later chapters' keyframes are snapshots at their own
    // start and become ROWS, so a time after one of them would be answered by a row and this case
    // would pass with the seeds unreadable -- which is exactly how the defect survived this long.
    REQUIRE(centre > 0);
    const int64_t t = (int64_t)reader.ChapterAt(centre - 1).tBeginUs + 500000;
    uint64_t raw = 0;
    REQUIRE(w.NetVars().ValueAt(capture_facts::kNvGameTimer, t, raw));
    MESSAGE("seed value = " << raw);
    CHECK(raw == kLateLastValue);   // this chapter's keyframe snapshot: the ramp's last value

    uint16_t slot = 0;
    REQUIRE(w.NetVarSlotOf(capture_facts::kNvGameTimer, slot));
    CHECK(slot == 1);
}

TEST_CASE("PLAYBACK-FIX-4: before the key's first row the PRIMER answers, and only then") {
    using namespace nvstamp;
    TempDemo demo(L"latekey_primed");
    WriteLateKeyDemo(demo.path);

    DemoReader reader;
    REQUIRE(reader.Open(demo.path));

    NetVarPrimer p;
    ScanNetVarPrimer(reader, p);
    REQUIRE(p.IndexOf(capture_facts::kNvGameTimer) != NetVarPrimer::kMax);

    // The first chapters: the key has not been written yet, so nothing in them names it.
    Window bare;
    BuildStats bare_stats;
    REQUIRE(BuildWindowRange(reader, 0, bare, bare_stats));
    REQUIRE(bare_stats.netvar_rows == 0);
    REQUIRE(bare_stats.netvar_seeds == 0);
    uint64_t unprimed = 0;
    CHECK_FALSE(bare.NetVars().ValueAt(capture_facts::kNvGameTimer, (int64_t)kLateT0, unprimed));

    // The same window, primed. The HUD at T0 is this and nothing else.
    Window w;
    BuildStats bs;
    REQUIRE(BuildWindowRange(reader, 0, w, bs, nullptr, &p));
    CHECK(bs.netvar_seeds_primed > 0);

    uint64_t raw = 0;
    REQUIRE(w.NetVars().ValueAt(capture_facts::kNvGameTimer, (int64_t)kLateT0, raw));
    CHECK(raw == kLateFirstSnapshotValue);   // the opening value, which is the primer's whole job
    uint16_t slot = 0;
    REQUIRE(w.NetVarSlotOf(capture_facts::kNvGameTimer, slot));
    CHECK(slot == 1);
}
