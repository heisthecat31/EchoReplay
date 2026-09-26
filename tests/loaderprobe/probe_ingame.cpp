// W1-C's in-game probe: the half of this packet's acceptance that no offline check can reach.
//
// tests/loaderprobe proves the loader-lock shape, the CWD-relative scan and the wrong-build REFUSAL,
// because every host it can build is the wrong build by construction. What it cannot do is watch the
// guard ACCEPT - that needs the real echovr.exe, where the md5 matches, the RAD_Version string is present
// exactly once, and the ten recorded prologues are the bytes actually mapped at those addresses.
//
// ★ IT REPORTS ON BOTH OUTCOMES. The 2026-08-28 sweep refused (rlay.dll had already detoured a guarded
// site) and this probe wrote nothing at all, so the harness saw probesReported 0 and could not tell a
// deliberate refusal from a DLL that never loaded. The diagnosis existed only in echoreplay.log, which
// the descriptor did not collect. A refusal is a result, so it files one.
//
// That is why the reporting lives on its own thread rather than in the REPLAY_MODULE: modules are only
// initialised when the guard accepts, which is exactly the case that does not need explaining.
//
// It also carries a KNOWN-ANSWER CONTROL. A guard only ever seen agreeing has not been shown capable of
// disagreeing, and "10 of 10 sites matched" is worth nothing if the comparison is broken. One site is
// re-checked with a deliberately corrupted expectation and MUST be refused; if it is not, the probe
// reports NOT-RUN rather than PASS, because the instrument was not working.
//
// Queued through tests/loaderprobe/W1-C.probe.json. This packet never launches the game.

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "bootstrap/bootstrap.h"
#include "bootstrap/buildguard.h"
#include "bootstrap/coexist.h"
#include "bootstrap/config.h"
#include "bootstrap/egress.h"
#include "bootstrap/hookmgr.h"
#include "bootstrap/log.h"
#include "bootstrap/modreg.h"
#include "bootstrap/offlinelock.h"
#include "bootstrap/paths.h"
#include "bootstrap/services.h"
#include "generated/facts_generated.h"

namespace {

const char* const kReplayModuleName = "w1c-probe";
const char* const kProbeId = "W1-C-bootstrap";

volatile LONG g_moduleInitialised;

struct Findings {
    unsigned checked = 0;
    unsigned failed = 0;
    char     detail[2048] = {};
    size_t   detailLen = 0;
};

void Note(Findings& f, const char* fmt, ...) {
    if (f.detailLen + 2 >= sizeof f.detail) {
        return;
    }
    if (f.detailLen != 0) {
        f.detail[f.detailLen++] = ';';
        f.detail[f.detailLen++] = ' ';
    }
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(f.detail + f.detailLen, sizeof f.detail - f.detailLen, fmt, args);
    va_end(args);
    if (n > 0) {
        f.detailLen += static_cast<size_t>(n);
        if (f.detailLen >= sizeof f.detail) {
            f.detailLen = sizeof f.detail - 1;
        }
    }
}

void Check(Findings& f, const char* what, bool ok, const char* detail) {
    ++f.checked;
    if (!ok) {
        ++f.failed;
        Note(f, "FAILED %s (%s)", what, detail);
    }
    replay::log::Direct(ok ? replay::LogLevel::Info : replay::LogLevel::Error, kReplayModuleName, "%s %s%s%s",
                        ok ? "ok  " : "FAIL", what,
                        detail != nullptr && detail[0] != '\0' ? " - " : "",
                        detail != nullptr ? detail : "");
}

// ★ A size limit must cost a FIELD, never the document.
//
// This file used to accumulate snprintf's return value into `n`, which is the length it WOULD have
// written. The 2026-08-28 sweep shipped a done.json cut mid-token - `"InternetConnectA", "I], ...` -
// because egress::HookNamesJson overflowed upstream; ConvertFrom-Json rejected the file, the harness's
// whole egress block never ran, and a launch whose blocker was fully armed at +94 ms was recorded as
// "NotReported". The guard worked and the evidence did not survive the trip.
//
// So every append below is bounded, all-or-nothing, and kTailReserve bytes are held back from the first
// byte onward so the object can always be closed. A document that says "truncated": true is a usable
// report; a document that does not parse is silence.
constexpr size_t kTailReserve = 48;

struct Json {
    char*  out;
    size_t cap;
    size_t len;
    bool   truncated;
};

void JsonAppendN(Json& j, const char* text, size_t n) {
    if (j.len + n + kTailReserve > j.cap) {
        j.truncated = true;
        return;
    }
    std::memcpy(j.out + j.len, text, n);
    j.len += n;
    j.out[j.len] = '\0';
}

// No call site writes a length down. Counting the characters of a JSON fragment by hand is a defect
// waiting to happen, and the first draft of this file already had one of them wrong by a byte.
void JsonAppend(Json& j, const char* text) { JsonAppendN(j, text, std::strlen(text)); }

void JsonFmt(Json& j, const char* fmt, ...) {
    char field[1024];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(field, sizeof field, fmt, args);
    va_end(args);
    if (n < 0) {
        j.truncated = true;
        return;
    }
    size_t len = static_cast<size_t>(n);
    if (len >= sizeof field) {
        len = sizeof field - 1;
        j.truncated = true;
    }
    JsonAppendN(j, field, len);
}

// Escapes into the buffer a character at a time. Safe to cut short because the caller has already
// written the opening quote and always writes the closing one: a clipped string still parses.
void AppendEscaped(Json& j, const char* text) {
    for (const char* p = text; *p != '\0'; ++p) {
        char esc[2];
        size_t n = 0;
        switch (*p) {
            case '"':  esc[0] = '\\'; esc[1] = '"';  n = 2; break;
            case '\\': esc[0] = '\\'; esc[1] = '\\'; n = 2; break;
            case '\n': esc[0] = '\\'; esc[1] = 'n';  n = 2; break;
            case '\r': esc[0] = '\\'; esc[1] = 'r';  n = 2; break;
            case '\t': esc[0] = '\\'; esc[1] = 't';  n = 2; break;
            default:
                if (static_cast<unsigned char>(*p) >= 0x20u) {
                    esc[0] = *p;
                    n = 1;
                }
                break;
        }
        if (n == 0) {
            continue;
        }
        if (j.len + n + kTailReserve > j.cap) {
            j.truncated = true;
            return;
        }
        std::memcpy(j.out + j.len, esc, n);
        j.len += n;
        j.out[j.len] = '\0';
    }
}

// The harness contract: %REPLAY_PROBE_ROOT%\<dll base name>\done.json.
//
// Deliberately not through replay::IsWritablePath. That policy exists so the SHIPPED plugin leaves
// nothing behind but its log and its state directory; a probe's artifacts are supposed to land outside
// the install, which is the whole reason the harness supplies a root.
bool WriteDone(const char* verdict, const Findings& f, const char* cwd) {
    char root[MAX_PATH] = {};
    if (GetEnvironmentVariableA("REPLAY_PROBE_ROOT", root, sizeof root) == 0) {
        return false;
    }
    char dir[MAX_PATH];
    std::snprintf(dir, sizeof dir, "%s\\probe_bootstrap", root);
    CreateDirectoryA(root, nullptr);
    CreateDirectoryA(dir, nullptr);

    char path[MAX_PATH];
    std::snprintf(path, sizeof path, "%s\\done.json", dir);

    static char body[8192];
    Json j{body, sizeof body, 0, false};
    body[0] = '\0';

    JsonFmt(j, "{\n  \"probe\": \"%s\",\n  \"verdict\": \"%s\",\n  \"checked\": %u,\n"
               "  \"failed\": %u,\n  \"cwd\": \"", kProbeId, verdict, f.checked, f.failed);
    AppendEscaped(j, cwd);

    // W1-J's egress contract. Reported whatever it says, and "not-attempted" is recorded as such
    // rather than omitted: a probe that says nothing about egress is `unknown`, never `safe`.
    const replay::egress::Report& eg = replay::egress::Current();
    JsonFmt(j, "\",\n  \"egress\": { \"armed\": %s, \"state\": \"%s\", \"armedAtMs\": %llu, "
               "\"armDurationMs\": %llu, \"hooks\": [%s], \"hooksLive\": %u, \"hooksRequested\": %u, "
               "\"refused\": %llu, \"allowedLocal\": %llu, \"destinations\": %u, "
               "\"extensionsRefused\": %llu, \"socketsCreated\": %llu, \"firstSocketAfterMs\": %lld }",
            eg.state == replay::egress::ArmState::Armed ? "true" : "false",
            replay::egress::ArmStateName(eg.state), static_cast<unsigned long long>(eg.armedAtMs),
            static_cast<unsigned long long>(eg.armDurationMs), replay::egress::HookNamesJson(),
            eg.hooksLive, eg.hooksRequested, static_cast<unsigned long long>(eg.refusedRemote),
            static_cast<unsigned long long>(eg.allowedLocal), eg.distinctDestinations,
            static_cast<unsigned long long>(eg.extensionsRefused),
            static_cast<unsigned long long>(eg.socketsCreated),
            static_cast<long long>(eg.firstSocketAfterMs));
    JsonAppend(j, ",\n  \"notCovered\": \"");
    AppendEscaped(j, replay::egress::CoverageNote());

    // ★ The offline-bit lock. `state` says whether it is installed; `exercise` says whether anything
    // ever asked it to do its job. They are different facts and a quiet run must not read as the second.
    const replay::offlinelock::Report& lk = replay::offlinelock::Current();
    JsonFmt(j, "\",\n  \"offlineLock\": { \"state\": \"%s\", \"exercise\": \"%s\", "
               "\"sitesFound\": %u, \"sitesExpected\": %u, \"functionVa\": %llu, \"scanMs\": %llu, "
               "\"scanBytes\": %u, \"foreignPrologue\": %s, \"selfTestRan\": %s, "
               "\"selfTestClearedByOriginal\": %s, \"selfTestHeldWhenPreserved\": %s, "
               "\"calls\": %llu, \"clearsPrevented\": %llu, \"callsBitAlreadyClear\": %llu, "
               "\"callsBitUntouched\": %llu, \"sites\": [",
            replay::offlinelock::LockStateName(lk.state),
            replay::offlinelock::ExerciseName(replay::offlinelock::Exercised()),
            lk.sitesFound, lk.sitesExpected, static_cast<unsigned long long>(lk.functionVa),
            static_cast<unsigned long long>(lk.scanMs), lk.scanBytes,
            lk.foreignPrologue ? "true" : "false", lk.selfTestRan ? "true" : "false",
            lk.selfTestClearedByOriginal ? "true" : "false",
            lk.selfTestHeldWhenPreserved ? "true" : "false",
            static_cast<unsigned long long>(lk.calls),
            static_cast<unsigned long long>(lk.clearsPrevented),
            static_cast<unsigned long long>(lk.callsBitAlreadyClear),
            static_cast<unsigned long long>(lk.callsBitUntouched));
    for (uint32_t i = 0; i < lk.sitesRecorded; ++i) {
        JsonFmt(j, "%s{ \"va\": %llu, \"mask\": %u, \"baseReg\": %u }", i != 0 ? ", " : "",
                static_cast<unsigned long long>(lk.sites[i].va), lk.sites[i].mask,
                lk.sites[i].baseReg);
    }
    JsonAppend(j, "] }");

    JsonAppend(j, ",\n  \"detail\": \"");
    AppendEscaped(j, f.detail[0] != '\0' ? f.detail : "all checks agreed");
    JsonAppend(j, "\"");

    // Inside the reserve, so it cannot itself be the thing that does not fit.
    const int tail = std::snprintf(body + j.len, sizeof body - j.len, ",\n  \"truncated\": %s\n}\n",
                                   j.truncated ? "true" : "false");
    if (tail > 0) {
        j.len += static_cast<size_t>(tail);
    }

    HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, body, static_cast<DWORD>(j.len), &written, nullptr);
    CloseHandle(file);
    return ok != FALSE && written == j.len;
}

const char* VerdictName(replay::BuildVerdict v) {
    switch (v) {
        case replay::BuildVerdict::Accept:           return "Accept";
        case replay::BuildVerdict::RefuseWrongBuild: return "RefuseWrongBuild";
        case replay::BuildVerdict::RefuseUnreadable: return "RefuseUnreadable";
    }
    return "?";
}

void Report() {
    Findings f;
    char cwd[MAX_PATH] = {};
    GetCurrentDirectoryA(sizeof cwd, cwd);
    char detail[256];

    const replay::BuildVerdict verdict = replay::buildguard::Verdict();
    const replay::BuildIdentity& id = replay::buildguard::LastIdentity();

    // Reported whichever way it went, so the sweep can tell a refusal from a plugin that never loaded.
    Note(f, "verdict=%s", VerdictName(verdict));

    std::snprintf(detail, sizeof detail, "md5=%s expected=%s", id.md5Read ? id.md5 : "<unread>",
                  replay::ExpectedMd5());
    Check(f, "the md5 is the build the registry was read from",
          id.md5Read && std::strcmp(id.md5, replay::ExpectedMd5()) == 0, detail);

    std::snprintf(detail, sizeof detail, "found %u time(s)", id.versionStringCount);
    Check(f, "the RAD_Version string is in the mapped image exactly once",
          id.imageScanned && id.versionStringCount == 1u, detail);

    std::snprintf(detail, sizeof detail, "%u of %u matched%s%s",
                  id.siteCount - id.siteMismatches, id.siteCount,
                  id.siteMismatches != 0u ? ", first mismatch " : "",
                  id.siteMismatches != 0u ? id.firstMismatch : "");
    // ★ siteUnexplained, NOT siteMismatches. Asserting zero mismatches re-imports the exact false
    // negative buildguard.h records as settled and removed: rlay.dll had already detoured
    // SESSION.STATE_MACHINE, so a correct build of the right executable failed this check outright,
    // which breaks the owner's "coexist, never replace" ruling and SC9. A CLASSIFIED foreign detour is
    // the coexistence case the design chose to accept; an unexplained one is still a finding.
    Check(f, "every prologue mismatch is explained by a classified foreign detour",
          id.sitesChecked && id.siteCount == replay::buildguard::SiteCount() &&
              id.siteUnexplained == 0u,
          detail);

    // ★ The control. One real site re-checked with one byte of the expectation flipped: this MUST be
    // refused. If it is not, every "matched" above is meaningless and the run has no verdict to give.
    uint8_t corrupted[sizeof facts::GAME_UPDATE_PROLOGUE];
    std::memcpy(corrupted, facts::GAME_UPDATE_PROLOGUE, sizeof corrupted);
    corrupted[0] = static_cast<uint8_t>(corrupted[0] ^ 0xFFu);
    const bool controlRefused = !replay::buildguard::VerifySite(
        "W1C-PROBE-CONTROL", facts::GAME_UPDATE, corrupted, sizeof corrupted);
    Check(f, "control: a deliberately wrong expectation is refused", controlRefused,
          "if this passes, the prologue comparison is broken and everything above proves nothing");

    // A zero-length expectation must also refuse (the BLOCK-2 regression), in the real image this time.
    Check(f, "control: a zero-length expectation is refused",
          !replay::buildguard::VerifySite("W1C-PROBE-ZERO", facts::GAME_UPDATE, corrupted, 0),
          "a check of zero bytes must not read as agreement");

    // V-I11. Presence is recorded, never depended on.
    const bool rlay = replay::coexist::IsModuleLoaded("rlay.dll");
    std::snprintf(detail, sizeof detail, "rlay.dll %s", rlay ? "loaded" : "absent");
    Note(f, "%s", detail);
    replay::log::Direct(replay::LogLevel::Info, kReplayModuleName, "coexistence: %s", detail);

    Check(f, "the CWD is the directory whose plugins folder was scanned", cwd[0] != '\0', cwd);

    std::snprintf(detail, sizeof detail, "%llu registered, module init %s",
                  static_cast<unsigned long long>(replay::modreg::Count()),
                  g_moduleInitialised != 0 ? "ran" : "did not run");
    // On a refusal no module initialises, and that is correct rather than a failure - so what is asserted
    // is the invariant that holds either way: registration happened before DllMain, and init ran if and
    // only if the guard accepted.
    Check(f, "static self-registration happened and init matched the verdict",
          replay::modreg::Count() >= 1 &&
              ((verdict == replay::BuildVerdict::Accept) == (g_moduleInitialised != 0)),
          detail);

    std::snprintf(detail, sizeof detail, "%llu key(s) declared, config.json %s",
                  static_cast<unsigned long long>(replay::ConfigKeyCount()),
                  replay::config::FileWasWritten() ? "written"
                                                   : (replay::config::FileWasPresent() ? "read"
                                                                                       : "absent"));
    Check(f, "config resolved", replay::ConfigKeyCount() > 0, detail);

    // ★ Still zero, and still meaning what it always meant: W1-C claims no LEDGER hook row. The
    // offline-bit lock does not go through hookmgr - it is a safety mechanism, not an intervention, the
    // same reasoning that keeps the egress blocker's ws2_32 trampolines out of this count.
    Check(f, "no ledger hook row was armed by this packet", replay::hookmgr::ArmedCount() == 0,
          "W1-C claims none; W1-D and Wave 2 own the ledger's hook rows");

    // ★ The named expectation that replaces what the count above used to imply. The count could only
    // ever say "how many"; this says WHICH, so a second hook appearing anywhere in this packet fails
    // here instead of quietly fitting inside a loosened bound.
    const replay::offlinelock::Report& lock = replay::offlinelock::Current();
    std::snprintf(detail, sizeof detail, "%s, %u site(s) in %u byte(s) of .text, function 0x%llX",
                  replay::offlinelock::LockStateName(lock.state), lock.sitesFound, lock.scanBytes,
                  static_cast<unsigned long long>(lock.functionVa));
    Check(f, "the only game-code hook this packet installs is the offline-bit lock",
          lock.state == replay::offlinelock::LockState::Locked &&
              lock.sitesFound == lock.sitesExpected && lock.functionVa != 0,
          detail);

    // The control travels with it. A lock whose self-test did not discriminate has not been shown to
    // have found the right instruction, and "locked" would then be a claim rather than a result.
    Check(f, "control: the located bytes cleared the bit and a preserving mask did not",
          lock.selfTestRan && lock.selfTestClearedByOriginal && lock.selfTestHeldWhenPreserved,
          "if this fails the lock is hooking bytes it has not identified");

    // ★ Reported, never asserted. Whether the teardown ran at all is a property of what the player did
    // in this launch, not of our correctness - so it is a Note. Asserting it would make a quiet launch
    // look like a broken lock.
    Note(f, "offlineLock=%s exercise=%s calls=%llu prevented=%llu",
         replay::offlinelock::LockStateName(lock.state),
         replay::offlinelock::ExerciseName(replay::offlinelock::Exercised()),
         static_cast<unsigned long long>(lock.calls),
         static_cast<unsigned long long>(lock.clearsPrevented));

    // The egress blocker protects a binding owner constraint, so its state is ASSERTED rather
    // than merely reported. PartiallyArmed fails here: an entry point that did not take the
    // trampoline is an open path, and calling that armed would be the lie it exists to prevent.
    const replay::egress::Report& armed = replay::egress::Current();
    std::snprintf(detail, sizeof detail, "%s, %u/%u live, armed %llu ms in, %llu refused",
                  replay::egress::ArmStateName(armed.state), armed.hooksLive,
                  armed.hooksRequested, static_cast<unsigned long long>(armed.armedAtMs),
                  static_cast<unsigned long long>(armed.refusedRemote));
    Check(f, "the egress blocker armed every entry point",
          armed.state == replay::egress::ArmState::Armed &&
              armed.hooksLive == armed.hooksRequested, detail);
    Note(f, "egress=%s", replay::egress::ArmStateName(armed.state));

    // A failed control means the instrument was not working, which is NOT-RUN rather than FAIL: the two
    // need opposite responses and must never share a colour. A refusal is a real, reportable outcome.
    const char* reported = "PASS";
    if (!controlRefused) {
        reported = "NOT-RUN";
    } else if (verdict != replay::BuildVerdict::Accept) {
        reported = "REFUSED";
    } else if (f.failed != 0u) {
        reported = "FAIL";
    }

    if (!WriteDone(reported, f, cwd)) {
        replay::log::Direct(replay::LogLevel::Error, kReplayModuleName,
                            "could not write done.json; REPLAY_PROBE_ROOT is %s",
                            GetEnvironmentVariableA("REPLAY_PROBE_ROOT", nullptr, 0) != 0 ? "set"
                                                                                          : "unset");
    }
    replay::log::Direct(replay::LogLevel::Info, kReplayModuleName, "probe %s: %u checked, %u failed", reported,
                        f.checked, f.failed);
}

DWORD WINAPI ProbeThread(LPVOID) {
    // The bootstrap sets Ready on both paths - after a refusal as well as after module init - so this
    // reaches a verdict either way. The deadline is a fallback: if the worker never got that far,
    // reporting what we can see beats reporting nothing.
    const ULONGLONG deadline = GetTickCount64() + 30000;
    while (!replay::bootstrap::Ready() && GetTickCount64() < deadline) {
        Sleep(10);
    }
    Report();
    return 0;
}

// Started from a static initialiser, which the CRT runs inside DllMain before the bootstrap's own
// DLL_PROCESS_ATTACH body. CreateThread is the one thing the loader contract sanctions there; the thread
// does not run until the loader lock is released, and it only polls until then.
struct ProbeStarter {
    ProbeStarter() {
        HANDLE thread = CreateThread(nullptr, 0, ProbeThread, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
    }
};

ProbeStarter g_starter;

bool Init(replay::ReplayServices* services) {
    REPLAY_REQUIRE(services, log);
    REPLAY_REQUIRE(services, config);
    InterlockedExchange(&g_moduleInitialised, 1);
    services->log->Write(replay::LogLevel::Info, kReplayModuleName,
                         "module registry live: %llu module(s) registered",
                         static_cast<unsigned long long>(replay::modreg::Count()));
    return true;
}

void Shutdown() {}

}  // namespace

// Order 900: after any adapter a future packet registers, so what the report sees is the settled state.
REPLAY_MODULE("w1c-probe", 900, Init, nullptr, Shutdown);
