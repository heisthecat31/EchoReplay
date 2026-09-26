#pragma once

// W2-G. Ledger row 0.2: find out which capture this process was launched to open.
//
// ============================================================================================
// WHY THIS IS AN ENVIRONMENT VARIABLE AND NOT A COMMAND-LINE SWITCH ANY MORE.
//
// It used to be `-replayviewer <path>`, read off GetCommandLineW. AUDIT_W1-F_R4 F2 traced what the
// engine actually does with a switch nobody registered, and it is not the permissive skip DESIGN
// 8.4.3 assumed:
//
//   sub_1400DA830 LABEL_15 returns non-zero for an unregistered token; sub_140503AB0 propagates it;
//   and ApplyCommandLine tests that return at 0x140116737 and jumps straight to its own epilogue at
//   0x1401181D1. EVERY switch handler on the line is skipped.
//
// So one `-replayviewer` on the line made the engine ignore `-offline`, `-level`, `-region` and all
// four 127.0.0.1 host pins, silently, and the launch went ONLINE. Registering the switch instead
// would cost a Level-2 detour on the one function rlay.dll already detours (SC9 coexistence), to buy
// a line in -help.
//
// `ECHOREPLAY_OPEN=<path>` costs the engine nothing. The child inherits it from CreateProcess, the
// engine never looks at it, and no token of ours is on the line for ApplyCommandLine to trip over.
//
// * THERE IS NO COMMAND-LINE FALLBACK, DELIBERATELY. A fallback re-opens the hazard: it would make a
// launch that carries the switch work, which is exactly the launch whose `-offline` was voided.
// What is left of `-replayviewer` here is a DIAGNOSTIC ONLY -- see CommandLineMentionsReplayViewer,
// whose whole job is to say so in the log when a stale launcher still passes it.
//
// * NOTHING HERE TOUCHES THE PROCESS' OWN STATE. The value is read once and validated; the read
// happens on the worker at module Init, which is where the switch was read too, and never in
// DllMain -- validation opens the file, and file I/O under the loader lock is how deadlocks start.
// ============================================================================================

#include <stddef.h>
#include <stdint.h>

#include "control/command.h"

namespace replay {
namespace control {

// The carrier. Set by the launcher on the child's environment block; never on the command line.
inline constexpr wchar_t kOpenEnvVar[] = L"ECHOREPLAY_OPEN";

// The one value that is not a path: enter from the most recent capture and open the browser, which
// is what a bare `-replayviewer` used to mean. A literal `*` because it cannot be a Windows file
// name, so it can never collide with a real path -- and because an EMPTY value cannot carry it:
// GetEnvironmentVariableW answers 0 for both "absent" and "set to nothing", so an empty string has
// no encoding here and is treated as absent.
inline constexpr wchar_t kOpenMostRecent[] = L"*";

// The container's extension. `src/ui/demoindex.cpp` spells the same one in its scan pattern; this
// packet cannot include that header without pulling the UI into a library that has no dependencies,
// so the two spellings are kept in step by hand.
inline constexpr wchar_t kCaptureExtension[] = L".echodemo";

// The legacy replays the viewer converts on open (DESIGN-FORMATS 7.8). src/ui/legacyimport.cpp
// spells the same two for the same reason as above.
inline constexpr wchar_t kLegacyReplayExtension[] = L".echoreplay";
inline constexpr wchar_t kLegacyTapeExtension[] = L".tape";

// Why a value that was set could not be opened. Every one of these is a refusal that is counted and
// named, never a silent no-op and never a guess at what the owner meant.
enum class OpenRefusal : uint8_t {
    None,
    TooLong,
    NotAbsolute,
    WrongExtension,
    Missing,
    NotAFile,
    Unreadable,
};

const char* OpenRefusalText(OpenRefusal reason);

struct OpenRequest {
    bool present = false;        // the variable was set to something
    bool mostRecent = false;     // ... and that something was kOpenMostRecent
    bool hasPath = false;        // ... or a path that passed every check below
    OpenRefusal refusal = OpenRefusal::None;
    // The value as read, trimmed and unquoted, even when it was refused -- a refusal that cannot
    // name the path it refused is not diagnosable. Empty when the value did not fit.
    wchar_t path[kMaxPathChars] = {0};
};

// Reads kOpenEnvVar and validates it: absolute, ending in kCaptureExtension, present on disk as a
// file, and openable for reading. Returns OpenRequest::present -- false means the variable was not
// set, which is the ordinary case and is not an error.
bool ReadOpenRequest(OpenRequest* out);

// The exact line the control module logs when a launcher still puts the old switch on the engine's
// line. Declared here so a test can pin the wording: by the time it is written the engine has
// already discarded the whole line, so this log line is the only evidence the launch leaves.
inline constexpr char kLegacySwitchWarning[] =
    "unregistered switch on command line: -replayviewer (engine ignored the command line -- see "
    "AUDIT_W1-F_R4 F2)";

// DIAGNOSTIC ONLY. True if `commandLine` carries a `-replayviewer` token under any of the three
// prefixes the engine's tokeniser honours ('-', '+', 0x96), in the separate or the attached form.
// Nothing acts on the answer except the log line that tells the owner the engine threw the whole
// line away. `commandLine` may be null or empty.
bool CommandLineMentionsReplayViewer(const wchar_t* commandLine);

}  // namespace control
}  // namespace replay
