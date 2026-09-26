#include "verify/o2.h"

#include <stdio.h>
#include <string.h>

namespace replay {
namespace verify {

namespace {

const char* const kDomainNames[] = {"published", "render-slot", "phys-slot", "phys-body"};

bool BitEqual(float a, float b) {
    uint32_t x, y;
    memcpy(&x, &a, 4);
    memcpy(&y, &b, 4);
    return x == y;
}

void FieldName(const StateView& v, uint32_t i, char* out, size_t cap) {
    if (v.names != nullptr && v.names[i] != nullptr) {
        snprintf(out, cap, "%s", v.names[i]);
    } else {
        snprintf(out, cap, "field[%u]", i);
    }
}

}  // namespace

const char* O2DomainName(O2Domain d) {
    const uint8_t i = uint8_t(d);
    return i < uint8_t(O2Domain::kCount) ? kDomainNames[i] : "?";
}

// The published domain is what we wrote and read straight back.  Scoring it would inflate the
// verdict with self-agreement, so it is reported and the verdict never touches it.
bool O2DomainIsAsserted(O2Domain d) {
    return d != O2Domain::Published;
}

O2Result O2Compare(const O2Streams& s) {
    O2Result r;
    memset(&r, 0, sizeof(r));
    r.verdict = Verdict::NotRun;
    r.reason = NotRunReason::NoSamples;
    snprintf(r.detail, sizeof(r.detail), "nothing compared");

    if (!SourceTagValid(s.fileTag) || !SourceTagValid(s.engineTag)) {
        r.reason = NotRunReason::SameSource;
        snprintf(r.detail, sizeof(r.detail),
                 "an untagged side cannot be shown to be independent of the other");
        return r;
    }
    if (s.fileTag.v == s.engineTag.v) {
        r.reason = NotRunReason::SameSource;
        snprintf(r.detail, sizeof(r.detail),
                 "the decoder and the engine read-back carry one SourceTag (%llu); O2 would be "
                 "comparing the file against itself",
                 (unsigned long long)s.fileTag.v);
        return r;
    }
    if (s.samples == nullptr || s.count == 0) {
        return r;
    }

    for (uint32_t k = 0; k < s.count; ++k) {
        const O2Sample& smp = s.samples[k];
        ++r.samples;
        for (uint8_t d = 0; d < uint8_t(O2Domain::kCount); ++d) {
            const O2DomainSample& ds = smp.domains[d];
            O2DomainResult& out = r.byDomain[d];
            if (!ds.valid) {
                ++out.samplesSkipped;
                continue;
            }
            if (ds.expected.count != ds.observed.count) {
                r.reason = NotRunReason::ShapeMismatch;
                snprintf(r.detail, sizeof(r.detail),
                         "%s: sample %u offers %u expected and %u observed fields",
                         O2DomainName(O2Domain(d)), k, ds.expected.count, ds.observed.count);
                return r;
            }
            if (ds.expected.count != 0 && ds.expected.values == ds.observed.values) {
                r.reason = NotRunReason::AliasedBuffers;
                snprintf(r.detail, sizeof(r.detail),
                         "%s: sample %u hands one buffer to both sides, so no difference is "
                         "representable",
                         O2DomainName(O2Domain(d)), k);
                return r;
            }
            ++out.samplesValid;
            for (uint32_t i = 0; i < ds.expected.count; ++i) {
                ++out.compared;
                if (BitEqual(ds.expected.values[i], ds.observed.values[i])) {
                    continue;
                }
                ++out.mismatched;
                if (out.firstMismatch[0] == '\0') {
                    char nm[48];
                    FieldName(ds.expected, i, nm, sizeof(nm));
                    snprintf(out.firstMismatch, sizeof(out.firstMismatch),
                             "entity %u at %llu us: %s file=%.9g engine=%.9g", smp.entity,
                             (unsigned long long)smp.sourceTimeUs, nm,
                             double(ds.expected.values[i]), double(ds.observed.values[i]));
                }
            }
        }
    }

    for (uint8_t d = 0; d < uint8_t(O2Domain::kCount); ++d) {
        if (!O2DomainIsAsserted(O2Domain(d))) {
            continue;
        }
        r.assertedCompared += r.byDomain[d].compared;
        r.assertedMismatched += r.byDomain[d].mismatched;
    }

    // RULE 0e.  The engine-authored domains are the ones that carry the claim; if none of them was
    // readable, the published domain agreeing with itself is not a result.
    if (r.assertedCompared == 0) {
        r.reason = NotRunReason::Unavailable;
        snprintf(r.detail, sizeof(r.detail),
                 "%u sample(s), but no engine-authored field was readable; the published domain "
                 "compared %llu field(s) against our own write and that is not a verdict",
                 r.samples, (unsigned long long)r.byDomain[uint8_t(O2Domain::Published)].compared);
        return r;
    }

    r.reason = NotRunReason::None;
    r.verdict = (r.assertedMismatched == 0) ? Verdict::Pass : Verdict::Fail;
    snprintf(r.detail, sizeof(r.detail),
             "asserted %llu/%llu fields across render=%llu phys=%llu body=%llu; published (reported "
             "only) %llu/%llu",
             (unsigned long long)r.assertedMismatched, (unsigned long long)r.assertedCompared,
             (unsigned long long)r.byDomain[uint8_t(O2Domain::RenderSlot)].compared,
             (unsigned long long)r.byDomain[uint8_t(O2Domain::PhysSlot)].compared,
             (unsigned long long)r.byDomain[uint8_t(O2Domain::PhysBody)].compared,
             (unsigned long long)r.byDomain[uint8_t(O2Domain::Published)].mismatched,
             (unsigned long long)r.byDomain[uint8_t(O2Domain::Published)].compared);
    return r;
}

}  // namespace verify
}  // namespace replay
