// Which recorded PLAYER a body or player record belongs to, proved on the two files
// that matter: the owner's real spectator capture of a live match, where every pose arrives over the
// wire, and the solo fixture, where none does.
//
// Neither file is in the repo (`*.echodemo` is ignored, and the real one lives in the owner's
// Documents). A file that is absent or does not open skips its case, or fails it when the run sets
// ECHOREPLAY_TEST_REQUIRE_CAPTURE=1 (tests/local_inputs.h). The decode and clock
// path itself does not depend on either file: the synthetic wire capture at the foot of this file
// is generated here, written through the real writer, and always runs.
#include "doctest.h"

#include <windows.h>
#include "../local_inputs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include "capture/capture_types.h"
#include "capture/chapterbuilder.h"
#include "capture/manifestbuild.h"
#include "capture/streamcodec.h"
#include "foundation/bitio/bitio.h"
#include "foundation/bitio/enginewire.h"
#include "foundation/bitio/wirefields.h"
#include "foundation/demofile/demofile.h"
#include "playback/core/sample.h"
#include "playback/core/window.h"
#include "playback/core/windowbuild.h"

using namespace replay;
using namespace replay::playback;

namespace {

#define TRACKMAP_REAL_CAPTURE "2026-09-18_03-33-45_576ED3F8428EBC4B_00000000FFFFFFFF.echodemo"
#define TRACKMAP_WIDEN_(x) L##x
#define TRACKMAP_WIDEN(x) TRACKMAP_WIDEN_(x)

bool OpenRealCapture(DemoReader& reader, char* where, size_t chars) {
    const wchar_t* home = _wgetenv(L"USERPROFILE");
    if (home == nullptr) {
        _snprintf_s(where, chars, _TRUNCATE, "USERPROFILE is not set");
        return false;
    }
    wchar_t path[1024];
    _snwprintf_s(path, _TRUNCATE, L"%s/Documents/EchoReplay/%s", home,
                 TRACKMAP_WIDEN(TRACKMAP_REAL_CAPTURE));
    _snprintf_s(where, chars, _TRUNCATE, "%%USERPROFILE%%/Documents/EchoReplay/%s",
                TRACKMAP_REAL_CAPTURE);
    return reader.Open(path);
}

bool OpenTiedHeadCapture(DemoReader& reader, char* where, size_t chars) {
    static const wchar_t kName[] = L"2026-09-24_12-59-23_576ED3F8428EBC4B_CB60A4DE7E1CAF73.echodemo";
    const wchar_t* home = _wgetenv(L"USERPROFILE");
    if (home == nullptr) {
        _snprintf_s(where, chars, _TRUNCATE, "USERPROFILE is not set");
        return false;
    }
    wchar_t path[1024];
    _snwprintf_s(path, _TRUNCATE, L"%s/Documents/EchoReplay/%s", home, kName);
    _snprintf_s(where, chars, _TRUNCATE, "%%USERPROFILE%%/Documents/EchoReplay/%ls", kName);
    return reader.Open(path);
}

bool OpenSoloFixture(DemoReader& reader, char* where, size_t chars) {
    _snprintf_s(where, chars, _TRUNCATE, "%s", REPLAY_TRACKMAP_SOLO_FIXTURE);
    return reader.Open(TRACKMAP_WIDEN(REPLAY_TRACKMAP_SOLO_FIXTURE));
}

// True when the case may go on.
bool FilePresentOrExcused(bool opened, const char* where) {
    if (opened) return true;
    if (replay_test::MissingCaptureFails()) {
        FAIL_CHECK("absent or unopenable: " << doctest::String(where) << " (" << replay_test::kRequireCaptureEnv
                                            << "=1 requires it)");
    } else {
        WARN_MESSAGE(false, "SKIPPED: absent or unopenable: ", doctest::String(where));
    }
    return false;
}

uint64_t Fnv(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}
constexpr uint64_t kFnvSeed = 1469598103934665603ull;

uint64_t BodyPoseKey(const BodyState& s) {
    uint64_t h = Fnv(kFnvSeed, s.com, sizeof s.com);
    h = Fnv(h, s.ori, sizeof s.ori);
    return Fnv(h, s.vel, sizeof s.vel);
}

uint64_t PlayerPoseKey(const PlayerState& s) {
    uint64_t h = Fnv(kFnvSeed, s.hmdOri, sizeof s.hmdOri);
    h = Fnv(h, s.lHandOri, sizeof s.lHandOri);
    h = Fnv(h, s.rHandOri, sizeof s.rHandOri);
    h = Fnv(h, s.lHandOff, sizeof s.lHandOff);
    return Fnv(h, s.rHandOff, sizeof s.rHandOff);
}

// Who a record says it belongs to, read straight off the decoders and independent of the keying
// under test. Wire bodies carry it twice -- the id field and the header's owner -- and the player
// bodies' two readings are asserted equal where the oracle is built.
enum SourceKind : uint32_t { kWirePlayer = 1, kWireBodyOfPlayer, kWireBodyComponent, kSampledBody };
uint64_t SourceOf(SourceKind kind, uint32_t who) { return ((uint64_t)kind << 32) | who; }

struct Oracle {
    std::map<uint64_t, std::set<uint64_t>> bodies;    // pose key -> every source that recorded it
    std::map<uint64_t, std::set<uint64_t>> players;
    std::set<uint32_t> wirePlayerSlots, wireBodySlots;
    uint32_t bodyWire = 0, bodySampled = 0, playerWire = 0, playerSampled = 0, verbatim = 0;
    uint32_t idDisagreesWithOwner = 0;
};

// Every verbatim record of these files failed only the re-encode proof; its values are the wire's.
bool DecodesVerbatim(capture::WireDecode r) {
    return r == capture::WireDecode::Ok || r == capture::WireDecode::ReencodeDiffers;
}

void BuildOracle(DemoReader& reader, Oracle& o) {
    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        if (reader.ChapterAt(i).section != kSectionCore) continue;
        DecodedChapter dc;
        REQUIRE(reader.DecodeChapter(i, dc));
        const struct { StreamId id; bool sampled, body; } streams[] = {
            {kStreamBodyWire, false, true},
            {kStreamBodySampled, true, true},
            {kStreamPlayerWire, false, false},
            {kStreamPlayerSampled, true, false},
        };
        for (const auto& st : streams) {
            const StreamView* sv = dc.Find(st.id);
            if (!sv || !sv->records || sv->bytes <= capture::kBlockPrefixBytes) continue;
            bitio::BitReader r(sv->records + capture::kBlockPrefixBytes,
                               sv->bytes - capture::kBlockPrefixBytes);
            capture::BodyDecoder bd(st.sampled);
            capture::PlayerDecoder pd(st.sampled);
            uint8_t scratch[512];
            for (uint32_t k = 0; k < sv->recordCount; ++k) {
                if (st.body) {
                    capture::DecodedBody d;
                    memset(&d, 0, sizeof d);
                    REQUIRE(bd.Decode(r, d, scratch, sizeof scratch));
                    ++(st.sampled ? o.bodySampled : o.bodyWire);
                    if (d.verbatim) {
                        ++o.verbatim;
                        if (!DecodesVerbatim(capture::DecodeBodyBody(d.body, d.bodyBytes, d.frame))) continue;
                    }
                    BodyState s;
                    memset(&s, 0, sizeof s);
                    memcpy(s.com, d.frame.com, sizeof s.com);
                    memcpy(s.ori, d.frame.ori, sizeof s.ori);
                    memcpy(s.vel, d.frame.vel, sizeof s.vel);
                    uint64_t src;
                    if (st.sampled) {
                        src = SourceOf(kSampledBody, d.entity);
                    } else if ((d.frame.idValue >> 32) == 0) {
                        const uint32_t slot = (uint32_t)d.frame.idValue & 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
                        if (slot != ((uint32_t)d.frame.hdr0 & 0xFFFFu)) ++o.idDisagreesWithOwner;  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
                        o.wireBodySlots.insert(slot);
                        src = SourceOf(kWireBodyOfPlayer, slot);
                    } else {
                        src = SourceOf(kWireBodyComponent, (uint32_t)d.frame.idValue);
                    }
                    o.bodies[BodyPoseKey(s)].insert(src);
                } else {
                    capture::DecodedPlayer d;
                    memset(&d, 0, sizeof d);
                    REQUIRE(pd.Decode(r, d, scratch, sizeof scratch));
                    ++(st.sampled ? o.playerSampled : o.playerWire);
                    if (d.verbatim) {
                        ++o.verbatim;
                        if (!DecodesVerbatim(capture::DecodePlayerBody(d.body, d.bodyBytes, d.frame))) continue;
                    }
                    PlayerState s;
                    memset(&s, 0, sizeof s);
                    memcpy(s.hmdOri, d.frame.hmdOri, sizeof s.hmdOri);
                    memcpy(s.lHandOri, d.frame.lHandOri, sizeof s.lHandOri);
                    memcpy(s.rHandOri, d.frame.rHandOri, sizeof s.rHandOri);
                    memcpy(s.lHandOff, d.frame.lHandOff, sizeof s.lHandOff);
                    memcpy(s.rHandOff, d.frame.rHandOff, sizeof s.rHandOff);
                    const uint32_t slot = st.sampled ? d.entity : ((uint32_t)d.frame.hdr0 & 0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
                    if (!st.sampled) o.wirePlayerSlots.insert(slot);
                    o.players[PlayerPoseKey(s)].insert(SourceOf(kWirePlayer, slot));
                }
            }
        }
    }
}

// The source a body track CLAIMS, from its key alone.
uint64_t ClaimedSource(const BodyTrack& t) {
    switch (ClassifyBodyIdentity(t.id.fill, t.id.tag)) {
        case IdKind::UserIdByTag:      return SourceOf(kWireBodyOfPlayer, (uint32_t)t.id.value & 0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
        case IdKind::ComponentIdByTag: return SourceOf(kWireBodyComponent, (uint32_t)t.id.value);
        default: break;
    }
    return SourceOf(kSampledBody, (uint32_t)t.id.value);
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

void PrintStream(const char* file, const char* name, const StreamStats& s) {
    printf("TRACKMAP %s %s read=%u keyed=%u recovered=%u verbatim=%u identity=%u clock=%u window=%u\n",
           file, name, s.read, s.keyed, s.recovered_verbatim, s.dropped_verbatim, s.dropped_identity,
           s.dropped_clock, s.dropped_window);
}

struct Totals {
    StreamStats bodyWire, bodySampled, playerWire, playerSampled;
    uint32_t cores = 0, unresolvable = 0, noNetId = 0, bodySamples = 0, playerSamples = 0;
    uint64_t hash = kFnvSeed;
};

// Every CORE chapter built on its own: the chapters partition the file, so the sums are the file's.
// The hash is over each chapter's track keys, stamps and one pose field per sample, in track order.
void BuildEveryChapter(DemoReader& reader, Window& w, const WireClocks* clocks, Totals& tot) {
    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        if (reader.ChapterAt(i).section != kSectionCore) continue;
        BuildStats st;
        REQUIRE_MESSAGE(BuildWindow(reader, i, w, st, clocks), st.refusal);
        ++tot.cores;
        Add(tot.bodyWire, st.body_wire);
        Add(tot.bodySampled, st.body_sampled);
        Add(tot.playerWire, st.player_wire);
        Add(tot.playerSampled, st.player_sampled);
        tot.unresolvable += st.body_identity_unresolvable;
        tot.noNetId += st.body_no_net_id;
        tot.bodySamples += st.body_samples;
        tot.playerSamples += st.player_samples;

        uint64_t h = kFnvSeed;
        for (size_t b = 0; b < w.BodyCount(); ++b) {
            const BodyTrack& t = w.BodyAt(b);
            h = Fnv(h, &t.id.value, 8);
            h = Fnv(h, &t.id.tag, 4);
            h = Fnv(h, &t.id.fill, 4);
            h = Fnv(h, w.BodyTimes(t), t.count * 8);
            for (size_t k = 0; k < t.count; ++k) h = Fnv(h, w.BodyFrames(t)[k].com, 12);
        }
        for (size_t p = 0; p < w.PlayerCount(); ++p) {
            const PlayerTrack& t = w.PlayerAt(p);
            h = Fnv(h, &t.user_slot, 2);
            h = Fnv(h, w.PlayerTimes(t), t.count * 8);
            for (size_t k = 0; k < t.count; ++k) h = Fnv(h, w.PlayerFrames(t)[k].hmdOri, 16);
        }
        tot.hash = Fnv(tot.hash, &h, 8);
    }
}

float Distance(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

}  // namespace

TEST_CASE("TRACKMAP: the real spectator capture keys every wire record to its recorded player") {
    DemoReader reader;
    char where[1024];
    const bool present = OpenRealCapture(reader, where, sizeof where);
    INFO("capture: ", where);
    if (!FilePresentOrExcused(present, where)) return;

    static Oracle oracle;
    oracle = Oracle();
    BuildOracle(reader, oracle);
    // THE SUBJECT MUST EXIST: a spectator capture, three players, their slots out of wire order.
    REQUIRE(oracle.playerSampled == 0);
    REQUIRE(oracle.playerWire == 6523);
    REQUIRE(oracle.bodyWire == 8703);
    REQUIRE(oracle.wirePlayerSlots == std::set<uint32_t>{0, 1, 3});
    REQUIRE(oracle.wireBodySlots == std::set<uint32_t>{0, 1, 3});
    // The writer-side fact the body key rests on: a tag-0 id's slot IS the header's owner slot.
    CHECK(oracle.idDisagreesWithOwner == 0);

    TrackScan scan;
    const char* why = "";
    REQUIRE_MESSAGE(ScanTrackSources(reader, scan, &why), why);
    for (size_t i = 0; i < scan.clocks.count; ++i) {
        // `untrimmed` is the estimate the whole-file extremes gave before the bounds were trimmed.
        const int64_t rawLate = scan.clocks.raw_late_us[i], rawEarly = scan.clocks.raw_early_us[i];
        const int64_t untrimmed = rawLate >= rawEarly ? rawLate : rawLate + (rawEarly - rawLate) / 2;
        printf("TRACKMAP real clock sender %08x chapters %u untrimmed %lld trimmed-untrimmed %lld us "
               "spread %lld\n",
               scan.clocks.sender[i], scan.clocks.chapters[i], (long long)untrimmed,
               (long long)(scan.clocks.offset_us[i] - untrimmed), (long long)scan.clocks.SpreadUs(i));
        // Trimming moves a sound sender by milliseconds, and no sender of this file is past the Warn.
        CHECK(llabs(scan.clocks.offset_us[i] - untrimmed) <= 3000);
        CHECK(scan.clocks.SpreadUs(i) <= WireClocks::kSpreadWarnUs);
        printf("TRACKMAP real clock sender %08x offset %lld early %lld late %lld\n", scan.clocks.sender[i],
               (long long)scan.clocks.offset_us[i], (long long)scan.clocks.early_us[i],
               (long long)scan.clocks.late_us[i]);
    }
    // Three players and the server, each seen across a chapter boundary: `late` pins the offset
    // (at or above `early`, or short of it by less than one send period) rather than bracketing it.
    REQUIRE(scan.clocks.count == 4);
    CHECK(scan.clocks.senders_overflowed == 0);
    for (size_t i = 0; i < scan.clocks.count; ++i) {
        CHECK(scan.clocks.late_us[i] - scan.clocks.early_us[i] > -30000);
    }

    Window w;
    REQUIRE(w.Reserve(CapsForScan(CapsForCadence(2000000, 33333), scan), &why));  // facts-ok: CLOCK.FIXEDSTEP_30HZ_US the 30 Hz step period restated as this case's own cadence fixture

    // ---- nothing dropped for want of an identity or a clock, over the whole file ---------------
    Totals tot;
    BuildEveryChapter(reader, w, &scan.clocks, tot);
    PrintStream("real", "body_wire", tot.bodyWire);
    PrintStream("real", "body_sampled", tot.bodySampled);
    PrintStream("real", "player_wire", tot.playerWire);
    PrintStream("real", "player_sampled", tot.playerSampled);
    CHECK(tot.bodyWire.read == oracle.bodyWire);
    CHECK(tot.playerWire.read == oracle.playerWire);
    CHECK(tot.bodySampled.read == oracle.bodySampled);
    CHECK(tot.unresolvable == 0);
    const StreamStats* all[] = {&tot.bodyWire, &tot.bodySampled, &tot.playerWire, &tot.playerSampled};
    uint32_t recovered = 0;
    for (const StreamStats* s : all) {
        CHECK(s->dropped_identity == 0);
        CHECK(s->dropped_clock == 0);
        CHECK(s->dropped_window == 0);
        CHECK(s->clock_clamped == 0);
        CHECK(s->read == s->keyed + s->Dropped());
        CHECK(s->dropped_verbatim == 0);
        recovered += s->recovered_verbatim;
    }
    CHECK(recovered == oracle.verbatim);   // every verbatim record the file declares, decoded

    // ---- every resident window: who is in each track -------------------------------------------
    std::set<uint32_t> playerSlots, bodySlots;
    size_t discSamples = 0, windows = 0, mixedBody = 0, mixedPlayer = 0, unknownPose = 0;
    size_t outsideSpan = 0;
    // [wire slot, sampled body] -> summed distance and count, per lag of the sampled copy behind the
    // wire track. Lag 0 is index kZeroLag.
    constexpr int kLagStepUs = 20000, kLags = 21, kZeroLag = 10;
    struct LagSums { double sum[kLags] = {}; size_t n[kLags] = {}; };
    std::map<std::pair<uint32_t, uint32_t>, LagSums> wireVsSampled;
    for (size_t c = 0; c < reader.ChapterCount(); ++c) {
        if (reader.ChapterAt(c).section != kSectionCore) continue;
        BuildStats st;
        REQUIRE_MESSAGE(BuildWindowRange(reader, c, w, st, &scan.clocks), st.refusal);
        ++windows;
        CHECK(w.Overflowed() == 0);

        for (size_t p = 0; p < w.PlayerCount(); ++p) {
            const PlayerTrack& t = w.PlayerAt(p);
            if (t.count) playerSlots.insert(t.user_slot);
            CHECK_FALSE(t.from_sampled);
            for (size_t k = 0; k < t.count; ++k) {
                const auto it = oracle.players.find(PlayerPoseKey(w.PlayerFrames(t)[k]));
                if (it == oracle.players.end()) { ++unknownPose; continue; }
                if (!it->second.count(SourceOf(kWirePlayer, t.user_slot))) ++mixedPlayer;
            }
        }
        for (size_t b = 0; b < w.BodyCount(); ++b) {
            const BodyTrack& t = w.BodyAt(b);
            const IdKind kind = ClassifyBodyIdentity(t.id.fill, t.id.tag);
            if (kind == IdKind::UserIdByTag && t.count) bodySlots.insert((uint32_t)t.id.value & 0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
            if (kind == IdKind::ComponentIdByTag) discSamples += t.count;
            const uint64_t claimed = ClaimedSource(t);
            for (size_t k = 0; k < t.count; ++k) {
                const auto it = oracle.bodies.find(BodyPoseKey(w.BodyFrames(t)[k]));
                if (it == oracle.bodies.end()) { ++unknownPose; continue; }
                if (!it->second.count(claimed)) ++mixedBody;
            }
            // A wire track sits on the capture axis now, inside the window it was filed under.
            if (kind != IdKind::Unresolvable && t.count) {
                const uint64_t slack = 250000;
                if (w.BodyTimes(t)[0] + slack < w.OuterBeginUs() ||
                    w.BodyTimes(t)[t.count - 1] > w.OuterEndUs() + slack) {
                    ++outsideSpan;
                }
            }
        }

        // The recorder sampled the same three bodies off its own pools, on the capture axis. A wire
        // track placed on that axis passes through those samples; one left on its sender's clock, or
        // keyed to the wrong player, does not.
        for (size_t b = 0; b < w.BodyCount(); ++b) {
            BodyTrack& wire = w.BodyAt(b);
            if (ClassifyBodyIdentity(wire.id.fill, wire.id.tag) != IdKind::UserIdByTag) continue;
            for (size_t s = 0; s < w.BodyCount(); ++s) {
                const BodyTrack& sampled = w.BodyAt(s);
                if (sampled.id.fill != 0) continue;
                LagSums& acc = wireVsSampled[{(uint32_t)wire.id.value & 0xFFFFu,  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
                                              (uint32_t)sampled.id.value}];
                for (size_t k = 0; k < sampled.count; ++k) {
                    for (int lag = 0; lag < kLags; ++lag) {
                        const int64_t t = (int64_t)w.BodyTimes(sampled)[k] -
                                          (int64_t)(lag - kZeroLag) * kLagStepUs;
                        if (t < (int64_t)w.BodyTimes(wire)[0] ||
                            t > (int64_t)w.BodyTimes(wire)[wire.count - 1]) {
                            continue;
                        }
                        BodyState at;
                        SeriesCursor cursor;
                        if (!SampleBodyAt(w.BodyFrames(wire), w.BodyTimes(wire), wire.count, cursor, t,
                                          at)) {
                            continue;
                        }
                        acc.sum[lag] += Distance(at.com, w.BodyFrames(sampled)[k].com);
                        ++acc.n[lag];
                    }
                }
            }
        }
    }
    printf("TRACKMAP real windows %zu disc samples %zu mixed body %zu player %zu unknown %zu\n", windows,
           discSamples, mixedBody, mixedPlayer, unknownPose);

    CHECK(playerSlots == std::set<uint32_t>{0, 1, 3});
    CHECK(bodySlots == std::set<uint32_t>{0, 1, 3});
    CHECK(mixedPlayer == 0);
    CHECK(mixedBody == 0);
    CHECK(unknownPose == 0);
    CHECK(discSamples > 0);
    CHECK(outsideSpan == 0);

    // Each wire body coincides with exactly one of the recorder's sampled bodies, to within what the
    // recorder's own interpolation delay moves a player, and is metres from the other two. The lag
    // that fits best is that delay: small and not negative, which a sender left on its own clock --
    // or placed by the wrong sender's offset -- cannot produce.
    for (uint32_t slot : {0u, 1u, 3u}) {
        size_t close = 0;
        for (const auto& kv : wireVsSampled) {
            const LagSums& a = kv.second;
            if (kv.first.first != slot || a.n[kZeroLag] == 0) continue;
            const double mean = a.sum[kZeroLag] / (double)a.n[kZeroLag];
            int best = kZeroLag;
            for (int lag = 0; lag < kLags; ++lag) {
                if (a.n[lag] && a.sum[lag] / (double)a.n[lag] < a.sum[best] / (double)a.n[best]) best = lag;
            }
            const int bestLagMs = (best - kZeroLag) * kLagStepUs / 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
            printf("TRACKMAP real wire slot %u vs sampled body %u: mean %.3f m over %zu, best lag %d ms "
                   "(%.3f m)\n",
                   slot, kv.first.second, mean, a.n[kZeroLag], bestLagMs,
                   a.sum[best] / (double)a.n[best]);
            if (mean < 0.5) {
                ++close;
                // The recorder drew this player behind the sender's own clock by no more than that
                // sender's latency spread plus its render delay and one lag step. Slot 3 is the
                // jittery one: 154 ms of spread, drawn 180 ms behind.
                int spreadMs = 0;
                for (size_t i = 0; i < scan.clocks.count; ++i) {
                    if (scan.clocks.sender[i] == slot) {
                        spreadMs = (int)(scan.clocks.SpreadUs(i) / 1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
                    }
                }
                CHECK(bestLagMs >= -20);
                CHECK(bestLagMs <= spreadMs + 80);
            }
        }
        CHECK(close == 1);
    }
}

TEST_CASE("TRACKMAP: the solo fixture builds the tracks it built before the re-key") {
    DemoReader reader;
    char where[1024];
    const bool present = OpenSoloFixture(reader, where, sizeof where);
    INFO("fixture: ", where);
    if (!FilePresentOrExcused(present, where)) return;

    TrackScan scan;
    const char* why = "";
    REQUIRE_MESSAGE(ScanTrackSources(reader, scan, &why), why);
    CHECK(scan.wire_records == 0);
    CHECK(scan.clocks.count == 0);

    Window w;
    REQUIRE(w.Reserve(CapsForScan(CapsForCadence(12000000, 11111), scan), &why));
    Totals tot;
    BuildEveryChapter(reader, w, &scan.clocks, tot);
    PrintStream("solo", "body_sampled", tot.bodySampled);
    PrintStream("solo", "player_sampled", tot.playerSampled);
    printf("TRACKMAP solo cores %u hash %016llx\n", tot.cores, (unsigned long long)tot.hash);

    // Pinned from the build BEFORE this packet (same loop, same hash, pre-change windowbuild).
    CHECK(tot.cores == 65);
    CHECK(tot.bodySamples == 6482);
    CHECK(tot.playerSamples == 6482);
    CHECK(tot.noNetId == 6482);
    CHECK(tot.unresolvable == 0);
    CHECK(tot.hash == 0x51f40a5c9a4b2e14ull);

    CHECK(tot.bodySampled.read == 6482);
    CHECK(tot.bodySampled.keyed == 6482);
    CHECK(tot.playerSampled.read == 6482);
    CHECK(tot.playerSampled.keyed == 6482);
    CHECK(tot.bodyWire.read == 0);
    CHECK(tot.playerWire.read == 0);

    // The pairing BindBodyHandles rests on: one sampled player on the recorded slot, one body with
    // no net id.
    BuildStats st;
    REQUIRE(BuildWindowRange(reader, ChapterForTime(reader, (int64_t)reader.GetManifest().t0Us + 60000000),
                             w, st, &scan.clocks));
    REQUIRE(w.PlayerCount() == 1);
    CHECK(w.PlayerAt(0).user_slot == 0);
    CHECK(w.PlayerAt(0).from_sampled);
    REQUIRE(w.BodyCount() == 1);
    CHECK(w.BodyAt(0).id.fill == 0);
}

// DIAG-REC-DROPOUT: user 0's head sat on a quaternion tie from ~369 s to ~386 s, so the capture
// stored every one of his player records verbatim and playback used to drop them all -- his hands
// froze while his body and the disc he carried kept moving.
TEST_CASE("TRACKMAP: user 0's hands keep moving through the 9/24 capture's verbatim span") {
    DemoReader reader;
    char where[1024];
    const bool present = OpenTiedHeadCapture(reader, where, sizeof where);
    INFO("capture: ", where);
    if (!FilePresentOrExcused(present, where)) return;

    TrackScan scan;
    const char* why = "";
    REQUIRE_MESSAGE(ScanTrackSources(reader, scan, &why), why);
    Window w;
    REQUIRE(w.Reserve(CapsForScan(CapsForCadence(2000000, 33333), scan), &why));  // facts-ok: CLOCK.FIXEDSTEP_30HZ_US the 30 Hz step period restated as this case's own cadence fixture

    const uint64_t t0 = reader.GetManifest().t0Us;
    const uint64_t lo = t0 + 369000000, hi = t0 + 386500000;
    size_t clock = 0;
    while (clock < scan.clocks.count && scan.clocks.sender[clock] != 0) ++clock;
    REQUIRE(clock < scan.clocks.count);
    const int64_t offsetUs = scan.clocks.offset_us[clock];
    StreamStats playerWire, bodyWire;
    uint32_t spanRecords = 0, spanVerbatim = 0;
    std::vector<std::pair<uint64_t, uint64_t>> hands;   // capture time, hand pose key
    for (size_t c = 0; c < reader.ChapterCount(); ++c) {
        const ChapterKey key = reader.ChapterAt(c);
        if (key.section != kSectionCore) continue;
        BuildStats st;
        REQUIRE_MESSAGE(BuildWindow(reader, c, w, st, &scan.clocks), st.refusal);
        Add(playerWire, st.player_wire);
        Add(bodyWire, st.body_wire);
        if (key.tEndUs <= lo || key.tBeginUs >= hi) continue;

        // What the file holds for user 0 in the span; the build before this fix keyed only the
        // records that are not verbatim.
        DecodedChapter dc;
        REQUIRE(reader.DecodeChapter(c, dc));
        if (const StreamView* sv = dc.Find(kStreamPlayerWire)) {
            bitio::BitReader r(sv->records + capture::kBlockPrefixBytes,
                               sv->bytes - capture::kBlockPrefixBytes);
            capture::PlayerDecoder pd(false);
            uint8_t scratch[512];
            for (uint32_t k = 0; k < sv->recordCount; ++k) {
                capture::DecodedPlayer d;
                memset(&d, 0, sizeof d);
                REQUIRE(pd.Decode(r, d, scratch, sizeof scratch));
                uint64_t hdr0 = d.frame.hdr0;
                if (d.verbatim) memcpy(&hdr0, d.body, sizeof hdr0);
                if ((uint32_t)hdr0 != 0) continue;
                const int64_t at = (int64_t)d.stampUs - offsetUs;
                if (at < (int64_t)lo || at >= (int64_t)hi) continue;
                ++spanRecords;
                spanVerbatim += d.verbatim;
            }
        }
        for (size_t p = 0; p < w.PlayerCount(); ++p) {
            const PlayerTrack& t = w.PlayerAt(p);
            if (t.user_slot != 0 || t.from_sampled) continue;
            for (size_t k = 0; k < t.count; ++k) {
                const uint64_t at = w.PlayerTimes(t)[k];
                if (at < lo || at >= hi) continue;
                const PlayerState& s = w.PlayerFrames(t)[k];
                uint64_t h = Fnv(kFnvSeed, s.lHandOri, sizeof s.lHandOri);
                h = Fnv(h, s.rHandOri, sizeof s.rHandOri);
                h = Fnv(h, s.lHandOff, sizeof s.lHandOff);
                h = Fnv(h, s.rHandOff, sizeof s.rHandOff);
                hands.emplace_back(at, h);
            }
        }
    }
    std::sort(hands.begin(), hands.end());
    size_t changes = 0;
    uint64_t worstGapUs = hands.empty() ? hi - lo : hands.front().first - lo;
    for (size_t i = 1; i < hands.size(); ++i) {
        changes += hands[i].second != hands[i - 1].second;
        worstGapUs = std::max(worstGapUs, hands[i].first - hands[i - 1].first);
    }
    if (!hands.empty()) worstGapUs = std::max(worstGapUs, hi - hands.back().first);
    PrintStream("tied", "player_wire", playerWire);
    PrintStream("tied", "body_wire", bodyWire);
    printf("TRACKMAP tied user 0 369-386.5 s: records %u, verbatim %u (kept before: %u) -> samples %zu, hand "
           "changes %zu, worst gap %.3f s\n",
           spanRecords, spanVerbatim, spanRecords - spanVerbatim, hands.size(), changes, double(worstGapUs) / 1e6);

    REQUIRE(spanVerbatim > spanRecords / 2);   // the subject: this span really is the verbatim one
    CHECK(hands.size() == spanRecords);
    CHECK(playerWire.dropped_verbatim == 0);
    CHECK(bodyWire.dropped_verbatim == 0);
    CHECK(playerWire.recovered_verbatim > 0);
    CHECK(hands.size() * 10 >= (hi - lo) / 1000000 * 45 * 9);   // 90 % of the 45 Hz send rate
    CHECK(changes * 10 >= hands.size() * 9);
    CHECK(worstGapUs < 250000);
}

