// W1-C's offline-local probe: the launch where the lock can actually be tested.
//
// ★ WHY A SECOND PROBE AND NOT A SECOND DESCRIPTOR ON THE FIRST. probe_bootstrap reports at ATTACH,
// inside the ~8 s window, which is correct for what it answers - the build guard's ACCEPT path happens
// at WinMain statement 2 and nothing later changes it. The lock is the opposite shape. `bare` never
// loads a match, so the menu-script path that clears the offline bit is unreachable there and the
// Exercise verdict could only ever read `not-exercised`. Reporting the lock from the attach probe would
// therefore publish a number that is structurally incapable of being anything else - the same defect the
// audit named in firstSocketAfterMs, rebuilt one level up.
//
// This probe runs on `offline-local`, reports at MATCH, and samples over the life of the session, so
// `held-under-exercise` and `not-exercised` are both reachable outcomes of the same code.
//
// ★ WHAT IT DOES NOT DO. It does not assert that the teardown ran. Whether a menu script node fires is a
// property of what the session did, not of our correctness, and asserting it would turn a quiet match
// into a failed lock. The verdict is PASS if the lock is installed and its self-test discriminated;
// the Exercise value is REPORTED beside it as a separate fact.

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bootstrap/bootstrap.h"
#include "bootstrap/buildguard.h"
#include "bootstrap/egress.h"
#include "bootstrap/log.h"
#include "bootstrap/offlinelock.h"
#include "offlinelock_report.h"
#include "generated/facts_generated.h"

namespace {

const char* const kModuleName = "w1c-offlinelock";
const char* const kProbeId = "W1-C-offlinelock";

// The session has to get far enough to load a level before any of this means anything. The probe files
// a real verdict at the deadline whatever phase it reached, so it never holds the launch open.
constexpr DWORD kObserveMs = 150000;
constexpr DWORD kSampleIntervalMs = 250;

using w1c::kMaxSeries;
using w1c::Reads;

Reads g_reads;
volatile LONG g_reported;

// ★ EVERY fact VA goes through this, and the first run of this probe is why.
//
// echovr.exe sets DYNAMICBASE and HIGH_ENTROPY_VA, so the loader relocates it on every launch. The
// addresses in the registry are PREFERRED-base: facts::CLOCK_GLOBAL_PTR is 0x1420A00E8 against an
// ImageBase of 0x140000000, and dereferencing it raw reads whatever is at that address in a relocated
// process - nothing, in the first launch, so all three clock reads reported "not seen" and looked like
// the engine had not built the objects yet.
//
// The offline-bit lock was unaffected because it scans the MAPPED .text through GetModuleHandleW(null)
// and never names an address, which is the same reason it found its two sites on the first attempt.
uint64_t Rebased(uint64_t preferredVa) {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (base == nullptr) {
        return preferredVa;
    }
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return preferredVa;
    }
    const IMAGE_NT_HEADERS64* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return preferredVa;
    }
    return preferredVa - nt->OptionalHeader.ImageBase + reinterpret_cast<uint64_t>(base);
}

// Recorded, so a wrong base is visible in the artifact instead of being inferred from a failed read.
// If moduleBase equals imageBase the loader did not relocate us, and the rebase was a no-op - which is
// worth knowing before anyone concludes the rebase is what fixed or failed to fix anything.
void RecordBases() {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (base == nullptr) {
        return;
    }
    g_reads.moduleBase = reinterpret_cast<uint64_t>(base);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    const IMAGE_NT_HEADERS64* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature == IMAGE_NT_SIGNATURE) {
        g_reads.imageBase = nt->OptionalHeader.ImageBase;
    }
}

// Finds or creates the record for one named read. The LAST attempt wins, so the artifact shows the
// state at the end of the window rather than the first thing that happened.
w1c::ReadSite* SiteFor(const char* name) {
    for (size_t i = 0; i < g_reads.siteCount; ++i) {
        if (std::strcmp(g_reads.sites[i].name, name) == 0) {
            return &g_reads.sites[i];
        }
    }
    if (g_reads.siteCount >= w1c::kMaxReadSites) {
        return nullptr;
    }
    w1c::ReadSite* s = &g_reads.sites[g_reads.siteCount++];
    std::snprintf(s->name, sizeof s->name, "%s", name);
    return s;
}

// ★ Every read below goes through this, and it records WHY it did not read rather than returning a
// boolean that means two opposite things. `preferredVa` is 0 for a derived address - one computed from
// a pointer we already read - because only registry addresses are rebased.
bool ProbeQword(const char* name, uint64_t absoluteVa, uint64_t preferredVa, uint64_t* out) {
    w1c::ReadSite* site = SiteFor(name);
    if (site != nullptr) {
        site->preferredVa = preferredVa;
        site->absoluteVa = absoluteVa;
        ++site->attempts;
        site->outcome = w1c::ReadOutcome::NotAttempted;
        site->value = 0;
    }
    const void* p = reinterpret_cast<const void*>(absoluteVa);
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof mbi) == 0) {
        if (site != nullptr) site->outcome = w1c::ReadOutcome::QueryFailed;
        return false;
    }
    if (site != nullptr) {
        site->state = mbi.State;
        site->protect = mbi.Protect;
        site->regionSize = mbi.RegionSize;
    }
    if (mbi.State != MEM_COMMIT) {
        if (site != nullptr) site->outcome = w1c::ReadOutcome::NotCommitted;
        return false;
    }
    if ((mbi.Protect & PAGE_GUARD) != 0) {
        if (site != nullptr) site->outcome = w1c::ReadOutcome::GuardPage;
        return false;
    }
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & readable) == 0) {
        if (site != nullptr) site->outcome = w1c::ReadOutcome::NotReadable;
        return false;
    }
    uint64_t value = 0;
    std::memcpy(&value, p, sizeof value);
    *out = value;
    if (site != nullptr) {
        site->value = value;
        // ★ The distinction that cost three launches: a page we read cleanly whose contents are zero
        // is the engine not having built the object, and is NOT the same as a refusal.
        site->outcome = (value == 0) ? w1c::ReadOutcome::ReadNull : w1c::ReadOutcome::ReadOk;
    }
    // True means "the read happened". Whether a zero is meaningful is the caller's chain to judge.
    return true;
}

// The global CClock. Its min and max elapsed fields are set when the clock is constructed and do not
// move afterwards, so one late sample is the honest form for those two.
void SampleGlobalClock(uint64_t nowMs) {
    if (g_reads.clockSeen) {
        return;
    }
    uint64_t clock = 0;
    if (!ProbeQword("clock.global", Rebased(facts::CLOCK_GLOBAL_PTR), facts::CLOCK_GLOBAL_PTR,
                    &clock) ||
        clock == 0) {
        return;
    }
    g_reads.clockSeen = true;
    g_reads.clockPtr = clock;
    g_reads.firstSeenAtMs = nowMs;
    g_reads.minElapsedRead = ProbeQword("clock.minElapsed", clock + facts::CLOCK_MINELAPSED_OFF, 0,
                                        &g_reads.minElapsedUs);
    g_reads.maxElapsedRead = ProbeQword("clock.maxElapsed", clock + facts::CLOCK_MAXELAPSED_OFF, 0,
                                        &g_reads.maxElapsedUs);
}

// Per-frame, so it is bracketed rather than kept: the ceiling calculation needs the frame period's
// range, not its shape.
void SampleFrameDelta() {
    if (!g_reads.clockSeen) {
        return;
    }
    uint64_t delta = 0;
    if (!ProbeQword("clock.scaledDelta", g_reads.clockPtr + facts::CLOCK_SCALEDDELTA_OFF, 0,
                    &delta)) {
        return;
    }
    if (!g_reads.deltaSeen) {
        g_reads.deltaSeen = true;
        g_reads.deltaMinUs = delta;
        g_reads.deltaMaxUs = delta;
    }
    if (delta < g_reads.deltaMinUs) g_reads.deltaMinUs = delta;
    if (delta > g_reads.deltaMaxUs) g_reads.deltaMaxUs = delta;
    g_reads.deltaLastUs = delta;
    ++g_reads.deltaSamples;
}

// The game clock's timescale is LIVE - it is what slow motion and pause move - so it is sampled as a
// series rather than read once.
void SampleTimescale(uint64_t nowMs) {
    uint64_t game = 0;
    if (!ProbeQword("game.singleton", Rebased(facts::GAME_SINGLETON_VA), facts::GAME_SINGLETON_VA,
                    &game) ||
        game == 0) {
        return;
    }
    g_reads.gameSeen = true;
    uint64_t gameClock = 0;
    if (!ProbeQword("game.clockPtr", game + facts::GAME_CLOCK_PTR_OFF, 0, &gameClock) ||
        gameClock == 0) {
        return;
    }
    g_reads.gameClockSeen = true;

    const void* at = reinterpret_cast<const void*>(gameClock + facts::CLOCK_TIMESCALE_OFF);
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(at, &mbi, sizeof mbi) == 0 || mbi.State != MEM_COMMIT) {
        return;
    }
    float value = 0.0f;
    std::memcpy(&value, at, sizeof value);

    // Only a CHANGE earns a slot. A series of identical samples spends the budget proving the clock was
    // idle and then has nothing left for the moment it moved.
    if (g_reads.seriesLen != 0 && g_reads.series[g_reads.seriesLen - 1].value == value) {
        return;
    }
    if (g_reads.seriesLen >= kMaxSeries) {
        ++g_reads.seriesDropped;
        return;
    }
    g_reads.series[g_reads.seriesLen].atMs = nowMs;
    g_reads.series[g_reads.seriesLen].value = value;
    ++g_reads.seriesLen;
}

// ★ The lobby capacity read. `lobby = *(QWORD*)(netgame + NETGAME_LOBBY_OFF) + 24`, then the two counts
// at candidate offsets. Sampled rather than read once: these offsets are PROPOSED, and a number that
// looks plausible at one instant and moves like a member count over a session is evidence, while the
// same number read once is a guess with a timestamp on it.
void SampleLobby(uint64_t nowMs) {
    uint64_t game = 0;
    if (!ProbeQword("game.singleton", Rebased(facts::GAME_SINGLETON_VA), facts::GAME_SINGLETON_VA,
                    &game) ||
        game == 0) {
        return;
    }
    uint64_t netgame = 0;
    if (!ProbeQword("game.netGamePtr", game + facts::GAME_NETGAME_PTR_OFF, 0, &netgame) ||
        netgame == 0) {
        return;
    }
    uint64_t lobbyBase = 0;
    if (!ProbeQword("netgame.lobbyPtr", netgame + facts::NETGAME_LOBBY_OFF, 0, &lobbyBase) ||
        lobbyBase == 0) {
        return;
    }
    // The +24 is part of the read as the registry states it, not an offset of ours.
    const uint64_t lobby = lobbyBase + 24;
    uint64_t members = 0;
    uint64_t conns = 0;
    if (!ProbeQword("lobby.memberCount", lobby + facts::LOBBY_MEMBER_COUNT_OFF, 0,
                    &members) ||
        !ProbeQword("lobby.connCount", lobby + facts::LOBBY_CONN_COUNT_OFF, 0, &conns)) {
        return;
    }
    if (!g_reads.lobbySeen) {
        g_reads.lobbySeen = true;
        g_reads.lobbyPtr = lobby;
        g_reads.lobbyFirstSeenAtMs = nowMs;
        g_reads.memberCountFirst = members;
        g_reads.connCountFirst = conns;
    } else {
        if (members != g_reads.memberCountLast) ++g_reads.memberCountChanges;
        if (conns != g_reads.connCountLast) ++g_reads.connCountChanges;
    }
    g_reads.memberCountLast = members;
    g_reads.connCountLast = conns;
    if (members > g_reads.memberCountMax) g_reads.memberCountMax = members;
    if (conns > g_reads.connCountMax) g_reads.connCountMax = conns;
    ++g_reads.lobbySamples;
}

// ★ The blocker's destinations, by name. A refused COUNT says the offline path is contained; only this
// list says what it was reaching for, which is what layer 1 needs in order to be extended.
void GatherEgress(w1c::EgressView* out) {
    const replay::egress::Report& eg = replay::egress::Current();
    out->reported = true;
    std::snprintf(out->state, sizeof out->state, "%s", replay::egress::ArmStateName(eg.state));
    std::snprintf(out->trigger, sizeof out->trigger, "%s",
                  replay::egress::ArmTriggerName(eg.trigger));
    std::snprintf(out->hooks, sizeof out->hooks, "%s", replay::egress::HookNamesJson());
    out->armedAtMs = eg.armedAtMs;
    out->armDurationMs = eg.armDurationMs;
    out->hooksLive = eg.hooksLive;
    out->hooksRequested = eg.hooksRequested;
    out->refused = eg.refusedRemote;
    out->allowedLocal = eg.allowedLocal;
    out->distinct = eg.distinctDestinations;
    out->overflow = eg.destinationOverflow;
    out->extensionsRefused = eg.extensionsRefused;
    out->socketsCreated = eg.socketsCreated;
    out->destCount = 0;
    for (size_t i = 0; i < w1c::kMaxDestinations; ++i) {
        const char* text = nullptr;
        const char* via = nullptr;
        uint64_t count = 0;
        bool local = false;
        if (!replay::egress::DestinationAt(i, &text, &via, &count, &local)) {
            break;
        }
        w1c::Destination& d = out->dest[out->destCount];
        std::snprintf(d.text, sizeof d.text, "%s", text != nullptr ? text : "(null)");
        std::snprintf(d.via, sizeof d.via, "%s", via != nullptr ? via : "(null)");
        d.count = count;
        d.local = local;
        ++out->destCount;
    }
}

bool WriteDone(const char* verdict, const char* note, unsigned checked, unsigned failed,
               const w1c::EgressView& eg) {
    char root[MAX_PATH] = {};
    if (GetEnvironmentVariableA("REPLAY_PROBE_ROOT", root, sizeof root) == 0) {
        return false;
    }
    char dir[MAX_PATH];
    std::snprintf(dir, sizeof dir, "%s\\probe_offlinelock", root);
    CreateDirectoryA(root, nullptr);
    CreateDirectoryA(dir, nullptr);
    char path[MAX_PATH];
    std::snprintf(path, sizeof path, "%s\\done.json", dir);

    // Sized for the whole destination table rather than for the common case: 128 destinations with a
    // 64-byte host and a 24-byte API each is the worst this can be asked to hold, and a report that
    // drops the tail of the list is a report that cannot answer what the path was reaching for.
    static char body[32768];
    const size_t len = w1c::BuildDoneJson(body, sizeof body, kProbeId, verdict, note, checked, failed,
                                          replay::offlinelock::Current(),
                                          replay::offlinelock::Exercised(), g_reads, eg);

    HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, body, static_cast<DWORD>(len), &written, nullptr);
    CloseHandle(file);
    return ok != FALSE && written == len;
}

void Report() {
    if (InterlockedExchange(&g_reported, 1) != 0) {
        return;
    }
    const replay::offlinelock::Report& lk = replay::offlinelock::Current();

    // Installed by us or by another module in this process - both mean the process is locked. Only a
    // refusal is a failure, and "somebody else got here first" is not a refusal.
    const bool installed = lk.state == replay::offlinelock::LockState::Locked ||
                           lk.state == replay::offlinelock::LockState::AlreadyLockedInProcess;
    // The control has to have discriminated, and only the module that ran it can speak to that.
    const bool controlOk = !lk.installedHere ||
                           (lk.selfTestRan && lk.selfTestClearedByOriginal &&
                            lk.selfTestHeldWhenPreserved);

    const char* verdict = "PASS";
    const char* note = "the offline bit cannot be cleared by the session teardown in this process";
    if (!installed) {
        verdict = "FAIL";
        note = "the lock is not installed; the offline bit is a configuration, not an invariant";
    } else if (!controlOk) {
        // NOT-RUN rather than FAIL: a control that did not discriminate means the instrument was not
        // working, which needs the opposite response to a real failure and must not share its colour.
        verdict = "NOT-RUN";
        note = "the lock installed but its known-answer control did not discriminate";
    } else if (replay::offlinelock::Exercised() == replay::offlinelock::Exercise::Failed) {
        verdict = "FAIL";
        note = "the teardown cleared the offline bit and the restore did not take";
    }

    w1c::EgressView eg{};
    GatherEgress(&eg);

    // Whether the blocker was SUPPOSED to arm in this session. live-online-capture and
    // disabled-by-config are the only two states where not-armed is correct, so a check must not read
    // them as failure.
    const replay::egress::ArmTrigger armTrigger = replay::egress::Current().trigger;
    const bool egressShouldBeArmed =
        armTrigger != replay::egress::ArmTrigger::LiveOnlineCapture &&
        armTrigger != replay::egress::ArmTrigger::DisabledByConfig;

    // Every call to a non-loopback destination the table recorded, for the reconciliation below.
    uint64_t remoteCalls = 0;
    for (uint32_t i = 0; i < eg.destCount; ++i) {
        if (!eg.dest[i].local) {
            remoteCalls += eg.dest[i].count;
        }
    }

    // ★ The substance floor reads `checked`, and the last run of this probe recorded NOT-RUN over a
    // perfect artifact because the field was absent. Each entry is a real assertion about this launch,
    // counted here so the number and the checks cannot drift apart.
    struct Case { const char* what; bool ok; };
    const Case cases[] = {
        {"the offline-bit lock is installed in this process", installed},
        {"its known-answer control discriminated", controlOk},
        {"the clearing sites found match the number expected", lk.sitesFound == lk.sitesExpected},
        {"the teardown never cleared the bit without us restoring it",
         replay::offlinelock::Exercised() != replay::offlinelock::Exercise::Failed},
        // ★ Mode-aware, because the blocker is no longer supposed to arm in every session. Asserting
        // it always armed would fail a capture session for behaving correctly - the same defect shape
        // as asserting refused == 0, one level up: a check that fires on the system working.
        {"the blocker armed every entry point, or correctly did not arm at all",
         egressShouldBeArmed ? (eg.hooksLive == eg.hooksRequested && eg.hooksRequested != 0)
                             : (eg.hooksLive == 0)},
        // ★ NOT an assertion that nothing was refused. A refusal is the blocker WORKING, and this
        // profile produced 82 of them; what would be a defect is refusing without recording WHERE.
        {"every refused destination is named, not just counted",
         eg.refused == 0 || eg.destCount != 0},
        {"the destination table did not overflow", eg.overflow == 0},
        // ★ Containment, reconciled rather than assumed. Every non-loopback destination in the table
        // must be accounted for in the refusal counter: if the two disagree, either something left
        // without being counted or the counter is describing calls the table never saw. Only checkable
        // while the table is intact, which is what the overflow row above is for.
        {"the refusal counter and the destination table agree on what left",
         eg.overflow != 0 || remoteCalls == eg.refused},
    };
    unsigned checked = 0;
    unsigned failed = 0;
    for (const Case& c : cases) {
        ++checked;
        if (!c.ok) {
            ++failed;
        }
        replay::log::Direct(c.ok ? replay::LogLevel::Info : replay::LogLevel::Error, kModuleName,
                            "%s %s", c.ok ? "ok  " : "FAIL", c.what);
    }
    if (failed != 0 && std::strcmp(verdict, "PASS") == 0) {
        verdict = "FAIL";
    }

    // ★ Every read attempt, in the log at the point of judgement. A refusal names what it refused and
    // why; a clean read of zero says so as a different word. That distinction is what three launches of
    // four booleans could not express.
    replay::log::Direct(replay::LogLevel::Info, kModuleName,
                        "module base 0x%llX, header ImageBase 0x%llX, relocated %s",
                        static_cast<unsigned long long>(g_reads.moduleBase),
                        static_cast<unsigned long long>(g_reads.imageBase),
                        g_reads.moduleBase != g_reads.imageBase ? "yes" : "NO - rebase was a no-op");
    for (size_t i = 0; i < g_reads.siteCount; ++i) {
        const w1c::ReadSite& rs = g_reads.sites[i];
        replay::log::Direct(rs.outcome == w1c::ReadOutcome::ReadOk ? replay::LogLevel::Info
                                                                  : replay::LogLevel::Error,
                            kModuleName,
                            "read %s: %s at 0x%llX (registry 0x%llX), state 0x%X protect 0x%X, "
                            "value 0x%llX, %u attempt(s)",
                            rs.name, w1c::ReadOutcomeName(rs.outcome),
                            static_cast<unsigned long long>(rs.absoluteVa),
                            static_cast<unsigned long long>(rs.preferredVa), rs.state, rs.protect,
                            static_cast<unsigned long long>(rs.value), rs.attempts);
    }

    // Every destination, in the log as well as the artifact. If done.json is ever lost or unparseable
    // again, the list that matters is still recoverable from echoreplay.log.
    for (uint32_t i = 0; i < eg.destCount; ++i) {
        replay::log::Direct(replay::LogLevel::Info, kModuleName, "egress destination %u: %s via %s, "
                            "%llu call(s), %s",
                            i, eg.dest[i].text, eg.dest[i].via,
                            static_cast<unsigned long long>(eg.dest[i].count),
                            eg.dest[i].local ? "allowed-local" : "REFUSED");
    }

    WriteDone(verdict, note, checked, failed, eg);
    replay::log::Direct(replay::LogLevel::Info, kModuleName,
                        "%s: lock=%s exercise=%s calls=%llu prevented=%llu; clock ptr %s, "
                        "minElapsedUs=%llu maxElapsedUs=%llu, %llu timescale sample(s)",
                        verdict, replay::offlinelock::LockStateName(lk.state),
                        replay::offlinelock::ExerciseName(replay::offlinelock::Exercised()),
                        static_cast<unsigned long long>(lk.calls),
                        static_cast<unsigned long long>(lk.clearsPrevented),
                        g_reads.clockSeen ? "seen" : "never appeared",
                        static_cast<unsigned long long>(g_reads.minElapsedUs),
                        static_cast<unsigned long long>(g_reads.maxElapsedUs),
                        static_cast<unsigned long long>(g_reads.seriesLen));
}

// The observation window, overridable so the artifact this probe writes can be exercised offline.
// BLOCK-1 shipped because the only configuration the tests could produce was not the one the game runs;
// a probe whose output nothing can parse until launch day repeats that in a different place.
DWORD ObserveMs() {
    char text[32] = {};
    if (GetEnvironmentVariableA("REPLAY_PROBE_OBSERVE_MS", text, sizeof text) == 0) {
        return kObserveMs;
    }
    const long parsed = std::strtol(text, nullptr, 10);
    return (parsed > 0) ? static_cast<DWORD>(parsed) : kObserveMs;
}

DWORD WINAPI ProbeThread(LPVOID) {
    RecordBases();
    const ULONGLONG start = GetTickCount64();
    const ULONGLONG deadline = start + ObserveMs();
    while (GetTickCount64() < deadline) {
        const uint64_t nowMs = GetTickCount64() - start;
        SampleGlobalClock(nowMs);
        SampleFrameDelta();
        SampleTimescale(nowMs);
        SampleLobby(nowMs);
        Sleep(kSampleIntervalMs);
    }
    Report();
    return 0;
}

// Started from a static initialiser, which the CRT runs inside DllMain before the bootstrap's own
// DLL_PROCESS_ATTACH body. This is now harness contract: coexist::ClaimInstance refuses every module
// after the first in a multi-probe launch, so a probe that filed from its worker would never file at
// all. CreateThread is the one call the loader contract sanctions here, and the thread does not run
// until the loader lock is released.
struct ProbeStarter {
    ProbeStarter() {
        HANDLE thread = CreateThread(nullptr, 0, ProbeThread, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
    }
};

ProbeStarter g_starter;

}  // namespace
