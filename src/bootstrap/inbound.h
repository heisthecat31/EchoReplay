// The INBOUND half of the owner's binding "no server" constraint, and the half nothing enforced.
//
// ARCH-18 (docs/spikes/ARCH-18-OFFLINE-INBOUND.md), ledger row 1X.4. The engine's offline session
// transport binds INADDR_ANY on both of its bind sites - 0x1401F5DCE writes sin_addr = 0 byte-verified,
// and 0x1401F5D10 reaches getaddrinfo(nullptr, port, AI_PASSIVE|AI_NUMERICHOST), which is documented to
// yield the wildcard. So "offline" never meant "unreachable": on L2 a 100.64/10 mesh peer joined an
// -offline session while every egress array read clean, because egress::IsLocalDestination measures
// DESTINATIONS and the constraint is about SOCKETS.
//
// This module detours ws2_32!bind in our own process and rewrites a wildcard sockaddr to loopback
// before the OS sees it, so no endpoint exists for a remote peer to reach. It never REFUSES a bind:
// ARCH-18 S4 rejected that (A2) because the in-client host then fails to start, and a rule that
// disables the sanctioned offline path is not a containment rule.
//
// * SAME PRIMITIVE AS THE EGRESS BLOCKER, deliberately. GetProcAddress + MH_CreateHook +
//   MH_QueueEnableHook + MH_ApplyQueued, on the worker thread, never a second hook library. bind is
//   imported BY ORDINAL (2) from ws2_32, so an IAT-by-name patch would miss it entirely - MinHook
//   rewrites the ws2_32 prologue in place, so an ordinal-resolved IAT slot still lands on the
//   trampoline. That is the same argument the delay-loaded wininet coverage rests on, and
//   tests/bootstrap/test_inbound.cpp exercises it through GetProcAddress(ws2, MAKEINTRESOURCEA(2)).
//
// * IT PATCHES ws2_32's .text, NOT echovr.exe's, so it takes NO SC7 ledger row. SC7's declared_sites
//   and its empty unattributed[] staying unchanged with this installed is the check that keeps the
//   Level-1X / Level-2 distinction honest (ARCH-18 S6.2).
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace replay {
namespace inbound {

// net.loopback_only. A tri-state string for the reason bootstrap.block_egress is one: IConfig::GetBool
// ignores a caller's default for a declared key, so an absent key and an explicit false cannot be told
// apart, and a bool therefore cannot say "no opinion".
enum class Mode : uint8_t {
    Auto,        // derive from the session mode: armed iff the offline lock reads Locked
    On,          // a deliberate override, named in the log with its value and its source
    Off,         // ... and so is this one
    Unrecognised // the key held something that is none of the three. Treated as Auto, said out loud
};

Mode ParseMode(const char* value);
const char* ModeName(Mode mode);

enum class ArmState : uint8_t {
    NotAttempted,
    Armed,
    Failed,
};

const char* ArmStateName(ArmState state);

// WHY we are or are not armed, which is a different fact from WHETHER we are.
//
// * The arming predicate is the OFFLINE LOCK READING Locked - never the presence of -offline in the
//   args. BRIEF_COMMON's R4 ruling: ApplyCommandLine bails at the first switch the engine has not
//   registered, so an earlier unregistered switch silently voids -offline and the launch goes online
//   while the args still say otherwise. A guard keyed on a switch's presence is keyed on a request,
//   not on what the engine did. The lock reaching Locked is durable observed in-process state.
enum class ArmTrigger : uint8_t {
    Unspecified,      // a caller with no opinion - tests and other packets' probes
    OfflineLocked,    // the offline lock reads Locked and net.loopback_only is auto
    ConfigForcedOn,   // net.loopback_only = on, whatever the lock says
    NotOfflineSession,// the lock is not Locked, so there is no offline session to contain
    DisabledByConfig, // net.loopback_only = off
    // `on` re-created the 2026-09-18 "cannot connect to servers" incident:
    // it rewrote the live client's binds to loopback and the join died. A live online session is
    // the one thing that outranks a config key, because no config key can make it safe.
    LiveOnlineRefused,// net.loopback_only = on, IGNORED: this is a live online session
};

const char* ArmTriggerName(ArmTrigger trigger);

// The arming decision, pure, so the table can be exercised without a lock, a socket or a game.
struct Decision {
    bool       arm;
    ArmTrigger trigger;
};
// `liveOnlineExempt` defaults false so a caller with no session fact gets the old table exactly.
Decision Decide(Mode mode, bool offlineLockLocked, bool liveOnlineExempt = false);

// What one intercepted bind turned out to be. Every bind lands in exactly one of these.
enum class Outcome : uint8_t {
    Rewritten,        // a wildcard of a family we handle; the OS saw loopback instead
    AlreadyLoopback,  // it was loopback before we touched it, and we touched nothing
    PassedThrough,    // a family or a length we do not handle: passed UNMODIFIED and recorded
    Refused,          // structurally unreachable - see Report::refused
};

const char* OutcomeName(Outcome outcome);

// ARCH-18 S6.3's four states. Two cannot carry this: a VIOLATION/silence pair cannot tell "armed and
// clean" from "never armed", and online play must land on never-asked rather than on a pass.
enum class Verdict : uint8_t {
    AskedPass,
    AskedFail,
    CouldNotAsk,
    NeverAsked,
};

const char* VerdictName(Verdict verdict);

// What W1-J's Get-BoundEndpoints reading plus this module's counters add up to. Kept pure and kept
// HERE, on the product side, so the rule the harness reports is the rule this suite tests.
struct VerdictInput {
    bool     armed;
    bool     endpointsRead;       // the OS endpoint enumeration was obtained at all
    uint32_t boundNonLoopback;    // entries classifying non-loopback. 0.0.0.0 and :: COUNT: a wildcard
                                  // bind IS the failure and no peer has to arrive first
    uint32_t inboundNonLoopback;  // peers that reached us from a non-loopback address
    uint64_t rewritten;           // the anti-vacuity pair: an all-loopback table on a launch that bound
    uint64_t alreadyLoopback;     // nothing looks identical to a launch this hook protected
};

Verdict Classify(const VerdictInput& in);

struct Report {
    ArmState   state;
    ArmTrigger trigger;
    Mode       mode;
    // Where the mode came from, for the log line ARCH-18 S7.1 requires: the key value AND its source.
    const char* modeSource;

    // ms from ATTACH to the trampoline being live - the window in which a bind is NOT covered.
    // Measuring Arm()'s own duration instead reports single-digit milliseconds and understates the real
    // exposure, which is a silent waiver of the risk this field exists to publish.
    uint64_t armedAtMs;
    uint64_t armDurationMs;
    uint32_t hooksRequested;
    uint32_t hooksLive;

    // ARCH-18 S6.5's counters, under the names W1-J publishes. Four, because a two-way split cannot say
    // the difference between a bind we fixed and a bind that needed no fixing - and the second is the
    // free positive control: the engine's own HTTP listener binds the literal 127.0.0.1 (ARCH-18 1.8),
    // so any launch reaching that code MUST show at least one alreadyLoopback.
    uint64_t rewritten;
    uint64_t alreadyLoopback;
    uint64_t passedThrough;
    // The subset of passedThrough that is a real interface address of a family we handle - neither
    // loopback nor wildcard. It is the one passed-through shape that IS an exposure (ARCH-18 S6.4 and
    // decision 4: the caller named an interface and we do not move it), and it is separated from a
    // short namelen or an AF we never heard of so that the verdict on this line can reach asked-fail
    // without an AF_IRDA bind reaching it too.
    uint64_t namedNonLoopback;
    // Structurally 0: ARCH-18 S4 rejected refusing a bind (A2), because the in-client host then fails
    // to start and locked constraint 2 says the built-in server is the sanctioned offline path. It is
    // PUBLISHED rather than omitted so that the claim "this module never refuses a bind" is falsifiable
    // from a launch record instead of being a sentence in a header - a non-zero value here means
    // somebody built A2 and the record no longer matches the design.
    uint64_t refused;

    // ARCH-18 S6.5's fourth bucket, `not-armed`, is this pair rather than a per-bind count: when the
    // decision is not to arm, no trampoline is installed, so there is no bind to count and inventing a
    // counter that is always zero would say less than the trigger already says. Online play is
    // never-asked and untouched: nothing is hooked at all.
    bool     armed;
};

// Worker thread only, after the offline lock has a verdict. MH_ApplyQueued suspends every other thread
// in the process to rewrite the prologue, which is the deadlock the DllMain design avoids - so this may
// never run under the loader lock. Safe to call twice; the second call reports the first call's state.
ArmState Arm(ArmTrigger trigger, Mode mode, const char* modeSource);

// Records WHY we are not armed without arming. A launch that leaves inbound open still has to say so:
// an absent trigger and a refused one are different facts, and never-asked is not a pass.
void RecordNotArmed(ArmTrigger trigger, Mode mode, const char* modeSource);

const Report& Current();

// Pure, and the part worth testing hardest. Classifies `addr`/`len`, and when the answer is Rewritten
// writes the loopback form into `out` (capacity `outCap`) leaving the CALLER'S buffer untouched - the
// engine memsets and reuses that sockaddr, and mutating it would change what the game believes it
// asked for. On every other outcome `out` is not written and the original is what the OS must see.
//
// A short `len` for the family is PassedThrough, never Rewritten: a rewrite needs bytes that are not
// there, and refusing is not on the table.
Outcome ClassifyBind(const void* addr, int len, void* out, size_t outCap);

// Whether this sockaddr is a loopback address of a family we handle. Exposed because "already
// loopback" is the anti-vacuity control and a control nobody can test is not one.
bool IsLoopbackBind(const void* addr, int len);

// True for 0.0.0.0 and ::, which are the primary failure signal - ahead of any peer.
bool IsWildcardBind(const void* addr, int len);

// True for a well-formed AF_INET/AF_INET6 address that is neither loopback nor wildcard: the bind that
// names an interface, which this module passes through unmodified and which is still an exposure.
bool IsNamedNonLoopbackBind(const void* addr, int len);

// The VerdictInput this module can attest ON ITS OWN, for the log line below. endpointsRead is "we
// classified at least one bind", which is the in-process form of the OS enumeration W1-J does; a launch
// that bound nothing therefore reads could-not-ask and never a pass. inboundNonLoopback stays 0 because
// this module never sees a peer - the harness supplies that half from its own scan and runs the same
// Classify over the union.
VerdictInput SelfView();

// Writes the whole Report as ONE line: every counter, the arming fields, and Classify(SelfView())'s
// word. `occasion` names why it fired. Called once when the first bind after arming is classified, and
// again at teardown - without it the counters exist only inside a process that has exited, which is
// what L2 #2 recorded (F4: "the rewrite is armed and unmeasured").
void LogReport(const char* occasion);

// * L2-RUN-3 F7. THE `teardown` LINE NEVER REACHES THE LOG ON A REAL LAUNCH. The harness kills the
// process tree, so no detach path runs: L2 #3 was graded on the six-second-old `first-bind` snapshot
// while this module's in-process counters had reached rewritten=1 alreadyLoopback=15 -- the rewrite
// WAS exercised and no log line carried the final counts. A report that only survives a clean exit
// measures nothing on the runs that matter. The two occasions below are the answer: one taken at the
// last moment the capture path is known to be alive, and one taken on a clock.

// The capture stop path calls this as it services a stop request -- the same moment the per-close
// sentinel is written. A free function rather than a registration list: there is exactly one
// subscriber, and the occasion WORD stays in this file so the line's contract has one owner.
//
// W2-A's one-line call, beside the WriteStopSentinelForClose in src/capture/capture.cpp (which
// already includes bootstrap/ headers, so no new dependency edge):
//
//     replay::inbound::NoteCaptureCloseServiced();
void NoteCaptureCloseServiced();

// The heartbeat interval. 60 s because the line is ~230 bytes and the ring is drained every
// log.drain_interval_ms (250 ms by default): an hour-long launch adds sixty lines, and the worst-case
// staleness of the counters under a kill is one minute rather than the whole launch.
constexpr uint64_t kHeartbeatIntervalMs = 60000;

// Emits a `heartbeat` report line once `nowMs` has reached the next due time, and schedules the next
// one from `nowMs`. Returns whether it logged. Called from the bootstrap worker's poll loop, which
// already ticks on that interval; it does nothing at all unless the module is ARMED, so an online
// launch stays silent and pays one predictable-branch load per tick.
//
// `nowMs` is a GetTickCount64()-domain stamp passed IN rather than read: a cadence that reads the
// clock itself can only be tested by waiting a minute, which is a test nobody runs. The FIRST call
// after arming sets the baseline and never logs -- otherwise the first worker tick after Arm would
// emit a heartbeat carrying exactly what the ARMED line carried a moment earlier.
//
// Rescheduling is from `nowMs` and not from the due time, so a worker starved for ten minutes owes
// one heartbeat rather than ten identical ones back to back.
bool MaybeLogHeartbeat(uint64_t nowMs);

void ResetForTest();

}  // namespace inbound
}  // namespace replay
