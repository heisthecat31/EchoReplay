#include "playback/core/scoregate.h"

#include <atomic>
#include <string.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/config.h"
#include "bootstrap/log.h"

namespace replay {
namespace playback {
namespace scoregate {

namespace {

constexpr uint64_t kOnScoreCbVa = 0x140185DF0ull;  // facts-ok: NETGAME.ON_SCORE_CB pending; CR15NetGame::OnScoreCB(rules, msg), only caller the thunk sub_14013D770
constexpr uint8_t kOnScoreCbPrologue[24] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};  // facts-ok: NETGAME.ON_SCORE_CB prologue

struct State {
    std::atomic<uint32_t> mode{uint32_t(Mode::PassThrough)};
    std::atomic<int64_t>  playhead_us{0};
    std::atomic<uint32_t> swallowed{0};
    std::atomic<uint32_t> passed{0};
    std::atomic<int64_t>  last_swallowed_us{0};
    std::atomic<uint8_t>  installed{0};
    OnScoreFn original = nullptr;
};
State g;

HookCreateFn g_hookCreate = &hookmgr::Create;
HookArmFn    g_hookArm = &hookmgr::Arm;
VerifySiteFn g_verifySite = &buildguard::VerifySite;

int64_t __fastcall OnScoreDetour(void* rules, void* msg) {
    const Mode mode = Mode(g.mode.load(std::memory_order_relaxed));
    if (mode == Mode::Swallow) {
        g.last_swallowed_us.store(g.playhead_us.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
        g.swallowed.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    if (mode == Mode::Observe) g.passed.fetch_add(1, std::memory_order_relaxed);
    return g.original(rules, msg);
}

}  // namespace

bool Install() {
    if (Installed()) return true;

    const char* resolved = config::Interface()->GetString(kLedgerKey, "auto");
    if (resolved == nullptr || strcmp(resolved, "hook") != 0) {
        log::Direct(LogLevel::Info, "playback",
                    "host score gate: not installed (%s is '%s'; bootstrap resolves it to 'hook' "
                    "only for a contained process with playback.disc_drive on)",
                    kLedgerKey, resolved != nullptr ? resolved : "");
        return false;
    }
    if (!g_verifySite(kFactId, kOnScoreCbVa, kOnScoreCbPrologue, sizeof kOnScoreCbPrologue)) {
        log::Direct(LogLevel::Warn, "playback",
                    "host score gate: %s does not hold its recorded prologue; not installed",
                    kFactId);
        return false;
    }

    // Into a local, published before Arm: an armed detour forwards through it on its next call.
    void* trampoline = nullptr;
    const HookStatus created =
        g_hookCreate(kFactId, kOnScoreCbVa, kOnScoreCbPrologue, sizeof kOnScoreCbPrologue,
                     reinterpret_cast<void*>(&OnScoreDetour), &trampoline, kLedgerKey);
    if (created != HookStatus::Created || trampoline == nullptr) {
        log::Direct(LogLevel::Warn, "playback", "host score gate: %s not created (%s)", kFactId,
                    HookStatusName(created));
        return false;
    }
    g.original = reinterpret_cast<OnScoreFn>(trampoline);

    const HookStatus armed = g_hookArm(kFactId);
    if (armed != HookStatus::Armed) {
        log::Direct(LogLevel::Warn, "playback", "host score gate: %s not armed (%s)", kFactId,
                    HookStatusName(armed));
        return false;
    }

    g.installed.store(1, std::memory_order_release);
    log::Direct(LogLevel::Info, "playback",
                "host score gate: CR15NetGame::OnScoreCB detour armed (ledger key %s), "
                "pass-through until a replay is loaded and in a match",
                kLedgerKey);
    return true;
}

bool Installed() { return g.installed.load(std::memory_order_acquire) != 0; }

void Publish(Mode mode, int64_t playheadUs) {
    if (!Installed()) mode = Mode::PassThrough;
    g.playhead_us.store(playheadUs, std::memory_order_relaxed);
    g.mode.store(uint32_t(mode), std::memory_order_release);
}

Mode CurrentMode() { return Mode(g.mode.load(std::memory_order_relaxed)); }

void DrainCounts(Counts& out) {
    out.swallowed = g.swallowed.exchange(0, std::memory_order_relaxed);
    out.passed = g.passed.exchange(0, std::memory_order_relaxed);
    out.lastSwallowedPlayheadUs = g.last_swallowed_us.load(std::memory_order_relaxed);
}

void SetHookAccessForTest(HookCreateFn create, HookArmFn arm, VerifySiteFn verify) {
    g_hookCreate = create != nullptr ? create : &hookmgr::Create;
    g_hookArm = arm != nullptr ? arm : &hookmgr::Arm;
    g_verifySite = verify != nullptr ? verify : &buildguard::VerifySite;
}

OnScoreFn DetourForTest() { return &OnScoreDetour; }

Decision Decide(const GateState& s) {
    Decision d;
    if (!s.contained) {
        d.why = "not contained offline playback, so any goal is the host's own";
        return d;
    }
    if (!s.loaded) {
        d.why = "no replay is loaded, so any goal is the host's own";
        return d;
    }
    d.mode = s.gate_on ? Mode::Swallow : Mode::Observe;
    d.why = s.gate_on ? "a replay is loaded, so no goal is the host's own"
                      : "playback.disc_score_gate is off";
    return d;
}

const char* ModeName(Mode m) {
    switch (m) {
        case Mode::PassThrough: return "pass-through";
        case Mode::Observe:     return "observe";
        case Mode::Swallow:     return "swallow";
    }
    return "?";
}

void SetOriginalForTest(OnScoreFn original) {
    g.original = original;
    g.installed.store(1, std::memory_order_release);
}

void ResetForTest() {
    g.mode.store(uint32_t(Mode::PassThrough), std::memory_order_release);
    g.installed.store(0, std::memory_order_release);
    g.original = nullptr;
    g.playhead_us.store(0, std::memory_order_relaxed);
    g.last_swallowed_us.store(0, std::memory_order_relaxed);
    Counts discard;
    DrainCounts(discard);
    SetHookAccessForTest(nullptr, nullptr, nullptr);
}

}  // namespace scoregate
}  // namespace playback
}  // namespace replay
