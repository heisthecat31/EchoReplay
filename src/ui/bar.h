#pragma once

// UI-2 P2. The player bar: one compact overlay with a timeline, a playhead and the six controls the
// owner actually uses. It is the DEFAULT visible surface; the old panel (panel.cpp) is demoted to an
// opt-in debug window behind `ui.debug_panel`.
//
// ---------------------------------------------------------------------------------------------
// * THE SPLIT IS THE PANEL'S SPLIT, WITH ONE NAMED EXCEPTION. Sample() runs on the stage-0 tick and
// is where every transport read and every transport COMMAND happens. Draw() renders what Sample
// left. The exception DESIGN-UI-2 S5.5 requires: the Win32 cursor and button are read at the top of
// Draw, because a mouse position sampled a phase earlier is a playhead one frame behind the hand,
// and "0 frames" is the whole of "smooth dragging". Those two syscalls take no lock and open no
// file, which is the property the Sample/Draw rule exists to protect -- RequestScan and LoadPath
// still stay on the tick.
//
// * THE ONLY OTHER THING DRAW TOUCHES ON THE TRANSPORT IS THE COALESCING SLOT: BeginScrub and
// ScrubTo, which overwrite one slot and return. Every discrete command -- play, pause, rate, step,
// seek, open, unload, and the release EndScrub, which is a full seek and can decode -- is posted as
// an action and serviced by the next Sample.
//
// * KEYS ARE READ IN SAMPLE, the mouse in Draw, and that asymmetry is deliberate (S4.4): a key edge
// must be seen even in the frames the bar is hidden and Draw returns early, and it is the only way
// F9 can bring back a window that is drawing nothing.
//
// * F8 IS THE GAME'S OWN KEY (it hides nameplates) and this plugin no longer touches it. F9 shows
// or hides the whole replay UI -- window AND cursor -- and F10 hands the cursor back to the
// spectator camera while the window stays on screen.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

#include "bootstrap/services.h"
#include "ui/inframe.h"

namespace replay {
namespace ui {
namespace bar {

void Init(ReplayServices* services);

// Tick thread, before the frame the bar draws into: snapshot, key edges, deferred actions.
void Sample(uint64_t frameDeltaUs);

// In-frame, after the panel. Reads the mouse, runs the drag machine, draws.
inframe::DrawOutcome Draw();

// Releases a scrub in flight (its EndScrub and its audio hold) and forgets the session's state.
void Shutdown();

// `ui.debug_panel` at runtime. `IConfig` is an immutable load-time snapshot, so the key's value is
// latched at Init and this is the only thing that can move it afterwards -- the console verb and
// the bar's own menu entry both come through here.
void SetDebugPanel(bool on);
bool DebugPanelOn();

// The runtime switch. `Auto` is the product's own decision -- off in a live online capture, on
// otherwise -- and the two forced states survive until something toggles them again, which is what
// makes `off` a dismissal rather than a hint the next mouse move undoes.
enum class Show : uint8_t { Auto, On, Off };
void SetShow(Show s);
void ToggleShow();
Show CurrentShow();

// Is the bar on screen right now, and why. The reason is one of a fixed set of words -- "visible",
// "live-capture", "bar off", "auto-hidden", "disabled" -- and never null.
bool Shown();
const char* ShowReason();

// One line for `diag`, never empty. The panel prints it as "bar : <this>"; AUDIT_C1 F7 forbids
// redefining the panel's own line, so the bar brought its own.
const char* DiagLine();

// ---- test seams -------------------------------------------------------------------------------
//
// The live reads are GetCursorPos / GetAsyncKeyState / GetClientRect, which no offline test can
// drive, and the wiring from a cursor pixel to a scrub target is the half worth testing. Null
// restores the live source.

struct MouseState {
    bool haveRect = false;      // GetClientRect answered
    int32_t clientW = 0;
    int32_t clientH = 0;
    bool havePos = false;       // GetCursorPos answered and mapped into the client
    int32_t x = 0;
    int32_t y = 0;
    bool foreground = false;    // our PROCESS owns the foreground window
    bool left = false;          // VK_LBUTTON
    bool shift = false;
    bool ctrl = false;
};

using MouseSourceFn = void (*)(MouseState*);
void SetMouseSourceForTest(MouseSourceFn fn);

// The shortcut set, as a bitmask of whatever is down this tick. 0 when the game is not foreground.
enum : uint32_t {
    kKeySpace = 1u << 0,
    kKeyLeft = 1u << 1,
    kKeyRight = 1u << 2,
    kKeyHome = 1u << 3,
    kKeyEnd = 1u << 4,
    kKeyComma = 1u << 5,
    kKeyPeriod = 1u << 6,
    kKeyK = 1u << 7,
    kKeyShift = 1u << 8,
    kKeyBarToggle = 1u << 9,    // F9: show or dismiss the whole replay UI, cursor included
    kKeyCursorToggle = 1u << 10,  // F10: the cursor alone, with the window left up
};

using KeySourceFn = uint32_t (*)();
void SetKeySourceForTest(KeySourceFn fn);

// Whether this process is recording a LIVE online match instead of replaying inside a contained
// session. The live read is the offline lock's own state, which no offline test can drive; null
// restores it.
using LiveCaptureFn = bool (*)();
void SetLiveCaptureSourceForTest(LiveCaptureFn fn);
bool LiveCapture();

// The playable span of the loaded capture: the first and last recorded sample, which is what the
// timeline draws and the only region a seek can land in. Zero-length when nothing is loaded.
struct PlayableSpan {
    int64_t firstUs = 0;
    int64_t lastUs = 0;
};
PlayableSpan SpanForTest();

// Where the window was last PLACED, and how many times the bar has placed it. The bar places it
// once per session (and again only when the client rect changes or the owner asks), which is what
// makes it draggable afterwards -- a window re-pinned every frame cannot be moved.
struct WindowRect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};
WindowRect WindowRectForTest();
uint32_t PlacementsForTest();
// The client y the drag's band is centred on. Derived from the window's own y through the same
// solved map the title strip is tested against, so a case can move the window and press where the
// track now is.
float TrackRowYForTest();
uint32_t BandMissesForTest();

// Has a facade-reported hit tied the facade's coordinates to the cursor's yet? Until it has, the
// bar will not route a press to the title strip: a window flung across the screen by a map that
// was still a guess is worse than a press that does nothing (LAUNCH-UI4 §4.4).
bool MapSolvedForTest();

// The window's own top edge in CURSOR pixels -- the strip a press has to land on to be a move.
float WindowClientTopForTest();

// How the facade's Button really behaves, counted from live hits: a hit reported with VK_LBUTTON
// still down is a press edge and arms a scrub outright; one reported with it up is a release edge
// and can only correct the map for the next press.
uint32_t HitsWhileDownForTest();
uint32_t HitsAfterReleaseForTest();

// Is a title-strip drag in progress? A press is a window move or a scrub, never both.
bool MovingWindowForTest();

// The width the `= menu`'s wider/narrower pair has settled on, clamped to what the viewport and
// the transport row can carry.
float BarWidthForTest();

// The track exactly as this frame drew it: one function answers for the draw and for the drag map,
// so the two cannot disagree. `clientX`/`clientW` are meaningful only once calibrated.
struct TrackExtent {
    float guiX = 0.0f;
    float guiW = 0.0f;
    float cellPx = 0.0f;
    uint32_t cells = 0;
    double clientX = 0.0;
    double clientW = 0.0;
};
TrackExtent TrackExtentForTest();

// What the last Draw handed the facade, against panel.h's kBarItemBudget / kBarGlyphBudget.
uint32_t LastItems();
uint32_t LastGlyphs();

// Whether the bar drew at all this frame, and whether it is hidden by auto-hide right now.
bool VisibleForTest();
bool HiddenForTest();

// Whether the last draw carried the "F10 gives you a mouse" hint, and whether it drew the minimal
// no-capture bar rather than the full window.
bool HintShownForTest();
bool MinimalForTest();

// The S5.2 map, so a test can assert the reject band rather than the happy path.
bool CalibratedForTest();
double CalibScaleForTest();
uint32_t CalibRejectsForTest();

// Track-cell hits eaten by the swallow rule (F3) since Init.
uint32_t SwallowedForTest();

// The UI-local playhead: what the track actually draws, which during a drag is the pending target
// and not a PlayheadUs() read-back.
int64_t DrawnPlayheadUsForTest();

// Adds to the auto-hide idle clock without waiting for it. The hide DECISION is still the product's
// -- the next Draw makes it, against the same pinned/calibrated rules -- so this shortens the wait
// and nothing else.
void AddIdleForTest(uint64_t ns);

void ResetForTest();

}  // namespace bar
}  // namespace ui
}  // namespace replay
