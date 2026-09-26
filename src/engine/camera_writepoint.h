#pragma once

// B.7 IReplayCamera -- the selected camera write point. Owned by W1-H; consumed by W2-E.
//
// ★ WHAT MOVES THE DESKTOP VIEW, and why this is the only arrangement that survives a frame
//   (all VERIFIED against echovr.exe md5 caa05ee3bca2f5df0809994e93e2cc05; derivations and the
//   two corpus refutations are in docs/spikes/W1-H.md):
//
//     CncaGame::RunLoop, one iteration = one rendered frame
//       CR15Game vtbl[+0x3F8]      UpdateClock
//       CR15Game vtbl[+0x400](0)   stage 0  -> CNSProvider::Update  <-- OUR TICK writes the pose
//           ...tail of every stage, in this order:
//           vtbl[+0x178]
//           vtbl[+0x188]  CAM.VIEW_COMMIT   on stage 3 only:
//                           viewstate[i]->vt[+0x08](vs, 1)   <-- RUNS THE ENGINE'S OWN INTEGRATOR,
//                                                                which rewrites vs+0x50 and vs+0x60
//                                                                unless vs+0x9C == 0
//                           node[i]->vt[+0x68](vs+0x60)      SetPosition
//                           node[i]->vt[+0x70](vs+0x50)      SetRotation
//                           node[i]+0x98 = 2*atan(tan(vs.fovY/2) * w/h)
//           vtbl[+0x190]  the gamespace / component-system update, which runs
//                           CAM.SPECTATOR_UPDATE  -> writes CCameraNode[0] AGAIN, from
//                                                    viewstate[0] if ci+0x274 != 0, else from ci
//                                                 -> and sets the Wwise listener transform from
//                                                    viewstate[0] if ci+0x3C == 5, else from ci
//           vtbl[+0x198]  CR15NetGame::Update
//           vtbl[+0x180]
//       CR15Game vtbl[+0x1C0]      render
//
//   Two consequences the corpus had backwards, and both decide the design:
//
//   1. THE SPECTATOR Update DOES NOT OVERWRITE THE VIEW-STATE. It reads it. The thing that stamps
//      a written pose back is the view-state's OWN input integrator, invoked by the commit point
//      earlier in the same stage -- so a pose written from stage 0 never even reaches the node.
//      `vs+0x9C = 0` disables that integrator, and it is the whole lever.
//
//   2. THE SPECTATOR Update IS THE LAST WRITER OF CCameraNode[0] before render, because component
//      systems run at vtbl[+0x190], AFTER the commit at vtbl[+0x188]. So the desktop view is
//      whatever the spectator Update chose -- which is why the CI flags matter even though the
//      pose itself travels through the view-state.
//
//   The implementation therefore writes BOTH: every view-state (so views >= 1 and a future VR
//   build are driven too, S8.6 commitment 2) and the spectator CI (so the audio listener follows
//   whatever mode the state machine happens to be in). Strategy() is "viewstate+api".

#include <stdint.h>

#include "engine/camera_writepoint_poseowner.h"

#include "bootstrap/tableguard.h"

// ★ IReplayCamera is in `namespace replay`, not at global scope. Architect ruling 2026-08-28 over
// the whole of Appendix B, and here it is load-bearing rather than stylistic:
// bootstrap/services.h forward-declares `replay::IReplayCamera` and types ReplayServices::camera as
// a pointer to it, so a global IReplayCamera is a DIFFERENT type and W2-E could never have assigned
// this packet's Create() into the locator. The two headers disagreed and nothing had yet included
// both; tests/camera does, which is how it surfaced.
namespace replay {

class IReplayCamera {
public:
    // `quat` is (x, y, z, w) and `pos` is world space, matching what the engine already stores at
    // viewstate+0x50 / +0x60. fovY is VERTICAL and in radians; the engine derives the horizontal
    // angle and the aspect itself.
    virtual void        SetPose(const float pos[3], const float quat[4], float fovYRadians) = 0;

    // True only for a write point that reaches every view rather than CCameraNode[0] alone.
    virtual bool        AppliesToAllViews() const = 0;

    // "viewstate" | "api" | "viewstate+api"
    virtual const char* Strategy() const = 0;

protected:
    // Not in B.7's text. The instance has static storage duration and is never deleted, and a
    // protected destructor makes deleting through the base a compile error rather than a rule
    // somebody has to remember -- the same shape W1-C used for ILog and W1-D for ITick.
    ~IReplayCamera() = default;
};

namespace camera {

// Why the write point is not available. "It refused" is not a diagnosis, so every refusal the
// implementation can reach has its own value.
enum class Refusal : uint8_t {
    None,
    NoModule,           // GetModuleHandleW(nullptr) failed
    NoGame,             // the CR15Game singleton is still null
    PrologueMismatch,   // a guarded site holds bytes other than the ones facts.tsv recorded
    PrologueUnreadable, // ★ a guarded site is NOT MAPPED in this image, so the question could not be
                        //   asked. Never folded into PrologueMismatch: "these are different bytes" is a
                        //   claim about a build we read, and on a short or wrong image we read nothing.
                        //   Stats::prologueWhy names the site, its address and the span.
    NoViewStates,       // CR15Game+0x7688 is null, or the engine reports zero views
    NoNetGame,          // no CR15NetGame yet: the pointer itself is null, so nothing was read
    LookupNull,         // CS.LOOKUP_BY_SYMBOL was asked on the level space and answered null: this
                        //   level space carries no R14NetSpectatorCamera component system
    NoLocalInstance,    // the component system has no instance at its local index
    NoLevelSpace,       // NETGAME.SPECTATORCAM_CS_OFF (CR15NetGame+0x2B10) is null: the level space
                        //   the lookup runs on does not exist yet. Expected on L3/L4 -- no map is
                        //   loaded, so sub_14017A050 has not run and there is nothing to ask.
    SpaceTableNotReady, // the level space's component-system table was read and is not safely
                        //   indexable -- Stats::spaceTable says which field disagreed
    SpaceAliased,       // ★ CR15NetGame+0x2B08 and +0x2B10 hold the SAME pointer. They are two
                        //   different spaces (ARCH-12 S2.2); one pointer in both slots means we are
                        //   not reading the field we think we are. Both values are in Stats.
    PoseOwnerConflict,  // ★★ the stock free camera already owns the pose. This module clears
                        //   viewstate+0x9C, the gate the engine's own integrator reads, so binding
                        //   both would silently stop the primary responding to input. The holder
                        //   and the claimant are named by LastPoseOwnerConflict().
    NetGameUnreadable,  // AUD-F3B F3B-2. We HELD a CR15NetGame pointer and could not ask: the
                        //   NETGAME.SPECTATORCAM_CS_OFF slot is not committed memory, so no slot
                        //   was read. Never folded into NoNetGame -- "there is no netGame yet" is a
                        //   claim about the engine's progress, this is a claim about our own read,
                        //   and the operator does different things with them. Materially reachable
                        //   because the slot is 11 KiB into the netGame: a stale, truncated or
                        //   wrong-type pointer is readable at +0x8 and is not readable there.
    LevelSpaceUnreadable, // the level-space POINTER is non-null and the object behind it could not
                        //   be read at all -- tableguard answered Unreadable, not NotReady. Split
                        //   out of SpaceTableNotReady because "we asked the table and it said no"
                        //   and "we could not reach the table" are different states (BRIEF S39).
    SpaceAmbiguous,     // the differential's second arm answered positively: the +0x2B08 space ALSO
                        //   carries CAM.SPECTATOR_SYMBOL, so which object we would bind is not
                        //   decided by ARCH-12 R1. Refuse rather than pick.
    SpectatorCsUnreadable, // the lookup returned a non-null CS and its instance block is not
                        //   committed. Was folded into the lookup's own null answer; they are
                        //   answered-null and could-not-read, which are not the same report.
    // Appended only, so every value above keeps the number it had.
};
const char* RefusalName(Refusal);

// Diagnostics. Read by the probe and by the startup log, never on the write path.
struct Stats {
    uint64_t poses;             // SetPose calls that reached the engine
    uint64_t viewsWritten;      // view-state objects written, summed over all calls
    uint64_t gateReasserts;     // times the integrator gate was found armed and re-cleared ★
    uint64_t gateUnrecorded;    // gates cleared with no room left in the restore ledger, so
                                //   Relinquish cannot put them back. Zero on every session the
                                //   engine did not replace its view states under us.

    // What the LAST Relinquish() handed back (ARCH-17 S3.3(c)). They survive the reset to the
    // never-created state on purpose: a hand-back that re-armed eight views and one that
    // re-armed none are different events, and a zeroed counter spells them the same.
    uint64_t gateRestored;      // recorded gates put back to the value SetPose found ★
    uint64_t gateNotOurs;       // recorded gates that no longer read as the zero we left --
                                //   the engine re-armed it, or that memory is something else
                                //   now. Left alone: we re-arm only what we disarmed.
    uint64_t gateUnreadable;    // recorded gates whose address no longer probes readable
    uint64_t ciWrites;          // CAM.SETCAMERA_API_IMPL calls made
    uint64_t freeCamReasserts;  // times ci+0x274 was found clear and re-set
    uint32_t viewCount;         // the engine's own view count as last read

    // The resolve step's working, kept so a refusal is a diagnosis rather than a verdict. Both
    // spaces are recorded even on the passing path: a probe that only ever sees the passing case
    // has not been shown able to see the other one.
    uint64_t levelSpace;        // CR15NetGame+0x2B10, the level space the lookup ran on
    uint64_t csFromLookup;      // CS.LOOKUP_BY_SYMBOL's answer on levelSpace for CAM.SPECTATOR_SYMBOL
    uint64_t csSpace;           // CR15NetGame+0x2B08, read for the differential only
    uint64_t csFromCsSpace;     // the same lookup on csSpace. 0 means null OR not asked -- which one
                                //   it was is in resolveWhy, because the two are not the same report
    tableguard::TableCheck spaceTable;    // levelSpace's component-system table
    tableguard::TableCheck csSpaceTable;  // csSpace's, for the differential's four-state verdict

    // One sentence naming what the resolve step did and, on a refusal, which read produced it.
    // Empty exactly when the resolve step did not run.
    char     resolveWhy[200];

    // Which guarded site refused, its rebased address and the span that was asked for. Empty
    // unless refusal is PrologueMismatch or PrologueUnreadable; a refusal that cannot say WHICH
    // site is a verdict without a diagnosis, and six sites are checked.
    char     prologueWhy[160];

    Refusal  refusal;
    bool     available;
};
void GetStats(Stats* out);

// ★ The probe's numeric oracle. Reads back, from the engine's own memory, every value the
// selected write point depends on plus what the renderer will actually use -- so "the pose
// persisted" is a comparison rather than a screenshot impression. See docs/spikes/W1-H.md S7.
//
// nodePos/nodeQuat come from the SoA transform pool the node setters actually write
// (CAM.NODE_SETPOSITION's effect), not from the node object, which those setters never touch.
struct Observation {
    bool     valid;

    uint32_t viewCount;
    uint32_t spectatorInstances;
    int32_t  spectatorMode;       // ci+0x3C   5 = free, 6 = api
    int32_t  freeCamActive;       // ci+0x274  non-zero -> node[0] comes from the view-state
    int32_t  integratorEnabled;   // viewstate[0]+0x9C  non-zero -> the engine rewrites the pose

    float    viewStatePos[3];
    float    viewStateQuat[4];
    float    viewStateFovY;

    float    ciPos[3];
    float    ciQuat[4];
    float    ciFovY;

    float    nodePos[3];          // what CCameraNode[0] will render with
    float    nodeQuat[4];
    float    nodeFovH;            // node+0x98, HORIZONTAL
};
bool Observe(Observation* out);

// ★★ THE READ RULING 4b NEEDS, and why Observe() above cannot be it.
//
// S8.1.2's claim is that the stock free camera keeps flying while the transport is PAUSED, because
// CAM.VIEWSTATE_INTEGRATE_FREE advances on CAM.WALLCLOCK_MS. Verifying it in vivo means sampling
// the pose the engine is about to render, twice, while paused -- and in exactly that scenario THIS
// module is not bound: the stock free camera holds the pose owner, Create() refused with
// PoseOwnerConflict, and Observe() returns false because stats_.available is clear. Observe() is
// the write point's own oracle and can only answer when the write point owns the camera. This read
// answers when it does not.
//
// The pose is on the VIEW-STATE and not on the CI. CAM.SPECTATOR_UPDATE sources CCameraNode[0]
// from view-state 0 whenever ci+0x274 is non-zero, which is what the stock free camera sets, so on
// that arm the view state IS the pose about to be rendered and the read must answer from it or
// refuse. The CI cannot stand in: CAM.SETCAMERA_API_STORES is only the writer THIS MODULE
// performs, and the engine writes ci+0x1C8 / ci+0x1D8 itself at seven DIRECT STORES inside
// CAM.SPECTATOR_UPDATE through an (array + index * SPECTATOR.CI_STRIDE) pointer -- the seven
// addresses, the stride proof, and three further instructions that hand those two field addresses
// to a callee (AUDIT_L1 finding 5) are in docs/spikes/W1-H.md. An image-wide "the only writer of
// ci+0x1C8 / ci+0x1D8" claim is FALSE (AUDIT_VIEWREAD_R2 section B); do not restate one without a
// scan row in facts.tsv.
// Four words, because there are four facts. Every one but Ok leaves the pose blank, so the caller
// must key on the word and never on 0.0f.
enum class ViewPoseRead : uint8_t {
    Ok,
    NotBound,     // THE PLUGIN has not bound: no rig resolved, or the control was released. Issued
                  // by camera_control, never by the read below -- this read is bind-independent.
    NoViewState,  // THE ENGINE has not built the object: no CR15Game, no view-state array, or no
                  // view-state 0 yet. Collapsing this into NotBound would tell a consumer that the
                  // plugin was idle when in fact the plugin asked and the engine had nothing.
    Unreadable,   // the build guard refused, or a link in the chain is not committed memory -- the
                  // question could not be asked, which is a different answer from either absence.
};
const char* ViewPoseReadName(ViewPoseRead);

// View-state 0's pose, as the engine holds it. Meaningless unless the read returned Ok.
struct ViewStatePose {
    float   pos[3]          = {0.0f, 0.0f, 0.0f};        // VIEWSTATE.POS_OFF  (+0x60), world space
    float   quat[4]         = {0.0f, 0.0f, 0.0f, 0.0f};  // VIEWSTATE.QUAT_OFF (+0x50), xyzw
    float   fovY            = 0.0f;                      // VIEWSTATE.FOVY_OFF (+0x88), radians
    int32_t integratorArmed = 0;                         // viewstate+0x9C: non-zero means the
                                                         // engine's own wall-clock integrator still
                                                         // rewrites the pose. THE ruling-4b gate.

    // ★ Whether +0x9C was read from a build that still carries the evidence for it. The pose
    // displacements (+0x50/+0x60) live inside CAM.LISTENER_SOURCE_TEST's guarded window, but
    // +0x9C's only V-PRIMARY evidence is VIEWSTATE.INPUT_GATE_FREE_SITE, in another function
    // entirely: on a drifted image the pose sites can still match while +0x9C has moved. So the
    // gate site is checked separately and, when it does not match, the field is left 0 with this
    // flag CLEAR -- unknown, which is not the same claim as "read, and disarmed".
    bool integratorArmedKnown = false;

    // ★ Why the read refused, when it did: the guard's own sentence, naming the site and the
    // address; the link in the walk that was not committed memory; or the link that was null.
    // EMPTY EXACTLY WHEN THE READ RETURNED Ok -- and that is true of every refusal this read
    // issues, Unreadable and NoViewState alike. It was NOT true of NoViewState until AUDIT_L1
    // finding 2 (the three absences returned with why untouched), and the comment said it was, so
    // a consumer written to the header -- camera_control.cpp keys on this field -- read "the read
    // did not refuse" on three of the nine ways it can. Unreadable is ONE word for six facts and
    // NoViewState ONE word for three; an operator staring at either on a drifted build cannot
    // tell which without this (AUDIT_VIEWREAD_R2 VR2-2, AUDIT_L1 finding 2). NotBound is the
    // control's word, not this read's, and never reaches this struct.
    char why[160] = {};
};

// Guarded, and independent of whether this module is bound: it resolves CR15Game from the image
// exactly as Init() does and reads. It calls nothing in the engine and writes nothing.
ViewPoseRead ReadViewStatePose(ViewStatePose* out);

// `netGame` is TickInfo::netGame. Returns null only if the engine did not check out; call
// GetStats() for the reason. The returned object has static storage duration.
IReplayCamera* Create(void* netGame);

// Drops the binding and releases the pose-owner token so the stock free camera can take it. Keyed
// on this module: it frees nothing if the token is held by anyone else. Writes nothing into the
// engine -- the view-state gate is left as the engine's own ctor makes it, armed.
void Release();

// ★ ARCH-19 R1. The hand-back half of Release(): re-arm every viewstate+0x9C this module cleared
// (ARCH-17 S3.3(c)) and drop this module's bookkeeping -- WITHOUT dropping the pose-owner token.
// The caller is camera_control::ReleasePose, which must hand the token to StockFreeCam through the
// arbiter's atomic TransferPoseOwner and never through a None holder: a release-then-claim opens a
// window in which a third claimant wins, and ClaimPoseOwner from a None holder moves `epoch`,
// which TransferPoseOwner is specified not to move (camera_writepoint_poseowner.h:315-317).
//
// Release() keeps its exact present behaviour and its callers -- it IS this call plus the keyed
// ReleasePoseOwner, in one body, so the S3.3(c) re-arm cannot diverge between the two paths and
// Stats::gateRestored rises identically on both.
void ReleaseKeepingToken();

// ★ The resolve step, exposed because it is the only part of Create() that can be driven without a
// running engine -- and it is the part that decides whether an engine routine is handed a pointer.
// Create() calls exactly this function with the guarded CS.LOOKUP_BY_SYMBOL; there is no second
// copy of the policy and no fixture flag that skips it.
//
// The correction it carries (ARCH-12 R1, superseding ARCH-10 S3.5 item 3). The CR15NetGame holds
// TWO spaces. `*(netGame+0x2B08)` is the CncaGameSpace; `*(netGame+0x2B10)` -- despite the id
// NETGAME.SPECTATORCAM_CS_OFF, which is a misnomer -- is the LEVEL space, a CncaGameSpaceMT built
// at map load, and the spectator camera CS is registered in THAT one. Both of the engine's own
// camera consumers prove it by handing that pointer, unchanged, as argument 1 of
// CS.LOOKUP_BY_SYMBOL with CAM.SPECTATOR_SYMBOL. There is no direct read of the CS anywhere in the
// image: the lookup IS the accessor.
//
// So the lookup runs on the level space, after tableguard says ITS component-system table is
// indexable. The +0x2B08 space is read for one purpose only -- the two-space differential, which is
// the sensitivity control that keeps R1 falsifiable: the same symbol looked up in the csSpace must
// answer null, and the two slots must not hold one pointer. Neither slot's readiness gates the
// other (ARCH-12 R4: statically +0x2B10 is written first, on the recorded L5 #3 run +0x2B08 was,
// and neither order may be assumed), so a null or not-Ready csSpace is a control that could not be
// asked -- recorded, never a refusal of the camera path.
namespace detail {

using LookupComponentSystemFn = void*(__fastcall*)(void* space, uint64_t symbol);

struct Resolution {
    Refusal   refusal = Refusal::NoNetGame;
    uint64_t  levelSpace = 0;     // *(netGame + NETGAME_SPECTATORCAM_CS_OFF), the lookup's space
    uint64_t  csFromLookup = 0;   // its answer for CAM.SPECTATOR_SYMBOL
    uint64_t  csSpace = 0;        // *(netGame + NETGAME_CS_SPACE_OFF), differential only
    uint64_t  csFromCsSpace = 0;  // the differential arm's answer; 0 is null OR not-asked, see why
    tableguard::TableCheck spaceTable;
    tableguard::TableCheck csSpaceTable;
    char      why[200] = {};
};

Resolution ResolveSpectatorCs(void* netGame, LookupComponentSystemFn lookup);

// ★ The build guard over the six sites this module calls or depends on, exposed for the same
// reason ResolveSpectatorCs is: it is the part of Create() that decides whether engine memory is
// touched, and it is drivable offline. Create() calls exactly this; there is no second site table.
//
// `slide` is mapped base minus camera_facts::kImageBase, so a test can point the whole table at a
// reserved range and reproduce the short-image case without a second echovr.exe.
//
// Every site is probed for readability over the FULL 20-byte compare span before the compare, and
// the sites are walked ONE AT A TIME: a site that matches vouches for itself and for nothing after
// it, so the first non-Match answer is the verdict and it names its own site.
struct GuardResult {
    Refusal refusal = Refusal::None;  // None, PrologueMismatch or PrologueUnreadable
    char    why[160] = {};            // empty exactly when refusal is None
};

GuardResult CheckGuardedSites(uintptr_t slide);

// ★ The read's OWN site table -- not Create()'s six. Exposed for the same reason
// CheckGuardedSites is, and for one more: without it the only offline exercise of this guard was
// the production entry answering Unreadable in a host that is not echovr.exe, which the LATER
// GAME_SINGLETON_RVA probe produces anyway. That test passed with the table emptied
// (AUDIT_K2b finding 1). Driven over a fabricated image it can now fail.
//
// Two sites, walked in order, first non-Match wins and names itself:
//   CAM.VIEW_COMMIT           its body carries VIEWSTATE.ARRAY_OFF -- mov rcx,[rbp+7688h] at
//                             0x140517B1D. The 20 bytes compared are the PROLOGUE, so this is a
//                             function-identity check: ARRAY_OFF and FOVY_OFF (0x14D and 0x19E
//                             into the body) rest on it only indirectly, and the comment that
//                             claimed otherwise was wrong.
//   CAM.LISTENER_SOURCE_TEST  20 B at CAM.SPECTATOR_UPDATE+0x4B6 literally containing
//                             VIEWSTATE.QUAT_OFF (movups xmm0,[rax+50h]) and VIEWSTATE.POS_OFF
//                             (movups xmm1,[rax+60h]), both V-PRIMARY. These two displacements
//                             ARE covered, byte for byte.
// Create()'s other four are deliberately absent: a read that refused because an unrelated
// write-path site drifted would be dead for the one question it exists to answer.
GuardResult CheckViewPoseSites(uintptr_t slide);

// VIEWSTATE.INPUT_GATE_FREE_SITE, the sole V-PRIMARY evidence for +0x9C. Separate from the table
// above because it is NOT fatal: a drifted gate site costs the ruling-4b field, not the pose.
bool IntegratorGateSiteMatches(uintptr_t slide);

// The production entry with the module base passed in rather than asked of the OS, so a test can
// fabricate an image -- guarded sites, GAME_SINGLETON_RVA slot and all -- and reach the Ok path.
// ReadViewStatePose() is exactly this called with GetModuleHandleW(nullptr).
ViewPoseRead ReadViewStatePoseAt(uintptr_t module, ViewStatePose* out);

// ★ The view-state read below the image resolve, exposed for the reason ResolveSpectatorCs is: it
// is the whole of ReadViewStatePose() except the lines that turn an image into a CR15Game
// pointer, and it is drivable offline against a constructed game object. There is no second copy
// of the walk and no fixture flag that skips a Readable() probe.
//
// `integratorGateSiteMatched` is the caller's answer for THIS image; it decides only whether
// +0x9C is read at all, and never whether the pose is published.
ViewPoseRead ReadViewStatePose(uintptr_t game, bool integratorGateSiteMatched, ViewStatePose* out);

// ★ WHAT SetPose TOOK OUT OF THE ENGINE, SO Relinquish CAN PUT IT BACK (ARCH-17 S3.3(c)).
//
// SetPose clears viewstate+0x9C on every view it writes, and +0x9C is written in exactly one
// place in echovr.exe -- the base constructor (W1-H S4). Nothing in the image re-arms it. So a
// Relinquish() that only zeroed this module's own bookkeeping left every view state we touched
// DISARMED, and the stock free camera -- the owner's "freely move around" -- dead for the rest
// of the session: `cam free` after `cam move` would select mode 5 and hand back a camera that
// does not answer input.
//
// The ledger keys on the view-state POINTER, keeps the pre-state of the FIRST disarm (a later
// SetPose finding the gate re-armed is the engine taking it back, not a new pre-state), and
// restores exactly what it recorded -- so a session in which SetPose never ran restores nothing.
//
// It is a per-image singleton rather than a member of the write point's object for one reason:
// that is what makes the restore drivable offline. Nothing outside this file can reach the
// Camera instance, and a re-arm no test can drive is a re-arm no test can show broken.
constexpr uint32_t kGateLedgerMax = 32;  // four generations of VIEWSTATE.MAX_VIEWS

struct GateLedger {
    struct Entry {
        uintptr_t viewState;
        int32_t   prior;   // what +0x9C held before we cleared it; never zero by construction
    };
    Entry    entries[kGateLedgerMax];
    uint32_t count = 0;

    // False exactly when the ledger is full and this gate is therefore owed a restore nobody
    // can make. Re-recording a view state already held keeps the first pre-state and succeeds.
    bool Record(uintptr_t viewState, int32_t prior);
};

GateLedger& ViewGateLedger();

// The per-view half of SetPose, exposed for the reason ResolveSpectatorCs is: it is everything
// the write does to the engine's view states, it is drivable over fabricated ones, and the
// re-arm it records cannot be shown broken through a private member. SetPose calls exactly
// this; there is no second copy of the loop. `views` is clamped to VIEWSTATE.MAX_VIEWS here.
void WriteViewStates(uintptr_t viewStates, uint32_t views, const float pos[3],
                     const float quat[4], float fovYRadians, Stats& stats);

// Re-arms every gate the ledger holds, empties it, and counts each outcome into `stats`.
// Relinquish() calls exactly this before it drops the token.
void RestoreViewGates(Stats& stats);

}  // namespace detail

}  // namespace camera
}  // namespace replay
