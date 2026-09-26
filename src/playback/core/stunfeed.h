#pragma once
// THE PLAYBACK SIDE OF A STUN.
//
// The owner's words: "I hear people shielding, but dont hear or see stuns."  Both halves have one
// cause.  A block travels in the pose stream's action word, which playback writes every tick, so
// the engine's own block delegates fire and he hears it.  A stun does not travel in the pose stream
// at all -- there is no stun bit anywhere in the recorded 28-bit action word, and no netvar moves
// at a stun (RE-STUNS.md S1.6/S1.7).  It travels as its own message, SR15StunPlayerMsg, which the
// recorder has been storing VERBATIM all along (tap 13, eight rows in the owner's 4v4 capture with
// victim, stunner, duration and a stamp) and which nothing on the playback side ever decoded.
//
// So this file is a replay, not a synthesis: it hands the engine back bytes the file already holds,
// at times the file already carries.  It draws nothing and plays nothing.  delegate_OnStun is the
// only trigger of the remote/spectator stun-FX script, and that script is what the owner is
// missing.
//
// WHY NOT THE RECEIVE HANDLER ITSELF.  CR15NetPunchableCS::StunPlayerCB is a pure function of
// (this, msg) and would work.  Its TAIL, though, is role-gated at netGame+0x2DA0 and does the
// announcer event, the STAT BUMP and a relay -- and playback already replays the recorded stat rows,
// so a second increment would put a stun count on the HUD that the match never had.  The handler's
// HEAD is three stores and two delegate raises; this file performs exactly those, in the engine's
// own order and with the engine's own masks, and never enters the tail.  RE-STUNS.md S3(b).
//
// EDGE AND LEVEL ARE TWO DIFFERENT WRITES, AND THAT SEPARATION IS THE WHOLE SCRUB STORY.
//   * the EDGE is delegate_OnHit followed by delegate_OnStun -- the sound, the FX, the limp.  It
//     fires only when the playhead crosses a recorded row moving FORWARD under its own power.
//   * the LEVEL is the three fields on the victim's punchable record.  It is a pure function of the
//     playhead: inside a recorded span the victim is stunned with the elapsed time the recording
//     implies, outside one he is not.  A seek, a load and a backward scrub restore the level and
//     NEVER raise, because a stun sound at a seek destination is precisely what the owner would
//     read as a bug.
// One entry edge buys the whole lifecycle: CR15NetPunchableCS::Update advances the elapsed field,
// ends the stun, fires delegate_OnReboot and runs the invulnerability window by itself.  Nothing has
// to be replayed to END a stun -- SR15RemoveStun was never sent in this session.
//
// THE DOUBLE STUN IS NOT A BUG TO COALESCE.  Two rows 41 ms apart on one victim are two stuns: the
// receive handler has no "already stunned" test and no invulnerability test, so on a real server the
// second one re-zeroes the timer and raises both delegates again.  That is what the owner heard
// live, so that is what this feed does.
//
// TIME AXIS.  Same as the goal feed: an S_EVENT row's stampUs is the tap's ARRIVAL axis and is not
// comparable with the playhead, so rows are placed inside their chapter's [tBegin, tEnd) by where
// their arrival stamp falls between the first and last arrival stamp of that chapter's events.  On
// the owner's file that interpolation is bounded by about 20 ms -- roughly one presented frame --
// because each stun-bearing chapter carries ~440 event rows across 4.0 s.

#include <stddef.h>
#include <stdint.h>

#include "foundation/demofile/demofile.h"

namespace replay {
namespace playback {
namespace stunfeed {

inline constexpr uint32_t kBodyBytes = 16;
// A match cannot produce anything near this many stuns; the cap exists so a corrupt tap index
// cannot turn the store into the payload.
inline constexpr uint32_t kMaxRows = 256;
// SR15NetUserId slots are < 16, so a file cannot name more victims than this.
inline constexpr uint32_t kMaxVictims = 16;

struct ScanCensus {
    uint32_t event_rows = 0;    // S_EVENT rows walked
    uint32_t tap_chapters = 0;  // chapters whose keyframe carried a tap table naming this message
    uint32_t rows = 0;          // stun rows stored
    uint32_t refused_rows = 0;  // the right tap, refused by the decoder (short body, bad duration)
    uint32_t dropped_rows = 0;  // over the row cap
    uint32_t placed_rows = 0;   // rows the arrival interpolation moved off the chapter begin
    uint16_t tap = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    const char* first_drop = nullptr;  // program-lifetime; names the bound that was hit
};

// One decoded stun, kept alongside its verbatim bytes.  The fields are for the log, the level and
// the tests; nothing recorded is handed to the engine unrewritten -- both ids belong to the
// recording's lobby and are remapped before anything is resolved off them.
struct Stun {
    uint64_t t_us = 0;
    uint32_t victim = 0;   // recorded SR15NetUserId
    uint32_t puncher = 0;  // recorded SR15NetUserId
    float duration = 0.0f;
    bool blocked = false;  // body+12: the punch was blocked and the PUNCHER is the one stunned
    uint8_t body[kBodyBytes] = {};
};

class RecordedStuns {
public:
    void Reset();

    // `arrivalUs` is the row's S_EVENT stamp; EndChapter turns it into a playhead-axis time.
    bool Offer(uint64_t arrivalUs, const void* body, uint32_t len, ScanCensus& census);

    void BeginChapter();
    // `firstArrivalUs`/`lastArrivalUs` bound the arrival stamps of EVERY event row in the chapter,
    // not just the stun rows -- that is what makes the interpolation tight.  A chapter that cannot
    // separate them leaves its rows at the chapter midpoint.
    void EndChapter(uint64_t tBeginUs, uint64_t tEndUs, uint64_t firstArrivalUs,
                    uint64_t lastArrivalUs, ScanCensus& census);

    uint32_t Count() const { return count_; }
    const Stun& At(uint32_t i) const { return rows_[i < count_ ? i : 0]; }

    // The first row strictly after `tUs`, as an index; Count() when there is none.
    uint32_t FirstAfter(int64_t tUs) const;
    // The newest row for `recSlot` at or before `tUs`; -1 when this file has none there.  This is
    // the level's whole answer: a later row on the same victim re-zeroed the timer, so the NEWEST
    // one is the stun he would be serving.
    int32_t LastForVictimAtOrBefore(uint16_t recSlot, int64_t tUs) const;

    // The distinct recorded victim slots, in first-seen order.  The level restore walks these and
    // nothing else, so its cost is the number of players a file ever stunned.
    uint32_t VictimCount() const { return victim_count_; }
    uint16_t VictimAt(uint32_t i) const { return victims_[i < victim_count_ ? i : 0]; }

private:
    Stun rows_[kMaxRows];
    uint32_t count_ = 0;
    uint32_t chapter_first_ = 0;
    uint16_t victims_[kMaxVictims] = {};
    uint32_t victim_count_ = 0;
};

// Walks every chapter's S_EVENT and fills `into` from the stun tap, resolving the type index out of
// the FILE'S OWN keyframe tap table -- never a compiled-in index.  The tap table is self-describing
// and reordering it would make a goal read as a stun, which is why every consumer in this tree
// resolves by name.  Returns the rows stored.
uint32_t ScanRecordedStuns(IDemoReader& reader, RecordedStuns& into, ScanCensus& census);

RecordedStuns& PlaybackRecordedStuns();
ScanCensus& PlaybackStunCensus();

// ---------------------------------------------------------------------------------------------
// The feed.  Everything below reaches the engine.
// ---------------------------------------------------------------------------------------------

// The two delegate wrappers, (CR15NetPunchableCS*, SComponentID).
using DelegateFn = void (*)(void* punchableCs, uint32_t componentId);
// CncaGameSpace::FindComponentSystem(space, CSymbol64) -> CComponentSystem*.
using CsLookupFn = void* (*)(void* gameSpace, uint64_t symbol);
// CComponentSystem::FirstActorComponent(cs, out, actorLo, actorHi).  Writes an SComponentID --
// component handle in the low word, generation in the high -- into the first dword of `out`.
using FindComponentFn = void (*)(void* cs, void* out, uint64_t actorLo, uint64_t actorHi);
// The deferred named-event raise, addressed at an actor: sub_140510060(space, CSymbol64, guid0,
// guid1, deferred).  The engine's own action-word differ raises every per-player delegate this way.
using RaiseEventFn = void (*)(void* gameSpace, uint64_t symbol, uint64_t actorLo, uint64_t actorHi,
                              uint32_t deferred);

struct Handlers {
    DelegateFn on_hit = nullptr;
    DelegateFn on_stun = nullptr;
    CsLookupFn lookup = nullptr;
    FindComponentFn find = nullptr;
    // The punchable component's CSymbol64, READ out of the image's radhash string table rather than
    // transcribed.  Zero means the anchor did not verify and no lookup will be attempted.
    uint64_t symbol = 0;
    // The fallback for a victim whose actor carries no punchable: the deferred named-event raise the
    // two wrappers above end in, addressed at the victim's own actor GUID.  All three are zero
    // unless every one of them verified, so the fallback is either whole or absent.
    RaiseEventFn raise = nullptr;
    uint64_t on_hit_symbol = 0;
    uint64_t on_stun_symbol = 0;
};
// Test seam: fakes that record what they were handed.  Null restores the bind against the mapped
// image, which on a machine with no game resolves to nothing and refuses by name.
void InjectHandlersForTest(const Handlers* h);

// A recorded user, as this session's roster resolves him.  The actor GUID is the engine's own
// 16-byte SActorGUID for the live entrant; it is what turns a user into a component, and a recorded
// one would be meaningless here.  `net_user_id` is for the log and for nothing else.
struct LiveUser {
    uint32_t net_user_id = 0;
    uint64_t actor_lo = 0;
    uint64_t actor_hi = 0;
};
// Supplied by the caller because the roster mapping and the engine's user table both live above
// this file; false means "this recorded user is not on the field", which is a refusal and never a
// guessed slot.
using MapUserFn = bool (*)(void* ctx, uint16_t recSlot, LiveUser& out);

struct ApplyCensus {
    uint32_t fires = 0;             // forward crossings that raised both delegates
    uint32_t level_writes = 0;      // punchable records written by a seek/load/reverse restore
    uint32_t level_clears = 0;      // of those, the ones that ended a stun the playhead left
    uint32_t seeks_suppressed = 0;  // jumps that consumed a crossing without firing
    uint32_t holds = 0;             // ticks skipped because the pre-roll owns the clock
    uint32_t reverse_levels = 0;    // level passes run because the playhead moved backwards
    uint32_t unmapped_victim = 0;   // the recorded victim is not on this session's field
    uint32_t unmapped_puncher = 0;  // log only: no delegate carries the stunner
    uint32_t refused_cs = 0;        // the punchable component system did not resolve
    uint32_t refused_handle = 0;    // no punchable component on that actor
    uint32_t actor_raises = 0;      // of those, the ones the actor-addressed delegate raise covered
    uint32_t refused_record = 0;    // the pool element did not read back
    uint32_t faulted = 0;
    const char* refusal = nullptr;  // program-lifetime; why the last sweep did nothing
};

void Arm(bool on);
bool Armed();
// Reporting only: the fire lines speak the playhead's axis rather than the file's absolute one.
void SetPlayheadOriginUs(int64_t originUs);

// A discontinuity the user caused.  The next sweep re-bases onto the new playhead and restores the
// level WITHOUT raising, so a forward seek across a stun is silent and a seek INTO one shows the
// victim already down for whatever the recording says is left of it.
void NoteSeek();

// Forgets what was fired and where the playhead was, without touching the engine.
void Forget();

// One stage-3 tail pass, on the engine tick thread.  `tUs` is the presented playhead, absolute.
// `hold` is true while the pre-roll owns the clock: the cursor is re-based and nothing fires.
//
// COST.  A tick that crosses nothing and is not a discontinuity does one comparison against the
// next row's stamp and returns; there is no per-player work in the steady state.  The level pass --
// one binary search and at most one small write per victim the FILE ever stunned -- runs only on a
// discontinuity or while the transport is moving backwards.
void Sweep(void* netGame, int64_t tUs, bool hold, MapUserFn map, void* mapCtx);

const ApplyCensus& Census();
void ResetCensusForTest();

}  // namespace stunfeed
}  // namespace playback
}  // namespace replay
