#include "playback/residue/residuesite.h"

#include <string.h>

#include "bootstrap/buildguard.h"
#include "playback/residue/residue_facts.h"

namespace replay {
namespace playback {
namespace residue {
namespace {

namespace rf = residue_facts;

// Reads through a base that may be null and an offset that is always a fact. Kept local rather than
// shared because the three subsystems that need this pattern each want a different refusal
// behaviour, and a common one would have to pick.
template <typename T>
T Rd(const void* base, uint32_t off) {
    T v{};
    if (!base) {
        return v;
    }
    const uint8_t* p = static_cast<const uint8_t*>(base) + off;
    memcpy(&v, p, sizeof(T));
    return v;
}

void* Deref(const void* base, uint32_t off) {
    return base ? Rd<void*>(base, off) : nullptr;
}

uint8_t* At(void* base, size_t off) {
    return base ? static_cast<uint8_t*>(base) + off : nullptr;
}

void Wr32(void* base, uint32_t off, uint32_t v) {
    memcpy(static_cast<uint8_t*>(base) + off, &v, sizeof v);
}

void Wr64(void* base, uint32_t off, uint64_t v) {
    memcpy(static_cast<uint8_t*>(base) + off, &v, sizeof v);
}

// The standard component-system triple, checked before any element is addressed. A system whose
// pool base is still null resolves fine and reads garbage one dereference later.
bool PoolReady(const void* cs, uint32_t mapOff, uint32_t poolOff) {
    return cs && Deref(cs, mapOff) && Deref(cs, poolOff);
}

// The memoised epoch. File-static because the engine has exactly one CR15Game and this packet's rows
// are singletons registered into one battery; a per-instance memo would need an instance nobody has.
CsSpaceKey g_epoch;
CsSite     g_memo;
bool       g_memoValid = false;

// Set when the memoised answer is a "not yet", with the count of binds it has been served for since
// it was taken. See BindCsSite's contract in the header: a readiness refusal is held for at most
// kCsRetryBinds binds and then re-asked, because the key cannot see readiness change.
bool     g_memoNotYet = false;
uint32_t g_notYetServed = 0;

BindEpochFn g_bindEpoch = nullptr;

}  // namespace

// Whether an outcome may be memoised, and for how long. The header carries the reasoning for each
// arm; this is the one place the classification exists, so a new Bind value has to be sorted here.
//
// NO `default:` ARM, DELIBERATELY (AUD-F3F F3F-2). A default made every future Bind value silently
// Definitive -- the F3D-1 defect, one enum value at a time -- and no test could see it. Every value
// is listed, an unlisted one falls out of the switch to Unclassified, and tests/residue reds on any
// value that answers Unclassified. /W4 also warns (C4062) on the unhandled enumerator, but /WX is
// off (CMakeLists.txt:29), so the test is the mechanism and the warning is the courtesy.
CsHold CsHoldFor(Bind b) {
    switch (b) {
        case Bind::NoComponentLookup:
            return CsHold::Never;

        // NOT YET: the space is coming up and the key cannot see it happen.
        case Bind::NoNetPhysicsCs:
        case Bind::NoRemotePlayerCs:
        case Bind::NoPhysicsCs:
        case Bind::PoolNotReady:
            return CsHold::NotYet;

        // DEFINITIVE: each of these either is the answer or moves a term of the key the instant it
        // stops being true. The five that this file never returns (NoPhysSystem, NoZoneTable,
        // NoClock, NoOriginBlock, NoGame is returned by ResolveCsSite) are sorted anyway, because
        // the classification is over the enum, not over today's call sites.
        case Bind::Ok:
        case Bind::NoGame:
        case Bind::NoNetGame:
        case Bind::NoGameSpace:
        case Bind::NetGameUnreadable:
        case Bind::NoPhysSystem:
        case Bind::NoZoneTable:
        case Bind::NoClock:
        case Bind::NoOriginBlock:
            return CsHold::Definitive;

        case Bind::Count:
            break;      // not an outcome
    }
    return CsHold::Unclassified;
}

namespace {

// The one predicate BindCsSiteOn and CsSiteMemoHolds share, so they cannot disagree about whether
// this bind is served from the memo.
bool MemoAnswers(const CsSpaceKey& key) {
    if (!g_memoValid || !(key == g_epoch)) {
        return false;
    }
    return !g_memoNotYet || g_notYetServed < kCsRetryBinds;
}

// buildguard::MappedAddress re-walks the main image's PE headers on every call -- two VirtualQuery
// (buildguard.cpp:457,461) -- to bound an address against SizeOfImage. That bound cannot move: the
// main image is mapped before this DLL loads and is never unmapped while our code runs out of it. So
// the one address this per-frame path needs is resolved once and held (AUD-F3D F3D-3). The SLOT is
// held, never the CR15Game in it: that pointer is exactly the thing that changes.
const void* g_gameSlot = nullptr;

const void* GameSingletonSlot() {
    if (!g_gameSlot) {
        g_gameSlot = buildguard::MappedAddress(facts::GAME_SINGLETON_VA);
    }
    return g_gameSlot;
}

CsSite ResolveOn(const CsSpaceKey& key, FindCsFn find);

}  // namespace

const char* BindText(Bind b) {
    switch (b) {
        case Bind::Ok: return "ok";
        case Bind::NoGame: return "no CR15Game yet -- no session has started";
        case Bind::NoNetGame: return "the CR15Game carries no CR15NetGame";
        case Bind::NoGameSpace: return "the CR15NetGame carries no gamespace";
        case Bind::NoComponentLookup:
            return "CncaGameSpace::FindComponentSystem failed its prologue guard";
        case Bind::NoNetPhysicsCs: return "this gamespace has no R14NetPhysics component system";
        case Bind::NoRemotePlayerCs: return "this gamespace has no R14RemotePlayer component system";
        case Bind::NoPhysicsCs: return "the R14NetPhysics system carries no CPhysicsCS";
        case Bind::PoolNotReady: return "a component system resolved but its pool is not addressable";
        case Bind::NoPhysSystem: return "the CPhSystem singleton is null -- physics has not started";
        case Bind::NoZoneTable: return "the CPhSystem carries no zone array";
        case Bind::NoClock: return "the CClock singleton is null";
        case Bind::NoOriginBlock: return "the world re-origin global carries no block yet";
        case Bind::NetGameUnreadable:
            return "the CR15NetGame's game-space slot is not addressable -- not a CR15NetGame";
        default: return "?";
    }
}

// ---------------------------------------------------------------------------------------------
// Resolution. The two functions that read the engine.

CsSite ResolveCsSite() {
    CsSite s;

    const void* gamePtr = GameSingletonSlot();
    void* game = gamePtr ? Rd<void*>(gamePtr, 0) : nullptr;
    if (!game) {
        s.bind = Bind::NoGame;
        return s;
    }
    void* netGame = Deref(game, facts::GAME_NETGAME_PTR_OFF);
    if (!netGame) {
        s.bind = Bind::NoNetGame;
        return s;
    }

    // The one call this packet makes to find anything, and it is the engine's own binary search over
    // the gamespace's system table -- so no component-system instance offset is ever guessed. A site
    // that fails its prologue guard yields a null `find`, which BindCsSite reports as
    // NoComponentLookup -- but only after it has decided whether there is a space at all, so the two
    // failures keep the order they had before this function was split.
    //
    // WHAT A MEMO HIT COSTS, since the header used to claim it was nothing (AUD-F3D F3D-3). One
    // VirtualQuery: the Readable() inside CsSpaceKeyOf. The key is built ONCE here and handed to
    // both the memo question and the bind, so it is one syscall rather than two -- and, more than a
    // syscall, the two can no longer read a DIFFERENT key and diagnose a protection change that
    // landed between them as NoComponentLookup. The game singleton's mapping is held (see
    // GameSingletonSlot) and the guard below is skipped, so nothing else is asked.
    //
    // That last one does not go away and should not: it is the `readable` term of the key, which is
    // how a CR15NetGame whose pages went away is told from one that carries no space.
    const CsSpaceKey key = CsSpaceKeyOf(netGame);

    FindCsFn find = nullptr;
    if (!CsSiteMemoHolds(key) &&
        buildguard::VerifySite("CS.LOOKUP_BY_SYMBOL", facts::CS_LOOKUP_BY_SYMBOL,
                               facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE,
                               sizeof facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE)) {
        find = reinterpret_cast<FindCsFn>(
            const_cast<void*>(buildguard::MappedAddress(facts::CS_LOOKUP_BY_SYMBOL)));
    }
    return BindCsSiteOn(key, find);
}

// NETGAME.CS_SPACE_OFF, NOT IDENT.NETGAME_GAMESPACE_OFF. The +0x8 slot is the CR15NetGame's
// back-pointer to the CR15Game singleton -- never a CncaGameSpace (ARCH-10 S1) -- so handing it to
// CS.LOOKUP_BY_SYMBOL searched a render-target budget block as a component-system table and this bind
// reached NoNetPhysicsCs or worse on every fire. The space is +0x2B08.
//
// A CR15NetGame is far wider than this slot, so a base we cannot read that far into is not a netGame
// at all: that is a different answer from a netGame carrying no space yet. Both distinctions live in
// the key, so both survive the memo.
CsSpaceKey CsSpaceKeyOf(void* netGame) {
    CsSpaceKey key;
    if (g_bindEpoch) {
        key.bind = g_bindEpoch();
    }
    key.netGame = netGame;
    if (!netGame) {
        return key;
    }
    const void* const slot = static_cast<const uint8_t*>(netGame) + facts::NETGAME_CS_SPACE_OFF;
    if (!Readable(slot, sizeof(void*))) {
        return key;
    }
    key.readable = true;
    key.space = Deref(netGame, facts::NETGAME_CS_SPACE_OFF);
    return key;
}

void SetBindEpochSource(BindEpochFn fn) {
    g_bindEpoch = fn;
    InvalidateCsSite();
}

bool CsSiteMemoHolds(const CsSpaceKey& key) {
    return MemoAnswers(key);
}

bool CsSiteMemoHolds(void* netGame) {
    return MemoAnswers(CsSpaceKeyOf(netGame));
}

void InvalidateCsSite() {
    g_memoValid = false;
    g_memoNotYet = false;
    g_notYetServed = 0;
    g_memo = CsSite();
    g_epoch = CsSpaceKey();
}

CsSite BindCsSite(void* netGame, FindCsFn find) {
    return BindCsSiteOn(CsSpaceKeyOf(netGame), find);
}

CsSite BindCsSiteOn(const CsSpaceKey& key, FindCsFn find) {
    if (MemoAnswers(key)) {
        if (g_memoNotYet) {
            ++g_notYetServed;
        }
        return g_memo;
    }
    const CsSite s = ResolveOn(key, find);

    // Unclassified rides with Never: a value nobody sorted is answered fresh every bind, which costs
    // lookups and cannot serve a stale or frozen answer.
    const CsHold hold = CsHoldFor(s.bind);
    if (hold == CsHold::Never || hold == CsHold::Unclassified) {
        // Answered fresh every time, and the previous memo goes with it: leaving a stale one behind
        // would let the next bind be served from an answer this one just contradicted.
        InvalidateCsSite();
        return s;
    }
    g_epoch = key;
    g_memo = s;
    g_memoValid = true;
    g_memoNotYet = hold == CsHold::NotYet;
    g_notYetServed = 0;
    return s;
}

namespace {

CsSite ResolveOn(const CsSpaceKey& key, FindCsFn find) {
    CsSite s;
    if (!key.netGame) {
        s.bind = Bind::NoNetGame;
        return s;
    }
    if (!key.readable) {
        s.bind = Bind::NetGameUnreadable;
        return s;
    }
    void* gameSpace = key.space;
    if (!gameSpace) {
        s.bind = Bind::NoGameSpace;
        return s;
    }
    if (!find) {
        s.bind = Bind::NoComponentLookup;
        return s;
    }

    s.netPhysicsCs = find(gameSpace, facts::CS_SYM_R14NETPHYSICS);
    if (!s.netPhysicsCs) {
        s.bind = Bind::NoNetPhysicsCs;
        return s;
    }
    s.remotePlayerCs = find(gameSpace, facts::CS_SYM_R14REMOTEPLAYER);
    if (!s.remotePlayerCs) {
        s.bind = Bind::NoRemotePlayerCs;
        return s;
    }
    s.physicsCs = Deref(s.netPhysicsCs, rf::kNetCsPhysicsCsOff);
    if (!s.physicsCs) {
        s.bind = Bind::NoPhysicsCs;
        return s;
    }

    if (!PoolReady(s.netPhysicsCs, facts::PHYS_CS_HANDLE_MAP_OFF, facts::PHYS_CS_POOL_BASE_OFF) ||
        !PoolReady(s.remotePlayerCs, facts::PLAYER_CS_HANDLE_MAP_OFF,
                   facts::PLAYER_CS_POOL_BASE_OFF) ||
        !PoolReady(s.physicsCs, facts::PHYS_CS_HANDLE_MAP_OFF, facts::PHYS_CS_POOL_BASE_OFF)) {
        s.bind = Bind::PoolNotReady;
        return s;
    }

    s.bind = Bind::Ok;
    return s;
}

}  // namespace

PhysSite ResolvePhysSite() {
    PhysSite s;

    // ★ CLOCK.THIRD_PTR, not a physics id of this packet's own. The address was already in the
    // registry from the clock side; see residue_facts.h. Using the existing id is what keeps one
    // address under one name.
    const void* slot = buildguard::MappedAddress(facts::CLOCK_THIRD_PTR);
    s.system = slot ? Rd<const void*>(slot, 0) : nullptr;
    if (!s.system) {
        s.bind = Bind::NoPhysSystem;
        return s;
    }
    if (!Deref(s.system, rf::kPhSystemZoneArrayOff)) {
        s.bind = Bind::NoZoneTable;
        return s;
    }
    s.bind = Bind::Ok;
    return s;
}

// ---------------------------------------------------------------------------------------------
// The pure half.

bool PhTarget::Dead() const {
    return index == rf::kPhTargetDeadIndex;
}

size_t NetCiCount(const CsSite& cs) {
    return cs.netPhysicsCs ? Rd<uint16_t>(cs.netPhysicsCs, facts::PHYS_CS_COUNT_OFF) : 0u;
}

uint16_t NetHandleAt(const CsSite& cs, size_t poolIndex) {
    const void* rev = Deref(cs.netPhysicsCs, facts::PHYS_CS_REVERSE_MAP_OFF);
    if (!rev) {
        return rf::kNoComponentHandle;
    }
    return Rd<uint16_t>(rev, static_cast<uint32_t>(2u * poolIndex));
}

const void* NetCi(const CsSite& cs, uint16_t handle) {
    const void* map = Deref(cs.netPhysicsCs, facts::PHYS_CS_HANDLE_MAP_OFF);
    void* base = Deref(cs.netPhysicsCs, facts::PHYS_CS_POOL_BASE_OFF);
    if (!map || !base) {
        return nullptr;
    }
    const uint16_t slot = Rd<uint16_t>(map, static_cast<uint32_t>(4u * handle));
    return At(base, static_cast<size_t>(facts::PHYS_CI_STRIDE) * slot);
}

size_t PlayerCiCount(const CsSite& cs) {
    return cs.remotePlayerCs ? Rd<uint16_t>(cs.remotePlayerCs, facts::PLAYER_CS_COUNT_OFF) : 0u;
}

uint16_t PlayerHandleAt(const CsSite& cs, size_t poolIndex) {
    const void* rev = Deref(cs.remotePlayerCs, facts::PLAYER_CS_REVERSE_MAP_OFF);
    if (!rev) {
        return rf::kNoComponentHandle;
    }
    return Rd<uint16_t>(rev, static_cast<uint32_t>(2u * poolIndex));
}

bool PrimaryBodyTarget(const CsSite& cs, uint16_t physicsHandle, PhTarget& out) {
    if (physicsHandle == rf::kNoComponentHandle) {
        return false;
    }
    const void* map = Deref(cs.physicsCs, facts::PHYS_CS_HANDLE_MAP_OFF);
    void* base = Deref(cs.physicsCs, facts::PHYS_CS_POOL_BASE_OFF);
    if (!map || !base) {
        return false;
    }
    const uint16_t slot = Rd<uint16_t>(map, static_cast<uint32_t>(4u * physicsHandle));

    // The engine's own bound. Its own code skips this test when a flag one dereference away is set;
    // that flag is a pool-growth mode we have no business inferring, so the bound is always applied
    // here. Applying it costs a body only in a state where the engine itself would be reading past
    // its declared count.
    if (slot >= Rd<uint16_t>(cs.physicsCs, facts::PHYS_CS_COUNT_OFF)) {
        return false;
    }

    const uint8_t* ci = At(base, static_cast<size_t>(rf::kPhysicsCiStride) * slot);
    if (Rd<uint64_t>(ci, rf::kPhysicsCiBodyGuard) == 0) {
        return false;   // the entity has no primary body: honest, not a failure
    }

    const uint8_t* t = ci + rf::kPhysicsCiTargetOff;
    out.type      = Rd<uint64_t>(t, 0);
    out.index     = Rd<uint32_t>(t, rf::kPhTargetIndexOff);
    out.zoneIndex = Rd<uint32_t>(t, rf::kPhTargetZoneOff);
    out.tag       = Rd<uint32_t>(t, rf::kPhTargetTagOff);
    out.trailing  = Rd<uint32_t>(t, rf::kPhTargetTagOff + 4u);
    return !out.Dead();
}

void* ResolveBody(const PhysSite& ph, const PhTarget& t) {
    if (!ph.Ok() || t.Dead()) {
        return nullptr;
    }
    void* zoneArray = Deref(ph.system, rf::kPhSystemZoneArrayOff);
    if (!zoneArray) {
        return nullptr;
    }
    if (t.zoneIndex >= Rd<uint64_t>(ph.system, rf::kPhSystemZoneCountOff)) {
        return nullptr;
    }
    uint8_t* zone = At(zoneArray, static_cast<size_t>(rf::kPhZoneStride) * t.zoneIndex);
    if (Rd<uint32_t>(zone, rf::kPhIdIndexOff) == rf::kPhIdDead) {
        return nullptr;
    }
    if (t.index >= Rd<uint64_t>(zone, facts::PHYS_ZONE_BODY_COUNT_OFF)) {
        return nullptr;
    }
    void* bodyArray = Deref(zone, facts::PHYS_ZONE_BODY_ARRAY_OFF);
    if (!bodyArray) {
        return nullptr;
    }
    uint8_t* body = At(bodyArray, static_cast<size_t>(facts::PHYS_BODY_STRIDE) * t.index);
    if (Rd<uint32_t>(body, rf::kPhIdIndexOff) == rf::kPhIdDead) {
        return nullptr;
    }
    // The generation tag. Without it a recycled slot answers for the entity that used to own it,
    // and the wake lands on somebody else's body -- locality is not identity.
    if (Rd<uint32_t>(body, rf::kPhIdTagOff) != t.tag) {
        return nullptr;
    }
    return body;
}

bool BodyAsleep(const void* body) {
    return body && Rd<uint64_t>(body, facts::PHYS_BODY_SLEEP_STAMP_OFF) != 0;
}

bool BodyWakeable(const void* body) {
    if (!body) {
        return false;
    }
    return (Rd<uint32_t>(body, facts::PHYS_BODY_FLAGS_OFF) & rf::kPhBodyNotWakeableMask) == 0;
}

bool BodyClassified(const void* body) {
    if (!body) {
        return false;
    }
    const uint32_t cls = Rd<uint32_t>(body, rf::kPhBodyClassOff);
    return cls == rf::kPhBodyClassCollected[0] || cls == rf::kPhBodyClassCollected[1];
}

BodyWalk ForEachRestoredBody(const CsSite& cs, const PhysSite& ph, BodyFn fn, void* ctx) {
    BodyWalk w;
    if (!cs.Ok() || !ph.Ok() || !fn) {
        return w;
    }
    const size_t n = NetCiCount(cs);
    for (size_t i = 0; i < n; ++i) {
        ++w.entities;
        const uint16_t handle = NetHandleAt(cs, i);
        if (handle == rf::kNoComponentHandle) {
            continue;
        }
        const void* ci = NetCi(cs, handle);
        if (!ci) {
            continue;
        }
        const uint16_t physicsHandle =
            static_cast<uint16_t>(Rd<uint32_t>(ci, rf::kNetCiPhysicsHandleOff));
        PhTarget t;
        if (!PrimaryBodyTarget(cs, physicsHandle, t)) {
            continue;
        }
        void* body = ResolveBody(ph, t);
        if (!body) {
            continue;
        }
        ++w.bodies;
        if (BodyAsleep(body)) {
            ++w.asleep;
        }
        if (!BodyWakeable(body)) {
            continue;
        }
        ++w.wakeable;
        if (BodyClassified(body)) {
            ++w.classified;
        }
        if (fn(body, ctx)) {
            ++w.acted;
        }
    }
    return w;
}

bool RaiseMotionBit(void* body) {
    if (!body) {
        return false;
    }
    const uint32_t flags = Rd<uint32_t>(body, facts::PHYS_BODY_FLAGS_OFF);
    if ((flags & facts::PHYS_BODY_MOTION_BIT) != 0) {
        return false;   // already raised this pass; saying so is what keeps `acted` meaningful
    }
    Wr32(body, facts::PHYS_BODY_FLAGS_OFF, flags | facts::PHYS_BODY_MOTION_BIT);
    return true;
}

bool ClearHysteresis(void* body) {
    if (!body) {
        return false;
    }
    if (Rd<uint32_t>(body, facts::PHYS_BODY_HYSTERESIS_SKIP_OFF) != 0) {
        return false;
    }
    const bool changed = Rd<uint64_t>(body, facts::PHYS_BODY_HYSTERESIS_A_OFF) != 0 ||
                         Rd<uint32_t>(body, facts::PHYS_BODY_HYSTERESIS_B_OFF) != 0;
    // The widths are the engine's: a qword store at 0x14068CC85 and a dword store at 0x14068CC8C.
    // Zeroing eight bytes where the engine zeroes four would take the neighbouring field with it.
    Wr64(body, facts::PHYS_BODY_HYSTERESIS_A_OFF, 0);
    Wr32(body, facts::PHYS_BODY_HYSTERESIS_B_OFF, 0);
    return changed;
}


// ---------------------------------------------------------------------------------------------
// N-6. The world re-origin.

WorldSite ResolveWorldSite() {
    WorldSite s;

    const void* originPtr = buildguard::MappedAddress(facts::WORLD_REORIGIN_GLOBAL);
    s.originBlock = originPtr ? Rd<const void*>(originPtr, 0) : nullptr;
    if (!s.originBlock) {
        s.bind = Bind::NoOriginBlock;
        return s;
    }
    s.bind = Bind::Ok;
    return s;
}

bool ReadWorldOrigin(const void* block, unsigned char out[kOriginSpanBytes]) {
    if (!block || !out) {
        return false;
    }
    // WORLD.REORIGIN_OFF_X is the first float the re-origin function touches and
    // WORLD.REORIGIN_PENDING_OFF_Z the last, so the span is [X, PENDING_Z+4) and it covers BOTH
    // triples -- the cumulative one and the transient one the render thread zeroes. Read as bytes
    // rather than as floats: this is a latch of raw state, and the restore it feeds is refused
    // (N-6b), so nothing here needs the span decomposed into vectors.
    // static_asserts rather than runtime tests: these are constants from the generated header, so a
    // registry edit that moves them must stop the BUILD rather than silently read the wrong bytes.
    // W0-2's split of this block into a cumulative and a transient triple is exactly such an edit,
    // and it landed while this packet was being written.
    static_assert(facts::WORLD_REORIGIN_OFF_Y == facts::WORLD_REORIGIN_OFF_X + 4u &&
                      facts::WORLD_REORIGIN_OFF_Z == facts::WORLD_REORIGIN_OFF_X + 8u,
                  "the cumulative world offset is no longer three contiguous floats");
    // ★ THE ONE THAT MATTERS. The transient triple must stay OUTSIDE the latch: the render thread
    // zeroes it on every particle compute pass, so including it would report a world shift on
    // essentially every discontinuity.
    static_assert(facts::WORLD_REORIGIN_PENDING_OFF_X >=
                      facts::WORLD_REORIGIN_OFF_X + kOriginSpanBytes,
                  "the latch overlaps the transient re-origin triple and would report drift every "
                  "frame the render thread clears it");
    memcpy(out, static_cast<const uint8_t*>(block) + facts::WORLD_REORIGIN_OFF_X, kOriginSpanBytes);
    return true;
}

// ---------------------------------------------------------------------------------------------
// N-18. The CClock accumulators.

const char* ClockVerdictText(ClockVerdict v) {
    switch (v) {
        case ClockVerdict::Fresh: return "fresh (first sight of this CClock)";
        case ClockVerdict::Forward: return "forward";
        case ClockVerdict::Wrapped: return "unsigned wrap repaired";
        case ClockVerdict::Backwards: return "moved backwards (reported, not repaired)";
        default: return "?";
    }
}

namespace {

// No elapsed time in microseconds reaches 2^63 -- that is about 292,000 years -- so the top bit is
// not a threshold, it is a proof. RESET.STAT_REBASE_A's own reader branches on exactly this bit,
// which is how the wrapped value reaches a float instead of being rejected.
bool Wrapped(uint64_t v) {
    return (v & (1ull << 63)) != 0;
}

}  // namespace

ClockSite ResolveClockSite() {
    ClockSite s;
    const void* slot = buildguard::MappedAddress(facts::CLOCK_GLOBAL_PTR);
    s.clock = slot ? Rd<void*>(slot, 0) : nullptr;
    s.bind = s.clock ? Bind::Ok : Bind::NoClock;
    return s;
}

ClockVerdict GuardClockMonotonic(void* clock, ClockLatch& latch) {
    if (!clock) {
        return ClockVerdict::Fresh;
    }
    const uint64_t stamp = Rd<uint64_t>(clock, facts::CLOCK_LASTSTAMP_OFF);
    const uint64_t a1 = Rd<uint64_t>(clock, facts::CLOCK_ACCUM1_OFF);
    const uint64_t a2 = Rd<uint64_t>(clock, facts::CLOCK_ACCUM2_OFF);

    // ★ A DIFFERENT CClock IS NOT A REGRESSION. Between matches the object is rebuilt and its
    // accumulators legitimately start again at zero. Judging that as backwards motion would make
    // every match transition a false finding -- locality is not identity, on a pointer as much as
    // on a body slot.
    if (!latch.valid || latch.clock != clock) {
        latch.clock = clock;
        latch.stamp = stamp;
        latch.accum1 = a1;
        latch.accum2 = a2;
        latch.valid = true;
        return ClockVerdict::Fresh;
    }

    if (Wrapped(stamp) || Wrapped(a1) || Wrapped(a2)) {
        // Repaired, because there is no reading of a wrapped accumulator that is anyone else's
        // business: it is not a value, it is the absence of one.
        Wr64(clock, facts::CLOCK_LASTSTAMP_OFF, latch.stamp);
        Wr64(clock, facts::CLOCK_ACCUM1_OFF, latch.accum1);
        Wr64(clock, facts::CLOCK_ACCUM2_OFF, latch.accum2);
        return ClockVerdict::Wrapped;
    }

    if (stamp < latch.stamp || a1 < latch.accum1 || a2 < latch.accum2) {
        // ★★ REPORTED, NOT REPAIRED. Ledger row 0.4 gives clock writes to the transport, and a
        // decrease that has not wrapped is a value somebody chose. Silently undoing it would hide
        // their defect behind ours; the latch is left alone so the NEXT comparison is against the
        // last value this packet actually believed.
        return ClockVerdict::Backwards;
    }

    latch.stamp = stamp;
    latch.accum1 = a1;
    latch.accum2 = a2;
    return ClockVerdict::Forward;
}

const char* OrderingText(Ordering o) {
    switch (o) {
        case Ordering::NotChecked: return "not checked (no jump yet)";
        case Ordering::Ok: return "ok (the ring held pre-jump state when N-3a fired)";
        case Ordering::Violated:
            return "VIOLATED: the history ring was already at the jump destination when N-3a fired, "
                   "so ApplyStateAt ran before the battery and the reset deleted the state it "
                   "installed. W2-B's frame loop must fire the battery at step 4, before the apply";
        case Ordering::NoEntity: return "unavailable: IEntity is null, so nothing can be read back";
        case Ordering::NoHistoryReadback:
            return "unavailable: this IEntity does not implement ReadHistoryWindow, so the ring's "
                   "newest sample time cannot be read and the ordering cannot be judged";
        default: return "?";
    }
}

}  // namespace residue
}  // namespace playback
}  // namespace replay
