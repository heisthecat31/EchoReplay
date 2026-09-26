#pragma once

// W2-F. A stand-in for src/ui/gui.cpp, so tests/ui can drive panel.cpp on a machine with no game.
//
// ★ WHAT THIS DOES AND DOES NOT PROVE. The real gui.cpp resolves twenty entry points out of the
// mapped echovr.exe and guards each one; nothing here touches the binary, and check.ps1's second
// step already re-reads every guard byte back out of the shipped exe. What was missing was a way to
// exercise the PANEL -- the load funnel, the session-entry decision, the browser's three states,
// the toggle and the cursor claim -- none of which is binary-dependent and all of which shipped
// with no test at all. This fake covers exactly that half.
//
// It implements gui.h verbatim, so a change to that header breaks this file rather than silently
// diverging from the facade the panel actually calls.

#include <stdint.h>

#include <string>
#include <vector>

#include "audio/voip/voipplay.h"
#include "ui/gui.h"

namespace uifake {

// Everything the panel drew this frame, and everything it asked of the facade.
void Reset();

// ---- WHAT THIS FRAME HANDED THE ENGINE, COUNTED EXACTLY ---------------------------------------
//
// The fake facade cannot tell you what NRadGui's ImGui charges for a widget -- no ImGui is linked
// into this tree, and the panel reaches the game's own copy through a table of resolved function
// pointers. What it CAN do is count the two quantities that copy's cost is made of, exactly and
// without a scheduler in the way:
//
//   Items   -- one per widget the panel issued. Each is an ItemAdd, an ID hash and a visibility
//              test in the real one, whether or not the widget ends up on screen.
//   Glyphs  -- the characters the panel asked to have MEASURED. ImGui runs CalcTextSize over the
//              whole label before it clips it, so a character past the right edge and a character
//              in the middle of the window cost the same. Everything from "##" onward is an ID and
//              is not measured, which is why the timeline's 230 cells are counted as 0 glyphs.
//
// Both reset with Reset(), so a test that calls Reset() once a frame reads them per frame.
//   Calls   -- every entry point the panel went through, layout included. SameLine and NewLine are
//              cursor moves, not widgets: they run no ItemAdd, hash no ID and measure no string,
//              so counting them as Items made the two quantities the engine charges for
//              unreadable. They are still calls through a resolved function pointer, and this is
//              the counter that says so.
uint64_t ItemsDrawn();
uint64_t GlyphsDrawn();
uint64_t FacadeCallsDrawn();

void SetReady(bool ready);            // Ready() and Bound()
void SetWindowGate(bool open);

// ★ THE FRAME BYTE, and it is the difference between a fixture and a decoration (AUDIT_F3J F3J-5).
// The real NRadGui::Begin opens with `cmp cs:byte_1420DAF2F, 0 / jz` and produces no state at all
// when the engine's frame is shut. This fake returned true unconditionally, so mutating it to false
// reddened 19 cases and not one of them was a draw-counter case: no test in this directory could
// tell a draw from a refusal, which is precisely the defect ARCH-15 exists to fix.
//
// Bind the fake to the SAME byte the row-2.5 precondition reads (inframe::SetProbeForTest) and the
// two cannot disagree about whether the frame was open. Unbound, the fake behaves as before, which
// is what keeps the cases that are about something else unaffected.
void SetFrameByte(const uint8_t* frameOpenByte);
uint32_t BeginRefusals();             // Begin calls that returned false because the frame was shut
void SetHave(replay::ui::gui::Entry e, bool have);   // and the BoundCount() it implies

// The next Button call whose label contains `needle` returns true, once. Labels carry the facade's
// "##id" suffix, so a substring match is how a test names a control.
void ClickOnce(const char* needle);

const std::vector<std::string>& Texts();
const std::vector<std::string>& ButtonLabels();
bool TextContains(const char* needle);
bool ButtonDrawn(const char* needle);
// Buttons drawn between PushButtonDisabled and PopButtonDisabled -- the browser's greyed rows.
bool ButtonDisabled(const char* needle);

// The VOIP seam, recorded in fake_deps.cpp. The panel is the only place that knows a capture was
// opened and where its path is, so this is where the player finds out a file exists.
const wchar_t* VoipDemoPath();
uint32_t VoipPathCalls();
uint32_t VoipPathClears();
void ResetVoip();
replay::audio::voip::VoipPlayStats& VoipStatsForTest();   // what the voice row will read

// playback::OwnerStateGametypeOfFile's stand-in: what the capture's own owner-state event "says",
// and how many captures were opened to ask.
void SetOwnerStateGametype(uint64_t gametype);
uint32_t OwnerStateReads();

uint32_t CursorCalls();
// Placement: SetNextWindowPos calls this frame and the last position asked for.
uint32_t WindowPosCalls();
uint32_t WindowSizeCalls();
replay::ui::gui::Vec2 LastWindowPos();
replay::ui::gui::Vec2 LastWindowSize();
int LastCursorArg();     // -1 when SetCursorEnabled has never been called

// Where the facade itself holds each window, across frames: a SetNextWindowPos lands at the next
// Begin, and DragWindowBody is the facade dragging a window from a press on its bare body without
// telling anyone -- the engine behaviour UI-DRAG-1 is about.
void DragWindowBody(const char* name, float dx, float dy);
replay::ui::gui::Vec2 WindowAt(const char* name);
bool WindowOpened();     // Begin() returned true at least once since Reset()
uint32_t BeginCount();
uint32_t EndCount();

// ---- the ARCH-20 gate setters -----------------------------------------------------------------
// The ORDER, as one string, because "clear, draw, restore" is a sequence and three independent
// counters cannot say a restore came after the draw rather than before it. 'C' = ClearSuppressed,
// 'D' = the draw, 'S' = SetSuppressed.
const std::string& GateEvents();
void NoteGateEvent(char c);
// The a1 ClearSuppressed was handed, which ARCH-20 s1.2.2 requires to be the OBSERVED hotkey byte.
// -1 when it has never been called.
int LastHotkeyArg();
// Unbind either half, so "an unmapped setter refuses by name" is driven rather than described.
void SetSuppressSettersBound(bool clearBound, bool setBound);
void ResetGate();

}  // namespace uifake
