#include "playback/core/discontinuity.h"

namespace replay {
namespace playback {

const char* CauseName(Cause c) {
    switch (c) {
        case Cause::Seek: return "seek";
        case Cause::MicroSeek: return "micro-seek";
        case Cause::GapCross: return "gap-cross";
        case Cause::ReverseTick: return "reverse-tick";
        case Cause::ChapterEpochRoll: return "epoch-roll";
        default: return "?";
    }
}

const char* AvailabilityName(Availability a) {
    switch (a) {
        case Availability::Ready: return "ready";
        case Availability::Unmapped: return "unmapped";
        case Availability::RefusedByConfig: return "refused-by-config";
        case Availability::FailedAtRuntime: return "failed-at-runtime";
        default: return "?";
    }
}

bool ResidueBattery::Register(IResidue* r) {
    if (!r || count_ >= kMax) {
        return false;
    }
    items_[count_++] = r;
    return true;
}

size_t ResidueBattery::Fire(const Discontinuity& d) {
    size_t ran = 0;
    for (size_t i = 0; i < count_; ++i) {
        IResidue* r = items_[i];
        if (!r->RunsOn(d.cause)) {
            continue;
        }
        if (r->Avail() != Availability::Ready) {
            ++skipped_;   // counted, never silent -- the whole point of the Availability enum
            continue;
        }
        r->Fire(d);
        ++ran;
        ++fired_;
    }
    return ran;
}

size_t ResidueBattery::UnavailableCount() const {
    size_t n = 0;
    for (size_t i = 0; i < count_; ++i) {
        if (items_[i]->Avail() != Availability::Ready) {
            ++n;
        }
    }
    return n;
}

size_t ResidueBattery::UnavailableIds(const char** out, size_t cap) const {
    size_t n = 0;
    for (size_t i = 0; i < count_; ++i) {
        if (items_[i]->Avail() != Availability::Ready) {
            if (out && n < cap) {
                out[n] = items_[i]->Id();
            }
            ++n;
        }
    }
    return n;
}

}  // namespace playback
}  // namespace replay
