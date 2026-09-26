#pragma once

// B.11.3 IIdentity (E-9) -- names, profiles, loadouts, skins and colours.  Owned by W2-C.
//
// ---------------------------------------------------------------------------------------
// WHAT THE BINARY ACTUALLY SAYS, because three of this packet's design premises were wrong
// and the code has to be built on the reading, not on the premise.  Every number reaches
// the .cpp from facts_generated.h by id; every id's anchor is in the W2-C anchor drop.
//
//   ONE POINTER CARRIES NAMES *AND* COSMETICS.  CR15NetGame + IDENT.PROFILE_JSON_OFF +
//   592*index is the entrant's profile CJson.  ApplyLoadoutItems (0x140154C00) returns
//   without a word when it is null or empty -- breaker 1 -- and UserDisplayName
//   (0x1401C98C0) reads `displayname` out of the same document.  Populate it and both the
//   name and the loadout work; leave it and both fail, one silently.
//
//   THE RESERVED REPLAY BAND RENAMES THE PLAYER.  UserDisplayName branches on
//   [IDENT.NONREPLAY_COUNT_OFF, IDENT.ENTRANT_COUNT_OFF) and formats "Replayer %u" from
//   slot-15 WITHOUT looking at the profile at all.  DESIGN 9.2 ID-A puts replayed players
//   in exactly that band and calls it "the difference between looks right and is 1:1 for
//   names".  It is the opposite: a player in the band cannot carry its recorded name.
//   ID-A is therefore refused here, with the reason, rather than offered.
//
//   ALIGNMENT IS PER-TEAM.  CR15NetGame::Alignment (0x140153F20) is
//   `teamid = (userword >> 10) & 7; teamid >= 2 ? 0xFFFF : *(u16*)(NG + 0xF0 + 2*teamid)`.
//   Two players on one team cannot hold different alignments through that path.  The only
//   per-player override is the u16 at IDENT.CUST_OVERRIDE_ALIGN_OFF inside the
//   customisation record, which is why this interface reaches the record rather than
//   stopping at the team table.
//
//   THE NAME THE CLIENT PRINTS IS NOT THE NAME IN THE PROFILE.  UserDisplayName has THREE
//   gates and the profile is only the third: the replay band renames outright; bit 27 of
//   **(NG + IDENT.NAME_MODEFLAGS_PTR_OFF) skips the profile entirely; and an empty profile
//   string falls through as well.  All three fall to the SLOT-NAME table, and then to the
//   literal "Unknown".  `-noovr` sets bit 27 unconditionally (GAME.NOOVR_SET_VA), and the
//   default entry strategy passes -noovr -- so on the shipped launch the profile is NEVER
//   read for a name.  Two consequences the code below is built on:
//     * the recorded name is written to BOTH sources, profile and slot table, unconditionally;
//     * the name is CHECKED by calling UserDisplayName itself.  A check that reads back the
//       profile we just wrote confirms only that our own write landed, on the one source
//       every live gate routes around.
//
//   EVERY ADDRESS IS REBASED.  echovr.exe is DYNAMICBASE + HIGH_ENTROPY_VA with a 163,956-byte
//   relocation directory, and this project's own field artifact recorded a .text operand at
//   0x7FF6267C4E68.  A literal 0x140...... is not in the process.  Every engine address this
//   module touches goes through buildguard::MappedAddress, the tree's one rebase point, and
//   the whole set is enumerable through EngineSite* below so a check can walk it.
//
//   OverrideAlignment ALREADY re-runs the loadout.  0x140D202C0 stores the override, and
//   IF AND ONLY IF the effective alignment changed, clears flags&~0x4 on every slot entry
//   carrying 0x10 and TAIL-CALLS ApplyCurrentLoadout.  So a caller that sets the same
//   value twice gets nothing the second time; ApplyAlignmentChange (0x140CAA5F0) is the
//   unconditional half and both are issued, in that order, on a side swap.
// ---------------------------------------------------------------------------------------

#include <stdint.h>
#include <stddef.h>

#include "engine/entity.h"                    // UserIndex / UserSlot -- B.4, owned by W1-E
#include "engine/pawn_provision.h"            // ARCH-23's frozen contract -- B.11.3a, owned by W2-P1
#include "foundation/demofile/demotypes.h"    // Blob, Symbol64 -- B.0, owned by W1-B
#include "engine/entity_json.h"               // CJsonReader -- L1 reads the recorded document through it

namespace replay {

// UserIndex and UserSlot are B.4's, referred to unqualified on purpose.  entity.h declares
// them at global scope today while services.h forward-declares its interfaces inside
// `namespace replay`; unqualified lookup from in here finds them either way, so this file
// does not have to be edited when that divergence is settled.

// engine/wiretap.h's tap, forward-declared so B.11.3's consumers do not inherit that header.
class IWiretap;

class IIdentity {
public:
    virtual bool      AddReplayUser(UserSlot, UserIndex& out) = 0;   // ID-A, or bot path ID-B

    // The other end of AddReplayUser: hand the entrant back when a replay unloads, so a second
    // load in the same session dresses a clean roster instead of inheriting the first one's.
    //
    // ★ Non-pure, and the default REFUSES.  B.11.3 is frozen and every fake implementing it
    // predates this call; a pure virtual would be a compile break in packets that have nothing
    // to do with releasing.  What the default must not be is `return true` -- an implementation
    // that reports a release it did not perform is exactly the silence this packet exists to
    // remove.
    virtual bool      ReleaseReplayUser(UserSlot) { return false; }

    // ARCH-23 S7.2 -- two names, two jobs, no verb that lies.  AddReplayUser is a LOOKUP and always
    // was (five predicates, no engine call), so ResolveReplayUser is its true name and defaults to
    // it; ProvisionReplayUser is the one that calls the engine's admission insert.
    //
    // Non-pure with refusing defaults, for the reason ReleaseReplayUser is: B.11.3 is frozen and
    // every fake implementing it predates these.  What the provision default must not be is `true`
    // -- an implementation that reports an admission it did not perform is today's AddReplayUser
    // defect wearing tomorrow's name.
    virtual bool      ResolveReplayUser(UserSlot slot, UserIndex& out) {
        return AddReplayUser(slot, out);
    }
    virtual bool      ProvisionReplayUser(UserSlot, const JoinDesc&, UserIndex&) { return false; }

    // Why the refusal is asked of the IDENTITY and not held by the provisioner: the provisioner can
    // see THAT an admission failed, never WHICH step of it did, and INV-PP1's ledger prints the
    // step by name.  The default is the shipping answer on this build (ARCH-23 S3.4).
    // THE ACTOR HOP CHAIN'S ENGINE SURFACE (P1-24), or nullptr when this build bound none.
    //
    // Non-pure and defaulted to nullptr for the reason ReleaseReplayUser is: B.11.3 is frozen and
    // every fake implementing it predates this call.  The default is not a convenience -- it is
    // the ledger's third state.  A provisioner handed nullptr prints `hops: unasked`, which is a
    // DIFFERENT line from a chain whose hops all read false, so a build that bound no engine read
    // cannot be mistaken in a log for a build whose reads all answered no.
    //
    // The pointer is owned by the implementation and outlives every caller inside a session; the
    // provisioner stores it and re-reads through it once per pass.
    virtual const PawnProvision::ActorHops* ActorHops() const { return nullptr; }

    virtual PawnProvision::Refusal LastProvisionRefusal() const {
        return PawnProvision::Refusal::AdmitNotPromoted;
    }
    // WHICH IDENTITY THIS ONE REALLY IS.  Every identity_access:: free function below is keyed on
    // the shipped singleton's ADDRESS, because a static_cast down from a foreign IIdentity is
    // undefined behaviour.  The dress pass does not hand them the singleton: it hands them a
    // ProvisionedIdentity wrapping it, so every one of them answered "not mine" -- SetSlotName
    // refused NoSlotNameTable, ResolvedDisplayName answered false, and every dress failure printed
    // "None: none" because LastRefusal could not reach the refusal that had been recorded.
    //
    // A wrapper answers with the identity it forwards TO; everything else answers with itself, so
    // the default is the honest one and no existing implementation has to change.
    virtual const IIdentity* Underlying() const { return this; }

    virtual bool      SetProfileJson(UserSlot, const Blob& serverProfileJson) = 0;
    virtual bool      SetTeam(UserIndex, uint8_t teamId, uint8_t teamSlot) = 0;
    // The READ side of SetTeam.  Non-pure and refusing by default, for the reason
    // ReleaseReplayUser is: an implementation that cannot answer must say so rather than be
    // forced to invent one.  A refusal leaves both out params untouched -- a confident 0 is team
    // RED, and the free readers below cannot be used instead because they take a raw netgame
    // pointer nothing behind this interface holds.
    virtual bool      TeamOf(UserIndex, uint8_t& teamId, uint8_t& teamSlot) { return false; }
    virtual bool      ApplyLoadout(UserIndex, const Symbol64 slots[20],
                                   uint16_t number, uint16_t numberBody) = 0;
    virtual bool      OverrideAlignment(UserIndex, uint16_t alignment) = 0;
    virtual bool      ReadAppliedItems(UserIndex, Symbol64 out[20]) const = 0;
    virtual UserSlot  SlotOf(UserIndex) const = 0;
    virtual UserIndex IndexOf(UserSlot) const = 0;

protected:
    ~IIdentity() = default;
};

namespace identity_access {

// Why every call has a reason string: each of the six mechanisms below has at least one
// path on which the engine does nothing and logs nothing.  A bool alone reproduces the
// silence this packet exists to remove, so every refusal names itself and the caller logs it.
enum class Refusal : uint8_t {
    None = 0,
    NoNetGame,          // ISessionEntry::NetGame() is null -- no session yet
    BuildMismatch,      // a prologue did not match; no engine call was made
    SlotOutOfRange,     // slot >= ENTITY.USER_SLOT_TABLE_MAX, so the 4-byte tables do not cover it
    NoSuchEntrant,      // the slot table holds 0xFFFF, or the generation disagrees
    ReplayBand,         // the index would be renamed "Replayer %u"; refused, never silently accepted
    EmptyProfile,       // the profile CJson is absent or empty -- breaker 1, before it goes silent
    NoLoadoutInstance,  // the live loadout table holds no instance for this slot
    NoActorComponent,   // the SActorGUID does not resolve to a customisation component
    NotAuthoritative,   // session flags say ApplyCurrentLoadout would return without applying
    ShortInstance,      // fewer engine item entries than the recording carries; reported, never padded
    NoSlotNameTable,    // CR15NetGame + IDENT.SLOTTABLE_OFF is null, or the slot is past the vector's bound
    SlotNameTooLong,    // the recorded name does not fit the engine's inline buffer; refused, never truncated
    NameNotResolved,    // UserDisplayName did not answer, so the rendered name could not be observed
    NoReleasePath,      // the release body is authored, but this build exposes no consumer for it
    RosterUnreadable,   // the entrant count, or the user array it claims, is not in committed memory
    ShortBuffer,        // the caller's buffer is smaller than the value; reported, never quietly cut
    NoJsonWriter,       // RETIRED as a ProfileJsonBytes outcome; kept so the values below keep theirs
    // The profile document is now serialised from the tree by src/engine/entity_json.cpp, so the
    // ways that walk can fail each get their own word here.  A missing engine SERIALISER was never a
    // missing capability: every node is readable through the engine's own accessors, and what these
    // values report is the small set of shapes THIS BUILD'S READ API genuinely cannot express.
    JsonNoRoot,         // IDENT.JSON_FIND_NODE("") answered null -- the document has no root
    JsonNodeUnreadable, // a node pointer inside the document is not in committed memory
    JsonDepthLimit,     // the tree is deeper than the walk's depth bound
    JsonNodeLimit,      // the tree holds more nodes than the walk's node bound
    JsonCycle,          // a node is one of its own ancestors, so the document does not terminate
    JsonArrayNotWalkable,   // a POPULATED array; IDENT.JSON_NEXT_ELEM iterates tag-0 containers only
    JsonNumberNotExact,     // a non-integer number; the per-node accessor is float32 and would re-quantise
    JsonUnknownKind,        // IDENT.JSON_NODE_KIND does not name this node's tag
    JsonChildCountDisagrees,// the child walk and IDENT.JSON_ELEM_COUNT disagree
    JsonStringUnreadable,   // a key or value string pointer is null, unreadable, or unterminated
    JsonStringNotUtf8,      // a key or value is not valid UTF-8, so no canonical JSON encodes it
    JsonTreeChanged,        // the sizing pass and the emitting pass disagreed on the length
    JsonNoSuchKey,          // a segment of a caller's key path names no child of its parent
    StatsNoFillMask,        // ReadPlayerStats was given no fill mask, so absence would be unsayable
    StatsUnreadable,        // the entrant's stat block is not in committed memory
    EmoteNoSystem,          // no R15NetCustomization component system was named for the emote read
    EmoteUnreadable,        // the emote id map, the pool, or the handle it selects is not committed
    EmoteVtableUnreadable,  // the handle's target carries no readable pointer in STATS.EMOTE_VTSLOT
    EmoteNoSeam,            // no entity-layer EmoteSource seam is installed, so nobody can be asked
    ProfileReadOnly,        // the entrant's document is the json db's CACHED one: IDENT.JSON_SET_STRING
                            // and IDENT.JSON_SET_INT both refuse it, log `json db is cached, read only`
                            // and write nothing, and both return void.  L3 #19 F2, by name.
    ProfileNotReadBack,     // the write returned and the engine's OWN reader does not answer the
                            // recorded value back at the key it was written to
    LoadoutsUnreadable,     // the live loadout header, or the instance vector its own count claims,
                            // is not in committed memory.  The walk was NOT STARTED: this is the
                            // word for "could not ask", and it is never a count of zero.
    ProfileNotAttached,     // the entrant carries no profile document, or the one it carries is
                            // still the json db's CACHED one.  Its loadouts describe the cache's
                            // user, not the recording's, so they are not read at all.
    TeamNotApplied,         // ARCH-23 S14.5: the packed role word is no longer written raw.  The
                            // team travels IDENT.SERVER_ASSIGN_TEAM, and this is the name for the
                            // call not binding, the session type refusing it silently, the role
                            // being one the engine does not accept, or the engine's own word not
                            // reading the requested team back afterwards.
    TeamNotRecorded,        // The RECORDING carries no team for this player --
                            // the 8-bit field reads the manifest's absent sentinel.  It is not a
                            // team the engine refused, and it is not a failure of the dress: it is
                            // one step that had no input, and it costs that step only.
    EngineCallReentrant,    // this call would have started an engine transition from INSIDE one.
                            // S14.6: the whole participating arm runs synchronously inside the
                            // call, so a hook of ours that fires in it is refused, never queued.
    AppliedSlotsUnstable,   // the two reads of the customisation slot array disagreed: the engine
                            // wrote the entries between them, so no arm can be attributed.  A
                            // could-not-ask, never a mismatch: blaming a torn read on the
                            // recording is the false verdict this word exists to prevent.

    // Four steps that had no INPUT and one refusal that had no reason.  None of them
    // is the engine saying no, and every one of them used to be reported as something else: a row
    // the capture recorded nothing for read as an empty profile, and a refusal nobody recorded
    // printed as "None: none".
    ProfileNotRecorded,     // the capture carries no profile document for this row at all
    ProfileNotParsed,       // a document was offered and the engine's own parser rejected the bytes
    LoadoutNotRecorded,     // every one of the twenty ids is kUnrecordedItemId: there is no loadout
    SpectatorRow,           // team id at or above IDENT.ALIGN_TEAM_LIMIT -- never a playing entrant
    ReasonNotRecorded,      // the step refused and the identity recorded no reason for it
    LoadoutStepOff,         // playback.dress_loadout is off: the outfit step was not asked to run

    // RECORDER-3 F1.  The SECOND storage for the same document: the lobby entrant record the
    // SNSLobbyEntrantStatev2 handler parses the message's own JSON text into.  On a live server
    // the user-table pointer is filled asynchronously by the profile service and is empty at t=0
    // for every remote entrant, so these two words name the fallback's own failures and must never
    // be confused with the user table's EmptyProfile.
    EntrantRecordUnreadable,  // the lobby member array, or the record this slot claims in it, is
                              // not in committed memory (or the slot is past the published count)
    EntrantRecordEmptyNode,   // the record exists and its +144 node is the engine's EMPTY node --
                              // the message carried no document, which is the else-arm's state

    // The release half now calls the engine's own NSLobbyRemovingEntrant handler, so
    // "it refused" has to say WHICH refusal -- four of these five stop the call being made at all.
    ReleaseNotContained,      // livesafety says this process is not a contained offline session, so
                              // the entrants belong to real people and none of them may be removed
    NotOurEntrant,            // the slot is not in this session's admission ledger.  The ledger is
                              // written only where ProvisionReplayUser's own postcondition passed,
                              // so this is what keeps the leave off the viewer and off every
                              // entrant the plugin did not create
    EntrantIsLocal,           // the entrant is below NETGAME.LOCAL_USER_COUNT_OFF -- the local-user
                              // band.  The engine's handler refuses the same set silently; this
                              // refuses it first and by name, because reaching it would be a bug
    ReleaseCallRefused,       // the byte-anchored direct call did not issue: ReplyBailName on the
                              // reply census says which guard stopped it
    EntrantStillSeated,       // the call WAS issued and the slot still resolves to an entrant that
                              // names it back.  The engine did not free it; nothing may reuse it
};

const char* RefusalText(Refusal);

// WHAT THE CAPTURE WRITES FOR AN ITEM POSITION IT DID NOT RECORD, and what the engine itself means
// by "no model" -- it is the FROM of every model swap in the engine's own log and never the TO.
// Neither the dress nor the read-back may treat it as an item: written it strands the per-entrant
// table the pose loop reads, and scored as a match it reports `20/20 items match` for an entrant
// wearing nothing.  A row whose twenty ids are all this has no recorded loadout at all.
constexpr Symbol64 kUnrecordedItemId = Symbol64(~uint64_t(0));

// The 64-byte live loadout header at CR15NetGame + IDENT.LOADOUT_TABLE_OFF + (slot << SHIFT),
// and the 64-byte instances it owns.  Hand-declared, never generated: DESIGN 4.6 rule 1 --
// a guard that emits both sides of its own comparison can never fail.  The static_asserts
// below compare this declaration against the registry, which is the second derivation.
struct LoadoutHeader {
    void*    instances;      // +0x00  array of LoadoutInstance
    uint8_t  gap08[0x28];    // +0x08
    uint64_t instanceCount;  // +0x30  IDENT.LOADOUT_COUNT_OFF -- a count: the walk compares against it
    uint16_t number;         // +0x38  IDENT.LOADOUT_NUMBER_OFF
    uint16_t numberBody;     // +0x3A  IDENT.LOADOUT_NUMBER_BODY_OFF
    uint32_t trailing;       // +0x3C  copied by IDENT.LOADOUT_ASSIGN; purpose not established
};

struct LoadoutItem {         // 16 bytes; the pair the apply loop reads
    Symbol64 slotId;         // +0x00  passed to LoadAndApplyItem as its slot argument
    Symbol64 itemId;         // +0x08  the item that gets applied
};

struct LoadoutInstance {
    Symbol64 name;           // +0x00  matched against the name ApplyLoadoutItems is called with
    LoadoutItem* items;      // +0x08
    uint8_t  gap10[0x28];    // +0x10
    uint64_t itemCount;      // +0x38  IDENT.INSTANCE_ITEM_COUNT_OFF -- a count: the loop bound
};

// The 48-byte customisation slot entry at record + IDENT.CUST_SLOTS_OFF.  The stride is not
// a literal anywhere in the build -- it is `lea rdx,[rcx+rcx*2]; shl rdx,4` -- so the registry
// stores that instruction sequence as IDENT.CUST_SLOT_WALK_BYTES and 48 is derived here.
struct CustSlotEntry {
    uint64_t key;            // +0x00
    uint32_t flags;          // +0x08  bit IDENT.CUST_SLOT_APPLIED_BIT: already applied
    uint16_t alignmentReq;   // +0x0C  0xFFFF, or the alignment this entry requires
    uint16_t pad0E;
    Symbol64 item;           // +0x10  what is CURRENTLY applied -- the post-apply truth
    Symbol64 previousItem;   // +0x18
    Symbol64 pendingItem;    // +0x20  IDENT.CUST_SLOT_PENDING_OFF; written instead of applying
                             //        when flags bit IDENT.CUST_SLOT_APPLIED_BIT is set
    uint32_t param;          // +0x28
    uint32_t pad2C;
};

// The three fields IDENT.LOAD_AND_APPLY_ITEM branches on, published beside the item so a reader
// can tell its four outcomes apart.  The engine's own order, from its decompilation:
//
//   if (alignmentReq == 0xFFFF || alignmentReq == Alignment(cs, entity)) {   // the OUTER gate
//       if (item == new)   { pendingItem = -1;  return; }   // already wearing it
//       if (flags & 1)     { pendingItem = new; return; }   // latched, applied later
//       previousItem = item; item = new; pendingItem = -1;  // the only write of +0x10
//   }                                                       // else: nothing happens at all
//
// so `item` alone collapses three silent outcomes and "never reached" into one value.
//
// THE GATE HAS TWO CONJUNCTS AND BOTH MUST BE ASKED.  It skips the entry only
// when `alignmentReq != 0xFFFF` AND `alignmentReq != Alignment(cs, entity)` -- `jnz
// loc_140D0DBA2 @0x140D0D39C`.  A set `alignmentReq` on its own says nothing: if the entrant
// MEETS the requirement the gate passed, an inner arm ran, and an item that still is not the
// recorded one is a substitution or a slot the apply never visited -- a mismatch, not a skip.
struct AppliedSlotState {
    Symbol64 pendingItem;    // CustSlotEntry.pendingItem at +0x20
    uint32_t flags;          // CustSlotEntry.flags at +0x08
    uint16_t alignmentReq;   // CustSlotEntry.alignmentReq at +0x0C
    uint16_t pad;
};

// No alignment requirement at all -- the outer gate's own literal (`mov eax, 0FFFFh` at
// 0x140D0D37F, compared against +0x0C at 0x140D0D384).
constexpr uint16_t kSlotAlignmentNone = 0xFFFFu;  // facts-ok: IDENT.CUST_SLOT_ALIGNREQ_OFF V-PRIMARY -- the gate compares +0x0C against this immediate (mov eax, 0FFFFh @0x140D0D37F, cmp @0x140D0D384); it is that row's own sentinel, not NETVAR_SLOT_NONE

// The ENTITY's alignment -- the gate's second operand, IDENT.CUSTOMISATION_ALIGNMENT itself
// (`call sub_140CA7BB0 @0x140D0D391`) called with the same customisation component and handle the
// rest of this module resolves.  It is the engine's own answer, fallback arm included: that
// function reads the u16 override at IDENT.CUST_OVERRIDE_ALIGN_OFF in the entity's 448-byte
// record, returns it when it is below IDENT.ALIGN_TEAM_LIMIT, and otherwise goes through a
// virtual call at record+0xF8 that no offline read reproduces -- which is exactly why the CALL is
// the reader here and the field read is not.  False means the component or the entry point could
// not be resolved in this image: could-not-ask, never an alignment.
bool EntityAlignment(void* ng, UserIndex, uint16_t& out, Refusal* why = nullptr);

// The 80-byte reference IDENT.ACTOR_COMPONENT_REF builds.  The two field positions are
// derived from IDENT.ACTOR_REF_USE_BYTES: the struct is built at rsp+0x30 and its
// component-system pointer read back from rsp+0x70, two slots before the handle.
struct ActorComponentRef {
    uint8_t  opaque[0x40];
    void*    componentSystem;  // +0x40
    uint32_t handle;           // +0x48
    uint32_t pad4C;
};

// -------------------------------------------------------------------------------------
// The engine bindings.  Each checks its 20-byte prologue once, before the first call, and
// refuses rather than jumping into the middle of some other build's function.
// -------------------------------------------------------------------------------------

// True when every entry point this module calls matches its recorded prologue IN THE MAPPED
// IMAGE, and every one of them resolved to an address inside it.  Callers that skip this get
// Refusal::BuildMismatch instead of a jump into the wrong build -- or, before the rebase was
// added, a jump into an address that is not in the process at all.
bool BuildMatches();

// The one diagnostic that has to exist: which entry points failed, for the log line.
const char* BuildMismatchDetail();

// The site table, enumerable so a check can walk it.  This is the countermeasure to C-1: the
// defect was that every address was a literal, and a set of literals scattered through the
// file is exactly what no test can inspect.  `Resolved` is what the module will actually call
// or read -- null until BuildMatches() has run, and null for any site it refused.
size_t      EngineSiteCount();
const char* EngineSiteName(size_t i);
uint64_t    EngineSiteVa(size_t i);          // the recorded VA, against PE_IMAGE_BASE
const void* EngineSiteResolved(size_t i);    // buildguard::MappedAddress of it, or null

// The two index spaces.  These are the whole reason UserIndex and UserSlot do not convert.
bool SlotOfIndex(void* ng, UserIndex, UserSlot& out);
bool IndexOfSlot(void* ng, UserSlot, UserIndex& out);

// [NONREPLAY_COUNT, ENTRANT_COUNT) is the band UserDisplayName renames.
bool InReplayBand(void* ng, UserIndex);

// The pointer everything hangs off.  Null or empty is breaker 1, caught here rather than
// discovered as a default-looking avatar.
void* ProfileJson(void* ng, UserIndex);
bool  ProfileJsonUsable(void* ng, UserIndex);

// Bit 0x4 of the word CR15NetGame + IDENT.SESSION_FLAGS_PTR_OFF points at makes
// ApplyCurrentLoadout return without applying anything.  Queried, never patched.
bool CosmeticsWouldApply(void* ng);

LoadoutHeader*  LiveLoadout(void* ng, UserSlot);

// THE PER-USER LOADOUT CONTAINER -- the one IDENT.LOADOUT_FROM_PROFILE_VA fills out of the profile
// document and IDENT.LOADOUT_ASSIGN publishes FROM: CR15NetGame + USER.PROFILE_OFF +
// USER.PROFILE_STRIDE*SLOT + USER.LOADOUT_OFF.
//
// Indexed by the user's SLOT.  Both engine sites do it that way -- IDENT.PROFILE_RECEIVE_VA:109
// derives `19720 * (*(WORD*)(user + 376) & IDENT.USER_SLOT_MASK)` and uses it for the parse
// destination, the publish source and the published flag alike -- and PublishLoadout used the
// entrant INDEX for the source while using the slot for the destination.  At one entrant slot
// equals index, so it never showed; the moment they differ it copies a different 19,720-byte
// block into the live table and the entrant wears another user's loadout.  Null when `ng` is
// null, when the slot is past the table, or when the block is not in committed memory.
void* LoadoutContainer(void* ng, UserSlot);
LoadoutInstance* FindInstance(LoadoutHeader*, Symbol64 name);

// The id a position holds when nothing is applied there.  The same zero VacateSlot writes across
// all twenty, so this build has ONE answer to "no item in this slot" and both paths write it.
constexpr Symbol64 kNoItemId = Symbol64(0);

// What ApplyLoadout writes into the engine's instance, extracted so the rule can be checked
// byte-for-byte without a game behind it: the ladder above it needs a live CR15NetGame and every
// offline driver refuses three steps earlier, which left the sentinel guard with no test at all.
// Writes `count` positions of `inst` and returns how many carried a RECORDED id.
struct ItemWrite {
    uint32_t recorded    = 0;   // positions the recording filled, written as recorded
    uint32_t cleared     = 0;   // positions it did not, and that this call stored kNoItemId into
    uint32_t alreadyEmpty = 0;  // unrecorded and already at kNoItemId: not stored again
};
ItemWrite WriteRecordedItems(LoadoutInstance& inst, const Symbol64 slots[20], uint64_t count);

// NAME-ALIGN (DIAG-SEAT-DRIVE core fix).  The engine's own enable-time loadout apply
// (IDENT.APPLY_LOADOUT_ITEMS sub_140154C00, via its profile-arm sub_1401544A0) selects the
// instance whose +0 name equals *IDENT.LOADOUT_DEFAULT_INSTANCE_PTR -- the startup constant
// SymbolFromName("unified") -- and prints "Unknown loadout instance name" and applies ZERO items
// on no match.  The engine's profile parser (IDENT.LOADOUT_FROM_PROFILE sub_140C4C920) names each
// instance verbatim after its own `loadout|instances|<NAME>` child, so a recording whose active
// instance is not literally "unified" lands its items under a symbol the engine never probes --
// the "items=N vs engine loaded 0" two-instance discrepancy.  When no instance already carries
// `engineName` with items AND exactly one populated instance exists under another name, this
// aliases that instance's +0 name onto `engineName` so the engine's OWN apply finds it non-empty.
// A single field write into a loadout instance the publish just installed -- never a seat
// descriptor and never a render puppet.  Returns true when the container now carries a populated
// instance under `engineName` (aligned or already so); false when it could not be proved, when
// several instances are populated (it refuses to guess), or when nothing is populated.
bool AlignPublishedInstanceName(void* container, Symbol64 engineName);

// Fills `out` from the SActorGUID at IDENT.USER_ACTOR_GUID_OFF.  False when the actor does
// not resolve, which is the difference between "no cosmetics" and "wrong cosmetics".
bool ResolveComponent(void* ng, UserIndex, ActorComponentRef& out);
CustSlotEntry* CustSlots(const ActorComponentRef&, uint64_t& countOut);

// The applied slot and the item that is in it.  Deliberately NOT LoadoutItem: that struct is the
// engine's 16-byte element inside a LoadoutInstance and its `slotId` comes from the profile
// document, while this pair comes from the live customisation component.  Two sources, two types,
// so a reader cannot mistake one for the other.
struct LoadoutPair {
    Symbol64 slotKey;   // CustSlotEntry.key at +0x00 -- the engine's own key for the slot
    Symbol64 itemId;    // CustSlotEntry.item at +0x10 -- the post-apply truth
};

// The walk itself, with no engine resolution in it, so a fixture can plant entries and the key
// extraction is a claim a unit test can falsify -- BuildMatches() is false off a real image, so
// ReadAppliedPairs below refuses before it ever reaches an entry.  Returns the number written
// (count clamped to 20); `out` is zeroed first, so a short component leaves no stale key.
// `state`, when given, receives the three branch fields of the same entry.
uint64_t PairsFromSlots(const CustSlotEntry* slots, uint64_t count, LoadoutPair out[20],
                        AppliedSlotState* state = nullptr);

// The pair form of ReadAppliedItems, and the one a recording must use.  The item-only reader
// throws the key away and leaves position as the only slot evidence, which is a guess the moment
// the component's order is not the name table's -- and that order is NOT established.
//
// `n` is the number of entries the component actually holds, clamped to 20; it is written only on
// success, so "the component holds no slots" (true, n == 0) and "we could not ask" (false, and
// `why` names which) stay different answers.  `out` is zeroed on entry either way, so a caller
// that ignores the bool cannot read a stale key.
// `held` is the component's UNCLAMPED entry count.  `n` is min(count, 20), so
// `n == 20` cannot by itself tell a full loadout from a wider component cut down to fit; a caller
// that wants to know which it has must ask for this one too.
// `state`, when given, is filled from a SECOND walk and the two are compared field by field.  That
// walk RESOLVES THE COMPONENT AGAIN: re-walking the pointer the first resolve
// returned re-reads whatever that pointer now holds, but it cannot see the engine swap the record
// itself, and it left the disagreement branch unreachable for every fixture.  If the two walks
// disagree in count or in any field the call FAILS with Refusal::AppliedSlotsUnstable rather than
// publishing an attribution derived from two different moments of engine state.
bool ReadAppliedPairs(void* ng, UserIndex, LoadoutPair out[20], uint64_t& n, uint64_t* held = nullptr,
                      Refusal* why = nullptr, AppliedSlotState* state = nullptr);

// THE SEAM THE TWO READS NEED.  ReadAppliedPairs refuses off a real image
// (BuildMatches() is false), and against a planted fixture both of its walks see the same bytes by
// construction -- so the disagreement branch, and the mutant that guards it, had nothing that
// could redden them.  When a walk is injected, ReadAppliedPairs calls it for EACH read instead of
// resolving the component, and a fixture that answers differently the second time is the engine
// writing between the two.  Not #ifdef-guarded, for the reason config.h's LoadFromTextForTest is
// not: a seam compiled out of the shipped build is a seam the shipped build is not proved through.
// `n` is the entry count the walk wrote; false means "this walk could not resolve".
using AppliedSlotsWalkFn = bool (*)(void* ng, UserIndex, LoadoutPair out[20],
                                    AppliedSlotState* state, uint64_t& n, uint64_t* held);
void AppliedSlotsInjectWalkForTest(AppliedSlotsWalkFn);

// ---------------------------------------------------------------------------------------------
// THE LOADOUT DRESS -- ARCH-23 S14.8 / S14.8b / S14.8c, steps L1..L6.
//
// L1 VERIFIES the recorded document; it authors nothing.  Both halves of the engine's
// {slotId, itemId} pair are IDENT.SYMBOL_FROM_NAME_VA hashes of strings taken out of the profile
// document, and that hash is one-way, so there is no item NAME to author from a recording -- the
// names are already in the recorded document and L1's whole job is to prove the two sides agree
// before a single engine call is made against them.
// ---------------------------------------------------------------------------------------------

// The capture's "could not ask" for a per-entrant pair count.  It is the same 0xFFFF the identity
// block writes; the block's own constant lives in src/capture and this layer may not include it,
// so the caller that bridges the two owes the static_assert.
constexpr uint16_t kLoadoutPairCountUnknown = uint16_t(~uint16_t(0));

// "no slot" for the diagnostic field below -- all ones, derived rather than transcribed.
constexpr uint16_t kLoadoutNoSlot = uint16_t(~uint16_t(0));

// The recorded side, and nothing else.  Never the document we authored from it (S14.8b C4): a
// comparison of an authored buffer against itself is green when nothing reached the engine.
// EVERY FIELD DEFAULTS TO ITS COULD-NOT-ASK VALUE, not to zero.  A value-initialised
// RecordedLoadout is a recording NOBODY HAS READ YET, and zero is a real answer for three of
// these fields: `filledPairCount = 0` says the entrant wore nothing, which is a claim, and it is
// the claim L1 would refuse as LoadoutNoSlotsRecorded where LoadoutPairCountUnknown is the truth.
struct RecordedLoadout {
    LoadoutPair pairs[20] = {};     // {slotKey, itemId} as the recording holds them
#ifdef W2P1_MUTATE_DEFAULT_COUNT_ZERO
    uint16_t filledPairCount = 0;   // the defect: a claim where a could-not-ask belongs
#else
    uint16_t filledPairCount = kLoadoutPairCountUnknown;   // could-not-ask, never zero
#endif
    uint16_t number = 0;
    uint16_t numberBody = 0;
    // The alignment the RECORDING holds for this entrant (PlayerRecord.alignment16), or
    // kSlotAlignmentNone for a recording that never carried one.  L5 compares it against the live
    // EntityAlignment before scoring any gated slot: the gate's answer decides which slots the
    // engine skipped, so a live alignment that is not the recorded one means the replayed entrant
    // is being dressed against a different requirement than the one that produced the recording.
    uint16_t alignment = kSlotAlignmentNone;
    bool hasSlotKeys = false;       // v1 identity block -> LoadoutSlotKeysNotRecorded
    bool documentRecorded = false;  // entrantJson present AND W2-A-2 did not answer ProfileRefused
};

// The live instance's item count could not be read.  A count is a number and 0 is a legal one --
// an instance the parse filled with nothing -- so "could not ask" needs a value of its own.
constexpr uint32_t kLoadoutItemsUnknown = 0xFFFFFFFFu;  // facts-ok: PHYS.ID_DEAD PC_PRESENT, so gen_facts emits no value -- this is a u32 count sentinel, not that id

enum class LoadoutRefusal : uint8_t {
    None = 0,
    DocumentNotRecorded,   // no entrantJson, or the capture answered ProfileRefused for it
    SlotKeysNotRecorded,   // a v1 identity block: it carries items by POSITION and no slot keys
    PairCountUnknown,      // the recorded count is the sentinel: could-not-ask, never zero
    NoSlotsRecorded,       // the recorded count is 0 -- and 0/0 is never a pass
    PairCountMismatch,     // the count disagrees with the non-empty pairs actually present
    InstanceNameUnknown,   // *IDENT.LOADOUT_DEFAULT_INSTANCE_PTR did not resolve, or is not the
                           // segment the engine's own path template carries
    SlotKeyUnresolved,     // a recorded slotKey is none of the twenty names' hashes
    SlotNodeAbsent,        // the document has no node at loadout|instances|<inst>|slots|<name>.
                           // RETAINED, not produced: L1 names that case PairUnfilled since
                           // W2-P1-35, and runs recorded before it are read back by this name
    SlotValueNotString,    // the node is there and its kind is not String: nothing to hash
    PairUnfilled,          // the capture marked this pair FILLED and the document has no node
                           // for its slot -- the two halves of the recording disagree.  A
                           // recorded -1 is an EMPTY slot: skipped and counted, never refused
    DocumentPairsSkew,     // the node hashes and the recorded itemId disagree -- a coherence
                           // fault between two engine objects, not a missing datum
    SentinelCollision,     // a slot name hashes to 0 or -1, which would make the padding and the
                           // unfilled sentinel ambiguous even for a filled slot
    NotBound,              // an entry point this step calls did not bind in this build
    DocumentNotAttached,   // L2: the entrant's profile pointer does not read back as our document
    DocumentNotUsable,     // L2: the engine's own IDENT.JSON_IS_EMPTY answers non-zero
    ContainerUnreadable,   // L3/L4: the per-user loadout container is not in committed memory
    ParseDidNotFill,       // L3: the container's own header does not read back the recorded
                           // number pair, or holds no instance
    PublishFlagUnexpected, // L4: the engine already considers this user's loadout published
    PublishDidNotTake,     // L4: the live per-slot count did not read back as the container's
    ActorNotEnabled,       // L5: hop 2's own re-read says the entrant's actor is not enabled
    NotAuthoritative,      // L5: the session flags' no-cosmetics bit is set
    AppliedPairsUnreadable,// L5: the customisation component could not be read back
    AppliedPairsMismatch,  // L5: recorded-vs-engine by slot key did not come out 20/20
    AlignmentRefused,      // L6: the alignment/chassis apply refused
    AlignmentUnverified,   // L5: a slot carries an alignment requirement and the gate's SECOND
                           // operand could not be read -- IDENT.CUSTOMISATION_ALIGNMENT did not
                           // bind, or the customisation component did not resolve.  The engine's
                           // skip and its substitution look identical without it, so the pass
                           // refuses rather than scoring the slot as one or the other
    AlignmentUnrecorded,   // L5: a slot was skipped by the engine's alignment gate and the
                           // recording carries no alignment for this entrant, so the live value
                           // the skip was decided by is uncorroborated.  An empty cosmetic slot
                           // is a VISIBLE difference; accepted on the engine's word alone it
                           // ships as a pass, so the capture must have recorded the alignment
    AlignmentMismatch,     // L5: the live alignment is not the one the recording holds for this
                           // entrant, so every gated slot would be decided against the wrong
                           // requirement -- the dress is refused before any slot is scored
    EngineCallReentrant,   // this sequence would have started an engine transition from INSIDE one
    EngineUnbound,         // BindLoadoutEngine did not resolve one of the entry points L1..L6
                           // calls in THIS image: a wrong or absent surface, refused whole rather
                           // than driven with a hole in it
    RecordingNotAsked,     // no recorded-identity source was provided, so the recording was never
                           // asked at all.  Never a synonym for "the recording said no"
    NotPublishedBeforeEnable,  // DIAG-MODEL-SWAP: the engine drives a user's loadout apply off
                           // the SOCIAL ENABLE event, and an enable issued while the live
                           // per-slot table still reads `Loadouts : 0` refuses for want of
                           // instances and is never re-run.  This value means the publish
                           // half had left no instance behind when the apply half asked, so
                           // the enable in between had nothing to apply
};

// The refusal's own identifier, so a census line names it and a test matches on it.
const char* LoadoutRefusalName(LoadoutRefusal);

// Everything L1 can decide from the RECORDING alone, with no document, no engine and no call.
// L1 itself runs this first, so the census and the verification cannot come to two opinions
// about whether a recording is even fit to verify.  None means "ask the document next".
LoadoutRefusal RecordedLoadoutRefusal(const RecordedLoadout&);

// The twenty slot names IDENT.LOADOUT_DOC_AUTHOR_VA writes, in its own order, out of the
// registry.  Position here is the registry's order and is never a slot index.
uint16_t    LoadoutSlotNameCount();
const char* LoadoutSlotName(uint16_t i);

// The instance segment of the engine's own path template IDENT.LOADOUT_INSTANCE_PATH_FMT
// ("loadout|instances|unified|slots|%s") -- taken from the template rather than spelled a second
// time, and still checked against the live *IDENT.LOADOUT_DEFAULT_INSTANCE_PTR before use.
bool LoadoutTemplateInstance(char* out, size_t cap);

// The live per-slot loadout instance count, read where the engine's own entrant dump reads
// the number it prints as `Loadouts : N`.  kLoadoutInstancesUnreadable when the table is not
// in committed memory -- not the same answer as a count of zero and never to be read as one:
// zero says the enable will refuse, unreadable says nobody could ask.
constexpr uint64_t kLoadoutInstancesUnreadable = ~uint64_t(0);

// THE FOUR GATES IDENT.APPLY_LOADOUT_ITEMS CLOSES AFTER ITS `Applying loadout items` PRINT.
// RUN-34 caught the entrant reaching that print and stopping there, silently: the engine skips
// its whole item loop, with no message of any kind, when the actor does not resolve, when the
// ref carries no component system, when that system does not own the ref's handle, or when the
// matched instance holds no item pairs.  A publish that leaves any of the four shut turns the
// entrant's enable -- a one-shot event nothing re-runs -- into a body-less apply, so they are
// read BEFORE the enable is issued and named one by one.  `Unasked` is the could-not-ask value
// and is never spelled the same as a gate that answered no.
enum class LoadoutApplyGate : uint8_t {
    // ZERO IS `Unasked` DELIBERATELY.  This value is latched per slot in a struct that is
    // zero-initialised on Arm(), and a slot that has never run a publish half must not read
    // back as one whose four gates all passed -- nor as one that answered no.
    Unasked = 0,      // no netgame, no instance name, an unbound entry point, or memory that
                      // does not read back -- never to be reported as a gate that said no
    Ready,            // all four answered yes: the engine's loop will run for this entrant
    InstanceEmpty,    // gate (d): the matched instance's item-pair count is 0
    ActorUnresolved,  // gate (a): IDENT.ACTOR_COMPONENT_REF resolved no actor for the guid
    CsPointerNull,    // gate (b): the ref resolved an actor that carries no component system
    CompKeyInvalid,   // gate (c): the component system does not own the ref's handle
};
const char* LoadoutApplyGateName(LoadoutApplyGate);

// The pair-count could-not-ask value.  0 is a legal count -- an instance the parse filled with
// nothing -- so the unreadable case needs a value of its own, exactly as the instance count does.
constexpr uint64_t kLoadoutPairsUnknown = ~uint64_t(0);

// What each gate actually answered, published so the census line prints the readings the verdict
// was decided from rather than re-deriving them from the verdict.
struct LoadoutApplyGates {
    uint64_t    instances = kLoadoutInstancesUnreadable;  // the live per-slot instance count
    uint64_t    pairs     = kLoadoutPairsUnknown;  // IDENT.INSTANCE_ITEM_COUNT_OFF of the instance
                                                   // whose name the engine's apply will match
    const void* actor     = nullptr;   // the ref's own actor field, gate (a)'s operand
    const void* compCs    = nullptr;   // the ref's component system, gate (b)'s operand
    uint32_t    compKey   = 0;  // the ref handle, gate (c) second argument.  No sentinel:
                                //  `verdict` is what says the reading never happened
    bool        compValidAsked = false;   // whether gate (c) was reached AND could be asked
    bool        compValid      = false;   // what it answered when it was
    // OBSERVE-ONLY (AUD-W2-P1-45 sec B): whether IDENT.APPLY_LOADOUT_ITEMS would run the entrant's
    // "Load and apply" INLINE or park it on the component system's CDeferredMethodQueue -- a queue
    // a spectator session may never drain.  These read only; nothing here touches the queue.
    bool        queueAsked  = false;  // the subsystem+queue read back and were asked
    uint32_t    inlineApply = 0;      // DEFERREDQUEUE.INLINE_FLAG_OFF: nonzero -> apply runs inline
    int64_t     queueDepth  = 0;      // parked-request count (DEFERREDQUEUE.HEAD_OFF byte head / stride)
    int64_t     queueCap    = 0;      // capacity in requests (DEFERREDQUEUE.CAP_OFF byte cap / stride)
    LoadoutApplyGate verdict   = LoadoutApplyGate::Unasked;
};

// The engine's inline-vs-deferred decision for one component system, read the way
// IDENT.APPLY_LOADOUT_ITEMS branches on it: *(cs + APPLY_QUEUE_SUBSYS_OFF) -> the queue, whose
// inline flag / head / cap sit at DEFERREDQUEUE.{INLINE_FLAG,HEAD,CAP}_OFF.  Pure pointer walk
// behind a live-page probe; fills the four queue fields of `out` and returns queueAsked.  Exposed
// so a must-fail test can drive it against a synthetic component system with no engine loaded.
bool ReadApplyQueueState(const void* compCs, LoadoutApplyGates& out);

// The pair-count verdict, gate (d), factored out so a test can pin it without the three actor-side
// gates in front of it (which need a live image): 0 pairs is InstanceEmpty, else Ready.
LoadoutApplyGate LoadoutVerdictFromPairs(uint64_t pairs);

// Copies the 16-byte actor guid to a caller-owned local, canonicalises it in place through
// `canon` exactly as both engine build sites do, then builds the ref with `build`.  The copy is
// mandatory: ENGINE.ACTOR_GUID_CANON_VA writes the canonical guid back through its argument, so
// handing it the engine's own live guid field would rewrite the engine's copy.  `canon` may be
// null (an image with no such site), in which case the raw copy is built.  Exposed for the
// must-fail test that asserts the canon ran over a COPY before the build saw the guid.
using FnActorComponentRefBuild = void* (*)(void*, void*, const void*, int64_t);
using FnActorGuidCanon         = char  (*)(void*, void*);
bool BuildRefFromGuidCopy(FnActorComponentRefBuild build, FnActorGuidCanon canon,
                          void* game, const void* liveGuid, ActorComponentRef& out);

// Reads all four, in the engine's own order, over the engine's own expressions.  Issues exactly
// one engine call -- IDENT.ACTOR_REF_IS_VALID_VA, prologue-guarded like every other -- and it is
// a predicate: it writes nothing and starts no transition.  The verdict is also stored in `out`.
LoadoutApplyGate ReadLoadoutApplyGates(void* ng, UserSlot slot, UserIndex index,
                                       uint64_t instanceName, LoadoutApplyGates& out);

// The instance name IDENT.APPLY_LOADOUT_ITEMS matches on, read in vivo out of
// IDENT.LOADOUT_DEFAULT_INSTANCE_PTR.  0 means it did not resolve -- never a guess at "unified".
uint64_t LoadoutInstanceName();

// Every engine call L1..L6 makes, as a table, for the same reason HopCallTable is one: each of
// these steps is a claim about WHICH call ran with WHICH argument, and a build that skipped one
// has to be able to go RED.  BindLoadoutEngine resolves each through the same prologue-checked
// Guarded<> the rest of this module uses; a member left null REFUSES its step (NotBound) rather
// than falling through to an address this process may not have.
struct LoadoutEngine {
    // L1.  The recorded document, already parsed, read-only.  Null: LoadoutDocumentNotRecorded.
    const entity_json::CJsonReader* doc = nullptr;
    // IDENT.SYMBOL_FROM_NAME_VA(str, seed, withCallback, maxChars, cbArg).  Called with
    // seed = -1 (both engine sites) and withCallback = 0: what that callback does is undecoded,
    // and a call whose side effect cannot be named has no place in a verification.
    uint64_t (*hash)(const char* s, uint64_t seed, int32_t withCallback, int32_t maxChars,
                     void* cbArg) = nullptr;
    // *IDENT.LOADOUT_DEFAULT_INSTANCE_PTR, read in vivo.  It is a Symbol64 -- the name the
    // instance search matches on -- so L1 hashes the template's own segment and requires the two
    // to be equal rather than spelling the instance a second time.
    uint64_t (*instanceSymbol)() = nullptr;
    // NEVER CALLED.  It is here so ..._VERIFY_THEN_AUTHOR can be written at all: an "authors
    // nothing" promise with no writer in reach is a promise no test can falsify.  The shipped
    // binding leaves it null and the fixture counts its calls.
    void (*setString)(void* doc, const char* path, const char* value) = nullptr;
    // OUTFITS.  IDENT.LOADOUT_DOC_SET_SLOT_VA: the engine's OWN per-slot document
    // writer.  Handed a Symbol64 that is not -1 it reads only the document at holder[0], turns the
    // id back into the item's NAME through the engine's own symbol->name registry and writes that
    // name at "loadout|instances|unified|slots|<slotName>".  The two words behind the document
    // steer the RANDOM default it invents for an UNWORN (-1) slot; this project never hands it one.
    void (*setSlotFromId)(void* holder, const uint64_t* itemId, const char* slotName) = nullptr;

    void* (*attach)(void* ng, uint16_t index, void* doc) = nullptr;      // L2
    void* (*attachedDoc)(void* ng, uint16_t index) = nullptr;            // L2's read-back
    bool  (*docUsable)(void* ng, uint16_t index) = nullptr;              // IDENT.JSON_IS_EMPTY == 0
    void  (*parse)(void* container, void* doc) = nullptr;                // L3
    void* (*publish)(void* dst, const void* src) = nullptr;              // L4
    // L5's gate.  It is hop 2's OWN pointer and its OWN ctx, taken straight off ActorHops rather
    // than bound a second time: two bindings of one read can answer differently about one
    // entrant, and then the gate and the hop disagree about whether the actor is enabled.
    void* actorCtx = nullptr;
    bool (*actorEnabled)(void* ctx, uint16_t index) = nullptr;
    void  (*applyItems)(void* ng, uint16_t index) = nullptr;             // L5
    bool  (*applyAlignment)(void* ng, uint16_t index) = nullptr;         // L6
    // L7 (DIAG-MODEL-INSTANTIATE, Lever A) -- re-fire the engine's OWN productive loadout apply:
    // IDENT.APPLY_ALIGNMENT_CHANGE (sub_140CAA5F0) -> APPLY_CURRENT_LOADOUT (sub_140CAB3B0), whose
    // worn-record loop calls IDENT.LOAD_AND_APPLY (sub_140D0D170) + Apply Chassis (sub_140CABF10)
    // per entry, so a NON-EMPTY name-matched loadout instance loads the asset, installs the
    // InstanceModel CI and swaps -- the same path the owner built its body through.  Driving
    // Apply Chassis DIRECTLY (ENG-W2-P1-51/53) was the wrong re-point: Apply Chassis only SKINS an
    // existing model-CI and its readiness gate (the vtable[536] the chain shares) refuses silently
    // on an entrant whose InstanceModel is absent, so nothing was ever created.  Bound to the SAME
    // resolve-then-call as applyAlignment.  The caller drives this ONCE the engine-selected
    // instance's +56 itemCount is non-empty, holding/re-firing until it lands.
    bool  (*driveModel)(void* ng, uint16_t index) = nullptr;  // L7
    // The ENGINE side of the acceptance: the customisation component's own {key, item} entries
    // after the apply.  Never the container, and never our authored array.
    // `state` carries the branch fields of the same entries, so the acceptance can score each
    // pair by the arm the engine took instead of by the one field three of its four outcomes
    // leave untouched.
    bool  (*readAppliedPairs)(void* ng, uint16_t index, LoadoutPair out[20], uint64_t& n,
                              AppliedSlotState state[20]) = nullptr;
    // The gate's SECOND operand: IDENT.CUSTOMISATION_ALIGNMENT for this entrant.  Asked only when
    // a compared slot actually carries a requirement, so an entrant whose twenty slots are all
    // 0xFFFF is dressed without it.  False = could not ask -> LoadoutRefusal::AlignmentUnverified.
    bool  (*entityAlignment)(void* ng, uint16_t index, uint16_t& out) = nullptr;
    // THE FOUR PRE-ENABLE GATES, on the surface rather than called directly, for the same reason
    // every other reading here is: the sequence must be drivable against a netgame this process
    // built, and a reader wired straight into the live registry can only ever answer `Unasked`
    // offline -- which is exactly the value a test of the gates must not be handed.
    // Null is not a gate saying no: it leaves the verdict Unasked.
    LoadoutApplyGate (*applyGates)(void* ng, uint16_t slot, uint16_t index,
                                   LoadoutApplyGates& out) = nullptr;
};

// THE SHIPPED BINDING.  Guarded<> over IDENT.SYMBOL_FROM_NAME_VA, IDENT.LOADOUT_FROM_PROFILE_VA,
// IDENT.LOADOUT_ASSIGN and IDENT.APPLY_LOADOUT_ITEMS.ENTRY_PROFILE_ARM, the qword at
// IDENT.LOADOUT_DEFAULT_INSTANCE_PTR through buildguard::MappedAddress, and trampolines over the
// reads this module already owns.  `actorEnabled` is taken from `hops` -- hop 2's own function
// pointer and ctx -- so the gate and the hop cannot answer separately about one entrant.
// `doc` is NOT bound here: the document is per entrant, not a property of the image.
// False means at least one entry point did not resolve in THIS image; the caller refuses
// LoadoutRefusal::EngineUnbound and drives nothing.  `out` is left with the members that did
// resolve, so a report can name which one was missing.
bool BindLoadoutEngine(LoadoutEngine& out, const PawnProvision::ActorHops& hops);

// The recorded entrant document, parsed into a document this process owns, with a reader over
// this build's own JSON accessors.  L2 attaches `out.json` and nothing else may; a caller that
// gets true and then refuses owes ReleaseLoadoutDocument.
bool BindLoadoutDocument(entity_json::CJsonReader& out, const Blob& recordedJson);
void ReleaseLoadoutDocument(entity_json::CJsonReader& doc);

// ---- OUTFITS: a document for a capture that recorded none -------------------------
//
// The capture carries the twenty (slot key, item id) pairs for every entrant but, on four of five
// rows, no profile DOCUMENT -- and the document is what the engine builds a loadout INSTANCE from,
// so `Loadouts : 0` and nothing to dress.  These two write the document the engine was never sent,
// out of the recording and nothing else: the skeleton below in text, then the engine's own per-slot
// writer for the items, because a Symbol64 cannot be spelled back into a name by us (the hash is
// one-way) while the engine keeps a symbol->name registry of every name it has ever hashed.

// What the authoring managed, per entrant, so the log names the losses instead of implying twenty.
constexpr uint16_t kAuthoredCountUnasked = uint16_t(~uint16_t(0));
struct AuthoredLoadout {
    uint16_t written = 0;          // pairs the engine's writer was driven for
    uint16_t unfilled = 0;         // recorded pairs whose item is the engine's -1 "wears nothing"
    uint16_t unresolvedSlot = 0;   // recorded slot keys none of the twenty engine names hash to
    uint16_t unresolvedItem = 0;   // ids the engine's own registry could not name -> not written
    uint64_t firstUnresolvedSlotKey = 0;
    uint64_t firstUnresolvedItemId = 0;
    uint16_t inDocument = kAuthoredCountUnasked;  // slots the document actually holds afterwards
    bool     nameOmitted = false;  // the recorded display name was not valid UTF-8, so no key
};

// The profile document the recording can support: the display name, both jersey numbers and an
// EMPTY instance object under the engine's own instance name.  Returns the length written, 0 when
// it would not fit or the instance name did not resolve.  NUL-terminates.  Every value comes from
// the recording; nothing is defaulted in.
size_t AuthorProfileDocument(const char* displayName, uint16_t number, uint16_t numberBody,
                             char* out, size_t cap, AuthoredLoadout& st);

// Drives eng.setSlotFromId once per RECORDED pair whose item is not the -1 sentinel and whose slot
// key hashes to one of the twenty engine names.  When the reader can count the document's children
// it is asked after every write, so an id the engine's registry cannot name is counted rather than
// assumed applied.  False = could not ask (no document, no writer, no hasher).
bool FillAuthoredLoadoutSlots(const entity_json::CJsonReader& doc, const LoadoutEngine& eng,
                              const uint64_t* slotKeys, const uint64_t* itemIds, uint16_t n,
                              AuthoredLoadout& st);

// The four outcomes of IDENT.LOAD_AND_APPLY_ITEM, plus the one that is ours: NotCompared is a
// recorded row L5 never scored (padding, or a row past the twenty).  Mismatch is the RESIDUE --
// the engine's gate passed, nothing was latched, and the item is still not the recorded one --
// so it stays the only value the acceptance refuses on.
enum class LoadoutSlotArm : uint8_t { NotCompared = 0, Matched, Pending, AlignGated, Mismatch };
const char* LoadoutSlotArmName(LoadoutSlotArm);

// No engine entry carried the recorded pair's slot key.  Distinct from entry 0, which a
// zero-initialised index would be indistinguishable from.
constexpr uint8_t kLoadoutNoEngineSlot = 0xFFu;

// WHO drove the item apply this pass.  `Engine` is the ordered path working as designed --
// the container was published before the entrant's enable and the engine applied it off that
// enable.  `Fallback` is the same pass reading back nothing applied and driving
// IDENT.APPLY_LOADOUT_ITEMS itself, once.  A census that spelled the two the same way could
// not tell an engine that dresses entrants from one that never has.
enum class LoadoutApplyBy : uint8_t { Engine = 0, Census, Fallback };
const char* LoadoutApplyByName(LoadoutApplyBy);


enum class LoadoutStep : uint8_t { Verify = 0, Attach, Parse, Publish, Apply, Align, kCount };
enum class LoadoutOutcome : uint8_t { NotReached = 0, Issued, SkippedAlreadyDone, Refused };

// What every step answered, so no step can be skipped silently: a run that reports fewer than
// six outcomes is a run that did not drive the sequence.
struct LoadoutSteps {
    LoadoutOutcome outcome[size_t(LoadoutStep::kCount)] = {};
    LoadoutRefusal why[size_t(LoadoutStep::kCount)] = {};
    uint16_t compared = 0;         // L1: pairs hashed and compared.  0 is never a pass.
    uint16_t unfilled = 0;         // L1: recorded pairs the player wore nothing in (the engine's
                                   // -1 sentinel).  Skipped, not verified and not a refusal:
                                   // an empty slot must replay empty.
    uint16_t skewSlot = kLoadoutNoSlot;  // the index into the twenty names L1 refused on
    // L4's read-back of LiveLoadout(ng,slot)->instanceCount.  Seeded with the could-not-ask
    // value, because every step before L4 leaves it untouched and a census that printed 0
    // there would report an empty table for one nobody asked (AUD-W2-P1-34 P1-38 M2).
    uint64_t instances = kLoadoutInstancesUnreadable;
    uint64_t engineHeld = 0;       // ReadAppliedPairs' own entry count
    uint16_t matched = 0;          // the 20/20, recorded-vs-engine BY SLOT KEY
    uint16_t mismatched = 0;
    uint16_t pending = 0;          // L5: the engine latched the recorded item at +0x20 instead of
                                   // applying it (the APPLIED_BIT arm).  Named, never hidden in
                                   // `mismatched`: the engine took our value, on its own schedule
    uint16_t alignGated = 0;       // L5: the slot carries an alignment requirement the entrant does
                                   // not meet, so the engine's outer gate skipped it entirely.
                                   // BOTH conjuncts: alignmentReq != 0xFFFF AND != entityAlignment
    uint16_t entityAlignment = kSlotAlignmentNone;  // what L5 read for the gate's second operand.
                                   // 0xFFFF is a legal answer -- an entrant the engine gives
                                   // no alignment -- so the flag below, not this value,
                                   // says whether the read happened at all
    bool entityAlignmentAsked = false;
    LoadoutApplyBy appliedBy = LoadoutApplyBy::Engine;
    uint32_t items = kLoadoutItemsUnknown;  // L4: items the engine's live loadout instance holds,
                                   // the count
                                   // IDENT.APPLY_LOADOUT_ITEMS iterates.  Printed beside the
                                   // tallies because "the parse produced K items" and "the apply
                                   // skipped them" are otherwise the same census line
    LoadoutPair engine[20] = {};   // the engine side, published for the census
    AppliedSlotState engineState[20] = {};  // the three fields it branched on, same index
    // Which arm each RECORDED pair scored, indexed by the recorded pair's own index, so the
    // per-slot census line names the outcome rather than re-deriving it from the counts.
    LoadoutSlotArm arm[20] = {};
    // THE FOUR PRE-ENABLE GATES, read at the end of the publish half -- the last moment before
    // the caller issues the entrant's enable, and the only one at which holding it is still free.
    LoadoutApplyGates applyGates{};
    // Which `engine`/`engineState` entry the recorded pair matched by key, or kLoadoutNoEngineSlot.
    // Published so the per-slot census line reads the SAME entry the scoring did, rather than
    // running the key search a second time and printing whatever that one finds.
    uint8_t engineAt[20] = {};
};

// THE MOST Mark() CALLS ONE DressLoadout PASS CAN ISSUE.  Published because the crash logger's
// per-tick marker budget is DERIVED from it rather than chosen: a budget below this truncates the
// very pass it exists to instrument, and the last-marker field then freezes on an L1 hash while
// the fault lands in the engine's apply.  L1: twenty slot-name hashes, two
// instance marks, one document walk, up to twenty document-value hashes.  L2..L6: nine call marks.
constexpr unsigned kLoadoutMaxMarksPerPass = 20 + 2 + 1 + 20 + 9;

const char* LoadoutStepName(LoadoutStep);
const char* LoadoutOutcomeName(LoadoutOutcome);

// L1..L6 in the engine's own order, each gated on a re-read and each refusing by name.  Returns
// LoadoutRefusal::None only when all six issued (or skipped as already done) AND the acceptance
// compared at least one pair recorded-vs-engine with no mismatch.
// WHICH HALF OF THE SEQUENCE ONE CALL DRIVES.  The engine applies a user's loadout off the
// social ENABLE the participating arm issues (DIAG-MODEL-SWAP), so the container has to be
// PUBLISHED before that enable and the apply can only run after it.  `Publish` is L1..L4;
// `Apply` is L1 again -- a read of the recording, which is the side the acceptance counts
// against -- then L5..L6.  `All` drives both halves in one call, for a caller that issues no
// enable of its own.  Apply RE-READS the live instance count instead of trusting Publish's
// read-back: the enable ran between the two calls and what the table says AFTER it is the
// whole question.
// `Source` is L1 then L3 ALONE -- parse the recording into the per-user loadout SOURCE container
// and stop.  It skips L2 (attach, which needs an admitted entrant) and L4 (publish to the live
// table), because it runs BEFORE admission on the engine's own remote reply route (ENG-NATIVE-
// ADMIT): the add-entrant handler's own tail bridge (IDENT.LOADOUT_ASSIGN) copies the SOURCE into
// the live table at admission, so this only has to leave a non-empty SOURCE for it to copy.
enum class LoadoutPhase : uint8_t { All = 0, Publish, Apply, Source };

LoadoutRefusal DressLoadout(void* ng, UserSlot slot, UserIndex index, const RecordedLoadout& rec,
                            const LoadoutEngine& eng, LoadoutSteps& out,
                            LoadoutPhase phase = LoadoutPhase::All);

uint64_t LiveLoadoutInstances(void* ng, UserSlot slot);

uint8_t  TeamOf(void* ng, UserIndex);
uint8_t  TeamSlotOf(void* ng, UserIndex);

// The user table's own bound, published because a scan over the roster needs one and nothing in
// here had one: LOCAL.NETGAME_LOCAL_USER_COUNT_OFF counts the LOCAL users, which on a host is the
// human plus the bots it authors and never the entrants.  IDENT.ENTRANT_COUNT_OFF is the count
// SlotOfIndex and InReplayBand already compare a UserIndex against, so a UserIndex at or past it
// is not an entrant, and this publishes that same number rather than a second opinion.
//
// False -- never a zero passed off as an empty roster -- when `ng` is null, when the count itself
// is not in committed memory, or when the user array the count claims does not fit in committed
// memory either.  A count that overruns its own table is not a roster, and a scan trusting it
// walks off the end of CR15NetGame.
bool     UserTableBound(void* ng, uint16_t& out);
uint16_t TeamAlignment(void* ng, uint8_t teamId);

// ---------------------------------------------------------------------------------------
// The three readers W2-A's roster asked for, so the capture stops
// guessing where the engine can be asked.
//
// Each takes an optional `why` because IIdentity is frozen on bool and these are free
// functions with no instance to hang LastRefusal() off.  A bare false is the silence this
// packet exists to remove: every one of the paths below has at least two ways to fail and
// the caller has to be able to tell "there is no session" from "the roster is corrupt"
// from "the engine holds nothing here".  Passing nullptr is allowed and costs nothing.
// ---------------------------------------------------------------------------------------

// The engine's OWN entrant count, so a scan does not have to walk a horizon of our choosing.
// This is IDENT.ENTRANT_COUNT_OFF -- the same number SlotOfIndex, InReplayBand and
// UserTableBound already compare a UserIndex against, published rather than re-derived.
//
// `nonReplay` is IDENT.NONREPLAY_COUNT_OFF: [nonReplay, count) is the band UserDisplayName
// renames "Replayer %u".  A roster scan needs both, because an entrant inside the band is an
// entrant whose recorded name the client will not print -- which is a capture the operator has
// to be told about, not one to drop.
//
// False -- never a zero passed off as an empty roster -- when `ng` is null (NoNetGame), or when
// the count or the user array it claims is not in committed memory (RosterUnreadable).
bool EntrantCount(void* ng, uint32_t& out, Refusal* why = nullptr);
bool EntrantCount(void* ng, uint32_t& out, uint32_t& nonReplay, Refusal* why = nullptr);

// The two loadout numbers -- the number on the jersey and the number on the body.
//
// ENG-W2A-ROSTER S1.3 reported these as unreadable ("no table this build can read holds
// them", written as 0).  That reading is wrong and this closes it: they are the u16 pair at
// +0x38 / +0x3A of the LIVE loadout header, already registered as IDENT.LOADOUT_NUMBER_OFF
// and IDENT.LOADOUT_NUMBER_BODY_OFF and already declared in LoadoutHeader above.  Their
// anchors are the engine's own reads of that pair, `movzx r8d, word ptr [rsi+38h]` at
// 0x1401551AA and `movzx r8d, word ptr [rsi+3Ah]` at 0x1401551D1.  Nothing new is derived
// here; the header this reads is the one FindInstance already walks.
//
// The whole 64-byte header is span-checked before either field is touched: the table is
// CR15NetGame + 0x51420 + (slot << 6), which for slot 15 is 62 KiB past the pointer, and a
// netGame that has not built it yet is a reserved page, not a zeroed one.
bool LoadoutNumbers(void* ng, UserSlot, uint16_t& number, uint16_t& numberBody,
                    Refusal* why = nullptr);

// ---------------------------------------------------------------------------------------------
// THE BOUNDED, GATED LOADOUT READ.  L3 #19 / ENG-W2-P1-16 dump 53400.
//
// The watched thread's innermost frame in the RUN-19 dump is OUR read of the live loadout header,
// under the pawn census -- not the engine's refresh -- and the process then took 0xC0000005.  Two
// shapes in this file could produce exactly that, and both are now impossible by construction
// rather than by care:
//
//   1. `instanceCount` is a u64 read out of engine memory and it was the LOOP BOUND (FindInstance,
//      HookLoadoutCount).  A netGame whose loadout table has not been built yet is a reserved page,
//      not a zeroed one, so the count can be anything -- and a walk of 2^64 sixty-four-byte
//      instances is indistinguishable from a hang before it is indistinguishable from a fault.
//      Every count in this file is now clamped to kLoadoutFormatMax BEFORE it sizes anything.
//   2. The read ran on an entrant whose profile document is the json db's CACHED one -- the L3 #19
//      F2 state, 58 refused key writes.  Such an entrant is half-admitted: its loadout container
//      belongs to whatever the cache last described.  The document gate is now the FIRST thing
//      CensusLoadout does, before a single loadout byte is touched.
//
// kLoadoutFormatMax is the recording format's own declared loadout capacity -- the `Symbol64
// slots[20]` that IIdentity::ApplyLoadout and ReadAppliedItems take, which is the only loadout
// magnitude this codebase declares.  ApplyLoadout's item loop already clamped to it as a bare
// literal 20; that literal is now this constant, so the bound is written once.  A count ABOVE it
// is REPORTED (`clamped`), never silently accepted as a fact and never silently truncated.
constexpr uint32_t kLoadoutFormatMax = 20;

// What one pass over one entrant's loadouts answered.  `steps` is the iteration counter: it is the
// only field that can distinguish a bounded walk from a runaway one, which is what makes the bound
// itself testable -- a dropped clamp shows up here as a number, not as a crashed process.
struct LoadoutCensus {
    uint16_t number           = 0;   // IDENT.LOADOUT_NUMBER_OFF
    uint16_t numberBody       = 0;   // IDENT.LOADOUT_NUMBER_BODY_OFF
    uint64_t claimedInstances = 0;   // what the engine's header said, verbatim and unclamped
    uint32_t instances        = 0;   // instances actually walked (<= kLoadoutFormatMax)
    uint32_t items            = 0;   // items summed over those, each clamped the same way
    uint32_t steps            = 0;   // iterations performed.  MUST be <= kLoadoutFormatMax.
    bool     clamped          = false; // the header claimed more than the format can describe
};

// Reads one entrant's loadout numbers and instance shape.  Never waits, never walks a count it
// read out of engine memory, never touches a loadout byte before the document gate passes.
//
// The entrant INDEX and the user SLOT are two facts and are taken as two arguments: the profile
// document hangs off the index and the loadout table is indexed by the slot, and deriving either
// from the other here would be this file's own guess at a mapping SlotOfIndex owns.
//
// False -- and the reason, by name -- for: NoNetGame; SlotOutOfRange; ProfileNotAttached (the
// document is absent or still the cache's); LoadoutsUnreadable (the header, or the instance vector
// its count claims, is not in committed memory).  An entrant with a readable header and ZERO
// instances is TRUE with instances == 0: an empty loadout is an answer, and "could not ask" has
// its own word above.
bool CensusLoadout(void* ng, UserIndex entrant, UserSlot slot, LoadoutCensus& out,
                   Refusal* why = nullptr);

// The byte offset of one slot's live loadout header from the netGame base -- published for the
// same reason ProfileDocFieldOffset is: a test that has to plant a header must not write the
// table geometry down a second time, or it pins its own arithmetic instead of this file's.
size_t LoadoutHeaderOffset(UserSlot);

// The entrant's ServerProfile document, AS BYTES -- ENG-W2A-ROSTER S1.2, "the largest
// remaining gap against profiles, skins".
//
// `written` is the FULL length of the document, whether or not it fitted; `truncated` says
// whether `cap` was short.  A caller that gets truncated=true has the length it needs to
// size a second call, and never a half document it cannot tell from a whole one.  The brief's
// flag is why this takes six parameters where S1.2 wrote five: a JSON blob cut at a byte
// boundary still parses as nothing and would reach the demo file looking like a capture.
//
// ★ IT NO LONGER REFUSES NoJsonWriter.  ENG-W1E-READERS established that the profile at
// CR15NetGame + IDENT.PROFILE_JSON_OFF + 592*index is a CJson NODE TREE and that this image
// ships `cjsoninspectorread` (0x1416D6EC8, 0x141C3A860) with no write-side counterpart -- and
// then read that as "no capability".  It is not: a missing SERIALISER is not a missing
// READER, and the document is fully readable one node at a time.  The bytes are produced by
// src/engine/entity_json.cpp, walking the tree with the ENGINE'S OWN accessors -- kind, key,
// value, sibling and count, eight registered entry points -- and never by guessing at node
// layout.  That is not a writer carrying its own copy of a format: the FORMAT is RFC 8259 and
// the DOCUMENT is read from the engine, so the two things DESIGN 4.6 forbids conflating stay
// apart.
//
// What this function still refuses, it refuses by name.  NoNetGame / RosterUnreadable /
// NoSuchEntrant / EmptyProfile are unchanged; ShortBuffer is the cap; and the Json* values
// above are the shapes this build's READ API cannot express -- a populated array, a
// non-integer number, a tag IDENT.JSON_NODE_KIND does not name.  Each names the key path it
// stopped at (entity_json::LastRefusalPath()), because "this player had no profile" and "this
// player's profile held one array we could not walk" are different facts.
// RECORDER-3 F1/F4.  `lossyNumber`, when asked for, comes back true if the walk wrote `null` in
// place of a non-integer number: the document is otherwise whole and the row is flagged, which is
// the owner's ruling reversing "one float refuses everything".
bool ProfileJsonBytesFromEntrantRecord(void* ng, UserSlot, void* out, size_t cap, size_t& written,
                                       bool& truncated, Refusal* why = nullptr,
                                       bool* lossyNumber = nullptr);

// The entrant's cross-platform id64 out of the same lobby record, without going through the
// profile document at all (RECORDER-3 F3). False means the record did not resolve; a resolved
// record whose head has never been written reads 0, which the caller treats as "no id".
bool LobbyEntrantAccountId(void* ng, UserSlot, uint64_t& out);

bool ProfileJsonBytes(void* ng, UserIndex, void* out, size_t cap, size_t& written,
                      bool& truncated, Refusal* why = nullptr, bool* lossyNumber = nullptr);

// ONE SUBTREE of the same document, addressed by a '.'-separated key path -- the reader
// ENG-W2A-ROSTERWIRE S2.2 asked for so that `unlocksJson` stops being refused by name.
//
// The unlocks block is NOT a second tree: CR15NetGame's own unlocks walk (0x1401552D0) calls
// FindChild(doc, "unlocks", 0) on the SAME profile document handle it was given, so its bytes
// are already inside what ProfileJsonBytes returns.  What was missing was only the framing, and
//
//     unlocksJson = ProfileJsonSubtree(ng, entrant, "unlocks", out, cap, written, truncated, why)
//
// is that framing.  Everything else is the whole-document contract UNCHANGED: the same canonical
// encoding, the same bounds, the same refusals -- ProfileJsonBytes is literally this call with a
// null path, so the two cannot drift apart -- plus JsonNoSuchKey when the path names no node.
// `written` is the length of the SUBTREE on every path and the short arm writes nothing, so a
// caller asking with cap == kPlayerJsonCap gets the whole block or none of it, never a prefix.
//
// A null or empty path is the document root.  The path is resolved level by level with
// IDENT.JSON_NEXT_ELEM / IDENT.JSON_NODE_NAME rather than by handing a dotted string to
// IDENT.JSON_FIND_NODE, whose separator and escaping this image does not state -- see
// entity_json::FindByKeyPath.  A key containing '.' is therefore not addressable, and is
// JsonNoSuchKey rather than some other node.
bool ProfileJsonSubtree(void* ng, UserIndex, const char* path, void* out, size_t cap,
                        size_t& written, bool& truncated, Refusal* why = nullptr,
                        bool* lossyNumber = nullptr);

// The cap/truncate half of the contract above, split out because it is the half that CAN be
// proved with no game present.  `written` is the length of the DOCUMENT on every path -- a
// caller refused with ShortBuffer sizes its second call from it -- and the short arm copies
// NOTHING: a JSON block cut at a byte boundary parses as nothing and would reach the demo file
// looking like a capture.  This is the function ProfileJsonBytes finishes through once a CJson
// serialiser is bound, and it is where the unlocks block goes too.
const char* LastProfileJsonRefusalPath();
const void* LastProfileJsonRefusalAt();
size_t      LastProfileJsonRefusalBytes();

bool CopyJsonBytes(const void* src, size_t len, void* out, size_t cap, size_t& written,
                   bool& truncated, Refusal* why = nullptr);

// The SR15NetUserId ApplyLoadoutToActor takes: slot in the low word, generation in the high.
// Built from the engine's own two tables so a stale generation refuses instead of dressing
// whoever now occupies the slot.
bool NetUserId(void* ng, UserSlot, uint32_t& out);

// The stock calls.  Named for what they DO, with their addresses in facts_generated.h.
bool CallApplyLoadoutToActor(void* ng, uint32_t netUserId, Symbol64 instanceName,
                             uint32_t flags, const void* actorGuid16, uint32_t skipGoalFx);
bool CallGenerateBotProfile(void* ng, UserIndex, void* entrant);
uint16_t CallCustomisationAlignment(void* componentSystem, uint16_t handle);
// The handle is the engine's full 32-bit SCompactPoolHandle, not the truncated word: both
// entry points take it in a 32-bit register and narrowing it here would silently address a
// different pool entry once the generation half is non-zero.
bool CallOverrideAlignment(void* componentSystem, uint32_t handle, uint16_t alignment);
bool CallApplyAlignmentChange(void* componentSystem, uint32_t handle);
bool CallApplyAlignmentAllUsers(void* ng);
bool CallJsonSetString(void* json, const char* key, const char* value);
bool CallJsonSetInt(void* json, const char* key, int64_t value);

// Reads a string key back out of the entrant's own profile CJson.  This reads ONE of the two
// name sources and is not the name check: see ResolvedDisplayName below for that.
bool ReadProfileString(void* ng, UserIndex, const char* key, char* out, size_t cap);
bool ReadProfileString(const IIdentity*, UserIndex, const char* key, char* out, size_t cap);

// ---------------------------------------------------------------------------------------
// The name, both halves.
//
// WRITE: SetSlotName writes the SLOT-NAME table -- the source UserDisplayName falls back to
// whenever the profile is skipped or empty, which on the shipped `-noovr` launch is always.
// It is an inline character buffer at
//     *(char**)(vec + IDENT.SLOTNAME_BASE_OFF) + IDENT.SLOTNAME_STRIDE*slot + IDENT.SLOTNAME_FIELD_OFF
// bounded by *(u64*)(vec + IDENT.SLOTNAME_COUNT_OFF); the vector is
//     *(void**)(NG + IDENT.SLOTTABLE_OFF) + IDENT.SLOTTABLE_VEC_OFF.
// IDENT.SLOTNAME_GET returns that address without dereferencing it, which is what establishes
// that the field is characters and not a pointer.  Nothing is allocated and the vector is
// never grown: a slot at or past the bound is refused with NoSlotNameTable.
//
// READ: ResolvedDisplayName calls CR15NetGame::UserDisplayName and copies out what it
// returned.  That is the OBSERVABLE -- the same call, on the same object, that the client
// prints from -- so it is indifferent to which of the three gates answered.  Reading our own
// profile write back instead is what let "every player renders Unknown" pass as "name ok".
// ---------------------------------------------------------------------------------------
// Returns Refusal::None, or which of the two ways it declined: NoSlotNameTable (the table is
// absent, or the slot is past the vector's own bound) and SlotNameTooLong.
Refusal SetSlotName(void* ng, UserSlot, const char* name);
Refusal SetSlotName(const IIdentity*, UserSlot, const char* name);

// THE PROFILE DOCUMENT AN OFFLINE ENTRANT HAS NO SENDER FOR.  On a real join the login service's
// entrant-state reply carries the JSON the engine parses into the entrant record, and everything
// downstream -- the scalar setters, the loadout apply, the rendered name -- reads that document.
// Offline nothing sends it, so the record holds the empty node and every later step answers
// EmptyProfile.  This puts a document there, out of the recording, exactly as the engine's own
// receive path does (IDENT.PROFILE_RECEIVE_VA).
Refusal AttachProfile(void* ng, UserIndex, const Blob& recordedJson);
Refusal AttachProfile(const IIdentity*, UserIndex, const Blob& recordedJson);
bool ResolvedDisplayName(void* ng, UserIndex, char* out, size_t cap);
bool ResolvedDisplayName(const IIdentity*, UserIndex, char* out, size_t cap);

// ---- releasing an entrant ---------------------------------------------------------------------
// The client removes a lobby entrant when its own EntrantRemovedCB receives SNSLobbyEntrantRemoved
// and resets the 216-byte member record at lobby + LOBBY.MEMBER_ARRAY_OFF + 216*slot.  This authors
// the body that callback reads: 16 bytes, [u64 slot][u64 pad].
//
// ★ The layout is IMPORTED from src/events/events_facts.h and not restated.  A writer carrying its
// own copy of a format is the defect that lets a checker certify broken files (DESIGN 4.6), and it
// is the same defect whether the private copy is on the read side or the write side.  The 16-byte
// length is not padding either: the client's own ListenProxy silently drops a shorter body, so a
// 8-byte body would be a release nobody performs and nobody reports.
constexpr size_t kEntrantRemovedBodyLen = 16;
bool BuildEntrantRemovedBody(UserSlot, uint8_t out[kEntrantRemovedBodyLen]);

// The engine's inline slot-name buffer, taken from the engine's OWN writes rather than from the
// distance to the next field a reader happens to know about.  CNSLobby::SetEntrantDisplayName
// fills 0x24 bytes at +0x60 (libr15.so 36db177905b0eca1ae452bb99b68613c, 0x192EC3C) and the PC
// refresh at 0x1406087F0 moves exactly 0x24 -- 16+16+4 -- between the two buffers
// (`movups xmm0,[rdx+rcx+3Ch]` @0x140608CC1 .. `mov [rdx+rcx+80h],eax` @0x140608CD9).  The old
// 0x8C-0x60 = 44 was an INFERENCE from the generation u16's offset, and it is wrong: +0x84 is a
// separate u32 the refresh zeroes (`mov dword ptr [rdx+rax+84h], 0` @0x140608CA6).  Forty-four
// bytes of name would have written through it.  A longer name is refused, never truncated.
// Contributed as IDENT.SLOTNAME_FIELD_SIZE / IDENT.SLOTNAME_MIRROR_OFF in the W2-C fact drop;
// they become facts:: constants when W0-2 merges.  Until then identity_access.cpp carries the
// static_assert that ties BOTH of them to the one number already registered
// (0x3C + 0x24 == IDENT.SLOTNAME_FIELD_OFF), so a wrong pair cannot compile -- this header cannot
// carry it itself without pulling the generated registry into every include of it.
constexpr size_t kSlotNameCap    = 0x24;
constexpr size_t kSlotNameMirror = 0x3C;

// A slot-name write is not one field.  IDENT.SLOTNAME_GET's `sel == 0` arm returns
// kSlotNameMirror, and that field is not a passive alternate view of the name: the engine's setter
// copies +0x60 into it after every write (libr15 0x192EC3C), and the refresh at 0x1406087F0 copies
// it BACK into +0x60 -- both arms, the strlen/copy at 0x140608C76-0x140608C8B and the 36-byte move
// at 0x140608CB3.  A write that leaves the mirror stale is a state the engine cannot produce, and
// the next refresh restores the stale name over ours.  SetSlotName authors both buffers.

// ---------------------------------------------------------------------------------------
// The recorded ServerProfile, walked one scalar leaf at a time.
//
// The engine's CJson has typed setters we have read -- string and integer, both taking a
// pipe-separated path, which is how GenerateBotProfile itself writes "stats|arena|Level|val".
// It has no verified parse-from-memory, so a whole recorded document cannot be handed over.
// Every scalar is therefore driven in through the setter the engine already uses.
//
// ARRAYS ARE NOT CARRIED.  There is no verified array setter on this build, and `unlocks` is
// an array.  They are counted and the first path is named, because a silently dropped
// cosmetic field is the exact failure this packet exists to remove.
//
// The sink is a parameter so the walk is a pure function: the gate drives it with a recording
// sink and no game present, and the shipped path drives it with the engine's setters.
// ---------------------------------------------------------------------------------------
struct ProfileScalarSink {
    void* ctx;
    void (*setString)(void* ctx, const char* key, const char* value);
    void (*setInt)(void* ctx, const char* key, int64_t value);
};

struct ProfileWalkStats {
    uint32_t scalars;
    uint32_t arrays;
    char     firstArray[256];
    bool     wellFormed;      // false leaves the scalars before the fault applied, and says so
};

ProfileWalkStats WalkProfileScalars(const Blob& json, const ProfileScalarSink&);

// Can this document be written AT ALL?  IDENT.JSON_SET_STRING and IDENT.JSON_SET_INT both open
// `cmp qword ptr [rcx+8], 0` and take the write arm only when that qword is zero; a non-zero one
// belongs to the netgame's json db cache, and every set on it is answered with
// `$ json path: <key>: ERROR, json db is cached, read only.` and no write.  A bot entrant's
// document is exactly that (IDENT.PROFILE_CACHE_ATTACH_VA hands GenerateBotProfile the cache's
// own document); a remote entrant is given its own by IDENT.PROFILE_RECEIVE_VA.  This is the
// engine's own predicate, read where the engine reads it, and it is the half of the read-back
// that cannot pass vacuously.
bool ProfileDocWritable(const void* doc);

// The recorded profile bytes this module is willing to hand to the engine's parser: non-empty,
// readable, NUL-free, and a balanced json OBJECT.  Published so the gate that keeps L3 #20's fault
// out of the engine is drivable with no engine present.
bool RecordedProfileJsonAcceptable(const Blob& recordedJson);

// The engine entry points the json path binds, named the way the CALLERS name them.  Published so
// a test can assert that each one still resolves to the fact it is meant to be -- the check that
// L3 #20's misordered site table would have failed and both shipped guards passed.
enum class JsonEntryPoint : uint8_t {
    DocCtor, ParseText, GetInt, NodeKind, NodeName, NodeString, NodeInt, NodeBool,
    NextElem, PathElem, ElemCount, FindNode, IsEmpty, Count,
};
uint64_t JsonEntryPointVa(JsonEntryPoint);

// Where that qword sits (IDENT.JSON_CACHED_OFF), so a caller that has to PLANT the engine's
// own flag -- a test standing a document up without a session -- takes the offset from the
// registry through this file rather than writing 8 down a second time.
size_t ProfileDocCachedOffset();

// THE READ-BACK.  Did the entrant's document actually take the recording?  Asked of the engine's
// own reader (IDENT.JSON_GET_STRING) at a key the recording carries -- preferring the loadout and
// unlocks keys the actor refresh reads -- and never of the setter's return, which is void.
bool ProfileAppliedReadBack(void* ng, UserIndex, const Blob& recordedJson);

// ATTACHING THE RECORDED PROFILE THE WAY A REAL JOIN DOES.  A remote join never writes keys into
// the entrant's document: IDENT.PROFILE_RECEIVE_VA takes the parsed document as its third
// argument and stores it whole -- `mov rcx,[rax+3C0h]` at 0x14019F608 reads the OLD pointer and
// `mov [rax+3C0h],rdi` at 0x14019F613 overwrites it.  The old document is only TESTED afterwards
// (the `if (!v61)` arm picks the first-profile path); nothing frees it, so the engine's own
// convention is that a replaced document is abandoned, not released.  Ours is abandoned the same
// way: it is allocated once, never handed to any engine free path, and must outlive the entrant.
//
// Three answers, because "the recording is not json the engine will take" and "the engine would
// not let the document be attached" are different facts and a bool would spend both.
enum class AttachResult : uint8_t { Ok = 0, ParseFailed, AttachFailed };

// The whole step: IDENT.JSON_DOC_CTOR over IDENT.JSON_DOC_SIZE bytes, IDENT.JSON_PARSE_TEXT over
// the recording's own bytes, the engine's writable gate on what came back, the store, and then
// the engine's own user-record reader (IDENT.JSON_GET_INT, the call sub_14016C360 builds the
// whole user record out of) through the field -- so what is proved is that the ENGINE can read
// the document at the pointer it now holds, not merely that a pointer landed there.
AttachResult AttachProfileDocument(void* ng, UserIndex, const Blob& recordedJson);

// The store half on its own, which is the half that is drivable without the engine: it does what
// 0x14019F613 does and then reads the field back.
AttachResult StoreProfileDocument(void* ng, uint16_t index, void* doc);

// Where the entrant's document pointer sits for that index, so a caller that has to PLANT one
// takes the offset from here rather than writing IDENT.PROFILE_JSON_OFF and USER.STRIDE down
// twice.
size_t ProfileDocFieldOffset(uint16_t index);

// ---------------------------------------------------------------------------------------------
// THE ENGINE'S OWN REMOTE-ADMISSION ROUTE -- the four bindings ENG-W2-P1-19 S5 named, bound here.
//
// P1-19 established the shape in bytes: the netgame's NSLobbyEntrantAdded handler (0x14017B850) is
// a broadcaster LISTENER, its remote arm is the DEFAULT arm, and the message that drives it is
// eight bytes -- one qword, the lobby slot.  Every other attribute the handler reads out of the
// 216-byte LOBBY MEMBER RECORD for that slot.  What was missing was where the publish may be
// ISSUED from, and this is that half.
//
// ★ THE PHASE, AND WHY IT IS A LISTENER AND NOT A FLAG WE SET OURSELVES.  L3 #21 hung the client
// by calling into the engine from inside the frame loop, and P1-19's `inReceivePhase` predicate
// was left unanswerable for exactly that reason: `replay::tick::Dispatch` is ALWAYS on our stack,
// so a latch over it can only ever answer false.  The engine's own publisher (0x1406087F0) is
// itself a message handler -- so the phase is not a time, it is a CALL SITE, and the plugin
// already owns one: netvar.cpp holds an IWiretap listener that the engine's broadcaster invokes
// from its own receive pump.  ReplyRouteArm registers a listener by the same mechanism and the
// route's phase predicate is true ONLY while that callback is on the stack.  The tick never
// publishes: it QUEUES, is answered ReplyWrongPhase (a DEFERRAL, not a failure), and the pump
// callback drains the queue at the one moment the engine is in the phase its own publisher runs
// in.  `pumpCallbacks` below is the number that says whether that moment ever arrived -- a run
// that prints zero has not refused anything, it has never been asked, and those are two facts.
//
// The message id is NOT hashed by this file and NOT read from an unfiled global: IWiretap::Symbol
// computes combine(EVENT.MATSYM_NS, hash(name+1)) through the engine's OWN two entry points, and
// EVENT.MATSYM_NS is 0x6D451003FB4B172E -- the same seed the netgame's handler registrar
// (NETGAME.MSG_HANDLER_REGISTRAR_VA) passes for "NSLobbyEntrantAdded" before caching the result.
// Reaching the registered handler is therefore structural rather than a value we agreed with.
// ---------------------------------------------------------------------------------------------

// IWiretap is forward-declared at replay scope above, beside IIdentity.

// Reason the direct drain refused to admit on a given frame -- the census's replacement for the old
// router-null/flags-unreadable bail sites, now that there is no router.  None once a call was issued.
enum class ReplyBail : uint8_t {
    None,
    ReceiverNull,        // g_reply.ng was null (route not armed on a netgame)
    ReceiverUnreadable,  // the receiver context did not pass the Readable probe
    HandlerUnbound,      // sub_14017B850 did not bind by byte-anchor (rebase / patched image)
    Reentrant,           // the shared EngineCallLatch was already held -- refused, not nested
    MsgUnreadable,       // the fabricated 40-byte argument did not pass the Readable probe
    Faulted,             // the guarded handler call raised -- contained by SEH, admitted nothing
};

struct ReplyRouteCensus {
    bool     armed         = false;  // ReplyRouteArm ran and the listener registration was tried
    bool     promoted      = false;  // every engine address the publish needs bound
    uint16_t listeners     = 0;      // registrations the engine accepted
    uint32_t pumpCallbacks = 0;      // times the engine invoked our listener from its own pump
    // Times the dedicated NETGAME.UPDATE drain driver actually entered ReplyDrainFromEngineHeartbeat,
    // counted UNCONDITIONALLY before the promoted/queued gate.  It separates "the call site never
    // fired in-match" (driverCalls==0) from "fired but the gate no-op'd it" (driverCalls>0, drained==0)
    // -- pumpCallbacks counts only the old inbound-replication listener, which a solo host never calls.
    uint32_t driverCalls   = 0;
    uint32_t queued        = 0;      // requests parked by the tick for the pump to drain
    uint32_t drained       = 0;      // queued requests the pump callback actually re-attempted
    uint32_t published     = 0;      // publishes issued -- every one of them from inside the pump
    // DIRECT-call proxy for "AddRemoteUser ran": the SHIPPED path now calls the engine's own
    // EntrantAdded handler sub_14017B850 directly (which calls AddRemoteUser), and this counts the
    // guarded calls that returned WITHOUT faulting.  Kept under its old name so the relaunch grep is
    // unchanged; read it beside lastActorCi (the actor-CI witness) to confirm the admission landed.
    uint32_t dispatchedLocal = 0;    // direct EntrantAdded-handler calls that returned without fault
    uint32_t directCalls   = 0;      // direct handler calls ATTEMPTED (entered the guarded call site)
    ReplyBail lastBail     = ReplyBail::None;  // why the last drain refused to admit (None once issued)
    uint32_t wrongPhase    = 0;      // publishes refused because the caller was not in the pump
    uint32_t seedRefusals  = 0;      // lobby-record seeds that refused
    Refusal  lastSeed      = Refusal::None;         // WHICH lobby step refused, by name
    // AUD-F2 witness: the actor component-instance handle read back SYNCHRONOUSLY right after the
    // dispatch drained -- HookActorCi resolves the template-instantiated CI within the same
    // dispatch (CS_REGISTER's precondition), distinct from DriveGate.physicsCi, the deferred
    // per-frame product.  An all-ones handle (UINT16_MAX) means no actor was left behind.
    uint16_t lastActorCi   = UINT16_MAX;
    const char* diagnostic = "";     // the wiretap's own word when the registration did not happen
};

// Register the plugin's listener on the netgame's broadcaster, so the route has a call site in the
// engine's receive phase.  Owns the tap it creates and is idempotent: a second call on the same
// netGame is a no-op and answers what the first one did.  False when nothing was registered --
// ReplyRouteStatus().diagnostic says why, and the route stays unpromoted so the direct insert
// keeps the fallback it has today.
//
// `symbolsCsv` is a comma-separated list of message struct names to listen on, leading 'S'
// included, or null for the built-in set.  It exists because WHICH messages an offline solo host
// publishes on that node is a question only a run can answer, and answering it must not need a
// rebuild.
// The tap is the CALLER'S -- the shipped one hands the plugin's own IWiretap, tests/identity hands
// a fake broadcaster so the pump callback can be delivered with no game running.  It is never owned
// or destroyed here: this module has no link dependency on wiretap.cpp, only on its interface, and
// a module that creates taps it does not tick is a lifetime nobody can see.
bool ReplyRouteArmWith(void* ng, IWiretap* tap, const char* symbolsCsv = nullptr);

void             ReplyRouteDisarm();
bool             ReplyRoutePromoted();
ReplyRouteCensus ReplyRouteStatus();

// BLOCKER-1 (DIAG-REPLYPHASE Fix B; DIAG-DRAIN-NOOP Q2 pivot).  Drain the wrong-phase-deferred reply
// admission by calling the engine's own EntrantAdded handler sub_14017B850 DIRECTLY (which reaches
// AddRemoteUser) -- no broadcaster, no router.  MUST be called only from a real in-match engine-update
// stage-0 stack (the dedicated NETGAME.UPDATE detour), never the plugin's async tick: inPump is the
// in-phase witness the direct call requires.  No-op unless the route is promoted and a request is
// parked.  Returns the census so the caller can read drained/published/directCalls.
ReplyRouteCensus ReplyDrainFromEngineHeartbeat();

// Install the DEDICATED, ALWAYS-ARMED detour on CR15NetGame::Update (NETGAME.UPDATE = sub_1401BBDB0)
// that drives ReplyDrainFromEngineHeartbeat every frame in-match, filtered to the stage-0 tick.  This
// is independent of tick.mode: the tick's own NetGameUpdateDetour is uninstalled under tick.mode=slot,
// which is exactly the run in which the entrant parks, so the drain must not ride it.  Byte-anchored
// through hookmgr (refuses if the recorded prologue is not what stands at the site -- e.g. tick's own
// hook-mode fallback already owns it), gated by pawn.reply_route.  Idempotent; returns true when armed.
// Call AFTER the tick has resolved its mode (i.e. once a session is up), so a hook-mode fallback that
// took NETGAME.UPDATE first makes this refuse by prologue rather than colliding.
bool InstallReplyDrainDriver();

// The detour cannot be taken back out of the engine's call chain, so unload disarms its BODY
// instead: the hook stays, and every call passes straight through to the original.
void SuspendReplyDrainBody();

// Load's half of the pair: re-arm the body of the detour that is already in the chain. Never
// creates a hook, so a driver that never installed stays uninstalled and this returns false.
bool ResumeReplyDrainBody();

// RELOAD-6. The entrant actor class is learned in one game space; a level cycle builds another.
void ForgetEntrantActorClass();

bool ReplyDrainBodyArmedForTest();
void ReplyDrainSetBodyArmedForTest(bool armed);
void ReplyDrainInstallForTest(bool installed);

// The detour's own gating, driven without the engine's original: what runs before the original and
// what runs after it. The leave feed rides the stage-3 tail, and Unload suspends the body before it
// queues the leave, so "disarmed but contained" has to keep the tail.
void ReplyDrainGatedForTest(void* netGame, uint64_t stage);

// ---- ENG-MODEL-PUMP FIX A: the model-swap completion pump ---------------------------------------
// The result of one flush attempt on the CR15NetPlayerModelSwapperCS driver (sub_14044A0C0), driven
// from the stage-0 NETGAME.UPDATE detour top: `resolved` is whether the swapper CS was found, `pumped`
// whether the driver ran this call (only when the pending queue was non-empty), and pendingBefore/After
// the queue depth around the call.  A run that shows pendingBefore>0 then pumped=1 then the entrant's
// in-flight clearing is the swap landing.
struct SwapperPumpCensus {
    bool     resolved      = false;
    bool     pumped        = false;
    bool     faulted       = false;
    bool     reentrant     = false;
    uint64_t pendingBefore = 0;
    uint64_t pendingAfter  = 0;
};

// The per-entrant swapper state, for the drivegate instrumentation: the pending queue depth (global to
// the swapper CS) and this actor's in-flight flag + handle.  `resolved` false means the swapper CS did
// not resolve; `handleRead` false means the actor carries no swapper record (handle absent).
struct SwapperEntrantState {
    bool     resolved   = false;
    bool     handleRead = false;
    uint16_t handle     = 0xFFFFu;  // facts-ok: LOCAL.NO_COMPONENT the component-absent sentinel
    uint32_t inFlight    = 0;
    uint64_t pending     = 0;
    // ENG-SWAPPER-RESET (A): the per-record CURRENT-MODEL-HASH the dispatcher compares against the
    // target.  `currentRead` false means the model-instance walk (rec+408 -> +0xB10) did not resolve;
    // a pooled actor reads its prior occupant's hash here (present) while no mesh is built.
    bool     currentRead  = false;
    uint64_t currentModel = 0;
};
bool ReadSwapperEntrant(void* ng, uint16_t index, SwapperEntrantState& out);

// Force OUR entrant's swapper CURRENT-MODEL-HASH to the -1 no-model sentinel so the
// engine's own Apply-Chassis swap sees current(-1) != target and takes the enqueue (!=) arm, which the
// installed FIX-A pump then drives to a built mesh.  Resolves the record for `index` ONLY (never other
// users), Readable-probes every hop of the sub_1404B5D90 walk, and SEH-guards the single 8-byte store;
// any unreadable hop or a faulting store refuses and is reported, never corrupting the record.
struct SwapperResetOutcome {
    bool     resolved = false;   // the swapper CS resolved (non-null)
    bool     located  = false;   // the current-model field address resolved (all hops readable)
    bool     wrote     = false;  // the -1 store completed without faulting
    bool     faulted   = false;  // the store raised, and was contained
    uint64_t before    = 0;      // the current-model hash before the write (the pooled 0xDAC)
    uint64_t after     = 0;      // the current-model hash after the write (the -1 sentinel when wrote)
};
SwapperResetOutcome ResetSwapperCurrent(void* ng, uint16_t index);

// The pre-apply reset gate: fire the -1 reset ONLY in the window before the engine's admission Apply
// Chassis reads current, and never after -- so the swap re-arms exactly once and a built mesh never
// churns.  Fire iff: the mesh is not yet built (meshPresent false), the swapper record resolves, no
// swap is already enqueued (pending == 0), and current is a real stale hash (read, and != -1).  A
// mesh present, an enqueue in flight, or an already-cleared current each STOP it.
bool SwapperResetShouldFire(bool meshPresent, const SwapperEntrantState& sw);

// The direct BeginModelSwap re-trigger.  When the pooled entrant's natural Apply
// Chassis has already no-op'd (from==to) and the sweep has forced current to -1 and it PERSISTED
// (nothing re-wrote it, so the engine is done touching current for this apply), the sweep calls the
// engine's OWN enqueue fn sub_14042D120 directly with the recorded target hash.  Because current is
// -1, BeginModelSwap's own `if (*current == target)` re-check is false and it takes the enqueue arm:
// rec+8 = 1, ++pending, schedule the FIX-A pump -> the mesh builds.  Fully guarded: the fn is bound
// through Guarded<> (byte-anchored prologue), the CS + full record walk are Readable-probed, the call
// is SEH-contained and takes the shared EngineCallLatch against re-entry.  Fired for OUR entrant index
// only, and at most once per entrant.
struct BeginSwapOutcome {
    bool     resolved      = false;  // the swapper CS resolved (non-null)
    bool     located       = false;  // the record's current-model walk resolved (all hops readable)
    bool     issued        = false;  // BeginModelSwap ran without faulting
    bool     faulted       = false;  // the call raised, and was contained
    bool     reentrant     = false;  // the shared EngineCallLatch was already held -- refused
    uint64_t pendingBefore = 0;      // the swapper pending-queue depth before the call
    uint64_t pendingAfter  = 0;      // and after: a 0 -> 1 step is the enqueue arm having run
};
BeginSwapOutcome TriggerBeginModelSwap(void* ng, uint16_t index, uint64_t targetHash);

// The re-trigger gate: fire the direct BeginModelSwap exactly once per entrant, only in the window
// after the sweep has forced current to -1 and it persisted.  Fire iff: not already issued, the mesh
// is not built, the swapper record resolves, no swap is already enqueued (pending == 0), current has
// been read and reads -1 (the sweep's write persisted), and a target hash has been captured.  An
// already-built mesh, an enqueue in flight, a current not yet -1, or a missing target each STOP it --
// so a body never churns and the swap is never double-enqueued.
bool BeginSwapShouldFire(bool meshPresent, const SwapperEntrantState& sw, bool targetKnown,
                         bool alreadyIssued);

// Test seam for ENG-BEGINSWAP: inject a stand-in BeginModelSwap (so the re-trigger drives with no
// image mapped).  Set AFTER SwapperInjectForTest (which sets the injected CS active).
void SwapperInjectBeginForTest(void (*begin)(void* cs, uint32_t index, uint64_t target));

// Emulate the engine's OWN "[NETGAME] Profile received" handler
// (IDENT.PROFILE_RECEIVE_VA / sub_14019F510) for a recorded remote entrant, so the ENGINE itself
// requests the model resource and seats the InstanceModel CI in response to the
// onlobbymemberprofilereceived delegate the handler fires -- the same mechanism a bot's
// locally-generated profile drives, reproduced on a non-bot remote.  The handler is called with:
//   a1 = the live CR15NetGame (byte-anchored; Readable-probed here);
//   a2 = the 16-byte id [accountType=5 (OVR, the REMOTE arm)][accountId=xplatformId] -- the SAME
//        synthetic id SeedLobbyRecordFor wrote into the slot, so the handler's slot lookup finds our
//        entrant and its own eviction site (clear of the viewer's id) cannot remove it;
//   a3 = the recorded profile parsed into an engine CJson document with IDENT.JSON_DOC_CTOR +
//        IDENT.JSON_PARSE_TEXT (the reader AttachProfileDocument uses).  The recorded bytes are the
//        already-decoded document the capture layer stored, not the zstd wire form.
// The handler stores a3 whole at [entrant+0x3C0] and abandons the old, so the document is leaked
// deliberately (no engine free path for a replaced doc) and must outlive the entrant.  Fully
// guarded: the fn is bound through Guarded<> (byte-anchored prologue), the receiver + the 16-byte id
// + the document are Readable-probed, the call is SEH-contained and takes the shared EngineCallLatch
// against re-entry.  Fired for OUR entrant only, at most once.  NEVER calls the pool rebuild
// (sub_140323790) or the CI seat (sub_14030F120): the engine seats the CI itself via the delegate.
struct ProfileReceivedOutcome {
    bool     receiverBad    = false;  // ng null/unreadable, or an arg did not read back
    bool     jsonRejected   = false;  // the recorded bytes are not an acceptable json object
    bool     parserUnbound  = false;  // the CJson ctor/parse entries did not bind (no image / OOM)
    bool     parseFailed    = false;  // the engine parser rejected the recorded bytes
    bool     handlerUnbound = false;  // IDENT.PROFILE_RECEIVE_VA did not bind (rebase / patched image)
    bool     reentrant      = false;  // the shared EngineCallLatch was already held -- refused
    bool     issued         = false;  // the handler ran without faulting
    bool     faulted        = false;  // the handler raised, and was contained by SEH
    uint64_t account        = 0;      // the a2 accountId passed (for the log line)
};
ProfileReceivedOutcome EmulateProfileReceivedCall(void* ng, uint64_t xplatformId,
                                                  const Blob& recordedJson);

// The fire gate: emulate the profile-received handler exactly once per entrant, only while the body
// is not yet dressed and the recorded profile is present to hand it.  Fire iff: not already issued,
// the InstanceModel CI is not yet seated (meshPresent false), and the recorded profile is present.
bool EmulateProfileShouldFire(bool meshPresent, bool profilePresent, bool alreadyIssued);

// Test seam for ENG-DRESS-PROFILE: inject a stand-in profile-received handler and a stand-in CJson
// ctor/parse so the emulation drives (and builds a3) with no image mapped.
void ProfileReceivedInjectForTest(void (*handler)(void* ng, const void* xplatId, void* doc),
                                  void* (*ctor)(void*),
                                  uint32_t (*parse)(void*, const char*, uint64_t));
void ProfileReceivedClearInjectForTest();

// Drive the engine's OWN R15NetPlayerEquipmentUpdate RECV handler (EQUIP.RECV_VA /
// sub_140D37300) for a recorded remote entrant, carrying the RECORDED chassis item id, so the engine's own
// model-changed delegate (sub_140CEC740) installs the net-model component through CModelSwapperCS and the
// framework's phase-16 rebuild seats a visible body -- the piece the offline remote route was missing
// (DIAG-MODEL-COMPONENT-LEVER: VIABLE, mark-dirty/deferred at every hop; NEVER sub_140323790/sub_14030F120).
// The handler is called handler(a1, a2) with:
//   a1 = the R14NetPlayerEquipment component system, resolved off the CncaGameSpace by EQUIP.CS_SYM (the
//        same FindComponentSystem lookup ResolveSwapperCS uses, with the equipment class symbol);
//   a2 = {0, 0, &body}, where body is the 56-byte payload: +0 = the entrant's SR15NetUserId (built by
//        NetUserId off the engine's live SLOT/GEN tables, so the handler's own id validity checks pass),
//        +8 = chassisHash (the recorded chassis item id), +16/+24/+32 = 0 (the equip fields left as the
//        record's default so only the chassis change fires; all four equip-changed delegates are proven
//        deferred-safe regardless).
// Fully guarded: the CS is Readable-probed and confirmed to back-reference OUR ng, the entity id is refused
// unless the engine's own tables validate it, the handler is bound through Guarded<> (byte-anchored
// prologue), the call is SEH-contained and takes the shared EngineCallLatch against re-entry.  Fired for
// OUR entrant index only, at most once (a re-fire would be a harmless no-op: the handler only fires the
// delegate when the chassis actually changes).
struct EquipmentRecvOutcome {
    bool     spaceResolved      = false;  // *(ng + NETGAME_CS_SPACE_OFF) read
    bool     equipResolved      = false;  // the equipment CS resolved by symbol (non-null)
    bool     netgameContradicted = false; // a1's back-ref was readable AND != ng -> refuse (wrong object)
    bool     netgameConfirmed   = false;  // a1's back-ref read back == ng (positive confirmation)
    bool     idResolved         = false;  // the engine validated the entrant's SR15NetUserId (slot/gen ok)
    bool     handlerBound       = false;  // EQUIP.RECV_VA bound (prologue matched)
    bool     reentrant          = false;  // the shared EngineCallLatch was already held -- refused
    bool     issued             = false;  // the handler ran without faulting
    bool     faulted            = false;  // the handler raised, and was contained by SEH
    bool     chassisRejected    = false;  // F1: chassis was the empty-slot sentinel -> never driven
    uint16_t index              = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    uint32_t entityId           = 0;      // the +0 SR15NetUserId passed
    uint64_t chassis            = 0;      // the +8 chassis item id passed
};
EquipmentRecvOutcome DriveEquipmentRecv(void* ng, uint16_t index, uint64_t chassisHash);

// The fire gate: drive the equipment RECV exactly once per entrant, only while the body is not yet dressed
// and the actor's swapper record exists (the actor is allocated+enabled).  Fire iff: not already issued,
// the InstanceModel CI is not yet seated (meshPresent false), a chassis was extracted (chassisKnown), and
// the entrant's swapper record resolved (recordReady) -- so the equipment record the handler writes exists.
bool EquipmentRecvShouldFire(bool meshPresent, bool chassisKnown, bool recordReady, bool alreadyIssued);

// IDENT.SYMBOL_FROM_NAME_VA(name) with the loadout hash seed, i.e. the engine's own key for a loadout slot
// name.  Reproduces the recorded pair's slotKey (CustSlotEntry.key), so a caller can match the recorded
// chassis by key.  0 when the hasher did not bind (no image) or the name is null/empty.
uint64_t LoadoutSlotSymbol(const char* slotName);

// Test seam for ENG-EQUIPMENT-RECV: inject a stand-in equipment CS + a stand-in RECV handler (so the drive
// runs with no image mapped).  A null handler turns it off; ClearEquipmentRecvInjectForTest resets both.
void EquipmentRecvInjectForTest(void* equipCs, void (*handler)(void* cs, void* msg));
// Stand-in for LoadoutSlotSymbol's engine hash, so the recorded-chassis lookup runs with no image
// mapped.  Cleared by EquipmentRecvClearInjectForTest.
void EquipmentSlotSymbolInjectForTest(uint64_t (*slotSymbol)(const char* slotName));
void EquipmentRecvClearInjectForTest();

// The stage-0 NETGAME.UPDATE detour drives a per-tick sweep so the entrant's swapper current is
// forced to -1 in the window BEFORE the engine's admission Apply Chassis reads it (the one-shot at
// the loadout-drive site fires ~2s too late -- after the engine already took its from==to no-op).
// The PawnProvisioner registers the sweep: it alone knows OUR entrant indices (viewer excluded) and
// the mesh-presence reader.  identity_access only invokes what is registered, guarded per engine touch.
using SwapperResetSweepFn = void (*)(void* ctx, void* ng);
void RegisterSwapperResetSweep(SwapperResetSweepFn fn, void* ctx);
void ClearSwapperResetSweep();

// The NETGAME.UPDATE detour's stage-3 tail: a registered sweep runs after the original returns on the
// frame's last stage, which is as late in the frame as this plugin runs. The playback layer registers
// its HUD-netvar writer here; nothing runs until something is registered.
using GameStateStageSweepFn = void (*)(void* ctx, void* ng);
void RegisterGameStateStageSweep(GameStateStageSweepFn fn, void* ctx);
void ClearGameStateStageSweep();
// Drives the stage-3 tail exactly as the live detour does, for a test that owns a fake netGame.
void GameStateStageSweepForTest(void* netGame, uint64_t stage);

// Test seams for FIX A: inject a stand-in swapper CS + a stand-in 1-arg pump (so the flush drives with
// no image mapped), and run the flush body returning its census.  A stand-in pump that clears the
// record's in-flight flag proves the gate + call by construction (FAIL-before/PASS-after).
void SwapperInjectForTest(void* cs, void (*pump)(void*));

// ENG-BOT-BODY test seam.  -1 defers to config (the shipped default routes recorded players through
// the bot body-build); 0 forces the legacy AddRemoteUser reply route so its tests still exercise it;
// 1 forces the bot path.  Global like the other inject seams; a test resets to -1 when done.
void BotBodyForceForTest(int state);
void SwapperClearInjectForTest();
SwapperPumpCensus SwapperFlushForTest(void* ng);

// Test seam: invoke the dedicated detour's BODY for a given netgame and engine stage, then return the
// census.  It takes no tick-mode input, which is the point -- the drain runs off the stage filter and
// the netgame's own in-match state word, proving the always-armed path does not depend on whether
// tick.mode installed the fallback detour.
ReplyRouteCensus ReplyDrainNetGameUpdateForTest(void* netGame, uint64_t stage);

// Test seam for the BLOCKER-2 authority-flip guard: runs the real flip/restore (InvokeSpawnNotify)
// over a caller-supplied mode-word cell + notify, so a self-test can prove the word is byte-identical
// after flip+restore on the normal AND the faulting path.  Returns false if notify faulted.
bool StageBAuthFlipProbeForTest(void (*notify)(void* ng, uint16_t idx), void* ng, uint16_t idx,
                                uint64_t* modeWordCell, bool flip);

// The built-in listen set, published so a test names the same strings the shipped path does.
const char* ReplyRouteDefaultSymbols();

// The TEST-inject publish shape (five arguments).  The SHIPPED route calls the engine's own
// sub_140F80860 (six arguments: router, msgId, ctx, peerId=sentinel, payload, size) -- an internal
// binding that reads the two .data globals live; ReplyRouteInjectForTest supplies THIS fake instead
// so the queue-and-drain half can be exercised with no image mapped and no engine bit to read.
using ReplyPublishFn = uint64_t (*)(void* broadcaster, uint64_t msgId, const char* name,
                                    const void* payload, uint64_t bytes);

// DIAG-DRAIN-NOOP Q2 pivot: the SHIPPED route no longer publishes through the broadcaster (offline
// its router is null); it calls the engine's own EntrantAdded handler sub_14017B850 DIRECTLY as
// handler(a1_receiver, &msg).  a1 is the CR15NetGame the handler was registered against; a2 is the
// 40-byte wrapper the engine's own size-trampoline sub_14013CD50 builds, whose +16 points at the
// 8-byte payload.  This is the type the direct call is issued through, and the stand-in a test injects.
using EntrantAddedHandlerFn = void (*)(void* a1, void* a2);
// ReplyBail (the direct-drain refusal reason read into the census) is declared above, before
// ReplyRouteCensus, because the census stores it.

// Hand ReplyRouteArmWith the three values it would otherwise read out of a mapped echovr.exe, so
// the queue-and-drain half can be driven with no image loaded: every one of them binds through
// buildguard::MappedAddress, which cannot resolve when nothing is mapped, and the route then
// returns unpromoted before it registers anything.
//
// Not #ifdef-guarded -- config.h's LoadFromTextForTest is this tree's precedent, and a seam behind
// a define would mean the binary under test is not the shipped one.  ReplyRouteDisarm does NOT
// clear it, because disarm resets the pump wholesale; a null `publish` turns it off.
void ReplyRouteInjectForTest(void* node, ReplyPublishFn publish, uint64_t msgId);

// Inject a STAND-IN EntrantAdded handler for the SHIPPED direct path, so the pivot can be driven with
// no engine mapped: the drain reaches HookReplyDispatch's shipped branch and calls this fn instead of
// the byte-anchored real sub_14017B850, with g_reply.ng as the receiver.  `node` may point at a null
// router -- the direct path never reads it, which is the whole point of the pivot -- so a test that
// binds a null-router node and still admits proves the offline router dependency is gone.  A null `fn`
// turns it off.  Like ReplyRouteInjectForTest, not #ifdef-guarded and not cleared by ReplyRouteDisarm.
void ReplyDirectInjectForTest(void* node, EntrantAddedHandlerFn fn, uint64_t msgId);

// A stand-in for the shipped NSLobbyRemovingEntrant handler, so the leave feed can be
// driven byte-for-byte with no echovr image mapped.  Injected over whichever route is armed.
void ReplyLeaveInjectForTest(EntrantAddedHandlerFn fn);

// The session admission ledger: one bit per lobby slot this process's ProvisionReplayUser seated an
// entrant into and has not yet seen leave.  ReleaseReplayUser will not name a slot outside it, which
// is what keeps the leave off the viewer, so a test that can read it can prove the target set.
uint32_t AdmittedSlotMask();
// Why a slot-name walk gave up, named in the order the hops are taken.
// The walk is on the live 1 Hz capture path, so every hop is proved and a failure is a counted,
// named refusal -- never a read through a pointer the engine has already freed.
enum class SlotNameRefusal : uint8_t {
    None,
    NoNetGame,
    OwnerSlotUnreadable,
    OwnerNull,
    VectorUnreadable,
    PastCount,
    BaseNull,
    EntryUnreadable,
    Count,
};
const char* SlotNameRefusalName(SlotNameRefusal r);
struct SlotNameWalkCensus {
    uint32_t refusals[size_t(SlotNameRefusal::Count)] = {};
    uint32_t logged = 0;   // one bit per reason, so each is said once per session
};
const SlotNameWalkCensus& SlotNameWalk();
void ResetSlotNameWalkForTest();
// Drives the guarded walk itself, so a fake engine whose second or third hop is unmapped can be
// shown to refuse rather than fault.
const char* SlotNameFieldForTest(void* ng, uint16_t slot);

void ForgetAdmittedSlotsForTest();
void SeedAdmittedSlotForTest(uint16_t slot);

// The pawn records of entrants who have left, restored to the engine's own "no actor"
// value.  The allocator (sub_1404E61E0) pre-sets exactly these 16 bytes before it decides anything,
// and a record that has never held a pawn reads them back; a record an entrant left behind keeps the
// DEAD actor's guid, and the next admission hands that guid to the engine's own component resolve,
// which cannot resolve it and dereferences null.  Only records at or above the engine's own user
// count are touched -- a live entrant is always below it.  Returns how many were restored.
// Exposed so a fake net game can drive it with no image mapped.
uint32_t ClearVacantPawnActorGuids(void* netGame);

// Test seam for the reply seed's `spawned` write: the stand-in store, side selector and setters it
// writes through with no image mapped.  Null restores the engine's own.  Not cleared by
// ReplyRouteDisarm, like the two above.
struct HopCallTable;
void ReplySpawnedSeedInjectForTest(const HopCallTable* calls);

// Test seam: run the registered stage-0 sweep exactly as the NETGAME.UPDATE detour does, inside the
// window ActorHops::participatePair requires.
void Stage0SweepForTest(void* netGame);

// The bail reason spelled for the census line.
const char* ReplyBailName(ReplyBail bail);

// Test seam: drive the shipped DIRECT call in isolation with an explicit receiver + injected stand-in
// handler, returning the bail reason.  It exercises DirectEntrantAddedCall's guard ladder (receiver
// null/unreadable, handler unbound, re-entry, fault) and the admit path WITHOUT the seed and
// postcondition hooks the full drain runs against a real ng, and WITHOUT any broadcaster router at all
// -- which is the pivot's whole claim.  Leaves the route mutated; call ReplyRouteDisarm after.
ReplyBail ReplyDirectCallForTest(void* receiver, EntrantAddedHandlerFn fn, uint16_t slot);

// `remotePlayerCs` is hop 4's provider (see ActorHopSurface) and it has NO DEFAULT: a caller that
// has no component system to offer must say so by passing null at the call site, where a reader
// can see it.  A defaulted parameter here would be a switched-off hop that no diff shows.
IIdentity* Create(void* (*netGame)(), void* (*remotePlayerCs)());
void       Destroy(IIdentity*);

// The CR15NetGame behind an IIdentity, or null when `p` is not this module's own instance.
// Every free accessor below goes through it.  IIdentity is B.11.3's frozen interface and is
// explicitly shaped for injection, so a static_cast to the concrete type would be undefined
// behaviour for any other implementation -- a fake in a test, or another packet's.  This is
// an identity comparison against the one instance Create() hands out, so a foreign IIdentity
// gets a null and a clean `false` instead.
void* NetGameOf(const IIdentity*);

// Why the last refusal is queryable rather than returned: IIdentity is frozen and returns
// bool.  A bare false is the silence this packet exists to remove, so the caller reads the
// reason here and logs it.  Set by every method, on success as well (Refusal::None).
Refusal     LastRefusal(const IIdentity*);
const char* LastDetail(const IIdentity*);

// SetProfileJson carries every SCALAR leaf of the recorded profile through the engine's own
// typed setters.  Arrays -- `unlocks` above all -- have no verified setter on this build, so
// they are COUNTED and named rather than silently dropped.  Read after each call.
uint32_t    UncarriedArrayCount(const IIdentity*);
const char* FirstUncarriedArray(const IIdentity*);
uint32_t    CarriedScalarCount(const IIdentity*);

// How many of the recorded twenty item ids the engine's own instance had room for.  Fewer
// than twenty is reported as Refusal::ShortInstance, never padded and never truncated quietly.
uint32_t    AppliedItemCount(const IIdentity*);

// WHAT THE ENGINE HOLDS, read back through the same four readers the engine's own entrant dump
// (sub_1401CD1A0) uses for its `Name` / `Loadouts` / `Loadout Number` / `Alignment` lines.  A
// dress that wrote every field and a dress that wrote none both leave the ladder saying "done",
// and LAUNCH-SHIPDEFAULTS was read as a total name failure off dumps the engine printed BEFORE
// the name was written -- so the plugin prints the engine's own answer at a moment it chose.
struct EngineIdentity {
    char     name[64];        // as UserDisplayName would return it; empty when it could not be called
    bool     nameRead;
    uint16_t number;          // live loadout header +0x38 -- the engine's `Loadout Number`
    uint16_t numberBody;
    bool     numbersRead;
    uint64_t instances;       // live loadout header +0x30 -- the engine's `Loadouts`
    bool     instancesRead;
    uint8_t  teamId;
    uint8_t  teamSlot;
    bool     teamRead;
};
bool ReadEngineIdentity(const IIdentity*, UserIndex, EngineIdentity& out);

// ------------------------------------------------------------------------------------------
// THE LIVE SCOREBOARD -- the fifteen per-entrant values the engine publishes beside the roster.
//
// Twelve are the /session writer's own `stats|` keys (sub_140155C80); ping, packetlossratio and
// is_emote_playing ride the identity block sub_1401B2EA0 writes. Every offset below is STATS.* in
// the registry, decoded from the imm32 of the engine's OWN `lea` for that field.
//
// Each stat is a three-word counter -- kind, value, count -- and the engine's reader divides value
// by count when the kind word holds the ratio kind. A ratio whose count is ZERO makes the engine
// emit a bare 0. That zero is not a score, so this reader leaves the field alone and clears its
// fill bit: denying a fabricated zero is the whole reason the mask exists.
//
// A field whose bit is CLEAR is left EXACTLY as the caller passed it. Nothing here writes an
// absence value -- the on-disk sentinels (kPlayerStatNone and friends) belong to the demo format
// and there is one copy of them, on the capture side.
enum PlayerStatBit : uint16_t {
    kPsFillPossessionTime = 1u << 0,
    kPsFillPoints         = 1u << 1,
    kPsFillGoals          = 1u << 2,
    kPsFillStuns          = 1u << 3,
    kPsFillBlocks         = 1u << 4,
    kPsFillAssists        = 1u << 5,
    kPsFillSaves          = 1u << 6,
    kPsFillInterceptions  = 1u << 7,
    kPsFillSteals         = 1u << 8,
    kPsFillCatches        = 1u << 9,
    kPsFillPasses         = 1u << 10,
    kPsFillShotsTaken     = 1u << 11,
    kPsFillPing           = 1u << 12,
    kPsFillPacketLoss     = 1u << 13,
    kPsFillEmote          = 1u << 14,
};

// The on-disk numbering is the same numbering: W2-A static_asserts its capture-side mirror
// against these fifteen names, and there is one copy of the numbering.
constexpr uint16_t kPlayerStatsFillAll = 0x7FFFu;

// Every bit this BUILD can source, and `fill & ~kPlayerStatsReadableMask` is always zero. Bit 14
// is the only one that costs the caller anything: is_emote_playing lives on the R15NetCustomization
// component system, not on CR15NetGame, so a call that names no EmoteSource can never set it. The
// other fourteen come from `ng` alone, which is what kPlayerStatsNetGameOnlyMask says.
constexpr uint16_t kPlayerStatsReadableMask    = 0x7FFFu;
constexpr uint16_t kPlayerStatsNetGameOnlyMask = 0x3FFFu;

// Where is_emote_playing is read from. sub_1401B2EA0 finds both by walking the entrant's actor to
// its customisation component, which is the entity layer's job, not this reader's: name them and
// the dispatch below answers, leave them out and bit 14 stays clear.
struct EmoteSource {
    void*    componentSystem = nullptr;  // the R15NetCustomization component system
    uint32_t emoteId         = 0;        // the entrant's id within it
};

struct PlayerStats {
    float   possessionTime;
    int32_t points, goals, stuns, blocks, assists, saves;
    int32_t interceptions, steals, catches, passes, shotsTaken;
    int32_t ping;             // milliseconds, the engine's own uint16 widened
    float   packetLossRatio;
    uint8_t isEmotePlaying;
};

// O(1) per entrant: two cached span proofs and the loads, no engine call and no allocation.
// `fill` is not optional -- without it every absent field would read as whatever the caller left
// there, which is the confusion this function exists to remove.
bool ReadPlayerStats(void* ng, UserIndex entrant, PlayerStats& out, uint16_t* fill,
                     Refusal* why = nullptr, const EmoteSource* emote = nullptr);

// The is_emote_playing dispatch on its own, so a caller that only wants the emote need not read a
// scoreboard for it. `out` is written only on a true return; a handle that names no target is the
// engine's own false, not a refusal.
bool ReadEmotePlaying(const EmoteSource& src, bool& out, Refusal* why = nullptr);

// The entity layer's answer to "which component system, and which emote id, for this entrant".
//
// WHY IT IS A SEAM.  sub_1401B2EA0 reaches the R15NetCustomization component system from the
// /session writer's own `this` -- the array at [this+0], bounded by [this+30h], searched at
// 0x1401B3F40-0x1401B3F84 -- and NOT from CR15NetGame, so `ng` alone can never source bit 14.  The
// route that can answer is the entity layer's (EntityBind / FindComponentSystem / the entrant's
// actor GUID), and identity_access must not name the entity module: the same one-way dependency
// camera_control::SetSubjectPose exists for.  The entity module installs this from its own Init
// and clears it in Shutdown.
//
// `Resolve` returns false when it cannot name a system for THIS entrant this tick.  That is a
// decline, and a decline is never published as "no emote is playing".
struct EmoteSourceSeam {
    bool (*Resolve)(void* ng, UserIndex entrant, EmoteSource* out);
};

void SetEmoteSource(const EmoteSourceSeam*);

// EmoteNoSeam when nothing is installed, EmoteNoSystem when the installed seam declines.  Both
// leave `out` untouched.
bool ResolveEmoteSource(void* ng, UserIndex entrant, EmoteSource& out, Refusal* why = nullptr);

// ---------------------------------------------------------------------------------------------
// THE ACTOR HOP CHAIN'S ENGINE CALLS (P1-24), AS A TABLE, SO THEIR ARGUMENT SHAPE IS TESTABLE.
//
// The three entry points differ in argument 2 by one modrm bit -- ENGINE.ACTOR_ENABLE takes a
// VALUE (48 8B DA), the predicate and the place take a POINTER (48 8B 1A) -- and the enable
// consumes it with a REX.W move, so its declaration must be the full 64-bit word pair
// { actorIndex, poolIndex } and not its low half.  None of that is guarded in bytes: a byte-string
// fact needs an anchor row that does not exist yet (P1-24 section 4).  It is guarded HERE instead.
// A test installs its own table, receives the exact arguments the chain pushed, and a declaration
// that lost the high half arrives truncated.
//
// `calls` is null in the shipped build and every call resolves through Guarded<> at its recorded
// VA.  A table with a null member REFUSES that hop rather than falling through to a real address:
// a process that is not echovr.exe has no such address to fall through to.
using ActorIsEnabledFn = uint8_t  (*)(void* rootSpace, const void* guid16);
using ActorLookupFn    = void*    (*)(void* space, uint64_t lookupKey);
// ENGINE.ACTOR_GUID_CANON.  It WRITES THROUGH its second argument, which is why every caller of
// it here hands it a copy of the entrant's guid and never the engine's own user record.
using ActorGuidCanonFn = void     (*)(void* rootSpace, void* guid16);
// NETGAME.USER_SPAWNED_GET -- the engine's own `user[N]|spawned` read, and the consumer whose
// answer decides A2.  A pure read: it is the function the participating arm gates on.
using UserSpawnedGetFn = uint64_t (*)(void* netGame, uint64_t index);
// SESSION.KV_SIDE_SELECT -- the READER's own side selector, asked at run time rather than
// predicted, so A2 writes the record the engine will actually read.
using KvSideSelectFn   = uint8_t  (*)(void* store, uint64_t slot);
using KvSetFlatFn      = void     (*)(void* store, const char* key, int32_t value);
using KvSetSlotFn      = void     (*)(void* store, uint64_t slot, const char* key, int32_t value);
// IDENT.SERVER_ASSIGN_TEAM -- rewrites the packed role word AND re-runs the engine's own
// participating/spectating dispatch.  Argument 2 is homed as a word by the callee's own prologue.
using AssignTeamFn     = void     (*)(void* netGame, uint16_t index, uint32_t teamId);
// NETGAME.PARTICIPATING_VA and the dress half sub_1401C7BE0 -- one shape, (ng, entrant index).
using EntrantDispatchFn = void    (*)(void* netGame, uint16_t index);

struct HopCallTable {
    ActorLookupFn    lookup    = nullptr;   // ENGINE.ACTOR_GUID_CONTAINER_LOOKUP(ROOT, guid.lo)
    ActorIsEnabledFn isEnabled = nullptr;   // ENGINE.ACTOR_IS_ENABLED(root, &guid)
    ActorGuidCanonFn canon     = nullptr;   // ENGINE.ACTOR_GUID_CANON(root, &copyOfGuid)
    UserSpawnedGetFn spawnedGet = nullptr;  // NETGAME.USER_SPAWNED_GET(ng, index)
    KvSideSelectFn   sideSelect = nullptr;  // SESSION.KV_SIDE_SELECT(store, slot)
    KvSetFlatFn      setFlat    = nullptr;  // SESSION.KV_SET_FLAT(store, "user[N]|spawned", 1)
    KvSetSlotFn      setSlot    = nullptr;  // SESSION.KV_SET_SLOT(store, slot, "spawned", 1)
    AssignTeamFn     assignTeam = nullptr;  // IDENT.SERVER_ASSIGN_TEAM(ng, index, role)
    EntrantDispatchFn participating = nullptr;  // NETGAME.PARTICIPATING(ng, index)
    EntrantDispatchFn dress         = nullptr;  // sub_1401C7BE0(ng, index)
    // The store the two setters write, and the slot count the per-slot side is bounded by.  A
    // test's stand-in supplies its own; the shipped build derives both from the netgame.
    void*            store      = nullptr;
    const uint64_t*  storeSlotCount = nullptr;
};

// The hop surface and everything its hooks carry.  The CR15NetGame is re-fetched through
// `netGame` on every call rather than cached: the provisioner's pass runs across a level load, and
// a stale netgame pointer is the one input that would make every read answer confidently about
// freed memory.  The storage is the caller's and must outlive `hops`.
struct ActorHopSurface {
    void* (*netGame)() = nullptr;
    // HOP 4's PROVIDER SLOT.  The CR15RemotePlayerCS whose element pool carries every entrant's
    // SR15RemotePlayerCI, asked for on every call for the same reason `netGame` is: it is resolved
    // out of the game space and does not survive a level load.  Hop 4 is a pure READ, so this is a
    // pointer the caller already holds and never an engine lookup issued from inside the hop.
    void* (*remotePlayerCs)() = nullptr;
    const HopCallTable* calls = nullptr;    // null: the engine, at its recorded VAs
    PawnProvision::ActorHops hops;
};

// Binds every hop this build can read the engine back on, and the packet's two authored hooks.
// `physicsCi` and `physicsCiAwaiting` are bound off the RemotePlayer CI's own record; the block
// above the binding site says how the element is located and why the fallback is still refused.
void BindActorHops(ActorHopSurface&);

// A2's KEY, exposed so a test can hold it against the engine's OWN formatter for the same slot.
// A one-byte difference here is a silent miss, not an error, which is why the format itself is
// pinned to the registry: `SpawnedKeyFormat` returns the registry's string and nothing else, and
// `SpawnedSlotKey` is DERIVED from it (the tail past the separator) rather than written twice.
const char* SpawnedKeyFormat();
const char* SpawnedSlotKey();
bool FormatSpawnedKey(char* out, size_t n, uint64_t slot);

}  // namespace identity_access
}  // namespace replay
