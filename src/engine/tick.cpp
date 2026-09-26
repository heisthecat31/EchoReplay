// E-1 / B-3: the per-frame tick, and the CNSProvider slot claim that buys it with zero hooks.
//
// ★ THE PROPERTY THIS FILE EXISTS TO PROTECT.
//
// CR15Game holds three hard-coded CNSProvider module handles at +0x8500 (pnsovr), +0x8508 (pnsrad) and
// +0x8510 (pnsdemo). CR15Game::Initialize creates only TWO of them: pnsrad always, plus pnsovr OR
// pnsdemo, selected by bit 27 of CR15Game+0x7AE0, which `-noovr` and nothing else sets. So one slot is
// NULL for the whole process lifetime, and storing our own pinned HMODULE there makes the engine call
// our exported Update() once per frame with its own scaled delta -- no detour, no patched byte.
//
// ★ AN EXHAUSTIVE CENSUS IS WHAT MAKES THAT SAFE, not an assumption. Every instruction in echovr.exe
//   that reads or writes +0x8500/+0x8508/+0x8510 -- all 19 of them -- lives in exactly four functions:
//     sub_1400F6130  CR15Game's constructor      zeroes all three
//     sub_140109810  CR15Game::Initialize        creates two, reads them back
//     sub_140127420  CR15Game::Update            the per-frame `if (slot) CNSProvider::Update(slot)`
//     sub_1401223A0  CR15Game::Shutdown          Destroy(slot) x3, then NULLs all three
//   Nothing else in 5.66M instructions touches a slot. Our HMODULE therefore exists in exactly one
//   place the engine can find it, and the only two names it can ever resolve against that handle are
//   "Update" (from CNSProvider::Update) and "RadPluginShutdown" (from CNSProvider::Destroy).
//   That is why exporting exactly two symbols is not a bet -- see docs/spikes/W1-D.md S5.
//
// ★ -noovr FLIPS WHICH SLOT IS FREE, so the free slot is PROBED, never assumed, and a shape we did not
//   verify (both free, or both taken) ABORTS to the fallback rather than guessing.

#include "engine/tick.h"

#include <windows.h>

#include <atomic>

#include "bootstrap/buildguard.h"
#include "bootstrap/hookmgr.h"
#include "bootstrap/log.h"
#include "bootstrap/modreg.h"
#include "engine/tick_facts.h"
#include "engine/tick_policy.h"
#include "generated/facts_generated.h"

namespace replay {
namespace tick {
namespace {

const char kReplayModuleName[] = "tick";

// The claim waits on the engine rather than on a fixed delay, but every wait has a deadline. ★ The
// design's sequence has none, and there is a real configuration that would hang it forever: the whole
// provider-creation block in CR15Game::Initialize sits under a guard on CR15Game+0x8A70, and when that
// guard refuses, NO provider is created and +0x8508 never becomes non-null. An unbounded wait there
// would never claim and never fall back -- the plugin would simply do nothing, silently.
constexpr DWORD kPollIntervalMs = 25;
// Generous: the engine has to get all the way to a running frame loop, which on a cold start with a
// shader cache miss is not quick. It is a constant rather than a config key because config_keys.inc is
// W1-C's file and an undeclared key would log "unknown key" on every run; the request is in the memo.
constexpr uint64_t kClaimTimeoutMs = 120000;

// Frames the engine must render before we touch anything, so we claim into a running frame loop rather
// than into a half-built CR15Game. The design asks for two.
constexpr uint64_t kFramesBeforeClaim = 2;

constexpr size_t kMaxCallbacks = 32;

struct Callback {
    TickFn fn;
    void*  ctx;
};

struct State {
    Callback              early[kMaxCallbacks];
    std::atomic<uint32_t> earlyCount;

    std::atomic<uint32_t> source;      // Source
    std::atomic<uint32_t> claim;       // ClaimResult
    std::atomic<uint32_t> slotOffset;

    std::atomic<uint64_t> rawCalls;
    std::atomic<uint64_t> zeroDeltas;
    std::atomic<uint64_t> deltaSumUs;

    // Written only by Dispatch, which the engine calls from one thread (the frame loop) whichever
    // implementation is live. GetStats reads it without synchronising: it is a diagnostic snapshot and
    // may land mid-update, which is why the arithmetic itself is proven in tests/tick rather than by
    // reading these fields at runtime.
    FrameAudit frames;

    std::atomic<void*>    game;        // CR15Game*
    std::atomic<void*>    clock;       // the GLOBAL CClock, the one CNSProvider::Update reads
    std::atomic<bool>     stopping;
    std::atomic<bool>     lateRefusalLogged;

    // The clock re-ask's state (tick_policy.h S7). clockCell is image geometry resolved once; the
    // other three are what lets the re-ask back off without ever going silent.
    ImageCellCache        clockCell;
    std::atomic<uint64_t> clockWaitStartMs;
    std::atomic<uint64_t> clockNullDispatches;
    std::atomic<bool>     clockHorizonLogged;

    HANDLE claimThread;
    HANDLE stopEvent;
    HMODULE self;
    char   modeName[32];
};

State g;

// ---------------------------------------------------------------------------------------------------
// Engine access. buildguard::MappedAddress is the one place in the tree that rebases a recorded VA onto
// the image the loader actually mapped, so every address below goes through it.
// ---------------------------------------------------------------------------------------------------

uint8_t* GameSingleton() {
    const void* cell = buildguard::MappedAddress(facts::GAME_SINGLETON_VA);
    if (cell == nullptr) {
        return nullptr;
    }
    uint8_t* game = *reinterpret_cast<uint8_t* const volatile*>(cell);
    // A single aligned 8-byte store on x64 cannot tear, so this is either the pointer or still null.
    // The alignment test rejects a garbage read from a wrong image base rather than dereferencing it.
    if (game == nullptr || (reinterpret_cast<uintptr_t>(game) & 7u) != 0) {
        return nullptr;
    }
    return game;
}

const void* ResolveClockCell(void*) { return buildguard::MappedAddress(facts::CLOCK_GLOBAL_PTR); }

// __try, not a VirtualQuery probe: a probe answers about the instant it ran and the read it protects
// happens afterwards, so it never covered an unmap in between. A guarded load answers about the instant
// of the load, is zero-cost on x64 when it does not fault, and refuses every case the probe refused.
// ENG-COST S2 made the same trade on the capture tick.
bool TryLoadPointer(const void* at, uint8_t** out) {
    __try {
        *out = *reinterpret_cast<uint8_t* const volatile*>(at);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                        GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return true;
}

uint8_t* GlobalClock() {
    // The cell is a fixed address inside the main image, so whether it is mapped is a property of the
    // process and not of this frame: the two VirtualQuery inside MappedAddress run once for the life
    // of the process, and every later call is this load.
    const void* cell = g.clockCell.Get(&ResolveClockCell, nullptr);
    if (cell == nullptr) {
        return nullptr;
    }
    uint8_t* clock = nullptr;
    if (!TryLoadPointer(cell, &clock)) {
        return nullptr;
    }
    // A single aligned 8-byte store on x64 cannot tear, so this is either the pointer or still null.
    if (clock == nullptr || (reinterpret_cast<uintptr_t>(clock) & 7u) != 0) {
        return nullptr;
    }
    return clock;
}

uint64_t ReadFrameCounter(uint8_t* game) {
    return *reinterpret_cast<const volatile uint64_t*>(game + tick_facts::kFrameCounterOff);
}

void** SlotAt(uint8_t* game, uint32_t offset) {
    return reinterpret_cast<void**>(game + offset);
}

// ---------------------------------------------------------------------------------------------------
// Dispatch. Runs on the game's main thread, inside the frame. Allocation-free and lock-free.
// ---------------------------------------------------------------------------------------------------

void Dispatch(uint64_t scaledDeltaUs) {
    g.rawCalls.fetch_add(1, std::memory_order_relaxed);
    if (g.stopping.load(std::memory_order_acquire)) {
        return;
    }
    uint8_t* game = static_cast<uint8_t*>(g.game.load(std::memory_order_acquire));
    if (game == nullptr) {
        return;
    }

    const uint64_t frame = ReadFrameCounter(game);
    uint8_t* clock = static_cast<uint8_t*>(g.clock.load(std::memory_order_acquire));
    if (clock == nullptr) {
        // * ASK AGAIN. The clock is resolved at slot-claim time, but the claim happens as early as we
        // can get it and the engine constructs the global CClock later, so a claim that wins that race
        // reads a null CLOCK.GLOBAL_PTR and simTimeUs is 0 for the life of the process. Every
        // frameDeltaUs is then 0 and everything downstream that divides by a frame period has nothing
        // to divide by: L3-RUN-2 F3, ui::RateCeiling() 0 for a whole launch.
        //
        // * BUT NOT ON EVERY FRAME FOREVER. This arm used to call MappedAddress -- two VirtualQuery --
        // on every frame until the clock bound, and on the build where it never binds, on every frame
        // of the launch (AUD-P17 finding 3). The cell is now resolved once and the re-ask backs off
        // after the first second, which is the window a bind can plausibly land in and the window the
        // late-bind line below has to stay frame-accurate over.
        const uint64_t nowMs = GetTickCount64();
        uint64_t startMs = g.clockWaitStartMs.load(std::memory_order_relaxed);
        if (startMs == 0) {
            g.clockWaitStartMs.store(nowMs, std::memory_order_relaxed);
            startMs = nowMs;
        }
        const uint64_t elapsedMs = nowMs - startMs;
        const uint64_t nulls = g.clockNullDispatches.fetch_add(1, std::memory_order_relaxed);
        if (ShouldReaskClock(nulls, elapsedMs)) {
            clock = GlobalClock();
            if (clock != nullptr) {
                g.clock.store(clock, std::memory_order_release);
                log::Direct(LogLevel::Info, kReplayModuleName,
                            "the engine's global CClock resolved %llu dispatch(es) and %llu ms after "
                            "the claim; every frame delta before this one was 0, so a frame period "
                            "can be measured from here",
                            static_cast<unsigned long long>(
                                g.rawCalls.load(std::memory_order_relaxed)),
                            static_cast<unsigned long long>(elapsedMs));
            } else if (ClockBindHorizonPassed(elapsedMs) &&
                       !g.clockHorizonLogged.exchange(true, std::memory_order_relaxed)) {
                log::Direct(LogLevel::Error, kReplayModuleName,
                            "CLOCK.GLOBAL_PTR is still null %llu ms and %llu dispatches into a "
                            "running frame loop: this build's global CClock is not where facts "
                            "records it, so simTimeUs and every frameDeltaUs stay 0 for this "
                            "process. Still re-asking, 1 dispatch in %llu",
                            static_cast<unsigned long long>(elapsedMs),
                            static_cast<unsigned long long>(nulls + 1),
                            static_cast<unsigned long long>(kClockReaskEveryNth));
            }
        }
    }
    const uint64_t simTimeUs =
        clock != nullptr ? *reinterpret_cast<const volatile uint64_t*>(clock + facts::CLOCK_ACCUM1_OFF)
                         : 0;
    AuditFrame(&g.frames, frame, simTimeUs);

    if (scaledDeltaUs == 0) {
        g.zeroDeltas.fetch_add(1, std::memory_order_relaxed);
    }
    g.deltaSumUs.fetch_add(scaledDeltaUs, std::memory_order_relaxed);

    TickInfo info;
    info.scaledDeltaUs = scaledDeltaUs;
    info.simTimeUs = simTimeUs;
    info.frameDeltaUs = g.frames.frameDeltaUs;
    // B.3 types this uint32_t; the engine's counter is a qword. Truncation is deliberate and harmless:
    // consumers compare consecutive values, and the difference survives the narrowing.
    info.frameIndex = static_cast<uint32_t>(frame);
    info.netGame = *SlotAt(game, facts::GAME_NETGAME_PTR_OFF);

    const uint32_t count = g.earlyCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i) {
        g.early[i].fn(info, g.early[i].ctx);
    }
    modreg::TickAll(info);
}

// ---------------------------------------------------------------------------------------------------
// The fallback: one MinHook detour on CR15NetGame::Update.
//
// ★ THE TRAP, and it is why this detour is not a two-line function. sub_1401BBDB0's second argument is
//   a STAGE INDEX 0..3, not a delta, and the call at 0x140127E3E sits outside the `if (a2 == 1)` body,
//   so it runs FOUR TIMES PER FRAME. Its own body branches on a2 == 1 and a2 == 3 and takes its timing
//   from the clock, never from a2. An existing loader on this machine types the argument `unsigned long
//   dt`. Filtering on the stage index is the entire difference between one tick per frame with the real
//   delta and four ticks per frame with a nonsense one.
// ---------------------------------------------------------------------------------------------------

using NetGameUpdateFn = char(__fastcall*)(void* netGame, uint64_t stage);
NetGameUpdateFn g_netGameUpdateOriginal = nullptr;

char __fastcall NetGameUpdateDetour(void* netGame, uint64_t stage) {
    if (IsTickStage(stage)) {
        uint8_t* clock = static_cast<uint8_t*>(g.clock.load(std::memory_order_acquire));
        const uint64_t delta =
            clock != nullptr
                ? *reinterpret_cast<const volatile uint64_t*>(clock + facts::CLOCK_SCALEDDELTA_OFF)
                : 0;
        Dispatch(delta);
    }
    return g_netGameUpdateOriginal(netGame, stage);
}

bool InstallFallback() {
    const HookStatus created = hookmgr::Create(
        "TICK.NETGAME_UPDATE", facts::NETGAME_UPDATE, facts::NETGAME_UPDATE_PROLOGUE,
        sizeof facts::NETGAME_UPDATE_PROLOGUE, reinterpret_cast<void*>(&NetGameUpdateDetour),
        reinterpret_cast<void**>(&g_netGameUpdateOriginal), "tick.mode");
    if (created != HookStatus::Created) {
        log::Direct(LogLevel::Error, kReplayModuleName, "fallback detour not created: %s",
                    HookStatusName(created));
        return false;
    }
    const HookStatus armed = hookmgr::Arm("TICK.NETGAME_UPDATE");
    if (armed != HookStatus::Armed) {
        log::Direct(LogLevel::Error, kReplayModuleName, "fallback detour not armed: %s",
                    HookStatusName(armed));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------
// The claim sequence, with every abort condition it names plus the deadline it omits.
// ---------------------------------------------------------------------------------------------------

// One poll step. False means stop waiting -- either the deadline passed or we are shutting down.
// GetTickCount64 rather than counting poll intervals: WaitForSingleObject may return early, and a
// deadline built from its nominal period would drift into a much longer wait than the one advertised.
bool Sleeping(uint64_t deadlineMs) {
    if (GetTickCount64() >= deadlineMs) {
        return false;
    }
    return WaitForSingleObject(g.stopEvent, kPollIntervalMs) == WAIT_TIMEOUT;
}

ClaimResult RunClaim(uint32_t* claimedOffset) {
    const uint64_t deadlineMs = GetTickCount64() + kClaimTimeoutMs;

    // 1. The CR15Game singleton, written once on entry to CR15Game::Initialize.
    uint8_t* game = nullptr;
    while ((game = GameSingleton()) == nullptr) {
        if (!Sleeping(deadlineMs)) {
            return ClaimResult::AbortNoGame;
        }
    }
    g.game.store(game, std::memory_order_release);

    // 2. pnsrad at +0x8508 is created unconditionally, so it filling is the "provider block finished"
    //    edge. If the block was skipped entirely this never fills, and the deadline is what turns that
    //    into a fallback instead of a hang.
    while (*SlotAt(game, facts::GAME_PROVIDER_SLOT_B) == nullptr) {
        if (!Sleeping(deadlineMs)) {
            return ClaimResult::AbortNoProviders;
        }
    }

    // 3. Belt and braces: the frame loop is genuinely running.
    //
    //    ★ The design waits for CClock+104 to "advance twice". That field is a per-frame DELTA, not a
    //    counter -- at a locked frame rate two consecutive frames legitimately carry the same value, and
    //    at the clamp it is pinned to a constant, so waiting for it to change can wait forever on a
    //    perfectly healthy frame loop. The render-frame counter is monotonic and is the thing we
    //    actually mean, so we wait on that instead.
    const uint64_t startFrame = ReadFrameCounter(game);
    while (ReadFrameCounter(game) - startFrame < kFramesBeforeClaim) {
        if (!Sleeping(deadlineMs)) {
            return ClaimResult::AbortNoFrames;
        }
    }

    uint8_t* clock = GlobalClock();
    if (clock != nullptr) {
        g.clock.store(clock, std::memory_order_release);
    }

    // 4. Probe. Never hard-coded: -noovr decides which of A and C exists, and our desktop viewer uses it.
    const SlotChoice choice = ChooseFreeSlot(*SlotAt(game, facts::GAME_PROVIDER_SLOT_A) != nullptr,
                                             *SlotAt(game, facts::GAME_PROVIDER_SLOT_C) != nullptr);
    const uint32_t offset = SlotChoiceOffset(choice);
    if (offset == 0) {
        log::Direct(LogLevel::Warn, kReplayModuleName, "slot probe found %s; not guessing",
                    SlotChoiceName(choice));
        return choice == SlotChoice::AbortBothFree ? ClaimResult::AbortBothFree
                                                   : ClaimResult::AbortBothTaken;
    }

    // 5. Pin, so the FreeLibrary at the end of CNSProvider::Destroy cannot unmap us mid-teardown. The
    //    address taken is our own exported Update, which is the pointer the engine will be calling.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(&Dispatch), &pinned) ||
        pinned == nullptr) {
        log::Direct(LogLevel::Error, kReplayModuleName, "could not pin this module (%lu); not claiming",
                    GetLastError());
        return ClaimResult::AbortRace;
    }
    g.self = pinned;

    // 6. Compare-and-swap, not a bare store: another plugin may be doing exactly this. The design's risk
    //    table asks for a compare-then-store with a read-back verify, and this is it.
    void** slot = SlotAt(game, offset);
    void* const previous = InterlockedCompareExchangePointer(slot, pinned, nullptr);
    if (previous != nullptr) {
        log::Direct(LogLevel::Warn, kReplayModuleName,
                    "slot +0x%X was claimed by someone else between the probe and the store", offset);
        return ClaimResult::AbortRace;
    }
    if (*reinterpret_cast<void* const volatile*>(slot) != pinned) {
        log::Direct(LogLevel::Warn, kReplayModuleName, "slot +0x%X did not hold our handle on read-back",
                    offset);
        return ClaimResult::AbortRace;
    }

    *claimedOffset = offset;
    return ClaimResult::Claimed;
}

void SetMode(const char* prefix, uint32_t offset) {
    // No snprintf on a path that a log drain may be reading; this is called once, before publication.
    char* out = g.modeName;
    const char* in = prefix;
    while (*in != '\0' && out < g.modeName + sizeof g.modeName - 8) {
        *out++ = *in++;
    }
    if (offset != 0) {
        static const char kHex[] = "0123456789ABCDEF";
        *out++ = '0';
        *out++ = 'x';
        bool leading = true;
        for (int shift = 28; shift >= 0; shift -= 4) {
            const char digit = kHex[(offset >> shift) & 0xFu];
            if (digit != '0' || !leading || shift == 0) {
                leading = false;
                *out++ = digit;
            }
        }
    }
    *out = '\0';
}

DWORD WINAPI ClaimMain(LPVOID) {
    const ModePlan plan = PlanMode();

    ClaimResult result = ClaimResult::AbortDisabled;
    uint32_t offset = 0;
    if (plan.slotAllowed) {
        result = RunClaim(&offset);
    }
    g.claim.store(static_cast<uint32_t>(result), std::memory_order_release);

    ClaimOutcome outcome{};
    outcome.slotClaimed = result == ClaimResult::Claimed;
    outcome.stopping = g.stopping.load(std::memory_order_acquire);
    if (!outcome.slotClaimed) {
        log::Direct(LogLevel::Warn, kReplayModuleName, "slot claim did not happen (%s)",
                    ClaimResultName(result));
        if (plan.hookAllowed && !outcome.stopping) {
            // The fallback needs the singleton and the clock even though it does not need a slot.
            if (g.game.load(std::memory_order_acquire) == nullptr) {
                if (uint8_t* game = GameSingleton()) {
                    g.game.store(game, std::memory_order_release);
                }
            }
            if (g.clock.load(std::memory_order_acquire) == nullptr) {
                if (uint8_t* clock = GlobalClock()) {
                    g.clock.store(clock, std::memory_order_release);
                }
            }
            outcome.gameResolved = g.game.load(std::memory_order_acquire) != nullptr;
            if (!outcome.gameResolved) {
                log::Direct(LogLevel::Error, kReplayModuleName,
                            "no CR15Game singleton, so neither implementation can tick");
            }
        }
    }

    switch (ResolveAndPublishMode(plan, outcome)) {
        case EffectiveMode::Slot:
            g.slotOffset.store(offset, std::memory_order_release);
            SetMode("slot:", offset);
            g.source.store(static_cast<uint32_t>(Source::Slot), std::memory_order_release);
            log::Direct(LogLevel::Info, kReplayModuleName,
                        "claimed the free CNSProvider slot at CR15Game+0x%X: one tick per frame, zero "
                        "hooks, zero patched bytes",
                        offset);
            break;
        case EffectiveMode::Hook:
            if (InstallFallback()) {
                SetMode("hook:", facts::NETGAME_UPDATE_RVA);
                g.source.store(static_cast<uint32_t>(Source::Hook), std::memory_order_release);
                log::Direct(
                    LogLevel::Info, kReplayModuleName,
                    "fallback tick live: one detour on CR15NetGame::Update, filtered to stage %llu of %llu",
                    static_cast<unsigned long long>(kTickStage),
                    static_cast<unsigned long long>(kEngineStagesPerFrame));
            }
            break;
        case EffectiveMode::None:
            break;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------
// ITick
// ---------------------------------------------------------------------------------------------------

class TickImpl final : public ITick {
public:
    bool RegisterEarly(TickFn fn, void* ctx) override {
        if (fn == nullptr) {
            return false;
        }
        const uint32_t index = g.earlyCount.load(std::memory_order_relaxed);
        if (index >= kMaxCallbacks) {
            log::Direct(LogLevel::Error, kReplayModuleName, "early tick table is full at %u", index);
            return false;
        }
        g.early[index].fn = fn;
        g.early[index].ctx = ctx;
        // Publish the entry before the count, so a tick in flight never reads a half-written slot.
        g.earlyCount.store(index + 1, std::memory_order_release);
        return true;
    }

    bool RegisterLate(TickFn, void*) override {
        // B.3: "RegisterLate may return false. IPosePublisher must then select a strategy that does not
        // need one, and log which." W1-D ships no late source, so this is the honest answer rather than
        // a silent early tick wearing a late tick's name.
        if (!g.lateRefusalLogged.exchange(true, std::memory_order_relaxed)) {
            log::Direct(LogLevel::Warn, kReplayModuleName,
                        "RegisterLate refused: W1-D ships no late tick source (S4.3.4's CTimerManager "
                        "route was not built). Callers must pick a strategy that does not need one");
        }
        return false;
    }

    const char* Mode() const override {
        return g.source.load(std::memory_order_acquire) == static_cast<uint32_t>(Source::None)
                   ? "none"
                   : g.modeName;
    }
};

TickImpl g_tick;

bool Init(ReplayServices* services) {
    REPLAY_REQUIRE(services, log);
    REPLAY_REQUIRE(services, config);

    g.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g.stopEvent == nullptr) {
        log::Direct(LogLevel::Error, kReplayModuleName, "CreateEvent failed (%lu)", GetLastError());
        return false;
    }

    // The claim waits on the engine for as long as it takes to reach a running frame loop, so it cannot
    // run on the worker: the worker drains the log, and a module that blocks it blocks every diagnostic
    // in the process.
    g.claimThread = CreateThread(nullptr, 0, ClaimMain, nullptr, 0, nullptr);
    if (g.claimThread == nullptr) {
        log::Direct(LogLevel::Error, kReplayModuleName, "CreateThread failed (%lu)", GetLastError());
        return false;
    }

    services->tick = &g_tick;
    return true;
}

void StopClaimThread() {
    if (g.stopEvent != nullptr) {
        SetEvent(g.stopEvent);
    }
    if (g.claimThread != nullptr) {
        // Bounded: the claim thread only ever waits on stopEvent, so this returns promptly. It is never
        // called under the loader lock -- RadPluginShutdown runs from CR15Game::Shutdown, and module
        // shutdown runs from the worker.
        WaitForSingleObject(g.claimThread, 5000);
        CloseHandle(g.claimThread);
        g.claimThread = nullptr;
    }
}

void ReleaseSlot() {
    const uint32_t offset = g.slotOffset.load(std::memory_order_acquire);
    uint8_t* game = static_cast<uint8_t*>(g.game.load(std::memory_order_acquire));
    if (offset == 0 || game == nullptr || g.self == nullptr) {
        return;
    }
    // Only if it is still ours. CR15Game::Shutdown NULLs all three slots itself right after calling
    // Destroy, so on the engine's own teardown this is usually a no-op -- it matters when the module is
    // shut down while the game keeps running.
    InterlockedCompareExchangePointer(SlotAt(game, offset), nullptr, g.self);
    g.slotOffset.store(0, std::memory_order_release);
}

void Shutdown() {
    g.stopping.store(true, std::memory_order_release);
    StopClaimThread();
    ReleaseSlot();
}

// The capture worker's shutdown close is drain, close, re-open, verify and three sentinels; its own
// module gave that 2000 ms.
constexpr uint32_t kPreExitBudgetMs = 2000;

void OnEngineShutdown() {
    const uint64_t start = GetTickCount64();
    modreg::RunPreExit(kPreExitBudgetMs);
    Shutdown();
    log::Direct(LogLevel::Info, kReplayModuleName,
                "RadPluginShutdown: pre-exit hooks and tick shutdown took %llu ms",
                static_cast<unsigned long long>(GetTickCount64() - start));
}

}  // namespace

const char* SourceName(Source source) {
    switch (source) {
        case Source::None: return "none";
        case Source::Slot: return "slot";
        case Source::Hook: return "hook";
    }
    return "?";
}

const char* ClaimResultName(ClaimResult result) {
    switch (result) {
        case ClaimResult::Pending:          return "pending";
        case ClaimResult::Claimed:          return "claimed";
        case ClaimResult::AbortNoGame:      return "abort:no-cr15game-singleton";
        case ClaimResult::AbortNoProviders: return "abort:no-providers-created";
        case ClaimResult::AbortNoFrames:    return "abort:frame-loop-not-running";
        case ClaimResult::AbortBothFree:    return "abort:both-slots-free";
        case ClaimResult::AbortBothTaken:   return "abort:both-slots-taken";
        case ClaimResult::AbortRace:        return "abort:lost-the-race";
        case ClaimResult::AbortDisabled:    return "abort:disabled-by-config";
    }
    return "?";
}

void GetStats(Stats* out) {
    if (out == nullptr) {
        return;
    }
    out->ticks = g.frames.ticks;
    out->rawCalls = g.rawCalls.load(std::memory_order_relaxed);
    out->firstFrameIndex = g.frames.first;
    out->lastFrameIndex = g.frames.last;
    out->duplicateFrames = g.frames.duplicates;
    out->skippedFrames = g.frames.skips;
    out->zeroDeltas = g.zeroDeltas.load(std::memory_order_relaxed);
    out->simRegressions = g.frames.simRegressions;
    out->deltaSumUs = g.deltaSumUs.load(std::memory_order_relaxed);
    out->source = static_cast<Source>(g.source.load(std::memory_order_acquire));
    out->claim = static_cast<ClaimResult>(g.claim.load(std::memory_order_acquire));
    out->slotOffset = g.slotOffset.load(std::memory_order_acquire);
}

bool ReadEngineClock(uint64_t* scaledDeltaUs, uint64_t* simTimeUs, uint64_t* frameIndex) {
    uint8_t* clock = GlobalClock();
    uint8_t* game = GameSingleton();
    if (clock == nullptr || game == nullptr) {
        return false;
    }
    if (scaledDeltaUs != nullptr) {
        *scaledDeltaUs = *reinterpret_cast<const volatile uint64_t*>(clock + facts::CLOCK_SCALEDDELTA_OFF);
    }
    if (simTimeUs != nullptr) {
        *simTimeUs = *reinterpret_cast<const volatile uint64_t*>(clock + facts::CLOCK_ACCUM1_OFF);
    }
    if (frameIndex != nullptr) {
        *frameIndex = ReadFrameCounter(game);
    }
    return true;
}

}  // namespace tick
}  // namespace replay

// ---------------------------------------------------------------------------------------------------
// ★ THE ENTIRE EXPORT SURFACE OF THIS DLL. Two names, both marked here and nowhere else.
//
// tests/exports pins the built export table against tests/exports/allowed.txt, and S2 F8b is why the
// list is exactly two: a no-op ProviderID returning 0 is indistinguishable from a valid provider id and
// would make us a plausible provider to any caller that reached us, whereas a MISSING method is
// tolerated -- CNSProvider::GetMethod logs "Method '%s' is unimplemented for net service provider '%s'"
// (cnsprovider.cpp:56) with our module's full path and returns without calling. Exporting nothing else
// makes a mistaken lookup loud instead of silently wrong, and that log line is W1-D's canary.
// ---------------------------------------------------------------------------------------------------

// CNSProvider::Update (sub_1406181E0) reads the global CClock's +0x68 into rbx, resolves "Update"
// against our HMODULE, and TAIL-JUMPS to it with rcx = that delta. One integer argument, and the
// return value is discarded by CR15Game::Update.
extern "C" __declspec(dllexport) void __fastcall Update(uint64_t scaledDeltaUs) {
    replay::tick::Dispatch(scaledDeltaUs);
}

// CNSProvider::Destroy (sub_140607190 -> sub_14105AE30) resolves this with a plain GetProcAddress, calls
// it with NO arguments, and passes the int result to the engine's error reporter -- which treats 0 and
// -1 as "no error" and raises an assert for anything else. So this returns 0, and that is a constraint
// on the value, not a convention.
//
// It is also the only teardown the plugin gets while it may still wait on its own threads: by
// DLL_PROCESS_DETACH they have been terminated and the loader lock is held. Reached only when the
// slot claim succeeded -- the hook fallback has no Destroy call, and the log line below is how a
// launch tells the two apart.
extern "C" __declspec(dllexport) int RadPluginShutdown(void) {
    replay::tick::OnEngineShutdown();
    return 0;
}

REPLAY_MODULE("tick", 10, replay::tick::Init, nullptr, replay::tick::Shutdown);
