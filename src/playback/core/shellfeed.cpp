#include "playback/core/shellfeed.h"
#include "engine/scriptgraph_facts.h"

namespace replay::shellfeed {

using shellplan::Colour;

const char* ActionName(Action a) {
    switch (a) {
        case Action::None: return "none";
        case Action::Show: return "show";
        case Action::Hide: return "hide";
        case Action::Refused: return "refused";
    }
    return "?";
}

ShowEvents ShowEventsFor(Colour colour) {
    namespace sgf = replay::scriptgraph_facts;
    ShowEvents e;
    e.fxState = colour == Colour::Red ? sgf::kSymFxStateRed : sgf::kSymFxStateBlue;
    e.spawn = colour == Colour::Red ? sgf::kSymSpawnOrange : sgf::kSymSpawnBlue;
    return e;
}

Observed ObserveWords(bool read, int32_t w1SpawnBlue, int32_t w6SpawnOrange) {
    Observed o;
    o.read = read;
    if (!read) return o;
    o.on = w1SpawnBlue > 0 || w6SpawnOrange > 0;
    o.colour = w6SpawnOrange > 0 ? Colour::Red : Colour::Blue;
    return o;
}

Step Reconciler::Next(const Input& in) {
    Step out;
    out.k = in.want.k;
    if (!in.allowed || !in.want.known || !in.obs.read) return out;

    Action action = Action::None;
    bool residual = false;
    if (in.want.show) {
        if (!in.obs.on) action = Action::Show;
        else if (in.obs.colour != in.want.colour) action = Action::Hide;
    } else if (in.obs.on && in.transport_set && !in.disc_owned) {
        action = Action::Hide;
        residual = true;
    }
    if (action == Action::None) {
        pending_ = false;
        return out;
    }

    Key key;
    key.k = in.want.k;
    key.epoch = in.epoch;
    key.action = action;
    key.colour = in.want.colour;

    const bool samePending = pending_ && pendingKey_.k == key.k &&
                             pendingKey_.action == key.action && pendingKey_.colour == key.colour;
    if (samePending) {
        if (!in.clock_running) {
            pendingSinceMs_ = in.now_ms;
            return out;
        }
        if (in.now_ms - pendingSinceMs_ < kRetryMs) return out;
    }

    if (!latched_ || latchKey_.k != key.k || latchKey_.epoch != key.epoch ||
        latchKey_.action != key.action || latchKey_.colour != key.colour) {
        latched_ = true;
        latchKey_ = key;
        attempts_ = 0;
        refusedSaid_ = false;
    }
    if (attempts_ >= 1 + kMaxRetries) {
        if (refusedSaid_) return out;
        refusedSaid_ = true;
        out.action = Action::Refused;
        out.attempts = attempts_;
        return out;
    }

    out.retry = attempts_ > 0;
    ++attempts_;
    pending_ = true;
    pendingKey_ = key;
    pendingSinceMs_ = in.now_ms;
    out.action = action;
    out.colour = in.want.colour;
    out.residual = residual;
    out.attempts = attempts_;
    return out;
}

bool BoundDiscActor(const IEntity::DiscRingReading& ring, uint32_t trackPhysId, uint64_t& actorId,
                    const char** why) {
    using Reading = IEntity::DiscRingReading;
    const Reading::Actor* bound = nullptr;
    for (uint32_t i = 0; i < ring.actorsWritten && i < Reading::kActorCap; ++i) {
        if (!ring.actors[i].bound) continue;
        if (bound) {
            *why = "two disc-ring census actors claim the bound disc";
            return false;
        }
        bound = &ring.actors[i];
    }
    if (!bound) {
        *why = "no disc-ring census actor is the bound disc";
        return false;
    }
    if (bound->physId != trackPhysId) {
        *why = "the census's bound disc actor carries another component id than the disc track drives";
        return false;
    }
    actorId = bound->actorId;
    return true;
}

}  // namespace replay::shellfeed
