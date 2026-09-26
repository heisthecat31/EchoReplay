#include "control/console.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <atomic>

#include "bootstrap/log.h"
#include "control/command.h"
#include "control/dispatch.h"

namespace replay {
namespace control {
namespace console {
namespace {

const char* const kReplayModuleName = "control";

HANDLE g_thread = nullptr;
HANDLE g_quit = nullptr;
HANDLE g_in = nullptr;
HANDLE g_out = nullptr;
bool   g_allocated = false;
DWORD  g_savedInputMode = 0;
bool   g_savedMode = false;
std::atomic<bool> g_running{false};
char   g_diagnostic[160] = "not started";

void SetDiagnostic(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_diagnostic, sizeof g_diagnostic, fmt, args);
    va_end(args);
}

void WriteOut(const char* text) {
    if (g_out == nullptr || text == nullptr) {
        return;
    }
    DWORD written = 0;
    const auto len = static_cast<DWORD>(strlen(text));
    if (len != 0) {
        WriteConsoleA(g_out, text, len, &written, nullptr);
    }
}

void WriteLine(const char* text) {
    WriteOut(text);
    WriteOut("\r\n");
}

void Prompt() { WriteOut("replay> "); }

// `help` is a rendering of the shared command table, so the text comes from the table and not from
// a second list that would drift out of step with it.
void PrintHelp() {
    WriteLine("");
    for (size_t i = 0; i < SpecCount(); ++i) {
        const CommandSpec& spec = SpecAt(i);
        char line[256];
        snprintf(line, sizeof line, "  %-46s %s", spec.usage, spec.help);
        WriteLine(line);
    }
    WriteLine("");
    WriteLine("  times: 90 seconds, 1:23.5 as m:s, 1:02:03 as h:m:s, or 50% of the capture");
    WriteLine("");
}

// What the input queue holds right now. Waiting on the handle alone is not enough: it signals for
// key-up, focus and menu events too, and a blocking read entered on one of those would ignore the
// quit event until the user happened to press Enter.
//
// ★ THE THREE CASES ARE KEPT APART because collapsing them is what made the drain blocking. "No
// records at all" and "records, but no carriage return among them" both used to answer `false`,
// and the caller's response to `false` was a blocking single-record read -- entered, in the first
// case, on an empty queue, where it waits for a keystroke and cannot see the quit event.
enum class InputState { LineReady, RecordsPending, Empty, Broken };

InputState PollInput() {
    DWORD pending = 0;
    if (!GetNumberOfConsoleInputEvents(g_in, &pending)) {
        return InputState::Broken;
    }
    if (pending == 0) {
        return InputState::Empty;
    }
    // 128 records is more than a person types between polls; anything beyond that is examined on
    // the next pass rather than needing a growing buffer.
    INPUT_RECORD records[128];
    DWORD read = 0;
    if (!PeekConsoleInputW(g_in, records, pending < 128 ? pending : 128, &read)) {
        return InputState::Broken;
    }
    for (DWORD i = 0; i < read; ++i) {
        if (records[i].EventType == KEY_EVENT && records[i].Event.KeyEvent.bKeyDown &&
            records[i].Event.KeyEvent.uChar.UnicodeChar == L'\r') {
            return InputState::LineReady;
        }
    }
    return InputState::RecordsPending;
}

// ============================================================================================
// WHAT THE COMMAND ACTUALLY DID, WHICH THIS CONSOLE USED TO NEVER SAY.
//
// dispatch::Submit answers "<name> accepted" for anything that mutates, because it has only queued
// it -- and that was the last word on the subject. `load C:\nope.echodemo` printed `load accepted`
// and nothing else ever; the real refusal went to the log and to a single-slot `last` line the
// next command overwrote. So a load that failed and a load that worked were indistinguishable on
// the owner's stated entry path.
//
// This waits, on the reader's own thread, which is a thread that exists to wait. The tick produces
// the outcome a frame later, so the normal case is one poll. The wait is bounded and the quit
// event is part of it: a `console close` stops this thread from the tick while it is sitting here.
//
// False means the console is stopping and the reader must return.
// ============================================================================================
bool ReportOutcome(const dispatch::Result& submitted) {
    constexpr DWORD kPollMs = 10;
    constexpr DWORD kWaitMs = 3000;
    for (DWORD waited = 0; waited < kWaitMs; waited += kPollMs) {
        dispatch::Result done;
        if (dispatch::TakeResult(submitted.seq, &done)) {
            WriteLine(done.text);
            return true;
        }
        if (g_quit != nullptr && WaitForSingleObject(g_quit, kPollMs) == WAIT_OBJECT_0) {
            return false;
        }
    }
    // Not "it failed". The tick has not run it, which is a different fact and is the one worth
    // reporting -- a paused game, a shutting-down module, or a tick that has stopped.
    WriteLine("(accepted, but the game has not run it within 3 s -- `diag` shows the queue)");
    return true;
}

DWORD WINAPI ReaderMain(LPVOID) {
    WriteLine("");
    WriteLine("EchoReplay console. Type `help` for the command list, `info` for the state.");
    WriteLine("Closing this window closes Echo VR with it -- the console belongs to the game's "
              "process. `console close` shuts the window down on its own.");
    Prompt();

    wchar_t line[kMaxLineChars];
    HANDLE waits[2] = {g_quit, g_in};

    for (;;) {
        const DWORD which = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (which == WAIT_OBJECT_0) {
            break;   // quit
        }
        if (which != WAIT_OBJECT_0 + 1) {
            break;   // the handle went away; there is nothing left to read
        }
        const InputState state = PollInput();
        if (state == InputState::Broken) {
            break;   // the handle is gone; there is nothing left to read
        }
        if (state == InputState::RecordsPending) {
            // Some other console event. Drain one record -- which cannot block, because the count
            // above said one is there -- so the handle stops being signalled by the same event
            // forever, and go back to the wait.
            INPUT_RECORD discard;
            DWORD read = 0;
            ReadConsoleInputW(g_in, &discard, 1, &read);
            continue;
        }
        if (state == InputState::Empty) {
            // Signalled with nothing in the queue. Re-entering an INFINITE wait would spin on a
            // handle that is still signalled, and a blocking read would ignore the quit event, so
            // this is the one place a short bounded wait is right.
            if (WaitForSingleObject(g_quit, 20) == WAIT_OBJECT_0) {
                break;
            }
            continue;
        }

        DWORD got = 0;
        if (!ReadConsoleW(g_in, line, static_cast<DWORD>(kMaxLineChars - 1), &got, nullptr)) {
            break;
        }
        line[got] = 0;
        while (got != 0 && (line[got - 1] == L'\r' || line[got - 1] == L'\n')) {
            line[--got] = 0;
        }

        const ParseResult parsed = Parse(line);
        if (!parsed.ok) {
            WriteLine(parsed.error);
            Prompt();
            continue;
        }
        if (parsed.cmd.id == CommandId::None) {
            Prompt();
            continue;
        }
        if (parsed.cmd.id == CommandId::Help) {
            PrintHelp();
            Prompt();
            continue;
        }

        const dispatch::Result submitted = dispatch::Submit(parsed.cmd);
        if (submitted.seq == 0) {
            // Answered on the spot: an immediate command, or a refusal at submission.
            if (submitted.text[0] != 0) {
                WriteLine(submitted.text);
            }
        } else if (!ReportOutcome(submitted)) {
            break;   // the console was stopped while we waited
        }
        Prompt();
    }

    g_running.store(false, std::memory_order_release);
    return 0;
}

}  // namespace

Mode ModeFromString(const char* value) {
    if (value == nullptr) {
        return Mode::Auto;
    }
    if (strcmp(value, "on") == 0) {
        return Mode::On;
    }
    if (strcmp(value, "off") == 0) {
        return Mode::Off;
    }
    return Mode::Auto;
}

bool ShouldStart(Mode mode, bool inSweep) {
    switch (mode) {
        case Mode::On:
            return true;
        case Mode::Off:
            return false;
        case Mode::Auto:
            // On for a person, off for a sweep. G12 names the console as one of the two ways a
            // replay is launched, so a default of off would be a config descope of a stated
            // requirement -- but a window appearing over a measured frame capture could disturb
            // the measurement it is sitting on top of.
            return !inSweep;
    }
    return !inSweep;
}

bool Start() {
    if (g_running.load(std::memory_order_acquire)) {
        return true;
    }

    // ============================================================================================
    // THE FOREGROUND IS TAKEN INSIDE AllocConsole, SO IT IS RESTORED AFTER IT.
    //
    // Measured on this machine: before AllocConsole the game's window is foreground; the instant it
    // returns, GetForegroundWindow() is the console, and it is STILL the console after the
    // SW_HIDE / SW_SHOWNOACTIVATE pair and a second later. Those two calls cannot undo an
    // activation that already happened -- there is no window to hide until AllocConsole has made,
    // shown and activated one. The only fix is to put the previous foreground window back, and
    // that is allowed here because the window we are restoring belongs to this same process.
    // ============================================================================================
    const HWND previousForeground = GetForegroundWindow();

    // A process launched from a terminal already has one. Allocating a second fails, and attaching
    // to the one that exists is the right answer rather than reporting a failure.
    if (AllocConsole()) {
        g_allocated = true;
    } else {
        const DWORD err = GetLastError();
        if (err != ERROR_ACCESS_DENIED) {
            SetDiagnostic("AllocConsole failed (%lu)", err);
            return false;
        }
    }

    const HWND window = GetConsoleWindow();
    if (window != nullptr) {
        ShowWindow(window, SW_HIDE);
        SetConsoleTitleW(L"EchoReplay");
        ShowWindow(window, SW_SHOWNOACTIVATE);
    }
    if (previousForeground != nullptr && previousForeground != window) {
        SetForegroundWindow(previousForeground);
    }

    g_in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                       OPEN_EXISTING, 0, nullptr);
    g_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, 0, nullptr);
    if (g_in == INVALID_HANDLE_VALUE || g_out == INVALID_HANDLE_VALUE) {
        SetDiagnostic("could not open CONIN$/CONOUT$ (%lu)", GetLastError());
        Stop();
        return false;
    }

    // Line input with echo, so the console itself provides editing and history and this file does
    // not reimplement backspace. Mouse and window events are switched off: they would signal the
    // input handle constantly and make the reader spin.
    if (GetConsoleMode(g_in, &g_savedInputMode)) {
        g_savedMode = true;
        DWORD wanted = g_savedInputMode;
        wanted |= ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT;
        wanted &= ~static_cast<DWORD>(ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT);
        SetConsoleMode(g_in, wanted);
    }

    g_quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_quit == nullptr) {
        SetDiagnostic("could not create the quit event (%lu)", GetLastError());
        Stop();
        return false;
    }

    g_running.store(true, std::memory_order_release);
    g_thread = CreateThread(nullptr, 0, &ReaderMain, nullptr, 0, nullptr);
    if (g_thread == nullptr) {
        g_running.store(false, std::memory_order_release);
        SetDiagnostic("could not start the reader thread (%lu)", GetLastError());
        Stop();
        return false;
    }

    SetDiagnostic("%s, %zu commands", g_allocated ? "own window" : "attached to an existing console",
                  SpecCount());
    return true;
}

void Stop() {
    if (g_quit != nullptr) {
        SetEvent(g_quit);
    }

    // ============================================================================================
    // AT PROCESS TERMINATION, TOUCH THE CONSOLE WITH NOTHING.
    //
    // This module's only caller is ControlShutdown, which runs from modreg::ShutdownAll. That had
    // one shipping caller, bootstrap::Detach when DllMain's lpReserved said the process was
    // terminating, and ENG-EXIT-FIX removed it; the rule below is for that place, where every other
    // thread has already been killed wherever it stood. If the reader was inside ReadConsoleW or
    // WriteConsoleA it was holding a console lock when it died, and SetConsoleMode / FreeConsole
    // from in here, under the loader lock, would wait on that lock forever: Echo VR would not
    // close and would have to be killed from Task Manager.
    //
    // The distinction is observable rather than assumed. ReaderMain clears g_running as its last
    // act, so a reader thread whose handle is SIGNALLED while g_running is still set did not
    // return -- it was terminated by somebody else, and this is that shutdown.
    // ============================================================================================
    if (g_thread != nullptr && g_running.load(std::memory_order_acquire) &&
        WaitForSingleObject(g_thread, 0) == WAIT_OBJECT_0) {
        g_running.store(false, std::memory_order_release);
        SetDiagnostic("the reader was terminated by process shutdown; the console was left alone");
        return;
    }

    if (g_thread != nullptr) {
        // Bounded. The reader is either in the wait, which the event releases at once, or inside a
        // read that a complete line has already made non-blocking. If it is somehow neither, this
        // returns anyway rather than deadlocking a shutdown.
        if (WaitForSingleObject(g_thread, 2000) != WAIT_OBJECT_0) {
            log::Direct(LogLevel::Warn, kReplayModuleName,
                        "the console reader did not stop within 2 s; leaving it and releasing "
                        "nothing it can still touch");
            // Deliberately NOT TerminateThread: it would abandon whatever lock the thread holds.
            // The handles below stay open in that case, which leaks two handles for the life of
            // the process and cannot corrupt anything.
            CloseHandle(g_thread);
            g_thread = nullptr;
            g_running.store(false, std::memory_order_release);
            SetDiagnostic("reader thread did not stop; console left running");
            return;
        }
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_quit != nullptr) {
        CloseHandle(g_quit);
        g_quit = nullptr;
    }
    if (g_in != nullptr && g_in != INVALID_HANDLE_VALUE) {
        if (g_savedMode) {
            SetConsoleMode(g_in, g_savedInputMode);
            g_savedMode = false;
        }
        CloseHandle(g_in);
    }
    g_in = nullptr;
    if (g_out != nullptr && g_out != INVALID_HANDLE_VALUE) {
        CloseHandle(g_out);
    }
    g_out = nullptr;
    if (g_allocated) {
        FreeConsole();
        g_allocated = false;
    }
    g_running.store(false, std::memory_order_release);
}

void Notice(const char* line) {
    if (!g_running.load(std::memory_order_acquire) || line == nullptr) {
        return;
    }
    // WriteConsoleA is safe from any thread; the reader is the only other writer and the worst a
    // race produces is an interleaved line, not a corrupted handle.
    WriteOut("\r\n");
    WriteLine(line);
}

bool Running() { return g_running.load(std::memory_order_acquire); }

const char* Diagnostic() { return g_diagnostic; }

}  // namespace console
}  // namespace control
}  // namespace replay
