#pragma once
// DESIGN-HOST-SYNC P1. The host's own round clock, pinned while a replay is loaded.
//
// The arena gametype graph counts its script-local `game_timer` down by graph->deltatime and leaves
// main_loop at zero -- mid replay, whatever the recorded clock says. Its first-round seed never
// passes through the netint the old hold owned (DESIGN-HOST-SYNC 1.2), so the pin writes the
// variable itself, through the graph's own `set` binding, every sweep, and reads it back. Nothing
// is written at unload: restoring an earlier value would expire the idle host's round a moment
// later, and 3600 keeps it in main_loop, which is the state the next load wants.

#include <stdint.h>

#include "engine/entity.h"

namespace replay::clockpin {

inline constexpr float kPinValue = 3600.0f;
// A lost ref is re-walked at most this often. The walk is O(rows) over a few hundred rows.
inline constexpr uint64_t kRewalkIntervalMs = 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 ms, not the CR15Game vtable slot

struct Policy {
    bool loaded = false;
    bool allowed = false;   // livesafety::Allowed(Feature::HostClockPin)
    bool enabled = true;    // playback.host_clock_pin
    uint64_t space = 0;     // levelspace::Key; a change drops the ref, 0 touches nothing
    void* netGame = nullptr;  // the tick's own: seeds the space chain the walk searches
};

// The walk searches every space of the chain the disc bind searches -- the bound game space is only
// one of them, and RUN-VERIFY-4's host had no arena graph on it.
inline constexpr uint32_t kSpaceCap = 8;

struct Decision {
    bool arm = false;
    float value = kPinValue;
};

Decision Decide(const Policy& p);

enum class Event : uint8_t {
    Idle,       // not loaded and nothing to release
    NotFound,   // this sweep walked and found no arena graph; `why` names it
    Waiting,    // no ref and the re-walk interval has not passed, or no level space to walk
    Observed,   // read only: disarmed, or refused earlier this load
    Armed,      // the first successful pin of this load
    Pinned,     // every later successful pin
    Refused,    // the setter refused or the read-back disagreed; latched until the next load
    Released,   // the unload tick: nothing written, the value read and left
};
const char* EventName(Event e);

struct Report {
    Event event = Event::Idle;
    bool haveBefore = false;
    float before = 0.0f;       // the value the sweep found, before any write
    bool firstSight = false;   // the first value read since this load began
    bool setAnswered = false;
    bool haveAfter = false;
    float after = 0.0f;
    const char* why = nullptr;
    uint32_t spacesWalked = 0;  // NotFound and a new ref: how many spaces the walk searched
    IEntity::ScriptGraphRef ref;
};

class Pin {
public:
    Report Step(IEntity& entity, const Policy& p, uint64_t nowMs);
    // Drops the ref and everything learned from the graph it points at; the next sweep in a level
    // space walks again at once and logs its first sight and ARMED as a new level's.
    void Forget();

private:
    void ForgetLoad();

    IEntity::ScriptGraphRef ref_;
    bool haveRef_ = false;
    uint64_t space_ = 0;
    bool walkedOnce_ = false;
    uint64_t lastWalkMs_ = 0;
    bool wasLoaded_ = false;
    bool armedSaid_ = false;
    bool refused_ = false;
    bool sawValue_ = false;
};

}  // namespace replay::clockpin
