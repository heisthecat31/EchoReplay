#pragma once

// A stand-in for the shipped ICaptureStatus, so the recorder surface can be driven offline.
//
// ★ IT IS ALSO HALF THE PROOF THAT THE UI NEVER CALLS THE RECORDER. ui_paneltest links no capture
// source at all -- see tests/ui/CMakeLists.txt -- so a UI that reached for CaptureIsRecording(),
// RequestManualRecordStart() or any other capture symbol would not LINK. Everything the surface
// knows arrives through this interface, and everything it asks for leaves through the command sink.

#include <stdint.h>

#include "capture/capturestatus.h"

namespace uifake {

class FakeCaptureStatus final : public replay::capture::status::ICaptureStatus {
public:
    replay::capture::status::Reading reading;
    mutable uint32_t reads = 0;

    void Read(replay::capture::status::Reading& out) const override {
        ++reads;
        out = reading;
    }

    void SetRecording(bool on, uint64_t elapsedUs) {
        reading.recording = on;
        reading.elapsedUs = elapsedUs;
    }
};

// Registers `status` for the life of the object and clears the slot afterwards, so one case's
// provider cannot answer another case's frame.
class ScopedStatus {
public:
    explicit ScopedStatus(const replay::capture::status::ICaptureStatus* status) {
        replay::capture::status::RegisterProvider(status);
    }
    ~ScopedStatus() { replay::capture::status::RegisterProvider(nullptr); }
    ScopedStatus(const ScopedStatus&) = delete;
    ScopedStatus& operator=(const ScopedStatus&) = delete;
};

}  // namespace uifake
