#include "ui/bar.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "bootstrap/livesafety.h"
#include "bootstrap/log.h"
#include "bootstrap/tickcost.h"
#include "bootstrap/offlinelock.h"
#include "capture/markers.h"
#include "playback/core/transport.h"
#include "ui/demoindex.h"
#include "ui/gui.h"
#include "ui/panel.h"
#include "ui/recorder.h"
#include "ui/timeline.h"
#include "ui/uiapi.h"

namespace replay {
namespace ui {
namespace bar {
namespace {

const char* const kModuleName = "ui";

// ---- geometry, in the facade's own units ------------------------------------------------------
// The window is a small player: name, markers, track, time, transport, speeds, and one line of
// state when there is state worth a line. The bar OWNS its position: it pins the window to
// (winX, winY) on every drawn frame and moves that point itself when the owner drags the title
// strip. Left to the facade, a press on any bare part of the body also dragged the window, and the
// bar -- which is told no window position -- kept mapping the cursor against where the window had
// been: the timeline drawn in one place and scrubbed from another (UI-DRAG-1). Title strip only is
// what a desktop window does, and the body is where the track and the buttons are.
//
// The facade lays out in client pixels: panel.h's kLinePitchPx / kGlyphAdvancePx are pixel
// measurements taken against this same NRadGui, and the bar's own placement has always been a
// client-pixel x fed straight to SetNextWindowPos. That is the whole of the gui->client map
// (kGuiToClient below), and a cell hit corrects its offset exactly whenever one lands.
constexpr float kRowPitch = panel::kLinePitchPx;
constexpr float kGlyphAdv = panel::kGlyphAdvancePx;
constexpr float kTitleBarPx = 22.0f;    // the strip the owner grabs to move the window
constexpr float kWindowPadPx = 8.0f;    // the facade's own content inset, corrected by a cell hit
constexpr float kTrackRowH = 26.0f;     // the tallest cell (kPlayheadH) plus its spacing
// Rows above the track, and rows below it, in row pitches. The track's client y is derived from
// these, which is what lets a press be recognised as a press ON THE TRACK with nothing calibrated.
constexpr float kRowsAboveTrack = 2.0f;   // the name row and the marker row
constexpr float kRowsBelowTrack = 3.0f;   // time, transport, speeds
constexpr float kContentW = 464.0f;       // the widest control row, measured from the widths below
constexpr float kContentInset = 16.0f;
constexpr float kEdgeGapPx = 32.0f;     // the viewport margin the bar never grows into
// With no capture open there is nothing to scrub, so the window collapses to the one thing that is
// worth screen space then: the way in.
constexpr float kMinimalW = 230.0f;
constexpr float kMinimalH = kTitleBarPx + 2.0f * kWindowPadPx + 2.0f * kRowPitch;
// BACKLOG-1. The recorder row -- one status line and one command button -- and the corner
// indicator. The row is drawn only in a session where recording is possible, so a window that does
// not carry it is exactly the window that shipped before.
constexpr float kRecorderRows = 2.0f;
constexpr float kMinimalRecW = 360.0f;   // the minimal shape with the status line in it
constexpr float kRecIndicatorW = 132.0f;
constexpr float kRecIndicatorH = kTitleBarPx + 2.0f * kWindowPadPx + kRowPitch;
constexpr float kMenuW = 260.0f;
constexpr size_t kMenuRateCols = 4;     // what fits across kMenuW without clipping the last one
constexpr float kMenuRows = 9.0f;       // width, open, unload, rates, rounds, bookmarks, auto-hide,
                                        // motion smoothing, smoothing strength
constexpr float kMenuH = kTitleBarPx + 2.0f * kWindowPadPx + kMenuRows * kRowPitch;
constexpr float kWidthStepPx = 60.0f;   // what one press of the menu's width control is worth
constexpr float kMinWidthPx = 280.0f;   // below this the transport row has nothing left to drop
// Rows the full window always draws, plus the state row it draws only when it has something to say.
constexpr float kBarH = kTitleBarPx + 2.0f * kWindowPadPx +
                        (kRowsAboveTrack + kRowsBelowTrack + 1.0f) * kRowPitch + kTrackRowH;

constexpr float kUnplayedH = 6.0f;
constexpr float kElapsedH = 10.0f;
constexpr float kMarkerH = 16.0f;
constexpr float kPlayheadH = 22.0f;

// Below this the control row starts dropping items, in the order S9 lists.
constexpr float kDropEndsBelow = 330.0f;
constexpr float kDropDirBelow = 250.0f;

constexpr int32_t kUnhideMotionPx = 2;
constexpr int32_t kDragThresholdPx = 3;
// Half the height of the client-y band a press must be in to count as a press on the track. Half
// the track row, plus the window padding the seeded map has not measured yet -- a press inside the
// row must arm the drag on a window nothing has been clicked on.
constexpr float kTrackBandPx = kTrackRowH * 0.5f + kWindowPadPx;
constexpr double kCalibMinSpanGui = 150.0;
constexpr double kCalibMinScale = 0.5;
constexpr double kCalibMaxScale = 4.0;
constexpr int32_t kCalibSameRowPx = 24;

constexpr uint64_t kNoticeNs = 6000000000ull;
constexpr uint64_t kMarkerLabelNs = 3000000000ull;

constexpr size_t kMaxTicks = 128;
constexpr size_t kMaxLabels = 12;

uint64_t NowNanos() {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) ::QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    ::QueryPerformanceCounter(&now);
    if (freq.QuadPart == 0) return 0;
    return static_cast<uint64_t>((now.QuadPart * 1000000000ll) / freq.QuadPart);
}

int64_t ClampI64(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
int32_t AbsI32(int32_t v) { return v < 0 ? -v : v; }
double AbsD(double v) { return v < 0.0 ? -v : v; }

// Every discrete command the bar issues. Draw records one; Sample runs it, because LoadPath opens
// files and EndScrub can decode three chapters, and neither may happen inside a draw.
enum class Act : uint8_t {
    TogglePlay,
    SetRate,
    Step,
    SeekUs,
    Unload,
    OpenBrowser,
    EndScrub,
};

struct Action {
    Act act;
    int64_t arg;
    float rate;
};

enum class Drag : uint8_t { Idle, Armed, Dragging };

struct Bar {
    ReplayServices* services = nullptr;
    bool initialised = false;

    // ---- config, latched at Init (IConfig is an immutable snapshot) ----
    bool enabled = true;
    bool debugPanel = false;
    bool anchorTop = false;
    float widthPx = 0.0f;   // 0 = size to content
    float configWidthPx = 0.0f;   // what `ui.player_width` asked for, so `reset` has somewhere to go
    float marginPx = 96.0f;
    double autohideS = 0.0;   // off unless ui.player_autohide_s asks for it
    bool keysOn = true;
    bool keysGlobal = false;
    bool dragOn = true;
    bool wantRounds = true;
    bool wantGoals = true;
    bool wantBookmarks = true;

    // ---- the per-tick snapshot ----
    bool haveTransport = false;
    bool loaded = false;
    TransportState state = TransportState::NotLoaded;
    EndState end = EndState::None;
    float rate = 1.0f;
    float ceiling = 0.0f;
    int64_t playheadUs = 0;
    int64_t durationUs = 0;
    // The playable span, on the playhead axis. `spanFirstUs` is 0 for a capture whose content
    // starts at its own origin, and every clamp, readout and cell in this file is against these two
    // and never against durationUs -- that is the whole of the owner's "shows way more than is
    // actually recorded".
    int64_t spanFirstUs = 0;
    int64_t spanLastUs = 0;
    bool spanKnown = false;
    bool spanLogged = false;
    bool preparing = false;
    bool preparePctKnown = false;
    int preparePct = 0;
    uint32_t loadSerial = 0;
    ScrubStats scrub;

    Marker ticks[kMaxTicks] = {};
    MarkerFamily tickFamily[kMaxTicks] = {};
    size_t tickCount = 0;
    size_t tickSourceCount = static_cast<size_t>(-1);   // so the first tick builds the table
    EntryVerdict lastVerdict = EntryVerdict::NotChecked;

    // ---- transient row-3 text ----
    char notice[96] = {};
    uint64_t noticeUntilNs = 0;

    // ---- visibility ----
    Show show = Show::Auto;
    bool liveCapture = false;
    const char* showReason = "visible";
    const char* loggedReason = nullptr;
    bool hintShown = false;
    bool minimal = false;
    float lastBarW = 760.0f;   // what the last draw actually used, so `diag` reports the real track
    bool hidden = false;
    bool menuOpen = false;
    bool showRemaining = false;
    uint64_t idleNs = 0;
    uint64_t lastFrameNs = 0;
    bool haveCursor = false;
    int32_t lastCursorX = 0;
    int32_t lastCursorY = 0;
    int32_t moveFromX = 0;      // where the window-move drag last saw the cursor
    int32_t moveFromY = 0;

    // ---- the window, placed once and moved by the owner ----
    bool placed = false;
    uint32_t placements = 0;
    float winX = 0.0f;
    float winY = 0.0f;
    float winW = kContentW + 2.0f * kWindowPadPx;
    float winH = kBarH;
    bool movingWindow = false;     // a press landed on the title strip and is dragging the frame

    // ---- the S5.2 map: clientX = a*guiX + b ----
    // `calibrated` no longer GATES the drag: the map is seeded from the placement the bar itself
    // made (the facade lays out in client pixels), so a first press-and-drag scrubs with no clicks
    // in front of it. A cell hit still solves the offset exactly and is what corrects the window
    // padding and any drift, and two hits far apart still correct the scale.
    bool calibrated = false;
    double a = 1.0;
    double b = kWindowPadPx;   // the facade's content inset, corrected by the first cell hit
    // ★ ONE AFFINE MAP FOR BOTH AXES: client = a*gui + (b, bY). `b` is solved from the cell's own
    // GUI x, `bY` from the track row's GUI y, and every window position the cursor is tested
    // against goes through the same two numbers -- so a stale `winX`/`winY` is absorbed by the
    // offsets instead of tilting the map, and the title strip and the track band can never end up
    // in different spaces. LAUNCH-UI4 §4.3/§4.4: they were, and one press both scrubbed and flung
    // the window 486 px.
    double bY = 0.0;
    bool mapSolved = false;    // a facade hit has tied the two spaces together
    uint32_t trackYResolves = 0;
    uint32_t bandMisses = 0;
    bool havePair = false;
    double pairGui = 0.0;
    double pairClient = 0.0;
    float pairClientY = 0.0f;
    bool pendPair = false;
    double pendGui = 0.0;
    double pendClient = 0.0;
    float pendClientY = 0.0f;
    // Was the physical button still down when the facade reported the hit? That single bit decides
    // whether a FRESH press-and-drag can scrub with nothing clicked in front of it: the hit is
    // ground truth that the cursor is on the track, so a hit reported while the button is down arms
    // the scrub outright and needs no map at all.
    bool pendDown = false;
    uint32_t hitsWhileDown = 0;
    uint32_t hitsAfterRelease = 0;
    uint32_t hitProbes = 0;
    uint32_t calibRejects = 0;
    uint32_t calibPairs = 0;
    uint32_t xHeals = 0;
    int32_t calClientW = 0;
    int32_t calClientH = 0;

    // ---- the drag ----
    Drag drag = Drag::Idle;
    bool scrubActive = false;
    bool lastLeft = false;
    int32_t pressX = 0;
    int32_t pressY = 0;
    int32_t lastX = 0;
    int64_t dragStartUs = 0;
    double dragAccumUs = 0.0;
    int64_t dragTargetUs = 0;
    double dragUsPerPx = 0.0;
    int fineDivisor = 1;
    uint32_t dragSteps = 0;
    uint32_t dragSwallowed = 0;
    bool swallowFrame = false;
    bool endRequested = false;
    int64_t endTargetUs = 0;
    const char* endReason = "released";
    // Held from the release until Sample has run EndScrub, so the drawn playhead does not flick
    // back to a pre-seek PlayheadUs for one frame at the end of every drag.
    bool holdDrawnTarget = false;

    // ---- click-to-seek, committed on the button's way UP ----
    // ★ WHICH EDGE NRadGui's Button FIRES ON IS STILL UNDERIVED (spike S-1a), so nothing here
    // depends on the answer: a cell hit is recorded, and committed only once the physical button is
    // observed up with no scrub having started in between. On press it waits for the release; on
    // release it commits on the next frame; and either way the press that begins a drag cannot
    // issue the cell-centre seek this design exists to avoid.
    bool clickPending = false;
    int64_t clickUs = 0;

    // ---- counters ----
    uint32_t swallowedTotal = 0;
    uint32_t items = 0;
    uint32_t glyphs = 0;
    uint32_t lastItems = 0;
    uint32_t lastGlyphs = 0;
    uint32_t drawnFrames = 0;
    uint64_t drawNanosMax = 0;
    bool drewThisFrame = false;

    Action pending[8] = {};
    size_t pendingCount = 0;

    CellClass cells[kMaxCells] = {};
    MarkerFamily cellFamily[kMaxCells] = {};

    char nameText[80] = {};
    char diag[384] = {};  // facts-ok: IDENT.USER_ACTOR_GUID_OFF a local buffer size, not the engine offset
    char hintText[24] = {};
    char timeText[64] = {};
    char rowText[96] = {};
    char speedText[16] = {};
};

Bar b;

// ---- the stream this frame handed the facade, counted where it is issued (panel.h budgets) -----
const char* Counted(const char* label) {
    ++b.items;
    uint32_t n = 0;
    while (label[n] != '\0' && !(label[n] == '#' && label[n + 1] == '#')) ++n;
    b.glyphs += n;
    return label;
}

void CText(const char* s) { gui::Text(Counted(s)); }

IReplayTransport* Transport() {
    return b.services != nullptr ? b.services->transport : nullptr;
}

void Post(Act act, int64_t arg = 0, float rate = 0.0f) {
    if (b.pendingCount >= sizeof b.pending / sizeof b.pending[0]) return;
    b.pending[b.pendingCount].act = act;
    b.pending[b.pendingCount].arg = arg;
    b.pending[b.pendingCount].rate = rate;
    ++b.pendingCount;
}

void Notice(const char* text) {
    snprintf(b.notice, sizeof b.notice, "%s", text);
    b.noticeUntilNs = NowNanos() + kNoticeNs;
}

bool NoticeLive() { return b.notice[0] != '\0' && NowNanos() < b.noticeUntilNs; }

// ---- the live Win32 reads ---------------------------------------------------------------------
//
// The game's window, found once by owning thread and process and re-found if it dies. The panel
// never needed a HWND -- it asks the facade for nothing but widgets -- but a cursor in client
// pixels does, and ScreenToClient is the only thing that turns GetCursorPos into one.

HWND g_gameWindow = nullptr;

// ★ THE RENDER WINDOW IS THE BIGGEST ONE, NOT THE FIRST ONE. LAUNCH-UI4 §4.5 measured the bar's
// cursor a constant 78 px above the truth on three independent samples while the facade's own
// widgets answered at the pixels they were drawn on -- one reader, one window, one disagreement.
// This process owns more than one top-level window (the runtime's mirror among them) and the old
// pick stopped at whichever came first in Z-order, so ScreenToClient could be subtracting a
// different window's client origin than the one the game draws into. Foreground first, because the
// window the cursor is being aimed at is the window the owner is looking at, then the largest
// client area, never the first.
long CandidateArea(HWND hwnd) {
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ::GetCurrentProcessId()) return 0;
    if (!::IsWindowVisible(hwnd)) return 0;
    if (::GetWindow(hwnd, GW_OWNER) != nullptr) return 0;
    RECT rc = {};
    if (!::GetClientRect(hwnd, &rc)) return 0;
    const long w = rc.right - rc.left;
    const long h = rc.bottom - rc.top;
    if (w < 320 || h < 200) return 0;  // facts-ok: PHYS.CI_COUNT_OFF a minimum window size in pixels, not an engine offset
    return w * h;
}

struct WindowPick {
    HWND best = nullptr;
    long area = 0;
};

BOOL CALLBACK PickWindow(HWND hwnd, LPARAM param) {
    const long area = CandidateArea(hwnd);
    if (area <= 0) return TRUE;
    WindowPick* pick = reinterpret_cast<WindowPick*>(param);
    if (area > pick->area) {
        pick->area = area;
        pick->best = hwnd;
    }
    return TRUE;
}

void LogGameWindow(HWND hwnd, const char* how) {
    RECT cl = {};
    RECT wr = {};
    POINT origin = {};
    ::GetClientRect(hwnd, &cl);
    ::GetWindowRect(hwnd, &wr);
    ::ClientToScreen(hwnd, &origin);
    log::Direct(LogLevel::Info, kModuleName,
                "bar: cursor window %s: client %ldx%ld, client origin on screen (%ld,%ld), window "
                "rect (%ld,%ld)-(%ld,%ld)",
                how, cl.right - cl.left, cl.bottom - cl.top, origin.x, origin.y, wr.left, wr.top,
                wr.right, wr.bottom);
}

HWND GameWindow() {
    const HWND fg = ::GetForegroundWindow();
    if (fg != nullptr && fg != g_gameWindow && CandidateArea(fg) > 0) {
        g_gameWindow = fg;
        LogGameWindow(fg, "is the foreground window");
        return g_gameWindow;
    }
    if (g_gameWindow != nullptr && ::IsWindow(g_gameWindow)) return g_gameWindow;
    WindowPick pick;
    ::EnumWindows(&PickWindow, reinterpret_cast<LPARAM>(&pick));
    g_gameWindow = pick.best;
    if (g_gameWindow != nullptr) LogGameWindow(g_gameWindow, "picked by largest client area");
    return g_gameWindow;
}

bool ForegroundIsOurs() {
    DWORD pid = 0;
    ::GetWindowThreadProcessId(::GetForegroundWindow(), &pid);
    return pid == ::GetCurrentProcessId();
}

void ReadMouseLive(MouseState* out) {
    const HWND hwnd = GameWindow();
    if (hwnd == nullptr) return;
    RECT rc = {};
    if (::GetClientRect(hwnd, &rc)) {
        out->haveRect = true;
        out->clientW = rc.right - rc.left;
        out->clientH = rc.bottom - rc.top;
    }
    POINT p = {};
    if (::GetCursorPos(&p) && ::ScreenToClient(hwnd, &p)) {
        out->havePos = true;
        out->x = p.x;
        out->y = p.y;
    }
    // ★ The cursor is read whatever has focus, because S4.1's unhide must work from raw motion and
    // needs no calibration and no focus. Everything that ACTS on the mouse is gated on foreground.
    out->foreground = ForegroundIsOurs();
    if (!out->foreground) return;
    out->left = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    out->shift = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    out->ctrl = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
}

// ★ THE LIVE-CAPTURE TEST IS THE OFFLINE LOCK, not a guess about what the owner is doing. The lock
// is applied on every launch EXCEPT an unambiguously live online match (bootstrap.cpp:
// IsLiveOnlineExempt), so "not locked" is exactly "this process may be recording other people's
// match", and a build where the lock refused for its own reasons falls to that same side. The bar
// is an overlay over somebody's recording there, so it stays off until it is asked for.
bool ReadLiveCaptureLive() {
    const offlinelock::LockState s = offlinelock::Current().state;
    return !(s == offlinelock::LockState::Locked ||
             s == offlinelock::LockState::AlreadyLockedInProcess);
}

uint32_t ReadKeysLive() {
    if (!ForegroundIsOurs()) return 0;
    struct Bind {
        int vk;
        uint32_t bit;
    };
    static const Bind kBinds[] = {
        {VK_SPACE, kKeySpace},       {VK_LEFT, kKeyLeft},        {VK_RIGHT, kKeyRight},
        {VK_HOME, kKeyHome},         {VK_END, kKeyEnd},          {VK_OEM_COMMA, kKeyComma},
        {VK_OEM_PERIOD, kKeyPeriod}, {'K', kKeyK},               {VK_SHIFT, kKeyShift},
        // F8 belongs to the GAME -- it hides nameplates -- so nothing here binds it.
        {VK_F9, kKeyBarToggle},      {VK_F10, kKeyCursorToggle},
    };
    uint32_t mask = 0;
    for (size_t i = 0; i < sizeof kBinds / sizeof kBinds[0]; ++i) {
        if ((::GetAsyncKeyState(kBinds[i].vk) & 0x8000) != 0) mask |= kBinds[i].bit;
    }
    return mask;
}

MouseSourceFn g_mouseSource = &ReadMouseLive;
// This frame's cursor, so the widgets drawn after RunMouse can report the pixel they were hit at
// without every draw helper growing a MouseState parameter.
MouseState g_frameMouse;
KeySourceFn g_keySource = &ReadKeysLive;
LiveCaptureFn g_liveCaptureSource = &ReadLiveCaptureLive;
uint32_t g_keysLast = 0;

// ---- the map ----------------------------------------------------------------------------------

int64_t SpanLen() {
    return b.spanLastUs > b.spanFirstUs ? b.spanLastUs - b.spanFirstUs : 0;
}

// Absolute playhead microseconds for a fraction of the drawn track, and back. Every seek this file
// issues goes through these two, so the track, the readout and the buttons cannot pick different
// origins -- and `seek <t>` on the console stays on the same absolute axis it always was.
int64_t SpanUsAt(double fraction) {
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    return b.spanFirstUs + static_cast<int64_t>(fraction * static_cast<double>(SpanLen()));
}

int64_t ClampToSpan(int64_t us) { return ClampI64(us, b.spanFirstUs, b.spanLastUs); }

// The width the owner chose, clamped to what the window can actually draw: narrow enough and the
// transport row drops its ends rather than overlapping, wide enough and it would run off the
// viewport he is looking at.
float BarWidth(int32_t clientW) {
    float w = b.widthPx > 0.0f ? b.widthPx : kContentW + 2.0f * kWindowPadPx;
    float maxW = panel::kBarMaxWidthPx;
    if (clientW > 0) {
        const float fit = static_cast<float>(clientW) - 2.0f * kEdgeGapPx;
        if (fit < maxW) maxW = fit;
    }
    if (maxW < kMinWidthPx) maxW = kMinWidthPx;
    if (w < kMinWidthPx) w = kMinWidthPx;
    if (w > maxW) w = maxW;
    return w;
}

// ★ THE PLACEMENT IS IN THE FACADE'S SPACE, NOT WINDOWS'. LAUNCH-BARFIX measured this window at
// x 102..858, y 300..385 on a 1536x874 client while "bottom, centred, 96 px margin" asks for
// x 388..1148, y 694..778 -- the right WIDTH (756 of 760, so the scale is 1) at the wrong ORIGIN.
// The GUI viewport is simply not the Win32 client rect, and a placement computed from
// GetClientRect puts the player over the middle of the arena. So the bar is placed the way the old
// panel is placed and for the same reason it has always landed correctly: a small constant in the
// facade's own coordinates, near the origin every stock window opens next to. He moves it from
// there, and where he moves it is where it stays.
constexpr float kFirstPlacementX = 40.0f;
constexpr float kFirstPlacementY = 40.0f;
// LAUNCH-UI3 measured gui(40,40) drawn at client(45,138) on a 1536x874 client: the facade's
// viewport sits 98 px below the client origin. The number is only used to keep the FIRST placement
// clear of the HUD -- the roster rails across the top, the scoreboard, and the minimap in the
// bottom-right corner -- so the lower LEFT is where the window opens. Wrong by 50 px it is still
// in an empty quadrant; the owner moves it wherever he likes from there.
constexpr float kGuiBelowClientPx = 98.0f;
constexpr float kBottomMarginPx = 24.0f;

float BarX(int32_t clientW, float barW) {
    (void)clientW;
    (void)barW;
    return kFirstPlacementX;
}

float BarY(int32_t clientH, float barH) {
    // `ui.bar_anchor` picks which end: the top is the offset above, the bottom is the lower-left
    // corner of whatever client the game is running at, so 993x519 and 5120x1440 both open clear.
    if (b.anchorTop || clientH <= 0) return kFirstPlacementY;
    const float y = static_cast<float>(clientH) - barH - kBottomMarginPx - kGuiBelowClientPx;
    return y > kFirstPlacementY ? y : kFirstPlacementY;
}

// The track row's centre in client pixels. It is knowable only from a hit: the facade reports no
// window position, and the placement above is in a space whose origin does not match the cursor's.
// So ONE click on the track arms every drag after it -- which is the honest version of the old
// rule, and a third of its cost.
constexpr float kTrackRowDy =
    kTitleBarPx + kWindowPadPx + kRowsAboveTrack * kRowPitch + kTrackRowH * 0.5f;

double ClientXOf(double guiX) { return b.a * guiX + b.b; }
double ClientYOf(double guiY) { return b.a * guiY + b.bY; }

float TrackRowClientY() {
    return static_cast<float>(ClientYOf(static_cast<double>(b.winY) + kTrackRowDy));
}

// A hit says where the cursor was when the facade reported a cell: the offset is whatever is left
// after the window's own y is taken out. Re-solved on EVERY hit, so a window move, a scale change
// or a wrong first guess heals on the next press without anybody being told to calibrate.
void SolveTrackY(float clientY, const char* how) {
    const double offset = static_cast<double>(clientY) - b.a * (static_cast<double>(b.winY) + kTrackRowDy);
    const bool moved = !b.mapSolved || AbsD(offset - b.bY) > kTrackBandPx;
    b.bY = offset;
    b.mapSolved = true;
    if (moved) {
        ++b.trackYResolves;
        log::Direct(LogLevel::Info, kModuleName,
                    "bar: track row solved from %s: client y=%.0f (window gui y %.0f, client offset "
                    "%+.0f); drag is live",
                    how, static_cast<double>(clientY), static_cast<double>(b.winY), offset);
    }
}

// A drag may not carry the title strip off the client, since the strip is the only way back: at
// least kStripKeptPx of it stays inside horizontally and all of it vertically.
constexpr double kStripKeptPx = 80.0;

void KeepTitleStripReachable() {
    const double scale = b.a > 0.0 ? b.a : 1.0;
    if (b.calClientW > 0) {
        const double maxX = (static_cast<double>(b.calClientW) - kStripKeptPx - b.b) / scale;
        if (static_cast<double>(b.winX) > maxX) b.winX = static_cast<float>(maxX);
    }
    if (b.calClientH > 0) {
        const double maxY =
            (static_cast<double>(b.calClientH) - scale * kTitleBarPx - b.bY) / scale;
        if (static_cast<double>(b.winY) > maxY) b.winY = static_cast<float>(maxY);
    }
    if (b.winX < 0.0f) b.winX = 0.0f;
    if (b.winY < 0.0f) b.winY = 0.0f;
}

// Where the window itself is, in the cursor's pixels. Every press test uses this and nothing else,
// so the strip the owner grabs and the band he scrubs on are answered from one map.
struct ClientRect {
    double left = 0.0, top = 0.0, right = 0.0, bottom = 0.0;
};

ClientRect WindowClientRect() {
    ClientRect r;
    r.left = ClientXOf(b.winX);
    r.right = ClientXOf(static_cast<double>(b.winX) + b.winW);
    r.top = ClientYOf(b.winY);
    r.bottom = ClientYOf(static_cast<double>(b.winY) + b.winH);
    return r;
}

// ★ ONE ANSWER FOR THE DRAWN TRACK, used by the draw AND by the hit and drag maps, because the two
// disagreeing is silent: the track stops growing at kMaxCells cells while a width that kept growing
// maps a drag to the wrong time with nothing on screen saying so. The cell WIDTH takes the slack
// instead of the cell count -- the count is the widget budget (panel.h kBarTrackCells) and the
// width is free -- so a wide bar draws a wide track at the same cost, and guiW is by construction
// what the cells actually cover.
TrackExtent Track(float barX, float barW) {
    TrackExtent e;
    e.guiX = barX;
    float w = barW - kContentInset;
    if (!(w > kBarCellPx)) w = kBarCellPx;
    e.cellPx = w / static_cast<float>(kMaxCells);
    if (e.cellPx < kBarCellPx) e.cellPx = kBarCellPx;
    e.cells = CellCount(w, e.cellPx);
    e.guiW = static_cast<float>(e.cells) * e.cellPx;
    e.clientX = ClientXOf(e.guiX);
    e.clientW = b.a * static_cast<double>(e.guiW);
    return e;
}

// The GUI-space x of cell `i`'s centre, measured from the window's origin. The window's own content
// padding is an unknown constant and does NOT have to be known: it is the same constant in the
// pairs the map is solved from and in the positions the map is applied to, so it lands in `b` and
// cancels.
double CellGuiX(const TrackExtent& e, uint32_t i) {
    return static_cast<double>(e.guiX) + (static_cast<double>(i) + 0.5) * e.cellPx;
}

// Back to the map the placement itself implies -- identity, because the facade lays out in client
// pixels -- rather than to no map at all. Dragging keeps working across this; only the correction a
// hit had made is dropped, because the thing it was measured against has moved.
void InvalidateCalibration(const char* why) {
    if (!b.calibrated && !b.havePair && !b.pendPair) return;
    b.calibrated = false;
    b.a = 1.0;
    b.b = kWindowPadPx;
    b.bY = 0.0;
    b.mapSolved = false;
    b.havePair = false;
    b.pendPair = false;
    log::Direct(LogLevel::Info, kModuleName, "bar: timeline offset correction dropped (%s)", why);
}

// ★ A HIT IS GROUND TRUTH ABOUT THE CURSOR, NOT ABOUT THE CELL. The 2026-09-19 17:07 session put
// the cursor on the drawn track at client (300,745) and the facade answered with cell 0, whose own
// GUI x is 42 -- so an offset solved from the reported index moved the map 258 px and broke the
// axis it was supposed to correct. What the hit DOES say without an index is that the cursor was
// on the track row when the facade routed the click, and that is exactly the y the band needs.
//
// The x map needs no correction: the facade lays out in the same client pixels the cursor arrives
// in, the bar placed the window itself, and the same session measured the track drawn at client
// x=49 against the seeded 48. It follows the window through the moves the bar tracks, and nothing
// else moves it.
void AcceptHit(double clientX, float clientY, const TrackExtent& e) {
    ++b.calibPairs;
    const bool first = !b.calibrated;
    SolveTrackY(clientY, "a cell hit");
    // The cursor WAS over the track when the facade routed this click, so a believed x range that
    // does not contain it is stale. The window is pinned, so this should not fire; if the facade
    // ever draws it somewhere else anyway, shift by exactly the shortfall and no more: the index
    // cannot be trusted, but "the cursor is inside the track" can.
    const double lo = e.clientX;
    const double hi = e.clientX + e.clientW;
    const double shift = clientX < lo ? clientX - lo : (clientX > hi ? clientX - hi : 0.0);
    if (shift != 0.0) {
        b.b += shift;
        ++b.xHeals;
        log::Direct(LogLevel::Info, kModuleName,
                    "bar: the track was %.0f px %s of where the cursor hit it; x map shifted to "
                    "client_x = %.3f*gui_x %+.1f",
                    AbsD(shift), shift < 0.0 ? "right" : "left", b.a, b.b);
    }
    b.calibrated = true;
    if (first) {
        log::Direct(LogLevel::Info, kModuleName,
                    "bar: track row tied to the cursor by a cell hit: client_x = %.3f*gui_x %+.1f "
                    "(from the placement), track row at y=%.0f; drag is live",
                    b.a, b.b, static_cast<double>(TrackRowClientY()));
    }
}

// The GUI y of a row's centre, in the same convention the track uses: measured from the window's
// own origin, with the facade's content padding left to be absorbed by the solved offset.
constexpr float kRowsTopDy = kTitleBarPx + kWindowPadPx;
float AboveRowGuiY(float row) { return b.winY + kRowsTopDy + (row + 0.5f) * kRowPitch; }
float BelowRowGuiY(float row) {
    return b.winY + kRowsTopDy + kRowsAboveTrack * kRowPitch + kTrackRowH + (row + 0.5f) * kRowPitch;
}

// ★ THE INSTRUMENT THAT SETTLES THE COORDINATE QUESTION. A facade-reported hit is one pixel the
// facade and the cursor agree on, so printing both sides of it says outright whether the map is
// right -- and whether the facade fires a Button on the press or on the release, which spike S-1a
// left underived and which decides whether a fresh press-and-drag can scrub with nothing clicked
// before it. Capped: this is a diagnosis, not a stream.
// The name row shares its line with the = button. LAUNCH-UI4 §8.1 measured the filename ending
// flush against that button with no gap at all; at a narrower width it would be drawn straight
// through it, so the name is cut to the columns the row actually has.
const char* FittedName(float contentW) {
    static char fitted[sizeof b.nameText];
    const float usable = contentW - 34.0f;
    size_t cols = usable > kGlyphAdv ? static_cast<size_t>(usable / kGlyphAdv) : 1u;
    if (cols >= sizeof fitted) cols = sizeof fitted - 1;
    const size_t n = strlen(b.nameText);
    if (n <= cols) return b.nameText;
    const size_t keep = cols > 3 ? cols - 3 : 0;
    memcpy(fitted, b.nameText, keep);
    memcpy(fitted + keep, "...", 3);
    fitted[keep + 3] = '\0';
    return fitted;
}

constexpr uint32_t kHitProbeCap = 40;

void NoteHit(const char* what, double guiX, double guiY, const MouseState& m) {
    if (m.left) {
        ++b.hitsWhileDown;
    } else {
        ++b.hitsAfterRelease;
    }
    if (b.hitProbes >= kHitProbeCap) return;
    ++b.hitProbes;
    log::Direct(LogLevel::Info, kModuleName,
                "bar hit: %s gui=(%.0f,%.0f) believed client=(%.0f,%.0f) cursor client=(%d,%d) "
                "button=%s",
                what, guiX, guiY, ClientXOf(guiX), ClientYOf(guiY), m.x, m.y,
                m.left ? "DOWN" : "up");
}

int64_t TimeAtClientX(double clientX, const TrackExtent& e) {
    if (!(e.clientW > 0.0) || SpanLen() <= 0) return b.spanFirstUs;
    return SpanUsAt((clientX - e.clientX) / e.clientW);
}

// ---- what the track draws ---------------------------------------------------------------------

int64_t DrawnPlayheadUs() {
    if (b.drag == Drag::Dragging || b.holdDrawnTarget) return b.dragTargetUs;
    return b.playheadUs;
}

void EndDrag(int64_t targetUs, const char* why) {
    if (b.drag == Drag::Idle && !b.scrubActive) return;
    b.drag = Drag::Idle;
    b.scrubActive = false;
    b.clickPending = false;
    if (!b.endRequested) {
        b.endRequested = true;
        b.endTargetUs = targetUs;
        b.endReason = why;
        b.dragTargetUs = targetUs;
        b.holdDrawnTarget = true;
    }
}

// ---- the tick snapshot ------------------------------------------------------------------------

// The loaded capture's file name, narrowed once per load rather than once per frame.
void RefreshName() {
    const wchar_t* path = LoadedPath();
    if (path == nullptr || path[0] == 0) {
        snprintf(b.nameText, sizeof b.nameText, "No capture loaded");
        return;
    }
    const wchar_t* leaf = path;
    for (const wchar_t* q = path; *q != 0; ++q) {
        if (*q == L'\\' || *q == L'/') leaf = q + 1;
    }
    char narrow[256] = {};
    if (::WideCharToMultiByte(CP_UTF8, 0, leaf, -1, narrow, sizeof narrow, nullptr, nullptr) == 0) {
        snprintf(b.nameText, sizeof b.nameText, "a capture is loaded");
        return;
    }
    snprintf(b.nameText, sizeof b.nameText, "%s", narrow);
}

void RefreshTicks() {
    b.tickCount = 0;
    const size_t rounds = RoundCount();
    const size_t marks = BookmarkCount();
    for (size_t i = 0; i < rounds && b.tickCount < kMaxTicks; ++i) {
        const uint16_t kind = RoundKindAt(i);
        const MarkerFamily fam = FamilyOfKind(kind);
        if (fam == MarkerFamily::Round && !b.wantRounds) continue;
        if (fam == MarkerFamily::Goal && !b.wantGoals) continue;
        if (fam == MarkerFamily::Bookmark && !b.wantBookmarks) continue;
        if (fam == MarkerFamily::None) continue;
        Marker& m = b.ticks[b.tickCount];
        m.tUs = RoundTimeUs(i);
        m.index = RoundNumberAt(i);
        m.kind = kind;
        b.tickFamily[b.tickCount] = fam;
        ++b.tickCount;
    }
    if (b.wantBookmarks) {
        for (size_t i = 0; i < marks && b.tickCount < kMaxTicks; ++i) {
            Marker& m = b.ticks[b.tickCount];
            m.tUs = BookmarkTimeUs(i);
            m.index = 0;
            m.kind = capture::kMarkBookmark;
            b.tickFamily[b.tickCount] = MarkerFamily::Bookmark;
            ++b.tickCount;
        }
    }
    b.tickSourceCount = rounds + marks;
}

void RunAction(const Action& a, IReplayTransport* t) {
    switch (a.act) {
        case Act::TogglePlay:
            if (b.state == TransportState::Play) {
                ui::Pause();
            } else {
                // Play at an end already seeks to the other one (playback_module's own path), so
                // the bar must not do it a second time.
                ui::Play();
            }
            return;
        case Act::SetRate: {
            const char* why = nullptr;
            if (!ui::SetRate(a.rate, &why)) Notice(why != nullptr ? why : "that speed is refused");
            return;
        }
        case Act::Step:
            ui::Step(static_cast<int>(a.arg));
            return;
        case Act::SeekUs:
            ui::SeekUs(a.arg);
            return;
        case Act::Unload:
            ui::Unload();
            return;
        case Act::OpenBrowser:
            ui::OpenBrowser();
            return;
        case Act::EndScrub:
            if (t != nullptr) t->EndScrub(a.arg);
            return;
    }
}

void ServiceEndScrub(IReplayTransport* t) {
    if (!b.endRequested) return;
    b.endRequested = false;
    if (t != nullptr) {
        t->EndScrub(b.endTargetUs);
        // Read back in the same breath: the snapshot above this call is pre-seek, and drawing it
        // would put one frame of the old playhead on the screen at the end of every drag.
        b.playheadUs = t->PlayheadUs();
        b.state = t->State();
    }
    log::Direct(LogLevel::Info, kModuleName,
                "[scrub] drag target=%lld steps=%u swallowed=%u s_per_px=%.4f fine=%d reason=%s",
                static_cast<long long>(b.endTargetUs), b.dragSteps, b.dragSwallowed,
                b.dragUsPerPx / 1e6, b.fineDivisor, b.endReason);
    b.dragSteps = 0;
    b.dragSwallowed = 0;
    b.holdDrawnTarget = false;
}

// Whether the bar would be on screen if it drew this instant, and why not when it would not. The
// two forced states outrank everything; `Auto` is off exactly where an overlay would be on top of
// somebody else's match.
bool ShowDecision(const char** why) {
    if (!b.enabled) {
        *why = "disabled";
        return false;
    }
    if (b.show == Show::Off) {
        *why = "bar off";
        return false;
    }
    if (b.show == Show::Auto && b.liveCapture) {
        *why = "live-capture";
        return false;
    }
    if (b.hidden) {
        *why = "auto-hidden";
        return false;
    }
    *why = "visible";
    return true;
}

void ApplyShow(Show s, const char* how) {
    b.show = s;
    // An explicit show also clears auto-hide, so `bar on` from a console is not a bar that is on
    // and invisible until the mouse moves.
    if (s != Show::Off) {
        b.hidden = false;
        b.idleNs = 0;
    }
    const char* why = nullptr;
    const bool shown = ShowDecision(&why);
    log::Direct(LogLevel::Info, kModuleName, "bar %s by %s (%s)", shown ? "shown" : "hidden", how,
                why);
}

// ★ F9 IS A REAL SHOW AND HIDE, which is what the owner asked for: "right now its less of a hide
// show and more of a show cursur or not show cursur". Shown means the window is on screen AND the
// engine's cursor is ours, so it is usable the instant it appears; hidden means nothing is drawn
// and the cursor is back with the spectator camera. The cursor itself is the panel's claim --
// ShowPanel is the one owner of it (panel.cpp's `cursorHeld`), and F10 moves that alone.
void ToggleShown(const char* how) {
    const char* why = nullptr;
    const bool wasShown = ShowDecision(&why);
    ApplyShow(wasShown ? Show::Off : Show::On, how);
    // ★ RECORDER-2 part A / L1. In a live session F9 puts the recorder surface on screen and
    // leaves the mouse with the spectator camera the owner is flying; F10 is the explicit claim,
    // and it still works because the window is up. Hiding always hands the cursor back.
    const bool claimCursor = !wasShown && livesafety::Allowed(livesafety::Feature::CursorClaim);
    ShowPanel(claimCursor);
    if (!wasShown && !claimCursor) {
        log::Direct(LogLevel::Info, kModuleName,
                    "%s showed the replay UI without claiming the cursor (%s) -- press F10 if "
                    "you want the mouse",
                    how, livesafety::VerdictName(livesafety::Current()));
    }
}

void ToggleCursor(const char* how) {
    const char* why = nullptr;
    if (!ShowDecision(&why)) {
        log::Direct(LogLevel::Info, kModuleName,
                    "cursor unchanged by %s: the replay UI is hidden (%s) -- F9 brings it back", how,
                    why);
        return;
    }
    const bool want = !PanelVisible();
    ShowPanel(want);
    log::Direct(LogLevel::Info, kModuleName, "cursor %s by %s; the replay window stays up",
                want ? "claimed" : "handed back to the game", how);
}

void HandleKeys() {
    const uint32_t keys = g_keySource();
    const uint32_t edges = keys & ~g_keysLast;
    g_keysLast = keys;
    if (edges == 0) return;

    // ★ THE ONE KEY THAT WORKS WHEREVER THE BAR IS. It is read above the unhide below and above
    // every gate: this is the key that has to answer when the owner wants the overlay off his live
    // capture, and a toggle that needed cursor mode or a loaded file would not be that key.
    if (edges & kKeyBarToggle) {
        ToggleShown("F9");
        return;
    }
    if (edges & kKeyCursorToggle) {
        ToggleCursor("F10");
        return;
    }

    // Any key at all brings the bar back -- before any gate, because S4.1's recovery must not
    // depend on a key being one this build acts on.
    b.hidden = false;
    b.idleNs = 0;

    const bool cursorMode = PanelVisible();
    const bool shift = (keys & kKeyShift) != 0;
    const int64_t nudge = shift ? 10000000 : 1000000;  // facts-ok: CLOCK.CTOR_MAXELAPSED_INIT 10,000,000 in tick arithmetic, not the clock constructor's initialiser
    if (b.keysOn && cursorMode) {
        if (edges & kKeySpace) Post(Act::TogglePlay);
        if (edges & kKeyLeft) Post(Act::SeekUs, ClampToSpan(b.playheadUs - nudge));
        if (edges & kKeyRight) Post(Act::SeekUs, ClampToSpan(b.playheadUs + nudge));
        if (edges & kKeyHome) Post(Act::SeekUs, b.spanFirstUs);
        if (edges & kKeyEnd) Post(Act::SeekUs, b.spanLastUs);
        if (edges & kKeyComma) Post(Act::Step, -1);
        if (edges & kKeyPeriod) Post(Act::Step, 1);
        return;
    }
    if (b.keysGlobal) {
        if (edges & kKeyK) Post(Act::TogglePlay);
        if (edges & kKeyComma) Post(Act::Step, -1);
        if (edges & kKeyPeriod) Post(Act::Step, 1);
    }
}

void BuildDiag() {
    const char* why = nullptr;
    const bool shown = ShowDecision(&why);
    const TrackExtent e = Track(0.0f, b.lastBarW);
    const ClientRect w = WindowClientRect();
    snprintf(b.diag, sizeof b.diag,
             "%s (%s), %s%s; track %u cells x %.2f = %.0f gui px / %.0f client px; drag %s (a=%.3f, "
             "%u pair(s), %u refused), swallowed %u, %u item(s)/%u glyph(s), draw %llu ns; window "
             "gui(%.0f,%.0f) %.0fx%.0f = client(%.0f,%.0f), track row y=%.0f, map %s, %u band "
             "miss(es), hits down/up %u/%u",
             shown ? "shown" : "hidden", why, b.liveCapture ? "live-capture" : "offline",
             b.hintShown ? ", cursor hint up" : "", e.cells, static_cast<double>(e.cellPx),
             static_cast<double>(e.guiW), e.clientW,
             !b.dragOn ? "off by ui.bar_drag" : (b.calibrated ? "hit-corrected" : "seeded"),
             b.a, b.calibPairs, b.calibRejects, b.swallowedTotal, b.lastItems, b.lastGlyphs,
             static_cast<unsigned long long>(b.drawNanosMax), static_cast<double>(b.winX),
             static_cast<double>(b.winY), static_cast<double>(b.winW), static_cast<double>(b.winH),
             w.left, w.top, static_cast<double>(TrackRowClientY()),
             b.mapSolved ? "solved" : "seeded", b.bandMisses, b.hitsWhileDown, b.hitsAfterRelease);
}

}  // namespace

// ---- the tick half ----------------------------------------------------------------------------

void Init(ReplayServices* services) {
    const bool wasDebugPanel = b.debugPanel;
    b = Bar();
    b.services = services;
    b.initialised = true;
    g_keysLast = 0;
    if (services != nullptr && services->config != nullptr) {
        const IConfig* c = services->config;
        b.enabled = c->GetBool("ui.bar", true);
        b.debugPanel = c->GetBool("ui.debug_panel", false);
        const char* anchor = c->GetString("ui.bar_anchor", "bottom");
        b.anchorTop = anchor != nullptr && strcmp(anchor, "top") == 0;
        // 0 means "size to the content", which is the default now: the fixed 760x84 did not fit
        // the rows the owner asked for, and a window he can resize needs a sane starting size.
        b.widthPx = static_cast<float>(c->GetInt("ui.player_width", 0));
        b.marginPx = static_cast<float>(c->GetInt("ui.bar_margin_px", 96));
        // OFF by default: F9 is the switch now, and a window that vanishes on its own is not one
        // the owner controls.
        b.autohideS = c->GetDouble("ui.player_autohide_s", 0.0);
        b.keysOn = c->GetBool("ui.bar_keys", true);
        b.keysGlobal = c->GetBool("ui.bar_keys_global", false);
        b.dragOn = c->GetBool("ui.bar_drag", true);
        const char* markers = c->GetString("ui.bar_markers", "rounds,goals,bookmarks");
        if (markers != nullptr) {
            b.wantRounds = strstr(markers, "rounds") != nullptr;
            b.wantGoals = strstr(markers, "goals") != nullptr;
            b.wantBookmarks = strstr(markers, "bookmarks") != nullptr;
        }
    } else {
        b.debugPanel = wasDebugPanel;
    }
    if (b.widthPx > 0.0f) b.widthPx = BarWidth(0);
    b.configWidthPx = b.widthPx;
    if (b.marginPx < 0.0f) b.marginPx = 0.0f;
    b.lastBarW = BarWidth(0);
    RefreshName();
    b.liveCapture = g_liveCaptureSource();
    b.lastFrameNs = NowNanos();
    BuildDiag();
    log::Direct(LogLevel::Info, kModuleName,
                "bar %s: %.0fx%.0f px, %s, %.0f px margin, drag %s, auto-hide %.1fs, debug panel "
                "%s; pinned, moved by its title strip only, width from the = menu (the facade has "
                "no resize grip)",
                b.enabled ? "enabled" : "DISABLED by ui.bar", BarWidth(0), kBarH,
                b.anchorTop ? "top" : "bottom", b.marginPx, b.dragOn ? "on" : "off", b.autohideS,
                b.debugPanel ? "on" : "off");
    const char* why = nullptr;
    const bool shown = ShowDecision(&why);
    b.loggedReason = why;
    log::Direct(LogLevel::Info, kModuleName,
                "bar %s at startup (%s); F9 shows or hides the replay UI, F10 the cursor alone, and "
                "`bar on|off|toggle` does the same from the console",
                shown ? "shown" : "hidden", why);
}

void SetDebugPanel(bool on) {
    if (b.debugPanel == on) return;
    b.debugPanel = on;
    log::Direct(LogLevel::Info, kModuleName, "debug panel %s", on ? "on" : "off");
}

bool DebugPanelOn() { return b.debugPanel; }

const char* DiagLine() {
    if (b.diag[0] == '\0') BuildDiag();
    return b.diag;
}

void Sample(uint64_t frameDeltaUs) {
    (void)frameDeltaUs;
    if (!b.initialised) return;

    IReplayTransport* t = Transport();
    b.haveTransport = t != nullptr;
    // Re-read every tick rather than latched at Init: the lock is applied early in the launch but
    // this module can initialise before it, and a session that turns out to be contained after all
    // must get its bar back without a restart.
    b.liveCapture = g_liveCaptureSource();

    const uint32_t serial = t != nullptr ? t->LoadSerial() : 0;
    if (serial != b.loadSerial) {
        b.loadSerial = serial;
        EndDrag(b.playheadUs, "the capture changed");
        InvalidateCalibration("a different capture was loaded");
        b.showRemaining = false;
        // ★ EVERY PER-FILE CACHE, not just the ones a drag touched. The marker table is rebuilt only
        // when `RoundCount() + BookmarkCount()` changes, and two Arena captures with three rounds
        // apiece do not change it -- so without this the timeline draws the PREVIOUS capture's round
        // times over the new one for the rest of the session.
        b.tickSourceCount = static_cast<size_t>(-1);
        b.tickCount = 0;
        b.durationUs = 0;
        b.playheadUs = 0;
        b.spanFirstUs = 0;
        b.spanLastUs = 0;
        b.spanKnown = false;
        b.spanLogged = false;
        b.notice[0] = '\0';
        b.noticeUntilNs = 0;
        RefreshName();
    }

    if (t != nullptr) {
        b.state = t->State();
        b.rate = t->Rate();
        b.playheadUs = t->PlayheadUs();
        b.durationUs = static_cast<int64_t>(t->DurationUs());
        // The timeline is the RECORDED span, never the file axis. A transport that cannot answer
        // (every double in the tree, and any capture with no core chapter) falls back to the whole
        // axis, which is what this file drew before the accessor existed.
        int64_t spanFirst = 0;
        int64_t spanLast = 0;
        b.spanKnown = t->PlayableSpanUs(&spanFirst, &spanLast) && spanLast > spanFirst;
        b.spanFirstUs = b.spanKnown ? spanFirst : 0;
        b.spanLastUs = b.spanKnown ? spanLast : b.durationUs;
        // Once per load, and only once the transport has a duration to compare against: the one
        // line that says whether the track is the content or the whole file.
        if (!b.spanLogged && b.loaded && b.durationUs > 0) {
            b.spanLogged = true;
            log::Direct(LogLevel::Info, kModuleName,
                        "bar span: file=[0..%lld] playable=[%lld..%lld] source=%s",
                        (long long)b.durationUs, (long long)b.spanFirstUs,
                        (long long)b.spanLastUs,
                        b.spanKnown ? "IReplayTransport::PlayableSpanUs"
                                    : "fallback: the whole file axis (the transport did not answer)");
        }
        b.end = t->Ended();
        b.scrub = t->Scrub();
        const PrepareStatus prep = t->Preparing();
        b.preparing = prep.preparing;
        b.preparePctKnown = prep.progress > 0.0;
        b.preparePct = static_cast<int>(prep.progress * 100.0 + 0.5);
        if (b.preparePct > 99) b.preparePct = 99;
        b.loaded = b.state != TransportState::NotLoaded;
    } else {
        b.loaded = false;
        b.state = TransportState::NotLoaded;
    }
    b.ceiling = RateCeiling();

    // A window build that refused did not move the playhead, so a drawn target would be a lie.
    if (b.scrub.lastRefused && (b.drag == Drag::Dragging || b.holdDrawnTarget)) {
        b.dragTargetUs = b.playheadUs;
        EndDrag(b.playheadUs, "the window build refused");
        Notice("that position is not decoded yet");
    }
    if (b.drag != Drag::Idle && !b.loaded) EndDrag(b.playheadUs, "nothing is loaded");
    if (b.drag == Drag::Dragging && b.end != EndState::None) {
        EndDrag(b.dragTargetUs, b.end == EndState::AtEnd ? "the end of the recording"
                                                         : "the start of the recording");
    }

    // ★ ONE PLACE ENDS A DRAG, AND IT IS BELOW EVERY WAY A DRAG CAN END. The release Draw recorded,
    // a refused window build, an unload, a focus loss and either end of the recording all arrive
    // here as the same request; EndScrub is a full seek and it is what gives the audio hold back,
    // so a `return` added above this line for any reason would be a session with dead audio and
    // nothing reporting it.
    ServiceEndScrub(t);

    // A load that was refused says so on the row for six seconds; the reason itself stays in the
    // log and in `info`, which is where S9 puts it.
    const EntryVerdict verdict = LastEntryVerdict();
    if (verdict != b.lastVerdict) {
        b.lastVerdict = verdict;
        if (verdict == EntryVerdict::Refused || verdict == EntryVerdict::RequestFailed) {
            Notice("could not open this file");
        }
    }

    HandleKeys();

    if (RoundCount() + BookmarkCount() != b.tickSourceCount) RefreshTicks();

    // The click a cell reported, serviced here because a seek is a discrete command and this is the
    // thread it belongs on. It waits for the physical button to be up: see Bar::clickPending.
    if (b.clickPending && !b.lastLeft) {
        b.clickPending = false;
        SeekUs(b.clickUs);
    }

    for (size_t i = 0; i < b.pendingCount; ++i) RunAction(b.pending[i], t);
    b.pendingCount = 0;

    BuildDiag();
}

void Shutdown() {
    ServiceEndScrub(Transport());
    b.initialised = false;
}

// ---- the draw half ----------------------------------------------------------------------------
namespace {

struct CellIdTable {
    char id[kMaxCells][12];
    CellIdTable() {
        for (size_t i = 0; i < kMaxCells; ++i) snprintf(id[i], sizeof id[i], "##b%zu", i);
    }
};

// ★ MF-3: WITHOUT THE ENGINE'S CURSOR THE WINDOW DRAWS PERFECTLY AND DOES NOTHING -- the facade
// has no mouse position at all then. F9 always brings up window AND cursor together; this line is
// for the state F10 puts the owner in deliberately, flying the camera with the player on screen.
void DrawHint(float atX) {
    snprintf(b.hintText, sizeof b.hintText, "%s: mouse", PanelHotkeyName());
    gui::SameLine(atX > 0.0f ? atX : 0.0f, 0.0f);
    CText(b.hintText);
    b.hintShown = true;
}

void DrawMarkerRow(const Span& span, float trackW) {
    MarkerLabel labels[kMaxLabels];
    size_t dropped = 0;
    const size_t n = b.tickCount == 0
                         ? 0
                         : LayoutMarkerLabels(span, trackW, 4.0f * kGlyphAdv, b.ticks, b.tickCount,
                                              labels, kMaxLabels, &dropped);
    CText(" ");
    for (size_t i = 0; i < n; ++i) {
        char buf[16];
        FormatRoundLabel(labels[i].kind, labels[i].index, 1, buf, sizeof buf);
        gui::SameLine(labels[i].x, 0.0f);
        CText(buf);
    }
    if (dropped > 0 && b.menuOpen) {
        char buf[32];
        snprintf(buf, sizeof buf, "  +%zu more", dropped);
        gui::SameLine(0.0f, 0.0f);
        CText(buf);
    }
}

// Returns the time a cell click asks for, or -1. Colour is pushed per RUN of equal-coloured cells
// rather than per cell: 230 push/pop pairs is three times the facade traffic of the track itself,
// and the colour of a track is made of four or five runs.
int64_t DrawTrack(const Span& span, const TrackExtent& e, const MouseState& m) {
    static const CellIdTable kIds;
    const uint32_t cells = e.cells;
    const int64_t drawn = DrawnPlayheadUs();
    ClassifyCellsElapsed(span, cells, b.ticks, b.tickCount, drawn, b.loaded && b.durationUs > 0,
                         b.cells);
    for (uint32_t i = 0; i < cells; ++i) b.cellFamily[i] = MarkerFamily::None;
    for (size_t k = 0; k < b.tickCount; ++k) {
        if (b.ticks[k].tUs < span.beginUs || b.ticks[k].tUs > span.endUs) continue;
        const uint32_t c = CellOfUs(span, cells, b.ticks[k].tUs);
        // Goal over round over bookmark, so a cell holding two ticks keeps the one the owner is
        // looking for rather than whichever came last in the table.
        if (b.cellFamily[c] == MarkerFamily::None || b.tickFamily[k] == MarkerFamily::Goal) {
            b.cellFamily[c] = b.tickFamily[k];
        }
    }

    int64_t seekTo = -1;
    bool pushed = false;
    Rgba current = {};
    const bool colour = gui::Have(gui::Entry::PushButtonDisabled) &&
                        gui::Have(gui::Entry::PopButtonDisabled);
    for (uint32_t i = 0; i < cells; ++i) {
        const CellClass cls = b.cells[i];
        gui::Vec2 size;
        size.x = e.cellPx;
        // ★ EVERY CELL IS FULL HEIGHT WHEN THE COLOURS ARE THERE, and that is not decoration. A
        // 6 px unplayed cell on a 22 px row leaves 16 px of bare window under the cursor, and the
        // facade moves a window from any press on its background: pressing the middle of the drawn
        // track both scrubbed AND dragged the window 250 px. Filling the row with
        // widgets is what makes a track press a track press -- and it makes the whole band
        // clickable, where before only the playhead and the nine marker cells could be hit.
        if (colour) {
            size.y = kPlayheadH;
        } else {
            switch (cls) {
                case CellClass::Playhead:
                case CellClass::MarkerPlayhead: size.y = kPlayheadH; break;
                case CellClass::Marker: size.y = kMarkerH; break;
                case CellClass::Elapsed: size.y = kElapsedH; break;
                default: size.y = kUnplayedH; break;
            }
        }
        if (colour) {
            const Rgba want = CellColour(cls, b.cellFamily[i]);
            if (!pushed || want.r != current.r || want.g != current.g || want.b != current.b ||
                want.a != current.a) {
                if (pushed) gui::PopButtonDisabled();
                gui::PushButtonDisabled(gui::Vec4{want.r, want.g, want.b, want.a});
                current = want;
                pushed = true;
            }
        }
        if (gui::Button(Counted(kIds.id[i]), size)) {
            if (b.scrubActive || b.swallowFrame) {
                ++b.swallowedTotal;
                ++b.dragSwallowed;
            } else {
                // ★ THE CLICK GOES WHERE THE CURSOR IS, NOT WHERE THE INDEX SAYS. The facade
                // answered cell 0 for a click at client x=300 in the 17:07 session, and cell 0 is
                // the start of the recording -- every click on the track would have jumped him to
                // 0:00. The cursor is the thing he aimed, and the x map is the placement's own.
                seekTo = m.havePos && m.foreground
                             ? ClampToSpan(TimeAtClientX(static_cast<double>(m.x), e))
                             : SeekTargetForCell(span, cells, i);
                if (m.havePos && m.foreground) {
                    b.pendPair = true;
                    b.pendGui = CellGuiX(e, i);
                    b.pendClient = static_cast<double>(m.x);
                    b.pendClientY = static_cast<float>(m.y);
                    b.pendDown = m.left;
                    NoteHit("track cell", b.pendGui,
                            static_cast<double>(b.winY) + kTrackRowDy, m);
                }
            }
        }
        if (i + 1 < cells) gui::SameLine(0.0f, 0.0f);
    }
    if (pushed) gui::PopButtonDisabled();
    return seekTo;
}

// ★ THE READOUT IS RELATIVE TO THE RECORDED SPAN, because that is the only axis the owner can act
// on: a capture whose content starts 13 minutes into its own time axis was reading "13:42 / 14:31"
// for its first frame. It reads 0:00.0 / 0:48.5 now, and the track under it covers the same span.
void FormatRow3Time() {
    char a[32], d[32];
    const int64_t len = SpanLen();
    const int64_t into = ClampToSpan(DrawnPlayheadUs()) - b.spanFirstUs;
    FormatTime(b.showRemaining && len > 0 ? into - len : into, a, sizeof a);
    if (len > 0) {
        FormatTime(len, d, sizeof d);
    } else {
        snprintf(d, sizeof d, "--:--.-");
    }
    snprintf(b.timeText, sizeof b.timeText, "%s / %s##bt", a, d);
}

// The one short sentence the window is allowed: what the tool is doing that the owner cannot see
// for himself. "not showing a million different things with large blocks of text" -- every module
// status line, every count and every diagnostic stays in the debug panel, behind `ui.debug_panel`.
bool DrawStateRow(float barW) {
    const char* text = nullptr;
    char buf[96];
    if (!b.haveTransport || !b.loaded) {
        text = "open a replay";
    } else if (b.preparing) {
        if (b.preparePctKnown) {
            snprintf(buf, sizeof buf, "preparing replay...  %d%%", b.preparePct);
            text = buf;
        } else {
            text = "preparing replay...";
        }
    } else if (NoticeLive()) {
        text = b.notice;
    } else if (b.end == EndState::AtEnd) {
        text = "end of recording";
    } else if (b.end == EndState::AtStart) {
        text = "start of recording";
    }
    if (text == nullptr) return false;
    CText(text);
    (void)barW;
    return true;
}

// One skip button. `us` is the offset it applies; the label is what the owner reads.
void SkipButton(float x, const char* label, int64_t us) {
    gui::SameLine(x, 0.0f);
    if (gui::Button(Counted(label), gui::Vec2{42.0f, 0.0f})) {
        Post(Act::SeekUs, ClampToSpan(b.playheadUs + us));
    }
}

// Row 4: play, direction, and the steps and skips, in the order a hand reaches for them. Items drop
// from the right as the window narrows, never from the middle, so the row never reflows under him.
void DrawTransportRow(float contentW) {
    const bool playing = b.state == TransportState::Play;
    if (gui::Button(Counted(playing ? "Pause##bplay" : "Play##bplay"), gui::Vec2{56.0f, 0.0f})) {
        Post(Act::TogglePlay);
        NoteHit("play button", static_cast<double>(b.winX) + 28.0, BelowRowGuiY(2.0f),
                g_frameMouse);
    }
    float x = 62.0f;
    if (contentW >= kDropDirBelow) {
        gui::SameLine(x, 0.0f);
        // A rate change, never a play: SetRate is the pause-preserving mutator P1 shipped, and
        // reversing while paused must leave the picture exactly where it is (R8).
        if (gui::Button(Counted(b.rate < 0.0f ? "Forward##bdir" : "Reverse##bdir"),
                        gui::Vec2{72.0f, 0.0f})) {
            const float r = b.rate != 0.0f ? -b.rate : -1.0f;
            Post(Act::SetRate, 0, r);
        }
        x += 78.0f;
    }
    if (contentW >= kDropEndsBelow) {
        SkipButton(x, "-10s##bm10", -10000000);  // facts-ok: CLOCK.CTOR_MAXELAPSED_INIT 10,000,000 in tick arithmetic, not the clock constructor's initialiser
        x += 46.0f;
        SkipButton(x, "-1s##bm1", -1000000);
        x += 46.0f;
    }
    gui::SameLine(x, 0.0f);
    if (gui::Button(Counted("-1f##bprev"), gui::Vec2{42.0f, 0.0f})) Post(Act::Step, -1);
    x += 46.0f;
    gui::SameLine(x, 0.0f);
    if (gui::Button(Counted("+1f##bnext"), gui::Vec2{42.0f, 0.0f})) Post(Act::Step, 1);
    x += 46.0f;
    if (contentW >= kDropEndsBelow) {
        SkipButton(x, "+1s##bp1", 1000000);
        x += 46.0f;
        SkipButton(x, "+10s##bp10", 10000000);  // facts-ok: CLOCK.CTOR_MAXELAPSED_INIT 10,000,000 in tick arithmetic, not the clock constructor's initialiser
    }
}

// BACKLOG-1. The recorder row of the F9 surface: what the recorder is doing, and the one button
// that changes it.
//
// ★ THE BUTTON DOES NOT CALL THE RECORDER. It records the press; the next Sample issues
// `record start` or `record stop` through the command layer, which is where the session gate and
// the refusal text live. That is the same Sample/Draw rule every other control on this window
// obeys, and it is why there is no Act for it: the recorder's own view model holds the press.
void DrawRecorderRow(float contentW) {
    static char fitted[192];
    const char* status = recorder::SurfaceStatus();
    size_t cols = contentW > kGlyphAdv ? static_cast<size_t>(contentW / kGlyphAdv) : 1u;
    if (cols >= sizeof fitted) cols = sizeof fitted - 1;
    if (strlen(status) > cols) {
        const size_t keep = cols > 3 ? cols - 3 : 0;
        memcpy(fitted, status, keep);
        memcpy(fitted + keep, "...", 3);
        fitted[keep + 3] = '\0';
        status = fitted;
    }
    CText(status);
    char label[24];
    snprintf(label, sizeof label, "%s##brecord", recorder::CommandLabel());
    const bool available = recorder::CommandAvailable();
    // Greyed exactly as the browser greys a capture this session cannot load: a control with
    // nowhere to send its command must look like one.
    if (!available) gui::PushButtonDisabled(gui::Vec4{0.45f, 0.45f, 0.45f, 1.0f});
    const bool pressed = gui::Button(Counted(label), gui::Vec2{76.0f, 0.0f});
    if (!available) gui::PopButtonDisabled();
    if (pressed && available) recorder::PressCommand();
}

// ★ THE REC INDICATOR IS NOT THE BAR. It is drawn while the player is dismissed and while
// `ui.bar` is off, because it answers a different question -- is this process recording -- and the
// owner turns it off with `capture.rec_indicator` and nothing else. One window, one line, pinned
// every frame to the top-right corner: nothing maps a cursor through it, it claims no cursor and
// it takes no hit, so it cannot cost the spectator camera he is flying a single click.
bool DrawRecIndicator(const MouseState& m) {
    if (!recorder::IndicatorOn()) return false;
    const float x = m.haveRect && static_cast<float>(m.clientW) > kRecIndicatorW + 2.0f * kEdgeGapPx
                        ? static_cast<float>(m.clientW) - kRecIndicatorW - kEdgeGapPx
                        : kEdgeGapPx;
    gui::SetNextWindowPos(gui::Vec2{x, kEdgeGapPx}, 0);
    gui::SetNextWindowSize(gui::Vec2{kRecIndicatorW, kRecIndicatorH}, 0);
    bool open = true;
    const bool shown = gui::Begin("##recindicator", &open, 0);
    if (shown) CText(recorder::IndicatorText());
    gui::End();
    return shown;
}

// Row 5: the speeds the transport will actually take, then the two file commands. A speed the
// engine would refuse is never offered -- timeline.h's CheckRate is the one policy.
void DrawSpeedRow(float contentW) {
    float rates[kOfferedRateCap];
    const size_t n = OfferedRates(b.ceiling, rates, kOfferedRateCap);
    const float mag = b.rate < 0.0f ? -b.rate : b.rate;
    float x = 0.0f;
    size_t drawn = 0;
    for (size_t i = 0; i < n && drawn < 5; ++i) {
        if (x + 48.0f > contentW - 160.0f) break;
        char label[24];
        // The running speed is marked rather than coloured: PushStyleColor's enum is underived
        // (gui.h), and a bracket costs nothing and reads at a glance.
        const bool now = mag > rates[i] - 0.01f && mag < rates[i] + 0.01f;
        snprintf(label, sizeof label, now ? "[%gx]##bsp%zu" : "%gx##bsp%zu",
                 static_cast<double>(rates[i]), i);
        if (drawn > 0) gui::SameLine(x, 0.0f);
        if (gui::Button(Counted(label), gui::Vec2{46.0f, 0.0f})) {
            Post(Act::SetRate, 0, b.rate < 0.0f ? -rates[i] : rates[i]);
        }
        x += 50.0f;
        ++drawn;
    }
    if (drawn == 0) CText(" ");
    gui::SameLine(contentW - 156.0f, 0.0f);
    if (gui::Button(Counted("Open...##bopen2"), gui::Vec2{76.0f, 0.0f})) Post(Act::OpenBrowser);
    gui::SameLine(contentW - 76.0f, 0.0f);
    if (gui::Button(Counted("Unload##bunload2"), gui::Vec2{72.0f, 0.0f})) Post(Act::Unload);
}

// Motion smoothing: one button cycles the mode, one the strength. "Off" is the 1:1 path at every rate.
// Read back from the transport every draw, so the labels can never disagree with what is running.
void DrawSmoothingRows() {
    static const char* const kModeLabel[3] = {"Off", "Slow-mo only", "Always"};
    static const char* const kStrengthLabel[3] = {"Low", "Med", "High"};
    const uint16_t packed = MotionSmoothing();
    const uint8_t mode = (uint8_t)(packed & 0xFF);
    const uint8_t strength = (uint8_t)(packed >> 8);
    const bool usable = !LiveCapture() && strength >= 1 && strength <= 3 && mode <= 2;
    char label[64];
    if (!usable) gui::PushButtonDisabled(gui::Vec4{0.45f, 0.45f, 0.45f, 1.0f});
    snprintf(label, sizeof label, "Motion smoothing: %s##bmsm", usable ? kModeLabel[mode] : "n/a");
    const bool cycleMode = gui::Button(Counted(label), gui::Vec2{236.0f, 0.0f});
    snprintf(label, sizeof label, "Smoothing strength: %s##bmss",
             usable ? kStrengthLabel[strength - 1] : "n/a");
    const bool cycleStrength = gui::Button(Counted(label), gui::Vec2{236.0f, 0.0f});
    if (!usable) gui::PopButtonDisabled();
    if (!usable) return;
    if (cycleMode) SetMotionSmoothing((uint8_t)((mode + 1) % 3), strength);
    if (cycleStrength) SetMotionSmoothing(mode, (uint8_t)(strength % 3 + 1));
}

void DrawMenu(float barX, float barY, float barH, int32_t clientH) {
    // The menu is a transient popup and is re-pinned every frame ON PURPOSE: it belongs to the
    // window it hangs off, and nothing maps a cursor through it.
    //
    // ★ IT OPENS ABOVE THE WINDOW UNLESS THERE IS NO ROOM ABOVE. LAUNCH-UI4 §6 found it drawn over
    // the orange roster rail with its last row -- the width control the owner asked for -- below a
    // fold it had no way to scroll: this facade has no scrolling child window, so a menu taller
    // than its rows is a control that does not exist.
    //
    // ★★ BELOW MEANS BELOW THE WHOLE WINDOW, and the flip is decided against the client area the
    // bar already measures, not against gui 0.  LAUNCH-INTEGRATED-1 §4.1: with the window carried
    // to gui y=191 the `=` press was accepted twice and no popup was drawn in eight frames.  The
    // old flip landed the menu at `barY + kTitleBarPx`, i.e. inside the player window's own body,
    // where it is behind it -- and `menuY < 0` is not where the client area starts anyway.  The
    // bottom is clamped too: a menu that fits nowhere is pulled inside rather than drawn off.
    const float guiBottom = clientH > 0 ? float(clientH) + kGuiBelowClientPx : 0.0f;
    float menuY = barY - kMenuH - 4.0f;
    if (menuY < 0.0f) menuY = barY + barH + 4.0f;
    if (guiBottom > kMenuH && menuY + kMenuH > guiBottom) menuY = guiBottom - kMenuH;
    if (menuY < 0.0f) menuY = 0.0f;
    gui::SetNextWindowPos(gui::Vec2{barX, menuY}, 0);
    gui::SetNextWindowSize(gui::Vec2{kMenuW, kMenuH}, 0);
    bool open = true;
    const bool shown = gui::Begin("##replaybarmenu", &open, 0);
    if (shown) {
        // The width row is FIRST because it is the one he went looking for: "the currend window
        // sizing for the ui doesnt fit all the components and I cant resize it". Twenty facade
        // entry points are bound and not one reads a window's size back (ui_facts.h), so a corner
        // grip is something we can neither honour nor observe; these three set it outright, the
        // whole window reflows, and the choice lasts the session.
        if (gui::Button(Counted("- narrower##bmnarrow"), gui::Vec2{76.0f, 0.0f})) {
            b.widthPx = BarWidth(b.calClientW) - kWidthStepPx;
        }
        gui::SameLine(80.0f, 0.0f);
        if (gui::Button(Counted("+ wider##bmwider"), gui::Vec2{76.0f, 0.0f})) {
            b.widthPx = BarWidth(b.calClientW) + kWidthStepPx;
        }
        gui::SameLine(160.0f, 0.0f);
        if (gui::Button(Counted("reset##bmwreset"), gui::Vec2{76.0f, 0.0f})) {
            b.widthPx = b.configWidthPx;
        }
        if (gui::Button(Counted("Open replay...##bmopen"), gui::Vec2{160.0f, 0.0f})) {
            Post(Act::OpenBrowser);
            b.menuOpen = false;
        }
        if (gui::Button(Counted("Unload##bmunload"), gui::Vec2{160.0f, 0.0f})) {
            Post(Act::Unload);
            b.menuOpen = false;
        }
        float rates[kOfferedRateCap];
        const size_t n = OfferedRates(b.ceiling, rates, kOfferedRateCap);
        // Four, not six: six 52 px buttons are 312 px of a 260 px menu and the sixth was drawn
        // half-off the right edge ("2.0|") in every frame of LAUNCH-UI4.
        for (size_t i = 0; i < n && i < kMenuRateCols; ++i) {
            char label[24];
            snprintf(label, sizeof label, "%.2fx##bmr%zu", static_cast<double>(rates[i]), i);
            if (i > 0) gui::SameLine(0.0f, 4.0f);
            if (gui::Button(Counted(label), gui::Vec2{48.0f, 0.0f})) {
                Post(Act::SetRate, 0, b.rate < 0.0f ? -rates[i] : rates[i]);
                b.menuOpen = false;
            }
        }
        // Checkbox is not a required entry point; these are Buttons so the menu cannot lose a row
        // to a facade that bound eighteen of twenty entries.
        char mk[40];
        snprintf(mk, sizeof mk, "[%c] rounds##bmrd", b.wantRounds ? 'x' : ' ');
        if (gui::Button(Counted(mk), gui::Vec2{110.0f, 0.0f})) {
            b.wantRounds = !b.wantRounds;
            RefreshTicks();
        }
        gui::SameLine(0.0f, 4.0f);
        snprintf(mk, sizeof mk, "[%c] goals##bmgl", b.wantGoals ? 'x' : ' ');
        if (gui::Button(Counted(mk), gui::Vec2{110.0f, 0.0f})) {
            b.wantGoals = !b.wantGoals;
            RefreshTicks();
        }
        snprintf(mk, sizeof mk, "[%c] bookmarks##bmbm", b.wantBookmarks ? 'x' : ' ');
        if (gui::Button(Counted(mk), gui::Vec2{110.0f, 0.0f})) {
            b.wantBookmarks = !b.wantBookmarks;
            RefreshTicks();
        }
        gui::SameLine(0.0f, 4.0f);
        snprintf(mk, sizeof mk, "[%c] as seen##bmas", AsSeen() ? 'x' : ' ');
        if (gui::Button(Counted(mk), gui::Vec2{110.0f, 0.0f})) SetAsSeen(!AsSeen());
        snprintf(mk, sizeof mk, "[%c] auto-hide##bmah", b.autohideS > 0.0 ? 'x' : ' ');
        if (gui::Button(Counted(mk), gui::Vec2{110.0f, 0.0f})) {
            b.autohideS = b.autohideS > 0.0 ? 0.0 : 3.0;
        }
        gui::SameLine(0.0f, 4.0f);
        snprintf(mk, sizeof mk, "[%c] details...##bmdbg", b.debugPanel ? 'x' : ' ');
        if (gui::Button(Counted(mk), gui::Vec2{110.0f, 0.0f})) SetDebugPanel(!b.debugPanel);
        DrawSmoothingRows();
    }
    gui::End();
    if (!open) b.menuOpen = false;
}

// The mouse half of one frame: unhide, calibration admission, and the drag machine. Everything it
// decides is expressed as UI-local state plus at most one ScrubTo; nothing here seeks.
void RunMouse(const MouseState& m, float barX, float barW) {
    const uint64_t now = NowNanos();
    const uint64_t dt = b.lastFrameNs != 0 && now > b.lastFrameNs ? now - b.lastFrameNs : 0;
    b.lastFrameNs = now;

    if (m.havePos && b.drewThisFrame) {
        // `diag`'s hovered counter had no production writer and read 0 while clicks worked. The bar
        // samples the same Win32 cursor it drags with, so the question is answerable here.
        const ClientRect w = WindowClientRect();
        inframe::NoteCursorOverPanel(
            static_cast<double>(m.x) >= w.left && static_cast<double>(m.x) <= w.right &&
            static_cast<double>(m.y) >= w.top && static_cast<double>(m.y) <= w.bottom);
    }
    if (m.havePos) {
        if (b.haveCursor && (AbsI32(m.x - b.lastCursorX) >= kUnhideMotionPx ||
                             AbsI32(m.y - b.lastCursorY) >= kUnhideMotionPx)) {
            b.hidden = false;
            b.idleNs = 0;
        }
        b.lastCursorX = m.x;
        b.lastCursorY = m.y;
        b.haveCursor = true;
    }

    if (m.haveRect && (m.clientW != b.calClientW || m.clientH != b.calClientH)) {
        const bool first = b.calClientW == 0 && b.calClientH == 0;
        b.calClientW = m.clientW;
        b.calClientH = m.clientH;
        if (!first) {
            EndDrag(b.dragTargetUs, "the window was resized");
            InvalidateCalibration("the client rect changed");
            // The position he chose can be off the screen the game just resized to, so the next
            // draw places the window again rather than leaving it somewhere he cannot reach.
            b.placed = false;
        }
    }

    // ★ NOTHING HAS TO BE CLICKED FIRST. The map is seeded by the placement the bar itself made,
    // so the FIRST press-and-drag on the track scrubs -- which is what the owner asked for and what
    // the old two-click gate refused him for a whole session.
    const bool canDrag = b.dragOn && b.loaded && !b.preparing && SpanLen() > 0 && PanelVisible();

    // The hit last frame's track reported. Its client x is the cursor AT THE HIT, so it is admitted
    // whatever the cursor has done since -- the old "was it still?" test was about a two-click
    // solve that no longer gates anything.
    if (b.pendPair) {
        AcceptHit(b.pendClient, b.pendClientY, Track(barX, barW));
        b.pendPair = false;
        if (b.pendDown) {
            // ★ THE FACADE SAID THIS PRESS IS ON THE TRACK. That is ground truth and it outranks
            // anything the map believed a frame ago, so a window move started on the same press is
            // cancelled here -- one press is a scrub or a move, never both (LAUNCH-UI4 §4.4).
            b.movingWindow = false;
            if (m.left && m.foreground && canDrag && b.drag == Drag::Idle && !b.scrubActive) {
                b.drag = Drag::Armed;
                b.pressX = static_cast<int32_t>(b.pendClient);
                b.pressY = static_cast<int32_t>(b.pendClientY);
            }
        }
        b.pendDown = false;
    }

    // Below the admission above, never beside it: a pair accepted this frame moves the map, and an
    // extent taken before it would map this frame's press with the previous solve.
    const TrackExtent e = Track(barX, barW);

    const bool press = m.left && !b.lastLeft && m.foreground;
    const bool release = !m.left && b.lastLeft;
    b.lastLeft = m.left && m.foreground;
    if (m.left && m.foreground) {
        b.hidden = false;
        b.idleNs = 0;
    }

    // ★ THE BAR MOVES THE WINDOW, from the same mouse and the same map the drag machine uses, and
    // the draw pins the facade to the result -- so the window drawn and the window the cursor is
    // tested against are one position on every frame. The delta arrives in client pixels and the
    // window lives in the facade's, hence the division by the scale.
    if (b.movingWindow) {
        if (!m.left || !m.foreground || !m.havePos) {
            b.movingWindow = false;
        } else {
            const double scale = b.a > 0.0 ? b.a : 1.0;
            b.winX += static_cast<float>(static_cast<double>(m.x - b.moveFromX) / scale);
            b.winY += static_cast<float>(static_cast<double>(m.y - b.moveFromY) / scale);
            b.moveFromX = m.x;
            b.moveFromY = m.y;
            KeepTitleStripReachable();
        }
    } else if (press && m.havePos && b.placed && b.drag == Drag::Idle) {
        const ClientRect w = WindowClientRect();
        const double titleBottom = ClientYOf(static_cast<double>(b.winY) + kTitleBarPx);
        const bool onStrip = static_cast<double>(m.x) >= w.left &&
                             static_cast<double>(m.x) <= w.right &&
                             static_cast<double>(m.y) >= w.top - 2.0 &&
                             static_cast<double>(m.y) <= titleBottom;
        const bool onTrack =
            AbsD(static_cast<double>(m.y) - TrackRowClientY()) <= kTrackBandPx;
        if (onStrip && !onTrack) {
            b.movingWindow = true;
            b.moveFromX = m.x;
            b.moveFromY = m.y;
        }
    }

    if (b.drag == Drag::Dragging && !m.foreground) {
        EndDrag(b.dragTargetUs, "the game lost focus");
    } else if (b.drag != Drag::Idle && release) {
        if (b.drag == Drag::Dragging) {
            b.swallowFrame = true;
            EndDrag(b.dragTargetUs, "released");
        } else {
            // ★ A PRESS AND RELEASE INSIDE THE BAND IS A CLICK-TO-SEEK, and the bar commits it
            // itself rather than waiting for a cell hit. Only the playhead and the marker cells are
            // tall enough to be hit at the row's centre -- an unplayed cell is 6 px of a 22 px row
            // -- so most of the drawn track reported nothing at all when it was clicked. The band
            // is 42 px and the cursor's x is the same x the drag would have used.
            b.drag = Drag::Idle;
            if (canDrag && m.havePos) {
                b.swallowFrame = true;     // so a cell hit for this release cannot seek twice
                b.clickPending = false;
                Post(Act::SeekUs, ClampToSpan(TimeAtClientX(b.pressX, e)));
            }
        }
    } else if (b.drag == Drag::Armed && m.left && m.havePos) {
        if (AbsI32(m.x - b.pressX) >= kDragThresholdPx) {
            IReplayTransport* t = Transport();
            if (t != nullptr && canDrag) {
                t->BeginScrub();
                b.scrubActive = true;
                b.drag = Drag::Dragging;
                b.clickPending = false;
                b.dragStartUs = TimeAtClientX(b.pressX, e);
                b.dragAccumUs = 0.0;
                b.dragTargetUs = b.dragStartUs;
                b.dragSteps = 0;
                b.dragSwallowed = 0;
                b.lastX = b.pressX;
            } else {
                b.drag = Drag::Idle;
            }
        }
    // ★ NOT GATED ON `calibrated`. The map is seeded by the placement the bar itself made and the
    // band follows the window, so the FIRST press-and-drag scrubs; a cell hit only corrects the
    // offset. The gate that was here is the one the owner's session log shows him losing every
    // drag to, since a track click is exactly what he was trying to make.
    } else if (b.drag == Drag::Idle && press && canDrag && m.havePos && !b.movingWindow) {
        const bool inX = static_cast<double>(m.x) >= e.clientX - 2.0 &&
                         static_cast<double>(m.x) <= e.clientX + e.clientW + 2.0;
        const bool inY = AbsD(static_cast<double>(m.y) - TrackRowClientY()) <= kTrackBandPx;
        if (inX && inY) {
            b.drag = Drag::Armed;
            b.pressX = m.x;
            b.pressY = m.y;
        } else if (inX && !b.menuOpen) {
            // The press was over the track's x range at the wrong y. If the facade reports a cell
            // hit for this same press the map re-solves from it and the next press drags; either
            // way the owner gets a line saying which y we believed. Not while the menu is open:
            // it is drawn over the bar and owns those pixels, and every press on it would
            // otherwise report a track the owner was not aiming at.
            ++b.bandMisses;
            log::Direct(LogLevel::Info, kModuleName,
                        "bar: a press at client y=%d missed the track band at y=%.0f by %.0f px "
                        "(map %s, window gui y %.0f, client offset %+.0f); a cell hit re-solves it",
                        m.y, static_cast<double>(TrackRowClientY()),
                        AbsD(static_cast<double>(m.y) - TrackRowClientY()),
                        b.mapSolved ? "solved" : "not solved yet", static_cast<double>(b.winY),
                        b.bY);
        }
    }

    if (b.drag == Drag::Dragging && m.havePos) {
        b.dragUsPerPx = e.clientW > 0.0 ? static_cast<double>(SpanLen()) / e.clientW : 0.0;
        b.fineDivisor = m.shift ? (m.ctrl ? 100 : 10) : 1;
        b.dragAccumUs += static_cast<double>(m.x - b.lastX) * b.dragUsPerPx /
                         static_cast<double>(b.fineDivisor);
        b.lastX = m.x;
        const int64_t target =
            ClampToSpan(b.dragStartUs + static_cast<int64_t>(b.dragAccumUs));
        if (target != b.dragTargetUs) {
            b.dragTargetUs = target;
            IReplayTransport* t = Transport();
            if (t != nullptr) t->ScrubTo(target);
            ++b.dragSteps;
        }
    }

    // Auto-hide. Forced off until the map is solved (F2): a bar that hides itself before dragging
    // can even be armed has taken the owner's only widget away for nothing.
    //
    // ★ WITH NOTHING LOADED THE BAR IS NOT PINNED, and that is the whole of MF-1's second half. It
    // used to be: `pinned` carried `!b.loaded` and the hide clock needed a calibration that cannot
    // happen with no track on screen, so an idle session kept an undismissable overlay up forever.
    // A paused or preparing CAPTURE is still pinned -- that is S4.1's table.
    const bool pinned = b.preparing || NoticeLive() || b.menuOpen || b.drag != Drag::Idle ||
                        (b.loaded && b.state != TransportState::Play);
    const double hideAfter = b.autohideS;
    if (pinned || !(hideAfter > 0.0)) {
        b.idleNs = 0;
        b.hidden = false;
    } else {
        b.idleNs += dt;
        if (static_cast<double>(b.idleNs) > hideAfter * 1e9) b.hidden = true;
    }
}

}  // namespace

inframe::DrawOutcome Draw() {
    // A cost root of its own: the engine calls this from the GUI frame, not from the playback tick,
    // so it is not inside that tick's total (PERF-1).
    const tickcost::Scope cost_root(tickcost::kRootUi);
    if (!b.initialised) return inframe::DrawOutcome::NotShowing;
    if (!gui::Ready() || !gui::WindowGateOpen()) return inframe::DrawOutcome::NotShowing;

    const uint64_t startNanos = NowNanos();
    b.items = 0;
    b.glyphs = 0;

    // ★ S5.5/F10: the two Win32 reads happen HERE, at the top of the bar's own draw, so the drawn
    // playhead is this frame's hand and not last phase's. Neither takes a lock nor opens a file.
    // The indicator needs the client rect too, so they are read above the bar's own switch.
    MouseState m;
    g_mouseSource(&m);
    g_frameMouse = m;

    // Drawn whatever the bar is doing, and its own key decides it. A recording with no indicator is
    // the failure this overlay exists to prevent, and `ui.bar off` is not a statement about that.
    const bool indicator = DrawRecIndicator(m);
    if (!b.enabled) {
        b.lastItems = b.items;
        b.lastGlyphs = b.glyphs;
        if (indicator) inframe::NoteImguiNanos(NowNanos() - startNanos);
        return indicator ? inframe::DrawOutcome::Drawn : inframe::DrawOutcome::NotShowing;
    }

    // ★ THE PANEL USED TO BE THE ONLY THING THAT SAID THIS, AND IT IS OPT-IN NOW. The flag is what
    // makes "a file was opened on the thread the game renders on" countable; with the debug panel
    // off, nothing else on this thread sets it and the guard would silently read zero forever.
    fileio::NoteDrawThread();

    // With no capture open the bar is one row and one button wide: the timeline, the controls and
    // the marker row all have nothing to say, and the owner needs the way in, not the frame.
    b.minimal = !b.haveTransport || (!b.loaded && !b.preparing && !NoticeLive());
    // The recorder row is two rows of content, and in the minimal shape it is also what decides the
    // window's width: a status line in a 230 px window is a sentence nobody can read.
    const bool recorderRow = recorder::SurfaceWanted();
    const float recorderH = recorderRow ? kRecorderRows * kRowPitch : 0.0f;
    const float barH = (b.minimal ? kMinimalH : kBarH) + recorderH;
    const float barW = b.minimal ? (recorderRow ? kMinimalRecW : kMinimalW)
                                 : BarWidth(m.haveRect ? m.clientW : 0);
    // Placed from the config on the first draw, then wherever the owner drags the title strip. A
    // size change (a capture opening or closing, the = menu) re-sizes it and leaves that alone.
    const bool sizeChanged = barW != b.winW || barH != b.winH;
    if (!b.placed) {
        b.winX = BarX(m.haveRect ? m.clientW : 0, barW);
        b.winY = BarY(m.haveRect ? m.clientH : 0, barH);
    }
    b.winW = barW;
    b.winH = barH;
    const float barX = b.winX;
    const float barY = b.winY;
    const bool wasSwallowFrame = b.swallowFrame;
    b.lastBarW = barW;
    RunMouse(m, barX, barW);

    b.drewThisFrame = false;
    b.hintShown = false;
    const char* why = nullptr;
    const bool show = ShowDecision(&why);
    b.showReason = why;
    if (why != b.loggedReason) {
        b.loggedReason = why;
        log::Direct(LogLevel::Info, kModuleName, "bar %s (%s)", show ? "shown" : "hidden", why);
    }
    if (!show) {
        if (wasSwallowFrame) b.swallowFrame = false;
        b.lastItems = b.items;
        b.lastGlyphs = b.glyphs;
        // A dismissed player over a live capture is the frame the indicator exists for, so a frame
        // that drew one is a DRAWN frame: csregister scores this answer.
        if (indicator) inframe::NoteImguiNanos(NowNanos() - startNanos);
        return indicator ? inframe::DrawOutcome::Drawn : inframe::DrawOutcome::NotShowing;
    }

    // Pinned on every drawn frame: the facade would otherwise move it from a press on the body,
    // and nothing tells the bar where it went.
    gui::SetNextWindowPos(gui::Vec2{b.winX, b.winY}, 0);
    if (!b.placed || sizeChanged) {
        gui::SetNextWindowSize(gui::Vec2{barW, barH}, 0);
        b.placed = true;
        ++b.placements;
    }
    bool open = true;
    // A real title, because the strip that carries it is the handle the owner drags the window by
    // and an empty one is a handle he cannot see.
    const bool shown = gui::Begin("Echo Replay##replaybar", &open, 0);
    if (shown) {
        b.drewThisFrame = true;
        const float contentW = barW - kContentInset;
        // Row 1 is the name row in both shapes, and it carries the menu button at its right edge.
        CText(FittedName(contentW));
        gui::SameLine(contentW - 24.0f, 0.0f);
        if (gui::Button(Counted("=##bmenu"), gui::Vec2{24.0f, 0.0f})) {
            b.menuOpen = !b.menuOpen;
            NoteHit("= menu button", static_cast<double>(barX) + contentW - 12.0,
                    AboveRowGuiY(0.0f), g_frameMouse);
        }
        if (b.minimal) {
            if (gui::Button(Counted("Open...##bopen"), gui::Vec2{140.0f, 0.0f})) {
                Post(Act::OpenBrowser);
            }
            if (!PanelVisible()) DrawHint(150.0f);
        } else {
            const TrackExtent e = Track(barX, barW);
            // ★ THE TRACK IS THE RECORDED SPAN AND NOTHING ELSE. Markers, cells, the readout and
            // every seek this window issues are against these two microseconds.
            const Span span = {b.spanFirstUs, b.spanLastUs};
            // Each row is started by its first widget rather than by a NewLine: that is the panel's
            // own idiom, and it is what keeps the track's first cell at the content origin.
            DrawMarkerRow(span, e.guiW);
            int64_t seekTo = -1;
            if (b.loaded && SpanLen() > 0) {
                seekTo = DrawTrack(span, e, m);
            } else {
                CText(" ");
            }
            if (seekTo >= 0) {
                b.clickPending = true;
                b.clickUs = seekTo;
            }
            FormatRow3Time();
            if (gui::Button(Counted(b.timeText), gui::Vec2{150.0f, 0.0f})) {
                b.showRemaining = !b.showRemaining;
            }
            if (!DrawStateRow(contentW)) CText(" ");
            if (!PanelVisible()) DrawHint(contentW - 11.0f * kGlyphAdv);
            DrawTransportRow(contentW);
            DrawSpeedRow(contentW);
        }
        // Last in both shapes: the recorder is what the window is for in a live session, and it is
        // the one row that is there whether or not a capture is loaded.
        if (recorderRow) DrawRecorderRow(contentW);
    }
    gui::End();
    (void)open;
    if (shown && b.menuOpen) DrawMenu(barX, barY, barH, m.haveRect ? m.clientH : 0);

    b.lastItems = b.items;
    b.lastGlyphs = b.glyphs;
    const uint64_t spent = NowNanos() - startNanos;
    if (spent > b.drawNanosMax) b.drawNanosMax = spent;
    // The in-frame site's own cost number is a MAX over the draws it timed, and with the debug
    // panel off the bar is the only thing drawing: leaving it out would report 0 ns for a frame
    // that drew 250 widgets (AC-2.12 measures both configurations from this).
    inframe::NoteImguiNanos(spent);
    if (shown) ++b.drawnFrames;
    // The swallow window is the whole frame the button went up in, so it is cleared only after the
    // track has had its chance to report the release's hit.
    if (wasSwallowFrame) b.swallowFrame = false;
    if (shown) return inframe::DrawOutcome::Drawn;
    return indicator ? inframe::DrawOutcome::Drawn : inframe::DrawOutcome::FacadeRefused;
}

// ---- test seams --------------------------------------------------------------------------------

void SetMouseSourceForTest(MouseSourceFn fn) { g_mouseSource = fn != nullptr ? fn : &ReadMouseLive; }

void SetKeySourceForTest(KeySourceFn fn) {
    g_keySource = fn != nullptr ? fn : &ReadKeysLive;
    g_keysLast = 0;
}

void SetLiveCaptureSourceForTest(LiveCaptureFn fn) {
    g_liveCaptureSource = fn != nullptr ? fn : &ReadLiveCaptureLive;
    b.liveCapture = g_liveCaptureSource();
}

bool LiveCapture() { return b.liveCapture; }

TrackExtent TrackExtentForTest() {
    const float barW = BarWidth(b.calClientW);
    return Track(b.placed ? b.winX : BarX(b.calClientW, barW), barW);
}

void SetShow(Show s) { ApplyShow(s, "a command"); }
void ToggleShow() { ToggleShown("a command"); }
Show CurrentShow() { return b.show; }

bool Shown() {
    const char* why = nullptr;
    return ShowDecision(&why);
}

const char* ShowReason() {
    const char* why = nullptr;
    ShowDecision(&why);
    return why;
}

uint32_t LastItems() { return b.lastItems; }
uint32_t LastGlyphs() { return b.lastGlyphs; }
bool VisibleForTest() { return b.drewThisFrame; }
bool HiddenForTest() { return b.hidden; }
bool HintShownForTest() { return b.hintShown; }
bool MinimalForTest() { return b.minimal; }
bool CalibratedForTest() { return b.calibrated; }
PlayableSpan SpanForTest() {
    PlayableSpan s;
    s.firstUs = b.spanFirstUs;
    s.lastUs = b.spanLastUs;
    return s;
}
WindowRect WindowRectForTest() {
    WindowRect r;
    r.x = b.winX;
    r.y = b.winY;
    r.w = b.winW;
    r.h = b.winH;
    return r;
}
uint32_t PlacementsForTest() { return b.placements; }
float TrackRowYForTest() { return TrackRowClientY(); }
uint32_t BandMissesForTest() { return b.bandMisses; }
bool MapSolvedForTest() { return b.mapSolved; }
float WindowClientTopForTest() { return static_cast<float>(WindowClientRect().top); }
uint32_t HitsWhileDownForTest() { return b.hitsWhileDown; }
uint32_t HitsAfterReleaseForTest() { return b.hitsAfterRelease; }
float BarWidthForTest() { return BarWidth(b.calClientW); }
bool MovingWindowForTest() { return b.movingWindow; }
double CalibScaleForTest() { return b.a; }
uint32_t CalibRejectsForTest() { return b.calibRejects; }
uint32_t SwallowedForTest() { return b.swallowedTotal; }
int64_t DrawnPlayheadUsForTest() { return DrawnPlayheadUs(); }

void AddIdleForTest(uint64_t ns) { b.idleNs += ns; }

void ResetForTest() {
    b = Bar();
    g_frameMouse = MouseState();
    g_keysLast = 0;
    g_mouseSource = &ReadMouseLive;
    g_keySource = &ReadKeysLive;
    g_liveCaptureSource = &ReadLiveCaptureLive;
    g_gameWindow = nullptr;
}

}  // namespace bar
}  // namespace ui
}  // namespace replay
