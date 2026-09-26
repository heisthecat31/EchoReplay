// W2-B. The one arm the rest of tests/playback structurally cannot be: does the shipped module
// actually CALL the two seams that put W2-H's and W3-A's residue rows into the battery.
//
// ---------------------------------------------------------------------------------------------
// ★★★ WHY A SECOND BINARY, AND WHY IT LINKS THREE PACKETS' MODULE TRANSLATION UNITS.
//
// playback_selftest drives replay_playback -- the library -- directly, so every assertion in it is
// true whether or not playback_module.cpp exists. That is exactly the blind spot this project has now
// hit four times: audio::ArmAudioResidue and residue::ArmPlaybackResidue were each written, tested
// green and left with no caller anywhere in src/ or tests/, and no suite in the tree could tell.
//
// So this binary compiles the three MODULE translation units -- W2-B's, W2-H's and W3-A's -- runs
// modreg::InitAll over them in the same ascending order the DLL does, and then asks the battery what
// is in it. Nothing here fabricates a battery: every row it observes was put there by the same lines
// that run in EchoReplay.dll.
//
// ★★ AND IT DRIVES A REAL SEEK. Registration is not firing. The last case writes an .echodemo,
// loads it through the transport the module published, seeks, and turns the module's own tick --
// then reads two counters that only a FIRED row can move. Remove either Arm call and both go still.
//
// ★ WHAT IT CANNOT SAY. No game, so W3-A's guarded entry points all refuse and its rows register
// Unmapped, and W2-H's Wwise exports do not bind. That is the correct offline outcome and it is what
// makes the id census stable; it is NOT evidence that a reconcile reaches Wwise. That needs a launch.
// ---------------------------------------------------------------------------------------------

#include "doctest.h"

#include <windows.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <string>
#include <vector>

#include "audio/audiopins.h"
#include "audio/audioresidue.h"
#include "bootstrap/bootstrap.h"
#include "bootstrap/modreg.h"
#include "bootstrap/offlinelock.h"
#include "engine/gamestate_facts.h"
#include "engine/identity_access_facts.h"
#include "identity/dresser.h"
#include "playback/core/netvarguard.h"
#include "engine/localpawn_facts.h"
#include "generated/facts_generated.h"
#include "bootstrap/services.h"
#include "engine/clock.h"
#include "engine/identity_access.h"
#include "capture/capture_types.h"
#include "capture/capturestop.h"
#include "capture/chapterbuilder.h"
#include "capture/keyframe.h"
#include "capture/manifestbuild.h"
#include "engine/netvar.h"
#include "engine/pawn_provision.h"
#include "engine/session.h"
#include "engine/tick.h"
#include "foundation/demofile/demofile.h"
#include "camera/camera_control.h"
#include "engine/camera_writepoint.h"
#include "capture/identity_block.h"
#include "playback/core/playback.h"
#include "playback/core/recorded_loadout.h"
#include "playback/core/transport.h"
#include "playback/core/viewer_role.h"
#include "playback/core/windowbuild.h"
#include "playback/core/windowprefetch.h"
#include "playback/residue/residue.h"
#include "../local_inputs.h"

namespace {

struct Line {
    replay::LogLevel level;
    std::string module;
    std::string text;
};

class RecordingLog final : public replay::ILog {
public:
    void Write(replay::LogLevel level, const char* module, const char* fmt, ...) override {
        char text[1024];
        va_list args;
        va_start(args, fmt);
        _vsnprintf_s(text, sizeof text, _TRUNCATE, fmt, args);
        va_end(args);
        lines.push_back(Line{level, module ? module : "", text});
    }
    bool Enabled(replay::LogLevel) const override { return true; }
    void DumpLedger() override {}

    std::vector<Line> lines;
};

class DefaultConfig final : public replay::IConfig {
public:
    bool GetBool(const char* key, bool dflt) const override {
        // S12.5.1's track is armed by config on the shipped path, so the case that wants one flips
        // the key the product reads rather than reaching past Load into the module.
        if (key != nullptr && strcmp(key, "playback.oracle_track") == 0) {
            return oracleTrack;
        }
        // The pre-roll exists to get the disc bound early, so it is gated on the disc drive and on
        // the score gate that makes driving it safe -- both latched at Init. A harness that could
        // not turn them on could not reach the pre-roll's state machine at all.
        if (key != nullptr && strcmp(key, "playback.disc_drive") == 0) return true;
        if (key != nullptr && strcmp(key, "playback.disc_score_gate") == 0) return false;
        // OFF unless a case asks for it: every other case in this file was written against a load
        // that starts playing the moment it is told to, and the pre-roll holds that back.
        if (key != nullptr && strcmp(key, "playback.preroll") == 0) return preroll;
        // WINDOW-DECODE-1's key, off by default exactly as the product has it: the boundary-walk
        // case turns it on the way an operator would rather than reaching past Load.
        if (key != nullptr && strcmp(key, "playback.async_window_decode") == 0) {
            return asyncWindows;
        }
        // ON unless a case asks otherwise, which is the shipped default. The case that turns it off
        // is checking that the pre-roll does not then wait for a dress that will never run.
        if (key != nullptr && strcmp(key, "playback.dress") == 0) return dress;
        if (key != nullptr && strcmp(key, "playback.reload_via_level_cycle") == 0) return reloadCycle;
        return dflt;
    }
    // Two timeouts, read once per load. A harness that could not shorten them could only test the
    // retry by waiting seconds of wall clock for each pass.
    int64_t GetInt(const char* key, int64_t dflt) const override {
        if (key != nullptr && dressRetryMs >= 0 && strcmp(key, "playback.dress_retry_ms") == 0) {
            return dressRetryMs;
        }
        if (key != nullptr && dressBoundMs >= 0 &&
            strcmp(key, "playback.dress_deferral_bound_ms") == 0) {
            return dressBoundMs;
        }
        if (key != nullptr && prerollBoundMs >= 0 &&
            strcmp(key, "playback.preroll_bound_ms") == 0) {
            return prerollBoundMs;
        }
        return dflt;
    }
    double GetDouble(const char* key, double dflt) const override {
        if (key != nullptr && prerollRate > 0.0 && strcmp(key, "playback.preroll_rate") == 0) {
            return prerollRate;
        }
        if (key != nullptr && prerollScale > 0.0 &&
            strcmp(key, "playback.preroll_clock_scale") == 0) {
            return prerollScale;
        }
        return dflt;
    }
    const char* GetString(const char*, const char* dflt) const override { return dflt; }
    bool Has(const char*) const override { return false; }

    bool oracleTrack = false;
    int64_t dressRetryMs = -1;
    int64_t dressBoundMs = -1;
    // The pre-roll's three, read once per load so a case can shorten the bound instead of waiting
    // forty seconds of wall clock for the give-up.
    bool preroll = false;
    int64_t prerollBoundMs = -1;
    double prerollRate = 0.0;
    double prerollScale = 0.0;
    bool dress = true;
    bool reloadCycle = true;
    bool asyncWindows = false;
};

// The modules' frame callbacks arrive here rather than being dropped: step 4 of S7.1 -- the battery
// fire -- lives inside playback's, and it is the only way to reach that step from outside the DLL.
//
// ★ EVERY registration is kept, not the last one. More than one module here registers early, and a
// harness that overwrote would silently decide which packet's frame ran -- the same shape as the
// defect this whole file exists to catch, reproduced in the instrument.
class CapturingTick final : public replay::ITick {
public:
    static const size_t kMax = 8;

    bool RegisterEarly(replay::TickFn fn, void* ctx) override {
        if (count >= kMax) {
            return false;
        }
        fns[count] = fn;
        ctxs[count] = ctx;
        ++count;
        return true;
    }
    bool RegisterLate(replay::TickFn, void*) override { return false; }
    const char* Mode() const override { return "none"; }

    void FireAll(const replay::TickInfo& info) const {
        for (size_t i = 0; i < count; ++i) {
            fns[i](info, ctxs[i]);
        }
    }

    replay::TickFn fns[kMax] = {};
    void* ctxs[kMax] = {};
    size_t count = 0;
};

// modreg::InitAll is once per process by construction, so the whole file shares one initialisation
// and every case reads the same battery the DLL would have built.
struct Env {
    RecordingLog log;
    DefaultConfig config;
    CapturingTick tick;
    replay::ReplayServices services{};
    size_t failures = 0;

    Env() {
        // Dressing writes another user's identity, so the module refuses it in a process that is
        // not contained (the offline bit unlocked = a live session). This harness never runs
        // bootstrap, so it reports the lock it would have applied. Every case below that expects a
        // roster to be dressed depends on this line; without it they read "still pending".
        replay::offlinelock::ReportLockedForTest();
        services.log = &log;
        services.config = &config;
        services.tick = &tick;
        replay::modreg::InitAll(&services);
        failures = replay::modreg::InitFailures();

        // One frame through modreg, so the module ticks run at least once against a battery that has
        // just been armed. W2-H's audio tick reads the battery from there, and a crash or a refusal
        // on that path is a failure of this arm rather than something a launch has to discover.
        replay::TickInfo info{};
        info.frameDeltaUs = 11111;
        info.simTimeUs = 1000000;
        info.frameIndex = 1;
        replay::modreg::TickAll(info);
    }
};

Env& TheEnv() {
    static Env env;
    return env;
}

bool Logged(const char* module, const char* needle) {
    for (const Line& l : TheEnv().log.lines) {
        if (l.module == module && l.text.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// How many times a line was logged, not whether it ever was: a boolean would let a line emitted by an
// earlier case stand in for one this case had to provoke.
size_t CountLoggedSince(size_t from, const char* module, const char* needle) {
    const std::vector<Line>& lines = TheEnv().log.lines;
    size_t n = 0;
    for (size_t i = from; i < lines.size(); ++i) {
        if (lines[i].module == module && lines[i].text.find(needle) != std::string::npos) {
            ++n;
        }
    }
    return n;
}

size_t CountLogged(const char* module, const char* needle) {
    return CountLoggedSince(0, module, needle);
}

// Anchored at the start of the line, because "recorded slot 0 -> slot 1" is a PREFIX of the
// provisioner's own mapping announcement and a SUBSTRING of every other line that names the pair --
// `actor pool gate: recorded slot 0 -> slot 1: read ok` among them. A substring count therefore
// rises whenever a new line mentions the mapping, which is not what any of these cases is about.
size_t CountLoggedOpeningSince(size_t from, const char* module, const char* prefix) {
    const std::vector<Line>& lines = TheEnv().log.lines;
    const size_t width = strlen(prefix);
    size_t n = 0;
    for (size_t i = from; i < lines.size(); ++i) {
        if (lines[i].module == module && lines[i].text.compare(0, width, prefix) == 0) ++n;
    }
    return n;
}

// The three counts playback::Init prints are the return values of the three registration passes, so
// reading them back is reading what the battery actually TOOK -- not what the three tables hold.
bool RegistrationCounts(unsigned* audioRows, unsigned* engineRows, unsigned* unserved) {
    for (const Line& l : TheEnv().log.lines) {
        if (l.module != "playback") {
            continue;
        }
        const size_t at = l.text.find("= ");
        if (at == std::string::npos) {
            continue;
        }
        if (sscanf_s(l.text.c_str() + at, "= %u audio + %u engine + %u unserved", audioRows,
                     engineRows, unserved) == 3) {
            return true;
        }
    }
    return false;
}

// ★ Ids that CANNOT come from playback_module.cpp's own table. Every one is registered by exactly one
// of the two Arm seams, so its presence is a positive witness that the seam was called -- not an
// inference from a count, and not something a placeholder could have imitated.
const char* const kAudioIds[] = {"N-5a", "N-16", "N-17a", "N-17b"};
const char* const kEngineIds[] = {"N-2", "N-3a", "N-3b", "N-4", "N-7a", "N-7b", "N-8a", "N-8b"};

// The rows nothing in this build serves, which is all W2-B's table is now. W3-A's header names the
// same six, with a reason apiece.
const char* const kUnservedIds[] = {"N-5", "N-9", "N-10", "N-12", "N-13", "N-19"};

// Ids W2-B's table used to carry beside the packets that own them. A placeholder that outlives its
// subject is a build reporting work as undone that is done -- and for N-2 and N-4 it was worse than
// that: the id collided EXACTLY, so ResidueDiagnostic's first-match answer would have been the stale
// one and UnavailableCount would have counted the row twice. Those two are in kEngineIds above; these
// four had no exact twin and simply have to be gone.
const char* const kRetiredIds[] = {"N-3", "N-7", "N-8", "N-17"};

struct TempDemo {
    wchar_t path[512];
    TempDemo() {
        wchar_t dir[400];  // facts-ok: AUDIO.LISTENER_COUNT_OFF the character capacity of a GetTempPathW buffer, not the listener-count offset
        const DWORD n = GetTempPathW(400, dir);
        if (!n) {
            dir[0] = L'.';
            dir[1] = 0;
        }
        // The serial is not decoration: named from the pid alone, two TempDemos in one case are ONE
        // file, and every "a second capture" case here was quietly loading the first one twice.
        static std::atomic<unsigned long> serial{0};
        _snwprintf_s(path, 512, _TRUNCATE, L"%lsw2b_arm_%lu_%lu.echodemo", dir,
                     (unsigned long)GetCurrentProcessId(), serial.fetch_add(1));
    }
    ~TempDemo() { DeleteFileW(path); }
};

template <class T>
void Feed(replay::capture::ChapterBuilder& b, uint16_t kind, const T& payload) {
    uint8_t buf[sizeof(replay::capture::RecHeader) + sizeof(T)];
    replay::capture::RecHeader h;
    h.kind = kind;
    h.bytes = (uint16_t)sizeof(T);
    h.producer = 0;
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, &payload, sizeof payload);
    b.Consume(buf, (uint32_t)sizeof buf);
}

// A small capture written through W2-A's real writer, because Transport::Load refuses anything else
// and a seek needs a window to land in.
//
// ★ `stallBeforeLastFrameUs` is how a caller makes the window REFUSE. Not `bodiesPerFrame`: the
// pools stopped being a literal at d3ab817 and are now derived from the capture's own cadence, so a
// deeper file simply reserves deeper pools, and not `max_bodies` either -- capture_types.h caps the
// format at kMaxBodies = 64 and slots bodies by `handle % 64`, so no well-formed file can carry a
// 65th. What a file can still do is hold a chapter open across DEAD AIR: the stall widens the last
// chapter's span without touching the tick period Load measures off the first, and the ticks that
// chapter then claims are what Reserve weighs against kWindowSampleCeiling.
// `playerSlots` are the capture's user slots: the same key PlayerRecord::userSlot carries and the
// same one a sampled player row is stored under, which is what makes the slot -> index mapping a
// join and not a guess. Default: one player in slot 0, as every existing case here expects.
bool WriteDemo(const wchar_t* path, uint64_t t0, uint64_t dt, uint32_t framesPerChapter,
               uint32_t chapters, uint32_t bodiesPerFrame,
               uint32_t keyframeIntervalUs = 2000000,
               const uint16_t* playerSlots = nullptr, uint32_t playerCount = 0,
               uint64_t stallBeforeLastFrameUs = 0, bool bodiesAsLocalPawn = false,
               // The TEAM the file recorded for every player row.  The default is the
               // recording's own "never captured" value, and leaves the row as every case
               // written before it wrote it; a case that needs the hop chain to reach its
               // authored step passes a real one, because A3 refuses TeamNotRecorded rather
               // than inventing a side for a player the capture never carried one for.
               uint16_t playerTeamId = replay::kPlayerNoTeam16,
               // Per-row teams, when the rows are not all the same side: a capture with spectator
               // rows in it is the shape the dress pass has to count correctly.
               const uint16_t* playerTeams = nullptr) {
    using namespace replay::capture;
    using namespace replay;

    ManifestBuilder mb;
    mb.Begin(t0, 0, 0x40);
    const uint8_t md5[16] = {0xca, 0xa0, 0x5e, 0xe3};
    mb.SetBuild(md5, 0x676F6C646D617374ull, 0x0102030405060708ull);
    mb.SetLevel(6300205991959903307ull, 0xF00Dull, 1780177787ull);
    mb.SetKeyframeIntervalUs(keyframeIntervalUs);
    mb.SetUuid(1, 2);
    const uint16_t oneSlot[1] = {0};
    if (playerSlots == nullptr || playerCount == 0) {
        playerSlots = oneSlot;
        playerCount = 1;
    }
    for (uint32_t i = 0; i < playerCount; ++i) {
        PlayerRecord p{};
        p.xplatformId = 0x1000 + i;
        p.userSlot = playerSlots[i];
        // Left exactly as the zero-initialised record was for every case written before this
        // parameter existed: a team is WRITTEN only when one is asked for.
        const uint16_t team = playerTeams ? playerTeams[i] : playerTeamId;
        if (team != replay::kPlayerNoTeam16) {
            p.teamId = uint8_t(team);
            p.teamId16 = team;
            // The writer stores the 16-bit team only for a producer that CLAIMS it sourced one
            // (manifest_io.cpp:201); without the claim the row lands on disk as never-recorded and
            // the value above is silently dropped.
            p.fill |= replay::kPlayerFillTeam;
        }
        mb.AddPlayer(p);
    }
    if (mb.Refusal() != nullptr) {
        return false;
    }

    DemoWriter writer;
    if (!writer.Open(path, mb.View())) {
        return false;
    }

    BuilderConfig cfg;
    cfg.keyframeIntervalUs = keyframeIntervalUs;
    for (uint16_t i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;
    for (uint16_t i = 0; i < kSectionCount; ++i) cfg.sectionEnabled[i] = true;

    ChapterBuilder b;
    if (!b.Open(&writer, mb.View(), cfg)) {
        return false;
    }
    b.EmitMatchBegin(t0);
    const uint32_t totalFrames = framesPerChapter * chapters;
    uint32_t f = 0;
    for (uint32_t c = 0; c < chapters; ++c) {
        for (uint32_t k = 0; k < framesPerChapter; ++k, ++f) {
            // Dead air ahead of the very last frame, so it lands inside the last chapter rather
            // than starting one: the chapter's span grows, its record count does not.
            const uint64_t stall = (f + 1 == totalFrames) ? stallBeforeLastFrameUs : 0;
            const uint64_t st = t0 + f * dt + stall;

            TickRec tr{};
            tr.sourceTimeUs = st;
            tr.simTimeUs = 1000000 + f * dt + stall;
            tr.frameIndex = f + 1;
            tr.frameDeltaUs = (uint32_t)(dt + stall);
            tr.scaledDeltaUs = (uint32_t)(dt + stall);
            Feed(b, kRecTick, tr);

            for (uint32_t e = 0; e < bodiesPerFrame; ++e) {
                SampledBodyRec br{};
                br.sourceTimeUs = st;
                br.frameIndex = f + 1;
                // Hi dword 1 is what makes it a real net id (S5.4.5); the low half is the identity.
                br.idValue = (1ull << 32) | (0xDEAD0000u + e);
                br.fill = kFillBodyTransform | kFillBodyLastHeld | kFillBodyBits | kFillBodyIdValue |
                          kFillBodyIdNetActor;
                br.handle = (uint16_t)e;
                // fill == 0 and tag == 0 is windowbuild.s local-pawn candidate: the body the
                // recorder was flying, binding through the player SLOT rather than the net-actor
                // table -- the one path that claims a synthetic owner.
                if (bodiesAsLocalPawn) {
                    br.idValue = 0;
                    br.fill = kFillBodyTransform | kFillBodyLastHeld | kFillBodyBits;
                }
                br.state.com[0] = (float)f;
                br.state.ori[3] = 1.0f;
                Feed(b, kRecSampledBody, br);
            }

            for (uint32_t pi = 0; pi < playerCount; ++pi) {
                SampledPlayerRec pr{};
                pr.sourceTimeUs = st;
                pr.frameIndex = f + 1;
                pr.entity = playerSlots[pi];
                pr.fill = kFillPose | kFillRefVel | kFillBody | kFillActionWord | kFillHolding;
                pr.state.hmdOri[3] = 1.0f;
                pr.state.lHandOri[3] = 1.0f;
                pr.state.rHandOri[3] = 1.0f;
                Feed(b, kRecSampledPlayer, pr);
            }
        }
        if (c + 1 == chapters) {
            b.EmitMatchEnd(t0 + f * dt + stallBeforeLastFrameUs);
        }
        b.FlushChapter();   // an explicit cut, so the chapter the poison lands in is this test's choice
    }
    b.Close();
    writer.Close();
    return true;
}

}  // namespace

using replay::playback::ResidueDiagnostic;
using replay::playback::ResidueSnapshot;
using replay::playback::ResidueStatus;

TEST_CASE("every module initialised, and the transport was published") {
    Env& e = TheEnv();
    CHECK(e.failures == 0);
    REQUIRE(e.services.transport != nullptr);
    REQUIRE(e.tick.count > 0);
}

// ---------------------------------------------------------------------------------------------
// ★★★ THE TRANSPORT IS PUBLISHED BEFORE ANY FILE EXISTS, AND MUST SAY SO.
//
// This case runs FIRST among the ones that touch a file, because the fact it asserts is about the
// session before anything has been loaded -- and it is the fact launch 1 turned into a stopped
// game: the module published `services.transport` from Init, the transport answered `Pause`, and
// W2-D's clock mirrored that pause onto the engine's own world clock 234 seconds before a level
// that never loaded (runs/20260902-071934Z/launch-01, `[clock] took 2 CClock(s)` at 03:19:41.932).
// `Pause` is a command; "no file" is not one. S9.2: absence is not a value.
// ---------------------------------------------------------------------------------------------

TEST_CASE("a transport with no file reports NotLoaded, never Pause") {
    Env& e = TheEnv();
    REQUIRE(e.services.transport != nullptr);
    CHECK(e.services.transport->State() == replay::TransportState::NotLoaded);
    CHECK(e.services.transport->State() != replay::TransportState::Pause);
    // The fact both W2-D's clock and W2-H's RateDriver engage on, in the same state.
    CHECK(e.services.transport->DurationUs() == 0);

    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 1, 1));
    REQUIRE(e.services.transport->Load(tmp.path));

    // Loaded and not playing is Pause, and now that IS a command: the two readings are distinct.
    CHECK(e.services.transport->DurationUs() > 0);
    CHECK(e.services.transport->State() == replay::TransportState::Pause);
    e.services.transport->Play(1.0f);
    CHECK(e.services.transport->State() == replay::TransportState::Play);

    e.services.transport->Unload();
    // ★ AND IT GOES BACK. An unload that reported Pause would re-arm the same failure on the tick
    // after a replay was closed.
    CHECK(e.services.transport->State() == replay::TransportState::NotLoaded);
    CHECK(e.services.transport->DurationUs() == 0);
}

TEST_CASE("playback::Init armed both seams: the battery holds all three tables and nothing twice") {
    TheEnv();
    const ResidueStatus st = ResidueSnapshot();
    CHECK(st.consulted);

    // ★ THE MUTATION TARGET. 4 audio + 8 engine + 6 unserved. Drop audio::ArmAudioResidue and this
    // is 14; drop residue::ArmPlaybackResidue and it is 10; drop both and it is the 6 unserved rows
    // alone. It is a FLOOR rather than an equality because W3-A and W3-B are still adding rows and a
    // battery that grows is the expected direction; every id below is checked by name, which is what
    // makes the floor discriminating rather than merely permissive.
    CHECK(st.registered >= 18);

    for (const char* id : kAudioIds) {
        INFO("audio row ", id);
        CHECK(ResidueDiagnostic(id) != nullptr);
    }
    for (const char* id : kEngineIds) {
        INFO("engine row ", id);
        CHECK(ResidueDiagnostic(id) != nullptr);
    }
    for (const char* id : kUnservedIds) {
        INFO("unserved row ", id);
        CHECK(ResidueDiagnostic(id) != nullptr);
    }
    for (const char* id : kRetiredIds) {
        INFO("retired placeholder ", id);
        CHECK(ResidueDiagnostic(id) == nullptr);
    }

    const size_t named = sizeof kAudioIds / sizeof kAudioIds[0] +
                         sizeof kEngineIds / sizeof kEngineIds[0] +
                         sizeof kUnservedIds / sizeof kUnservedIds[0];
    CHECK(st.registered >= named);

    // N-2 and N-4 are the two ids that existed on BOTH tables. They must answer with W3-A's reason
    // now, not with the placeholder's -- the collision this change removed.
    REQUIRE(ResidueDiagnostic("N-2") != nullptr);
    REQUIRE(ResidueDiagnostic("N-4") != nullptr);
    CHECK(strstr(ResidueDiagnostic("N-2"), "RESET.GRAB_POS_HAND_FIXUPS_PC") != nullptr);
    CHECK(strstr(ResidueDiagnostic("N-4"), "RESET.SNAP_TIME_OFFSET_PC") != nullptr);
}

TEST_CASE("the startup line reports what each seam actually returned, and the three sum to the battery") {
    TheEnv();
    unsigned audioRows = 0, engineRows = 0, unserved = 0;
    REQUIRE(RegistrationCounts(&audioRows, &engineRows, &unserved));

    // Both Arm calls hand back how many rows the battery TOOK. Printing the tables' sizes instead
    // would report 4 and 8 even from a battery that had refused every one of them.
    CHECK(audioRows == 4);     // N-5a, N-17a, N-16, N-17b
    CHECK(engineRows >= 8);    // N-2, N-3a/b, N-4, N-7a/b, N-8a/b, and growing
    CHECK(unserved == 6);      // this file's own table

    // ★ Three separately measured numbers against the battery's own count. They are equal only while
    // these are the only three passes that register; a fourth registrar, or a row taken by something
    // that did not report it, splits them.
    CHECK((size_t)(audioRows + engineRows + unserved) == ResidueSnapshot().registered);
}

TEST_CASE("W3-A's armed flag is set, and the module that exists to catch an unarmed battery is silent") {
    Env& e = TheEnv();

    // The direct witness: RegisterPlaybackResidue sets this and nothing else does.
    CHECK(replay::playback::residue::Stats().armed);

    // ★ And the silence below is a verdict rather than an absence, because the module that would have
    // broken it is demonstrably present and demonstrably initialised: residue-check reports at Error
    // when Stats().armed is false, and it is in the registry with no failures. Without these two
    // lines "no such log line" and "no such module" would read alike.
    bool haveChecker = false;
    for (size_t i = 0; i < replay::modreg::Count(); ++i) {
        if (strcmp(replay::modreg::At(i)->name, "residue-check") == 0) {
            haveChecker = true;
        }
    }
    CHECK(haveChecker);
    CHECK(e.failures == 0);
    CHECK_FALSE(Logged("residue-check", "was never armed"));
}

TEST_CASE("a seek through the armed battery reaches both packets' FIRE paths, not just their rows") {
    Env& e = TheEnv();
    TempDemo tmp;
    const uint64_t t0 = 5000000;
    const uint64_t dt = 11111;
    REQUIRE(WriteDemo(tmp.path, t0, dt, 60, 1, 1));

    REQUIRE(e.services.transport->Load(tmp.path));
    REQUIRE(e.services.transport->DurationUs() > 0);

    const uint32_t rebasesBefore = replay::audio::AudioResidueStatsRef().clockRebases;

    // Forward, then back over the same span: the backwards one is the jump the owner makes and the
    // one the memo cache survives wrongly.
    e.services.transport->Seek((int64_t)(t0 + 50 * dt));
    e.tick.FireAll(replay::TickInfo{});
    e.services.transport->Seek((int64_t)(t0 + 5 * dt));
    e.tick.FireAll(replay::TickInfo{});

    // N-5a increments only inside ClockRebaseResidue::Fire, and the battery skips a row it does not
    // hold. Two seeks, so two rebases -- a count that moved by one would mean a jump was swallowed.
    CHECK(replay::audio::AudioResidueStatsRef().clockRebases == rebasesBefore + 2);

    // W3-A's half has no counter that can move without a game -- every effect it has is a write into
    // engine memory that is not there. What it does emit is this, from ReportCs, on the first fire of
    // a Ready row: proof the battery reached W3-A's code, and simultaneously the honest statement
    // that the rows did nothing.
    CHECK(Logged("residue", "component-system rows are inert"));

    e.services.transport->Unload();
}

// ==============================================================================================
// ★★★ THE MODULE SEAM WINDOW-DECODE-1 SHIPPED WITHOUT. tests/playback/test_windowprefetch.cpp
// drives the WORKER, and the worker was never where the defect was: the adoption in
// playback_module.cpp read `ready_centre` three lines after clearing it, so every adoption set the
// window centre to SIZE_MAX, RequestPrefetch's bounds guard then refused for the rest of the
// session, and the module coasted to the coverage edge and took the full decode there -- the
// hitch the packet exists to delete, moved later and made worse.
//
// This is the walk that would have caught it: a real file, real chapter boundaries, the module's
// own Tick, and the four claims the feature is -- no decode on the tick in steady playback either
// way at either rate, one per seek, the centre following what was adopted, and three slots.
//
// ★ THE PLAYHEAD IS MOVED BY WALL TIME. This module reads QPC itself rather than the tick's own
// delta (S7.3.1), so a boundary is crossed by REAL milliseconds passing -- which is also what
// gives the worker a genuine race to win. Hence a short chapter and a frame-length sleep.
namespace {

using replay::playback::WindowDecodeStats;
using replay::playback::WindowDecodeStatsForTest;

constexpr uint64_t kWalkDt = 11111;        // the cadence the fixture files are written at
constexpr uint32_t kWalkFramesPerChapter = 8;   // ~89 ms of source per chapter, so a walk is seconds

// Ticks the module until the adopted window centre leaves `from`, and answers where it went.
// Bounded in wall milliseconds: a centre that never moves must fail a case, not hang one.
size_t WalkUntilCentreLeaves(Env& e, size_t from, unsigned boundMs) {
    const uint64_t deadline = GetTickCount64() + boundMs;
    for (;;) {
        replay::TickInfo info{};
        info.frameDeltaUs = kWalkDt;
        e.tick.FireAll(info);
        const size_t centre = WindowDecodeStatsForTest().windowCentre;
        if (centre != from || GetTickCount64() >= deadline) {
            return centre;
        }
        Sleep(1);
    }
}

}  // namespace

TEST_CASE("WINDOW-DECODE: the boundary walk costs no tick decode, and the centre follows the "
          "window that was adopted") {
    Env& e = TheEnv();
    TempDemo tmp;
    const uint64_t t0 = 5000000;
    REQUIRE(WriteDemo(tmp.path, t0, kWalkDt, kWalkFramesPerChapter, 12, 1));

    e.config.asyncWindows = true;
    REQUIRE(e.services.transport->Load(tmp.path));
    WindowDecodeStats s = WindowDecodeStatsForTest();
    REQUIRE(s.asyncOn);
    CHECK(s.tickDecodes == 1);          // the load's own window, and that one is not a defect
    CHECK(s.windowsAllocated == 3);     // the memory bound: cur + ready + free + borrowed == 3
    const uint64_t decodesAfterLoad = s.tickDecodes;

    // 1. FORWARD AT 1x. Two boundaries, and the centre must be the chapter that was adopted --
    // SIZE_MAX here is the shipped blocker, and after it no prefetch is ever posted again.
    e.services.transport->Play(1.0f);
    size_t centre = s.windowCentre;
    for (int boundary = 0; boundary < 2; ++boundary) {
        const size_t moved = WalkUntilCentreLeaves(e, centre, 4000);
        REQUIRE(moved != (size_t)-1);
        CHECK(moved > centre);
        centre = moved;
    }
    s = WindowDecodeStatsForTest();
    CHECK(s.adoptions >= 2);
    CHECK(s.tickDecodes == decodesAfterLoad);   // nothing decoded on the tick to get here
    CHECK(s.windowsAllocated == 3);

    // 2. FORWARD AT 0.1x -- the rate the owner watches at, and the one a slow worker has the most
    // room to serve.
    e.services.transport->Play(0.1f);
    centre = WalkUntilCentreLeaves(e, centre, 6000);
    REQUIRE(centre != (size_t)-1);
    CHECK(WindowDecodeStatsForTest().tickDecodes == decodesAfterLoad);

    // 3. REVERSE. The prefetch target is the next core chapter in the DIRECTION OF TRAVEL, so a
    // flip that kept warming forwards would show up here as a tick decode.
    e.services.transport->Play(-1.0f);
    const size_t back = WalkUntilCentreLeaves(e, centre, 4000);
    REQUIRE(back != (size_t)-1);
    CHECK(back < centre);
    CHECK(WindowDecodeStatsForTest().tickDecodes == decodesAfterLoad);
    CHECK(WindowDecodeStatsForTest().windowsAllocated == 3);

    // 4. ONE SEEK, ONE DECODE. A jump out of the resident span has nothing decoded to adopt, and
    // the covering guard owes exactly one build for it.
    e.services.transport->Pause();
    const uint64_t beforeSeek = WindowDecodeStatsForTest().tickDecodes;
    e.services.transport->Seek((int64_t)(10 * kWalkFramesPerChapter * kWalkDt));
    replay::TickInfo info{};
    info.frameDeltaUs = kWalkDt;
    e.tick.FireAll(info);
    CHECK(WindowDecodeStatsForTest().tickDecodes == beforeSeek + 1);
    CHECK(WindowDecodeStatsForTest().windowCentre != (size_t)-1);

    e.services.transport->Unload();
    e.config.asyncWindows = false;
}

// ★★ A JOIN THAT TIMED OUT LEAVES A WORKER INSIDE A BUFFER THIS MODULE CAN NO LONGER NAME. Reusing
// those slots on the next Load is not a leak, it is a heap corruption: Window::Reserve delete[]s
// the pools when the new file needs deeper ones, under a thread still writing into them. So the
// slots are abandoned, `cur` starts again on storage nobody else has ever seen, and async decode is
// off for the rest of the process.
TEST_CASE("WINDOW-DECODE: a timed-out join abandons its windows and the next load starts clean") {
    Env& e = TheEnv();
    TempDemo tmp;
    const uint64_t t0 = 5000000;
    REQUIRE(WriteDemo(tmp.path, t0, kWalkDt, kWalkFramesPerChapter, 6, 1));

    e.config.asyncWindows = true;
    REQUIRE(e.services.transport->Load(tmp.path));
    const WindowDecodeStats before = WindowDecodeStatsForTest();
    REQUIRE(before.asyncOn);
    REQUIRE(before.windowsAllocated == 3);

    replay::playback::winprefetch::FailNextStopForTest();
    e.services.transport->Unload();
    const WindowDecodeStats abandoned = WindowDecodeStatsForTest();
    CHECK(abandoned.windowsAllocated == 1);            // one fresh slot; the other three are gone
    CHECK(abandoned.current != before.current);        // and `cur` is not the borrowed storage

    REQUIRE(e.services.transport->Load(tmp.path));
    const WindowDecodeStats reloaded = WindowDecodeStatsForTest();
    CHECK_FALSE(reloaded.asyncOn);                     // no second worker over an abandoned first
    CHECK(reloaded.windowsAllocated == 1);             // and no slot 1/2 to Reserve over
    CHECK(reloaded.current != before.current);
    CHECK(Logged("playback", "off for the rest of this process"));

    e.services.transport->Unload();
    e.config.asyncWindows = false;
}

// The load-time sizing refusal, spelled once here. window.cpp says this half of the sentence and
// playback_module.cpp wraps it in the cadence it measured; this is the part both agree on.
const char* const kSizingRefusal = "more samples than the playback window can allocate";

// ★★★ INV-D. A capture the window cannot be sized for is refused AT LOAD, by name, and said once.
//
// This case used to provoke a MID-FILE "window build refused" out of a 4-chapter, 64-body file whose
// third chapter overran the literal pool of 16384 samples. d3ab817 took the literal away:
// CapsForCadence reads the capture's own chapter span and tick period at Load and Reserve grows the
// pools to them, so a well-formed file's three-chapter range now fits BY CONSTRUCTION and no file
// this build can write refuses at a seek. The old fixture was asserting a refusal the product had
// stopped owing -- it read 0 == 1 because the product got better, not worse.
//
// The refusal that is real sits at the other end of the same formula: Reserve will not allocate past
// kWindowSampleCeiling, and Load says so, by name, before a playhead has moved.
TEST_CASE("INV-D: a capture whose window cannot be sized is refused AT LOAD, by name") {
    Env& e = TheEnv();
    TempDemo tmp;   // ONE path: TempDemo names itself after the process, so two live ones alias.
    const uint64_t t0 = 5000000;
    const uint64_t dt = 11111;

    // ★ THE CONTROL, FIRST -- the very file this case was written around: 64 bodies over 90 frames,
    // four chapters. Its window is sized from its own cadence, so it opens, it jumps, and it refuses
    // nothing. Without it, a refusal that fired for every capture would read exactly like the one
    // below.
    REQUIRE(WriteDemo(tmp.path, t0, dt, 90, 4, 64));
    const size_t refusalsBefore = CountLogged("playback", kSizingRefusal);
    const uint32_t rebasesBefore = replay::audio::AudioResidueStatsRef().clockRebases;
    REQUIRE(e.services.transport->Load(tmp.path));
    CHECK(CountLogged("playback", kSizingRefusal) == refusalsBefore);

    // The residue debt this case is named for is still owed on the tick a jump is taken, and Step is
    // the producer that makes one. Say plainly what this no longer proves: the window build behind
    // the jump now SUCCEEDS, so it cannot discriminate the early return that used to sit between
    // TakeDiscontinuity and the battery. That precondition left the product; it is not hidden here.
    e.services.transport->Step(150);
    e.tick.FireAll(replay::TickInfo{});
    CHECK(replay::audio::AudioResidueStatsRef().clockRebases == rebasesBefore + 1);
    e.services.transport->Unload();

    // ★★ THE REFUSAL. The ceiling is on SAMPLES, so the file is built in the terms the product sizes
    // in: the ticks one chapter must claim before max_bodies of them across kWindowChapters chapters
    // pass kWindowSampleCeiling. Those ticks are bought with dead air rather than with records, so
    // the file stays eight frames long and still asks for millions of samples. The keyframe interval
    // is widened past the stall because ChapterBuilder cuts a chapter at that width
    // (chapterbuilder.cpp:1588): a cut at the gap would leave the stalled tick alone in a zero-span
    // chapter, which MeasureCadence skips, and the file would load.
    const replay::playback::WindowCaps defaults;
    const size_t ticksOverCeiling =
        replay::playback::kWindowSampleCeiling /
            (defaults.max_bodies * replay::playback::kWindowChapters) +
        1;
    const uint64_t stallUs = (uint64_t)ticksOverCeiling * dt;
    REQUIRE(WriteDemo(tmp.path, t0, dt, 4, 2, 1, (uint32_t)(2 * stallUs), nullptr, 0, stallUs));

    const size_t sizingBefore = CountLogged("playback", kSizingRefusal);
    REQUIRE_FALSE(e.services.transport->Load(tmp.path));
    REQUIRE(CountLogged("playback", kSizingRefusal) == sizingBefore + 1);

    // By NAME means the sentence carries the arithmetic it refused on, not a bare verdict.
    const std::string ceiling =
        std::string("the ceiling is ") + std::to_string(replay::playback::kWindowSampleCeiling);
    CHECK(Logged("playback", ceiling.c_str()));
}


// ★★★ THE RETIRED STREAM. S_WORLD (id 10) sat in the window's decode table after the stream itself
// was withdrawn, and a row in that table is not an inert name: DecodeBodyStream takes its codec from
// the flag beside the id, so an old capture's S_WORLD block was handed to the SAMPLED BODY decoder.
//
// This build refuses to WRITE a retired id, so no fixture can contain one -- which is exactly why
// BuildStats publishes the mask of ids the decode asked for. Bit 10 clear is the invariant, and it is
// true whether or not any file exercises it.
TEST_CASE("INV-STREAM-ID: the window never asks for the retired stream, and says what it skipped") {
    Env& e = TheEnv();
    TempDemo tmp;
    const uint64_t t0 = 5000000;
    const uint64_t dt = 11111;
    REQUIRE(WriteDemo(tmp.path, t0, dt, 30, 2, 2));

    replay::DemoReader reader;
    REQUIRE(reader.Open(tmp.path));
    replay::playback::Window w;
    replay::playback::BuildStats bs;
    REQUIRE(replay::playback::BuildWindow(reader, replay::playback::ChapterForTime(reader, (int64_t)t0),
                                          w, bs));

    // ★ Prove the mask is real before reading anything out of it. A mask that came back zero would
    // make the assertion below pass for the wrong reason -- the vacuous shape this file exists to
    // refuse.
    REQUIRE((bs.streams_asked & (1u << replay::kStreamBodySampled)) != 0);
    REQUIRE((bs.streams_asked & (1u << replay::kStreamTick)) != 0);

    // THE MUTATION TARGET. Putting {kStreamWorld, ...} back in kStreams sets this bit, because Take()
    // records every id the table names whether or not the chapter carries one.
    CHECK((bs.streams_asked & (1u << replay::kStreamWorld)) == 0);

    // And what the window did not read is a number with a name on it, not an absence. A real capture
    // carries S_MARK and the shed-ladder streams, which this window does not decode.
    CHECK(bs.streams_undecoded > 0);
    REQUIRE(bs.first_undecoded_stream != nullptr);
    CHECK(bs.first_undecoded_stream[0] != 0);

    // The hand-built case, because no writer in this build can produce one: a chapter carrying only
    // stream 10 is counted and named S_WORLD rather than decoded as bodies.
    replay::StreamView sv{};
    sv.id = replay::kStreamWorld;
    replay::DecodedChapter dc{};
    dc.streamCount = 1;
    dc.streams = &sv;
    replay::playback::BuildStats only;
    replay::playback::CountUndecodedStreams(dc, bs.streams_asked, only);
    CHECK(only.streams_undecoded == 1);
    REQUIRE(only.first_undecoded_stream != nullptr);
    CHECK(strcmp(only.first_undecoded_stream, "S_WORLD") == 0);

    reader.Close();
    (void)e;
}

namespace {

replay::audio::AkPlayingID FakePostEvent(replay::audio::AkUniqueID, replay::audio::AkGameObjectID,
                                         uint32_t, void (*)(int32_t, void*), void*, uint32_t, void*,
                                         replay::audio::AkPlayingID) {
    return 1;   // a non-zero playing id is what RearmResidue counts as re-armed
}

int32_t FakeSeekOnEvent(replay::audio::AkUniqueID, replay::audio::AkGameObjectID,
                        replay::audio::AkTimeMs, bool, replay::audio::AkPlayingID) {
    return 0;
}

}  // namespace

// ★★★ INV-AXIS. Everything crossing IReplayTransport is microseconds since the START OF PLAYBACK;
// absolute source time lives only inside the transport.
//
// The fixture's t0Us is deliberately NOT zero, because zero is the one value that makes the two axes
// the same number -- which is what let 31 cases and a FakeTransport with no origin at all miss a
// timeline that could not seek. Every assertion below is false on the pre-ruling transport.
TEST_CASE("INV-AXIS: the public axis starts at zero and a fraction of the duration is seekable") {
    Env& e = TheEnv();
    TempDemo tmp;
    const uint64_t t0 = 5000000;   // NOT zero: the fixture must be able to fail
    const uint64_t dt = 11111;
    REQUIRE(WriteDemo(tmp.path, t0, dt, 60, 1, 1));
    REQUIRE(e.services.transport->Load(tmp.path));

    replay::IReplayTransport* t = e.services.transport;
    const int64_t duration = (int64_t)t->DurationUs();
    REQUIRE(duration > 0);
    // ★ The fixture discriminates only while the origin is bigger than the length; otherwise a
    // wrong-axis answer could still land inside the file and look plausible.
    REQUIRE((int64_t)t0 > duration);

    // Load parks the playhead at the START OF PLAYBACK, which is zero on this axis.
    CHECK(t->PlayheadUs() == 0);

    // Seek(0) is the first frame and Seek(duration) is the last, and they are different places.
    t->Seek(0);
    CHECK(t->PlayheadUs() == 0);
    t->Seek(duration);
    const int64_t atEnd = t->PlayheadUs();
    CHECK(atEnd > 0);
    CHECK(atEnd <= duration);

    // ★★ THE THING THAT WAS BROKEN: a fraction of the duration is DIRECTLY seekable, with no
    // correction from the caller. Before the ruling this asked for a time below the first recorded
    // tick and SnapToTarget clamped it to the start, so every scrub landed on frame one.
    t->Seek(duration / 2);
    const int64_t mid = t->PlayheadUs();
    CHECK(mid > 0);                 // it MOVED -- the whole defect was that it did not
    CHECK(mid < atEnd);
    CHECK(mid <= duration / 2);
    CHECK(mid > duration / 2 - (int64_t)dt * 2);   // snapped down to a recorded tick, not clamped

    // ★ The playhead stays inside the span the UI classifies cells against. The panel drew nothing
    // at all before, because an absolute playhead falls outside a 0-based span.
    CHECK(t->PlayheadUs() >= 0);
    CHECK(t->PlayheadUs() <= duration);

    // A bookmark round-trips: whatever PlayheadUs reports is directly seekable back. W2-G's mark and
    // goto are axis-agnostic exactly because of this, and must stay that way.
    const int64_t bookmark = t->PlayheadUs();
    t->Seek(0);
    REQUIRE(t->PlayheadUs() == 0);
    t->Seek(bookmark);
    CHECK(t->PlayheadUs() == bookmark);

    e.services.transport->Unload();
}

namespace {

// The entity surface as W1-E ships it: one object for the process whose Available() flips when a
// game space binds. Handles resolve from the low bits of the net id, so a demo's bodies bind.
class SwitchableEntity : public replay::IEntity {
public:
    bool available = false;
    size_t installs = 0;

    size_t PhysCount() const override { return 16; }
    bool IsLocallyControlled(replay::PhysHandle) const override { return false; }
    void SetSyntheticOwner(replay::PhysHandle) override {}
    void ClearSyntheticOwner(replay::PhysHandle) override {}
    bool ReadBody(replay::PhysHandle, replay::BodyState&) const override { return false; }
    void PushHistory(replay::PhysHandle, const replay::BodyState&, uint64_t) override {}
    void TrimHistory(replay::PhysHandle, uint64_t, uint64_t) override {}
    bool SetHistoryWindow(replay::PhysHandle, const replay::BodyState*, const uint64_t*,
                          size_t) override {
        ++installs;
        return true;
    }
    replay::PhysHandle PhysOfNetId(uint64_t id) const override {
        return available ? replay::PhysHandle{(uint16_t)(id & 0xF)} : replay::PhysHandle{0xFFFFu};  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    }
    float GetTimeOffset(replay::PhysHandle) const override { return 0.0f; }
    void SetTimeOffset(replay::PhysHandle, float) override {}
    size_t PlayerCount() const override { return 0; }
    bool ReadPlayer(replay::PlayerHandle, replay::PlayerState&) const override { return false; }
    void PushPlayerHistory(replay::PlayerHandle, const replay::PlayerState&, uint64_t) override {}
    replay::PhysHandle PhysOf(replay::PlayerHandle) const override {
        return replay::PhysHandle{0xFFFFu};  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    }
    replay::PlayerHandle PlayerOf(replay::UserIndex) const override {
        return replay::PlayerHandle{0xFFFFu};  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    }
    // The LOD enable dword is an engine write; this fake is the recorder for
    // it. Claims and releases are counted separately because "put it back" and "take it" are the
    // two facts the gate case has to tell apart.
    std::vector<int> lodClaims;   // one entry per write, the value written
    size_t lodReleases = 0;

    void SetLodThrottle(bool enabled) override { lodClaims.push_back(enabled ? 1 : 0); }
    bool ClaimLodThrottle(bool enabled) override {
        lodClaims.push_back(enabled ? 1 : 0);
        return true;
    }
    bool ReleaseLodThrottle() override {
        ++lodReleases;
        return true;
    }
    bool Available() const override { return available; }
    const char* Diagnostic() const override { return available ? "fake: bound" : "fake: unbound"; }
};

class CountingNetVar final : public replay::INetVar {
public:
    size_t Poll(replay::NetVarWrite*, size_t) override { return 0; }
    bool Write(const replay::NetVarWrite&) override {
        ++writes;
        return true;
    }
    bool Read(replay::Symbol64, uint8_t, uint64_t&) const override { return false; }
    size_t ObservedCount() const override { return 0; }

    size_t writes = 0;
};

}  // namespace

using replay::playback::BindingSnapshot;
using replay::playback::BindingStatus;
using replay::playback::IdentitySnapshot;
using replay::playback::IdentityStatus;

TEST_CASE("the applier writes through what is published NOW, not what Init saw") {
    Env& e = TheEnv();

    // W1-E publishes both at Init (order 15), ahead of playback (300), through the real InitAll --
    // its module TU is linked here for exactly this assertion. Null here is the wiring gap
    // DIAG-WIRING ranked first, and no fake may fill it.
    CHECK(e.services.entity != nullptr);
    CHECK(e.services.pose != nullptr);
    CHECK(BindingSnapshot().entity == e.services.entity);
    CHECK(BindingSnapshot().pose == e.services.pose);

    // W2-A publishes INetVar from its FIRST TICK -- capture.cpp's module Tick, which runs after
    // every Init. So at this point, with InitAll done, playback's Init-time binding is null...
    const BindingStatus atInit = BindingSnapshot();
    CHECK(atInit.netvar == nullptr);
    CHECK(atInit.netvar == e.services.netVar);

    // ...and the publication lands the way capture's does: after Init, before the next tick.
    CountingNetVar nv;
    e.services.netVar = &nv;
    const uint64_t rebindsBefore = atInit.rebinds;
    e.tick.FireAll(replay::TickInfo{});

    const BindingStatus afterTick = BindingSnapshot();
    CHECK(afterTick.netvar == &nv);
    CHECK(afterTick.rebinds == rebindsBefore + 1);
    CHECK(Logged("playback", "services re-bound: entity"));

    // A second tick with nothing changed re-binds nothing: the compare is the whole cost.
    e.tick.FireAll(replay::TickInfo{});
    CHECK(BindingSnapshot().rebinds == rebindsBefore + 1);

    // Withdrawal is a change too. Publishing null must not leave the applier writing into a
    // surface its owner has torn down.
    e.services.netVar = nullptr;
    e.tick.FireAll(replay::TickInfo{});
    CHECK(BindingSnapshot().netvar == nullptr);
    CHECK(BindingSnapshot().rebinds == rebindsBefore + 2);
}

TEST_CASE("a loaded roster is dressed or DEFERRED and counted, never silently skipped") {
    Env& e = TheEnv();
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1));
    replay::IReplayTransport* t = e.services.transport;
    const size_t mark = e.log.lines.size();
    REQUIRE(t->Load(tmp.path));

    // This process publishes no IIdentity and no validated IEntity, so Load itself is the first
    // deferral: counted at once, with the reason in the log.
    const IdentityStatus atLoad = IdentitySnapshot();
    CHECK(atLoad.pending);
    CHECK(atLoad.players == 1);
    CHECK(atLoad.dressed == 0);
    CHECK(atLoad.deferred == 1);
    CHECK(Logged("playback", "identity dressing deferred:"));

    // Every tick asks the gate again -- the surfaces arrive on their own schedule -- and counts.
    e.tick.FireAll(replay::TickInfo{});
    e.tick.FireAll(replay::TickInfo{});
    const IdentityStatus afterTicks = IdentitySnapshot();
    CHECK(afterTicks.pending);
    CHECK(afterTicks.deferred == 3);
    CHECK(strstr(t->ActiveModeLabel(), "identity: DEFERRED (3 ticks)") != nullptr);
    // Said once, not once per tick.
    CHECK(CountLoggedSince(mark, "playback", "identity dressing deferred:") == 1);

    t->Unload();
    const IdentityStatus unloaded = IdentitySnapshot();
    CHECK_FALSE(unloaded.pending);
    CHECK(unloaded.deferred == 0);
}

// Through the MODULE: the load path the DLL runs, on the owner's real spectator
// capture. test_track_map.cpp proves the keying against the file; this proves Load measures the wire
// clocks, sizes the window from the scan, hands the clocks to every rebuild, and that the bind
// refuses every wire body while no recorded slot has been provisioned. The file lives in the
// owner's Documents; without it the case fails unless the run opts out by name.
namespace {

class SlotPawnEntity final : public SwitchableEntity {
public:
    float com[16][3] = {};
    size_t installsAt[16] = {};

    replay::PlayerHandle PlayerOf(replay::UserIndex u) const override {
        return replay::PlayerHandle{u.v < 16 ? u.v : (uint16_t)0xFFFFu};  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    }
    replay::PhysHandle PhysOf(replay::PlayerHandle h) const override {
        return replay::PhysHandle{h.v};
    }
    bool SetHistoryWindow(replay::PhysHandle h, const replay::BodyState* s, const uint64_t*,
                          size_t n) override {
        if (h.v >= 16 || n == 0) return false;
        memcpy(com[h.v], s[0].com, sizeof com[h.v]);
        ++installsAt[h.v];
        return true;
    }
};

}  // namespace

TEST_CASE("TRACKMAP: the module loads the real spectator capture and refuses unprovisioned slots") {
    Env& e = TheEnv();
    const wchar_t* home = _wgetenv(L"USERPROFILE");
    wchar_t path[1024] = L"";
    if (home != nullptr) {
        _snwprintf_s(path, _TRUNCATE,
                     L"%s/Documents/EchoReplay/"
                     L"2026-09-18_03-33-45_576ED3F8428EBC4B_00000000FFFFFFFF.echodemo",
                     home);
    }
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        // Absent skips, as in test_track_map.cpp, unless ECHOREPLAY_TEST_REQUIRE_CAPTURE=1.
        if (replay_test::MissingCaptureFails()) {
            FAIL_CHECK("capture absent: %USERPROFILE%/Documents/EchoReplay/"
                       "2026-09-18_03-33-45_576ED3F8428EBC4B_00000000FFFFFFFF.echodemo");
        } else {
            WARN_MESSAGE(false, "SKIPPED: capture absent");
        }
        return;
    }

    replay::IEntity* const shipped = e.services.entity;
    SlotPawnEntity fake;
    fake.available = true;
    e.services.entity = &fake;
    e.tick.FireAll(replay::TickInfo{});

    replay::IReplayTransport* t = e.services.transport;
    const size_t mark = e.log.lines.size();
    const bool loaded = t->Load(path);
    if (!loaded) {
        e.services.entity = shipped;   // `fake` dies with this frame
        e.tick.FireAll(replay::TickInfo{});
    }
    REQUIRE_MESSAGE(loaded, "the real capture is present and did not open");
    // 15211 proved records plus the 13 player and 2 body records stored verbatim, now decoded.
    CHECK(CountLoggedSince(mark, "playback", "track scan: 15226 wire records from 4 senders") == 1);
    CHECK(CountLoggedSince(mark, "playback", "wire clock: sender slot 0 gen 0 stamps 2051951221 us") == 1);
    CHECK(CountLoggedSince(mark, "playback", "wire clock: sender slot 1 gen 0 stamps 91123921 us") == 1);
    CHECK(CountLoggedSince(mark, "playback", "wire clock: sender slot 3 gen 0 stamps 1327375929 us") == 1);
    CHECK(CountLoggedSince(mark, "playback", "wire clock: sender server (unowned)") == 1);

    // Seek is relative to t0, and the content starts 822.9 s after it (DATA-REAL-CAPTURE S4: t0 is
    // on the sim clock, the first tick at 843887448 on the peer clock). Ten seconds into the match.
    const int64_t contentStart = 843887448 - 20999790;
    const size_t seekMark = e.log.lines.size();
    t->Seek(contentStart + 10000000);  // facts-ok: CLOCK.CTOR_MAXELAPSED_INIT 10,000,000 in tick arithmetic, not the clock constructor's initialiser
    e.tick.FireAll(replay::TickInfo{});
    CHECK(CountLoggedSince(seekMark, "playback", "identity=0 clock=0 window=0") >= 1);
    CHECK(CountLoggedSince(seekMark, "playback", "window build refused") == 0);
    // Nothing here has provisioned the recorded players, so recorded slots 0, 1 and 3 have no live
    // slot. Under the identity map they would land on live 0, 1 and 3 -- and live 0 is the
    // viewer's. Every wire body is refused by name and no pawn is driven, the viewer's least of all.
    CHECK(CountLoggedSince(seekMark, "playback", "body handles re-bound: 0 bound") >= 1);
    CHECK(CountLoggedSince(seekMark, "playback", "3 slot unassigned (recorded slot ") >= 1);
    CHECK(CountLoggedSince(seekMark, "playback", "by slot 0->0") == 0);
    for (int slot = 0; slot < 16; ++slot) {
        CHECK(fake.installsAt[slot] == 0);
    }
    // The lines a launch shows for the same file, in the run's own output.
    for (size_t i = mark; i < e.log.lines.size(); ++i) {
        const std::string& text = e.log.lines[i].text;
        for (const char* needle : {"track scan:", "wire clock:", "window tracks", "body handles re-bound",
                                   "pawn slot "}) {
            if (text.find(needle) != std::string::npos) {
                printf("TRACKMAP log: %s\n", text.c_str());
                break;
            }
        }
    }

    t->Unload();
    e.services.entity = shipped;
    e.tick.FireAll(replay::TickInfo{});
}

TEST_CASE("body handles bind on the tick the entity surface becomes available, not on a rebuild") {
    Env& e = TheEnv();
    replay::IEntity* const shipped = e.services.entity;

    SwitchableEntity fake;
    e.services.entity = &fake;
    e.tick.FireAll(replay::TickInfo{});
    REQUIRE(BindingSnapshot().entity == &fake);
    CHECK_FALSE(BindingSnapshot().entity_available);

    // Loaded while the space is unbound: the window is built, no handle resolves, nothing installs.
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 2));
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));
    e.tick.FireAll(replay::TickInfo{});
    CHECK(fake.installs == 0);

    // The pointer does not change -- only Available() does -- and that alone must re-bind.
    const uint64_t rebinds = BindingSnapshot().rebinds;
    fake.available = true;
    e.tick.FireAll(replay::TickInfo{});
    CHECK(BindingSnapshot().entity_available);
    CHECK(BindingSnapshot().rebinds == rebinds + 1);
    CHECK(Logged("playback", "body handles re-bound: 2 bound"));
    CHECK(fake.installs == 2);

    e.tick.FireAll(replay::TickInfo{});
    CHECK(fake.installs == 4);

    t->Unload();
    e.services.entity = shipped;
    e.tick.FireAll(replay::TickInfo{});
    CHECK(BindingSnapshot().entity == shipped);
}


namespace {

// Slot -> index, one number apart per slot: a mapping that ignored the slot would still pass with
// a constant, so it does not use one.
uint16_t IndexForSlot(uint16_t slot) { return uint16_t(slot * 3 + 1); }

class RosterIdentity final : public replay::IIdentity {
public:
    uint16_t releases = 0;
    // THE ACTOR-HOP SURFACE THIS IDENTITY OFFERS. Null by default because that is what every case
    // written before the wire modelled -- a build that bound no hop read, whose census says
    // `hops: unasked` -- while the SHIPPED identity always answers one (identity_access.cpp:1775).
    const replay::PawnProvision::ActorHops* hopSurface = nullptr;
    const replay::PawnProvision::ActorHops* ActorHops() const override { return hopSurface; }
    // THE ENGINE'S OWN SLOT TABLE. IndexOf is the only thing PawnProvisioner asks about occupancy,
    // so a fake answering an index for every slot says the whole table is full, every recorded slot
    // is refused, and the remap under test cannot be exercised at all -- the fixture, not the code,
    // decides the result. A slot is occupied once somebody is seated in it and free before that.
    bool occupied[facts::ENTITY_USER_SLOT_TABLE_MAX] = {};
    void Seat(uint16_t slot) { occupied[slot] = true; }

    bool AddReplayUser(replay::UserSlot s, replay::UserIndex& out) override {
        occupied[s.v] = true;
        out = replay::UserIndex(IndexForSlot(s.v));
        return true;
    }
    // ARCH-23 S7.2: the dress ladder ADMITS before it looks up, and the contract's default
    // refuses.  This lobby admits where it is asked, which is what it always modelled.
    uint16_t provisionedSlot = replay::kSlotUnmapped;
    // The engine's reply route parks the entrant-add and answers ReplyWrongPhase -- a DEFERRAL,
    // not a decision -- until its own pump drains the queue. That is what every player of the
    // subject capture answered, and reading it as a refusal is the whole defect.
    uint32_t defersLeft = 0;
    // What the dress ASKED FOR, per recorded slot: a spectator row must never appear here, and a
    // pass that re-armed itself would push these counts up once per tick instead of once.
    uint32_t provisionCalls = 0;
    uint32_t askedSlot[facts::ENTITY_USER_SLOT_TABLE_MAX] = {};
    // Which ENGINE slot each leave was fed, so a case can say "these three and nobody else" -- a
    // bare count cannot tell three correct removals from two correct ones and the viewer's.
    uint32_t releasedSlot[facts::ENTITY_USER_SLOT_TABLE_MAX] = {};
    // The ORDER the leaves were fed in, and how many landed per pass. RELOAD-5's fix is an order
    // and a pacing, so a per-slot count cannot see it: bottom-up-all-at-once and top-down-one-at-a-
    // time produce identical `releasedSlot` tallies.
    uint16_t releaseOrder[facts::ENTITY_USER_SLOT_TABLE_MAX * 2] = {};
    uint16_t releaseOrderCount = 0;
    uint32_t profileWrites[facts::ENTITY_USER_SLOT_TABLE_MAX] = {};
    replay::PawnProvision::Refusal lastProvision = replay::PawnProvision::Refusal::None;
    replay::PawnProvision::Refusal LastProvisionRefusal() const override { return lastProvision; }
    bool ProvisionReplayUser(replay::UserSlot s, const replay::JoinDesc&,
                             replay::UserIndex& out) override {
        ++provisionCalls;
        if (s.v < facts::ENTITY_USER_SLOT_TABLE_MAX) ++askedSlot[s.v];
        if (defersLeft > 0) {
            --defersLeft;
            lastProvision = replay::PawnProvision::Refusal::ReplyWrongPhase;
            return false;
        }
        lastProvision = replay::PawnProvision::Refusal::None;
        // The engine's insert refuses a slot that already holds an entrant -- the (A) case the
        // provisioner's remap exists for. A fake that admitted anywhere would seat two users in one
        // slot and report success.
        if (s.v >= facts::ENTITY_USER_SLOT_TABLE_MAX || occupied[s.v]) return false;
        provisionedSlot = s.v;
        return AddReplayUser(s, out);
    }
    bool ReleaseReplayUser(replay::UserSlot s) override {
        if (s.v < facts::ENTITY_USER_SLOT_TABLE_MAX) {
            occupied[s.v] = false;
            ++releasedSlot[s.v];
        }
        if (releaseOrderCount < uint16_t(sizeof releaseOrder / sizeof releaseOrder[0]))
            releaseOrder[releaseOrderCount++] = s.v;
        ++releases;
        return true;
    }
    bool SetProfileJson(replay::UserSlot s, const replay::Blob&) override {
        if (s.v < facts::ENTITY_USER_SLOT_TABLE_MAX) ++profileWrites[s.v];
        return true;
    }
    bool SetTeam(replay::UserIndex, uint8_t, uint8_t) override { return true; }
    bool ApplyLoadout(replay::UserIndex, const replay::Symbol64[20], uint16_t,
                      uint16_t) override { return true; }
    bool OverrideAlignment(replay::UserIndex, uint16_t) override { return true; }
    bool ReadAppliedItems(replay::UserIndex, replay::Symbol64 out[20]) const override {
        memset(out, 0, sizeof(replay::Symbol64) * 20);
        return true;
    }
    replay::UserSlot  SlotOf(replay::UserIndex i) const override {
        // The engine's entrant names its OWN slot back, which is what occupancy means: this
        // answered the last provisioned slot for ANY index, so a SEATED slot's entrant named
        // nothing back and read as a row that is not there.  W2-P1 now reads an absent row as
        // FREE (L3 #17 F1), so a fake that cannot name a seat back hands the viewer's slot away.
        for (uint16_t s = 0; s < facts::ENTITY_USER_SLOT_TABLE_MAX; ++s)
            if (IndexOf(replay::UserSlot(s)).v == i.v) return replay::UserSlot(s);
        return replay::UserSlot(replay::kSlotUnmapped);
    }
    replay::UserIndex IndexOf(replay::UserSlot s) const override {
        if (s.v >= facts::ENTITY_USER_SLOT_TABLE_MAX || !occupied[s.v])
            return replay::UserIndex(replay::kSlotUnmapped);
        return replay::UserIndex(IndexForSlot(s.v));
    }
};

// Resolves a player handle only for an index the roster actually produced, so a track published
// under a wrong or defaulted owner cannot get a handle by accident.
class RosterEntity final : public SwitchableEntity {
public:
    size_t discDrops = 0;   // RELOAD-6: the fresh space must not inherit the old disc binding
    void DropDisc() override { ++discDrops; }
    replay::PlayerHandle PlayerOf(replay::UserIndex i) const override {
        if (i.v == IndexForSlot(2)) return replay::PlayerHandle{21};
        if (i.v == IndexForSlot(4)) return replay::PlayerHandle{41};
        return replay::PlayerHandle{0xFFFFu};  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    }
};

class CountingPose final : public replay::IPosePublisher {
public:
    size_t publishes = 0;
    uint16_t handles[8] = {};
    void Publish(replay::PlayerHandle h, const replay::PlayerState&,
                 const replay::PlayerState&) override {
        if (publishes < 8) handles[publishes] = h.v;
        ++publishes;
    }
    const char* Strategy() const override { return "PP-B"; }
    bool NeedsLateTick() const override { return false; }
};

class FixedBias final : public replay::IAudioRate {
public:
    int64_t bias = 0;
    bool Available() const override { return true; }
    float MaxRate() const override { return 1.0f; }
    void SetRate(float) override {}
    void Suspend(bool) override {}
    void Wakeup() override {}
    double AudioSeconds() const override { return 0.0; }
    const char* Diagnostic() const override { return ""; }
    int64_t DisplayBiasSceneUs() const override { return bias; }
    const char* RateLabel() const override { return "audio: 1.0x native"; }
};

struct ArmSpace;

// ARCH-10 section 3.4. The CncaGameSpace is the netGame member at NETGAME.CS_SPACE_OFF; the
// pointer at IDENT_NETGAME_GAMESPACE_OFF (+0x8) is the CR15Game and has never been a space. The
// buffer is sized off the fact rather than off 0x2B18, so a rig that is too small to hold the field
// cannot compile; the DECOY at +0x8 is what makes this rig unsatisfiable by a consumer still
// reading the old offset -- it is a readable, non-null object whose system table refuses.
struct ArmNetGame {
    static_assert(facts::NETGAME_CS_SPACE_OFF + sizeof(void*) >= 0x2B10u,
                  "the netGame rig must be able to hold the game space at NETGAME.CS_SPACE_OFF");
    alignas(16) uint8_t bytes[facts::NETGAME_CS_SPACE_OFF + 0x10] = {};
    // 0x7B00 is a fixture literal on purpose (section 3.3): it covers the two CR15Game candidate
    // slots entity_module's evidence line probes off the +0x8 pointer, so the probe stays inside
    // this object. Importing the module's private offsets would tie the rig to the code it feeds.
    alignas(16) uint8_t decoy[0x7B00] = {};

    ArmNetGame() {
        memset(decoy, 0xDD, sizeof decoy);
        void* p = decoy;
        memcpy(bytes + facts::IDENT_NETGAME_GAMESPACE_OFF, &p, sizeof p);
        SetState(facts::NETGAME_STATE_INMATCH);
    }
    // The session state word the provisioner latches on, at the netGame's offset 0. A rig left
    // zeroed is STATE_OFF, which is a DEFERRAL and not an error, so a fixture that forgot this
    // would wait forever and read as "the wiring is broken". In match unless a case says otherwise.
    void SetState(uint32_t state) { memcpy(bytes, &state, sizeof state); }
    void PointAt(const ArmSpace* s);
    void PointOldSlotAt(const ArmSpace* s);
};

// Publishes a roster-answering identity, an available entity and a counting pose sink, and puts
// every one of them back afterwards -- these cases run in the same process as the rest of the file.
struct RosterFixture {
    Env& e;
    RosterIdentity id;
    RosterEntity ent;
    CountingPose pose;
    // An in-match session. The dress pass admits through the netGame THIS TICK carries; without one
    // it defers, so a fixture with no rig can never dress and every case below would be measuring
    // the gate instead of the wiring.
    ArmNetGame netGame;
    replay::IIdentity* savedId;
    replay::IEntity* savedEnt;
    replay::IPosePublisher* savedPose;

    RosterFixture() : e(TheEnv()) {
        // A case is a fresh session: the previous case's entrants must not be adoptable by this
        // one's first load, or every roster that shares a capture slot with it re-binds instead of
        // admitting and no case below can observe an admission at all.
        replay::playback::ForgetSessionEntrantsForTest();
        // ...and the leave feed with it: this rig never drives the stage-3 tail that drains it,
        // so a queue left by an earlier case would defer every later case's dress pass.
        replay::identity::ForgetRosterLeave();
        savedId = e.services.identity;
        savedEnt = e.services.entity;
        savedPose = e.services.pose;
        ent.available = true;
        e.services.identity = &id;
        e.services.entity = &ent;
        e.services.pose = &pose;
        Tick();
    }
    void Tick() {
        // A real engine update feeds the leave queue before anything else this tick does; this rig
        // has no engine update, so the one pass the stage-3 tail would make is made here.
        replay::playback::ServiceLeaveFeedForTest();
        replay::TickInfo info{};
        info.netGame = netGame.bytes;
        e.tick.FireAll(info);
    }
    ~RosterFixture() {
        e.services.transport->Unload();
        e.services.identity = savedId;
        e.services.entity = savedEnt;
        e.services.pose = savedPose;
        e.tick.FireAll(replay::TickInfo{});
    }
};

// RELOAD-5's contract: the unload QUEUES the removals and the engine's own update feeds them, at
// most one per update.  Ticks until the queue is empty and answers how many updates that took, so a
// case can pin the pass count rather than settling for "eventually empty".  Bounded well above any
// roster the slot table can hold, so a queue that never drains fails the count instead of hanging.
uint16_t DrainLeaveFeed(RosterFixture& f) {
    uint16_t passes = 0;
    while (replay::identity::RosterLeavePending() && passes < 64) {
        f.Tick();
        ++passes;
    }
    return passes;
}

}  // namespace

TEST_CASE("a two-player roster maps to live user indices and both bodies publish") {
    // THE GAP THIS PACKET CLOSES. Entity, netvar and dressing were all green while every player
    // publish counted refused, because nothing carried the UserIndex from W2-C to PlayerTrack. The
    // observable is not "the mapping exists" -- it is that the poses come out.
    RosterFixture f;
    const uint16_t slots[2] = {2, 4};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 2));
    replay::IReplayTransport* t = f.e.services.transport;
    REQUIRE(t->Load(tmp.path));
f.Tick();   // the dress pass runs on a tick, not inside Load

    const IdentityStatus st = IdentitySnapshot();
    CHECK_FALSE(st.pending);
    CHECK(st.players == 2);
    CHECK(st.mapped == 2);
    CHECK(st.owner_refused == 0);
    CHECK(strstr(t->ActiveModeLabel(), "owners: 2/2 mapped") != nullptr);

    f.pose.publishes = 0;
    f.Tick();
    const replay::playback::ApplyStats a = replay::playback::ApplyStatsSnapshot();
    CHECK(a.players_published == 2);
    CHECK(a.players_refused_owner == 0);
    CHECK(a.players_unmapped == 0);
    CHECK(a.players_unhandled == 0);
    CHECK(f.pose.publishes == 2);
    // Published under the handles the ROSTER's indices resolve to, not under whatever the tracks
    // were built with: slot 2 -> index 7 -> handle 21, slot 4 -> index 13 -> handle 41.
    const bool pair = (f.pose.handles[0] == 21 && f.pose.handles[1] == 41) ||
                      (f.pose.handles[0] == 41 && f.pose.handles[1] == 21);
    CHECK(pair);

    // And a seek, which rebuilds the window from scratch, does not un-own them.
    t->Seek((int64_t)(5000000 + 5 * 11111));
    f.pose.publishes = 0;
    f.Tick();
    CHECK(replay::playback::ApplyStatsSnapshot().players_published == 2);
    CHECK(IdentitySnapshot().mapped == 2);
    // A second reading of the same call site, from the other end. `mapped` is a counter this
    // module keeps; what only RebuildWindow's ApplyRosterMapping can give the seek is tracks that
    // carry a HANDLE, and the applier's first gate is the one that reports their absence. Without
    // the call both of these move and `mapped` alone would still have to be believed.
    CHECK(replay::playback::ApplyStatsSnapshot().players_unhandled == 0);
    CHECK(replay::playback::ApplyStatsSnapshot().players_unmapped == 0);
}

TEST_CASE("a roster with spectator rows is dressed once, not re-armed on every tick") {
    // `report.players` counts the DRESSABLE rows -- spectators removed -- and the
    // mid-file roster-change test compared it against the manifest's ROW count, so on this capture
    // shape the test was true on every pass: the latches were wiped, the pacing and the progress
    // clock reset and the whole ladder re-run every frame, for the whole file.
    RosterFixture f;
    const uint16_t slots[5] = {2, 4, 5, 6, 7};
    // Three players and two spectators, which is the subject capture: a team at or above
    // IDENT.ALIGN_TEAM_LIMIT is a row the engine gives no body and the dress never admits.
    const uint16_t spec = uint16_t(replay::identity_facts::kALIGN_TEAM_LIMIT);
    const uint16_t teams[5] = {0, 1, 0, spec, spec};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 5, 0, false,
                      replay::kPlayerNoTeam16, teams));
    replay::IReplayTransport* t = f.e.services.transport;
    // Every admission defers, which is what the subject capture's engine answered and what keeps
    // the pass PENDING -- the state in which the re-arm defect runs.
    f.id.defersLeft = 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    REQUIRE(t->Load(tmp.path));
    f.Tick();

    const IdentityStatus first = IdentitySnapshot();
    CHECK(first.players == 3);              // the spectators are not part of the denominator
    CHECK(first.pending);                   // ...and the pass is still waiting on the three

    for (int i = 0; i < 40; ++i) f.Tick();

    const IdentityStatus later = IdentitySnapshot();
    // ONE ASK PER DRESSABLE ROW PER PASS, and the passes are counted by the module itself: the two
    // numbers agree only while the latch and the pass counter survive the tick.  A re-arm resets
    // `passes` to zero and wipes the latches, so under the defect this reads three asks a tick
    // against a pass count stuck at one.
    CHECK(later.passes >= first.passes);
    // Forty ticks are not forty retries: the pass is paced at 250 ms and forty ticks do not span
    // more than a couple of those windows.  Under the defect the pacing is reset by the re-arm and
    // every tick asks all three rows again -- better than a hundred asks by the end of this loop.
    CHECK(f.id.provisionCalls <= 18);
    CHECK(f.id.askedSlot[2] == f.id.provisionCalls / 3);
    CHECK(f.id.askedSlot[4] == f.id.provisionCalls / 3);
    CHECK(f.id.askedSlot[5] == f.id.provisionCalls / 3);
    CHECK(later.players == 3);
    // The spectator rows were never asked for, on any pass.
    CHECK(f.id.askedSlot[6] == 0);
    CHECK(f.id.askedSlot[7] == 0);
}

TEST_CASE("a settled roster with spectator rows is not dressed again on later ticks") {
    RosterFixture f;
    const uint16_t slots[5] = {2, 4, 5, 6, 7};
    const uint16_t spec = uint16_t(replay::identity_facts::kALIGN_TEAM_LIMIT);
    const uint16_t teams[5] = {0, 1, 0, spec, spec};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 5, 0, false,
                      replay::kPlayerNoTeam16, teams));
    replay::IReplayTransport* t = f.e.services.transport;
    REQUIRE(t->Load(tmp.path));
    f.Tick();
    // Nothing is deferred, so the pass has SETTLED and the module has stopped asking.
    REQUIRE_FALSE(IdentitySnapshot().pending);
    const uint32_t settledWrites[3] = {f.id.profileWrites[2], f.id.profileWrites[4],
                                       f.id.profileWrites[5]};
    const uint32_t settledCalls = f.id.provisionCalls;

    for (int i = 0; i < 40; ++i) f.Tick();

    // Not one further write, for the rest of the file: the roster-change test must not fire on a
    // capture whose spectator rows make the dressable count differ from the row count.
    CHECK(f.id.profileWrites[2] == settledWrites[0]);
    CHECK(f.id.profileWrites[4] == settledWrites[1]);
    CHECK(f.id.profileWrites[5] == settledWrites[2]);
    CHECK(f.id.provisionCalls == settledCalls);
    CHECK_FALSE(IdentitySnapshot().pending);
    // ...and no spectator was ever written to at all.
    CHECK(f.id.profileWrites[6] == 0);
    CHECK(f.id.profileWrites[7] == 0);
    CHECK(IdentitySnapshot().players == 3);
}

TEST_CASE("Unload hands the replay users back and empties the mapping") {
    RosterFixture f;
    const uint16_t slots[2] = {2, 4};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 2));
    replay::IReplayTransport* t = f.e.services.transport;
    REQUIRE(t->Load(tmp.path));
f.Tick();   // the dress pass runs on a tick, not inside Load
    REQUIRE(IdentitySnapshot().mapped == 2);

    f.id.releases = 0;
    f.id.releaseOrderCount = 0;
    t->Unload();
    // RELOAD-5. The unload QUEUES the leaves; the engine's own update feeds them, TOP DOWN and at
    // most ONE PER UPDATE, because the engine compacts the user table on every removal and walking
    // it any other way leaves one pooled actor record un-re-keyed. One tick is one update.
    CHECK(f.id.releases == 0);
    f.Tick();
    CHECK(f.id.releases == 1);
    CHECK(f.id.releaseOrder[0] == 4);          // the HIGHEST engine slot goes first
    CHECK(replay::identity::RosterLeavePending());
    // Unload completion is signalled only after the LAST removal: with one still queued the count
    // published to the status stands at one, not two.
    CHECK(IdentitySnapshot().released == 1);
    f.Tick();
    CHECK(f.id.releases == 2);
    CHECK(f.id.releaseOrderCount == 2);
    CHECK(f.id.releaseOrder[1] == 2);
    CHECK_FALSE(replay::identity::RosterLeavePending());   // two entrants, exactly two passes
    CHECK(IdentitySnapshot().released == 2);
    CHECK(IdentitySnapshot().release_refused == 0);
    CHECK(IdentitySnapshot().mapped == 0);

    // A second load in the same session starts from an empty mapping and re-dresses.
    f.id.releases = 0;
    REQUIRE(t->Load(tmp.path));
    CHECK(IdentitySnapshot().mapped == 2);
}

// RELOAD-5's other half: while a leave is IN FLIGHT nothing is admitted. A load that landed between
// the first and the last removal would dress into a slot whose entrant is mid-teardown -- which is
// the adoption of something about to be destroyed that the paced leave exists to prevent.
TEST_CASE("RELOAD-5: a load during the paced leave admits nothing until the last entrant is gone") {
    RosterFixture f;
    const uint16_t slots[3] = {1, 2, 3};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 3));
    replay::IReplayTransport* t = f.e.services.transport;
    REQUIRE(t->Load(tmp.path));
    f.Tick();
    REQUIRE(IdentitySnapshot().mapped == 3);

    f.id.provisionCalls = 0;
    f.id.releaseOrderCount = 0;
    t->Unload();
    f.Tick();                                   // pass 1 of 3: one removal in, two still queued
    REQUIRE(replay::identity::RosterLeavePending());
    REQUIRE(t->Load(tmp.path));
    f.Tick();                                   // pass 2 of 3, and the load's first dress attempt
    // That tick fed one more removal and then ran the dress pass, which deferred naming the leave:
    // one entrant was still queued when it looked.
    CHECK(f.id.releases == 2);
    CHECK(f.id.provisionCalls == 0);
    CHECK(IdentitySnapshot().mapped == 0);
    REQUIRE(IdentitySnapshot().defer_why != nullptr);
    CHECK(strstr(IdentitySnapshot().defer_why, "still leaving the engine") != nullptr);

    CHECK(DrainLeaveFeed(f) == 1);               // pass 3 of 3, and the queue is empty
    CHECK(f.id.releaseOrderCount == 3);
    CHECK(f.id.releaseOrder[0] == 3);
    CHECK(f.id.releaseOrder[1] == 2);
    CHECK(f.id.releaseOrder[2] == 1);
    f.Tick();
    CHECK(f.id.provisionCalls == 3);             // and only then are the fresh three admitted
    CHECK(IdentitySnapshot().mapped == 3);
}

namespace {

// RELOAD-6. The session surface the level cycle asks: a state word the case moves by hand, and a
// count of what was asked of it. No engine call exists behind it.
class CycleSession final : public replay::ISessionEntry {
public:
    int32_t state = 9;
    uint32_t cycleRequests = 0;
    replay::EntryRefusal answer = replay::EntryRefusal::Accepted;
    bool pending = false;

    bool CanChangeLevelMidSession() const override { return true; }
    bool Matches(uint64_t, uint64_t) const override { return true; }
    bool RequestEntry(uint64_t, uint64_t) override { return true; }
    replay::NetGameState State() const override { return static_cast<replay::NetGameState>(state); }
    void* NetGame() const override { return nullptr; }
    const char* Strategy() const override { return "fake"; }
    const char* FailureReason() const override { return "the fake session said no"; }
    replay::EntryRefusal RequestLevelCycle() override {
        ++cycleRequests;
        if (answer == replay::EntryRefusal::Accepted) pending = true;
        return answer;
    }
    bool EntryPending() const override { return pending; }
};

// A loaded three-player roster with the fake session published, put back on the way out.
struct CycleFixture {
    RosterFixture f;
    CycleSession session;
    replay::ISessionEntry* saved;
    TempDemo tmp;
    replay::IReplayTransport* t;

    CycleFixture() {
        saved = f.e.services.session;
        f.e.services.session = &session;
        const uint16_t slots[3] = {1, 2, 3};
        REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 3));
        t = f.e.services.transport;
        REQUIRE(t->Load(tmp.path));
        f.Tick();
        REQUIRE(IdentitySnapshot().mapped == 3);
        REQUIRE(session.cycleRequests == 0);   // a first load never cycles
        f.id.provisionCalls = 0;
    }
    ~CycleFixture() {
        t->Unload();
        f.e.config.reloadCycle = true;
        f.e.services.session = saved;
    }
};

}  // namespace

TEST_CASE("RELOAD-6: a reload leaves and re-enters the level before the new roster is admitted") {
    CycleFixture c;
    const size_t from = TheEnv().log.lines.size();
    REQUIRE(c.t->Load(c.tmp.path));
    CHECK(CountLoggedSince(from, "playback", "level cycle #") == 1);   // the armed line

    // The paced leave runs first; the request goes out on the tick the last entrant has left.
    CHECK(DrainLeaveFeed(c.f) == 3);
    CHECK(c.session.cycleRequests == 1);
    CHECK(c.f.id.provisionCalls == 0);

    c.f.Tick();                         // accepted, the engine still in game
    CHECK(c.f.id.provisionCalls == 0);
    // Nothing of this load is played while its level is re-entered: the playhead stands still.
    c.t->Play(1.0f);
    const int64_t heldAt = c.t->PlayheadUs();
    Sleep(40);
    c.f.Tick();
    CHECK(c.t->PlayheadUs() == heldAt);
    c.session.state = 4;                // loading global: the old space is gone
    c.f.Tick();
    c.session.state = 7;
    c.f.Tick();
    // The fresh space binding re-dresses from RebindServices, above the hold; the dress gate holds it.
    c.f.ent.available = false;
    c.f.Tick();
    c.f.ent.available = true;
    c.f.Tick();
    CHECK(c.f.id.provisionCalls == 0);
    CHECK(IdentitySnapshot().mapped == 0);
    CHECK(c.session.cycleRequests == 1);   // asked once, never again while it runs

    const size_t dropsBefore = c.f.ent.discDrops;
    c.session.state = 9;                // in game, in the fresh space
    c.f.Tick();
    CHECK(c.f.ent.discDrops == dropsBefore + 1);   // every fact of the old space is dropped
    CHECK(CountLoggedSince(from, "playback", "reload: level cycle #") >= 3);
    CHECK(CountLoggedSince(from, "playback", "done in") == 1);
    CHECK(c.f.id.provisionCalls == 3);  // admitted on the tick the hold ended, as a first load
    CHECK(IdentitySnapshot().mapped == 3);
    Sleep(40);
    c.f.Tick();
    CHECK(c.t->PlayheadUs() > heldAt);   // and plays once it is in
}

TEST_CASE("RELOAD-6: a refused level cycle reopens in place and says so") {
    CycleFixture c;
    c.session.answer = replay::EntryRefusal::CannotEnter;
    const size_t from = TheEnv().log.lines.size();
    REQUIRE(c.t->Load(c.tmp.path));
    const size_t drops = c.f.ent.discDrops;
    CHECK(DrainLeaveFeed(c.f) == 3);
    CHECK(c.session.cycleRequests == 1);
    CHECK(CountLoggedSince(from, "playback", "FAILED (request-refused)") == 1);
    CHECK(c.f.ent.discDrops == drops);   // the level was never left: nothing of it is dropped
    CHECK(CountLoggedSince(from, "playback", "the fake session said no") == 1);
    CHECK(c.f.id.provisionCalls == 3);
    CHECK(IdentitySnapshot().mapped == 3);
}

TEST_CASE("RELOAD-6: with playback.reload_via_level_cycle off the reload is in place, as before") {
    CycleFixture c;
    c.f.e.config.reloadCycle = false;
    const size_t from = TheEnv().log.lines.size();
    REQUIRE(c.t->Load(c.tmp.path));
    CHECK(CountLoggedSince(from, "playback", "reload_via_level_cycle is off") == 1);
    CHECK(DrainLeaveFeed(c.f) == 3);
    c.f.Tick();
    CHECK(c.session.cycleRequests == 0);
    CHECK(c.f.id.provisionCalls == 3);
}

TEST_CASE("RELOAD-6: a reload while the session is not in game waits like a first load, no cycle") {
    CycleFixture c;
    c.session.state = 7;
    const size_t from = TheEnv().log.lines.size();
    REQUIRE(c.t->Load(c.tmp.path));
    CHECK(CountLoggedSince(from, "playback", "no level cycle") == 1);
    CHECK(DrainLeaveFeed(c.f) == 3);
    c.f.Tick();
    CHECK(c.session.cycleRequests == 0);
}

TEST_CASE("RELOAD-6: an unload mid-cycle cancels it, and the next load is a first load") {
    CycleFixture c;
    const size_t from = TheEnv().log.lines.size();
    REQUIRE(c.t->Load(c.tmp.path));
    CHECK(DrainLeaveFeed(c.f) == 3);
    c.session.state = 4;
    c.f.Tick();
    c.t->Unload();
    CHECK(CountLoggedSince(from, "playback", "cancelled after") == 1);
    CHECK(DrainLeaveFeed(c.f) == 0);   // nothing was admitted, so nothing leaves
    c.session.state = 9;
    c.session.pending = false;
    REQUIRE(c.t->Load(c.tmp.path));
    c.f.Tick();
    CHECK(c.session.cycleRequests == 1);
    CHECK(c.f.id.provisionCalls == 3);
}

TEST_CASE("the display playhead is biased by the audio buffering, flat in scene time") {
    // S10.4.1a. The sink's buffering is constant in SCENE time, so ONE subtraction serves every
    // rate -- and the wall-time offset a viewer perceives is bias/r, which is the number that
    // grows: 85 ms at 1x, 170 ms at 0.5x, 853 ms at 0.1x. A per-rate table would be the bug.
    RosterFixture f;
    FixedBias rate;
    rate.bias = 85000;
    replay::IAudioRate* const saved = f.e.services.audioRate;
    f.e.services.audioRate = &rate;

    TempDemo tmp;
    const uint64_t t0 = 5000000;
    REQUIRE(WriteDemo(tmp.path, t0, 11111, 60, 2, 1));
    replay::IReplayTransport* t = f.e.services.transport;
    REQUIRE(t->Load(tmp.path));
f.Tick();   // the dress pass runs on a tick, not inside Load

    // Well inside the file, so the origin clamp is not what is being measured.
    t->Seek((int64_t)(t0 + 30 * 11111));
    f.Tick();
    replay::playback::PresentSnapshot p = replay::playback::LastPresent();
    CHECK(p.bias_us == 85000);
    CHECK(p.presented_us == p.transport_us - 85000);

    // Half speed: the SCENE subtraction is unchanged, and that is the claim. The wall-time offset
    // it corrects is bias/r = 170 ms -- computed from the same one number, never stored twice.
    t->Play(0.5f);
    f.Tick();
    p = replay::playback::LastPresent();
    CHECK(p.bias_us == 85000);
    CHECK(p.presented_us == p.transport_us - 85000);
    CHECK((int64_t)(p.bias_us / 0.5) == 170000);
    CHECK((int64_t)(p.bias_us / 0.1) == 850000);

    // Paused: the transport does not advance and neither does the presented time, so the bias is
    // held rather than re-applied into a drift.
    t->Pause();
    f.Tick();
    const replay::playback::PresentSnapshot a = replay::playback::LastPresent();
    f.Tick();
    const replay::playback::PresentSnapshot b = replay::playback::LastPresent();
    CHECK(a.transport_us == b.transport_us);
    CHECK(a.presented_us == b.presented_us);

    // Zero until W2-H's measurement latches, and zero must mean "no correction", not "no picture".
    rate.bias = 0;
    f.Tick();
    p = replay::playback::LastPresent();
    CHECK(p.presented_us == p.transport_us);

    // The rate plan's own label reaches the panel; until now the string existed with no printer.
    CHECK(strstr(t->ActiveModeLabel(), "audio: 1.0x native") != nullptr);

    f.e.services.audioRate = saved;
}

namespace {

namespace lp = replay::localpawn_facts;

// The game space as the engine's CTable constructor leaves it, so the REAL W1-E module can bind to
// it from this process. Every offset is a named fact rather than a literal: a fixture with its own
// copy of the layout is a fixture that certifies a layout nothing else uses.
struct ArmSpace {
    alignas(16) uint8_t bytes[lp::kGameSpaceSystemTableOff + 0x40] = {};
    alignas(16) uint8_t elements[4 * (size_t)lp::kSystemTableStride] = {};
    uint64_t vtable[8] = {};
    uint64_t allocatorObject = reinterpret_cast<uintptr_t>(vtable);

    void Put(uint32_t off, uint64_t v) {
        memcpy(bytes + lp::kGameSpaceSystemTableOff + off, &v, sizeof v);
    }
    void Construct(uint8_t fill) {
        memset(elements, fill, sizeof elements);
        Put(0x00, reinterpret_cast<uintptr_t>(elements));
        Put(0x08, 4 * lp::kSystemTableStride);
        Put(0x10, reinterpret_cast<uintptr_t>(&allocatorObject));
        Put(0x20, 4);
        Put(0x28, 4);
        Put(0x30, 1);
        Put(0x38, 0);
    }
};

inline void ArmNetGame::PointAt(const ArmSpace* s) {
    void* p = s ? const_cast<uint8_t*>(s->bytes) : nullptr;
    memcpy(bytes + facts::NETGAME_CS_SPACE_OFF, &p, sizeof p);
}
// The mirror rig: a real, constructed space planted where the CR15Game lives and nowhere else.
inline void ArmNetGame::PointOldSlotAt(const ArmSpace* s) {
    void* p = s ? const_cast<uint8_t*>(s->bytes) : nullptr;
    memcpy(bytes + facts::IDENT_NETGAME_GAMESPACE_OFF, &p, sizeof p);
}

// Drives the REAL entity module -- the one this target links and modreg::InitAll already ran -- so
// the transition under test is the one the shipping module produces, not one a test can only
// produce by writing a different pointer into ReplayServices.
void TickEntityModule(void* netGame) {
    replay::TickInfo info{};
    info.frameIndex = 1;
    info.netGame = netGame;
    replay::modreg::TickAll(info);
}

}  // namespace

TEST_CASE("a rebuilt game space invalidates the mapping, and only the bind epoch can see it") {
    // AUDIT_WIRING_A F-A. The previous version of this case drove the transition with
    // `services.entity = &second` -- a different IEntity*, which the shipping module CANNOT
    // produce: entity.cpp's implementation is one `static Entity` for the process. So it was green
    // whether or not the protection was reachable. This one drives W1-E's real module, whose Tick
    // re-binds a changed space by calling Bind() with no Unbind: the pointer does not move, and
    // Available() is false on both sides of the change (no echovr.exe is mapped on a test host, so
    // every EntityCreate refuses at the site guard). Both of the terms this block used to key on
    // are therefore constant across the step that matters, and only the epoch moves.
    RosterFixture f;
    const uint16_t slots[2] = {2, 4};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 2));
    replay::IReplayTransport* t = f.e.services.transport;
    REQUIRE(t->Load(tmp.path));
f.Tick();   // the dress pass runs on a tick, not inside Load
    REQUIRE(IdentitySnapshot().mapped == 2);
    REQUIRE(BindingSnapshot().entrants == 2);

    // Hand playback the real singleton, bound to a first space. This step DOES move the pointer --
    // it is setup, not the assertion.
    ArmSpace first;
    first.Construct(0xA1);
    ArmNetGame netGame;
    netGame.PointAt(&first);
    TickEntityModule(netGame.bytes);
    f.Tick();

    const BindingStatus bound = BindingSnapshot();
    REQUIRE(bound.entity == f.e.services.entity);
    REQUIRE(bound.entity_bind_state == replay::IEntity::BindEpoch::Ok);
    REQUIRE_FALSE(bound.entity_available);
    // The indices the old space issued are gone, and so are the INV-OWN1 registrations they were
    // admitted under -- the guard is re-armed, not merely left holding them.
    CHECK(IdentitySnapshot().mapped == 0);
    CHECK(bound.entrants == 0);
    CHECK(IdentitySnapshot().pending);

    // Some ticks with nothing changing: no epoch, no re-bind, and `deferred` accumulates so that
    // the reset below is a number that could not have been there already.
    for (int i = 0; i < 4; ++i) {
        TickEntityModule(netGame.bytes);
        f.Tick();
    }
    const BindingStatus held = BindingSnapshot();
    CHECK(held.entity_bind_epoch == bound.entity_bind_epoch);
    CHECK(held.rebinds == bound.rebinds);
    CHECK(held.owner_resets == bound.owner_resets);
    const uint32_t deferredBefore = IdentitySnapshot().deferred;
    CHECK(deferredBefore >= 4);

    // ★ THE STEP. A different game space. Same IEntity*, same Available() == false, same everything
    // the old predicate could read.
    const size_t mark = TheEnv().log.lines.size();
    ArmSpace second;
    second.Construct(0xB2);
    netGame.PointAt(&second);
    TickEntityModule(netGame.bytes);
    REQUIRE(f.e.services.entity == held.entity);
    REQUIRE_FALSE(f.e.services.entity->Available());
    f.Tick();

    const BindingStatus after = BindingSnapshot();
    CHECK(after.entity_bind_epoch == held.entity_bind_epoch + 1);
    CHECK(after.rebinds == held.rebinds + 1);
    CHECK(after.owner_resets == held.owner_resets + 1);
    CHECK(CountLoggedSince(mark, "playback",
                           "the roster's user indices belonged to the previous game space") == 1);
    // `deferred` is reset with the rest of the identity status and re-counted from zero by the two
    // TryDressRoster calls this step runs (the re-bind's own, then the tick's), so it is 2 -- a
    // number it could not already have been holding.
    CHECK(IdentitySnapshot().deferred < deferredBefore);
    CHECK(IdentitySnapshot().deferred == 2);

    // And the same-block rebuild -- W1-E's digest case, the transition the space ADDRESS cannot
    // see either. Same base, same capacity, same count, different systems inside it.
    const size_t mark2 = TheEnv().log.lines.size();
    second.Construct(0xC3);
    TickEntityModule(netGame.bytes);
    f.Tick();
    CHECK(BindingSnapshot().entity_bind_epoch == after.entity_bind_epoch + 1);
    CHECK(CountLoggedSince(mark2, "playback",
                           "the roster's user indices belonged to the previous game space") == 1);

    TickEntityModule(nullptr);
    f.e.services.entity = &f.ent;
    f.Tick();
}

// The must-fail control for ARCH-10 section 3.4. The rig above passes only because the module reads
// NETGAME.CS_SPACE_OFF, and this is what proves that: the SAME constructed space, planted at
// IDENT_NETGAME_GAMESPACE_OFF -- where every rig planted it before this change -- and nowhere else.
// A module still reading +0x8 would arm here exactly as it arms above. It must not, and the reason
// must be the ruling's: the slot the binding reads held null, not "the space was bad".
TEST_CASE("a game space planted only at the CR15Game back-pointer does not arm the binding") {
    RosterFixture f;
    ArmSpace good;
    good.Construct(0xD4);
    ArmNetGame netGame;              // +0x2B08 stays null; the decoy at +0x8 is replaced below
    netGame.PointOldSlotAt(&good);

    const size_t mark = TheEnv().log.lines.size();
    TickEntityModule(netGame.bytes);
    f.Tick();

    CHECK(BindingSnapshot().entity_bind_state != replay::IEntity::BindEpoch::Ok);

    // "unbound game space" contains "bound game space", so the absence of a bind is counted on the
    // start of the line rather than on a substring that its own opposite would satisfy.
    size_t bindLines = 0;
    std::string candidates;
    const std::vector<Line>& lines = TheEnv().log.lines;
    for (size_t i = mark; i < lines.size(); ++i) {
        if (lines[i].module != "entity") continue;
        if (lines[i].text.rfind("bound game space", 0) == 0) ++bindLines;
        if (candidates.empty() &&
            lines[i].text.rfind("cs-space candidates:", 0) == 0) {
            candidates = lines[i].text;
        }
    }
    CHECK(bindLines == 0);

    // The true reason, read off the module's own evidence line: the slot it binds on rendered null
    // while the CR15Game slot rendered a Ready table. No other cause prints that pair.
    REQUIRE_FALSE(candidates.empty());
    CHECK(candidates.find("+0x2B08=null") != std::string::npos);
    const size_t oldSlot = candidates.find(" | +0x8=");
    REQUIRE(oldSlot != std::string::npos);
    const size_t nextSlot = candidates.find(" | +0x2B08=", oldSlot);
    REQUIRE(nextSlot != std::string::npos);
    CHECK(candidates.substr(oldSlot, nextSlot - oldSlot).find("Ready") != std::string::npos);

    TickEntityModule(nullptr);
    f.e.services.entity = &f.ent;
    f.Tick();
}

namespace {

// A roster whose capture slots map to the user indices 0 and 1 -- IndexForSlot cannot reach 0, and
// 0 is the whole point: it is a legal engine user index that the placeholder used to eat.
class ZeroIndexIdentity final : public replay::IIdentity {
public:
    bool AddReplayUser(replay::UserSlot s, replay::UserIndex& out) override {
        out = replay::UserIndex(s.v);
        return true;
    }
    uint16_t provisionedSlot = replay::kSlotUnmapped;
    // The engine's own slot table, as IndexOf reports it -- the only thing PawnProvisioner asks
    // about occupancy. A fake answering an index for every slot says the table is full and no
    // recorded slot can be admitted anywhere; one that admits an occupied slot seats two users in
    // it and calls that success. Neither is a lobby.
    bool occupied[facts::ENTITY_USER_SLOT_TABLE_MAX] = {};
    bool ProvisionReplayUser(replay::UserSlot s, const replay::JoinDesc&,
                             replay::UserIndex& out) override {
        if (s.v >= facts::ENTITY_USER_SLOT_TABLE_MAX || occupied[s.v]) return false;
        provisionedSlot = s.v;
        occupied[s.v] = true;
        return AddReplayUser(s, out);
    }
    bool ReleaseReplayUser(replay::UserSlot s) override {
        if (s.v < facts::ENTITY_USER_SLOT_TABLE_MAX) occupied[s.v] = false;
        return true;
    }
    bool SetProfileJson(replay::UserSlot, const replay::Blob&) override { return true; }
    bool SetTeam(replay::UserIndex, uint8_t, uint8_t) override { return true; }
    bool ApplyLoadout(replay::UserIndex, const replay::Symbol64[20], uint16_t,
                      uint16_t) override { return true; }
    bool OverrideAlignment(replay::UserIndex, uint16_t) override { return true; }
    bool ReadAppliedItems(replay::UserIndex, replay::Symbol64 out[20]) const override {
        memset(out, 0, sizeof(replay::Symbol64) * 20);
        return true;
    }
    replay::UserSlot  SlotOf(replay::UserIndex i) const override {
        // The engine's entrant names its OWN slot back, which is what occupancy means: this
        // answered the last provisioned slot for ANY index, so a SEATED slot's entrant named
        // nothing back and read as a row that is not there.  W2-P1 now reads an absent row as
        // FREE (L3 #17 F1), so a fake that cannot name a seat back hands the viewer's slot away.
        for (uint16_t s = 0; s < facts::ENTITY_USER_SLOT_TABLE_MAX; ++s)
            if (IndexOf(replay::UserSlot(s)).v == i.v) return replay::UserSlot(s);
        return replay::UserSlot(replay::kSlotUnmapped);
    }
    replay::UserIndex IndexOf(replay::UserSlot s) const override {
        if (s.v >= facts::ENTITY_USER_SLOT_TABLE_MAX || !occupied[s.v])
            return replay::UserIndex(replay::kSlotUnmapped);
        return replay::UserIndex(s.v);
    }
};

class LocalUserEntity final : public SwitchableEntity {
public:
    replay::IEntity::LocalUser answer = replay::IEntity::LocalUser::NotAvailable;
    uint16_t index = 0;

    LocalUser LocalUserIndex(replay::UserIndex& out) const override {
        if (answer != LocalUser::Ok) return answer;
        out = replay::UserIndex(index);
        return LocalUser::Ok;
    }
    replay::PlayerHandle PlayerOf(replay::UserIndex i) const override {
        return replay::PlayerHandle{(uint16_t)(50 + i.v)};
    }
};

// Publishes a local-user-answering entity for one case and puts the rest of the file's back.
struct LocalUserFixture {
    Env& e;
    ZeroIndexIdentity id;
    LocalUserEntity ent;
    CountingPose pose;
    ArmNetGame netGame;   // in match: without one the dress pass defers and nothing is mapped
    replay::IIdentity* savedId;
    replay::IEntity* savedEnt;
    replay::IPosePublisher* savedPose;

    LocalUserFixture() : e(TheEnv()) {
        replay::playback::ForgetSessionEntrantsForTest();
        // ...and the leave feed with it: this rig never drives the stage-3 tail that drains it,
        // so a queue left by an earlier case would defer every later case's dress pass.
        replay::identity::ForgetRosterLeave();   // fresh session -- see RosterFixture
        savedId = e.services.identity;
        savedEnt = e.services.entity;
        savedPose = e.services.pose;
        ent.available = true;
        e.services.identity = &id;
        e.services.entity = &ent;
        e.services.pose = &pose;
        Tick();
    }
    void Tick() {
        // A real engine update feeds the leave queue before anything else this tick does; this rig
        // has no engine update, so the one pass the stage-3 tail would make is made here.
        replay::playback::ServiceLeaveFeedForTest();
        replay::TickInfo info{};
        info.netGame = netGame.bytes;
        e.tick.FireAll(info);
    }
    ~LocalUserFixture() {
        e.services.transport->Unload();
        e.services.identity = savedId;
        e.services.entity = savedEnt;
        e.services.pose = savedPose;
        e.tick.FireAll(replay::TickInfo{});
    }
};

}  // namespace

TEST_CASE("INV-OWN1 refuses the engine's own local index 0, and admits a replay player at 0 when there is none") {
    // AUDIT_WIRING_A OPEN-1. The guard used to be armed `Reset(0, 0xFFFFFFFF)` -- a placeholder, not
    // an answer -- and Classify refuses `id == local_`, so a GENUINE user index 0 was refused as
    // is-local-user and that player's body never published. Both halves are asserted here, because
    // either alone would pass with the placeholder or with the check deleted.
    LocalUserFixture f;
    const uint16_t slots[2] = {0, 1};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 2));
    replay::IReplayTransport* t = f.e.services.transport;

    SUBCASE("the engine says the local user IS index 0") {
        f.ent.answer = replay::IEntity::LocalUser::Ok;
        f.ent.index = 0;
        REQUIRE(t->Load(tmp.path));
        f.Tick();   // the dress pass runs on a tick, not inside Load

        const BindingStatus b = BindingSnapshot();
        CHECK(b.local_user_known);
        CHECK(b.local_user_id == 0);
        CHECK(b.entrants == 1);          // index 1 only; index 0 is us
        CHECK(Logged("playback", "INV-OWN1 armed against this session's own local user index 0"));

        const IdentityStatus st = IdentitySnapshot();
        CHECK(st.mapped == 1);
        CHECK(st.owner_refused == 1);

        f.pose.publishes = 0;
        f.Tick();
        const replay::playback::ApplyStats a = replay::playback::ApplyStatsSnapshot();
        CHECK(a.players_published == 1);
        // Refused at admission, so the track never got a handle either: the applier's first gate
        // is the one that reports it, and `players_refused_owner` is for a track that HAS a handle
        // and a user id and is then turned away at publish.
        CHECK(a.players_unhandled == 1);
        CHECK(a.players_refused_owner == 0);
        CHECK(f.pose.publishes == 1);
        CHECK(f.pose.handles[0] == 51);   // index 1, not index 0
    }

    SUBCASE("the engine has no local user index to give") {
        // NotAvailable is not 0. Nothing is refused on that arm, the fact is logged, and the replay
        // player at index 0 -- which the placeholder ate -- publishes.
        f.ent.answer = replay::IEntity::LocalUser::NotAvailable;
        REQUIRE(t->Load(tmp.path));
        f.Tick();   // the dress pass runs on a tick, not inside Load

        const BindingStatus b = BindingSnapshot();
        CHECK_FALSE(b.local_user_known);
        CHECK(b.entrants == 2);
        CHECK(Logged("playback", "INV-OWN1 armed with NO local user index"));

        const IdentityStatus st = IdentitySnapshot();
        CHECK(st.mapped == 2);
        CHECK(st.owner_refused == 0);

        f.pose.publishes = 0;
        f.Tick();
        const replay::playback::ApplyStats a = replay::playback::ApplyStatsSnapshot();
        CHECK(a.players_published == 2);
        CHECK(a.players_refused_owner == 0);
        const bool pair = (f.pose.handles[0] == 50 && f.pose.handles[1] == 51) ||
                          (f.pose.handles[0] == 51 && f.pose.handles[1] == 50);
        CHECK(pair);
    }
}

TEST_CASE("the pose drive stops on the tick the match ends and resumes with the next match") {
    // The owner leaves, or quits, with the replay still loaded. The applier writes through
    // component-system pointers cached at bind time, so `loaded` alone kept it writing into a
    // session the engine was tearing down.
    LocalUserFixture f;
    const uint16_t slots[2] = {0, 1};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 2));
    f.ent.answer = replay::IEntity::LocalUser::NotAvailable;
    REQUIRE(f.e.services.transport->Load(tmp.path));
    f.Tick();

    // The control: in the match, every tick publishes both players.
    f.pose.publishes = 0;
    f.Tick();
    REQUIRE(f.pose.publishes == 2);

    SUBCASE("the state word leaves in-match") {
        f.netGame.SetState(facts::NETGAME_STATE_INMATCH - 1);   // ready-for-game
        f.pose.publishes = 0;
        f.Tick();
        f.Tick();
        CHECK(f.pose.publishes == 0);

        f.netGame.SetState(facts::NETGAME_STATE_INMATCH);
        f.Tick();
        CHECK(f.pose.publishes == 2);
    }

    SUBCASE("the netgame is gone") {
        f.pose.publishes = 0;
        f.e.tick.FireAll(replay::TickInfo{});
        f.e.tick.FireAll(replay::TickInfo{});
        CHECK(f.pose.publishes == 0);
    }
}

TEST_CASE("INV-OWN1: a Load before the local player is seated arms on the tick he is, and the "
          "roster is re-validated against him then") {
    // The entity is bound and READABLE the whole way through this case --
    // Available() never changes, the pointer never changes, the bind epoch never changes -- and the
    // only thing that moves is the broadcaster pool's answer. That is the real shape: Entity's
    // Available() is decided without the broadcaster pool, so `None` is what a bound space says
    // while the local player has not been seated yet. The old code asked at Load, got `None`, and
    // never asked again for the rest of the session.
    LocalUserFixture f;
    const uint16_t slots[2] = {0, 1};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 2));
    replay::IReplayTransport* t = f.e.services.transport;

    f.ent.answer = replay::IEntity::LocalUser::None;
    REQUIRE(t->Load(tmp.path));

    BindingStatus b = BindingSnapshot();
    REQUIRE_FALSE(b.local_user_known);
    CHECK(b.local_user_state == replay::IEntity::LocalUser::None);
    // C2's half: the reason is on the line the operator reads, not only in the log.
    CHECK(strstr(t->ActiveModeLabel(), "INV-OWN1 local-unknown: none") != nullptr);

    replay::IEntity* const entityAtLoad = b.entity;
    const uint64_t rebindsAtLoad = b.rebinds;
    const uint64_t resetsAtLoad = b.owner_resets;

    // A tick with the pool still empty: still unknown, and the wait is counted rather than lost.
    f.Tick();
    b = BindingSnapshot();
    CHECK_FALSE(b.local_user_known);
    CHECK(b.local_unknown_ticks == 1);
    // Dressed on THIS tick, not inside Load: the admission needs the session a tick carries, and
    // the arm is still off, which is what leaves the re-validation below something to catch.
    CHECK(b.entrants == 2);                     // both replay players admitted, arm off
    CHECK(IdentitySnapshot().mapped == 2);
    CHECK(IdentitySnapshot().owner_refused == 0);
    CHECK(b.owner_resets == resetsAtLoad);      // asked, not re-armed: there was still no answer

    // The local player is seated -- at index 0, which this replay also carries. Marked, because the
    // file's earlier cases log the same refusal text and a whole-log search would pass on theirs.
    const size_t mark = TheEnv().log.lines.size();
    f.ent.answer = replay::IEntity::LocalUser::Ok;
    f.ent.index = 0;
    f.pose.publishes = 0;
    f.Tick();

    b = BindingSnapshot();
    CHECK(b.local_user_known);
    CHECK(b.local_user_id == 0);
    CHECK(b.owner_resets == resetsAtLoad + 1);
    CHECK(CountLoggedSince(mark, "playback",
                           "INV-OWN1 armed against local index 0 after 2 ticks") == 1);
    // ★ Nothing else moved. If any of these had, the re-arm could be the old rebind-edge path
    // firing rather than the tick's re-ask, and this case would prove nothing about C1.
    CHECK(b.entity == entityAtLoad);
    CHECK(b.entity_available);
    CHECK(b.rebinds == rebindsAtLoad);

    // The re-validation, which is the half that a "record the id" fix would not do: the entrant
    // admitted while the arm was off is refused NOW, counted, and named.
    CHECK(b.entrants == 1);
    const IdentityStatus st = IdentitySnapshot();
    CHECK(st.mapped == 1);
    CHECK(st.owner_refused == 1);
    CHECK(CountLoggedSince(mark, "playback", "REFUSED by INV-OWN1: is-local-user") == 1);
    CHECK(CountLoggedSince(mark, "playback",
                           "INV-OWN1 re-validated the loaded roster against local index 0") == 1);
    CHECK(strstr(t->ActiveModeLabel(), "local-unknown") == nullptr);

    // And it took effect on that same tick: the body that would have been locally controlled did
    // not publish, and the other one did.
    const replay::playback::ApplyStats a = replay::playback::ApplyStatsSnapshot();
    CHECK(a.players_published == 1);
    CHECK(f.pose.publishes == 1);
    CHECK(f.pose.handles[0] == 51);             // index 1; index 0 is us
}

// =============================================================================================
// L3-RUN-3 F1 and F5, through the REAL module, on a file with more chapters than a window holds.
//
// ★★★ WHY THIS COULD NOT FAIL BEFORE. Every seek case in this file writes ONE chapter (INV-AXIS)
// or TWO (the audio bias case). A window spans THREE, so the whole file was always resident and a
// transport that could only reach the resident window looked perfect. The launch used a 102-chapter
// capture and every absolute seek landed on 2133312 us -- the edge of the load-time window -- on
// both an 80 s and a 120 s file, while sequential playback traversed all of it. The fixture, not
// the product, is what had been passing.
// =============================================================================================

TEST_CASE("F1: the whole file is seekable, not just the chapters resident at load") {
    Env& e = TheEnv();
    TempDemo tmp;
    const uint64_t t0 = 5000000;      // NOT zero, for the reason INV-AXIS states
    const uint64_t dt = 11111;
    const uint32_t per_chapter = 180;
    const uint32_t chapters = 12;
    REQUIRE(WriteDemo(tmp.path, t0, dt, per_chapter, chapters, 1));

    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));
    const int64_t duration = (int64_t)t->DurationUs();
    const int64_t resident = (int64_t)(dt * per_chapter * 3);
    // The fixture must be able to fail: the recording has to be materially longer than the three
    // chapters a window holds, or a transport clamped to its window still answers correctly.
    REQUIRE(duration > resident * 2);

    int64_t previous = -1;
    for (int n = 1; n <= 8; ++n) {
        const int64_t target = duration * n / 8;
        INFO("eighth ", n, " -> ", target, " us of ", duration);
        t->Seek(target);
        const int64_t landed = t->PlayheadUs();
        // Snapped DOWN to a recorded tick -- never clamped to the end of whatever was resident.
        CHECK(landed <= target);
        CHECK(landed > target - (int64_t)dt * 2);
        // Every seek reaching the SAME value is the launch's exact signature.
        CHECK(landed > previous);
        previous = landed;
    }
    CHECK(previous > resident);

    // Backwards over the same span: the window has to follow the playhead in both directions, and
    // a rebuild that only ever moved forward would pass the loop above on its own.
    for (int n = 7; n >= 1; --n) {
        const int64_t target = duration * n / 8;
        INFO("back to eighth ", n);
        t->Seek(target);
        const int64_t landed = t->PlayheadUs();
        CHECK(landed <= target);
        CHECK(landed > target - (int64_t)dt * 2);
        CHECK(landed < previous);
        previous = landed;
    }

    // Out of range in both directions clamps to the file, and the file is the whole file.
    t->Seek(duration * 4);
    CHECK(t->PlayheadUs() > resident);
    CHECK(t->PlayheadUs() <= duration);
    t->Seek(-duration);
    CHECK(t->PlayheadUs() == 0);
}

TEST_CASE("F5: reverse at the first frame cannot show a position the file does not contain") {
    Env& e = TheEnv();
    TempDemo tmp;
    const uint64_t t0 = 5000000;
    REQUIRE(WriteDemo(tmp.path, t0, 11111, 90, 3, 1));

    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));
    t->Seek(0);
    REQUIRE(t->PlayheadUs() == 0);

    t->Play(-1.0f);
    for (int i = 0; i < 16; ++i) {
        e.tick.FireAll(replay::TickInfo{});
        INFO("reverse frame ", i, " playhead ", t->PlayheadUs());
        // The launch read -20880 us here, and reported it as context rather than as the finding.
        // There is nothing behind the first frame; a negative playhead is not a place.
        CHECK(t->PlayheadUs() >= 0);
    }
    t->Pause();
    CHECK(t->PlayheadUs() >= 0);
}

// ---------------------------------------------------------------------------------------------
// ★★★ THE FIVE SHIPPED SEAMS, OBSERVED ON THE FAR SIDE OF THE INTERFACE.
//
// c2d5471 wired five calls into the engine's surfaces and committed no test, and the five counters
// it added beside them cannot stand in for one: `entity_->ClearSyntheticOwner(tr.handle); ++released;`
// keeps counting when the call is deleted, because what the counter records is this module reaching
// the line. Every assertion below is therefore made on a RECORDING FAKE -- what the clock and the
// entity actually RECEIVED -- and every one of them goes red when its call is removed.
//
// The fakes are installed into ReplayServices the way LocalUserFixture installs its own, and each
// fixture unloads first: F5 leaves a file loaded, and the LOD seam's first write depends on whether
// one is.
// ---------------------------------------------------------------------------------------------

namespace {

// W2-D's clock surface. Only SetStepUs is written by playback_module.cpp (the transport owns rate
// and pause through its own module), so only SetStepUs records.
class RecordingClock final : public replay::IReplayClock {
public:
    std::vector<uint64_t> steps;
    // The pre-roll's own seam, recorded the same way and for the same reason: `preroll active` as a
    // module counter would keep counting with the call deleted.
    struct PreRollCall {
        bool active;
        float rate;
    };
    std::vector<PreRollCall> prerolls;

    void SetStepUs(uint64_t stepUs) override { steps.push_back(stepUs); }
    void SetPreRoll(bool active, float rate) override {
        PreRollCall c;
        c.active = active;
        c.rate = rate;
        prerolls.push_back(c);
    }
    void SetPlayhead(int64_t) override {}
    int64_t Playhead() const override { return 0; }
    void EnterBurst() override {}
    void PetWatchdog() override {}
    void ExitBurst() override {}
    uint64_t LastEngineDeltaUs() const override { return 0; }
    void SetRate(float) override {}
    void SetPaused(bool) override {}
    void SetDurationUs(uint64_t) override {}
    void Release() override {}
    replay::ClockStatus Status() const override {
        replay::ClockStatus s{};
        return s;
    }
};

struct ClockFixture {
    Env& e;
    RecordingClock clk;
    replay::IReplayClock* saved;

    ClockFixture() : e(TheEnv()) {
        replay::playback::ForgetSessionEntrantsForTest();
        // ...and the leave feed with it: this rig never drives the stage-3 tail that drains it,
        // so a queue left by an earlier case would defer every later case's dress pass.
        replay::identity::ForgetRosterLeave();   // fresh session -- see RosterFixture
        e.services.transport->Unload();
        saved = e.services.clock;
        e.services.clock = &clk;
    }
    ~ClockFixture() {
        e.services.transport->Unload();
        e.services.clock = saved;
    }
};

struct TrimCall {
    uint16_t handle;
    uint64_t from;
    uint64_t to;
};

// The entity surface, recording. `ownedUntilClaimed` models the half of INV-OWN1 that makes a claim
// necessary: the engine answers "this body is mine" until the synthetic owner is written, and stops
// the moment it is -- so a claim that is never released leaves `held` non-empty for the rest of the
// session, which is exactly the state entity.h:473 exists to prevent.
class RecordingEntity : public SwitchableEntity {
public:
    std::vector<TrimCall> trims;
    std::vector<bool> lod;
    std::vector<uint16_t> claims;
    std::vector<uint16_t> releases;
    std::vector<uint16_t> held;
    bool ownedUntilClaimed = false;

    void TrimHistory(replay::PhysHandle h, uint64_t from, uint64_t to) override {
        TrimCall c;
        c.handle = h.v;
        c.from = from;
        c.to = to;
        trims.push_back(c);
    }
    void SetLodThrottle(bool enabled) override { lod.push_back(enabled); }
    bool ClaimLodThrottle(bool enabled) override {
        lod.push_back(enabled);
        return true;
    }
    bool ReleaseLodThrottle() override {
        ++lodReleases;
        return true;
    }
    size_t lodReleases = 0;

    bool IsLocallyControlled(replay::PhysHandle h) const override {
        if (!ownedUntilClaimed) {
            return false;
        }
        for (size_t i = 0; i < held.size(); ++i) {
            if (held[i] == h.v) {
                return false;
            }
        }
        return true;
    }
    void SetSyntheticOwner(replay::PhysHandle h) override {
        claims.push_back(h.v);
        held.push_back(h.v);
    }
    void ClearSyntheticOwner(replay::PhysHandle h) override {
        releases.push_back(h.v);
        for (size_t i = 0; i < held.size(); ++i) {
            if (held[i] == h.v) {
                held.erase(held.begin() + (ptrdiff_t)i);
                return;
            }
        }
    }

    // A slot-bound pawn needs both steps to resolve: PlayerOf takes the capture's user slot and
    // PhysOf takes the PlayerCI it returns. 0xFFFF at either step is absence, so neither may be it.
    replay::PlayerHandle PlayerOf(replay::UserIndex i) const override {
        return replay::PlayerHandle{(uint16_t)(50 + i.v)};
    }
    replay::PhysHandle PhysOf(replay::PlayerHandle p) const override {
        return replay::PhysHandle{(uint16_t)(0x20 + (p.v - 50))};
    }
};

struct SeamEntityFixture {
    Env& e;
    ZeroIndexIdentity id;
    RecordingEntity ent;
    CountingPose pose;
    replay::IIdentity* savedId;
    replay::IEntity* savedEnt;
    replay::IPosePublisher* savedPose;

    SeamEntityFixture() : e(TheEnv()) {
        replay::playback::ForgetSessionEntrantsForTest();
        // ...and the leave feed with it: this rig never drives the stage-3 tail that drains it,
        // so a queue left by an earlier case would defer every later case's dress pass.
        replay::identity::ForgetRosterLeave();   // fresh session -- see RosterFixture
        e.services.transport->Unload();
        savedId = e.services.identity;
        savedEnt = e.services.entity;
        savedPose = e.services.pose;
        ent.available = true;
        e.services.identity = &id;
        e.services.entity = &ent;
        e.services.pose = &pose;
        e.tick.FireAll(replay::TickInfo{});
    }
    ~SeamEntityFixture() {
        e.services.transport->Unload();
        e.services.identity = savedId;
        e.services.entity = savedEnt;
        e.services.pose = savedPose;
        e.tick.FireAll(replay::TickInfo{});
    }
};

}  // namespace

TEST_CASE("SEAM-M1: every frame writes the transport's commanded step to the engine clock") {
    ClockFixture f;
    Env& e = f.e;
    TempDemo tmp;
    const uint64_t dt = 11111;
    REQUIRE(WriteDemo(tmp.path, 5000000, dt, 60, 6, 1));
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));

    // Paused is where clock.h:123's "NEVER 0" is decided: S7.3.3 answers a 1 us external step, and a
    // paused clock with no step armed reads a raw delta of 0, which clock.cpp measured turning into
    // +inf. Every frame, not only the ones that move.
    t->Pause();
    f.clk.steps.clear();
    for (int i = 0; i < 4; ++i) {
        e.tick.FireAll(replay::TickInfo{});
    }
    REQUIRE(f.clk.steps.size() == 4);
    for (size_t i = 0; i < f.clk.steps.size(); ++i) {
        INFO("paused frame ", i, " step ", f.clk.steps[i]);
        CHECK(f.clk.steps[i] == 1u);
    }

    // Playing: the step the clock is handed is a RECORDED dt, never the wall delta and never a
    // scaled magnitude -- the rate scales how many of them one frame consumes. The sleep is what
    // makes that observable: a frame with less wall time than one tick consumes none and commands 1.
    t->Seek(0);
    t->Play(1.0f);
    e.tick.FireAll(replay::TickInfo{});   // absorbs the wall time that accumulated before Play
    f.clk.steps.clear();
    Sleep(40);
    e.tick.FireAll(replay::TickInfo{});
    REQUIRE(f.clk.steps.size() == 1);
    CHECK(f.clk.steps[0] == dt);

    t->Seek(0);
    t->Play(4.0f);
    e.tick.FireAll(replay::TickInfo{});
    const int64_t before = t->PlayheadUs();
    f.clk.steps.clear();
    Sleep(40);
    e.tick.FireAll(replay::TickInfo{});
    const int64_t advanced = t->PlayheadUs() - before;
    REQUIRE(f.clk.steps.size() == 1);
    CHECK(f.clk.steps[0] == dt);
    INFO("4x advanced ", advanced, " us of scene time over 40 ms of wall");
    CHECK(advanced > (int64_t)dt * 4);

    t->Pause();
    f.clk.steps.clear();
    e.tick.FireAll(replay::TickInfo{});
    REQUIRE(f.clk.steps.size() == 1);
    CHECK(f.clk.steps[0] == 1u);
}

TEST_CASE("SEAM-M2: a synthetic owner claim is released at every discontinuity and at unload") {
    SeamEntityFixture f;
    Env& e = f.e;
    f.ent.ownedUntilClaimed = true;

    TempDemo tmp;
    const uint64_t dt = 11111;
    // One local-pawn body and one player: windowbuild's slot pairing is only solved when there is
    // exactly one of each, and the slot path is the only one that claims a synthetic owner.
    REQUIRE(WriteDemo(tmp.path, 5000000, dt, 60, 6, 1, 2000000, nullptr, 0, 0, true));
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));
    e.tick.FireAll(replay::TickInfo{});
    e.tick.FireAll(replay::TickInfo{});
    REQUIRE(f.ent.claims.size() >= 1);
    CHECK(f.ent.held.size() == f.ent.claims.size() - f.ent.releases.size());

    // ★ NO SEEK BETWEEN THE CLAIM AND THE UNLOAD in this case: this is the load-to-unload half of
    // the pairing. The half that crosses a window rebuild is OWNER-PAIRING below, which is where
    // the leak this case once reported (a rebuild constructing fresh BodyTracks with
    // synthetic_owner_set == false, windowbuild.cpp:334, after the claim) is now asserted against.
    t->Unload();
    INFO("claims ", f.ent.claims.size(), " releases ", f.ent.releases.size(), " still held ",
         f.ent.held.size());
    CHECK(f.ent.held.empty());
    CHECK(f.ent.releases.size() == f.ent.claims.size());
}

TEST_CASE("DISC-BIND: every claim is handed back once on the tick the match ends") {
    SeamEntityFixture f;
    Env& e = f.e;
    f.ent.ownedUntilClaimed = true;
    ArmNetGame netGame;   // in match
    replay::TickInfo info{};
    info.netGame = netGame.bytes;

    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 6, 1, 2000000, nullptr, 0, 0, true));
    REQUIRE(e.services.transport->Load(tmp.path));
    e.tick.FireAll(info);
    e.tick.FireAll(info);
    REQUIRE(f.ent.claims.size() >= 1);
    REQUIRE(f.ent.held.size() == f.ent.claims.size());

    netGame.SetState(facts::NETGAME_STATE_INMATCH - 1);
    e.tick.FireAll(info);
    CHECK(f.ent.held.empty());
    const size_t released = f.ent.releases.size();
    CHECK(released == f.ent.claims.size());

    e.tick.FireAll(info);
    e.tick.FireAll(info);
    CHECK(f.ent.releases.size() == released);   // the edge, not every tick after it
    CHECK(f.ent.claims.size() == released);     // and nothing re-claims into a session that is over
}

TEST_CASE("OWNER-PAIRING: a claim is handed back for the SAME handle before the seek re-binds it") {
    SeamEntityFixture f;
    Env& e = f.e;
    f.ent.ownedUntilClaimed = true;

    TempDemo tmp;
    const uint64_t dt = 11111;
    // One local-pawn body and one player: the slot path is the only one that claims an owner.
    REQUIRE(WriteDemo(tmp.path, 5000000, dt, 60, 6, 1, 2000000, nullptr, 0, 0, true));
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));
    e.tick.FireAll(replay::TickInfo{});
    e.tick.FireAll(replay::TickInfo{});
    REQUIRE(f.ent.claims.size() >= 1);
    const std::vector<uint16_t> heldBeforeSeek = f.ent.held;
    const size_t claimsBeforeSeek = f.ent.claims.size();
    const size_t releasesBeforeSeek = f.ent.releases.size();
    REQUIRE(heldBeforeSeek.size() >= 1);

    // The SHIPPED seek path, far enough to leave the resident window: SeekAbsolute rebuilds there
    // and then, and the rebuild is what forgets the claim -- so the release has to be ahead of it.
    // Nothing is ticked in between; every call below is what Seek() itself did.
    const int64_t duration = (int64_t)t->DurationUs();
    t->Seek(duration * 5 / 6);

    INFO("claims ", f.ent.claims.size(), " releases ", f.ent.releases.size(), " still held ",
         f.ent.held.size());
    REQUIRE(f.ent.releases.size() == releasesBeforeSeek + heldBeforeSeek.size());
    // The SAME handle, not merely a matching count.
    for (size_t i = 0; i < heldBeforeSeek.size(); ++i) {
        bool found = false;
        for (size_t j = releasesBeforeSeek; j < f.ent.releases.size(); ++j) {
            if (f.ent.releases[j] == heldBeforeSeek[i]) {
                found = true;
            }
        }
        INFO("handle ", heldBeforeSeek[i], " claimed before the seek");
        CHECK(found);
    }
    // BEFORE any new claim: the count cannot have moved, because the release must precede the
    // rebind that would re-arm it.
    CHECK(f.ent.claims.size() == claimsBeforeSeek);
    CHECK(f.ent.held.empty());

    // And the engine answers "locally controlled" again at the new time, which is only true if the
    // release actually reached it -- so the next apply re-claims rather than driving a body it no
    // longer owns.
    e.tick.FireAll(replay::TickInfo{});
    CHECK(f.ent.claims.size() > claimsBeforeSeek);

    // Unload releases all: the pairing holds across the seek as well as across the session.
    t->Unload();
    INFO("final claims ", f.ent.claims.size(), " releases ", f.ent.releases.size());
    CHECK(f.ent.held.empty());
    CHECK(f.ent.releases.size() == f.ent.claims.size());
}

TEST_CASE("SEAM-M3: history is trimmed at the window's cadence, to the resident span") {
    SeamEntityFixture f;
    Env& e = f.e;
    const uint64_t dt = 11111;
    const uint32_t per = 60;
    const uint32_t chapters = 9;
    const uint32_t bodies = 2;
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, dt, per, chapters, bodies));

    f.ent.trims.clear();
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));

    // ONE CALL PER BOUND BODY PER WINDOW BUILD. The resident window holds three chapters, so a trim
    // driven per RECORD would be 180 calls a body here rather than one; the bound is the property,
    // not the count.
    REQUIRE(f.ent.trims.size() >= 1);
    CHECK(f.ent.trims.size() <= bodies);

    // entity.cpp:1768: per-CI growth "is bounded by the caller's TrimHistory window". The window is
    // the resident span, so no call may hand the engine a keep-range wider than the chapters the
    // build actually holds -- an unbounded range would keep every sample the session ever pushed.
    const uint64_t residentSpan = (uint64_t)dt * per * 3 + (uint64_t)dt * per;
    for (size_t i = 0; i < f.ent.trims.size(); ++i) {
        INFO("trim ", i, " handle ", f.ent.trims[i].handle, " [", f.ent.trims[i].from, ", ",
             f.ent.trims[i].to, "]");
        CHECK(f.ent.trims[i].to >= f.ent.trims[i].from);
        CHECK(f.ent.trims[i].to - f.ent.trims[i].from <= residentSpan);
    }

    const size_t afterLoad = f.ent.trims.size();
    const int64_t duration = (int64_t)t->DurationUs();
    for (int n = 1; n <= 8; ++n) {
        t->Seek(duration * n / 9);
        e.tick.FireAll(replay::TickInfo{});
    }
    // Eight windows later the call count has grown with the REBUILDS and with nothing else, and
    // every keep-range is still the resident one: bounded history over N windows.
    CHECK(f.ent.trims.size() > afterLoad);
    CHECK(f.ent.trims.size() <= afterLoad + bodies * 8);
    for (size_t i = afterLoad; i < f.ent.trims.size(); ++i) {
        INFO("window trim ", i, " [", f.ent.trims[i].from, ", ", f.ent.trims[i].to, "]");
        CHECK(f.ent.trims[i].to >= f.ent.trims[i].from);
        CHECK(f.ent.trims[i].to - f.ent.trims[i].from <= residentSpan);
    }
}

TEST_CASE("SEAM-M4: the LOD throttle is CLAIMED at load and handed back at unload") {
    // ★ REWRITTEN BY the review. What this case asserted before was the defect: that a
    // tick with NO FILE LOADED writes `true` into the enable dword -- a value this module had
    // never read, in every session, including the owner's live public spectate, with no restore.
    // The contract now is a borrow: nothing is written until a file is open, what is written is
    // only the suppression, and the hand-back is the ENGINE's own saved value rather than a 1 the
    // plugin picked. `lod` therefore records claims, and `lodReleases` records the hand-back.
    SeamEntityFixture f;
    Env& e = f.e;

    CHECK(f.ent.lod.empty());          // no file: not one byte written
    CHECK(f.ent.lodReleases == 0);

    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 3, 2));
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));
    e.tick.FireAll(replay::TickInfo{});
    REQUIRE(f.ent.lod.size() == 1);
    CHECK(f.ent.lod[0] == false);      // the round-robin off, which is what a free camera wants

    e.tick.FireAll(replay::TickInfo{});
    e.tick.FireAll(replay::TickInfo{});
    CHECK(f.ent.lod.size() == 1);      // written only on a change, not once a frame

    t->Unload();
    e.tick.FireAll(replay::TickInfo{});
    CHECK(f.ent.lod.size() == 1);      // the hand-back is not another claim of ours
    CHECK(f.ent.lodReleases == 1);     // it is the engine's value going back, once
}

TEST_CASE("SEAM-M5: the pins a keyframe carries reach the audio residue, not just a counter") {
    Env& e = TheEnv();
    replay::IReplayTransport* t = e.services.transport;
    t->Unload();

    // A pin block published the way the capture path publishes one (keyframe.h:167, tick thread) and
    // written into the keyframe by W2-A's real writer, so what the reader decodes is a file's block
    // and not a struct this test handed it. Seven loops, zero-valued: the offline residue counts
    // them without reading them, and seven is a number no other path in this binary produces.
    std::vector<uint8_t> block(replay::audio::MaxPinBytes());
    replay::audio::AudioPins pins{};
    pins.loopCount = 7;
    pins.totals.loopsSeen = 7;
    pins.totals.loopsStored = 7;
    const size_t encoded = replay::audio::EncodePins(pins, block.data(), block.size());
    REQUIRE(encoded != 0);
    replay::capture::PublishAudioPins(block.data(), (uint32_t)encoded);

    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 4, 1));
    REQUIRE(t->Load(tmp.path));

    // The observation is on the far side of audio::SetAudioPins: the residue's own copy of the pins
    // after a seek is the file's block, and seven loops is a number no other path produces.
    t->Seek((int64_t)t->DurationUs() * 3 / 4);
    e.tick.FireAll(replay::TickInfo{});
    const replay::playback::SeamStatus seam = replay::playback::SeamSnapshot();
    INFO("published bytes ", encoded, " pins pushed ", seam.audio_pins_pushed, " absent ",
         seam.audio_pins_absent, " refused ", seam.audio_pins_refused);
    const replay::audio::AudioPins* held = replay::audio::AudioPinsForTest();
    REQUIRE(held != nullptr);
    CHECK(held->loopCount == 7u);

    replay::capture::ResetAudioPinsForTest();
    t->Unload();
}

// ---------------------------------------------------------------------------------------------
// S12.5.1's ORACLE CAMERA TRACK: does the SHIPPED load and unload path open and close W2-E's
// bracket. Same blind spot as the arm above -- camera_control.cpp has carried BeginOracleTrack,
// EndOracleTrack and SetMode's OracleTrackActive refusal, tested, since W2-E landed, and until the
// module called them "the ORACLE track never drives the camera and S12.5.1 has no mechanism in the
// product" (CENSUS-UNWIRED, row BeginOracleTrack).
namespace {

// Records rather than decides. The refusal semantics of the REAL control -- SetMode answering
// OracleTrackActive while a track holds the camera -- are proven against the shipped Control in
// tests/camera/test_camera_module.cpp ("commanded pose: the oracle's track holds the camera, and
// says which refusal that is"). What no case there can show is that anything ever puts it into
// that state, and that is exactly what these cases are for.
class RecordingCameraControl final : public replay::IReplayCameraControl {
public:
    bool                   beginResult = true;
    replay::ControlRefusal beginRefusal = replay::ControlRefusal::None;
    size_t                 begins = 0;
    size_t                 ends = 0;
    size_t                 modeRefusals = 0;
    bool                   track = false;

    bool SetMode(replay::CameraMode, int32_t) override {
        if (track) {
            last = replay::ControlRefusal::OracleTrackActive;
            ++modeRefusals;
            return false;
        }
        last = replay::ControlRefusal::None;
        return true;
    }
    replay::CameraMode RequestedMode() const override { return replay::CameraMode::Free; }
    bool ObservedMode(replay::CameraMode*) const override { return false; }
    bool ObservedSelector(int32_t*, uint16_t*) const override { return false; }
    bool ActiveLevelCameraSet(uint16_t*) const override { return false; }
    bool BeginOracleTrack() override {
        if (!beginResult) {
            last = beginRefusal;
            return false;
        }
        ++begins;
        track = true;
        return true;
    }
    void EndOracleTrack() override {
        ++ends;
        track = false;
    }
    bool OracleTrackActive() const override { return track; }
    bool Available() const override { return true; }
    replay::ControlRefusal LastRefusal() const override { return last; }

    replay::ControlRefusal last = replay::ControlRefusal::None;
};

struct RecordedPose {
    float pos[3];
    float quat[4];
    float fovY;
};

class RecordingPoseSink final : public replay::IReplayCamera {
public:
    std::vector<RecordedPose> poses;

    void SetPose(const float pos[3], const float quat[4], float fovYRadians) override {
        RecordedPose p{};
        memcpy(p.pos, pos, sizeof p.pos);
        memcpy(p.quat, quat, sizeof p.quat);
        p.fovY = fovYRadians;
        poses.push_back(p);
    }
    bool AppliesToAllViews() const override { return true; }
    const char* Strategy() const override { return "recording"; }
};

// Installs both ends and takes them out again. The Env is shared by every case in this file, so a
// camera left installed would arm a later case's Load without that case asking for a track.
struct OracleArm {
    Env&                   e;
    RecordingCameraControl control;
    RecordingPoseSink      sink;
    size_t                 from;

    explicit OracleArm(bool ask, bool withSink = true) : e(TheEnv()), from(e.log.lines.size()) {
        e.config.oracleTrack = ask;
        e.services.camera = withSink ? &sink : nullptr;
        replay::playback::SetCameraControlForTest(&control);
    }
    ~OracleArm() {
        e.services.transport->Unload();
        replay::playback::SetCameraControlForTest(nullptr);
        e.services.camera = nullptr;
        e.config.oracleTrack = false;
    }
};

}  // namespace

TEST_CASE("S12.5.1: a load that asked for the track enters mode api, and unload always leaves it") {
    OracleArm arm(true);
    TempDemo tmp;
    const uint64_t t0 = 5000000, dt = 11111;
    REQUIRE(WriteDemo(tmp.path, t0, dt, 60, 1, 1));
    REQUIRE(arm.e.services.transport->Load(tmp.path));

    CHECK(arm.control.begins == 1);
    CHECK(arm.control.ends == 0);
    replay::playback::OracleTrackStatus st = replay::playback::OracleTrackSnapshot();
    CHECK(st.asked);
    CHECK(st.active);
    CHECK(std::string(st.refusal).empty());
    CHECK(CountLoggedSince(arm.from, "playback", "mode api entered") == 1);

    arm.e.tick.FireAll(replay::TickInfo{});
    arm.e.tick.FireAll(replay::TickInfo{});
    CHECK(arm.sink.poses.size() == 2);
    CHECK(replay::playback::OracleTrackSnapshot().poses == 2);

    // The exclusivity the bracket exists for, now that something enters it.
    CHECK(arm.control.OracleTrackActive());
    CHECK_FALSE(arm.control.SetMode(replay::CameraMode::Free, 0));
    CHECK(std::string(replay::ControlRefusalName(arm.control.LastRefusal())) ==
          "oracle-track-active");

    arm.e.services.transport->Unload();
    CHECK(arm.control.ends == 1);
    CHECK_FALSE(arm.control.OracleTrackActive());
    CHECK_FALSE(replay::playback::OracleTrackSnapshot().active);
    CHECK(CountLoggedSince(arm.from, "playback", "mode api left (unload)") == 1);

    // And the camera is genuinely handed back: no pose is written after the release.
    const size_t after = arm.sink.poses.size();
    arm.e.tick.FireAll(replay::TickInfo{});
    CHECK(arm.sink.poses.size() == after);
}

TEST_CASE("S12.5.1: the pose is an exact function of source time, not of when the frame ran") {
    OracleArm arm(true);
    TempDemo tmp;
    const uint64_t t0 = 5000000, dt = 11111;
    REQUIRE(WriteDemo(tmp.path, t0, dt, 60, 1, 1));
    REQUIRE(arm.e.services.transport->Load(tmp.path));
    // Load leaves the transport PAUSED, which is the point: the playhead moves only where this
    // case seeks it, so an equal pose is a statement about the track and not about the clock.
    REQUIRE(arm.e.services.transport->State() == replay::TransportState::Pause);

    arm.e.services.transport->Seek((int64_t)(20 * dt));
    arm.e.tick.FireAll(replay::TickInfo{});
    REQUIRE(arm.sink.poses.size() >= 1);
    const RecordedPose first = arm.sink.poses.back();

    arm.e.services.transport->Seek((int64_t)(45 * dt));
    arm.e.tick.FireAll(replay::TickInfo{});
    const RecordedPose elsewhere = arm.sink.poses.back();

    arm.e.services.transport->Seek((int64_t)(20 * dt));
    arm.e.tick.FireAll(replay::TickInfo{});
    const RecordedPose again = arm.sink.poses.back();

    CHECK(memcmp(&first, &again, sizeof first) == 0);
    // The control that stops "identical" from being trivially true of a track that never moves.
    CHECK(memcmp(&first, &elsewhere, sizeof first) != 0);

    // Evaluated without a camera behind it, the same function answers the same numbers -- so both
    // passes of the image gate can be shown to run one track rather than two copies of one.
    RecordedPose direct{};
    replay::playback::OracleTrackPoseAt((int64_t)(20 * dt), direct.pos, direct.quat, &direct.fovY);
    CHECK(memcmp(&first, &direct, sizeof first) == 0);
}

TEST_CASE("S12.5.1: a control that refuses the track is reported BY NAME and drives nothing") {
    OracleArm arm(true);
    arm.control.beginResult = false;
    arm.control.beginRefusal = replay::ControlRefusal::PoseOwnerConflict;
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 1, 1));
    REQUIRE(arm.e.services.transport->Load(tmp.path));

    CHECK(arm.control.begins == 0);
    replay::playback::OracleTrackStatus st = replay::playback::OracleTrackSnapshot();
    CHECK(st.asked);
    CHECK_FALSE(st.active);
    // The word is W2-E's, read back through W2-E's own namer rather than spelled a second time
    // here: a checker with its own copy of somebody else's table certifies its own copy. What this
    // case owns is that the module published the refusal the CONTROL gave it, and not a bare false.
    const std::string named = replay::ControlRefusalName(replay::ControlRefusal::PoseOwnerConflict);
    CHECK_FALSE(named.empty());
    CHECK(named != "none");
    CHECK(std::string(st.refusal) == named);
    CHECK(CountLoggedSince(arm.from, "playback",
                             (std::string("oracle camera track refused: ") + named).c_str()) == 1);

    arm.e.tick.FireAll(replay::TickInfo{});
    CHECK(arm.sink.poses.empty());

    // Nothing to hand back: a track that never opened must not close a bracket it does not hold.
    arm.e.services.transport->Unload();
    CHECK(arm.control.ends == 0);
}

TEST_CASE("S12.5.1: with no pose sink the track is refused rather than taking the camera") {
    OracleArm arm(true, false);
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 1, 1));
    REQUIRE(arm.e.services.transport->Load(tmp.path));

    CHECK(arm.control.begins == 0);
    CHECK_FALSE(arm.control.OracleTrackActive());
    CHECK(std::string(replay::playback::OracleTrackSnapshot().refusal) == "no-pose-sink");
    CHECK(CountLoggedSince(arm.from, "playback", "oracle camera track refused: no-pose-sink") == 1);
}

TEST_CASE("S12.5.1: a load nobody asked a track for takes no camera and logs no track line") {
    OracleArm arm(false);
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 1, 1));
    REQUIRE(arm.e.services.transport->Load(tmp.path));

    replay::playback::OracleTrackStatus st = replay::playback::OracleTrackSnapshot();
    CHECK_FALSE(st.asked);
    CHECK_FALSE(st.active);
    CHECK(arm.control.begins == 0);
    CHECK(CountLoggedSince(arm.from, "playback", "oracle camera track") == 0);

    arm.e.tick.FireAll(replay::TickInfo{});
    CHECK(arm.sink.poses.empty());
    arm.e.services.transport->Unload();
    CHECK(arm.control.ends == 0);
}

TEST_CASE("L3 #11: the viewer holds the recorded slot, so the roster is REMAPPED and dressed there") {
    // THE CASE THE PACKET EXISTS FOR. Identity::ProvisionReplayUser refuses an occupied recorded
    // slot, and the viewer's own user holds slot 0 in every live lobby, so a dresser handed the
    // shipped singleton dresses NOBODY. The remap lives in PawnProvisioner and only reaches the
    // dresser through the provisioner-backed IIdentity.
    RosterFixture f;
    f.id.Seat(0);
    const uint16_t slots[1] = {0};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 1));
    replay::IReplayTransport* t = f.e.services.transport;
    const size_t from = TheEnv().log.lines.size();
    REQUIRE(t->Load(tmp.path));
    f.Tick();

    const IdentityStatus st = IdentitySnapshot();
    CHECK_FALSE(st.pending);
    CHECK(st.players == 1);
    CHECK(st.mapped == 1);
    // Slot 0 was taken, so the ascending scan admitted into 1 -- and BOTH numbers are printed, so a
    // map that quietly did not fire reads as 0 -> 0 and cannot be mistaken for this.
    CHECK(CountLoggedOpeningSince(from, "pawn", "recorded slot 0 -> slot 1") == 1);
    CHECK(CountLoggedOpeningSince(from, "pawn", "recorded slot 0 -> slot 0") == 0);
    CHECK(CountLoggedSince(from, "pawn", "provisioned 1, adopted") == 1);
    // Asked of the engine, at the slot the remap chose: the admission the dresser was handed is the
    // one that reached the lobby.
    CHECK(f.id.provisionedSlot == 1);
    CHECK(f.id.IndexOf(replay::UserSlot(1)).v == IndexForSlot(1));
}

// W2-P1-30e: THE WIRE, not the unit. Every loadout case before this one armed the provisioner
// itself, and the shipped Load path filled neither provider slot -- so a launch could only print
// `loadout: not-authored (LoadoutRecordingNotAsked)`, which is the one word that cannot be told
// apart from a build that was never asked at all. Both readings are taken from ONE fixture here and
// they differ: with a block the drive consumes it and stops at the ENGINE bind (no game image is
// mapped under a test, so L1 is as far as a unit can reach); with none, the reader's own refusal
// rides the census line. An unwired slot produces neither -- no slot is dressed at all.
// The owner sets no ECHOREPLAY_* variable. What the provisioner is armed with then
// must be the route the launches rendered bodies on: the reply route (bot_body off) with its
// equipment RECV and source loadout, and neither the rejected backstop nor the bot-route lever.
TEST_CASE("With no variable set, Load arms the launch-verified body route") {
    RosterFixture f;
    f.id.Seat(0);
    const uint16_t rosterSlots[1] = {0};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, rosterSlots, 1));
    REQUIRE(f.e.services.transport->Load(tmp.path));
    f.Tick();

    // THE MUTANT: restore either old default in config_keys.inc and equipmentRecv reads false --
    // the coupling at the arm needs BOTH halves of the pair.
    const replay::playback::ArmedBodyRoute route = replay::playback::ArmedBodyRouteForTest();
    CHECK(route.equipmentRecv);
    CHECK(route.sourceLoadoutBeforeAdmit);
    CHECK_FALSE(route.botBodySourceLoadout);
    CHECK_FALSE(route.participateBackstop);
}

TEST_CASE("W2-P1-30e: the loaded file's identity block reaches the dress through the shipped env") {
    RosterFixture f;
    f.id.Seat(0);
    const uint16_t rosterSlots[1] = {0};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, rosterSlots, 1));
    replay::IReplayTransport* t = f.e.services.transport;
    REQUIRE(t->Load(tmp.path));

    bool planted = false;
    SUBCASE("a v3 block for the recorded slot is read, and the drive reaches the engine bind") {
        planted = true;
    }
    SUBCASE("no block in the file is the READER'S word, not a silence") { planted = false; }

    if (planted) {
        // The block this file would have carried, written by the SHIPPED writer and offered to the
        // same singleton the load-time scan fills. Stamped at 0, so it is at or before every
        // horizon a tick can set and the read cannot fail for a reason this case is not about.
        replay::capture::IdentityEntrant fields{};
        fields.xplatformId = 0xDEC0DE0000000000ull;
        fields.userSlot = 0;
        fields.userIndex = 0;
        fields.generation = 1;
        fields.alignment = 3;
        fields.teamId = 1;
        fields.bodyType = 2;
        fields.loadoutNumber = 11;
        fields.filledPairCount = replay::capture::kIdentityBlockSlotCount;
        fields.flags = replay::capture::kIdentityEntrantProfileAsked;
        _snprintf_s(fields.displayName, sizeof fields.displayName, _TRUNCATE, "Dressed");
        _snprintf_s(fields.platformStr, sizeof fields.platformStr, _TRUNCATE, "PC");
        replay::capture::IdentityLoadoutSlot items[replay::capture::kIdentityBlockSlotCount] = {};
        for (uint16_t i = 0; i < replay::capture::kIdentityBlockSlotCount; ++i) {
            items[i].slotIndex = i;
            items[i].itemId = 0x1701000000000000ull + i;
            items[i].slotKey = 0x5EED000000000000ull + i;
        }
        const std::string json = "{\"loadout\":{\"slot_0\":\"item\"}}";
        std::vector<uint8_t> buf(replay::capture::kMaxEventBodyBytes);
        replay::capture::IdentityBlockWriter w(buf.data(), buf.size(), 0, 0xC0FFEEull);
        replay::capture::IdentityBlockEntrantIn in{};
        in.fields = fields;
        in.slots = items;
        replay::Blob b{};
        b.p = reinterpret_cast<const uint8_t*>(json.data());
        b.n = uint32_t(json.size());
        in.profileJson = b;
        REQUIRE(w.Add(in));
        size_t written = 0;
        REQUIRE(w.Finish(written));
        REQUIRE(replay::identity::PlaybackRecordedIdentity().Offer(buf.data(), uint32_t(written)));
    }

    const size_t from = TheEnv().log.lines.size();
    f.Tick();

    // (1) THE SLOT IS FILLED, and by the shipped path -- this case armed nothing.
    replay::playback::RecordedLoadoutProviderFn provider =
        replay::playback::ArmedRecordedLoadoutProviderForTest();
    REQUIRE(provider != nullptr);

    // (2) It is bound to the SAME source the load-time census prints, so the two routes cannot
    // disagree about what the file said: the block planted in that singleton comes back through it.
    replay::identity::RecordedLoadout rec{};
    const char* why = nullptr;
    const bool read = provider(0, rec, &why);
    CHECK(read == planted);
    if (planted) {
        CHECK(rec.hasSlotKeys);
        CHECK(rec.filledPairCount == replay::identity::kRecordedLoadoutPairCap);
        CHECK(rec.pairs[0].slotKey == 0x5EED000000000000ull);
        CHECK(rec.pairs[0].itemId == 0x1701000000000000ull);
        CHECK(rec.documentBytes != 0);
        CHECK(rec.document != nullptr);
        CHECK(replay::identity::PlaybackRecordedIdentity().SlotCount() == 1);
    } else {
        REQUIRE(why != nullptr);
        CHECK(strcmp(why, "IdentityBlockNotRecorded") == 0);
    }

    // (3) AUD-W2-P1-30-part2 F8. The `hops: unasked` clause that stood here was FIXTURE-KEYED and
    // stayed green through the very wire it promised to catch: this case's identity offers no hop
    // surface, so the word appears whether or not `Arm` asks for one, and the day the wire landed
    // (bfdd00d) it went on passing. A canary whose subject is the fixture is deleted, not moved:
    // the SHIPPED wire is asserted where it can fail -- `W2-B-HOPS: the shipped Load arms the hop
    // surface` binds a real surface before Load and requires `hops: unasked` to be ABSENT.
    // `LoadoutRecordingNotAsked` must never appear either way: that word is reserved for a build
    // whose Load path did not fill the slot at all.
    // DIAG-APPLY-KEY sec A: with no hop surface the entrant is PARKED, so the census names the
    // held deferral (the session reason) rather than a recording verdict synthesised from a zeroed
    // struct; the recorded-side DocumentNotRecorded is DriveLoadout's to name.
    CHECK(CountLoggedSince(from, "pawn", "loadout: not-attempted") >= 1);
    CHECK(CountLoggedSince(from, "pawn", "loadout: not-authored (LoadoutRecordingNotAsked)") == 0);
}

namespace {

// An engine that has already finished building the pawn: every one of the four hop reads answers
// done and both authored steps succeed. The index is recorded because a chain driven for the
// RECORDED slot instead of the live one would read the same `true` and pass on the wrong entrant.
struct DoneHopEngine {
    unsigned reads = 0;
    uint16_t sawIndex = replay::kSlotUnmapped;
    unsigned assigns = 0;
    uint8_t  team = 0xFE;      // the live packed role; A3 assigns the recorded one over it
    uint8_t  teamSlot = 0;
};
DoneHopEngine* DoneEng(void* c) { return static_cast<DoneHopEngine*>(c); }
bool DoneHopRead(void* c, uint16_t i) {
    DoneEng(c)->reads++;
    DoneEng(c)->sawIndex = i;
    return true;
}
replay::PawnProvision::LocalUserAnswer DoneHopNotLocal(void*, uint16_t) {
    return replay::PawnProvision::LocalUserAnswer::Remote;
}
replay::PawnProvision::PhysicsCiReq DoneHopReq(void*, uint16_t) {
    return replay::PawnProvision::PhysicsCiReq::None;
}
replay::PawnProvision::SpawnedWrite DoneHopSpawned(void*, uint16_t) {
    return replay::PawnProvision::SpawnedWrite::AlreadySet;
}
bool DoneHopAssign(void* c, uint16_t, uint16_t role) {
    DoneEng(c)->assigns++;
    DoneEng(c)->team = uint8_t(role);
    return true;
}
bool DoneHopTeamOf(void* c, uint16_t, uint8_t* teamId, uint8_t* teamSlot) {
    *teamId = DoneEng(c)->team;
    *teamSlot = DoneEng(c)->teamSlot;
    return true;
}
replay::PawnProvision::ActorHops DoneHopsFor(DoneHopEngine& e) {
    replay::PawnProvision::ActorHops h;
    h.ctx = &e;
    h.inRemotePlayerCs = DoneHopRead;
    h.enabled = DoneHopRead;
    h.placed = DoneHopRead;
    h.physicsCi = DoneHopRead;
    h.physicsCiAwaiting = DoneHopReq;
    h.isLocalUser = DoneHopNotLocal;
    h.writeSpawned = DoneHopSpawned;
    h.assignTeam = DoneHopAssign;
    h.teamOf = DoneHopTeamOf;
    return h;
}

}  // namespace

// AUD-ARCH-23-S14 R1-F1. The wrapper the dress pass is handed forwards fifteen methods; the one it
// dropped answers nullptr from the base, which is the word for "this build bound no hop read". The
// forward is asserted BOTH ways: a wrapper that answered a constant surface would pass a test that
// only ever handed it one.
TEST_CASE("W2-B-HOPS T9: the admission wrapper forwards the inner identity's own hop surface") {
    DoneHopEngine eng;
    replay::PawnProvision::ActorHops h = DoneHopsFor(eng);
    RosterIdentity inner;

    CHECK(inner.ActorHops() == nullptr);
    CHECK(replay::playback::ProvisionedIdentityHopsForTest(inner) == nullptr);

    inner.hopSurface = &h;
    const void* through = replay::playback::ProvisionedIdentityHopsForTest(inner);
    CHECK(through == static_cast<const void*>(&h));
    CHECK(through == static_cast<const void*>(inner.ActorHops()));
}

namespace {

// IDENT.SERVER_ASSIGN_TEAM as the engine implements it for the case that matters: it writes the
// requested role into entrant `index`'s packed word.  A stand-in that only counted calls could not
// tell V3's postcondition from its wish.
struct ViewerSeatEngine {
    unsigned calls = 0;
    // 9999 is a FIXTURE sentinel with no registry meaning: LOBBY.TEAM_NONE's 0xFFFF would make
    // this struct claim to model the engine's own "no team" value, which it does not. What
    // proves these two were written at all is `calls`, asserted before either is read.
    uint16_t sawIndex = 9999;
    uint16_t sawTeam = 9999;
    uint8_t* ng = nullptr;
};

uint16_t* EntrantWordAt(uint8_t* ng, uint16_t index) {
    return reinterpret_cast<uint16_t*>(ng + facts::LOCAL_NETGAME_USER_RECORD_BASE_OFF +
                                       size_t(index) * facts::USER_STRIDE);
}

void PutRole(uint8_t* ng, uint16_t index, uint16_t role) {
    const uint16_t field =
        uint16_t(uint16_t(facts::IDENT_USER_TEAM_MASK) << facts::IDENT_USER_TEAM_SHIFT);
    const uint16_t bits = uint16_t(uint16_t(role & facts::IDENT_USER_TEAM_MASK)
                                   << facts::IDENT_USER_TEAM_SHIFT);
    uint16_t* w = EntrantWordAt(ng, index);
    *w = uint16_t(uint16_t(*w & uint16_t(~field)) | bits);
}

uint16_t GetRole(uint8_t* ng, uint16_t index) {
    return uint16_t((*EntrantWordAt(ng, index) >> facts::IDENT_USER_TEAM_SHIFT) &
                    facts::IDENT_USER_TEAM_MASK);
}

bool ViewerSeatAssign(void* ctx, uint16_t index, uint16_t team) {
    ViewerSeatEngine* e = static_cast<ViewerSeatEngine*>(ctx);
    ++e->calls;
    e->sawIndex = index;
    e->sawTeam = team;
    PutRole(e->ng, index, team);
    return true;
}

// The viewer seat runs in a PLAYBACK session and in no other, and the shipped
// tick asks bootstrap::OpenRequestPresent() -- the process environment -- for that answer. A test
// of the seat is therefore a test of a session that was launched to open a replay, and has to say
// so; without this every case below would exercise the capture-session refusal instead.
struct PlaybackSessionEnv {
    PlaybackSessionEnv() {
        const wchar_t* const name = replay::bootstrap::OpenEnvVarName();
        had_ = GetEnvironmentVariableW(name, saved_, DWORD(_countof(saved_))) != 0;
        SetEnvironmentVariableW(name, L"C:/demos/seat-scope.echodemo");
        // The carrier alone is no longer a playback session -- this module has
        // to OWN one. This is the HARNESS-OPENED arm: the carrier in the environment and a demo
        // open in this process, which is the state a `replay-local` launch reaches.
        const uint16_t slots[2] = {2, 4};
        REQUIRE(WriteDemo(demo_.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 2));
        REQUIRE(TheEnv().services.transport->Load(demo_.path));
    }
    ~PlaybackSessionEnv() {
        TheEnv().services.transport->Unload();
        SetEnvironmentVariableW(replay::bootstrap::OpenEnvVarName(), had_ ? saved_ : nullptr);
    }
    PlaybackSessionEnv(const PlaybackSessionEnv&) = delete;
    PlaybackSessionEnv& operator=(const PlaybackSessionEnv&) = delete;

private:
    TempDemo demo_;
    wchar_t saved_[1024] = {0};
    bool had_ = false;
};

// The launch carrier with NOTHING open -- RK-11's shape: an environment variable left set
// machine-wide by an earlier viewer launch, in a process that never opens a replay.
struct CarrierOnlyEnv {
    CarrierOnlyEnv() {
        const wchar_t* const name = replay::bootstrap::OpenEnvVarName();
        had_ = GetEnvironmentVariableW(name, saved_, DWORD(_countof(saved_))) != 0;
        SetEnvironmentVariableW(name, L"C:/demos/never-opened.echodemo");
    }
    ~CarrierOnlyEnv() {
        SetEnvironmentVariableW(replay::bootstrap::OpenEnvVarName(), had_ ? saved_ : nullptr);
    }
    CarrierOnlyEnv(const CarrierOnlyEnv&) = delete;
    CarrierOnlyEnv& operator=(const CarrierOnlyEnv&) = delete;

private:
    wchar_t saved_[1024] = {0};
    bool had_ = false;
};

// The capture module's own publisher, driven the way capture.cpp drives it and restored on the way
// out. Nothing else in this process can arm a capture: capture.cpp is not linked here.
struct CaptureRecordingHere {
    CaptureRecordingHere() { replay::capture::PublishCaptureRecording(true); }
    ~CaptureRecordingHere() { replay::capture::PublishCaptureRecording(false); }
    CaptureRecordingHere(const CaptureRecordingHere&) = delete;
    CaptureRecordingHere& operator=(const CaptureRecordingHere&) = delete;
};

}  // namespace

// ★ ARCH-23 S14.10 T3, and it is a WIRE case: the receiver is the SHIPPED `ProvisionedIdentity`
// reached through the SHIPPED module tick, not a stand-in this case hands to a chain itself.  The
// forward T9 asserts is the only thing standing between that receiver and IIdentity's base
// `return nullptr`, and R1-F1 named this exact failure: every unit test green while the seat
// refuses ViewerSeatNotIssued on every window of every session.  So the assertion is the ISSUED
// STATE -- one call, for entrant 0, with the registry's own spectator constant, and the record
// reading spectator when the tick returns -- never the absence of a refusal.
TEST_CASE("W2-B-31 T3: the shipped tick seats the viewer through ProvisionedIdentity's hops") {
    const PlaybackSessionEnv playbackSession;
    RosterFixture f;
    ViewerSeatEngine eng;
    eng.ng = f.netGame.bytes;
    replay::PawnProvision::ActorHops h{};
    h.ctx = &eng;
    h.assignTeam = ViewerSeatAssign;
    f.id.hopSurface = &h;

    // A seated local viewer on a PLAYING team -- the state every run so far has reported, and the
    // one the owner calls "you cant really spectate the replay".
    const uint16_t locals = 1;
    memcpy(f.netGame.bytes + facts::LOCAL_NETGAME_LOCAL_USER_COUNT_OFF, &locals, sizeof locals);
    PutRole(f.netGame.bytes, 0, 1);
    const size_t from = TheEnv().log.lines.size();

    f.Tick();

    // The route is live end to end: the wrapper answered the inner identity's surface, V3 issued
    // through it, and the ENGINE RECORD moved.
    CHECK(replay::playback::ProvisionedIdentityHopsForTest(f.id) ==
          static_cast<const void*>(&h));
    CHECK(eng.calls == 1);
    CHECK(eng.sawIndex == 0);
    CHECK(eng.sawTeam == uint16_t(facts::GAME_TEAM_SPECTATOR));
    CHECK(GetRole(f.netGame.bytes, 0) == uint16_t(facts::GAME_TEAM_SPECTATOR));
    CHECK(replay::playback::RoleSpectates(GetRole(f.netGame.bytes, 0)));

    // The census line the L3 clause anchors on, and the pre-seat measurement before it.  Neither
    // carries a `lobbyteam=` field any more: that instrument could only ever print -1.
    CHECK(CountLoggedSince(from, "identity", "viewer slot 0 role=1 (orange) preseat") == 1);
    CHECK(CountLoggedSince(from, "identity", "viewer slot 0 role=2 (spectator)") >= 1);
    CHECK(CountLoggedSince(from, "identity", "lobbyteam=") == 0);
    CHECK(CountLoggedSince(from, "identity", "ViewerNotSpectator") == 0);
    CHECK(CountLoggedSince(from, "identity", "ViewerSeatNotIssued") == 0);
    CHECK(CountLoggedSince(from, "identity", "ViewerSeatRefused") == 0);

    // Once. A second tick inside the same census window re-reads the record, finds the gate
    // already satisfied and issues nothing.
    f.Tick();
    CHECK(eng.calls == 1);
}

// The wiring half of the unit case in test_viewer_role.cpp: the SHIPPED tick, the
// same fixture T3 seats, and the only difference is that this process was not launched to open a
// replay.  This is L2 #17's capture-local session -- entrant 0 ORANGE, no ECHOREPLAY_OPEN -- and
// the seat it must not issue is the one that emptied that capture's physics pool for 216 s.
TEST_CASE("SEAT-SCOPE: the shipped tick issues no seat and no census in a capture session") {
    SetEnvironmentVariableW(replay::bootstrap::OpenEnvVarName(), nullptr);
    REQUIRE(replay::bootstrap::OpenRequestPresent() == false);

    RosterFixture f;
    ViewerSeatEngine eng;
    eng.ng = f.netGame.bytes;
    replay::PawnProvision::ActorHops h{};
    h.ctx = &eng;
    h.assignTeam = ViewerSeatAssign;
    f.id.hopSurface = &h;

    const uint16_t locals = 1;
    memcpy(f.netGame.bytes + facts::LOCAL_NETGAME_LOCAL_USER_COUNT_OFF, &locals, sizeof locals);
    PutRole(f.netGame.bytes, 0, 1);
    const size_t from = TheEnv().log.lines.size();

    for (int i = 0; i < 3; ++i) f.Tick();

    // Nothing was asked of the engine and the recording user keeps the team it was given.
    CHECK(eng.calls == 0);
    CHECK(GetRole(f.netGame.bytes, 0) == 1);

    // Named -- and named ONCE PER SESSION, not once per tick.  `g.viewer_seat` is the module's
    // one instance and its refusal latch is reset by a load, not by a doctest case, so the
    // process-wide count is the presence assertion and the count since `from` is the one that
    // says the line does not repeat.  test_viewer_role.cpp's SEAT-SCOPE case pins "exactly one"
    // on a fresh chain, which is the property this one cannot isolate.
    CHECK(CountLoggedSince(0, "identity", "ViewerSeatNotPlayback") >= 1);
    CHECK(CountLoggedSince(from, "identity", "ViewerSeatNotPlayback") <= 1);
    CHECK(CountLoggedSince(from, "identity", "viewer slot 0 role=") == 0);
    CHECK(CountLoggedSince(from, "identity", "preseat") == 0);
    CHECK(CountLoggedSince(from, "identity", "ViewerNotSpectator") == 0);
    CHECK(CountLoggedSince(from, "identity", "viewer seat: deferred") == 0);

    // THE CONTROL, on the same fixture and the same tick: declare the session a playback and the
    // very next tick seats.  Without it this case would pass against a tick that stopped calling
    // SeatViewer at all.
    {
        const PlaybackSessionEnv playbackSession;
        const size_t after = TheEnv().log.lines.size();
        f.Tick();
        CHECK(eng.calls == 1);
        CHECK(GetRole(f.netGame.bytes, 0) == uint16_t(facts::GAME_TEAM_SPECTATOR));
        CHECK(CountLoggedSince(after, "identity", "viewer slot 0 role=") >= 1);
    }
}

namespace {

// A session that has NOT reached NETGAME.STATE_INMATCH, with entrant 0 readable and seated on a
// playing team -- the state L3 #28 recorded at the first readable tick, and the one AUD-W2-B-31
// F1 is about.
void MakePreMatchViewer(RosterFixture& f, uint16_t role) {
    const uint16_t locals = 1;
    memcpy(f.netGame.bytes + facts::LOCAL_NETGAME_LOCAL_USER_COUNT_OFF, &locals, sizeof locals);
    PutRole(f.netGame.bytes, 0, role);
    f.netGame.SetState(facts::NETGAME_STATE_INMATCH - 1);
}

}  // namespace

// First half. Before state 9 the engine has not been asked for a seat, so the
// census must not report entrant 0's playing team as a VERDICT about one. The rendering is the
// thing under test, so the line is compared whole rather than by its prefix.
TEST_CASE("W2-B-31 F1: the pre-match census renders the role as not yet seated") {
    const PlaybackSessionEnv playbackSession;
    RosterFixture f;
    MakePreMatchViewer(f, 1);
    const size_t from = TheEnv().log.lines.size();

    f.Tick();

    CHECK(CountLoggedSince(from, "identity", "viewer slot 0 role=1 (not yet seated)") == 1);
    // The only line rendering the raw name pre-match is the PRESEAT measurement, which is a
    // reading of the record and not a verdict about a seat; the census itself must not.
    CHECK(CountLoggedSince(from, "identity", "viewer slot 0 role=1 (orange) preseat") == 1);
    CHECK(CountLoggedSince(from, "identity", "viewer slot 0 role=1 (orange)") == 1);
    // And the deferral says the question WAS asked, so the line above is not silence.
    CHECK(CountLoggedSince(from, "identity", "viewer seat: deferred (SessionNotInMatch)") == 1);

    // Past state 9 the same reading renders as the verdict it now is.
    f.netGame.SetState(facts::NETGAME_STATE_INMATCH);
    for (int i = 0; i < 64; ++i) f.Tick();
    CHECK(CountLoggedSince(from, "identity", "viewer slot 0 role=1 (orange)") >= 2);
}

// Second half. `ViewerNotSpectator` is in the acceptance's refusal set, so a
// build that raises it while the session is still walking up to state 9 makes the clause
// unsatisfiable by a run that goes on to seat correctly. The suppression is asserted with its
// own control: the SAME fixture, past state 9 and with no hop surface to seat through, must
// raise it -- otherwise this case would pass on a build that never prints the word at all.
TEST_CASE("W2-B-31 F1: ViewerNotSpectator is not raised before state 9, and is after") {
    const PlaybackSessionEnv playbackSession;
    RosterFixture f;
    f.id.hopSurface = nullptr;   // nothing can seat, so the verdict is the only variable
    MakePreMatchViewer(f, 1);
    const size_t from = TheEnv().log.lines.size();

    for (int i = 0; i < 128; ++i) f.Tick();
    CHECK(CountLoggedSince(from, "identity", "ViewerNotSpectator") == 0);
    CHECK(CountLoggedSince(from, "identity", "viewer slot 0 role=1 (not yet seated)") >= 1);

    const size_t after = TheEnv().log.lines.size();
    f.netGame.SetState(facts::NETGAME_STATE_INMATCH);
    for (int i = 0; i < 64; ++i) f.Tick();
    CHECK(CountLoggedSince(after, "identity", "ViewerNotSpectator") >= 1);
}

// THE WIRE, not the unit -- the same distinction P1-30e drew for the recorded loadout.
// Nothing in this case arms anything: the fixture installs an identity that offers a hop surface,
// the SHIPPED Load path arms the provisioner, and the pump drives the chain. With no surface bound
// (`hops: unasked`) PawnProvisioner::Settle takes every admitted entrant straight to Provisioned
// and never enters AdvanceSlot -- no enable, no place, no physics CI, no dress -- and the census
// still says `provisioned 1`. So the reading that separates the two builds is asserted here: the
// engine was actually READ, at the live index the remap chose, and the count is the number of
// entrants whose every hop answered done.
TEST_CASE("W2-B-HOPS: the shipped Load arms the hop surface, and the pump drives it to Done") {
    RosterFixture f;
    DoneHopEngine eng;
    replay::PawnProvision::ActorHops h = DoneHopsFor(eng);
    f.id.hopSurface = &h;   // before Load: the arm is the LOAD's, not a later tick's
    f.id.Seat(0);           // the viewer holds recorded slot 0, so the remap moves this entrant

    const uint16_t rosterSlots[1] = {0};
    TempDemo tmp;
    // A file that RECORDED a team. A3 is an authored step and it refuses TeamNotRecorded for a row
    // that carries none, so a chain driven over the default row reads every hop done and is still
    // failed for want of an issue -- which is the correct answer to a different question.
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, rosterSlots, 1, 0, false, 1));
    const size_t from = TheEnv().log.lines.size();
    REQUIRE(f.e.services.transport->Load(tmp.path));

    // (1) THE ARM BOUND IT, and bound the identity's own surface rather than a copy: `hops.ctx`
    // names the surface object, so a copy would drive a different engine than the one asserted on.
    CHECK(replay::playback::ArmedActorHopsForTest() == static_cast<const void*>(&h));
    CHECK(CountLoggedSince(from, "pawn", "hops: HopsUnbound") == 0);

    // (2) AND THE PUMP DROVE IT. The window line is on the status cadence, so the loop pumps until
    // the count appears rather than assuming which tick prints it; the bound is small enough that a
    // chain that never advances still fails.
    for (int i = 0; i < 40 && CountLoggedSince(from, "pawn", "provisioned 1, failed 0") == 0; ++i) {
        f.Tick();
    }
    CHECK(eng.reads > 0);
    CHECK(eng.sawIndex == IndexForSlot(1));
    CHECK(CountLoggedSince(from, "pawn", "provisioned 1, failed 0") >= 1);
    CHECK(CountLoggedSince(from, "pawn", "physicsCi=none") >= 1);
    CHECK(CountLoggedSince(from, "pawn", "hops: 0->1=Done") >= 1);
    CHECK(CountLoggedSince(from, "pawn", "hops: unasked") == 0);
}

namespace {

// Answers for LIVE slot 1 and for nothing else, and records every index the surfaces were asked
// about. Both halves matter: a bind that asks the recorded slot gets no pawn here, and the record
// says which number it asked rather than leaving that to be inferred from a refusal count.
class SlotAskEntity final : public SwitchableEntity {
public:
    mutable std::vector<uint16_t> asked;

    replay::PlayerHandle PlayerOf(replay::UserIndex i) const override {
        asked.push_back(i.v);
        return i.v == 1 ? replay::PlayerHandle{61}
                         : replay::PlayerHandle{replay::playback::kPawnAbsent};
    }
    replay::PhysHandle PhysOf(replay::PlayerHandle p) const override {
        return p.v == 61 ? replay::PhysHandle{0x31}
                         : replay::PhysHandle{replay::playback::kPawnAbsent};
    }
};

struct SlotAskFixture {
    Env& e;
    RosterIdentity id;
    SlotAskEntity ent;
    CountingPose pose;
    ArmNetGame netGame;
    replay::IIdentity* savedId;
    replay::IEntity* savedEnt;
    replay::IPosePublisher* savedPose;

    SlotAskFixture() : e(TheEnv()) {
        replay::playback::ForgetSessionEntrantsForTest();
        // ...and the leave feed with it: this rig never drives the stage-3 tail that drains it,
        // so a queue left by an earlier case would defer every later case's dress pass.
        replay::identity::ForgetRosterLeave();   // fresh session -- see RosterFixture
        savedId = e.services.identity;
        savedEnt = e.services.entity;
        savedPose = e.services.pose;
        ent.available = true;
        e.services.identity = &id;
        e.services.entity = &ent;
        e.services.pose = &pose;
        Tick();
    }
    void Tick() {
        // A real engine update feeds the leave queue before anything else this tick does; this rig
        // has no engine update, so the one pass the stage-3 tail would make is made here.
        replay::playback::ServiceLeaveFeedForTest();
        replay::TickInfo info{};
        info.netGame = netGame.bytes;
        e.tick.FireAll(info);
    }
    ~SlotAskFixture() {
        e.services.transport->Unload();
        e.services.identity = savedId;
        e.services.entity = savedEnt;
        e.services.pose = savedPose;
        e.tick.FireAll(replay::TickInfo{});
    }
};

}  // namespace

TEST_CASE("SLOTMAP: the bind asks the PROVISIONED slot, never the recorded one the viewer holds") {
    // AUD-W2-P1-29 R2.9. PawnProvisioner maps recorded slot 0 -> live slot 1 because the viewer
    // already holds 0, and PawnResolver::SetSlotMap had no caller in src/ -- so the bind asked the
    // surfaces about slot 0 and L3 #27 read `0 bound (0 by slot 65535->65535), 1 no player ci`.
    SlotAskFixture f;
    f.id.Seat(0);
    const uint16_t slots[1] = {0};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 1, 0, true));
    const size_t from = TheEnv().log.lines.size();
    REQUIRE(f.e.services.transport->Load(tmp.path));
    // Load binds before any tick has run the dress pass, so THAT bind has no map to ask through
    // and asks slot 0 -- it binds nothing and prints `0 bound (0 by slot 65535->65535`, which is
    // exactly what L3 #27 recorded. The asks under test are the ones taken after the map exists.
    f.ent.asked.clear();
    f.Tick();   // the dress pass maps the slot and re-takes the bind through the map
    f.Tick();   // the position line the re-bind armed

    CHECK(CountLoggedOpeningSince(from, "pawn", "recorded slot 0 -> slot 1") == 1);
    // The bind summary is the sentence a launch quotes back. BOTH numbers print, so a map that
    // quietly did not fire reads `0->0` and cannot be mistaken for this.
    CHECK(CountLoggedSince(from, "playback", "1 bound (1 by slot 0->1") == 1);
    CHECK(CountLoggedSince(from, "playback", "by slot 0->0") == 0);
    // The pose line names the LIVE slot, so it correlates with the engine's own `added to slot 1`.
    CHECK(CountLoggedSince(from, "playback", "pawn slot 1 ent=") >= 1);
    CHECK(CountLoggedSince(from, "playback", "pawn slot 0 ent=") == 0);

    size_t askedZero = 0, askedOne = 0;
    for (size_t i = 0; i < f.ent.asked.size(); ++i) {
        if (f.ent.asked[i] == 0) ++askedZero;
        if (f.ent.asked[i] == 1) ++askedOne;
    }
    INFO("surface asks: zero=", askedZero, " one=", askedOne, " total=", f.ent.asked.size());
    CHECK(askedOne > 0);
    CHECK(askedZero == 0);
}

TEST_CASE("SLOTMAP: a 3-player roster maps every recorded slot and the map reaches the bind") {
    // AUD SLOTMAP S1. The dress pass runs ONCE per load; whatever maps after it must still reach
    // the resolver, or those players are bound on their RECORDED slots for the life of the session.
    SlotAskFixture f;
    f.id.Seat(0);   // the viewer
    const uint16_t slots[3] = {0, 1, 2};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 3, 0, true));
    const size_t from = TheEnv().log.lines.size();
    REQUIRE(f.e.services.transport->Load(tmp.path));
    f.Tick();
    const size_t afterDress = TheEnv().log.lines.size();
    const size_t mappedAtDress = CountLoggedOpeningSince(from, "pawn", "recorded slot ");
    for (int i = 0; i < 4; ++i) f.Tick();

    const size_t mappedLater = CountLoggedOpeningSince(afterDress, "pawn", "recorded slot ");
    const size_t installedLater = CountLoggedSince(afterDress, "playback", "(slot map installed)");
    INFO("mapped at dress ", mappedAtDress, " later ", mappedLater, " installs later ",
         installedLater);
    // Every recorded slot is displaced: the viewer holds 0, and each admission claims the next.
    CHECK(CountLoggedOpeningSince(from, "pawn", "recorded slot 0 -> slot 1") == 1);
    CHECK(CountLoggedOpeningSince(from, "pawn", "recorded slot 1 -> slot 2") == 1);
    CHECK(CountLoggedOpeningSince(from, "pawn", "recorded slot 2 -> slot 3") == 1);
    // The map reached the resolver, and then STOPPED re-binding: a refresh with no change detection
    // would re-bind on every quiet tick, which is a second defect.
    CHECK(CountLoggedSince(from, "playback", "(slot map installed)") >= 1);
    CHECK(mappedAtDress == 3);
    CHECK(installedLater == 0);
}

TEST_CASE("SLOTMAP: a mapping made AFTER the dress pass still reaches the bind") {
    // AUD SLOTMAP S6. That no mapping lands after the dress pass today is a five-part conjunction
    // across two modules -- Request has no src/ caller, Admit bypasses Pump's slice, AttemptDue's
    // window is per-pump, and the two occupancy refusals latch -- and nothing here asserts any of
    // it, so the per-tick refresh had no case that could fail. This queues the slot the way a
    // second producer would and drives the pass that maps it.
    SlotAskFixture f;
    f.id.Seat(0);   // the viewer
    const uint16_t slots[1] = {0};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 1, 0, true));
    const size_t from = TheEnv().log.lines.size();
    REQUIRE(f.e.services.transport->Load(tmp.path));
    f.Tick();
    REQUIRE(CountLoggedOpeningSince(from, "pawn", "recorded slot 0 -> slot 1") == 1);

    // A foreign entrant takes slot 2, so the queued recorded slot 2 cannot map onto itself: the
    // mapping it gets is a REAL change to the map and not the identity entry already installed.
    f.id.Seat(2);
    const size_t afterDress = TheEnv().log.lines.size();
    REQUIRE(replay::playback::RequestPawnSlotForTest(2));
    // The retry cadence is counted in pumps, so the mapping lands some pumps later -- which is the
    // point: it is a tick the dress pass will never run on again.
    for (int i = 0; i < 150; ++i) f.Tick();

    const size_t installed = CountLoggedSince(afterDress, "playback", "(slot map installed)");
    INFO("installs after the dress pass: ", installed);
    CHECK(installed == 1);
    // The re-bind still asks the provisioned slot for the player the file does carry.
    CHECK(CountLoggedSince(afterDress, "playback", "1 bound (1 by slot 0->1") == 1);
    CHECK(CountLoggedSince(afterDress, "playback", "by slot 0->0") == 0);
}

TEST_CASE("L3 #12: the shipped load path pumps a census that names the SURFACE, not just a count") {
    // THE TEST 1b883d4 SHIPPED WITHOUT. `ProvisionEnv::entity/log/stateRaw` are assigned in
    // TryDressRoster and read by nothing else, so deleting all three left every armtest case
    // green -- an equivalent mutant, which is a missing test and not a clean bill. The census is
    // their only observable: wired, the tick pumps and the three lines below print; with any one
    // unset the census either says nothing at all, or says UNKNOWN about a state the gate above
    // it had already proved.
    RosterFixture f;
    f.id.Seat(0);
    const uint16_t slots[1] = {0};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 1));
    replay::IReplayTransport* t = f.e.services.transport;
    const size_t from = TheEnv().log.lines.size();
    REQUIRE(t->Load(tmp.path));
    f.Tick();

    // Controls. The census is skipped unless a pawn was provisioned, and it REFUSES by name on
    // an unavailable surface -- a fixture failing either would report "no census line" as a
    // wiring failure when the wiring was never asked.
    REQUIRE(IdentitySnapshot().mapped == 1);
    REQUIRE(f.ent.available);
    REQUIRE(CountLoggedSince(from, "pawn", "census REFUSED") == 0);

    // env.log AND env.entity: the census block runs only when both are non-null.
    CHECK(CountLoggedSince(from, "pawn",
                           "census: provisioned 1, mapped 1, failed 0 -- state=known inmatch=yes")
          == 1);
    // env.entity again, separately: this line prints only after Available() answered ON the
    // surface the env carries, and it quotes that surface own counts.
    CHECK(CountLoggedSince(from, "pawn", "census pools: remote CIs=0, phys CIs=16") == 1);

    // env.stateRaw: derived from the fact, never spelled. A needle holding a literal 9 would keep
    // passing over a changed NETGAME_STATE_INMATCH, which is the one thing this line reports.
    char stateLine[64];
    snprintf(stateLine, sizeof stateLine, "census: stateRaw=%d",
             (int)facts::NETGAME_STATE_INMATCH);
    CHECK(CountLoggedSince(from, "pawn", stateLine) == 1);

    // One census per epoch: a second tick adds no second header, so the count above is a
    // property of the census and not of how many ticks this case happened to run.
    f.Tick();
    CHECK(CountLoggedSince(from, "pawn", "census: provisioned 1,") == 1);
}

TEST_CASE("the position line reports the DRIVEN pose at the presented time, not a recorded frame") {
    // WriteDemo writes com[0] = the frame index, so the pose at a playhead IS the frame number
    // there. A line printing the nearest resident recorded frame instead would read the window's
    // first sample at every seek -- a frozen replay that looks like it is moving.
    RosterFixture f;
    const uint16_t slots[1] = {2};
    TempDemo tmp;
    const uint64_t dt = 11111;
    REQUIRE(WriteDemo(tmp.path, 5000000, dt, 60, 4, 1, 2000000, slots, 1));
    replay::IReplayTransport* t = f.e.services.transport;
    REQUIRE(t->Load(tmp.path));
    f.Tick();   // the dress pass runs on a tick, not inside Load

    auto pos_after = [](size_t from) -> float {
        const std::vector<Line>& lines = TheEnv().log.lines;
        for (size_t i = lines.size(); i > from; --i) {
            const std::string& s = lines[i - 1].text;
            const size_t p = s.find("pawn slot ");
            const size_t q = s.find("driven=(");
            if (lines[i - 1].module != "playback" || p == std::string::npos ||
                q == std::string::npos) {
                continue;
            }
            return (float)atof(s.c_str() + q + 8);
        }
        return -1.0f;
    };

    // Tolerance is one tenth of the gap between the two probes: wall time passes between the seek
    // and the tick that prints, so the playhead drifts a little, but it cannot drift 8 frames.
    for (uint32_t frame = 40; frame <= 120; frame += 80) {
        const size_t from = TheEnv().log.lines.size();
        t->Seek((int64_t)(frame * dt));
        f.Tick();
        const float com = pos_after(from);
        INFO("frame " << frame << " printed com[0]=" << com);
        CHECK(com > (float)frame - 8.0f);
        CHECK(com < (float)frame + 8.0f);
    }
}

TEST_CASE("a load before the session is in match DEFERS the dress and runs it on the tick that is") {
    // THE ORDERING THIS PACKET TURNS ON. NoNetGame and SessionNotInMatch are DEFERRALS, and the
    // dress pass first runs inside Load -- before the level is up, before any tick has carried a
    // session. Read as refusals they cleared `pending` and the roster stayed undressed for the rest
    // of the load, which is the whole L3 #11 symptom arriving one step earlier.
    RosterFixture f;
    const uint16_t slots[1] = {2};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 1));
    replay::IReplayTransport* t = f.e.services.transport;

    // The names are taken from the producer's own table, not re-spelled here: a deferral reported
    // under some other word is a deferral the operator cannot act on.
    char no_session[256], not_in_match[256];
    _snprintf_s(no_session, sizeof no_session, _TRUNCATE, "identity dressing deferred: %s",
                replay::PawnProvision::RefusalText(replay::PawnProvision::Refusal::NoNetGame));
    _snprintf_s(not_in_match, sizeof not_in_match, _TRUNCATE, "identity dressing deferred: %s",
                replay::PawnProvision::RefusalText(
                    replay::PawnProvision::Refusal::SessionNotInMatch));

    f.netGame.SetState(0);   // NETGAME.STATE_OFF: the level is still loading
    const size_t from = TheEnv().log.lines.size();
    REQUIRE(t->Load(tmp.path));
    f.Tick();

    IdentityStatus st = IdentitySnapshot();
    CHECK(st.pending);              // still owed, not failed
    CHECK(st.mapped == 0);
    CHECK(st.deferred >= 1);
    CHECK(CountLoggedSince(from, "playback", not_in_match) == 1);
    CHECK(CountLoggedSince(from, "pawn", "waiting: ") >= 1);

    // A tick carrying no session at all is the other deferral, and it is named differently.
    f.e.tick.FireAll(replay::TickInfo{});
    CHECK(CountLoggedSince(from, "playback", no_session) == 1);
    CHECK(IdentitySnapshot().pending);

    // The match starts. The pass that was deferred runs now, on the tick that can carry it.
    f.netGame.SetState(facts::NETGAME_STATE_INMATCH);
    f.Tick();
    st = IdentitySnapshot();
    CHECK_FALSE(st.pending);
    CHECK(st.players == 1);
    CHECK(st.mapped == 1);
    CHECK(CountLoggedSince(from, "playback", "identity dressing ran after") == 1);
    // One line per distinct blocker, not one per tick: the wait outlasts thousands of frames.
    CHECK(CountLoggedSince(from, "playback", not_in_match) == 1);
    CHECK(CountLoggedSince(from, "playback", no_session) == 1);
}

TEST_CASE("L3 #26: the provisioner pumps across the whole wait -- not only once the dress pass runs") {
    // ★★★ RUN-26, REPRODUCED IN A TEST. Provisioning enabled, the file loaded, the reply route's
    // arm never completing on the solo offline host, and the dress pass deferring for the whole
    // span the engine needs to come up. On the build that flew, `[pawn] window` printed ZERO times
    // in a 465-line log: the arm sat BELOW the dress gate, so on a session that was not yet in
    // match nothing was ever armed and therefore nothing was ever pumped. The engine seated slot 1
    // at 23:16:03; the plugin's last `[pawn]` line was 23:15:58.994. Every CHECK below reads 0
    // against that build, and the two REQUIREs above them are the controls that keep this case
    // from passing for the wrong reason.
    RosterFixture f;
    f.netGame.SetState(0);     // NETGAME.STATE_OFF: the level is still coming up
    f.ent.available = false;   // "entity surface not bound to a game space", the other deferral
    const uint16_t slots[1] = {2};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 1));
    replay::IReplayTransport* t = f.e.services.transport;
    const size_t from = TheEnv().log.lines.size();
    REQUIRE(t->Load(tmp.path));
    for (uint32_t i = 0; i < 4; ++i) f.Tick();

    // CONTROL 1 -- the dress pass really is shut for the whole case. Without this the pump could
    // be running because the roster dressed, which is the state that was never in question.
    REQUIRE(IdentitySnapshot().pending);
    REQUIRE(IdentitySnapshot().mapped == 0);
    // CONTROL 2 -- and the reply route really is NOT armed here, which is the other thing the pump
    // must not depend on. This harness has no engine broadcaster to register on, so the route
    // refuses exactly as the solo host does; if it ever started arming, this case would be proving
    // nothing and says so by failing rather than by passing quietly.
    REQUIRE(CountLoggedSince(from, "pawn", "reply route: not armed --") >= 1);

    // ★ AND THE PUMP RAN ANYWAY, FROM THE FIRST TICK. `window 0` is the provisioner's first
    // cadence window: a run whose process dies at 51 s still carries it.
    CHECK(CountLoggedSince(from, "pawn", "window 0:") == 1);
    // What it says is the empty table, not a fabricated one -- the counts report the work, the
    // line reports the pump.
    CHECK(CountLoggedSince(from, "pawn", "pending 0, awaiting 0, provisioned 0, failed 0") >= 1);

    // ★ AND WHEN THE SESSION FINALLY COMES UP the same armed provisioner takes the roster: the
    // early arm did not consume the epoch the dress pass needs.
    f.ent.available = true;
    f.netGame.SetState(facts::NETGAME_STATE_INMATCH);
    f.Tick();
    CHECK_FALSE(IdentitySnapshot().pending);
    CHECK(IdentitySnapshot().mapped == 1);
    CHECK(CountLoggedSince(from, "pawn", "provisioned 1,") >= 1);
}

// The viewer's SEAT is decided by ApplyCommandLine and the admit path, in
// the lobby, before any .echodemo is opened and whether or not one ever is -- so the census that
// reports it must not sit behind the demo-load gate. It did, and the cost was not a missing line:
// L3 #28's acceptance is "our line, and NO ViewerNotSpectator anywhere", so a session whose fixture
// failed to open satisfied the absence half by never asking the question. Asked-and-passed and
// never-asked are not the same answer, and a two-state check cannot tell them apart.
//
// NOTHING IS LOADED HERE. This case calls no Load(): the only thing it does is tick a session that
// has an entrant 0 to read. VIEWER_LINE_BEHIND_LOAD puts the call back under the gate, which is the
// shipped code as of f6b066e, and under it this case sees zero lines.
TEST_CASE("the viewer's seat is reported on a session with no demo loaded") {
    const PlaybackSessionEnv playbackSession;
    const size_t from = TheEnv().log.lines.size();
    RosterFixture f;   // its constructor ticks once, and no transport Load is called anywhere here

    // The rig starts zeroed, so the local-user count is 0: there is no entrant 0 and the word at
    // USER_ARRAY_OFF is whatever was last there. The line must be WITHHELD, not guessed -- without
    // this the case below could not tell "reported the seat" from "reports on every tick".
    CHECK(CountLoggedSince(from, "identity", "viewer slot 0 role=") == 0);

    // Now the record is readable and entrant 0 is on the engine's own spectator team. TeamOf reads
    // the packed word at USER_ARRAY_OFF and takes IDENT_USER_TEAM_MASK bits from
    // IDENT_USER_TEAM_SHIFT up, so the team is written through those, never as a raw 2.
    const uint16_t users = 1;
    memcpy(f.netGame.bytes + facts::LOCAL_NETGAME_LOCAL_USER_COUNT_OFF, &users, sizeof users);
    const uint16_t seat = (uint16_t)((facts::GAME_TEAM_SPECTATOR & facts::IDENT_USER_TEAM_MASK)
                                     << facts::IDENT_USER_TEAM_SHIFT);
    memcpy(f.netGame.bytes + facts::USER_ARRAY_OFF, &seat, sizeof seat);
    f.Tick();

    // TWO lines, and they are different readings: the pre-seat measurement ARCH-23 S14.10 asks for
    // once a session, and the census verdict. Counting only the shared prefix would let either one
    // stand in for the other.
    CHECK(CountLoggedSince(from, "identity", "viewer slot 0 role=") == 2);
    CHECK(CountLoggedSince(from, "identity", "viewer slot 0 role=2 (spectator) preseat") == 1);
    CHECK(CountLoggedSince(from, "identity", "preseat") == 1);
    // The seat is a spectating one, so the refusal must NOT fire -- an absence that means something
    // only because the line above proves the question was asked at all.
    CHECK(CountLoggedSince(from, "identity", "ViewerNotSpectator") == 0);
    // No `lobbyteam=` field: GAME.TEAM_OFF is reset to -1 before the level loads, so that reading
    // could only ever print -1 and was being read as evidence (S14.10, upheld).
    CHECK(CountLoggedSince(from, "identity", "lobbyteam=") == 0);
    // PER-WINDOW, not per tick and not latched: V4 re-reads the record every census window, so a
    // line printed once could say only what the role WAS the first time anybody looked -- but a
    // line printed every frame would drown the log it is read from. A second tick inside the same
    // window adds nothing.
    f.Tick();
    CHECK(CountLoggedSince(from, "identity", "viewer slot 0 role=") == 2);
}

// ---------------------------------------------------------------------------------------------
// W2-B POSEREAD. The owner's clause is "it will look the same as it did before ... physics", and a
// pose line printing only what WE wrote cannot answer whether the engine holds it. These four pin
// the two-reads rule: driven= and read= come from different places, a refused read says so by name
// rather than printing a pose-shaped zero, and an install the identity gate declines is not a write.

namespace {

// A ring that keeps what was installed and hands it back with a deliberate bias, so the printed
// delta has a KNOWN non-zero value. A fake echoing the write back exactly would make delta 0.000 for
// a correct line and for a line that never read the engine at all -- the same number twice.
class BiasedRingEntity final : public SwitchableEntity {
public:
    float bias = 0.0f;             // metres added to com[0] on the way back out
    bool  refuse = false;          // the read-back refuses at a named gate
    replay::BodyRefusal refusal = replay::BodyRefusal::CiEntityMismatch;
    bool  refuse_install = false;  // the install refuses the way CheckCiIdentity does
    size_t identity_refusals = 0;

    bool SetHistoryWindowDetailed(replay::PhysHandle h, const replay::BodyState* samples,
                                  const uint64_t* timesUs, size_t n,
                                  replay::InstallReading& ir) override {
        (void)h;
        ir = replay::InstallReading();
        ir.from = replay::CiPath::Pool;
        if (refuse_install) {
            ++identity_refusals;
            ir.why = replay::InstallRefusal::CiIdentityMismatch;
            return false;
        }
        if (!samples || !timesUs || n == 0) return false;
        held_ = samples[0];
        held_t_ = timesUs[0];
        have_ = true;
        ++installs;
        return true;
    }

    size_t ReadHistoryWindow(replay::PhysHandle, replay::BodyState* out, uint64_t* t,
                             size_t cap) const override {
        if (!have_ || cap == 0) return 0;
        *out = held_;
        out->com[0] += bias;
        *t = held_t_;
        return 1;
    }

    bool ReadBodyDetailed(replay::PhysHandle, replay::BodyState& out,
                          replay::BodyReading& r) const override {
        r = replay::BodyReading();
        if (refuse) {
            r.refusedAt = refusal;
            r.source = replay::BodySource::None;
            return false;
        }
        if (!have_) {
            r.source = replay::BodySource::None;
            return false;
        }
        out = held_;
        out.com[0] += bias;
        r.source = replay::BodySource::History;
        r.historyPresent = true;
        return true;
    }

    // Plant a pose the engine "already holds", distinct from anything the applier will install.
    // It is what a pre-Apply reading must return and what a post-Apply one cannot.
    void Plant(float x) {
        held_ = replay::BodyState();
        held_.com[0] = x;
        have_ = true;
    }

private:
    replay::BodyState held_ = {};
    uint64_t held_t_ = 0;
    bool have_ = false;
};

// The last `pawn slot` line the module printed since `from`, or "" if it printed none.
std::string LastPawnLine(size_t from) {
    const std::vector<Line>& lines = TheEnv().log.lines;
    for (size_t i = lines.size(); i > from; --i) {
        if (lines[i - 1].module != "playback") continue;
        if (lines[i - 1].text.find("pawn slot ") == std::string::npos) continue;
        return lines[i - 1].text;
    }
    return std::string();
}

}  // namespace

TEST_CASE("POSEREAD: the pose line reads the pose BACK from the engine and prints the delta") {
    RosterFixture f;
    BiasedRingEntity ring;
    ring.available = true;
    ring.bias = 3.0f;   // a read-back three metres from what we drove
    f.e.services.entity = &ring;
    const uint16_t slots[1] = {2};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 4, 1, 2000000, slots, 1));
    REQUIRE(f.e.services.transport->Load(tmp.path));
    f.Tick();
    // The engine "already holds" this pose when the printing tick starts. Only a reading taken
    // before Apply can see it.
    ring.Plant(555.0f);
    // The pose line is printed once a second of wall time OR on the tick after a seek, and a
    // unit test does not sit still for a second. The seek is what arms it.
    const size_t from = TheEnv().log.lines.size();
    f.e.services.transport->Seek(1000000);
    f.Tick();

    const std::string line = LastPawnLine(from);
    INFO("line: " << line);
    REQUIRE(!line.empty());
    // BOTH poses on the line, from two different places. A build printing only the source sample
    // fails here, and so does one printing the read-back twice.
    CHECK(line.find("driven=(") != std::string::npos);
    CHECK(line.find("read=(") != std::string::npos);
    CHECK(line.find("src=history") != std::string::npos);
    // THE TWO NUMBERS, not merely the two field names. A build that prints the source sample under
    // both labels still carries `driven=(`, `read=(` and the right delta -- only the difference
    // between the printed poses can tell it from a line that asked the engine.
    const size_t dp = line.find("driven=(");
    const size_t rp = line.find("read=(");
    REQUIRE(dp != std::string::npos);
    REQUIRE(rp != std::string::npos);
    const float printed_driven = (float)atof(line.c_str() + dp + 8);
    const float printed_read = (float)atof(line.c_str() + rp + 6);
    INFO("driven " << printed_driven << " read " << printed_read);
    CHECK(printed_read - printed_driven > 2.9f);
    CHECK(printed_read - printed_driven < 3.1f);
    // The bias is the whole point: a line whose delta is 0.000 while the ring hands back a pose
    // three metres away is not reading the ring.
    CHECK(line.find("delta=3.000") != std::string::npos);
    CHECK(line.find("delta=0.000") == std::string::npos);
    CHECK(ring.installs > 0);

    // `read_next=` is the reading taken BEFORE this tick's install -- the ring as the previous
    // window left it. It is the only field on the line that is not an echo of this tick's write,
    // and motion is read from it CHANGING across windows.
    CHECK(line.find("read_next=(") != std::string::npos);
    CHECK(line.find("src_next=history") != std::string::npos);
    // THE VALUE, not the field name. `read_next=` is only worth printing if it is the reading taken
    // BEFORE this tick's install: a snapshot moved after Apply still prints a parenthesised pose
    // with src_next=history and a plausible delta, and every presence check passes (AUD MAJOR 4).
    // The planted pose is 555 and the ring's bias is 3, so a pre-Apply reading is 558 and a
    // post-Apply one is the driven pose plus 3 -- nowhere near it.
    const size_t np = line.find("read_next=(");
    REQUIRE(np != std::string::npos);
    const float printed_next = (float)atof(line.c_str() + np + 11);
    INFO("read_next " << printed_next << " driven " << printed_driven);
    CHECK(printed_next > 557.9f);
    CHECK(printed_next < 558.1f);
    // ... and its delta is measured against the DRIVEN pose of this window, not against the ring.
    const size_t ndp = line.find("delta_next=");
    REQUIRE(ndp != std::string::npos);
    const float printed_next_delta = (float)atof(line.c_str() + ndp + 11);
    const float expect_next_delta = printed_next - printed_driven;
    CHECK(printed_next_delta > expect_next_delta - 0.1f);
    CHECK(printed_next_delta < expect_next_delta + 0.1f);
    CHECK(line.find("delta_next=3.000") == std::string::npos);

    // The POSITIVE direction of installed-but-unread: the install returned true and the ring hands
    // back a pose three metres away, so the write is NOT counted as read back. Every other case
    // pins the zero direction, which a counter that never increments would also satisfy.
    const replay::playback::ApplyStats a = replay::playback::ApplyStatsSnapshot();
    CHECK(a.bodies_installed > 0);
    CHECK(a.bodies_installed_unread == a.bodies_installed);
    CHECK(a.bodies_read_back == 0);
}

TEST_CASE("POSEREAD: a read-back that refuses prints the gate's NAME, and no delta") {
    RosterFixture f;
    BiasedRingEntity ring;
    ring.available = true;
    ring.refuse = true;
    ring.refusal = replay::BodyRefusal::CiEntityMismatch;
    f.e.services.entity = &ring;
    const uint16_t slots[1] = {2};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 4, 1, 2000000, slots, 1));
    REQUIRE(f.e.services.transport->Load(tmp.path));
    f.Tick();
    // The pose line is printed once a second of wall time OR on the tick after a seek, and a
    // unit test does not sit still for a second. The seek is what arms it.
    const size_t from = TheEnv().log.lines.size();
    f.e.services.transport->Seek(1000000);
    f.Tick();

    const std::string line = LastPawnLine(from);
    INFO("line: " << line);
    REQUIRE(!line.empty());
    CHECK(line.find("read=ciEntityMismatch") != std::string::npos);
    // No pose-shaped number where there was no reading, and no distance from nothing.
    CHECK(line.find("read=(") == std::string::npos);
    CHECK(line.find("delta=n/a") != std::string::npos);
    // A refused read still reports which source was asked. `none` is not `notAsked`.
    CHECK(line.find("src=none") != std::string::npos);
}

TEST_CASE("POSEREAD: an install the identity gate declines is COUNTED BY NAME, never as a write") {
    RosterFixture f;
    BiasedRingEntity ring;
    ring.available = true;
    ring.refuse_install = true;
    f.e.services.entity = &ring;
    const uint16_t slots[1] = {2};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 4, 1, 2000000, slots, 1));
    REQUIRE(f.e.services.transport->Load(tmp.path));
    f.Tick();
    f.Tick();

    const replay::playback::ApplyStats a = replay::playback::ApplyStatsSnapshot();
    CHECK(ring.identity_refusals > 0);
    // The engine declined to vouch for the element, so nothing was driven into it -- and the
    // session says WHY instead of reporting a generic install failure.
    CHECK(a.bodies_installed == 0);
    CHECK(a.bodies_read_back == 0);
    CHECK(a.bodies_install_failed > 0);
    CHECK(a.bodies_install_identity_refused == a.bodies_install_failed);
}

TEST_CASE("POSEREAD: an install that lands is counted as READ BACK, not merely as written") {
    RosterFixture f;
    BiasedRingEntity ring;
    ring.available = true;
    ring.bias = 0.0f;   // the ring hands back exactly what was installed
    f.e.services.entity = &ring;
    const uint16_t slots[1] = {2};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 4, 1, 2000000, slots, 1));
    REQUIRE(f.e.services.transport->Load(tmp.path));
    f.Tick();
    f.Tick();

    const replay::playback::ApplyStats a = replay::playback::ApplyStatsSnapshot();
    CHECK(a.bodies_installed > 0);
    CHECK(a.bodies_read_back == a.bodies_installed);
    CHECK(a.bodies_installed_unread == 0);
    CHECK(a.bodies_install_identity_refused == 0);
}

// ============ ENG-SEAT-PREDICATE: the session-mode predicate, all three answers ============
//
// Nothing in src/ ever sets ECHOREPLAY_OPEN, so a demo the owner opens at
// RUNTIME -- the panel's browser click serviced by ui::LoadPath, or the console `load` -- reached
// the old predicate as "not a playback session" and the seat was refused for the life of the
// process. The session is a playback because THIS MODULE HAS A DEMO OPEN, and that is what this
// case drives: no carrier in the environment at all.
TEST_CASE("SEAT-PREDICATE: a demo opened at RUNTIME seats, with no launch carrier") {
    SetEnvironmentVariableW(replay::bootstrap::OpenEnvVarName(), nullptr);
    REQUIRE(replay::bootstrap::OpenRequestPresent() == false);
    REQUIRE(replay::capture::CaptureIsRecording() == false);

    RosterFixture f;
    ViewerSeatEngine eng;
    eng.ng = f.netGame.bytes;
    replay::PawnProvision::ActorHops h{};
    h.ctx = &eng;
    h.assignTeam = ViewerSeatAssign;
    f.id.hopSurface = &h;
    const uint16_t locals = 1;
    memcpy(f.netGame.bytes + facts::LOCAL_NETGAME_LOCAL_USER_COUNT_OFF, &locals, sizeof locals);
    PutRole(f.netGame.bytes, 0, 1);

    // Nothing is open yet, so the seat is refused -- and this is the state the shipped process is
    // in from Init until the owner picks a file.
    CHECK(replay::playback::PlaybackOwnsSession() == false);

    TempDemo tmp;
    const uint16_t slots[2] = {2, 4};
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 2));
    const size_t from = TheEnv().log.lines.size();
    REQUIRE(f.e.services.transport->Load(tmp.path));
    f.Tick();

    CHECK(replay::playback::PlaybackOwnsSession() == true);
    CHECK(eng.calls == 1);
    CHECK(eng.sawIndex == 0);
    CHECK(eng.sawTeam == uint16_t(facts::GAME_TEAM_SPECTATOR));
    CHECK(GetRole(f.netGame.bytes, 0) == uint16_t(facts::GAME_TEAM_SPECTATOR));
    CHECK(CountLoggedSince(from, "identity", "viewer slot 0 role=2 (spectator)") >= 1);
    CHECK(CountLoggedSince(from, "identity", "ViewerSeatNotPlayback") == 0);
}

// L3 RUN-31 F1, which is the shape of EVERY playback launch: capture is ALWAYS ON (locked
// constraint 6 -- the plugin records during playback sessions too), so a demo is open here AND a
// capture is recording. The predicate that carried a recording term answered `not ours` on the
// state-9 tick of every one of those launches -- 0 x role=2, first person, hands and HUD.
TEST_CASE("SEAT-PREDICATE: a loaded demo seats WHILE a capture is recording (L3 RUN-31 F1)") {
    const CaptureRecordingHere recording;
    const PlaybackSessionEnv playbackSession;   // a demo open through the shipped transport
    REQUIRE(replay::capture::CaptureIsRecording() == true);
    CHECK(replay::playback::PlaybackOwnsSession() == true);

    const size_t from = TheEnv().log.lines.size();
    RosterFixture f;
    ViewerSeatEngine eng;
    eng.ng = f.netGame.bytes;
    replay::PawnProvision::ActorHops h{};
    h.ctx = &eng;
    h.assignTeam = ViewerSeatAssign;
    f.id.hopSurface = &h;
    const uint16_t locals = 1;
    memcpy(f.netGame.bytes + facts::LOCAL_NETGAME_LOCAL_USER_COUNT_OFF, &locals, sizeof locals);
    PutRole(f.netGame.bytes, 0, 1);

    f.Tick();

    // The seat ISSUED, with the capture still recording -- this is the L3 line the run measures.
    CHECK(eng.calls == 1);
    CHECK(eng.sawIndex == 0);
    CHECK(eng.sawTeam == uint16_t(facts::GAME_TEAM_SPECTATOR));
    CHECK(GetRole(f.netGame.bytes, 0) == uint16_t(facts::GAME_TEAM_SPECTATOR));
    CHECK(CountLoggedSince(from, "identity", "viewer slot 0 role=2 (spectator)") >= 1);
    CHECK(CountLoggedSince(from, "identity", "ViewerSeatNotPlayback") == 0);
    CHECK(replay::capture::CaptureIsRecording() == true);
}

// The other half of the same rule, and it is L2 #17 verbatim: a `capture-local` session with a
// live capture and NOTHING OPEN. There the recording user is the player, and seating him as a
// spectator emptied the physics pool for 216 s. `loaded` is the term that refuses him -- not the
// capture, which is on in the seating case above too.
TEST_CASE("SEAT-PREDICATE: a capture-only session with no demo refuses ViewerSeatNotPlayback") {
    const CaptureRecordingHere recording;
    SetEnvironmentVariableW(replay::bootstrap::OpenEnvVarName(), nullptr);
    REQUIRE(replay::capture::CaptureIsRecording() == true);
    CHECK(replay::playback::PlaybackOwnsSession() == false);

    const size_t from = TheEnv().log.lines.size();
    RosterFixture f;
    ViewerSeatEngine eng;
    eng.ng = f.netGame.bytes;
    replay::PawnProvision::ActorHops h{};
    h.ctx = &eng;
    h.assignTeam = ViewerSeatAssign;
    f.id.hopSurface = &h;
    const uint16_t locals = 1;
    memcpy(f.netGame.bytes + facts::LOCAL_NETGAME_LOCAL_USER_COUNT_OFF, &locals, sizeof locals);
    PutRole(f.netGame.bytes, 0, 1);

    for (int i = 0; i < 3; ++i) f.Tick();

    // Nothing was asked of the engine and the RECORDING user keeps the team the engine gave him.
    CHECK(eng.calls == 0);
    CHECK(GetRole(f.netGame.bytes, 0) == 1);
    CHECK(CountLoggedSince(from, "identity", "ViewerSeatNotPlayback") == 1);
    CHECK(CountLoggedSince(from, "identity", "viewer slot 0 role=") == 0);
    CHECK(CountLoggedSince(from, "identity", "preseat") == 0);
}

// The third answer: the carrier alone, with nothing open. The old predicate seated on this, which
// is what made a stale environment variable enough to take a user off his team.
TEST_CASE("SEAT-PREDICATE: a launch carrier with nothing open is not a playback session") {
    const CarrierOnlyEnv carrier;
    REQUIRE(replay::bootstrap::OpenRequestPresent() == true);
    REQUIRE(replay::capture::CaptureIsRecording() == false);
    CHECK(replay::playback::PlaybackOwnsSession() == false);

    const size_t from = TheEnv().log.lines.size();
    RosterFixture f;
    ViewerSeatEngine eng;
    eng.ng = f.netGame.bytes;
    replay::PawnProvision::ActorHops h{};
    h.ctx = &eng;
    h.assignTeam = ViewerSeatAssign;
    f.id.hopSurface = &h;
    const uint16_t locals = 1;
    memcpy(f.netGame.bytes + facts::LOCAL_NETGAME_LOCAL_USER_COUNT_OFF, &locals, sizeof locals);
    PutRole(f.netGame.bytes, 0, 1);

    for (int i = 0; i < 3; ++i) f.Tick();

    CHECK(eng.calls == 0);
    CHECK(GetRole(f.netGame.bytes, 0) == 1);
    // The word is latched once per session and the capture-only case above already spent it in
    // this binary (only a Load resets the latch), so what pins THIS case is that the carrier
    // alone asked nothing of the engine.
    CHECK(CountLoggedSince(0, "identity", "ViewerSeatNotPlayback") >= 1);
    CHECK(CountLoggedSince(from, "identity", "ViewerSeatNotPlayback") <= 1);
    CHECK(CountLoggedSince(from, "identity", "viewer slot 0 role=") == 0);
}


namespace {
// The harness reports a lock for the whole run (see Env). A case that needs the other answer puts
// it back afterwards even if a REQUIRE aborts it -- every later case depends on the lock.
struct UncontainedScope {
    UncontainedScope()  { replay::offlinelock::ResetForTest(); }
    ~UncontainedScope() { replay::offlinelock::ReportLockedForTest(); }
};
}  // namespace

TEST_CASE("an uncontained process dresses NOBODY, and says why") {
    // ★ LIVE SAFETY. Dressing admits entrants and overwrites another user's name, profile, team,
    // loadout and colour. In a live online match those belong to the host: ours to record, never
    // to author. The offline bit lock is the discriminator -- bootstrap applies it on every launch
    // that is not a live online match -- so an unlocked process must write nothing at all.
    RosterFixture f;
    const uint16_t slots[1] = {2};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 1));
    f.netGame.SetState(facts::NETGAME_STATE_INMATCH);

    const size_t from = TheEnv().log.lines.size();
    {
        UncontainedScope uncontained_;
        // RECORDER-2 part A moved the refusal one step earlier: the LOAD itself is refused by name
        // in an uncontained process, so nothing is open for the dress pass to write from, and the
        // ladder that used to be the last line of defence is now the second.
        CHECK_FALSE(f.e.services.transport->Load(tmp.path));
        f.Tick();
        f.Tick();

        const IdentityStatus st = IdentitySnapshot();
        CHECK(st.passes == 0);      // not one pass ran
        CHECK(st.dressed == 0);
        CHECK(st.mapped == 0);
        CHECK(CountLoggedSince(from, "playback", "not a contained offline session") == 1);
    }

    // Contained again: the same load, the same roster, and now it dresses. Without this arm the
    // case above would pass against a build that had simply stopped dressing altogether.
    REQUIRE(f.e.services.transport->Load(tmp.path));
    f.Tick();
    CHECK(IdentitySnapshot().passes >= 1);
    CHECK(IdentitySnapshot().mapped == 1);
    f.e.services.transport->Unload();
}

TEST_CASE("the LOD enable dword is written only by a contained process with a file open") {
    // ★ This write used to run above the load gate, so EVERY session --
    // including the owner's live public spectate -- had a 1 stamped into CR15RemotePlayerCS+520,
    // a dword this module had never read and never put back. Three claims are made here:
    //   (a) no file open  => no write at all;
    //   (b) file open but not contained => still no write (the gate INSIDE ApplyLodThrottle);
    //   (c) contained and open => the write DOES happen, and unload gives it back.
    // (c) is the control: without it (a) and (b) would pass against a build that simply stopped
    // writing the byte, which is not the fix that was asked for.
    RosterFixture f;
    const uint16_t slots[1] = {2};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 1));
    f.netGame.SetState(facts::NETGAME_STATE_INMATCH);
    f.ent.lodClaims.clear();
    f.ent.lodReleases = 0;

    // (a) No file. Ticks all day, writes nothing -- this is the live capture's whole tick.
    f.Tick();
    f.Tick();
    CHECK(f.ent.lodClaims.empty());

    // (c) Contained and loaded: the claim is made, and it writes 0 -- the round-robin OFF, which
    // is what a free-camera replay wants. Never a 1: putting a 1 back is the engine's value to
    // choose, not ours.
    REQUIRE(f.e.services.transport->Load(tmp.path));
    f.Tick();
    REQUIRE(f.ent.lodClaims.size() == 1);
    CHECK(f.ent.lodClaims[0] == 0);
    f.Tick();
    CHECK(f.ent.lodClaims.size() == 1);   // held, not re-written every frame

    // (b) THE MUTANT KILLER. Drop the surface so the module forgets its claim, then bring it back
    // in a process that is NOT contained. The load gate cannot carry this case -- the file is
    // still open -- so the only thing that can refuse the write is the predicate inside
    // ApplyLodThrottle. Delete that line and this CHECK goes red.
    f.ent.available = false;
    f.Tick();
    replay::offlinelock::ResetForTest();     // the process stops being contained
    f.ent.available = true;
    f.Tick();
    f.Tick();
    CHECK(f.ent.lodClaims.size() == 1);      // nothing new was written
    replay::offlinelock::ReportLockedForTest();

    // Contained again, same open file, same bound surface: it writes. The both-arms control.
    f.Tick();
    REQUIRE(f.ent.lodClaims.size() == 2);
    CHECK(f.ent.lodClaims[1] == 0);

    // And unload hands the engine's own value back exactly once.
    f.e.services.transport->Unload();
    CHECK(f.ent.lodReleases == 1);
    CHECK(f.ent.lodClaims.size() == 2);   // the release is not another claim
}

TEST_CASE("the dress pass is paced, and a load re-arms its clock") {
    // The retry runs a few times a second, not once a frame: the pass re-asks the engine to admit
    // every undressed entrant and then writes six fields into it.
    RosterFixture f;
    const uint16_t slots[1] = {2};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 1));
    f.netGame.SetState(facts::NETGAME_STATE_INMATCH);
    REQUIRE(f.e.services.transport->Load(tmp.path));

    // This roster dresses on its first pass, so the count pins the OTHER half of the rule: once
    // nothing is deferred the driver stops passing at all, however many ticks go by.
    const uint32_t after_load = IdentitySnapshot().passes;
    CHECK(after_load == 1);
    CHECK_FALSE(IdentitySnapshot().pending);
    for (int i = 0; i < 20; ++i) f.Tick();
    CHECK(IdentitySnapshot().passes == after_load);
    CHECK_FALSE(IdentitySnapshot().gave_up);

    // A second load re-arms: the counter starts again rather than continuing the previous file's.
    // The tick is the engine update that drains the unload's leave feed; without it the next load's
    // in-Load pass would be measuring the leave deferral instead of the clock.
    f.e.services.transport->Unload();
    f.Tick();
    REQUIRE(f.e.services.transport->Load(tmp.path));
    CHECK(IdentitySnapshot().passes == 1);
    f.e.services.transport->Unload();
}


namespace {
// The Env is a process singleton, so a case that shortens the dress timeouts puts them back even if
// a REQUIRE aborts it.
struct DressTimingScope {
    DressTimingScope(int64_t retry, int64_t bound) {
        TheEnv().config.dressRetryMs = retry;
        TheEnv().config.dressBoundMs = bound;
    }
    ~DressTimingScope() {
        TheEnv().config.dressRetryMs = -1;
        TheEnv().config.dressBoundMs = -1;
    }
};
}  // namespace

TEST_CASE("a DEFERRED admission keeps the roster pending across ticks, and the retry dresses it") {
    // ★★★ THE DEFECT, REPRODUCED AT THE DRIVER. Every player of the subject capture refused its
    // admission with ReplyWrongPhase at the instant the adds were queued; `pending` was cleared
    // anyway, and the per-tick driver that calls this pass every frame no-opped for the next nine
    // minutes. The queue drained milliseconds later, the engine admitted all five players, and
    // nothing ever wrote a name, a number, a team or a loadout: `engine held 0` of 20 recorded rows.
    DressTimingScope timing_(0, 30000);   // pace off; the BOUND stays shipped-length
    RosterFixture f;
    const uint16_t slots[1] = {2};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 1));
    f.netGame.SetState(facts::NETGAME_STATE_INMATCH);
    f.id.defersLeft = 2;

    REQUIRE(f.e.services.transport->Load(tmp.path));
    IdentityStatus st = IdentitySnapshot();
    CHECK(st.pending);                 // OWED, not decided
    CHECK(st.entrant_deferred == 1);
    CHECK(st.dressed == 0);
    CHECK(st.mapped == 0);
    CHECK_FALSE(st.gave_up);

    f.Tick();
    st = IdentitySnapshot();
    CHECK(st.pending);
    CHECK(st.entrant_deferred == 1);
    CHECK(st.passes == 2);

    // The pass the engine's queue has drained by. It lands, and the roster stops being pending.
    f.Tick();
    st = IdentitySnapshot();
    CHECK_FALSE(st.pending);
    CHECK(st.entrant_deferred == 0);
    CHECK(st.mapped == 1);
    CHECK_FALSE(st.gave_up);

    // ...and it stops asking. A retry that kept re-dressing a landed roster would re-apply the
    // loadout to the actor a few times a second for the rest of the file.
    const uint32_t settled = IdentitySnapshot().passes;
    for (int i = 0; i < 5; ++i) f.Tick();
    CHECK(IdentitySnapshot().passes == settled);
}

TEST_CASE("a roster that never stops deferring is given up on LOUDLY, and bounded") {
    // Silence is how the last one survived a nine-minute session. Past the bound each still-deferred
    // player is named, with the refusal that names why, and the retry stops.
    DressTimingScope timing_(0, 0);   // the bound, reached on the second deferring pass
    RosterFixture f;
    const uint16_t slots[1] = {2};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 1));
    f.netGame.SetState(facts::NETGAME_STATE_INMATCH);
    f.id.defersLeft = 10000;

    const size_t from = TheEnv().log.lines.size();
    REQUIRE(f.e.services.transport->Load(tmp.path));
    CHECK(IdentitySnapshot().pending);       // the FIRST deferring pass only starts the clock
    CHECK_FALSE(IdentitySnapshot().gave_up);

    f.Tick();
    const IdentityStatus st = IdentitySnapshot();
    CHECK_FALSE(st.pending);
    CHECK(st.gave_up);
    CHECK(CountLoggedSince(from, "playback", "identity dressing GIVEN UP for player") == 1);
    CHECK(CountLoggedSince(from, "playback", "identity dressing stopped retrying") == 1);
    // By NAME: the refusal is the engine's own word, not a sentence this module invented.
    CHECK(CountLoggedSince(from, "playback",
                           replay::PawnProvision::RefusalText(
                               replay::PawnProvision::Refusal::ReplyWrongPhase)) >= 1);

    // And it really has stopped: no further passes, and no second give-up report.
    const uint32_t passes = st.passes;
    for (int i = 0; i < 5; ++i) f.Tick();
    CHECK(IdentitySnapshot().passes == passes);
    CHECK(CountLoggedSince(from, "playback", "identity dressing GIVEN UP for player") == 1);
}


TEST_CASE("the retry is PACED: two ticks inside one period run one pass") {
    // A pass re-asks the engine to admit every undressed entrant and then writes six fields into
    // it. At 90 Hz that is ninety passes a second for a wait the engine measures in milliseconds.
    DressTimingScope timing_(60000, 300000);   // one minute: no second pass can be due in this case
    RosterFixture f;
    const uint16_t slots[1] = {2};
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 1));
    f.netGame.SetState(facts::NETGAME_STATE_INMATCH);
    f.id.defersLeft = 1;

    REQUIRE(f.e.services.transport->Load(tmp.path));
    CHECK(IdentitySnapshot().pending);
    CHECK(IdentitySnapshot().passes == 1);
    // Twenty ticks, all inside the period. The roster is still deferred -- so the pass is WANTED --
    // and it still does not run.
    for (int i = 0; i < 20; ++i) f.Tick();
    CHECK(IdentitySnapshot().passes == 1);
    CHECK(IdentitySnapshot().pending);
    CHECK(IdentitySnapshot().mapped == 0);
    // ...and the entrant is still waiting to be admitted, so this is pacing and not a landed dress.
    CHECK(f.id.defersLeft == 0);
}

// ---------------------------------------------------------------------------------------------
// ★★★ THE PRE-ROLL (DISC-EARLY-3).
//
// The offline host does not resolve its own frisbee ref until its pre_match script has counted 45 s
// down, and that countdown is metered by the engine clock this module masters -- so a load holds the
// playhead where the owner left it and runs that clock forward until the disc binds. Every case
// below is asserted on what the CLOCK RECEIVED and on what the transport did, never on a counter the
// module increments beside the call.
// ---------------------------------------------------------------------------------------------

namespace {

// Everything the pre-roll reads, restored on the way out so the cases after these three see the
// file's own defaults.
struct PreRollScope {
    Env& e;
    bool savedOn;
    int64_t savedBound;
    double savedRate;
    double savedScale;
    // `scale` defaults to `rate`, so a case that does not care about the ramp sees ONE rate across
    // both halves of the hold and asserts on it exactly as it did before the two scales existed.
    PreRollScope(bool on, int64_t boundMs, double rate, double scale = 0.0) : e(TheEnv()) {
        savedOn = e.config.preroll;
        savedBound = e.config.prerollBoundMs;
        savedRate = e.config.prerollRate;
        savedScale = e.config.prerollScale;
        e.config.preroll = on;
        e.config.prerollBoundMs = boundMs;
        e.config.prerollRate = rate;
        e.config.prerollScale = scale > 0.0 ? scale : rate;
    }
    ~PreRollScope() {
        e.config.preroll = savedOn;
        e.config.prerollBoundMs = savedBound;
        e.config.prerollRate = savedRate;
        e.config.prerollScale = savedScale;
    }
};

// The candidate diagnostic's subject: an entity whose early path refuses with ONE unchanging
// literal while what its candidates say moves underneath. That is the shape the launches had, and
// the shape the old de-dup could not see through.
class DiagEntity final : public RecordingEntity {
public:
    mutable replay::IEntity::DiscBind::EarlyDiag diag;
    const char* DiscEarlyRefusal() const override {
        return "no live game_frisbee actor in any candidate space or its parents";
    }
    const replay::IEntity::DiscBind::EarlyDiag* DiscEarlyDiagnostics() const override {
        return &diag;
    }
};

}   // namespace

TEST_CASE("PRE-ROLL: the playhead is held and the ENGINE clock is what runs, at the asked rate") {
    ClockFixture cf;
    PreRollScope cfg(true, 30000, 2.0);
    SeamEntityFixture f;
    Env& e = f.e;
    ArmNetGame netGame;
    replay::TickInfo info{};
    info.netGame = netGame.bytes;

    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 6, 1, 2000000, nullptr, 0, 0, true));
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));
    const int64_t t0 = t->PlayheadUs();
    cf.clk.prerolls.clear();
    cf.clk.steps.clear();
    for (int i = 0; i < 3; ++i) e.tick.FireAll(info);

    REQUIRE(!cf.clk.prerolls.empty());
    CHECK(cf.clk.prerolls.back().active);
    CHECK(cf.clk.prerolls.back().rate == 2.0f);
    // ★ AND THE STEP PUSH IS HELD. Advance answers 1 us while paused; pushed at a clock that is
    // deliberately not paused it is the one write that would make the speed-up fictional.
    CHECK(cf.clk.steps.empty());
    // The picture does not move, and the transport is exactly what it says it is.
    CHECK(t->PlayheadUs() == t0);
    CHECK(t->State() == replay::TransportState::Pause);
    const replay::PrepareStatus p = t->Preparing();
    CHECK(p.preparing);
    CHECK(!p.playRequested);
    CHECK(p.hostRemainingS <= 65.0);   // pre-match AND the launch behind it
    CHECK(p.boundS == doctest::Approx(30.0));
    t->Unload();
}

TEST_CASE("PRE-ROLL: a play pressed while it prepares is remembered and starts when it ends") {
    ClockFixture cf;
    PreRollScope cfg(true, 40, 2.0);   // 40 ms, so the give-up is reached inside the case
    SeamEntityFixture f;
    Env& e = f.e;
    ArmNetGame netGame;
    replay::TickInfo info{};
    info.netGame = netGame.bytes;

    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 6, 1, 2000000, nullptr, 0, 0, true));
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));
    const int64_t t0 = t->PlayheadUs();
    const size_t from = e.log.lines.size();
    cf.clk.prerolls.clear();

    e.tick.FireAll(info);
    REQUIRE(t->Preparing().preparing);
    t->Play(1.0f);
    // Remembered, NOT obeyed: the disc is not bound yet, and this is the second the owner would
    // otherwise be watching an undriven disc in.
    CHECK(t->State() == replay::TransportState::Pause);
    CHECK(t->Preparing().playRequested);
    e.tick.FireAll(info);
    CHECK(t->PlayheadUs() == t0);

    Sleep(60);
    e.tick.FireAll(info);
    CHECK(!t->Preparing().preparing);
    CHECK(t->State() == replay::TransportState::Play);
    REQUIRE(!cf.clk.prerolls.empty());
    CHECK(!cf.clk.prerolls.back().active);   // the clock is handed back on the way out
    CHECK(CountLoggedSince(from, "playback", "PRE-ROLL GAVE UP") == 1);
    CHECK(CountLoggedSince(from, "playback", "the play pressed while it prepared starts now") == 1);

    // ...and the step push is exactly what it was before: the seam goes back to mirroring.
    cf.clk.steps.clear();
    Sleep(40);
    e.tick.FireAll(info);
    REQUIRE(cf.clk.steps.size() == 1);
    CHECK(cf.clk.steps[0] == 11111u);
    t->Unload();
}

TEST_CASE("PRE-ROLL: a pause during the wait cancels the remembered play; unload hands the clock back") {
    ClockFixture cf;
    PreRollScope cfg(true, 40, 2.0);
    SeamEntityFixture f;
    Env& e = f.e;
    ArmNetGame netGame;
    replay::TickInfo info{};
    info.netGame = netGame.bytes;

    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 6, 1, 2000000, nullptr, 0, 0, true));
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));
    e.tick.FireAll(info);
    REQUIRE(t->Preparing().preparing);
    t->Play(1.0f);
    t->Pause();
    CHECK(!t->Preparing().playRequested);
    Sleep(60);
    e.tick.FireAll(info);
    CHECK(!t->Preparing().preparing);
    CHECK(t->State() == replay::TransportState::Pause);

    // And a load unloaded mid-wait releases the clock rather than leaving the host running.
    REQUIRE(t->Load(tmp.path));
    e.tick.FireAll(info);
    REQUIRE(t->Preparing().preparing);
    cf.clk.prerolls.clear();
    t->Unload();
    REQUIRE(!cf.clk.prerolls.empty());
    CHECK(!cf.clk.prerolls.back().active);
    CHECK(t->State() == replay::TransportState::NotLoaded);
}

TEST_CASE("PRE-ROLL: off by config is off -- the clock is never asked and the step push is unbroken") {
    ClockFixture cf;
    PreRollScope cfg(false, 40000, 2.0);
    SeamEntityFixture f;
    Env& e = f.e;
    ArmNetGame netGame;
    replay::TickInfo info{};
    info.netGame = netGame.bytes;

    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 6, 1, 2000000, nullptr, 0, 0, true));
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));
    cf.clk.prerolls.clear();
    cf.clk.steps.clear();
    for (int i = 0; i < 3; ++i) e.tick.FireAll(info);
    for (size_t i = 0; i < cf.clk.prerolls.size(); ++i) {
        INFO("call ", i, " active ", cf.clk.prerolls[i].active);
        CHECK(!cf.clk.prerolls[i].active);
    }
    CHECK(cf.clk.steps.size() == 3);
    CHECK(!t->Preparing().preparing);
    // A seek during a load that is preparing keeps preparing -- the pre-roll is about the host,
    // not about where the playhead sits.
    t->Unload();
}

TEST_CASE("PRE-ROLL: a seek during the wait moves the playhead and the wait continues") {
    ClockFixture cf;
    PreRollScope cfg(true, 30000, 2.0);
    SeamEntityFixture f;
    Env& e = f.e;
    ArmNetGame netGame;
    replay::TickInfo info{};
    info.netGame = netGame.bytes;

    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 6, 1, 2000000, nullptr, 0, 0, true));
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));
    e.tick.FireAll(info);
    REQUIRE(t->Preparing().preparing);
    t->Seek(2000000);
    e.tick.FireAll(info);
    const int64_t moved = t->PlayheadUs();
    CHECK(moved > 0);
    CHECK(t->Preparing().preparing);
    e.tick.FireAll(info);
    CHECK(t->PlayheadUs() == moved);   // held THERE, not dragged back to T0
    t->Unload();
}

// The review named three pre-roll exits with no test at all. Two of them are below; the
// third, the disc-bind exit, is reported in ENG-SHIP-DEFAULTS as still uncovered -- reaching it
// needs a fake entity that satisfies the whole BindBodyHandles disc path, not a lever on this
// harness. All three run the same EndPreRoll -> SetPreRoll(false), which is what these assert.

TEST_CASE("PRE-ROLL: the match ending hands the clock back and stops the wait") {
    ClockFixture cf;
    PreRollScope cfg(true, 30000, 2.0);   // a bound far longer than the case, so only the end fires
    SeamEntityFixture f;
    Env& e = f.e;
    ArmNetGame netGame;
    replay::TickInfo info{};
    info.netGame = netGame.bytes;

    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 6, 1, 2000000, nullptr, 0, 0, true));
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));
    const size_t from = e.log.lines.size();
    e.tick.FireAll(info);
    REQUIRE(t->Preparing().preparing);
    cf.clk.prerolls.clear();

    // The host's match is gone: no netGame is the session blocker the module watches for.
    replay::TickInfo over{};
    over.netGame = nullptr;
    e.tick.FireAll(over);

    CHECK(!t->Preparing().preparing);
    REQUIRE(!cf.clk.prerolls.empty());
    CHECK(!cf.clk.prerolls.back().active);
    CHECK(CountLoggedSince(from, "playback", "the match ended") == 1);
    CHECK(CountLoggedSince(from, "playback", "PRE-ROLL GAVE UP") == 0);
    t->Unload();
}

TEST_CASE("PRE-ROLL: a second Load during the wait hands the clock back before it re-arms") {
    ClockFixture cf;
    PreRollScope cfg(true, 30000, 2.0);
    SeamEntityFixture f;
    Env& e = f.e;
    ArmNetGame netGame;
    replay::TickInfo info{};
    info.netGame = netGame.bytes;

    TempDemo first;
    TempDemo second;
    REQUIRE(WriteDemo(first.path, 5000000, 11111, 60, 6, 1, 2000000, nullptr, 0, 0, true));
    REQUIRE(WriteDemo(second.path, 5000000, 11111, 60, 6, 1, 2000000, nullptr, 0, 0, true));
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(first.path));
    e.tick.FireAll(info);
    REQUIRE(t->Preparing().preparing);
    cf.clk.prerolls.clear();

    REQUIRE(t->Load(second.path));
    // Load's own Unload ran first, so the clock was handed back before the new pre-roll took it.
    REQUIRE(cf.clk.prerolls.size() >= 1);
    CHECK(!cf.clk.prerolls.front().active);

    cf.clk.prerolls.clear();
    // Load's own Unload queued the first file's entrants to leave, and the pre-roll waits on the
    // dress. The engine feeds that queue from its update; this rig has to do it by hand.
    replay::playback::ServiceLeaveFeedForTest();
    e.tick.FireAll(info);
    CHECK(t->Preparing().preparing);
    REQUIRE(!cf.clk.prerolls.empty());
    CHECK(cf.clk.prerolls.back().active);   // and the second load has it, at its own rate
    CHECK(cf.clk.prerolls.back().rate == 2.0f);
    t->Unload();
}

// ArmDressPass latches identity.pending before anything asks whether the
// dresser is on, and the pre-roll's Waiting gate holds on that flag -- so playback.dress=false, the
// switch a crashing launch is repeated with, used to buy a full 40 s dead wait and a "GAVE UP" line
// that blamed the disc. With dressing off nothing may be pending.
TEST_CASE("PRE-ROLL: playback.dress off leaves nothing pending, so the wait is not stranded") {
    ClockFixture cf;
    PreRollScope cfg(true, 30000, 2.0);
    SeamEntityFixture f;
    Env& e = f.e;
    const bool savedDress = e.config.dress;
    e.config.dress = false;
    ArmNetGame netGame;
    replay::TickInfo info{};
    info.netGame = netGame.bytes;

    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 6, 1, 2000000, nullptr, 0, 0, true));
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));
    const size_t from = e.log.lines.size();
    cf.clk.prerolls.clear();
    for (int i = 0; i < 3; ++i) e.tick.FireAll(info);

    // The clock was actually asked for: a pre-roll that never leaves Waiting never calls SetPreRoll.
    REQUIRE(!cf.clk.prerolls.empty());
    CHECK(cf.clk.prerolls.back().active);
    CHECK(cf.clk.prerolls.back().rate == 2.0f);
    CHECK(t->Preparing().preparing);
    CHECK(CountLoggedSince(from, "playback", "PRE-ROLL GAVE UP") == 0);
    t->Unload();
    e.config.dress = savedDress;
}

TEST_CASE("EARLY-PATH DIAG: the candidate lines follow the CANDIDATES, not the refusal literal") {
    ClockFixture cf;
    PreRollScope cfg(false, 40000, 2.0);
    Env& e = TheEnv();
    e.services.transport->Unload();
    replay::IIdentity* savedId = e.services.identity;
    replay::IEntity* savedEnt = e.services.entity;
    ZeroIndexIdentity id;
    DiagEntity ent;
    ent.available = true;
    e.services.identity = &id;
    e.services.entity = &ent;

    using Diag = replay::IEntity::DiscBind::EarlyDiag;
    ent.diag.n = 2;
    ent.diag.cand[0].name = "ng+0x2B10";
    ent.diag.cand[0].root = (void*)0x1000;
    ent.diag.cand[0].depth = 2;
    ent.diag.cand[0].regAt[0] = Diag::Reg::Empty;
    ent.diag.cand[0].regAt[1] = Diag::Reg::Populated;
    ent.diag.cand[0].countAt[1] = 7;
    ent.diag.cand[1].name = "ng+0x2B08";
    ent.diag.cand[1].root = (void*)0x2000;  // facts-ok: PHYS.CI_PAUSED_BIT a test fixture value this case compares against, not an engine position
    ent.diag.cand[1].depth = 1;
    ent.diag.cand[1].regAt[0] = Diag::Reg::Populated;
    ent.diag.cand[1].countAt[0] = 99;

    ArmNetGame netGame;
    replay::TickInfo info{};
    info.netGame = netGame.bytes;
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 6, 1, 2000000, nullptr, 0, 0, true));
    const size_t from = e.log.lines.size();
    REQUIRE(e.services.transport->Load(tmp.path));
    e.tick.FireAll(info);
    // EMPTY is printed as itself. `actors=0(unreadable)` said this and a torn pointer with one word.
    CHECK(CountLoggedSince(from, "playback", "d0=empty d1=7") == 1);
    CHECK(CountLoggedSince(from, "playback", "d0=99") == 1);
    const size_t after_first = e.log.lines.size();

    // Same answer, more polls: silent.
    Sleep(1100);
    e.tick.FireAll(info);
    e.tick.FireAll(info);
    CHECK(CountLoggedSince(after_first, "playback", "early path candidate") == 0);

    // ★ THE WHOLE POINT: the refusal literal is unchanged and a count AT DEPTH 1 moved. The old
    // key was the refusal pointer, so this transition -- the space filling -- printed nothing.
    // The sleeps are the retry's own 500 ms cadence: a tick that does not poll sees nothing.
    ent.diag.cand[0].countAt[1] = 8;
    Sleep(600);
    e.tick.FireAll(info);
    CHECK(CountLoggedSince(after_first, "playback", "d0=empty d1=8") == 1);
    const size_t after_second = e.log.lines.size();

    // A second change inside one second of the last set: rate-limited, and the swallowed one is
    // COUNTED onto the set that does print rather than thrown away.
    ent.diag.cand[0].regAt[0] = Diag::Reg::Unreadable;
    Sleep(600);
    e.tick.FireAll(info);
    CHECK(CountLoggedSince(after_second, "playback", "early path candidate") == 0);
    Sleep(600);
    e.tick.FireAll(info);
    CHECK(CountLoggedSince(after_second, "playback", "d0=unreadable d1=8") == 1);
    CHECK(CountLoggedSince(after_second, "playback", "change(s) not printed") == 1);

    e.services.transport->Unload();
    e.services.identity = savedId;
    e.services.entity = savedEnt;
}

// =============================================================================================
// DESIGN-UI-2 P1 -- the drag. Everything here drives the SHIPPED transport through the SHIPPED
// tick, because the two guards the packet exists to move live at two different sites and only a
// real tick walks past both of them.
// =============================================================================================

namespace {

// The `[scrub] step` line is the packet's own oracle, and this reads it back out of the log. A
// field that is absent reads as -1, so a renamed field fails loudly instead of matching zero.
long ScrubField(const std::string& line, const char* key) {
    const std::string k = std::string(key) + "=";
    const size_t at = line.find(k);
    if (at == std::string::npos) return -1;
    return strtol(line.c_str() + at + k.size(), nullptr, 10);
}

const std::string* LastLineWith(size_t from, const char* needle) {
    const std::vector<Line>& lines = TheEnv().log.lines;
    const std::string* found = nullptr;
    for (size_t i = from; i < lines.size(); ++i) {
        if (lines[i].text.find(needle) != std::string::npos) found = &lines[i].text;
    }
    return found;
}

size_t CountLinesWith(size_t from, const char* needle) {
    const std::vector<Line>& lines = TheEnv().log.lines;
    size_t n = 0;
    for (size_t i = from; i < lines.size(); ++i) {
        if (lines[i].text.find(needle) != std::string::npos) ++n;
    }
    return n;
}

void FireOneTick() {
    replay::TickInfo info{};
    info.frameDeltaUs = 11111;
    info.simTimeUs = 1000000;
    info.frameIndex = 1;
    TheEnv().tick.FireAll(info);
}

}  // namespace

TEST_CASE("a drag coalesces to the newest target and applies exactly one per tick") {
    Env& e = TheEnv();
    TempDemo tmp;
    const uint64_t t0 = 5000000, dt = 11111;
    REQUIRE(WriteDemo(tmp.path, t0, dt, 180, 12, 1));
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));
    const int64_t duration = (int64_t)t->DurationUs();
    REQUIRE(duration > 0);

    const size_t from = e.log.lines.size();
    t->BeginScrub();
    CHECK(t->State() == replay::TransportState::Scrub);

    // Three targets, no tick between them: two are coalesced away and only the newest is ever
    // applied. A queue would land on the first one and replay the other two.
    t->ScrubTo(duration / 8);
    t->ScrubTo(duration / 4);
    t->ScrubTo(duration / 2);
    CHECK(t->Scrub().skipped == 2u);
    CHECK(t->Scrub().pendingUs == duration / 2);
    CHECK(t->Scrub().appliedUs != duration / 2);

    FireOneTick();
    CHECK(t->Scrub().appliedUs == duration / 2);
    CHECK(CountLinesWith(from, "[scrub] step") == 1u);

    // A tick with nothing new pending issues no second step.
    FireOneTick();
    CHECK(CountLinesWith(from, "[scrub] step") == 1u);

    t->EndScrub(-1);
    CHECK(CountLinesWith(from, "[scrub] settle") == 1u);
    CHECK(t->State() != replay::TransportState::Scrub);
    t->Unload();
}

// =============================================================================================
// SCRUB-BODIES-2 / LAUNCH-QUALIFY-2100 G3. The launch measured the minimap player dots frozen
// byte-identical across 16 consecutive drag steps while the disc marker moved.
//
// ★ THE FAKE IS THE CONSUMER THE LAUNCH SAW. SwitchableEntity::installs counts one per BOUND body
// per tick -- it is the plugin's only world-position write (apply.cpp installs a one-node phys
// history ring, and both the 3-D body and the minimap dot are drawn from it). An unbound track is
// skipped, so with the bind deferred to EndScrub the counter stops dead on the first drag step
// that crosses a chapter and the bodies stand still exactly as the owner reported. The assertion
// is per STEP, because the old code still moved them at the settle.
// =============================================================================================
TEST_CASE("G3: a drag that crosses chapters keeps the bodies bound and moving on every step") {
    Env& e = TheEnv();
    replay::IEntity* const shipped = e.services.entity;
    SwitchableEntity fake;
    fake.available = true;
    e.services.entity = &fake;
    e.tick.FireAll(replay::TickInfo{});

    TempDemo tmp;
    const uint64_t t0 = 5000000, dt = 11111;
    const uint32_t per_chapter = 180, chapters = 12, bodies = 2;
    REQUIRE(WriteDemo(tmp.path, t0, dt, per_chapter, chapters, bodies));
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));
    FireOneTick();
    const int64_t duration = (int64_t)t->DurationUs();
    REQUIRE(duration > 0);

    const int kSteps = 12;
    const size_t from = e.log.lines.size();
    t->BeginScrub();
    REQUIRE(t->State() == replay::TransportState::Scrub);

    size_t rebuilt_steps = 0, inwindow_steps = 0;
    for (int i = 1; i <= kSteps; ++i) {
        const size_t mark = e.log.lines.size();
        const size_t installs_before = fake.installs;
        t->ScrubTo(duration * i / (kSteps + 1));
        FireOneTick();

        CHECK(fake.installs > installs_before);
        const std::string* line = LastLineWith(mark, "[scrub] step");
        REQUIRE(line != nullptr);
        CHECK(ScrubField(*line, "bound") == (long)bodies);
        CHECK(ScrubField(*line, "refused") == 0);
        if (ScrubField(*line, "seek_rebuilt") == 1 || ScrubField(*line, "tick_rebuilt") == 1) {
            ++rebuilt_steps;
        } else {
            ++inwindow_steps;
            CHECK(ScrubField(*line, "covered") == 1);
        }
    }
    // Without a crossing the case proves nothing, and with a rebuild on every step the budget half
    // of the evidence would be untested.
    CHECK(rebuilt_steps > 0);
    CHECK(inwindow_steps > 0);

    // A light build binds without saying so, and the settle is where it says so.
    const int64_t landing = duration * kSteps / (kSteps + 1);
    t->EndScrub(-1);
    const std::string* settle = LastLineWith(from, "[scrub] settle");
    REQUIRE(settle != nullptr);
    CHECK(ScrubField(*settle, "binds") == (long)rebuilt_steps);
    CHECK(ScrubField(*settle, "refused") == 0);
    const int64_t after_drag = t->PlayheadUs();

    // The landing a drag reaches is the landing a paused seek reaches.
    t->Seek(0);
    FireOneTick();
    t->Seek(landing);
    FireOneTick();
    CHECK(t->PlayheadUs() == after_drag);

    t->Unload();
    e.services.entity = shipped;
    e.tick.FireAll(replay::TickInfo{});
}

// The bind is not a function of the pre-drag transport state, and the two states that reach
// BeginScrub by a different route are the ones worth naming: a drag started while PLAYING, and a
// drag started in reverse -- which keeps its negative rate for the whole scrub.
TEST_CASE("G3: a drag from play and a drag from reverse bind their bodies the same way") {
    Env& e = TheEnv();
    replay::IEntity* const shipped = e.services.entity;
    SwitchableEntity fake;
    fake.available = true;
    e.services.entity = &fake;
    e.tick.FireAll(replay::TickInfo{});

    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 180, 12, 2));
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));
    const int64_t duration = (int64_t)t->DurationUs();

    const float rates[2] = {1.0f, -1.0f};
    for (int r = 0; r < 2; ++r) {
        t->Seek(duration / 2);
        t->Play(rates[r]);
        FireOneTick();
        REQUIRE(t->Rate() == rates[r]);

        t->BeginScrub();
        CHECK(t->State() == replay::TransportState::Scrub);
        for (int i = 1; i <= 6; ++i) {
            const size_t mark = e.log.lines.size();
            const size_t installs_before = fake.installs;
            t->ScrubTo(duration * i / 7);
            FireOneTick();
            CHECK(fake.installs > installs_before);
            const std::string* line = LastLineWith(mark, "[scrub] step");
            REQUIRE(line != nullptr);
            CHECK(ScrubField(*line, "bound") == 2);
        }
        t->EndScrub(-1);
        // S5.3 step 10: a scrub never changes rate or direction.
        CHECK(t->Rate() == rates[r]);
    }

    t->Pause();
    t->Unload();
    e.services.entity = shipped;
    e.tick.FireAll(replay::TickInfo{});
}

TEST_CASE("AC-1.6: a step inside the resident span rebuilds at NEITHER guard") {
    Env& e = TheEnv();
    TempDemo tmp;
    const uint64_t t0 = 5000000, dt = 11111;
    const uint32_t per_chapter = 180;
    REQUIRE(WriteDemo(tmp.path, t0, dt, per_chapter, 12, 1));
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));

    const int64_t span = (int64_t)(dt * per_chapter);      // one chapter
    t->Seek(span * 4 + span / 2);                          // centre on chapter 4
    FireOneTick();

    // Chapter 5: inside the resident OUTER span, outside the CENTRE chapter. This is the exact
    // target that used to decode three chapters for data already in memory -- and the one that
    // still would if only G1 had been moved, because the tick's own guard reaches it next frame.
    const int64_t target = span * 5 + span / 2;
    const size_t from = e.log.lines.size();
    t->BeginScrub();
    t->ScrubTo(target);
    FireOneTick();

    const std::string* step = LastLineWith(from, "[scrub] step");
    REQUIRE(step != nullptr);
    INFO("step line: ", *step);
    CHECK(ScrubField(*step, "covered") == 1);
    CHECK(ScrubField(*step, "seek_rebuilt") == 0);   // G1
    CHECK(ScrubField(*step, "tick_rebuilt") == 0);   // G2 -- both, or the fix is worth nothing
    CHECK(ScrubField(*step, "refused") == 0);

    t->EndScrub(-1);
    t->Unload();
}

TEST_CASE("the audio hold is taken once per drag and released on every exit") {
    Env& e = TheEnv();
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 180, 12, 1));
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));
    const int64_t duration = (int64_t)t->DurationUs();
    const bool before = replay::audio::AudioSeekMuteHeld();

    // Three BeginScrubs take ONE hold. The refcount cannot report an unbalanced take, so this
    // idempotence is the only thing between a second mouse-down and a session with no audio.
    t->BeginScrub();
    t->BeginScrub();
    t->BeginScrub();
    CHECK(replay::audio::AudioSeekMuteHeld());
    t->EndScrub(duration / 3);
    CHECK(replay::audio::AudioSeekMuteHeld() == before);

    t->BeginScrub();                       // exit: a seek from elsewhere
    t->Seek(duration / 4);
    CHECK(replay::audio::AudioSeekMuteHeld() == before);

    t->BeginScrub();                       // exit: a play from elsewhere
    t->Play(1.0f);
    CHECK(replay::audio::AudioSeekMuteHeld() == before);

    t->Pause();
    t->EndScrub(-1);                       // exit: EndScrub with no drag active, twice
    t->EndScrub(-1);
    CHECK(replay::audio::AudioSeekMuteHeld() == before);

    t->BeginScrub();                       // exit: unload mid-drag
    CHECK(replay::audio::AudioSeekMuteHeld());
    t->Unload();
    CHECK(replay::audio::AudioSeekMuteHeld() == before);
}

TEST_CASE("a drag never changes the rate or the sign, and restores the run state it found") {
    Env& e = TheEnv();
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 180, 12, 1));
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));
    const int64_t duration = (int64_t)t->DurationUs();

    // Paused before the drag stays paused after it, at the rate and sign it had.
    t->SetRate(-2.0f);
    t->Pause();
    REQUIRE(t->State() == replay::TransportState::Pause);
    t->BeginScrub();
    t->ScrubTo(duration / 2);
    FireOneTick();
    t->EndScrub(-1);
    CHECK(t->State() == replay::TransportState::Pause);
    CHECK(t->Rate() == doctest::Approx(-2.0f));

    // Playing before the drag resumes after it, at the same signed rate: a reverse drag must not
    // come back forwards.
    t->Play(-2.0f);
    REQUIRE(t->State() == replay::TransportState::Play);
    t->BeginScrub();
    CHECK(t->State() == replay::TransportState::Scrub);
    // A second press mid-drag must not re-read the run state it already paused: without the
    // idempotence guard the drag would come back PAUSED from a transport that was playing.
    t->BeginScrub();
    t->ScrubTo(duration / 3);
    FireOneTick();
    t->EndScrub(-1);
    CHECK(t->State() == replay::TransportState::Play);
    CHECK(t->Rate() == doctest::Approx(-2.0f));
    t->Pause();
    t->Unload();
}

TEST_CASE("scrubbing with no file, and past either end, is safe") {
    Env& e = TheEnv();
    replay::IReplayTransport* t = e.services.transport;
    t->Unload();
    t->BeginScrub();
    CHECK(t->State() == replay::TransportState::NotLoaded);
    t->ScrubTo(1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    t->EndScrub(-1);
    CHECK(t->Scrub().active == false);

    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 180, 12, 1));
    REQUIRE(t->Load(tmp.path));
    const int64_t duration = (int64_t)t->DurationUs();
    t->BeginScrub();
    t->ScrubTo(-duration);            // clamped to the start, never negative
    CHECK(t->Scrub().pendingUs == 0);
    t->ScrubTo(duration * 4);         // clamped to the end, never past the file
    CHECK(t->Scrub().pendingUs == duration);
    FireOneTick();
    CHECK(t->PlayheadUs() <= duration);
    t->EndScrub(-1);
    CHECK(t->PlayheadUs() <= duration);
    t->Unload();
}

TEST_CASE("RecentreMargin keeps lookahead in BOTH directions, and never fires outside the span") {
    replay::playback::Window w;
    const uint64_t chapter = 2000000;             // 2 s, the short end of a real chapter
    const uint64_t outer_begin = 10000000;  // facts-ok: CLOCK.CTOR_MAXELAPSED_INIT 10,000,000 in tick arithmetic, not the clock constructor's initialiser
    const uint64_t outer_end = outer_begin + 3 * chapter;
    const uint64_t inner_begin = outer_begin + chapter;
    const uint64_t inner_end = inner_begin + chapter;
    w.SetCoverage(outer_begin, outer_end, inner_begin, inner_end);

    const int64_t mid = (int64_t)(inner_begin + chapter / 2);
    CHECK_FALSE(w.RecentreMargin(mid, 1));
    CHECK_FALSE(w.RecentreMargin(mid, -1));
    CHECK_FALSE(w.RecentreMargin(mid, 0));

    // A whole chapter of lookahead ahead of the travel, so the re-centre happens a chapter before
    // the crossing rather than at it.
    const int64_t near_end = (int64_t)(outer_end - chapter / 2);
    CHECK(w.RecentreMargin(near_end, 1));
    CHECK_FALSE(w.RecentreMargin(near_end, -1));   // going the other way there is room behind
    CHECK(w.RecentreMargin(near_end, 0));          // paused or scrubbing: both edges

    const int64_t near_begin = (int64_t)(outer_begin + chapter / 2);
    CHECK(w.RecentreMargin(near_begin, -1));       // reverse gets the same margin as forward
    CHECK_FALSE(w.RecentreMargin(near_begin, 1));
    CHECK(w.RecentreMargin(near_begin, 0));

    // Outside the outer span is the correctness guard's case, not this one: answering yes here
    // would rebuild twice for one crossing.
    CHECK_FALSE(w.RecentreMargin((int64_t)outer_end + 1, 1));
    CHECK_FALSE(w.RecentreMargin((int64_t)outer_begin - 1, -1));
    CHECK_FALSE(w.RecentreMargin(-1, 0));

    replay::playback::Window fresh;
    CHECK_FALSE(fresh.RecentreMargin(mid, 0));     // an unset coverage asks for nothing
}

// ---- UI-4: the production transport answers where the content is ------------------------------

TEST_CASE("the production transport publishes the recorded span, not the file axis") {
    Env& e = TheEnv();
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t != nullptr);

    int64_t first = -1, last = -1;
    CHECK_FALSE(t->PlayableSpanUs(&first, &last));   // nothing loaded answers nothing
    CHECK(first == -1);                              // and leaves the out-params alone

    TempDemo tmp;
    const uint64_t dt = 33333;  // facts-ok: CLOCK.FIXEDSTEP_30HZ_US the 30 Hz step period restated as this case's own cadence fixture
    REQUIRE(WriteDemo(tmp.path, 5000000, dt, 60, 3, 1));
    REQUIRE(t->Load(tmp.path));

    // ★ THE DEFAULTED VIRTUAL RETURNS FALSE. If ReplayTransportImpl ever stops overriding it --
    // which is what LAUNCH-UI3 photographed, a timeline 94% dead air -- this REQUIRE is the first
    // thing that fails, in a unit test, instead of in the owner's hands.
    REQUIRE(t->PlayableSpanUs(&first, &last));
    CHECK(first >= 0);
    CHECK(last > first);
    // The span is measured off the TICKS, so it ends at the last recorded tick and not at the last
    // chapter's end: a chapter table answer would read exactly DurationUs here.
    CHECK(last <= (int64_t)t->DurationUs());
    CHECK(last == (int64_t)((60 * 3 - 1) * dt));
    // And it is the axis PlayheadUs speaks: a seek to the span's end lands inside it.
    t->Seek(last);
    CHECK(t->PlayheadUs() >= first);
    CHECK(t->PlayheadUs() <= last);

    t->Unload();
    first = -1;
    CHECK_FALSE(t->PlayableSpanUs(&first, &last));
    CHECK(first == -1);
}

// ENG-LEAVE's contract, and the reason ENG-RELOAD-PHASE's re-bind is now the FALLBACK: every
// entrant this session admitted leaves the engine on Unload, so the next load of the same file
// admits its own three into empty slots. What must never happen is what put six rows on the rail
// for three players -- a second body for a player already seated -- and the counts below say so
// exactly: three admissions and three leaves per cycle, one per slot, and nobody else touched.
TEST_CASE("RELOAD: the same capture loaded again leaves its entrants and admits three fresh") {
    RosterFixture f;
    const uint16_t slots[3] = {1, 2, 3};   // IndexForSlot is 3s+1: these are the slots the rig seats
    const uint16_t viewerSlot = 7;         // somebody else's seat: the owner's own spectator entrant
    f.id.Seat(viewerSlot);
    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 4, 2, 1, 2000000, slots, 3));
    replay::IReplayTransport* t = f.e.services.transport;

    REQUIRE(t->Load(tmp.path));
    f.Tick();
    REQUIRE(IdentitySnapshot().mapped == 3);
    CHECK(f.id.provisionCalls == 3);

    for (int cycle = 1; cycle <= 3; ++cycle) {
        INFO("cycle ", cycle);
        t->Unload();
        f.id.releaseOrderCount = 0;
        // RELOAD-5: one removal per engine update, from the top down. Three entrants, three passes.
        const uint16_t passes = DrainLeaveFeed(f);
        CHECK(passes == 3);
        CHECK(f.id.releaseOrderCount == 3);
        CHECK(f.id.releaseOrder[0] == 3);
        CHECK(f.id.releaseOrder[1] == 2);
        CHECK(f.id.releaseOrder[2] == 1);
        // The leave is fed and finished BEFORE the next load: the slots are empty again.
        CHECK_FALSE(replay::identity::RosterLeavePending());
        for (uint16_t s : slots) CHECK_FALSE(f.id.occupied[s]);
        REQUIRE(t->Load(tmp.path));
        f.Tick();
        f.Tick();

        const IdentityStatus st = IdentitySnapshot();
        CHECK_FALSE(st.pending);
        CHECK(st.players == 3);          // the roster is exactly the file's, not the file's plus A's
        CHECK(st.mapped == 3);
        CHECK(st.dressed + st.failed == 3);         // every row still gets exactly one verdict
        CHECK(f.id.provisionCalls == uint32_t(3 * (cycle + 1)));   // each load admits its own three
        CHECK(f.id.releases == uint16_t(3 * cycle));               // and each unload releases three
        for (uint16_t s : slots) {
            CHECK(f.id.askedSlot[s] == uint32_t(cycle + 1));
            CHECK(f.id.releasedSlot[s] == uint32_t(cycle));   // one leave per slot per cycle
            CHECK(f.id.occupied[s]);
        }
        // The viewer is not ours to remove and no cycle ever fed his slot.
        CHECK(f.id.releasedSlot[viewerSlot] == 0);
        CHECK(f.id.occupied[viewerSlot]);
    }
}

// §2.4, settled. A slot that recurs across two different captures is not special: A's entrant left
// on the unload, so B asks for all three of its own -- the shared numbers included -- and the one
// slot only B has is asked for exactly once.
TEST_CASE("RELOAD: a second capture admits all three of its own, shared slot numbers included") {
    RosterFixture f;
    const uint16_t a_slots[3] = {1, 2, 3};
    const uint16_t b_slots[3] = {1, 2, 4};   // two shared with A, one B has of its own
    TempDemo a, b;
    REQUIRE(WriteDemo(a.path, 5000000, 11111, 4, 2, 1, 2000000, a_slots, 3));
    REQUIRE(WriteDemo(b.path, 5000000, 11111, 4, 2, 1, 2000000, b_slots, 3));
    replay::IReplayTransport* t = f.e.services.transport;

    REQUIRE(t->Load(a.path));
    f.Tick();
    REQUIRE(IdentitySnapshot().mapped == 3);
    const uint32_t after_a = f.id.provisionCalls;
    REQUIRE(after_a == 3);

    t->Unload();
    // A's three left -- including slot 3, which B does not use and which therefore stays empty --
    // one per engine update, from the top down (RELOAD-5).
    CHECK(DrainLeaveFeed(f) == 3);
    CHECK(f.id.releases == 3);
    CHECK(f.id.releaseOrderCount == 3);
    CHECK(f.id.releaseOrder[0] == 3);
    CHECK(f.id.releaseOrder[1] == 2);
    CHECK(f.id.releaseOrder[2] == 1);
    for (uint16_t s : a_slots) { INFO("a slot ", s); CHECK(f.id.releasedSlot[s] == 1); }
    REQUIRE(t->Load(b.path));
    f.Tick();
    f.Tick();

    const IdentityStatus st = IdentitySnapshot();
    CHECK(st.players == 3);
    CHECK(st.mapped == 3);
    CHECK(f.id.askedSlot[1] == 2);               // asked once by A, once by B
    CHECK(f.id.askedSlot[2] == 2);
    CHECK(f.id.askedSlot[4] == 1);               // B's own, asked for the first time
    CHECK(f.id.provisionCalls == after_a + 3);
    CHECK(st.dressed + st.failed == 3);
    CHECK_FALSE(f.id.occupied[3]);               // A's third seat is empty, not inherited
}

TEST_CASE("RELOAD: a second capture with no slot in common admits its own and vacates A's") {
    RosterFixture f;
    const uint16_t a_slots[2] = {1, 2};
    const uint16_t b_slots[2] = {3, 4};
    TempDemo a, b;
    REQUIRE(WriteDemo(a.path, 5000000, 11111, 4, 2, 1, 2000000, a_slots, 2));
    REQUIRE(WriteDemo(b.path, 5000000, 11111, 4, 2, 1, 2000000, b_slots, 2));
    replay::IReplayTransport* t = f.e.services.transport;

    REQUIRE(t->Load(a.path));
    f.Tick();
    REQUIRE(IdentitySnapshot().mapped == 2);
    const uint32_t after_a = f.id.provisionCalls;

    t->Unload();
    CHECK(DrainLeaveFeed(f) == 2);               // A's two, one per engine update, slot 2 then 1
    CHECK(f.id.releaseOrder[0] == 2);
    CHECK(f.id.releaseOrder[1] == 1);
    REQUIRE(t->Load(b.path));
    f.Tick();
    f.Tick();

    const IdentityStatus st = IdentitySnapshot();
    CHECK(st.players == 2);
    CHECK(st.mapped == 2);
    // A's two slots are named by nothing in B's manifest, so B's roster is 2 and not 4: the
    // entrants A left behind are not counted into it, dressed by it, or mapped for it.
    CHECK(st.dressed + st.failed == 2);
    CHECK(f.id.provisionCalls == after_a + 2);   // B admitted its own two
    for (uint16_t s : a_slots) {
        INFO("a slot ", s);
        CHECK(f.id.releasedSlot[s] == 1);        // and A's two left rather than lingering
        CHECK_FALSE(f.id.occupied[s]);
    }
}

// ---- PREROLL-2: the end condition is a bind AND the host's own launch ---------------------------
// The offline host binds the recorded disc at ITS round_start and then spends ~20 s of host time in
// the launch sequence, parking the disc at the arena origin every frame. Ending the hold on the bind
// alone handed the owner that fight. The phase is read where HOST-NV sees the host's own write:
// the netvar store answers with the value the REPLAY drives, so it cannot be asked.

namespace {

// Leaves nothing behind: the record is process-wide, and a case that forgot to clear it would
// silently change the host phase every case after it.
struct HostPhaseScope {
    explicit HostPhaseScope(uint64_t symbol) {
        if (symbol != 0) Set(symbol);
    }
    ~HostPhaseScope() { replay::playback::netvarguard::NoteHostSymbolForTest(0, 0); }
    static void Set(uint64_t symbol) {
        replay::playback::netvarguard::NoteHostSymbolForTest(
            replay::gamestate::facts::kVarGameState.symbol, symbol);
    }
};

constexpr uint64_t kSymMainLoop = ::facts::GS_SYM_MAIN_LOOP_ENTER;
constexpr uint64_t kSymPreLaunch = 0x37756D8877ED5768ull;   // pre_launch_enter -- players in tubes
double Abs(double v) { return v < 0.0 ? -v : v; }

constexpr uint64_t kSymPreMatch = 0x6C19A96227539FACull;    // pre_match_enter -- the 45 s countdown

// The two numbers off one `preroll: timeline` line. Read out of the LINE rather than off a counter
// beside it, because the line is the whole deliverable: a timeline nobody can parse measures
// nothing. Returns false when the phase is not on any line since `from`.
bool TimelineRow(size_t from, const char* phase, double& wallS, double& engineS) {
    Env& e = TheEnv();
    for (size_t i = e.log.lines.size(); i > from; --i) {
        const std::string& text = e.log.lines[i - 1].text;
        if (text.find("preroll: timeline") == std::string::npos) continue;
        const size_t at = text.find(phase);
        if (at == std::string::npos) continue;
        return sscanf(text.c_str() + at + strlen(phase), " %lf s wall, %lf s engine", &wallS,
                      &engineS) == 2;
    }
    return false;
}

}   // namespace

TEST_CASE("PRE-ROLL: the host playing with no disc bound is still a wait, and the bar spans the whole of it") {
    ClockFixture cf;
    PreRollScope cfg(true, 30000, 2.0);
    HostPhaseScope host(0);   // the module publishes the watched key at Load; set the phase after
    SeamEntityFixture f;
    Env& e = f.e;
    ArmNetGame netGame;
    replay::TickInfo info{};
    info.netGame = netGame.bytes;

    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 6, 1, 2000000, nullptr, 0, 0, true));
    replay::IReplayTransport* t = e.services.transport;
    REQUIRE(t->Load(tmp.path));
    HostPhaseScope::Set(kSymMainLoop);
    e.tick.FireAll(info);
    // Playing but unbound: half the condition is not the condition.
    REQUIRE(t->Preparing().preparing);
    const double first = t->Preparing().progress;

    // The bar is measured over 65 s of host time, not 45: at 2x that is 32.5 s of real time, and
    // three ticks of it is nowhere near done.
    CHECK(first < 0.5);
    CHECK(t->Preparing().hostRemainingS <= 65.0);
    CHECK(t->Preparing().hostRemainingS > 45.0);   // the launch is still in front of it

    Sleep(30);
    e.tick.FireAll(info);
    const replay::PrepareStatus p = t->Preparing();
    REQUIRE(p.preparing);
    CHECK(p.progress >= first);   // monotone, and it does not restart when the host changes phase
    HostPhaseScope::Set(kSymPreLaunch);
    Sleep(30);
    e.tick.FireAll(info);
    CHECK(t->Preparing().progress >= p.progress);
    t->Unload();
    REQUIRE(!cf.clk.prerolls.empty());
    CHECK(!cf.clk.prerolls.back().active);   // and the clock comes back on this exit too
}

// ---- PREROLL-FAST-1: two scales, one ramp, the same exit -----------------------------------------
// The 45 s pre-match is an empty countdown and is run out at playback.preroll_clock_scale; the 20 s
// launch sequence behind it teleports and launches bodies and stays at playback.preroll_rate. What
// the clock RECEIVED is the assertion, as everywhere else in this block.

TEST_CASE("PREROLL-FAST-1: the pre-match runs fast, the launch sequence ramps down, and it latches") {
    ClockFixture cf;
    PreRollScope cfg(true, 30000, 2.0, 6.0);   // launch 2x, pre-match 6x
    HostPhaseScope host(0);
    SeamEntityFixture f;
    Env& e = f.e;
    ArmNetGame netGame;
    replay::TickInfo info{};
    info.netGame = netGame.bytes;

    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 6, 1, 2000000, nullptr, 0, 0, true));
    replay::IReplayTransport* t = e.services.transport;
    const size_t from = e.log.lines.size();
    REQUIRE(t->Load(tmp.path));
    const int64_t t0 = t->PlayheadUs();
    HostPhaseScope::Set(kSymPreMatch);
    cf.clk.prerolls.clear();
    for (int i = 0; i < 3; ++i) e.tick.FireAll(info);

    REQUIRE(!cf.clk.prerolls.empty());
    CHECK(cf.clk.prerolls.back().active);
    CHECK(cf.clk.prerolls.back().rate == 6.0f);
    CHECK(CountLoggedSince(from, "playback", "the engine clock goes from") == 0);

    // The host reaches its launch sequence: back to the proven rate, once, with a line naming it.
    HostPhaseScope::Set(kSymPreLaunch);
    e.tick.FireAll(info);
    CHECK(cf.clk.prerolls.back().rate == 2.0f);
    CHECK(CountLoggedSince(from, "playback", "the engine clock goes from") == 1);

    // ★ AND IT LATCHES. A phase republish that reads as pre-match again must not put the clock back
    // up under a host that is already launching -- that would be a rate change per frame.
    HostPhaseScope::Set(kSymPreMatch);
    e.tick.FireAll(info);
    CHECK(cf.clk.prerolls.back().rate == 2.0f);
    CHECK(CountLoggedSince(from, "playback", "the engine clock goes from") == 1);

    // Nothing is visible throughout: the exit condition has not been met, so the picture is still
    // the frame the owner left and the transport still says so.
    CHECK(t->PlayheadUs() == t0);
    CHECK(t->State() == replay::TransportState::Pause);
    CHECK(t->Preparing().preparing);
    t->Unload();
    CHECK(!cf.clk.prerolls.back().active);
}

TEST_CASE("PREROLL-FAST-1: the timeline charges every slice once and reports the engine's own seconds") {
    ClockFixture cf;
    PreRollScope cfg(true, 40, 2.0, 6.0);   // 40 ms, so the give-up is reached inside the case
    HostPhaseScope host(0);
    SeamEntityFixture f;
    Env& e = f.e;
    ArmNetGame netGame;
    replay::TickInfo info{};
    info.netGame = netGame.bytes;

    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 6, 1, 2000000, nullptr, 0, 0, true));
    replay::IReplayTransport* t = e.services.transport;
    const size_t from = e.log.lines.size();
    REQUIRE(t->Load(tmp.path));
    HostPhaseScope::Set(kSymPreMatch);

    // The engine's sim clock, handed in the way the tick provider hands it in. Half a second a
    // tick: inside the one-second plausibility bound, so every slice is a sample and none is
    // rejected.
    info.simTimeUs = 1000000;
    cf.clk.prerolls.clear();
    for (int i = 0; i < 3; ++i) e.tick.FireAll(info);   // reach the fast clock; sim stands still
    REQUIRE(!cf.clk.prerolls.empty());
    REQUIRE(cf.clk.prerolls.back().rate == 6.0f);
    info.simTimeUs += 500000;
    e.tick.FireAll(info);                      // 0.5 s of pre-match
    REQUIRE(t->Preparing().preparing);
    const double remainingFast = t->Preparing().hostRemainingS;
    CHECK(remainingFast < 65.0);               // the engine's own seconds came off the estimate
    CHECK(remainingFast > 63.0);

    HostPhaseScope::Set(kSymPreLaunch);
    info.simTimeUs += 500000;
    e.tick.FireAll(info);                      // 0.5 s of launch sequence
    CHECK(t->Preparing().hostRemainingS < remainingFast);

    Sleep(60);
    info.simTimeUs += 500000;
    e.tick.FireAll(info);                      // the bound expires: the hold ends here
    REQUIRE(!t->Preparing().preparing);

    double waitW = -1, waitE = -1, fastW = -1, fastE = -1, rampW = -1, rampE = -1, totW = -1,
           totE = -1;
    REQUIRE(TimelineRow(from, "wait", waitW, waitE));
    REQUIRE(TimelineRow(from, "pre-match", fastW, fastE));
    REQUIRE(TimelineRow(from, "launch", rampW, rampE));
    REQUIRE(TimelineRow(from, "TOTAL", totW, totE));
    INFO("wait ", waitW, "/", waitE, " fast ", fastW, "/", fastE, " launch ", rampW, "/", rampE,
         " total ", totW, "/", totE);
    // Every slice is charged to exactly one phase, so the three add up to the total -- to the 0.1 s
    // the line itself prints, which is the resolution the claim is made at.
    CHECK(Abs(waitW + fastW + rampW - totW) <= 0.2);
    CHECK(Abs(waitE + fastE + rampE - totE) <= 0.2);
    // 1.5 s of engine time was handed in after the fast clock took over, and every microsecond of
    // it is on one of the two running rows. The tick the ramp is decided IN is charged to the
    // pre-match, which is where the module was for the interval that tick measured -- so the split
    // is 1.0 / 0.5, not 0.5 / 1.0.
    CHECK(fastE == doctest::Approx(1.0).epsilon(0.001));
    CHECK(rampE == doctest::Approx(0.5).epsilon(0.001));
    CHECK(CountLoggedSince(from, "playback", "preroll: timeline") == 4);
    t->Unload();
}

TEST_CASE("PRE-ROLL: a host in its launch sequence names the phase it gave up in") {
    ClockFixture cf;
    PreRollScope cfg(true, 40, 2.0);   // 40 ms, so the give-up is reached inside the case
    HostPhaseScope host(0);
    SeamEntityFixture f;
    Env& e = f.e;
    ArmNetGame netGame;
    replay::TickInfo info{};
    info.netGame = netGame.bytes;

    TempDemo tmp;
    REQUIRE(WriteDemo(tmp.path, 5000000, 11111, 60, 6, 1, 2000000, nullptr, 0, 0, true));
    replay::IReplayTransport* t = e.services.transport;
    const size_t from = e.log.lines.size();
    REQUIRE(t->Load(tmp.path));
    HostPhaseScope::Set(kSymPreLaunch);
    e.tick.FireAll(info);
    REQUIRE(t->Preparing().preparing);
    Sleep(60);
    e.tick.FireAll(info);
    CHECK(!t->Preparing().preparing);
    CHECK(CountLoggedSince(from, "playback", "PRE-ROLL GAVE UP") == 1);
    for (size_t i = from; i < e.log.lines.size(); ++i) {
        if (e.log.lines[i].text.find("GAVE UP") != std::string::npos) {
            INFO("gave-up line: ", e.log.lines[i].text);
            CHECK(e.log.lines[i].text.find("pre_launch_enter") != std::string::npos);
        }
    }
    REQUIRE(!cf.clk.prerolls.empty());
    CHECK(!cf.clk.prerolls.back().active);
    t->Unload();
}
