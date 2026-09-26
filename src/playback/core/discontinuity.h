#pragma once

// W2-B. The seams a playhead jump crosses, and the registry the category-N battery lives in.
//
// ---------------------------------------------------------------------------------------------
// ★ WHY THIS HEADER EXISTS AT ALL, which is the whole structural call of docs/spikes/W2-B.md S3.
//
// The design specifies seek as a nine-step PROCEDURE that applies keyframe state, fires a reset
// battery, rebuilds rings and pre-rolls -- running beside S7.1's nine-step frame, which also applies
// state every frame. Two code paths that must agree about "the complete state at time T" is a
// provenance fork, and when they drift the symptom is a replay that looks right when you play into a
// moment and wrong when you scrub to it. That is the single hardest bug class to attribute here, and
// it is exactly what the owner's "go to different points" asks us not to have.
//
// So there is no seek procedure. There is ONE ApplyStateAt(window, t, mode) -- used by linear play,
// scrub, reverse and fast-forward alike -- and a DISCONTINUITY, which is what a jump costs. The
// residue owners subscribe to the discontinuity; they are not a second way to apply state.
//
// ★ ApplyStateAt takes no Discontinuity parameter and never will. It is not that it ignores one: there
// is no channel through which it could learn a jump happened. That is INV-P made structural, and it is
// why path-independence is something an implementation has to WORK to violate rather than something a
// test has to catch after the fact.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

namespace replay {
namespace playback {

// Why the playhead moved discontinuously. Every cause is a real transport event, not an error path:
// ReverseTick fires on EVERY tick consumed backwards (the design -- reverse crosses the same
// boundaries a backwards seek does), and ChapterEpochRoll fires about every 2 s of source time.
enum class Cause : uint8_t {
    Seek = 0,            // a user seek, a round jump, -replayviewer entry
    MicroSeek,           // S7.5.2: above the sub-step ceiling K, the remainder is jumped
    GapCross,            // E-P3b: linear playback reached a gap the file does not contain
    ReverseTick,         // one recorded tick consumed backwards
    ChapterEpochRoll,    // the rebased time axis changed origin (W2-B.md S7.2)
    Count
};

const char* CauseName(Cause);

struct Discontinuity {
    int64_t from_us = 0;
    int64_t to_us = 0;
    Cause cause = Cause::Seek;
    // Only a user-visible jump earns the S7.4.5 pre-roll; ReverseTick and ChapterEpochRoll fire many
    // times a second and must not. W3-A owns what pre-roll DOES; this bit is who asks for it.
    bool needs_preroll = false;
};

// ★ Why a primitive reports instead of vanishing. Ten of the design's nineteen rows are
// LIBR15_VA with no PC mapping and W1-I owns that work. A battery that silently omitted an unmapped
// primitive would make "we could not map it" and "it is not needed" indistinguishable -- and the owner
// asked us to respect EVERYTHING, so he is entitled to know which parts a given build does not.
// S13.10's "a packet running out of road is a status, not a finding", expressed at runtime.
enum class Availability : uint8_t {
    Ready = 0,
    Unmapped,          // W1-I has not mapped the address yet
    RefusedByConfig,   // a ledger row the operator turned off
    FailedAtRuntime,   // it resolved, was called, and reported failure
    Count
};

const char* AvailabilityName(Availability);

class IResidue {
public:
    virtual const char* Id() const = 0;           // "N-5", "N-13", ... The design's row ids
    virtual Availability Avail() const = 0;
    // Never empty when Avail() != Ready. "it is unavailable" is not a diagnosis.
    virtual const char* Diagnostic() const = 0;
    virtual bool RunsOn(Cause) const = 0;
    virtual void Fire(const Discontinuity&) = 0;

protected:
    ~IResidue() = default;
};

// Fires in registration order, which is the design's "in table order". Holds borrowed
// pointers; primitives have static storage duration and are never deleted through this.
class ResidueBattery {
public:
    static const size_t kMax = 32;   // S7.4.3 has 19 rows; this is headroom, not a guess at the count

    bool Register(IResidue*);
    // Returns the number of primitives that ran. Ones whose Avail() != Ready are SKIPPED AND COUNTED,
    // never silently passed over.
    size_t Fire(const Discontinuity&);

    size_t Count() const { return count_; }
    IResidue* At(size_t i) const { return i < count_ ? items_[i] : nullptr; }

    // For ActiveModeLabel() and the UI: how many registered primitives are not Ready. A non-zero value
    // is a fidelity statement the user is shown, not a log line.
    size_t UnavailableCount() const;
    // Fills `out` with up to `cap` ids of the not-Ready primitives; returns how many exist.
    size_t UnavailableIds(const char** out, size_t cap) const;

    size_t FiredTotal() const { return fired_; }
    size_t SkippedTotal() const { return skipped_; }

private:
    IResidue* items_[kMax] = {};
    size_t count_ = 0;
    size_t fired_ = 0;
    size_t skipped_ = 0;
};

}  // namespace playback
}  // namespace replay
