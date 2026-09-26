#include "audio/foleymix.h"

#include <string.h>

namespace replay::audio {

bool ParseFoleyMix(const char* text, FoleyMix* out) {
    if (text == nullptr || out == nullptr) {
        return false;
    }
    if (strcmp(text, "engine") == 0) {
        *out = FoleyMix::Engine;
        return true;
    }
    if (strcmp(text, "headset") == 0) {
        *out = FoleyMix::Headset;
        return true;
    }
    if (strcmp(text, "broadcast") == 0) {
        *out = FoleyMix::Broadcast;
        return true;
    }
    return false;
}

bool FoleyMixFromConfig(const IConfig* cfg, FoleyMix* out, const char** text) {
    const char* word = cfg != nullptr ? cfg->GetString(kFoleyMixKey, "headset") : nullptr;
    if (text != nullptr) {
        *text = word;
    }
    *out = FoleyMix::Engine;
    return ParseFoleyMix(word, out);
}

const char* FoleyMixName(FoleyMix mix) {
    switch (mix) {
        case FoleyMix::Engine: return "engine";
        case FoleyMix::Headset: return "headset";
        case FoleyMix::Broadcast: return "broadcast";
    }
    return "?";
}

FoleyMixHold::FoleyMixHold(FoleyMix mix, uint64_t pollIntervalMs)
    : mix_(mix), pollIntervalMs_(pollIntervalMs) {}

uint32_t FoleyMixHold::WantedState() const {
    return mix_ == FoleyMix::Broadcast ? kFoleyMixBroadcastState : kFoleyMixHeadsetState;
}

void FoleyMixHold::Reset() {
    polled_ = false;
    nextPollMs_ = 0;
}

FoleyMixOutcome FoleyMixHold::Tick(IAudioState* state, uint64_t nowMs) {
    if (mix_ == FoleyMix::Engine) {
        return FoleyMixOutcome::Idle;
    }
    if (polled_ && nowMs < nextPollMs_) {
        return FoleyMixOutcome::Idle;
    }
    polled_ = true;
    nextPollMs_ = nowMs + pollIntervalMs_;
    ++report_.polls;
    if (state == nullptr) {
        ++report_.refusals;
        return FoleyMixOutcome::Refused;
    }
    Symbol64 truth = 0;
    if (!state->GetStateTruth(kFoleyMixStateGroup, truth)) {
        ++report_.refusals;
        return FoleyMixOutcome::Refused;
    }
    report_.lastSeen = static_cast<uint32_t>(truth);
    const uint32_t wanted = WantedState();
    if (report_.lastSeen == wanted) {
        return FoleyMixOutcome::Held;
    }
    if (!state->SetState(kFoleyMixStateGroup, wanted)) {
        ++report_.refusals;
        return FoleyMixOutcome::Refused;
    }
    ++report_.corrections;
    return FoleyMixOutcome::Corrected;
}

}  // namespace replay::audio
