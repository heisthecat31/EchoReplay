// W1-L's in-game half: the three reads tests/wirefeed cannot construct offline.
//
// docs/spikes/W1-L.md section 9 named three checks whose inputs could not be built without a live
// client-mode session. The `offline-local` profile makes that session reachable with nothing remote,
// so they become one launch's worth of work rather than an unrunnable list:
//
//   wiretap-client-broadcaster  does a client-mode CR15NetGame carry a non-null CBroadcaster at
//                               +0x28C8? Reported as the RAW pointer, not as a boolean, so the
//                               reader can tell "null" from "we never got a netgame" from "we did".
//   wiretap-callback-thread     is the dispatch thread the tick thread? The header assumes the worst
//                               and must not block; this says whether it had to.
//   wiretap-text-untouched      the W1-L half of SC7: this tap is Level 0, so nothing it does may
//                               change a byte of .text. Scope is in W1-L.probe.json -- a Level-0
//                               packet cannot produce the full SC7 runtime half on its own, because
//                               a diff declaring zero expected rows is refused by design.
//
// Reporting follows HARNESS.md section 2.3 exactly: the verdict is filed from a static initialiser,
// which runs before DllMain and therefore before the instance claim any later-loading probe is
// refused by; a provisional record is written before anything is waited on; and every check carries
// three outcomes, because "the session was quiet" is not "the tap is broken".

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "bootstrap/bootstrap.h"
#include "bootstrap/egress.h"
#include "bootstrap/log.h"
#include "bootstrap/md5.h"
#include "bootstrap/offlinelock.h"
#include "bootstrap/services.h"
#include "engine/tick.h"
#include "engine/wiretap.h"
#include "engine/wiretap_facts.h"

namespace {

const char kReplayModuleName[] = "w1l-probe";

using namespace replay::wiretap_facts;
using replay::IWiretap;

// How long the tap is left registered before teardown, and how long silence is watched for after
// it. Both are wall clock on the tick thread, so a session that never loads simply never leaves the
// first phase and every check downstream reports COULD-NOT-DETERMINE.
constexpr uint64_t kObserveMs = 60000;
constexpr uint64_t kSilenceMs = 10000;
// The worker files whatever it has by then. It must be inside the launch deadline, or the harness
// kills the process while the only record on disk is the provisional one.
constexpr uint64_t kFileAtMs = 200000;

enum Phase { kWaiting = 0, kListening = 1, kTornDown = 2, kFinished = 3 };

struct Stream {
    const char* name;
    uint64_t    symbol;
    uint16_t    handle;
};

struct State {
    HMODULE self = nullptr;
    uint64_t attachMs = 0;

    std::atomic<int>      phase{kWaiting};
    std::atomic<uint64_t> ticks{0};
    std::atomic<uint32_t> tickThread{0};
    std::atomic<uint32_t> callbackThread{0};
    std::atomic<uint64_t> delivered[3]{};
    std::atomic<uint64_t> afterTeardown{0};
    std::atomic<uint64_t> bytesSeen{0};

    // The read. A raw pointer value, because "null" and "we never looked" are different answers and
    // a boolean cannot carry both.
    std::atomic<uint64_t> broadcaster{0};
    std::atomic<bool>     netGameSeen{false};
    std::atomic<bool>     lookedForBroadcaster{false};

    std::atomic<int>  available{-1};   // -1 not attempted, 0 refused, 1 ok
    std::atomic<bool> tickRegistered{false};
    std::atomic<bool> bootstrapOk{false};
    char              diagnostic[192] = {0};

    // -1 not attempted, 0 a prologue moved, 1 all five read back byte for byte.
    std::atomic<int> prologuesBefore{-1};
    std::atomic<int> prologuesAfter{-1};
    char             textBefore[replay::kMd5HexChars] = {0};
    char             textAfter[replay::kMd5HexChars] = {0};
    std::atomic<uint64_t> textBytes{0};

    uint64_t listenAtMs = 0;
    uint64_t teardownAtMs = 0;
    Stream   streams[3] = {{kNamePlayerStateMsg, 0, IWiretap::kInvalidHandle},
                           {kNameDeltaUnreliableVars, 0, IWiretap::kInvalidHandle},
                           {kNameComponentEvent, 0, IWiretap::kInvalidHandle}};
};

State g;
IWiretap* g_tap = nullptr;

// ------------------------------------------------------------------------------------------
// Reporting. Nothing below touches the bootstrap, on purpose.
// ------------------------------------------------------------------------------------------

void JsonEscape(char* out, size_t cap, const char* in) {
    size_t o = 0;
    for (size_t i = 0; in[i] != 0 && o + 2 < cap; ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == 0x5C || c == 0x22) {
            out[o++] = 0x5C;
            out[o++] = static_cast<char>(c);
        } else if (c < 0x20 || c > 0x7E) {
            out[o++] = ' ';
        } else {
            out[o++] = static_cast<char>(c);
        }
    }
    out[o] = 0;
}

void DllStem(char* out, size_t cap) {
    char dll[MAX_PATH] = {0};
    GetModuleFileNameA(g.self, dll, sizeof dll);
    const char* base = strrchr(dll, 0x5C);
    const char* alt = strrchr(dll, '/');
    if (alt != nullptr && (base == nullptr || alt > base)) {
        base = alt;
    }
    strncpy_s(out, cap, base != nullptr ? base + 1 : dll, _TRUNCATE);
    if (char* dot = strrchr(out, '.')) {
        *dot = 0;
    }
}

void ProbeDir(char* out, size_t cap) {
    char root[MAX_PATH] = {0};
    GetEnvironmentVariableA("REPLAY_PROBE_ROOT", root, sizeof root);
    char stem[MAX_PATH];
    DllStem(stem, sizeof stem);
    _snprintf_s(out, cap, _TRUNCATE, "%s%c%s", root, static_cast<char>(0x5C), stem);
    CreateDirectoryA(out, nullptr);
}

// The plan's id for this DLL, from REPLAY_PROBE_IDS. A verdict filed under a name the plan does not
// know is NOT-RUN, so the fallback is deliberately not a real probe id.
void ProbeId(char* out, size_t cap) {
    strncpy_s(out, cap, "W1-L-unknown", _TRUNCATE);
    char ids[1024] = {0};
    GetEnvironmentVariableA("REPLAY_PROBE_IDS", ids, sizeof ids);
    char stem[MAX_PATH];
    DllStem(stem, sizeof stem);
    for (char* entry = ids; *entry != 0;) {
        char* end = strchr(entry, ';');
        if (end != nullptr) {
            *end = 0;
        }
        if (char* eq = strchr(entry, '=')) {
            *eq = 0;
            if (_stricmp(entry, stem) == 0) {
                strncpy_s(out, cap, eq + 1, _TRUNCATE);
                return;
            }
        }
        if (end == nullptr) {
            break;
        }
        entry = end + 1;
    }
}

bool OpenArtifact(FILE** fh, const char* name) {
    char root[MAX_PATH] = {0};
    // HARNESS.md 2.3: write nothing when the harness did not ask for artifacts, so a probe cannot
    // scatter files into somebody's install.
    if (GetEnvironmentVariableA("REPLAY_PROBE_ROOT", root, sizeof root) == 0) {
        *fh = nullptr;
        return false;
    }
    char dir[MAX_PATH];
    ProbeDir(dir, sizeof dir);
    char path[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s%c%s", dir, static_cast<char>(0x5C), name);
    if (fopen_s(fh, path, "wb") != 0) {
        *fh = nullptr;
        return false;
    }
    return true;
}

void WriteProvisional(const char* id) {
    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(sizeof cwd, cwd);
    char cwdEscaped[MAX_PATH * 2];
    JsonEscape(cwdEscaped, sizeof cwdEscaped, cwd);
    FILE* fh = nullptr;
    if (OpenArtifact(&fh, "done.json") && fh != nullptr) {
        fprintf(fh,
                "{\"probe\":\"%s\",\"verdict\":\"NOT-RUN\",\"provisional\":true,\"checked\":0,"
                "\"cwd\":\"%s\",\"detail\":\"attached and waiting for a CR15NetGame; this file is "
                "provisional and is overwritten by the real verdict. If you are reading it, the "
                "process was killed first.\"}\n",
                id, cwdEscaped);
        fclose(fh);
    }
}

struct Check {
    const char* name;
    bool        ok;
    bool        notEvaluable;   // neither a pass nor a failure, and it is data
};

void WriteWiretapArtifact() {
    FILE* fh = nullptr;
    if (!OpenArtifact(&fh, "wiretap.jsonl") || fh == nullptr) {
        return;
    }
    fprintf(fh,
            "{\"kind\":\"read\",\"id\":\"wiretap-client-broadcaster\",\"netGameSeen\":%s,"
            "\"looked\":%s,\"broadcaster\":\"0x%016llX\",\"broadcasterOff\":\"0x%X\"}\n",
            g.netGameSeen.load() ? "true" : "false",
            g.lookedForBroadcaster.load() ? "true" : "false",
            (unsigned long long)g.broadcaster.load(), kBroadcasterOff);
    fprintf(fh,
            "{\"kind\":\"read\",\"id\":\"wiretap-callback-thread\",\"tickThread\":%u,"
            "\"callbackThread\":%u,\"sameThread\":%s}\n",
            g.tickThread.load(), g.callbackThread.load(),
            (g.callbackThread.load() != 0 && g.callbackThread.load() == g.tickThread.load())
                ? "true"
                : "false");
    fprintf(fh,
            "{\"kind\":\"read\",\"id\":\"wiretap-text-untouched\",\"prologuesBefore\":%d,"
            "\"prologuesAfter\":%d,\"textMd5Before\":\"%s\",\"textMd5After\":\"%s\","
            "\"textBytes\":%llu}\n",
            g.prologuesBefore.load(), g.prologuesAfter.load(), g.textBefore, g.textAfter,
            (unsigned long long)g.textBytes.load());
    const uint64_t observedMs =
        g.teardownAtMs > g.listenAtMs ? g.teardownAtMs - g.listenAtMs : 0;
    for (int i = 0; i < 3; ++i) {
        fprintf(fh,
                "{\"kind\":\"stream\",\"name\":\"%s\",\"symbol\":\"0x%016llX\",\"handle\":%u,"
                "\"delivered\":%llu,\"observedMs\":%llu,\"hz\":%.4f}\n",
                g.streams[i].name, (unsigned long long)g.streams[i].symbol, g.streams[i].handle,
                (unsigned long long)g.delivered[i].load(), (unsigned long long)observedMs,
                observedMs != 0 ? (double)g.delivered[i].load() * 1000.0 / (double)observedMs : 0.0);
    }
    fprintf(fh,
            "{\"kind\":\"teardown\",\"afterUnlistenAll\":%llu,\"silenceMs\":%llu,\"ticks\":%llu}\n",
            (unsigned long long)g.afterTeardown.load(), (unsigned long long)kSilenceMs,
            (unsigned long long)g.ticks.load());
    fclose(fh);
}

void WriteVerdict(const char* id) {
    WriteWiretapArtifact();

    const int phase = g.phase.load();
    const bool reachedListen = phase >= kListening;
    const bool reachedTeardown = phase >= kTornDown;
    uint64_t total = 0;
    for (int i = 0; i < 3; ++i) {
        total += g.delivered[i].load();
    }

    const Check checks[] = {
        // ★ The bootstrap being refused is a NORMAL outcome in a multi-probe launch -- the
        // instance claim goes to whichever DLL the loader reached first -- and modreg::InitAll then
        // never runs, so Init never had the chance to register. Calling that a failure would be a
        // false accusation of the one thing this probe cannot control. It is a real failure only
        // when the bootstrap came up and RegisterEarly still refused.
        {"tick-registered", g.tickRegistered.load(), !g.bootstrapOk.load()},
        {"netgame-reached", g.netGameSeen.load(), !g.tickRegistered.load()},
        // ★ The read. A null broadcaster on a client is a real FAIL - it is the one thing section 4
        // argued for and could not run - so it is not laundered into COULD-NOT-DETERMINE.
        {"broadcaster-non-null", g.broadcaster.load() != 0, !g.lookedForBroadcaster.load()},
        {"wiretap-available", g.available.load() == 1, g.available.load() < 0},
        {"listen-playerstate", g.streams[0].handle != IWiretap::kInvalidHandle, !reachedListen},
        {"listen-netvar", g.streams[1].handle != IWiretap::kInvalidHandle, !reachedListen},
        {"listen-event", g.streams[2].handle != IWiretap::kInvalidHandle, !reachedListen},
        // ★ Zero deliveries is NOT a failure here and saying so is the whole point. An offline
        // local match may legitimately carry no traffic on these symbols, and this probe cannot
        // tell a quiet session from a wrong bus. tests/wirefeed is what separates those, offline,
        // against a recorded session where the traffic is known to exist.
        {"callbacks-arrived", total != 0, total == 0},
        {"callback-thread-recorded", g.callbackThread.load() != 0, g.callbackThread.load() == 0},
        {"unlistenall-silences", g.afterTeardown.load() == 0, !reachedTeardown},
        {"entrypoints-unpatched", g.prologuesAfter.load() == 1, g.prologuesAfter.load() < 0},
        // ★ The offline-bit-lock guard this probe declares. It is installed by bootstrap::Attach,
        // so a DLL whose instance claim was refused never applied it -- and another probe in the
        // same launch may well hold it, which makes "we did not" different from "nobody did".
        // Undetermined is the only honest answer in that case; the raw state is in the report.
        {"offline-bit-lock",
         replay::offlinelock::Current().state == replay::offlinelock::LockState::Locked,
         !g.bootstrapOk.load()},
    };
    constexpr size_t kCount = sizeof checks / sizeof checks[0];

    unsigned failed = 0;
    unsigned unknown = 0;
    char detail[1600] = {0};
    for (size_t i = 0; i < kCount; ++i) {
        if (checks[i].notEvaluable) {
            ++unknown;
        } else if (!checks[i].ok) {
            ++failed;
        }
        char one[192];
        _snprintf_s(one, sizeof one, _TRUNCATE, "%s%s=%s", i ? "; " : "", checks[i].name,
                    checks[i].notEvaluable ? "COULD-NOT-DETERMINE" : (checks[i].ok ? "yes" : "NO"));
        strncat_s(detail, sizeof detail, one, _TRUNCATE);
    }
    // FAIL beats NOT-RUN: something evaluable was wrong, and that is the more urgent fact.
    const char* verdict = failed != 0 ? "FAIL" : (unknown != 0 ? "NOT-RUN" : "PASS");

    char tail[512];
    char diagEscaped[256];
    JsonEscape(diagEscaped, sizeof diagEscaped, g.diagnostic);
    _snprintf_s(tail, sizeof tail, _TRUNCATE,
                "; broadcaster=0x%016llX diag='%s' delivered=%llu/%llu/%llu after-teardown=%llu "
                "tickThread=%u cbThread=%u ticks=%llu",
                (unsigned long long)g.broadcaster.load(), diagEscaped,
                (unsigned long long)g.delivered[0].load(),
                (unsigned long long)g.delivered[1].load(),
                (unsigned long long)g.delivered[2].load(),
                (unsigned long long)g.afterTeardown.load(), g.tickThread.load(),
                g.callbackThread.load(), (unsigned long long)g.ticks.load());
    strncat_s(detail, sizeof detail, tail, _TRUNCATE);

    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(sizeof cwd, cwd);
    char cwdEscaped[MAX_PATH * 2];
    JsonEscape(cwdEscaped, sizeof cwdEscaped, cwd);

    FILE* fh = nullptr;
    if (!OpenArtifact(&fh, "done.json") || fh == nullptr) {
        return;
    }
    // HARNESS.md 2.3: the blocker is a structured object; an omitted one is recorded as unknown,
    // and unknown is not safe. This probe opens no socket of its own, so `refused` is a statement
    // about the game.
    const replay::egress::Report& eg = replay::egress::Current();
    const replay::offlinelock::Report& lock = replay::offlinelock::Current();
    fprintf(fh,
            "{\"probe\":\"%s\",\"verdict\":\"%s\",\"checked\":%u,\"cwd\":\"%s\","
            "\"egress\":{\"armed\":%s,\"state\":\"%s\",\"armedAtMs\":%llu,\"refused\":%llu,"
            "\"allowedLocal\":%llu,\"hooksLive\":%u,\"hooksRequested\":%u,\"hooks\":[%s],"
            "\"uncovered\":\"%s\"},"
            "\"offlineLock\":{\"state\":\"%s\",\"exercise\":\"%s\",\"sitesFound\":%u,"
            "\"sitesExpected\":%u,\"calls\":%llu,\"clearsPrevented\":%llu,"
            "\"installedByThisDll\":%s},"
            "\"detail\":\"%s\"}\n",
            id, verdict, static_cast<unsigned>(kCount), cwdEscaped,
            eg.state == replay::egress::ArmState::Armed ? "true" : "false",
            replay::egress::ArmStateName(eg.state), (unsigned long long)eg.armedAtMs,
            (unsigned long long)eg.refusedRemote, (unsigned long long)eg.allowedLocal,
            eg.hooksLive, eg.hooksRequested, replay::egress::HookNamesJson(),
            replay::egress::CoverageNote(),
            replay::offlinelock::LockStateName(lock.state),
            replay::offlinelock::ExerciseName(replay::offlinelock::Exercised()), lock.sitesFound,
            lock.sitesExpected, (unsigned long long)lock.calls,
            (unsigned long long)lock.clearsPrevented,
            lock.state == replay::offlinelock::LockState::Locked ? "true" : "false", detail);
    fclose(fh);
}

// ------------------------------------------------------------------------------------------
// The .text witness: the W1-L half of SC7.
// ------------------------------------------------------------------------------------------

const IMAGE_SECTION_HEADER* TextSection(uintptr_t base) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sec[i].Name, ".text", 5) == 0) {
            return &sec[i];
        }
    }
    return nullptr;
}

// Hashed on the worker thread, never on the tick: .text is over 23 MB here and two stalls of that
// size on the tick thread would perturb whatever frame-continuity probe shares the launch.
void HashText(char* out) {
    out[0] = 0;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const IMAGE_SECTION_HEADER* text = base != 0 ? TextSection(base) : nullptr;
    if (text == nullptr) {
        return;
    }
    const auto bytes = static_cast<size_t>(text->Misc.VirtualSize);
    g.textBytes.store(bytes);
    replay::Md5 md5;
    md5.Update(reinterpret_cast<const void*>(base + text->VirtualAddress), bytes);
    uint8_t digest[16];
    md5.Final(digest);
    replay::Md5::ToHex(digest, out);
}

// The five entry points the wiretap calls, re-read at the rebased address. A Level-0 tap writes a
// listener node the engine allocated and nothing else, so these must not move.
int ProloguesIntact() {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (base == 0) {
        return -1;
    }
    const uintptr_t slide = base - static_cast<uintptr_t>(kImageBase);
    struct Site {
        uint64_t       va;
        const uint8_t* want;
    };
    const Site sites[] = {
        {kBroadcasterListen, kListenPrologue},
        {kBroadcasterUnlisten, kUnlistenPrologue},
        {kBroadcasterUnlistenAll, kUnlistenAllPrologue},
        {kMatSymHash, kMatSymHashPrologue},
        {kMatSymCombine, kMatSymCombinePrologue},
    };
    for (const Site& s : sites) {
        const auto* at = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(s.va) + slide);
        if (memcmp(at, s.want, 20) != 0) {
            return 0;
        }
    }
    return 1;
}

// ------------------------------------------------------------------------------------------
// Measurement.
// ------------------------------------------------------------------------------------------

void Sink(const replay::WireMessage& msg, void* ctx) {
    const auto index = static_cast<int>(reinterpret_cast<uintptr_t>(ctx));
    g.callbackThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g.bytesSeen.fetch_add(msg.bytes, std::memory_order_relaxed);
    if (g.phase.load(std::memory_order_relaxed) >= kTornDown) {
        g.afterTeardown.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (index >= 0 && index < 3) {
        g.delivered[index].fetch_add(1, std::memory_order_relaxed);
    }
}

void OnTick(const replay::TickInfo& info, void*) {
    g.ticks.fetch_add(1, std::memory_order_relaxed);
    g.tickThread.store(GetCurrentThreadId(), std::memory_order_relaxed);

    const uint64_t now = GetTickCount64();
    switch (g.phase.load(std::memory_order_relaxed)) {
        case kWaiting: {
            if (info.netGame == nullptr) {
                return;
            }
            g.netGameSeen.store(true);
            // ★ The read, taken before anything else touches the object: the raw pointer at
            // CR15NetGame + NETGAME.BROADCASTER_OFF in a client-mode session.
            const auto base = reinterpret_cast<uintptr_t>(info.netGame);
            g.broadcaster.store(*reinterpret_cast<const uint64_t*>(base + kBroadcasterOff));
            g.lookedForBroadcaster.store(true);
            g.prologuesBefore.store(ProloguesIntact());

            g_tap = replay::WiretapCreate(info.netGame);
            if (g_tap == nullptr) {
                strncpy_s(g.diagnostic, sizeof g.diagnostic, "WiretapCreate returned null",
                          _TRUNCATE);
                g.available.store(0);
                g.phase.store(kFinished);
                return;
            }
            strncpy_s(g.diagnostic, sizeof g.diagnostic, g_tap->Diagnostic(), _TRUNCATE);
            g.available.store(g_tap->Available() ? 1 : 0);
            replay::log::Direct(replay::LogLevel::Info, kReplayModuleName,
                                "netGame=%p broadcaster=0x%llX wiretap=%s", info.netGame,
                                (unsigned long long)g.broadcaster.load(), g.diagnostic);
            if (!g_tap->Available()) {
                g.phase.store(kFinished);
                return;
            }
            for (int i = 0; i < 3; ++i) {
                g.streams[i].symbol = g_tap->Symbol(g.streams[i].name);
                g.streams[i].handle = g_tap->Listen(g.streams[i].name, Sink,
                                                    reinterpret_cast<void*>(
                                                        static_cast<uintptr_t>(i)));
            }
            g.prologuesAfter.store(ProloguesIntact());
            g.listenAtMs = now;
            g.phase.store(kListening);
            return;
        }
        case kListening: {
            if (now - g.listenAtMs < kObserveMs) {
                return;
            }
            g.teardownAtMs = now;
            // The order matters: mark the phase FIRST, so a dispatch already on another thread is
            // counted as an after-teardown arrival rather than being lost between the two stores.
            g.phase.store(kTornDown);
            g_tap->UnlistenAll();
            return;
        }
        case kTornDown: {
            if (now - g.teardownAtMs < kSilenceMs) {
                return;
            }
            g.phase.store(kFinished);
            return;
        }
        default:
            return;
    }
}

DWORD WINAPI WorkerMain(LPVOID) {
    // The blocker is the shared facility now; arming is idempotent and reports the first call's
    // state, so doing it here costs nothing if another probe got there first.
    replay::egress::Arm();
    HashText(g.textBefore);

    char id[128];
    ProbeId(id, sizeof id);

    // The offline reporting proof has no game and therefore no phase to finish; it lowers the
    // deadline so the real verdict lands inside a test rather than 200 seconds later. Same code,
    // same checks, same aggregation -- only the wait is shorter, and the harness never sets it.
    uint64_t fileAtMs = kFileAtMs;
    char override_[32] = {0};
    if (GetEnvironmentVariableA("REPLAY_PROBE_DEADLINE_MS", override_, sizeof override_) != 0) {
        const long long parsed = _atoi64(override_);
        if (parsed > 0) {
            fileAtMs = static_cast<uint64_t>(parsed);
        }
    }
    const uint64_t deadline = g.attachMs + fileAtMs;
    while (g.phase.load() != kFinished && GetTickCount64() < deadline) {
        Sleep(200);
    }
    HashText(g.textAfter);
    WriteVerdict(id);
    return 0;
}

bool Init(replay::ReplayServices* services) {
    REPLAY_REQUIRE(services, tick);
    REPLAY_REQUIRE(services, log);
    // RegisterEarly, not Late: W1-D ships no late source, so RegisterLate ALWAYS returns false
    // today and every read below would have come back undetermined on a launch nobody could repeat.
    // Nothing here needs the late position anyway -- the tick is only how this probe reaches
    // netGame, and the tap's ordering against the engine's own handler is decided by Listen's
    // prepend argument, not by where in the frame we happen to be standing.
    if (!services->tick->RegisterEarly(OnTick, nullptr)) {
        replay::log::Direct(replay::LogLevel::Error, kReplayModuleName,
                            "RegisterEarly refused; every read below will be undetermined");
        return false;
    }
    g.tickRegistered.store(true);
    return true;
}

void Shutdown() {
    if (g_tap != nullptr) {
        replay::WiretapDestroy(g_tap);
        g_tap = nullptr;
    }
}

// Order 900: after the tick module has published ITick into ReplayServices.
REPLAY_MODULE("w1l-probe", 900, Init, nullptr, Shutdown);

// ★ A static initialiser runs before DllMain, and therefore before coexist::ClaimInstance can refuse
// this module for not being the first DLL the loader reached. That refusal returns from
// bootstrap::Attach before modreg::InitAll, so a verdict filed from Init would never be filed at
// all -- which cost sweep 4 three probes. One code path, always taken.
struct WiretapAttachFiler {
    WiretapAttachFiler();
};

WiretapAttachFiler::WiretapAttachFiler() {
    g.attachMs = GetTickCount64();
    // Pin: the worker outlives DllMain, and a refused bootstrap would otherwise let the loader
    // unload the module out from under it.
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&WriteProvisional), &g.self);
    char id[128];
    ProbeId(id, sizeof id);
    WriteProvisional(id);
    if (HANDLE worker = CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr)) {
        CloseHandle(worker);
    }
}

#if !W1L_NO_STATIC_FILER
WiretapAttachFiler g_filer;
#else
// ★ THE MUTATION BUILD reproduces the OLD shape rather than deleting the new one: it files from a
// REPLAY_MODULE init, which is where sweep 4's three silent probes filed from. It therefore files
// normally when it is the first DLL the loader reached, and goes SILENT when it is the refused one
// -- which is the defect itself, not an approximation of it. tests/wirefeed asserts both halves, so
// no launch is spent rediscovering it.
bool LegacyFilerInit(replay::ReplayServices* services) {
    REPLAY_REQUIRE(services, log);
    if (HANDLE worker = CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr)) {
        CloseHandle(worker);
    }
    return true;
}

REPLAY_MODULE("w1l-probe-legacy", 901, LegacyFilerInit, nullptr, nullptr);
#endif

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (g.self == nullptr) {   // the mutation build has no static filer to have set these
                g.self = module;
                g.attachMs = GetTickCount64();
            }
            // Attach's result is reported, not obeyed: the worker is already measuring, and a
            // refused instance claim is a normal outcome rather than a reason to disappear.
            g.bootstrapOk.store(replay::bootstrap::Attach(module));
            return TRUE;
        case DLL_PROCESS_DETACH:
            replay::bootstrap::Detach(reserved != nullptr);
            break;
        default:
            break;
    }
    return TRUE;
}
