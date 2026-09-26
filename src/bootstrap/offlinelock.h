// Layer 4 of the machinery enforcing the owner's binding "no server" constraint: the offline mode bit
// must not become clearable behind a menu.
//
// WHAT IT PROTECTS. `-offline` sets SESSION_OFFLINE_FLAG_BIT in the SessionFlags qword at
// CR15Game + SESSION_OFFLINE_FLAG_OFF. sub_140157FB0's online-provider block is gated on that bit being
// clear, so under -offline the MatchmakingLib provider is never instantiated, `lobby + 40` stays null,
// and both session-request builders null-guard that pointer before their only dispatch. Every
// create/join request therefore logs its intent and returns without sending. That is the whole network
// safety property, and it rests on one bit.
//
// ★ THE HAZARD, and it is plural. The session-teardown routine clears the bit with
// `and qword [reg+SESSION_OFFLINE_FLAG_OFF], <mask without the bit>`. It has seven callers and only two
// of them put the bit back: the image contains exactly two `or [reg+OFF], BIT` instructions and both
// live inside those two callers. The other five - one of them reachable from a menu script node - leave
// the bit clear, and the next pass through the provider loader then instantiates matchmaking for real.
//
// ★ WHY A HOOK AND NOT A BYTE PATCH. Neutralising the two clearing writes was the first proposal. It is
// refused three ways by the design: constraint P2 ("no byte patch at all", L1), S4.5.3 rule 4 ("we install
// no raw byte patches", which traces to the owner's Q7 "coexist, never replace"), and the explicitly
// refused "-offline patch at 0xFDE0E". Strategy PP-D - NOP a test of this same flag word - was already
// adjudicated and refused by the same rule. There is an empirical half too: code-section writes tripped
// Behavior:Win32/DefenseEvasion.A!ml where plugin DLLs were 14/14 clean. MinHook trampolines chain; a
// raw patch does not.
// (ENG-OFFLINE-PATCH amends the -offline refusal for replay sessions only, on the owner's request:
// bootstrap/offlinepatch.h. This lock is unchanged by it and still holds the bit in those sessions.)
//
// ★ WHY IT CANNOT AFFECT NORMAL ONLINE PLAY. The detour restores the bit only if it was SET when the
// teardown was entered. A session that never entered offline mode never has the bit, so the detour
// observes nothing to restore and changes nothing. The lock is inert outside the case it exists for.
//
// ★ THE INSTRUMENT IS INTRINSIC. The detour is the only thing that can report on itself, so it counts
// what it saw rather than leaving a separate sampler to infer it. `calls == 0` is "never exercised";
// calls with a prevented clear are "held under exercise"; a clear we failed to reverse is a failure.
// Those are three different facts and the harness must never read the first as the second.
//
// ★ WHAT ELSE READS THIS BIT, AND WHAT CHANGES WHEN IT NEVER CLEARS.
//
// The bit is not read through CR15Game+OFF by the consumers that matter. GAME.INITIALIZE takes its
// address (`lea r12,[rsi+8A70h]`) and hands it to the CR15NetGame constructor, which stores it at
// netgame+0x2DA0 - so the reader set is reached by a second, much larger family than a scan for the
// direct displacement finds. They share one inlined predicate:
//
//     mov rax,[netgame+0x2DA0]; mov rcx,[rax]      // the SessionFlags qword
//     bit 0x02 set                       -> AUTHORITY
//     bit 0x04 set                       -> not authority
//     bit 0x40 set (and 0x04 clear)      -> AUTHORITY
//
// which is exactly the recorded host predicate, `host = bit1 || ((f & 0x44) == 0x40)`. 252 sites in at
// least 216 functions carry it, from 0x140145B30 to 0x140DE6660. That count is a LOWER bound: it is
// attributed through .pdata, and leaf functions have no unwind record - NETGAME.PEER_TIME_BY_USER is
// itself one, and is missing from the tally for that reason alone.
//
// So the bit has ONE meaning everywhere - "this client is the authority" - and a permanent bit pins
// every one of those sites to the authority arm. Per reader, with its consumer:
//
//   NETGAME.PEER_TIME_BY_USER (0x14018AF00). Returns netgame+0x2DA8, the local time, instead of
//       netgame+NETGAME.SERVERTIME_OFF_PC, a server-time estimate. Consumed by NET.UPDATE_INTERP for
//       both pose times and written to PHYS.CI_TARGET_TIME_OFF. In a session with no server the
//       estimate is the wrong clock, so holding the bit holds the right one.
//   NET.UPDATE_INTERP (0x140D7DA20). Takes the authority arm at 0x140D7DBE4, so
//       ENTITY.INTERPOLATE_PLAYER_STATE never runs - which is what the design already states for an
//       offline replay.
//   the netvar release gate (0x140D048D0). Skips to 0x140D04B5F, past the lock cmpxchg release block.
//   the provider load (sub_140157FB0). The gate this lock exists for: no MatchmakingLib, lobby+40 null.
//
// ★ THE DIRECTION MATTERS AND IT IS THE SAFE ONE. In a replay the bit is ALREADY set - the -offline
// path ORs it - so every reader above is already on the authority arm and the lock changes nothing
// about a session that behaves. What it prevents is a teardown flipping all 252 sites to the
// non-authority arm mid-session. The design's PP-C warned about these same readers, but PP-C proposed
// CLEARING the bit; this is the opposite direction.
//
// ★★ AND IT CLOSES A SILENT-FREEZE PATH RATHER THAN OPENING ONE. W2-B traced the synthetic owner
// W1-E ships to defeat IsLocallyControlled: its low word exceeds PeerTime's 16-slot bound, so PeerTime
// returns 0, and a sample time of zero makes the interpolator walk past every history node and clamp -
// every replayed body frozen at its oldest sample, with no crash and no log line. That chain needs the
// PeerTime CALL to happen. In NET.UPDATE_INTERP the call is at 0x140D7DAB6 and the bit-0x40 test is at
// 0x140D7DA93, branching AWAY to 0x140D7DBE4 - so while the offline bit holds, the call is never
// reached and the freeze is unreachable. Clearing the bit is what arms it.
//
// RESIDUAL WINDOW, stated rather than buried. Between the original's AND and our restore there is a
// window of roughly ten instructions. Both the teardown and the provider load appear to be main-thread,
// so we do not believe another thread can observe the cleared bit - but single-threadedness is not
// proven. The proper close is guarding the CONSUMER (sub_140157FB0) instead of the state; that is a
// follow-up, deliberately not done here.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace replay {
namespace offlinelock {

enum class LockState : uint8_t {
    NotAttempted,
    Locked,
    // ★ NOT a failure, and it must not share an answer with one. Every probe DLL links this library, so
    // a multi-probe launch runs Apply() once per module and only the first can hold the trampoline.
    // MinHook refuses the duplicates, and a module that reported RefusedByMinHook for that reason would
    // be describing a process that IS locked as one that is not. The instrument lives in shared memory
    // precisely so a module that did not install the hook can still report what the hook observed.
    AlreadyLockedInProcess,
    RefusedByConfig,
    RefusedNoImage,          // the main module's .text could not be located
    RefusedNoSites,          // nothing in .text clears the bit: not the build we measured
    RefusedSiteSpread,       // the clearing sites are not all in one function
    RefusedSelfTest,         // the located bytes did not behave as the design says they do
    RefusedByMinHook,
};

const char* LockStateName(LockState state);

// What the run actually demonstrated. Kept apart from LockState on purpose: "armed and never needed" and
// "armed and did its job" are different facts, and a quiet run must not be read as the second.
enum class Exercise : uint8_t {
    NotExercised,        // the teardown never ran; the lock is armed and unproven in this run
    RanWithNothingToHold,// it ran, but the bit was already clear on entry every time
    HeldUnderExercise,   // it ran, tried to clear a set bit, and we put the bit back
    Failed,              // the bit was clear after a call and we could not restore it
};

const char* ExerciseName(Exercise state);

// One instruction that clears the bit, as located in the mapped image.
struct Site {
    uint64_t va;
    uint8_t  mask;      // the AND immediate, low byte
    uint8_t  baseReg;   // 0-15, the register holding the object pointer
    uint8_t  length;    // encoded instruction length
};

constexpr size_t kMaxSites = 8;

struct Report {
    LockState state;

    uint32_t sitesFound;            // total, even if more than kMaxSites - a truncated count hides a
    uint32_t sitesRecorded;         // surprise, and a surprise here is the whole signal
    uint32_t sitesExpected;
    Site     sites[kMaxSites];
    uint64_t functionVa;            // the teardown routine the sites live in
    uint64_t scanMs;
    uint32_t scanBytes;             // .text bytes examined, so a scan that found nothing is separable
                                    // from a scan that never ran
    bool     foreignPrologue;       // somebody else was already detouring the teardown

    // The known-answer control, run on the bytes actually found in this image.
    bool selfTestRan;
    bool selfTestClearedByOriginal;  // the located instruction really does clear the bit
    bool selfTestHeldWhenPreserved;  // ... and the same harness reports "held" for a preserving mask

    // The intrinsic instrument. Read through shared memory, so these are the whole process's numbers
    // and not just this module's - a probe that did not win the install race still reports the truth.
    uint64_t calls;                 // times the teardown ran with the lock live
    uint64_t clearsPrevented;       // ... and the bit had gone, and we put it back
    uint64_t callsBitAlreadyClear;  // ... and there was no offline bit to protect
    uint64_t callsBitUntouched;     // ... and the teardown left the bit alone
    bool     instrumentShared;      // false means these counters are this module's only
    bool     installedHere;         // whether THIS module owns the trampoline
};

// Worker thread, after the build guard accepts. Safe to call twice; the second call reports the first.
LockState Apply();

const Report& Current();
Exercise Exercised();

// Pure, and the part worth testing hardest: every instruction in `text` that ANDs the SessionFlags qword
// with a mask that does not preserve the offline bit. Returns the number FOUND, which may exceed
// `outCap` - the caller needs to know it was surprised, not just what fitted.
size_t FindClearSites(const uint8_t* text, size_t len, uint64_t textVa, Site* out, size_t outCap);

// Executes one located instruction against a scratch object seeded with `seed`, and reports the flag
// word it leaves behind. Touches no game state. False if the stub could not be built.
bool RunSiteOnScratch(const uint8_t* insn, size_t insnLen, uint8_t baseReg, uint64_t seed,
                      uint64_t* out);

void ResetForTest();

// Report a lock this process never applied, so an offline harness can drive the code paths that are
// gated on containment.  It installs NOTHING -- no hook, no shared mapping, no site scan -- and only
// the reported STATE changes, which is exactly why it is confined to tests: a shipped caller would
// be claiming protection it does not have.  ResetForTest puts it back.
void ReportLockedForTest();

}  // namespace offlinelock
}  // namespace replay
