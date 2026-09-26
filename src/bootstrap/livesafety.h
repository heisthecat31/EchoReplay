#pragma once

// ONE PREDICATE FOR EVERY ENGINE-WRITING PLAYBACK PATH (RECORDER-2 part A, SPEC-RECORDER-2 S5.2).
//
// The owner records demos by SPECTATING LIVE PUBLIC MATCHES. In that process the offline-bit lock is
// never even attempted -- bootstrap.cpp takes the IsLiveOnlineExempt arm -- so every containment
// question in this plugin has the same answer, and it is "no". The problem the audit found is that
// only some of the playback write paths asked: the engine clock take, the grip writes, the pose and
// synthetic-owner stamps on OTHER PEOPLE'S BODIES, the audio rate control, scrub, the cursor claim
// and the band-3 game-space registration were dormant only because no demo was loaded. One `load`
// typed into the console during a live match would have armed all of them.
//
// So the predicate is stated once, here, and every one of those paths asks IT rather than its own
// re-derivation. Two terms, both from boot:
//   * this session is NOT the live-online exemption, and
//   * the offline-bit lock is held.
// Anything else -- including a process that has not classified itself yet -- refuses. Ambiguity
// falls to the contained side, which is the same rule bootstrap.h states for the exemption itself.

#include <stdint.h>

namespace replay::livesafety {

// Why the answer is what it is. The startup block prints this, and so does every refusal line, so
// "inert" always arrives with its cause attached.
enum class Verdict : uint8_t {
    Contained,        // offline lock held, not exempt: playback may write engine state
    LiveOnlineExempt, // a live public match -- the one session this plugin must not write into
    LockNotHeld,      // offline session, but the lock did not take: not contained enough to write
    NotDecided,       // boot has not classified this process yet. Fail closed
};

const char* VerdictName(Verdict v);

// The human half of the startup line, e.g. "live online session: the offline-bit lock is not applied".
const char* VerdictReason(Verdict v);

// PURE, so both answers are constructible in a test without a game.
Verdict Classify(bool liveOnlineExempt, bool offlineLockHeld);

// The features that ask. The argument at each call site is what makes the gate greppable and what
// makes the startup block an enumeration of the real gates rather than a hand-kept list.
enum class Feature : uint8_t {
    EngineClock,        // clock take / timescale / pause bit
    PlayerPose,         // pose publish + the synthetic-owner stamp on player bodies
    GripWrites,         // trigger + hold-id writes into live hands
    DiscDrive,          // disc bind and possession stamping
    Dress,              // outfit writes into another user's identity
    PreRoll,
    Scrub,              // seek, which drives the clock and the apply pass
    AudioRate,          // sink rate / pause
    CursorClaim,        // SetCursorEnabled -- takes the mouse off the live spectator camera
    GameSpaceRegister,  // band-3 registration into the LIVE game space
    ReplayLoad,         // opening a demo at all
    Camera,             // view-state pose/FOV writes and the zeroed input gate (`cam` verbs)
    LodThrottle,        // CR15RemotePlayerCS+520, the remote-body LOD enable dword
    Stats,              // per-player / per-team CGameStats records, through the engine's appliers
    GoalEvents,         // the recorded goal message, through the engine's own two receive handlers.
                        // Its own enumerator because it RAISES a script event rather than writing a
                        // field: in a live session the graph that event starts is the one a real
                        // goal starts, and a replayed one would announce a goal that did not happen
    EntrantRelease,     // feeding the engine's own NSLobbyRemovingEntrant handler to take a replay
                        // entrant back out of the lobby.  Its own enumerator because it DESTROYS an
                        // engine object rather than overwriting a field: in a live session the
                        // entrants are real people and a leave would disconnect one of them.
    ViewerSeat,         // IDENT.SERVER_ASSIGN_TEAM on the LOCAL user -- it moves the person at the
                        // keyboard to spectator.  Was reachable only through `g.loaded`, which is
                        // one hop of indirection rather than a gate
    WatchdogBracket,    // the engine deadlock-watchdog refcount.  Holding it down disables the
                        // monitor for the rest of the process, so the bracket is a write even
                        // though it is balanced
    VoipTalkerSlots,    // VOIP.CONNECT into the live receiver's talker pool. The CLAIM only: the
                        // release stays ungated so anything we did claim is always given back, and
                        // an unclaimed pool has nothing to release
    BootModeBits,       // the CR15Game boot-into-multiplayer bit. Its launch policy already refuses
                        // every live-online launch; this is the second, non-derived term
    ReplyDrain,         // the stage-0 netgame-update detour body: admission sweeps and the swapper
                        // flush.  The detour has no removal path, so the gate has to be in the
                        // callback rather than at the install
    HttpRoutes,         // the four control routes on the GAME'S OWN HTTP listener.  Not an engine
                        // write at all -- an unauthenticated inbound surface, which in a live match
                        // can end the owner's recording and list his replay folder.  Its own
                        // enumerator so it is stated in the startup block beside everything else
                        // that goes inert (control.http_routes_live overrides it, default off)
    HostHold,           // holding one of the host gametype script's OWN netvars at a value that
                        // stops its round clock, so a 10-minute viewing outlives the host's 5-minute
                        // round.  Its own enumerator because the key is the HOST's, not the
                        // recording's: in a live match it would freeze a real round for everyone
    DiscTouchState,     // the disc's IsHeld bit and LastOwner dword beside the possession stamp.
                        // Its own enumerator because it is what every "who touched this" consumer
                        // reads: in a live match it would rewrite the real disc's history
    ComponentEvents,    // the recorded SR15NetComponentEvent rows, through the engine's own
                        // CR15NetGame::ComponentEventCB.  Its own enumerator rather than GoalEvents'
                        // because it raises into the LEVEL's script graphs: in a live session those
                        // graphs move tubes, doors and lights that real players are standing in
    GoalExplosion,      // evt_glb_scored_matanimation, the goal rig's own animation event.  Its own
                        // enumerator rather than GoalEvents': that one hands the engine a recorded
                        // message, this one raises a LEVEL script event with no message behind it,
                        // and in a live match it would explode a goal nobody scored in
    StunEvents,         // the recorded SR15StunPlayerMsg rows: the two punchable delegates plus the
                        // three fields on the victim's punchable record.  Its own enumerator rather
                        // than GoalEvents' because it WRITES A PLAYER'S STATE -- in a live match it
                        // would stun somebody nobody hit, and the engine's own timer would hold him
                        // there for the recorded two seconds
    LevelCycle,         // RELOAD-6: SESSION.CREATE_OFFLINE_MATCH from "in game" to open a second
                        // replay in a fresh level.  In a live match it would end the match for the
                        // person at the keyboard
    HostClockPin,       // DESIGN-HOST-SYNC P1: the host arena graph's own `game_timer`, pinned through
                        // the graph's own setter so the host's round never expires under a replay.
                        // In a live match it would stop a real round's clock for everyone
    DiscShellFeed,      // DESIGN-HOST-SYNC P4: forceSetFXState_* / OnNetTouchGrab delivered into the
                        // disc FX graph. In a live match it would light or clear the real disc's shell
    FoleyMix,           // AUDIO-CUTOFF-FIX: AK::SoundEngine::SetState on the foley-mix state group
                        // (audio/foleymix.h). In a live match it would swap the person's own foley
                        // mix under them
    Count,
};

const char* FeatureName(Feature f);

// As one decidable function rather than a condition spelled twice. The
// routes go up when this session is contained offline playback, or when the owner has said
// control.http_routes_live. `liveOverride` is that key, already read.
bool HttpRoutesAllowed(bool liveOverride);

// The in-process -offline -windowed (offlinepatch.h). It cannot ask Allowed(): it is
// decided at DllMain, before boot has classified anything, where Allowed() is NotDecided by design. So
// the rule is stated here from the two inputs that ARE fixed that early: a replay was requested (the
// explicit trigger, ECHOREPLAY_OPEN) and this is not the live-online exemption. Both must hold.
bool OfflinePatchAllowed(bool replayRequested, bool liveOnlineExempt);

// THE gate. Every engine-writing playback path calls this at its entry.
//
// The verdict is a property of the process, not of the feature, so today every feature gets the same
// answer; `f` names the caller for the log and keeps a per-feature carve-out possible without the
// call sites moving. A feature that would need its own rule must state it HERE, not at its site.
bool Allowed(Feature f);

Verdict Current();

// Boot publishes what it classified, once, and then prints the block. The LOCK half is read live
// from offlinelock -- it is the fact that answers correctly at every instant of a real process,
// including before boot reaches this line -- and the exemption half is boot's classification,
// which only boot can know.
void PublishBootFacts(bool liveOnlineExempt, bool offlineLockHeld);

// `live-safety: <feature>=inert(<reason>)`, one line per feature, emitted in a process that is not
// contained. In a contained process it prints one line saying so instead.
void LogStartupBlock();

// Test seam: drive both answers without a game. Reset() puts the module back to NotDecided.
void SetForTest(bool liveOnlineExempt, bool offlineLockHeld);
void ResetForTest();

}  // namespace replay::livesafety
