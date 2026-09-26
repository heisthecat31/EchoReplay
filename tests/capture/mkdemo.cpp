// Writes a scripted capture through the REAL ChapterBuilder and the REAL container, so the file under
// test is produced by the shipping path rather than by a fixture that agrees with it.
//
// ---------------------------------------------------------------------------------------------
// ★ THE MUTATION LEVER, AND WHY IT IS THE SAMPLER AND NOT A COUNTER.
//
// S5.6.2 names its own mutation: "drop every second game_timer sample BY HALVING THE CAPTURE'S SAMPLE
// RATE, not by decrementing a counter", and records that under the old design both `echodemo verify`
// and O1 stayed green because the writer counted its own reduced sample set as complete.
//
// --sample-divisor N therefore skips the GATHER on frames where the ENGINE's frame index is not a
// multiple of N. The frame counter still advances by one per frame, because it is the engine's and not
// ours. Nothing else changes: no counter is touched, no observed value is edited. If the accounting is
// engine-sourced the file records the loss and `echodemo verify` refuses it; if it were sourced from
// our own loop the file would be certified clean, which is exactly the artefact this lever produces on
// demand so a gate can be shown to catch it.
// ---------------------------------------------------------------------------------------------

#include <windows.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <cmath>
#include <string>
#include <vector>

#include "capture/capture_types.h"
#include <stdlib.h>

#include "capture/chapterbuilder.h"
#include "capture/gather.h"
#include "capture/keyframe.h"
#include "capture/playerstats.h"
#include "events/lobbytrack.h"

#include "../events/fixtures.h"
#include "capture/manifestbuild.h"
#include "capture/streamcodec.h"
#include "capture/wirebuild.h"
#include "foundation/bitio/enginewire.h"
#include "foundation/bitio/wirefields.h"
#include "foundation/demofile/demofile.h"

using namespace replay;
using namespace replay::capture;

namespace {

// One scoreboard record every third of a second at this fixture's 90 Hz, taking the users in turn,
// so each user is sampled once a second -- the cadence kStatsRescanIntervalUs gives a real capture.
// The users are STAGGERED rather than emitted as a group of three because a group makes the
// per-chapter count move in steps of three, and echodemo verify's rate heuristic (which needs a
// count delta of 2 before it will call a step real) then fires on chapter lengths alone. Staggering
// is also the truer picture: SweepPlayerStats writes only the entrants whose values moved.
constexpr uint32_t kMkStatsPeriodFrames = 30;

// The scoreboard users. Three rather than all of them because `PLAYERSTATS n record(s), u user(s)`
// with u below the manifest's player count is the only shape that shows the user tally is counted
// from the records and not copied from the roster.
constexpr uint32_t kMkStatsUsers = 3;

// AUD-FINAL R9's shape, at the fixture level. The 2026-09-07 field captures collapse from ~71 Hz to
// 30.0 Hz partway through and carry S_DEGRADED records saying the S6.4 ladder did it. The collapse is
// modelled as a THIRD of the tick rate in wall-clock time, not as a skipped gather: a shed that skips
// frames also opens a frame-index gap, and the field captures show no gap. So the sampler keeps
// running on every frame it sees; it just sees a third as many per second.
constexpr uint32_t kShedFactor = 3;
// ~5.6 oracle records per 2 s chapter before the shed: enough that its own per-chapter count moves by
// more than the rate heuristic's two-record floor when the cadence collapses.
constexpr uint32_t kMkOraclePeriodFrames = 32;

struct Options {
    const wchar_t* out = nullptr;
    // 80 s at 90 Hz. NOT taste: the launch runbook regenerates F-SYNTH by running this tool with NO
    // --frames (L3-RUN.md s.1 row 9), so this default IS the playback fixture's length. At 900
    // frames it was 9.999 s -- shorter than the boot it has to outlive, and far shorter than the
    // "scripted 60 s replay" The design make the playback acceptance. A runner who forgets
    // a flag must not be able to produce a 10 s fixture; tests/replay/check.ps1 asserts this default
    // times frameIntervalUs is at least that 60 s floor.
    //
    // 120 s, which is what the L3 arithmetic asked for. It was 7200 (80 s) for one release because
    // --frames=10800 reported `MKDEMO-ORIGIN ... BROKEN` / `status=FAIL`, and AUDIT_M9 F5 established
    // that the 64 in that verdict was THIS FILE's audit array, not a cap in the writer:
    // chapterbuilder.cpp:559 emits KB_WORLD_ORIGIN once per keyframe with no guard, and
    // `echodemo verify` passed the same bytes. The array is gone (see KeyframeAudit below) and the
    // default is back where the fixture needs it.
    uint32_t frames = 10800;
    uint32_t players = 8;
    uint32_t sampleDivisor = 1;    // the S5.6.2 mutation lever
    // A SECOND MUTATION LEVER, and it exists to test a CHECK rather than the capture.
    //
    // check.ps1 takes one named exemption: echodemo verify's rate-step heuristic measures a sampler
    // and fires on any event stream whose chapters differ in event count, so a FAIL matching
    // "S_MARK/S_EVENT rate steps" is not treated as a refusal. An exemption whose replacement check is
    // untested is a hole with a comment on it. This lever plants a REAL completeness deficit on
    // S_MARK -- markers the builder derived and then lost downstream, which is E-C10's own shape --
    // so the gate can assert the replacement check goes red on it WHILE the rate-step FAIL on the very
    // same stream is still being exempted. Keeping those two apart is the whole job of the exemption.
    uint32_t dropMarks = 0;
    // A3's lever. The frame set still reports the player as PRESENT and the sample is not stored --
    // which is what a gather does when ReadBody returns false, or when any per-entity read fails. It
    // is NOT the same as the sampler stopping: the tick still runs, the frame index still advances by
    // one, so the frame-skip detector sees nothing wrong. Before presence was reported separately this
    // produced no record and no observation, and the audit got bit-identical output for it and for a
    // player who simply was not there.
    uint32_t skipSample = 0;
    uint32_t frameIntervalUs = 11111;
    uint64_t t0Us = 1700000000000000ull;
    // ★ Two motion models, and the difference between them is the honest range of what this file can
    // say about size. `smooth` walks the quantisation indices a few steps a frame, which is what a body
    // in flight does; `chaos` jumps them across the domain every frame, which is the worst case delta
    // coding can meet. Neither is a measurement of a real match -- Appendix D.2 is explicit that no
    // corpus on this machine can supply one -- and reporting both is what stops either being read as
    // the shipping number.
    bool chaos = false;
    // R9's lever. From the moment this many chapters have been flushed, the sampler's cadence
    // collapses by kShedFactor and the builder is told so through NoteDegraded -- the same call the
    // shed ladder makes. `verify` must score the resulting rate step as NOT-RUN-DEGRADED, not FAIL.
    uint32_t shedAtChapter = 0;
    // The same collapse with NOTHING declared: a capture that really did slow down and never said so.
    // Without this fixture the new verdict would be an unconditional amnesty and nothing could tell.
    bool shedUndeclared = false;
    // S_ORACLE records, so a file can carry oracle content and the HAS_ORACLE flag can disagree with
    // it. Off by default: the neverProduced audit below expects this stream to declare itself silent.
    bool oracle = false;
    // The file the two field captures are: the process went away with it still open, so the close path
    // never ran and the header's HAS_* flags were never stamped from the content.
    bool leaveOpen = false;
};

// Set by main from Options::chaos. A global rather than a parameter because the two fill functions are
// called from four places and threading it through would say nothing extra.
bool g_chaos = false;

// A deterministic value generator. A PRNG would make the file's compressibility a property of the PRNG;
// this walks the quantisation grids so the deltas look like motion rather than like noise.
struct Motion {
    uint64_t k = 0;
    uint64_t Next(uint64_t mod) {
        k = k * 6364136223846793005ull + 1442695040888963407ull;
        return mod ? (k >> 33) % mod : 0;
    }
};

// The smallest-three codec is defined on the unit sphere: it rebuilds the dropped component from a
// square root, so an unnormalised quaternion is refused rather than approximated.
void Normalise(float q[4]) {
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 0.0f) {
        for (int i = 0; i < 4; ++i) q[i] /= n;
    }
}

float OnGrid(const bitio::WireField& f, uint64_t index) {
    return bitio::DequantReal(index, f.min, f.step);
}
uint64_t MaxIndex(const bitio::WireField& f) {
    return bitio::RealStepsWriter(f.min, f.max, f.step);
}

// A body that walks smoothly, which is what delta coding is supposed to exploit. The indices move by a
// few steps a frame, exactly as a real body does at 44 Hz.
void FillBody(BodyFrame& f, uint32_t entity, uint32_t frame) {
    const bitio::WireField* W = bitio::kSimStateWire;
    const uint64_t comMax = MaxIndex(W[2]);
    const uint64_t velMax = MaxIndex(W[4]);
    const uint64_t angMax = MaxIndex(W[5]);
    const uint64_t step = g_chaos ? 7919ull : 4ull;
    const uint64_t base = uint64_t(entity) * (g_chaos ? 7919ull : 97ull) + uint64_t(frame) * step;
    for (int i = 0; i < 3; ++i) {
        f.com[i] = OnGrid(W[2], (comMax / 2 + base * 3 + uint64_t(i) * 11) % (comMax + 1));
        f.vel[i] = OnGrid(W[4], (velMax / 2 + base + uint64_t(i)) % (velMax + 1));
        f.angVel[i] = OnGrid(W[5], (angMax / 2 + base / 4 + uint64_t(i)) % (angMax + 1));
        f.lastHeldCom[i] = OnGrid(W[6], (comMax / 2 + uint64_t(entity) * 13) % (comMax + 1));
    }
    float q[4] = {0.1f + 0.001f * float(frame % 100), 0.2f, 0.3f, 0.9f};
    Normalise(q);
    bitio::Quat10 packed{};
    bitio::QuantQuat(q, W[3].bits, packed);
    bitio::DequantQuat(packed, W[3].bits, f.ori);
    f.bits11 = entity % 2048;
    f.idValue = 0x0000000100000000ull | entity;
    f.idTag = 1;
    f.fill = kFillBodyWireAll;
}

void FillPlayer(PlayerFrame& f, uint32_t entity, uint32_t frame) {
    const bitio::WireField* W = bitio::kPlayerStateWire;
    const uint64_t velMax = MaxIndex(W[3]);
    const uint64_t offMax = MaxIndex(W[12]);
    const uint64_t base = uint64_t(entity) * (g_chaos ? 104729ull : 131ull) +
                          uint64_t(frame) * (g_chaos ? 6151ull : 3ull);
    for (int i = 0; i < 3; ++i) {
        f.refVel[i] = OnGrid(W[3], (velMax / 2 + base + uint64_t(i)) % (velMax + 1));
        f.refVelDup[i] = f.refVel[i];
        f.lHandOff[i] = OnGrid(W[12], (offMax / 2 + base * 2 + uint64_t(i) * 3) % (offMax + 1));
        f.rHandOff[i] = OnGrid(W[13], (offMax / 2 + base * 3 + uint64_t(i) * 5) % (offMax + 1));
    }
    float qs[3][4] = {{0.1f + 0.0007f * float(frame % 200), 0.2f, 0.3f, 0.9f},
                      {0.4f, 0.1f, 0.2f + 0.0005f * float(frame % 300), 0.8f},
                      {-0.3f, 0.5f, 0.1f, 0.7f}};
    float* dst[3] = {f.hmdOri, f.lHandOri, f.rHandOri};
    for (int i = 0; i < 3; ++i) {
        Normalise(qs[i]);
        bitio::Quat10 packed{};
        bitio::QuantQuat(qs[i], W[i].bits, packed);
        bitio::DequantQuat(packed, W[i].bits, dst[i]);
    }
    f.bendBack = OnGrid(W[9], (base / 8) % (MaxIndex(W[9]) + 1));
    f.bendFwd = OnGrid(W[10], (base / 9) % (MaxIndex(W[10]) + 1));
    f.restPoseHeight = OnGrid(W[11], (base / 11) % (MaxIndex(W[11]) + 1));
    f.trigL = OnGrid(W[14], (frame / 30) % (MaxIndex(W[14]) + 1));
    f.trigR = OnGrid(W[15], (frame / 40) % (MaxIndex(W[15]) + 1));
    // The 28-bit action word changes on about 1% of frames, as R4 measured. It is stored raw and never
    // decoded: EmitChangedDelegates diffs the word, so bit-identical is the whole requirement.
    f.bits28 = (frame / 100) * 0x00010001u & 0x0FFFFFFFu;
    f.holdLSlot = entity;
    f.holdLGen = 1;
    f.holdRSlot = 0xFFFFu;
    f.holdRGen = 0;
    f.anchor = entity % 8;
    f.hasDup = true;
}

// ARCH-21 S4.3's producer half, offline. The shipped sampler reads the engine's variable store, so
// this stands in for it AT THE IGameState SEAM: same interface, same postcondition -- Sample()
// zeroes its out-parameter and leaves engineTimeUs/captureTimeUs to its caller, exactly as
// gamestate.cpp does -- and everything downstream of the seam is the shipped code. The walk is the
// one a match takes, pre_match -> (round_start, playing, score, round_over) x3 -> post_match, and
// the file's round table is DERIVED from these edges by the real LocalMarkerSource. It used to be a
// literal three-row table filed before a single record existed, which is a table that cannot
// disagree with the capture it describes.
constexpr uint32_t kMkRounds = 3;
constexpr uint32_t kMkPhasesPerRound = 4;  // round_start, playing, score, round_over
constexpr uint32_t kMkSegments = kMkRounds * kMkPhasesPerRound + 2;  // + pre_match and post_match
// One S_GAMESTATE copy a second at this fixture's 90 Hz, on top of every change -- capture.cpp's
// "on change plus a periodic copy".
constexpr uint32_t kMkGameStatePeriodFrames = 90;
// Distinct, non-zero, and derived rather than hatched, the same way test_localmarkers derives its
// fake CSymbol64s: only distinctness and non-zeroness are load-bearing, and a round hex literal
// beside a real symbol table invites someone to read it as one.
constexpr uint64_t kMkSymSeed = uint64_t(kRoundNumberNone) << 4;

class ScriptedGameState : public gamestate::IGameState {
public:
    ScriptedGameState(uint32_t frames, uint32_t frameIntervalUs)
        : segFrames_(frames / kMkSegments ? frames / kMkSegments : 1),
          frameIntervalUs_(frameIntervalUs) {}

    void SetFrame(uint32_t frame) { frame_ = frame; }

    gamestate::Availability Sample(gamestate::GameStateSample& out) override {
        out = gamestate::GameStateSample{};
        uint32_t seg = frame_ / segFrames_;
        if (seg >= kMkSegments) seg = kMkSegments - 1;
        const bool inRound = seg > 0 && seg <= kMkRounds * kMkPhasesPerRound;
        const uint32_t round = inRound ? (seg - 1) / kMkPhasesPerRound + 1 : 0;
        const uint32_t step = inRound ? (seg - 1) % kMkPhasesPerRound : 0;
        const bool scored = inRound && step >= 2;  // score and round_over carry the round's points

        gamestate::Phase phase = gamestate::Phase::PreMatch;
        if (inRound) {
            const gamestate::Phase steps[kMkPhasesPerRound] = {
                gamestate::Phase::RoundStart, gamestate::Phase::Playing, gamestate::Phase::Score,
                gamestate::Phase::RoundOver};
            phase = steps[step];
        } else if (seg > 0) {
            phase = gamestate::Phase::PostMatch;
        }

        // Blue takes 2 points a round and red 1, so no two rows of the table are alike and a reader
        // that reordered or duplicated one has nowhere to hide. The match score is the running sum of
        // the rounds already scored, which is what makes it a different number from the round points.
        const uint32_t done = inRound ? round - 1 : (seg > 0 ? kMkRounds : 0);
        out.blueMatchScore = done * (done + 1);
        out.redMatchScore = done * (done + 1) / 2;
        if (scored) {
            out.bluePoints = round * 2;
            out.redPoints = round;
            out.blueMatchScore += round * 2;
            out.redMatchScore += round;
            out.lastScoreTeam = 0;
            out.lastScorePoints = 2;
        }
        out.roundCounter = round;
        out.roundWinningTeam = (inRound && step == kMkPhasesPerRound - 1) ? 0 : -1;
        out.frisbeeOwnerId = gamestate::kFrisbeeUnowned;
        out.phase = static_cast<uint8_t>(phase);
        out.phaseSymbol = kMkSymSeed + static_cast<uint64_t>(phase);
        out.pausedSymbol = kMkSymSeed;  // never a phase symbol: no Phase has the value 0

        // Counts DOWN over the round the way the engine's does, so a reader that took it for an
        // ascending axis says so. Zero outside a round, where the engine has no round clock.
        if (inRound) {
            const uint32_t roundEnd = (round * kMkPhasesPerRound + 1) * segFrames_;
            const uint32_t left = roundEnd > frame_ ? roundEnd - frame_ : 0;
            out.gameTimer = float(double(left) * double(frameIntervalUs_) / 1e6);
        }
        if (inRound && step == 0) {
            const uint32_t segEnd = (seg + 1) * segFrames_;
            const uint32_t left = segEnd > frame_ ? segEnd - frame_ : 0;
            out.countdownTimer = float(double(left) * double(frameIntervalUs_) / 1e6);
        }
        out.present = gamestate::kHasPhaseSymbol | gamestate::kHasPausedSymbol |
                      gamestate::kHasRoundCounter | gamestate::kHasRoundPoints |
                      gamestate::kHasMatchScore | gamestate::kHasFrisbeeOwnerId |
                      gamestate::kHasGameTimer | gamestate::kHasCountdownTimer |
                      gamestate::kHasFirstLaunchOfRound | gamestate::kHasRoundWinningTeam |
                      gamestate::kHasLastScore;
        out.availability = static_cast<uint8_t>(gamestate::Availability::Ok);
        ++stats_.samples;
        return gamestate::Availability::Ok;
    }

    gamestate::Availability State() const override { return gamestate::Availability::Ok; }
    const gamestate::Stats& GetStats() const override { return stats_; }

private:
    uint32_t segFrames_;
    uint32_t frameIntervalUs_;
    uint32_t frame_ = 0;
    gamestate::Stats stats_ = {};
};

// capture.cpp's change test, minus the clocks and the two timers: a record rewritten whenever
// game_timer moved would make S_GAMESTATE a verbatim copy of the tick stream.
bool GameStateScriptChanged(const gamestate::GameStateSample& a,
                            const gamestate::GameStateSample& b) {
    return a.phase != b.phase || a.phaseSymbol != b.phaseSymbol ||
           a.roundCounter != b.roundCounter || a.bluePoints != b.bluePoints ||
           a.redPoints != b.redPoints || a.blueMatchScore != b.blueMatchScore ||
           a.redMatchScore != b.redMatchScore || a.roundWinningTeam != b.roundWinningTeam;
}

// The ring is not exercised here: mkdemo drives ChapterBuilder directly so the file it produces is a
// property of the encoder and the container alone. The ring has its own check in capture_selftest, and
// mixing the two would make a file-level failure ambiguous between them.
struct RecordBuf {
    std::vector<uint8_t> bytes;
    void Begin(uint16_t kind) {
        bytes.clear();
        bytes.resize(sizeof(RecHeader));
        RecHeader h{};
        h.kind = kind;
        memcpy(bytes.data(), &h, sizeof h);
    }
    void Add(const void* p, size_t n) {
        const size_t at = bytes.size();
        bytes.resize(at + n);
        memcpy(bytes.data() + at, p, n);
    }
    void Finish() {
        RecHeader h{};
        memcpy(&h, bytes.data(), sizeof h);
        h.bytes = static_cast<uint16_t>(bytes.size() - sizeof(RecHeader));
        memcpy(bytes.data(), &h, sizeof h);
    }
};


// Re-opens the file just written and walks every S_KEY record as a BLOCK TABLE. This is the
// end-to-end half of the framing change: the unit tests prove the reader handles a block from
// tomorrow, and this proves the bytes a real capture puts on disk are actually block-framed rather
// than a struct that happens to parse.
//
// It reports UNKNOWN blocks as well as known ones, because that count is what a future player will
// use to say "this file carries pins I cannot read" instead of seeking to a moment that quietly
// sounds wrong. Today it must be zero: nothing writes a reserved id yet.
struct KeyframeAudit {
    uint32_t records = 0;
    uint32_t refused = 0;
    uint32_t inconsistent = 0;
    // Streams the file DECLARES it never produced anything on, by id.
    uint32_t originBlocks = 0;
    uint32_t originValid = 0;
    uint32_t originSuspect = 0;
    // These were a `float originX[64]` the parse loop truncated into, so a file with more
    // than 64 keyframes lost every block past the 64th, `originBlocks == records` was false for any
    // capture longer than ~90 s, and the tool printed "75 keyframes but only 64 carry the block" about
    // bytes that carried all 75. The check needs two TALLIES over the recorded x, not the values, so
    // it keeps the tallies and no array can bound the file.
    uint32_t originZeroX = 0;       // x recorded as exactly 0 -- the pre-shift frames
    uint32_t originShiftedX = 0;    // x recorded as the fixture's -128.5 -- the post-shift frames
    bool neverProduced[32] = {};
    uint32_t neverProducedCount = 0;
    bool wireSlotSeen[kMaxEntities] = {};
    uint64_t wireSlotHdr[kMaxEntities] = {};
    uint32_t wireSlotRebinds = 0;     // A4: a wire slot that changed which header word it denotes
    uint32_t wireSlotBindings = 0;    // slot/header pairs read out of keyframes, so 0 rebinds means something
    uint32_t rosterEntrants = 0;      // from the LAST keyframe: the roster the file actually carries
    uint32_t rosterBlocks = 0;        // keyframes that carried a kKeyBlockRoster at all
    uint32_t rosterNoTeam = 0;        // entrants whose team is 0xFFFF, the value a u8 field destroys
    uint32_t rosterMaxNameLen = 0;
    uint32_t unknownBlocks = 0;
    uint32_t blockSeen[64] = {};
    uint32_t regRefused = 0;      // TAP_REGISTRATION blocks that did not close their identities
    uint32_t regRegistered = 0, regTotal = 0, regTableRows = 0;
};

bool AuditKeyframes(const wchar_t* path, KeyframeAudit& a) {
    DemoReader r;
    if (!r.Open(path)) {
        return false;
    }
    std::vector<uint8_t> body(64 * 1024);
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        DecodedChapter c{};
        if (!r.DecodeChapter(i, c)) {
            continue;
        }
        if (const StreamView* dv = c.Find(kStreamDegraded)) {
            const size_t n = dv->bytes / sizeof(DegradedRecordDisk);
            for (size_t j = 0; j < n; ++j) {
                DegradedRecordDisk d{};
                memcpy(&d, dv->records + j * sizeof d, sizeof d);
                if (d.reason == kDegradeStreamNeverProduced && d.what < 32 &&
                    !a.neverProduced[d.what]) {
                    a.neverProduced[d.what] = true;
                    ++a.neverProducedCount;
                }
            }
        }

        const StreamView* v = c.Find(kStreamKey);
        if (!v || v->bytes < kBlockPrefixBytes) {
            continue;
        }
        bitio::BitReader br(v->records + kBlockPrefixBytes, v->bytes - kBlockPrefixBytes);
        for (uint32_t k = 0; k < v->storedCount; ++k) {
            uint16_t n = 0;
            if (!DecodeRawRecord(br, body.data(), body.size(), n)) {
                ++a.refused;
                break;
            }
            ++a.records;
            KeyframeReader kr;
            if (!kr.Open(body.data(), n)) {
                ++a.refused;
                continue;
            }
            uint16_t id = 0;
            const uint8_t* data = nullptr;
            uint32_t len = 0;
            while (kr.Next(id, data, len)) {
                if (id < 64) {
                    ++a.blockSeen[id];
                }
                if (id >= kKeyBlockFirstReserved) {
                    kr.NoteUnknown(len);
                }
                if (id == kKeyBlockTapRegistration) {
                    // The DENOMINATOR: how many of the capture's own surfaces were live
                    // when this keyframe was written. Without it "S_EVENT holds 0 records" cannot be
                    // told from "the tap was never registered" by reading the file.
                    TapRegistrationBlock rb{};
                    if (ReadTapRegistrationBlock(data, len, rb)) {
                        a.regRegistered = rb.registered;
                        a.regTotal = rb.total;
                        a.regTableRows = rb.tableRows;
                    } else {
                        ++a.regRefused;
                    }
                }
                if (id == kKeyBlockWorldOrigin) {
                    if (len >= 14) {
                        float xyz[3];
                        memcpy(xyz, data, sizeof xyz);
                        uint8_t valid = data[12], suspect = data[13];
                        if (xyz[0] == 0.0f) ++a.originZeroX;
                        if (xyz[0] == -128.5f) ++a.originShiftedX;
                        ++a.originBlocks;
                        a.originValid += valid ? 1u : 0u;
                        a.originSuspect += suspect ? 1u : 0u;
                    }
                }
                if (id == kKeyBlockPlayerWire) {
                    // A4, read the way a reader must: a wire slot is bound to a person only through
                    // the header word the keyframe carries beside it. If slot k denotes one header
                    // word in one keyframe and a different one in the next, no track survives a
                    // chapter boundary and the file cannot say who anybody is.
                    size_t at = 0;
                    uint16_t n = 0;
                    memcpy(&n, data + at, 2);
                    at += 2;
                    for (uint16_t e = 0; e < n; ++e) {
                        if (at + 2 + 8 + sizeof(PlayerFrame) > len) break;
                        uint16_t slot = 0;
                        memcpy(&slot, data + at, 2);
                        at += 2 + 8;
                        PlayerFrame pf{};
                        memcpy(&pf, data + at, sizeof pf);
                        at += sizeof pf;
                        if (slot >= kMaxEntities) continue;
                        ++a.wireSlotBindings;
                        if (a.wireSlotSeen[slot] && a.wireSlotHdr[slot] != pf.hdr0) {
                            ++a.wireSlotRebinds;
                        }
                        a.wireSlotSeen[slot] = true;
                        a.wireSlotHdr[slot] = pf.hdr0;
                    }
                }
                if (id == kKeyBlockRoster) {
                    // Walked back the way a reader with no access to this build's source would: from
                    // the bytes, with every field's width taken from the block's own layout.
                    ++a.rosterBlocks;
                    size_t at = 0;
                    uint16_t entrants = 0;
                    memcpy(&entrants, data + at, 2);
                    at += 2;
                    a.rosterEntrants = entrants;
                    a.rosterNoTeam = 0;
                    a.rosterMaxNameLen = 0;
                    for (uint16_t e = 0; e < entrants && at + 44 <= len; ++e) {
                        at += 40;               // platformCode, accountId, sessionId, firstSeen, lastSeen
                        at += 4;                // entrantIndex, generation
                        uint16_t team = 0;
                        memcpy(&team, data + at, 2);
                        at += 4;                // team, marker
                        at += 2;                // active, hasProfileJson
                        uint16_t nameLen = 0;
                        memcpy(&nameLen, data + at, 2);
                        at += 2 + nameLen;
                        uint16_t platLen = 0;
                        if (at + 2 > len) break;
                        memcpy(&platLen, data + at, 2);
                        at += 2 + platLen;
                        if (team == 0xFFFFu) {
                            ++a.rosterNoTeam;
                        }
                        if (nameLen > a.rosterMaxNameLen) {
                            a.rosterMaxNameLen = nameLen;
                        }
                    }
                    if (at != len) {
                        ++a.inconsistent;   // the block did not end where its own fields said it does
                    }
                }
            }
            if (!kr.Consistent()) {
                ++a.inconsistent;
            }
            a.unknownBlocks += kr.UnknownBlocks();
        }
    }
    return true;
}


// THE BODY POOL, driven through the REAL GatherBodies. Before AUDIT_FINAL_E2E B1 this fixture fed
// no body records at all and listed S_BODY_SAMPLED as expected-never-produced, so the one stream
// carrying a player's world position was a fixture that could not fail: it certified its own
// absence. Four bodies, one of them locally controlled with no net id -- the shape a solo host
// actually has -- so the id-fill arms are both exercised.
constexpr uint16_t kFixtureBodies = 4;

struct FixtureBodyPool {
    uint32_t frame = 0;

    bool Available() const { return true; }
    size_t PhysCount() const { return kFixtureBodies; }
    // Body 0 is the recorder's own. The rest are net-driven, and NOTE that the gather no longer
    // filters on this: it walks the pool and records the answer.
    bool IsLocallyControlled(replay::PhysHandle h) const { return h.v == 0; }
    uint64_t NetIdOf(replay::PhysHandle h) const {
        return h.v == 0 ? 0ull : 0x9000ull + h.v;   // 0 is the honest answer for a purely local body
    }
    // WHICH SOURCE answered, stated by the fixture because the gather buckets on it. A fake that
    // said NotAsked here would drive every source counter to zero and make the assertions on them
    // pass on a gather that never read the field.
    bool ReadBodyDetailed(replay::PhysHandle h, replay::BodyState& out,
                          replay::BodyReading& r) const {
        r = replay::BodyReading{};
        if (!ReadBody(h, out)) {
            r.source = replay::BodySource::None;
            return false;
        }
        r.ciResolved = true;
        r.bodyResolved = h.v == 0;
        r.historyPresent = h.v != 0;
        r.source = h.v == 0 ? replay::BodySource::Local : replay::BodySource::History;
        return true;
    }
    bool ReadBody(replay::PhysHandle h, replay::BodyState& out) const {
        if (h.v >= kFixtureBodies) return false;
        out = replay::BodyState{};
        // MOTION, derived from the frame so a still pool cannot pass: each body travels on its own
        // line at its own rate, and no component is ever zero for every body at once.
        const float t = float(frame) * 0.0083f;
        const float k = float(h.v + 1);
        out.com[0] = 1.5f + k * t;
        out.com[1] = 2.5f - 0.5f * k * t;
        out.com[2] = -3.25f + 0.25f * k * t;
        out.ori[0] = 0.0f; out.ori[1] = 0.0f; out.ori[2] = 0.0f; out.ori[3] = 1.0f;
        out.vel[0] = k;
        out.vel[1] = -0.5f * k;
        out.vel[2] = 0.25f * k;
        out.angVel[0] = 0.125f * k;
        out.angVel[1] = 0.25f * k;
        out.angVel[2] = -0.125f * k;
        out.lastHeldCom[0] = out.com[0];
        out.lastHeldCom[1] = out.com[1];
        out.lastHeldCom[2] = out.com[2];
        out.bits11 = 0x11u * (h.v + 1u);
        return true;
    }
};

// The gather's sink, framed into a ring record exactly as the capture tick frames it, and handed
// straight to the real ChapterBuilder.
bool FixtureGatherSink(uint16_t kind, const void* head, uint32_t headBytes, const void* tail,
                       uint32_t tailBytes, void* ctx) {
    auto* b = static_cast<replay::capture::ChapterBuilder*>(ctx);
    static uint8_t buf[4096];
    const uint32_t payload = headBytes + tailBytes;
    if (sizeof(replay::capture::RecHeader) + payload > sizeof buf) return false;
    replay::capture::RecHeader h;
    h.kind = kind;
    h.bytes = static_cast<uint16_t>(payload);
    h.producer = 0;
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, head, headBytes);
    if (tailBytes) memcpy(buf + sizeof h + headBytes, tail, tailBytes);
    b->Consume(buf, static_cast<uint32_t>(sizeof h + payload));
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        if (wcsncmp(argv[i], L"--frames=", 9) == 0) {
            opt.frames = static_cast<uint32_t>(_wtoi(argv[i] + 9));
        } else if (wcsncmp(argv[i], L"--players=", 10) == 0) {
            opt.players = static_cast<uint32_t>(_wtoi(argv[i] + 10));
        } else if (wcsncmp(argv[i], L"--skip-sample=", 14) == 0) {
            opt.skipSample = static_cast<uint32_t>(_wtoi(argv[i] + 14));
        } else if (wcsncmp(argv[i], L"--drop-marks=", 13) == 0) {
            opt.dropMarks = static_cast<uint32_t>(_wtoi(argv[i] + 13));
        } else if (wcscmp(argv[i], L"--chaos") == 0) {
            opt.chaos = true;
        } else if (wcsncmp(argv[i], L"--sample-divisor=", 17) == 0) {
            opt.sampleDivisor = static_cast<uint32_t>(_wtoi(argv[i] + 17));
        } else if (wcsncmp(argv[i], L"--shed-at=", 10) == 0) {
            opt.shedAtChapter = static_cast<uint32_t>(_wtoi(argv[i] + 10));
        } else if (wcscmp(argv[i], L"--shed-undeclared") == 0) {
            opt.shedUndeclared = true;
        } else if (wcscmp(argv[i], L"--oracle") == 0) {
            opt.oracle = true;
        } else if (wcscmp(argv[i], L"--leave-open") == 0) {
            opt.leaveOpen = true;
        } else if (argv[i][0] != L'-') {
            opt.out = argv[i];
        }
    }
    if (!opt.out || opt.sampleDivisor == 0) {
        printf("capture_mkdemo <out.echodemo> [--frames=N] [--players=N] [--sample-divisor=N]"
               " [--chaos] [--drop-marks=N] [--skip-sample=N] [--shed-at=CHAPTER]"
               " [--shed-undeclared] [--oracle] [--leave-open]\n");
        return 2;
    }
    if (opt.players > kMaxEntities) opt.players = kMaxEntities;
    g_chaos = opt.chaos;

    ManifestBuilder mb;
    mb.Begin(opt.t0Us, 0, 0x40);
    const uint8_t md5[16] = {0xca, 0xa0, 0x5e, 0xe3};
    mb.SetBuild(md5, 0x676F6C646D617374ull, 0x0102030405060708ull);
    mb.SetLevel(6300205991959903307ull, 0xF00Dull, 1780177787ull);
    mb.SetKeyframeIntervalUs(2000000);
    mb.SetUuid(0x1122334455667788ull, 0x99AABBCCDDEEFF00ull);
    // A v1 ROSTER, so `echodemo info`/`verify` exercise the fields a capture now writes rather than
    // only the B.0 half. The last player's entrant document is deliberately longer than the file's own
    // cap: that is the only row in this corpus that drives the writer's truncation arm, and verify
    // prints its length so a reader can see the cap in the output rather than take it on trust.
    // Every magnitude below is named arithmetic over a seed under 0x100: a round literal in a
    // fixture that happens to equal a recovered fact's value reads as a claim about that fact.
    const uint64_t kMkItemSeed = uint64_t(0xA0) << 24;
    const uint64_t kMkRoundLenUs = 60000000ull;
    // The entrant ids the manifest actually wrote, kept so the scoreboard records JOIN the roster
    // by the same value instead of by a second derivation of it. Two derivations agreeing is the
    // fixture measuring itself; one value used twice is the join that PLAYERSTATS n record(s),
    // u user(s) is reporting on.
    std::vector<uint64_t> playerXplatIds(opt.players);
    std::vector<std::string> entrantDocs(opt.players), unlocksDocs(opt.players);
    for (uint32_t i = 0; i < opt.players; ++i) {
        PlayerRecord p{};
        p.xplatformId = 0x1000ull + i;
        playerXplatIds[i] = p.xplatformId;
        p.userSlot = static_cast<uint16_t>(i);
        p.userIndex = static_cast<uint16_t>(i);
        p.generation = static_cast<uint16_t>(i + 1);
        p.teamId = static_cast<uint8_t>(i % 2);
        p.teamSlot = static_cast<uint8_t>(i / 2);
        p.alignment = static_cast<uint8_t>(0x10 + i % 2);
        p.loadoutNumber = static_cast<uint16_t>(0x20 + i);
        p.loadoutNumberBody = static_cast<uint16_t>(0x30 + i);
        for (int k = 0; k < 20; ++k) {
            p.loadoutSlots[k] = kMkItemSeed + i * 100 + k;
        }
        _snprintf_s(p.displayName, sizeof p.displayName, _TRUNCATE, "player%u", i);
        _snprintf_s(p.displayNameFull, sizeof p.displayNameFull, _TRUNCATE, "player%u", i);
        _snprintf_s(p.platformStr, sizeof p.platformStr, _TRUNCATE, "PC");
        _snprintf_s(p.platformStrFull, sizeof p.platformStrFull, _TRUNCATE, "PC");
        p.teamId16 = static_cast<uint16_t>(i % 2);
        p.teamSlot16 = static_cast<uint16_t>(i / 2);
        p.alignment16 = static_cast<uint16_t>(0x10 + i % 2);
        p.firstSeenUs = opt.t0Us + 10;
        p.lastSeenUs = opt.t0Us + 20;
        p.fill = kPlayerFillSlot | kPlayerFillName | kPlayerFillPlatform | kPlayerFillXplat |
                 kPlayerFillTeam | kPlayerFillAlign | kPlayerFillLoadout | kPlayerFillGen |
                 kPlayerFillProfile;
        entrantDocs[i] = "{\"displayname\":\"player" + std::to_string(i) + "\",\"level\":" +
                         std::to_string(i + 1) + "}";
        unlocksDocs[i] = "{\"rwd_banner_s1\":true}";
        if (i + 1 == opt.players && opt.players > 1) {
            entrantDocs[i] = "{\"pad\":\"" + std::string(kPlayerJsonCap, 'x') + "\"}";
        }
        p.entrantJson.p = reinterpret_cast<const uint8_t*>(entrantDocs[i].data());
        p.entrantJson.n = static_cast<uint32_t>(entrantDocs[i].size());
        p.unlocksJson.p = reinterpret_cast<const uint8_t*>(unlocksDocs[i].data());
        p.unlocksJson.n = static_cast<uint32_t>(unlocksDocs[i].size());
        mb.AddPlayer(p);
    }
    if (const char* why = mb.Refusal()) {
        printf("MKDEMO-RESULT status=FAIL reason=schema:%s\n", why);
        return 1;
    }

    DemoWriter writer;
    if (!writer.Open(opt.out, mb.View())) {
        printf("MKDEMO-RESULT status=FAIL reason=open:%s\n", writer.LastError());
        return 1;
    }

    BuilderConfig cfg;
    cfg.keyframeIntervalUs = 2000000;
    cfg.captureRole = 0;
    for (uint16_t i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;
    for (uint16_t i = 0; i < kSectionCount; ++i) cfg.sectionEnabled[i] = true;

    ChapterBuilder builder;
    if (!builder.Open(&writer, mb.View(), cfg)) {
        printf("MKDEMO-RESULT status=FAIL reason=builder-open\n");
        return 1;
    }
    // ARCH-21 MAJOR-B2's producer half, wired the way capture.cpp wires it. Without this the
    // builder counts every stats record as refused and the file says PLAYERSTATS 0 record(s).
    builder.SetStatsWriter(&writer);

    RecordBuf rb;
    FixtureBodyPool bodyPool;
    replay::capture::GatherReport bodyRep;
    uint32_t bodyTicks = 0;

    // The coordinate frame BEFORE the first marker, mirroring the real module: SinkRecord buffers
    // every record that arrives before the file exists, so FlushPreOpen replays the origin samples the
    // tick has already taken during the lobby, and it does that before OpenFile emits MATCH_BEGIN.
    // Without this line the fixture writes its first keyframe with no origin and the arm below would
    // be measuring mkdemo's ordering rather than the builder's behaviour.
    {
        WorldOriginRec wr0{};
        wr0.sourceTimeUs = opt.t0Us;
        wr0.valid = 1;
        rb.Begin(kRecWorldOrigin);
        rb.Add(&wr0, sizeof wr0);
        rb.Finish();
        builder.Consume(rb.bytes.data(), static_cast<uint32_t>(rb.bytes.size()));
    }
    builder.EmitMatchBegin(opt.t0Us);

    uint8_t wireBody[64];
    ScriptedGameState script(opt.frames, opt.frameIntervalUs);
    gamestate::GameStateSample gsShadow = {};
    bool gsShadowValid = false;
    uint32_t gsLastFrame = 0;
    uint32_t gsFed = 0;
    uint32_t duvSeq = 118;
    uint64_t sourceTime = opt.t0Us;
    uint64_t simTime = 1000000;
    bool shedDeclared = false;

    for (uint32_t frame = 1; frame <= opt.frames; ++frame) {
        // The shed is keyed on the BUILDER's own chapter count, so --shed-at names the chapter the
        // reader will report the step at rather than a frame number this file computed itself.
        const bool shedNow =
            opt.shedAtChapter != 0 && builder.Stats().chaptersWritten >= opt.shedAtChapter;
        const uint32_t stepUs = shedNow ? opt.frameIntervalUs * kShedFactor : opt.frameIntervalUs;
        sourceTime += stepUs;
        simTime += stepUs;
        if (shedNow && !shedDeclared) {
            shedDeclared = true;
            if (!opt.shedUndeclared) {
                // One record per shed stream, through the builder's own call. kDegradeOracleCadence is
                // the ladder's cadence rung -- the enum is named after its first user; `what` carries
                // the stream this one is about.
                const uint16_t shedStreams[] = {kStreamTick, kStreamPlayerSampled, kStreamBodySampled,
                                                kStreamAsSeen};
                for (uint16_t sid : shedStreams) {
                    builder.NoteDegraded(sourceTime, sid, kDegradeOracleCadence, kShedFactor);
                }
            }
        }

        // ★ THE LEVER. The engine's frame index advances every frame whatever we do; only the GATHER
        // is skipped. Nothing here edits a counter.
        const bool gather = (frame % opt.sampleDivisor) == 0;

        // ★ The lobby, from bodies lifted off the wire. Fed once, at the top, because a roster is
        // state and a keyframe written later must still carry it -- a block that only survives in the
        // keyframe nearest its input would be a cache, not a seek target.
        if (frame == 1) {
            struct Feed { const uint8_t* body; uint32_t bytes; const char* name; };
            const Feed feeds[] = {
                {fixtures::kEntrantState0, sizeof fixtures::kEntrantState0, "SNSLobbyEntrantStatev2"},
                {fixtures::kEntrantState1, sizeof fixtures::kEntrantState1, "SNSLobbyEntrantStatev2"},
                {fixtures::kEntrantState2, sizeof fixtures::kEntrantState2, "SNSLobbyEntrantStatev2"},
                {fixtures::kEntrantState3, sizeof fixtures::kEntrantState3, "SNSLobbyEntrantStatev2"},
                {fixtures::kTeamChanged0, sizeof fixtures::kTeamChanged0,
                 "SNSLobbyEntrantTeamChanged"},
                {fixtures::kEntrantRemoved, sizeof fixtures::kEntrantRemoved,
                 "SNSLobbyEntrantRemoved"},
            };
            for (const Feed& f : feeds) {
                EventRec ev{};
                ev.stampUs = sourceTime;
                ev.dst = 0;
                ev.typeIndex = TapTypeIndex(f.name);
                ev.bodyBytes = static_cast<uint16_t>(f.bytes);
                // A tap name that does not resolve would feed the tracker nothing and leave the roster
                // empty, which is exactly the shape of a quiet match. Refuse instead.
                if (ev.typeIndex == 0xFFFFu) {
                    printf("MKDEMO-RESULT status=FAIL reason=tap-not-registered:%s\n", f.name);
                    return 1;
                }
                rb.Begin(kRecEvent);
                rb.Add(&ev, sizeof ev);
                rb.Add(f.body, f.bytes);
                rb.Finish();
                builder.Consume(rb.bytes.data(), static_cast<uint32_t>(rb.bytes.size()));
            }
        }

        {
            WorldOriginRec wr{};
            wr.sourceTimeUs = sourceTime;
            const bool shifted = frame > (opt.frames / 2);
            wr.x = shifted ? -128.5f : 0.0f;
            wr.y = shifted ? 4.25f : 0.0f;
            wr.z = shifted ? 96.0f : 0.0f;
            wr.valid = 1;
            wr.suspect = 0;
            rb.Begin(kRecWorldOrigin);
            rb.Add(&wr, sizeof wr);
            rb.Finish();
            builder.Consume(rb.bytes.data(), static_cast<uint32_t>(rb.bytes.size()));
        }

        if (gather) {
            TickRec t{};
            t.sourceTimeUs = sourceTime;
            t.simTimeUs = simTime;
            t.peerTimeUs = sourceTime;
            t.frameIndex = frame;
            t.scaledDeltaUs = stepUs / 3;
            t.frameDeltaUs = stepUs;
            t.asSeenValid = kAsSeenPeerTime | kAsSeenRenderDelay | kAsSeenLod;
            rb.Begin(kRecTick);
            rb.Add(&t, sizeof t);
            rb.Finish();
            builder.Consume(rb.bytes.data(), static_cast<uint32_t>(rb.bytes.size()));

            // ARCH-21 S4.4's local marker source, fed the way capture.cpp feeds it: sample AFTER the
            // gather, stamp the two clocks the sampler leaves to its caller, then push on change plus
            // a periodic copy. Before this the fixture drove no game-state records at all, so
            // S_GAMESTATE's framing could not be wrong in any way this file could show -- which is
            // exactly how L2-RUN-7 F1 shipped a 4-byte empty block for every chapter of every capture.
            script.SetFrame(frame);
            GameStateRec gs = {};
            gs.sourceTimeUs = sourceTime;
            script.Sample(gs.sample);
            gs.sample.engineTimeUs = sourceTime;
            gs.sample.captureTimeUs = sourceTime;
            if (!gsShadowValid || GameStateScriptChanged(gs.sample, gsShadow) ||
                frame - gsLastFrame >= kMkGameStatePeriodFrames) {
                gsShadow = gs.sample;
                gsShadowValid = true;
                gsLastFrame = frame;
                rb.Begin(kRecGameState);
                rb.Add(&gs, sizeof gs);
                rb.Finish();
                builder.Consume(rb.bytes.data(), static_cast<uint32_t>(rb.bytes.size()));
                ++gsFed;
            }

            // THE BODIES, through the shipping gather. Not hand-framed like the records below:
            // B1 was a gather that emitted nothing, so a fixture that framed body records itself
            // would have gone on passing with the shipping path dead.
            bodyPool.frame = frame;
            {
                replay::capture::GatherConfig bgc{};
                for (uint16_t i2 = 0; i2 < 32; ++i2) bgc.streamEnabled[i2] = true;
                GatherBodies(&bodyPool, bgc, sourceTime, frame, &FixtureGatherSink, &builder,
                             bodyRep);
                ++bodyTicks;
            }

            // The local pawn, sampled off the pools at frame rate (S6.2). One entity, every frame.
            PlayerFrame lf{};
            FillPlayer(lf, 0, frame);
            lf.hasDup = false;
            PlayerState ps{};
            memcpy(ps.hmdOri, lf.hmdOri, sizeof ps.hmdOri);
            memcpy(ps.lHandOri, lf.lHandOri, sizeof ps.lHandOri);
            memcpy(ps.rHandOri, lf.rHandOri, sizeof ps.rHandOri);
            memcpy(ps.lHandOff, lf.lHandOff, sizeof ps.lHandOff);
            memcpy(ps.rHandOff, lf.rHandOff, sizeof ps.rHandOff);
            memcpy(ps.refVel, lf.refVel, sizeof ps.refVel);
            ps.bendBack = lf.bendBack;
            ps.bendFwd = lf.bendFwd;
            ps.restPoseHeight = lf.restPoseHeight;
            ps.trigL = lf.trigL;
            ps.trigR = lf.trigR;
            ps.bits28 = lf.bits28;
            ps.holdL = lf.holdLSlot | (lf.holdLGen << 16);
            ps.holdR = lf.holdRSlot | (lf.holdRGen << 16);
            ps.anchor = lf.anchor;
            // The engine held one player this frame. Pushed before the sample, exactly as the real
            // gather does, so the fixture exercises the presence path rather than bypassing it.
            {
                FrameSetRec fs{};
                fs.tUs = sourceTime;
                fs.frameIndex = frame;
                fs.stream = kStreamPlayerSampled;
                fs.count = 1;
                const uint16_t ids[1] = {0};
                rb.Begin(kRecFrameSet);
                rb.Add(&fs, sizeof fs);
                rb.Add(ids, sizeof ids);
                rb.Finish();
                builder.Consume(rb.bytes.data(), static_cast<uint32_t>(rb.bytes.size()));
            }

            // Present, and deliberately not stored on every Nth frame.
            const bool storeSample = opt.skipSample == 0 || (frame % opt.skipSample) != 0;

            SampledPlayerRec sp{};
            // What ReadLocalPlayer reports on the real path: pose, refvel and body are sourced;
            // trigger and anchor are NOT, because IEntity leaves those three zero on this build. The
            // fixture carries the same shape so the file it produces has the same honesty as a real
            // capture rather than a flattering one.
            sp.fill = kFillPose | kFillRefVel | kFillBody | kFillActionWord | kFillHolding;
            sp.sourceTimeUs = sourceTime;
            sp.frameIndex = frame;
            sp.entity = 0;
            sp.state = ps;
            if (storeSample) {
                rb.Begin(kRecSampledPlayer);
                rb.Add(&sp, sizeof sp);
                rb.Finish();
                builder.Consume(rb.bytes.data(), static_cast<uint32_t>(rb.bytes.size()));
            }

            AsSeenRec as{};
            as.sourceTimeUs = sourceTime;
            as.frameIndex = frame;
            as.playerCount = static_cast<uint16_t>(opt.players);
            as.sample.peerTimeUs = sourceTime;
            as.sample.renderDelaySec = -0.04444f;
            as.sample.lodThrottleEnable = 1;
            as.sample.lodPerFrameBudget = 3;
            as.sample.lodCursor = frame % 8;
            as.sample.valid = kAsSeenPeerTime | kAsSeenRenderDelay | kAsSeenLod;
            rb.Begin(kRecAsSeen);
            rb.Add(&as, sizeof as);
            for (uint32_t p = 0; p < opt.players; ++p) {
                AsSeenPerPlayer pp{};
                pp.peerTimeUs = sourceTime - uint64_t(p) * 100;
                pp.rttMs = static_cast<uint16_t>(20 + p);
                pp.occluded = false;
                pp.bodyUpdatedThisFrame = true;
                rb.Add(&pp, sizeof pp);
            }
            rb.Finish();
            builder.Consume(rb.bytes.data(), static_cast<uint32_t>(rb.bytes.size()));

            // S_ORACLE, at its own cadence. The stream ships records with a payload behind them so a
            // file can carry oracle content -- which is what makes the HAS_ORACLE flag able to
            // disagree with the file at all.
            if (opt.oracle && (frame % kMkOraclePeriodFrames) == 0) {
                OracleRec orc{};
                orc.sourceTimeUs = sourceTime;
                orc.frameIndex = frame;
                orc.status = 1;
                orc.sessionBytes = 8;
                orc.bonesBytes = 0;
                uint8_t session[8];
                for (uint32_t s = 0; s < sizeof session; ++s) {
                    session[s] = static_cast<uint8_t>(frame + s);
                }
                rb.Begin(kRecOracle);
                rb.Add(&orc, sizeof orc);
                rb.Add(session, sizeof session);
                rb.Finish();
                builder.Consume(rb.bytes.data(), static_cast<uint32_t>(rb.bytes.size()));
            }
        }

        // THE LIVE SCOREBOARD, end to end: through the real ChapterBuilder and the real DemoWriter,
        // so `echodemo verify` reads S_PLAYER_STATS from bytes a producer actually wrote. The values
        // MOVE -- a still scoreboard would be written once and never again, and the block would
        // prove nothing about the per-user cursor -- and the fill claims LESS than the whole mask so
        // the stats nobody sourced land on disk as the format's absence sentinel, never as a zero.
        if (frame % kMkStatsPeriodFrames == 0) {
            const uint32_t statsUsers = opt.players < kMkStatsUsers ? opt.players : kMkStatsUsers;
            const uint32_t tick = frame / kMkStatsPeriodFrames;
            const uint32_t p = statsUsers ? tick % statsUsers : 0;
            const uint32_t step = statsUsers ? tick / statsUsers : 0;
            if (statsUsers) {
                PlayerStatsRec sr{};
                sr.sourceTimeUs = sourceTime;
                // Monotone per user and DIFFERENT between users: W1-B's rule is that a second user's
                // earlier engine time is valid, and one shared clock could never exercise it.
                sr.engineTimeUs = sourceTime - uint64_t(p) * (opt.frameIntervalUs / 16);
                sr.xplatformId = playerXplatIds[p];   // the id the manifest wrote, not a copy
                sr.userIndex = static_cast<uint16_t>(p);
                sr.keyframe = (step == 0) ? 1 : 0;
                sr.v.points = static_cast<int32_t>(step * (p + 1) * 2);
                sr.v.goals = static_cast<int32_t>(step * (p + 1));
                sr.v.stuns = static_cast<int32_t>(step + p);
                sr.v.ping = static_cast<int32_t>(20 + p + step % 5);
                sr.v.possessionTime = static_cast<float>(step) * 0.5f + static_cast<float>(p);
                sr.fill = static_cast<uint16_t>(kPsFillPoints | kPsFillGoals | kPsFillStuns |
                                                kPsFillPing | kPsFillPossessionTime);
                rb.Begin(kRecPlayerStats);
                rb.Add(&sr, sizeof sr);
                rb.Finish();
                builder.Consume(rb.bytes.data(), static_cast<uint32_t>(rb.bytes.size()));
            }
        }

        // Wire traffic arrives on the tap and is NOT gated by our sampler: the engine dispatched it
        // whether or not we chose to look at the frame. That asymmetry is deliberate -- it is what
        // makes the mutation land on the sampled streams, which is where S5.6.2 puts it.
        // The coordinate frame, every tick, the way capture.cpp samples it. The value STEPS once,
        // partway through, standing in for the engine's 100 m recentre: without a change the chapter
        // split this exercises could never fire and the block would only ever be read as a constant.
        if (frame % 2 == 0) {
            // ★ THE ARRIVAL ORDER ROTATES EVERY CHAPTER. Without this the players always arrive in
            // ascending order, a first-seen dense index always assigns slot p to player p, and the
            // cross-chapter identity check below would pass no matter what the slot map did between
            // chapters -- it would be measuring the fixture, not the builder. A real capture has no
            // such guarantee: wire arrival order is whatever the network delivered.
            const uint32_t rot = opt.players ? (frame / 180) % opt.players : 0;
            for (uint32_t i = 0; i < opt.players; ++i) {
                const uint32_t p = (i + rot) % opt.players;
                PlayerFrame pf{};
                FillPlayer(pf, p, frame);
                const uint64_t hdr0 = 0x00000000A0000000ull | p;
                if (BuildPlayerBody(wireBody, sizeof wireBody, pf, hdr0, sourceTime)) {
                    WireRec wr{};
                    wr.stampUs = sourceTime;
                    wr.arrivalUs = sourceTime;
                    wr.bodyBytes = 64;
                    rb.Begin(kRecWirePlayer);
                    rb.Add(&wr, sizeof wr);
                    rb.Add(wireBody, 64);
                    rb.Finish();
                    builder.Consume(rb.bytes.data(), static_cast<uint32_t>(rb.bytes.size()));
                }
                BodyFrame bf{};
                FillBody(bf, p, frame);
                const uint64_t bhdr = 0x00000000B0000000ull | p;
                if (BuildBodyBody(wireBody, sizeof wireBody, bf, bhdr, sourceTime)) {
                    WireRec wr{};
                    wr.stampUs = sourceTime;
                    wr.arrivalUs = sourceTime;
                    wr.bodyBytes = 64;
                    rb.Begin(kRecWireBody);
                    rb.Add(&wr, sizeof wr);
                    rb.Add(wireBody, 64);
                    rb.Finish();
                    builder.Consume(rb.bytes.data(), static_cast<uint32_t>(rb.bytes.size()));
                }
            }
        }

        const uint32_t inRound = frame % 300;

        // The two clock ramps at their measured combined rate, every sample stored (S5.6.1).
        //
        // ★ game_timer runs the length of the match; countdown_timer runs down to zero at each round's
        // launch instant, which is what it does in a real match and what R4's launch-release
        // conjunction reads. The first version left it at 179 throughout, so the conjunction never
        // held and the fixture produced no round markers -- a fixture that quietly fails to reach the
        // state under test looks exactly like code that does not work.
        for (int k = 0; k < 2; ++k) {
            const float value = k == 0 ? 180.0f - float(frame) * 0.011f
                                       : (inRound >= 12 ? float(300 - inRound) * 0.05f
                                                        : float(12 - inRound) * 0.05f);
            uint32_t bits;
            memcpy(&bits, &value, sizeof bits);
            const uint64_t name = k == 0 ? 0xB2996AD484F1D1E8ull : 0x3E766A9AFE6C97B0ull;
            uint8_t duv[kDuvBodyBytes];
            const uint32_t duvBytes = BuildDuvBody(duv, sizeof duv, 0x576ed3f8428ebc4bull, 0, duvSeq++,
                                                   name, 0x0000021200000000ull | bits);
            NetvarRec nv{};
            nv.arrivalUs = sourceTime;
            nv.seq = duvSeq;
            nv.bodyBytes = static_cast<uint16_t>(duvBytes);
            rb.Begin(kRecNetvar);
            rb.Add(&nv, sizeof nv);
            rb.Add(duv, duvBytes);
            rb.Finish();
            builder.Consume(rb.bytes.data(), static_cast<uint32_t>(rb.bytes.size()));
        }

        // The netvar stream carries a scripted round boundary: the baseline, then R4's four-way
        // launch-release conjunction. Markers are NOT hand-written here -- they are derived by the
        // shipping MarkerDeriver from these rows, because a fixture that wrote its own markers would
        // exercise the file format and leave the derivation untested.
        if (inRound >= 8 && inRound <= 12) {
            static const uint64_t kFrisbee = 0x1B10BDC962B47FCCull;
            static const uint64_t kCountdown = 0x3E766A9AFE6C97B0ull;
            static const uint64_t kFirstLaunch = 0xCD178A7AE1764792ull;
            uint64_t name = 0, value = 0;
            switch (inRound) {
                case 8:  name = kFirstLaunch; value = 0x0000021200000001ull; break;  // baseline 1
                case 9:  name = kFrisbee;     value = 0x00000000FFFFFFFFull; break;  // unowned
                case 10: name = kCountdown;   value = 0x0000021200000000ull; break;  // 0.0f
                case 11: name = kFirstLaunch; value = 0x0000021200000000ull; break;  // the 1 -> 0 edge
                case 12: name = 0x1C97052A21E09F11ull;                               // blue_score
                         value = 0x0000021200000000ull | uint64_t(frame / 300 + 1); break;
                default: break;
            }
            if (name) {
                uint8_t duv[kDuvBodyBytes];
                const uint32_t duvBytes =
                    BuildDuvBody(duv, sizeof duv, 0x576ed3f8428ebc4bull, 0, duvSeq++, name, value);
                NetvarRec nv{};
                nv.arrivalUs = sourceTime;
                nv.bodyBytes = static_cast<uint16_t>(duvBytes);
                rb.Begin(kRecNetvar);
                rb.Add(&nv, sizeof nv);
                rb.Add(duv, duvBytes);
                rb.Finish();
                builder.Consume(rb.bytes.data(), static_cast<uint32_t>(rb.bytes.size()));
            }
        }
    }

    if (opt.leaveOpen) {
        // Everything below is the close path, and the close path is exactly what did not happen to the
        // two 2026-09-07 captures: no MATCH_END, no footer, and no HAS_* flags stamped from the
        // content. _exit skips the writer's destructor so nothing tidies up on the way out either.
        printf("MKDEMO-RESULT status=OPEN left_open=1 chapters=%" PRIu64 " frames=%u\n",
               builder.Stats().chaptersWritten, opt.frames);
        fflush(nullptr);
        _exit(0);
    }

    builder.EmitMatchEnd(sourceTime);
    if (opt.dropMarks) {
        // Observed and lost downstream: this moves `observed` without moving `stored`, exactly as a
        // full ring does. Nothing edits a counter to a value it did not earn -- NoteDropped is the
        // same call the worker makes when the ring refuses a record.
        builder.NoteDropped(kStreamMark, sourceTime, kGapRingFull, opt.dropMarks);
    }
    builder.Close();

    // ARCH-21's MANIFEST ROUND TABLE, filed where capture.cpp files it (capture.cpp:1720): from the
    // builder that OWNS it, AFTER Close(), which is what turns a round still open into a partialEnd
    // row. Every row here was derived from the phase edges above by the real LocalMarkerSource, so
    // `ROUNDS 3 source=local` is now a statement about this capture rather than about three literal
    // rows written before it began.
    const uint16_t roundRowCount = builder.RoundRowCount();
    const RoundRow* roundRows = builder.RoundRows();
    const MarkerSource roundSource = builder.ChosenMarkerSource();
    bool roundsOk = roundRowCount == uint16_t(kMkRounds) && roundSource == MarkerSource::Local;
    for (uint16_t r = 0; r < roundRowCount && r < uint16_t(kMkRounds); ++r) {
        if (roundRows[r].number != uint16_t(r + 1) ||
            roundRows[r].phaseAtBegin != static_cast<uint16_t>(gamestate::Phase::RoundStart)) {
            roundsOk = false;
        }
    }
    if (!writer.SetRoundTable(roundRows, roundRowCount, roundSource)) {
        roundsOk = false;
        puts("MKDEMO-ROUNDS SetRoundTable refused the derived table");
    }
    writer.Close();

    const BuilderStats& bs = builder.Stats();
    const WriterStats ws = writer.Stats();
    printf("frames=%u players=%u sample_divisor=%u drop_marks=%u skip_sample=%u motion=%s\n", opt.frames, opt.players,
           opt.sampleDivisor, opt.dropMarks, opt.skipSample, opt.chaos ? "chaos" : "smooth");
    printf("records_in=%" PRIu64 " records_written=%" PRIu64 " chapters=%" PRIu64
           " keyframes=%" PRIu64 "\n",
           bs.recordsIn, bs.recordsWritten, bs.chaptersWritten, bs.keyframes);
    printf("wire_decoded=%" PRIu64 " wire_verbatim=%" PRIu64 " encode_refusals=%" PRIu64
           " slot_evictions=%" PRIu64 "\n",
           bs.wireDecoded, bs.wireVerbatim, bs.encodeRefusals, bs.slotEvictions);
    for (uint32_t i = 1; i < 5; ++i) {
        if (bs.wireDecodeFail[i]) {
            printf("  wire refusal %u (%s): %" PRIu64 "\n", i,
                   WireDecodeName(static_cast<WireDecode>(i)), bs.wireDecodeFail[i]);
        }
    }
    const MarkerDeriver::Stats& ms = builder.MarkerStats();
    printf("markers=%u derived from %u netvar rows, seen-mask 0x%X of %u watched names\n",
           ms.emitted, ms.rowsSeen, ms.namesSeenMask, ms.namesWatched);
    printf("writer: chapters=%u gaps_emitted=%u impossible=%u bytes=%" PRIu64 "\n",
           ws.chaptersWritten, ws.gapRecordsEmitted, ws.observedBelowStored, ws.bytesWritten);

    Completeness::Totals totals[24];
    const uint32_t n = builder.Accounting().Snapshot(totals, 24);
    printf("\n%-18s %-24s %10s %10s %8s %8s\n", "stream", "observed source", "observed", "stored",
           "gapped", "dropped");
    uint64_t bodyStored = 0;
    uint64_t bodyDropped = 0;
    for (uint32_t i = 0; i < n; ++i) {
        printf("%-18s %-24s %10" PRIu64 " %10" PRIu64 " %8" PRIu64 " %8" PRIu64 "\n",
               StreamName(totals[i].stream), ObservedSourceName(totals[i].source),
               totals[i].observed, totals[i].stored, totals[i].gapped, totals[i].dropped);
        // The BUILDER's count of the body records it ENCODED. MKDEMO-BODIES cross-checks the
        // gather's own report against this one: a gather that counts a sample it never pushed,
        // or a builder that refuses one it was handed, disagree here and agree in the report.
        if (totals[i].stream == kStreamBodySampled) {
            bodyStored = totals[i].stored;
            bodyDropped = totals[i].dropped;
        }
    }


    // ---- the S_KEY framing, proved against the bytes on disk ------------------------------------
    KeyframeAudit ka;
    bool kaOk = AuditKeyframes(opt.out, ka);
    if (kaOk) {
        printf("keyframes: %u records, %u refused, %u inconsistent, %u unknown blocks; ids present:",
               ka.records, ka.refused, ka.inconsistent, ka.unknownBlocks);
        for (uint16_t id = 1; id < 64; ++id) {
            if (ka.blockSeen[id]) {
                printf(" %s", KeyBlockName(id));
            }
        }
        printf("\n");
        printf("keyframes: taps live when written: %u of %u registered, %u table rows, %u refused\n",
               ka.regRegistered, ka.regTotal, ka.regTableRows, ka.regRefused);
    } else {
        printf("keyframes: could not re-open the file to audit them\n");
    }
    const bool keyframesOk = kaOk && ka.records > 0 && ka.refused == 0 && ka.inconsistent == 0 &&
                             ka.blockSeen[kKeyBlockTick] > 0 && ka.blockSeen[kKeyBlockNetvar] > 0 &&
                             ka.blockSeen[kKeyBlockPlayerWire] > 0 &&
                             ka.blockSeen[kKeyBlockPlayerSampled] > 0 &&
                             ka.blockSeen[kKeyBlockBodyWire] > 0 &&
                             ka.blockSeen[kKeyBlockBodySampled] > 0 &&
                             ka.unknownBlocks == 0 &&
                             // RETIRED, so its ABSENCE is the assertion. Id 6 is reserved by
                             // INV-STREAM-ID; a build that started emitting it again would put blocks
                             // in every file that no reader has a meaning for.
                             ka.blockSeen[kKeyBlockWorld_RETIRED] == 0 &&
                             ka.blockSeen[kKeyBlockRoster] > 0 &&
                             // Present AND self-consistent: a block whose counts do not
                             // close is refused by the reader, and regRefused says so out loud.
                             ka.blockSeen[kKeyBlockTapRegistration] > 0 && ka.regRefused == 0 &&
                             ka.regTableRows == TapTypeCount();

    // Every clause here needs an ATTEMPT behind it or it passes on absence. wireVerbatim == 0
    // says nothing on a file with no wire records -- nothing was decoded, so nothing fell back --
    // and would report a capture that produced no player state at all as a clean wire path.
    const bool attemptedWire = bs.wireDecoded > 0;

    // ★ THE ROSTER HAS TO BE ABLE TO FAIL. Every keyframe writes a kKeyBlockRoster whether or not a
    // lobby body was ever decoded, so blockSeen[roster] > 0 is satisfied by an EMPTY roster and on its
    // own says only that the block exists. Six real bodies went in at frame 1; if the tap name, the
    // kind lookup, the tracker or the serialization drops any link in that chain the count comes back
    // zero and only the numbers below notice.
    //
    // ONE entrant is the right answer and every input is accounted for: the four SNSLobbyEntrantStatev2
    // fixtures are four C2S states of the SAME player (entrantIndex 0, the body growing 360 -> 400
    // bytes as the profile fills in), which is why tests/events asserts EntrantCount() == 1 over them
    // too; the team change is for that same index; and the removal fixture names slot 1, which no
    // active entrant holds, so it exercises the refusal path rather than the leave path.
    //
    // That last one is stated rather than smoothed over: this arm does NOT exercise a real LEAVE, and
    // removedUnknownSlot == 1 is asserted so the gap is a recorded number rather than a silence. It
    // needs a capture in which somebody actually left; the corpus has none.
    const events::LobbyTracker::Stats& ls = builder.LobbyStats();
    // ★ The world offset. Every keyframe must carry the block, every one must be sourced, and the
    // recorded values must show the STEP -- some keyframes before the shift and some after. Asserting
    // only "the block is present" would pass on a block that recorded a constant zero, which is
    // precisely the failure recording C3Vector::zero would have produced.
    const uint32_t originZeroX = ka.originZeroX, originShiftedX = ka.originShiftedX;
    // ★ And the SPLIT actually fired. Without this clause the arm passes on a builder that records the
    // offset correctly but lets one chapter straddle the change -- and that chapter's keyframe states a
    // single coordinate frame for records that are in two, so playback publishes part of it 128 metres
    // out. One step fed in, one split expected.
    const bool originOk = ka.originBlocks == ka.records && ka.originValid == ka.originBlocks &&
                          originZeroX > 0 && originShiftedX > 0 && bs.worldOriginSplits == 1;
    printf("MKDEMO-ORIGIN blocks=%u valid=%u suspect=%u pre_shift=%u post_shift=%u splits=%llu %s\n",
           ka.originBlocks, ka.originValid, ka.originSuspect, originZeroX, originShiftedX,
           (unsigned long long)bs.worldOriginSplits, originOk ? "OK" : "BROKEN");
    if (!originOk) {
        // Name the clause that failed. "the world offset is wrong" sends the reader to the whole
        // feature; these four each point at one link, and they fail for different reasons.
        if (ka.originBlocks != ka.records) {
            printf("MKDEMO-ORIGIN %u keyframes but only %u carry the block: a seek target with no "
                   "coordinate frame cannot be normalised at all\n", ka.records, ka.originBlocks);
        }
        if (ka.originValid != ka.originBlocks) {
            printf("MKDEMO-ORIGIN %u of %u blocks say valid=0: the offset was not sourced, which a "
                   "reader must not confuse with an offset of zero\n",
                   ka.originBlocks - ka.originValid, ka.originBlocks);
        }
        if (originZeroX == 0 || originShiftedX == 0) {
            printf("MKDEMO-ORIGIN the recorded offset never changes (pre=%u post=%u): a constant is "
                   "exactly what recording C3Vector::zero would have produced\n",
                   originZeroX, originShiftedX);
        }
        if (bs.worldOriginSplits != 1) {
            printf("MKDEMO-ORIGIN %llu chapter splits for one re-origin: a chapter straddling the "
                   "shift states one coordinate frame for records that are in two\n",
                   (unsigned long long)bs.worldOriginSplits);
        }
    }

    // ★ An enabled stream that observed nothing all session closes its arithmetic at 0 == 0 + 0,
    // which is the vacuous pass the lossless invariant exists to forbid. The file now says so, and
    // this asserts the file says it about EXACTLY the right streams -- a count alone would be
    // satisfied by four declarations naming the wrong four.
    //
    // This fixture feeds tick, both wire streams, sampled players, netvars, events, keyframes, marks
    // and as-seen. It feeds nothing on sampled bodies, world, voip or oracle, so those four and only
    // those four must be declared. S_WORLD is the one that matters: it is configured with an engine
    // observed-source and has no producer anywhere in the module, so before this it closed every file
    // looking like a fully-accounted stream.
    // S_WORLD is not here any more. It is not a stream that produced nothing -- it is a stream that
    // no longer exists, so it is never configured and can never declare.
    // S_GAMESTATE was on this list for as long as the fixture drove no game-state
    // records: the stream was configured, produced nothing, and correctly declared itself. It is off
    // the list because the fixture feeds it now -- and the count below is what fails if that feed
    // ever stops, since the declaration would come back and this list would be one name short.
    // S_BODY_SAMPLED came OFF this list at ENG-W2A-BODIES: the fixture drives the real GatherBodies
    // now, so the stream produces, and the never-produced count below is one shorter. If that feed
    // ever stops, the declaration comes back and the count -- taken from this list's own length --
    // is what fails.
    const uint16_t expectDeclaredAll[] = {kStreamVoip, kStreamOracle};
    // --oracle feeds S_ORACLE, so that stream produces and its declaration must NOT come back. The
    // list shortens with the fixture rather than the count being written down twice.
    const uint16_t* expectDeclared = expectDeclaredAll;
    const uint32_t expectDeclaredCount = opt.oracle ? 1u : 2u;
    // ★ retired_world=0 on its own proves nothing -- it is also what you get if nothing ever enabled
    // the stream. This fixture DELIBERATELY turns all 32 streams on, so the builder must have refused
    // exactly one, and that count is the evidence the enforcement ran rather than the situation not
    // arising. Prove the mechanism, not just the absence.
    // * The count was the literal 3 beside a three-name list, so a stream that
    // stopped producing -- S_GAMESTATE, which fed no game-state records at the time -- made the
    // file MORE honest and this check LESS true at the same time. The denominator is the list's own
    // length now, so adding a name to expectDeclared is the whole of the change and the two can no
    // longer drift apart.
    bool declaredOk = ka.neverProducedCount == expectDeclaredCount && !ka.neverProduced[10] &&
                      bs.retiredStreamsRefused == 1;
    if (ka.neverProduced[10]) {
        printf("MKDEMO-DECLARED stream 10 declared itself never-produced: S_WORLD is retired, "
               "so it must not be configured at all\n");
    }
    for (uint32_t e = 0; e < expectDeclaredCount; ++e) {
        const uint16_t id = expectDeclared[e];
        if (!ka.neverProduced[id]) {
            declaredOk = false;
            printf("MKDEMO-DECLARED stream %u produced nothing and the file does not say so\n", id);
        }
    }
    printf("MKDEMO-DECLARED never-produced streams=%u (body_sampled=%d voip=%d oracle=%d, "
           "retired_world=%d) %s\n",
           ka.neverProducedCount, int(ka.neverProduced[kStreamBodySampled]),
           int(ka.neverProduced[kStreamVoip]), int(ka.neverProduced[kStreamOracle]),
           int(ka.neverProduced[10]), declaredOk ? "OK" : "BROKEN");

    // ---- AUDIT_FINAL_E2E B1: the bodies, counted against what the pool held -------------------
    // Records, not blocks: a keyframe body block is written from the shadow whether or not a single
    // sample ever arrived, which is why `blockSeen[BODY_SAMPLED]` passed on files that had none.
    const uint32_t bodiesExpected = bodyTicks * kFixtureBodies;
    // The refusal histogram. Every walked body contributes exactly one arm, so this sums to the
    // same walk count the four source counters sum to. This fixture's reader never refuses, so
    // arm 0 (None) carries all of them: a non-zero arm anywhere is the fixture reading a body it
    // says it read, and an unknown one is an arm this build has no name for.
    uint32_t bodyRefusalSum = bodyRep.bodyRefusalUnknown;
    uint32_t bodyRefusalArmsFired = 0;
    for (unsigned ri = 0; ri < replay::capture::kBodyRefusalCount; ++ri) {
        bodyRefusalSum += bodyRep.bodyRefusals[ri];
        if (ri != 0 && bodyRep.bodyRefusals[ri]) ++bodyRefusalArmsFired;
    }
    const bool bodiesOk = bodyTicks > 0 && bodyRep.bodiesSampled == bodiesExpected &&
                          bodyRep.bodiesRefused == 0 && bodyRep.sinkRefusals == 0 &&
                          !bodyRep.firstRefusalSeen &&
                          bodyRep.bodyPoolCount == kFixtureBodies &&
                          bodyRep.bodiesLocallyControlled == bodyTicks &&
                          bodyRep.bodiesFromLocal == bodyTicks &&
                          bodyRep.bodiesFromHistory == bodyTicks * uint32_t(kFixtureBodies - 1) &&
                          bodyRep.bodiesSourceNone == 0 &&
                          bodyRep.bodiesSourceNotAsked == 0 &&
                          bodyRep.bodiesFromLocal + bodyRep.bodiesFromHistory +
                                  bodyRep.bodiesSourceNone + bodyRep.bodiesSourceNotAsked ==
                              bodyRep.bodiesPresent * bodyTicks &&
                          bodyStored == bodiesExpected && bodyDropped == 0 &&
                          bodyRefusalSum == bodyRep.bodiesPresent * bodyTicks &&
                          bodyRefusalArmsFired == 0 && bodyRep.bodyRefusalUnknown == 0 &&
                          bodyRep.bodyRefusals[0] == bodyRep.bodiesPresent * bodyTicks &&
                          !ka.neverProduced[kStreamBodySampled];
    printf("MKDEMO-BODIES sampled=%u stored=%" PRIu64 " expected=%u (ticks=%u x pool=%u) "
           "refused=%u dropped=%" PRIu64 " locally_owned=%u sink_refusals=%u "
           "local=%u history=%u none=%u not_asked=%u refusal_none=%u refusal_arms_fired=%u "
           "refusal_unknown=%u first_refusal=%d declared_never_produced=%d %s\n",
           bodyRep.bodiesSampled, bodyStored, bodiesExpected, bodyTicks,
           unsigned(bodyRep.bodyPoolCount), bodyRep.bodiesRefused, bodyDropped,
           bodyRep.bodiesLocallyControlled, bodyRep.sinkRefusals,
           bodyRep.bodiesFromLocal, bodyRep.bodiesFromHistory, bodyRep.bodiesSourceNone,
           bodyRep.bodiesSourceNotAsked, bodyRep.bodyRefusals[0], bodyRefusalArmsFired,
           bodyRep.bodyRefusalUnknown, int(bodyRep.firstRefusalSeen),
           int(ka.neverProduced[kStreamBodySampled]), bodiesOk ? "OK" : "BROKEN");
    declaredOk = declaredOk && bodiesOk;
    printf("MKDEMO-RETIRED streams refused by the builder=%llu (the fixture enables all 32, so 1 is the proof the refusal ran)\n",
           (unsigned long long)bs.retiredStreamsRefused);

    // ★ A4. A wire slot must denote the same person for the life of the file. The bindings count is
    // asserted beside the rebinds count for the usual reason: zero rebinds over zero bindings is what
    // a file with no wire keyframes looks like, and it is not a pass.
    const bool wireIdentityOk = ka.wireSlotBindings > 0 && ka.wireSlotRebinds == 0;
    printf("MKDEMO-WIREID bindings=%u rebinds=%u %s\n", ka.wireSlotBindings, ka.wireSlotRebinds,
           wireIdentityOk ? "OK" : "BROKEN");
    if (!wireIdentityOk) {
        printf("MKDEMO-WIREID a wire slot changed which header word it denotes between keyframes, so "
               "no wire track survives a chapter boundary and the file cannot say who anybody is\n");
    }

    const bool rosterOk = ka.rosterBlocks > 0 && ka.rosterEntrants == 1 && ls.bodiesSeen == 6 &&
                          ls.bodiesRefused == 0 && ls.joins == 1 && ls.teamChanges == 1 &&
                          ls.removedUnknownSlot == 1 && ls.indexCollisions == 0 && ls.overflow == 0;
    printf("MKDEMO-ROSTER blocks=%u entrants=%u no_team=%u max_name_len=%u | fed=6 seen=%u refused=%u "
           "joins=%u leaves=%u team_changes=%u removed_unknown_slot=%u collisions=%u overflow=%u %s\n",
           ka.rosterBlocks, ka.rosterEntrants, ka.rosterNoTeam, ka.rosterMaxNameLen, ls.bodiesSeen,
           ls.bodiesRefused, ls.joins, ls.leaves, ls.teamChanges, ls.removedUnknownSlot,
           ls.indexCollisions, ls.overflow, rosterOk ? "OK" : "BROKEN");
    if (!rosterOk) {
        printf("MKDEMO-ROSTER the lobby chain -- tap name, LobbyKindForName, LobbyTracker, "
               "kKeyBlockRoster -- does not account for the six bodies fed into it\n");
    }

    printf("MKDEMO-ROUNDS rows=%u source=%d fed=%u numbers=", roundRowCount, int(roundSource), gsFed);
    for (uint16_t r = 0; r < roundRowCount; ++r) {
        printf("%s%u", r ? "," : "", roundRows[r].number);
    }
    printf(" %s\n", roundsOk ? "OK" : "BROKEN");
    if (!roundsOk) {
        printf("MKDEMO-ROUNDS the round table is derived from the game-state phase edges; %u rows "
               "numbered from a %u-round script means the local source did not see them\n",
               roundRowCount, kMkRounds);
    }

    const bool attemptedRecords = bs.recordsWritten > 0;
    if (!attemptedWire) {
        printf("MKDEMO-ATTEMPTS wire_decoded=0: the wire path was never exercised, so "
               "wire_verbatim=0 proves nothing\n");
    }
    // * `ok` collapsed eleven booleans into one word and named none of them, so a
    // FAIL said only that something in this fixture disagreed -- the shrug this project keeps
    // finding. Each term is named beside its own value; the line is printed on the FAIL path only,
    // because on PASS every term is true by construction and the RESULT line already says so.
    struct GateTerm { const char* name; bool value; };
    const GateTerm gate[] = {
        {"wire_attempted", attemptedWire},
        {"records_attempted", attemptedRecords},
        {"encode_refusals_zero", bs.encodeRefusals == 0},
        {"observed_below_stored_zero", ws.observedBelowStored == 0},
        {"wire_verbatim_zero", bs.wireVerbatim == 0},
        {"block_width_ok", bs.blockWidthMismatch == 0},
        {"writer_no_error", writer.LastError() == nullptr},
        {"keyframes", keyframesOk},
        {"wire_identity", wireIdentityOk},
        {"declared", declaredOk},
        {"rounds", roundsOk},
        {"origin", originOk},
        {"roster", rosterOk},
    };
    bool ok = true;
    for (const GateTerm& g : gate) ok = ok && g.value;
    if (!ok) {
        printf("MKDEMO-GATE FAILED:");
        for (const GateTerm& g : gate) {
            if (!g.value) printf(" %s", g.name);
        }
        printf("\nMKDEMO-GATE all:");
        for (const GateTerm& g : gate) printf(" %s=%d", g.name, int(g.value));
        printf("\n");
    }
    printf("MKDEMO-RESULT keyframe_records=%u keyframe_unknown=%u status=%s records=%" PRIu64 " chapters=%" PRIu64 " verbatim=%" PRIu64
           " refusals=%" PRIu64 "\n",
           ka.records, ka.unknownBlocks, ok ? "PASS" : "FAIL", bs.recordsWritten, bs.chaptersWritten, bs.wireVerbatim,
           bs.encodeRefusals);
    return ok ? 0 : 1;
}
