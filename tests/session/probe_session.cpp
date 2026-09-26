// W1-F's in-game probe. Queued through runs\queue\W1-F.probe.json; this packet never launches the
// game itself.
//
// ★ THE PROBE ENFORCES THE OWNER'S CONSTRAINT RATHER THAN OBSERVING IT.
//
// "DONT do anything relating to an actual server" cannot be checked after the fact: by the time a
// census sees a connection, the connection has happened. A real launch of
// `-noovr -spectatorstream echo_arena -level mpl_arena_a` went login -> matchmaking -> a live
// server, so the flat-screen switches do not make a launch safe either. This probe therefore hooks
// the process's egress entry points and REFUSES every non-loopback destination with
// WSAENETUNREACH. Nothing can reach a server while it is loaded, whatever the engine intends.
//
// The surface is closed rather than guessed. echovr.exe's WS2_32 imports are the whole of its
// networking: connect, sendto, send, WSASend, WSARecv, recv, recvfrom, socket, bind, listen,
// accept, getaddrinfo. TCP egress needs connect; UDP egress needs connect-then-send, or sendto.
// Blocking connect and sendto closes both, and send/WSASend cannot reach anywhere on their own
// because the connect that would have aimed them was refused. WSAConnect and WSASendTo are hooked
// too: echovr.exe does not import them, but another module in the process might, and WinHTTP and
// WinINet both bottom out in this same ws2_32.
//
// ★ WS2_32 is a STATIC import of echovr.exe, so it is fully loaded before our DllMain runs. The
// trampolines are built and armed by replay::egress on the worker's first instruction, because
// MH_EnableHook suspends every thread in the process and that is not legal under the loader lock.
// This probe builds NO hook of its own on that surface - see the egress note below - and reports
// egress's own armedAtMs/firstSocketAfterMs rather than measuring the same thing twice.
//
// ★ This is also the answer to the census caveat: Windows exposes no remote address for a UDP
// socket, so a table-reading census can only ever say "no TCP peer". A hook on sendto sees the
// destination of every datagram.
//
// Two modes, chosen by the probe id the plan gives us, because the two questions carry very
// different risk:
//
//   *-attach   Inside the harness's attach window, on the `bare` profile. Measures only: is
//              CR15Game reachable this early, is the offline bit clear, is boot-to-multiplayer
//              clear, did anything try to leave the machine. Calls nothing.
//   *-offline  The full run. Enters an offline match with the stock call and measures what the
//              engine did about it: the offline bit, the 6719 bind, state 9. No descriptor has
//              scheduled it yet, so its expectations come from the launch's switches, not from a
//              profile it assumes.
//
// None of this ships. echoreplay.dll installs no socket hook and no CR15NetGame::Update detour.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include <MinHook.h>

#include "bootstrap/bootstrap.h"
#include "bootstrap/egress.h"
#include "bootstrap/hookmgr.h"
#include "bootstrap/services.h"
#include "engine/session.h"
#include "engine/session_facts.h"

#include "boot_flag_expectation.h"

#pragma comment(lib, "ws2_32.lib")

namespace sf = replay::session_facts;

namespace {

const char kReplayModuleName[] = "w1f-probe";

constexpr uint32_t kMaxTransitions = 64;

// The level and gametype the sweep asks for, resolved to ids by reading the image, so a wrong hash
// cannot be introduced here.
const char kLevelName[] = "mpl_arena_a";
const char kGametypeName[] = "echo_arena";

struct Transition {
    int32_t  from;
    int32_t  to;
    uint64_t atMs;
};

struct ProbeState {
    Transition            transitions[kMaxTransitions];
    std::atomic<uint32_t> transitionCount;
    int32_t               lastState;

    std::atomic<uint64_t> pumpCalls;
    // ★ True once SessionInit was observed to have run - it publishes services->session. Reading
    // ISessionEntry::Strategy() before that is reading a field nobody has written yet, which is
    // exactly what made S8 unpassable in runs/20260830-022305Z.
    std::atomic<bool>     bootstrapReady;
    std::atomic<bool>     bootstrapOk;
    uint64_t              attachMs;
    uint64_t              armedMs;
    uint64_t              gameSeenMs;

    uint64_t modeFlagsAtBoot;
    uint64_t sessionFlagsBefore;
    uint64_t sessionFlagsAfter;
    uint16_t httpPortOverride;

    HMODULE self;
};

ProbeState g;

// ---------------------------------------------------------------------------------------------
// Egress. The blocker itself now lives in W1-C's src/bootstrap/egress.*, promoted there on the
// orchestrator's ruling; this probe consumes that contract rather than carrying a second copy.
// Two copies would be worse than one: MinHook refuses a second MH_CreateHook on a target that
// already has one, so whichever armed second would report PartiallyArmed - which egress.h
// correctly calls "NOT a safe state" - over a process that was in fact fully guarded.
//
// ★★★ THE SOCKET DATER IS GONE, AND ITS REMOVAL IS THE FIX FOR S1 AND S2.
//
// This block used to keep one private trampoline on ws2_32!socket, justified in this comment by
// "nothing else hooks it, so there is no collision". That sentence was true the day it was written
// and false by the time it ran. egress.cpp's kRows carries `socket`, `WSASocketW` and `WSASocketA`
// - added later, to produce `socketsCreated` and `firstSocketAfterMs`, which are the two numbers
// this dater existed to produce. So the probe created the hook first, in DllMain, and the shared
// blocker's MH_CreateHook for the same target then returned ALREADY_CREATED on the worker:
//
//     [egress] MH_CreateHook failed for socket
//     [egress] partially-armed: 15 of 10 entry point(s) live 156 ms after ATTACH
//
// 15 of 16, so `state != Armed`, so S1 failed and S2 - which requires Armed - failed with it, on
// every W1-F launch in runs/20260830-022305Z (04, 15, 16). The probe was breaking the property it
// was measuring. Containment itself was never lost: egress's `socket` row is pure accounting and
// connect/WSAConnect/sendto/WSASendTo/send/WSASend/WSAIoctl were all live.
//
// The rule this violated is "ask the loader, never re-implement its rule": egress::Report already
// publishes socketsCreated and firstSocketAfterMs, so those are read from it below.
// ---------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------
// The engine-thread pump: one detour on CR15NetGame::Update.
// ---------------------------------------------------------------------------------------------
//
// Four arguments declared and all four forwarded. The real function's second argument is a stage
// index and it runs four times per frame; over-declaring is safe under the MS x64 ABI, while
// under-declaring would drop register arguments on the way to the trampoline.
using NetGameUpdateFn = int64_t (*)(void*, uint64_t, uint64_t, uint64_t);
NetGameUpdateFn g_realNetGameUpdate;

void RecordState() {
    const int32_t now = replay::session::StateRaw();
    if (now == g.lastState) {
        return;
    }
    const uint32_t at = g.transitionCount.load(std::memory_order_relaxed);
    if (at < kMaxTransitions) {
        g.transitions[at] = {g.lastState, now, GetTickCount64() - g.attachMs};
        g.transitionCount.store(at + 1, std::memory_order_release);
    }
    g.lastState = now;
}

int64_t HookNetGameUpdate(void* netGame, uint64_t stage, uint64_t a3, uint64_t a4) {
    g.pumpCalls.fetch_add(1, std::memory_order_relaxed);
    RecordState();
    replay::session::PumpOnEngineThread();
    return g_realNetGameUpdate(netGame, stage, a3, a4);
}

// ---------------------------------------------------------------------------------------------
// Reporting.
// ---------------------------------------------------------------------------------------------

// Everything that reaches done.json as a JSON string value goes through here. FailureReason is
// formatted at runtime and can carry a path or a quote, and an unescaped one would make the
// harness read a real verdict as malformed.
void JsonEscape(char* out, size_t cap, const char* in) {
    size_t o = 0;
    for (size_t i = 0; in[i] != 0 && o + 2 < cap; ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == 0x5C || c == 0x22) {  // backslash, double quote
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

void ProbeId(char* out, size_t cap) {
    // ★ NOT "W1-F-offline". The mode selector keys on that suffix, so a default of it would send a
    // probe whose id could not be resolved straight into the 300-second path that calls
    // sub_1400FE0A0 - the fail-safe defaulting to the unsafe mode. W1-F's own offline proof caught
    // this: with REPLAY_PROBE_IDS unparseable, every load took offline mode.
    strncpy_s(out, cap, "W1-F-unknown", _TRUNCATE);
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

void OpenArtifact(FILE** fh, const char* name) {
    char dir[MAX_PATH];
    ProbeDir(dir, sizeof dir);
    char path[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s%c%s", dir, static_cast<char>(0x5C), name);
    if (fopen_s(fh, path, "wb") != 0) {
        *fh = nullptr;
    }
}

// Four outcomes, because two cannot carry them: asked and passed, asked and failed, asked and could
// not tell, never asked. "Could not tell" reported as ok=false is a false accusation and as ok=true a
// false all-clear. "Never asked" is the one this probe used to print as PASS: S4 on a launch with no
// -httpport and S7 on a launch with no switch were comparing the constructor's zero with zero.
// ★ The expectation words a verdict line may carry. A voided line has no boot-flag expectation at all
// (boot_flag_expectation.h, AUDIT_W1-F_R4 F2), so it gets its own word rather than the CLEAR that the
// constructor values would otherwise be reported as - CLEAR here would read as "the switches did what
// they should have" on a launch whose -offline and four pins were silently ignored.
inline const char* ExpectedWord(bool expectedSet, bool lineVoided) {
    return lineVoided ? "line-voided-by-unregistered-switch" : (expectedSet ? "SET" : "CLEAR");
}

struct Check {
    const char* name;
    bool        ok;
    bool        notEvaluable = false;
    bool        notAsked = false;
    const char* why = nullptr;   // printed beside COULD-NOT-DETERMINE and NOT-ASKED
};

// ★ Written before the probe waits for anything, and overwritten by the real verdict later. A probe
// that is killed mid-measurement then still says "I loaded and I got this far", which is the one thing
// silence can never distinguish itself from. Sweep 3 lost three of my launches to exactly that.
void WriteProvisional(const char* id) {
    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(sizeof cwd, cwd);
    char cwdEscaped[MAX_PATH * 2];
    JsonEscape(cwdEscaped, sizeof cwdEscaped, cwd);
    FILE* fh = nullptr;
    OpenArtifact(&fh, "done.json");
    if (fh != nullptr) {
        fprintf(fh,
                "{\"probe\":\"%s\",\"verdict\":\"NOT-RUN\",\"checked\":0,\"cwd\":\"%s\","
                "\"detail\":\"attached and measuring; this file is provisional and is overwritten "
                "by the real verdict. If you are reading it, the process was killed first.\"}\n",
                id, cwdEscaped);
        fclose(fh);
    }
}

void WriteVerdict(const char* id, const Check* checks, size_t count, const char* extra) {
    FILE* fh = nullptr;
    OpenArtifact(&fh, "sockets.jsonl");
    if (fh != nullptr) {
        fprintf(fh,
                "{\"attachMs\":%llu,\"armedAfterMs\":%llu,\"hooksLive\":%u,\"armed\":%s,"
                "\"allowedLocal\":%llu,\"blockedRemote\":%llu,\"overflow\":%llu,"
                "\"socketsCreated\":%llu,\"firstSocketAfterMs\":%lld}\n",
                (unsigned long long)g.attachMs, (unsigned long long)(g.armedMs - g.attachMs),
                replay::egress::Current().hooksLive,
                (replay::egress::Current().state == replay::egress::ArmState::Armed) ? "true" : "false",
                (unsigned long long)replay::egress::Current().allowedLocal,
                (unsigned long long)replay::egress::Current().refusedRemote,
                (unsigned long long)replay::egress::Current().destinationOverflow,
                (unsigned long long)replay::egress::Current().socketsCreated,
                (long long)replay::egress::Current().firstSocketAfterMs);
        const uint32_t n = replay::egress::Current().distinctDestinations;
        for (uint32_t i = 0; i < n; ++i) {
            const char* text = nullptr;
            const char* via = nullptr;
            uint64_t calls = 0;
            bool local = false;
            if (replay::egress::DestinationAt(i, &text, &via, &calls, &local)) {
                fprintf(fh, "{\"dest\":\"%s\",\"via\":\"%s\",\"calls\":%llu,\"local\":%s}\n",
                        text, via, (unsigned long long)calls, local ? "true" : "false");
            }
        }
        fclose(fh);
    }

    OpenArtifact(&fh, "states.jsonl");
    if (fh != nullptr) {
        const uint32_t n = g.transitionCount.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < n; ++i) {
            fprintf(fh,
                    "{\"from\":%d,\"fromName\":\"%s\",\"to\":%d,\"toName\":\"%s\",\"atMs\":%llu}\n",
                    g.transitions[i].from, replay::session::StateName(g.transitions[i].from),
                    g.transitions[i].to, replay::session::StateName(g.transitions[i].to),
                    (unsigned long long)g.transitions[i].atMs);
        }
        fclose(fh);
    }

    unsigned failed = 0;
    unsigned unknown = 0;
    unsigned notAsked = 0;
    char detail[1800] = {0};
    for (size_t i = 0; i < count; ++i) {
        const Check& c = checks[i];
        const char* outcome = c.ok ? "yes" : "NO";
        if (c.notAsked) {
            ++notAsked;
            outcome = "NOT-ASKED";
        } else if (c.notEvaluable) {
            ++unknown;
            outcome = "COULD-NOT-DETERMINE";
        } else if (!c.ok) {
            ++failed;
        }
        char one[256];
        _snprintf_s(one, sizeof one, _TRUNCATE, "%s%s=%s%s%s%s", i ? "; " : "", c.name, outcome,
                    (c.notAsked || c.notEvaluable) && c.why ? " (" : "",
                    (c.notAsked || c.notEvaluable) && c.why ? c.why : "",
                    (c.notAsked || c.notEvaluable) && c.why ? ")" : "");
        strncat_s(detail, sizeof detail, one, _TRUNCATE);
    }
    // FAIL beats NOT-RUN: something we could evaluate was wrong, and that is the more urgent fact.
    // And a launch on which nothing was asked has no verdict: `checked` below counts the asked ones,
    // and the harness reads checked=0 as "asserted nothing".
    const unsigned asked = static_cast<unsigned>(count) - notAsked;
    const char* verdict = failed != 0 ? "FAIL" : ((unknown != 0 || asked == 0) ? "NOT-RUN" : "PASS");
    strncat_s(detail, sizeof detail, extra, _TRUNCATE);

    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(sizeof cwd, cwd);
    char cwdEscaped[MAX_PATH * 2];
    JsonEscape(cwdEscaped, sizeof cwdEscaped, cwd);

    OpenArtifact(&fh, "done.json");
    if (fh != nullptr) {
        // HARNESS.md S2.3: the blocker is reported as a structured object, never as prose. An
        // omitted object is recorded as `unknown`, and `unknown` is not `safe`. armedAtMs is the
        // window in which nothing was guarded, taken from the shared facility rather than from our
        // own clock, so it is the number the blocker actually achieved.
        // ★ The names come from the blocker too, and used to be a four-name literal printed beside a
        // computed hooksLive: a launch that lost `connect` filed a verdict naming `connect` as live,
        // and a partial arm is entirely about WHICH row is missing. The count and the names now have
        // one source, so they cannot disagree.
        const replay::egress::Report& eg = replay::egress::Current();
        fprintf(fh,
                "{\"probe\":\"%s\",\"verdict\":\"%s\",\"checked\":%u,\"undetermined\":%u,"
                "\"notAsked\":%u,\"cwd\":\"%s\","
                "\"egress\":{\"armed\":%s,\"state\":\"%s\",\"trigger\":\"%s\",\"armedAtMs\":%llu,"
                "\"refused\":%llu,\"allowedLocal\":%llu,\"hooksLive\":%u,"
                "\"hooksRequested\":%u,\"hooks\":[%s],\"firstSocketAfterMs\":%lld},"
                "\"detail\":\"%s\"}\n",
                id, verdict, asked, unknown, notAsked, cwdEscaped,
                eg.state == replay::egress::ArmState::Armed ? "true" : "false",
                replay::egress::ArmStateName(eg.state), replay::egress::ArmTriggerName(eg.trigger),
                (unsigned long long)eg.armedAtMs,
                (unsigned long long)eg.refusedRemote, (unsigned long long)eg.allowedLocal,
                eg.hooksLive, eg.hooksRequested, replay::egress::HookNamesJson(),
                (long long)eg.firstSocketAfterMs,
                detail);
        fclose(fh);
    }
}

// Ask the network whether the engine bound the port, rather than reading a field back.
bool PortIsListening(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    // Our own connect is hooked. The destination is loopback, so the blocker lets it through and
    // records it, which is correct: it belongs in the census like any other.
    const bool up = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    closesocket(s);
    return up;
}

bool WaitForGame(uint64_t deadlineMs) {
    while (GetTickCount64() < deadlineMs) {
        if (replay::session::ModeFlags() != 0) {
            g.gameSeenMs = GetTickCount64() - g.attachMs;
            return true;
        }
        Sleep(10);
    }
    return false;
}

void RunAttachMode(const char* id) {
    // ★ The completion point is real: everything this mode reports is readable the moment CR15Game
    // exists, so it waits for that and writes immediately. The previous version slept to a fixed
    // attach+4500 ms and was killed before it filed anything - "attach" was a label, not a fact.
    // The budget is measured from the WORKER's start because the worker itself only begins after
    // bootstrap has hashed 35 MB for the build guard.
    const uint64_t workerStart = GetTickCount64();
    const uint64_t deadline = workerStart + 2000;
    const bool gameSeen = WaitForGame(deadline);
    if (gameSeen) {
        g.modeFlagsAtBoot = replay::session::ModeFlags();
        g.sessionFlagsBefore = replay::session::SessionFlags();
        g.httpPortOverride = replay::session::HttpPortOverride();
    }

    // ★ The modules are published by the BOOTSTRAP's worker, not this one. Same deadline as the
    // game wait: measured need is ~170 ms beyond CR15Game, budget is 2000 ms from worker start,
    // and the descriptor's 6 s timeout is well clear of it.
    // ★ Ready() alone is NOT the signal, and bootstrap.h says so out loud: a run can be ready and
    // still have refused the build, in which case modreg::InitAll never ran and no module
    // published anything. The honest observable for "SessionInit ran" is the pointer SessionInit
    // itself writes - services->session - so that is what is waited on and what decides whether S8
    // is a verdict or a COULD-NOT-DETERMINE.
    auto sessionInitRan = []() {
        const replay::ReplayServices* s = replay::bootstrap::Services();
        return s != nullptr && s->session != nullptr;
    };
    bool bootstrapReady = sessionInitRan();
    while (!bootstrapReady && GetTickCount64() < deadline) {
        Sleep(10);
        bootstrapReady = sessionInitRan();
    }
    g.bootstrapReady.store(bootstrapReady, std::memory_order_release);

    replay::ISessionEntry* entry = replay::session::Interface();
    const bool no = !gameSeen;   // every game-derived check is undetermined without a CR15Game

    // Read once, and reported below, so the verdict says which branch S5/S6 took rather than leaving a
    // reader to guess the profile from the numbers.
    const wchar_t* commandLine = GetCommandLineW();
    const bool levelGiven = w1f::LevelSwitchGiven(commandLine);
    const wchar_t* bootMpSetter = w1f::BootMpSetterGiven(commandLine);
    const wchar_t* onlineClearer = w1f::OnlineClearerGiven(commandLine);
    const bool httpPortGiven = replay::bootstrap::HasSwitch(commandLine, L"-httpport");
    const bool offlineGiven = replay::bootstrap::HasSwitch(commandLine, L"-offline");
    const bool anySwitch = w1f::AnySwitchGiven(commandLine);
    // ★ Asked BEFORE either bit is scored. One token the engine does not register makes
    // ApplyCommandLine return at 0x140116739 with every handler skipped, so S5 and S6 have no
    // expectation to compare against - not a failing one, none. See boot_flag_expectation.h.
    wchar_t voidedBy[w1f::kTokenCap];
    const bool lineVoided = w1f::FirstUnregisteredSwitch(commandLine, voidedBy, w1f::kTokenCap);
    char voidedWhy[192];
    _snprintf_s(voidedWhy, sizeof voidedWhy, _TRUNCATE,
                "line-voided-by-unregistered-switch '-%ls': ApplyCommandLine returns at 0x140116739 "
                "and every switch handler is skipped, so both bits hold their constructor values and "
                "neither answers this launch's switches",
                voidedBy);

    // ★ egress::Arm holds a per-process sole-instance mutex, and each probe DLL links its own copy of
    // the blocker's statics. In a multi-probe launch whichever DLL arms first owns it and every other
    // copy reports Failed - over a process that is in fact fully guarded. Reporting that as a failed
    // check would be a false alarm, and reporting it as a pass would be a false all-clear, so it is
    // the third outcome. `hooksRequested == 0` is what distinguishes it: Arm() returns before setting
    // that field on the duplicate path, and sets it to the row count on every other path.
    const replay::egress::Report& eg = replay::egress::Current();
    const bool egressIsOurs = !(eg.state == replay::egress::ArmState::Failed &&
                                eg.hooksRequested == 0);
    const Check checks[] = {
        {"S1 egress blocker armed", eg.state == replay::egress::ArmState::Armed, !egressIsOurs},
        // ★ NOT "nothing was attempted". A live launch on the offline profile refused 82 outbound
        // destinations - Oculus platform traffic, plus g.echovrce.com reached by hostname through
        // WinINet from _local/config.json. The offline path is network-CONTAINED, not
        // network-silent, so a check that failed on attempts would fail on every real launch while
        // reporting nothing about containment. What must hold is that the blocker was armed for the
        // whole window, which is what makes every one of those attempts a refusal rather than a
        // connection. The count itself is REPORTED, in `refused`, and is never a verdict.
        // ★ Both numbers come from egress::Report and share ONE origin - bootstrap::AttachTickMs -
        // so they are directly comparable. And "no socket was created at all" is NOT a pass: it is
        // the case where the question could not be asked, so it is the third outcome. Scoring it
        // `ok` would have let this check certify containment on a window in which nothing happened.
        {"S2 blocker covered the whole window, so any attempt was refused",
         eg.state == replay::egress::ArmState::Armed &&
             eg.firstSocketAfterMs >= (int64_t)eg.armedAtMs,
         !egressIsOurs || eg.socketsCreated == 0 || eg.firstSocketAfterMs < 0},
        {"S3 CR15Game reachable inside the attach window", gameSeen, false},
        // ★ Asked only when the launch carries -httpport. With none, the override is the constructor's
        // zero and "== 0" compares it with itself: no scheduled profile passes -httpport, so that
        // check could only ever print PASS.
        {"S4 -httpport override is what the launch asked for",
         httpPortGiven && g.httpPortOverride != 0, no, !httpPortGiven,
         "no -httpport on this launch; a zero override is the default and refutes nothing"},
        // ★ DERIVED FROM THIS LAUNCH'S OWN SWITCHES, not fixed per profile, and from ALL the switches
        // that write the bits - boot_flag_expectation.h lists the sites. The first fix keyed on -level
        // alone and would have scored offline-local (-level -region) as a FAIL for correct engine
        // behaviour; the table case in test_session.cpp now covers -level beside every setter.
        {"S5 boot-to-multiplayer bit matches what this launch's switches set",
         w1f::BootMpBitVerdict(g.modeFlagsAtBoot, commandLine) == w1f::BitVerdict::AsExpected,
         no || lineVoided, false, lineVoided ? voidedWhy : nullptr},
        {"S6 SessionFlags online bit matches what this launch's switches set",
         w1f::OnlineBitVerdict(g.sessionFlagsBefore, commandLine) == w1f::BitVerdict::AsExpected,
         no || lineVoided, false, lineVoided ? voidedWhy : nullptr},
        // ★ "No switch reaches the offline bit" is a claim about the switches the launch carries.
        // With -offline on the line the bit is rlay's to set, on a schedule this probe does not
        // model - could not tell. With no switch at all there is nothing to test - not asked.
        {"S7 offline bit clear: no switch on this launch reached it",
         (g.sessionFlagsBefore & sf::kFlagBitOffline) == 0, no || offlineGiven, !anySwitch,
         offlineGiven ? "-offline given: rlay's PatchEnableOffline owns bit 0x40 on this launch"
         : !anySwitch ? "this launch carries no switch, so nothing could have reached it"
                      : nullptr},
        // ★★★ THE RACE THAT MADE THIS CHECK UNPASSABLE. Strategy() is written by SessionInit, which
        // runs from modreg::InitAll, which runs on the BOOTSTRAP's worker after config::Load, the
        // 35 MB build-guard md5, offlinelock::Apply and the ledger dump. This probe's worker is a
        // different thread, started from a static initialiser before DllMain, and it used to read
        // Strategy() the instant CR15Game appeared. Measured in all three W1-F launches: verdict
        // filed at attach+265..313 ms, "[session] SE-B ready" logged at attach+406..469 ms. The
        // probe always read the pre-init default and always reported SE-C. So it waits for the
        // publisher now - and if SessionInit never ran at all (bootstrap refused its instance claim,
        // or the build guard refused), that is COULD-NOT-DETERMINE and not an accusation. SE-C with
        // SessionInit having run IS a real failure: config off, a bad prologue, or the name-id
        // control failing, and each of those says so in reason=.
        // ★ THE L5 CHECK. `-offline` makes CR15Game+0x7AE0 bit 0x2000000 the launch's own boot
        // path - rlay redirects the arm it selects to sub_1400FE0E0, host this level locally - so
        // clearing it is the defect that parked launch-01 in the main menu for 234 s. The
        // observable is the module's own WRITE counter, not its decision and not a re-read of the
        // bit: the bit races the sample (the clear landed at attach+6.4 s, after this verdict is
        // written), the counter does not. Not asked without -offline, because there the suppression
        // is allowed to run and a zero would mean nothing.
        {"S9 the boot-into-multiplayer bit was left intact on this -offline launch",
         replay::session::BootSuppressionCount() == 0, false, !offlineGiven,
         "no -offline on this launch: nothing here needs the bit kept"},
        {"S8 SE-B selected, so the stock call is guarded and callable",
         strcmp(entry->Strategy(), "SE-B") == 0, !bootstrapReady},
    };

    char reason[256];
    JsonEscape(reason, sizeof reason, entry->FailureReason());
    char extra[1024];
    _snprintf_s(extra, sizeof extra, _TRUNCATE,
                "; SCOPE: this establishes the two mode bits (CR15Game+0x7AE0 bit 0x2000000 and "
                "SessionFlags bit 0x1) and NOTHING about login suppression - the window closes "
                "before the ~8s login, so a quiet network is the expected result whether -level "
                "works or not; pnsrad is created unconditionally with no bit-27 test. "
                "sites read from the MAPPED image via CR15Game* at 0x1420A0478"
                "; bootstrap=%s egressOwnedByUs=%s levelSwitch=%s bootMpSetter=%ls onlineClearer=%ls "
                "expected bootMp=%s online=%s"
                "; strategy=%s reason='%s' gameSeenAfterMs=%llu workerStartAfterMs=%llu "
                "modeFlags=0x%llX sessionFlags=0x%llX state=%d(%s) egressArmedAtMs=%llu "
                "allowedLocal=%llu refusedRemote=%llu",
                g.bootstrapOk.load() ? "up" : "REFUSED (normal in a multi-probe launch)",
                egressIsOurs ? "yes" : "no (another probe armed it first)",
                levelGiven ? "yes" : "no", bootMpSetter ? bootMpSetter : L"none",
                onlineClearer ? onlineClearer : L"none",
                ExpectedWord(w1f::BootMpBitExpectedSet(commandLine), lineVoided),
                ExpectedWord(w1f::OnlineBitExpectedSet(commandLine), lineVoided),
                entry->Strategy(), reason, (unsigned long long)g.gameSeenMs,
                (unsigned long long)(workerStart - g.attachMs),
                (unsigned long long)g.modeFlagsAtBoot,
                (unsigned long long)g.sessionFlagsBefore, replay::session::StateRaw(),
                replay::session::StateName(replay::session::StateRaw()),
                (unsigned long long)replay::egress::Current().armedAtMs,
                (unsigned long long)replay::egress::Current().allowedLocal,
                (unsigned long long)replay::egress::Current().refusedRemote);
    char policyNote[192];
    _snprintf_s(policyNote, sizeof policyNote, _TRUNCATE, " bootBitPolicy=%s suppressions=%u",
                replay::session::BootBitPolicyName(replay::session::BootBitPolicyForThisLaunch()),
                replay::session::BootSuppressionCount());
    strncat_s(extra, policyNote, _TRUNCATE);
    WriteVerdict(id, checks, sizeof(checks) / sizeof(checks[0]), extra);
}

void RunOfflineMode(const char* id) {
    const uint64_t deadline = g.attachMs + 300000;
    WaitForGame(deadline);
    g.modeFlagsAtBoot = replay::session::ModeFlags();
    g.httpPortOverride = replay::session::HttpPortOverride();

    replay::ISessionEntry* entry = replay::session::Interface();

    // The pump. CR15NetGame::Update exists once NetGame does, and hookmgr checks the recorded
    // prologue before building the trampoline, so a build that moved it refuses here.
    replay::HookStatus pump = replay::HookStatus::Absent;
    while (GetTickCount64() < deadline && entry->NetGame() == nullptr) {
        Sleep(20);
    }
    if (entry->NetGame() != nullptr) {
        pump = replay::hookmgr::Create(
            "NETGAME.UPDATE", facts::NETGAME_UPDATE, facts::NETGAME_UPDATE_PROLOGUE,
            sizeof(facts::NETGAME_UPDATE_PROLOGUE), reinterpret_cast<void*>(&HookNetGameUpdate),
            reinterpret_cast<void**>(&g_realNetGameUpdate), nullptr);
        if (pump == replay::HookStatus::Created) {
            pump = replay::hookmgr::Arm("NETGAME.UPDATE");
        }
    }

    // Ten seconds past boot, so anything the switches were going to do has happened before the
    // offline bit is read as "switches did not reach offline".
    Sleep(10000);
    g.sessionFlagsBefore = replay::session::SessionFlags();
    // Everything C5-C7 read after the call is read before it too. On offline-local rlay hosts the
    // match with no call of ours, so a check already satisfied here cannot be attributed to the
    // stock call and is COULD-NOT-DETERMINE, the same rule C4 applies to the offline bit.
    const int32_t stateBefore = replay::session::StateRaw();
    const bool hostPortBefore = PortIsListening(sf::kHttpPortHost);
    const bool defaultPortBefore = PortIsListening(sf::kHttpPortDefault);
    const bool hostBefore =
        (g.sessionFlagsBefore & sf::kFlagBitOffline) != 0 && sf::IsHostFlags(g.sessionFlagsBefore);
    const bool inGameBefore = stateBefore == static_cast<int32_t>(sf::kStateInGame);
    const uint64_t callMs = GetTickCount64();

    const uint64_t levelId = replay::session::HashName(kLevelName);
    const uint64_t gametypeId = replay::session::HashName(kGametypeName);
    const bool accepted = entry->RequestEntry(levelId, gametypeId);

    while (GetTickCount64() < deadline &&
           replay::session::StateRaw() != static_cast<int32_t>(sf::kStateInGame)) {
        Sleep(100);
    }
    Sleep(3000);  // the HTTP listener binds during the transition; let it finish

    g.sessionFlagsAfter = replay::session::SessionFlags();
    const bool hostPort = PortIsListening(sf::kHttpPortHost);
    const bool defaultPort = PortIsListening(sf::kHttpPortDefault);
    const int32_t finalState = replay::session::StateRaw();
    const wchar_t* commandLine = GetCommandLineW();
    const bool httpPortGiven = replay::bootstrap::HasSwitch(commandLine, L"-httpport");
    const bool offlineGiven = replay::bootstrap::HasSwitch(commandLine, L"-offline");
    wchar_t voidedBy[w1f::kTokenCap];
    const bool lineVoided = w1f::FirstUnregisteredSwitch(commandLine, voidedBy, w1f::kTokenCap);
    char voidedWhy[192];
    _snprintf_s(voidedWhy, sizeof voidedWhy, _TRUNCATE,
                "line-voided-by-unregistered-switch '-%ls': ApplyCommandLine returns at 0x140116739 "
                "and every switch handler is skipped, so both bits hold their constructor values and "
                "neither answers this launch's switches",
                voidedBy);

    const Check checks[] = {
        {"C1 egress blocker armed and nothing non-loopback attempted",
         (replay::egress::Current().state == replay::egress::ArmState::Armed) && replay::egress::Current().refusedRemote == 0},
        // The same four-outcome rules as S4, S5 and S7: this half has never been scheduled, so the
        // profile it runs on is whatever the descriptor says on the day, and the expectations must
        // come from that launch's switches rather than from a `flat` this code once assumed.
        {"C2 -httpport override is what the launch asked for",
         httpPortGiven && g.httpPortOverride != 0, false, !httpPortGiven,
         "no -httpport on this launch; a zero override is the default and refutes nothing"},
        {"C3 boot-to-multiplayer bit matches what this launch's switches set",
         w1f::BootMpBitVerdict(g.modeFlagsAtBoot, commandLine) == w1f::BitVerdict::AsExpected,
         lineVoided, false, lineVoided ? voidedWhy : nullptr},
        {"C4 offline bit clear before the stock call",
         (g.sessionFlagsBefore & sf::kFlagBitOffline) == 0, offlineGiven, false,
         "-offline given: rlay's PatchEnableOffline may have set bit 0x40 before our call"},
        {"C5 offline bit set after it and the host predicate true",
         (g.sessionFlagsAfter & sf::kFlagBitOffline) != 0 && sf::IsHostFlags(g.sessionFlagsAfter),
         hostBefore, false, "host predicate already true before the stock call: another writer hosted"},
        {"C6 6719 listening, 6721 not", hostPort && !defaultPort, hostPortBefore, false,
         "6719 already listening before the stock call: another writer bound it"},
        {"C7 NetGame reached state 9", finalState == static_cast<int32_t>(sf::kStateInGame),
         inGameBefore, false, "state 9 already reached before the stock call: another writer entered"},
        {"C8 the entry request was accepted", accepted},
    };

    char reason[256];
    JsonEscape(reason, sizeof reason, entry->FailureReason());
    char extra[768];
    _snprintf_s(extra, sizeof extra, _TRUNCATE,
                "; levelSwitch=%s bootMpSetter=%ls onlineClearer=%ls expected bootMp=%s online=%s"
                "; strategy=%s reason='%s' level=0x%016llX gametype=0x%016llX modeFlags=0x%llX "
                "before=0x%llX after=0x%llX stateBefore=%d(%s) portsBefore=%d/%d "
                "callAtMs=%llu state=%d(%s) pumpCalls=%llu pumpHook=%s "
                "allowedLocal=%llu blockedRemote=%llu",
                w1f::LevelSwitchGiven(commandLine) ? "yes" : "no",
                w1f::BootMpSetterGiven(commandLine) ? w1f::BootMpSetterGiven(commandLine) : L"none",
                w1f::OnlineClearerGiven(commandLine) ? w1f::OnlineClearerGiven(commandLine) : L"none",
                ExpectedWord(w1f::BootMpBitExpectedSet(commandLine), lineVoided),
                ExpectedWord(w1f::OnlineBitExpectedSet(commandLine), lineVoided),
                entry->Strategy(), reason, (unsigned long long)levelId,
                (unsigned long long)gametypeId, (unsigned long long)g.modeFlagsAtBoot,
                (unsigned long long)g.sessionFlagsBefore, (unsigned long long)g.sessionFlagsAfter,
                stateBefore, replay::session::StateName(stateBefore), hostPortBefore ? 1 : 0,
                defaultPortBefore ? 1 : 0, (unsigned long long)(callMs - g.attachMs),
                finalState, replay::session::StateName(finalState),
                (unsigned long long)g.pumpCalls.load(), replay::HookStatusName(pump),
                (unsigned long long)replay::egress::Current().allowedLocal,
                (unsigned long long)replay::egress::Current().refusedRemote);
    WriteVerdict(id, checks, sizeof(checks) / sizeof(checks[0]), extra);
}

DWORD WINAPI WorkerMain(LPVOID) {
    // First instruction, before anything else this probe does: close the egress surface.
    // The shared blocker first, then our recorder. Both are worker-thread work, because
    // MH_EnableHook suspends every thread in the process.
    // ★ SweepOverride, said out loud rather than defaulted. A probe IS a sweep, so the blocker must
    // arm whatever mode the session looks like and whatever the config says - and the report then
    // carries WHY it armed, which is the half a bare "armed: true" never told anyone.
    // ★ ONE arm, and no private recorder beside it. The socket counters this probe reports are
    // egress's own, so there is nothing left here to collide with its `socket` row.
    replay::egress::Arm(replay::egress::ArmTrigger::SweepOverride);
    g.armedMs = GetTickCount64();

    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    char id[128];
    ProbeId(id, sizeof id);

    // Silence is the one result the harness cannot tell from "never loaded", so file something now.
    WriteProvisional(id);

    // ★ FAIL-SAFE, and it was not. The old test was "id ends with -attach", which sent
    // W1-F-attach-norlay and W1-F-level down RunOfflineMode - a 300-second path that CALLS
    // sub_1400FE0A0 - on launches authorised as attach-only. Nothing came of it because the process
    // was killed first, but the descriptor said completesAt:attach while the code would have entered
    // a session. Offline mode is now opt-IN, by an explicit -offline suffix, and everything else
    // takes the mode that touches nothing.
    const size_t len = strlen(id);
    const bool offline = len >= 8 && _stricmp(id + len - 8, "-offline") == 0;
    if (offline) {
        RunOfflineMode(id);
    } else {
        RunAttachMode(id);
    }
    return 0;
}

// ★ A STATIC INITIALISER, WHICH IS THE ONLY PLACE THAT ALWAYS RUNS.
//
// This probe used to file its verdict from a REPLAY_MODULE init, and in sweep 4 it filed nothing
// three times. The cause was not the fixed sleep I first blamed: every probe DLL embeds the same
// bootstrap and resolves the same log path, coexist::ClaimInstance is a named mutex on that path
// plus the pid, and in a multi-probe launch the FIRST DLL the loader reaches wins. bootstrap::Attach
// then returns false at that check - before hookmgr::Initialize and before it starts its worker - so
// modreg::InitAll never runs and a verdict written from a module init is never written at all.
// probe_session.dll is not alphabetically first and never will be.
//
// A static initialiser runs before DllMain, and therefore before the instance claim is even
// attempted. One code path, always taken: whether the bootstrap runs, is refused, or never gets the
// chance. tests/session proves it offline against the real refusal, so no launch is spent on it.
struct SessionAttachFiler {
    SessionAttachFiler();
};

SessionAttachFiler::SessionAttachFiler() {
    g.attachMs = GetTickCount64();
    // Pin: our own thread outlives DllMain, and a refused bootstrap would otherwise see the loader
    // unload the module out from under it.
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&WriteProvisional), &g.self);

    char id[128];
    ProbeId(id, sizeof id);
    WriteProvisional(id);

    // Our own thread, not the bootstrap's. CreateThread is legal under the loader lock - the thread
    // does not run until the lock is released - and it is what bootstrap::Attach does itself.
    if (HANDLE worker = CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr)) {
        CloseHandle(worker);
    }
}

#if !W1F_NO_STATIC_FILER
SessionAttachFiler g_filer;
#else
// ★ THE MUTATION BUILD reproduces the OLD shape rather than merely deleting the new one: it files
// from a REPLAY_MODULE init, exactly as this probe did in sweep 4. So it files normally when it is
// the first DLL loaded (bootstrap up, modreg runs) and goes SILENT when it is the refused one -
// which is the defect itself, not an approximation of it. The offline proof asserts both halves.
bool ProbeInitLegacy(replay::ReplayServices* services) {
    REPLAY_REQUIRE(services, log);
    if (HANDLE worker = CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr)) {
        CloseHandle(worker);
    }
    return true;
}

REPLAY_MODULE("w1f-probe-legacy", 900, ProbeInitLegacy, nullptr, nullptr);
#endif

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (g.self == nullptr) {   // the mutation build has no static filer to have set these
                g.self = module;
                g.attachMs = GetTickCount64();
            }
            // ★ The verdict and the worker are already running - the static initialiser started them
            // before this ran. Attach is attempted for the log and the shared blocker, and its result
            // is REPORTED rather than obeyed: returning FALSE here would ask the loader to unload a
            // module whose thread is already measuring, and a refused instance claim is a normal
            // outcome in a multi-probe launch rather than a reason to disappear.
            g.bootstrapOk.store(replay::bootstrap::Attach(module), std::memory_order_release);
            return TRUE;
        case DLL_PROCESS_DETACH:
            replay::bootstrap::Detach(reserved != nullptr);
            break;
        default:
            break;
    }
    return TRUE;
}
