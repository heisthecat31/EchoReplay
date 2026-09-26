#include "playback/residue/residue.h"

#include <new>
#include <string.h>
#include <type_traits>

#include "bootstrap/buildguard.h"
#include "bootstrap/services.h"
#include "engine/entity.h"
#include "playback/residue/residue_facts.h"

namespace replay {
namespace playback {
namespace residue {
namespace {

static const char* const kModule = "residue";

ResidueStats g_stats;
ResidueEnv g_env;

// The battery this packet was registered into. Held so Roster() can look the DESIGN's list up in
// the LIVE battery -- including W2-B's placeholder rows and W2-H's audio rows, which is the point:
// the roster reports the whole battery against the design, not this packet against itself.
ResidueBattery* g_battery = nullptr;

// Borrowed for the process lifetime, stashed by this directory's module at order 290 so that it is
// already here when W2-B arms at 300.
ReplayServices* g_services = nullptr;

IEntity* ArmedEntity() {
    return g_services ? g_services->entity : nullptr;
}

// The bind-epoch term of CsSpaceKey. AUD-F3D F3D-2: {netGame, space, readable} reads identically
// across a game space torn down and rebuilt at the SAME address, and the memo would then serve the
// freed image's component systems to four rows that write through them. This is the discriminator
// entity_module already maintains -- it moves on exactly the edge where SpaceIdentity's `entries`
// digest of the system table differs (entity_module.cpp:95-101, entity.h:310-317).
//
// The three IEntity answers are mapped by hand rather than cast: they are three states and the enum
// values on either side are free to move independently.
//
// Read live rather than at arm, because ArmedEntity() is only non-null once W1-E has resolved and
// the battery arms before that in the module order.
BindEpochTerm ArmedBindEpoch() {
    BindEpochTerm t;
    IEntity* e = ArmedEntity();
    if (!e) {
        return t;
    }
    uint64_t gen = 0;
    switch (e->BindEpochNow(gen)) {
        case IEntity::BindEpoch::Ok:
            t.state = BindEpochTerm::State::Bound;
            t.value = gen;
            break;
        case IEntity::BindEpoch::Unbound:
            t.state = BindEpochTerm::State::Unbound;
            break;
        case IEntity::BindEpoch::NotAvailable:
            break;
    }
    return t;
}

ClockLatch g_clockLatch;
bool g_originLatched = false;
unsigned char g_origin[kOriginSpanBytes] = {};

// Edge-triggered, so a micro-seek stream cannot flood the ring. A seek that resolves nothing must
// say so once; saying it sixty times a second turns a diagnosis into noise and hides the next one.
Bind g_loggedCsBind = Bind::Count;
Bind g_loggedPhysBind = Bind::Count;

// The causes a jump-repair primitive runs on.
//
// ★★ NOT ReverseTick, AND THE REASON IS NOT COST. S7.5.5's per-tick reverse subset is
// {N-5, N-13, N-16} and this packet serves none of the three. Reverse consumes one recorded tick at
// a time and ApplyStateAt re-installs the whole window every tick, so a body's history never goes
// stale under reverse -- there is no residue to clear. Running a wake sweep at tick rate would
// instead hold every body in the level permanently awake, which is a behaviour change rather than a
// repair.
//
// ★ NOT ChapterEpochRoll either. That is a change of ORIGIN on the time axis, not of position on it;
// no engine object this packet touches holds an absolute source time.
bool IsJump(Cause c) {
    return c == Cause::Seek || c == Cause::GapCross;
}

// MicroSeek is a real discontinuity, but S7.5.2 fires it EVERY FRAME above the sub-step ceiling. Only
// the idempotent, cheap rows take it: raising a request bit or collapsing a blend costs one store and
// converges. The destructive rows -- recycling a history ring, zeroing a latched grab -- would run at
// frame rate and never let the thing they reset re-establish itself.
bool IsJumpOrMicro(Cause c) {
    return IsJump(c) || c == Cause::MicroSeek;
}

// The rows' one door to the CS site, and the reason it is a function rather than a call to
// g_env.resolveCs.
//
// BindCsSite answers from a memo while the space epoch holds, so a micro-seek storm costs no engine
// lookup at all (AUD-F3C F3C-1). What that memo cannot see is a game space torn down and rebuilt at
// the SAME address -- the netGame's +0x2B08 slot would read identically and the memo would hand back
// component systems belonging to freed memory. A real jump is where the owner actually went
// somewhere, it is rare, and it is the natural place to pay one lookup to be sure; MicroSeek, which
// the transport sets every frame, deliberately is not.
// ONCE PER FIRE, NOT ONCE PER ROW. Five rows call this on a Seek and they all see the same
// Discontinuity, so an unconditional invalidate here would cost five resolves for one jump -- the
// exact cost this function exists to bound. The mark is the jump itself; a fire that repeats the
// previous jump byte for byte does not re-ask, which is a deliberate bound and not a gap: the key
// still refuses any space pointer that moved, and the next distinct jump re-asks.
struct JumpMark {
    int64_t from = 0;
    int64_t to = 0;
    Cause   cause = Cause::Seek;
    bool    seen = false;
};
JumpMark g_lastJump;

CsSite ResolveCsFor(const Discontinuity& d) {
    if (IsJump(d.cause) && (!g_lastJump.seen || g_lastJump.from != d.from_us ||
                            g_lastJump.to != d.to_us || g_lastJump.cause != d.cause)) {
        g_lastJump = JumpMark{d.from_us, d.to_us, d.cause, true};
        InvalidateCsSite();
    }
    return g_env.resolveCs ? g_env.resolveCs() : CsSite();
}

int64_t Distance(int64_t a, int64_t b) {
    return a > b ? a - b : b - a;
}

// ★★★ THE N-3a ORDERING INVARIANT, MEASURED AT THE ONE MOMENT THAT CAN TELL.
//
// Read the history ring's newest sample time BEFORE the reset. If it is nearer the jump's
// DESTINATION than its ORIGIN, ApplyStateAt has already run for this jump and the reset is about to
// delete what it installed. There is no threshold and no tuning: the test is which of two known
// times the sample is closer to, so it scales with the size of the jump and is exactly as sharp for
// a two-second scrub as for a ten-minute one.
//
// A `from == to` discontinuity is not a jump; the distances are equal and the answer is Ok.
Ordering CheckOrdering(const CsSite& cs, const Discontinuity& d) {
    IEntity* e = g_env.entity;
    if (!e) {
        return Ordering::NoEntity;
    }
    const size_t n = NetCiCount(cs);
    if (n == 0) {
        return Ordering::NotChecked;   // nothing to reset, so nothing to get wrong
    }
    for (size_t i = 0; i < n; ++i) {
        const uint16_t h = NetHandleAt(cs, i);
        if (h == residue_facts::kNoComponentHandle) {
            continue;
        }
        BodyState state[1];
        uint64_t times[1] = {0};
        if (e->ReadHistoryWindow(PhysHandle(h), state, times, 1) == 0) {
            continue;
        }
        const int64_t head = static_cast<int64_t>(times[0]);
        return Distance(head, d.to_us) < Distance(head, d.from_us) ? Ordering::Violated
                                                                   : Ordering::Ok;
    }
    // Handles exist but no ring answered. Either this build's IEntity does not implement the
    // readback, or every ring is empty -- both mean the ordering could not be judged, and neither
    // is "ok". Reporting it as Ok would be the vacuous pass this whole structure exists to refuse.
    return Ordering::NoHistoryReadback;
}

void ReportCs(const CsSite& cs) {
    g_stats.lastCsBind = cs.bind;
    if (g_env.log && cs.bind != g_loggedCsBind) {
        g_loggedCsBind = cs.bind;
        if (!cs.Ok()) {
            g_env.log->Write(LogLevel::Warn, kModule, "component-system rows are inert: %s",
                             BindText(cs.bind));
        }
    }
}

void ReportPhys(const PhysSite& ph) {
    g_stats.lastPhysBind = ph.bind;
    if (g_env.log && ph.bind != g_loggedPhysBind) {
        g_loggedPhysBind = ph.bind;
        if (!ph.Ok()) {
            g_env.log->Write(LogLevel::Warn, kModule, "physics rows are inert: %s",
                             BindText(ph.bind));
        }
    }
}

// -------------------------------------------------------------------------------------------
// The shared shape of a row. Availability is a property of THE BUILD -- did the address resolve and
// pass its guard -- and never of the moment. A row whose entry point is present but whose gamespace
// is not up yet is Ready and does nothing, and says so in the stats; calling it Unmapped would make
// "this build cannot do it" and "no session yet" the same word.
class Row : public IResidue {
public:
    Row(const char* id, const char* why, bool microToo)
        : id_(id), why_(why), micro_(microToo) {}

    const char* Id() const override { return id_; }
    Availability Avail() const override {
        return why_[0] == 0 ? Availability::Ready : Availability::Unmapped;
    }
    const char* Diagnostic() const override { return why_; }
    bool RunsOn(Cause c) const override { return micro_ ? IsJumpOrMicro(c) : IsJump(c); }

protected:
    ~Row() = default;

private:
    const char* id_;
    const char* why_;
    bool micro_;
};

// -------------------------------------------------------------------------------------------
// N-3a. The net-physics CI, reset.
//
// ★★★ THIS ROW MUST RUN BEFORE ApplyStateAt, AND IT DOES -- BUT NOT BECAUSE IT ASKED TO.
//
// ResetCI's first act is to walk the CI's history list at PHYS.CI_HIST_LIST_OFF, hand every node
// back to the component system's free list, and zero the count at PHYS.CI_HIST_COUNT_OFF. That is
// the same ring IEntity::SetHistoryWindow fills, and W1-E is explicit that a CI with an empty list
// freezes: the interpolator walks head->next expecting decreasing times and there is no head.
//
// So fired AFTER the apply, this row would delete the state the apply just installed. The battery
// fires at the frame loop's step 4 and the apply is step 5, so the ordering is right today -- but it
// is right by W2-B's construction, not by anything this row can enforce, and the two are one line
// apart in a file this packet does not own. It is recorded here because a future reordering would
// produce a body that freezes after every seek, and nothing in the type system would object.
//
// The recycling is also why this row is a net positive rather than a cost: it returns nodes to the
// pool that SetHistoryWindow draws from, on the one path where the pool is most likely to be short.
class CiResetRow final : public Row {
public:
    CiResetRow(const char* why, ResidueEnv::CsHandleFn fn) : Row("N-3a", why, false), fn_(fn) {}

    void Fire(const Discontinuity& d) override {
        const CsSite cs = ResolveCsFor(d);
        ReportCs(cs);   // this row fires first, so it is where a broken chain is noticed
        if (!cs.Ok() || !fn_) {
            return;
        }

        // ★ Measured BEFORE the first reset, because after it the ring is empty and the evidence
        // is gone.
        g_stats.ordering = CheckOrdering(cs, d);
        if (g_stats.ordering == Ordering::Violated) {
            // ★★★ AND IT REFUSES, RATHER THAN REPORTING AND PROCEEDING.
            //
            // A detector that logs and then does the damage anyway has converted a silent bug into a
            // loud one, which is worth something but is not the fix. ResetCI recycles the history
            // ring ApplyStateAt just filled; running it here would freeze every restored body until
            // the next apply, and the log line would explain a broken replay rather than prevent
            // one. Skipping is strictly better than proceeding: the CI keeps last frame's smoothing
            // state, which is stale, where proceeding empties the ring, which is fatal.
            //
            // This is what W3-A can enforce from its own side. The STRUCTURAL fix -- a phase token
            // on IResidue so that a row which must precede the apply cannot be handed a post-apply
            // discontinuity at all -- needs W2-B, and is specified in docs/spikes/W3-A.md S6.
            ++g_stats.orderingViolations;
            if (g_env.log) {
                g_env.log->Write(LogLevel::Error, kModule, "%s", OrderingText(Ordering::Violated));
            }
            return;
        }

        const size_t n = NetCiCount(cs);
        for (size_t i = 0; i < n; ++i) {
            const uint16_t h = NetHandleAt(cs, i);
            if (h == residue_facts::kNoComponentHandle) {
                continue;
            }
            fn_(cs.netPhysicsCs, h);
            ++g_stats.ciResets;
        }
    }

private:
    ResidueEnv::CsHandleFn fn_;
};

// N-3b. The warp/blend request bits, raised.
//
// ★ ORDERED AFTER N-3a DELIBERATELY. ResetCI rewrites the CI status word at ci+4 and masks it with
// 0x9BFF, and which of its two arms runs is decided by the net-game role flags rather than by
// anything this packet controls. On one arm the low byte comes out set and on the other it comes out
// cleared, so raising the request bits BEFORE ResetCI would be a coin flip. Registration order is
// fire order, and this row registers second.
class WarpFrameRow final : public Row {
public:
    WarpFrameRow(const char* why, ResidueEnv::CsHandleFn fn) : Row("N-3b", why, true), fn_(fn) {}

    void Fire(const Discontinuity& d) override {
        const CsSite cs = ResolveCsFor(d);
        if (!cs.Ok() || !fn_) {
            return;
        }
        const size_t n = NetCiCount(cs);
        for (size_t i = 0; i < n; ++i) {
            const uint16_t h = NetHandleAt(cs, i);
            if (h == residue_facts::kNoComponentHandle) {
                continue;
            }
            fn_(cs.netPhysicsCs, h);
            ++g_stats.warpRequests;
        }
    }

private:
    ResidueEnv::CsHandleFn fn_;
};

// N-4. The per-entity time offset, snapped.
//
// ★★ THE VALUE IS READ, NOT INVENTED. S7.4.3's fallback says "write ci+292 directly" and names no
// value; W1-I S5.5 then found the fallback incomplete, because the engine's own SnapTimeOffset
// writes BOTH the goal at ci+292 and the current at ci+288, and writing only the goal leaves the
// blend where the pre-seek smoothing left it. So this row reads the goal the engine is already
// holding and snaps the current onto it. That is the one value that is correct without knowing
// anything the recording does not carry: it collapses the blend without moving the target.
//
// IEntity::SetTimeOffset is not used for this. It writes ci+292 only -- the incomplete half.
class SnapTimeRow final : public Row {
public:
    SnapTimeRow(const char* why, ResidueEnv::CsSnapFn fn) : Row("N-4", why, true), fn_(fn) {}

    void Fire(const Discontinuity& d) override {
        const CsSite cs = ResolveCsFor(d);
        if (!cs.Ok() || !fn_) {
            return;
        }
        const size_t n = NetCiCount(cs);
        for (size_t i = 0; i < n; ++i) {
            const uint16_t h = NetHandleAt(cs, i);
            if (h == residue_facts::kNoComponentHandle) {
                continue;
            }
            const void* ci = NetCi(cs, h);
            if (!ci) {
                continue;
            }
            float goal = 0.0f;
            memcpy(&goal,
                   static_cast<const uint8_t*>(ci) + facts::PHYS_CI_TIME_OFFSET_S_OFF,
                   sizeof goal);
            fn_(cs.netPhysicsCs, h, goal);
            ++g_stats.timeSnaps;
        }
    }

private:
    ResidueEnv::CsSnapFn fn_;
};

// N-2. The latched grab / hand fixups, cleared.
//
// ★★★ THE DESIGN'S FALLBACK FOR THIS ROW IS DEAD CODE ON THIS BUILD, which is why the row is a call.
// S7.4.3 N-2 specifies the sentinel idiom: publish one frame with holdingontouserid = kInvalid and
// the grab bits cleared, then the recorded word, so EmitChangedDelegates diffs a detach edge and a
// re-attach edge and the fixup re-latches. That idiom needs a netvar write, and INetVar::Write
// unconditionally refuses on PC -- W2-A refuses loudly rather than writing to a guessed store
// offset, and W2-B's ApplyStats counts the refusals. So the sentinel is not a fallback this build
// can take, and calling ResetGrabPosHandFixups is not a preference over it: it is the only path.
//
// The function zeroes four C3Vectors and four floats in one 64-byte block of the 832-byte player CI,
// which is the clear the row asks for -- verb confirmed from the body, not from the name.
class GrabFixupRow final : public Row {
public:
    GrabFixupRow(const char* why, ResidueEnv::CsHandleFn fn) : Row("N-2", why, false), fn_(fn) {}

    void Fire(const Discontinuity& d) override {
        const CsSite cs = ResolveCsFor(d);
        if (!cs.Ok() || !fn_) {
            return;
        }
        const size_t n = PlayerCiCount(cs);
        for (size_t i = 0; i < n; ++i) {
            const uint16_t h = PlayerHandleAt(cs, i);
            if (h == residue_facts::kNoComponentHandle) {
                continue;
            }
            fn_(cs.remotePlayerCs, h);
            ++g_stats.grabClears;
        }
    }

private:
    ResidueEnv::CsHandleFn fn_;
};

// -------------------------------------------------------------------------------------------
// The physics rows. All three share ForEachRestoredBody and differ only in what they do per body.

bool RaiseOne(void* body, void*) {
    // ★★★ ONLY WHERE THE BIT HAS A READER. CollectClassifiedBodies is the only thing in the image
    // that reads this bit, and it walks two lists rather than every body. On a body it never visits
    // the write is inert AND never cleared -- litter with no reader, permanent until the body dies.
    // A body can be neither kinematic nor sphere-effect and still be off both lists, so the flag
    // mask N-7b uses does not answer this; the body's own class field does.
    if (!BodyClassified(body)) {
        return false;
    }
    return RaiseMotionBit(body);
}

bool ClearOne(void* body, void*) {
    return ClearHysteresis(body);
}

// A struct rather than a function pointer squeezed through the void*: casting between object and
// function pointers is conditionally supported, and the one platform where it works is not a reason
// to write it that way when a two-line struct is exact.
struct WakeCtx {
    ResidueEnv::BodyWakeFn fn;
};

bool WakeOne(void* body, void* ctx) {
    if (!BodyAsleep(body)) {
        return false;   // BeAwake_ is safe on an awake body, but calling it would not be an effect
    }
    static_cast<WakeCtx*>(ctx)->fn(body, 1u, 1u);
    return true;
}

// N-7a. The motion bit.
//
// ★★★ THIS IS THE HALF THAT CANNOT FAIL, AND IT IS A SEPARATE ROW FOR THAT REASON ALONE.
// It calls nothing. It resolves no address beyond the CPhSystem the whole physics group needs, and
// it stores one bit. If N-7b's guard refuses, this still runs -- and it still achieves the wake,
// because CPhZone::CollectClassifiedBodies TESTS this bit to choose between its wake arm and its
// GoToSleep arm, and the test precedes the clear in the same pass.
//
// ★★ WHY A ONE-SHOT WRITE IS THE CORRECT SHAPE AND NOT A LEAK. The same pass clears the bit on both
// arms, every frame, so the bit is not durable state and there is nothing to restore. One write buys
// one classification, which is exactly the one classification that follows the jump. After that the
// engine's own integrator owns the bit again. The fact registry says so in as many words -- "the bit
// is not durable state" -- and the mistake that observation prevents is a sweep that re-sets it
// every frame and pins the level awake.
class MotionBitRow final : public Row {
public:
    MotionBitRow() : Row("N-7a", "", false) {}

    void Fire(const Discontinuity& d) override {
        const CsSite cs = ResolveCsFor(d);
        const PhysSite ph = g_env.resolvePhys ? g_env.resolvePhys() : PhysSite();
        ReportCs(cs);
        ReportPhys(ph);
        const BodyWalk w = ForEachRestoredBody(cs, ph, &RaiseOne, nullptr);
        g_stats.lastWalk = w;
        g_stats.motionBitsRaised += w.acted;
    }
};

// N-7b. CPhBody::BeAwake_ on the bodies that are parked right now.
//
// ★ ONLY THE ASLEEP ONES, BECAUSE THAT IS WHAT THE COUNT HAS TO MEAN. BeAwake_ is safe to call on an
// awake body -- every structural step inside it is gated on the sleep stamp being non-zero, so on an
// awake body it stores a zero over a zero and returns. Calling it anyway would be harmless and would
// make `wakesCalled` a count of pool slots rather than a count of bodies un-parked, and this packet
// has exactly one observable.
//
// Both flags are set. a2 clears the sleep-frame counter, which is what GoToSleep compares against its
// threshold -- so the body gets a full fresh timeout instead of re-sleeping on the next pass with a
// counter the pre-seek session left high. a3 re-voxelises the body's bounds and wakes its immediate
// neighbours and attached skeletons, which is the nearest thing this build has to N-8's manifold
// clear and is why N-8's row below is not simply refused.
class WakeRow final : public Row {
public:
    WakeRow(const char* why, ResidueEnv::BodyWakeFn fn) : Row("N-7b", why, false), fn_(fn) {}

    void Fire(const Discontinuity& d) override {
        if (!fn_) {
            return;
        }
        const CsSite cs = ResolveCsFor(d);
        const PhysSite ph = g_env.resolvePhys ? g_env.resolvePhys() : PhysSite();
        WakeCtx ctx = {fn_};
        const BodyWalk w = ForEachRestoredBody(cs, ph, &WakeOne, &ctx);
        g_stats.wakesCalled += w.acted;
    }

private:
    ResidueEnv::BodyWakeFn fn_;
};

// N-8a. The hysteresis latches.
//
// W1-I S6.1: CPhZone::ResetHysteresisAllBodies has no PC function, but every field it writes, the
// guard it tests and the array it walks are mapped PC facts, so its body is reproducible line for
// line. This is that reproduction, over the bodies the replay owns rather than over the whole zone.
class HysteresisRow final : public Row {
public:
    HysteresisRow() : Row("N-8a", "", false) {}

    void Fire(const Discontinuity& d) override {
        const CsSite cs = ResolveCsFor(d);
        const PhysSite ph = g_env.resolvePhys ? g_env.resolvePhys() : PhysSite();
        const BodyWalk w = ForEachRestoredBody(cs, ph, &ClearOne, nullptr);
        g_stats.hysteresisCleared += w.acted;
    }
};

// N-8b. The half that is not served, registered so that it is reported.
//
// ★★ A ROW THAT EXISTS ONLY TO SAY NO. Without it, N-8a's presence would imply that N-8 is handled,
// and the panel would show no unavailable id for contact state at all. The reason is specific enough
// to act on: CPhZone::ResetDiscoverPhases has no PC counterpart (W1-I searched all 7,519 PC functions
// making five to seven direct calls, including folded variants, and scoped that search in its own
// record), and CPhContact::Initialize IS mapped but is refused on role -- it CONSTRUCTS a contact,
// so sweeping it over a live pool would install manifolds rather than clear them.
class ManifoldRow final : public Row {
public:
    ManifoldRow() : Row("N-8b",
                        "contact manifolds and discover phases: ResetDiscoverPhases has no PC "
                        "counterpart, and CPhContact::Initialize is mapped but refused on role -- "
                        "it constructs contacts. BeAwake_'s neighbour wake (N-7b) is the partial "
                        "substitute; stale manifolds are not cleared",
                        false) {}

    void Fire(const Discontinuity&) override {}   // never called: the battery skips a non-Ready row
};

// N-6a. The cumulative world re-origin, WATCHED rather than reset.
//
// WORLD.REORIGIN_FN adds a CUMULATIVE offset, so running it twice double-shifts the entire world.
// This packet never calls it, which is the whole of "never re-run it" as far as we are concerned.
//
// ★★★ AND IT NO LONGER CLEARS THE REQUEST BIT, WHICH THE FIRST DROP DID AND WAS WRONG ABOUT.
// S7.4.3's fallback says to clear bits 0x100/0x200. They are not two request flags: sub_14050BD00
// swaps them in one read-modify-write (`btr rcx,8` then `bts rcx,9` at 0x14050BF09/0E), so 0x100
// means PENDING and 0x200 means BEGUN. Clearing 0x200 from outside cancels a translation that has
// already started, with 0x100 already gone -- no bit remains that would ever finish it, and the
// world stays permanently half-shifted. The fallback would have caused the defect the row exists to
// prevent. residue_facts.h carries the full derivation.
//
// So this row writes nothing. It latches the CUMULATIVE triple and reports a shift that happened
// anyway, which is the observable the hazard actually produces.
class ReoriginRow final : public Row {
public:
    ReoriginRow() : Row("N-6a", "", true) {}

    void Fire(const Discontinuity&) override {
        const WorldSite w = g_env.resolveWorld ? g_env.resolveWorld() : WorldSite();
        if (!w.Ok()) {
            return;
        }
        unsigned char now[kOriginSpanBytes];
        if (!ReadWorldOrigin(w.originBlock, now)) {
            return;
        }
        if (!g_originLatched) {
            memcpy(g_origin, now, sizeof g_origin);
            g_originLatched = true;
            return;
        }
        if (memcmp(g_origin, now, sizeof g_origin) != 0) {
            // The world moved under us despite the request bit being held clear. That is a finding
            // about somebody else's write, not something to paper over -- so it is counted and
            // logged, the new value becomes the baseline, and nothing is rewritten.
            ++g_stats.worldOriginShifts;
            memcpy(g_origin, now, sizeof g_origin);
            if (g_env.log) {
                g_env.log->Write(LogLevel::Warn, kModule,
                                 "the world re-origin offset changed across a discontinuity; the "
                                 "cumulative shift ran despite its request bit being cleared");
            }
        }
    }
};

// N-6b. Restoring that offset, refused, and registered so the refusal is visible.
class ReoriginRestoreRow final : public Row {
public:
    ReoriginRestoreRow()
        : Row("N-6b",
              "the design's remedy for N-6 -- clear the request bits, restore the offset -- is "
              "refused on BOTH halves. Clearing: 0x100 and 0x200 are one two-phase state that "
              "sub_14050BD00 swaps atomically (btr 8 / bts 9), so clearing 0x200 cancels a "
              "translation that has already begun and leaves no bit that could ever finish it. "
              "Restoring: the recording DOES carry the origin -- capture writes kKeyBlockWorldOrigin "
              "every chapter -- but nothing on the playback side reads that block yet, and the write "
              "target is inside the particle-effect renderer singleton whose field layout beyond the "
              "cumulative triple was never derived. That is a buildable row waiting on a keyframe "
              "reader, not an unmappable one. N-6a latches the triple and reports a shift instead",
              false) {}

    void Fire(const Discontinuity&) override {}   // never called: the battery skips a non-Ready row
};

// N-18. The CClock accumulators.
//
// ★★★ THE ROW IS ABOUT AN UNSIGNED SUBTRACTION THAT NOBODY WROTE. CLOCK.ACCUM1_OFF and
// CLOCK.ACCUM2_OFF are only ever reached by `add [rdi+28h],rdx` / `add [rdi+30h],rdx`. Add a
// negative delta -- which is what driving a playhead backwards produces -- and an accumulator
// smaller than the delta wraps to about 1.8e19 microseconds instead of going negative.
// RESET.STAT_REBASE_A then converts that to a float, and every stat, score and UI value derived
// from it reads about 5.8e5 centuries. Same defect as W2-H's 3 ms sound reporting 4.29e9 ms.
//
// ★★ IT RUNS ON EVERY CAUSE, INCLUDING ReverseTick. Reverse is the direction that can produce the
// negative delta in the first place, and a per-seek check would let a whole reverse pass wrap
// unobserved. It is three loads and three compares.
//
// ★ IT REPAIRS A WRAP AND ONLY REPORTS A PLAIN DECREASE. Ledger row 0.4 gives clock writes to the
// transport; a decrease that has not wrapped is a value somebody chose, and silently undoing it
// would hide their defect behind ours. A wrapped accumulator is not a value anybody chose.
class ClockGuardRow final : public Row {
public:
    ClockGuardRow() : Row("N-18", "", true) {}

    bool RunsOn(Cause) const override { return true; }

    void Fire(const Discontinuity& d) override {
        // ★★★ THE RAISE COUNT, TAKEN ABOVE THE SITE GUARD DELIBERATELY. The discontinuity reached
        // this packet whether or not a clock resolved, and counting it below the guard would turn
        // `fires` into a twelfth EFFECT counter -- handing the reader back the ambiguous zero the
        // field exists to remove. N-18 is where it lives because it is the only row the battery
        // never skips: always Ready, and RunsOn returns true for every cause.
        ++g_stats.fires;
        const size_t causeIdx = static_cast<size_t>(d.cause);
        if (causeIdx < static_cast<size_t>(Cause::Count)) {
            ++g_stats.firesByCause[causeIdx];
            g_stats.lastFireCause = d.cause;
        }

        const ClockSite c = g_env.resolveClock ? g_env.resolveClock() : ClockSite();
        if (!c.Ok()) {
            return;
        }
        const ClockVerdict v = GuardClockMonotonic(c.clock, g_clockLatch);
        g_stats.lastClockVerdict = v;
        if (v == ClockVerdict::Wrapped) {
            ++g_stats.clockWrapsRepaired;
            if (g_env.log) {
                g_env.log->Write(LogLevel::Error, kModule,
                                 "a CClock accumulator wrapped past 2^63 and was restored: a "
                                 "negative delta reached an unsigned add");
            }
        } else if (v == ClockVerdict::Backwards) {
            ++g_stats.clockBackwardsSeen;
        }
    }
};

// -------------------------------------------------------------------------------------------
// Static storage. The battery holds borrowed pointers and never deletes through them, and a fire
// path that allocated would be the one allocation on a path that must not have any.

alignas(CiResetRow) uint8_t g_ciStore[sizeof(CiResetRow)];
alignas(WarpFrameRow) uint8_t g_warpStore[sizeof(WarpFrameRow)];
alignas(SnapTimeRow) uint8_t g_snapStore[sizeof(SnapTimeRow)];
alignas(GrabFixupRow) uint8_t g_grabStore[sizeof(GrabFixupRow)];
alignas(MotionBitRow) uint8_t g_motionStore[sizeof(MotionBitRow)];
alignas(WakeRow) uint8_t g_wakeStore[sizeof(WakeRow)];
alignas(HysteresisRow) uint8_t g_hystStore[sizeof(HysteresisRow)];
alignas(ManifoldRow) uint8_t g_manifoldStore[sizeof(ManifoldRow)];
alignas(ReoriginRow) uint8_t g_reoriginStore[sizeof(ReoriginRow)];
alignas(ReoriginRestoreRow) uint8_t g_reoriginRestoreStore[sizeof(ReoriginRestoreRow)];
alignas(ClockGuardRow) uint8_t g_clockStore[sizeof(ClockGuardRow)];

const char* WhyMissing(const void* fn, const char* text) {
    return fn ? "" : text;
}

// Resolves one guarded entry point. Returns null and leaves the caller to report when the mapped
// bytes are not the fact's -- which is also how another mod's hook at the same address is caught.
//
// ★ THE GUARD PROVES UNHOOKED, NOT UNIQUE. RESET.WARP_FRAME_PC and RESET.SNAP_TIME_OFFSET_PC have
// BYTE-IDENTICAL 20-byte prologues -- both open with the same CI-indexing preamble -- so this check
// cannot tell those two functions apart from each other. It is still the right check, because it is
// asked at a specific address and answers whether THAT address still holds the bytes the fact
// recorded. It is not evidence that the address is the function the id names; that came from the
// bodies, in docs/spikes/W3-A.md.
//
// ★★★ AND IT TAKES THE SIGNATURE AS AN ARGUMENT, BECAUSE A CAST AFTERWARDS ERASES ARITY.
//
// This used to return void* and every caller wrote reinterpret_cast<ResidueEnv::CsHandleFn>(...)
// around it. That cast will convert ANY address to ANY function type in silence, which is how
// RESET.CI_PC -- an entry point that takes the CI ROW POINTER -- came to be called with a uint16
// handle and killed the client at 0x140D3EA4A (DIAG-CRASH). Naming the signature HERE means the
// only thing the compiler will accept is a type that matches the ResidueEnv field being assigned:
// hand Guarded<CsSnapFn> to a CsHandleFn field, or drop an argument from the type, and it does not
// compile. It also puts the declared signature next to the fact id in the source, which is where
// tools/gen_facts.py --check reads the pair from to ask the exe whether the code at that address
// actually consumes argument 2 the way this line says it does.
template <typename Sig>
Sig Guarded(const char* factId, uint64_t va, const uint8_t* prologue, size_t len) {
    static_assert(std::is_pointer<Sig>::value &&
                      std::is_function<typename std::remove_pointer<Sig>::type>::value,
                  "Guarded<Sig> takes the pointer-to-function type the entry point is called as");
    if (!buildguard::VerifySite(factId, va, prologue, len)) {
        return nullptr;
    }
    return reinterpret_cast<Sig>(const_cast<void*>(buildguard::MappedAddress(va)));
}

// ---------------------------------------------------------------------------------------------
// THE ROSTER. The design's nineteen active rows, as a table this file owns.
//
// N-1 is absent on purpose: S7.4.3 struck it through and reclassified it to category I (body joint
// springs are exponential filters that pre-roll converges, which IS the definition of I). Nineteen
// is the count the design's own text gives, and tests/residue re-derives that list from the design
// and fails if this table and that document disagree -- so the denominator cannot quietly follow
// the work.
//
// `note` is non-empty exactly when something is missing, and it is the reason, not a restatement of
// the fact that it is missing.
const RosterRow kRoster[kRosterRows] = {
    {"N-2", "grab / hand fixups: latched attach offsets", ""},
    {"N-3", "net-physics CI smoothing and the latched warp frame", ""},
    {"N-4", "per-entity time offset blending toward a goal", ""},
    {"N-5", "stat / score / UI accumulators: clockTime - storedBase goes negative on a back seek",
     "not one mechanism. RESET.STAT_REBASE_A only reads (clock+0x30 minus a base at obj+0xC44); "
     "RESET.STAT_REBASE_B writes a different base into netGame+0x64030+116*slot and has zero code "
     "xrefs. 'Rebase the stored base' is an unbounded set of fields across many objects, not a "
     "call, and no bounded enumeration of them exists. N-18 guards the accumulator they all read"},
    {"N-6", "cumulative world re-origin: replaying it double-shifts the world", ""},
    {"N-7", "physics sleep: a body restored moving with its sleep flag set never wakes", ""},
    {"N-8", "contact manifold cache and hysteresis latches", ""},
    {"N-9", "particle pools and per-particle age",
     "CGParticleEffect::KillParticles is mapped, but nothing enumerates the LIVE effects at "
     "runtime: no particle-manager pointer is in the registry and this packet did not derive one. "
     "The call exists and has nothing to call it on"},
    {"N-10", "particle RNG: emission is a seeded deterministic stream",
     "same missing enumeration as N-9, and independently the recording does not capture the seed, "
     "so even a reachable emitter would have nothing to be pinned to"},
    {"N-11", "trails / ribbons: vertex history streaks across the map",
     "the design calls this RESTORABLE rather than resettable -- S_KEY carries the state like any "
     "other replicated component -- but that block is NOT WRITTEN. src/capture/keyframe.h reserves "
     "kKeyBlockTrailState = 19 and lists it unwritten, so no recording contains it. Until it is, a "
     "seek leaves the disc trail's vertex history spanning both playheads and it draws one streak "
     "across the map, which lasts as long as the trail's own history window"},
    {"N-12", "decals accumulate across the jump",
     "needs a CS.SYM_R14DECAL that is not in the registry (derivable: the CSymbol64 sits "
     "immediately before the OlPrEfIxDecal literal), and RESET.DECAL_ACTOR_GENERATIONS_PC is "
     "ICF-folded across about 169 vtables so the address only means anything with the right this. "
     "Deprioritised against the design's own 'accept; Arena has few', not blocked"},
    {"N-13", "animation clocks hold an absolute UTime a pre-roll cannot recover",
     "two independent gaps. The three setters are mapped but no reachability path to a live "
     "CAnimClockEvaluator was established; and S_KEY row 18 marks ANIM_PHASE as W1-I -> W2-A, so "
     "the clip id and phase needed to restart a clip may not be in a recording at all. Either "
     "alone blocks it"},
    {"N-14", "camera history: previous == current for one frame, or TAA smears",
     "S7.4.6 classifies this category X -- one frame of smear is accepted by design. The writer "
     "is VIEW.PROPAGATE and the array offset and stride are mapped, so it is buildable; it is not "
     "built because the design does not ask for it. Listed so 'accepted' is visible rather than "
     "indistinguishable from 'forgotten'"},
    {"N-15", "engine timers hold stale remaining time or already fired",
     "the CTimerManager pointer is mapped (TIMER.MANAGER_PTR) but neither ClearTimer nor "
     "UpdateTimer has a PC address in the registry, so there is nothing to call. Not attempted by "
     "this packet and not attempted by W1-I either"},
    {"N-16", "Wwise state/switch memo cache silently no-ops on a repeat",
     "W2-H's, and built: src/audio/audioresidue.cpp registers it through audio::ArmAudioResidue"},
    {"N-17", "playing voices and loops: one-shots already fired, loops still running",
     "W2-H's, and built: registered as N-17a and N-17b through audio::ArmAudioResidue"},
    {"N-18", "CClock last stamp and accumulators hold pre-seek time", ""},
    {"N-19", "mission-script fibers hold suspended execution state",
     "SCRIPT.UPDATE_FAST_FORWARD is reachable and takes only the CR15Game singleton, but its body "
     "reads as one half of an enter/leave pair around a depth counter at +0x7AB8 and the partner "
     "was not identified. Calling one half of a pair is the defect W2-H found in its own mute; "
     "refused rather than guessed"},
    {"N-20", "the 30-entry history lists land in a state the original never had",
     "served outside the battery, and the two lists are served differently. BODY ring: "
     "IEntity::SetHistoryWindow replaces it whole from the file every frame, which is exactly what "
     "the row asks for; a battery row that cleared it would be the freeze W1-E's one-call design "
     "exists to prevent. PLAYER ring: never written, deliberately -- under authority, which an "
     "offline replay always is, UpdateInterpolatedPlayerStates short-circuits on the net flags "
     "before it reads the ring, so its contents are unobservable and PP-B publishes through the raw "
     "state block instead (W1-E S4.1)"},
};

// True when a registered battery id covers a design row. Prefix matching, but only across a LETTER
// suffix -- the split rows are N-3a, N-7b, N-17a, and the digits must not run together.
//
// ★ THE CASE THIS EXISTS FOR: "N-13" must NOT count as covering "N-1", and a naive prefix test says
// it does. N-1 is not in the table today, but N-2 against N-20 is the same shape and that pair IS
// here. The character after the prefix has to be end-of-string or a lowercase letter; a digit means
// this is a different, longer row number.
bool CoversRow(const char* registered, const char* designId) {
    const size_t n = strlen(designId);
    if (strncmp(registered, designId, n) != 0) {
        return false;
    }
    const char c = registered[n];
    return c == '\0' || (c >= 'a' && c <= 'z');
}

}  // namespace

const char* CoverageText(Coverage c) {
    switch (c) {
        case Coverage::Ready: return "ready";
        case Coverage::Partial: return "partial";
        case Coverage::Unavailable: return "unavailable";
        case Coverage::Unregistered: return "UNREGISTERED (nobody built it)";
        default: return "?";
    }
}

size_t Roster(RosterRow* out, size_t cap) {
    for (size_t i = 0; i < kRosterRows && i < cap; ++i) {
        RosterRow row = kRoster[i];
        size_t ready = 0;
        size_t notReady = 0;

        // The numerator comes from the LIVE battery, so it counts W2-B's placeholders and W2-H's
        // audio rows too. The denominator above does not, which is the whole design.
        if (g_battery) {
            for (size_t j = 0; j < g_battery->Count(); ++j) {
                IResidue* r = g_battery->At(j);
                if (!r || !CoversRow(r->Id(), row.id)) {
                    continue;
                }
                if (r->Avail() == Availability::Ready) {
                    ++ready;
                } else {
                    ++notReady;
                }
            }
        }

        row.registered = ready + notReady;
        if (row.registered == 0) {
            row.coverage = Coverage::Unregistered;
        } else if (notReady == 0) {
            row.coverage = Coverage::Ready;
        } else if (ready == 0) {
            row.coverage = Coverage::Unavailable;
        } else {
            // ★ The third state, and it is the one that would otherwise be rounded off. N-8a works
            // and N-8b does not; calling that Ready overclaims a manifold reset that never runs,
            // and calling it Unavailable throws away a hysteresis clear that does.
            row.coverage = Coverage::Partial;
        }
        out[i] = row;
    }
    return kRosterRows;
}

RosterSummary Summary() {
    RosterSummary sum;
    sum.consulted = (g_battery != nullptr);
    RosterRow rows[kRosterRows];
    Roster(rows, kRosterRows);
    for (size_t i = 0; i < kRosterRows; ++i) {
        switch (rows[i].coverage) {
            case Coverage::Ready: ++sum.ready; break;
            case Coverage::Partial: ++sum.partial; break;
            case Coverage::Unavailable: ++sum.unavailable; break;
            default: ++sum.unregistered; break;
        }
    }
    return sum;
}

void SetArmServices(ReplayServices* s) {
    g_services = s;
}

const ResidueStats& Stats() {
    return g_stats;
}

void ResetStatsForTest() {
    g_stats = ResidueStats();
    InvalidateCsSite();
    g_lastJump = JumpMark();
    g_loggedCsBind = Bind::Count;
    g_loggedPhysBind = Bind::Count;
    g_clockLatch = ClockLatch();
    g_originLatched = false;
    // ★ THE BATTERY POINTER TOO, AND A TEST FOUND OUT WHY. The roster reads the live battery
    // through this pointer, and a suite that registers into a battery on the STACK leaves it
    // dangling the moment that test case returns -- the next Roster() call then walks freed memory.
    // It segfaulted exactly there. In the product the battery is a member of a file-static Global
    // and outlives everything, but "the only caller happens to be safe" is not a lifetime contract,
    // so the seam that resets state resets this too.
    g_battery = nullptr;
}

size_t RegisterPlaybackResidue(ResidueBattery& battery, const ResidueEnv& env) {
    g_env = env;
    g_stats.armed = true;
    // Arming is a new session as far as this packet is concerned; nothing from the last one is still
    // a live component system.
    InvalidateCsSite();
    g_lastJump = JumpMark();
    g_battery = &battery;
    g_clockLatch = ClockLatch();
    g_originLatched = false;

    CiResetRow* ci = new (g_ciStore) CiResetRow(
        WhyMissing(reinterpret_cast<const void*>(env.resetCi),
                   "net-physics CI reset: RESET.CI_PC did not pass its prologue guard"),
        env.resetCi);
    WarpFrameRow* warp = new (g_warpStore) WarpFrameRow(
        WhyMissing(reinterpret_cast<const void*>(env.resetWarpFrame),
                   "warp/blend request: RESET.WARP_FRAME_PC did not pass its prologue guard"),
        env.resetWarpFrame);
    SnapTimeRow* snap = new (g_snapStore) SnapTimeRow(
        WhyMissing(reinterpret_cast<const void*>(env.snapTimeOffset),
                   "per-entity time offset: RESET.SNAP_TIME_OFFSET_PC did not pass its prologue "
                   "guard"),
        env.snapTimeOffset);
    GrabFixupRow* grab = new (g_grabStore) GrabFixupRow(
        WhyMissing(reinterpret_cast<const void*>(env.clearGrabFixups),
                   "grab/hand fixups: RESET.GRAB_POS_HAND_FIXUPS_PC did not pass its prologue "
                   "guard, and the netvar sentinel fallback is unavailable because INetVar::Write "
                   "refuses on this build"),
        env.clearGrabFixups);
    MotionBitRow* motion = new (g_motionStore) MotionBitRow();
    WakeRow* wake = new (g_wakeStore) WakeRow(
        WhyMissing(reinterpret_cast<const void*>(env.wake),
                   "physics wake: RESET.PHYS_WAKE_PC did not pass its prologue guard. N-7a still "
                   "raises the motion bit, which the engine's own classification pass acts on"),
        env.wake);
    HysteresisRow* hyst = new (g_hystStore) HysteresisRow();
    ManifoldRow* manifold = new (g_manifoldStore) ManifoldRow();
    ReoriginRow* reorigin = new (g_reoriginStore) ReoriginRow();
    ReoriginRestoreRow* reoriginRestore = new (g_reoriginRestoreStore) ReoriginRestoreRow();
    ClockGuardRow* clockGuard = new (g_clockStore) ClockGuardRow();

    // ★ REGISTRATION ORDER IS FIRE ORDER, AND TWO OF THESE PAIRS ARE ORDER-DEPENDENT.
    //
    //   N-3a before N-3b -- ResetCI rewrites the same status word N-3b raises bits in, and masks it
    //                       differently on its two arms. Raising first is a coin flip.
    //   N-3a before N-4  -- ResetCI zeroes the time-offset goal at ci+292. N-4 reads that goal and
    //                       snaps the current value onto it, so running after leaves the pair
    //                       agreeing on whatever ResetCI decided rather than on a stale target.
    //   N-7a before N-7b -- the bit is what survives if the wake's guard refused; ordering it first
    //                       means the half that always works is never behind the half that may not.
    size_t n = 0;
    n += battery.Register(ci) ? 1u : 0u;
    n += battery.Register(warp) ? 1u : 0u;
    n += battery.Register(snap) ? 1u : 0u;
    n += battery.Register(grab) ? 1u : 0u;
    n += battery.Register(motion) ? 1u : 0u;
    n += battery.Register(wake) ? 1u : 0u;
    n += battery.Register(hyst) ? 1u : 0u;
    n += battery.Register(manifold) ? 1u : 0u;
    n += battery.Register(reorigin) ? 1u : 0u;
    n += battery.Register(reoriginRestore) ? 1u : 0u;
    n += battery.Register(clockGuard) ? 1u : 0u;
    return n;
}

size_t ArmPlaybackResidue(ResidueBattery& battery, ILog* log) {
    ResidueEnv env;
    env.log = log;
    env.resolveCs = &ResolveCsSite;
    SetBindEpochSource(&ArmedBindEpoch);
    env.resolvePhys = &ResolvePhysSite;
    env.resolveWorld = &ResolveWorldSite;
    env.resolveClock = &ResolveClockSite;
    env.entity = ArmedEntity();

    env.resetCi = Guarded<ResidueEnv::CsHandleFn>(
        "RESET.CI_PC", facts::RESET_CI_PC, facts::RESET_CI_PC_PROLOGUE,
        sizeof facts::RESET_CI_PC_PROLOGUE);
    env.resetWarpFrame = Guarded<ResidueEnv::CsHandleFn>(
        "RESET.WARP_FRAME_PC", facts::RESET_WARP_FRAME_PC, facts::RESET_WARP_FRAME_PC_PROLOGUE,
        sizeof facts::RESET_WARP_FRAME_PC_PROLOGUE);
    env.snapTimeOffset = Guarded<ResidueEnv::CsSnapFn>(
        "RESET.SNAP_TIME_OFFSET_PC", facts::RESET_SNAP_TIME_OFFSET_PC,
        facts::RESET_SNAP_TIME_OFFSET_PC_PROLOGUE, sizeof facts::RESET_SNAP_TIME_OFFSET_PC_PROLOGUE);
    env.clearGrabFixups = Guarded<ResidueEnv::CsHandleFn>(
        "RESET.GRAB_POS_HAND_FIXUPS_PC", facts::RESET_GRAB_POS_HAND_FIXUPS_PC,
        facts::RESET_GRAB_POS_HAND_FIXUPS_PC_PROLOGUE,
        sizeof facts::RESET_GRAB_POS_HAND_FIXUPS_PC_PROLOGUE);
    env.wake = Guarded<ResidueEnv::BodyWakeFn>(
        "RESET.PHYS_WAKE_PC", facts::RESET_PHYS_WAKE_PC, facts::RESET_PHYS_WAKE_PC_PROLOGUE,
        sizeof facts::RESET_PHYS_WAKE_PC_PROLOGUE);

    const size_t n = RegisterPlaybackResidue(battery, env);
    if (log) {
        log->Write(LogLevel::Info, kModule,
                   "armed %u category-N rows (N-2, N-3a/b, N-4, N-6a/b, N-7a/b, N-8a/b, N-18); "
                   "%u of the design's %u rows are covered by SOMETHING, %u are registered by "
                   "nobody",
                   (unsigned)n, (unsigned)(Summary().ready + Summary().partial),
                   (unsigned)kRosterRows, (unsigned)Summary().unregistered);
    }
    return n;
}

}  // namespace residue
}  // namespace playback
}  // namespace replay
