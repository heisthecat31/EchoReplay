// A synthetic spectator capture, generated here and written through the real ManifestBuilder,
// ChapterBuilder, stream codec and container, then read back through DemoReader, ScanTrackSources
// and BuildWindow. It needs no file outside the build, so the wire decode path and the sender-clock
// estimator are exercised on every run -- including the arms no real capture has reached.
//
// The cast, each on its own clock with its own latency:
//   A   slot 1 gen 0   the whole file; carries two wrongly stamped records
//   B   slot 0 gen 0   the whole file
//   C   slot 3 gen 0   leaves at 10 s, a jittery link
//   C2  slot 3 gen 1   the same slot rejoined by someone else at 12 s
//   D   slot 5 gen 0   one second inside one chapter
//   the server         stamps the disc while nobody owns it, and five player records with no user
// plus a last-chapter flood of one-record disc owners that overruns the sender table.
#include "doctest.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <vector>

#include "bootstrap/log.h"
#include "capture/capture_types.h"
#include "capture/chapterbuilder.h"
#include "capture/manifestbuild.h"
#include "capture/streamcodec.h"
#include "foundation/bitio/enginewire.h"
#include "foundation/bitio/wirefields.h"
#include "foundation/demofile/demofile.h"
#include "fake_disc_entity.h"
#include "playback/core/apply.h"
#include "playback/core/window.h"
#include "playback/core/windowbuild.h"

using namespace replay;
using namespace replay::playback;

namespace {

constexpr uint64_t kT0 = 800000000;          // the capture axis, like the real file's ~844 s
constexpr uint64_t kTickUs = 11111;
constexpr uint64_t kSendUs = 22222;          // 45 Hz, the real senders' rate
constexpr uint32_t kSends = 1080;            // 24 s
constexpr uint64_t kEndUs = kSends * kSendUs;
constexpr uint32_t kServerId = UINT32_MAX;
constexpr uint64_t kDiscId = 0x100000000ull;  // tag 1, payload 0

struct Sender {
    const char* name;
    uint32_t id;
    int64_t offsetUs;                  // sender stamp minus capture time
    uint32_t minLatUs, jitterUs;
    uint64_t fromUs, toUs;             // active span, relative to kT0
    bool hasPawn;
};

const Sender kCast[] = {
    {"A", 0x00000001u, 91000000, 20000, 20000, 0, kEndUs, true},
    {"B", 0x00000000u, 2052000000, 30000, 15000, 0, kEndUs, true},
    {"C", 0x00000003u, -500000000, 60000, 140000, 0, 10000000, true},  // facts-ok: CLOCK.CTOR_MAXELAPSED_INIT 10,000,000 in tick arithmetic, not the clock constructor's initialiser
    {"C2", 0x00010003u, 7000000, 25000, 10000, 12000000, kEndUs, true},
    {"D", 0x00000005u, 40000000, 30000, 2000, 6500000, 7500000, true},
    {"server", kServerId, 25000, 10000, 2000, 0, kEndUs, false},
};
constexpr size_t kCastCount = sizeof kCast / sizeof kCast[0];
enum : size_t { kA, kB, kC, kC2, kD, kServer };

// A's two bad records, by send index: one stamped 5 s into the future, one stamped like the
// uninitialised stamps a killed run once recorded.
constexpr uint32_t kFutureSend = 400;  // facts-ok: AUDIO.LISTENER_COUNT_OFF a send index of this fixture, not the listener count offset
constexpr uint64_t kFutureByUs = 5000000;
constexpr uint32_t kBogusSend = 600;
constexpr uint64_t kBogusStamp = 1000000;

// The flood: more one-record disc owners than the sender table has room left for.
constexpr uint32_t kFlood = 70;
constexpr uint32_t kFloodBits = 1100;        // bits11 at and above this marks a flood record
constexpr uint32_t kNoUserRecords = 5;

// Latency drifts along a triangle wave, slowly enough that a sender's records arrive in order.
uint32_t LatencyUs(const Sender& s, uint32_t send) {
    const uint32_t phase = (send + s.id * 37u) % 200u;
    const uint32_t tri = phase < 100u ? phase : 200u - phase;
    return s.minLatUs + (uint32_t)((uint64_t)s.jitterUs * tri / 100u);
}

uint64_t SendTimeUs(uint32_t send) { return kT0 + (uint64_t)send * kSendUs; }

size_t DiscOwnerAt(uint32_t send) {
    const uint64_t t = (uint64_t)send * kSendUs;
    if (t < 4000000) return kA;
    if (t < 6000000) return kServer;
    if (t < 14000000) return kB;
    if (t < 16000000) return kServer;
    return kC2;
}

uint32_t FloodId(uint32_t i) { return 0x0100u + i; }
uint64_t FloodStampUs(uint32_t i) {
    return kT0 + 23000000 + (uint64_t)i * 5000 - 15000 + (uint64_t)(i + 1) * 1000000;
}

struct Arrival {
    uint64_t atUs;
    uint16_t kind;
    uint8_t body[64];
    uint64_t stampUs;
};

// B's head sits on the -90 degree yaw tie for these sends, as user 0's did for 17 s of the 9/24
// capture: the capture cannot prove those records and stores them verbatim.
constexpr uint32_t kTiedFrom = 300;  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF a send index of this fixture, not the extrapolation cap offset
constexpr uint32_t kTiedTo = 700;

// |y| == |w|: the engine sent selector 3, and re-encoding the decoded rotation picks selector 1.
constexpr bitio::Quat10 kTiedHmd = {3, {511, 0, 511}};

void PlayerBody(Arrival& a, uint64_t hdr0, uint32_t bits28, bool tiedHmd = false) {
    capture::PlayerFrame f{};
    f.hmdOri[3] = f.lHandOri[3] = f.rHandOri[3] = 1.0f;
    f.lHandOff[0] = (float)(bits28 % 50) * 0.01f;
    f.bits28 = bits28;
    memset(a.body, 0, sizeof a.body);
    memcpy(a.body, &hdr0, 8);
    memcpy(a.body + 8, &a.stampUs, 8);
    const bitio::WireField* W = bitio::kPlayerStateWire;
    bitio::EngineBitWriter w(a.body + 16, sizeof a.body - 16);
    if (tiedHmd) {
        w.Write(kTiedHmd.selector, 2);
        for (uint32_t c : kTiedHmd.component) w.Write(c, W[0].bits);
    } else {
        w.BitPackQuat(f.hmdOri, W[0].bits);
    }
    w.BitPackQuat(f.lHandOri, W[1].bits);
    w.BitPackQuat(f.rHandOri, W[2].bits);
    w.BitPackVec(f.refVel, W[3].min, W[3].max, W[3].step);
    w.Write(f.bits28, W[4].bits);
    w.Write(f.holdLSlot, W[5].bits);
    w.Write(f.holdLGen, W[6].bits);
    w.Write(f.holdRSlot, W[7].bits);
    w.Write(f.holdRGen, W[8].bits);
    w.BitPackReal(f.bendBack, W[9].min, W[9].max, W[9].step);
    w.BitPackReal(f.bendFwd, W[10].min, W[10].max, W[10].step);
    w.BitPackReal(f.restPoseHeight, W[11].min, W[11].max, W[11].step);
    w.BitPackVec(f.lHandOff, W[12].min, W[12].max, W[12].step);
    w.BitPackVec(f.rHandOff, W[13].min, W[13].max, W[13].step);
    w.BitPackReal(f.trigL, W[14].min, W[14].max, W[14].step);
    w.BitPackReal(f.trigR, W[15].min, W[15].max, W[15].step);
    w.Write(f.anchor, W[16].bits);
    w.BitPackVec(f.refVelDup, W[17].min, W[17].max, W[17].step);
    w.Finish();
    REQUIRE(w.Status() == bitio::WireStatus::Ok);
}

void BodyBody(Arrival& a, uint64_t idValue, uint32_t owner, uint32_t bits11) {
    capture::BodyFrame f{};
    f.ori[3] = 1.0f;
    f.bits11 = bits11;
    f.idValue = idValue;
    const uint64_t hdr0 = (0x50ull << 32) | owner;
    const uint64_t hdr2 = 0x50;
    memset(a.body, 0, sizeof a.body);
    memcpy(a.body, &hdr0, 8);
    memcpy(a.body + 8, &a.stampUs, 8);
    memcpy(a.body + 16, &hdr2, 8);
    const bitio::WireField* W = bitio::kSimStateWire;
    bitio::EngineBitWriter w(a.body + 24, sizeof a.body - 24);
    w.Write(f.bits11, W[0].bits);
    w.Write(f.idValue, W[1].bits);
    w.BitPackVec(f.com, W[2].min, W[2].max, W[2].step);
    w.BitPackQuat(f.ori, W[3].bits);
    w.BitPackVec(f.vel, W[4].min, W[4].max, W[4].step);
    w.BitPackVec(f.angVel, W[5].min, W[5].max, W[5].step);
    w.BitPackVec(f.lastHeldCom, W[6].min, W[6].max, W[6].step);
    w.Finish();
    REQUIRE(w.Status() == bitio::WireStatus::Ok);
}

struct Fed {
    uint32_t players = 0, bodies = 0, discs = 0;
};

// Every record of the capture with the time it reaches the recorder, in arrival order.
std::vector<Arrival> MakeArrivals(Fed& fed) {
    std::vector<Arrival> out;
    for (uint32_t send = 0; send < kSends; ++send) {
        const uint64_t rel = (uint64_t)send * kSendUs;
        for (size_t c = 0; c < kCastCount; ++c) {
            const Sender& s = kCast[c];
            if (!s.hasPawn || rel < s.fromUs || rel >= s.toUs) continue;
            Arrival a{};
            a.atUs = SendTimeUs(send) + LatencyUs(s, send);
            a.stampUs = (uint64_t)((int64_t)SendTimeUs(send) + s.offsetUs);
            a.kind = capture::kRecWirePlayer;
            if (c == kA && send == kFutureSend) a.stampUs += kFutureByUs;
            if (c == kA && send == kBogusSend) a.stampUs = kBogusStamp;
            PlayerBody(a, s.id, send, c == kB && send >= kTiedFrom && send < kTiedTo);
            out.push_back(a);
            ++fed.players;

            a.stampUs = (uint64_t)((int64_t)SendTimeUs(send) + s.offsetUs);
            a.kind = capture::kRecWireBody;
            BodyBody(a, s.id, s.id, send);
            out.push_back(a);
            ++fed.bodies;
        }
        const Sender& owner = kCast[DiscOwnerAt(send)];
        Arrival d{};
        d.atUs = SendTimeUs(send) + LatencyUs(owner, send);
        d.stampUs = (uint64_t)((int64_t)SendTimeUs(send) + owner.offsetUs);
        d.kind = capture::kRecWireBody;
        BodyBody(d, kDiscId, owner.id, send);
        out.push_back(d);
        ++fed.discs;
    }
    const Sender& server = kCast[kServer];
    for (uint32_t i = 0; i < kNoUserRecords; ++i) {
        const uint32_t send = 100 + i * 200;
        Arrival a{};
        a.atUs = SendTimeUs(send) + LatencyUs(server, send);
        a.stampUs = (uint64_t)((int64_t)SendTimeUs(send) + server.offsetUs);
        a.kind = capture::kRecWirePlayer;
        PlayerBody(a, UINT64_MAX, send);
        out.push_back(a);
        ++fed.players;
    }
    for (uint32_t i = 0; i < kFlood; ++i) {
        Arrival d{};
        d.atUs = kT0 + 23000000 + (uint64_t)i * 5000;
        d.stampUs = FloodStampUs(i);
        d.kind = capture::kRecWireBody;
        BodyBody(d, kDiscId, FloodId(i), kFloodBits + i);
        out.push_back(d);
        ++fed.discs;
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Arrival& x, const Arrival& y) { return x.atUs < y.atUs; });
    return out;
}

template <class T>
void Feed(capture::ChapterBuilder& b, uint16_t kind, const T& payload, const uint8_t* tail = nullptr,
          size_t tailBytes = 0) {
    uint8_t buf[sizeof(capture::RecHeader) + sizeof(T) + 64];
    REQUIRE(tailBytes <= 64);
    capture::RecHeader h;
    h.kind = kind;
    h.bytes = (uint16_t)(sizeof(T) + tailBytes);
    h.producer = 0;
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, &payload, sizeof payload);
    if (tailBytes) memcpy(buf + sizeof h + sizeof payload, tail, tailBytes);
    b.Consume(buf, (uint32_t)(sizeof h + sizeof payload + tailBytes));
}

struct TempDemo {
    wchar_t path[512];
    TempDemo() {
        wchar_t dir[400];  // facts-ok: AUDIO.LISTENER_COUNT_OFF a temp-path buffer length, not the listener count offset
        if (!GetTempPathW(400, dir)) { dir[0] = L'.'; dir[1] = 0; }  // facts-ok: AUDIO.LISTENER_COUNT_OFF a temp-path buffer length, not the listener count offset
        _snwprintf_s(path, 512, _TRUNCATE, L"%lstrackmap_wire_%lu.echodemo", dir,
                     (unsigned long)GetCurrentProcessId());
    }
    ~TempDemo() { DeleteFileW(path); }
};

void WriteCapture(const wchar_t* path, Fed& fed) {
    using namespace replay::capture;
    ManifestBuilder mb;
    mb.Begin(kT0, 0, 0x40);
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
    b.EmitMatchBegin(kT0);

    const std::vector<Arrival> arrivals = MakeArrivals(fed);
    size_t next = 0;
    uint32_t seq = 0;
    const uint32_t ticks = (uint32_t)(kEndUs / kTickUs) + 30;   // past the last latency
    for (uint32_t f = 0; f < ticks; ++f) {
        const uint64_t now = kT0 + (uint64_t)f * kTickUs;
        TickRec tr{};
        tr.sourceTimeUs = now;
        tr.simTimeUs = 1000000 + (uint64_t)f * kTickUs;
        tr.frameIndex = f + 1;
        tr.frameDeltaUs = (uint32_t)kTickUs;
        tr.scaledDeltaUs = (uint32_t)kTickUs;
        Feed(b, kRecTick, tr);
        // What the tap delivers before the next tick.
        while (next < arrivals.size() && arrivals[next].atUs < now + kTickUs) {
            const Arrival& a = arrivals[next++];
            WireRec r{};
            r.stampUs = a.stampUs;
            r.arrivalUs = a.atUs;
            r.seq = seq++;
            r.bodyBytes = sizeof a.body;
            Feed(b, a.kind, r, a.body, sizeof a.body);
        }
    }
    REQUIRE(next == arrivals.size());
    b.EmitMatchEnd(kT0 + (uint64_t)ticks * kTickUs);
    b.FlushChapter();
    b.Close();
    writer.Close();
}

size_t ClockOf(const WireClocks& c, uint32_t sender) {
    for (size_t i = 0; i < c.count; ++i) {
        if (c.sender[i] == sender) return i;
    }
    return c.count;
}

void Add(StreamStats& sum, const StreamStats& s) {
    sum.read += s.read;
    sum.keyed += s.keyed;
    sum.dropped_verbatim += s.dropped_verbatim;
    sum.recovered_verbatim += s.recovered_verbatim;
    sum.dropped_identity += s.dropped_identity;
    sum.dropped_clock += s.dropped_clock;
    sum.dropped_window += s.dropped_window;
    sum.clock_clamped += s.clock_clamped;
}

}  // namespace

TEST_CASE("TRACKMAP: a synthetic wire capture is keyed and clocked through the real codec") {
    TempDemo tmp;
    Fed fed;
    WriteCapture(tmp.path, fed);

    DemoReader reader;
    REQUIRE(reader.Open(tmp.path));
    TrackScan scan;
    const char* why = "";
    REQUIRE_MESSAGE(ScanTrackSources(reader, scan, &why), why);
    const WireClocks& clocks = scan.clocks;
    CHECK(scan.wire_records == fed.players + fed.bodies + fed.discs);

    // ---- the sender table: full, and the flood past it counted ----------------------------------
    CHECK(clocks.count == WireClocks::kMaxSenders);
    const uint32_t floodWithClock = (uint32_t)(WireClocks::kMaxSenders - kCastCount);
    CHECK(clocks.senders_overflowed == kFlood - floodWithClock);

    // ---- each sender's offset: its own clock, less a latency it really had -----------------------
    // A chapter's bound is off (offset - latency) by at most the wait for its nearest record, one
    // send period, whichever of the two bounds or their midpoint the estimate came from.
    for (size_t c = 0; c < kCastCount; ++c) {
        if (c == kD) continue;   // bracketed, below
        const Sender& s = kCast[c];
        const size_t at = ClockOf(clocks, s.id);
        REQUIRE_MESSAGE(at < clocks.count, s.name);
        const int64_t residual = s.offsetUs - clocks.offset_us[at];
        INFO(s.name, " residual us ", residual, " chapters ", clocks.chapters[at]);
        CHECK(residual >= (int64_t)s.minLatUs - (int64_t)kSendUs);
        CHECK(residual <= (int64_t)(s.minLatUs + s.jitterUs + kSendUs));
    }

    // ---- A: one record 5 s in the future and one stamped near zero ------------------------------
    // Both own a whole-file extreme. Neither moves the offset, and the load line says so.
    {
        const size_t a = ClockOf(clocks, kCast[kA].id);
        // 5 s ahead of a send 1.1 s before its chapter closed.
        CHECK(clocks.raw_late_us[a] - clocks.late_us[a] > 3000000);
        CHECK(clocks.ignored[a] == 2);
        CHECK(clocks.early_us[a] - clocks.raw_early_us[a] > 80000000);
        CHECK(clocks.SpreadUs(a) > WireClocks::kSpreadWarnUs);
        char line[256];
        FormatWireClock(clocks, a, "slot 1 gen 0", line, sizeof line);
        CHECK(strstr(line, "the chapters disagree") != nullptr);

        const size_t b = ClockOf(clocks, kCast[kB].id);
        CHECK(clocks.SpreadUs(b) <= (int64_t)kCast[kB].jitterUs + (int64_t)(kSendUs + kTickUs));
        FormatWireClock(clocks, b, "slot 0 gen 0", line, sizeof line);
        CHECK(strstr(line, "the chapters disagree") == nullptr);
        CHECK(strstr(line, "per-chapter offset spread") != nullptr);
    }

    // ---- D: seen inside one chapter only, so the two bounds bracket the offset ------------------
    {
        const Sender& d = kCast[kD];
        const size_t at = ClockOf(clocks, d.id);
        REQUIRE(at < clocks.count);
        CHECK(clocks.chapters[at] == 1);
        CHECK(clocks.early_us[at] > clocks.late_us[at]);
        const uint32_t firstSend = (uint32_t)((d.fromUs + kSendUs - 1) / kSendUs);
        const uint32_t lastSend = (uint32_t)((d.toUs - 1) / kSendUs);
        const size_t ci = ChapterForTime(reader, (int64_t)(kT0 + 7000000));
        const ChapterKey ck = reader.ChapterAt(ci);
        const int64_t early = (int64_t)SendTimeUs(firstSend) + d.offsetUs - (int64_t)ck.tBeginUs;
        const int64_t late = (int64_t)SendTimeUs(lastSend) + d.offsetUs - (int64_t)ck.tEndUs;
        CHECK(clocks.offset_us[at] == late + (early - late) / 2);
        CHECK(llabs(clocks.offset_us[at] - d.offsetUs) < 300000);   // `late` alone is ~0.5 s out
    }

    // ---- the second generation is a second sender ------------------------------------------------
    CHECK(ClockOf(clocks, kCast[kC].id) != ClockOf(clocks, kCast[kC2].id));

    // ---- every chapter built: what was keyed, dropped, clamped -----------------------------------
    Window w;
    REQUIRE(w.Reserve(CapsForScan(CapsForCadence(2000000, (uint32_t)kTickUs), scan), &why));
    StreamStats bodyWire, playerWire;
    std::map<uint32_t, uint32_t> playerSamples, bodySamples;   // by sender id
    uint32_t discSamples = 0, discFlood = 0, misplaced = 0, futureSeen = 0;
    uint32_t held = 0, heldElsewhere = 0;
    size_t regressions = 0;
    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        if (reader.ChapterAt(i).section != kSectionCore) continue;
        BuildStats st;
        REQUIRE_MESSAGE(BuildWindow(reader, i, w, st, &clocks), st.refusal);
        Add(bodyWire, st.body_wire);
        Add(playerWire, st.player_wire);
        CHECK(st.body_sampled.read == 0);
        CHECK(st.player_sampled.read == 0);
        regressions += w.SampleTimeRegressions();

        for (size_t p = 0; p < w.PlayerCount(); ++p) {
            const PlayerTrack& t = w.PlayerAt(p);
            CHECK_FALSE(t.from_sampled);
            const uint32_t id = (uint32_t)t.user_slot | ((uint32_t)t.generation << 16);
            size_t c = 0;
            while (c < kCastCount && kCast[c].id != id) ++c;
            REQUIRE(c < kCastCount);
            const int64_t residual = kCast[c].offsetUs - clocks.offset_us[ClockOf(clocks, id)];
            // A track's stamps never run backwards (Window::AddPlayerSample), so a sample placed
            // before its predecessor is held at the predecessor's time and counted.
            int64_t floor = 0;
            for (size_t k = 0; k < t.count; ++k) {
                const uint32_t send = w.PlayerFrames(t)[k].bits28;
                const int64_t at = (int64_t)w.PlayerTimes(t)[k];
                ++playerSamples[id];
                int64_t placed = (int64_t)SendTimeUs(send) + residual;
                if (c == kA && send == kFutureSend) {
                    placed += (int64_t)kFutureByUs;
                    futureSeen += at == placed;
                } else if (c == kA && send == kBogusSend) {
                    placed = 0;   // stamped before A's offset
                }
                if (at != std::max(placed, floor)) ++misplaced;
                if (at != placed) {
                    ++held;
                    heldElsewhere += c != kA;
                }
                floor = at;
            }
        }
        for (size_t b = 0; b < w.BodyCount(); ++b) {
            const BodyTrack& t = w.BodyAt(b);
            const IdKind kind = ClassifyBodyIdentity(t.id.fill, t.id.tag);
            REQUIRE((kind == IdKind::UserIdByTag || kind == IdKind::ComponentIdByTag));
            const bool disc = kind == IdKind::ComponentIdByTag;
            if (disc) CHECK(t.id.value == 0);
            int64_t floor = 0;
            for (size_t k = 0; k < t.count; ++k) {
                const uint32_t send = w.BodyFrames(t)[k].bits11;
                const int64_t at = (int64_t)w.BodyTimes(t)[k];
                int64_t placed = 0;
                if (disc && send >= kFloodBits) {
                    const uint32_t i = send - kFloodBits;
                    placed = (int64_t)FloodStampUs(i) - clocks.offset_us[ClockOf(clocks, FloodId(i))];
                    ++discFlood;
                } else {
                    const uint32_t id = disc ? kCast[DiscOwnerAt(send)].id : (uint32_t)t.id.value;
                    size_t c = 0;
                    while (c < kCastCount && kCast[c].id != id) ++c;
                    REQUIRE(c < kCastCount);
                    if (!disc) ++bodySamples[id];
                    placed = (int64_t)SendTimeUs(send) + kCast[c].offsetUs -
                             clocks.offset_us[ClockOf(clocks, id)];
                }
                discSamples += disc;
                if (at != std::max(placed, floor)) ++misplaced;
                if (at != placed) {
                    ++held;
                    // The disc changes clocks when it changes hands, and the new owner's latency
                    // residue can be the smaller one. A player's own body never does.
                    heldElsewhere += !disc;
                }
                floor = at;
            }
        }
    }

    // B's tied records were stored verbatim and every one decoded back; nothing else was verbatim.
    CHECK(bodyWire.dropped_verbatim == 0);
    CHECK(playerWire.dropped_verbatim == 0);
    CHECK(bodyWire.recovered_verbatim == 0);
    CHECK(playerWire.recovered_verbatim == kTiedTo - kTiedFrom);
    CHECK(bodyWire.read == fed.bodies + fed.discs);
    CHECK(playerWire.read == fed.players);
    CHECK(playerWire.dropped_identity == kNoUserRecords);   // all-ones header: no user
    CHECK(bodyWire.dropped_identity == 0);
    CHECK(bodyWire.dropped_clock == kFlood - floodWithClock);   // owners past the sender table
    CHECK(playerWire.dropped_clock == 0);
    CHECK(playerWire.clock_clamped == 1);                   // A's near-zero stamp
    CHECK(bodyWire.clock_clamped == 0);
    CHECK(bodyWire.read == bodyWire.keyed + bodyWire.Dropped());
    CHECK(playerWire.read == playerWire.keyed + playerWire.Dropped());

    // Every record sits where its sender's clock puts it, or is held at its predecessor's time by
    // the track's no-backwards rule and counted. The damage of A's two bad stamps stays in A's
    // player track -- the future one holds the samples of its own chapter that follow it, the
    // near-zero one is held itself -- and the only other held samples are the disc's, where it
    // changes hands.
    CHECK(misplaced == 0);
    CHECK(futureSeen == 1);
    CHECK(held >= 2);
    CHECK(held == regressions);
    CHECK(heldElsewhere == 0);

    // One track per recorded player, the rejoined slot as two, and one disc under every owner.
    for (size_t c = 0; c < kCastCount; ++c) {
        if (!kCast[c].hasPawn) continue;
        const Sender& s = kCast[c];
        const uint32_t sends =
            (uint32_t)((s.toUs + kSendUs - 1) / kSendUs) - (uint32_t)((s.fromUs + kSendUs - 1) / kSendUs);
        INFO(s.name);
        CHECK(playerSamples[s.id] == sends);
        CHECK(bodySamples[s.id] == sends);
    }
    CHECK(playerSamples.size() == 5);
    CHECK(bodySamples.size() == 5);
    CHECK(discSamples == kSends + floodWithClock);
    CHECK(discFlood == floodWithClock);

    BuildStats total;
    total.player_wire = playerWire;
    char summary[1024];
    FormatBuildSummary(total, 1, summary, sizeof summary);   // the player streams' part
    CHECK(strstr(summary, "window=0) clamped=1 | player_sampled") != nullptr);
    // Every part fits the log writer's line with the caller's prefix still to come, which is the
    // whole point of splitting it: at 512 the unsplit summary was cut off mid-number.
    for (size_t part = 0; part < kBuildSummaryParts; ++part) {
        FormatBuildSummary(total, part, summary, sizeof summary);
        CHECK(strlen(summary) + 64 < replay::kLogLineMax);
    }

    // ---- DISC-BIND: the tag-1 disc, whoever owns it on the wire, ends up bound and driven ---------
    disctest::DiscEntity ent;
    constexpr uint16_t kDiscHandle = 5;
    ent.PlaceDisc((uint32_t)kDiscId, kDiscHandle);
    uint16_t slotMap[PawnResolver::kSlots];
    for (uint16_t s = 0; s < PawnResolver::kSlots; ++s) slotMap[s] = (uint16_t)(s + 6);
    StateApplier ap;
    ap.Bind(&ent, nullptr, nullptr, nullptr);

    const struct { uint64_t atUs; size_t owner; } kEpochs[] = {
        {2000000, kA}, {5000000, kServer}, {10000000, kB}, {15000000, kServer}, {20000000, kC2}};  // facts-ok: CLOCK.CTOR_MAXELAPSED_INIT 10,000,000 in tick arithmetic, not the clock constructor's initialiser
    size_t installsBefore = 0;
    for (const auto& epoch : kEpochs) {
        CAPTURE(epoch.atUs);
        const int64_t playhead = (int64_t)(kT0 + epoch.atUs);
        BuildStats st;
        REQUIRE(BuildWindow(reader, ChapterForTime(reader, playhead), w, st, &clocks));
        const BindResult bind =
            BindBodyHandles(ent, w, slotMap, PawnResolver::kSlots, /*drive_disc=*/true);
        CHECK(bind.bound_disc == 1);
        CHECK(bind.disc_verdict == BindResult::DiscVerdict::Agreed);
        CHECK(bind.not_a_net_id == 0);
        for (size_t b = 0; b < w.BodyCount(); ++b) {
            const BodyTrack& t = w.BodyAt(b);
            const bool disc = ClassifyBodyIdentity(t.id.fill, t.id.tag) == IdKind::ComponentIdByTag;
            CHECK(t.claim_owner == disc);
            if (t.handle_valid) CHECK((t.handle.v == kDiscHandle) == disc);
        }

        REQUIRE(ent.IsLocallyControlled(PhysHandle(kDiscHandle)));   // the host's, until claimed
        const ApplyStats as = ap.Apply(w, playhead, ApplyOptions());
        CHECK(as.disc_install_refused.why == InstallRefusal::None);
        CHECK(ent.owner[kDiscHandle] == disctest::DiscEntity::kSyntheticOwner);
        const disctest::DiscEntity::Ring& ring = ent.ring[kDiscHandle];
        REQUIRE(ring.installs == installsBefore + 1);
        installsBefore = ring.installs;
        CHECK(ring.t == (uint64_t)playhead);
        // bits11 is the send index the fixture stamped into the record.
        const uint32_t send = ring.s.bits11;
        CHECK(DiscOwnerAt(send) == epoch.owner);
        CHECK(llabs((int64_t)((uint64_t)send * kSendUs) - (int64_t)epoch.atUs) < 500000);

        CHECK(ap.ReleaseSyntheticOwners(w) >= 1);
        CHECK(ent.owner[kDiscHandle] == disctest::DiscEntity::kHostOwner);
    }
}

TEST_CASE("a verbatim player record decodes to the pose the proved path gives") {
    Arrival a{};
    a.stampUs = 1700000000000000ull;
    PlayerBody(a, kCast[kA].id, 17);

    capture::PlayerFrame proved{};
    REQUIRE(capture::DecodePlayerBody(a.body, sizeof a.body, proved) == capture::WireDecode::Ok);

    std::vector<uint8_t> buf(1u << 12, 0);
    bitio::BitWriter w(buf.data(), buf.size());
    capture::PlayerEncoder enc(false);
    REQUIRE(enc.EncodeVerbatim(w, 0, a.stampUs, a.body, sizeof a.body));
    REQUIRE(enc.Encode(w, 0, a.stampUs, proved));
    bitio::BitReader r(buf.data(), buf.size());
    capture::PlayerDecoder dec(false);
    uint8_t scratchA[512], scratchB[512];
    capture::DecodedPlayer stored{}, normal{};
    REQUIRE(dec.Decode(r, stored, scratchA, sizeof scratchA));
    REQUIRE(dec.Decode(r, normal, scratchB, sizeof scratchB));
    REQUIRE(stored.verbatim);
    REQUIRE_FALSE(normal.verbatim);

    REQUIRE(RecoverWireVerbatim(stored));
    CHECK_FALSE(stored.verbatim);
    CHECK(stored.stampUs == normal.stampUs);
    const capture::PlayerFrame& v = stored.frame;
    const capture::PlayerFrame& p = normal.frame;
    CHECK(memcmp(v.hmdOri, p.hmdOri, sizeof v.hmdOri) == 0);
    CHECK(memcmp(v.lHandOri, p.lHandOri, sizeof v.lHandOri) == 0);
    CHECK(memcmp(v.rHandOri, p.rHandOri, sizeof v.rHandOri) == 0);
    CHECK(memcmp(v.lHandOff, p.lHandOff, sizeof v.lHandOff) == 0);
    CHECK(memcmp(v.rHandOff, p.rHandOff, sizeof v.rHandOff) == 0);
    CHECK(memcmp(v.refVel, p.refVel, sizeof v.refVel) == 0);
    CHECK(v.trigL == p.trigL);
    CHECK(v.trigR == p.trigR);
    CHECK(v.bits28 == p.bits28);
    CHECK(v.anchor == p.anchor);
    CHECK(v.hdr0 == p.hdr0);
    CHECK(v.fill == p.fill);

    SUBCASE("a tied head the proof refuses still decodes to the engine's own rotation") {
        PlayerBody(a, kCast[kA].id, 17, true);
        capture::PlayerFrame tied{};
        REQUIRE(capture::DecodePlayerBody(a.body, sizeof a.body, tied) ==
                capture::WireDecode::ReencodeDiffers);
        capture::DecodedPlayer d{};
        d.verbatim = true;
        d.body = a.body;
        d.bodyBytes = sizeof a.body;
        REQUIRE(RecoverWireVerbatim(d));
        float engine[4];
        REQUIRE(bitio::DequantQuat(kTiedHmd, bitio::kPlayerStateWire[0].bits, engine) ==
                bitio::WireStatus::Ok);
        CHECK(memcmp(d.frame.hmdOri, engine, sizeof engine) == 0);
        CHECK(memcmp(d.frame.lHandOri, proved.lHandOri, sizeof proved.lHandOri) == 0);
        CHECK(memcmp(d.frame.lHandOff, proved.lHandOff, sizeof proved.lHandOff) == 0);
        CHECK(d.frame.bits28 == 17);
    }
    SUBCASE("a body whose padding is not zero stays verbatim") {
        PlayerBody(a, kCast[kA].id, 17, true);
        a.body[sizeof a.body - 1] = 0x80;
        capture::DecodedPlayer d{};
        d.verbatim = true;
        d.body = a.body;
        d.bodyBytes = sizeof a.body;
        CHECK_FALSE(RecoverWireVerbatim(d));
        CHECK(d.verbatim);
    }
}

