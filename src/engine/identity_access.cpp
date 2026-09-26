// E-9: the engine side of names, profiles, loadouts, skins and colours.
//
// Every number reaches this file from identity_access_facts.h, which is generated from
// the W2-C fact drop by the script that decoded each field out of the instruction bytes
// in the W2-C anchor drop.  Nothing here is transcribed.  The structs below are
// hand-declared and compared against those constants: the design -- a guard that
// generates both sides of its own comparison can never fail.
//
// The one thing to know before reading: FOUR paths in this subsystem do nothing and log
// nothing.  ApplyLoadoutItems returns on an empty profile CJson; ApplyCurrentLoadout returns
// on session-flags bit 0x4; GenerateBotLoadout returns unless the session is authoritative;
// and OverrideAlignment returns when the stored override already equals the new one.  Each
// has an explicit Refusal here, checked BEFORE the call, so the caller learns which one bit.

#include "engine/identity_access.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <type_traits>

#include <windows.h>   // __try/__except filter codes for the guarded stat load

#include "bootstrap/buildguard.h"
#include "bootstrap/config.h"
#include "bootstrap/hookmgr.h"
#include "bootstrap/livesafety.h"
#include "bootstrap/log.h"
#include "engine/actor_alias.h"
#include "engine/entity_json.h"
#include "engine/tick_policy.h"
#include "engine/wiretap.h"
#include "engine/identity_access_facts.h"
#include "generated/facts_generated.h"
#include "events/events_facts.h"

namespace replay::identity_access {

// Every k* below is from identity_access_facts.h; every facts::* is from the merged registry.
using namespace ::replay::identity_facts;

namespace {

// The 4-byte stride of the slot->index table is the SIB scale of
// `movzx ebx, word ptr [rcx+r9*4+138h]`, whose bytes are the anchor of
// IDENT.SLOT_TABLE_OFF: 42 0F B7 9C 89 38 01 00 00, SIB 0x89 = scale 4.
constexpr uint32_t kSlotTableStride = 4;

// 3 << 4, from the two instructions IDENT.CUST_SLOT_WALK_BYTES pins.  No instruction in this
// build encodes 48, so it is derived here and the byte string is what goes red if it moves.
constexpr uint32_t kCustSlotStride = 3u << 4;

constexpr uint16_t kNoAlignment = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
constexpr uint16_t kInvalidU16  = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids

// Session-flags bit that makes IDENT.APPLY_CURRENT_LOADOUT return without applying anything.
constexpr uint32_t kFlagNoCosmetics = 0x4;

static_assert(sizeof(LoadoutHeader) == 64, "the live loadout header is 64 bytes");
static_assert(offsetof(LoadoutHeader, instanceCount) == kLOADOUT_COUNT_OFF, "instance count moved");
static_assert(offsetof(LoadoutHeader, number) == kLOADOUT_NUMBER_OFF, "loadout|number moved");
static_assert(offsetof(LoadoutHeader, numberBody) == kLOADOUT_NUMBER_BODY_OFF, "number_body moved");
static_assert(sizeof(LoadoutInstance) == 64, "an instance is 1 << IDENT.LOADOUT_ENTRY_SHIFT bytes");
static_assert(sizeof(LoadoutInstance) == (1u << kLOADOUT_ENTRY_SHIFT), "instance stride disagrees");
static_assert(offsetof(LoadoutInstance, itemCount) == kINSTANCE_ITEM_COUNT_OFF, "item count moved");
static_assert(sizeof(LoadoutItem) == 16, "the apply loop steps 16 bytes per item");
static_assert(sizeof(CustSlotEntry) == kCustSlotStride, "the slot entry is not 48 bytes");
static_assert(offsetof(CustSlotEntry, flags) == kCUST_SLOT_FLAGS_OFF, "slot flags moved");
static_assert(offsetof(CustSlotEntry, alignmentReq) == kCUST_SLOT_ALIGNREQ_OFF, "alignmentreq moved");
// The recording's "no alignment was read" value and the engine gate's own sentinel are the same
// number, and L5 leans on that: a descriptor that never carried an alignment must not read
// as an entrant who requires alignment 0xFFFF.
static_assert(kPlayerNoAlignment16 == kSlotAlignmentNone, "recorded no-alignment moved");
static_assert(offsetof(CustSlotEntry, item) == kCUST_SLOT_ITEM_OFF, "the applied item moved");
static_assert(offsetof(CustSlotEntry, pendingItem) == kCUST_SLOT_PENDING_OFF, "the pending item moved");
static_assert(sizeof(ActorComponentRef) == 80, "the actor component reference is 80 bytes");
static_assert(offsetof(ActorComponentRef, componentSystem) == 0x40, "IDENT.ACTOR_REF_CS_BYTES");
static_assert(offsetof(ActorComponentRef, handle) == 0x48, "IDENT.ACTOR_REF_HANDLE_BYTES");
// The two masks are numerically equal and name different fields of one word.  This asserts
// they were not collapsed into one id somewhere upstream.
static_assert(kUSER_SLOT_MASK == kUSER_TEAMSLOT_MASK, "if these ever differ, the code below is wrong");

inline uint8_t* At(void* base, uint64_t off) {
    return static_cast<uint8_t*>(base) + off;
}

template <typename T>
inline T Rd(const void* base, uint64_t off) {
    T v;
    memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof(T));
    return v;
}

template <typename T>
inline void Wr(void* base, uint64_t off, const T& v) {
    memcpy(static_cast<uint8_t*>(base) + off, &v, sizeof(T));
}

inline uint64_t UserOff(uint16_t index) {
    return uint64_t(facts::USER_ARRAY_OFF) + uint64_t(facts::USER_STRIDE) * index;
}

// The 16 bytes of the pawn record the engine's actor allocator owns: the space guid
// at record + 8 and the actor guid at record + 16 (ng + 592*slot + 384 / + 392).  `sub_1404E61E0`
// writes this exact pattern into them before it looks anything up, so it is the value a slot that
// holds no pawn carries -- measured on every slot of a first load.
constexpr uint32_t kPawnRecordActorOff = 8u;
constexpr uint64_t kNoActorSpaceGuid   = 0xFFFFFFFFFFFFFFFFull;   // facts-ok: LOCAL.NO_CONSTRAINT the allocator's own all-ones "none"
constexpr uint64_t kNoActorGuid        = 0x0000FFFFFFFFFFFFull;   // facts-ok: LOCAL.NO_CONSTRAINT class/index/generation all ones, the two spare bytes zero

bool  gChecked = false;
bool  gMatches = false;

// Signatures as the decompiled bodies take them, not as a header wished they were.  The
// fourth argument of IDENT.GEN_BOT_LOADOUT is a lobby entrant whose first dword selects the
// branch and which it dereferences unconditionally -- that is why it is absent below.
using FnApplyLoadoutToActor = void  (*)(void*, uint32_t, uint64_t, uint32_t, const void*, uint32_t);
using FnActorComponentRef   = void* (*)(void*, void*, const void*, int64_t);
using FnAlignment           = uint64_t (*)(void*, uint16_t);
using FnOverrideAlignment   = void* (*)(void*, uint32_t, uint16_t);
using FnApplyAlignment      = void* (*)(void*, uint32_t);
// IDENT.APPLY_CHASSIS (sub_140CABF10): (ng, userIndex, itemId, pairedVal, 0).  edx = the entrant's
// user index, r8 = the item, r9 = the worn-record entry's key (v21 = *(v20-2) in APPLY_CURRENT_LOADOUT).
using FnApplyChassis        = void  (*)(void*, uint32_t, uint64_t, uint64_t, uint64_t);
using FnApplyAllUsers       = void  (*)(void*);
using FnJsonIsEmpty         = bool  (*)(const void*);
using FnJsonSetString       = void  (*)(void*, const char*, const char*);
using FnJsonSetInt          = void  (*)(void*, const char*, int64_t);
using FnJsonGetString       = const char* (*)(const void*, const char*, const char*, uint32_t);
using FnUserDisplayName     = const char* (*)(void*, uint16_t);
using FnLoadoutDocSetSlot   = void  (*)(void*, const uint64_t*, const char*);

// ---------------------------------------------------------------------------------------
// The site table.  Every engine address this module calls or reads is here and nowhere else.
//
// echovr.exe is DYNAMICBASE + HIGH_ENTROPY_VA: the recorded 0x140...... addresses are against
// the PE's preferred base and the loader does not honour it.  buildguard::MappedAddress is the
// tree's one rebase point, and buildguard::VerifySite rebases, range-checks, compares the
// twenty bytes and names the owning module when another tool got to the site first.  Both are
// used here; nothing in this file dereferences a recorded VA.
//
// The two flavours are checked identically and on purpose.  Nine are entry points we call; the
// last three are byte SHAPES -- the instruction sequences that establish the 48-byte
// customisation stride and the two ActorComponentRef field positions.  A drifted struct offset
// would otherwise be discovered as a wrong avatar.
// ---------------------------------------------------------------------------------------
// ONE list, two generated forms.  L3 #20 (pid 62356) died with an access violation at
// IDENT.JSON_ELEM_COUNT+0x16 because this was TWO hand-kept lists: W2-P1-16's three rows were
// INSERTED at row 11 of the table and APPENDED to the enum, so Site::JsonParseText named
// IDENT.JSON_ELEM_COUNT, Site::JsonDocCtor named IDENT.JSON_PATH_ELEM, and every accessor from
// JsonNodeKind down was bound to a neighbour three rows away.  Neither guard could see it: the
// count static_assert compares two lengths, and VerifySite compares row i's recorded bytes at
// row i's OWN address, which is exactly as true of a misordered table as of a correct one.  The
// enum and the rows are generated from one list here, so they cannot disagree by construction.
#define IDENT_SITE_LIST(X)        \
    X(ApplyLoadoutToActor    , facts::IDENT_APPLY_LOADOUT_TO_ACTOR  , facts::IDENT_APPLY_LOADOUT_TO_ACTOR_PROLOGUE) \
    X(CustomisationAlignment , facts::IDENT_CUSTOMISATION_ALIGNMENT , facts::IDENT_CUSTOMISATION_ALIGNMENT_PROLOGUE) \
    X(UserDisplayName        , facts::IDENT_USER_DISPLAY_NAME       , facts::IDENT_USER_DISPLAY_NAME_PROLOGUE) \
    X(ActorComponentRef      , kACTOR_COMPONENT_REF                 , kACTOR_COMPONENT_REFPrologue) \
    X(OverrideAlignment      , kOVERRIDE_ALIGNMENT                  , kOVERRIDE_ALIGNMENTPrologue) \
    X(ApplyAlignmentChange   , kAPPLY_ALIGNMENT_CHANGE              , kAPPLY_ALIGNMENT_CHANGEPrologue) \
    X(ApplyChassis           , kAPPLY_CHASSIS                       , kAPPLY_CHASSISPrologue) \
    X(ApplyAlignmentAllUsers , kAPPLY_ALIGNMENT_ALL_USERS           , kAPPLY_ALIGNMENT_ALL_USERSPrologue) \
    X(JsonIsEmpty            , kJSON_IS_EMPTY                       , kJSON_IS_EMPTYPrologue) \
    X(JsonSetString          , kJSON_SET_STRING                     , kJSON_SET_STRINGPrologue) \
    X(JsonSetInt             , kJSON_SET_INT                        , kJSON_SET_INTPrologue) \
    X(JsonGetString          , kJSON_GET_STRING                     , kJSON_GET_STRINGPrologue) \
    X(JsonDocCtor            , kJSON_DOC_CTOR                       , kJSON_DOC_CTORPrologue) \
    X(JsonParseText          , kJSON_PARSE_TEXT                     , kJSON_PARSE_TEXTPrologue) \
    X(JsonGetInt             , kJSON_GET_INT                        , kJSON_GET_INTPrologue) \
    X(JsonNodeKind           , facts::IDENT_JSON_NODE_KIND          , facts::IDENT_JSON_NODE_KIND_PROLOGUE) \
    X(JsonNodeName           , facts::IDENT_JSON_NODE_NAME          , facts::IDENT_JSON_NODE_NAME_PROLOGUE) \
    X(JsonNodeString         , facts::IDENT_JSON_NODE_STRING        , facts::IDENT_JSON_NODE_STRING_PROLOGUE) \
    X(JsonNodeInt            , facts::IDENT_JSON_NODE_INT           , facts::IDENT_JSON_NODE_INT_PROLOGUE) \
    X(JsonNodeBool           , facts::IDENT_JSON_NODE_BOOL          , facts::IDENT_JSON_NODE_BOOL_PROLOGUE) \
    X(JsonNextElem           , facts::IDENT_JSON_NEXT_ELEM          , facts::IDENT_JSON_NEXT_ELEM_PROLOGUE) \
    X(JsonPathElem           , facts::IDENT_JSON_PATH_ELEM          , facts::IDENT_JSON_PATH_ELEM_PROLOGUE) \
    X(JsonElemCount          , facts::IDENT_JSON_ELEM_COUNT         , facts::IDENT_JSON_ELEM_COUNT_PROLOGUE) \
    X(JsonFindNode           , facts::IDENT_JSON_FIND_NODE          , facts::IDENT_JSON_FIND_NODE_PROLOGUE) \
    X(CustSlotWalkShape      , kCUST_SLOT_WALK_BYTESVa              , kCUST_SLOT_WALK_BYTES) \
    X(ActorRefCsShape        , kACTOR_REF_CS_BYTESVa                , kACTOR_REF_CS_BYTES) \
    X(ActorRefHandleShape    , kACTOR_REF_HANDLE_BYTESVa            , kACTOR_REF_HANDLE_BYTES)  \
    X(ActorRefIsValid        , facts::IDENT_ACTOR_REF_IS_VALID_VA   , facts::IDENT_ACTOR_REF_IS_VALID_VA_PROLOGUE) \
    X(ActorGuidCanon         , kACTOR_GUID_CANON                    , kACTOR_GUID_CANONPrologue) \
    X(LoadoutDocSetSlot      , facts::IDENT_LOADOUT_DOC_SET_SLOT_VA , facts::IDENT_LOADOUT_DOC_SET_SLOT_VA_PROLOGUE)

enum class Site : uint8_t {
#define IDENT_SITE_ENUMERATOR(name, va, bytes) name,
    IDENT_SITE_LIST(IDENT_SITE_ENUMERATOR)
#undef IDENT_SITE_ENUMERATOR
    Count,
};

constexpr size_t kSiteBytes = 20;

struct SiteRow {
    const char*    name;
    uint64_t       va;
    const uint8_t* bytes;
};

constexpr SiteRow kSites[] = {
#define IDENT_SITE_ROW(name, va, bytes) {#name, va, bytes},
    IDENT_SITE_LIST(IDENT_SITE_ROW)
#undef IDENT_SITE_ROW
};

static_assert(sizeof kSites / sizeof *kSites == size_t(Site::Count),
              "every Site needs a row, and every row a Site: Entry() indexes one by the other");

// Which entry points failed, as a fixed string built once.  Named individually because "the
// build does not match" is useless when one function of twenty-two has been detoured by another
// mod and the rest are fine.
//
// The size is DERIVED FROM THE TABLE, not chosen: the longest answer this can ever have to give
// is every site name, space-separated.  It was a fixed 256 bytes, which named the first sixteen
// and silently dropped the rest once W1-E's readers and JSON accessors took the table to
// twenty-two -- a detail that stops naming sites is exactly the "one bool for eleven answers"
// defect this string exists to remove, and a buffer whose size does not come from what it has to
// hold will be wrong again the next time a site is added.
constexpr size_t MismatchBufferBytes() {
    size_t n = 1;  // the terminator
    for (const SiteRow& s : kSites) {
        size_t len = 0;
        while (s.name[len]) ++len;
        n += len + 1;  // the name, and the separator in front of it
    }
    return n;
}

char gMismatch[MismatchBufferBytes()] = "";

void Note(const char* name) {
    const size_t used = strlen(gMismatch);
    const size_t sep  = used ? 1u : 0u;
    const size_t n    = strlen(name);
    // Unreachable by construction -- MismatchBufferBytes() sizes for every site at once -- and
    // kept as the refusal it would have to be if that ever stopped being true.
    if (used + sep + n + 1 > sizeof gMismatch) {
        gMismatch[sizeof gMismatch - 1] = '\0';
        return;
    }
    if (sep) gMismatch[used] = ' ';
    memcpy(gMismatch + used + sep, name, n + 1);
}

const void* gResolved[size_t(Site::Count)] = {};

// Null until BuildMatches() has run and accepted this site.  Every caller checks it, because
// "the address is not in this image" is now a reachable answer rather than a fault.
// The ONE place a Site becomes a row index.  Both the resolved entry points and the recorded VA
// JsonEntryPointVa() publishes go through it, so the mutant below moves them together -- which is
// what the L3 #20 defect did, and what a check that only reads one of the two could not see.
constexpr size_t SiteIndex(Site s) {
#ifdef W2P1_MUTATE_SITE_INDEX_SHIFT
    return (size_t(s) + 3u) % size_t(Site::Count);
#else
    return size_t(s);
#endif
}

template <typename F>
inline F Entry(Site s) {
    if (!BuildMatches()) return nullptr;
    return reinterpret_cast<F>(reinterpret_cast<uintptr_t>(gResolved[SiteIndex(s)]));
}

}  // namespace

const char* RefusalText(Refusal r) {
    switch (r) {
        case Refusal::None:             return "None: none";
        case Refusal::ProfileNotRecorded: return "ProfileNotRecorded: the capture carries no profile document for this player, so there is nothing to attach";
        case Refusal::ProfileNotParsed: return "ProfileNotParsed: the engine's own json parser refused the recorded document";
        case Refusal::LoadoutNotRecorded: return "LoadoutNotRecorded: every one of the twenty recorded item ids is the no-model sentinel, so this row has no loadout to apply";
        case Refusal::LoadoutStepOff: return "LoadoutStepOff: playback.dress_loadout is off, so the outfit step was not asked to run";
        case Refusal::EntrantRecordUnreadable: return "EntrantRecordUnreadable: the lobby member record for this slot is not in committed memory";
        case Refusal::EntrantRecordEmptyNode: return "EntrantRecordEmptyNode: the lobby message carried no profile document, so the record holds the empty node";
        case Refusal::ReleaseNotContained: return "ReleaseNotContained: this process is not a contained offline session, so no entrant may be removed from the lobby";
        case Refusal::NotOurEntrant:    return "NotOurEntrant: that slot is not in this session's admission ledger, so this plugin did not put the entrant there and will not take it out";
        case Refusal::EntrantIsLocal:   return "EntrantIsLocal: that entrant is in the engine's local-user band, which the removing-entrant handler refuses and which is where the viewer sits";
        case Refusal::ReleaseCallRefused: return "ReleaseCallRefused: the byte-anchored call to the engine's NSLobbyRemovingEntrant handler did not issue";
        case Refusal::EntrantStillSeated: return "EntrantStillSeated: the leave was fed and the slot still resolves to an entrant that names it back";
        case Refusal::SpectatorRow:     return "SpectatorRow: the recorded team is at or above IDENT.ALIGN_TEAM_LIMIT, so this row was never a playing entrant";
        case Refusal::ReasonNotRecorded: return "ReasonNotRecorded: the step refused and the identity recorded no reason for it";
        case Refusal::NoNetGame:        return "NoNetGame: no CR15NetGame yet";
        case Refusal::BuildMismatch:    return "BuildMismatch: a called entry point does not match its recorded prologue";
        case Refusal::SlotOutOfRange:   return "SlotOutOfRange: slot is outside the engine's own 16-entry slot table";
        case Refusal::NoSuchEntrant:    return "NoSuchEntrant: no entrant in that slot, or the generation disagrees";
        case Refusal::ReplayBand:       return "ReplayBand: that index is inside the replay band and would be renamed \"Replayer %u\"";
        case Refusal::EmptyProfile:     return "EmptyProfile: the entrant's profile CJson is absent or empty, so the loadout apply would return silently";
        case Refusal::NoLoadoutInstance: return "NoLoadoutInstance: the live loadout table holds no instance for that slot";
        case Refusal::NoActorComponent: return "NoActorComponent: the entrant's SActorGUID does not resolve to a customisation component";
        case Refusal::NotAuthoritative: return "NotAuthoritative: session flags bit 0x4 is set, so ApplyCurrentLoadout would apply nothing";
        case Refusal::ShortInstance:    return "ShortInstance: the engine's loadout instance holds fewer items than the recording carries";
        case Refusal::NoSlotNameTable:  return "NoSlotNameTable: the slot-name table is absent, or that slot is past the vector's own bound";
        case Refusal::SlotNameTooLong:  return "SlotNameTooLong: the recorded display name does not fit the engine's inline slot-name buffer";
        case Refusal::NameNotResolved:  return "NameNotResolved: CR15NetGame::UserDisplayName did not answer, so the rendered name could not be observed";
        case Refusal::NoReleasePath:    return "NoReleasePath: the engine's NSLobbyRemovingEntrant handler did not bind by byte-anchor on this image, so there is nothing to hand the body to";
        case Refusal::RosterUnreadable: return "RosterUnreadable: the entrant count, or the user array it claims, is not in committed memory";
        case Refusal::ShortBuffer:      return "ShortBuffer: the caller's buffer is shorter than the value; nothing was copied and the full length was reported";
        case Refusal::NoJsonWriter:     return "NoJsonWriter: RETIRED: the profile document is serialised from the node tree, so this is no longer a reachable outcome";
        case Refusal::JsonNoRoot:               return "JsonNoRoot: the entrant's profile document has no root node";
        case Refusal::JsonNodeUnreadable:       return "JsonNodeUnreadable: a node inside the entrant's profile document is not in committed memory";
        case Refusal::JsonDepthLimit:           return "JsonDepthLimit: the entrant's profile document is deeper than the walk's depth bound";
        case Refusal::JsonNodeLimit:            return "JsonNodeLimit: the entrant's profile document holds more nodes than the walk's node bound";
        case Refusal::JsonCycle:                return "JsonCycle: a node in the entrant's profile document is one of its own ancestors";
        case Refusal::JsonArrayNotWalkable:     return "JsonArrayNotWalkable: the entrant's profile holds a populated array, and IDENT.JSON_NEXT_ELEM iterates objects only";
        case Refusal::JsonNumberNotExact:       return "JsonNumberNotExact: the entrant's profile holds a non-integer number, and the only per-node accessor on this build returns float32";
        case Refusal::JsonUnknownKind:          return "JsonUnknownKind: IDENT.JSON_NODE_KIND does not name the tag of a node in the entrant's profile";
        case Refusal::JsonChildCountDisagrees:  return "JsonChildCountDisagrees: the child walk and IDENT.JSON_ELEM_COUNT disagree about a container in the entrant's profile";
        case Refusal::JsonStringUnreadable:     return "JsonStringUnreadable: a key or string value in the entrant's profile is null, unreadable or unterminated";
        case Refusal::JsonStringNotUtf8:        return "JsonStringNotUtf8: a key or string value in the entrant's profile is not valid UTF-8";
        case Refusal::JsonTreeChanged:          return "JsonTreeChanged: the entrant's profile changed between the sizing walk and the emitting walk";
        case Refusal::JsonNoSuchKey:            return "JsonNoSuchKey: the entrant's profile has no node at the key path that was asked for";
        case Refusal::StatsNoFillMask:          return "StatsNoFillMask: no fill mask was passed, so a stat the engine has not written could not be reported as absent";
        case Refusal::StatsUnreadable:          return "StatsUnreadable: the entrant's stat block is not in committed memory";
        case Refusal::EmoteNoSystem:            return "EmoteNoSystem: no R15NetCustomization component system was named for the emote read";
        case Refusal::EmoteUnreadable:          return "EmoteUnreadable: the emote id map, the pool, or the handle it selects is not in committed memory";
        case Refusal::EmoteVtableUnreadable:    return "EmoteVtableUnreadable: the emote handle's target carries no readable pointer in the dispatch slot";
        case Refusal::EmoteNoSeam:              return "EmoteNoSeam: no entity-layer EmoteSource seam is installed, so no emote source could be asked for";
        case Refusal::ProfileReadOnly:          return "ProfileReadOnly: the engine holds this entrant's profile document in its json db cache, so every key written to it is refused with `json db is cached, read only` and nothing is applied";
        case Refusal::ProfileNotReadBack:       return "ProfileNotReadBack: the profile was written and the engine's own reader does not answer the recorded value back at the key it went to";
        case Refusal::LoadoutsUnreadable:       return "LoadoutsUnreadable: the entrant's live loadout header, or the instance vector its own count claims, is not in committed memory -- the walk was not started, so this is not a count of zero";
        case Refusal::TeamNotRecorded:          return "TeamNotRecorded: the recording carries no team for this player -- the manifest's 8-bit team field holds its absent sentinel, so there is nothing to apply.  The engine refused nothing and the step had no input; every other step of the dress is unaffected";
        case Refusal::EngineCallReentrant:      return "EngineCallReentrant: this call would have entered the engine from inside an engine call of ours -- the participating arm runs synchronously, so a nested transition is refused rather than started";
        case Refusal::AppliedSlotsUnstable:     return "AppliedSlotsUnstable: the two reads of the customisation slot array disagreed, so the engine wrote the entries between them and no per-slot outcome can be attributed";
        case Refusal::TeamNotApplied:           return "TeamNotApplied: the entrant's team was not applied -- the raw write to the packed role word is retired, so the team travels IDENT.SERVER_ASSIGN_TEAM and either that call did not bind, the session refused it, the role is not one the engine accepts, or the engine's own word did not read it back";
        case Refusal::ProfileNotAttached:       return "ProfileNotAttached: the entrant carries no profile document, or still carries the json db's cached one, so its loadouts describe the cache's user and were not read";
    }
    return "unknown";
}

bool BuildEntrantRemovedBody(UserSlot slot, uint8_t out[kEntrantRemovedBodyLen]) {
    if (!out) return false;
    memset(out, 0, kEntrantRemovedBodyLen);
    const uint64_t v = slot.v;
    memcpy(out + events_facts::kRemovedSlotOff, &v, sizeof v);
    return true;
}

namespace {

// ★ THE ADMISSION LEDGER, and it is the whole of the leave's target-set proof.
//
// One bit per lobby slot that THIS PROCESS's ProvisionReplayUser actually seated an entrant into --
// written only where the admission's own postcondition passed, which is the only place that knows
// the engine created something.  ReleaseReplayUser refuses by name any slot that is not in it, so
// the set the leave can ever name is exactly the set this plugin admitted: the owner's own viewer
// entrant was never admitted by us and therefore can never be a target, whatever the roster
// mapping, the residue or a capture file happens to say.
//
// It is SESSION-scoped and deliberately outlives an Unload: an entrant this plugin admitted is
// still ours to remove after the file that named it is gone.  A slot leaves the ledger only when
// its entrant has provably left the engine.
uint32_t g_admittedSlots = 0;

void NoteAdmittedSlot(uint16_t slot) {
    if (slot < facts::ENTITY_USER_SLOT_TABLE_MAX) g_admittedSlots |= uint32_t(1) << slot;
}

// The ledger the leave is allowed to name. Reported by AdmitIntoLobbySlot the moment the engine's
// own pair confirms the entrant exists, so a pass that seats a slot and then DEFERS still records it.
void HookSeated(void* /*ctx*/, uint16_t slot, uint16_t /*index*/) { NoteAdmittedSlot(slot); }

void ForgetAdmittedSlot(uint16_t slot) {
    if (slot < facts::ENTITY_USER_SLOT_TABLE_MAX) g_admittedSlots &= ~(uint32_t(1) << slot);
}

bool SlotWasAdmittedHere(uint16_t slot) {
    return slot < facts::ENTITY_USER_SLOT_TABLE_MAX &&
           (g_admittedSlots & (uint32_t(1) << slot)) != 0;
}

// Defined beside the add's own direct call, and forward-declared here because the release half that
// issues it is written far above it.
bool DirectEntrantRemovedCall(uint16_t slot);
bool LeaveHandlerBound();

}  // namespace

uint32_t AdmittedSlotMask() { return g_admittedSlots; }

// Our Unload drives the engine's own NSLobbyRemovingEntrant, which frees the pooled
// actor -- the pool reads `free 15 of 15` straight afterwards -- but the pawn records keep the freed
// actors' guids, because the teardown COMPACTS: removing entrant i copies the LAST entrant's whole
// record down onto i, and nothing ever wipes the record it came from.  Measured on the reload that
// killed the process: after eight leaves the eight records read 7,6,5,4,4,5,6,7 -- the first four
// dragged down by compaction, the last four left where they were, all eight naming dead actors.
// A first load finds the allocator's all-ones "none" instead and takes SET_PLAYER_ACTOR's allocate
// arm cleanly; the reload's resolve of a dead guid dereferences null inside sub_140CB89B0 with
// nothing between it and the process.
//
// So the records to restore are the VACANT ones -- at or above the engine's own user count, which is
// the one number that says where the live entrants end.  A live entrant is always below it, so this
// can never wipe a pawn somebody is still using; that is the whole reason the count is the bound and
// the leaving slot is not.  Returns how many records it restored.
uint32_t ClearVacantPawnActorGuids(void* netGame) {
    if (netGame == nullptr) return 0;
    if (!Readable(At(netGame, facts::IDENT_ENTRANT_COUNT_OFF), sizeof(uint16_t))) return 0;
    const uint16_t users = Rd<uint16_t>(netGame, facts::IDENT_ENTRANT_COUNT_OFF);
    if (users > facts::ENTITY_USER_SLOT_TABLE_MAX) return 0;   // nonsense count: touch nothing
    uint32_t cleared = 0;
    for (uint16_t i = users; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i) {
        const uint64_t off = uint64_t(facts::NETGAME_PAWN_TABLE_OFF) +
                             uint64_t(facts::NETGAME_PAWN_TABLE_STRIDE) * i + kPawnRecordActorOff;
        if (!Readable(At(netGame, off), 16)) break;
        if (Rd<uint64_t>(netGame, off) == kNoActorSpaceGuid &&
            Rd<uint64_t>(netGame, off + 8) == kNoActorGuid) {
            continue;
        }
        Wr<uint64_t>(netGame, off, kNoActorSpaceGuid);
        Wr<uint64_t>(netGame, off + 8, kNoActorGuid);
        ++cleared;
    }
    return cleared;
}

// The one reading that says WHY a recycled pool entry's handle is refused, taken at
// the four moments it means anything: either side of an admission and either side of a leave.  The
// engine resolves an actor guid by an exact match on {classId, index, generation} in the
// ncaActorAlias table, and the free is the only thing that ever advances a pooled record's
// generation -- so a row missing at re-admission and a row a generation behind are different faults
// that look identical from the crash.  Read-only, one line, never from a frame.
// The class the replayed entrants' actors actually came out of, remembered from the first pawn
// record that names a live one.  Not spelled as a number and not looked up in the level's name
// table: the class that matters is the one the engine ITSELF allocated for these entrants, and the
// guid it wrote carries it in its low word.  It survives an Unload because the reload's own
// "before" reading -- taken when every record has been restored to "no actor" -- is the one that
// has to name a class, and the session that poisoned the entry is the session that taught it.
static uint16_t g_entrantActorClass = kInvalidU16;

static void LearnEntrantActorClass(void* netGame) {
    // From slot 1 up. Slot 0 is the local viewer, whose actor comes out of a different class
    // (67 in the a1 measurement, against the admitted entrants' 59), so a scan that starts at 0
    // learns the one class none of the replayed pawns is ever allocated from.
    for (uint16_t i = 1; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i) {
        const uint64_t off = uint64_t(facts::NETGAME_PAWN_TABLE_OFF) +
                             uint64_t(facts::NETGAME_PAWN_TABLE_STRIDE) * i + kPawnRecordActorOff;
        if (!Readable(At(netGame, off), 16)) return;
        const uint64_t actor = Rd<uint64_t>(netGame, off + 8);
        if (actor == kNoActorGuid || actor == 0) continue;
        g_entrantActorClass = uint16_t(actor & 0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed actor guid, not the engine no-slot field
        return;
    }
}

void ForgetEntrantActorClass() { g_entrantActorClass = kInvalidU16; }

static void LogActorAliasRows(void* netGame, const char* when, unsigned slot) {
    if (netGame == nullptr) return;
    const uint8_t* const at = static_cast<const uint8_t*>(netGame) + facts::NETGAME_CS_SPACE_OFF;
    void* space = nullptr;
    if (!Readable(at, sizeof space)) return;
    memcpy(&space, at, sizeof space);
    if (space == nullptr) return;
    LearnEntrantActorClass(netGame);
    if (g_entrantActorClass == kInvalidU16) {
        log::Direct(LogLevel::Info, "pawn",
                    "alias %s slot %u: no entrant actor class seen yet", when, slot);
        return;
    }
    // BOTH objects, each table named by the offset it was found at.  RELOAD-4 read
    // only the child space and settled by content on space+3192 -- ncaPhysics, not the ncaActorAlias
    // the engine's resolve reads -- so every reading it took was of a table no consumer consults.
    void* root = nullptr;
    if (Readable(At(netGame, kNETGAME_GAMESPACE_OFF), sizeof root))
        memcpy(&root, At(netGame, kNETGAME_GAMESPACE_OFF), sizeof root);
    char line[4096];
    actoralias::DescribeReload(root, space, g_entrantActorClass, line, sizeof line);
    log::Direct(LogLevel::Info, "pawn", "alias %s slot %u: %s", when, slot, line);
}

// RUN THE FREE'S OWN RE-KEY FOR THE ENTRY THE FREE MISSED.
//
// The unload's engine free advances the pool generation for every entry it releases and then
// broadcasts the old guid to each component system's +640 "actor re-keyed" virtual.  One entry per
// 8-player session comes back with its record still at the old generation; the handle the next
// allocation stamps then resolves to nothing, and two engine consumers dereference that null without
// a guard -- the admission itself, and an ncaScript graph of the freed actor that is still
// subscribed and asks for its own SELF key long after the load.  Repairing the record closes both,
// because a resolvable entry is what every other entry already is.
//
// Runs after every leave (so the window between the unload and the load is closed too) and before
// every admission.  READ-ONLY, and silent unless something lags: this is the instrument that says
// ENG-RELOAD-5's paced top-down leave is still making the engine's own re-key land.
static void ReportActorRecordLag(void* netGame, const char* when) {
    if (netGame == nullptr || g_entrantActorClass == kInvalidU16) return;
    void* space = nullptr;
    void* root = nullptr;
    if (!Readable(At(netGame, facts::NETGAME_CS_SPACE_OFF), sizeof space)) return;
    memcpy(&space, At(netGame, facts::NETGAME_CS_SPACE_OFF), sizeof space);
    if (space == nullptr) return;
    if (Readable(At(netGame, kNETGAME_GAMESPACE_OFF), sizeof root))
        memcpy(&root, At(netGame, kNETGAME_GAMESPACE_OFF), sizeof root);

    actoralias::detail::Pool pool;
    if (!actoralias::detail::ReadPool(space, g_entrantActorClass, pool)) return;

    const actoralias::LagReport onRoot =
        root != nullptr ? actoralias::ReportLaggingRecords(root, pool, g_entrantActorClass)
                        : actoralias::LagReport{};
    const actoralias::LagReport onChild =
        actoralias::ReportLaggingRecords(space, pool, g_entrantActorClass);

    const uint32_t lagging = onRoot.lagging + onChild.lagging;
    if (lagging == 0) return;  // the common case, and it says nothing
    log::Direct(LogLevel::Warn, "pawn",
                "actor record lag %s: root %s (%u table(s), %u lagging) child %s (%u table(s), %u "
                "lagging) -- first entry %u row gen %u vs pool gen %u. The next allocation of that "
                "entry stamps a handle the engine's resolve will refuse",
                when, actoralias::LagResultName(onRoot.result), onRoot.tables, onRoot.lagging,
                actoralias::LagResultName(onChild.result), onChild.tables, onChild.lagging,
                unsigned(onRoot.lagging != 0 ? onRoot.firstIndex : onChild.firstIndex),
                unsigned(onRoot.lagging != 0 ? onRoot.firstRowGen : onChild.firstRowGen),
                unsigned(onRoot.lagging != 0 ? onRoot.firstPoolGen : onChild.firstPoolGen));
}

// THE ADMISSION'S LAST GATE, and the one that keeps a reload from killing the
// process.  The engine allocates this entrant's actor inside the call that follows, and it
// dereferences the handle it resolves for it three instructions later with no guard of its own.  A
// pool entry whose registry record the previous session's free failed to re-key resolves to
// nothing, so this asks the engine's own question -- is {class, entry, generation} in the table --
// about the entry the pool is ABOUT to hand out, and moves a resolvable entry to the top of the
// free stack if it is not.  Reads on every admission; writes only when an entry is poisoned, and
// says so at Warn when it does.
static void SteerActorPoolBeforeAdmission(void* netGame, uint64_t slot) {
    if (netGame == nullptr || g_entrantActorClass == kInvalidU16) return;
    const uint8_t* const at = static_cast<const uint8_t*>(netGame) + facts::NETGAME_CS_SPACE_OFF;
    void* space = nullptr;
    if (!Readable(at, sizeof space)) return;
    memcpy(&space, at, sizeof space);
    if (space == nullptr) return;

    ReportActorRecordLag(netGame, "pre-admit");

    const actoralias::Steer s = actoralias::SteerAwayFromUnresolvable(space, g_entrantActorClass);
    const bool quiet = s.result == actoralias::SteerResult::Resolvable;
    log::Direct(quiet ? LogLevel::Info : LogLevel::Warn, "pawn",
                "actor pool steer slot %u: %s -- class %u entry %u gen %u, free %u%s",
                unsigned(slot), actoralias::SteerResultName(s.result),
                unsigned(g_entrantActorClass), unsigned(s.index), unsigned(s.generation),
                s.freeCount,
                quiet ? "" : " -- the engine's actor registry has no row at that generation, so "
                             "the handle it would stamp resolves to nothing and the admission that "
                             "follows may take the process with it");
}

void ForgetAdmittedSlotsForTest() { g_admittedSlots = 0; }
// Seeds the ledger the way a successful admission would.  It exists so the RELEASE can be driven
// without standing a whole admission up first; that the real admission writes the ledger is proved
// where the real admission is driven, not here.
void SeedAdmittedSlotForTest(uint16_t slot) { NoteAdmittedSlot(slot); }

bool BuildMatches() {
    if (gChecked) return gMatches;
    gChecked = true;

    // One pass over the table.  VerifySite rebases onto the mapped image, refuses an address
    // that is not in it, compares the twenty bytes and names the owning module of a foreign
    // detour; MappedAddress is the same rebase, kept so the call itself never re-derives one.
    // A site that fails either half stays null and names itself -- eleven answers, not one bool.
    for (size_t i = 0; i < size_t(Site::Count); ++i) {
        const SiteRow& s = kSites[i];
        if (buildguard::VerifySite(s.name, s.va, s.bytes, kSiteBytes))
            gResolved[i] = buildguard::MappedAddress(s.va);
        if (gResolved[i] == nullptr) Note(s.name);
    }

    gMatches = gMismatch[0] == '\0';
    return gMatches;
}

const char* BuildMismatchDetail() { return gMismatch; }

size_t      EngineSiteCount() { return size_t(Site::Count); }
const char* EngineSiteName(size_t i) { return i < size_t(Site::Count) ? kSites[i].name : nullptr; }
uint64_t    EngineSiteVa(size_t i) { return i < size_t(Site::Count) ? kSites[i].va : 0; }
const void* EngineSiteResolved(size_t i) { return i < size_t(Site::Count) ? gResolved[i] : nullptr; }

bool SlotOfIndex(void* ng, UserIndex index, UserSlot& out) {
    if (!ng) return false;
    if (index.v >= Rd<uint16_t>(ng, kENTRANT_COUNT_OFF)) return false;
    const uint16_t word = Rd<uint16_t>(ng, UserOff(index.v));
    out = UserSlot(static_cast<uint16_t>(word & kUSER_SLOT_MASK));
    return true;
}

bool IndexOfSlot(void* ng, UserSlot slot, UserIndex& out) {
    if (!ng) return false;
    if (slot.v >= facts::ENTITY_USER_SLOT_TABLE_MAX) return false;
    const uint16_t idx = Rd<uint16_t>(ng, uint64_t(kSLOT_TABLE_OFF) + uint64_t(kSlotTableStride) * slot.v);
    if (idx == kInvalidU16) return false;
    out = UserIndex(idx);
    return true;
}

bool NetUserId(void* ng, UserSlot slot, uint32_t& out) {
    if (!ng || slot.v >= facts::ENTITY_USER_SLOT_TABLE_MAX) return false;
    const uint64_t base = uint64_t(kSlotTableStride) * slot.v;
    if (Rd<uint16_t>(ng, uint64_t(kSLOT_TABLE_OFF) + base) == kInvalidU16) return false;
    const uint16_t gen = Rd<uint16_t>(ng, uint64_t(kGEN_TABLE_OFF) + base);
    if (gen == kInvalidU16) return false;   // ApplyLoadoutToActor rejects this itself, silently
    out = uint32_t(slot.v) | (uint32_t(gen) << 16);
    return true;
}

bool InReplayBand(void* ng, UserIndex index) {
    if (!ng) return false;
    return index.v >= Rd<uint16_t>(ng, kNONREPLAY_COUNT_OFF) &&
           index.v <  Rd<uint16_t>(ng, kENTRANT_COUNT_OFF);
}

void* ProfileJson(void* ng, UserIndex index) {
    if (!ng) return nullptr;
    return Rd<void*>(ng, uint64_t(kPROFILE_JSON_OFF) + uint64_t(facts::USER_STRIDE) * index.v);
}

bool ProfileJsonUsable(void* ng, UserIndex index) {
    void* json = ProfileJson(ng, index);
    if (!json) return false;
    auto isEmpty = Entry<FnJsonIsEmpty>(Site::JsonIsEmpty);
    return isEmpty && !isEmpty(json);
}

bool CosmeticsWouldApply(void* ng) {
    if (!ng) return false;
    // CR15NetGame + 0x2DA0 is a POINTER to the flag word, which is why this dereferences
    // twice.  The registry already holds that offset twice -- NETGAME.ROLEFLAGS_OFF and
    // NETGAME.UIFLAGS_PTR_OFF, same base, same value, S4.6 rule 12 -- so W2-C cites the
    // survivor rather than adding a third id for the same field.  ApplyCurrentLoadout tests
    // bit 0x4 of *flags at 0x140CAB3D1 and returns without applying anything when it is set.
    const uint32_t* flags = Rd<const uint32_t*>(ng, facts::NETGAME_UIFLAGS_PTR_OFF);
    if (!flags) return false;
    return (*flags & kFlagNoCosmetics) == 0;
}

void* LoadoutContainer(void* ng, UserSlot slot) {
    if (!ng || slot.v >= facts::ENTITY_USER_SLOT_TABLE_MAX) return nullptr;
#ifdef W2P1_MUTATE_PUBLISH_BY_INDEX
    const uint64_t which = 0;   // the defect: the entrant INDEX, which is 0 for the first entrant
#else
    const uint64_t which = uint64_t(slot.v);
#endif
    void* p = At(ng, uint64_t(facts::USER_PROFILE_OFF) +
                         uint64_t(facts::USER_PROFILE_STRIDE) * which +
                         uint64_t(facts::USER_LOADOUT_OFF));
    if (!p || !Readable(p, sizeof(LoadoutHeader))) return nullptr;
    return p;
}

LoadoutHeader* LiveLoadout(void* ng, UserSlot slot) {
    if (!ng || slot.v >= facts::ENTITY_USER_SLOT_TABLE_MAX) return nullptr;
    return reinterpret_cast<LoadoutHeader*>(
        At(ng, uint64_t(kLOADOUT_TABLE_OFF) + (uint64_t(slot.v) << kLOADOUT_ENTRY_SHIFT)));
}

ItemWrite WriteRecordedItems(LoadoutInstance& inst, const Symbol64 slots[20], uint64_t count) {
    ItemWrite w;
    if (!inst.items) return w;
    for (uint64_t i = 0; i < count; ++i) {
        // ★ THE NO-MODEL SENTINEL IS NEVER WRITTEN.
        // ~0ull is what the capture stores for a position it did not
        // record, and it is the engine's own "no model" id -- it appears in the engine's own logs
        // only ever as the FROM of a model swap.  We do not hand the catalogue a value it cannot
        // resolve: the per-item apply sub_140D0D170 would log "[NETGAME] Unknown item" and return,
        // and a read-back would score the position 20/20 against a row we never recorded.
        // (The 0xC0000005 in sub_1403C4470 belongs to the POSE path -- that function is
        // XFORM_GET_TRANSFORM_PC, facts_generated.h:1017 -- and pawn_provision.h:757-760 carries
        // its real cause, sub_140D89DD0 on an unbound entrant.  Not this write.)
        // The position is CLEARED rather than left alone: what it would otherwise hold is the last
        // thing written there, and on a slot a previous load dressed that is another player's item.
        // Zero is the engine's own empty -- VacateSlot writes twenty of them -- so the two paths
        // that can leave a position undressed leave it holding the same value.
#ifndef NAMES3_MUTATE_SENTINEL_WRITTEN
        if (slots[i] == kUnrecordedItemId) {
            // A position already at the engine's empty is left alone, so a re-apply over an
            // instance this function already cleared stores nothing.
            if (inst.items[i].itemId == kNoItemId) {
                ++w.alreadyEmpty;
            } else {
                inst.items[i].itemId = kNoItemId;
                ++w.cleared;
            }
            continue;
        }
#endif
        inst.items[i].itemId = slots[i];
        ++w.recorded;
    }
    return w;
}

uint64_t LiveLoadoutInstances(void* ng, UserSlot slot) {
    const LoadoutHeader* hdr = LiveLoadout(ng, slot);
    if (!hdr || !Readable(hdr, sizeof(LoadoutHeader))) return kLoadoutInstancesUnreadable;
    return hdr->instanceCount;
}

LoadoutInstance* FindInstance(LoadoutHeader* hdr, Symbol64 name) {
    if (!hdr || !hdr->instances) return nullptr;
    LoadoutInstance* v = static_cast<LoadoutInstance*>(hdr->instances);
    // instanceCount is engine memory, so it bounds nothing until it is clamped.
    const uint64_t n = hdr->instanceCount > kLoadoutFormatMax ? kLoadoutFormatMax
                                                             : hdr->instanceCount;
    for (uint64_t i = 0; i < n; ++i)
        if (v[i].name == name) return &v[i];
    return nullptr;
}

bool AlignPublishedInstanceName(void* container, Symbol64 engineName) {
    if (!container || engineName == 0) return false;
    LoadoutHeader* const hdr = static_cast<LoadoutHeader*>(container);
    if (!Readable(hdr, sizeof(LoadoutHeader)) || !hdr->instances) return false;
    const uint64_t n = hdr->instanceCount > kLoadoutFormatMax ? kLoadoutFormatMax
                                                             : hdr->instanceCount;
    if (n == 0) return false;
    LoadoutInstance* const v = static_cast<LoadoutInstance*>(hdr->instances);
    if (!Readable(v, size_t(n) * sizeof(LoadoutInstance))) return false;

    LoadoutInstance* named = nullptr;      // an instance already carrying the engine-selected name
    LoadoutInstance* populated = nullptr;  // the sole instance carrying items, if there is exactly one
    uint32_t populatedCount = 0;
    for (uint64_t i = 0; i < n; ++i) {
        if (v[i].name == engineName) named = &v[i];
        if (v[i].itemCount != 0) { ++populatedCount; populated = &v[i]; }
    }
    // Already aligned: the engine-selected instance holds items.
    if (named && named->itemCount != 0) return true;
    // Exactly one populated instance under a different name, and the engine-selected name is not
    // already taken by an (empty) instance -- two instances of one name would let the engine's
    // first-match walk land on the empty one.  Alias the populated instance onto the engine's name.
    if (!named && populatedCount == 1 && populated != nullptr) {
        populated->name = engineName;
        return true;
    }
    return false;
}

bool BuildRefFromGuidCopy(FnActorComponentRefBuild build, FnActorGuidCanon canon,
                          void* game, const void* liveGuid, ActorComponentRef& out) {
    memset(&out, 0, sizeof out);
    if (!build || !liveGuid) return false;
    // A COPY is mandatory: ENGINE.ACTOR_GUID_CANON_VA (sub_140363610) canonicalises the guid its
    // argument points at and writes the result back THROUGH that pointer, so canonicalising the
    // engine's own live user-record field would rewrite the engine's copy of the guid.  Both
    // engine build sites (sub_140155900, sub_140154C00) copy the 16 bytes to a stack local first.
    uint8_t guid[16];
    memcpy(guid, liveGuid, sizeof guid);
#ifndef W2P1_MUTATE_SKIP_GUID_REWRITE
    if (canon) canon(game, guid);
#else
    (void)canon;
#endif
    build(&out, game, guid, -1);
    return out.componentSystem != nullptr;
}

bool ResolveComponent(void* ng, UserIndex index, ActorComponentRef& out) {
    memset(&out, 0, sizeof out);
    if (!ng) return false;
    auto build = Entry<FnActorComponentRef>(Site::ActorComponentRef);
    if (!build) return false;
    // kNETGAME_GAMESPACE_OFF is a misnomer kept for continuity: +0x8 is CR15NetGame's
    // back-pointer to the CR15Game singleton, which is what this call wants for argument 2.
    // The CncaGameSpace is a different member, NETGAME.CS_SPACE_OFF (+0x2B08).
    void* game = Rd<void*>(ng, kNETGAME_GAMESPACE_OFF);
    if (!game) return false;
    const void* guid = At(ng, uint64_t(kUSER_ACTOR_GUID_OFF) + uint64_t(facts::USER_STRIDE) * index.v);
    if (!Readable(guid, 16)) return false;
    auto canon = Entry<FnActorGuidCanon>(Site::ActorGuidCanon);
    return BuildRefFromGuidCopy(build, canon, game, guid, out);
}

bool ReadApplyQueueState(const void* compCs, LoadoutApplyGates& out) {
    out.queueAsked = false;
    if (!compCs) return false;
#ifndef W2P1_MUTATE_CENSUS_INLINE_UNREAD
    // *(cs + APPLY_QUEUE_SUBSYS_OFF) is the queue IDENT.APPLY_LOADOUT_ITEMS branches on.  Every
    // hop is behind a live-page probe: an entrant whose component system is half torn down must
    // read as `unread`, never as a queue that happened to hold zero.
    const uint8_t* sp = static_cast<const uint8_t*>(compCs) + kAPPLY_QUEUE_SUBSYS_OFF;
    if (!Readable(sp, sizeof(void*))) return false;
    const void* subsys = Rd<const void*>(compCs, kAPPLY_QUEUE_SUBSYS_OFF);
    if (!subsys) return false;
    const uint8_t* base = static_cast<const uint8_t*>(subsys);
    if (!Readable(base + kDEFERRED_QUEUE_INLINE_FLAG_OFF, sizeof(uint32_t))) return false;
    if (!Readable(base + kDEFERRED_QUEUE_HEAD_OFF, sizeof(int64_t)))         return false;
    if (!Readable(base + kDEFERRED_QUEUE_CAP_OFF, sizeof(int64_t)))          return false;
    out.inlineApply = Rd<uint32_t>(subsys, kDEFERRED_QUEUE_INLINE_FLAG_OFF);
    // The head is a BYTE offset the engine bumps by ENTRY_STRIDE per parked request; the census
    // wants the request count, so divide here where the stride is a registered fact.
    const int64_t headBytes = Rd<int64_t>(subsys, kDEFERRED_QUEUE_HEAD_OFF);
    const int64_t capBytes  = Rd<int64_t>(subsys, kDEFERRED_QUEUE_CAP_OFF);
    out.queueDepth  = headBytes > 0 ? headBytes / int64_t(kDEFERRED_QUEUE_ENTRY_STRIDE) : 0;
    out.queueCap    = capBytes  > 0 ? capBytes  / int64_t(kDEFERRED_QUEUE_ENTRY_STRIDE) : 0;
    out.queueAsked  = true;
#else
    (void)out;
#endif
    return out.queueAsked;
}

LoadoutApplyGate LoadoutVerdictFromPairs(uint64_t pairs) {
#ifdef W2P1_MUTATE_BARE_PARSE_KEPT
    // The defect: gate (d) is not asked at all, so an instance the bare parse left with zero item
    // pairs reports Ready and the enable goes out over it.
    (void)pairs;
    return LoadoutApplyGate::Ready;
#else
    return pairs == 0 ? LoadoutApplyGate::InstanceEmpty : LoadoutApplyGate::Ready;
#endif
}

const char* LoadoutApplyGateName(LoadoutApplyGate g) {
    switch (g) {
        case LoadoutApplyGate::Ready:           return "Ready";
        case LoadoutApplyGate::InstanceEmpty:   return "InstanceEmpty";
        case LoadoutApplyGate::ActorUnresolved: return "ActorUnresolved";
        case LoadoutApplyGate::CsPointerNull:   return "CsPointerNull";
        case LoadoutApplyGate::CompKeyInvalid:  return "CompKeyInvalid";
        case LoadoutApplyGate::Unasked:         break;
    }
    return "Unasked";
}

// IDENT.ACTOR_REF_IS_VALID_VA(ref) -- sub_140128B20, the whole three-gate predicate
// IDENT.APPLY_LOADOUT_ITEMS hand-inlines at 0x140154FEA / 0x140154FF3 / 0x140154FFB.  Gates (a)
// and (b) are plain field reads of the ref, so they are read here and named one by one; this
// call is issued only once both have answered yes, which makes a false from it gate (c) alone.
using FnActorRefIsValid = uint8_t (*)(const void*);

LoadoutApplyGate ReadLoadoutApplyGates(void* ng, UserSlot slot, UserIndex index,
                                       uint64_t instanceName, LoadoutApplyGates& out) {
    out = LoadoutApplyGates{};
#ifdef W2P1_MUTATE_GATES_ALWAYS_READY
    // The defect: the whole reader is short-circuited to Ready -- no instance count, no pair count,
    // no actor resolve, no IsValid.  The enable then goes out over an apply nobody read the gates of.
    out.verdict = LoadoutApplyGate::Ready;
    return out.verdict;
#endif
    // No session, or an instance name that did not resolve in vivo.  Neither is a gate saying
    // no, and reporting one as a refusal would blame the entrant for an unasked question.
    if (!ng || instanceName == 0) return out.verdict;

    // EVERY READING FIRST, THEN THE VERDICT IN THE ENGINE'S OWN ORDER.  A census that printed
    // only the fields the first failing gate happened to reach could not say whether the other
    // three would have passed, which is the difference between one fix and four.
    LoadoutHeader* const hdr = LiveLoadout(ng, slot);
    if (!hdr || !Readable(hdr, sizeof(LoadoutHeader))) return out.verdict;
    out.instances = hdr->instanceCount;

    // The instance is found the way IDENT.APPLY_LOADOUT_ITEMS finds it -- by NAME, over the live
    // vector -- and never by taking entry 0 on faith: the engine walks past every instance whose
    // +0 symbol is not the one it was handed, and applies whichever one it lands on.
    const LoadoutInstance* const inst = FindInstance(hdr, instanceName);
    if (!inst || !Readable(inst, sizeof(LoadoutInstance))) return out.verdict;
    out.pairs = inst->itemCount;   // IDENT.INSTANCE_ITEM_COUNT_OFF: `cmp [r14+38h], r12`

    // ResolveComponent answers gate (b) only.  Gate (a) is the ref's OWN actor field, which the
    // builder zeroes on exactly the arm where the guid resolved nothing -- without reading it,
    // an unresolved actor and an actor with no customisation component share one word.
    // AN UNBOUND BUILDER IS NOT AN UNRESOLVED ACTOR.  ResolveComponent answers false for both,
    // and reporting the first as ActorUnresolved would hold every entrant in a build whose
    // IDENT.ACTOR_COMPONENT_REF simply did not resolve -- a gate nobody could ask.
    if (!Entry<FnActorComponentRef>(Site::ActorComponentRef)) return out.verdict;
    ActorComponentRef ref;
    const bool built = ResolveComponent(ng, index, ref);
    memcpy(&out.actor, ref.opaque, sizeof(out.actor));
    out.compCs  = ref.componentSystem;
    out.compKey = ref.handle;

    // OBSERVE-ONLY: whichever verdict follows, record whether the engine's own apply for this
    // component system would run inline or park on the deferred queue.  Reads only; drives nothing.
    ReadApplyQueueState(out.compCs, out);

    if (!out.actor)            { out.verdict = LoadoutApplyGate::ActorUnresolved; return out.verdict; }
    if (!built || !out.compCs) { out.verdict = LoadoutApplyGate::CsPointerNull;   return out.verdict; }

    // (c) is a virtual on the component system, so it is asked through the engine's own
    // prologue-guarded predicate rather than by dispatching a vtable slot of our own choosing.
    // Unbound leaves the verdict Unasked: a gate nobody could ask never answered no.
    const auto isValid = Entry<FnActorRefIsValid>(Site::ActorRefIsValid);
    if (!isValid) return out.verdict;
    out.compValidAsked = true;
    out.compValid = isValid(&ref) != 0;
    if (!out.compValid) { out.verdict = LoadoutApplyGate::CompKeyInvalid; return out.verdict; }

    // (d) LAST, in the engine's order: it is tested after the three actor-side gates, and a
    // pass here is the only state in which the engine's item loop runs at all.
    out.verdict = LoadoutVerdictFromPairs(out.pairs);
    return out.verdict;
}

CustSlotEntry* CustSlots(const ActorComponentRef& ref, uint64_t& countOut) {
    countOut = 0;
    if (!ref.componentSystem) return nullptr;
    const uint16_t* table = Rd<const uint16_t*>(ref.componentSystem, kCUST_INDEX_TABLE_OFF);
    uint8_t* records = Rd<uint8_t*>(ref.componentSystem, kCUST_RECORDS_OFF);
    if (!table || !records) return nullptr;
    // The index table is stepped 4 bytes per handle and only its low u16 is read.
    // The table steps 4 bytes per handle and only the low u16 of each element is read,
    // so in u16 units that is two steps: movzx edx, word ptr [rax+r8*4].
    const uint16_t recordIndex = table[uint64_t(ref.handle) * 2];
    uint8_t* record = records + uint64_t(kCUST_RECORD_STRIDE) * recordIndex;
    countOut = Rd<uint64_t>(record, kCUST_SLOT_COUNT_OFF);
    CustSlotEntry* slots = Rd<CustSlotEntry*>(record, kCUST_SLOTS_OFF);
    if (!slots) countOut = 0;
    return slots;
}

uint8_t TeamOf(void* ng, UserIndex index) {
    if (!ng) return 0xFF;
    const uint16_t word = Rd<uint16_t>(ng, UserOff(index.v));
    return static_cast<uint8_t>((word >> kUSER_TEAM_SHIFT) & kUSER_TEAM_MASK);
}

uint8_t TeamSlotOf(void* ng, UserIndex index) {
    if (!ng) return 0xFF;
    const uint16_t word = Rd<uint16_t>(ng, UserOff(index.v));
    return static_cast<uint8_t>((word >> kUSER_TEAMSLOT_SHIFT) & kUSER_TEAMSLOT_MASK);
}

namespace {
// L2-RUN-7 F3.  The capture roster's change signal asks EntrantCount on EVERY tick, and this
// function used to answer with two `replay::Readable` calls -- VirtualQuery, which clock.cpp:128
// says is asked "once per take, never on the tick path".  In the field it measured ~400 us apiece
// under the game's own address-space churn: 1.2 ms of every tick, and the cost meter killed every
// capture at 50 s.  The header fields and the user array are ONE span from ONE base (the netGame),
// so one proof line holds both: the header prove sizes the array prove, and once the array is
// proved the header prove is a subset of it and hits.  Warm cost is zero syscalls per tick.  A
// refusal is never cached (entity.h) and SpanProofMaxCachedReads bounds how long a success is
// honoured, so a netGame freed at an unchanged base is re-probed rather than latched.
entity::SpanProof g_userSpan;

// Both count fields, in one span from the netGame base -- read as two facts at two offsets, the
// way EntrantCount's band check needs them.
constexpr uint32_t kNetGameHeaderBytes =
    (kENTRANT_COUNT_OFF > kNONREPLAY_COUNT_OFF ? kENTRANT_COUNT_OFF : kNONREPLAY_COUNT_OFF) +
    uint32_t(sizeof(uint16_t));
}  // namespace

bool UserTableBound(void* ng, uint16_t& out) {
    if (!ng) return false;
    if (!g_userSpan.Prove(ng, kNetGameHeaderBytes)) return false;
    const uint16_t count = Rd<uint16_t>(ng, kENTRANT_COUNT_OFF);
    // Zero entrants is an empty roster, not an unreadable one, and a zero-length span is nothing
    // asked -- so the array is proved only when there is an array.
    if (count && !g_userSpan.Prove(ng, size_t(facts::USER_ARRAY_OFF) +
                                       size_t(facts::USER_STRIDE) * count)) {
        return false;
    }
    out = count;
    return true;
}

namespace {
// One place that tolerates a null `why`, so no caller below has to branch on it.
inline bool Why(Refusal* out, Refusal r) {
    if (out) *out = r;
    return r == Refusal::None;
}
}  // namespace

bool EntrantCount(void* ng, uint32_t& out, Refusal* why) {
    uint32_t ignored = 0;
    return EntrantCount(ng, out, ignored, why);
}

bool EntrantCount(void* ng, uint32_t& out, uint32_t& nonReplay, Refusal* why) {
    if (!ng) return Why(why, Refusal::NoNetGame);

    // UserTableBound is the derivation: it reads the same IDENT.ENTRANT_COUNT_OFF and span-checks
    // the user array the count claims.  Re-reading the field here would be a second opinion on a
    // number that already has one, and the two would drift.
    uint16_t count = 0;
    if (!UserTableBound(ng, count)) return Why(why, Refusal::RosterUnreadable);

    // The band is its own fact at its own offset -- neither count may be derived from the other --
    // but it needs no second probe: kNetGameHeaderBytes is sized from BOTH offsets, and
    // UserTableBound proved exactly that span from this same base a few instructions ago.  The
    // static_assert is the mechanism; a comment could not keep the two in step.
    static_assert(kNONREPLAY_COUNT_OFF + sizeof(uint16_t) <= kNetGameHeaderBytes,
                  "the non-replay count must lie inside the span UserTableBound proves");
    const uint16_t band = Rd<uint16_t>(ng, kNONREPLAY_COUNT_OFF);

    // The band is [nonReplay, count).  A nonReplay ABOVE the count is not a roster this code can
    // describe -- InReplayBand would call the band empty and a scan would treat replayed entrants
    // as normal ones.  Refused rather than clamped: a clamp here invents a roster shape.
    if (band > count) return Why(why, Refusal::RosterUnreadable);

    out = count;
    nonReplay = band;
    return Why(why, Refusal::None);
}

bool LoadoutNumbers(void* ng, UserSlot slot, uint16_t& number, uint16_t& numberBody, Refusal* why) {
    if (!ng) return Why(why, Refusal::NoNetGame);
    if (slot.v >= facts::ENTITY_USER_SLOT_TABLE_MAX) return Why(why, Refusal::SlotOutOfRange);

    LoadoutHeader* hdr = LiveLoadout(ng, slot);
    if (!hdr) return Why(why, Refusal::SlotOutOfRange);
    if (!Readable(hdr, sizeof(LoadoutHeader))) return Why(why, Refusal::NoLoadoutInstance);

    number = hdr->number;
    numberBody = hdr->numberBody;
    return Why(why, Refusal::None);
}

uint64_t PairsFromSlots(const CustSlotEntry* slots, uint64_t count, LoadoutPair out[20],
                        AppliedSlotState* state) {
    if (!out) return 0;
    memset(out, 0, sizeof(LoadoutPair) * 20);
    if (state) memset(state, 0, sizeof(AppliedSlotState) * 20);
    if (!slots) return 0;
    const uint64_t take = count < 20 ? count : 20;
    for (uint64_t i = 0; i < take; ++i) {
#ifdef W2C2_MUTATE_DROP_KEY
        out[i].slotKey = slots[i].item;
#else
        out[i].slotKey = slots[i].key;
#endif
        out[i].itemId = slots[i].item;
        if (state) {
            state[i].pendingItem = slots[i].pendingItem;
            state[i].flags = slots[i].flags;
            state[i].alignmentReq = slots[i].alignmentReq;
        }
    }
    return take;
}

namespace {
AppliedSlotsWalkFn g_appliedWalkForTest = nullptr;

// ONE read: resolve the component, find its entries, walk them.  Both of ReadAppliedPairs' reads
// go through here, so the second one repeats the resolve as well as the walk.
bool WalkAppliedOnce(void* ng, UserIndex index, LoadoutPair out[20], AppliedSlotState* state,
                     uint64_t& n, uint64_t* held) {
    if (g_appliedWalkForTest) return g_appliedWalkForTest(ng, index, out, state, n, held);
    ActorComponentRef ref;
    if (!ResolveComponent(ng, index, ref)) return false;
    uint64_t count = 0;
    CustSlotEntry* slots = CustSlots(ref, count);
    if (!slots) return false;
    if (held) *held = count;   // the UNCLAMPED count: `n` below is min(count, 20)
    n = PairsFromSlots(slots, count, out, state);
    return true;
}
}  // namespace

void AppliedSlotsInjectWalkForTest(AppliedSlotsWalkFn fn) { g_appliedWalkForTest = fn; }

bool ReadAppliedPairs(void* ng, UserIndex index, LoadoutPair out[20], uint64_t& n,
                      uint64_t* held, Refusal* why, AppliedSlotState* state) {
    if (out) memset(out, 0, sizeof(LoadoutPair) * 20);
    if (state) memset(state, 0, sizeof(AppliedSlotState) * 20);
    if (!out) return Why(why, Refusal::NoActorComponent);
    if (!ng) return Why(why, Refusal::NoNetGame);
    if (!g_appliedWalkForTest && !BuildMatches()) return Why(why, Refusal::BuildMismatch);

    if (!WalkAppliedOnce(ng, index, out, state, n, held))
        return Why(why, Refusal::NoActorComponent);
    if (!state) return Why(why, Refusal::None);

    // The SECOND read.  A single walk of live engine memory cannot say whether the values it
    // returned belong to one moment: the apply this read-back follows can still be running out of
    // a queued transition, and an attribution stitched from two moments would name an arm nobody
    // took.  Resolve and walk again, and compare field by field.
    LoadoutPair again[20] = {};
    AppliedSlotState againState[20] = {};
    uint64_t n2 = 0;
    uint64_t held2 = 0;
#if defined(W2C2_MUTATE_SINGLE_SLOT_READ) || defined(W2P1_MUTATE_SECOND_READ_SKIPPED)
    // The defect: the second read is the first one's answer, copied.
    n2 = n;
    memcpy(again, out, sizeof again);
    memcpy(againState, state, sizeof againState);
#else
    if (!WalkAppliedOnce(ng, index, again, againState, n2, &held2))
        return Why(why, Refusal::NoActorComponent);
#endif
    bool agreed = n2 == n && (held == nullptr || held2 == *held);
    for (uint64_t i = 0; agreed && i < n && i < 20; ++i) {
        agreed = again[i].slotKey == out[i].slotKey && again[i].itemId == out[i].itemId &&
                 againState[i].pendingItem == state[i].pendingItem &&
                 againState[i].flags == state[i].flags &&
                 againState[i].alignmentReq == state[i].alignmentReq;
    }
    if (!agreed) {
        // Same discipline as the entry: a caller that ignores the bool must not find a readable
        // half-answer here.  `n` goes back to 0 because the contract writes it on success only.
        memset(out, 0, sizeof(LoadoutPair) * 20);
        memset(state, 0, sizeof(AppliedSlotState) * 20);
        n = 0;
        return Why(why, Refusal::AppliedSlotsUnstable);
    }
    return Why(why, Refusal::None);
}

namespace {
// One proof line per slot for the header, one for the instance vector.  Two lines because they are
// two bases: the header is netGame-relative and the vector is a pointer the header hands out, and
// one line asked about both alternately would spend its cache on every call.  Proved once per bind
// epoch (entity.h), so a warm census pass costs no syscall at all -- which is what keeps the whole
// pass inside its 30 us budget.
entity::SpanProof g_loadoutHdr[facts::ENTITY_USER_SLOT_TABLE_MAX];
entity::SpanProof g_loadoutVec[facts::ENTITY_USER_SLOT_TABLE_MAX];
}  // namespace

size_t LoadoutHeaderOffset(UserSlot slot) {
    return size_t(kLOADOUT_TABLE_OFF) + (size_t(slot.v) << kLOADOUT_ENTRY_SHIFT);
}

bool CensusLoadout(void* ng, UserIndex entrant, UserSlot slot, LoadoutCensus& out, Refusal* why) {
    out = LoadoutCensus{};
    if (!ng) return Why(why, Refusal::NoNetGame);
    if (slot.v >= facts::ENTITY_USER_SLOT_TABLE_MAX ||
        entrant.v >= facts::ENTITY_USER_SLOT_TABLE_MAX)
        return Why(why, Refusal::SlotOutOfRange);

#ifndef W2C_MUTATE_NO_GATE
    // THE GATE, AND IT IS FIRST.  Not one loadout byte is read for an entrant whose document is
    // absent or is still the json db's cached one -- that entrant's loadout container describes
    // whoever the cache last held, and reading it is how a half-admitted entrant gets censused as
    // a dressed one.  L3 #19 F2 is this state, 58 refused key writes long.
    const void* doc = ProfileJson(ng, entrant);
    if (!doc || !ProfileDocWritable(doc)) return Why(why, Refusal::ProfileNotAttached);
#endif

    LoadoutHeader* hdr = LiveLoadout(ng, slot);
    if (!hdr) return Why(why, Refusal::SlotOutOfRange);
    if (!g_loadoutHdr[slot.v].Prove(hdr, sizeof(LoadoutHeader)))
        return Why(why, Refusal::LoadoutsUnreadable);

    // Every field through the guarded load, including the two the proof just covered: the proof is
    // honoured for a bounded number of reads, not forever, and TryLoad is what stands between a
    // stale proof and the game's thread.
    uint64_t claimed = 0;
    void*    vec     = nullptr;
    if (!entity::TryLoad(At(hdr, kLOADOUT_COUNT_OFF), claimed) ||
        !entity::TryLoad(hdr, vec) ||
        !entity::TryLoad(At(hdr, kLOADOUT_NUMBER_OFF), out.number) ||
        !entity::TryLoad(At(hdr, kLOADOUT_NUMBER_BODY_OFF), out.numberBody))
        return Why(why, Refusal::LoadoutsUnreadable);

    out.claimedInstances = claimed;
    out.clamped          = claimed > kLoadoutFormatMax;
#ifdef W2C_MUTATE_NO_BOUND
    const uint32_t walk = uint32_t(claimed);
#else
    const uint32_t walk = out.clamped ? kLoadoutFormatMax : uint32_t(claimed);
#endif
    if (walk == 0 || vec == nullptr) return Why(why, Refusal::None);

    // The vector is proved at the CLAMP, never at the claim: a claim of 0xFFFF would ask for a
    // 4 MiB span and a claim of 2^64 would overflow the multiply before VirtualQuery ever saw it.
    if (!g_loadoutVec[slot.v].Prove(vec, size_t(kLoadoutFormatMax) * sizeof(LoadoutInstance)))
        return Why(why, Refusal::LoadoutsUnreadable);

    LoadoutInstance* v = static_cast<LoadoutInstance*>(vec);
    for (uint32_t i = 0; i < walk; ++i) {
        ++out.steps;
        uint64_t items = 0;
        if (!entity::TryLoad(At(v + i, kINSTANCE_ITEM_COUNT_OFF), items))
            return Why(why, Refusal::LoadoutsUnreadable);
        out.items += items > kLoadoutFormatMax ? kLoadoutFormatMax : uint32_t(items);
        ++out.instances;
    }
    return Why(why, Refusal::None);
}

bool CopyJsonBytes(const void* src, size_t len, void* out, size_t cap, size_t& written,
                   bool& truncated, Refusal* why) {
    // `written` is the length of the DOCUMENT, not the length of the copy, on every path: a caller
    // that was refused for a short buffer sizes its second call from it.
    written = len;
    truncated = false;
    if (!src && len) return Why(why, Refusal::EmptyProfile);
    if (!out && cap) return Why(why, Refusal::ShortBuffer);

    if (len > cap) {
        // Nothing is copied.  A prefix of a JSON document parses as nothing and reaches the demo
        // file looking like a capture, so the short arm hands back the length and no bytes.
        truncated = true;
        return Why(why, Refusal::ShortBuffer);
    }
    if (len) memcpy(out, src, len);
    return Why(why, Refusal::None);
}

namespace {

// The engine's CJson read API, bound once per call out of the verified site table.  The walker in
// entity_json.cpp holds no engine address of its own -- it is pure over this struct -- which is
// what lets the whole emitter be driven by a fake tree on a host with no game.
using FnJsonNodeKind   = uint32_t    (*)(void*, void*);
using FnJsonNodeName   = const char* (*)(void*, void*);
using FnJsonNodeString = const char* (*)(void*, void*, const char*);
using FnJsonNodeInt    = int64_t     (*)(void*, void*, int64_t);
using FnJsonNodeBool   = int         (*)(void*, void*, int);
using FnJsonNextElem   = void*       (*)(void*, void*, void*);
using FnJsonElemCount  = int64_t     (*)(void*, void*);
// IDENT.JSON_FIND_NODE returns a CHILD COUNT at a path, not a node: sub_1405EFA60 ends on the
// same two callees (sub_14135C270 tag 0, sub_14135BCA0 tag 1) as IDENT.JSON_ELEM_COUNT, and
// IDENT.JSON_IS_EMPTY is literally `that == 0`.  Typing it as void* and handing the count to
// the walker as a root node is what refused JsonNodeUnreadable on every live profile.
using FnJsonPathCount  = int64_t     (*)(void*, const char*);
using FnJsonPathElem   = void*       (*)(void*, const char*, void*);

bool ReadableProbe(const void* p, size_t bytes) { return Readable(p, bytes); }

bool BuildCJsonReader(void* json, entity_json::CJsonReader& r) {
    r.json        = json;
    r.kind        = Entry<FnJsonNodeKind>(Site::JsonNodeKind);
    r.name        = Entry<FnJsonNodeName>(Site::JsonNodeName);
    r.stringValue = Entry<FnJsonNodeString>(Site::JsonNodeString);
    r.intValue    = Entry<FnJsonNodeInt>(Site::JsonNodeInt);
    r.boolValue   = Entry<FnJsonNodeBool>(Site::JsonNodeBool);
    r.nextElem    = Entry<FnJsonNextElem>(Site::JsonNextElem);
    r.elemCount   = Entry<FnJsonElemCount>(Site::JsonElemCount);
    r.pathElem    = Entry<FnJsonPathElem>(Site::JsonPathElem);
    r.pathCount   = Entry<FnJsonPathCount>(Site::JsonFindNode);
    r.readable    = &ReadableProbe;
    return r.kind && r.name && r.stringValue && r.intValue && r.boolValue && r.nextElem &&
           r.elemCount && r.pathElem && r.pathCount;
}

// One walker refusal, one identity refusal.  Exhaustive rather than defaulted: a value added to
// the walker later has to be given a word here or this switch stops compiling.
Refusal MapJsonRefusal(entity_json::JsonRefusal r) {
    switch (r) {
        case entity_json::JsonRefusal::None:                return Refusal::None;
        case entity_json::JsonRefusal::NoReader:            return Refusal::BuildMismatch;
        case entity_json::JsonRefusal::NoRoot:              return Refusal::JsonNoRoot;
        case entity_json::JsonRefusal::NodeUnreadable:      return Refusal::JsonNodeUnreadable;
        case entity_json::JsonRefusal::KeyUnreadable:       return Refusal::JsonStringUnreadable;
        case entity_json::JsonRefusal::StringUnreadable:    return Refusal::JsonStringUnreadable;
        case entity_json::JsonRefusal::StringTooLong:       return Refusal::JsonStringUnreadable;
        case entity_json::JsonRefusal::StringNotUtf8:       return Refusal::JsonStringNotUtf8;
        case entity_json::JsonRefusal::DepthLimit:          return Refusal::JsonDepthLimit;
        case entity_json::JsonRefusal::NodeLimit:           return Refusal::JsonNodeLimit;
        case entity_json::JsonRefusal::Cycle:               return Refusal::JsonCycle;
        case entity_json::JsonRefusal::ArrayNotWalkable:    return Refusal::JsonArrayNotWalkable;
        case entity_json::JsonRefusal::NumberNotExact:      return Refusal::JsonNumberNotExact;
        case entity_json::JsonRefusal::UnknownNodeKind:     return Refusal::JsonUnknownKind;
        case entity_json::JsonRefusal::ChildCountDisagrees: return Refusal::JsonChildCountDisagrees;
        case entity_json::JsonRefusal::TreeChanged:         return Refusal::JsonTreeChanged;
        case entity_json::JsonRefusal::NoSuchKey:           return Refusal::JsonNoSuchKey;
        case entity_json::JsonRefusal::ShortBuffer:         return Refusal::ShortBuffer;
    }
    return Refusal::JsonUnknownKind;
}

}  // namespace

namespace {

// THE ONE WALK BOTH STORAGES GO THROUGH.  `json` is a document object -- the user table holds a
// pointer to one, the lobby member record embeds one at +144 -- and from here on the two sources
// are the same code, so a rule added for either can never be missing from the other.
//
// The document root is a DESCRIPTOR, not a node -- there is no root node pointer on this build --
// so the walk enters through IDENT.JSON_PATH_ELEM, which is how CR15NetGame's own /session writer
// (0x1401552D0) reaches the children of a path.
bool EmitProfileDocument(void* json, const char* path, void* out, size_t cap, size_t& written,
                         bool& truncated, bool* lossyNumber, Refusal* why) {
    if (lossyNumber) *lossyNumber = false;
    entity_json::CJsonReader r;
    if (!BuildCJsonReader(json, r)) return Why(why, Refusal::BuildMismatch);
    if (r.pathCount(json, "") == 0) return Why(why, Refusal::JsonNoRoot);

    entity_json::JsonRefusal jr = entity_json::JsonRefusal::None;
    const bool ok = entity_json::EmitDocumentJson(r, path, out, cap, written, truncated, &jr);
    if (lossyNumber) *lossyNumber = entity_json::LastEmitDroppedNumber();
    if (ok) return Why(why, Refusal::None);
    return Why(why, MapJsonRefusal(jr));
}

}  // namespace

bool ProfileJsonSubtree(void* ng, UserIndex index, const char* path, void* out, size_t cap,
                        size_t& written, bool& truncated, Refusal* why, bool* lossyNumber) {
    written = 0;
    truncated = false;
    if (lossyNumber) *lossyNumber = false;
    if (!ng) return Why(why, Refusal::NoNetGame);

    uint16_t count = 0;
    if (!UserTableBound(ng, count)) return Why(why, Refusal::RosterUnreadable);
    if (index.v >= count) return Why(why, Refusal::NoSuchEntrant);

    // ProfileJson() indexes the same user array UserTableBound just span-checked, so the read of
    // the pointer itself is covered; the pointer's TARGET is the engine's own allocation and gets
    // its own check before ProfileJsonUsable dereferences it through IsEmpty.
    void* json = ProfileJson(ng, index);
    if (!json || !Readable(json, sizeof(void*))) return Why(why, Refusal::EmptyProfile);
    if (!ProfileJsonUsable(ng, index)) return Why(why, Refusal::EmptyProfile);

    // ProfileJsonUsable has already asked IDENT.JSON_IS_EMPTY, which is the same count the walk
    // cross-checks against.
    return EmitProfileDocument(json, path, out, cap, written, truncated, lossyNumber, why);
}

// The whole document IS the subtree at the empty path, so this is the same walk and not a
// second one: a rule added to one of them can never be missing from the other.
bool ProfileJsonBytes(void* ng, UserIndex index, void* out, size_t cap, size_t& written,
                      bool& truncated, Refusal* why, bool* lossyNumber) {
    return ProfileJsonSubtree(ng, index, nullptr, out, cap, written, truncated, why, lossyNumber);
}

// A Json* refusal names a WORD; these three name the place it was raised, so a real unreadable
// node can still be found instead of being indistinguishable from the bug that used to raise the
// same word at a fabricated address.
const char* LastProfileJsonRefusalPath()  { return entity_json::LastRefusalPath(); }
const void* LastProfileJsonRefusalAt()    { return entity_json::LastRefusalAt(); }
size_t      LastProfileJsonRefusalBytes() { return entity_json::LastRefusalExtent(); }

uint16_t TeamAlignment(void* ng, uint8_t teamId) {
    if (!ng || teamId >= kALIGN_TEAM_LIMIT) return kNoAlignment;
    return Rd<uint16_t>(ng, uint64_t(facts::NETGAME_ALIGNMENT_BASE_OFF) + 2u * teamId);
}

bool CallApplyLoadoutToActor(void* ng, uint32_t netUserId, Symbol64 instanceName,
                             uint32_t flags, const void* actorGuid16, uint32_t skipGoalFx) {
    auto apply = Entry<FnApplyLoadoutToActor>(Site::ApplyLoadoutToActor);
    if (!ng || !actorGuid16 || !apply) return false;
    apply(ng, netUserId, instanceName, flags, actorGuid16, skipGoalFx);
    return true;
}

uint16_t CallCustomisationAlignment(void* cs, uint16_t handle) {
    auto alignmentOf = Entry<FnAlignment>(Site::CustomisationAlignment);
    if (!cs || !alignmentOf) return kNoAlignment;
    return static_cast<uint16_t>(alignmentOf(cs, handle));
}

bool EntityAlignment(void* ng, UserIndex index, uint16_t& out, Refusal* why) {
    out = kNoAlignment;
    if (!ng) return Why(why, Refusal::NoNetGame);
    if (!BuildMatches()) return Why(why, Refusal::BuildMismatch);
    // Asked before the call, because CallCustomisationAlignment answers kNoAlignment both for an
    // entrant the engine gives no alignment AND for an entry point that did not bind, and those
    // are the two answers this reader exists to keep apart.
    if (!Entry<FnAlignment>(Site::CustomisationAlignment)) return Why(why, Refusal::BuildMismatch);
    ActorComponentRef ref;
    if (!ResolveComponent(ng, index, ref)) return Why(why, Refusal::NoActorComponent);
    out = CallCustomisationAlignment(ref.componentSystem, uint16_t(ref.handle));
    return Why(why, Refusal::None);
}

bool CallOverrideAlignment(void* cs, uint32_t handle, uint16_t alignment) {
    auto over = Entry<FnOverrideAlignment>(Site::OverrideAlignment);
    if (!cs || !over) return false;
    over(cs, handle, alignment);
    return true;
}

bool CallApplyAlignmentChange(void* cs, uint32_t handle) {
    auto change = Entry<FnApplyAlignment>(Site::ApplyAlignmentChange);
    if (!cs || !change) return false;
    change(cs, handle);
    return true;
}

bool CallApplyAlignmentAllUsers(void* ng) {
    auto all = Entry<FnApplyAllUsers>(Site::ApplyAlignmentAllUsers);
    if (!ng || !all) return false;
    all(ng);
    return true;
}

bool CallJsonSetString(void* json, const char* key, const char* value) {
    auto set = Entry<FnJsonSetString>(Site::JsonSetString);
    if (!json || !key || !value || !set) return false;
    set(json, key, value);
    return true;
}

bool CallJsonSetInt(void* json, const char* key, int64_t value) {
    auto set = Entry<FnJsonSetInt>(Site::JsonSetInt);
    if (!json || !key || !set) return false;
    set(json, key, value);
    return true;
}

bool ReadProfileString(void* ng, UserIndex index, const char* key, char* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    auto isEmpty = Entry<FnJsonIsEmpty>(Site::JsonIsEmpty);
    auto get = Entry<FnJsonGetString>(Site::JsonGetString);
    if (!ng || !key || !isEmpty || !get) return false;
    void* doc = ProfileJson(ng, index);
    if (!doc || isEmpty(doc)) return false;
    // The fourth argument is 0 and the third an empty fallback, exactly as UserDisplayName
    // calls it at 0x1401C991B.  The engine owns the returned string; it is copied out here.
    const char* v = get(doc, key, "", 0);
    if (!v) return false;
    size_t n = 0;
    while (v[n] && n + 1 < cap) { out[n] = v[n]; ++n; }
    out[n] = '\0';
    return n != 0;
}

// The engine's own write gate, read where the engine reads it.  Both setters open
// `cmp qword ptr [rcx+8], 0` (48 83 79 08 00, at 0x1405FDB4E and 0x1405FD50E) and write only on
// the zero arm; the other arm logs `json db is cached, read only` and returns.  Both return void,
// which is why L3 #19 could take 58 refusals for a success.
size_t ProfileDocCachedOffset() { return size_t(kJSON_CACHED_OFF); }

size_t ProfileDocFieldOffset(uint16_t index) {
    return size_t(uint64_t(kPROFILE_JSON_OFF) + uint64_t(facts::USER_STRIDE) * uint64_t(index));
}

// The engine's own three calls, bound the way PublishLoadout binds IDENT.LOADOUT_ASSIGN.
// IDENT.JSON_PARSE_TEXT is sub_1405F0BD0(doc, text, len): it trims trailing whitespace, parses
// with sub_14135C700, and returns 0 on success -- any other value came from the engine's own
// `Json Format Error: %s - [%s] line: %llu column: %llu` reporter at 0x1405F0CCE.  The whole
// idiom is the engine's, read off sub_14019EFA0, which builds a 24-byte document on its stack,
// calls the constructor, parses into it, and destroys it when it is done.
using FnJsonDocCtor   = void*   (*)(void*);
using FnJsonParseText = uint32_t(*)(void*, const char*, uint64_t);
// ARGSHAPE.  sub_1405F3080 takes FOUR arguments, not the three the recorded IDA prototype guessed:
// its body is `v10 = sub_1405FCEA0(a2, *a1, a4, a1[1]); ... else if (a4) <report not found>`, so a4
// is both handed to the lookup and gates the engine's own `$ json path: [%s] not found or not an
// integer.` report.  Calling it as a three-argument function left R9 holding whatever the caller
// had there; it is passed explicitly and quietly (0) below.
using FnJsonGetInt    = int64_t (*)(void*, const char*, int64_t, uint32_t);

// A pre-parse structural bound, not the walker's: it exists so a malformed blob cannot make the
// scan below walk a pathological nesting.  entity_json's own JsonRefusal::DepthLimit is what
// speaks for a document the engine did parse.
constexpr size_t kAttachNestingMax = 64;

bool IsJsonSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

AttachResult StoreProfileDocument(void* ng, uint16_t index, void* doc) {
    if (!ng || !doc) return AttachResult::AttachFailed;
    void** field = reinterpret_cast<void**>(At(ng, uint64_t(ProfileDocFieldOffset(index))));
    if (!field || !Readable(field, sizeof(void*))) return AttachResult::AttachFailed;
#ifndef W2P1_MUTATE_ATTACH_IN_PLACE
    *field = doc;   // 0x14019F613: mov [rax+3C0h], rdi.  The displaced document is abandoned,
                    // which is what the engine does with it too.
#endif
    if (*field != doc) return AttachResult::AttachFailed;
    return AttachResult::Ok;
}

// The recording's bytes, checked by US before the engine's parser is handed them.  L3 #20 took the
// process down inside an engine entry point with nothing to quote, and the reason a plugin-side
// gate is worth its cost is that a refusal survives where a fault does not: a blob that cannot be
// a json OBJECT is ProfileParseFailed by name, and no engine code runs at all.
//
// The checks are the ones the engine's own reader makes load-bearing: it walks the text as a C
// string, so an embedded NUL silently truncates the document; and IDENT.JSON_GET_INT can only
// answer about an object, so a root that is not one would attach a document no user-record read
// can use.  Structure is checked, CONTENT is not -- the parser owns that, and this refusing a
// document the engine would have taken is the failure mode to avoid.
bool RecordedProfileJsonAcceptable(const Blob& recordedJson) {
#ifdef W2P1_MUTATE_NO_PROFILE_VALIDATION
    return recordedJson.p != nullptr && recordedJson.n != 0;
#else
    if (!recordedJson.p || recordedJson.n == 0) return false;
    const char* const s = reinterpret_cast<const char*>(recordedJson.p);
    if (!Readable(s, recordedJson.n)) return false;

    size_t i = 0;
    while (i < recordedJson.n && IsJsonSpace(s[i])) ++i;
    if (i == recordedJson.n || s[i] != '{') return false;

    size_t depth = 0;
    size_t lastGlyph = i;
    bool inString = false, escaped = false;
    for (; i < recordedJson.n; ++i) {
        const char c = s[i];
        if (c == '\0') return false;
        if (inString) {
            if (escaped)        escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"')  inString = false;
            continue;
        }
        if (c == '"') { inString = true; lastGlyph = i; continue; }
        if (c == '{' || c == '[') {
            if (++depth > kAttachNestingMax) return false;
        } else if (c == '}' || c == ']') {
            if (depth == 0) return false;
            --depth;
        }
        if (!IsJsonSpace(c)) lastGlyph = i;
    }
    return !inString && depth == 0 && s[lastGlyph] == '}';
#endif
}

uint64_t JsonEntryPointVa(JsonEntryPoint e) {
    switch (e) {
        case JsonEntryPoint::DocCtor:    return kSites[SiteIndex(Site::JsonDocCtor)].va;
        case JsonEntryPoint::ParseText:  return kSites[SiteIndex(Site::JsonParseText)].va;
        case JsonEntryPoint::GetInt:     return kSites[SiteIndex(Site::JsonGetInt)].va;
        case JsonEntryPoint::NodeKind:   return kSites[SiteIndex(Site::JsonNodeKind)].va;
        case JsonEntryPoint::NodeName:   return kSites[SiteIndex(Site::JsonNodeName)].va;
        case JsonEntryPoint::NodeString: return kSites[SiteIndex(Site::JsonNodeString)].va;
        case JsonEntryPoint::NodeInt:    return kSites[SiteIndex(Site::JsonNodeInt)].va;
        case JsonEntryPoint::NodeBool:   return kSites[SiteIndex(Site::JsonNodeBool)].va;
        case JsonEntryPoint::NextElem:   return kSites[SiteIndex(Site::JsonNextElem)].va;
        case JsonEntryPoint::PathElem:   return kSites[SiteIndex(Site::JsonPathElem)].va;
        case JsonEntryPoint::ElemCount:  return kSites[SiteIndex(Site::JsonElemCount)].va;
        case JsonEntryPoint::FindNode:   return kSites[SiteIndex(Site::JsonFindNode)].va;
        case JsonEntryPoint::IsEmpty:    return kSites[SiteIndex(Site::JsonIsEmpty)].va;
        case JsonEntryPoint::Count:      break;
    }
    return 0;
}

AttachResult AttachProfileDocument(void* ng, UserIndex index, const Blob& recordedJson) {
    // Missing session and unusable bytes are two facts: only the second is a PARSE failure, and
    // the parse answer must be reachable without the engine so a test can drive it.
    if (!ng) return AttachResult::AttachFailed;
    if (!RecordedProfileJsonAcceptable(recordedJson)) return AttachResult::ParseFailed;
    const FnJsonDocCtor   ctor  = Entry<FnJsonDocCtor>(Site::JsonDocCtor);
    const FnJsonParseText parse = Entry<FnJsonParseText>(Site::JsonParseText);
    const FnJsonGetInt    get   = Entry<FnJsonGetInt>(Site::JsonGetInt);
    // Could-not-ask, and it is an ATTACH failure rather than a parse one: nothing was read.
    if (ctor == nullptr || parse == nullptr || get == nullptr) return AttachResult::AttachFailed;

    void* doc = ::calloc(1, size_t(kJSON_DOC_SIZE));
    if (doc == nullptr) return AttachResult::AttachFailed;
    ctor(doc);
    if (parse(doc, reinterpret_cast<const char*>(recordedJson.p),
              uint64_t(recordedJson.n)) != 0) {
        ::free(doc);   // untouched by the engine on its error arm: it reports and returns
        return AttachResult::ParseFailed;
    }
    // The engine's own write gate on what we are about to attach.  A document that does not pass
    // it would reproduce L3 #19 with a new pointer instead of the cache's.
    // Both arms below are PRE-STORE: the engine has never seen this pointer, so the document and
    // the tree the parse hung off it are still ours and nothing else will ever free them.  Past the
    // store they are the engine's -- it owns the slot at entrant+0x3C0 and frees the document it
    // replaces -- which is why the success arm frees nothing.  This runs per player per pass at
    // 4 Hz, so a leak here is a leak per second for the length of the file.
    if (!ProfileDocWritable(doc)) {
        ::free(doc);
        return AttachResult::AttachFailed;
    }
    if (StoreProfileDocument(ng, index.v, doc) != AttachResult::Ok) {
        ::free(doc);
        return AttachResult::AttachFailed;
    }
    // The user-record read, through the field, with the engine's own reader.  The return value is
    // the recording's `_version` or the 0 default; what is being proved is the call, not a value.
    (void)get(ProfileJson(ng, index), "_version", 0, 0u);
    return AttachResult::Ok;
}

bool ProfileDocWritable(const void* doc) {
#ifdef W2P1_MUTATE_DOC_ALWAYS_WRITABLE
    return doc != nullptr;
#else
    if (!doc || !Readable(doc, size_t(kJSON_CACHED_OFF) + sizeof(uint64_t))) return false;
    const uint64_t cached =
        *reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(doc) + kJSON_CACHED_OFF);
    return cached == 0;
#endif
}


// ---------------------------------------------------------------------------------------
// The name, both halves.  See the header for why there are two sources and why the check is
// on neither of them but on what UserDisplayName returns.
// ---------------------------------------------------------------------------------------

namespace {

// The vector header holds the count and the base; one proof covers whichever ends further out.
constexpr uint64_t kSlotNameVecNeed =
    facts::IDENT_SLOTNAME_COUNT_OFF + sizeof(uint64_t) >
            facts::IDENT_SLOTNAME_BASE_OFF + sizeof(uint8_t*)
        ? facts::IDENT_SLOTNAME_COUNT_OFF + sizeof(uint64_t)
        : facts::IDENT_SLOTNAME_BASE_OFF + sizeof(uint8_t*);

SlotNameWalkCensus g_slotNameWalk;

}  // namespace

const char* SlotNameRefusalName(SlotNameRefusal r) {
    switch (r) {
        case SlotNameRefusal::None: return "none";
        case SlotNameRefusal::NoNetGame: return "no netGame";
        case SlotNameRefusal::OwnerSlotUnreadable: return "the slot-table pointer is unreadable";
        case SlotNameRefusal::OwnerNull: return "the slot table is null";
        case SlotNameRefusal::VectorUnreadable: return "the name vector's header is unreadable";
        case SlotNameRefusal::PastCount: return "the slot is at or past the vector's own count";
        case SlotNameRefusal::BaseNull: return "the name array is null";
        case SlotNameRefusal::EntryUnreadable: return "the entry's name field is unreadable";
        case SlotNameRefusal::Count: break;
    }
    return "?";
}

const SlotNameWalkCensus& SlotNameWalk() { return g_slotNameWalk; }

void ResetSlotNameWalkForTest() { g_slotNameWalk = SlotNameWalkCensus{}; }

namespace {

void NoteSlotNameRefusal(SlotNameRefusal r) {
    const size_t i = size_t(r);
    if (i >= size_t(SlotNameRefusal::Count)) return;
    ++g_slotNameWalk.refusals[i];
    const uint32_t bit = 1u << i;
    if ((g_slotNameWalk.logged & bit) != 0) return;
    g_slotNameWalk.logged |= bit;
    log::Direct(LogLevel::Warn, "identity",
                "slot-name walk refused (%s): the engine's slot table did not prove out, so the "
                "name and the lobby record read as absent this pass. Said once per reason",
                SlotNameRefusalName(r));
}

// ★ THE 216-BYTE ENTRY'S INLINE NAME BUFFER -- AND EVERY HOP TO IT IS PROVED FIRST.
//
// This walk is on the LIVE 1 Hz capture path (capture.cpp's XPlatformId
// and ProfileJsonBytesFromEntrantRecord), on the game's own frame thread, and it used to be three
// bare memcpys with null checks and nothing else.  An entrant leaving while the engine rebuilds the
// slot table hands it a non-null freed page, and the access violation is then echovr's, mid live
// match, with that match's capture truncated at the last complete chapter.  Every hop is a
// Readable, which is the shape LobbyMemberRecord already used for the record it returns -- and the
// right shape rather than __try, because __except does not catch a stack-cookie or __fastfail.
//
// Readable is a VirtualQuery, so this is three of them per call.  That is affordable HERE and only
// here: the live caller is 1 Hz per entrant and the others run on admission, never per tick.
char* SlotNameField(void* ng, UserSlot slot) {
    if (!ng) {
        NoteSlotNameRefusal(SlotNameRefusal::NoNetGame);
        return nullptr;
    }
    if (!Readable(At(ng, facts::IDENT_SLOTTABLE_OFF), sizeof(void*))) {
        NoteSlotNameRefusal(SlotNameRefusal::OwnerSlotUnreadable);
        return nullptr;
    }
    void* owner = Rd<void*>(ng, facts::IDENT_SLOTTABLE_OFF);
    if (!owner) {
        NoteSlotNameRefusal(SlotNameRefusal::OwnerNull);
        return nullptr;
    }
    uint8_t* vec = At(owner, facts::IDENT_SLOTTABLE_VEC_OFF);
    if (!Readable(vec, size_t(kSlotNameVecNeed))) {
        NoteSlotNameRefusal(SlotNameRefusal::VectorUnreadable);
        return nullptr;
    }
    // The engine's own bound, read from the engine.  IDENT.SLOTNAME_GET returns the empty
    // string at or above it, so writing there would be a write into whatever follows the array.
    const uint64_t count = Rd<uint64_t>(vec, facts::IDENT_SLOTNAME_COUNT_OFF);
    if (slot.v >= count) {
        NoteSlotNameRefusal(SlotNameRefusal::PastCount);
        return nullptr;
    }
    uint8_t* base = Rd<uint8_t*>(vec, facts::IDENT_SLOTNAME_BASE_OFF);
    if (!base) {
        NoteSlotNameRefusal(SlotNameRefusal::BaseNull);
        return nullptr;
    }
    char* field = reinterpret_cast<char*>(base + uint64_t(facts::IDENT_SLOTNAME_STRIDE) * slot.v +
                                          facts::IDENT_SLOTNAME_FIELD_OFF);
    if (!Readable(field, kSlotNameCap)) {
        NoteSlotNameRefusal(SlotNameRefusal::EntryUnreadable);
        return nullptr;
    }
    return field;
}

}  // namespace

const char* SlotNameFieldForTest(void* ng, uint16_t slot) { return SlotNameField(ng, UserSlot(slot)); }

// The two numbers the header states as literals, tied to the one already in the registry: the
// mirror buffer ends exactly where the name buffer begins, and they are the same size.
static_assert(kSlotNameMirror + kSlotNameCap == facts::IDENT_SLOTNAME_FIELD_OFF,
              "IDENT.SLOTNAME_MIRROR_OFF + IDENT.SLOTNAME_FIELD_SIZE must equal "
              "IDENT.SLOTNAME_FIELD_OFF");

Refusal SetSlotName(void* ng, UserSlot slot, const char* name) {
    if (!name) return Refusal::SlotNameTooLong;
    const size_t n = strlen(name);
    if (n + 1 > kSlotNameCap) return Refusal::SlotNameTooLong;   // refused, never truncated
    char* dst = SlotNameField(ng, slot);
    if (!dst) return Refusal::NoSlotNameTable;
    // Byte-for-byte what CNSLobby::SetEntrantDisplayName leaves behind: the whole buffer zeroed,
    // the name copied over the front, and the 0x24-byte MIRROR at +0x3C made equal to it.  The
    // tail matters because the engine zero-fills it too, and the mirror matters because
    // 0x140608BEC reads its first byte raw as a control byte that steers the entrant refresh,
    // and IDENT.SLOTNAME_GET returns +0x3C directly for sel == 0.  A name is set here; ""
    // vacates.
    memset(dst, 0, kSlotNameCap);
    memcpy(dst, name, n);
    memcpy(dst - (facts::IDENT_SLOTNAME_FIELD_OFF - kSlotNameMirror), dst, kSlotNameCap);
    return Refusal::None;
}

bool ResolvedDisplayName(void* ng, UserIndex index, char* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    auto shown = Entry<FnUserDisplayName>(Site::UserDisplayName);
    if (!ng || !shown) return false;
    // The engine owns the returned pointer -- it is a literal, a slot-table interior pointer or
    // the CJson's own storage -- so the bytes are copied out before anything else can move them.
    const char* v = shown(ng, index.v);
    if (!v) return false;
    size_t n = 0;
    while (v[n] && n + 1 < cap) { out[n] = v[n]; ++n; }
    out[n] = '\0';
    return true;   // "" is an answer: it is what the engine rendered, not a failure to ask.
}

// GenerateBotProfile is deliberately NOT bound.  Its third argument flows straight through
// to GenerateBotLoadout's fourth, which dereferences it as a lobby entrant before doing
// anything else (`if (*(_DWORD *)a4 == 1)`), and this packet has not derived where a valid
// one comes from.  Calling it with a null or invented pointer is a fault inside the tick,
// which is the exact defect class the brief names.  ProfileJsonUsable reports the gap instead.
bool CallGenerateBotProfile(void*, UserIndex, void*) { return false; }

// ---------------------------------------------------------------------------------------
// The recorded ServerProfile, driven in one scalar at a time.
//
// The engine's CJson has typed setters we have read (string and integer) taking a
// pipe-separated path -- "stats|arena|Level|val" is how GenerateBotProfile itself writes.  It
// has no verified from-memory PARSE, so a whole document cannot be handed over; instead every
// scalar leaf of the recording is walked and set through the setter the engine already uses.
// Arrays have no verified setter, so they are counted and the first one is named.  That is
// the honest boundary: `unlocks` is an array.
// ---------------------------------------------------------------------------------------
namespace {

constexpr size_t kMaxKey = 256;
constexpr int    kMaxDepth = 16;

struct JsonWalk {
    const char*              p;
    const char*              end;
    const ProfileScalarSink* sink;
    char                     key[kMaxKey];
    size_t                   keyLen;
    uint32_t                 scalars;
    uint32_t                 arrays;
    char                     firstArray[kMaxKey];
};

void SkipWs(JsonWalk& w) {
    while (w.p < w.end && (*w.p == ' ' || *w.p == '\t' || *w.p == '\n' || *w.p == '\r')) ++w.p;
}

// Copies the JSON string body into `out`, resolving the six escapes the format defines.
// \u is passed through as its raw six characters rather than transcoded: the engine's own
// profile is ASCII, and inventing a UTF-8 encoder here would be a second, untested one.
bool ReadString(JsonWalk& w, char* out, size_t cap, size_t& len) {
    if (w.p >= w.end || *w.p != '"') return false;
    ++w.p;
    len = 0;
    while (w.p < w.end && *w.p != '"') {
        char c = *w.p++;
        if (c == '\\') {
            if (w.p >= w.end) return false;
            const char e = *w.p++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                default:  c = e;    break;
            }
        }
        if (len + 1 >= cap) return false;
        out[len++] = c;
    }
    if (w.p >= w.end) return false;
    ++w.p;
    out[len] = '\0';
    return true;
}

bool PushKey(JsonWalk& w, const char* name, size_t n) {
    const size_t sep = w.keyLen ? 1u : 0u;
    if (w.keyLen + sep + n + 1 > kMaxKey) return false;
    if (sep) w.key[w.keyLen] = '|';
    memcpy(w.key + w.keyLen + sep, name, n);
    w.keyLen += sep + n;
    w.key[w.keyLen] = '\0';
    return true;
}

void PopKey(JsonWalk& w, size_t to) {
    w.keyLen = to;
    w.key[to] = '\0';
}

bool WalkValue(JsonWalk& w, int depth);

bool WalkObject(JsonWalk& w, int depth) {
    ++w.p;  // '{'
    SkipWs(w);
    if (w.p < w.end && *w.p == '}') { ++w.p; return true; }
    for (;;) {
        SkipWs(w);
        char name[kMaxKey];
        size_t n = 0;
        if (!ReadString(w, name, sizeof name, n)) return false;
        SkipWs(w);
        if (w.p >= w.end || *w.p != ':') return false;
        ++w.p;
        const size_t mark = w.keyLen;
        if (!PushKey(w, name, n)) return false;
        if (!WalkValue(w, depth + 1)) return false;
        PopKey(w, mark);
        SkipWs(w);
        if (w.p < w.end && *w.p == ',') { ++w.p; continue; }
        if (w.p < w.end && *w.p == '}') { ++w.p; return true; }
        return false;
    }
}

// Arrays are consumed for structure and NOT carried: there is no verified array setter on
// this build.  Skipping without recording would be the silence this packet exists to remove,
// so the count and the first path are kept for the caller to log.
bool SkipArray(JsonWalk& w, int depth) {
    if (w.arrays == 0) {
        memcpy(w.firstArray, w.key, w.keyLen + 1);
    }
    ++w.arrays;
    ++w.p;  // '['
    int nest = 1;
    while (w.p < w.end && nest > 0) {
        const char c = *w.p;
        if (c == '"') {
            char scratch[8];
            size_t n = 0;
            const char* save = w.p;
            if (!ReadString(w, scratch, sizeof scratch, n)) {
                // A string longer than the scratch buffer is still legal input; step over it
                // by hand rather than failing the whole profile on a long item name.
                w.p = save + 1;
                while (w.p < w.end && *w.p != '"') w.p += (*w.p == '\\') ? 2 : 1;
                if (w.p < w.end) ++w.p;
            }
            continue;
        }
        if (c == '[' || c == '{') ++nest;
        if (c == ']' || c == '}') --nest;
        ++w.p;
        if (nest == 0) return true;
    }
    (void)depth;
    return nest == 0;
}

bool WalkValue(JsonWalk& w, int depth) {
    if (depth > kMaxDepth) return false;
    SkipWs(w);
    if (w.p >= w.end) return false;
    const char c = *w.p;
    if (c == '{') return WalkObject(w, depth);
    if (c == '[') return SkipArray(w, depth);
    if (c == '"') {
        char value[512];
        size_t n = 0;
        if (!ReadString(w, value, sizeof value, n)) return false;
        w.sink->setString(w.sink->ctx, w.key, value);
        ++w.scalars;
        return true;
    }
    // number, true, false, null
    const char* start = w.p;
    while (w.p < w.end && *w.p != ',' && *w.p != '}' && *w.p != ']' &&
           *w.p != ' ' && *w.p != '\n' && *w.p != '\r' && *w.p != '\t') ++w.p;
    const size_t n = size_t(w.p - start);
    if (n == 0 || n >= 64) return false;
    char text[64];
    memcpy(text, start, n);
    text[n] = '\0';
    if (strcmp(text, "null") == 0) return true;   // absent is not the same as zero
    if (strcmp(text, "true") == 0 || strcmp(text, "false") == 0) {
        w.sink->setInt(w.sink->ctx, w.key, text[0] == 't' ? 1 : 0);
        ++w.scalars;
        return true;
    }
    // A fractional or exponent form has no verified setter, so it is carried as a string --
    // which is what the engine's own profile does for every non-integer field it stores.
    bool integral = true;
    for (size_t i = 0; i < n; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (i == 0 && text[i] == '-'))) integral = false;
    if (integral) {
        int64_t v = 0;
        bool neg = text[0] == '-';
        for (size_t i = neg ? 1u : 0u; i < n; ++i) v = v * 10 + (text[i] - '0');
        w.sink->setInt(w.sink->ctx, w.key, neg ? -v : v);
        ++w.scalars;
    } else {
        w.sink->setString(w.sink->ctx, w.key, text);
        ++w.scalars;
    }
    return true;
}

// ---------------------------------------------------------------------------------------

}  // namespace

ProfileWalkStats WalkProfileScalars(const Blob& json, const ProfileScalarSink& sink) {
    ProfileWalkStats out{};
    if (!json.p || json.n == 0 || !sink.setString || !sink.setInt) return out;
    JsonWalk w{};
    w.p = reinterpret_cast<const char*>(json.p);
    w.end = w.p + json.n;
    w.sink = &sink;
    SkipWs(w);
    out.wellFormed = w.p < w.end && *w.p == '{' && WalkObject(w, 0);
    out.scalars = w.scalars;
    out.arrays = w.arrays;
    memcpy(out.firstArray, w.firstArray, sizeof out.firstArray);
    return out;
}


namespace {

void SinkSetString(void* ctx, const char* key, const char* value) {
    CallJsonSetString(ctx, key, value);
}

void SinkSetInt(void* ctx, const char* key, int64_t value) {
    CallJsonSetInt(ctx, key, value);
}

// The witness the read-back asks back.  It is chosen from the RECORDING by the same pure walk the
// write uses, so the key asked for is a key that was written -- never one this file made up -- and
// a loadout or unlocks key wins when the recording carries one, because those are the keys the
// actor refresh reads.  Only string leaves qualify: IDENT.JSON_GET_STRING is the reader this build
// exposes, and a value that does not fit the buffer is skipped rather than compared truncated.
struct WitnessSink {
    char key[kMaxKey];
    char value[192];
    bool preferred;
    bool have;
};

bool PreferredWitness(const char* key) {
    return strncmp(key, "loadout", 7) == 0 || strncmp(key, "unlocks", 7) == 0;
}

void WitnessSetString(void* ctx, const char* key, const char* value) {
    WitnessSink* w = static_cast<WitnessSink*>(ctx);
    if (!key || !value || !*value) return;
    if (strlen(key) + 1 > sizeof w->key || strlen(value) + 1 > sizeof w->value) return;
    const bool preferred = PreferredWitness(key);
    if (w->have && (w->preferred || !preferred)) return;
    memcpy(w->key, key, strlen(key) + 1);
    memcpy(w->value, value, strlen(value) + 1);
    w->preferred = preferred;
    w->have = true;
}

void WitnessSetInt(void*, const char*, int64_t) {}

}  // namespace

bool ProfileAppliedReadBack(void* ng, UserIndex index, const Blob& recordedJson) {
    if (!ng) return false;
    void* doc = ProfileJson(ng, index);
    if (!doc || !ProfileJsonUsable(ng, index)) return false;
    // First the engine's own gate: on a cached document nothing was written and nothing can be,
    // so there is no point asking a reader that would answer the cache's own values back.
    if (!ProfileDocWritable(doc)) return false;

    WitnessSink w{};
    const ProfileScalarSink sink{&w, &WitnessSetString, &WitnessSetInt};
    (void)WalkProfileScalars(recordedJson, sink);
    // No string leaf small enough to ask back is COULD-NOT-ASK, and the caller must not treat a
    // question it never asked as an answer: refuse.
    if (!w.have) return false;

    char back[sizeof w.value];
    if (!ReadProfileString(ng, index, w.key, back, sizeof back)) return false;
    return strcmp(back, w.value) == 0;
}

namespace {


// The call site.  ARCH-23 S3.4 forbade emitting it until IDENT.ADMIT_USER_PROTO was promoted;
// the promotion landed (callee_arity = 3, decoded at BOTH call sites, 0x14017BB52 and
// 0x14017B497), so the call is emitted here and the refusal AdmitNotPromoted now names the
// binding failing rather than the fact being absent.
//
// Three arguments, and the third is an INPUT join record the body dereferences at every
// IDENT.ADMIT_JOINREC_*_OFF -- not the &netUserId out-parameter DESIGN 9.2.4 assumed. The user
// INDEX comes back in rax.
using AdmitUserFn = uint16_t (*)(void* netGame, uint64_t slot, const void* joinRec);

static_assert(facts::IDENT_ADMIT_USER_PROTO == 3,
              "AdmitUserFn declares three arguments and IDENT.ADMIT_USER_PROTO is the arity the "
              "registry decodes at the entry point's own call sites; the two may not drift");

// The same mechanism gamestate_live.cpp binds its getters through, for the same reason and after
// the same accident: reinterpret_cast converts ANY address to ANY function type without a word,
// and GS.GET_SYMBOL called on a two-argument shape killed the client at 0x140D5CF01. Naming the
// signature makes a mismatched type a compile error, and the address is not called at all until
// the ARGUMENT MOVES at IDENT.ADMIT_USER.ARGMOVE_OFF still match the registry -- so a rebase, a
// patched image or a re-derivation that disagrees fails at bind instead of inside the insert with
// an undefined JoinRec pointer in R8.
template <typename Sig, size_t N>
Sig Guarded(uint64_t va, const uint8_t (&argMove)[N], uint64_t argMoveOff) {
    static_assert(std::is_pointer<Sig>::value &&
                      std::is_function<typename std::remove_pointer<Sig>::type>::value,
                  "Guarded<Sig> takes the pointer-to-function type the entry point is called as");
    const void* at = buildguard::MappedAddress(va);
    if (at == nullptr) return nullptr;
    const uint8_t* moves = static_cast<const uint8_t*>(at) + argMoveOff;
    if (!Readable(moves, N)) return nullptr;
    for (size_t i = 0; i < N; ++i) {
        if (moves[i] != argMove[i]) return nullptr;
    }
    return reinterpret_cast<Sig>(const_cast<void*>(at));
}

PawnProvision::Refusal AdmitUser(void* ng, UserSlot slot, const uint8_t rec[kJoinRecBytes],
                                 UserIndex& out) {
    const AdmitUserFn admit = Guarded<AdmitUserFn>(facts::IDENT_ADMIT_USER_VA,
                                                   facts::IDENT_ADMIT_USER_ARGMOVE,
                                                   facts::IDENT_ADMIT_USER_ARGMOVE_OFF);
    if (admit == nullptr) return PawnProvision::Refusal::AdmitNotPromoted;
    out = UserIndex(admit(ng, slot.v, rec));
    if (out.v == kInvalidU16) return PawnProvision::Refusal::InsertReturnedNoIndex;
    return PawnProvision::Refusal::None;
}

// The three engine reads PawnProvision::AdmitIntoLobbySlot makes on our behalf. They are hooks and
// not direct calls because the decision they feed is the one L3 #14 got wrong, and a decision that
// can only run against a live CR15NetGame cannot be shown to have changed.
struct AdmitCtx {
    void*           ng;
    const uint8_t*  rec;
    IIdentity*      id;     // the shipped Identity, so the dress runs through its own two paths
    const JoinDesc* desc;   // the recording's profile and loadout, authored from the PlayerRecord
};

// L3 #18 F1's two hooks.  Declared here and defined below the class because they run the SAME
// SetProfileJson and ApplyLoadout the dress ladder's steps 2 and 4 run -- there is no second
// implementation of either, which is the point: the admission does not dress the entrant its own
// way, it does W2-C's work in W2-C's order, early enough to be upstream of the actor step.
bool     HookApplyProfile(void* ctx, uint16_t slot, uint16_t index);
bool     HookProfileApplied(void* ctx, uint16_t slot, uint16_t index);
PawnProvision::Refusal HookAttachProfile(void* ctx, uint16_t slot, uint16_t index);
bool     HookPublishLoadout(void* ctx, uint16_t slot, uint16_t index);
bool     HookSourceLoadout(void* ctx, uint16_t slot, uint16_t index);
uint64_t LoadoutDefaultInstanceSymbol();
uint64_t HookLoadoutCount(void* ctx, uint16_t slot);

static_assert(kSlotUnmapped == kInvalidU16,
              "the hooks answer absence in the provisioner's word and read it in this file's; if "
              "the two ever diverge a live index reads as \"could not say\" and the viewer's own "
              "slot stops being excluded");

// The reply route's four hooks and the queue the tick parks a request in.  Declared here and
// defined at the foot of the file, beside the pump they belong to: they are the ONLY things in
// this file that must not run from the tick, and keeping them together is what makes that legible.
bool     HookReplyPromoted(void* ctx);
bool     HookReplyInReceivePhase(void* ctx);
PawnProvision::Refusal HookReplySeed(void* ctx, uint16_t slot);
bool     HookReplyDispatch(void* ctx, const PawnProvision::ReplyMsg& msg, uint32_t bytes);
void     ReplyEnqueue(uint16_t slot, const JoinDesc& desc, IIdentity* id);
void     ReplyNoteWrongPhase();

uint16_t HookIndexOfSlot(void* ctx, uint16_t slot) {
    UserIndex index(kInvalidU16);
    if (!IndexOfSlot(static_cast<AdmitCtx*>(ctx)->ng, UserSlot(slot), index)) return kSlotUnmapped;
    return index.v;
}

uint16_t HookSlotOfIndex(void* ctx, uint16_t index) {
    UserSlot slot(kInvalidU16);
    if (!SlotOfIndex(static_cast<AdmitCtx*>(ctx)->ng, UserIndex(index), slot)) return kSlotUnmapped;
    return slot.v;
}

PawnProvision::Refusal HookAdmit(void* ctx, uint16_t slot, UserIndex& out) {
    AdmitCtx* c = static_cast<AdmitCtx*>(ctx);
    return AdmitUser(c->ng, UserSlot(slot), c->rec, out);
}

// THE ACTOR STEP, and it is the engine's own -- NETGAME.REFRESH_PLAYER_ACTOR_VA, two arguments,
// the second the user index the insert just answered.  L3 #16 admitted an entrant with the state
// word already at 9 and still got `driveable=no` on both surfaces, because the bot insert
// IDENT.ADMIT_USER_VA registers the new user into R14PlayerBroadcaster, R14NetBot and RxAI and
// stops: NETGAME.SET_PLAYER_ACTOR_VA -- the call that builds an actor out of a body template and
// attaches R14RemotePlayer, R14NetPhysics, R14NetVoipReceiver and R15NetCustomization to it -- is
// reached only from the engine's REMOTE entrant admission NETGAME.ADD_REMOTE_USER_VA, the sibling
// the add-entrant reply handler takes on its other arm.  This wrapper is what the engine's own
// message handler sub_14017C370 calls, and it authors nothing: the body template comes from the
// lobby's per-slot `bodytype` netvar through NETGAME.PICK_BODY_TEMPLATE_VA.
using RefreshPlayerActorFn = uint8_t (*)(void* netGame, uint16_t index);

bool HookCreateActor(void* ctx, uint16_t index) {
    const RefreshPlayerActorFn refresh = Guarded<RefreshPlayerActorFn>(
        facts::NETGAME_REFRESH_PLAYER_ACTOR_VA, facts::NETGAME_REFRESH_PLAYER_ACTOR_VA_PROLOGUE, 0);
    if (refresh == nullptr) return false;
    return refresh(static_cast<AdmitCtx*>(ctx)->ng, index) != 0;
}

// What the step must leave behind, read back off the engine rather than off its return value: the
// step answers 0 when the entrant's actor already IS the requested template, which is a legitimate
// no-op, so the postcondition is the component instance itself.
uint16_t HookActorCi(void* ctx, uint16_t index) {
    ActorComponentRef ref;
    if (!ResolveComponent(static_cast<AdmitCtx*>(ctx)->ng, UserIndex(index), ref))
        return kSlotUnmapped;
    if (ref.handle > uint32_t(UINT16_MAX)) return kSlotUnmapped;
    const uint16_t handle = uint16_t(ref.handle);
    return handle == kInvalidU16 ? kSlotUnmapped : handle;
}

// THE READINESS PREDICATE FOR THE ACTOR STEP -- read off the engine, before the call, never after.
// NETGAME.REFRESH_PLAYER_ACTOR_VA has exactly two code xrefs in the image and both are netgame
// message handlers (sub_14017C370, sub_14019F260) reacting to a bodytype change on an entrant that
// is ALREADY a remote player: the component instance is that call's PRECONDITION, and the actorCi
// read below is the same engine surface the postcondition uses.  L3 #21 issued the refresh on an
// entrant that held none, from inside the engine's per-frame update, and the main thread never came
// back -- Rip inside engine code, no wait object of any kind on the stack, the watchdog's
// "Deadlock detected!" 11 s later.
bool HookActorAssetsReady(void* ctx, uint16_t index) {
    return HookActorCi(ctx, index) != kSlotUnmapped;
}

// ---------------------------------------------------------------------------------------
// THE ACTOR HOP CHAIN'S ENGINE SURFACE (P1-24).  What L3 #23 measured -- `Actor : 0x... (disabled)
// (pooled)` -- is an actor the remote admission arm allocated and then left alone: the engine's
// own bot arm sub_140152210 ENABLES it and PLACES it three instructions apart, and the remote arm
// sub_1401539A0 does neither.  These are the reads and the one driver that close that distance.
//
// TWO SPACES, AND THEY ARE NOT THE SAME OBJECT.  ENG-W2-P1-23 settled this out of instruction
// bytes and it is the whole reason this block exists:
//
//     1401526A3  mov rdx, [r12]           49 8B 14 24   <- guid.hi BY VALUE
//     1401526A7  mov rcx, [rdi+2B08h]     48 8B 8F ..   <- the CncaGameSpace, NETGAME.CS_SPACE_OFF
//     1401526AE  call sub_1404EDA40                     <- ENABLE, on the CHILD space
//     140152710  mov rcx, [rdi+8]         48 8B 4F 08   <- the CR15Game, IDENT.NETGAME_GAMESPACE_OFF
//     14015271C  mov rdx, rbx                           <- &guid, a POINTER
//     14015272D  call sub_140519BF0                     <- PLACE, on the ROOT space
//
// The enable is handed the ALREADY-RESOLVED container and the guid's second qword as a value; the
// predicate and the place are handed the root and resolve the container themselves through
// ENGINE.ACTOR_GUID_CONTAINER_LOOKUP.  Binding either one with the other's shape indexes the wrong
// object with a pointer where a word pair belongs and neither the call nor its return says a word
// about it, which is the L3 #21 class of accident.
//
// ARGUMENT SHAPE IS IN THE TYPE SYSTEM HERE, AND IS NOW GUARDED IN BYTES.  The .PROLOGUE
// guard proves the address still holds the bytes we wrote down; it says nothing about how the
// callee CONSUMES argument 2, and all three of these entry points share a stack frame with
// hundreds of others.  What settles it is the argument move itself, filed and byte-anchored by
// ENG-W2-P1-OWED and read back out of the shipped exe by the registry's own anchor check:
//
//   ENGINE.ACTOR_ENABLE      +0x35  48 8B DA 4C 8B F9 ... 48 89 55 7F   mod==11: a VALUE, and
//                                   the 48 89 55 7F stores it as a FULL QWORD, which is why
//                                   argument 2 is uint64_t and a u32 would drop the pool half
//   ENGINE.ACTOR_IS_ENABLED  +0x0A  48 83 3A FF 48 8B DA 0F             mod==00: a POINTER
//   ENGINE.ACTOR_PLACE       +0x1A  48 8B 1A 48 8B F2 48 8B             mod==00: a POINTER
//
// The OFFSETS are deliberately not registry facts: no instruction encodes the distance from an
// entry point to its argument move, so it has no anchor and the registry withholds such a row.
// They are search POSITIONS, not trusted inputs -- a wrong one makes the byte compare fail and
// Guarded returns nullptr, which is the same refusal a moved entry point produces.
//
// The three call types and the ActorHopSurface that carries them are in the header: a test stands
// in for the engine by installing its own HopCallTable, which is the only way the argument SHAPE
// above can be asserted at all in a process that is not echovr.exe.

static_assert(sizeof(uint64_t) == 8,
              "ENGINE.ACTOR_ENABLE consumes argument 2 with a REX.W move: the declaration must be "
              "the full word pair, not its low half");

// The four addresses one hop read needs, resolved together so a partial resolution cannot be
// mistaken for a negative answer: every hook below returns false when this returns false, and
// false out of a POSTCONDITION is "not yet", which is the chain's own could-not-ask.
struct HopAddr {
    void*          ng   = nullptr;
    void*          root = nullptr;   // [ng+0x8],    the CR15Game the predicate resolves through
    void*          cs   = nullptr;   // [ng+0x2B08], the CncaGameSpace the actor was built in
    const uint8_t* guid = nullptr;   // 16 B: { u64 lookupKey; u64 indexWords; }
    const HopCallTable* calls = nullptr;   // a test's stand-in table, or null for the engine
};

bool HopResolve(void* ctx, uint16_t index, HopAddr& out) {
    ActorHopSurface* c = static_cast<ActorHopSurface*>(ctx);
    if (c == nullptr || c->netGame == nullptr) return false;
    if (index == kSlotUnmapped) return false;
    out.calls = c->calls;
    out.ng = c->netGame();
    if (out.ng == nullptr) return false;
    if (!Readable(At(out.ng, kNETGAME_GAMESPACE_OFF), sizeof(void*))) return false;
    if (!Readable(At(out.ng, facts::NETGAME_CS_SPACE_OFF), sizeof(void*))) return false;
    out.root = Rd<void*>(out.ng, kNETGAME_GAMESPACE_OFF);
    out.cs   = Rd<void*>(out.ng, facts::NETGAME_CS_SPACE_OFF);
    if (out.root == nullptr || out.cs == nullptr) return false;
    const uint8_t* guid = At(out.ng, uint64_t(kUSER_ACTOR_GUID_OFF) +
                                     uint64_t(facts::USER_STRIDE) * index);
    if (!Readable(guid, 16)) return false;
    out.guid = guid;
    return true;
}

// HOP 1 -- REGISTERED, and it is a READ-BACK, never a call.  The remote arm has already run
// NETGAME.SET_PLAYER_ACTOR, which CREATES the actor and CS-registers it (ARCH-23 S14.0, which
// refutes the reading that said otherwise); the engine's own way to ask whether a guid names an
// actor is ENGINE.ACTOR_GUID_CONTAINER_LOOKUP, the binary search over a sorted (key -> container*)
// table that is the first act of both the predicate and the place.
//
// ★ THE TABLE LIVES ON THE ROOT, NOT ON THE CHILD SPACE (S14.4).  The lookup binary-searches
// [a1+8016] bounded by [a1+8064] and falls back to [a1+8080]/[a1+8128], and EVERY site in the
// image supplies [ng+8]: the resolver's own first act, and sub_1404F3570([ng+8], ...) inside both
// SET_PLAYER_ACTOR and the participating arm.  Passing the child space searched a field of a
// different object, so this hop's answer never meant what its comment said it meant.
//
// The engine's own three steps, and the COPY is not a nicety: ENGINE.ACTOR_GUID_CANON writes
// through its argument and the source here is the engine's own user record.
bool HopInRemotePlayerCs(void* ctx, uint16_t index) {
    HopAddr a;
    if (!HopResolve(ctx, index, a)) return false;
    const ActorLookupFn lookup = a.calls != nullptr
        ? a.calls->lookup
        : Guarded<ActorLookupFn>(facts::ENGINE_ACTOR_GUID_CONTAINER_LOOKUP_VA,
                                 facts::ENGINE_ACTOR_GUID_CONTAINER_LOOKUP_VA_PROLOGUE, 0);
    if (lookup == nullptr) return false;

    uint8_t g[16];
    memcpy(g, a.guid, sizeof g);
    const ActorGuidCanonFn canon = a.calls != nullptr
        ? a.calls->canon
        : Guarded<ActorGuidCanonFn>(facts::ENGINE_ACTOR_GUID_CANON_VA,
                                    facts::ENGINE_ACTOR_GUID_CANON_VA_PROLOGUE, 0);
    if (canon == nullptr) return false;
    canon(a.root, g);
#ifdef W2P1_MUTATE_CANON_IN_PLACE
    memcpy(const_cast<uint8_t*>(a.guid), g, sizeof g);
#endif
#ifdef W2P1_MUTATE_HOP1_ARG_CS
    return lookup(a.cs, Rd<uint64_t>(g, 0)) != nullptr;
#else
    return lookup(a.root, Rd<uint64_t>(g, 0)) != nullptr;
#endif
}

// Distance from each entry point's VA to its argument move, i.e. the anchor address of the
// matching .ARG2_SHAPE row minus the VA the row guards.
constexpr uint64_t kActorEnableArg2Off = 0x35;      // 0x1404EDA75 - 0x1404EDA40
constexpr uint64_t kActorIsEnabledArg2Off = 0x0A;   // 0x1404E4EAA - 0x1404E4EA0

// HOP 2 POSTCONDITION -- the engine's OWN predicate, on the ROOT space and with a POINTER to the
// guid, which is the same function and the same argument shape sub_1401CD1A0 renders its
// `Actor : 0x... (enabled)` line off.  Our hop and that log line therefore cannot disagree, which
// is the point of using it rather than reading the flag byte ourselves.
bool HopEnabled(void* ctx, uint16_t index) {
    HopAddr a;
    if (!HopResolve(ctx, index, a)) return false;
    const ActorIsEnabledFn isEnabled = a.calls != nullptr
        ? a.calls->isEnabled
        : Guarded<ActorIsEnabledFn>(facts::ENGINE_ACTOR_IS_ENABLED_VA,
                                    facts::ENGINE_ACTOR_IS_ENABLED_ARG2_SHAPE,
                                    kActorIsEnabledArg2Off);
    if (isEnabled == nullptr) return false;
    return isEnabled(a.root, a.guid) != 0;
}

// A0's PARTITION -- and it is not a bound (S14.5).  NETGAME.LOCAL_USER_COUNT_OFF counts the LOCAL
// user records, which occupy entrant indices [0, count).  The engine's participating arm branches
// on exactly this and handles an entrant below it in its own first block, so a replay entrant --
// which is always at or above it, because the remote arm increments only the entrant count -- is
// the only kind this chain authors anything for.
PawnProvision::LocalUserAnswer HopIsLocalUser(void* ctx, uint16_t index) {
    using Answer = PawnProvision::LocalUserAnswer;
    ActorHopSurface* s = static_cast<ActorHopSurface*>(ctx);
    if (s == nullptr || s->netGame == nullptr) return Answer::Unreadable;
    void* ng = s->netGame();
    if (ng == nullptr) return Answer::Unreadable;
    // AUD-W2-P1-32 MINOR-4: a count that will not prove is UNREADABLE, never "not a local user".
    // The arm behind this answer now issues NETGAME.REFRESH_PLAYER_ACTOR, which does no index
    // check of its own and rebuilds a player actor, so this partition must fail CLOSED.
    if (!Readable(At(ng, facts::LOCAL_NETGAME_LOCAL_USER_COUNT_OFF), sizeof(uint16_t)))
        return Answer::Unreadable;
    return index < Rd<uint16_t>(ng, facts::LOCAL_NETGAME_LOCAL_USER_COUNT_OFF) ? Answer::Local
                                                                              : Answer::Remote;
}

// The session store the two `spawned` setters write: *(u64*)(ng + NETGAME.SESSION_OBJ_OFF) +
// SESSION.STORE_SUBOBJ_OFF, which is the expression both engine sites use.  A test's stand-in
// table carries its own store instead, so nothing here has to be reachable to exercise the rules.
void* SpawnedStore(void* ng, const HopCallTable* calls) {
    if (calls != nullptr) return calls->store;
    if (!Readable(At(ng, facts::NETGAME_SESSION_OBJ_OFF), sizeof(void*))) return nullptr;
    void* session = Rd<void*>(ng, facts::NETGAME_SESSION_OBJ_OFF);
    if (session == nullptr) return nullptr;
    return At(session, facts::SESSION_STORE_SUBOBJ_OFF);
}

// HOP 3 POSTCONDITION, AND A2's -- NETGAME.USER_SPAWNED_GET, the engine's own boolean.  Reading
// the CONSUMER's function rather than either record is what makes A2 side-agnostic: it cannot
// answer true off the record the engine is not going to read.
bool HopSpawned(void* ctx, uint16_t index) {
    ActorHopSurface* s = static_cast<ActorHopSurface*>(ctx);
    if (s == nullptr || s->netGame == nullptr) return false;
    void* ng = s->netGame();
    if (ng == nullptr) return false;
    // The bound is read on the game thread, inside the window RUN-27 narrowed
    // the fault to, so it is probed before it is read here as it is in every other hop body.
    if (!Readable(At(ng, kENTRANT_COUNT_OFF), sizeof(uint16_t))) return false;
    if (index >= Rd<uint16_t>(ng, kENTRANT_COUNT_OFF)) return false;
    const UserSpawnedGetFn get = s->calls != nullptr
        ? s->calls->spawnedGet
        : Guarded<UserSpawnedGetFn>(facts::NETGAME_USER_SPAWNED_GET_VA,
                                    facts::NETGAME_USER_SPAWNED_GET_VA_PROLOGUE, 0);
    if (get == nullptr) return false;
    return get(ng, index) != 0;
}

// A2 -- THE ENTRANT'S `spawned` RECORD, ON THE SIDE ITS READER SELECTS.
//
// There are two records and the consumer chooses between them: SESSION.KV_SIDE_SELECT is
// `slot < *(u64*)(store + SESSION.STORE_SLOT_COUNT_OFF) && (rec[0] & 0xF) == 6 && rec[1]` --
// true -> the FLAT key `user[N]|spawned` in the store's top-level map, false -> the per-slot user
// record's own `spawned`.  We do not predict which a replay-local session lands on; we ASK the
// reader's own selector, which is a pure read, and write the side it names through the engine's
// setter for that side.  Both setters end in the engine's own dirty/replication push, so neither
// is a memory poke.
//
// The per-slot setter computes store[864] + 216*slot with NO bounds check of its own, so a slot
// past the store's slot count is refused BY NAME rather than written: that write would be an
// out-of-bounds store into the live session document.
//
// SLOT-keyed, and that is the whole reason it is its own function: A2 derives the slot from the
// entrant record, while the reply seed runs BEFORE the entrant add, when no entrant record exists
// and the slot is the only coordinate there is.
PawnProvision::SpawnedWrite WriteSpawnedForSlot(void* ng, const HopCallTable* calls, uint64_t slot,
                                                bool spawned) {
    using PawnProvision::SpawnedWrite;
    void* store = SpawnedStore(ng, calls);
    if (store == nullptr) return SpawnedWrite::Unavailable;

    const KvSideSelectFn select = calls != nullptr
        ? calls->sideSelect
        : Guarded<KvSideSelectFn>(facts::SESSION_KV_SIDE_SELECT_VA,
                                  facts::SESSION_KV_SIDE_SELECT_VA_PROLOGUE, 0);
    const KvSetFlatFn setFlat = calls != nullptr
        ? calls->setFlat
        : Guarded<KvSetFlatFn>(facts::SESSION_KV_SET_FLAT_VA,
                               facts::SESSION_KV_SET_FLAT_VA_PROLOGUE, 0);
    const KvSetSlotFn setSlot = calls != nullptr
        ? calls->setSlot
        : Guarded<KvSetSlotFn>(facts::SESSION_KV_SET_SLOT_VA,
                               facts::SESSION_KV_SET_SLOT_VA_PROLOGUE, 0);
    if (select == nullptr || setFlat == nullptr || setSlot == nullptr)
        return SpawnedWrite::Unavailable;

#ifdef W2P1_MUTATE_A2_WRONG_SIDE
    const bool flat = true;
#else
    const bool flat = select(store, slot) != 0;
#endif
    if (flat) {
        char key[sizeof("user[18446744073709551615]|spawned")];
        if (!FormatSpawnedKey(key, sizeof key, slot)) return SpawnedWrite::Unavailable;
        setFlat(store, key, spawned ? 1 : 0);
        return SpawnedWrite::Issued;
    }

    // The per-slot side, and its bound is the store's own slot count.
    const uint64_t* count = calls != nullptr
        ? calls->storeSlotCount
        : (Readable(At(store, facts::SESSION_STORE_SLOT_COUNT_OFF), sizeof(uint64_t))
               ? reinterpret_cast<const uint64_t*>(At(store, facts::SESSION_STORE_SLOT_COUNT_OFF))
               : nullptr);
    if (count == nullptr) return SpawnedWrite::Unavailable;
    if (slot >= *count) return SpawnedWrite::Unwritable;
    setSlot(store, slot, SpawnedSlotKey(), spawned ? 1 : 0);
    return SpawnedWrite::Issued;
}

PawnProvision::SpawnedWrite HopWriteSpawned(void* ctx, uint16_t index) {
    using PawnProvision::SpawnedWrite;
    ActorHopSurface* s = static_cast<ActorHopSurface*>(ctx);
    if (s == nullptr || s->netGame == nullptr) return SpawnedWrite::Unavailable;
    void* ng = s->netGame();
    if (ng == nullptr) return SpawnedWrite::Unavailable;
    if (!Readable(At(ng, kENTRANT_COUNT_OFF), sizeof(uint16_t))) return SpawnedWrite::Unavailable;
    if (index >= Rd<uint16_t>(ng, kENTRANT_COUNT_OFF)) return SpawnedWrite::Unavailable;

    // The engine's own consumer, asked first: an already-true record is not written again.
    if (HopSpawned(ctx, index)) return SpawnedWrite::AlreadySet;

    const uint64_t slot = uint64_t(Rd<uint16_t>(ng, UserOff(index)) & kUSER_SLOT_MASK);
    return WriteSpawnedForSlot(ng, s->calls, slot, true);
}

// Whether the reply seed wrote a true `spawned` into this entrant's SLOT before the add.  Index ->
// slot through the entrant record, because the seed's ledger is slot-keyed like the seed itself.
bool ReplySeededSpawned(uint16_t slot);
bool HopSeededBeforeAdd(void* ctx, uint16_t index) {
    ActorHopSurface* s = static_cast<ActorHopSurface*>(ctx);
    if (s == nullptr || s->netGame == nullptr) return false;
    void* ng = s->netGame();
    if (ng == nullptr) return false;
    if (!Readable(At(ng, kENTRANT_COUNT_OFF), sizeof(uint16_t))) return false;
    UserSlot slot(kSlotUnmapped);
    if (!SlotOfIndex(ng, UserIndex(index), slot)) return false;
    return ReplySeededSpawned(slot.v);
}

// THE BACKSTOP'S PAIR -- NETGAME.PARTICIPATING_VA then the dress half, in the order
// NETGAME.USER_SETTINGS_APPLY's own tail issues them (sub_1401B1FD0: `sub_1401C9BA0(a1, a2)` ...
// `sub_1401C7BE0(a1, a2)`), with none of its Team Slot reallocation.  PARTICIPATING walks component
// systems, so it may be issued only from the stage-0 NETGAME.UPDATE window (L3 #21), and this flag
// is that window's witness: true only while the detour runs the registered sweep.
bool g_inStage0Sweep = false;

// SEH in its own POD-only function (MSVC C2712).
bool CallEntrantPairGuarded(EntrantDispatchFn participating, EntrantDispatchFn dress, void* ng,
                            uint16_t index) {
    __try {
        participating(ng, index);
        dress(ng, index);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool HopParticipatePair(void* ctx, uint16_t index) {
    ActorHopSurface* s = static_cast<ActorHopSurface*>(ctx);
    if (s == nullptr || s->netGame == nullptr) return false;
    if (!g_inStage0Sweep) return false;
    void* ng = s->netGame();
    if (ng == nullptr) return false;
    if (!Readable(At(ng, kENTRANT_COUNT_OFF), sizeof(uint16_t))) return false;
    if (index >= Rd<uint16_t>(ng, kENTRANT_COUNT_OFF)) return false;
    // Neither callee bounds its index, and PARTICIPATING's LOCAL arm enables with no `spawned`
    // gate at all, so the partition fails closed here as well as in the caller.
    if (HopIsLocalUser(ctx, index) != PawnProvision::LocalUserAnswer::Remote) return false;

    // BOTH entry points are proven before EITHER is called: half the pair is not the pair.
    const EntrantDispatchFn participating = s->calls != nullptr
        ? s->calls->participating
        : Guarded<EntrantDispatchFn>(facts::NETGAME_PARTICIPATING_VA,
                                     facts::NETGAME_PARTICIPATING_VA_PROLOGUE, 0);
    const EntrantDispatchFn dress = s->calls != nullptr
        ? s->calls->dress
        : Guarded<EntrantDispatchFn>(kNETGAME_DRESS_VA, kNETGAME_DRESS_VAPrologue, 0);
    if (participating == nullptr || dress == nullptr) return false;
    return CallEntrantPairGuarded(participating, dress, ng, index);
}

// A3 -- IDENT.SERVER_ASSIGN_TEAM, which is the ONE call this packet issues.  It rewrites the
// packed role word through NETGAME.USER_SETTINGS_APPLY and re-runs the engine's own
// participating/spectating dispatch, so the enable, the chassis apply and the CI chain are the
// ENGINE's work with the ENGINE's arguments.  It answers only whether the call was ISSUED; the
// packed word is re-read by the caller and only that decides.
bool HopAssignTeam(void* ctx, uint16_t index, uint16_t recordedRole) {
    ActorHopSurface* s = static_cast<ActorHopSurface*>(ctx);
    if (s == nullptr || s->netGame == nullptr) return false;
    void* ng = s->netGame();
    if (ng == nullptr) return false;
    if (!Readable(At(ng, kENTRANT_COUNT_OFF), sizeof(uint16_t))) return false;
    if (index >= Rd<uint16_t>(ng, kENTRANT_COUNT_OFF)) return false;
    const AssignTeamFn assign = s->calls != nullptr
        ? s->calls->assignTeam
        : Guarded<AssignTeamFn>(facts::IDENT_SERVER_ASSIGN_TEAM,
                                facts::IDENT_SERVER_ASSIGN_TEAM_PROLOGUE, 0);
    if (assign == nullptr) return false;
    assign(ng, index, uint32_t(recordedRole));
    return true;
}

// A3's OTHER ARM -- the engine's own late-join re-provision, for the entrant whose role word was
// already the recorded one before we got there.  L3 #29b: the engine's `Server Assigning Team`
// auto-pick wrote team 0 into user index 1 a second before our `spawned` write landed, so
// IDENT.SERVER_ASSIGN_TEAM's `v10 != v3` early-out makes A3 a no-op and the participating dispatch
// -- which had already run twice against a `spawned` of 0 -- never runs again.  This call re-runs
// it: NETGAME.REFRESH_PLAYER_ACTOR's body is PICK_BODY_TEMPLATE then NETGAME.SET_PLAYER_ACTOR,
// whose dispatch re-derives participating/spectating off the CURRENT role word unconditionally, so
// with `spawned` now 1 the engine's own enable site fires.  Its only two callers in the image are
// netgame message handlers a replay-local host never receives.
//
// Asked of the PROVIDER SLOT on every call, like every other hop: a cached netgame does not
// survive a level load.  The answer is whether the call was ISSUED and nothing more -- the engine
// returns 0 for a legitimate no-op -- so the caller re-reads ENGINE.ACTOR_IS_ENABLED after it.
PawnProvision::RefreshAnswer HopRefreshPlayerActor(void* ctx, uint16_t index) {
    using PawnProvision::RefreshAnswer;
    ActorHopSurface* s = static_cast<ActorHopSurface*>(ctx);
    if (s == nullptr || s->netGame == nullptr) return RefreshAnswer::Unavailable;
    void* ng = s->netGame();
    if (ng == nullptr) return RefreshAnswer::Unavailable;
    if (!Readable(At(ng, kENTRANT_COUNT_OFF), sizeof(uint16_t))) return RefreshAnswer::Unavailable;
    if (index >= Rd<uint16_t>(ng, kENTRANT_COUNT_OFF)) return RefreshAnswer::Unavailable;
    const RefreshPlayerActorFn refresh = Guarded<RefreshPlayerActorFn>(
        facts::NETGAME_REFRESH_PLAYER_ACTOR_VA, facts::NETGAME_REFRESH_PLAYER_ACTOR_VA_PROLOGUE, 0);
    if (refresh == nullptr) return RefreshAnswer::Unavailable;
    // The WRAPPER's return, and it is ONE-SIDED.  sub_14019F1C0 forwards SET_PLAYER_ACTOR's
    // answer only on the zero path (`14019F1FC test eax,eax / jz 14019F24C`); on the SUCCESS
    // path it CLOBBERS the return register with a session flags byte --
    // `14019F200 mov rax,[rdi+2DA0h] / mov rcx,[rax] / movzx eax,cl / shr al,2` -- and can also
    // return sub_140CB7C50's own al, or 0 where sub_140105510 answers null.  So NON-ZERO does
    // imply sub_1401B0D80 reached :407, and ZERO implies nothing on its own: it is `:91`, and
    // it is equally a full allocate-and-dispatch whose flags bit read 0.  Redispatched is a
    // fact; Declined is a CANDIDATE the caller corroborates against the entrant's SActorGUID
    // before it acts on it (pawn_provision.cpp arm 1's two-read).  Never a verdict either way:
    // the caller re-reads ENGINE.ACTOR_IS_ENABLED after it.
    return refresh(ng, index) ? RefreshAnswer::Redispatched : RefreshAnswer::Declined;
}

// A4 -- THE SPAWN HALF, and it is the engine's own: NETGAME.SPAWN_NOTIFY_VA = sub_1401CAC00,
// two arguments, void return -- verified from the decompile: `void __fastcall sub_1401CAC00(
// __int64 ng, unsigned __int16 index)`.  It fires "[NETGAME] User %s (%s) spawned" then branches
// on the session mode word **(ng + NETGAME.UIFLAGS_PTR_OFF): its else-if remote arm sub_140CB81B0
// -> sub_140D43D90 is what installs + dresses the ncaModel/swapper CI for an entrant.  Our
// synthetic admission drives REFRESH (above) but never this, so the entrant's actor carried no
// model CI (DIAG-SPAWN-MODEL-CI Lever B).  Same guarded shape as HopRefreshPlayerActor, same
// ENTRANT_COUNT bound.  The mode word is read here, BEFORE the call, for the section-4 branch
// probe -- bit1 set, or bit2 clear and bit6 set, is the host arm that does NOT install; the caller
// logs it alongside the model-CI A/B so the run settles in vivo which arm ran for our host-side
// entrant.  Read-only and bounds-checked; the call's own answer is void, so `issued` is all it says.
using SpawnNotifyFn = void (*)(void* netGame, uint16_t index);

// BLOCKER-2 (DIAG-REPLYPHASE section 3).  The bit that forces UserSpawned's host test to false so it
// takes the else-if REMOTE arm (sub_140CB81B0).  host = (m&2) || (!(m&4) && (m&0x40)); setting bit2
// makes (m&4)!=0, so host collapses to (m&2), which the offline session word (0x..4040) leaves clear.
constexpr uint64_t kSessionAuthNonHostBit = 0x4;

// ECHOREPLAY_STAGEB_AUTHFLIP (config row stageb.authflip), default OFF: the
// remote spawn arm the flip forces is the equipment path and cannot dress the body (DIAG-SEAT-DRIVE
// 2.3), so the clean loadout-apply route dresses instead.  Read live so a launch can re-arm the flip
// without a rebuild; the fallback here MUST equal config_keys.inc's declared default or GetBool warns
// on default drift.  config::Interface() is a static and safe before init (returns the default), so
// this is safe on the off-image self-test path too.
bool StageBAuthFlipEnabled() {
    return config::Interface()->GetBool("stageb.authflip", false);
}

// ENG-BOT-BODY gates.  Read live so a launch can A/B the admission paths without a rebuild; the
// fallback here MUST equal config_keys.inc's declared default or GetBool warns on default drift.
// config::Interface() is a static safe before init (returns the default), so both are safe on the
// off-image self-test path too.
// Test override: -1 defers to config (the shipped behavior), 0/1 force the choice so a test can
// exercise the bot path or the legacy reply route deterministically, off-image, without depending on
// the process-wide env-override cache.  The env-override resolves once per process, so a test cannot
// steer the config value after the first read; this seam is how it steers the CHOICE instead.
int g_botBodyForce = -1;
bool BotBodyEnabled() {
    if (g_botBodyForce >= 0) return g_botBodyForce != 0;
    return config::Interface()->GetBool("pawn.bot_body", false);
}
bool BotLoadoutOverlayEnabled() { return config::Interface()->GetBool("pawn.bot_loadout", false); }
// Default OFF so the bot's loadout count stays 0, the L3 #18 guard defers the
// actor step, and the out-of-band model-build that deadlocks the engine never runs.
bool BotPublishLoadoutEnabled() { return config::Interface()->GetBool("pawn.bot_publish_loadout", false); }
// Default OFF so the shipped bot-body default is unchanged (count stays 0, the
// L3 #18 guard defers, no actor step, no model-build, no deadlock).  ON, the recorded loadout is
// parsed into the SOURCE after the insert and copied into the live table before the guard, so the
// guard passes on REAL instances and the bot's actor+model build over a non-empty loadout.  The
// fallback here MUST equal config_keys.inc's declared default or GetBool warns on default drift.
bool BotBodySourceLoadoutEnabled() {
    return config::Interface()->GetBool("pawn.bot_body_source_loadout", false);
}

// ENG-BOT-BODY SECONDARY seed, defined below beside the reply route it also serves.  It writes the
// lobby member record's account nibble to kSeedAccountType (5, not 6) so SPAWN_USER's type-6-gated
// AI-brain record is never allocated for our bot (verified: AddBotUser does not overwrite this word).
// Forward-declared here -- anonymous-namespace blocks in one TU are one namespace -- so the bot
// admission can seed before it calls the insert.
PawnProvision::Refusal SeedLobbyRecordFor(void* ng, const JoinDesc& desc, uint16_t slot, Refusal& why);

// ENG-BOT-BODY TERTIARY AI-mute belt.  Best-effort: write 0 into the NetBot per-actor control word
// the activation's deferred body (sub_140D44330) writes the AI drive LEVEL into --
//   *(u16*)(base + 432*idx + 296),  base = *(NetBotCS + 0x100),
//   idx  = *(u16*)(*(NetBotCS + 208) + 4*slot),  NetBotCS = CS_LOOKUP_BY_SYMBOL(gameSpace, R14NetBot).
// The PRIMARY history-drive (the pose wins over motor input) and the SECONDARY type-5 seed (which
// stops SPAWN_USER allocating the type-6-gated AI-brain record at all) carry the mute; this is a
// defensive extra and is launch-unverified, so it is built to be PROVABLY SAFE rather than trusted:
//   * the engine's symbol hasher is trusted for the unfiled name "R14NetBot" only after it reproduces
//     the two CS symbols this build already carries as facts (R14NetPhysics, R14RemotePlayer) -- a
//     hasher or string-format mismatch fails the known-answer control and SKIPS, never mis-writes;
//   * the CS host is proved by requiring the same lookup to resolve R14NetPhysics on it before it is
//     used for R14NetBot, so a wrong host pointer skips too;
//   * every dereference is Readable-probed and idx is bounds-checked; any failure returns false.
// Never calls into the engine except the two byte-anchored resolvers (hash, lookup).  Returns true
// iff it actually wrote the zero.
using FnFindCsBySymbol = void* (*)(void* host, uint64_t symbol);
using FnSymbolHash     = uint64_t (*)(const char*, uint64_t, int32_t, int32_t, void*);

bool MuteBotNetBotLevel(void* ng, uint16_t slot) {
    if (ng == nullptr || slot >= facts::ENTITY_USER_SLOT_TABLE_MAX) return false;

    const FnSymbolHash hash = Guarded<FnSymbolHash>(facts::IDENT_SYMBOL_FROM_NAME_VA,
                                                    facts::IDENT_SYMBOL_FROM_NAME_VA_PROLOGUE, 0);
    if (hash == nullptr) return false;
    constexpr uint64_t kSeed = ~uint64_t(0);
    constexpr int32_t  kNoCb = 0;
    constexpr int32_t  kUnbounded = -1;
    // Known-answer control on the hasher AND the exact name-string form: prove it against the two
    // filed CS symbols before trusting it for the unfiled R14NetBot.
    if (hash("R14NetPhysics", kSeed, kNoCb, kUnbounded, nullptr) != facts::CS_SYM_R14NETPHYSICS)
        return false;
    if (hash("R14RemotePlayer", kSeed, kNoCb, kUnbounded, nullptr) != facts::CS_SYM_R14REMOTEPLAYER)
        return false;
    const uint64_t netBotSym = hash("R14NetBot", kSeed, kNoCb, kUnbounded, nullptr);
    if (netBotSym == 0) return false;

    const FnFindCsBySymbol find = Guarded<FnFindCsBySymbol>(
        facts::CS_LOOKUP_BY_SYMBOL, facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE, 0);
    if (find == nullptr) return false;

    // The CS host: the CncaGameSpace at NETGAME.CS_SPACE_OFF (a pointer field, deref'd), proved by
    // requiring the same lookup to resolve a known CS on it.  A garbage host resolves nothing and skips.
    if (!Readable(At(ng, facts::NETGAME_CS_SPACE_OFF), sizeof(void*))) return false;
    void* host = Rd<void*>(ng, facts::NETGAME_CS_SPACE_OFF);
    if (host == nullptr) return false;
    if (find(host, facts::CS_SYM_R14NETPHYSICS) == nullptr) return false;   // host proof

    void* netBotCs = find(host, netBotSym);
    if (netBotCs == nullptr || !Readable(netBotCs, 0x108)) return false;  // facts-ok: LOCAL.BROADCASTER_CS_KEYS_OFF a byte span handed to Readable, not a field offset

    void* base     = Rd<void*>(netBotCs, 0x100);
    void* idxTable = Rd<void*>(netBotCs, 208);
    if (base == nullptr || idxTable == nullptr) return false;
    if (!Readable(At(idxTable, uint64_t(4) * slot), sizeof(uint16_t))) return false;
    const uint16_t idx = Rd<uint16_t>(idxTable, uint64_t(4) * slot);

    uint8_t* ctrl = At(base, uint64_t(432) * idx + 296);
    if (!Readable(ctrl, sizeof(uint16_t))) return false;
    *reinterpret_cast<uint16_t*>(ctrl) = 0;
    return true;
}

// The Stage-B window, and it is exactly ONE engine call wide.  SEH in its own function (C2712): the
// authority word is a LIVE word the session reads, so it is restored on EVERY path -- the normal
// return and the SEH fault path (C++ destructors do not unwind for SEH under /EHsc).  The __except
// swallows an engine fault so echovr never crashes.  Returns false if the call faulted (contained).
bool InvokeSpawnNotify(SpawnNotifyFn notify, void* ng, uint16_t index,
                       volatile uint64_t* modeWord, uint64_t savedWord, bool flip) {
    if (flip && modeWord != nullptr) *modeWord = savedWord | kSessionAuthNonHostBit;
    bool faulted = false;
    __try {
        notify(ng, index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
    }
    if (flip && modeWord != nullptr) *modeWord = savedWord;   // unconditional restore, both paths
    return !faulted;
}

PawnProvision::SpawnNotifyResult HopSpawnNotify(void* ctx, uint16_t index) {
    PawnProvision::SpawnNotifyResult r{};
    ActorHopSurface* s = static_cast<ActorHopSurface*>(ctx);
    if (s == nullptr || s->netGame == nullptr) return r;
    void* ng = s->netGame();
    if (ng == nullptr) return r;
    if (!Readable(At(ng, kENTRANT_COUNT_OFF), sizeof(uint16_t))) return r;
    if (index >= Rd<uint16_t>(ng, kENTRANT_COUNT_OFF)) return r;
    const SpawnNotifyFn notify = Guarded<SpawnNotifyFn>(
        kNETGAME_SPAWN_NOTIFY_VA, kNETGAME_SPAWN_NOTIFY_VAPrologue, 0);
    if (notify == nullptr) return r;
    // The branch probe, read here because we are about to issue: **(ng + NETGAME.UIFLAGS_PTR_OFF)
    // is the mode word sub_1401CAC00 selects its arm on.  Only reached once the site resolved, so a
    // stub netgame that has no such field is never dereferenced past its end.  ng+11680 == 0x2DA0.
    uint64_t* mode = Rd<uint64_t*>(ng, facts::NETGAME_UIFLAGS_PTR_OFF);
    uint64_t savedWord = 0;
    bool haveMode = false;
    if (mode != nullptr && Readable(mode, sizeof(uint64_t))) {
        savedWord = *mode;
        r.modeWord = savedWord;          // the HONEST session-authority word (host, offline)
        r.modeRead = true;
        haveMode = true;
    }
    // BLOCKER-2 flip: only when the live word currently reads host, only when enabled, only around
    // the single spawnNotify call.  Probed (Readable) before touching; RAII/SEH restored in
    // InvokeSpawnNotify.  If the session already reads non-host we do not touch it.
    const bool hostNow = haveMode &&
        (((savedWord & 2) != 0) || (((savedWord & 4) == 0) && ((savedWord & 0x40) != 0)));
    const bool flip = hostNow && StageBAuthFlipEnabled();
    r.authFlipApplied = flip;
    r.modeWordEffective = flip ? (savedWord | kSessionAuthNonHostBit) : savedWord;
    volatile uint64_t* modeW = flip ? reinterpret_cast<volatile uint64_t*>(mode) : nullptr;
    const bool ok = InvokeSpawnNotify(notify, ng, index, modeW, savedWord, flip);
    r.spawnFaulted = !ok;
    r.issued = ok;
    return r;
}

// The entrant's SActorGUID, both halves, read straight off IDENT.USER_ACTOR_GUID_OFF -- the word
// sub_1401B0D80:319 writes when it allocates and the word sub_1401C9BA0:95 reads to find the actor
// it enables.  Two reads of this across a refresh are what say an actor was rebuilt.
bool HopActorGuid(void* ctx, uint16_t index, uint64_t* lo, uint64_t* hi) {
    ActorHopSurface* s = static_cast<ActorHopSurface*>(ctx);
    if (s == nullptr || s->netGame == nullptr || lo == nullptr || hi == nullptr) return false;
    void* ng = s->netGame();
    if (ng == nullptr) return false;
    if (!Readable(At(ng, kENTRANT_COUNT_OFF), sizeof(uint16_t))) return false;
    if (index >= Rd<uint16_t>(ng, kENTRANT_COUNT_OFF)) return false;
    const uint64_t off = uint64_t(kUSER_ACTOR_GUID_OFF) + uint64_t(facts::USER_STRIDE) * index;
    if (!Readable(At(ng, off), 2 * sizeof(uint64_t))) return false;
    *lo = Rd<uint64_t>(ng, off);
    *hi = Rd<uint64_t>(ng, off + sizeof(uint64_t));
    return true;
}

// THE READINESS PREDICATE FOR THAT CALL -- read off the engine, before the call, never after.  It
// is the same surface `HookActorAssetsReady` uses on the admission path and for the same reason:
// L3 #21 issued this refresh on an entrant that held no actor component instance and the main
// thread never returned.  Both of the engine's own callers run it on an entrant that already IS a
// remote player, and a resolving component ref is what says ours is one.
bool HopActorAssetsReady(void* ctx, uint16_t index) {
    ActorHopSurface* s = static_cast<ActorHopSurface*>(ctx);
    if (s == nullptr || s->netGame == nullptr) return false;
    void* ng = s->netGame();
    if (ng == nullptr) return false;
    if (!Readable(At(ng, kENTRANT_COUNT_OFF), sizeof(uint16_t))) return false;
    if (index >= Rd<uint16_t>(ng, kENTRANT_COUNT_OFF)) return false;
    ActorComponentRef ref;
    if (!ResolveComponent(ng, UserIndex(index), ref)) return false;
    return ref.handle <= uint32_t(UINT16_MAX) && uint16_t(ref.handle) != kInvalidU16;
}

// HOP 4 stays a WAIT.  The physics component instance is built by the R14RemotePlayer component
// system's own per-frame init, off the registration hop 1 asserts and the enable the engine issues
// on its participating arm; there is no entry point here that may drive it from outside that
// update, and S14.8 does not licence one.  ENGINE.ACTOR_PLACE stays unbound for the same reason it
// always should have been: our poses are authored, so an engine spawn transform is worth nothing
// to us, and its argument shape is recorded in ARCH-23 S14.2 for the day one is wanted.

class Identity final : public IIdentity {
public:
    Identity(void* (*netGame)(), void* (*remotePlayerCs)()) : netGame_(netGame) {
        surface_.netGame        = netGame;
        surface_.remotePlayerCs = remotePlayerCs;
        BindActorHops(surface_);
    }

    const PawnProvision::ActorHops* ActorHops() const override { return &surface_.hops; }

    // `hops.ctx` names THIS instance's surface, so a COPY of an Identity carries a ctx that names
    // the object it was copied from.  Create() assigns from a temporary; without this re-bind every
    // hop the provisioner drives would dereference that temporary's dead frame.
    void RebindHops() { BindActorHops(surface_); }

    bool AddReplayUser(UserSlot slot, UserIndex& out) override {
        void* ng = Ng();
        if (!ng) return Refuse(Refusal::NoNetGame);
        if (slot.v >= facts::ENTITY_USER_SLOT_TABLE_MAX) return Refuse(Refusal::SlotOutOfRange);
        if (!IndexOfSlot(ng, slot, out)) return Refuse(Refusal::NoSuchEntrant);
        // ID-A is refused rather than offered: UserDisplayName formats "Replayer %u" for any
        // index in this band without consulting the profile, so a replayed player put there
        // cannot carry its recorded name -- the opposite of what DESIGN 9.2 ID-A claims.
        if (InReplayBand(ng, out)) return Refuse(Refusal::ReplayBand);
        if (!ProfileJsonUsable(ng, out)) return Refuse(Refusal::EmptyProfile);
        return Refuse(Refusal::None);
    }

    // ARCH-23 W2-P1.  The join record is authored here, out of the descriptor the roster built
    // from the file, and handed to the admission entry point.
    bool ProvisionReplayUser(UserSlot slot, const JoinDesc& desc, UserIndex& out) override {
        out = UserIndex(kInvalidU16);
        void* ng = Ng();
        if (!ng) return RefuseProvision(PawnProvision::Refusal::NoNetGame);
        if (slot.v >= facts::ENTITY_USER_SLOT_TABLE_MAX)
            return RefuseProvision(PawnProvision::Refusal::SlotOutOfRange);

        // ARCH-23 S2.1. NETGAME.STATE_OFF is 0 -- a zero offset carries no displacement field for
        // an anchor to sit on, so the read is at the netgame pointer itself and this comment is
        // the citation. Admitted below NETGAME.STATE_INMATCH the entrant gets an actor GUID and
        // NO PLACED PAWN, silently, which is a worse outcome than not asking. A DEFERRAL: the
        // caller asks again after the edge.
        if (!Readable(ng, sizeof(uint32_t)) ||
            *static_cast<const uint32_t*>(ng) != facts::NETGAME_STATE_INMATCH)
            return RefuseProvision(PawnProvision::Refusal::SessionNotInMatch);

        // ONE OCCUPANCY PREDICATE, and it is not this file's. L3 #14: the provisioner mapped
        // `recorded slot 0 -> slot 1` with the engine's own lobby predicate and this gate then
        // re-derived occupancy from the side table at IDENT.SLOT_TABLE_OFF -- whose +0 word
        // admission never writes, so its zero-filled entries answer "index 0" for
        // all sixteen slots -- and refused SlotAlreadyOccupied on the slot the engine itself had
        // just called free. The gate, the viewer-slot exclusion and the postcondition all live in
        // PawnProvision::AdmitIntoLobbySlot now; the three hooks below are the only engine reads it
        // makes. INV-PP1 is unchanged: an occupied slot is refused, never adopted.
        uint8_t rec[kJoinRecBytes];
        if (!WriteJoinRec(desc, rec)) return RefuseProvision(PawnProvision::Refusal::NoGameSpace);

        // ★ Route each recorded player through the engine's OWN bot body-build
        // (AddBotUser) instead of the type-5 remote reply route below: AddBotUser spawns a visible
        // default body locally with no peer, and the actor step it reaches (SET_PLAYER_ACTOR)
        // installs the ncaPhysics/R14RemotePlayer pair the history-drive needs.  The AI is MUTED, not
        // launched: the shipped movement decouple makes the recorded pose win over motor input
        // (PRIMARY), the type-5 seed keeps SPAWN_USER from allocating the type-6-gated AI-brain record
        // (SECONDARY), and the belt zeroes the NetBot drive level (TERTIARY).  Config pawn.bot_body
        // (default off); off falls through to the AddRemoteUser reply route.
        if (BotBodyEnabled()) {
            // SECONDARY: seed the lobby member record's account nibble to 5 BEFORE the insert, so
            // SPAWN_USER (which fires inside AddBotUser) reads not-a-bot and allocates no AI-brain
            // record.  Best-effort -- a seed refusal does not block the visible body.
            Refusal seedWhy = Refusal::None;
            (void)SeedLobbyRecordFor(ng, desc, slot.v, seedWhy);

            AdmitCtx botCtx{ng, rec, this, &desc};
            PawnProvision::AdmitHooks hooks;         // reply left null -> AdmitIntoLobbySlot takes the insert
            hooks.ctx              = &botCtx;
            hooks.botBody          = true;
            hooks.indexOfSlot      = &HookIndexOfSlot;
            hooks.slotOfIndex      = &HookSlotOfIndex;
            hooks.admit            = &HookAdmit;        // IDENT.ADMIT_USER_VA = AddBotUser
            hooks.seated           = &HookSeated;
            hooks.createActor      = &HookCreateActor;  // REFRESH_PLAYER_ACTOR -> SET_PLAYER_ACTOR
            hooks.actorCi          = &HookActorCi;
            hooks.actorAssetsReady = &HookActorAssetsReady;
            hooks.loadoutCount     = &HookLoadoutCount;
            // ENG-BOT-LOADOUT-FIX bridges the default loadout AddBotUser generated into the per-slot
            // live table before the crash guard, so the count reads > 0 and the actor step
            // (SET_PLAYER_ACTOR -> phys/remote CI) runs.  ENG-BOT-GATE-OFF (safety revert): that
            // actor step triggers the framework model-build out-of-band from our stage-0 detour and
            // DEADLOCKS the engine (DIAG-BOT-DEADLOCK).  Default OFF -- the count stays 0, the L3 #18
            // guard refuses LoadoutsEmpty, no actor step, no model-build, no deadlock (the prior
            // non-crashing behavior: bot admitted, no driveable body).  Flip on for re-arch testing.
            if (BotPublishLoadoutEnabled())
                hooks.publishLoadout = &HookPublishLoadout;
            // ENG-BOT-BODY-SOURCE (echo-102): the working-body experiment.  Fill the loadout SOURCE
            // with the RECORDED loadout AFTER the insert (sourceLoadout) and copy it into the live
            // table before the guard + actor step (publishLoadout), so the L3 #18 guard is satisfied
            // by REAL parsed instances and the phase-16 model-build runs over a NON-EMPTY loadout --
            // no deadlock, a visible native body.  Default OFF (BotBodySourceLoadoutEnabled false),
            // so the shipped bot-body default is byte-for-byte unchanged; the publish is wired here
            // too because on the bot path there is no engine bridge to copy SOURCE->LIVE for us.
            if (BotBodySourceLoadoutEnabled()) {
                hooks.sourceLoadout  = &HookSourceLoadout;
                hooks.publishLoadout = &HookPublishLoadout;
            }
            // OPTIONAL recorded-loadout overlay (config pawn.bot_loadout, default off).  On the bot
            // path AdmitIntoLobbySlot attempts these best-effort and never refuses on them, so a
            // failed overlay cannot discard the guaranteed default body.
            if (BotLoadoutOverlayEnabled()) {
                hooks.attachProfile  = &HookAttachProfile;
                hooks.applyProfile   = &HookApplyProfile;
                hooks.profileApplied = &HookProfileApplied;
            }
            const PawnProvision::Refusal r =
                PawnProvision::AdmitIntoLobbySlot(ng, slot.v, hooks, out);
            // TERTIARY belt: zero the NetBot drive level for the admitted slot (best-effort; skips
            // cleanly, never mis-writes, when the record is not provably resolvable).
            if (r == PawnProvision::Refusal::None) {
                (void)MuteBotNetBotLevel(ng, slot.v);
                NoteAdmittedSlot(slot.v);   // this session seated it, so this session may remove it
            }
            return RefuseProvision(r);
        }

        // ★ THE NO-AI GATE.  AdmitIntoLobbySlot's fall-through -- the direct insert
        // it takes when the reply route is not promoted -- is IDENT.ADMIT_USER
        // (facts::IDENT_ADMIT_USER_VA = 0x140152210 = AddBotUser), which ATTACHES AI (R14NetBot /
        // RxAI).  This provisioner is that function's ONLY production caller, so nothing legitimate
        // depends on the fall-through: when the engine's own remote-admission route cannot promote
        // -- any of its four runtime binds null, or pawn.reply_route disarmed, both of which read
        // out as ReplyRoutePromoted() false -- we seat NOTHING and refuse by name.  On a solo
        // offline host the direct insert would otherwise seat a type-6 AI bot into the owner's LIVE
        // game; the launch's bar is no-AI AND no-crash and the insert clears neither.  RETRYABLE:
        // the slot is refused as a deferral (PawnProvisioner::Refuse), so a later pump that promotes
        // the route re-provisions it through the engine's own AddRemoteUser instead.
#ifndef W2P1_MUTATE_NO_NOAI_GATE
        if (!ReplyRoutePromoted()) {
            out = UserIndex(kInvalidU16);
            return RefuseProvision(PawnProvision::Refusal::DirectInsertSuppressed);
        }
#endif

        AdmitCtx ctx{ng, rec, this, &desc};
        PawnProvision::AdmitHooks hooks;
        // THE ENGINE'S OWN ROUTE, WIRED WHEN IT IS PROMOTED.  The NO-AI gate above already refused
        // the unpromoted case, so in the shipped build the route is always promoted here; the check
        // stays because W2P1_MUTATE_NO_NOAI_GATE removes the gate to prove the seat then reaches the
        // AI-attaching direct insert -- the FAIL-before half of the gate's own test.
        PawnProvision::ReplyRoute route;
        if (ReplyRoutePromoted()) {
            route.ctx             = &ctx;
            route.promoted        = &HookReplyPromoted;
            route.inReceivePhase  = &HookReplyInReceivePhase;
            route.seedLobbyRecord = &HookReplySeed;
#ifndef W2P1_MUTATE_REPLYROUTE_UNWIRED
            // The shape this route must never ship in: one of the four pointers left null makes
            // ProvisionViaReply answer ReplyRouteNotPromoted, so admission falls back to the direct
            // bot insert -- which builds no model CI (reproduces RUN-45: modelCI absent, no body).
            route.dispatch        = &HookReplyDispatch;
#endif
            hooks.reply           = &route;
        }
        hooks.ctx          = &ctx;
        hooks.indexOfSlot  = &HookIndexOfSlot;
        hooks.slotOfIndex  = &HookSlotOfIndex;
        hooks.admit        = &HookAdmit;
        hooks.seated       = &HookSeated;
        hooks.applyProfile = &HookApplyProfile;
        hooks.attachProfile  = &HookAttachProfile;
        hooks.profileApplied = &HookProfileApplied;
        hooks.loadoutCount = &HookLoadoutCount;
        hooks.createActor  = &HookCreateActor;
        hooks.actorCi      = &HookActorCi;
        hooks.actorAssetsReady = &HookActorAssetsReady;
        const PawnProvision::Refusal r =
            PawnProvision::AdmitIntoLobbySlot(ng, slot.v, hooks, out);
        // The ledger the leave is allowed to name, written at the one point that knows the engine
        // created an entrant: AdmitIntoLobbySlot answers None only after its own postcondition read
        // the issued index back out of the engine and saw it name this slot.
        if (r == PawnProvision::Refusal::None) NoteAdmittedSlot(slot.v);
        // ★ THE TICK NEVER PUBLISHES.  ReplyWrongPhase means the route IS bound and this call is
        // not in the phase the engine's own publisher runs in -- so the request is parked, and the
        // listener registered on the netgame's broadcaster drains it from inside the engine's own
        // pump.  It is a DEFERRAL: the provisioner leaves the slot queued and asks again.
        if (r == PawnProvision::Refusal::ReplyWrongPhase) {
            ReplyNoteWrongPhase();
            ReplyEnqueue(slot.v, desc, this);
        }
        return RefuseProvision(r);
    }

    PawnProvision::Refusal LastProvisionRefusal() const override { return lastProvision_; }

    // ★ THE RELEASE HALF: EMULATE A PLAYER LEAVE, the mirror of the admission's emulated join.
    //
    // The recorded players are admitted by driving the engine's own NSLobbyEntrantAdded handler.
    // This drives its SIBLING -- NSLobbyRemovingEntrant, registered by the same registrar against
    // the same receiver -- so the entrant is torn down by the code that owns tearing entrants down:
    // lobby record, user slot, actor, emote and model state, conn stats, entrant table, user
    // counts.  Nothing here writes an engine field itself.
    //
    // MUST run on the engine thread with a live netGame, for the same reason the add does; the
    // stage-3 tail is where playback calls it from.
    //
    // Five refusals come BEFORE the call and each one is a set this leave must never touch:
    bool ReleaseReplayUser(UserSlot slot) override {
        // Containment first, because everything below destroys an engine object. In a live session
        // these entrants are other people and a leave would disconnect one of them.
        if (!livesafety::Allowed(livesafety::Feature::EntrantRelease))
            return Refuse(Refusal::ReleaseNotContained);
        void* ng = Ng();
        if (!ng) return Refuse(Refusal::NoNetGame);
        if (slot.v >= facts::ENTITY_USER_SLOT_TABLE_MAX) return Refuse(Refusal::SlotOutOfRange);
        // The body is still authored, and still from the same builder, because it is the shape the
        // handler reads: one u64 slot at events_facts::kRemovedSlotOff.  The direct call hands the
        // handler that qword through the trampoline's own argument shape.
        uint8_t body[kEntrantRemovedBodyLen];
        if (!BuildEntrantRemovedBody(slot, body)) return Refuse(Refusal::SlotOutOfRange);
        // THE TARGET-SET PROOF.  Not "the roster says so" -- the ledger this process wrote where an
        // admission of ours provably seated an entrant.
        if (!SlotWasAdmittedHere(slot.v)) return Refuse(Refusal::NotOurEntrant);
        // An image where the sibling handler does not anchor keeps the OLD answer by name: the body
        // is authored and there is nowhere to hand it.  Adoption is the fallback, as it was.
        if (!LeaveHandlerBound()) return Refuse(Refusal::NoReleasePath);

        // ★ THE CROSS-CHECK THE ENGINE'S OWN HANDLER DOES NOT DO, and the reason it is here.  The
        // handler takes the slot table's word at face value: `index = slotTable[slot]; if (index <
        // userCount && index >= localCount) teardown(index)`.  The teardown's own compaction leaves
        // a removed slot's word naming an index PAST the user count -- harmless until a later
        // admission grows the count back over it, at which point that word names somebody else.
        // So the slot is only a target while the entrant it names NAMES THE SLOT BACK, which is the
        // same pair the admission's postcondition checks.
        UserIndex index(kInvalidU16);
        UserSlot  back(kInvalidU16);
        if (!IndexOfSlot(ng, slot, index) || !SlotOfIndex(ng, index, back) || back.v != slot.v) {
            // Nothing is seated there under our name any more: the release is complete, and saying
            // so is not the same as saying a call was made.  Drop the ledger bit -- a slot whose
            // entrant is gone must not keep a claim that would let a later leave fire at whatever
            // the engine puts there next.
            ForgetAdmittedSlot(slot.v);
            return Refuse(Refusal::None);
        }
        // The engine's handler refuses the local-user band silently.  Refuse it first and by name:
        // the viewer lives there, and an admission of ours landing in it would be a bug this says
        // out loud instead of feeding to a teardown.
        if (!Readable(At(ng, facts::LOCAL_NETGAME_LOCAL_USER_COUNT_OFF), sizeof(uint16_t)))
            return Refuse(Refusal::RosterUnreadable);
        if (index.v < Rd<uint16_t>(ng, facts::LOCAL_NETGAME_LOCAL_USER_COUNT_OFF))
            return Refuse(Refusal::EntrantIsLocal);

        // ENG-RELOAD-3 points (b) and (c): the alias table either side of the engine's own removal,
        // which is where the pool free -- and with it the only re-key a pooled record ever gets --
        // happens.  A row that is correct before and gone after names the leave; a row that never
        // reaches generation+1 names the re-key.
        LogActorAliasRows(ng, "before-leave", slot.v);
        const bool removed = DirectEntrantRemovedCall(slot.v);
        LogActorAliasRows(ng, "after-leave", slot.v);
        // Immediately, not at the next load: between the unload and the load the
        // freed actor's orphaned script graph is still subscribed and still asks for its own SELF
        // key at the new generation.  Idempotent, and silent unless an entry actually lagged.
        ReportActorRecordLag(ng, "post-leave");
        if (!removed) return Refuse(Refusal::ReleaseCallRefused);

        // ★ THE POSTCONDITION, asked of the engine and not of the call's return value.  Same read
        // as above: the slot is free when nothing in the user table names it back.
        UserIndex after(kInvalidU16);
        UserSlot  afterBack(kInvalidU16);
        if (IndexOfSlot(ng, slot, after) && SlotOfIndex(ng, after, afterBack) &&
            afterBack.v == slot.v)
            return Refuse(Refusal::EntrantStillSeated);
        // ★ The engine's handler frees the pooled actor but leaves its guid behind in
        // the pawn records, and the next load's admission hands that dead guid straight to a resolve
        // that cannot fail safely.  Run after every leave rather than once at the end: each leave
        // compacts, so the set of vacant records grows by one and the sweep is idempotent over the
        // ones it already restored.  A/B off (playback.clear_actor_on_leave=0) reproduces the fault.
        if (config::Interface()->GetBool("playback.clear_actor_on_leave", true)) {
            const uint32_t cleared = ClearVacantPawnActorGuids(ng);
            if (cleared > 0) {
                log::Direct(LogLevel::Info, "pawn",
                            "slot %u left: %u vacant pawn record(s) restored to 'no actor'",
                            unsigned(slot.v), cleared);
            }
        }
        ForgetAdmittedSlot(slot.v);
        return Refuse(Refusal::None);
    }

    bool SetProfileJson(UserSlot slot, const Blob& json) override {
        void* ng = Ng();
        if (!ng) return Refuse(Refusal::NoNetGame);
        if (!BuildMatches()) return Refuse(Refusal::BuildMismatch);
        UserIndex index(0);
        if (!IndexOfSlot(ng, slot, index)) return Refuse(Refusal::NoSuchEntrant);
        void* doc = ProfileJson(ng, index);
        if (!doc || !ProfileJsonUsable(ng, index)) return Refuse(Refusal::EmptyProfile);
        if (!json.p || json.n == 0) return Refuse(Refusal::EmptyProfile);
        // L3 #19 F2, refused before it is spent: the engine's setters write only into a
        // document whose IDENT.JSON_CACHED_OFF qword is zero.  A bot entrant carries the
        // cache's own document, so the walk below would log one `json db is cached, read
        // only` line per key and change nothing -- and answer success, because both setters
        // return void.
        if (!ProfileDocWritable(doc)) {
            SetDetail("the engine holds this entrant's profile document in its json db cache: every key is refused `json db is cached, read only` and nothing would be applied");
            return Refuse(Refusal::ProfileReadOnly);
        }

        const ProfileScalarSink sink{doc, &SinkSetString, &SinkSetInt};
        const ProfileWalkStats w = WalkProfileScalars(json, sink);
        arrays_ = w.arrays;
        memcpy(firstArray_, w.firstArray, sizeof firstArray_);
        if (!w.wellFormed) {
            // Whatever was set before the malformed byte stays set: the engine's document is
            // still valid, it is our recording that is not, and saying so beats reverting.
            SetDetail("the recorded profile is not well-formed JSON; the scalars before the fault were applied");
            return Refuse(Refusal::EmptyProfile);
        }
        scalars_ = w.scalars;
        return Refuse(Refusal::None);
    }

    // ★ THE RAW WRITE IS RETIRED (ARCH-23 S14.5).  This used to poke the packed role word with a
    // bare Wr<uint16_t>: no IDENT.SERVER_ASSIGN_TEAM, so no NETGAME.USER_SETTINGS_APPLY, no
    // participating/spectating dispatch, no per-team count maintenance and no delegate.  Its cost
    // was silent and total: a caller that reached the recorded team this way left the word already
    // reading it, A3's own early-out then SKIPPED the engine call, and the entrant kept a
    // participating role with a disabled pawn -- permanently, with every hop reading plausible.
    //
    // So the team now travels the engine's own path, and a role the engine will not take is
    // refused by name rather than written behind its back.  `teamSlot` is the engine's to derive:
    // USER_SETTINGS_APPLY maintains the per-team counts and the slot within them, and a caller's
    // slot is reported back as unapplied rather than forced.
    bool SetTeam(UserIndex index, uint8_t teamId, uint8_t teamSlot) override {
        void* ng = Ng();
        if (!ng) return Refuse(Refusal::NoNetGame);
        if (index.v >= Rd<uint16_t>(ng, kENTRANT_COUNT_OFF)) return Refuse(Refusal::NoSuchEntrant);
        // A team the RECORDING never captured is a different answer from a team
        // the engine will not take, and the difference is load-bearing: the caller keeps dressing
        // on the first and stops on the second.  The sentinel is the manifest's own -- the 8-bit
        // team field is written as the low byte of kPlayerNoTeam16 for "absent" -- so it is
        // derived from that constant here rather than spelled again.
        if (teamId == uint8_t(kPlayerNoTeam16)) return Refuse(Refusal::TeamNotRecorded);
        if (teamId >= uint8_t(facts::NETGAME_USER_ROLE_BOUND))
            return Refuse(Refusal::TeamNotApplied);
#ifdef W2P1_MUTATE_SETTEAM_RAW
        const uint64_t off = UserOff(index.v);
        uint16_t word = Rd<uint16_t>(ng, off);
        word = uint16_t((word & ~(uint16_t(kUSER_TEAM_MASK) << kUSER_TEAM_SHIFT)) |
                        (uint16_t(teamId & kUSER_TEAM_MASK) << kUSER_TEAM_SHIFT));
        word = uint16_t((word & ~(uint16_t(kUSER_TEAMSLOT_MASK) << kUSER_TEAMSLOT_SHIFT)) |
                        (uint16_t(teamSlot & kUSER_TEAMSLOT_MASK) << kUSER_TEAMSLOT_SHIFT));
        Wr<uint16_t>(ng, off, word);
        return Refuse(Refusal::None);
#else
        (void)teamSlot;
        // This is the SECOND site in the tree that issues
        // IDENT.SERVER_ASSIGN_TEAM, it runs on the same stage-0 tick as the hop chain (the dress
        // is immediately before Pump), and until now it did neither of the two things that make a
        // crash block readable.  The marker names THIS call, so a `lastmarker=` cannot be the hop
        // chain's stale one; the latch is the chain's own, so an engine callback that re-entered
        // us here is refused by name instead of starting a second transition inside the first.
        PawnProvision::EngineCallLatch latch;
        if (!latch.taken) return Refuse(Refusal::EngineCallReentrant);
#ifndef W2P1_MUTATE_SETTEAM_NO_MARKER
        PawnProvision::MarkHop(0, "dress assignTeam", &surface_, index.v);
#endif
        if (!HopAssignTeam(&surface_, index.v, teamId)) return Refuse(Refusal::TeamNotApplied);
        // The engine's own word, re-read.  A call that the session type refused is silent, and
        // this is what catches it.
        if (::replay::identity_access::TeamOf(ng, index) != teamId)
            return Refuse(Refusal::TeamNotApplied);
        return Refuse(Refusal::None);
#endif
    }

    // Bounded by the entrant count SetTeam bounds itself with, which IS the span proof: it is the
    // bound ENGINE.SPAWN_POINT_AUTHOR_VA relies on when it indexes netgame + 376 + 592*index for
    // this same word.  The shift/mask pair is not copied a third time -- the free readers own it.
    bool TeamOf(UserIndex index, uint8_t& teamId, uint8_t& teamSlot) override {
        void* ng = Ng();
        if (!ng) return Refuse(Refusal::NoNetGame);
        if (index.v >= Rd<uint16_t>(ng, kENTRANT_COUNT_OFF)) return Refuse(Refusal::NoSuchEntrant);
#ifdef W2C_MUTATE_TEAM_WRONG_FIELD
        teamId = ::replay::identity_access::TeamSlotOf(ng, index);
#else
        teamId = ::replay::identity_access::TeamOf(ng, index);
#endif
        teamSlot = ::replay::identity_access::TeamSlotOf(ng, index);
        return Refuse(Refusal::None);
    }

    bool ApplyLoadout(UserIndex index, const Symbol64 slots[20],
                      uint16_t number, uint16_t numberBody) override {
        void* ng = Ng();
        if (!ng) return Refuse(Refusal::NoNetGame);
        if (!BuildMatches()) return Refuse(Refusal::BuildMismatch);
        if (!slots) return Refuse(Refusal::NoLoadoutInstance);
        if (!ProfileJsonUsable(ng, index)) return Refuse(Refusal::EmptyProfile);
        if (!CosmeticsWouldApply(ng)) return Refuse(Refusal::NotAuthoritative);

        UserSlot slot(0);
        if (!SlotOfIndex(ng, index, slot)) return Refuse(Refusal::NoSuchEntrant);
        uint32_t netUserId = 0;
        if (!NetUserId(ng, slot, netUserId)) return Refuse(Refusal::NoSuchEntrant);

        LoadoutHeader* hdr = LiveLoadout(ng, slot);
        if (!hdr) return Refuse(Refusal::NoLoadoutInstance);

        // THE TWO JERSEY NUMBERS FIRST, AND THEY DO NOT NEED AN INSTANCE (NAMES-4).  They live in
        // the per-slot header the engine prints as `Loadout Number` (+0x38, read at
        // netgame+0x51420+64*slot+0x38 by sub_1401CD1A0), not inside the instance array that
        // carries the twenty item ids.  Written below the instance gate they were lost for every
        // entrant the recording has no loadout document for -- which is four of the five rows of
        // the subject capture -- and the body wore no number plate at all.
        hdr->number     = number;
        hdr->numberBody = numberBody;

        if (hdr->instanceCount == 0 || !hdr->instances)
            return Refuse(Refusal::NoLoadoutInstance);

        // The instance is the engine's own, allocated by the engine, and only the item ID
        // half of each 16-byte pair is rewritten.  Nothing is allocated or freed here, so
        // ownership of the vector never changes hands -- which is why this is a Level 0 data
        // write and not a heap intervention.  The slot IDs stay as the catalogue laid them
        // out, and the recording's 20 ids are positional against that same order.
        LoadoutInstance* inst = static_cast<LoadoutInstance*>(hdr->instances);
        applied_ = 0;
        if (inst->items) {
            const uint64_t n = inst->itemCount < kLoadoutFormatMax ? inst->itemCount
                                                                  : kLoadoutFormatMax;
            // The write rule lives in WriteRecordedItems, which is drivable without a game.
            (void)WriteRecordedItems(*inst, slots, n);
            applied_ = uint32_t(n);
        }

        const void* guid = At(ng, uint64_t(kUSER_ACTOR_GUID_OFF) +
                                  uint64_t(facts::USER_STRIDE) * index.v);
        if (!CallApplyLoadoutToActor(ng, netUserId, inst->name, 0, guid, 0))
            return Refuse(Refusal::BuildMismatch);

        if (applied_ < 20) {
            SetDetail("the engine's loadout instance is shorter than the recorded twenty ids");
            return Refuse(Refusal::ShortInstance);
        }
        return Refuse(Refusal::None);
    }

    bool OverrideAlignment(UserIndex index, uint16_t alignment) override {
        void* ng = Ng();
        if (!ng) return Refuse(Refusal::NoNetGame);
        if (!BuildMatches()) return Refuse(Refusal::BuildMismatch);
        ActorComponentRef ref;
        if (!ResolveComponent(ng, index, ref)) return Refuse(Refusal::NoActorComponent);
        const uint16_t handle = uint16_t(ref.handle);
        // Both calls, in this order, and neither is redundant.  OverrideAlignment stores the
        // value and re-runs only when the EFFECTIVE alignment changed; ApplyAlignmentChange
        // re-runs unconditionally.  Issuing only the first silently does nothing whenever the
        // override already held that value, which is exactly the side-swap case that matters.
        if (!CallOverrideAlignment(ref.componentSystem, ref.handle, alignment) ||
            !CallApplyAlignmentChange(ref.componentSystem, ref.handle))
            return Refuse(Refusal::BuildMismatch);
        const uint16_t now = CallCustomisationAlignment(ref.componentSystem, handle);
        if (now != alignment) {
            SetDetail("the customisation component reports a different alignment than the one requested");
            return Refuse(Refusal::NoActorComponent);
        }
        return Refuse(Refusal::None);
    }

    bool ReadAppliedItems(UserIndex index, Symbol64 out[20]) const override {
        if (!out) return false;
        memset(out, 0, sizeof(Symbol64) * 20);
        void* ng = netGame_ ? netGame_() : nullptr;
        if (!ng) return Refuse(Refusal::NoNetGame);
        if (!BuildMatches()) return Refuse(Refusal::BuildMismatch);
        ActorComponentRef ref;
        if (!ResolveComponent(ng, index, ref)) return Refuse(Refusal::NoActorComponent);
        uint64_t count = 0;
        CustSlotEntry* slots = CustSlots(ref, count);
        if (!slots) return Refuse(Refusal::NoActorComponent);
        const uint64_t n = count < 20 ? count : 20;
        for (uint64_t i = 0; i < n; ++i) out[i] = slots[i].item;
        return Refuse(Refusal::None);
    }

    UserSlot SlotOf(UserIndex index) const override {
        void* ng = netGame_ ? netGame_() : nullptr;
        UserSlot s(kInvalidU16);
        SlotOfIndex(ng, index, s);
        return s;
    }

    UserIndex IndexOf(UserSlot slot) const override {
        void* ng = netGame_ ? netGame_() : nullptr;
        UserIndex i(kInvalidU16);
        IndexOfSlot(ng, slot, i);
        return i;
    }

    void*       NetGame() const { return Ng(); }
    Refusal     Last() const { return last_; }
    uint32_t    Scalars() const { return scalars_; }
    uint32_t    Applied() const { return applied_; }
    const char* Detail() const { return detail_; }
    uint32_t    Arrays() const { return arrays_; }
    const char* FirstArray() const { return firstArray_; }

private:
    void* Ng() const { return netGame_ ? netGame_() : nullptr; }

    bool RefuseProvision(PawnProvision::Refusal r) {
        lastProvision_ = r;
        return r == PawnProvision::Refusal::None;
    }

    bool Refuse(Refusal r) const {
        last_ = r;
        if (r == Refusal::None) detail_[0] = '\0';
        return r == Refusal::None;
    }

    void SetDetail(const char* s) const {
        const size_t n = strlen(s) < sizeof(detail_) - 1 ? strlen(s) : sizeof(detail_) - 1;
        memcpy(detail_, s, n);
        detail_[n] = '\0';
    }

    void* (*netGame_)() = nullptr;
    ActorHopSurface surface_;

    // The provision half's own word, kept beside the netgame source because it is set on the same
    // calls.  It starts at NotAttempted rather than None: nobody has asked yet, and that is not the
    // same statement as an admission that succeeded.
    PawnProvision::Refusal lastProvision_ = PawnProvision::Refusal::NotAttempted;
    mutable Refusal last_ = Refusal::None;
    mutable char    detail_[192] = "";
    uint32_t        scalars_ = 0;
    uint32_t        applied_ = 0;
    uint32_t        arrays_ = 0;
    char            firstArray_[kMaxKey] = "";
};

// THE DRESS, RUN INSIDE THE ADMISSION.  Two engine calls and nothing authored here: the recorded
// server profile goes into the entrant's own CJson through SetProfileJson, and the recorded 168
// bytes of loadout ids go onto the engine's own loadout instance through ApplyLoadout -- the same
// two members the dress ladder calls, so there is one implementation of each and W2-C's checks
// still describe what happened.
//
// ApplyLoadout's answer is deliberately NOT the gate.  It refuses ShortInstance when the engine's
// instance holds fewer than twenty ids, which is a fidelity finding for W2-C's report and not a
// reason to leave an entrant with no actor; and it refuses NoLoadoutInstance on exactly the count
// this function's caller is about to read for itself, by name.  What must be true for the actor
// step to be safe is the count, and the count is read off the engine below.
// THE STEP THE DIRECT INSERT SKIPS, and the reason L3 #18's entrant read `Loadouts : 0`.
//
// IDENT.ADMIT_USER_VA calls GenerateBotProfile (sub_14016D960) unconditionally at 0x140152660, so
// the entrant DOES get a profile -- but GenerateBotProfile writes it into the USER RECORD at
// CR15NetGame + USER.PROFILE_OFF + USER.PROFILE_STRIDE*index, whose loadout container sits at
// + USER.LOADOUT_OFF.  What IDENT.APPLY_LOADOUT_ITEMS reads, and what the engine's entrant dump
// prints as `Loadouts : %llu`, is a DIFFERENT container: the per-slot one at
// IDENT.LOADOUT_TABLE_OFF + (slot << IDENT.LOADOUT_ENTRY_SHIFT).  The engine bridges the two with
// IDENT.LOADOUT_ASSIGN -- the container's own copy-assign -- and it runs that call in
// sub_14017B190, the add-entrant handler ABOVE the insert, on both of its arms.  This plugin calls
// the insert directly and so never reached it: the profile was generated and never published, and
// the per-slot count stayed 0 for the actor step to fault on.
//
// So this is not an authored write.  It is the engine's own step, with the engine's own two
// addresses, run in the engine's own order, and it is the one thing between a bot entrant with a
// profile and a bot entrant the customisation walk can read.
using FnLoadoutAssign = void* (*)(void* dst, const void* src);

bool PublishLoadout(void* ng, uint16_t slot, uint16_t index) {
    const FnLoadoutAssign assign = Guarded<FnLoadoutAssign>(
        facts::IDENT_LOADOUT_ASSIGN, facts::IDENT_LOADOUT_ASSIGN_PROLOGUE, 0);
    if (assign == nullptr) return false;

    void* dst = LiveLoadout(ng, UserSlot(slot));
    // The SOURCE is indexed by SLOT, exactly as both engine sites index it -- see LoadoutContainer.
    // `index` is still this function's parameter because the entrant index is what the caller has;
    // it is now only used to name the entrant, never to address memory.
    void* src = LoadoutContainer(ng, UserSlot(slot));
    (void)index;
    if (!dst || !src) return false;
    if (!Readable(dst, sizeof(LoadoutHeader))) return false;
    // The source is the entrant's OWN generated profile. An empty one is not an error here -- the
    // count read after this decides -- so a copy that publishes nothing still reports true.
    assign(dst, src);
    return true;
}

// THE READ-BACK HOOK.  Separate from HookApplyProfile on purpose: the write's answer and the
// engine's answer are two different facts, and L3 #19 is what happens when the first is read
// as the second.  This one asks the engine.
bool HookProfileApplied(void* ctx, uint16_t slot, uint16_t index) {
    AdmitCtx* c = static_cast<AdmitCtx*>(ctx);
    (void)slot;
    if (!c->desc) return false;
    return ProfileAppliedReadBack(c->ng, UserIndex(index), c->desc->entrantJson);
}

// THE ATTACH HOOK.  It runs before the dress and it is what makes the dress possible at all: a
// bot entrant is handed the json db CACHE's own document (IDENT.PROFILE_CACHE_ATTACH_VA), whose
// IDENT.JSON_CACHED_OFF qword is non-zero, and every setter refuses it.  This replaces it with a
// document parsed out of the recording, exactly as IDENT.PROFILE_RECEIVE_VA replaces it with the
// one a join reply carried.
PawnProvision::Refusal HookAttachProfile(void* ctx, uint16_t slot, uint16_t index) {
    AdmitCtx* c = static_cast<AdmitCtx*>(ctx);
    (void)slot;
    if (!c->desc) return PawnProvision::Refusal::ProfileAttachFailed;
    switch (AttachProfileDocument(c->ng, UserIndex(index), c->desc->entrantJson)) {
        case AttachResult::Ok:           return PawnProvision::Refusal::None;
        case AttachResult::ParseFailed:  return PawnProvision::Refusal::ProfileParseFailed;
        case AttachResult::AttachFailed: break;
    }
    return PawnProvision::Refusal::ProfileAttachFailed;
}

// THE DEFAULT-LOADOUT PUBLISH HOOK.  The bot path's own step, independent of
// the recorded-loadout overlay: it publishes the DEFAULT loadout AddBotUser generated into the user
// record across to the per-slot live table, so the crash guard sees a non-empty count and the actor
// step runs.  No recorded dress, no profile write -- just the engine's own copy (PublishLoadout ->
// IDENT.LOADOUT_ASSIGN), which is why it needs no desc and runs whether or not the overlay is armed.
bool HookPublishLoadout(void* ctx, uint16_t slot, uint16_t index) {
    void* const ng = static_cast<AdmitCtx*>(ctx)->ng;
    const bool ok = PublishLoadout(ng, slot, index);
    // NAME-ALIGN (same alias the remote dress ladder's L4 applies, DIAG-SEAT-DRIVE): the engine's
    // enable-time apply selects the instance named *IDENT.LOADOUT_DEFAULT_INSTANCE_PTR ("unified"),
    // and the raw copy above published the recording's instance under ITS own name.  Alias the sole
    // populated instance onto the engine name in the live table so the phase-16 model-build finds a
    // non-empty loadout instead of applying nothing.  A no-op when the name already matches, when the
    // instance is empty (the legacy bot_publish_loadout path's empty source), or when it did not
    // resolve, so it never harms the guaranteed-default path.
    if (ok) (void)AlignPublishedInstanceName(LiveLoadout(ng, UserSlot(slot)),
                                             LoadoutDefaultInstanceSymbol());
    return ok;
}

// THE RECORDED-LOADOUT SOURCE FILL.  It reuses LoadoutPhase::Source EXACTLY --
// the same DressLoadout(..., LoadoutPhase::Source) the remote route drives -- so the SOURCE container
// is filled by the engine's OWN loadout parser (IDENT.LOADOUT_FROM_PROFILE, eng.parse), never by a
// forked copy.  It runs on the bot path AFTER AddBotUser (whose GenerateBotProfile clears+reparses
// that same container into the empty default) and BEFORE HookPublishLoadout copies SOURCE->LIVE, so
// the recording's instances are what the live table -- and the phase-16 model-build over it -- read.
// The document is the recorded server profile (desc.entrantJson), the same bytes the remote route
// parses (recorded_loadout.cpp: out.document == the profile view) and the same the bot overlay's
// AttachProfileDocument uses; L2 (attach) is skipped on the Source phase, so nothing here needs an
// admitted entrant and the parsed document is always released.  Best-effort: false on any refusal,
// and AdmitIntoLobbySlot lets the guard defer on an empty SOURCE (no deadlock, no body).
bool HookSourceLoadout(void* ctx, uint16_t slot, uint16_t index) {
    AdmitCtx* c = static_cast<AdmitCtx*>(ctx);
    if (c == nullptr || c->ng == nullptr || c->id == nullptr || c->desc == nullptr) return false;

    const PawnProvision::ActorHops* hops = c->id->ActorHops();
    if (hops == nullptr) return false;
    LoadoutEngine eng{};
    if (!BindLoadoutEngine(eng, *hops)) return false;

    // ENG-BOTROUTE-FIX (Change A).  Bind the IDENTITY-BLOCK loadout document the pawn layer plumbed in
    // (recordedLoadoutDocument -- the exact {rl.document, rl.documentBytes} the remote route parses to
    // one real instance), NOT the manifest entrantJson, which the engine parser fills to ZERO loadout
    // instances (DIAG-BOTROUTE-PARSE).  The two channels come from the SAME recording; feeding the
    // identity-block bytes makes this Source parse byte-identical to the remote route's proven
    // `instances 1`.  Fall back to entrantJson only when no identity-block document was carried (feature
    // off or no present profile), which reproduces today's safe defer-on-empty behaviour.
    const Blob& sourceDoc = c->desc->recordedLoadoutDocument.n != 0
                                ? c->desc->recordedLoadoutDocument
                                : c->desc->entrantJson;
    const bool  fromIdentityBlock = c->desc->recordedLoadoutDocument.n != 0;

    entity_json::CJsonReader parsed{};
    if (!BindLoadoutDocument(parsed, sourceDoc)) return false;
    eng.doc = &parsed;

    // The container's header pair L3 verifies the parse against.  Sourced from the descriptor the
    // remote route sources it from (JoinDesc::loadoutNumber/loadoutNumberBody/alignment); the Source
    // phase reads no recorded pairs, so only these three are needed.
    RecordedLoadout rec{};
    rec.number     = c->desc->loadoutNumber;
    rec.numberBody = c->desc->loadoutNumberBody;
    rec.alignment  = c->desc->alignment;
    // Reflect the bound document's presence the way the remote route does (pawn_provision.cpp:2542),
    // gated strictly on the bytes actually bound above -- no document, no claim -- so an absent profile
    // still refuses at L1 (DocumentNotRecorded) rather than fabricating a loadout.  BindLoadoutDocument
    // already returned false above on an empty blob, so sourceDoc.n is non-zero once we reach here.
    rec.documentRecorded = sourceDoc.n != 0;

    LoadoutSteps steps{};
    const LoadoutRefusal why =
        DressLoadout(c->ng, UserSlot(slot), UserIndex(index), rec, eng, steps, LoadoutPhase::Source);

    // The launch signal: the count the SOURCE now holds is the one HookPublishLoadout copies into the
    // live table and the phase-16 model-build reads.  > 0 with `parsed None` is the working-body
    // precondition; a refusal here means the guard below will defer (no deadlock, no body).
    char inst[24];
    if (steps.instances == kLoadoutInstancesUnreadable)
        _snprintf_s(inst, sizeof(inst), _TRUNCATE, "unread");
    else
        _snprintf_s(inst, sizeof(inst), _TRUNCATE, "%llu", (unsigned long long)steps.instances);
    // The doc channel + length make the byte trace verifiable at launch -- the
    // identity-block document should be the same length the remote route parses to `instances 1`
    // (~1671 B), never the manifest entrantJson that parses to zero.  The FNV-1a checksum of the bound
    // bytes is the content signal: the same document keyed to the same recorded slot hashes the same on
    // both routes, so the launch shows numerically that this is the identity-block content, not a
    // coincidental second document of matching length.
    uint32_t docHash = 2166136261u;
    for (uint32_t i = 0; i < sourceDoc.n; ++i) {
        docHash = (docHash ^ sourceDoc.p[i]) * 16777619u;
    }
    log::Direct(LogLevel::Info, "pawn",
                "bot slot %u: ENG-BOTROUTE-FIX recorded loadout parsed into SOURCE %s -- source "
                "instances %s [doc=%s %u B hash=%08x] (numberBody %u; copied into the live table by "
                "HookPublishLoadout before the guard + SET_PLAYER_ACTOR)",
                unsigned(slot), LoadoutRefusalName(why), inst,
                fromIdentityBlock ? "identity-block" : "manifest-fallback", unsigned(sourceDoc.n),
                docHash, unsigned(rec.numberBody));

    // The Source phase never attaches (L2 skipped), so this module still owns the document.
    ReleaseLoadoutDocument(parsed);
    return why == LoadoutRefusal::None;
}

bool HookApplyProfile(void* ctx, uint16_t slot, uint16_t index) {
    AdmitCtx* c = static_cast<AdmitCtx*>(ctx);
    if (!c->id || !c->desc) return false;
    // ★ ADVISORY, not a gate.  PublishLoadout refuses LoadoutDocumentNotRecorded whenever the
    // recorder could not capture the entrant's profile DOCUMENT -- four of the five players in the
    // subject capture -- and returning here skipped everything after it.  But ApplyLoadout writes
    // the twenty item ids and BOTH jersey numbers straight into the engine's own live loadout
    // header, and it needs no source document to do it: that is the write whose absence renders as
    // `00` on the roster row and on the minimap dot.  So the publish is attempted, its answer is
    // carried out to the caller, and the writes that do not depend on it still run.
    // SetProfileJson refuses cleanly on an empty blob (Refusal::EmptyProfile) and is likewise not
    // allowed to stop the numbers.
    const bool published = PublishLoadout(c->ng, slot, index);
    const bool profiled = c->id->SetProfileJson(UserSlot(slot), c->desc->entrantJson);
    // Jersey number 0 is a REAL number -- one of this capture's five players wears it -- so there
    // is no value test here and there must never be one: absence is a property of the recording,
    // carried by the record's own fill bits, never of the number being zero.
    (void)c->id->ApplyLoadout(UserIndex(index), c->desc->loadoutSlots, c->desc->loadoutNumber,
                              c->desc->loadoutNumberBody);
    // The ANSWER is unchanged -- the bot route turns it into Refusal::ProfileNotApplied and this
    // change is about what gets written, not about which slots that route gives up on.  Only the
    // early return is gone.
    return published && profiled;
}

// The number the engine itself prints as `Loadouts : N`, read where the engine reads it.  Span-
// proved first: an unprovable table answers 0 and the caller refuses LoadoutsEmpty, which is the
// safe direction -- the alternative to refusing is asking for an actor, and that is the call that
// took the process down at L3 #18.  The refusal's own sentence names both causes, so a reader is
// never told "the count was zero" about memory that was never read.
uint64_t HookLoadoutCount(void* ctx, uint16_t slot) {
    AdmitCtx* c = static_cast<AdmitCtx*>(ctx);
    LoadoutHeader* hdr = LiveLoadout(c->ng, UserSlot(slot));
    if (!hdr || !Readable(hdr, sizeof(LoadoutHeader))) return 0;
    // Clamped for the same reason the walks are: this number is handed to a caller that gates the
    // actor refresh on it, and a garbage count must not read as a very well dressed entrant.
    const uint64_t n = hdr->instanceCount;
    return n > kLoadoutFormatMax ? kLoadoutFormatMax : n;
}


Identity gInstance{nullptr, nullptr};
bool     gLive = false;

}  // namespace

IIdentity* Create(void* (*netGame)(), void* (*remotePlayerCs)()) {
    if (gLive) return nullptr;
    gInstance = Identity(netGame, remotePlayerCs);
    gInstance.RebindHops();
    gLive = true;
    return &gInstance;
}

void Destroy(IIdentity* p) {
    if (p == &gInstance) gLive = false;
}

namespace {

// IIdentity is B.11.3's frozen interface and is meant to be implemented by fakes as well as by
// this module, so a static_cast down to Identity is undefined behaviour for anything else.
// Create() hands out exactly one instance, so an address comparison settles it exactly and a
// foreign IIdentity gets a clean `false` instead of a reinterpreted vtable.
const Identity* Own(const IIdentity* p) {
    // Through the wrapper, once.  Each wrapper's Underlying() forwards to its own inner, so one
    // hop reaches the end of any chain of them, and a bare implementation answers itself.
    const IIdentity* self = (p != nullptr) ? p->Underlying() : nullptr;
    return (self == &gInstance) ? &gInstance : nullptr;
}

}  // namespace

void* NetGameOf(const IIdentity* p) {
    const Identity* self = Own(p);
    return self ? self->NetGame() : nullptr;
}

Refusal LastRefusal(const IIdentity* p) {
    const Identity* self = Own(p);
    return self ? self->Last() : Refusal::None;
}

const char* LastDetail(const IIdentity* p) {
    const Identity* self = Own(p);
    return self ? self->Detail() : "";
}

uint32_t UncarriedArrayCount(const IIdentity* p) {
    const Identity* self = Own(p);
    return self ? self->Arrays() : 0;
}

const char* FirstUncarriedArray(const IIdentity* p) {
    const Identity* self = Own(p);
    return self ? self->FirstArray() : "";
}

uint32_t CarriedScalarCount(const IIdentity* p) {
    const Identity* self = Own(p);
    return self ? self->Scalars() : 0;
}

uint32_t AppliedItemCount(const IIdentity* p) {
    const Identity* self = Own(p);
    return self ? self->Applied() : 0;
}

bool ReadEngineIdentity(const IIdentity* p, UserIndex index, EngineIdentity& out) {
    memset(&out, 0, sizeof out);
    out.number = out.numberBody = kInvalidU16;
    out.teamId = out.teamSlot = uint8_t(0xFF);
    void* ng = NetGameOf(p);
    if (!ng) return false;

    out.nameRead = ResolvedDisplayName(ng, index, out.name, sizeof out.name);

    UserSlot slot(kInvalidU16);
    if (SlotOfIndex(ng, index, slot)) {
        out.numbersRead = LoadoutNumbers(ng, slot, out.number, out.numberBody, nullptr);
        const uint64_t n = LiveLoadoutInstances(ng, slot);
        out.instancesRead = n != kLoadoutInstancesUnreadable;
        if (out.instancesRead) out.instances = n;
    }

    // The entrant-count bound the team readers are only defined inside; below it they would read
    // the flags word of whatever follows the user array.
    if (index.v < Rd<uint16_t>(ng, kENTRANT_COUNT_OFF)) {
        out.teamId   = TeamOf(ng, index);
        out.teamSlot = TeamSlotOf(ng, index);
        out.teamRead = true;
    }
    return true;
}

bool ReadProfileString(const IIdentity* p, UserIndex index, const char* key,
                       char* out, size_t cap) {
    return ReadProfileString(NetGameOf(p), index, key, out, cap);
}

Refusal SetSlotName(const IIdentity* p, UserSlot slot, const char* name) {
    return SetSlotName(NetGameOf(p), slot, name);
}

Refusal AttachProfile(void* ng, UserIndex index, const Blob& recordedJson) {
    if (!ng) return Refusal::NoNetGame;
    if (!recordedJson.p || recordedJson.n == 0) return Refusal::ProfileNotRecorded;
    switch (AttachProfileDocument(ng, index, recordedJson)) {
        case AttachResult::Ok:          return Refusal::None;
        case AttachResult::ParseFailed: return Refusal::ProfileNotParsed;
        case AttachResult::AttachFailed: break;
    }
    return Refusal::ProfileNotAttached;
}

Refusal AttachProfile(const IIdentity* p, UserIndex index, const Blob& recordedJson) {
    return AttachProfile(NetGameOf(p), index, recordedJson);
}

bool ResolvedDisplayName(const IIdentity* p, UserIndex index, char* out, size_t cap) {
    return ResolvedDisplayName(NetGameOf(p), index, out, cap);
}

// ------------------------------------------------------------------------------------------
// THE LIVE SCOREBOARD.  The read chain is the /session writer's, step for step:
//
//   slot  = *(u16*)(ng + USER.ARRAY_OFF + USER.STRIDE*index) & IDENT.USER_SLOT_MASK   (SlotOfIndex)
//   stat  = ng + USER.PROFILE_STRIDE*slot + STATS.<NAME>_{KIND,VALUE,COUNT}_OFF
//   value = kind == ratio ? (count ? value/count : ABSENT) : value
//
// and for ping,  sub_1401B2EA0 -> sub_1406084D0:
//
//   peers = *(void**)(ng + 0x40) + 0x18
//   ping  = slot < *(u64*)(peers+0x368) ? *(u16*)(*(void**)(peers+0x360) + 0xD8*slot + 0x8A)
//                                       : ABSENT   (the accessor returns a bare 0 there)

namespace {

// Seven STATS.* rows the registry now decodes from their anchors (imm8/disp8/imm32/disp32), so the
// values come from the generated header and the anchor bytes beside each one say where they live.
constexpr uint32_t kStatKindRatio = facts::STATS_KIND_RATIO;     // 41 83 3F 05  cmp dword ptr [r15],5 @0x140156A03
constexpr uint32_t kPeerObjOff    = facts::STATS_PEER_OBJ_OFF;   // 48 8B 48 40  mov rcx,[rax+40h]     @0x1401B3D74
constexpr uint32_t kPeerObjAdj    = facts::STATS_PEER_OBJ_ADJ;   // 48 83 C1 18  add rcx,18h           @0x1401B3D81
constexpr uint32_t kPeerCountOff  = facts::STATS_PEER_COUNT_OFF; // 48 3B 91 68030000                  @0x1406084D0
constexpr uint32_t kPeerTableOff  = facts::STATS_PEER_TABLE_OFF; // 48 8B 81 60030000                  @0x1406084D9
constexpr uint32_t kPeerStride    = facts::STATS_PEER_STRIDE;    // 48 69 D2 D8000000                  @0x1406084E0
constexpr uint32_t kPeerPingOff   = facts::STATS_PEER_PING_OFF;  // 0F B7 84 10 8A000000               @0x1406084E7

// packetlossratio, the four arms sub_1401B2EA0 chooses between at 0x1401B3E18-0x1401B3EA1.
constexpr uint32_t kPlrEntrantCountOff = facts::STATS_PLR_ENTRANT_COUNT_OFF;
constexpr uint32_t kPlrLocalCountOff   = facts::STATS_PLR_LOCAL_COUNT_OFF;
constexpr uint32_t kPlrLocalLossOff    = facts::STATS_PLR_LOCAL_LOSS_OFF;
constexpr uint32_t kPlrRoleObjOff      = facts::STATS_PLR_ROLE_OBJ_OFF;
constexpr uint32_t kPlrPeerLossOff     = facts::STATS_PLR_PEER_LOSS_OFF;
constexpr uint32_t kPlrPeerLossStride  = facts::STATS_PLR_PEER_LOSS_STRIDE;
constexpr uint32_t kPlrSvrLossOff      = facts::STATS_PLR_SVR_LOSS_OFF;
constexpr uint32_t kPlrSvrLossStride   = facts::STATS_PLR_SVR_LOSS_STRIDE;

// The three role bits sub_1401B2EA0 tests, in the order it tests them.
constexpr uint8_t kRoleBitServer = 0x02;
constexpr uint8_t kRoleBitClient = 0x04;
constexpr uint8_t kRoleBitHost   = 0x40;

// is_emote_playing: sub_140D08D00 addresses the handle, sub_140128B20 dispatches through it.
constexpr uint32_t kEmoteMapOff     = facts::STATS_EMOTE_MAP_OFF;
constexpr uint32_t kEmotePoolOff    = facts::STATS_EMOTE_POOL_OFF;
constexpr uint32_t kEmoteElemStride = facts::STATS_EMOTE_ELEM_STRIDE;
constexpr uint32_t kEmoteElemAdj    = facts::STATS_EMOTE_ELEM_ADJ;
constexpr uint32_t kEmoteTargetOff  = facts::STATS_EMOTE_TARGET_OFF;
constexpr uint32_t kEmoteArgOff     = facts::STATS_EMOTE_ARG_OFF;
constexpr uint32_t kEmoteVtSlot     = facts::STATS_EMOTE_VTSLOT;

// 42 0F B7 14 80  movzx edx, word ptr [rax+r8*4] @0x140D08D0E -- the scale lives in the SIB byte,
// which no anchor kind encodes, so it stays a literal here with its instruction beside it.
constexpr uint32_t kEmoteMapStride = 4;

struct StatWords {
    uint32_t kind, value, count;
    uint16_t bit;
    int32_t PlayerStats::*dst;   // null for possessionTime, whose value word is a float
};

// The order is the order sub_140155C80 emits them, which is the on-disk bit order too.
constexpr StatWords kStats[] = {
    { facts::STATS_POSSESSION_TIME_KIND_OFF, facts::STATS_POSSESSION_TIME_VALUE_OFF,
      facts::STATS_POSSESSION_TIME_COUNT_OFF, kPsFillPossessionTime, nullptr },
    { facts::STATS_POINTS_KIND_OFF,        facts::STATS_POINTS_VALUE_OFF,
      facts::STATS_POINTS_COUNT_OFF,        kPsFillPoints,        &PlayerStats::points },
    { facts::STATS_GOALS_KIND_OFF,         facts::STATS_GOALS_VALUE_OFF,
      facts::STATS_GOALS_COUNT_OFF,         kPsFillGoals,         &PlayerStats::goals },
    { facts::STATS_STUNS_KIND_OFF,         facts::STATS_STUNS_VALUE_OFF,
      facts::STATS_STUNS_COUNT_OFF,         kPsFillStuns,         &PlayerStats::stuns },
    { facts::STATS_BLOCKS_KIND_OFF,        facts::STATS_BLOCKS_VALUE_OFF,
      facts::STATS_BLOCKS_COUNT_OFF,        kPsFillBlocks,        &PlayerStats::blocks },
    { facts::STATS_ASSISTS_KIND_OFF,       facts::STATS_ASSISTS_VALUE_OFF,
      facts::STATS_ASSISTS_COUNT_OFF,       kPsFillAssists,       &PlayerStats::assists },
    { facts::STATS_SAVES_KIND_OFF,         facts::STATS_SAVES_VALUE_OFF,
      facts::STATS_SAVES_COUNT_OFF,         kPsFillSaves,         &PlayerStats::saves },
    { facts::STATS_INTERCEPTIONS_KIND_OFF, facts::STATS_INTERCEPTIONS_VALUE_OFF,
      facts::STATS_INTERCEPTIONS_COUNT_OFF, kPsFillInterceptions, &PlayerStats::interceptions },
    { facts::STATS_STEALS_KIND_OFF,        facts::STATS_STEALS_VALUE_OFF,
      facts::STATS_STEALS_COUNT_OFF,        kPsFillSteals,        &PlayerStats::steals },
    { facts::STATS_CATCHES_KIND_OFF,       facts::STATS_CATCHES_VALUE_OFF,
      facts::STATS_CATCHES_COUNT_OFF,       kPsFillCatches,       &PlayerStats::catches },
    { facts::STATS_PASSES_KIND_OFF,        facts::STATS_PASSES_VALUE_OFF,
      facts::STATS_PASSES_COUNT_OFF,        kPsFillPasses,        &PlayerStats::passes },
    { facts::STATS_SHOTS_TAKEN_KIND_OFF,   facts::STATS_SHOTS_TAKEN_VALUE_OFF,
      facts::STATS_SHOTS_TAKEN_COUNT_OFF,   kPsFillShotsTaken,    &PlayerStats::shotsTaken },
};
constexpr size_t kStatCount = sizeof(kStats) / sizeof(kStats[0]);

// The span the loop below addresses, computed from the table rather than written down: adding a
// stat moves both ends, and a fact that moves out from under the span cannot be silently missed.
constexpr uint32_t StatSpanLow() {
    uint32_t lo = kStats[0].kind;
    for (size_t i = 1; i < kStatCount; ++i)
        if (kStats[i].kind < lo) lo = kStats[i].kind;
    return lo;
}
constexpr uint32_t StatSpanHigh() {
    uint32_t hi = 0;
    for (size_t i = 0; i < kStatCount; ++i) {
        if (kStats[i].kind + 4u > hi) hi = kStats[i].kind + 4u;
        if (kStats[i].value + 4u > hi) hi = kStats[i].value + 4u;
        if (kStats[i].count + 4u > hi) hi = kStats[i].count + 4u;
    }
    return hi;
}
constexpr uint32_t kStatSpanOff   = StatSpanLow();
constexpr uint32_t kStatSpanBytes = StatSpanHigh() - StatSpanLow();
static_assert(kStatSpanBytes > 0 && kStatSpanBytes < facts::USER_PROFILE_STRIDE,
              "the stat block must sit inside one entrant's slice of the profile array");

// One line per slot: eight players prove eight different bases every tick, and a single shared
// line would miss on all but one of them -- which is the syscall this exists to remove.
entity::SpanProof g_statSpan[facts::ENTITY_USER_SLOT_TABLE_MAX];
entity::SpanProof g_peerHeader;
entity::SpanProof g_peerTable;

// The netGame's own fixed fields the scoreboard reads -- the peer object pointer, both counts and
// the session-role pointer -- are one span from one base, so a sixteen-entrant sweep asks about
// them once and not sixteen times.  Its own line rather than UserTableBound's: that one is the
// roster gate's, its extent is sized by the entrant count, and widening it here would make
// EntrantCount's proved zero-syscall tick a property of this reader's arithmetic.
constexpr uint32_t Widest(uint32_t a, uint32_t b, uint32_t c) {
    return a > b ? (a > c ? a : c) : (b > c ? b : c);
}
constexpr uint32_t kNgStatsBytes = Widest(kPeerObjOff + uint32_t(sizeof(void*)),
                                          kPlrEntrantCountOff + uint32_t(sizeof(uint16_t)),
                                          kPlrRoleObjOff + uint32_t(sizeof(void*)));
static_assert(kPlrLocalCountOff + sizeof(uint16_t) <= kNgStatsBytes,
              "the local-player count must lie inside the span this reader proves");
entity::SpanProof g_ngStats;


// The session-role object is a second base, reached through a pointer in the span above.
entity::SpanProof g_roleObj;

// One line per packetlossratio table.  Each is proved from ITS OWN base with an extent grown to
// the row actually addressed, so the first sweep down the roster grows the extent to the last
// slot and every later sweep is inside what that proved.  Keyed on the table and not on the row:
// a line per row would be a miss on every entrant, which is no cache at all.
entity::SpanProof g_localLoss;
entity::SpanProof g_svrLoss;
entity::SpanProof g_peerLoss;

// The emote chain.  The component system, the id->element map and the element pool are one base
// each and shared by every entrant.  The TARGET is not -- it is a different object per entrant, so
// a line keyed on it would miss every call and the guarded load is the whole check there; the
// vtable behind it is the CLASS's and is shared again.
entity::SpanProof g_emoteCs;
entity::SpanProof g_emoteMap;
entity::SpanProof g_emotePool;
entity::SpanProof g_emoteVtable;
entity::SpanProof g_emoteFn;

// The engine's own arithmetic, in one place so the ratio arm has one implementation.  `has` is
// false exactly where the engine would publish a fabricated zero: a ratio with no samples.
struct StatRead {
    uint32_t raw;
    uint32_t count;
    bool     ratio;
    bool     has;
    bool     ok;      // false: the span went away between the proof and this load
};
// POD locals only -- an object needing unwinding may not share a frame with __try.
StatRead ReadStatWords(const uint8_t* block, const StatWords& s) {
    StatRead r;
    r.raw = 0; r.count = 0; r.ratio = false; r.has = false; r.ok = false;
    // SpanProof::Prove answers about the instant of ITS VirtualQuery, and it deliberately does not
    // re-probe on every cached read -- that is the syscall it exists to remove. So the load itself
    // has to be the thing that answers, the way entity.cpp does it: a page decommitted between the
    // proof and this read is a refusal, not an access violation inside the engine's own tick.
    __try {
        r.ratio = Rd<uint32_t>(const_cast<uint8_t*>(block), s.kind) == kStatKindRatio;
        r.raw   = Rd<uint32_t>(const_cast<uint8_t*>(block), s.value);
        r.count = Rd<uint32_t>(const_cast<uint8_t*>(block), s.count);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return r;
    }
    r.has = !r.ratio || r.count != 0;
    r.ok  = true;
    return r;
}

}  // namespace

// Which conn-stats table this client holds, read from the session role bits sub_1401B2EA0 tests at
// 0x1401B3E45-0x1401B3E68 in exactly this order.  False means the bits could not be read at all --
// an unreadable role is not "client", and the caller must not source a ratio from a guess.
// The netGame span this reads from is proved by the caller; the load is guarded either way, so a
// role pointer that goes away between the two is a refusal and not a fault.
static bool ServerSideConnStats(void* ng, bool& serverSide) {
    uint8_t* role = nullptr;
    if (!entity::TryLoad(At(ng, kPlrRoleObjOff), role)) return false;
    if (!role || !g_roleObj.Prove(role, sizeof(uint64_t))) return false;
    uint8_t bits = 0;
    if (!entity::TryLoad(role, bits)) return false;
    serverSide = (bits & kRoleBitServer) != 0 ||
                 ((bits & kRoleBitClient) == 0 && (bits & kRoleBitHost) != 0);
    return true;
}

bool ReadPlayerStats(void* ng, UserIndex entrant, PlayerStats& out, uint16_t* fill, Refusal* why,
                     const EmoteSource* emote) {
    if (!fill) return Why(why, Refusal::StatsNoFillMask);
    *fill = 0;
    if (!ng) return Why(why, Refusal::NoNetGame);

    // UserTableBound is the entrant-count derivation and the span check over the user array that
    // SlotOfIndex is about to index; SlotOfIndex itself performs neither.
    uint16_t entrants = 0;
    if (!UserTableBound(ng, entrants)) return Why(why, Refusal::RosterUnreadable);

    UserSlot slot(kInvalidU16);
    if (!SlotOfIndex(ng, entrant, slot)) return Why(why, Refusal::NoSuchEntrant);
    if (slot.v >= facts::ENTITY_USER_SLOT_TABLE_MAX) return Why(why, Refusal::SlotOutOfRange);

    uint8_t* const block = At(ng, uint64_t(facts::USER_PROFILE_STRIDE) * slot.v);
    const uint8_t* proved = g_statSpan[slot.v].Prove(block + kStatSpanOff, kStatSpanBytes);
    if (!proved) return Why(why, Refusal::StatsUnreadable);

    for (size_t i = 0; i < kStatCount; ++i) {
        const StatWords& s = kStats[i];
        const StatRead   r = ReadStatWords(block, s);
        if (!r.ok) return Why(why, Refusal::StatsUnreadable);
        if (!r.has) continue;   // a ratio with no samples: the engine's 0 is not a score
        if (s.dst) {
            const int32_t v = static_cast<int32_t>(r.raw);
            out.*(s.dst) = r.ratio ? v / static_cast<int32_t>(r.count) : v;
        } else {
            float v;
            memcpy(&v, &r.raw, sizeof(v));
            out.possessionTime = r.ratio ? v / static_cast<float>(r.count) : v;
        }
        *fill = uint16_t(*fill | s.bit);
    }

    // Everything below reads the netGame's own fixed fields, so they are proved together once.
    // A refusal here costs ping and packetlossratio their bits and leaves the twelve stats alone:
    // they came out of a span that was proved on its own.
    const bool ngFields = g_ngStats.Prove(ng, kNgStatsBytes) != nullptr;

    // ping.  Every step is a load the engine makes inline, so it is made inline here; the accessor
    // returns a bare 0 for a slot past the peer count, and that 0 is denied the same way.
    if (ngFields) {
        uint8_t* peers = nullptr;
        if (entity::TryLoad(At(ng, kPeerObjOff), peers) && peers &&
            g_peerHeader.Prove(peers + kPeerObjAdj + kPeerTableOff,
                               (kPeerCountOff + 8u) - kPeerTableOff)) {
            uint8_t* const hdr   = peers + kPeerObjAdj;
            uint64_t       count = 0;
            uint8_t*       table = nullptr;
            if (entity::TryLoad(At(hdr, kPeerCountOff), count) &&
                entity::TryLoad(At(hdr, kPeerTableOff), table) &&
                table && slot.v < count &&
                g_peerTable.Prove(table, size_t(kPeerStride) * size_t(count))) {
                uint16_t ms = 0;
                if (entity::TryLoad(At(table, uint64_t(kPeerStride) * slot.v + kPeerPingOff), ms)) {
                    out.ping = ms;
                    *fill = uint16_t(*fill | kPsFillPing);
                }
            }
        }
    }

    // packetlossratio.  sub_1401B2EA0 sources it four ways and the arm is chosen here the way the
    // engine chooses it, not picked: past the entrant count it publishes a zeroed xmm15, and that
    // 0.0 is fabricated, so that arm sets no bit.  The two conn-stats tables are indexed by the
    // same slot the ping accessor takes.
    if (ngFields) {
        const uint16_t localCount   = Rd<uint16_t>(ng, kPlrLocalCountOff);
        const uint16_t entrantCount = Rd<uint16_t>(ng, kPlrEntrantCountOff);
        // The arm names a table, the row inside it, and the line that table is proved on.
        uint8_t*           table = nullptr;
        const uint8_t*     src   = nullptr;
        size_t             span  = 0;
        entity::SpanProof* line  = nullptr;
        bool serverSide = false;
        if (entrant.v >= entrantCount) {
            table = nullptr;
        } else if (entrant.v < localCount) {
            table = At(ng, kPlrLocalLossOff);
            src   = table;
            span  = sizeof(float);
            line  = &g_localLoss;
        } else if (ServerSideConnStats(ng, serverSide)) {
            const uint64_t stride = serverSide ? kPlrSvrLossStride : kPlrPeerLossStride;
            table = At(ng, serverSide ? kPlrSvrLossOff : kPlrPeerLossOff);
            src   = table + stride * slot.v;
            span  = size_t(stride * slot.v) + sizeof(float);
            line  = serverSide ? &g_svrLoss : &g_peerLoss;
        }
        if (table && line->Prove(table, span)) {
            float v = 0.0f;
            if (entity::TryLoad(src, v)) {
                out.packetLossRatio = v;
                *fill = uint16_t(*fill | kPsFillPacketLoss);
            }
        }
    }

    // The emote source is the caller's when it named one, and otherwise whatever the entity
    // layer's seam answers.  With neither, bit 14 stays clear -- ResolveEmoteSource names which of
    // the two it was, and nothing here turns "could not ask" into "not playing".
    EmoteSource resolved;
    const EmoteSource* src = emote;
    if (!src && ResolveEmoteSource(ng, entrant, resolved, nullptr)) src = &resolved;
    if (src) {
        bool playing = false;
        if (ReadEmotePlaying(*src, playing, nullptr)) {
            out.isEmotePlaying = playing ? 1u : 0u;
            *fill = uint16_t(*fill | kPsFillEmote);
        }
    }

    return Why(why, Refusal::None);
}

const EmoteSourceSeam* g_emoteSeam = nullptr;

void SetEmoteSource(const EmoteSourceSeam* seam) { g_emoteSeam = seam; }

bool ResolveEmoteSource(void* ng, UserIndex entrant, EmoteSource& out, Refusal* why) {
    if (!g_emoteSeam || !g_emoteSeam->Resolve) return Why(why, Refusal::EmoteNoSeam);
    EmoteSource answered;
    if (!g_emoteSeam->Resolve(ng, entrant, &answered)) return Why(why, Refusal::EmoteNoSystem);
    if (!answered.componentSystem) return Why(why, Refusal::EmoteNoSystem);
    out = answered;
    return Why(why, Refusal::None);
}

bool ReadEmotePlaying(const EmoteSource& src, bool& out, Refusal* why) {
    if (!src.componentSystem) return Why(why, Refusal::EmoteNoSystem);
    uint8_t* const cs = static_cast<uint8_t*>(src.componentSystem);

    // Both pointers are one span from the system's own base.
    if (!g_emoteCs.Prove(cs, kEmotePoolOff + sizeof(void*)))
        return Why(why, Refusal::EmoteUnreadable);
    uint8_t* map  = nullptr;
    uint8_t* pool = nullptr;
    if (!entity::TryLoad(At(cs, kEmoteMapOff), map) ||
        !entity::TryLoad(At(cs, kEmotePoolOff), pool)) return Why(why, Refusal::EmoteUnreadable);
    if (!map || !pool) return Why(why, Refusal::EmoteUnreadable);

    const uint64_t mapOff = uint64_t(kEmoteMapStride) * src.emoteId;
    if (!g_emoteMap.Prove(map, size_t(mapOff) + sizeof(uint16_t)))
        return Why(why, Refusal::EmoteUnreadable);
    uint16_t element = 0;
    if (!entity::TryLoad(At(map, mapOff), element)) return Why(why, Refusal::EmoteUnreadable);

    const uint64_t handleOff = uint64_t(kEmoteElemStride) * element + kEmoteElemAdj;
    if (!g_emotePool.Prove(pool, size_t(handleOff) + kEmoteArgOff + sizeof(int32_t)))
        return Why(why, Refusal::EmoteUnreadable);
    uint8_t* const handle = At(pool, handleOff);

    // sub_140128B20 answers false without dispatching when the handle is empty or names no target.
    // That false is the engine's own answer, so it is reported as one.
    void*    live   = nullptr;
    uint8_t* target = nullptr;
    if (!entity::TryLoad(handle, live) ||
        !entity::TryLoad(At(handle, kEmoteTargetOff), target))
        return Why(why, Refusal::EmoteUnreadable);
    if (!live || !target) { out = false; return Why(why, Refusal::None); }

    // A different target object per entrant, so a proof line keyed on it would miss every call --
    // and the guarded load is the stronger of the two checks anyway.  The vtable behind it is the
    // class's and every entrant's target shares it, so that one is worth a line.
    uint8_t* vtable = nullptr;
    if (!entity::TryLoad(target, vtable) || !vtable)
        return Why(why, Refusal::EmoteVtableUnreadable);
    if (!g_emoteVtable.Prove(vtable, kEmoteVtSlot + sizeof(void*)))
        return Why(why, Refusal::EmoteVtableUnreadable);

    // The slot INDEX is the fact; the pointer is whatever this object's vtable holds right now.
    using EmoteQuery = int (*)(void*, int32_t);
    void* slot = nullptr;
    if (!entity::TryLoad(At(vtable, kEmoteVtSlot), slot) || !slot)
        return Why(why, Refusal::EmoteVtableUnreadable);
    if (!g_emoteFn.Prove(slot, 1)) return Why(why, Refusal::EmoteVtableUnreadable);

    int32_t arg = 0;
    if (!entity::TryLoad(At(handle, kEmoteArgOff), arg)) return Why(why, Refusal::EmoteUnreadable);
    out = reinterpret_cast<EmoteQuery>(slot)(target, arg) != 0;
    return Why(why, Refusal::None);
}


// =============================================================================================
// THE ENGINE'S OWN REMOTE-ADMISSION ROUTE.  See identity_access.h for the whole argument; what
// follows is the four bindings ENG-W2-P1-19 S5 left unbuilt.
// =============================================================================================

// THE SLOT-NAME TABLE AND THE LOBBY MEMBER ARRAY ARE ONE TABLE, and these five lines are the
// derivation rather than an assumption.  sub_1406093C0 -- the reader the entrant-added handler
// asks for the entrant's account type and id -- reads `*(_OWORD *)(*(a1 + 864) + 216 * slot)`
// bounded by `*(a1 + 872)`, on the object at CR15NetGame+0x40 plus 0x18.  Every one of those five
// numbers is already in the registry twice, under an IDENT.SLOTNAME_* id and a LOBBY.* one, and
// they agree.  So SlotNameField above already writes into the record this route has to seed, and
// the seed reuses it instead of walking the chain a second time with its own arithmetic.
static_assert(facts::IDENT_SLOTTABLE_OFF == facts::NETGAME_LOBBY_OFF,
              "the slot-name table hangs off CR15NetGame's lobby pointer");
static_assert(facts::IDENT_SLOTTABLE_VEC_OFF == facts::LOBBY_OBJ_OFF,
              "the slot-name vector is the CNSLobby object");
static_assert(facts::IDENT_SLOTNAME_BASE_OFF == facts::LOBBY_MEMBER_ARRAY_OFF,
              "the slot-name rows ARE the lobby member records");
static_assert(facts::IDENT_SLOTNAME_COUNT_OFF == facts::LOBBY_MEMBER_COUNT_OFF,
              "one count bounds both readings of the same array");
static_assert(facts::IDENT_SLOTNAME_STRIDE == facts::LOBBY_MEMBER_STRIDE,
              "one stride, 216 bytes, for both readings of the same array");

namespace {

// The TEST-only publish shape (five arguments: node, msgId, name, payload, length).  The shipped
// path does NOT go through this -- ReplyRouteInjectForTest supplies it so the queue-and-drain half
// can be driven with no image mapped, and the fake records exactly these five.
using PublishLocalFn = ReplyPublishFn;

// DIAG-DRAIN-NOOP Q2 pivot: the shipped route no longer publishes through the broadcaster
// sub_140F80860 -- offline its router (*node) is null, so the manufactured dispatch bailed on every
// frame (identity_access:3668, proven in-vivo).  It now calls the engine's own EntrantAdded handler
// sub_14017B850 DIRECTLY from the stage-0 drain: the handler decodes the seeded account type and takes
// its REMOTE arm to AddRemoteUser (sub_1401539A0), reaching the same admission with no router at all.
// EntrantAddedHandlerFn (identity_access.h) is the shape it is called as.

// Read a plain data qword at a rebased VA -- the two globals S-A must read at runtime (the cached
// message id and the local-peer sentinel) live in .data and their values are not in the DB, so
// they are read from the mapped image, never transcribed.  Zero when nothing is mapped or the
// address is not readable, and the route stays unpromoted in that case.
uint64_t ReadDataQword(uint64_t va) {
    const void* at = buildguard::MappedAddress(va);
    if (at == nullptr || !Readable(at, sizeof(uint64_t))) return 0;
    return Rd<uint64_t>(at, 0);
}

// The struct name, spelled the way the binary spells it -- leading 'S' included, because
// IWiretap::Symbol hashes the name WITHOUT it and that is precisely what the netgame's registrar
// does for "NSLobbyEntrantAdded" before caching the qword its listener subscribed under.
const char* const kEntrantAddedStruct = "SNSLobbyEntrantAdded";

// The listen set.  These three are the only message names in this tree whose symbols the wiretap
// itself known-answer-checks at Init, so a registration on them cannot silently be a registration
// on nothing.  WHICH of them an offline solo host actually publishes is a question no offline test
// can settle -- ReplyRouteCensus::pumpCallbacks is the number a run answers it with, and
// ReplyRouteArm takes an override so answering it does not need a rebuild.
const char* const kDefaultSymbols =
    "SR15NetComponentEvent,SR15NetPlayerStateMsg,SR15NetReplicatedDeltaUnreliableVars";

constexpr size_t kReplyListenMax = 8;

struct ReplyPending {
    bool       live = false;
    JoinDesc   desc{};
    IIdentity* id   = nullptr;
};

struct ReplyPump {
    IWiretap*       tap      = nullptr;   // the caller's; never owned here
    void*           ng       = nullptr;
    void*           node     = nullptr;   // hub == *(CR15NetGame + NETGAME.BROADCASTER_OFF); kept for
                                          // the census only -- the direct path never reads it
    // The SHIPPED direct target: the engine's own EntrantAdded handler sub_14017B850, byte-anchored at
    // arm (Guarded).  a1 at call time is g_reply.ng, the CR15NetGame it was registered against.  In a
    // test this holds an injected stand-in (ReplyDirectInjectForTest) so the pivot drives with no image.
    EntrantAddedHandlerFn handler = nullptr;
    // The SHIPPED leave target, the add's sibling sub_14017D240, byte-anchored at the
    // same moment and against the same receiver.  Null is a refusal by name (NoReleasePath), never
    // a silent skip -- an Unload that cannot free the slots must say so.
    EntrantAddedHandlerFn leaveHandler = nullptr;
    // Set when `handler` is an injected stand-in rather than a Guarded bind: the receiver is a fixture
    // and the deeper Readable probes the real handler needs are skipped (the stand-in touches only +16).
    bool            directInjected = false;
    PublishLocalFn  publish  = nullptr;
    // One bit per lobby slot: the seed wrote a TRUE `spawned` there before issuing the add.
    uint32_t        seededSpawned = 0;
    uint64_t        msgId    = 0;
    // TRUE ONLY WHILE THE ENGINE'S OWN CALLBACK IS ON THIS THREAD'S STACK.  Not a tick counter,
    // not a time window: the predicate is "the engine called us", and nothing else can set it.
    bool           inPump   = false;
    uint16_t       handles[kReplyListenMax] = {};
    ReplyRouteCensus census{};
    ReplyPending   queue[facts::ENTITY_USER_SLOT_TABLE_MAX];
};

ReplyPump g_reply;

// Deliberately outside ReplyPump: ReplyRouteDisarm resets that struct wholesale, and a seam a
// disarm clears cannot survive the disarm ReplyRouteArmWith itself performs on entry.
struct ReplyInject {
    void*                 node          = nullptr;
    ReplyPublishFn        publish       = nullptr;
    EntrantAddedHandlerFn directHandler = nullptr;  // stand-in for the shipped direct call
    EntrantAddedHandlerFn leaveHandler  = nullptr;  // stand-in for the shipped leave call
    uint64_t              msgId         = 0;
    const HopCallTable*   spawnedCalls  = nullptr;  // stand-in store + setters for the seed's write
};
ReplyInject g_replyInject;

// The 216-byte lobby member record, reached through the one walk this file already has.  Null
// means the chain did not resolve or the slot is past the vector's own count -- never a pointer
// into whatever follows the array.
uint8_t* LobbyMemberRecord(void* ng, UserSlot slot) {
    char* name = SlotNameField(ng, slot);
    if (!name) return nullptr;
    uint8_t* rec = reinterpret_cast<uint8_t*>(name) - facts::IDENT_SLOTNAME_FIELD_OFF;
    return Readable(rec, facts::LOBBY_MEMBER_STRIDE) ? rec : nullptr;
}

}  // namespace

// RECORDER-3 F1.  THE DOCUMENT THE LOBBY MESSAGE ITSELF CARRIED.  sub_1406087F0
// (SNSLobbyEntrantStatev2) parses the JSON text out of the message into this record's +144 node
// (`sub_1405F0BD0(rec+144, (const char*)(a2+24), len)`); its else-arm installs the EMPTY node when
// the message carried none.  The user table's pointer is a different copy, filled later by the
// profile service's reply -- which is why a spectator on a live server reads EmptyProfile there
// for every remote entrant at t=0 while the document is already sitting here.
//
// Keyed on the SLOT, because that is what indexes this array (the handler's own index is
// `*(u16*)(*(a2+16)+98)`).  Read-only: one pointer chase, one Readable, then the shared walk.
bool ProfileJsonBytesFromEntrantRecord(void* ng, UserSlot slot, void* out, size_t cap,
                                       size_t& written, bool& truncated, Refusal* why,
                                       bool* lossyNumber) {
    written = 0;
    truncated = false;
    if (lossyNumber) *lossyNumber = false;
    if (!ng) return Why(why, Refusal::NoNetGame);

    uint8_t* rec = LobbyMemberRecord(ng, slot);
    if (!rec) return Why(why, Refusal::EntrantRecordUnreadable);

    void* doc = rec + kLOBBY_REC_KV_OFF;
    // The engine's own emptiness question, asked of the embedded node exactly as the user-table
    // path asks it of the pointed-to one: the else-arm's node is a valid object with no children.
    auto isEmpty = Entry<FnJsonIsEmpty>(Site::JsonIsEmpty);
    if (isEmpty && isEmpty(doc)) return Why(why, Refusal::EntrantRecordEmptyNode);

    return EmitProfileDocument(doc, nullptr, out, cap, written, truncated, lossyNumber, why);
}

// The id64 half of the same record's head. sub_1406087F0 copies `*(_OWORD*)(a2+16)` -- the
// message's own [accountTypeWord][id64] pair -- into the record at +0/+8, so this value is
// present for a remote entrant from the first lobby message, with no profile document involved.
bool LobbyEntrantAccountId(void* ng, UserSlot slot, uint64_t& out) {
    if (!ng) return false;
    uint8_t* rec = LobbyMemberRecord(ng, slot);
    if (!rec) return false;
    out = Rd<uint64_t>(rec, kLOBBY_REC_ID64_OFF);
    return true;
}

namespace {

// The first sixteen bytes of that record: the _OWORD sub_1406093C0 hands the handler back, as
// [u64 accountTypeWord][u64 accountId].  The handler masks the word and compares it against the bot
// type; every OTHER value falls to the remote arm, which is the arm this route exists to reach.
// 5 is the type the handler's own tag table spells "OVR" -- an ordinary human entrant -- and it is
// written rather than left as whatever the page holds so that the arm the engine takes is a
// consequence of what we wrote and not of uninitialised memory.
constexpr uint64_t kSeedAccountType = 5;
static_assert((kSeedAccountType & facts::IDENT_ACCOUNT_TYPE_MASK) != facts::IDENT_ACCOUNT_TYPE_BOT,
              "the seed must take the handler's REMOTE arm -- the arm that calls "
              "NETGAME.SET_PLAYER_ACTOR -- and the bot type is the only value that does not");
constexpr size_t   kMemberAcctTypeOff = 0x00;
constexpr size_t   kMemberAcctIdOff   = 0x08;

PawnProvision::Refusal SeedLobbyRecordFor(void* ng, const JoinDesc& desc, uint16_t slot,
                                          Refusal& why) {
    uint8_t* rec = LobbyMemberRecord(ng, UserSlot(slot));
    if (!rec) {
        why = Refusal::NoSlotNameTable;
        return PawnProvision::Refusal::ReplySeedRefused;
    }
    // The id matters because the handler prints it: `[NETGAME] Entrant <TAG>-<id> added to slot
    // %llu`.  That line is the discriminator a run is read against, and it cannot appear for an
    // entrant whose record holds no id.  desc.xplatformId is the SYNTHETIC id -- authored upstream
    // clear of the viewer's {platform-key,id64}, so neither of the engine's two
    // eviction sites (sub_1406051C0 lobby-accept dedup, sub_14019F510 profile-received) can match
    // the viewer and remove it.  desc.recordedXplatId carries the real recorded id for the census.
    const uint64_t type = kSeedAccountType;
    const uint64_t id   = desc.xplatformId;
    memcpy(rec + kMemberAcctTypeOff, &type, sizeof type);
    memcpy(rec + kMemberAcctIdOff, &id, sizeof id);
    // S-B fields the remote arm and the actor build read besides the head.  flags bit0 marks the
    // slot valid/ready; team @+142 is authored to the recorded team (a non-4 value keeps the
    // handler's `sub_1406092E0 != 4` belt-and-suspenders happy under any session mode).  bodytype
    // in the KV blob @+144 is left absent on purpose: PICK_BODY_TEMPLATE defaults a missing
    // bodytype to template index 0, a model-bearing remoteplayer_* body, so no KV write is needed.
    uint16_t flags = Rd<uint16_t>(rec, kLOBBY_REC_FLAGS_OFF);
    flags |= uint16_t(kLOBBY_REC_FLAGS_VALID_BIT);
    Wr<uint16_t>(rec, kLOBBY_REC_FLAGS_OFF, flags);
    Wr<uint16_t>(rec, kLOBBY_REC_TEAM_OFF, desc.team);

    // THE DISPLAY NAME, WRITTEN WHERE THE ENGINE'S OWN ENTRANT-STATE HANDLER WRITES IT AND AT THE
    // ONLY MOMENT THAT IS EARLY ENOUGH (NAMES-4).  sub_1406087F0 (NSLobbyEntrantStatev2) fills
    // this record's +0x60/+0x3C pair on a real join, BEFORE the entrant-added message is handled;
    // offline nothing sends that message, so the record kept its constructor default "..."
    // (0x141C3B32C) through the add.  The dress ladder's own SetSlotName lands 700 ms later and
    // does fix the roster and the spectator UI -- both re-read UserDisplayName -- but the add's
    // participating arm (sub_1401C9BA0) has already built the in-world name tag by then, and that
    // tag is not re-read.  Same field, same writer, one dispatch earlier.
    // Idempotent by construction: SetSlotName writes a fixed 0x24-byte buffer plus its mirror, so
    // a re-seed of the same slot stores the same bytes.  An empty recorded name is NOT written:
    // "" is what VacateSlot writes to release a slot, and a seed must never vacate one.
    // A COSMETIC SOURCE COSTS ITSELF: a name the buffer cannot hold must not cost this entrant its
    // admission, its team and its body.  The dress ladder's own SetSlotName step reports that
    // refusal by name a moment later, on the player's own line.
#ifdef NAMES4_MUTATE_SEED_NAME_UNGUARDED
    (void)SetSlotName(ng, UserSlot(slot), desc.displayName);
#else
    if (desc.displayName[0] != '\0')
        (void)SetSlotName(ng, UserSlot(slot), desc.displayName);
#endif
    why = Refusal::None;
    return PawnProvision::Refusal::None;
}

static_assert(facts::ENTITY_USER_SLOT_TABLE_MAX <= 32, "ReplyPump::seededSpawned is one bit per slot");
bool ReplySeededSpawned(uint16_t slot) {
    return slot < facts::ENTITY_USER_SLOT_TABLE_MAX &&
           (g_reply.seededSpawned & (uint32_t(1) << slot)) != 0;
}

bool HookReplyPromoted(void*) { return ReplyRoutePromoted(); }

bool HookReplyInReceivePhase(void*) {
#if defined(W2C_MUTATE_PUBLISH_FROM_TICK)
    // The shape this route must never ship in: the phase gate answers true wherever it is asked,
    // so the tick publishes.  tests/identity builds exactly one target with this defined.
    return true;
#else
    // DIAG-DRAIN-NOOP Q2: with the pivot there is no broadcaster receive phase to wait for -- the
    // handler is called directly.  "In phase" now means "on the blessed stage-0 NETGAME.UPDATE drain
    // stack", and inPump is that window's only witness: it is true ONLY while ReplyDrainFromEngine-
    // Heartbeat is draining (nothing else sets it), so the first provisioner attempt from the plugin
    // tick still parks wrong-phase and only the drain admits.  L3 #21 (a component-system call issued
    // from inside another engine update never returned) is honored because the drain runs at the TOP
    // of CR15NetGame::Update, before the original walks its systems -- not nested inside one.
    return g_reply.inPump;
#endif
}

PawnProvision::Refusal HookReplySeed(void* ctx, uint16_t slot) {
    AdmitCtx* c = static_cast<AdmitCtx*>(ctx);
    Refusal why = Refusal::None;
    const PawnProvision::Refusal r = SeedLobbyRecordFor(c->ng, *c->desc, slot, why);
    g_reply.census.lastSeed = why;
    if (r != PawnProvision::Refusal::None) {
        ++g_reply.census.seedRefusals;
        return r;
    }

    // `spawned` is one more field the add-entrant handler reads that the message does not carry.
    // The add performs the entrant's ONE role transition (65535 -> recorded team), and the
    // participating arm it dispatches to enables the actor only if NETGAME.USER_SPAWNED_GET is
    // already true at that instant (sub_1401C9BA0's remote arm); every later re-entry early-outs
    // on an unchanged role or actor symbol.  Written AFTER the member record, because
    // SESSION.KV_SIDE_SELECT reads the account type the seed has just put there.  Never a refusal
    // of the admission: A2 still writes it after the add, and says so by name if it cannot.
    const bool spawned = PawnProvision::RoleParticipates(c->desc->team);
    const PawnProvision::SpawnedWrite wrote =
        WriteSpawnedForSlot(c->ng, g_replyInject.spawnedCalls, slot, spawned);
    const uint32_t slotBit = uint32_t(1) << slot;
    if (spawned && wrote == PawnProvision::SpawnedWrite::Issued) g_reply.seededSpawned |= slotBit;
    else                                                         g_reply.seededSpawned &= ~slotBit;
    // The seeded name read back through IDENT.SLOTNAME_GET -- the engine's own getter, the one
    // UserDisplayName falls back to -- so the launch can tell "the tag is still `...`" from
    // "the name never reached the record the tag is built from".
    char seededName[kSlotNameCap + 1] = "<unreadable>";
    if (const char* field = SlotNameField(c->ng, UserSlot(slot))) {
        memcpy(seededName, field, kSlotNameCap);
        seededName[kSlotNameCap] = '\0';
    }
    log::Direct(LogLevel::Info, "pawn",
                "reply seed: slot=%u recordedRole=%u spawned=%d name='%s' written=%s "
                "before the entrant add",
                unsigned(slot), unsigned(c->desc->team), spawned ? 1 : 0,
                seededName,
                wrote == PawnProvision::SpawnedWrite::Issued       ? "yes"
                : wrote == PawnProvision::SpawnedWrite::Unwritable ? "no(Unwritable)"
                                                                   : "no(Unavailable)");
    return r;
}

bool DirectEntrantAddedCall(const PawnProvision::ReplyMsg& msg);

bool HookReplyDispatch(void*, const PawnProvision::ReplyMsg& msg, uint32_t bytes) {
    if (!ReplyRoutePromoted()) return false;
    // Not an assertion about our own struct -- an assertion about the ONE number the engine's
    // trampoline sub_14013CD50 tests (`cmp r9,8; jb`) before it calls the handler at all.
    if (bytes != uint32_t(facts::MSG_ENTRANT_ADDED_SIZE)) return false;

    if (g_reply.publish != nullptr) {
        // TEST path: the injected five-argument fake, recording what the route would publish.
        g_reply.publish(g_reply.node, g_reply.msgId, kEntrantAddedStruct, &msg, bytes);
        ++g_reply.census.published;
        return true;
    }

    // SHIPPED path (DIAG-DRAIN-NOOP Q2): call the engine's own EntrantAdded handler sub_14017B850
    // DIRECTLY -- no broadcaster, no router.  The handler reads the account type/ids the seed wrote
    // into the lobby record for this slot and takes its REMOTE arm to AddRemoteUser.  Fully guarded
    // (byte-anchor + Readable probe + EngineCallLatch + SEH) so a bad receiver/msg refuses, never faults.
    return DirectEntrantAddedCall(msg);
}

void ReplyDrain();
bool AnyReplyQueued();
void DrainReplyDirect();

void ReplyPumpThunk(const WireMessage&, void* /*ctx*/) {
    ++g_reply.census.pumpCallbacks;
    // Re-entrancy is the engine's own idiom here -- its handlers publish from inside handlers --
    // but OUR queue must not be drained twice down one stack, or a slot already published for
    // would be published for again on the way out.
    if (g_reply.inPump) return;
    g_reply.inPump = true;
    ReplyDrain();
    g_reply.inPump = false;
}

}  // namespace

const char* ReplyRouteDefaultSymbols() { return kDefaultSymbols; }

// Drives the exact Stage-B authority-flip guard (InvokeSpawnNotify) over a caller-supplied mode-word
// cell and notify, so a test can prove the word is byte-identical after flip+restore on BOTH the
// normal and the faulting path, with no engine mapped.  Returns false if `notify` faulted (contained).
bool StageBAuthFlipProbeForTest(void (*notify)(void* ng, uint16_t idx), void* ng, uint16_t idx,
                                uint64_t* modeWordCell, bool flip) {
    const uint64_t saved = modeWordCell != nullptr ? *modeWordCell : 0;
    return InvokeSpawnNotify(reinterpret_cast<SpawnNotifyFn>(notify), ng, idx,
                             reinterpret_cast<volatile uint64_t*>(modeWordCell), saved, flip);
}

bool ReplyRoutePromoted() {
    if (g_reply.publish != nullptr)                       // injected publish test path
        return g_reply.node != nullptr && g_reply.msgId != 0;
    // shipped / injected DIRECT path (DIAG-DRAIN-NOOP Q2): the receiver context (g_reply.ng, the
    // CR15NetGame the handler was registered against) and the handler itself -- byte-anchored to
    // sub_14017B850 at arm, or a test stand-in.  The broadcaster node/sentinel are gone.
    return g_reply.ng != nullptr && g_reply.handler != nullptr;
}

ReplyRouteCensus ReplyRouteStatus() {
    ReplyRouteCensus c = g_reply.census;
    c.promoted = ReplyRoutePromoted();
    return c;
}

namespace {

// UNCONDITIONAL gate-state trace (DIAG-DRAIN-VISIBLE).  The "reply drain (heartbeat)" line below prints
// only when drained/published moves, so a gate that refuses every frame -- OR a direct call that bails
// every frame -- printed NOTHING, and two launches could not read which of the three gate conditions was
// false.  This prints regardless: every 60th driver call, and immediately on any change of a gate
// sub-condition, the mapped slot, directCalls/drained, or the bail.  Pure reads of our own globals --
// ng/handler are compared, never dereferenced; AnyReplyQueued scans only g_reply.queue.  No engine
// deref, no behaviour change.  It surfaces exactly which of promoted / queued / inPump is false when the
// gate refuses, and if the gate passes, which bail DirectEntrantAddedCall hit.
void LogDrainGateState() {
    const bool ngNonNull      = g_reply.ng != nullptr;
    const bool handlerNonNull = g_reply.handler != nullptr;
    const bool promoted       = ReplyRoutePromoted();
    const bool anyQueued      = AnyReplyQueued();
    const bool inPump         = g_reply.inPump;

    int  mappedSlot = -1;
    bool slotLive   = false;
    bool slotIdSet  = false;
    for (uint16_t i = 0; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i) {
        if (g_reply.queue[i].live || g_reply.queue[i].id) {
            mappedSlot = int(i);
            slotLive   = g_reply.queue[i].live;
            slotIdSet  = g_reply.queue[i].id != nullptr;
            break;
        }
    }

    const ReplyRouteCensus& c = g_reply.census;

    static bool      primed      = false;
    static bool      lastPromoted = false, lastNg = false, lastHandler = false;
    static bool      lastQueued   = false, lastInPump = false, lastLive = false, lastIdSet = false;
    static int       lastSlot     = -2;
    static uint32_t  lastDirect   = 0, lastDrained = 0;
    static ReplyBail lastBail     = ReplyBail::None;

    const bool changed =
        promoted != lastPromoted || ngNonNull != lastNg || handlerNonNull != lastHandler ||
        anyQueued != lastQueued || inPump != lastInPump || slotLive != lastLive ||
        slotIdSet != lastIdSet || mappedSlot != lastSlot || c.directCalls != lastDirect ||
        c.drained != lastDrained || c.lastBail != lastBail;
    // The CHANGE path is untouched -- a gate that moved still prints the
    // moment it moves. Only the every-60-calls reprint of an unchanged gate is spaced out.
    static uint64_t chatterAt = 0;
    const bool cadence = (c.driverCalls % 60u) == 0u && log::ChatterDue(chatterAt);
    if (primed && !changed && !cadence) return;

    primed       = true;
    lastPromoted = promoted; lastNg = ngNonNull; lastHandler = handlerNonNull;
    lastQueued   = anyQueued; lastInPump = inPump; lastLive = slotLive; lastIdSet = slotIdSet;
    lastSlot     = mappedSlot; lastDirect = c.directCalls; lastDrained = c.drained; lastBail = c.lastBail;

    log::Direct(LogLevel::Info, "pawn",
                "reply drain gate: driverCalls=%u promoted=%s(ng=%s,handler=%s) "
                "queued=%s(slot=%d,live=%s,id=%s) inPump=%s directCalls=%u dispatchedLocal=%u "
                "drained=%u lastBail=%s",
                c.driverCalls, promoted ? "yes" : "no", ngNonNull ? "yes" : "no",
                handlerNonNull ? "yes" : "no", anyQueued ? "yes" : "no", mappedSlot,
                slotLive ? "yes" : "no", slotIdSet ? "yes" : "no", inPump ? "yes" : "no",
                c.directCalls, c.dispatchedLocal, c.drained, ReplyBailName(c.lastBail));
}

}  // namespace

// BLOCKER-1 DRAIN (DIAG-REPLYPHASE Fix B; DIAG-DRAIN-NOOP Q2 pivot).  Driven from the dedicated stage-0
// NETGAME.UPDATE detour -- a real in-match engine-update stack, NOT the plugin's async tick (the very
// stack the phase gate refuses, L3 #21).  The wrong-phase-deferred admission would otherwise sit queued
// for ever (RUN: queued=1 drained=0).  Here we drain it by calling the engine's own EntrantAdded handler
// directly.  Cheap no-op unless the route is promoted AND something is parked, so it is safe every frame.
ReplyRouteCensus ReplyDrainFromEngineHeartbeat() {
    // UNCONDITIONAL, before the gate: this is the "did the call site fire at all" witness the last run
    // could not read.  driverCalls>0 with drained==0 means the driver fired but the gate no-op'd it;
    // driverCalls==0 means the driver never fired in-match (the defect this fix removes).
    ++g_reply.census.driverCalls;
    // Then the per-frame gate-state trace, BEFORE the gate's early return so a persistent refusal is
    // visible (the reason two launches were blind).  ALWAYS prints on its cadence/change rule.
    LogDrainGateState();
    if (!ReplyRoutePromoted() || !AnyReplyQueued() || g_reply.inPump) return ReplyRouteStatus();

    const uint32_t drainedBefore   = g_reply.census.drained;
    const uint32_t publishedBefore = g_reply.census.published;

    // inPump is the re-entrancy guard AND the phase witness HookReplyInReceivePhase reads: it is true
    // ONLY while this drain runs, so the direct call is issued exactly on the blessed stage-0 stack and
    // the first provisioner attempt from the plugin tick still parks wrong-phase.
    g_reply.inPump = true;
    if (g_reply.publish != nullptr) {
        ReplyDrain();        // injected publish test path: inPump alone puts us in phase, no engine touch
    } else {
        DrainReplyDirect();  // shipped: SEH-wrapped ReplyDrain -> HookReplyDispatch -> direct handler call
    }
    g_reply.inPump = false;

    const ReplyRouteCensus c = ReplyRouteStatus();
    if (c.drained != drainedBefore || c.published != publishedBefore) {
        log::Direct(LogLevel::Info, "pawn",
                    "reply drain (heartbeat): driverCalls=%u pump=%u queued=%u drained=%u published=%u "
                    "directCalls=%u dispatchedLocal=%u wrongphase=%u lastActorCi=%u bail=%s",
                    c.driverCalls, c.pumpCallbacks, c.queued, c.drained, c.published, c.directCalls,
                    c.dispatchedLocal, c.wrongPhase, c.lastActorCi, ReplyBailName(c.lastBail));
    }
    return c;
}

// The DEDICATED, ALWAYS-ARMED driver (DIAG-DRAIN-NOFIRE Q1 fix A).  The prior fix drove the drain from
// the GUI app-frame heartbeat, which sits behind an ImGui owner-thread gate and delivered ZERO in-match
// invocations offline (the inframe telemetry froze at the state 8->9 transition).  This detour rides
// CR15NetGame::Update instead -- the engine fn that ticks every frame in-match -- so the drain fires on
// the netgame update thread, correct-by-construction, with no owner-thread gate.  It is a separate hook
// from tick's own NetGameUpdateDetour (tick.cpp), which tick.mode=slot leaves uninstalled: the drain
// must not depend on it.  The stage filter is the engine's -- CR15NetGame::Update runs four times a
// frame on a stage index, and only stage 0 is the tick (tick_policy.h IsTickStage), so an unfiltered
// drain would fire four times per frame with three of them on the wrong stage.
namespace {
using ReplyDrainNetGameUpdateFn = char(__fastcall*)(void*, uint64_t);
ReplyDrainNetGameUpdateFn g_replyDrainOriginal = nullptr;
// Cleared by SuspendReplyDrainBody() on unload: the hook stays in the chain, the body does not run.
bool g_replyDrainBodyArmed = false;

// ---- ENG-MODEL-PUMP FIX A: pump the entrant's enqueued model swap to completion ------------------
// The authority apply enqueues the model swap and marks the per-actor record in-flight, but the offline
// scheduler that would run CR15NetPlayerModelSwapperCS's completion never ticks (tick.mode=slot), so the
// body never lands.  This drives the engine's OWN 1-argument completion driver sub_14044A0C0 on the
// swapper CS, from the stage-0 detour top (the shallow, outside-the-component-walk stack the prior audit
// blessed), ONLY while the queue holds a pending swap, SEH-guarded and latched.  It is a TARGETED flush
// of ONE component system's own driver -- never a full CDeferredMethodQueue drain -- so no unrelated
// deferred callback runs.  Never sub_140323790, never an AI fn (proven by BFS; see identity_access_facts.h).
using SwapperPumpFn = void (__fastcall*)(void* swapperCS);
using SwapperFindFn = void* (__fastcall*)(void* space, uint64_t symbol);
// Sub_14042D120's exact decompiled shape -- (swapper CS, user index, target model hash).
using BeginSwapFn   = void (__fastcall*)(void* swapperCS, uint32_t index, uint64_t targetHash);

struct SwapperInject {
    void*         cs     = nullptr;   // stand-in swapper CS memory (tests), or null for the live find()
    SwapperPumpFn pump   = nullptr;   // stand-in for sub_14044A0C0 (tests), or null for the Guarded bind
    BeginSwapFn   begin  = nullptr;   // stand-in for sub_14042D120 (tests), or null for the Guarded bind
    bool          active = false;
};
SwapperInject g_swapperInject;

// ENG-SWAPPER-RESET timing move: the registered per-tick sweep the stage-0 detour drives (set by the
// PawnProvisioner, which knows our entrant indices and the mesh reader).  Null until registered.
SwapperResetSweepFn g_resetSweepFn  = nullptr;
void*               g_resetSweepCtx = nullptr;

// ===== The NETGAME.UPDATE stage-3 tail ============================================================
// The detour rides CR15NetGame::Update; stage 3 is its last stage of a frame (sub_1401BBDB0:
// `else if (a2 == 3)`), so a sweep registered here runs as late in the frame as this plugin can.
// The playback layer registers its HUD-netvar writer; this module only invokes
// what is registered. Null until then, so the shipped default leaves the tail a null check.
constexpr uint64_t kNetGameUpdateStageLast = 3;

GameStateStageSweepFn g_gsStageSweepFn  = nullptr;
void*                 g_gsStageSweepCtx = nullptr;

// Factored out so the test seam drives exactly what the detour drives after the original returns.
void GameStateStageSweepAfterOriginal(void* netGame, uint64_t stage) {
    if (stage != kNetGameUpdateStageLast) return;
    if (g_gsStageSweepFn != nullptr) g_gsStageSweepFn(g_gsStageSweepCtx, netGame);
}

struct SwapperFlushOutcome {
    bool     resolved      = false;   // the swapper CS resolved (non-null)
    bool     pumped        = false;   // the driver was invoked this call
    bool     faulted       = false;   // the driver raised, and was contained
    bool     reentrant     = false;   // the shared engine-call latch was already held
    uint64_t pendingBefore = 0;
    uint64_t pendingAfter  = 0;
};

// SEH lives in its own POD-only functions (MSVC C2712 forbids __try beside a C++ object that unwinds).
void* CallFindGuarded(SwapperFindFn find, void* space, uint64_t sym) {
    __try {
        return find(space, sym);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
bool CallSwapperPumpGuarded(SwapperPumpFn pump, void* cs) {
    __try {
        pump(cs);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallBeginSwapGuarded(BeginSwapFn begin, void* cs, uint32_t index, uint64_t targetHash) {
    __try {
        begin(cs, index, targetHash);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Resolve the CR15NetPlayerModelSwapperCS off the netgame's CncaGameSpace via FindComponentSystem -- the
// same byte-anchored lookup the entity layer resolves its component systems through.  Cached per netgame:
// a component system does not move within a session.  Returns null (never faults) when the space, the
// lookup or the symbol does not resolve, and the caller then simply does not pump.
void* ResolveSwapperCS(void* ng) {
    if (g_swapperInject.active) return g_swapperInject.cs;
    static void* cached   = nullptr;
    static void* cachedNg = nullptr;
    if (cached != nullptr && cachedNg == ng) return cached;
    if (!ng || !Readable(At(ng, facts::NETGAME_CS_SPACE_OFF), sizeof(void*))) return nullptr;
    void* space = Rd<void*>(ng, facts::NETGAME_CS_SPACE_OFF);
    if (!space || !Readable(space, sizeof(void*))) return nullptr;
    SwapperFindFn find = Guarded<SwapperFindFn>(facts::CS_LOOKUP_BY_SYMBOL,
                                                facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE, 0);
    if (!find) return nullptr;
    void* cs = CallFindGuarded(find, space, kSWAPPER_SYM);
    if (cs != nullptr) { cached = cs; cachedNg = ng; }
    return cs;
}

// The address of the per-record CURRENT-MODEL-HASH qword for a user index, or null if any hop is
// unreadable.  Mirrors sub_1404B5D90 exactly: record (handle table[+0xD0][index] -> base[+0x100] +
// stride*handle) -> model-instance ptr (rec+408) & key (rec+416) -> inst[+0xD0] u16 index table read
// at 4*key -> inst[+0x100] pool -> slot object pointer at (pool + 16*slotIndex + 8) -> field at
// slot+0xB10.  Every dereference is Readable-probed, so a torn or pooled record yields null, not a
// fault.  This is the qword the dispatcher sub_140CB0370 compares against the swap target.
uint64_t* ResolveSwapperCurrentField(void* cs, uint16_t index) {
    if (!cs) return nullptr;
    if (!Readable(At(cs, kSWAPPER_HANDLE_TABLE_OFF), sizeof(void*))) return nullptr;
    void* table = Rd<void*>(cs, kSWAPPER_HANDLE_TABLE_OFF);
    if (!table || !Readable(At(table, 4u * index), sizeof(uint16_t))) return nullptr;
    const uint16_t handle = Rd<uint16_t>(table, 4u * index);
    if (!Readable(At(cs, kSWAPPER_RECORD_BASE_OFF), sizeof(void*))) return nullptr;
    void* base = Rd<void*>(cs, kSWAPPER_RECORD_BASE_OFF);
    if (!base) return nullptr;
    uint8_t* rec = At(base, uint64_t(kSWAPPER_RECORD_STRIDE) * handle);

    if (!Readable(At(rec, kSWAPPER_MODELINST_OFF), sizeof(void*))) return nullptr;
    void* inst = Rd<void*>(rec, kSWAPPER_MODELINST_OFF);
    if (!inst) return nullptr;
    if (!Readable(At(rec, kSWAPPER_MODELKEY_OFF), sizeof(uint32_t))) return nullptr;
    const uint16_t key = uint16_t(Rd<uint32_t>(rec, kSWAPPER_MODELKEY_OFF));

    if (!Readable(At(inst, kSWAPPER_INST_INDEX_TABLE_OFF), sizeof(void*))) return nullptr;
    void* idxTable = Rd<void*>(inst, kSWAPPER_INST_INDEX_TABLE_OFF);
    if (!idxTable || !Readable(At(idxTable, 4u * key), sizeof(uint16_t))) return nullptr;
    const uint16_t slotIndex = Rd<uint16_t>(idxTable, 4u * key);

    if (!Readable(At(inst, kSWAPPER_INST_POOL_BASE_OFF), sizeof(void*))) return nullptr;
    void* pool = Rd<void*>(inst, kSWAPPER_INST_POOL_BASE_OFF);
    if (!pool) return nullptr;
    const uint64_t slotOff =
        uint64_t(kSWAPPER_INST_SLOT_STRIDE) * slotIndex + kSWAPPER_INST_SLOT_PTR_OFF;
    if (!Readable(At(pool, slotOff), sizeof(void*))) return nullptr;
    void* slot = Rd<void*>(pool, slotOff);
    if (!slot) return nullptr;

    if (!Readable(At(slot, kSWAPPER_CURRENT_MODEL_OFF), sizeof(uint64_t))) return nullptr;
    return reinterpret_cast<uint64_t*>(At(slot, kSWAPPER_CURRENT_MODEL_OFF));
}

// The -1 store lives in its own POD-only function (MSVC C2712 forbids __try beside an unwinding C++
// object).  Readable proved the page committed+readable; a read-only protection would still fault on
// write, and this contains it so a mis-typed record never crashes the process.
bool StoreQwordGuarded(uint64_t* field, uint64_t value) {
    __try {
        *field = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

SwapperFlushOutcome SwapperFlushBody(void* ng) {
    SwapperFlushOutcome o;
    void* cs = ResolveSwapperCS(ng);
    o.resolved = cs != nullptr;
    if (!cs) return o;
    // Gate on the pending-queue count: the driver drains while it is non-zero and prints "No pending
    // swaps" otherwise, so an empty-queue call is a harmless but noisy no-op we skip.  This is also the
    // "only while a swap is in flight" condition -- an enqueued, un-landed swap keeps the count > 0.
    if (!Readable(At(cs, kSWAPPER_PENDING_COUNT_OFF), sizeof(uint64_t))) return o;
    o.pendingBefore = Rd<uint64_t>(cs, kSWAPPER_PENDING_COUNT_OFF);
    o.pendingAfter  = o.pendingBefore;
    if (o.pendingBefore == 0) return o;

    SwapperPumpFn pump = (g_swapperInject.active && g_swapperInject.pump)
                             ? g_swapperInject.pump
                             : Guarded<SwapperPumpFn>(kSWAPPER_PUMP_VA, kSWAPPER_PUMP_VAPrologue, 0);
    if (!pump) return o;

    // The shared re-entry latch every engine call this plugin issues from the stage-0 tick takes: a
    // callback that re-entered us is refused, not allowed to start a second pump inside the first.
    PawnProvision::EngineCallLatch latch;
    if (!latch.taken) { o.reentrant = true; return o; }

    if (!CallSwapperPumpGuarded(pump, cs)) { o.faulted = true; return o; }
    o.pumped = true;
    if (Readable(At(cs, kSWAPPER_PENDING_COUNT_OFF), sizeof(uint64_t)))
        o.pendingAfter = Rd<uint64_t>(cs, kSWAPPER_PENDING_COUNT_OFF);
    return o;
}

void SwapperFlushTick(void* ng) {
    const SwapperFlushOutcome o = SwapperFlushBody(ng);
    // One-shot resolution dump so a null CS is never mistaken for an empty queue.
    static bool dumped = false;
    if (!dumped) {
        dumped = true;
        log::Direct(LogLevel::Info, "pawn", "swapper pump: CR15NetPlayerModelSwapperCS %s",
                    o.resolved ? "resolved (non-null)"
                               : "UNRESOLVED (null) -- no swap can be pumped");
    }
    // Then only on a real event or a queue-depth change, so the frame loop does not spam.
    static uint64_t lastPending = ~0ull;
    if (o.pumped || o.faulted || o.reentrant || o.pendingBefore != lastPending) {
        lastPending = o.pendingBefore;
        log::Direct(LogLevel::Info, "pawn",
                    "swapper pump: resolved=%s pendingBefore=%llu pendingAfter=%llu pumped=%d "
                    "faulted=%d reentrant=%d",
                    o.resolved ? "yes" : "no", (unsigned long long)o.pendingBefore,
                    (unsigned long long)o.pendingAfter, o.pumped ? 1 : 0, o.faulted ? 1 : 0,
                    o.reentrant ? 1 : 0);
    }
}

void RunStage0Sweep(void* netGame) {
    if (g_resetSweepFn == nullptr) return;
    g_inStage0Sweep = true;
    g_resetSweepFn(g_resetSweepCtx, netGame);
    g_inStage0Sweep = false;
}

// Everything the detour does ahead of the original, factored out so the test seam drives exactly
// what the live detour drives.  The drain's shipped path (DirectEntrantAddedCall) does its own
// Readable-probing, byte-anchor, latch and SEH.  Never sub_140323790.
//
// The detour outlives every match, and the drain and the pump both call into engine objects that go
// with the level: the pump even resolves its CS once per netgame and keeps it.  So both wait on the
// session predicate every other per-tick writer uses.  The sweep carries the same gate of its own.
void NetGameUpdateStage0Body(void* netGame, uint64_t stage) {
    if (!::replay::tick::IsTickStage(stage)) return;
#ifdef EXITFIX_MUTATE_UNGATED_HEARTBEAT
    const bool inMatch = true;
#else
    const bool inMatch = PawnProvision::SessionBlocker(netGame) == PawnProvision::Refusal::None;
#endif
    if (inMatch) ReplyDrainFromEngineHeartbeat();
    // Force our pooled entrant's swapper current to -1 BEFORE the engine's
    // admission Apply Chassis reads it (this is the fast in-match cadence that fires in that
    // window; the loadout-drive site fired ~2s too late).  Then FIX A pumps the swap it enqueues.
    RunStage0Sweep(netGame);
    if (inMatch) SwapperFlushTick(netGame);
}

// ★ The gate's decision, in ONE place so the test seam cannot drift from the shipped detour.
// Only the STAGE-0 body is the loaded half. The stage-3 tail is the one live driver of the leave
// feed, and Unload disarms the body BEFORE it queues the entrants' leave -- so gating the tail on
// `armed` too made the hook swallow the plugin's own removals and the rails stayed full
// (DISC-FLIGHT defect 2). Containment is the live-safety condition here; loadedness is not.
struct ReplyDrainGate {
    bool pass_through;   // uncontained: the plugin makes no engine write this frame
    bool run_body;       // a replay is loaded, so the stage-0 sweeps and the swapper flush run
};

ReplyDrainGate ReplyDrainDecide() {
    ReplyDrainGate g{};
    g.pass_through = !livesafety::Allowed(livesafety::Feature::ReplyDrain);
    g.run_body = !g.pass_through && g_replyDrainBodyArmed;
    return g;
}

char __fastcall ReplyDrainNetGameUpdateDetour(void* netGame, uint64_t stage) {
    // This detour is never removed once armed, so the gate has to live here rather than at the
    // install. Two terms, and both are pass-through: the body's sweeps and swapper flush are engine
    // writes, so an uncontained process must not run them, and neither must a process whose demo
    // has been unloaded from under the still-armed hook.
    const ReplyDrainGate gate = ReplyDrainDecide();
    if (gate.pass_through) {
        return g_replyDrainOriginal(netGame, stage);
    }
    if (gate.run_body) {
        NetGameUpdateStage0Body(netGame, stage);  // before the original walks its systems this frame
    }
    const char result = g_replyDrainOriginal(netGame, stage);
    // After the original, so a registered stage-3 sweep is the last writer this detour can be.
    GameStateStageSweepAfterOriginal(netGame, stage);
    return result;
}

// The engine's own "[NETGAME] Profile received" handler (sub_14019F510):
// (a1 = CR15NetGame*, a2 = the 16-byte [accountType][accountId] id, a3 = a parsed CJson document
// stored whole at [entrant+0x3C0]).  __fastcall spelled for parity with the other engine fns (x64
// passes rcx/rdx/r8 regardless).
using ProfileReceivedFn = void (__fastcall*)(void* netGame, const void* xplatId, void* doc);

struct ProfileReceivedInject {
    ProfileReceivedFn handler = nullptr;   // stand-in for sub_14019F510 (tests), or null for Guarded
    void*    (*ctor)(void*)                         = nullptr;  // stand-in for JSON_DOC_CTOR
    uint32_t (*parse)(void*, const char*, uint64_t) = nullptr;  // stand-in for JSON_PARSE_TEXT
    bool active = false;
};
ProfileReceivedInject g_profileInject;

// SEH lives in a POD-only function (MSVC C2712 forbids __try beside the EngineCallLatch in the
// caller).  a3 is read heavily by the handler and its callees; a malformed document faults into
// __except here and admits nothing rather than crashing echovr.
bool CallProfileReceivedGuarded(ProfileReceivedFn fn, void* ng, const void* xplatId, void* doc) {
    __try {
        fn(ng, xplatId, doc);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

// ENG-BOT-BODY test seam.  -1 defers to config (shipped), 0 forces the legacy reply route, 1 forces
// the bot path -- so a test steers the admission choice deterministically without the process-wide
// env-override cache.  Not #ifdef-guarded, matching the file's other inject seams.
void BotBodyForceForTest(int state) { g_botBodyForce = state; }

void SwapperInjectForTest(void* cs, void (*pump)(void*)) {
    g_swapperInject.cs     = cs;
    g_swapperInject.pump   = reinterpret_cast<SwapperPumpFn>(pump);
    g_swapperInject.active = true;
}
void SwapperClearInjectForTest() { g_swapperInject = SwapperInject{}; }

void SwapperInjectBeginForTest(void (*begin)(void* cs, uint32_t index, uint64_t target)) {
    g_swapperInject.begin = reinterpret_cast<BeginSwapFn>(begin);
}

SwapperPumpCensus SwapperFlushForTest(void* ng) {
    const SwapperFlushOutcome o = SwapperFlushBody(ng);
    SwapperPumpCensus c;
    c.resolved      = o.resolved;
    c.pumped        = o.pumped;
    c.faulted       = o.faulted;
    c.reentrant     = o.reentrant;
    c.pendingBefore = o.pendingBefore;
    c.pendingAfter  = o.pendingAfter;
    return c;
}

bool ReadSwapperEntrant(void* ng, uint16_t index, SwapperEntrantState& out) {
    out = SwapperEntrantState{};
    void* cs = ResolveSwapperCS(ng);
    if (!cs) return false;
    out.resolved = true;
    if (Readable(At(cs, kSWAPPER_PENDING_COUNT_OFF), sizeof(uint64_t)))
        out.pending = Rd<uint64_t>(cs, kSWAPPER_PENDING_COUNT_OFF);
    // Per-actor record: handle table (+0xD0) -> handle -> record (base +0x100 + stride*handle) -> +8.
    if (!Readable(At(cs, kSWAPPER_HANDLE_TABLE_OFF), sizeof(void*))) return true;
    void* table = Rd<void*>(cs, kSWAPPER_HANDLE_TABLE_OFF);
    if (!table || !Readable(At(table, 4u * index), sizeof(uint16_t))) return true;
    const uint16_t handle = Rd<uint16_t>(table, 4u * index);
    if (!Readable(At(cs, kSWAPPER_RECORD_BASE_OFF), sizeof(void*))) return true;
    void* base = Rd<void*>(cs, kSWAPPER_RECORD_BASE_OFF);
    if (!base) return true;
    const uint8_t* rec = At(base, uint64_t(kSWAPPER_RECORD_STRIDE) * handle);
    if (!Readable(rec, size_t(kSWAPPER_INFLIGHT_OFF) + sizeof(uint32_t))) return true;
    out.inFlight   = Rd<uint32_t>(rec, kSWAPPER_INFLIGHT_OFF);
    out.handle     = handle;
    out.handleRead = true;
    // (A) the CURRENT-MODEL-HASH the dispatcher compares against the target: present (a real hash)
    // while no mesh is built is the pooled-record from==to state this pass exists to clear.
    if (uint64_t* field = ResolveSwapperCurrentField(cs, index)) {
        out.currentModel = *field;
        out.currentRead  = true;
    }
    return true;
}

SwapperResetOutcome ResetSwapperCurrent(void* ng, uint16_t index) {
    SwapperResetOutcome o;
    void* cs = ResolveSwapperCS(ng);
    o.resolved = cs != nullptr;
    if (!cs) return o;
    uint64_t* field = ResolveSwapperCurrentField(cs, index);
    if (!field) return o;   // any hop unreadable -> refuse, never write a guessed address
    o.located = true;
    o.before  = *field;     // Readable-proved inside the resolve; the pooled prior-occupant hash
    o.after   = o.before;
    if (StoreQwordGuarded(field, ~uint64_t(0))) {   // -1 = the no-model sentinel the != arm needs
        o.wrote = true;
        if (Readable(field, sizeof(uint64_t))) o.after = *field;
    } else {
        o.faulted = true;
    }
    return o;
}

bool SwapperResetShouldFire(bool meshPresent, const SwapperEntrantState& sw) {
    if (meshPresent) return false;                  // mesh already built -> STOP (no re-swap)
    if (!sw.resolved) return false;                 // swapper CS not resolvable yet
    if (sw.pending != 0) return false;              // a swap is already enqueued -> STOP
    if (!sw.currentRead) return false;              // the record's current field is not walkable yet
    if (sw.currentModel == ~uint64_t(0)) return false;  // already -1 -> no churn
    return true;                                    // mesh absent, real stale current, nothing enqueued
}

bool BeginSwapShouldFire(bool meshPresent, const SwapperEntrantState& sw, bool targetKnown,
                         bool alreadyIssued) {
    if (alreadyIssued) return false;                     // fire once per entrant -> no double-enqueue
    if (meshPresent) return false;                       // body already built -> STOP
    if (!sw.resolved) return false;                      // swapper CS not resolvable yet
    if (sw.pending != 0) return false;                   // a swap is already enqueued -> STOP
    if (!sw.currentRead) return false;                   // the record's current field is not walkable
    if (sw.currentModel != ~uint64_t(0)) return false;   // current not yet forced to -1 (sweep pending)
    if (!targetKnown) return false;                      // no captured target to enqueue with
    return true;                                         // -1 persisted, nothing enqueued, target known
}

BeginSwapOutcome TriggerBeginModelSwap(void* ng, uint16_t index, uint64_t targetHash) {
    BeginSwapOutcome o;
    void* cs = ResolveSwapperCS(ng);
    o.resolved = cs != nullptr;
    if (!cs) return o;
    // BeginModelSwap dereferences the SAME record walk (rec+408 -> +0xB10) the reset does; refuse to
    // hand the engine fn a torn or pooled record that fails the walk.  This also confirms the record
    // for THIS index exists before we enqueue against it.
    uint64_t* field = ResolveSwapperCurrentField(cs, index);
    if (!field) return o;
    o.located = true;

    if (Readable(At(cs, kSWAPPER_PENDING_COUNT_OFF), sizeof(uint64_t)))
        o.pendingBefore = Rd<uint64_t>(cs, kSWAPPER_PENDING_COUNT_OFF);
    o.pendingAfter = o.pendingBefore;

    BeginSwapFn begin = (g_swapperInject.active && g_swapperInject.begin)
                            ? g_swapperInject.begin
                            : Guarded<BeginSwapFn>(kSWAPPER_BEGIN_SWAP_VA,
                                                   kSWAPPER_BEGIN_SWAP_VAPrologue, 0);
    if (!begin) return o;   // prologue moved / unmapped -> refuse, never call a guessed address

    // The same re-entry latch FIX A takes: a callback that re-entered us cannot start a second enqueue.
    PawnProvision::EngineCallLatch latch;
    if (!latch.taken) { o.reentrant = true; return o; }

    if (!CallBeginSwapGuarded(begin, cs, uint32_t(index), targetHash)) { o.faulted = true; return o; }
    o.issued = true;
    if (Readable(At(cs, kSWAPPER_PENDING_COUNT_OFF), sizeof(uint64_t)))
        o.pendingAfter = Rd<uint64_t>(cs, kSWAPPER_PENDING_COUNT_OFF);
    return o;
}

bool EmulateProfileShouldFire(bool meshPresent, bool profilePresent, bool alreadyIssued) {
    if (alreadyIssued) return false;    // fire once per entrant -> the engine seats the CI once
    if (meshPresent) return false;      // the InstanceModel CI is already seated -> STOP
    if (!profilePresent) return false;  // no recorded profile bytes to hand the handler
    return true;
}

ProfileReceivedOutcome EmulateProfileReceivedCall(void* ng, uint64_t xplatformId,
                                                  const Blob& recordedJson) {
    ProfileReceivedOutcome o;
    o.account = xplatformId;

    // a1 -- the live netgame.  Refuse a null/unmapped receiver cleanly; a mapped-but-wrong one
    // faults deep in the handler and the SEH in CallProfileReceivedGuarded contains that.
    if (ng == nullptr || !Readable(ng, sizeof(void*))) { o.receiverBad = true; return o; }

    // The recorded bytes must be an acceptable json OBJECT before the engine parser sees them --
    // the same gate AttachProfileDocument applies, so a malformed document never reaches the engine.
    if (!RecordedProfileJsonAcceptable(recordedJson)) { o.jsonRejected = true; return o; }

    // a3 -- parse the recording into an engine CJson document with the engine's OWN reader
    // (IDENT.JSON_DOC_CTOR over IDENT.JSON_DOC_SIZE bytes, IDENT.JSON_PARSE_TEXT over the recorded
    // bytes): the exact ctor+parse AttachProfileDocument/BindLoadoutDocument use.  The capture layer
    // stores the DECODED document (not the zstd wire form), so no decompression is needed.
    const FnJsonDocCtor   ctor  = (g_profileInject.active && g_profileInject.ctor)
                                      ? g_profileInject.ctor : Entry<FnJsonDocCtor>(Site::JsonDocCtor);
    const FnJsonParseText parse = (g_profileInject.active && g_profileInject.parse)
                                      ? g_profileInject.parse
                                      : Entry<FnJsonParseText>(Site::JsonParseText);
    if (ctor == nullptr || parse == nullptr) { o.parserUnbound = true; return o; }

    void* doc = ::calloc(1, size_t(kJSON_DOC_SIZE));
    if (doc == nullptr) { o.parserUnbound = true; return o; }   // could-not-ask; nothing was parsed
    ctor(doc);
    if (parse(doc, reinterpret_cast<const char*>(recordedJson.p), uint64_t(recordedJson.n)) != 0) {
        ::free(doc);   // the engine leaves its arg untouched on the parse-error arm: report + return
        o.parseFailed = true;
        return o;
    }

    // a2 -- the 16-byte id the handler reads: [accountType=5][accountId=the slotted id].  The type
    // is kSeedAccountType (the exact value SeedLobbyRecordFor wrote) and the id is the SAME synthetic
    // xplatformId in the slot, so the handler's slot lookup finds our entrant and its eviction site
    // (clear of the viewer's id) cannot remove it.  Tied to the seed's own field layout.
    static_assert(kMemberAcctTypeOff == 0 && kMemberAcctIdOff == 8,
                  "the emulated a2 must be [u64 accountType][u64 accountId] to match the lobby "
                  "member record SeedLobbyRecordFor wrote");
    struct EmulatedXPlatformId { uint64_t accountType; uint64_t accountId; };
    EmulatedXPlatformId xpid{ kSeedAccountType, xplatformId };

    // Readable-probe both remaining args (the briefing's rule); the doc probe is the meaningful one
    // (heap the ctor wrote to), the stack id is a formality kept for parity.  A failure here is
    // before the call, so the doc is ours to free.
    if (!Readable(&xpid, sizeof xpid) || !Readable(doc, size_t(kJSON_DOC_SIZE))) {
        ::free(doc);
        o.receiverBad = true;   // an arg did not read back; refuse, do not call
        return o;
    }

    const ProfileReceivedFn fn =
        (g_profileInject.active && g_profileInject.handler)
            ? g_profileInject.handler
            : Guarded<ProfileReceivedFn>(facts::IDENT_PROFILE_RECEIVE_VA,
                                         facts::IDENT_PROFILE_RECEIVE_VA_PROLOGUE, 0);
    if (fn == nullptr) { ::free(doc); o.handlerUnbound = true; return o; }   // rebase/patch -> refuse

    // The shared re-entry latch every engine call this plugin issues from the stage-0 tick takes: a
    // callback that re-entered us cannot start a second emulation inside the first.  Refusal is
    // self-healing -- the sweep re-asks next tick.
    PawnProvision::EngineCallLatch latch;
    if (!latch.taken) { ::free(doc); o.reentrant = true; return o; }

    if (!CallProfileReceivedGuarded(fn, ng, &xpid, doc)) {
        // The handler faulted.  It may already have stored the doc at [entrant+0x3C0] before raising,
        // so the engine could now hold it -- do NOT free (a leaked document is bounded and safe; a
        // freed one the engine still points at is a use-after-free).
        o.faulted = true;
        return o;
    }
    // The handler stored the doc whole at [entrant+0x3C0] and abandoned the old one; the engine owns
    // it and has no free path for a replaced document, so it is deliberately not freed here -- it
    // outlives the entrant, exactly as AttachProfileDocument leaves its own doc.
    o.issued = true;
    return o;
}

void ProfileReceivedInjectForTest(void (*handler)(void* ng, const void* xplatId, void* doc),
                                  void* (*ctor)(void*),
                                  uint32_t (*parse)(void*, const char*, uint64_t)) {
    g_profileInject.handler = reinterpret_cast<ProfileReceivedFn>(handler);
    g_profileInject.ctor    = ctor;
    g_profileInject.parse   = parse;
    g_profileInject.active  = true;
}

void ProfileReceivedClearInjectForTest() { g_profileInject = ProfileReceivedInject{}; }

// ===== ENG-EQUIPMENT-RECV ==========================================================================

namespace {

// The engine's own R15NetPlayerEquipmentUpdate RECV handler sub_140D37300: (a1 = R14NetPlayerEquipment
// component system, a2 = the 3-qword message wrapper whose a2[2] points at the 56-byte body).  __fastcall
// spelled for parity with the other engine fns (x64 passes rcx/rdx regardless).
using EquipRecvFn = void (__fastcall*)(void* equipCs, void* msg);

struct EquipmentRecvInject {
    void*      cs      = nullptr;   // stand-in equipment CS (tests), or null for the real resolve
    EquipRecvFn handler = nullptr;  // stand-in for sub_140D37300 (tests), or null for Guarded
    bool       active  = false;
    uint64_t (*slotSymbol)(const char* slotName) = nullptr;  // stand-in for the slot-name hash
};
EquipmentRecvInject g_equipInject;

// SEH in its own POD-only function (MSVC C2712 forbids __try beside the EngineCallLatch in the caller).
// The handler dereferences the body and the equipment record heavily; a torn record or a wrong object
// faults into __except here and drives nothing, rather than crashing echovr.
bool CallEquipRecvGuarded(EquipRecvFn fn, void* cs, void* msg) {
    __try {
        fn(cs, msg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Resolve the R14NetPlayerEquipment component system off the netgame's CncaGameSpace via
// FindComponentSystem -- the same byte-anchored lookup ResolveSwapperCS uses, with the equipment class
// symbol EQUIP.CS_SYM.  Returns null (never faults) when the space, the lookup or the symbol does not
// resolve.  Not cached: the drive is fire-once per entrant, so the extra lookup is negligible and a stale
// cache across a session change would be a hazard the swapper cache does not have (it re-keys on ng).
void* ResolveEquipmentCS(void* ng) {
    if (g_equipInject.active) return g_equipInject.cs;
    if (!ng || !Readable(At(ng, facts::NETGAME_CS_SPACE_OFF), sizeof(void*))) return nullptr;
    void* space = Rd<void*>(ng, facts::NETGAME_CS_SPACE_OFF);
    if (!space || !Readable(space, sizeof(void*))) return nullptr;
    SwapperFindFn find = Guarded<SwapperFindFn>(facts::CS_LOOKUP_BY_SYMBOL,
                                                facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE, 0);
    if (!find) return nullptr;
    return CallFindGuarded(find, space, kEQUIP_CS_SYM);
}

// IDENT.SYMBOL_FROM_NAME_VA(name, seed, withCallback, maxChars, cbArg).
using EquipSymbolFn = uint64_t (*)(const char*, uint64_t, int32_t, int32_t, void*);

}  // namespace

uint64_t LoadoutSlotSymbol(const char* slotName) {
    if (slotName == nullptr || slotName[0] == 0) return 0;
    if (g_equipInject.slotSymbol != nullptr) return g_equipInject.slotSymbol(slotName);
    EquipSymbolFn hash = Guarded<EquipSymbolFn>(facts::IDENT_SYMBOL_FROM_NAME_VA,
                                                facts::IDENT_SYMBOL_FROM_NAME_VA_PROLOGUE, 0);
    if (!hash) return 0;
    // Both engine call sites seed with -1, no callback, unbounded length -- the same shape kSlotNames
    // are hashed with, so this reproduces the recorded CustSlotEntry.key for the slot.
    return hash(slotName, ~uint64_t(0), 0, -1, nullptr);
}

bool EquipmentRecvShouldFire(bool meshPresent, bool chassisKnown, bool recordReady, bool alreadyIssued) {
    if (alreadyIssued) return false;   // fire once per entrant -> the engine installs the model once
    if (meshPresent) return false;     // the InstanceModel CI is already seated -> STOP
    if (!chassisKnown) return false;   // no recorded chassis to drive with
    if (!recordReady) return false;    // the actor's swapper record is not up yet -> re-ask next tick
    return true;
}

EquipmentRecvOutcome DriveEquipmentRecv(void* ng, uint16_t index, uint64_t chassisHash) {
    EquipmentRecvOutcome o;
    o.index   = index;
    o.chassis = chassisHash;

    // Twin of the extraction-site guard in EmulateEquipmentRecvSweep: the engine's own unfilled-pair
    // sentinel (0xFFFFFFFFFFFFFFFF) or an unworn 0 chassis names no model, so it is not driven. The
    // sweep already gates this out before the call; this refuses it for any other caller too.
    if (chassisHash == ~uint64_t(0) || chassisHash == 0) {
        o.chassisRejected = true;
        return o;
    }

    // a1 -- the equipment component system for THIS netgame's space.
    if (!ng || !Readable(At(ng, facts::NETGAME_CS_SPACE_OFF), sizeof(void*))) return o;
    o.spaceResolved = true;
    void* cs = ResolveEquipmentCS(ng);
    if (!cs) return o;   // component not up yet (or symbol did not resolve) -> transient, re-ask
    o.equipResolved = true;

    // Correctness confirmation: a1[16] -> +8 -> +BACKREF is a back-reference to the owning CR15NetGame
    // (sub_140113A90).  If it reads back and DISAGREES with our ng, this is not our entrant's equipment
    // component -- refuse.  If a hop is unreadable we cannot confirm; the CS lookup already scoped a1 to
    // ng's own space, and the SEH around the call contains any fault, so we proceed logging unconfirmed.
    if (Readable(At(cs, kEQUIP_NETGAME_CTX_OFF), sizeof(void*))) {
        void* ctx = Rd<void*>(cs, kEQUIP_NETGAME_CTX_OFF);
        if (ctx && Readable(At(ctx, kEQUIP_NETGAME_CTX2_OFF), sizeof(void*))) {
            void* inner = Rd<void*>(ctx, kEQUIP_NETGAME_CTX2_OFF);
            if (inner && Readable(At(inner, kEQUIP_NETGAME_BACKREF_OFF), sizeof(void*))) {
                void* back = Rd<void*>(inner, kEQUIP_NETGAME_BACKREF_OFF);
                if (back == ng) o.netgameConfirmed = true;
                else            o.netgameContradicted = true;
            }
        }
    }
    if (o.netgameContradicted) return o;   // wrong object -> never drive the handler on it

    // +0 -- the entrant's SR15NetUserId, built off the engine's OWN live SLOT/GEN tables (NetUserId):
    // low word = slot, high word = generation.  The sweep keys on the entrant's UserIndex (ModelVisible
    // and ReadSwapperEntrant are both index-keyed) and hands us that index, but the SLOT/GEN tables are
    // SLOT-keyed -- so convert index -> live slot through the engine's own side table first (SlotOfIndex),
    // exactly as ApplyLoadout does.  Feeding the index straight to NetUserId would read the wrong slot
    // whenever slot != index (the general multi-remote case) and fabricate an id for a DIFFERENT entrant
    // (or none).  The resolved slot is the one DressLoadout dresses (SlotOfIndex(index) == s.mapped in the
    // dressable state), so the id's low word matches the entrant the rest of the sweep is driving.
    // SlotOfIndex false == the entrant is not in the table yet; NetUserId false == its slot/gen words are
    // not valid yet -- the exact checks the handler re-applies on a2[2]+0, so a success means the handler's
    // id resolution cannot fail.  Either false is transient: re-ask next tick.
    UserSlot slot(0);
    if (!SlotOfIndex(ng, UserIndex(index), slot)) return o;
    uint32_t entityId = 0;
    if (!NetUserId(ng, slot, entityId)) return o;
    o.idResolved = true;
    o.entityId   = entityId;

    // The 56-byte body, and the 3-qword wrapper the handler reads (a2[2] = &body).  Only +8 (chassis) is
    // set to a value that differs from the record's default, so the handler fires ONLY the model-changed
    // delegate; +16/+24/+32 stay 0 (the equip fields' default), a no-op on a freshly-admitted record.
    struct EquipBody {
        uint32_t entityId;   // +0
        uint32_t pad;        // +4
        uint64_t chassis;    // +8
        uint64_t equipA;     // +16
        uint64_t equipB;     // +24
        uint32_t equipC;     // +32
        uint32_t pad2;       // +36
        uint8_t  tail[16];   // pad to 0x38 (56)
    };
    static_assert(sizeof(EquipBody) >= 0x38, "the equipment body must be at least the 56 bytes the RECV reads");
    EquipBody body{};
    body.entityId = entityId;
    body.chassis  = chassisHash;
    uint64_t msg[3] = {0, 0, reinterpret_cast<uint64_t>(&body)};

    EquipRecvFn fn = (g_equipInject.active && g_equipInject.handler)
                         ? g_equipInject.handler
                         : Guarded<EquipRecvFn>(kEQUIP_RECV_VA, kEQUIP_RECV_VAPrologue, 0);
    if (!fn) return o;   // prologue moved / unmapped -> refuse, never call a guessed address
    o.handlerBound = true;

    // The shared re-entry latch every engine call this plugin issues from the stage-0 tick takes: a
    // callback that re-entered us cannot start a second RECV inside the first.  Refusal self-heals.
    PawnProvision::EngineCallLatch latch;
    if (!latch.taken) { o.reentrant = true; return o; }

    if (!CallEquipRecvGuarded(fn, cs, msg)) { o.faulted = true; return o; }
    o.issued = true;
    return o;
}

void EquipmentRecvInjectForTest(void* equipCs, void (*handler)(void* cs, void* msg)) {
    g_equipInject.cs      = equipCs;
    g_equipInject.handler = reinterpret_cast<EquipRecvFn>(handler);
    g_equipInject.active  = true;
}
void EquipmentSlotSymbolInjectForTest(uint64_t (*slotSymbol)(const char* slotName)) {
    g_equipInject.slotSymbol = slotSymbol;
}
void EquipmentRecvClearInjectForTest() { g_equipInject = EquipmentRecvInject{}; }

void RegisterSwapperResetSweep(SwapperResetSweepFn fn, void* ctx) {
    g_resetSweepFn  = fn;
    g_resetSweepCtx = ctx;
}
void ClearSwapperResetSweep() {
    g_resetSweepFn  = nullptr;
    g_resetSweepCtx = nullptr;
}
void Stage0SweepForTest(void* netGame) { RunStage0Sweep(netGame); }

void RegisterGameStateStageSweep(GameStateStageSweepFn fn, void* ctx) {
    g_gsStageSweepFn  = fn;
    g_gsStageSweepCtx = ctx;
}
void ClearGameStateStageSweep() {
    g_gsStageSweepFn  = nullptr;
    g_gsStageSweepCtx = nullptr;
}

// Drives the stage-3 tail the way the live detour does, for a test that owns a fake netGame.
void GameStateStageSweepForTest(void* netGame, uint64_t stage) {
    GameStateStageSweepAfterOriginal(netGame, stage);
}

bool InstallReplyDrainDriver() {
    // The route's kill switch also disarms its driver: pawn.reply_route off means the route never
    // promotes, so the drain would be a permanent no-op -- installing a detour for it would be a hook
    // nobody switched on.
    if (!config::Interface()->GetBool("pawn.reply_route", true)) return false;

    // A second load in the same process: the detour is still in the chain from the first, and
    // hookmgr would refuse to create it again. Re-arming the body IS the install here.
    if (g_replyDrainOriginal != nullptr) {
        g_replyDrainBodyArmed = true;
        return true;
    }

    void* trampoline = nullptr;
    const HookStatus created = hookmgr::Create(
        "NETGAME.UPDATE_DETOUR_MIN", facts::NETGAME_UPDATE, facts::NETGAME_UPDATE_PROLOGUE,
        sizeof facts::NETGAME_UPDATE_PROLOGUE,
        reinterpret_cast<void*>(&ReplyDrainNetGameUpdateDetour),
        reinterpret_cast<void**>(&trampoline), "pawn.reply_drain");
    if (created != HookStatus::Created) {
        // Expected, not a fault, when tick.mode=hook: hookmgr::Create's own config gate refuses because
        // pawn.reply_drain's row is NotDeclared in hook mode (RowDeclarationState value-gates it to slot
        // only), and tick's fallback already owns NETGAME.UPDATE so the byte-anchored guard would refuse
        // too.  In that mode the drain simply does not run; the fix targets tick.mode=slot, where the
        // row is Declared and the site is pristine, so this installs and SC7 attributes it to row 2.7.
        log::Direct(LogLevel::Info, "pawn",
                    "reply drain driver: dedicated NETGAME.UPDATE detour not created (%s)",
                    HookStatusName(created));
        return false;
    }
    g_replyDrainOriginal = reinterpret_cast<ReplyDrainNetGameUpdateFn>(trampoline);

    const HookStatus armed = hookmgr::Arm("NETGAME.UPDATE_DETOUR_MIN");
    if (armed != HookStatus::Armed) {
        log::Direct(LogLevel::Warn, "pawn",
                    "reply drain driver: dedicated NETGAME.UPDATE detour not armed (%s)",
                    HookStatusName(armed));
        return false;
    }
    g_replyDrainBodyArmed = true;
    log::Direct(LogLevel::Info, "pawn",
                "reply drain driver armed: dedicated always-armed NETGAME.UPDATE detour drives the "
                "reply drain every frame in-match, off the ImGui owner gate, independent of tick.mode");
    return true;
}

void SuspendReplyDrainBody() {
    if (!g_replyDrainBodyArmed) return;
    g_replyDrainBodyArmed = false;
    log::Direct(LogLevel::Info, "pawn",
                "reply drain body suspended: the NETGAME.UPDATE detour stays in the chain and now "
                "passes straight through to the original");
}

// The other half of SuspendReplyDrainBody. A second load re-arms the body of the detour that is
// already in the chain; it never creates one, so a driver that never installed stays uninstalled
// and this is not a hook site.
bool ResumeReplyDrainBody() {
    if (g_replyDrainOriginal == nullptr) return false;
    if (g_replyDrainBodyArmed) return true;
    g_replyDrainBodyArmed = true;
    log::Direct(LogLevel::Info, "pawn",
                "reply drain body resumed: this load re-arms the NETGAME.UPDATE detour's body, "
                "which the previous unload suspended");
    return true;
}

bool ReplyDrainBodyArmedForTest() { return g_replyDrainBodyArmed; }

ReplyRouteCensus ReplyDrainNetGameUpdateForTest(void* netGame, uint64_t stage) {
    NetGameUpdateStage0Body(netGame, stage);
    return ReplyRouteStatus();
}

void ReplyDrainSetBodyArmedForTest(bool armed) { g_replyDrainBodyArmed = armed; }

namespace {
char __fastcall ReplyDrainNoopOriginal(void*, uint64_t) { return 0; }
}  // namespace

// Stands in for "the detour is in the chain", which in a test process nothing installs. Resume
// only ever reads this pointer, so a stand-in is the whole of what it needs.
void ReplyDrainInstallForTest(bool installed) {
    g_replyDrainOriginal = installed ? &ReplyDrainNoopOriginal : nullptr;
}

// Everything the detour does EXCEPT calling the engine's original, which no test has. Same gate
// decision, so a case here measures the shipped one.
void ReplyDrainGatedForTest(void* netGame, uint64_t stage) {
    const ReplyDrainGate gate = ReplyDrainDecide();
    if (gate.pass_through) return;
    if (gate.run_body) NetGameUpdateStage0Body(netGame, stage);
    GameStateStageSweepAfterOriginal(netGame, stage);
}

bool ReplyRouteArmWith(void* ng, IWiretap* tap, const char* symbolsCsv) {
    if (g_reply.census.armed && g_reply.ng == ng) return g_reply.census.listeners != 0;
    ReplyRouteDisarm();
    g_reply.census = ReplyRouteCensus{};
    g_reply.census.armed = true;
    g_reply.ng = ng;
    if (!ng || !tap) {
        g_reply.census.diagnostic = "no CR15NetGame, or no wiretap to register through";
        return false;
    }
    g_reply.tap = tap;

    if (g_replyInject.publish) {
        g_reply.publish = g_replyInject.publish;
        g_reply.node    = g_replyInject.node;
        g_reply.msgId   = g_replyInject.msgId;
    } else if (g_replyInject.directHandler) {
        // Injected DIRECT test path: a stand-in for the shipped handler call.  node may point at a null
        // router -- the direct path never reads it -- so this exercises the pivot with no image mapped.
        g_reply.handler        = g_replyInject.directHandler;
        g_reply.directInjected = true;
        g_reply.node           = g_replyInject.node;
        g_reply.msgId          = g_replyInject.msgId;
    } else {
        // BINDING 1 -- the SHIPPED direct target.  The engine's own EntrantAdded handler sub_14017B850,
        // typed and byte-anchored: a rebase or a patched image that moved it out from under the
        // registration would leave a direct call reaching the wrong code, and this fails at bind instead.
        g_reply.handler = Guarded<EntrantAddedHandlerFn>(
            facts::NETGAME_MSG_ENTRANT_ADDED_HANDLER_VA,
            facts::NETGAME_MSG_ENTRANT_ADDED_HANDLER_VA_PROLOGUE, 0);
        // BINDING 1b -- the SHIPPED leave target, bound exactly like the add and beside it, because
        // it is the add's sibling in the same registration table against this same receiver.  It is
        // NOT part of ReplyRoutePromoted: a build where the leave does not anchor must still admit.
        g_reply.leaveHandler = Guarded<EntrantAddedHandlerFn>(
            kNETGAME_MSG_ENTRANT_REMOVED_HANDLER_VA,
            kNETGAME_MSG_ENTRANT_REMOVED_HANDLER_VAPrologue, 0);
        // The hub, kept for the census only -- the direct call does not dispatch through it.
        g_reply.node = Rd<void*>(ng, facts::NETGAME_BROADCASTER_OFF);
        // BINDING 2 -- the cached NSLobbyEntrantAdded id, read at runtime from the engine's own global;
        // the direct call does not need it, but the census reads it and a lobby that has populated it is
        // a live-session witness.  Fall back to the wiretap's own hash the way the registrar does.
        g_reply.msgId = ReadDataQword(kNETGAME_ENTRANT_ADDED_MSG_ID_VA);
        if (g_reply.msgId == 0) g_reply.msgId = tap->Symbol(kEntrantAddedStruct);
    }
    // After the branches, so a test can inject a leave stand-in over either route -- including over
    // the shipped Guarded bind, which is null with no image mapped.
    if (g_replyInject.leaveHandler) g_reply.leaveHandler = g_replyInject.leaveHandler;
    if (!ReplyRoutePromoted()) {
        g_reply.census.diagnostic = tap->Available()
            ? "the entrant-added handler did not bind by byte-anchor, or the netgame was null"
            : tap->Diagnostic();
        return false;
    }

#if defined(W2C_MUTATE_NO_LISTENER)
    // The shape this route must never ship in: promoted, and with no call site in the engine's
    // receive phase, so nothing can ever drain the queue.  tests/identity builds one target with
    // this defined.
    g_reply.census.diagnostic = "mutant: listener registration suppressed";
    return false;
#else
    // BINDING 3 -- inReceivePhase.  The predicate has no answer without a call site in the
    // engine's own pump, and this is where that call site is acquired.
    const char* csv = (symbolsCsv && symbolsCsv[0]) ? symbolsCsv : kDefaultSymbols;
    char one[128];
    size_t n = 0;
    for (const char* p = csv;; ++p) {
        if (*p != ',' && *p != 0) {
            if (n + 1 < sizeof one) one[n++] = *p;
            continue;
        }
        one[n] = 0;
        if (n != 0 && g_reply.census.listeners < kReplyListenMax) {
            const uint16_t h = g_reply.tap->Listen(one, &ReplyPumpThunk, nullptr);
            if (h != IWiretap::kInvalidHandle)
                g_reply.handles[g_reply.census.listeners++] = h;
        }
        n = 0;
        if (*p == 0) break;
    }
    if (g_reply.census.listeners == 0) {
        g_reply.census.diagnostic = g_reply.tap->Diagnostic();
        return false;
    }
    g_reply.census.diagnostic = "";
    return true;
#endif
}

void ReplyRouteInjectForTest(void* node, ReplyPublishFn publish, uint64_t msgId) {
    g_replyInject.node          = node;
    g_replyInject.publish       = publish;
    g_replyInject.directHandler = nullptr;
    g_replyInject.msgId         = msgId;
}

void ReplySpawnedSeedInjectForTest(const HopCallTable* calls) { g_replyInject.spawnedCalls = calls; }

void ReplyLeaveInjectForTest(EntrantAddedHandlerFn fn) { g_replyInject.leaveHandler = fn; }

void ReplyDirectInjectForTest(void* node, EntrantAddedHandlerFn fn, uint64_t msgId) {
    g_replyInject.node          = node;
    g_replyInject.publish       = nullptr;   // the direct path is taken only when publish is null
    g_replyInject.directHandler = fn;
    g_replyInject.msgId         = msgId;
}

const char* ReplyBailName(ReplyBail bail) {
    switch (bail) {
        case ReplyBail::None:               return "none";
        case ReplyBail::ReceiverNull:       return "receiver-null";
        case ReplyBail::ReceiverUnreadable: return "receiver-unreadable";
        case ReplyBail::HandlerUnbound:     return "handler-unbound";
        case ReplyBail::Reentrant:          return "reentrant";
        case ReplyBail::MsgUnreadable:      return "msg-unreadable";
        case ReplyBail::Faulted:            return "faulted";
    }
    return "?";
}

ReplyBail ReplyDirectCallForTest(void* receiver, EntrantAddedHandlerFn fn, uint16_t slot) {
    g_reply.ng             = receiver;
    g_reply.handler        = fn;
    g_reply.directInjected = true;
    const PawnProvision::ReplyMsg msg = PawnProvision::AuthorEntrantAddedMsg(slot);
    DirectEntrantAddedCall(msg);
    return g_reply.census.lastBail;
}

void ReplyRouteDisarm() {
    if (g_reply.tap) {
        for (uint16_t i = 0; i < g_reply.census.listeners; ++i)
            g_reply.tap->Unlisten(g_reply.handles[i]);
    }
    g_reply = ReplyPump{};
}

namespace {

// The queue the TICK writes and only the PUMP reads.  One entry per slot, because a second request
// for a slot already parked is the same request and not a second one.
void ReplyEnqueue(uint16_t slot, const JoinDesc& desc, IIdentity* id) {
    if (slot >= facts::ENTITY_USER_SLOT_TABLE_MAX) return;
    ReplyPending& p = g_reply.queue[slot];
    if (p.live) return;
    p.live = true;
    p.desc = desc;
    p.id   = id;
    ++g_reply.census.queued;
}

void ReplyNoteWrongPhase() { ++g_reply.census.wrongPhase; }

void ReplyDrain() {
    for (uint16_t slot = 0; slot < facts::ENTITY_USER_SLOT_TABLE_MAX; ++slot) {
        ReplyPending& p = g_reply.queue[slot];
        if (!p.live || !p.id) continue;
        // Taken off the queue BEFORE the attempt: a publish that faults, or a route that refuses,
        // must not leave an entry the next callback republishes for.  The provisioner's own retry
        // re-asks through the tick, which queues again.
        p.live = false;
        ++g_reply.census.drained;
        IIdentity* id = p.id;
        const JoinDesc desc = p.desc;
        UserIndex out(kInvalidU16);
        id->ProvisionReplayUser(UserSlot(slot), desc, out);
        // AUD-F2 witness, read SYNCHRONOUSLY on the same stack the dispatch ran on: the actor CI
        // the engine's remote arm left behind.  If this is a real handle the CI resolved WITHIN the
        // dispatch (CS_REGISTER's precondition), which is the F2 claim -- distinct from the
        // deferred DriveGate.physicsCi the per-frame update mints later.
        if (out.v != kInvalidU16) {
            ActorComponentRef ref;
            g_reply.census.lastActorCi =
                (ResolveComponent(g_reply.ng, out, ref) && ref.handle <= uint32_t(UINT16_MAX) &&
                 uint16_t(ref.handle) != kInvalidU16)
                    ? uint16_t(ref.handle) : kInvalidU16;
        }
    }
}

// Is there a parked request the pump has not drained?  ReplyEnqueue sets p.live; ReplyDrain clears
// it before the attempt.  A cheap scan so the heartbeat driver can no-op on the common frame.
bool AnyReplyQueued() {
    for (uint16_t i = 0; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i)
        if (g_reply.queue[i].live && g_reply.queue[i].id) return true;
    return false;
}

// The 40-byte argument the engine's own size-trampoline sub_14013CD50 builds for an 8-byte payload and
// hands the handler as a2 (decompiled: {a6, a5, a3, a4<=8?0:a3+8, a4-8}).  The handler reads ONLY a2+16
// -- **(a2+16) is the entrant slot key -- so the payload pointer at +16 is the one load-bearing field;
// the rest is zero, exactly what the trampoline writes for an 8-byte payload (size<=8 => +24=0, +32=0).
struct EntrantAddedArg {
    uint64_t        f0   = 0;        // a6
    uint64_t        f8   = 0;        // a5
    const uint64_t* pKey = nullptr;  // a3: points at the 8-byte payload; handler reads **(a2+16)
    uint64_t        f24  = 0;        // a3+8, past an 8-byte payload; unread by the handler
    uint64_t        f32  = 0;        // size-8 == 0 for an 8-byte payload; unread by the handler
};

// SEH lives in its own function with no C++ unwinding objects (MSVC C2712 forbids __try beside them).
// The one engine call the direct path makes -- sub_14017B850(receiver, &arg) -- is contained here so a
// bad receiver or a malformed arg raises into __except and admits nothing rather than crashing echovr.
// No dispatch bit, no router: the pivot dropped the 0x20000 manufacture entirely.  Never sub_140323790.
bool CallEntrantAddedGuarded(EntrantAddedHandlerFn fn, void* a1, void* a2) {
    __try {
        fn(a1, a2);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// THE ADMISSION'S OWN BEFORE/AFTER PICTURE, and the only place from which it can be
// taken: the engine allocates the entrant's actor and dereferences it inside this one call, so a
// reading taken anywhere else is taken at a different moment.  Four numbers per line --
//   rec.space / rec.actor   CR15NetGame + 592*slot + 384 / + 392, the user record the engine's own
//                           SET_PLAYER_ACTOR reads before it allocates and writes after it does.
//                           The actor guid is {classId:u16, index:u16, generation:u16, 0}.
// The pool the allocation comes out of is the admission gate's line, printed immediately above this
// one (pawn_provision.cpp), so it is not read twice here.
// Read-only, two lines per admitted slot, nothing per frame.
void LogAdmitRecord(uint16_t slot, const char* when) {
    void* const ng = g_reply.ng;
    if (ng == nullptr) return;
    // ng + 592*slot + 384 / + 392, written as the record the engine indexes: base 0x178, stride
    // 0x250, and the actor guid is the 16 bytes at record + 8 (SET_PLAYER_ACTOR :82 and :319).
    constexpr uint32_t kRecordActorOff = 8u;
    const uint8_t* const rec = static_cast<const uint8_t*>(ng) + facts::NETGAME_PAWN_TABLE_OFF +
                               size_t(facts::NETGAME_PAWN_TABLE_STRIDE) * slot;
    uint64_t space = 0, actor = 0;
    const bool haveRec = Readable(rec + kRecordActorOff, 16);
    if (haveRec) {
        memcpy(&space, rec + kRecordActorOff, sizeof space);
        memcpy(&actor, rec + kRecordActorOff + 8, sizeof actor);
    }

    void* gameSpace = nullptr;
    const uint8_t* const at = static_cast<const uint8_t*>(ng) + facts::NETGAME_CS_SPACE_OFF;
    if (Readable(at, sizeof gameSpace)) memcpy(&gameSpace, at, sizeof gameSpace);

    if (!haveRec) {
        log::Direct(LogLevel::Info, "pawn", "admit slot %u %s: space=%p, record unreadable",
                    unsigned(slot), when, gameSpace);
        return;
    }
    // The guid's three words, spelled out: a generation above zero is an entry a previous session
    // held, and the class id says which pool it came from.
    log::Direct(LogLevel::Info, "pawn",
                "admit slot %u %s: space=%p rec.space=%016llX rec.actor=%016llX "
                "(class %u entry %u gen %u)",
                unsigned(slot), when, gameSpace, (unsigned long long)space,
                (unsigned long long)actor, unsigned(actor & 0xFFFFu),  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed actor guid, not the engine no-slot field
                unsigned((actor >> 16) & 0xFFFFu), unsigned((actor >> 32) & 0xFFFFu));  // facts-ok: NETVAR.SLOT_NONE u16 masks over a packed actor guid, not the engine no-slot field
    // ENG-RELOAD-3 points (a) and (d): the same table beside the admission that allocates out of the
    // pool and resolves the handle three instructions later.  "before" on a reload is the reading
    // that decides the packet -- the entry about to be popped either has a row at the generation the
    // pool will stamp, or the resolve inside sub_140CB89B0 is already doomed.
    LogActorAliasRows(ng, when, slot);
}

// DIAG-DRAIN-NOOP Q2: call the engine's own EntrantAdded handler directly, in place of the broadcaster
// dispatch whose router is null offline.  Fully guarded: the receiver (g_reply.ng, the CR15NetGame the
// handler was registered against) and the fabricated arg are Readable-probed; the handler is byte-
// anchored at arm (g_reply.handler); the shared EngineCallLatch refuses a re-entry; SEH contains a fault.
bool DirectEntrantAddedCall(const PawnProvision::ReplyMsg& msg) {
    void* receiver = g_reply.ng;
    if (receiver == nullptr) { g_reply.census.lastBail = ReplyBail::ReceiverNull; return false; }
    // Refuse a null/unmapped receiver cleanly (the Option-A no-guessed-a1 rule).  A receiver that is
    // mapped-but-wrong faults deep in the handler; the SEH in CallEntrantAddedGuarded contains that.
    if (!Readable(receiver, sizeof(void*))) {
        g_reply.census.lastBail = ReplyBail::ReceiverUnreadable;
        return false;
    }
    EntrantAddedHandlerFn fn = g_reply.handler;
    if (fn == nullptr) { g_reply.census.lastBail = ReplyBail::HandlerUnbound; return false; }

    const uint64_t key = msg.slot;   // payload[0] == **(a2+16), the entrant slot key
    EntrantAddedArg arg;
    arg.pKey = &key;
    if (!Readable(&arg, sizeof arg)) { g_reply.census.lastBail = ReplyBail::MsgUnreadable; return false; }

    // The shared re-entry latch every engine call this plugin issues from the stage-0 tick takes: an
    // engine callback that re-entered us here is refused, not allowed to start a second admission inside
    // the first.  A refusal is self-healing -- the drain fires every frame and a later free frame admits.
    PawnProvision::EngineCallLatch latch;
    if (!latch.taken) { g_reply.census.lastBail = ReplyBail::Reentrant; return false; }

    ++g_reply.census.directCalls;
    LogAdmitRecord(uint16_t(msg.slot), "before");
    SteerActorPoolBeforeAdmission(g_reply.ng, msg.slot);
    if (!CallEntrantAddedGuarded(fn, receiver, &arg)) {
        g_reply.census.lastBail = ReplyBail::Faulted;
        return false;
    }
    LogAdmitRecord(uint16_t(msg.slot), "after");
    g_reply.census.lastBail = ReplyBail::None;
    ++g_reply.census.published;        // a call was issued -- parity with the old dispatch census
    ++g_reply.census.dispatchedLocal;  // AddRemoteUser-dispatch proxy: the handler calls it directly
    return true;
}

// The leave's own SEH belt.  Same reason and same shape as the add's: the engine call
// is alone in a function with no C++ unwinding objects, so a bad receiver or a slot the handler
// dislikes raises into __except and removes nothing rather than crashing echovr.
bool CallEntrantRemovedGuarded(EntrantAddedHandlerFn fn, void* a1, void* a2) {
    __try {
        fn(a1, a2);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Call the engine's own NSLobbyRemovingEntrant handler directly, the way the add is called: the
// receiver is g_reply.ng (the CR15NetGame both handlers were registered against), the argument is
// the same 40-byte trampoline frame with the slot qword at +16, the handler is byte-anchored at
// arm, the shared re-entry latch refuses a call from inside an engine callback, and SEH contains a
// fault.  The census fields are the add's -- one route, one set of counters.
bool LeaveHandlerBound() { return g_reply.leaveHandler != nullptr; }

bool DirectEntrantRemovedCall(uint16_t slot) {
    void* receiver = g_reply.ng;
    if (receiver == nullptr) { g_reply.census.lastBail = ReplyBail::ReceiverNull; return false; }
    // Probed as far as the counts the handler itself compares against, not just the first pointer:
    // this handler reads the slot table and both user counts before it decides anything.
    if (!Readable(receiver, size_t(facts::LOCAL_NETGAME_LOCAL_USER_COUNT_OFF) + sizeof(uint16_t))) {
        g_reply.census.lastBail = ReplyBail::ReceiverUnreadable;
        return false;
    }
    EntrantAddedHandlerFn fn = g_reply.leaveHandler;
    if (fn == nullptr) { g_reply.census.lastBail = ReplyBail::HandlerUnbound; return false; }

    const uint64_t key = uint64_t(slot);   // **(a2+16): the lobby slot, the only field it reads
    EntrantAddedArg arg;
    arg.pKey = &key;
    if (!Readable(&arg, sizeof arg)) { g_reply.census.lastBail = ReplyBail::MsgUnreadable; return false; }

    PawnProvision::EngineCallLatch latch;
    if (!latch.taken) { g_reply.census.lastBail = ReplyBail::Reentrant; return false; }

    ++g_reply.census.directCalls;
    if (!CallEntrantRemovedGuarded(fn, receiver, &arg)) {
        g_reply.census.lastBail = ReplyBail::Faulted;
        return false;
    }
    g_reply.census.lastBail = ReplyBail::None;
    return true;
}

// SEH-only belt around the whole drain (the surrounding engine reads -- ProvisionReplayUser's postcond
// hooks and ResolveComponent -- touch the netgame too).  No C++ unwinding objects here; the inner
// handler call has its own __except.  Restores nothing: the pivot writes no engine bit to undo.
void DrainReplyDirect() {
    __try {
        ReplyDrain();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // contained: an engine read around the direct call faulted; the drain's own guards fail closed.
    }
}

// HOP 3 PRECONDITION -- the entrant's team, off the same user word the engine's own spawn-point
// author reads: ENGINE.SPAWN_POINT_AUTHOR_VA indexes netgame + 376 + 592*index and does the
// shifting itself, so this hook and the engine cannot disagree about which team a place is
// authored for.  Bounded by the entrant count, so an index past the roster refuses instead of
// reading a word that is not a user's -- and a refusal issues no place at all.
bool HopTeamOf(void* ctx, uint16_t index, uint8_t* teamId, uint8_t* teamSlot) {
    ActorHopSurface* s = static_cast<ActorHopSurface*>(ctx);
    if (s == nullptr || s->netGame == nullptr || teamId == nullptr || teamSlot == nullptr) return false;
    void* ng = s->netGame();
    if (ng == nullptr) return false;
    if (!Readable(At(ng, kENTRANT_COUNT_OFF), sizeof(uint16_t))) return false;
    if (index >= Rd<uint16_t>(ng, kENTRANT_COUNT_OFF)) return false;
    *teamId   = TeamOf(ng, UserIndex(index));
    *teamSlot = TeamSlotOf(ng, UserIndex(index));
    return true;
}

// ---- HOP 4 -- the RemotePlayer CI's OWN record, re-read -------------------------------------
//
// CR15RemotePlayerCS::InitCI (ENTITY.REMOTE_PLAYER_CS_INITCI_VA) asks six sibling component
// systems for this actor's instance index IN THIS ORDER, stores each answer into the CI element,
// and returns on the first that answers PLAYER.CI_REQ_UNSET -- the return that raises
// "R14RemotePlayer requires <name>".  So the element records which requirement the engine last
// refused on, and this reads it rather than inferring it from anything a call returned.
//
// The element is located the way the engine locates it.  sub_140C82050 -- the only caller of the
// element initialiser, and the function that takes InitCI's address -- is called as
// `sub_140C82050(cs, *(_QWORD**)(cs + 192), cs + 200, InitCI, ...)`, and its element arithmetic is
// `imul r14, rcx, 340h` / `add r14, [rdi+38h]` with rdi = its own third argument.  cs + 200 + 0x38
// is cs + 0x100, which is PLAYER.CS_POOL_BASE_OFF, and 0x340 is PLAYER.CI_STRIDE: the engine's own
// pool base and stride, reached from the component system through the registry's own two ids
// rather than through a second copy of the same numbers.
// A pointer field, loaded byte-wise: the caller has already probed the field itself.
void* Deref(const void* base, uint32_t off) {
    void* p = nullptr;
    memcpy(&p, At(const_cast<void*>(base), off), sizeof p);
    return p;
}

constexpr uint32_t kPLAYER_CS_POOL_BASE_OFF   = facts::PLAYER_CS_POOL_BASE_OFF;
constexpr uint32_t kPLAYER_CS_COUNT_OFF       = facts::PLAYER_CS_COUNT_OFF;
constexpr uint32_t kPLAYER_CS_HANDLE_MAP_OFF  = facts::PLAYER_CS_HANDLE_MAP_OFF;
constexpr uint32_t kPLAYER_CS_REVERSE_MAP_OFF = facts::PLAYER_CS_REVERSE_MAP_OFF;
constexpr uint32_t kPLAYER_CI_STRIDE          = facts::PLAYER_CI_STRIDE;
constexpr uint32_t kPLAYER_CI_USERID_OFF      = facts::PLAYER_CI_USERID_OFF;
constexpr uint32_t kPLAYER_CI_INIT_WITNESS_OFF = facts::PLAYER_CI_INIT_WITNESS_OFF;

// InitCI's own order, and the order is the whole point: the engine returns on the FIRST one that
// is unset, so a reader walking them in any other order would name a requirement the engine has
// not reached yet.  Index i here is PhysicsCiReq(i + 1).
constexpr uint32_t kCiRequirementOffs[] = {
    facts::PLAYER_CI_REQ_NCAPHYSICS_OFF,     // cr15remoteplayercs.cpp:328, checked first
    facts::PLAYER_CI_REQ_NCATRANSFORM_OFF,   // :332
    facts::PLAYER_CI_REQ_NCAMODEL_OFF,       // :336
    facts::PLAYER_CI_REQ_R15BODY_OFF,        // :340
    facts::PLAYER_CI_REQ_R15NETPHYSICS_OFF,  // :344 -- the same word as PLAYER.CI_HANDLE_OFF
    facts::PLAYER_CI_REQ_R15TOUCHINTERACT_OFF,  // :348, checked last
};
static_assert(sizeof(kCiRequirementOffs) / sizeof(kCiRequirementOffs[0]) ==
                  size_t(PawnProvision::PhysicsCiReq::R15TouchInteract) -
                      size_t(PawnProvision::PhysicsCiReq::None),
              "one offset per named requirement, in InitCI's order, or the census names the wrong "
              "one");

// The entrant's element, or null with the reason.  The walk is Entity::PlayerOf's -- the reverse
// map gives the handle, the forward map gives the pool index, the user id word settles identity --
// because that is the route the engine's own pool accessor takes and the one this project's body
// reads already run on.
void* PlayerCiElement(void* cs, uint16_t index, PawnProvision::PhysicsCiReq& why) {
    why = PawnProvision::PhysicsCiReq::Unreadable;
    if (cs == nullptr) return nullptr;
    if (!Readable(At(cs, kPLAYER_CS_COUNT_OFF), sizeof(uint16_t)) ||
        !Readable(At(cs, kPLAYER_CS_POOL_BASE_OFF), sizeof(void*)) ||
        !Readable(At(cs, kPLAYER_CS_HANDLE_MAP_OFF), sizeof(void*)) ||
        !Readable(At(cs, kPLAYER_CS_REVERSE_MAP_OFF), sizeof(void*))) {
        return nullptr;
    }
    const uint16_t live = Rd<uint16_t>(cs, kPLAYER_CS_COUNT_OFF);
    void* pool = Deref(cs, kPLAYER_CS_POOL_BASE_OFF);
    void* fwd  = Deref(cs, kPLAYER_CS_HANDLE_MAP_OFF);
    void* rev  = Deref(cs, kPLAYER_CS_REVERSE_MAP_OFF);
    if (pool == nullptr || fwd == nullptr || rev == nullptr) return nullptr;

    for (uint16_t i = 0; i < live; ++i) {
        if (!Readable(At(rev, 2u * i), sizeof(uint16_t))) return nullptr;
        const uint16_t handle = Rd<uint16_t>(rev, uint32_t(2u * i));
#ifndef W2P1_MUTATE_HANDLE_FFFF_UNCOVERED
        // A HOLE in the reverse map, and the engine leaves them: the dense index is not
        // compacted on removal.  Reading the forward map at 4*0xFFFF turns "this entrant is
        // not in the pool" into "the pool is unreadable", which is a different answer.
        if (handle == kInvalidU16) continue;
#endif
        if (!Readable(At(fwd, 4u * handle), sizeof(uint16_t))) return nullptr;
        const uint16_t slot = Rd<uint16_t>(fwd, uint32_t(4u * handle));
        void* ci = At(pool, uint64_t(kPLAYER_CI_STRIDE) * slot);
        if (!Readable(At(ci, kPLAYER_CI_USERID_OFF), sizeof(uint16_t))) return nullptr;
        if (Rd<uint16_t>(ci, kPLAYER_CI_USERID_OFF) == index) {
            why = PawnProvision::PhysicsCiReq::None;
            return ci;
        }
    }
    // Every live element was read and none carries this entrant's user id, so the component
    // system has allocated nothing for it and InitCI has not run over it.  That is NotAttempted
    // and not Unreadable: the walk itself succeeded.
    why = PawnProvision::PhysicsCiReq::NotAttempted;
    return nullptr;
}

// The ONE reader both hop-4 hooks answer from, so `physicsCi` can never disagree with the
// requirement name printed beside it.
PawnProvision::PhysicsCiReq PhysicsCiRequirement(void* ctx, uint16_t index) {
    ActorHopSurface* s = static_cast<ActorHopSurface*>(ctx);
    if (s == nullptr || s->remotePlayerCs == nullptr) return PawnProvision::PhysicsCiReq::Unknown;
    // The same bound every other hook takes, and for the same reason: an index past the entrant
    // table is not a user, so matching a CI's user id word against it would be matching noise.
    if (s->netGame == nullptr) return PawnProvision::PhysicsCiReq::Unknown;
    void* ng = s->netGame();
    if (ng == nullptr) return PawnProvision::PhysicsCiReq::Unknown;
    if (!Readable(At(ng, kENTRANT_COUNT_OFF), sizeof(uint16_t)))
        return PawnProvision::PhysicsCiReq::Unreadable;
    if (index >= Rd<uint16_t>(ng, kENTRANT_COUNT_OFF))
        return PawnProvision::PhysicsCiReq::IndexOutOfRange;

    PawnProvision::PhysicsCiReq why = PawnProvision::PhysicsCiReq::Unreadable;
    void* ci = PlayerCiElement(s->remotePlayerCs(), index, why);
    if (ci == nullptr) return why;

#ifdef W2P1_MUTATE_HOP4_TRUSTS_RETURN
    // The defect this hop exists not to have: decide on what a call answered rather than on the
    // engine's own record.  The `spawned` getter says the participation was authored, which says
    // nothing at all about whether the component instance was built.
    if (s->calls != nullptr && s->calls->spawnedGet != nullptr)
        return s->calls->spawnedGet(ng, index) != 0 ? PawnProvision::PhysicsCiReq::None
                                                    : PawnProvision::PhysicsCiReq::NotAttempted;
#endif

    // The requirement words are stored as dwords and COMPARED AS WORDS by the engine itself
    // (`cmp r8w, bx` against `mov ebx, 0FFFFh`), so this compares the word.  The low half is the
    // sibling system's instance index; PLAYER.CI_HANDLE_OFF reads the same word at +0x1C.
    for (size_t i = 0; i < sizeof(kCiRequirementOffs) / sizeof(kCiRequirementOffs[0]); ++i) {
        if (!Readable(At(ci, kCiRequirementOffs[i]), sizeof(uint16_t)))
            return PawnProvision::PhysicsCiReq::Unreadable;
        if (Rd<uint16_t>(ci, kCiRequirementOffs[i]) == uint16_t(facts::PLAYER_CI_REQ_UNSET))
            return PawnProvision::PhysicsCiReq(size_t(PawnProvision::PhysicsCiReq::None) + 1 + i);
    }

    // PLAYER.CI_INIT_WITNESS_OFF IS EIGHT BYTES.  `mov [rdi+1D8h], rax` carries REX.W and the
    // field is re-read as a qword at two further sites.  The pool memsets the element to zero on
    // allocation, so a truncated read of a pointer whose low dword happens to be zero would answer
    // "InitCI never ran" about an element it demonstrably did.
#ifdef W2P1_MUTATE_WITNESS_READ_4_BYTES
    uint32_t witness = 0;
    if (!Readable(At(ci, kPLAYER_CI_INIT_WITNESS_OFF), sizeof(witness)))
        return PawnProvision::PhysicsCiReq::Unreadable;
    memcpy(&witness, At(ci, kPLAYER_CI_INIT_WITNESS_OFF), sizeof witness);
#else
    uint64_t witness = 0;
    if (!Readable(At(ci, kPLAYER_CI_INIT_WITNESS_OFF), sizeof(witness)))
        return PawnProvision::PhysicsCiReq::Unreadable;
    memcpy(&witness, At(ci, kPLAYER_CI_INIT_WITNESS_OFF), sizeof witness);
#endif
    // Every requirement is met AND the init completed.  Either half alone is not the answer: a
    // zeroed element meets every word-compare it is asked and has had nothing done to it.
    if (witness == 0) return PawnProvision::PhysicsCiReq::NotAttempted;
    return PawnProvision::PhysicsCiReq::None;
}

// Hop 4's postcondition and hop 4's WHY, off the same read.  `physicsCi` is true only for None --
// Unknown, Unreadable and every named requirement are all "not built", and none of them is a pass.
bool HopPhysicsCi(void* ctx, uint16_t index) {
    return PhysicsCiRequirement(ctx, index) == PawnProvision::PhysicsCiReq::None;
}

PawnProvision::PhysicsCiReq HopPhysicsCiAwaiting(void* ctx, uint16_t index) {
    return PhysicsCiRequirement(ctx, index);
}

}  // namespace

// The one external door onto the hooks above, which have internal linkage: the shipped Identity
// and a test both take their surface from here, so a test can never be exercising a binding the
// product does not use.
//
// ★ AUD-W2-P1-29 F1 IS CLOSED HERE.  S14.7's contract table asks for `physicsCiAwaiting` bound to
// the CI requirement read and `physicsCi` = `Awaiting()==None`, and BOTH ARE NOW BOUND.  What
// blocked it was spelling, not shape: every row the read needs -- PLAYER.CI_REQ_{NCAPHYSICS,
// NCATRANSFORM,NCAMODEL,R15BODY,R15NETPHYSICS,R15TOUCHINTERACT}_OFF, PLAYER.CI_REQ_UNSET and
// PLAYER.CI_INIT_WITNESS_OFF -- was filed PC_PRESENT / U and declared an incomplete type, so
// naming one was a compile error.  FACTS-CI-PROMOTE (6f705e2, fb3cacf) promoted all nine to
// V-PRIMARY off ENTITY.REMOTE_PLAYER_CS_INITCI_VA's own stores, and the reader above names them.
//
// The fallback that was REFUSED then is still refused: PLAYER.CI_HANDLE_OFF answers only "some
// requirement at or before R15NetPhysics is outstanding" and can never answer `None`, so binding
// it would have made the census name a requirement it could not know.  The read here is the
// engine's own -- the six words in InitCI's order, then the eight-byte init witness -- and it
// answers `Unreadable` or `IndexOutOfRange` where it cannot ask, never a wait it did not observe.
// Everything else IS bound: `placed` reads the
// engine's own `user[N]|spawned` boolean (S14.2 gave it the readable postcondition it never had),
// and the two participation hooks are the packet's whole authored surface.
// A2's KEY, and it is the registry's string rather than a copy of it.  The engine formats
// `user[%llu]|spawned` at both sites that author an entrant's record; a checker carrying its own
// copy of a format certifies broken output, so the format is read from the registry here and the
// per-slot key -- the engine's own `spawned` at its line 87 -- is DERIVED from the same string as
// the tail past the separator.  Two keys, one fact, and they cannot drift apart.
const char* SpawnedKeyFormat() {
#ifdef W2P1_MUTATE_A2_KEY_SPRINTF_PERCENT_D
    return "user[%d]|spawned";
#else
    return facts::NETGAME_USER_SPAWNED_KEY_FMT_VA_TEXT;
#endif
}

const char* SpawnedSlotKey() {
    const char* fmt = SpawnedKeyFormat();
    for (const char* p = fmt; *p; ++p)
        if (*p == '|') return p + 1;
    return fmt;
}
bool FormatSpawnedKey(char* out, size_t n, uint64_t slot) {
    if (out == nullptr || n == 0) return false;
    const int w = snprintf(out, n, SpawnedKeyFormat(), static_cast<unsigned long long>(slot));
    return w > 0 && size_t(w) < n;
}

void BindActorHops(ActorHopSurface& s) {
    s.hops = PawnProvision::ActorHops{};
    s.hops.ctx              = &s;
    s.hops.inRemotePlayerCs = HopInRemotePlayerCs;
    s.hops.enabled          = HopEnabled;
    s.hops.placed           = HopSpawned;
    s.hops.isLocalUser      = HopIsLocalUser;
    s.hops.writeSpawned     = HopWriteSpawned;
    s.hops.assignTeam       = HopAssignTeam;
    s.hops.teamOf           = HopTeamOf;
    s.hops.refreshPlayerActor = HopRefreshPlayerActor;
    s.hops.spawnNotify        = HopSpawnNotify;
    s.hops.actorAssetsReady   = HopActorAssetsReady;
    s.hops.actorGuid          = HopActorGuid;
    s.hops.participatePair    = HopParticipatePair;
    s.hops.seededBeforeAdd    = HopSeededBeforeAdd;
#ifdef W2P1_MUTATE_HOP4_UNBOUND
    // The state this packet ENDED: hop 4 null, the chain stopping at Hop::PhysicsCi forever and
    // the census printing a requirement it could not have read.
#else
    s.hops.physicsCi        = HopPhysicsCi;
    s.hops.physicsCiAwaiting = HopPhysicsCiAwaiting;
#endif
#ifdef W2P1_MUTATE_PHYSICS_CI_STUBBED
    s.hops.physicsCi        = [](void*, uint16_t) { return true; };
#endif
}


// =============================================================================================
// THE LOADOUT DRESS -- L1..L6.  ARCH-23 S14.8 / S14.8b / S14.8c.
// =============================================================================================

namespace {

// The twenty names IDENT.LOADOUT_DOC_AUTHOR_VA writes, in ITS order, straight off the registry.
const char* const kSlotNames[] = {
    facts::IDENT_LOADOUT_SLOT_NAME_00_TEXT, facts::IDENT_LOADOUT_SLOT_NAME_01_TEXT,
    facts::IDENT_LOADOUT_SLOT_NAME_02_TEXT, facts::IDENT_LOADOUT_SLOT_NAME_03_TEXT,
    facts::IDENT_LOADOUT_SLOT_NAME_04_TEXT, facts::IDENT_LOADOUT_SLOT_NAME_05_TEXT,
    facts::IDENT_LOADOUT_SLOT_NAME_06_TEXT, facts::IDENT_LOADOUT_SLOT_NAME_07_TEXT,
    facts::IDENT_LOADOUT_SLOT_NAME_08_TEXT, facts::IDENT_LOADOUT_SLOT_NAME_09_TEXT,
    facts::IDENT_LOADOUT_SLOT_NAME_10_TEXT, facts::IDENT_LOADOUT_SLOT_NAME_11_TEXT,
    facts::IDENT_LOADOUT_SLOT_NAME_12_TEXT, facts::IDENT_LOADOUT_SLOT_NAME_13_TEXT,
    facts::IDENT_LOADOUT_SLOT_NAME_14_TEXT, facts::IDENT_LOADOUT_SLOT_NAME_15_TEXT,
    facts::IDENT_LOADOUT_SLOT_NAME_16_TEXT, facts::IDENT_LOADOUT_SLOT_NAME_17_TEXT,
    facts::IDENT_LOADOUT_SLOT_NAME_18_TEXT, facts::IDENT_LOADOUT_SLOT_NAME_19_TEXT,
};
constexpr size_t kSlotNameCount = sizeof kSlotNames / sizeof kSlotNames[0];
static_assert(kSlotNameCount == 20, "the document author writes twenty slots");

// Both engine call sites seed the hash with -1, and the accumulator is not carried between
// children, so hashing one value alone with this seed reproduces the recorded itemId.
constexpr uint64_t kHashSeed = ~uint64_t(0);
// IDENT.SYMBOL_FROM_NAME_VA returns its SEED for a null or empty string, so this one value is
// both hash(NULL), hash("") and the engine's own unfilled-pair sentinel.  Never compared to
// itself: see LoadoutRefusal::PairUnfilled.
constexpr uint64_t kUnfilledItem = ~uint64_t(0);

// THE ROWS L1 DOES NOT COMPARE, as one predicate, because L5's acceptance asserts
// `matched + pending + alignGated == compared` and that is an identity between two different
// populations the moment the two loops skip different rows.  L3 #34: L1 skipped a row whose
// ITEM was the empty-slot sentinel, L5 skipped only rows whose KEY was, the four tallies summed
// to 20 against a `compared` of 19, and the acceptance refused a dress the engine had performed.
// Padding (a zero key) and an empty slot (either half of the pair carrying the engine's -1) are
// both rows the player wore nothing in: there is no item to verify and nothing to attribute.
inline bool RecordedPairIsPadding(const LoadoutPair& p) { return p.slotKey == 0; }
inline bool RecordedPairIsEmptySlot(const LoadoutPair& p) {
    return p.slotKey == kUnfilledItem || p.itemId == kUnfilledItem;
}
inline bool RecordedPairNotCompared(const LoadoutPair& p) {
    return RecordedPairIsPadding(p) || RecordedPairIsEmptySlot(p);
}
constexpr int32_t  kHashUnbounded = -1;
// S14.8c: `a3` gates only a tail callback whose effect is UNDECODED.  A call whose side effect
// cannot be named has no place in a verification, and the parser registers the reverse names by
// its own direct call, so nothing downstream depends on our passing 1.
#ifdef W2P1_MUTATE_HASHER_A3_ONE
constexpr int32_t kHashWithCallback = 1;
#else
constexpr int32_t kHashWithCallback = 0;
#endif

const char* const kPathFmt = facts::IDENT_LOADOUT_INSTANCE_PATH_FMT_TEXT;

// The segment between the 2nd and 3rd '|' of the engine's own template -- read out of the
// template rather than spelled a second time in this file.
bool TemplateSegment(size_t which, char* out, size_t cap) {
    if (!out || cap == 0) return false;
    const char* p = kPathFmt;
    for (size_t i = 0; i < which; ++i) {
        const char* bar = strchr(p, '|');
        if (!bar) return false;
        p = bar + 1;
    }
    const char* end = strchr(p, '|');
    const size_t n = end ? size_t(end - p) : strlen(p);
    if (n + 1 > cap) return false;
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

struct LoadoutDrive {
    void* ng;
    UserSlot slot;
    UserIndex index;
    const RecordedLoadout* rec;
    const LoadoutEngine* eng;
    LoadoutSteps* out;
    LoadoutPhase phase;
};

LoadoutRefusal Step(LoadoutSteps& o, LoadoutStep s, LoadoutRefusal r) {
    o.outcome[size_t(s)] =
        r == LoadoutRefusal::None ? LoadoutOutcome::Issued : LoadoutOutcome::Refused;
    o.why[size_t(s)] = r;
    return r;
}

void Mark(const LoadoutDrive& d, const char* call) {
    PawnProvision::MarkHop(0, call, d.eng, d.index.v);
}

// ---- L1 -------------------------------------------------------------------------------------
LoadoutRefusal VerifyDocument(const LoadoutDrive& d) {
    const RecordedLoadout& rec = *d.rec;
    const LoadoutEngine& eng = *d.eng;
    LoadoutSteps& o = *d.out;

    // The Source phase parses a plain body straight out of the recording's own profile document and
    // carries no live customisation component, so the recorded-pair/key fidelity (slot keys, filled-
    // pair count, per-pair hash) has nothing to compare against and does not apply.  For this phase
    // L1 keeps only "a document is recorded" (RecordedLoadoutRefusal's first arm) plus the document-
    // bound pointer check, then returns; L3's own instanceCount==0 -> ParseDidNotFill remains the
    // emptiness gate, so an actually-empty document is still refused.
    const bool sourcePhase = d.phase == LoadoutPhase::Source;

#ifndef W2P1_MUTATE_GUESS_ON_ABSENT_DOCUMENT
    if (sourcePhase) {
        if (!rec.documentRecorded) return LoadoutRefusal::DocumentNotRecorded;
    } else {
        const LoadoutRefusal recorded = RecordedLoadoutRefusal(rec);
        if (recorded != LoadoutRefusal::None) return recorded;
    }
#endif
    if (eng.doc == nullptr || eng.doc->json == nullptr || eng.doc->pathElem == nullptr ||
        eng.doc->name == nullptr || eng.doc->kind == nullptr || eng.doc->stringValue == nullptr)
        return LoadoutRefusal::DocumentNotRecorded;
    if (sourcePhase) return LoadoutRefusal::None;
    if (eng.hash == nullptr || eng.instanceSymbol == nullptr) return LoadoutRefusal::NotBound;
    const uint16_t filled = rec.filledPairCount;

    // The hasher's own control: the twenty known names must reproduce the recorded slot keys, so
    // a wrong table, seed, folding or bound resolves none of them and this refuses before an item
    // is ever compared.  Its blind spot is the null/empty path, which the sentinel guard and
    // PairUnfilled cover instead -- every name here is short and non-empty.
    uint64_t nameHash[kSlotNameCount];
    for (size_t i = 0; i < kSlotNameCount; ++i) {
        Mark(d, "loadout L1 hash slot name");
        nameHash[i] = eng.hash(kSlotNames[i], kHashSeed, kHashWithCallback, kHashUnbounded, nullptr);
        // A name that hashed to the padding value or to the unfilled sentinel would make an empty
        // row and a real one indistinguishable even for a filled slot.
        if (nameHash[i] == 0 || nameHash[i] == kUnfilledItem)
            return LoadoutRefusal::SentinelCollision;
    }

    char instance[64] = {0};
    if (!TemplateSegment(2, instance, sizeof instance)) return LoadoutRefusal::InstanceNameUnknown;
    Mark(d, "loadout L1 hash instance name");
    const uint64_t templateSym =
        eng.hash(instance, kHashSeed, kHashWithCallback, kHashUnbounded, nullptr);
    Mark(d, "loadout L1 read live instance symbol");
    const uint64_t liveSym = eng.instanceSymbol();
    if (liveSym == 0 || liveSym != templateSym) return LoadoutRefusal::InstanceNameUnknown;

    // The parent the twenty leaves hang off: the template with its "|%s" tail cut, so the path
    // this walk uses and the path the engine's own author writes cannot come apart.
    char parent[128] = {0};
    {
        const char* tail = strrchr(kPathFmt, '|');
        if (!tail) return LoadoutRefusal::InstanceNameUnknown;
        const size_t n = size_t(tail - kPathFmt);
        if (n + 1 > sizeof parent) return LoadoutRefusal::InstanceNameUnknown;
        memcpy(parent, kPathFmt, n);
        parent[n] = 0;
    }

    // One walk of the document, matching each child's own name against the twenty.  A child count
    // past the twenty names is a document we do not model and the extra children are ignored --
    // every recorded pair still has to find its node below or the pair refuses.
    void* nodeFor[kSlotNameCount] = {nullptr};
    {
        const entity_json::CJsonReader& r = *eng.doc;
        void* prev = nullptr;
        // ONE mark for the walk, not one per child: the loop is bounded at 256 and a per-child
        // mark would spend the whole tick's marker budget inside L1.
        Mark(d, "loadout L1 walk document children");
        for (size_t guard = 0; guard < 256; ++guard) {
            void* node = r.pathElem(r.json, parent, prev);
            if (node == nullptr) break;
            const char* nm = r.name(r.json, node);
            if (nm) {
                for (size_t j = 0; j < kSlotNameCount; ++j)
                    if (nodeFor[j] == nullptr && strcmp(nm, kSlotNames[j]) == 0) nodeFor[j] = node;
            }
            prev = node;
        }
    }

    const entity_json::CJsonReader& r = *eng.doc;
    uint16_t compared = 0;
    uint16_t unfilled = 0;
    for (size_t i = 0; i < kSlotNameCount; ++i) {
        const LoadoutPair& p = rec.pairs[i];
#ifndef W2P1_MUTATE_COMPARE_ALL_TWENTY
        // Exactly the pairs the capture says it HAS.  A zero row is padding ReadAppliedPairs
        // wrote, not a slot the recording lost, and the count above is what tells them apart.
        if (RecordedPairIsPadding(p)) continue;
#endif
#ifndef W2P1_MUTATE_EMPTY_KEY_RESOLVED
        // AN EMPTY ENGINE PAIR, ASKED BEFORE THE KEY IS RESOLVED.  The
        // container is fixed width and the engine writes -1 into BOTH halves of a slot the
        // player wore nothing in, so a -1 KEY is not a key: SentinelCollision has already
        // proved that no slot name hashes to it, which makes the resolution below certain
        // to fail.  Resolved first, a legitimately empty pair refuses SlotKeyUnresolved and
        // takes the whole document with it -- one bare slot and the entrant wears nothing.
        if (p.slotKey == kUnfilledItem) {
            ++unfilled;
            continue;
        }
#endif
        size_t j = kSlotNameCount;
        for (size_t k = 0; k < kSlotNameCount; ++k)
            if (nameHash[k] == p.slotKey) { j = k; break; }
        if (j == kSlotNameCount) return LoadoutRefusal::SlotKeyUnresolved;

        o.skewSlot = uint16_t(j);

#ifndef W2P1_MUTATE_SENTINEL_COMPARES_EQUAL
        // The engine's container is FIXED WIDTH and an empty slot carries -1, so a recorded -1
        // is a slot the player wore nothing in.  It is skipped and counted, never refused: the
        // document is installed as-is and the engine parses its own empty slots out of it.
        // There is nothing here to verify either -- hash(NULL) == hash("") == -1 == the seed, so
        // comparing a sentinel against an absent node would report a slot verified having
        // compared nothing (AUD-ARCH-23-S14 s14.8c).
#ifdef W2P1_MUTATE_UNFILLED_REFUSES_DRESS
        if (p.itemId == kUnfilledItem) return LoadoutRefusal::PairUnfilled;
#else
        if (p.itemId == kUnfilledItem) {
#ifdef W2P1_MUTATE_UNFILLED_COUNTED_AS_VERIFIED
            ++compared;
#else
            ++unfilled;
#endif
            continue;
        }
#endif
#endif

        // Marked FILLED by the capture and the document carries no node for its slot: the pairs
        // and the profile came out of two different engine objects and they disagree.
        if (nodeFor[j] == nullptr) return LoadoutRefusal::PairUnfilled;

#ifndef W2P1_MUTATE_HASH_BEFORE_KIND_CHECK
        // IDENT.JSON_NODE_STRING returns its third argument VERBATIM for a missing node or a
        // kind that is not a string, so the kind is asked first and the default handed in is 0,
        // never a buffer of ours: "not a string" must be a null return and not a hash of our
        // own stack.
        if (r.kind(r.json, nodeFor[j]) != facts::IDENT_JSON_KIND_STRING)
            return LoadoutRefusal::SlotValueNotString;
#endif
#ifdef W2P1_MUTATE_HASH_BEFORE_KIND_CHECK
        // The defect this models: the engine's own call shape hands the accessor a STACK BUFFER
        // as its default, so a node that is not a string hashes whatever bytes sit in that local.
        char scratch[32] = "stale-stack-bytes";
        const char* value = r.stringValue(r.json, nodeFor[j], scratch);
#else
        const char* value = r.stringValue(r.json, nodeFor[j], nullptr);
#endif
        if (value == nullptr) return LoadoutRefusal::SlotValueNotString;
        Mark(d, "loadout L1 hash document value");
        const uint64_t h = eng.hash(value, kHashSeed, kHashWithCallback, kHashUnbounded, nullptr);
#ifndef W2P1_MUTATE_HASH_NOT_VERIFIED
        // The two sides come from different engine objects -- the pairs from the live
        // customisation component, the document from the account profile -- so a disagreement is
        // a coherence fault with its own name, not a missing datum.
        if (h != p.itemId) return LoadoutRefusal::DocumentPairsSkew;
#endif
        ++compared;
    }
    o.skewSlot = kLoadoutNoSlot;
#ifdef W2P1_MUTATE_SKIP_ZERO_ROWS_SILENTLY
    compared = uint16_t(kSlotNameCount);
#endif
    o.compared = compared;
    o.unfilled = unfilled;
    // compared == 0 is never a pass: a dress that verified nothing verified nothing, and an
    // entrant whose every recorded slot is empty is LoadoutNoSlotsRecorded, not a silent pass.
    if (compared == 0) return LoadoutRefusal::NoSlotsRecorded;
    // Every recorded pair is accounted for -- verified, or skipped as an empty slot.
    // filledPairCount is the number of REAL rows the engine's container held (capture writes
    // loadoutSlotsAvailable, src/capture/identity_block.h), not the number wearing an item, so
    // the SUM is what has to match it and a loop that walked a different set of rows refuses.
    if (uint16_t(compared + unfilled) != filled) return LoadoutRefusal::PairCountMismatch;

#ifdef W2P1_MUTATE_VERIFY_THEN_AUTHOR
    if (eng.setString) eng.setString(r.json, parent, kSlotNames[0]);
#endif
    return LoadoutRefusal::None;
}

}  // namespace

// The recording alone, asked once.  L1 runs this before it touches the document, and the census
// runs it with no engine at all, so "this recording cannot be verified" has exactly one spelling.
LoadoutRefusal RecordedLoadoutRefusal(const RecordedLoadout& rec) {
    if (!rec.documentRecorded) return LoadoutRefusal::DocumentNotRecorded;
    // A v1 block carries items BY POSITION and no slot key.  Position is a guess the moment the
    // component's order is not the name table's, and that order is not established.
    if (!rec.hasSlotKeys) return LoadoutRefusal::SlotKeysNotRecorded;
    uint16_t filled = rec.filledPairCount;
#ifdef W2P1_MUTATE_COUNT_UNKNOWN_AS_ZERO
    if (filled == kLoadoutPairCountUnknown) filled = 0;
#else
    if (filled == kLoadoutPairCountUnknown) return LoadoutRefusal::PairCountUnknown;
#endif
    if (filled == 0) return LoadoutRefusal::NoSlotsRecorded;
    if (filled > kSlotNameCount) return LoadoutRefusal::PairCountMismatch;
    uint16_t present = 0;
    for (size_t i = 0; i < kSlotNameCount; ++i)
        if (rec.pairs[i].slotKey != 0) ++present;
    if (present != filled) return LoadoutRefusal::PairCountMismatch;
    return LoadoutRefusal::None;
}

const char* LoadoutRefusalName(LoadoutRefusal r) {
    switch (r) {
        case LoadoutRefusal::None:                   return "None";
        case LoadoutRefusal::DocumentNotRecorded:    return "LoadoutDocumentNotRecorded";
        case LoadoutRefusal::SlotKeysNotRecorded:    return "LoadoutSlotKeysNotRecorded";
        case LoadoutRefusal::PairCountUnknown:       return "LoadoutPairCountUnknown";
        case LoadoutRefusal::NoSlotsRecorded:        return "LoadoutNoSlotsRecorded";
        case LoadoutRefusal::PairCountMismatch:      return "LoadoutPairCountMismatch";
        case LoadoutRefusal::InstanceNameUnknown:    return "LoadoutInstanceNameUnknown";
        case LoadoutRefusal::SlotKeyUnresolved:      return "LoadoutSlotKeyUnresolved";
        case LoadoutRefusal::SlotNodeAbsent:         return "LoadoutSlotNodeAbsent";
        case LoadoutRefusal::SlotValueNotString:     return "LoadoutSlotValueNotString";
        case LoadoutRefusal::PairUnfilled:           return "LoadoutPairUnfilled";
        case LoadoutRefusal::DocumentPairsSkew:      return "LoadoutDocumentPairsSkew";
        case LoadoutRefusal::SentinelCollision:      return "LoadoutSentinelCollision";
        case LoadoutRefusal::NotBound:               return "LoadoutStepNotBound";
        case LoadoutRefusal::DocumentNotAttached:    return "LoadoutDocumentNotAttached";
        case LoadoutRefusal::DocumentNotUsable:      return "LoadoutDocumentNotUsable";
        case LoadoutRefusal::ContainerUnreadable:    return "LoadoutContainerUnreadable";
        case LoadoutRefusal::ParseDidNotFill:        return "LoadoutParseDidNotFill";
        case LoadoutRefusal::PublishFlagUnexpected:  return "LoadoutPublishFlagUnexpected";
        case LoadoutRefusal::PublishDidNotTake:      return "LoadoutPublishDidNotTake";
        case LoadoutRefusal::ActorNotEnabled:        return "ActorNotEnabled";
        case LoadoutRefusal::NotAuthoritative:       return "NotAuthoritative";
        case LoadoutRefusal::AppliedPairsUnreadable: return "LoadoutAppliedPairsUnreadable";
        case LoadoutRefusal::AppliedPairsMismatch:   return "LoadoutAppliedPairsMismatch";
        case LoadoutRefusal::AlignmentRefused:       return "LoadoutAlignmentRefused";
        case LoadoutRefusal::AlignmentUnverified:    return "LoadoutAlignmentUnverified";
        case LoadoutRefusal::AlignmentUnrecorded:    return "LoadoutAlignmentUnrecorded";
        case LoadoutRefusal::AlignmentMismatch:      return "LoadoutAlignmentMismatch";
        case LoadoutRefusal::EngineCallReentrant:    return "EngineCallReentrant";
        case LoadoutRefusal::EngineUnbound:          return "LoadoutEngineUnbound";
        case LoadoutRefusal::RecordingNotAsked:      return "LoadoutRecordingNotAsked";
        case LoadoutRefusal::NotPublishedBeforeEnable:
            return "LoadoutNotPublishedBeforeEnable";
    }
    return "LoadoutRefusalUnknown";
}

const char* LoadoutSlotArmName(LoadoutSlotArm a) {
    switch (a) {
        case LoadoutSlotArm::NotCompared: return "notCompared";
        case LoadoutSlotArm::Matched:     return "matched";
        case LoadoutSlotArm::Pending:     return "pending";
        case LoadoutSlotArm::AlignGated:  return "alignGated";
        case LoadoutSlotArm::Mismatch:    return "mismatch";
    }
    return "armUnknown";
}

const char* LoadoutApplyByName(LoadoutApplyBy a) {
    switch (a) {
        case LoadoutApplyBy::Engine:   return "engine";
        case LoadoutApplyBy::Census:   return "census";
        case LoadoutApplyBy::Fallback: return "fallback";
    }
    return "applyByUnknown";
}

const char* LoadoutStepName(LoadoutStep s) {
    switch (s) {
        case LoadoutStep::Verify:  return "L1verify";
        case LoadoutStep::Attach:  return "L2attach";
        case LoadoutStep::Parse:   return "L3parse";
        case LoadoutStep::Publish: return "L4publish";
        case LoadoutStep::Apply:   return "L5apply";
        case LoadoutStep::Align:   return "L6align";
        case LoadoutStep::kCount:  break;
    }
    return "?";
}

const char* LoadoutOutcomeName(LoadoutOutcome o) {
    switch (o) {
        case LoadoutOutcome::NotReached:         return "NotReached";
        case LoadoutOutcome::Issued:             return "Issued";
        case LoadoutOutcome::SkippedAlreadyDone: return "SkippedAlreadyDone";
        case LoadoutOutcome::Refused:            return "Refused";
    }
    return "?";
}

uint16_t    LoadoutSlotNameCount() { return uint16_t(kSlotNameCount); }
const char* LoadoutSlotName(uint16_t i) { return i < kSlotNameCount ? kSlotNames[i] : nullptr; }

bool LoadoutTemplateInstance(char* out, size_t cap) { return TemplateSegment(2, out, cap); }

namespace {

// THE ACCEPTANCE, RUN AS A UNIT because it runs TWICE: once for whatever the entrant's enable
// left behind, and again after the fallback apply below.  Every tally it writes is cleared
// first, so the second pass reports the second read and not the sum of the two.
LoadoutRefusal ScoreAppliedPairs(LoadoutDrive& d) {
    void* const ng = d.ng;
    const UserIndex index = d.index;
    const RecordedLoadout& rec = *d.rec;
    const LoadoutEngine& eng = *d.eng;
    LoadoutSteps& out = *d.out;
    out.matched = out.mismatched = out.pending = out.alignGated = 0;
    out.engineHeld = 0;
    out.entityAlignment = kSlotAlignmentNone;
    out.entityAlignmentAsked = false;
    for (size_t i = 0; i < kSlotNameCount; ++i) {
        out.engine[i] = LoadoutPair{};
        out.engineState[i] = AppliedSlotState{};
        out.arm[i] = LoadoutSlotArm::NotCompared;
        out.engineAt[i] = kLoadoutNoEngineSlot;
    }
    uint64_t held = 0;
    Mark(d, "loadout L5 read applied pairs back");
    if (!eng.readAppliedPairs(ng, index.v, out.engine, held, out.engineState))
        return LoadoutRefusal::AppliedPairsUnreadable;
    out.engineHeld = held;
#ifdef W2P1_MUTATE_COMPARE_AUTHORED_SIDE
    for (size_t i = 0; i < kSlotNameCount; ++i) out.engine[i] = rec.pairs[i];
    out.engineHeld = held = kSlotNameCount;
#endif
    // SCORED BY THE ARM THE ENGINE TOOK, in the engine's own order (identity_access.h,
    // AppliedSlotState).  `item` alone answers for one of four outcomes, so a run that read it
    // alone reported three silent successes-of-a-kind as one undifferentiated `mismatched`.
    // THE GATE'S SECOND OPERAND, asked at most once and only if a slot needs it.  An entrant
    // whose twenty entries all carry 0xFFFF is dressed without ever reading it, so an image where
    // IDENT.CUSTOMISATION_ALIGNMENT did not bind still replays every unrestricted loadout.
    uint16_t entityAlign = kSlotAlignmentNone;
    bool alignAsked = false;
    bool alignUnrecorded = false;
    for (size_t i = 0; i < kSlotNameCount; ++i) {
        // Padding, and then the engine's own EMPTY pair: neither is a slot the player wore
        // anything in, L1 counted the second in `unfilled`, and scoring it here would look for
        // a -1 key among the engine's entries, find none and book a mismatch for a slot both
        // sides agree is bare.
#ifdef W2P1_MUTATE_L5_SCORES_UNFILLED
        // The defect: L5's own copy of the skip test, missing the half that keys on the ITEM.
        if (rec.pairs[i].slotKey == 0 || rec.pairs[i].slotKey == kUnfilledItem) continue;
#else
        if (RecordedPairNotCompared(rec.pairs[i])) continue;
#endif
        LoadoutSlotArm arm = LoadoutSlotArm::Mismatch;
        out.engineAt[i] = kLoadoutNoEngineSlot;
        for (uint64_t j = 0; j < held && j < kSlotNameCount; ++j) {
            if (out.engine[j].slotKey != rec.pairs[i].slotKey) continue;
            const AppliedSlotState& st = out.engineState[j];
#ifdef W2P1_MUTATE_COMPARE_ITEM_ONLY
            // The defect: the applied item is the only field consulted, so the pending latch and
            // the alignment gate both come back as a mismatch nobody can act on.
            arm = out.engine[j].itemId == rec.pairs[i].itemId ? LoadoutSlotArm::Matched
                                                              : LoadoutSlotArm::Mismatch;
            (void)st;
#else
            if (out.engine[j].itemId == rec.pairs[i].itemId)
                arm = LoadoutSlotArm::Matched;
            else if ((st.flags & kCUST_SLOT_APPLIED_BIT) != 0 &&
                     st.pendingItem == rec.pairs[i].itemId)
                arm = LoadoutSlotArm::Pending;
            else if (st.alignmentReq == kSlotAlignmentNone)
                arm = LoadoutSlotArm::Mismatch;
            else {
                // BOTH conjuncts of the engine's gate.  A requirement the
                // entrant MEETS did not skip anything: the apply ran and left something that is
                // not the recorded id, which is a substitution or a slot it never visited.
                if (!alignAsked) {
                    if (!eng.entityAlignment)
                        return LoadoutRefusal::NotBound;
                    if (!eng.entityAlignment(ng, index.v, entityAlign))
                        return LoadoutRefusal::AlignmentUnverified;
                    // The gate is about to decide slots off this value, so it has to be the
                    // entrant the recording describes and not whoever the engine now holds.
                    if (rec.alignment != kSlotAlignmentNone && rec.alignment != entityAlign)
                        return LoadoutRefusal::AlignmentMismatch;
                    out.entityAlignment = entityAlign;
                    out.entityAlignmentAsked = true;
                    alignAsked = true;
                }
#ifdef W2P1_MUTATE_ALIGN_SECOND_CONJUNCT_MISSING
                // The defect: a set requirement accepted as a skip without asking whether the
                // entrant meets it.
                arm = LoadoutSlotArm::AlignGated;
#else
                arm = st.alignmentReq != entityAlign ? LoadoutSlotArm::AlignGated
                                                     : LoadoutSlotArm::Mismatch;
#ifndef W2P1_MUTATE_ALIGN_SENTINEL_ACCEPTED
                // AUD-W2-P1-34 P1-39 F1.  A SKIP is only a faithful replay if the alignment the
                // engine decided it by is the one the capture recorded.  With no recorded
                // alignment the live value corroborates nothing, and the slot the entrant is
                // left wearing nothing in is exactly the visible difference this packet exists
                // to catch -- so the row scores as the mismatch it is and the pass is refused
                // by a word that says what the capture owes, not what the engine did.
                if (arm == LoadoutSlotArm::AlignGated && rec.alignment == kSlotAlignmentNone) {
                    arm = LoadoutSlotArm::Mismatch;
                    alignUnrecorded = true;
                }
#endif
#endif
            }
#endif
            out.engineAt[i] = uint8_t(j);
            break;
        }
        out.arm[i] = arm;
        switch (arm) {
            case LoadoutSlotArm::Matched:    ++out.matched; break;
            case LoadoutSlotArm::Pending:    ++out.pending; break;
            case LoadoutSlotArm::AlignGated: ++out.alignGated; break;
            default:                         ++out.mismatched; break;
        }
    }
    // MISMATCH IS THE ONLY REFUSAL. `pending` and `alignGated` are outcomes the engine chose and
    // named; hiding them inside `mismatched` is what made the 16 unexplainable. The second half
    // keeps every compared row accounted for, so an arm that stopped being scored cannot pass by
    // shrinking all four tallies at once.
    if (out.mismatched != 0 ||
        unsigned(out.matched) + out.pending + out.alignGated != out.compared)
        // The more specific word wins where it applies: a re-apply cannot corroborate an
        // alignment the capture never recorded, and the fallback below keys on the other one.
        return alignUnrecorded ? LoadoutRefusal::AlignmentUnrecorded
                               : LoadoutRefusal::AppliedPairsMismatch;
    return LoadoutRefusal::None;
}

}  // namespace

LoadoutRefusal DressLoadout(void* ng, UserSlot slot, UserIndex index, const RecordedLoadout& rec,
                            const LoadoutEngine& eng, LoadoutSteps& out,
                            LoadoutPhase phase) {
    out = LoadoutSteps{};
    LoadoutDrive d{ng, slot, index, &rec, &eng, &out, phase};

    // The hop chain's own latch: an engine callback that re-entered us here is refused by name
    // rather than starting a second transition inside the first.
    PawnProvision::EngineCallLatch latch;
    if (!latch.taken) return Step(out, LoadoutStep::Verify, LoadoutRefusal::EngineCallReentrant);

    // L1 -- VERIFY.  It reads the document and authors nothing; nothing is called on the engine's
    // state until it has passed.
    const LoadoutRefusal v = VerifyDocument(d);
    if (Step(out, LoadoutStep::Verify, v) != LoadoutRefusal::None) return v;

    if (ng == nullptr) return Step(out, LoadoutStep::Attach, LoadoutRefusal::ContainerUnreadable);
    // THE APPLY HALF ENTERS HERE, past L1 and past nothing else.  L2..L4 ran in an earlier
    // call and the entrant's enable has fired since; what settles whether they landed is the
    // LIVE table read back NOW, not their outcomes remembered from before the enable.
    if (phase == LoadoutPhase::Apply) {
        out.instances = LiveLoadoutInstances(ng, slot);
        if (out.instances == kLoadoutInstancesUnreadable)
            return Step(out, LoadoutStep::Publish, LoadoutRefusal::ContainerUnreadable);
        if (out.instances == 0)
            return Step(out, LoadoutStep::Publish, LoadoutRefusal::NotPublishedBeforeEnable);
        // Three steps this call did not drive, marked as the earlier call's work rather than
        // left NotReached: a six-outcome array with holes in it reads as a truncated pass.
        out.outcome[size_t(LoadoutStep::Attach)]  = LoadoutOutcome::SkippedAlreadyDone;
        out.outcome[size_t(LoadoutStep::Parse)]   = LoadoutOutcome::SkippedAlreadyDone;
        out.outcome[size_t(LoadoutStep::Publish)] = LoadoutOutcome::SkippedAlreadyDone;
    } else {
        void* doc = eng.doc->json;

        // L2 -- ATTACH, and its read-back is the engine's own two answers, not the write's.  SKIPPED
        // on the Source phase: it attaches the document to the entrant's user record by INDEX, and the
        // Source phase runs before admission when there is no entrant to attach to.  L3 below does not
        // depend on it -- it parses the document object directly into the SOURCE container.
        if (phase != LoadoutPhase::Source) {
            if (!eng.attach || !eng.attachedDoc || !eng.docUsable)
                return Step(out, LoadoutStep::Attach, LoadoutRefusal::NotBound);
            Mark(d, "loadout L2 attach document");
            (void)eng.attach(ng, index.v, doc);
            Mark(d, "loadout L2 read back attached document");
            if (eng.attachedDoc(ng, index.v) != doc)
                return Step(out, LoadoutStep::Attach, LoadoutRefusal::DocumentNotAttached);
            Mark(d, "loadout L2 ask JSON_IS_EMPTY");
            if (!eng.docUsable(ng, index.v))
                return Step(out, LoadoutStep::Attach, LoadoutRefusal::DocumentNotUsable);
            Step(out, LoadoutStep::Attach, LoadoutRefusal::None);
        }

        // L3 -- PARSE, the step that was missing entirely: nothing in this tree ever filled the
        // per-user container FROM our document, so the publish copied whatever the bot generator left.
        if (!eng.parse) return Step(out, LoadoutStep::Parse, LoadoutRefusal::NotBound);
        void* container = LoadoutContainer(ng, slot);
        if (!container) return Step(out, LoadoutStep::Parse, LoadoutRefusal::ContainerUnreadable);
        Mark(d, "loadout L3 parse document into container");
        // eng.parse force-zeroes number (+0x38) but INHERITS numberBody (+0x3A) from the container, and
        // the identity-block document carries neither key.  On the bot body-source path GenerateBotProfile
        // already parsed the bot's own profile into this same container, leaving the BOT's numberBody in
        // +0x3A; the keyless document never overwrites it, so L3 below refuses on src->numberBody !=
        // rec.numberBody before it ever reports the instance count.  Seed the recorded numberBody before
        // the parse -- nothing zeroes +0x3A afterward, so it survives and the gate reads back the match.
        // Source phase only; a no-op on the remote Source phase, where the value already matched.
        if (phase == LoadoutPhase::Source) {
            LoadoutHeader* pre = static_cast<LoadoutHeader*>(container);
            if (Readable(pre, sizeof(LoadoutHeader))) {
                const uint16_t beforeNumberBody = pre->numberBody;
                pre->numberBody = rec.numberBody;
                log::Direct(LogLevel::Info, "identity",
                            "loadout Source numberBody seed: container +0x3A %u -> %u (rec.numberBody %u)",
                            unsigned(beforeNumberBody), unsigned(pre->numberBody),
                            unsigned(rec.numberBody));
            }
        }
        eng.parse(container, doc);
        const LoadoutHeader* src = static_cast<const LoadoutHeader*>(container);
        if (!Readable(src, sizeof(LoadoutHeader)))
            return Step(out, LoadoutStep::Parse, LoadoutRefusal::ContainerUnreadable);
        if (src->number != rec.number || src->numberBody != rec.numberBody || src->instanceCount == 0)
            return Step(out, LoadoutStep::Parse, LoadoutRefusal::ParseDidNotFill);
        Step(out, LoadoutStep::Parse, LoadoutRefusal::None);

        // THE SOURCE PHASE ENDS HERE.  The SOURCE now holds the recorded
        // instances; the engine's own add-entrant handler bridges it into the live table when the
        // reply is dispatched.  Report the count the bridge will copy -- it is the launch signal the
        // experiment turns on -- and drive no L4 publish or L5 apply of our own.
        if (phase == LoadoutPhase::Source) {
            out.instances = src->instanceCount;
            return LoadoutRefusal::None;
        }

        // L4 -- PUBLISH.  The flag at IDENT.LOADOUT_PUBLISHED_FLAG_OFF is READ, never written: it is
        // the engine's own bookkeeping, and setting it would tell the receive path this user's
        // loadout is already published and suppress a real one.  A 1 means a path we did not model
        // ran, so this refuses rather than publishing into a state the engine considers published.
        if (!eng.publish) return Step(out, LoadoutStep::Publish, LoadoutRefusal::NotBound);
        const uint32_t* flag = reinterpret_cast<const uint32_t*>(
            At(ng, uint64_t(facts::USER_PROFILE_STRIDE) * uint64_t(slot.v) +
                       uint64_t(facts::IDENT_LOADOUT_PUBLISHED_FLAG_OFF)));
        if (!flag || !Readable(flag, sizeof(uint32_t)))
            return Step(out, LoadoutStep::Publish, LoadoutRefusal::ContainerUnreadable);
        if (*flag != 0) return Step(out, LoadoutStep::Publish, LoadoutRefusal::PublishFlagUnexpected);
        LoadoutHeader* dst = LiveLoadout(ng, slot);
        if (!dst || !Readable(dst, sizeof(LoadoutHeader)))
            return Step(out, LoadoutStep::Publish, LoadoutRefusal::ContainerUnreadable);
        Mark(d, "loadout L4 publish container into the live table");
        (void)eng.publish(dst, container);
        // NAME-ALIGN (DIAG-SEAT-DRIVE core fix): the engine's own enable-time apply selects the
        // instance named *IDENT.LOADOUT_DEFAULT_INSTANCE_PTR (SymbolFromName("unified")); the parse
        // above named ours after the recording's own `loadout|instances|<NAME>` child.  Alias the
        // populated instance onto the engine-selected name in the live table so the engine's OWN
        // apply finds it non-empty instead of printing "Unknown loadout instance name" and applying
        // nothing.  No-op when the parse already produced "unified" or when the name did not resolve.
        (void)AlignPublishedInstanceName(dst, eng.instanceSymbol());
        // The same qword the engine's own entrant dump prints as `Loadouts : N`, read where it reads
        // it -- never a count of our own array.
#ifdef W2P1_MUTATE_COUNT_FROM_OUR_ARRAY
        out.instances = out.compared;
#else
        out.instances = dst->instanceCount;
#endif
        if (out.instances != src->instanceCount || out.instances == 0)
            return Step(out, LoadoutStep::Publish, LoadoutRefusal::PublishDidNotTake);
        // The apply iterates the PARSED INSTANCE's item count, not our twenty
        // recorded rows, so "the parse produced K items" and "the apply skipped them" are the same
        // census line without this number.  Instance 0 is the one the publish just installed; a
        // vector that does not read back leaves the sentinel rather than a confident zero.
        out.items = kLoadoutItemsUnknown;
        if (const LoadoutInstance* inst = static_cast<const LoadoutInstance*>(dst->instances))
            if (Readable(inst, sizeof(LoadoutInstance)))
                out.items = inst->itemCount > kLoadoutFormatMax ? kLoadoutFormatMax
                                                                : uint32_t(inst->itemCount);
        Step(out, LoadoutStep::Publish, LoadoutRefusal::None);

        // THE FOUR GATES, read here and nowhere else: this is the last instruction before the
        // caller issues the entrant's enable, and the enable is a one-shot event -- the engine
        // drives its apply off it once and nothing re-runs it.  Read after the publish, so the
        // instance the engine will match is the one the publish just installed.
        if (eng.applyGates != nullptr)
            (void)eng.applyGates(ng, slot.v, index.v, out.applyGates);

        // THE PUBLISH HALF ENDS HERE.  Its caller now issues the entrant's enable, and the
        // engine drives its own apply off that enable against the instance just published.
        if (phase == LoadoutPhase::Publish) return LoadoutRefusal::None;
    }

    // L5 -- APPLY, gated on hop 2's OWN re-read.  The apply resolves in the actor/space domain,
    // so an apply issued before the participating arm enabled and registered the actor is exactly
    // the failure the packet sequencing could never prevent.
    if (!eng.actorEnabled || !eng.applyItems || !eng.readAppliedPairs)
        return Step(out, LoadoutStep::Apply, LoadoutRefusal::NotBound);
    Mark(d, "loadout L5 re-read actor enabled");
    if (!eng.actorEnabled(eng.actorCtx, index.v))
        return Step(out, LoadoutStep::Apply, LoadoutRefusal::ActorNotEnabled);
#ifndef W2P1_MUTATE_IGNORE_COSMETIC_GATE
    if (!CosmeticsWouldApply(ng))
        return Step(out, LoadoutStep::Apply, LoadoutRefusal::NotAuthoritative);
#endif

#ifdef W2P1_MUTATE_ALIGN_BEFORE_APPLY
    if (eng.applyAlignment) {
        Mark(d, "loadout L6 apply alignment");
        (void)eng.applyAlignment(ng, index.v);
    }
#endif
    // OUR APPLY, and on the ordered path it is the FALLBACK and not the mechanism: the
    // container went into the live table before the entrant's enable and the engine drives its
    // own apply off that enable.  The `All` phase has no enable to be second to -- its caller
    // issues none -- so there we apply up front.
    bool appliedByUs = false;
    if (phase == LoadoutPhase::All) {
        Mark(d, "loadout L5 apply loadout items");
        eng.applyItems(ng, index.v);
        appliedByUs = true;
        out.appliedBy = LoadoutApplyBy::Census;
    } else {
        Mark(d, "loadout L5 engine applied off the enable");
    }

    // THE ACCEPTANCE names its two sources: the RECORDED pairs from the demo's own bytes, and
    // the ENGINE's post-apply truth read back off the customisation component.  Never the
    // authored side against itself -- that is green when nothing reached the engine.
    LoadoutRefusal accepted = ScoreAppliedPairs(d);

    // THE LIVE FALLBACK (AUD-W2-P1-34 P1-38 F1).  The enable fired and the engine's apply took
    // one of its two silent arms -- an empty item array, or a deferred-method push that never
    // ran -- so the read-back holds nothing the recording asked for.  Before this, that state
    // latched `loadoutAttempted` and the entrant wore nothing for the match.  ONCE: a pass that
    // already drove the apply itself does not drive it again, and the dress runs once per load,
    // so the engine cannot be re-entered on this path however the second read scores.
#ifdef W2P1_MUTATE_L5_FALLBACK_UNREACHABLE
    // The defect: the fallback is written but its condition is the census phase, which issues
    // no enable, so on the live path there is no state in which it fires.
    if (accepted == LoadoutRefusal::AppliedPairsMismatch && !appliedByUs &&
        phase == LoadoutPhase::All) {
#elif defined(W2P1_MUTATE_L5_FALLBACK_MISMATCH_ONLY)
    // The defect: the fallback keys on the mismatch word alone, so the shape today's captures
    // actually produce -- a recording that read no alignment -- never reaches the apply.
    if (accepted == LoadoutRefusal::AppliedPairsMismatch && !appliedByUs) {
#else
    // AUD-W2-P1-34 P1-40 M1.  BOTH WORDS REACH THE APPLY.  `AlignmentUnrecorded' is what
    // ScoreAppliedPairs returns for a capture that recorded no alignment and read back slots the
    // recording asked for and did not get -- which is the same engine state the mismatch word
    // names, arrived at from the commoner capture shape.  Keyed on the mismatch alone, every
    // such entrant wore nothing and the fallback that exists for it never ran.
    if ((accepted == LoadoutRefusal::AppliedPairsMismatch ||
         accepted == LoadoutRefusal::AlignmentUnrecorded) && !appliedByUs) {
#endif
        Mark(d, "loadout L5 fallback apply after the enable");
        eng.applyItems(ng, index.v);
        appliedByUs = true;
        out.appliedBy = LoadoutApplyBy::Fallback;
        accepted = ScoreAppliedPairs(d);
    }
    if (accepted != LoadoutRefusal::None) return Step(out, LoadoutStep::Apply, accepted);
    Step(out, LoadoutStep::Apply, LoadoutRefusal::None);

    // L6 -- the chassis/skin/colour apply rides the alignment path and walks the SAME
    // customisation slots, so it runs after the twenty are filled or it authors from nothing.
    if (!eng.applyAlignment) return Step(out, LoadoutStep::Align, LoadoutRefusal::NotBound);
    Mark(d, "loadout L6 apply alignment");
    if (!eng.applyAlignment(ng, index.v))
        return Step(out, LoadoutStep::Align, LoadoutRefusal::AlignmentRefused);
    return Step(out, LoadoutStep::Align, LoadoutRefusal::None);
}


// =============================================================================================
// THE SHIPPED LOADOUT SURFACE.  Every address comes from the registry through the same
// prologue-checked Guarded<> the rest of this module binds with, so a rebase, a patched image or
// a re-derivation that disagrees leaves the member NULL and its step refuses by name -- rather
// than calling an address this process may not have.
// =============================================================================================

namespace {

// IDENT.SYMBOL_FROM_NAME_VA(str, seed, withCallback, maxChars, cbArg).
using FnSymbolFromName = uint64_t (*)(const char*, uint64_t, int32_t, int32_t, void*);
// IDENT.LOADOUT_FROM_PROFILE_VA(container, doc) -- L3, the step that was missing entirely.
using FnLoadoutFromProfile = void (*)(void*, void*);
// IDENT.APPLY_LOADOUT_ITEMS.ENTRY_PROFILE_ARM(netGame, entrantIndex) -- L5.
using FnApplyLoadoutItems = void (*)(void*, uint16_t);

// The qword IDENT.APPLY_LOADOUT_ITEMS.ENTRY_PROFILE_ARM loads as its instance-name argument, read
// in vivo through the same rebase every other address here goes through.  0 means "did not
// resolve", which L1 turns into LoadoutInstanceNameUnknown -- never a guess at "unified".
uint64_t LoadoutDefaultInstanceSymbol() {
    const void* at = buildguard::MappedAddress(facts::IDENT_LOADOUT_DEFAULT_INSTANCE_PTR);
    if (at == nullptr || !Readable(at, sizeof(uint64_t))) return 0;
    return *static_cast<const uint64_t*>(at);
}

// L2's two halves and its read-back, over the reads this module already owns.  The attach returns
// the document it stored so the caller's read-back compares two pointers and not two booleans.
void* LoadoutAttach(void* ng, uint16_t index, void* doc) {
    if (doc == nullptr) return nullptr;
    if (StoreProfileDocument(ng, index, doc) != AttachResult::Ok) return nullptr;
    return doc;
}
void* LoadoutAttachedDoc(void* ng, uint16_t index) { return ProfileJson(ng, UserIndex(index)); }
bool  LoadoutDocUsable(void* ng, uint16_t index) {
    return ProfileJsonUsable(ng, UserIndex(index));
}

bool LoadoutReadAppliedPairs(void* ng, uint16_t index, LoadoutPair out[20], uint64_t& n,
                             AppliedSlotState state[20]) {
    return ReadAppliedPairs(ng, UserIndex(index), out, n, nullptr, nullptr, state);
}

bool LoadoutEntityAlignment(void* ng, uint16_t index, uint16_t& out) {
    return EntityAlignment(ng, UserIndex(index), out, nullptr);
}

// L6.  ApplyAlignmentChange re-runs the chassis/skin/colour apply UNCONDITIONALLY off the same
// customisation slots L5 has just filled; OverrideAlignment is deliberately not issued here
// because this step changes no alignment -- it only re-runs the one already stored, and storing
// the value it already holds is the no-op arm this module documents at its head.
// The gate reader over the instance name THIS image carries, so the sequence asks the engine for
// the name it will match on instead of the caller passing one in.
LoadoutApplyGate LoadoutApplyGatesFor(void* ng, uint16_t slot, uint16_t index,
                                      LoadoutApplyGates& out) {
    return ReadLoadoutApplyGates(ng, UserSlot(slot), UserIndex(index),
                                 LoadoutDefaultInstanceSymbol(), out);
}

bool LoadoutApplyAlignment(void* ng, uint16_t index) {
    ActorComponentRef ref;
    if (!ResolveComponent(ng, UserIndex(index), ref)) return false;
    return CallApplyAlignmentChange(ref.componentSystem, ref.handle);
}

}  // namespace

// The same qword BindLoadoutEngine hands the sequence, published so a caller that reads the
// gates outside DressLoadout asks the engine for the instance name instead of guessing one.
uint64_t LoadoutInstanceName() { return LoadoutDefaultInstanceSymbol(); }

bool BindLoadoutEngine(LoadoutEngine& out, const PawnProvision::ActorHops& hops) {
    out.hash = Guarded<FnSymbolFromName>(facts::IDENT_SYMBOL_FROM_NAME_VA,
                                         facts::IDENT_SYMBOL_FROM_NAME_VA_PROLOGUE, 0);
    out.instanceSymbol = &LoadoutDefaultInstanceSymbol;
    // NEVER BOUND.  L1 verifies and authors nothing; the slot exists only so that promise can be
    // falsified by a test that counts writer calls.
    out.setString = nullptr;

    out.attach      = &LoadoutAttach;
    out.attachedDoc = &LoadoutAttachedDoc;
    out.docUsable   = &LoadoutDocUsable;
    out.parse       = Guarded<FnLoadoutFromProfile>(
        facts::IDENT_LOADOUT_FROM_PROFILE_VA, facts::IDENT_LOADOUT_FROM_PROFILE_VA_PROLOGUE, 0);
    out.publish     = Guarded<FnLoadoutAssign>(facts::IDENT_LOADOUT_ASSIGN,
                                               facts::IDENT_LOADOUT_ASSIGN_PROLOGUE, 0);
    out.applyItems  = Guarded<FnApplyLoadoutItems>(
        facts::IDENT_APPLY_LOADOUT_ITEMS_ENTRY_PROFILE_ARM,
        facts::IDENT_APPLY_LOADOUT_ITEMS_ENTRY_PROFILE_ARM_PROLOGUE, 0);
    out.applyAlignment   = &LoadoutApplyAlignment;
    // L7 (DIAG-MODEL-INSTANTIATE, Lever A) re-fires the engine's OWN productive apply -- the SAME
    // LoadoutApplyAlignment (sub_140CAA5F0 -> APPLY_CURRENT_LOADOUT sub_140CAB3B0), whose worn-record
    // loop calls IDENT.LOAD_AND_APPLY (sub_140D0D170) + Apply Chassis per entry and, over a non-empty
    // name-matched loadout instance, loads the asset, installs the InstanceModel CI and swaps.  The
    // prior re-point drove IDENT.APPLY_CHASSIS DIRECTLY per pair (ENG-W2-P1-51/53); that was wrong --
    // Apply Chassis only SKINS an existing model-CI and its readiness gate refuses silently on an
    // entrant whose InstanceModel is absent, so RUN-43 saw 0 engine Apply Chassis and no body.  The
    // caller gates this on the engine-selected instance's +56 being non-empty, holding/re-firing.
    out.driveModel       = &LoadoutApplyAlignment;
    out.applyGates       = &LoadoutApplyGatesFor;
    out.readAppliedPairs = &LoadoutReadAppliedPairs;
    // BOUND OR ABSENT, DECIDED HERE.  The trampoline is our own function and can always be
    // called, so leaving it installed over an unresolved IDENT.CUSTOMISATION_ALIGNMENT turns
    // `the image did not carry the getter' into `the component would not answer' -- L5 booked
    // AlignmentUnverified for a build that simply has no getter to ask (AUD-W2-P1-34 P1-39 F2).
    out.entityAlignment  = Entry<FnAlignment>(Site::CustomisationAlignment)
                               ? &LoadoutEntityAlignment
                               : nullptr;

    // OUTFITS: the engine's own per-slot document writer.  NOT part of the answer below -- a build
    // that cannot resolve it still dresses every entrant whose capture carries a real document, and
    // the authoring refuses by name rather than taking the whole ladder down with it.
    out.setSlotFromId = Entry<FnLoadoutDocSetSlot>(Site::LoadoutDocSetSlot);

    // HOP 2'S OWN READ, pointer and ctx, not a second binding of it.
    out.actorCtx     = hops.ctx;
    out.actorEnabled = hops.enabled;

    // The four ENGINE calls plus the hasher and hop 2's gate.  The trampolines above cannot fail
    // to bind -- they are our own functions -- so they are not part of this answer.
    return out.hash != nullptr && out.parse != nullptr && out.publish != nullptr &&
           out.applyItems != nullptr && out.actorEnabled != nullptr &&
           LoadoutDefaultInstanceSymbol() != 0;
}

namespace {

// The engine's JSON reader refuses a document whose strings are not UTF-8, and a display name the
// recorder caught mid-encoding must not cost the entrant its outfit: such a name is left out and
// said so, never transliterated into something the player is not called.
bool Utf8Valid(const char* s, size_t n) {
    for (size_t i = 0; i < n;) {
        const unsigned char c = (unsigned char)s[i];
        size_t follow = 0;
        if (c < 0x80) follow = 0;
        else if ((c & 0xE0) == 0xC0 && c >= 0xC2) follow = 1;
        else if ((c & 0xF0) == 0xE0) follow = 2;
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4) follow = 3;
        else return false;
        if (i + follow >= n && follow != 0) return false;   // a sequence cut off by the end
        for (size_t k = 1; k <= follow; ++k)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        i += follow + 1;
    }
    return true;
}

bool Put(char* out, size_t cap, size_t& at, const char* s) {
    const size_t n = ::strlen(s);
    if (at + n + 1 > cap) return false;
    ::memcpy(out + at, s, n);
    at += n;
    out[at] = '\0';
    return true;
}

// RFC 8259 minimal escaping: the two mandatory glyphs and every control byte.  Bytes above 0x7F
// are copied verbatim -- the caller has already established they form valid UTF-8, which JSON
// carries as itself.
bool PutEscaped(char* out, size_t cap, size_t& at, const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char)s[i];
        char esc[8];
        if (c == '"')       { esc[0] = '\\'; esc[1] = '"';  esc[2] = 0; }
        else if (c == '\\') { esc[0] = '\\'; esc[1] = '\\'; esc[2] = 0; }
        else if (c == '\n') { esc[0] = '\\'; esc[1] = 'n';  esc[2] = 0; }
        else if (c == '\r') { esc[0] = '\\'; esc[1] = 'r';  esc[2] = 0; }
        else if (c == '\t') { esc[0] = '\\'; esc[1] = 't';  esc[2] = 0; }
        else if (c < 0x20)  { _snprintf_s(esc, sizeof esc, _TRUNCATE, "\\u%04x", unsigned(c)); }
        else                { esc[0] = (char)c; esc[1] = 0; }
#ifdef OUTFITS_MUTATE_NO_JSON_ESCAPING
        esc[0] = (char)c; esc[1] = 0;   // the name goes in raw, quotes and all
#endif
        if (!Put(out, cap, at, esc)) return false;
    }
    return true;
}

}  // namespace

size_t AuthorProfileDocument(const char* displayName, uint16_t number, uint16_t numberBody,
                             char* out, size_t cap, AuthoredLoadout& st) {
    if (out == nullptr || cap == 0) return 0;
    out[0] = '\0';
    // The instance name comes from the engine's own path template, never from a literal of ours.
    char inst[64] = {0};
    if (!LoadoutTemplateInstance(inst, sizeof inst)) return 0;

    const size_t nameLen = displayName != nullptr ? ::strnlen(displayName, 128) : 0;
    st.nameOmitted = nameLen == 0 || !Utf8Valid(displayName, nameLen);

    size_t at = 0;
    if (!Put(out, cap, at, "{")) { out[0] = '\0'; return 0; }
    if (!st.nameOmitted) {
        if (!Put(out, cap, at, "\"displayname\":\"") ||
            !PutEscaped(out, cap, at, displayName, nameLen) || !Put(out, cap, at, "\",")) {
            out[0] = '\0';
            return 0;
        }
    }
    char nums[128];
    _snprintf_s(nums, sizeof nums, _TRUNCATE,
                "\"loadout\":{\"number\":%u,\"number_body\":%u,\"instances\":{\"",
                unsigned(number), unsigned(numberBody));
    // A document that does not fit is NO document: a truncated one would parse to an outfit the
    // player never wore.
    if (!Put(out, cap, at, nums) || !PutEscaped(out, cap, at, inst, ::strlen(inst)) ||
        !Put(out, cap, at, "\":{\"slots\":{}}}}}")) {
        out[0] = '\0';
        return 0;
    }
    return at;
}

bool FillAuthoredLoadoutSlots(const entity_json::CJsonReader& doc, const LoadoutEngine& eng,
                              const uint64_t* slotKeys, const uint64_t* itemIds, uint16_t n,
                              AuthoredLoadout& st) {
    if (doc.json == nullptr || eng.setSlotFromId == nullptr || eng.hash == nullptr ||
        slotKeys == nullptr || itemIds == nullptr)
        return false;

    uint64_t nameHash[kSlotNameCount];
    for (size_t i = 0; i < kSlotNameCount; ++i)
        nameHash[i] = eng.hash(kSlotNames[i], kHashSeed, kHashWithCallback, kHashUnbounded, nullptr);

    // The engine writer's own path minus its "|%s" leaf, so a build that writes somewhere else
    // reads back zero here instead of being agreed with.
    char parent[160] = {0};
    {
        const size_t fmtLen = ::strnlen(kPathFmt, sizeof parent);
        size_t cut = 0;
        for (size_t i = 0; i < fmtLen; ++i)
            if (kPathFmt[i] == '|') cut = i;
        if (cut == 0 || cut >= sizeof parent) return false;
        ::memcpy(parent, kPathFmt, cut);
        parent[cut] = '\0';
    }

    const bool canCount = doc.pathCount != nullptr;
    int64_t held = canCount ? doc.pathCount(doc.json, parent) : 0;
    if (held < 0) held = 0;

    // holder[0] is the only field the writer reads for an id that is not -1; the two behind it
    // steer the random default it would invent for an unworn slot, which is never asked for here.
    void* holder[3] = {doc.json, nullptr, nullptr};
    const uint16_t take = n < kSlotNameCount ? n : uint16_t(kSlotNameCount);
    for (uint16_t i = 0; i < take; ++i) {
        const LoadoutPair p{slotKeys[i], itemIds[i]};
#ifndef OUTFITS_MUTATE_WRITE_THE_UNWORN_SLOTS
        if (RecordedPairNotCompared(p)) { ++st.unfilled; continue; }  // padding, or wears nothing
#endif
        size_t slot = kSlotNameCount;
        for (size_t s = 0; s < kSlotNameCount; ++s)
            if (nameHash[s] == slotKeys[i]) { slot = s; break; }
        if (slot == kSlotNameCount) {
            if (st.unresolvedSlot == 0) st.firstUnresolvedSlotKey = slotKeys[i];
            ++st.unresolvedSlot;
            continue;
        }
        uint64_t id = itemIds[i];
        eng.setSlotFromId(holder, &id, kSlotNames[slot]);
        ++st.written;
        if (!canCount) continue;
        // The writer says nothing when the engine's registry cannot name the id; the document's
        // own child count is what tells an applied slot from a silently dropped one.
        const int64_t now = doc.pathCount(doc.json, parent);
        if (now > held) { held = now; continue; }
        if (st.unresolvedItem == 0) st.firstUnresolvedItemId = itemIds[i];
        ++st.unresolvedItem;
    }
    if (canCount) st.inDocument = uint16_t(held);
    return true;
}

bool BindLoadoutDocument(entity_json::CJsonReader& out, const Blob& recordedJson) {
    out = entity_json::CJsonReader{};
    if (!RecordedProfileJsonAcceptable(recordedJson)) return false;
    const FnJsonDocCtor   ctor  = Entry<FnJsonDocCtor>(Site::JsonDocCtor);
    const FnJsonParseText parse = Entry<FnJsonParseText>(Site::JsonParseText);
    if (ctor == nullptr || parse == nullptr) return false;

    void* doc = ::calloc(1, size_t(kJSON_DOC_SIZE));
    if (doc == nullptr) return false;
    ctor(doc);
    if (parse(doc, reinterpret_cast<const char*>(recordedJson.p), uint64_t(recordedJson.n)) != 0) {
        ::free(doc);   // untouched by the engine on its error arm: it reports and returns
        return false;
    }
    // The engine's own write gate on what L2 is about to attach.  A document that does not pass it
    // would reproduce L3 #19 with a new pointer instead of the json db cache's.
    if (!ProfileDocWritable(doc) || !BuildCJsonReader(doc, out)) {
        ::free(doc);
        out = entity_json::CJsonReader{};
        return false;
    }
    return true;
}

void ReleaseLoadoutDocument(entity_json::CJsonReader& doc) {
    // Only ever the document THIS module allocated, and only on the path where L2 never attached
    // it: once the attach has taken, the entrant's user record owns it and freeing it here would
    // leave the engine dereferencing our free list.
    if (doc.json != nullptr) ::free(doc.json);
    doc = entity_json::CJsonReader{};
}

}  // namespace replay::identity_access
