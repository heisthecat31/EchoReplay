#pragma once

#include <stdint.h>

namespace replay {

// A loaded replay outlives the session it was playing in: the owner can leave the match, or quit,
// with the file still open. Whatever writes engine state per tick stops on that edge, and whatever
// holds engine state hands it back there, while the engine still owns the objects -- process exit
// is too late, they are freed by then. A load that has not reached a match yet is not an ended
// session, so level loading keeps the behaviour it had.
struct SessionWatch {
    bool sawInMatch = false;
    bool inMatchPrev = false;
    bool released = false;
    uint32_t loadSerial = 0;

    bool Ended(bool inMatchNow) {
        // The RISING edge starts a session. One process plays more than one match -- a private
        // lobby rematch is the ordinary case -- and without this the second match's claims are
        // made against a release the first match already spent, so nothing is ever handed back
        // and every body it claimed stays net-driven with nothing driving it.
        if (inMatchNow && !inMatchPrev) {
            sawInMatch = true;
            released = false;
        }
        inMatchPrev = inMatchNow;
        return !inMatchNow && sawInMatch;
    }

    // True on the first tick of an ended session and false on every later one, so the release runs
    // once per session rather than once per load.
    bool ReleaseDue() {
        if (released) return false;
        released = true;
        return true;
    }

    // For a watcher that does not perform the load itself and only sees the transport: the watch
    // belongs to one load (IReplayTransport::LoadSerial). A second replay opened in the same process
    // loads its level out of match exactly as the first did, and must not read as the tail of the
    // first one's session.
    bool Ended(uint32_t serial, bool inMatchNow) {
#ifndef EXITFIX_MUTATE_WATCH_SPANS_LOADS
        if (serial != loadSerial) {
            *this = SessionWatch{};
            loadSerial = serial;
        }
#endif
        return Ended(inMatchNow);
    }
};

}  // namespace replay
