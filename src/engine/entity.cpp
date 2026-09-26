// E-4: typed access to the two component pools, HP1's synthetic owner, and the PP-B publisher.
//
// Every number here comes from facts_generated.h by id. None is transcribed, and the two pools'
// offsets never mix: PhysHandle and PlayerHandle do not convert, and each pool has its own
// accessor block below with its own facts.
//
// Derivations, and the evidence for every claim in the comments, are in docs/spikes/W1-E.md.

#include "engine/entity.h"

#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/livesafety.h"
#include "bootstrap/log.h"
#include "bootstrap/pageprobe.h"
#include "bootstrap/tableguard.h"
#include "engine/localpawn_facts.h"
#include "engine/scriptgraph_facts.h"
#include "generated/facts_generated.h"
#include "foundation/bitio/wirefields.h"
#include "foundation/gametype.h"

namespace {

// --------------------------------------------------------------------------------------
// Raw access. Confined to these four helpers so that every game-memory read in the file is
// one line long and obviously bounds-free -- the pools are engine-owned and the handle maps
// are the engine's own, so there is nothing for us to bounds-check that it has not already.
//
// That holds for a stride the registry states DECISIVELY. It does not hold for the strides
// W0-2's merge left at PC_PRESENT/U (ECHOUNIT_CI, BROADCASTER_CI, MODEL_CI, NETACTOR_TABLE_ENTRY,
// NETGAME_SLOT_TABLE): the pool is still the engine's, but the number that turns an index into
// an address is only PRESENT, so the address it computes is tested with Readable before it is
// read. The test is coarse -- page-granular, so it catches an element off the end of the mapping
// and not one merely inside the wrong element -- and coarse is the whole point: it converts a
// fault on the tick thread into a refusal.
// --------------------------------------------------------------------------------------
inline uint8_t* At(void* base, uint32_t off) { return static_cast<uint8_t*>(base) + off; }

template <typename T>
inline T Rd(const void* base, uint32_t off) {
    T v;
    memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof(T));
    return v;
}

template <typename T>
inline void Wr(void* base, uint32_t off, const T& v) {
    memcpy(static_cast<uint8_t*>(base) + off, &v, sizeof(T));
}

inline void* Deref(const void* base, uint32_t off) { return Rd<void*>(base, off); }

// Whether a binding is in force at all. The generation number itself lives in entity_spanproof.cpp with
// the cache it invalidates, because identity_access.cpp reads it through a link edge this file
// is not on; this flag is entity's alone, so it stays here.
bool     g_bindCurrent = false;

// ---------------------------------------------------------------------------------------
// THE SPAN CACHE, and the guarded load that replaced the per-element probe.
//
// `replay::Readable` is a `VirtualQuery` -- NtQueryVirtualMemory, which takes the target address
// space's lock and walks its VAD tree. ENG-W2A-PERF measured it at 652 ns on this host and counted
// 110 engine accessor calls per capture tick, each proving its own element: 71.7 us of a 30 us
// budget, uncontended, before echovr.exe's own churn. The element strides are still PC_PRESENT/U,
// so the range still has to be proved -- but a pool's range is a property of where the engine put
// the pool, not of the call, and it only changes when the pool moves, when a higher element is
// addressed than has been proved, or when the binding is replaced.
//
// So: prove the range once, and make the ELEMENT access a guarded load. That is ENG-COST section 2's
// slot pattern and its argument transfers unchanged -- a probe answers about the instant it ran and
// the read it protects happens afterwards, so it never covered an unmap in between; a load under
// __try answers about the instant of the load, costs nothing on x64 when it does not fault, and
// refuses in every case the probe existed to refuse. The two together are strictly stronger than
// the probe alone was: the probe bounds the stride, the load sees the page that went away after it.
//
// AUDIT_M9 (1): the pool base is re-read out of the component system on EVERY call and the epoch is
// part of the key, so a CR15Game that moved on level reload and a session torn down and rebuilt by
// SESSION.CREATE_OFFLINE_MATCH are both picked up on the first access after the change. This is
// never "proved once per process".
// ---------------------------------------------------------------------------------------

// EVERY page, not just the two ends: a span whose middle page is unmapped is not readable, and a
// two-byte touch called it readable. pageprobe's touch is the same cost per page as this one was
// per end, it re-arms a guard page it trips, and it counts its faults where the pacing line can
// print them.
bool TryTouchSpan(const void* at, size_t bytes) {
    return replay::PageSpanReadable(at, bytes);
}

// The cache is entity.h's SpanProof, so the scoreboard reader shares this one implementation
// rather than growing a second copy of it. The three pools below hold their own lines.
using SpanCache = ::replay::entity::SpanProof;
using ::replay::entity::TryLoad;
using ::replay::entity::TryStore;

// The wire stores the two hand offsets as three IEEE binary16 each, and the engine's authority
// branch expands them with (h << 13) + 0x70000000 plus a denormal fixup. These are the exact
// inverse pair, so a value written by Float3ToHalf and read back by the engine round-trips to
// the half's own precision -- which is the resolution the recorded pose has in the first place.
inline float HalfToFloat(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    const uint32_t mag  = static_cast<uint32_t>(h & 0x7FFFu) << 13;
    const uint32_t bits = sign | (mag ? mag + 0x38000000u : 0u);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

inline uint16_t FloatToHalf(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000u);
    const int32_t  exp  = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    if (exp <= 0)  return sign;                              // underflows to signed zero
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7BFFu);  // clamp, never produce inf
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) |
                                 ((bits >> 13) & 0x3FFu));
}

inline void HalfToFloat3(const void* src, float out[3]) {
    for (uint32_t i = 0; i < 3; ++i)
        out[i] = HalfToFloat(Rd<uint16_t>(src, 2u * i));
}

inline void Float3ToHalf(const float in[3], void* dst) {
    for (uint32_t i = 0; i < 3; ++i)
        Wr<uint16_t>(dst, 2u * i, FloatToHalf(in[i]));
}

// *** EVERY address in facts_generated.h is a PREFERRED-BASE VA, and echovr.exe does not have to
// load there: its DllCharacteristics are 0x8160 -- DYNAMICBASE and HIGH_ENTROPY_VA -- and it
// carries 163,956 bytes of relocations, read out of the shipped file's own optional header.  A
// run in runs/20260828-200206Z resolved a .text operand at 0x7FF6267C4E68, so the slide is
// observed and not merely permitted.  Casting a fact straight to a pointer therefore works on
// every run that happens to land at 0x140000000 and is a wild dereference inside the owner's
// game on the rest -- the failure class that passes all local testing.  buildguard::MappedAddress
// is the ONE place that rebases; nothing in this file may dereference a fact without it.
const void* Mapped(uint64_t pcVa) {
    return pcVa ? replay::buildguard::MappedAddress(pcVa) : nullptr;
}

// A site is only touched after its recorded prologue bytes match the mapped image. B-2 runs the
// same comparison centrally over build_sites.inc; this repeats it locally because E-4 calls engine
// functions and a wrong build must refuse here too, not merely be logged elsewhere.
//
// `len` is a parameter rather than a fixed 20 because 20 is not always discriminating: the
// CR15EchoUnitCS transform getters are leaves over one pool whose bodies differ only in a
// displacement, and 32 other functions in this image share BaseTransform's first 20 bytes.  See
// engine/localpawn_facts.h for the sweep.
bool PrologueOkN(uint64_t va, const uint8_t* want, size_t len) {
    const void* at = Mapped(va);
    return at && memcmp(at, want, len) == 0;
}

bool PrologueOk(uint64_t va, const uint8_t (&want)[20]) {
    return PrologueOkN(va, want, 20);
}

// CncaGameSpace::FindComponentSystem(CSymbol64) -- a binary search over the space's own system
// table. Using it means E-4 never guesses a component-system instance offset.
using FindCsFn      = void* (*)(void* gameSpace, uint64_t symbol);
using IsLocalFn     = uint64_t (*)(void* cs, uint16_t handle);
using UnlinkFn      = void (*)(void* node);
using RecycleFn     = void (*)(void* cs, void* ci, void* node);
using EmitDeltasFn  = void (*)(void* cs, uint32_t userId, const void* oldState, const void* newState);
// CR15RemotePlayerCS vtable slot 472: the handle -> actor resolve sub_140CCE580 makes before every
// delegate it raises. The scratch it fills is the caller's, and the pointer it returns names it.
using ActorOfHandleFn = void* (*)(void* cs, void* scratch, uint32_t compactHandle);

// The script module's `get` binding (module+240). sub_1412EC3A0 calls it with exactly these four
// arguments and the compiled script_get_variable has exactly this signature; it returns 1 when it
// recognised both hashes and wrote `out`, and 0 without touching `out` when it did not.
using ScriptGetFn = int (*)(void* graph, uint64_t name, uint64_t type, void* out);

// The three symbols the script census spends, hashed by the project's own SymbolHash so no literal
// has to be trusted: the component system to walk, and the {name, type} pair that names `self`.
inline constexpr uint64_t kSymNcaScript = replay::gametype::SymbolHash("ncaScript");
inline constexpr uint64_t kSymSelf      = replay::gametype::SymbolHash("self");
inline constexpr uint64_t kSymActorType = replay::gametype::SymbolHash("actor");

// DISC-RING-2. The rest of RE-DISC-RING-2 §5's discrimination table, hashed the same way. The
// R14Frisbee row is the control: lp::kSymR14Frisbee was transcribed from the image's own symbol
// qword long before SymbolHash existed, so a build in which these derivations are wrong cannot
// link. `game_frisbee` is already pinned the same way at lp::kSymGameFrisbee.
inline constexpr uint64_t kSymTouchInteract   = replay::gametype::SymbolHash("R14TouchInteract");
inline constexpr uint64_t kSymNcaPhysics      = replay::gametype::SymbolHash("ncaPhysics");
inline constexpr uint64_t kSymNetFrisbeeTrail = replay::gametype::SymbolHash("R15NetFrisbeeTrail");
inline constexpr uint64_t kSymPracticeDisc    = replay::gametype::SymbolHash("practice_disc");
static_assert(replay::gametype::SymbolHash("R14Frisbee") ==
                  replay::localpawn_facts::kSymR14Frisbee,
              "SymbolHash does not reproduce the frisbee system symbol read out of the image");
static_assert(kSymTouchInteract == 0x2E40131422F7B1FCull,
              "R14TouchInteract must hash to the component the disc script's AttachHand arm gates on");

// The disc's own visual script: the graph that owns the `bubble` actor -- the floating ring.
inline constexpr uint64_t kDiscScriptHash = 0x38965D90A823F03Full;

// RE-DISC-RING-3 §6's R1. Every name is hashed here rather than transcribed, and the two the packet
// states as verified constants are pinned by a static_assert -- so a SymbolHash that ever stopped
// agreeing with the image would fail the build instead of reading a variable nobody named.
inline constexpr uint64_t kSymBubble    = replay::gametype::SymbolHash("bubble");
inline constexpr uint64_t kSymModelComp = replay::gametype::SymbolHash("model_component");
inline constexpr uint64_t kFlagType     = 0xC8C33E483C7F1FB7ull;   // script_get_variable's flag case
inline constexpr uint64_t kSymbolType   = 0x41D2D42417210812ull;   // ...its symbol case
static_assert(kSymBubble == 0x41D2C52818210B1Bull,
              "SymbolHash does not reproduce the disc script's `bubble` actor variable");
static_assert(kSymModelComp == 0x49A99FB410CA0728ull,
              "SymbolHash does not reproduce the disc script's `model_component` symbol variable");
// Bit order IS ScriptSelfRow::flagsAnswered's bit order; entity.h's kDiscFlagNames prints them.
inline constexpr uint64_t kDiscFlagSyms[replay::IEntity::kDiscFlagCount] = {
    replay::gametype::SymbolHash("is_game_disc"),
    replay::gametype::SymbolHash("is_local_disc"),
    replay::gametype::SymbolHash("is_sim_disc"),
    replay::gametype::SymbolHash("is_practice_disc"),
    replay::gametype::SymbolHash("in_score_state"),
    replay::gametype::SymbolHash("fx_active"),
};
static_assert(kDiscFlagSyms[1] == 0x035BD9264086A009ull,
              "SymbolHash does not reproduce `is_local_disc`, the flag HIDE-A is gated on");

// facts.tsv records addresses against the image base the PE declares; the loader may map it
// elsewhere. Read-only, probed, and only ever spent on a one-shot diagnostic.
const uint32_t* RebasedDword(uint64_t va) {
    const auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (base == nullptr || va < facts::PE_IMAGE_BASE) return nullptr;
    const uint8_t* const at = base + (va - facts::PE_IMAGE_BASE);
    return replay::Readable(at, sizeof(uint32_t)) ? reinterpret_cast<const uint32_t*>(at) : nullptr;
}

// Why a delegate handle could not be built, because "no handle" and "the map disagrees with the
// pool" are different faults and the probe has to name which one it saw.
enum class HandleBuild : uint8_t { Ok, NoElement, NoMapEntry };

// A component-system table longer than this is not a table we understand; the walk refuses rather
// than reading an unbounded stride off a stale pointer.
inline constexpr uint64_t kSystemScanMax = 4096u;

// csoundplayer.cpp caps listeners at 6 and hands out view ids one at a time; a count far past that
// means the pointer is stale, so the census reports the number and walks nothing.
inline constexpr uint64_t kAudioTableScanMax = 256u;

// The emitter map is a different order of magnitude -- every sound-emitting component in the level
// holds a row -- so it gets the component-system bound instead of the listener one.
inline constexpr uint64_t kAudioEmitterScanMax = kSystemScanMax;

// One pool, one set of offsets. The two instances of this struct are the only place the two
// pools' numbers appear, and they are filled from different fact ids.
struct Pool {
    void*    cs        = nullptr;
    uint32_t stride    = 0;
    uint32_t mapOff    = 0;   // CS -> u16 handle map, stride 4
    uint32_t poolOff   = 0;   // CS -> element base
    uint32_t countOff  = 0;   // CS -> live element count (u16)
    uint32_t revMapOff = 0;   // CS -> index-to-handle map, stride 2

    size_t Count() const { return cs ? Rd<uint16_t>(cs, countOff) : 0u; }

    void* Element(uint16_t handle, uint16_t* indexOut = nullptr) const {
        if (!cs) return nullptr;
        const void* map = Deref(cs, mapOff);
        if (!map) return nullptr;
        const uint16_t index = Rd<uint16_t>(map, static_cast<uint32_t>(4u * handle));
        if (indexOut) *indexOut = index;
        void* base = Deref(cs, poolOff);
        if (!base) return nullptr;
        return At(base, stride * index);
    }

    // The element the DENSE walk addresses: PHYS.CS_POOL_BASE_OFF + stride * i, no map in it. The
    // enumerating caller's i is bounded by the same PHYS.CS_COUNT_OFF the map lookup is bounded by,
    // and it satisfied that bound before it called.
    void* ElementAt(size_t index) const {
        if (!cs) return nullptr;
        void* base = Deref(cs, poolOff);
        return base ? At(base, stride * static_cast<uint32_t>(index)) : nullptr;
    }

    // BOTH halves of the stride-4 forward map entry. Element() reads only the low one, so a 1-based
    // or half-swapped entry has never been visible in any artefact this reader produced.
    bool MapEntry(uint16_t handle, uint16_t& low, uint16_t& high) const {
        const void* map = cs ? Deref(cs, mapOff) : nullptr;
        if (!map) return false;
        low  = Rd<uint16_t>(map, static_cast<uint32_t>(4u * handle));
        high = Rd<uint16_t>(map, static_cast<uint32_t>(4u * handle + 2u));
        return true;
    }

    uint16_t HandleAt(size_t index) const {
        const void* rev = ReverseMap();
        return rev ? Rd<uint16_t>(rev, static_cast<uint32_t>(2u * index)) : 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    }

    // The map that turns a pool index back into a handle. Element() reports the index itself, so
    // the identity gate re-reads neither the map nor the arithmetic over it.
    void* ReverseMap() const { return cs ? Deref(cs, revMapOff) : nullptr; }

    bool Ready() const { return cs && Deref(cs, mapOff) && Deref(cs, poolOff); }

    // Everything the offsets above are read out of, in one span. The two u16 counts ComponentIdIn
    // reads (idCapacity, live) sit below the pool-base slot on both systems -- asserted where they
    // are read -- so this one extent covers them too.
    size_t HeaderExtent() const {
        uint32_t hi = countOff;
        if (mapOff > hi) hi = mapOff;
        if (poolOff > hi) hi = poolOff;
        if (revMapOff > hi) hi = revMapOff;
        return static_cast<size_t>(hi) + sizeof(void*);
    }

    // The CS header, proved and guarded. disc_.cs is a raw pointer into the LEVEL game space, which
    // a map teardown frees without changing anything the bind keyed on, and this is read per tick:
    // it goes through the cache for the same reason every other per-tick span in this file does.
    const uint8_t* ProvedCs() const {
        if (!cs) return nullptr;
        const size_t extent = HeaderExtent();
        const uint8_t* proved = csSpan_.Prove(cs, extent);
        return (proved && TryTouchSpan(proved, extent)) ? proved : nullptr;
    }

    // ElementAt through the cache: the element is addressed off the base the proof was taken over,
    // never off a base read after it, and the load still goes through the guard.
    uint8_t* ProvedElementAt(size_t index) const {
        const uint8_t* header = ProvedCs();
        void* base = header ? Deref(header, poolOff) : nullptr;
        if (!base) return nullptr;
        const size_t extent = static_cast<size_t>(stride) * (index + 1u);
        const uint8_t* proved = poolSpan_.Prove(base, extent);
        if (!proved) return nullptr;
        uint8_t* elem = const_cast<uint8_t*>(proved) + static_cast<size_t>(stride) * index;
        return TryTouchSpan(elem, stride) ? elem : nullptr;
    }

    mutable SpanCache csSpan_;
    mutable SpanCache mapSpan_;
    mutable SpanCache poolSpan_;
};

// ---------------------------------------------------------------------------------------
// THE LOCAL PAWN.
//
// There is no SR15RemotePlayerCI for the local player and there is no engine function that hands
// one SR15NetPlayerState back for it either: the composition exists in exactly one place,
// CR15PlayerBroadcasterCS::UpdateAfterPhysics (sub_140D6E980, named by the string beside its
// registration at 0x140D34806), and its only exit is the wire.  What that function is BUILT from
// is a handful of leaf accessors, and those are callable.  So this reads the same sources the
// engine reads, in the same order, and fills the fields it can source outright.
//
// The engine's own answer to "who is local" is the broadcaster's compact pool: one 24-byte
// element per player this machine authors, user id at +4, actor GUID in the parallel key array at
// CS+0x108.  That is the list UpdateAfterPhysics walks, so it includes the local human AND any
// bots a host owns, which is exactly the set whose pose no wire message will ever deliver to us.
namespace lp = replay::localpawn_facts;

// CTransfQ as the accessors write it: quaternion, position, scale.
struct Transform { float quat[4]; float pos[3]; float scale[3]; float pad[2]; };
static_assert(sizeof(Transform) >= lp::kTransformSize, "CTransfQ is 48 bytes");

using FindComponentFn = void* (*)(void* cs, void* out, uint64_t actorLo, uint64_t actorHi);
using TransformFn     = void* (*)(void* cs, void* out, uint16_t handle);
using HandTransformFn = void* (*)(void* cs, void* out, uint16_t handle, uint32_t arm);
using RefVelFn        = const float* (*)(void* cs, uint16_t handle);
// PHYS.ANGVEL_FROM_CLOUD. The convention is read off the call site at 0x140D7EF6A, not assumed:
//   lea rdx,[out] / mov rcx,body / call / movsd xmm0,[rax] / mov eax,[rax+8]
// -- the 12-byte result travels through the caller's buffer in rdx and comes back in rax.
using AngVelCloudFn   = const float* (*)(void* body, float* out3);
using AnchorFn        = uint32_t (*)(void* solver, uint64_t constraintId);
using HeldPlayerFn    = const uint32_t* (*)(void* out, void* netGame, const void* holderId, uint32_t arm);
using UserActorFn     = const void* (*)(void* netGame, void* out16, uint16_t slot);

// A component system whose elements are addressed the unshifted way -- handle map at 0xC8,
// element base at 0xF8.  Both CR15EchoUnitCS and CR15BodyCS are this shape; the broadcaster is
// not, which is why it uses Pool above with the PlayerCS offsets instead.
struct LeafPool {
    void*    cs     = nullptr;
    uint32_t stride = 0;

    // echo_'s stride is LOCAL.ECHOUNIT_CI_STRIDE, PC_PRESENT/U since W0-2's merge, so the
    // element address is tested rather than trusted -- see the note on At/Rd above, and on
    // SpanCache for why the test is no longer a syscall per element.
    void* Element(uint16_t handle) const {
        if (!cs) return nullptr;
        const void* map = Deref(cs, lp::kCsHandleMapOff);
        void* base = Deref(cs, lp::kCsPoolBaseOff);
        if (!map || !base) return nullptr;
        const size_t index = Rd<uint16_t>(map, static_cast<uint32_t>(4u * handle));
        // The element is addressed off the base the proof was taken over, never off a base read
        // after it: the two are the same pointer whenever the cache is valid, and when they are
        // not the same pointer the cache is not valid.
        const uint8_t* proved = span_.Prove(base, static_cast<size_t>(stride) * (index + 1u));
        if (!proved) return nullptr;
        uint8_t* elem = const_cast<uint8_t*>(proved) + static_cast<size_t>(stride) * index;
        return TryTouchSpan(elem, stride) ? elem : nullptr;
    }

    mutable SpanCache span_;
};

// ---------------------------------------------------------------------------------------
// HP1's synthetic owner is a CHOICE, not a measurement, so it is derived here rather than
// carried as a fact: 0xFFFE0010 appears nowhere in echovr.exe and the registry's immediate
// guard is right to refuse it. What IS measured is the bound it exploits -- both
// IsLocallyControlled and PeerTime return early for a low word >= the user-slot table size --
// so the choice is written as that fact plus a high word deliberately clear of the 0xFFFF
// sentinel both functions test. If the bound is ever re-measured, this follows it.
inline constexpr uint32_t kSyntheticOwner =
    (0xFFFEu << 16) | facts::ENTITY_USER_SLOT_TABLE_MAX;

// The pose-warp sequence is the ownership sequence's nibble shifted up four. Expressing it that
// way keeps one measured mask in the file instead of two, and the relationship is the thing
// UPDATE_TARGET_STATE actually encodes: it compares (x >> 4) & 0xF through the same comparator
// that 0x140D20FB0 applies to (x & 0xF).
inline constexpr uint16_t kWarpNibbleMask =
    static_cast<uint16_t>(facts::PHYS_CI_OWNERSEQ_NIBBLE_MASK << 4);

// The part of the node's SR15NetPhysicsSimState header the RECORDING owns: the 11 wire bits with
// both nibbles removed, so bits 8-10 -- among them the 0x200 the remote-holder gate tests.
inline constexpr uint16_t kNodeStateRecordedMask = static_cast<uint16_t>(
    0x7FFu & ~(facts::PHYS_CI_OWNERSEQ_NIBBLE_MASK | (facts::PHYS_CI_OWNERSEQ_NIBBLE_MASK << 4)));

// `playback.disc_net_touch_held`, defined here because the publish path needs it. Off leaves the
// node's 0x200 exactly as bits11 records it, which is every build before DISC-RING-4.
bool g_discNodeHeld = false;

// Read-only counters behind the same key. See entity::NodeHeldDiag.
replay::entity::NodeHeldDiag g_nodeDiag;

// No instruction in the image encodes the two hold-user-id offsets as a displacement: the engine
// writes them as part of one 16-byte store at the action word and reads them back only through
// EmitChangedDelegates, which indexes them as state[21] and state[22] against state[20] -- the
// action word. The registry's anchor guard therefore refuses standalone rows for them, correctly,
// and they are derived here from the offset that IS anchored, in the same relationship.
inline constexpr uint32_t kHoldLOff = facts::PLAYER_CI_ACTION_WORD_OFF + 4u;
inline constexpr uint32_t kHoldROff = facts::PLAYER_CI_ACTION_WORD_OFF + 8u;

// ---------------------------------------------------------------------------------------
// trigL, trigR and anchor, derived the same way and for the same reason.
//
// The raw block at PlayerCI+60 IS the 108-byte SR15NetPlayerState the wire carries, so a field's
// CI offset is the raw base plus its offset inside that struct. The three below sit at struct +76,
// +78 and +104, established from the ARM64 wire encoder SR15NetPlayerState::BitPack (libr15
// 0x21CAD80), which reads this+38*2 and this+39*2 and packs each as a real in [0,1] to 0.1, then
// reads this+26*4 and packs it as a 3-bit word guarded by `< 5`. No PC instruction encodes those
// three displacements, so there is nothing for an anchor to decode -- exactly the situation the
// two hold ids are already in.
//
// What makes the derivation checkable rather than asserted is that NINE other fields of the same
// struct ARE anchored, independently, from the CI side. If the raw base or the struct layout were
// wrong, those nine would not line up. The static_asserts below are that alignment, spelled out:
// break any one of the registry offsets and this file stops compiling.
inline constexpr uint32_t kRawBase = facts::PLAYER_RAW_HMD_ORI_OFF;

static_assert(kRawBase +  16u == facts::PLAYER_RAW_HANDOFF_A_OFF,  "raw block: lHandOff at struct+16");
static_assert(kRawBase +  22u == facts::PLAYER_RAW_HANDOFF_B_OFF,  "raw block: rHandOff at struct+22");
static_assert(kRawBase +  28u == facts::PLAYER_RAW_HAND_ORI_A_OFF, "raw block: lHandOri at struct+28");
static_assert(kRawBase +  44u == facts::PLAYER_RAW_HAND_ORI_B_OFF, "raw block: rHandOri at struct+44");
static_assert(kRawBase +  60u == facts::PLAYER_RAW_REFVEL_OFF,     "raw block: refVel at struct+60");
static_assert(kRawBase +  80u == facts::PLAYER_CI_ACTION_WORD_OFF, "raw block: bits28 at struct+80");
static_assert(kRawBase +  92u == facts::PLAYER_RAW_BEND_BACK_OFF,  "raw block: bendBack at struct+92");
static_assert(kRawBase +  96u == facts::PLAYER_RAW_BEND_FWD_OFF,   "raw block: bendFwd at struct+96");
static_assert(kRawBase + 100u == facts::PLAYER_RAW_RESTPOSE_OFF,   "raw block: restPose at struct+100");

inline constexpr uint32_t kTrigLOff  = kRawBase + 76u;   // PlayerCI+0x88, IEEE binary16
inline constexpr uint32_t kTrigROff  = kRawBase + 78u;   // PlayerCI+0x8A, IEEE binary16
inline constexpr uint32_t kAnchorOff = kRawBase + 104u;  // PlayerCI+0xA4, 3 bits used

// The synthetic owner we restore FROM. ClearSyntheticOwner puts back the exact dword that was
// there rather than writing SR15NetUserId::kInvalid: this packet never read kInvalid's value out
// of the binary, and a constant nobody measured is not one to invent.
// Read out of sub_140CED3E0 / sub_140CED640 / sub_140113A90, which is the whole of the chain the
// two band gates walk: physCs[16] is the host, host+8 its owner object, and sub_140113A90 is one
// load at +0x8518 that lands on the CR15NetGame carrying the per-slot tables at +0x138/+0x13A.
inline constexpr uint32_t kPhysCsHostOff      = 128u;
inline constexpr uint32_t kHostOwnerOff       = 8u;
inline constexpr uint32_t kOwnerNetGameOff    = facts::GAME_NETGAME_PTR_OFF;   // 0x8518
inline constexpr uint32_t kRemoteBandEndOff   = 220u;     // one past the last remote slot
inline constexpr uint32_t kRemoteBandBeginOff = 224u;     // the first remote slot
inline constexpr uint32_t kListHeadNodeOff    = 16u;
inline constexpr uint16_t kCiSimulatedBit     = 0x4000u;  // the local arm's last term  // facts-ok: SESSION.FLAG_BIT_ACTIVE PC_PRESENT, so gen_facts emits no value; 0x4000 here is the CI simulated bit
inline constexpr uint16_t kNodeHeldBit        = 0x0200u;  // the remote arm's last term

// CR15FrisbeeCS::IsHeld reads `(*(u16*)(PhysCI+6) >> 1) & 1`, and the engine's own writer of it is
// the ownership-transfer handler at 0x140D2A387 (`mov [r8+6],cx`), which is why the other fifteen
// bits are preserved here rather than assumed to be spare.
inline constexpr uint32_t kPhysCiHeldOff = 6u;       // PHYS.CI_HELD_OFF  // facts-ok: PHYS.CI_HELD_OFF an unmerged pending drop, so gen_facts emits no constant for it yet
inline constexpr uint16_t kPhysCiHeldBit = 0x0002u;  // PHYS.CI_HELD_BIT  // facts-ok: PHYS.CI_HELD_BIT an unmerged pending drop, so gen_facts emits no constant for it yet

struct OwnerSave {
    uint16_t handle;
    uint16_t ciIndex;   // the element the claim was stamped into, not just the handle it came from
    uint8_t  pool;      // and which pool that index is dense against -- the disc's is not the players'
    uint32_t original;
    // The disc's touch-state pair travels in the SAME entry as the owner dword so one release
    // hands back everything one body had taken. `touched` is what says the two below are a saved
    // original rather than a zero nobody wrote.
    bool     touched = false;
    uint16_t origHeld = 0;
    uint32_t origPrevOwner = 0;
};

class Entity final : public IEntity {
public:
    // `injectedFind` is null on every shipping path; entity.h's EntityCreateWithLookup explains
    // the one caller that is not.
    bool Init(void* gameSpace, FindCsFn injectedFind, const replay::entity::EngineEntryPoints* injected);

    // ---- PhysCI ------------------------------------------------------------------
    size_t PhysCount() const override { return phys_.Count(); }

    bool IsLocallyControlled(PhysHandle h) const override {
        // Ask the engine's own predicate rather than keeping a second copy of it. A
        // reimplementation would be a duplicate of a five-branch test that decides whether the
        // solver or our recording owns every body, and the two copies would drift silently.
        const Pool& pl = PoolOf(h);
        return isLocal_ && pl.cs && isLocal_(pl.cs, h.v) != 0;
    }

    void SetSyntheticOwner(PhysHandle h) override { StampOwner(h, /*decoupled=*/false); }

    // The recorded remote entrant's stamp: the phys CI the engine already drives for this handle
    // (ResolveCi(25) -> ElementAt(2)), resolved with only the ci[+0].entityId==handle field compare
    // dropped -- the reverse-map inverse guard the engine relies on stays. DIAG-POSE-DECOUPLE 4b;
    // scoped to that surface only.
    void SetSyntheticOwnerDriven(PhysHandle h) override { StampOwner(h, /*decoupled=*/true); }

    uint32_t SyntheticOwnerValue() const override { return kSyntheticOwner; }

    // The disc's recorded holder. Same store, same gate, same OwnerSave -- only the value differs,
    // and Remember() keeps the FIRST original, so republishing a new level never overwrites the
    // engine's own owner with one of ours.
    bool SetRecordedOwner(PhysHandle h, uint32_t owner) override {
        return StampOwner(h, /*decoupled=*/false, owner);
    }

    // {u16 slot, u16 generation} out of the two per-slot tables on CR15NetGame -- the same pair
    // sub_140D09210 validates an owner against at netGame+0x138 / +0x13A, stride 4.
    bool UserIdOf(UserIndex u, uint32_t& out) const override {
        if (u.v >= facts::ENTITY_USER_SLOT_TABLE_MAX) return false;
        void* netGame = bcastCs_ ? Deref(bcastCs_, lp::kBroadcasterNetGameOff) : nullptr;
        if (!netGame) return false;
        const uint32_t slotOff = facts::IDENT_SLOT_TABLE_OFF + 4u * u.v;
        const uint32_t genOff  = facts::IDENT_GEN_TABLE_OFF + 4u * u.v;
        if (!replay::Readable(At(netGame, genOff), sizeof(uint16_t))) return false;
        const uint16_t live = Rd<uint16_t>(netGame, slotOff);
        const uint16_t gen  = Rd<uint16_t>(netGame, genOff);
        // Both sentinels the gates reject: an unoccupied slot, and the null generation.
        if (live == 0xFFFFu || gen == 0xFFFFu) return false;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        out = (uint32_t)u.v | ((uint32_t)gen << 16);
        return true;
    }

    // Both band gates, re-derived from their decompiles and walked READ-ONLY. sub_140CED3E0 and
    // sub_140CED640 differ only in the band they demand and in their last term, and both reach the
    // CR15NetGame as *(*(*(physCs+128) + 8) + 0x8518) -- the engine's own pointer for this pool,
    // not the one this plugin bound, so a disagreement between them is itself visible here.
    bool EngineHolderOf(PhysHandle h, HolderReading& out) const override {
        out = HolderReading();
        const Pool& pl = PoolOf(h);
        uint16_t index = 0, high = 0;
        if (!pl.cs || !pl.MapEntry(h.v, index, high)) return false;
        uint8_t* const ci = pl.ProvedElementAt(index);
        uint16_t ciFlags = 0;
        if (!ci || !TryLoad(At(ci, facts::PHYS_CI_OWNER_OFF), out.owner) ||
            !TryLoad(At(ci, facts::PHYS_CI_FLAGS_OFF), ciFlags)) {
            return false;
        }
        void* const host = replay::Readable(At(pl.cs, kPhysCsHostOff), sizeof(void*))
                               ? Deref(pl.cs, kPhysCsHostOff) : nullptr;
        void* const owner = host && replay::Readable(At(host, kHostOwnerOff), sizeof(void*))
                                ? Deref(host, kHostOwnerOff) : nullptr;
        void* const netGame = owner && replay::Readable(At(owner, kOwnerNetGameOff), sizeof(void*))
                                  ? Deref(owner, kOwnerNetGameOff) : nullptr;
        if (!netGame || !replay::Readable(At(netGame, kRemoteBandBeginOff), sizeof(uint16_t))) return false;
        out.read = true;
        out.band_hi = Rd<uint16_t>(netGame, kRemoteBandEndOff);
        out.band_lo = Rd<uint16_t>(netGame, kRemoteBandBeginOff);
        out.sim_bit = (ciFlags & kCiSimulatedBit) != 0;

        const uint16_t slot = static_cast<uint16_t>(out.owner);
        const uint16_t gen  = static_cast<uint16_t>(out.owner >> 16);
        if (slot >= facts::ENTITY_USER_SLOT_TABLE_MAX || gen == 0xFFFFu) return true;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        if (!replay::Readable(At(netGame, facts::IDENT_GEN_TABLE_OFF + 4u * slot), sizeof(uint16_t)))
            return true;
        out.live_gen  = Rd<uint16_t>(netGame, facts::IDENT_GEN_TABLE_OFF + 4u * slot);
        out.live_slot = Rd<uint16_t>(netGame, facts::IDENT_SLOT_TABLE_OFF + 4u * slot);
        out.gen_ok = (out.live_gen == gen);
        if (!out.gen_ok || out.live_slot == 0xFFFFu) return true;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids

        if (out.live_slot < out.band_lo) {
            out.local_arm = out.sim_bit;
        } else if (out.live_slot < out.band_hi) {
            // PhysCI+0x158 -> +16 is the head node the remote gate reads 0x200 off; an empty list
            // leaves held_bit false, which is the "no grab event reached this body" answer.
            void* const list = replay::Readable(At(ci, facts::PHYS_CI_LIST_HEAD_OFF), sizeof(void*))
                                   ? Deref(ci, facts::PHYS_CI_LIST_HEAD_OFF) : nullptr;
            void* const head = list && replay::Readable(At(list, kListHeadNodeOff), sizeof(void*))
                                   ? Deref(list, kListHeadNodeOff) : nullptr;
            if (head && replay::Readable(At(head, facts::PHYS_NODE_STATE_OFF), sizeof(uint16_t))) {
                out.node_state = Rd<uint16_t>(head, facts::PHYS_NODE_STATE_OFF);
                out.node_read = true;
                out.held_bit = (out.node_state & kNodeHeldBit) != 0;
            }
            out.remote_arm = out.held_bit;
        }
        out.held = out.local_arm || out.remote_arm;
        return true;
    }

    bool StampOwner(PhysHandle h, bool decoupled, uint32_t owner = kSyntheticOwner) {
        // A single dword store, and it may never become a call to CR15NetPhysicsCS::SetOwner:
        // that function broadcasts an ownership transfer, frees every history node whose
        // node+128 owner differs, clears PhysCI+320 / +304 and rewrites both nibbles of
        // PhysCI+4. Each of those on its own defeats the replay. See W1-E.md §2.1.
        //
        // Through the SAME gate the install passes: this stamps a body as driven from outside, and
        // stamping an element the identity gate declines is the same fault as installing a pose
        // into one. The element and the index come from one resolution (AUD-W2-B-POSEREAD major 2).
        // `decoupled` is the remote-replay slot surface: the engine drives ElementAt(2) by pool index
        // regardless, so dropping the ci[+0] field compare (reverse-map guard kept) stamps that same
        // element the engine drives rather than refusing it.
        replay::CiPath from = replay::CiPath::NotAsked;
        uint16_t ciIndex = 0;
        BodyReading r;
        if (!ResolveVouched(h, from, ciIndex, r, decoupled)) return false;
        // Re-addressed off the pool's PROVED base. The disc's element lives in the level game
        // space, which a teardown frees, and this is a store into engine memory on the game's
        // thread: the proof and the guard are what turn that into a refusal.
        uint8_t* ci = PoolOf(h).ProvedElementAt(ciIndex);
        uint32_t current = 0;
        if (!ci || !TryLoad(At(ci, facts::PHYS_CI_OWNER_OFF), current)) return false;
        if (current == owner) return true;   // the level is already published: no store, no save
        if (!TryStore(At(ci, facts::PHYS_CI_OWNER_OFF), owner)) return false;
        Remember(h, ciIndex, current);
        return true;
    }

    // The disc's touch-state pair, through the SAME gate, the same proved element and the same
    // save as the owner dword above. Two plain stores; never SetOwner, never the transfer handler.
    bool SetDiscTouchState(PhysHandle h, bool held, uint32_t lastOwner) override {
        replay::CiPath from = replay::CiPath::NotAsked;
        uint16_t ciIndex = 0;
        BodyReading r;
        if (!ResolveVouched(h, from, ciIndex, r, /*decoupled=*/false)) return false;
        uint8_t* ci = PoolOf(h).ProvedElementAt(ciIndex);
        uint16_t flags = 0;
        uint32_t prev = 0;
        if (!ci || !TryLoad(At(ci, kPhysCiHeldOff), flags) ||
            !TryLoad(At(ci, facts::PHYS_CI_PREV_OWNER_OFF), prev)) {
            return false;
        }
        const uint16_t wantFlags =
            (uint16_t)((flags & (uint16_t)~kPhysCiHeldBit) | (held ? kPhysCiHeldBit : 0u));
        if (wantFlags == flags && prev == lastOwner) return true;   // the level is already published
        // Saved BEFORE the first store and only once, so a body reclaimed twice still hands back
        // what the engine had rather than what we last wrote.
        RememberTouch(h, ciIndex, flags, prev);
        if (wantFlags != flags && !TryStore(At(ci, kPhysCiHeldOff), wantFlags)) return false;
        if (prev != lastOwner && !TryStore(At(ci, facts::PHYS_CI_PREV_OWNER_OFF), lastOwner))
            return false;
        return true;
    }

    // The remote holder arm's last term, published as a level. The store lands on the newest
    // history node -- the one sub_140CED640 dereferences -- and the level is kept so PushHistory
    // stamps every later node the same way; without that, the bit lives exactly one node.
    bool SetRecordedNodeHeld(PhysHandle h, bool held) override {
        if (!::g_discNodeHeld) return false;
        nodeHeldHandle_ = h.v;
        nodeHeldPool_ = h.pool;
        nodeHeldValue_ = held;
        nodeHeldSet_ = true;
        ++::g_nodeDiag.calls;
        ::g_nodeDiag.lastHeld = held;
        replay::CiPath from = replay::CiPath::NotAsked;
        uint16_t ciIndex = 0;
        BodyReading r;
        // The SAME gate the install that created the node passed: the driven surface drops only
        // the ci[+0] field compare, and stamping a different element from the one just written is
        // the one way this level can be applied and still read back missing.
        if (!ResolveVouched(h, from, ciIndex, r, /*decoupled=*/true)) {
            ++::g_nodeDiag.resolveFail;
            return false;
        }
        uint8_t* ci = PoolOf(h).ProvedElementAt(ciIndex);
        return ci && StampNodeHeld(ci, held);
    }

    // Shared by the level publish and by every push, so the two can never disagree about which
    // word or which bit. The head is absent on a body the plugin has not pushed for yet, which is
    // not a fault: the next push carries the level in.
    bool StampNodeHeld(void* ci, bool held) const {
        void* const list = replay::Readable(At(ci, facts::PHYS_CI_HIST_LIST_OFF), sizeof(void*))
                               ? Deref(ci, facts::PHYS_CI_HIST_LIST_OFF) : nullptr;
        void* const head = list && replay::Readable(At(list, facts::PHYS_LIST_HEAD_PTR_OFF), sizeof(void*))
                               ? Deref(list, facts::PHYS_LIST_HEAD_PTR_OFF) : nullptr;
        uint16_t state = 0;
        if (!head || !TryLoad(At(head, facts::PHYS_NODE_STATE_OFF), state)) {
            ++::g_nodeDiag.headNull;
            ::g_nodeDiag.histCount = Rd<uint64_t>(ci, facts::PHYS_CI_HIST_COUNT_OFF);
            return false;
        }
        const uint16_t want =
            (uint16_t)((state & (uint16_t)~kNodeHeldBit) | (held ? kNodeHeldBit : 0u));
        ++::g_nodeDiag.stamped;
        ::g_nodeDiag.histCount = Rd<uint64_t>(ci, facts::PHYS_CI_HIST_COUNT_OFF);
        ::g_nodeDiag.lastState = want;
        if (want == state) return true;
        return TryStore(At(head, facts::PHYS_NODE_STATE_OFF), want);
    }

    // NOT gated, deliberately: a claim that was made must be releasable even if the gate has since
    // stopped vouching for the element, or the body is left permanently net-driven with nothing
    // driving it. The element is addressed by the INDEX the claim recorded, so the release lands
    // on the object the claim stamped whichever arm resolved it.
    void ClearSyntheticOwner(PhysHandle h) override { Release(h, /*restore=*/true); }

    // The component is gone: drop the save so teardown does not write a dead entity's owner into
    // whatever the engine recycled into its pool slot.
    void ForgetSyntheticOwner(PhysHandle h) override { Release(h, /*restore=*/false); }

    void Release(PhysHandle h, bool restore) {
        for (uint32_t i = 0; i < savedCount_; ++i) {
            if (saved_[i].handle != h.v || saved_[i].pool != h.pool) continue;
            if (restore) {
                if (uint8_t* ci = PoolOf(h).ProvedElementAt(saved_[i].ciIndex)) {
                    TryStore(At(ci, facts::PHYS_CI_OWNER_OFF), saved_[i].original);
                    if (saved_[i].touched) {
                        TryStore(At(ci, kPhysCiHeldOff), saved_[i].origHeld);
                        TryStore(At(ci, facts::PHYS_CI_PREV_OWNER_OFF), saved_[i].origPrevOwner);
                    }
                }
            }
            saved_[i] = saved_[--savedCount_];
            return;
        }
    }

    bool ReadBody(PhysHandle h, BodyState& out) const override;
    bool ReadBodyDetailed(PhysHandle h, BodyState& out, BodyReading& r) const override;
    bool ReadBodyDetailedDriven(PhysHandle h, BodyState& out, BodyReading& r) const override;
    // The body both share; `decoupled` drops only the ci[+0] field compare in the identity gate.
    bool ReadBodyDetailedImpl(PhysHandle h, BodyState& out, BodyReading& r, bool decoupled) const;
    const uint8_t* FindSpaceRegistryEntry(void* space, uint64_t symbol, BodyReading& r) const;
    bool  CsHasComponent(void* cs, uint16_t entityId, SpanCache& probe) const;
    void* FindPhysicsCsForEntity(uint16_t entityId, BodyReading& r) const;
    const void* ResolveCi(PhysHandle h, uint16_t& ciIndex, replay::CiPath& from, BodyReading& r) const;
    bool ProveByUniqueIndex(uint16_t namingId, uint16_t ciIndex, const uint8_t* revProved,
                            size_t live, BodyReading& r) const;
    bool CheckCiIdentity(PhysHandle h, uint16_t ciIndex, replay::CiPath from, const void* ci,
                         BodyReading& r, bool dropCiFieldCompare = false) const;
    bool ReadLocalBody(const Pool& pl, const void* ci, BodyState& out, BodyReading& r) const;
    bool ReadLocalTransform(const Pool& pl, const void* ci, BodyState& out, BodyReading& r) const;
    bool AnswerFromTransform(const Pool& pl, const void* ci, bool poolWalked, BodyState& out,
                             BodyReading& r) const;
    bool ReadNodeChainTransform(const void* node, float quat[4], float pos[3], BodyReading& r) const;
    void PushHistory(PhysHandle h, const BodyState& s, uint64_t tUs) override;
    void TrimHistory(PhysHandle h, uint64_t keepFromUs, uint64_t keepToUs) override;
    bool SetHistoryWindow (PhysHandle h, const BodyState* samples,
                           const uint64_t* timesUs, size_t n) override;
    bool SetHistoryWindowDetailed(PhysHandle h, const BodyState* samples,
                                  const uint64_t* timesUs, size_t n,
                                  replay::InstallReading& ir) override;
    bool SetHistoryWindowDriven(PhysHandle h, const BodyState* samples,
                                const uint64_t* timesUs, size_t n,
                                replay::InstallReading& ir) override;
    // The body both share; `decoupled` picks bounds-only resolution for the remote-replay surface.
    bool InstallHistoryWindow(PhysHandle h, const BodyState* samples,
                              const uint64_t* timesUs, size_t n,
                              replay::InstallReading& ir, bool decoupled);
    // The ONE resolution every write and every counted read shares. ResolveCi's two arms address
    // DIFFERENT elements -- Handle takes ElementAt(handleMap[4v]), Pool takes ElementAt(v) -- so a
    // caller that gates on one and then re-resolves inside the write vouches for one object and
    // writes to another (AUD-W2-B-POSEREAD blocker 1). Take the element from here; never re-resolve.
    // `decoupled` keeps the reverse-map inverse guard but drops the ci[+0].entityId==v field compare,
    // so the remote-replay surface vouches for the element the engine drives (DIAG-POSE-DECOUPLE 4a).
    void* ResolveVouched(PhysHandle h, replay::CiPath& from, uint16_t& ciIndex, BodyReading& r,
                         bool decoupled = false) const;
    void PushHistoryInto(PhysHandle h, const Pool& pl, void* ci, void* container,
                         const BodyState& s, uint64_t tUs);
    size_t ReadHistoryWindow(PhysHandle h, BodyState* samples,
                             uint64_t* timesUs, size_t cap) const override;
    size_t ReadHistoryWindowDriven(PhysHandle h, BodyState* samples,
                                   uint64_t* timesUs, size_t cap) const override;
    // The body both share; `decoupled` picks the reverse-map-only resolve for the remote surface.
    size_t ReadHistoryWindowImpl(PhysHandle h, BodyState* samples,
                                 uint64_t* timesUs, size_t cap, bool decoupled) const;

    uint64_t   NetIdOf(PhysHandle h) const override;
    PhysHandle PhysOfNetId(uint64_t netId) const override;
    PhysHandle PhysOfComponentId(uint32_t componentId, ComponentIdReading* seen) const override;
    // The same read over an arbitrary pool: the disc's id is only ever valid against the disc's.
    PhysHandle ComponentIdIn(const Pool& pl, uint8_t poolTag, uint32_t componentId,
                             ComponentIdReading* seen) const;
    bool BindDisc(void* netGame, DiscBind& out) override;
    bool DiscChainStands(void* netGame, DiscChainReading* seen) const override;
    bool DiscBoundEarly() const override { return disc_.cs != nullptr && discEarly_; }
    const char* DiscEarlyRefusal() const override { return discEarlyRefusal_; }
    const DiscBind::EarlyDiag* DiscEarlyDiagnostics() const override {
        return discEarlyDiag_.n != 0 ? &discEarlyDiag_ : nullptr;
    }
    const char* DiscEarlySpaceName() const override {
        const char* const n = discEarlySpace_ < DiscBind::EarlyDiag::kCandidates
                                  ? discEarlyDiag_.cand[discEarlySpace_].name
                                  : nullptr;
        return n ? n : "none";
    }
    void DropDisc() override { disc_ = Pool(); discEarly_ = false; discDisagreeStreak_ = 0; }
    bool DiscStands(uint32_t physId) const override {
        return disc_.cs && ComponentIdIn(disc_, PhysHandle::kPoolDisc, physId, nullptr).v != 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    }

    float GetTimeOffset(PhysHandle h) const override {
        const void* ci = PoolOf(h).Element(h.v);
        return ci ? Rd<float>(ci, facts::PHYS_CI_TIME_OFFSET_S_OFF) : 0.0f;
    }

    void SetTimeOffset(PhysHandle h, float seconds) override {
        if (void* ci = PoolOf(h).Element(h.v)) Wr<float>(ci, facts::PHYS_CI_TIME_OFFSET_S_OFF, seconds);
    }

    // ---- PlayerCI ----------------------------------------------------------------
    size_t PlayerCount() const override { return player_.Count(); }
    bool   ReadPlayer(PlayerHandle h, PlayerState& out) const override;
    void   PushPlayerHistory(PlayerHandle h, const PlayerState& s, uint64_t tUs) override;

    PhysHandle PhysOf(PlayerHandle h) const override {
        const void* ci = player_.Element(h.v);
        // PlayerCI+28 is a component HANDLE into the physics pool. On PhysCI the same offset is
        // the owner id. That is §2 F11, and it is why these two types do not convert.
        return PhysHandle(ci ? Rd<uint16_t>(ci, facts::PLAYER_CI_HANDLE_OFF) : 0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    }

    PlayerHandle PlayerOf(UserIndex u) const override;

    // ---- the local pawn ------------------------------------------------------------
    size_t    LocalPlayerCount() const override { return BroadcastCount(); }
    UserIndex LocalPlayerAt(size_t index) const override;

    // Both sources, read together, and neither trusted to be the only one.  The broadcaster pool
    // answers "who does this machine author": exactly one authored player is one local user and
    // the engine has already said which.  It is silent on the launch configuration this exists
    // for -- an `-offline` host authors the human AND every bot -- so netGame's own local-user
    // slot is read too, and when only it answers, it is the answer.  When both answer and they
    // DISAGREE the reading is Ambiguous with both numbers kept: one of the two is describing a
    // different session than the caller thinks, and picking either would publish under the wrong
    // id.  The pool id is read straight out of the element rather than through LocalPlayerAt,
    // whose 0xFFFF miss value would then have to be told apart from a user index -- and index 0 is
    // precisely the value this accessor exists to deliver intact.
    LocalUser ReadLocalUser(LocalUserReading& r) const override {
        r = LocalUserReading();
        if (!Available()) return r.state;   // no resolve, so neither source has a base to read

        const size_t n = bcastCs_ ? BroadcastCount() : 0u;
        r.poolCount = n;
        if (bcastCs_ && n == 1) {
            if (const void* ci = BroadcastElement(0)) {
                r.pool = UserIndex(static_cast<uint16_t>(Rd<uint32_t>(ci, lp::kBroadcasterUserIdOff)));
                r.poolOk = true;
            }
        }
        uint16_t slot = 0;
        if (NetGameLocalSlot(slot)) {
            r.slot = UserIndex(slot);
            r.slotOk = true;
        }

        if (r.poolOk && r.slotOk) {
            if (r.pool.v != r.slot.v) {
                r.state = LocalUser::Ambiguous;   // and r keeps both readings, so the log can name them
                return r.state;
            }
            r.index = r.pool;
            r.state = LocalUser::Ok;
        } else if (r.poolOk) {
            r.index = r.pool;
            r.state = LocalUser::Ok;
        } else if (r.slotOk) {
            r.index = r.slot;
            r.state = LocalUser::Ok;
        } else if (!bcastCs_ || n == 1) {
            // No pool at all, or a pool that named one player and then would not hand it over:
            // "could not ask", which is not "authors nobody" and is certainly not an index.
            r.state = LocalUser::NotAvailable;
        } else {
            r.state = (n == 0) ? LocalUser::None : LocalUser::Ambiguous;
        }
        return r.state;
    }

    LocalUser LocalUserIndex(UserIndex& out) const override {
        LocalUserReading r;
        if (ReadLocalUser(r) != LocalUser::Ok) return r.state;
        out = r.index;
        return LocalUser::Ok;
    }
    uint32_t  ReadLocalPlayer(UserIndex u, PlayerState& out) const override;
    uint32_t  ReadLocalPlayerDetailed(UserIndex u, PlayerState& out,
                                     replay::IEntity::LocalPlayerReading& r) const override;

    bool ModelVisible(UserIndex u, bool& visible) const override;
    uint16_t ModelComponentHandle(UserIndex u) const override;
    bool ModelSystemResolved() const override;
    uint16_t EchoUnitComponentHandle(UserIndex u) const override;
    bool ProbeEchoUnitBind(UserIndex u, replay::IEntity::EchoBind& out) const override;
    uint32_t ComponentCensus(replay::IEntity::CensusRow* out, uint32_t cap) const override;
    uint32_t ActorPoolCensus(replay::IEntity::ActorPoolRow* out, uint32_t cap) const override;
    bool ActorPoolFor(uint64_t prefabId, replay::IEntity::ActorPoolRow& out) const override;
    uint32_t SystemCensus(const uint16_t* classes, uint32_t nClasses,
                          replay::IEntity::SystemRow* out, uint32_t cap) const override;
    bool RemoteTrackProbe_(UserIndex u, replay::IEntity::RemoteTrackProbe& out) const override;
    bool ReadDiscRing(replay::IEntity::DiscRing& out) const override;
    bool ScriptSelfCensus(replay::IEntity::ScriptCensus& io) const override;
    bool FindScriptGraph(void* space, uint64_t hash, uint64_t actorLo, uint64_t actorHi,
                         replay::IEntity::ScriptGraphRef& out, const char** why) override;
    bool ScriptGraphValid(const replay::IEntity::ScriptGraphRef& r) override;
    bool ScriptGetReal(const replay::IEntity::ScriptGraphRef& r, uint64_t name, float& out) override;
    bool ScriptSetReal(const replay::IEntity::ScriptGraphRef& r, uint64_t name, uint64_t id,
                       float v) override;
    bool ScriptDataWord(const replay::IEntity::ScriptGraphRef& r, uint32_t word,
                        int32_t& out) override;
    bool ScriptSendEvent(const replay::IEntity::ScriptGraphRef& r, uint64_t name,
                         uint64_t component) override;
    bool AudioMixCensus(replay::IEntity::AudioMix& out) const override;
    bool NetTouchCensus(replay::IEntity::NetTouchReading& out) const override;
    bool DiscRingCensus(replay::IEntity::DiscRingReading& out) const override;
    uint32_t DiscCandidateSpaces(void* netGame, void** out, uint32_t cap) const override;
    bool ActorIndexOfSymbol(void* space, uint64_t symbol, uint16_t& index, bool& live,
                            replay::QueriedSpans& spans) const;
    void ScanCandidateSpaces(replay::IEntity::DiscRingReading& out) const;
    void ScanSpaceSystems(void* space, replay::IEntity::DiscRingReading& out,
                          replay::QueriedSpans& spans) const;
    // The one construction of the SCompactPoolHandle the delegate emit sends, so a probe of where
    // those delegates land measures the handle the feed uses and not a second copy of it.
    HandleBuild BuildDelegateHandle(PlayerHandle h, uint32_t& compact, uint16_t& index) const;
    bool ReadActorPool(uint16_t classId, replay::IEntity::ActorPoolRow& out) const;
    // The 24-byte stride of the engine's own actor index; `written` in, total out, so the three
    // pools fill one array without the caller tracking a cursor.
    static constexpr uint32_t kCensusStride = 24u;
    uint32_t CensusPool_(void* cs, replay::IEntity::CensusPool pool, bool withPos,
                         replay::IEntity::CensusRow* out, uint32_t cap, uint32_t written) const;
    bool BroadcastIndexOf(UserIndex u, size_t& out) const;
    bool BroadcastKeyActor(UserIndex u, uint8_t actor[lp::kActorGuidSize]) const;
    replay::IEntity::LocalPawn LocalPawnAt(UserIndex u,
                                           replay::IEntity::LocalPawnRef& out) const override;
    replay::IEntity::EntityKey EntityKeyOf(UserIndex u, replay::IEntity::PawnSurface surface,
                                           uint16_t& out) const override;
    replay::IEntity::EntityKey EntityKeyOfDriven(PhysHandle h, uint16_t& out) const override;
    replay::IEntity::PhysVouch VouchPhysForUser(UserIndex u) const override;

    void SetLodThrottle(bool enabled) override {
        // R6 §8.5's throttle: with the enable dword zero the round-robin block is skipped
        // entirely and every remote body updates at full rate, which is what a free-camera
        // replay wants. All three offsets were re-read on PC; R6 had them only from ARM64.
        if (player_.cs) Wr<uint32_t>(player_.cs, facts::PLAYER_CS_LOD_ENABLE_OFF, enabled ? 1u : 0u);
    }

    bool ClaimLodThrottle(bool enabled) override {
        void* const cs = player_.cs;
        if (!cs) return false;
        if (!lodClaimed_ || lodClaimedCs_ != cs) {
            // Read before the first write, and only through an address the process will give us:
            // the value put back has to be the engine's own, not a guess at what it must have been.
            if (!replay::Readable(At(cs, facts::PLAYER_CS_LOD_ENABLE_OFF), sizeof(uint32_t))) {
                return false;
            }
            lodOriginal_  = Rd<uint32_t>(cs, facts::PLAYER_CS_LOD_ENABLE_OFF);
            lodClaimedCs_ = cs;
            lodClaimed_   = true;
        }
        Wr<uint32_t>(cs, facts::PLAYER_CS_LOD_ENABLE_OFF, enabled ? 1u : 0u);
        return true;
    }

    bool ReleaseLodThrottle() override {
        if (!lodClaimed_) return false;
        // A rebound component system is a different object: the saved value describes the old one
        // and writing it into the new one would be the clobber this pair exists to prevent.
        const bool sameSurface = lodClaimedCs_ == player_.cs && player_.cs != nullptr;
        lodClaimed_ = false;
        if (!sameSurface) return false;
        Wr<uint32_t>(player_.cs, facts::PLAYER_CS_LOD_ENABLE_OFF, lodOriginal_);
        return true;
    }

    bool        Available()  const override { return resolve_ == replay::entity::Resolve::Ok; }
    const char* Diagnostic() const override { return replay::entity::ResolveText(resolve_); }

    BindEpoch BindEpochNow(uint64_t& out) const override {
        if (!g_bindCurrent) return BindEpoch::Unbound;
        out = replay::entity::BindEpochCounter();
        return BindEpoch::Ok;
    }

    // Used by the publisher, which lives in this translation unit, and by E-8, which needs the
    // component SYSTEM rather than a component for the LOD triple.
    void* PlayerElement(PlayerHandle h) const { return player_.Element(h.v); }
    void* RemotePlayerCs() const { return player_.cs; }
    void  EmitDelegates(PlayerHandle h, const void* before, const void* after) const;

    // The net-actor table, or nulls. One resolution shared by both directions, because the two
    // differ only in which half of an entry they match on.
    const void* NetIdTable(size_t& count) const {
        count = 0;
        const void* netActorCs = phys_.cs ? Deref(phys_.cs, lp::kPhysCsNetActorOff) : nullptr;
        if (!netActorCs) return nullptr;
        const void* table = Deref(netActorCs, lp::kNetActorTableOff);
        if (!table) return nullptr;
        const uint64_t n = Rd<uint64_t>(netActorCs, lp::kNetActorCountOff);
        // The count is a qword the engine keeps; a corrupted one must not turn a read into a
        // walk off the end of the heap on the tick thread.
        if (n == 0 || n > kMaxNetActors) return nullptr;
        // WORLD.NETACTOR_TABLE_ENTRY_STRIDE is PC_PRESENT/U. NetIdOf and PhysOfNetId both walk n
        // entries at that stride, so the span they will cover is tested once, here, rather than
        // per entry in two loops that would then have to agree about the bound.
        //
        // AUDIT_K1 open item 3: Readable is a VirtualQuery, and capture/gather.h calls NetIdOf once
        // per PRESENT BODY per tick, so this was a syscall inside the sample loop against a design
        // that budgets the tick at zero. The verdict is a property of (table, n) within one
        // binding, and Init -- the only thing that changes a binding, from EntityCreate, EntityBind
        // and EntityUnbind alike -- resets these members with the rest of the object. So it is
        // remembered against the exact pair it was taken for: a table the engine moves, or a count
        // it grows, is a different span and is checked again. Nothing is remembered across a
        // refusal, so a span that fails is re-asked every call rather than latched.
        if (table != spanTable_ || n != spanCount_) {
            if (!replay::Readable(table, lp::kNetActorEntryStride * static_cast<size_t>(n)))
                return nullptr;
            spanTable_ = table;
            spanCount_ = n;
        }
        count = static_cast<size_t>(n);
        return table;
    }

    // The pair the span check above was last taken for, and the reason it need not be retaken.
    // Reset with everything else by Init's `*this = Entity()`.
    mutable const void* spanTable_ = nullptr;
    mutable uint64_t    spanCount_ = 0;

    static constexpr uint64_t kMaxNetActors = 4096;

    // Retire every node on the list, including the last one. TrimHistory deliberately keeps one
    // sample because a body with no history stops moving; SetHistoryWindow refills in the same
    // call, on the same thread, before anything can observe the gap.
    void ClearHistory(const Pool& pl, void* ci, void* container) {
        for (uint32_t guard = 0; guard < 4096u; ++guard) {
            void* tail = Deref(container, facts::PHYS_LIST_TAIL_PTR_OFF);
            if (!tail) return;
            recycle_(pl.cs, ci, tail);
        }
    }


private:
public:
    // Teardown. Only the owners we actually replaced are restored, and by their saved dword --
    // a body left with a synthetic owner is permanently net-driven with nothing driving it.
    void RestoreAll() {
        while (savedCount_) {
            const OwnerSave e = saved_[savedCount_ - 1];
            if (void* ci = PoolTagged(e.pool).ElementAt(e.ciIndex)) {
                Wr<uint32_t>(ci, facts::PHYS_CI_OWNER_OFF, e.original);
                if (e.touched) {
                    Wr<uint16_t>(ci, kPhysCiHeldOff, e.origHeld);
                    Wr<uint32_t>(ci, facts::PHYS_CI_PREV_OWNER_OFF, e.origPrevOwner);
                }
            }
            --savedCount_;
        }
    }

private:
    void Remember(PhysHandle h, uint16_t ciIndex, uint32_t original) {
        for (uint32_t i = 0; i < savedCount_; ++i)
            if (saved_[i].handle == h.v && saved_[i].pool == h.pool) return;
        if (savedCount_ < kMaxSaved)
            saved_[savedCount_++] = OwnerSave{h.v, ciIndex, h.pool, original};
    }

    // The touch-state half of the same entry. An entry may already exist because the owner stamp
    // made one; in that case only the touch originals are filled, and only the first time.
    void RememberTouch(PhysHandle h, uint16_t ciIndex, uint16_t flags, uint32_t prevOwner) {
        for (uint32_t i = 0; i < savedCount_; ++i) {
            if (saved_[i].handle != h.v || saved_[i].pool != h.pool) continue;
            if (saved_[i].touched) return;
            saved_[i].touched = true;
            saved_[i].origHeld = flags;
            saved_[i].origPrevOwner = prevOwner;
            return;
        }
        if (savedCount_ >= kMaxSaved) return;
        // No owner stamp on this body yet. The owner original recorded here is the one already on
        // the element, so a later release restores a value nobody replaced.
        OwnerSave e{h.v, ciIndex, h.pool, 0};
        if (uint8_t* ci = PoolOf(h).ProvedElementAt(ciIndex))
            TryLoad(At(ci, facts::PHYS_CI_OWNER_OFF), e.original);
        e.touched = true;
        e.origHeld = flags;
        e.origPrevOwner = prevOwner;
        saved_[savedCount_++] = e;
    }

    // One entry per body we take over. The engine's own pools are the size of a match, and 64
    // is past the 22 networked slots plus 8 players with room to spare; overflowing it means we
    // simply do not restore, which is strictly safer than a heap allocation on the tick thread.
    static constexpr uint32_t kMaxSaved = 64;

    Pool phys_;
    Pool player_;
    // The LOD enable dword this module borrowed, and from which component system. Keyed on the CS
    // pointer so a rebind cannot make the release write a stale value into a fresh object.
    bool     lodClaimed_   = false;
    void*    lodClaimedCs_ = nullptr;
    uint32_t lodOriginal_  = 0;
    // The disc's net-physics system, bound by BindDisc out of the rules block. It is a DIFFERENT
    // CR15NetPhysicsCS instance from phys_ whenever the frisbee lives in the level space, so it
    // carries its own base, its own maps and its own handle namespace. Empty until BindDisc lands.
    Pool disc_;
    // Set by the early path, and everything it needs to re-walk its own chain at 2 Hz. `mutable`
    // because the re-validation is const and hands the drive over to the engine's ref the moment
    // that ref resolves to the same body: an agreement is not a change of what we drive.
    mutable bool     discEarly_       = false;
    void*            discLevelSpace_  = nullptr;   // *(netGame + 0x2B10) as it was at bind time
    // The netGame the last bind attempt was handed, kept so a diagnostic can re-derive the
    // candidate spaces the bind itself walked. Never dereferenced off the bind's own cadence.
    void*            discNetGame_     = nullptr;
    void*            discActorSpace_  = nullptr;   // the space the game_frisbee actor was found in
    void*            discFrisbeeCs_   = nullptr;
    uint32_t         discPhysId_      = 0;
    uint16_t         discActorIndex_  = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    const char*      discEarlyRefusal_ = nullptr;
    void*            discEarlyRoot_   = nullptr;   // the candidate space the bind's walk started at
    uint8_t          discEarlySpace_  = 0;         // which candidate that was
    DiscBind::EarlyDiag discEarlyDiag_;
    // Consecutive fresh, fully resolved rules refs that named a different disc than the one being
    // driven. Two of them is the engine having really moved the disc; one is the chain flapping.
    static constexpr uint8_t kDiscDisagreeConfirm = 2;
    mutable uint8_t  discDisagreeStreak_ = 0;
    bool ResolveDiscFromRules(void* netGame, DiscBind& out, Pool& candidate) const;
    bool ResolveDiscEarly(void* netGame, DiscBind& out, Pool& candidate) const;
    bool EarlyChainStands(void* netGame) const;
    // Both read-only walks the early path is built out of. Neither calls the engine.
    void* FrisbeeActorSpace(void* levelSpace, uint16_t& actorIndex,
                            DiscBind::EarlyDiag* diag = nullptr, size_t at = 0) const;
    void* EarlySpaceRoot(void* netGame, size_t which, const char** name) const;
    bool  ActorSlotLive(void* space, uint16_t actorIndex) const;
    void* SystemInSpace(void* space, uint64_t symbol) const;

    const Pool& PoolTagged(uint8_t pool) const {
        return pool == PhysHandle::kPoolDisc ? disc_ : phys_;
    }
    const Pool& PoolOf(PhysHandle h) const { return PoolTagged(h.pool); }
    // The recorded held level and the body it belongs to. One body: the disc is the only one whose
    // holder the engine asks about, and a second entry would be a second thing to keep coherent.
    uint16_t   nodeHeldHandle_ = 0;
    uint8_t    nodeHeldPool_ = 0;
    bool       nodeHeldSet_ = false;
    bool       nodeHeldValue_ = false;
    OwnerSave saved_[kMaxSaved]{};
    uint32_t  savedCount_ = 0;

    IsLocalFn    isLocal_ = nullptr;
    UnlinkFn     unlink_  = nullptr;
    RecycleFn    recycle_ = nullptr;
    EmitDeltasFn emit_    = nullptr;
    // The CPhBody chain is REPRODUCED rather than called -- localpawn_facts.h carries the reason
    // and the instruction sites -- so what is bound for it is a guard verdict and one data slot.
    bool               bodyChainOk_   = false;
    const void* const* phSystemSlot_  = nullptr;
    const void*        phSystemFixed_ = nullptr;

    // Re-read every time. The engine fills that global after startup and replaces it on a level
    // change, so a CPhSystem cached at bind time can outlive its object.
    const void* PhSystem() const {
        if (phSystemFixed_) return phSystemFixed_;
        if (!phSystemSlot_) return nullptr;
        void* slot = const_cast<void*>(static_cast<const void*>(phSystemSlot_));
        const uint8_t* proved = ProveSpan(phSystemSpan_, slot, sizeof(void*));
        if (!proved || !TryTouchSpan(proved, sizeof(void*))) return nullptr;
        return *phSystemSlot_;
    }

    // One replay::Readable per (base, extent, bind epoch) for every span the body path touches.
    // L2-RUN-10 cost-killed a live capture at +74.5 s because ReadLocalBody asked the OS five
    // times per body per tick: gather went 13.8 us to 242-277 us and was charged 328 us against a
    // 200 us rung, with the body encoder never entered -- the whole cost was the refusal path.
    // clock.cpp's contract for Readable is "once per take -- never on the tick path", and this is
    // what holds it. A proof is a property of the span, so a pool that moves, a slot past the
    // proved extent, and a rebind all re-prove; the LOAD still goes through TryTouchSpan, which
    // is the half that sees a page unmapped after the proof was taken.
    const uint8_t* ProveSpan(SpanCache& cache, void* base, size_t bytes) const {
#if defined(W1E_MUTATE_BODY_SPAN_UNCACHED)
        // Mutation control: the pre-fix shape, one VirtualQuery per span per body per tick.
        (void)cache;
        return replay::Readable(base, bytes) ? static_cast<const uint8_t*>(base) : nullptr;
#else
        return cache.Prove(base, bytes);
#endif
    }

    // The first term of PrimaryBody's slot condition -- localpawn_facts.h carries the five
    // instructions. An absent or unreadable state object means the bound STANDS: refusing is the
    // safe answer on the owner's game thread, and claiming the bypass off a pointer we could not
    // read would be indexing an unbounded pool on a guess.
    // Three-valued, because Clear and Unreadable gate the same way and diagnose oppositely:
    // Clear says the engine's own PrimaryBody would refuse this slot too, Unreadable says we
    // never got to ask. Only Set lifts the bound, so an unreadable state object still leaves the
    // bound STANDING -- the caller collapses the tri-state, this does not.
    BypassRead ReadSlotBoundBypass(const void* physCs) const {
#if defined(W1E_MUTATE_BODY_SLOT_BOUND)
        (void)physCs;   // Mutation control: the pre-fix shape, an unconditional bound.
        return BypassRead::Clear;
#else
#if defined(W1E_MUTATE_DIAG_BYPASS_TWO_STATE)
        // Mutation control: collapse "could not ask" into "asked, and the bit was clear" -- the
        // two-valued shape the reader had before the diag, and the shape that made L2 #11's
        // "the bit is CLEAR" unfalsifiable.
        const BypassRead kNotRead = BypassRead::Clear;
#else
        const BypassRead kNotRead = BypassRead::Unreadable;
#endif
        void* state = Deref(physCs, lp::kPhysicsCsStateObjOff);
        if (!state) return kNotRead;
        const size_t extent = lp::kPhysicsCsStateFlagsOff + sizeof(uint8_t);
        const uint8_t* proved = ProveSpan(stateSpan_, state, extent);
        if (!proved || !TryTouchSpan(proved, extent)) return kNotRead;
        return (Rd<uint8_t>(proved, lp::kPhysicsCsStateFlagsOff) &
                lp::kPhysicsCsSlotBoundBypassBit) != 0
                   ? BypassRead::Set
                   : BypassRead::Clear;
#endif
    }


    mutable SpanCache phSystemSpan_;
    mutable SpanCache stateSpan_;
    mutable SpanCache mapSpan_;
    mutable SpanCache poolSpan_;
    mutable SpanCache revSpan_;
    mutable SpanCache zoneSpan_;
    mutable SpanCache bodySpan_;
    mutable SpanCache xformMapSpan_;
    mutable SpanCache xformPoolSpan_;
    // Re-proved per node: a parent chain is a chain of unrelated addresses, so this cache holds
    // the one being read rather than a span that covers them all.
    mutable SpanCache nodeSpan_;
    mutable SpanCache nodeOwnerSpan_;
    mutable SpanCache nodeEntrySpan_;

    replay::entity::Resolve resolve_ = replay::entity::Resolve::NoGameSpace;

    // ---- the local pawn ------------------------------------------------------------
public:
    const char* LocalPawnText() const { return replay::entity::ResolveText(localResolve_); }

private:
    void InitLocalPawn(void* gameSpace, FindCsFn find, const replay::entity::EngineEntryPoints* injected);

    // The broadcaster's pool is walked DENSELY, by index, the way UpdateAfterPhysics walks it --
    // not through the handle map. The engine keeps it compact, and the element and its key share
    // the index, so a handle indirection here would be a second way to say the same thing and a
    // second way to get it wrong.
    size_t BroadcastCount() const {
        return bcastCs_ ? Rd<uint16_t>(bcastCs_, facts::PLAYER_CS_COUNT_OFF) : 0u;
    }
    // CR15NetGame's own "my user slot", the second source ENG-W1E-R2 S9 named as missing.  Four
    // conditions, each of which is a refusal and never a zero: the netGame must be readable at
    // all; it must hold EXACTLY one local user (none means this machine has no player, more than
    // one means record 0 does not name a single human); the slot must be inside the 16-entry table
    // both owner consumers bound-check; and the slot table must map that slot BACK to record 0.
    //
    // Record 0 IS the local user, and it is AddPlayerUser that makes it so: its insert chain ends
    // with a literal UserSwap(this, 0, newRecord) (libr15 0x1253758) and logs "idx 0" one line
    // earlier.  AddBotUser (0x12547EC), AddRemoteUser (0x1256674) and AddReplayUser (0x125828C)
    // stop at the +0xDC / +0xE0 / +0xE2 partition boundaries, every one of which AddPlayerUser has
    // already incremented, so a non-player lands at record >= 1 once a player exists.
    //
    // The last condition is NOT what establishes that, and must not be read as doing so: UserSwap
    // (0x124E004) exchanges the +0x138 back-map entries in LOCKSTEP with the record contents, so
    // after a hypothetical UserSwap(0, k) the table would map record 0's new slot to 0 and the
    // cross-check would pass while naming the wrong user.  What it does catch is a torn,
    // uninitialised or wrong-object netGame, whose back-map does not agree with the record it is
    // read beside -- a corruption check worth keeping, not a test of the convention.
    bool NetGameLocalSlot(uint16_t& out) const {
        if (!bcastCs_) return false;
        void* netGame = Deref(bcastCs_, lp::kBroadcasterNetGameOff);
        if (!netGame || !replay::Readable(netGame, lp::kNetGameUserRecordOff + sizeof(uint16_t)))
            return false;
        if (Rd<uint16_t>(netGame, lp::kNetGameLocalUserCountOff) != 1u) return false;
        const uint16_t slot = static_cast<uint16_t>(
            Rd<uint16_t>(netGame, lp::kNetGameUserRecordOff) & lp::kNetGameUserSlotMask);
        if (slot >= facts::ENTITY_USER_SLOT_TABLE_MAX) return false;
        // LOCAL.NETGAME_SLOT_TABLE_OFF and _STRIDE are both PC_PRESENT/U, and the bound above
        // bounds the INDEX, not the byte the stride turns it into.
        //
        // With TODAY's numbers this cannot refuse and that is stated rather than left to be
        // discovered: the whole table ends at 0x138 + 4*15 + 2 = 0x176, below the 0x17A the check
        // at the top of the function has already cleared, so it is subsumed. It stays because the
        // two facts it is made of are UNCERTAIN -- a stride or base that moves puts the far end of
        // the table past the record group, and then it is the only thing between a wrong fact and
        // a fault on the tick thread. tests/entity pins the subsumption so the day it stops
        // holding is a red test rather than a silent widening.
        const uint32_t backMapOff = lp::kNetGameSlotTableOff + lp::kNetGameSlotTableStride * slot;
        if (!replay::Readable(At(netGame, backMapOff), sizeof(uint16_t))) return false;
        if (Rd<uint16_t>(netGame, backMapOff) != 0u)
            return false;
        out = slot;
        return true;
    }

    // LOCAL.BROADCASTER_CI_STRIDE is PC_PRESENT/U: a wrong stride walks off the compact pool
    // instead of naming a player, so the element is tested before it is handed out.
    const void* BroadcastElement(size_t i) const {
        void* base = bcastCs_ ? Deref(bcastCs_, facts::PLAYER_CS_POOL_BASE_OFF) : nullptr;
        if (!base) return nullptr;
        const uint8_t* proved =
            bcastSpan_.Prove(base, static_cast<size_t>(lp::kBroadcasterCiStride) * (i + 1u));
        if (!proved) return nullptr;
        const uint8_t* elem = proved + static_cast<size_t>(lp::kBroadcasterCiStride) * i;
        return TryTouchSpan(elem, lp::kBroadcasterCiStride) ? elem : nullptr;
    }
    const uint8_t* LocalActorGuid(UserIndex u, size_t poolIndex,
                                  replay::IEntity::LocalPlayerReading& r) const;
    // 0xFFFF when the actor does not carry the component. FirstActorComponent writes exactly that
    // into the low word when its sorted actor table has no entry, so no separate validity call is
    // needed -- and the second half of the GUID being -1 means "this game space", which is the
    // substitution the engine's own wrapper performs.
    uint16_t ComponentOnActor(void* cs, const void* actorGuid) const;
    uint16_t FindComponent(void* cs, uint64_t actorId) const;
    uint16_t ModelHandleFor(UserIndex u, uint8_t actor[replay::localpawn_facts::kActorGuidSize]) const;

    void*     gameSpace_ = nullptr;
    void*     bcastCs_ = nullptr;
    LeafPool  echo_;
    LeafPool  body_;
    LeafPool  touch_;

    FindComponentFn findComp_ = nullptr;
    TransformFn     baseXf_   = nullptr;
    TransformFn     headXf_   = nullptr;
    HandTransformFn handXf_   = nullptr;
    RefVelFn        refVel_   = nullptr;
    AngVelCloudFn   angVelFn_ = nullptr;
    AnchorFn        anchor_   = nullptr;
    HeldPlayerFn    heldBy_   = nullptr;
    UserActorFn     userActor_ = nullptr;
    LeafPool        model_;

    replay::entity::Resolve localResolve_ = replay::entity::Resolve::NoGameSpace;

    // The local pawn's actor, resolved ONCE per binding rather than once per frame. Init() rebuilds
    // this object, so a GUID minted against the old game space cannot outlive it.
    mutable uint8_t  localActor_[lp::kActorGuidSize] = {};
    // Meaningful only while localActorSource_ is not None, which is what makes it a resolved
    // cache rather than a sentinel comparison.
    mutable uint16_t localActorSlot_ = 0;
    mutable replay::IEntity::ActorSource localActorSource_ = replay::IEntity::ActorSource::None;

    // The two pools that are not a LeafPool: the broadcaster's compact pool and CModelCS's shifted
    // one. Init's `*this = Entity()` drops all three caches with the rest of the object.
    mutable SpanCache bcastSpan_;
    mutable SpanCache modelSpan_;
    // The ncaPhysics CS search.  Three separate caches because the three spans are three
    // different objects, and one cache walked by a loop would miss on every candidate.
    mutable SpanCache csRegistrySpan_;
    mutable SpanCache csListSpan_;
    // One span proof per LIST POSITION, not one shared by every candidate: the walk asks about
    // a different handle map at each position, so a single cache is evicted by the next candidate
    // and every entry pays a Readable call on every body on every tick -- F19 of L2-RUN-10, the
    // cost that killed a live capture. The positions a tick touches are stable (the engine's own
    // array order), so a proof per position stands exactly as long as the other proofs do.
    static constexpr uint64_t kCsProbeSlots = 4;
    mutable SpanCache csProbeSpan_[kCsProbeSlots];
};

// LOCAL.ACTOR_GUID_SIZE bytes of zero is the UNSET SActorGUID, not an actor: handing it to
// FirstActorComponent asks a sorted actor table for a key no player owns, and the 0xFFFF that
// comes back is indistinguishable from "this actor has no such component".
bool AllZero(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (p[i]) return false;
    }
    return true;
}

bool Entity::Init(void* gameSpace, FindCsFn injectedFind, const replay::entity::EngineEntryPoints* injected) {
    // A second Init is a rebuilt game space (level load). Nothing resolved against the old one may
    // survive -- least of all the saved owners, whose handles would index the NEW pool at RestoreAll.
    *this = Entity();
    if (!gameSpace) { resolve_ = replay::entity::Resolve::NoGameSpace; return false; }

    // The engine's lookup guards on the table's count and indexes its base unguarded, so the table is
    // asked of the memory manager before the first call into the engine, whoever the caller is.
    // `gameSpace` must be the CncaGameSpace at NETGAME.CS_SPACE_OFF, which is the object this check
    // is a check ON: the refusal recorded against it for 240 s was not an unbuilt space but the
    // CR15Game at IDENT.NETGAME_GAMESPACE_OFF read as one, and that object's +0x420 is a
    // render-target budget block, so it refuses CapacityOverBlock forever (ARCH-10 section 1).
    const replay::tableguard::TableCheck table = replay::tableguard::CheckSpaceTable(
        gameSpace, lp::kGameSpaceSystemTableOff, lp::kSystemTableStride, false);
    if (table.verdict != replay::tableguard::TableVerdict::Ready || (table.raw.flags & lp::kSystemTableUnsorted)) {
        resolve_ = replay::entity::Resolve::SpaceNotReady;
        return false;
    }

    FindCsFn find = injectedFind;
    if (!find) {
        if (!PrologueOk(facts::CS_LOOKUP_BY_SYMBOL, facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE)) {
            resolve_ = replay::entity::Resolve::NoPhysicsCs;
            return false;
        }
        find = reinterpret_cast<FindCsFn>(const_cast<void*>(Mapped(facts::CS_LOOKUP_BY_SYMBOL)));
    }

    phys_.cs        = find(gameSpace, facts::CS_SYM_R14NETPHYSICS);
    phys_.stride    = facts::PHYS_CI_STRIDE;
    phys_.mapOff    = facts::PHYS_CS_HANDLE_MAP_OFF;
    phys_.poolOff   = facts::PHYS_CS_POOL_BASE_OFF;
    phys_.countOff  = facts::PHYS_CS_COUNT_OFF;
    phys_.revMapOff = facts::PHYS_CS_REVERSE_MAP_OFF;
    if (!phys_.cs) { resolve_ = replay::entity::Resolve::NoPhysicsCs; return false; }

    player_.cs        = find(gameSpace, facts::CS_SYM_R14REMOTEPLAYER);
    player_.stride    = facts::PLAYER_CI_STRIDE;
    player_.mapOff    = facts::PLAYER_CS_HANDLE_MAP_OFF;
    player_.poolOff   = facts::PLAYER_CS_POOL_BASE_OFF;
    player_.countOff  = facts::PLAYER_CS_COUNT_OFF;
    player_.revMapOff = facts::PLAYER_CS_REVERSE_MAP_OFF;
    if (!player_.cs) { resolve_ = replay::entity::Resolve::NoRemotePlayerCs; return false; }

    if (!phys_.Ready() || !player_.Ready()) {
        resolve_ = replay::entity::Resolve::PoolNotReady;
        return false;
    }
    if (PrologueOk(facts::ENTITY_IS_LOCALLY_CONTROLLED, facts::ENTITY_IS_LOCALLY_CONTROLLED_PROLOGUE))
        isLocal_ = reinterpret_cast<IsLocalFn>(const_cast<void*>(Mapped(facts::ENTITY_IS_LOCALLY_CONTROLLED)));
    if (PrologueOk(facts::ENTITY_LIST_UNLINK, facts::ENTITY_LIST_UNLINK_PROLOGUE))
        unlink_ = reinterpret_cast<UnlinkFn>(const_cast<void*>(Mapped(facts::ENTITY_LIST_UNLINK)));
    if (PrologueOk(facts::ENTITY_HISTORY_RECYCLE, facts::ENTITY_HISTORY_RECYCLE_PROLOGUE))
        recycle_ = reinterpret_cast<RecycleFn>(const_cast<void*>(Mapped(facts::ENTITY_HISTORY_RECYCLE)));
    // Test-only, and the same seam `find` and `phSystem` are: with no image mapped both
    // guards above refuse, so the history ring is unwritable on a test host and no fixture
    // could reach the element the install gates. Nothing shipping passes a non-null
    // EngineEntryPoints, so the guarded resolution above is still the only one taken.
    if (injected && injected->historyUnlink)
        unlink_ = reinterpret_cast<UnlinkFn>(injected->historyUnlink);
    if (injected && injected->historyRecycle)
        recycle_ = reinterpret_cast<RecycleFn>(injected->historyRecycle);
    if (PrologueOk(facts::ENTITY_EMIT_CHANGED_DELEGATES, facts::ENTITY_EMIT_CHANGED_DELEGATES_PROLOGUE))
        emit_ = reinterpret_cast<EmitDeltasFn>(const_cast<void*>(Mapped(facts::ENTITY_EMIT_CHANGED_DELEGATES)));

    // The CPhBody chain. NEITHER half is called: PHYS.PRIMARY_BODY_PC returns a 24-byte aggregate
    // and facts/facts.tsv says outright that which register carries `this` versus the hidden
    // return slot is an ABI question the disassembly does not settle. ReadLocalBody walks both
    // function bodies instead. The prologue guards still gate that walk, because every
    // displacement in it was decoded out of these two functions and a build that moved them
    // invalidates a reproduction exactly as it would invalidate a call.
    bodyChainOk_ = PrologueOk(facts::PHYS_PRIMARY_BODY_PC, facts::PHYS_PRIMARY_BODY_PC_PROLOGUE) &&
                   PrologueOk(facts::PHYS_RESOLVE_BODY_PC, facts::PHYS_RESOLVE_BODY_PC_PROLOGUE);
    phSystemSlot_ = static_cast<const void* const*>(Mapped(facts::CLOCK_THIRD_PTR));
    if (injected && injected->phSystem) {
        // A synthetic image: no echovr.exe is mapped, so the guards above cannot pass and the
        // walk would be unreachable on a test host. What this bypasses is the BUILD check and
        // never a step of the walk -- every refusal below is still reached and still asserted.
        phSystemFixed_ = injected->phSystem;
        bodyChainOk_   = true;
    }

    resolve_ = replay::entity::Resolve::Ok;

    // Deliberately last, and deliberately unable to fail this function. A capture that loses
    // ReadPlayer as well as ReadLocalPlayer is strictly worse than one that loses only the second.
    InitLocalPawn(gameSpace, find, injected);
    return true;
}

void Entity::InitLocalPawn(void* gameSpace, FindCsFn find, const replay::entity::EngineEntryPoints* injected) {
    gameSpace_ = gameSpace;

    // PHYS.ANGVEL_FROM_CLOUD resolves FIRST, above every `return` in this function. It is a
    // standalone engine entry point that takes a CPhBody and nothing else: it has no relationship
    // to the broadcaster, echo-unit or body component systems, nor to FirstActorComponent, and
    // each of those four failures returns. ReadLocalBody still runs when they refuse -- a host
    // that cannot find a component system can still measure a body's angular velocity, and
    // resolving here is what makes that true instead of silently writing NotStoredByEngine.
    if (injected && injected->physAngVelFromCloud)
        angVelFn_ = reinterpret_cast<AngVelCloudFn>(injected->physAngVelFromCloud);
    else if (PrologueOkN(lp::kPhysAngVelFromCloud, lp::kPhysAngVelFromCloudPrologue, lp::kGuardLen))
        angVelFn_ = reinterpret_cast<AngVelCloudFn>(const_cast<void*>(Mapped(lp::kPhysAngVelFromCloud)));

    bcastCs_ = find(gameSpace, lp::kSymPlayerBroadcaster);
    if (!bcastCs_) { localResolve_ = replay::entity::Resolve::NoBroadcasterCs; return; }

    echo_.cs     = find(gameSpace, lp::kSymEchoUnit);
    echo_.stride = lp::kEchoCiStride;
    if (!echo_.cs) { localResolve_ = replay::entity::Resolve::NoEchoUnitCs; return; }

    // LOCAL.SYM_R14BODY is PC_PRESENT/U. It is a lookup KEY and never an address: a wrong hash
    // matches no component system, find returns null, and the line below refuses. Nothing is
    // dereferenced from it, so there is no bound to check.
    body_.cs     = find(gameSpace, lp::kSymBody);
    body_.stride = lp::kBodyCiStride;
    if (!body_.cs) { localResolve_ = replay::entity::Resolve::NoBodyCs; return; }

    // FirstActorComponent forwards through the OBJECT's own vtable, so one guarded address serves
    // every component system. The vtable slot is checked against that address rather than trusted:
    // the slot index says where to look and the bytes say what is there, and neither alone is an
    // identification.
    if (injected && injected->findActorComponent) {
        findComp_ = reinterpret_cast<FindComponentFn>(injected->findActorComponent);
    } else {
        if (!PrologueOkN(lp::kFindActorComponent, lp::kFindActorComponentPrologue, lp::kGuardLen)) {
            localResolve_ = replay::entity::Resolve::NoFindComponent;
            return;
        }
        const void* want = Mapped(lp::kFindActorComponent);
        void* const systems[2] = {echo_.cs, body_.cs};
        for (void* cs : systems) {
            const void* vtbl = Deref(cs, 0);
            if (!vtbl || Rd<const void*>(vtbl, lp::kFindActorComponentSlotOff) != want) {
                localResolve_ = replay::entity::Resolve::NoFindComponent;
                return;
            }
        }
        findComp_ = reinterpret_cast<FindComponentFn>(const_cast<void*>(want));
    }

    if (injected && injected->echoBaseTransform)
        baseXf_ = reinterpret_cast<TransformFn>(injected->echoBaseTransform);
    else if (PrologueOkN(lp::kEchoBaseTransform, lp::kEchoBaseTransformPrologue, lp::kGuardLen))
        baseXf_ = reinterpret_cast<TransformFn>(const_cast<void*>(Mapped(lp::kEchoBaseTransform)));
    if (injected && injected->echoHeadTransform)
        headXf_ = reinterpret_cast<TransformFn>(injected->echoHeadTransform);
    else if (PrologueOkN(lp::kEchoHeadTransform, lp::kEchoHeadTransformPrologue, lp::kGuardLen))
        headXf_ = reinterpret_cast<TransformFn>(const_cast<void*>(Mapped(lp::kEchoHeadTransform)));
    if (injected && injected->echoHandTransform)
        handXf_ = reinterpret_cast<HandTransformFn>(injected->echoHandTransform);
    else if (PrologueOkN(lp::kEchoHandTransform, lp::kEchoHandTransformPrologue, lp::kGuardLen))
        handXf_ = reinterpret_cast<HandTransformFn>(const_cast<void*>(Mapped(lp::kEchoHandTransform)));
    if (injected && injected->echoReferenceVel)
        refVel_ = reinterpret_cast<RefVelFn>(injected->echoReferenceVel);
    else if (PrologueOkN(lp::kEchoReferenceVel, lp::kEchoReferenceVelPrologue, lp::kGuardLen))
        refVel_ = reinterpret_cast<RefVelFn>(const_cast<void*>(Mapped(lp::kEchoReferenceVel)));
    if (injected && injected->anchorBodyPart)
        anchor_ = reinterpret_cast<AnchorFn>(injected->anchorBodyPart);
    else if (PrologueOkN(lp::kAnchorBodyPart, lp::kAnchorBodyPartPrologue, lp::kGuardLenLong))
        anchor_ = reinterpret_cast<AnchorFn>(const_cast<void*>(Mapped(lp::kAnchorBodyPart)));
    if (injected && injected->heldPlayer)
        heldBy_ = reinterpret_cast<HeldPlayerFn>(injected->heldPlayer);
    else if (PrologueOkN(lp::kHeldPlayer, lp::kHeldPlayerPrologue, lp::kGuardLen))
        heldBy_ = reinterpret_cast<HeldPlayerFn>(const_cast<void*>(Mapped(lp::kHeldPlayer)));
    if (injected && injected->userActor)
        userActor_ = reinterpret_cast<UserActorFn>(injected->userActor);
    else if (PrologueOkN(lp::kUserActor, lp::kUserActorPrologue, lp::kGuardLen))
        userActor_ = reinterpret_cast<UserActorFn>(const_cast<void*>(Mapped(lp::kUserActor)));

    // CModelCS uses the SHIFTED field group, so LeafPool's 0xC8/0xF8 do not read it. Its offsets are
    // its own facts, from its own getter, rather than the physics pool's numbers reused.
    // VIS.SYM_NCAMODEL is PC_PRESENT/U, and like the other two hashes it is a key, not an
    // address -- a wrong one leaves model_.cs null and visibility simply unavailable.
    model_.cs     = find(gameSpace, lp::kSymNcaModel);
    model_.stride = lp::kModelCiStride;

    // The trigger blend is the one local-pawn source that is NOT required: the broadcaster reaches
    // it through a component handle cached on the system rather than through the entry's actor, so
    // whether it sits on the same actor is a live question. Resolving it on the actor here makes
    // the wrong answer VISIBLE -- FirstActorComponent returns 0xFFFF and kLocalTrigger stays clear
    // -- instead of a plausible number from another player's hands.
    // LOCAL.SYM_R14TOUCHANIMBLENDERS is PC_PRESENT/U; key, not address, and this source is
    // already the optional one, so a wrong hash lands in the visible-failure case described above.
    touch_.cs     = find(gameSpace, lp::kSymTouchAnimBlenders);
    touch_.stride = lp::kTouchAnimCiStride;

    localResolve_ = replay::entity::Resolve::Ok;
}

uint16_t Entity::FindComponent(void* cs, uint64_t actorId) const {
    // The engine's own wrapper hands this a 40-byte scratch buffer even though only the first
    // dword is written. Matching that costs nothing and does not depend on a belief about how
    // much the callee touches.
    uint8_t scratch[40] = {};
    findComp_(cs, scratch, actorId, lp::kThisGameSpace);
    uint32_t id = 0;
    memcpy(&id, scratch, sizeof id);
    return static_cast<uint16_t>(id);
}

// ★ THE BIND THAT FOUR SESSIONS READ AS "THE PAWNS CARRY NO COMPONENTS".
// The stored 16 bytes are NOT (actorLo, actorHi) as this call's two arguments: the search key is
// (actor id, game space), and the game space half has to be the system's own -- see
// lp::kThisGameSpace. PRESENT-G's census settled which half of the stored GUID is the actor id:
// every user slot's second qword has the engine's (index << 16 | class) shape and appears verbatim
// in the systems' actor indices, while the first qword is one constant shared by every user in the
// session. Passing that constant as the actor id missed for EVERY actor -- including the local
// viewer's, whose EchoUnit the census shows sitting in the table it was searching.
// The other half is still tried when the first misses, and costs one lookup only on a miss: the two
// producers of a GUID here (CR15NetGame's user record and the broadcaster's key array) are
// different writers, and a miss is the engine saying "not this actor", never a wrong component.
uint16_t Entity::ComponentOnActor(void* cs, const void* actorGuid) const {
    if (!findComp_ || !cs || !actorGuid) return lp::kNoComponent;
    const uint16_t byActorHalf = FindComponent(cs, Rd<uint64_t>(actorGuid, 8));
    if (byActorHalf != lp::kNoComponent) return byActorHalf;
    return FindComponent(cs, Rd<uint64_t>(actorGuid, 0));
}

UserIndex Entity::LocalPlayerAt(size_t index) const {
    if (index >= BroadcastCount()) return UserIndex(0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    const void* ci = BroadcastElement(index);
    if (!ci) return UserIndex(0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    // The low word of SR15NetUserId is the user SLOT, which is what PlayerOf takes, so the local
    // pawn keys into the same streams as everybody else without a conversion.
    return UserIndex(static_cast<uint16_t>(Rd<uint32_t>(ci, lp::kBroadcasterUserIdOff)));
}

// TWO INDEPENDENT SOURCES for the local pawn's actor, tried in a fixed order and NAMED in the
// reading. L2 lost a whole match of player state to one: the broadcaster pool's parallel key array
// deref came back null, `if (!actor) return 0` fired, and the pose, the body, the trigger, the
// anchor and the hold ids went with it -- 976 declared-present, zero-sampled records.
//
// Order. The key array first: it is the array the engine's own broadcast loop indexes
// (`mov rax,[r13+108h]` then `movups xmm0,[rax+rcx*8]`, rcx = 2i, at 0x140D6EB0F), it costs no
// call, and it is per POOL INDEX, which is the pawn this element actually is.
// CR15NetGame::UserActor second: it answers for ANY user slot, local or remote, off the user
// record the netGame owns -- a different table from the broadcaster's, published by a different
// path. That is what a HOST needs, where the human and every bot are local pawns and nothing
// makes the key array's publication simultaneous with the element's.
//
// Resolved once per BINDING, never per frame: a resolved GUID is cached and re-used. While it is
// still unresolved the pair of lookups repeats, so an actor that appears after the bind is picked
// up within the epoch and costs nothing once it has been.
const uint8_t* Entity::LocalActorGuid(UserIndex u, size_t poolIndex,
                                      replay::IEntity::LocalPlayerReading& r) const {
    using Src = replay::IEntity::ActorSource;
    if (localActorSource_ != Src::None && localActorSlot_ == u.v) {
        r.actorSource = localActorSource_;
        return localActor_;
    }

    uint8_t fromKeys[lp::kActorGuidSize] = {};
    bool haveKeys = false;
    const void* keys = bcastCs_ ? Deref(bcastCs_, lp::kBroadcasterKeysOff) : nullptr;
    r.bcastKeys = keys;
    if (keys) {
        // BroadcastElement tested its element and this did not, so a wrong
        // LOCAL.BROADCASTER_CS_KEYS_OFF was a fault on the tick thread rather than a refusal.
        const void* g = At(const_cast<void*>(keys),
                           static_cast<uint32_t>(lp::kActorGuidSize * poolIndex));
        if (replay::Readable(g, lp::kActorGuidSize)) {
            memcpy(fromKeys, g, sizeof fromKeys);
            haveKeys = !AllZero(fromKeys, sizeof fromKeys);
        }
    }

    uint8_t fromUser[lp::kActorGuidSize] = {};
    bool haveUser = false;
    void* netGame = bcastCs_ ? Deref(bcastCs_, lp::kBroadcasterNetGameOff) : nullptr;
    r.netGame = netGame;
    // UserActor indexes the user-record array with no bound of its own -- its first instructions
    // are the multiply and the load. The slot table both owner consumers bound-check covers the
    // same 16-user set (it maps one to the other), so a slot outside it names no record and the
    // index is bounded by it; Readable bounds the byte the multiply lands on.
    if (userActor_ && netGame && u.v < facts::ENTITY_USER_SLOT_TABLE_MAX) {
        const void* rec = At(netGame, lp::kNetGameUserActorOff + lp::kNetGameUserStride * u.v);
        if (replay::Readable(rec, lp::kActorGuidSize)) {
            userActor_(netGame, fromUser, u.v);
            haveUser = !AllZero(fromUser, sizeof fromUser);
        }
    }

    r.actorSourcesDisagree =
        haveKeys && haveUser && memcmp(fromKeys, fromUser, sizeof fromKeys) != 0;

    if (haveKeys) {
        memcpy(localActor_, fromKeys, sizeof localActor_);
        localActorSource_ = Src::BroadcasterKeys;
    } else if (haveUser) {
        memcpy(localActor_, fromUser, sizeof localActor_);
        localActorSource_ = Src::NetGameUserActor;
    } else {
        r.actorSource = Src::None;
        return nullptr;
    }
    localActorSlot_ = u.v;
    r.actorSource = localActorSource_;
    return localActor_;
}

uint32_t Entity::ReadLocalPlayer(UserIndex u, PlayerState& out) const {
    replay::IEntity::LocalPlayerReading r;
    return ReadLocalPlayerDetailed(u, out, r);
}

uint32_t Entity::ReadLocalPlayerDetailed(UserIndex u, PlayerState& out,
                                         replay::IEntity::LocalPlayerReading& r) const {
    using Refusal = replay::IEntity::LocalRefusal;
    r = replay::IEntity::LocalPlayerReading();
    r.bcastCs = bcastCs_;
    r.heldByFn = heldBy_ != nullptr;
    if (!bcastCs_) {
        r.refusal = Refusal::NoBroadcaster;
        return 0;
    }

    const size_t n = BroadcastCount();
    const void* entry = nullptr;
    size_t poolIndex = 0;
    bool found = false;
    for (size_t i = 0; i < n; ++i) {
        if (LocalPlayerAt(i).v == u.v) {
            poolIndex = i;
            entry = BroadcastElement(i);
            found = true;
            break;
        }
    }
    if (!found) {
        r.refusal = Refusal::SlotNotInPool;
        return 0;
    }

    const uint8_t* actor = LocalActorGuid(u, poolIndex, r);
    if (!actor) {
        r.refusal = Refusal::ActorNull;
        return 0;
    }

    uint32_t filled = 0;

    // The hold ids are the one part of the action-word group whose source does not depend on which
    // arm of the builder a player takes: both call this against the NET GAME, with the address of
    // the element's own user-id field as the holder. They are also identity rather than animation --
    // each is an SR15NetUserId whose low word is the user slot the rest of the capture is keyed on.
    if (heldBy_ && entry) {
        if (void* netGame = Deref(bcastCs_, lp::kBroadcasterNetGameOff)) {
            const void* holder = At(const_cast<void*>(entry), lp::kBroadcasterUserIdOff);
            // The engine hands this a stack local well wider than the dword it reads back; matching
            // that costs nothing and does not rest on a belief about how much the callee touches.
            uint8_t outL[32] = {}, outR[32] = {};
            const uint32_t* l = heldBy_(outL, netGame, holder, 0);
            const uint32_t* r = heldBy_(outR, netGame, holder, 1);
            if (l && r) {
                out.holdL = *l;
                out.holdR = *r;
                filled |= kLocalHolding;
            }
        }
    }

    const uint16_t echoH = ComponentOnActor(echo_.cs, actor);
    r.echoComponent = echoH != lp::kNoComponent;
    r.echoHandle = echoH;
    if (echoH != lp::kNoComponent) {
        if (baseXf_ && headXf_ && handXf_) {
            // What the wire carries is the BASE orientation and each hand's WORLD position minus
            // the head's -- a plain difference in world axes, not a change of frame. That is what
            // UpdateAfterPhysics computes at 0x140D6F2xx, and reproducing anything more than a
            // subtraction here would be inventing a convention the engine does not use.
            Transform base{}, head{}, hand{};
            baseXf_(echo_.cs, &base, echoH);
            headXf_(echo_.cs, &head, echoH);
            memcpy(out.hmdOri, base.quat, sizeof out.hmdOri);

            handXf_(echo_.cs, &hand, echoH, 0);
            memcpy(out.lHandOri, hand.quat, sizeof out.lHandOri);
            for (uint32_t k = 0; k < 3; ++k) out.lHandOff[k] = hand.pos[k] - head.pos[k];

            handXf_(echo_.cs, &hand, echoH, 1);
            memcpy(out.rHandOri, hand.quat, sizeof out.rHandOri);
            for (uint32_t k = 0; k < 3; ++k) out.rHandOff[k] = hand.pos[k] - head.pos[k];
            filled |= kLocalPose;
        }
        if (refVel_) {
            // Returns a pointer into the attached object, or into a static zero triple when
            // nothing is attached. Never null, so the three floats are always readable.
            if (const float* v = refVel_(echo_.cs, echoH)) {
                memcpy(out.refVel, v, sizeof out.refVel);
                filled |= kLocalRefVel;
            }
        }
        if (anchor_) {
            // The EchoUnit element points at the collision object it is attached to, that object
            // carries the constraint id, and the solver is keyed on it. The builder dereferences
            // the attachment pointer unconditionally; we do not, because a null there is a crash
            // on the tick thread and 2 is the same answer the solver gives for anything it cannot
            // find -- including the id being absent, which is the common case in open play.
            out.anchor = lp::kAnchorDefault;
            const void* elem = echo_.Element(echoH);
            const void* attached = elem ? Deref(elem, lp::kEchoAttachedOff) : nullptr;
            const void* game = gameSpace_ ? Deref(gameSpace_, lp::kGameSpaceGameOff) : nullptr;
            if (attached && game) {
                const uint64_t constraint = Rd<uint64_t>(attached, lp::kAttachedConstraintOff);
                if (constraint != lp::kNoConstraint)
                    out.anchor = anchor_(At(const_cast<void*>(game), lp::kGameSolverOff), constraint);
                filled |= kLocalAnchor;
            }
        }
    }

    const uint16_t touchH = ComponentOnActor(touch_.cs, actor);
    if (touchH != lp::kNoComponent) {
        if (const void* ci = touch_.Element(touchH)) {
            // The engine converts both to fp16 on the way to the wire. E-4 keeps the float: it is
            // the same value one quantisation earlier, and the recording is ground truth rather
            // than a reconstruction of what a remote client received.
            out.trigL = Rd<float>(ci, lp::kTouchAnimTrigLOff);
            out.trigR = Rd<float>(ci, lp::kTouchAnimTrigROff);
            filled |= kLocalTrigger;
        }
    }

    const uint16_t bodyH = ComponentOnActor(body_.cs, actor);
    if (bodyH != lp::kNoComponent) {
        if (const void* ci = body_.Element(bodyH)) {
            out.bendBack       = Rd<float>(ci, lp::kBodyBendBackOff);
            out.bendFwd        = Rd<float>(ci, lp::kBodyBendFwdOff);
            out.restPoseHeight = Rd<float>(ci, lp::kBodyRestPoseOff);
            filled |= kLocalBody;
        }
    }
    r.fill = filled;
    // An actor and no group is a DIFFERENT answer from no actor, and only this separates them.
    r.refusal = filled ? Refusal::Ok : Refusal::NoSourceAnswered;
    return filled;
}

// The SimState payload lives inside the history node at PHYS_NODE_STATE_OFF, and the interpolator
// writes its output with the same field offsets less that base. Reading and writing a node
// therefore share one field table, which is why they are one function each rather than two.
// TWO-SOURCED, history first. A net-driven body carries history and the node is what the
// recording is made of; a body this machine simulates carries none, and its live state is on the
// CPhBody. The order is not a preference between two answers to one question -- at most one arm
// can answer for any given body -- and before the second arm existed a solo capture, which is
// every body on the owner's own machine, recorded nothing at all.
bool Entity::ReadBody(PhysHandle h, BodyState& out) const {
    BodyReading r;
    return ReadBodyDetailed(h, out, r);
}

// The engine's component-system bound, in one place because it is one rule.  Both of the image's
// spellings are UNSIGNED and STRICT: PrimaryBody at 0x14044CC6C is `66 3B 8A F4 00 00 00` /
// `73 2C` (cmp cx,[rdx+0F4h]; jnb -> the -1 sentinel) and HasComponent at 0x1402F7FA4 is
// `66 42 39 0C 82` / `0F 92 C0` (cmp word [rdx+r8*4], cx; setb al).  An index EQUAL to the live
// count is out of range in both -- which is the boundary L2 #12's refusal landed exactly on.
#if defined(W1E_MUTATE_BODY_SLOT_BOUND_OFF_BY_ONE)
static inline bool IndexPastCount(size_t index, size_t count) { return index > count; }
#else
static inline bool IndexPastCount(size_t index, size_t count) { return index >= count; }
#endif

// WHICH READING of the caller's u16 the engine itself can stand behind, and the CI it addresses.
// The two paths that produce a value do not mean the same thing by it:
//
//   handle -- CncaComponentSystem::HasComponent (0x1402F7FA4, `66 42 39 0C 82 / 0F 92 C0`) is
//     `cmp word [map + 4*id], count / setb`, so the forward map's low u16 IS the pool index when it
//     is inside the live count, and the entity has NO component of this system when it is not.
//   pool   -- the enumerating caller walks i in [0, PHYS.CS_COUNT_OFF) and addresses
//     PHYS.CS_POOL_BASE_OFF + PHYS.CI_STRIDE * i directly, the way ENTITY.UPDATE_TARGET_STATE does.
//     The bound is that SAME count (0xF4, the u16 the element base at 0xF8 is dense against) and the
//     walk satisfied it before it called; re-deriving an index it already has can only lose it.
//
// The forward map is asked FIRST and its answer is authoritative whenever HasComponent accepts it,
// so every refusal a handle lookup could raise is still raised. Only when the engine's own predicate
// says this id has no component -- the normal state of a locally-owned CI, which was never given a
// net handle -- is the value read as the walk's own index. That is the whole of L2 #13: handle 0,
// map entry 1, live count 1, 7062 of 7062 bodies refused before either arm ran.
//
// What this cannot prove is the CALLER's intent, and it does not pretend to: on the pool reading the
// identity below is over the index, and `ciFrom` publishes which reading answered.
const void* Entity::ResolveCi(PhysHandle h, uint16_t& ciIndex, replay::CiPath& from,
                              BodyReading& r) const {
    const Pool& pl = PoolOf(h);
    const uint16_t v = h.v;
    const size_t live = pl.Count();
    r.diag.netLive  = static_cast<uint16_t>(live);
    r.diag.identity = replay::IdentityProof::Refused;   // until one of the two proofs stands
    r.diag.filled  |= replay::kBodyDiagNetLive | replay::kBodyDiagIdentity;

    uint16_t mapLow = 0, mapHigh = 0;
    if (!pl.MapEntry(v, mapLow, mapHigh)) { r.refusedAt = BodyRefusal::NoCi; return nullptr; }
    r.diag.mapHigh = mapHigh;
    r.diag.filled |= replay::kBodyDiagMapHigh;

#if defined(W1E_MUTATE_NO_HANDLE_BOUND)
    // Mutation control: the map's index believed whatever it says. An id with no component then
    // addresses an element past the live range instead of refusing.
    const bool handleStands = true;
#else
    const bool handleStands = !IndexPastCount(mapLow, live);
#endif
#if defined(W1E_MUTATE_GATE_POOL_WALKED_CI)
    // Mutation control: the shape L2 #13 refused on -- the forward map's bound applied to a CI the
    // pool walk had already bounded, so the local pawn never reaches either arm.
    const bool poolStands = false;
#else
    const bool poolStands = !IndexPastCount(v, live);
#endif
    if (handleStands) {
        from    = replay::CiPath::Handle;
        ciIndex = mapLow;
    } else if (poolStands) {
        from    = replay::CiPath::Pool;
        ciIndex = v;
    } else {
        // Neither reading is inside the count: the map addresses no instance and the value is not
        // an index of one either. The index that was compared is published with the refusal.
        r.diag.ciIndex = mapLow;
        r.diag.filled |= replay::kBodyDiagCiIndex;
        r.refusedAt    = BodyRefusal::CiIndexNotLive;
        return nullptr;
    }
    r.diag.ciFrom  = from;
    r.diag.ciIndex = ciIndex;
    r.diag.filled |= replay::kBodyDiagCiFrom | replay::kBodyDiagCiIndex;
    void* ci = pl.ElementAt(ciIndex);
    if (!ci) { r.refusedAt = BodyRefusal::NoCi; return nullptr; }
    return ci;
}

bool Entity::CheckCiIdentity(PhysHandle h, uint16_t ciIndex, replay::CiPath from, const void* ci,
                             BodyReading& r, bool dropCiFieldCompare) const {
    const Pool& pl = PoolOf(h);
    const uint16_t v = h.v;
    // ---- the IDENTITY gates, before EITHER arm believes a field of this CI --------------------
    // Every number both arms return is read OFF this CI, so a CI that is not the object the handle
    // names produces a confident WRONG answer -- the history arm silently returns another entity's
    // pose, and the local arm refuses at whichever gate the wrong object happens to fail. That is
    // the fault no arm in this enum could name when every body read in L2-RUN-11 refused at BadSlot. The
    // engine keeps two maps over this one pool and they are inverses of each other:
    // PHYS.CS_HANDLE_MAP_OFF is handle -> index and PHYS.CS_REVERSE_MAP_OFF is index -> handle, the
    // pair ENTITY.UPDATE_TARGET_STATE walks in both directions every frame. Two u16 loads, no
    // syscall, and each stride is the registry row's own, never a number spelled here.
#if defined(W1E_MUTATE_NO_IDENTITY_GATE)
    // Mutation control: the shape this packet replaced -- both arms believe whatever element the
    // forward map addressed. A fixture that cannot fail this returns another entity's pose.
    (void)v; (void)ciIndex; (void)from; (void)ci; (void)r; (void)dropCiFieldCompare;
    return true;
#else
    void* revMap = pl.ReverseMap();
    if (!revMap) { r.refusedAt = BodyRefusal::NoReverseMap; return false; }
    // The engine's OWN rule for a component-system map lookup, and the one Pool::Element does not
    // apply: HasComponent (vtable +0x218, sub_1402F7F90) is `66 42 39 0C 82 / 0F 92 C0` --
    // cmp word [map + 4*id], count / setb -- an UNSIGNED, STRICT less-than against the live count,
    // so an index at or past the count addresses no instance.  The map holds a raw 0-based index:
    // the same u16 is used unscaled as an element subscript at 0x14044CC75 (`imul rcx, 78h`) and
    // the count is decremented one per removal (`66 01 87 F4 00 00 00`, sub_1402F4C70).  There is
    // no +1 encoding to undo, and no `none` value other than "at or past the count".
    // The whole live range, once: on the pool reading the CI's handle IS what this map says lives
    // at its index, so the read has to happen before the equality gates and not after them.
    const size_t live      = pl.Count();
    const size_t revExtent = sizeof(uint16_t) * live;
    const uint8_t* revProved = ProveSpan(revSpan_, revMap, revExtent);
    if (!revProved || !TryTouchSpan(revProved, revExtent)) {
        r.refusedAt = BodyRefusal::NoReverseMap;
        return false;
    }
    // WHAT THIS MAP HOLDS, settled in bytes rather than by its name: an ENTITY ID -- never a net
    // handle, never a player slot.  Three sites read revMap[i] and every one feeds the u16 straight
    // back in as the subscript of the STRIDE-4 forward map at PHYS.CS_HANDLE_MAP_OFF, taking the
    // +2 half as a generation, to compose a {u16 entityId, u16 generation} component reference:
    // ENTITY.UPDATE_TARGET_STATE at 0x140D87E18 (`49 8B 8C 24 D8 00 00 00`), 0x140D87E20
    // (`0F B7 14 59`), 0x140D87E2B (`49 8B 8C 24 C8 00 00 00`), 0x140D87E33 (`0F B7 54 91 02`);
    // and the two generic component-system walks sub_1402F4A90 / sub_1402F6E70, which spell the
    // identical pair.  The forward map is the one HasComponent (sub_1402F7F90,
    // `66 42 39 0C 82 / 0F 92 C0`) subscripts with its `u16 entityId` argument, so its KEY is an
    // entity id and this map's VALUE therefore is one too.  `v` is that same key.
    const uint16_t revEntityId =
        Rd<uint16_t>(revProved, static_cast<uint32_t>(sizeof(uint16_t) * ciIndex));
    r.diag.revHandle = revEntityId;
    r.diag.filled   |= replay::kBodyDiagRevHandle;
    const uint16_t entityId = Rd<uint16_t>(ci, lp::kNetCiEntityIdOff);
    r.diag.ciEntityId = entityId;
    r.diag.filled    |= replay::kBodyDiagCiEntity;
    // Entity id 0 is the local pawn's by design AND the value a zero-filled CI and a zero-filled
    // reverse map both carry, so every equality gate below is notApplicable on it rather than
    // passed.  A gate that cannot fail is not a gate.  The id that decides which is the one that
    // NAMES this instance: the caller's on the handle reading, the reverse map's on the pool one.
    const uint16_t namingId = (from == replay::CiPath::Pool) ? revEntityId : v;
#if defined(W1E_MUTATE_POOL_ZERO_DISCRIMINATES)
    // Mutation control: entity id 0 treated as a discriminating value on the pool reading, so the
    // local pawn's own CI is asked an equality only a non-zero id can answer.
    const bool discriminates = true;
#else
    const bool discriminates = (namingId != 0u);
#endif
    if (!discriminates) { return ProveByUniqueIndex(namingId, ciIndex, revProved, live, r); }
    if (from == replay::CiPath::Handle) {
        // The caller asserted an entity id and both tables have to say it back: the CI's own field
        // first -- it is the one the forward map cannot see past -- then the reverse map, which
        // must send the index the forward map produced back to the id the caller gave.
        // The remote-replay drive surface drops ONLY the ci[+0].entityId field compare -- the engine
        // reads pool index / reverse map and never demands it (DIAG-POSE-DECOUPLE 4a). The reverse-map
        // inverse guard below stays: it is the engine's own forward<->reverse invariant, and without
        // it a stale/reused forward entry (forwardMap[v]==ciIndex but revMap[ciIndex]!=v) would drive
        // the wrong on-screen object with this track.
        if (!dropCiFieldCompare && entityId != v) {
            r.refusedAt = BodyRefusal::CiEntityMismatch; return false;
        }
        if (revEntityId != v) { r.refusedAt = BodyRefusal::RevMapMismatch;   return false; }
        r.diag.identity = replay::IdentityProof::HandleCompare;
        return true;
    }
#if defined(W1E_MUTATE_POOL_ID_COMPARE)
    // Mutation control: the shape L2 #14 refused on -- two entity ids compared against each other
    // with nothing to say which of them this instance belongs to.
    if (entityId != revEntityId) { r.refusedAt = BodyRefusal::CiEntityMismatch; return false; }
    r.diag.identity = replay::IdentityProof::MapRoundTrip;
    return true;
#else
    // THE POOL READING, and the whole of L2 #14's F36.  Both u16s in hand are entity ids, so
    // `entityId != revEntityId` compares like with like -- and decides nothing on its own, because
    // it cannot say WHICH of the two is this instance's.  The engine can: the forward map is the
    // reverse map's inverse over the live range (UPDATE_TARGET_STATE walks the pair in both
    // directions every frame), so the id that owns this instance is the id the forward map sends
    // back to this index.  Asking it of BOTH candidates separates the readings that a single
    // `ent != rev` collapsed into one refusal.
    uint16_t low = 0, high = 0;
    const bool revRoundTrips = pl.MapEntry(revEntityId, low, high) && low == ciIndex;
    const bool ciRoundTrips  = entityId != 0u && pl.MapEntry(entityId, low, high) &&
                               low == ciIndex;
    if (revRoundTrips && ciRoundTrips && entityId != revEntityId) {
        // Two ids, one instance: the forward map is not injective over the live range and neither
        // answer can be believed.  No arm before this one could have named that.
        r.refusedAt = BodyRefusal::ForwardMapAmbiguous;
        return false;
    }
    if (!revRoundTrips && !ciRoundTrips) {
        r.refusedAt = BodyRefusal::CiEntityMismatch;
        return false;
    }
    // Exactly one stood, and it is the engine's own map that says so.  Where it is the reverse map
    // and the CI's field disagrees, the CI is still the instance both maps address -- the field
    // that disagrees is the one displacement in this chain no anchor can certify -- so the read
    // proceeds and `ident=` publishes which table proved it.
    r.diag.identity = revRoundTrips ? replay::IdentityProof::MapRoundTrip
                                    : replay::IdentityProof::CiIdRoundTrip;
    return true;
#endif
#endif
}

// Entity id 0: no equality against it can discriminate, so the proof has to come from somewhere a
// zero fill cannot reach.  Each live instance belongs to one entity, so its id stands at exactly
// one live index of the reverse map.
bool Entity::ProveByUniqueIndex(uint16_t namingId, uint16_t ciIndex, const uint8_t* revProved,
                                size_t live, BodyReading& r) const {
#if !defined(W1E_MUTATE_IDENTITY_VACUOUS_AT_ZERO)
    // A zero-filled table fails here the moment a second instance is live -- the reading no
    // equality against 0 could ever produce.
    for (size_t i = 0; i < live; ++i) {
        if (i == static_cast<size_t>(ciIndex)) continue;
        if (Rd<uint16_t>(revProved, static_cast<uint32_t>(sizeof(uint16_t) * i)) == namingId) {
            r.refusedAt = BodyRefusal::RevMapAmbiguous;
            return false;
        }
    }
#else
    (void)revProved; (void)live;
#endif
    (void)namingId;
    r.diag.identity = replay::IdentityProof::UniqueLiveIndex;
    return true;
}

bool Entity::ReadBodyDetailed(PhysHandle h, BodyState& out, BodyReading& r) const {
    return ReadBodyDetailedImpl(h, out, r, /*decoupled=*/false);
}

// The remote-replay slot surface's read-back oracle. Same read as ReadBodyDetailed, but the identity
// gate drops only the ci[+0].entityId==handle field compare (reverse-map guard kept), so it resolves
// the same ElementAt(2) the install drove and reports source=History off the node we pushed -- which
// is what lets the pawn line log src=history instead of ciEntityMismatch (DIAG-POSE-DECOUPLE 4a).
bool Entity::ReadBodyDetailedDriven(PhysHandle h, BodyState& out, BodyReading& r) const {
    return ReadBodyDetailedImpl(h, out, r, /*decoupled=*/true);
}

bool Entity::ReadBodyDetailedImpl(PhysHandle h, BodyState& out, BodyReading& r, bool decoupled) const {
    r = BodyReading();
    uint16_t ciIndex = 0;
    replay::CiPath from = replay::CiPath::NotAsked;
    const Pool& pl = PoolOf(h);
    const void* ci = ResolveCi(h, ciIndex, from, r);
    if (!ci) { r.source = BodySource::None; return false; }
    r.ciResolved = true;
    r.diag.ciTag   = replay::BodyDiagTag(ci);
    r.diag.filled |= replay::kBodyDiagCi;
    // WHICH reading produced the CI decides whether a refusal below ends the read. A pool-walked
    // CI is one the forward map holds no live component for -- the local pawn's own shape, an
    // entity this machine simulates and was never given a net handle for -- and its pose lives in
    // the ncaTransform chain whatever the physics chain says, so every physics-arm refusal on one
    // falls through to that chain with its reason kept on the diag. A handle-resolved CI names an
    // entity the net layer owns; there is no second source for it here and its refusals stand.
    const bool poolWalked = (from == replay::CiPath::Pool);
    // The identity gates are part of "the physics arm refused": they are asked of the ncaPhysics
    // CI, and the transform chain reaches the entity's pose through ci+0x10 without them.
    if (!CheckCiIdentity(h, ciIndex, from, ci, r, /*dropCiFieldCompare=*/decoupled)) {
        return AnswerFromTransform(pl, ci, poolWalked, out, r);
    }

    const void* container = Deref(ci, facts::PHYS_CI_HIST_LIST_OFF);
    const void* node = container ? Deref(container, facts::PHYS_LIST_HEAD_PTR_OFF) : nullptr;
    if (node) {
        r.historyPresent = true;
        memcpy(out.com,         At(const_cast<void*>(node), facts::PHYS_NODE_COM_OFF),      sizeof out.com);
        memcpy(out.ori,         At(const_cast<void*>(node), facts::PHYS_NODE_ORI_OFF),      sizeof out.ori);
        memcpy(out.vel,         At(const_cast<void*>(node), facts::PHYS_NODE_VEL_OFF),      sizeof out.vel);
        memcpy(out.angVel,      At(const_cast<void*>(node), facts::PHYS_NODE_ANGVEL_OFF),   sizeof out.angVel);
        r.diag.angVel  = replay::AngVelSource::NodeStored;
        r.diag.filled |= replay::kBodyDiagAngVel;
        memcpy(out.lastHeldCom, At(const_cast<void*>(node), facts::PHYS_NODE_LASTHELD_OFF), sizeof out.lastHeldCom);
        out.bits11 = replay::BodyWireBits(Rd<uint32_t>(ci, facts::PHYS_CI_FLAGS_OFF));
        out.owner_slot = replay::kBodyOwnerNone;
        r.source = BodySource::History;
        return true;
    }

    if (ReadLocalBody(pl, ci, out, r)) {
        r.source = BodySource::Local;
        r.diag.origin  = replay::BodyOrigin::CphBody;
        r.diag.filled |= replay::kBodyDiagOrigin;
        return true;
    }
    return AnswerFromTransform(pl, ci, poolWalked, out, r);
}

// The physics arm has refused, at any of its gates. Record WHY, then -- on a pool-walked CI only
// -- ask the ncaTransform chain for the pose.
//
// `refusedAt` has to go back to None when the fallback answers: src/capture counts it into the
// refusal histogram whether or not the read succeeded, so a refusal counted against a body that
// WAS recorded would make the stop line's refusedBy set disagree with its own sample count. That
// makes BodyDiag::cphBodyArm the only surviving record of why the physics chain could not answer,
// and it is filled on every refusal -- including the ones that do not fall through -- so a reader
// of the stop line measures both arms at once.
bool Entity::AnswerFromTransform(const Pool& pl, const void* ci, bool poolWalked, BodyState& out,
                                 BodyReading& r) const {
    const BodyRefusal cphArm = r.refusedAt;
    r.diag.cphBodyArm = cphArm;
    r.diag.filled    |= replay::kBodyDiagCphArm;
#if defined(W1E_MUTATE_FALLBACK_ON_REMOTE_CI)
    // Mutation control: the fallback offered to a handle-resolved CI as well, which answers a
    // remote entity's pose out of a chain that only this machine's simulation fills.
    const bool localCi = true;
#else
    const bool localCi = poolWalked;
#endif
#if defined(W1E_MUTATE_TRANSFORM_FALLBACK_ON_NOCHAIN)
    const bool rightBuild = true;   // Mutation control: fall back even off the wrong build.
#else
    // NoChain means the PRIMARY_BODY / RESOLVE_BODY prologue guards did not match the mapped
    // image, i.e. this is not the build every displacement in the walk was read from.  The
    // transform chain calls neither of those functions, but it is spelled out of the SAME build's
    // bytes, so answering from it there would be reading a different program's memory map. The
    // identity gates refuse ahead of that check, so the flag itself is asked, not just the arm.
    const bool rightBuild = bodyChainOk_ && cphArm != BodyRefusal::NoChain;
#endif
#if defined(W1E_MUTATE_FALLBACK_ONLY_BADSLOT)
    // Mutation control: the fallback restricted to the one refusal L2 #12 happened to measure, so
    // every other arm of the physics chain refuses the local pawn outright.
    const bool armFallsThrough = (cphArm == BodyRefusal::BadSlot);
#else
    const bool armFallsThrough = true;
#endif
    if (localCi && rightBuild && armFallsThrough && ReadLocalTransform(pl, ci, out, r)) {
        r.source = BodySource::Local;
        r.diag.origin  = replay::BodyOrigin::LocalTransform;
        r.diag.filled |= replay::kBodyDiagOrigin;
        r.refusedAt    = BodyRefusal::None;
        return true;
    }
    // Both chains refused.  `refusedAt` stays the FIRST refusal -- the physics chain's, which is
    // the one every scorecard to date is written against -- and BodyDiag::xformArm carries the
    // fallback's own.
    r.refusedAt = cphArm;
    r.source    = BodySource::None;
    return false;
}

// The live transform of a body THIS machine integrates.
//
// `com` is a SUM. ENTITY.UPDATE_TARGET_STATE computes the world centre of mass as
// *(body+kPhBodyPosOff) + *(body+kPhBodyComOffsetOff) and steers it onto the SimState com by two
// independent routes -- a Translate by the difference, and the same difference added straight into
// the offset slot. So a reader that returns only the position slot returns a DIFFERENT QUANTITY
// from the one the history nodes carry, and the two sources would disagree by a constant that
// nothing downstream could see.
// The space's component-system registry, walked for one symbol.  The engine's own resolver is a
// BINARY SEARCH over this array -- and when the dirty bit is set it SORTS THE ARRAY IN PLACE
// (`test byte [rcx+498h],1` / `je` past `call 0x1400F0580` / `and [rdi+498h],~1`) before it
// searches.  A capture reader may not write engine state on a tick, and it may not binary-search
// a registry it has not sorted either, so this walks the array linearly and compares the key:
// the answer the engine reaches on a clean registry, and the right answer on a dirty one.
const uint8_t* Entity::FindSpaceRegistryEntry(void* space, uint64_t symbol, BodyReading& r) const {
    void* base = Deref(space, facts::CS_LIST_TABLE_OFF);
    const uint64_t n = Rd<uint64_t>(space, facts::CS_LIST_TABLE_COUNT_OFF);
    if (!base || n == 0) { r.refusedAt = BodyRefusal::NoPhysicsCsList; return nullptr; }
    const size_t extent = static_cast<size_t>(facts::CS_LIST_RECORD_STRIDE) * static_cast<size_t>(n);
    const uint8_t* proved = ProveSpan(csRegistrySpan_, base, extent);
    if (!proved || !TryTouchSpan(proved, extent)) {
        r.refusedAt = BodyRefusal::PhysicsCsListUnreadable;
        return nullptr;
    }
    for (uint64_t i = 0; i < n; ++i) {
        const uint8_t* rec = proved + static_cast<size_t>(facts::CS_LIST_RECORD_STRIDE) * i;
        // The key is the record's first qword -- `4A 39 1C 11` cmp qword [rcx+r10], rbx at
        // 0x140105633.  Offset zero has no displacement byte, so the registry withholds
        // CS.LIST_RECORD_KEY_OFF rather than certify a number its anchors cannot see.
        if (Rd<uint64_t>(rec, 0u) == symbol) return rec + facts::CS_LIST_RECORD_VALUE_OFF;
    }
    r.refusedAt = BodyRefusal::NoPhysicsCsList;
    return nullptr;
}

// HasComponent, reproduced -- `cmp word [map + 4*id], count` / `setb al` at 0x1402F7FA4.  The
// same UNSIGNED, STRICT rule IndexPastCount carries for every other component-system bound in
// this file, asked of a CANDIDATE component system rather than of one already chosen.
bool Entity::CsHasComponent(void* cs, uint16_t entityId, SpanCache& probe) const {
    void* map = Deref(cs, facts::PHYS_CS_HANDLE_MAP_OFF);
    if (!map) return false;
    const size_t extent =
        static_cast<size_t>(lp::kPhysicsCsMapEntryBytes) * (static_cast<size_t>(entityId) + 1u);
    const uint8_t* proved = ProveSpan(probe, map, extent);
    if (!proved || !TryTouchSpan(proved, extent)) return false;
    const uint16_t slot = Rd<uint16_t>(
        proved,
        static_cast<uint32_t>(lp::kPhysicsCsMapEntryBytes * entityId) + lp::kPhysicsCsMapSlotHalfOff);
    return !IndexPastCount(slot, Rd<uint16_t>(cs, facts::PHYS_CS_COUNT_OFF));
}

// The engine's own per-frame body producer's search, reproduced.  This is the packet's whole
// point: the registry entry for the ncaPhysics symbol is a LIST -- array at CS.LIST_ARR_OFF,
// count at CS.LIST_COUNT_OFF -- and that producer walks it and takes the FIRST component system
// that answers HasComponent for the entity (0x140D49F10..0x140D49F4C).  PHYS.NETCS_PHYSICS_CS_OFF
// is ONE cached pointer, consumed with no re-check and no iteration, so on a space holding more
// than one ncaPhysics CS it can name a CS that does not own the entity -- and nothing below it
// bounds the subscript.  That is the mechanism behind a badSlot on a live entity.
void* Entity::FindPhysicsCsForEntity(uint16_t entityId, BodyReading& r) const {
#if defined(W1E_MUTATE_PHYSCS_CACHED_POINTER)
    // Mutation control: the shape this packet replaced -- the single cached pointer, no search.
    void* cached = Deref(phys_.cs, lp::kNetCsPhysicsCsOff);
    if (!cached) { r.refusedAt = BodyRefusal::NoPhysicsCs; return nullptr; }
    r.diag.csIdx   = 0;
    r.diag.filled |= replay::kBodyDiagCsIdx;
    return cached;
#else
    void* space = Deref(phys_.cs, facts::PHYS_PHYSICS_CS_STATE_OBJ_OFF);
    if (!space) { r.refusedAt = BodyRefusal::NoPhysicsSpace; return nullptr; }
    const uint8_t* entry = FindSpaceRegistryEntry(space, facts::CS_SYM_NCAPHYSICS, r);
    if (!entry) return nullptr;                       // FindSpaceRegistryEntry named its own arm
    // `49 8B 0E` mov rcx,[r14] at 0x140D49F10 -- the array is the container's first field, and
    // CS.LIST_ARR_OFF is withheld for the same reason CS.LIST_RECORD_KEY_OFF is.
    void* arr = Deref(entry, 0u);
    const uint64_t count = Rd<uint64_t>(entry, facts::CS_LIST_COUNT_OFF);
    r.diag.csCount = static_cast<uint32_t>(count < UINT32_MAX ? count : UINT32_MAX);
    r.diag.filled |= replay::kBodyDiagCsCount;
    // An entry that exists but holds nothing is not the same answer as a list that was walked
    // and said no, and the two must not print the same word.
    if (!arr || count == 0) { r.refusedAt = BodyRefusal::NoPhysicsCs; return nullptr; }
    // The engine's loop trusts CS.LIST_COUNT_OFF with nothing behind it.  A reader cannot: every
    // component system in this list is registered in the space's own registry, so the list can
    // never be longer than that registry's count, and a longer one is a count read off an object
    // that is not the container.  The bound is the engine's OWN number, not one spelled here.
#if !defined(W1E_MUTATE_CSLIST_NO_COUNT_BOUND)
    if (count > Rd<uint64_t>(space, facts::CS_LIST_TABLE_COUNT_OFF)) {
        r.refusedAt = BodyRefusal::PhysicsCsListUnreadable;
        return nullptr;
    }
#endif
    const size_t extent = sizeof(void*) * static_cast<size_t>(count);
    const uint8_t* proved = ProveSpan(csListSpan_, arr, extent);
    if (!proved || !TryTouchSpan(proved, extent)) {
        r.refusedAt = BodyRefusal::PhysicsCsListUnreadable;
        return nullptr;
    }
    // A list of ONE cannot be mis-selected: the cached pointer and the search necessarily name
    // the same component system, so selection defers to PrimaryBody -- which is where the engine
    // reads the bypass bit that makes the live count non-authoritative -- the one
    // lp::kPhysicsCsSlotBoundBypassBit names, taken on every body of L2-RUN-10. Applying it would
    // refuse those bodies, which is a worse answer than the engine's own. With two or more, the
    // count is exactly what discriminates them and HasComponent is the producer's own selector.
    // DECISION: the list length is what
    // decides whether HasComponent may exclude a component system at all.
    if (count == 1) {
        void* only = nullptr;
        memcpy(&only, proved, sizeof only);
        if (only) {
            r.diag.csIdx   = 0;
            r.diag.filled |= replay::kBodyDiagCsIdx;
            return only;
        }
    }
    for (uint64_t i = 0; i < count; ++i) {
        void* cs = nullptr;
        memcpy(&cs, proved + sizeof(void*) * static_cast<size_t>(i), sizeof cs);
        if (!cs) continue;
#if !defined(W1E_MUTATE_CSLIST_NO_HASCOMPONENT)
        if (!CsHasComponent(cs, entityId, csProbeSpan_[i % kCsProbeSlots])) continue;
#endif
        r.diag.csIdx   = static_cast<uint16_t>(i);
        r.diag.filled |= replay::kBodyDiagCsIdx;
        return cs;
    }
    r.refusedAt = BodyRefusal::NoPhysicsCsForEntity;
    return nullptr;
#endif
}

bool Entity::ReadLocalBody(const Pool& pl, const void* ci, BodyState& out, BodyReading& r) const {
    // Each diag field is recorded AT the gate that computed it, so a refusal carries exactly what
    // was known when it fired and nothing it did not reach. `filled` is what separates the two.
    if (!bodyChainOk_ || !pl.cs) { r.refusedAt = BodyRefusal::NoChain; return false; }

    // The entity id comes first now: it is the KEY the search runs on, not just a subscript.
    const uint32_t rawBodyId = Rd<uint32_t>(ci, lp::kPhysCiBodyIdOff);
    r.diag.bodyId  = rawBodyId;
    r.diag.filled |= replay::kBodyDiagBodyId;
    const uint16_t bodyId = static_cast<uint16_t>(rawBodyId);

    void* physCs = FindPhysicsCsForEntity(bodyId, r);
    r.diag.physCsTag = replay::BodyDiagTag(physCs);   // 0 here means "searched, matched nothing"
    r.diag.filled   |= replay::kBodyDiagPhysCs;
    if (!physCs) return false;

    // ---- CPhysicsCS::PrimaryBody, reproduced ------------------------------------------------
    // `map=[rcx+0C8h]; slot=[map+4*r8w]; slot<[rcx+0F4h]; ci=[rcx+0F8h]+78h*slot; [ci+38h]!=0;
    //  out=24 B from ci+40h`. The component-system triple is read off a CPhysicsCS here and off
    // a CR15NetPhysicsCS everywhere else in this file: same shape, different instance.
    void* map  = Deref(physCs, facts::PHYS_CS_HANDLE_MAP_OFF);
    void* base = Deref(physCs, facts::PHYS_CS_POOL_BASE_OFF);
    if (!map || !base) { r.refusedAt = BodyRefusal::NoPhysicsPool; return false; }
    // bodyId is a raw CI field and the map is indexed by it before anything bounds it, so the
    // span up to and including that entry is proved. Every probe on this path goes through
    // ProveSpan; they used to be a VirtualQuery each, per body, per tick.
    const size_t mapExtent =
        static_cast<size_t>(lp::kPhysicsCsMapEntryBytes) * (static_cast<size_t>(bodyId) + 1u);
    const uint8_t* mapProved = ProveSpan(mapSpan_, map, mapExtent);
    if (!mapProved || !TryTouchSpan(mapProved, mapExtent)) {
        r.refusedAt = BodyRefusal::MapUnreadable;
        return false;
    }
#if defined(W1E_MUTATE_BODY_SLOT_HIGH_HALF)
    const uint32_t slotHalf = lp::kPhysicsCsMapSlotHalfOff + sizeof(uint16_t);  // mutation control
#else
    const uint32_t slotHalf = lp::kPhysicsCsMapSlotHalfOff;
#endif
    const uint16_t slot = Rd<uint16_t>(
        mapProved, static_cast<uint32_t>(lp::kPhysicsCsMapEntryBytes * bodyId) + slotHalf);
    r.diag.slot    = slot;
    r.diag.filled |= replay::kBodyDiagSlot;
    // The bound is the SECOND term of the engine's condition, not the whole of it. Both operands
    // are read unconditionally now: the short circuit saved one u16 load off an object this
    // function already reads at three other displacements, and cost the refusal its numbers.
    const uint16_t   liveCount = Rd<uint16_t>(physCs, facts::PHYS_CS_COUNT_OFF);
    const BypassRead bypass    = ReadSlotBoundBypass(physCs);
    r.diag.liveCount = liveCount;
    r.diag.bypass    = bypass;
    r.diag.filled   |= replay::kBodyDiagLiveCount | replay::kBodyDiagBypass;
    if (bypass != BypassRead::Set && IndexPastCount(slot, liveCount)) {
        r.refusedAt = BodyRefusal::BadSlot;
        return false;
    }

    const size_t ciSpan =
        static_cast<size_t>(facts::PHYS_PHYSICS_CI_STRIDE) * slot + lp::kPhysicsCiReadExtent;
    const uint8_t* poolProved = ProveSpan(poolSpan_, base, ciSpan);
    if (!poolProved) { r.refusedAt = BodyRefusal::CiUnreadable; return false; }
    const uint8_t* pci = poolProved + static_cast<size_t>(facts::PHYS_PHYSICS_CI_STRIDE) * slot;
    if (!TryTouchSpan(pci, lp::kPhysicsCiReadExtent)) { r.refusedAt = BodyRefusal::CiUnreadable; return false; }
    if (Rd<uint64_t>(pci, facts::PHYS_PHYSICS_CI_BODY_GUARD_OFF) == 0) {
        r.refusedAt = BodyRefusal::NoPrimaryBody;   // honest: this entity HAS no primary body
        return false;
    }
    const uint8_t* target = pci + facts::PHYS_PHYSICS_CI_TARGET_OFF;
    const uint32_t bodyIndex = Rd<uint32_t>(target, facts::PHYS_TARGET_INDEX_OFF);
    const uint32_t zoneIndex = Rd<uint32_t>(target, facts::PHYS_TARGET_ZONE_OFF);
    const uint32_t tag       = Rd<uint32_t>(target, lp::kPhTargetTagOff);
    if (bodyIndex == lp::kPhIdDead) { r.refusedAt = BodyRefusal::DeadTarget; return false; }

    // ---- CPhTarget::ResolveBody, reproduced --------------------------------------------------
    // `index [rcx+8] != -1; zone = [*CLOCK.THIRD_PTR + 8] + 1E18h*[rcx+0Ch]; index < [zone+298h];
    //  body = [zone+268h] + 0F38h*index; [body+8] != -1; [body+10h] == [rcx+10h]` -- plus the
    // zone-count bound CPhTarget::ResolveZone applies and ResolveBody omits, and the recycled-zone
    // test from the same function. ResolveBody omits both because it trusts an index the engine
    // produced one instruction earlier; this reader is further from that guarantee than the engine
    // is, so it applies them.
    const void* phSystem = PhSystem();
    void* zoneArray = phSystem ? Deref(phSystem, facts::PHYS_SYSTEM_ZONE_ARRAY_OFF) : nullptr;
    if (!zoneArray) { r.refusedAt = BodyRefusal::NoPhysSystem; return false; }
    if (zoneIndex >= Rd<uint64_t>(phSystem, facts::PHYS_SYSTEM_ZONE_COUNT_OFF)) {
        r.refusedAt = BodyRefusal::BadZone;
        return false;
    }
    const size_t zoneExtent = facts::PHYS_ZONE_BODY_COUNT_OFF + sizeof(uint64_t);
    const uint8_t* zonesProved =
        ProveSpan(zoneSpan_, zoneArray,
                  static_cast<size_t>(facts::PHYS_ZONE_STRIDE) * zoneIndex + zoneExtent);
    if (!zonesProved) { r.refusedAt = BodyRefusal::ZoneUnreadable; return false; }
    const uint8_t* zone = zonesProved + static_cast<size_t>(facts::PHYS_ZONE_STRIDE) * zoneIndex;
    if (!TryTouchSpan(zone, zoneExtent)) {
        r.refusedAt = BodyRefusal::ZoneUnreadable;
        return false;
    }
    if (Rd<uint32_t>(zone, lp::kPhIdIndexOff) == lp::kPhIdDead) { r.refusedAt = BodyRefusal::DeadZone; return false; }
    if (bodyIndex >= Rd<uint64_t>(zone, facts::PHYS_ZONE_BODY_COUNT_OFF)) {
        r.refusedAt = BodyRefusal::BadBodyIndex;
        return false;
    }
    void* bodyArray = Deref(zone, facts::PHYS_ZONE_BODY_ARRAY_OFF);
    if (!bodyArray) { r.refusedAt = BodyRefusal::NoBodyArray; return false; }
    // One proof, on the widest of the four extents, covers every load below. This runs on the
    // game thread: a CPhBody freed between the resolve and the read would otherwise fault in the
    // owner's match rather than refuse, which is why the touch is here and not only the proof.
    const uint8_t* bodiesProved =
        ProveSpan(bodySpan_, bodyArray,
                  static_cast<size_t>(facts::PHYS_BODY_STRIDE) * bodyIndex + lp::kPhBodyReadExtent);
    if (!bodiesProved) { r.refusedAt = BodyRefusal::Unreadable; return false; }
    const void* body = bodiesProved + static_cast<size_t>(facts::PHYS_BODY_STRIDE) * bodyIndex;
    if (!TryTouchSpan(body, lp::kPhBodyReadExtent)) { r.refusedAt = BodyRefusal::Unreadable; return false; }
    if (Rd<uint32_t>(body, lp::kPhIdIndexOff) == lp::kPhIdDead) { r.refusedAt = BodyRefusal::DeadBody; return false; }
    if (Rd<uint32_t>(body, facts::PHYS_ID_TAG_OFF) != tag) { r.refusedAt = BodyRefusal::TagMismatch; return false; }
    r.bodyResolved = true;

    float pos[3], comOff[3];
    memcpy(pos,    At(const_cast<void*>(body), lp::kPhBodyPosOff),       sizeof pos);
    memcpy(comOff, At(const_cast<void*>(body), lp::kPhBodyComOffsetOff), sizeof comOff);
    for (int i = 0; i < 3; ++i) out.com[i] = pos[i] + comOff[i];

    memcpy(out.ori,    At(const_cast<void*>(body), lp::kPhBodyOriOff),    sizeof out.ori);
    memcpy(out.vel,    At(const_cast<void*>(body), lp::kPhBodyVelOff),    sizeof out.vel);

    // ANGULAR VELOCITY IS NOT A FIELD ON THE CPhBody, so we ask the engine for it the same way
    // the engine asks itself.  0x91C, which this used to read, is the PREVIOUS TICK's LINEAR
    // velocity.  The netCS send path builds its outgoing sim-state row from a PER-CALL TLS
    // scratch buffer that is serialised and freed inside sub_140D7E4A0, so there is no stored
    // row to read either; what persists is the FUNCTION.  PHYS.ANGVEL_FROM_CLOUD is that
    // function -- the identical call at 0x140D7EF6A -- so a locally simulated body yields the
    // very bytes a remote peer would have received for it.
    //
    // It dereferences the body's particle arrays (0x408/0x478, count 0x438), which are engine
    // heap outside bodySpan_ and cannot be proved here.  The tag and dead-index checks above
    // have just established that this body is live, which is exactly the warrant the engine
    // itself acts on at the send site; the alternative is to publish no physics at all.
#if defined(W1E_MUTATE_ANGVEL_NO_ENGINE_CALL)
    const AngVelCloudFn angVelFn = nullptr;
#else
    const AngVelCloudFn angVelFn = angVelFn_;
#endif
    if (angVelFn) {
        float omega[3] = {0.0f, 0.0f, 0.0f};
        const float* got = angVelFn(const_cast<void*>(body), omega);
        memcpy(out.angVel, got ? got : omega, sizeof out.angVel);
        // (0,0,0) here is a reading, not a gap: the callee returns it for a sleeping body
        // (0xBC bit 2) or an empty cloud, and the diag word is what tells the two apart.
        r.diag.angVel = replay::AngVelSource::EngineComputed;
    } else {
        // The prologue guard refused, or no entry point was injected. An honest zero, and a word
        // that does not claim the engine was asked.
        out.angVel[0] = out.angVel[1] = out.angVel[2] = 0.0f;
        r.diag.angVel = replay::AngVelSource::NotStoredByEngine;
    }
    r.diag.filled |= replay::kBodyDiagAngVel;

    // A game-level field the CPhBody does not carry. An honest zero beats a fabricated value.
    out.lastHeldCom[0] = out.lastHeldCom[1] = out.lastHeldCom[2] = 0.0f;
    // From the CI, exactly as the history arm takes it, so the two sources agree on this field by
    // construction rather than by coincidence.
    out.bits11 = replay::BodyWireBits(Rd<uint32_t>(ci, facts::PHYS_CI_FLAGS_OFF));
    out.owner_slot = replay::kBodyOwnerNone;
    return true;
}

// Quaternion algebra for the CNode3D parent walk.  The 48-byte entry is {quat, translate+w,
// scale+w} and the layout is x,y,z,w -- the scalar is the LAST lane.
static inline void QuatMul(const float a[4], const float b[4], float out[4]) {
    const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = x; out[1] = y; out[2] = z; out[3] = w;
}

// v' = q * v * conj(q), written out rather than through a matrix so no convention is implied by
// a storage order this reader does not control.
static inline void QuatRotate(const float q[4], const float v[3], float out[3]) {
    const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
    const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
    const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
    out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

// The ncaTransform FALLBACK.  It answers a POSE and says so: BodyDiag::origin is
// LocalTransform, angVel is Unavailable, and vel is a structural zero.
//
// WHY vel is not differenced from consecutive poses: the capture record has nowhere to say that
// it was.  BodyState carries no per-field provenance, and src/capture/gather.h keeps a
// BodyReading only for the FIRST REFUSAL of a run -- a read that ANSWERS discards its diag -- so
// a differenced velocity would reach the file indistinguishable from one the engine integrated,
// on every tick, with the flag it needs unable to travel beside it.  An honest zero plus a diag
// word is the answer the record can actually carry.
bool Entity::ReadLocalTransform(const Pool& pl, const void* ci, BodyState& out,
                                BodyReading& r) const {
    if (!pl.cs) { r.diag.xformArm = BodyRefusal::NoTransformCs; r.diag.filled |= replay::kBodyDiagXformArm; return false; }
    r.diag.filled |= replay::kBodyDiagXformArm;
    void* xfCs = Deref(pl.cs, lp::kNetCsTransformCsOff);
    if (!xfCs) { r.diag.xformArm = BodyRefusal::NoTransformCs; return false; }
    void* map  = Deref(xfCs, lp::kXformCsHandleMapOff);
    void* base = Deref(xfCs, lp::kXformCsPoolBaseOff);
    if (!map || !base) { r.diag.xformArm = BodyRefusal::NoTransformPool; return false; }

    // InitNetPhysicsCI caches the ncaTransform component ref on the CI the same way it caches the
    // ncaPhysics one, and fatals if either comes back 0xFFFF -- so this is an entity id in the
    // ncaTransform CS's own entity space, not an index, and its low half is the id.
    const uint16_t xfEntity = static_cast<uint16_t>(Rd<uint32_t>(ci, lp::kNetCiXformRefOff));
    const size_t mapExtent =
        static_cast<size_t>(lp::kXformMapEntryBytes) * (static_cast<size_t>(xfEntity) + 1u);
    const uint8_t* mapProved = ProveSpan(xformMapSpan_, map, mapExtent);
    if (!mapProved || !TryTouchSpan(mapProved, mapExtent)) {
        r.diag.xformArm = BodyRefusal::TransformMapUnreadable;
        return false;
    }
    const uint16_t index = Rd<uint16_t>(mapProved, static_cast<uint32_t>(lp::kXformMapEntryBytes * xfEntity));
    const uint16_t live  = Rd<uint16_t>(xfCs, lp::kXformCsCountOff);
    r.diag.xformIndex = index;
    r.diag.xformLive  = live;
    r.diag.filled    |= replay::kBodyDiagXformIdx | replay::kBodyDiagXformLive;
    // THE ENGINE OMITS THIS BOUND.  GetWorldTransform subscripts the pool with whatever the map
    // gave, because the engine produced that index itself one call earlier; this reader is
    // further from that guarantee than the engine is, so it applies HasComponent's own rule --
    // unsigned, strict, against the live count -- exactly as ReadLocalBody applies the zone and
    // recycled-slot tests ResolveBody omits.
#if !defined(W1E_MUTATE_XFORM_NO_HASCOMPONENT)
    if (IndexPastCount(index, live)) { r.diag.xformArm = BodyRefusal::NoTransformInstance; return false; }
#endif   // the mutation control is the ENGINE's own shape: subscript whatever the map gave

    const size_t poolExtent =
        static_cast<size_t>(lp::kXformCsInstStride) * index + lp::kXformInstReadExtent;
    const uint8_t* poolProved = ProveSpan(xformPoolSpan_, base, poolExtent);
    if (!poolProved) { r.diag.xformArm = BodyRefusal::TransformUnreadable; return false; }
    const uint8_t* inst = poolProved + static_cast<size_t>(lp::kXformCsInstStride) * index;
    if (!TryTouchSpan(inst, lp::kXformInstReadExtent)) {
        r.diag.xformArm = BodyRefusal::TransformUnreadable;
        return false;
    }

    float quat[4], pos[3];
    void* node = Deref(inst, lp::kXformInstNodeOff);
    if (node) {
        if (!ReadNodeChainTransform(node, quat, pos, r)) return false;
    } else {
        // The instance holds the transform outright.  x and y are one 8-byte load in the engine
        // and z a separate dword four bytes on, which is why the position is two reads and not
        // one -- a single 12-byte copy would be right by luck rather than by the bytes.
        memcpy(quat, At(const_cast<uint8_t*>(inst), lp::kXformInstRotOff), sizeof quat);
        memcpy(pos,  At(const_cast<uint8_t*>(inst), lp::kXformInstPosOff), 2u * sizeof(float));
        memcpy(pos + 2, At(const_cast<uint8_t*>(inst), lp::kXformInstPosZOff), sizeof(float));
    }

    // com is the entity's ORIGIN here, not its centre of mass: the CPhBody's com is
    // POS + COM_OFFSET and the ncaTransform carries no equivalent of the second term.
    // BodyDiag::origin is what tells a consumer which quantity this is.
    memcpy(out.com, pos,  sizeof out.com);
    memcpy(out.ori, quat, sizeof out.ori);
    out.vel[0] = out.vel[1] = out.vel[2] = 0.0f;
    out.angVel[0] = out.angVel[1] = out.angVel[2] = 0.0f;
    r.diag.angVel  = replay::AngVelSource::Unavailable;
    r.diag.filled |= replay::kBodyDiagAngVel;
    out.lastHeldCom[0] = out.lastHeldCom[1] = out.lastHeldCom[2] = 0.0f;
    out.bits11 = replay::BodyWireBits(Rd<uint32_t>(ci, facts::PHYS_CI_FLAGS_OFF));
    out.owner_slot = replay::kBodyOwnerNone;
    return true;
}

// The CNode3D parent walk.  The engine's own (sub_140628680) reads the node's 48-byte entry out
// of the block its owner keeps -- `e = *(*(node+0x70)+0x28) + 48 * *(u16*)(node+0x78)` -- and
// composes up `node = *(node+0x60)` for as long as the parent's class id is CNode3D's.
//
// TWO THINGS THIS DOES THAT THE ENGINE DOES NOT.  It bounds the walk (a corrupt or cyclic parent
// link would otherwise spin inside a capture tick), and it proves every span it touches, because
// each node is an unrelated engine-heap address rather than an offset into a pool this reader
// already bounded.
bool Entity::ReadNodeChainTransform(const void* node, float quat[4], float pos[3],
                                    BodyReading& r) const {
    float accQ[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float accP[3] = {0.0f, 0.0f, 0.0f};
    float accS[3] = {1.0f, 1.0f, 1.0f};
    const void* n = node;
    uint32_t depth = 0;
    while (n) {
        if (depth >= lp::kNodeChainDepthMax) {
            r.diag.xformArm = BodyRefusal::NodeChainTooDeep;
            return false;
        }
        const uint8_t* nProved = ProveSpan(nodeSpan_, const_cast<void*>(n), lp::kNode3dReadExtent);
        if (!nProved || !TryTouchSpan(nProved, lp::kNode3dReadExtent)) {
            r.diag.xformArm = BodyRefusal::TransformUnreadable;
            return false;
        }
        // The engine's own terminator, and it is applied to ANCESTORS only: sub_140628680 copies
        // the argument node's entry unconditionally and tests the class id of each PARENT before
        // consuming it.  A mid-chain failure STOPS and returns what has been accumulated -- the
        // jne at 0x14062874B goes straight to the epilogue, with no error and no identity
        // fallback -- so this breaks rather than refusing.
        if (depth != 0 && Rd<uint64_t>(nProved, lp::kNode3dClassIdOff) != lp::kNode3dClassId)
            break;
        void* owner = Deref(nProved, lp::kNode3dXformOwnerOff);
        const size_t ownerExtent = lp::kXformBlockBaseOff + sizeof(void*);
        const uint8_t* ownerProved = owner ? ProveSpan(nodeOwnerSpan_, owner, ownerExtent) : nullptr;
        if (!ownerProved || !TryTouchSpan(ownerProved, ownerExtent)) {
            r.diag.xformArm = BodyRefusal::TransformUnreadable;
            return false;
        }
        void* block = Deref(ownerProved, lp::kXformBlockBaseOff);
        const uint16_t entryIdx = Rd<uint16_t>(nProved, lp::kNode3dXformIdxOff);
        const size_t blockExtent =
            static_cast<size_t>(lp::kXformEntryStride) * (static_cast<size_t>(entryIdx) + 1u);
        const uint8_t* blockProved = block ? ProveSpan(nodeEntrySpan_, block, blockExtent) : nullptr;
        if (!blockProved || !TryTouchSpan(blockProved, blockExtent)) {
            r.diag.xformArm = BodyRefusal::TransformUnreadable;
            return false;
        }
        const uint8_t* e = blockProved + static_cast<size_t>(lp::kXformEntryStride) * entryIdx;
        float q[4], p[3], s[3];
        memcpy(q, e + lp::kXformEntryQuatOff,  sizeof q);
        memcpy(p, e + lp::kXformEntryPosOff,   sizeof p);
        memcpy(s, e + lp::kXformEntryScaleOff, sizeof s);

        if (depth == 0) {
            memcpy(accQ, q, sizeof accQ);
            memcpy(accP, p, sizeof accP);
            memcpy(accS, s, sizeof accS);
        } else {
            // acc is expressed in THIS node's frame; compose it into the node's own:
            //   p' = p_n + s_n * (R(q_n) . p_acc)     -- the SCALE IS APPLIED AFTER THE ROTATION
            //   q' = q_n * q_acc                      -- the node is the LEFT operand
            //   s' = s_n * s_acc
            // The scale's position is not cosmetic and a uniform-scale fixture cannot catch it:
            // CR15Node3D's composer (sub_1401DCCE0) hands the RAW child translation to the rotate
            // at 0x1401DCCF5 with no multiply ahead of it, and multiplies the already-rotated
            // components at 0x1401DCD1B/24/2E.  The two orders coincide under uniform scale.
            float rotated[3];
            QuatRotate(q, accP, rotated);
            for (int i = 0; i < 3; ++i) {
                accP[i] = p[i] + s[i] * rotated[i];
                accS[i] = s[i] * accS[i];
            }
            QuatMul(q, accQ, accQ);
        }
        n = Deref(nProved, lp::kNode3dParentOff);
        ++depth;
    }
    // The engine renormalises the composed quaternion and leaves the translation alone
    // (sqrtss at 0x1406287BF, divss by the 1.0f at 0x1416CCEF8).  A zero-norm accumulator is a
    // corrupt chain, not a rotation: refuse rather than divide.
    const float norm2 = accQ[0] * accQ[0] + accQ[1] * accQ[1] + accQ[2] * accQ[2] + accQ[3] * accQ[3];
    if (!(norm2 > 0.0f)) { r.diag.xformArm = BodyRefusal::TransformUnreadable; return false; }
    const float inv = 1.0f / sqrtf(norm2);
    for (int i = 0; i < 4; ++i) accQ[i] *= inv;
    memcpy(quat, accQ, sizeof accQ);
    memcpy(pos,  accP, sizeof accP);
    return true;
}

void Entity::PushHistory(PhysHandle h, const BodyState& s, uint64_t tUs) {
    const Pool& pl = PoolOf(h);
    void* ci = pl.Element(h.v);
    if (!ci || !unlink_ || !recycle_) return;
    void* container = Deref(ci, facts::PHYS_CI_HIST_LIST_OFF);
    if (!container) return;
    PushHistoryInto(h, pl, ci, container, s, tUs);
}

// The push itself, over an element the CALLER resolved. Splitting it is the whole of
// AUD-W2-B-POSEREAD blocker 1: SetHistoryWindowDetailed gates one element and must write that
// element, and re-deriving it from the handle here took the Pool arm to a different object.
void Entity::PushHistoryInto(PhysHandle h, const Pool& pl, void* ci, void* container,
                             const BodyState& s, uint64_t tUs) {
    if (!ci || !container || !unlink_ || !recycle_) return;

    // Take a free node; if the pool is out, retire this CI's oldest sample and take that one.
    // This is SimStateCB's second arm, and it deliberately does NOT test a ring-cap constant:
    // the cap the engine compares against is a qword in .rdata, and the registry's guards refuse
    // both it and its address (they read PC_PRESENT, which docs/PC_DERIVATION.md is explicit is
    // not a pass). The free list is the real resource and testing it needs no constant at all.
    // Per-CI growth is bounded by the caller's TrimHistory window rather than by a magic number.
    void* node = Deref(pl.cs, facts::PHYS_CS_FREE_TAKE_OFF);
    if (!node) {
        void* tail = Deref(container, facts::PHYS_LIST_TAIL_PTR_OFF);
        if (!tail) return;
        recycle_(pl.cs, ci, tail);
        node = Deref(pl.cs, facts::PHYS_CS_FREE_TAKE_OFF);
        if (!node) return;
    }

    unlink_(node);
    Wr<uint64_t>(ci, facts::PHYS_CI_HIST_COUNT_OFF,
                 Rd<uint64_t>(ci, facts::PHYS_CI_HIST_COUNT_OFF) + 1u);

    // Link as the newest: +8 walks toward older, +16 toward newer, and the container's +16 is
    // the head the interpolator starts from.
    Wr<void*>(node, facts::PHYS_NODE_OWNER_CONTAINER_OFF, container);
    void* oldHead = Deref(container, facts::PHYS_LIST_HEAD_PTR_OFF);
    if (oldHead) Wr<void*>(oldHead, facts::PHYS_NODE_PREV_OFF, node);
    Wr<void*>(node, facts::PHYS_NODE_NEXT_OFF, oldHead);
    Wr<void*>(container, facts::PHYS_LIST_HEAD_PTR_OFF, node);
    if (!Deref(container, facts::PHYS_LIST_TAIL_PTR_OFF))
        Wr<void*>(container, facts::PHYS_LIST_TAIL_PTR_OFF, node);

    memcpy(At(node, facts::PHYS_NODE_COM_OFF),      s.com,         sizeof s.com);
    memcpy(At(node, facts::PHYS_NODE_ORI_OFF),      s.ori,         sizeof s.ori);
    memcpy(At(node, facts::PHYS_NODE_VEL_OFF),      s.vel,         sizeof s.vel);
    memcpy(At(node, facts::PHYS_NODE_ANGVEL_OFF),   s.angVel,      sizeof s.angVel);
    memcpy(At(node, facts::PHYS_NODE_LASTHELD_OFF), s.lastHeldCom, sizeof s.lastHeldCom);

    // Stamp the node's owner with the CI's CURRENT owner. A node whose owner disagrees is what
    // SetOwner frees, and it is also what SimStateCB's owner gate drops.
    Wr<uint32_t>(node, facts::PHYS_NODE_OWNERID_OFF, Rd<uint32_t>(ci, facts::PHYS_CI_OWNER_OFF));

    // The warp nibble is copied from the CI, so UPDATE_TARGET_STATE's cyclic comparison of
    // node against CI returns 0 and the CI's own nibble never moves. That equality IS the
    // acceptance criterion "no unexpected (v & 0xF) %% 0xF dominance behaviour" (PROBE-3).
    //
    // Above the two nibbles, the header carries the recording's own bits. The remote-holder gate
    // sub_140CED640 reads bit 0x200 of THIS word through PhysCI+0x158 -> head node -> +0x20, so a
    // node installed without them answers a gate the engine asks about every disc frame. Only bits
    // 8-10 are carried: bits 0-3 are the ownership sequence and 4-7 the pose warp, both of which
    // the engine sequences against the CI and neither of which the recording is entitled to move.
    const uint16_t ciFlags = Rd<uint16_t>(ci, facts::PHYS_CI_FLAGS_OFF);
    uint16_t stateHdr = Rd<uint16_t>(node, facts::PHYS_NODE_STATE_OFF);
    stateHdr = static_cast<uint16_t>((stateHdr & static_cast<uint16_t>(~kNodeStateRecordedMask)) |
                                     (static_cast<uint16_t>(s.bits11) & kNodeStateRecordedMask));
    stateHdr = static_cast<uint16_t>((stateHdr & static_cast<uint16_t>(~kWarpNibbleMask)) |
                                     (ciFlags & kWarpNibbleMask));
    // The recorded held level outranks the sample's own bit 9 for the one body it was published
    // for: bits11 carries it on the tick possession changed and on no other, and the engine reads
    // this node every frame the disc is held.
    if (nodeHeldSet_ && nodeHeldHandle_ == h.v && nodeHeldPool_ == h.pool) {
        stateHdr = static_cast<uint16_t>((stateHdr & static_cast<uint16_t>(~kNodeHeldBit)) |
                                         (nodeHeldValue_ ? kNodeHeldBit : 0u));
        ++::g_nodeDiag.pushes;
        ::g_nodeDiag.lastState = stateHdr;
    }
    Wr<uint16_t>(node, facts::PHYS_NODE_STATE_OFF, stateHdr);

    // Two different times, and conflating them is a bug the field names hide: +120 is the
    // owner's stamp and the interpolation key, +112 is the local clock at arrival.
    Wr<uint64_t>(node, facts::PHYS_NODE_TIME_OFF, tUs);
    Wr<uint64_t>(node, facts::PHYS_NODE_RECVSTAMP_OFF, tUs);
}

// Through the SAME gate the install passes (AUD-W2-B-POSEREAD major 2): retiring nodes off an
// element the identity gate declines is destroying another entity's history, and on the Pool arm
// phys_.Element addresses a different object from the one the install vouched for.
void Entity::TrimHistory(PhysHandle h, uint64_t keepFromUs, uint64_t keepToUs) {
    replay::CiPath from = replay::CiPath::NotAsked;
    uint16_t ciIndex = 0;
    BodyReading rd;
    void* ci = ResolveVouched(h, from, ciIndex, rd);
    if (!ci || !recycle_) return;
    void* container = Deref(ci, facts::PHYS_CI_HIST_LIST_OFF);
    if (!container) return;

    // Retire from the tail while the oldest sample is outside the window. Nodes are ordered, so
    // stopping at the first one inside the window is correct and bounds the work per call. The
    // loop counter is a second, independent bound: a corrupted list must not spin the tick thread,
    // and the engine's own ring is two orders of magnitude smaller than this.
    for (uint32_t guard = 0; guard < 4096u; ++guard) {
        void* tail = Deref(container, facts::PHYS_LIST_TAIL_PTR_OFF);
        if (!tail) return;
        const uint64_t t = Rd<uint64_t>(tail, facts::PHYS_NODE_TIME_OFF);
        if (t >= keepFromUs && t <= keepToUs) return;
        if (Deref(container, facts::PHYS_LIST_HEAD_PTR_OFF) == tail) return;  // keep one sample
        recycle_(PoolOf(h).cs, ci, tail);
    }
}

void* Entity::ResolveVouched(PhysHandle h, replay::CiPath& from, uint16_t& ciIndex,
                             BodyReading& r, bool decoupled) const {
    from = replay::CiPath::NotAsked;
    ciIndex = 0;
    void* ci = const_cast<void*>(ResolveCi(h, ciIndex, from, r));
    if (!ci) return nullptr;
    if (!CheckCiIdentity(h, ciIndex, from, ci, r, /*dropCiFieldCompare=*/decoupled)) return nullptr;
    return ci;
}

// One call, because n pushes cannot express "these samples, in this order". See B.4's comment.
bool Entity::SetHistoryWindow(PhysHandle h, const BodyState* samples,
                              const uint64_t* timesUs, size_t n) {
    replay::InstallReading ir;
    return SetHistoryWindowDetailed(h, samples, timesUs, n, ir);
}

// DIAG-PAWN-STATIC (4): this used to install through phys_.Element with no identity gate at all,
// while ReadBodyDetailed asked ResolveCi + CheckCiIdentity of the very same handle -- so a session
// could report a driven-pose write into a CI its own reader refused to vouch for, and did. The two
// surfaces now pass the SAME gate: an element the reader would decline is refused here by name
// rather than driven blind.
bool Entity::SetHistoryWindowDetailed(PhysHandle h, const BodyState* samples,
                                      const uint64_t* timesUs, size_t n,
                                      replay::InstallReading& ir) {
    return InstallHistoryWindow(h, samples, timesUs, n, ir, /*decoupled=*/false);
}

// The remote-replay slot surface (DIAG-POSE-DECOUPLE 4b): resolve the CI by pool bounds only so the
// push lands on the element the engine already drives for this handle (ResolveCi(25) -> ElementAt(2)),
// without the HANDLE-arm identity proof the engine never makes. Everything downstream is identical.
bool Entity::SetHistoryWindowDriven(PhysHandle h, const BodyState* samples,
                                    const uint64_t* timesUs, size_t n,
                                    replay::InstallReading& ir) {
    return InstallHistoryWindow(h, samples, timesUs, n, ir, /*decoupled=*/true);
}

bool Entity::InstallHistoryWindow(PhysHandle h, const BodyState* samples,
                                  const uint64_t* timesUs, size_t n,
                                  replay::InstallReading& ir, bool decoupled) {
    using Refusal = replay::InstallRefusal;
    ir = replay::InstallReading();
    if (!samples || !timesUs || n == 0 || n > kMaxHistoryWindow) {
        ir.why = Refusal::BadWindow;
        return false;
    }

    // ONE resolution, and every line below addresses `ci` -- never phys_.Element(h.v) again. The
    // arm is published with the refusal because Handle and Pool are different elements.
    uint16_t ciIndex = 0;
    BodyReading r;
#if defined(W1E_MUTATE_INSTALL_BLIND)
    // Mutation control: the shape DIAG-PAWN-STATIC (4) refused on -- the install driving
    // whatever element the map addressed, with no identity proof the reader would demand.
    void* ci = const_cast<void*>(ResolveCi(h, ciIndex, ir.from, r));
#else
    // The remote-replay slot surface drops only the ci[+0] field compare; the reverse-map inverse
    // guard stays (decoupled). Every other caller keeps the full gate.
    void* ci = ResolveVouched(h, ir.from, ciIndex, r, decoupled);
#endif
    if (!ci) {
        // NOT keyed on the arm: ResolveCi publishes `from` the moment it picks an arm, which is
        // BEFORE ElementAt can fail, so a live map over a null pool base would be reported as an
        // identity mismatch on an element that never resolved. The reader's own refusal separates
        // them (AUD-W2-B-POSEREAD minor 8).
        ir.why = (r.refusedAt == BodyRefusal::NoCi || r.refusedAt == BodyRefusal::CiIndexNotLive)
                     ? Refusal::NoElement
                     : Refusal::CiIdentityMismatch;
        return false;
    }
    if (!unlink_ || !recycle_) { ir.why = Refusal::NoRecycler; return false; }
    void* container = Deref(ci, facts::PHYS_CI_HIST_LIST_OFF);
    if (!container) { ir.why = Refusal::NoHistoryList; return false; }

    // Order by ascending time and push in that order: PushHistory prepends, so the list comes out
    // strictly decreasing from the head, which is the only order the interpolator's walk accepts.
    // n is a replay window and the engine's own ring is 30 nodes, so the selection sort is bounded
    // by kMaxHistoryWindow and never sees data that grows.
    uint8_t order[kMaxHistoryWindow];
    for (size_t i = 0; i < n; ++i) order[i] = static_cast<uint8_t>(i);
    for (size_t i = 0; i + 1 < n; ++i) {
        size_t least = i;
        for (size_t j = i + 1; j < n; ++j)
            if (timesUs[order[j]] < timesUs[order[least]]) least = j;
        const uint8_t t = order[i]; order[i] = order[least]; order[least] = t;
    }

    ClearHistory(PoolOf(h), ci, container);
    const uint64_t before = Rd<uint64_t>(ci, facts::PHYS_CI_HIST_COUNT_OFF);
#if defined(W1E_MUTATE_GATE_WRITE_ELEMENT_DIVERGE)
    // Mutation control: AUD-W2-B-POSEREAD blocker 1 -- the push re-deriving its element from
    // the handle, so on the Pool arm the gate vouches for one object and the ring nodes land
    // on another. Only a fixture that can COMPLETE an install sees this.
    for (size_t i = 0; i < n; ++i) PushHistory(h, samples[order[i]], timesUs[order[i]]);
#else
    for (size_t i = 0; i < n; ++i)
        PushHistoryInto(h, PoolOf(h), ci, container, samples[order[i]], timesUs[order[i]]);
#endif

    // PushHistory's own exhaustion arm retires this CI's oldest sample to make room, so a short
    // pool would leave a WINDOW that is quietly missing its oldest entries. Comparing the count
    // is how that becomes a false return instead of a plausible wrong answer.
    if (Rd<uint64_t>(ci, facts::PHYS_CI_HIST_COUNT_OFF) - before != n) {
        ir.why = Refusal::ShortInstall;
        return false;
    }
    return true;
}

size_t Entity::ReadHistoryWindow(PhysHandle h, BodyState* samples,
                                 uint64_t* timesUs, size_t cap) const {
    return ReadHistoryWindowImpl(h, samples, timesUs, cap, /*decoupled=*/false);
}

// The remote-replay slot surface's read-back counter (apply.cpp bodies_read_back). It resolves the
// same element the driven install wrote by dropping only the ci[+0] field compare (reverse-map guard
// kept), so a working install reads its own node back instead of refusing at CiEntityMismatch.
size_t Entity::ReadHistoryWindowDriven(PhysHandle h, BodyState* samples,
                                       uint64_t* timesUs, size_t cap) const {
    return ReadHistoryWindowImpl(h, samples, timesUs, cap, /*decoupled=*/true);
}

size_t Entity::ReadHistoryWindowImpl(PhysHandle h, BodyState* samples,
                                     uint64_t* timesUs, size_t cap, bool decoupled) const {
    if (!samples || !timesUs || cap == 0) return 0;
    // The gated element, because ApplyStats counts bodies_read_back from here while the pawn line
    // prints read= from ReadBodyDetailed. Two surfaces asked of one handle resolved two different
    // elements on the Pool arm, so a line could print a refusal while the counter scored a read.
    replay::CiPath from = replay::CiPath::NotAsked;
    uint16_t ciIndex = 0;
    BodyReading rd;
    const void* ci = ResolveVouched(h, from, ciIndex, rd, decoupled);
    if (!ci) return 0;
    const void* container = Deref(ci, facts::PHYS_CI_HIST_LIST_OFF);
    if (!container) return 0;

    const uint32_t bits = Rd<uint32_t>(ci, facts::PHYS_CI_FLAGS_OFF);
    size_t written = 0;
    const void* node = Deref(container, facts::PHYS_LIST_HEAD_PTR_OFF);
    // +8 walks toward OLDER, so this comes out newest-first -- the order the interpolator sees
    // and therefore the order a gate should be comparing against.
    for (; node && written < cap; node = Deref(node, facts::PHYS_NODE_NEXT_OFF)) {
        BodyState& s = samples[written];
        void* nd = const_cast<void*>(node);
        memcpy(s.com,         At(nd, facts::PHYS_NODE_COM_OFF),      sizeof s.com);
        memcpy(s.ori,         At(nd, facts::PHYS_NODE_ORI_OFF),      sizeof s.ori);
        memcpy(s.vel,         At(nd, facts::PHYS_NODE_VEL_OFF),      sizeof s.vel);
        memcpy(s.angVel,      At(nd, facts::PHYS_NODE_ANGVEL_OFF),   sizeof s.angVel);
        memcpy(s.lastHeldCom, At(nd, facts::PHYS_NODE_LASTHELD_OFF), sizeof s.lastHeldCom);
        s.bits11 = replay::BodyWireBits(bits);
        s.owner_slot = replay::kBodyOwnerNone;
        timesUs[written] = Rd<uint64_t>(node, facts::PHYS_NODE_TIME_OFF);
        ++written;
    }
    return written;
}

// The local-pawn surface's answer to "which handles does this slot's pawn carry", beside
// ModelVisible because both walk the same actor. ARCH-23 F-B: this is the surface the capture read
// the recorder's own pawn through, and until now nothing on the playback side could ask it.
replay::IEntity::LocalPawn Entity::LocalPawnAt(UserIndex u,
                                               replay::IEntity::LocalPawnRef& out) const {
    using Refusal = replay::IEntity::LocalPawn;
    out = replay::IEntity::LocalPawnRef();
    if (!bcastCs_) return Refusal::NoBroadcaster;

    const size_t kNone = static_cast<size_t>(-1);
    size_t poolIndex = kNone;
    const size_t n = BroadcastCount();
    for (size_t i = 0; i < n; ++i) {
        if (LocalPlayerAt(i).v != u.v) continue;
        if (poolIndex != kNone) { out.duplicateInPool = true; break; }
        poolIndex = i;
    }
    if (poolIndex == kNone) return Refusal::SlotNotInPool;
    out.inPool = true;

    replay::IEntity::LocalPlayerReading r;
    const uint8_t* actor = LocalActorGuid(u, poolIndex, r);
    if (!actor) return Refusal::ActorNull;

    // FirstActorComponent was proved against the ECHO and BODY systems' vtable slots at bind time.
    // The physics system is a THIRD object and its slot is checked here rather than assumed:
    // calling through an unproved vtable slot on the tick thread is not a resolve.
    const void* vtbl = phys_.cs ? Deref(phys_.cs, 0) : nullptr;
    if (!findComp_ || !vtbl ||
        Rd<const void*>(vtbl, lp::kFindActorComponentSlotOff) !=
            reinterpret_cast<const void*>(findComp_)) {
        return Refusal::NoFindComponent;
    }

    const uint16_t h = ComponentOnActor(phys_.cs, actor);
    if (h == lp::kNoComponent) return Refusal::NoPhysComponent;
    out.phys = PhysHandle(h);
    // The model ENTRY resolving is the question here -- whether it is currently visible is a
    // different one, and a pawn that is physical and invisible still has to be driven.
    bool visible = false;
    out.model = ModelVisible(u, visible);
    return Refusal::Ok;
}

namespace {

// WHICH of the numbers in hand names this instance is what the identity proof just decided, and the
// key is that one: the caller's handle where the forward map addressed the CI, the CI's own field
// where only it round-tripped, and the reverse map's id otherwise -- including at entity id 0,
// where the unique-index proof stands and the reverse map is what named it.
uint16_t ProvedEntityKey(uint16_t handle, replay::CiPath from, const BodyReading& r) {
    return from == replay::CiPath::Handle                          ? handle
         : r.diag.identity == replay::IdentityProof::CiIdRoundTrip ? r.diag.ciEntityId
                                                                   : r.diag.revHandle;
}

}  // namespace

// The key is read through the SAME two gates every body read passes: ResolveCi bounds the handle
// against the live count and hands back a span-proved element, CheckCiIdentity proves the element
// is the one this handle names. A key taken off an unproved CI is a confident wrong answer, which
// is the one thing an instrument that settles FACT-PP1 must not produce.
replay::IEntity::EntityKey Entity::EntityKeyOf(UserIndex u, replay::IEntity::PawnSurface surface,
                                               uint16_t& out) const {
    using Key = replay::IEntity::EntityKey;
    if (!Available()) return Key::Unavailable;

#if defined(W1E_MUTATE_ENTITYKEY_SURFACE)
    // Mutation control: the surface the caller named is ignored and the other one is asked, so a
    // local pawn is answered with the remote pool's entity and the two arms of FACT-PP1 that
    // differ only in which surface holds the entrant become indistinguishable.
    const replay::IEntity::PawnSurface asked =
        surface == replay::IEntity::PawnSurface::Local ? replay::IEntity::PawnSurface::Remote
                                                       : replay::IEntity::PawnSurface::Local;
#else
    const replay::IEntity::PawnSurface asked = surface;
#endif

    uint16_t h = lp::kNoComponent;
    if (asked == replay::IEntity::PawnSurface::Remote) {
        const PlayerHandle p = PlayerOf(u);
        if (p.v == lp::kNoComponent) return Key::NoPawn;
        h = PhysOf(p).v;
    } else {
        replay::IEntity::LocalPawnRef ref;
        if (LocalPawnAt(u, ref) != replay::IEntity::LocalPawn::Ok) return Key::NoPawn;
        h = ref.phys.v;
    }
    if (h == lp::kNoComponent) return Key::NoPawn;

    BodyReading r;
    uint16_t ciIndex = 0;
    replay::CiPath from = replay::CiPath::NotAsked;
    const void* ci = ResolveCi(PhysHandle(h), ciIndex, from, r);
    if (!ci) return Key::NoCi;
    if (!CheckCiIdentity(PhysHandle(h), ciIndex, from, ci, r)) return Key::IdentityRefused;
    out = ProvedEntityKey(h, from, r);
    return Key::Ok;
}

// The key of the element the driven surface writes for `h`: the same resolve and the same relaxed
// gate as SetHistoryWindowDriven and ReadBodyDetailedDriven, so the id names what is being driven.
replay::IEntity::EntityKey Entity::EntityKeyOfDriven(PhysHandle h, uint16_t& out) const {
    using Key = replay::IEntity::EntityKey;
    if (!Available()) return Key::Unavailable;
    if (h.v == lp::kNoComponent) return Key::NoPawn;

    BodyReading r;
    uint16_t ciIndex = 0;
    replay::CiPath from = replay::CiPath::NotAsked;
    const void* ci = ResolveCi(h, ciIndex, from, r);
    if (!ci) return Key::NoCi;
    if (!CheckCiIdentity(h, ciIndex, from, ci, r, /*dropCiFieldCompare=*/true)) {
        return Key::IdentityRefused;
    }
    out = ProvedEntityKey(h.v, from, r);
    return Key::Ok;
}

// P3-MOTION.  The handle the slot bind produces for a remote entrant, PhysOf(PlayerOf(u)), is
// refused by the pose-write gate when the phys CI it addresses carries an entity id that does not
// round-trip to it (DIAG-ENTRANT-MOTION-TRACK).  Read that handle through the SAME gate the write
// passes -- so `diag` carries CI[+0] and the reverse-map entry whichever way it goes -- and, when
// it is refused, ask the engine's OWN actor->component resolver for the entrant's phys handle and
// validate THAT one.  A handle is handed back to bind ONLY when it vouches; otherwise the caller
// keeps the old handle, which the write refuses exactly as it does today (safe under Scenario B).
replay::IEntity::PhysVouch Entity::VouchPhysForUser(UserIndex u) const {
    replay::IEntity::PhysVouch v;
    if (!Available()) return v;

    const PlayerHandle p = PlayerOf(u);
    if (p.v == lp::kNoComponent) return v;      // no remote player CI for this slot
    const uint16_t bh = PhysOf(p).v;
    v.boundHandle = bh;
    if (bh == lp::kNoComponent) return v;

    // The bound handle through ResolveVouched.  Its BodyReading fills ciEntityId (CI[+0]) and
    // revHandle before the equality gate that refuses it, so the diag is the A/B discriminator
    // whether or not the handle vouches.
    {
        replay::CiPath from = replay::CiPath::NotAsked;
        uint16_t ciIndex = 0;
        BodyReading r;
        if (ResolveVouched(PhysHandle(bh), from, ciIndex, r)) {
            v.diag = r.diag;
            v.boundVouched = true;
            v.vouched = true;
            v.vouchedHandle = bh;
            return v;                            // today's handle already vouches: nothing to fix
        }
        v.diag = r.diag;
    }

    // The bound handle is refused.  Re-resolve through the engine's own actor->component map, the
    // same call LocalPawnAt makes at entity.cpp:2366 and ModelVisible at :2435.  The physics system
    // is a third object; its FindActorComponent vtable slot is proved before a call through it,
    // never assumed -- calling through an unproved slot on the tick thread is not a resolve.
    if (!userActor_ || !phys_.cs || !bcastCs_) return v;
    void* netGame = Deref(bcastCs_, lp::kBroadcasterNetGameOff);
    if (!netGame) return v;
    const void* vtbl = Deref(phys_.cs, 0);
    if (!findComp_ || !vtbl ||
        Rd<const void*>(vtbl, lp::kFindActorComponentSlotOff) !=
            reinterpret_cast<const void*>(findComp_)) {
        return v;
    }
    if (u.v >= facts::ENTITY_USER_SLOT_TABLE_MAX) return v;
    const void* rec = At(netGame, lp::kNetGameUserActorOff + lp::kNetGameUserStride * u.v);
    if (!replay::Readable(rec, lp::kActorGuidSize)) return v;

    uint8_t actor[lp::kActorGuidSize] = {};
    userActor_(netGame, actor, u.v);
    if (AllZero(actor, sizeof actor)) return v;

    const uint16_t h = ComponentOnActor(phys_.cs, actor);
    v.actorComponent = h;
    if (h == lp::kNoComponent) return v;        // Scenario B: no phys component on this actor
    v.componentPresent = true;

    replay::CiPath from = replay::CiPath::NotAsked;
    uint16_t ciIndex = 0;
    BodyReading r;
    if (ResolveVouched(PhysHandle(h), from, ciIndex, r)) {
        v.vouched = true;
        v.vouchedHandle = h;
    }
    return v;
}

// Resolve the SActorGUID for a slot into `actor` (16 bytes) and return the ncaModel component handle
// on it, or kNoComponent if the model system, the netgame or the actor does not resolve. This is the
// stride-INDEPENDENT presence signal: it reads only through the engine's own FirstActorComponent
// (findComp_ / vtable+424, ratified at InitLocalPawn), never through the CModelCS CI pool.
uint16_t Entity::ModelHandleFor(UserIndex u, uint8_t actor[lp::kActorGuidSize]) const {
    if (!userActor_ || !model_.cs || !bcastCs_) return lp::kNoComponent;
    void* netGame = Deref(bcastCs_, lp::kBroadcasterNetGameOff);
    if (!netGame) return lp::kNoComponent;
    userActor_(netGame, actor, u.v);
    return ComponentOnActor(model_.cs, actor);
}

bool Entity::ModelVisible(UserIndex u, bool& visible) const {
    visible = false;
    // PRESENCE is the ncaModel component being ON the actor, read stride-independently. It does NOT
    // depend on the CModelCS CI pool layout: VIS.MODEL_CI_STRIDE (16) is REFUTED -- no CModelCS
    // element is 16 bytes (across 1034 CS getters not one strides by 16; the 16 was mis-derived from
    // the swap notifier's 16-byte TLS event record), so a read at that stride cannot decide presence.
    // The handle from FirstActorComponent can, and it is what the engine's own model resolver uses.
    uint8_t actor[lp::kActorGuidSize] = {};
    const uint16_t handle = ModelHandleFor(u, actor);
    if (handle == lp::kNoComponent) return false;

    // BEST-EFFORT draw-bit, for the `visible` diagnostic ONLY -- it does not gate the return. The CI
    // stride is unratified, so this is a hint, not a fact; a caller trusts the return value (present)
    // over `visible`, and a refused span leaves visible=false while presence still holds. The offsets
    // +0xD0 (handle map) and +0x100 (pool base) ARE ratified; only the element stride is not.
    const void* map = Deref(model_.cs, lp::kModelCsHandleMapOff);
    void* base = Deref(model_.cs, lp::kModelCsPoolBaseOff);
    if (map && base) {
        const size_t index = Rd<uint16_t>(map, static_cast<uint32_t>(4u * handle));
        const uint8_t* proved =
            modelSpan_.Prove(base, static_cast<size_t>(lp::kModelCiStride) * (index + 1u));
        if (proved) {
            const uint8_t* ci = proved + static_cast<size_t>(lp::kModelCiStride) * index;
            if (TryTouchSpan(ci, lp::kModelCiStride))
                visible = Rd<uint32_t>(ci, lp::kModelCiVisibleOff) != 0;
        }
    }
    return true;
}

uint16_t Entity::ModelComponentHandle(UserIndex u) const {
    uint8_t actor[lp::kActorGuidSize] = {};
    return ModelHandleFor(u, actor);
}

bool Entity::ModelSystemResolved() const { return model_.cs != nullptr; }

// The EchoUnit component on the slot's actor -- the same two-step ModelHandleFor makes, against the
// other pool. Read-only since CLEANUP-1: the census asks it to report which actors carry what, and
// nothing writes through the handle any more.
// ★ A REPLAYED PAWN HAS TWO CANDIDATE ACTORS AND ONLY ONE OF THEM CARRIES COMPONENTS.
// CR15NetGame::UserActor answers off the net game's USER RECORD; the broadcaster's parallel key
// array answers off the broadcast ELEMENT, per pool index. For the local pawn they agree, which is
// why the thruster/grab/boost bind was written against UserActor alone and passed every test.
// For the eight emulated-join remotes they do NOT: PRESENT-F measured the UserActor GUID resolving
// non-zero and distinct per slot and carrying NONE of EchoUnit, ncaModel or CR15Body, while 22
// EchoUnit components existed in the same session. LocalActorGuid has preferred the key array since
// L2 for exactly this reason; this is that preference, applied to every user rather than the local
// one. The pool index is the key array's index, so the user slot has to be turned into one.
bool Entity::BroadcastIndexOf(UserIndex u, size_t& out) const {
    const size_t n = BroadcastCount();
    for (size_t i = 0; i < n; ++i) {
        if (LocalPlayerAt(i).v == u.v) {
            out = i;
            return true;
        }
    }
    return false;
}

bool Entity::BroadcastKeyActor(UserIndex u, uint8_t actor[lp::kActorGuidSize]) const {
    size_t poolIndex = 0;
    if (!bcastCs_ || !BroadcastIndexOf(u, poolIndex)) return false;
    const void* keys = Deref(bcastCs_, lp::kBroadcasterKeysOff);
    if (!keys) return false;
    // BroadcastElement tests its element and this array is read raw, so the span is proved here:
    // a wrong LOCAL.BROADCASTER_CS_KEYS_OFF must be a refusal and never a fault on the tick thread.
    const void* g =
        At(const_cast<void*>(keys), static_cast<uint32_t>(lp::kActorGuidSize * poolIndex));
    if (!replay::Readable(g, lp::kActorGuidSize)) return false;
    memcpy(actor, g, lp::kActorGuidSize);
    return !AllZero(actor, lp::kActorGuidSize);
}

uint16_t Entity::EchoUnitComponentHandle(UserIndex u) const {
    replay::IEntity::EchoBind b;
    ProbeEchoUnitBind(u, b);
    return b.echoHandle;
}

// The actor index every component system carries: a sorted array of 24-byte (actor, symbol) ->
// SComponentID entries at cs+0x110 with its element count at cs+0x118, which is what the engine's
// own actor->component resolver binary-searches (sub_140D53DA0: `v13 + 24 * v14`, bound `a1[35]`).
// Read as a POPULATION only -- how many components of this kind exist anywhere in the session.
static uint32_t ComponentPopulation(const void* cs) {
    if (!cs) return 0;
    if (!TryTouchSpan(cs, lp::kCsActorIndexCountOff + sizeof(uint64_t))) return 0;
    const uint64_t n = Rd<uint64_t>(cs, lp::kCsActorIndexCountOff);
    return n > 0xFFFFu ? 0xFFFFu : static_cast<uint32_t>(n);
}

// One pool's share of the census. The table is the engine's own sorted {SActorGUID, SComponentID}
// index -- 16 bytes of GUID then the u16 handle, 24-byte stride -- so a row is the engine saying
// "this actor carries this component", with no belief of ours in between. Every element is proved
// before it is read: a wrong count or a stale base must be a short census, never a fault.
uint32_t Entity::CensusPool_(void* cs, replay::IEntity::CensusPool pool, bool withPos,
                             replay::IEntity::CensusRow* out, uint32_t cap, uint32_t written) const {
    if (!cs || written >= cap) return written;
    if (!TryTouchSpan(cs, lp::kCsActorIndexCountOff + sizeof(uint64_t))) return written;
    const uint64_t n = Rd<uint64_t>(cs, lp::kCsActorIndexCountOff);
    const void* base = Deref(cs, lp::kCsActorIndexOff);
    if (!base || n == 0 || n > 0xFFFFu) return written;
    for (uint64_t i = 0; i < n && written < cap; ++i) {
        const void* e = At(const_cast<void*>(base), static_cast<uint32_t>(kCensusStride * i));
        if (!replay::Readable(e, kCensusStride)) break;
        replay::IEntity::CensusRow& row = out[written++];
        row.pool = pool;
        row.actorLo = Rd<uint64_t>(e, 0);
        row.actorHi = Rd<uint64_t>(e, 8);
        row.component = Rd<uint16_t>(e, 16);
        if (withPos && baseXf_ && row.component != lp::kNoComponent) {
            Transform xf{};
            baseXf_(cs, &xf, row.component);
            memcpy(row.pos, xf.pos, sizeof row.pos);
            row.havePos = true;
        }
    }
    return written;
}

uint32_t Entity::ComponentCensus(replay::IEntity::CensusRow* out, uint32_t cap) const {
    if (!out || cap == 0) return 0;
    using Pool = replay::IEntity::CensusPool;
    uint32_t n = CensusPool_(echo_.cs, Pool::EchoUnit, true, out, cap, 0);
    n = CensusPool_(model_.cs, Pool::NcaModel, false, out, cap, n);
    return CensusPool_(body_.cs, Pool::Body, false, out, cap, n);
}

// One class's pool descriptor and prefab name, or false. Every read is guarded and a row that
// reads back nonsense (no name, a capacity outside the sane band, more free than capacity) is a
// refusal rather than a zero: the scan below walks a table whose length the space does not
// publish, so "this index is not a class" has to be distinguishable from "this class is full".
static bool ReadActorPoolIn(void* gameSpace, uint16_t classId,
                            replay::IEntity::ActorPoolRow& out) {
    out = replay::IEntity::ActorPoolRow();
    if (!gameSpace) return false;

    const void* classTable = Deref(gameSpace, lp::kSpaceClassTableOff);
    if (!replay::Readable(classTable, lp::kClassNameTableOff + sizeof(void*))) return false;
    const uint64_t* names =
        static_cast<const uint64_t*>(Deref(classTable, lp::kClassNameTableOff));
    if (!replay::Readable(names + classId, sizeof(uint64_t))) return false;
    const uint64_t prefabId = names[classId];
    if (prefabId == 0 || prefabId == UINT64_MAX) return false;

    void* poolTable = Deref(gameSpace, lp::kSpacePoolTableOff);
    if (!poolTable) return false;
    const void* desc = At(poolTable, lp::kPoolDescStride * classId);
    if (!replay::Readable(desc, lp::kPoolDescCapOff + sizeof(uint16_t))) return false;
    const uint32_t capacity = Rd<uint16_t>(desc, lp::kPoolDescCapOff);
    const uint64_t freeSlots = Rd<uint64_t>(desc, lp::kPoolDescFreeOff);
    if (capacity == 0 || capacity > lp::kPoolCapSane || freeSlots > capacity) return false;

    out.classId = classId;
    out.prefabId = prefabId;
    out.capacity = capacity;
    out.freeSlots = static_cast<uint32_t>(freeSlots);

    // The next entry the allocator would pop, and its generation. Read exactly as sub_1404F4420 and
    // sub_1404E61E0 read them, and left at "none" whenever any link is unreadable -- this is
    // instrumentation and must never be the reason a row is refused.
    if (freeSlots > 0) {
        const uint16_t* stack = static_cast<const uint16_t*>(Deref(desc, lp::kPoolDescFreeStackOff));
        if (replay::Readable(stack + (freeSlots - 1), sizeof(uint16_t))) {
            const uint16_t idx = stack[freeSlots - 1];
            if (idx < capacity) {
                out.nextIndex = idx;
                const uint16_t* gens =
                    static_cast<const uint16_t*>(Deref(desc, lp::kPoolDescGenTableOff));
                if (replay::Readable(gens + idx, sizeof(uint16_t))) out.nextGen = gens[idx];
            }
        }
    }
    return true;
}

// These three read the PAWNS' space and only that one, because `gameSpace_` is
// the space the entity surface bound to. The level keeps its actors in more than one -- the disc's
// lives in another -- so an absence any of them reports is an absence IN THAT SPACE and nothing more,
// which is the error RING-1's two "absences" were. A question about another space goes through the
// explicit-space forms: ActorPoolInSpace, ScanSpaceSystems, and the `space` field on ScriptCensus and
// NetTouchReading.
bool Entity::ReadActorPool(uint16_t classId, replay::IEntity::ActorPoolRow& out) const {
    return ReadActorPoolIn(gameSpace_, classId, out);
}

uint32_t Entity::ActorPoolCensus(replay::IEntity::ActorPoolRow* out, uint32_t cap) const {
    if (!out || cap == 0) return 0;
    uint32_t n = 0;
    for (uint32_t c = 0; c < lp::kClassScanMax && n < cap; ++c) {
        if (ReadActorPool(static_cast<uint16_t>(c), out[n])) ++n;
    }
    return n;
}

bool Entity::ActorPoolFor(uint64_t prefabId, replay::IEntity::ActorPoolRow& out) const {
    return replay::IEntity::ActorPoolInSpace(gameSpace_, prefabId, out);
}

// P0 reading 3. The space's component-system table is the same one CS_LOOKUP_BY_SYMBOL searches --
// {u64 CSymbol64, u64 cs} pairs -- so walking it and bucketing each system's actor index by the
// actor id's low word says, for every component the level knows about, how many actors of a given
// CLASS carry it. That is the difference between "our pawns are missing a component" and "the
// prefab never had one". The walk never sorts the table and never clears its unsorted mark.
uint32_t Entity::SystemCensus(const uint16_t* classes, uint32_t nClasses,
                              replay::IEntity::SystemRow* out, uint32_t cap) const {
    using Row = replay::IEntity::SystemRow;
    if (!out || cap == 0 || !gameSpace_) return 0;
    if (!classes) nClasses = 0;
    if (nClasses > Row::kClasses) nClasses = Row::kClasses;
    if (!TryTouchSpan(gameSpace_, lp::kSystemTableCountOff + sizeof(uint64_t))) return 0;
    const uint64_t systems = Rd<uint64_t>(gameSpace_, lp::kSystemTableCountOff);
    void* const base = Deref(gameSpace_, lp::kGameSpaceSystemTableOff);
    if (!base || systems == 0 || systems > kSystemScanMax) return 0;

    uint32_t written = 0;
    for (uint64_t i = 0; i < systems && written < cap; ++i) {
        const void* entry = At(base, static_cast<uint32_t>(lp::kSystemTableStride * i));
        if (!replay::Readable(entry, static_cast<size_t>(lp::kSystemTableStride))) break;
        Row& row = out[written++];
        row = Row();
        row.symbol = Rd<uint64_t>(entry, 0);
        void* const cs = Deref(const_cast<void*>(entry), lp::kSystemTableEntryCsOff);
        if (!cs || !TryTouchSpan(cs, lp::kCsActorIndexCountOff + sizeof(uint64_t))) continue;
        const uint64_t rows = Rd<uint64_t>(cs, lp::kCsActorIndexCountOff);
        void* const index = Deref(cs, lp::kCsActorIndexOff);
        if (!index || rows == 0 || rows > 0xFFFFu) continue;
        row.rows = static_cast<uint32_t>(rows);
        for (uint64_t r = 0; r < rows; ++r) {
            const void* element = At(index, static_cast<uint32_t>(kCensusStride * r));
            if (!replay::Readable(element, kCensusStride)) break;
            const uint16_t cls = static_cast<uint16_t>(Rd<uint64_t>(element, 0) & 0xFFFFu);
            for (uint32_t k = 0; k < nClasses; ++k) {
                if (classes[k] == cls) ++row.perClass[k];
            }
        }
    }
    return written;
}

// DISC-RING-1. The net-touch system, read-only, found by scanning the space's own system table for
// its symbol -- the same table CS_LOOKUP_BY_SYMBOL searches, already guarded above, so the census
// needs no engine call to answer. Every field read here is one sub_140D70F40 reads or writes on
// the same pass, which is why the latch bit is evidence and not a restatement of our own publish:
// the engine sets it, we only read it.
bool Entity::NetTouchCensus(replay::IEntity::NetTouchReading& out) const {
    void* const gameSpace = out.space ? out.space : gameSpace_;
    out = replay::IEntity::NetTouchReading();
    out.space = gameSpace;
    if (!gameSpace) { out.refused = "no game space is bound"; return false; }
    if (!TryTouchSpan(gameSpace, lp::kSystemTableCountOff + sizeof(uint64_t))) {
        out.refused = "the space's component-system table is not readable";
        return false;
    }
    const uint64_t systems = Rd<uint64_t>(gameSpace, lp::kSystemTableCountOff);
    void* const table = Deref(gameSpace, lp::kGameSpaceSystemTableOff);
    if (!table || systems == 0 || systems > kSystemScanMax) {
        out.refused = "the component-system table is empty or implausibly long";
        return false;
    }
    void* cs = nullptr;
    for (uint64_t i = 0; i < systems; ++i) {
        void* const entry = At(table, static_cast<uint32_t>(lp::kSystemTableStride * i));
        if (!replay::Readable(entry, static_cast<size_t>(lp::kSystemTableStride))) break;
        ++out.systemsSeen;
        const uint64_t symbol = Rd<uint64_t>(entry, 0);
        if (symbol == facts::CS_SYM_R14NETPHYSICS) out.controlFound = true;
        if (symbol == lp::kNetTouchSymbol && !cs) cs = Deref(entry, lp::kSystemTableEntryCsOff);
    }
    if (!cs) {
        out.refused = "this level publishes no R14NetTouchInteract system";
        return false;
    }
    if (!TryTouchSpan(cs, facts::PHYS_CS_POOL_BASE_OFF + sizeof(void*))) {
        out.refused = "the R14NetTouchInteract system is not readable";
        return false;
    }
    out.read = true;
    out.refused = nullptr;
    out.cs = reinterpret_cast<uint64_t>(cs);
    out.ciCount = Rd<uint16_t>(cs, facts::PHYS_CS_COUNT_OFF);
    void* const base = Deref(cs, facts::PHYS_CS_POOL_BASE_OFF);

    // The netgame the update itself gates on, off the physics system the plugin already bound --
    // the same three links EngineHolderOf walks, so a disagreement between the two is visible.
    // ...off the physics system of whichever space was asked for: the update gates on the netgame
    // of the space its own components live in, and the disc's space is not the pawns'.
    const Pool& bodies =
        out.space && out.space != gameSpace_ && disc_.cs ? disc_ : phys_;
    void* const physCs = bodies.cs;
    void* const host = physCs && replay::Readable(At(physCs, kPhysCsHostOff), sizeof(void*))
                           ? Deref(physCs, kPhysCsHostOff) : nullptr;
    void* const owner = host && replay::Readable(At(host, kHostOwnerOff), sizeof(void*))
                            ? Deref(host, kHostOwnerOff) : nullptr;
    void* const netGame = owner && replay::Readable(At(owner, kOwnerNetGameOff), sizeof(void*))
                              ? Deref(owner, kOwnerNetGameOff) : nullptr;
    if (netGame && replay::Readable(At(netGame, kRemoteBandBeginOff), sizeof(uint16_t))) {
        out.netGameRead  = true;
        out.netGameState = Rd<uint32_t>(netGame, lp::kNetGameStateOff);
        out.bandLo = Rd<uint16_t>(netGame, kRemoteBandBeginOff);
        out.bandHi = Rd<uint16_t>(netGame, kRemoteBandEndOff);
    }
    // The two sibling systems sub_140CEDEB0 resolves the holder through. Read from the net-touch
    // system itself, so the chain below is the engine's own and not a second copy of this plugin's.
    void* const engPhysCs = replay::Readable(At(cs, lp::kNetTouchCsPhysCsOff), sizeof(void*))
                                ? Deref(cs, lp::kNetTouchCsPhysCsOff) : nullptr;
    void* const engTouchCs = replay::Readable(At(cs, lp::kNetTouchCsTouchCsOff), sizeof(void*))
                                 ? Deref(cs, lp::kNetTouchCsTouchCsOff) : nullptr;
    out.csPhys  = reinterpret_cast<uint64_t>(engPhysCs);
    out.csTouch = reinterpret_cast<uint64_t>(engTouchCs);

    if (!base) return true;

    for (uint32_t i = 0; i < out.ciCount && out.rowsWritten < replay::IEntity::NetTouchReading::kRowCap;
         ++i) {
        const void* ci = At(base, lp::kNetTouchCiStride * i);
        if (!replay::Readable(ci, lp::kNetTouchCiStride)) break;
        replay::IEntity::NetTouchRow& row = out.rows[out.rowsWritten++];
        row.ciIndex   = static_cast<uint16_t>(i);
        row.entity    = Rd<uint16_t>(ci, 0);
        row.physId    = Rd<uint32_t>(ci, lp::kNetTouchCiPhysIdOff);
        row.touchId   = Rd<uint32_t>(ci, lp::kNetTouchCiTouchIdOff);
        row.lastOwner = Rd<uint32_t>(ci, lp::kNetTouchCiLastOwnerOff);
        row.flags     = Rd<uint8_t>(ci, lp::kNetTouchCiFlagsOff);
        row.heldLatch = (row.flags & lp::kNetTouchHeldLatchBit) != 0;
        // The body the update asks the holder of. Its low half is the R14NetPhysics handle, which
        // is the namespace the plugin's own owner stamp lands in, so the two lines can be read
        // against each other without a mapping.
        const uint16_t physHandle = static_cast<uint16_t>(row.physId);

        // The IF arm's second gate, resolved the way sub_14075FD80 resolves the same system: the
        // forward map at +200 answers an index, and the live count at +244 is the bound it has to
        // clear. A touch handle the system does not own sends the holder resolve down its ELSE
        // arm, which answers "nobody" for a body no local attach ever touched.
        const uint16_t touchHandle = static_cast<uint16_t>(row.touchId);
        void* const touchMap = engTouchCs && replay::Readable(At(engTouchCs, lp::kTouchCsMapOff), sizeof(void*))
                                   ? Deref(engTouchCs, lp::kTouchCsMapOff) : nullptr;
        if (touchMap && touchHandle != lp::kNoComponent &&
            replay::Readable(At(touchMap, 4u * touchHandle), sizeof(uint32_t))) {
            row.touchCsRead = true;
            row.touchCount  = Rd<uint16_t>(engTouchCs, lp::kTouchCsCountOff);
            row.touchIndex  = Rd<uint16_t>(touchMap, 4u * touchHandle);
            row.touchLive   = row.touchIndex < row.touchCount;
            void* const tbase = Deref(engTouchCs, lp::kTouchCsPoolBaseOff);
            const void* tci = row.touchLive && tbase ? At(tbase, lp::kTouchCiStride * row.touchIndex) : nullptr;
            if (tci && replay::Readable(tci, lp::kTouchCiStride)) {
                row.touchHolderA = Rd<uint32_t>(tci, lp::kTouchCiHolderAOff);
                row.touchHolderB = Rd<uint32_t>(tci, lp::kTouchCiHolderBOff);
                row.touchHand    = Rd<uint32_t>(tci, lp::kTouchCiHandOff);
                row.touchFlags   = Rd<uint16_t>(tci, lp::kTouchCiFlagsOff);
            }
        }

        if (physHandle == lp::kNoComponent || !bodies.cs) continue;
        const void* pci = bodies.Element(physHandle);
        if (!pci || !replay::Readable(pci, facts::PHYS_CI_STRIDE)) continue;
        row.physRead  = true;
        row.physOwner = Rd<uint32_t>(pci, facts::PHYS_CI_OWNER_OFF);
        row.physFlags = Rd<uint16_t>(pci, facts::PHYS_CI_FLAGS_OFF);

        // The same element addressed the update's way, and the remote arm's last term off it.
        void* const engMap = engPhysCs && replay::Readable(At(engPhysCs, facts::PHYS_CS_HANDLE_MAP_OFF), sizeof(void*))
                                 ? Deref(engPhysCs, facts::PHYS_CS_HANDLE_MAP_OFF) : nullptr;
        void* const engBase = engPhysCs && replay::Readable(At(engPhysCs, facts::PHYS_CS_POOL_BASE_OFF), sizeof(void*))
                                  ? Deref(engPhysCs, facts::PHYS_CS_POOL_BASE_OFF) : nullptr;
        void* epci = nullptr;
        if (engMap && engBase && replay::Readable(At(engMap, 4u * physHandle), sizeof(uint32_t))) {
            epci = At(engBase, facts::PHYS_CI_STRIDE * Rd<uint16_t>(engMap, 4u * physHandle));
            if (!replay::Readable(epci, facts::PHYS_CI_STRIDE)) epci = nullptr;
        }
        if (!epci) continue;
        row.engSameCi = (static_cast<const void*>(epci) == pci);
        row.engOwner  = Rd<uint32_t>(epci, facts::PHYS_CI_OWNER_OFF);
        void* const list = replay::Readable(At(epci, facts::PHYS_CI_LIST_HEAD_OFF), sizeof(void*))
                               ? Deref(epci, facts::PHYS_CI_LIST_HEAD_OFF) : nullptr;
        void* const head = list && replay::Readable(At(list, kListHeadNodeOff), sizeof(void*))
                               ? Deref(list, kListHeadNodeOff) : nullptr;
        if (head && replay::Readable(At(head, facts::PHYS_NODE_STATE_OFF), sizeof(uint16_t))) {
            row.nodeRead  = true;
            row.nodeState = Rd<uint16_t>(head, facts::PHYS_NODE_STATE_OFF);
        }
    }
    return true;
}

// The /scripts/%016llx behind one ncaScript component, and the instance state beside it. Lifted
// out of ScriptSelfCensus so the disc-ring census walks the same chain rather than a second
// reading of it; every hop is the one sub_140446220 spells out.
static uint64_t ScriptHashOfCi(void* pool, uint16_t ciIndex, int32_t* stateOut,
                               replay::QueriedSpans& spans) {
    if (stateOut) *stateOut = -1;
    if (!pool || ciIndex == lp::kNoComponent) return 0;
    void* const ci = At(pool, lp::kScriptCiStride * uint32_t(ciIndex));
    if (!spans.Readable(ci, lp::kScriptCiStride)) return 0;
    void* const inst = Deref(ci, lp::kScriptCiInstanceOff);
    if (!inst || !spans.Readable(inst, lp::kScriptInstStateOff + sizeof(int32_t))) return 0;
    void* const module = Deref(inst, lp::kScriptInstModuleOff);
    if (!module || !spans.Readable(module, lp::kScriptModuleHashOff + sizeof(uint64_t))) return 0;
    if (stateOut) *stateOut = Rd<int32_t>(inst, lp::kScriptInstStateOff);
    return Rd<uint64_t>(module, lp::kScriptModuleHashOff);
}

// One actor-name symbol looked up in a space's actor registry, the way sub_1404FF1A0 looks one up:
// a lower bound over 16-byte entries keyed by the CSymbol64 at +0, then an equality test.
bool Entity::ActorIndexOfSymbol(void* space, uint64_t symbol, uint16_t& index, bool& live,
                                replay::QueriedSpans& spans) const {
    index = lp::kNoComponent;
    live = false;
    if (!space || !spans.Readable(space, lp::kGameSpaceActorRegOff + sizeof(void*))) return false;
    void* const reg = Deref(space, lp::kGameSpaceActorRegOff);
    if (!reg || !spans.Readable(reg, lp::kActorRegCountOff + sizeof(uint64_t))) return false;
    const uint64_t count = Rd<uint64_t>(reg, lp::kActorRegCountOff);
    void* const base = Deref(reg, lp::kActorRegBaseOff);
    if (!base || count == 0 || count > lp::kActorRegMaxEntries ||
        !spans.Readable(base, size_t(lp::kActorRegEntryStride) * size_t(count)))
        return false;
    uint64_t lo = 0, hi = count;
    while (lo < hi) {
        const uint64_t mid = lo + (hi - lo) / 2u;
        if (Rd<uint64_t>(base, uint32_t(lp::kActorRegEntryStride * mid)) < symbol)
            lo = mid + 1u;
        else
            hi = mid;
    }
    if (lo >= count || Rd<uint64_t>(base, uint32_t(lp::kActorRegEntryStride * lo)) != symbol)
        return false;
    index = Rd<uint16_t>(base, uint32_t(lp::kActorRegEntryStride * lo + lp::kActorRegEntryIndexOff));
    live = ActorSlotLive(space, index);
    return true;
}

// Every space the disc bind could have meant, deduplicated: the space its own chain named, the
// pawns', and each early candidate root with its parent chain. Which of them registers the system
// the bind is holding is then a reading rather than an assumption -- DISC-RING-2's second launch
// found that neither of the two spaces the bind itself names does.
//
// The parent walk reaches objects that are not spaces (RUN-VERIFY-6: the loadout actor, whose
// "registry" qword is 0x10000000A), so every read below is proved by query, never by touch.
void Entity::ScanCandidateSpaces(replay::IEntity::DiscRingReading& out) const {
    replay::QueriedSpans spans;
    using Reading = replay::IEntity::DiscRingReading;
    void* roots[2 + DiscBind::EarlyDiag::kCandidates] = {discActorSpace_, gameSpace_};
    for (size_t i = 0; i < DiscBind::EarlyDiag::kCandidates; ++i)
        roots[2 + i] = EarlySpaceRoot(discNetGame_, i, nullptr);

    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; ++i) {
        void* space = roots[i];
        for (uint32_t d = 0; space && d < lp::kSpaceParentWalkMax; ++d) {
            bool seen = false;
            for (uint32_t s = 0; s < out.scansWritten && !seen; ++s)
                seen = out.scans[s].space == reinterpret_cast<uint64_t>(space);
            if (!seen) {
                if (out.scansWritten >= Reading::kSpaceCap) return;
                ScanSpaceSystems(space, out, spans);
            }
            space = spans.Readable(space, lp::kGameSpaceParentOff + sizeof(void*))
                        ? Deref(space, lp::kGameSpaceParentOff)
                        : nullptr;
        }
    }
}

// DISC-RING-2. Which disc actor the track drives, what that actor carries, and where the scripted
// disc is if it is not that one -- read in the space the DISC was bound in, which RING-1's walk
// never was. Read-only throughout: one binary search per actor name, one walk of the frisbee
// system's actor index, and one walk of the space's system table.
bool Entity::DiscRingCensus(replay::IEntity::DiscRingReading& out) const {
    using Reading = replay::IEntity::DiscRingReading;
    out = Reading();
    out.pawnSpace = reinterpret_cast<uint64_t>(gameSpace_);
    out.boundPhysId    = discPhysId_;
    out.bindActorIndex = discActorIndex_;

    // The system the bind is holding is the one to walk -- not whatever a space happens to register
    // under the same symbol. The space is then found BY that system, because a component ref names
    // the space its component lives in and the early path names the space the actor was found in,
    // and DISC-RING-2's first launch showed those two need not answer the same way.
    void* const frisbeeCs = discFrisbeeCs_;
    if (!frisbeeCs) { out.refused = "the disc has never bound, so there is no system to walk"; return false; }
    if (!TryTouchSpan(frisbeeCs, lp::kCsActorIndexCountOff + sizeof(uint64_t))) {
        out.refused = "the bound R14Frisbee system is not readable";
        return false;
    }
    out.frisbeeLive = Rd<uint16_t>(frisbeeCs, lp::kPhysCsLiveCountOff);
    out.refused = nullptr;

    // Every actor the frisbee system holds a component for. This is the population the owner's
    // "more than one live component" refusal was counting, now named actor by actor.
    {
        void* const index = Deref(frisbeeCs, lp::kCsActorIndexOff);
        void* const pool  = Deref(frisbeeCs, lp::kFrisbeeCsPoolBaseOff);
        const uint64_t rows = Rd<uint64_t>(frisbeeCs, lp::kCsActorIndexCountOff);
        for (uint64_t r = 0; index && rows <= 0xFFFFu && r < rows &&
                             out.actorsWritten < Reading::kActorCap;
             ++r) {
            const void* element = At(index, static_cast<uint32_t>(kCensusStride * r));
            if (!replay::Readable(element, kCensusStride)) break;
            Reading::Actor& a = out.actors[out.actorsWritten++];
            a.actorId   = Rd<uint64_t>(element, 0);
            a.rowSymbol = Rd<uint64_t>(element, sizeof(uint64_t));
            a.frisbeeCi = Rd<uint16_t>(element, 2u * sizeof(uint64_t));
            a.has |= Reading::kFrisbee;
            if (!pool || a.frisbeeCi == lp::kNoComponent) continue;
            const void* ci = At(pool, lp::kFrisbeeCiStride * uint32_t(a.frisbeeCi));
            if (!replay::Readable(ci, lp::kFrisbeeCiStride)) continue;
            a.physId        = Rd<uint32_t>(ci, lp::kFrisbeeCiPhysIdOff);
            a.timeSinceHeld = Rd<float>(ci, lp::kFrisbeeCiTimeSinceHeldOff);
            a.bound = disc_.cs != nullptr && a.physId == discPhysId_;
        }
    }

    ScanCandidateSpaces(out);
    for (uint32_t i = 0; i < out.scansWritten; ++i) {
        if (!out.scans[i].isBoundCs) continue;
        out.discSpace = out.scans[i].space;
        out.spacesDiffer = out.discSpace != reinterpret_cast<uint64_t>(gameSpace_);
        break;
    }
    return true;
}

// The same candidate list ScanCandidateSpaces walks, handed out as pointers instead of scanned in
// place. `netGame` is the tick's own -- in a live spectate the disc has never bound, so
// discNetGame_ is null and the remembered spaces are null with it, and the tick's netGame is the
// only source there is.
uint32_t Entity::DiscCandidateSpaces(void* netGame, void** out, uint32_t cap) const {
    if (!out || cap == 0) return 0;
    void* const roots[2 + DiscBind::EarlyDiag::kCandidates] = {
        discActorSpace_, gameSpace_, EarlySpaceRoot(netGame, 0, nullptr),
        EarlySpaceRoot(netGame, 1, nullptr), EarlySpaceRoot(netGame, 2, nullptr)};
    static_assert(DiscBind::EarlyDiag::kCandidates == 3, "the initializer names every candidate");

    replay::QueriedSpans spans;
    uint32_t n = 0;
    for (size_t i = 0; i < sizeof roots / sizeof roots[0] && n < cap; ++i) {
        void* space = roots[i];
        for (uint32_t d = 0; space && d < lp::kSpaceParentWalkMax && n < cap; ++d) {
            bool seen = false;
            for (uint32_t s = 0; s < n && !seen; ++s) seen = out[s] == space;
            if (!seen) out[n++] = space;
            space = spans.Readable(space, lp::kGameSpaceParentOff + sizeof(void*))
                        ? Deref(space, lp::kGameSpaceParentOff)
                        : nullptr;
        }
    }
    return n;
}

// One space's component-system table, bucketed against the actors already in `out` -- the same
// table CS_LOOKUP_BY_SYMBOL searches, walked without sorting it and without clearing its mark.
void Entity::ScanSpaceSystems(void* space, replay::IEntity::DiscRingReading& out,
                              replay::QueriedSpans& spans) const {
    using Reading = replay::IEntity::DiscRingReading;
    if (out.scansWritten >= Reading::kSpaceCap) return;
    Reading::SpaceScan& scan = out.scans[out.scansWritten++];
    scan.space = reinterpret_cast<uint64_t>(space);
    if (!space) return;
    ActorIndexOfSymbol(space, lp::kSymGameFrisbee, scan.idxGameFrisbee, scan.liveGameFrisbee, spans);
    ActorIndexOfSymbol(space, kSymPracticeDisc, scan.idxPracticeDisc, scan.livePracticeDisc, spans);
    if (!spans.Readable(space, lp::kSystemTableCountOff + sizeof(uint64_t))) return;
    const uint64_t systems = Rd<uint64_t>(space, lp::kSystemTableCountOff);
    void* const table = Deref(space, lp::kGameSpaceSystemTableOff);
    if (!table || systems == 0 || systems > kSystemScanMax) return;
    scan.walked = true;

    for (uint64_t i = 0; i < systems; ++i) {
        void* const entry = At(table, static_cast<uint32_t>(lp::kSystemTableStride * i));
        if (!spans.Readable(entry, static_cast<size_t>(lp::kSystemTableStride))) break;
        ++scan.systemsSeen;
        const uint64_t symbol = Rd<uint64_t>(entry, 0);
        if (symbol == facts::CS_SYM_R14NETPHYSICS) scan.controlFound = true;
        // Found by the walk this census is already making, not by a second SystemInSpace call: the
        // census has the row in hand. The two now agree -- SystemInSpace used to refuse for the
        // very space the disc lives in, which is the defect ENG-DISC-RING-2 §5 named.
        if (symbol == lp::kSymR14Frisbee && scan.frisbeeCs == 0) {
            void* const fcs = Deref(entry, lp::kSystemTableEntryCsOff);
            scan.frisbeeCs = reinterpret_cast<uint64_t>(fcs);
            scan.isBoundCs = fcs != nullptr && fcs == discFrisbeeCs_;
        }

        uint32_t bit = 0;
        if (symbol == lp::kSymR14Frisbee)         bit = Reading::kFrisbee;
        else if (symbol == kSymTouchInteract)     bit = Reading::kTouch;
        else if (symbol == kSymNcaPhysics)        bit = Reading::kNcaPhysics;
        else if (symbol == kSymNcaScript)         bit = Reading::kNcaScript;
        else if (symbol == facts::CS_SYM_R14NETPHYSICS) bit = Reading::kNetPhysics;
        else if (symbol == lp::kNetTouchSymbol)   bit = Reading::kNetTouch;
        else if (symbol == kSymNetFrisbeeTrail)   bit = Reading::kTrail;
        const bool isScript = symbol == kSymNcaScript;

        void* const cs = Deref(entry, lp::kSystemTableEntryCsOff);
        if (!cs || !spans.Readable(cs, lp::kCsActorIndexCountOff + sizeof(uint64_t))) continue;
        const uint64_t rows = Rd<uint64_t>(cs, lp::kCsActorIndexCountOff);
        void* const index = Deref(cs, lp::kCsActorIndexOff);
        if (!index || rows == 0 || rows > 0xFFFFu) continue;
        void* const scriptPool = isScript ? Deref(cs, lp::kScriptCsPoolBaseOff) : nullptr;

        for (uint64_t r = 0; r < rows; ++r) {
            const void* element = At(index, static_cast<uint32_t>(kCensusStride * r));
            if (!spans.Readable(element, kCensusStride)) break;
            // The row's first qword is the actor -- {slot, pool, generation, pad}. Its second is
            // the owning system's own CSymbol64, so it identifies the SYSTEM and must play no part
            // in deciding whether two rows name the same actor.
            const uint64_t actorId = Rd<uint64_t>(element, 0);
            const uint16_t ci = Rd<uint16_t>(element, 2u * sizeof(uint64_t));

            int32_t state = -1;
            uint64_t hash = 0;
            if (isScript) {
                ++scan.scriptsWalked;
                hash = ScriptHashOfCi(scriptPool, ci, &state, spans);
                if (hash == kDiscScriptHash) {
                    ++scan.discScriptInstances;
                    if (out.sitesWritten < Reading::kActorCap) {
                        Reading::ScriptSite& s = out.sites[out.sitesWritten++];
                        s.actorId = actorId;
                        s.space = scan.space;
                        s.ciIndex = ci;
                        s.state = state;
                    }
                }
            }
            for (uint32_t a = 0; a < out.actorsWritten; ++a) {
                Reading::Actor& act = out.actors[a];
                if (act.actorId != actorId) continue;
                ++act.systems;
                act.has |= bit;
                if (isScript && act.scripts < Reading::kScriptCap) act.scriptHash[act.scripts] = hash;
                if (isScript) ++act.scripts;
            }
        }
    }
}

// The second argument sub_140CCE580 wants is an SCompactPoolHandle in the REMOTE PLAYER pool: the
// handle in the low half and the forward map entry's high half -- its generation -- above it. It
// was a net user id once, a different namespace, so vtable slot +472 resolved no actor and every
// delegate landed on nothing.
HandleBuild Entity::BuildDelegateHandle(PlayerHandle h, uint32_t& compact, uint16_t& index) const {
    compact = 0;
    index = lp::kNoComponent;
    uint16_t element = 0;
    if (!player_.Element(h.v, &element)) return HandleBuild::NoElement;
    uint16_t low = 0, generation = 0;
    if (!player_.MapEntry(h.v, low, generation) || low != element) return HandleBuild::NoMapEntry;
    index = element;
    compact = uint32_t(h.v) | (uint32_t(generation) << 16);
    return HandleBuild::Ok;
}

// SEH lives in a POD-only function: MSVC C2712 forbids __try beside a destructor in scope.
static bool CallActorOfHandleGuarded(ActorOfHandleFn fn, void* cs, void* scratch, uint32_t compact,
                                     void** returned) {
    __try {
        *returned = fn(cs, scratch, compact);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// P0 readings 4 and 5. One call, the identical one sub_140CCE580 makes with the identical handle,
// so the actor printed here is the actor every grab/boost/brake delegate is raised on. Beside it,
// the partition the CD sits in (a CD at or above the enabled bound runs no scripts) and the
// snapshot-history head that DESIGN-REMOTE-ACTOR 1.4 requires to stay NULL.
bool Entity::RemoteTrackProbe_(UserIndex u, replay::IEntity::RemoteTrackProbe& out) const {
    out = replay::IEntity::RemoteTrackProbe();
    if (!player_.cs) { out.refused = "no CR15RemotePlayerCS is bound"; return false; }
    if (!TryTouchSpan(player_.cs, facts::PLAYER_CS_COUNT_OFF + sizeof(uint16_t))) {
        out.refused = "the remote-player system is not readable";
        return false;
    }
    out.totalCds   = Rd<uint16_t>(player_.cs, lp::kRemoteCsTotalCdOff);
    out.enabledCds = Rd<uint16_t>(player_.cs, facts::PLAYER_CS_COUNT_OFF);

    const PlayerHandle h = PlayerOf(u);
    if (h.v == lp::kNoComponent) { out.refused = "no remote-player CD carries that user id"; return true; }
    uint16_t index = 0;
    uint32_t compact = 0;
    switch (BuildDelegateHandle(h, compact, index)) {
        case HandleBuild::NoElement:
            out.refused = "the CD element did not read back";
            return true;
        case HandleBuild::NoMapEntry:
            out.refused = "the forward map entry does not round-trip to the CD index";
            return true;
        case HandleBuild::Ok:
            break;
    }
    out.cdIndex = index;
    out.compact = compact;

    if (void* const cd = player_.Element(h.v)) {
        if (TryTouchSpan(cd, kHoldROff + sizeof(uint32_t))) {
            out.bits28 = Rd<uint32_t>(cd, facts::PLAYER_CI_ACTION_WORD_OFF);
            out.holdL = Rd<uint32_t>(cd, kHoldLOff);
            out.holdR = Rd<uint32_t>(cd, kHoldROff);
            out.actionRead = true;
        }
        if (TryTouchSpan(cd, lp::kRemoteCdHistoryOff + sizeof(void*))) {
            void* const list = Deref(cd, lp::kRemoteCdHistoryOff);
            if (!list) {
                out.historyRead = true;
            } else if (replay::Readable(list, lp::kRemoteCdHistoryHeadOff + sizeof(void*))) {
                out.historyRead = true;
                out.historyEmpty = Deref(list, lp::kRemoteCdHistoryHeadOff) == nullptr;
            }
        }
    }

    void* const vtable = Deref(player_.cs, 0);
    if (!vtable || !replay::Readable(At(vtable, lp::kRemoteCsVtblActorOfHandle), sizeof(void*))) {
        out.refused = "the remote-player vtable slot did not read back";
        return true;
    }
    ActorOfHandleFn fn = reinterpret_cast<ActorOfHandleFn>(
        Rd<void*>(vtable, lp::kRemoteCsVtblActorOfHandle));
    if (!fn) { out.refused = "the remote-player vtable slot is null"; return true; }

    // The engine writes its answer into the caller's scratch (sub_140CCE580 passes 16 bytes) and
    // returns a pointer to it: {u64 actor id, u64 gamespace name hash}.
    uint64_t scratch[8] = {};
    void* returned = nullptr;
    if (!CallActorOfHandleGuarded(fn, player_.cs, scratch, compact, &returned) || !returned ||
        !replay::Readable(returned, 2 * sizeof(uint64_t))) {
        out.refused = "the handle-to-actor resolve refused";
        return true;
    }
    out.targetLo = Rd<uint64_t>(returned, 0);
    out.targetHi = Rd<uint64_t>(returned, sizeof(uint64_t));
    out.targetRead = true;

    if (const uint32_t* const sentinel = RebasedDword(lp::kHoldSentinelVa)) {
        out.holdSentinel = *sentinel;
        out.sentinelRead = true;
    }
    out.refused = nullptr;
    return true;
}

// SEH lives in a POD-only function, same rule as CallActorOfHandleGuarded above. `get` is the
// script module's own read binding: it copies out of EXECUTION_DATA and returns 1 only when it
// recognised both the type hash and the name hash, so a graph with no `self` answers 0 and writes
// nothing.
static bool CallScriptGetGuarded(ScriptGetFn fn, void* graph, uint64_t name, uint64_t type,
                                 void* out, int* answered) {
    __try {
        *answered = fn(graph, name, type, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *answered = 0;
        return false;
    }
}

// P1C M1 and D5. Walk the ncaScript system's actor index and ask every graph on it for `self`.
// This is the only read that separates the four things the silence could be -- the script is not
// on the pawn, the graph never initialised, `self` is unbound, `self` names a different actor --
// and it is the same read for the disc's `bubble` gate, which tests `self` the same way.
// The ncaScript system's actor index in `gameSpace`: the part of the walk the census and the
// script-graph seam share.
struct ScriptIndexView {
    uint64_t rows = 0;
    void* index = nullptr;
    void* pool = nullptr;
};

static bool ResolveScriptIndex(void* gameSpace, ScriptIndexView& v, const char** why) {
    if (!gameSpace) { *why = "no game space is bound"; return false; }
    if (!TryTouchSpan(gameSpace, lp::kSystemTableCountOff + sizeof(uint64_t))) {
        *why = "the game space's component-system table is not readable";
        return false;
    }
    const uint64_t systems = Rd<uint64_t>(gameSpace, lp::kSystemTableCountOff);
    void* const table = Deref(gameSpace, lp::kGameSpaceSystemTableOff);
    if (!table || systems == 0 || systems > kSystemScanMax) {
        *why = "the component-system table did not read back";
        return false;
    }
    void* cs = nullptr;
    for (uint64_t i = 0; i < systems; ++i) {
        void* const entry = At(table, static_cast<uint32_t>(lp::kSystemTableStride * i));
        if (!replay::Readable(entry, static_cast<size_t>(lp::kSystemTableStride))) break;
        if (Rd<uint64_t>(entry, 0) != kSymNcaScript) continue;
        cs = Deref(entry, lp::kSystemTableEntryCsOff);
        break;
    }
    if (!cs) { *why = "the level registered no ncaScript system"; return false; }
    if (!TryTouchSpan(cs, lp::kCsActorIndexCountOff + sizeof(uint64_t))) {
        *why = "the ncaScript system is not readable";
        return false;
    }
    v.rows = Rd<uint64_t>(cs, lp::kCsActorIndexCountOff);
    v.index = Deref(cs, lp::kCsActorIndexOff);
    v.pool = Deref(cs, lp::kScriptCsPoolBaseOff);
    if (!v.index || !v.pool || v.rows == 0 || v.rows > 0xFFFFu) {
        *why = "the ncaScript actor index is empty or did not read back";
        return false;
    }
    return true;
}

// The instance behind one actor-index row, or null when any pointer on the way does not read back.
static void* ScriptInstOfRow(const ScriptIndexView& v, uint16_t ciIndex) {
    if (ciIndex == lp::kNoComponent) return nullptr;
    void* const ci = At(v.pool, lp::kScriptCiStride * uint32_t(ciIndex));
    if (!replay::Readable(ci, lp::kScriptCiStride)) return nullptr;
    void* const inst = Deref(ci, lp::kScriptCiInstanceOff);
    if (!inst || !replay::Readable(inst, lp::kScriptInstStateOff + sizeof(int32_t))) return nullptr;
    return inst;
}

bool Entity::ScriptSelfCensus(replay::IEntity::ScriptCensus& io) const {
    io.rowsWritten = 0;
    io.hashesWritten = 0;
    io.walked = 0;
    io.unreadable = 0;
    ScriptIndexView v;
    if (!ResolveScriptIndex(io.space ? io.space : gameSpace_, v, &io.refused)) return false;
    const uint64_t rows = v.rows;
    void* const index = v.index;
    io.refused = nullptr;

    for (uint64_t r = 0; r < rows; ++r) {
        void* const element = At(index, static_cast<uint32_t>(kCensusStride * r));
        if (!replay::Readable(element, kCensusStride)) break;
        ++io.walked;

        replay::IEntity::ScriptSelfRow row;
        row.actorLo = Rd<uint64_t>(element, 0);
        row.actorHi = Rd<uint64_t>(element, sizeof(uint64_t));
        row.ciIndex = Rd<uint16_t>(element, 2u * sizeof(uint64_t));
        if (row.ciIndex == lp::kNoComponent) { ++io.unreadable; continue; }

        void* const inst = ScriptInstOfRow(v, row.ciIndex);
        if (!inst) { ++io.unreadable; continue; }
        void* const module = Deref(inst, lp::kScriptInstModuleOff);
        if (!module || !replay::Readable(module, lp::kScriptModuleGetOff + sizeof(void*))) {
            ++io.unreadable;
            continue;
        }
        row.scriptHash = Rd<uint64_t>(module, lp::kScriptModuleHashOff);
        row.state = Rd<int32_t>(inst, lp::kScriptInstStateOff);

        // The graph is embedded at inst+24, not pointed to; its `data` is what the getter walks.
        void* const graph = At(inst, lp::kScriptInstGraphOff);
        void* const data = Deref(graph, lp::kScriptGraphDataOff);
        row.graphTime = Rd<int64_t>(graph, lp::kScriptGraphGameTimeOff);
        if (data && replay::Readable(data, sizeof row.data)) {
            memcpy(row.data, data, sizeof row.data);
            row.dataRead = true;
        }
        ScriptGetFn get = reinterpret_cast<ScriptGetFn>(Rd<void*>(module, lp::kScriptModuleGetOff));
        uint8_t var[lp::kActorVarSize] = {};
        if (get && data) {
            int answered = 0;
            if (CallScriptGetGuarded(get, graph, kSymSelf, kSymActorType, var, &answered) &&
                answered == 1) {
                row.answered = true;
                row.selfSpace = Rd<uint64_t>(var, lp::kActorVarSpaceOff);
                row.selfActor = Rd<uint64_t>(var, lp::kActorVarActorOff);
                row.selfNode = Rd<uint64_t>(var, lp::kActorVarNodeOff);
            }
            // R1. Only for the disc's own graph: every other script on the level would answer 0 to
            // all of these, and asking 8 more questions of every graph in the arena would turn a
            // one-shot into a walk. `discRead` says the attempt was made, so an all-zero row is
            // "the getter refused" and not "the census never asked".
            if (row.scriptHash == kDiscScriptHash) {
                row.discRead = true;
                uint8_t bub[lp::kActorVarSize] = {};
                answered = 0;
                if (CallScriptGetGuarded(get, graph, kSymBubble, kSymActorType, bub, &answered) &&
                    answered == 1) {
                    row.bubbleAnswered = true;
                    row.bubbleSpace = Rd<uint64_t>(bub, lp::kActorVarSpaceOff);
                    row.bubbleActor = Rd<uint64_t>(bub, lp::kActorVarActorOff);
                    row.bubbleNode = Rd<uint64_t>(bub, lp::kActorVarNodeOff);
                }
                for (uint32_t f = 0; f < replay::IEntity::kDiscFlagCount; ++f) {
                    // The flag case writes ONE byte. A wider read would take three bytes of
                    // whatever follows it and call them part of the answer.
                    uint8_t flag = 0;
                    answered = 0;
                    if (!CallScriptGetGuarded(get, graph, kDiscFlagSyms[f], kFlagType, &flag,
                                              &answered) ||
                        answered != 1) {
                        continue;
                    }
                    row.flagsAnswered |= static_cast<uint8_t>(1u << f);
                    if (flag != 0) row.flagsValue |= static_cast<uint8_t>(1u << f);
                }
                uint64_t sym = 0;
                answered = 0;
                if (CallScriptGetGuarded(get, graph, kSymModelComp, kSymbolType, &sym, &answered) &&
                    answered == 1) {
                    row.modelComponentAnswered = true;
                    row.modelComponent = sym;
                }
            }
        }

        bool counted = false;
        for (uint32_t k = 0; k < io.hashesWritten; ++k) {
            if (io.hashes[k].hash != row.scriptHash) continue;
            ++io.hashes[k].instances;
            if (row.answered) ++io.hashes[k].withSelf;
            counted = true;
            break;
        }
        if (!counted && io.hashes && io.hashesWritten < io.hashCap) {
            replay::IEntity::ScriptHashRow& h = io.hashes[io.hashesWritten++];
            h.hash = row.scriptHash;
            h.instances = 1;
            h.withSelf = row.answered ? 1u : 0u;
        }

        bool wanted = io.nWanted == 0;
        for (uint32_t k = 0; k < io.nWanted && !wanted; ++k) wanted = io.wanted[k] == row.scriptHash;
        if (wanted && io.rows && io.rowsWritten < io.rowCap) io.rows[io.rowsWritten++] = row;
    }
    return true;
}

namespace sgf = replay::scriptgraph_facts;

// The two bindings beside `get`, with the signatures the engine's own wrappers call them with:
// sub_1412ED110 -> *(module+248)(graph, name, type, &value, &delegateVariable, id) and
// sub_1412ECF00 -> *(module+512)(graph, &event).
using ScriptSetFn = int (*)(void* graph, uint64_t name, uint64_t type, void* value,
                            bool* delegateVariable, uint64_t id);
using ScriptSendEventFn = int64_t (*)(void* graph, void* event);

static bool CallScriptSetGuarded(ScriptSetFn fn, void* graph, uint64_t name, uint64_t type,
                                 void* value, bool* delegateVariable, uint64_t id, int* answered) {
    __try {
        *answered = fn(graph, name, type, value, delegateVariable, id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *answered = 0;
        return false;
    }
}

static bool CallScriptSendEventGuarded(ScriptSendEventFn fn, void* graph, void* event) {
    __try {
        fn(graph, event);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Both bindings are exports of the one script DLL the loader opened; a pointer outside that image
// is not a binding we have verified, whatever it points at.
static bool SameImage(const void* a, const void* b) {
    MEMORY_BASIC_INFORMATION ma = {}, mb = {};
    if (!VirtualQuery(a, &ma, sizeof ma) || !VirtualQuery(b, &mb, sizeof mb)) return false;
    return ma.AllocationBase != nullptr && ma.AllocationBase == mb.AllocationBase;
}

bool Entity::FindScriptGraph(void* space, uint64_t hash, uint64_t actorLo, uint64_t actorHi,
                             replay::IEntity::ScriptGraphRef& out, const char** why) {
    const char* scratch = nullptr;
    const char** reason = why ? why : &scratch;
    *reason = nullptr;
    out = replay::IEntity::ScriptGraphRef();
    ScriptIndexView v;
    if (!ResolveScriptIndex(space ? space : gameSpace_, v, reason)) return false;
    const bool wantActor = (actorLo | actorHi) != 0;
    bool sawHash = false;
    for (uint64_t r = 0; r < v.rows; ++r) {
        void* const element = At(v.index, static_cast<uint32_t>(kCensusStride * r));
        if (!replay::Readable(element, kCensusStride)) break;
        void* const inst = ScriptInstOfRow(v, Rd<uint16_t>(element, 2u * sizeof(uint64_t)));
        if (!inst) continue;
        void* const module = Deref(inst, lp::kScriptInstModuleOff);
        if (!module || !replay::Readable(module, sgf::kModuleSendEventOff + sizeof(void*))) continue;
        if (Rd<uint64_t>(module, lp::kScriptModuleHashOff) != hash) continue;
        sawHash = true;
        const uint64_t lo = Rd<uint64_t>(element, 0);
        const uint64_t hi = Rd<uint64_t>(element, sizeof(uint64_t));
        if (wantActor && (lo != actorLo || (actorHi != 0 && hi != actorHi))) continue;
        void* const graph = At(inst, lp::kScriptInstGraphOff);
        if (!Deref(graph, lp::kScriptGraphDataOff)) {
            *reason = "the graph's EXECUTION_DATA is null (never initialised)";
            continue;
        }
        out.inst = inst;
        out.graph = graph;
        out.module = module;
        out.hash = hash;
        out.actorLo = lo;
        out.actorHi = hi;
        ScriptGetFn get = reinterpret_cast<ScriptGetFn>(Rd<void*>(module, lp::kScriptModuleGetOff));
        int answered = 0;
        out.selfValid = get &&
                        CallScriptGetGuarded(get, graph, kSymSelf, kSymActorType, out.self,
                                             &answered) &&
                        answered == 1;
        *reason = nullptr;
        return true;
    }
    if (*reason == nullptr) {
        *reason = !sawHash   ? "no ncaScript graph with this module hash on the walked space"
                  : wantActor ? "the module is present, but not on this actor"
                              : "every graph with this module hash was unreadable";
    }
    return false;
}

bool Entity::ScriptGraphValid(const replay::IEntity::ScriptGraphRef& r) {
    if (!r.inst || !r.graph || !r.module) return false;
    if (!replay::Readable(r.inst, lp::kScriptInstGraphOff + lp::kScriptGraphDataOff + sizeof(void*)))
        return false;
    if (Deref(r.inst, lp::kScriptInstModuleOff) != r.module) return false;
    if (!replay::Readable(r.module, sgf::kModuleSendEventOff + sizeof(void*))) return false;
    return Rd<uint64_t>(r.module, lp::kScriptModuleHashOff) == r.hash &&
           Deref(r.graph, lp::kScriptGraphDataOff) != nullptr;
}

bool Entity::ScriptGetReal(const replay::IEntity::ScriptGraphRef& r, uint64_t name, float& out) {
    if (!ScriptGraphValid(r)) return false;
    ScriptGetFn get = reinterpret_cast<ScriptGetFn>(Rd<void*>(r.module, lp::kScriptModuleGetOff));
    if (!get) return false;
    float v = 0.0f;
    int answered = 0;
    if (!CallScriptGetGuarded(get, r.graph, name, sgf::kRealType, &v, &answered) || answered != 1)
        return false;
    out = v;
    return true;
}

bool Entity::ScriptSetReal(const replay::IEntity::ScriptGraphRef& r, uint64_t name, uint64_t id,
                           float v) {
    if (!replay::livesafety::Allowed(replay::livesafety::Feature::HostClockPin)) return false;
    if (!ScriptGraphValid(r)) return false;
    ScriptSetFn set = reinterpret_cast<ScriptSetFn>(Rd<void*>(r.module, sgf::kModuleSetOff));
    ScriptGetFn get = reinterpret_cast<ScriptGetFn>(Rd<void*>(r.module, lp::kScriptModuleGetOff));
    if (!set || !get || !SameImage(reinterpret_cast<const void*>(set),
                                   reinterpret_cast<const void*>(get))) {
        return false;
    }
    float value = v;
    bool delegateVariable = false;
    int answered = 0;
    return CallScriptSetGuarded(set, r.graph, name, sgf::kRealType, &value, &delegateVariable, id,
                                &answered) &&
           answered == 1;
}

bool Entity::ScriptDataWord(const replay::IEntity::ScriptGraphRef& r, uint32_t word,
                            int32_t& out) {
    if (!ScriptGraphValid(r)) return false;
    void* const data = Deref(r.graph, lp::kScriptGraphDataOff);
    if (!replay::Readable(data, 4u * word + sizeof(int32_t))) return false;
    out = Rd<int32_t>(data, 4u * word);
    return true;
}

bool Entity::ScriptSendEvent(const replay::IEntity::ScriptGraphRef& r, uint64_t name,
                             uint64_t component) {
    if (!replay::livesafety::Allowed(replay::livesafety::Feature::DiscShellFeed)) return false;
    if (!r.selfValid || !ScriptGraphValid(r)) return false;
    ScriptSendEventFn send =
        reinterpret_cast<ScriptSendEventFn>(Rd<void*>(r.module, sgf::kModuleSendEventOff));
    ScriptGetFn get = reinterpret_cast<ScriptGetFn>(Rd<void*>(r.module, lp::kScriptModuleGetOff));
    if (!send || !get || !SameImage(reinterpret_cast<const void*>(send),
                                    reinterpret_cast<const void*>(get))) {
        return false;
    }
    alignas(8) uint8_t event[sgf::kEventSize] = {};
    const uint64_t noUserData = ~0ull;
    memcpy(event, &name, sizeof name);
    memcpy(event + sgf::kEventActorOff, r.self, sizeof r.self);
    memcpy(event + sgf::kEventActorNameOff, r.self + lp::kActorVarNodeOff, sizeof(uint64_t));
    memcpy(event + sgf::kEventComponentOff, &component, sizeof component);
    memcpy(event + sgf::kEventUserDataOff, &noUserData, sizeof noUserData);
    return CallScriptSendEventGuarded(send, r.graph, event);
}

bool ReadAudioMixTables(void* sys, replay::IEntity::AudioMix& out);
void ReadWwiseListenerSets(replay::IEntity::AudioMix& out);
void ReadWwisePlayingSounds(replay::IEntity::AudioMix& out);

// ENG-P1D item C. Two links off a global, then two flat tables -- no component system, no game
// space, no bind. It answers in L3/L4 as readily as in a match, which is deliberate: "how many
// listeners does this session have" is a question about the sound player, not about the level.
bool Entity::AudioMixCensus(replay::IEntity::AudioMix& out) const {
    out = replay::IEntity::AudioMix();
    const auto image = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (image == nullptr || lp::kAudioRootVa < facts::PE_IMAGE_BASE) {
        out.refused = "the image did not resolve";
        return false;
    }
    const uint8_t* const rootAt = image + (lp::kAudioRootVa - facts::PE_IMAGE_BASE);
    if (!replay::Readable(rootAt, sizeof(void*))) {
        out.refused = "AUDIO.ROOT_VA is not committed memory in this image";
        return false;
    }
    void* const root = Rd<void*>(rootAt, 0);
    if (!root || !TryTouchSpan(root, lp::kAudioSystemOff + sizeof(void*))) {
        out.refused = "the engine root is null or does not reach AUDIO.SYSTEM_OFF";
        return false;
    }
    return ReadAudioMixTables(Deref(root, lp::kAudioSystemOff), out);
}

bool ReadAudioMixTables(void* sys, replay::IEntity::AudioMix& out) {
    out = replay::IEntity::AudioMix();
    if (!sys || !TryTouchSpan(sys, lp::kAudioViewCountOff + sizeof(uint64_t))) {
        out.refused = "the sound player is null or its tables are not readable";
        return false;
    }
    out.system = reinterpret_cast<uint64_t>(sys);
    out.spatializedId = Rd<uint32_t>(sys, lp::kAudioSpatializedOff);
    out.activeViewId = Rd<uint32_t>(sys, lp::kAudioActiveViewOff);
    out.listenerCount = Rd<uint64_t>(sys, lp::kAudioListenerCountOff);
    out.viewCount = Rd<uint64_t>(sys, lp::kAudioViewCountOff);

    // A count past the cap is reported, not clamped away: the whole finding this read exists for is
    // "there is more than one listener", so an over-long count is itself the answer and must show.
    void* const listeners = Deref(sys, lp::kAudioListenerBaseOff);
    if (listeners && out.listenerCount != 0 && out.listenerCount <= kAudioTableScanMax) {
        for (uint64_t i = 0; i < out.listenerCount &&
                             out.listenersWritten < replay::IEntity::AudioMix::kListenerCap; ++i) {
            void* const rec = At(listeners, static_cast<uint32_t>(lp::kAudioListenerStride * i));
            if (!replay::Readable(rec, lp::kAudioListenerStride)) break;
            replay::IEntity::AudioListener& l = out.listeners[out.listenersWritten++];
            l.id = Rd<uint32_t>(rec, 0);
            memcpy(l.quat, At(rec, lp::kAudioListenerQuatOff), sizeof l.quat);
            memcpy(l.pos, At(rec, lp::kAudioListenerPosOff), sizeof l.pos);
        }
    }
    void* const views = Deref(sys, lp::kAudioViewBaseOff);
    if (views && out.viewCount != 0 && out.viewCount <= kAudioTableScanMax) {
        for (uint64_t i = 0; i < out.viewCount &&
                             out.viewsWritten < replay::IEntity::AudioMix::kViewCap; ++i) {
            void* const rec = At(views, static_cast<uint32_t>(lp::kAudioViewStride * i));
            if (!replay::Readable(rec, lp::kAudioViewStride)) break;
            replay::IEntity::AudioView& v = out.views[out.viewsWritten++];
            v.id = Rd<uint32_t>(rec, 0);
            memcpy(v.pos, At(rec, lp::kAudioViewPosOff), sizeof v.pos);
        }
    }
    // The emitter registry. Unlike the two tables above this one is a map of {id, pointer}, so each
    // row costs a second probe -- a record that has been freed under a stale map entry is exactly
    // the shape of bug this read exists to catch, and it must refuse rather than fault. The audio
    // thread's spinlock at sys+88 is NOT taken: acquiring it would be a write into the engine, and
    // a census that tears mid-walk reports one wrong row, while a census that deadlocks the mixer
    // ends the session.
    void* const emitters = Deref(sys, lp::kAudioEmitterMapOff);
    out.emitterCount = Rd<uint64_t>(sys, lp::kAudioEmitterMapCountOff);
    if (emitters && out.emitterCount != 0 && out.emitterCount <= kAudioEmitterScanMax) {
        for (uint64_t i = 0; i < out.emitterCount &&
                             out.emittersWritten < replay::IEntity::AudioMix::kEmitterCap; ++i) {
            void* const slot = At(emitters, static_cast<uint32_t>(lp::kAudioEmitterMapStride * i));
            if (!replay::Readable(slot, lp::kAudioEmitterMapStride)) break;
            void* const rec = Deref(slot, lp::kAudioEmitterMapRecOff);
            if (!rec || !replay::Readable(rec, lp::kAudioEmitterRecSpan)) continue;
            replay::IEntity::AudioEmitter& e = out.emitters[out.emittersWritten++];
            e.id = Rd<uint32_t>(rec, lp::kAudioEmitterIdOff);
            e.flags = Rd<uint32_t>(rec, lp::kAudioEmitterFlagsOff);
            memcpy(e.pos, At(rec, lp::kAudioEmitterPosOff), sizeof e.pos);
            // Its own probe: the selector sits past kAudioEmitterRecSpan, which is the span the
            // rest of this row was proved against, and a read past a proved span is not a read.
            if (replay::Readable(At(rec, lp::kAudioEmitterListenerSelOff), sizeof(uint32_t)) &&
                Rd<uint32_t>(rec, lp::kAudioEmitterListenerSelOff) ==
                    lp::kAudioEmitterListener2D) {
                ++out.listener2dEmitters;
            }
        }
    }
    // The pool gauges and the Wwise limits. All four pool fields live on the same object this walk
    // already proved readable, and the three Wwise numbers are image globals, rebased and probed.
    if (replay::Readable(At(sys, lp::kSoundActiveCountOff), sizeof(uint64_t))) {
        out.soundPoolCap = Rd<uint64_t>(sys, lp::kSoundPoolCapOff);
        out.soundPoolFree = Rd<uint64_t>(sys, lp::kSoundPoolFreeOff);
        out.soundsActive = Rd<uint64_t>(sys, lp::kSoundActiveCountOff);
        out.emitterPoolFree = Rd<uint64_t>(sys, lp::kEmitterPoolFreeOff);
        out.poolRead = true;
    }
    if (const uint32_t* const maxVoices = RebasedDword(lp::kWwiseMaxVoicesVa)) {
        // A u16 in the image; the dword read takes two bytes of the next global with it, so it is
        // masked here rather than reported as a number nobody wrote.
        out.maxVoices = static_cast<uint16_t>(*maxVoices);
        out.limitsRead = true;
        if (const uint32_t* const hi = RebasedDword(lp::kWwiseVoiceHighVa)) out.voiceHigh = *hi;
        if (const uint32_t* const lo = RebasedDword(lp::kWwiseVoiceLowVa)) out.voiceLow = *lo;
        if (const uint32_t* const th = RebasedDword(lp::kWwiseVolumeThreshVa)) {
            memcpy(&out.volumeThresholdDb, th, sizeof out.volumeThresholdDb);
        }
    }
    ReadWwiseListenerSets(out);
    ReadWwisePlayingSounds(out);
    out.refused = nullptr;
    return true;
}

namespace {

using AkGetListenersFn = uint32_t(__fastcall*)(uint64_t gameObj, uint64_t* out, uint32_t* count);
using AkGetListenerPosFn = uint32_t(__fastcall*)(uint64_t listener, float* transform36);
using AkGetMaxRadiusFn = float(__fastcall*)(uint64_t gameObj);
using AkGetPlayingIdsFn = uint32_t(__fastcall*)(uint64_t gameObj, uint32_t* count, uint32_t* ids);
using AkGetEventIdFn = uint32_t(__fastcall*)(uint32_t playingId);
using AkGetPositioningInfoFn = uint32_t(__fastcall*)(uint32_t playingId, uint8_t* info68);
// AkTransform out, 36 bytes. AkAuxSendValue is {AkGameObjectID listener; AkAuxBusID bus; float
// value} -- 16 bytes, the stride sub_14020E220 fills when it hands the setter its list.
using AkQueryObjectIdsFn = uint32_t(__fastcall*)(uint32_t eventId, uint32_t* count, uint8_t* infos);
using AkGetObjPositionFn = uint32_t(__fastcall*)(uint64_t gameObj, float* transform9);
using AkGetAuxSendsFn = uint32_t(__fastcall*)(uint64_t gameObj, uint8_t* sends, uint32_t* count);
using AkGetDryLevelFn = uint32_t(__fastcall*)(uint64_t gameObj, uint64_t listener, float* out);

// One resolve for both: a VA rebased onto this image, with the recorded prologue compared byte for
// byte. A signature this long cannot be shared by a neighbouring function, which is the failure a
// two-byte check would wave through.
const uint8_t* ResolveByVa(uint64_t va, const uint8_t* sig, size_t sigLen) {
    const auto image = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (image == nullptr || va < facts::PE_IMAGE_BASE) return nullptr;
    const uint8_t* const at = image + (va - facts::PE_IMAGE_BASE);
    if (!replay::Readable(at, sigLen)) return nullptr;
    return memcmp(at, sig, sigLen) == 0 ? at : nullptr;
}

}  // namespace

// The one place this plugin calls into the sound engine. Both entry points are Wwise's documented
// read-only queries: they take the engine's own critical section, return by out-parameter and
// change nothing. It is console-driven and one-shot, so it adds no per-frame work and cannot be
// running while a live session mixes.
void ReadWwiseListenerSets(replay::IEntity::AudioMix& out) {
    const auto getListeners = reinterpret_cast<AkGetListenersFn>(const_cast<uint8_t*>(
        ResolveByVa(lp::kAkGetListenersVa, lp::kAkGetListenersSig, sizeof lp::kAkGetListenersSig)));
    const auto getPos = reinterpret_cast<AkGetListenerPosFn>(const_cast<uint8_t*>(ResolveByVa(
        lp::kAkGetListenerPositionVa, lp::kAkGetListenerPositionSig,
        sizeof lp::kAkGetListenerPositionSig)));
    if (getListeners == nullptr || getPos == nullptr) {
        out.wwiseRefused = "the Wwise query entry points are not where this image's facts say";
        return;
    }
    for (uint32_t i = 0; i < out.emittersWritten; ++i) {
        uint64_t ids[replay::IEntity::AudioListenerSet::kIdCap] = {};
        uint32_t count = replay::IEntity::AudioListenerSet::kIdCap;
        if (getListeners(out.emitters[i].id, ids, &count) != 1u) continue;
        const uint32_t stored = count < replay::IEntity::AudioListenerSet::kIdCap
                                    ? count : replay::IEntity::AudioListenerSet::kIdCap;
        replay::IEntity::AudioAccumulateListenerSet(out, ids, stored, count);
    }
    // Every listener id any set names, positioned by Wwise itself -- the number the game-side
    // listener table cannot answer, because a listener Wwise still attenuates against need not be
    // in that table at all.
    for (uint32_t s = 0; s < out.setsWritten; ++s) {
        for (uint32_t i = 0; i < replay::IEntity::AudioListenerSet::kIdCap &&
                             i < out.sets[s].size; ++i) {
            const uint64_t id = out.sets[s].ids[i];
            bool seen = false;
            for (uint32_t k = 0; k < out.wwiseListenersWritten && !seen; ++k) {
                seen = out.wwiseListeners[k].id == id;
            }
            if (seen || out.wwiseListenersWritten >= replay::IEntity::AudioMix::kSetCap) continue;
            replay::IEntity::AudioWwiseListener& l = out.wwiseListeners[out.wwiseListenersWritten++];
            l.id = id;
            float xform[9] = {};
            if (getPos(id, xform) == 1u) {
                l.read = true;
                // AkTransform is {front[3]; top[3]; position[3]}, and sub_14020E560 handed Wwise
                // `-x` on the way in, so the x comes back flipped and is flipped again here --
                // otherwise this position could not be compared with the two tables above.
                l.pos[0] = -xform[6];
                l.pos[1] = xform[7];
                l.pos[2] = xform[8];
            }
        }
    }
    out.wwiseRefused = nullptr;
}

// FINAL-FIX-1 item 1(c). What is actually PLAYING on each registered emitter, and the last point of
// the attenuation curve each playing voice carries. Read-only, console-driven, one shot, same
// family and same critical section as the two queries above.
// FAR-SOUND-2. The emitter as WWISE holds it, for one emitter. Called only for emitters that are
// carrying a voice, because that is the only set whose attenuation anyone can hear, and 500 dead
// game objects would cost 1,500 locked queries for nothing.
void ReadWwiseEmitterState(replay::IEntity::AudioEmitter& e) {
    const auto getPos = reinterpret_cast<AkGetObjPositionFn>(const_cast<uint8_t*>(ResolveByVa(
        lp::kAkGetObjPositionVa, lp::kAkGetObjPositionSig, sizeof lp::kAkGetObjPositionSig)));
    const auto getAux = reinterpret_cast<AkGetAuxSendsFn>(const_cast<uint8_t*>(ResolveByVa(
        lp::kAkGetAuxSendsVa, lp::kAkGetAuxSendsSig, sizeof lp::kAkGetAuxSendsSig)));
    const auto getDry = reinterpret_cast<AkGetDryLevelFn>(const_cast<uint8_t*>(
        ResolveByVa(lp::kAkGetDryLevelVa, lp::kAkGetDryLevelSig, sizeof lp::kAkGetDryLevelSig)));
    if (getPos != nullptr) e.wwiseRead = getPos(e.id, e.wwise) == 1u;
    if (getDry != nullptr) e.dryRead = getDry(e.id, 0u, &e.dryLevel) == 1u;
    if (getAux == nullptr) return;
    // The count is in/out: it goes in as the room this call has and comes back as the room Wwise
    // used, so a send list longer than four is reported by `auxCount` rather than written past it.
    constexpr uint32_t kSendCap = 4;
    uint8_t sends[kSendCap * 16] = {};
    uint32_t count = kSendCap;
    if (getAux(e.id, sends, &count) != 1u) return;
    e.auxRead = true;
    e.auxCount = count;
    const uint32_t stored = count < kSendCap ? count : kSendCap;
    for (uint32_t i = 0; i < stored; ++i) {
        memcpy(&e.auxBus[i], sends + i * 16u + 8u, sizeof e.auxBus[i]);
        memcpy(&e.auxValue[i], sends + i * 16u + 12u, sizeof e.auxValue[i]);
    }
}

void ReadWwisePlayingSounds(replay::IEntity::AudioMix& out) {
    const auto getMaxRadius = reinterpret_cast<AkGetMaxRadiusFn>(const_cast<uint8_t*>(
        ResolveByVa(lp::kAkGetMaxRadiusVa, lp::kAkGetMaxRadiusSig, sizeof lp::kAkGetMaxRadiusSig)));
    const auto getPlaying = reinterpret_cast<AkGetPlayingIdsFn>(const_cast<uint8_t*>(ResolveByVa(
        lp::kAkGetPlayingIdsVa, lp::kAkGetPlayingIdsSig, sizeof lp::kAkGetPlayingIdsSig)));
    const auto getEventId = reinterpret_cast<AkGetEventIdFn>(const_cast<uint8_t*>(ResolveByVa(
        lp::kAkGetEventIdVa, lp::kAkGetEventIdSig, sizeof lp::kAkGetEventIdSig)));
    const auto getPosInfo = reinterpret_cast<AkGetPositioningInfoFn>(const_cast<uint8_t*>(
        ResolveByVa(lp::kAkGetPositioningInfoVa, lp::kAkGetPositioningInfoSig,
                    sizeof lp::kAkGetPositioningInfoSig)));
    const auto queryObjs = reinterpret_cast<AkQueryObjectIdsFn>(const_cast<uint8_t*>(ResolveByVa(
        lp::kAkQueryObjectIdsVa, lp::kAkQueryObjectIdsSig, sizeof lp::kAkQueryObjectIdsSig)));
    if (getMaxRadius == nullptr || getPlaying == nullptr || getEventId == nullptr ||
        getPosInfo == nullptr) {
        out.playingRefused = "the Wwise playing-sound entry points are not where this image's "
                             "facts say";
        return;
    }
    for (uint32_t i = 0; i < out.emittersWritten; ++i) {
        // The array is sized for the query's own answer; a voice count past it is reported by
        // `playingSeen` rather than written past the end.
        constexpr uint32_t kPerObject = 16;
        uint32_t ids[kPerObject] = {};
        uint32_t count = kPerObject;
        if (getPlaying(out.emitters[i].id, &count, ids) != 1u || count == 0u) continue;
        ++out.emittersActive;
        ReadWwiseEmitterState(out.emitters[i]);
        const float radius = getMaxRadius(out.emitters[i].id);
        const uint32_t stored = count < kPerObject ? count : kPerObject;
        for (uint32_t p = 0; p < stored; ++p) {
            ++out.playingSeen;
            if (out.playingWritten >= replay::IEntity::AudioMix::kPlayingCap) continue;
            replay::IEntity::AudioPlayingSound& s = out.playing[out.playingWritten++];
            s.emitterId = out.emitters[i].id;
            s.playingId = ids[p];
            s.eventId   = getEventId(ids[p]);
            s.maxRadius = radius;
            s.emitterPos[0] = out.emitters[i].pos[0];
            s.emitterPos[1] = out.emitters[i].pos[1];
            s.emitterPos[2] = out.emitters[i].pos[2];
            // event id -> the bank objects it references -> the first of those with a positioning
            // block. The first object is the one taken because a foley event references one sound;
            // `objCount` prints the rest of the claim so a multi-object event is not silently
            // reported by its first child alone.
            constexpr uint32_t kObjCap = 8;
            uint8_t infos[kObjCap * lp::kAkObjectInfoBytes] = {};
            uint32_t objs = kObjCap;
            if (queryObjs != nullptr && queryObjs(s.eventId, &objs, infos) == 1u && objs != 0u) {
                s.objCount = objs;
                const uint32_t objsStored = objs < kObjCap ? objs : kObjCap;
                // ★ AUDIO-FIX-2 C.2 / RE-AUDIO-CUTOFF R2. EVERY node of the sub-tree, with its
                // parent and depth, before the one-node pick below. Which child a given playingID
                // resolved to is not answerable from any query this image exports, so the honest
                // instrument records the whole tree and lets the curves be compared, rather than
                // naming one child and calling it the curve that plays.
                s.nodeFirst = out.objNodesWritten;
                for (uint32_t o = 0; o < objsStored; ++o) {
                    ++out.objNodesSeen;
                    if (out.objNodesWritten >= replay::IEntity::AudioMix::kObjNodeCap) continue;
                    const uint8_t* const rec = infos + o * lp::kAkObjectInfoBytes;
                    replay::IEntity::AudioObjNode& n = out.objNodes[out.objNodesWritten];
                    memcpy(&n.objId, rec, sizeof n.objId);
                    memcpy(&n.parentId, rec + 4, sizeof n.parentId);
                    memcpy(&n.depth, rec + 8, sizeof n.depth);
                    uint8_t info[replay::IEntity::kPositioningInfoBytes] = {};
                    if (n.objId != 0u && getPosInfo(n.objId, info) == 1u) {
                        replay::IEntity::AudioPlayingSound probe;
                        replay::IEntity::AudioDecodePositioningInfo(info, probe);
                        n.info = true;
                        n.attenuation = probe.attenuation;
                        n.maxDistance = probe.maxDistance;
                        n.volDryAtMax = probe.volDryAtMax;
                        n.volAuxGameDefAtMax = probe.volAuxGameDefAtMax;
                        n.volAuxUserDefAtMax = probe.volAuxUserDefAtMax;
                    }
                    ++out.objNodesWritten;
                    ++s.nodeCount;
                }
                // An event answers with its whole sub-tree, and the FIRST node that has a
                // positioning block is the container, which carries no attenuation -- the curve
                // lives on the sound that actually plays. So the scan does not stop at the first
                // answer; it stops at the first ATTENUATED answer and keeps the first plain one
                // only as the fallback, which is how "NOT ATTENUATED" stays a real finding rather
                // than an artefact of reading the wrong node.
                for (uint32_t o = 0; o < objsStored; ++o) {
                    uint32_t objId = 0;
                    memcpy(&objId, infos + o * lp::kAkObjectInfoBytes, sizeof objId);
                    uint8_t info[replay::IEntity::kPositioningInfoBytes] = {};
                    if (objId == 0u || getPosInfo(objId, info) != 1u) continue;
                    replay::IEntity::AudioPlayingSound probe;
                    replay::IEntity::AudioDecodePositioningInfo(info, probe);
                    if (s.info && !probe.attenuation) continue;
                    s.objId = objId;
                    s.info = true;
                    replay::IEntity::AudioDecodePositioningInfo(info, s);
                    memcpy(s.raw, info, sizeof s.raw);
                    if (probe.attenuation) break;
                }
            }
        }
    }
    out.playingRefused = nullptr;
}

// P0 reading 6. Read exactly where CR15FrisbeeCS::Update reads: `timesincelastheld` off the one
// live frisbee component, then the touch-interact CI its own `+4` handle names, whose `+392`/`+396`
// are the only thing that resets that timer.
bool Entity::ReadDiscRing(replay::IEntity::DiscRing& out) const {
    out = replay::IEntity::DiscRing();
    // `discFrisbeeCs_` outlives a drop -- DropDisc releases the drive, not the last chain it walked
    // -- so the bound net-physics component is what says the pointer is still worth reading.
    void* const frisbeeCs = disc_.cs ? discFrisbeeCs_ : nullptr;
    if (!frisbeeCs) { out.refused = "no R14Frisbee system is bound"; return false; }
    if (!TryTouchSpan(frisbeeCs, lp::kFrisbeeCsPoolBaseOff + sizeof(void*))) {
        out.refused = "the R14Frisbee system is not readable";
        return false;
    }
    void* const base = Deref(frisbeeCs, lp::kFrisbeeCsPoolBaseOff);
    const uint16_t live = Rd<uint16_t>(frisbeeCs, lp::kPhysCsLiveCountOff);
    // The ENABLED prefix is what CR15FrisbeeCS::Update's dense walk is bounded by, so a disc the
    // host has parked accumulates nothing -- and a frozen timer would read as "never reset".
    out.enabled = Rd<uint16_t>(frisbeeCs, lp::kFrisbeeCsCountOff);
    if (!base || live != 1) {
        out.refused = "the frisbee system does not hold exactly one live component";
        return false;
    }
    const void* ci = base;
    if (!replay::Readable(ci, lp::kFrisbeeCiStride)) {
        out.refused = "the frisbee component instance is not readable";
        return false;
    }
    out.timeSinceLastHeld = Rd<float>(ci, lp::kFrisbeeCiTimeSinceHeldOff);
    out.touchHandle = static_cast<uint16_t>(Rd<uint32_t>(ci, lp::kFrisbeeCiTouchIdOff));
    out.refused = nullptr;

    if (!TryTouchSpan(frisbeeCs, lp::kFrisbeeCsTouchCsOff + sizeof(void*))) return true;
    void* const touchCs = Deref(frisbeeCs, lp::kFrisbeeCsTouchCsOff);
    if (!touchCs || !TryTouchSpan(touchCs, lp::kTouchCsPoolBaseOff + sizeof(void*))) return true;
    void* const map = Deref(touchCs, lp::kTouchCsMapOff);
    void* const pool = Deref(touchCs, lp::kTouchCsPoolBaseOff);
    if (!map || !pool) return true;
    const uint32_t mapByte = 4u * static_cast<uint32_t>(out.touchHandle);
    if (!replay::Readable(At(map, mapByte), sizeof(uint16_t))) return true;
    const void* tci = At(pool, lp::kTouchCiStride * uint32_t(Rd<uint16_t>(map, mapByte)));
    if (!replay::Readable(tci, lp::kTouchCiStride)) return true;
    out.touchRead   = true;
    out.holderA     = Rd<uint32_t>(tci, lp::kTouchCiHolderAOff);
    out.holderB     = Rd<uint32_t>(tci, lp::kTouchCiHolderBOff);
    out.flags       = Rd<uint16_t>(tci, lp::kTouchCiFlagsOff);
    out.askParent   = (out.flags & lp::kTouchCiAskParentBit) != 0;
    out.heldByOwnCi = out.holderA != 0 || out.holderB != 0;
    return true;
}

bool Entity::ProbeEchoUnitBind(UserIndex u, replay::IEntity::EchoBind& out) const {
    out = replay::IEntity::EchoBind();
    using Src = replay::IEntity::ActorSource;
    out.system = echo_.cs != nullptr;
    out.echoPopulation = ComponentPopulation(echo_.cs);
    if (!bcastCs_) return false;
    void* netGame = Deref(bcastCs_, lp::kBroadcasterNetGameOff);
    out.netGame = netGame != nullptr;

    uint8_t keys[lp::kActorGuidSize] = {};
    out.haveKeys = BroadcastKeyActor(u, keys);
    if (out.haveKeys) {
        memcpy(&out.keysLo, keys, sizeof out.keysLo);
        memcpy(&out.keysHi, keys + 8, sizeof out.keysHi);
        out.keysEchoHandle = ComponentOnActor(echo_.cs, keys);
    }

    uint8_t fromUser[lp::kActorGuidSize] = {};
    bool haveUser = false;
    if (userActor_ && netGame && u.v < facts::ENTITY_USER_SLOT_TABLE_MAX) {
        const void* rec = At(netGame, lp::kNetGameUserActorOff + lp::kNetGameUserStride * u.v);
        if (replay::Readable(rec, lp::kActorGuidSize)) {
            userActor_(netGame, fromUser, u.v);
            haveUser = !AllZero(fromUser, sizeof fromUser);
        }
    }
    if (haveUser) {
        memcpy(&out.actorLo, fromUser, sizeof out.actorLo);
        memcpy(&out.actorHi, fromUser + 8, sizeof out.actorHi);
    }
    out.actorsDisagree =
        out.haveKeys && haveUser && memcmp(keys, fromUser, lp::kActorGuidSize) != 0;

    // The source that ANSWERS wins, and the key array is asked first because it names the actor the
    // broadcast element is. Neither is trusted over the other by assertion: a handle came back or
    // it did not.
    if (out.haveKeys && out.keysEchoHandle != lp::kNoComponent) {
        out.actorSource = Src::BroadcasterKeys;
        out.echoHandle = out.keysEchoHandle;
        out.modelHandle = ComponentOnActor(model_.cs, keys);
        out.bodyHandle = ComponentOnActor(body_.cs, keys);
        return true;
    }
    if (haveUser) {
        out.actorSource = Src::NetGameUserActor;
        out.echoHandle = ComponentOnActor(echo_.cs, fromUser);
        out.modelHandle = ComponentOnActor(model_.cs, fromUser);
        out.bodyHandle = ComponentOnActor(body_.cs, fromUser);
        return out.echoHandle != lp::kNoComponent;
    }
    if (out.haveKeys) {
        out.actorSource = Src::BroadcasterKeys;
        out.modelHandle = ComponentOnActor(model_.cs, keys);
        out.bodyHandle = ComponentOnActor(body_.cs, keys);
    }
    return false;
}

uint64_t Entity::NetIdOf(PhysHandle h) const {
    size_t n = 0;
    const void* table = NetIdTable(n);
    if (!table) return 0;
    for (size_t i = 0; i < n; ++i) {
        const void* e = At(const_cast<void*>(table),
                           static_cast<uint32_t>(lp::kNetActorEntryStride * i));
        // The entry pairs the net id with an SComponentID -- handle in the low word, generation
        // in the high word. SimStateCB matches it against the PHYSICS system, which is what makes
        // the low word comparable to a PhysHandle here.
        if (static_cast<uint16_t>(Rd<uint32_t>(e, lp::kNetActorCompIdOff)) == h.v)
            return Rd<uint64_t>(e, 0);
    }
    return 0;
}

PhysHandle Entity::PhysOfNetId(uint64_t netId) const {
    size_t n = 0;
    const void* table = NetIdTable(n);
    if (!table || netId == 0) return PhysHandle(0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    for (size_t i = 0; i < n; ++i) {
        const void* e = At(const_cast<void*>(table),
                           static_cast<uint32_t>(lp::kNetActorEntryStride * i));
        if (Rd<uint64_t>(e, 0) == netId)
            return PhysHandle(static_cast<uint16_t>(Rd<uint32_t>(e, lp::kNetActorCompIdOff)));
    }
    return PhysHandle(0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
}

PhysHandle Entity::PhysOfComponentId(uint32_t componentId, ComponentIdReading* seen) const {
    return ComponentIdIn(phys_, PhysHandle::kPoolPlayers, componentId, seen);
}

// CComponentSystem::IsComponentValid sub_1403319D0, plus the CI flags the caller wants to see.
// The dense index is bounded by cs+0xF2, the LIVE count, exactly as the engine bounds it: cs+0xF4
// is the ENABLED count and is a PREFIX of the same array, so using it refuses a live component the
// engine would accept -- a disc parked between rounds is the case that costs a bind.
PhysHandle Entity::ComponentIdIn(const Pool& pl, uint8_t poolTag, uint32_t componentId,
                                 ComponentIdReading* seen) const {
    ComponentIdReading local;
    ComponentIdReading& r = seen ? *seen : local;
    r = ComponentIdReading();
    const PhysHandle absent(0xFFFFu, poolTag);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids

    // Pool::HeaderExtent measures from the pool-base slot, so the two counts have to live below it.
    static_assert(lp::kPhysCsIdCapacityOff + 2u <= facts::PHYS_CS_POOL_BASE_OFF &&
                      lp::kPhysCsLiveCountOff + 2u <= facts::PHYS_CS_POOL_BASE_OFF &&
                      lp::kPhysCsIdCapacityOff + 2u <= lp::kFrisbeeCsPoolBaseOff &&
                      lp::kPhysCsLiveCountOff + 2u <= lp::kFrisbeeCsPoolBaseOff,
                  "the CS header proof must cover the two counts read below it");

    // DiscStands calls this EVERY TICK while the disc is driven, so nothing here may reach
    // replay::Readable directly: L2-RUN-10 cost-killed a live capture for exactly that, and
    // clock.cpp's contract for Readable says never on the tick path. Both spans go through the
    // cache, and both loads through the guard that sees a page unmapped after the proof.
    const uint8_t* cs = pl.ProvedCs();
    if (!cs) return absent;

    const uint16_t index = static_cast<uint16_t>(componentId);
    const uint16_t generation = static_cast<uint16_t>(componentId >> 16);
    r.idCapacity = Rd<uint16_t>(cs, lp::kPhysCsIdCapacityOff);
#if defined(DISCFIX_MUTATE_DENSE_BOUND_IS_ENABLED_COUNT)
    r.liveCount = Rd<uint16_t>(cs, pl.countOff);   // Mutation control: the +0xF4 bound the audit rejected.
#else
    r.liveCount = Rd<uint16_t>(cs, lp::kPhysCsLiveCountOff);
#endif
    if (index >= r.idCapacity) return absent;

    void* map = Deref(cs, pl.mapOff);
    const uint32_t entry = 4u * index;
    const uint8_t* proved = map ? pl.mapSpan_.Prove(map, entry + 4u) : nullptr;
    if (!proved || !TryTouchSpan(proved + entry, 4u)) return absent;
    r.mapRead    = true;
    r.poolIndex  = Rd<uint16_t>(proved, entry);
    r.generation = Rd<uint16_t>(proved, entry + 2u);
    if (r.generation != generation) return absent;
    if (IndexPastCount(r.poolIndex, r.liveCount)) return absent;

    // The CI flags are the bind probe's business alone. DiscStands passes null and pays nothing
    // for a reading it would discard.
    if (seen) {
        if (const uint8_t* ci = pl.ProvedElementAt(r.poolIndex)) {
            r.ciFlags = Rd<uint32_t>(ci, facts::PHYS_CI_FLAGS_OFF);
            r.ciFlagsRead = true;
        }
    }
    return PhysHandle(index, poolTag);
}

// ------------------------------------------------------------------------------------------
// THE EARLY PATH. The engine fills the rules block's frisbee ref only from sub_140172AD0, which
// runs on nevt_round_start / pre_launch_enter; an offline host sits in pre_match for ~98 s, so for
// the first ~100 s of every viewing the ref is null and the disc cannot be driven at all
// (DIAG-CLOCK-BIND-2 QC). The actor is in the level space's registry from map load regardless of
// round state, so we walk to it the way sub_140172AD0 does -- minus the rules block.
//
// RE-IMPLEMENTED, NOT CALLED. Both engine lookups on this chain write: sub_1404FF1A0's caller
// takes the registry through a space-parent walk, and BOTH symbol searches (sub_140105510 at
// space+0x420, sub_1401055C0 at space+0x460) sort the table IN PLACE and clear its dirty bit when
// flags bit 0 is set. A read-only plugin must not do that on the game thread, and what remains
// once that is excluded is two binary searches over sorted u64 keys and one array index -- simple
// enough that reproducing it is cheaper than proving a call safe. Nothing below calls the engine.
// ------------------------------------------------------------------------------------------

// The engine's liveness test on an actor slot, from sub_1404FF1A0's return expression
//   *(_WORD *)(*(_QWORD *)(*(_QWORD *)(a1 + 880) + 352) + 2 * idx) == 0xFFFF
// -- two dereferences, not one: registry+0x160 holds a pointer to the slot-word array. A slot
// nobody occupies holds a free-list link, so this is an equality and not a "not cleared" test.
bool Entity::ActorSlotLive(void* space, uint16_t actorIndex) const {
    void* const reg = Deref(space, lp::kGameSpaceActorRegOff);
    if (!reg || !replay::Readable(reg, lp::kActorRegSlotWordsPtrOff + sizeof(void*))) return false;
    void* const words = Deref(reg, lp::kActorRegSlotWordsPtrOff);
    const uint32_t end = 2u * (uint32_t(actorIndex) + 1u);
    if (!words || !replay::Readable(words, end)) return false;
    return Rd<uint16_t>(words, 2u * uint32_t(actorIndex)) == lp::kActorSlotLive;
}

// The candidate spaces, in the order the early path tries them, and the expression each is read
// by. Candidate 1 is candidate 0 read the long way round -- sub_140172AD0 resolves its space as
// `*(*(rules) + 0x2B10)` and the rules constructor stores the netGame at rules+0 -- and it is kept
// only because two standing REs disagreed about that, so one launch that prints both pointers ends
// the argument instead of a second build.
void* Entity::EarlySpaceRoot(void* netGame, size_t which, const char** name) const {
    switch (which) {
        case 0:
            if (name) *name = "ng+0x2B10";
            return replay::Readable(netGame, lp::kNetGameLevelSpaceOff + sizeof(void*))
                       ? Deref(netGame, lp::kNetGameLevelSpaceOff)
                       : nullptr;
        case 1: {
            if (name) *name = "*(*(ng+0x2AA8))+0x2B10";
            if (!replay::Readable(netGame, lp::kNetGameRulesOff + sizeof(void*))) return nullptr;
            void* const rules = Deref(netGame, lp::kNetGameRulesOff);
            if (!rules || !replay::Readable(rules, lp::kRulesOwnerNetGameOff + sizeof(void*)))
                return nullptr;
            void* const owner = Deref(rules, lp::kRulesOwnerNetGameOff);
            if (!owner || !replay::Readable(owner, lp::kNetGameLevelSpaceOff + sizeof(void*)))
                return nullptr;
            return Deref(owner, lp::kNetGameLevelSpaceOff);
        }
        case 2:
            if (name) *name = "ng+0x2B08";
            return replay::Readable(netGame, lp::kNetGameCsSpaceOff + sizeof(void*))
                       ? Deref(netGame, lp::kNetGameCsSpaceOff)
                       : nullptr;
        default:
            if (name) *name = "?";
            return nullptr;
    }
}

// sub_1404E4F50: ask each space's own registry, and on a miss climb to the parent at space+0x18.
// Returns the space the live game_frisbee actor was found in, which is the space its components
// are then looked up in -- exactly as sub_140172AD0 passes the found space to sub_1401692F0.
void* Entity::FrisbeeActorSpace(void* levelSpace, uint16_t& actorIndex, DiscBind::EarlyDiag* diag,
                                size_t at) const {
    actorIndex = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    DiscBind::EarlyDiag::Cand* const c =
        (diag && at < DiscBind::EarlyDiag::kCandidates) ? &diag->cand[at] : nullptr;
    using Reg = DiscBind::EarlyDiag::Reg;
    static_assert(lp::kSpaceParentWalkMax <= DiscBind::EarlyDiag::kDepths,
                  "the per-depth diagnostic has to cover every space the walk can reach");
    void* space = levelSpace;
    for (uint32_t depth = 0; space && depth < lp::kSpaceParentWalkMax; ++depth) {
        if (!replay::Readable(space, lp::kGameSpaceActorRegOff + sizeof(void*))) {
            if (c) { c->depth = depth + 1u; c->regAt[depth] = Reg::NoReg; }
            return nullptr;
        }
        if (c) c->depth = depth + 1u;
        void* reg = Deref(space, lp::kGameSpaceActorRegOff);
        if (c) c->regAt[depth] = Reg::NoReg;
        if (reg && replay::Readable(reg, lp::kActorRegCountOff + sizeof(uint64_t))) {
            const uint64_t count = Rd<uint64_t>(reg, lp::kActorRegCountOff);
            void* base = Deref(reg, lp::kActorRegBaseOff);
            const bool usable = base != nullptr && count != 0 && count <= lp::kActorRegMaxEntries &&
                                replay::Readable(base, size_t(lp::kActorRegEntryStride) *
                                                           size_t(count));
            if (c) {
                c->countAt[depth] = count;
                // sub_1404FF1A0 reads the count and returns before it touches the base, so a
                // count of 0 is EMPTY however the base reads -- the engine never looks at it.
                c->regAt[depth] = count == 0 ? Reg::Empty
                                             : (usable ? Reg::Populated : Reg::Unreadable);
            }
            if (c && depth == 0) {
                c->count = count;
                c->registryRead = usable;
            }
            if (usable && diag && diag->sampleN == 0) {
                const uint64_t take = count < DiscBind::EarlyDiag::kSymbolSamples
                                          ? count
                                          : uint64_t(DiscBind::EarlyDiag::kSymbolSamples);
                for (uint64_t i = 0; i < take; ++i)
                    diag->sample[i] =
                        Rd<uint64_t>(base, uint32_t(lp::kActorRegEntryStride * i));
                diag->sampleN = size_t(take);
                diag->sampleFrom = space;
            }
            if (usable) {
                // sub_1404FF1A0's own search: a lower bound over 16-byte entries keyed by the
                // CSymbol64 at +0, then an equality test on where it landed.
                uint64_t lo = 0, hi = count;
                while (lo < hi) {
                    const uint64_t mid = lo + (hi - lo) / 2u;
                    if (Rd<uint64_t>(base, uint32_t(lp::kActorRegEntryStride * mid)) <
                        lp::kSymGameFrisbee)
                        lo = mid + 1u;
                    else
                        hi = mid;
                }
                if (lo < count &&
                    Rd<uint64_t>(base, uint32_t(lp::kActorRegEntryStride * lo)) ==
                        lp::kSymGameFrisbee) {
                    const uint16_t idx = Rd<uint16_t>(
                        base, uint32_t(lp::kActorRegEntryStride * lo + lp::kActorRegEntryIndexOff));
                    if (c) {
                        c->symbolMatched = true;
                        c->index = idx;
                    }
                    if (ActorSlotLive(space, idx)) {
                        if (c) c->slotLive = true;
                        actorIndex = idx;
                        return space;
                    }
                }
            }
        }
        space = Deref(space, lp::kGameSpaceParentOff);
    }
    return nullptr;
}

// sub_140105510's search, without its sort. The table guard is what makes the unguarded indexing
// below safe, and it refuses outright while the engine's own dirty bit is set -- a table we would
// have to sort to search is a table we do not search.
void* Entity::SystemInSpace(void* space, uint64_t symbol) const {
    const replay::tableguard::TableCheck t = replay::tableguard::CheckSpaceTable(
        space, lp::kGameSpaceSystemTableOff, lp::kSystemTableStride, false);
    // This search reads the rows itself and never past the table's own count, so
    // the objections that are about what the ENGINE would do to the block are not refusals here --
    // see BoundForSearch. Taking them as refusals made this answer null for every sample of the
    // DISC's game space, which is the only space the disc bind can validate in, and removed that arm
    // of the bind's validation without ever saying so. The extent is that space's own row count: 68
    // where the disc lives, 99 where the pawns do.
    const replay::tableguard::SearchBound bound =
        replay::tableguard::BoundForSearch(t, lp::kSystemTableStride);
    if (!bound.ok) return nullptr;
    void* const base = reinterpret_cast<void*>(static_cast<uintptr_t>(t.raw.base));
    if (!bound.sorted) {
        // The sort mark is set: the rows are not in symbol order yet, so a bisection would walk past
        // the one being looked for and answer "absent" for a system that is right there.
        for (uint64_t i = 0; i < bound.rows; ++i) {
            const uint32_t at = uint32_t(lp::kSystemTableStride * i);
            if (Rd<uint64_t>(base, at) == symbol) return Deref(base, at + 8u);
        }
        return nullptr;
    }
    uint64_t lo = 0, hi = bound.rows;
    while (lo < hi) {
        const uint64_t mid = lo + (hi - lo) / 2u;
        if (Rd<uint64_t>(base, uint32_t(lp::kSystemTableStride * mid)) < symbol)
            lo = mid + 1u;
        else
            hi = mid;
    }
    if (lo >= bound.rows) return nullptr;
    const uint32_t at = uint32_t(lp::kSystemTableStride * lo);
    return Rd<uint64_t>(base, at) == symbol ? Deref(base, at + 8u) : nullptr;
}

bool Entity::ResolveDiscEarly(void* netGame, DiscBind& out, Pool& candidate) const {
    out.early = true;
    if (!netGame) {
        out.earlyRefused = "no netGame to read the level space from";
        return false;
    }

    // Every candidate is probed even once one has answered: the walk is three binary searches over
    // a table of hundreds, it runs at the 2 Hz retry cadence and never per tick, and what it buys
    // is a refusal line that names which space was wrong.
    DiscBind::EarlyDiag& diag = out.earlyDiag;
    diag.n = DiscBind::EarlyDiag::kCandidates;
    void* actorSpace = nullptr;
    uint16_t actorIndex = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    size_t chosen = 0;
    bool anyRoot = false;
    for (size_t i = 0; i < DiscBind::EarlyDiag::kCandidates; ++i) {
        DiscBind::EarlyDiag::Cand& c = diag.cand[i];
        c.root = EarlySpaceRoot(netGame, i, &c.name);
        if (!c.root) continue;
        anyRoot = true;
        for (size_t j = 0; j < i; ++j) {
            if (diag.cand[j].root == c.root) { c.aliasOf = int8_t(j); break; }
        }
        if (c.aliasOf >= 0) continue;   // the same space answers the same way
        uint16_t idx = actorIndex;      // FrisbeeActorSpace clears it before it reads anything
        void* const found = FrisbeeActorSpace(c.root, idx, &diag, i);
        if (found && !actorSpace) {
            actorSpace = found;
            actorIndex = idx;
            chosen = i;
        }
    }
    if (!anyRoot) { out.earlyRefused = "the level game space does not exist yet"; return false; }
    if (!actorSpace) {
        out.earlyRefused = "no live game_frisbee actor in any candidate space or its parents";
        return false;
    }
    out.earlyActorIndex = actorIndex;
    out.earlyActorSpace = actorSpace;
    out.discSpace       = actorSpace;
    out.earlyRoot       = diag.cand[chosen].root;
    out.earlySpace      = uint8_t(chosen);

    void* const frisbeeCs = SystemInSpace(actorSpace, lp::kSymR14Frisbee);
    if (!frisbeeCs) {
        out.earlyRefused = "that space registers no R14Frisbee system";
        return false;
    }
    if (!replay::Readable(frisbeeCs, lp::kFrisbeeCsPhysCsOff + sizeof(void*))) {
        out.earlyRefused = "the R14Frisbee system is not readable";
        return false;
    }
    out.frisbeeCs = frisbeeCs;

    // The LIVE count, never the enabled count at +0xF4: the host parks the disc between rounds and
    // a bind bounded by the enabled prefix would refuse exactly the pre_match state this path
    // exists to serve (localpawn_facts.h, DISC.PHYS_CS_LIVE_COUNT_OFF).
    const uint16_t live = Rd<uint16_t>(frisbeeCs, lp::kPhysCsLiveCountOff);
    if (live == 0) { out.earlyRefused = "the level's frisbee system holds no live component"; return false; }
    if (live != 1) {
        // The actor registry names the actor; it does not name which of several components on it
        // is that actor's. With one live component there is nothing to pick between, and the
        // engine's own validity test reduces to the dense bound we are already inside.
        out.earlyRefused = "the level's frisbee system holds more than one live component";
        return false;
    }

    Pool frisbee;
    frisbee.cs       = frisbeeCs;
    frisbee.stride   = lp::kFrisbeeCiStride;
    frisbee.mapOff   = lp::kFrisbeeCsMapOff;
    frisbee.poolOff  = lp::kFrisbeeCsPoolBaseOff;
    frisbee.countOff = lp::kPhysCsLiveCountOff;

    // Best effort, for the log line only: the forward map read backwards. A handle the engine has
    // freed can still point below the live count -- only its generation says otherwise -- so an
    // ambiguous map yields no id rather than a guessed one, and the bind does not depend on it.
    out.frisbeeCid = lp::kClearedComponentId;
    const uint16_t ids = Rd<uint16_t>(frisbeeCs, lp::kPhysCsIdCapacityOff);
    void* const map = Deref(frisbeeCs, lp::kFrisbeeCsMapOff);
    if (map && replay::Readable(map, 4u * size_t(ids))) {
        uint32_t found = 0;
        for (uint16_t h = 0; h < ids; ++h) {
            if (Rd<uint16_t>(map, 4u * uint32_t(h)) >= live) continue;
            ++found;
            out.frisbeeCid = uint32_t(h) | (uint32_t(Rd<uint16_t>(map, 4u * uint32_t(h) + 2u)) << 16);
        }
        if (found != 1) out.frisbeeCid = lp::kClearedComponentId;
    }

    const uint8_t* frisbeeCi = static_cast<const uint8_t*>(frisbee.ElementAt(0));
    if (!frisbeeCi || !replay::Readable(frisbeeCi, lp::kFrisbeeCiStride)) {
        out.earlyRefused = "the frisbee component instance is not readable";
        return false;
    }
    out.physId = Rd<uint32_t>(frisbeeCi, lp::kFrisbeeCiPhysIdOff);
    out.physCs = Deref(frisbeeCs, lp::kFrisbeeCsPhysCsOff);
    if (!out.physCs) {
        out.earlyRefused = "the frisbee system caches no net-physics system";
        return false;
    }

    void* rules = replay::Readable(netGame, lp::kNetGameRulesOff + sizeof(void*))
                      ? Deref(netGame, lp::kNetGameRulesOff)
                      : nullptr;
    out.shortcutAgreed = rules && replay::Readable(rules, lp::kRulesPhysIdOff + sizeof(uint32_t)) &&
                         Deref(rules, lp::kRulesPhysCsOff) == out.physCs &&
                         Rd<uint32_t>(rules, lp::kRulesPhysIdOff) == out.physId;

    candidate           = Pool();
    candidate.cs        = out.physCs;
    candidate.stride    = facts::PHYS_CI_STRIDE;
    candidate.mapOff    = facts::PHYS_CS_HANDLE_MAP_OFF;
    candidate.poolOff   = facts::PHYS_CS_POOL_BASE_OFF;
    candidate.countOff  = facts::PHYS_CS_COUNT_OFF;
    candidate.revMapOff = facts::PHYS_CS_REVERSE_MAP_OFF;
    const PhysHandle h = ComponentIdIn(candidate, PhysHandle::kPoolDisc, out.physId, &out.reading);
    if (h.v == 0xFFFFu) {  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        out.physPoolRefused = true;
        out.earlyRefused = "the net-physics system does not vouch for the disc body";
        return false;
    }
    out.handle = h;
    out.earlyRefused = nullptr;
    return true;
}

bool Entity::BindDisc(void* netGame, DiscBind& out) {
    out = DiscBind();
    out.playerPhysCs = phys_.cs;
    disc_ = Pool();
    discNetGame_ = netGame;
    discEarly_ = false;
    discEarlyRefusal_ = nullptr;
    discEarlyDiag_ = DiscBind::EarlyDiag();
    discEarlyRoot_ = nullptr;
    discEarlySpace_ = 0;
    discDisagreeStreak_ = 0;

    Pool candidate;
    if (ResolveDiscFromRules(netGame, out, candidate)) {
        disc_ = candidate;
        discPhysId_ = out.physId;
        // The rules path names the R14Frisbee system too, and the diagnostic read of
        // `timesincelastheld` needs it from whichever arm bound the disc, not just the early one.
        discFrisbeeCs_ = out.frisbeeCs;
        discActorSpace_ = out.discSpace;
        discActorIndex_ = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE the ref names a component, not a registry slot
        out.refused = nullptr;
        return true;
    }

    // The engine has not resolved its own ref yet -- the whole ~100 s of pre_match. Resolve the
    // actor ourselves. The rules refusal stays in `refused` so the shipped bind line is unchanged;
    // the early hop that stopped us, if any, rides alongside it.
    const char* const rulesRefused = out.refused;
    DiscBind early;
    early.playerPhysCs = phys_.cs;
    Pool earlyPool;
    if (!ResolveDiscEarly(netGame, early, earlyPool)) {
        out.earlyRefused = early.earlyRefused;
        out.earlyDiag    = early.earlyDiag;
        discEarlyDiag_   = early.earlyDiag;
        discEarlyRefusal_ = early.earlyRefused;
        out.refused = rulesRefused;
        out.earlyActorIndex = early.earlyActorIndex;
        // "The chain named a disc and the pool would not vouch for it" is a different verdict from
        // "the chain could not be read", and on this path only the early attempt got far enough to
        // tell the two apart. The rules path's own answer, when it had one, still wins.
        if (!out.physPoolRefused && early.physPoolRefused) {
            out.physPoolRefused = true;
            out.reading = early.reading;
        }
        return false;
    }
    out = early;
    out.refused = nullptr;
    disc_ = earlyPool;
    discPhysId_ = out.physId;
    discEarly_      = true;
    discEarlyDiag_  = out.earlyDiag;
    discLevelSpace_ = replay::Readable(netGame, lp::kNetGameLevelSpaceOff + sizeof(void*))
                          ? Deref(netGame, lp::kNetGameLevelSpaceOff)
                          : nullptr;
    discEarlySpace_ = out.earlySpace;
    discEarlyRoot_  = out.earlyRoot;
    discActorSpace_ = out.earlyActorSpace;
    discActorIndex_ = out.earlyActorIndex;
    discFrisbeeCs_  = out.frisbeeCs;
    return true;
}

bool Entity::ResolveDiscFromRules(void* netGame, DiscBind& out, Pool& candidate) const {
    void* rules = netGame ? Deref(netGame, lp::kNetGameRulesOff) : nullptr;
    if (!rules) { out.refused = "no rules block on this netGame"; return false; }

    // The R14Frisbee component ref. sub_14011C8A0 refuses an unresolved one the same way.
    void* ref = At(rules, lp::kRulesFrisbeeRefOff);
    if (!replay::Readable(ref, 0x50u)) { out.refused = "the frisbee ref is not readable"; return false; }
    void* space = Deref(ref, lp::kCompRefSpaceOff);
    if (!space) { out.refused = "the frisbee ref is unresolved"; return false; }
    if (!replay::Readable(space, lp::kGameSpaceEpochOff + sizeof(uint64_t))) {
        out.refused = "the space the frisbee ref names is not readable";
        return false;
    }
    // The engine would take a spinlock and re-resolve here. A read-only plugin refuses the tick and
    // comes back: reading the stale {cs, id} pair is how you drive a component that no longer exists.
    if (Rd<uint64_t>(space, lp::kGameSpaceEpochOff) != Rd<uint64_t>(ref, lp::kCompRefStampOff)) {
        out.refused = "the frisbee ref is stale (the engine has not re-resolved it yet)";
        return false;
    }

    out.discSpace  = space;
    out.frisbeeCs  = Deref(ref, lp::kCompRefCsOff);
    out.frisbeeCid = Rd<uint32_t>(ref, lp::kCompRefIdOff);
    if (!out.frisbeeCs) { out.refused = "the frisbee ref names no component system"; return false; }
    if (static_cast<uint16_t>(out.frisbeeCid) == 0xFFFFu) {  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        out.refused = "the frisbee ref holds the cleared component id";
        return false;
    }

    // IsComponentValid on the FRISBEE system, then its own dense pool, exactly as
    // CR15FrisbeeCS::Owner sub_1409174D0 does before it reads SR15FrisbeeCI+0x18.
    Pool frisbee;
    frisbee.cs       = out.frisbeeCs;
    frisbee.stride   = lp::kFrisbeeCiStride;
    frisbee.mapOff   = lp::kFrisbeeCsMapOff;
    frisbee.poolOff  = lp::kFrisbeeCsPoolBaseOff;
    frisbee.countOff = lp::kPhysCsLiveCountOff;
    ComponentIdReading fr;
    if (ComponentIdIn(frisbee, PhysHandle::kPoolDisc, out.frisbeeCid, &fr).v == 0xFFFFu) {  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        out.refused = "the frisbee system does not vouch for its own component id";
        return false;
    }
    const void* frisbeeCi = frisbee.ElementAt(fr.poolIndex);
    if (!frisbeeCi || !replay::Readable(frisbeeCi, lp::kFrisbeeCiStride)) {
        out.refused = "the frisbee component instance is not readable";
        return false;
    }
    out.physId = Rd<uint32_t>(frisbeeCi, lp::kFrisbeeCiPhysIdOff);
    out.physCs = Deref(out.frisbeeCs, lp::kFrisbeeCsPhysCsOff);
    if (!out.physCs) { out.refused = "the frisbee system caches no net-physics system"; return false; }

    // rules+0x290 / +0x298 is the R14NetPhysics record on the same actor: the same two values by a
    // route that does not touch the frisbee pool at all. Disagreement is reported, not fatal -- the
    // frisbee route is the one CR15FrisbeeCS::Owner uses and stays authoritative.
    out.shortcutAgreed = Deref(rules, lp::kRulesPhysCsOff) == out.physCs &&
                         Rd<uint32_t>(rules, lp::kRulesPhysIdOff) == out.physId;

    candidate           = Pool();
    candidate.cs        = out.physCs;
    candidate.stride    = facts::PHYS_CI_STRIDE;
    candidate.mapOff    = facts::PHYS_CS_HANDLE_MAP_OFF;
    candidate.poolOff   = facts::PHYS_CS_POOL_BASE_OFF;
    candidate.countOff  = facts::PHYS_CS_COUNT_OFF;
    candidate.revMapOff = facts::PHYS_CS_REVERSE_MAP_OFF;
    const PhysHandle h = ComponentIdIn(candidate, PhysHandle::kPoolDisc, out.physId, &out.reading);
    if (h.v == 0xFFFFu) {  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        out.physPoolRefused = true;
        out.refused = "the net-physics system does not vouch for the disc body";
        return false;
    }
    out.handle = h;
    out.refused = nullptr;
    return true;
}

// The hop BindDisc walked, walked again -- and NOT on the tick path: disc_.cs is a raw pointer into
// the level game space, and a map teardown frees it without moving anything the bind keyed on. The
// caller runs this at the 2 Hz maintenance cadence, so the syscalls here are affordable and the
// per-tick DiscStands stays three guarded reads.
// The early path's own re-walk. Same question, same cadence, different chain: the level space, the
// actor slot the bind keyed on, and the system that space registers must all still be the ones we
// resolved -- and that system must still cache the net-physics pool the disc is bound to.
bool Entity::EarlyChainStands(void* netGame) const {
    if (!netGame || !replay::Readable(netGame, lp::kNetGameLevelSpaceOff + sizeof(void*)))
        return false;
    if (Deref(netGame, lp::kNetGameLevelSpaceOff) != discLevelSpace_) return false;

    // The bind may have come from a candidate other than the level space, so the re-walk starts
    // where the bind started -- re-derived from the netGame, not remembered, so a root that has
    // been swapped under us refuses instead of being trusted.
    void* const root = EarlySpaceRoot(netGame, discEarlySpace_, nullptr);
    if (!root || root != discEarlyRoot_) return false;

    uint16_t actorIndex = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    if (FrisbeeActorSpace(root, actorIndex) != discActorSpace_ ||
        actorIndex != discActorIndex_ || actorIndex == 0xFFFFu)  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        return false;

    void* const frisbeeCs = SystemInSpace(discActorSpace_, lp::kSymR14Frisbee);
    if (!frisbeeCs || frisbeeCs != discFrisbeeCs_ ||
        !replay::Readable(frisbeeCs, lp::kFrisbeeCsPhysCsOff + sizeof(void*)))
        return false;
    return Deref(frisbeeCs, lp::kFrisbeeCsPhysCsOff) == disc_.cs;
}

bool Entity::DiscChainStands(void* netGame, DiscChainReading* seen) const {
    DiscChainReading r;
    r.early = discEarly_;
    if (seen) *seen = r;
    if (!disc_.cs) return false;

    if (discEarly_) {
        // Once the host reaches round start the engine fills its own ref, and then there are two
        // answers. Agreement hands the drive over without a drop -- it is the same body, so
        // nothing has to be released and re-claimed. Disagreement prefers the engine's ref, which
        // means dropping cleanly and letting the retry re-bind through it.
        DiscBind fromRules;
        Pool rulesPool;
        if (ResolveDiscFromRules(netGame, fromRules, rulesPool)) {
            r.refResolved = true;
            r.refPhysCs   = fromRules.physCs;
            r.refPhysId   = fromRules.physId;
            r.agreed      = fromRules.physCs == disc_.cs && fromRules.physId == discPhysId_;
            if (r.agreed) {
                discDisagreeStreak_ = 0;
                if (seen) *seen = r;
                discEarly_ = false;
                return true;
            }
            // LAUNCH-FIX3 counted 425 changes of answer from this chain at 2 Hz while the host
            // idled: a ref that resolves for one sample and refuses the next would otherwise drop
            // and re-bind a perfectly good disc twice a second. A disagreement has to hold across
            // two consecutive fresh, fully valid resolutions before it costs the drive a drop.
            ++discDisagreeStreak_;
            r.disagreeStreak = discDisagreeStreak_;
            if (seen) *seen = r;
            return discDisagreeStreak_ < kDiscDisagreeConfirm;
        }
        discDisagreeStreak_ = 0;
        return EarlyChainStands(netGame);
    }

    if (!netGame || !replay::Readable(netGame, lp::kNetGameRulesOff + sizeof(void*)))
        return false;
    void* rules = Deref(netGame, lp::kNetGameRulesOff);
    void* ref = rules ? At(rules, lp::kRulesFrisbeeRefOff) : nullptr;
    if (!ref || !replay::Readable(ref, 0x50u)) return false;

    void* space = Deref(ref, lp::kCompRefSpaceOff);
    if (!space || !replay::Readable(space, lp::kGameSpaceEpochOff + sizeof(uint64_t))) return false;
    // Equal means fresh -- sub_14011C8A0 returns without re-resolving on equality. An unequal stamp
    // is the engine's own word that the {cs, id} pair behind this ref no longer names anything.
    if (Rd<uint64_t>(space, lp::kGameSpaceEpochOff) != Rd<uint64_t>(ref, lp::kCompRefStampOff))
        return false;

    void* frisbeeCs = Deref(ref, lp::kCompRefCsOff);
    if (!frisbeeCs || !replay::Readable(frisbeeCs, lp::kFrisbeeCsPhysCsOff + sizeof(void*)))
        return false;
    // And that the route still names the SAME net-physics system the disc pool is bound to. A
    // level rebuilt under us resolves to a different object, which is a drop, not a re-bind.
    return Deref(frisbeeCs, lp::kFrisbeeCsPhysCsOff) == disc_.cs;
}

bool Entity::ReadPlayer(PlayerHandle h, PlayerState& out) const {
    const void* ci = player_.Element(h.v);
    if (!ci) return false;

    memcpy(out.hmdOri,   At(const_cast<void*>(ci), facts::PLAYER_RAW_HMD_ORI_OFF),    sizeof out.hmdOri);
    memcpy(out.lHandOri, At(const_cast<void*>(ci), facts::PLAYER_RAW_HAND_ORI_A_OFF), sizeof out.lHandOri);
    memcpy(out.rHandOri, At(const_cast<void*>(ci), facts::PLAYER_RAW_HAND_ORI_B_OFF), sizeof out.rHandOri);
    HalfToFloat3(At(const_cast<void*>(ci), facts::PLAYER_RAW_HANDOFF_A_OFF), out.lHandOff);
    HalfToFloat3(At(const_cast<void*>(ci), facts::PLAYER_RAW_HANDOFF_B_OFF), out.rHandOff);
    memcpy(out.refVel,   At(const_cast<void*>(ci), facts::PLAYER_RAW_REFVEL_OFF),     sizeof out.refVel);

    out.bendBack       = Rd<float>(ci, facts::PLAYER_RAW_BEND_BACK_OFF);
    out.bendFwd        = Rd<float>(ci, facts::PLAYER_RAW_BEND_FWD_OFF);
    out.restPoseHeight = Rd<float>(ci, facts::PLAYER_RAW_RESTPOSE_OFF);
    out.trigL          = HalfToFloat(Rd<uint16_t>(ci, kTrigLOff));
    out.trigR          = HalfToFloat(Rd<uint16_t>(ci, kTrigROff));
    out.bits28         = Rd<uint32_t>(ci, facts::PLAYER_CI_ACTION_WORD_OFF);
    out.holdL          = Rd<uint32_t>(ci, kHoldLOff);
    out.holdR          = Rd<uint32_t>(ci, kHoldROff);
    out.anchor         = Rd<uint32_t>(ci, kAnchorOff);
    return true;
}

PlayerHandle Entity::PlayerOf(UserIndex u) const {
    // The pool is small (one entry per player), so a linear scan of the reverse map costs less
    // than any index we could build and keep coherent with the engine's own compaction.
    const size_t n = player_.Count();
    for (size_t i = 0; i < n; ++i) {
        const uint16_t handle = player_.HandleAt(i);
        if (handle == 0xFFFFu) continue;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        const void* ci = player_.Element(handle);
        if (ci && Rd<uint16_t>(ci, facts::PLAYER_CI_USERID_OFF) == u.v) return PlayerHandle(handle);
    }
    return PlayerHandle(0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
}

// P2, diagnostic only. If no wall-grab sound is heard, exactly two things can be true below the
// publish: EmitChangedDelegates was never resolved, or it ran and nothing listened. Nothing today
// separates them, so each published pose is counted three ways -- attempted, refused for a null
// entry point, and carrying a bit-5/6 ("hand attached to geo") transition the engine's own diff
// would fire on. Bits 5 and 6 of the action word are the two delegates; sub_140CCE580 raises them.
namespace {

constexpr uint32_t kActionWordInRaw =
    facts::PLAYER_CI_ACTION_WORD_OFF - facts::PLAYER_RAW_HMD_ORI_OFF;
constexpr uint32_t kGeoAttachBits = (1u << 5) | (1u << 6);

struct GrabEmitCounters {
    uint64_t attempted = 0;
    uint64_t emit_null = 0;
    uint64_t geo_edges = 0;
    uint64_t no_handle = 0;
    uint64_t gated = 0;
    uint64_t emitted = 0;
    uint32_t last_handle = 0;
    uint64_t reported_ms = 0;
} g_grabEmit;

// The A/B switch behind `playback.delegate_events`. It exists so a launch can answer "does this
// feed make any sound at all" by running the same content twice; without it the only control is a
// different span of the recording, which carries different sound of its own.
bool g_delegateEmit = true;

}  // namespace

void Entity::EmitDelegates(PlayerHandle h, const void* before, const void* after) const {
    ++g_grabEmit.attempted;
    if (before && after) {
        const uint32_t b = Rd<uint32_t>(before, kActionWordInRaw);
        const uint32_t a = Rd<uint32_t>(after, kActionWordInRaw);
        if (((b ^ a) & kGeoAttachBits) != 0u) ++g_grabEmit.geo_edges;
    }
    const uint64_t now = GetTickCount64();
    if (now - g_grabEmit.reported_ms >= 1000u) {  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
        g_grabEmit.reported_ms = now;
        replay::log::Direct(replay::LogLevel::Info, "entity",
                    "grab emit: published %llu poses, %llu refused for a null "
                    "EMIT_CHANGED_DELEGATES, %llu carried a hand-attached-to-geo bit transition, "
                    "%llu refused for an unbuildable pool handle, last handle 0x%08X, "
                    "%llu reached the engine, %llu withheld (playback.delegate_events %s)",
                    (unsigned long long)g_grabEmit.attempted,
                    (unsigned long long)g_grabEmit.emit_null,
                    (unsigned long long)g_grabEmit.geo_edges,
                    (unsigned long long)g_grabEmit.no_handle,
                    g_grabEmit.last_handle,
                    (unsigned long long)g_grabEmit.emitted,
                    (unsigned long long)g_grabEmit.gated,
                    g_delegateEmit ? "on" : "off");
    }
    if (!g_delegateEmit) {
        ++g_grabEmit.gated;
        return;
    }
    if (!emit_) {
        ++g_grabEmit.emit_null;
        return;
    }
    uint16_t index = 0;
    uint32_t compact = 0;
    const HandleBuild built = BuildDelegateHandle(h, compact, index);
    if (built == HandleBuild::NoElement) return;
    if (built != HandleBuild::Ok) {
        ++g_grabEmit.no_handle;
        return;
    }
    g_grabEmit.last_handle = compact;
    ++g_grabEmit.emitted;
    emit_(player_.cs, compact, before, after);
}

void Entity::PushPlayerHistory(PlayerHandle, const PlayerState&, uint64_t) {
    // Deliberately inert, and this is a decision rather than an omission. Under authority --
    // which an offline replay always is -- UpdateInterpolatedPlayerStates never reads the player
    // history: its short-circuit fires on the net flags before it looks at the ring, so a filled
    // ring changes nothing an observer could see. PP-B publishes through PLAYER_CI_RAW_STATE_OFF
    // instead. See docs/spikes/W1-E.md §4.1.
}

// --------------------------------------------------------------------------------------
// PP-B. Write the raw block; the engine's own authority branch decodes it into both pose slots
// later in the same frame. PP-A, which authors the slots directly, needs a late tick because
// that branch would otherwise overwrite it -- W1-E did not select it and does not ship it.
// --------------------------------------------------------------------------------------
class PosePublisherB final : public IPosePublisher {
public:
    explicit PosePublisherB(Entity* e) : e_(e) {}

    void Publish(PlayerHandle h, const PlayerState& renderPose, const PlayerState&) override {
        void* ci = e_->PlayerElement(h);
        if (!ci) return;

        // The engine copies pose slot 1 verbatim into slot 2, so PP-B cannot give the two slots
        // different times. Ground-truth playback wants them identical anyway (both at the
        // playhead); as-seen playback is the caller that must select PP-A instead.
        uint8_t before[kRawBytes];
        memcpy(before, At(ci, facts::PLAYER_RAW_HMD_ORI_OFF), sizeof before);

        WriteRaw(ci, renderPose);

        // EmitChangedDelegates has exactly one caller in the image and it is the wire ingest, so
        // nothing raises the action delegates on a playback path unless we do. §5.4.5's capture
        // rule is right that the raw word reproduces every edge -- but only once something
        // diffs it. See docs/spikes/W1-E.md F-B.
        e_->EmitDelegates(h, before, At(ci, facts::PLAYER_RAW_HMD_ORI_OFF));
    }

    const char* Strategy() const override { return "PP-B"; }
    bool        NeedsLateTick() const override { return false; }

private:
    // The raw block spans the fields the authority branch reads, from the HMD quaternion through
    // the last float it copies.
    static constexpr uint32_t kRawBytes =
        (facts::PLAYER_RAW_RESTPOSE_OFF + 4u) - facts::PLAYER_RAW_HMD_ORI_OFF;

    static void WriteRaw(void* ci, const PlayerState& p) {
        memcpy(At(ci, facts::PLAYER_RAW_HMD_ORI_OFF),    p.hmdOri,   sizeof p.hmdOri);
        memcpy(At(ci, facts::PLAYER_RAW_HAND_ORI_A_OFF), p.lHandOri, sizeof p.lHandOri);
        memcpy(At(ci, facts::PLAYER_RAW_HAND_ORI_B_OFF), p.rHandOri, sizeof p.rHandOri);
        Float3ToHalf(p.lHandOff, At(ci, facts::PLAYER_RAW_HANDOFF_A_OFF));
        Float3ToHalf(p.rHandOff, At(ci, facts::PLAYER_RAW_HANDOFF_B_OFF));
        memcpy(At(ci, facts::PLAYER_RAW_REFVEL_OFF),     p.refVel,   sizeof p.refVel);
        Wr<float>(ci, facts::PLAYER_RAW_BEND_BACK_OFF, p.bendBack);
        Wr<float>(ci, facts::PLAYER_RAW_BEND_FWD_OFF,  p.bendFwd);
        Wr<float>(ci, facts::PLAYER_RAW_RESTPOSE_OFF,  p.restPoseHeight);
        Wr<uint32_t>(ci, facts::PLAYER_CI_ACTION_WORD_OFF, p.bits28);
        // THE grip write site, and now the only thing that asks
        // Feature::GripWrites. It was covered by the PlayerPose gate one frame upstream, which
        // is one hop of indirection rather than a gate -- the same pattern the header calls out
        // for ViewerSeat. These four stores reach into a live player's hands.
        if (!replay::livesafety::Allowed(replay::livesafety::Feature::GripWrites)) return;
        Wr<uint32_t>(ci, kHoldLOff,     p.holdL);
        Wr<uint32_t>(ci, kHoldROff,     p.holdR);
        // The per-hand analog grab input. sub_140C20DE0 gates the engine's own AttachHand on this
        // field being above ~4.8e-7, and AttachHand is the only site in the image that posts
        // event_sound_hand_grab_left/right -- so until these two stores existed, no replayed
        // player could grab geometry or another player, silently and invisibly. Recorded as a
        // LEVEL, so the engine derives its own edges and a scrub cannot double-fire one.
        Wr<uint16_t>(ci, kTrigLOff, FloatToHalf(p.trigL));
        Wr<uint16_t>(ci, kTrigROff, FloatToHalf(p.trigR));
    }

    Entity* e_;
};

}  // namespace

namespace replay {

// The width is taken from the wire table the ENCODER itself checks against, so the reader and the
// writer cannot drift apart: anything this returns fits kSimStateWire[0] by construction.  The
// assertions below say which named CI bits that keeps and which it drops -- the ownership-sequence
// nibble is wire content and survives; the pose-warp nibble (bits 4-7, the design row 6) sits
// inside the same byte and survives with it; the two local-only bits are above the field and cannot
// be carried by this format at any value.
uint32_t BodyWireBits(uint32_t ciFlags) {
    constexpr uint32_t kWireBits = (1u << bitio::kSimStateWire[0].bits) - 1u;
    static_assert((kWireBits & facts::PHYS_CI_PAUSED_BIT) == 0u,
                  "paused is local state; the 11-bit wire field cannot hold it");
    static_assert((kWireBits & facts::PHYS_CI_INTERP_ENABLE_BIT) == 0u,
                  "integration-disabled is local state; the 11-bit wire field cannot hold it");
    static_assert((kWireBits & facts::PHYS_CI_OWNERSEQ_NIBBLE_MASK) ==
                      facts::PHYS_CI_OWNERSEQ_NIBBLE_MASK,
                  "the ownership-sequence nibble is wire content and must survive the conversion");
    static_assert((kWireBits & (facts::PHYS_CI_OWNERSEQ_NIBBLE_MASK << 4)) ==
                      (facts::PHYS_CI_OWNERSEQ_NIBBLE_MASK << 4),
                  "the pose-warp nibble is wire content and must survive the conversion");
    return ciFlags & kWireBits;
}

const char* DescribeDiag(const BodyDiag& d, char* out, size_t cap) {
    if (!out || cap == 0) return "";
    // Each renderer owns its own buffer, so the unspecified argument evaluation order below is
    // not a hazard. A field the walk never reached prints `?`; a tag of 0 is a pointer that was
    // read and was null, which is a different thing and says so.
#if defined(W1E_MUTATE_DIAG_UNFILLED_ZERO)
    const bool kHonourFilled = false;   // Mutation control: print every field as a number.
#else
    const bool kHonourFilled = true;
#endif
    auto num = [kHonourFilled](char* b, size_t n, bool have, uint32_t v) -> const char* {
        if (!have && kHonourFilled) return "?";
        snprintf(b, n, "%u", v);
        return b;
    };
    auto tag = [kHonourFilled](char* b, size_t n, bool have, uint32_t v) -> const char* {
        if (!have && kHonourFilled) return "?";
        if (v == 0) return "null";
        snprintf(b, n, "%08x", v);
        return b;
    };
    char b0[12], b1[8], b2[8], b3[12], b4[12], b5[8], b6[8], b7[8], b8[8], b9[8], b10[8], b11[12],
         b12[8], b13[8];
    snprintf(out, cap,
             "bodyId=%s slot=%s liveCount=%s bypass=%s physCs=%s ci=%s ent=%s idx=%s rev=%s "
             "ident=%s angVel=%s bodySource=%s cphBody=%s xform=%s xfIdx=%s xfLive=%s "
             "csIdx=%s csCount=%s ciFrom=%s netLive=%s mapHigh=%s",
             num(b0, sizeof b0, (d.filled & kBodyDiagBodyId) != 0, d.bodyId),
             num(b1, sizeof b1, (d.filled & kBodyDiagSlot) != 0, d.slot),
             num(b2, sizeof b2, (d.filled & kBodyDiagLiveCount) != 0, d.liveCount),
             (d.filled & kBodyDiagBypass) ? BypassReadName(d.bypass) : "?",
             tag(b3, sizeof b3, (d.filled & kBodyDiagPhysCs) != 0, d.physCsTag),
             tag(b4, sizeof b4, (d.filled & kBodyDiagCi) != 0, d.ciTag),
             num(b5, sizeof b5, (d.filled & kBodyDiagCiEntity) != 0, d.ciEntityId),
             num(b6, sizeof b6, (d.filled & kBodyDiagCiIndex) != 0, d.ciIndex),
             num(b7, sizeof b7, (d.filled & kBodyDiagRevHandle) != 0, d.revHandle),
             (d.filled & kBodyDiagIdentity) ? IdentityProofName(d.identity) : "?",
             (d.filled & kBodyDiagAngVel) ? AngVelSourceName(d.angVel) : "?",
             (d.filled & kBodyDiagOrigin) ? BodyOriginName(d.origin) : "?",
             (d.filled & kBodyDiagCphArm) ? BodyRefusalName(d.cphBodyArm) : "?",
             (d.filled & kBodyDiagXformArm) ? BodyRefusalName(d.xformArm) : "?",
             num(b8, sizeof b8, (d.filled & kBodyDiagXformIdx) != 0, d.xformIndex),
             num(b9, sizeof b9, (d.filled & kBodyDiagXformLive) != 0, d.xformLive),
             num(b10, sizeof b10, (d.filled & kBodyDiagCsIdx) != 0, d.csIdx),
             num(b11, sizeof b11, (d.filled & kBodyDiagCsCount) != 0, d.csCount),
             (d.filled & kBodyDiagCiFrom) ? CiPathName(d.ciFrom) : "?",
             num(b12, sizeof b12, (d.filled & kBodyDiagNetLive) != 0, d.netLive),
             num(b13, sizeof b13, (d.filled & kBodyDiagMapHigh) != 0, d.mapHigh));
    out[cap - 1] = '\0';
    return out;
}

namespace entity {

void SetDelegateEmitEnabled(bool on) { ::g_delegateEmit = on; }
bool DelegateEmitEnabled() { return ::g_delegateEmit; }

void SetDiscNodeHeldEnabled(bool on) { ::g_discNodeHeld = on; }
bool DiscNodeHeldEnabled() { return ::g_discNodeHeld; }
NodeHeldDiag NodeHeldDiagRead() { return ::g_nodeDiag; }

const char* ResolveText(Resolve r) {
    switch (r) {
        case Resolve::Ok:               return "ok";
        case Resolve::NoGameSpace:      return "no game space supplied";
        case Resolve::SpaceNotReady:    return "the game space's component-system table is not ready to be searched";
        case Resolve::NoPhysicsCs:      return "CR15NetPhysicsCS did not resolve";
        case Resolve::NoRemotePlayerCs: return "CR15RemotePlayerCS did not resolve";
        case Resolve::PoolNotReady:     return "a component system resolved but its pool is not built yet";
        case Resolve::NoBroadcasterCs:  return "CR15PlayerBroadcasterCS did not resolve, so no player is known to be local";
        case Resolve::NoEchoUnitCs:     return "CR15EchoUnitCS did not resolve, so the local pose has no source";
        case Resolve::NoBodyCs:         return "CR15BodyCS did not resolve, so the local body scalars have no source";
        case Resolve::NoFindComponent:  return "the actor-to-component entry point failed its byte guard";
    }
    return "unknown";
}

// Both objects have static storage duration and are never deleted; the Destroy entry points exist
// so a consumer's teardown reads symmetrically and so the choice is made here rather than in
// twelve call sites.
static Entity          g_entity;
static PosePublisherB  g_publisherB(&g_entity);

IEntity* EntityCreate(void* gameSpace) {
    g_entity.Init(gameSpace, nullptr, nullptr);
    return &g_entity;
}

bool ScanActorPoolFor(void* gameSpace, uint64_t prefabId, IEntity::ActorPoolRow& out) {
    out = IEntity::ActorPoolRow();
    if (prefabId == 0) return false;
    IEntity::ActorPoolRow row;
    for (uint32_t c = 0; c < lp::kClassScanMax; ++c) {
        if (!ReadActorPoolIn(gameSpace, static_cast<uint16_t>(c), row)) continue;
        if (row.prefabId != prefabId) continue;
        out = row;
        return true;
    }
    return false;
}

IEntity* EntityBind(void* gameSpace) {
    BindEpochAdvance();
    g_bindCurrent = true;
    return EntityCreate(gameSpace);
}

IEntity* EntityUnbind() {
    g_bindCurrent = false;
    return EntityCreate(nullptr);
}

IEntity* EntityCreateWithLookup(void* gameSpace, CsLookupFn find,
                                const replay::entity::EngineEntryPoints* injected) {
    if (find) g_entity.Init(gameSpace, find, injected);
    return &g_entity;
}

void EntityDestroy(IEntity* e) {
    // Restoring the owners we replaced is the only teardown that matters: leaving a synthetic
    // owner behind would leave a body permanently net-driven with nothing driving it.
    if (e == &g_entity) g_entity.RestoreAll();
}

IPosePublisher* PosePublisherCreate(IEntity* e, PoseStrategy strategy) {
    if (strategy != PoseStrategy::PpB || e != &g_entity) return nullptr;
    return &g_publisherB;
}

void PosePublisherDestroy(IPosePublisher*) {}

const char* LocalPawnDiagnostic(IEntity* e) {
    return (e == &g_entity) ? g_entity.LocalPawnText() : "no IEntity";
}

void* RemotePlayerSystem(IEntity* e) {
    return (e == &g_entity) ? g_entity.RemotePlayerCs() : nullptr;
}


}  // namespace entity

bool IEntity::ActorPoolInSpace(void* gameSpace, uint64_t prefabId, IEntity::ActorPoolRow& out) {
    return entity::ScanActorPoolFor(gameSpace, prefabId, out);
}

bool IEntity::AudioMixFrom(void* soundPlayer, IEntity::AudioMix& out) {
    return ::ReadAudioMixTables(soundPlayer, out);
}

// The bit order of ScriptSelfRow::flagsAnswered, once, so the reader's kDiscFlagSyms and this
// printer's names are the same list read twice rather than two lists kept in step by hand.
const char* const IEntity::kDiscFlagNames[IEntity::kDiscFlagCount] = {
    "is_game_disc", "is_local_disc", "is_sim_disc", "is_practice_disc", "in_score_state",
    "fx_active",
};

void IEntity::AudioDecodePositioningInfo(const uint8_t* buf, AudioPlayingSound& out) {
    if (buf == nullptr) return;
    float v = 0.0f;
    out.attenuation = buf[lp::kAkPosInfoAttenuationOff] != 0u;
    memcpy(&v, buf + lp::kAkPosInfoMaxDistanceOff, sizeof v);  out.maxDistance = v;
    memcpy(&v, buf + lp::kAkPosInfoVolDryOff, sizeof v);       out.volDryAtMax = v;
    memcpy(&v, buf + lp::kAkPosInfoVolAuxGameOff, sizeof v);   out.volAuxGameDefAtMax = v;
    memcpy(&v, buf + lp::kAkPosInfoVolAuxUserOff, sizeof v);   out.volAuxUserDefAtMax = v;
}

// The distinct-set fold. `stored` is how many ids the query actually wrote, `reported` the size it
// says the set has -- they differ when the set is longer than the buffer, and reporting the second
// is the whole point: "this emitter answers to more listeners than we asked for" is a finding.
void IEntity::AudioAccumulateListenerSet(IEntity::AudioMix& io, const uint64_t* ids,
                                         uint32_t stored, uint32_t reported) {
    const uint32_t kept = stored < AudioListenerSet::kIdCap ? stored : AudioListenerSet::kIdCap;
    for (uint32_t s = 0; s < io.setsWritten; ++s) {
        AudioListenerSet& have = io.sets[s];
        if (have.size != reported) continue;
        bool same = true;
        for (uint32_t i = 0; i < kept && same; ++i) {
            same = have.ids[i] == ids[i];
        }
        if (!same) continue;
        ++have.emitters;
        return;
    }
    if (io.setsWritten >= AudioMix::kSetCap) return;
    AudioListenerSet& add = io.sets[io.setsWritten++];
    add.emitters = 1;
    add.size = reported;
    for (uint32_t i = 0; i < kept; ++i) {
        add.ids[i] = ids[i];
    }
}

}  // namespace replay
