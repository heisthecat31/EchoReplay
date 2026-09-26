#include "livesafety.h"

#include "log.h"
#include "offlinelock.h"

namespace replay::livesafety {
namespace {

const char* const kModuleName = "live-safety";

struct State {
    // Default FALSE, and that is not a fail-open: the LOCK term below answers correctly on its own
    // at every instant of a real process. The lock is applied during boot, before any playback path
    // can run, and until then it reads NotAttempted -- which refuses. This term is the second
    // defence, for the session where the lock was deliberately not attempted at all.
    bool liveExempt = false;
    bool override_ = false;
    Verdict overrideVerdict = Verdict::NotDecided;
};

State& S() {
    static State s;
    return s;
}

bool LockHeld() {
    const offlinelock::LockState lock = offlinelock::Current().state;
    return lock == offlinelock::LockState::Locked ||
           lock == offlinelock::LockState::AlreadyLockedInProcess;
}

}  // namespace

const char* VerdictName(Verdict v) {
    switch (v) {
        case Verdict::Contained: return "contained";
        case Verdict::LiveOnlineExempt: return "live-online";
        case Verdict::LockNotHeld: return "lock-not-held";
        case Verdict::NotDecided: return "not-decided";
    }
    return "?";
}

const char* VerdictReason(Verdict v) {
    switch (v) {
        case Verdict::Contained:
            return "the offline-bit lock is held and this is not a live online session";
        case Verdict::LiveOnlineExempt:
            return "live online session: the offline-bit lock is not applied, so nothing here may "
                   "write engine state";
        case Verdict::LockNotHeld:
            return "the offline-bit lock is not held, so this process is not contained";
        case Verdict::NotDecided:
            return "boot has not classified this process yet";
    }
    return "?";
}

Verdict Classify(bool liveOnlineExempt, bool offlineLockHeld) {
    if (liveOnlineExempt) return Verdict::LiveOnlineExempt;
    if (!offlineLockHeld) return Verdict::LockNotHeld;
    return Verdict::Contained;
}

const char* FeatureName(Feature f) {
    switch (f) {
        case Feature::EngineClock: return "engine-clock";
        case Feature::PlayerPose: return "player-pose";
        case Feature::GripWrites: return "grip-writes";
        case Feature::DiscDrive: return "disc-drive";
        case Feature::Dress: return "dress";
        case Feature::PreRoll: return "pre-roll";
        case Feature::Scrub: return "scrub";
        case Feature::AudioRate: return "audio-rate";
        case Feature::CursorClaim: return "cursor-claim";
        case Feature::GameSpaceRegister: return "gamespace-register";
        case Feature::ReplayLoad: return "replay-load";
        case Feature::Camera: return "camera";
        case Feature::LodThrottle: return "lod-throttle";
        case Feature::Stats: return "stats";
        case Feature::GoalEvents: return "goal-events";
        case Feature::EntrantRelease: return "entrant-release";
        case Feature::ViewerSeat: return "viewer-seat";
        case Feature::WatchdogBracket: return "watchdog-bracket";
        case Feature::VoipTalkerSlots: return "voip-talker-slots";
        case Feature::BootModeBits: return "boot-mode-bits";
        case Feature::ReplyDrain: return "reply-drain";
        case Feature::HttpRoutes: return "http-routes";
        case Feature::HostHold: return "host-hold";
        case Feature::DiscTouchState: return "disc-touch-state";
        case Feature::ComponentEvents: return "component-events";
        case Feature::GoalExplosion: return "goal-explosion";
        case Feature::StunEvents: return "stun-events";
        case Feature::LevelCycle: return "level-cycle";
        case Feature::HostClockPin: return "host-clock-pin";
        case Feature::DiscShellFeed: return "disc-shell-feed";
        case Feature::FoleyMix: return "foley-mix";
        case Feature::Count: break;
    }
    return "?";
}

Verdict Current() {
    const State& s = S();
    if (s.override_) return s.overrideVerdict;
    return Classify(s.liveExempt, LockHeld());
}

bool Allowed(Feature) { return Current() == Verdict::Contained; }

void PublishBootFacts(bool liveOnlineExempt, bool) { S().liveExempt = liveOnlineExempt; }

void LogStartupBlock() {
    const Verdict v = Current();
    if (v == Verdict::Contained) {
        log::Direct(LogLevel::Info, kModuleName,
                    "contained offline playback: every playback write path is armed (%s)",
                    VerdictReason(v));
        return;
    }
    log::Direct(LogLevel::Info, kModuleName,
                "%s -- %s. Every engine-writing playback path below is inert; the recorder is "
                "read-only on the engine",
                VerdictName(v), VerdictReason(v));
    for (uint8_t i = 0; i < static_cast<uint8_t>(Feature::Count); ++i) {
        log::Direct(LogLevel::Info, kModuleName, "live-safety: %s=inert(%s)",
                    FeatureName(static_cast<Feature>(i)), VerdictName(v));
    }
}

bool HttpRoutesAllowed(bool liveOverride) {
    return liveOverride || Allowed(Feature::HttpRoutes);
}

bool OfflinePatchAllowed(bool replayRequested, bool liveOnlineExempt) {
    return replayRequested && !liveOnlineExempt;
}

void SetForTest(bool liveOnlineExempt, bool offlineLockHeld) {
    State& s = S();
    s.override_ = true;
    s.overrideVerdict = Classify(liveOnlineExempt, offlineLockHeld);
}

void ResetForTest() {
    State& s = S();
    s.override_ = false;
    s.overrideVerdict = Verdict::NotDecided;
    s.liveExempt = false;
}

}  // namespace replay::livesafety
