#include "verify/verify.h"

namespace replay {
namespace verify {

const char* VerdictName(Verdict v) {
    switch (v) {
        case Verdict::NotRun: return "NOT-RUN";
        case Verdict::Pass:   return "PASS";
        case Verdict::Fail:   return "FAIL";
    }
    return "?";
}

const char* NotRunReasonName(NotRunReason r) {
    switch (r) {
        case NotRunReason::None:           return "-";
        case NotRunReason::NoSamples:      return "no-samples";
        case NotRunReason::SameSource:     return "same-source";
        case NotRunReason::AliasedBuffers: return "aliased-buffers";
        case NotRunReason::Skew:           return "skew";
        case NotRunReason::ShapeMismatch:  return "shape-mismatch";
        case NotRunReason::ClockDrift:     return "clock-drift";
        case NotRunReason::Unavailable:    return "unavailable";
        case NotRunReason::NoBaseline:     return "no-baseline";
    }
    return "?";
}

bool SourceTagValid(SourceTag t) {
    return t.v != 0;
}

// FNV-1a over the producer name, mixed with the caller's nonce.  Two producers that pick the same
// name and nonce collide, and that is the correct outcome: they have not established that they are
// two producers.  The final OR keeps zero -- the "no tag at all" value -- out of the range.
SourceTag MintSourceTag(const char* producer, uint64_t nonce) {
    uint64_t h = 1469598103934665603ull;
    for (const char* p = producer; p != nullptr && *p != '\0'; ++p) {
        h ^= uint64_t(uint8_t(*p));
        h *= 1099511628211ull;
    }
    h ^= nonce;
    h *= 1099511628211ull;
    SourceTag t;
    t.v = h | 1ull;
    return t;
}

}  // namespace verify
}  // namespace replay
