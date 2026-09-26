// BACKLOG-1. The REC indicator and the recorder surface's view model.
//
// The drawing half -- the corner window, the recorder row and what F9 puts on screen in a live
// session -- is in test_bar.cpp, against the same fake facade the rest of the bar is tested with.
// What is here is everything that does not need a frame: the elapsed format, the four states the
// indicator can be in, where the Record button's command goes, and what the tick costs.

#include "doctest.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "bootstrap/services.h"
#include "capture/capturestatus.h"
#include "fake_capture_status.h"
#include "foundation/ring/alloc_trap.h"
#include "ui/recorder.h"
#include "ui/uiapi.h"

using namespace replay;
using namespace replay::ui;

namespace {

class RecConfig final : public IConfig {
public:
    bool indicator = true;
    bool GetBool(const char* key, bool dflt) const override {
        if (key != nullptr && strcmp(key, "capture.rec_indicator") == 0) return indicator;
        return dflt;
    }
    int64_t GetInt(const char*, int64_t dflt) const override { return dflt; }
    double GetDouble(const char*, double dflt) const override { return dflt; }
    const char* GetString(const char*, const char* dflt) const override { return dflt; }
    bool Has(const char*) const override { return false; }
};

// The command layer's sink, stood in for so that WHAT the surface asks for can be read back. The
// real one parses the line and submits it to the queue the console types into.
std::wstring g_lastLine;
uint32_t g_lines = 0;
bool g_accept = true;

bool FakeSink(const wchar_t* line) {
    g_lastLine = line != nullptr ? line : L"";
    ++g_lines;
    return g_accept;
}

struct RecFix {
    RecConfig config;
    ReplayServices services{};
    uifake::FakeCaptureStatus status;

    RecFix() {
        services.config = &config;
        g_lastLine.clear();
        g_lines = 0;
        g_accept = true;
        SetCommandSink(&FakeSink);
        recorder::ResetForTest();
    }
    ~RecFix() {
        SetCommandSink(nullptr);
        recorder::Shutdown();
        capture::status::RegisterProvider(nullptr);
    }
    void Init() { recorder::Init(&services); }
};

std::string Elapsed(uint64_t us) {
    char out[16] = {0};
    recorder::FormatElapsed(us, out, sizeof out);
    return out;
}

constexpr uint64_t kSecond = 1000000ull;

}  // namespace

TEST_CASE("recorder: mm:ss below the hour, h:mm:ss at and above it") {
    CHECK(Elapsed(0) == "00:00");
    CHECK(Elapsed(999999) == "00:00");           // a second is a second, not a rounding
    CHECK(Elapsed(59 * kSecond) == "00:59");
    CHECK(Elapsed(60 * kSecond) == "01:00");
    CHECK(Elapsed(83 * kSecond) == "01:23");
    CHECK(Elapsed(3599 * kSecond) == "59:59");
    // ★ THE HOUR ROLLS THE FIELD OVER. "60:00" would make the minute field mean two different
    // things either side of an hour, on the one readout the owner uses to decide whether his
    // capture is still running.
    CHECK(Elapsed(3600 * kSecond) == "1:00:00");
    CHECK(Elapsed(3661 * kSecond) == "1:01:01");
    CHECK(Elapsed(10 * 3600 * kSecond) == "10:00:00");
}

TEST_CASE("recorder: nothing is drawn while nothing is recording") {
    RecFix f;
    uifake::ScopedStatus scope(&f.status);
    f.status.reading.recordable = true;
    f.status.reading.sessionKind = "live-online";
    f.Init();

    CHECK_FALSE(recorder::IndicatorOn());
    CHECK(std::string(recorder::IndicatorText()).empty());
    CHECK_FALSE(recorder::Recording());
    // The surface is still offered: this session records, it just is not recording yet.
    CHECK(recorder::SurfaceWanted());
    CHECK(std::string(recorder::SurfaceStatus()).find("not recording") != std::string::npos);
    CHECK(std::string(recorder::CommandLabel()) == "Record");
}

TEST_CASE("recorder: the indicator shows the recorder's own elapsed time") {
    RecFix f;
    uifake::ScopedStatus scope(&f.status);
    f.status.reading.recordable = true;
    f.status.reading.sessionKind = "live-online";
    f.status.reading.file = L"C:\\Users\\x\\Documents\\EchoReplay\\2026_arena.echodemo";
    f.Init();

    f.status.SetRecording(true, 83 * kSecond);
    recorder::Sample();
    CHECK(recorder::IndicatorOn());
    CHECK(std::string(recorder::IndicatorText()) == "REC 01:23");
    CHECK(std::string(recorder::CommandLabel()) == "Stop");
    // The status line carries the capture's own name, not the whole path.
    CHECK(std::string(recorder::SurfaceStatus()).find("2026_arena.echodemo") != std::string::npos);
    CHECK(std::string(recorder::SurfaceStatus()).find("C:\\") == std::string::npos);

    f.status.SetRecording(false, 0);
    f.status.reading.lastStopReason = "match-end";
    recorder::Sample();
    CHECK_FALSE(recorder::IndicatorOn());
    CHECK(std::string(recorder::IndicatorText()).empty());
    CHECK(std::string(recorder::SurfaceStatus()).find("match-end") != std::string::npos);
}

TEST_CASE("recorder: with no provider registered the surface says nothing at all") {
    RecFix f;
    f.Init();   // no ScopedStatus: the slot is null
    CHECK_FALSE(recorder::IndicatorOn());
    CHECK_FALSE(recorder::SurfaceWanted());
    CHECK_FALSE(recorder::Recording());
}

TEST_CASE("recorder: capture.rec_indicator off hides the indicator and nothing else") {
    RecFix f;
    f.config.indicator = false;
    uifake::ScopedStatus scope(&f.status);
    f.status.reading.recordable = true;
    f.Init();
    f.status.SetRecording(true, 5 * kSecond);
    recorder::Sample();

    CHECK_FALSE(recorder::IndicatorOn());
    CHECK(std::string(recorder::IndicatorText()).empty());
    // The surface is a different question: a key that hides the overlay must not hide the control.
    CHECK(recorder::Recording());
    CHECK(recorder::SurfaceWanted());
}

TEST_CASE("recorder: an offline session carries no recorder surface") {
    RecFix f;
    uifake::ScopedStatus scope(&f.status);
    f.status.reading.recordable = false;     // capture's own gate in a contained session
    f.status.reading.sessionKind = "contained";
    f.Init();

    CHECK_FALSE(recorder::SurfaceWanted());
    CHECK_FALSE(recorder::IndicatorOn());
    CHECK(std::string(recorder::SurfaceStatus()).find("contained") != std::string::npos);
    CHECK(std::string(recorder::SurfaceStatus()).find("records nothing") != std::string::npos);
}

TEST_CASE("recorder: the button issues record start and record stop, and nothing else") {
    RecFix f;
    uifake::ScopedStatus scope(&f.status);
    f.status.reading.recordable = true;
    f.Init();

    CHECK(recorder::CommandAvailable());
    // A press is recorded by the draw and issued by the NEXT tick, like every other bar control.
    recorder::PressCommand();
    CHECK(g_lines == 0);
    recorder::Sample();
    CHECK(g_lines == 1);
    CHECK(g_lastLine == L"record start");
    CHECK(recorder::CommandsIssuedForTest() == 1);

    // The recorder answers by ITS OWN state, not by the button having been pressed: the surface
    // reads back what happened rather than assuming it.
    f.status.SetRecording(true, kSecond);
    recorder::Sample();
    recorder::PressCommand();
    recorder::Sample();
    CHECK(g_lines == 2);
    CHECK(g_lastLine == L"record stop");
    CHECK(std::wstring(recorder::LastCommandForTest()) == L"record stop");

    // One press is one command: a held button does not queue a second.
    recorder::Sample();
    CHECK(g_lines == 2);
}

TEST_CASE("recorder: with no command sink the control is unavailable and presses go nowhere") {
    RecFix f;
    SetCommandSink(nullptr);
    uifake::ScopedStatus scope(&f.status);
    f.status.reading.recordable = true;
    f.Init();

    CHECK_FALSE(recorder::CommandAvailable());
    recorder::PressCommand();
    recorder::Sample();
    CHECK(g_lines == 0);
    CHECK_FALSE(RunCommand(L"record start"));
}

TEST_CASE("recorder: the steady tick allocates nothing") {
    RecFix f;
    uifake::ScopedStatus scope(&f.status);
    f.status.reading.recordable = true;
    f.status.reading.sessionKind = "live-online";
    f.status.reading.file = L"C:\\Users\\x\\Documents\\EchoReplay\\2026_arena.echodemo";
    f.Init();
    f.status.SetRecording(true, 83 * kSecond);
    recorder::Sample();

    const uint32_t live = TrapInstall(kTrapNew | kTrapHeap);
    REQUIRE((live & kTrapNew) != 0);

    // ★ PROVE THE TRAP FIRES BEFORE TRUSTING A ZERO FROM IT. A trap that cannot see an allocation
    // reports a clean frame for every possible implementation of this file.
    TrapTake();
    {
        TrapGuard armed;
        volatile char* p = new char[64];
        delete[] p;
    }
    const TrapReport control = TrapTake();
    CHECK(control.allocations > 0);

    // The steady frame: the elapsed second does not change, so nothing is even reformatted.
    {
        TrapGuard armed;
        for (int i = 0; i < 8; ++i) {
            f.status.reading.elapsedUs += 11000;   // 90 Hz, well inside one second
            recorder::Sample();
        }
    }
    const TrapReport steady = TrapTake();
    CHECK(steady.allocations == 0);

    // And the frame the readout DOES change on: a second rolls, the name changes, the byte count
    // arrives. Formatting is the only work here and it happens in fixed buffers.
    {
        TrapGuard armed;
        f.status.reading.elapsedUs = 84 * kSecond;
        recorder::Sample();
        f.status.reading.file = L"C:\\Users\\x\\Documents\\EchoReplay\\2026_other.echodemo";
        f.status.reading.bytesKnown = true;
        f.status.reading.bytesWritten = 7340032;
        recorder::Sample();
    }
    const TrapReport changing = TrapTake();
    CHECK(changing.allocations == 0);
    TrapUninstall();

    CHECK(std::string(recorder::IndicatorText()) == "REC 01:24");
    CHECK(std::string(recorder::SurfaceStatus()).find("2026_other.echodemo") != std::string::npos);
    CHECK(std::string(recorder::SurfaceStatus()).find("7.0 MB") != std::string::npos);
}

// ★ THE ONE RULE THIS PACKET EXISTS TO KEEP. The owner records live public matches with this DLL,
// so the UI reads the recorder and never drives it. Half of that is proven by the link -- this
// binary contains no capture source, so a call to CaptureIsRecording() or
// RequestManualRecordStart() would not resolve -- and the other half is proven here, because a
// future edit could add the capture library to this target and quietly make such a call link.
TEST_CASE("ui: the only capture names in the UI are the status seam and the marker kinds") {
    const char* const files[] = {"src\\ui\\recorder.cpp", "src\\ui\\bar.cpp",
                                 "src\\ui\\ui_module.cpp", "src\\ui\\panel.cpp",
                                 "src\\ui\\timeline.cpp"};
    for (const char* leaf : files) {
        const std::string path = std::string(REPLAY_SOURCE_DIR) + "\\" + leaf;
        FILE* fp = nullptr;
        REQUIRE(fopen_s(&fp, path.c_str(), "rb") == 0);
        REQUIRE(fp != nullptr);
        std::string text;
        char buf[4096];
        size_t n = 0;
        while ((n = fread(buf, 1, sizeof buf, fp)) > 0) text.append(buf, n);
        fclose(fp);

        // The scan read a real file: two zeros over an empty string would pass this case on a
        // build where the sources had been renamed out from under it.
        REQUIRE(text.size() > 500);

        size_t at = 0;
        size_t found = 0;
        while ((at = text.find("capture::", at)) != std::string::npos) {
            const std::string tail = text.substr(at, 32);
            const bool allowed = tail.compare(0, 17, "capture::status::") == 0 ||
                                 tail.compare(0, 14, "capture::kMark") == 0;
            INFO(leaf << ": " << tail);
            CHECK(allowed);
            ++found;
            at += 9;
        }
        // And the matcher has been seen say YES: recorder.cpp reaches the status seam four times,
        // so a scanner that found nothing anywhere would be caught here rather than reported as a
        // clean sweep.
        if (strcmp(leaf, "src\\ui\\recorder.cpp") == 0) CHECK(found > 0);
    }
}

TEST_CASE("recorder: a byte count that is not published is not drawn as zero") {
    RecFix f;
    uifake::ScopedStatus scope(&f.status);
    f.status.reading.recordable = true;
    f.Init();
    f.status.SetRecording(true, kSecond);
    f.status.reading.bytesKnown = false;
    f.status.reading.bytesWritten = 0;
    recorder::Sample();
    CHECK(std::string(recorder::SurfaceStatus()).find("MB") == std::string::npos);
    CHECK(std::string(recorder::SurfaceStatus()).find("0 bytes") == std::string::npos);
}
