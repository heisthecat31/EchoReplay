#pragma once

// ARCH-23 W2-P1 -- the frozen pawn-provision contract, and the provisioner behind it.
//
// The question this answers: L3 #11 proved the file, the window and the binder are right and that
// there is no pawn.  ARCH-23 S3 decides (B) -- every recorded player, the recorder's own included,
// gets a pawn this session PROVISIONS through the engine's own admission path, driven by a join
// descriptor authored from that player's PlayerRecord.  Nothing is ever adopted.
//
// Why the ledger carries `adopted` when nothing can increment it: INV-PP1 makes "the provisioner is
// the only source of pawns" a number a run can print rather than a paragraph a reader has to trust.
// A future arm that adopts cannot be added without moving that counter off zero in the report.
//
// ★ THE ADMISSION CALL IS EMITTED.  IDENT.ADMIT_USER_PROTO is promoted -- arity 3, decoded by
// callee_arity at BOTH of the insert's call sites -- so identity_access.cpp binds it as a typed
// Guarded<AdmitUserFn> that will not call the address until the argument moves at
// IDENT.ADMIT_USER.ARGMOVE_OFF still match the registry.  AdmitNotPromoted survives as the named
// refusal for that binding failing: a rebase, a patched image or a re-derivation that disagrees
// refuses by name instead of entering the insert with an undefined JoinRec pointer in R8.

#include <stdint.h>
#include <stddef.h>

#include "engine/entity.h"
#include "foundation/demofile/demotypes.h"
#include "generated/facts_generated.h"

// FORWARD DECLARATIONS, not includes.  identity_access.h includes THIS header, and the recorded
// reader lives in src/playback; both types below are only ever named through a reference inside a
// provider slot's signature, which needs no definition here.  pawn_provision.cpp includes both.
namespace replay {
namespace identity { struct RecordedLoadout; }
namespace identity_access {
struct LoadoutEngine;
enum class LoadoutRefusal : uint8_t;
enum class LoadoutPhase : uint8_t;
enum class LoadoutApplyGate : uint8_t;
}  // namespace identity_access
}  // namespace replay

namespace replay {

// Absence, at every slot and index field this header carries.  NETVAR.SLOT_NONE is the engine's
// own "no such slot" word, and a UserIndex, a UserSlot and a map entry all spell absence with it,
// so there is one name for it rather than a literal per field.
constexpr uint16_t kSlotUnmapped = uint16_t(facts::NETVAR_SLOT_NONE);

enum class PawnClass : uint8_t { Unknown = 0, Local, Remote };

// ABSENCE FOR AN ENTITY KEY, AND IT CANNOT BE 0.  Entity id 0 is the local pawn's by design
// (entity.h, EntityKeyOf) and the local pawn is the one a launch looks at, so a PawnRef carrying 0
// would mean "the viewer's own pawn" and "nothing was ever asked" at once.  EntityKeyOf answers in
// a uint16_t, so every key a surface can produce widens to at most UINT16_MAX and this word lies
// outside that range whatever the pools hold.
constexpr uint32_t kEntityKeyUnknown = UINT32_MAX;
static_assert(kEntityKeyUnknown > uint32_t(UINT16_MAX),
              "the unknown word must lie outside the range of a widened entity key");

struct PawnRef {                 // what a resolved pawn IS. 0xFFFF is absence at both handles.
    PlayerHandle player;         // the CI that carries pose      (0xFFFF if none)
    PhysHandle   phys;           // the CI that carries the body  (0xFFFF if none)
    uint32_t     entity;         // the engine entity key, or kEntityKeyUnknown -- never 0 for none
    PawnClass    cls;            // WHICH surface answered -- INV-PP2's subject
    bool         model;          // an NcaModel entry resolved, i.e. it is visible
};

struct JoinDesc {                // authored from PlayerRecord; never from the live session
    uint64_t xplatformId;        // JOINREC +0x00
    uint16_t team;               // +0x08   from PlayerRecord::team
    uint16_t role;               // +0x0A   spectator/moderator per E-I6
    bool     useSpawnPoint;      // true -> write the sentinel transform, engine picks the spawn
    float    xform[8];           // +0x10   only read when useSpawnPoint is false

    // WHAT THE ENTRANT MUST BE WEARING BEFORE ITS ACTOR IS BUILT.  L3 #18: the insert landed, the
    // actor step ran, and the engine then read customisation for an entrant whose loadout side was
    // empty -- `User has 0 loadout instances` -> `Offset is past the end of the stream`
    // (cmemstream.cpp:89) -> `Deadlock detected!` -> 0xC0000005.  The engine's own REMOTE admission
    // never reaches that state: the add-entrant REPLY carries the joiner's profile and loadout and
    // applies them, and only then does NETGAME.SET_PLAYER_ACTOR run.  These five fields are the
    // recording's answer to the same question, so the provision can run the same order.
    //
    // They are VIEWS, not copies: `Blob` is {const uint8_t*, uint32_t} over the loaded manifest,
    // and a JoinDesc is only valid while the PlayerRecord it was authored from is.  The
    // provisioner is armed and disarmed per load, which is that record's lifetime.
    Blob     entrantJson;        // PlayerRecord::entrantJson -- the recorded server profile
    Blob     unlocksJson;        // PlayerRecord::unlocksJson -- S9.3 breaker 2, carried for W2-C

    // ENG-BOTROUTE-FIX (Change A): the identity-block loadout document -- the SAME bytes the remote
    // route parses to one real instance (identity::ReadRecordedLoadout's `view.json`, reached through
    // env.recordedLoadout).  A DEDICATED channel, distinct from entrantJson: the manifest entrantJson
    // above parses to ZERO loadout instances, while this identity-block document parses to a non-empty
    // SOURCE (DIAG-BOTROUTE-PARSE).  Empty ({nullptr,0}) when the feature is off or the recording has
    // no present profile for this slot; HookSourceLoadout then falls back to entrantJson (today's safe,
    // defer-on-empty behaviour).  Like entrantJson it is a VIEW into the loaded recording, valid only
    // for the descriptor's own load lifetime.
    Blob     recordedLoadoutDocument;
    Symbol64 loadoutSlots[20];   // the recorded 168-byte SR15NetLoadout, ids only
    uint16_t loadoutNumber;
    uint16_t loadoutNumberBody;
    // PlayerRecord::alignment16, or kPlayerNoAlignment16 for a recording that never read one.
    // The dress ladder compares it against the live IDENT.CUSTOMISATION_ALIGNMENT before it
    // lets that value decide which slots the engine skipped.
    uint16_t alignment = kPlayerNoAlignment16;

    // THE RECORDED xplatform id, carried rather than consumed.  The authored `xplatformId` above
    // is synthetic and the live side is the only side that can see which ids are already in the
    // session, so the value the synthesis started from has to travel with the descriptor for the
    // provisioner to re-author it -- and for the census line to print the recorded id's own high
    // half beside the authored one, which is the only reading that tells a synthetic id from a
    // regression that hands the engine the recorded one (AUD-W2-P1-34 P1-40 F1).
    uint64_t recordedXplatId = 0;

    // THE DISPLAY NAME, AND IT TRAVELS BECAUSE THE SEED IS THE ONLY MOMENT IT IS EARLY ENOUGH
    // (NAMES-4).  The lobby member record this descriptor seeds is where CR15NetGame::
    // UserDisplayName falls back to, and the engine's own add-entrant handler runs the
    // participating transition -- which builds the in-world name tag -- inside the dispatch that
    // follows the seed.  A name written by the dress ladder afterwards reaches the roster and the
    // spectator UI, which re-read it, but not the tag above the body, which does not.
    // A COPY, not a view: it is written into the engine before the ladder runs, and 32 recorded
    // bytes are not guaranteed to be terminated.  Empty means the recording carried no name and
    // the seed leaves the record alone.
    char     displayName[33] = {};
};

namespace PawnProvision {
// ARCH-23 S5.  A two-state gate cannot express asked-pass / asked-fail / could-not-ask /
// never-asked, and L3 #11's `absent_this_session` merged two causes and cost a run.
enum class Refusal : uint8_t {
    None = 0,
    NotAttempted,          // the provisioner was never armed, so nothing was asked of anything
    NoNetGame,             // the session carries no CR15NetGame yet
    NoGameSpace,           // the netgame carries no game space, so no component system can be reached
    SessionNotInMatch,     // a DEFERRAL, not a failure: the state word is not NETGAME.STATE_INMATCH
    NoMemberRecord,        // INV-ID2: the lobby's own member count does not cover this slot
    SlotOutOfRange,        // slot >= ENTITY.USER_SLOT_TABLE_MAX
    SlotAlreadyOccupied,   // the (A) case: the engine's slot table has nothing free to map into
    AdmitNotPromoted,      // S3.4: the insert's prototype is unpromoted, so nothing was called
    InsertReturnedNoIndex, // the insert ran and answered 0xFFFF
    ReplayBand,            // the index the insert produced would be renamed "Replayer %u"
    ActorGuidAbsent,       // the entrant carries no SActorGUID, so nothing can be placed
    ProfileStillEmpty,     // the insert ran and the profile CJson is STILL empty
    BudgetExceeded,        // a DEFERRAL: this frame's provision.per_frame_max is spent
    SlotOccupancyUnreadable, // COULD-NOT-ASK, not asked-fail: no slot was proven free and at least
                             // one could not be read at all -- see SlotOccupancy below
    AdmitDidNotCreateUser,   // the insert answered an index and the engine's own entrant table
                             // does not name that slot back for it: no user record was created
    AdmitDidNotCreatePlayer, // the entrant exists and names the slot, but no actor component ref
                             // resolves for it, so the engine placed no pawn
    AdmitDidNotCreatePhysics, // the entrant, its actor and the engine's own actor step are all
                              // in place and no component instance answers for that actor, so
                              // there is no body to write a pose into
    RetryNotDue,             // a DEFERRAL: the blocker has not moved and the cadence's next pump
                             // has not arrived, so no lobby was walked and no insert was called
    ProfileNotApplied,       // L3 #18: the insert created the entrant and the recorded profile
                             // could not be installed on it, so the actor step was NOT called
    ProfileParseFailed,      // P1-16: the recording's own profile bytes are not json the
                             // engine's parser will take -- IDENT.JSON_PARSE_TEXT answered
                             // non-zero -- so no document was attached and the actor step
                             // was NOT called
    ProfileAttachFailed,     // P1-16: the document parsed and could not be put on the entrant:
                             // the profile field would not prove, the store did not read back,
                             // or one of the engine entry points the attach needs did not bind
                             // -- could-not-ask, never a silent skip
    LoadoutsEmpty,           // L3 #18's crash, refused by name: the entrant's engine-side loadout
                             // instance count is 0 (or its memory could not be proved), so the
                             // actor step was NOT called -- see the sentence in pawn_provision.cpp
    ActorAssetsNotReady,     // L3 #21's HANG, refused by name: NETGAME.REFRESH_PLAYER_ACTOR is a
                             // REFRESH, not a constructor -- its only two callers in the image are
                             // netgame message handlers that run it on an entrant that ALREADY
                             // holds the actor component instance.  Called on an entrant that
                             // holds none, from inside the engine's own per-frame update, it never
                             // returns: the main thread was still EXECUTING (no wait object of any
                             // kind on its stack) in the component walk under NETGAME.SET_PLAYER_
                             // ACTOR 11 s later, when the engine's watchdog printed
                             // "Deadlock detected!".  So the component instance is a PRECONDITION
                             // of the call, not only its postcondition.
    ReplyRouteNotPromoted,   // P1-19: the engine's OWN route to a driveable remote entrant is the
                             // NSLobbyEntrantAdded handler's remote arm, reached by publishing that
                             // message on the broadcaster the netgame registered the handler
                             // against.  This value means no engine address for that route is
                             // bound, so nothing was published and the caller falls back to the
                             // direct insert.  It is the ONLY value the fallback is taken on.
    ReplyWrongPhase,         // the route is bound and the caller is not in the phase the engine's
                             // own receive path publishes from, so nothing was published: L3 #21
                             // proved a component-system call issued from inside another engine
                             // update does not return.
    ReplyNotDispatched,      // the message was authored and the engine's publish entry did not
                             // take it, so no handler ran -- a could-not-ask, never a silent skip.
    ReplySeedRefused,        // the LOBBY RECORD the handler reads could not be seeded, so nothing
                             // was published: the message carries one qword and every other
                             // attribute of the entrant comes out of that record.

    // ---- P1-22, THE FOUR HOPS BETWEEN "Player Actor Allocating" AND A DRIVEABLE BODY ----
    // Each is the name of ONE engine record read back, and each is a DEFERRAL: the step is the
    // engine's own and the record that answers it becomes readable after the state-9 edge, so a
    // slot parked on one is re-asked once per cadence window for the life of the session.
    NotInRemotePlayerCs,   // the actor is not registered with the R14RemotePlayer component
                           // system.  NETGAME.SET_PLAYER_ACTOR registers it in its FIRST-
                           // ALLOCATION arm and nowhere else (one caller in the image), so this
                           // reading means the actor step either did not run or re-pointed an
                           // actor that was already there.
    ActorNotEnabled,       // L3 #23 F2, by its own name: the engine's dump reads
                           // "Actor : 0x... (disabled) (pooled)".  ENGINE.ACTOR_IS_ENABLED is
                           // the predicate that dump itself renders, and the enable is a flag
                           // CLEAR the engine performs on exactly one arm -- the bot arm's
                           // state-9 tail.  The remote arm never enables, so an entrant admitted
                           // through it stays disabled until this hop drives the same step.
    ActorNotPlaced,        // the actor is enabled and carries no world transform: the engine's
                           // place step (the spawn-point sentinel write that follows the enable
                           // on the same arm) has not run for it.
    ParticipationNotIssued,// every hop reads the engine's own record as done and THIS session
                           // never issued the participation that authored it, so the pawn is not
                           // this session's to claim.  S14.7's exit criterion in one refusal: a
                           // reads-only build, or one whose driver was gated off, provisions
                           // nothing however green its hops read.
    NoPhysicsCi,           // the last hop, and the only one nothing here can drive: the physics
                           // component instance is built by the R14RemotePlayer component
                           // system's OWN per-frame init, which requires ncaPhysics, ncaTransform,
                           // ncaModel, R15Body, R15NetPhysics and R15TouchInteract.  Registering
                           // the actor (hop 1) is what admits it; this hop waits for the engine.

    // L3 #25's CRASH, refused by name.  The engine's entrant table held ONE user -- the viewer's,
    // the one entrant this session did not create -- and the re-read settled a recorded slot onto
    // it, so the dress pass applied a FILE's loadout and profile to the live local user and the
    // engine's own loadout apply took an access violation at the state 8->9 edge.  A slot the
    // provisioner saw OCCUPIED before it asked for anything is never counted as ours, however the
    // engine's records answer for it afterwards: INV-PP1 forbids adopting, and the viewer is the
    // one entrant that can be adopted by accident.
    ViewerSlotExcluded,

    // Hop 4's two FAULTS, which are not the same answer as its wait.  NoPhysicsCi says the engine
    // has not built the instance yet; these two say this reader could not ask.  They are appended
    // rather than filed beside NoPhysicsCi so that no existing refusal changes its number.
    PhysicsCiUnreadable,      // the R14RemotePlayer component system, its element pool or the
                              // entrant's own CI element failed a Readable probe, so no
                              // requirement word and no init witness were read at all
    PhysicsCiIndexOutOfRange, // the entrant index is past the engine's own entrant count: the CI
                              // walk would be comparing a word that belongs to no user
    // The dress's own DEFERRAL, appended so no existing refusal renumbers.  It is not the retry
    // cadence: the hops all read done and the participation this session issued is only as old as
    // THIS pump, and S14.6 condition 2 forbids the loadout apply in the same frame as A3.
    LoadoutSameFrameAsParticipation,

    // THE ORDERING DEFECT, refused by name (DIAG-MODEL-SWAP).  The engine applies an
    // entrant's loadout off the SOCIAL ENABLE the participating arm issues, and an enable
    // that fires while the live table still reads `Loadouts : 0` refuses for want of
    // instances and is never re-run -- the entrant then wears nothing for the whole match.
    // So the publish half runs first and the hop chain is HELD, one cadence window at a
    // time, until the live count reads back.  A DEFERRAL: it does not latch, and it gives
    // up after kPublishGraceWindows rather than trading a bodyless pawn for no pawn at all.
    LoadoutNotPublishedBeforeEnable,
    // THE FOUR GATES IDENT.APPLY_LOADOUT_ITEMS CLOSES SILENTLY.  Each is a DEFERRAL, not a
    // verdict on the entrant: the enable is a one-shot the engine drives its loadout apply
    // off, so a window that would issue it into a body-less apply holds instead and asks the
    // slot again.  Named apart because they need four different fixes.
    LoadoutInstanceEmpty,
    LoadoutActorUnresolved,
    LoadoutCsPointerNull,
    LoadoutCompKeyInvalid,

    // The synthesis ran out of salts against the ids the live session already holds
    // (FACTS-VIEWER-ID64).  A FAILURE, not a deferral: admitting the entrant anyway is the one
    // outcome that evicts a live entrant, and the viewer is one of them.
    XplatIdCollision,

    // THE BODY NEVER INSTANTIATED (DIAG-MODEL-BUILD fix (b)).  The entrant is enabled and its
    // productive chassis apply was driven on an UNDRESSED record, but ComponentOnActor(model_.cs,
    // actor) still reads kNoComponent after kModelGraceWindows.  A DEFERRAL turned into a named
    // refusal at the bound: the model load is async (sub_140CC4B50 enqueues onto
    // CDeferredMethodQueue and the mesh arrives a few pumps later), so this is the difference
    // between "the load is still in flight" and "it will never arrive" -- never the silent
    // noModelOnActor RUN-36 read, which said nothing a run could act on.
    ModelNotInstantiated,

    // THE SETTLE DEFERRAL (DIAG-PARTICIPATION-FLAKINESS).  Every hop reads done -- hop 2's own
    // enable re-read among them, so the actor IS on -- while a re-seat booking is still
    // pending (the P1-47 settle latch is live).  The enable hop 2 just observed is the very one the
    // latch converts to Issued on the NEXT window's top read; latching ParticipationNotIssued here
    // (as the pre-fix gate did) killed the slot Failed between the bounce window and that booking,
    // so the latch never ran and no named outcome ever printed.  A DEFERRAL: keeps the slot
    // Awaiting so the latch reaches Issued, or -- if the enable truly regresses -- the budget in
    // Participate spends down to a NAMED ReSeatSettleExhausted.  Never a
    // silent Failed.  Distinct from ActorNotEnabled: the actor is enabled; it is the BOOKING that
    // has not landed.
    ParticipationSettling,

    // THE NO-AI GATE.  The engine's own remote-admission reply route is the ONLY
    // admission path the replay provisioner may take: AdmitIntoLobbySlot's fall-through -- the
    // direct insert it takes when that route is not promoted -- is IDENT.ADMIT_USER
    // (0x140152210 = AddBotUser), which ATTACHES AI (R14NetBot / RxAI).  On a solo offline host the
    // route can fail to promote (any of its four runtime binds null, or pawn.reply_route disarmed),
    // and the fall-through would then seat a type-6 AI bot into the owner's LIVE game.  So
    // ProvisionReplayUser refuses BY NAME the moment the route reads unpromoted and seats nothing --
    // the launch's bar is no-AI AND no-crash, and the direct insert clears neither here.  A
    // DEFERRAL, never a failure: a later pump may promote the route.  Distinct from AdmitNotPromoted
    // (the provisioner's hooks are unwired) and from ReplyRouteNotPromoted (the fall-through signal
    // INSIDE AdmitIntoLobbySlot, which this gate now stops the provisioner from ever reaching).
    DirectInsertSuppressed,
};
// The enum's own spelling, and the FIRST thing every refusal line carries.  L3 #15 F3: the log
// caps a line at kLogLineMax and the cap ate the disambiguating tail of a sentence, so the NAME --
// the half a reader greps for, and the half that binds the line back to this file -- goes in front
// of the prose where no cap can reach it.  RefusalText is "Name: sentence" for every value.
const char* RefusalName(Refusal);
const char* RefusalText(Refusal);
constexpr size_t kRefusalCount = size_t(Refusal::DirectInsertSuppressed) + 1;

// The two DEFERRAL blockers, answerable BEFORE an attempt is spent.  Provision asks exactly this,
// so a caller deciding whether to WAIT tests the same two conditions the attempt would -- and a
// caller that has to wait must not spend an attempt to find out, because a dress pass that reads a
// deferral as a refusal latches it for the whole load.
Refusal SessionBlocker(void* netGame);

// THE DEFERRAL SET, in one place because two readers now depend on the same answer.  A deferral is
// an attempt that is outstanding, not one that was decided: the provisioner leaves the slot queued
// instead of latching it Failed, and a caller that owns a retry -- the dress pass -- must keep
// asking for exactly these and stop asking for everything else.  Spelled here rather than twice,
// because a second copy that drifted would latch a slot in one reader and retry it forever in the
// other.
bool IsDeferral(Refusal);

// ONE OCCUPANCY PREDICATE, and this is it.  Three states, because "held" and "the surface would
// not answer" are different facts and L3 #13 printed sixteen of one when the other is what the
// session could support.  Both are treated as not-free -- mapping over a slot nobody proved empty
// is how the viewer's own pawn gets written through -- but only one of them means the lobby is
// full, and only one of them is worth growing the table over.
//
// FREE is the answer for a slot NO ENTRANT OCCUPIES, and a row that is simply ABSENT is how the
// engine's own table spells that: L3 #17 read sixteen absent rows on an EMPTY offline lobby as
// sixteen faults, so no slot could ever be proven free and `provisioned 1` became `provisioned 0`.
// UNREADABLE means the TABLE'S OWN MEMORY could not be proved -- a span proof refusal on the
// table -- and never a table that answered `nobody is in this slot`.
enum class SlotOccupancy : uint8_t { Free = 0, Held, Unreadable };

struct LobbyRecord {
    SlotOccupancy state = SlotOccupancy::Unreadable;
    uint64_t      peer  = 0;   // conn[slot]'s first qword: the peer id, or the free sentinel
};

// The engine's own free predicate, walked through the live lobby: CR15NetGame + NETGAME.LOBBY_OFF
// -> CNSLobby + LOBBY.OBJ_OFF -> conn[slot], whose first qword equals LOBBY.JOIN_FREE_SENTINEL
// exactly when sub_1406051C0 would pick that slot.  FALSE means the chain could not be walked at
// all -- neither free nor held -- and `out` is left untouched.
bool ReadLobbySlot(void* netGame, uint16_t slot, LobbyRecord& out);

// The engine reads AdmitIntoLobbySlot makes on the caller's behalf, so the decision itself is
// drivable offline.  `indexOfSlot` is the per-slot side table's word at IDENT.SLOT_TABLE_OFF and
// `slotOfIndex` is the slot that entrant's OWN record names back; both answer kSlotUnmapped when
// they cannot say. Admission never writes the side table's +0 word, so its
// zero-filled entries read "index 0 held" for every slot -- which is why the pair is a CROSS-CHECK
// used for one purpose only, excluding a slot a real entrant names back, and never as "held".
// THE MESSAGE THE ENGINE'S OWN REMOTE ADMISSION IS DRIVEN BY, and its whole layout.
//
// P1-19, in bytes: the netgame's lobby-message handlers are broadcaster LISTENERS
// (NETGAME.MSG_HANDLER_REGISTRAR_VA takes each handler's address beside the message name it hashes
// and calls EVENT.BROADCASTER_LISTEN on the node at NETGAME.BROADCASTER_OFF), and the handler
// registered for NSLobbyEntrantAdded is NETGAME.MSG_ENTRANT_ADDED_HANDLER_VA.  That handler is the
// REMOTE arm by DEFAULT: it takes the bot insert only when the session flag word says bot AND the
// entrant's lobby record carries account-id type 6, and every other type falls to
// NETGAME.ADD_REMOTE_USER_VA -- the arm that calls NETGAME.SET_PLAYER_ACTOR and attaches
// R14RemotePlayer and R14NetPhysics.  So the ENGINE builds the pawn and no REFRESH is issued here.
//
// The payload is ONE FIELD.  The engine's own two publish sites write a single qword -- the lobby
// slot, all-ones for none -- and pass MSG.ENTRANT_ADDED_SIZE as the byte count.  The ids, the
// profile document, the team and the account type are NOT in the message: the handler reads all of
// them out of the LOBBY record for the slot the message names, which is why seeding that record is
// a separate step of the route rather than a wider struct here.
struct ReplyMsg {
    uint64_t slot;
};
static_assert(sizeof(ReplyMsg) == size_t(facts::MSG_ENTRANT_ADDED_SIZE),
              "the authored payload is the engine's own: the trampoline the handler is registered "
              "behind returns WITHOUT calling the handler when the size it is given is under 8, so "
              "a struct that has grown or shrunk is a message the engine silently drops");
static_assert(offsetof(ReplyMsg, slot) == 0,
              "the slot is the first qword of the payload at both of the engine's publish sites");

// Author the message for one live slot.  kSlotUnmapped becomes the engine's own all-ones "none"
// rather than a slot number, so a caller that never mapped a slot cannot publish slot 0xFFFF.
ReplyMsg AuthorEntrantAddedMsg(uint16_t slot);

// The engine surface the reply route needs, and every entry is REQUIRED: a route missing one of
// them answers ReplyRouteNotPromoted and publishes nothing, which is what makes the direct insert a
// fallback rather than a silent default.
struct ReplyRoute {
    void* ctx = nullptr;
    // True only when every engine address the route publishes through bound against its registry
    // guard.  Null is not "promoted": a route nobody wired is a route nobody may take.
    bool    (*promoted)(void* ctx) = nullptr;
    // True only in the phase the ENGINE's own receive path publishes this message from.  L3 #21: a
    // component-system call issued from inside another engine update never returned, and the
    // engine's own publisher is a message handler, not a per-frame update step.  False publishes
    // nothing and refuses ReplyWrongPhase.
    bool    (*inReceivePhase)(void* ctx) = nullptr;
    // Put the recorded entrant into the LOBBY record for that slot -- ids, account type, team and
    // the recorded profile document -- because that record, not the message, is what the handler
    // reads.  Answers a refusal by name so "the recording would not parse" and "the lobby would not
    // take it" stay two facts.
    Refusal (*seedLobbyRecord)(void* ctx, uint16_t slot) = nullptr;
    // Publish the authored payload through EVENT.BROADCASTER_PUBLISH_LOCAL, on the node the handler
    // was registered against and under the message id the engine itself cached.  False means the
    // engine did not take it, and no handler ran.
    bool    (*dispatch)(void* ctx, const ReplyMsg& msg, uint32_t bytes) = nullptr;
};

struct AdmitHooks {
    // The engine's OWN route, and it is tried FIRST.  Null -- or a route that answers not-promoted
    // -- is the only condition on which the direct insert below is used at all.
    const ReplyRoute* reply = nullptr;

    // The direct insert is the engine's own bot body-build (AddBotUser), which unlike
    // the remote path applies the default profile+loadout and a visible default body itself.  So on
    // this path the recorded-profile dress is an OPTIONAL overlay: `attachProfile`/`applyProfile`/
    // `profileApplied` may be null (overlay off) or, when present, are attempted BEST-EFFORT and never
    // refuse -- a failed overlay must not discard the visible body.  The crash guard (`loadoutCount`)
    // and the readiness predicate (`actorAssetsReady`) still gate the actor step exactly as on the
    // remote path.  Default false leaves every existing caller on the full remote dress ladder.
    bool botBody = false;

    void* ctx = nullptr;
    uint16_t (*indexOfSlot)(void* ctx, uint16_t slot) = nullptr;
    uint16_t (*slotOfIndex)(void* ctx, uint16_t index) = nullptr;
    Refusal  (*admit)(void* ctx, uint16_t slot, UserIndex& out) = nullptr;
    // Called the instant the insert's postcondition confirms the engine created the entrant, which
    // is BEFORE the steps that can still defer. Optional; it is how a caller learns it seated a slot
    // on a pass that went on to return a deferral rather than None.
    void     (*seated)(void* ctx, uint16_t slot, uint16_t index) = nullptr;
    // THE ACTOR STEP, and it is the half of the engine's entrant admission the bot insert does
    // not carry.  sub_140152210 ("[NETGAME] Adding bot user to slot %llu, idx %hu") registers the
    // new user into R14PlayerBroadcaster, R14NetBot and RxAI and stops; the engine's REMOTE
    // entrant admission sub_1401539A0 ("[NETGAME] Adding remote user to slot %llu, idx %hu")
    // reaches the same insert's neighbour and then calls NETGAME.SET_PLAYER_ACTOR, which is where
    // R14RemotePlayer, R14NetPhysics, R14NetVoipReceiver and R15NetCustomization are attached.
    // `createActor` is NETGAME.REFRESH_PLAYER_ACTOR -- the engine's own two-argument wrapper over
    // that call, which reads the body template out of the lobby's own `bodytype` netvar for the
    // slot the entrant's record names, so the only thing this session authors is the user index.
    // `actorCi` answers the component-instance handle that step must leave behind, or
    // kSlotUnmapped when none does.
    bool     (*createActor)(void* ctx, uint16_t index) = nullptr;
    uint16_t (*actorCi)(void* ctx, uint16_t index) = nullptr;

    // THE DRESS, AND IT RUNS BEFORE `createActor`.  `applyProfile` installs the recorded
    // PlayerRecord's profile and loadout on the entrant the insert just created, through the same
    // two engine paths the dress ladder's steps 2 and 4 use (SetProfileJson, then ApplyLoadout on
    // the engine's own loadout instance).  It is not the dresser calling later: the ladder ran
    // AFTER provisioning, and provisioning is where the actor step lives, so the order the engine
    // requires cannot be expressed by reordering W2-C alone -- W2-C would still be downstream of
    // the call that faults.  Running it here makes the order structural: every caller of the
    // admission gets it, the Pump path included, and no caller can get the actor step without it.
    // THE ATTACH, AND IT RUNS BEFORE THE DRESS.  L3 #19: a bot entrant's profile document is
    // the json db CACHE's own (IDENT.PROFILE_CACHE_ATTACH_VA), its IDENT.JSON_CACHED_OFF qword
    // is non-zero, and both engine setters refuse every key of it while returning void -- so
    // `applyProfile` below cannot put anything on that entrant however it is called.  A real
    // remote join does not write keys either: it REPLACES the document pointer with the one the
    // reply carried (0x14019F613).  This hook does the same with a document parsed out of the
    // recording, and it answers with a REFUSAL rather than a bool because "the recording is not
    // parseable" and "the engine would not take the document" are two facts a caller has to log.
    Refusal  (*attachProfile)(void* ctx, uint16_t slot, uint16_t index) = nullptr;

    bool     (*applyProfile)(void* ctx, uint16_t slot, uint16_t index) = nullptr;

    // THE READ-BACK, and it is what DECIDES ProfileNotApplied.  `applyProfile` above answers
    // for the WRAPPER, and L3 #19 proved that answer is not the engine's: the engine's json
    // setters (IDENT.JSON_SET_STRING / IDENT.JSON_SET_INT) return VOID and refuse every key
    // of a document the json db holds cached -- 58 `$ json path: <key>: ERROR, json db is
    // cached, read only.` lines went by while the wrapper reported success, and the entrant
    // reached the actor step with a loadout container and no profile.  This hook asks the
    // ENGINE whether its own document now carries the recorded profile: the cached qword at
    // IDENT.JSON_CACHED_OFF that the setters themselves test, and then the engine's own
    // reader at a key the recording wrote.  False refuses ProfileNotApplied and the actor
    // step is not called.
    bool     (*profileApplied)(void* ctx, uint16_t slot, uint16_t index) = nullptr;

    // THE RECORDED-LOADOUT SOURCE FILL, bot path only, and it RUNS AFTER `admit` AND BEFORE
    // `publishLoadout`. AddBotUser's GenerateBotProfile CLEARS and reparses the
    // per-user loadout SOURCE container during the insert (sub_14016C140 -> IDENT.LOADOUT_FROM_PROFILE
    // on CR15NetGame + record + 0x4CC0), so a SOURCE pre-fill done before admit does not survive.  This
    // hook parses the RECORDED player's loadout into that SOURCE after the insert has finished clobbering
    // it, so the SOURCE->LIVE copy below carries the recording's real instances (not the bot generator's
    // empty default that stalled echo-98).  Best-effort: a parse that does not take leaves the count at
    // 0 and the guard defers exactly as before -- it never discards the visible body.  Null unless the
    // bot-source experiment is armed; only consulted for botBody.
    bool     (*sourceLoadout)(void* ctx, uint16_t slot, uint16_t index) = nullptr;

    // THE DEFAULT-LOADOUT PUBLISH, bot path only, and it RUNS BEFORE the crash guard below.
    // AddBotUser (`admit`) calls GenerateBotProfile, which writes a default
    // profile+loadout into the entrant's USER RECORD container -- but what the crash guard and the
    // engine's own actor step read is a DIFFERENT container, the per-slot live loadout table.  The
    // engine bridges the two with IDENT.LOADOUT_ASSIGN inside its add-entrant handler, ABOVE the
    // insert; a session that calls the insert directly never reaches that copy, so the per-slot count
    // stays 0 and `loadoutCount` below refuses LoadoutsEmpty with the actor step never called.  This
    // hook runs that same copy for the default loadout the insert already generated, so the count is
    // non-empty at the guard.  Best-effort: a copy that does not take leaves the count at 0 and the
    // guard defers exactly as before -- it never discards the visible default body.  Null on the
    // remote path (its dress ladder publishes through `applyProfile`) and only consulted for botBody.
    bool     (*publishLoadout)(void* ctx, uint16_t slot, uint16_t index) = nullptr;

    // The engine's OWN loadout-instance count for that slot -- CR15NetGame +
    // IDENT.LOADOUT_TABLE_OFF + (slot << IDENT.LOADOUT_ENTRY_SHIFT) + IDENT.LOADOUT_COUNT_OFF.
    // This is the number the engine prints as `Loadouts : N` in its entrant block and the bound
    // IDENT.APPLY_LOADOUT_ITEMS walks the instance array against; L3 #18 printed it as 0 and then
    // faulted.  It answers 0 when the count is zero AND when the table's memory could not be
    // proved, because both mean the same thing to the only caller: do not ask for an actor.
    uint64_t (*loadoutCount)(void* ctx, uint16_t slot) = nullptr;

    // THE READINESS PREDICATE FOR THE ACTOR STEP, and it is read off the ENGINE, before the call.
    // L3 #21 hung here with no wait primitive anywhere on the main thread's stack -- the thread was
    // running, deep in the component walk NETGAME.SET_PLAYER_ACTOR drives, and the engine's own
    // watchdog ended the process.  A call that can not return may not be issued speculatively from
    // a tick, so the one contract its own callers satisfy is tested first: NETGAME.REFRESH_PLAYER_
    // ACTOR has exactly two code xrefs in the image, both netgame message handlers reacting to a
    // bodytype change on an entrant that is ALREADY a remote player, and `actorCi` is the surface
    // that says whether this entrant is one.  False refuses ActorAssetsNotReady, no engine call is
    // made, and the deferral gate asks again on a later tick -- the game thread is never held.
    bool     (*actorAssetsReady)(void* ctx, uint16_t index) = nullptr;
};

// Admit one entrant into one live slot: gate on the lobby predicate above, call the engine's
// insert, then ASK THE ENGINE WHAT IT CREATED.
//
// L3 #15 F2 settled what a successful LOCAL entrant leaves behind, and it is not a connection
// record.  sub_1406051C0 -- "[NSLOBBY] attempting to accept player for add entrant from %s" -- is
// the ONLY writer of conn[slot]'s peer qword (`*(_QWORD *)(connArray + 40 * slot) = *a2`, from a
// join request carrying a peer address), and nothing in the image calls it directly: it is the net
// layer's add-entrant handler.  The insert this plugin calls IS the engine's own bot/local-entrant
// path -- sub_140152210 opens by logging "[NETGAME] Adding bot user to slot %llu, idx %hu" -- and
// it never touches the connection array: it READS the lobby (members[slot] + 0x8C, through
// sub_140608350) and writes the NETGAME's own tables.  A postcondition keyed on conn[slot]
// therefore refuses every correct local admission, which is what L3 #15 printed.
//
// What the insert DOES write for a local entrant, and what this reads back instead:
//   * the entrant count at +0xDC is incremented and user[index].word & IDENT.USER_SLOT_MASK names
//     the slot -- `slotOfIndex` -- else AdmitDidNotCreateUser;
//   * the actor component ref built from user[index] + IDENT.USER_ACTOR_GUID_OFF resolves and
//     answers OUR index for that slot -- `indexOfSlot` -- else AdmitDidNotCreatePlayer.  The insert
//     places the actor in its `cmp dword ptr [rdi], 9` arm at 0x14015269A, and state 9 is what the
//     caller has already gated on, so this is a postcondition and not a race.
// Neither is ever SlotAlreadyOccupied, which is a statement about somebody else.  `out` carries an
// index only when None is returned.
Refusal AdmitIntoLobbySlot(void* netGame, uint16_t slot, const AdmitHooks&, UserIndex& out);

// THE ENGINE'S OWN PATH TO A DRIVEABLE REMOTE ENTRANT, and it makes no component-system call of its
// own: it seeds the lobby record, authors the NSLobbyEntrantAdded payload for the mapped slot and
// hands it to the engine's publish entry, and the ENGINE then performs the insert, the profile
// attach and the actor build on its own stack, exactly as it does for a network peer.  The
// postconditions are the ones already built -- the entrant table names the slot back, and the actor
// component instance the R14RemotePlayer system registers exists -- so success is read off the
// engine and never off the fact that a publish returned.
//
// ReplyRouteNotPromoted is the ONE answer the caller may fall back on; every other answer is this
// route's verdict and is returned as it stands.
Refusal ProvisionViaReply(void* netGame, uint16_t slot, const AdmitHooks&, UserIndex& out);

// ---------------------------------------------------------------------------------------
// THE ACTOR HOP CHAIN (P1-22).  What an admission leaves behind is an ENTRANT; what a replay
// needs is a BODY, and L3 #23 measured the whole distance between them in one engine line:
// `Actor : 0xEF05DD95418C9973 (disabled)  (pooled)`.  These are the hops that line is short of,
// in the ENGINE'S OWN ORDER, each named by the record that answers it.
//
// Where the order comes from, and it is one function: sub_140152210's `cmp dword ptr [rdi], 9`
// tail is the only site in the image that ENABLES a player actor (it is the sole caller of the
// enable at 0x1404EDA40, from 0x1401526AE) and the only site that PLACES one (sole caller of
// 0x140519BF0, from 0x14015272D), and it registers the entrant with its component systems before
// both.  The REMOTE arm sub_1401539A0 allocates the actor through NETGAME.SET_PLAYER_ACTOR and
// registers it (sub_140CB89B0, 0x1401B15D8 -- again the only caller in the image) and then does
// NEITHER of the other two.  Our session takes the remote arm, which is why its actor is
// allocated, pooled and disabled: the two steps it is missing are exactly the two this chain
// drives, and the fourth hop is the one only the engine's own per-frame update can do.
enum class Hop : uint8_t { RemotePlayerCs = 0, Enabled, Placed, PhysicsCi, Done };
const char* HopName(Hop);

// The gate verdict is latched at publish time and re-read every window from
// AdvanceSlot as the actor side comes up.  A re-read that came back Unasked -- no netgame this
// window, an unresolved instance name, an unbound predicate, memory that did not read back --
// must NOT erase a NO the publish half already took: a question that could not be put a second
// time is not a yes.  Adopt the re-read only when it actually answered, or when there was no prior
// answer to preserve.  Exposed so the must-fail control can pin it without the AdvanceSlot harness.
identity_access::LoadoutApplyGate MergeGateReread(identity_access::LoadoutApplyGate prior,
                                                  identity_access::LoadoutApplyGate reread);
// Whether this entrant needs a profile document AUTHORED from its recorded pairs.
// Four separate nos, each of which has to be able to fail on its own: the switch
// (playback.dress_loadout) is off; the recorder captured a real document, which always wins over
// one of ours; the block recorded no slot keys, so a pair is a position and a position is a guess;
// or the count is the could-not-ask value or zero, which is an entrant wearing nothing rather than
// an entrant we may dress.  Exposed so the gate can be pinned without a live engine, which the
// authoring itself needs (the document is built by the engine's own JSON parser).
bool ShouldAuthorDocument(bool enabled, bool documentRecorded, bool hasSlotKeys,
                          uint16_t filledPairCount);

// The refusal that names an outstanding hop.  Hop::Done has none and answers Refusal::None.
Refusal HopRefusal(Hop);
constexpr size_t kHopCount = size_t(Hop::Done) + 1;

// WHY hop 4 has not happened, in the engine's own vocabulary.  CR15RemotePlayerCS::InitCI
// (ENTITY.REMOTE_PLAYER_CS_INITCI_VA, 0x140CF74F0) asks six sibling component systems for this
// actor's instance index, in THIS order, writes each answer into the RemotePlayer CI element, and
// returns early on the first that answers 0xFFFF -- the return that raises
// "R14RemotePlayer requires <name>" at cr15remoteplayercs.cpp:328/332/336/340/344/348.  So the CI
// element records which requirement the engine last refused on, and a reader can name it.
//
// NotAttempted is the third state and not a synonym for None: the CI carries no requirement word
// reading 0xFFFF AND no completed init, i.e. the engine never ran InitCI over this actor at all.
enum class PhysicsCiReq : uint8_t {
    None = 0,          // every requirement met -- the init completed
    NcaPhysics,        // CI+0x0C, checked first
    NcaTransform,      // CI+0x04
    NcaModel,          // CI+0x08
    R15Body,           // CI+0x10
    R15NetPhysics,     // CI+0x1C
    R15TouchInteract,  // CI+0x20, checked last
    NotAttempted,      // no requirement word is unset and no init completed: InitCI never ran --
                       // which includes the component system holding no element for this entrant
    Unreadable,        // the component system, its element pool or the element itself did not
                       // survive a probe.  A COULD-NOT-READ, and never a wait: the element the
                       // pool allocates is memset to zero, so an unprobed read of freed or
                       // unmapped memory would answer "not attempted" with total confidence
    IndexOutOfRange,   // the entrant index is past the engine's own entrant count, so the word a
                       // walk would compare against is not a user's
    Unknown,           // nothing is wired to read the CI, so the question was never asked
};
const char* PhysicsCiReqName(PhysicsCiReq);
// Hop 4 does not answer with one refusal.  A requirement the engine is still meeting is a WAIT
// (NoPhysicsCi); a CI this reader could not probe, or an index past the entrant table, are faults
// and must not be reported as though the engine were making progress.
Refusal PhysicsCiRefusal(PhysicsCiReq);

// The place is the one step in the chain whose postcondition the engine keeps NOWHERE: vtable slot
// 119 (ENGINE.ACTOR_PLACE_VSLOT 0x3B8) runs the transform straight into each component system the
// actor is already an instance of and sets no per-actor bit, so a chain that re-asks "is it
// placed?" gets the same answer forever.  Re-issuing on that answer would drive the engine's place
// over playback's own pose on EVERY frame.  This gate is what makes the driver safe to bind before
// the read-back exists: the place is issued at most once per epoch, and never at all without a
// gate -- a null gate cannot be latched, so it refuses the drive rather than repeating it.
// The recorded role a slot has not been given one for, and the roster's own "never recorded"
// fill.  They are one value: a DriveGate that has issued nothing and a player the recording never
// carried a team for are both "no role here", and spelling them apart would invent a distinction
// the capture side does not make.  pawn_provision.cpp static_asserts this against the roster's own
// constant -- the header keeps the engine layer off capture's headers, and the .cpp proves it.
constexpr uint16_t kRoleNotIssued   = uint16_t(facts::NETVAR_SLOT_NONE);
constexpr uint16_t kRoleNotRecorded = kRoleNotIssued;

// The engine's OWN spectating predicate, not our enumeration of the roles it happens to be true
// for.  NETGAME.SET_PLAYER_ACTOR and NETGAME.USER_SETTINGS_APPLY both dispatch on
// `((role - bias) & mask) != 0` -> participating, else spectating; a hand-written {2, 4} would be
// a transcription of today's answer rather than the engine's question.
inline bool RecordedAsSpectatorRole(uint16_t role) {
    return uint16_t((role - uint16_t(facts::NETGAME_USER_ROLE_SPECTATING_BIAS)) &
                    uint16_t(facts::NETGAME_USER_ROLE_SPECTATING_MASK)) == 0;
}

// A recorded role the engine's dispatch gives a body: recorded, inside the role bound, and not
// spectating.  It is the recorded value of the entrant's `spawned` record.
inline bool RoleParticipates(uint16_t recordedRole) {
    return recordedRole != kRoleNotRecorded &&
           recordedRole < uint16_t(facts::NETGAME_USER_ROLE_BOUND) &&
           !RecordedAsSpectatorRole(recordedRole);
}

// Whether ANY model-swap drive (the equipment RECV, the pooled-swapper reset, BeginModelSwap) may
// run for an entrant.  A recorded spectator had no body, and an entrant whose recorded chassis is
// the unfilled-pair sentinel has no target but the pooled actor's previous occupant.
inline bool SwapDriveAllowed(uint16_t recordedRole, bool chassisUnfilled) {
    return !RecordedAsSpectatorRole(recordedRole) && !chassisUnfilled;
}

// WHICH engine call authored an issue.  L3 #29b: the participation arm that found the role word
// already equal to the recorded one booked `Issued`, latched the once-per-epoch gate and called
// nothing at all -- the engine's own auto-pick had set the word, so the postcondition it reported
// success off had been true before this session touched anything.  `Issued` now requires an engine
// call this session made AND a postcondition that flipped, and this says which call, because
// "we assigned the role" and "the role already matched, so we re-ran the engine's own dispatch"
// are two different drives over the same word and a census may not print them as one.
// NETGAME.REFRESH_PLAYER_ACTOR's own answer, which is NETGAME.SET_PLAYER_ACTOR's return and the
// single reading that says WHICH of the two things happened inside it.  sub_1401B0D80 opens by
// resolving the entrant's SActorGUID to a live actor and reading that actor's body-template key
// out of its own table -- `v10 = *(*(*(v7+880)+184) + 8 * *(u16*)(ng + 592*idx + 392))` at :86 --
// and compares it to the key NETGAME.PICK_BODY_TEMPLATE just derived from the entrant's team:
//
//     :90   if ( v10 == v76 )
//     :91     return 0;                 <-- nothing runs.  No allocate, no dispatch, no enable.
//
// Everything the refresh exists to drive lives below that line: the re-dispatch at :392-395
// (`if ( ((v50 - 2) & 0xFFFD) != 0 ) sub_1401C9BA0(...)`) is inside `if ( v76 != -1 )` at :316,
// which is inside the `return 0` the equal key takes.
//
// BUT THAT IS NOT THE RETURN WE SEE.  The bound call is the WRAPPER sub_14019F1C0, and only its
// non-zero answer is sound: the zero path jumps straight to the epilogue, so non-zero requires
// :407, while the success path overwrites the return register with
// `(*(u8*)**(u64**)(ng + 0x2DA0)) >> 2` at 14019F200-223.  A wrapper 0 is therefore `:91` OR a
// completed allocate-and-dispatch under a clear flags bit.  The arm treats a 0 as a CANDIDATE
// and decides it with a two-read of the entrant's SActorGUID, which sub_1401B0D80:319 is the
// only writer of on this path.  Discarding the answer altogether is what left L3 #31's three
// refreshes unexplained for two packets; believing it whole would retire the cheap arm and free
// a LIVE actor on a window where nothing was wrong (AUD-W2-P1-34 MAJOR-1).
enum class RefreshAnswer : uint8_t {
    Unavailable,   // the call was NOT issued -- unbound, unresolvable VA, index out of range
    Declined,      // issued and the WRAPPER returned 0.  A CANDIDATE and never a fact on its
                   // own -- sub_14019F1C0 rewrites its own return on the success path -- so it
                   // is corroborated by the SActorGUID two-read before anything acts on it
    Redispatched,  // issued, wrapper returned non-zero.  Sound in this direction only: the
                   // zero path jumps to the epilogue, so non-zero requires :407 and :392-395
};

// A4 -- what NETGAME.SPAWN_NOTIFY (sub_1401CAC00) hands back.  The engine call returns void, so
// `issued` is the only thing the hop can say about the drive itself; the caller re-reads the
// model CI before and after to see whether it actually installed one.  `modeWord` is the session
// mode word the handler branches on (**(ng + NETGAME.UIFLAGS_PTR_OFF)), read for the
// DIAG-SPAWN-MODEL-CI section-4 branch probe: bit1 set, or bit2 clear and bit6 set, selects the
// host arm (no model install); otherwise the else-if remote arm installs the ncaModel/swapper CI.
// `modeRead` says whether that read succeeded -- an unread mode word is a could-not-ask, never a
// verdict about which arm ran.
struct SpawnNotifyResult {
    bool     issued = false;
    bool     modeRead = false;
    uint64_t modeWord = 0;             // the HONEST session-authority word, read before the flip
    // BLOCKER-2 (DIAG-REPLYPHASE section 3): the Stage-B authority flip.  `authFlipApplied` says the
    // hop transiently spoofed non-host around the UserSpawned call so it took the REMOTE install arm;
    // `modeWordEffective` is the value the engine actually branched on (modeWord | 0x4 when flipped);
    // `spawnFaulted` says the engine call raised and was contained by SEH (the word was still restored).
    bool     authFlipApplied = false;
    uint64_t modeWordEffective = 0;
    bool     spawnFaulted = false;
};

enum class ParticipateDrive : uint8_t {
    None = 0,
    AssignTeam,
    RefreshPlayerActor,
    AssignTeamReSeat,
    // The entrant add itself.  This session wrote `spawned` into the slot BEFORE it issued the
    // add, so the add's own role transition (65535 -> recorded team) is what enabled the actor.
    SeededEntrantAdd,
};
const char* ParticipateDriveName(ParticipateDrive);

struct DriveGate {
    uint32_t epoch = 0;                  // the provisioner's arm epoch, written by the caller
    uint32_t issuedEpoch = UINT32_MAX;   // the epoch a transition was last ISSUED in
    uint32_t issues = 0;                 // transitions issued for this slot, over the session
    // The engine call the last issue was made with.  None while nothing has been driven.
    ParticipateDrive via = ParticipateDrive::None;
    // Windows the re-dispatch arm spent WITHOUT calling the engine -- the one-pump ordering
    // deferral and the readiness guard's no.  Both can come good later (the component instance is built by
    // the RemotePlayer CS's own per-frame init), so the arm asks again next window rather than
    // giving up on the first no; it is bounded because a deferral nobody ever stops making is a
    // shrug.  PER-ARM, not per-session: Arm() rebuilds every slot, gate included, so a level load
    // starts a fresh budget.
    uint16_t refreshAttempts = 0;
    // Refreshes actually ISSUED, and a SEPARATE, far smaller budget -- the two arms do not cost
    // the same thing.  A deferral costs nothing at all; NETGAME.REFRESH_PLAYER_ACTOR calls
    // NETGAME.SET_PLAYER_ACTOR UNCONDITIONALLY, which allocates a player actor from a body
    // template, attaches seven components and re-registers it with the RemotePlayer CS.  Spending
    // a 64-window budget on those is not a retry, it is an actor rebuild every second on the very
    // run this arm exists to make readable.
    uint16_t refreshIssues = 0;
    // The recorded role the last issue was authored for, or kRoleNotIssued when none has been.
    // A transition is never issued without one, so this doubles as the record of WHICH role the
    // engine was asked to assign -- the census can say that, and it cannot guess it.
    uint16_t issuedRole = kRoleNotIssued;
    // Re-seats ISSUED -- the two-leg role bounce through IDENT.SERVER_ASSIGN_TEAM.  Each leg is a
    // real team transition the engine broadcasts and counts, so this budget is the tightest of the
    // three: the dispatch enables synchronously off the current role word, so a re-seat that did
    // not enable will not enable on the second try either.
    uint16_t reSeatIssues = 0;
    // A re-seat was issued and its postcondition has NOT been read yet.  The read happens on a
    // LATER pump than the call, so `Issued` is decided on a fresh ACTOR_IS_ENABLED and never on
    // the assign's return value.
    bool reSeatPending = false;
    // Windows a pending re-seat has waited for its async enable to land.  The engine
    // enables the re-seated actor a tick or more AFTER the call, so the pending flag
    // and the observed enable fall in different windows; the flag is HELD across kReSeatSettleWindows
    // rather than consumed in the first window that reads the actor still off, and this counts that
    // wait.  Cleared on success or on exhaustion (DIAG-PARTICIPATION-REGRESSION).
    uint16_t reSeatSettle = 0;
    // The engine's OWN answer to the last refresh we issued, kept because it is what selects the
    // next arm.  A Declined is the runtime proof that sub_1401B0D80:90-91 is the gate in the way,
    // and the role bounce is admissible ONLY once we have been told that.
    //
    // There is deliberately NO arm that frees the actor to get past that gate.
    // NETGAME.SET_PLAYER_ACTOR(ng, index, -1) releases the actor but leaves the entrant's
    // SActorGUID naming it (:342 is that word's only writer and it is on the allocate path), so
    // the refresh that follows reads the same key and declines again -- 4 of 4 across L3 #35 and
    // LAUNCH-GAMETYPE-FIX.  The engine's next frame then poses every entrant whose role is below
    // NETGAME.USER_ROLE_BOUND: sub_140D89DD0 resolves the guid to a model and calls the bone
    // getter sub_1403C4470 on the miss without a null test -- the 0xC0000005 read of +0xC8.
    RefreshAnswer lastRefresh = RefreshAnswer::Unavailable;
    bool refreshDeclined = false;
};

// --- ARCH-23 S14: THE ENTRANT'S PARTICIPATION, WHICH IS THE THING THE ENGINE IS MISSING --------
//
// S14.0 refuted the premise the old chain drove: NETGAME.SET_PLAYER_ACTOR already CREATES and
// CS-REGISTERS a remote entrant's actor, and the engine's own next act is a two-way dispatch on
// the entrant's packed role word -- participating, which enables and applies the chassis, or
// spectating, which does neither.  `(disabled) (pooled)` is the engine correctly declining to give
// a pawn to an entrant it has been told is not participating, or one whose `spawned` record is
// absent.  So the chain stops driving the ACTOR and drives the ENTRANT, in the engine's own order:
//
//   A2  write the `spawned` record, on the side the engine's OWN reader selects
//   A3  IDENT.SERVER_ASSIGN_TEAM with the RECORDED role
//
// and the engine does create / register / enable / chassis / CI itself, with arguments it derives.

// A0's partition, in three answers rather than two.  An entrant BELOW the netgame's local-user
// count is a local user whose pawn the engine's own first block already owns; one at or above it is
// ours to author for; and a count that would not prove readable is NEITHER -- and must not be
// spelled as "not a local user", because the arm behind that answer now calls the engine.
enum class LocalUserAnswer : uint8_t { Remote = 0, Local, Unreadable };

// What A2's write did.  Four states, because "the record is already set" and "the record cannot be
// written" are different answers and neither is a failure to ask.
enum class SpawnedWrite : uint8_t {
    Issued = 0,   // the engine's setter was called for this entrant
    AlreadySet,   // the consumer's own read already answered true, so nothing was written
    Unwritable,   // the per-slot side is selected and the slot is past the store's slot count
    Unavailable,  // nothing is bound to write it -- a could-not-ask, never a skip
};

// Three states, never a bool: an issue, a skip and a refusal are three different things and only
// the first one may ever be counted as a provision (S14.7 test 12).
enum class Participation : uint8_t { Issued = 0, SkippedAlreadyDone, Refused };

// WHY a participation refused, in S14.5's own vocabulary.  Every one of these is a name a census
// line can print; none of them is a shrug.
enum class ParticipateRefusal : uint8_t {
    None = 0,
    NoDriver,                 // never-asked: no participation driver is bound at all
    NoGate,                   // no gate was handed in, so nothing may be issued -- by design
    Reentrant,                // our own "inside an engine call" latch was already held
    EntrantIsLocalUser,       // index < the netgame's local-user count: the engine's first block
                              // already owns that entrant's pawn, so nothing is authored for it
    TeamNotRecorded,          // the recording never carried this player's team
    TeamOutOfRange,           // the recorded role is not one the engine accepts
    RecordedAsSpectator,      // the engine's own spectating predicate is TRUE for the recorded
                              // role.  That player had no pawn in the recording and gets none now
    SpawnedRecordUnwritable,  // A2's per-slot side, past the store's own slot count.  The engine's
                              // per-slot setter computes store[864] + 216*slot with NO bounds
                              // check, so an unguarded write there is an out-of-bounds store into
                              // the live session document
    SpawnedRecordNotWritten,  // A2 was issued and its OWN consumer still reads 0, so A3 is not
                              // issued: the engine would enable nothing and the call would lie
    AssignTeamNotApplied,     // A3 was issued and the engine's packed word did not take the role
    ViewerIndexRefused,       // A0's partition COULD NOT BE ASKED: the netgame's local-user count
                              // would not prove readable, so whether this index is the viewer's
                              // own is unknown.  It fails CLOSED, because the arm behind it now
                              // issues NETGAME.REFRESH_PLAYER_ACTOR -- which does no index check
                              // of its own and rebuilds a player actor
    ParticipationNotReRun,    // the live role already equals the recorded one -- the engine's own
                              // auto-pick got there first -- so A3 would be a no-op, and nothing
                              // is bound that can re-run the engine's dispatch and prove it:
                              // NETGAME.REFRESH_PLAYER_ACTOR, its readiness guard, or the enable
                              // re-read.  This session drove NOTHING and says so
    RefreshDeferredToNextPump,// the `spawned` record was authored on THIS pump and the engine
                              // reads it from inside the refresh, so the refresh goes on a later
                              // one.  A DEFERRAL: the gate is not latched and the next window asks
    ActorAssetsNotReady,      // the readiness guard read false BEFORE the call, so no engine call
                              // was made.  L3 #21 issued this refresh on an entrant holding no
                              // actor component instance and the main thread never came back
    ActorNotEnabledAfterRefresh,  // the refresh was issued and the engine's own ACTOR_IS_ENABLED
                              // still reads false, so the dispatch did not enable the actor.  The
                              // call's return value is never the verdict; this re-read is
    RefreshAttemptsExhausted, // the deferral's bound: kRefreshAttemptMax windows asked and the
                              // arm never reached an enable, so it stops asking and stops calling
    ReSeatLegNotApplied,      // the re-seat's OUTBOUND leg did not take: the engine refused the
                              // away role or its packed word did not read it back.  Nothing was
                              // moved, so there is nothing to put back
    ReSeatNotRestored,        // the outbound leg took and the leg BACK did not, so the entrant is
                              // sitting on a role the recording never gave it.  Said by name and
                              // never papered over; the next window's live role no longer matches
                              // the recorded one, so the ordinary A3 assign re-seats it
    ReSeatAwaitingReRead,     // the re-seat was issued and its enable postcondition is read on a
                              // LATER pump.  A DEFERRAL: the gate is not latched and the next
                              // window either books the issue or falls back to the refresh
    RefreshDeclinedKeyUnchanged,  // the refresh was issued, the wrapper answered 0 AND the
                              // entrant's SActorGUID did not move across the call.  It is the
                              // PAIR of readings that names sub_1401B0D80:90-91: the actor
                              // already carries the body template its team asks for, so the
                              // function returned before the allocate at :319 and before the
                              // dispatch.  The wrapper's 0 alone cannot say that (it clobbers
                              // its own return on the success path), which is why the guid is
                              // read on both sides.  This -- not the return -- is what makes
                              // the role bounce admissible next window
    BounceUnsafeForMultipleEntrants,  // the last-resort role bounce was NOT issued.
                              // IDENT.SERVER_ASSIGN_TEAM is a team-SLOT allocator: :198-217 frees
                              // the entrant's live Team Slot into its old team's heap and :227-267
                              // pops a new one off a count that the away leg just depressed, so
                              // the leg back lands on count[T]-1.  That is the entrant's own slot
                              // only when it was the sole member; with two or more it lands on a
                              // slot another player holds.  Team Slot is recorded (roster.h:200),
                              // printed by the engine's entrant dump and keys the team spawn
                              // point, so the bounce is refused unless this session provisions
                              // exactly one entrant.  A count of 0 means we were not told, and
                              // that fails closed under this same name
    TeamSlotChangedByBounce,  // the bounce ran with the count that permits it and the Team Slot
                              // read back after the leg home differs from the one read before the
                              // leg away.  The entrant is on the right team wearing a slot the
                              // recording never gave it -- said by name, never counted as Done
    RefreshDeclineNotCorroborated,  // the wrapper answered 0 and the SActorGUID two-read that
                              // tells `:91` from a completed rebuild could not be taken: the
                              // hop is unbound, or a read failed on one side.  COULD NOT ASK,
                              // and it is a third word on purpose -- a gate this one-way (it
                              // retires the cheap arm for the epoch and admits the bounce)
                              // may not latch off a reading we never got.
                              // Nothing is latched, the next window re-asks
    ActorEnabledSoNotFreed,   // the refresh was NOT issued: the engine's own ACTOR_IS_ENABLED
                              // read TRUE at the call site.  The refresh rebuilds the player
                              // actor and can take NETGAME.SET_PLAYER_ACTOR's `Player Actor
                              // Freeing` arm; destroying an enabled actor takes away a body the
                              // engine is already rendering.  Read at the call rather than
                              // inherited from the window's entry read
    ReSeatSettleExhausted,    // a re-seat was issued and its enable postcondition never read true
                              // within kReSeatSettleWindows of holding the pending latch.  The
                              // engine's dump shows the re-seat DOES enable the actor eventually,
                              // so this is a settle budget too small or an enable that never landed
                              // -- a NAMED give-up, never the silent flag-drop that hid it before
};
const char* ParticipateRefusalName(ParticipateRefusal);
constexpr size_t kParticipateRefusalCount =
    size_t(ParticipateRefusal::ReSeatSettleExhausted) + 1;

// How many windows the refresh arm may spend on deferrals that CALL NOTHING.  The census cadence
// is roughly one window a second and L3 #29b's run held 294 of them, so this is a wait measured in
// a minute, not a spin: long enough for the RemotePlayer CS to build a component instance, short
// enough that a run reading `provisioned 0` is told why inside the match.
constexpr uint16_t kRefreshAttemptMax = 64;

// How many refreshes may actually reach the engine, and it is derived from what the call DOES
// rather than from the window budget.  One issue is the whole fix: the dispatch re-runs off the
// current role word and enables synchronously, so if the enable was going to happen it happened on
// the first call.  A second and a third exist only to cover a component instance that finished
// between windows.  A fourth would be conceding that this call is not the thing that enables --
// and each one is a player-actor rebuild, not a probe, so the arm stops instead of churning.
constexpr uint16_t kRefreshIssueMax = 3;
static_assert(kRefreshIssueMax < kRefreshAttemptMax,
              "the call budget must be strictly tighter than the no-call budget, or the split "
              "does nothing and every deferral window becomes an actor rebuild");

// How many role bounces may reach the engine.  ONE, because a re-seat is not a probe: both legs
// are real team transitions the engine counts at [ng+230], broadcasts as `New user settings`, and
// runs the whole participating dispatch for.  The dispatch enables synchronously off the role
// word it just wrote, so a bounce that left the actor off has already told us that this entry
// point is not the one.  It is LAST because it is the only arm that moves something the recording
// owns: IDENT.SERVER_ASSIGN_TEAM reallocates the entrant's Team Slot on every leg
// (sub_1401AB730:198-217 frees it, :227-267 pops a new one), so it runs only when the two arms
// that touch no recorded state have already been tried and told us they could not.
constexpr uint16_t kReSeatIssueMax = 1;
static_assert(kReSeatIssueMax <= kRefreshIssueMax,
              "the bounce is the last resort, so it may not be allowed to spend more engine calls "
              "than the two arms that run before it");

// How many windows a pending re-seat may hold its latch waiting for the engine's ASYNC
// enable to land.  The dispatch enables a tick or more after the call, and the census asks once a
// window (~1 s), so the enable and the pending flag never fell in the same window -- the pre-fix
// code consumed the flag in the window that first read the actor still off, before the enable could
// coincide with it, so a re-seat that DID enable was never booked (DIAG-PARTICIPATION-REGRESSION).
// A few windows is a wait of seconds, not a spin; each settle window reads only the enable
// predicate and drives nothing, and past it the state is named ReSeatSettleExhausted.
constexpr uint16_t kReSeatSettleWindows = 8;

struct ParticipateOutcome {
    Participation      state = Participation::Refused;
    ParticipateRefusal why   = ParticipateRefusal::NoDriver;
};


// The engine surface the chain reads and drives.  Every POSTCONDITION is a read of the engine's
// own record; a postcondition that is not wired answers COULD-NOT-ASK under its own hop's name
// and never a pass, because a hop nobody can read is not a hop that happened.
//
// The two DRIVERS are optional and their return value is never the verdict -- the same rule the
// actor step already follows, and for the same reason: the engine's own enable is a no-op on an
// actor that is already enabled, so a chain keyed on the call's answer would refuse a correct
// state.  The postcondition is re-read after the call and that is what decides.
struct ActorHops {
    void* ctx = nullptr;
    bool (*inRemotePlayerCs)(void* ctx, uint16_t index) = nullptr;
    bool (*enabled)(void* ctx, uint16_t index) = nullptr;
    bool (*placed)(void* ctx, uint16_t index) = nullptr;
    bool (*physicsCi)(void* ctx, uint16_t index) = nullptr;
    // Hop 4's WHY, read off the RemotePlayer CI's own requirement words.  Unwired answers Unknown,
    // which is a COULD-NOT-ASK and never a pass -- the census prints the name it gets back.
    PhysicsCiReq (*physicsCiAwaiting)(void* ctx, uint16_t index) = nullptr;
    // A0's partition, and it is not a bound (S14.5).  NETGAME.LOCAL_USER_COUNT_OFF is the count of
    // LOCAL user records; the engine's participating arm branches on it and handles an entrant
    // below it in its own first block.  We refuse those by name rather than authoring anything.
    LocalUserAnswer (*isLocalUser)(void* ctx, uint16_t index) = nullptr;
    // A2 -- the entrant's `spawned` record, written through the engine's setter for the side the
    // engine's OWN reader selects.  Not a bool: see SpawnedWrite.
    SpawnedWrite (*writeSpawned)(void* ctx, uint16_t index) = nullptr;
    // A3 -- IDENT.SERVER_ASSIGN_TEAM with the recorded role.  Answers only whether the call was
    // ISSUED; the packed word is re-read afterwards and only that decides.
    bool (*assignTeam)(void* ctx, uint16_t index, uint16_t recordedRole) = nullptr;
    // The entrant's LIVE team, read off the engine's own packed word.  It is A3's early-out gate:
    // the engine skips its whole body when the requested role already equals this, so a chain that
    // did not read it could not tell a no-op call from a success.  The engine's own spawn-point
    // author reads the same word, so this hook and the engine cannot disagree.  The engine's own
    // spawn-point author (ENGINE.SPAWN_POINT_AUTHOR_VA) reads the same word for the same entrant.
    // Unwired means A3 cannot tell an issue from a no-op, so no call is made at all.
    bool (*teamOf)(void* ctx, uint16_t index, uint8_t* teamId, uint8_t* teamSlot) = nullptr;
    // A3's OTHER arm -- NETGAME.REFRESH_PLAYER_ACTOR(ng, index), the engine's own late-join
    // re-provision.  L3 #29b: the engine's `Server Assigning Team` auto-pick set the entrant's
    // role word to the recorded value a second BEFORE our `spawned` write landed, so its
    // participating arm ran twice against a `spawned` of 0 and its only enable site --
    // `else if (USER_SPAWNED_GET(ng,idx)) ENABLE(...)` -- never fired.  A3 cannot re-run it: the
    // engine early-outs on `role == requested`.  This call can: its body ends in
    // NETGAME.SET_PLAYER_ACTOR, whose dispatch re-runs unconditionally off the CURRENT role word,
    // and with `spawned` now 1 the enable fires.  Its two callers in the image are netgame lobby
    // messages a replay-local host never receives, which is why nothing re-ran it for our entrant.
    // The return value is NEVER the verdict -- `enabled` is re-read after it and only that decides
    // -- but it IS the diagnostic that names which gate the call met, so it is carried out whole
    // rather than flattened to a bool.  See RefreshAnswer.
    RefreshAnswer (*refreshPlayerActor)(void* ctx, uint16_t index) = nullptr;
    // A4 -- NETGAME.SPAWN_NOTIFY(ng, index) = sub_1401CAC00, the SPAWN half of the same
    // Entrant-Data-Changed callback sub_14017C370 whose REFRESH half `refreshPlayerActor` drives.
    // On a live remote player the server's entrant-data change fires both; a replay-local host runs
    // only the refresh, so the entrant's actor carried RemotePlayer + phys but NO ncaModel CI
    // (DIAG-SPAWN-MODEL-CI Lever B, RUN-44: modelCI=absent(kNoComponent)).  This drives the engine's
    // own spawn handler so its remote arm sub_140CB81B0 -> sub_140D43D90 installs+dresses that CI.
    // Whether the handler's host/remote branch installs it for our synthetic host-side entrant is a
    // runtime residual: SpawnNotifyResult carries the branch mode word for the section-4 probe.
    SpawnNotifyResult (*spawnNotify)(void* ctx, uint16_t index) = nullptr;
    // The two-read around the refresh: the entrant's SActorGUID at IDENT.USER_ACTOR_GUID_OFF, read
    // before and after the call.  :319 is the ONLY writer of that word on this path,
    // so a guid that did not move is the engine saying it did not allocate -- and therefore did
    // not reach the dispatch either.  Nothing else can tell those apart from outside.
    bool (*actorGuid)(void* ctx, uint16_t index, uint64_t* lo, uint64_t* hi) = nullptr;
    // The readiness predicate for that call, read off the engine BEFORE it and never after.  L3
    // #21 issued the refresh on an entrant that held no actor component instance and the main
    // thread never returned: Rip inside engine code, no wait object on the stack, the watchdog's
    // "Deadlock detected!" eleven seconds later.  Both of the engine's own callers run it on an
    // entrant that already IS a remote player, and this is the surface that says whether ours is.
    bool (*actorAssetsReady)(void* ctx, uint16_t index) = nullptr;
    // Whether THIS SESSION wrote a true `spawned` into the entrant's slot before it issued the
    // entrant add.  It is what lets an actor the engine enabled during that add be claimed: the
    // add is this session's engine call, and the record it enabled off is this session's write.
    // Unwired or false, an already-enabled actor stays a skip and is never claimed.
    bool (*seededBeforeAdd)(void* ctx, uint16_t index) = nullptr;
    // THE BACKSTOP -- NETGAME.PARTICIPATING_VA then the dress half sub_1401C7BE0, which is
    // NETGAME.USER_SETTINGS_APPLY's own tail without its Team Slot reallocation.  Answers only
    // whether BOTH calls were issued; `enabled` is re-read on a later sweep and only that decides.
    // Refused outside the stage-0 NETGAME.UPDATE window: PARTICIPATING walks component systems.
    bool (*participatePair)(void* ctx, uint16_t index) = nullptr;
};

// What the caller hands the chain so it may DRIVE, and what the chain hands back about the drive.
// A chain called without one reads and refuses -- there is no path that issues an engine call
// without a gate, because a gate is the only thing that can hold a transition to once per epoch.
struct HopDrive {
    DriveGate* gate = nullptr;
    // The role this player was RECORDED with, in the engine's own encoding (the field
    // IDENT.USER_TEAM_SHIFT / IDENT.USER_TEAM_MASK select).  The roster's "never recorded"
    // sentinel arrives here unchanged and refuses as TeamNotRecorded rather than being mapped.
    uint16_t recordedRole = kRoleNotRecorded;
    // How many entrants this session is provisioning, written by the caller.  It gates the
    // last-resort role bounce and nothing else: the bounce round-trips the entrant's Team Slot
    // only when the entrant is the sole member of its team, which for a one-entrant session it
    // is.  ZERO means the caller did not say, and the bounce refuses on it -- a gate that cannot
    // be asked must not answer yes.
    uint32_t entrantCount = 0;
    // Written by the chain on every call that reached the participation step.
    ParticipateOutcome outcome{};
    // Hop 4's requirement word, decided by the SAME read that decided the hop.
    // A caller that asks the surface a second time can watch the engine finish InitCI in between
    // and print `PhysicsCiAwaiting(none)` beside a hop that answered false.  Unknown until a pass
    // reaches hop 4, which is a could-not-ask and never a pass.
    PhysicsCiReq physicsCiWhy = PhysicsCiReq::Unknown;
};

// A2 then A3, in that order, under the re-entrancy latch and once per epoch per slot.  This is the
// ONLY place in the tree that issues an engine call for an entrant's participation.  Every refusal
// is by name and issues nothing; the two postconditions are the engine's OWN records, re-read, and
// a postcondition that did not flip is a refusal and never a success.
ParticipateOutcome Participate(const ActorHops&, uint16_t index, HopDrive&);

// Advance one entrant as far along the chain as the engine's own records allow, and answer the
// FIRST hop that is still outstanding -- Hop::Done only when all four read done.  Hop 1 is a read
// of the ROOT's guid table; hops 2, 3 and 4 are reads of the engine's own records for the enable,
// the `spawned` boolean and the RemotePlayer CI.  NOTHING in the chain drives an actor: the one
// authored step is `Participate`, which runs once between hop 1 and hop 2 and lets the engine's
// own participating arm do create / enable / chassis / CI with the arguments it derives.
Hop AdvanceActorHops(const ActorHops&, uint16_t index, HopDrive* drive = nullptr);

// Whether this build can drive a participation at all, and -- when it cannot -- the one word that
// says why.  A build that reports Armed while nothing was ever issued is the state S14.7 test 11
// exists to fail: `provisioned` may not be claimed off a driver that was compiled out or unbound.
struct FeatureState {
    bool armed = false;
    const char* reason = "";   // "" when armed; a single word naming the disablement otherwise
};
FeatureState ParticipationFeatureState(const ActorHops&);

// The requirement hop 4 is waiting on, or Unknown when nothing is wired to ask.  Answers None only
// when the surface says the CI is built.
PhysicsCiReq PhysicsCiAwaiting(const ActorHops&, uint16_t index);

// --- THE CRASH LOGGER AND THE PHASE MARKERS (L3 #25/#26/#27 F4) -------------------------------
//
// Three consecutive launches died of 0xC0000005 in the second after the entrant record was
// re-read, and not one of them left a stack: `dbgcore.dll` beside `echovr.exe` IS EchoLoader, so
// `dbghelp!MiniDumpWriteDump` forwards to a module that does not export it and the engine's own
// writer prints `Unable to find MiniDumpWriteDump` every time.  There is no dump to read and no
// external dump writer on this machine.  These calls are the replacement.  Both are DIAGNOSTICS:
// neither one is read by a provisioning decision, and the handler never swallows an exception.

// Install a first-chance vectored handler and open `logPath` for synchronous append.  The handler
// writes ONE `[crash]` block -- code, faulting address and VA, the register file, the module the
// fault is in, both stack walks and the last hop marker -- and then answers
// EXCEPTION_CONTINUE_SEARCH so the engine's own handler still runs.  It logs the FATAL codes only
// (access violation, in-page error, illegal/privileged instruction, stack overflow, the two
// non-continuable classes, and the two NTSTATUS reports 0xC0000374 / 0xC0000409 -- see
// CrashCodeIsFatal, which is the one list both arms ask); the engine's benign first-chance traffic -- the C++ EH code
// 0xE06D7363, the thread-name code 0x406D1388, DBG_PRINTEXCEPTION -- is passed through untouched,
// because a handler that logged those would write a megabyte per session and hide the one block
// that matters.
//
// Idempotent: the first successful install wins and every later call answers false without
// disturbing it.  Never called from DllMain -- the loader lock is held there.
bool InstallCrashLogger(const wchar_t* logPath);

// Install the logger ONCE PER PROCESS, from the bootstrap worker, in EVERY session mode.  A capture
// process never arms a PawnProvisioner, so the Arm() call is unreachable there and a capture crash
// reached no handler at all.  Returns true only on the call that installed
// it: a later call -- Arm's, or a second worker -- is a no-op and logs nothing.  `mode` is the
// session word echoed into the one install line, so a log says which kind of run is covered.
bool EnsureCrashLogger(const wchar_t* logPath, const char* mode);

// Uninstall (both arms), close the file and reset the counters.  Called by the tests and --
// this is the part that is wiring and not decoration -- by DLL_PROCESS_DETACH's FreeLibrary
// route in src/plugin/dllmain.cpp: a vectored handler left in the chain after our pages are
// unmapped turns the next exception of any kind into an instant unexplained death.
void RemoveCrashLogger();

// Successful FlushFileBuffers calls since the install.  The flush is the entire point of this
// writer -- a line still sitting in a buffer when the process is killed has told nobody anything,
// which is how three runs' worth of evidence was lost -- and in-process this counter is the only
// thing that can witness it.
uint64_t CrashLogFlushes();

// Crash blocks written since the install, which is also the sequence number of the last
// one.  Blocks are numbered because a first-chance block may be a fault somebody's __try
// was about to swallow: the reader needs to tell block 1 from the last one.
uint32_t CrashBlocksWritten();

// Re-snapshot the module list the crash handler resolves addresses against.  Called from
// the tick, NEVER from the handler: every Win32 module API takes the loader lock, and a
// handler that blocks on one does not truncate its block, it hangs the game.
void RefreshCrashModules();

// The allow-list itself, published so a test can drive it without raising a real fault.  The VEH
// and the chained UEF both ask THIS function -- there is no second copy of the list -- so a code
// that answers false here gets no block, whatever it does to the process.  0xC0000374
// (STATUS_HEAP_CORRUPTION) and 0xC0000409 (STATUS_STACK_BUFFER_OVERRUN) are members: the first is
// what killed L2 #17b with no block written at all.
bool CrashCodeIsFatal(uint32_t code);

// Reopen the per-tick marker budget.  The pump calls this once per tick; a settled loop
// over twelve slots cannot then flood a synchronously flushed log.
void ResetHopMarkerBudget();
// The per-tick marker budget, published because it is DERIVED from the longest driven pass and a
// test that spelled its own copy of the number would certify a budget that had drifted below it.
unsigned HopMarkerBudget();

// Marks ASKED FOR since the last reset, budget-suppressed ones included.  A test can drive real
// passes and compare what they issued against the published budget, instead of re-asserting the
// constants the budget is built from.
unsigned HopMarksRequested();
// How many driven passes one tick can spend the budget on -- ReopenAttemptWindow makes every
// slot due again the moment a provision lands, so this is the slot table depth, not one.
unsigned HopDrivenPassesPerTick();

// ---- THE CENSUS LINE'S LOADOUT CLAUSE (AUD-W2-P1-30-part2 F10) ---------------------------------
// It is the line B1's acceptance greps, so it has to be able to report a PARTIAL load: one entrant
// dressed 20/20 out of ten used to print `applied 20/20` and nine pawns in the engine's default kit
// read as a clean pass.  The decision is a function over one row per slot -- not a scan that stops
// at the first hit -- so it can be asked directly with a population no fixture has to provision.
struct LoadoutOutcome {
    bool alive = false;         // Awaiting or Provisioned: this row is a live entrant
    bool docPresent = false;    // the RECORDING carried a profile document for it
    bool attempted = false;     // the dress ran for it (loadoutAttempted)
    // Why a LIVE entrant has not been dressed yet, by name (the hop it is parked on).  A hop
    // deferral does not latch, so without this an entrant can sit undressed for the whole match
    // while the clause reports only the ones that got as far as being asked (part2 F15).
    const char* deferralWhy = nullptr;
    identity_access::LoadoutRefusal why{};   // zero is None -- static_asserted in the .cpp
    const char* sourceWhy = nullptr;         // the reader's own word, past our enum
    uint32_t compared = 0;
    uint32_t matched = 0;
};

struct LoadoutCensus {
    uint16_t live = 0;          // entrants this load has admitted -- K/N's REAL denominator
    uint16_t deferred = 0;      // live, not attempted: parked on a hop, asked for nothing yet
    const char* firstDeferral = nullptr;
    uint16_t attempted = 0;
    uint16_t authored = 0;      // compared > 0 and every compared pair matched
    uint32_t compared = 0;
    uint32_t matched = 0;
    identity_access::LoadoutRefusal firstWhy{};
    const char* firstSourceWhy = nullptr;
    bool     anyRefusal = false;
    bool     anyDocument = false;
};

LoadoutCensus SummariseLoadouts(const LoadoutOutcome* rows, uint16_t n);
// Renders the clause into `out`; never writes past `cap` and always NUL-terminates.
void FormatLoadoutClause(const LoadoutCensus&, char* out, size_t cap);

// One line per engine call the hop chain is about to make, written and flushed BEFORE the call:
//
//     HH:MM:SS.mmm INFO [pawn] hop 2 enable: ctx=0x00007ff8000c1230 index=0x0001
//
// so the last line in the log names the call the process was inside when it died.  Exactly one
// line per call, and no line at all until the logger is installed.
void MarkHop(unsigned hop, const char* call, const void* ctx, uint16_t index);

// The text of the last marker MarkHop wrote, or "" before the first.  Echoed into the crash block
// and read by the tests; never by a decision.
const char* LastHopMarker();

// S14.6's re-entrancy latch, and it is NOT private to Participate.  The whole participating arm --
// assign team, user settings, the participating dispatch, the enable, the chassis and the CI chain
// -- runs synchronously inside the engine call, from our tick, so any hook of ours that fires
// inside it must be refused rather than allowed to start a second transition.  Every engine call
// this plugin issues from the stage-0 tick therefore shares ONE latch: the dress path's
// IDENT.SERVER_ASSIGN_TEAM takes the same one the chain's A2/A3 take.
// `taken` is false when it was already held; the holder releases on scope exit.
struct EngineCallLatch {
    bool taken;
    EngineCallLatch();
    ~EngineCallLatch();
    EngineCallLatch(const EngineCallLatch&) = delete;
    EngineCallLatch& operator=(const EngineCallLatch&) = delete;
};
}  // namespace PawnProvision

namespace PawnResolve {
// The bind's live-surface lookup -- L3 #11 F1's split.  Owned here because the vocabulary is one
// contract; W2-P2 owns every code path that returns a value from it.
enum class Refusal : uint8_t {
    None = 0,
    NotAttempted,
    NoEntityService,
    NoPlayerCi,        // neither pool holds a CI for this slot
    NoPhysCi,          // a CI exists; PLAYER_CI_HANDLE_OFF is 0xFFFF
    NoModel,           // no NcaModel entry: the pawn is physical but invisible
    PawnClassMismatch, // INV-PP2
    SlotCollision,     // two CIs claim one slot
};
const char* RefusalName(Refusal);
const char* RefusalText(Refusal);
constexpr size_t kRefusalCount = size_t(Refusal::SlotCollision) + 1;
}  // namespace PawnResolve

// One recorded -> provisioned pair.  The field names are the resolver's (windowbuild's
// BindResult), because the two halves of `recorded slot N -> slot M` have to be the same two
// numbers on both sides of the bind.  Both default to absence, so a ledger nobody filled says
// "no map" and never "everything maps to slot 0".
struct SlotPair {
    uint16_t slot_recorded = kSlotUnmapped;
    uint16_t slot_resolved = kSlotUnmapped;
    // How far along the actor chain this pair got, as of the last cadence window.  It defaults to
    // the FIRST hop and not to Done: a pair nobody advanced has answered nothing.
    uint8_t  hop = uint8_t(PawnProvision::Hop::RemotePlayerCs);
};

// INV-PP1's subject.  `adopted` is structurally zero: no code path increments it.
struct PawnLedger {
    uint32_t requested;    // recorded players handed to the provisioner this load
    uint32_t provisioned;  // pawns THIS session created
    uint32_t adopted;      // structurally 0 -- see INV-PP1
    uint32_t deferred;     // attempts that could not be made yet (state gate, budget)
    uint32_t failed;       // attempts that were made and refused
    // S14.7 test 12: an issue, a skip and a refusal are three numbers, and only the first may ever
    // make `provisioned`.  A build that drove nothing therefore cannot report a provision.
    uint32_t participateIssued;
    uint32_t participateSkipped;
    uint32_t participateRefused;
    uint32_t participateRefusals[PawnProvision::kParticipateRefusalCount];
    uint32_t refusals[PawnProvision::kRefusalCount];  // by name, so `0 provisioned` says which step
    // The recorded -> provisioned slot map (S5).  Published because the two numbers are NOT the
    // same number: the viewer's own user holds a slot in the engine's table, so the slot a FILE
    // recorded is not the slot its pawn is provisioned into.  Only the first `mapped` entries were
    // assigned; the rest read kSlotUnmapped at both ends.
    uint32_t mapped;
    SlotPair map[facts::ENTITY_USER_SLOT_TABLE_MAX];
};

class IPawnProvider {
public:
    // Attempt to provision the pawn for one recorded player. Idempotent per slot per load:
    // a second call on a provisioned slot returns true without touching the engine.
    virtual bool Provision(UserSlot, const JoinDesc&) = 0;
    // Resolve the live pawn for a slot. Never creates. Refuses by name (PawnResolve::Refusal).
    virtual bool ResolvePawn(UserSlot, PawnRef& out) const = 0;
    virtual PawnProvision::Refusal LastProvisionRefusal() const = 0;
    virtual PawnResolve::Refusal   LastResolveRefusal()   const = 0;
    // The ledger the probe and the log line read. INV-PP1's `adopted` lives here.
    virtual PawnLedger Ledger() const = 0;
protected: ~IPawnProvider() = default;
};

// ---------------------------------------------------------------------------------------
// The authoring half, and the provisioner.  Both are engine-free and both are driven offline by
// tests/engine/pawn against a fake IIdentity.

class IIdentity;
class ILog;

// The engine's join record, as sub_140152210 dereferences it.  0x40 bytes: the last field the body
// reads is the profile argument at IDENT.ADMIT_JOINREC_PROFILE_OFF (0x30), and it is taken by
// address (lea r8, [rsi+30h]) rather than read, so the record has to own storage past it.
constexpr size_t kJoinRecBytes = 0x40;

// E-I6: teamid 2 is a spectator and teamid 4 a moderator; both are entrants with no pawn.  The
// insert's own spectator test is `cmp word ptr [rsi+0Ah], 1`, so 1 is the value the engine reads as
// "spectator" and 0 is the ordinary player.
enum : uint16_t { kJoinRolePlayer = 0, kJoinRoleSpectator = 1 };
enum : uint16_t { kTeamSpectator = 2, kTeamModerator = 4 };

// Author one join descriptor from one recorded player.  Never consults the live session: the
// descriptor is a statement about the RECORDING, which is what makes (B) reproducible.
// The JOINREC +0x00 id the admission is authored with.  NEVER the recorded one: the engine's
// duplicate-entrant eviction is keyed on {platform, id64} and a capture replayed on the account
// that made it would evict the viewer's own entrant.  Unique per entrant per session by
// construction -- the entrant index occupies the low 16 bits -- and never 0 or ~0, which are the
// engine's own `no id' and `supply one' requests.  `viewerId` is the id to stay off; 0 means the
// caller does not know it, which weakens nothing else.
// The low bits the entrant index occupies in that id -- the half that carries the uniqueness --
// and the mask the census logs it through, derived from the width rather than written twice.
constexpr unsigned kXplatIndexBits = 16;
constexpr uint64_t kXplatIndexMask = (uint64_t(1) << kXplatIndexBits) - 1;
// The window the census prints instead of the index: bits 16..31 are the low end of the MIX, so
// they move per entrant and per session, and the index bits below them prove nothing.
constexpr unsigned kXplatSaltShift = kXplatIndexBits;
// How many re-salts an id gets before the synthesis gives up and says so by name.  Eight is far
// past what a sixteen-entrant session can collide against by chance; the point of the bound is
// that a caller handed a degenerate in-use set gets a refusal instead of a spin.
constexpr unsigned kXplatSaltAttempts = 8;

// THE IDS THE ENGINE'S DUPLICATE-ENTRANT EVICTION WOULD MATCH US AGAINST.  sub_14015D780 compares
// {platform, id64}; slot 0 of the netgame's entrant table is the LOCAL VIEWER (written by
// sub_140152D80 with no stride) and every other slot is an entrant already admitted.  An id we
// author that equals any of them evicts that entrant -- the viewer included.
struct XplatIdsInUse {
    uint64_t ids[facts::ENTITY_USER_SLOT_TABLE_MAX];
    uint32_t count = 0;
    // COULD-NOT-ASK, not "none in use": no netgame surface means the set is unknown, and an
    // unknown set must not read as an empty one on the line that reports the avoidance.
    bool     known = false;
    bool Has(uint64_t id) const {
        for (uint32_t i = 0; i < count; ++i)
            if (ids[i] == id) return true;
        return false;
    }
};

// Read that set off the live netgame: entrant 0 at NETGAME.ENTRANT_XPLAT_ID64_OFF with no stride
// (the viewer), then every further entrant at NETGAME.PAWN_TABLE_STRIDE steps.  Zero and ~0 are
// not collected -- they are the engine's own `no id' and `supply one' words, which the synthesis
// already refuses to emit.  Returns `out.known`.
bool ReadXplatIdsInUse(void* netGame, XplatIdsInUse& out);

// Author an id that is nobody else's.  Returns false -- XplatIdCollision -- when every salt in
// the budget landed on an id already in use; `out` is then untouched.
bool SyntheticXplatformId(uint64_t recordedId, uint16_t entrantIndex,
                          const XplatIdsInUse& inUse, uint64_t& out);
JoinDesc JoinDescFromRecord(const PlayerRecord&, uint16_t entrantIndex = 0,
                            const XplatIdsInUse* inUse = nullptr);

// Serialise a descriptor into the engine's record layout.  Every offset comes from the registry.
// Returns false only when `out` is null.
bool WriteJoinRec(const JoinDesc&, uint8_t out[kJoinRecBytes]);

struct ProvisionEnv {
    IIdentity* identity = nullptr;
    void*    (*netGame)() = nullptr;   // the session's CR15NetGame, re-read on every attempt
    uint32_t   perFrameMax = 2;        // provision.per_frame_max (ARCH-23 S7.3)

    // THE ACTOR HOP CHAIN, and null is COULD-NOT-ASK rather than "all four passed".  A
    // provisioner armed without it settles at the admission exactly as it always did, and the
    // hop line prints `hops=unasked` so a build that bound no hop read cannot be mistaken in a
    // log for a build whose hops all answered done.
    const PawnProvision::ActorHops* hops = nullptr;
    // The post-provision census (S9's W2-P0 "after" pass).  All three default to null and the
    // census is skipped when any is: a provisioner armed by code that predates this reads and
    // writes nothing extra, and no product path depends on the instrument being wired.
    const IEntity* entity   = nullptr;   // read-only; every read is one IEntity already guards
    ILog*          log      = nullptr;
    int32_t      (*stateRaw)() = nullptr;  // the session state word, printed with the census

    // Whether the ENTRANT TABLE'S OWN MEMORY passed its span proof, which is the one thing
    // `SlotOf(index) == kSlotUnmapped` cannot report for itself: it answers the same word for
    // `no entrant carries this index` (an absent row -- the slot is FREE) and for `the table
    // could not be read` (UNREADABLE).  Null is not `unreadable': a fault has to be PROVED,
    // because `unreadable' is as confident an answer as `free' and it is the one that blocks.
    bool         (*entrantTableProved)() = nullptr;

    // ---- THE LOADOUT DRESS'S TWO PROVIDER SLOTS (P1-18) -------------------------------------
    // Both are slots and not direct calls, for the reason every other engine read here is one:
    // the drive must be exercisable with no game present, and the reader lives in src/playback,
    // which this layer links nothing of.  Null on either is a COULD-NOT-ASK, refused by name --
    // never "the entrant has no loadout".

    // The recording's identity block for one RECORDED user slot, i.e. playback's
    // identity::ReadRecordedLoadout bound to the file's own source.
    bool (*recordedLoadout)(uint32_t recordedSlot, identity::RecordedLoadout& out,
                            const char** refusal) = nullptr;
    // The engine surface L1..L6 call, i.e. identity_access::BindLoadoutEngine over this image.
    // False means an entry point did not resolve and the dress refuses LoadoutEngineUnbound.
    bool (*loadoutEngine)(identity_access::LoadoutEngine& out) = nullptr;

    // ENG-NATIVE-ADMIT (echo-101).  Parse the recorded loadout into the per-user SOURCE container
    // BEFORE admission, so the engine's own remote add-entrant handler bridges a non-empty loadout
    // into the live table instead of the zero-length array its phase-16 model-build deadlocked on.
    // Only meaningful on the remote reply route: the module sets it from
    // (pawn.remote_source_loadout && !pawn.bot_body), so the shipped bot-body default never runs it
    // and the default admission flow is byte-for-byte unchanged.  Default false.
    bool sourceLoadoutBeforeAdmit = false;

    // ENG-BOTROUTE-FIX (Change A).  On the BOT body-build route, carry the identity-block loadout
    // document into the admission so HookSourceLoadout's SOURCE parse feeds the engine parser the same
    // bytes the remote route parses to a non-empty loadout, instead of the manifest entrantJson that
    // parses to zero (DIAG-BOTROUTE-PARSE).  The module sets it from (pawn.bot_body_source_loadout &&
    // pawn.bot_body) -- the same config that wires HookSourceLoadout in the identity layer -- so the
    // shipped default never fetches or carries the document and the default admission flow is unchanged.
    // Default false.
    bool botBodySourceLoadout = false;

    // OUTFITS.  Author a profile document for an entrant the recording carries no
    // document for, out of that entrant's own recorded name, numbers and twenty item ids, so the
    // engine has something to build a loadout INSTANCE from.  The module sets it from
    // playback.dress_loadout -- the same switch that turns the dresser's outfit step off -- so a
    // launch with the outfit step off authors nothing.  Default false.
    bool authorLoadoutDocument = false;

    // On the engine's OWN remote-admission reply route (pawn.bot_body OFF), drive the
    // engine's R15NetPlayerEquipmentUpdate RECV (identity_access::DriveEquipmentRecv) for each settled,
    // still-undressed entrant once its actor+swapper record exist, carrying the recorded chassis -- so the
    // engine installs the net-model component and the framework's phase-16 rebuild seats a visible body.
    // The module sets it from (pawn.equipment_recv && !pawn.bot_body); default false, so the shipped
    // behaviour is byte-for-byte unchanged and no RECV is ever fabricated.
    bool equipmentRecv = false;

    // ENG-SPAWNED-SEED backstop.  For an admitted entrant recorded on a participating role whose
    // `spawned` record reads true and whose actor is still off, drive ActorHops::participatePair
    // once from the stage-0 sweep.  The module sets it from (pawn.participate_backstop &&
    // !pawn.bot_body); default false.
    bool participateBackstop = false;
};

// The state-9 latch and the sliced admission pass.  One instance per load.
//
// It is a latch and not a call because provisioning cannot complete synchronously with the file
// load: the actor is placed only at NETGAME.STATE_INMATCH, and the file load races the level load.
// An attempt before that edge is a DEFERRAL and says so; it is not a failure and not silence.
class PawnProvisioner {
public:
    void Arm(const ProvisionEnv&);
    void Disarm();
    bool Armed() const { return env_.identity != nullptr && env_.netGame != nullptr; }
    // What this provisioner was armed WITH. `Armed()` cannot answer for the provider slots -- a
    // provisioner armed with both null is armed, pumps, provisions, and refuses every dress by
    // name -- so the env is readable and the module that filled it can be asked whether it did.
    const ProvisionEnv& Env() const { return env_; }

    // Queue one recorded player.  Requesting a slot twice is not a second request.
    bool Request(UserSlot, const JoinDesc&);

    // One attempt for one slot.  Idempotent: a provisioned slot answers true and touches nothing.
    bool Provision(UserSlot, const JoinDesc&);

    // The sliced pass: at most perFrameMax admissions per call, so a 12-player file cannot deliver
    // a 12-insert spike into one tick (S7.3 -- L3 #11's tail rung already fires at N=1).
    // Returns how many were provisioned by THIS call.
    uint32_t Pump();

    // The per-tick swapper-current reset sweep the stage-0 NETGAME.UPDATE detour
    // drives (identity_access::RegisterSwapperResetSweep, installed at Arm).  Runs in the fast in-match
    // cadence, in the window BEFORE the engine's admission Apply Chassis, for each of OUR settled
    // entrants whose mesh is not yet built; forces the pooled record's current model to -1 so the
    // engine's swap takes the enqueue arm.  Gated (identity_access::SwapperResetShouldFire) to fire
    // once and never churn a built mesh.  Guarded per engine touch; the viewer is never in slots_, so
    // it is never touched.  Public because the detour invokes it through a registered thunk.
    void SwapperResetSweep(void* netGame);

    // The ungated pooled-swapper dress sequence (NOT the disabled SwapperResetSweep
    // above; NOT gated by stageb.swapper_hook).  Our replay entrant is admitted into a POOLED actor whose
    // CModelSwapperCS::InitCI (sub_14043BC80) seeds the swapper current-model from the pooled instance's
    // residual worn hash rather than the -1 a live remote gets, so CModelSwapperCS::BeginModelSwap
    // (sub_14042D120) always takes its `current == target` no-op arm and never enqueues the mesh-build
    // job that seats the InstanceModel CI.  Runs from the same stage-0 detour for each of OUR settled
    // undressed entrants in two self-gated phases: (1) capture the pooled worn-model hash and force the
    // pooled current-model to the -1 no-model sentinel; (2) on a LATER tick, once current reads -1 and
    // the mesh is still absent, call the engine's OWN BeginModelSwap once with the captured target so
    // current(-1) != target takes the enqueue (!=) arm -- LEVERB's reset alone was insufficient because
    // the engine's one synchronous enable-time apply already no-op'd on the from==to seed.  The ungated
    // SwapperFlushTick pump drains the enqueued swap and the framework's phase-16 (sub_140323790) seats
    // the CI; we never call sub_140323790 or the seat wrapper sub_14030F120.  Idempotent/self-stopping
    // via SwapperResetShouldFire (reset) and BeginSwapShouldFire (single enqueue).
    void UndressPooledSwapperSweep(void* netGame);

    // The primary dress lever, driven every tick from the stage-0 detour before
    // the swapper sweeps.  For each of OUR settled, still-undressed entrants that carries a recorded
    // profile, emulate the engine's own "[NETGAME] Profile received" handler once (see
    // identity_access::EmulateProfileReceivedCall) so the ENGINE requests the model resource and
    // seats the InstanceModel CI itself via the onlobbymemberprofilereceived delegate.  Fire-once per
    // entrant, self-stopping once the CI seats (ModelVisible) or the entrant resolves.
    void EmulateProfileReceivedSweep(void* netGame);

    // The multi-player-capable model-component install, driven every tick from the
    // stage-0 detour (gated by env.equipmentRecv, default off).  For each of OUR settled, still-undressed
    // entrants whose actor+swapper record exist, extract the recorded chassis item id (recordedLoadout,
    // matched by the "chassis" slot key) and drive the engine's own R15NetPlayerEquipmentUpdate RECV once
    // (identity_access::DriveEquipmentRecv) so the engine installs the net-model component via
    // CModelSwapperCS and the framework's phase-16 rebuild seats a visible body.  Fire-once per entrant,
    // self-stopping once the CI seats (ModelVisible).  Keyed per-entrant by the resolved index, so it
    // generalises to N recorded remotes.
    void EmulateEquipmentRecvSweep(void* netGame);
    // ENG-SPAWNED-SEED backstop, called from the stage-0 detour ahead of the dress sweeps.
    void ParticipateBackstopSweep(void* netGame);

    // The recorded -> provisioned slot map (S5).  Deterministic and stable across a load: a slot
    // is assigned once, on the first attempt that could read the engine's own slot table, and is
    // never reassigned.  It never answers a slot the engine already holds an entrant for -- the
    // viewer's own slot included -- so a recording cannot be redirected onto the viewer's pawn.
    // kSlotUnmapped until that attempt happens.
    UserSlot MappedSlot(UserSlot recorded) const;

    // THE RETRY CADENCE (L3 #15 F1).  A blocked attempt walks the lobby chain once per pending
    // slot and calls the engine's insert, and while the session blocker is UNCHANGED a second walk
    // this tick answers exactly what the first one did.  So an attempt is spent only when the
    // blocker moved, when a provision just succeeded (which moves the answer for every other
    // slot), or once every kRetryTicks pumps.  Pump is called once per frame and is the tick.
    //
    // One second at the engine's own fixed step, derived rather than chosen: a literal here would
    // be a number nothing checks, and the ratio is what makes the cost test's bound derivable.
    static constexpr uint32_t kRetryTicks = 1000000u / facts::CLOCK_FIXEDSTEP_30HZ_US;

    // The cost test's subjects.  `attempts` counts the passes that actually walked the engine;
    // `pumps` counts the ticks.  A per-tick retry makes them equal, which is the mutant.
    uint32_t Attempts() const { return attempts_; }
    uint64_t Pumps() const { return pumps_; }

    bool                   Provisioned(UserSlot) const;
    PawnProvision::Refusal SlotRefusal(UserSlot) const;
    PawnProvision::Refusal LastProvisionRefusal() const { return last_; }
    PawnLedger             Ledger() const;

    // The admission the DRESSER asks for, and the reason it is not Provision itself: a re-dress
    // after an entity re-bind asks a second time for a recorded slot this load already mapped.
    // Re-running the assignment would either refuse -- the entrant this load admitted is what holds
    // the mapped slot now -- or hand the same recorded player a second live slot.  So the mapping
    // this load made is handed back, and the INDEX is re-asked of the engine rather than remembered,
    // because a new CR15NetGame issues new indices while our slot numbers stay ours.
    bool Admit(UserSlot recorded, const JoinDesc& desc, UserSlot& slotOut, UserIndex& indexOut);

private:
    // ★ AWAITING IS THE RESUMABLE STATE P1-18 SS3c NAMED AND DID NOT BUILD: admitted and dressed,
    // with the actor chain outstanding.  It exists because the alternative two both lie -- Failed
    // latches a slot whose engine record fills in a moment later (L3 #23 F3/F4: the ledger's last
    // line is stamped at the state-9 edge, four and a half minutes before the process ends), and
    // Provisioned would count a pawn that has no body.
    enum class SlotState : uint8_t { Idle = 0, Pending, Awaiting, Provisioned, Failed };

    struct Slot {
        JoinDesc               desc;
        UserIndex              index{kSlotUnmapped};
        uint16_t               mapped = kSlotUnmapped;   // the live slot this recorded slot owns
        SlotState              state = SlotState::Idle;
        PawnProvision::Refusal why = PawnProvision::Refusal::NotAttempted;
        // An engine call was MADE for this slot at least once.  It is what makes the engine-record
        // re-read of the next attempt safe: an entrant found in a slot nobody asked for would be
        // an adoption, and INV-PP1 forbids adopting.  Set at the call, never at its answer.
        bool                   asked = false;
        // THE AUTHORED XPLATFORM ID, LATCHED.  The synthesis stays off every id the live session
        // holds, and once this entrant is admitted its own id is one of them -- a second asking
        // would move off itself, and an entrant whose id changes mid-admission reads to the
        // engine as a second person.  So it is authored once per slot per load and re-applied
        // over every descriptor the caller hands in afterwards.
        uint64_t               xplatId = 0;
        bool                   xplatAuthored = false;
        // Whether the avoidance set was READABLE when the id was authored -- unknown is not
        // "nothing in use", and the census line must not read as the stronger claim.
        bool                   xplatInUseKnown = false;
        // The outstanding hop, as of the last window.  RemotePlayerCs -- the first -- is the
        // value for a slot nothing has advanced, so the field cannot read "done" by default.
        PawnProvision::Hop     hop = PawnProvision::Hop::RemotePlayerCs;
        // The participation's once-per-epoch latch, per slot.  Lives here and not in ActorHops
        // because the surface is one shared binding and the latch is per entrant.
        PawnProvision::DriveGate driveGate{};
        // What the last participation attempt for this slot did, kept so the census can count
        // issues apart from skips and refusals rather than summing them into one number.
        PawnProvision::ParticipateOutcome participate{};
        // Hop 4's last named reason, so the window line and the census agree on one word.
        PawnProvision::PhysicsCiReq ciWhy = PawnProvision::PhysicsCiReq::Unknown;
        // THE DRESS.  `participatePump` is the pump on which this slot's participation was
        // ISSUED; the dress is refused until a LATER one, because S14.6 condition 2 (restated by
        // AUD-ARCH-23-S14 C3) forbids driving the loadout in the same frame as A3.
        uint64_t participatePump = 0;
        bool     participatePumpKnown = false;
        // One attempt per pass, and the answer latched: a dress that ISSUED must not be re-issued
        // on the next window, and a dress the recording cannot support must not be retried for
        // the life of the session.
        bool     loadoutAttempted = false;
        // The publish half (L1..L4) has its own latch: it runs BEFORE the hop chain issues
        // the enable, and the apply half runs after, so the two cannot share one.
        bool     loadoutPublishAttempted = false;
        // The SOURCE-loadout parse (LoadoutPhase::Source) has run for this slot.
        // It runs once, BEFORE admission, so the engine's own remote add-entrant bridge copies a
        // non-empty loadout into the live table instead of the zero-length array that deadlocked
        // the phase-16 model-build.  Latched because the SOURCE persists across the admission that
        // reads it -- re-parsing every retry window would be wasted work.
        bool     sourceLoadoutParsed = false;
        // The equipment RECV has been driven for this slot.  Latched on a definitive
        // attempt (the handler ran or faulted, or the recording carries no chassis to drive with) so it
        // fires once per entrant; a transient refusal (component/entrant not up yet) leaves it clear to
        // re-ask next tick.
        bool     equipmentRecvIssued = false;
        // The recorded chassis slot holds the engine's unfilled-pair sentinel: no model swap of any
        // kind is driven for this entrant, because the only target left is the pooled residue.
        bool     chassisUnfilled = false;
        // ENG-SPAWNED-SEED backstop: the pair was issued for this entrant (never twice), and the
        // enable re-read that follows it has been logged.
        bool     backstopIssued = false;
        bool     backstopReadBack = false;
        bool     backstopRefusalLogged = false;
        // Re-drives of the publish half spent so far, and whether the budget ran out.
        // A refusal the engine can clear is worth re-asking a bounded number of times;
        // re-asking it for ever would drive L1..L4 on every window of the match.
        uint8_t  loadoutPublishTries = 0;
        bool     loadoutPublishExhausted = false;
        // The word the publish half came back with, kept apart from loadoutWhy because the
        // apply half overwrites that one and the decision to hold the enable is made off
        // this one, a window later.
        identity_access::LoadoutRefusal loadoutPublishWhy{};
        // The first pump the enable was held on, so the grace window is counted from the
        // hold rather than from the load: a slot admitted late gets the same three windows
        // as one admitted first.
        uint64_t publishHeldPump = 0;
        bool     publishHeldPumpKnown = false;
        // WHICH OF THE FOUR PRE-ENABLE GATES ANSWERED NO on the last publish half this slot ran,
        // then re-read every window from AdvanceSlot as the actor side comes up.  This publish-time
        // value is the FLOOR the re-read may lower but not erase: a re-read that comes back Unasked
        // (no netgame this window, an unresolved instance name, an unbound predicate, memory that
        // did not read back) keeps this NO rather than clearing it, because a question that could
        // not be put a second time is not a yes.
        // The VERDICT only -- the readings it was decided from are printed where they are read.
        identity_access::LoadoutApplyGate loadoutGateVerdict{};
        // Value-initialised because the enum is only forward-declared here; pawn_provision.cpp
        // asserts that zero IS LoadoutRefusal::None rather than assuming it.
        identity_access::LoadoutRefusal loadoutWhy{};
        // The refusal the RECORDING'S OWN reader named, kept verbatim: it says which block was
        // missing or refused, which no enum of ours can.  Points into the source's static text.
        const char* loadoutSourceWhy = nullptr;
        uint16_t loadoutMatched = 0;    // the acceptance: recorded-vs-engine, BY SLOT KEY
        uint16_t loadoutCompared = 0;   // L1's own count.  0/0 is never a pass
        // THE MODEL DRIVE (DIAG-MODEL-BUILD fix (b)).  `modelDrivePump` is the pump the productive
        // chassis apply was driven on, latched ONCE per load so the async model load is not
        // re-enqueued every window; the postcondition -- ComponentOnActor(model_.cs, actor) -- is
        // re-read every window from there until it reads present or the grace budget runs out.
        // `modelResolved` latches on the first present read, so a body that resolves is never
        // re-driven (which would re-skin an already-dressed actor -- the from==to no-op).
        uint64_t modelDrivePump = 0;
        bool     modelDrivePumpKnown = false;
        bool     modelResolved = false;
        // Latched once the SPAWN handler (NETGAME.SPAWN_NOTIFY) has been driven for this entrant, so
        // the model-CI install is issued at most once per slot -- re-driving a spawn is not a no-op.
        bool     spawnNotified = false;
        // Re-publishes driven from the model step when the instance the engine matches BY NAME reads
        // empty (IDENT.INSTANCE_ITEM_COUNT_OFF==0 or freed): SERVER_ASSIGN_TEAM re-seats the entrant
        // and frees the instance our publish installed, and the engine's own PROFILE_RECEIVE refill
        // never fires for a driven entrant, so the drive would enumerate nothing (RUN-38).  Bounded.
        uint8_t  modelInstanceRebuilds = 0;
        // Cadence windows the model step has HELD waiting for the entrant's recorded profile to
        // land.  The instance's items derive from the profile document alone (IDENT.LOADOUT_FROM
        // _PROFILE) and it resolves LATE in the recording (DIAG-SWAP-TRIGGER: empty at the spawn,
        // real bytes ~a second on).  Driving before it lands reaches APPLY_LOADOUT_ITEMS with a
        // +56==0 instance and skips the loop -- no swap, the RUN-39 bodyless entrant.  Held, not
        // spent, until the profile lands; bounded so a genuinely profileless entrant is not held
        // for ever.  Separate from the rebuild count: an absent profile is a wait, not a re-publish.
        uint8_t  modelProfileWaits = 0;
        // The model refusal as of the last window, so the census and the window line agree on one
        // word.  None while the body is present or the async load is still inside its grace.
        PawnProvision::Refusal modelWhy = PawnProvision::Refusal::None;
        // The direct BeginModelSwap re-trigger.  `swapTargetHash` is the pooled target
        // model hash the sweep captures the first time it reads a real stale current -- under the
        // from==to no-op this equals the engine's own swap target, and it survives the -1 the sweep
        // then writes.  Once the sweep has forced current to -1 and it PERSISTED (mesh still absent,
        // nothing enqueued), the sweep calls the engine's BeginModelSwap once with this target so
        // current(-1) != target takes the enqueue arm.  `beginSwapIssued` latches the single fire so
        // the enqueue never repeats.
        uint64_t swapTargetHash = 0;
        bool     swapTargetKnown = false;
        bool     beginSwapIssued = false;
        // Latches once the engine's own Profile-received handler has been emulated
        // for this entrant (or the emulation reached a definitive parse/json refusal).  Fire-once so
        // the engine seats the InstanceModel CI at most once via the onlobbymemberprofilereceived
        // delegate; a transient refusal (image not yet bound, latch held) leaves it clear to re-ask.
        bool     profileEmulated = false;
    };

    // What this process's surface says about ONE live slot -- the same three states the admission
    // gate decides on, spelled once at namespace scope so the mapper and the gate cannot come to
    // disagree about what "free" is.  That disagreement is L3 #14.
    using SlotOccupancy = PawnProvision::SlotOccupancy;

    struct SlotProbe {
        SlotOccupancy state = SlotOccupancy::Unreadable;
        uint16_t index = kSlotUnmapped;   // the per-slot side table's word for this slot
        uint16_t named = kSlotUnmapped;   // the slot that entrant's OWN record names back
        uint64_t peer  = 0;               // conn[slot]'s peer id, when the lobby answered
        bool     lobby = false;           // true when the ENGINE's own predicate answered, in
                                          // which case index and named were never consulted
    };

    bool Refuse(UserSlot, PawnProvision::Refusal);
    // Author (or re-apply) the slot's synthetic XPlatformID against the ids the live session
    // holds.  Returns XplatIdCollision when the salt budget ran out against a set we could read.
    PawnProvision::Refusal AuthorXplatId(Slot&);
    // One pass of the actor chain for one admitted slot.  True only when every hop reads done, at
    // which point the slot becomes Provisioned; otherwise the outstanding hop is refused by name
    // as a DEFERRAL and the slot stays Awaiting, to be asked again on the next cadence window.
    bool AdvanceSlot(UserSlot, Slot&);
    // L1..L6 for one slot, driven from AdvanceSlot once every hop reads done and the
    // participation this session issued is at least one pump old.  Never called earlier: hop 2's
    // own re-read is L5's gate and the apply resolves in the actor/space domain.
    void DriveLoadout(UserSlot, Slot&, identity_access::LoadoutPhase);
    // DIAG-MODEL-BUILD fix (b): drive the entrant's productive chassis apply on an UNDRESSED
    // record and poll the engine's own model postcondition.  Called once per window from
    // AdvanceSlot AFTER every hop reads done (the entrant enabled) and the loadout has been
    // applied, so the chassis id is known.  Two-read: precondition is ComponentOnActor(model_.cs,
    // actor) ABSENT (the observable of the swapper's current == -1 that the engine's != arm
    // needs); we drive ONCE, then re-read the SAME postcondition on LATER pumps because the load
    // is async; past kModelGraceWindows without it we name the state ModelNotInstantiated.  We
    // NEVER pre-stamp the swapper's current.  Returns true while the slot must DEFER -- stay
    // Awaiting and be re-polled next window -- as the async mesh loads; false when it may proceed
    // to Provisioned, either because the InstanceModel is present (s.modelResolved) or because the
    // grace ran out and the absence is NAMED in s.modelWhy (a bodyless pawn is still provisioned,
    // like a loadout that could not be authored).  A null entity surface is a could-not-ask and
    // never holds.  The hold mirrors kPublishGraceWindows: hold a bounded number of windows for the
    // body, then provision rather than trade a late body for no pawn.
    bool DriveEntrantModel(void* netGame, UserSlot, Slot&);
    // How many cadence windows the async model load is given before the absent InstanceModel is
    // named a refusal rather than a wait.  The mesh comes off CDeferredMethodQueue a few pumps
    // after the swap is enqueued, so a same-pump read is always absent; this is the horizon past
    // which "still loading" becomes "never arrived".
    static constexpr uint32_t kModelGraceWindows = 4;
    // How many times the model step rebuilds (re-publishes) the loadout when the name-matched
    // instance reads empty before giving up and provisioning bodyless.  Bounded like the enable's
    // publish retry; the rebuild runs AFTER the hop chain (the re-seat included) reads Done, so it
    // is not re-wiped, and one success is enough.  Exhaustion is LOGGED, never silent.
    static constexpr uint8_t kModelInstanceRebuildLimit = 3;
    // How many cadence windows the model step holds waiting for the entrant's recorded profile to
    // land before it stops holding and provisions bodyless.  The profile's own landing is the
    // terminal state -- faithful playback delivers it at its recorded playhead offset, ~2 windows
    // past the spawn at 1x -- so this is only the backstop for a recording that genuinely never
    // carries one (a spectator that reached this step): generous over the landing, still finite.
    static constexpr uint8_t kModelProfileWaitWindows = 6;
    // How many cadence windows the enable is held while the live loadout count reads 0.
    // Not unbounded: a publish that can never take would otherwise trade "a pawn wearing
    // nothing" for "no pawn", and the pawn is the thing the rest of the replay is hung on.
    static constexpr uint32_t kPublishGraceWindows = 3;
    // RE-ASKS OF THE PUBLISH HALF, past the first attempt.  Bounded for the same reason
    // the hold is: an engine state that has not cleared in this many windows is not
    // going to, and the entrant is better off enabled undressed than re-driven for ever.
    // Exhaustion is LOGGED by name -- a retry that quietly stops reads as one that worked.
    // ONE FEWER THAN THE WINDOWS THE HOLD BUYS: the last re-ask has to fall inside them, or the
    // budget outlives the only state in which it can be spent and the exhaustion never prints.
    static constexpr uint8_t kPublishRetryLimit = uint8_t(kPublishGraceWindows) - 1;
    // The success tail, factored because there are now two ways to reach it: the attempt that
    // returned an index, and the engine's own record answering for an attempt that deferred.
    bool Settle(UserSlot, Slot&, UserIndex);
    // Pick the live slot for one recorded slot, or answer false when none is free.  `unreadable`
    // counts the slots the surface would not answer for, so the caller can refuse by the right name.
    bool AssignMappedSlot(uint16_t recorded, uint16_t& out, uint16_t& unreadable);
    bool SlotClaimed(uint16_t live) const;
    SlotProbe ProbeSlot(uint16_t slot) const;
    // The two halves of the viewer exclusion.  A slot the ENTRANT TABLE named back before this
    // provisioner asked for anything is held by an entrant this session did not create, and in a
    // replay session that is the viewer's own user.  Its INDEX is what has to be remembered, not
    // its slot: the engine re-seats the local user across the state 8->9 edge, and what the
    // re-read is handed is an index it would otherwise pass straight to the dress pass.
    void NoteForeignEntrant(const SlotProbe& probe);
    bool ForeignEntrant(UserIndex index) const;
    // True unless a wired hook says the entrant table's memory did not pass its span proof.
    bool EntrantTableProved() const;
    // The engine's own free predicate, walked through the live lobby.  False means the chain
    // could not be walked at all -- neither free nor held -- and `out` is left untouched.
    bool ProbeLobbySlot(uint16_t slot, SlotProbe& out) const;

    // True when this tick may spend an attempt.  Decided ONCE per pump and cached, so every slot
    // in one Pump pass shares the window and a 12-player file is not spread over 12 seconds.
    bool AttemptDue(PawnProvision::Refusal blocker);
    void ReopenAttemptWindow();   // a provision landed: the next slot's answer can have changed

    // Entrant indices seen holding a slot before this provisioner asked for one.  Bounded by the
    // table it is read off: no more slots can name an entrant back than the table has rows.
    uint16_t     foreign_[facts::ENTITY_USER_SLOT_TABLE_MAX];
    uint16_t     foreignCount_ = 0;

    bool         censused_ = false;   // one census per epoch; an epoch is one Arm
    uint32_t     epoch_ = 0;          // arm epoch -- the place latch's period
    uint64_t     pumps_ = 0;          // Pump calls this epoch -- the cadence's tick counter
    uint64_t     next_attempt_ = 0;   // the pump at which a still-blocked retry may be spent
    // The pump at which the next WINDOW LINE is due.  Its own counter and not next_attempt_: the
    // attempt cadence is never consulted while the session blocker holds (the blocker refuses on
    // one guarded read, before AttemptDue), and the line has to print through that too.
    uint64_t     next_status_ = 0;
    uint64_t     next_census_ = 0;    // The pump at which the full census reprints (~5s)
    // Per-provisioner, not per-process: a second provisioner in a test gets its own window rather
    // than inheriting one the first one already spent.
    uint64_t     statusChatterAt_ = 0;
    uint64_t     censusChatterAt_ = 0;
    uint64_t     window_pump_ = UINT64_MAX;   // the pump AttemptDue last decided for
    bool         window_open_ = false;
    uint32_t     attempts_ = 0;       // attempt passes SPENT this epoch
    uint32_t     suppressed_ = 0;     // attempt passes the cadence declined
    PawnProvision::Refusal blocker_ = PawnProvision::Refusal::NotAttempted;  // as of the last attempt
    ProvisionEnv env_{};
    Slot         slots_[facts::ENTITY_USER_SLOT_TABLE_MAX];
    PawnLedger   ledger_{};
    PawnProvision::Refusal last_ = PawnProvision::Refusal::NotAttempted;
};

// ---------------------------------------------------------------------------------------
// THE POST-PROVISION CENSUS (ARCH-23 S9, the W2-P0 row's "after" pass; L3 #12's FACT-PP1 line).
//
// Prints one row per MAPPED slot -- slot / class / phys / entity / driveable on BOTH surfaces --
// plus the session state word, and then states which FACT-PP1 arm the numbers support.  It exists
// because S1 forbids every packet from assuming an arm: W2-P2's ResolvePawn scans the remote pool
// while the capture read the same pawn through the broadcaster pool, and only a count off a live
// session says whether a provisioned entrant appears in both.
//
// Read-only and bounded.  Every value comes back through IEntity, whose reads are span-guarded, so
// there is no pointer this function can fault on and nothing it can perturb.  It walks
// `ledger.mapped` entries, never the whole table, and Pump runs it at most once per epoch.
//
// `stateRaw` is INT32_MIN when the caller has no state word to offer, and the census says so
// rather than printing a zero that reads like a state.
//
// Returns the number of rows printed, i.e. the mapped slots it could actually ask about.
// `netGame` is what lets the census print the plugin's OWN loadout count per entrant beside the
// pool answers -- the number ENG-LOADOUT-GUARD S7 left unwired.  It defaults to absent because a
// caller that has no session must still be able to census the pools, and an absent session prints
// the REASON (`loadouts=?(NoNetGame)`) rather than the zero that would read as a dressed entrant
// wearing nothing.
uint32_t CensusProvisionedPawns(const PawnLedger&, const IEntity&, int32_t stateRaw, ILog&,
                                void* netGame = nullptr);

constexpr int32_t kCensusStateUnknown = INT32_MIN;


}  // namespace replay
