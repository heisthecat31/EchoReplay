#pragma once

// Decoding the NEXT sample window off the tick thread.
//
// ---------------------------------------------------------------------------------------------
// ★★★ THE DEFECT THIS EXISTS TO REMOVE. A window is three chapters, and building one decodes all
// three: measured at 9-13 ms. Until now that ran inside the tick, on the frame the playhead crossed
// out of the centre chapter -- so steady playback dropped one frame every two seconds of source
// time, and the slower the rate the more source seconds a viewer sat through per hitch. The owner
// saw it as "at slower playback speeds theres a weird hitching".
//
// ★★ WHY A WORKER AND NOT A SMALLER WINDOW. The decode cost is the file's, not the window's: a
// one-chapter window hitches a third as long three times as often, which is the same milliseconds
// spent and more seams to get wrong. The only way a boundary costs nothing is for the material to
// already be decoded when the playhead arrives.
//
// ★★ WHAT THIS MODULE IS NOT. It does not own windows, it does not adopt anything and it knows
// nothing about the playhead. It is a one-deep request/result channel with its own reader and its
// own decode scratch. The caller owns every Window object, hands one over as a BORROWED buffer and
// gets it back from Collect; between those two calls the caller must not touch it. That rule is
// the whole synchronisation contract, and it is why nothing here takes a lock on the frame path.
//
// ★ THE THREE STRUCTURAL REASONS THIS COULD NOT SIMPLY BE A std::async CALL, all found before a
// line of it was written:
//   1. BuildWindowRange decoded through a function-static ~860 KB accumulator. Two threads inside
//      it corrupted each other silently. Fixed at source: windowbuild.h's BuildScratch, one per
//      thread, and the tick path kept its static.
//   2. DemoReader holds ONE file handle with a stateful cursor and reusable decode buffers, and a
//      documented depth-2 view guarantee. It cannot be shared. This owns a SECOND reader, opened on
//      the same path, closed on the worker's side of the join.
//   3. Window is non-copyable and non-movable and holds megabytes. Hence borrowed buffers rather
//      than results that carry their own storage.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include "playback/core/window.h"
#include "playback/core/windowbuild.h"

namespace replay {
namespace playback {
namespace winprefetch {

// What a finished decode carries back. `window` is the buffer the caller lent, returned.
struct Result {
    Window* window = nullptr;
    BuildStats stats;
    size_t centre = 0;
    uint32_t load_serial = 0;
    uint64_t request_id = 0;
    bool built = false;      // false means BuildWindowRange refused; stats.refusal names it
    uint64_t decode_us = 0;  // wall time the worker spent inside the decode
};

// Everything the 1 Hz operator line reports. Plain counters; read on the tick thread.
struct Counters {
    uint64_t requests = 0;    // accepted by Request
    uint64_t coalesced = 0;   // replaced a queued request the worker had not started yet
    uint64_t decodes = 0;     // decodes the worker actually ran
    uint64_t refused = 0;     // of those, the ones BuildWindowRange returned false for
    uint64_t collected = 0;   // results the caller took out
    uint64_t decode_us = 0;   // total worker time inside the decode
};

// Opens a second reader on `path` and starts the worker parked on its condition variable.
// `clocks` and `primer` are borrowed for the life of the worker and must not change while it runs
// -- both are load-time scans that never do. Idempotent: a second call while running is refused
// without disturbing the first. `why` names the refusal when it returns false.
bool Start(const wchar_t* path, const WireClocks* clocks, const NetVarPrimer* primer,
           const char** why = nullptr);

bool Running();

// Cancels, joins, closes the reader and frees the scratch. Returns false only if the worker did not
// finish inside `wait_ms`, in which case NOTHING is freed and the worker keeps everything it may
// still touch -- including the borrowed buffer, which the caller must then not free either.
//
// ★★ MUST RUN BEFORE THE MODULE'S OWN READER IS CLOSED AND BEFORE ANY LENT WINDOW IS DESTROYED.
// The worker's reader is its own, but the buffer and the clocks are the caller's.
bool Stop(unsigned wait_ms);

// The borrowed buffer the worker decodes into. Refused while it already holds one. After this
// returns true the caller MUST NOT touch `w` until Collect hands it back (or Stop returns true).
bool GiveBuffer(Window* w);
bool HasBuffer();

// Posts the request, or replaces a queued one that has not started (latest-wins coalescing, so a
// seek storm costs one decode rather than one per step). Refused when there is no buffer, when a
// finished result has not been collected, or when the worker is not running.
//
// NOT a per-frame call: the caller posts when the wanted centre CHANGES, not every tick.
bool Request(size_t centre, uint32_t load_serial);

// True while a request is queued or running. Nothing to collect yet.
bool Busy();

// Non-blocking. True when a finished decode was taken out; `out.window` is the buffer, and the
// worker holds nothing afterwards. One atomic acquire-load on the frame path, no lock.
bool Collect(Result& out);

Counters ReadCounters();

// Test seam: forgets every counter. Only legal while stopped.
void ResetCountersForTest();

// Test seam: the NEXT Stop joins the worker as it always does and then reports the timeout anyway.
// What the caller owes a join that did not finish -- abandoning every buffer the worker could still
// be inside -- is a module-level rule with no other way to reach it, and hanging a real worker for
// five seconds to reach it would test the harness rather than the rule. Self-clearing.
void FailNextStopForTest();

}  // namespace winprefetch
}  // namespace playback
}  // namespace replay
