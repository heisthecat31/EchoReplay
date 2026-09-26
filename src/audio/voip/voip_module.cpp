// P-? `voip`. Arms the capture detour on VOIP.RECEIVE and drives replayed voice back through it.
//
// ★ ONE LEDGER ROW GOVERNS BOTH HALVES. `capture.voip` (row 2.3) reads "Capture and replay voice",
// and it is the switch here too: a build that could record voice but not replay it, or the other
// way round, would need two keys to describe one capability and would make "voice is off" ambiguous.
//
// ★ EVERY OUTCOME IS ANNOUNCED, INCLUDING SILENCE. Voice has more ways to be absent than any other
// stream in this project -- the file may have no voice section, the receiver may not resolve, the
// engine's own provider may be null, no live instance may carry a recorded speaker, and the viewer
// may simply have muted them. A replay that is quietly silent reads as a broken feature, so the
// module logs which one it is at load and the player's StatusLine keeps saying so.

#include "audio/voip/voipcapture.h"
#include "audio/voip/voipplay.h"
#include "bootstrap/services.h"
#include "engine/entity.h"
#include "engine/session.h"
#include "engine/tick.h"
#include "playback/core/transport.h"

#include <string.h>
#include <wchar.h>

#include <chrono>

namespace replay {
namespace {

const char* const kReplayModuleName = "voip";

ReplayServices* g_services = nullptr;
audio::voip::VoipPlayer g_player;
bool g_enabled = false;

wchar_t g_demoPath[512] = {};
bool g_pathPending = false;


// The bind-epoch term of VoipSpaceKey. AUD-F3D F3D-2: {netGame, space, readable} reads identically
// across a game space torn down and rebuilt at the SAME address, and the held VoipSite would then
// keep injecting replayed voice through the freed image's receiver. This is the discriminator
// entity_module already maintains -- it moves on exactly the edge where SpaceIdentity's `entries`
// digest of the system table differs (entity_module.cpp:95-102, entity.h:310-317).
//
// The three IEntity answers are mapped by hand rather than cast: they are three states, and the
// enum values on either side are free to move independently.
//
// Read live rather than latched at init, because `services->entity` is only non-null once W1-E has
// resolved and this module inits at order 46, long before that.
audio::voip::BindEpochTerm VoipBindEpoch() {
    audio::voip::BindEpochTerm t;
    IEntity* e = g_services != nullptr ? g_services->entity : nullptr;
    if (e == nullptr) {
        return t;
    }
    uint64_t gen = 0;
    switch (e->BindEpochNow(gen)) {
        case IEntity::BindEpoch::Ok:
            t.state = audio::voip::BindEpochTerm::State::Bound;
            t.value = gen;
            break;
        case IEntity::BindEpoch::Unbound:
            t.state = audio::voip::BindEpochTerm::State::Unbound;
            break;
        case IEntity::BindEpoch::NotAvailable:
            break;
    }
    return t;
}

bool LiveSlotFromTransport(uint16_t recordedSlot, uint16_t* liveSlot, void* ctx) {
    return static_cast<const IReplayTransport*>(ctx)->LiveUserSlot(recordedSlot, liveSlot);
}

// Voice used to report only at load, so a session where it came and went left nothing in the log to
// say which refusal was eating it. Silent while nothing moves.
void LogStatusEvery30s() {
    static std::chrono::steady_clock::time_point last{};
    static audio::voip::VoipPlayStats seen{};
    const auto now = std::chrono::steady_clock::now();
    if (g_services->log == nullptr || now - last < std::chrono::seconds(30)) {
        return;
    }
    last = now;
    const audio::voip::VoipPlayStats& s = g_player.Stats();
    if (s.recordsRead == seen.recordsRead) {
        return;
    }
    char line[192];
    g_services->log->Write(
        LogLevel::Info, kReplayModuleName,
        "%s | last 30 s: read %llu, injected %llu, not-at-1x %llu, not-seated %llu, no-talker %llu, "
        "gate %llu, no-site %llu, discontinuities %llu",
        g_player.StatusLine(line, sizeof line),
        static_cast<unsigned long long>(s.recordsRead - seen.recordsRead),
        static_cast<unsigned long long>(s.injected - seen.injected),
        static_cast<unsigned long long>(s.skippedRate - seen.skippedRate),
        static_cast<unsigned long long>(s.refusedNotSeated - seen.refusedNotSeated),
        static_cast<unsigned long long>(s.refusedNoTalker - seen.refusedNoTalker),
        static_cast<unsigned long long>(s.refusedGate - seen.refusedGate),
        static_cast<unsigned long long>(s.refusedNoSite - seen.refusedNoSite),
        static_cast<unsigned long long>(s.discontinuities - seen.discontinuities));
    seen = s;
}

// The session's netGame as of RIGHT NOW. The teardown paths below do not go through a tick, so
// this is the only term that can tell a replaced space from the one our pool slots were bound
// against; every term re-read through the bound pointer answers the same either way (AUD-F3I
// F3I-1).
void* LiveNetGame() {
    return g_services != nullptr && g_services->session != nullptr ? g_services->session->NetGame()
                                                                   : nullptr;
}

void VoipTick(const TickInfo& /*info*/) {
    if (!g_enabled || g_services == nullptr) {
        return;
    }
    IReplayTransport* transport = g_services->transport;
    if (transport == nullptr) {
        return;
    }
    // Read once, above the load: Load's implicit Unload can release engine-side talker slots too,
    // and it must be keyed on the current netGame for the same reason the teardowns are.
    void* netGame = LiveNetGame();
    // Above the load, because Load's implicit Unload can park a claim of its own and a retry in the
    // same tick would be attempting it before the table has had any chance to settle.
    g_player.RetryPendingRelease(netGame);
    if (g_pathPending) {
        g_pathPending = false;
        char line[192];
        if (g_player.Load(g_demoPath, g_services->log, netGame) && g_services->log != nullptr) {
            g_services->log->Write(LogLevel::Info, kReplayModuleName, "%s",
                                   g_player.StatusLine(line, sizeof line));
        }
    }
    if (!g_player.Loaded()) {
        return;
    }

    // ★ THE PLAYHEAD IS PASSED, AND NOTHING IS REMEMBERED HERE. This module used to keep its own
    // `lastPlayhead` and hand it back as the span's start, which silently defeated the player's
    // jump detection: the value it compared against was the one this loop had just mirrored. The
    // player owns both ends of the span now, so there is no state here to drift out of step with
    // it. The first call establishes the span and delivers nothing, which is why the old
    // first-tick special case is gone too.
    // ★ THIS CALL IS UNCONDITIONAL IN THE RATE, AND THAT IS LOAD-BEARING (AUD-F3F F3F-4). Advance
    // observes the game space before any of its own early returns, so a space replaced while the
    // transport is paused or scrubbing is seen HERE, on the tick, rather than at the next 1.0x
    // frame that may never come. Anything added between this line and the top of the function must
    // not become a rate or playhead gate.
    g_player.SetLiveSlotMap(&LiveSlotFromTransport, transport);
    g_player.Advance(transport->PlayheadUs(), transport->Rate(), netGame);
    LogStatusEvery30s();
}

bool VoipInit(ReplayServices* services) {
    REPLAY_REQUIRE(services, log);
    REPLAY_REQUIRE(services, config);
    g_services = services;
    ILog* log = services->log;

    audio::voip::SetBindEpochSource(&VoipBindEpoch);

    g_enabled = services->config->GetBool("capture.voip", true);
    if (!g_enabled) {
        log->Write(LogLevel::Info, kReplayModuleName,
                   "capture.voip is off: voice is neither recorded nor replayed");
        return true;
    }

    // Create is safe from DllMain and Arm is not, so they are two calls -- B-4's loader-lock split.
    // Module init already runs on the worker, so both happen here and the status is reported once.
    const HookStatus created = audio::voip::CreateVoipCapture();
    HookStatus armed = created;
    if (created == HookStatus::Created) {
        armed = audio::voip::ArmVoipCapture();
    }
    log->Write(armed == HookStatus::Armed ? LogLevel::Info : LogLevel::Warn, kReplayModuleName,
               "VOIP.RECEIVE: %s", HookStatusName(armed));
    return true;
}

// ★ THIS IS WRITTEN FOR PROCESS TERMINATION UNDER THE LOADER LOCK, AND MAKES NO ENGINE CALL
// (AUD-F3L F3L-3). Nothing in a shipping build runs it any more: ENG-EXIT-FIX took
// modreg::ShutdownAll() out of bootstrap::Detach, which now only flushes the log when the process
// is terminating, so ShutdownAll -- and with it every module Shutdown -- has test callers only.
// The rule stands for whoever calls it again: at that point every other thread has been terminated
// wherever it stood. CS.LOOKUP_BY_SYMBOL sorts the gamespace's system table in place, and
// VOIP.DISCONNECT writes into a receiver whose owning threads are dead; console.cpp draws the
// identical rule for the console and names the cost of breaking it -- "Echo VR would not close and
// would have to be killed from Task Manager." And the release would buy nothing: no one will ever
// read that pool again.
//
// So there is no LiveNetGame() here either -- that is a virtual call through the session object,
// which is another module's, on a thread that must assume nothing is still standing.
void VoipShutdown() {
    audio::voip::SetVoipSink(nullptr, nullptr);
    g_player.UnloadAtTermination();
    if (g_services != nullptr && g_services->log != nullptr) {
        // One line, into the lock-free ring log::Direct is built for (log.cpp:112 keeps its state at
        // namespace scope precisely so it is usable under the loader lock).
        g_services->log->Write(LogLevel::Info, kReplayModuleName,
                               "shutdown at process termination: %u talker slot(s) dropped without "
                               "an engine call",
                               g_player.Stats().abandonedAtTermination);
    }
    audio::voip::SetBindEpochSource(nullptr);
    g_services = nullptr;
    g_enabled = false;
}

// Order 46: immediately after `audio` (45), because a voice frame is handed to the engine's own
// receiver and the receiver's sink is the thing `audio` has just finished reporting on.
REPLAY_MODULE("voip", 46, VoipInit, VoipTick, VoipShutdown);

}  // namespace

namespace audio {
namespace voip {

// The seam whoever owns the transport calls when a demo is loaded, mirroring the shape W2-H's
// SetAudioKeyframeIntervalUs already uses. The path is COPIED: the caller's buffer is its own.
//
// Absent by default, and the absence is visible -- a replay whose path was never handed over
// reports "no demo loaded" rather than pretending the recording carries no voice.
void SetVoipDemoPath(const wchar_t* path) {
    if (path == nullptr || path[0] == L'\0') {
        g_demoPath[0] = L'\0';
        g_pathPending = false;
        g_player.Unload(LiveNetGame());
        return;
    }
    wcsncpy_s(g_demoPath, path, _TRUNCATE);
    g_pathPending = true;
}

const VoipPlayStats& PlaybackStats() { return g_player.Stats(); }

const char* PlaybackStatusLine(char* buf, size_t cap) { return g_player.StatusLine(buf, cap); }

}  // namespace voip
}  // namespace audio
}  // namespace replay
