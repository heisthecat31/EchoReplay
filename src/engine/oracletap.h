#pragma once

// E-7 `oracletap`, owned by W2-I. S5.4.7, S12.3.
//
// "In-process invocation of the /session and /player_bones builders from one tick, no HTTP, no skew."
//
// --------------------------------------------------------------------------------------------
// ★ WHY THIS EXISTS AT ALL, AND WHAT IT IS ACTUALLY GUARANTEEING
//
// R1 found that `.echoreplay` fetches /session and /player_bones as two concurrent HTTP GETs stamped
// with ONE timestamp, so its body pose and bone pose come from DIFFERENT simulation instants and
// nothing in the file says so.  Every fidelity number computed from such a pair is measuring the gap
// between two ticks as though it were an error in the replay.
//
// ★ THE OBVIOUS FIX IS A TAUTOLOGY.  Stamping once and writing that stamp into both snapshots makes
// "both snapshots carry the identical sim_time_us" true by construction, unfalsifiable, and worth
// nothing -- it is the same shape as the fidelity column this project shipped that compared each
// engine step against the tick it had just been assigned from.  No mutation can reach an assertion
// over one copied value.
//
// So the tap BRACKETS.  It reads the engine's clock and the engine's frame counter before the first
// builder and again after the second, and publishes all four numbers.  O3 asserts they are equal.
// That assertion CAN fail -- a tick boundary between the two calls moves both -- and tests/verify
// plants exactly that, in both domains, as the positive control.
//
// Two domains and not one, on purpose: CClock+0x28 accumulates on every heartbeat (three per frame,
// two of them after our tick -- W1-D), while CncaGame+0x830 increments once per rendered frame.  A
// clock that stalls and a frame counter that stalls are different faults, and one reading cannot tell
// them apart.
//
// ★ THE RESIDUAL, NAMED HERE RATHER THAN DISCOVERED LATER.  The two builders' ADDRESSES are pinned
// and byte-guarded (ORACLE.SESSION_BUILDER, ORACLE.BONES_BUILDER and their prologues); their
// PROTOTYPES are not.  Both are called with engine-constructed argument objects whose layout no
// offline read establishes, and inventing a prototype for a call into someone's running game is the
// one experiment this packet will not run blind.  The builders therefore reach the tap as INJECTED
// function pointers: nothing here calls an address it guessed the shape of, the bracket is fully
// exercised offline against stubs, and Bind() refuses any site whose prologue does not verify.
// Promoting the prototype is integration item V-I-W2I1.
// --------------------------------------------------------------------------------------------

#include <stdint.h>
#include <stddef.h>

namespace replay {

// What the tap could do this tick.  "It did not work" is not a diagnosis, so every refusal that can
// happen has its own value, in the manner of tick::ClaimResult and entity::Resolve.
enum class TapStatus : uint8_t {
    Ok = 0,
    NoClock,              // the engine clock was not readable; nothing was invoked
    NotBound,             // no builders bound: V-I-W2I1 is open and the tap says so rather than guessing
    SiteRefused,          // a builder site's 20-byte prologue did not match its fact
    SessionBuilderFailed,
    BonesBuilderFailed,
    Skewed,               // the bracket moved: the two payloads are NOT one instant
};
const char* TapStatusName(TapStatus);

// The bracket, published in full.  O3 asserts before == after in both domains; it is never handed a
// single stamp to compare against itself.
struct OracleBracket {
    uint64_t simTimeBeforeUs;
    uint64_t simTimeAfterUs;
    uint32_t frameIndexBefore;
    uint32_t frameIndexAfter;
};

struct OracleSample {
    TapStatus     status;
    OracleBracket bracket;
    // Borrowed views of whatever the builders wrote into the caller's buffers.  Valid only until the
    // next Sample() on this tap.
    const uint8_t* sessionBytes;
    uint32_t       sessionLen;
    const uint8_t* bonesBytes;
    uint32_t       bonesLen;
};

// A builder writes its payload into `dst` and returns the byte count, or 0 on failure.  This is the
// seam the engine sites plug into once V-I-W2I1 promotes their prototypes, and the seam a test drives
// offline today.
using BuilderFn = uint32_t (*)(void* ctx, uint8_t* dst, uint32_t cap);

// Reads CClock+0x28 and CncaGame+0x830.  Defaults to W1-D's tick::ReadEngineClock; injectable so the
// bracket's own logic is testable with no engine.
using ClockFn = bool (*)(uint64_t* simTimeUs, uint32_t* frameIndex);

class IOracleTap {
public:
    // MUST be called from the engine tick.  Everything the no-skew claim rests on is the absence of a
    // frame boundary between the two builder calls, and that is only true on the tick thread.
    virtual bool Sample(OracleSample& out) = 0;

    virtual TapStatus   Status() const = 0;
    virtual const char* Diagnostic() const = 0;

protected:
    ~IOracleTap() = default;
};

namespace oracletap {

// Verifies both builder sites against their recorded prologues.  Returns false and leaves the tap
// unbound -- Sample() then reports NotBound -- when either site disagrees, which is also how another
// mod's detour on the same site is detected rather than called through.
bool VerifySites(char* why, size_t whyCap);

// The two fact-registered sites, exposed so a probe can report what it checked rather than assert it.
size_t      SiteCount();
const char* SiteFactId(size_t i);
uint64_t    SiteAddress(size_t i);

IOracleTap* Create(uint8_t* sessionBuf, uint32_t sessionCap, uint8_t* bonesBuf, uint32_t bonesCap);
void        Destroy(IOracleTap*);

// Both must be supplied before Sample() will invoke anything; binding one is NotBound, not half a
// sample.  `Create` starts unbound on purpose (see the residual note above).
void BindBuilders(IOracleTap*, BuilderFn session, void* sessionCtx, BuilderFn bones, void* bonesCtx);
void BindClock(IOracleTap*, ClockFn);

}  // namespace oracletap
}  // namespace replay
