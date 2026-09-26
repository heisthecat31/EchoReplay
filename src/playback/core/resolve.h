#pragma once

// W2-B. Resolution: the value of a thing AT a playhead, in either direction.
//
// ---------------------------------------------------------------------------------------------
// ★ THE CHANGE THIS FILE IS, and it is the one that makes reverse and backward seek work.
//
// The design says "apply netvar writes and events whose replay time has passed since last
// frame". That is a FORWARD EDGE APPLICATION: it walks a stream and replays writes. It cannot reverse
// -- there is no "un-write" -- and after a backwards seek it leaves every netvar holding whatever the
// forward pass last wrote. It is packet playback's semantics wearing a state-capture coat, and the
// owner ruled packet playback out for exactly the properties it costs here.
//
// So a netvar is RESOLVED, never applied: value(name, slot, t) is the last write with stamp <= t,
// seeded from the keyframe. A resolved value has no direction, so seek, reverse and fast-forward are
// the same code as linear play, and the delegate edges the game needs still fire -- because
// EmitChangedDelegates diffs our published value against the previous one, and a diff of a resolved
// function is direction-agnostic.
//
// ★ AND THE CURSOR IS AN OPTIMISATION OF THAT FUNCTION, NOT THE DEFINITION OF IT. The definition is
// the binary search. SeriesCursor is an O(1)-amortised incremental evaluator of the same function, and
// tests/playback asserts they agree over random queries in random order INCLUDING strictly descending
// runs -- with the backward-rewind branch removed as a demonstrated red. A cursor is exactly the kind
// of carried state INV-P calls a cache, and it is invalidated on a discontinuity, never updated.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

namespace replay {
namespace playback {

// Returned when no record is at or before the query time -- i.e. the query precedes the first sample
// and there is no keyframe seed. A caller must branch on this rather than reading index 0, because
// "the earliest value" and "no value yet" are different answers and E-P1 depends on the difference.
static const size_t kNoRecord = (size_t)-1;

// The DEFINITION. Last index whose time is <= t, or kNoRecord. `times` must be non-decreasing.
size_t ResolveBySearch(const uint64_t* times, size_t n, int64_t t);

// The incremental evaluator of the same function.
//
// ★ Bidirectional by construction: forward playback advances, reverse retreats, and a seek invalidates.
// The walk threshold bounds the worst case -- past it the cursor falls back to the definition rather
// than walking, so a 4x fast-forward frame that crosses 30 records costs a search, not 30 compares.
class SeriesCursor {
public:
    // kWalkLimit is a COST bound, never a correctness one: both paths compute the same function, so a
    // wrong value here makes the resolver slower and never wrong. That is why it is a constant and not
    // a config key someone has to defend.
    static const size_t kWalkLimit = 8;

    void Invalidate() { valid_ = false; at_ = kNoRecord; }
    bool Valid() const { return valid_; }

    size_t Resolve(const uint64_t* times, size_t n, int64_t t);

    // Instrumentation for the gate, not for the hot path.
    uint64_t Walks() const { return walks_; }
    uint64_t Searches() const { return searches_; }
    uint64_t StepsWalked() const { return steps_; }

private:
    size_t at_ = kNoRecord;
    bool valid_ = false;
    uint64_t walks_ = 0, searches_ = 0, steps_ = 0;
};

// ★ THE MUTATION, COMPILED INTO THE SHIPPING TREE ON PURPOSE.
//
// Identical to SeriesCursor except that it never retreats: if the answer lies behind the cursor it
// keeps the cursor where it is. That is the single most plausible way to write this class if you only
// ever thought about forward playback, and it is silently correct for every forward query -- which is
// exactly why the gate needs it. A red arm that lives only in a comment has never been shown to be
// red, and this project has already shipped three checks whose outcome space did not contain the truth.
//
// It is never instantiated by src/playback outside a test. tests/playback asserts it AGREES with the
// definition on ascending queries and DIVERGES on descending ones; if it ever stopped diverging, the
// gate would have lost its discriminating power and the test says so by failing.
class ForwardOnlyCursorMutant {
public:
    void Invalidate() { at_ = kNoRecord; valid_ = false; }
    size_t Resolve(const uint64_t* times, size_t n, int64_t t);

private:
    size_t at_ = kNoRecord;
    bool valid_ = false;
};

// ---------------------------------------------------------------------------------------------
// A keyed set of series -- the shape S_NETVAR and S_WORLD both need.
//
// The file stores netvar writes as one time-ordered stream of {t, name, slot, value}. Answering "the
// value of name X at t" from that needs the last write to X at or before t, which one stream-wide
// cursor cannot give. So the window build demultiplexes into one series per key, each seeded with the
// keyframe's value at the chapter start. ~79 keys over a 2 s chapter, so the demux is cheap and the
// per-key cursor is what makes the per-frame cost O(keys) with no search in steady state.
// ---------------------------------------------------------------------------------------------

struct KeyedRecord {
    uint64_t t_us;
    uint64_t key;      // Symbol64 name, with the slot folded in by the caller
    uint64_t value;    // the raw SValue qword -- never re-encoded
};

class KeyedResolver {
public:
    static const size_t kMaxKeys = 128;   // 56 slot-0 + 23 slot-1 = 79, plus headroom

    // Demultiplexes `records` (time-sorted, any key order) into per-key series. `seeds` is the keyframe
    // shadow: one record per key at or before `t_begin`, so a query at the chapter start resolves
    // without reaching into the previous chapter. Returns false if the key count exceeds kMaxKeys --
    // and REFUSES rather than dropping keys, because a dropped netvar is a silently wrong replay.
    bool Build(const KeyedRecord* seeds, size_t seed_count,
               const KeyedRecord* records, size_t record_count);

    // Every cursor forgets where it was. Called on any Discontinuity: INV-P says a cache is
    // invalidated, never updated.
    void InvalidateCursors();

    size_t KeyCount() const { return key_count_; }
    uint64_t KeyAt(size_t i) const { return i < key_count_ ? keys_[i] : 0; }

    // The resolved value of `key` at `t`. False when the key is unknown or has no record at or before
    // t. Never returns a stale value on a false return -- `out` is untouched.
    bool ValueAt(uint64_t key, int64_t t, uint64_t& out);
    // Same function computed by the definition. The gate compares the two.
    bool ValueAtBySearch(uint64_t key, int64_t t, uint64_t& out) const;
    // The key's earliest value in this window, seed included. False when the window has none.
    bool FirstValue(uint64_t key, uint64_t& out) const;

    size_t Overflowed() const { return overflow_; }

private:
    struct Series {
        uint64_t key = 0;
        size_t first = 0, count = 0;   // into times_/values_
        SeriesCursor cursor;
    };

    size_t IndexOf(uint64_t key) const;

    static const size_t kMaxRecords = 8192;

    uint64_t keys_[kMaxKeys] = {};
    Series series_[kMaxKeys];
    size_t key_count_ = 0;

    uint64_t times_[kMaxRecords] = {};
    uint64_t values_[kMaxRecords] = {};
    size_t record_count_ = 0;
    size_t overflow_ = 0;
};

}  // namespace playback
}  // namespace replay
