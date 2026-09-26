// Layer 3 of the machinery enforcing the owner's binding "no server" constraint.
//
// Built by W1-F for its probe and promoted here on the orchestrator's ruling (2026-08-28): this packet
// owns hookmgr and the loader-lock discipline the trampolines need, it has the attach-time entry point,
// and every probe already links bootstrap, so promoting it creates no new dependency.
//
// WHAT IT DOES. echovr.exe imports WS2_32 statically, so its egress entry points can be trampolined and
// every non-loopback destination refused with WSAENETUNREACH. The harness's socket census is a TCP table
// read: a UDP socket that never connects has no table entry, so a census can only ever say "no TCP peer".
// This closes that hole from inside the process.
//
// ★ ARM TIMING, WHICH IS THE HONEST PART. MH_EnableHook enumerates threads with
// CreateToolhelp32Snapshot and SUSPENDS them to rewrite prologues. Doing that under the loader lock is
// the deadlock this packet's whole DllMain design avoids, so the blocker arms on the WORKER THREAD, not
// at attach. There is therefore a window between attach and arm in which egress is NOT guarded. That
// window is MEASURED and reported per launch as armedAtMs - never assumed constant, and never assumed
// smaller than it is.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace replay {
namespace egress {

// Three states, not two. "Already initialised by hookmgr" is not a failure, and a component that treated
// it as one would silently not arm while reporting nothing wrong - which the harness records as
// `unknown`, never as `safe`.
enum class ArmState : uint8_t {
    NotAttempted,
    Armed,
    PartiallyArmed,   // some entry points took the trampoline and some did not: NOT a safe state
    Failed,
};

const char* ArmStateName(ArmState state);

// Is "live of requested" a ratio that describes one set of entry points? The arm line printed
// "15 of 10" for a whole sweep: `live` counts both row tables and the denominator was the size of
// one of them, so the count ran past its own total and read as MORE coverage than was asked for.
// A zero denominator fails for the mirror reason - the total was never written, so the ratio has
// nothing to say and must not be printed as though it did.
bool ArmTallyConsistent(uint32_t live, uint32_t requested);

// ★ WHY the blocker is armed, which is a different fact from WHETHER it is.
//
// ★★ ARMED BY MODE, NOT BY A GLOBAL DEFAULT, and the correction matters more than the original bug.
//
// Until 2026-08-29 the blocker armed on `inSweep || config` with the key defaulting to false, so a
// shipped viewer phoned out freely and only the harness ever saw a refusal. The first fix - default it
// to armed - broke something worse: THIS PLUGIN CAPTURES ONLINE MATCHES. The owner plays normally with
// it loaded, and in that session the network is the SUBJECT of the capture, not a leak. A blocker armed
// there stops his matches, out of a folder shared with other people's mods.
//
// ★★★ THE RULE IS ONE CONDITION, NOT A TABLE:
//
//       DO NOT ARM ONLY WHEN THE SESSION IS A LIVE ONLINE MATCH BEING CAPTURED. ARM OTHERWISE.
//
// The invariant is P3b - this plugin originates no outbound traffic of its own. The single reason a
// capture session is exempt is that in a LIVE ONLINE MATCH the client's traffic is the SUBJECT of the
// recording, and blocking it would make the owner's primary use case impossible. In an -offline session
// there is no live match, so that justification is simply absent and the blocker arms.
//
// ★ -offline is the axis that matters, not -replayviewer. -offline says whether there is a live match
// to record; -replayviewer says what we intend to do with one, which is weaker - and it is absent from
// `offline-local`, the very profile that produced 82 refused destinations.
//
// ★ The bias when the command line cannot be read is ARM. Refusing a socket during a live capture is
// visible and recoverable; an offline or replay session phoning out is the breach.
//
// S8's P2/P3 ("no socket to any non-loopback address, ever") is a property of the REPLAY VIEWER, not of
// the capture plugin. They are one DLL in two modes and the design conflates them; the design and
// 6321 still describe the old default and need the architect.
enum class ArmTrigger : uint8_t {
    Unspecified,       // a caller that did not say - tests and other packets' probes

    // ★★ ARMED ON A HINT. These are guesses made before the session exists, and every one of them is
    // RELEASABLE: if the session state later contradicts the guess, the arm must be given up.
    // A command-line switch is evidence of what the user ASKED FOR, never of what the engine did -
    // decisively so for -offline, which is absent from stock echovr.exe and exists only because
    // rlay.dll patches it in. Plugin loaded, -offline passed, rlay absent: the engine ignores the
    // switch, the player goes ONLINE, and an arm taken on that hint would cut off their match.
    OfflineHint,       // -offline on the command line
    ReplayHint,        // -replayviewer on the command line
    ModeUnknown,       // ★ the command line could not be read at all, so we took the strict side

    // ★★ ARMED ON STATE OR POLICY. Not releasable: nothing later can make these wrong.
    OfflineSession,    // SessionFlags bit 0x40 observed SET - the session really is offline
    ReplayMode,        // a replay was actually entered, not merely requested
    SweepOverride,     // a sweep, whatever anything else says
    ConfigForcedOn,    // the config asks for it outright

    // NOT ARMED, and why. Exactly one reason each.
    LiveOnlineCapture, // ★ the ONLY exemption: a live online match whose traffic is the recording
    DisabledByConfig,  // somebody turned it off on purpose
};

// Whether an arm taken for this reason may be given up when the session state contradicts it. Hints
// yes, observed state and policy no.
bool TriggerIsReleasable(ArmTrigger trigger);

const char* ArmTriggerName(ArmTrigger trigger);

// The live entry points as a JSON array body, e.g. "connect", "sendto". HARNESS.md 2.3 asks for the
// NAMES: "4 hooks" does not say which four, and a partial arm is entirely about which one is missing.
const char* HookNamesJson();

// What this blocker does NOT cover, as a string the report carries.
//
// The harness reads `refused: 0` as evidence, and it is only evidence over the covered set. A guard
// credited with protection it does not provide is worse than one nobody trusts, so the scope travels
// with the number instead of living in a document somebody has to remember to read.
// tests/loaderprobe/network_surface.tsv is the enumerated form, regenerated per gate run.
const char* CoverageNote();

struct Report {
    ArmState state;
    // ★ Milliseconds from ATTACH to the trampolines being live - HARNESS.md 2.3's definition,
    // and the window in which the blocker is not yet protecting anything. Measuring Arm()'s own
    // duration instead reports single-digit milliseconds and understates the real exposure by about a
    // second, which is a silent waiver of exactly the risk this field exists to publish.
    uint64_t armedAtMs;
    uint64_t armDurationMs;  // how long the trampolines themselves took, for separating the two
    uint32_t hooksRequested;
    uint32_t hooksLive;
    uint64_t refusedRemote;  // destinations refused because they could leave the machine
    uint64_t allowedLocal;
    uint32_t distinctDestinations;
    uint32_t destinationOverflow;
    uint64_t extensionsRefused;   // ConnectEx / WSASendMsg lookups turned away at WSAIoctl
    uint64_t socketsCreated;
    // ms from attach to the FIRST socket in the process, or -1 if none was created. Turns "armed early
    // enough" from an assumption into a measurement: a first socket later than armedAtMs proves the
    // unguarded window contained no socket at all.
    int64_t  firstSocketAfterMs;
    // Whether wininet.dll was mapped when we armed. echovr.exe statically imports BugSplat64.dll, which
    // statically imports wininet, AND echovr.exe imports wininet directly - so the loader resolves it
    // before the entry point and this should always be true in the game. Reported rather than assumed,
    // because the whole WININET half of the coverage claim depends on it.
    bool     wininetLoaded;
    // Reported beside the state so a reader can tell a guarded product from a guarded test run.
    ArmTrigger trigger;
    // ★ An arm given up because the session state contradicted the hint it was taken on. `released`
    // with a non-zero refusedRemote is the case worth reading closely: this process refused traffic
    // before it learned it should not have been.
    bool       released;
    ArmTrigger releaseReason;
    uint64_t   refusedBeforeRelease;
};

// Worker thread only. Creates and enables the trampolines. Safe to call twice; the second call reports
// the first call's state rather than re-arming.
// The trigger is defaulted so callers that have no opinion - unit tests, other packets' probes - keep
// compiling unchanged; only the bootstrap's own decision is worth recording.
ArmState Arm(ArmTrigger trigger = ArmTrigger::Unspecified);

// ★★ Give up an arm that was taken on a HINT the session state has now contradicted.
//
// It does NOT unhook. Removing the trampolines would need a thread freeze and would risk a thread
// standing inside a detour; the hooks stay and every one of them becomes a passthrough, which is a
// single relaxed atomic on the hot path and cannot fault. That is why this does not conflict with the
// design note above about never tearing hooks down at teardown - nothing is torn down here.
//
// Refused, and logged, when the current arm was NOT taken on a hint: a sweep, an explicit config key,
// an observed offline SessionFlags bit and an actually-entered replay are all facts rather than
// guesses, and nothing later can make them wrong. Capture -> replay therefore stays one-way, while
// hint -> contradicted-state is reversible, which is the distinction W2-G's reasoning asks for.
//
// Whatever was already refused stays counted. A release changes what happens NEXT; it cannot rewrite
// what the blocker did while it was enforcing.
bool Release(ArmTrigger reason);

// False once Release has taken effect. The hooks are still installed either way.
bool Enforcing();

// Records WHY the blocker is not armed, without arming it. A run that deliberately leaves egress open
// still has to say so in its report: an absent trigger and a refused one are different facts.
//
// ★ It deliberately does NOT mark the blocker as attempted. A capture session that later begins playback
// is expected to call Arm(ArmTrigger::ReplayMode) at that point, and a flag set here would silently
// refuse it - which is the failure this whole enum exists to stop.
void RecordNotArmed(ArmTrigger trigger);

const Report& Current();

// Every destination seen, for the probe's report. `index` < Current().distinctDestinations.
bool DestinationAt(size_t index, const char** text, const char** via, uint64_t* count, bool* local);

// ---------------------------------------------------------------------------------------------------
// ★ THE DESTINATION SET, REDACTED. AUD-L5 §8: "16/16 hooks live, 60 refused" carries no hosts, so
// "the blocker did its job" is not falsifiable - the auditor had to infer the peer set out of the
// engine's own log. Counts alone cannot distinguish 60 refusals of 127.0.0.1 from 60 refusals of a
// production login host, and those are opposite findings.
//
// It cannot be closed by emitting the raw set. The owner's real endpoints live in _local/config.json
// and the compiled-in fallbacks are live production hosts; a record that names them turns a safety
// artefact into the leak it exists to prevent. So the set travels REDACTED:
//
//   * loopback, unspecified, RFC1918, CGNAT and IPv6 ULA/link-local literals go VERBATIM with their
//     port. They cannot be secrets - that is the whole point of those ranges - and a reader needs to
//     see them exactly to conclude "nothing left this machine".
//   * every other host becomes `redacted-<8 hex>`, the first 8 hex digits of SHA-256 over a per-LAUNCH
//     RANDOM salt and the lowercased host. Stable within one launch, so a reader can count distinct
//     hosts and correlate rows; not invertible from the record, which is what the record must not
//     contain. The salt was the build id, which the plugin log prints -- log plus record then
//     brute-forces a candidate host list, and the token was only as strong as the list is long.
//   * a DNS NAME additionally keeps its TLD (`redacted-1a2b3c4d.net`). A TLD is a one-of-a-few-hundred
//     fact that separates "our own host" from "somewhere on the internet" without naming anything.
//   * the port is kept in every case. It is a protocol fact, not an identity.
//
// Never a config key, never a full hostname, never a URL or its query string - the recorder never held
// any of those; `text` is a peer address or a bare host.
//
// ★ FAIL-CLOSED. If the salt is unavailable the tokens would not be stable and the temptation is to
// emit the raw set instead. These return nullptr and RedactionRefusalReason() says why. No caller can
// get a raw destination out of this header.
//
// ★ COST. Both build their answer on call, walking at most kMaxDestinations recorded rows. Nothing
// here runs on the hook path - the trampolines only append to the destination table, exactly as
// before - and the record writer calls each of these once per launch.

// The array BODY for the record's `refusedDestinations` / `allowedDestinations`, elements already
// formed: {"dest": "...", "via": "...", "count": N}. Empty string when the set is empty, which is a
// real answer and not a refusal. nullptr means REFUSED.
const char* RefusedDestinationsJson();
const char* AllowedDestinationsJson();

// Why the two above refused, or nullptr when they did not.
const char* RedactionRefusalReason();

// The rendering of one recorded destination text, exposed so the rule can be tested against a
// synthetic set without a game. `salt` must be non-empty: a null or empty salt is the unavailable
// redactor, and this returns false rather than falling back to the raw text.
bool RedactDestination(const char* text, const char* salt, char* out, size_t outCap);

// The salt the two accessors use: 16 bytes from BCryptGenRandom, drawn once at egress init, held in
// process memory only and never written to the log, the record or any other emitted string. A token
// therefore means the same host for the life of ONE LAUNCH and nothing across launches, which is all
// a reader of one record needs. nullptr when the RNG refused - see RedactionRefusalReason().
const char* RedactionSalt();

// Pure, and the part worth testing hardest: is this destination one that cannot leave the machine?
// A null or non-IP sockaddr counts as local - there is nowhere for it to go, and refusing it would break
// local IPC for nothing.
struct SockAddrView { const void* addr; int len; };
bool IsLocalDestination(const void* addr, int len, char* out, size_t outCap);

// Test seam: plant a destination as though a trampoline had recorded it, so the redaction rule can be
// exercised over a public IP and a hostname without this process ever opening a socket to one.
void RecordDestinationForTest(const char* text, const char* via, bool local);

void ResetForTest();

}  // namespace egress
}  // namespace replay
