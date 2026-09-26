#pragma once

// W1-E. The verdict WORD for probe_entitymatch, in one place so the offline suite can run it
// against the counters launch 07 actually recorded.
//
// ---------------------------------------------------------------------------------------------
// ★ WHY THIS IS A FUNCTION AND NOT `g.failed ? "FAIL" : "PASS"`.
//
// The probe kept an `undetermined` counter, printed it into done.json, and then decided its verdict
// with that two-state expression at FIVE sites, none of which read the counter. On launch 07 the
// probe recorded "CR15Game resolved but its CR15NetGame is null -- no match", wrote the sentence
// "a probe that reported PASS here would be reporting its own silence", and filed PASS with
// checked: 6. It reported the absence of the only thing it exists to look at as a green.
//
// Four outcomes do not fit in two states: asked and passed, asked and failed, could not ask, never
// asked. They fit here, and tests/entity/test_entity.cpp feeds this function launch 07's own
// numbers so the fix is proven against the artifact rather than asserted over it.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>

namespace replay {
namespace probeverdict {

// FAIL                  a scored check answered NO.
// COULD-NOT-DETERMINE   a read this probe needed could not be taken, or the run stopped before its
//                       last check, so some of its questions were never put at all.
// NOT-RUN               nothing scored.
// PASS                  every question was asked, to the end, and answered yes.
inline const char* Word(size_t ok, size_t failed, size_t undetermined, bool reachedEnd) {
    if (failed != 0) return "FAIL";
    if (ok == 0 && undetermined == 0) return "NOT-RUN";
    if (undetermined != 0 || !reachedEnd) return "COULD-NOT-DETERMINE";
    return "PASS";
}

}  // namespace probeverdict
}  // namespace replay
