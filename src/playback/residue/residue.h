#pragma once

// W3-A. The engine-facing half of the design's category-N battery: the primitives that undo
// what the engine is left holding after the playhead jumps.
//
// ---------------------------------------------------------------------------------------------
// ★★★ THERE IS NO SEEK PATH HERE EITHER.
//
// W2-B's rule binds this packet exactly as it binds W2-H's: one ApplyStateAt serves play, scrub,
// reverse and fast-forward, with no channel through which it could learn a jump happened. So none
// of what follows is reachable from an `if (seeking)` branch. Every primitive is an IResidue that
// subscribes to the discontinuity, and the battery -- not this file -- decides when they run.
//
// ---------------------------------------------------------------------------------------------
// ★★★ WHY THERE ARE EIGHT ROWS FOR FOUR DESIGN ROWS, AND IT IS THE SAME LESSON TWICE.
//
// W2-H found a mute whose ACQUIRE and RELEASE lived on two battery rows with two readiness
// conditions, so the mute was taken by a ready row and released by a skipped one and the session
// went silent with nothing reporting a failure. The lesson it wrote down is the one that shaped
// this file: *an availability enum cannot express "half of this works", and gating a whole row on
// its optional half is how the load-bearing half gets skipped.*
//
// Three rows of S7.4.3 are half-and-half, so each is split at the seam where the two halves can
// become independently unavailable:
//
//   N-3  is TWO functions. ResetCI and ResetWarpFrame are separate addresses with separate guards.
//        One row would have made a failed guard on either silence both.
//
//   N-7  is a CALL plus a STORE. The call (BeAwake_) can fail a prologue guard; the store (the
//        motion bit) cannot fail at all, and on its own it still buys the wake, because the engine's
//        own classification pass reads that bit and takes its wake arm. Gating the store on the
//        call's guard would have discarded the half that always works.
//
//   N-8  is a hysteresis clear plus a manifold clear. The first is fully reproducible from mapped
//        facts (W1-I S6.1). The second has no PC counterpart and its cited symbol is refused on
//        role. One row would have reported the whole of N-8 as unavailable and skipped a clear that
//        works -- or, worse, reported it as available and implied a manifold reset that never runs.
//
// ---------------------------------------------------------------------------------------------
// ★★ WHAT THIS PACKET DOES NOT SERVE, said here because a reader of this header is entitled to it.
//
// N-5, N-9, N-10, N-12, N-13 and N-19 are NOT registered by this packet, and W2-B's own placeholder
// rows continue to report them. Each reason is in docs/spikes/W3-A.md, and none of them is "we ran
// out of time": three are blocked on an enumeration path that does not exist, one is an unbounded
// set of fields rather than a call, one is half of an unpaired acquire, and one is low-value.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include "playback/core/discontinuity.h"
#include "playback/residue/residuesite.h"

namespace replay {

class ILog;
class IEntity;
struct ReplayServices;

namespace playback {
namespace residue {

// The engine entry points this packet calls, and the two site resolvers.
//
// ★ THE ONE THING AN OFFLINE TEST CANNOT HAVE, SUBSTITUTED -- AND NOTHING ELSE. Every field below
// is a boundary to the running game; everything on the other side of the boundary (which entities
// to walk, which bodies to filter out, what to write, in what order) is the same code in a test and
// in the product. src/audio/voip/voipsite.h states the rule this follows.
struct ResidueEnv {
    // CR15NetPhysicsCS::ResetCI / ::ResetWarpFrame / CR15RemotePlayerCS::ResetGrabPosHandFixups.
    // All three are (component system, handle) and return a pointer this packet ignores.
    typedef uint64_t (*CsHandleFn)(void* cs, uint16_t handle);
    // CR15NetPhysicsCS::SnapTimeOffset -- the float rides in xmm2 by position.
    typedef uint64_t (*CsSnapFn)(void* cs, uint16_t handle, float seconds);
    // CPhBody::BeAwake_(bResetSleepFrame, bWakeUpContacts).
    typedef uint64_t (*BodyWakeFn)(void* body, uint32_t resetSleepFrame, uint32_t wakeContacts);

    CsSite    (*resolveCs)()    = nullptr;
    PhysSite  (*resolvePhys)()  = nullptr;
    WorldSite (*resolveWorld)() = nullptr;
    ClockSite (*resolveClock)() = nullptr;

    // Borrowed, for the N-3a ordering check ONLY -- it reads the history ring's newest sample time
    // and writes nothing. Null is a reported state, not a silent skip.
    IEntity* entity = nullptr;

    CsHandleFn resetCi          = nullptr;   // N-3a
    CsHandleFn resetWarpFrame   = nullptr;   // N-3b
    CsSnapFn   snapTimeOffset   = nullptr;   // N-4
    CsHandleFn clearGrabFixups  = nullptr;   // N-2
    BodyWakeFn wake             = nullptr;   // N-7b

    ILog* log = nullptr;
};

// What the last fire actually did. Every count is of an EFFECT, not of a call: W2-B paid for a
// netvar counter that incremented on the attempt while the write unconditionally refused, and the
// only observable this packet has must not repeat it.
struct ResidueStats {
    // ★ Whether anybody armed this packet at all. Separate from every count below, because a zero
    // count answers "armed and nothing happened" and "never armed" identically -- which is the
    // false all-clear the whole battery structure exists to make unrepresentable.
    bool armed = false;

    Bind lastCsBind   = Bind::NoGame;
    Bind lastPhysBind = Bind::NoPhysSystem;

    uint64_t ciResets = 0;
    uint64_t warpRequests = 0;
    uint64_t timeSnaps = 0;
    uint64_t grabClears = 0;
    uint64_t motionBitsRaised = 0;
    uint64_t wakesCalled = 0;
    uint64_t hysteresisCleared = 0;
    // ★ No reoriginRequestsCleared counter: this packet writes nothing to the re-origin flag word.
    // See residue_facts.h -- 0x100/0x200 are one two-phase state and clearing 0x200 cancels a
    // translation that has already begun.
    uint64_t worldOriginShifts = 0;   // a shift that happened ANYWAY: a finding, not a repair
    uint64_t clockWrapsRepaired = 0;
    uint64_t clockBackwardsSeen = 0;  // reported, never repaired -- W2-D owns clock writes

    // ★★ THE RAISE COUNT. Every other number on this struct is an EFFECT count, and that is
    // exactly why a reader cannot use them for this: a zero effect-sum reads the same for "the
    // battery never fired" and "it fired and every row resolved nothing to act on". W2-R's residue
    // row could only answer could-not-ask over that pair. This counts DISCONTINUITIES DELIVERED, so
    // the two come apart and the row becomes "exactly one fire per commanded discontinuity".
    //
    // ★ ITS WITNESS IS N-18, AND THE WITNESS PROPERTY IS ENFORCED RATHER THAN ASSUMED. The count
    // has to sit on the one row the battery can never skip -- always Ready, RunsOn every Cause --
    // and N-18 is that row. tests/residue asserts both of those properties of N-18 directly, so a
    // later change that made it skippable reds there instead of silently stopping this counter.
    uint64_t fires = 0;
    // Per cause, because "a thousand micro-seeks and one seek" and "a thousand and one seeks" are
    // different sessions and the total cannot tell them apart. A cause outside the enum is counted
    // in `fires` and in no bucket, rather than written past the end.
    uint64_t firesByCause[static_cast<size_t>(Cause::Count)] = {};
    // ★ Count, not Seek, as the initial value: Seek is 0, which would make "nothing has fired
    // yet" indistinguishable from "the last fire was a seek".
    Cause lastFireCause = Cause::Count;

    ClockVerdict lastClockVerdict = ClockVerdict::Fresh;

    // ★★★ THE ORDERING INVARIANT, MEASURED RATHER THAN COMMENTED. N-3a recycles the history ring
    // ApplyStateAt fills, so it is correct only BEFORE the apply. This is what that check found.
    Ordering ordering = Ordering::NotChecked;
    // Non-zero means N-3a REFUSED to run, not merely that it complained: a reset that would have
    // emptied a ring the apply had already filled is skipped rather than logged and performed.
    uint64_t orderingViolations = 0;

    // ★ The denominator. A fire that resolved zero entities and a fire that resolved twelve and
    // changed none of them are different failures, and a single "we ran" flag renders them alike.
    BodyWalk lastWalk;
};

const ResidueStats& Stats();
void ResetStatsForTest();

// Registers this packet's rows into the battery and returns how many were accepted.
//
// ★★ EVERY ROW REGISTERS, INCLUDING THE ONES THAT CANNOT RUN. A row whose bind is missing registers
// as Unmapped with the reason, because the battery's UnavailableIds() is what the panel shows the
// owner, and a row that declined to register would make "we could not map it" and "it is not needed"
// indistinguishable. That is the defect this whole structure was built after.
// ★ THE BATTERY IS BORROWED AND MUST OUTLIVE THIS CALL. A pointer to it is kept so Roster() can
// report the design's list against what is actually registered, so a battery with a shorter
// lifetime than the process leaves a dangling read behind. W2-B's is a member of a file-static
// Global, which satisfies this; a caller whose battery is a local must call ResetStatsForTest()
// before it goes out of scope.
size_t RegisterPlaybackResidue(ResidueBattery&, const ResidueEnv&);

// The production seam: resolves every entry point in ResidueEnv through buildguard, then registers.
// Kept separate from RegisterPlaybackResidue so the offline half of this packet is reachable without
// a game, and named to match audio::ArmAudioResidue, which W2-B must call in the same place.
size_t ArmPlaybackResidue(ResidueBattery&, ILog*);

// The services this packet needs at ARM time, handed over before arming happens.
//
// ★ WHY THIS EXISTS RATHER THAN A THIRD PARAMETER ON ArmPlaybackResidue. W2-B is editing
// playback_module.cpp right now against the two-parameter signature above; widening it mid-flight
// would break their build for a dependency that is optional to every row. So the module in this
// directory registers at order 290 -- one BEFORE playback's 300 -- and stashes the services, which
// are therefore already here when W2-B calls the seam during its own Init.
//
// The only thing taken from them is IEntity, and the only thing IEntity is used for is reading the
// history ring's newest sample time to judge the N-3a ordering invariant. Nothing is written
// through it. A null leaves that check reporting Ordering::NoEntity, which is a state, not a pass.
void SetArmServices(ReplayServices*);

// ---------------------------------------------------------------------------------------------
// ★★★ THE DENOMINATOR, AND WHERE IT COMES FROM.
//
// UnavailableCount() is taken over what REGISTERED, so a residue nobody implemented cannot appear as
// missing -- it is simply not counted. Thirteen registered rows with thirteen reported unavailable
// read as "we know about all of it" while six of the design's nineteen were invisible. A
// completeness metric whose denominator is supplied by the work being measured cannot fail, and this
// one was reporting hardest at the moment it should have been loudest.
//
// So the denominator below is the DESIGN'S list, held as a static table of all nineteen active rows,
// and the numerator is looked up in the live battery. A row nothing registers for reports
// Unregistered -- a state distinct from both Unavailable and Ready -- and the four this packet
// deliberately does not serve stay visible with the reason attached.
//
// tests/residue cross-checks this table against the design itself, so the design gaining an N-21
// fails the gate rather than silently shrinking the denominator.
// ---------------------------------------------------------------------------------------------

enum class Coverage : uint8_t {
    Ready = 0,       // every registered row covering this design row is Ready
    Partial,         // some are Ready and some are not -- N-8a works, N-8b does not
    Unavailable,     // registered, and no covering row can run
    Unregistered,    // ★ NOTHING registered for it. Not a degraded Unavailable: nobody built it
    Count
};

const char* CoverageText(Coverage);

struct RosterRow {
    const char* id;      // "N-2" .. "N-20", as the design numbers them
    const char* what;    // one line, in the design's own terms
    const char* note;    // why it is not served; "" when it is
    Coverage coverage = Coverage::Unregistered;
    size_t registered = 0;    // how many battery rows cover it
};

// The design has 19 active rows (N-1 was reclassified to category I). Always returns 19;
// `cap` bounds only how many are written.
static constexpr size_t kRosterRows = 19;
size_t Roster(RosterRow* out, size_t cap);

struct RosterSummary {
    size_t known = kRosterRows;
    size_t ready = 0;
    size_t partial = 0;
    size_t unavailable = 0;
    size_t unregistered = 0;
    bool consulted = false;   // false => no battery was ever handed to us; the counts mean nothing
};

RosterSummary Summary();

}  // namespace residue
}  // namespace playback
}  // namespace replay
