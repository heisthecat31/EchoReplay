#pragma once

// W2-I. The shared vocabulary of the O2 and O3 oracles.
//
// ============================================================================================
// THE THREE RULES THIS HEADER EXISTS TO MAKE STRUCTURAL
//
// 1. A VERDICT IS NEVER THE ABSENCE OF A FAILURE.  Every result carries a denominator -- how
//    many bones, how many fields, how many instants were actually compared -- and Pass is
//    unreachable while that denominator is zero.  S12.0 RULE 0e: "a check that parses a set
//    asserts the set is non-empty before judging it."  An oracle that compared nothing reports
//    NotRun with a reason, never Pass.
//
// 2. NOT-RUN IS NOT FAIL.  A mismatched camera, a starved machine, two snapshots from different
//    instants -- none of those are evidence against the change under test, and reporting them as
//    Fail is how a gate gets weakened by whoever needs it green (S12.10.2 item 5).  Every refusal
//    carries a NotRunReason, and the reason is in the record.
//
// 3. THE TWO SIDES MUST BE ABLE TO DISAGREE.  This project has already shipped a fidelity column
//    that compared each engine step against the tick it had just been assigned from and reported
//    "100%, every cell".  Both comparators here therefore REFUSE when the two sides alias one
//    buffer, and REFUSE when they carry one SourceTag.  Neither refusal is advice; both are
//    NotRun, and tests/verify plants both.
// ============================================================================================

#include <stdint.h>
#include <stddef.h>

namespace replay {
namespace verify {

enum class Verdict : uint8_t { NotRun = 0, Pass, Fail };
const char* VerdictName(Verdict);

// Why an oracle declined to return a verdict.  "it did not run" is not a diagnosis, which is the
// same standard entity::Resolve and tick::ClaimResult hold themselves to.
enum class NotRunReason : uint8_t {
    None = 0,
    NoSamples,        // nothing was compared; the denominator is zero
    SameSource,       // both sides carry one SourceTag: they cannot disagree
    AliasedBuffers,   // both sides point into one allocation
    Skew,             // the two snapshots did not come from one engine instant
    ShapeMismatch,    // the player sets, or the bone counts, differ between the sides
    ClockDrift,       // source time and sim time disagree: the producer integrated the wrong delta
    Unavailable,      // the engine side never resolved (IEntity::Available() false)
    NoBaseline,       // a measurement offered without the baseline it is judged against
};
const char* NotRunReasonName(NotRunReason);

// A token minted by whatever produced a set of samples.  Two sets that came from one producer
// carry one tag, and an oracle handed two equal tags refuses: comparing a thing to itself is the
// single most common way an oracle in this project has reported a false green.
//
// Zero is deliberately not a valid tag, so a default-constructed stream refuses rather than
// comparing.  MintSourceTag() is the only way to obtain one.
struct SourceTag {
    uint64_t v;
};
bool SourceTagValid(SourceTag);
SourceTag MintSourceTag(const char* producer, uint64_t nonce);

}  // namespace verify
}  // namespace replay
