// W2-F's in-game probe: does a band-3 component-system registration LAND, does the callback FIRE,
// and on WHICH THREAD. Queued through runs\queue\W2-F.probe.json; this packet never launches the
// game.
//
// ---------------------------------------------------------------------------------------------
// ★ IT MEASURES THE SHIPPING CODE, NOT A COPY OF IT. This DLL links src\ui\csregister.cpp itself,
// plus the shipped bootstrap and the shipped tick -- W1-H's probe_camera pattern. A probe that
// reimplemented the registration would answer a question about the reimplementation: the two silent
// traps here are in the ARGUMENTS (a key in no band) and in the ENGINE (bit 23 of [game+0x828]
// skipping the whole stage-0..3 pass), and either could be got right by accident in a second copy
// while the shipping one had it wrong. csregister::Status is reported verbatim.
//
// ★ IT DOES NOT DRAW. The registered callback counts, records its thread id, and returns. No
// NRadGui call, no ImGui, nothing. Drawing would answer a question nobody asked and would do it
// inside a frame this probe exists to establish the safety of -- and if the callback turns out to
// run on a CTaskKernel worker, a drawing probe would have made an unsafe ImGui call to find out.
//
// ★ REGISTRATION HAPPENS ON THE STAGE-0 TICK, WHICH IS WHY THE TICK IS LINKED AT ALL. The record
// table grows by REALLOCATION and the runner re-reads its base after a callback returns, with no
// lock anywhere in the registrar. Appending from this probe's sampler thread could free the array
// the main thread is walking. That is a crash in the owner's game, and the launch that carried the
// probe is spent either way -- so the sampler only ever READS counters, and csregister::OnTick is
// driven from the real tick like the shipping plugin drives it.
//
// ★ THE VERDICT IS FILED FROM A STATIC INITIALISER. coexist::ClaimInstance refuses every module
// after the first in a multi-probe launch, and this DLL carries a bootstrap, so it is refusable.
// The provisional record is written before DllMain and before any claim, so a refused or killed
// run still says "I loaded and got this far" instead of nothing. W1-M and W1-H each proved this
// mechanism with a mutant; it is not re-proved here, it is reused.
//
// ★ COULD-NOT-DETERMINE IS THE THIRD OUTCOME AND HERE IT IS A LIKELY ONE. If the callback never
// fires, that is a DETERMINATION about the band system -- and which of its two traps -- not a
// failure of the read. `checked` counts only Ok and Failed, so a launch that never reaches a match
// reports checked=0 rather than a confident nothing.
// ---------------------------------------------------------------------------------------------

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bootstrap/bootstrap.h"
#include "bootstrap/buildguard.h"
#include "bootstrap/offlinelock.h"
#include "bootstrap/services.h"
#include "offlinelock_held.h"
#include "engine/tick.h"
#include "generated/facts_generated.h"
#include "ui/csregister.h"
#include "probe_ui_outcome.h"

namespace replay {
namespace {

const char* const kReplayModuleName = "w2f-probe";

// OkQualified is the fourth thing a check can be: it ASKED its question and the answer was yes, and
// in answering it it raised a second question it cannot answer. CSREGISTER-EPOCH section 3 calls
// that case PASS-WITH-STALE and asks for its own word, but the harness's verdict vocabulary is
// fixed at PASS/FAIL/NOT-RUN/REFUSED and any other word is read as NOT-RUN
// (:2719) -- writing it would render a RECOVERED stale as could-not-ask and redden the sweep. The
// harness does have a channel for exactly this shape, and it is the one it already reads: a record
// with undetermined > 0 is marked Qualified (:2728-2731) and Get-SweepTally counts it apart from a
// clean pass without taking it out of FAIL. So a qualified check keeps the verdict word PASS,
// contributes 1 to the reported `undetermined` count, and says why in `why` and in the detail.
enum class Outcome { Undetermined, Ok, OkQualified, Failed };

struct Check {
    const char* id;
    Outcome outcome;
    char detail[512];
};

// Every invocation of the registered callback, up to this many, is recorded raw. The payload asks
// for the RAW ids per invocation and not a boolean, because "non-zero on SOME frames" is the case
// that matters and a summary hides it.
constexpr size_t kMaxSamples = 120;

struct Sample {
    uint32_t threadId;
    uint32_t dispatchFlag;   // *(u32*)(TlsGetValue(idx) + 1840); 0xFFFFFFFF when unreadable
    uint32_t frameIndex;
};

struct State {
    HMODULE self = nullptr;
    char stem[64] = {0};

    volatile LONG moduleInitRan = 0;
    volatile LONG tickCount = 0;
    uint32_t tickThreadId = 0;

    volatile LONG sampleCount = 0;
    Sample samples[kMaxSamples] = {};

    uint32_t clockMaxElapsedUs = 0;
    bool clockRead = false;
    uint64_t frameDeltaSumUs = 0;
    uint64_t frameDeltaCount = 0;

    // Why the verdict is qualified, when it is. Empty means it is not.
    char why[256] = {0};
};

State g;

// Whether bootstrap::Attach accepted this module. Separate from the lock's own state because they
// answer different questions: a refused instance claim means WE installed nothing, while the lock
// may still be held by whichever module won. Only the second decides whether the launch is safe.
volatile LONG g_bootstrapOk = 0;

// ---- reads -----------------------------------------------------------------------------------

// The per-thread flag the CS runner tests to decide between calling a node inline and handing it to
// CTaskKernel. Read through ReadProcessMemory on our own process: the TLS array is only as long as
// the thread needed, so a short one must return false rather than take the game down.
uint32_t ReadDispatchFlag() {
    const void* idxAddr = buildguard::MappedAddress(0x142098EE0ull);
    if (idxAddr == nullptr) return 0xFFFFFFFFu;
    DWORD slot = 0;
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), idxAddr, &slot, sizeof slot, &got) ||
        got != sizeof slot) {
        return 0xFFFFFFFFu;
    }
    const void* ctx = TlsGetValue(slot);
    if (ctx == nullptr) return 0xFFFFFFFFu;
    uint32_t value = 0;
    const void* at = static_cast<const uint8_t*>(ctx) + 1840;
    if (!ReadProcessMemory(GetCurrentProcess(), at, &value, sizeof value, &got) ||
        got != sizeof value) {
        return 0xFFFFFFFFu;
    }
    return value;
}

void ReadClockCeiling() {
    const void* slot = buildguard::MappedAddress(facts::CLOCK_GLOBAL_PTR);
    if (slot == nullptr) return;
    void* clock = nullptr;
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), slot, &clock, sizeof clock, &got) ||
        got != sizeof clock || clock == nullptr) {
        return;
    }
    uint32_t value = 0;
    const void* at = static_cast<const uint8_t*>(clock) + facts::CLOCK_MAXELAPSED_OFF;
    if (!ReadProcessMemory(GetCurrentProcess(), at, &value, sizeof value, &got) ||
        got != sizeof value) {
        return;
    }
    g.clockMaxElapsedUs = value;
    g.clockRead = true;
}

// ---- the callback under test -------------------------------------------------------------------

// Installed as csregister's DrawFn. It draws nothing; see the header.
//
// NOTE csregister refuses to call this at all when the callback lands off the tick's thread, and
// counts that instead -- which is the shipping behaviour and exactly what is being measured. So the
// off-thread case shows up in csregister::Report::offThreadInvocations, not here, and a sample
// recorded here is by construction a same-thread one. Both numbers are reported.
// It reports Drawn because the probe measures DISPATCH, not pixels: there is no facade bound in a
// probe and the panel it stands in for is not this module's. The frame-open question the probe does
// answer is the W2-R row's, sampled from the globals rather than inferred from this return.
ui::inframe::DrawOutcome CountDraw() {
    const LONG i = InterlockedIncrement(&g.sampleCount) - 1;
    if (i < 0 || static_cast<size_t>(i) >= kMaxSamples) return ui::inframe::DrawOutcome::Drawn;
    g.samples[i].threadId = GetCurrentThreadId();
    g.samples[i].dispatchFlag = ReadDispatchFlag();
    g.samples[i].frameIndex = static_cast<uint32_t>(g.tickCount);
    return ui::inframe::DrawOutcome::Drawn;
}

// ---- the shipping tick, driving the shipping registrar ------------------------------------------

void ProbeTick(const TickInfo& info) {
    InterlockedIncrement(&g.tickCount);
    g.tickThreadId = GetCurrentThreadId();
    if (info.frameDeltaUs > 0 && info.frameDeltaUs < 200000) {
        g.frameDeltaSumUs += info.frameDeltaUs;
        ++g.frameDeltaCount;
    }
    if (!g.clockRead) ReadClockCeiling();
    // The shipping call, on the shipping thread. Nothing about this line differs from src/ui.
    ui::csregister::OnTick(info.netGame, GetCurrentThreadId());
}

bool ProbeInit(ReplayServices* services) {
    REPLAY_REQUIRE(services, log);
    InterlockedExchange(&g.moduleInitRan, 1);
    ui::csregister::SetDrawFn(&CountDraw);
    // The shipping order, on the shipping path: src/ui/ui_module.cpp enables here too, before any
    // tick and without waiting on the registration (AUDIT_F3I F3I-4).
    ui::csregister::Enable();
    services->log->Write(LogLevel::Info, kReplayModuleName,
                         "probe armed: band-3 registration will be attempted from the tick");
    return true;
}

// ---- reporting ----------------------------------------------------------------------------------

bool ProbeRoot(char* out, size_t bytes) {
    const DWORD n = GetEnvironmentVariableA("REPLAY_PROBE_ROOT", out, static_cast<DWORD>(bytes));
    return n != 0 && n < bytes;
}

void ProbeId(char* out, size_t bytes) {
    // The most restrictive fallback: an id we did not recognise names itself as unresolved rather
    // than silently becoming some other probe's identity.
    strncpy_s(out, bytes, "W2-F-uipanel-unresolved-id", _TRUNCATE);
    char ids[512] = {0};
    if (GetEnvironmentVariableA("REPLAY_PROBE_IDS", ids, sizeof ids) == 0) return;
    char key[80];
    _snprintf_s(key, sizeof key, _TRUNCATE, "%s=", g.stem);
    const char* found = strstr(ids, key);
    if (found == nullptr) return;
    const char* value = found + strlen(key);
    size_t n = strcspn(value, ";");
    if (n == 0) return;
    if (n >= bytes) n = bytes - 1;
    memcpy(out, value, n);
    out[n] = '\0';
}

void EscapeInto(char* out, size_t bytes, const char* value) {
    size_t w = 0;
    for (size_t r = 0; value[r] != '\0' && w + 2 < bytes; ++r) {
        const char c = value[r];
        if (c == '"' || c == '\\') {
            out[w++] = '\\';
            out[w++] = c;
        } else if (c == '\n') {
            out[w++] = ' ';
        } else {
            out[w++] = c;
        }
    }
    out[w < bytes ? w : bytes - 1] = '\0';
}

// `why` is for whoever opens done.json: the harness reads the qualification off `undetermined`
// and the reason off `detail`, so both carry it too and neither depends
// on this field. Empty when the verdict is not qualified.
void WriteVerdictFile(bool provisional, const char* verdict, size_t checked, size_t undetermined,
                      const char* why, const char* detail) {
    char root[MAX_PATH];
    if (!ProbeRoot(root, sizeof root)) return;  // not in a sweep; never scatter files into an install
    char dir[MAX_PATH];
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", root, g.stem);
    CreateDirectoryA(dir, nullptr);

    char id[128];
    ProbeId(id, sizeof id);
    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(sizeof cwd, cwd);
    char cwdEscaped[MAX_PATH * 2];
    EscapeInto(cwdEscaped, sizeof cwdEscaped, cwd);
    char detailEscaped[8192];
    EscapeInto(detailEscaped, sizeof detailEscaped, detail);
    char whyEscaped[512];
    EscapeInto(whyEscaped, sizeof whyEscaped, why);

    char path[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\done.json", dir);
    FILE* fh = nullptr;
    if (fopen_s(&fh, path, "wb") != 0 || fh == nullptr) return;
    fprintf(fh,
            "{ \"probe\": \"%s\", \"verdict\": \"%s\", \"checked\": %llu,\n"
            "  \"undetermined\": %llu, \"why\": \"%s\", \"provisional\": %s, \"cwd\": \"%s\",\n"
            "  \"detail\": \"%s\" }\n",
            id, verdict, (unsigned long long)checked, (unsigned long long)undetermined, whyEscaped,
            provisional ? "true" : "false", cwdEscaped, detailEscaped);
    fclose(fh);
}

void FileProvisional() {
    WriteVerdictFile(true, "NOT-RUN", 0, 0, "",
                     "provisional record written from a static initialiser, before DllMain and "
                     "before any module's coexist instance claim. The sampler has not run. If this "
                     "record is still here at the deadline the probe LOADED and was killed "
                     "mid-measurement, which is a different fact from never having loaded.");
}

void Append(char* buf, size_t cap, const char* fmt, ...) {
    const size_t used = strlen(buf);
    if (used + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + used, cap - used, fmt, ap);
    va_end(ap);
}

// ---- the sampler ---------------------------------------------------------------------------------

DWORD WINAPI SamplerMain(LPVOID) {
    // A settle, then a bounded wait for a match. The callback only runs in a loaded session, which
    // is why the descriptor says completesAt: match.
    Sleep(3000);

    const DWORD deadline = GetTickCount() + 150000;
    while (GetTickCount() < deadline) {
        const ui::csregister::Report r = ui::csregister::Status_();
        // Enough invocations to answer the thread question, or enough frames to prove it is not
        // going to happen. Either is a determination.
        if (g.sampleCount >= static_cast<LONG>(kMaxSamples)) break;
        // ★ AUDIT_F3E F3E-2 / ARCH-13 s1.3: THE EARLY BREAK IS STRUCK for starvation.
        // THE WINDOW IS NEVER SHORTENED BY A CONDITION THIS PROBE JUDGES.
        // `Unconfirmed` and `Invoked` are exactly the states check 2 scores -- never dispatched,
        // and dispatched but never drawn -- and their only remedy is more time, so breaking on
        // them cut the 150 s window to about ten seconds and then recorded the short window as a
        // verdict. The two states below are different: both mean the callback DID fire and the
        // panel DID draw and then something stopped, so the answer is already in hand and more
        // time cannot change it. Stale is transient (the next tick re-registers) and is waited
        // through, not broken on.
        if (probeui::SamplerMayBreakOn(r.status, r.ownerDraws) && g.tickCount > 600) break;
        Sleep(200);
    }

    const ui::csregister::Report r = ui::csregister::Status_();
    // AUDIT_F3I F3I-3: the draw check is scored over the panel's OWN open window, not over the
    // module's lifetime. See probe_ui_outcome.h.
    const probeui::DrawWindow w = probeui::DrawWindowSinceEnable(r);

    Check checks[4] = {};
    checks[0].id = "w2f-band3-registration-lands";
    checks[1].id = "w2f-band3-callback-thread";
    checks[2].id = "w2f-clock-ceiling";
    checks[3].id = "offline-bit-lock-held";

    // ---- check 1: did the registration land -----------------------------------------------
    if (g.moduleInitRan == 0) {
        checks[0].outcome = Outcome::Undetermined;
        _snprintf_s(checks[0].detail, sizeof checks[0].detail, _TRUNCATE,
                    "the probe module's Init never ran, so nothing was ever registered. This DLL "
                    "carries a bootstrap, so the most likely cause is coexist::ClaimInstance "
                    "refusing it because another bootstrap-carrying probe loaded first "
                    "(alphabetical). ticks=%ld", g.tickCount);
    } else if (r.status == ui::csregister::Status::SpaceNotReady) {
        // Never FAILED: the registrar was never called, so nothing can be said about the scheme
        // layout. This is the state DIAG-CRASH launch-12 crashed in, and seeing it here with a
        // deferral count is the guard working rather than the registration failing.
        checks[0].outcome = Outcome::Undetermined;
        char table[256];
        tableguard::FormatTable(r.lastRefused, table, sizeof table);
        _snprintf_s(checks[0].detail, sizeof checks[0].detail, _TRUNCATE,
                    "the game space never finished building, so the registration was DEFERRED and "
                    "never attempted: %llu deferral(s) (%llu on an unsorted table), last refusal: "
                    "%s (%s), ticks=%ld",
                    (unsigned long long)r.deferrals, (unsigned long long)r.unsortedDeferrals,
                    ui::csregister::RefusalName(r.lastRefusal), table, g.tickCount);
    } else if (r.status == ui::csregister::Status::NoGameSpace ||
               r.status == ui::csregister::Status::NoComponentSystem) {
        checks[0].outcome = Outcome::Undetermined;
        _snprintf_s(checks[0].detail, sizeof checks[0].detail, _TRUNCATE,
                    "never reached a game space with the component system in it: status=%s ticks=%ld",
                    ui::csregister::StatusName(r.status), g.tickCount);
    } else if (r.registrations == 0) {
        // "Could not ask" is not "asked and failed". Nothing below this point may name the scheme
        // layout as the cause when the registrar was never reached.
        checks[0].outcome = Outcome::Undetermined;
        _snprintf_s(checks[0].detail, sizeof checks[0].detail, _TRUNCATE,
                    "the registrar was never called: status=%s deferrals=%llu ticks=%ld",
                    ui::csregister::StatusName(r.status), (unsigned long long)r.deferrals,
                    g.tickCount);
    } else if (r.status == ui::csregister::Status::Stale) {
        // F3 observed and NOT recovered by the end of the window: its own word, never Ok.
        checks[0].outcome = Outcome::Failed;
        _snprintf_s(checks[0].detail, sizeof checks[0].detail, _TRUNCATE,
                    "STALE at the end of the window: our record left the space's table %llu "
                    "time(s) and the re-registration had not landed (registrations=%u "
                    "deferrals=%llu unsortedDeferrals=%llu spaceChanges=%llu) ticks=%ld",
                    (unsigned long long)r.staleDetected, r.registrations,
                    (unsigned long long)r.deferrals, (unsigned long long)r.unsortedDeferrals,
                    (unsigned long long)r.spaceChanges, g.tickCount);
    } else if (r.status == ui::csregister::Status::Unconfirmed) {
        checks[0].outcome = Outcome::Failed;
        _snprintf_s(checks[0].detail, sizeof checks[0].detail, _TRUNCATE,
                    "UNCONFIRMED: the record landed (%llu -> %llu) and is still in the table, and "
                    "nothing dispatched it for %llu tick(s); staleDetected=%llu ticks=%ld",
                    (unsigned long long)r.recordsBefore, (unsigned long long)r.recordsAfter,
                    (unsigned long long)r.unconfirmedTicks, (unsigned long long)r.staleDetected,
                    g.tickCount);
    } else if (r.recordsAfter > r.recordsBefore) {
        // Reaching here with staleDetected > 0 IS the recovery: the two branches above have already
        // taken Stale-at-the-end and Unconfirmed. It passes, and it carries a finding -- the
        // allocator reused the block under a live registration, and what happened in the window
        // between the record vanishing and the re-registration landing is not knowable from here.
        checks[0].outcome = r.staleDetected > 0 ? Outcome::OkQualified : Outcome::Ok;
        if (r.staleDetected > 0) {
            _snprintf_s(g.why, sizeof g.why, _TRUNCATE,
                        "stale re-registration recovered (staleDetected=%llu spaceChanges=%llu)",
                        (unsigned long long)r.staleDetected, (unsigned long long)r.spaceChanges);
        }
        _snprintf_s(checks[0].detail, sizeof checks[0].detail, _TRUNCATE,
                    "%sscheme+0x610 moved %llu -> %llu over %u registration(s) after deferring "
                    "%llu tick(s) for an unbuilt game space; status=%s staleDetected=%llu "
                    "spaceChanges=%llu",
                    r.staleDetected > 0 ? "PASS-WITH-STALE: the space was rebuilt under our "
                                          "registration and the re-registration recovered it; "
                      : "",
                    (unsigned long long)r.recordsBefore, (unsigned long long)r.recordsAfter,
                    r.registrations, (unsigned long long)r.deferralsBeforeFirst,
                    ui::csregister::StatusName(r.status), (unsigned long long)r.staleDetected,
                    (unsigned long long)r.spaceChanges);
    } else {
        checks[0].outcome = Outcome::Failed;
        _snprintf_s(checks[0].detail, sizeof checks[0].detail, _TRUNCATE,
                    "the registrar was called and scheme+0x610 did NOT move (%llu -> %llu). It has "
                    "no failure path, so this means the scheme layout is not what facts.tsv "
                    "describes. status=%s",
                    (unsigned long long)r.recordsBefore, (unsigned long long)r.recordsAfter,
                    ui::csregister::StatusName(r.status));
    }

    // ---- check 2: did it fire, and on which thread ------------------------------------------
    if (checks[0].outcome != Outcome::Ok && checks[0].outcome != Outcome::OkQualified) {
        checks[1].outcome = Outcome::Undetermined;
        _snprintf_s(checks[1].detail, sizeof checks[1].detail, _TRUNCATE,
                    "nothing was registered, so there was no callback to observe");
    } else if (r.invocations == 0) {
        // ★ THE DETERMINATION THE COORDINATOR ASKED FOR. A registration that landed and never fired
        // is not a failed read: it is positive evidence about the band system, and it narrows to
        // one of two documented traps.
        checks[1].outcome = Outcome::Failed;
        _snprintf_s(checks[1].detail, sizeof checks[1].detail, _TRUNCATE,
                    "REGISTERED AND SKIPPED: scheme+0x610 moved %llu -> %llu so the record landed, "
                    "and the callback was entered ZERO times over %ld ticks. The order key used was "
                    "0x%04X, which IS inside band 3 (0x6000-0x7FFE), so the band trap is excluded "
                    "and bit 23 of *(u32*)(game+0x828) skipping the whole stage-0..3 pass is the "
                    "remaining explanation. This is the outcome that would invert the panel's draw "
                    "route.",
                    (unsigned long long)r.recordsBefore, (unsigned long long)r.recordsAfter,
                    g.tickCount, ui::csregister::kOrderKey);
    } else if (!w.valid) {
        // No window to score. Either the panel was never opened -- nothing was ever asked of
        // the draw -- or a counter went backwards across the stamp, which is ARCH-13 s2.3
        // item 4: the module was reset under the observer. Neither is a failure of the panel.
        checks[1].outcome = Outcome::Undetermined;
        _snprintf_s(checks[1].detail, sizeof checks[1].detail, _TRUNCATE,
                    "NO MEASUREMENT WINDOW: %s. Lifetime totals were ownerTicks=%llu "
                    "draws=%llu against a baseline of ownerTicksAtEnable=%llu "
                    "drawsAtEnable=%llu over %ld probe tick(s), and a window that does not "
                    "exist cannot be passed or failed.",
                    w.everOpened ? "a counter went BACKWARDS across the enable stamp, so the "
                                   "module was reset under the observer"
                                 : "the panel was never opened, so csregister::Enable() never "
                                   "ran and no frame was ever owed",
                    (unsigned long long)r.ownerTicks, (unsigned long long)r.ownerDraws,
                    (unsigned long long)r.ownerTicksAtEnable,
                    (unsigned long long)r.ownerDrawsAtEnable, g.tickCount);
    } else if (probeui::DrawWindowIsCensored(w.draws, w.ownerTicks)) {
        // ★ DESIGN S8.5.4a CONSEQUENCE 3, IMPLEMENTED (AUDIT_F3E F3E-2). A launch that ends before
        // any draw is COULD-NOT-ASK and never FAIL: `marshalledDraws == 0` over a window that never
        // reached a drawable frame is a censored measurement, and the missing leg is named. The
        // panel is enabled BY the registration, which happens inside an owner tick, and that tick's
        // own drain runs before it -- so the first tick that can draw is the second one, and fewer
        // than two owner ticks means the window never offered a frame to draw in.
        checks[1].outcome = Outcome::Undetermined;
        _snprintf_s(checks[1].detail, sizeof checks[1].detail, _TRUNCATE,
                    "CENSORED, NOT A FAILURE: the callback was entered %llu time(s) (%llu off the "
                    "owner thread %lu) and our stage-0 tick ran %llu time(s) on the owner thread, "
                    "so this window never reached a frame the panel could be drawn in. The missing "
                    "leg of S8.5.4a's Running is marshalledDraws > 0, and over %llu owner tick(s) "
                    "since the panel opened that is a lower bound (>= 0 draws in >= %ld probe "
                    "ticks), not a verdict.",
                    (unsigned long long)r.invocations, (unsigned long long)r.offThreadInvocations,
                    (unsigned long)r.lastCallbackThreadId, (unsigned long long)w.ownerTicks,
                    (unsigned long long)w.ownerTicks, g.tickCount);
    } else if (w.draws == 0) {
        // ★ REWRITTEN FOR THE MARSHAL (L5-RUN-3 F6). An OFF-THREAD dispatch is no longer a failure
        // and must not be scored as one: the callback records and returns on whatever thread the
        // runner chose, and the owner thread runs the draw. What fails is the marshal not landing --
        // and with the render-cadence invariant (F3E-4) the panel is drawn on EVERY owner tick
        // while enabled, so owner ticks WITHOUT draws is the drain refusing, not a quiet dispatcher.
        checks[1].outcome = Outcome::Failed;
        _snprintf_s(checks[1].detail, sizeof checks[1].detail, _TRUNCATE,
                    "MARSHAL DID NOT LAND: the callback was entered %llu time(s) (%llu of them off "
                    "the owner thread %lu, last on thread %lu) and the owner thread ran ZERO "
                    "marshalled draw(s) over the %llu owner tick(s) since the panel opened / %ld "
                    "probe ticks, with %llu "
                    "entry(s) still undrained. The panel is drawn on every owner tick while "
                    "enabled and the callback draws nothing by design -- ImGui is not thread-safe "
                    "-- so this is the owner side failing to draw, not the band-3 registration "
                    "failing.",
                    (unsigned long long)r.invocations, (unsigned long long)r.offThreadInvocations,
                    (unsigned long)r.mainThreadId, (unsigned long)r.lastCallbackThreadId,
                    (unsigned long long)w.ownerTicks, g.tickCount,
                    (unsigned long long)r.pendingInvocations);
    } else if (!probeui::DrawCoverageHolds(w.draws, w.ownerTicks)) {
        // ★ ARCH-13 s2.3 item 2. A MIDDLING RATIO IS AN ASKED-FAIL, NOT A PASS: existence is not
        // coverage, and one draw in four thousand ticks is the flickering panel the render-cadence
        // ruling exists to forbid. Under that ruling the drain no longer skips ticks, so a
        // shortfall means the draw itself refused -- a degraded gui entry point, or a Disable()
        // mid-window -- and that is a finding, not a quiet pass.
        checks[1].outcome = Outcome::Failed;
        _snprintf_s(checks[1].detail, sizeof checks[1].detail, _TRUNCATE,
                    "THE PANEL IS MISSING FROM MOST FRAMES: %llu draw(s) over %llu owner tick(s) "
                    "SINCE THE PANEL OPENED (lifetime %llu/%llu, baseline %llu/%llu; coverage "
                    "requires draws >= ownerTicks - 1, so %llu are unaccounted for), "
                    "with %llu callback entry(s) and %llu undrained. The panel is drawn on every "
                    "owner tick while enabled, so the shortfall is the draw refusing, not the "
                    "band-3 dispatcher being quiet.",
                    (unsigned long long)w.draws, (unsigned long long)w.ownerTicks,
                    (unsigned long long)r.ownerDraws, (unsigned long long)r.ownerTicks,
                    (unsigned long long)r.ownerDrawsAtEnable,
                    (unsigned long long)r.ownerTicksAtEnable,
                    (unsigned long long)(w.ownerTicks - w.draws - 1),
                    (unsigned long long)r.invocations,
                    (unsigned long long)r.pendingInvocations);
    } else if (r.mailboxTornReads > 0 || r.wrongThreadDrains > 0) {
        // Two things that must be zero by construction. A non-zero either way means the mailbox
        // or the owner-thread guard is not what this code believes, and that is worth its own
        // loud outcome rather than a pass with a number nobody reads.
        checks[1].outcome = Outcome::Failed;
        _snprintf_s(checks[1].detail, sizeof checks[1].detail, _TRUNCATE,
                    "MAILBOX INVARIANT BROKEN: mailboxTornReads=%llu wrongThreadDrains=%llu over "
                    "%llu invocation(s) and %llu marshalled draw(s). Both must be zero: a read "
                    "never sees a half-written record, and the draw path is never entered off the "
                    "owner thread %lu.",
                    (unsigned long long)r.mailboxTornReads,
                    (unsigned long long)r.wrongThreadDrains, (unsigned long long)r.invocations,
                    (unsigned long long)r.ownerDraws, (unsigned long)r.mainThreadId);
    } else if (r.registrationInvocations == 0) {
        // The case the lifetime total used to hide (AUDIT_W2-F_R2 R2-2): every main-thread dispatch
        // arrived from a component system we are NOT registered with. Either a game space we have
        // left is still running our record -- registration is one-way -- or opaqueThis is not
        // rec.cs and the attribution is itself wrong. The panel drew either way, so this is a
        // finding about the invariant rather than about the UI, and both readings are named.
        checks[1].outcome = Outcome::Failed;
        _snprintf_s(checks[1].detail, sizeof checks[1].detail, _TRUNCATE,
                    "DISPATCHED, BUT NOT BY OUR REGISTRATION: %llu main-thread invocation(s), of "
                    "which %llu carried an opaqueThis that is not the component system we "
                    "registered with (last %p, ours %p), and ZERO belong to the current "
                    "registration over %ld ticks. Either a game space we have left is still "
                    "dispatching our record, or the record's this-adjustor is not zero so "
                    "opaqueThis != rec.cs.",
                    (unsigned long long)r.invocations, (unsigned long long)r.staleInvocations,
                    r.lastStaleDispatch, r.componentSystem, g.tickCount);
    } else {
        checks[1].outcome = Outcome::Ok;
        _snprintf_s(checks[1].detail, sizeof checks[1].detail, _TRUNCATE,
                    "the callback ran %llu time(s) (%llu off the owner thread %lu, last on thread "
                    "%lu) and the owner thread drew the panel %llu time(s) over %llu owner tick(s) "
                    "[F3E#2: draws/ownerTicks, 1.0 means every frame carried the panel], with %llu "
                    "entries drained, %llu coalesced and %llu undrained; %llu belong to the current "
                    "registration and %llu arrived from a component system we are no longer "
                    "registered with. S8.5.4a-three-part Running=%s. status=%s",
                    (unsigned long long)r.invocations,
                    (unsigned long long)r.offThreadInvocations, (unsigned long)r.mainThreadId,
                    (unsigned long)r.lastCallbackThreadId,
                    (unsigned long long)r.marshalledDraws,
                    (unsigned long long)r.ownerTicks,
                    (unsigned long long)r.drainedInvocations,
                    (unsigned long long)r.coalescedInvocations,
                    (unsigned long long)r.pendingInvocations,
                    (unsigned long long)r.registrationInvocations,
                    (unsigned long long)r.staleInvocations, r.runningPerDesign ? "yes" : "no",
                    ui::csregister::StatusName(r.status));
    }

    // ---- check 3: the live clock ceiling ------------------------------------------------------
    if (!g.clockRead) {
        checks[2].outcome = Outcome::Undetermined;
        _snprintf_s(checks[2].detail, sizeof checks[2].detail, _TRUNCATE,
                    "the global clock pointer was never non-null, so +0x60 was not read");
    } else {
        const double meanFrame =
            g.frameDeltaCount > 0
                ? static_cast<double>(g.frameDeltaSumUs) / static_cast<double>(g.frameDeltaCount)
                : 0.0;
        checks[2].outcome = Outcome::Ok;
        _snprintf_s(checks[2].detail, sizeof checks[2].detail, _TRUNCATE,
                    "clock+0x60 = %u us (raw), mean frameDeltaUs = %.0f over %llu ticks, so the "
                    "fast-forward ceiling on this machine is %.2fx. The panel currently falls back "
                    "to the CLOCK.MAXELAPSED_US constant when it has not measured one.",
                    g.clockMaxElapsedUs, meanFrame, (unsigned long long)g.frameDeltaCount,
                    meanFrame > 0.0 ? static_cast<double>(g.clockMaxElapsedUs) / meanFrame : 0.0);
    }

    // ---- check 4: is the guard this descriptor declares actually held -------------------------
    //
    // ★ THE DECLARATION MUST NOT BE ABLE TO OUTLIVE THE MECHANISM. Calling bootstrap::Attach is
    // necessary and NOT sufficient: Attach returns false at coexist::ClaimInstance before it ever
    // reaches CreateThread(WorkerMain), so a probe that loses the instance claim in a shared
    // plugins folder installs no lock at all. Promising "we install it" would therefore be a
    // promise this probe cannot keep.
    //
    // So it does not promise. offlinelock::Current() reads the instrument through SHARED MEMORY --
    // which is why that instrument is shared -- so this is a statement about the PROCESS and stays
    // true when another module won the race. FAILED, not COULD-NOT-DETERMINE: an -offline launch
    // whose network-safety property was not actually installed is a finding, not an absent read.
    {
        // Apply() runs on the bootstrap worker after the build guard accepts, so it is not resolved
        // the instant DllMain returns. Bounded, and only while the answer can still change: with no
        // worker there is nothing to wait for and the wait would be thirty seconds of nothing.
        const DWORD lockDeadline = GetTickCount() + 30000;
        while (InterlockedCompareExchange(&g_bootstrapOk, 0, 0) != 0 &&
               offlinelock::Current().state == offlinelock::LockState::NotAttempted &&
               GetTickCount() < lockDeadline) {
            Sleep(250);
        }

        const offlinelock::Report& lock = offlinelock::Current();
        const bool attachOk = InterlockedCompareExchange(&g_bootstrapOk, 0, 0) != 0;
        checks[3].outcome = uiprobe::OfflineLockHeld(lock.state) ? Outcome::Ok : Outcome::Failed;
        _snprintf_s(checks[3].detail, sizeof checks[3].detail, _TRUNCATE,
                    "state=%s (bootstrap::Attach %s, installed by this module %s, sites %u of %u, "
                    "calls %llu, clears prevented %llu, bit already clear %llu, exercise %s, "
                    "instrument shared %s). This probe runs on offline-local, so the whole "
                    "no-network property rests on this bit; a launch that ran without it was "
                    "protected by a configuration rather than by an invariant.",
                    offlinelock::LockStateName(lock.state), attachOk ? "accepted" : "REFUSED",
                    lock.installedHere ? "yes" : "no", lock.sitesRecorded, lock.sitesExpected,
                    (unsigned long long)lock.calls, (unsigned long long)lock.clearsPrevented,
                    (unsigned long long)lock.callsBitAlreadyClear,
                    offlinelock::ExerciseName(offlinelock::Exercised()),
                    lock.instrumentShared ? "yes" : "no");
    }

    // ---- fold ---------------------------------------------------------------------------------
    size_t ok = 0, failed = 0, undetermined = 0, qualified = 0;
    char detail[8192] = {0};
    for (const Check& c : checks) {
        const char* name = c.outcome == Outcome::Ok            ? "OK"
                           : c.outcome == Outcome::OkQualified ? "OK-WITH-FINDING"
                           : c.outcome == Outcome::Failed      ? "FAILED"
                                                               : "COULD-NOT-DETERMINE";
        if (c.outcome == Outcome::Ok) {
            ++ok;
        } else if (c.outcome == Outcome::OkQualified) {
            ++ok;
            ++qualified;
        } else if (c.outcome == Outcome::Failed) {
            ++failed;
        } else {
            ++undetermined;
        }
        Append(detail, sizeof detail, "[%s] %s: %s | ", name, c.id, c.detail);
    }

    // csregister::Status verbatim, which is the whole point: three causes that otherwise present
    // identically as "my callback never fired". The space-change tables carry their verdicts, so a
    // header we could not read is not printed as a table full of zeroes (R2-3).
    char csChangeTable[256];
    char recChangeTable[256];
    tableguard::FormatTable(r.lastSpaceChange.cs, csChangeTable, sizeof csChangeTable);
    tableguard::FormatTable(r.lastSpaceChange.rec, recChangeTable, sizeof recChangeTable);
    Append(detail, sizeof detail,
           "csregister::Status=%s recordsBefore=%llu recordsAfter=%llu registrations=%u "
           "invocations=%llu regInvocations=%llu staleInvocations=%llu lastStaleThis=%p cs=%p "
           "offThread=%llu tickThread=%lu lastCallbackThread=%lu ticks=%ld "
           "staleDetected=%llu headerMoved=%llu unsortedDeferrals=%llu unconfirmedTicks=%llu "
           "spaceChanges=%llu lastSpaceChange={space=%p cs(%s) rec(%s) ours=%lld inv=%llu} "
           "window={enableStamps=%llu ownerTicks=%llu/%llu ownerDraws=%llu/%llu since=%llu/%llu "
           "marshalledDraws=%llu} "
           "inline={draws=%llu deferrals=%llu unfilled=%llu} "
           "diagnostic=\"%s\" | ",
           ui::csregister::StatusName(r.status), (unsigned long long)r.recordsBefore,
           (unsigned long long)r.recordsAfter, r.registrations,
           (unsigned long long)r.invocations, (unsigned long long)r.registrationInvocations,
           (unsigned long long)r.staleInvocations, r.lastStaleDispatch, r.componentSystem,
           (unsigned long long)r.offThreadInvocations,
           (unsigned long)r.mainThreadId, (unsigned long)r.lastCallbackThreadId, g.tickCount,
           (unsigned long long)r.staleDetected, (unsigned long long)r.headerMoved,
           (unsigned long long)r.unsortedDeferrals, (unsigned long long)r.unconfirmedTicks,
           (unsigned long long)r.spaceChanges, r.lastSpaceChange.gameSpace, csChangeTable,
           recChangeTable, (long long)r.lastSpaceChange.ourIndex,
           (unsigned long long)r.lastSpaceChange.invocationsAtChange,
           (unsigned long long)r.enableStamps, (unsigned long long)r.ownerTicks,
           (unsigned long long)r.ownerTicksAtEnable, (unsigned long long)r.ownerDraws,
           (unsigned long long)r.ownerDrawsAtEnable, (unsigned long long)w.draws,
           (unsigned long long)w.ownerTicks, (unsigned long long)r.marshalledDraws,
           (unsigned long long)r.inlineDraws,
           (unsigned long long)r.inlineDeferrals,
           (unsigned long long)r.inlineDeferralsUnfilled, ui::csregister::Diagnostic());

    // The raw per-invocation samples the payload asked for, rather than a summary of them.
    const LONG n = g.sampleCount < static_cast<LONG>(kMaxSamples) ? g.sampleCount
                                                                  : static_cast<LONG>(kMaxSamples);
    Append(detail, sizeof detail, "samples(%ld)=", n);
    for (LONG i = 0; i < n && i < 24; ++i) {
        Append(detail, sizeof detail, "{tid=%lu,flag=0x%08X,frame=%u}",
               (unsigned long)g.samples[i].threadId, g.samples[i].dispatchFlag,
               g.samples[i].frameIndex);
    }

    // The verdict word is decided by the questions that WERE asked; a qualified OK answered its
    // own question, so it neither fails nor un-runs the record. The qualification travels in the
    // reported `undetermined` count instead, which is the field Read-ProbeVerdict turns into a
    // Qualified row -- out of the clean-pass column, never out of a FAIL.
    const char* verdict = failed != 0 ? "FAIL" : (undetermined != 0 ? "NOT-RUN" : "PASS");
    WriteVerdictFile(false, verdict, ok + failed, undetermined + qualified, g.why, detail);
    return 0;
}

void StartSampler() {
    const HANDLE th = CreateThread(nullptr, 0, SamplerMain, nullptr, 0, nullptr);
    if (th != nullptr) CloseHandle(th);
}

void PinSelfAndNameIt() {
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&StartSampler), &g.self);
    char dll[MAX_PATH] = {0};
    GetModuleFileNameA(g.self, dll, sizeof dll);
    const char* leaf = strrchr(dll, '\\');
    leaf = leaf != nullptr ? leaf + 1 : dll;
    strncpy_s(g.stem, sizeof g.stem, leaf, _TRUNCATE);
    if (char* dot = strrchr(g.stem, '.')) *dot = '\0';
}

// ★ The static initialiser: pin, learn our own name, file the provisional record. It starts no
// thread and waits for nothing -- this runs under the Windows loader lock.
struct EarlyFiling {
    EarlyFiling();
};

EarlyFiling::EarlyFiling() {
    PinSelfAndNameIt();
    FileProvisional();
}

const EarlyFiling g_earlyFiling;

REPLAY_MODULE("w2f-probe", 700, ProbeInit, ProbeTick, nullptr);

}  // namespace
}  // namespace replay

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            // ★ FIRST, AND IT IS TWO THINGS AT ONCE.
            //
            // The safety half: this descriptor declares the offline-bit-lock guard, and
            // offlinelock::Apply() runs only from WorkerMain, which only bootstrap::Attach starts.
            // Linking replay_bootstrap put every one of those sources in this binary and reached
            // none of them, so the launch would have run -offline past the login window on a
            // promise. This is the call that turns the declaration into a mechanism.
            //
            // The measurement half, found while fixing the first: modreg::InitAll has exactly one
            // caller and it is inside the same WorkerMain. Without this line ProbeInit never ran,
            // ProbeTick never ran, csregister never registered, and all three checks below could
            // only ever report COULD-NOT-DETERMINE. The probe could not answer its own questions.
            //
            // Reported, not obeyed: a refused instance claim is a normal outcome in a shared
            // plugins folder and is not a reason to stop measuring. The verdict says which it was.
            InterlockedExchange(&replay::g_bootstrapOk, replay::bootstrap::Attach(self) ? 1 : 0);
            // Created here rather than in the static initialiser: CreateThread under the loader lock
            // is legal only because we never wait on the result, and DllMain is where that is
            // visible.
            replay::StartSampler();
            break;
        case DLL_PROCESS_DETACH:
            replay::bootstrap::Detach(reserved != nullptr);
            break;
        default:
            break;
    }
    return TRUE;
}
