#pragma once

// B.6, owned by W1-F.  Getting a loaded match on screen with no network.
//
// ★ The spike's headline result, because it changes which implementation ships.
//
// Command-line augmentation DOES reach the parser - WinMain calls the plugin loader at
// 0x14151D42E and GetCommandLineA 46 bytes later at 0x14151D45C, straight-line, and hands the
// returned pointer to the tokeniser; the CRT's __argv is written and never read by game code.
// But no switch reaches offline.  Every offline decision the engine makes reads bit 0x40 of the
// SessionFlags qword at CR15Game+0x8A70, and the only writers of that bit in the whole image are
// sub_1400FE0A0 and its sibling sub_1400FE0E0.  So SE-A is mechanically sound and still cannot
// do the job, which is why SE-B is the default here.  docs/spikes/W1-F.md carries the evidence.
//
// The three implementations, and what each can actually do:
//
//   SE-B  (default)  the stock call sub_1400FE0A0(CR15Game*, SessionParams*).  Ledger row 0.3,
//                    Level 0.  Sets the offline bit itself, can be issued at any time, and is
//                    therefore the only strategy that can change level mid-session (M7).
//   SE-A             command-line augmentation, ledger row 1X.1.  Proven to reach the parser,
//                    used only to carry engine-MODE switches (-fixedtimestep, and the flat-screen
//                    pair when configured).  RequestEntry on SE-A alone refuses and says why.
//   SE-C             the launcher supplies the switches.  Always works, never enters offline.
//
// Threading: RequestEntry is accepted on any thread and PERFORMED on the tick thread, because
// sub_1400FE0A0 tears the current session down and builds a new one.  Doing that underneath a
// running frame is a crash, so the call is deferred to a tick boundary and State() is how a
// caller observes the outcome.

#include <stdint.h>
#include <stddef.h>

#include "bootstrap/bootstrap.h"

// Architect ruling 2026-08-28: the whole of Appendix B lives in `namespace replay`, and
// services.h forward-declares ISessionEntry there.
namespace replay {

enum class NetGameState { LoadingLevel = 7, ReadyForGame = 8, InGame = 9 };

// ★ Why an entry request was not performed - and the one thing a caller needs in order to know
// whether asking again can ever work.
//
// L3 (2026-09-03 launch test, finding F1) is what this exists for. The boot auto-open
// called RequestEntry at NetGame state 1 ("loading root"); the tick performed
// SESSION.CREATE_OFFLINE_MATCH, which tears the live session down and builds a new one; 1.7 s
// later the engine reached its login step with the session it was half-way through building
// already gone and said so - "[LOGIN] Log in request failed: One or more prerequisites are
// missing" -> state -94, "Remaining in state (login failed) after leaving game". Terminal: the
// run never saw a lobby or a level again, and 14 of 17 probe rows filed could-not-ask.
//
// An accepted/refused pair cannot express "not yet", so a caller that meets one has no way to
// tell a wait from a dead end. This is the third answer.
enum class EntryRefusal : uint8_t {
    NotAsked = 0,   // no entry has been requested of this module yet - not an answer about one
    Accepted,       // the request was taken; entry performs on the tick
    NotInLevelYet,  // TRANSIENT. The session is still being built - ask again on a later tick.
    CannotEnter,    // SETTLED for this session: asking again will not change the answer.
};

const char* EntryRefusalName(EntryRefusal refusal);

class ISessionEntry {
public:
    // SE-A augments the command line BEFORE the parser reads it, so it CANNOT change level
    // after startup. The UI must not offer what the selected strategy cannot do (M7).
    virtual bool         CanChangeLevelMidSession() const = 0;
    virtual bool         Matches(uint64_t levelId, uint64_t gametypeId) const = 0;
    virtual bool         RequestEntry(uint64_t levelId, uint64_t gametypeId) = 0;
    virtual NetGameState State() const = 0;
    virtual void*        NetGame() const = 0;
    virtual const char*  Strategy() const = 0;      // "SE-A" | "SE-B" | "SE-C"
    virtual const char*  FailureReason() const = 0;

    // Why the most recent RequestEntry answered as it did. Read it after a false return: it is
    // the difference between "wait for the level and ask again" and "stop asking".
    //
    // Deliberately NOT pure. This interface is frozen and other packets implement it; an
    // implementation written before the question existed answers CannotEnter, which tells a
    // caller to stop rather than to spin - the behaviour every caller already had.
    virtual EntryRefusal LastRefusal() const { return EntryRefusal::CannotEnter; }

    // RELOAD-6. Leave the level this session is in and enter it again through the same stock call
    // RequestEntry uses, with the level and gametype CR15Game already holds -- a new match on the
    // same host. Only from "in game" (NotInLevelYet otherwise); performed on the tick like any
    // entry. Defaulted for the implementations written before it: they cannot, and say so.
    virtual EntryRefusal RequestLevelCycle() { return EntryRefusal::CannotEnter; }
    virtual bool         EntryPending() const { return false; }

protected:
    ~ISessionEntry() = default;
};

namespace session {

// The guarded one-byte load the pointer walk is built on, under its own name. It is here because the
// __try filter behind it is only provable from outside: a page kind the filter does not take does not
// make a check fail, it leaves an UNHANDLED exception on the calling thread, so the only way to show
// the filter covers a guard page is to raise one through this call and still be running afterwards
// (tests/session, AUDIT_M9 F6). It answers about the instant of the load and nothing later.
bool ProbeReadableByte(const void* at);

// The engine's raw state, which is wider than NetGameState: the negative values are failure
// states and -98 ("no network") is what a failed HTTP bind produces.  Callers that only branch on
// InGame use ISessionEntry::State(); callers that report use these two.
int32_t     StateRaw();
const char* StateName(int32_t state);

// The SessionFlags qword at CR15Game+0x8A70, or 0 when there is no CR15Game yet.  Bit 0x40 is
// the offline/host bit this whole packet turns on.
uint64_t SessionFlags();

// CR15Game+0x7AE0.  Bit 0x2000000 is "boot into multiplayer": if it is set when the engine
// reaches GAME.BOOT_BRANCH the game goes to matchmaking, which SC10 forbids.
uint64_t ModeFlags();

// ★ Sets the offline bit EARLY - before login has finished - which changes what the engine does
// when login fails, and is the closest thing this build has to a login-suppression mechanism.
//
// sub_14017E2D0 (the login callback, at state 2 "logging in") and sub_14017F070 (the login/HTTP
// error handler) both branch on exactly (SessionFlags & 0x44) == 0x40:
//
//     offline bit set   -> sub_14017F520: load json|r14|defaultserverprofile.json, tell NSUser to
//                          go offline ("[NSUSER] Going offline"), continue
//     offline bit clear -> state -94, "login failed"
//
// So arming the bit before login resolves turns a failed login from a dead end into the engine's
// own offline mode. It does NOT stop the login being ATTEMPTED - that is still unestablished, and
// the probe's egress blocker is what makes the attempt harmless.
//
// Also sets SessionFlags bit 41 (0x20000000000) when `forceNoNetwork` is true. That is the
// engine's own force-offline flag: every connectivity decision in CR15NetGame reads
// `(flags & 0x20000000000) != 0 || !connectivityCheck()` and takes the no-network branch. In this
// build the only thing that would set it is a cmov against a .rdata constant that ships as zero,
// so it is dead code we are re-enabling rather than behaviour we are inventing.
//
// Returns false when there is no CR15Game yet. Idempotent.
bool ArmOfflineBeforeLogin(bool forceNoNetwork);

// ---------------------------------------------------------------------------------------------
// The authoritative mode signal, per the 2026-08-29 ruling: the STATE decides, the switch is only
// a pre-session hint. It lives here because W1-F owns the only two writers of the bit it reads.
// ---------------------------------------------------------------------------------------------

enum class SessionNetwork {
    NotYetKnown,      // no session to judge yet: nothing decided, the caller's hint still stands
    Offline,          // SessionFlags says offline/client-hosted: there is no remote peer to cut off
    LiveOnlineMatch,  // a match is up and it is not the offline one: the ONE no-arm case
};

const char* SessionNetworkName(SessionNetwork state);

// Cheap: two pointer reads and a compare. Safe to call before CR15Game exists, before CR15NetGame
// exists, and from any thread.
//
// How each answer is reached, and why not the obvious alternatives:
//
//   Offline          (flags & 0x44) == 0x40 - the same predicate the engine's own port selection
//                    and host branch use. DECISIVE the moment it is set, because the only two
//                    writers of bit 0x40 in the whole image are sub_1400FE0A0 and sub_1400FE0E0.
//   LiveOnlineMatch  NetGame state >= 8 ("ready for game"), and not offline. A match exists and it
//                    is not ours.
//   NotYetKnown      everything else, including the whole menu and matchmaking phase.
//
// ★ NOT the "session active" bit 0x4000. It looked like the right witness and it is not: it is set
// once, by NETGAME_INIT, and NOTHING in the image ever clears it - EndMultiplayer clears only bits
// 0x02 and 0x40. So it means "a session has existed in this process", and after a session ends it
// would report a torn-down session as a live one. The state field does not have that flaw.
SessionNetwork NetworkState();

// ★ The credibility test for a -offline hint lives in replay::bootstrap, NOT here. Orchestrator
// ruling 2026-08-29: both its inputs are bootstrap-level - a command-line switch and a loaded
// module - and neither is an engine read, while src/bootstrap cannot call up into src/engine
// without inverting the layering. NetworkState() below genuinely needs the engine; that predicate
// does not. See replay::bootstrap::OfflineHintIsCredible.

// CR15Game+0x855E, the -httpport override in network byte order. Non-zero means the engine
// skipped the 6719/6721 selection entirely, so any conclusion drawn from the bound port is void.
uint16_t HttpPortOverride();

// ★ CR15Game+0x7AE0 bit 0x100. It is NOT a witness that GAME.BOOT_BRANCH ran - an earlier
// version of this header said it was, and the audit was right that it is not. The single write,
// at 0x140108C4D, sits inside the ELSE arm and behind a SECOND guard:
//
//     if (modeFlags & 0x2000000) { sub_140100B90(...); ... }        // multiplayer boot: never sets it
//     else if (levelHash != -1) { log "Creating start gamespaces"; modeFlags |= 0x100; ... }
//
// So it means "the gamespace arm ran, with a level set", and it stays clear both after a
// multiplayer boot and on the else arm with no level.
bool GamespacesCreated();

// The multiplayer arm's own footprint: it sets CR15Game+0x7B14 to 1, but only when SessionFlags
// bit 0 (online) is set. On the no--level default both bits are set together, so this is a usable
// positive witness there - and only there.
bool MultiplayerBootSeen();

// ★★★ WHEN THE SUPPRESSION BELOW MAY RUN, WHICH L5 PROVED IS NOT "ALWAYS".
//
// It used to run every tick on the assumption that a set bit was always a mistake. On
// `-region usc -offline -windowed -level mpl_arena_a ...` the bit is CORRECT engine behaviour
// (-region is one of the seven setters; tests/session/boot_flag_expectation.h) and it is also that
// launch's only boot path, because rlay's -offline patch redirects the arm this bit selects to
// sub_1400FE0E0 - host this level locally, the sanctioned in-client host of
// Clearing it removed BOTH arms: no login block, no
// "Beginning multiplayer", no "to loading level", and the game idled in the main menu for 234 s
// (launch run 20260902-071934Z, diagnosis DIAG-L5; AUDIT_WAVE1C.md:229-240
// predicted exactly this).
//
// The suppression exists for one case and is now gated on it: the plugin is starting a REPLAY
// session and must not boot-join a match.
enum class BootBitPolicy {
    LeaveIntactOfflineHost,     // -offline on the line, or our offline patch: the bit IS the local host
    SuppressForReplaySession,   // a replay was requested and no local host needs the bit
    LeaveIntactNoReplay,        // nobody asked for a replay: this is the owner's own game
};

const char* BootBitPolicyName(BootBitPolicy policy);

// Pure, so all three outcomes are constructible without a game or a launch.
//
// ★ -offline OUTRANKS Replay, and that is the whole fix. bootstrap::ClassifySession answers
// Replay for an -offline launch that also carries ECHOREPLAY_OPEN - which is precisely the L5
// profile, whose ECHOREPLAY_OPEN named the replay directory - so gating on the mode alone would
// have cleared the bit again. An -offline launch keeps the bit whatever else is true of it.
//
// ★ offlineSwitchSeen is the SWITCH, not bootstrap::OfflineHintIsCredible, deliberately: that
// predicate needs rlay.dll to be loaded and plugins load alphabetically, so echoreplay.dll asks
// before rlay exists and would get `false` on exactly the launch that must not be touched. The
// switch has no such window, and erring towards leaving the bit alone is the safe direction.
//
// ★ offlineHost is that switch OR this plugin's own offline patch (bootstrap/offlinepatch.h) having
// applied or found the -offline bytes: either one makes the bit the in-client local host.
BootBitPolicy ClassifyBootBitPolicy(replay::bootstrap::SessionMode mode, bool offlineHost);

// The policy for THIS process, decided once from the command line and the environment - both fixed
// before the first tick - and logged once. Reads them through W1-C's own published readers
// (bootstrap::ClassifySession over bootstrap::OpenRequestPresent, and bootstrap::HasSwitch); this
// packet parses no command line of its own. Callable before CR15Game exists.
BootBitPolicy BootBitPolicyForThisLaunch();

enum class BootSuppression {
    AlreadyClear,        // nothing to do, and that is provable
    ClearedTooLate,      // MultiplayerBootSeen(): sub_140100B90 has already run
    ClearedUnverified,   // the bit is now clear, but we CANNOT prove we were in time
    NoGame,
    RefusedByPolicy,     // the launch needs the bit; see BootBitPolicy
    RefusedUncontained,  // livesafety::Feature::BootModeBits -- this process may not write at all
};

const char* BootSuppressionName(BootSuppression result);

// How many times this module has actually cleared the bit. The observable behind the probe's S9:
// it counts the WRITE, not the decision, so a gate that stops working shows up here.
uint32_t BootSuppressionCount();

// Clears the boot-into-multiplayer bit and says honestly what it can prove about the ordering.
// ★ It never claims to have been "before GAME.BOOT_BRANCH" unless a witness supports it. There is
// no flag that positively means "the branch has not run yet", so ClearedUnverified is the normal
// answer and the probe reports it as such rather than as a success.
//
// The policy is checked FIRST and INSIDE, not at the call site, so no caller can bypass it. The
// explicit-policy overload exists so all four non-policy outcomes stay reachable from a test host
// that has no CR15Game and no control over its own command line.
BootSuppression SuppressBootToMultiplayer();
BootSuppression SuppressBootToMultiplayer(BootBitPolicy policy);

// CMatSym::Hash, the engine's own name hash, so a level or gametype name becomes the exact u64
// the engine stores.  Zero if the build guard refused the site.
uint64_t HashName(const char* name);

// ---- SE-A, separated from its installation so it can be tested without a game ---------------
//
// Appends the tokens in `tokens` that are not already present to `original`, into `out`.
// Space-separated, because CMDLINE.TOKENIZE splits unquoted tokens on 0x20 only - a tab would
// not separate them.  Idempotent: a token already on the line is never repeated, so a relaunch
// through the same hook cannot grow the string without bound.  Returns the length written, or 0
// if it would not fit, in which case `out` is left holding a copy of `original`.
size_t AugmentCommandLine(const char* original, const char* const* tokens, size_t tokenCount,
                          char* out, size_t outCap);

// True when `line` already carries `token` as a whole token, honouring the '=' value form and
// the engine's three switch prefixes ('-', '+', 0x96).
bool HasToken(const char* line, const char* token);

// ★ Runs from bootstrap::Attach via REPLAY_ATTACH_HOOK, under the loader lock, before the worker
// starts - because WinMain reaches GetCommandLineA 46 bytes after our DllMain returns and no
// thread we could start would win that race.
//
// It redirects ONE IAT slot: echovr.exe's __imp_GetCommandLineA at 0x1416C3340. Not a MinHook
// detour, because arming one suspends every thread in the process and we are holding the loader
// lock. An 8-byte write to a data page suspends nothing, modifies no .text, and is exactly ledger
// category 1X - our own process's import thunk, never game code.
//
// ★ THE A VARIANT ONLY, AND THAT IS LOAD-BEARING. The engine never reads GetCommandLineW, but
// rlay.dll does: its Detours hook on CLI.APPLY calls GetCommandLineW and CommandLineToArgvW and
// scans the result for its own switches. Augmenting W would silently turn on EchoRelay behaviours
// nobody asked for. tests/session asserts the W slot is left alone.
//
// Refuses when the slot does not currently hold kernel32's GetCommandLineA: someone else got there
// first, and S4.5.3 rule 4 says refuse and log rather than overwrite.
bool InstallCommandLineHook();

// What the hook appends, exposed so a test can assert the set rather than infer it.
// ★ DELIBERATELY NOT the flat-screen pair. S8.4.2 lists -noovr -spectatorstream here, but
// -spectatorstream sets CR15Game+0x7AE0 bit 0x2000000 (boot into multiplayer), and this packet
// measured that no switch reaches offline anyway. Shipping them would aim every launch at
// matchmaking for no gain. See docs/spikes/W1-F.md S10.
const char* const* AugmentTokens(size_t* count);

// ★ THE WHOLE OF RequestEntry's DECISION, as a function of everything it reads. RequestEntry does
// the engine reads and then asks this; nothing else decides. The gate is therefore INSIDE the
// entry point and no caller can lose it, and the precondition stays reachable from a test host
// that has no CR15Game - the same shape as SuppressBootToMultiplayer's explicit-policy overload.
//
// The threshold is the in-level states 7/8/9 ("loading level", "ready for game", "in game"), for
// two reasons that agree: DESIGN 8.4a.1 defines the level as the observer's first
// `[session] NetGame state -> 7/8/9` and requires the browser to open OVER it, and every state
// below that is one in which the engine is still assembling the session that the stock call would
// destroy - 1 "loading root" and 2 "logging in" being exactly where L3 destroyed it.
//
// `alreadyThere` is the caller's Matches() && state==9 short-circuit, and is answered Accepted
// without a latch. A negative state (a failure state such as -94) is NOT in level and is refused
// like any other: the session cannot host a replay and re-entering it does not repair the login.
EntryRefusal EntryVerdict(bool enabled, bool sitesOk, bool gameConstructed, bool alreadyThere,
                          int32_t netGameState);

// ENG-GAMETYPE-ENABLE (F2). True when a recorded gametype cannot be a recognized full-64-bit CMatSym
// hash and so must be replaced before it seeds CR15Game: the all-ones sentinel, or any non-zero value
// whose high 32 bits are clear (the manifest widens the engine's 32-bit gametype, so a real hash never
// survives it). 0 is the "leave whatever the command line set" wildcard and is NOT overridden. Pure,
// so the substitution rule is testable without an image; the actual substitute is resolved from a
// symbol name at init. Exposed for the discriminating test.
bool NeedsGametypeOverride(uint64_t gametypeId);

// What RequestLevelCycle does with a request. A cycle entry already performed
// whose level has not been left yet IS the cycle a second request asks for (two Opens in one frame),
// so the second folds into it instead of running CREATE_OFFLINE_MATCH again while the first
// transition starts. `enteredAtMs` is 0 when no performed cycle is waiting for its leave. The fold
// lapses after kCycleLeaveGraceMs, the playback cycle's own exit bound: an entry that never leaves
// the level must not swallow every later request.
enum class CycleRequest : uint8_t { Queue, InFlight, NotInLevel };
inline constexpr uint64_t kCycleLeaveGraceMs = 5000;
CycleRequest CycleRequestVerdict(bool pending, uint64_t enteredAtMs, uint64_t nowMs, bool inGame);

// The answer the most recent RequestEntry gave, for callers that hold no ISessionEntry.
EntryRefusal LastEntryRefusal();

// A request has been accepted and the tick has not performed it yet.
bool EntryRequestPending();

// How many times this module has actually called SESSION.CREATE_OFFLINE_MATCH. Like
// BootSuppressionCount it counts the CALL and not the decision, so a refusal that still reaches
// the engine shows up here.
uint32_t EntryAttemptCount();

// Performs whatever RequestEntry accepted. MUST run on the engine thread: the stock call tears
// the live session down and builds a new one. The module's tick calls this; W1-F's probe calls it
// from its own engine-thread pump so the probe does not depend on W1-D landing first.
void PumpOnEngineThread();

ISessionEntry* Interface();

}  // namespace session
}  // namespace replay
