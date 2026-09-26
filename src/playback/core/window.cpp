#include "playback/core/window.h"

#include <string.h>

namespace replay {
namespace playback {

Window::Window(const WindowCaps& caps) : caps_(caps) {
    bodies_ = new BodyTrack[caps_.max_bodies];
    players_ = new PlayerTrack[caps_.max_players];
    body_frames_ = new BodyState[caps_.max_body_samples];
    body_times_ = new uint64_t[caps_.max_body_samples];
    player_frames_ = new PlayerState[caps_.max_player_samples];
    player_times_ = new uint64_t[caps_.max_player_samples];
    tick_storage_ = new uint64_t[caps_.max_ticks];
    nv_last_ = new uint64_t[KeyedResolver::kMaxKeys]();
    nv_has_ = new bool[KeyedResolver::kMaxKeys]();
    gs_clock_times_ = new uint64_t[caps_.max_ticks];
    gs_clock_vals_ = new float[caps_.max_ticks];
    body_owner_ = new uint16_t[caps_.max_body_samples];
    player_owner_ = new uint16_t[caps_.max_player_samples];
    body_scratch_ = new BodyState[caps_.max_body_samples];
    body_scratch_t_ = new uint64_t[caps_.max_body_samples];
    player_scratch_ = new PlayerState[caps_.max_player_samples];
    player_scratch_t_ = new uint64_t[caps_.max_player_samples];
    Clear();
}

WindowCaps CapsForCadence(uint64_t chapterSpanUs, uint64_t tickPeriodUs, size_t maxBodies,
                          size_t maxPlayers) {
    WindowCaps c;
    c.max_bodies = maxBodies;
    c.max_players = maxPlayers;
    const size_t ticks = TicksPerChapter(chapterSpanUs, tickPeriodUs);
    c.max_ticks = ticks * kWindowChapters;
    c.max_body_samples = maxBodies * c.max_ticks;
    c.max_player_samples = maxPlayers * c.max_ticks;
    return c;
}

bool Window::Reserve(const WindowCaps& caps, const char** why) {
    if (caps.max_body_samples == 0 || caps.max_player_samples == 0 || caps.max_ticks == 0 ||
        caps.max_bodies == 0 || caps.max_players == 0) {
        if (why != nullptr) *why = "the capture did not say how long a chapter is or how far apart its ticks are";
        return false;
    }
    if (caps.max_body_samples > kWindowSampleCeiling ||
        caps.max_player_samples > kWindowSampleCeiling) {
        if (why != nullptr) *why = "this capture's chapters hold more samples than the playback window can allocate";
        return false;
    }
    // Grows only, and only where it must: the caps of a window already wide enough are left exactly
    // as they are, so a second load of the same shape neither reallocates nor narrows anything.
    WindowCaps want = caps_;
    bool grow = false;
    if (caps.max_bodies > want.max_bodies) { want.max_bodies = caps.max_bodies; grow = true; }
    if (caps.max_players > want.max_players) { want.max_players = caps.max_players; grow = true; }
    if (caps.max_body_samples > want.max_body_samples) { want.max_body_samples = caps.max_body_samples; grow = true; }
    if (caps.max_player_samples > want.max_player_samples) { want.max_player_samples = caps.max_player_samples; grow = true; }
    if (caps.max_ticks > want.max_ticks) { want.max_ticks = caps.max_ticks; grow = true; }
    if (!grow) {
        return true;
    }

    // Allocate the new pools BEFORE releasing the old ones would be the safer shape, but every
    // allocation here is `new[]` on a fatal-on-failure heap, so the simple order is honest: the
    // window is cleared, the storage is replaced, and Clear() re-establishes every count.
    delete[] bodies_;
    delete[] players_;
    delete[] body_frames_;
    delete[] body_times_;
    delete[] player_frames_;
    delete[] player_times_;
    delete[] tick_storage_;
    delete[] gs_clock_times_;
    delete[] gs_clock_vals_;
    delete[] body_owner_;
    delete[] player_owner_;
    delete[] body_scratch_;
    delete[] body_scratch_t_;
    delete[] player_scratch_;
    delete[] player_scratch_t_;

    caps_ = want;
    bodies_ = new BodyTrack[caps_.max_bodies];
    players_ = new PlayerTrack[caps_.max_players];
    body_frames_ = new BodyState[caps_.max_body_samples];
    body_times_ = new uint64_t[caps_.max_body_samples];
    player_frames_ = new PlayerState[caps_.max_player_samples];
    player_times_ = new uint64_t[caps_.max_player_samples];
    tick_storage_ = new uint64_t[caps_.max_ticks];
    gs_clock_times_ = new uint64_t[caps_.max_ticks];
    gs_clock_vals_ = new float[caps_.max_ticks];
    body_owner_ = new uint16_t[caps_.max_body_samples];
    player_owner_ = new uint16_t[caps_.max_player_samples];
    body_scratch_ = new BodyState[caps_.max_body_samples];
    body_scratch_t_ = new uint64_t[caps_.max_body_samples];
    player_scratch_ = new PlayerState[caps_.max_player_samples];
    player_scratch_t_ = new uint64_t[caps_.max_player_samples];
    Clear();
    return true;
}

Window::~Window() {
    delete[] bodies_;
    delete[] players_;
    delete[] body_frames_;
    delete[] body_times_;
    delete[] player_frames_;
    delete[] player_times_;
    delete[] tick_storage_;
    delete[] nv_last_;
    delete[] nv_has_;
    delete[] gs_clock_times_;
    delete[] gs_clock_vals_;
    delete[] body_owner_;
    delete[] player_owner_;
    delete[] body_scratch_;
    delete[] body_scratch_t_;
    delete[] player_scratch_;
    delete[] player_scratch_t_;
}

bool Window::LastWritten(size_t key_index, uint64_t& out) const {
    if (key_index >= KeyedResolver::kMaxKeys || !nv_has_[key_index]) {
        return false;
    }
    out = nv_last_[key_index];
    return true;
}

void Window::NoteWritten(size_t key_index, uint64_t value) {
    if (key_index < KeyedResolver::kMaxKeys) {
        nv_last_[key_index] = value;
        nv_has_[key_index] = true;
    }
}

void Window::Clear() {
    body_count_ = player_count_ = 0;
    body_used_ = player_used_ = 0;
    outer_begin_ = outer_end_ = inner_begin_ = inner_end_ = 0;
    coverage_set_ = false;
    overflow_ = 0;
    dropped_samples_ = 0;
    post_finalize_adds_ = 0;
    sample_time_regressions_ = 0;
    tick_regressions_ = 0;
    finalized_ = false;
    begins_file_ = ends_file_ = false;
    BumpSmoothGen();
    gs_clock_count_ = 0;
    gs_clock_last_t_ = 0;
    ticks_.Set(tick_storage_, 0);
}

// One match-clock sample. Bounded by the tick pool (S_GAMESTATE is at most one sample per tick), and a
// stamp that would go backwards is clamped forward so ResolveBySearch's non-decreasing precondition
// holds -- the same rule the body/player tracks apply, and for the same reason.
bool Window::AddGameStateClock(uint64_t t_us, float gameTimer) {
    if (gs_clock_count_ >= caps_.max_ticks) {
        ++overflow_;
        ++dropped_samples_;
        return false;
    }
    uint64_t at = t_us;
    if (gs_clock_count_ && at < gs_clock_last_t_) {
        at = gs_clock_last_t_;
        ++sample_time_regressions_;
    }
    gs_clock_times_[gs_clock_count_] = at;
    gs_clock_vals_[gs_clock_count_] = gameTimer;
    ++gs_clock_count_;
    gs_clock_last_t_ = at;
    return true;
}

bool Window::ClockAt(int64_t t_us, float& out) const {
    const size_t i = ResolveBySearch(gs_clock_times_, gs_clock_count_, t_us);
    if (i == kNoRecord) {
        return false;   // T precedes the first sample (or the track is empty): the engine's value stands
    }
    // i is the last sample at or before T, so T lies in [times[i], times[i+1]). Interpolate toward the
    // next sample for a continuous clock; hold the last value when there is no later sample.
    if (i + 1 < gs_clock_count_) {
        const uint64_t t0 = gs_clock_times_[i];
        const uint64_t t1 = gs_clock_times_[i + 1];
        const uint64_t ut = (uint64_t)t_us;
        if (t1 > t0 && ut > t0) {
            const float f = (float)(ut - t0) / (float)(t1 - t0);
            out = gs_clock_vals_[i] + (gs_clock_vals_[i + 1] - gs_clock_vals_[i]) * f;
            return true;
        }
    }
    out = gs_clock_vals_[i];
    return true;
}

// The counting pass. Runs once, after ingest, off the sampler's path.
template <class T>
static void Regroup(const uint16_t* owner, const T* src_v, const uint64_t* src_t, size_t used,
                    size_t track_count, T* dst_v, uint64_t* dst_t,
                    size_t* first_out, size_t* count_out) {
    for (size_t k = 0; k < track_count; ++k) {
        first_out[k] = 0;
        count_out[k] = 0;
    }
    for (size_t i = 0; i < used; ++i) {
        ++count_out[owner[i]];
    }
    size_t at = 0;
    for (size_t k = 0; k < track_count; ++k) {
        first_out[k] = at;
        at += count_out[k];
        count_out[k] = 0;   // reused as a fill cursor
    }
    // Stable within a track, so the file's time order survives -- which is the cursor's precondition.
    for (size_t i = 0; i < used; ++i) {
        const uint16_t k = owner[i];
        const size_t dst = first_out[k] + count_out[k];
        dst_v[dst] = src_v[i];
        dst_t[dst] = src_t[i];
        ++count_out[k];
    }
}

void Window::Finalize() {
    if (finalized_) {
        return;
    }
    size_t first[256], count[256];
    if (body_count_ <= 256) {
        Regroup(body_owner_, body_scratch_, body_scratch_t_, body_used_, body_count_,
                body_frames_, body_times_, first, count);
        for (size_t k = 0; k < body_count_; ++k) {
            bodies_[k].first = first[k];
            bodies_[k].count = count[k];
        }
    }
    if (player_count_ <= 256) {
        Regroup(player_owner_, player_scratch_, player_scratch_t_, player_used_, player_count_,
                player_frames_, player_times_, first, count);
        for (size_t k = 0; k < player_count_; ++k) {
            players_[k].first = first[k];
            players_[k].count = count[k];
        }
    }
    finalized_ = true;
    BumpSmoothGen();
    InvalidateCursors();
}

void Window::SetSmoothing(const SmoothParams& p) {
    if (p.sigma_s != smoothing_.sigma_s || p.radius_s != smoothing_.radius_s ||
        p.strength != smoothing_.strength) {
        smoothing_ = p;
        BumpSmoothGen();
    }
}

size_t Window::FindOrAddBody(const BodyIdentity& id) {
    for (size_t i = 0; i < body_count_; ++i) {
        if (bodies_[i].id.SameAs(id)) {
            return i;
        }
    }
    if (body_count_ >= caps_.max_bodies) {
        ++overflow_;
        return (size_t)-1;
    }
    BodyTrack& t = bodies_[body_count_];
    t = BodyTrack();
    t.id = id;
    t.first = body_used_;
    t.count = 0;
    return body_count_++;
}

size_t Window::FindOrAddPlayer(const PlayerIdentity& who) {
    for (size_t i = 0; i < player_count_; ++i) {
        if (players_[i].user_slot == who.user_slot && players_[i].generation == who.generation) {
            return i;
        }
    }
    if (player_count_ >= caps_.max_players) {
        ++overflow_;
        return (size_t)-1;
    }
    PlayerTrack& t = players_[player_count_];
    t = PlayerTrack();
    t.user_slot = who.user_slot;
    t.generation = who.generation;
    t.first = player_used_;
    t.count = 0;
    return player_count_++;
}

// Samples must arrive in non-decreasing time per track -- the cursor's precondition and the
// interpolator's. A sample that would go backwards is CLAMPED to its predecessor's time rather than
// rejected: dropping it would silently shorten the track, and the file's own stamps are what they are.
bool Window::AddBodySample(const BodyIdentity& id, uint64_t t_us, const BodyState& s) {
    if (finalized_) {
        ++post_finalize_adds_;
        return false;   // ★ refused, not silently staged into a pool nothing will regroup
    }
    const size_t k = FindOrAddBody(id);
    if (k == (size_t)-1) {
        return false;
    }
    BodyTrack& t = bodies_[k];
    if (body_used_ >= caps_.max_body_samples) {
        ++overflow_;
        ++dropped_samples_;
        return false;
    }
    // Non-decreasing per TRACK, which is the cursor's precondition. Compared against this track's own
    // previous sample, not against the pool's last write, because the pool is interleaved.
    uint64_t at = t_us;
    if (t.count && at < t.last_t) {
        at = t.last_t;
        ++sample_time_regressions_;   // counted: a rewritten stamp is a finding, not housekeeping
    }
    body_scratch_[body_used_] = s;
    body_scratch_t_[body_used_] = at;
    body_owner_[body_used_] = (uint16_t)k;
    ++body_used_;
    ++t.count;
    t.last_t = at;
    finalized_ = false;
    return true;
}

bool Window::AddPlayerSample(const PlayerIdentity& who, uint64_t t_us, const PlayerState& s) {
    if (finalized_) {
        ++post_finalize_adds_;
        return false;
    }
    const size_t k = FindOrAddPlayer(who);
    if (k == (size_t)-1) {
        return false;
    }
    PlayerTrack& t = players_[k];
    if (player_used_ >= caps_.max_player_samples) {
        ++overflow_;
        ++dropped_samples_;
        return false;
    }
    t.from_sampled = t.from_sampled || who.sampled;
    uint64_t at = t_us;
    if (t.count && at < t.last_t) {
        at = t.last_t;
        ++sample_time_regressions_;
    }
    player_scratch_[player_used_] = s;
    player_scratch_t_[player_used_] = at;
    player_owner_[player_used_] = (uint16_t)k;
    ++player_used_;
    ++t.count;
    t.last_t = at;
    finalized_ = false;
    return true;
}

void Window::SetTicks(const uint64_t* boundaries, size_t n) {
    if (n > caps_.max_ticks) {
        n = caps_.max_ticks;
        ++overflow_;
    }
    if (boundaries && n) {
        memcpy(tick_storage_, boundaries, n * sizeof(uint64_t));
        // Counted, never repaired. The transport classifies a regression itself and crosses it; what
        // matters here is that somebody says the table arrived out of order.
        for (size_t i = 1; i < n; ++i) {
            if (tick_storage_[i] < tick_storage_[i - 1]) {
                ++tick_regressions_;
            }
        }
    }
    ticks_.Set(tick_storage_, n);
}

void Window::SetCoverage(uint64_t outer_begin, uint64_t outer_end, uint64_t inner_begin,
                         uint64_t inner_end) {
    outer_begin_ = outer_begin;
    outer_end_ = outer_end;
    inner_begin_ = inner_begin;
    inner_end_ = inner_end;
    coverage_set_ = true;
}

bool Window::CoversForSampling(int64_t t_us) const {
    if (!coverage_set_) {
        return false;   // an unset coverage answers NO, so a caller cannot inherit a default yes
    }
    if (t_us < 0) {
        return false;
    }
    const uint64_t t = (uint64_t)t_us;
    return t >= outer_begin_ && t <= outer_end_;
}

bool Window::NeedsRebuild(int64_t t_us) const {
    if (!coverage_set_) {
        return true;
    }
    if (t_us < 0) {
        return true;
    }
    const uint64_t t = (uint64_t)t_us;
    return t < inner_begin_ || t > inner_end_;
}

bool Window::RecentreMargin(int64_t t_us, int dir) const {
    // Outside the outer span there is nothing to look ahead with: that is the correctness guard's
    // case, and answering yes here would double the rebuild.
    if (!CoversForSampling(t_us)) {
        return false;
    }
    // The margin is the resident CENTRE chapter's own span, so it tracks 2-4 s chapters instead of
    // a number that would be wrong on both. A degenerate inner span asks for nothing.
    if (inner_end_ <= inner_begin_) {
        return false;
    }
    const uint64_t margin = inner_end_ - inner_begin_;
    const uint64_t t = (uint64_t)t_us;
    if (dir >= 0 && outer_end_ >= t && outer_end_ - t < margin) {
        return true;
    }
    if (dir <= 0 && t >= outer_begin_ && t - outer_begin_ < margin) {
        return true;
    }
    return false;
}

void Window::InvalidateCursors() {
    for (size_t i = 0; i < body_count_; ++i) {
        bodies_[i].cursor.Invalidate();
    }
    for (size_t i = 0; i < player_count_; ++i) {
        players_[i].cursor.Invalidate();
    }
    netvars_.InvalidateCursors();
    // ★ The write shadow goes too. After a jump the engine holds whatever the pre-jump frame wrote,
    // so "unchanged since last write" is a statement about a world that no longer exists -- keeping it
    // would skip exactly the writes a seek needs most.
    for (size_t i = 0; i < KeyedResolver::kMaxKeys; ++i) {
        nv_has_[i] = false;
    }
}

}  // namespace playback
}  // namespace replay
