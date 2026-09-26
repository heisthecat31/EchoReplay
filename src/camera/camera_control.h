#pragma once

// W2-E. Selecting a camera MODE, which B.7 does not expose and the panel needs.
//
// ---------------------------------------------------------------------------------------------
// ★ WHY THIS INTERFACE EXISTS, because a frozen contract said it would not be needed.
//
// B.7's text: "Mode selection is not on this interface because it does not cross a packet
// boundary." It does. The panel is W2-F and the console is W2-G, and DESIGN S13's W2-E row says
// "the modes drive from the panel". W2-F shipped no buttons rather than dead ones and stated the
// gap instead of omitting it; this is the seam that closes it.
//
// IReplayCamera stays exactly as B.7 froze it. It is a POSE SINK -- the view-state write point,
// now the retained fallback (S8.1.1) -- and mode selection is a different mechanism on a
// different object: the shipping spectator rig, CR15NetSpectatorCameraCS. Putting a selector on
// the fallback's interface would tie the primary path to the thing it replaced. This mirrors
// what B.11 did when five Wave-2 packets needed reads no frozen interface exposed.
//
// ---------------------------------------------------------------------------------------------
// THE MECHANISM IS THE ENGINE'S OWN. CAM.SET_CAMERA_MODE_IMPL (sub_140D44F70) writes, for the
// instance whose index equals SPECTATOR.CS_LOCAL_INDEX_OFF:
//
//     ci + SPECTATOR.CI_MODE_OFF (0x3C) = mode
//     ci + 0x58                         = 0
//     ci + 0x26                         = a packed (team, teamSlot) dword derived from num
//     ci + 0x32                         = min(num, perSetCameraCount - 1)
//
// This interface is that call and nothing more. It invents no mode, no blending and no state
// machine of its own -- the engine already ships all three, plus camera collision, an
// auto-director and an in-game camera editor.
//
// ---------------------------------------------------------------------------------------------
// ★★ THE ONE HAZARD, MADE UNREPRESENTABLE RATHER THAN DOCUMENTED.
//
// IReplayCamera::SetPose reaches CAM.SETCAMERA_API_IMPL, whose second store is
// "mov dword ptr [rcx+3Ch], 6" -- it FORCES mode 6 (api) on every single call, and
// CAM.SETCAMERA_API_STORES has guarded those bytes since W1-H. So a per-frame pose write and a
// human mode selection are not two features; they are two writers of ci+0x3C, and the pose
// writer wins every frame. Selecting Free while anything feeds SetPose would present as a
// camera that ignores the button -- a bug that looks like a UI fault and is not one.
//
// The two are therefore exclusive by construction: BeginOracleTrack() takes the camera and
// SetMode() refuses with OracleTrackActive until EndOracleTrack(). A caller cannot express the
// broken combination. Same shape as W2-B's ApplyStateAt taking no Discontinuity parameter: the
// defect is something an implementation has to work to reach, not something a test catches after.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

#include "engine/camera_writepoint.h"
#include "engine/camera_writepoint_poseowner.h"

namespace replay {

// SR15NetSpectatorCameraCI::EMode. Read out of CAM.HTTP_CAMERA_MODE_HANDLER, which string-matches
// "pov","follow","level","disc","side","free","api" against 0..6 in that order -- so these are the
// engine's values and the engine's names, not ours. W1-H S9 item 7 recorded 0..4 as R7's
// unverified table; CAM.MODE_FOLLOW / _LEVEL / _DISC / _SIDE now carry them with disp8 anchors.
enum class CameraMode : int32_t {
    Pov    = 0,   // through a player's eyes
    Follow = 1,   // chase cam, with the engine's own smoothing and collision
    Level  = 2,   // the authored camera sets in mp_spectator_cameras.json
    Disc   = 3,   // tracks the disc
    Side   = 4,   // side-field camera
    Free   = 5,   // 6DOF free-fly -- the owner's "freely move around", already built and tuned
    Api    = 6,   // pose driven externally; RESERVED for the oracle track (S8.1.5)
};

// Why a selection did not happen. "It refused" is not a diagnosis, so every reachable refusal has
// its own value and W2-G's cam command prints the name.
enum class ControlRefusal : uint8_t {
    None = 0,
    NotAvailable,           // the rig did not check out; camera::Stats::refusal carries the reason
    ModeReservedForOracle,  // Api is not selectable by a human
    OracleTrackActive,      // a deterministic track owns the camera; EndOracleTrack() first
    NumOutOfRange,          // num outside what this mode accepts
    UnknownModeName,        // ModeFromName saw a string the engine does not define
    PoseOwnerConflict,      // ★★ the S8.1.7 write point already owns the pose. It clears
                            //   viewstate+0x9C, the gate this camera's integrator runs on, so the
                            //   two cannot both be bound; camera_writepoint_poseowner.h carries
                            //   the rule and Diagnostic() names the holder.
    // Appended by ARCH-17 S3.2, so every value above keeps its number.
    PoseNotOwned,           // the pose-owner transfer did not complete; LastPoseOwnerConflict()
                            //   names the holder. Distinct from PoseOwnerConflict, which is the
                            //   BIND-time refusal: this one is command-time, and an operator does
                            //   different things with the two.
    OracleTrackHolds,       // OracleTrackActive(). The commanded family is refused so the oracle's
                            //   "exact function of source time" survives a human typing `cam move`
                            //   mid-track. Distinct from OracleTrackActive, which is SetMode's.
    // Appended by ARCH-19 R4, so every value above keeps its number. NotAvailable stays exactly
    // what it was -- the rig did not check out -- and stops being the drawer everything else is
    // put in.
    PoseUnreadable,       // GetViewPose() != Ok. ViewReadRefusal() carries the sentence and the
                          //   four-word ViewPoseRead the reason.
    WritePointRefused,    // no write point installed in this image, or camera::Create returned
                          //   null. camera::RefusalName's twenty values carry which
                          //   (camera_writepoint.h:81-137).
    FacingDegenerate,     // the facing a yaw would be added to is within 0.1 deg of vertical, or
                          //   `cam look at` resolved a target the camera is standing inside.
                          //   A clamp would silently change which way the camera points.
    PoseNotFinite,        // a composed destination component is inf or nan. Unreachable through
                          //   the parser today, which refuses both; it is here because a defensive
                          //   arm that refuses under a borrowed name teaches the wrong thing.
    LookAtUnresolvable,   // ARCH-19 R3. The selector named no subject this tick.
    // Appended by ARCH-23 S14.11, so every value above keeps its number.
    FreeCamHoldsNode,     // SPECTATOR.CI_FREECAM_ACTIVE_OFF is set: the stock free camera is
                          //   sourcing CCameraNode[0] from view-state 0. CAM.SET_CAMERA_MODE_IMPL
                          //   is not one of the eleven callers of the free-cam EXIT, so a mode set
                          //   here would write ci+0x3C and leave ci+0x274 alone -- camera flying,
                          //   listener switched, panel reporting the new mode. The engine's own
                          //   SwitchTo* arms are exit-then-set and that exit stores through its
                          //   a3, so calling it outside HandleInput is not ours to do.
};
const char* ControlRefusalName(ControlRefusal);

// The numbers above, pinned, on the RigProbe precedent below: W2-G's `cam` command prints
// ControlRefusalName and a probe row scores on the word, so an INSERTION anywhere in this enum
// must be a compile error here rather than a silent re-labelling of somebody else's refusal.
static_assert(static_cast<uint8_t>(ControlRefusal::None) == 0, "ControlRefusal numbers are stable");
static_assert(static_cast<uint8_t>(ControlRefusal::NotAvailable) == 1, "ControlRefusal numbers are stable");
static_assert(static_cast<uint8_t>(ControlRefusal::ModeReservedForOracle) == 2, "ControlRefusal numbers are stable");
static_assert(static_cast<uint8_t>(ControlRefusal::OracleTrackActive) == 3, "ControlRefusal numbers are stable");
static_assert(static_cast<uint8_t>(ControlRefusal::NumOutOfRange) == 4, "ControlRefusal numbers are stable");
static_assert(static_cast<uint8_t>(ControlRefusal::UnknownModeName) == 5, "ControlRefusal numbers are stable");
static_assert(static_cast<uint8_t>(ControlRefusal::PoseOwnerConflict) == 6, "ControlRefusal numbers are stable");
static_assert(static_cast<uint8_t>(ControlRefusal::PoseNotOwned) == 7, "ControlRefusal numbers are stable");
static_assert(static_cast<uint8_t>(ControlRefusal::OracleTrackHolds) == 8, "ControlRefusal numbers are stable");
static_assert(static_cast<uint8_t>(ControlRefusal::PoseUnreadable) == 9, "ControlRefusal numbers are stable");
static_assert(static_cast<uint8_t>(ControlRefusal::WritePointRefused) == 10, "ControlRefusal numbers are stable");
static_assert(static_cast<uint8_t>(ControlRefusal::FacingDegenerate) == 11, "ControlRefusal numbers are stable");
static_assert(static_cast<uint8_t>(ControlRefusal::PoseNotFinite) == 12, "ControlRefusal numbers are stable");
static_assert(static_cast<uint8_t>(ControlRefusal::LookAtUnresolvable) == 13, "ControlRefusal numbers are stable");
static_assert(static_cast<uint8_t>(ControlRefusal::FreeCamHoldsNode) == 14, "ControlRefusal numbers are stable");

// Which of the engine's two pose sources CAM.SPECTATOR_UPDATE is reading this frame. It is a
// read of SPECTATOR.CI_FREECAM_ACTIVE_OFF (ci+0x274) and not a guess: the engine tests exactly
// that field to choose, at 0x140D6AACB.
enum class ViewPoseSource : uint8_t {
    ViewState,       // ci+0x274 != 0. The stock free camera's arm: the pose is view-state 0's, and
                     // the engine's wall-clock integrator is what moves it.
    CameraInstance,  // ci+0x274 == 0. The pose is the CI's, written by CAM.SETCAMERA_API_IMPL.
};
const char* ViewPoseSourceName(ViewPoseSource);

// The view pose as the ENGINE holds it. Every field is meaningless unless GetViewPose returned Ok;
// a refusal leaves the whole struct blank, so no consumer can score a refusal's zeros as a pose.
struct ViewPose {
    float          pos[3]  = {0.0f, 0.0f, 0.0f};
    float          quat[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // xyzw, as the engine stores it
    float          fovYRadians = 0.0f;
    int32_t        mode   = -1;                         // ci+0x3C, the engine's own EMode
    ViewPoseSource source = ViewPoseSource::CameraInstance;

    // viewstate[0]+0x9C, the gate S8.1.2 rests on. Read even on the CI arm, because "armed" is the
    // fact ruling 4b names -- but its own two states, because a read that could not reach the
    // view-state must not report the gate as 0 (disarmed), which is the opposite claim.
    //
    // `Known` is also clear when the read reached the view-state on a build whose
    // VIEWSTATE.INPUT_GATE_FREE_SITE no longer matches: that site is the only V-PRIMARY evidence
    // for +0x9C, and the pose displacements are carried by a different window, so a drifted image
    // can hand back a valid pose and a meaningless gate. Unknown, not 0.
    bool           integratorArmedKnown = false;
    int32_t        integratorArmed      = 0;
};

// ★ WHICH OF THE TWO REGIMES THE SESSION IS IN (ARCH-17 S2). There are only two and a session is
// always in exactly one: the pose is either the engine's to integrate or ours to place.
enum class PoseRegime : uint8_t {
    Free,        // PoseOwner::StockFreeCam. The engine's integrator has the pose and flies it on
                 // wall time; viewstate+0x9C is armed (RULING 4b). This is the state at bind.
    Commanded,   // PoseOwner::WritePoint. We have it, viewstate+0x9C is cleared and re-asserted,
                 // and the pose is exactly where the last command put it.
    Unavailable, // no rig bound, or the arbiter section is PoseOwner::Unavailable. Never inferred
                 // from a mode number: "the CI says 6" is not the claim "we own the pose".
};
const char* PoseRegimeName(PoseRegime);

// What a `cam move` / `cam look` line asks for, after the parser and before the engine.
struct CommandedPose {
    // What to change. Either or both; a command that changes neither writes the pose back
    // unchanged, which is what `cam move 0 0 0` means and not an error.
    bool  setPosition    = false;
    bool  setOrientation = false;

    float pos[3]  = {0.0f, 0.0f, 0.0f};   // metres, world space, viewstate+0x60's frame
    float yawDeg = 0.0f, pitchDeg = 0.0f; // orientation as the surfaces express it

    // false = pos/yaw/pitch are DELTAS on the engine's current pose; true = absolute. A flag and
    // never a sentinel: (0,0,0) is a real absolute destination and a real no-op delta, and they
    // are different commands.
    bool  absolute = false;

    // `cam look at <n>`: aim at the subject the selector names. The engine's own 1-based player
    // selector, the same argument SetMode takes for Pov/Follow.
    bool    lookAt         = false;
    int32_t lookAtSelector = 0;
};

class IReplayCameraControl {
public:
    // Player selector bound. num is 1-based over two teams of five.
    // constexpr, not const: a static const member with an in-class initialiser is still not
    // implicitly inline in C++20, so a consumer that odr-uses it -- std::clamp binds it to a
    // const int&, which W2-F's index control naturally would -- gets an unresolved external
    // rather than a compile error, at W2-F's link step and not at ours.
    static constexpr int32_t kPlayerSelectorMax = 10;

    // Selects a HUMAN mode. Returns false and records a refusal otherwise.
    //
    // What num means is per mode, and it is the engine's own argument rather than our
    // reinterpretation of one:
    //   Pov, Follow   a 1-based player selector, 0..10. CAM.SET_CAMERA_MODE_IMPL packs it as
    //                 low word = team, high word = team slot: num 1..5 -> team 1 slot num-1,
    //                 num 6..10 -> team 0 slot num-6. Those are the same two fields W2-C
    //                 registered as IDENT.USER_TEAM_SHIFT and IDENT.USER_TEAMSLOT_SHIFT.
    //                 num == 0 makes the engine write the constant 0x40000, whose meaning is
    //                 [U]; it is accepted and passed through unaltered rather than guessed at.
    //   Level         a camera index inside the active set. The engine clamps it itself to
    //                 min(num, perSetCameraCount - 1). This interface does NOT pre-clamp, so
    //                 what the engine did stays observable through ObservedMode and the CI.
    //   Disc, Side, Free   ignored by the engine; pass 0.
    virtual bool SetMode(CameraMode mode, int32_t num) = 0;

    // What we last asked for, and what the engine's CI actually holds right now.
    //
    // ★ Deliberately two calls. W1-D found the panel draws into the GUI frame opened by the
    // PREVIOUS engine frame (S8.5.2), so a panel that renders ObservedMode() shows a
    // one-frame-stale answer and its buttons feel wrong to drag. A panel renders
    // RequestedMode() -- the value it published. ObservedMode() is for diagnostics and for
    // noticing that the engine's own state machine moved the camera out from under us, which
    // UpdateAutoCamera and the shipped SwitchTo* input bindings both do.
    virtual CameraMode RequestedMode() const = 0;
    virtual bool       ObservedMode(CameraMode* out) const = 0;  // false if the CI is unreachable

    // ★ NOT ON THIS INTERFACE, AND THE ABSENCE IS DELIBERATE: a level-camera COUNT, so the panel
    // could bound its index control. Reading it means calling sub_140CE6D60 on cs+128 and then
    // three offsets (56, 64, 48) that no packet has registered, and an unguarded call into game
    // code is what S4.4 refuses. Returning a constant 0 would be worse -- W2-F cannot tell an
    // empty set from an unimplemented bound. So the engine's own clamp stands, the panel ships a
    // stepper rather than a slider, and ObservedSelector() shows where the engine actually
    // landed. Two facts would close it; they are parked, not skipped.
    //
    // What the engine did with num, read back from the CI: the camera index for Level
    // (SPECTATOR.CI_CAMERA_INDEX_OFF, after the engine's own clamp) and the packed
    // (team, teamSlot) selector for Pov and Follow (SPECTATOR.CI_SELECTOR_PACKED_OFF). This is
    // the observable that makes the clamp visible instead of silent. Either pointer may be null.
    virtual bool ObservedSelector(int32_t* packedOut, uint16_t* cameraIndexOut) const = 0;

    // SPECTATOR.CI_CAMERA_SET_OFF -- a plain guarded read, so the panel can at least label which
    // set it is stepping through even though the set's SIZE is not reachable (above).
    virtual bool ActiveLevelCameraSet(uint16_t* out) const = 0;

    // ARCH-23 S14.11. SPECTATOR.CI_FREECAM_ACTIVE_OFF, raw, on its own -- the gate SetMode now
    // refuses on and the census field `freecam`. GetViewPose reports the same bit as `source`,
    // but only when it can also produce a pose: on the free arm a refused view-state read leaves
    // the census with no answer for a flag the CI alone already settled. Defaulted false for the
    // reason GetViewPose is defaulted -- W2-F's panel test implements this interface with no
    // camera behind it -- and false means COULD NOT ASK, never "not in free cam".
    virtual bool ObservedFreeCam(int32_t* out) const { (void)out; return false; }

    // ★★ THE VIEW POSE, published because RULING 4b CANNOT BE ASKED WITHOUT IT.
    //
    // S8.1.2 says the stock free camera keeps moving while playback is paused: viewstate+0x9C stays
    // armed and the engine's own integrator runs on wall time. The observer verifies that by
    // sampling the pose twice while paused -- and until now nothing published a pose READ.
    // IReplayCamera is a pose SINK (SetPose only, B.7 frozen), and camera::Observe() answers only
    // while the W1-H write point is bound, which is precisely the state ruling 4b excludes.
    //
    // The pose comes back from the source the ENGINE is using (`source`), read through this
    // control's own already-validated resolve chain -- the same LocalInstance() SetMode uses, with
    // the same Readable() discipline -- and, on the free camera's arm, through W1-H's view-state
    // resolve rather than a second derivation of it. It never falls back from one source to the
    // other: publishing the frozen CI pose while the engine renders the view-state's is exactly how
    // a flying camera would be recorded as stationary.
    //
    // Defaulted rather than pure, and the default is a refusal: W2-F's panel test implements this
    // interface for a surface that has no camera behind it at all, and a pure virtual would break
    // a packet that has no business knowing about a pose read. An implementation that has not
    // written this one has no pose to publish, so NotBound is the truthful answer rather than a
    // convenient one.
    //
    // The refusals are four words and they are four facts. NotBound is THIS control's: nothing is
    // bound, or nothing implements the read. NoViewState comes back up from W1-H: the plugin
    // asked and the ENGINE has no CR15Game, no view-state array or no view-state 0 yet. The two
    // used to share a word, which told a consumer the plugin was idle when the plugin had asked.
    virtual camera::ViewPoseRead GetViewPose(ViewPose* out) const {
        if (out) *out = ViewPose{};
        return camera::ViewPoseRead::NotBound;
    }

    // ★ THE COMMANDED POSE (ARCH-17 S3.2). PLACES the camera; never integrates it (FC-2): one
    // teleport plus an orientation, evaluated once per command, so S8.1.2's deleted integrator
    // does not re-enter under a command's name.
    //
    // Returns false and records a ControlRefusal on: OracleTrackActive() (OracleTrackHolds), a
    // pose-owner transfer that did not complete (PoseNotOwned, holder named), a write point that
    // did not check out, or an unreadable current pose (both NotAvailable, with Diagnostic()
    // carrying the reason). It NEVER half-applies: position and orientation land in one SetPose or
    // neither does.
    //
    // `delta` semantics are the command's and are resolved HERE, not in the parser: a relative
    // move must be resolved against the pose the ENGINE holds this frame, which only GetViewPose
    // can read, and a parser that pre-resolved it would resolve against a pose one command stale.
    //
    // ★ Defaulted rather than pure for the reason GetViewPose is: W2-F's panel test implements
    // this interface for a surface with no camera behind it, and a control that cannot write a
    // pose has none to command. The default is a refusal, never a convenient success.
    virtual bool CommandPose(const CommandedPose&) { return false; }

    // Which regime the session is in RIGHT NOW. A panel renders it; a probe scores it. Never
    // inferred from ObservedMode(): the engine's own state machine moves the mode out from under
    // us (UpdateAutoCamera, the shipped SwitchTo* bindings).
    virtual PoseRegime Regime() const { return PoseRegime::Unavailable; }

    // Hand the pose back to the stock free camera, so the human's "freely move around" resumes.
    // Idempotent: in regime Free or Unavailable it is a success that did nothing, not a refusal,
    // because `cam release` is what a confused operator types and it must always be safe.
    virtual bool ReleasePose() { return true; }

    // The oracle's bracket. Between these the camera belongs to a deterministic track fed
    // through IReplayCamera::SetPose (which forces mode Api), and SetMode refuses.
    virtual bool BeginOracleTrack() = 0;
    virtual void EndOracleTrack() = 0;
    virtual bool OracleTrackActive() const = 0;

    virtual bool           Available() const = 0;
    virtual ControlRefusal LastRefusal() const = 0;

protected:
    // Never deleted through this: the instance has static storage duration. Same shape W1-C used
    // for ILog, W1-D for ITick and W1-H for IReplayCamera.
    ~IReplayCameraControl() = default;
};

namespace camera_control {

// ★ The engine's own seven names, so W2-G's cam parser and the panel's labels have ONE source and
// it is the binary. A second hand-written table is how two surfaces end up disagreeing about what
// "side" means.
const char* ModeName(CameraMode);
bool        ModeFromName(const char* name, CameraMode* out);

// Api is not selectable; everything else is. The panel greys rather than omits, so a reserved
// mode reads as reserved instead of missing.
bool        IsSelectableByHuman(CameraMode);

// THE SPECTATOR-SYMBOL REFERENCE CENSUS, COMMITTED. Routed to this packet by
// ARCH-15 S7 item 2: the 17-site count was published in three documents and the sites themselves in
// none, so nobody could close, count or re-check the thirteen unattributed ones without redoing the
// scan. They are enumerated here, in the packet that owns the consumer, so that attributing one is
// an edit to a comment on a line that already exists.
//
// Every .text reference to CAM.SPECTATOR_SYMBOL ("fOlPrEfIxR14NetSpectatorCamera", the string at
// 0x1416D800F): 17 sites, four attributed, thirteen not. Each value is the address of the
// instruction that takes the symbol's address (the lea), which is why the attributed rows differ
// from the CALLER VAs the design quotes for chains 1 and 2 (0x140195FC8 and 0x140165641).
//
// Derived offline from the reference image's database by data-xref query on that string -- not
// copied from an audit's prose, which gave the total only. Image md5 caa05ee3bca2f5df0809994e93e2cc05.
// Ordering is ascending and strict; tests/camera asserts the total, the split and the ordering, so
// an addition that does not move the counts cannot land quietly.
inline constexpr uint64_t kSpectatorSymbolRefSites[] = {
    0x1401B3525ull,   // ATTRIBUTED 3: in sub_1401B2EA0; rcx is descriptor +0x08 == *(ng+0x2B10)
    0x1407F37B0ull,
    0x14088FBA1ull,
    0x140C93EB0ull,
    0x140CC36FBull,
    0x140CC4550ull,
    0x140CCBC13ull,
    0x140CD442Aull,
    0x140D086D3ull,
    0x140D1F9ACull,   // ATTRIBUTED 1: in sub_140D1F940, the GUI.CAMERA_EDITOR chain from 0x140195FC8
    0x140D2F044ull,
    0x140D32072ull,
    0x140D34622ull,
    0x140D44E06ull,   // ATTRIBUTED 2: in sub_140D44DE0, the CAM.SET_CAMERA_API chain from 0x140165641
    0x140D450B0ull,
    0x140DA2965ull,   // ATTRIBUTED 4: in sub_140DA28E0, a DIRECT +0x420 / +0x450 walk, no lookup
    0x140F49DA9ull,
};

// The four rows above that have a chain, listed separately so the split is countable rather than
// readable: 17 - 4 = the thirteen ARCH-14 S6 item 3 names as unclassified.
inline constexpr uint64_t kSpectatorSymbolAttributedSites[] = {
    0x1401B3525ull, 0x140D1F9ACull, 0x140D44E06ull, 0x140DA2965ull,
};

// The string those sites reference, so the census names its own subject.
inline constexpr uint64_t kSpectatorSymbolStringVa = 0x1416D800Full;

// How far TickInfo::netGame resolves toward a rig, with every field asked of the memory manager
// before it is read. A null check on the pointer is not a check on what it points at (AUDIT
// W2-F/AVFIX): the LEVEL space is validated through W1-C's shared table check, the same predicate
// the engine's own lookup relies on by construction ordering. Ready does not mean the lookup ran;
// Create does that, once, against a space that has passed here.
//
// ★ The subject is the level space at NETGAME.SPECTATORCAM_CS_OFF (ARCH-12 R1/R4). The CS space at
// NETGAME.CS_SPACE_OFF is recorded, never gated on: it is filled by a different state transition
// and on L5 run 3 it arrived 1.3 s BEFORE the level space, so neither order may be assumed and a
// null there is not a refusal of the camera path.
enum class RigProbe : uint8_t {
    NoNetGame,            // before state 9, or between matches
    NetGameUnreadable,    // the two slots read off netGame are not committed memory
    NoLevelSpace,         // NETGAME.SPECTATORCAM_CS_OFF (+0x2B10) holds null: no map is loaded.
                          // The L3/L4 expectation -- an absent question, not a negative answer
    LevelSpaceUnreadable,  // the level space's component-system table header is not committed memory
    TableNotReady,        // allocated, not yet constructed: the load window
    TableEmpty,           // constructed, nothing installed: the lookup would find nothing
    RetiredSlotNull,      // ★ RETIRED IN POSITION, never returned. This was NoRigSlot, back when
                          // NETGAME.CS_SPACE_OFF was the gated slot and SPECTATORCAM_CS_OFF was a
                          // second one; ARCH-12 R1 made the level space the subject, so that state
                          // is NoLevelSpace. The value stays because the sentence below is only
                          // true if nothing above it ever moves -- deleting it shifted Ready and
                          // TableSortPending down by one under that very sentence (AUD-F3H F3H-4),
                          // and P-B kept its own superseded Refusal values in position for this.
    Ready,
    // Appended after Ready and never renumbered, for the reason tableguard.h gives on its own
    // enum: observer.cpp carries this value through a LONG and renders it back by name.
    TableSortPending,     // constructed, but CS.TABLE_FLAGS_OFF bit 0 is set, so the engine's lookup
                          // would sort the table IN PLACE on our tick (W1-C's TableWhy::SortPending)
};

// The numbers the sentence above promises, pinned. An insertion or a deletion anywhere in the enum
// breaks the build here instead of silently re-labelling a state that a carried LONG renders back
// by name.
static_assert(static_cast<uint8_t>(RigProbe::NoNetGame) == 0, "RigProbe numbers are stable");
static_assert(static_cast<uint8_t>(RigProbe::NetGameUnreadable) == 1, "RigProbe numbers are stable");
static_assert(static_cast<uint8_t>(RigProbe::NoLevelSpace) == 2, "RigProbe numbers are stable");
static_assert(static_cast<uint8_t>(RigProbe::LevelSpaceUnreadable) == 3, "RigProbe numbers are stable");
static_assert(static_cast<uint8_t>(RigProbe::TableNotReady) == 4, "RigProbe numbers are stable");
static_assert(static_cast<uint8_t>(RigProbe::TableEmpty) == 5, "RigProbe numbers are stable");
static_assert(static_cast<uint8_t>(RigProbe::RetiredSlotNull) == 6, "RigProbe numbers are stable");
static_assert(static_cast<uint8_t>(RigProbe::Ready) == 7, "RigProbe numbers are stable");
static_assert(static_cast<uint8_t>(RigProbe::TableSortPending) == 8, "RigProbe numbers are stable");
const char* RigProbeName(RigProbe);

// What a binding was made against. The table base is part of it because a level reload can hand
// back the same space address with a rebuilt table behind it (AVFIX F3).
//
// csSpace is carried but is NOT part of the identity, and that asymmetry is the ruling: it feeds
// the two-space differential at bind time and nothing else, so a csSpace that arrives seconds
// after the bind cannot re-key the identity and tear down a working camera.
struct RigIdentity {
    void*    netGame    = nullptr;
    void*    levelSpace = nullptr;  // *(netGame + NETGAME.SPECTATORCAM_CS_OFF): the lookup's space
    void*    csSpace    = nullptr;  // *(netGame + NETGAME.CS_SPACE_OFF): differential only
    uint64_t tableBase  = 0;

    bool operator==(const RigIdentity& o) const {
        return netGame == o.netGame && levelSpace == o.levelSpace && tableBase == o.tableBase;
    }
    bool operator!=(const RigIdentity& o) const { return !(*this == o); }
};

// No engine call and no write: safe on every tick. `out` carries whatever resolved, so a refusal
// can still name the space.
RigProbe Probe(void* netGame, RigIdentity* out);

// The spectator CS the live binding resolved, or null when nothing is bound. The rig is the
// lookup's answer, so it exists nowhere until the lookup has run -- which is why it is not a
// field of RigIdentity, which Probe fills without calling anything.
void* BoundSpectatorCs();

// The word the live binding's ARCH-12 R2 differential currently carries -- "CsSpaceClear" (the arm
// PASSED), "CsSpaceAbsent" or "CsSpaceNotAsked" (could not ask) -- or null when nothing is bound.
const char* DifferentialWord();

// Re-asks R2's CS-space arm against a freshly probed identity and republishes the word WITHOUT
// re-binding. `id` must name the same binding (same netGame and level space) or this is a no-op.
// Returns false when the arm now REFUSES -- SpaceAliased or SpaceAmbiguous; Diagnostic() says
// which -- and the caller must then release, because which object to bind is no longer decided.
//
// ★ Why it exists (AUD-F3H F3H-2): sub_14017A050 writes NETGAME.SPECTATORCAM_CS_OFF and only then,
// through SESSION.STATE_MACHINE(netGame,7), NETGAME.CS_SPACE_OFF. A bind inside that window records
// "CsSpaceAbsent", and csSpace is deliberately outside RigIdentity equality (R4), so without this
// the could-not-ask would be latched for the life of the bind and the arm never asked at all.
// ★ It can call the engine's lookup, so it is called on the module's bind-epoch cadence and NEVER
// per frame: ARCH-10 S7 -- "do not re-derive *(netGame+0x2B08) inside a per-frame lookup and call
// CS.LOOKUP_BY_SYMBOL again", because that lookup sorts the table in place on our thread.
bool ReviseDifferential(const RigIdentity& id);

// Null only if the engine did not check out; Diagnostic() then says why. netGame is
// TickInfo::netGame. The returned object has static storage duration. Called from the camera
// module's tick when Probe says Ready and the identity is new; consumers use Get().
//
// ★ Claims the pose owner (camera_writepoint_poseowner.h) BEFORE it resolves anything: if the
// S8.1.7 write point holds it, this returns null with PoseOwnerConflict rather than binding a
// camera whose input the write point's first SetPose would silently switch off.
IReplayCameraControl* Create(void* netGame);

// Drops the instance: Get() returns null until the next Create. Nothing is written into the engine.
//
// ★ camera_module OWNS this call. It clears this file's g_created, but the module keeps its own
// `bound` flag; a call from anywhere else leaves the module believing it is still bound, so Tick
// sees an unchanged rig identity, never rebinds, and every later Get() is null. All three product
// call sites are in camera_module.cpp and each leaves the module unbound (Unbind clears the flag
// on the next line; Init and Shutdown reset the whole state), which is what
// test_camera_module's "Release is the module's to call" case asserts. W2-F and W2-G unbind
// through the module, never through here.
void Release();  // and drops the pose-owner claim, if this module is the one holding it

// The last Create's refusal, or "ready". A refusal that only says NotAvailable is not a diagnosis.
const char* Diagnostic();

// ★ The FIRST view-state read refusal of this binding session, and how many there have been.
// AUDIT_L1 finding 3: Diagnostic() is overwritten by every other refusal in this file and its one
// shipped caller runs only on a bind refusal, so the sentence GetViewPose gets from W1-H needed a
// channel of its own. camera_module publishes both on the [camera] status line. Empty string and 0
// mean no refusal since the last Release(); the sentence never changes while the count rises.
const char* ViewReadRefusal();
unsigned long long ViewReadRefusals();

// The already-created instance, or null. This is the accessor W2-F and W2-G use today; if the
// architect ratifies a ReplayServices::cameraControl field it will point at this same singleton,
// so adopting it later is an addition rather than a second route.
IReplayCameraControl* Get();

// The two engine routines this control calls, so the wiring can be driven offline against a fake
// rig. Production resolves both through the build guard; nothing outside tests/ installs one
// (grep SetEngineForTest). Null restores the real engine.
struct RigEngine {
    void* (*lookupSystem)(void* space, uint64_t symbol);          // CS.LOOKUP_BY_SYMBOL
    void  (*setMode)(void* cs, int mode, unsigned short num);     // CAM.SET_CAMERA_MODE_IMPL
};
void SetEngineForTest(const RigEngine* engine);

// W1-H's guarded view-state pose read (camera::ReadViewStatePose), which GetViewPose needs on the
// stock free camera's arm. INSTALLED, not called directly: this translation unit is linked into
// images that do not carry camera_writepoint.cpp -- W2-F's panel test and W1-H's two pose-owner
// control images each compile camera_control.cpp alone -- and a direct call would make all three
// fail to link over a read they never make. camera_module.cpp installs the real one at Init and
// tests install a fake through the same call, so there is one route and no fixture flag. Never
// installed reads as Unreadable, never as a pose.
using ViewStateReader = camera::ViewPoseRead (*)(camera::ViewStatePose* out);
void            SetViewStateReader(ViewStateReader reader);
ViewStateReader InstalledViewStateReader();

// ★ W1-H's WRITE POINT, the pose SINK a commanded pose lands through (ARCH-17 S3.3(a)).
// INSTALLED, not called directly, for exactly the reason the view-state reader above is: this
// translation unit is linked into images that do not carry camera_writepoint.cpp -- W2-F's panel
// test, W1-H's two pose-owner control images and camera_control_consumer each compile
// camera_control.cpp alone (build/target_manifest.json) -- and a direct call to camera::Create
// would make all four fail to link over a write they never make. camera_module.cpp installs the
// real one at Init and tests install a fake through the same call, so there is one route and no
// fixture flag. Never installed reads as "no write point in this image" and refuses; it never
// reads as a pose that landed.
//
// `create` is camera::Create -- it re-claims the WritePoint token this control has just
// transferred to itself and resolves the write point's own spectator CS, so it is called ONCE per
// entry into regime Commanded and never per command: its resolve runs CS.LOOKUP_BY_SYMBOL, and
// that lookup sorts the engine's table in place on our tick (ARCH-10 S7, ARCH-17 S6).
// `release` is camera::Release -- Relinquish(), which re-arms every viewstate+0x9C SetPose cleared
// (ARCH-17 S3.3(c)), and only then drops the token.
// `why` names the write point's own refusal when create returns null, so the operator gets
// camera::RefusalName's twenty values rather than a bare "refused".
// `releaseKeepingToken` is camera::ReleaseKeepingToken -- Relinquish() WITHOUT the keyed
// ReleasePoseOwner, so ReleasePose can hand the token to StockFreeCam through the arbiter's atomic
// TransferPoseOwner (ARCH-19 R1). Appended, so no existing initialiser breaks; the static_assert
// below is what makes a THREE-field initialiser a compile error rather than a silent null.
struct WritePointSeam {
    IReplayCamera* (*create)(void* netGame);
    void           (*release)();
    const char*    (*why)();
    void           (*releaseKeepingToken)();   // ARCH-19 R1
};
// AUDIT_ARCH_19_20 C4's second remedy. An image built against the three-field header would leave
// this null, and a null here costs the epoch guarantee; the size check turns that into a build
// break. It is a floor, not the whole answer -- a seam initialised in code the compiler cannot see
// can still arrive null, which is why ReleasePose ALSO carries the degraded arm C4's first remedy
// asks for and never turns a null into a refusal.
static_assert(sizeof(WritePointSeam) == 4 * sizeof(void (*)()),
              "WritePointSeam gained a member: every image must be rebuilt against it");
void                  SetWritePoint(const WritePointSeam* wp);
const WritePointSeam* InstalledWritePoint();

// ---------------------------------------------------------------------------------------------
// ARCH-19 R3. The selector -> world point read that `cam look at <n>` aims with. Installed the way
// the view-state reader and the write point are, and for the same reason: this module links no
// entity header and must not acquire one through the service locator (RULING 4c).
//
// Each function is separately null-able because each can be absent for its own reason, and a
// missing one is a NAMED refusal and never a substituted point. `ng` is the same netGame this
// control bound with (RigIdentity::netGame); the seam never resolves one of its own.

// Why an aim could not be resolved. An operator does something different with each, and "there is
// no such player" must never read the same as "the roster could not be read".
enum class SubjectResolve : uint8_t {
    Resolved = 0,
    NoSeam,                 // nothing installed this read in this image
    RosterUnreadable,       // the user table did not resolve; TeamOf/TeamSlotOf could not be asked
    NoSuchSubject,          // the selector is in range and no user occupies that (team, teamSlot)
    NoBody,                 // the user resolves and carries no physics body this tick
    // Appended by AUDIT_ARCH_19_20 C3, so every value above keeps its number. The roster is fine
    // and the selector may well name someone; what is missing is that THIS session's engine has
    // confirmed our n -> (teamId, teamSlot) derivation for THIS n. RosterUnreadable was the value
    // ARCH-19 R3(b) named for this and it diagnoses the wrong thing.
    DerivationUncertified,
};
static_assert(static_cast<uint8_t>(SubjectResolve::NoBody) == 4,
              "SubjectResolve numbers are stable; append, never insert");
static_assert(static_cast<uint8_t>(SubjectResolve::DerivationUncertified) == 5,
              "SubjectResolve numbers are stable; append, never insert");
const char* SubjectResolveName(SubjectResolve);

struct SubjectPoseSeam {
    // Which UserIndex the engine's 1-based selector names, via (teamId, teamSlot). Fills `out` and
    // returns Resolved, or names why not. Scans the user table; it is one command, not a frame.
    SubjectResolve (*userOfSelector)(void* ng, int32_t selector, uint16_t* userIndexOut);
    // The body centre of mass for that user, in the engine's world frame -- the same frame
    // viewstate+0x60 is in. False when the user has no player or no physics body this tick.
    bool           (*worldPointOfUser)(void* ng, uint16_t userIndex, float outPos[3]);
};
static_assert(sizeof(SubjectPoseSeam) == 2 * sizeof(void (*)()),
              "SubjectPoseSeam gained a member: every image must be rebuilt against it");
void                   SetSubjectPose(const SubjectPoseSeam*);
const SubjectPoseSeam* InstalledSubjectPose();

// THE ENGINE'S OWN SELECTOR DERIVATION, and the one thing that must not be invented twice.
// CAM.SET_CAMERA_MODE_IMPL composes the dword it stores at SPECTATOR.CI_SELECTOR_PACKED_OFF from
// `num` and stores it with the guarded `mov [rbx+26h], eax` -- read at 0x140D44FD7-0x140D45009:
//
//     num == 0        -> 0x00040000
//     1 <= num <= 5   -> low word 1, high word num - 1
//     num >= 6        -> low word 0, high word num - 6
//
// and sub_140D6A350 composes the SAME word the other way round, from a user record, with
//     LOWORD = (word >> IDENT.USER_TEAM_SHIFT)     & IDENT.USER_TEAM_MASK
//     HIWORD = (word >> IDENT.USER_TEAMSLOT_SHIFT) & IDENT.USER_TEAMSLOT_MASK
// which is what fixes WHICH half is the team: the low word is the teamId and the high word is the
// teamSlot. Selector 0 is not this function's business -- `cam look at 0` is a parse refusal --
// and it is refused here rather than reproduced, because the engine's own answer for it collides
// with selector 10.
//
// PackedSelectorWord is the companion AUDIT_ARCH_19_20 C2 requires: p10-cam-9's predicate is
// `packed == PackedSelectorWord(teamId, teamSlot)` with (teamId, teamSlot) out of PackSelector(n),
// which tests THE PAIR AND THE LAYOUT TOGETHER. Testing the packed word alone would leave the
// split unverified, and a second hand-written copy of the bit layout is the defect this exists to
// prevent.
bool    PackSelector(int32_t selector, uint8_t* teamIdOut, uint8_t* teamSlotOut);
int32_t PackedSelectorWord(uint8_t teamId, uint8_t teamSlot);

// Whether the derivation above has been confirmed AGAINST THE ENGINE for one selector, in this
// session. ARCH-19 R3(b) pins `cam look at` shut until DL-02/f is green; AUDIT_ARCH_19_20 C6 asks
// that the pin have an explicit, per-n release, because a row scored all-or-nothing over ten
// selectors can sit in could-not-ask forever and take a shipped feature with it.
//
// The release is the oracle itself, running in the shipping image: every SetMode(mode, n) with
// n >= 1 hands the engine a selector, the engine writes ITS derivation to ci+0x26, and
// ObservedSelector reads it back. p10-cam-9 drives exactly that and scores SelectorDerivationState
// per n; an operator who has selected a player has certified that player without a probe run.
// Falsified is STICKY: a derivation that has ever disagreed is never re-certified by a later read.
enum class SelectorDerivation : uint8_t { Uncertified = 0, Certified, Falsified };
SelectorDerivation SelectorDerivationState(int32_t selector);
const char*        SelectorDerivationName(SelectorDerivation);
// The scoring step, published so the probe row and the control share one body. `packed` is what
// ObservedSelector read; (teamId, teamSlot) is what PackSelector(selector) produced.
void RecordSelectorObservation(int32_t selector, int32_t packed, uint8_t teamId, uint8_t teamSlot);

// The direction half of the family's arithmetic, and the INVERSE OF BuildFacing rather than a
// second convention: BuildFacing(yaw, pitch) faces
//     (sin(yaw) * cos(pitch), sin(pitch), cos(yaw) * cos(pitch))
// so the facing that lands on `to` from `from` is yaw = atan2(dx, dz), pitch = atan2(dy, |dxz|).
// False when the two points coincide -- the camera is standing inside its subject and there is no
// direction to face, which is ControlRefusal::FacingDegenerate and never a normalised NaN.
bool FacingToward(const float from[3], const float to[3], float* yawDeg, float* pitchDeg);


// How many hand-backs took the DEGRADED route: an image whose seam carries no
// releaseKeepingToken, where ReleasePose falls back to release() + ClaimPoseOwner. That route
// works -- the camera is never stranded, which is the whole reason it exists (AUDIT_ARCH_19_20
// C4) -- but it moves the pose-owner epoch and opens the window ARCH-19 R1 closed, so it is
// counted rather than merely tolerated. Zero on the shipping image.
uint64_t PoseHandbackDegraded();

// ★ ARCH-19 E2. THE ONE quaternion <-> facing pair in the tree. Every surface that prints or
// commands a facing calls these; a second copy is two surfaces disagreeing about the convention,
// which is the defect this codebase keeps naming.
//
// Convention (ARCH-19 R5, [U] until w2e-p17-axis-plus-z reports): yaw about world +Y measured from
// +Z toward +X, positive pitch raises the view, quat is xyzw as the engine stores it at
// viewstate+0x50. `cam look to 0 0` is the identity quaternion and faces +Z.
//
// Returns false where the facing is within 0.1 deg of vertical and a yaw does not exist. The
// out-parameters are then UNTOUCHED, so no caller can score a refusal's zeros as a facing.
bool DecomposeFacing(const float quat[4], float* yawDeg, float* pitchDeg);

// The inverse. Exposed because the L5 axis row and `cam look at` both need to name the quaternion a
// facing produces, and a second copy would be a second convention. Roll is not expressible: the
// command family cannot carry one and inventing a third angle would be worse.
void BuildFacing(float yawDeg, float pitchDeg, float quat[4]);

// ---------------------------------------------------------------------------------------------
// ★ THE OBSERVER'S ROW CONSUMER for ruling 4b, published here so tests/replay/observer_rows.h
// (W2-R's) holds a call and not a copy of the policy.
//
// The row's question is "does the free camera's view keep moving while playback is PAUSED". It is
// a TWO-SAMPLE question and its time base is WALL time, not sim time: at pause the sim step is
// 1 us (S7.3.3), and a row that measured its own elapsed time from the transport clock would
// divide by ~0 and report an absurd speed for a camera that is behaving exactly as S8.1.2 says.
// SampleViewPose stamps QueryPerformanceCounter -- the same clock CAM.WALLCLOCK_MS reads.
//
// The row's own preconditions, which this code cannot check and the observer must:
//   * the transport is PAUSED across both samples,
//   * free-cam input is being applied between them (a free camera nobody is steering is still,
//     and "Still" then means nothing),
//   * pose.integratorArmedKnown && pose.integratorArmed != 0 -- otherwise the gate S8.1.2 names
//     was not observed and the row has not tested the ruling.
struct ViewPoseSample {
    camera::ViewPoseRead read = camera::ViewPoseRead::NotBound;
    ViewPose             pose{};
    int64_t              wallTicks = 0;  // QueryPerformanceCounter at the moment of the sample
};

// Stamps the wall clock and reads. A null control is NotBound rather than a crash, because the
// observer runs rows whether or not the camera bound.
camera::ViewPoseRead SampleViewPose(const IReplayCameraControl* control, ViewPoseSample* out);

// Three outcomes, not two: "could not ask" is not "did not move". A two-state answer here is the
// defect the observer row was written to stop reporting.
enum class PoseMotion : uint8_t {
    Moved,        // ruling 4b holds for this pair of samples
    Still,        // the engine held the pose across a measurable wall-time span
    CouldNotAsk,  // a sample refused, the engine changed arms between them, or no wall time passed
};
const char* PoseMotionName(PoseMotion);

// `metresMoved` is the straight-line distance between the two positions and `wallSeconds` the span
// between the stamps; both are written on every outcome (0 when the answer is CouldNotAsk), and
// both are for the row's text -- the verdict is the return value. Moved/Still is decided on the
// exact bit patterns of the seven floats, not on an epsilon: nothing here computes, so a changed
// pose is a changed bit pattern and a threshold would only be a number nobody could defend.
PoseMotion PoseMotionBetween(const ViewPoseSample& first, const ViewPoseSample& second,
                             double* metresMoved, double* wallSeconds);

// ============================================================================================
// ARCH-23 S14.11 -- free flight for the spectator viewer. The flying is the ENGINE's, entire;
// what follows is the census that names which of its five gates is shut, and the clause that can
// say the camera did not move.
// ============================================================================================

// P0a. CR15NetSpectatorCameraCS::Update returns at :290-293 when bit 2 of the session-flags word
// reached through CR15NetGame + NETGAME.UIFLAGS_PTR_OFF is SET, so the healthy reading is 0 and a
// frozen camera with a perfect seat can be this gate alone. RAW BIT, no predicate: 1 means the
// gate is firing. Returns -1 when either dereference could not be taken -- the pointer is a
// pointer to the word, so both are guarded, and "could not ask" is not 0.
int NetGameUpdateGateBit(void* netGame);

// P2 and P3, from the BOUND rig's header: the instance count (cs+SPECTATOR.CS_INSTANCE_COUNT_OFF)
// and the local index (cs+SPECTATOR.CS_LOCAL_INDEX_OFF). From outside, "no instance exists" and
// "the local index selects none of them" look identical -- a camera that never moves -- and these
// two numbers are what part them. False when nothing is bound or the header is not committed
// memory, and then neither output is written.
bool BoundInstanceCounts(uint16_t* count, uint16_t* local);

// The `cam flight begin|end` verb's entry point. It is W2-G-32a's row in src/control's command
// table that calls this; this packet does not reach into src/control, and until that row lands the
// flight clause is NOT-RUN by name rather than absent.
//
// QUEUED rather than logged here, because the marker's whole job is to be on the SAME clock and in
// the SAME stream as the pose samples it brackets: camera_module drains it inside its next census
// window and stamps it there. Tolerance is therefore one 60-tick window, which is the sample
// cadence and the smallest interval any marker could resolve.
//
// Returns FALSE when the queue was full and this mark was DROPPED. The verb's
// reply used to say "the window is open" whatever happened here, so a dropped `begin` left the
// operator flying inside a window that was never stamped -- and the clause would then score the
// wrong samples, or none. The caller must report the drop by name.
bool MarkFlightWindow(bool begin);

// The marker's exact words, defined ONCE and here -- beside the verb that raises it -- because
// camera_module writes them, W2-G-32a acknowledges them and the run greps for them, and three
// copies of a string is three places for one of them to drift.
const char* FlightMarkerText(bool begin);

// What a caller says when MarkFlightWindow returned false. Here, beside the marker's own words,
// for the same reason they are: one definition, and the module, the verb and the run's grep all
// read it rather than each spelling it.
const char* FlightMarkerDroppedText();

// The pose clock, published so a consumer need not pull windows.h in to share it: WallTicksNow()
// is what SampleViewPose stamps into ViewPoseSample::wallTicks, and WallSeconds() converts either.
int64_t WallTicksNow();
double  WallSeconds(int64_t ticks);

// Drains one queued mark, oldest first. False when the queue is empty.
bool TakeFlightMark(bool* begin);

// Marks the queue could not hold. A dropped marker silently widens or loses a window, so it is
// counted and the module reports it rather than evaluating a window it cannot delimit.
unsigned long long FlightMarksDropped();

enum class FlightVerdict : uint8_t { Pass, Fail, NotRun };
const char* FlightVerdictName(FlightVerdict);

// One census window's contribution to the clause. `poseOk` is GetViewPose() == Ok; `freeCam` and
// `armed` are the two states that make the sample a sample OF FREE FLIGHT rather than of some
// other camera standing still, and a sample missing any of the three is not scored at all.
struct FlightSample {
    double seconds = 0.0;   // the pose clock -- QueryPerformanceCounter, what SampleViewPose stamps
    float  pos[3]  = {0.0f, 0.0f, 0.0f};
    bool   poseOk  = false;
    bool   freeCam = false;
    bool   armed   = false;
};

struct FlightWindow {
    const FlightSample* samples = nullptr;
    size_t              count   = 0;
    bool                haveBegin = false;
    bool                haveEnd   = false;
    double              beginSeconds = 0.0;
    double              endSeconds   = 0.0;
    // Samples were dropped from the ring while this window was open. The FAIL below is a
    // UNIVERSAL over the window's samples, so a subset could redden a window the whole of which
    // would have passed; a truncated window is therefore not scored at all.
    bool                truncated = false;
};

struct FlightReport {
    const char* reason      = "";   // never empty: the FAIL's name, or the NOT-RUN's own cause
    double      maxDeltaM   = 0.0;
    double      spanSeconds = 0.0;
    size_t      scored      = 0;    // samples inside the window that carried a flying pose
};

// The threshold and the two floors. 0.25 m is well inside one second of the shipped
// free_cam_settings.max_speed (8.0 m/s) and well outside any jitter in a pose the engine is not
// integrating.
inline constexpr double kFlightMovedMetres  = 0.25;
inline constexpr double kFlightStillSeconds = 3.0;
inline constexpr size_t kFlightStillSamples = 3;

// ★ PASS BEATS FAIL BY CONSTRUCTION, NOT BY A PRECEDENCE RULE. PASS is "some pair inside the
// window moved more than 0.25 m"; FAIL is "no pair did", plus the floors. They are exact
// complements over one population, so at most one can hold and the twenty-seconds-flying-then-two-
// seconds-stopped window that a working camera produces cannot be scored red. Everything else is
// NOT-RUN with its own reason -- "nobody flew" and "the camera is broken" are different sentences.
FlightVerdict EvaluateFlight(const FlightWindow& window, FlightReport* out);

}  // namespace camera_control
}  // namespace replay
