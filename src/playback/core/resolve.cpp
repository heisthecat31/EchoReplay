#include "playback/core/resolve.h"

#include <string.h>

namespace replay {
namespace playback {

// The definition. Last index whose time is <= t. Times are u64 and t is signed because the playhead is
// signed (B.5's F-4: reverse into t0 underflows an unsigned form and the loop reads it as forward), so
// a negative t must resolve to kNoRecord rather than wrapping to "the very last record".
size_t ResolveBySearch(const uint64_t* times, size_t n, int64_t t) {
    if (n == 0 || t < 0) {
        return kNoRecord;
    }
    const uint64_t ut = (uint64_t)t;
    if (times[0] > ut) {
        return kNoRecord;
    }
    size_t lo = 0, hi = n;            // invariant: times[lo] <= ut, and hi is one past the candidates
    while (hi - lo > 1) {
        const size_t mid = lo + (hi - lo) / 2;
        if (times[mid] <= ut) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

size_t SeriesCursor::Resolve(const uint64_t* times, size_t n, int64_t t) {
    if (n == 0 || t < 0) {
        Invalidate();
        return kNoRecord;
    }
    const uint64_t ut = (uint64_t)t;

    if (!valid_) {
        at_ = ResolveBySearch(times, n, t);
        valid_ = true;
        ++searches_;
        return at_;
    }

    // Behind every record: the query precedes the series. Not a walk -- there is nowhere to walk to.
    if (times[0] > ut) {
        at_ = kNoRecord;
        return at_;
    }
    if (at_ == kNoRecord) {
        at_ = ResolveBySearch(times, n, t);
        ++searches_;
        return at_;
    }

    size_t i = at_;
    size_t steps = 0;

    // Forward: advance while the NEXT record is still at or before t.
    while (i + 1 < n && times[i + 1] <= ut) {
        if (++steps > kWalkLimit) {
            at_ = ResolveBySearch(times, n, t);
            ++searches_;
            return at_;
        }
        ++i;
    }
    // Backward -- the branch ForwardOnlyCursorMutant omits. Retreat while THIS record is after t.
    while (times[i] > ut) {
        if (++steps > kWalkLimit) {
            at_ = ResolveBySearch(times, n, t);
            ++searches_;
            return at_;
        }
        if (i == 0) {
            at_ = kNoRecord;   // unreachable given the times[0] > ut guard above; kept as a hard floor
            return at_;
        }
        --i;
    }

    if (steps) {
        ++walks_;
        steps_ += steps;
    }
    at_ = i;
    return at_;
}

// The mutant. Everything above except the retreat loop.
size_t ForwardOnlyCursorMutant::Resolve(const uint64_t* times, size_t n, int64_t t) {
    if (n == 0 || t < 0) {
        Invalidate();
        return kNoRecord;
    }
    const uint64_t ut = (uint64_t)t;
    if (!valid_) {
        at_ = ResolveBySearch(times, n, t);
        valid_ = true;
        return at_;
    }
    if (times[0] > ut) {
        at_ = kNoRecord;
        return at_;
    }
    if (at_ == kNoRecord) {
        at_ = ResolveBySearch(times, n, t);
        return at_;
    }
    size_t i = at_;
    while (i + 1 < n && times[i + 1] <= ut) {
        ++i;
    }
    at_ = i;
    return at_;
}

// ---------------------------------------------------------------------------------------------

size_t KeyedResolver::IndexOf(uint64_t key) const {
    for (size_t i = 0; i < key_count_; ++i) {
        if (keys_[i] == key) {
            return i;
        }
    }
    return kNoRecord;
}

bool KeyedResolver::Build(const KeyedRecord* seeds, size_t seed_count,
                          const KeyedRecord* records, size_t record_count) {
    key_count_ = 0;
    record_count_ = 0;
    overflow_ = 0;
    memset(keys_, 0, sizeof keys_);
    for (size_t i = 0; i < kMaxKeys; ++i) {
        series_[i] = Series();
    }

    // Pass 1: the key set, seeds first so a key that only ever appears in the keyframe still exists.
    // A key we cannot hold is REFUSED, not dropped: a missing netvar is a silently wrong replay, and
    // "the file had more keys than we can carry" is a fact the caller must see.
    for (size_t pass = 0; pass < 2; ++pass) {
        const KeyedRecord* src = pass == 0 ? seeds : records;
        const size_t n = pass == 0 ? seed_count : record_count;
        for (size_t i = 0; i < n; ++i) {
            if (IndexOf(src[i].key) != kNoRecord) {
                continue;
            }
            if (key_count_ >= kMaxKeys) {
                ++overflow_;
                return false;
            }
            keys_[key_count_] = src[i].key;
            series_[key_count_].key = src[i].key;
            ++key_count_;
        }
    }

    // Pass 2: count per key, then lay out contiguous runs, then fill. Two passes rather than a
    // per-key vector because this runs on the tick thread's window build and S6.4 forbids allocation.
    size_t counts[kMaxKeys] = {};
    for (size_t i = 0; i < seed_count; ++i) {
        ++counts[IndexOf(seeds[i].key)];
    }
    for (size_t i = 0; i < record_count; ++i) {
        ++counts[IndexOf(records[i].key)];
    }

    size_t at = 0;
    for (size_t k = 0; k < key_count_; ++k) {
        series_[k].first = at;
        series_[k].count = 0;
        at += counts[k];
    }
    if (at > kMaxRecords) {
        ++overflow_;
        return false;
    }
    record_count_ = at;

    // Seeds carry the keyframe value; they sort before every in-chapter record by construction, and a
    // seed whose stamp is later than an in-chapter record would break the non-decreasing invariant the
    // cursor depends on -- so it is clamped to the first record's time rather than trusted.
    for (size_t pass = 0; pass < 2; ++pass) {
        const KeyedRecord* src = pass == 0 ? seeds : records;
        const size_t n = pass == 0 ? seed_count : record_count;
        for (size_t i = 0; i < n; ++i) {
            const size_t k = IndexOf(src[i].key);
            Series& s = series_[k];
            const size_t dst = s.first + s.count;
            uint64_t t = src[i].t_us;
            if (s.count && t < times_[dst - 1]) {
                t = times_[dst - 1];   // never let a series go backwards; the cursor's precondition
            }
            times_[dst] = t;
            values_[dst] = src[i].value;
            ++s.count;
        }
    }

    InvalidateCursors();
    return true;
}

void KeyedResolver::InvalidateCursors() {
    for (size_t i = 0; i < key_count_; ++i) {
        series_[i].cursor.Invalidate();
    }
}

bool KeyedResolver::ValueAt(uint64_t key, int64_t t, uint64_t& out) {
    const size_t k = IndexOf(key);
    if (k == kNoRecord) {
        return false;
    }
    Series& s = series_[k];
    const size_t i = s.cursor.Resolve(times_ + s.first, s.count, t);
    if (i == kNoRecord) {
        return false;
    }
    out = values_[s.first + i];
    return true;
}

bool KeyedResolver::FirstValue(uint64_t key, uint64_t& out) const {
    const size_t k = IndexOf(key);
    if (k == kNoRecord || series_[k].count == 0) {
        return false;
    }
    out = values_[series_[k].first];
    return true;
}

bool KeyedResolver::ValueAtBySearch(uint64_t key, int64_t t, uint64_t& out) const {
    const size_t k = IndexOf(key);
    if (k == kNoRecord) {
        return false;
    }
    const Series& s = series_[k];
    const size_t i = ResolveBySearch(times_ + s.first, s.count, t);
    if (i == kNoRecord) {
        return false;
    }
    out = values_[s.first + i];
    return true;
}

}  // namespace playback
}  // namespace replay
