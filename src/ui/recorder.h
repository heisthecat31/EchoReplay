#pragma once

// BACKLOG-1. The REC indicator and the recorder surface's state.
//
// ---------------------------------------------------------------------------------------------
// * THE UI NEVER TOUCHES THE RECORDER. Everything shown here is one Read of
// capture::status::ICaptureStatus, taken on the tick, and the two things the owner can ask for --
// Record and Stop -- are issued as the console's own `record start` / `record stop` through the
// command layer's sink (uiapi.h). That is where the session gate, the "already recording" refusal
// and the status line he pastes already live, and a second caller of the recorder would be a
// second copy of all three.
//
// * THIS FILE MAKES NO FACADE CALL. bar.cpp owns every widget the UI issues, and its item and
// glyph counters are what panel.h's budgets are asserted against; a second file drawing into the
// same frame would make those numbers undercount. So this is the view model -- text in fixed
// buffers, filled on the tick -- and bar.cpp draws it.
//
// * NOTHING HERE ALLOCATES, on the tick or on the frame. Every string is a fixed buffer and every
// draw-side accessor is a pointer read.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include "bootstrap/services.h"

namespace replay {
namespace ui {
namespace recorder {

void Init(ReplayServices* services);
void Shutdown();

// Tick thread: one Read of the provider into our buffers, then the frame's pending press.
void Sample();

// ---- what bar.cpp draws -----------------------------------------------------------------------

// The corner overlay is on when `capture.rec_indicator` is on, a provider has registered, and that
// provider says a capture is recording. No provider is NOT "not recording": with nothing to ask,
// nothing is drawn and nothing is claimed.
bool IndicatorOn();
// "REC 01:23". Never null; empty while the indicator is off.
const char* IndicatorText();

// Does the F9 surface carry a recorder row at all? True while recording, and true wherever the
// RECORDER says this session can record -- its own gate, carried in the reading. FALSE in an
// offline playback session, where capture refuses to open a file at all and a Record button would
// be an offer nothing will honour.
bool SurfaceWanted();
bool Recording();
// One line for the surface: what is recording, for how long, into what -- or why not. Never null.
const char* SurfaceStatus();
// The label the surface's one command button carries: "Record" or "Stop". Never null.
const char* CommandLabel();
// False when no control surface has registered a command sink, in which case the button is drawn
// disabled rather than pressed into nothing.
bool CommandAvailable();

// Draw thread: records the press. The command is issued by the next Sample, because a command runs
// on the tick and a draw is not one (bar.h's Sample/Draw rule).
void PressCommand();

// ---- the elapsed format, which is the one rule worth stating -----------------------------------
//
// `mm:ss` up to 59:59, then `h:mm:ss` -- 3600 s is "1:00:00", not "60:00". A capture that ran for
// an hour and eight minutes reading `68:23` is a number the owner has to do arithmetic on, and the
// minute field silently means two different things either side of the hour.
void FormatElapsed(uint64_t elapsedUs, char* out, size_t cap);

// ---- test seams ---------------------------------------------------------------------------------
uint32_t CommandsIssuedForTest();
const wchar_t* LastCommandForTest();
void ResetForTest();

}  // namespace recorder
}  // namespace ui
}  // namespace replay
