#pragma once

// W2-A. How long this process was launched to capture for, and where that number comes from.
//
// ============================================================================================
// WHY THIS IS AN ENVIRONMENT VARIABLE AND NOT A COMMAND-LINE SWITCH.
//
// The plan's R3 draft put the capture deadline on the engine's line as `-replaycapturesecs <N>`.
// AUDIT_W1-F_R4 F2 then traced what the engine does with a switch nobody registered, and it is not
// the permissive skip DESIGN 8.4.3 assumed:
//
//   sub_1400DA830 LABEL_15 returns non-zero for an unregistered token; sub_140503AB0 propagates it;
//   and ApplyCommandLine tests that return at 0x140116737 and jumps straight to its own epilogue at
//   0x1401181D1. EVERY switch handler on the line is skipped.
//
// `-replaycapturesecs` is the SECOND instance of that failure, and the worse of
// the two: it would have ridden on `capture-local`, the launch that produces F-REAL. One such token
// makes the engine ignore `-offline`, `-level`, `-region` and all four 127.0.0.1 host pins,
// silently, and a 300 s capture session runs ONLINE and unpinned.
//
// `ECHOREPLAY_CAPTURE_SECS=<N>` costs the engine nothing: the child inherits it from CreateProcess,
// no engine parser reads the environment block, and no token of ours is on the line to trip over.
//
// * THERE IS NO COMMAND-LINE FALLBACK, DELIBERATELY, and there never was a reader here to keep one
// working. A fallback would make the launch whose `-offline` was voided appear to work, which is
// exactly the launch that must not. What is left of the old switch is a DIAGNOSTIC -- see
// CommandLineMentionsCaptureSecs, whose only job is to say so in the log when a stale launcher
// still passes it, because by then the engine has already discarded the whole line and this log
// line is the only evidence the launch leaves.
//
// * RK-11: unlike a command-line token, an environment variable can already exist machine-wide, so
// a value can arrive from a user-level setting nobody meant for this launch. Nothing here can tell
// those apart, so the caller LOGS the value it read -- an armed deadline is never silent.
// ============================================================================================

#include <stddef.h>
#include <stdint.h>

namespace replay {
namespace capture {

// The carrier. Set by the launcher on the child's environment block.
inline constexpr wchar_t kCaptureSecsEnvVar[] = L"ECHOREPLAY_CAPTURE_SECS";

// The bound, and why it is these two numbers.
//
// The FLOOR is HC-1c's own: 30 s. Below it the deadline can fire before a match has started, which
// would produce an empty capture and report it as a completed one.
//
// The CEILING is this module's. HC-1c's real upper bound is `timeoutSec - 15`, which only the
// harness knows -- it holds the launch's timeout and refuses the plan before any launch. The module
// cannot see that number, so it enforces the one bound it can justify on its own: a deadline more
// than a day out is a value that was mistyped or handed over in milliseconds, and arming it would
// be indistinguishable from arming nothing while reporting that a deadline is set. The harness's
// tighter bound still applies on top of this one; both refuse, neither clamps.
inline constexpr uint32_t kCaptureSecsMin = 30;
inline constexpr uint32_t kCaptureSecsMax = 86400;

// The old switch's name, spelled ONCE in this project. The scanner below has to hold the token it
// looks for and the warning has to name it, and two spellings of one name drift silently -- the
// drift here would make the scanner quietly stop matching the thing it exists to catch.
#define REPLAY_LEGACY_CAPTURE_SECS_SWITCH "replaycapturesecs"

// Why a value that was set could not be used. Every one of these is a refusal that is named,
// counted and logged, and leaves the default behaviour exactly as it was -- never a silent clamp
// into range, which would run a capture of a length nobody asked for and report success.
enum class CaptureSecsRefusal : uint8_t {
    None,
    TooLong,       // the value did not fit the buffer, so it is not the value that was meant
    NotANumber,    // empty after trimming, signed, or carrying a non-digit
    OutOfRange,    // a number, but outside [kCaptureSecsMin, kCaptureSecsMax]
};

const char* CaptureSecsRefusalText(CaptureSecsRefusal reason);

// Longest value this reads. Ten digits is the whole u32 domain, and the rest is room for the
// whitespace and quotes a launcher leaves behind; anything longer is refused as TooLong rather than
// truncated into a different number.
inline constexpr size_t kMaxCaptureSecsChars = 32;

struct CaptureSecsRequest {
    bool present = false;        // the variable was set to something
    bool hasSeconds = false;     // ... and that something passed every check below
    uint32_t seconds = 0;        // valid only when hasSeconds
    CaptureSecsRefusal refusal = CaptureSecsRefusal::None;
    // The value as read, trimmed and unquoted, even when it was refused -- a refusal that cannot
    // quote what it refused is not diagnosable. Empty when the value did not fit.
    wchar_t value[kMaxCaptureSecsChars] = {0};
};

// The parse alone, with no environment behind it, so every boundary can be driven directly.
// `text` need not be null-terminated; `len` is its length. On CaptureSecsRefusal::None, *seconds
// holds the parsed value; otherwise *seconds is left at zero.
CaptureSecsRefusal ParseCaptureSeconds(const wchar_t* text, size_t len, uint32_t* seconds);

// Reads kCaptureSecsEnvVar and validates it. Returns CaptureSecsRequest::present -- false means the
// variable was not set (or was set to nothing, which GetEnvironmentVariableW cannot tell apart from
// unset and which this therefore treats the same way). That is the ordinary case, not an error, and
// the caller's behaviour must be unchanged by it.
bool ReadCaptureSecondsRequest(CaptureSecsRequest* out);

// The exact line the capture module logs when a launcher still puts the old switch on the engine's
// line. Declared here so a test can pin the wording: by the time it is written the engine has
// already discarded the entire line, pins and all, so nothing else records that it happened.
inline constexpr char kLegacyCaptureSecsWarning[] =
    "unregistered switch on command line: -" REPLAY_LEGACY_CAPTURE_SECS_SWITCH " (engine ignored "
    "the whole command line -- -offline and every host pin with it -- see AUDIT_W1-F_R4 F2; the "
    "capture deadline travels in ECHOREPLAY_CAPTURE_SECS now)";

// DIAGNOSTIC ONLY. True if `commandLine` carries the old token under any of the three prefixes the
// engine's tokeniser honours ('-', '+', 0x96), separated from its value or attached to it by '=' or
// ':'. Nothing acts on the answer except the log line. `commandLine` may be null or empty.
bool CommandLineMentionsCaptureSecs(const wchar_t* commandLine);

}  // namespace capture
}  // namespace replay
