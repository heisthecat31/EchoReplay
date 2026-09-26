#pragma once

// The two decisions W1-D can be wrong about without the engine noticing, split out so they can be
// driven from a unit test that needs no game, no Windows and no MinHook.
//
// Both are shipped code, not a copy of it: tick.cpp calls exactly these functions. A test against a
// reimplementation would prove only that the test agrees with itself.

#include <atomic>
#include <stdint.h>

namespace replay {
namespace tick {

// ---------------------------------------------------------------------------------------------------
// 1. WHICH SLOT IS FREE.
//
// `-noovr` decides whether pnsovr (into +0x8500) or pnsdemo (into +0x8510) is created; pnsrad (+0x8508)
// is created either way. So exactly one of A and C is null, and WHICH ONE depends on the command line.
// Our own desktop viewer passes -noovr, so both answers occur in normal use and neither may be assumed.
//
// Anything other than exactly-one-free is a shape we did not verify against the binary, and the design
// is explicit that we abort rather than guess.
// ---------------------------------------------------------------------------------------------------
enum class SlotChoice : uint8_t {
    ClaimA,          // +0x8500 free: pnsdemo took C, so this build was launched with -noovr
    ClaimC,          // +0x8510 free: pnsovr took A, so this build was launched without it
    AbortBothFree,   // no provider was created at all
    AbortBothTaken,  // three of three occupied: another plugin got here, or the model is wrong
};

SlotChoice  ChooseFreeSlot(bool slotAOccupied, bool slotCOccupied);
const char* SlotChoiceName(SlotChoice);

// The CR15Game offset a choice resolves to, or 0 for an abort.
uint32_t SlotChoiceOffset(SlotChoice);

// ---------------------------------------------------------------------------------------------------
// 2. THE STAGE-INDEX FILTER, which is the one that has already bitten someone.
//
// The fallback detour sits on CR15NetGame::Update (sub_1401BBDB0). Its second argument is a STAGE INDEX
// 0..3, not a delta, and the call site at 0x140127E3E is outside the `if (a2 == 1)` body, so the engine
// runs it FOUR TIMES PER FRAME. An existing loader on this machine declares that parameter
// `unsigned long dt`. Without this filter the fallback ticks four times a frame and hands every consumer
// a stage index where a microsecond delta belongs.
// ---------------------------------------------------------------------------------------------------
constexpr uint64_t kEngineStagesPerFrame = 4;
constexpr uint64_t kTickStage = 0;  // matches where the slot claim ticks: ahead of all simulation

inline bool IsTickStage(uint64_t stage) { return stage == kTickStage; }

// ---------------------------------------------------------------------------------------------------
// 4. THE COMMAND-LINE DERIVATION, so a probe can predict the answer the way the engine does.
//
// The engine decides which provider it creates from bit 27 of CR15Game+0x7AE0, set by exactly one
// instruction, guarded by a search for the literal "-noovr". A probe that derives the same prediction
// can ASSERT the slot it claimed matched, instead of merely reporting which slot it got - and it stays
// correct on a profile nobody has written yet.
//
// Token match, not substring: "-noovrfoo" is a different switch and "x-noovr" is not one at all. This
// mirrors the engine's search rather than reproducing its argument normalisation exactly, which is why
// the probe prints the command line it judged.
bool CommandLineHasFlag(const char* commandLine, const char* flag);

// ---------------------------------------------------------------------------------------------------
// 5. FRAME CONTINUITY -- the acceptance criterion's arithmetic.
// ---------------------------------------------------------------------------------------------------
struct FrameAudit {
    uint64_t ticks;
    uint64_t duplicates;
    uint64_t skips;
    uint64_t first;
    uint64_t last;
    // ★ CClock+0x28 accumulates every heartbeat, and there are at least three per frame, so its
    // advance between two consecutive stage-0 ticks is exactly one frame of scaled sim time -- which
    // the value CNSProvider::Update hands us is NOT. This is where the real frame delta comes from.
    uint64_t lastSimTimeUs;
    uint64_t frameDeltaUs;    // advance since the previous tick; 0 on the first
    uint64_t simRegressions;  // sim time went backwards. Monotonic by construction, so must stay 0
    bool     started;
};

void AuditFrame(FrameAudit* audit, uint64_t frameCounter, uint64_t simTimeUs);

// ---------------------------------------------------------------------------------------------------
// 6. THE MODE RESOLUTION -- which implementation is actually ticking, and the publish that says so.
//
// ClaimMain is tick.mode's provider: the file asks for auto | slot | hook and the claim's outcome
// decides what that resolves to. Every later reader -- hookmgr's declaration gate, SC7's expected-row
// denominator, the startup log -- must be served the mode that is RUNNING, not the word that was asked
// for, so the resolution ends in a publish rather than in a local variable.
//
// It lives here rather than in tick.cpp because none of it touches the engine, and because the version
// that lived in tick.cpp could not be mutated by any offline test: tick.cpp is compiled only into the
// launch probes.
// ---------------------------------------------------------------------------------------------------
enum class EffectiveMode : uint8_t {
    Slot,  // the free-slot claim carried it: zero hooks, and ledger row 1 stays undeclared
    Hook,  // the fallback detour, and the only resolution that DECLARES ledger row 1
    None,  // nothing ticks, so there is no effective mode and publishing one would be a lie
};

// "slot" / "hook" -- the words S4.4 declares for the key, and the words that get published.
const char* EffectiveModeName(EffectiveMode);

// What the configuration ASKED for, and what that permits.
struct ModePlan {
    const char* requested;  // never null; "auto" when the key is undeclarable
    bool slotAllowed;       // run the slot claim at all
    bool hookAllowed;       // the fallback detour may be installed
};

// Reads the request through config::RequestedString, NEVER through GetString: this thread is the row's
// provider, and an ordinary read closes the publish window before there is anything to publish -- both
// resolutions below are then refused and the packet ships doing nothing.
ModePlan PlanMode();

// The three facts about the run that the resolution depends on. Everything engine-shaped -- the poll
// loop, the singleton lookup, CreateThread -- stays in tick.cpp and arrives here as a bool.
struct ClaimOutcome {
    bool slotClaimed;    // RunClaim returned Claimed
    bool stopping;       // shutdown was asked for while the claim ran
    bool gameResolved;   // the CR15Game singleton is available; read only on the fallback path
};

// Decides the mode and publishes it. Returns what it published; None publishes nothing.
//
// The publish must happen BEFORE the caller acts on Hook, because hookmgr's declaration gate runs
// inside Create -- an unpublished row 1 is refused there and the fallback never installs.
EffectiveMode ResolveAndPublishMode(const ModePlan&, const ClaimOutcome&);


// ---------------------------------------------------------------------------------------------------
// 7. THE CLOCK RE-ASK, and what it is allowed to cost.
//
// The re-ask in tick.cpp's `clock == nullptr` arm walked the mapped PE headers on EVERY frame until the
// engine's global CClock bound: buildguard::MappedAddress calls Readable twice, and Readable loops on
// VirtualQuery -- NtQueryVirtualMemory, which takes the target address space's lock and walks its VAD
// tree. Two of those per frame, forever, on the path where the clock never binds at all. AUD-P17
// finding 3, and the same defect class ENG-COST took off the capture tick.
//
// Two pieces, both here so they can be driven with no engine, no Windows and no MinHook in the process:
//
//   * the CELL is image geometry, so it is resolved ONCE and read per tick (ENG-COST S2's slot
//     pattern), and
//   * the re-ask BACKS OFF -- every dispatch for the first second, which is the window a bind can
//     plausibly land in and the window the late-bind line has to stay frame-accurate over, then one
//     dispatch in kClockReaskEveryNth. It never stops: a backoff that stopped would be a silent "no
//     clock for the rest of this process", which is the state the re-ask exists to escape.
//
// A cell that fails to resolve is deliberately NOT cached. "Not mapped when I asked" is not an answer
// about the process, and caching it would make ClockBindHorizonPassed below unreachable.
// ---------------------------------------------------------------------------------------------------
class ImageCellCache {
  public:
    // Returns null when the cell is not resolvable yet; the caller's backoff decides when to re-ask.
    using Resolver = const void* (*)(void* ctx);

    const void* Get(Resolver resolve, void* ctx);
    bool Resolved() const { return cell_.load(std::memory_order_acquire) != nullptr; }

  private:
    std::atomic<const void*> cell_{nullptr};
};

constexpr uint64_t kClockReaskWarmMs = 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF one second in ms, not that slot
constexpr uint64_t kClockReaskEveryNth = 256;
// A live frame loop with no global CClock after this long is not a race we lost, it is a build whose
// CLOCK.GLOBAL_PTR does not mean what facts_generated.h records. It gets a named line, once, not
// silence and not a counter nobody reads.
constexpr uint64_t kClockBindHorizonMs = 30000;

// nullDispatches is how many dispatches have ALREADY found a null clock, so it is 0 on the first one.
bool ShouldReaskClock(uint64_t nullDispatches, uint64_t elapsedMs);
bool ClockBindHorizonPassed(uint64_t elapsedMs);

}  // namespace tick
}  // namespace replay
