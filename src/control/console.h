#pragma once

// W2-G. The console window: DESIGN 8.5.2's primary surface for the owner's requirement G12,
// "a console command to a file".
//
// * WHY A SEPARATE WINDOW AT ALL. R2 5.1 and R12 8.4 established independently that this binary has
// no in-game text console, no cvar system and no command registry. There is nothing to register a
// command with, so the surface has to be one we bring.
//
// * IT DOES NOT STEAL FOCUS, AND THE FIX IS NOT THE ONE THIS COMMENT USED TO CLAIM. AllocConsole
// shows AND ACTIVATES its window, which in a VR title means yanking the user out of the headset's
// mirror at launch. Creating it hidden is impossible -- there is no window to hide until
// AllocConsole has already made, shown and activated one -- and measurement confirmed it: with the
// SW_HIDE / SW_SHOWNOACTIVATE pair in place, GetForegroundWindow() was still the console a second
// later. So Start() records the foreground window BEFORE the allocation and puts it back after.
// The window it restores belongs to this same process, which is what makes SetForegroundWindow
// succeed here.
//
// * CLOSING IT CLOSES ECHO VR, AND THAT IS THE OWNER'S OWN RULING. This console is the game
// process's console, so the OS ends the process when its console goes. An audit called that a
// blocker and a guard was built and measured for it; the owner then looked at the behaviour and
// said he knows the window is attached and does not want it worked around. The measurements are
// kept in docs/spikes/W2-G.md so nobody has to re-derive them, and the mechanism is gone. The
// banner says so, and `console close` is the deliberate way to be rid of the window.
//
// * THE READER THREAD CAN BE STOPPED. A `std::getline` loop is the obvious shape and it cannot be
// shut down: the thread sits inside a blocking read forever, and on a FreeLibrary unload that is a
// thread parked in code that is about to be unmapped. Instead the thread waits on the console input
// handle AND a quit event, and only calls the blocking read once a complete line is already queued.
// So Stop() returns promptly and the DLL can be unloaded.
//
// * NOTHING HERE TOUCHES THE TRANSPORT, THE UI OR THE ENGINE. A line is parsed and handed to
// dispatch::Submit, which either answers it from the published snapshot or queues it for the tick.

#include <stddef.h>

namespace replay {
namespace control {
namespace console {

// auto | on | off, from `control.console`. `auto` is on everywhere except a sweep, where a console
// window would appear over a measured frame capture and could disturb the very thing being
// measured.
enum class Mode { Auto, On, Off };

Mode ModeFromString(const char* value);

// Decides whether the console should exist, given the config value and whether this is a sweep.
// Pure, so the table of cases is testable without creating a window.
bool ShouldStart(Mode mode, bool inSweep);

// Allocates the console (or attaches to an existing one) and starts the reader thread. False when
// it could not, with Diagnostic() saying why. Never fatal: the panel and the HTTP surface are
// unaffected by a console that did not come up.
bool Start();

// Signals the reader thread and waits briefly for it. Safe to call when Start failed or was never
// called.
//
// ★ AT PROCESS TERMINATION IT DELIBERATELY DOES NOTHING TO THE CONSOLE. Its only caller runs from
// DllMain under the loader lock with every other thread already killed, and a reader terminated
// inside a console API still holds that API's lock -- SetConsoleMode or FreeConsole from here
// would then hang the game's exit. Stop() detects that case (a signalled reader thread that never
// cleared its own running flag) and returns without touching the console.
void Stop();

// Writes one line to the console from any thread, if there is one. The tick uses it to say
// something the reader thread will not be alive to say -- `console close` stops that thread, so its
// own farewell could not come from there.
void Notice(const char* line);

bool Running();

// Never null. Says what happened, whether or not it worked.
const char* Diagnostic();

}  // namespace console
}  // namespace control
}  // namespace replay
