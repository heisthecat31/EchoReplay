#pragma once

// W3-C. Where the VOIP receiver is, which instance inside it belongs to a recorded player, and
// which of VOIP.RECEIVE's own gates would refuse a frame handed to it.
//
// ---------------------------------------------------------------------------------------------
// ★ THE RECEIVER IS A COMPONENT SYSTEM, WHICH IS WHY THIS IS A LOOKUP AND NOT A LATCH.
//
// libr15 keeps the symbol, and it settles the shape:
//
//   CR15NetVoipReceiverCS::ReceiveVoip(SCompactPoolHandle, SR15NetUserVoipEvent const&,
//                                      void const*, unsigned int)
//
// A `...CS` is the SYSTEM object -- one per gamespace -- and per-entity state lives in its pool as
// a `...CI`. So the `this` VOIP.RECEIVE wants is not a per-player component and does not have to be
// latched from a detour, which matters because during a REPLAY no network delivers anything and no
// detour fires. It is reached through CncaGameSpace::FindComponentSystem, the same entry point
// W1-E's E-4 and W2-C already use for five other systems.
//
// ★ AND THE SECOND ARGUMENT IS NOT A "TALKER KEY". It is an SCompactPoolHandle,
// {u16 poolIndex, u16 generation}, and VOIP.RECEIVE selects the instance from its LOW WORD ALONE:
// the generation half is written verbatim into the queued record and into the dedup key and is
// never validated against the pool. A recorded handle replayed into a later session therefore does
// not fail -- it succeeds into whoever now holds that pool slot, and the voice comes out of the
// wrong player's mouth with nothing complaining. HandleOf re-derives one from the LIVE tables every
// time, keyed on the recording's durable user slot; the recorded value is a diagnostic and never a
// route.
//
// ★ THE JOIN KEY IS AN IDENTITY THIS PROJECT ALREADY CARRIES. The u32 at CI+0x20 is an
// SR15NetUserId = {u16 userSlot, u16 generation} -- the encoding ENTITY.OWNER_SYNTHETIC and
// PHYS.CI_OWNER_OFF use, the pair ENTITY.IS_LOCALLY_CONTROLLED validates against the netGame slot
// and generation tables, and the pair PlayerRecord already records. libr15's Connect writes its
// SR15NetUserId argument to exactly that offset at exactly that stride, so the CI layout has two
// derivations in two binaries rather than one reading.
//
// EVERYTHING BELOW THE LOOKUP IS DRIVEN BY RAW POINTERS, deliberately: the walk is the part that
// can be wrong, and a walk that could only run inside a game could only be checked inside a game.
// tests/voip builds a synthetic component system at these offsets and drives the real code.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

namespace replay {
namespace audio {
namespace voip {

// Why a resolution did not produce a receiver or a handle. Every value is a distinct answer: a
// caller that collapsed "there is no session" into "no such talker" would report a missing player
// for a replay that had not started yet.
enum class Resolve : uint8_t {
    Ok = 0,
    NoNetGame,        // ISessionEntry::NetGame() is null -- no session
    NoGameSpace,      // netGame + NETGAME.CS_SPACE_OFF is null: the session has no game space
    GameSpaceUnreadable,  // that field itself is not mapped, so whether there is a space is unknown
    BuildMismatch,    // a prologue did not match; no engine call was made
    NoVoipCs,         // FindComponentSystem returned null for VOIP.RECEIVER_CS_SYMBOL
    UnreadableCs,     // the system resolved and one of its own fields is not mapped
    NoInstances,      // the system holds zero receiver instances
    SlotOutOfRange,   // userSlot >= ENTITY.USER_SLOT_TABLE_MAX, so the netGame tables do not cover it
    NoGeneration,     // the generation table holds 0xFFFF for this slot: nobody is in it
    NoSuchTalker,     // no live instance carries this SR15NetUserId
    TableDisagree,    // the dense->pool and pool->dense maps do not agree; this is not our layout
};

const char* ResolveText(Resolve);

// Which of VOIP.RECEIVE's own gates would stop a frame. Gates 1-3 are reads and are checked here;
// gate 4 is VOIP.MUTE_QUERY, an engine call this code deliberately does NOT make -- see Open.
enum class Gate : uint8_t {
    // Gates 1-3 are open. Gate 4 is the engine's own mute decision and is left to it: a muted
    // talker being silent is the correct outcome, not a failure, and speculatively calling an
    // engine predicate to find out would buy a label at the price of a call into somebody's
    // running game.
    Open = 0,
    NoProvider,       // gate 1: the provider global is null and VOIP.RECEIVE returns at once
    NoNetGameSub,     // gate 2: *(netGame + 0x20) is null
    NotConnected,     // gate 3: the CI's decoder state is set and a talker dword is still unset
    Unreadable,       // a gate's own memory is not mapped, so the answer is unknown, not "open"
};

const char* GateText(Gate);

// The receiver component system. Held rather than re-looked-up per frame, and keyed on the
// netGame it was resolved against so a new session cannot reuse a stale one.
struct VoipSite {
    void* cs = nullptr;
    void* netGame = nullptr;

    bool Valid() const { return cs != nullptr && netGame != nullptr; }
};

// ★ EVERYTHING A RESOLUTION'S ANSWER DEPENDS ON BEFORE THE ENGINE IS CALLED: the session, and the
// game space it carries. Two guarded reads and no lookup, so a caller can ask "could the answer
// have changed since it refused?" without paying CS.LOOKUP_BY_SYMBOL to find out -- which is the
// whole point, because that call SORTS the gamespace's system table in place on the calling thread
// when CS.TABLE_FLAGS bit 0 is set (ARCH-10 S7's note to callers).
//
// `readable` is carried because a null space and an unmapped space field are different states, and
// the transition between them is exactly the kind of change that makes a retry worthwhile.
//
// Deliberately the same shape as W3-A's CsSpaceKey (src/playback/residue/residuesite.h): one
// question, one shape, in the two packets that ask it. The term is redeclared here rather than
// included from there because that header is W3-A's, and an audio file reaching into the residue
// battery for a four-byte struct would be a dependency far larger than the duplication.

// The engine's own bind generation, as a key term. AUD-F3D F3D-2.
//
// THE POINTER TRIPLE ABOVE CANNOT SEE AN ABA REBUILD. A game space torn down and rebuilt at the
// same address under the same netGame -- which a heap allocator handing back the block it just
// freed makes ordinary rather than unlikely -- reads identically in {netGame, space, readable}, so
// the held VoipSite would keep serving the FREED image's R14NetVoipReceiver and every replayed
// voice frame would be Connect'd and injected through a component system that no longer exists.
//
// ★ THE EDGE IT ACTUALLY MOVES ON, corrected (AUD-F3F F3F-3 -- this used to read "moves on exactly
// that edge and no other", which is wrong in both directions). IEntity::BindEpochNow
// (src/engine/entity.h:254) advances when entity_module's SpaceIdentity differs, and SpaceIdentity
// is {netGame, space, entries} where `entries` is an FNV-1a digest of the component-system table's
// CONTENTS (SpaceIdentity, entity_module.cpp:79-102). The bind itself happens in
// entity_module::Tick, which takes the `g.last.id == r.id` branch and does NOT re-bind when they
// match (entity_module.cpp:458) -- so the epoch is a snapshot a tick maintains, and a path with
// no tick in it reads whatever the last tick left. So:
//
//   MORE than a rebuild -- any change to the table's bytes moves it, a system registering included,
//     which costs a re-resolve and nothing else; and
//   LESS than a rebuild -- a space torn down and rebuilt at the SAME address whose table
//     reconstructs identically (same count, same symbols in the same order, same component-system
//     addresses) digests the same and moves NOTHING. That is the ordinary output of an arena reset
//     that hands back the blocks it just freed, which is the allocator behaviour the paragraph
//     above invokes to argue the ABA case is common in the first place. The retry that re-Inits the
//     SAME space until its pools come up does not move it either, and that one is intended.
//
// The epoch is therefore blind in exactly one direction, and `tableBase` below is the term that is
// not.
//
// Carried as a state plus a value rather than a bare number, because "this build does not track
// binds" and "nothing is bound yet" are not each other and neither is a generation (entity.h S9.2).
//
// ★ WHEN THE SOURCE IS UNAVAILABLE the state stays NotTracked and every key carries that same
// value, so this term contributes nothing and the ABA residual sits exactly where it did before:
// covered only by OnDiscontinuity(), i.e. by a real seek or a gap cross. That is a genuine hole on
// a build with no IEntity and it is named here rather than papered over -- forward playback across
// a same-address rebuild produces no discontinuity at all.
struct BindEpochTerm {
    enum class State : uint8_t { NotTracked = 0, Unbound, Bound };

    State    state = State::NotTracked;
    uint64_t value = 0;               // meaningful only for Bound

    bool operator==(const BindEpochTerm& o) const {
        return state == o.state && value == o.value;
    }
};

// Where VoipSpaceKeyOf gets that term. Installed rather than read here: this file has no services
// pointer and IEntity is W1-E's interface, so voip_module.cpp installs the real source at init and
// tests install one they can move. A null source means NotTracked, which is the honest answer.
typedef BindEpochTerm (*BindEpochFn)();
void SetBindEpochSource(BindEpochFn);

struct VoipSpaceKey {
    void* netGame = nullptr;
    void* space = nullptr;
    bool readable = false;   // false: the field itself was not mapped, so `space` says nothing
    BindEpochTerm bind;

    // ★ THE SECOND DISCRIMINATOR, for the one rebuild the epoch above cannot see (AUD-F3F F3F-3).
    // The component-system table's block header sits INLINE at space + CS.TABLE_OFF and its first
    // qword is the element array's base -- CS_TABLE_COUNT_OFF (0x450) and CS_TABLE_FLAGS_OFF
    // (0x458) are that same header's +0x30 and +0x38 (bootstrap/tableguard.h:60-67). A game space
    // torn down and rebuilt reconstructs that block through the allocator, so the base moves even
    // where the entries digest does not.
    //
    // ★ THE ENGINE KEEPS NO GENERATION TO ASK INSTEAD. The block header is {base, bytes, allocator,
    // grow, capacity, count, flags} (tableguard.h:73-81) and none of those is a counter; entity.cpp
    // holds no per-space one either. The base pointer is the best available term, not a perfect
    // one: an allocator that hands back the very same block leaves it equal, and that residual is
    // the deferred launch check AUD-F3F F3F-L1 asks for -- two `entries %llx` values from
    // entity_module's own bind line across a round boundary.
    //
    // ★ IT ALSO MOVES WHEN THE TABLE MERELY GROWS under the same space -- an append that finds the
    // block full reallocates (tableguard.h:63). That is a false positive, and it is chosen
    // deliberately: SpaceReplaced in voipplay.cpp states what each of the two mistakes costs.
    uint64_t tableBase = 0;
    bool tableRead = false;   // false: `space` was null, or the header was not mapped

    bool operator==(const VoipSpaceKey& o) const {
        return netGame == o.netGame && space == o.space && readable == o.readable &&
               bind == o.bind && tableRead == o.tableRead && tableBase == o.tableBase;
    }
};

VoipSpaceKey VoipSpaceKeyOf(void* netGame);

// ★ THE MEMORY-SAFETY INVARIANT OF THIS PACKET (AUD-F3K F3K-1, AUD-F3L F3L-1). NO WRITE EVER GOES
// THROUGH A CACHED RECEIVER POINTER, OR A CACHED POOL INDEX, WITHOUT RE-ESTABLISHING BOTH AGAINST
// THE LIVE TABLES FIRST. Before UnbindTalker -- the only write we make into a system somebody else
// owns -- the caller re-runs the symbol lookup on the LIVE space through this function and proceeds
// ONLY if the engine answers the identical pointer we bound. A different pointer, a null one, a
// space that cannot be read, a table that is NotReady: every one of those ABANDONS. SortPending is
// the one that DEFERS instead (AUD-F3L F3L-2): the claim is parked and retried, and abandoned only
// once its eight attempts run out. Neither disposition ever writes, and that half of the sentence
// is the one the safety argument rests on.
//
// ★ AND THAT IS ONLY HALF THE ADDRESS. The receiver says WHICH system; `poolIndex` says which
// INSTANCE inside it, and the two churn independently -- a player leaving and another joining
// reuses a pool slot under a receiver that never moved. UnbindTalker therefore re-establishes the
// second half itself, against the SR15NetUserId recorded at bind. Neither half is sufficient and
// the invariant is not met by either alone.
//
// ★ WHY THE KEY ABOVE IS NOT ENOUGH. VoipSpaceKey compares {netGame, space, bind, tableBase}, and
// every one of those is a PROXY for identity. entity.h:243 enumerates three ways a space is
// replaced -- a new netGame, a new space, or the same block carrying a rebuilt system table -- and
// the third moves none of the four: the netGame is unmoved, the space pointer is unmoved, the block
// base is unmoved, and the bind epoch is a snapshot a tick maintains, on a path with no tick in it.
// A receiver rebuilt in place therefore reads IDENTICAL in the key, and Disconnect would go through
// the freed image. The key stays as the cheap early-out; this is the term that cannot be fooled,
// because it asks the engine instead of asking a snapshot, and the engine has no generation counter
// to ask instead.
//
// ★ WHAT IT COSTS, AND ON WHICH THREAD (AUD-F3L F3L-3). One CS.LOOKUP_BY_SYMBOL per teardown that
// re-asks. A rate alone is not the cost model: a lookup sorts an engine table in place, so WHICH
// THREAD makes it decides whether it is legal at all. The three callers are not alike:
//
//   * SetVoipDemoPath(nullptr) and Load -- reached from the panel draw (panel.cpp:1435,1476,1533),
//     which csregister.cpp:220 marshals to the owner thread and REFUSES off it. Owner thread ==
//     tick thread (ui_module.cpp:35). The re-ask is legal here, and a next tick exists, which is
//     what makes the deferred retry below possible.
//   * VoipShutdown -- reached ONLY through modreg::ShutdownAll(), which has had no shipping caller
//     since ENG-EXIT-FIX; it is kept to the rule of the place it used to run, the LOADER LOCK at
//     DLL_PROCESS_DETACH with every other thread already killed wherever it stood (console.cpp
//     draws the same conclusion independently). An engine call there is illegal, and the release
//     is pointless -- nothing will
//     read that pool again. That caller makes NO engine call: no LiveNetGame, no lookup, no
//     Disconnect. See VoipPlayer::UnloadAtTermination.
//
// So the rate is <= 1 lookup per teardown on the tick thread and ZERO on the terminating one:
// ARCH-10 S7's budget as amended by ARCH-14 (<= 4 lookups/s/consumer) is not threatened, and the
// frame path is untouched and still resolves once per session.
//
// The table is judged BEFORE the engine is entered, never after: CS.LOOKUP_BY_SYMBOL sorts the block
// in place while flags bit 0 is set (ARCH-10 S7), which is an engine write on our thread, and a
// teardown is exactly when that block may be mid-rebuild.
//
// ★ `outRetryable` SEPARATES "COME BACK IN A MOMENT" FROM "IT IS GONE" (AUD-F3L F3L-2). Exactly one
// of the refusals above is transient by construction: SortPending is flags bit 0, set because a
// component system registered since the last lookup, and cleared by the next lookup anyone makes
// (tableguard.h:118). Every other refusal here says the receiver or the space is not what we bound.
// Set true ONLY for the SortPending arm; a caller that treats the rest as retryable would spin.
bool SameReceiverStillLive(void* liveNetGame, const VoipSite& bound, bool* outRetryable = nullptr);

// The only engine call in this file: guarded, rebased, and refused rather than assumed.
bool ResolveVoipSite(void* netGame, VoipSite* out, Resolve* why);

// ★ Substitutes the ONE thing an offline test cannot have -- CncaGameSpace::FindComponentSystem --
// and nothing else. ResolveVoipSite still reads the game space out of the netGame at
// NETGAME.CS_SPACE_OFF, still refuses a null one and an unreadable one separately, still reads the
// four component-system fields and still refuses a system whose tables do not come back. A seam that handed over a finished VoipSite would leave every one of those
// untested, which is the difference between a substitution and a bypass.
using ComponentLookupFn = void*(__fastcall*)(void* gameSpace, uint64_t symbol);
void SetComponentLookupForTest(ComponentLookupFn fn);

// The SR15NetUserId a recorded user slot has in THIS session. The generation half comes from the
// live table, never from the file: a recording's generation belongs to the recording's session.
bool UserIdOf(void* netGame, uint16_t userSlot, uint32_t* outUserId, Resolve* why);

// Convenience for the two halves of an SR15NetUserId, so no caller open-codes the packing.
inline uint16_t SlotOfUserId(uint32_t userId) { return static_cast<uint16_t>(userId & 0xFFFFu); }
inline uint16_t GenOfUserId(uint32_t userId) { return static_cast<uint16_t>(userId >> 16); }
inline uint32_t MakeUserId(uint16_t slot, uint16_t gen) {
    return static_cast<uint32_t>(slot) | (static_cast<uint32_t>(gen) << 16);
}

// The walk: SR15NetUserId -> the SCompactPoolHandle VOIP.RECEIVE takes, and the CI it selects.
//
// ★ IT ROUND-TRIPS, AND THAT IS THE CHECK. The dense->pool map at CS+0xE0 and the pool->dense map
// at CS+0xD0 are two independent arrays. The walk goes forward through one and verifies the other
// leads back to the dense index it started from. A layout that is not ours fails that comparison
// instead of producing a plausible handle -- which is the whole difference between refusing and
// injecting a stranger's voice into a slot that happens to be occupied.
bool HandleOf(const VoipSite& site, uint32_t userId, uint32_t* outHandle, const uint8_t** outCi,
              Resolve* why);

// ---- binding a recorded speaker to a live receiver instance ----------------------------------
//
// ★ THE REPLAY DRIVES THE ENGINE'S OWN Connect. It is what a live session uses to bind a pool slot
// to a user and create that talker's decoder, and a decoder built here would be a second
// implementation of something the engine already does correctly. Its only outward call --
// CNSProvider::VoipCreateDecoder -- sits behind exactly the two gates VOIP.RECEIVE checks, so in
// the only session where it does anything, the engine is already running its own VoIP and we are
// adding a decoder for a user rather than opening anything. See docs/spikes/W3-C.md S7.
//
// ★ AND Connect DOES NOT ALLOCATE. It takes a pool index that must already exist, so this binds a
// receiver component that is there. NoFreeInstance is a real and expected answer.
enum class Bind : uint8_t {
    AlreadyBound = 0,   // a live instance already carries this user id -- the SESSION owns it
    Bound,              // we drove VOIP.CONNECT on a free instance, and we now own it
    NoSite,
    NoFreeInstance,     // every instance is spoken for; nothing was touched
    BuildMismatch,      // VOIP.CONNECT does not hold its recorded prologue; no call was made
    Unreadable,
    NoSentinel,         // the no-user id could not be read, so "free" could not be established
};

const char* BindText(Bind);

// ★ AlreadyBound AND Bound ARE KEPT APART BECAUSE TEARDOWN DEPENDS ON IT. A slot the session
// already owned must never be disconnected by us -- that would poison the pool for live play,
// which is worse than a replay with no voice. Only `Bound` is ours to release.
Bind BindTalker(const VoipSite&, uint32_t userId, uint16_t* outPoolIndex);

// ★ WHAT THE RELEASE ANSWERED, NOT WHETHER IT WORKED (AUD-F3L F3L-1). `PoolMoved` is the arm that
// makes this an enum: it is not a failure, it is the guard REFUSING to write, and folding it into
// the same `false` as "the read-back did not confirm" would hide the one disposition that says a
// live stranger was spared. A caller counts them apart or it cannot tell a leak from a rescue.
enum class Release : uint8_t {
    Released = 0,   // Disconnect made, and the slot read back free
    PoolMoved,      // the slot no longer carries the id we bound -- NO write was made
    Unreadable,     // could not verify before the write, or could not confirm after it
    NoSite,
    BuildMismatch,
};
const char* ReleaseText(Release);

// Releases a slot BindTalker returned `Bound` for. Never call it on one it called AlreadyBound.
//
// ★ THE POOL INDEX IS RE-ESTABLISHED TOO, AND BEFORE THE WRITE (AUD-F3L F3L-1). `poolIndex` was
// issued at bind time and the engine never validates one against the pool -- see the top of this
// file: a stale index "succeeds into whoever now holds that pool slot". A receiver can survive a
// churn of its own pool intact, so SameReceiverStillLive answering YES says nothing about the slot.
// So the read this function already made AFTER Disconnect -- poolIndex -> dense -> the CI's
// SR15NetUserId -- is made BEFORE it as well, against the id we recorded at bind, and a slot that
// no longer carries our talker is refused rather than disconnected. It does not make ABA
// impossible: a reborn receiver could coincidentally hold the same user at that index. It collapses
// the write from "whatever talker is there now" to "the identity we actually bound".
Release UnbindTalker(const VoipSite&, uint16_t poolIndex, uint32_t ownedUserId);

// Substitutes the two engine calls and the sentinel read -- the three things an offline test cannot
// have -- and nothing else. The scan, the free-slot rule, the round-trip check and the
// already-bound/bound distinction all still run.
using ConnectFn = void(__fastcall*)(void* cs, uint16_t poolIndex, uint32_t userId);
using DisconnectFn = void(__fastcall*)(void* cs, uint16_t poolIndex);
void SetBindCallsForTest(ConnectFn, DisconnectFn, const uint32_t* sentinel);

// `provider` is the qword AT VOIP.PROVIDER_GATE_VA, already dereferenced by the caller, so this
// stays free of any address of its own and a test can drive both arms.
Gate InspectGates(const VoipSite& site, const uint8_t* ci, const void* provider);

// The live instance count, for reporting. False when the system's own fields are not mapped --
// which is a different answer from zero instances and must not be folded into one.
bool InstanceCount(const VoipSite& site, uint16_t* out);

// Reads one instance by dense index, for the diagnostic dump the parked read asks for. Every field
// is a number; there is no boolean summary, because "not connected" and "could not be read" are
// different findings.
struct VoipInstanceRow {
    uint16_t dense;
    uint16_t poolIndex;
    uint16_t generation;
    uint32_t userId;
    uint32_t talkerA;
    uint32_t talkerB;
    uint32_t decoderState;
    bool readable;
};
bool ReadInstance(const VoipSite& site, uint16_t dense, VoipInstanceRow* out);

}  // namespace voip
}  // namespace audio
}  // namespace replay
