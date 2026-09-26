#include "playback/core/hosthold.h"

#include <atomic>

#include "bootstrap/log.h"

namespace replay {
namespace playback {
namespace hosthold {
namespace {

struct State {
    std::atomic<uint32_t> borrow_depth{0};
    // Tick-thread only.
    uint64_t    borrow_us = 0;        // how long the current borrow has been outstanding
    bool        borrow_said = false;  // the watchdog has already complained about this one
    bool        pause_armed = false;
    uint64_t    outside_us = 0;       // ...and how long the host has been seen outside main_loop
    const char* edge_phase = "not published";
};
State g;

}  // namespace

Want Step(const Policy& p, uint64_t wall_us) {
    // The watchdog first: a borrow that outlives its window has to stop suppressing the hold, or a
    // caller that faulted between Borrow and Return takes the whole viewing with it.
    if (g.borrow_depth.load(std::memory_order_acquire) != 0) {
        g.borrow_us += wall_us;
        if (g.borrow_us >= kBorrowWatchdogUs) {
            if (!g.borrow_said) {
                g.borrow_said = true;
                log::Direct(LogLevel::Error, "playback",
                            "host hold: a borrow has been outstanding for %.1f s and is being force-"
                            "returned -- the borrower did not put it back",
                            double(g.borrow_us) / 1e6);
            }
            g.borrow_depth.store(0, std::memory_order_release);
            g.borrow_us = 0;
        }
    } else {
        g.borrow_us = 0;
        g.borrow_said = false;
    }
    const bool borrowed = g.borrow_depth.load(std::memory_order_relaxed) != 0;

    if (!p.loaded) {
        g.pause_armed = false;
        g.outside_us = 0;
        return Want{};
    }

    if (p.playing) {
        g.outside_us = 0;
        if (!g.pause_armed) g.edge_phase = p.phase_name;
        g.pause_armed = true;
    } else if (g.pause_armed) {
        // Not a release yet. The host walks a countdown or two between main_loop runs even with the
        // round clock held, and dropping the hold on the first frame outside would make it flap.
        g.outside_us += wall_us;
        if (g.outside_us >= kOutsideMainLoopGraceUs) {
            g.pause_armed = false;
            g.outside_us = 0;
            g.edge_phase = p.phase_name;
        }
    }

    Want w;
    w.round_time = p.round_time_on && !borrowed;
    w.pause = p.pause_on && g.pause_armed && !borrowed;
    return w;
}

void Borrow() { g.borrow_depth.fetch_add(1, std::memory_order_acq_rel); }

void Return() {
    uint32_t seen = g.borrow_depth.load(std::memory_order_relaxed);
    while (seen != 0 &&
           !g.borrow_depth.compare_exchange_weak(seen, seen - 1, std::memory_order_acq_rel,
                                                 std::memory_order_relaxed)) {
    }
}

uint32_t BorrowDepth() { return g.borrow_depth.load(std::memory_order_relaxed); }

const char* LastPauseEdgePhase() { return g.edge_phase; }

void ResetForTest() {
    g.borrow_depth.store(0, std::memory_order_relaxed);
    g.borrow_us = 0;
    g.borrow_said = false;
    g.pause_armed = false;
    g.outside_us = 0;
    g.edge_phase = "not published";
}

}  // namespace hosthold
}  // namespace playback
}  // namespace replay
