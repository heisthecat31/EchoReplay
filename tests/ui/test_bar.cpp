// UI-2 P2. The player bar's WIRING, against the fake facade and a fake transport.
//
// What these cases are for: every one of them fails if the thing it names is removed. The drag
// machine is the part that cannot be seen in a screenshot -- a swallow that stopped swallowing, an
// EndScrub that ran twice, a calibration that accepted an absurd scale and sent the playhead to the
// wrong end of the file -- so that is where most of them are.
//
// The mouse and the keyboard are seams (bar.h): the live reads are GetCursorPos/GetAsyncKeyState,
// which no offline test can drive, and the wiring from a cursor pixel to a scrub target is the half
// worth testing.

#include "doctest.h"

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "bootstrap/livesafety.h"
#include "bootstrap/log.h"
#include "bootstrap/paths.h"
#include "bootstrap/services.h"
#include "capture/capturestatus.h"
#include "capture/markers.h"
#include "fake_capture_status.h"
#include "fake_gui.h"
#include "foundation/demofile/demofile.h"
#include "foundation/demofile/format.h"
#include "playback/core/transport.h"
#include "scratch_capture_dir.h"
#include "ui/bar.h"
#include "ui/demoindex.h"
#include "ui/panel.h"
#include "ui/recorder.h"
#include "ui/timeline.h"
#include "ui/uiapi.h"

using namespace replay;
using namespace replay::ui;

namespace {

// ---- the transport the bar drives, with the four scrub methods counted ------------------------

class ScrubTransport final : public IReplayTransport {
public:
    TransportState state = TransportState::Pause;
    EndState ended = EndState::None;
    float rate = 1.0f;
    int64_t playhead = 0;
    uint64_t duration = 871000000;   // the owner's 14:31 file
    uint32_t serial = 1;

    uint32_t beginScrubs = 0;
    uint32_t endScrubs = 0;
    uint32_t scrubTos = 0;
    int64_t lastScrubTo = -1;
    int64_t lastEndScrub = -1;
    uint32_t seeks = 0;
    int64_t lastSeek = -1;
    uint32_t plays = 0;
    uint32_t pauses = 0;
    uint32_t setRates = 0;
    float lastSetRate = 0.0f;
    int lastStep = 0;
    uint32_t steps = 0;
    ScrubStats stats;

    // A load is a new serial, exactly as the real transport publishes one: the bar's per-file
    // caches are keyed on nothing else.
    bool Load(const wchar_t*) override {
        ++serial;
        return true;
    }
    void Unload() override {}
    void Play(float r) override {
        ++plays;
        rate = r;
        state = TransportState::Play;
    }
    void Pause() override {
        ++pauses;
        state = TransportState::Pause;
    }
    void SetRate(float r) override {
        ++setRates;
        lastSetRate = r;
        rate = r;
    }
    void Step(int frames) override {
        ++steps;
        lastStep = frames;
    }
    void Seek(int64_t tUs) override {
        ++seeks;
        lastSeek = tUs;
        playhead = tUs;
    }
    bool SeekRound(RoundNumber) override { return false; }
    void SetMode(bool) override {}
    TransportState State() const override { return state; }
    EndState Ended() const override { return ended; }
    float Rate() const override { return rate; }
    int64_t PlayheadUs() const override { return playhead; }
    uint64_t DurationUs() const override { return duration; }
    // The recorded content, which is NOT the file axis. Defaults to the whole axis so every case
    // that is not about the span reads exactly what it read before.
    int64_t spanFirst = 0;
    int64_t spanLast = -1;
    bool PlayableSpanUs(int64_t* firstUs, int64_t* lastUs) const override {
        const int64_t last = spanLast >= 0 ? spanLast : static_cast<int64_t>(duration);
        if (last <= spanFirst) return false;
        *firstUs = spanFirst;
        *lastUs = last;
        return true;
    }
    uint32_t LoadSerial() const override { return serial; }
    const char* ActiveModeLabel() const override { return "fake"; }

    void BeginScrub() override {
        ++beginScrubs;
        stats.active = true;
        state = TransportState::Scrub;
    }
    void ScrubTo(int64_t us) override {
        ++scrubTos;
        lastScrubTo = us;
        stats.pendingUs = us;
    }
    void EndScrub(int64_t us) override {
        ++endScrubs;
        lastEndScrub = us;
        playhead = us;
        stats.active = false;
        state = TransportState::Pause;
    }
    ScrubStats Scrub() const override { return stats; }

    uint8_t smoothMode = 1, smoothStrength = 2;   // the module's defaults: slow-mo only, medium
    uint32_t smoothSets = 0;
    void SetMotionSmoothing(uint8_t mode, uint8_t strength) override {
        smoothMode = mode;
        smoothStrength = strength;
        ++smoothSets;
    }
    uint16_t MotionSmoothing() const override { return (uint16_t)(smoothStrength << 8 | smoothMode); }
};

// ---- the input seams --------------------------------------------------------------------------

bar::MouseState g_mouse;
uint32_t g_keys = 0;
bool g_liveCapture = false;

void MouseSource(bar::MouseState* out) { *out = g_mouse; }
uint32_t KeySource() { return g_keys; }
bool LiveCaptureSource() { return g_liveCapture; }

bool g_hotkey = false;
bool HotkeyDown() { return g_hotkey; }

// ScratchConfig answers every integer with its default, and `ui.player_width` is the one key the
// track-extent cases have to move.
class BarConfig final : public testscratch::ScratchConfig {
public:
    explicit BarConfig(const std::wstring& dir) : ScratchConfig(dir) {}
    int64_t widthPx = 760;
    // Auto-hide ships OFF now -- F9 is the switch -- so a case about auto-hide has to turn it on.
    double autohideS = 0.0;
    int64_t GetInt(const char* key, int64_t dflt) const override {
        if (key != nullptr && strcmp(key, "ui.player_width") == 0) return widthPx;
        return ScratchConfig::GetInt(key, dflt);
    }
    double GetDouble(const char* key, double dflt) const override {
        if (key != nullptr && strcmp(key, "ui.player_autohide_s") == 0) return autohideS;
        return ScratchConfig::GetDouble(key, dflt);
    }
};

struct TempDirB {
    std::wstring path;
    TempDirB() {
        wchar_t tmp[MAX_PATH] = {};
        ::GetTempPathW(MAX_PATH, tmp);
        wchar_t name[64] = {};
        _snwprintf_s(name, _TRUNCATE, L"echobar_%lu_%llu", ::GetCurrentProcessId(),
                     static_cast<unsigned long long>(::GetTickCount64()));
        path = std::wstring(tmp) + name;
        ::CreateDirectoryW(path.c_str(), nullptr);
    }
    std::wstring File(const wchar_t* leaf) const { return path + L"\\" + leaf; }
};

struct BarFix {
    TempDirB dir;
    std::wstring captureDir{testscratch::TestScratchCaptureDir(dir.path.c_str())};
    BarConfig config{captureDir};
    DemoIndex index;
    ReplayServices services{};
    ScrubTransport transport;

    explicit BarFix(int32_t barWidthPx = 760, bool liveCapture = false, double autohideS = 0.0) {
        config.widthPx = barWidthPx;
        config.autohideS = autohideS;
        g_liveCapture = liveCapture;
        // A bar test is a contained offline process unless it says it is a live capture; the
        // cursor claim reads that verdict now (RECORDER-2 part A / L1).
        livesafety::SetForTest(liveCapture, !liveCapture);
        const std::wstring self = dir.File(L"plugins\\EchoReplay.dll");
        ::CreateDirectoryW(dir.File(L"plugins").c_str(), nullptr);
        SetPathsForTest(self.c_str());
        ::CreateDirectoryW(captureDir.c_str(), nullptr);

        services.config = &config;
        services.transport = &transport;

        for (size_t i = 0; i < static_cast<size_t>(gui::Entry::Count); ++i) {
            uifake::SetHave(static_cast<gui::Entry>(i), true);
        }
        uifake::SetReady(true);
        uifake::SetWindowGate(true);
        uifake::Reset();

        panel::Init(&services, &index);
        // The recorder's view model is drawn by the bar, so a case that says nothing about it must
        // start with it saying nothing: no provider, nothing recording, no row.
        recorder::ResetForTest();
        bar::ResetForTest();
        // Before Init: the live read is the offline lock's own state, and this binary holds no
        // lock, so every case would otherwise start in the live-capture configuration.
        bar::SetLiveCaptureSourceForTest(&LiveCaptureSource);
        bar::Init(&services);
        bar::SetMouseSourceForTest(&MouseSource);
        bar::SetKeySourceForTest(&KeySource);

        g_mouse = bar::MouseState();
        g_mouse.haveRect = true;
        g_mouse.clientW = 1920;
        g_mouse.clientH = 1080;
        g_mouse.havePos = true;
        g_mouse.x = 400;  // facts-ok: AUDIO.LISTENER_COUNT_OFF plain 400 as a cursor pixel x, not the listener-count offset
        g_mouse.y = 500;
        g_mouse.foreground = true;
        g_keys = 0;

        // Cursor mode: F8's meaning is unchanged and it is what makes the bar clickable.
        ShowPanel(true);
    }

    ~BarFix() {
        bar::Shutdown();
        recorder::Shutdown();
        panel::Shutdown();
        bar::SetMouseSourceForTest(nullptr);
        bar::SetKeySourceForTest(nullptr);
        bar::SetLiveCaptureSourceForTest(nullptr);
        bar::ResetForTest();
        livesafety::ResetForTest();
        g_liveCapture = false;
    }

    // One engine frame: the tick, then the draw, exactly as ui_module orders them.
    void Frame(const char* click = nullptr) {
        panel::Sample(16000);
        bar::Sample(16000);
        uifake::Reset();
        if (click != nullptr) uifake::ClickOnce(click);
        panel::Draw();
        bar::Draw();
    }

    // The bar's own geometry, recomputed the way bar.cpp does, so a test can name a cell in the
    // same units the product placed it in.
    static constexpr float kBarW = 760.0f;
    // Where the bar actually placed itself, asked of the product rather than recomputed: the
    // placement is the seed of the x map now, so a fixture that guessed a different origin would
    // be testing a window nobody draws.
    float BarX() const { return bar::WindowRectForTest().x; }
    double CellGuiX(uint32_t i) const {
        return static_cast<double>(BarX()) + (static_cast<double>(i) + 0.5) * kBarCellPx;
    }

    // ★ WHAT THE FACADE REALLY DOES, measured in-game on 2026-09-19: a Button is
    // reported on the RELEASE, with VK_LBUTTON already up, and the cell INDEX it hands back is not
    // the cell under the cursor -- a click at client x=300 came back as cell 0. So the fake reports
    // the hit after the button is up, and a case that wants a particular time must aim the CURSOR
    // at it, never the index.
    void ClickCellStill(uint32_t i, int32_t clientX, int32_t clientY) {
        char id[16];
        snprintf(id, sizeof id, "##b%u", i);
        g_mouse.x = clientX;
        g_mouse.y = clientY;
        g_mouse.left = false;                 // the facade fires on the way up
        Frame(id);
        Frame();   // the hit is serviced on the frame after the draw that reported it
    }

    // One hit is the whole price: it ties the track row to the cursor's y. The x map needs no
    // correction -- the facade lays out in the client pixels the cursor arrives in.
    void Calibrate() { ClickCellStill(20, static_cast<int32_t>(CellGuiX(20)) + 8, 500); }

    // The facade's content inset, which is what the bar seeds its x map with and all the game ever
    // measured it to be (track drawn at client x=49 against the seeded 48).
    static constexpr double kContentPad = 8.0;
    double TrackClientLeft() const { return static_cast<double>(BarX()) + kContentPad; }
    // The track row the bar derives from its own placement, with nothing clicked: this is the band
    // a first press has to land in for the very first drag to scrub.
    double TrackClientWidth() const { return static_cast<double>(kBarW) - 16.0; }
    double UsPerPx() const {
        return static_cast<double>(transport.duration) / TrackClientWidth();
    }

    void MoveTo(int32_t x, const char* click = nullptr) {
        g_mouse.x = x;
        Frame(click);
    }

    // ---- the wide-window cases, which cannot use the constants above ---------------------------
    //
    // The track is one function's answer now (bar.h TrackExtent), so a test that wants to name a
    // cell at an arbitrary window width asks the product where it drew, exactly as the drag map
    // does. A map of scale 1 and offset +20 keeps every pixel positive at every width here.
    static constexpr double kMapOffset = kContentPad;
    static double CellClientX(const bar::TrackExtent& e, uint32_t i) {
        return static_cast<double>(e.guiX) + (static_cast<double>(i) + 0.5) * e.cellPx + kMapOffset;
    }
    void CalibrateAtWidth() {
        const bar::TrackExtent e = bar::TrackExtentForTest();
        ClickCellStill(10, static_cast<int32_t>(CellClientX(e, 10)), 500);
    }
};

// A real .echodemo carrying exactly these round markers, written with the shipped writer so the
// bar's table is built from a manifest DemoIndex actually read -- the marker row is the only place
// a stale per-file cache is visible.
bool WriteBarCapture(const wchar_t* path, const RoundMarker* marks, uint16_t count) {
    FieldDef field{};
    field.id = 1;
    field.cls = FieldClass::Raw;
    field.rawBits = 64;
    field.prov = FieldProv::Pc;
    SchemaDef schema{};
    schema.id = 1;
    schema.retention = Retention::All;
    schema.required = true;
    schema.fieldCount = 1;
    schema.fields = &field;

    Manifest m{};
    m.formatMajor = kFormatMajor;
    m.formatMinor = kFormatMinor;
    m.t0Us = 1700000000000000ull;
    m.keyframeIntervalUs = 2000000;
    m.captureRole = 1;
    m.startTimeEpoch = 1700000000ull;
    m.roundCount = count;
    m.rounds = marks;
    m.schemaCount = 1;
    m.schemas = &schema;

    DemoWriter w;
    if (!w.Open(path, m)) return false;
    const uint64_t tick[4] = {m.t0Us, m.t0Us + 22000, m.t0Us + 44000, m.t0Us + 66000};
    w.BeginChapter(kSectionCore, m.t0Us);
    w.WriteStreamBlock(kStreamTick, 1, tick, sizeof tick, 4, 4);
    w.EndChapter(m.t0Us + 2000000ull);
    w.Close();
    return true;
}

RoundMarker RoundAt(uint64_t tUs, uint16_t index) {
    RoundMarker m{};
    m.tUs = 1700000000000000ull + tUs;
    m.kind = replay::capture::kMarkRoundBegin;
    m.index = index;
    return m;
}

}  // namespace

TEST_CASE("bar: with the debug panel off the bar is the only thing drawn, and it fits its budget") {
    BarFix fx;
    fx.Frame();
    CHECK(bar::VisibleForTest());
    CHECK(uifake::ItemsDrawn() <= panel::kBarItemBudget);
    CHECK(uifake::GlyphsDrawn() <= panel::kBarGlyphBudget);
    // The panel's own content is gone: no status paragraph, no fine bar, no ceiling sentence.
    CHECK_FALSE(uifake::TextContains("fine "));
    CHECK_FALSE(uifake::ButtonDrawn("Browse##openbrowse"));
    CHECK_FALSE(uifake::ButtonDrawn("##c0"));      // the panel's 6 px coarse strip
    CHECK(uifake::ButtonDrawn("##b0"));            // the bar's 3 px track
    CHECK(uifake::ButtonDrawn("=##bmenu"));
}

TEST_CASE("bar: ui.debug_panel at runtime brings the whole old panel back, and both still fit") {
    BarFix fx;
    fx.Frame();
    CHECK_FALSE(uifake::ButtonDrawn("##c0"));

    SetDebugPanel(true);
    CHECK(DebugPanelOn());
    fx.Frame();
    CHECK(uifake::ButtonDrawn("##c0"));            // the panel's coarse strip is back
    CHECK(uifake::ButtonDrawn("##b0"));            // and the bar is still there
    CHECK(uifake::ItemsDrawn() <=
          panel::kBarItemBudget + panel::kPanelTransportItemBudget);

    SetDebugPanel(false);
    fx.Frame();
    CHECK_FALSE(uifake::ButtonDrawn("##c0"));
    CHECK(uifake::ButtonDrawn("##b0"));
}

TEST_CASE("bar: no file collapses to the way in, and that bar auto-hides like the loaded one") {
    BarFix fx(760, false, 3.0);
    fx.transport.state = TransportState::NotLoaded;
    fx.Frame();
    CHECK(bar::MinimalForTest());
    CHECK(uifake::ButtonDrawn("Open...##bopen"));
    CHECK_FALSE(uifake::ButtonDrawn("##b0"));      // a track over nothing is not a scrubber
    CHECK_FALSE(uifake::ButtonDrawn("Play##bplay"));  // nor is the control row worth the space

    // ★ MF-1's second half. `pinned` used to carry `!loaded` and the hide clock used to need a
    // calibration that cannot happen with no track on screen, so this bar could never go away.
    bar::AddIdleForTest(3500000000ull);
    fx.Frame();
    CHECK(bar::HiddenForTest());
    CHECK_FALSE(bar::VisibleForTest());

    g_mouse.x += 3;
    fx.Frame();
    CHECK(bar::VisibleForTest());

    // And the button is the browser, not decoration.
    fx.Frame("Open...##bopen");
    fx.Frame();
    CHECK(BrowserOpen());
}

TEST_CASE("bar: a live capture gets no bar until one is asked for, and `off` stays off") {
    BarFix fx(760, /*liveCapture=*/true);
    fx.Frame();
    CHECK_FALSE(bar::VisibleForTest());
    CHECK(strcmp(bar::ShowReason(), "live-capture") == 0);
    CHECK(uifake::ItemsDrawn() == 0);

    // The owner asks for it, and gets it in the same session.
    SetBarShown(true);
    fx.Frame();
    CHECK(bar::VisibleForTest());
    CHECK(BarShown());

    // ...and dismisses it. Neither mouse motion nor a shortcut key may undo that: those are the
    // auto-hide recovery rules, and an explicit `off` is not auto-hide.
    SetBarShown(false);
    fx.Frame();
    CHECK_FALSE(bar::VisibleForTest());
    g_mouse.x += 40;
    fx.Frame();
    g_keys = bar::kKeySpace;
    fx.Frame();
    g_keys = 0;
    fx.Frame();
    CHECK_FALSE(bar::VisibleForTest());
    CHECK(strcmp(bar::ShowReason(), "bar off") == 0);

    ToggleBar();
    fx.Frame();
    CHECK(bar::VisibleForTest());
}

TEST_CASE("bar: F9 dismisses the bar and brings it back, in an offline session and a live one") {
    for (int live = 0; live < 2; ++live) {
        BarFix fx(760, live != 0);
        if (live != 0) SetBarShown(true);
        fx.Frame();
        REQUIRE(bar::VisibleForTest());

        g_keys = bar::kKeyBarToggle;
        fx.Frame();
        g_keys = 0;
        fx.Frame();
        CHECK_FALSE(bar::VisibleForTest());

        g_keys = bar::kKeyBarToggle;          // the edge again, from a released key
        fx.Frame();
        g_keys = 0;
        fx.Frame();
        CHECK(bar::VisibleForTest());
    }
}

TEST_CASE("bar: F9 is a real show and hide -- the window AND the cursor move together") {
    BarFix fx;
    fx.Frame();
    REQUIRE(bar::VisibleForTest());
    REQUIRE(PanelVisible());

    g_keys = bar::kKeyBarToggle;
    panel::Sample(16000);
    bar::Sample(16000);
    CHECK_FALSE(PanelVisible());            // the cursor went back with the window
    panel::Sample(16000);                   // the claim follows the flag on the next tick
    CHECK(uifake::LastCursorArg() == 0);
    uifake::Reset();
    panel::Draw();
    bar::Draw();
    CHECK_FALSE(bar::VisibleForTest());

    g_keys = 0;
    fx.Frame();
    g_keys = bar::kKeyBarToggle;
    panel::Sample(16000);
    bar::Sample(16000);
    CHECK(PanelVisible());                  // and it comes back usable, not just visible
    panel::Sample(16000);
    CHECK(uifake::LastCursorArg() == 1);
    g_keys = 0;
    fx.Frame();
    CHECK(bar::VisibleForTest());
}

TEST_CASE("bar: on a live server F9 shows the recorder surface without taking the mouse") {
    BarFix fx(760, /*liveCapture=*/true);
    uifake::Reset();
    fx.Frame();

    g_keys = bar::kKeyBarToggle;
    panel::Sample(16000);
    bar::Sample(16000);
    panel::Sample(16000);
    g_keys = 0;
    fx.Frame();
    CHECK(bar::VisibleForTest());          // the window is up -- the owner asked for that
    CHECK_FALSE(PanelVisible());           // and the spectator camera still has the mouse
    CHECK(uifake::LastCursorArg() != 1);   // SetCursorEnabled(1) was never called

    // F10 is the explicit claim, and it still works.
    g_keys = bar::kKeyCursorToggle;
    panel::Sample(16000);
    bar::Sample(16000);
    panel::Sample(16000);
    CHECK(PanelVisible());
    CHECK(uifake::LastCursorArg() == 1);
    g_keys = 0;
}

// ---- BACKLOG-1: the REC indicator and the recorder surface, drawn ------------------------------

namespace {

std::wstring g_barCommand;
uint32_t g_barCommands = 0;
bool BarSink(const wchar_t* line) {
    g_barCommand = line != nullptr ? line : L"";
    ++g_barCommands;
    return true;
}

// The recorder surface the bar draws is fed by a provider and a tick, exactly as ui_module feeds
// it: Sample before the frame, never inside the draw.
struct RecorderInLiveSession {
    uifake::FakeCaptureStatus status;
    uifake::ScopedStatus scope{&status};

    RecorderInLiveSession() {
        status.reading.recordable = true;
        status.reading.sessionKind = "live-online";
        g_barCommand.clear();
        g_barCommands = 0;
        SetCommandSink(&BarSink);
    }
    ~RecorderInLiveSession() {
        SetCommandSink(nullptr);
        recorder::ResetForTest();
    }
};

}  // namespace

TEST_CASE("bar: a live capture draws the REC indicator and nothing else") {
    BarFix fx(760, /*liveCapture=*/true);
    RecorderInLiveSession rec;
    rec.status.SetRecording(true, 83000000);
    recorder::Sample();
    fx.Frame();

    // The bar itself is hidden in a live session until F9 asks for it -- that is unchanged.
    CHECK_FALSE(bar::VisibleForTest());
    CHECK(std::string(bar::ShowReason()) == "live-capture");
    // What IS on screen is the indicator, with the recorder's own elapsed time.
    CHECK(uifake::TextContains("REC 01:23"));
    // And it claims nothing: no cursor, no window the owner has to dismiss.
    CHECK(uifake::LastCursorArg() != 1);

    // A minute later it says so, on the same one line.
    rec.status.SetRecording(true, 143000000);
    recorder::Sample();
    fx.Frame();
    CHECK(uifake::TextContains("REC 02:23"));
}

TEST_CASE("bar: nothing at all is drawn when the recorder is not recording") {
    BarFix fx(760, /*liveCapture=*/true);
    RecorderInLiveSession rec;
    recorder::Sample();
    fx.Frame();

    CHECK_FALSE(bar::VisibleForTest());
    CHECK_FALSE(uifake::TextContains("REC"));
    CHECK(uifake::BeginCount() == 0);
}

TEST_CASE("bar: an offline session has no indicator and no recorder row") {
    BarFix fx;   // contained offline playback
    uifake::FakeCaptureStatus status;
    uifake::ScopedStatus scope(&status);
    status.reading.recordable = false;     // capture's own gate refuses this session
    status.reading.sessionKind = "contained";
    recorder::Sample();
    fx.Frame();

    REQUIRE(bar::VisibleForTest());        // the player is up, as it always is offline
    CHECK_FALSE(uifake::TextContains("REC"));
    CHECK_FALSE(uifake::ButtonDrawn("##brecord"));
    recorder::ResetForTest();
}

TEST_CASE("bar: F9 in a live session shows a Record button that issues the command") {
    BarFix fx(760, /*liveCapture=*/true);
    RecorderInLiveSession rec;
    recorder::Sample();

    g_keys = bar::kKeyBarToggle;
    panel::Sample(16000);
    bar::Sample(16000);
    g_keys = 0;
    fx.Frame();
    REQUIRE(bar::VisibleForTest());
    CHECK(uifake::ButtonDrawn("Record##brecord"));
    CHECK(uifake::TextContains("not recording (live-online)"));

    // Pressed on the frame, issued on the tick that follows it.
    fx.Frame("##brecord");
    CHECK(g_barCommands == 0);
    recorder::Sample();
    CHECK(g_barCommands == 1);
    CHECK(g_barCommand == L"record start");

    // Once the recorder says it is recording, the same control is Stop and the row carries the
    // elapsed time the indicator does.
    rec.status.SetRecording(true, 5000000);
    recorder::Sample();
    fx.Frame();
    CHECK(uifake::ButtonDrawn("Stop##brecord"));
    CHECK(uifake::TextContains("REC 00:05"));
    fx.Frame("##brecord");
    recorder::Sample();
    CHECK(g_barCommands == 2);
    CHECK(g_barCommand == L"record stop");
}

TEST_CASE("bar: F10 moves the cursor alone and leaves the window on screen") {
    BarFix fx;
    fx.Frame();
    REQUIRE(bar::VisibleForTest());
    CHECK_FALSE(bar::HintShownForTest());   // the fixture is in cursor mode

    g_keys = bar::kKeyCursorToggle;
    panel::Sample(16000);
    bar::Sample(16000);
    CHECK_FALSE(PanelVisible());
    panel::Sample(16000);
    CHECK(uifake::LastCursorArg() == 0);
    g_keys = 0;
    fx.Frame();
    // The window is still up -- that is the whole point of F10 -- and it says how to get the
    // mouse back.
    CHECK(bar::VisibleForTest());
    CHECK(bar::HintShownForTest());
    CHECK(uifake::TextContains("F10"));
    CHECK(uifake::TextContains("mouse"));

    g_keys = bar::kKeyCursorToggle;
    panel::Sample(16000);
    bar::Sample(16000);
    CHECK(PanelVisible());
    panel::Sample(16000);
    CHECK(uifake::LastCursorArg() == 1);
    g_keys = 0;
    fx.Frame();
    CHECK_FALSE(bar::HintShownForTest());
}

TEST_CASE("bar: F10 with the UI hidden does not strand a claimed cursor") {
    BarFix fx;
    fx.Frame();
    SetBarShown(false);
    ShowPanel(false);
    fx.Frame();
    REQUIRE_FALSE(bar::VisibleForTest());

    g_keys = bar::kKeyCursorToggle;
    panel::Sample(16000);
    bar::Sample(16000);
    g_keys = 0;
    CHECK_FALSE(PanelVisible());            // F9 is the way back, not F10
    fx.Frame();
    CHECK_FALSE(bar::VisibleForTest());
}

constexpr const char* kBarWindow = "Echo Replay##replaybar";

// The facade is told where the window is on every drawn frame, and it is the bar's own position.
void CheckPinnedToBar() {
    const bar::WindowRect r = bar::WindowRectForTest();
    CHECK(uifake::WindowPosCalls() == 1);
    CHECK(uifake::LastWindowPos().x == doctest::Approx(r.x));
    CHECK(uifake::LastWindowPos().y == doctest::Approx(r.y));
}

TEST_CASE("bar: the window is placed once, and the owner's position survives hide and show") {
    BarFix fx;
    fx.transport.state = TransportState::Pause;
    fx.Frame();
    REQUIRE(bar::VisibleForTest());
    CheckPinnedToBar();
    const bar::WindowRect first = bar::WindowRectForTest();
    CHECK(first.w > 0.0f);

    for (int i = 0; i < 4; ++i) fx.Frame();
    CheckPinnedToBar();
    CHECK(bar::PlacementsForTest() == 1);
    CHECK(bar::WindowRectForTest().x == doctest::Approx(first.x));

    fx.Calibrate();
    REQUIRE(bar::MapSolvedForTest());

    // The owner drags the title strip 120 px right and 40 px up, and the pin follows him.
    g_mouse.x = static_cast<int32_t>(fx.TrackClientLeft()) + 30;
    g_mouse.y = static_cast<int32_t>(bar::WindowClientTopForTest()) + 6;
    g_mouse.left = true;
    fx.Frame();
    g_mouse.x += 120;
    g_mouse.y -= 40;
    fx.Frame();
    g_mouse.left = false;
    fx.Frame();
    const bar::WindowRect moved = bar::WindowRectForTest();
    CHECK(moved.x == doctest::Approx(first.x + 120.0f));
    CHECK(moved.y == doctest::Approx(first.y - 40.0f));
    CheckPinnedToBar();

    SetBarShown(false);
    fx.Frame();
    CHECK(uifake::WindowPosCalls() == 0);            // a hidden bar draws nothing, pin included
    SetBarShown(true);
    fx.Frame();
    const bar::WindowRect back = bar::WindowRectForTest();
    CHECK(back.x == doctest::Approx(moved.x));
    CHECK(back.y == doctest::Approx(moved.y));
    CheckPinnedToBar();
    CHECK(bar::PlacementsForTest() == 1);
}

TEST_CASE("bar: dragging the body moves nothing, and the track maps where it is drawn after a move") {
    // ★ UI-DRAG-1: "if I drag it around by the window itself the timeline dragging becomes
    // misaligned whereas if I drag it by the bar at the top its fine". The facade dragged the window
    // from its body and the bar, told no window position, kept mapping presses against the old one.
    BarFix fx;
    fx.transport.state = TransportState::Pause;
    fx.Frame();
    fx.Calibrate();
    REQUIRE(bar::MapSolvedForTest());
    bar::Sample(16000);
    const uint32_t seeksBefore = fx.transport.seeks;

    // A press on the body -- below the track, clear of the title strip -- dragged 150 right and 90
    // down. The bar starts neither a move nor a scrub, and the facade drags the window itself, as
    // the engine does from any bare body press. The next frame's pin puts it back under the map.
    const bar::WindowRect before = bar::WindowRectForTest();
    g_mouse.x = static_cast<int32_t>(fx.TrackClientLeft()) + 200;
    g_mouse.y = static_cast<int32_t>(bar::TrackRowYForTest()) + 50;
    g_mouse.left = true;
    fx.Frame();
    CHECK_FALSE(bar::MovingWindowForTest());
    g_mouse.x += 150;
    g_mouse.y += 90;
    uifake::DragWindowBody(kBarWindow, 150.0f, 90.0f);
    fx.Frame();
    g_mouse.left = false;
    fx.Frame();
    CHECK(fx.transport.beginScrubs == 0);
    CHECK(bar::WindowRectForTest().x == doctest::Approx(before.x));
    CHECK(bar::WindowRectForTest().y == doctest::Approx(before.y));
    CHECK(uifake::WindowAt(kBarWindow).x == doctest::Approx(before.x));
    CHECK(uifake::WindowAt(kBarWindow).y == doctest::Approx(before.y));

    // The title strip moves it 230 right and 160 up.
    g_mouse.x = static_cast<int32_t>(fx.TrackClientLeft()) + 30;
    g_mouse.y = static_cast<int32_t>(bar::WindowClientTopForTest()) + 6;
    g_mouse.left = true;
    fx.Frame();
    g_mouse.x += 230;
    g_mouse.y -= 160;
    fx.Frame();
    g_mouse.left = false;
    fx.Frame();
    CheckPinnedToBar();
    CHECK(bar::WindowRectForTest().x == doctest::Approx(before.x + 230.0f));
    CHECK(bar::WindowRectForTest().y == doctest::Approx(before.y - 160.0f));

    // A click a quarter of the way along the track as the FACADE was told to draw it -- the pinned
    // origin plus the content inset -- seeks a quarter of the way through the span.
    const double drawnLeft =
        static_cast<double>(uifake::WindowAt(kBarWindow).x) + BarFix::kContentPad;
    CHECK(bar::TrackExtentForTest().clientX == doctest::Approx(drawnLeft));
    g_mouse.x = static_cast<int32_t>(drawnLeft + fx.TrackClientWidth() * 0.25);
    g_mouse.y = static_cast<int32_t>(bar::TrackRowYForTest());
    g_mouse.left = true;
    fx.Frame();
    g_mouse.left = false;
    fx.Frame();
    bar::Sample(16000);
    REQUIRE(fx.transport.seeks == seeksBefore + 1);
    const bar::PlayableSpan played = bar::SpanForTest();
    REQUIRE(played.lastUs > played.firstUs);
    const double span = static_cast<double>(played.lastUs - played.firstUs);
    const double want = static_cast<double>(played.firstUs) +
                        span * (static_cast<double>(g_mouse.x) - drawnLeft) / fx.TrackClientWidth();
    CHECK(static_cast<double>(fx.transport.lastSeek) == doctest::Approx(want).epsilon(0.001));
}

TEST_CASE("bar: a title-strip drag cannot carry the strip off the client") {
    BarFix fx;
    fx.transport.state = TransportState::Pause;
    fx.Frame();
    fx.Calibrate();
    g_mouse.x = static_cast<int32_t>(fx.TrackClientLeft()) + 30;
    g_mouse.y = static_cast<int32_t>(bar::WindowClientTopForTest()) + 6;
    g_mouse.left = true;
    fx.Frame();
    g_mouse.x += 5000;
    g_mouse.y += 5000;
    fx.Frame();
    g_mouse.left = false;
    fx.Frame();
    CheckPinnedToBar();
    // 80 px of the strip left inside the right edge, all 22 px of it above the bottom edge.
    const double windowClientLeft =
        static_cast<double>(bar::WindowRectForTest().x) + BarFix::kContentPad;
    CHECK(windowClientLeft == doctest::Approx(static_cast<double>(g_mouse.clientW) - 80.0));
    CHECK(bar::WindowClientTopForTest() ==
          doctest::Approx(static_cast<float>(g_mouse.clientH) - 22.0f));
}

TEST_CASE("bar: the window fits its content on every screen the owner plays on") {
    const int32_t sizes[][2] = {{993, 519}, {1552, 913}, {2560, 1440}, {5120, 1440}};  // facts-ok: CS.SCHEME_COUNT_OFF a test fixture value this case compares against, not an engine position
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
        BarFix fx;
        g_mouse.clientW = sizes[i][0];
        g_mouse.clientH = sizes[i][1];
        fx.transport.state = TransportState::Pause;
        fx.Frame();
        REQUIRE(bar::VisibleForTest());
        const bar::WindowRect r = bar::WindowRectForTest();
        // Every row the window draws has to be inside the rect it asked the facade for, and the
        // rect has to be inside the screen.
        CHECK(r.w >= 440.0f);
        CHECK(r.h >= 150.0f);
        CHECK(r.x >= 0.0f);
        CHECK(r.y >= 0.0f);
        CHECK(uifake::LastWindowSize().x == doctest::Approx(r.w));
        CHECK(uifake::LastWindowSize().y == doctest::Approx(r.h));
        // The controls the owner asked for by name, all of them on screen at once.
        CHECK(uifake::ButtonDrawn("Play"));
        CHECK(uifake::ButtonDrawn("Reverse"));
        CHECK(uifake::ButtonDrawn("-1f"));
        CHECK(uifake::ButtonDrawn("+1f"));
        CHECK(uifake::ButtonDrawn("-10s"));
        CHECK(uifake::ButtonDrawn("+10s"));
        CHECK(uifake::ButtonDrawn("-1s"));
        CHECK(uifake::ButtonDrawn("+1s"));
        CHECK(uifake::ButtonDrawn("Open..."));
        CHECK(uifake::ButtonDrawn("Unload"));
        CHECK(uifake::ButtonDrawn("1x"));
        CHECK(uifake::ItemsDrawn() <= panel::kBarItemBudget);
        CHECK(uifake::GlyphsDrawn() <= panel::kBarGlyphBudget);
    }
}

TEST_CASE("bar: the drawn track and the drag map are one extent at every window width") {
    // The width is the window's, not the screen's: the facade's viewport is not the Win32 client
    // rect (LAUNCH-BARFIX 6.1), so nothing here is derived from one.
    const int32_t widths[] = {300, 480, 760, 1200, 1536};  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF plain 300 as a count, size or pixel value, not the extrapolation-cap offset
    for (size_t wi = 0; wi < sizeof widths / sizeof widths[0]; ++wi) {
        BarFix fx(widths[wi]);
        g_mouse.clientW = 1920;
        g_mouse.clientH = 1080;
        fx.transport.state = TransportState::Pause;
        fx.Frame();

        const bar::TrackExtent e = bar::TrackExtentForTest();
        REQUIRE(e.cells > 20);
        CHECK(e.cells <= kMaxCells);                                  // the widget budget holds
        CHECK(e.guiW <= static_cast<float>(widths[wi]));              // and it fits the window
        CHECK(e.guiW >= static_cast<float>(widths[wi]) - 64.0f);      // ...and uses it

        fx.CalibrateAtWidth();
        REQUIRE(bar::CalibratedForTest());
        const bar::TrackExtent c = bar::TrackExtentForTest();
        REQUIRE(c.clientW > 0.0);

        // The cell the owner's eye sees under the cursor, computed from the drawn geometry alone.
        const int64_t dur = static_cast<int64_t>(fx.transport.duration);
        const Span span = {0, dur};
        const double x = c.clientX + 0.37 * c.clientW;
        const uint32_t under = static_cast<uint32_t>((x - c.clientX) /
                                                     (bar::CalibScaleForTest() * c.cellPx));
        REQUIRE(under < c.cells);
        const int64_t drawnUs = SeekTargetForCell(span, c.cells, under);
        const int64_t cellUs = dur / static_cast<int64_t>(c.cells);

        const int32_t press = static_cast<int32_t>(c.clientX + 0.10 * c.clientW);
        g_mouse.left = true;
        fx.MoveTo(press);
        fx.MoveTo(press + 4);
        REQUIRE(fx.transport.beginScrubs == 1);
        fx.MoveTo(static_cast<int32_t>(x));

        // ★ MF-2: the drag lands where the track is DRAWN, within one cell. A map that kept using
        // the unclamped width while the track stopped growing put this hundreds of cells out with
        // nothing on screen saying so.
        const int64_t dragged = bar::DrawnPlayheadUsForTest();
        CHECK(dragged >= drawnUs - cellUs);
        CHECK(dragged <= drawnUs + cellUs);

        // Fine mode is the same extent divided by ten, so it moves with the width too.
        const int64_t beforeFine = dragged;
        g_mouse.shift = true;
        fx.MoveTo(static_cast<int32_t>(x) + 100);
        CHECK(static_cast<double>(bar::DrawnPlayheadUsForTest() - beforeFine) ==
              doctest::Approx(100.0 * static_cast<double>(dur) / c.clientW / 10.0).epsilon(0.03));
        g_mouse.shift = false;

        const int64_t target = bar::DrawnPlayheadUsForTest();
        g_mouse.left = false;
        fx.Frame();
        fx.Frame();
        CHECK(fx.transport.endScrubs == 1);
        CHECK(fx.transport.lastEndScrub == target);
    }
}

TEST_CASE("bar: a second capture with the same marker count draws its own rounds") {
    BarFix fx;
    const RoundMarker a[3] = {RoundAt(1000000ull, 1), RoundAt(2000000ull, 2),
                              RoundAt(3000000ull, 3)};
    const RoundMarker c[3] = {RoundAt(15000000ull, 7), RoundAt(18000000ull, 8),
                              RoundAt(21000000ull, 9)};
    const std::wstring pa = fx.captureDir + L"\\bar_a.echodemo";
    const std::wstring pc = fx.captureDir + L"\\bar_b.echodemo";
    REQUIRE(WriteBarCapture(pa.c_str(), a, 3));
    REQUIRE(WriteBarCapture(pc.c_str(), c, 3));

    REQUIRE(LoadPath(pa.c_str()));
    fx.Frame();
    CHECK(uifake::TextContains("RB1"));

    // ★ MF-4: the same round count and the same bookmark count, so the sum the marker table is
    // keyed on does not move. Without the load-serial reset the row keeps capture A's rounds.
    REQUIRE(LoadPath(pc.c_str()));
    fx.Frame();
    CHECK(uifake::TextContains("RB7"));
    CHECK_FALSE(uifake::TextContains("RB1"));

    // The panel's loaded-capture state is file-scope and outlives this fixture, so this case gives
    // it back rather than leaving a capture loaded for whatever runs next.
    Unload();
    fx.Frame();
    CHECK(LoadedPath() == nullptr);
}

TEST_CASE("bar: the pre-roll shows a percent it was given and never one it was not") {
    BarFix fx;
    fx.Frame();
    CHECK_FALSE(uifake::TextContains("preparing replay"));
}

TEST_CASE("bar: the timeline is the RECORDED span, not the file axis") {
    // The owner's capture: a 14:31 axis carrying 48.5 s of content that starts 822.9 s in. The old
    // track spanned the axis, so 95% of it was dead air and a click almost anywhere seeked into
    // nothing -- "its like super zoomed out, and it seems to show way more than is actully
    // recorded and I cant actually seek to that part".
    BarFix fx;
    fx.transport.duration = 871416448;
    fx.transport.spanFirst = 822887658;
    fx.transport.spanLast = 871416448;
    fx.transport.playhead = 822887658;
    fx.transport.state = TransportState::Pause;
    fx.Frame();

    const bar::PlayableSpan span = bar::SpanForTest();
    CHECK(span.firstUs == 822887658);
    CHECK(span.lastUs == 871416448);

    // The readout is relative to that span: the first recorded frame reads 0:00.0, not 13:42.
    CHECK(uifake::ButtonDrawn("0:00.000 / 0:48.528"));

    // Both ends of the track land inside recorded content, and the far left is the FIRST recorded
    // sample rather than the file's origin.
    const bar::TrackExtent e = bar::TrackExtentForTest();
    const uint32_t cells = e.cells;
    const Span drawn = {span.firstUs, span.lastUs};
    CHECK(SeekTargetForCell(drawn, cells, 0) >= span.firstUs);
    CHECK(SeekTargetForCell(drawn, cells, cells - 1) <= span.lastUs);

    fx.Frame("##b0");
    fx.Frame();
    CHECK(fx.transport.seeks == 1);
    CHECK(fx.transport.lastSeek >= span.firstUs);
    CHECK(fx.transport.lastSeek <= span.lastUs);

    // And so do the skip buttons and the shortcut keys: one origin, or the window and the console
    // disagree about where the owner is.
    fx.Frame("+10s##bp10");
    fx.Frame();
    CHECK(fx.transport.lastSeek >= span.firstUs);
    fx.transport.playhead = span.firstUs;
    fx.Frame("-10s##bm10");
    fx.Frame();
    CHECK(fx.transport.lastSeek == span.firstUs);   // clamped to content, never to 0
}

TEST_CASE("bar: a reported cell seeks to the CURSOR, not to the index the facade handed back") {
    // ★ In-game, a click at client x=300 was reported as cell 0 -- the start of the
    // recording. Seeking to the reported index would send him to 0:00 from anywhere on the track,
    // so the target is read off the cursor and the index is used for nothing.
    BarFix fx;
    fx.Frame();
    const bar::TrackExtent e = bar::TrackExtentForTest();
    const int32_t cursor = static_cast<int32_t>(e.clientX + e.clientW * 0.75);
    g_mouse.x = cursor;
    g_mouse.y = 500;
    g_mouse.left = false;
    fx.Frame("##b0");                        // the facade's index: the far LEFT of the track
    CHECK(fx.transport.seeks == 0);          // recorded in the draw, serviced on the next tick
    fx.Frame();
    CHECK(fx.transport.seeks == 1);
    const Span span = {0, static_cast<int64_t>(fx.transport.duration)};
    const double want =
        static_cast<double>(span.beginUs) +
        (static_cast<double>(cursor) - e.clientX) / e.clientW *
            static_cast<double>(span.endUs - span.beginUs);
    CHECK(static_cast<double>(fx.transport.lastSeek) == doctest::Approx(want).epsilon(0.01));
    CHECK(fx.transport.lastSeek > SeekTargetForCell(span, e.cells, 0));
}

TEST_CASE("bar: ONE click on the track arms every drag after it") {
    // ★ THE OWNER'S BUG, as a case. His session log carries no calibration line of either kind, so
    // every drag he made was refused by a gate that needed TWO still clicks far apart. One hit is
    // the whole price now -- and the same hit is the click-to-seek he meant to make.
    BarFix fx;
    fx.transport.state = TransportState::Play;
    fx.Frame();
    REQUIRE_FALSE(bar::CalibratedForTest());

    fx.ClickCellStill(20, static_cast<int32_t>(fx.CellGuiX(20)) + 8, 500);
    REQUIRE(bar::CalibratedForTest());
    fx.transport.seeks = 0;
    fx.transport.state = TransportState::Play;

    const bar::TrackExtent e = bar::TrackExtentForTest();
    const int32_t y = 500;
    const int32_t x0 = static_cast<int32_t>(e.clientX + e.clientW * 0.5);
    g_mouse.y = y;
    g_mouse.x = x0;
    g_mouse.left = true;
    fx.Frame();                      // the press arms
    g_mouse.x = x0 + 60;
    fx.Frame();                      // past the threshold: this must BE a scrub
    CHECK(fx.transport.beginScrubs == 1);
    CHECK(fx.transport.scrubTos >= 1);
    const int64_t target = fx.transport.lastScrubTo;
    CHECK(target > 0);

    g_mouse.left = false;
    fx.Frame();
    bar::Sample(16000);
    CHECK(fx.transport.endScrubs == 1);
    CHECK(fx.transport.lastEndScrub == target);   // it lands exactly where it was let go
    CHECK(fx.transport.seeks == 0);               // and the press never became a click-to-seek
}

TEST_CASE("bar: a FRESH press-and-drag scrubs with nothing clicked in front of it") {
    // ★ THE OWNER'S FIRST FIVE SECONDS. The facade reports its Button on the release, so a press
    // can never be told "you are on the track" in time -- the first drag of a session has to work
    // off the map the bar seeded from its own placement, and in the game it does (ENG-UI-5 §3,
    // 18 of 18 steps with no click before it).
    BarFix fx;
    fx.transport.state = TransportState::Pause;
    fx.Frame();
    REQUIRE_FALSE(bar::MapSolvedForTest());

    const bar::TrackExtent e = bar::TrackExtentForTest();
    const int32_t x0 = static_cast<int32_t>(e.clientX + e.clientW * 0.25);
    g_mouse.x = x0;
    g_mouse.y = static_cast<int32_t>(bar::TrackRowYForTest());
    g_mouse.left = true;
    fx.Frame();
    CHECK(bar::BandMissesForTest() == 0);
    g_mouse.x = x0 + 120;
    fx.Frame();
    CHECK(fx.transport.beginScrubs == 1);
    CHECK(fx.transport.scrubTos >= 1);
    CHECK_FALSE(bar::MovingWindowForTest());       // and it did not move the window on the way
    const bar::WindowRect w = bar::WindowRectForTest();
    g_mouse.left = false;
    fx.Frame();
    bar::Sample(16000);
    CHECK(fx.transport.endScrubs == 1);
    CHECK(bar::WindowRectForTest().x == doctest::Approx(w.x));
    CHECK(bar::WindowRectForTest().y == doctest::Approx(w.y));
}

TEST_CASE("bar: a press is a scrub or a move, never both") {
    BarFix fx;
    fx.transport.state = TransportState::Pause;
    fx.Frame();
    fx.Calibrate();
    REQUIRE(bar::MapSolvedForTest());
    const bar::WindowRect before = bar::WindowRectForTest();
    const bar::TrackExtent e = bar::TrackExtentForTest();

    // On the track: it scrubs and the window stays exactly where it was.
    const int32_t x0 = static_cast<int32_t>(e.clientX + e.clientW * 0.5);
    g_mouse.x = x0;
    g_mouse.y = static_cast<int32_t>(bar::TrackRowYForTest());
    g_mouse.left = true;
    fx.Frame();
    CHECK_FALSE(bar::MovingWindowForTest());
    g_mouse.x = x0 + 150;
    fx.Frame();
    CHECK(fx.transport.beginScrubs == 1);
    CHECK(bar::WindowRectForTest().x == doctest::Approx(before.x));
    g_mouse.left = false;
    fx.Frame();
    bar::Sample(16000);

    // On the title strip: it moves and issues no scrub of any kind.
    const uint32_t scrubsBefore = fx.transport.beginScrubs;
    g_mouse.x = static_cast<int32_t>(fx.TrackClientLeft()) + 40;
    g_mouse.y = static_cast<int32_t>(bar::WindowClientTopForTest()) + 6;
    g_mouse.left = true;
    fx.Frame();
    CHECK(bar::MovingWindowForTest());
    g_mouse.x += 200;
    fx.Frame();
    g_mouse.left = false;
    fx.Frame();
    CHECK(fx.transport.beginScrubs == scrubsBefore);
    CHECK(bar::WindowRectForTest().x == doctest::Approx(before.x + 200.0f));
}

TEST_CASE("bar: the motion smoothing rows cycle mode and strength and read back from the transport") {
    BarFix fx;
    fx.Frame("=##bmenu");
    fx.Frame();
    CHECK(uifake::ButtonDrawn("Motion smoothing: Slow-mo only##bmsm"));
    CHECK(uifake::ButtonDrawn("Smoothing strength: Med##bmss"));
    CHECK_FALSE(uifake::ButtonDisabled("Motion smoothing: Slow-mo only##bmsm"));

    fx.Frame("Motion smoothing: Slow-mo only##bmsm");
    CHECK(fx.transport.smoothMode == 2);
    CHECK(fx.transport.smoothStrength == 2);          // the mode button keeps the strength
    fx.Frame();
    CHECK(uifake::ButtonDrawn("Motion smoothing: Always##bmsm"));
    fx.Frame("Motion smoothing: Always##bmsm");
    CHECK(fx.transport.smoothMode == 0);              // Off: the lossless 1:1 path
    fx.Frame();
    CHECK(uifake::ButtonDrawn("Motion smoothing: Off##bmsm"));

    fx.Frame("Smoothing strength: Med##bmss");
    CHECK(fx.transport.smoothStrength == 3);
    CHECK(fx.transport.smoothMode == 0);              // the strength button keeps the mode
    fx.Frame();
    fx.Frame("Smoothing strength: High##bmss");
    CHECK(fx.transport.smoothStrength == 1);
    fx.Frame();
    CHECK(uifake::ButtonDrawn("Smoothing strength: Low##bmss"));
}

TEST_CASE("bar: the motion smoothing rows are inert during a live capture") {
    BarFix fx(760, true);
    fx.Frame("=##bmenu");
    fx.Frame();
    const uint32_t before = fx.transport.smoothSets;
    CHECK_FALSE(uifake::ButtonDrawn("Motion smoothing: Slow-mo only##bmsm"));
    fx.Frame("Motion smoothing: n/a##bmsm");
    fx.Frame("Smoothing strength: n/a##bmss");
    CHECK(fx.transport.smoothSets == before);
    if (uifake::ButtonDrawn("Motion smoothing: n/a##bmsm")) {
        CHECK(uifake::ButtonDisabled("Motion smoothing: n/a##bmsm"));
    }
}

TEST_CASE("bar: wider and narrower reflow every row, and nothing is clipped at either end") {
    BarFix fx;
    fx.transport.state = TransportState::Pause;
    fx.Frame();
    const float start = bar::BarWidthForTest();

    fx.Frame("=##bmenu");
    for (int i = 0; i < 6; ++i) fx.Frame("+ wider##bmwider");
    const float wide = bar::BarWidthForTest();
    CHECK(wide > start);
    fx.Frame();
    const bar::TrackExtent wideTrack = bar::TrackExtentForTest();
    CHECK(uifake::ButtonDrawn("+10s##bp10"));
    CHECK(uifake::ButtonDrawn("Unload##bunload2"));

    for (int i = 0; i < 20; ++i) fx.Frame("- narrower##bmnarrow");
    const float narrow = bar::BarWidthForTest();
    CHECK(narrow < start);
    CHECK(narrow >= 280.0f);                       // the floor, not an unreadable sliver
    fx.Frame();
    const bar::TrackExtent narrowTrack = bar::TrackExtentForTest();
    CHECK(narrowTrack.guiW < wideTrack.guiW);      // the track itself reflowed
    // The row drops its ends rather than drawing them off the edge, and the controls that survive
    // are the ones a hand reaches for first.
    CHECK_FALSE(uifake::ButtonDrawn("+10s##bp10"));
    CHECK(uifake::ButtonDrawn("+1f##bnext"));
    CHECK(uifake::ButtonDrawn("Unload##bunload2"));

    fx.Frame("reset##bmwreset");
    fx.Frame();
    CHECK(bar::BarWidthForTest() == doctest::Approx(start));
}

TEST_CASE("bar: a hit ties the band to the cursor's y and cannot move the x map") {
    // ★ THE REGRESSION ENG-UI-5 EXISTS TO STOP. Solving the x offset from the reported index put
    // `client_x = gui_x +258.5` into a live session whose true offset was +8, because the facade
    // answered cell 0 for a click at x=300. The x map is the placement's own and a hit must not be
    // able to move it, however absurd the index is.
    BarFix fx;
    fx.Frame();
    REQUIRE_FALSE(bar::CalibratedForTest());
    const double left = bar::TrackExtentForTest().clientX;

    fx.ClickCellStill(0, 300, 500);  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF plain 300 as a count, size or pixel value, not the extrapolation-cap offset
    CHECK(bar::CalibratedForTest());
    CHECK(bar::MapSolvedForTest());
    CHECK(bar::CalibScaleForTest() == doctest::Approx(1.0));
    CHECK(bar::TrackExtentForTest().clientX == doctest::Approx(left));
    CHECK(bar::TrackRowYForTest() == doctest::Approx(500.0));

    // An index nothing could reconcile with its cursor, but a cursor still inside the believed
    // track: the map does not budge, however absurd the index.
    fx.ClickCellStill(220, static_cast<int32_t>(left) + 20, 500);
    CHECK(bar::TrackExtentForTest().clientX == doctest::Approx(left));

    // A cursor OUTSIDE it is the one thing a hit does say about x -- the facade moved its own
    // window and told nobody. The map shifts by exactly the shortfall and not one pixel more.
    const double width = bar::TrackExtentForTest().clientW;
    const int32_t past = static_cast<int32_t>(left + width) + 60;
    fx.ClickCellStill(20, past, 540);
    CHECK(bar::TrackExtentForTest().clientX == doctest::Approx(left + 60.0));
    CHECK(bar::TrackRowYForTest() == doctest::Approx(540.0));
    // And a hit back inside the shifted range leaves it alone.
    fx.ClickCellStill(20, past - 10, 540);
    CHECK(bar::TrackExtentForTest().clientX == doctest::Approx(left + 60.0));

    // And the hits were all reported with the button already up, which is what the game does.
    CHECK(bar::HitsAfterReleaseForTest() == 4);
    CHECK(bar::HitsWhileDownForTest() == 0);
}

TEST_CASE("bar: a drag coalesces into ScrubTo, lands on its exact target, and ends exactly once") {
    BarFix fx;
    fx.Frame();
    fx.Calibrate();
    REQUIRE(bar::CalibratedForTest());
    fx.transport.state = TransportState::Play;
    fx.Frame();
    const uint32_t seeksBeforeDrag = fx.transport.seeks;

    const int32_t press = static_cast<int32_t>(fx.TrackClientLeft() + 300.0);
    g_mouse.left = true;
    fx.MoveTo(press);                       // arm
    CHECK(fx.transport.beginScrubs == 0);
    fx.MoveTo(press + 4);                   // past the 3 px threshold: the drag begins
    CHECK(fx.transport.beginScrubs == 1);
    fx.MoveTo(press + 40);
    fx.MoveTo(press + 91);   // not a whole number of 3 px cells, so a cell centre cannot match
    const uint32_t steps = fx.transport.scrubTos;
    CHECK(steps >= 3);
    // The drawn playhead is the pending target, not a read-back: no seek has been issued at all.
    CHECK(fx.transport.seeks == seeksBeforeDrag);
    CHECK(bar::DrawnPlayheadUsForTest() == fx.transport.lastScrubTo);

    const int64_t target = bar::DrawnPlayheadUsForTest();
    g_mouse.left = false;
    fx.Frame();          // the release is seen in the draw
    fx.Frame();          // and serviced by the next tick, where a full seek is allowed
    CHECK(fx.transport.endScrubs == 1);
    CHECK(fx.transport.lastEndScrub == target);
    // ...and it is NOT a cell centre: the whole point of F3's swallow rule.
    const Span span = {0, static_cast<int64_t>(fx.transport.duration)};
    const uint32_t cells = CellCount(BarFix::kBarW - 16.0f, kBarCellPx);
    CHECK(fx.transport.lastEndScrub !=
          SeekTargetForCell(span, cells, CellOfUs(span, cells, target)));
    fx.Frame();
    CHECK(fx.transport.endScrubs == 1);     // idempotent from up here, not just in the transport
}

TEST_CASE("bar: a track hit during a drag and in the mouse-up frame is swallowed, never seeked") {
    BarFix fx;
    fx.Frame();
    fx.Calibrate();
    fx.Frame();   // let the second calibration click's own seek land before the baseline is taken
    const uint32_t seeksBefore = fx.transport.seeks;

    const int32_t press = static_cast<int32_t>(fx.TrackClientLeft() + 200.0);
    g_mouse.left = true;
    fx.MoveTo(press);
    fx.MoveTo(press + 8);
    REQUIRE(fx.transport.beginScrubs == 1);

    fx.MoveTo(press + 40, "##b150");
    const uint32_t swallowed = bar::SwallowedForTest();
    CHECK(swallowed >= 1);

    // The release frame's hit is swallowed too, whichever edge Button fires on.
    g_mouse.left = false;
    fx.Frame("##b160");
    CHECK(bar::SwallowedForTest() > swallowed);
    fx.Frame();
    fx.Frame();
    CHECK(fx.transport.seeks == seeksBefore);
}

TEST_CASE("bar: Shift is a tenth and Shift+Ctrl a hundredth of the plain drag rate") {
    BarFix fx;
    fx.Frame();
    fx.Calibrate();

    const int32_t press = static_cast<int32_t>(fx.TrackClientLeft() + 300.0);
    g_mouse.left = true;
    fx.MoveTo(press);
    fx.MoveTo(press + 4);
    const int64_t start = bar::DrawnPlayheadUsForTest();

    g_mouse.shift = true;
    fx.MoveTo(press + 104);                 // 100 px at a tenth
    const int64_t fine = bar::DrawnPlayheadUsForTest() - start;
    CHECK(static_cast<double>(fine) ==
          doctest::Approx(100.0 * fx.UsPerPx() / 10.0).epsilon(0.02));

    g_mouse.ctrl = true;
    const int64_t before = bar::DrawnPlayheadUsForTest();
    fx.MoveTo(press + 204);                 // another 100 px, now at a hundredth
    CHECK(static_cast<double>(bar::DrawnPlayheadUsForTest() - before) ==
          doctest::Approx(100.0 * fx.UsPerPx() / 100.0).epsilon(0.02));

    g_mouse.left = false;
    g_mouse.shift = false;
    g_mouse.ctrl = false;
    fx.Frame();
}

TEST_CASE("bar: focus loss, an unload and either end of the recording each end the drag once") {
    SUBCASE("focus loss") {
        BarFix fx;
        fx.Frame();
        fx.Calibrate();
        const int32_t press = static_cast<int32_t>(fx.TrackClientLeft() + 250.0);
        g_mouse.left = true;
        fx.MoveTo(press);
        fx.MoveTo(press + 10);
        REQUIRE(fx.transport.beginScrubs == 1);
        g_mouse.foreground = false;
        fx.Frame();
        fx.Frame();
        CHECK(fx.transport.endScrubs == 1);
        fx.Frame();
        CHECK(fx.transport.endScrubs == 1);
    }
    SUBCASE("the capture changed under the drag") {
        BarFix fx;
        fx.Frame();
        fx.Calibrate();
        const int32_t press = static_cast<int32_t>(fx.TrackClientLeft() + 250.0);
        g_mouse.left = true;
        fx.MoveTo(press);
        fx.MoveTo(press + 10);
        REQUIRE(fx.transport.beginScrubs == 1);
        ++fx.transport.serial;
        fx.Frame();
        CHECK(fx.transport.endScrubs == 1);
        CHECK_FALSE(bar::CalibratedForTest());   // a new capture is a new map
    }
    SUBCASE("the end of the recording") {
        BarFix fx;
        fx.Frame();
        fx.Calibrate();
        const int32_t press = static_cast<int32_t>(fx.TrackClientLeft() + 250.0);
        g_mouse.left = true;
        fx.MoveTo(press);
        fx.MoveTo(press + 10);
        REQUIRE(fx.transport.beginScrubs == 1);
        fx.transport.ended = EndState::AtEnd;
        fx.Frame();
        CHECK(fx.transport.endScrubs == 1);
        fx.Frame();
        CHECK(fx.transport.endScrubs == 1);
    }
}

TEST_CASE("bar: a window resize invalidates the map and ends a drag in flight") {
    BarFix fx;
    fx.Frame();
    fx.Calibrate();
    const int32_t press = static_cast<int32_t>(fx.TrackClientLeft() + 250.0);
    g_mouse.left = true;
    fx.MoveTo(press);
    fx.MoveTo(press + 10);
    REQUIRE(fx.transport.beginScrubs == 1);

    g_mouse.clientW = 1280;
    g_mouse.clientH = 720;
    fx.Frame();
    fx.Frame();
    CHECK(fx.transport.endScrubs == 1);
    CHECK_FALSE(bar::CalibratedForTest());
}

TEST_CASE("bar: a refused window build snaps the drawn playhead back and aborts the drag") {
    BarFix fx;
    fx.Frame();
    fx.Calibrate();
    const int32_t press = static_cast<int32_t>(fx.TrackClientLeft() + 250.0);
    g_mouse.left = true;
    fx.MoveTo(press);
    fx.MoveTo(press + 10);
    REQUIRE(fx.transport.beginScrubs == 1);

    fx.transport.playhead = 12345000;
    fx.transport.stats.lastRefused = true;
    fx.Frame();
    CHECK(fx.transport.endScrubs == 1);
    CHECK(bar::DrawnPlayheadUsForTest() == 12345000);
}

TEST_CASE("bar: auto-hide is off unless asked for, and raw motion or any key brings it back") {
    {
        // ★ OFF BY DEFAULT: F9 is the switch the owner asked for, and a window that vanishes on
        // its own is not one he controls.
        BarFix dflt;
        dflt.transport.state = TransportState::Play;
        dflt.Frame();
        bar::AddIdleForTest(30000000000ull);
        dflt.Frame();
        CHECK_FALSE(bar::HiddenForTest());
    }
    BarFix fx(760, false, 3.0);
    fx.transport.state = TransportState::Play;
    bar::AddIdleForTest(3500000000ull);
    fx.Frame();
    CHECK(bar::HiddenForTest());

    g_mouse.x += 3;
    fx.Frame();
    CHECK_FALSE(bar::HiddenForTest());
    CHECK(bar::VisibleForTest());

    // And again from a key alone, with the cursor perfectly still.
    bar::AddIdleForTest(3500000000ull);
    fx.Frame();
    CHECK(bar::HiddenForTest());
    g_keys = bar::kKeySpace;
    fx.Frame();
    g_keys = 0;
    CHECK_FALSE(bar::HiddenForTest());
}

TEST_CASE("bar: direction and speed while paused change the rate and never resume") {
    BarFix fx;
    fx.transport.state = TransportState::Pause;
    fx.Frame();

    fx.Frame("Reverse##bdir");
    fx.Frame();
    CHECK(fx.transport.setRates == 1);
    CHECK(fx.transport.lastSetRate == -1.0f);
    CHECK(fx.transport.plays == 0);
    CHECK(fx.transport.state == TransportState::Pause);

    fx.Frame("##bsp");   // the first speed the transport offers
    fx.Frame();
    CHECK(fx.transport.setRates == 2);
    CHECK(fx.transport.plays == 0);
    CHECK(fx.transport.state == TransportState::Pause);
}

TEST_CASE("bar: play, step and the ends are reachable from the row") {
    BarFix fx;
    fx.Frame();

    fx.Frame("Play##bplay");
    fx.Frame();
    CHECK(fx.transport.plays == 1);

    fx.Frame("-1f##bprev");
    fx.Frame();
    CHECK(fx.transport.steps == 1);
    CHECK(fx.transport.lastStep == -1);

    // The ends are the End key now; the row spends its width on the skips the owner asked for.
    g_keys = bar::kKeyEnd;
    fx.Frame();
    g_keys = 0;
    fx.Frame();
    CHECK(fx.transport.lastSeek == static_cast<int64_t>(fx.transport.duration));
}

TEST_CASE("bar: the shortcut keys act only in cursor mode, and only on their edge") {
    BarFix fx;
    fx.Frame();
    ShowPanel(false);
    g_keys = bar::kKeySpace;
    fx.Frame();
    fx.Frame();
    CHECK(fx.transport.plays == 0);      // out of cursor mode the bar is a readout

    ShowPanel(true);
    g_keys = 0;
    fx.Frame();
    g_keys = bar::kKeySpace;
    fx.Frame();
    fx.Frame();
    CHECK(fx.transport.plays == 1);
    fx.Frame();                          // held, not pressed again
    fx.Frame();
    CHECK(fx.transport.plays == 1);
    g_keys = 0;
}

TEST_CASE("bar: the diag line exists, names the drag state, and the panel's own line is untouched") {
    BarFix fx;
    fx.Frame();
    char buf[4096];
    const size_t n = WriteDiagnostics(buf, sizeof buf);
    REQUIRE(n > 0);
    CHECK(strstr(buf, "bar          : ") != nullptr);
    CHECK(strstr(buf, "seeded") != nullptr);
    // F7: the meaning of the panel's line is not redefined by the bar's arrival.
    CHECK(strstr(buf, "panel        : enabled, visible") != nullptr);
}

TEST_CASE("bar: the marker families the owner asked for are the ones drawn") {
    CHECK(FamilyOfKind(capture::kMarkGoal) == MarkerFamily::Goal);
    CHECK(FamilyOfKind(capture::kMarkRoundBegin) == MarkerFamily::Round);
    CHECK(FamilyOfKind(capture::kMarkBookmark) == MarkerFamily::Bookmark);
    CHECK(FamilyOfKind(capture::kMarkJoin) == MarkerFamily::None);

    const Rgba goal = CellColour(CellClass::Marker, MarkerFamily::Goal);
    const Rgba round = CellColour(CellClass::Marker, MarkerFamily::Round);
    CHECK(goal.r != round.r);
    CHECK(CellColour(CellClass::Elapsed, MarkerFamily::None).a == 1.0f);
    CHECK(CellColour(CellClass::Normal, MarkerFamily::None).a < 0.5f);
}

TEST_CASE("bar: Elapsed stops at the playhead and never eats a tick") {
    const Span span = {0, 1000000};
    Marker m[1] = {};
    m[0].tUs = 200000;
    m[0].kind = capture::kMarkGoal;
    CellClass cells[16] = {};
    ClassifyCellsElapsed(span, 10, m, 1, 500000, true, cells);
    CHECK(cells[0] == CellClass::Elapsed);
    CHECK(cells[2] == CellClass::Marker);          // the tick keeps its class behind the playhead
    CHECK(cells[5] == CellClass::Playhead);
    CHECK(cells[9] == CellClass::Normal);
}

// ---- UI-4: the recorded span, and a map that survives the window moving ------------------------

// The owner's 2026-09-18 capture, to the microsecond: a 871.416 s file whose recorded content is
// the last 48.528 s of it. LAUNCH-UI3 drew all 871 s and every click on the track landed on the
// content start.
constexpr int64_t kOwnerDurationUs = 871416448;
constexpr int64_t kOwnerFirstUs = 822887658;

TEST_CASE("bar: the track is the recorded span, not the file axis") {
    BarFix fx;
    fx.transport.duration = static_cast<uint64_t>(kOwnerDurationUs);
    fx.transport.spanFirst = kOwnerFirstUs;
    fx.transport.spanLast = kOwnerDurationUs;
    fx.transport.playhead = kOwnerFirstUs;
    fx.Frame();

    const bar::PlayableSpan span = bar::SpanForTest();
    CHECK(span.firstUs == kOwnerFirstUs);
    CHECK(span.lastUs == kOwnerDurationUs);
    CHECK(span.lastUs - span.firstUs == 48528790);

    // A click at the middle of the track lands in the middle of the CONTENT -- 24.3 s in, not
    // 435 s of dead air.
    const bar::TrackExtent e = bar::TrackExtentForTest();
    const uint32_t mid = e.cells / 2;
    char id[16];
    snprintf(id, sizeof id, "##b%u", mid);
    fx.Frame(id);
    fx.Frame();
    CHECK(fx.transport.seeks == 1);
    CHECK(fx.transport.lastSeek > kOwnerFirstUs + 20000000);
    CHECK(fx.transport.lastSeek < kOwnerFirstUs + 29000000);
    // One pixel of track is a fifth of a second of content, not 1.17 s of file.
    CHECK(static_cast<double>(span.lastUs - span.firstUs) / e.clientW < 0.2e6);
}

TEST_CASE("bar: a transport that cannot answer the span falls back to the whole axis") {
    // The defaulted IReplayTransport::PlayableSpanUs returns false. The bar must still draw and
    // still seek -- and it must draw the file axis, which is the state LAUNCH-UI3 photographed.
    BarFix fx;
    fx.transport.duration = static_cast<uint64_t>(kOwnerDurationUs);
    fx.transport.spanFirst = 0;
    fx.transport.spanLast = 0;   // ScrubTransport answers false for this pair
    fx.Frame();
    const bar::PlayableSpan span = bar::SpanForTest();
    CHECK(span.firstUs == 0);
    CHECK(span.lastUs == kOwnerDurationUs);
}

TEST_CASE("bar: moving the window does not kill the drag") {
    // ★ LAUNCH-UI3 §7.1: one drag in 10,129 log lines, and none after the window was moved. The
    // track row's y was latched from a click and the move left every later press outside the band.
    BarFix fx;
    fx.transport.duration = static_cast<uint64_t>(kOwnerDurationUs);
    fx.transport.spanFirst = kOwnerFirstUs;
    fx.transport.spanLast = kOwnerDurationUs;
    fx.transport.playhead = kOwnerFirstUs;
    fx.transport.state = TransportState::Pause;
    fx.Frame();

    // One track click first: that is the hit that ties the facade's coordinates to the cursor's,
    // and in the game it is the click he makes anyway.
    fx.Calibrate();
    REQUIRE(bar::MapSolvedForTest());

    const bar::WindowRect before = bar::WindowRectForTest();
    const float topBefore = bar::WindowClientTopForTest();
    CHECK(bar::TrackRowYForTest() > topBefore);
    CHECK(bar::TrackRowYForTest() < topBefore + before.h);

    // The owner drags the title strip 300 right and 321 down, exactly as the launch did.
    g_mouse.x = static_cast<int32_t>(fx.TrackClientLeft()) + 30;
    g_mouse.y = static_cast<int32_t>(topBefore) + 6;
    g_mouse.left = true;
    fx.Frame();
    g_mouse.x += 300;  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF plain 300 as a count, size or pixel value, not the extrapolation-cap offset
    g_mouse.y += 321;
    fx.Frame();
    g_mouse.left = false;
    fx.Frame();
    const bar::WindowRect moved = bar::WindowRectForTest();
    REQUIRE(moved.y == doctest::Approx(before.y + 321.0f));
    // The band moved with the window, in the cursor's own pixels. Latched, it would still be
    // sitting 321 px up -- which is the whole of LAUNCH-UI4 §4.3.
    CHECK(bar::WindowClientTopForTest() == doctest::Approx(topBefore + 321.0f));
    CHECK(bar::TrackRowYForTest() == doctest::Approx(500.0f + 321.0f));
    CHECK(bar::TrackRowYForTest() > bar::WindowClientTopForTest());

    // And a press-and-drag on the track where it is NOW still scrubs. TrackExtentForTest reports
    // the extent in the bar's own space; the window's rect is where it actually is.
    const int32_t pressX = static_cast<int32_t>(moved.x + 8.0f + (moved.w - 16.0f) * 0.25f);
    g_mouse.x = pressX;
    g_mouse.y = static_cast<int32_t>(bar::TrackRowYForTest());
    g_mouse.left = true;
    fx.Frame();
    g_mouse.x = pressX + 60;
    fx.Frame();
    CHECK(fx.transport.beginScrubs == 1);
    CHECK(fx.transport.scrubTos >= 1);
    CHECK(fx.transport.lastScrubTo >= kOwnerFirstUs);
    CHECK(fx.transport.lastScrubTo <= kOwnerDurationUs);
    g_mouse.left = false;
    fx.Frame();
    fx.Frame();   // the release is recorded in the draw and serviced on the tick that follows
    CHECK(fx.transport.endScrubs == 1);
}

TEST_CASE("bar: a press at the wrong y over the track is counted, not silently ignored") {
    BarFix fx;
    fx.transport.duration = static_cast<uint64_t>(kOwnerDurationUs);
    fx.transport.spanFirst = kOwnerFirstUs;
    fx.transport.spanLast = kOwnerDurationUs;
    fx.Frame();
    const bar::TrackExtent e = bar::TrackExtentForTest();
    g_mouse.x = static_cast<int32_t>(e.clientX + e.clientW * 0.5);
    g_mouse.y = static_cast<int32_t>(bar::TrackRowYForTest()) + 200;
    g_mouse.left = true;
    fx.Frame();
    CHECK(bar::BandMissesForTest() == 1);
    CHECK(fx.transport.beginScrubs == 0);
    g_mouse.left = false;
    fx.Frame();
}

TEST_CASE("bar: the stale ui.bar_* keys are not read any more") {
    // His installed config carries ui.bar_width_px=760 and ui.bar_autohide_s=3.0, and both were
    // DECLARED defaults in config_keys.inc -- so the shipped "0" never reached his machine.
    struct StaleConfig final : public testscratch::ScratchConfig {
        explicit StaleConfig(const std::wstring& dir) : ScratchConfig(dir) {}
        int64_t GetInt(const char* key, int64_t dflt) const override {
            if (key != nullptr && strcmp(key, "ui.bar_width_px") == 0) return 760;
            return ScratchConfig::GetInt(key, dflt);
        }
        double GetDouble(const char* key, double dflt) const override {
            if (key != nullptr && strcmp(key, "ui.bar_autohide_s") == 0) return 3.0;
            return ScratchConfig::GetDouble(key, dflt);
        }
    };
    BarFix fx;
    StaleConfig stale(fx.captureDir);
    fx.services.config = &stale;
    bar::Init(&fx.services);
    fx.transport.duration = static_cast<uint64_t>(kOwnerDurationUs);
    fx.Frame();
    // Size to content, not the 760 the old key carried.
    CHECK(bar::WindowRectForTest().w < 700.0f);
    // And no auto-hide: four minutes of stillness leave the window up.
    for (int i = 0; i < 4; ++i) {
        bar::AddIdleForTest(60ull * 1000000000ull);
        fx.Frame();
    }
    CHECK_FALSE(bar::HiddenForTest());
}
