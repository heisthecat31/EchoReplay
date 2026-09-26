// Where this plugin is allowed to write, and how it works that out without trusting the CWD.
//
// The loader scans `current_path()/plugins`, not the exe directory (R11 S0.2), so the CWD is an input we
// cannot rely on: a launcher that sets it elsewhere still loads us, from a different plugins folder. Every
// path here is therefore derived from our own module handle.
//
// The design - "clean uninstall = delete echoreplay.dll" - is only true if nothing writes
// outside the two places INSTALL.md lists. IsWritablePath is that rule made checkable rather than
// remembered, and every file this packet opens for writing goes through it.
//
// ★ A DISAGREEMENT BETWEEN TWO AUTHORITIES, RAISED RATHER THAN QUIETLY RESOLVED. DESIGN S4.5.3 rule 5
// says "All state lives in bin\win10\echoreplay\ (config, log)", which puts the log INSIDE the state
// directory. docs/INSTALL.md - W0-1, committed before this packet started - documents and this code
// implements bin\win10\echoreplay.log, BESIDE it. The implemented layout is what INSTALL.md tells a user
// to look for and what the probe asserts, so changing it now would break a published instruction; but
// this packet does not get to overrule the design by choosing. Flagged for the architect and W0-1 to
// reconcile. Either layout satisfies "uninstall is deleting one file"; only one of the two documents is
// right about where to look.
#pragma once

#include <stddef.h>

#include <string>
#include <vector>

namespace replay {

// Long enough for a real install path with room to spare; truncation is detected and reported, never
// silently accepted, because a truncated path is a write to the wrong place.
constexpr size_t kPathMax = 1024;

// Pure: strip the file name, and if the containing directory is named `plugins`, strip that too.
// `...\bin\win10\plugins\echoreplay.dll` -> `...\bin\win10`, which is where INSTALL.md puts the log and
// the state directory. A DLL that is not inside a `plugins` directory (a unit test, a probe host) keeps
// its own directory, so nothing escapes into a parent nobody expects.
// Returns false if `modulePath` has no directory part or the result does not fit.
bool DeriveInstallRoot(const wchar_t* modulePath, wchar_t* out, size_t cap);

// Pure: is `path` inside `root`, comparing case-insensitively and only at a separator boundary, so
// `C:\a\binary` is not inside `C:\a\bin`. Both are expected to be absolute and already normalised;
// this is a containment test, not a canonicaliser.
bool IsUnderDirectory(const wchar_t* path, const wchar_t* root);

// The process-wide paths, resolved once from our own module handle. Resolve() must be called before any
// of the accessors; it is the first thing the bootstrap does after the attach line.
bool Resolve(void* selfModule);
bool Resolved();

const wchar_t* SelfModulePath();      // ...\plugins\echoreplay.dll
const wchar_t* SelfModuleDir();       // ...\plugins
const wchar_t* InstallRoot();         // ...\bin\win10
const wchar_t* LogPath();             // ...\bin\win10\echoreplay.log
const wchar_t* StateDir();            // ...\bin\win10\echoreplay
const wchar_t* ConfigPath();          // ...\bin\win10\echoreplay\config.json

// The uninstall rule, enforced. True only for the log file and paths under the state directory.
bool IsWritablePath(const wchar_t* path);

// The `capture.dir` row, named once so the writer and the reader cannot spell it differently.
constexpr const char* kCaptureDirKey = "capture.dir";

// ★ THE ONE PLACE THE CAPTURE DIRECTORY IS DECIDED. DESIGN S6.6: captures go to the user's
// Documents and NEVER inside the game install. `configDir` is the `capture.dir` row's value as
// UTF-8 (null or empty when unset); the fallback is `%USERPROFILE%\Documents\EchoReplay`, and
// `.\EchoReplay` only when the profile directory is unknowable.
//
// It lives here rather than in either module because it had been written twice: src/capture wrote
// captures to Documents\EchoReplay while the src/ui browser scanned
// `<install>\bin\win10\echoreplay\replays`, and the panel in L5-RUN-7-run2.png reports "No
// .echodemo files in:" a folder nothing has ever written to. Two constants for one directory is a
// browser that is guaranteed to be looking somewhere else.
//
// Returns false when the result had to be truncated or could not be built; `out` is always
// NUL-terminated, and a truncated path is a path to somewhere else, never silently accepted.
bool ResolveCaptureDir(const char* configDir, wchar_t* out, size_t cap);

// Pure: `<root>\EchoReplay`. Every arm of ResolveCaptureDir that builds a path goes through this,
// so the folder name and the separator exist once and a unit test can pin the result byte for byte
// without a profile directory, a shell, or a machine. The redirect arm had
// shipped a corrupted literal for a whole packet because no test asserted the VALUE it produced.
bool CaptureDirUnder(const wchar_t* root, wchar_t* out, size_t cap);

// Pure: `<userProfile>\Documents\EchoReplay`, the production rule with its only machine-specific
// part passed in. src/capture builds the identical string from the same USERPROFILE (capture.cpp,
// the `capture.dir` else-arm); the writer and the browser must not be given two different ideas of
// where Documents is, so this stays env-derived until that duplicate collapses onto
// ResolveCaptureDir.
bool CaptureDirForProfile(const wchar_t* userProfile, wchar_t* out, size_t cap);

// `import.dirs`: the folders the browser also lists legacy replays (.echoreplay, .tape) from, beside
// the capture directory (DESIGN-FORMATS 7.8). A ';'-separated list; set to "" it lists none.
constexpr const char* kImportDirsKey = "import.dirs";

// `configValue` is the row as UTF-8, null when unset. Unset means Spark's own default folder,
// `%USERPROFILE%\Documents\Spark\replays`, or none when the profile directory is unknown. Under
// SetPathsForTest unset means none: a test must never list the owner's real Spark folder.
std::vector<std::wstring> ResolveImportDirs(const char* configValue);

// Test seam: pretend Resolve() ran against this module path. Used by the unit tests so the policy can be
// exercised without a loaded DLL. Not called by the plugin.
void SetPathsForTest(const wchar_t* modulePath);

}  // namespace replay
