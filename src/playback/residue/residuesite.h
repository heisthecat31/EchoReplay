#pragma once

// W3-A. Where the category-N primitives reach, and the pure walks over what they find.
//
// ---------------------------------------------------------------------------------------------
// ★★★ WHY RESOLUTION AND EFFECT ARE SEPARATE FILES.
//
// A reset primitive is two things bolted together: a chain of dereferences that finds the engine
// object, and a store or a call once it is found. The chain is the part that cannot be run without
// a game, and the effect is the part that must be right. Fusing them makes the whole primitive
// untestable, which is how a battery ends up verified only by the fact that it compiled.
//
// So everything below takes its bases as PARAMETERS. In the product they come from ResolveCsSite()
// and ResolvePhysSite(), which read the engine. In tests/residue they come from a synthetic image
// built in a std::vector, and the SAME walk code runs over it -- not a mock of the walk, the walk.
// What the offline half cannot exercise is the two Resolve* functions' reads of the CR15Game and
// CPhSystem globals. The netGame -> game space -> lookup hop inside ResolveCsSite is NOT in that
// set -- it is BindCsSite below, and tests/residue drives it, because that hop is where the offset
// lives and an offset nothing can test is an offset that was wrong for months (ARCH-10 F3).
//
// ★★ AND WHY EVERY WALK RETURNS A COUNT.
//
// "the battery fired" is not an observable. A sweep that resolved nothing and a sweep that resolved
// twelve bodies and skipped them all both terminate quietly. Each walk here reports how many
// candidates it saw and how many it acted on, and the two numbers are separate because they fail
// differently: zero seen is a broken chain, seen-but-none-acted-on is a filter that is too tight.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

namespace replay {
namespace playback {
namespace residue {

// Why a site did not resolve. B.12's rule, applied one level down: "it is unavailable" is not a
// diagnosis, so every failure names the hop that failed rather than the destination that is missing.
enum class Bind : uint8_t {
    Ok = 0,
    NoGame,             // the CR15Game singleton is null -- no session has started
    NoNetGame,          // CR15Game carries no CR15NetGame
    NoGameSpace,        // the CR15NetGame carries no CncaGameSpace
    NoComponentLookup,  // CS.LOOKUP_BY_SYMBOL failed its 20-byte prologue guard
    NoNetPhysicsCs,     // this gamespace has no R14NetPhysics component system
    NoRemotePlayerCs,   // this gamespace has no R14RemotePlayer component system
    NoPhysicsCs,        // CR15NetPhysicsCS carries no CPhysicsCS
    PoolNotReady,       // a system resolved but its handle map or pool base is still null
    NoPhysSystem,       // the CPhSystem singleton is null -- physics has not started
    NoZoneTable,        // the CPhSystem carries no zone array
    NoClock,            // the CClock singleton is null
    NoOriginBlock,      // the world re-origin global carries no block yet
    NetGameUnreadable,  // the CR15NetGame's CS.SPACE slot is not addressable -- appended, so the
                        // values above keep their numbers
    Count
};

const char* BindText(Bind);

// -------------------------------------------------------------------------------------------
// The component systems N-2, N-3 and N-4 write through.
//
// ★ HELD ONLY FOR AS LONG AS THE SITE'S OWNER IS PROVABLY THE SAME ONE. A component system belongs
// to a gamespace, and the -replayviewer flow tears one down and builds another; a CS pointer that
// outlives its space is a write into freed memory, and src/audio/voip/voipplay.cpp records the same
// hazard for the same reason. This is NOT resolved fresh on every fire -- that cost the table sort
// ARCH-10 S7 forbids (AUD-F3C F3C-1) -- so the thing that makes holding it safe is CsSpaceKey below
// and nothing else. Read BindCsSite's contract before assuming any lifetime for these pointers.
struct CsSite {
    void* netPhysicsCs   = nullptr;   // CR15NetPhysicsCS -- N-3, N-4
    void* remotePlayerCs = nullptr;   // CR15RemotePlayerCS -- N-2
    void* physicsCs      = nullptr;   // CPhysicsCS, for the body chain N-7 and N-8 walk
    Bind  bind = Bind::NoGame;

    bool Ok() const { return bind == Bind::Ok; }
};

// The physics world N-7 and N-8 reach into. `system` is the CPhSystem singleton; the zone array and
// its count are read from it on demand rather than cached, because CPhSystem::AdvanceSimulation
// re-points the global around itself and a cached array base would outlive the object.
struct PhysSite {
    const void* system = nullptr;
    Bind        bind = Bind::NoPhysSystem;

    bool Ok() const { return bind == Bind::Ok; }
};

// Read the engine. The only two functions in this packet that cannot run offline.
CsSite   ResolveCsSite();
PhysSite ResolvePhysSite();

// CncaGameSpace::FindComponentSystem -- CS.LOOKUP_BY_SYMBOL, the one engine call this packet makes.
typedef void* (*FindCsFn)(void* gameSpace, uint64_t symbol);

// EVERYTHING THE ANSWER DEPENDS ON BEFORE THE ENGINE IS CALLED: the session, and the game space it
// carries. Two guarded reads and no lookup, so "could the answer have changed?" is answerable without
// paying CS.LOOKUP_BY_SYMBOL to find out -- that call SORTS the space's system table in place on the
// calling thread when CS.TABLE_FLAGS bit 0 is set (ARCH-10 S7's note to callers).
//
// Deliberately the same shape as W3-C's VoipSpaceKey (src/audio/voip/voipsite.h): one question, one
// shape, in the two packets that ask it. `readable` is carried because a null space and an unmapped
// space slot are different states, and the transition between them is exactly the change that must
// re-ask -- AUD-F3C F3C-6 found that term untested next door, so tests/residue flips it.
// The engine's own bind generation, as a key term. AUD-F3D F3D-2.
//
// ★★ THE POINTER TRIPLE ABOVE CANNOT SEE AN ABA REBUILD. A space torn down and rebuilt at the same
// address -- which a heap allocator handing back the block it just freed makes ordinary, not
// unlikely -- reads identically in {netGame, space, readable}, so the memo would serve the FREED
// image's component systems and N-2/N-3/N-3b/N-4 would write through them. IEntity::BindEpochNow
// (src/engine/entity.h:254) is the term that can: entity_module's SpaceIdentity is
// {netGame, space, entries} where `entries` is FNV-1a over the system table's CONTENTS
// (entity_module.cpp:73, 95-101, 157), and EntityBind advances the counter only when that identity
// differs -- entity_module.cpp:454-457 takes the `g.bound && g.last.id == r.id` branch and does not
// re-bind when they match.
//
// SO NAME THE REAL EDGE, WHICH IS NARROWER THAN "the space was rebuilt" (AUD-F3F F3F-3). The epoch
// moves on a rebuild whose table DIGESTS DIFFERENTLY. A same-address rebuild that reconstructs the
// table identically -- same count, same symbols in the same order, same component-system addresses,
// the ordinary output of an arena reset that hands back the blocks it just freed -- moves nothing,
// and this key does not drop the memo.
//
// For THIS consumer that is benign, and the reason is a property of CsSite rather than of the guard:
// CsSite holds only the three component-system POINTERS, and an identical digest is those same three
// addresses, so what the memo serves is exactly what a fresh lookup would return. The memo is not
// wrong; it is only unable to prove itself right. tests/residue drives that case and asserts the
// held answer equals the re-resolved one. It is NOT benign for a consumer holding anything the old
// instance ISSUED -- a pool index, a registration, a handle (entity.h:231-233) -- which is W3-C's
// problem, not this file's. Launch check F3F-L1 settles whether equal digests actually occur across
// a real rebuild: entity_module.cpp:372 already logs `entries %llx` on every bind, so one session
// crossing a round boundary answers it.
//
// Carried as a state plus a value rather than a bare number, because "this build does not track
// binds" and "nothing is bound yet" are not each other and neither is a generation (entity.h S9.2).
struct BindEpochTerm {
    enum class State : uint8_t { NotTracked = 0, Unbound, Bound };

    State    state = State::NotTracked;
    uint64_t value = 0;               // meaningful only for Bound

    bool operator==(const BindEpochTerm& o) const {
        return state == o.state && value == o.value;
    }
};

// Where CsSpaceKeyOf gets that term. Installed rather than read here: this file has no services
// pointer and IEntity is W1-E's interface, so residue.cpp installs the real source at registration
// and tests/residue installs one it can move. Null source == NotTracked, which is the honest answer
// and leaves the ABA residual exactly where it was before F3D-2.
typedef BindEpochTerm (*BindEpochFn)();
void SetBindEpochSource(BindEpochFn);

struct CsSpaceKey {
    void* netGame = nullptr;
    void* space = nullptr;
    bool  readable = false;   // false: the slot itself was not mapped, so `space` says nothing
    BindEpochTerm bind;

    bool operator==(const CsSpaceKey& o) const {
        return netGame == o.netGame && space == o.space && readable == o.readable && bind == o.bind;
    }
};

CsSpaceKey CsSpaceKeyOf(void* netGame);

// The half of ResolveCsSite that starts at a CR15NetGame: read NETGAME.CS_SPACE_OFF and ask the
// lookup for the three component systems. Split out so a test with no game can drive the offset the
// space is read from -- the part F3 got wrong -- with a synthetic netGame and a recording lookup.
// `find` may be null, which is how ResolveCsSite reports a lookup that failed its prologue guard.
//
// THE INVARIANT, AND IT IS A CORRECTNESS ONE AS MUCH AS A COST ONE:
//
//   THE ENGINE IS ASKED AT MOST ONCE PER SPACE EPOCH -- an epoch being one CsSpaceKey value.
//
// The battery's caller sets Cause::MicroSeek EVERY FRAME above the sub-step ceiling (transport.cpp,
// and residue.cpp says it against itself beside IsJumpOrMicro), and N-3b and N-4 are micro rows, so
// an unmemoised resolve here is two CS.LOOKUP_BY_SYMBOL calls per row per frame for the whole of any
// fast-forward or slow-mo -- the one thing ARCH-10 S7 names as forbidden. P5 is what made it bite:
// before it the lookup was handed a CR15Game and never reached a real table. AUD-F3C F3C-1.
//
// So the answer is memoised on the key and returned without an engine call while the key holds.
//
// ★★★ A MEMO IS ONLY EVER TAKEN ON A DEFINITIVE ANSWER, AND "NOT YET" IS NOT ONE. AUD-F3D F3D-1.
//
// The key is the space's IDENTITY and says nothing about its CONTENTS, so an outcome that describes
// how far the space has got in coming up cannot be seen to change by the key. Freeze one and the
// whole battery refuses for the session: InvalidateCsSite runs on a real Seek/GapCross and on arm,
// and play-forward, fast-forward and slow-mo are all MicroSeek, so nothing ever re-asks. That is
// what `bc41fc7` self-healed by re-resolving per frame and what the memo would otherwise reintroduce
// one enum value over. The three outcomes for the three kinds of answer:
//
//   DEFINITIVE -- held until the key moves. Ok, and the three refusals the key itself carries
//     (NoNetGame, NetGameUnreadable, NoGameSpace): each of those moves a key term the instant it
//     stops being true, so the memo drops itself.
//
//   NOT YET -- NoNetPhysicsCs, NoRemotePlayerCs (the system has not registered into this space),
//     NoPhysicsCs (registered, CPhysicsCS field still null) and PoolNotReady (registered, pool base
//     or handle map still null). Held for at most kCsRetryBinds binds and then re-asked, so the
//     engine cost is one table sort per kCsRetryBinds+1 binds -- O(1/N), never per micro-row -- and
//     liveness is bounded by kCsRetryBinds binds rather than by a seek that may never come. At least
//     one row binds per frame while the battery is armed, so that bound is also a frame bound; more
//     rows per frame make the re-ask land SOONER, never the cost higher, because the cadence counts
//     binds and not frames. It becomes definitive on the first re-ask that finds the space ready.
//
//   NOT MEMOISED AT ALL -- NoComponentLookup. It costs nothing to re-ask: ResolveOn refuses before
//     it ever calls `find`, so no lookup and no table sort happens on that path, and it depends on
//     our own prologue guard and on when the module was mapped rather than on the epoch.
//
// Plus, of course, any outcome dropped by InvalidateCsSite.
//
// EVERY Bind value is sorted into one of those three by CsHoldFor below, and a value nobody sorted
// answers Unclassified rather than falling through to a default -- see there.
//
// WHAT THE CADENCE COSTS, MEASURED RATHER THAN ASSERTED (AUD-F3F m3, its counting reproduced;
// restated in ONE unit with the derivation corrected, AUD-ARCH-14 B7/B8 routed here by ARCH-15 S7
// item 1).
//
// THE UNIT IS LOOKUPS everywhere below -- entries into CS.LOOKUP_BY_SYMBOL. ARCH-10 S7.1's
// reference ceiling of 240/s is 120 binds/s x 2 lookups per resolve, i.e. what the per-row-per-frame
// regime it forbids would cost. It is NOT "4 rows x 60 Hz": same number, invented derivation, in the
// sentence that makes the rule checkable. Entries and lookups are not interchangeable -- one entry
// is at most two lookups -- so a budget that mixes them cannot be compared with the ceiling.
//
// TABLE-SORT-CAPABLE IS COUNTED AS "EVERY ENTRY". The narrow reading -- an entry made while
// CS.TABLE_FLAGS_OFF bit 0 is set -- names engine-side runtime state that no consumer reads, so no
// consumer can count the sort-capable subset of its own calls. Every entry counts as capable here;
// that is the conservative direction, and it is what the pinning test counts.
//
// A STEADY-STATE FRAME is an ordinary play-forward / fast-forward / slow-mo frame: the caller sets
// Cause::MicroSeek, there is no discontinuity, and nothing calls InvalidateCsSite. The re-ask frames
// of a readiness refusal ARE steady-state frames -- while the answer is "not yet", the cadence IS
// the steady state -- so the budget below is stated over the refused path, the expensive one, and
// not over the settled path, which costs zero lookups per second for as long as the epoch lasts.
//
// ResolveCsFor has seven call sites, one per row that needs the CS (residue.cpp:242, 298, 332, 376,
// 448, 479, 500). On a steady-state frame the two micro rows (N-3b, N-4) fire, so R = 2 binds/frame
// = 120 binds/s at 60 Hz. One re-ask per kCsRetryBinds+1 = 61 binds is two engine entries per second,
// each at most two lookups:
//
//     MEASURED BUDGET: <= 4 lookups/s per consumer = 4/240 of the regime ARCH-10 S7 forbids (1.7%).
//
// That number is not prose. tests/residue/test_residue.cpp "one steady-state second of a readiness
// refusal costs the four lookups this header states" drives exactly 120 refused binds and counts the
// calls at the fake-lookup seam against the 4 above, and pins 120 x 2 = 240 in the same unit, so a
// cadence change reds the suite instead of dating this comment. Worst-case recovery is 61/2 = ~31
// frames = ~0.51 s at 60 fps; on a jump frame all seven rows can fire and it lands within ~9 frames.
//
// That is a DELIBERATE DEVIATION from ARCH-10 S7's literal text ("re-resolve only when the space
// pointer changes"): obeying it literally IS the F3D-1 defect, because a readiness refusal is not
// visible in the space pointer. The budget above is what makes the deviation payable, and it is
// recorded here because S7 still reads as binding to anyone who has not read this file.
static constexpr uint32_t kCsRetryBinds = 60;

// The classification itself, exposed so tests/residue can assert it is TOTAL. AUD-F3F F3F-2: a
// mis-sorted value is invisible to a green suite unless something drives that value, and the arm
// list must not be able to fall behind the enum. Unclassified is not a policy -- it is what a Bind
// value nobody wrote a case for answers, and BindCsSiteOn treats it as Never (answer fresh, hold
// nothing) so that a forgotten value costs lookups rather than correctness.
enum class CsHold : uint8_t { Definitive, NotYet, Never, Unclassified };

CsHold CsHoldFor(Bind);

CsSite BindCsSite(void* netGame, FindCsFn find);

// The same, for a caller that has already built the key. ResolveCsSite uses it so that the key is
// computed ONCE per fire instead of once in CsSiteMemoHolds and again in BindCsSite -- which was a
// duplicated Readable() syscall and, worse, two reads that a protection change landing between them
// could make disagree (AUD-F3D F3D-3).
CsSite BindCsSiteOn(const CsSpaceKey& key, FindCsFn find);

// Drop the memo, so the next BindCsSite asks the engine again.
//
// THIS IS THE TEARDOWN GUARD, not a cache-tuning knob. A CComponentSystem belongs to a game space;
// the -replayviewer flow tears one down and builds another, and a CS pointer that outlives its space
// is a write into freed memory. The key covers that on its own now: the +0x2B08 slot moving is one
// term of it and the engine's bind epoch is another, so a rebuild AT THE SAME ADDRESS drops the memo
// too. residue.cpp still calls this on every REAL jump (Seek / GapCross, never MicroSeek) and on arm
// -- belt and braces on the one edge where a stale CS pointer is a write into freed memory, and the
// only thing that covers a build whose IEntity answers NotTracked.
//
// It is NOT the recovery path for a space that is merely not ready yet: that is the retry cadence on
// BindCsSite, because a forward playback produces no jump at all and would never reach this.
void InvalidateCsSite();

// True when BindCsSite would answer from the memo on its VERY NEXT call for this key -- so the build
// guard need not be asked this frame either. ResolveCsSite uses it to skip VerifySite +
// MappedAddress(CS.LOOKUP_BY_SYMBOL), three VirtualQuery calls and a memcmp for an answer the epoch
// already fixed.
//
// It is a pure predicate and it agrees with BindCsSiteOn exactly, INCLUDING on the bind where a
// "not yet" memo comes due: on that bind it answers false, ResolveCsSite therefore hands the engine
// lookup down, and the re-ask has something to ask with. A predicate that only approximated the
// memo would turn every retry into a spurious NoComponentLookup.
bool CsSiteMemoHolds(void* netGame);
bool CsSiteMemoHolds(const CsSpaceKey& key);

// -------------------------------------------------------------------------------------------
// The pure half. Everything below is pointer arithmetic over whatever memory it is handed.

// A CPhTarget as CPhysicsCS::PrimaryBody writes it. Held by value because the engine returns it by
// value, and because a caller holding a POINTER into the component instance would be holding a
// pointer whose validity the generation tag exists to deny.
struct PhTarget {
    uint64_t type = 0;
    uint32_t index = 0;
    uint32_t zoneIndex = 0;
    uint32_t tag = 0;
    uint32_t trailing = 0;

    bool Dead() const;
};

// How many entities the net-physics pool carries, and the handle for slot `index`.
//
// ★ THE REVERSE MAP, NOT A GUESS AT THE HANDLE RANGE. The forward map is indexed BY handle, so
// walking it needs a bound nobody carries; the reverse map is indexed by pool slot and yields the
// handle, and the pool slot count IS carried. W1-E's Pool uses the same pair for the same reason.
size_t   NetCiCount(const CsSite&);
uint16_t NetHandleAt(const CsSite&, size_t poolIndex);

// The 504-byte SR15NetPhysicsCI for a handle, or null if the pool is not addressable.
const void* NetCi(const CsSite&, uint16_t handle);

// The same pair for the remote-player pool N-2 writes through. Separate functions rather than one
// parameterised by a pool, because the two pools' offsets differ in every field and B2's rule holds
// here too: a converting accessor would let a caller address one pool with the other's numbers and
// nothing would complain.
size_t   PlayerCiCount(const CsSite&);
uint16_t PlayerHandleAt(const CsSite&, size_t poolIndex);

// CPhysicsCS::PrimaryBody, reimplemented rather than called.
//
// ★ WHY REIMPLEMENTED. The engine's own entry point is at a known address, but it returns a 24-byte
// aggregate, and the register that carries `this` versus the hidden return slot is decided by the
// ABI rather than by the disassembly -- getting it backwards passes a scratch buffer where the
// function expects a component system. The body is nine instructions of pointer arithmetic and two
// guards; reproducing it costs three offsets, removes the ABI question entirely, and is the only
// version of this that a test with no game can run. W1-I S6.1 set the precedent: a body that is
// fully reproducible from mapped facts does not need its function.
//
// Returns false when the entity has no primary body, which is the honest answer for an entity that
// is not simulated -- distinct from a broken chain, which returns false with `out` untouched too but
// is separated by the site's Bind.
bool PrimaryBodyTarget(const CsSite&, uint16_t physicsHandle, PhTarget& out);

// CPhTarget::ResolveBody, reimplemented. Applies the engine's own three guards -- index in range,
// slot not dead, generation tag matching -- plus the zone-count bound the engine's ResolveZone
// applies and its ResolveBody omits. ResolveBody omits it because it trusts an index it produced;
// this packet did not produce it.
void* ResolveBody(const PhysSite&, const PhTarget&);

// The one traversal N-7 and N-8 share: every body the net-physics pool can reach.
//
// It is deliberately NOT the zone body array. A zone walk reaches every rigid body in the level,
// most of which the replay does not own and never restored; "coexist, never replace" is a statement
// about what we touch, not only about what we overwrite. The pool walk reaches exactly the entities
// the file installs state for, which is exactly the set that can be "restored moving".
struct BodyWalk {
    size_t entities = 0;      // pool slots examined
    size_t bodies = 0;        // slots that resolved to a live CPhBody
    size_t wakeable = 0;      // of those, the ones BeAwake_ would not no-op on (flags & 0xC clear)
    size_t classified = 0;    // of those, the ones CollectClassifiedBodies actually visits
    size_t asleep = 0;        // of the wakeable, the ones parked right now
    size_t acted = 0;         // how many the callback actually changed
};

// `fn` is called once per live wakeable body. It returns true when it changed something, which is
// what `acted` counts -- so a sweep that ran and did nothing is distinguishable from one that ran.
typedef bool (*BodyFn)(void* body, void* ctx);
BodyWalk ForEachRestoredBody(const CsSite&, const PhysSite&, BodyFn fn, void* ctx);

// -------------------------------------------------------------------------------------------
// The three per-body effects, separated from the walk so each is testable on one fabricated body.

// N-7a. Raise PHYS.BODY_MOTION_BIT so the next CPhZone::CollectClassifiedBodies pass takes its wake
// arm instead of its GoToSleep arm.
//
// ★★ THIS IS A ONE-SHOT, AND THAT IS NOT A DEFECT -- IT IS WHY IT WORKS. That pass TESTS the bit at
// 0x14068CC4A and CLEARS it at 0x14068CC92, in that order, once per zone per frame. So the bit is
// not durable state and cannot be "restored"; what a write buys is exactly one classification, which
// is exactly what a discontinuity needs. A body whose keyframe state has it moving is then carried
// by the engine's own integrator, which re-raises the bit for as long as it keeps moving. Setting it
// every frame would instead pin the whole world awake.
bool RaiseMotionBit(void* body);

// N-8a. Clear the two hysteresis latches, honouring the engine's own skip guard.
//
// ★ THE GUARD IS COPIED, NOT INVENTED. CollectClassifiedBodies zeroes these two fields only when
// PHYS.BODY_HYSTERESIS_SKIP_OFF reads zero (cmp at 0x14068CC7A gating the stores at 0x14068CC85 and
// 0x14068CC8C). W1-I S6.1 says ResetHysteresisAllBodies has no PC counterpart but that its behaviour
// is reproducible from mapped facts; this is that reproduction, and dropping the guard would make it
// a different function that happens to touch the same two fields.
bool ClearHysteresis(void* body);

// True when the body is parked. The sleep stamp doubles as the predicate: CPhysicsCS::Awake is
// literally `return *(u64*)(body + 0xC70) == 0`, and both BeAwake_ and GoToSleep branch on it.
bool BodyAsleep(const void* body);

// True when BeAwake_ would do something. False for kinematic and sphere-effect bodies, which its
// own first branch routes away from every wake step. Independent of the body's class: BeAwake_ acts
// on a body directly and does not care which list it is on.
bool BodyWakeable(const void* body);

// True when CPhZone::CollectClassifiedBodies will VISIT this body -- i.e. its class field says it is
// on one of the two lists that pass walks.
//
// ★★★ N-7a IS GATED ON THIS AND NOT ON BodyWakeable, AND THE DIFFERENCE IS A LEAK. The motion bit
// has exactly one reader and two clearers in the whole image, and all three are inside that pass or
// inside the GoToSleep it calls. On a body it never visits, raising the bit sets a flag nothing
// reads and nothing will ever clear -- inert, and permanent until the body is destroyed. A body can
// be neither kinematic nor sphere-effect and still be class 8 (a bit in a SECOND flags word at
// +0xC0) or class 0, so the mask BodyWakeable uses does not answer this question. residue_facts.h
// carries the derivation.
bool BodyClassified(const void* body);


// -------------------------------------------------------------------------------------------
// N-6: the cumulative world re-origin.
//
// WORLD.REORIGIN_FN adds a CUMULATIVE offset into the block at WORLD.REORIGIN_GLOBAL, so replaying
// it twice double-shifts the whole world. It is never called from here, and -- after the correction
// recorded in residue_facts.h -- nothing here writes the flag word either: 0x100 and 0x200 are one
// two-phase state, swapped atomically by `btr 8 / bts 9`, so clearing 0x200 cancels a translation
// that has already BEGUN and leaves no bit that would ever finish it.
//
// ★ SO THIS ROW IS A DETECTOR, NOT A RESET. It latches the cumulative offset and reports a shift
// that happened anyway, which is the observable the row's hazard actually produces. Writing nothing
// is not a shortfall here; it is what the two-phase machine leaves as the only safe action.
struct WorldSite {
    const void* originBlock = nullptr;   // *(void**)WORLD.REORIGIN_GLOBAL
    Bind bind = Bind::NoOriginBlock;

    bool Ok() const { return bind == Bind::Ok; }
};

WorldSite ResolveWorldSite();

// The CUMULATIVE float3 at WORLD_REORIGIN_OFF_X..OFF_Z -- twelve bytes, and deliberately NOT the
// transient triple that follows it at WORLD_REORIGIN_PENDING_OFF_X. The render thread zeroes that
// one on every particle compute pass, so latching it would report a world shift on essentially
// every discontinuity: a false positive on the single observable this row has.
static constexpr size_t kOriginSpanBytes = 12;
bool ReadWorldOrigin(const void* block, unsigned char out[kOriginSpanBytes]);

// -------------------------------------------------------------------------------------------
// N-18: the CClock accumulators.
//
// ★★★ THEY ARE UNSIGNED, AND THAT IS THE WHOLE ROW. CLOCK.ACCUM1_OFF and CLOCK.ACCUM2_OFF are only
// ever reached by `add [rdi+28h],rdx` and `add [rdi+30h],rdx` in the heartbeat. Add a NEGATIVE delta
// -- which is what driving a playhead backwards would produce -- and an accumulator smaller than the
// delta does not go negative, it wraps to about 1.8e19 microseconds. The engine's own reader at
// RESET.STAT_REBASE_A already branches on the top bit, so it converts that to a float and every
// stat, score and UI readout derived from it reads about 5.8e5 centuries.
//
// This is the same defect W2-H found in audio, where an unsigned duration under reverse playback
// turned a 3 ms sound into 4.29e9 ms and made every one-shot re-fire as a loop.
struct ClockSite {
    void* clock = nullptr;
    Bind bind = Bind::NoClock;

    bool Ok() const { return bind == Bind::Ok; }
};

ClockSite ResolveClockSite();

// What a clock guard found. Three outcomes, because they have three different owners.
enum class ClockVerdict : uint8_t {
    Fresh = 0,     // first sight, or a different CClock object -- latched, nothing judged
    Forward,       // all three advanced or held: normal
    Wrapped,       // a value has bit 63 set. Unambiguous: no elapsed time reaches 2^63 us
                   // (~292,000 years), so this is an unsigned underflow and it is REPAIRED
    Backwards,     // a value decreased without wrapping. REPORTED, NOT REPAIRED -- W2-D owns
                   // clock writes, and silently undoing one would hide their defect, not fix it
    Count
};

const char* ClockVerdictText(ClockVerdict);

struct ClockLatch {
    const void* clock = nullptr;
    uint64_t stamp = 0;    // CLOCK.LASTSTAMP_OFF
    uint64_t accum1 = 0;   // CLOCK.ACCUM1_OFF
    uint64_t accum2 = 0;   // CLOCK.ACCUM2_OFF
    bool valid = false;
};

// Compares the live clock against `latch`, repairs an unambiguous wrap, and re-latches. Pure: it
// takes the clock pointer it is given, so a test drives it over a fabricated CClock.
ClockVerdict GuardClockMonotonic(void* clock, ClockLatch& latch);

// -------------------------------------------------------------------------------------------
// The N-3a ordering invariant, checked rather than commented.
//
// ★★★ RESET.CI_PC RECYCLES THE HISTORY RING THAT ApplyStateAt FILLS, so it is only correct BEFORE
// the apply. Today that holds because W2-B's frame loop fires the battery at step 4 and applies at
// step 5 -- adjacent lines in a file this packet does not own, and adjacency is exactly what the
// next refactor destroys.
//
// So it is measured at the one moment that can tell: immediately BEFORE the reset. If the ring's
// newest sample is already at the discontinuity's DESTINATION rather than at its ORIGIN, the apply
// for this jump has already run and the reset is about to delete it. No threshold is involved --
// the test is which of the two known times the sample is nearer, so it scales with the jump.
enum class Ordering : uint8_t {
    NotChecked = 0,   // no jump has happened yet
    Ok,               // the ring still held pre-jump state, as it must
    Violated,         // the ring was already at the destination: the apply ran first
    NoEntity,         // IEntity is null -- W1-E did not resolve, so nothing can be read back
    NoHistoryReadback,// this IEntity does not implement ReadHistoryWindow
    Count
};

const char* OrderingText(Ordering);

}  // namespace residue
}  // namespace playback
}  // namespace replay
