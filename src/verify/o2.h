#pragma once

// W2-I.  O2 -- the state-replication oracle.  In-game, cheap, high frequency.
//
// After publishing state at the playhead, read the ENGINE's state back and compare it against what
// the file said.  Exact equality for values we publish.
//
// --------------------------------------------------------------------------------------------
// ★ THE HONEST SPLIT, AND IT IS THE WHOLE POINT OF THIS FILE
//
// S12.2 says plainly that for the publish path this comparison is "close to tautological".  It is:
// we write PlayerCI+60, then read PlayerCI+60 back, and of course it agrees.  An oracle that folds
// that into one percentage reports a number that is mostly self-agreement, which is the shape of the
// fidelity column this project already shipped that compared each engine step against the tick it
// had been assigned from and reported "100%, every cell".
//
// So O2 is scored per DOMAIN, and the domains are not equal:
//
//   Published   what we wrote and read straight back.  REPORTED, NEVER ASSERTED.  Its only value is
//               as a liveness witness -- if even this disagrees, the write did not land at all.
//   RenderSlot  PlayerCI+168, SYNTHESISED by the engine's own UpdateInterpolatedPlayerStates from
//   PhysSlot    PlayerCI+320, the same, on the physics-aligned ring.
//   PhysBody    CPhBody com/ori/vel/angVel, the engine's own physics state.
//
// The VERDICT is over the three engine-authored domains.  They are what catch the failures that
// actually happen: the owner gate rejecting our write, IsLocallyControlled still true so physics
// overwrote us, a net id that did not resolve, a body asleep and never woken, a ring the
// interpolator refused.  The two slots are scored SEPARATELY, so S12.2's "write +168 but not +320"
// mutation goes red on exactly one of them, which is also the proof that they are independently
// checked rather than summed.
//
// ★ WHAT O2 CANNOT SEE, stated here rather than discovered later.
//
//   1. A CAPTURE-SIDE ERROR.  If capture wrote the wrong value into the file and playback
//      publishes it faithfully, the engine reproduces it and every domain agrees.  O2 compares OUR
//      DECODE against THE RUNNING PROGRAM; it does not compare either against the match that was
//      played.  That is O3's job, and O5's.
//
//   2. A WRONG ENTITY PAIRING, which is S12.2's own "publish to the wrong entity index" mutation.
//      O2Compare is handed an expected side and an observed side already paired by the caller.  If
//      the caller published to entity A and read entity B back, this code compares A's file values
//      against B's engine values and reports whatever it finds -- a mismatch that reads as a
//      fidelity failure, or, for two players in similar poses, agreement.  `O2Sample::entity` is
//      carried into the report so a mismatch names the entity, but nothing here can check that the
//      pairing was right.  ★ THAT MUTATION IS THE CALLER'S TO DETECT, at the point where the
//      handle used to publish and the handle used to read back are both in scope, and W2-B's T2
//      harness owns it.  Recorded here so it is not counted as covered by this oracle.
// --------------------------------------------------------------------------------------------

#include "verify/verify.h"

namespace replay {
namespace verify {

enum class O2Domain : uint8_t {
    Published = 0,   // reported, never asserted
    RenderSlot,      // PlayerCI+168
    PhysSlot,        // PlayerCI+320
    PhysBody,        // CPhBody
    kCount,
};
const char* O2DomainName(O2Domain);
bool O2DomainIsAsserted(O2Domain);

// A named set of values, compared bit-exactly.  Bit-exact and not epsilon-equal on purpose: the
// claim is losslessness, and +0.0 against -0.0, or a NaN payload that changed, are real differences
// in a value we said we reproduced.
//
// The names are carried so a mismatch report can say which field moved.  A view whose names array is
// null reports indices instead, which is worse but never wrong.
struct StateView {
    const char* const* names;
    const float*       values;
    uint32_t           count;
};

// One tick of one entity.  `expected` comes from the decoder; `observed` from an IEntity read-back.
// A domain whose `valid` flag is false was not readable this tick and is counted as skipped, never
// as agreeing -- "we could not look" and "it matched" must not share an outcome.
struct O2DomainSample {
    StateView expected;
    StateView observed;
    bool      valid;
};

struct O2Sample {
    uint64_t       sourceTimeUs;
    uint32_t       entity;
    O2DomainSample domains[uint8_t(O2Domain::kCount)];
};

struct O2DomainResult {
    uint64_t compared;      // fields actually compared: the denominator for this domain
    uint64_t mismatched;
    uint32_t samplesValid;
    uint32_t samplesSkipped;
    char     firstMismatch[128];
};

struct O2Result {
    Verdict        verdict;
    NotRunReason   reason;
    uint32_t       samples;
    O2DomainResult byDomain[uint8_t(O2Domain::kCount)];
    uint64_t       assertedCompared;    // the denominator the verdict rests on
    uint64_t       assertedMismatched;
    char           detail[192];
};

struct O2Streams {
    SourceTag       fileTag;    // whoever decoded the file
    SourceTag       engineTag;  // whoever read the engine back
    const O2Sample* samples;
    uint32_t        count;
};

O2Result O2Compare(const O2Streams&);

}  // namespace verify
}  // namespace replay
