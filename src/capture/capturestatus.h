#pragma once

// The recorder as a SURFACE is allowed to see it: five facts, read-only, read on the game thread.
//
// ---------------------------------------------------------------------------------------------
// * WHY THIS IS NOT A ReplayServices SLOT. B.12's struct is frozen (bootstrap/services.h), so an
// addition to it is the architect's change and not a packet's. The tree already has the pattern
// for an interface that lives outside it -- camera/camera_control.h's Create/Get singleton, whose
// own header says a ratified ReplayServices field would point at that same object -- and this
// mirrors it exactly: one module publishes one provider from its own translation unit through
// REPLAY_MODULE, and every consumer reads it through Provider().
//
// * READ-ONLY IS THE WHOLE POINT. The owner records by spectating live public matches with this
// same DLL, and a UI that can reach into the recorder is a UI that can end his capture by
// accident. Nothing here can start, stop or configure anything. The two things a surface may ask
// for -- `record start` and `record stop` -- go through the command layer, where the session gate,
// the "already recording" refusal and the status line the owner pastes already live.
//
// * NULL UNTIL SOMETHING REGISTERS, and a surface with no provider draws NOTHING. "We could not
// ask" and "it is not recording" are different answers, and only one of them is allowed to put a
// REC indicator on a live capture.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

namespace replay {
namespace capture {
namespace status {

// One reading. `file` and `lastStopReason` point into the provider's own storage and are valid
// until the next Read() on this thread; everything else is a value.
struct Reading {
    bool recording = false;
    // ★ THE RECORDER'S OWN SESSION GATE, not a second reading of it. A surface that offers Record
    // where the recorder would refuse to open a file is offering something that cannot happen, and
    // a surface that derives the answer itself is a second copy of a rule that has already moved
    // once. False in a contained offline playback session; `sessionKind` is the word it refuses by.
    bool recordable = false;
    const char* sessionKind = "unknown";   // never null
    // Since the recorder said it was recording, on a wall clock. 0 when it is not.
    uint64_t elapsedUs = 0;
    // ★ TWO FIELDS, BECAUSE ZERO IS A BYTE COUNT. `bytesKnown` false means the recorder does not
    // publish one; rendering that as "0 bytes" would report a capture that is writing nothing.
    uint64_t bytesWritten = 0;
    bool bytesKnown = false;
    // The capture this reading is about: the open file while recording, the last closed one
    // afterwards. Never null; empty means no name has been published, which again is not "no file".
    const wchar_t* file = L"";
    // StopReasonName of the last close, or "none" before the first one. Never null.
    const char* lastStopReason = "none";
};

class ICaptureStatus {
public:
    virtual void Read(Reading& out) const = 0;

protected:
    ~ICaptureStatus() = default;
};

// The capture module registers the shipped provider from its own init; a test registers its own.
// Null clears. The pointer is borrowed and must outlive every reader, which is what makes the
// provider a static object rather than something a session allocates.
//
// The slot is inline and constant-initialised rather than a definition in the capture library, so
// a surface's own suite can register a stand-in without linking the recorder -- the same reason
// bootstrap's registrar is a zero-initialised array: no initialisation-order edge between the
// module that publishes and the module that reads.
namespace detail {
inline const ICaptureStatus* g_provider = nullptr;
}  // namespace detail

inline void RegisterProvider(const ICaptureStatus* provider) { detail::g_provider = provider; }
inline const ICaptureStatus* Provider() { return detail::g_provider; }

// The recorder publishes a FLAG, not a start time, so the flag's rising edge is where an elapsed
// number comes from. It is here, pure and inline, because the provider that feeds it needs the
// game to run and this rule does not: a test drives it with its own clock.
//
// A false->true edge starts the clock, a true->false edge stops it, and `Note` must be called with
// a non-decreasing `nowUs`. A second true with no edge between does NOT restart it -- that is the
// difference between an elapsed time and a frame counter.
class ElapsedLatch {
public:
    void Note(bool recording, uint64_t nowUs) {
        if (recording && !recording_) startedUs_ = nowUs;
        recording_ = recording;
        nowUs_ = nowUs;
    }
    bool Recording() const { return recording_; }
    uint64_t ElapsedUs() const {
        if (!recording_ || nowUs_ < startedUs_) return 0;
        return nowUs_ - startedUs_;
    }

private:
    uint64_t startedUs_ = 0;
    uint64_t nowUs_ = 0;
    bool recording_ = false;
};

}  // namespace status
}  // namespace capture
}  // namespace replay
