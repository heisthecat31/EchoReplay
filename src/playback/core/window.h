#pragma once

// W2-B. The owned sample window: what ApplyStateAt reads, and the only thing it reads.
//
// ---------------------------------------------------------------------------------------------
// ★ IT OWNS ITS MEMORY, AND THAT IS A CORRECTNESS PROPERTY, NOT TIDINESS. IDemoReader guarantees a
// DecodedChapter's pointers survive exactly TWO further DecodeChapter calls -- demotypes.h is blunt
// about it: "three may not". The design wants a THREE-chapter resident window,
// [current-1, current+1], which is one more than the reader promises. Rather than argue the number,
// the window COPIES: no StreamView and no `records` pointer is ever stored in a member here, so the
// loop that collects N chapters and then walks them -- the use-after-free that does not fail on two
// and does fail on three -- is not writable against this type.
//
// ★ AND IT ALLOCATES ONCE. The buffers are sized in the constructor, at Load, off the tick thread.
// Build() only fills them. S6.4 forbids allocation on the tick, and a chapter roll happens ON the
// tick, so a window that grew its storage per build would break that budget about every 2 seconds.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include "engine/entity.h"
#include "engine/pawn_provision.h"   // kEntityKeyUnknown
#include "playback/core/resolve.h"
#include "playback/core/smooth.h"
#include "playback/core/transport.h"

namespace replay {
namespace playback {

// ★★★ THE POOL IS SIZED BY THE FILE, NOT BY A NUMBER.
//
// `max_body_samples` was 16384, and the note that chose it read "64 bodies x 90 frames x 3" -- one
// SECOND of chapters. Real chapters are the manifest's keyframe interval: the L2 #3 capture's are
// 4.033 s, ~363 ticks at this project's cadence, so that pool held about fifteen concurrently
// sampled bodies where its own note claimed sixty-four. A window that cannot hold the range refuses
// the whole build, so the number decided whether the middle of an 8-player Arena match was
// reachable at all.
//
// The window spans kWindowChapters chapters, so the samples it must hold are
//   bodies-per-tick x ticks-per-chapter x kWindowChapters,
// and ticks-per-chapter is the file's own chapter span divided by the file's own tick period. Both
// are read off the capture at LOAD (CapsForCadence + Window::Reserve); the defaults below only size
// the window a process holds before any file is open.
constexpr size_t kWindowChapters = 3;

// The fastest cadence anything in this system samples at -- clock_policy.h's own 90 Hz row,
// "66666/11111 = 6.0x at 90 Hz". Used ONLY for the pre-load default; a loaded file supplies its own
// measured period and this value never rounds it.
constexpr uint64_t kFastestTickPeriodUs = 11111;

// S5.5's keyframe interval, which is the chapter span. It is a manifest FIELD and never a constant
// in a reader: this is the pre-load default only, and it is the same 2.0 s capture::ChapterConfig
// writes by default.
constexpr uint64_t kDefaultChapterSpanUs = 2000000;

// Ceil, so a cadence that does not divide evenly is never one tick short. ZERO IN, ZERO OUT: a span
// or period the file did not answer must come back as "could not size", never as a confident
// default that would then refuse builds nobody could explain.
constexpr size_t TicksPerChapter(uint64_t chapterSpanUs, uint64_t tickPeriodUs) {
    return (chapterSpanUs == 0 || tickPeriodUs == 0)
               ? 0
               : static_cast<size_t>((chapterSpanUs + tickPeriodUs - 1) / tickPeriodUs);
}

struct WindowCaps {
    size_t max_bodies = 64;
    size_t max_players = 16;
    size_t max_body_samples =
        max_bodies * TicksPerChapter(kDefaultChapterSpanUs, kFastestTickPeriodUs) * kWindowChapters;
    size_t max_player_samples =
        max_players * TicksPerChapter(kDefaultChapterSpanUs, kFastestTickPeriodUs) * kWindowChapters;
    size_t max_ticks = TicksPerChapter(kDefaultChapterSpanUs, kFastestTickPeriodUs) * kWindowChapters;
};

// The caps a capture with this cadence needs. `maxBodies`/`maxPlayers` stay the roster bounds -- an
// 8-player Arena match is eight players, the disc and both goals, well inside the default 64 -- and
// only the per-tick depth changes with the file. A zero span or period returns caps whose sample
// pools are 0, which Reserve refuses by name rather than silently sizing to nothing.
WindowCaps CapsForCadence(uint64_t chapterSpanUs, uint64_t tickPeriodUs, size_t maxBodies = 64,
                          size_t maxPlayers = 16);

// Above this the window will not allocate. A capture whose chapters need more says so AT LOAD, by
// name, so the owner learns before a seek into the middle of the file refuses with nothing to read.
// 2^21 body samples is 64 bodies at 90 Hz across three chapters of about twelve seconds each --
// far past S5.5's cadence, and still a bounded allocation on a machine also running the game.
constexpr size_t kWindowSampleCeiling = 1u << 21;

// One replayed body. `net_id` is its identity in the FILE; `handle` is where it lives in the engine
// this session. The two are separate because a replay creates its entities in its own order, so the
// file's identity is never a handle and a type that conflated them would be a wrong-entity write.
// ★★ THE BODY'S IDENTITY IS TAGGED, AND THE TAG IS PART OF THE KEY.
//
// W2-A's BodyFrame carries `idValue` (the qword at SimState+0x48) and `idTag` (the dword at +0x4C
// saying whether that value is a user id, a component id or a NET id). Its own comment is the rule:
// "idValue on its own is a number whose meaning was not recorded." So a window that keyed on the bare
// qword would merge a user id and a net id that happened to collide, and would hand a component id to
// PhysOfNetId -- correct for the common case and silently wrong for the others.
//
// The key here is therefore the PAIR, and `id_fill` carries whether the identity is resolvable at all
// (capture::BodyIdentityResolvable). A sample whose identity is not resolvable is not keyed on: it is
// counted and dropped, because a track built on a meaningless number is worse than a missing track.
struct BodyIdentity {
    uint64_t value = 0;
    uint32_t tag = 0;
    // ONLY the identity bits of BodyFill -- masked at the boundary in windowbuild, never the whole
    // word, because the transform/lastheld/bits flags vary per record and would split one body into
    // one track per fill combination.
    uint32_t fill = 0;

    // ★★ THE KEY IS value + tag + INTERPRETATION, and the third part was learned the hard way.
    //
    // A qword is not an identity; a qword under a stated reading is. The same 0x1234 carrying
    // kFillBodyIdNetActor ("this is a net id, the accessor guarantees it") and carrying
    // kFillBodyIdTag with tag 0 ("the tag was read and it says SR15NetUserId") denote two DIFFERENT
    // things that happen to share bits. Keying on value+tag alone merged them into one track -- the
    // exact defect the pair-key was introduced to prevent, arriving one level further in because the
    // pair was not wide enough. A test caught it; nothing else would have.
    bool SameAs(const BodyIdentity& o) const {
        return value == o.value && tag == o.tag && fill == o.fill;
    }
};

struct BodyTrack {
    BodyIdentity id;
    PhysHandle handle{0};
    bool handle_valid = false;
    // Bound through the recording's USER SLOT rather than through the net-actor table. The recording
    // client's own body has no net-actor entry, so `NetIdOf` answers 0 and the capture leaves every
    // identity fill bit clear (gather.h: "0 is the honest answer for a purely local body"). Its
    // counterpart on the replay machine is the viewer's own pawn, which that machine SIMULATES -- so
    // the applier has to claim it before it can drive it, and both facts have to travel with the
    // track for it to know that.
    bool bound_by_slot = false;
    // The recorded entrant this slot resolved to is a REMOTE pawn (a different user than the viewer),
    // whose already-present phys CI the engine drives by pool index but our stricter-than-engine
    // identity gate refuses. Only this surface takes the bounds-only (decoupled) drive so its pose
    // moves without waiting on the model instantiation; the local pawn keeps the full gate.
    bool slot_remote = false;
    // Bound through a recorded SComponentID the live frisbee vouched for: the disc. The host
    // simulates it, so the applier claims it exactly as it claims a slot-bound body, and the
    // release hands it back to the host's physics.
    bool claim_owner = false;
    // The SComponentID the claim was made against. Re-asked of the engine once per drive tick: the
    // engine recycles the component on a round reset and the handle then addresses another body.
    uint32_t disc_component_id = 0;
    uint16_t user_slot = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    // The engine entity key of the pawn this track bound to by slot, per track because a multi-player
    // capture binds several and the bind's summary can name only one.
    uint32_t slot_entity = kEntityKeyUnknown;
    bool synthetic_owner_set = false;
    // The disc's possession LEVEL, as a recorded user slot, that was last stamped into PhysCI+0x1C.
    // The engine derives its own grab/release edges by diffing the level against its latch, so the
    // applier only has to keep the level true -- one compare per tick, a store when the recording
    // says the holder changed. Cleared wherever synthetic_owner_set is: a released or re-bound
    // track has nothing stamped, so the next tick must publish again.
    uint32_t owner_level_slot = 0;
    bool owner_level_published = false;
    // The published level resolved to a REAL live user id rather than to the out-of-table value.
    // Only then is the engine's own game_frisbee_ownerid republish the right answer.
    bool owner_level_real = false;
    // The recording says nobody holds the disc and the published level is still the last real
    // holder (see StateApplier::PublishDiscOwner). The engine's republish is not the truth then.
    bool owner_level_held_over = false;
    size_t first = 0, count = 0;
    uint64_t last_t = 0;      // this track's newest staged stamp; the per-track ordering guard
    SeriesCursor cursor;
    BodyKnotMemo smooth_memo;
};

// Who a player record belongs to IN THE RECORDING: the recorded user slot, which is the number the
// manifest roster, the dresser's RosterMapping and the pawn slot map are all keyed on.
//
// It is never a stream's dense index. S_PLAYER_SAMPLED stores the user slot as its entity
// (gather.h GatherLocalPawns); S_PLAYER_WIRE stores a first-seen index over the message header word
// (chapterbuilder.cpp SlotMap), so its slot comes out of `hdr0` instead -- on the real spectator
// capture wire index 0 is user slot 1. Keying both on the bare integer put two people on one track.
constexpr uint16_t kGenerationUnknown = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids

struct PlayerIdentity {
    uint16_t user_slot = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    // The SR15NetUserId generation beside the slot. Only the wire carries it; a sampled row says
    // kGenerationUnknown. Part of the key, so a slot handed to a second person mid-file is a second
    // track rather than one track with two people in it.
    uint16_t generation = kGenerationUnknown;
    // Read off the recorder's own pools (S_PLAYER_SAMPLED) rather than the wire. Not part of the
    // key; BindBodyHandles pairs the recorder's bodiless-id body with the one sampled player.
    bool sampled = true;
};

struct PlayerTrack {
    // ★★ TWO INDEX SPACES, AND THEY DO NOT CONVERT.
    //
    // `user_slot` is the RECORDED user slot (PlayerIdentity above). `user_id` is the entrant's engine
    // user index IN THIS SESSION -- INV-OWN1's subject, and the number IsLocallyControlled and
    // PeerTime key on. The viewer already occupies a slot, so the two differ; putting the recorded
    // slot in `user_id` would make INV-OWN1 validate the wrong value and PASS. Resolving one to the
    // other is W2-C's, through the manifest's PlayerRecord table; until it has, `user_id_valid` is
    // false and this track is not published.
    uint16_t user_slot = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    uint16_t generation = kGenerationUnknown;
    bool from_sampled = false;
    uint32_t user_id = 0;
    bool user_id_valid = false;
    PlayerHandle handle{0};
    bool handle_valid = false;
    size_t first = 0, count = 0;
    uint64_t last_t = 0;
    SeriesCursor cursor;
    PlayerKnotMemo smooth_memo;
    // As-seen inputs (S7.2.4a M1). Both zero in ground truth, so ONE code path serves both modes and
    // the difference is three numbers rather than a second publisher.
    int64_t render_delay_us = 0;
    int64_t time_offset_us = 0;
    bool occluded = false;             // the client did not update this player that frame
    bool body_updated_this_frame = true;
};

class Window {
public:
    explicit Window(const WindowCaps& caps = WindowCaps());
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    void Clear();

    // Re-size the pools for a file that has just opened. Grows only: a window already wide enough
    // for this cadence keeps its storage, so a load between two files of the same shape allocates
    // nothing. Returns false -- and changes NOTHING -- when the request is unsized (a zero pool) or
    // past kWindowSampleCeiling; `why` then names which, for the load-time refusal sentence. Any
    // growth clears the window: the old pools are gone, so nothing that pointed into them survives.
    bool Reserve(const WindowCaps& caps, const char** why = nullptr);

    const WindowCaps& Caps() const { return caps_; }

    // Fill. The caller decodes from IDemoReader and hands over plain values; nothing borrowed from a
    // DecodedChapter reaches a member here.
    bool AddBodySample(const BodyIdentity& id, uint64_t t_us, const BodyState&);
    bool AddPlayerSample(const PlayerIdentity& who, uint64_t t_us, const PlayerState&);
    // A sampled row for `user_slot`, generation unknown.
    bool AddPlayerSample(uint16_t user_slot, uint64_t t_us, const PlayerState& s) {
        PlayerIdentity who;
        who.user_slot = user_slot;
        return AddPlayerSample(who, t_us, s);
    }
    void SetTicks(const uint64_t* boundaries, size_t n);

    // ★★ MANDATORY AFTER THE LAST Add*, AND THE REASON IS A BUG THIS CLASS SHIPPED WITH.
    //
    // Samples arrive INTERLEAVED -- a real chapter is body A frame 0, body B frame 0, body A frame 1,
    // and so on -- but the sampler needs each track's samples CONTIGUOUS and time-ordered, because a
    // cursor walks an array. The first version appended straight into the flat pool and could only
    // extend the most recently touched track, so on the ordinary interleaved shape it kept the first
    // sample of each track and silently refused the rest: 61 of 120 in the round-trip that caught it.
    //
    // So Add* now STAGES, and Finalize regroups by a counting pass -- the same two-pass shape
    // KeyedResolver::Build uses, and for the same reason: no allocation, and the layout the consumer
    // needs is produced once instead of hoped for.
    void Finalize();
    bool Finalized() const { return finalized_; }
    // Adds refused because Finalize had already run. ★★ A PRECONDITION NOTHING ENFORCES IS A
    // COMMENT. After Finalize the tracks point at contiguous runs in the regrouped pool; one further
    // Add staged a sample the regroup never saw and left a track reading across into its neighbour --
    // an audit got a track's newest com[0] as its NEIGHBOUR's first frame. Refused and counted now,
    // so the repair cannot preserve the defect's shape.
    size_t PostFinalizeAdds() const { return post_finalize_adds_; }
    // ★★ Samples whose stamp went BACKWARDS within their own track and were clamped forward to keep
    // the cursor's precondition. The clamp is necessary -- an out-of-order track makes the resolver
    // answer nonsense -- but it REWRITES a timestamp the file supplied, and a rewrite nobody counts is
    // a file defect the replay silently smooths over. Non-zero means the file is not what it claims.
    size_t SampleTimeRegressions() const { return sample_time_regressions_; }
    // Tick boundaries that went backwards, counted at SetTicks for the same reason.
    size_t TickRegressions() const { return tick_regressions_; }

    // ★★ WHAT SPAN THIS WINDOW CAN ANSWER FOR, and why a caller must ask.
    //
    // A window holds three chapters. Sampling OUTSIDE that span does not fault: the cursor clamps to
    // the track's first or last sample and returns a pose that is plausible and stale, which renders
    // as a body frozen slightly wrong rather than as an error. So the span is recorded and the frame
    // loop checks it -- a rebuild is a ChapterEpochRoll discontinuity, not a silent clamp.
    //
    // The inner span is narrower than the outer one on purpose: with chapters [c-1, c, c+1] resident,
    // the playhead should live in c, and leaving c is the trigger to rebuild while c-1 and c+1 still
    // cover the sampling either side of the move.
    void SetCoverage(uint64_t outer_begin, uint64_t outer_end, uint64_t inner_begin,
                     uint64_t inner_end);
    bool CoversForSampling(int64_t t_us) const;   // inside the OUTER span: a sample here is honest
    bool NeedsRebuild(int64_t t_us) const;        // outside the INNER span: rebuild before it is not
    // ★ LOOKAHEAD, not correctness. DESIGN-UI-2 S5.1: once the rebuild guards read
    // CoversForSampling, a rebuild happens only at the OUTER edge -- zero decoded material ahead,
    // i.e. a guaranteed stall exactly at the crossing, forwards and backwards. This is the
    // predicate that re-centres a whole chapter before that, on an idle tick. True when the sample
    // is honest but `t_us` is within one centre-chapter of the outer edge in the direction of
    // travel. `dir` is the sign of the rate; 0 (paused, or scrubbing) means both edges.
    bool RecentreMargin(int64_t t_us, int dir) const;
    uint64_t OuterBeginUs() const { return outer_begin_; }
    uint64_t OuterEndUs() const { return outer_end_; }

    size_t BodyCount() const { return body_count_; }
    size_t PlayerCount() const { return player_count_; }
    BodyTrack& BodyAt(size_t i) { return bodies_[i]; }
    PlayerTrack& PlayerAt(size_t i) { return players_[i]; }
    const BodyState* BodyFrames(const BodyTrack& t) const { return body_frames_ + t.first; }
    const uint64_t* BodyTimes(const BodyTrack& t) const { return body_times_ + t.first; }
    const PlayerState* PlayerFrames(const PlayerTrack& t) const { return player_frames_ + t.first; }
    const uint64_t* PlayerTimes(const PlayerTrack& t) const { return player_times_ + t.first; }

    KeyedResolver& NetVars() { return netvars_; }

    // ★ The match clock (S_GAMESTATE gameTimer) as one time-indexed track, decoded
    // beside the netvar track and resolved on the SAME playhead T that drives body motion. Continuous
    // (unlike a netvar): ClockAt linearly interpolates between the two samples bracketing T, so the
    // displayed clock is smooth under scrub and slow-mo. Only samples whose gameTimer answered are added
    // here, so a chapter that sampled everything but the clock leaves the track alone rather than
    // inserting a zero. Appended in file-time order across chapters; a stamp that would go backwards is
    // clamped forward, matching the body/player tracks' cursor precondition.
    bool AddGameStateClock(uint64_t t_us, float gameTimer);
    // The clock at T: last sample <= T, interpolated toward the next sample. False when the track is
    // empty or T precedes the first sample (the engine's own value then stands), mirroring resolve.h's
    // kNoRecord contract. Const: it holds no cursor, so a seek costs it nothing to invalidate.
    bool ClockAt(int64_t t_us, float& out) const;
    size_t GameStateClockCount() const { return gs_clock_count_; }

    // ★ The netvar SLOT, which the resolver's key does not carry. A netvar is addressed by
    // (slot, name): the slot picks the update record the engine writes into (echovr.exe
    // sub_140D3DF90, `movzx r15d, word ptr [rcx+8]` then `[level+0xD0 + 4*slot]`), and the name picks
    // the row inside it. Both ingest paths read a slot -- S_NETVAR's message header and the keyframe
    // block's leading u16 -- and both used to drop it, so every replayed row was addressed to slot 0
    // and the 23 slot-1 rows (mp_arena_rules_netsettings) would have been written into slot 0's
    // record. A name lives in exactly one slot, so one map answers for the whole window.
    void NoteNetVarSlot(uint64_t name, uint16_t slot) {
        for (size_t i = 0; i < slot_count_; ++i) {
            if (slot_names_[i] == name) return;
        }
        if (slot_count_ < KeyedResolver::kMaxKeys) {
            slot_names_[slot_count_] = name;
            slot_values_[slot_count_] = slot;
            ++slot_count_;
        }
    }
    // False when the window never saw a slot for this name. NOT a zero default: slot 0 is a real
    // slot, so answering it for an unknown name would address a guess to a live record.
    bool NetVarSlotOf(uint64_t name, uint16_t& out) const {
        for (size_t i = 0; i < slot_count_; ++i) {
            if (slot_names_[i] == name) {
                out = slot_values_[i];
                return true;
            }
        }
        return false;
    }
    size_t NetVarSlotCount() const { return slot_count_; }

    // The last raw SValue we WROTE per key, so a frame writes only what changed.
    //
    // ★ It lives here rather than in the applier on purpose. It is a cache of a resolved function, so
    // INV-P says it must be invalidated on a discontinuity and never updated -- and the window is the
    // thing that owns invalidation. Putting it in StateApplier would have forced the applier to learn
    // that a jump happened, which is the one thing its signature exists to prevent.
    bool LastWritten(size_t key_index, uint64_t& out) const;
    void NoteWritten(size_t key_index, uint64_t value);
    const TickTable& Ticks() const { return ticks_; }

    // ★ Called on EVERY discontinuity. INV-P: a cache is invalidated, never updated. This is the
    // whole of what a seek costs the sample layer -- there is no second path that re-reads the file
    // differently, because there is no second path.
    void InvalidateCursors();

    // Motion smoothing. The file edges are set by the range build: a window that holds the file's
    // first (last) core chapter has nothing before (after) it anywhere, so a knot there is complete.
    void SetFileEdges(bool begins_file, bool ends_file) { begins_file_ = begins_file; ends_file_ = ends_file; }
    bool BeginsFile() const { return begins_file_; }
    bool EndsFile() const { return ends_file_; }
    // A strength change invalidates every memoised knot; the same strength again is free.
    void SetSmoothing(const SmoothParams& p);
    const SmoothParams& Smoothing() const { return smoothing_; }
    // Stamps the per-track knot memos. Bumped by Clear, Finalize and a strength change; never 0.
    uint32_t SmoothGen() const { return smooth_gen_; }

    size_t Overflowed() const { return overflow_; }
    // Samples the pool could not hold. Distinct from Overflowed(), which also counts track-table and
    // tick-table overruns; a caller that wants "did I lose sample data" wants this one.
    size_t DroppedSamples() const { return dropped_samples_; }

private:
    size_t FindOrAddBody(const BodyIdentity& id);
    size_t FindOrAddPlayer(const PlayerIdentity& who);

    WindowCaps caps_;
    BodyTrack* bodies_ = nullptr;
    PlayerTrack* players_ = nullptr;
    BodyState* body_frames_ = nullptr;
    uint64_t* body_times_ = nullptr;
    PlayerState* player_frames_ = nullptr;
    uint64_t* player_times_ = nullptr;
    uint64_t* tick_storage_ = nullptr;

    // Staging: the track each pooled sample belongs to, before Finalize regroups them.
    uint16_t* body_owner_ = nullptr;
    uint16_t* player_owner_ = nullptr;
    BodyState* body_scratch_ = nullptr;
    uint64_t* body_scratch_t_ = nullptr;
    PlayerState* player_scratch_ = nullptr;
    uint64_t* player_scratch_t_ = nullptr;

    size_t body_count_ = 0, player_count_ = 0;
    size_t body_used_ = 0, player_used_ = 0;
    size_t overflow_ = 0;
    size_t dropped_samples_ = 0;
    size_t post_finalize_adds_ = 0;
    size_t sample_time_regressions_ = 0;
    size_t tick_regressions_ = 0;
    bool finalized_ = false;

    uint64_t outer_begin_ = 0, outer_end_ = 0;
    uint64_t inner_begin_ = 0, inner_end_ = 0;
    bool coverage_set_ = false;

    KeyedResolver netvars_;
    uint64_t slot_names_[KeyedResolver::kMaxKeys] = {};
    uint16_t slot_values_[KeyedResolver::kMaxKeys] = {};
    size_t slot_count_ = 0;
    uint64_t* nv_last_ = nullptr;
    bool* nv_has_ = nullptr;

    // The match-clock track. Sized like the tick table (at most one gamestate sample per tick), so it
    // is bounded by the file's own cadence and allocated once with the rest of the pools.
    uint64_t* gs_clock_times_ = nullptr;
    float* gs_clock_vals_ = nullptr;
    size_t gs_clock_count_ = 0;
    uint64_t gs_clock_last_t_ = 0;

    TickTable ticks_;

    bool begins_file_ = false, ends_file_ = false;
    SmoothParams smoothing_;
    uint32_t smooth_gen_ = 1;
    void BumpSmoothGen() { smooth_gen_ = smooth_gen_ == UINT32_MAX ? 1 : smooth_gen_ + 1; }
};

}  // namespace playback
}  // namespace replay
