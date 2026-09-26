#include "playback/core/transport.h"

#include <math.h>

namespace replay {
namespace playback {

const char* StepKindName(StepKind k) {
    switch (k) {
        case StepKind::Normal: return "normal";
        case StepKind::ZeroDt: return "zero-dt";
        case StepKind::Gap: return "gap";
        case StepKind::Regressed: return "regressed";
        default: return "?";
    }
}

StepKind ClassifyStep(uint64_t prev, uint64_t now, uint64_t max_step_us, uint64_t& dt_out) {
    dt_out = 0;
    // ★★ THE ORDER IS THE POINT. "Did the source go backwards" is answered FIRST, in signed form,
    // before any magnitude is computed -- checking afterwards returns the wrong verdict on the way to
    // the right one, because the unsigned difference has already collapsed a regression and a gap onto
    // the same enormous value.
    if (now < prev) {
        return StepKind::Regressed;
    }
    if (now == prev) {
        return StepKind::ZeroDt;   // a real reading: the recorded clock did not advance
    }
    const uint64_t d = now - prev;
    if (d > max_step_us) {
        return StepKind::Gap;
    }
    dt_out = d;
    return StepKind::Normal;
}

size_t TickTable::IndexAt(int64_t t) const {
    if (n_ == 0) {
        return 0;
    }
    if (t <= (int64_t)t_[0]) {
        return 0;
    }
    if (t >= (int64_t)t_[n_ - 1]) {
        return n_ - 1;
    }
    const uint64_t ut = (uint64_t)t;
    size_t lo = 0, hi = n_ - 1;
    while (hi - lo > 1) {
        const size_t mid = lo + (hi - lo) / 2;
        if (t_[mid] <= ut) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

void Transport::Reset(const TickTable& ticks, const TransportTuning& tuning) {
    ticks_ = ticks;
    tuning_ = tuning;
    index_ = 0;
    acc_us_ = 0.0;
    rate_ = 1.0;
    paused_ = true;
    disc_pending_ = false;
    disc_cause_ = Cause::Seek;
    micro_seeks_ = steps_total_ = 0;
    gaps_crossed_ = largest_step_us_ = 0;
    table_regressions_ = zero_dt_steps_ = 0;
    end_clamps_ = 0;
    // A span belongs to a file, so a Reset for the next one must not inherit it. The caller states
    // it after Reset; until it does, nothing is clamped.
    span_set_ = false;
    span_first_us_ = span_last_us_ = 0;
    wall_clamps_ = wall_dropped_us_ = coverage_holds_ = 0;
    retable_jumps_ = 0;
    // A file's edges belong to that file; the caller restates them with the first window.
    table_at_file_start_ = table_at_file_end_ = false;
    end_state_ = EndState::None;
    NoteDisplay();
}

void Transport::Retable(const TickTable& ticks) {
    // display_us_, never At(index_) + acc_: the caller has already refilled the storage the old
    // table pointed at, so the index is stale and reading through it is what teleported the
    // playhead one whole chapter per rebuild.
    const int64_t held = display_us_;
    ticks_ = ticks;
    SnapToTarget(held);
    // ★★★ A TABLE THAT DOES NOT COVER THE HELD TIME HAS MOVED THE PLAYHEAD, and a moved playhead is
    // a jump whoever caused it. The header's "the position in time does not change" holds only while
    // the new table spans the held instant; when it does not, SnapToTarget clamps to the new table's
    // edge -- measured 1.000 s -> 50.000 s -- and staying silent about it left the caller's cursors
    // pointing into the old span, the synthetic owners unreleased and the residue battery unfired,
    // which is the one failure the module describes as "audio quietly wrong for the rest of the
    // session with nothing failing anywhere".
    //
    // The transport cannot hold a time its table does not contain -- index_ must name a real row --
    // so the fix here is to REPORT the move, not to refuse it. Not moving the playhead at all is the
    // caller's contract: it centres the window on the target before handing the table over.
    if (display_us_ != held) {
        // ★ PARKED AT AN END OF THE RECORDING IS NOT A JUMP.  There the display is clamped to the
        // FILE span, which the resident table need not reach, so every rebuild snaps it back to the
        // table's edge and the next Advance clamps it out again -- a move that reports a Seek,
        // fires the whole reset battery, releases the synthetic owners and asks for a preroll, for
        // a playhead that never left.  The held instant is put back into the accumulator, which is
        // exactly the state ClampDisplayToFileSpan leaves at an end, and nothing is reported.
        // ★ ...AND ONLY A RE-CLAMP IS THAT.  Parked is not by itself a reason
        // to stay silent: a rebuild that re-centres the resident table under a held playhead moves
        // it for real, in either direction and by any amount, and the caller's cursors, the
        // synthetic owners and the preroll all depend on hearing about it.  A re-clamp is the
        // narrow shape: the held instant lies outside the new table ON THE PARKED SIDE and the
        // snap put the display on exactly that edge.  Anything else is reported like any other
        // retable jump.
        const bool reclamp =
            !ticks_.Empty() &&
            ((end_state_ == EndState::AtStart && held < (int64_t)ticks_.FirstUs() &&
              display_us_ == (int64_t)ticks_.FirstUs()) ||
             (end_state_ == EndState::AtEnd && held > (int64_t)ticks_.LastUs() &&
              display_us_ == (int64_t)ticks_.LastUs()));
#ifdef NAMES3_MUTATE_PARK_SUPPRESSES_ANY_MOVE
        const bool suppress = end_state_ != EndState::None;   // the pre-fix guard
        (void)reclamp;
#else
        const bool suppress = reclamp;
#endif
        if (suppress) {
            acc_us_ += (double)(held - display_us_);
            NoteDisplay();
            return;
        }
        ++retable_jumps_;
        end_state_ = EndState::None;
        if (!disc_pending_) {
            disc_from_ = held;
        }
        disc_to_ = display_us_;
        disc_cause_ = Cause::Seek;
        disc_pending_ = true;
    }
}

void Transport::SetFileSpan(int64_t first_us, int64_t last_us) {
    // A backwards span is not a span. Refused rather than swapped: a caller that computed its ends
    // the wrong way round has a defect, and silently fixing it hides the one thing worth reporting.
    if (last_us < first_us) {
        span_set_ = false;
        return;
    }
    span_first_us_ = first_us;
    span_last_us_ = last_us;
    span_set_ = true;
}

void Transport::ClampDisplayToFileSpan() {
    if (!span_set_) {
        return;
    }
    const int64_t d = ComputeDisplayUs();
    if (d < span_first_us_) {
        acc_us_ += (double)(span_first_us_ - d);
        ++end_clamps_;
    } else if (d > span_last_us_) {
        acc_us_ -= (double)(d - span_last_us_);
        ++end_clamps_;
    }
}

void Transport::ClampDisplayToTable() {
    if (ticks_.Count() == 0) {
        return;
    }
    const int64_t d = ComputeDisplayUs();
    const int64_t first = (int64_t)ticks_.FirstUs();
    const int64_t last = (int64_t)ticks_.LastUs();
    if (d < first) {
        acc_us_ += (double)(first - d);
    } else if (d > last) {
        acc_us_ -= (double)(d - last);
    }
}

void Transport::SetRate(double rate) {
    rate_ = rate;
    // A rate of zero is expressed as paused, never as a zero step. The two are different states in
    // this class because they are different states in the clock: pause is a flag, a zero step is +inf.
    if (rate == 0.0) {
        paused_ = true;
    }
}

int64_t Transport::SolverPlayheadUs() const {
    return ticks_.Count() ? (int64_t)ticks_.At(index_) : 0;
}

int64_t Transport::ComputeDisplayUs() const {
    const double d = (double)SolverPlayheadUs() + acc_us_;
    return (int64_t)llround(d);
}

// The cached value. Every mutator refreshes it while its own table is still the live one, so a
// reader between a window refill and Retable is answered the playhead it last actually had rather
// than a boundary out of somebody else's chapters.
int64_t Transport::DisplayPlayheadUs() const {
    return display_us_;
}

bool Transport::TakeDiscontinuity(int64_t& from_us, int64_t& to_us, Cause& cause) {
    if (!disc_pending_) {
        return false;
    }
    from_us = disc_from_;
    to_us = disc_to_;
    cause = disc_cause_;
    disc_pending_ = false;
    return true;
}

// Land on the recorded boundary at or below `target` and carry the remainder. This is the one place
// the solver playhead is allowed to move by anything other than a whole recorded tick, and it never
// leaves it off a boundary -- the remainder lives in acc_, which is why the fidelity property survives
// a micro-seek.
// The boundary at or below `t_us`. Count()==0 rather than Empty() is the guard on purpose: Empty()
// is `n < 2`, which asks whether the table can be STEPPED, and a single recorded sample is still a
// boundary the solver sits on.
bool Transport::TickAtOrBelow(int64_t t_us, int64_t* out_us) const {
    if (out_us == nullptr || ticks_.Count() == 0) {
        return false;
    }
    if (t_us < (int64_t)ticks_.FirstUs()) {
        return false;
    }
    *out_us = (int64_t)ticks_.At(ticks_.IndexAt(t_us));
    return true;
}

void Transport::SnapToTarget(int64_t target_us) {
    if (ticks_.Empty()) {
        index_ = 0;
        acc_us_ = 0.0;
        NoteDisplay();
        return;
    }
    if (target_us < (int64_t)ticks_.FirstUs()) {
        target_us = (int64_t)ticks_.FirstUs();   // E-P1: clamp to the first sample
    }
    if (target_us > (int64_t)ticks_.LastUs()) {
        target_us = (int64_t)ticks_.LastUs();    // E-P2: clamp to the last
    }
    index_ = ticks_.IndexAt(target_us);
    acc_us_ = (double)(target_us - (int64_t)ticks_.At(index_));
    NoteDisplay();
}

void Transport::SeekTo(int64_t t_us) {
    const int64_t from = DisplayPlayheadUs();
    SnapToTarget(t_us);
    // A seek is the answer to "ended": the playhead is wherever it was asked for, and the next
    // Advance decides for itself whether that is an end of the file.
    end_state_ = EndState::None;
    // Two seeks in one frame collapse into one discontinuity spanning both (E-P15's debounce): the
    // `from` of the first is kept, because that is what the residue owners must reconcile away from.
    if (!disc_pending_) {
        disc_from_ = from;
    }
    disc_to_ = DisplayPlayheadUs();
    disc_cause_ = Cause::Seek;
    disc_pending_ = true;
}

int Transport::StepTicks(int n) {
    if (ticks_.Empty() || n == 0) {
        return 0;
    }
    const int64_t from = DisplayPlayheadUs();
    const int sgn = n > 0 ? 1 : -1;
    int remaining = n > 0 ? n : -n;
    int moved = 0;
    // ★★★ THE SURPLUS IS THE FIRST STEP, NOT A FREE ONE (L3-RUN-4 S4).
    //
    // Between two recorded ticks the display playhead is At(index_) + acc_us_, and the tick nearest
    // it in the direction of travel is At(index_) itself whenever the accumulator points the other
    // way. Moving the index as well then travels a whole extra period: live, `step +1` moved one
    // tick and `step -1` moved two, because reverse leaves acc_us_ negative and forward leaves it
    // positive, so the backward arm paid the surplus AND the index. Discarding the surplus IS one
    // step, and it is counted as one.
    //
    // llround, not a raw compare, because that is exactly what DisplayPlayheadUs does: a surplus
    // that rounds to zero does not move the playhead, and counting it would be a step the operator
    // cannot see.
    const int64_t surplus = (int64_t)llround(acc_us_);
    if (sgn > 0 ? surplus < 0 : surplus > 0) {
        acc_us_ = 0.0;
        ++steps_total_;
        ++moved;
        --remaining;
    }
    while (remaining-- > 0) {
        if (sgn > 0) {
            if (index_ + 1 >= ticks_.Count()) {
                break;
            }
            ++index_;
        } else {
            if (index_ == 0) {
                break;
            }
            --index_;
        }
        ++steps_total_;
        ++moved;
    }
    // A step is exact by definition: it discards the accumulator rather than blending with it.
    acc_us_ = 0.0;
    NoteDisplay();
    if (moved != 0) {
        end_state_ = EndState::None;
    }
    if (!disc_pending_) {
        disc_from_ = from;
    }
    disc_to_ = DisplayPlayheadUs();
    disc_cause_ = Cause::Seek;
    disc_pending_ = true;
    return sgn * moved;
}

StepResult Transport::Advance(uint64_t wall_dt_us) {
    StepResult r;
    if (ticks_.Empty()) {
        r.commanded_step_us = 1;
        return r;
    }

    if (paused_ || rate_ == 0.0) {
        // S7.3.3: playhead frozen, external step of 1 us, floor 1. Not timescale 0, not a zero step.
        r.display_playhead_us = DisplayPlayheadUs();
        r.solver_playhead_us = SolverPlayheadUs();
        r.commanded_step_us = 1;
        r.end_state = end_state_;
        return r;
    }

    // Running again, so whatever end it was parked on it is no longer parked on. Re-decided below
    // from this frame's own direction of travel.
    end_state_ = EndState::None;

    const int sgn = rate_ >= 0.0 ? 1 : -1;

    // ★★★ A HITCH IS NOT SCENE TIME OWED TO THE VIEWER. The accumulator used to take wall_dt whole
    // and pay it back afterwards; what that buys is a replay that sprints through exactly the
    // seconds the watcher was waiting on. The excess is dropped here and counted, so the playhead
    // advances by `rate * REAL time, clamped` and a stall resumes where it stopped.
    if (wall_dt_us > tuning_.max_wall_dt_us) {
        r.wall_dropped_us = wall_dt_us - tuning_.max_wall_dt_us;
        wall_dropped_us_ += r.wall_dropped_us;
        ++wall_clamps_;
        wall_dt_us = tuning_.max_wall_dt_us;
    }
    acc_us_ += rate_ * (double)wall_dt_us;

    uint64_t last_step = 0;
    bool at_table_end = false;
    while (true) {
        // The tick the solver would cross next, in the direction of travel. Classified BEFORE its
        // magnitude is used, because the classification is what unsigned arithmetic would destroy.
        uint64_t d = 0;
        StepKind kind;
        if (sgn > 0) {
            if (index_ + 1 >= ticks_.Count()) {
                at_table_end = true;
                break;   // E-P2: at the end; the caller auto-pauses
            }
            kind = ClassifyStep(ticks_.At(index_), ticks_.At(index_ + 1), tuning_.max_step_us, d);
        } else {
            if (index_ == 0) {
                at_table_end = true;
                break;   // E-P1: at the start
            }
            kind = ClassifyStep(ticks_.At(index_ - 1), ticks_.At(index_), tuning_.max_step_us, d);
        }

        // ★★★ EQUAL IS A STEP OF ZERO, AND IT MUST STILL MOVE THE INDEX. Duplicate stamps are
        // legal -- the resolver has a case for them -- and the previous code BROKE the loop on d == 0,
        // so the index never advanced past the pair and playback froze there permanently, every frame,
        // with the transport reporting a healthy 1 us idle step. A stalled reading is a real reading;
        // what must not happen is COMMANDING a zero, because that is the +inf hazard.
        if (kind == StepKind::ZeroDt) {
            index_ = (sgn > 0) ? index_ + 1 : index_ - 1;
            ++zero_dt_steps_;
            ++steps_total_;
            continue;
        }

        if (kind == StepKind::Gap || kind == StepKind::Regressed) {
            if (kind == StepKind::Regressed) {
                // Same remedy, different finding. Crossing is right either way; calling a corrupt
                // table a capture gap is not, and it is what an unsigned difference would have forced.
                ++table_regressions_;
            }
            const int64_t from = ComputeDisplayUs();
            index_ = (sgn > 0) ? index_ + 1 : index_ - 1;
            acc_us_ = 0.0;              // the gap consumed whatever deficit was outstanding
            ++gaps_crossed_;
            r.crossed_gap = true;
            r.micro_seek = true;
            if (!disc_pending_) {
                disc_from_ = from;
            }
            disc_to_ = ComputeDisplayUs();
            disc_cause_ = Cause::GapCross;
            disc_pending_ = true;
            break;                      // one gap per frame; the next frame resumes normally
        }

        if (fabs(acc_us_) < (double)d) {
            break;
        }

        // ★★★ SATURATED MEANS THE LOOP WANTED MORE, WHICH IS WHAT transport.h:366 SAYS AND WHAT THE
        // OLD `r.saturated = (steps_consumed == max_substeps)` DID NOT COMPUTE. That form was also
        // true of the healthy exit above with the counter sitting on K: the frame had spent its
        // time and had no remainder to jump, yet the caller micro-seeked to
        // `SolverPlayheadUs() + acc_us_`, which IS the display playhead -- a zero-distance jump that
        // still fired the whole category-N residue battery. Asked here, after affordability, the
        // answer is the question: there is a whole tick left and no budget to take it.
        if (r.steps_consumed >= tuning_.max_substeps) {
            r.saturated = true;
            break;
        }

        index_ = (sgn > 0) ? index_ + 1 : index_ - 1;
        acc_us_ -= (double)sgn * (double)d;   // F-1: SIGNED. The unsigned form makes reverse run away.
        last_step = d;
        ++r.steps_consumed;
        ++steps_total_;
    }

    if (r.steps_consumed == 0) {
        r.commanded_step_us = 1;
    } else {
        r.commanded_step_us = last_step;   // a recorded dt, and never longer than max_step_us
    }
    if (r.commanded_step_us > largest_step_us_) {
        largest_step_us_ = r.commanded_step_us;
    }

    // ★★★ COVERAGE LAGGING THE PLAYHEAD IS A REASON TO WAIT. The loop ran out of table in the
    // direction of travel: the next instants are in a chapter nobody has decoded yet. Jumping the
    // surplus past that edge skips a span the viewer asked to watch, so the playhead holds there
    // and the rebuild that follows lets it carry on from the same instant. Checked BEFORE the
    // catch-up micro-seek, because "there was nowhere to step" is not "the rate needs help".
    if (at_table_end) {
        ClampDisplayToTable();
        // ★★★ THE END OF THE RECORDING IS ASKED FIRST, because it is the stronger statement about
        // the same edge. A window that reaches the file's last tick has nothing left to decode, so
        // calling that "coverage is behind" reported a finished replay as a stalled one -- one hold
        // per frame, forever, with EndClamps stuck at zero and nothing ever pausing. The recording's
        // ends are a state: the playhead clamps there and the transport pauses itself, which is what
        // a demo player does and what the caller was already documented as doing and did not.
        const int64_t here = ComputeDisplayUs();
        // ★ THE TABLE'S OWN EDGE COUNTS WHEN IT IS THE FILE'S EDGE.  The span is stated from the
        // manifest origin, which can precede the first recorded TICK, so at the front the span test
        // alone never fires: reverse play reaching the first tick reported "coverage is behind"
        // every frame, forever, with EndClamps at zero and nothing ever pausing -- the exact
        // asymmetry the back end does not have.  Whether this table is the file's edge is the
        // caller's fact, because it owns the chapters and this class owns one table at a time.
        const bool at_file_edge =
            (span_set_ && (sgn > 0 ? here >= span_last_us_ : here <= span_first_us_)) ||
            (sgn > 0 ? table_at_file_end_ : table_at_file_start_);
        if (at_file_edge) {
            acc_us_ = 0.0;   // the surplus has nowhere to spend itself; dropped once, here
            end_state_ = sgn > 0 ? EndState::AtEnd : EndState::AtStart;
            paused_ = true;
            ++end_clamps_;
        } else {
            r.held_for_coverage = true;
            ++coverage_holds_;
        }
    } else if (r.saturated || fabs(acc_us_) > tuning_.catchup_limit_us) {
        // Above the ceiling the remainder is jumped rather than stepped. S7.5.2: this is not a
        // graceful degradation, it is what makes the rate invariant hold above K at all.
        const int64_t from = ComputeDisplayUs();
        const int64_t target = (int64_t)llround((double)SolverPlayheadUs() + acc_us_);
        SnapToTarget(target);
        r.micro_seek = true;
        ++micro_seeks_;
        if (!disc_pending_) {
            disc_from_ = from;
        }
        disc_to_ = ComputeDisplayUs();
        // A gap already claimed this frame's discontinuity and is the stronger statement about it.
        if (disc_cause_ != Cause::GapCross) {
            disc_cause_ = Cause::MicroSeek;
        }
        disc_pending_ = true;
    }

    // ★★ THE SURPLUS AT AN END OF THE FILE HAS NOWHERE TO GO. The accumulator never discards wall
    // time -- that is what makes a hitch survive -- but at the first or last recorded tick of the
    // RECORDING there is no tick to spend it on, and carrying it puts the display playhead outside
    // the file: negative at the front, past the duration at the back. Both are positions the panel,
    // the applier and every row that reads a playhead have no meaning for. Dropped to the edge and
    // counted, so nothing about it is silent.
    ClampDisplayToFileSpan();
    NoteDisplay();

    r.end_state = end_state_;
    r.display_playhead_us = DisplayPlayheadUs();
    r.solver_playhead_us = SolverPlayheadUs();
    return r;
}

}  // namespace playback
}  // namespace replay
