#include "ui/recorder.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "bootstrap/log.h"
#include "capture/capturestatus.h"
#include "ui/uiapi.h"

namespace replay {
namespace ui {
namespace {

// uiapi.h's sink, at this scope because the three functions at the foot of this file and the
// recorder above it are both inside it.
CommandSinkFn g_sink = nullptr;

}  // namespace

namespace recorder {
namespace {

const char* const kModuleName = "ui";

constexpr wchar_t kStartCommand[] = L"record start";
constexpr wchar_t kStopCommand[] = L"record stop";

struct Recorder {
    bool initialised = false;
    bool indicatorKey = true;   // capture.rec_indicator, latched at Init
    bool haveProvider = false;
    bool recording = false;
    bool recordable = false;
    bool pressPending = false;
    // What the last Sample read, formatted on the tick. Draw reads these and nothing else.
    char indicator[24] = {0};
    char status[256] = {0};
    char leaf[96] = {0};        // the capture's own name, narrowed once when it changes
    // What the text was last built FROM. A recording's readout changes once a second; rebuilding a
    // 256-byte line ninety times a second to print the same string is work the frame does not owe.
    uint64_t textSecond = 0;
    uint64_t textBytes = 0;
    bool textRecording = false;
    bool textRecordable = false;
    bool textBytesKnown = false;
    bool textBuilt = false;
    const char* textReason = nullptr;
    const char* textKind = nullptr;
    uint32_t commandsIssued = 0;
    wchar_t lastCommand[32] = {0};
};

Recorder r;

const wchar_t* LeafOf(const wchar_t* path) {
    if (path == nullptr) return L"";
    const wchar_t* leaf = path;
    for (const wchar_t* p = path; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/') leaf = p + 1;
    }
    return leaf;
}

// ★ NARROWED HERE AND NOT BY %ls. The CRT's wide conversions run through the locale, which is more
// machinery than a file name is worth on the tick path, and a capture's name is ASCII by
// construction -- the recorder builds it from a date, a level id and a hex session id. Anything
// else is a character this font cannot draw either, and it is shown as '?' rather than dropped.
void NarrowLeaf(const wchar_t* wide, char* out, size_t cap) {
    if (cap == 0) return;
    size_t n = 0;
    for (; wide[n] != L'\0' && n + 1 < cap; ++n) {
        const wchar_t c = wide[n];
        out[n] = c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?';
    }
    out[n] = '\0';
}

void BuildStatus(const capture::status::Reading& reading) {
    if (!reading.recording) {
        // The session word comes from the recorder's own gate, so a surface that says nothing will
        // be written is saying what the recorder would say (capturestatus.h).
        const char* const why = reading.recordable ? "" : " -- this session records nothing";
        if (strcmp(reading.lastStopReason, "none") == 0) {
            _snprintf_s(r.status, sizeof r.status, _TRUNCATE, "not recording (%s)%s",
                        reading.sessionKind, why);
            return;
        }
        _snprintf_s(r.status, sizeof r.status, _TRUNCATE, "not recording (%s)%s; last stop: %s",
                    reading.sessionKind, why, reading.lastStopReason);
        return;
    }
    char bytes[32] = {0};
    // Not "0 bytes": the recorder publishes no byte count while a file is open, and a zero there
    // would report a capture that is writing nothing (capturestatus.h).
    if (reading.bytesKnown) {
        _snprintf_s(bytes, sizeof bytes, _TRUNCATE, ", %.1f MB",
                    static_cast<double>(reading.bytesWritten) / 1048576.0);
    }
    _snprintf_s(r.status, sizeof r.status, _TRUNCATE, "%s into %s%s", r.indicator,
                r.leaf[0] != '\0' ? r.leaf : "a capture whose name is not published yet", bytes);
}

}  // namespace

void Init(ReplayServices* services) {
    r = Recorder();
    r.initialised = true;
    if (services != nullptr && services->config != nullptr) {
        r.indicatorKey = services->config->GetBool("capture.rec_indicator", true);
    }
    Sample();
    log::Direct(LogLevel::Info, kModuleName, "recorder surface ready: REC indicator %s, %s",
                r.indicatorKey ? "on" : "off (capture.rec_indicator)",
                r.haveProvider ? "recorder status bound"
                               : "no recorder status registered yet -- nothing is drawn until one "
                                 "is");
}

void Shutdown() { r = Recorder(); }

void Sample() {
    const capture::status::ICaptureStatus* const provider = capture::status::Provider();
    r.haveProvider = provider != nullptr;
    const bool was = r.recording;
    capture::status::Reading reading;
    if (provider != nullptr) provider->Read(reading);
    r.recording = reading.recording;
    r.recordable = reading.recordable;

    // The readout is rebuilt when what it says changes, and not once per frame. `textBuilt` is
    // what makes the FIRST sample build it: every other term can legitimately match its default.
    const uint64_t second = reading.elapsedUs / 1000000ull;
    // The two `const char*` terms are compared as pointers: both are interned names from a fixed
    // set, so a different word is a different pointer and the same word never rebuilds.
    const bool changed = !r.textBuilt || r.textRecording != r.recording || r.textSecond != second ||
                         r.textReason != reading.lastStopReason || r.textKind != reading.sessionKind ||
                         r.textRecordable != reading.recordable ||
                         r.textBytesKnown != reading.bytesKnown || r.textBytes != reading.bytesWritten;
    const wchar_t* const leaf = LeafOf(reading.file);
    // A file name is a few dozen characters and it changes once per capture; comparing it is what
    // keeps the narrowing off the steady frame.
    bool leafChanged = false;
    for (size_t i = 0;; ++i) {
        const char mine = r.leaf[i];
        const wchar_t theirs = i < sizeof r.leaf ? leaf[i] : L'\0';
        if ((theirs >= 0x20 && theirs < 0x7F ? static_cast<char>(theirs)
                                             : (theirs == L'\0' ? '\0' : '?')) != mine) {
            leafChanged = true;
            break;
        }
        if (mine == '\0') break;
    }
    if (leafChanged) NarrowLeaf(leaf, r.leaf, sizeof r.leaf);

    if (changed || leafChanged) {
        if (r.recording) {
            char elapsed[16];
            FormatElapsed(reading.elapsedUs, elapsed, sizeof elapsed);
            _snprintf_s(r.indicator, sizeof r.indicator, _TRUNCATE, "REC %s", elapsed);
        } else {
            r.indicator[0] = 0;
        }
        BuildStatus(reading);
        r.textBuilt = true;
        r.textRecording = r.recording;
        r.textRecordable = reading.recordable;
        r.textKind = reading.sessionKind;
        r.textSecond = second;
        r.textReason = reading.lastStopReason;
        r.textBytes = reading.bytesWritten;
        r.textBytesKnown = reading.bytesKnown;
    }

    if (r.recording != was) {
        log::Direct(LogLevel::Info, kModuleName, "recorder: %s",
                    r.recording ? (r.indicatorKey ? "recording -- REC indicator on"
                                                  : "recording -- REC indicator off by "
                                                    "capture.rec_indicator")
                                : "not recording -- REC indicator off");
    }

    if (!r.pressPending) return;
    r.pressPending = false;
    // What the button SAID when it was pressed is what the state says now: the label and this
    // choice read the same flag, so a press cannot ask for the verb the owner did not see.
    const wchar_t* const line = r.recording ? kStopCommand : kStartCommand;
    const bool ran = RunCommand(line);
    ++r.commandsIssued;
    wcscpy_s(r.lastCommand, line);
    log::Direct(LogLevel::Info, kModuleName, "recorder: `%ls` from the F9 surface%s", line,
                ran ? "" : " went nowhere: no command surface has registered");
}

bool IndicatorOn() { return r.indicatorKey && r.haveProvider && r.recording; }

const char* IndicatorText() { return IndicatorOn() ? r.indicator : ""; }

// A live capture session is what this process is FOR, so the row is offered there before anything
// is recording. Offline it is absent entirely -- and the verdict is the RECORDER's, taken from the
// reading: capture's session gate refuses to open a file in a contained playback session, and a
// Record button that cannot record is worse than no button.
bool SurfaceWanted() { return r.haveProvider && (r.recording || r.recordable); }

bool Recording() { return r.recording; }

const char* SurfaceStatus() { return r.status; }

const char* CommandLabel() { return r.recording ? "Stop" : "Record"; }

bool CommandAvailable() { return CommandSinkPresent(); }

void PressCommand() { r.pressPending = true; }

void FormatElapsed(uint64_t elapsedUs, char* out, size_t cap) {
    if (out == nullptr || cap == 0) return;
    const uint64_t totalS = elapsedUs / 1000000ull;
    const unsigned s = static_cast<unsigned>(totalS % 60ull);
    const uint64_t totalM = totalS / 60ull;
    const unsigned m = static_cast<unsigned>(totalM % 60ull);
    const unsigned long long h = totalM / 60ull;
    if (h == 0) {
        _snprintf_s(out, cap, _TRUNCATE, "%02u:%02u", m, s);
        return;
    }
    _snprintf_s(out, cap, _TRUNCATE, "%llu:%02u:%02u", h, m, s);
}

uint32_t CommandsIssuedForTest() { return r.commandsIssued; }
const wchar_t* LastCommandForTest() { return r.lastCommand; }
void ResetForTest() { r = Recorder(); }

}  // namespace recorder

// ---- uiapi.h's command sink --------------------------------------------------------------------
//
// It lives here because the recorder surface is its only caller. W2-G registers one from its own
// module init, exactly as it pushes bookmarks in: src/ui includes nothing from src/control, and
// that is what keeps the dependency a line rather than a cycle.

void SetCommandSink(CommandSinkFn fn) { g_sink = fn; }

bool CommandSinkPresent() { return g_sink != nullptr; }

bool RunCommand(const wchar_t* line) {
    if (g_sink == nullptr || line == nullptr) return false;
    return g_sink(line);
}

}  // namespace ui
}  // namespace replay
