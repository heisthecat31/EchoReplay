// The shipped ICaptureStatus: capture's own published state, turned into the reading a surface
// draws. One translation unit that declares itself, as B.10 requires; nothing in capture.cpp
// changes to make it run.
//
// ★ IT PULLS, IT IS NOT PUSHED. Every source below is something the capture module already
// publishes for a reader on another thread -- CaptureIsRecording() (the phase, published by
// SetPhase) and LastStopFacts() (the close document, published by every close). Adding a push
// would mean editing the recorder to feed a display, and the display is the thing most likely to
// change.
//
// ★ WHAT THIS PROVIDER CANNOT ANSWER YET, stated here so nobody reads a zero as a measurement:
// the recorder holds the OPEN capture's path and the writer's byte count in capture.cpp's own
// state (g.openPath is worker-owned, g.writer's stats are the worker's), and publishes neither
// while the file is open. So `bytesKnown` is false and `file` is empty until a close publishes
// them. Two lines in capture.cpp's own publish would fix both; that file belongs to the recorder's
// owner and this packet does not touch it.

#include "capture/capturestatus.h"

#include <windows.h>

#include <string.h>

#include "bootstrap/modreg.h"
#include "bootstrap/module_order.h"
#include "bootstrap/paths.h"
#include "bootstrap/services.h"
#include "capture/capturestop.h"
#include "engine/tick.h"

namespace replay {
namespace capture {
namespace status {
namespace {

const char* const kReplayModuleName = "capture-status";

// A wall clock, because a recording's length is wall time. TickInfo::simTimeUs is scaled sim time
// and a paused or slowed session would report a capture shorter than the file it is writing.
uint64_t NowUs() {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) ::QueryPerformanceFrequency(&freq);
    if (freq.QuadPart == 0) return 0;
    LARGE_INTEGER now;
    ::QueryPerformanceCounter(&now);
    return static_cast<uint64_t>((now.QuadPart * 1000000ll) / freq.QuadPart);
}

class ShippedStatus final : public ICaptureStatus {
public:
    void Read(Reading& out) const override {
        out.recording = latch.Recording();
        out.recordable = recordable;
        out.sessionKind = sessionKind;
        out.elapsedUs = latch.ElapsedUs();
        out.bytesWritten = 0;
        out.bytesKnown = false;
        out.file = file;
        out.lastStopReason = stopReason;
    }

    ElapsedLatch latch;
    wchar_t file[kPathMax] = {};
    const char* stopReason = "none";
    bool recordable = false;
    const char* sessionKind = "unknown";
};

ShippedStatus g_status;

// The close document, which is published whenever a capture ends and is the only place the file's
// name and the reason it stopped are readable from outside the recorder.
void TakeLastStop() {
    StopFactsSnapshot facts;
    if (!LastStopFacts(facts)) return;
    g_status.stopReason = StopReasonName(static_cast<StopReason>(facts.reason));
    // A close that closed nothing names the PREVIOUS capture (capturestop.h's fileClosed), so its
    // file is not this reading's file.
    if (facts.fileClosed) wcscpy_s(g_status.file, kPathMax, facts.file);
}

// The recorder's own answer to "can this session record at all", which is what decides whether a
// surface offers a Record button. Sampled rather than asked on every Read, so a reading is a copy.
void SampleSessionGate() {
    const char* kind = "unknown";
    g_status.recordable = CaptureSessionRecordable(&kind);
    g_status.sessionKind = kind != nullptr ? kind : "unknown";
}

bool Init(ReplayServices* services) {
    REPLAY_REQUIRE(services, log);
    // A close from earlier in this launch is still the last thing that happened.
    TakeLastStop();
    g_status.latch.Note(CaptureIsRecording(), NowUs());
    SampleSessionGate();
    RegisterProvider(&g_status);
    return true;
}

void Tick(const TickInfo&) {
    const bool recording = CaptureIsRecording();
    const bool was = g_status.latch.Recording();
    g_status.latch.Note(recording, NowUs());
    SampleSessionGate();
    if (recording && !was) {
        // The open capture's name is not published, and the last close's name is not this
        // capture's: an empty string says so, where a stale one would name the wrong file.
        g_status.file[0] = 0;
        return;
    }
    if (!recording && was) TakeLastStop();
}

void Shutdown() {
    RegisterProvider(nullptr);
    g_status.latch.Note(false, NowUs());
    g_status.file[0] = 0;
}

// One slot after the recorder, so a frame's reading is taken from a phase this frame published.
REPLAY_MODULE("capture-status", ::replay::kModuleOrderCapture + 1, Init, Tick, Shutdown);

}  // namespace
}  // namespace status
}  // namespace capture
}  // namespace replay
