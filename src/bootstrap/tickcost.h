// PERF-1's permanent instrument: where the plugin's per-frame microseconds go.
//
// The capture side has had `capture::CostMeter` since W1, and it is the only reason capture could be
// cleared of the 27 fps complaint in one line ("gate 0.9 us mean, max 5"). Playback had no equivalent,
// so DIAG-PERF-27FPS had to reconstruct "2.4 ms per recorded player" from thirty sessions of log
// archaeology. This is the line that answers it directly, once a second, in every launch.
//
// It lives in bootstrap rather than in playback because the engine enters this plugin from three places
// it drives independently -- the playback tick, the per-netgame-update stage sweep (about four times a
// rendered frame), and the UI draw -- and those are three different libraries. Each is a ROOT; the named
// phases nest inside them. `other` is whatever the roots cost that no named phase claimed, which is
// where the next unmeasured regression will surface first.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace replay {
namespace tickcost {

enum Phase : uint8_t {
    kApply,       // the pose/state write pass
    kHud,         // HUD netvar sweep
    kStats,       // stat feed
    kGoals,       // goal feed
    kStuns,       // stun feed
    kDress,       // identity / dress ladder
    kJump,        // the discontinuity battery a seek or micro-seek fires: cursor invalidation, the
                  // synthetic-owner release and every category-N residue row. Unmeasured until
                  // PERF-2, which is how ~100 ms a frame hid in `other` for the whole of the 10 fps
    kEvents,      // the component-event feed, on the stage sweep outside the HUD phase
    kWindow,      // the rolling window: the covering rebuild and the idle re-centre, decode included
    kNetVar,      // the generic netvar bind check and the HUD key resolve/publish, on the tick
    kSweepTail,   // the stage sweep's own prologue: stat release, roster leave, engine user counts
    kHudSite,     // NESTED INSIDE kHud: re-validating the netvar route, once per sweep
    kHudHolds,    // NESTED INSIDE kHud: the two host-hold keys and their read-backs
    kSeat,        // the seat/viewer census, run above the load gate on every tick
    kProvision,   // the per-tick provisioning ladder: LOD throttle, netvar bind, owner guard, the
                  // provisioner, the reply route and the slot map
    kDrive,       // the per-tick drivers that are not the pose apply: score gate, oracle track, disc
                  // bind, host phase and the host holds
    kAudioRate,   // the rate driver and the sink rate tick: probes, COM calls, the re-arm loop
    kAudioVoices, // the voice tracker
    kAudioPins,   // the keyframe pin ladder
    kRootTick,    // the whole playback tick
    kRootSweep,   // the whole netgame stage sweep
    kRootUi,      // the whole in-frame UI draw
    kRootAudio,   // the whole audio module tick -- a ROOT, because the engine enters it separately
    kPhaseCount
};

// Raw QueryPerformanceCounter deltas. Callers normally use Scope.
void Add(Phase p, int64_t ticks);

// One engine frame, with the number of players the loaded recording drives. Called once per tick.
void NoteFrame(uint32_t recordedPlayers);

// Renders the `cost:` line into `out` and starts a new window. False when the window saw no frames,
// so a 1 Hz caller stays silent while nothing is running.
bool FormatAndReset(char* out, size_t cap);

// Microseconds a phase has accumulated in the window still in progress.
uint64_t PhaseUsForTest(Phase p);
void     ResetForTest();

// ---- AUDIO-FIX-1 part B: the hitch recorder ---------------------------------------------------
//
// A 1 Hz frame COUNT cannot tell nine 111 ms frames from one 900 ms stall, and those are different
// bugs that currently print identically. So: one bucket increment per frame for the distribution,
// and one WARN line naming what the plugin was doing on the frame that was late.

// One frame's wall delta, in milliseconds. Cheap by construction: one bucket index and one
// increment, no allocation, no sort.
void NoteFrameDt(double dtMs);

struct DtSummary {
    double   p50 = 0.0;
    double   p95 = 0.0;
    double   p99 = 0.0;
    double   max = 0.0;
    uint32_t frames = 0;
};
// Drains the second's distribution. Percentiles are bucket-resolution, which is what a log line
// needs: the question is 8 ms versus 900 ms, not 8.0 versus 8.1.
DtSummary TakeDtSummary();

// True when this frame deserves a hitch line: late, or a second that has clamped more than twice.
// Rate-limited to 8 lines a second and 40 a minute; `suppressed` carries what the limit swallowed
// since the last line, so a burst is never silently shortened.
bool HitchDue(double dtMs, uint32_t clampsThisSecond, uint32_t* suppressed);
constexpr double kHitchDtMs = 25.0;
constexpr uint32_t kHitchClamps = 2;

// Per-phase microseconds charged on the frame BEFORE the current one, snapshotted by NoteFrame.
// The 1 Hz `cost:` line is a mean and a mean cannot describe a 111 ms frame.
bool FormatFrameCost(char* out, size_t cap);

// Two QPC reads and one relaxed fetch_add: about 40 ns, which is the budget this meter has to stay
// inside to be worth leaving switched on.
class Scope {
public:
    explicit Scope(Phase p);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    // Ends the phase early, for a region whose end is not a brace. Idempotent; the destructor of a
    // scope already closed adds nothing.
    void Close();

private:
    int64_t start_;
    Phase   phase_;
};

}  // namespace tickcost
}  // namespace replay
