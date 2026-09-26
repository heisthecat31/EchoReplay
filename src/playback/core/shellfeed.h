#pragma once
// DESIGN-HOST-SYNC P4. Reconciles the disc FX graph's shell with shellplan's Want, per sweep.
//
// Pure: it sees what the plan wants, what the graph shows (its two particle-spawn sentinels), and
// how the playhead got here, and answers with at most one delivery. The module performs it.
//
//  - want && !on: show, forceSetFXState_<colour>.
//  - want && on in the other colour (a seek across restarts): hide now, show on a later sweep.
//  - !want && on: normally NOTHING -- the first recorded grab reaches the engine as a possession
//    level and the FX graph's own OnNetTouchGrab arm clears the shell. Only a playhead set by seek,
//    scrub or reverse, on a disc that is unowned at t, can have no grab edge coming; then the hide
//    is OnNetTouchGrab delivered to the FX graph alone.
//
// Deliveries are latched per (span, seek epoch, action, colour): up to 1 + kMaxRetries attempts,
// each retry at least kRetryMs of RUNNING clock after the last -- while the engine clock is paused
// the graph cannot act on an event, so a paused drag across many seeks sends each wanted delivery
// once and waits, rather than one per seek.

#include <stdint.h>

#include "engine/entity.h"
#include "playback/core/shellplan.h"

namespace replay::shellfeed {

inline constexpr uint64_t kRetryMs = 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 ms, not the CR15Game vtable slot
inline constexpr uint32_t kMaxRetries = 3;

enum class Action : uint8_t { None, Show, Hide, Refused };
const char* ActionName(Action a);

struct Observed {
    bool read = false;
    bool on = false;
    shellplan::Colour colour = shellplan::Colour::Blue;   // meaningful only when `on`
};
// w6 (spawn_orange_particles) wins: the orange thread is the Red state's.
Observed ObserveWords(bool read, int32_t w1SpawnBlue, int32_t w6SpawnOrange);

struct Input {
    shellplan::Want want;
    Observed obs;
    bool allowed = false;         // livesafety::Allowed(Feature::DiscShellFeed) && playback.disc_shell
    bool transport_set = false;   // the playhead was last placed by seek, scrub or reverse
    bool disc_owned = false;      // the recorded disc holder at t is a player
    uint32_t epoch = 0;           // bumped by every user seek
    uint64_t now_ms = 0;
    bool clock_running = true;
};

struct Step {
    Action action = Action::None;
    shellplan::Colour colour = shellplan::Colour::Blue;   // Show: the state to send
    bool residual = false;       // Hide because no grab edge can come
    bool retry = false;
    uint32_t attempts = 0;       // including this one
    int32_t k = -1;
};

// The disc-ring census row of the actor the disc track drives: the one the bind marked, which must
// carry the same component id as the track. None, two, or a disagreeing id refuse with `why`.
bool BoundDiscActor(const IEntity::DiscRingReading& ring, uint32_t trackPhysId, uint64_t& actorId,
                    const char** why);

// A Show delivers two events into the FX graph, both the graph's own self-events. The first is the
// state the disc controller sends (it also sets the FX-state symbol). Its thread is gated on
// R15NetIsDedicated before it self-delivers the particle arm, and on the replay host that arm
// never followed: RUN-VERIFY-4 saw the state sentinel w5 rise within 0.5 s of every delivery and
// the particle sentinel w6 stay 0 for all four. So the second event is that particle arm itself,
// built the way the graph's own node c9ecb89d builds it; its handler is a no-op while it runs.
struct ShowEvents {
    uint64_t fxState = 0;
    uint64_t spawn = 0;
};
ShowEvents ShowEventsFor(shellplan::Colour colour);

// "The first grab cleared the shell" once per (span, seek epoch). The particle sentinels also pulse
// on ordinary catches later in a span, and each of those fall looked like a first grab: 106 lines
// in one RUN-VERIFY-4 session for 7 spans.
class GrabClearLatch {
public:
    // True exactly when this fall is the first one seen for (k, epoch).
    bool Fall(int32_t k, uint32_t epoch) {
        if (said_ && k == k_ && epoch == epoch_) return false;
        said_ = true;
        k_ = k;
        epoch_ = epoch;
        return true;
    }
    void Reset() { *this = GrabClearLatch(); }

private:
    bool said_ = false;
    int32_t k_ = -1;
    uint32_t epoch_ = 0;
};

class Reconciler {
public:
    Step Next(const Input& in);
    void Reset() { *this = Reconciler(); }

private:
    struct Key {
        int32_t k = -1;
        uint32_t epoch = 0;
        Action action = Action::None;
        shellplan::Colour colour = shellplan::Colour::Blue;
    };

    bool pending_ = false;
    Key pendingKey_;          // epoch is not part of the pending comparison
    uint64_t pendingSinceMs_ = 0;
    bool latched_ = false;
    Key latchKey_;
    uint32_t attempts_ = 0;
    bool refusedSaid_ = false;
};

}  // namespace replay::shellfeed
