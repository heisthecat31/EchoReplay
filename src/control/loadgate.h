#pragma once

// W2-G. "Is there a capture to drive?", as a pure function, so the offline suite can sweep every
// answer -- tests/control links replay_control_core and reaches no transport and no replay::ui.
//
// TWO SOURCES ANSWER THIS AND THEY CAN DISAGREE. replay::ui holds the path the user chose;
// the transport holds the file. Asking only the transport POINTER was the original defect: the
// object exists from module init onward, so every transport command reported success over a
// Play() that returned at its first line. Asking only replay::ui is the same defect one seam
// later -- it is what makes `speed 2` print "speed 2 (playing)" against a fileless transport now
// that W2-B has given the transport a word for "no file". Both must say yes.
//
// A DISAGREEMENT IS A REFUSAL, NEVER A GUESS. If the panel names a path and the transport says
// NotLoaded, the load is in flight or it failed; driving it would command a capture that is not
// there, and the command is refused with the reason rather than accepted and ignored.

#include "playback/core/transport.h"

namespace replay {
namespace control {

// `state` is only consulted when `transportPresent`; pass NotLoaded when there is no transport.
inline bool CaptureIsOpen(bool uiHasPath, bool transportPresent, TransportState state) {
    return uiHasPath && transportPresent && state != TransportState::NotLoaded;
}

// FOUR-VALUED, and that is the point. `State() != Pause` meant "running", so the moment
// TransportState gained NotLoaded a fileless transport started reporting "playing". Every value
// gets its own word, and the one that means "there is no file" does not borrow another's.
inline const char* TransportRunWord(TransportState s) {
    switch (s) {
        case TransportState::NotLoaded: return "nothing loaded";
        case TransportState::Pause:     return "paused";
        case TransportState::Play:      return "playing";
        case TransportState::Scrub:     return "scrubbing";
        case TransportState::Seek:      return "seeking";
        case TransportState::Step:      return "stepping";
    }
    return "in an unnamed state";
}

// The reason behind a pause at either end of the file, as a suffix to the word above. Empty
// everywhere else, so a caller concatenates it unconditionally.
inline const char* TransportEndWord(EndState e) {
    switch (e) {
        case EndState::AtEnd:   return " at the end of the recording";
        case EndState::AtStart: return " at the start of the recording";
        case EndState::None:    break;
    }
    return "";
}

}  // namespace control
}  // namespace replay
