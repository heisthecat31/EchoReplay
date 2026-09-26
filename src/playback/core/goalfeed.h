#pragma once
// THE PLAYBACK SIDE OF A GOAL.
//
// The recorder already stored SR15NetReliablePostScoreUpdate verbatim in S_EVENT.  Nothing decoded
// it, so a replayed goal produced no banner, no celebration graph and no announcer -- the
// scoreboard numbers moved (they are netvars, and the netvar replay already owns them) and nothing
// else happened.  This file decodes those 28-byte rows and hands them back to the ENGINE'S OWN two
// receive handlers.  It draws nothing, announces nothing and synthesises no field.
//
// WHY A DIRECT CALL AND NOT A BROADCAST.  The engine registers a different set of listeners on an
// authority than on a client, and CR15NetGameplay::OnPostScoreStats is registered in the client arm
// only.  An offline replay host is the authority, so a synthesized broadcast would run half the
// path: the /session cache would fill, the banner would appear, and its goal-type text would read
// "[NO GOAL]" because the expression node behind it reads the gameplay field only the unregistered
// handler writes.  Calling both handlers directly is what closes that, and it is also what makes
// the feed contained, guardable and reversible.
//
// EDGE AND LEVEL ARE TWO DIFFERENT CALLS, AND THAT SEPARATION IS THE WHOLE SCRUB STORY.
//   * the FILL (CR15NetGameplay::OnPostScoreStats) is eight stores into the gameplay object.  It is
//     a LEVEL: the banner's expression nodes read those fields whenever a widget evaluates, so the
//     correct value after a seek is simply the newest recorded row at or before the playhead.
//   * the RECEIVE (CR15NetGame::ReliablePostScoreUpdateCB) fills the /session cache and then raises
//     nevt_post_score_update_received, which is what STARTS the banner.  It is an EDGE, and it
//     fires only when the playhead crosses a row moving FORWARD under its own power.
// A seek therefore restores the fill and never the receive: the score itself comes from the netvar
// level, so the edge is safely skippable, and a banner appearing at a seek destination is exactly
// what the owner would notice as wrong.
//
// THE ONE FIELD THAT IS NOT VERBATIM.  body+6 and body+10 are SR15NetUserIds -- slot in the low
// half, generation in the high half -- and both handlers resolve them against the LIVE slot table.
// The recorded halves belong to the recording's lobby, so they are rewritten to the live
// slot|generation of the mapped emulated player.  A recorded user that maps to nothing is written
// as the engine's own "nobody" and the engine's own placeholder name is what appears.  The other
// twenty bytes are the recording's, byte for byte.
//
// TIME AXIS.  An S_EVENT row's stampUs is the tap's ARRIVAL axis and is not comparable with the
// playhead; what the file does say is which chapter a row was written into.  Rows are therefore
// placed inside their chapter's [tBegin, tEnd) by where their arrival stamp falls between the
// first and last arrival stamp of that chapter's events -- an interpolation, and the load line says
// so, but a far tighter one than an even spread because a chapter carries tens of event rows.  The
// resulting stamps are on the same absolute axis as the presented playhead.

#include <stddef.h>
#include <stdint.h>

#include "foundation/demofile/demofile.h"

namespace replay {
namespace playback {
namespace goalfeed {

inline constexpr uint32_t kBodyBytes = 28;
// A match cannot produce anything near this many goals; the cap exists so a corrupt tap index
// cannot turn the store into the payload.
inline constexpr uint32_t kMaxRows = 256;

struct ScanCensus {
    uint32_t event_rows = 0;    // S_EVENT rows walked
    uint32_t tap_chapters = 0;  // chapters whose keyframe carried a tap table naming this message
    uint32_t rows = 0;          // goal rows stored
    uint32_t refused_rows = 0;  // the right tap, refused by the decoder (short body, bad goal type)
    uint32_t dropped_rows = 0;  // over the row cap
    uint32_t placed_rows = 0;   // rows the arrival interpolation moved off the chapter begin
    uint16_t tap = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    const char* first_drop = nullptr;  // program-lifetime; names the bound that was hit
};

// One decoded goal, kept alongside its verbatim bytes: the fields are for the log and the tests,
// the bytes are what the engine is handed.
struct Goal {
    uint64_t t_us = 0;
    uint16_t team = 0;
    uint16_t points = 0;
    uint16_t goal_type = 0;
    uint32_t scorer = 0;
    uint32_t assist = 0;
    float speed = 0.0f;
    float distance = 0.0f;
    float possession_s = 0.0f;
    uint8_t body[kBodyBytes] = {};
};

class RecordedGoals {
public:
    void Reset();

    // `arrivalUs` is the row's S_EVENT stamp; EndChapter turns it into a playhead-axis time.
    bool Offer(uint64_t arrivalUs, const void* body, uint32_t len, ScanCensus& census);

    void BeginChapter();
    // `firstArrivalUs`/`lastArrivalUs` bound the arrival stamps of EVERY event row in the chapter,
    // not just the goal rows -- that is what makes the interpolation tight.  A chapter that cannot
    // separate them leaves its rows at the chapter midpoint.
    void EndChapter(uint64_t tBeginUs, uint64_t tEndUs, uint64_t firstArrivalUs,
                    uint64_t lastArrivalUs, ScanCensus& census);

    uint32_t Count() const { return count_; }
    const Goal& At(uint32_t i) const { return rows_[i < count_ ? i : 0]; }

    // The newest row at or before `tUs`, as an index; -1 when this file has none there.
    int32_t IndexAtOrBefore(int64_t tUs) const;
    // The first row strictly after `tUs`, as an index; Count() when there is none.
    uint32_t FirstAfter(int64_t tUs) const;

private:
    Goal rows_[kMaxRows];
    uint32_t count_ = 0;
    uint32_t chapter_first_ = 0;
};

// Walks every chapter's S_EVENT and fills `into` from the goal tap, resolving the type index out of
// the FILE'S OWN keyframe tap table -- never a compiled-in index.  Returns the rows stored.
uint32_t ScanRecordedGoals(IDemoReader& reader, RecordedGoals& into, ScanCensus& census);

RecordedGoals& PlaybackRecordedGoals();
ScanCensus& PlaybackGoalCensus();

// ---------------------------------------------------------------------------------------------
// The feed.  Everything below reaches the engine.
// ---------------------------------------------------------------------------------------------

// The message the engine's Listen path hands its handlers: five qwords, one of them read here.
struct EngineGoalMsg {
    uint64_t selfPeerId = 0;
    uint64_t cookie = 0;
    uint64_t body = 0;
    uint64_t blob = 0;
    uint64_t blobLen = 0;
};

using FillFn = void (*)(void* gameplay, const void* msg);
using ReceiveFn = void (*)(void* netGame, const void* msg);
using GoalSfxFn = void (*)(void* netGame, uint32_t netUserId);
// The engine's own deferred named-event raise, (netGame, CSymbol64).  It picks the space itself.
using RaiseFn = void (*)(void* netGame, uint64_t symbol);

struct Handlers {
    FillFn fill = nullptr;
    ReceiveFn receive = nullptr;
    GoalSfxFn sfx = nullptr;
    RaiseFn raise = nullptr;
};
// Test seam: fakes that record what they were handed.  Null restores the bind against the mapped
// image, which on a machine with no game resolves to nothing and refuses by name.
void InjectHandlersForTest(const Handlers* h);

// Recorded user slot -> live SR15NetUserId.  Supplied by the caller because the roster mapping and
// the engine's slot table both live above this file; false means "this recorded user is not on the
// field", which is a refusal and never a guessed slot.
using MapUserFn = bool (*)(void* ctx, uint16_t recSlot, uint32_t& liveNetUserId);

struct ApplyCensus {
    uint32_t fires = 0;            // forward crossings that raised the event
    uint32_t level_writes = 0;     // fill-only calls, the seek/load restore
    uint32_t seeks_suppressed = 0; // jumps that consumed a crossing without firing
    uint32_t holds = 0;            // ticks skipped because the pre-roll owns the clock
    uint32_t unmapped_scorer = 0;
    uint32_t unmapped_assist = 0;
    uint32_t horn_calls = 0;
    uint32_t explosion_raises = 0;  // evt_glb_scored_matanimation raises that reached the engine
    uint32_t pass_reapplies = 0;   // level restores forced by a host score, not by a new row
    uint32_t faulted = 0;
    const char* refusal = nullptr;  // program-lifetime; why the last sweep did nothing
};

void Arm(bool on);
bool Armed();
// playback.goal_horn.  Default OFF: the script's own fanfare runs on a replayed goal (the engine
// gamelog prints "[GOALSCORE] Printing fanfare sound..." and then the fanfare), so this direct call
// doubles it.  The first fire still logs the role word.
void SetHornEnabled(bool on);
bool HornEnabled();

// playback.goal_explosion.  Default ON.
//
// THE EXPLOSION IS NOT DOWNSTREAM OF THE SCORE.  What animates the goal rig is
// evt_glb_scored_matanimation, which the DISC script raises with a SENDEVENTGLOBALNODE once it has
// seen the disc cross.  Neither the score netvars nor the banner touches it, which is why a
// replayed goal had every other element and not this one.
//
// WHY IT IS SAFE TO RAISE AND nevt_increment_score_* IS NOT (ENG-PRESENT-B stopped on that one).
// Scanned by symbol HASH across all 567 local script DBs, this event has exactly one listener --
// the rig, 38965d90ab25e230 -- and that script's entire node vocabulary is cosmetic: cue, sound,
// instance model colour, delay, enable-component, debug print, plus expressions.  It links no
// send-event node, no R15NETSET*, no teleport, no physics, no reset and no lobby node, so there is
// nothing downstream of the raise that can move the world or re-broadcast.  The state machine, the
// disc reset and the teleports hang off nevt_increment_score_*/nevt_game_score, which this does not
// touch.
//
// WHAT IS NOT PROVEN.  The raise is unaddressed: the engine's own entry passes actor = -1, and each
// listening graph decides whether an unaddressed event is for it.  The real raiser addresses the
// scoring goal's rig, so if the rig's entry node carries an authored actor filter this raise is a
// no-op -- silent, not wrong.  That is the one thing only a session can settle, and the fire line
// says so.
void SetExplosionEnabled(bool on);
bool ExplosionEnabled();
// Reporting only: the change lines speak the playhead's axis rather than the file's absolute one.
void SetPlayheadOriginUs(int64_t originUs);

// Reporting only.  Why a change to blue_score/red_score would NOT reach the engine's own change
// notifier right now, as a program-lifetime string; nullptr means it would.  That notifier is the
// trigger for the score flash and the accrual SFX, so this is the reason both name when a recorded
// goal shows neither.  The explosion has a different trigger and its own reason (goalfeed.cpp).
// Supplied by the caller, which owns the HUD netvar writer.
void SetScoreTriggerRefusal(const char* why);

// A discontinuity the user caused.  The next sweep re-bases onto the new playhead and restores the
// level WITHOUT raising, so a forward seek across a goal shows no banner.
void NoteSeek();

// The host's own OnScoreCB reached the engine since the last sweep, so CR15NetGameplay::Increment-
// Score has put the HOST'S team and point count in the scorer fields.  Set by the score gate's
// drain on the tick thread; consumed by the next Sweep's level restore, which re-applies the
// RECORDED fill over them -- fill only, never a raise.
void NotePassSinceLastSweep();

// Forgets what was fired and where the playhead was, without touching the engine.
void Forget();

// One stage-3 tail pass, on the engine tick thread.  `tUs` is the presented playhead, absolute.
// `hold` is true while the pre-roll owns the clock: the cursor is re-based and nothing fires.
void Sweep(void* netGame, int64_t tUs, bool hold, MapUserFn map, void* mapCtx);

const ApplyCensus& Census();
void ResetCensusForTest();

}  // namespace goalfeed
}  // namespace playback
}  // namespace replay
