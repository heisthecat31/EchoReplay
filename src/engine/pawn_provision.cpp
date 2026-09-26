#include "engine/pawn_provision.h"

#include <windows.h>
#include <winternl.h>

#include <stdio.h>
#include <string.h>

#include "bootstrap/log.h"
#include "bootstrap/bootstrap.h"
#include "bootstrap/config.h"
#include "bootstrap/module_order.h"
#include "bootstrap/paths.h"
#include "bootstrap/services.h"
#include "engine/entity.h"
#include "capture/roster.h"
#include "engine/identity_access.h"
#include "playback/core/recorded_loadout.h"
#include "generated/facts_generated.h"

namespace replay {

namespace PawnProvision {

// NAME FIRST, sentence after -- L3 #15 F3.  bootstrap/log.h caps a line at kLogLineMax and the cap
// ate the tail of a refusal sentence, taking with it precisely the clause that said the refusal was
// OUR postcondition rather than an occupancy claim.  The name a reader greps for now leads the
// line, so a cap can only ever cost prose.  kRows is INDEXED BY THE ENUM: the table and the
// vocabulary agree by position, and the test asserts that agreement value by value.
namespace {

struct RefusalRow { const char* name; const char* text; };

// Every text is "<name>: <sentence>", so a consumer that prints only RefusalText still leads with
// the name.  How long a text may be is not stated here: the bound is the DRESSER's prefix plus the
// text inside kLogLineMax, and the test asserts exactly that rather than a number beside it.
constexpr RefusalRow kRows[] = {
    {"None", "None: no refusal"},
    // It is the value the constructor and Reset() install, so it is what a slot -- and the
    // window line's `blocker` -- reads before anything has been asked of the engine at all.  It
    // is NOT a statement that the provisioner is unarmed: an armed provisioner that no dress
    // pass has reached yet reads exactly this, and DIAG-NO-REQUEST spent a run on the old text.
    {"NotAttempted",
     "NotAttempted: nothing has been asked for this subject yet -- the initial value, not a "
     "refusal the engine gave and not a claim that the provisioner is unarmed"},
    {"NoNetGame", "NoNetGame: no CR15NetGame yet"},
    {"NoGameSpace", "NoGameSpace: the netgame carries no game space"},
    {"SessionNotInMatch",
     "SessionNotInMatch: the session's state word is not NETGAME.STATE_INMATCH, so an admitted "
     "entrant would get no placed pawn"},
    {"NoMemberRecord", "NoMemberRecord: the lobby's own member count does not cover that slot"},
    {"SlotOutOfRange", "SlotOutOfRange: slot is outside the engine's own 16-entry slot table"},
    {"SlotAlreadyOccupied",
     "SlotAlreadyOccupied: every slot in the engine's table is held by an entrant this session did "
     "not create, so there was nowhere to map this one; refused, never adopted"},
    {"AdmitNotPromoted",
     "AdmitNotPromoted: the admission insert's prototype is not promoted, so nothing was called"},
    {"InsertReturnedNoIndex", "InsertReturnedNoIndex: the admission insert answered 0xFFFF"},
    {"ReplayBand",
     "ReplayBand: the index the insert produced is inside the replay band and would be renamed "
     "\"Replayer %u\""},
    {"ActorGuidAbsent",
     "ActorGuidAbsent: the entrant carries no actor GUID, so nothing could be placed"},
    {"ProfileStillEmpty",
     "ProfileStillEmpty: the insert ran and the entrant's profile CJson is still empty"},
    {"BudgetExceeded",
     "BudgetExceeded: this frame's provision.per_frame_max is spent; the slot stays queued"},
    {"SlotOccupancyUnreadable",
     "SlotOccupancyUnreadable: no slot could be proven free and at least one could not be read at "
     "all -- the entrant table's own memory did not pass its span proof. An ABSENT row is NOT "
     "this: a table that names no entrant for a slot says that slot is FREE. The surface COULD "
     "NOT SAY: refused rather than guessed, and this is not \"the lobby is full\""},
    {"AdmitDidNotCreateUser",
     "AdmitDidNotCreateUser: the insert answered a user index and the engine's entrant table does "
     "not name that slot back for it, so no user record was created: a postcondition this session "
     "failed, NOT a slot somebody else holds"},
    {"AdmitDidNotCreatePlayer",
     "AdmitDidNotCreatePlayer: the entrant exists and names the slot, but no actor component ref "
     "resolves for it, so the insert's state-9 arm placed no pawn: a postcondition this session "
     "failed, NOT a slot somebody else holds"},
    {"AdmitDidNotCreatePhysics",
     "AdmitDidNotCreatePhysics: the entrant exists, names the slot, and the engine's own actor "
     "step ran over it, and no component instance answers for its actor -- the pawn has no body "
     "to write a pose into: a postcondition this session failed, NOT a slot somebody else holds"},
    {"RetryNotDue",
     "RetryNotDue: a DEFERRAL -- the session blocker has not moved since the last attempt and the "
     "retry cadence's next pump has not arrived, so no lobby was walked and no insert was called"},
    {"ProfileNotApplied",
     "ProfileNotApplied: the insert created the entrant and the recorded profile is NOT on it -- "
     "either the dress refused, or it reported success and the engine's own document does not "
     "answer the recording back (L3 #19: 58 x `json db is cached, read only`) -- so the actor "
     "step was NOT called: an entrant with no profile is the one the engine faults on, and "
     "refusing is what not crashing looks like"},
    {"ProfileParseFailed",
     "ProfileParseFailed: the recorded profile is not json the engine's own parser will take "
     "(IDENT.JSON_PARSE_TEXT answered non-zero over the recording's own bytes), so no document "
     "was attached to the entrant and the actor step was NOT called: a postcondition this "
     "session failed, NOT a slot somebody else holds"},
    {"ProfileAttachFailed",
     "ProfileAttachFailed: the recorded profile parsed and the engine would not take it -- the "
     "entrant's profile document field would not prove, the store did not read back, or one of "
     "the engine entry points the attach needs did not bind on this image -- so the entrant kept "
     "the json db's CACHED document, which refuses every key, and the actor step was NOT called"},
    {"LoadoutsEmpty",
     "LoadoutsEmpty: the entrant's own loadout instance count is 0, or the loadout table's memory "
     "could not be proved, so the engine's actor step was NOT called. L3 #18 called it on exactly "
     "this entrant and the engine read customisation off the end of a zero-length stream: "
     "\"User has 0 loadout instances\" -> cmemstream.cpp:89 -> Deadlock detected! -> 0xC0000005"},
    {"ActorAssetsNotReady",
     "ActorAssetsNotReady: the entrant holds no actor component instance, so the engine's actor "
     "step was NOT called. NETGAME.REFRESH_PLAYER_ACTOR is a REFRESH -- both of its callers run "
     "it on an entrant that already IS a remote player -- and L3 #21 issued it on one that was "
     "not: the main thread never returned and the watchdog printed \"Deadlock detected!\""},
    {"ReplyRouteNotPromoted",
     "ReplyRouteNotPromoted: the engine's own remote-admission route -- publishing the entrant-added "
     "message to the handler the netgame registered for it -- is not bound, so nothing was "
     "published and the direct insert was used instead"},
    {"ReplyWrongPhase",
     "ReplyWrongPhase: the reply route is bound and this is not the phase the engine's own receive "
     "path publishes from, so no message was published and no engine call was made"},
    {"ReplyNotDispatched",
     "ReplyNotDispatched: the entrant-added message was authored and the engine's publish entry "
     "would not take it, so the registered handler never ran"},
    {"ReplySeedRefused",
     "ReplySeedRefused: the lobby member record the entrant-added handler reads its ids, account "
     "type and name out of could not be seeded, so nothing was published -- the message itself "
     "carries one qword and no other attribute of the entrant"},
    {"NotInRemotePlayerCs",
     "NotInRemotePlayerCs: the entrant's actor is not registered with the R14RemotePlayer "
     "component system, which the engine does only in the first-allocation arm of its own actor "
     "step, so no component instance can be built for it yet"},
    {"ActorNotEnabled",
     "ActorNotEnabled: the engine's own predicate says this entrant's actor is still disabled -- "
     "the reading its entrant dump renders as \"(disabled) (pooled)\" -- so it has no body and "
     "cannot be driven"},
    {"ActorNotPlaced",
     "ActorNotPlaced: the actor is enabled and the engine's place step has not run for it, so it "
     "holds no world transform to move"},
    {"ParticipationNotIssued",
     "ParticipationNotIssued: every hop reads the engine's own record as done and this session "
     "never issued the participation that authored it, so this pawn is not this session's to "
     "claim -- a build whose driver is unbound or gated off provisions nothing"},
    {"NoPhysicsCi",
     "NoPhysicsCi: the actor is enabled and placed and the component system that carries its body "
     "has not built an instance for it yet; that step is the engine's own per-frame update and "
     "this waits for it rather than calling into it"},
    {"ViewerSlotExcluded",
     "ViewerSlotExcluded: the entrant the engine's record answers with was already in the table "
     "before this provisioner asked for anything -- the viewer's own user -- so it is not this "
     "session's pawn and no recorded loadout or profile is applied to it"},
    {"PhysicsCiUnreadable",
     "PhysicsCiUnreadable: the R14RemotePlayer component system, its element pool or this "
     "entrant's own CI element did not survive a probe, so no requirement word and no init "
     "witness were read -- this is a could-not-ask and not a report that the engine is still "
     "building the instance"},
    {"PhysicsCiIndexOutOfRange",
     "PhysicsCiIndexOutOfRange: the entrant index is past the engine's own entrant count, so the "
     "CI walk would be matching a user id word that belongs to no user; nothing was read"},
    {"LoadoutSameFrameAsParticipation",
     "LoadoutSameFrameAsParticipation: a DEFERRAL -- every hop reads done and the participation "
     "this session issued is this same pump old. The loadout apply resolves in the actor/space "
     "domain and S14.6 condition 2 forbids it in the frame that authored the participation, so "
     "the dress waits one cadence window and the slot is asked again"},
    {"LoadoutNotPublishedBeforeEnable",
     "LoadoutNotPublishedBeforeEnable: a DEFERRAL -- the loadout container has been published and the engine's own live per-slot count still reads 0, so the enable the hop chain is about to issue would drive the engine's loadout apply against no instances and refuse for want of them, once, with nothing to re-run it. The hop chain is held and the slot is asked again next window"},
    {"LoadoutInstanceEmpty",
     "LoadoutInstanceEmpty: a DEFERRAL -- the published instance the engine will match by name holds 0 item pairs (IDENT.INSTANCE_ITEM_COUNT_OFF), so IDENT.APPLY_LOADOUT_ITEMS would print `Applying loadout items` and skip its whole loop without a word. The pairs come from the profile document alone: nothing downstream of the parse can put them there"},
    {"LoadoutActorUnresolved",
     "LoadoutActorUnresolved: a DEFERRAL -- IDENT.ACTOR_COMPONENT_REF resolved no actor for this entrant's guid, which is the first gate IDENT.APPLY_LOADOUT_ITEMS closes after its print. The apply would run against no body at all"},
    {"LoadoutCsPointerNull",
     "LoadoutCsPointerNull: a DEFERRAL -- the entrant's actor resolved and carries no customisation component system, so the engine's apply has nothing to write the items into and skips its loop silently"},
    {"LoadoutCompKeyInvalid",
     "LoadoutCompKeyInvalid: a DEFERRAL -- IDENT.ACTOR_REF_IS_VALID_VA says the customisation component system does not own this ref's handle, the third gate the apply closes. The component exists and the entrant is not in it yet"},
    {"XplatIdCollision",
     "XplatIdCollision: the synthetic XPlatformID could not be authored clear of the ids the live session already holds -- the viewer's own at NETGAME.ENTRANT_XPLAT_ID64_OFF and every admitted entrant's -- after its whole salt budget. Admitting the entrant on a colliding id is what sub_1406051C0 evicts a live entrant for, so nothing is admitted"},
    {"ModelNotInstantiated",
     "ModelNotInstantiated: the entrant was enabled and its productive chassis apply driven on an undressed record, but ComponentOnActor(model_.cs, actor) reads no InstanceModel yet. Holds the slot Awaiting while the async swap (sub_140CC4B50 -> sub_14042D120 -> CModel::SwapModel) is in flight; past the grace it provisions bodyless, named -- a verdict, not the silent noModelOnActor"},
    {"ParticipationSettling",
     "ParticipationSettling: every hop reads done and the actor is enabled, but a re-seat booking is still pending -- the enable hop 2 read is the one the P1-47 latch books Issued next window. A DEFERRAL that holds the slot Awaiting to reach Issued or a named settle-exhaustion, never the Failed latch ParticipationNotIssued would take"},
    {"DirectInsertSuppressed",
     "DirectInsertSuppressed: the engine's own remote-admission reply route is not promoted (its "
     "runtime binds did not resolve, or pawn.reply_route is disarmed), so the replay provisioner "
     "refused rather than fall through to AdmitIntoLobbySlot's direct insert -- which is "
     "IDENT.ADMIT_USER 0x140152210 = AddBotUser and attaches AI (R14NetBot / RxAI). No entrant was "
     "seated. A DEFERRAL: a later pump may promote the route and the slot re-asks"},
};
static_assert(sizeof(kRows) / sizeof(kRows[0]) == kRefusalCount,
              "the table is indexed by the enum: a value without a row would answer Unknown, and a "
              "row without a value would shift every name past it onto the wrong refusal");

}  // namespace

const char* RefusalName(Refusal r) {
    const size_t i = size_t(r);
    return i < kRefusalCount ? kRows[i].name : "Unknown";
}

const char* RefusalText(Refusal r) {
    const size_t i = size_t(r);
    return i < kRefusalCount ? kRows[i].text : "Unknown: unknown refusal";
}

namespace {
constexpr const char* const kHopNames[kHopCount] = {"RemotePlayerCs", "Enabled", "Placed",
                                                    "PhysicsCi", "Done"};
}  // namespace

namespace {
// The enum's own spelling, so a census line names the refusal a reader can grep this file for.
constexpr const char* const kParticipateRefusalNames[kParticipateRefusalCount] = {
    "None",           "NoDriver",           "NoGate",
    "Reentrant",      "EntrantIsLocalUser", "TeamNotRecorded",
    "TeamOutOfRange", "RecordedAsSpectator", "SpawnedRecordUnwritable",
    "SpawnedRecordNotWritten", "AssignTeamNotApplied", "ViewerIndexRefused",
    "ParticipationNotReRun",
    "RefreshDeferredToNextPump", "ActorAssetsNotReady", "ActorNotEnabledAfterRefresh",
    "RefreshAttemptsExhausted", "ReSeatLegNotApplied", "ReSeatNotRestored",
    "ReSeatAwaitingReRead", "RefreshDeclinedKeyUnchanged", "BounceUnsafeForMultipleEntrants",
    "TeamSlotChangedByBounce", "RefreshDeclineNotCorroborated",
    "ActorEnabledSoNotFreed", "ReSeatSettleExhausted"};

// The reading a census prints beside an `Issued`.  The second one names the state it was issued
// FROM, because that arm's whole hazard is that it looks like a no-op: the role word already read
// the recorded value and the drive was the engine's dispatch, not the assign.
constexpr const char* const kParticipateDriveNames[] = {"none", "assignTeam",
                                                        "roleAlreadyMatched:refreshed",
                                                        "roleAlreadyMatched:assignTeam",
                                                        "seededEntrantAdd"};

// Compile-time widths, so the census buffer below is derived from the tables that fill it rather
// than transcribed from one example entry that stops being the longest the moment a name is added.
constexpr size_t CLen(const char* s) {
    size_t n = 0;
    while (s[n] != 0) ++n;
    return n;
}
constexpr size_t MaxNameLen(const char* const* names, size_t count) {
    size_t m = 0;
    for (size_t i = 0; i < count; ++i) {
        const size_t l = CLen(names[i]);
        if (l > m) m = l;
    }
    return m;
}
}  // namespace

const char* ParticipateRefusalName(ParticipateRefusal r) {
    const size_t i = size_t(r);
    return i < kParticipateRefusalCount ? kParticipateRefusalNames[i] : "Unknown";
}

const char* ParticipateDriveName(ParticipateDrive d) {
    const size_t i = size_t(d);
    return i < (sizeof(kParticipateDriveNames) / sizeof(kParticipateDriveNames[0]))
               ? kParticipateDriveNames[i]
               : "Unknown";
}

const char* HopName(Hop h) {
    const size_t i = size_t(h);
    return i < kHopCount ? kHopNames[i] : "Unknown";
}

bool ShouldAuthorDocument(bool enabled, bool documentRecorded, bool hasSlotKeys,
                          uint16_t filledPairCount) {
#ifdef OUTFITS_MUTATE_AUTHOR_OVER_RECORDED_DOCUMENT
    // The defect: a capture that DID record the entrant's own profile gets ours instead, so the
    // one row in the file with real social/publisher material is replaced by a skeleton.
    return enabled && hasSlotKeys && filledPairCount != 0 &&
           filledPairCount != replay::identity::kRecordedPairCountUnknown;
#else
    return enabled && !documentRecorded && hasSlotKeys && filledPairCount != 0 &&
           filledPairCount != replay::identity::kRecordedPairCountUnknown;
#endif
}

identity_access::LoadoutApplyGate MergeGateReread(identity_access::LoadoutApplyGate prior,
                                                  identity_access::LoadoutApplyGate reread) {
    using G = identity_access::LoadoutApplyGate;
#ifdef W2P1_MUTATE_REFUSAL_CLOBBERED_TO_UNASKED
    // The defect: an Unasked re-read overwrites a refusal taken at publish time, gateWhy falls to
    // None, no hold is taken, and the one-shot enable is spent on the apply this hold exists to stop.
    return reread;
#else
    if (reread != G::Unasked || prior == G::Unasked) return reread;
    return prior;
#endif
}

Refusal HopRefusal(Hop h) {
    switch (h) {
        case Hop::RemotePlayerCs: return Refusal::NotInRemotePlayerCs;
        case Hop::Enabled:        return Refusal::ActorNotEnabled;
        case Hop::Placed:         return Refusal::ActorNotPlaced;
        case Hop::PhysicsCi:      return Refusal::NoPhysicsCi;
        case Hop::Done:           return Refusal::None;
    }
    return Refusal::NotInRemotePlayerCs;
}

// Every value is spelled out rather than swept into a default arm: a requirement added to the
// vocabulary must be classified here as wait-or-fault by whoever adds it, not silently inherit
// "the engine is still working on it".
Refusal PhysicsCiRefusal(PhysicsCiReq r) {
    switch (r) {
        case PhysicsCiReq::Unreadable:      return Refusal::PhysicsCiUnreadable;
        case PhysicsCiReq::IndexOutOfRange: return Refusal::PhysicsCiIndexOutOfRange;
        case PhysicsCiReq::None:
        case PhysicsCiReq::NcaPhysics:
        case PhysicsCiReq::NcaTransform:
        case PhysicsCiReq::NcaModel:
        case PhysicsCiReq::R15Body:
        case PhysicsCiReq::R15NetPhysics:
        case PhysicsCiReq::R15TouchInteract:
        case PhysicsCiReq::NotAttempted:    return Refusal::NoPhysicsCi;
        // `Unknown` is "nothing is wired to read the CI, so the question was
        // never asked", and NoPhysicsCi's own text is a positive claim about the engine -- "the
        // component system that carries its body has not built an instance for it yet ... this
        // waits for it".  Reporting the first as the second is the could-not-ask / asked-and-failed
        // conflation Unreadable and IndexOutOfRange were added to avoid, left standing for the one
        // case where the reader is not wired at all.  PhysicsCiUnreadable asserts nothing about the
        // engine's progress, which is the property this arm needs.
        case PhysicsCiReq::Unknown:         return Refusal::PhysicsCiUnreadable;
    }
    return Refusal::NoPhysicsCi;
}

namespace {
// The engine's own spelling, from the six "R14RemotePlayer requires <name>" literals InitCI raises
// (cr15remoteplayercs.cpp:328/332/336/340/344/348).  A census that prints one of these names is
// printing the requirement the engine itself refused on, not our paraphrase of it.
constexpr const char* const kPhysicsCiReqNames[] = {"none",             "ncaPhysics",    "ncaTransform",
                                          "ncaModel",         "R15Body",       "R15NetPhysics",
                                          "R15TouchInteract", "not-attempted", "unreadable",
                                          "index-out-of-range", "unknown"};
static_assert(sizeof(kPhysicsCiReqNames) / sizeof(kPhysicsCiReqNames[0]) ==
                  size_t(PhysicsCiReq::Unknown) + 1,
              "one name per requirement, or the census prints the wrong one");
}  // namespace

const char* PhysicsCiReqName(PhysicsCiReq r) {
    const size_t i = size_t(r);
    return i < sizeof(kPhysicsCiReqNames) / sizeof(kPhysicsCiReqNames[0]) ? kPhysicsCiReqNames[i]
                                                                          : "unknown";
}

// --- the synchronous diagnostic writer, the phase markers and the crash block -----------------
//
// Everything below writes through ONE appending handle opened at install and flushed after every
// line.  The plugin's own ring log cannot be used for this: it is drained by a worker thread that
// will not run again once the process is dying, which is exactly why L3 #25-#27 each ended with a
// log whose last line was seconds older than the fault.
namespace {

HANDLE   g_diag = INVALID_HANDLE_VALUE;
PVOID    g_veh = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_prevUef = nullptr;
bool     g_uefInstalled = false;
LONG     g_inHandler = 0;      // re-entrancy latch ONLY -- released after every block
LONG     g_blocks = 0;         // blocks written, and the sequence number a reader sorts on
uint64_t g_flushes = 0;
char     g_lastMarker[192] = {0};
// DERIVED, never chosen.  One driven pass issues the hop chain's ten markers
// AND every mark DressLoadout makes; at the old flat 16 the whole loadout sequence past L1's
// sixteenth slot-name hash was suppressed, so the crash block's last marker read "hash slot name"
// while the fault was landing in IDENT.APPLY_LOADOUT_ITEMS.
constexpr unsigned kHopMarksPerDrivingPass = 10;
// AUD-W2-P1-30-part2 F11.  The budget is spent per TICK and a tick drives as many passes as the
// slot table has rows -- ReopenAttemptWindow makes every slot due again the moment a provision
// lands -- so a budget sized for ONE pass silences every entrant after the first and freezes
// g_lastMarker on entrant 1, which attributes entrant 2's fault to entrant 1's last call.  Both
// factors are read from the code that produces them; neither is chosen here.
constexpr unsigned kDrivenPassesPerTick = facts::ENTITY_USER_SLOT_TABLE_MAX;
constexpr unsigned kMarksPerDrivenPass =
    kHopMarksPerDrivingPass + identity_access::kLoadoutMaxMarksPerPass;
#if defined(W2P1_MUTATE_MARKER_BUDGET_TOO_SMALL)
constexpr unsigned kMarkersPerTick = 16;
#elif defined(W2P1_MUTATE_BUDGET_ONE_PASS)
constexpr unsigned kMarkersPerTick = kMarksPerDrivenPass;
#else
constexpr unsigned kMarkersPerTick = kDrivenPassesPerTick * kMarksPerDrivenPass;
#endif
unsigned g_markersThisTick = 0;
// Every mark ASKED FOR this tick, counted whether or not the diag file is open and whether or not
// the budget let it through.  It is what makes the budget checkable against a real dress instead
// of against a hand count of the constants it is built from (F12).
unsigned g_markRequestsThisTick = 0;

// THIS PROCESS TAKES ACCESS VIOLATIONS ON PURPOSE.  Our own DLL swallows them at
// twenty-odd probe sites -- entity.cpp's SafeReadFilter/TryTouchSpan on essentially every per-frame
// entity read, tick.cpp's TryLoadPointer on the clock cell every tick, session.cpp,
// identity_access.cpp -- each inside a __try that a first-chance VEH cannot see.  A logger with a
// LIFETIME latch spends its one block on the first swallowed probe and then prints NOTHING for the
// fault that kills the process, which is worse than silence: the block it did write names
// echoreplay.dll and points the diagnosis at the wrong module.
//
// So the latch is released after every block and the total is capped instead.  A swallowed probe
// costs a numbered block; it can never cost the LAST one.  And the block that matters is written
// twice over: once first-chance, and again from a chained SetUnhandledExceptionFilter -- the arm
// that only ever runs for a fault nothing handled.
constexpr LONG kMaxBlocks = 8;

// The last fatal first-chance exception, staged by the VEH with NO I/O at all and committed by the
// unhandled filter.  A ring of one: the newest overwrites the oldest, because the newest is the
// one the process died on.
struct Staged {
    LONG    seq;
    DWORD   code;
    DWORD   threadId;
    uint64_t address;
};
Staged g_staged = {0, 0, 0, 0};

// THE CRASH PATH TAKES NO LOADER LOCK.  GetModuleHandleEx/GetModuleFileName both
// acquire it, and a handler that blocks on a LoadLibrary in flight on another thread does not
// truncate the block -- it HANGS THE GAME, which costs the whole run and looks like nothing at all.
// The module list is snapshotted off the tick instead, and the handler does a linear scan of the
// snapshot with no Win32 call and no MAX_PATH buffer (the latter also matters on the
// STATUS_STACK_OVERFLOW path this claims to survive).
struct ModuleEntry {
    uint64_t base;
    uint64_t size;
    uint64_t preferred;   // the module's own OptionalHeader.ImageBase -- the base IDA names
    char     name[64];
};
constexpr unsigned kMaxModules = 256;
ModuleEntry g_modules[kMaxModules];
volatile LONG g_moduleCount = 0;

// A read that may fault.  The unwinder hands back frame pointers off memory the fault may have
// already invalidated, and a second fault inside the handler would cost the block we came for.
bool SafeReadU64(const void* p, uint64_t* out) {
    __try {
        *out = *static_cast<const uint64_t*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The PE walk is BOUNDED at every step and guarded against a fault, because a
// module list is not a promise that every DllBase in it still points at a well-formed image.
// Answers the module's preferred base and its mapped size, which is what the snapshot needs.
bool ReadImageHeader(const void* base, uint64_t* preferred, uint64_t* size) {
    __try {
        const IMAGE_DOS_HEADER* dos = static_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const LONG at = dos->e_lfanew;
        if (at <= 0 || at > 0x1000) return false;
        const IMAGE_NT_HEADERS64* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            static_cast<const char*>(base) + at);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
        if (nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) return false;
        *preferred = uint64_t(nt->OptionalHeader.ImageBase);
        *size = uint64_t(nt->OptionalHeader.SizeOfImage);
        return *size != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void AddModule(const void* base, const wchar_t* path, size_t pathLen) {
    LONG n = g_moduleCount;
    if (base == nullptr || n >= LONG(kMaxModules)) return;
    uint64_t preferred = 0;
    uint64_t size = 0;
    if (!ReadImageHeader(base, &preferred, &size)) return;
    ModuleEntry& e = g_modules[n];
    e.base = uint64_t(uintptr_t(base));
    e.size = size;
    e.preferred = preferred;
    size_t leaf = 0;
    for (size_t i = 0; i < pathLen; ++i)
        if (path[i] == L'\\' || path[i] == L'/') leaf = i + 1;
    size_t k = 0;
    for (size_t i = leaf; i < pathLen && k + 1 < sizeof e.name; ++i)
        e.name[k++] = path[i] < 128 ? char(path[i]) : '?';
    e.name[k] = '\0';
    // Published last: the handler reads the count first, so an entry is either complete or unseen.
    InterlockedExchange(&g_moduleCount, n + 1);
}

// The PEB's own loader list, walked off the tick.  No Win32 module API is called anywhere in this
// file, at install time or after it: EnumProcessModules would drag in psapi and a link edit to a
// file this packet does not own, and the Toolhelp snapshot can fail under load.  The list is read,
// never written, and every DllBase is validated by ReadImageHeader before it is recorded.
void RefreshModules() {
    g_moduleCount = 0;
    const PEB* peb = reinterpret_cast<const PEB*>(NtCurrentTeb()->ProcessEnvironmentBlock);
    if (peb == nullptr || peb->Ldr == nullptr) return;
    const LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;
    unsigned guard = 0;
    for (const LIST_ENTRY* it = head->Flink; it != head && guard < kMaxModules * 2;
         it = it->Flink, ++guard) {
        const LDR_DATA_TABLE_ENTRY* e = CONTAINING_RECORD(it, LDR_DATA_TABLE_ENTRY,
                                                          InMemoryOrderLinks);
        AddModule(e->DllBase, e->FullDllName.Buffer,
                  e->FullDllName.Length / sizeof(wchar_t));
    }
}

const ModuleEntry* FindModule(uint64_t addr) {
    const LONG n = g_moduleCount;
    for (LONG i = 0; i < n; ++i)
        if (addr >= g_modules[i].base && addr < g_modules[i].base + g_modules[i].size)
            return &g_modules[i];
    return nullptr;
}

// A fixed formatter.  The crash path must not call vsnprintf: the CRT's formatter walks its own
// locale state and a stack this handler was entered on may already be the damaged thing.
struct Line {
    char   b[512];
    size_t n;

    void Reset() { n = 0; }
    void Ch(char c) { if (n + 1 < sizeof b) b[n++] = c; }
    void Str(const char* s) {
        if (s == nullptr) s = "(null)";
        while (*s != '\0') Ch(*s++);
    }
    void Hex(uint64_t v, unsigned digits) {
        char t[16];
        unsigned w = 0;
        do { t[w++] = "0123456789abcdef"[v & 0xF]; v >>= 4; } while (v != 0 && w < sizeof t);
        while (w < digits && w < sizeof t) t[w++] = '0';
        while (w-- != 0) Ch(t[w]);
    }
    void Dec(uint64_t v) {
        char t[24];
        unsigned w = 0;
        do { t[w++] = char('0' + (v % 10)); v /= 10; } while (v != 0 && w < sizeof t);
        while (w-- != 0) Ch(t[w]);
    }
    // The plugin log's own line shape, so a [crash] block sorts and greps beside the [pawn] lines
    // it follows rather than looking like a different file.
    void Prefix() {
        SYSTEMTIME now;
        GetLocalTime(&now);
        Pad2(now.wHour); Ch(':'); Pad2(now.wMinute); Ch(':'); Pad2(now.wSecond); Ch('.');
        if (now.wMilliseconds < 100) Ch('0');
        if (now.wMilliseconds < 10) Ch('0');
        Dec(now.wMilliseconds);
        Str(" INFO [pawn] ");
    }
    void Pad2(unsigned v) { if (v < 10) Ch('0'); Dec(v); }
};

void WriteSync(const char* bytes, size_t n) {
    if (g_diag == INVALID_HANDLE_VALUE || n == 0) return;
    DWORD written = 0;
    if (!WriteFile(g_diag, bytes, DWORD(n), &written, nullptr)) return;
#ifndef W2P1_MUTATE_NO_FLUSH
    // Counted rather than assumed: FlushFileBuffers is the difference between a line that survives
    // the kill and a line that does not, and nothing else in this process can observe it.
    if (FlushFileBuffers(g_diag)) ++g_flushes;
#endif
}

void Emit(Line& l) {
    l.Ch('\r');
    l.Ch('\n');
    WriteSync(l.b, l.n);
    l.Reset();
}

// `basename+0xoffset (ida 0xva)` off the snapshot, or `<unmapped>` for an address in no recorded
// image.  No Win32 call, so no loader lock -- see F3 above.
void AppendSite(Line& l, uint64_t addr) {
    const ModuleEntry* m = addr != 0 ? FindModule(addr) : nullptr;
    if (m == nullptr) {
        l.Str("<unmapped>");
        return;
    }
    l.Str(m->name[0] != '\0' ? m->name : "<module>");
    const uint64_t off = addr - m->base;
    l.Str("+0x");
    l.Hex(off, 0);
    l.Str(" (ida 0x");
    l.Hex(m->preferred + off, 0);
    l.Ch(')');
}

void WriteRegisters(Line& l, const CONTEXT& c) {
    struct Reg { const char* name; DWORD64 v; };
    const Reg regs[] = {
        {"rax", c.Rax}, {"rbx", c.Rbx}, {"rcx", c.Rcx}, {"rdx", c.Rdx},
        {"rsi", c.Rsi}, {"rdi", c.Rdi}, {"rbp", c.Rbp}, {"rsp", c.Rsp},
        {"r8",  c.R8},  {"r9",  c.R9},  {"r10", c.R10}, {"r11", c.R11},
        {"r12", c.R12}, {"r13", c.R13}, {"r14", c.R14}, {"r15", c.R15},
    };
    for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i += 4) {
        l.Prefix();
        l.Str("[crash] ");
        for (size_t j = i; j < i + 4; ++j) {
            if (j != i) l.Ch(' ');
            l.Str(regs[j].name);
            l.Ch('=');
            l.Str("0x");
            l.Hex(uint64_t(regs[j].v), 16);
        }
        Emit(l);
    }
}

constexpr unsigned kMaxFrames = 48;

// `phase` is `first-chance` or `unhandled`.  The two are the same block; which arm wrote it is the
// single most important word in it, because only `unhandled` is certainly the fault that ended the
// process -- a `first-chance` block may be a probe somebody's __try was about to swallow.
void WriteCrashBlock(EXCEPTION_POINTERS* ep, const char* phase, LONG seq) {
    static Line l;   // static, not a local: STACK_OVERFLOW is one of the codes this must survive
    l.Reset();
    const EXCEPTION_RECORD& er = *ep->ExceptionRecord;
    const CONTEXT& ctx = *ep->ContextRecord;

    l.Prefix();
    l.Str("[crash] block ");
    l.Dec(uint64_t(seq));
    l.Ch(' ');
    l.Str(phase);
    l.Str(" code=0x");
    l.Hex(uint64_t(er.ExceptionCode), 8);
    l.Str(" flags=0x");
    l.Hex(uint64_t(er.ExceptionFlags), 8);
    l.Str(" at=0x");
    l.Hex(uint64_t(uintptr_t(er.ExceptionAddress)), 16);
    l.Str(" thread=");
    l.Dec(GetCurrentThreadId());
    Emit(l);

    l.Prefix();
    l.Str("[crash] site=");
    AppendSite(l, uint64_t(uintptr_t(er.ExceptionAddress)));
    Emit(l);

    if ((er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
         er.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        er.NumberParameters >= 2) {
        // ExceptionInformation[0] is 0 read / 1 write / 8 DEP, and [1] is the address that could
        // not be touched -- the two facts that separate "we dereferenced a stale pointer" from
        // "we jumped into freed code".
        const ULONG_PTR kind = er.ExceptionInformation[0];
        l.Prefix();
        l.Str("[crash] access=");
        l.Str(kind == 0 ? "read" : kind == 1 ? "write" : kind == 8 ? "execute" : "unknown");
        l.Str(" va=0x");
        l.Hex(uint64_t(er.ExceptionInformation[1]), 16);
        Emit(l);
    }

    l.Prefix();
    l.Str("[crash] rip=0x");
    l.Hex(uint64_t(ctx.Rip), 16);
    l.Str(" rsp=0x");
    l.Hex(uint64_t(ctx.Rsp), 16);
    l.Str(" rbp=0x");
    l.Hex(uint64_t(ctx.Rbp), 16);
    Emit(l);
    WriteRegisters(l, ctx);

    l.Prefix();
    l.Str("[crash] lastmarker=");
    l.Str(g_lastMarker[0] != '\0' ? g_lastMarker : "(none)");
    Emit(l);

    if (g_staged.seq != 0 && g_staged.seq != seq) {
        // The last fatal first-chance exception, named even when this block is a different one.
        l.Prefix();
        l.Str("[crash] staged block ");
        l.Dec(uint64_t(g_staged.seq));
        l.Str(" code=0x");
        l.Hex(uint64_t(g_staged.code), 8);
        l.Str(" thread=");
        l.Dec(g_staged.threadId);
        l.Str(" at=");
        AppendSite(l, g_staged.address);
        Emit(l);
    }

    // WALK 1 -- the capture.  Cheap and reliable, but it starts inside THIS function: the faulting
    // frame is not on it, which is precisely the frame the last three runs needed.
    void* frames[kMaxFrames];
    const USHORT captured = RtlCaptureStackBackTrace(0, kMaxFrames, frames, nullptr);
    for (USHORT i = 0; i < captured; ++i) {
        l.Prefix();
        l.Str("[crash] capture ");
        l.Dec(i);
        l.Str(": 0x");
        l.Hex(uint64_t(uintptr_t(frames[i])), 16);
        l.Ch(' ');
        AppendSite(l, uint64_t(uintptr_t(frames[i])));
        Emit(l);
    }

    // WALK 2 -- the unwind, from the faulting CONTEXT, so frame 0 IS the fault.  A copy, because
    // RtlVirtualUnwind rewrites the context it is handed and the engine's own handler runs next.
    CONTEXT walk = ctx;
    for (unsigned i = 0; i < kMaxFrames && walk.Rip != 0; ++i) {
        l.Prefix();
        l.Str("[crash] frame ");
        l.Dec(i);
        l.Str(": 0x");
        l.Hex(uint64_t(walk.Rip), 16);
        l.Ch(' ');
        AppendSite(l, uint64_t(walk.Rip));
        Emit(l);

        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(walk.Rip, &imageBase, nullptr);
        if (entry == nullptr) {
            // A leaf function has no unwind record at all; its return address is simply at RSP.
            uint64_t ret = 0;
            if (!SafeReadU64(reinterpret_cast<const void*>(uintptr_t(walk.Rsp)), &ret)) break;
            if (ret == 0) break;
            walk.Rip = ret;
            walk.Rsp += sizeof(uint64_t);
            continue;
        }
        PVOID   handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, walk.Rip, entry, &walk, &handlerData,
                         &establisher, nullptr);
    }

    l.Prefix();
    l.Str("[crash] end block ");
    l.Dec(uint64_t(seq));
    Emit(l);
}

// The codes worth a block.  An ALLOW-list and not a deny-list: the engine's first-chance traffic
// is a moving population (0xE06D7363 from every C++ throw, 0x406D1388 whenever a thread is named,
// DBG_PRINTEXCEPTION from its own debug prints) and a deny-list would let the next unnamed one
// through and bury the block this exists to write.
bool FatalCode(DWORD code) { return CrashCodeIsFatal(uint32_t(code)); }

// Answers the sequence number to write under, or 0 when this handler must not write: either the
// cap is spent or another block is in flight on this or another thread.  `capped` is false for the
// unhandled arm, whose block is the one that must never be dropped.
LONG ClaimBlock(bool capped) {
    if (InterlockedCompareExchange(&g_inHandler, 1, 0) != 0) return 0;
    if (capped && g_blocks >= kMaxBlocks) {
#ifndef W2P1_MUTATE_LATCH_NEVER_RELEASED
        InterlockedExchange(&g_inHandler, 0);
#endif
        return 0;
    }
    return InterlockedIncrement(&g_blocks);
}

void ReleaseBlock() {
#ifndef W2P1_MUTATE_LATCH_NEVER_RELEASED
    // ★ RELEASED, and this is the whole of F1's fix.  A latch that stays taken makes the FIRST
    // swallowed probe fault the only thing this logger ever reports.
    InterlockedExchange(&g_inHandler, 0);
#endif
}

LONG CALLBACK CrashVeh(EXCEPTION_POINTERS* ep) {
    if (ep == nullptr || ep->ExceptionRecord == nullptr || ep->ContextRecord == nullptr)
        return EXCEPTION_CONTINUE_SEARCH;
    if (!FatalCode(ep->ExceptionRecord->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;

    const LONG seq = ClaimBlock(true);
    if (seq != 0) {
        // STAGED FIRST, and with no I/O in it: whatever happens to the write, the unhandled arm
        // can still name the last fatal first-chance exception this process took.
        g_staged.seq = seq;
        g_staged.code = ep->ExceptionRecord->ExceptionCode;
        g_staged.threadId = GetCurrentThreadId();
        g_staged.address = uint64_t(uintptr_t(ep->ExceptionRecord->ExceptionAddress));
        WriteCrashBlock(ep, "first-chance", seq);
        ReleaseBlock();
    }
    // NEVER EXCEPTION_CONTINUE_EXECUTION and never "handled": this logger observes, and swallowing
    // the fault would turn a crash we can read into a corrupted process we cannot.
    return EXCEPTION_CONTINUE_SEARCH;
}

// The commit arm.  It runs only for an exception NOTHING handled, so its block is the fault that
// ended the process -- the one distinction a first-chance handler cannot make on its own.  It is
// not capped, and it chains: the engine's previous filter is called and its answer is returned, so
// whatever the engine does about its own crash still happens.
LONG WINAPI CrashUef(EXCEPTION_POINTERS* ep) {
    if (ep != nullptr && ep->ExceptionRecord != nullptr && ep->ContextRecord != nullptr) {
        const LONG seq = ClaimBlock(false);
        if (seq != 0) {
            WriteCrashBlock(ep, "unhandled", seq);
            ReleaseBlock();
        }
    }
    if (g_prevUef != nullptr) return g_prevUef(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

// The codes worth a block.  An ALLOW-list and not a deny-list: the engine's first-chance traffic
// is a moving population (0xE06D7363 from every C++ throw, 0x406D1388 whenever a thread is named,
// DBG_PRINTEXCEPTION from its own debug prints) and a deny-list would let the next unnamed one
// through and bury the block this exists to write.
//
// The two NTSTATUS members have no EXCEPTION_* spelling in <winnt.h>, so they are named here from
// ntstatus.h's values.  They are on the list because L2 #17b died of 0xC0000374 and this logger
// wrote nothing at all: the heap manager reports its own damaged metadata through
// RtlReportCriticalFailure, which raises the status as an ordinary exception, so the block names
// the REPORTER's frames rather than the bad write -- the module, the thread and the stack are
// still three facts a status code alone cannot give.  0xC0000409 is /GS and every other
// __fastfail report; some of those never reach a user-mode handler at all, and admitting it costs
// one comparison against the chance that this one does.
bool CrashCodeIsFatal(uint32_t code) {
    constexpr uint32_t kStatusHeapCorruption = 0xC0000374u;
    constexpr uint32_t kStatusStackBufferOverrun = 0xC0000409u;
    const bool fatal = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ||
                       code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                       code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW ||
                       code == EXCEPTION_NONCONTINUABLE_EXCEPTION ||
                       code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED
#ifndef W2P1_MUTATE_CRASH_FILTER_NO_HEAP
                       || code == kStatusHeapCorruption || code == kStatusStackBufferOverrun
#endif
        ;
#ifdef W2P1_MUTATE_FILTER_INVERTED
    return !fatal;
#else
    return fatal;
#endif
}

void RefreshCrashModules() { RefreshModules(); }

bool InstallCrashLogger(const wchar_t* logPath) {
    if (g_veh != nullptr) return false;
    if (logPath == nullptr || logPath[0] == L'\0') return false;
    // log.cpp's uninstall rule (S4.5.3 rule 5), enforced at the same point it enforces it: the log
    // is one of exactly two paths this plugin may create.  A test redirect runs with no resolved
    // install root and is exempt by construction, exactly as log.cpp's own redirect is.
    if (replay::Resolved() && !replay::IsWritablePath(logPath)) return false;
    HANDLE file = CreateFileW(logPath, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    RefreshModules();   // off the crash path, once, before any handler can need it
    // FIRST, not last: the engine installs handlers of its own and a logger that ran after them
    // would only ever see what they had already decided not to pass on.
    g_veh = AddVectoredExceptionHandler(1, CrashVeh);
    if (g_veh == nullptr) {
        CloseHandle(file);
        return false;
    }
    g_prevUef = SetUnhandledExceptionFilter(CrashUef);
    g_uefInstalled = true;
    g_diag = file;
    g_inHandler = 0;
    g_blocks = 0;
    g_flushes = 0;
    g_staged = Staged{0, 0, 0, 0};
    g_markersThisTick = 0;
    g_lastMarker[0] = '\0';
    return true;
}

bool EnsureCrashLogger(const wchar_t* logPath, const char* mode) {
#ifdef CRASHLOG_PLAYBACK_ONLY
    (void)logPath;
    (void)mode;
    return false;
#else
    if (!InstallCrashLogger(logPath)) return false;
    log::Direct(LogLevel::Info, "crash", "logger installed (mode=%s)",
                mode != nullptr ? mode : "unknown");
    return true;
#endif
}

void RemoveCrashLogger() {
    if (g_veh != nullptr) {
        RemoveVectoredExceptionHandler(g_veh);
        g_veh = nullptr;
    }
    if (g_uefInstalled) {
        SetUnhandledExceptionFilter(g_prevUef);
        g_prevUef = nullptr;
        g_uefInstalled = false;
    }
    if (g_diag != INVALID_HANDLE_VALUE) {
        CloseHandle(g_diag);
        g_diag = INVALID_HANDLE_VALUE;
    }
    g_inHandler = 0;
    g_blocks = 0;
    g_flushes = 0;
    g_staged = Staged{0, 0, 0, 0};
    g_markersThisTick = 0;
    g_lastMarker[0] = '\0';
    g_moduleCount = 0;
}

uint64_t CrashLogFlushes() { return g_flushes; }

uint32_t CrashBlocksWritten() { return uint32_t(g_blocks); }

const char* LastHopMarker() { return g_lastMarker; }

void ResetHopMarkerBudget() {
    g_markersThisTick = 0;
    g_markRequestsThisTick = 0;
}

unsigned HopMarkerBudget() { return kMarkersPerTick; }

unsigned HopMarksRequested() { return g_markRequestsThisTick; }
unsigned HopDrivenPassesPerTick() { return kDrivenPassesPerTick; }

LoadoutCensus SummariseLoadouts(const LoadoutOutcome* rows, uint16_t n) {
    LoadoutCensus c;
    if (rows == nullptr) return c;
    for (uint16_t i = 0; i < n; ++i) {
        const LoadoutOutcome& r = rows[i];
        if (r.alive) {
            c.anyDocument = c.anyDocument || r.docPresent;
#ifndef W2P1_MUTATE_CENSUS_DENOMINATOR_ATTEMPTED
            // THE DENOMINATOR IS THE LOAD'S ENTRANTS, not the ones that got as far as being
            // asked (part2 F15).  An entrant parked on Hop::PhysicsCi is never attempted, and a
            // fraction that counts only attempts reads 2/2 over a naked pawn -- for ever, because
            // a hop deferral does not latch and nothing else on this line moves.
            ++c.live;
#endif
            if (!r.attempted) {
                ++c.deferred;
                if (c.firstDeferral == nullptr) c.firstDeferral = r.deferralWhy;
            }
        }
        if (!r.attempted) continue;
#ifdef W2P1_MUTATE_CENSUS_FIRST_HIT
        // The defect: the first attempted slot is reported for the whole load.
        if (c.attempted != 0) continue;
#endif
        ++c.attempted;
#ifdef W2P1_MUTATE_CENSUS_DENOMINATOR_ATTEMPTED
        ++c.live;
#endif
        c.compared += r.compared;
        c.matched += r.matched;
        // AUTHORED IS THE ACCEPTANCE'S OWN VERDICT, not a second one derived from the tallies.
        // `None` already means no compared row mismatched and every one of them landed on a
        // named arm; requiring matched == compared demoted an entrant whose slots the engine
        // latched (pending) or gated on alignment to not-authored, so RUN-34's line never
        // printed the applied clause at all -- for loadouts that were applied.
        if (r.why == identity_access::LoadoutRefusal::None && r.compared != 0) {
            ++c.authored;
        } else if (!c.anyRefusal) {
            c.anyRefusal = true;
            c.firstWhy = r.why;
            c.firstSourceWhy = r.sourceWhy;
        }
    }
    return c;
}

void FormatLoadoutClause(const LoadoutCensus& c, char* out, size_t cap) {
    if (out == nullptr || cap == 0) return;
    out[0] = 0;
    const char* sep = c.firstSourceWhy != nullptr ? " -- " : "";
    const char* word = c.firstSourceWhy != nullptr ? c.firstSourceWhy : "";
    if (c.attempted == 0) {
#ifdef W2P1_MUTATE_SYNTHESISED_RECORDING_VERDICT
        // The defect DIAG-APPLY-KEY sec A names: a verdict synthesised from a zero-initialised
        // RecordedLoadout, which only ever reads back DocumentNotRecorded / SlotKeysNotRecorded
        // about a struct no file was read into -- printed for a run whose recording DID carry
        // keys, while the one true word (the hop deferral) was dropped.
        identity_access::RecordedLoadout recorded{};
        recorded.documentRecorded = c.anyDocument;
        snprintf(out, cap, "not-authored (%s) -- 0/0 attempted of %u live",
                 identity_access::LoadoutRefusalName(
                     identity_access::RecordedLoadoutRefusal(recorded)),
                 unsigned(c.live));
#else
        // DIAG-APPLY-KEY sec A: when the entrant is PARKED, nothing-attempted is a property of THIS
        // session and the held deferral (e.g. ActorNotEnabled) is the one true word -- not a
        // verdict synthesised from a zero-initialised RecordedLoadout, whose hasSlotKeys defaults
        // false and so names SlotKeysNotRecorded about keys the recording DID hold (RUN-35).  A
        // held deferral takes precedence; with nothing parked the recorded-side name is honest.
        if (c.deferred != 0) {
            snprintf(out, cap, "not-attempted -- 0/0 attempted of %u live, deferred: %s x%u",
                     unsigned(c.live),
                     c.firstDeferral != nullptr ? c.firstDeferral : "unstated",
                     unsigned(c.deferred));
        } else {
            identity_access::RecordedLoadout recorded{};
            recorded.documentRecorded = c.anyDocument;
            snprintf(out, cap, "not-authored (%s) -- 0/0 attempted of %u live",
                     identity_access::LoadoutRefusalName(
                         identity_access::RecordedLoadoutRefusal(recorded)),
                     unsigned(c.live));
        }
#endif
    } else if (c.authored == 0) {
        snprintf(out, cap, "not-authored (%s)%s%s -- 0/%u attempted of %u live",
                 identity_access::LoadoutRefusalName(c.firstWhy), sep, word,
                 unsigned(c.attempted), unsigned(c.live));
    } else {
        int w = snprintf(out, cap, "applied %u/%u attempted of %u live (%u/%u pairs)",
                         unsigned(c.authored), unsigned(c.attempted), unsigned(c.live),
                         unsigned(c.matched), unsigned(c.compared));
        if (c.anyRefusal && w > 0 && size_t(w) < cap) {
            w += snprintf(out + w, cap - size_t(w), ", first refusal %s%s%s",
                          identity_access::LoadoutRefusalName(c.firstWhy), sep, word);
        }
        // A parked entrant is the difference between K==N and K==N==M, and it is the reading B1
        // turns on, so it is named on the line rather than left to the per-slot lines above.
        if (c.deferred != 0 && w > 0 && size_t(w) < cap) {
            snprintf(out + w, cap - size_t(w), ", deferred: %s x%u",
                     c.firstDeferral != nullptr ? c.firstDeferral : "unstated",
                     unsigned(c.deferred));
        }
    }
}

void MarkHop(unsigned hop, const char* call, const void* ctx, uint16_t index) {
    ++g_markRequestsThisTick;
    if (g_diag == INVALID_HANDLE_VALUE) return;
    // ReopenAttemptWindow makes every slot's attempt due again the moment a
    // provision lands, so one tick can drive twelve hop chains -- ~9 flushed writes each, on the
    // game thread, inside the exact window RUN-27 narrowed the fault to.  The budget bounds the
    // instrument so it cannot become the thing it is measuring, and it SAYS when it has run out
    // rather than going quiet.
    if (g_markersThisTick >= kMarkersPerTick) {
        if (g_markersThisTick == kMarkersPerTick) {
            ++g_markersThisTick;
            Line q;
            q.Reset();
            q.Prefix();
            q.Str("markers suppressed for the rest of this tick (budget ");
            q.Dec(kMarkersPerTick);
            q.Ch(')');
            Emit(q);
        }
        return;
    }
    ++g_markersThisTick;
    Line l;
    l.Reset();
    l.Prefix();
    const size_t body = l.n;
    l.Str("hop ");
    l.Dec(hop);
    l.Ch(' ');
    l.Str(call);
    l.Str(": ctx=0x");
    l.Hex(uint64_t(uintptr_t(ctx)), 16);
    l.Str(" index=0x");
    l.Hex(index, 4);
    // Remembered BEFORE the write, so a call that never returns still has its marker in the block:
    // the file write is the slow half and the fault we are hunting lands past it either way.
    size_t k = 0;
    for (size_t i = body; i < l.n && k + 1 < sizeof g_lastMarker; ++i) g_lastMarker[k++] = l.b[i];
    g_lastMarker[k] = '\0';
    Emit(l);
}

// One marker per engine call, and the only reason it is a wrapper is that the mutant that
// deletes every marker has to be one edit rather than eleven.
void Mark(unsigned hop, const char* call, const ActorHops& h, uint16_t index) {
#ifndef W2P1_MUTATE_MARKER_BEFORE_CALL_REMOVED
    MarkHop(hop, call, h.ctx, index);
#endif
}

PhysicsCiReq PhysicsCiAwaiting(const ActorHops& h, uint16_t index) {
    if (index == kSlotUnmapped) return PhysicsCiReq::Unknown;
    // Order matters: a surface that says the CI is built has no requirement outstanding, and one
    // that cannot be asked answers Unknown rather than borrowing the built answer.
    if (h.physicsCi != nullptr) {
        Mark(4, "physicsCi", h, index);
        if (h.physicsCi(h.ctx, index)) return PhysicsCiReq::None;
    }
    if (h.physicsCiAwaiting == nullptr) return PhysicsCiReq::Unknown;
    Mark(4, "physicsCiAwaiting", h, index);
    return h.physicsCiAwaiting(h.ctx, index);
}

namespace {

static_assert(kRoleNotRecorded == ::replay::capture::kRosterNoTeam,
              "the chain's 'never recorded' role is the roster's own fill value, not a second "
              "sentinel that could drift away from it");

// The one role value IDENT.SERVER_ASSIGN_TEAM does not treat as a request.  sub_1401AB730:68
// rewrites it to whatever the word already holds -- `if ( v3 == 3 ) v3 = (*v7 >> 10) & 7;` -- so
// asking for it outside a social lobby lands on the same-team early-out by construction.
constexpr uint16_t kRoleRequestKeepsCurrent = 3;

// The role to bounce THROUGH, derived from the engine's own participating predicate rather than
// from a hand-written pair: any role its dispatch treats as participating, that it will actually
// take as a request, and that differs from the recorded one satisfies every same-team early-out
// on both legs.  kRoleNotRecorded when the recorded role has no such partner, which is the arm
// saying the bounce cannot be built rather than building a wrong one.
uint16_t ReSeatAwayRole(uint16_t recorded) {
    if (recorded == kRoleRequestKeepsCurrent) return kRoleNotRecorded;
    for (uint16_t role = 0; role < uint16_t(facts::NETGAME_USER_ROLE_BOUND); ++role) {
        if (role != recorded && role != kRoleRequestKeepsCurrent && !RecordedAsSpectatorRole(role))
            return role;
    }
    return kRoleNotRecorded;
}

// Our own "inside an engine call" flag.  S14.6: the WHOLE participating arm -- assign team, user
// settings, the participating dispatch, the enable, the chassis and the CI chain -- runs
// synchronously inside A3, from our tick.  A hook of ours that fires anywhere in that arm must not
// be able to start a second transition, so the latch spans A2 and A3 together and not each call.
bool g_inEngineCall = false;

// The engine's own answer to the refresh, put in the plugin log where a launch greps it.  L3 #31
// issued three refreshes and recorded nothing but "the actor is still off"; which of
// sub_1401B0D80's two exits they took was the one cheap fact that would have named the gate, and
// it was thrown away at the call site.
const char* RefreshAnswerMark(RefreshAnswer a) {
    switch (a) {
        case RefreshAnswer::Declined:     return "a3 refresh#declined(keyUnchanged)";
        case RefreshAnswer::Redispatched: return "a3 refresh#redispatched";
        default:                          return "a3 refresh#notIssued";
    }
}

ParticipateOutcome RefusedWith(ParticipateRefusal why) {
    return ParticipateOutcome{Participation::Refused, why};
}

}  // namespace

// The latch itself is declared in the header because the dress path takes it too:
// one flag for every engine call this plugin issues from the stage-0 tick, not one per site.
EngineCallLatch::EngineCallLatch() : taken(!g_inEngineCall) {
#ifndef W2P1_MUTATE_LATCH_OFF
    if (taken) g_inEngineCall = true;
#endif
}

EngineCallLatch::~EngineCallLatch() {
    if (taken) g_inEngineCall = false;
}

FeatureState ParticipationFeatureState(const ActorHops& h) {
#ifdef W2P1_MUTATE_REPORT_ARMED_WHEN_GATED
    (void)h;
    return FeatureState{true, ""};
#else
    if (h.writeSpawned == nullptr) return FeatureState{false, "NoSpawnedWriter"};
    if (h.assignTeam == nullptr)   return FeatureState{false, "NoAssignTeam"};
    if (h.teamOf == nullptr)       return FeatureState{false, "NoTeamRead"};
    if (h.placed == nullptr)       return FeatureState{false, "NoSpawnedRead"};
    return FeatureState{true, ""};
#endif
}

ParticipateOutcome Participate(const ActorHops& h, uint16_t index, HopDrive& drive) {
    if (index == kSlotUnmapped) return RefusedWith(ParticipateRefusal::NoDriver);
    if (drive.gate == nullptr)  return RefusedWith(ParticipateRefusal::NoGate);
    if (h.writeSpawned == nullptr || h.assignTeam == nullptr || h.teamOf == nullptr ||
        h.placed == nullptr) {
        return RefusedWith(ParticipateRefusal::NoDriver);
    }

    // A0's partition, and it is not a bound: an entrant below the netgame's LOCAL-user count is a
    // local user whose pawn the engine's own first block already handles.  Refused by name rather
    // than authored for.
    if (h.isLocalUser != nullptr) {
        Mark(0, "a0 isLocalUser", h, index);
        const LocalUserAnswer who = h.isLocalUser(h.ctx, index);
        if (who == LocalUserAnswer::Local)
            return RefusedWith(ParticipateRefusal::EntrantIsLocalUser);
        // FAIL CLOSED.  A count that would not prove cannot say this index is not the viewer's
        // own, and the arm below it issues an engine call that does no index check of its own.
        if (who == LocalUserAnswer::Unreadable)
            return RefusedWith(ParticipateRefusal::ViewerIndexRefused);
    }

    // The recorded role decides BEFORE anything is written.  A recorded spectator had no pawn in
    // the recording and must not be given one now -- and that means not even its `spawned` record
    // is authored, because that record is what the engine's participating arm reads.
#ifndef W2P1_MUTATE_TEAM_MAP_CLAMP
    if (drive.recordedRole == kRoleNotRecorded)
        return RefusedWith(ParticipateRefusal::TeamNotRecorded);
    if (drive.recordedRole >= uint16_t(facts::NETGAME_USER_ROLE_BOUND))
        return RefusedWith(ParticipateRefusal::TeamOutOfRange);
    if (RecordedAsSpectatorRole(drive.recordedRole))
        return RefusedWith(ParticipateRefusal::RecordedAsSpectator);
    const uint16_t recorded = drive.recordedRole;
#else
    // The clamp this design refuses by name: a recorded spectator forced onto a playing team.
    const uint16_t recorded =
        (RecordedAsSpectatorRole(drive.recordedRole) || drive.recordedRole == kRoleNotRecorded ||
         drive.recordedRole >= uint16_t(facts::NETGAME_USER_ROLE_BOUND))
            ? uint16_t(0)
            : drive.recordedRole;
#endif

    // One authored transition per epoch per slot.  Checked before the latch so a settled slot
    // costs nothing at all.
    if (drive.gate->issuedEpoch == drive.gate->epoch)
        return ParticipateOutcome{Participation::SkippedAlreadyDone, ParticipateRefusal::None};

    const EngineCallLatch latch;
    if (!latch.taken) return RefusedWith(ParticipateRefusal::Reentrant);

#ifndef W2P1_MUTATE_PARTICIPATE_ORDER
    // A2 -- the `spawned` record, on the side the engine's own reader selects.
    Mark(0, "a2 writeSpawned", h, index);
    const SpawnedWrite wrote = h.writeSpawned(h.ctx, index);
    if (wrote == SpawnedWrite::Unwritable)
        return RefusedWith(ParticipateRefusal::SpawnedRecordUnwritable);
    if (wrote == SpawnedWrite::Unavailable) return RefusedWith(ParticipateRefusal::NoDriver);

    // A2's postcondition is the CONSUMER's own function, not a field: side-agnostic by
    // construction, so it cannot read green while the engine reads the other record.
    Mark(0, "a2 spawned", h, index);
    if (!h.placed(h.ctx, index)) return RefusedWith(ParticipateRefusal::SpawnedRecordNotWritten);
#else
    const SpawnedWrite wrote = SpawnedWrite::Issued;
#endif

    // A3's early-out gate, read off the engine's own packed word.  The engine skips its entire
    // body when the requested role already equals this, so issuing the call without reading it
    // would make a no-op indistinguishable from a transition.
    uint8_t liveRole = 0xFF;
    uint8_t liveSlot = 0xFF;
    Mark(0, "a3 teamOf", h, index);
    if (!h.teamOf(h.ctx, index, &liveRole, &liveSlot))
        return RefusedWith(ParticipateRefusal::AssignTeamNotApplied);

#ifndef W2P1_MUTATE_PARTICIPATE_ALWAYS
    if (uint16_t(liveRole) == recorded) {
        // A3 IS DEAD ON THIS ARM, and booking `Issued` off it is what hid L3 #29b for a whole
        // launch.  The engine's own `Server Assigning Team` auto-pick had already written the
        // recorded role into the word, so IDENT.SERVER_ASSIGN_TEAM would take its `v10 != v3`
        // early-out and drive nothing -- while the participating dispatch that arm exists to
        // re-run had already run, twice, against a `spawned` record of 0.  So this arm does not
        // report success off a postcondition that was true before we arrived: it either drives
        // the engine's OWN late-join re-provision, or it refuses by name.
#ifdef W2P1_MUTATE_EARLYOUT_BOOKS_ISSUED
        // The defect exactly as L3 #29b measured it: an arm that calls no engine function books
        // the issue, latches the once-per-epoch gate, and leaves the actor disabled forever.
        if (wrote == SpawnedWrite::Issued) {
            drive.gate->issuedEpoch = drive.gate->epoch;
            drive.gate->issuedRole  = recorded;
            drive.gate->via         = ParticipateDrive::AssignTeam;
            ++drive.gate->issues;
            return ParticipateOutcome{Participation::Issued, ParticipateRefusal::None};
        }
        return ParticipateOutcome{Participation::SkippedAlreadyDone, ParticipateRefusal::None};
#else
        // ONE read of the engine's own enable predicate, and every branch below decides off it:
        // the re-seat's postcondition, the genuine skip and the fallback's precondition are the
        // same question, so they are asked once.
        bool actorOn = false;
        if (h.enabled != nullptr) {
            Mark(0, "a3 enabled#1", h, index);
            actorOn = h.enabled(h.ctx, index);
        }

        // THE DEFERRED POSTCONDITIONS, read on a LATER pump than the call that earned them.  Never
        // an engine call's return: neither IDENT.SERVER_ASSIGN_TEAM nor NETGAME.SET_PLAYER_ACTOR
        // answers whether the dispatch it reached enabled anything, and this re-read is the same
        // call the engine's own entrant dump renders as `(enabled)`/`(disabled)`.
        const bool reSeatPending = drive.gate->reSeatPending;

        if (actorOn) {
            if (reSeatPending) {
                // The async enable landed in a window the pending latch was STILL held for: this
                // is the coincidence the settle budget exists to catch.  Clear the latch and book.
                drive.gate->reSeatPending = false;
                drive.gate->reSeatSettle  = 0;
                drive.gate->issuedEpoch = drive.gate->epoch;
                drive.gate->issuedRole  = recorded;
                drive.gate->via         = ParticipateDrive::AssignTeamReSeat;
                ++drive.gate->issues;
                return ParticipateOutcome{Participation::Issued, ParticipateRefusal::None};
            }
            // THE SEEDED ADD.  The reply seed wrote `spawned` before this session issued the
            // entrant add, so the add's own role transition enabled the actor: the engine call
            // that authored this state IS ours, and the enable it is booked off was read from the
            // engine a line above.  Without the seed's own word this stays the skip below.
            if (h.seededBeforeAdd != nullptr) {
                Mark(0, "a3 seededBeforeAdd", h, index);
                if (h.seededBeforeAdd(h.ctx, index)) {
                    drive.gate->issuedEpoch = drive.gate->epoch;
                    drive.gate->issuedRole  = recorded;
                    drive.gate->via         = ParticipateDrive::SeededEntrantAdd;
                    ++drive.gate->issues;
                    return ParticipateOutcome{Participation::Issued, ParticipateRefusal::None};
                }
            }
            // The one genuine skip on this arm, and it is a READ of the engine rather than a
            // belief about our own earlier calls: the role matches, the record is there and the
            // actor is already on, so there is nothing for the dispatch to do and no call is
            // made.  It is still not a provision -- this session drove nothing -- and the
            // `provisioned` gate refuses it by name, which is what keeps a re-read from becoming
            // an adoption.
            return ParticipateOutcome{Participation::SkippedAlreadyDone,
                                      ParticipateRefusal::None};
        }

        // THE SETTLE LATCH.  actorOn is false and a re-seat is pending -- the engine's
        // dispatch enables the actor ASYNCHRONOUSLY, a tick or more after the call, so its enable
        // lands a window or more after the one that issued it.  Consuming the pending flag here (as
        // the pre-fix code did, above the actorOn test) discarded it before the enable could ever
        // coincide with it, so the re-seat -> Issued transition at the top could never fire and the
        // slot churned on Enabled:refused / ParticipationNotIssued forever
        // (DIAG-PARTICIPATION-REGRESSION).  So the flag is HELD: while the settle budget is unspent
        // this window DEFERS without re-driving -- the arms below re-seat the actor and would
        // undo the very enable being waited on -- and the flag is cleared only above on success or
        // here on exhaustion, where the give-up gets its own name rather than a silent drop.
#ifndef W2P1_MUTATE_RESEAT_PENDING_CLEARED_EARLY
        if (reSeatPending) {
            if (drive.gate->reSeatSettle < kReSeatSettleWindows) {
                ++drive.gate->reSeatSettle;
                return RefusedWith(ParticipateRefusal::ReSeatAwaitingReRead);
            }
            drive.gate->reSeatPending = false;
            drive.gate->reSeatSettle  = 0;
            return RefusedWith(ParticipateRefusal::ReSeatSettleExhausted);
        }
#else
        // The defect this latch fixes: consume the pending flag in the first window that reads the
        // actor still off, before the engine's async enable can be observed with the flag set.
        drive.gate->reSeatPending = false;
#endif

        // The two re-entries this arm holds, and NEITHER being bound is what the refusal names.
        const uint16_t away = ReSeatAwayRole(recorded);
        const bool canReSeat = h.assignTeam != nullptr && h.teamOf != nullptr &&
                               h.enabled != nullptr && away != kRoleNotRecorded;
        const bool canRefresh = h.refreshPlayerActor != nullptr &&
                                h.actorAssetsReady != nullptr && h.enabled != nullptr;
        if (!canReSeat && !canRefresh)
            return RefusedWith(ParticipateRefusal::ParticipationNotReRun);

        // THE WINDOW BUDGET.  Windows that call nothing are cheap, so they get the wide one; the
        // per-call budgets below are separate and far tighter.
        if (drive.gate->refreshAttempts >= kRefreshAttemptMax)
            return RefusedWith(ParticipateRefusal::RefreshAttemptsExhausted);
        ++drive.gate->refreshAttempts;

        // THE ORDERING, and it is THIS DESIGN'S rule, not a fact about the engine: the record's
        // reader inside both re-entries is the same function A2's postcondition just called, so
        // the engine WOULD see a same-pump write.  We do not feed a record this pump authored into
        // an engine call on the same pump anyway -- the rule the dress already follows
        // (`LoadoutSameFrameAsParticipation`), one step earlier in the chain.  A deferral, not a
        // refusal of the work: the gate is NOT latched, so the next window re-enters, the write
        // answers AlreadySet, and the re-dispatch goes out then.
#ifndef W2P1_MUTATE_REFRESH_BEFORE_SPAWNED_LANDS
        if (wrote == SpawnedWrite::Issued)
            return RefusedWith(ParticipateRefusal::RefreshDeferredToNextPump);
#endif

#ifndef W2P1_MUTATE_EQUAL_ROLE_SKIPS_ASSIGN
        // THE RE-SEAT, and it is the LAST RESORT -- the only arm here that moves something the
        // recording owns.  It is kept because it is the only re-entry a launch has ever seen turn
        // this actor on: L3 #30 drove IDENT.SERVER_ASSIGN_TEAM against a live role of 2 and the
        // actor came on in the same block.
        //
        // What makes the assign reach the dispatch is the ROLE CHANGE, not the call.  The engine
        // tests it three times over: sub_1401AB730:73 wraps its entire body, the sub_1401B1FD0
        // call included, in `if ( v10 >= 5u || v10 != v3 )`; :187 tests it again and :189 prints
        // `ServerAssignTeam: attempting to assign to same team - early out`; and sub_1401B1FD0:36
        // opens with `if ( v11 != a3 )`.  Re-seating with the SAME role is therefore a proven
        // no-op at three nested guards.  So the role is bounced through the other participating
        // role and back: each leg satisfies every one of those tests, each ends in
        // sub_1401C9BA0's participating dispatch -- which enables through
        // `if ( sub_140177F80(a1, v2) ) sub_1404EDA40(...)` at :334-336, the `spawned` read that
        // is now 1 -- and the pair is symmetric in the engine's own team counters at
        // [ng + 2*role + 230].  It is NOT symmetric in the Team Slot, which is why both the
        // entrant-count precondition above and the slot comparison below exist.
        if (canReSeat) {
#ifdef W2P1_MUTATE_BOUNCE_BEFORE_REFRESH
            // The order this design forbids: the bounce -- the only arm that reallocates the
            // entrant's recorded Team Slot -- gets first refusal, before either arm that touches
            // nothing the recording owns has even been asked.
            const bool reSeatTurn = true;
#else
            // LAST RESORT, and this is what makes it one.  The bounce may not be issued until the
            // refresh has been issued AND told us by its own return that sub_1401B0D80:90-91 is
            // the gate in the way.  The refresh moves nothing the recording owns; this arm
            // reallocates a Team Slot that the recording DOES own, and it is the ONLY re-entry a
            // launch has ever seen turn this actor on (L3 #30).  Where it cannot run -- two or
            // more recorded entrants -- it is refused by name below and NOTHING replaces it: the
            // only other way past that gate is freeing the actor, and that orphans the entrant
            // under the engine's per-frame pose loop (DriveGate::refreshDeclined).
            const bool reSeatTurn = drive.gate->refreshDeclined;
#endif
            if (reSeatTurn && drive.gate->reSeatIssues < kReSeatIssueMax) {
#ifndef W2P1_MUTATE_BOUNCE_ON_MULTI
                // THE PRECONDITION THAT MAKES THE BOUNCE SURVIVABLE, and it is about the recorded
                // Team Slot, not the role.  IDENT.SERVER_ASSIGN_TEAM is a slot ALLOCATOR: every
                // call pushes the entrant's live slot onto its old team's free heap and
                // `--count[old]` (sub_1401AB730:198-217), then takes a new one -- and when the
                // destination's heap holds one entry or none it IGNORES the heap and grabs
                // `count[dest]` (:227-230, `Grabbing last teamslot`).  So the leg home lands on
                // `count[T]-1`, which is the entrant's own slot ONLY when it was the sole member
                // of team T.  With two or more it lands on a slot another player already holds,
                // and the entrant's own slot stays in the free list.  Team Slot is recorded
                // (roster.h:200), rendered by the engine's entrant dump and keys the team spawn
                // point, so a 4v4 replay is exactly the case this must refuse.
                if (drive.entrantCount != 1)
                    return RefusedWith(ParticipateRefusal::BounceUnsafeForMultipleEntrants);
#endif
                // The slot the entrant holds BEFORE either leg, read this window off the engine's
                // own packed word.  0xFF is `teamOf` saying it could not read one, and a
                // comparison against a value we never got is not a comparison.
                const uint8_t slotBeforeBounce = liveSlot;
                if (slotBeforeBounce == 0xFF)
                    return RefusedWith(ParticipateRefusal::BounceUnsafeForMultipleEntrants);

                // Spent at the call site and not after it: a leg that faults or does not return
                // has still spent the budget.
                ++drive.gate->reSeatIssues;

                uint8_t seenRole = 0xFF;
                uint8_t seenSlot = 0xFF;
                Mark(0, "a3 reSeatAway", h, index);
                if (!h.assignTeam(h.ctx, index, away))
                    return RefusedWith(ParticipateRefusal::ReSeatLegNotApplied);
                Mark(0, "a3 teamOf#away", h, index);
                if (!h.teamOf(h.ctx, index, &seenRole, &seenSlot) || uint16_t(seenRole) != away)
                    return RefusedWith(ParticipateRefusal::ReSeatLegNotApplied);

                // The entrant holds a role the recording never gave it for the rest of THIS call
                // and no longer -- both legs run inside one tick, with no engine update between
                // them.  If the leg back does not take, the arm says so by name rather than
                // papering over it, and the next window's live role no longer equals the recorded
                // one, so the ordinary A3 assign re-seats it: the same drive L3 #30 measured.
                Mark(0, "a3 reSeatBack", h, index);
                if (!h.assignTeam(h.ctx, index, recorded))
                    return RefusedWith(ParticipateRefusal::ReSeatNotRestored);
                Mark(0, "a3 teamOf#back", h, index);
                if (!h.teamOf(h.ctx, index, &seenRole, &seenSlot) ||
                    uint16_t(seenRole) != recorded)
                    return RefusedWith(ParticipateRefusal::ReSeatNotRestored);

#ifndef W2P1_MUTATE_TEAMSLOT_NOT_COMPARED
                // THE OTHER HALF OF THE RESTORE, and the arm read it on both legs and threw it
                // away until now.  The role coming back is not the entrant coming back: the
                // allocator hands out the slot, and a slot that moved is a persistent difference
                // from a player assigned once -- the recording's own `teamSlot` substituted by a
                // function of a transient count.  Never Done, and said by its own name.
                if (seenSlot != slotBeforeBounce)
                    return RefusedWith(ParticipateRefusal::TeamSlotChangedByBounce);
#endif

                drive.gate->reSeatPending = true;
                return RefusedWith(ParticipateRefusal::ReSeatAwaitingReRead);
            }
        }
#endif

        // The refresh below has already been told it is a no-op for this entrant, so it is not
        // issued again -- a call the engine has said it will decline is not a retry.
        if (drive.gate->refreshDeclined)
            return RefusedWith(ParticipateRefusal::ActorNotEnabledAfterRefresh);

        // ARM 1 -- THE REFRESH ITSELF, and it runs FIRST of the two because it is the only one
        // that touches nothing at all when it declines: it allocates no actor, moves no Team Slot
        // and, on the Declined path, does not even reach its own function body's second half.
        // Its return is what selects the arm above.
        if (!canRefresh) return RefusedWith(ParticipateRefusal::ActorNotEnabledAfterRefresh);

        // THE CALL BUDGET, and it is terminal for the epoch.  Once kRefreshIssueMax refreshes have
        // reached the engine and the actor is still not on, the standing fact is exactly that --
        // so the arm keeps saying it and stops rebuilding the actor to say it again.
#ifndef W2P1_MUTATE_REFRESH_UNBOUNDED
        if (drive.gate->refreshIssues >= kRefreshIssueMax)
            return RefusedWith(ParticipateRefusal::ActorNotEnabledAfterRefresh);
#endif

        // THE READINESS GUARD, and it is the last thing before the call.  A call that can fail to
        // return may not be issued speculatively from a tick.
        Mark(0, "a3 assetsReady", h, index);
        if (!h.actorAssetsReady(h.ctx, index))
            return RefusedWith(ParticipateRefusal::ActorAssetsNotReady);

#ifndef W2P1_MUTATE_REFRESH_LIVE_ACTOR
        // NEVER REFRESH A LIVE ACTOR EITHER, and this read is taken AT the call rather than
        // inherited from `actorOn` at the top of the window (deferred check #8).  The refresh is
        // not harmless: sub_14019F1C0:8-9 builds its own third argument, so
        // NETGAME.SET_PLAYER_ACTOR can take its `Player Actor Freeing` arm, and an arm that can
        // destroy the body may not be issued against one the engine is already rendering.
        Mark(0, "a3 enabled#beforeRefresh", h, index);
        if (h.enabled(h.ctx, index))
            return RefusedWith(ParticipateRefusal::ActorEnabledSoNotFreed);
#endif

        // THE FIRST HALF OF THE TWO-READ, and it is taken BEFORE the call because the call's own
        // answer cannot carry it: the entrant's SActorGUID, which sub_1401B0D80:319 is the only
        // writer of on this path.  A guid that moved is the engine saying it allocated, and
        // everything the refresh exists to drive lives below that allocate.
        uint64_t guidBeforeLo = 0;
        uint64_t guidBeforeHi = 0;
        bool guidBefore = false;
        if (h.actorGuid != nullptr) {
            Mark(0, "a3 actorGuid#preRefresh", h, index);
            guidBefore = h.actorGuid(h.ctx, index, &guidBeforeLo, &guidBeforeHi);
        }

        // Spent at the call site and not after it: the budget is for ISSUING, so a call that
        // faults or does not return has still spent one.
        ++drive.gate->refreshIssues;
        Mark(0, "a3 refreshPlayerActor", h, index);
        const RefreshAnswer answer = h.refreshPlayerActor(h.ctx, index);
        drive.gate->lastRefresh = answer;

        // THE RETURN, KEPT -- AND CORROBORATED BEFORE IT DECIDES ANYTHING.  It is not the verdict
        // and it never becomes one -- the enable re-read below is still what books an issue -- but
        // it is the only reading that separates "the engine ran its dispatch and nothing came on"
        // from "the engine returned at :91 having run nothing at all", and those two need
        // different next moves.  L3 #31 spent three calls and 276 windows unable to tell them
        // apart because this value was discarded at the call site.  What the wrapper cannot do is
        // say the SECOND of those by itself: sub_14019F1C0 clobbers the return register on its
        // success path (14019F200-223), so a 0 is decided by the guid two-read and never by the
        // return.  AUD-W2-P1-34 MAJOR-1.
#ifndef W2P1_MUTATE_REFRESH_RETURN_DISCARDED
        Mark(0, RefreshAnswerMark(answer), h, index);
        if (answer == RefreshAnswer::Unavailable)
            return RefusedWith(ParticipateRefusal::ParticipationNotReRun);
        if (answer == RefreshAnswer::Declined) {
#ifdef W2P1_MUTATE_DECLINE_FROM_RETURN
            // The defect: the wrapper's 0 latches the one-way gate by itself, so a session whose
            // flags bit reads 0 retires the refresh for the epoch and hands selection to the bounce.
            drive.gate->refreshDeclined = true;
            return RefusedWith(ParticipateRefusal::RefreshDeclinedKeyUnchanged);
#else
            uint64_t guidAfterLo = 0;
            uint64_t guidAfterHi = 0;
            bool guidAfter = false;
            if (h.actorGuid != nullptr) {
                Mark(0, "a3 actorGuid#postRefresh", h, index);
                guidAfter = h.actorGuid(h.ctx, index, &guidAfterLo, &guidAfterHi);
            }
            // COULD NOT ASK gets its own word.  This gate is one-way -- it retires the cheap arm
            // for the whole epoch and admits the bounce -- so it may not be latched off a
            // reading we did not get.
            if (!guidBefore || !guidAfter)
                return RefusedWith(ParticipateRefusal::RefreshDeclineNotCorroborated);
            if (guidAfterLo == guidBeforeLo && guidAfterHi == guidBeforeHi) {
                drive.gate->refreshDeclined = true;
                return RefusedWith(ParticipateRefusal::RefreshDeclinedKeyUnchanged);
            }
            // The guid MOVED: :319 ran, so the 0 was the wrapper's own flags-byte rewrite and the
            // engine rebuilt and dispatched.  Nothing is latched, and the
            // enable re-read below decides -- exactly as it does for a non-zero return.
            Mark(0, "a3 refresh#rebuiltUnderZeroReturn", h, index);
#endif
        }
#endif

        // THE POSTCONDITION, and it is the engine's own enable predicate re-read -- the same call
        // its entrant dump renders as `(enabled)`/`(disabled)`.  A non-zero return says the
        // function reached :407, not that the dispatch enabled anything, so it is this read and
        // never that return that decides.
#ifndef W2P1_MUTATE_ISSUED_WITHOUT_DRIVE
        Mark(0, "a3 enabled#2", h, index);
        if (!h.enabled(h.ctx, index))
            return RefusedWith(ParticipateRefusal::ActorNotEnabledAfterRefresh);
#endif

        drive.gate->issuedEpoch = drive.gate->epoch;
        drive.gate->issuedRole  = recorded;
        drive.gate->via         = ParticipateDrive::RefreshPlayerActor;
        ++drive.gate->issues;
        return ParticipateOutcome{Participation::Issued, ParticipateRefusal::None};
#endif
    }
#endif

#ifdef W2P1_MUTATE_PARTICIPATE_ORDER
    // The order this design forbids: the team is assigned before the record the engine's
    // participating arm reads exists, so that arm runs against an absent `spawned`.
    Mark(0, "a2 writeSpawned", h, index);
    (void)h.writeSpawned(h.ctx, index);
#endif

    // A3 -- the engine's own IDENT.SERVER_ASSIGN_TEAM, which rewrites the packed role word and
    // re-runs the participating/spectating dispatch.
    Mark(0, "a3 assignTeam", h, index);
    if (!h.assignTeam(h.ctx, index, recorded))
        return RefusedWith(ParticipateRefusal::AssignTeamNotApplied);

    Mark(0, "a3 teamOf#2", h, index);
    if (!h.teamOf(h.ctx, index, &liveRole, &liveSlot) || uint16_t(liveRole) != recorded)
        return RefusedWith(ParticipateRefusal::AssignTeamNotApplied);

#ifndef W2P1_MUTATE_PARTICIPATE_EVERY_FRAME
    drive.gate->issuedEpoch = drive.gate->epoch;
#endif
    drive.gate->issuedRole = recorded;
    drive.gate->via        = ParticipateDrive::AssignTeam;
    ++drive.gate->issues;
    return ParticipateOutcome{Participation::Issued, ParticipateRefusal::None};
}

Hop AdvanceActorHops(const ActorHops& h, uint16_t index, HopDrive* drive) {
    // An index nobody issued is not at the end of the chain, it is before the start of it.
    if (index == kSlotUnmapped) return Hop::RemotePlayerCs;

    // HOP 1 -- REGISTERED, and it is a READ of the ROOT's own guid table.  S14.4: every site in
    // the image supplies [ng+8], and the container this hop used to search is a field of a
    // different object -- a wrong read whose answer never meant what its comment said.  Nothing is
    // driven here: NETGAME.SET_PLAYER_ACTOR creates and CS-registers the actor on the remote arm
    // already, which S14.0 established against the reading DIAG-HOP-AV had.
    if (h.inRemotePlayerCs == nullptr) return Hop::RemotePlayerCs;
    Mark(1, "inRemotePlayerCs", h, index);
    if (!h.inRemotePlayerCs(h.ctx, index)) return Hop::RemotePlayerCs;

    // THE ONE AUTHORED STEP.  Everything after it is a read of a record the ENGINE writes on its
    // own participating arm: it enables the actor, applies the chassis and drives InitCI with
    // arguments it derives.  We author the entrant's participation and nothing else.
    if (drive != nullptr) drive->outcome = Participate(h, index, *drive);

    // HOP 2 -- ENABLED.  The engine's own predicate, on the ROOT and with a POINTER to the guid:
    // the same function and the same argument shape its `Actor : 0x... (enabled)` dump renders, so
    // this hop and that line cannot disagree.
    if (h.enabled == nullptr) return Hop::Enabled;
    Mark(2, "enabled", h, index);
    if (!h.enabled(h.ctx, index)) return Hop::Enabled;

    // HOP 3 -- SPAWNED, which is what `placed` reads now that it HAS a readable postcondition
    // (S14.2): NETGAME.USER_SPAWNED_GET, the engine's own `user[N]|spawned` boolean, written
    // immediately after its own place and read by its own participating arm.  The place itself
    // stays unbound -- our poses are authored, so the engine's spawn transform is worth nothing
    // to us and its shape is recorded in the spike for the day one is wanted.
    if (h.placed == nullptr) return Hop::Placed;
    Mark(3, "placed", h, index);
    if (!h.placed(h.ctx, index)) return Hop::Placed;

    // HOP 4 -- THE BODY, and this one is only waited for.  The component instance is built by the
    // component system's own per-frame init off the registration hop 1 asserted.
    //
    // ONE read of the surface per pass, and the requirement word leaves with
    // the answer.  The caller used to ask PhysicsCiAwaiting after this returned, which walked the
    // pool twice more: two extra markers out of the sixteen a tick has, and -- worse -- a window
    // in which the engine could finish InitCI between the two reads, so the pass printed
    // `PhysicsCiAwaiting(none)` beside a hop that had answered false.  `physicsCi=none` is the
    // substring an L3 runner reads as proof hop 4 reached Done, and it could appear on a pass
    // where it did not.  The state is decided here, once, and every reader prints THIS decision.
    if (h.physicsCi == nullptr) {
        if (drive != nullptr) drive->physicsCiWhy = PhysicsCiReq::Unknown;
        return Hop::PhysicsCi;
    }
    Mark(4, "physicsCi", h, index);
    if (!h.physicsCi(h.ctx, index)) {
        if (drive != nullptr) {
            if (h.physicsCiAwaiting == nullptr) {
                drive->physicsCiWhy = PhysicsCiReq::Unknown;
            } else {
                Mark(4, "physicsCiAwaiting", h, index);
                drive->physicsCiWhy = h.physicsCiAwaiting(h.ctx, index);
            }
        }
        return Hop::PhysicsCi;
    }
    if (drive != nullptr) drive->physicsCiWhy = PhysicsCiReq::None;
    return Hop::Done;
}

}  // namespace PawnProvision

namespace PawnResolve {

namespace {

struct ResolveRow { const char* name; const char* text; };

constexpr ResolveRow kRows[] = {
    {"None", "None: no refusal"},
    {"NotAttempted", "NotAttempted: no resolve was attempted for that slot"},
    {"NoEntityService",
     "NoEntityService: no entity service is bound, so neither pool could be asked"},
    {"NoPlayerCi",
     "NoPlayerCi: neither the remote pool nor the local-pawn surface holds a CI for that slot"},
    {"NoPhysCi", "NoPhysCi: a CI exists and its physics handle is 0xFFFF"},
    {"NoModel", "NoModel: no NcaModel entry resolved -- the pawn is physical but invisible"},
    {"PawnClassMismatch",
     "PawnClassMismatch: the recorded row's class is not the class that answered; the wrong CI was "
     "NOT driven"},
    {"SlotCollision", "SlotCollision: two component instances claim that slot"},
};
static_assert(sizeof(kRows) / sizeof(kRows[0]) == kRefusalCount,
              "the table is indexed by the enum");

}  // namespace

const char* RefusalName(Refusal r) {
    const size_t i = size_t(r);
    return i < kRefusalCount ? kRows[i].name : "Unknown";
}

const char* RefusalText(Refusal r) {
    const size_t i = size_t(r);
    return i < kRefusalCount ? kRows[i].text : "Unknown: unknown refusal";
}

}  // namespace PawnResolve

// ---------------------------------------------------------------------------------------
// Authoring.

// THE SESSION'S OWN SALT, drawn once per process.  Two replays of the same capture must not
// author the same ids: the engine's lobby outlives a load, and an id left behind by the previous
// one is a duplicate to evict.
static uint64_t SessionSalt() {
    static const uint64_t salt = [] {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        return (uint64_t(qpc.QuadPart) * 0x9E3779B97F4A7C15ull) ^
               (uint64_t(GetCurrentProcessId()) << 32) ^ GetTickCount64();
    }();
    return salt;
}

// A read of the engine's entrant table, which the crash logger's own SafeReadU64 cannot serve:
// that one is file-local to the handler's namespace.  The table is allocated for the session's
// real entrant capacity and the walk runs to the slot maximum, so the tail is genuinely allowed
// to be unmapped and a fault there must not cost the admission.
static bool ReadEntrantU64(const void* p, uint64_t* out) {
    __try {
        *out = *static_cast<const uint64_t*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadXplatIdsInUse(void* netGame, XplatIdsInUse& out) {
    out = XplatIdsInUse{};
    if (netGame == nullptr) return false;
    // Entrant 0 IS the viewer and it is written with no stride, so the walk starts at the base
    // and the viewer's id is simply the first element of it (FACTS-VIEWER-ID64).
    const uint8_t* const base = static_cast<const uint8_t*>(netGame);
    for (uint32_t i = 0; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i) {
        uint64_t id = 0;
        const void* const at =
            base + size_t(i) * facts::NETGAME_PAWN_TABLE_STRIDE + facts::NETGAME_ENTRANT_XPLAT_ID64_OFF;
        // A record we cannot read is not a record holding no id: the walk stops rather than
        // reporting a short set as a complete one.
        if (!ReadEntrantU64(at, &id)) return false;
        if (id != 0 && id != ~uint64_t(0)) out.ids[out.count++] = id;
    }
    out.known = true;
    return true;
}

bool SyntheticXplatformId(uint64_t recordedId, uint16_t entrantIndex,
                          const XplatIdsInUse& inUse, uint64_t& out) {
    for (unsigned attempt = 0; attempt < kXplatSaltAttempts; ++attempt) {
        // splitmix64 over the recorded id and the salt.  The ENTRANT INDEX is not mixed in, it is
        // carried: it occupies the low 16 bits, so two entrants of one session differ in the id
        // whatever the mix does, which is the property a probabilistic hash cannot promise.  The
        // attempt counter rides the same mix, so a re-salt moves the whole 48-bit window and not
        // one bit of it.
        uint64_t x = recordedId ^ SessionSalt() ^
                     ((uint64_t(entrantIndex) + (uint64_t(attempt) << 16)) * 0xD1B54A32D192ED03ull);
        x += 0x9E3779B97F4A7C15ull * (uint64_t(attempt) + 1);
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        x ^= x >> 31;
        const uint64_t id = (x << kXplatIndexBits) | uint64_t(entrantIndex);
        // The engine's own two reserved words, and then every id the live session already holds.
        if (id == 0 || id == ~uint64_t(0)) continue;
#ifndef W2P1_MUTATE_XPLAT_VIEWER_NOT_AVOIDED
        if (inUse.Has(id)) continue;
#else
        (void)inUse;
#endif
        out = id;
        return true;
    }
    return false;
}

JoinDesc JoinDescFromRecord(const PlayerRecord& rec, uint16_t entrantIndex,
                            const XplatIdsInUse* inUse) {
    JoinDesc d{};
    // NOT THE RECORDED ID (DIAG-ENTITY-COLLISION 4).  sub_1406051C0/sub_14060A920 evict a
    // duplicate entrant on {platform, id64}, and the only reason a capture replayed on its own
    // account has not evicted the viewer is that the platform halves differed.  ~0 was worse
    // still: it IS the viewer's own id64 in a `-offline' host.  A synthetic id owes the engine
    // nothing except being nobody else's.
    d.recordedXplatId = rec.xplatformId;
#ifdef W2P1_MUTATE_XPLAT_ID_TILDE0
    d.xplatformId = rec.xplatformId ? rec.xplatformId : ~uint64_t(0);
#else
    static const XplatIdsInUse kNothingKnownInUse{};
    // A synthesis that ran out of salts leaves the id at 0, which is the engine's own `no id'
    // and the one value the provisioner refuses XplatIdCollision on -- the descriptor never
    // carries a colliding id, whichever caller authored it.
    d.xplatformId = 0;
    SyntheticXplatformId(rec.xplatformId, entrantIndex,
                         inUse != nullptr ? *inUse : kNothingKnownInUse, d.xplatformId);
#endif

    // ONE SOURCE FOR THE TEAM, both record shapes.  `teamId16` is what the READER hands back, and
    // for a B.0 row the reader has already widened the 8-bit field -- turning its only absence
    // value, 0xFF, into kPlayerNoTeam16 (manifest_io.cpp). Reading the 8-bit field again here
    // threw that upgrade away and refused TeamOutOfRange for a player whose team was simply never
    // recorded, which is TeamNotRecorded: a different fact, and the only one that is true.
#ifdef W2P1_MUTATE_B0_TEAM_TRUNCATED
    const uint16_t team = rec.extended ? rec.teamId16 : uint16_t(rec.teamId);
#else
    const uint16_t team = rec.teamId16;
#endif
    d.team = team;
    d.role = (team == kTeamSpectator || team == kTeamModerator) ? kJoinRoleSpectator
                                                                : kJoinRolePlayer;

    // The recording carries a player's POSES, not the transform it entered the lobby at, and the
    // playback driver writes those poses itself.  Authoring a spawn transform out of a pose would
    // be a value this file does not hold, so the descriptor asks for the level's own spawn point.
    d.useSpawnPoint = true;

    // WHAT THE ENTRANT WEARS, carried by view.  These are the same four values the dress ladder's
    // steps 2 and 4 read out of this record; they travel with the descriptor so the admission can
    // install them BEFORE it asks the engine to build an actor (L3 #18 F1).  Copying the ids
    // rather than pointing at them keeps the 168-byte loadout independent of the record's storage;
    // the two JSON blobs are large and are views, which is why the header states the lifetime.
    d.entrantJson       = rec.entrantJson;
    d.unlocksJson       = rec.unlocksJson;
    d.loadoutNumber     = rec.loadoutNumber;
    d.loadoutNumberBody = rec.loadoutNumberBody;
    d.alignment         = rec.alignment16;
    for (size_t i = 0; i < 20; ++i) d.loadoutSlots[i] = rec.loadoutSlots[i];

    // PlayerRecord::displayName is a fixed array the format pads but does not terminate.
    static_assert(sizeof d.displayName > sizeof rec.displayName,
                  "the descriptor's name buffer must hold the recorded bytes AND a terminator");
    memcpy(d.displayName, rec.displayName, sizeof rec.displayName);
    d.displayName[sizeof rec.displayName] = '\0';
    return d;
}

bool WriteJoinRec(const JoinDesc& d, uint8_t out[kJoinRecBytes]) {
    if (!out) return false;
    memset(out, 0, kJoinRecBytes);

    // The id sits at offset 0.  IDENT.ADMIT_JOINREC_ID_OFF is filed and WITHHELD -- a zero offset
    // has no encoded displacement field for an anchor to sit on, the same shape as
    // NETGAME.STATE_OFF -- so the row cannot reach a constant here and `mov rbx, [rsi]` at
    // 0x140152356 is the whole derivation.
    memcpy(out, &d.xplatformId, sizeof d.xplatformId);
    memcpy(out + facts::IDENT_ADMIT_JOINREC_TEAM_OFF, &d.team, sizeof d.team);
    memcpy(out + facts::IDENT_ADMIT_JOINREC_ROLE_OFF, &d.role, sizeof d.role);

    // Two OWORDs, position then orientation.  The engine compares the floats at +0x10 + 0x10,
    // +0x14 and +0x18 of the SECOND oword against the sentinel; matching all three is what selects
    // the level spawn point, so the sentinel is written across the whole orientation oword rather
    // than into three of its four lanes -- a partial match is not a match.
    float* xf = reinterpret_cast<float*>(out + facts::IDENT_ADMIT_JOINREC_XFORM_OFF);
    if (d.useSpawnPoint) {
        float sentinel;
        const uint32_t bits = facts::IDENT_ADMIT_XFORM_SENTINEL;
        memcpy(&sentinel, &bits, sizeof sentinel);
        for (int i = 0; i < 8; ++i) xf[i] = sentinel;
    } else {
        for (int i = 0; i < 8; ++i) xf[i] = d.xform[i];
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// The latch.

namespace {

uint32_t StateWord(const void* ng) {
    uint32_t v = 0;
    // NETGAME.STATE_OFF is 0 and the registry WITHHOLDS it -- a zero offset has no encoded
    // displacement for an anchor to sit on, so the row is filed as present-but-incomplete and
    // cannot reach a constant.  The derivation is cmp dword ptr [rdi], 9 at 0x14015269A, where rdi
    // is the netgame pointer itself and there is no displacement byte to read.
    memcpy(&v, ng, sizeof v);
    return v;
}

}  // namespace

namespace PawnProvision {

Refusal SessionBlocker(void* netGame) {
    if (!netGame) return Refusal::NoNetGame;
#ifndef W2P1_MUTATE_NO_STATE_GATE
    if (StateWord(netGame) != facts::NETGAME_STATE_INMATCH) return Refusal::SessionNotInMatch;
#endif
    return Refusal::None;
}

// ReplyWrongPhase is a DEFERRAL and not a failure: the route is bound, the request has been
// parked, and the listener on the netgame's broadcaster publishes it from inside the engine's own
// pump.  Latching it would refuse the slot for the life of the load on the one route that can
// actually build the pawn.  PawnProvisioner::Refuse leaves every value below queued; the dress
// pass keeps its roster pending for exactly the same set.
bool IsDeferral(Refusal why) {
    return why == Refusal::SessionNotInMatch || why == Refusal::BudgetExceeded ||
           why == Refusal::RetryNotDue || why == Refusal::ReplyWrongPhase ||
           // A slot the viewer holds is refused, never failed: the engine re-seats the local user
           // and the slot can come free again, and a latch here would give up on a recorded
           // player for the load.
           why == Refusal::ViewerSlotExcluded ||
           // The dress's one-frame wait.  Latching it would give up on the loadout for the whole
           // load on the single frame the rule exists to skip.
           why == Refusal::LoadoutSameFrameAsParticipation ||
           // The enable's hold, for the same reason: the live count can read back on any later
           // window and latching would give the entrant up for the load on the first one that
           // read 0.
           why == Refusal::LoadoutNotPublishedBeforeEnable ||
           // The four gates the engine's apply closes silently, for the same reason again: three
           // of them are the actor side still coming up.
           why == Refusal::LoadoutInstanceEmpty || why == Refusal::LoadoutActorUnresolved ||
           why == Refusal::LoadoutCsPointerNull || why == Refusal::LoadoutCompKeyInvalid ||
           // The re-seat settle deferral (DIAG-PARTICIPATION-FLAKINESS): the actor is enabled and
           // the booking is one window from landing, so the slot must stay Awaiting for the P1-47
           // latch to reach Issued or a named exhaustion.  Unconditional -- it is not a
           // hop-awaiting refusal and does not belong to the AWAITING_LATCHES mutant's group.
           why == Refusal::ParticipationSettling ||
           // The seat was refused for want of a promoted no-AI route, not failed.
           // The route can promote on any later pump (its binds read .data live, and
           // pawn.reply_route can be re-enabled), so latching here would give a recorded player up
           // for the load on the one route that can build the pawn without attaching AI.
           why == Refusal::DirectInsertSuppressed
#ifndef W2P1_MUTATE_AWAITING_LATCHES
           || why == Refusal::NotInRemotePlayerCs || why == Refusal::ActorNotEnabled ||
           why == Refusal::ActorNotPlaced || why == Refusal::NoPhysicsCi ||
           // Hop 4's two faults join the hop group rather than latching: the component system is
           // resolved once the space is up, and the entrant index is re-resolved on every pass, so
           // both can clear on their own.  They stay counted by NAME, which is what separates them
           // from a wait.
           why == Refusal::PhysicsCiUnreadable || why == Refusal::PhysicsCiIndexOutOfRange ||
           // The model drive's async HOLD (DIAG-MODEL-BUILD fix (b)): the mesh arrives a few
           // windows after the swap is enqueued, so the slot stays Awaiting and re-reads
           // ComponentOnActor each window.  Past the grace the drive stops returning the hold and
           // the slot provisions bodyless -- so this never latches a slot off, it only keeps it
           // re-polling meanwhile.
           why == Refusal::ModelNotInstantiated
#endif
        ;
}

}  // namespace PawnProvision

namespace { void SwapperResetSweepThunk(void* ctx, void* ng); }

void PawnProvisioner::Arm(const ProvisionEnv& env) {
    env_ = env;
    // Install the per-tick reset sweep on the stage-0 detour.  `this` outlives the
    // detour (the provisioner has static storage); the sweep re-checks env_ each call and no-ops when
    // disarmed, and Disarm clears the registration.
    replay::identity_access::RegisterSwapperResetSweep(&SwapperResetSweepThunk, this);
#ifndef W2P1_MUTATE_NO_HOPS_FROM_IDENTITY
    // WHERE THE HOP SURFACE COMES FROM.  The env is built in W2-B's playback_module.cpp, which
    // already assigns `identity` from the shipped identity_access object; asking THAT for its
    // surface completes the wire with no edit to a file this packet does not own.  An explicit
    // `hops` in the env still wins -- the tests hand one in directly -- so this fills a gap
    // rather than overriding a decision.
    if (env_.hops == nullptr && env_.identity != nullptr) env_.hops = env_.identity->ActorHops();
#endif
    // THE CRASH LOGGER GOES ON HERE, and here is the only place it can.  DllMain holds the
    // loader lock and AddVectoredExceptionHandler is not a call to make under it; Arm runs on the
    // post-attach tick, once per load, before the first hop is ever driven.  It is unconditional:
    // a crash logger that has to be switched on is switched off on the run that needed it.
    if (replay::Resolved()) PawnProvision::InstallCrashLogger(replay::LogPath());
    // Modules load and unload across a session, so the snapshot the handler scans is
    // re-taken on every arm -- here, on the tick, and never from inside the handler.
    PawnProvision::RefreshCrashModules();
    censused_ = false;
    ++epoch_;   // a new epoch: the place latch reopens for every slot
    foreignCount_ = 0;
    pumps_ = 0;
    next_attempt_ = 0;
    next_status_ = 0;
    next_census_ = 0;
    window_pump_ = UINT64_MAX;
    window_open_ = false;
    attempts_ = 0;
    suppressed_ = 0;
    blocker_ = PawnProvision::Refusal::NotAttempted;
    if (env_.perFrameMax == 0) env_.perFrameMax = 1;  // a zero slice is a stall, not a budget
    for (auto& s : slots_) s = Slot{};
    ledger_ = PawnLedger{};
    last_ = PawnProvision::Refusal::NotAttempted;
}

void PawnProvisioner::Disarm() {
    replay::identity_access::ClearSwapperResetSweep();
    env_ = ProvisionEnv{};
    censused_ = false;
    foreignCount_ = 0;
    pumps_ = 0;
    next_attempt_ = 0;
    next_status_ = 0;
    next_census_ = 0;
    window_pump_ = UINT64_MAX;
    window_open_ = false;
    attempts_ = 0;
    suppressed_ = 0;
    blocker_ = PawnProvision::Refusal::NotAttempted;
    for (auto& s : slots_) s = Slot{};
    ledger_ = PawnLedger{};
    last_ = PawnProvision::Refusal::NotAttempted;
}

bool PawnProvisioner::Refuse(UserSlot slot, PawnProvision::Refusal why) {
    last_ = why;
    ++ledger_.refusals[size_t(why)];
    // The set itself lives beside SessionBlocker, because the dress pass's retry now keys on the
    // same answer and two copies would drift.
    const bool deferral = PawnProvision::IsDeferral(why);
    // WHAT `deferred` COUNTS IS NOT THE SET THAT LATCHES (L3 #24).  The ledger's own word for the
    // field is "attempts that could not be made yet (state gate, budget)", and only those two
    // kinds could not be made.  RetryNotDue is the cadence WORKING and fires once a frame;
    // ReplyWrongPhase and the four hop refusals are attempts that WERE made and are outstanding.
    // Counting them turned `deferred` into a frame counter -- L3 #24 read `deferred 309` over a
    // seven-second load -- so a reader could not tell a session still behind the state gate from
    // one making progress past it.  Each of them is still counted by NAME in `refusals`.
    const bool notYetMade = why == PawnProvision::Refusal::SessionNotInMatch ||
                            why == PawnProvision::Refusal::NoNetGame ||
                            why == PawnProvision::Refusal::BudgetExceeded;
    if (notYetMade) {
        ++ledger_.deferred;
    } else if (!deferral && why != PawnProvision::Refusal::None) {
        ++ledger_.failed;
    }
    if (slot.v < facts::ENTITY_USER_SLOT_TABLE_MAX) {
        slots_[slot.v].why = why;
        // A deferral leaves the slot queued; a failure latches, so a permanently refused player
        // does not re-ask the engine on every frame of a four-minute session.
        if (!deferral && why != PawnProvision::Refusal::None)
            slots_[slot.v].state = SlotState::Failed;
    }
    return why == PawnProvision::Refusal::None;
}

// THE ONLY PLACE THE IDS ALREADY IN THE SESSION CAN BE READ.  The descriptor was authored off
// the recording alone; the set it has to stay off is the viewer's own id64 at
// NETGAME.ENTRANT_XPLAT_ID64_OFF (entrant 0, no stride) and every admitted entrant's
// (FACTS-VIEWER-ID64).  The entrant index the author carried in the low 16 bits is kept, so the
// per-session uniqueness the mix cannot promise survives the re-authoring.
PawnProvision::Refusal PawnProvisioner::AuthorXplatId(Slot& s) {
    if (s.xplatAuthored) {          // latched: see the field's comment
        s.desc.xplatformId = s.xplatId;
        return PawnProvision::Refusal::None;
    }
    XplatIdsInUse inUse{};
    if (env_.netGame != nullptr) ReadXplatIdsInUse(env_.netGame(), inUse);
    s.xplatInUseKnown = inUse.known;
    uint64_t authored = 0;
    // A set we could NOT read is not an empty one, but it is also not a collision: the synthesis
    // runs against what we have and the census line says which of the two it was.  Only a budget
    // spent against ids we actually read is XplatIdCollision.
    if (!SyntheticXplatformId(s.desc.recordedXplatId,
                              uint16_t(s.desc.xplatformId & kXplatIndexMask), inUse, authored))
        return PawnProvision::Refusal::XplatIdCollision;
    s.xplatId = authored;
    s.xplatAuthored = true;
    s.desc.xplatformId = authored;
    return PawnProvision::Refusal::None;
}

bool PawnProvisioner::Request(UserSlot slot, const JoinDesc& desc) {
    if (slot.v >= facts::ENTITY_USER_SLOT_TABLE_MAX)
        return Refuse(slot, PawnProvision::Refusal::SlotOutOfRange);
    Slot& s = slots_[slot.v];
    if (s.state != SlotState::Idle) return true;   // requesting twice is not a second request
    s.desc = desc;
    const PawnProvision::Refusal xplatWhy = AuthorXplatId(s);
    if (xplatWhy != PawnProvision::Refusal::None) return Refuse(slot, xplatWhy);
    s.state = SlotState::Pending;
    // ONCE PER ENTRANT, and MASKED -- but never through the low 16 bits, which the synthesis
    // SETS to the entrant index: that window printed 0x000N for a correct id and 0x000N for a
    // regression to the recorded one alike, and proved nothing (AUD-W2-P1-34 P1-40 F1).  What is
    // printed instead is the high half of the mix (bits 16..31, which no regression can hold) and
    // the authored and RECORDED high 16 bits side by side: `d.xplatformId = rec.xplatformId'
    // makes those two equal on every entrant, and the synthesis makes them differ.
    if (env_.log != nullptr)
#ifndef W2P1_MUTATE_XPLAT_LOG_MASKS_INDEX
        env_.log->Write(LogLevel::Info, "identity",
                        "entrant %u xplat salt=0x%04X hi=0x%04X rec=0x%04X inuse=%s",
                        unsigned(slot.v),
                        unsigned((s.desc.xplatformId >> kXplatSaltShift) & kXplatIndexMask),
                        unsigned((s.desc.xplatformId >> 48) & kXplatIndexMask),
                        unsigned((s.desc.recordedXplatId >> 48) & kXplatIndexMask),
                        s.xplatInUseKnown ? "read" : "unknown");
#else
        env_.log->Write(LogLevel::Info, "identity", "entrant %u xplat salt=0x%04X",
                        unsigned(slot.v), unsigned(s.desc.xplatformId & kXplatIndexMask));
#endif
    s.why = PawnProvision::Refusal::NotAttempted;
    ++ledger_.requested;
    return true;
}

bool PawnProvisioner::Provision(UserSlot slot, const JoinDesc& desc) {
    if (!Armed()) return Refuse(slot, PawnProvision::Refusal::NotAttempted);
    if (slot.v >= facts::ENTITY_USER_SLOT_TABLE_MAX)
        return Refuse(slot, PawnProvision::Refusal::SlotOutOfRange);

    Slot& s = slots_[slot.v];
    if (s.state == SlotState::Provisioned) {
        last_ = PawnProvision::Refusal::None;   // idempotent: no engine is touched, nothing counted
        return true;
    }
    // ★ AWAITING NEVER LATCHES AND NEVER STOPS (L3 #23 F3/F4).  The admission landed; what is
    // outstanding is a step the ENGINE performs, and the record that answers it becomes readable
    // at or after the state-9 edge -- which is the exact millisecond the old pass stopped looking,
    // because a slot that was not Pending was skipped by Pump and never asked again.  A slot
    // parked here is re-read once per cadence window for the LIFE OF THE SESSION: there is no
    // attempt budget to run out, and the only thing bounding it is the same one-second cadence
    // every other engine read here is bounded by.
    if (s.state == SlotState::Awaiting) {
        const PawnProvision::Refusal awaitBlocked = PawnProvision::SessionBlocker(env_.netGame());
        if (awaitBlocked != PawnProvision::Refusal::None) return Refuse(slot, awaitBlocked);
        if (!AttemptDue(awaitBlocked)) return Refuse(slot, PawnProvision::Refusal::RetryNotDue);
        return AdvanceSlot(slot, s);
    }

    if (s.state == SlotState::Idle) Request(slot, desc);
    s.desc = desc;
    // The descriptor the caller hands in carries the AUTHOR's id; the one the engine was told
    // about is the slot's latched one, and re-telling it a different id admits a second person.
    const PawnProvision::Refusal xplatWhy = AuthorXplatId(s);
    if (xplatWhy != PawnProvision::Refusal::None) return Refuse(slot, xplatWhy);

    // INV-PP4.  The insert places the actor only at NETGAME.STATE_INMATCH; admitted before it, an
    // entrant gets an actor GUID and NO PLACED PAWN, silently.  That is a deferral, and a deferral
    // is neither a failure nor never-having-asked.  Both blockers are spelled once, in
    // SessionBlocker, so the gate a caller waits on and the gate an attempt fails on cannot drift.
    const PawnProvision::Refusal blocked = PawnProvision::SessionBlocker(env_.netGame());
    if (blocked != PawnProvision::Refusal::None) return Refuse(slot, blocked);

    // THE RETRY CADENCE (L3 #15 F1).  Everything past this point walks the lobby chain once per
    // candidate slot and calls the engine's insert; L3 #15 measured that at ~1 ms a tick, every
    // tick, for a whole load, and its own capture died of the cost.  Nothing below reads anything
    // the blocker above does not already key, so while that blocker is unchanged a second pass
    // answers exactly what the first one did.  An attempt is spent when the blocker MOVED, when a
    // provision just landed, or once every kRetryTicks pumps -- never per tick.  The deferral
    // above still reports every tick: it is one guarded read.
    if (!AttemptDue(blocked)) return Refuse(slot, PawnProvision::Refusal::RetryNotDue);

    // The (A) case, closed by MAPPING and not by writing through.  W2-P0 measured the viewer's own
    // user holding a slot with an EMPTY profile, so "not dressed" does not mean "not there":
    // occupancy is what the engine's own slot table answers (IndexOf), never what the profile says.
    // A recorded slot the table already holds is provisioned SOMEWHERE ELSE, and when the table has
    // nothing free the attempt is refused by name.  INV-PP1: remapped or refused, never adopted.
    //
    // The pair is published at ASSIGNMENT, not at success, because the claim is what a later
    // attempt on the same slot has to reuse; `provisioned` and `refusals` say whether the pawn
    // arrived, and the map says where it was asked for.
    uint16_t live = s.mapped;
    if (live == kSlotUnmapped) {
        uint16_t unreadable = 0;
        if (!AssignMappedSlot(slot.v, live, unreadable)) {
            // L3 #13 printed sixteen slots "held" on a session with ONE local player, which cannot
            // be sixteen entrants; the old two-state line could not tell that apart from a full
            // lobby, so the run could not say which. Both WORDS are printed now: the side table's
            // index for the slot, and the slot the entrant that index names names back. Neither is
            // an identifier -- one is a table index, one is a slot number, both 16 bits wide by
            // the engine's own layout.  The lobby's peer id IS one, so it is cut to that same
            // width: its low 16 bits tell two records apart inside one line, which is all this
            // line asks of a peer, and the state word beside them says which record is free.
            // Bounded: ENTITY.USER_SLOT_TABLE_MAX entries into a fixed buffer, and the legend is
            // terse because bootstrap/log.h caps a line at kLogLineMax and the sixteen entries are
            // the payload: a ..xxxx peer is the LOBBY's own conn[slot] word cut to its low 16
            // bits, and an xxxx/xxxx pair is the side table answering because the lobby chain could
            // not be walked. The widest table the loop can now build -- sixteen unreadable side
            // table entries -- fits inside kLogLineMax, which the full-width peer form did not.
            if (env_.log) {
                // The widest entry this loop can append -- the side table form, now that the peer
                // is redacted -- spelled as the entry itself so the buffer is sized by the format
                // it holds and not by a number beside it.
                constexpr size_t kEntryMax = sizeof("15=..ffff/ffff:unreadable,") - 1;
                char seen[facts::ENTITY_USER_SLOT_TABLE_MAX * kEntryMax + 1];
                size_t n = 0;
                for (uint16_t i = 0; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i) {
                    const SlotProbe pr = ProbeSlot(i);
                    const char* st = pr.state == SlotOccupancy::Held   ? "held"
                                     : pr.state == SlotOccupancy::Free ? "free"
                                                                       : "unreadable";
                    const unsigned long long peerLow =
                        static_cast<unsigned long long>(static_cast<uint16_t>(pr.peer));
                    const int w =
                        pr.lobby ? snprintf(seen + n, sizeof(seen) - n, "%s%u=..%04llx/%04x:%s",
                                            n ? "," : "", i, peerLow, pr.index, st)
                                 : snprintf(seen + n, sizeof(seen) - n, "%s%u=%04x/%04x:%s",
                                            n ? "," : "", i, pr.index, pr.named, st);
                    if (w <= 0 || size_t(w) >= sizeof(seen) - n) break;
                    n += size_t(w);
                }
                seen[n] = 0;
                env_.log->Write(LogLevel::Warn, "pawn",
                                "recorded slot %u not mapped, slot=[..peer/]index[/named]:state "
                                "(entrant table decides; ..peer is conn[] diagnostic only) -- %s",
                                slot.v, seen);
            }
            // asked-fail and could-not-ask are different refusals. Only the first means the table
            // is full, and only the first is an argument for growing or re-keying it.
            return Refuse(slot, unreadable ? PawnProvision::Refusal::SlotOccupancyUnreadable
                                           : PawnProvision::Refusal::SlotAlreadyOccupied);
        }
        s.mapped = live;
        // Bounded by construction: one increment per slots_ entry, and slots_ and map are both
        // ENTITY.USER_SLOT_TABLE_MAX long.
        ledger_.map[ledger_.mapped].slot_recorded = slot.v;
        ledger_.map[ledger_.mapped].slot_resolved = live;
        ++ledger_.mapped;
    }
    const UserSlot target(live);

    // ★ THE ENGINE'S OWN RECORD DECIDES, NOT THE ROUTE THAT ASKED.  L3 #22: the reply route
    // deferred (ReplyWrongPhase, nothing published from this phase) and the ENGINE nevertheless
    // printed `Entrant OVR-... added to slot 1` / `Adding remote user to slot 1` / `Player Actor
    // Allocating` on a later tick.  A provision this session asked for had COMPLETED, and the
    // ledger still said `provisioned 0`, because the answer was taken from the route's return
    // value instead of from the engine.  So an attempt that has been made is re-read, once per
    // cadence window, against the engine's own entrant table: whichever route completed it, a live
    // entrant in the slot THIS provisioner claimed is this provisioner's pawn.
    //
    // It is a deferral, not a latch -- the slot stays Pending and the next window asks again -- and
    // it is bounded by the same retry cadence every other engine read here is bounded by.
    // `s.asked` is the guard that keeps it from being an ADOPTION (INV-PP1): the slot was proven
    // free at assignment and nothing but our own attempt has asked for an entrant in it.
#ifndef W2P1_MUTATE_PROVISION_KEYED_ON_ROUTE
    if (s.asked) {
        // THE ENTRANT TABLE ANSWERS, NOT THE DRESS PREDICATE (L3 #24 F1).  The engine's own slot-1
        // dump reads `Actor 0x... (disabled)  (pooled)` with `Loadouts : 0`: the entrant IS in the
        // table and it is NOT dressed, and ResolveReplayUser is the five-predicate dress test,
        // which answers no for exactly that record.  Keyed on it, this re-read could never fire on
        // the one shape it was written for and the slot stayed Pending for the life of the
        // session.  Occupancy is what IndexOf answers -- the same surface Admit re-asks and the
        // same one AssignMappedSlot proved FREE before we asked -- and being dressed is a LATER
        // step the hop chain reports on by its own name.
        UserIndex settled(kSlotUnmapped);
#ifndef W2P1_MUTATE_REREAD_WANTS_A_DRESSED_ENTRANT
        settled = env_.identity->IndexOf(target);
#else
        if (!env_.identity->ResolveReplayUser(target, settled)) settled = UserIndex(kSlotUnmapped);
#endif
        // ★ THE BARE INDEX WORD IS NOT EVIDENCE (L3 #25's CRASH).  The per-slot side table
        // IndexOf reads is not written by the admission path, and on the empty offline lobby L3
        // #17 recorded it answers the slot's OWN NUMBER as a legal index for a slot no entrant
        // occupies.  Keyed on that alone this re-read settled recorded slot 0 onto live slot 1
        // five seconds before the engine took an access violation inside its loadout apply, with
        // no `added to slot 1` anywhere in the engine's log: there was no entrant.  The rule this
        // file already states for that table 300 lines below -- believed only when the entrant it
        // names agrees it holds the slot -- is the rule here too, and it is the same cross-check
        // ProbeSlot makes to call a slot Held.
#ifndef W2P1_MUTATE_REREAD_TRUSTS_THE_BARE_INDEX
        if (settled.v != kSlotUnmapped && env_.identity->SlotOf(settled).v != target.v)
            settled = UserIndex(kSlotUnmapped);
#endif
        if (settled.v != kSlotUnmapped) {
            // The entrant names the slot back and it is STILL not necessarily ours: the engine
            // re-seats the local user across the state 8->9 edge, and an index that was in the
            // table before this provisioner asked belongs to the viewer.  Handing it on is how a
            // file's loadout reaches the live local user (INV-PP1: never adopted).
#ifndef W2P1_MUTATE_NO_VIEWER_EXCLUSION_ON_REREAD
            if (ForeignEntrant(settled))
                return Refuse(slot, PawnProvision::Refusal::ViewerSlotExcluded);
#endif
            if (env_.log) {
                env_.log->Write(LogLevel::Info, "pawn",
                                "recorded slot %u -> slot %u: the engine's own entrant record "
                                "answers index %u, so the provision this session asked for "
                                "completed after '%s' -- counted from the record, not the route",
                                slot.v, live, settled.v, PawnProvision::RefusalName(s.why));
            }
            return Settle(slot, s, settled);
        }
    }
#endif

    // ★ ENG-NATIVE-ADMIT (echo-101).  On the engine's OWN remote-admission reply route, nothing
    // fills the per-user loadout SOURCE that the add-entrant handler's unconditional tail bridge
    // (IDENT.LOADOUT_ASSIGN, sub_14014F230) copies into the live table -- online it is replication,
    // offline it is nobody -- so the entrant is admitted over a ZERO-LENGTH instance array and the
    // framework's phase-16 model-build reads past the end of an empty stream, which the deadlock
    // monitor aborts on.  Parse the recorded loadout into that SOURCE (LoadoutContainer, at
    // ng + 31648 + 19720*slot) BEFORE the reply is dispatched -- the handler's AddRemoteUser writes
    // nothing into the loadout header before the bridge, so a pre-fill survives to be copied.  Once
    // per slot: the SOURCE persists across the admission that reads it.  Best-effort -- a parse that
    // does not take leaves the count at 0 and the entrant is admitted exactly as before (the prior
    // non-crashing outcome), never blocked.  Harmless on the bot path (AddBotUser's direct insert
    // has no bridge, so the live count stays 0 and the L3 #18 guard defers as it does today).
    // Wired only for the remote reply route (env.sourceLoadoutBeforeAdmit ==
    // pawn.remote_source_loadout && !pawn.bot_body): the shipped bot-body default never enters here,
    // so the default admission flow is unchanged, and A/B off (ECHOREPLAY_PAWN_REMOTE_SOURCE_LOADOUT=0)
    // reproduces the empty-loadout deadlock as a control.
    if (env_.sourceLoadoutBeforeAdmit && !s.sourceLoadoutParsed && env_.recordedLoadout != nullptr) {
        s.sourceLoadoutParsed = true;
        // The RECORDED slot keys the recorded-loadout lookup; DriveLoadout writes the SOURCE at the
        // LIVE slot it owns (s.mapped == target), which is the slot the admission bridge reads.
        DriveLoadout(slot, s, replay::identity_access::LoadoutPhase::Source);
    }

    // The attempt is on record BEFORE the engine is called: a call that defers, refuses or does not
    // return still happened, and the re-read above is what makes an asynchronous completion visible.
    s.asked = true;

    // ENG-BOTROUTE-FIX (Change A).  On the bot body-build route the SOURCE parse ran off the manifest
    // entrantJson, which the engine parser fills to ZERO loadout instances -> empty-loadout deadlock
    // (DIAG-BOTROUTE-PARSE).  Carry the identity-block loadout document -- the exact bytes the remote
    // route's DriveLoadout parses to one real instance, {rl.document, rl.documentBytes} from
    // env_.recordedLoadout / ReadRecordedLoadout, keyed on the RECORDED slot -- to the admission on a
    // dedicated JoinDesc field, so HookSourceLoadout binds byte-identical input.  The header pair stays
    // on the descriptor unchanged (it already matched).  Gated on env_.botBodySourceLoadout (the same
    // config that wires HookSourceLoadout), so the shipped default never enters here and passes `desc`
    // as before.  The document is a view into the loaded recording, alive for this call.
    const JoinDesc* admit = &desc;
    JoinDesc withRecordedDocument;
    if (env_.botBodySourceLoadout && env_.recordedLoadout != nullptr) {
        replay::identity::RecordedLoadout rl{};
        if (env_.recordedLoadout(slot.v, rl, nullptr) &&
            rl.document != nullptr && rl.documentBytes != 0) {
            withRecordedDocument = desc;
            withRecordedDocument.recordedLoadoutDocument =
                Blob{reinterpret_cast<const uint8_t*>(rl.document), rl.documentBytes};
            admit = &withRecordedDocument;
        }
    }

    UserIndex index(kSlotUnmapped);
    if (!env_.identity->ProvisionReplayUser(target, *admit, index))
        return Refuse(slot, env_.identity->LastProvisionRefusal());
    if (index.v == kSlotUnmapped)
        return Refuse(slot, PawnProvision::Refusal::InsertReturnedNoIndex);


    // The postcondition, asserted through the same five predicates the dresser will use.  An insert
    // that "succeeded" and left the profile empty is the L3 #11 failure arriving one step later.
    UserIndex check(kSlotUnmapped);
    if (!env_.identity->ResolveReplayUser(target, check))
        return Refuse(slot, env_.identity->LastProvisionRefusal());

    return Settle(slot, s, index);
}

bool PawnProvisioner::Settle(UserSlot slot, Slot& s, UserIndex index) {
    s.index = index;
    // THE ADMISSION IS NOT THE PAWN.  An entrant exists; whether it has a body is four engine
    // records away, and L3 #23 read all four of them as no.  With the chain wired the slot goes
    // to Awaiting and is counted as provisioned only when every hop reads done, so `provisioned N`
    // stays the number of DRIVEABLE pawns rather than the number of admissions.
    if (env_.hops != nullptr) {
        s.state = SlotState::Awaiting;
        s.why = PawnProvision::Refusal::None;
        return AdvanceSlot(slot, s);
    }
    s.state = SlotState::Provisioned;
    s.why = PawnProvision::Refusal::None;
    ++ledger_.provisioned;
    last_ = PawnProvision::Refusal::None;
    // An entrant just landed, so every other pending slot's occupancy answer has moved: the rest
    // of this Pump pass must not be held behind a cadence keyed on a blocker that did not change.
    ReopenAttemptWindow();
    return true;
}

// =============================================================================================
// THE DRESS.  L1..L6 for one slot, ARCH-23 S14.8/S14.8b/S14.8c, driven from AdvanceSlot and from
// nowhere else.  Everything it needs arrives through a provider slot (P1-18): a build with no
// recorded-identity reader and a build with no engine surface both refuse BY NAME, and neither
// can be read as an entrant who simply had nothing to wear.
// =============================================================================================

static_assert(replay::identity_access::LoadoutRefusal{} ==
                  replay::identity_access::LoadoutRefusal::None,
              "Slot::loadoutWhy is value-initialised because pawn_provision.h only forward-"
              "declares the enum; zero has to BE None or an undressed slot reads as dressed");
static_assert(replay::identity_access::kLoadoutPairCountUnknown ==
                  replay::identity::kRecordedPairCountUnknown,
              "the engine side's could-not-ask sentinel and the recording's are one value; a "
              "mismatch would turn `the capture could not ask' into a pair count of 65535");
static_assert(replay::identity::kRecordedLoadoutPairCap == 20,
              "the recorded pair array and the twenty slots the document author writes are the "
              "same twenty");

// THE PUBLISH HALF'S REFUSALS THAT CAN CLEAR ON A LATER WINDOW, and they are the ones about
// the ENGINE'S state rather than the recording's: the per-user container is memory that
// becomes committed some windows after the entrant is admitted, a parse that did not fill it
// and a publish that did not read back are both reports about THIS pass.  A recording that
// carries no slot keys, or no document at all, will say so identically on every window.
static bool PublishHalfRetryable(replay::identity_access::LoadoutRefusal why) {
    namespace ia = replay::identity_access;
    return why == ia::LoadoutRefusal::ContainerUnreadable ||
           why == ia::LoadoutRefusal::ParseDidNotFill ||
           why == ia::LoadoutRefusal::PublishDidNotTake ||
#ifndef W2P1_MUTATE_PUBLISH_LATCH_FOREVER
           // AUD-W2-P1-34 P1-38 F2.  L2's two answers are the engine's own: the attach read
           // back a document that is not ours, or a per-user profile the engine does not yet
           // hold as usable.  The publish half now runs in the SAME window the entrant is
           // admitted, which is exactly when that record is youngest, and excluding these two
           // latched the half for ever on a state the next window routinely clears -- the
           // enable then went out at `Loadouts : 0` and the entrant wore nothing all match.
           why == ia::LoadoutRefusal::DocumentNotAttached ||
           why == ia::LoadoutRefusal::DocumentNotUsable ||
#endif
           false;
}

void PawnProvisioner::DriveLoadout(UserSlot slot, Slot& s,
                                   replay::identity_access::LoadoutPhase phase) {
    namespace ia = replay::identity_access;
    // ONCE PER SLOT PER LOAD (AUD-W2-P1-30-part2 F14): `loadoutAttempted` latches here and
    // AdvanceSlot's gate never asks twice, so the recording is read for an entrant exactly once.
    // The provider re-asks its source on every call, which is a property the CENSUS route uses;
    // on THIS path it buys nothing and a loadout swapped mid-match is not picked up.  Making the
    // dress re-askable when the source's horizon moves is S14.8b's design question, not a defect.
    // The Source phase has its OWN latch (sourceLoadoutParsed, set by the
    // caller) and runs before admission, so it must not touch either dress latch: setting
    // loadoutAttempted here would make AdvanceSlot's later apply-half gate think the recording was
    // already read and skip it, and the entrant would go undressed after admission.
    if (phase == ia::LoadoutPhase::Publish) s.loadoutPublishAttempted = true;
    else if (phase != ia::LoadoutPhase::Source) s.loadoutAttempted = true;
    s.loadoutMatched = 0;
    s.loadoutCompared = 0;
    s.loadoutSourceWhy = nullptr;

    // THE RECORDING, asked first and asked of the reader that owns the format.  No provider is a
    // could-not-ask; a provider that refuses hands back its own word and we keep it verbatim,
    // because "which block was missing" is a thing no enum of ours can say.
    if (env_.recordedLoadout == nullptr) {
        s.loadoutWhy = ia::LoadoutRefusal::RecordingNotAsked;
        return;
    }
    replay::identity::RecordedLoadout rl{};
    const char* sourceWhy = nullptr;
    if (!env_.recordedLoadout(slot.v, rl, &sourceWhy)) {
        s.loadoutSourceWhy = sourceWhy;
        s.loadoutWhy = ia::LoadoutRefusal::DocumentNotRecorded;
        return;
    }

    // THE ENGINE SURFACE.  Null slot means "this image, at its recorded VAs"; false from either
    // means an entry point did not resolve and nothing is driven with a hole in the sequence.
    ia::LoadoutEngine eng{};
    bool bound = env_.loadoutEngine != nullptr
                     ? env_.loadoutEngine(eng)
                     : (env_.hops != nullptr && ia::BindLoadoutEngine(eng, *env_.hops));
#ifdef W2P1_MUTATE_LOADOUT_ENGINE_UNBOUND_SILENT
    bound = true;   // drive the sequence with whatever bound, and let the steps fall where they may
#endif
    if (!bound) {
        s.loadoutWhy = ia::LoadoutRefusal::EngineUnbound;
        return;
    }

    // The recorded side, translated field for field.  Nothing is invented: a v1 block arrives with
    // hasSlotKeys false and L1 refuses it by name rather than pairing items with positions.
    ia::RecordedLoadout rec{};
    for (uint16_t i = 0; i < replay::identity::kRecordedLoadoutPairCap; ++i) {
        rec.pairs[i].slotKey = rl.pairs[i].slotKey;
        rec.pairs[i].itemId  = rl.pairs[i].itemId;
    }
    rec.filledPairCount = rl.filledPairCount;
    rec.hasSlotKeys     = rl.hasSlotKeys;
    rec.documentRecorded =
        rl.profile == replay::identity::RecordedProfileState::Present && rl.documentBytes != 0;
    // The container's own header pair, which L3 reads back off the parse.  The recording carries
    // them on the JoinDesc the admission was authored from; they are not in the identity block.
    rec.number     = s.desc.loadoutNumber;
    rec.numberBody = s.desc.loadoutNumberBody;
    rec.alignment  = s.desc.alignment;

    // THE DOCUMENT.  A surface that already carries one (a test's) is used as it stands; otherwise
    // the recording's own bytes are parsed into a document this process owns, which L2 attaches.
    entity_json::CJsonReader parsed{};
    bool ours = false;
    char authoredDoc[512] = {0};
    ia::AuthoredLoadout authored{};
    bool didAuthor = false;
    if (eng.doc == nullptr && rl.document != nullptr && rl.documentBytes != 0) {
        const Blob bytes{reinterpret_cast<const uint8_t*>(rl.document), rl.documentBytes};
        if (ia::BindLoadoutDocument(parsed, bytes)) {
            eng.doc = &parsed;
            ours = true;
        }
    } else if (eng.doc == nullptr &&
               PawnProvision::ShouldAuthorDocument(env_.authorLoadoutDocument,
                                                   rl.document != nullptr && rl.documentBytes != 0,
                                                   rl.hasSlotKeys, rl.filledPairCount)) {
        // The recorder could not read this entrant's profile, so the engine has no
        // document to build a loadout INSTANCE from and the body wears the default.  Author the
        // document the recording CAN support -- the recorded name and both numbers -- and let the
        // engine's own per-slot writer put the items in it: a recorded Symbol64 is a one-way hash
        // of the item's name, and the engine's symbol registry is the only thing that can undo it.
        const size_t n = ia::AuthorProfileDocument(s.desc.displayName, s.desc.loadoutNumber,
                                                   s.desc.loadoutNumberBody, authoredDoc,
                                                   sizeof authoredDoc, authored);
        const Blob bytes{reinterpret_cast<const uint8_t*>(authoredDoc), uint32_t(n)};
        if (n != 0 && ia::BindLoadoutDocument(parsed, bytes)) {
            eng.doc = &parsed;
            ours = true;
            didAuthor = true;
            uint64_t keys[replay::identity::kRecordedLoadoutPairCap];
            uint64_t items[replay::identity::kRecordedLoadoutPairCap];
            for (uint16_t i = 0; i < replay::identity::kRecordedLoadoutPairCap; ++i) {
                keys[i] = rl.pairs[i].slotKey;
                items[i] = rl.pairs[i].itemId;
            }
            (void)ia::FillAuthoredLoadoutSlots(parsed, eng, keys, items, rl.filledPairCount,
                                               authored);
            // The document now holds what the recording holds, so L1's source gate is answered by
            // the authored document -- not by a claim that the recorder captured one.
            rec.documentRecorded = true;
        }
    }
    if (didAuthor && env_.log != nullptr) {
        char held[24];
        if (authored.inDocument == ia::kAuthoredCountUnasked)
            _snprintf_s(held, sizeof(held), _TRUNCATE, "unasked");
        else
            _snprintf_s(held, sizeof(held), _TRUNCATE, "%u", unsigned(authored.inDocument));
        env_.log->Write(LogLevel::Info, "pawn",
                        "recorded slot %u: no profile document was captured -- authored one from "
                        "the recording (%u pairs written, %s in the document, %u unworn, %u slot "
                        "key(s) unknown [first %016llx], %u item id(s) the engine could not name "
                        "[first %016llx]%s)",
                        slot.v, unsigned(authored.written), held, unsigned(authored.unfilled),
                        unsigned(authored.unresolvedSlot),
                        (unsigned long long)authored.firstUnresolvedSlotKey,
                        unsigned(authored.unresolvedItem),
                        (unsigned long long)authored.firstUnresolvedItemId,
                        authored.nameOmitted ? ", display name omitted: not valid UTF-8" : "");
    }

    ia::LoadoutSteps steps{};
    void* ng = env_.netGame != nullptr ? env_.netGame() : nullptr;
    s.loadoutWhy = ia::DressLoadout(ng, UserSlot(s.mapped), s.index, rec, eng, steps, phase);
    s.loadoutCompared = steps.compared;
    s.loadoutMatched  = steps.matched;

    // Freed only when L2 never took it: past the attach the entrant's user record owns it and
    // freeing it here would leave the engine dereferencing our free list.
    if (ours && steps.outcome[size_t(ia::LoadoutStep::Attach)] != ia::LoadoutOutcome::Issued)
        ia::ReleaseLoadoutDocument(parsed);

    // THE PUBLISH HALF'S OWN LINE, and the count on it is the one the enable about to be
    // issued will read.  It is not the acceptance census: nothing has been applied yet and
    // printing "matched 0" here would read as twenty slots the engine refused.
    if (phase == ia::LoadoutPhase::Publish) {
        // LATCHED HERE from the publish half, one instruction before it returned.  AdvanceSlot
        // re-reads it every window as the actor side comes up, but an Unasked re-read cannot erase
        // this NO -- so this line reports the reading that spoke for the publish.
        s.loadoutGateVerdict = steps.applyGates.verdict;
        // The count L4 read back, and only L4 reads it: a refusal at L1..L3 leaves the
        // could-not-ask value, which prints as a WORD.  "live instances 0" for a table nobody
        // asked about is the enable-will-refuse reading, and it is not what happened.
        char inst[24];
        if (steps.instances == ia::kLoadoutInstancesUnreadable)
            _snprintf_s(inst, sizeof(inst), _TRUNCATE, "unread");
        else
            _snprintf_s(inst, sizeof(inst), _TRUNCATE, "%llu",
                        (unsigned long long)steps.instances);
        // THE FOUR GATES, on the line the publish half is judged by.  Each reading prints its
        // could-not-ask as a WORD: `pairs=unread` is not `pairs=0`, and an actor nobody could
        // resolve must not appear here as an actor that resolved to nothing.
        const ia::LoadoutApplyGates& g = steps.applyGates;
        char pairs[16];
        if (g.pairs == ia::kLoadoutPairsUnknown)
            _snprintf_s(pairs, sizeof(pairs), _TRUNCATE, "unread");
        else
            _snprintf_s(pairs, sizeof(pairs), _TRUNCATE, "%llu", (unsigned long long)g.pairs);
        char valid[16];
        if (!g.compValidAsked)
            _snprintf_s(valid, sizeof(valid), _TRUNCATE, "notAsked");
        else
            _snprintf_s(valid, sizeof(valid), _TRUNCATE, "%u", unsigned(g.compValid));
        // OBSERVE-ONLY (sec B): whether this entrant's apply runs inline or is parked on the
        // deferred queue a spectator session may never drain.  `unread` when the queue could not
        // be probed -- a queue that would not read back must not print as one holding nothing.
        char inl[12], queued[24];
        if (!g.queueAsked) {
            _snprintf_s(inl, sizeof(inl), _TRUNCATE, "unread");
            _snprintf_s(queued, sizeof(queued), _TRUNCATE, "unread");
        } else {
            _snprintf_s(inl, sizeof(inl), _TRUNCATE, "%s", g.inlineApply ? "y" : "n");
            _snprintf_s(queued, sizeof(queued), _TRUNCATE, "%lld", (long long)g.queueDepth);
        }
        if (env_.log != nullptr)
            env_.log->Write(LogLevel::Info, "pawn",
                            "recorded slot %u -> slot %u: index %u loadout publish %s -- "
                            "live instances %s, pairs=%s actor=0x%p compCs=0x%p compKey=0x%08X "
                            "compValid=%s inline=%s queued=%s -> gate=%s, before the enable",
                            slot.v, s.mapped, s.index.v, ia::LoadoutRefusalName(s.loadoutWhy),
                            inst, pairs, g.actor, g.compCs, unsigned(g.compKey), valid,
                            inl, queued, ia::LoadoutApplyGateName(g.verdict));
        return;
    }

    // THE SOURCE PHASE'S OWN LINE.  It ran before admission and filled the
    // per-user loadout SOURCE the engine's remote add-entrant handler bridges into the live table.
    // The count printed here is the one that decides the experiment: > 0 means the model-build the
    // framework runs at its phase-16 tick reads real instances, not the zero-length array the
    // deadlock monitor aborted on.
    if (phase == ia::LoadoutPhase::Source) {
        if (env_.log != nullptr) {
            char inst[24];
            if (steps.instances == ia::kLoadoutInstancesUnreadable)
                _snprintf_s(inst, sizeof(inst), _TRUNCATE, "unread");
            else
                _snprintf_s(inst, sizeof(inst), _TRUNCATE, "%llu",
                            (unsigned long long)steps.instances);
            env_.log->Write(LogLevel::Info, "pawn",
                            "recorded slot %u -> slot %u: SOURCE loadout parsed %s -- source "
                            "instances %s (ENG-NATIVE-ADMIT: the engine's own add-entrant bridge "
                            "copies this into the live table when the reply is dispatched)",
                            slot.v, s.mapped, ia::LoadoutRefusalName(s.loadoutWhy), inst);
        }
        return;
    }

    if (env_.log != nullptr) {
        // THE RECORDED ROW COUNT, PRINTED AS IT WAS RECORDED, and no arithmetic on it.  L1's rule
        // is `compared + unfilled == filledPairCount`, so a reader checks it by ADDING the two
        // tallies on this line -- which is the one thing a derived difference cannot support:
        // `filledPairCount` carries a SENTINEL for `the capture could not ask` (0xFFFF, printed
        // `unknown` here as the capture census prints it), and the PairCountMismatch path can
        // hold `unfilled > filledPairCount`, which wraps unsigned into a ten-digit number.
        char rows[16];
        if (rec.filledPairCount == ia::kLoadoutPairCountUnknown)
            _snprintf_s(rows, sizeof(rows), _TRUNCATE, "unknown");
        else
            _snprintf_s(rows, sizeof(rows), _TRUNCATE, "%u", unsigned(rec.filledPairCount));

        // The count the engine's apply ITERATES, and the gate's second operand
        // L5 read.  Both carry a could-not-ask value, and both print it as a word: an instance
        // this pass never got to read must not appear on the line as an instance holding nothing.
        char items[16];
        if (steps.items == ia::kLoadoutItemsUnknown)
            _snprintf_s(items, sizeof(items), _TRUNCATE, "unread");
        else
            _snprintf_s(items, sizeof(items), _TRUNCATE, "%u", unsigned(steps.items));
        char entAlign[16];
        if (!steps.entityAlignmentAsked)
            _snprintf_s(entAlign, sizeof(entAlign), _TRUNCATE, "notAsked");
        else
            _snprintf_s(entAlign, sizeof(entAlign), _TRUNCATE, "0x%04X", unsigned(steps.entityAlignment));
#ifdef W2P1_MUTATE_CENSUS_DERIVES_DENOMINATOR
        // The defect: a denominator DERIVED from the recorded count, which has no answer
        // to derive from when the count is the `could not ask` sentinel.
        _snprintf_s(rows, sizeof(rows), _TRUNCATE, "%u",
                    unsigned(rec.filledPairCount) - unsigned(steps.unfilled));
#endif
        env_.log->Write(LogLevel::Info, "pawn",
                        "recorded slot %u -> slot %u: index %u loadout %s -- L1 compared %u, "
                        "unfilled %u, of %s recorded rows, engine held %llu, instance items=%s, "
                        "entityAlign=%s, appliedBy=%s, matched %u "
                        "pending %u alignGated %u mismatch %u of %u",
                        slot.v, s.mapped, s.index.v, ia::LoadoutRefusalName(s.loadoutWhy),
                        unsigned(steps.compared), unsigned(steps.unfilled), rows,
                        (unsigned long long)steps.engineHeld, items, entAlign,
                        ia::LoadoutApplyByName(steps.appliedBy),
                        unsigned(steps.matched), unsigned(steps.pending),
                        unsigned(steps.alignGated), unsigned(steps.mismatched),
                        unsigned(steps.compared));

        // ONE ROW PER SLOT THE ENGINE DID NOT SIMPLY WEAR. The counts above say how many took
        // each arm; only these say WHICH, and they carry the three fields the engine branched on
        // so a reader can check the attribution instead of taking the word for it.  A matched
        // slot prints nothing: twenty rows a run cannot act on is the same as none.
        for (size_t i = 0; i < 20; ++i) {
            const ia::LoadoutSlotArm arm = steps.arm[i];
            if (arm == ia::LoadoutSlotArm::NotCompared || arm == ia::LoadoutSlotArm::Matched)
                continue;
            const uint8_t at = steps.engineAt[i];
            const bool have = at < 20;
            // No engine entry carried this key, so there is no item, no pending and no
            // requirement to print: the word says which, and a zero would read as a reading.
            char item[24] = "noEngineSlot";
            char pending[24] = "noEngineSlot";
            char flags[16] = "-";
            char align[16] = "-";
            if (have) {
                const ia::AppliedSlotState& st = steps.engineState[at];
                snprintf(item, sizeof item, "0x%016llX",
                         (unsigned long long)steps.engine[at].itemId);
                snprintf(pending, sizeof pending, "0x%016llX",
                         (unsigned long long)st.pendingItem);
                snprintf(flags, sizeof flags, "0x%08X", unsigned(st.flags));
                snprintf(align, sizeof align, "0x%04X", unsigned(st.alignmentReq));
            }
            env_.log->Write(LogLevel::Info, "identity",
                            "loadout slot %s: rec=0x%016llX item=%s pending=%s "
                            "flags=%s align=%s -> %s",
                            ia::LoadoutSlotName(uint16_t(i)),
                            (unsigned long long)rec.pairs[i].itemId, item, pending, flags, align,
                            ia::LoadoutSlotArmName(arm));
        }
    }
}

// The model-CI presence word, from ComponentOnActor(model_.cs, actor) via IEntity::ModelVisible:
// `absent` is no CI (Lever B), `present(hidden)` a CI with no mesh (a swap state), `present(visible)`
// drawn.  The one signal that separates the two levers, so the drivegate line and the spawn-notify
// A/B probe read it through the same words.
static const char* ModelCiWord(bool present, bool visible) {
    return present ? (visible ? "present(visible)" : "present(hidden)") : "absent(kNoComponent)";
}

// Which arm sub_1401CAC00 takes for the mode word it read: bit1 set, or bit2 clear and bit6 set,
// is the host arm (sub_140CB7C50 + record init) that does NOT install a model CI; otherwise the
// else-if remote arm installs+dresses the ncaModel/swapper CI.  An unread mode word is could-not-ask.
static const char* SpawnNotifyArm(const PawnProvision::SpawnNotifyResult& sn) {
    if (!sn.modeRead) return "modeUnread";
    // The arm the engine ACTUALLY branched on -- the effective (post-flip) word when Stage-B spoofed
    // non-host, else the session word unchanged.  This is what "which arm ran" must report.
    const uint64_t m = sn.modeWordEffective;
    const bool host = (m & 2) != 0 || ((m & 4) == 0 && (m & 0x40) != 0);
    return host ? "host(noInstall)" : "remote(install)";
}

// DIAG-MODEL-BUILD fix (b).  The two-read contract is documented on the declaration.  This is the
// ONE place a run turns RUN-36's silent `reason=noModelOnActor` into an engine-faithful drive of
// the entrant's own body plus a NAMED, BOUNDED account of whether it arrived.
bool PawnProvisioner::DriveEntrantModel(void* ng, UserSlot slot, Slot& s) {
    namespace ia = replay::identity_access;
    (void)slot;
    s.modelWhy = PawnProvision::Refusal::None;
#ifdef W2P1_MUTATE_MODEL_STEP_ABSENT
    (void)ng;
    return false;
#else
    // POSTCONDITION reader, read EVERY window: the engine's own ComponentOnActor(model_.cs, actor),
    // published as IEntity::ModelVisible.  A null surface is a could-not-ask, never a hold.
    if (env_.entity == nullptr) return false;
    bool visible = false;
    const bool present = env_.entity->ModelVisible(s.index, visible);

#ifndef W2P1_MUTATE_PRESTAMP_CURRENT
    if (present) {
        // Dressed.  Latch it so the body is never re-driven: re-skinning an already-dressed actor
        // is exactly the from==to no-op RUN-36 read on the viewer's own record.
        if (!s.modelResolved && env_.log != nullptr)
            env_.log->Write(LogLevel::Info, "pawn",
                            "model idx%u: InstanceModel present (model_.cs on actor != kNoComponent)",
                            unsigned(s.index.v));
        s.modelResolved = true;
        return false;
    }
#else
    // THE DEFECT (PRESTAMP_CURRENT): claim the body resolved without reading the postcondition, so
    // a slot proceeds "dressed" having driven nothing and read nothing.
    (void)present;
    s.modelResolved = true;
    return false;
#endif
    // A body that resolved earlier and now reads absent for a window is not re-driven: the record
    // was dressed once, and the engine owns its lifetime from there.
    if (s.modelResolved) return false;

    // PRECONDITION: undressed (InstanceModel ABSENT -- the observable of the swapper's current ==
    // -1 the engine's != arm needs).  Re-fire the engine's OWN productive apply (LoadoutApplyAlignment
    // -> APPLY_CURRENT_LOADOUT), whose worn-record loop calls Load-and-apply + Apply Chassis and, over
    // a non-empty name-matched loadout instance, loads the asset, installs the InstanceModel CI and
    // takes the swap's != arm (DIAG-MODEL-INSTANTIATE Lever A).  Gated below on the engine-selected
    // instance's +56 being non-empty and the recorded profile being present; while either is not yet
    // satisfied the step HOLDS and re-fires, spending no drive against an instance that would build
    // nothing.
    if (!s.modelDrivePumpKnown) {
        ia::LoadoutEngine eng{};
        const bool bound = env_.loadoutEngine != nullptr
                               ? env_.loadoutEngine(eng)
                               : (env_.hops != nullptr && ia::BindLoadoutEngine(eng, *env_.hops));
        if (!bound || eng.driveModel == nullptr) return false;

        // BOTH the engine's productive apply preconditions, read ONCE.  (a) The engine-visible
        // name-matched instance itemCount (+56), the same reading IDENT.APPLY_LOADOUT_ITEMS gates
        // its loop on.  (b) The entrant's PROFILE, present in the recording: the instance's items
        // derive from the profile document alone (IDENT.LOADOUT_FROM_PROFILE) and it lands LATE.
        ia::LoadoutApplyGates g{};
        bool gatesRead = false;
        if (eng.applyGates != nullptr) {
            // The RESOLVED slot, matching the publish the rebuild re-runs (DressLoadout into
            // UserSlot(s.mapped)) and every sibling live read: a remapped entrant's instance lives
            // at s.mapped, not the recorded slot.v.
#ifndef W2P1_MUTATE_GATE_READ_RECORDED_SLOT
            eng.applyGates(ng, s.mapped, s.index.v, g);
#else
            eng.applyGates(ng, slot.v, s.index.v, g);
#endif
            gatesRead = true;
        }
        replay::identity::RecordedLoadout rl{};
        bool profileAsked = false;
        bool profilePresent = true;   // a getterless image cannot ask -> drive as before
        const char* profileWord = "unasked";
        if (env_.recordedLoadout != nullptr) {
            const char* rlWhy = nullptr;
            const bool got = env_.recordedLoadout(slot.v, rl, &rlWhy);
            profileAsked = true;
            profilePresent = got &&
                             rl.profile == replay::identity::RecordedProfileState::Present &&
                             rl.documentBytes != 0;
            profileWord = !got ? (rlWhy != nullptr ? rlWhy : "unread")
                        : rl.profile == replay::identity::RecordedProfileState::Present  ? "present"
                        : rl.profile == replay::identity::RecordedProfileState::Absent   ? "absent"
                        : rl.profile == replay::identity::RecordedProfileState::Refused  ? "refused"
                                                                                         : "notAsked";
        }

        // INSTRUMENTATION (DIAG-MODEL-INSTANTIATE section 4, RUN-L3-44): the three camera-independent
        // engine reads at the entrant's drive site, one grep-able line per window, so the run settles
        // Lever A (contained) vs Lever B (structural) instead of relaunching blind:
        //   #1 modelCI -- ComponentOnActor(model_.cs, actor) via ModelVisible: absent(kNoComponent)
        //      distinguishes "no CI at all" (Lever B) from "present, meshless/visible" (a swap state);
        //   #2 inst/name/items -- the by-name loadout instance the gate selects (LoadoutInstanceName,
        //      the engine's default-instance symbol) and its +56 itemCount at apply time; the engine's
        //      own "[NETGAME] Applying loadout items for user"/"Unknown loadout instance name" lines
        //      name the engine-side selection to cross-check against this `name`;
        //   plus the recorded profile the drive gates on.  If items>0 while the engine enumerates 0 the
        //   divergence is instance-selection/TOCTOU; if items reads 0 here it is the late profile.
        char items[24];
        if (!gatesRead)
            _snprintf_s(items, sizeof(items), _TRUNCATE, "notAsked");
        else if (g.pairs == ia::kLoadoutPairsUnknown)
            _snprintf_s(items, sizeof(items), _TRUNCATE, "unread");
        else
            _snprintf_s(items, sizeof(items), _TRUNCATE, "%llu", (unsigned long long)g.pairs);
        const char* modelCI = ModelCiWord(present, visible);
        if (env_.log != nullptr) {
            const void* gateInst = nullptr;
            if (ia::LoadoutHeader* hdr = ia::LiveLoadout(ng, UserSlot(s.mapped)))
                gateInst = ia::FindInstance(hdr, ia::LoadoutInstanceName());
            env_.log->Write(LogLevel::Info, "pawn",
                            "model idx%u drivegate: modelCI=%s gate slot %u inst=0x%016llX "
                            "name=0x%016llX items=%s | recorded profile=%s docBytes=%u",
                            unsigned(s.index.v), modelCI, unsigned(s.mapped),
                            (unsigned long long)(uintptr_t)gateInst,
                            (unsigned long long)ia::LoadoutInstanceName(), items,
                            profileWord, profileAsked ? rl.documentBytes : 0u);
        }

        // ENG-MODEL-PUMP instrumentation: the two facts a relaunch needs to split "enqueued and stuck"
        // (pump not landing) from "landed but unread" (reader).  The RAW ncaModel handle for the LOCAL
        // viewer (idx0, which renders offline) and for this entrant proves the reader by construction:
        // idx0 != 0xFFFF means the reader can see a present model.  The swapper in-flight flag + pending
        // queue depth say whether the swap is still enqueued (inflight=1, pending>0 -> FIX A pump) or has
        // completed (inflight=0).  modelCsResolved separates a null model system from an absent component.
        if (env_.log != nullptr && env_.entity != nullptr) {
            const uint16_t h0  = env_.entity->ModelComponentHandle(UserIndex(0));
            const uint16_t hIx = env_.entity->ModelComponentHandle(s.index);
            ia::SwapperEntrantState sw;
            ia::ReadSwapperEntrant(ng, s.index.v, sw);
            // (B) is the built-mesh signal (ncaModel handle via ComponentOnActor: 0xFFFF -> present
            // is the landing); (A) is the desired-model hash the dispatcher compares (0xDAC before the
            // reset, then the built model once the engine completes the swap).  The pair (A present +
            // B absent) is the from==to no-op this pass clears.
            char curWord[24];
            if (sw.currentRead)
                _snprintf_s(curWord, sizeof(curWord), _TRUNCATE, "0x%016llX",
                            (unsigned long long)sw.currentModel);
            else
                _snprintf_s(curWord, sizeof(curWord), _TRUNCATE, "unread");
            env_.log->Write(LogLevel::Info, "pawn",
                            "model idx%u cadence: modelCsResolved=%s modelCI handle idx0=0x%04X "
                            "idx%u=0x%04X | swapper resolved=%s inflight=%u pending=%llu handle=0x%04X "
                            "current=%s",
                            unsigned(s.index.v),
                            env_.entity->ModelSystemResolved() ? "yes" : "no", unsigned(h0),
                            unsigned(s.index.v), unsigned(hIx), sw.resolved ? "yes" : "no",
                            unsigned(sw.inFlight), (unsigned long long)sw.pending, unsigned(sw.handle),
                            curWord);
        }

        // PRECONDITION (a): the entrant's recorded profile is present.  Until it lands the instance
        // is empty and driving skips the +56 loop with no swap.  HOLD -- do NOT spend the one-shot
        // drive and do NOT burn a rebuild (re-publishing an absent profile authors nothing) -- and
        // re-fire once it lands.  Bounded: a recording that genuinely never carries one provisions
        // bodyless rather than holding for ever.
#ifndef W2P1_MUTATE_DRIVE_BEFORE_PROFILE_LANDS
        if (profileAsked && !profilePresent) {
            if (s.modelProfileWaits >= kModelProfileWaitWindows) {
                if (env_.log != nullptr)
                    env_.log->Write(LogLevel::Info, "pawn",
                                    "model idx%u: entrant profile never landed (%s) after %u windows "
                                    "-- the loadout instance stays empty, provisioning bodyless",
                                    unsigned(s.index.v), profileWord,
                                    unsigned(kModelProfileWaitWindows));
                s.modelWhy = PawnProvision::Refusal::LoadoutInstanceEmpty;
                return false;
            }
            ++s.modelProfileWaits;
            if (env_.log != nullptr)
                env_.log->Write(LogLevel::Info, "pawn",
                                "model idx%u: entrant profile not yet present (%s) -- holding the "
                                "model drive until it lands", unsigned(s.index.v), profileWord);
            s.modelWhy = PawnProvision::Refusal::LoadoutInstanceEmpty;
            return true;  // hold Awaiting; re-read the recording on a LATER pump
        }
#endif

        // PRECONDITION (b) (DIAG-MODEL-INSTANTIATE Lever A): the engine's productive apply builds the
        // body only over a NON-EMPTY name-matched loadout instance -- its Load-and-apply loop reads the
        // instance's items (+56) and skips the whole loop when the count is 0 (RUN-40/43: the instance
        // was empty, no Load and apply, no body).  So GATE THE DRIVE on the engine-selected instance's
        // +56 being non-empty, and while it is empty HOLD: best-effort re-publish the instance to fill
        // it (bounded) and re-fire on a later pump.  This replaces the prior "drive Apply Chassis per
        // recorded pair" -- Apply Chassis only skins an existing model-CI and could not create one.  No
        // one-shot latch here: the drive is spent only once the instance is actually non-empty.
#ifndef W2P1_MUTATE_DRIVE_APPLY_CHASSIS_ON_ABSENT_MODEL
        if (gatesRead && (g.pairs == ia::kLoadoutPairsUnknown || g.pairs == 0)) {
            if (s.modelInstanceRebuilds >= kModelInstanceRebuildLimit) {
                if (env_.log != nullptr)
                    env_.log->Write(LogLevel::Info, "pawn",
                                    "model idx%u: engine-selected instance still empty (+56=%s) after "
                                    "%u re-publishes -- no items to apply, provisioning bodyless",
                                    unsigned(s.index.v), items, unsigned(kModelInstanceRebuildLimit));
                s.modelWhy = PawnProvision::Refusal::LoadoutInstanceEmpty;
                return false;
            }
            ++s.modelInstanceRebuilds;
#ifndef W2P1_MUTATE_NO_REFIRE_ON_PROFILE_SETTLE
            // Re-askable dress: the source horizon moved (the late profile / a re-seat that freed the
            // instance), so reset the publish latch and re-run the engine's parse+publish into the live
            // slot to refill the name-matched instance, then re-read +56 on a LATER pump.
            s.loadoutPublishAttempted = false;
            DriveLoadout(slot, s, ia::LoadoutPhase::Publish);
#endif
            if (env_.log != nullptr)
                env_.log->Write(LogLevel::Info, "pawn",
                                "model idx%u: engine-selected instance empty (+56==0) -> best-effort "
                                "re-publish %u; holding the productive-apply re-fire until it fills",
                                unsigned(s.index.v), unsigned(s.modelInstanceRebuilds));
            s.modelWhy = PawnProvision::Refusal::LoadoutInstanceEmpty;
            return true;  // hold; re-read +56 on a LATER pump (drop the one-shot latch)
        }
#endif

        // SECTION-6 A/B PROBE (DIAG-SEAT-DRIVE stops relying on this to dress).  DIAG-SEAT-DRIVE 2.3
        // proved sub_1401CAC00's remote arm is the EQUIPMENT path (R14NetPlayerEquipment) and can
        // NEVER dress the body, so the authflip that forces it is not a body-dress mechanism -- it is
        // disabled by default (config stageb.authflip=false).  This block is kept as the section-4/6
        // A/B probe and for the host (equipment) arm: read the model CI immediately BEFORE and AFTER
        // the call through the same ModelVisible surface and log the mode word the handler branched
        // on.  The body is dressed by the engine's own loadout-apply below, NOT by this call; the
        // probe line records whether the CI moved so a launch can still see the arm's effect (or lack
        // of one).  With authflip off, `arm=host(noInstall)` and modelCI stays absent here -- expected.
#ifndef W2P1_MUTATE_SKIP_SPAWN_NOTIFY
        if (env_.hops != nullptr && env_.hops->spawnNotify != nullptr && !s.spawnNotified) {
            bool visBefore = false;
            const bool ciBefore = env_.entity->ModelVisible(s.index, visBefore);
            const PawnProvision::SpawnNotifyResult sn =
                env_.hops->spawnNotify(env_.hops->ctx, s.index.v);
            bool visAfter = false;
            const bool ciAfter = env_.entity->ModelVisible(s.index, visAfter);
            s.spawnNotified = sn.issued;
            if (env_.log != nullptr)
                env_.log->Write(LogLevel::Info, "pawn",
                                "model idx%u spawnNotify: issued=%d faulted=%d modelCI %s -> %s "
                                "arm=%s authflip=%s sessmode=0x%016llX effmode=0x%016llX "
                                "(bit1=%d bit2=%d bit40=%d) -- equipment arm; the body is dressed by "
                                "the loadout-apply below. absent->absent with authflip=off is expected; "
                                "authflip is disabled because this arm cannot dress the body",
                                unsigned(s.index.v), sn.issued ? 1 : 0, sn.spawnFaulted ? 1 : 0,
                                ModelCiWord(ciBefore, visBefore), ModelCiWord(ciAfter, visAfter),
                                SpawnNotifyArm(sn), sn.authFlipApplied ? "on" : "off",
                                (unsigned long long)sn.modeWord,
                                (unsigned long long)sn.modeWordEffective,
                                int((sn.modeWord >> 1) & 1), int((sn.modeWord >> 2) & 1),
                                int((sn.modeWord >> 6) & 1));
        }
#endif

        // The engine-selected instance is non-empty (or the image cannot ask its +56): re-fire the
        // engine's OWN productive apply ONCE.  This marker precedes the engine's own "[NETGAME] Load
        // and apply 0x.. for user <entrant>" + "[NETGAME] Apply Chassis" + "Starting model swap from
        // 0xFFFF... to 0x<chassis>" lines (all absent for the entrant in RUN-43); their presence for
        // this user index is the RUN-L3-44 proof the creation path ran.
        const PawnProvision::EngineCallLatch latch;
        if (!latch.taken) return false;
#ifndef W2P1_MUTATE_MODEL_NOT_DRIVEN
        if (env_.log != nullptr)
            env_.log->Write(LogLevel::Info, "pawn",
                            "model idx%u: undressed, +56=%s profile=%s -> re-fire engine productive "
                            "apply (LoadoutApplyAlignment); expect [NETGAME] Load and apply for entrant",
                            unsigned(s.index.v), items, profileWord);

        // THE from==to FIX is a TIMING one and lives in UndressPooledSwapperSweep,
        // driven every tick from the stage-0 detour, not here: our entrant grabbed a POOLED actor whose
        // CModelSwapperCS::InitCI seeded "current" from the pooled instance's residual worn hash == the
        // loadout target, so the engine's own enable-time apply's BeginModelSwap took its from==to NO-OP
        // and never enqueued the mesh-build that seats the InstanceModel CI.  That sweep forces current
        // back to -1 in the window BEFORE the engine's apply, so the apply takes the enqueue (!=) arm;
        // the drive below stays as the engine's own productive-apply re-fire.
        const bool drove = eng.driveModel(ng, s.index.v);
        if (env_.log != nullptr)
            env_.log->Write(LogLevel::Info, "pawn",
                            "model idx%u: productive apply dispatched (drove=%d) -- awaiting the async "
                            "InstanceModel swap (drove=0 means the resolve/readiness gate refused)",
                            unsigned(s.index.v), drove ? 1 : 0);
#endif
        s.modelDrivePump = pumps_;
        s.modelDrivePumpKnown = true;
        s.modelWhy = PawnProvision::Refusal::ModelNotInstantiated;
        return true;  // async: hold and re-poll on a LATER pump
    }

    // BOUNDED.  The load is enqueued; past the grace budget without an InstanceModel, stop holding
    // and provision the pawn bodyless with the absence NAMED -- a late body is worth waiting a
    // bounded number of windows for, but a pawn held for ever is worse than one dressed late.
    if (pumps_ - s.modelDrivePump >= kModelGraceWindows) {
        if (env_.log != nullptr)
            env_.log->Write(LogLevel::Info, "pawn",
                            "model idx%u: ModelNotInstantiated after %u windows -- the async swap "
                            "never landed a body, provisioning bodyless", unsigned(s.index.v),
                            unsigned(kModelGraceWindows));
        s.modelWhy = PawnProvision::Refusal::ModelNotInstantiated;
        return false;
    }
    s.modelWhy = PawnProvision::Refusal::ModelNotInstantiated;
    return true;  // still inside the async grace: keep holding
#endif
}

void PawnProvisioner::SwapperResetSweep(void* ng) {
    namespace ia = replay::identity_access;
    if (ng == nullptr || env_.entity == nullptr) return;   // disarmed / not asked
    // ENG-DRESS-LOADOUT (DIAG-SEAT-DRIVE): the sanctioned body-dress mechanism is the engine's own
    // loadout-apply, NOT the model-swapper hook.  This sweep (the pooled current-reset + the direct
    // BeginModelSwap re-trigger) is a competing swapper drive; it is disabled by default so the
    // section-6 probe can settle whether the name-aligned loadout-apply alone dresses (Lever A).
    // Re-armable via config for a launch that finds the loadout-apply route insufficient (Lever B).
    if (!config::Interface()->GetBool("stageb.swapper_hook", false)) return;
    for (Slot& s : slots_) {
        // OUR settled entrants only (the viewer is never admitted into slots_), and only while the
        // body is not yet dressed.  An idle/unsettled slot has no user index to reset.
        if (s.index.v == kSlotUnmapped) continue;
        if (s.state != SlotState::Awaiting && s.state != SlotState::Provisioned) continue;
        if (s.modelResolved) continue;
        if (!PawnProvision::SwapDriveAllowed(s.desc.team, s.chassisUnfilled)) continue;

        // The built-mesh signal (ComponentOnActor(model_.cs, actor)): present -> the body landed, STOP.
        bool visible = false;
        const bool meshPresent = env_.entity->ModelVisible(s.index, visible);

        ia::SwapperEntrantState sw;
        ia::ReadSwapperEntrant(ng, s.index.v, sw);

        // Capture the pooled target hash while current is still a real stale value: under the from==to
        // no-op this is exactly the model the engine's own apply targets, and it survives the -1 the
        // sweep writes next.  The direct re-trigger below needs it once current reads -1.
        if (sw.currentRead && sw.currentModel != ~uint64_t(0) && sw.currentModel != 0) {
            s.swapTargetHash  = sw.currentModel;
            s.swapTargetKnown = true;
        }

        if (ia::SwapperResetShouldFire(meshPresent, sw)) {
            // FIRE the -1 reset so the engine's Apply Chassis takes the enqueue (!=) arm.  Once done,
            // current reads -1 -> the gate above stops firing until the engine moves it, so a landed
            // body never churns.
            const ia::SwapperResetOutcome rst = ia::ResetSwapperCurrent(ng, s.index.v);
            if (env_.log != nullptr)
                env_.log->Write(LogLevel::Info, "pawn",
                                "model idx%u: swapper current reset (pre-apply sweep) resolved=%s "
                                "located=%s wrote=%d faulted=%d before=0x%016llX after=0x%016llX "
                                "pending=%llu meshPresent=%d",
                                unsigned(s.index.v), rst.resolved ? "yes" : "no",
                                rst.located ? "yes" : "no", rst.wrote ? 1 : 0, rst.faulted ? 1 : 0,
                                (unsigned long long)rst.before, (unsigned long long)rst.after,
                                (unsigned long long)sw.pending, meshPresent ? 1 : 0);
            continue;   // one action per slot per tick
        }

        // The sweep already forced current to -1 and it PERSISTED (the engine's own
        // apply has run and no-op'd and is done touching current).  The natural apply builds nothing
        // from a from==to no-op, so enqueue the swap ourselves: call the engine's BeginModelSwap once
        // with the captured target.  current(-1) != target takes the enqueue arm; the FIX-A pump that
        // runs right after this sweep in the same detour then drains it to a built mesh.
        if (ia::BeginSwapShouldFire(meshPresent, sw, s.swapTargetKnown, s.beginSwapIssued)) {
            const ia::BeginSwapOutcome bs = ia::TriggerBeginModelSwap(ng, s.index.v, s.swapTargetHash);
            if (bs.issued || bs.faulted) s.beginSwapIssued = true;   // an attempt ran -> never repeat
            if (env_.log != nullptr)
                env_.log->Write(LogLevel::Info, "pawn",
                                "model idx%u: direct BeginModelSwap issued=%d target=0x%016llX "
                                "resolved=%s located=%s faulted=%d reentrant=%d pendingBefore=%llu "
                                "pendingAfter=%llu",
                                unsigned(s.index.v), bs.issued ? 1 : 0,
                                (unsigned long long)s.swapTargetHash, bs.resolved ? "yes" : "no",
                                bs.located ? "yes" : "no", bs.faulted ? 1 : 0, bs.reentrant ? 1 : 0,
                                (unsigned long long)bs.pendingBefore,
                                (unsigned long long)bs.pendingAfter);
        }
    }
}

// The ungated pooled-swapper dress sequence -- a current-reset FOLLOWED by our own
// BeginModelSwap re-fire, both with NO stageb.swapper_hook gate.  Root cause (RE, ENG-DRESS-LEVERB.md):
// our replay entrant grabs a POOLED actor whose CModelSwapperCS::InitCI (sub_14043BC80) seeds the
// swapper current-model from the pooled instance's residual worn hash (a real chassis model, e.g.
// 0xDAC...) instead of the -1 no-model sentinel a freshly-joined live remote gets.  So every
// CModelSwapperCS::BeginModelSwap (sub_14042D120) evaluates `if (current == target)` true and takes
// the no-op arm -- it fires the swap-COMPLETE delegate and enqueues nothing, so the mesh-build job
// that seats the InstanceModel CI never runs (observed: swapper resolved=yes inflight=0 pending=0
// current=0xDAC while ComponentOnActor(model_.cs,actor)=0xFFFF, and `Starting model swap from 0xDAC
// to 0xDAC`).  LEVERB's reset alone (force current -> -1 before the engine's enable-time apply) was
// not enough: the engine's ONE synchronous enable-time BeginModelSwap already ran and no-op'd on the
// from==to seed, and re-firing the loadout apply only re-skins -- it never re-issues the base-model
// swap.  So at the stable POST-enable state (current already -1 from our reset, pending==0, mesh
// absent, target known) we call the engine's OWN enqueue fn BeginModelSwap (sub_14042D120) once with
// the captured target: current(-1) != target takes the CI-building enqueue arm; the ungated
// SwapperFlushTick pump (identity_access.cpp) then drains it to a built mesh, and the framework's own
// phase-16 (sub_140323790) seats the InstanceModel CI on its own tick -- never sub_140323790 nor the
// seat wrapper sub_14030F120 by us, no puppet, no authflip, no exe patch.  Driving BeginModelSwap
// (sub_14042D120) is the sanctioned enqueue; the target is the pooled worn-model hash (0xDAC-family),
// captured from current while it is still a real value and reused once current reads -1.  The disabled
// SwapperResetSweep (stageb.swapper_hook, still off) is left untouched; this is a distinct path.
// Idempotent/self-stopping: the shared SwapperResetShouldFire stops the reset once current is -1 or a
// mesh lands, and BeginSwapShouldFire fires the enqueue once (mesh-absent, pending==0, current==-1,
// target-known, fire-once latch), so a built body never churns and the reset and enqueue never fire
// the same tick (the reset tick reads current still real -> the enqueue guard's current==-1 is false).
// The primary dress lever.  Offline no loopback login-service answers the
// entrant's profile request, so the engine's own profile handler (sub_14019F510) never fires, the
// model-resource dependency is never requested, and the InstanceModel CI never lands.  This emulates
// that handler with the recorded profile: the ENGINE then requests the resource and seats the CI
// itself via the onlobbymemberprofilereceived delegate -- no puppet, no synthesized model, no seat
// call.  Fire-once per entrant; self-stopping once the CI is visible or the entrant resolves.  Runs
// before the swapper sweeps in the same detour; those self-stop once this lever dresses the body.
void PawnProvisioner::EmulateProfileReceivedSweep(void* ng) {
    namespace ia = replay::identity_access;
    if (ng == nullptr || env_.entity == nullptr) return;   // disarmed / not asked
    for (Slot& s : slots_) {
        // OUR settled entrants only (the viewer is never admitted into slots_), and only while the
        // body is not yet dressed.
        if (s.index.v == kSlotUnmapped) continue;
        if (s.state != SlotState::Awaiting && s.state != SlotState::Provisioned) continue;
        if (s.modelResolved) continue;

        // The built-mesh signal (ComponentOnActor(model_.cs, actor)): present -> the CI seated, STOP.
        bool visible = false;
        const bool meshPresent = env_.entity->ModelVisible(s.index, visible);
        const bool profilePresent =
            s.desc.entrantJson.p != nullptr && s.desc.entrantJson.n != 0;

        if (!ia::EmulateProfileShouldFire(meshPresent, profilePresent, s.profileEmulated)) continue;

        const ia::ProfileReceivedOutcome pr =
            ia::EmulateProfileReceivedCall(ng, s.desc.xplatformId, s.desc.entrantJson);
        // Latch on a definitive outcome -- the handler ran (issued/faulted) or the recording will
        // never parse (parseFailed/jsonRejected).  A transient refusal (image not yet bound, latch
        // held, an arg that did not read back) leaves it clear so a later tick re-asks.
        if (pr.issued || pr.faulted || pr.parseFailed || pr.jsonRejected) s.profileEmulated = true;
        if (env_.log != nullptr)
            env_.log->Write(LogLevel::Info, "pawn",
                            "model idx%u: emulate Profile received (ENG-DRESS-PROFILE) issued=%d "
                            "account=0x%016llX docBytes=%u parseFailed=%d jsonRejected=%d "
                            "parserUnbound=%d handlerUnbound=%d reentrant=%d receiverBad=%d "
                            "faulted=%d meshPresent=%d -- engine seats the InstanceModel CI via the "
                            "onlobbymemberprofilereceived delegate; watch for [NETGAME] Profile "
                            "received for '<entrant>'",
                            unsigned(s.index.v), pr.issued ? 1 : 0,
                            (unsigned long long)pr.account, unsigned(s.desc.entrantJson.n),
                            pr.parseFailed ? 1 : 0, pr.jsonRejected ? 1 : 0, pr.parserUnbound ? 1 : 0,
                            pr.handlerUnbound ? 1 : 0, pr.reentrant ? 1 : 0, pr.receiverBad ? 1 : 0,
                            pr.faulted ? 1 : 0, meshPresent ? 1 : 0);
    }
}

// The multi-player-capable model-component install (DIAG-MODEL-COMPONENT-LEVER).  On
// the remote route the engine already allocates the entrant's actor, binds it and moves it from history;
// the ONE missing thing for a visible body is the net-model component on that actor.  Offline no peer ever
// delivers the entrant's R15NetPlayerEquipmentUpdate, so the model-change signal never reaches the engine.
// This drives that engine RECV itself, carrying the RECORDED chassis: sub_140D37300 -> sub_140CEC740
// (model-changed, which ENQUEUES a deferred callback) -> CModelSwapperCS -> mark-dirty -> the framework's
// own phase-16 rebuild installs the model.  Verified to stay mark-dirty/deferred and NEVER recurse into
// the forbidden flush sub_140323790 or the seat sub_14030F120.  Gated by env.equipmentRecv (default off);
// runs before the swapper sweeps in the same detour and self-stops once the CI seats.
void PawnProvisioner::EmulateEquipmentRecvSweep(void* ng) {
    namespace ia = replay::identity_access;
    if (ng == nullptr || env_.entity == nullptr) return;   // disarmed / not asked
    if (!env_.equipmentRecv) return;                        // default-safe: no RECV is fabricated

    const uint16_t slotCount = uint16_t(sizeof slots_ / sizeof slots_[0]);
    for (uint16_t r = 0; r < slotCount; ++r) {
        Slot& s = slots_[r];
        // OUR settled entrants only (the viewer is never admitted into slots_), and only while the
        // body is not yet dressed.
        if (s.index.v == kSlotUnmapped) continue;
        if (s.state != SlotState::Awaiting && s.state != SlotState::Provisioned) continue;
        if (s.modelResolved) continue;
        if (s.equipmentRecvIssued) continue;
        // A recorded spectator had no body in the recording: it is admitted and never dressed.
        if (PawnProvision::RecordedAsSpectatorRole(s.desc.team)) {
            s.equipmentRecvIssued = true;
            continue;
        }

        // The built-mesh signal (ComponentOnActor(model_.cs, actor)): present -> the CI seated, STOP.
        bool visible = false;
        const bool meshPresent = env_.entity->ModelVisible(s.index, visible);

        // The actor's swapper record: resolved+handleRead means the actor is allocated+enabled and its
        // per-actor record exists, so the equipment record the RECV writes exists too.  This is the same
        // readiness the sibling swapper sweeps wait for, and it is the right sequencing point for the RECV.
        ia::SwapperEntrantState sw;
        ia::ReadSwapperEntrant(ng, s.index.v, sw);
        const bool recordReady = sw.resolved && sw.handleRead;

        // The recorded chassis item id, matched by the "chassis" slot key (never by position -- applied
        // pairs are packed in the engine's enumeration order).  A recording that carries no chassis (no
        // slot keys, or the chassis slot was not worn) latches: it will never drive one.
        uint64_t chassis = 0;
        bool chassisKnown = false;
        bool chassisSlotUnfilled = false;   // F1: chassis slot present but its item is the sentinel
        const char* chassisWhy = "recordedLoadout unbound";
        if (env_.recordedLoadout != nullptr) {
            replay::identity::RecordedLoadout rl{};
            const char* rlWhy = nullptr;
            if (!env_.recordedLoadout(r, rl, &rlWhy)) {
                chassisWhy = rlWhy != nullptr ? rlWhy : "document not recorded";
            } else if (!rl.hasSlotKeys) {
                chassisWhy = "no slot keys recorded";
            } else {
                const uint64_t chassisSym = ia::LoadoutSlotSymbol("chassis");
                chassisWhy = chassisSym == 0 ? "chassis symbol unbound" : "chassis slot not worn";
                const uint16_t n = rl.filledPairCount > replay::identity::kRecordedLoadoutPairCap
                                       ? replay::identity::kRecordedLoadoutPairCap
                                       : rl.filledPairCount;
                for (uint16_t i = 0; chassisSym != 0 && i < n; ++i) {
                    if (rl.pairs[i].slotKey == chassisSym) {
                        // The chassis slot exists but its item is the engine's own unfilled-pair
                        // sentinel (0xFFFFFFFFFFFFFFFF) or an unworn 0: it names no model, so it is
                        // not driven. chassisKnown stays false, as for "chassis slot not worn".
                        if (rl.pairs[i].itemId == ~uint64_t(0) || rl.pairs[i].itemId == 0) {
                            chassisSlotUnfilled = true;
                            chassisWhy = "chassis slot unfilled (empty-slot sentinel)";
                            break;
                        }
                        chassis      = rl.pairs[i].itemId;
                        chassisKnown = true;
                        chassisWhy   = "ok";
                        break;
                    }
                }
            }
        }

        if (!ia::EquipmentRecvShouldFire(meshPresent, chassisKnown, recordReady,
                                         s.equipmentRecvIssued)) {
            // A recording that can never yield a chassis latches (so we stop re-reading it); a transient
            // not-ready (mesh pending + record not up, or chassis known but record not up) does not.
            // F1: an explicit unfilled chassis slot is itself definitive -- no chassis will ever come
            // from this recording -- so it latches without a re-probe.
            if (!chassisKnown && !meshPresent) {
                bool noChassisEver = chassisSlotUnfilled;
                if (!noChassisEver && env_.recordedLoadout != nullptr) {
                    replay::identity::RecordedLoadout probe{};
                    const char* pw = nullptr;
                    const bool got = env_.recordedLoadout(r, probe, &pw);
                    if (got && (!probe.hasSlotKeys || probe.filledPairCount == 0)) noChassisEver = true;
                }
                if (noChassisEver) {
                    s.equipmentRecvIssued = true;   // no chassis will ever come from this recording
                    s.chassisUnfilled = chassisSlotUnfilled;
                    if (env_.log != nullptr)
                        env_.log->Write(LogLevel::Info, "pawn",
                                        "model idx%u: equipment RECV not driven -- %s (no valid chassis "
                                        "in the recording); latched",
                                        unsigned(s.index.v), chassisWhy);
                }
            }
            continue;
        }

        const ia::EquipmentRecvOutcome er = ia::DriveEquipmentRecv(ng, s.index.v, chassis);
        // Latch on a definitive attempt: the handler ran (issued) or faulted, or the object was proven
        // wrong (netgameContradicted).  A transient refusal (component/entrant not up: !equipResolved,
        // !idResolved, handler unbound, re-entry) leaves it clear so a later tick re-asks.
        if (er.issued || er.faulted || er.netgameContradicted || er.chassisRejected)
            s.equipmentRecvIssued = true;
        if (env_.log != nullptr)
            env_.log->Write(LogLevel::Info, "pawn",
                            "model idx%u: equipment RECV (ENG-EQUIPMENT-RECV) issued=%d entityId=0x%08X "
                            "chassis=0x%016llX spaceResolved=%d equipResolved=%d netgameConfirmed=%d "
                            "netgameContradicted=%d idResolved=%d handlerBound=%d reentrant=%d faulted=%d "
                            "meshPresent=%d recordReady=%d -- engine installs the net-model component via "
                            "CModelSwapperCS; watch for [NETGAME] Swapping to model",
                            unsigned(s.index.v), er.issued ? 1 : 0, er.entityId,
                            (unsigned long long)er.chassis, er.spaceResolved ? 1 : 0,
                            er.equipResolved ? 1 : 0, er.netgameConfirmed ? 1 : 0,
                            er.netgameContradicted ? 1 : 0, er.idResolved ? 1 : 0,
                            er.handlerBound ? 1 : 0, er.reentrant ? 1 : 0, er.faulted ? 1 : 0,
                            meshPresent ? 1 : 0, recordReady ? 1 : 0);
    }
}

// ENG-SPAWNED-SEED backstop.  The reply seed writes `spawned` before the entrant add, so the
// engine's one role transition enables the actor itself; this covers an entrant that was added
// while the record still read 0.  It re-runs the engine's own participating dispatch and dress --
// NETGAME.USER_SETTINGS_APPLY's tail -- and moves no Team Slot, so the entrant-count guard the role
// bounce needs does not apply.  Once per entrant; the enable lands on a later frame and is re-read.
void PawnProvisioner::ParticipateBackstopSweep(void* ng) {
    if (ng == nullptr || !env_.participateBackstop || env_.hops == nullptr) return;
    const PawnProvision::ActorHops& h = *env_.hops;
    if (h.participatePair == nullptr || h.enabled == nullptr || h.placed == nullptr ||
        h.isLocalUser == nullptr)
        return;
    for (Slot& s : slots_) {
        if (s.index.v == kSlotUnmapped) continue;
        if (s.state != SlotState::Awaiting && s.state != SlotState::Provisioned) continue;
        if (!PawnProvision::RoleParticipates(s.desc.team)) continue;
        if (s.backstopIssued) {
            if (!s.backstopReadBack) {
                s.backstopReadBack = true;
                if (env_.log != nullptr)
                    env_.log->Write(LogLevel::Info, "pawn",
                                    "participate backstop idx%u: enabled=%d on the sweep after the pair",
                                    unsigned(s.index.v), h.enabled(h.ctx, s.index.v) ? 1 : 0);
            }
            continue;
        }
        // The participating arm's REMOTE block is the one with the `spawned` gate; a local or
        // unprovable index is not ours to drive.
        if (h.isLocalUser(h.ctx, s.index.v) != PawnProvision::LocalUserAnswer::Remote) continue;
        // The pair enables only when the engine's own reader answers true, so asking first is what
        // keeps the one issue from being spent on a call that cannot enable.
        if (!h.placed(h.ctx, s.index.v)) continue;
        if (h.enabled(h.ctx, s.index.v)) continue;

        const PawnProvision::EngineCallLatch latch;
        if (!latch.taken) return;
        const bool issued = h.participatePair(h.ctx, s.index.v);
        // Latched on the issue alone: a refusal (wrong phase, entry point moved) calls nothing and
        // is re-asked next frame, and an issued pair is never repeated.
        s.backstopIssued = issued;
        if (env_.log != nullptr && (issued || !s.backstopRefusalLogged))
            env_.log->Write(LogLevel::Info, "pawn",
                            "participate backstop idx%u: NETGAME.PARTICIPATING + dress issued=%d "
                            "role=%u", unsigned(s.index.v), issued ? 1 : 0, unsigned(s.desc.team));
        s.backstopRefusalLogged = !issued;
        if (issued) return;   // one engine call pair per frame
    }
}

void PawnProvisioner::UndressPooledSwapperSweep(void* ng) {
    namespace ia = replay::identity_access;
    if (ng == nullptr || env_.entity == nullptr) return;   // disarmed / not asked
    for (Slot& s : slots_) {
        // OUR settled entrants only (the viewer is never admitted into slots_), and only while the
        // body is not yet dressed.  An idle/unsettled slot has no user index to reset.
        if (s.index.v == kSlotUnmapped) continue;
        if (s.state != SlotState::Awaiting && s.state != SlotState::Provisioned) continue;
        if (s.modelResolved) continue;
        if (!PawnProvision::SwapDriveAllowed(s.desc.team, s.chassisUnfilled)) continue;

        // The built-mesh signal (ComponentOnActor(model_.cs, actor)): present -> the body landed, STOP.
        bool visible = false;
        const bool meshPresent = env_.entity->ModelVisible(s.index, visible);

        ia::SwapperEntrantState sw;
        ia::ReadSwapperEntrant(ng, s.index.v, sw);

        // Capture the pooled target hash while current is still a real stale value: under the from==to
        // no-op this is exactly the worn model the engine's own apply targets (0xDAC-family), and it
        // survives the -1 the reset writes next.  The BeginModelSwap re-fire below needs it once current
        // reads -1.  Logged once, on the transition to known, as the falsifiable target signal.
        if (sw.currentRead && sw.currentModel != ~uint64_t(0) && sw.currentModel != 0) {
            if (!s.swapTargetKnown && env_.log != nullptr)
                env_.log->Write(LogLevel::Info, "pawn",
                                "model idx%u: swap target captured (ENG-DRESS-LEVERC) target=0x%016llX",
                                unsigned(s.index.v), (unsigned long long)sw.currentModel);
            s.swapTargetHash  = sw.currentModel;
            s.swapTargetKnown = true;
        }

        // PHASE 1 -- reset.  Fire only on the poisoned residual state (mesh absent, a real stale
        // current-model hash, nothing already enqueued) -- the orchestrator's gate.  Once we write -1
        // (or the engine builds the mesh) this predicate is false, so the reset fires and stops.
        if (ia::SwapperResetShouldFire(meshPresent, sw)) {
            const ia::SwapperResetOutcome rst = ia::ResetSwapperCurrent(ng, s.index.v);
            if (env_.log != nullptr)
                env_.log->Write(LogLevel::Info, "pawn",
                                "model idx%u: pooled swapper current reset (ENG-DRESS-LEVERC, pre-apply) "
                                "resolved=%s located=%s wrote=%d faulted=%d before=0x%016llX "
                                "after=0x%016llX pending=%llu meshPresent=%d",
                                unsigned(s.index.v), rst.resolved ? "yes" : "no",
                                rst.located ? "yes" : "no", rst.wrote ? 1 : 0, rst.faulted ? 1 : 0,
                                (unsigned long long)rst.before, (unsigned long long)rst.after,
                                (unsigned long long)sw.pending, meshPresent ? 1 : 0);
            continue;   // one action per slot per tick; current reads -1 only from the NEXT tick's read
        }

        // PHASE 2 -- enqueue.  The reset forced current to -1 on an earlier tick and it PERSISTED (the
        // engine's synchronous enable-time apply already ran and no-op'd; nothing re-issues the base
        // swap).  So enqueue it ourselves: call the engine's OWN BeginModelSwap once with the captured
        // target.  current(-1) != target takes the CI-building enqueue arm; the ungated SwapperFlushTick
        // pump drains it and the framework's phase-16 seats the InstanceModel CI (never us).
        // BeginSwapShouldFire holds this to a single fire, mesh-absent, pending==0, current==-1,
        // target-known -- so it never fires in the reset tick (current not yet observably -1) and never
        // fights an in-flight build.  pendingBefore->pendingAfter is the enqueue witness the launch reads
        // to tell a real build (0 -> nonzero) from a re-skin of an absent CI; meshPresent is the pre-fire
        // CI state (the post-fire modelCI handle lands later, on the drivegate/cadence line).
        if (ia::BeginSwapShouldFire(meshPresent, sw, s.swapTargetKnown, s.beginSwapIssued)) {
            const ia::BeginSwapOutcome bs = ia::TriggerBeginModelSwap(ng, s.index.v, s.swapTargetHash);
            if (bs.issued || bs.faulted) s.beginSwapIssued = true;   // an attempt ran -> never repeat
            if (env_.log != nullptr)
                env_.log->Write(LogLevel::Info, "pawn",
                                "model idx%u: direct BeginModelSwap (ENG-DRESS-LEVERC) issued=%d "
                                "target=0x%016llX resolved=%s located=%s faulted=%d reentrant=%d "
                                "pendingBefore=%llu pendingAfter=%llu meshPresent=%d",
                                unsigned(s.index.v), bs.issued ? 1 : 0,
                                (unsigned long long)s.swapTargetHash, bs.resolved ? "yes" : "no",
                                bs.located ? "yes" : "no", bs.faulted ? 1 : 0, bs.reentrant ? 1 : 0,
                                (unsigned long long)bs.pendingBefore,
                                (unsigned long long)bs.pendingAfter, meshPresent ? 1 : 0);
        }
    }
}

namespace {
void SwapperResetSweepThunk(void* ctx, void* ng) {
    PawnProvisioner* const p = static_cast<PawnProvisioner*>(ctx);
    // The detour hands over every CR15NetGame::Update. Slots are only ever provisioned in match, so
    // out of it these sweeps have work exactly when the session is ending under a loaded replay --
    // and each of them calls into the engine.
    if (PawnProvision::SessionBlocker(ng) != PawnProvision::Refusal::None) return;
    // ENG-DRESS-LEVERC is the live fix: the ungated pooled-swapper dress sequence (reset current->-1,
    // then a LATER-tick direct BeginModelSwap with the captured target) runs every tick from the stage-0
    // detour.  SwapperResetSweep (the old machinery) stays wired but is gated OFF by stageb.swapper_hook
    // (default false) -> its body early-returns, so it is a no-op here; kept, not deleted, so re-arming
    // remains a one-config-flip diagnosis lever.  Note: LEVERC now issues BeginModelSwap itself.  The
    // per-slot beginSwapIssued latch is SHARED with SwapperResetSweep, so even under a deliberate re-arm
    // (stageb.swapper_hook=true) at most one BeginModelSwap fires per entrant -- whichever sweep reaches
    // it first sets the latch and the other's arm self-refuses.  A re-arm is the orchestrator's call.
    // ENG-DRESS-PROFILE is the primary dress lever and runs FIRST: emulate the engine's own
    // Profile-received handler so the engine requests the model resource and seats the InstanceModel
    // CI itself.  The swapper sweeps below stay wired as the fallback (LEVERC) and self-stop the
    // moment the CI seats (ModelVisible), so they never fight a body this lever has already dressed.
    p->ParticipateBackstopSweep(ng);
    p->EmulateProfileReceivedSweep(ng);
    // The multi-player-capable model-component install on the remote route.  Gated by
    // env.equipmentRecv (default off), so this is a no-op unless the launch arms it; when on, it installs
    // the net-model component by driving the engine's own equipment RECV, and self-stops once the CI seats.
    p->EmulateEquipmentRecvSweep(ng);
    p->UndressPooledSwapperSweep(ng);
    p->SwapperResetSweep(ng);
}
}  // namespace

bool PawnProvisioner::AdvanceSlot(UserSlot slot, Slot& s) {
    namespace ia = replay::identity_access;
#ifdef W2P1_MUTATE_LOADOUT_DRIVEN_BEFORE_HOP2
    // The defect: dressed before hop 2's re-read says the actor is enabled at all.
    if (!s.loadoutAttempted && env_.recordedLoadout != nullptr)
        DriveLoadout(slot, s, ia::LoadoutPhase::All);
#endif

    // ★ THE PUBLISH HALF RUNS BEFORE THE HOP CHAIN, because the hop chain is what issues the
    // entrant's ENABLE and the engine drives its OWN loadout apply off that enable
    // (DIAG-MODEL-SWAP).  RUN-33: the enable fired at 05:25:56 and refused -- "User has 0
    // loadout instances" -- and our publish landed 1.2 s later, by which time the event was
    // spent and nothing re-ran it, so the entrant wore nothing for the match.  L1..L4 need
    // no actor: they read the recording and write the per-user container, both of which are
    // there the moment the entrant is admitted.  L5..L6 still run after, below.
#ifndef W2P1_MUTATE_ENABLE_BEFORE_PUBLISH
    // Re-asked while its refusal is one the ENGINE can clear on a later window; every
    // other word L1..L4 can name is a property of the recording, reads the same on every
    // window of this load, and waiting on it would cost the entrant the windows its pawn
    // is not placed in and buy it nothing.
    const bool publishRetryable = s.loadoutPublishAttempted &&
                                  PublishHalfRetryable(s.loadoutPublishWhy) &&
                                  s.loadoutPublishTries < kPublishRetryLimit;
    if (env_.recordedLoadout != nullptr && (!s.loadoutPublishAttempted || publishRetryable)) {
        if (s.loadoutPublishAttempted) ++s.loadoutPublishTries;
        DriveLoadout(slot, s, ia::LoadoutPhase::Publish);
        s.loadoutPublishWhy = s.loadoutWhy;
    }
    // WHAT THIS WINDOW LEFT, asked after the re-ask and not before it, so the budget is spent
    // and reported in the same window the last re-ask failed in.  A hold that outlived the
    // re-asks would park the entrant on a publish nobody is going to drive again.
    const bool publishStillClearable =
        s.loadoutPublishAttempted && PublishHalfRetryable(s.loadoutPublishWhy);
    if (publishStillClearable && s.loadoutPublishTries >= kPublishRetryLimit &&
        !s.loadoutPublishExhausted) {
        // THE BUDGET SAYS SO BY NAME.  A re-ask that simply stops leaves a census line
        // identical to one that was never retryable, and the two need different fixes.
        s.loadoutPublishExhausted = true;
        if (env_.log != nullptr)
            env_.log->Write(LogLevel::Warn, "pawn",
                            "recorded slot %u -> slot %u: index %u loadout publish retries "
                            "exhausted after %u re-asks -- %s, the enable goes out undressed",
                            slot.v, s.mapped, s.index.v, unsigned(s.loadoutPublishTries),
                            ia::LoadoutRefusalName(s.loadoutPublishWhy));
    }
    // AUD-W2-P1-34 P1-40 M2.  THE SUCCESS PATH NEEDS THIS READING MOST.  Keyed on
    // `publishStillClearable' alone, the independent re-read ran only for a publish that had
    // already refused with a word this pass can clear -- so the case it exists for, a publish
    // that RETURNED None and still left the engine's own count at zero, was the one case it
    // never looked at, and that enable went out over `Loadouts : 0' unheld.  A publish that
    // refused for a property of the RECORDING is still excluded: no window clears it, and
    // holding the entrant would cost it its pawn for a count that is never going to read back.
    const bool publishTook =
        s.loadoutPublishAttempted && s.loadoutPublishWhy == ia::LoadoutRefusal::None;
    if ((publishStillClearable || publishTook) && !s.loadoutPublishExhausted) {
        // THE SECOND READ, and it is the engine's own count and not our publish's return:
        // the number the entrant dump prints as `Loadouts : N` is what the apply the enable
        // drives will walk.  A readable ZERO holds the enable; UNREADABLE does not, because
        // a count nobody could ask for is not a count of nothing and parking every build
        // with no netgame surface is not what this gate is for.
        void* const ng = env_.netGame != nullptr ? env_.netGame() : nullptr;
        if (ia::LiveLoadoutInstances(ng, UserSlot(s.mapped)) == 0) {
            if (!s.publishHeldPumpKnown) {
                s.publishHeldPump = pumps_;
                s.publishHeldPumpKnown = true;
            }
            // BOUNDED.  Held forever, a publish that can never take would cost the entrant
            // its pawn as well as its clothes, and a placed pawn wearing nothing is the
            // thing every other part of the replay is hung on.
            if (pumps_ - s.publishHeldPump < kPublishGraceWindows)
                return Refuse(slot,
                              PawnProvision::Refusal::LoadoutNotPublishedBeforeEnable);
        }
    }

    // THE OTHER THREE SILENT SKIPS.  `Loadouts : N` non-zero only clears the instance SEARCH;
    // IDENT.APPLY_LOADOUT_ITEMS closes four more gates between its `Applying loadout items`
    // print and its item loop, and it prints nothing on any of them -- RUN-34's entrant reached
    // that print and stopped there.  The readings were taken one instruction after the publish;
    // an enable issued over a shut gate is spent, so the window is held instead.
    if (s.loadoutPublishAttempted && !s.loadoutPublishExhausted) {
        // RE-READ EVERY WINDOW, through the same surface the publish half read it through.
        // Three of the four gates are the actor side coming up and clear on their own -- and
        // the publish is not re-run once it has taken, so a verdict latched at publish time
        // could never clear and the hold would run its whole budget out against a stale word.
        ia::LoadoutEngine eng{};
        const bool bound = env_.loadoutEngine != nullptr
                               ? env_.loadoutEngine(eng)
                               : (env_.hops != nullptr && ia::BindLoadoutEngine(eng, *env_.hops));
        if (bound && eng.applyGates != nullptr) {
            void* const gateNg = env_.netGame != nullptr ? env_.netGame() : nullptr;
            ia::LoadoutApplyGates g{};
            const ia::LoadoutApplyGate reread = eng.applyGates(gateNg, s.mapped, s.index.v, g);
            s.loadoutGateVerdict = PawnProvision::MergeGateReread(s.loadoutGateVerdict, reread);
        }
        // UNASKED IS NOT A NO.  An unbound predicate, a build with no netgame surface or an
        // instance name that did not resolve leave the verdict Unasked, and holding every such
        // entrant would cost it its pawn for a question nobody managed to put.
        const PawnProvision::Refusal gateWhy =
            s.loadoutGateVerdict == ia::LoadoutApplyGate::InstanceEmpty
                ? PawnProvision::Refusal::LoadoutInstanceEmpty
            : s.loadoutGateVerdict == ia::LoadoutApplyGate::ActorUnresolved
                ? PawnProvision::Refusal::LoadoutActorUnresolved
            : s.loadoutGateVerdict == ia::LoadoutApplyGate::CsPointerNull
                ? PawnProvision::Refusal::LoadoutCsPointerNull
            : s.loadoutGateVerdict == ia::LoadoutApplyGate::CompKeyInvalid
                ? PawnProvision::Refusal::LoadoutCompKeyInvalid
                : PawnProvision::Refusal::None;
#ifdef W2P1_MUTATE_ENABLE_WITH_EMPTY_INSTANCE
        // The defect: the four gates are read and the enable goes out over them anyway.
        const bool holdOnGate = false;
#else
        const bool holdOnGate = true;
#endif
        if (holdOnGate && gateWhy != PawnProvision::Refusal::None) {
            if (!s.publishHeldPumpKnown) {
                s.publishHeldPump = pumps_;
                s.publishHeldPumpKnown = true;
            }
            // THE SAME BOUND the instance-count hold uses, and deliberately the same budget:
            // three of the four gates are the actor side coming up, which the next windows fix
            // or never will, and a pawn held forever is worse than one dressed late.
            if (pumps_ - s.publishHeldPump < kPublishGraceWindows) return Refuse(slot, gateWhy);
        }
    }
#endif
#ifdef W2P1_MUTATE_DRIVE_BEFORE_ENABLE
    // THE DEFECT (DRIVE_BEFORE_ENABLE): drive the model before the entrant is enabled, on the
    // pooled shell's stale record -- current is whatever the pool held, so the swap is a no-op or
    // targets the wrong record.  The shipped drive is sequenced after the hop chain reads done.
    (void)DriveEntrantModel(env_.netGame != nullptr ? env_.netGame() : nullptr, slot, s);
#endif
#ifndef W2P1_MUTATE_NO_HOP_CHAIN
    s.driveGate.epoch = epoch_;
    PawnProvision::HopDrive drive;
    drive.gate = &s.driveGate;
    // The role this player was RECORDED with, straight off the descriptor authored from the
    // PlayerRecord.  Never the live word: the live word is what A3 compares against.
    drive.recordedRole = s.desc.team;
    // How many recorded players this load handed us, which is what the last-resort role bounce is
    // gated on: IDENT.SERVER_ASSIGN_TEAM reallocates the entrant's Team Slot off a team count the
    // away leg has just depressed, so the leg home returns the entrant to its own slot only when
    // it was the sole member.  `requested` is that population and nothing narrower -- a count that
    // only saw the slots we got as far as would read 1 on a 4v4 and permit exactly the case the
    // refusal exists to stop.
    drive.entrantCount = ledger_.requested;
    const PawnProvision::Hop hop =
        env_.hops != nullptr
            ? PawnProvision::AdvanceActorHops(*env_.hops, s.index.v, &drive)
            : PawnProvision::Hop::Done;
    // An Issued is not lost to a later pass: once this session has authored the transition, the
    // gate makes every subsequent pass a skip, and the skip must not overwrite the record of it.
    if (s.participate.state != PawnProvision::Participation::Issued) s.participate = drive.outcome;
    // The pump A3 was issued on, latched once.  It is the dress's gate and nothing else reads it.
    if (s.participate.state == PawnProvision::Participation::Issued && !s.participatePumpKnown) {
        s.participatePump = pumps_;
        s.participatePumpKnown = true;
    }
    // The state the chain DECIDED, not a second reading of the same pool.
    s.ciWhy = env_.hops != nullptr ? drive.physicsCiWhy : PawnProvision::PhysicsCiReq::Unknown;
#else
    const PawnProvision::Hop hop = PawnProvision::Hop::Done;
#endif
    s.hop = hop;
    for (uint32_t i = 0; i < ledger_.mapped; ++i)
        if (ledger_.map[i].slot_recorded == slot.v) ledger_.map[i].hop = uint8_t(hop);

    if (hop != PawnProvision::Hop::Done) {
        // Hop 4 alone can answer three ways -- a wait, an unreadable CI and an index past the
        // entrant table -- and the census must not read the last two as progress.
        const PawnProvision::Refusal hopWhy = hop == PawnProvision::Hop::PhysicsCi
                                                  ? PawnProvision::PhysicsCiRefusal(s.ciWhy)
                                                  : PawnProvision::HopRefusal(hop);
        // One line per slot per cadence window, for as long as the hop is outstanding.  This is
        // the ledger that L3 #23 F4 watched stop at the state-9 edge: it now cannot stop, because
        // the thing that prints it is the re-read itself and the re-read has no terminal state
        // short of the hop reading done.
        if (env_.log) {
            // Hop 4 carries the engine's own reason word, because "NoPhysicsCi" alone cannot tell
            // a CI the engine refused to build from one it was never asked to build.
            char why[64] = {0};
            if (hop == PawnProvision::Hop::PhysicsCi) {
                snprintf(why, sizeof(why), " PhysicsCiAwaiting(%s)",
                         PawnProvision::PhysicsCiReqName(s.ciWhy));
            }
            env_.log->Write(LogLevel::Info, "pawn",
                            "recorded slot %u -> slot %u: index %u is awaiting hop '%s'%s -- %s",
                            slot.v, s.mapped, s.index.v, PawnProvision::HopName(hop), why,
                            PawnProvision::RefusalText(hopWhy));
        }
        return Refuse(slot, hopWhy);
    }

#ifndef W2P1_MUTATE_COUNT_SKIPS_AS_PROVISIONED
    // S14.7's exit criterion, as a gate rather than a paragraph.  Every hop reads done; if this
    // session did not ISSUE the participation that authored that state, the pawn is not ours to
    // claim and `provisioned` must not move.  A skip and a refusal are both this branch.
    if (env_.hops != nullptr &&
        s.participate.state != PawnProvision::Participation::Issued) {
#ifndef W2P1_MUTATE_PARTNOTISSUED_LATCHES_UNDER_PENDING
        // A re-seat settle is in flight.  hop 2's enable re-read reading done means the
        // actor is ON -- the very enable the P1-47 latch books Issued on the NEXT window's top read
        // (Participate 1263-1275).  Latching ParticipationNotIssued here (below) would kill the slot
        // Failed in the one window between the bounce and that booking, and Pump would never pump it
        // again -- the latch could neither reach Issued nor spend its budget to name an exhaustion.
        // So DEFER, non-latching, and let the latch resolve.  Bounded: hop==Done implies actor ON,
        // so the next Participate books Issued; the actorOn==false regression is bounded by the
        // settle budget (Participate 1298-1311), which then returns a named exhaustion.
        if (s.driveGate.reSeatPending) {
            if (env_.log) {
                env_.log->Write(LogLevel::Info, "pawn",
                                "recorded slot %u -> slot %u: index %u reads done and a re-seat "
                                "settle is pending (%s) -- awaiting the booking (ParticipationSettling)",
                                slot.v, s.mapped, s.index.v,
                                PawnProvision::ParticipateRefusalName(s.participate.why));
            }
            return Refuse(slot, PawnProvision::Refusal::ParticipationSettling);
        }
#endif
        if (env_.log) {
            env_.log->Write(LogLevel::Info, "pawn",
                            "recorded slot %u -> slot %u: index %u reads done and this session "
                            "issued no participation for it (%s) -- not claimed",
                            slot.v, s.mapped, s.index.v,
                            PawnProvision::ParticipateRefusalName(s.participate.why));
        }
        return Refuse(slot, PawnProvision::Refusal::ParticipationNotIssued);
    }
#endif

    // THE APPLY HALF.  Every hop reads done -- hop 2's own re-read among them -- and the
    // participation this session issued authored that state, so this is the first moment
    // L5's precondition can be true.  L1..L4 already ran, above, before the enable; this
    // half re-reads the live count, re-verifies the recording and scores the twenty against
    // what the engine holds.  It is asked ONCE: a dress that issued is not re-issued, and a
    // recording that cannot support one is not retried all match.
    if (!s.loadoutAttempted && env_.recordedLoadout != nullptr) {
#ifndef W2P1_MUTATE_LOADOUT_ATTEMPT_UNGATED
        // S14.6 condition 2: never in the frame that issued A3.  The slot
        // stays Awaiting for exactly one cadence window and is asked again.
        if (s.participatePumpKnown && pumps_ <= s.participatePump)
            return Refuse(slot, PawnProvision::Refusal::LoadoutSameFrameAsParticipation);
#endif
        DriveLoadout(slot, s, ia::LoadoutPhase::Apply);
    }

    // DIAG-MODEL-BUILD fix (b) -- THE BODY.  Reached only here: every hop reads done, so the
    // entrant is ENABLED and its loadout has been applied (the chassis id is known).  This is the
    // first window the model drive's precondition can hold, and driving before the enable would
    // swap on the pooled shell's stale record.  Re-asked every window because the mesh load is
    // async.  A model that never instantiates does not un-provision the pawn -- like the loadout,
    // it is a named fact the census carries beside a placed, driveable pawn.
#ifndef W2P1_MUTATE_DRIVE_BEFORE_ENABLE
    // HOLD the slot Awaiting while the async body loads, bounded; it is re-polled next window.
    if (DriveEntrantModel(env_.netGame != nullptr ? env_.netGame() : nullptr, slot, s))
        return Refuse(slot, PawnProvision::Refusal::ModelNotInstantiated);
#endif

    // A loadout that could not be authored does NOT un-provision the pawn: the pawn is placed,
    // driveable and this session's, and `provisioned` is the count of those.  Why it is wearing
    // nothing is a separate fact and the census line carries it by name.
    s.state = SlotState::Provisioned;
    s.why = PawnProvision::Refusal::None;
    ++ledger_.provisioned;
    last_ = PawnProvision::Refusal::None;
    ReopenAttemptWindow();
    return true;
}

// The three participation numbers are counted OFF THE SLOTS, not accumulated as attempts happen:
// a slot has exactly one outcome at any moment, so a count taken here cannot double-count a
// cadence window and cannot let an intention leak into the total `provisioned` is claimed off.
PawnLedger PawnProvisioner::Ledger() const {
    PawnLedger out = ledger_;
    out.participateIssued = 0;
    out.participateSkipped = 0;
    out.participateRefused = 0;
    for (size_t i = 0; i < PawnProvision::kParticipateRefusalCount; ++i)
        out.participateRefusals[i] = 0;
    for (uint16_t i = 0; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i) {
        const Slot& s = slots_[i];
        if (s.state == SlotState::Idle) continue;
        switch (s.participate.state) {
            case PawnProvision::Participation::Issued: ++out.participateIssued; break;
            case PawnProvision::Participation::SkippedAlreadyDone:
                ++out.participateSkipped;
                break;
            case PawnProvision::Participation::Refused:
                ++out.participateRefused;
                ++out.participateRefusals[size_t(s.participate.why)];
                break;
        }
    }
    return out;
}

bool PawnProvisioner::Admit(UserSlot recorded, const JoinDesc& desc, UserSlot& slotOut,
                            UserIndex& indexOut) {
    slotOut  = UserSlot(kSlotUnmapped);
    indexOut = UserIndex(kSlotUnmapped);
    if (!Armed()) {
        last_ = PawnProvision::Refusal::NotAttempted;
        return false;
    }
    if (recorded.v >= facts::ENTITY_USER_SLOT_TABLE_MAX)
        return Refuse(recorded, PawnProvision::Refusal::SlotOutOfRange);

    Slot& s = slots_[recorded.v];
    // ALREADY ADMITTED, IN EITHER STATE THAT MEANS IT.  Awaiting is a statement about the PAWN hop
    // chain -- no actor yet -- and not about the identity: the entrant exists and the engine has
    // issued its index, which is everything a dress needs.  Keyed on Provisioned alone, a slot that
    // settles only to Awaiting re-entered Provision on every pass and was answered a deferral for
    // the life of the session, so it was never dressed and never reported by name.
    const SlotState wasState = s.state;
    if (wasState == SlotState::Provisioned || wasState == SlotState::Awaiting) {
        // Asked of the engine, never remembered.  The mapped SLOT is ours and survives a re-bind;
        // the index was issued by a game space that may be gone.
        const UserIndex live = env_.identity->IndexOf(UserSlot(s.mapped));
        // The same cross-check the re-read makes, for the same reason: this is the value the DRESS
        // PASS is handed, and an unwritten side table answers a legal index for a slot that holds
        // nobody.  The entrant must name the slot back, and it must not be one that was in the
        // table before this provisioner asked.
        const bool oursByRecord =
            live.v != kSlotUnmapped && env_.identity->SlotOf(live).v == s.mapped &&
            !ForeignEntrant(live);
        if (oursByRecord) {
            s.index  = live;
            slotOut  = UserSlot(s.mapped);
            indexOut = live;
            last_    = PawnProvision::Refusal::None;
            return true;
        }
        // The entrant this load admitted is gone: re-admit into the slot already assigned, never
        // through a second assignment, which would seat one recorded player twice.
        s.state = SlotState::Pending;
        // Only the Provisioned state was ever counted here; decrementing for an Awaiting slot
        // would spend somebody else's entry.
        if (wasState == SlotState::Provisioned && ledger_.provisioned) --ledger_.provisioned;
    }
    if (!Provision(recorded, desc)) return false;
    slotOut  = UserSlot(s.mapped);
    indexOut = s.index;
    return true;
}

// The cadence's decision, made ONCE per pump and cached for it, so every slot in one Pump pass
// shares one window and a 12-player file is not spread across 12 seconds.  `blocker` is the word
// the caller already read, so the decision costs no engine read of its own.
bool PawnProvisioner::AttemptDue(PawnProvision::Refusal blocker) {
    if (window_pump_ == pumps_) return window_open_;
    window_pump_ = pumps_;
    if (blocker != blocker_) {
        // The state word moved: whatever the last attempt learned is about a session that is gone.
        blocker_ = blocker;
        window_open_ = true;
    } else {
        window_open_ = pumps_ >= next_attempt_;
    }
    if (window_open_) {
        next_attempt_ = pumps_ + kRetryTicks;
        ++attempts_;
    } else {
        ++suppressed_;
    }
    return window_open_;
}

void PawnProvisioner::ReopenAttemptWindow() {
    window_pump_ = pumps_;
    window_open_ = true;
    // DUE NOW, not one cadence period from now: while entrants keep landing the pass is making
    // progress, and a 12-player file must not be spread across twelve seconds.  The cadence closes
    // again on the first pump that spends an attempt and provisions nobody.
    next_attempt_ = pumps_;
}

namespace {
// Defined below beside the per-slot census it shares its words with: ONE formatter, so the
// window line and the census line can never disagree about what the model gate answered.
void ModelClauseText(const IEntity* e, void* netGame, const uint16_t* live,
                     const PawnProvision::Refusal* whys, uint16_t n, char* out, size_t cap);
}  // namespace

uint32_t PawnProvisioner::Pump() {
    // One tick, one marker budget.  Pump is the tick: TryDressRoster
    // runs before it in the same frame and shares its pump number.
    PawnProvision::ResetHopMarkerBudget();
    uint32_t done = 0;
    uint16_t awaiting = 0;
    for (uint16_t i = 0; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i) {
        const SlotState st = slots_[i].state;
        if (st != SlotState::Pending && st != SlotState::Awaiting) continue;
        // The per-frame budget bounds ADMISSIONS -- the inserts and the lobby walks they need.  An
        // awaiting slot makes no insert and walks no lobby: it re-reads four engine records at the
        // one-second cadence, which is what a budget would otherwise stop it doing on the very
        // frames a 12-player file needs it most.
        if (st == SlotState::Pending && done >= env_.perFrameMax) {
            Refuse(UserSlot(i), PawnProvision::Refusal::BudgetExceeded);
            continue;
        }
        if (Provision(UserSlot(i), slots_[i].desc)) ++done;
    }

    // The state of the whole table AFTER the pass, counted once and read by both lines below.
    uint16_t pending = 0, provisioned = 0, failed = 0;
    PawnProvision::PhysicsCiReq ciSeen = PawnProvision::PhysicsCiReq::Unknown;
    bool ciAsked = false, ciMixed = false;
    for (uint16_t i = 0; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i) {
        switch (slots_[i].state) {
            case SlotState::Pending:     ++pending; break;
            case SlotState::Awaiting:    ++awaiting; break;
            case SlotState::Provisioned: ++provisioned; break;
            case SlotState::Failed:      ++failed; break;
            default: break;
        }
        // WHAT HOP 4 ANSWERED, in one word, over the slots that were actually asked.  A slot the
        // chain carried to Done answers `none` -- no requirement outstanding -- because that is
        // the reading that let it through; a slot still on hop 4 answers its own requirement word.
        // `unasked` is NOT a synonym for built and prints as itself, and two slots disagreeing
        // print `mixed` rather than one of them standing in for the table.
        const bool asked = slots_[i].state == SlotState::Awaiting ||
                           slots_[i].state == SlotState::Provisioned;
        if (asked) {
            const PawnProvision::PhysicsCiReq r =
                slots_[i].hop == PawnProvision::Hop::Done ? PawnProvision::PhysicsCiReq::None
                                                          : slots_[i].ciWhy;
            if (!ciAsked) { ciSeen = r; ciAsked = true; }
            else if (r != ciSeen) { ciMixed = true; }
        }
    }
    const char* const ciState = !ciAsked ? "unasked"
                                : ciMixed ? "mixed"
                                          : PawnProvision::PhysicsCiReqName(ciSeen);

    // THE WINDOW LINE, AND IT IS THE PUMP THAT PRINTS IT (L3 #24 F2).  The ledger a run reads was
    // printed by the DRESS pass, which runs once per load and then never again -- so the last
    // [pawn] line of a four-minute session was stamped at the state-9 edge, the millisecond the
    // thing worth watching appeared, and the two runs that were told the ledger "cannot stop" both
    // watched it stop there.  This one is on the pump, on the same one-second cadence every engine
    // read here is bounded by, and its condition is that the table still has work on it -- NOT
    // that a slot reached some particular state.  A slot stuck in Pending is exactly the case the
    // line exists to report, and it is the case the old gate could not print.
    // This block and its hop census were 4.7 MB of the auditor's 17.28 MB log
    // -- six prints a second of a line that changes a few times a minute. The pump cadence still
    // decides WHETHER there is something to report; `log.chatter_period_ms` decides how often the
    // report is written down. `next_status_` advances only on a pass that actually prints, so a
    // suppressed pass leaves the pump ready rather than skipping a whole status window.
    const bool window = pumps_ >= next_status_ && replay::log::ChatterDue(statusChatterAt_);
    if (window) next_status_ = pumps_ + kRetryTicks;
    // ★ AND THERE IS NO WORK PREDICATE LEFT ON IT (L3 #26 F1).  Three gates have now been tried
    // here -- `awaiting` (L3 #24), `pending || awaiting` (L3 #25) and the request count added for
    // #26 -- and every one of them is FALSE over the state an armed pump spends its first seconds
    // in: a table nothing has been asked of yet.  L3 #26 spent a whole run there.  `[pawn] window`
    // printed zero times, and the run could not separate "the pump is not running" from "the pump
    // ran over an empty table", so the launch answered nothing about the one subsystem it was
    // flown for.  The COUNTS in the text report the work; the LINE reports the pump.  A pump that
    // is running says so every cadence window, `pending 0, awaiting 0 ... requested 0` included.
#ifdef W2P1_MUTATE_WINDOW_GATE_NEEDS_WORK
    if (window && env_.log != nullptr && (pending || awaiting || ledger_.requested != 0)) {
#else
    if (window && env_.log != nullptr) {
#endif
        env_.log->Write(LogLevel::Info, "pawn",
                        "window %llu: pending %u, awaiting %u, provisioned %u, failed %u -- "
                        "requested %u, deferred %u, attempts %u, suppressed %u, physicsCi=%s, "
                        "blocker '%s', shutter '%s', last '%s'",
                        (unsigned long long)pumps_, unsigned(pending), unsigned(awaiting),
                        unsigned(provisioned), unsigned(failed), ledger_.requested,
                        ledger_.deferred, attempts_, suppressed_, ciState,
                        PawnProvision::RefusalName(blocker_),
                        // The SHUTTER: what the session would refuse a dress pass with RIGHT NOW,
                        // asked of the engine on this line rather than remembered.  `blocker` is
                        // the word the cadence is keyed on and reads NotAttempted while no pass
                        // has run -- which is an initial value, not a reason, and a run that read
                        // it as one is what this field exists to prevent.
                        PawnProvision::RefusalName(PawnProvision::SessionBlocker(
                            env_.netGame != nullptr ? env_.netGame() : nullptr)),
                        PawnProvision::RefusalName(last_));

        // The hop census, gated on the slots PAST Pending and not on Awaiting alone: a build whose
        // hop surface is unwired settles at the admission and goes straight to Provisioned, and it
        // must still print the reading it got rather than looking like a build with no entrant.
        // `hops=unasked` is what an unwired chain prints, because a build that bound no hop read
        // must not look like a build whose hops all answered done.
        if (awaiting + provisioned) {
            // Derived from the tables that render the entry, not transcribed from one example:
            // a census that drops slots silently is a could-not-ask printed as an answer, and the
            // longest entry stops being the longest the moment a refusal name is added.
            constexpr size_t kIssuedMax = PawnProvision::CLen(":issued(epoch ,role ,)") + 10 + 5 +
                                          PawnProvision::MaxNameLen(PawnProvision::kParticipateDriveNames,
                                                                    sizeof(PawnProvision::kParticipateDriveNames) / sizeof(PawnProvision::kParticipateDriveNames[0]));
            constexpr size_t kRefusedMax =
                PawnProvision::CLen(":refused()") + PawnProvision::MaxNameLen(PawnProvision::kParticipateRefusalNames, PawnProvision::kParticipateRefusalCount);
            constexpr size_t kAwaitingMax =
                PawnProvision::CLen(":awaiting()") +
                PawnProvision::MaxNameLen(PawnProvision::kPhysicsCiReqNames,
                           sizeof(PawnProvision::kPhysicsCiReqNames) / sizeof(PawnProvision::kPhysicsCiReqNames[0]));
            constexpr size_t kReadingMax =
                kIssuedMax > kRefusedMax
                    ? (kIssuedMax > kAwaitingMax ? kIssuedMax : kAwaitingMax)
                    : (kRefusedMax > kAwaitingMax ? kRefusedMax : kAwaitingMax);
            constexpr size_t kEntryMax = PawnProvision::CLen("15->15=") + PawnProvision::MaxNameLen(PawnProvision::kHopNames, PawnProvision::kHopCount) +
                                         kReadingMax + PawnProvision::CLen(",");
            static_assert(kEntryMax >= sizeof("15->15=RemotePlayerCs:awaiting(R15TouchInteract),") - 1,
                          "the derived bound must not be narrower than the transcribed one it "
                          "replaces");
            static_assert(kReadingMax < sizeof(char[96]),
                          "every reading is rendered through a 96-byte `why` buffer first");
            char hops[facts::ENTITY_USER_SLOT_TABLE_MAX * kEntryMax + 1];
            size_t n = 0;
            uint16_t dropped = 0;
            for (uint16_t i = 0; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i) {
                if (slots_[i].state != SlotState::Awaiting &&
                    slots_[i].state != SlotState::Provisioned) {
                    continue;
                }
                // recorded->live, because the two are not the same number and a run reading this
                // line is looking for the LIVE slot the engine's own dump names.
                char why[96] = {0};
                if (slots_[i].hop == PawnProvision::Hop::PhysicsCi) {
                    snprintf(why, sizeof(why), ":awaiting(%s)",
                             PawnProvision::PhysicsCiReqName(slots_[i].ciWhy));
                } else if (slots_[i].participate.state ==
                           PawnProvision::Participation::Issued) {
                    snprintf(why, sizeof(why), ":issued(epoch %u,role %u,%s)",
                             unsigned(slots_[i].driveGate.epoch),
                             unsigned(slots_[i].driveGate.issuedRole),
                             PawnProvision::ParticipateDriveName(slots_[i].driveGate.via));
                } else if (slots_[i].participate.state ==
                           PawnProvision::Participation::SkippedAlreadyDone) {
                    snprintf(why, sizeof(why), ":skipped");
                } else {
                    snprintf(why, sizeof(why), ":refused(%s)",
                             PawnProvision::ParticipateRefusalName(slots_[i].participate.why));
                }
                const int w = snprintf(hops + n, sizeof(hops) - n, "%s%u->%u=%s%s", n ? "," : "",
                                       i, slots_[i].mapped, PawnProvision::HopName(slots_[i].hop),
                                       why);
                if (w <= 0 || size_t(w) >= sizeof(hops) - n) {
                    // Cannot happen while kEntryMax is derived, and it SAYS SO if it ever does
                    // rather than letting the reader take a short list for a short table.
                    ++dropped;
                    continue;
                }
                n += size_t(w);
            }
            if (dropped != 0) {
                const int w = snprintf(hops + n, sizeof(hops) - n, ",+%u truncated",
                                       unsigned(dropped));
                if (w > 0 && size_t(w) < sizeof(hops) - n) n += size_t(w);
            }
            hops[n] = 0;
            // FeatureState rides the SAME line as the census (S14.7 (vii)), because a census
            // read without it cannot tell a driver that refused from one that was never built.
            const PawnProvision::FeatureState fs =
                env_.hops != nullptr ? PawnProvision::ParticipationFeatureState(*env_.hops)
                                     : PawnProvision::FeatureState{false, "NoHopSurface"};
            const PawnLedger led = Ledger();
            // WHY NO LOADOUT WAS AUTHORED, BY NAME.  Asked of the recording, not asserted: the
            // demo -> JoinDesc path carries the twenty item ids BY POSITION and no slot key
            // (foundation/demofile/demotypes.h and manifest_io.cpp are where the key has to
            // arrive), so a recording that HAS a profile document still answers
            // LoadoutSlotKeysNotRecorded and L1 refuses rather than dressing an entrant in a
            // positional guess.  A placeholder here would read, to the next run, as a dress that
            // simply had nothing to do.
            // WHAT THE DRESS DID, BY NAME (part2 F15/F18).  Three numbers, because two of them
            // cannot express a parked entrant: K authored of N attempted of M LIVE.  B1 closes on
            // K == N == M with every compared pair matched; anything else names the first refusal
            // and the hop the deferred entrants are parked on.
            char loadoutClause[224] = {0};
            PawnProvision::LoadoutOutcome rows[facts::ENTITY_USER_SLOT_TABLE_MAX];
            for (uint16_t i = 0; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i) {
                const Slot& c = slots_[i];
                PawnProvision::LoadoutOutcome& r = rows[i];
                r.alive = c.state == SlotState::Awaiting || c.state == SlotState::Provisioned;
                r.docPresent = c.desc.entrantJson.n != 0;
                r.attempted = c.loadoutAttempted;
                r.deferralWhy = c.loadoutAttempted ? nullptr : PawnProvision::HopName(c.hop);
                r.why = c.loadoutWhy;
                r.sourceWhy = c.loadoutSourceWhy;
                r.compared = c.loadoutCompared;
                r.matched = c.loadoutMatched;
            }
#ifdef W2P1_MUTATE_CENSUS_BARE_ZERO
            snprintf(loadoutClause, sizeof(loadoutClause), "not-authored (S2 open)");
#else
            PawnProvision::FormatLoadoutClause(
                PawnProvision::SummariseLoadouts(rows, facts::ENTITY_USER_SLOT_TABLE_MAX),
                loadoutClause, sizeof(loadoutClause));
#endif
            // THE MODEL GATE, ON THE CADENCE THE REST OF THIS LINE IS READ AT (F44).  The
            // per-slot census carries the same clause, but it runs once per epoch and only once a
            // slot has reached Provisioned -- and every run so far parked its entrant in Awaiting,
            // so the clause could not print on the one state it was written for.  Here it is asked
            // of the SAME slots the hop census above just walked, every window, from the same
            // formatter.  `mixed` rather than one slot answering for the table, and `unasked` when
            // no slot could be asked at all -- the three-way `physicsCi` on the line above uses.
            uint16_t modelSlots[facts::ENTITY_USER_SLOT_TABLE_MAX];
            PawnProvision::Refusal modelWhys[facts::ENTITY_USER_SLOT_TABLE_MAX];
            uint16_t modelN = 0;
            for (uint16_t i = 0; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i) {
                if (slots_[i].state != SlotState::Awaiting &&
                    slots_[i].state != SlotState::Provisioned) {
                    continue;
                }
                if (slots_[i].mapped < facts::ENTITY_USER_SLOT_TABLE_MAX) {
                    // The slot's OWN held verdict rides beside its mapped index, so the census
                    // reason names asked-and-failed (ModelNotInstantiated) apart from a never-driven
                    // absence rather than re-guessing off the live surface (AUD-W2-P1-46 MAJOR-1).
                    modelWhys[modelN]  = slots_[i].modelWhy;
                    modelSlots[modelN] = slots_[i].mapped;
                    ++modelN;
                }
            }
            char modelClause[192] = {0};
#ifndef W2P1_MUTATE_CENSUS_MODEL_UNWIRED
            ModelClauseText(env_.entity, env_.netGame != nullptr ? env_.netGame() : nullptr,
                            modelSlots, modelWhys, modelN, modelClause, sizeof modelClause);
#endif
            env_.log->Write(LogLevel::Info, "pawn",
                            "hops: %s feature: %s%s%s%s issued: %u skipped: %u refused: %u%s "
                            "loadout: %s (%s)",
                            env_.hops != nullptr ? hops : "unasked",
                            fs.armed ? "Armed" : "Disabled(", fs.armed ? "" : fs.reason,
                            fs.armed ? "" : ")", "",
                            unsigned(led.participateIssued), unsigned(led.participateSkipped),
                            unsigned(led.participateRefused), modelClause, loadoutClause,
                            env_.hops != nullptr
                                ? "each hop is the engine's own record for that step, re-read "
                                  "every cadence window until it reads Done"
                                : "no actor-hop surface is wired, so no hop was asked of the "
                                  "engine");
        }
    }

    // A census at first provision, and only once a pawn exists to census (before that the pools hold
    // nothing this packet put there and the numbers would answer FACT-PP1 about the viewer alone), then
    // a FULL REPRINT every ~5s: post-admit model and swapper drift must be visible
    // frame-over-frame, not only at the instant the first pawn appeared.  kRetryTicks pumps is the ~1s
    // status cadence above; five of them is the reprint period.
    if (ledger_.provisioned >= 1 && env_.entity != nullptr && env_.log != nullptr &&
        (!censused_ || pumps_ >= next_census_) && replay::log::ChatterDue(censusChatterAt_)) {
        censused_ = true;
        next_census_ = pumps_ + 5ull * uint64_t(kRetryTicks);
        CensusProvisionedPawns(ledger_, *env_.entity,
                               env_.stateRaw != nullptr ? env_.stateRaw() : kCensusStateUnknown,
                               *env_.log, env_.netGame != nullptr ? env_.netGame() : nullptr);
    }
    // LAST, not first: TryDressRoster runs before Pump in the same frame and admits through the
    // same cadence, so the two share one pump number and therefore one decision.  Bumping it on
    // entry would give a blocked frame two windows instead of one.
    ++pumps_;
    return done;
}

bool PawnProvisioner::SlotClaimed(uint16_t live) const {
    for (const Slot& s : slots_)
        if (s.mapped == live) return true;
    return false;
}

// The lobby's connection record, read for the log and not for the decision.
//
// sub_1406051C0 (LOBBY.JOIN_PICK_SLOT_VA) picks the first slot whose CONNECTION record says nobody
// holds it: conn[slot] + LOBBY.CONN_PEERID_OFF == the qword at LOBBY.JOIN_FREE_SENTINEL_VA,
// bounded by LOBBY.MEMBER_COUNT_OFF -- but only when LOBBY.JOIN_LOCKED_OFF is clear and the
// optional chooser at LOBBY.PICK_SLOT_HOOK_OFF declined, and it is reached only by a
// SNSLobbyAddEntrantRequestv4 round trip.  A local entrant is admitted here WITHOUT that round
// trip, so its slot never appears in conn[] at all and this record can neither grant nor deny it.
//
// IndexOf reads exactly that side table, so an entry nobody wrote answers index 0 -- a LEGAL index
// -- for every one of the 16 slots.  That is what L3 #12's "every slot in the engine's table is
// held by an entrant this session did not create" was: an unwritten table, not a full lobby.
//
// What this process can read back is the entrant itself, whose own record names its slot: every
// engine site that asks reads `user[idx].word & IDENT.USER_SLOT_MASK` bounded by the entrant count
// (inside the insert at sub_140152210, and again in sub_1401539A0, sub_14017B190 and
// sub_14017B850 after it returns), which is what SlotOf answers.  So the side table is believed
// only when the entrant it names agrees it holds the slot.
// The lobby's connection array, proven once per (base, extent, bind epoch).  A refusal is never
// cached, so a lobby that binds a frame later is picked up on the next call.
entity::SpanProof g_lobbyConn;

// The engine's own answer, off the engine's own surface.
//
// CR15NetGame + NETGAME.LOBBY_OFF holds the CNSLobby; LOBBY.OBJ_OFF steps to the SUBOBJECT the
// LOBBY.* offsets are measured from -- address arithmetic, not a load, which is what the
// constructor's `add rcx, 18h` at 0x140147CE0 encodes and what its callee proves by initialising
// exactly the four fields used below.  A record's first qword is the peer id: LOBBY.CONN_PEERID_OFF
// is zero and the registry withholds it, a zero displacement having no encoded byte for an anchor
// to stand on, so the term is absent here rather than spelled as a number no guard could check.
//
// False means the chain could not be walked -- not "held", not "free".  The caller then falls back
// to the side table, the only surface this plugin could reach before LOBBY.OBJ_OFF was filed.
namespace PawnProvision {

bool ReadLobbySlot(void* ng, uint16_t slot, LobbyRecord& out) {
    if (!ng) return false;

    void* lobby = nullptr;
    if (!entity::TryLoad(static_cast<const uint8_t*>(ng) + facts::NETGAME_LOBBY_OFF, lobby) ||
        !lobby)
        return false;

    const uint8_t* table = static_cast<const uint8_t*>(lobby) + facts::LOBBY_OBJ_OFF;
    void*          conn  = nullptr;
    uint64_t       count = 0;
    if (!entity::TryLoad(table + facts::LOBBY_CONN_ARRAY_OFF, conn) || !conn) return false;
    if (!entity::TryLoad(table + facts::LOBBY_CONN_COUNT_OFF, count) || count == 0) return false;

    // The engine bounds its own walk by LOBBY.CONN_COUNT_OFF, and this plugin never asks about more
    // slots than ENTITY.USER_SLOT_TABLE_MAX, so the span proved is the smaller of the two.
    const uint64_t n = count < facts::ENTITY_USER_SLOT_TABLE_MAX
                           ? count
                           : facts::ENTITY_USER_SLOT_TABLE_MAX;
    const uint8_t* base = g_lobbyConn.Prove(conn, size_t(n) * facts::LOBBY_CONN_STRIDE);
    if (!base) return false;

    if (slot >= n) {
        // At or above the connection count the engine skips the slot without reading its record,
        // so it is not a candidate here either -- and no peer id was read to print.
        out.state = SlotOccupancy::Held;
        return true;
    }
    if (!entity::TryLoad(base + size_t(slot) * facts::LOBBY_CONN_STRIDE, out.peer)) {
        out.state = SlotOccupancy::Unreadable;
        return true;
    }
    out.state = out.peer == facts::LOBBY_JOIN_FREE_SENTINEL ? SlotOccupancy::Free
                                                            : SlotOccupancy::Held;
    return true;
}

// THE ADMISSION GATE, and the whole of L3 #14's fix: it asks the predicate above -- the one the
// mapper mapped with -- instead of re-deriving occupancy from the side table.  ENG-W2-P1-5 proved
// admission never writes that table's +0 word, so its zero-filled entries answer "index 0" for all
// sixteen slots; a gate keyed on them refuses SlotAlreadyOccupied on the very slot the engine
// itself calls free, which is what L3 #14 printed twelve lines after `recorded slot 0 -> slot 1`.
//
// The side table survives for ONE job: a slot whose named index names that slot BACK is held by an
// entrant this session did not create -- the viewer, at slot 0 -- and the insert is not made on it.
// That cross-check is positive evidence; the table's bare index word is not.
ReplyMsg AuthorEntrantAddedMsg(uint16_t slot) {
    ReplyMsg m{};
    // The engine spells "no slot" as an all-ones qword at both of its own publish sites, and
    // kSlotUnmapped is 16 bits of ones -- widening it would publish slot 65535, which is a slot
    // number and not an absence.
    m.slot = (slot == kSlotUnmapped) ? UINT64_MAX : uint64_t(slot);
#ifdef W2P1_MUTATE_MSG_SLOT_OFF_BY_ONE
    m.slot = uint64_t(uint16_t(slot + 1));
#endif
    return m;
}

Refusal ProvisionViaReply(void* netGame, uint16_t slot, const AdmitHooks& hooks, UserIndex& out) {
    out = UserIndex(kSlotUnmapped);
    if (!netGame) return Refusal::NoNetGame;
    if (slot >= facts::ENTITY_USER_SLOT_TABLE_MAX) return Refusal::SlotOutOfRange;

    const ReplyRoute* route = hooks.reply;
    if (!route || !route->promoted || !route->inReceivePhase || !route->seedLobbyRecord ||
        !route->dispatch || !route->promoted(route->ctx))
        return Refusal::ReplyRouteNotPromoted;

    // The postconditions are read off the ENGINE, so a route wired without them could report a
    // publish as a provision.  Asked before anything is published, not after.
    if (!hooks.indexOfSlot || !hooks.slotOfIndex || !hooks.actorCi) return Refusal::AdmitNotPromoted;

    // ALREADY SEATED, AND IT IS ASKED BEFORE THE PHASE GATE.  The engine's own drain admits this
    // slot from inside the pump; a dress pass asking again from the tick can only ever be told the
    // phase is wrong -- structurally, not as a race -- so an entrant that is already there was
    // reported as a deferral forever and nothing ever retried into a dress.  The evidence is the
    // same occupancy pair the success tail below asserts, so this answers what that tail would.
    // INV-PP1 is not weakened: PawnProvisioner::Admit tests ForeignEntrant on the same slot before
    // this route is reached, so a slot held by somebody else is never adopted here.
#ifndef W2P1_MUTATE_NO_REPLY_IDEMPOTENCE
    {
        const uint16_t held = hooks.indexOfSlot(hooks.ctx, slot);
        if (held != kSlotUnmapped && hooks.slotOfIndex(hooks.ctx, held) == slot) {
            // The engine's own drain seated it from THIS session's dispatch (a foreign entrant was
            // refused upstream), so this is a slot the caller owns and may hand back.
            if (hooks.seated) hooks.seated(hooks.ctx, slot, held);
            out = UserIndex(held);
            return Refusal::None;
        }
    }
#endif

    // THE PHASE GATE, AND IT IS BEFORE EVERY ENGINE CALL THIS ROUTE MAKES.  The handler runs on the
    // publisher's own thread and builds an actor inside the component system; L3 #21 proved that
    // call does not return when it is issued from inside another engine update.  The engine's own
    // publisher is a message handler, so this route may only run where the engine's own does.
#ifndef W2P1_MUTATE_NO_PHASE_GUARD
    if (!route->inReceivePhase(route->ctx)) return Refusal::ReplyWrongPhase;
#endif

    // The handler reads the entrant's ids, account type, team and profile document out of the LOBBY
    // record for this slot -- the message carries none of them -- so the record is seeded first and
    // its refusal is returned by name.
    const Refusal seeded = route->seedLobbyRecord(route->ctx, slot);
    if (seeded != Refusal::None) return seeded;

    const ReplyMsg msg = AuthorEntrantAddedMsg(slot);
    if (!route->dispatch(route->ctx, msg, uint32_t(sizeof(msg))))
        return Refusal::ReplyNotDispatched;

    // What the ENGINE did, asked of the engine.  A publish that returned true is not a pawn.
    const uint16_t issued = hooks.indexOfSlot(hooks.ctx, slot);
    if (issued == kSlotUnmapped) return Refusal::AdmitDidNotCreateUser;
    if (hooks.slotOfIndex(hooks.ctx, issued) != slot) return Refusal::AdmitDidNotCreateUser;
    // The entrant exists from here on, and AdmitDidNotCreatePhysics below still leaves it existing.
    if (hooks.seated) hooks.seated(hooks.ctx, slot, issued);
    if (hooks.actorCi(hooks.ctx, issued) == kSlotUnmapped)
        return Refusal::AdmitDidNotCreatePhysics;

    out = UserIndex(issued);
    return Refusal::None;
}

Refusal AdmitIntoLobbySlot(void* netGame, uint16_t slot, const AdmitHooks& hooks, UserIndex& out) {
    out = UserIndex(kSlotUnmapped);

    // ★ THE ENGINE'S OWN ROUTE FIRST.  ARCH-23 S3 decision B is "authored joins via the engine's
    // remote-player spawn path", and P1-19 settled in bytes what that path IS: the handler the
    // netgame registers for the lobby's entrant-added message, whose default arm is the remote one.
    // Only ReplyRouteNotPromoted falls through to the direct insert below -- every other answer,
    // success included, is the route's verdict and the insert is not called at all.
    {
        const Refusal viaReply = ProvisionViaReply(netGame, slot, hooks, out);
        if (viaReply != Refusal::ReplyRouteNotPromoted) return viaReply;
        out = UserIndex(kSlotUnmapped);
    }

    if (!netGame) return Refusal::NoNetGame;
    if (slot >= facts::ENTITY_USER_SLOT_TABLE_MAX) return Refusal::SlotOutOfRange;
    if (!hooks.indexOfSlot || !hooks.slotOfIndex || !hooks.admit) return Refusal::AdmitNotPromoted;
    if (!hooks.createActor || !hooks.actorCi) return Refusal::AdmitNotPromoted;
    if (!hooks.loadoutCount) return Refusal::AdmitNotPromoted;
    if (!hooks.actorAssetsReady) return Refusal::AdmitNotPromoted;
    // The recorded-profile dress is the REMOTE path's job (that entrant joins undressed).  The bot
    // path admits through AddBotUser, which applies the default profile+loadout and a
    // visible default body itself, so those three hooks are an OPTIONAL overlay that may be absent
    // (config pawn.bot_loadout off).  Require them only off the bot path.
    if (!hooks.botBody &&
        (!hooks.applyProfile || !hooks.attachProfile || !hooks.profileApplied))
        return Refusal::AdmitNotPromoted;

#ifdef W2P1_MUTATE_SLOT_RULE_FROM_CONN
    LobbyRecord before{};
    if (!ReadLobbySlot(netGame, slot, before) || before.state == SlotOccupancy::Unreadable)
        return Refusal::SlotOccupancyUnreadable;
    if (before.state != SlotOccupancy::Free) return Refusal::SlotAlreadyOccupied;
#endif

    // THE GATE, and it is the engine's own entrant table.  conn[] is not consulted: the engine
    // reaches sub_1406051C0's conn scan for a bot only by sending SNSLobbyAddEntrantRequestv4 and
    // letting the reply hand it a slot, and this plugin calls the insert directly instead.  What
    // the engine tests before it will even ask for a local entrant's slot is the entrant COUNT --
    // LOBBY.BOT_ROOM_TEST_SITE, limit ENTITY.USER_SLOT_TABLE_MAX -- over the same table below.
    const uint16_t named = hooks.indexOfSlot(hooks.ctx, slot);
#ifdef W2P1_MUTATE_NO_VIEWER_EXCLUSION
    (void)named;
#else
    if (named != kSlotUnmapped && hooks.slotOfIndex(hooks.ctx, named) == slot)
        return Refusal::SlotAlreadyOccupied;
#endif

    const Refusal why = hooks.admit(hooks.ctx, slot, out);
    if (why != Refusal::None) {
        out = UserIndex(kSlotUnmapped);
        return why;
    }

    // THE POSTCONDITION, on what the engine writes for a LOCAL entrant -- the header carries the
    // bytes.  It is the SAME pair the gate above consulted, read the other way round: before the
    // insert neither may name this slot, after it BOTH must name it, with the index we were just
    // handed.  Reading conn[slot] back here is what L3 #15 refused every correct admission on --
    // the connection array is written only by the net layer's add-entrant handler, and the insert
    // this plugin calls is the engine's own bot/local path, which never touches it.
    if (out.v == kSlotUnmapped) return Refusal::InsertReturnedNoIndex;
    const UserIndex issued = out;
    out = UserIndex(kSlotUnmapped);
    if (hooks.slotOfIndex(hooks.ctx, issued.v) != slot) return Refusal::AdmitDidNotCreateUser;
    if (hooks.indexOfSlot(hooks.ctx, slot) != issued.v) return Refusal::AdmitDidNotCreatePlayer;
    // ★ THE ENTRANT NOW EXISTS, and every exit below this line is a DEFERRAL that leaves it existing
    // -- LoadoutsEmpty and ActorAssetsNotReady both return after the insert has taken.  A caller that
    // records "this session seated slot N" from the RETURN VALUE therefore misses exactly the slots
    // it seated on a deferring pass, and then refuses to remove its own entrant later.  So the fact
    // is reported HERE, at the one point the engine's own pair has just confirmed it.
    if (hooks.seated) hooks.seated(hooks.ctx, slot, issued.v);

    // ★ THE BOT-BODY PATH, and it returns before the remote dress ladder.  AddBotUser
    // (the `admit` above) already installed the default profile+loadout and a visible default body
    // (sub_140152210 -> sub_14016D960 default-server-profile -> local model swap; DIAG-BOT-SPAWN-
    // PATH).  What it did NOT install is the ncaPhysics/R14RemotePlayer pair the history-drive needs
    // -- those come only from SET_PLAYER_ACTOR, which the actor step below reaches.  So the recorded
    // profile is an OPTIONAL overlay run BEST-EFFORT (never refuses: a failed overlay must not discard
    // the visible body), while the crash guard and the readiness predicate still gate the actor step
    // exactly as on the remote path.
    if (hooks.botBody) {
        // Fill the per-user loadout SOURCE with the RECORDED loadout, and it
        // runs HERE -- after `admit` and before the SOURCE->LIVE copy below -- on purpose:
        // AddBotUser's GenerateBotProfile clears and reparses that SOURCE container during the insert
        // (sub_14016C140 -> IDENT.LOADOUT_FROM_PROFILE), so a fill done before admit does not survive.
        // Parsed here, after the insert has finished clobbering it, the recording's real instances are
        // what `publishLoadout` copies into the live table -- not the empty default that stalled
        // echo-98.  Best-effort: a parse that does not take leaves the SOURCE (and so the live count)
        // at 0 and the guard below defers, so the body still ships without a deadlock.
        if (hooks.sourceLoadout) (void)hooks.sourceLoadout(hooks.ctx, slot, issued.v);
        // Bridge the loadout SOURCE across to the per-slot live table the crash
        // guard and the actor step read -- the IDENT.LOADOUT_ASSIGN copy the engine's own add-entrant
        // handler runs above the insert, which a direct insert never reached.  BEFORE the guard, and
        // best-effort: a copy that does not take leaves the count at 0 and the guard below defers, so
        // the body still ships.
        if (hooks.publishLoadout) (void)hooks.publishLoadout(hooks.ctx, slot, issued.v);
        if (hooks.attachProfile) (void)hooks.attachProfile(hooks.ctx, slot, issued.v);
        if (hooks.applyProfile)  (void)hooks.applyProfile(hooks.ctx, slot, issued.v);
        // THE CRASH GUARD stays: IDENT.APPLY_LOADOUT_ITEMS faults on a zero-length instance array and
        // the actor step drives it.  The bot's default loadout keeps this > 0; a 0 is a DEFERRAL (the
        // default apply has not landed yet), retried on a later tick, not a hard failure.
        if (hooks.loadoutCount(hooks.ctx, slot) == 0) return Refusal::LoadoutsEmpty;
        // THE READINESS PREDICATE stays (L3 #21): the actor step is not issued speculatively from a
        // tick against an entrant whose actor component does not resolve.
        if (!hooks.actorAssetsReady(hooks.ctx, issued.v)) return Refusal::ActorAssetsNotReady;
        (void)hooks.createActor(hooks.ctx, issued.v);
        // The admitted index is the result -- the phys/remote CI the actor step installs is vouched
        // per-tick by the history-drive, not gated here, so the visible body ships even if that CI
        // lands a tick later (unlike the remote path's AdmitDidNotCreatePhysics postcondition).
        out = issued;
        return Refusal::None;
    }

    // THE ACTOR STEP.  L3 #16 admitted an entrant with the state word already at 9 -- the census
    // printed `stateRaw=9` -- so the insert's own `cmp dword ptr [rdi], 9` arm at 0x14015269A DID
    // run, and the run still had no body: that arm ENABLES and PLACES an actor
    // (NETGAME.ENABLE_ACTOR then NETGAME.PLACE_ACTOR, whose lookup returns without writing when
    // the guid resolves to nothing), it does not build one out of a body template.  The step that
    // does is NETGAME.SET_PLAYER_ACTOR, reached only from the engine's REMOTE entrant admission,
    // and the bot insert this plugin calls is its sibling, not its caller.  So the provision runs
    // the same step the engine runs, through its own wrapper, and then asks whether a component
    // instance answers -- the one fact `driveable` is read off downstream.
    // THE DRESS, AND IT IS ORDERED BEFORE THE ACTOR STEP.  This is L3 #18 F1's fix and the order
    // is the whole of it.  The engine's REMOTE admission gets a profile and a loadout out of the
    // add-entrant reply and applies them before NETGAME.SET_PLAYER_ACTOR runs; the bot insert this
    // plugin calls has no reply to read, so nothing dressed the entrant and `Loadouts : 0` was
    // what the engine printed.  Asking for an actor in that state made IDENT.APPLY_LOADOUT_ITEMS
    // walk an empty instance array, miss the name it was applying, and deserialise customisation
    // off the end of a zero-length CMemStream.  So the recorded profile goes on FIRST.
#ifndef W2P1_MUTATE_ACTOR_BEFORE_PROFILE
    // THE ATTACH, AND IT IS UPSTREAM OF THE DRESS.  L3 #19's 58 `json db is cached, read only`
    // lines were not a dress that ran badly: they were a dress writing into a document the json
    // db owns, which refuses every key and returns void.  The engine's own remote join answers
    // that by REPLACING the document (0x14019F613), so that is what runs first here -- and its
    // refusal is returned VERBATIM, because which of the two ways it failed is the whole of what
    // the next run has to read.
#ifndef W2P1_MUTATE_NO_ATTACH
    const Refusal attached = hooks.attachProfile(hooks.ctx, slot, issued.v);
    if (attached != Refusal::None) return attached;
#endif

    if (!hooks.applyProfile(hooks.ctx, slot, issued.v)) return Refusal::ProfileNotApplied;

#if !defined(W2P1_MUTATE_PROFILE_TRUSTS_RETURN) && !defined(W2P1_MUTATE_READBACK_AFTER_ACTOR)
    // THE READ-BACK, and it is what decides.  L3 #19: this call answered true over 58 consecutive
    // `$ json path: <key>: ERROR, json db is cached, read only.` lines, because the two engine
    // setters it drives return void and a bot entrant's profile document belongs to the json db
    // CACHE (IDENT.PROFILE_CACHE_ATTACH_VA hands GenerateBotProfile the cache's own document,
    // where a remote join is given its own by IDENT.PROFILE_RECEIVE_VA).  So the wrapper's answer
    // is taken as an attempt, and the ENGINE is asked whether the recording is on the entrant.
    if (hooks.profileApplied == nullptr ||
        !hooks.profileApplied(hooks.ctx, slot, issued.v))
        return Refusal::ProfileNotApplied;
#endif

    // THE CRASH GUARD, and it is a POSTCONDITION on the dress rather than a belief about it.
    // What matters is not that the profile call returned true -- it is that the engine now holds
    // at least one loadout instance for this slot, because that count is the loop bound
    // IDENT.APPLY_LOADOUT_ITEMS reads and 0 is the value it faults on.  Read off the engine, never
    // off what we just asked for: a dress that reported success and left the count at 0 is exactly
    // the entrant this refusal exists to keep the actor step away from.
#ifndef W2P1_MUTATE_NO_LOADOUT_GUARD
    if (hooks.loadoutCount(hooks.ctx, slot) == 0) return Refusal::LoadoutsEmpty;
#endif
#endif

    // THE READINESS PREDICATE, AND IT IS THE LAST THING BEFORE THE ENGINE CALL.  L3 #21 hung
    // exactly here: no WaitForSingleObject, no critical section, no fence -- the main thread's Rip
    // was inside engine code and its stack carried no wait frame at all, eleven seconds after the
    // call and four seconds after "Deadlock detected!".  A call that can fail to RETURN cannot be
    // made speculatively from a tick, so the contract its own two callers satisfy is tested first
    // and the answer comes off the engine, never off what we just asked for.
#ifndef W2P1_MUTATE_NO_ASSETS_GUARD
#ifndef W2P1_MUTATE_ASSETS_GUARD_AFTER_ACTOR
    if (!hooks.actorAssetsReady(hooks.ctx, issued.v)) return Refusal::ActorAssetsNotReady;
#endif
#endif

#ifndef W2P1_MUTATE_NO_ACTOR_STEP
    (void)hooks.createActor(hooks.ctx, issued.v);
#endif

#ifdef W2P1_MUTATE_ASSETS_GUARD_AFTER_ACTOR
    if (!hooks.actorAssetsReady(hooks.ctx, issued.v)) return Refusal::ActorAssetsNotReady;
#endif

#ifdef W2P1_MUTATE_READBACK_AFTER_ACTOR
    // THE READ-BACK, and it is what decides.  L3 #19: this call answered true over 58 consecutive
    // `$ json path: <key>: ERROR, json db is cached, read only.` lines, because the two engine
    // setters it drives return void and a bot entrant's profile document belongs to the json db
    // CACHE (IDENT.PROFILE_CACHE_ATTACH_VA hands GenerateBotProfile the cache's own document,
    // where a remote join is given its own by IDENT.PROFILE_RECEIVE_VA).  So the wrapper's answer
    // is taken as an attempt, and the ENGINE is asked whether the recording is on the entrant.
    if (hooks.profileApplied == nullptr ||
        !hooks.profileApplied(hooks.ctx, slot, issued.v))
        return Refusal::ProfileNotApplied;
#endif

#ifdef W2P1_MUTATE_ACTOR_BEFORE_PROFILE
    if (!hooks.applyProfile(hooks.ctx, slot, issued.v)) return Refusal::ProfileNotApplied;
    if (hooks.loadoutCount(hooks.ctx, slot) == 0) return Refusal::LoadoutsEmpty;
#endif
    if (hooks.actorCi(hooks.ctx, issued.v) == kSlotUnmapped)
        return Refusal::AdmitDidNotCreatePhysics;

    out = issued;
    return Refusal::None;
}

}  // namespace PawnProvision

bool PawnProvisioner::ProbeLobbySlot(uint16_t slot, SlotProbe& out) const {
    if (!env_.netGame) return false;
    PawnProvision::LobbyRecord rec;
    if (!PawnProvision::ReadLobbySlot(env_.netGame(), slot, rec)) return false;
    out.lobby = true;
    out.state = rec.state;
    out.peer  = rec.peer;
    return true;
}

bool PawnProvisioner::EntrantTableProved() const {
    // Absence of the hook is absence of a FAULT REPORT, not absence of a table.  Every caller
    // that can span-prove the entrant table is downstream of this header's freeze, so a
    // provisioner armed by code that predates the hook reads an absent row as what the engine's
    // own table means by it: FREE.
    return env_.entrantTableProved == nullptr || env_.entrantTableProved();
}

PawnProvisioner::SlotProbe PawnProvisioner::ProbeSlot(uint16_t slot) const {
    SlotProbe p;
    // conn[] is read for the DIAGNOSTIC line and never to decide.  sub_1406051C0 scans it, and the
    // engine reaches that scan for a BOT too -- but only by sending SNSLobbyAddEntrantRequestv4
    // (LOBBY.REQUEST_ADD_BOT_VA -> sub_140601290) and letting the reply hand it the slot.  This
    // plugin calls IDENT.ADMIT_USER_VA directly, so nothing it admits ever gets a conn[] record,
    // and a table this process does not write cannot say where it may write.  What the engine
    // itself demands before it will even ask for a local entrant's slot is a COUNT over the
    // entrant table -- `cmp word ptr [rax+IDENT.NONREPLAY_COUNT_OFF], 10h` at
    // LOBBY.BOT_ROOM_TEST_SITE, whose limit is ENTITY.USER_SLOT_TABLE_MAX -- so the entrant table
    // bounded by that same max is the rule below, and conn[] is a line in the log.
    ProbeLobbySlot(slot, p);
#ifdef W2P1_MUTATE_SLOT_RULE_FROM_CONN
    if (p.lobby) return p;
#endif
    p.index = env_.identity->IndexOf(UserSlot(slot)).v;
    if (p.index == kSlotUnmapped) {
        // The side table names nobody. That is the one answer on this surface that is positive
        // evidence of a free slot, and it is what a written table says about an empty row.
        p.state = SlotOccupancy::Free;
        return p;
    }
#ifdef W2P1_MUTATE_NO_VIEWER_EXCLUSION
    // The viewer is the one entrant this session did not create; the side table is the only
    // surface here that names it.  Calling a named slot free hands the viewer's pawn to a file.
    p.state = SlotOccupancy::Free;
    return p;
#endif
    p.named = env_.identity->SlotOf(UserIndex(p.index)).v;
    // The entrant that index names either names THIS slot back -- it occupies it -- or names a
    // different one, which says nothing about this slot but does say the table answered.
    //
    // No entrant carrying that index at all is the third case and it is the empty lobby's normal
    // state: an ABSENT row is how the engine's table spells `nobody is in this slot`, so it is
    // FREE.  L3 #17 scored it UNREADABLE and refused all fifteen empty slots of a one-player
    // offline lobby.  UNREADABLE is reserved for the table's memory failing its own span proof,
    // which is a fault about the TABLE and is the only thing that can make an absence unsafe.
    if (p.named == slot) {
        p.state = SlotOccupancy::Held;
    } else if (p.named != kSlotUnmapped) {
        p.state = SlotOccupancy::Free;
    } else {
#ifdef W2P1_MUTATE_ABSENT_ROW_UNREADABLE
        p.state = SlotOccupancy::Unreadable;
#else
        p.state = EntrantTableProved() ? SlotOccupancy::Free : SlotOccupancy::Unreadable;
#endif
    }
    return p;
}

void PawnProvisioner::NoteForeignEntrant(const SlotProbe& probe) {
    // Only the side-table cross-check names an ENTRANT.  When the lobby's own predicate answered,
    // `index` was never consulted and there is no index to remember; and Held-by-the-lobby is not
    // evidence about the entrant table at all.
    if (probe.lobby || probe.state != SlotOccupancy::Held || probe.index == kSlotUnmapped) return;
    for (uint16_t i = 0; i < foreignCount_; ++i)
        if (foreign_[i] == probe.index) return;
    if (foreignCount_ < facts::ENTITY_USER_SLOT_TABLE_MAX) foreign_[foreignCount_++] = probe.index;
}

bool PawnProvisioner::ForeignEntrant(UserIndex index) const {
    for (uint16_t i = 0; i < foreignCount_; ++i)
        if (foreign_[i] == index.v) return true;
    return false;
}

bool PawnProvisioner::AssignMappedSlot(uint16_t recorded, uint16_t& out, uint16_t& unreadable) {
    // Ascending, with the recorded slot tried first, so the answer is a function of the recorded
    // number and the engine's occupancy alone.  Pump walks the table ascending, so the same file
    // loaded twice into the same session assigns the same live slots in the same order -- which is
    // what "deterministic and stable" in the header has to mean to be worth printing.
    unreadable = 0;
    if (!SlotClaimed(recorded)) {
        const SlotProbe pr = ProbeSlot(recorded);
        NoteForeignEntrant(pr);
        if (pr.state == SlotOccupancy::Free) {
            out = recorded;
            return true;
        }
    }
    // On the failing path every slot is visited, so `unreadable` is a count over the whole table
    // by the time the caller reads it; on the succeeding path the caller does not look.
    for (uint16_t i = 0; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i) {
        if (SlotClaimed(i)) continue;
        const SlotProbe p = ProbeSlot(i);
        NoteForeignEntrant(p);
        if (p.state == SlotOccupancy::Free) {
            out = i;
            return true;
        }
        if (p.state == SlotOccupancy::Unreadable) ++unreadable;
    }
    return false;
}

UserSlot PawnProvisioner::MappedSlot(UserSlot recorded) const {
    if (recorded.v >= facts::ENTITY_USER_SLOT_TABLE_MAX) return UserSlot(kSlotUnmapped);
    return UserSlot(slots_[recorded.v].mapped);
}

bool PawnProvisioner::Provisioned(UserSlot slot) const {
    return slot.v < facts::ENTITY_USER_SLOT_TABLE_MAX &&
           slots_[slot.v].state == SlotState::Provisioned;
}

PawnProvision::Refusal PawnProvisioner::SlotRefusal(UserSlot slot) const {
    if (slot.v >= facts::ENTITY_USER_SLOT_TABLE_MAX)
        return PawnProvision::Refusal::SlotOutOfRange;
    return slots_[slot.v].why;
}

}  // namespace replay

namespace replay {

// ---------------------------------------------------------------------------------------

namespace {

const char* Yn(bool b) { return b ? "yes" : "no"; }

const char* PawnClassName(PawnClass c) {
    switch (c) {
        case PawnClass::Local:  return "Local";
        case PawnClass::Remote: return "Remote";
        default: break;
    }
    return "Unknown";
}

// A handle is driveable when a body can be read back through it: that is the same PhysHandle the
// playback driver writes, so a successful read is evidence the write would have a subject.
// The WORD when no key was read, the DIGITS when one was. Entity id 0 is the local pawn's, so a
// census that spelled absence 0 would answer FACT-PP1 with the number that also means nothing was
// asked -- on the one slot the launch cares about.
void EntityKeyText(IEntity::EntityKey word, uint16_t key, char* out, size_t cap) {
    switch (word) {
        case IEntity::EntityKey::Ok:
            _snprintf_s(out, cap, _TRUNCATE, "%u", static_cast<unsigned>(key));
            return;
        case IEntity::EntityKey::NotAsked:        _snprintf_s(out, cap, _TRUNCATE, "not-asked"); return;
        case IEntity::EntityKey::Unavailable:     _snprintf_s(out, cap, _TRUNCATE, "unavailable"); return;
        case IEntity::EntityKey::NoPawn:          _snprintf_s(out, cap, _TRUNCATE, "no-pawn"); return;
        case IEntity::EntityKey::NoCi:            _snprintf_s(out, cap, _TRUNCATE, "no-ci"); return;
        case IEntity::EntityKey::IdentityRefused: _snprintf_s(out, cap, _TRUNCATE, "identity-refused"); return;
    }
    _snprintf_s(out, cap, _TRUNCATE, "?");
}

bool Driveable(const IEntity& e, uint16_t phys) {
    if (phys == kSlotUnmapped) return false;
    BodyState body;
    return e.ReadBody(PhysHandle(phys), body);
}

// `loadouts=N`, or `loadouts=?(<Word>)` when the read could not be made.  A count and a
// could-not-ask must not share a spelling: the engine's own `Loadouts : 0` at L3 #20 was printed at
// INSERT time, before anything had dressed the entrant, and was read as a fact about the entrant.
void LoadoutText(void* netGame, uint16_t slot, char* out, size_t cap) {
#ifdef W2P1_MUTATE_NO_LOADOUT_COUNT
    (void)netGame;
    (void)slot;
    if (cap) out[0] = '\0';
#else
    if (netGame == nullptr) {
        snprintf(out, cap, "loadouts=?(NoNetGame)");
        return;
    }
    identity_access::LoadoutCensus lc;
    identity_access::Refusal why = identity_access::Refusal::None;
    // The pools above are asked with the RESOLVED slot as the key; the loadout read is asked with
    // the same key rather than inventing a second mapping here.  Where index and slot do diverge
    // the read refuses BY NAME and this field carries the word instead of a number.
    if (!identity_access::CensusLoadout(netGame, UserIndex(slot), UserSlot(slot), lc, &why)) {
        char word[48] = {};
        const char* text = identity_access::RefusalText(why);
        size_t n = 0;
        while (text[n] && text[n] != ':' && n + 1 < sizeof word) {
            word[n] = text[n];
            ++n;
        }
        snprintf(out, cap, "loadouts=?(%s)", word);
        return;
    }
    snprintf(out, cap, "loadouts=%u%s", unsigned(lc.instances), lc.clamped ? " clamped" : "");
#endif
}

// ONE source for the word on `model: resolved=no`. DIAG-PAWN-STATIC H3 is the whole of
// "invisible" and the census printed the verdict without it: Entity::ModelVisible declines at
// `ComponentOnActor(model_.cs, actor) == kNoComponent` (entity.cpp:2355), and every reader of the
// old line had to guess whether the slot, the actor, the component or the pool was the one missing.
//
// IEntity publishes ONE bool for all of those gates, so this names what the census can SEE and no
// more: `noModelOnActor` is the class every remaining gate of ModelVisible belongs to (no model
// system, no netGame, no component on the actor, no pool, span refused) -- the engine-confirmed
// instance is ComponentOnActor and this line does not claim to have measured which -- and the
// slot's own loadout reading rides beside it, because a chassis is what carries the model and
// `loadouts=0` is the engine's own account of why one was never applied (client:733).
void ModelReasonText(bool modelOk, PawnProvision::Refusal why, const char* loadouts, char* out,
                     size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
#ifdef W2P1_MUTATE_MODEL_REASON_MISSING
    (void)modelOk;
    (void)why;
    (void)loadouts;
    return;
#else
    if (modelOk) return;  // resolved=yes has nothing to name
    // ASKED-AND-FAILED gets its own word, and it is the slot's HELD verdict rather than a re-guess
    // of the live surface.  A slot the drive ran on and whose async body never landed within the
    // grace carries ModelNotInstantiated; every other `resolved=no` is a never-driven /
    // could-not-ask absence, which is noModelOnActor.  Collapsing the two -- printing noModelOnActor
    // for a driven-then-bodyless slot -- is the never-asked vs asked-and-failed defect this line
    // exists to separate (AUD-W2-P1-46 MAJOR-1); `modelWhy` is what holds the answer across windows.
#ifndef W2P1_MUTATE_MODEL_WHY_DEAD
    const char* word =
        why == PawnProvision::Refusal::ModelNotInstantiated ? "ModelNotInstantiated"
                                                            : "noModelOnActor";
#else
    (void)why;
    const char* word = "noModelOnActor";
#endif
    snprintf(out, cap, " reason=%s %s", word, loadouts);
#endif
}

// `unasked` when nothing could be asked, `mixed` when two slots disagree, and the word itself
// otherwise.  One slot must never answer for a twelve-entrant table.
const char* ModelWord(bool asked, bool mixed, bool value) {
    return !asked ? "unasked" : mixed ? "mixed" : Yn(value);
}

void ModelClauseText(const IEntity* e, void* netGame, const uint16_t* live,
                     const PawnProvision::Refusal* whys, uint16_t n, char* out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    if (e == nullptr || !e->Available()) {
        // A could-not-ask is NOT `resolved=no`: the second would send a run looking for a model on
        // an actor nobody was ever able to ask about.
        snprintf(out, cap, " model: resolved=unasked visible=unasked reason=%s",
                 e == nullptr ? "NoEntitySurface" : "EntityUnavailable");
        return;
    }
    bool asked = false, resMixed = false, visMixed = false, res = false, vis = false;
    char loadouts[64] = {};
    bool named = false;
    PawnProvision::Refusal namedWhy = PawnProvision::Refusal::None;
    for (uint16_t k = 0; k < n; ++k) {
        bool visible = false;
        const bool ok = e->ModelVisible(UserIndex(live[k]), visible);
        if (!asked) {
            res = ok;
            vis = visible;
            asked = true;
        } else {
            if (ok != res) resMixed = true;
            if (visible != vis) visMixed = true;
        }
        // The FIRST slot that answered no owns the reason, with its OWN loadout reading: that is
        // the slot a run has to go and look at, and a count borrowed from another one would lie.
        if (!ok && !named) {
            LoadoutText(netGame, live[k], loadouts, sizeof loadouts);
            namedWhy = whys != nullptr ? whys[k] : PawnProvision::Refusal::None;
            named = true;
        }
    }
    char reason[128] = {};
    if (named) ModelReasonText(false, namedWhy, loadouts, reason, sizeof reason);

    // THE CONTROL THE READING NEEDS (DIAG-L3-33 B.5).  `resolved=no` for an entrant has meant
    // either "our actor pointer is wrong" or "the actor is right and the engine never built this
    // one a body", and no run since RUN-30 could tell them apart.  User index 0 is the LOCAL
    // viewer, whose actor is asked through the SAME ModelVisible and the same component system,
    // and who demonstrably wears a model.  `ctrl0=y` beside `resolved=no` fixes the fork on the
    // entrant; `ctrl0=n` says the probe itself answered no for someone who has one, and the
    // finding is not-run rather than a failure.
    char ctrl[16] = {};
#ifdef W2P1_MUTATE_MODEL_CTRL_MISSING
    // The defect: the reading ships with no control, so `resolved=no` stays unfalsifiable.
    (void)ctrl;
#else
    bool ctrlVisible = false;
    snprintf(ctrl, sizeof ctrl, " ctrl0=%s", Yn(e->ModelVisible(UserIndex(0), ctrlVisible)));
#endif
    snprintf(out, cap, " model: resolved=%s visible=%s%s%s", ModelWord(asked, resMixed, res),
             ModelWord(asked, visMixed, vis), ctrl, reason);
}

}  // namespace

uint32_t CensusProvisionedPawns(const PawnLedger& ledger, const IEntity& e, int32_t stateRaw,
                                ILog& log, void* netGame) {
    const bool known = stateRaw != kCensusStateUnknown;
    log.Write(LogLevel::Info, "pawn",
              "census: provisioned %u, mapped %u, failed %u -- state=%s inmatch=%s",
              ledger.provisioned, ledger.mapped, ledger.failed,
              known ? "known" : "UNKNOWN",
              known ? Yn(stateRaw == int32_t(facts::NETGAME_STATE_INMATCH)) : "unknown");
    if (known)
        log.Write(LogLevel::Info, "pawn", "census: stateRaw=%d", stateRaw);

    if (!e.Available()) {
        log.Write(LogLevel::Error, "pawn",
                  "census REFUSED NoEntityService: diag='%s'. No pool was read, so this census "
                  "answers NOTHING about FACT-PP1.", e.Diagnostic());
        return 0;
    }

    log.Write(LogLevel::Info, "pawn",
              "census pools: remote CIs=%llu, phys CIs=%llu, local entries=%llu, diag='%s'",
              static_cast<unsigned long long>(e.PlayerCount()),
              static_cast<unsigned long long>(e.PhysCount()),
              static_cast<unsigned long long>(e.LocalPlayerCount()), e.Diagnostic());

    uint32_t rows = 0, remoteSeen = 0, localSeen = 0, both = 0;
    const uint32_t entries = ledger.mapped < facts::ENTITY_USER_SLOT_TABLE_MAX
                                 ? ledger.mapped : facts::ENTITY_USER_SLOT_TABLE_MAX;
    for (uint32_t i = 0; i < entries; ++i) {
        const uint16_t recorded = ledger.map[i].slot_recorded;
        const uint16_t slot     = ledger.map[i].slot_resolved;
        if (slot == kSlotUnmapped || slot >= facts::ENTITY_USER_SLOT_TABLE_MAX) {
            log.Write(LogLevel::Warn, "pawn",
                      "census: map entry %u (recorded slot %u) carries no resolved slot -- nothing "
                      "to ask the pools about", i, static_cast<unsigned>(recorded));
            continue;
        }

        const uint16_t ci   = e.PlayerOf(UserIndex(slot)).v;
        const uint16_t phys = ci == kSlotUnmapped ? kSlotUnmapped : e.PhysOf(PlayerHandle(ci)).v;

        PlayerState ls;
        IEntity::LocalPlayerReading lr;
        const uint32_t fill = e.ReadLocalPlayerDetailed(UserIndex(slot), ls, lr);
        const bool hasLocal = lr.refusal != IEntity::LocalRefusal::SlotNotInPool &&
                              lr.refusal != IEntity::LocalRefusal::NoBroadcaster;
        const uint16_t echo = lr.echoComponent ? lr.echoHandle : kSlotUnmapped;

        bool visible = false;
        const bool modelOk = e.ModelVisible(UserIndex(slot), visible);

        // WHICH surface answered is the class. Remote wins when both do, because that is the pool
        // W2-P2's ResolvePawn scans; `both` below is what says the choice was not forced.
        const PawnClass cls = ci != kSlotUnmapped ? PawnClass::Remote
                            : hasLocal            ? PawnClass::Local
                                                  : PawnClass::Unknown;

        if (ci != kSlotUnmapped) ++remoteSeen;
        if (hasLocal)            ++localSeen;
        if (ci != kSlotUnmapped && hasLocal) ++both;
        ++rows;

        // BOTH SURFACES, ASKED SEPARATELY. The two routes reach a CI differently and hand back
        // different handles for one slot, so one key could not say which entity it named. Two keys
        // that are EQUAL is FACT-PP1 arm (1) as a number instead of an inference; two that differ
        // separate arms (2) and (3), which is the whole reason the read was ordered.
        // Sized by the widest form it can hold: the refusal word plus its wrapper.
        char loadouts[64] = {};
        LoadoutText(netGame, slot, loadouts, sizeof loadouts);
        // Sized by the widest form: the clause, the refusal word and the loadout text's wrapper.
        char modelReason[128] = {};
        // This once-per-epoch census reads only the ledger, not the slots_ that hold modelWhy, so
        // it cannot name asked-and-failed apart from never-driven -- None keeps the noModelOnActor
        // word.  The per-window `hops:` line (which DOES carry the verdict) is what L3 greps.
        ModelReasonText(modelOk, PawnProvision::Refusal::None, loadouts, modelReason,
                        sizeof modelReason);

        char remoteEnt[24] = {}, localEnt[24] = {};
        uint16_t remoteKey = 0, localKey = 0;
        const IEntity::EntityKey remoteWord =
            e.EntityKeyOf(UserIndex(slot), IEntity::PawnSurface::Remote, remoteKey);
        const IEntity::EntityKey localWord =
            e.EntityKeyOf(UserIndex(slot), IEntity::PawnSurface::Local, localKey);
        EntityKeyText(remoteWord, remoteKey, remoteEnt, sizeof remoteEnt);
        EntityKeyText(localWord, localKey, localEnt, sizeof localEnt);
        const bool bothKeys = remoteWord == IEntity::EntityKey::Ok &&
                              localWord == IEntity::EntityKey::Ok;

        log.Write(LogLevel::Info, "pawn",
                  "census slot=%u (recorded %u) class=%s entity: remote=%s local=%s same=%s | "
                  "remote: ci=%s phys=%s "
                  "driveable=%s | local: ci=%s echo=%s driveable=%s | model: resolved=%s "
                  "visible=%s%s",
                  static_cast<unsigned>(slot), static_cast<unsigned>(recorded), PawnClassName(cls),
                  remoteEnt, localEnt, bothKeys ? Yn(remoteKey == localKey) : "-",
                  ci == kSlotUnmapped ? "ABSENT" : "present",
                  phys == kSlotUnmapped ? "-" : "present", Yn(Driveable(e, phys)),
                  hasLocal ? "present" : "ABSENT", echo == kSlotUnmapped ? "-" : "present",
                  Yn(Driveable(e, echo)), Yn(modelOk), Yn(visible), modelReason);
        log.Write(LogLevel::Info, "pawn",
                  "census slot=%u handles: remote_ci=%u remote_phys=%u local_echo=%u fill=0x%08x "
                  "actor=%s refusal=%s disagree=%s heldByFn=%s %s",
                  static_cast<unsigned>(slot), static_cast<unsigned>(ci),
                  static_cast<unsigned>(phys), static_cast<unsigned>(echo), fill,
                  ActorSourceName(lr.actorSource), LocalRefusalName(lr.refusal),
                  Yn(lr.actorSourcesDisagree), Yn(lr.heldByFn), loadouts);
    }

    log.Write(LogLevel::Info, "pawn",
              "census totals over %u provisioned slot(s): remote CIs=%u, local CIs=%u, in BOTH=%u",
              rows, remoteSeen, localSeen, both);

    if (rows == 0) {
        log.Write(LogLevel::Warn, "pawn",
                  "FACT-PP1 UNDETERMINED: the ledger maps no slot, so there is no provisioned "
                  "entrant to ask about.");
    } else if (remoteSeen == 0 && localSeen == 0) {
        log.Write(LogLevel::Warn, "pawn",
                  "FACT-PP1 UNDETERMINED: neither surface holds a CI for any of the %u provisioned "
                  "slot(s) -- the admission produced no pawn, which is a provision defect and not "
                  "an answer about the pools.", rows);
    } else if (both == 0 && localSeen > 0) {
        log.Write(LogLevel::Info, "pawn",
                  "FACT-PP1 -> ARM (2)/(3): %u provisioned entrant(s) on the local surface and NOT "
                  "ONE carries an SR15RemotePlayerCI. PlayerOf() cannot resolve a provisioned pawn.",
                  localSeen);
    } else if (both == rows) {
        log.Write(LogLevel::Info, "pawn",
                  "FACT-PP1 -> ARM (1): all %u provisioned entrant(s) carry a remote CI as well; "
                  "PlayerOf() resolves a provisioned pawn.", rows);
    } else {
        log.Write(LogLevel::Warn, "pawn",
                  "FACT-PP1 -> MIXED: %u of %u provisioned entrants carry both. Neither arm holds "
                  "session-wide; ResolvePawn must ask per slot.", both, rows);
    }
    return rows;
}

}  // namespace replay

namespace replay {
namespace PawnProvision {
namespace {

// The logger's only caller was PawnProvisioner::Arm, which the playback
// module reaches behind `!g.loaded` and in-match -- so a CAPTURE process, the one that has been
// dying, ran with no VEH and no UEF and its 0xC0000374 was swallowed with nothing written.
// A module of our own is how a translation unit gets a post-attach entry point without editing
// dllmain.cpp or bootstrap.cpp (B.10), and it is the right seam here for a second reason: only
// the artifacts that carry this file get the module, so no probe DLL or bootstrap-only test exe
// acquires an unresolved symbol it has no use for.
//
// modreg::InitAll runs on the bootstrap WORKER thread, whose body cannot begin until our DllMain
// returns and the loader releases the lock that serialises its DLL_THREAD_ATTACH -- so
// AddVectoredExceptionHandler here is outside the loader lock, which is the whole reason the
// install could not simply be moved into DllMain.
constexpr int kModuleOrderCrashLog = 10;
static_assert(kModuleOrderCrashLog < kModuleOrderCapture,
              "the crash logger must be installed before any module it would have to log a fault in");

bool CrashLogModuleInit(ReplayServices*) {
    // Unconditional and mode-blind: a crash logger that has to be switched on is switched off on
    // the run that needed it.  The mode word only says which kind of run the block will describe.
    const char* const modeWord = bootstrap::OpenRequestPresent() ? "playback" : "capture";
    EnsureCrashLogger(LogPath(), modeWord);
    return true;
}

}  // namespace
}  // namespace PawnProvision
}  // namespace replay

REPLAY_MODULE("crashlog", ::replay::PawnProvision::kModuleOrderCrashLog,
              ::replay::PawnProvision::CrashLogModuleInit, nullptr, nullptr);
