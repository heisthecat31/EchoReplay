#include "verify/o3.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace replay {
namespace verify {

namespace {

// The gate's own distance function, used by the gate and by the sensitivity ladder alike.
float PosErrorM(const float a[3], const float b[3]) {
    const double dx = double(a[0]) - double(b[0]);
    const double dy = double(a[1]) - double(b[1]);
    const double dz = double(a[2]) - double(b[2]);
    return float(sqrt(dx * dx + dy * dy + dz * dz));
}

bool BitsEqual(const float* a, const float* b, size_t n) {
    return memcmp(a, b, n * sizeof(float)) == 0;
}

void Note(O3Result& r, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r.detail, sizeof(r.detail), fmt, ap);
    va_end(ap);
}

void Refuse(O3Result& r, NotRunReason why, const char* fmt, ...) {
    r.verdict = Verdict::NotRun;
    r.reason = why;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r.detail, sizeof(r.detail), fmt, ap);
    va_end(ap);
}

// Two streams share storage if their instant arrays are the same object, or if any bone array is.
// The pointer test is deliberately shallow: it catches the defect that has actually happened here --
// one buffer handed to both sides -- and does not pretend to prove two allocations are disjoint.
bool InstantsAlias(const SkeletonStream& a, const SkeletonStream& b) {
    return a.instants != nullptr && a.instants == b.instants;
}

bool FramesAlias(const BoneFrame& a, const BoneFrame& b) {
    return (a.boneO != nullptr && a.boneO == b.boneO) || (a.boneT != nullptr && a.boneT == b.boneT);
}

// Monotonic, strictly.  Two instants at one source time cannot be matched unambiguously, and
// silently taking the first is how a duplicated sample becomes an invisible half of a corpus.
int FirstNonIncreasing(const SkeletonStream& s) {
    for (uint32_t i = 1; i < s.count; ++i) {
        if (s.instants[i].sourceTimeUs <= s.instants[i - 1].sourceTimeUs) {
            return int(i);
        }
    }
    return -1;
}

// Per-stream: does the recorded source time advance at the same rate as the engine's own sim clock?
// W1-D measured that the engine beats its clock three times a frame and that the delta handed to our
// tick is beat 1 alone, so a producer that integrated the handed delta under-reports elapsed time by
// a large fraction.  This catches that at the oracle rather than after the fidelity numbers are
// published.  Only meaningful at r == 1, which is the only rate S12.5.2 lets the gate run at.
bool DriftRefused(const SkeletonStream& s, float maxFrac, double* outFrac) {
    *outFrac = 0.0;
    if (maxFrac <= 0.0f || s.count < 2) {
        return false;
    }
    const uint64_t srcSpan = s.instants[s.count - 1].sourceTimeUs - s.instants[0].sourceTimeUs;
    const uint64_t simFirst = s.instants[0].simTimeBeforeUs;
    const uint64_t simLast = s.instants[s.count - 1].simTimeAfterUs;
    if (simLast <= simFirst) {
        *outFrac = 1.0;
        return true;
    }
    const double simSpan = double(simLast - simFirst);
    *outFrac = fabs(double(srcSpan) - simSpan) / simSpan;
    return *outFrac > double(maxFrac);
}

}  // namespace

O3Config O3Defaults() {
    O3Config c;
    c.posToleranceM = 0.0005f;   // SC2
    c.oriToleranceDeg = 0.1f;    // SC2
    c.minPassRatio = 0.95f;      // SC2
    c.maxClockDriftFrac = 0.10f; // W1-D's own discriminating margin between the two clocks
    return c;
}

// ★ NOT 2*acos(|dot|), and the difference decides whether this gate can see what it claims to.
//
// acos is ill-conditioned exactly where O3 lives. Near identity d(acos)/d(dot) = -1/sqrt(1-dot^2),
// which for a 0.02 deg separation is about 5700: the ~4e-7 of rounding that two float-stored unit
// quaternions carry becomes ~0.05 deg of metric noise -- half of SC2's whole 0.1 deg tolerance, on
// a pair that differs by nothing. A gate whose own metric is that noisy at its threshold would have
// been "calibrated" by widening the threshold until the noise fitted inside it.
//
// The relative rotation and atan2 are well conditioned at every separation, including zero, so the
// number below is the rotation and not the rotation plus the conditioning. Measured on the harness's
// golden pair, this takes the reported max on an unperturbed comparison from 4.9e-2 deg to 0.
float QuatAngleDeg(const float a[4], const float b[4]) {
    // rel = conj(a) * b, both taken as unit quaternions.
    const double ax = a[0], ay = a[1], az = a[2], aw = a[3];
    const double bx = b[0], by = b[1], bz = b[2], bw = b[3];
    const double rx = aw * bx - ax * bw - ay * bz + az * by;
    const double ry = aw * by + ax * bz - ay * bw - az * bx;
    const double rz = aw * bz - ax * by + ay * bx - az * bw;
    const double rw = aw * bw + ax * bx + ay * by + az * bz;
    const double vec = sqrt(rx * rx + ry * ry + rz * rz);
    // fabs on the scalar part folds q and -q onto one rotation, which is what makes the result the
    // shorter of the two arcs rather than its 360-degree complement.
    return float(2.0 * atan2(vec, fabs(rw)) * (180.0 / 3.14159265358979323846));
}

O3Result O3Compare(const SkeletonStream& live, const SkeletonStream& replay, const O3Config& cfg) {
    O3Result r;
    memset(&r, 0, sizeof(r));
    r.verdict = Verdict::NotRun;
    r.reason = NotRunReason::NoSamples;
    snprintf(r.detail, sizeof(r.detail), "nothing compared");

    if (!SourceTagValid(live.tag) || !SourceTagValid(replay.tag)) {
        Refuse(r, NotRunReason::SameSource,
               "a stream carries no SourceTag; an untagged side cannot be shown to be independent");
        return r;
    }
    if (live.tag.v == replay.tag.v) {
        Refuse(r, NotRunReason::SameSource,
               "both sides carry SourceTag %llu: one producer, so the comparison cannot fail",
               (unsigned long long)live.tag.v);
        return r;
    }
    if (InstantsAlias(live, replay)) {
        Refuse(r, NotRunReason::AliasedBuffers, "both sides point at one instant array");
        return r;
    }
    if (live.count == 0 || replay.count == 0 || live.instants == nullptr || replay.instants == nullptr) {
        Refuse(r, NotRunReason::NoSamples, "live=%u replay=%u instants", live.count, replay.count);
        return r;
    }

    int bad = FirstNonIncreasing(live);
    if (bad >= 0) {
        Refuse(r, NotRunReason::ShapeMismatch, "live source time is not strictly increasing at index %d", bad);
        return r;
    }
    bad = FirstNonIncreasing(replay);
    if (bad >= 0) {
        Refuse(r, NotRunReason::ShapeMismatch, "replay source time is not strictly increasing at index %d", bad);
        return r;
    }

    double frac = 0.0;
    if (DriftRefused(live, cfg.maxClockDriftFrac, &frac)) {
        Refuse(r, NotRunReason::ClockDrift,
               "live source-time span differs from its own sim-time span by %.1f%%, over the %.1f%% "
               "bound; the producer measured elapsed time with the wrong clock",
               frac * 100.0, double(cfg.maxClockDriftFrac) * 100.0);
        return r;
    }
    if (DriftRefused(replay, cfg.maxClockDriftFrac, &frac)) {
        Refuse(r, NotRunReason::ClockDrift,
               "replay source-time span differs from its own sim-time span by %.1f%%, over the %.1f%% bound",
               frac * 100.0, double(cfg.maxClockDriftFrac) * 100.0);
        return r;
    }

    // Merge join on source time.  Both sides are strictly increasing, so one pass suffices.
    uint32_t i = 0, j = 0;
    while (i < live.count && j < replay.count) {
        const SkeletonInstant& L = live.instants[i];
        const SkeletonInstant& R = replay.instants[j];
        if (L.sourceTimeUs < R.sourceTimeUs) {
            ++r.liveOnlyInstants;
            ++i;
            continue;
        }
        if (R.sourceTimeUs < L.sourceTimeUs) {
            ++r.replayOnlyInstants;
            ++j;
            continue;
        }
        ++i;
        ++j;

        // The no-skew acceptance.  Both readings are taken from the running engine, one before the
        // /session builder and one after the /player_bones builder, in two domains.  Either moving
        // means the pair did not come from one instant, and the numbers derived from it are void.
        const bool skewed = (L.simTimeBeforeUs != L.simTimeAfterUs) ||
                            (L.frameIndexBefore != L.frameIndexAfter) ||
                            (R.simTimeBeforeUs != R.simTimeAfterUs) ||
                            (R.frameIndexBefore != R.frameIndexAfter);
        if (skewed) {
            ++r.skewInstants;
            continue;
        }

        if (L.playerCount != R.playerCount) {
            Refuse(r, NotRunReason::ShapeMismatch,
                   "at source time %llu the two passes carry %u and %u players",
                   (unsigned long long)L.sourceTimeUs, L.playerCount, R.playerCount);
            return r;
        }

        ++r.matchedInstants;
        bool instantClean = true;

        for (uint32_t p = 0; p < L.playerCount; ++p) {
            const BoneFrame& lf = L.players[p];
            const BoneFrame& rf = R.players[p];
            if (lf.playerId != rf.playerId || lf.boneCount != rf.boneCount) {
                Refuse(r, NotRunReason::ShapeMismatch,
                       "at source time %llu player slot %u is id %u/%u with %u/%u bones",
                       (unsigned long long)L.sourceTimeUs, p, lf.playerId, rf.playerId,
                       lf.boneCount, rf.boneCount);
                return r;
            }
            if (FramesAlias(lf, rf)) {
                Refuse(r, NotRunReason::AliasedBuffers,
                       "player %u shares a bone array between the two passes", lf.playerId);
                return r;
            }
            if (lf.boneCount == 0 || lf.boneO == nullptr || lf.boneT == nullptr) {
                Refuse(r, NotRunReason::NoSamples, "player %u carries %u bones", lf.playerId, lf.boneCount);
                return r;
            }

            for (uint32_t b = 0; b < lf.boneCount; ++b) {
                const float* lo = lf.boneO + 4 * b;
                const float* ro = rf.boneO + 4 * b;
                const float* lt = lf.boneT + 3 * b;
                const float* rt = rf.boneT + 3 * b;

                ++r.comparedBones;
                if (BitsEqual(lo, ro, 4) && BitsEqual(lt, rt, 3)) {
                    ++r.bitIdenticalBones;
                }

                const float dp = PosErrorM(lt, rt);
                if (!r.maxPos.seen || dp > r.maxPos.value) {
                    r.maxPos.seen = true;
                    r.maxPos.value = dp;
                    r.maxPos.player = lf.playerId;
                    r.maxPos.bone = b;
                    r.maxPos.sourceTimeUs = L.sourceTimeUs;
                }
                if (dp > cfg.posToleranceM) {
                    ++r.posViolations;
                    instantClean = false;
                }

                const float da = QuatAngleDeg(lo, ro);
                if (!r.maxOri.seen || da > r.maxOri.value) {
                    r.maxOri.seen = true;
                    r.maxOri.value = da;
                    r.maxOri.player = lf.playerId;
                    r.maxOri.bone = b;
                    r.maxOri.sourceTimeUs = L.sourceTimeUs;
                }
                if (da > cfg.oriToleranceDeg) {
                    ++r.oriViolations;
                    instantClean = false;
                }
            }
        }
        if (instantClean) {
            ++r.passedInstants;
        }
    }
    r.liveOnlyInstants += (live.count - i);
    r.replayOnlyInstants += (replay.count - j);

    if (r.skewInstants > 0) {
        Refuse(r, NotRunReason::Skew,
               "%u of %u source-time matches had the sim clock or the frame index move between the "
               "/session and /player_bones builders; the snapshots are not one instant",
               r.skewInstants, r.skewInstants + r.matchedInstants);
        return r;
    }
    if (r.matchedInstants == 0 || r.comparedBones == 0) {
        Refuse(r, NotRunReason::NoSamples,
               "%u matched instants, %llu compared bones; a zero denominator is not a pass",
               r.matchedInstants, (unsigned long long)r.comparedBones);
        return r;
    }

    const double ratio = double(r.passedInstants) / double(r.matchedInstants);
    if (ratio + 1e-9 < double(cfg.minPassRatio)) {
        r.verdict = Verdict::Fail;
        r.reason = NotRunReason::None;
        Note(r, "%u/%u instants clean (%.4f < %.4f); max pos %.6f m bone %u, max ori %.4f deg bone %u",
             r.passedInstants, r.matchedInstants, ratio, double(cfg.minPassRatio),
             double(r.maxPos.value), r.maxPos.bone, double(r.maxOri.value), r.maxOri.bone);
        return r;
    }

    r.verdict = Verdict::Pass;
    r.reason = NotRunReason::None;
    Note(r, "%u/%u instants clean (%.4f); %llu bones; max pos %.6f m bone %u, max ori %.4f deg bone %u",
         r.passedInstants, r.matchedInstants, ratio, (unsigned long long)r.comparedBones,
         double(r.maxPos.value), r.maxPos.bone, double(r.maxOri.value), r.maxOri.bone);
    return r;
}

}  // namespace verify
}  // namespace replay
