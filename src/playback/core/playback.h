#pragma once

// W2-B. The playback module's public surface: what other packets may ask it.
//
// ---------------------------------------------------------------------------------------------
// ★★ WHY THIS HEADER EXISTS AT ALL, and it is the same shape as W2-A's gather.h one round earlier.
//
// The residue battery lives in an anonymous-namespace Global inside playback_module.cpp, so
// UnavailableIds() was unreachable from outside src/playback. W2-F's panel could render the COUNT --
// ActiveModeLabel carries it verbatim -- but not the NAMED ids, and it refused to fake the difference:
// "flipping `consulted` to true with a count of zero would be exactly the false all-clear the
// classifier exists to make unrepresentable, so it stays false and the wording is 'we could not ask'."
//
// That refusal was right, and the fix belongs on this side. The unmapped primitives WITH THEIR
// REASONS are what a user needs to see when a replay is missing something; a count alone tells them
// something is wrong and not what. The number is deliberately not quoted here: the battery is filled
// by three registration passes now -- this file's unserved rows, W2-H's audio rows and W3-A's engine
// rows -- and a header that named a total would be wrong the first time one of them landed a row.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include "playback/core/apply.h"
#include "playback/core/windowbuild.h"

namespace replay {
namespace identity {
struct RecordedLoadout;
}

class IEntity;
class IPosePublisher;
class INetVar;
class IReplayCameraControl;

namespace playback {

// ---------------------------------------------------------------------------------------------
// ★★★ INV-AXIS, STATED ONCE, AS A PROPERTY A READER CAN CHECK.
//
//   EVERYTHING CROSSING IReplayTransport IS MICROSECONDS SINCE THE START OF PLAYBACK.
//   ABSOLUTE SOURCE TIME EXISTS ONLY INSIDE THE TRANSPORT.
//
// Concretely, and each clause is testable: Seek(0) is the first recorded frame; Seek(DurationUs())
// is the last; PlayheadUs() is in [0, DurationUs()] whenever a file is loaded; and
// `fraction * DurationUs()` is a directly seekable position, needing no correction from anybody.
//
// ★★ WHY THIS AXIS AND NOT ABSOLUTE. The transport is the boundary between FILE time and VIEWER
// time. The file is absolute and stays that way -- sourceTimeUs in S_TICK, t0Us in the manifest,
// AttributedSimTime at capture -- because that is where absolute belongs. Everything ABOVE the
// transport is a viewer surface: a scrub bar runs 0..duration, a bookmark means "4:12 into the
// match", a percentage seek multiplies a fraction by a length. Making those speak absolute would
// put a t0 subtraction in every one of them, and DurationUs() -- which was already 0-based and
// already correct -- would have had to change to match the things that were wrong.
//
// ★★★ WHAT IT COST TO NOT HAVE THIS. The interface published a 0-based LENGTH, an ABSOLUTE setter
// and no origin, so `fraction * DurationUs()` into Seek() was the only expression available and it
// was wrong by t0Us. Modelled on a real t0 of 1.7e15 us: every seek collapsed to the start of the
// capture and the playhead never drew at all, because it was classified against a span it fell
// outside. The scrub bar, seek-by-percentage and bookmark jumps were all non-functional on any real
// capture -- while passing every offline test, because a fixture written with t0Us = 0 makes the two
// axes the same number.
//
// ★ SO: NO TIME FIXTURE ON THIS PROJECT MAY USE A ZERO ORIGIN. A t0 of 0 is the degenerate case in
// which this invariant cannot fail, and 31 test cases plus a FakeTransport with no origin at all is
// what that bought. The conversion lives at exactly two lines -- Transport::Seek and
// Transport::PlayheadUs -- and nowhere else; a second one anywhere above them is a bug even when it
// computes the same answer today.
//
// AN OLD FILE CAN CARRY A STAMP BEFORE ITS OWN ORIGIN, and that is not a seek target at the time it
// records. Lobby-derived marks were filed on the TAP'S ARRIVAL clock instead of the capture axis
// until the writer fix; bytes already on disk cannot be rewritten, so every capture
// made before it still holds marks -- and the round row derived from them -- below t0Us. Seeking
// there yields a NEGATIVE playhead: outside the file span, outside every window, and the transport's
// own span clamp then lands the viewer on the first tick looking exactly as if that were the round.
// L3-RUN-6 S8 measured that as -17 025 384 us on a real capture, and it read as a seek fault.
//
// So the reader clamps it once, here, and SAYS SO: the round is kRoundPartialStart, which is what
// that bit already means -- the capture opened after this round began. Refusing the seek outright
// would be worse: the round IS in the file, only its recorded begin is not.
inline int64_t ClampRoundSeekUs(int64_t tUs, int64_t originUs, bool* partialStartOut) {
    if (tUs < originUs) {
        if (partialStartOut != nullptr) {
            *partialStartOut = true;
        }
        return originUs;
    }
    return tUs;
}
// ---------------------------------------------------------------------------------------------

// ★★★ THREE STATES, NOT TWO, AND THE THIRD IS THE ONE THAT MATTERS.
//
// `consulted` is false when the module has not initialised -- nobody has registered a primitive yet,
// so the honest answer is "we could not ask", not "nothing is unavailable". Those two render
// identically if the answer is a count alone, and an unavailable-count of zero reads as
// "everything is available", which is the false all-clear this whole structure exists to prevent.
//
// So a caller checks `consulted` FIRST. W2-F's panel already had this right on its own side and could
// not act on it because it had no way to ask; this is what makes the distinction reachable.
struct ResidueStatus {
    bool consulted = false;   // false => the module never initialised; the counts below mean nothing
    size_t registered = 0;
    size_t unavailable = 0;
};

ResidueStatus ResidueSnapshot();

// Fills `out` with up to `cap` ids of the primitives this build cannot run, and returns HOW MANY
// EXIST -- which may exceed `cap`, so a caller that renders `min(returned, cap)` and prints the
// total does not silently drop the rest. Ids are static strings owned by this module ("N-5", "N-13").
//
// Returns 0 when the module has not initialised, which is why ResidueSnapshot().consulted is the
// question to ask first: 0 here means "none" and "could not ask" alike, and only `consulted` separates
// them. The two calls together are a three-state answer; either alone is a two-state one.
size_t UnavailableResidueIds(const char** out, size_t cap);

// Why that primitive is unavailable, in the words the design uses. Null for an unknown id.
// Never an empty string for a row that is NOT Ready: "it is unavailable" is not a diagnosis. A Ready
// row answers with "" -- it has nothing to explain -- so null and empty are different answers and
// only the first means "no such id". The distinction became reachable when W2-H's and W3-A's real
// rows joined the battery; before that every registered row was Unmapped and the two could not part.
const char* ResidueDiagnostic(const char* id);

// What the applier currently writes through. Taken from ReplayServices every tick, not once at
// Init: the netvar surface is published from W2-A's first tick, after every Init has run, so an
// Init-time binding held null for the life of the process and every netvar write counted refused.
struct BindingStatus {
    IEntity* entity = nullptr;
    IPosePublisher* pose = nullptr;
    INetVar* netvar = nullptr;
    // W1-E publishes one entity singleton at Init and binds it to a game space later, so the
    // pointer never changes while Available() does. Body handles are bound on that edge too.
    bool entity_available = false;
    uint64_t rebinds = 0;   // times the tick found a pointer, the availability or the epoch changed
    // ★ The signal the other two could not carry (AUDIT_WIRING_A F-A). W1-E's entity is a
    // process-lifetime singleton and re-binds without unbinding, so a rebuilt game space changes
    // neither the pointer nor Available(); IEntity::BindEpochNow changes on exactly that edge, and
    // every user index, INV-OWN1 registration and PlayerHandle we hold was issued by the binding
    // it names.
    IEntity::BindEpoch entity_bind_state = IEntity::BindEpoch::NotAvailable;
    uint64_t entity_bind_epoch = 0;   // meaningless unless entity_bind_state is Ok
    // INV-OWN1's registry, which is re-armed from the engine on every one of those edges. `local_*`
    // is the id it refuses against: unknown means the is-local-user arm refuses nobody this
    // session rather than refusing a fabricated 0 (OPEN-1).
    uint64_t owner_resets = 0;
    uint32_t entrants = 0;
    bool local_user_known = false;
    uint32_t local_user_id = 0;
    // ★ WHY the arm is off, and for how long it has been off. The engine can be
    // bound and readable and still unable to answer -- Available() does not depend on the
    // broadcaster pool -- so local-unknown is a WINDOW that the tick re-asks out of, not a state a
    // session enters once and keeps. `local_unknown_ticks` counts the ticks spent in it since the
    // last arm; it stops when `local_user_known` turns true.
    IEntity::LocalUser local_user_state = IEntity::LocalUser::NotAvailable;
    uint64_t local_unknown_ticks = 0;
};

BindingStatus BindingSnapshot();

// ---------------------------------------------------------------------------------------------
// What this module has DRIVEN on the surfaces other packets declared: SetStepUs, ClearSyntheticOwner,
// TrimHistory, SetLodThrottle and SetAudioPins. Counts of CALLS, not of the states those calls put
// the engine in -- a state can be reached without the call, and it is the call that was missing.
struct SeamStatus {
    uint64_t clock_steps_pushed;   // StepResult.commanded_step_us written to IReplayClock
    uint64_t owner_releases;       // tracks whose synthetic-owner claim was handed back
    uint64_t history_trims;        // per-CI TrimHistory calls, one per bound body per window build
    uint64_t audio_pins_pushed;    // keyframes that carried AUDIO_PINS and were pinned from
    uint64_t audio_pins_absent;    // keyframes resolved with no AUDIO_PINS block to pin from
    uint64_t audio_pins_refused;   // keyframes whose AUDIO_PINS block this build could not read
    uint64_t lod_throttle_writes;
    bool     lod_throttle_suppressed;
};

SeamStatus SeamSnapshot();

// Identity dressing for the loaded file's roster (W2-C's DressRoster, driven from Load). It is
// gated on a validated entity surface, and while the gate is shut the roster is DEFERRED and
// counted -- `pending` stays true and `deferred` grows by one per tick -- never skipped.
struct IdentityStatus {
    bool pending = false;     // a roster is loaded and has not been dressed yet
    uint32_t players = 0;     // the loaded manifest's roster size
    uint32_t dressed = 0;
    uint32_t failed = 0;
    uint32_t deferred = 0;    // ticks (plus the load itself) on which the gate refused
    // Players whose ADMISSION is outstanding as of the last pass -- the engine answered a documented
    // deferral (ReplyWrongPhase and its siblings), not a decision. Separate from `deferred` above,
    // which counts TICKS the session gate held, and from `failed`, which a deferred player is also
    // counted in: it is the number that decides whether the roster is still being retried.
    uint32_t entrant_deferred = 0;
    uint32_t passes = 0;      // dress passes run for this load, retries included
    // The manifest rows the last pass dressed, as one number: what the roster-change test compares
    // against. It is a digest and not a count because a count cannot tell five rows out and five
    // different rows in apart, and because the count this module holds beside it (`players`) is the
    // DRESSABLE one and comparing the two re-armed the dress every frame.
    uint64_t roster_digest = 0;
    bool gave_up = false;     // the deferral bound expired; these avatars are as dressed as they get
    // What the gate is waiting on, or null when it is not waiting. A string literal with static
    // lifetime: it is the same pointer every tick the same blocker holds, which is what rate-limits
    // the deferral line to one per distinct reason.
    const char* defer_why = nullptr;
    // Ownership, which is NOT dressing. `mapped` counts players whose capture slot resolved to a
    // live engine user index -- the number PlayerTrack::user_id carries and INV-OWN1 validates.
    // A player can be mapped without being dressed (its avatar is wrong but its body moves) and
    // it can never be dressed without being mapped, so one number cannot report both.
    uint32_t mapped = 0;
    uint32_t owner_refused = 0;   // mapped, then refused by OwnerGuard: an INV-OWN1 finding
    uint32_t released = 0;        // replay users the engine took back on Unload
    uint32_t release_refused = 0;
};

IdentityStatus IdentitySnapshot();

// GIVING UP ON THE DRESS IS NOT TERMINAL.  Load, a seek and a mid-file roster change re-arm it
// themselves; this is the same door for a caller that knows the question changed -- a console verb,
// or a test driving the re-arm without one.  False means there was nothing to re-arm.
// `force` runs the write ladder over players this load already dressed too. Off (the console verb's
// default), those slots are skipped: re-dressing one attaches a second profile document over the
// first and the engine abandons the first, so the cost is memory that is never given back.
bool ReArmIdentityDress(bool force = false);

// The actor-index census, written to the log: every actor the engine says carries an EchoUnit, an
// ncaModel or a CR15Body, beside the actor this plugin resolves for each user slot. Returns the
// number of lines written, 0 when there is no entity surface to ask. One-shot and hand-driven: it
// walks three whole component tables and must never be put on a frame path.
uint32_t LogComponentCensus();

// What the last tick actually presented from, beside where the transport says the user is.
// S10.4.1a's bias is a subtraction the viewer is supposed to perceive and nothing else is supposed
// to see, so it needs one place that reports both numbers -- otherwise the only way to tell a
// correct bias from a wrong one is to watch the picture.
struct PresentSnapshot {
    int64_t transport_us = 0;   // the transport's own display playhead, unbiased
    int64_t presented_us = 0;   // what ApplyStateAt was given
    int64_t bias_us = 0;        // scene-time subtraction asked for; flat across rates by design
};

PresentSnapshot LastPresent();

// The last tick's apply, so a wiring test can read what was published rather than infer it from a
// pose sink's call count -- the count says something came out, this says what the applier decided
// about every track, including the ones it refused.
ApplyStats ApplyStatsSnapshot();

// The last window BUILD, mirroring ApplyStatsSnapshot: the applier's snapshot says what was done
// with the tracks this window holds, and this says what the build put in them.  body_samples,
// decode_failures and the two identity counters had no reader outside this module, so "were any
// body samples decoded at all?" could only be answered by inference from a pose sink's call count.
BuildStats BuildStatsSnapshot();

// ---------------------------------------------------------------------------------------------
// S12.5.1's SCRIPTED CAMERA TRACK -- what this module did with it on the load that is running.
//
// S5.4.7's ORACLE table names "our own transport" as the track's source and this module is it, so
// the track lives here rather than in a stream the file would have to carry before the image gate
// could run at all. The bracket it drives is W2-E's: BeginOracleTrack() takes the camera and
// SetMode() refuses with OracleTrackActive until EndOracleTrack() (camera_control.h:42-44).
//
// `refusal` is a NAME, never a bare false. A track that did not enter because no pose sink is
// installed and one that did not enter because another track holds the camera are different
// facts, and an operator does different things with the two.
struct OracleTrackStatus {
    bool        asked   = false;   // config playback.oracle_track was true at Load
    bool        active  = false;   // the bracket is open: this module holds the camera
    uint64_t    begins  = 0;       // BeginOracleTrack() calls that succeeded
    uint64_t    ends    = 0;       // EndOracleTrack() calls made
    uint64_t    poses   = 0;       // frames the track drove
    const char* refusal = "";      // why it is not active, by name; empty while it is
};
OracleTrackStatus OracleTrackSnapshot();

// The track itself, as the pure function of source time the oracle's precondition asks for
// ("camera pose is an exact function of sim time"). Exposed so both passes -- and a test -- can
// evaluate it without a camera behind them; the module calls this same function per frame.
// `sourceTimeUs` is microseconds since the first recorded frame, so a track is identical across
// two passes over one file whatever wall clock or t0 either pass ran at.
void OracleTrackPoseAt(int64_t sourceTimeUs, float pos[3], float quat[4], float* fovYRadians);

// The camera control the track brackets. Null restores camera_control::Get(), which is what every
// shipping path uses; nothing outside tests/ installs one (the shape W2-F's panel uses at
// panel.cpp:1679 and W2-E's SetEngineForTest uses for its rig).
void SetCameraControlForTest(IReplayCameraControl*);

// THE SESSION BOUNDARY. The dress residue deliberately outlives Unload -- that is what lets a
// second load re-bind its entrants instead of admitting a second body -- so only process start
// clears it. A test case is a fresh session but not a fresh process, and a fixture that leaves its
// entrants behind makes the NEXT case's first load an adoption. Call this where a process would
// have started.
void ForgetSessionEntrantsForTest();

// One pass of the leave feed, the way the stage-3 tail runs it. A rig that drives Load/Unload without
// an engine update has no other way to reach it, and a test that skipped it would be testing a system
// in which the entrants never leave.
void ServiceLeaveFeedForTest();

// AUD SLOTMAP S6. A slot QUEUED into the provisioner's table without going through the dress pass,
// so its mapping is assigned by a later Pump -- the second producer the shipped tree does not yet
// have (PawnProvisioner::Request has no caller in src/, and that is a five-part conjunction across
// two modules, not a property this file can hold). It is what makes the per-tick slot-map refresh
// a testable claim instead of an argument. Returns false if no file is loaded or the table refuses.
bool RequestPawnSlotForTest(uint16_t recorded);

// The recorded-loadout provider slot AS THE ARMED PROVISIONER HOLDS IT, not as this module meant to
// fill it. Null is the shape of "unit-tested but unwired": PawnProvisioner::DriveLoadout answers
// RecordingNotAsked for every entrant and no launch can tell that from a file with no block in it.
// Calling the returned pointer asks the same source the census prints, so a test can assert the two
// routes read one recording.
using RecordedLoadoutProviderFn = bool (*)(uint32_t recordedSlot,
                                           ::replay::identity::RecordedLoadout& out,
                                           const char** refusal);
RecordedLoadoutProviderFn ArmedRecordedLoadoutProviderForTest();

// THE ACTOR-HOP SURFACE AS THE ARMED PROVISIONER HOLDS IT. Null is the shape of "admitted but
// never driven": PawnProvisioner::Settle takes an entrant with no surface straight to Provisioned
// and never enters AdvanceSlot, which is the only place a hop is advanced or a loadout dressed
// from -- so a null here is a build whose census says `provisioned N` about pawns no hop ever
// read. Void because the surface's type lives in an engine header this one does not include; the
// only question asked of it here is whether the arm bound one, and which one.
const void* ArmedActorHopsForTest();

// Which body-route levers the armed provisioner holds, as the module's config read left them. The
// pair pawn.bot_body / pawn.equipment_recv is coupled at the arm, so the declared defaults alone
// do not say what a launch with no ECHOREPLAY_* variable gets; this does.
struct ArmedBodyRoute {
    bool equipmentRecv;
    bool sourceLoadoutBeforeAdmit;
    bool botBodySourceLoadout;
    bool participateBackstop;
};
ArmedBodyRoute ArmedBodyRouteForTest();

// What the admission wrapper the dress pass is handed answers for the inner identity's hop
// surface. IIdentity defaults ActorHops() to nullptr, so a wrapper that drops the forward reports
// "this build bound no hop read" for a build that bound every one -- silently, and only for the
// callers holding the wrapper. Pointer-identical to `inner.ActorHops()` or the forward is gone.
const void* ProvisionedIdentityHopsForTest(::replay::IIdentity& inner);

// ★★ THE MODULE SEAM WINDOW-DECODE-1 HAD NO WAY TO ASSERT. The window slot bookkeeping -- which
// buffer is current, which centre it holds, who decoded it -- lives in the module translation unit,
// so every suite that links the LIBRARY is blind to it, and the blocker that shipped (an adoption
// that set the centre to SIZE_MAX and stopped every later prefetch) sits exactly there.
// playback_armtest compiles the module TU, and these are what it drives the boundary walk against.
struct WindowDecodeStats {
    uint64_t tickDecodes;        // windows decoded ON the tick: the defect itself
    uint64_t adoptions;          // windows taken from the worker instead
    uint64_t prefetchLate;       // the playhead wanted one the worker had not landed
    uint64_t prefetchStale;      // results dropped: wrong load, or a centre nobody wants
    uint64_t prefetchFallbacks;  // boundaries the tick decoded because coverage was running out
    size_t windowCentre;         // the chapter `cur` is centred on
    size_t windowsAllocated;     // slots held: the memory bound, never above three
    const void* current;         // WHICH storage `cur` names, for the abandoned-slot case
    bool asyncOn;
};
WindowDecodeStats WindowDecodeStatsForTest();

}  // namespace playback
}  // namespace replay
