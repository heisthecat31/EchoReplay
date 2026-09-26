#include "playback/core/clockpin.h"

#include "engine/scriptgraph_facts.h"

namespace replay::clockpin {

namespace sgf = replay::scriptgraph_facts;

Decision Decide(const Policy& p) {
    Decision d;
    d.arm = p.loaded && p.allowed && p.enabled;
    return d;
}

const char* EventName(Event e) {
    switch (e) {
        case Event::Idle: return "idle";
        case Event::NotFound: return "not-found";
        case Event::Waiting: return "waiting";
        case Event::Observed: return "observed";
        case Event::Armed: return "armed";
        case Event::Pinned: return "pinned";
        case Event::Refused: return "refused";
        case Event::Released: return "released";
    }
    return "?";
}

void Pin::Forget() {
    haveRef_ = false;
    walkedOnce_ = false;
    armedSaid_ = false;
    refused_ = false;
    sawValue_ = false;
}

void Pin::ForgetLoad() {
    wasLoaded_ = false;
    Forget();
}

Report Pin::Step(IEntity& entity, const Policy& p, uint64_t nowMs) {
    Report r;
    if (p.space != space_) {
        Forget();
        space_ = p.space;
    }
    if (!p.loaded) {
        if (!wasLoaded_) return r;
        r.event = Event::Released;
        if (haveRef_ && entity.ScriptGraphValid(ref_)) {
            r.haveBefore = entity.ScriptGetReal(ref_, sgf::kSymGameTimer, r.before);
        }
        r.ref = ref_;
        ForgetLoad();
        return r;
    }
    wasLoaded_ = true;
    if (p.space == 0) {
        r.event = Event::Waiting;
        r.why = "no level space";
        return r;
    }

    if (haveRef_ && !entity.ScriptGraphValid(ref_)) haveRef_ = false;
    if (!haveRef_) {
        if (walkedOnce_ && nowMs - lastWalkMs_ < kRewalkIntervalMs) {
            r.event = Event::Waiting;
            return r;
        }
        walkedOnce_ = true;
        lastWalkMs_ = nowMs;
        void* spaces[kSpaceCap] = {};
        // The chain starts at the bound game space; an empty chain still walks that (null).
        const uint32_t n = entity.DiscCandidateSpaces(p.netGame, spaces, kSpaceCap);
        const uint32_t walks = n ? n : 1;
        for (uint32_t i = 0; i < walks && !haveRef_; ++i) {
            ++r.spacesWalked;
            haveRef_ = entity.FindScriptGraph(spaces[i], sgf::kArenaScriptHash, 0, 0, ref_, &r.why);
        }
        if (!haveRef_) {
            r.event = Event::NotFound;
            return r;
        }
    }
    r.ref = ref_;

    r.haveBefore = entity.ScriptGetReal(ref_, sgf::kSymGameTimer, r.before);
    if (r.haveBefore && !sawValue_) {
        r.firstSight = true;
        sawValue_ = true;
    }

    const Decision d = Decide(p);
    if (!d.arm || refused_) {
        r.event = Event::Observed;
        return r;
    }

    r.setAnswered = entity.ScriptSetReal(ref_, sgf::kSymGameTimer, sgf::kGameTimerId, d.value);
    r.haveAfter = entity.ScriptGetReal(ref_, sgf::kSymGameTimer, r.after);
    if (!r.setAnswered || !r.haveAfter || r.after != d.value) {
        refused_ = true;
        r.event = Event::Refused;
        return r;
    }
    r.event = armedSaid_ ? Event::Pinned : Event::Armed;
    armedSaid_ = true;
    return r;
}

}  // namespace replay::clockpin
