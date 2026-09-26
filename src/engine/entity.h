#pragma once

// B.4 IEntity and IPosePublisher -- typed access to the two component pools, and the pose
// publisher HP1 needs.  Owned by W1-E; consumed by W2-A (capture) and W2-B (playback).
//
// Everything here is in `namespace replay`, which is where bootstrap/services.h forward-declares
// IEntity and IPosePublisher.  While these lived at global scope they were DIFFERENT types with the
// same spelling, so ReplayServices::entity named a class that was declared and never defined and
// W2-A could not gather S_BODY_SAMPLED or S_ASSEEN through it at all.  The rule is the whole
// appendix, not the `I`-prefixed half of it: UserIndex, PhysHandle and the two state structs are
// B.4's vocabulary and move with the interfaces.
//
// The compatibility using-declarations at the foot of this file keep the unqualified spelling valid
// for translation units that are still at global scope.  They introduce no second type and no
// symbol: `::IEntity` and `replay::IEntity` are one class.
//
// ---------------------------------------------------------------------------------------
// WHAT THIS INTERFACE ENCODES, and why it is shaped this way (docs/spikes/W1-E.md carries the
// derivation; every number reaches the .cpp from facts_generated.h by id, never transcribed):
//
//   HP1.  CR15NetPhysicsCS::IsLocallyControlled returns false for ANY owner whose low word is
//         >= 0x10, decided before it touches memory.  So SetSyntheticOwner is a single dword
//         store and CANNOT be anything else: the engine's own SetOwner broadcasts an ownership
//         transfer, frees the history ring we are about to fill, and rewrites both nibbles of
//         the CI flags.  There is deliberately no "SetOwner" entry point on this interface.
//
//   POSE. The offline session is unavoidably the net authority, so the engine's player
//         interpolator never runs; instead UpdateInterpolatedPlayerStates SYNTHESISES both pose
//         slots from the raw block at PlayerCI+60 every frame.  Publish() therefore writes that
//         block -- PP-B -- from our stage-0 tick, and the engine's own update DAG fills
//         PlayerCI+168 and PlayerCI+320 later in the same frame.  No late tick.
//
//   B2 / S2 F11.  PhysHandle and PlayerHandle do not convert.  +28 and +320 mean different
//         things in the two pools, and so do the history pairs (+344/+352 against +664/+672,
//         in the OPPOSITE order), so a converting handle would let PushHistory write a count
//         through a list pointer with nothing in the engine complaining.
// ---------------------------------------------------------------------------------------

#include <stdint.h>
#include <stddef.h>

namespace replay {

struct UserIndex   { uint16_t v; explicit UserIndex(uint16_t x) : v(x) {} };
struct UserSlot    { uint16_t v; explicit UserSlot (uint16_t x) : v(x) {} };

// `pool` names WHICH CR15NetPhysicsCS the handle indexes. The disc's is resolved out of the LEVEL
// game space (CR15FrisbeeCS::Initialize takes it from frisbeeCs+0x80) while every player body comes
// from the persistent global MP space, so the two are different instances with different pool bases
// and different handle namespaces. Indexing one with the other's handle reads a live player's body.
struct PhysHandle {
    static constexpr uint8_t kPoolPlayers = 0;
    static constexpr uint8_t kPoolDisc    = 1;
    uint16_t v;
    uint8_t  pool;
    explicit PhysHandle(uint16_t x, uint8_t p = kPoolPlayers) : v(x), pool(p) {}
};
struct PlayerHandle { uint16_t v; explicit PlayerHandle(uint16_t x) : v(x) {} };

// The RECORDED holder's capture user slot, carried beside the pose because the disc's possession is
// a level the engine derives its grab/release edges from (RE-GRAB-POSSESSION S1.1). kBodyOwnerNone
// is what the file writes when nobody holds the body, and it is also what every ENGINE-sourced read
// answers: an engine body has a live owner dword, not a recorded slot, and 0 is a real slot.
inline constexpr uint32_t kBodyOwnerNone = 0xFFFFFFFFu;  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop

struct BodyState   { float com[3], ori[4], vel[3], angVel[3], lastHeldCom[3]; uint32_t bits11;
                     uint32_t owner_slot; };

// bits11 is the format's field, not the engine's word.  SR15NetPhysicsSimState carries `bits` in
// ELEVEN bits; the CI flags word at PHYS.CI_FLAGS_OFF is a full dword and holds
// local-only state above that width -- PHYS.CI_INTERP_ENABLE_BIT (bit 11) and PHYS.CI_PAUSED_BIT
// (bit 13).  Handing the raw word to the encoder is a raw value that does not fit its declared
// width, and bitio refuses the whole record for it: L2 #15 F38 recorded 7080 refusals and 0 stored
// S_BODY_SAMPLED rows against a reader that answered every tick.  Every body reader converts here
// and nowhere else, so the value a BodyState carries fits the wire by construction.
uint32_t BodyWireBits(uint32_t ciFlags);

// WHICH source answered for a body. The two are not two opinions of one quantity: a net-driven
// body has history and a locally simulated one has none, so at most one arm can answer for any
// given body -- and before this existed the second arm did not exist either, which is why a solo
// capture recorded no body at all.
//
// NotAsked matters for the same reason it does in LocalRefusal: an IEntity written before
// ReadBodyDetailed must not answer None, which is a claim about the engine rather than about
// itself.
enum class BodySource : uint8_t {
    NotAsked = 0,
    None,       // neither source produced a body
    History,    // the newest node on PHYS.CI_HIST_LIST_OFF
    Local,      // the live state this machine integrates.  TWO chains can answer it and
                // BodyDiag::origin says which: the CPhBody the physics CS resolves, or -- when
                // that entity has no ncaPhysics instance -- the ncaTransform the scene graph
                // carries.  It stays ONE enumerator because src/capture's source histogram is a
                // switch with no default arm: a new enumerator there would be counted by nothing
                // and a transform-sourced tick would read as a body that was never sampled.
};

// WHICH gate refused. `None` means nothing refused -- a read that answered, or one nobody made
// -- so a histogram of these is all zero on a healthy capture and names its own cause on a
// broken one. There is one enumerator per step of the local arm because the local arm now
// REPRODUCES PHYS.PRIMARY_BODY_PC and PHYS.RESOLVE_BODY_PC step for step: L2 #9 saw
// `refused 1` every tick with five candidate causes and no way to tell them apart, and a
// counter that cannot name its cause buys another launch that reproduces the same zero.
enum class BodyRefusal : uint8_t {
    None = 0,       // nothing refused
    NoCi,           // the SR15NetPhysicsCI element itself did not resolve
    NoChain,        // the PRIMARY_BODY / RESOLVE_BODY prologue guards refused: not the pinned build
    // The three IDENTITY gates.  Everything below them reads numbers OFF the CI, so a CI that is
    // not the object the handle names produces a confident wrong answer at every one of them --
    // which is the fault no arm in this enum could name when 7068 of 7068 reads refused at
    // BadSlot.  They cost two u16 loads and they run before the first field is believed.
    NoReverseMap,      // CR15NetPhysicsCS + PHYS.CS_REVERSE_MAP_OFF was null: the index cannot be checked
    CiEntityMismatch,  // no entity id this CI is named by comes back to its index through the forward map
    RevMapMismatch,    // the index-to-entity map does not map this CI's index back to that entity id
    CiIndexNotLive,    // the forward map's index is at or past the net CS live count: not an instance
    RevMapAmbiguous,   // another LIVE index carries this handle too, so the reverse map identifies nothing
    ForwardMapAmbiguous, // two entity ids both round-trip to this index: the forward map names no one CI
    NoPhysicsCs,    // the space's ncaPhysics list entry exists and holds no component system
    NoPhysicsPool,  // the CPhysicsCS handle map or pool base was null
    MapUnreadable,  // the handle map is mapped too short to hold this body id's slot
    BadSlot,        // that slot is at or past the CPhysicsCS live count
    CiUnreadable,   // the CPhysicsCI the slot addresses is not mapped
    NoPrimaryBody,  // PHYS.PHYSICS_CI_BODY_GUARD_OFF was 0 -- this entity HAS no primary body
    DeadTarget,     // the CPhTarget carries the dead index
    NoPhysSystem,   // the CPhSystem, or its zone array, was null
    BadZone,        // the target's zone index is at or past the CPhSystem zone count
    ZoneUnreadable, // the CPhZone that index addresses is not mapped
    DeadZone,       // that CPhZone is a recycled slot
    BadBodyIndex,   // the target's body index is at or past the CPhZone body count
    NoBodyArray,    // the CPhZone's body array was null
    Unreadable,     // the CPhBody address resolved but its read span is not mapped
    DeadBody,       // that CPhBody is a recycled slot
    // The ncaTransform FALLBACK's own arms.  They are reported in BodyDiag::xformArm, never in
    // `refusedAt`: this chain only runs after the CPhBody chain has already refused, and the
    // FIRST refusal is the one src/capture publishes.
    NoTransformCs,          // CR15NetPhysicsCS + XFORM.NETCS_XFORM_CS_OFF was null
    NoTransformPool,        // the ncaTransform CS's handle map or pool base was null
    TransformMapUnreadable, // that handle map is mapped too short to hold this entity's entry
    NoTransformInstance,    // HasComponent's own rule: the index is at or past the live count
    TransformUnreadable,    // the instance, or a node on its parent chain, is not mapped
    NodeChainTooDeep,       // the parent walk passed its bound without reaching a root
    NodeChainUnproved,      // the instance delegates to a CNode3D and this reader cannot compose it
    TagMismatch,    // the CPhBody's generation does not match the CPhTarget's
    // The ncaPhysics CS SEARCH.  PHYS.NETCS_PHYSICS_CS_OFF is one cached pointer; the space's
    // registry entry for that symbol is a LIST, and the engine's own per-frame producer picks
    // from it by HasComponent.  These are appended, never inserted, so no arm above renumbers.
    NoPhysicsSpace,          // CR15NetPhysicsCS + CS.NETCS_SPACE_OFF was null
    NoPhysicsCsList,         // the space's registry holds no entry for the ncaPhysics symbol
    PhysicsCsListUnreadable, // the registry, or the list array, is not mapped over its own count
    NoPhysicsCsForEntity,    // the list was walked and no CS in it claims this entity
};

// Which BodyDiag fields the walk actually reached.  Not decoration: every numeric field below
// has a legitimate all-ones AND a legitimate zero value, so an unreached field has no spare
// encoding of its own and must be readable as unreached rather than as a measurement.
enum BodyDiagField : uint32_t {
    kBodyDiagCi        = 1u << 0,
    kBodyDiagPhysCs    = 1u << 1,
    kBodyDiagBodyId    = 1u << 2,
    kBodyDiagSlot      = 1u << 3,
    kBodyDiagLiveCount = 1u << 4,
    kBodyDiagBypass    = 1u << 5,
    kBodyDiagCiEntity  = 1u << 6,
    kBodyDiagCiIndex   = 1u << 7,
    kBodyDiagRevHandle = 1u << 8,
    kBodyDiagAngVel    = 1u << 9,
    kBodyDiagIdentity  = 1u << 10,
    kBodyDiagOrigin    = 1u << 11,
    kBodyDiagCphArm    = 1u << 12,
    kBodyDiagXformArm  = 1u << 13,
    kBodyDiagXformIdx  = 1u << 14,
    kBodyDiagXformLive = 1u << 15,
    kBodyDiagCsIdx     = 1u << 16,
    kBodyDiagCsCount   = 1u << 17,
    kBodyDiagCiFrom    = 1u << 18,
    kBodyDiagNetLive   = 1u << 19,
    kBodyDiagMapHigh   = 1u << 20,   // `filled` is 32 bits wide; 21 of them are spoken for
};

// WHICH READING of the caller's u16 produced the CI.  Two callers produce one and they do not mean
// the same thing by it: the enumerating caller walks i in [0, PHYS.CS_COUNT_OFF) and its value is a
// POOL INDEX it has already bounded, while a caller holding a component handle means an id the
// forward map has to turn into an index first.  A refusal or a success that does not say which
// reading answered cannot be scored -- L2 #13 refused 7062 of 7062 bodies because the walk's index
// was re-derived through the map as though it were a handle.
enum class CiPath : uint8_t {
    NotAsked = 0,  // the CI never resolved
    Handle   = 1,  // the forward handle map answered and HasComponent's bound accepted its index
    Pool     = 2,  // that id has no component of this system; the value is the walk's own index
};

inline const char* CiPathName(CiPath p) {
    switch (p) {
        case CiPath::NotAsked: return "notAsked";
        case CiPath::Handle:   return "handle";
        case CiPath::Pool:     return "pool";
    }
    return "notAsked";
}

// HOW the CI's identity was proved, because the two handle comparisons are not always able to
// prove it.  The local pawn's handle is 0, and 0 is also what an unwritten CI's entity id and an
// unwritten reverse-map entry read as, so `ent == handle` and `rev == handle` are both satisfied
// by zero fill on exactly the object they exist to catch.  On that handle the equality gates are
// NOT APPLICABLE and the proof has to come from somewhere a zero fill cannot reach: the reverse
// map is a mapping over the live range (ENTITY.UPDATE_TARGET_STATE walks i in [0, count) and
// reads revMap[i] as that instance's entity id), so no OTHER live index may carry this handle.
enum class IdentityProof : uint8_t {
    NotAsked        = 0,  // the gate never ran
    HandleCompare   = 1,  // handle != 0: both equality gates discriminate, and both agreed
    UniqueLiveIndex = 2,  // handle == 0: the equality gates are notApplicable, uniqueness proved it
    Refused         = 3,  // a gate refused; refusedAt names which
    // The pool reading.  Both u16s the gate holds there are ENTITY IDS -- the reverse map's value
    // is the subscript of the stride-4 forward map at 0x140D87E33 -- so an inequality between them
    // compares like with like and still names no culprit.  What discriminates is the engine's own
    // round trip, forwardMap[id].low == this index, asked of each.  Which one survived it is the
    // whole of what L2 #14's `ent=3 rev=40` could not say.
    MapRoundTrip    = 4,  // the reverse map's entity id came back to this index
    CiIdRoundTrip   = 5,  // only the CI's own entity id did: the reverse map's entry names another
};

inline const char* IdentityProofName(IdentityProof p) {
    switch (p) {
        case IdentityProof::NotAsked:        return "notAsked";
        case IdentityProof::HandleCompare:   return "handleCompare";
        case IdentityProof::UniqueLiveIndex: return "uniqueLiveIndex";
        case IdentityProof::MapRoundTrip:    return "mapRoundTrip";
        case IdentityProof::CiIdRoundTrip:   return "ciIdRoundTrip";
        case IdentityProof::Refused:         return "refused";
    }
    return "notAsked";
}

// The bypass term is THREE-valued at the reader, not two.  ReadLocalBody applies the live-count
// bound unless the bit is Set, so Clear and Unreadable gate identically -- but they diagnose
// oppositely: Clear means the engine's own PrimaryBody would refuse this slot too, Unreadable
// means we never got to ask.  L2 #11 concluded "the bit is CLEAR" from an arm that cannot tell
// those apart; this field is what makes that conclusion checkable.
enum class BypassRead : uint8_t { Clear = 0, Set = 1, Unreadable = 2 };

inline const char* BypassReadName(BypassRead b) {
    switch (b) {
        case BypassRead::Clear:      return "clear";
        case BypassRead::Set:        return "set";
        case BypassRead::Unreadable: return "unreadable";
    }
    return "unknown";
}

// WHICH chain produced a BodySource::Local reading.  The two are not equivalent and the
// difference is not cosmetic: the CPhBody carries a centre of mass, a linear velocity and a
// cloud the engine can derive omega from, while the ncaTransform carries a POSE and nothing
// else.  A capture that cannot tell them apart would read a transform-sourced com as a centre
// of mass -- it is the body ORIGIN, which differs from the com by PHYS.BODY_COM_OFFSET_OFF --
// and a structural zero velocity as a body at rest.
enum class BodyOrigin : uint8_t {
    NotAsked = 0,
    CphBody,         // PHYS.PRIMARY_BODY / PHYS.RESOLVE_BODY resolved a live CPhBody
    LocalTransform,  // the ncaTransform CS at XFORM.NETCS_XFORM_CS_OFF: pose only
};

inline const char* BodyOriginName(BodyOrigin o) {
    switch (o) {
        case BodyOrigin::NotAsked:       return "notAsked";
        case BodyOrigin::CphBody:        return "cphBody";
        case BodyOrigin::LocalTransform: return "localTransform";
    }
    return "unknown";
}

// WHERE out.angVel came from.  The two body arms do not answer this field the same way and the
// difference is not an opinion: a history node carries a stored PHYS.NODE_ANGVEL_OFF the engine
// wrote, while a locally simulated CPhBody carries NO angular velocity anywhere -- the engine
// least-squares one out of the body's particle cloud (sub_140643B30) at each of the two netCS
// sites that need it and stores the result nowhere.  Before this existed the local arm returned
// PHYS.BODY_PREV_VEL_OFF, the previous tick's LINEAR velocity, in that field, and nothing
// downstream could tell that from a measurement.  NotAsked is distinct from NotStoredByEngine
// for the usual reason: an arm that never reached the field must not claim the engine has none.
enum class AngVelSource : uint8_t {
    NotAsked = 0,
    NodeStored,          // read from PHYS.NODE_ANGVEL_OFF on a history node
    NotStoredByEngine,   // a live CPhBody: the quantity is derived on demand, never stored
    EngineComputed,      // PHYS.ANGVEL_FROM_CLOUD answered for a live CPhBody -- a measurement.
                         // Its (0,0,0) is a real reading (asleep body / empty cloud), not a gap.
    Unavailable,         // the ncaTransform arm answered: there is no CPhBody, so no cloud to
                         // derive omega from and no field anywhere that holds one.  The zero in
                         // out.angVel is structural and must never be read as a rate.
};

inline const char* AngVelSourceName(AngVelSource s) {
    switch (s) {
        case AngVelSource::NotAsked:          return "notAsked";
        case AngVelSource::NodeStored:        return "nodeStored";
        case AngVelSource::NotStoredByEngine: return "notStoredByEngine";
        case AngVelSource::EngineComputed:    return "engineComputed";
        case AngVelSource::Unavailable:       return "unavailable";
    }
    return "unknown";
}

// A pointer's IDENTITY without its ADDRESS.  A stop line has to be able to say "every tick saw
// the same CPhysicsCS" or "the CI and the CS came from different objects" without putting a live
// address out of the owner's process into a file that leaves their machine.  Equal pointers give
// equal tags; the 64->32 fold is not invertible.  Zero is reserved for the null pointer, so a
// tag of 0 means "we read it and it was null", never "we did not read it" -- that is `filled`.
inline uint32_t BodyDiagTag(const void* p) {
    uint64_t x = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p));
    if (x == 0) return 0;
    x ^= x >> 33; x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33; x *= 0xC4CEB9FE1A85EC53ull;
    x ^= x >> 33;
    const uint32_t t = static_cast<uint32_t>(x);
    return t ? t : 1u;
}

// The NUMBERS the local arm's gates were computed FROM.  `refusedAt` names WHICH gate refused;
// on its own it cannot separate "a body id nothing ever wrote" from "a slot in a pool that is
// simply smaller than it", and L2 #11 spent a whole launch on exactly that ambiguity.  Filling
// these costs nothing on the tick path -- every value is already in a register at the gate.
struct BodyDiag {
    uint32_t   filled    = 0;                        // an OR of BodyDiagField
    uint32_t   bodyId    = 0;                        // the RAW dword at PHYS.CI_BODY_ID_OFF, before the u16 truncation
    uint16_t   slot      = 0;                        // the low half of the handle-map entry
    uint16_t   liveCount = 0;                        // PHYS.CS_COUNT_OFF on the same CPhysicsCS
    BypassRead bypass    = BypassRead::Unreadable;   // the [[physCs+STATE_OBJ]+STATE_FLAGS]&1 term
    uint32_t   physCsTag = 0;                        // BodyDiagTag of PHYS.NETCS_PHYSICS_CS_OFF's value
    uint32_t   ciTag     = 0;                        // BodyDiagTag of the SR15NetPhysicsCI walked
    // The identity gates' three numbers.  `ciEntityId` is the CI's own first field, `ciIndex` the
    // pool index the handle map gave, `revHandle` what the index-to-handle map says that index is.
    // A refusal that prints all three says WHICH of the two directions disagreed, and a SUCCESS
    // that prints them is the record that the CI walked was the CI the handle names.
    uint16_t   ciEntityId = 0;
    uint16_t   ciIndex    = 0;
    uint16_t   revHandle  = 0;
    // WHICH reading produced `ciIndex`, the count that bounded it, and the half of the forward
    // map entry nothing has ever read.  `netLive` is the NET CS's PHYS.CS_COUNT_OFF and is a
    // different number from `liveCount`, which is the CPhysicsCS's: the refusal that is entirely
    // about an index-versus-count comparison used to print the index and withhold the count.
    // `mapHigh` is the u16 at map + 4*id + 2 -- Element() reads only the low one, so a 1-based or
    // half-swapped entry was unreadable from any artefact this reader produced.
    CiPath     ciFrom  = CiPath::NotAsked;
    uint16_t   netLive = 0;
    uint16_t   mapHigh = 0;
    // Which of the two proofs stood.  `handleCompare` on a zero handle would be a 0 == 0 pass and
    // is never written there; the stop line can tell the two apart without re-deriving the handle.
    IdentityProof identity = IdentityProof::NotAsked;
    // Which arm answered out.angVel, and whether the engine had one to give at all.
    AngVelSource angVel = AngVelSource::NotAsked;
    // WHICH chain answered, and what each chain said if it did not.  `cphBodyArm` is the only
    // record that survives a SUCCESSFUL fallback read -- `refusedAt` has to be None there or
    // src/capture's refusal histogram counts a refusal against a body it recorded -- so a run
    // whose bodies all came from the transform still names the reason the physics chain could
    // not answer.  `xformArm` carries the fallback's own refusal, including when both refused.
    BodyOrigin  origin     = BodyOrigin::NotAsked;
    BodyRefusal cphBodyArm = BodyRefusal::None;
    BodyRefusal xformArm   = BodyRefusal::None;
    // The ncaTransform CS's own index and live count, at the gate that compared them -- the
    // fallback's BadSlot, and unreadable from `slot`/`liveCount`, which are the physics CS's.
    uint16_t xformIndex = 0;
    uint16_t xformLive  = 0;
    // WHICH ncaPhysics CS in the space's list claimed this entity, and how many were offered.
    // `csIdx` is the whole point of the search: a nonzero index is a live entity whose body the
    // cached pointer at PHYS.NETCS_PHYSICS_CS_OFF would have missed, and a `csCount` of 1 says
    // the list could not have told the two shapes apart on this session.
    uint16_t csIdx   = 0;
    uint32_t csCount = 0;
};

// One short `field=value` line for a stop document or a log.  An unreached field prints `?`,
// never a zero -- a zero here would be scored as a measurement.  Always NUL-terminates; returns
// `out` so it can be used directly as a printf argument.  `cap` of 0 is a no-op returning "".
const char* DescribeDiag(const BodyDiag& d, char* out, size_t cap);

// Enough for every field at its widest, with room for the caller not to have to think about it.
// Sized from the LONGEST member of every field, summed: 278 bytes, plus 30 for `csIdx=65535`
// and `csCount=4294967295` with their separators, plus 44 for ` ciFrom=notAsked`, ` netLive=65535`
// and ` mapHigh=65535`, plus the NUL.  The widest single term is a BodyRefusalName
// (`physicsCsListUnreadable`, 23) and it appears twice.
inline constexpr size_t kBodyDiagTextMax = 400;

struct BodyReading {
    BodySource  source         = BodySource::NotAsked;
    bool        ciResolved     = false;  // the SR15NetPhysicsCI element itself
    bool        historyPresent = false;  // a history container AND a head node on it
    bool        bodyResolved   = false;  // the CPhBody address resolved and is mapped
    BodyRefusal refusedAt      = BodyRefusal::None;
    BodyDiag    diag;                    // what the gates SAW, beside which gate refused
};

// WHY an install into the engine's own history ring refused.  A SEPARATE enum from BodyRefusal,
// not an addition to it: BodyRefusal's numeric values index the capture's refusal histogram
// (kBodyRefusalCount) and are written into recorded files, so an enumerator inserted there
// renumbers what an already-recorded capture means.  A write's gates are not a read's anyway.
enum class InstallRefusal : uint8_t {
    None = 0,            // the window installed
    BadWindow,           // no samples, or more than the ring can hold
    NoElement,           // the physics CI element did not resolve for this handle
    NoRecycler,          // this build has no unlink/recycle pair, so the list cannot be rebuilt
    NoHistoryList,       // the element carries no history container
    CiIdentityMismatch,  // the element resolves and CheckCiIdentity declines to vouch for it
    ShortInstall,        // the node pool ran short: the list is not the window that was asked for
};

// The install's answer: WHY it refused and WHICH arm of ResolveCi addressed the element. The arm
// is on the record because the two arms address DIFFERENT elements -- Handle takes
// ElementAt(handleMap[4v]), Pool takes ElementAt(v) -- so a refusal without it names a gate over an
// object the reader cannot identify.
struct InstallReading {
    InstallRefusal why  = InstallRefusal::None;
    CiPath         from = CiPath::NotAsked;
};

inline const char* InstallRefusalName(InstallRefusal r) {
    switch (r) {
        case InstallRefusal::None:               return "none";
        case InstallRefusal::BadWindow:          return "badWindow";
        case InstallRefusal::NoElement:          return "noElement";
        case InstallRefusal::NoRecycler:         return "noRecycler";
        case InstallRefusal::NoHistoryList:      return "noHistoryList";
        case InstallRefusal::CiIdentityMismatch: return "CiIdentityMismatch";
        case InstallRefusal::ShortInstall:       return "shortInstall";
    }
    return "unknown";
}

// Every enumerator spelled, for the same reason BodySourceName spells its four: an uncased value
// must read as "unknown", never as a blank field a log consumer scores as health.
inline const char* BodyRefusalName(BodyRefusal r) {
    switch (r) {
        case BodyRefusal::None:           return "none";
        case BodyRefusal::NoCi:           return "noCi";
        case BodyRefusal::NoChain:        return "noChain";
        case BodyRefusal::NoReverseMap:     return "noReverseMap";
        case BodyRefusal::CiEntityMismatch: return "ciEntityMismatch";
        case BodyRefusal::RevMapMismatch:   return "revMapMismatch";
        case BodyRefusal::CiIndexNotLive:   return "ciIndexNotLive";
        case BodyRefusal::RevMapAmbiguous:  return "revMapAmbiguous";
        case BodyRefusal::ForwardMapAmbiguous: return "forwardMapAmbiguous";
        case BodyRefusal::NoTransformCs:          return "noTransformCs";
        case BodyRefusal::NoTransformPool:        return "noTransformPool";
        case BodyRefusal::TransformMapUnreadable: return "transformMapUnreadable";
        case BodyRefusal::NoTransformInstance:    return "noTransformInstance";
        case BodyRefusal::TransformUnreadable:    return "transformUnreadable";
        case BodyRefusal::NodeChainTooDeep:       return "nodeChainTooDeep";
        case BodyRefusal::NodeChainUnproved:      return "nodeChainUnproved";
        case BodyRefusal::NoPhysicsCs:    return "noPhysicsCs";
        case BodyRefusal::NoPhysicsPool:  return "noPhysicsPool";
        case BodyRefusal::MapUnreadable:  return "mapUnreadable";
        case BodyRefusal::BadSlot:        return "badSlot";
        case BodyRefusal::CiUnreadable:   return "ciUnreadable";
        case BodyRefusal::NoPrimaryBody:  return "noPrimaryBody";
        case BodyRefusal::DeadTarget:     return "deadTarget";
        case BodyRefusal::NoPhysSystem:   return "noPhysSystem";
        case BodyRefusal::BadZone:        return "badZone";
        case BodyRefusal::ZoneUnreadable: return "zoneUnreadable";
        case BodyRefusal::DeadZone:       return "deadZone";
        case BodyRefusal::BadBodyIndex:   return "badBodyIndex";
        case BodyRefusal::NoBodyArray:    return "noBodyArray";
        case BodyRefusal::Unreadable:     return "unreadable";
        case BodyRefusal::DeadBody:       return "deadBody";
        case BodyRefusal::TagMismatch:    return "tagMismatch";
        case BodyRefusal::NoPhysicsSpace:          return "noPhysicsSpace";
        case BodyRefusal::NoPhysicsCsList:         return "noPhysicsCsList";
        case BodyRefusal::PhysicsCsListUnreadable: return "physicsCsListUnreadable";
        case BodyRefusal::NoPhysicsCsForEntity:    return "noPhysicsCsForEntity";
    }
    return "unknown";
}

// The same values as a sentence, for the one line a capture prints when it sampled no body at
// all. The token above is what a histogram is keyed on; this is what a human reads.
inline const char* Describe(BodyRefusal r) {
    switch (r) {
        case BodyRefusal::None:           return "nothing refused";
        case BodyRefusal::NoCi:           return "the physics CI element did not resolve";
        case BodyRefusal::NoChain:        return "the CPhBody chain's prologue guards refused: this is not the pinned build";
        case BodyRefusal::NoReverseMap:     return "the net physics CS carries no index-to-entity map, so the CI's identity cannot be checked";
        case BodyRefusal::CiEntityMismatch: return "neither the index-to-entity map's entity id nor the CI's own comes back to this index through the forward map";
        case BodyRefusal::RevMapMismatch:   return "the index-to-entity map does not map this CI's pool index back to the entity id that addressed it";
        case BodyRefusal::CiIndexNotLive:   return "the handle map's index is at or past the net physics CS live count: it addresses no live instance";
        case BodyRefusal::RevMapAmbiguous:  return "another live index carries this handle too, so the reverse map does not identify this CI";
        case BodyRefusal::ForwardMapAmbiguous: return "two different entity ids both map to this index, so the forward map does not identify this CI";
        case BodyRefusal::NoTransformCs:          return "the net physics CS carries no ncaTransform CS pointer";
        case BodyRefusal::NoTransformPool:        return "the ncaTransform CS has no handle map or no pool base";
        case BodyRefusal::TransformMapUnreadable: return "the ncaTransform handle map is too short for this entity id";
        case BodyRefusal::NoTransformInstance:    return "this entity has no ncaTransform component in the ncaTransform CS";
        case BodyRefusal::TransformUnreadable:    return "the ncaTransform instance, or a node on its parent chain, is not mapped";
        case BodyRefusal::NodeChainTooDeep:       return "the CNode3D parent walk passed its depth bound without reaching a root";
        case BodyRefusal::NodeChainUnproved:      return "the ncaTransform instance delegates to a CNode3D, whose world composition this reader does not reproduce";
        case BodyRefusal::NoPhysicsCs:    return "the space's ncaPhysics list holds no component system";
        case BodyRefusal::NoPhysicsPool:  return "the CPhysicsCS has no handle map or no pool base";
        case BodyRefusal::MapUnreadable:  return "the CPhysicsCS handle map is too short for this body id";
        // NOT "a slot past the live count".  CPhysicsCS::PrimaryBody performs, byte for byte, the
        // body of ENTITY.HAS_COMPONENT(cs, entityId) -- `[map + 4*entityId] < [cs+0F4h]`, where
        // the operand is an ENTITY id and 0F4h is the LIVE INSTANCE count.  The condition means
        // the entity has no ncaPhysics component in that CS, not that an index ran off the end.
        case BodyRefusal::BadSlot:        return "this entity has no ncaPhysics component in the CPhysicsCS";
        case BodyRefusal::CiUnreadable:   return "the CPhysicsCI that slot addresses is not mapped";
        case BodyRefusal::NoPrimaryBody:  return "this entity has no primary body";
        case BodyRefusal::DeadTarget:     return "the CPhTarget carries the dead index";
        case BodyRefusal::NoPhysSystem:   return "the CPhSystem or its zone array is null";
        case BodyRefusal::BadZone:        return "the CPhTarget names a zone past the zone count";
        case BodyRefusal::ZoneUnreadable: return "the CPhZone it names is not mapped";
        case BodyRefusal::DeadZone:       return "the CPhZone it names is a recycled slot";
        case BodyRefusal::BadBodyIndex:   return "the CPhTarget names a body past the zone's body count";
        case BodyRefusal::NoBodyArray:    return "the CPhZone has no body array";
        case BodyRefusal::Unreadable:     return "the CPhBody resolved but its read span is not mapped";
        case BodyRefusal::DeadBody:       return "the CPhBody it names is a recycled slot";
        case BodyRefusal::TagMismatch:    return "the CPhBody's generation does not match the CPhTarget's";
        case BodyRefusal::NoPhysicsSpace:          return "the net physics CS carries no space, so its ncaPhysics list cannot be found";
        case BodyRefusal::NoPhysicsCsList:         return "the space's component-system registry holds no ncaPhysics entry";
        case BodyRefusal::PhysicsCsListUnreadable: return "the ncaPhysics registry or list is not mapped over its own count";
        case BodyRefusal::NoPhysicsCsForEntity:    return "no ncaPhysics component system in the space's list claims this entity";
    }
    return "unknown refusal";
}

// Every enumerator spelled, and an uncased value is "unknown" rather than an empty string: a
// blank field in a diagnostic reads as evidence of health, and this one would be lying.
inline const char* BodySourceName(BodySource s) {
    switch (s) {
        case BodySource::NotAsked: return "notAsked";
        case BodySource::None:     return "none";
        case BodySource::History:  return "history";
        case BodySource::Local:    return "local";
    }
    return "unknown";
}

// The wire field set of S5.4.5.  bits28 / holdL / holdR live in the RAW block at PlayerCI+140,
// NOT in a pose slot: only bit 27 of bits28 reaches the slots (docs/spikes/W1-E.md S5).
struct PlayerState { float hmdOri[4], lHandOri[4], rHandOri[4], lHandOff[3], rHandOff[3], refVel[3],
                     bendBack, bendFwd, restPoseHeight, trigL, trigR;
                     uint32_t bits28, holdL, holdR, anchor; };

// Which PlayerState fields ReadLocalPlayer filled.  Each bit names the group one engine accessor
// supplies, because that is the granularity at which a source can be present or absent.
//
// THE GROUPS ARE DECLARED ONCE, HERE. kLocalAll and LocalGroupName are both generated
// from this list, so "the bit was added and kLocalAll was not" -- which leaves every consumer's
// totality check passing on a mask that did not change -- is not a mistake this file can hold.
#define REPLAY_LOCAL_GROUP_BITS(X)                                          \
    X(kLocalPose,    0)  /* hmdOri, lHandOri, rHandOri, lHandOff, rHandOff */ \
    X(kLocalRefVel,  1)  /* refVel */                                       \
    X(kLocalBody,    2)  /* bendBack, bendFwd, restPoseHeight */            \
    X(kLocalTrigger, 3)  /* trigL, trigR */                                 \
    X(kLocalAnchor,  4)  /* anchor */                                       \
    X(kLocalHolding, 5)  /* holdL, holdR */

enum : uint32_t {
#define REPLAY_LOCAL_GROUP_BIT(name, index) name = 1u << (index),
    REPLAY_LOCAL_GROUP_BITS(REPLAY_LOCAL_GROUP_BIT)
#undef REPLAY_LOCAL_GROUP_BIT

    // NOT DEFINED, and its absence is the statement: there is no kLocalActionWord, because
    // ReadLocalPlayer cannot source bits28 today.  The engine assembles that word twice, once per
    // arm of CR15PlayerBroadcasterCS::UpdateAfterPhysics, from DIFFERENT components -- the same bit
    // position comes from CR15HandGestureOverrideCS for a human and CR15NetBotAnimCS for a bot.
    // Sourcing half of it and shipping a word would put a zero that means "unsourced" in the same
    // field as a zero that means "not pressed", which is the one thing this mask exists to prevent.
    // docs/spikes/W1-E.md S13 maps every bit of both arms to its accessor.

    // Every group bit this interface can set, so a consumer that translates the mask can assert its
    // table is TOTAL rather than discover a missing line as a dropped row. L2-RUN-2 F2: kLocalHolding
    // was added here and W2-A's translator had no line for it, so a pawn the engine sourced came back
    // as fill 0 -- refused, no row written -- while the reading still said `ok`. A bit added above and
    // nowhere else must break a build, not a capture.
#define REPLAY_LOCAL_GROUP_OR(name, index) | name
    kLocalAll = 0 REPLAY_LOCAL_GROUP_BITS(REPLAY_LOCAL_GROUP_OR),
#undef REPLAY_LOCAL_GROUP_OR
};

// One group bit's own name, for a log line that has to say WHICH group was lost. Generated from the
// same list as the bits, so it cannot fall behind them; the default is a distinct word rather than
// any real group's, so an unknown bit reads as unknown instead of as kLocalPose.
inline const char* LocalGroupName(uint32_t bit) {
    switch (bit) {
#define REPLAY_LOCAL_GROUP_NAME(name, index) case name: return #name;
        REPLAY_LOCAL_GROUP_BITS(REPLAY_LOCAL_GROUP_NAME)
#undef REPLAY_LOCAL_GROUP_NAME
        default: return "kLocal<unknown>";
    }
}

// ★ WHY THE ADDITIONS BELOW HAVE BODIES AND THE ORIGINAL METHODS DO NOT.
//
// B.4 was frozen with pure virtuals, and every method that was in it stays pure: an implementer that
// forgets one of those is a bug the compiler should catch.  The methods added after the freeze are
// virtual WITH a default instead, and the default is precisely the "this build could not source it"
// answer the contract already defines -- an empty local-player list, a zero fill mask, a net id of 0,
// PhysOfNetId returning 0xFFFF, ModelVisible returning false with `visible` false.
//
// This is not politeness toward existing implementers.  Making them pure broke W2-B's FakeEntity in
// tests/playback the moment the first one landed, in a shared tree, for a capability that test does
// not exercise and has no opinion about.  A default that returns the documented unavailable value
// keeps every existing implementer honest -- W2-A's gap list is DERIVED from the mask, so a fake that
// sources nothing reports sourcing nothing -- while a compile error in someone else's packet would
// have taught only that the interface changed.
class IEntity {
public:
    // ---- PhysCI pool (stride 504 on PC) -------------------------------------------
    virtual size_t PhysCount() const = 0;
    virtual bool   IsLocallyControlled(PhysHandle) const = 0;   // reimplemented; S5.3.2
    virtual void   SetSyntheticOwner  (PhysHandle) = 0;         // PhysCI+28, HP1 Attack B
    virtual void   ClearSyntheticOwner(PhysHandle) = 0;
    // Discard the record of a claim WITHOUT restoring the owner. For the one case where restoring
    // is the wrong write: the component the claim was stamped into has been destroyed or recycled,
    // so its old owner dword belongs to whatever now occupies that pool slot.
    virtual void   ForgetSyntheticOwner(PhysHandle) {}
    // The recorded remote entrant's stamp. Same single dword store as SetSyntheticOwner, but its
    // identity gate drops ONLY the ci[+0].entityId == handle field compare: the engine's own
    // UPDATE_TARGET_STATE drives this body by pool index and identifies it via the reverse map, never
    // demanding that field, so our reader's extra demand is stricter than the engine and refuses the
    // very element the engine drives. The reverse-map inverse guard (revMap[index] == handle) stays,
    // so a stale/reused forward entry still refuses. Scoped to the replay applier's remote slot
    // surface; every other caller keeps the full gate. Default forwards to the gated store
    // (DIAG-POSE-DECOUPLE 4b).
    virtual void   SetSyntheticOwnerDriven(PhysHandle h) { SetSyntheticOwner(h); }
    // ---- the disc's RECORDED holder ------------------------------------------------
    // The same single dword store as SetSyntheticOwner, with a real SR15NetUserId instead of the
    // out-of-table sentinel. The sentinel was chosen to fail IsLocallyControlled's `(u16)owner <
    // 0x10` early-out, and the two holder gates CR15NetTouchInteractCS asks -- sub_140CED3E0 and
    // sub_140CED640 -- ride on the SAME early-out, so it also tells the engine that nobody is
    // holding the disc. A live REMOTE user id fails the local band and passes the remote one, which
    // is both properties at once: the engine still will not simulate the body, and it fires its own
    // OnNetTouchGrab / OnNetTouchRelease. Returns false when the element did not resolve or the
    // store refused; the caller must not record a claim it did not make.
    virtual bool   SetRecordedOwner(PhysHandle, uint32_t) { return false; }
    // The live SR15NetUserId for an engine user slot: {u16 slot, u16 generation}, the generation
    // read from CR15NetGame's own per-slot table so the value is the one the gates compare against
    // rather than one we remembered. False when the slot is out of the table, the netGame is not
    // reachable, or the slot is not occupied.
    virtual bool   UserIdOf(UserIndex, uint32_t&) const { return false; }
    // ---- what the ENGINE would answer for this body --------------------------------
    // sub_140CEDEB0 hands CR15NetTouchInteractCS the owner dword only when one of its two band
    // gates passes; otherwise it answers with the invalid id and the touch system reads the body
    // as unheld. Our own stamp proves nothing about that, so this re-derives both gates READ-ONLY
    // off the same pointers they walk -- no engine call, no store -- and reports each term. It is
    // the only thing that can tell "held" apart from "the slot is a live index, not a user slot",
    // "the node's 0x200 never got set", and "the generation moved on".
    struct HolderReading {
        bool     read       = false;    // the element and the CR15NetGame both resolved
        uint32_t owner      = 0;        // PhysCI+0x1C as the gates read it
        bool     held       = false;    // one of the two gates passes: the engine reports a holder
        bool     local_arm  = false;    // sub_140CED3E0
        bool     remote_arm = false;    // sub_140CED640
        bool     gen_ok     = false;    // genTable[(u16)owner] == HIWORD(owner)
        bool     held_bit   = false;    // the head node's 0x200, the remote arm's last term
        // ★ THE WORD held_bit WAS DERIVED FROM, verbatim. One player of two reads `held bit 0`
        // with a good generation and an in-band slot at the same moments on every pass, and the
        // boolean alone cannot say whether 0x200 is the wrong bit for that grab (a different hand,
        // side or team would show a DIFFERENT bit set here) or whether no grab state reached the
        // node at all (this word reads 0). Two readings of this field decide it without a guess.
        bool     node_read  = false;    // the head node's state word was reachable and read
        uint16_t node_state = 0;        // PHYS_NODE_STATE_OFF as read
        bool     sim_bit    = false;    // PhysCI+4 & 0x4000, the local arm's last term
        uint16_t live_slot  = 0xFFFFu;  // slotTable[(u16)owner]  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        uint16_t live_gen   = 0xFFFFu;  // genTable[(u16)owner]  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        uint16_t band_lo    = 0;        // netGame+224: the first REMOTE slot
        uint16_t band_hi    = 0;        // netGame+220: one past the last
    };
    virtual bool EngineHolderOf(PhysHandle, HolderReading&) const { return false; }
    // The value SetSyntheticOwner stamps. The applier needs to name it to publish "nobody holds the
    // disc" through the same path as a real holder, and to tell its own stamp from the engine's.
    virtual uint32_t SyntheticOwnerValue() const { return 0; }
    // The remote-replay read-back oracle for the pawn line (src=/read=). Same read as
    // ReadBodyDetailed, its identity gate relaxed the same way SetSyntheticOwnerDriven's is, so a
    // working driven install reports source=History off the node it pushed instead of refusing at
    // CiEntityMismatch. Scoped to slot_remote tracks; default forwards to the gated read.
    virtual bool   ReadBodyDetailedDriven(PhysHandle h, BodyState& s, BodyReading& r) const {
        return ReadBodyDetailed(h, s, r);
    }
    virtual bool   ReadBody   (PhysHandle, BodyState&) const = 0;
    // The same read, plus which of the two sources answered and how far the other one got.
    // The default forwards, so an IEntity that predates it still compiles and says NotAsked
    // instead of inventing a cause.
    virtual bool   ReadBodyDetailed(PhysHandle h, BodyState& s, BodyReading& r) const {
        r = BodyReading();
        return ReadBody(h, s);
    }
    virtual void   PushHistory(PhysHandle, const BodyState&, uint64_t tUs) = 0;
    virtual void   TrimHistory(PhysHandle, uint64_t keepFromUs, uint64_t keepToUs) = 0;

    // Replace the whole history list in ONE call, and read it back.
    //
    // ★ WHY THIS IS NOT n CALLS TO PushHistory.  The interpolator walks head -> +8 expecting
    // DECREASING times.  PushHistory prepends unconditionally and TrimHistory explicitly keeps
    // one sample, so a list cannot be emptied and refilled in order through them: in reverse
    // playback the window slides toward OLDER times, every push lands a sample newer than
    // nothing, and the body freezes.  One call makes an out-of-order list unrepresentable --
    // this one orders by time itself, so no caller can produce one -- and n pushes cannot.
    //
    // `n` must be 1..kMaxHistoryWindow.  Zero is refused rather than treated as a clear: a list
    // with no samples is the same freeze by another route.
    //
    // A false return means the window is NOT what you asked for, and it is not a promise that
    // nothing changed: bad arguments and an unresolved pool are rejected before anything is
    // touched, but a node pool that runs short mid-rebuild leaves a SHORTER list.  That case is
    // detected by comparing the sample count the CI carries before and after -- the engine's own
    // exhaustion arm silently retires this CI's oldest node to make room, so without that
    // comparison a short pool would produce a plausible window missing its oldest entries.  The
    // honest recovery is to call again with fewer samples, not to trust a false return's list.
    static constexpr size_t kMaxHistoryWindow = 64;
    virtual bool   SetHistoryWindow (PhysHandle, const BodyState*, const uint64_t*, size_t) {
        return false;
    }
    // The same install, with the reason it refused.  The bool overload cannot tell an element
    // that does not resolve from one the identity gate DECLINES TO VOUCH FOR, and those are two
    // different findings: the second means we were about to drive a body whose CI our own reader
    // would refuse to read back.  Overriders implement THIS one; the bool overload delegates.
    virtual bool   SetHistoryWindowDetailed(PhysHandle h, const BodyState* samples,
                                            const uint64_t* timesUs, size_t n,
                                            InstallReading& r) {
        r = InstallReading();
        const bool ok = SetHistoryWindow(h, samples, timesUs, n);
        r.why = ok ? InstallRefusal::None : InstallRefusal::NoElement;
        return ok;
    }
    // The remote-replay slot-bound install.  Same push onto the phys CI the engine drives for this
    // handle as SetHistoryWindowDetailed, but the identity gate drops ONLY the ci[+0].entityId ==
    // handle field compare the engine never makes -- which is what refuses the recorded entrant's
    // already-present CI.  The reverse-map inverse guard (revMap[index] == handle) stays, so a
    // stale/reused forward entry still refuses rather than driving the wrong element.  Only the replay
    // applier's remote slot surface calls this; the general writer keeps the full gate.  Default
    // forwards to the gated install so an IEntity that predates it drives exactly as before.
    virtual bool   SetHistoryWindowDriven(PhysHandle h, const BodyState* samples,
                                          const uint64_t* timesUs, size_t n, InstallReading& r) {
        return SetHistoryWindowDetailed(h, samples, timesUs, n, r);
    }
    // Newest first, the order the engine's own walk produces.  Returns how many were written.
    virtual size_t ReadHistoryWindow(PhysHandle, BodyState*, uint64_t*, size_t) const { return 0; }
    // The remote-replay read-back counter (apply.cpp bodies_read_back). Resolves the element the
    // driven install wrote by relaxing the identity gate the same way SetHistoryWindowDriven does, so
    // a working install reads its own node back instead of scoring installed_unread. Scoped to
    // slot_remote tracks; default forwards to the gated read.
    virtual size_t ReadHistoryWindowDriven(PhysHandle h, BodyState* s, uint64_t* t, size_t cap) const {
        return ReadHistoryWindow(h, s, t, cap);
    }
    // The durable identity of a networked body, both ways.
    //
    // A PhysHandle is a pool slot and does not survive a session, let alone a recording; the value
    // that does is the u64 net id the wire carries.  The engine keeps a table of exactly this
    // mapping on CR15NetActorCS, and CR15NetPhysicsCS::SimStateCB resolves every inbound record
    // through it, so neither direction is a map we invented.
    //
    // NetIdOf returns 0 when the body has no net-actor entry -- which is the honest answer for a
    // purely local body -- and PhysOfNetId returns 0xFFFF for an id this session does not carry.
    // Both scan linearly on purpose: the table is sorted, but the engine's own binary search sorts
    // it first when a dirty bit is set, and a capture read that rewrites an engine table is not a
    // read.  The table is one entry per networked object, so the scan is over tens of entries.
    virtual uint64_t   NetIdOf   (PhysHandle) const { return 0; }
    virtual PhysHandle PhysOfNetId(uint64_t) const { return PhysHandle(0xFFFFu); }  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids

    // The third wire addressing: an SComponentID{u16 index, u16 generation} of CR15NetPhysicsCS sent
    // verbatim, which is how a level-placed body -- the disc -- names itself. Resolved the way
    // SimStateCB resolves it (IsComponentValid sub_1403319D0, then map -> CI): the index inside the
    // forward map, the entry's generation equal to the id's, the pool index inside the live count.
    // 0xFFFF for an id this session does not carry. `seen` reports what each step read.
    struct ComponentIdReading {
        bool     mapRead = false;
        uint16_t idCapacity = 0;
        uint16_t poolIndex = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        uint16_t generation = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        size_t   liveCount = 0;
        bool     ciFlagsRead = false;
        uint32_t ciFlags = 0;     // SR15NetPhysicsCI+0x4: 0x0800 interpolation enabled, 0x2000 paused
    };
    virtual PhysHandle PhysOfComponentId(uint32_t, ComponentIdReading* seen = nullptr) const {
        if (seen) *seen = ComponentIdReading();
        return PhysHandle(0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    }

    // The disc, through the engine's own chain rather than a component-system lookup. The frisbee
    // is registered in the LEVEL game space, which is empty at map load and which the plugin's
    // space bind never re-measures -- so the lookup could only ever miss. CR15NetGame+0x2AA8 is the
    // rules block the engine keeps validated component refs in, and sub_1401C6A40 (its own
    // game_frisbee_ownerid writer, 50 Hz on this host) walks exactly this:
    //
    //   ref = rules+0x160 -> require *(void**)ref and *(u64*)(ref[0]+0x2138) == *(u64*)(ref+0x38)
    //   frisbeeCs = *(ref+0x40), cid = *(u32*)(ref+0x48), IsComponentValid(frisbeeCs, cid)
    //   frisbeeCi = frisbeeCs[0xF8] + 560*fwd[cid] -> physId = *(u32*)(frisbeeCi+0x18)
    //   physCs = *(void**)(frisbeeCs+0x158), IsComponentValid(physCs, physId)
    //
    // The freshness stamp is the engine's invalidation signal: a mismatch means it would re-resolve
    // under a spinlock, and a read-only plugin must refuse that tick and come back, not read the
    // stale pair. rules+0x290 / rules+0x298 hold physCs / physId directly and are the cross-check.
    struct DiscBind {
        const char* refused = "this entity surface has no disc binder";   // null iff `handle` stands
        void*    frisbeeCs    = nullptr;
        void*    physCs       = nullptr;   // the disc's CR15NetPhysicsCS -- the LEVEL space's
        void*    playerPhysCs = nullptr;   // the one player bodies come from, for the comparison
        uint32_t frisbeeCid   = 0;
        uint32_t physId       = 0;         // the disc's net-physics SComponentID
        bool     shortcutAgreed = false;   // rules+0x290/+0x298 named the same {cs, id}
        // The chain resolved and named a disc, but the net-physics system does not vouch for that
        // component -- a different refusal from "the chain could not be read", and the one a bind
        // reports as no-such-component.
        bool     physPoolRefused = false;
        // The engine's cached ref was not usable and the disc was resolved out of the level space's
        // own actor registry instead. Same outputs; a different thing to re-validate.
        bool     early = false;
        uint16_t earlyActorIndex = 0xFFFFu;   // the registry slot the game_frisbee actor holds  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        // The space the frisbee system was resolved in, on WHICHEVER path bound. The disc need not
        // live in the space the pawns do, and a census that walks the pawn space reports an absence
        // it never looked for.
        void*    discSpace = nullptr;
        // What every candidate space held on this attempt. The early path refused for 552 s on one
        // launch with nothing in the log but the word "no", and a second launch to learn whether
        // the space or the symbol was wrong costs more than carrying this.
        struct EarlyDiag {
            static constexpr size_t kCandidates    = 3;
            static constexpr size_t kSymbolSamples = 16;
            static constexpr size_t kDepths        = 8;   // lp::kSpaceParentWalkMax

            // ★ `actors=0(unreadable)` said all four of these with one word, and an EMPTY space is
            // the ordinary state of a level that is still streaming -- not a fault, and the whole
            // reason the first launch could not say whether the space ever fills.
            enum class Reg : uint8_t {
                Unseen = 0,   // the walk did not reach this depth
                NoReg,        // the space is unreadable, or carries no registry pointer
                Unreadable,   // a non-zero count whose entry array does not read
                Empty,        // count == 0: created and not yet filled. sub_1404FF1A0 returns here
                Populated,    // count entries, readable, searched
            };
            struct Cand {
                const char* name  = nullptr;      // the expression that produced `root`
                void*    root     = nullptr;      // the candidate space, as read
                uint64_t count    = 0;            // actor-registry entries in `root` itself
                uint32_t depth    = 0;            // spaces reached along the parent chain
                uint16_t index    = 0xFFFFu;      // the actor index the symbol named, if any  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
                int8_t   aliasOf  = -1;           // `root` is the same pointer as this candidate
                bool     registryRead = false;    // root's registry passed the bounds guard
                bool     symbolMatched = false;   // game_frisbee's CSymbol64 is in the chain
                bool     slotLive = false;        // ...and the engine's slot word says it is live
                // EVERY space the parent walk touched, not just the root: `depth=2 actors=0` used
                // to report the root's count and say nothing at all about the parent it climbed to.
                uint64_t countAt[kDepths] = {};
                Reg      regAt[kDepths]   = {};
            };
            Cand   cand[kCandidates];
            size_t n = 0;
            // The head of the first registry the walk could read, for the case where the space is
            // right and the actor is registered under a symbol we do not know.
            uint64_t sample[kSymbolSamples] = {};
            size_t   sampleN    = 0;
            void*    sampleFrom = nullptr;
        };
        EarlyDiag earlyDiag;
        void*    earlyActorSpace = nullptr;   // the space in the chain the actor was found in
        void*    earlyRoot       = nullptr;   // the candidate root that chain started from
        uint8_t  earlySpace      = 0;         // which candidate that root was
        // The hop the EARLY path stopped at, when it was tried and did not land. `refused` stays
        // the engine ref's own refusal so the shipped bind line reads as it always has.
        const char* earlyRefused = nullptr;
        ComponentIdReading reading;
        PhysHandle handle{0xFFFFu, PhysHandle::kPoolDisc};  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    };

    // What the 2 Hz re-validation saw, for the caller that logs it. `agreed` is only meaningful
    // once `refResolved` is true: until the host reaches round start there is no second answer to
    // agree with.
    struct DiscChainReading {
        bool early       = false;   // the live bind came from the early path
        bool refResolved = false;   // the engine's cached ref now names a component
        bool agreed      = false;   // ...and it names the same {physCs, physId} we are driving
        void*    refPhysCs = nullptr;
        uint32_t refPhysId = 0;
        // Consecutive disagreeing re-validations. The drive is only dropped once this reaches the
        // implementation's confirmation count: a ref that flaps between resolved and refused would
        // otherwise drop and re-bind a good disc at the re-validation rate.
        uint8_t  disagreeStreak = 0;
    };
    // Resolves the chain and, on success, binds this surface's disc pool to `physCs`. Non-const:
    // the pool context is installed here and every later disc read and write goes through it.
    virtual bool BindDisc(void* netGame, DiscBind& out) {
        (void)netGame;
        out = DiscBind();
        return false;
    }
    // The per-drive-tick revalidation: the engine's own IsComponentValid over the bound disc pool,
    // which is one bounds test, one map load and one generation compare. False means the component
    // was destroyed or recycled and the claim must be released without a write.
    virtual bool DiscStands(uint32_t physId) const { (void)physId; return false; }

    // The slower half of the same question, for the 2 Hz maintenance cadence rather than the tick:
    // does the rules chain still resolve, is its ref fresh, and does it still name the net-physics
    // system the disc pool is bound to. The bound pool is a raw pointer into the LEVEL game space
    // and a teardown frees it without touching anything the bind keyed on, so nothing else would
    // ever notice. A surface with no chain of its own has none to invalidate and says so.
    // A disc bound by the early path has no rules ref to re-walk, so the re-validation follows
    // whichever path bound it and reports which, so the caller can log the first agreement.
    virtual bool DiscChainStands(void* netGame, DiscChainReading* seen = nullptr) const {
        (void)netGame;
        if (seen) *seen = DiscChainReading();
        return true;
    }

    // Which path the live bind came from, and the hop the early path last stopped at. The bind
    // summary is windowbuild's and carries neither, so the module logs them on their own line.
    virtual bool DiscBoundEarly() const { return false; }
    virtual const char* DiscEarlyRefusal() const { return nullptr; }
    // The candidate spaces the last early attempt looked in. Null when this surface has no early
    // path, or when the path has not been asked yet.
    virtual const DiscBind::EarlyDiag* DiscEarlyDiagnostics() const { return nullptr; }
    // The candidate space the live early bind resolved the actor in, for the log line.
    virtual const char* DiscEarlySpaceName() const { return "none"; }

    // Forget the disc pool. No write: the saved owner dword belongs to whatever the engine
    // recycled into that slot, and the pointer it would be written through may be freed.
    virtual void DropDisc() {}

    virtual float  GetTimeOffset(PhysHandle) const = 0;         // PhysCI+292
    virtual void   SetTimeOffset(PhysHandle, float seconds) = 0;

    // ---- PlayerCI pool (stride 832, both builds) -----------------------------------
    virtual size_t PlayerCount() const = 0;
    virtual bool   ReadPlayer (PlayerHandle, PlayerState&) const = 0;
    virtual void   PushPlayerHistory(PlayerHandle, const PlayerState&, uint64_t tUs) = 0;
    virtual PhysHandle   PhysOf  (PlayerHandle) const = 0;      // PlayerCI+28 is a HANDLE
    virtual PlayerHandle PlayerOf(UserIndex) const = 0;

    // ---- the local pawn ------------------------------------------------------------
    // The local player has NO SR15RemotePlayerCI, so no PlayerHandle names it and ReadPlayer
    // cannot reach it.  Its equivalent structure is SR15PlayerBroadcasterCI, stride 24, in the
    // compact pool of CR15PlayerBroadcasterCS -- one entry per player THIS machine authors and
    // broadcasts, which is the local human plus every bot on a host.  That CS has the same
    // layout as CR15RemotePlayerCS (docs/spikes/W1-E.md S10.2), so the same pool offsets read it.
    //
    // LocalPlayerAt() walks that pool in the engine's own order, so "who is local" is the
    // engine's answer and not a heuristic of ours.  The user ids it returns index the SAME user
    // slot table PlayerOf() takes, so a capture can key the local pawn into the same streams.
    //
    // ReadLocalPlayer returns a mask of the kLocal* bits below rather than a bool.  A field
    // whose bit is CLEAR was not sourced on this build; it is zero because it is absent, not
    // because it measured zero -- the same rule AsSeenSample::valid follows, and for the same
    // reason (S12.0: a wrong replay renders happily and silently).
    virtual size_t    LocalPlayerCount() const { return 0; }
    virtual UserIndex LocalPlayerAt(size_t) const { return UserIndex(0xFFFFu); }  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids

    // WHO THIS SESSION'S OWN LOCAL USER IS.  W2-B's OwnerGuard refuses `id == local_`, so it needs
    // this index and it must never be handed a fabricated one: held at a placeholder 0, it refuses
    // a GENUINE index 0 and that player never publishes.  Absence therefore gets its own value
    // rather than a number (S9.2, and the same rule kLocal* bits follow), and so does the case
    // where the pool has answers but not A SINGLE answer -- picking the first would be the same
    // defect one level down.  `out` is written only for Ok.
    enum class LocalUser : uint8_t {
        NotAvailable = 0,  // the entity did not resolve, so the broadcaster pool has no source
        None,              // resolved, and this machine authors nobody
        Ambiguous,         // this machine authors more than one: a host with bots.  Which of them
                           // is the human is not decidable from this pool
        Ok,                // `out` is the engine's own answer, read this tick.  It may be 0
    };
    //
    // There are TWO sources, because the first one cannot answer on the launch configuration that
    // matters. The broadcaster pool says who this machine AUTHORS, which on the `-offline`
    // in-client host is the human plus every bot: unambiguously more than one, so `Ambiguous` is
    // the DEFAULT there and the is-local-user arm of INV-OWN1 would never arm. CR15NetGame keeps
    // the second: its own local user's slot (localpawn_facts.h, kNetGameUserRecordOff). This
    // struct carries both as they read THIS tick, so a caller that has to log a disagreement has
    // the two numbers and not just a verdict it cannot explain. A field is meaningful only when
    // its own `Ok` flag is set; `index` only when `state` is `Ok`.
    struct LocalUserReading {
        LocalUser state = LocalUser::NotAvailable;
        UserIndex index{0xFFFFu};   // the answer; written only when state == Ok  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        bool      poolOk = false;   // the broadcaster pool named exactly one authored player
        UserIndex pool{0xFFFFu};  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        size_t    poolCount = 0;    // how many this machine authors -- pool source only
        bool      slotOk = false;   // netGame named one local user AND it cross-checked
        UserIndex slot{0xFFFFu};  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    };
    virtual LocalUser LocalUserIndex(UserIndex& out) const { (void)out; return LocalUser::NotAvailable; }
    virtual LocalUser ReadLocalUser(LocalUserReading& out) const {
        out = LocalUserReading();
        return out.state;
    }
    virtual uint32_t  ReadLocalPlayer(UserIndex, PlayerState&) const { return 0; }

    // WHICH SOURCE ANSWERED FOR THE LOCAL PAWN'S ACTOR, and which arm refused when none did.
    // L2 recorded 976 ticks of `fill == 0` and neither the log nor the file could say more than
    // that: a four-state question -- no pawn / no actor / an actor carrying no component / an
    // actor whose accessors all declined -- answered by a one-bit mask. These make the arm
    // decidable at the caller, so "the capture has no player state" becomes a named cause.
    enum class ActorSource : uint8_t {
        None = 0,          // neither source produced an actor
        BroadcasterKeys,   // the pool's parallel SActorGUID array, LOCAL.BROADCASTER_CS_KEYS_OFF
        NetGameUserActor,  // CR15NetGame::UserActor, which answers for ANY user slot
    };
    enum class LocalRefusal : uint8_t {
        NotAsked = 0,      // this IEntity predates the detailed reading and cannot say
        Ok,                // at least one group was sourced
        NoBroadcaster,     // the local-pawn half of the bind never reached a broadcaster system
        SlotNotInPool,     // no broadcaster element carries this user slot
        ActorNull,         // BOTH actor sources refused -- the return at entity.cpp's `!actor`
        NoSourceAnswered,  // an actor resolved and every accessor group still declined
    };
    struct LocalPlayerReading {
        uint32_t     fill = 0;
        ActorSource  actorSource = ActorSource::None;
        LocalRefusal refusal = LocalRefusal::NotAsked;
        // Both sources answered and named DIFFERENT actors. Not a refusal -- the keys array wins,
        // because it is the array the engine's own broadcast loop indexes -- but it says one of
        // the two is describing a different player, which no mask could.
        bool         actorSourcesDisagree = false;
        bool         heldByFn = false;      // LOCAL.HELD_PLAYER resolved on this build
        // Asked as a bool rather than compared against the engine's no-component word: the
        // handle is meaningful only when the actor carried the component at all.
        bool         echoComponent = false;
        uint16_t     echoHandle = 0;
        const void*  bcastCs = nullptr;
        const void*  bcastKeys = nullptr;   // the deref that was null through the whole of L2
        const void*  netGame = nullptr;
    };
    // The same read as ReadLocalPlayer, plus the reason. The default forwards, so an IEntity that
    // predates it still compiles and answers NotAsked rather than inventing a cause.
    virtual uint32_t ReadLocalPlayerDetailed(UserIndex u, PlayerState& s,
                                             LocalPlayerReading& r) const {
        r = LocalPlayerReading();
        r.fill = ReadLocalPlayer(u, s);
        return r.fill;
    }

    // ---- visibility ----------------------------------------------------------------
    // Was this player DRAWN, or did it merely not move?  Those two states look identical in a
    // screenshot and lead to opposite conclusions about a launch, and nothing else in B.4 can tell
    // them apart.  CR15NetSpectatorCameraCS::EnableRemoteHeads and EnableRemoteBodies both drive
    // CModelCS::SetVisible, so visibility is a CModelCS field and this reads it.
    //
    // Works for ANY user slot, local or remote, because the actor comes from CR15NetGame::UserActor.
    // Returns false when the slot, its actor or its model component does not resolve -- which is a
    // different answer from "resolved, and not visible", and the caller gets to tell them apart.
    virtual bool ModelVisible(UserIndex, bool& visible) const {
        visible = false;
        return false;
    }

    // The RAW ncaModel component handle for a slot -- ComponentOnActor(model_.cs, actor), the number
    // ModelVisible's presence test keys on, before it is collapsed to a bool.  A run that logs this
    // for the local viewer (index 0) proves the reader by construction: a value other than
    // LOCAL.NO_COMPONENT (0xFFFF) for a slot that renders means the model system, the actor and the
    // component lookup all resolved.  0xFFFF means either the component is genuinely absent or the
    // reader could not source it -- ModelSystemResolved() separates those two.
    virtual uint16_t ModelComponentHandle(UserIndex) const {
        return 0xFFFFu;  // facts-ok: LOCAL.NO_COMPONENT the component-absent sentinel
    }

    // Whether the ncaModel component system itself resolved (model_.cs != null), independent of any
    // actor.  ModelVisible returns false BOTH when the system is unresolved and when a resolved
    // system carries no component on the actor; this splits the two so a null system is never
    // reported as an absent component (entity.cpp:2498 vs 2507).
    virtual bool ModelSystemResolved() const { return false; }

    // ---- the wrist thrusters -----------------------------------------------------------------
    //
    // ★ THE HANDLE TRAP. `PlayerTrack::handle` is a CR15RemotePlayerCS handle; the thruster
    // controller lives on the player's **EchoUnit** element, whose handle comes from a different
    // pool and does not convert (§2 F11, the same rule PhysOf exists for). Resolving it costs the
    // engine's own actor lookup plus a component scan, so the caller resolves ONCE PER BIND and
    // caches; the value is invalidated by a re-bind, never carried across one.
    //
    // 0xFFFF means the slot, its actor or its EchoUnit component did not resolve.
    virtual uint16_t EchoUnitComponentHandle(UserIndex) const {
        return 0xFFFFu;  // facts-ok: LOCAL.NO_COMPONENT the component-absent sentinel
    }

    // Why a bind answered what it answered. `EchoUnitComponentHandle` collapses six distinct
    // failures into one 0xFFFF, and three sessions have now read that 0xFFFF as "the component is
    // absent" when it could equally have been a null component system or a user record the join
    // path has not published an actor for yet. Each field below is one link of the chain, in the
    // order the chain is walked.
    struct EchoBind {
        bool system = false;      // the CR15EchoUnitCS resolved
        bool netGame = false;     // the broadcaster's net game resolved
        // The actor the handles below were read off, and where it came from. A replayed pawn has
        // TWO candidate actors and only one of them carries components: see `ActorSource`.
        ActorSource actorSource = ActorSource::None;
        uint64_t actorLo = 0;     // SActorGUID from CR15NetGame::UserActor -- zero means no actor
        uint64_t actorHi = 0;
        uint64_t keysLo = 0;      // the same slot's GUID out of the broadcaster's key array
        uint64_t keysHi = 0;
        bool haveKeys = false;
        bool actorsDisagree = false;
        uint16_t keysEchoHandle = 0xFFFFu;  // facts-ok: LOCAL.NO_COMPONENT the component-absent sentinel
        uint16_t echoHandle = 0xFFFFu;   // facts-ok: LOCAL.NO_COMPONENT the component-absent sentinel
        uint16_t modelHandle = 0xFFFFu;  // facts-ok: LOCAL.NO_COMPONENT the component-absent sentinel
        uint16_t bodyHandle = 0xFFFFu;   // facts-ok: LOCAL.NO_COMPONENT the component-absent sentinel
        // Elements in the EchoUnit system's own actor index (cs+0x118). One says only the local
        // pawn has an EchoUnit; nine says the replayed pawns have one and this lookup is wrong.
        uint32_t echoPopulation = 0;
    };
    // The default answers out of `EchoUnitComponentHandle`, so an implementation that overrides
    // only the handle -- every test double, and any future entity that has nothing to say about
    // the chain -- keeps binding exactly as it did. Only the diagnosis is lost, never the bind.
    virtual bool ProbeEchoUnitBind(UserIndex u, EchoBind& out) const {
        out = EchoBind();
        out.echoHandle = EchoUnitComponentHandle(u);
        return out.echoHandle != 0xFFFFu;  // facts-ok: LOCAL.NO_COMPONENT the component-absent sentinel
    }

    // ---- the actor-index census (ONE-SHOT DIAGNOSTIC, never a per-frame path) -----------------
    //
    // PRESENT-F measured every replayed pawn's actor carrying no EchoUnit while 22 EchoUnit
    // components existed in the session, and could not say WHOSE those 22 were. Each component
    // system keeps the sorted {SActorGUID, SComponentID} table the engine's own
    // FirstActorComponent binary-searches (cs+0x110, count at cs+0x118, 24-byte stride); walking it
    // names every owner. A row is the engine's own answer to "which actors carry this component",
    // which is the only thing that separates "the pawns are unprovisioned" from "the bind resolves
    // the wrong actor".
    enum class CensusPool : uint8_t { EchoUnit = 0, NcaModel = 1, Body = 2 };
    struct CensusRow {
        uint64_t actorLo = 0;
        uint64_t actorHi = 0;
        uint16_t component = 0xFFFFu;  // facts-ok: LOCAL.NO_COMPONENT the component-absent sentinel
        CensusPool pool = CensusPool::EchoUnit;
        bool havePos = false;          // an EchoUnit row whose BaseTransform could be read
        float pos[3] = {0.f, 0.f, 0.f};
    };
    // Fills `out` with up to `cap` rows across all three pools and returns how many were written.
    virtual uint32_t ComponentCensus(CensusRow* /*out*/, uint32_t /*cap*/) const { return 0; }

    // ---- the pooled-actor census (ONE-SHOT DIAGNOSTIC, never a per-frame path) -----------------
    //
    // Every actor class the level defines owns a fixed pool. CncaGameSpace::AllocateActor
    // (`sub_1404E61E0`) allocates out of the descriptor at `*(u64*)(space + 1000) + 136*classId`
    // and takes the allocating branch only while `capacity - free < capacity`, i.e. only while
    // `free > 0`. With none free it returns the INVALID guid `{-1, 0xFFFFFFFFFFFF}` and does not
    // say so; `sub_1401B0D80` stores that into the user record and hands it straight to
    // `sub_140CB89B0`, which cannot resolve it, falls through to `sub_140D47440(nullptr, ...)` and
    // reads `[0 + 128]`. That is an unrecoverable engine-side null deref, so `free` is the one
    // number an admission has to read BEFORE it asks the engine for an actor.
    struct ActorPoolRow {
        uint16_t classId  = 0xFFFFu;  // facts-ok: LOCAL.NO_COMPONENT this row's own all-ones u16 "none"
        uint64_t prefabId = 0;        // the class's CSymbol64, as the level's name table holds it
        uint32_t capacity = 0;
        uint32_t freeSlots = 0;
        // The entry the NEXT allocation out of this pool will take, and the generation the engine
        // will stamp into the guid for it. `nextIndex == 0xFFFF` means the pool is empty or the
        // free stack did not read back. A generation above zero is a RECYCLED entry: one a previous
        // session held and released.
        uint32_t nextIndex = 0xFFFFu;   // facts-ok: LOCAL.NO_COMPONENT this row's own all-ones u16 "none"
        uint32_t nextGen = 0;
    };
    // Every class whose pool descriptor and name both read back sanely, in class-id order.
    virtual uint32_t ActorPoolCensus(ActorPoolRow* /*out*/, uint32_t /*cap*/) const { return 0; }
    // The one class whose prefab name matches, and whether it was found at all.
    virtual bool ActorPoolFor(uint64_t /*prefabId*/, ActorPoolRow& /*out*/) const { return false; }

    // ---- P0 readings 3-6 (ONE-SHOT DIAGNOSTIC, never a per-frame path) ------------------------
    //
    // DESIGN-REMOTE-ACTOR settled that the replayed pawns ARE `remoteplayer_arena` and that the
    // delegate feed reaches the engine, so the remaining sound causes are all downstream reads:
    // does the raise resolve to the player's own actor (A-a), does that actor's class carry the
    // components whose scripts would listen (A-b), and is the CD in the enabled partition at all
    // (A-d). These three surfaces answer one each, and the disc one answers gate (ii).

    // Reading 3. One row per component system the level registered, with how many rows of its
    // actor index belong to each class the caller asks about. A class is an actor id's LOW WORD.
    struct SystemRow {
        static constexpr uint32_t kClasses = 4;
        uint64_t symbol = 0;
        uint32_t rows = 0;                  // rows in this system's actor index, any actor
        uint32_t perClass[kClasses] = {};   // ...and how many of them carry each queried class
    };
    virtual uint32_t SystemCensus(const uint16_t* /*classes*/, uint32_t /*nClasses*/,
                                  SystemRow* /*out*/, uint32_t /*cap*/) const { return 0; }

    // Readings 4 and 5. The actor the engine itself resolves for the compact handle the delegate
    // emit builds, beside the partition the CD sits in and the history head that must stay NULL.
    struct RemoteTrackProbe {
        const char* refused = "this entity surface has no remote-player probe";
        uint32_t compact = 0;
        uint16_t cdIndex = 0xFFFFu;    // facts-ok: LOCAL.NO_COMPONENT the component-absent sentinel
        uint16_t totalCds = 0;
        uint16_t enabledCds = 0;
        uint64_t targetLo = 0, targetHi = 0;
        bool     targetRead = false;   // the vtable resolve ran and its result read back
        bool     historyRead = false;  // CD+664 was readable...
        bool     historyEmpty = true;  // ...and its head was NULL, as section 1.4 requires
        // P1C M0. The geo-attach delegates are the only ones in the table with a second engine
        // gate: sub_140CCE580 raises bits 5 and 6 only while the matching hand field equals the
        // "holding nothing" sentinel BOTH before and after the diff. No instruction in the image
        // writes that sentinel, so its value can only be read at runtime -- and without it the
        // 282 counted bit-5/6 transitions say nothing about what the engine did with them.
        uint32_t bits28 = 0, holdL = 0, holdR = 0;
        bool     actionRead = false;
        uint32_t holdSentinel = 0;     // dword_141FFCEA0, rebased
        bool     sentinelRead = false;
    };
    virtual bool RemoteTrackProbe_(UserIndex, RemoteTrackProbe& out) const {
        out = RemoteTrackProbe();
        return false;
    }

    // Reading 6. The disc's untouched gate, read where CR15FrisbeeCS::Update reads it.
    struct DiscRing {
        const char* refused = "this entity surface has no disc binder";
        float    timeSinceLastHeld = 0.f;
        // CR15FrisbeeCS::Update's dense walk is bounded by the ENABLED prefix, so zero here means
        // the timer is frozen rather than never reset -- the two look identical when sampled.
        uint16_t enabled = 0;
        uint16_t touchHandle = 0xFFFFu;  // facts-ok: LOCAL.NO_COMPONENT the component-absent sentinel
        bool     touchRead = false;
        uint32_t holderA = 0, holderB = 0;
        uint16_t flags = 0;
        bool     askParent = false;
        // What sub_14075FD80 would answer from its first gate alone: a reset this frame or not.
        bool     heldByOwnCi = false;
    };
    virtual bool ReadDiscRing(DiscRing& out) const {
        out = DiscRing();
        return false;
    }

    // ---- P1C. The subscriber side of the action-word delegates (ONE-SHOT DIAGNOSTIC) ----------
    //
    // Both foley graphs on `remoteplayer_arena` -- f39961be341aabb7 (hands, wrist thrusters) and
    // acd35bdc14481483 (boost, brake, backpack booster) -- free the thread they just started for
    // every delegate whose `event.actor` is not the graph's own `self` variable, and that is the
    // ONLY per-event condition either of them tests. So the question the silence turns on is what
    // `self` holds on a replayed pawn's graph, against the identity the engine's own resolve puts
    // into the event. The disc's script (38965d90a823f03f) gates its `bubble` toggle the same way.
    //
    // The read is the script module's own `get` binding at module+240, whose implementation
    // switches on the type hash and then on the name hash and writes NOTHING when neither matches,
    // returning 0 -- so asking every ncaScript graph for `self` is safe on the graphs that have no
    // such variable. It is a pure copy out of EXECUTION_DATA; nothing in the graph is written.
    struct ScriptSelfRow {
        uint64_t actorLo = 0;        // the actor-index row's SActorGUID, both halves as stored
        uint64_t actorHi = 0;
        uint16_t ciIndex = 0xFFFFu;  // facts-ok: LOCAL.NO_COMPONENT the component-absent sentinel
        uint64_t scriptHash = 0;     // *(u64*)(module+56): the /scripts/%016llx the loader opened
        int32_t  state = -1;         // *(int*)(inst+224); 5 is what a checkpointed graph holds
        bool     answered = false;   // the graph's `get` returned 1 for {self, actor}
        uint64_t selfSpace = 0;      // actor_var_t.gamespace
        uint64_t selfActor = 0;      // {idx, poolidx, genidx} -- the qword the event carries at +16
        uint64_t selfNode = 0;       // actor_var_t.nodeid
        // EXECUTION_GRAPH::gametime. The per-delegate sentinels below are cleared by a thread that
        // exits, and a DELAY-bearing thread exits on this clock -- so a clock wound backwards by a
        // reverse scrub is the one thing that could jam them, and it has to be read beside them.
        int64_t  graphTime = 0;
        // The head of EXECUTION_DATA, which for these graphs is the per-delegate re-entrancy
        // sentinel block: `event_handler` drops an event whose sentinel is already >= 1 before it
        // looks at `self`. The layout is per-script, so the plugin dumps the words and the script
        // DB names them rather than this file carrying 24 offsets it cannot check.
        static constexpr uint32_t kDataWords = 26;
        uint32_t data[kDataWords] = {};
        bool     dataRead = false;

        // ---- RE-DISC-RING-3 §6's read R1, filled only for the disc's own script ----------------
        // The same `get` binding, asked for the rest of the graph's own variables rather than only
        // `self`. Together they say which of the four readings in that packet's table this disc is:
        // `bubble` unbound (the ring cannot be it), `bubble` bound with is_local_disc clear (HIDE-A
        // is unreachable by construction), is_local_disc set (we bound the WRONG disc), or a graph
        // that never initialised. A pure read -- script_get_variable only copies out of
        // EXECUTION_DATA -- so it is safe at any transport state and on any frame.
        bool     discRead = false;       // this row IS the disc script and the reads were attempted
        bool     bubbleAnswered = false;
        uint64_t bubbleSpace = 0, bubbleActor = 0, bubbleNode = 0;
        // Six flag variables, as a pair of masks rather than twelve fields: bit N of `flagsAnswered`
        // says the getter recognised kDiscFlagNames[N], bit N of `flagsValue` is what it wrote.
        // A value bit is meaningless with its answered bit clear, which is why they are separate.
        uint8_t  flagsAnswered = 0;
        uint8_t  flagsValue = 0;
        bool     modelComponentAnswered = false;
        uint64_t modelComponent = 0;     // the symbol the three ENABLEMODELSCENESETNODEs act on (C1)
    };
    // The order of the bits in ScriptSelfRow::flagsAnswered / flagsValue, and the names the log
    // prints for them. Declared beside the row so the reader and the printer cannot drift.
    static constexpr uint32_t kDiscFlagCount = 6;
    static const char* const kDiscFlagNames[kDiscFlagCount];
    // Every distinct script the walk met, so a graph that is simply ABSENT from a replayed pawn is
    // distinguishable from one that is present with an unbound `self`.
    struct ScriptHashRow { uint64_t hash = 0; uint32_t instances = 0; uint32_t withSelf = 0; };
    struct ScriptCensus {
        // The space to walk; null means the one the pawns were bound in. The disc lives in a
        // different game space from the pawns on this host, and a walk of the pawns' space reports
        // an absence it never looked for (DISC-RING-2).
        void*           space = nullptr;
        // Detail rows are written only for these script hashes; empty means every script that
        // answers `self`, which on a full level is hundreds of lines.
        const uint64_t* wanted = nullptr;
        uint32_t        nWanted = 0;
        ScriptSelfRow*  rows = nullptr;
        uint32_t        rowCap = 0;
        uint32_t        rowsWritten = 0;
        ScriptHashRow*  hashes = nullptr;
        uint32_t        hashCap = 0;
        uint32_t        hashesWritten = 0;
        uint32_t        walked = 0;      // ncaScript component instances visited
        uint32_t        unreadable = 0;  // ...of which some pointer on the way did not read back
        const char*     refused = "this entity surface has no script census";
    };
    virtual bool ScriptSelfCensus(ScriptCensus& io) const { (void)io; return false; }

    // DESIGN-HOST-SYNC P0. One script graph, reached by the census walk and then spoken to through
    // the graph's OWN module bindings: `get` (module+240), `set` (module+248) and `sendevent`
    // (module+512), the three the engine's own wrappers call (scriptgraph_facts.h). Game thread only;
    // every call is SEH-guarded, and a ref is re-validated before it is spent.
    struct ScriptGraphRef {
        void* inst = nullptr;
        void* graph = nullptr;
        void* module = nullptr;
        uint64_t hash = 0;
        uint64_t actorLo = 0, actorHi = 0;   // the actor-index row the graph was found on
        uint8_t self[24] = {};               // the graph's own `self` actor_var, as `get` wrote it
        bool selfValid = false;
    };
    // The first graph with module hash `hash` in `space` (null = the bound game space). A non-zero
    // actorLo|actorHi also requires the row's actor to match. The actor is the row's first qword;
    // the second is the owning system's symbol (DiscRingReading::Actor::rowSymbol), so a zero
    // actorHi matches the actor on any system's row. `why` names the refusal.
    virtual bool FindScriptGraph(void* space, uint64_t hash, uint64_t actorLo, uint64_t actorHi,
                                 ScriptGraphRef& out, const char** why) {
        (void)space, (void)hash, (void)actorLo, (void)actorHi, (void)out;
        if (why) *why = "this entity surface has no script-graph access";
        return false;
    }
    // The module still names `r.hash` and the graph's EXECUTION_DATA is non-null.
    virtual bool ScriptGraphValid(const ScriptGraphRef& r) { (void)r; return false; }
    virtual bool ScriptGetReal(const ScriptGraphRef& r, uint64_t name, float& out) {
        (void)r, (void)name, (void)out;
        return false;
    }
    // True only when the setter answered 1. It is the caller's job to read the value back.
    virtual bool ScriptSetReal(const ScriptGraphRef& r, uint64_t name, uint64_t id, float v) {
        (void)r, (void)name, (void)id, (void)v;
        return false;
    }
    // *(int32*)(data + 4*word).
    virtual bool ScriptDataWord(const ScriptGraphRef& r, uint32_t word, int32_t& out) {
        (void)r, (void)word, (void)out;
        return false;
    }
    // Delivers {name, actor = self, actorname = self.nodeid, component, userdata = -1} to this graph
    // alone -- byte for byte the event the graph builds when it delivers to itself. Refuses when
    // `self` was not read.
    virtual bool ScriptSendEvent(const ScriptGraphRef& r, uint64_t name, uint64_t component) {
        (void)r, (void)name, (void)component;
        return false;
    }

    // ★ THE READ THAT DECIDES ITEM C. The owner hears player foley at any distance, with falloff
    // and distant reverb but no cutoff to silence. Wwise attenuates against the LISTENER set, and
    // csoundplayer.cpp hands `AK::SoundEngine::SetDefaultListeners` the WHOLE of its listener
    // table on every create (sub_140206990) -- so a second listener parked anywhere in the arena
    // keeps every emitter audible no matter where the camera is, which is exactly that symptom.
    // The table is therefore the measurement: how many listeners there are, and where each is.
    //
    // Read-only, one-shot, console-driven: the table is the engine's own cache of what it last
    // passed to AK::SoundEngine::SetPosition (sub_14020E560 writes the record and then calls it),
    // so the positions below are what Wwise holds, not a second guess at them.
    struct AudioListener {
        uint32_t id = 0;
        float    quat[4] = {};   // AUDIO.LISTENER_QUAT_OFF
        float    pos[3] = {};    // AUDIO.LISTENER_POS_OFF, engine world space
    };
    // The camera/view registry beside it: sub_140928E50 gives every camera-audio component BOTH a
    // listener and a view entry, and AUDIO.ACTIVE_VIEW_OFF names the one the mix treats as current.
    // A spectator camera that registered a listener the plugin never moves shows up as a view id
    // whose listener sits at a pose the camera left long ago.
    struct AudioView {
        uint32_t id = 0;
        float    pos[3] = {};    // AUDIO.VIEW_POS_OFF
    };
    // ENG-P1E item 2. The OTHER side of the attenuation: every game object a sound can be posted
    // on. sub_14020BD80 binary-searches AUDIO.EMITTER_MAP_OFF for the id the play request names and
    // posts only on a record whose AUDIO.EMITTER_FLAGS_OFF has bit 0x20; sub_14020EA80 caches the
    // transform it last handed AK::SoundEngine::SetPosition in the same record. So `pos` below is
    // literally where Wwise believes that emitter is -- an emitter at the origin, or at a pose the
    // pawn left long ago, is a sound with no distance to the listener to attenuate over.
    struct AudioEmitter {
        uint32_t id = 0;        // AUDIO.EMITTER_ID_OFF, the AkGameObjectID PostEvent is given
        uint32_t flags = 0;     // AUDIO.EMITTER_FLAGS_OFF, bit 0x20 = postable
        float    pos[3] = {};   // AUDIO.EMITTER_POS_OFF, engine world space
        // FAR-SOUND-2: the same emitter as WWISE holds it, filled only for emitters carrying a
        // voice. `pos` above is the game's cache of what it last sent; these are the read-back.
        float    wwise[9] = {};      // AkTransform, all three vectors -- see kAkTransformFloats
        float    dryLevel = 0.0f;    // Query::GetGameObjectDryLevelValue against listener 0
        uint32_t auxCount = 0;       // sends Wwise reports, before the cap below
        uint32_t auxBus[4] = {};     // AkAuxSendValue::auxBusID
        float    auxValue[4] = {};   // AkAuxSendValue::fControlValue
        bool     wwiseRead = false;
        bool     dryRead = false;
        bool     auxRead = false;
    };
    // What WWISE holds, as opposed to what the game holds. Every number above is read out of the
    // engine's own cache of what it last sent; these two are read back out of the sound engine
    // through its read-only query API, because the set of listeners an emitter is attenuated
    // against is fed by SetDefaultListeners and is not mirrored anywhere on the game side. A
    // listener nobody moves, still in that set, keeps every emitter audible from any camera.
    struct AudioListenerSet {
        uint32_t emitters = 0;   // how many emitters Wwise reports this same set for
        uint32_t size = 0;       // the set size Wwise reports, which may exceed what is stored
        static constexpr uint32_t kIdCap = 4;
        uint64_t ids[kIdCap] = {};
    };
    struct AudioWwiseListener {
        uint64_t id = 0;
        float    pos[3] = {};    // AkTransform::Position, as Wwise holds it
        bool     read = false;
    };
    // FINAL-FIX-1 item 1(c). Every link up to here says where Wwise thinks the sound IS. This one
    // says what the sound is ALLOWED to do over that distance, read out of the bank through
    // Query::GetPositioningInfo: the curve's last point. `maxDistance` is where the curve ends and
    // `volDryAtMax` / `volAuxGameDefAtMax` are the dry and game-defined-reverb levels it ends AT.
    // A curve whose dry end is silent but whose reverb end is not is exactly "super far away echoy
    // sounding instead of being silent"; a curve that ends past the arena's 121 m never reaches its
    // end at all. Neither is inferable from a position.
    struct AudioPlayingSound {
        uint32_t emitterId = 0;
        uint32_t playingId = 0;
        uint32_t eventId = 0;        // Query::GetEventIDFromPlayingID, the bank's event hash
        uint32_t objId = 0;          // the bank object the event references, via QueryAudioObjectIDs
        uint32_t objCount = 0;       // how many objects the event references (0 = the query refused)
        float    maxRadius = -1.0f;  // Query::GetMaxRadius(gameObject), <0 when nothing plays on it
        float    emitterPos[3] = {};
        float    maxDistance = 0.0f;         // AkPositioningInfo +44
        float    volDryAtMax = 0.0f;         // +48, dB
        float    volAuxGameDefAtMax = 0.0f;  // +52, dB
        float    volAuxUserDefAtMax = 0.0f;  // +56, dB
        bool     attenuation = false;        // +20, bEnableAttenuation
        bool     info = false;               // GetPositioningInfo answered AK_Success
        // FAR-SOUND-2. The block VERBATIM. Every named offset above was chosen before any call had
        // ever succeeded, so the names are a hypothesis and this is the evidence: GetMaxRadius
        // already says what the max distance must be, and a census that prints the raw floats lets
        // that known value locate its own field instead of being asserted into one.
        uint8_t  raw[68] = {};
        // AUDIO-FIX-2 C.2. The slice of `objNodes` below that belongs to this voice. FAR-SOUND-2
        // read ONE node per voice -- "the first attenuated child" -- and RE-AUDIO-CUTOFF R2 is the
        // observation that for a switch or state container "first" is an arbitrary child, so a
        // -19 dB floor read off it is a floor nobody can say plays. The whole sub-tree is recorded
        // instead, with each node's parent and depth, so the tree is read rather than guessed.
        uint32_t nodeFirst = 0;
        uint32_t nodeCount = 0;
    };
    // One row per object the event's sub-tree names, for one voice. `parentId`/`depth` come from
    // AkObjectInfo itself, so the curve that is read can be placed in the tree it came from.
    struct AudioObjNode {
        uint32_t objId = 0;
        uint32_t parentId = 0;
        int32_t  depth = 0;
        float    maxDistance = 0.0f;
        float    volDryAtMax = 0.0f;
        float    volAuxGameDefAtMax = 0.0f;
        float    volAuxUserDefAtMax = 0.0f;
        bool     attenuation = false;
        bool     info = false;      // GetPositioningInfo answered for this node
    };
    struct AudioMix {
        static constexpr uint32_t kListenerCap = 8;   // csoundplayer.cpp refuses past 6
        static constexpr uint32_t kViewCap = 8;
        static constexpr uint32_t kSetCap = 8;
        // Big, because the row that answers the question is one of the level's hundreds and the map
        // is keyed by id, not by owner: there is no prefix of it that is known to contain a pawn.
        static constexpr uint32_t kEmitterCap = 512;
        uint64_t      system = 0;          // the CSoundPlayer the whole read hangs off
        uint64_t      listenerCount = 0;   // AUDIO.LISTENER_COUNT_OFF, as the engine holds it
        uint32_t      listenersWritten = 0;
        AudioListener listeners[kListenerCap];
        uint32_t      spatializedId = 0;   // AUDIO.SPATIALIZED_LISTENER_OFF
        uint32_t      activeViewId = 0;    // AUDIO.ACTIVE_VIEW_OFF
        uint64_t      viewCount = 0;       // AUDIO.VIEW_COUNT_OFF
        uint32_t      viewsWritten = 0;
        AudioView     views[kViewCap];
        uint64_t      emitterCount = 0;    // AUDIO.EMITTER_MAP_COUNT_OFF, as the engine holds it
        uint32_t      emittersWritten = 0;
        AudioEmitter  emitters[kEmitterCap];
        // Filled only when the two query entry points resolved; `wwiseRefused` says why not.
        uint32_t         setsWritten = 0;
        AudioListenerSet sets[kSetCap];
        uint32_t         wwiseListenersWritten = 0;
        AudioWwiseListener wwiseListeners[kSetCap];
        static constexpr uint32_t kPlayingCap = 64;
        uint32_t         playingWritten = 0;
        uint32_t         playingSeen = 0;      // before the cap, so a full table says so
        uint32_t         emittersActive = 0;   // emitters with at least one voice on them
        AudioPlayingSound playing[kPlayingCap];
        // Eight objects per voice is the query's own cap here, so a full playing table cannot
        // overrun this one; `objNodesSeen` counts what the events named before the cap bit.
        static constexpr uint32_t kObjNodeCap = 256;
        uint32_t         objNodesWritten = 0;
        uint32_t         objNodesSeen = 0;
        AudioObjNode     objNodes[kObjNodeCap];
        // ---- ENG-LIVE-CENSUS-2 item 2: how close to its walls a session is running ----------
        // Emitters bound to the 2D listener (game object 7), counted over the WHOLE map rather
        // than over the capped `emitters` array: the count is the question, the rows are not.
        uint32_t listener2dEmitters = 0;
        // The sound pool, as CSoundPlayer::Init built it: a free stack, so in-use is cap - free.
        // The engine refuses a post the moment free hits 0 and counts that refusal nowhere.
        bool     poolRead = false;
        uint64_t soundPoolCap = 0;
        uint64_t soundPoolFree = 0;
        uint64_t soundsActive = 0;     // the live one-shot list's own count
        uint64_t emitterPoolFree = 0;
        // Wwise's voice accounting. `maxVoices` is the configured cap; `voicesLive` is the pair of
        // counters its admission check subtracts. `volumeThresholdDb` is the dB floor below which
        // a voice goes virtual -- the other half of "why did that voice not sound".
        bool     limitsRead = false;
        uint32_t maxVoices = 0;
        uint32_t voiceHigh = 0, voiceLow = 0;
        float    volumeThresholdDb = 0.0f;
        const char*      playingRefused = "the Wwise playing-sound queries were not asked for";
        const char*   wwiseRefused = "the Wwise query entry points were not asked for";
        const char*   refused = "this entity surface has no audio census";
    };

    // DISC-RING-1. CR15NetTouchInteractCS::Update (sub_140D70F40) is the sole raiser of
    // `delegate_OnNetTouchGrab` / `OnNetTouchRelease` / `OnNetTouchResetOnIdle`, the three events
    // the disc's script gates its `bubble` (the "untouched" ring) on. It runs only while the
    // netgame state word reads 9, and per component instance it compares the holder the engine
    // reads for that body against a LATCH bit it keeps in the CI, raising an edge when the two
    // disagree. So the latch is the engine's own witness: with a holder published and the latch
    // still clear, the update is not seeing it; with the latch set, the grab edge WAS raised and
    // whatever is left is downstream of the delegate. `lastOwner` is rewritten from the body's
    // owner dword on every pass of the loop, so it also says whether the loop reached this CI.
    struct NetTouchRow {
        uint16_t ciIndex   = 0;
        uint16_t entity    = 0;      // CI+0, the actor handle the component was built for
        uint32_t physId    = 0;      // CI+8, the R14NetPhysics id -- the body this touch state is for
        uint32_t touchId   = 0;      // CI+12, the R15TouchInteract id
        uint32_t lastOwner = 0;      // CI+48, the owner dword as the last update pass saw it
        uint8_t  flags     = 0;      // CI+52
        bool     heldLatch = false;  // CI+52 & 4: the update's "this body was held last pass"
        bool     physRead  = false;  // the R14NetPhysics element for physId read back
        uint32_t physOwner = 0;      // that element's PHYS_CI_OWNER_OFF, what the gates read
        uint16_t physFlags = 0;      // that element's PHYS_CI_FLAGS_OFF

        // sub_140CEDEB0's own chain, walked through the NET-TOUCH system's sibling pointers rather
        // than through the pool this plugin bound. The two can name different elements, and the
        // engine obeys its own; a row where they disagree is the whole answer.
        bool     engSameCi   = false;  // *(cs+368)'s map resolves physId to the element above
        uint32_t engOwner    = 0;      // that element's owner dword, as the update reads it
        bool     nodeRead    = false;  // PHYS_CI_LIST_HEAD_OFF -> +16 -> PHYS_NODE_STATE_OFF
        uint16_t nodeState   = 0;      // the word whose 0x200 is the remote arm's last term
        bool     touchCsRead = false;  // *(cs+376) is a readable R15TouchInteract system
        uint16_t touchIndex  = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE the "no such component" answer of a forward handle map, not the unset-slot field

        uint16_t touchCount  = 0;      // its live element count, the bound that index must clear
        bool     touchLive   = false;  // index within the bound: the IF arm's second gate
        uint32_t touchHolderA = 0, touchHolderB = 0;  // TOUCH_CI_HOLDER_A/B_OFF
        uint32_t touchHand   = 0;      // TOUCH_CI_HAND_OFF, what AttachHand last wrote
        uint16_t touchFlags  = 0;      // TOUCH_CI_FLAGS_OFF
    };
    struct NetTouchReading {
        static constexpr uint32_t kRowCap = 32;
        // The space to walk; null means the one the pawns were bound in. See ScriptCensus::space.
        void*       space = nullptr;
        bool        read = false;
        uint64_t    cs = 0;
        uint64_t    csPhys = 0;   // *(cs+368): the R14NetPhysics system the holder resolve uses
        uint64_t    csTouch = 0;  // *(cs+376): the R15TouchInteract system its second gate uses
        uint32_t    ciCount = 0;
        uint32_t    rowsWritten = 0;
        // The positive control on the scan itself: how many systems the space published, and
        // whether the same pass found a symbol the plugin already binds a system by. Without it a
        // refusal cannot tell "this level has no net-touch system" from "the walk read nothing".
        uint32_t    systemsSeen = 0;
        bool        controlFound = false;
        bool        netGameRead = false;
        uint32_t    netGameState = 0;   // 9 = in match, the only state the update runs in
        uint16_t    bandLo = 0;
        uint16_t    bandHi = 0;
        NetTouchRow rows[kRowCap];
        const char* refused = "this entity surface has no net-touch census";
    };
    virtual bool NetTouchCensus(NetTouchReading& out) const { (void)out; return false; }

    // DISC-RING-2. One read that says WHICH disc actor the track drives, what that actor carries,
    // and where the scripted disc is if it is not that one. RING-1 walked the space the PAWNS live
    // in; the disc is bound in whatever space the frisbee system was found in, and those need not
    // be the same space -- a script census that walks the wrong table reports an absence it never
    // looked for. Every row below is read in the DISC's space.
    struct DiscRingReading {
        static constexpr uint32_t kActorCap  = 8;   // actors carrying R14Frisbee, plus the bound one
        // RE-DISC-RING-3 C3: the disc carries TEN ncaScript components and nine were never
        // identified. Four rows could not have shown that, so a cap of four was itself the finding
        // being missed.
        static constexpr uint32_t kScriptCap = 12;  // script hashes recorded per actor
        // The wanted-component bits, in the order RE-DISC-RING-2 §5 asks for them.
        enum Has : uint32_t {
            kFrisbee     = 1u << 0,
            kTouch       = 1u << 1,   // R14TouchInteract, the NON-net component the ring's arm gates on
            kNcaPhysics  = 1u << 2,
            kNcaScript   = 1u << 3,
            kNetPhysics  = 1u << 4,
            kNetTouch    = 1u << 5,
            kTrail       = 1u << 6,
        };
        struct Actor {
            uint64_t actorId = 0;    // the actor-index row's first qword: {slot, pool, gen, pad}
            uint64_t rowSymbol = 0;  // its second, which is the owning SYSTEM's own CSymbol64 --
                                     // not a game space, and not part of the actor's identity
            uint32_t physId = 0;     // SR15FrisbeeCI+0x18, the net-physics component id
            uint16_t frisbeeCi = 0xFFFFu;  // facts-ok: LOCAL.NO_COMPONENT the component-absent sentinel
            float    timeSinceHeld = 0.f;  // SR15FrisbeeCI+120, the untouched timer
            bool     bound = false;  // this is the actor whose body the disc track drives
            uint32_t systems = 0;    // how many of the space's systems name this actor
            uint32_t has = 0;        // bitmask over Has
            uint32_t scripts = 0;    // ncaScript components on it
            uint64_t scriptHash[kScriptCap] = {};
        };
        // An instance of the disc's own script, wherever it sits.
        struct ScriptSite { uint64_t actorId = 0, space = 0; uint16_t ciIndex = 0xFFFFu; int32_t state = -1; };  // facts-ok: LOCAL.NO_COMPONENT the component-absent sentinel

        // One space's component-system table, walked. EVERY space the disc bind could have meant
        // gets a row -- the bind's own, the pawns', and each early candidate root with its parent
        // chain -- because RING-1's `0 instances` was measured in the pawns' space alone.
        struct SpaceScan {
            uint64_t space = 0;
            uint64_t frisbeeCs = 0;      // what this space registers under R14Frisbee; 0 = neither
                                         // registered nor readable
            bool     isBoundCs = false;  // ...and it is the system the disc track is driving
            bool     walked = false;
            uint32_t systemsSeen = 0;
            bool     controlFound = false;   // R14NetPhysics met in the same pass
            uint32_t scriptsWalked = 0;
            uint32_t discScriptInstances = 0;
            uint16_t idxGameFrisbee = 0xFFFFu, idxPracticeDisc = 0xFFFFu;  // facts-ok: LOCAL.NO_COMPONENT the component-absent sentinel
            bool     liveGameFrisbee = false, livePracticeDisc = false;
        };
        static constexpr uint32_t kSpaceCap = 8;

        uint64_t pawnSpace = 0;      // the space the pawn census walks
        uint64_t discSpace = 0;      // the space that registers the frisbee system the bind holds
        bool     spacesDiffer = false;
        uint32_t scansWritten = 0;
        SpaceScan scans[kSpaceCap];
        uint16_t frisbeeLive = 0;        // live components in the bound R14Frisbee system
        uint32_t boundPhysId = 0;        // the component id the disc track drives
        uint16_t bindActorIndex = 0xFFFFu;  // facts-ok: LOCAL.NO_COMPONENT the registry slot the early bind keyed on, if it bound
        uint32_t actorsWritten = 0;
        Actor    actors[kActorCap];
        // Every instance of the disc's script found in either space, with the actor that owns it.
        uint32_t sitesWritten = 0;
        ScriptSite sites[kActorCap];
        const char* refused = "this entity surface has no disc-ring census";
    };
    virtual bool DiscRingCensus(DiscRingReading& out) const { (void)out; return false; }

    // ★ THE ONE ENGINE-DEPENDENT STEP OF THE LIVE DISC CENSUS, and the reason it is here rather
    // than in src/engine/disccensus.cpp: that file walks game spaces and must contain no engine
    // resolve at all, so the spaces are named for it from the surface that already knows how to
    // name them. Every candidate the disc bind would have considered, plus each one's parent
    // chain, deduplicated, writing at most `cap`. A LIVE session never binds the disc -- nothing is
    // loaded, so there is no track to drive it -- which is exactly why this takes the netGame the
    // tick was handed instead of reading the bind's remembered pointers alone.
    // Read-only: probed pointer hops, no call, no write.
    virtual uint32_t DiscCandidateSpaces(void* /*netGame*/, void** /*out*/, uint32_t /*cap*/) const {
        return 0;
    }

    // The distinct-set bookkeeping, exposed because it is the only part of the Wwise read that a
    // test host can drive: the query entry points live in echovr.exe and cannot be planted.
    // PURE. The 68-byte AkPositioningInfo sub_1413E9220 fills, decoded into the four numbers that
    // answer "does this sound ever go quiet". Separated from the call so a test can drive the
    // layout without a sound engine.
    static constexpr uint32_t kPositioningInfoBytes = 68;
    static void AudioDecodePositioningInfo(const uint8_t* buf, AudioPlayingSound& out);

    static void AudioAccumulateListenerSet(AudioMix& io, const uint64_t* ids, uint32_t stored,
                                           uint32_t reported);
    virtual bool AudioMixCensus(AudioMix& out) const { (void)out; return false; }

    // The read below the two global links, for the same reason camera_writepoint exposes its walk
    // below the image resolve: everything that can be got wrong -- the strides, the field offsets,
    // an over-long count, a table pointer that stops reading back part way -- lives here, and a
    // test can drive it against a sound player it built itself. The production entry is exactly
    // this called with *(void**)(*(void**)AUDIO.ROOT_VA + AUDIO.SYSTEM_OFF).
    static bool AudioMixFrom(void* soundPlayer, AudioMix& out);

    // The same read against a game space the caller names, for callers that have no entity surface
    // to ask. The admission gate is one of them, and it is the reason this exists: the surface is
    // released across an Unload and has not re-bound 150 ms into the next load, which is exactly
    // when the allocation that must not happen happens.
    static bool ActorPoolInSpace(void* gameSpace, uint64_t prefabId, ActorPoolRow& out);


    // ---- the disc's touch state --------------------------------------------------------------
    //
    // The two fields beside the owner dword that nothing in this plugin has ever written:
    // PhysCI+6 bit1 (CR15FrisbeeCS::IsHeld) and PhysCI+0x20 (CR15FrisbeeCS::LastOwner). With
    // IsHeld false forever, every consumer of the engine's own "if not held, use LastOwner" idiom
    // reads the unclaimed sentinel, which is why the disc renders as never touched. Both are pure
    // functions of the playhead, both are plain stores through the same gate and the same save as
    // the owner dword, and neither may ever become a call to CR15NetPhysicsCS::SetOwner.
    // `lastOwner` is the same mapped value the owner dword carries, or the none sentinel.
    virtual bool SetDiscTouchState(PhysHandle /*h*/, bool /*held*/, uint32_t /*lastOwner*/) {
        return false;
    }

    // The third field of the same answer, and the one the RING packets were missing: the engine's
    // remote holder arm (sub_140CED640) ends on bit 0x200 of the state word in the NEWEST physics
    // history node, not on anything in the CI. The recording carries that bit in `bits11`, so a
    // node pushed on the tick possession changes has it and every node after it does not -- which
    // is why the owner dword is agreed and the net-touch update's latch never moves. Held is a
    // LEVEL here: it is remembered per handle and re-applied to every node this plugin pushes for
    // that body, so it survives a seek, a pause and a reverse without an edge being replayed.
    virtual bool SetRecordedNodeHeld(PhysHandle /*h*/, bool /*held*/) { return false; }

    // ---- the local pawn's HANDLES, which is a different question from its STATE ----
    //
    // ★ WHY THIS EXISTS BESIDE PlayerOf/PhysOf.  PlayerOf scans CR15RemotePlayerCS -- the pool
    // named for the players this machine does NOT author -- while the capture read the recorder's
    // own pawn through CR15PlayerBroadcasterCS and the pools hanging off its actor.  A resolver
    // that asks only the first cannot find a pawn the second produced, and that asymmetry, not a
    // missing entrant, is what a bind reporting `absent this session` was describing
    // (ARCH-23 F-A / F-B).
    //
    // Which surface carries a driveable CI for a LOCALLY AUTHORED entrant is FACT-PP1, measured in
    // vivo and unresolved here: nothing in this method assumes an arm, and a caller must ask both.
    // It refuses BY NAME so the caller can say which half declined rather than merging two causes
    // into one absence.
    struct LocalPawnRef {
        PhysHandle phys{0xFFFFu};  // facts-ok: LOCAL.NO_COMPONENT the component-absent word
        bool       inPool = false;     // this machine authors the slot at all
        bool       model  = false;     // a model component resolved on the actor: it can be drawn
        // TWO pool elements carry this user slot.  Which of them is the pawn is not decidable from
        // the pool, and driving either is the crossover this whole surface exists to prevent, so it
        // is reported rather than resolved to the first match.
        bool       duplicateInPool = false;
    };
    enum class LocalPawn : uint8_t {
        NotAsked = 0,      // this IEntity predates the surface and cannot answer -- not an absence
        Ok,
        NoBroadcaster,     // no CR15PlayerBroadcasterCS, so the local surface has no source at all
        SlotNotInPool,     // resolved, and this machine does not author that slot
        ActorNull,         // in the pool, and neither actor source named an actor
        NoFindComponent,   // FirstActorComponent is not proved against the PHYSICS system's vtable
        NoPhysComponent,   // an actor, and no physics component on it
    };
    virtual LocalPawn LocalPawnAt(UserIndex, LocalPawnRef& out) const {
        out = LocalPawnRef();
        return LocalPawn::NotAsked;
    }

    // ---- the engine's own name for the pawn: its ENTITY ID ----
    //
    // Both surfaces end at a physics CI, and a CI belongs to an ENTITY -- the u16 the forward map
    // at PHYS.CS_HANDLE_MAP_OFF is keyed by and the reverse map at PHYS.CS_REVERSE_MAP_OFF holds
    // (LOCALBODY-12 F36 settled that both tables carry entity ids, and `ci+0x00` is the CI's own).
    // That id is the only thing here that can say whether the pawn a remote row names and the pawn
    // the local surface holds are ONE entity or two -- FACT-PP1's arms (2) and (3) differ in
    // exactly that, and a census printing `entity=none` cannot separate them.
    //
    // THE SURFACE IS THE CALLER'S TO STATE. The two reach the CI by different routes -- remote
    // through PlayerOf/PhysOf, local through LocalPawnAt's broadcaster actor -- and the routes
    // hand back different handles for the same user slot, so a key read off the route the caller
    // did not mean names another entity with nothing in the answer to say it did.
    enum class PawnSurface : uint8_t { Remote = 0, Local = 1 };
    enum class EntityKey : uint8_t {
        NotAsked = 0,     // this IEntity predates the surface -- not an absence, and not id 0
        Ok,               // `out` is the entity id. 0 IS A LEGAL ID (the local pawn's, F28), so
                          // this word, never the value, is what says an id was read
        Unavailable,      // no component system is bound; nothing was asked of any pool
        NoPawn,           // resolved, and that surface holds no pawn for this slot
        NoCi,             // a handle, and it addresses no live component instance
        IdentityRefused,  // a CI, and the identity gates refused it -- a key off it would name
                          // another entity, which is worse than no key
    };
    // `out` is written ONLY for Ok, for the same reason LocalUserIndex leaves it alone: every u16
    // is a legal entity id, so there is no value absence could be spelled with.
    virtual EntityKey EntityKeyOf(UserIndex, PawnSurface, uint16_t& out) const {
        (void)out;
        return EntityKey::NotAsked;
    }
    // The same key for a handle the DRIVEN surface writes (SetHistoryWindowDriven), read through
    // that surface's relaxed gate. EntityKeyOf refuses exactly the remote entrants that surface
    // exists for, which left every `pawn slot` line of a replay printing `ent=?`.
    virtual EntityKey EntityKeyOfDriven(PhysHandle, uint16_t& out) const {
        (void)out;
        return EntityKey::NotAsked;
    }

    // ---- the pose-write vouch for a slot-bound remote entrant (P3-MOTION) ----
    //
    // DIAG-ENTRANT-MOTION-TRACK: a remote entrant whose recorded body track carries no net id binds
    // through PhysOf(PlayerOf(slot)) -- PlayerCI+0x1C -- and the pose-write gate CheckCiIdentity
    // refuses that handle, because the phys CI it addresses carries an entity id that does not
    // round-trip back to it.  The handle the write NEEDS is the one the engine's OWN actor->component
    // resolver hands back for the same actor (ComponentOnActor(phys_.cs, actor), the call the local
    // surface makes at entity.cpp:2366), validated through the SAME ResolveVouched round-trip the
    // write passes.  This asks that question and answers with a handle ONLY when it vouches -- the
    // existing gate is never weakened, and a handle it would decline is reported, never driven.
    //
    // It is read-only: two map reads and one component lookup, no pool rebuild and no CI mutation.
    // `diag` carries the gate's own numbers for the bound handle (CI[+0] and the reverse-map entry),
    // which is the one fact that separates Scenario A (a coherent CI id and a vouched re-resolve)
    // from Scenario B (an uninitialised CI id and no phys component on the actor -- the receive-path
    // wall) in a single run's log.
    struct PhysVouch {
        uint16_t boundHandle      = 0xFFFFu;  // facts-ok: LOCAL.NO_COMPONENT the component-absent word
        bool     boundVouched     = false;    // does boundHandle already pass ResolveVouched?
        uint16_t actorComponent   = 0xFFFFu;  // facts-ok: LOCAL.NO_COMPONENT ComponentOnActor(phys), absent
        bool     componentPresent = false;    // actorComponent != kNoComponent
        bool     vouched          = false;    // a ResolveVouched-passing handle was found
        uint16_t vouchedHandle    = 0xFFFFu;  // facts-ok: LOCAL.NO_COMPONENT the vouched handle, or absent
        BodyDiag diag;                        // the bound handle's gate reading: CI[+0], revMap, index
    };
    virtual PhysVouch VouchPhysForUser(UserIndex) const { return PhysVouch{}; }

    // ---- global -------------------------------------------------------------------
    virtual void   SetLodThrottle(bool enabled) = 0;            // CR15RemotePlayerCS + 520

    // SetLodThrottle writes a dword it never read; in a live session that
    // is a clobber with no way back.  A caller that means to BORROW the field uses this pair: the
    // claim reads the engine's value once and keeps it, the release writes exactly that value
    // back.  Both answer false when there is nothing to write through, so a caller can tell a
    // refused claim from a made one and never releases a claim it did not take.
    //
    // Defaulted rather than pure: every test fake in the tree implements SetLodThrottle, and
    // forwarding keeps those fakes observing the writes they were written to observe.
    virtual bool   ClaimLodThrottle(bool enabled) { SetLodThrottle(enabled); return true; }
    virtual bool   ReleaseLodThrottle() { return false; }

    // Not in B.4's text.  False when a component system did not resolve; every call above is
    // inert while it is false, and Diagnostic() says which one refused.  The alternative --
    // dereferencing a null pool base on the tick thread -- is not a diagnosis.
    virtual bool        Available() const = 0;
    virtual const char* Diagnostic() const = 0;

    // WHICH BINDING THIS IS.  Everything a consumer derives from the bound game space -- a user
    // index, an INV-OWN1 registration, a PlayerHandle -- was issued by ONE CR15NetGame and is
    // garbage against the next one, so a consumer that caches any of it needs to know when the
    // engine rebuilt the space underneath it.  Neither of the two signals it already has can say:
    //
    //   the POINTER    -- the shipping implementation is a process-lifetime singleton, the same
    //                     address from Init to Shutdown, so a consumer watching it sees a rebuild
    //                     only when a TEST swaps a fake in (AUDIT_WIRING_A F-A);
    //   Available()    -- entity_module re-binds by calling its Bind() with no Unbind, so a space
    //                     rebuilt inside one engine call reads available before and after.
    //
    // The epoch is that signal.  It advances once per bind to a game space whose identity differs
    // from the bound one -- a new netGame, a new space, or the same block carrying a rebuilt system
    // table -- and never otherwise: the retry that re-Inits the SAME space until its pools resolve
    // does not advance it, or a load window would re-dress the roster every tick.
    //
    // Three states, because "this build does not track binds" is not "nothing is bound yet", and
    // neither of them is a generation number (S9.2).  `out` is written only for Ok.
    enum class BindEpoch : uint8_t {
        NotAvailable = 0,  // this implementation does not track binds; ask something else
        Unbound,           // tracked, and no game space is bound right now
        Ok,                // `out` is the current generation: >= 1, monotonic, never reused
    };
    virtual BindEpoch BindEpochNow(uint64_t& out) const { (void)out; return BindEpoch::NotAvailable; }

protected:
    ~IEntity() = default;
};

// There is deliberately NO SetSyntheticOwner(PlayerHandle) and NO pose write on PhysHandle.
// The two mistakes B2 describes are not expressible in this interface.

class IPosePublisher {
public:
    virtual void        Publish(PlayerHandle, const PlayerState& renderPose,
                                              const PlayerState& physicsPose) = 0;
    virtual const char* Strategy() const = 0;      // "PP-A" | "PP-B"
    virtual bool        NeedsLateTick() const = 0;

protected:
    ~IPosePublisher() = default;
};

namespace entity {

// Which publisher is live.  PP-B is the default and the only one W1-E ships; PP-A exists as a
// named alternate for as-seen playback, which needs the two pose slots to differ and therefore
// cannot use PP-B (the engine copies slot 1 into slot 2 verbatim).
enum class PoseStrategy : uint8_t {
    PpB,   // author PlayerCI+60; the authority branch fills both slots.  Stage 0.
    PpA,   // author PlayerCI+168 and +320 directly.  Requires ITick::RegisterLate.
};

// Why the pools did not resolve.  "it is unavailable" is not a diagnosis.
enum class Resolve : uint8_t {
    Ok,
    NoGameSpace,        // the caller handed us null
    SpaceNotReady,      // the space's component-system table is not constructed, empty, or unsorted
    NoPhysicsCs,        // FindComponentSystem(R14NetPhysics) returned null
    NoRemotePlayerCs,   // FindComponentSystem(R14RemotePlayer) returned null
    PoolNotReady,       // a system resolved but its pool base or handle map is still null

    // The local-pawn path resolves separately and NEVER fails Init: the rest of B.4 works
    // without it, and a capture that loses ReadPlayer as well as ReadLocalPlayer is strictly
    // worse than one that loses only the second.  LocalPawnDiagnostic() reports these.
    NoBroadcasterCs,    // FindComponentSystem(R14PlayerBroadcaster) returned null
    NoEchoUnitCs,       // FindComponentSystem(R15EchoUnit) returned null
    NoBodyCs,           // FindComponentSystem(R14Body) returned null
    NoFindComponent,    // the actor-to-component entry point failed its byte guard
};

const char* ResolveText(Resolve);

// `gameSpace` is the CncaGameSpace the component systems are registered in.  Both pools are
// resolved through the engine's own CncaGameSpace::FindComponentSystem(CSymbol64) -- a stock
// call and a binary search over the system table, so no component-system offset is guessed.
IEntity*        EntityCreate(void* gameSpace);
void            EntityDestroy(IEntity*);

// Every VirtualQuery entity.cpp still issues to prove a component pool's element range, counted.
// The accessors used to issue one PER ELEMENT READ -- 110 a capture tick, 652 ns each on the host
// ENG-W2A-PERF measured, which is 71.7 us of a 30 us tick budget before the game's own address-space
// churn is counted. They now prove the range once per (pool base, extent, bind epoch) and touch the
// element under __try. Monotonic, process-wide, and never rewound: a per-tick budget is written as a
// difference across the tick, which is the only form that can distinguish "cached" from "never asked".
uint64_t        SpanProbeCount();

// That cache itself, published because a SECOND caller now needs it (identity_access.cpp's
// per-entrant scoreboard read).  One replay::Readable per (base, extent, bind epoch); BindEpochNow
// drops every line at once.  Callers own their instances and one line holds one base -- two bases
// proved in the same frame through one line is a miss every call, which is no cache at all.
//
// A REFUSAL is cached too, on its own line and on a much shorter cadence than a success
// (SpanProofReprobeTicks).  It used to be cached not at all, on the reading that "not mapped when I
// asked" is an answer about that instant -- and ENG-GATHER-COST measured what that costs: a
// 64-slot pool whose every slot refuses paid one VirtualQuery per slot per tick, forever, ~1 ms a
// tick late in a match.  The safety the old shape bought is kept by the cadence rather than by
// asking every time: a pool built a frame later is picked up one interval late, not never, and the
// guarded load (TryLoadBytes) is still what stands between a stale answer and the game's thread.
struct SpanProof {
    const uint8_t* base   = nullptr;
    size_t         proved = 0;
    uint64_t       epoch  = 0;
    uint32_t       reads  = 0;

    // The refusal, on its own fields: a refusal that overwrote the proof would cost a line its
    // cache the first time a caller asked about a second base through it.
    const uint8_t* refusedBase  = nullptr;
    size_t         refusedBytes = 0;
    uint64_t       refusedEpoch = 0;
    uint32_t       refusedCalls = 0;

    const uint8_t* Prove(void* rawBase, size_t bytes);
};

// AUDIT_ARCH21 CONDITION-B6. How many reads one proof is honoured for before Prove asks again.
// A pool can be FREED at an UNCHANGED base without anything the binding keys on moving:
// EntityBind fires only when {netGame, space, entries} differs, the base is not in that
// identity, and b == base then holds over unmapped memory for the rest of the binding. A proof
// that never expires is a latched SUCCESS -- the same claim as the latched refusal Prove
// already declines to make -- so the cadence is what turns `proved once` into `proved
// recently`, and it bounds the stale window in the unit that is actually exposed: reads.
uint32_t        SpanProofMaxCachedReads();

// How many calls one REFUSAL is honoured for before Prove asks again.
// The gather walks each pool slot once a tick through its own line, so a call on a refusing line is
// a tick, and the interval is one second at the engine's fixed step -- three orders below
// SpanProofMaxCachedReads, because a stale refusal hides a pool that has appeared while a stale
// success hands back a base that has gone.  The two failure modes are not the same size.
uint32_t        SpanProofReprobeTicks();

// The other half of a proof: the load itself.  Prove answers about the instant of ITS VirtualQuery
// and deliberately does not re-probe on every cached read, so the LOAD is the thing that sees a
// page unmapped after the proof -- the TOCTOU window VirtualQuery never covered.  Refuses on the
// three codes replay::Readable refuses for, guard page included: entity.cpp's SafeReadFilter and
// this share one filter so the two link edges cannot answer differently.  False leaves `out`
// untouched, and a caller that publishes on false would be publishing a value it never read.
bool            TryLoadBytes(const void* at, void* out, size_t bytes);

template <class T>
inline bool TryLoad(const void* at, T& out) { return TryLoadBytes(at, &out, sizeof(T)); }

// The store side of the same guard, for the two places this plugin writes engine memory. False
// means nothing was written.
bool            TryStoreBytes(void* at, const void* in, size_t bytes);

template <class T>
inline bool TryStore(void* at, const T& in) { return TryStoreBytes(at, &in, sizeof(T)); }

// The bind generation the cache keys on, and the single advance EntityBind makes.  It is
// published because the cache lives in entity_spanproof.cpp, on both link edges, while EntityBind lives
// in entity.cpp on only one of them -- and one generation held by two counters is two answers.
// EntityBind is the only caller of BindEpochAdvance.
uint64_t        BindEpochCounter();
void            BindEpochAdvance();

// The two calls that move BindEpochNow, and the reason it is not EntityCreate that moves it.
//
// EntityBind is "this is a NEW binding": it advances the epoch by one and then resolves, whether or
// not the resolve succeeds -- a space whose pools are not built yet is still the space we bound to,
// and the retry that re-Inits it every tick until they are must NOT look like a rebuild.  That is
// why EntityCreate leaves the epoch alone: entity_module's Tick reaches its Bind() only when the
// resolved SpaceIdentity differs from the bound one, so "one EntityBind, one epoch" holds by
// construction, and the retry path keeps calling EntityCreate.
//
// EntityUnbind drops the binding.  The counter is monotonic and is NOT rewound; only the answer
// changes, to Unbound.
IEntity*        EntityBind(void* gameSpace);
IEntity*        EntityUnbind();

// The same resolve, with the engine's lookup supplied instead of taken from the mapped image.
//
// EntityCreate resolves CS.LOOKUP_BY_SYMBOL behind its byte guard, and a test host maps no
// echovr.exe, so on a test host every Init refuses at that guard and NOTHING below it has ever
// been exercised: not the two pools, not PoolNotReady, and not Init's reset of a previous binding
// -- the property that keeps an old pool's saved owner handles out of a new pool.  This is the
// seam that lets a test drive them, and it is the ONLY thing it does: the guarded path is
// untouched and is still what every shipping caller takes (tests/entity/test_entity_module.cpp
// asserts the guard still refuses).  `find` must not be null.
using CsLookupFn = void* (*)(void* gameSpace, uint64_t symbol);

// The second half of the same seam, for the same reason.  `find` alone reaches the pools, but two
// of the local-pawn paths go through an ENGINE ENTRY POINT that InitLocalPawn resolves out of the
// mapped image behind a prologue guard (and, for FirstActorComponent, a vtable-slot match): the
// echo unit's LeafPool::Element, reached only after ComponentOnActor, and ModelVisible, which needs
// UserActor as well.  On a test host both guards refuse, both handles come back kNoComponent, and
// the bodies below them -- including their span checks -- are unreachable however the pools are
// wired.  A non-null member here REPLACES that resolution and nothing else; a null one leaves the
// guarded path exactly as it was, which is what every shipping caller takes.
using AnyEngineFn = void (*)();
struct EngineEntryPoints {
    AnyEngineFn findActorComponent = nullptr;   // LOCAL.FIND_ACTOR_COMPONENT
    AnyEngineFn userActor          = nullptr;   // LOCAL.USER_ACTOR
    // ReadLocalPlayer reaches the echo unit's pool element only inside the block these three
    // gate, so a test that wants that element's span check has to get past them as well.
    AnyEngineFn echoBaseTransform  = nullptr;   // LOCAL.ECHO_BASE_TRANSFORM
    AnyEngineFn echoHeadTransform  = nullptr;   // LOCAL.ECHO_HEAD_TRANSFORM
    AnyEngineFn echoHandTransform  = nullptr;   // LOCAL.ECHO_HAND_TRANSFORM
    AnyEngineFn anchorBodyPart     = nullptr;   // LOCAL.ANCHOR_BODY_PART
    // Added for the local-pawn rig: without these two, kLocalRefVel and kLocalHolding are the
    // only groups no test can drive to their SOURCED arm, and a mask that cannot reach them
    // cannot show a whole-mask read succeeding.
    AnyEngineFn echoReferenceVel   = nullptr;   // LOCAL.ECHO_REFERENCE_VEL
    AnyEngineFn heldPlayer         = nullptr;   // LOCAL.HELD_PLAYER
    // The CPhBody chain B1 reads a locally simulated body through. Neither half is CALLED any
    // more -- entity.cpp reproduces both, so no aggregate-return convention is guessed -- and
    // what the chain needs from outside is DATA: the CPhSystem the zone table hangs off, which
    // the product reads through the global at CLOCK.THIRD_PTR. Injecting it is what makes the
    // whole walk reachable on a test host, where no echovr.exe is mapped and both prologue
    // guards therefore refuse.
    // PHYS.ANGVEL_FROM_CLOUD. The CPhBody stores no angular velocity; this is the engine's own
    // derivation of it, and the only way ReadLocalBody can answer angVel with a measurement.
    AnyEngineFn physAngVelFromCloud = nullptr;  // PHYS.ANGVEL_FROM_CLOUD
    void*       phSystem           = nullptr;   // *CLOCK.THIRD_PTR, the CPhSystem
    // The pair every push into a CI's history ring goes through. Both are resolved out of
    // the mapped image behind prologue guards, so on a test host both are null and
    // SetHistoryWindowDetailed stops at NoRecycler before one node is pushed -- which left
    // the WRITE side of the identity gate with no fixture that could reach it, and the gate
    // and the write addressing different elements therefore unobservable
    // (AUD-W2-B-POSEREAD residual 3). A non-null member REPLACES that resolution and
    // nothing else; a null one leaves the guarded path exactly as it was, which is what
    // every shipping caller takes -- `injected` is null on all of them.
    AnyEngineFn historyUnlink      = nullptr;   // ENTITY.LIST_UNLINK
    AnyEngineFn historyRecycle     = nullptr;   // ENTITY.HISTORY_RECYCLE
};
IEntity*        EntityCreateWithLookup(void* gameSpace, CsLookupFn find,
                                       const EngineEntryPoints* injected = nullptr);

// The publisher borrows the IEntity; it does not own it.  `strategy` selects the
// implementation; PP-A returns true from NeedsLateTick() and the caller must then have an
// ITick::RegisterLate source, or Publish() is a no-op and Diagnostic() says so.
IPosePublisher* PosePublisherCreate(IEntity*, PoseStrategy strategy);
void            PosePublisherDestroy(IPosePublisher*);

// EmitChangedDelegates is what makes a replayed player's grab, boost, brake and thruster sound.
// The switch is here rather than inside the publisher because the only honest test of "does this
// feed produce sound" is the same content played twice, once with it withheld.
void SetDelegateEmitEnabled(bool on);

// `playback.disc_net_touch_held`. The remote arm of the engine's holder resolve (sub_140CED640)
// ends on bit 0x200 of the newest physics history node's state word, so a body whose owner dword
// names a remote user still reads as UNHELD unless that bit is live when the net-touch update
// samples it. Off means the recording's own bits11 is the only thing that ever sets it, which is
// the behaviour every build before DISC-RING-4 had.
void SetDiscNodeHeldEnabled(bool on);
bool DiscNodeHeldEnabled();

// DISC-RING-5's read of WHERE the 0x200 level is lost. Counts rather than a trace: the level is a
// function of the playhead, reconciled on every publish and re-applied to every node pushed, so
// the only question is which of the three steps drops it and whether the word survives the tick.
struct NodeHeldDiag {
    uint32_t calls        = 0;  // SetRecordedNodeHeld reached, feature on
    uint32_t resolveFail  = 0;  // the CI gate declined the disc's handle
    uint32_t headNull     = 0;  // resolved, but PhysCI+0x158 had no newest node
    uint32_t stamped      = 0;  // the level reached that node
    uint32_t pushes       = 0;  // nodes PushHistoryInto created for the level's body
    uint64_t histCount    = 0;  // PHYS_CI_HIST_COUNT_OFF at the last stamp attempt
    uint16_t lastState    = 0;  // the head node's state word after the last stamp
    bool     lastHeld     = false;
};
NodeHeldDiag NodeHeldDiagRead();
bool DelegateEmitEnabled();

// The CR15RemotePlayerCS instance E-4 resolved, or null. E-8 needs it for the LOD throttle triple,
// which lives on the component SYSTEM rather than on any component. It is a free function rather
// than an IEntity method because B.4 is frozen and this is not in it.
void* RemotePlayerSystem(IEntity*);

// Why ReadLocalPlayer returns 0, when it does.  Separate from IEntity::Diagnostic() because the
// local-pawn systems resolve independently of the two B.4 pools and must not be able to take them
// down: "it is unavailable" is not a diagnosis, and neither is a silently empty stream.
const char* LocalPawnDiagnostic(IEntity*);

}  // namespace entity
// The names a log line prints. Every enumerator is spelled out and the default answers "unknown"
// rather than reusing a neighbour's word: a new state that silently rendered as an existing one
// would be a gap no reader could see.
inline const char* ActorSourceName(IEntity::ActorSource s) {
    switch (s) {
        case IEntity::ActorSource::None:             return "none";
        case IEntity::ActorSource::BroadcasterKeys:  return "broadcasterKeys";
        case IEntity::ActorSource::NetGameUserActor: return "netGameUserActor";
    }
    return "unknown";
}
inline const char* LocalRefusalName(IEntity::LocalRefusal r) {
    switch (r) {
        case IEntity::LocalRefusal::NotAsked:         return "notAsked";
        case IEntity::LocalRefusal::Ok:               return "ok";
        case IEntity::LocalRefusal::NoBroadcaster:    return "noBroadcasterCs";
        case IEntity::LocalRefusal::SlotNotInPool:    return "slotNotInPool";
        case IEntity::LocalRefusal::ActorNull:        return "actorNull";
        case IEntity::LocalRefusal::NoSourceAnswered: return "noSourceAnswered";
    }
    return "unknown";
}

}  // namespace replay

// Transitional: the unqualified spelling, for translation units that have not been moved into
// `namespace replay` yet.  A using-declaration names the existing class -- it does not declare a
// second one -- so `::IEntity` and `replay::IEntity` are the same type and stay that way.
using replay::UserIndex;
using replay::UserSlot;
using replay::PhysHandle;
using replay::PlayerHandle;
using replay::BodyState;
using replay::BodySource;
using replay::BodyReading;
using replay::BodyRefusal;
using replay::BodyDiag;
using replay::BypassRead;
using replay::PlayerState;
using replay::IEntity;
using replay::IPosePublisher;
using replay::kLocalPose;
using replay::kLocalRefVel;
using replay::kLocalBody;
using replay::kLocalTrigger;
using replay::kLocalAnchor;
using replay::kLocalHolding;
