#include "bootstrap/inbound.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include <MinHook.h>

#include "bootstrap/bootstrap.h"
#include "bootstrap/hookmgr.h"
#include "bootstrap/log.h"

namespace replay {
namespace inbound {
namespace {

using BindFn = int(WSAAPI*)(SOCKET, const sockaddr*, int);

BindFn g_realBind;

struct State {
    Report            report;
    std::atomic<uint64_t> rewritten;
    std::atomic<uint64_t> alreadyLoopback;
    std::atomic<uint64_t> passedThrough;
    std::atomic<uint64_t> namedNonLoopback;
    std::atomic<uint64_t> refused;
    std::atomic<uint32_t> reported;   // the first-bind line is one-shot
    std::atomic<uint32_t> enforcing;
    // Worker-thread only, so plain: MaybeLogHeartbeat is called from one place, the bootstrap poll
    // loop. `scheduled` is a separate flag rather than a 0 sentinel because 0 is a reachable
    // GetTickCount64() domain value and a baseline of 0 would fire a heartbeat on the first tick.
    uint64_t          heartbeatDueMs;
    bool              heartbeatScheduled;
    bool              attempted;
    void*             target;      // the ws2_32!bind prologue we patched, for ResetForTest
    HANDLE            soleInstance;
};

State g;

// The trampoline. It must be total: an address family we have never seen, a length too short to hold
// the family we did see, and a null pointer all have to reach the real bind unmodified. A containment
// rule that drops a bind it did not understand is the rule that stops the offline host from starting.
int WSAAPI HookBind(SOCKET s, const sockaddr* name, int namelen) {
    if (g.enforcing.load(std::memory_order_relaxed) == 0) {
        return g_realBind(s, name, namelen);
    }
    sockaddr_storage rewritten{};
    const Outcome outcome = ClassifyBind(name, namelen, &rewritten, sizeof rewritten);
    const sockaddr* toBind = name;
    switch (outcome) {
        case Outcome::Rewritten:
            g.rewritten.fetch_add(1, std::memory_order_relaxed);
            toBind = reinterpret_cast<const sockaddr*>(&rewritten);
            break;
        case Outcome::AlreadyLoopback:
            g.alreadyLoopback.fetch_add(1, std::memory_order_relaxed);
            break;
        case Outcome::PassedThrough:
            g.passedThrough.fetch_add(1, std::memory_order_relaxed);
            if (IsNamedNonLoopbackBind(name, namelen)) {
                g.namedNonLoopback.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        case Outcome::Refused:
            // Unreachable: ClassifyBind never returns it. Counted rather than asserted, so that if a
            // later edit ever does return it the launch record says so instead of the process dying.
            g.refused.fetch_add(1, std::memory_order_relaxed);
            break;
    }
    // AFTER the count, so the line carries what this bind was rather than a promise, and one-shot. The
    // flag is claimed BEFORE the log call: a logger that ever binds a socket re-enters here, and a
    // second entry that found the flag unclaimed would recurse without bound.
    if (g.reported.exchange(1, std::memory_order_relaxed) == 0) {
        LogReport("first-bind");
    }
    return g_realBind(s, toBind, namelen);
}

void PublishCounters() {
    g.report.rewritten = g.rewritten.load(std::memory_order_relaxed);
    g.report.alreadyLoopback = g.alreadyLoopback.load(std::memory_order_relaxed);
    g.report.passedThrough = g.passedThrough.load(std::memory_order_relaxed);
    g.report.namedNonLoopback = g.namedNonLoopback.load(std::memory_order_relaxed);
    g.report.refused = g.refused.load(std::memory_order_relaxed);
}

}  // namespace

Mode ParseMode(const char* value) {
    if (value == nullptr) {
        return Mode::Auto;
    }
    if (std::strcmp(value, "auto") == 0) {
        return Mode::Auto;
    }
    if (std::strcmp(value, "on") == 0) {
        return Mode::On;
    }
    if (std::strcmp(value, "off") == 0) {
        return Mode::Off;
    }
    return Mode::Unrecognised;
}

const char* ModeName(Mode mode) {
    switch (mode) {
        case Mode::Auto:         return "auto";
        case Mode::On:           return "on";
        case Mode::Off:          return "off";
        case Mode::Unrecognised: return "unrecognised";
    }
    return "?";
}

const char* ArmStateName(ArmState state) {
    switch (state) {
        case ArmState::NotAttempted: return "not-attempted";
        case ArmState::Armed:        return "armed";
        case ArmState::Failed:       return "failed";
    }
    return "?";
}

const char* ArmTriggerName(ArmTrigger trigger) {
    switch (trigger) {
        case ArmTrigger::Unspecified:       return "unspecified";
        case ArmTrigger::OfflineLocked:     return "offline-lock-locked";
        case ArmTrigger::ConfigForcedOn:    return "config-forced-on";
        case ArmTrigger::NotOfflineSession: return "not-an-offline-session";
        case ArmTrigger::DisabledByConfig:  return "disabled-by-config";
        case ArmTrigger::LiveOnlineRefused: return "live-online-refused";
    }
    return "?";
}

const char* OutcomeName(Outcome outcome) {
    switch (outcome) {
        case Outcome::Rewritten:       return "rewritten";
        case Outcome::AlreadyLoopback: return "already-loopback";
        case Outcome::PassedThrough:   return "passed-through";
        case Outcome::Refused:         return "refused";
    }
    return "?";
}

const char* VerdictName(Verdict verdict) {
    switch (verdict) {
        case Verdict::AskedPass:   return "asked-pass";
        case Verdict::AskedFail:   return "asked-fail";
        case Verdict::CouldNotAsk: return "could-not-ask";
        case Verdict::NeverAsked:  return "never-asked";
    }
    return "?";
}

Decision Decide(Mode mode, bool offlineLockLocked, bool liveOnlineExempt) {
    // `off` outranks the lock: it is a deliberate override and the key exists to be obeyed.
    if (mode == Mode::Off) {
        return Decision{false, ArmTrigger::DisabledByConfig};
    }
    // And it is ABOVE `on` deliberately. `on` is a request; a live online
    // session is a fact about the process, and in that session the rewrite does not contain
    // anything -- it just stops the client reaching the host. The session always wins. The caller
    // logs the ignored key by name so an operator who set it is told, not left guessing.
    if (liveOnlineExempt) {
        return Decision{false, mode == Mode::On ? ArmTrigger::LiveOnlineRefused
                                                : ArmTrigger::NotOfflineSession};
    }
    if (mode == Mode::On) {
        return Decision{true, ArmTrigger::ConfigForcedOn};
    }
    // auto, and an unrecognised value which is treated as auto and said out loud by the caller. The
    // predicate is the LOCK, never the presence of -offline: an unregistered switch earlier on the
    // engine's command line voids the whole line silently, so the args say what was asked for and the
    // lock says what the engine did.
    if (offlineLockLocked) {
        return Decision{true, ArmTrigger::OfflineLocked};
    }
    return Decision{false, ArmTrigger::NotOfflineSession};
}

Verdict Classify(const VerdictInput& in) {
    if (!in.armed) {
        return Verdict::NeverAsked;
    }
    // BEFORE the counters clause. A wildcard bind with the counters at zero is still an exposure, and
    // grading it could-not-ask would let the one case this whole row exists for read as an instrument
    // problem rather than as the failure it is.
    if (in.boundNonLoopback > 0 || in.inboundNonLoopback > 0) {
        return Verdict::AskedFail;
    }
    if (!in.endpointsRead) {
        return Verdict::CouldNotAsk;
    }
    // The anti-vacuity clause. An all-loopback endpoint table on a launch that bound nothing is
    // indistinguishable from one this hook protected, so it is not a pass.
    if (in.rewritten + in.alreadyLoopback == 0) {
        return Verdict::CouldNotAsk;
    }
    return Verdict::AskedPass;
}

bool IsWildcardBind(const void* addr, int len) {
    if (addr == nullptr || len < static_cast<int>(sizeof(sockaddr))) {
        return false;
    }
    const sockaddr* sa = static_cast<const sockaddr*>(addr);
    if (sa->sa_family == AF_INET) {
        if (len < static_cast<int>(sizeof(sockaddr_in))) {
            return false;
        }
        return reinterpret_cast<const sockaddr_in*>(sa)->sin_addr.s_addr == htonl(INADDR_ANY);
    }
    if (sa->sa_family == AF_INET6) {
        if (len < static_cast<int>(sizeof(sockaddr_in6))) {
            return false;
        }
        const in6_addr& a = reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr;
        return std::memcmp(&a, &in6addr_any, sizeof a) == 0;
    }
    return false;
}

bool IsLoopbackBind(const void* addr, int len) {
    if (addr == nullptr || len < static_cast<int>(sizeof(sockaddr))) {
        return false;
    }
    const sockaddr* sa = static_cast<const sockaddr*>(addr);
    if (sa->sa_family == AF_INET) {
        if (len < static_cast<int>(sizeof(sockaddr_in))) {
            return false;
        }
        // The whole 127.0.0.0/8 block, not just the literal 127.0.0.1: every address in it is
        // loopback by RFC 1122, the engine's HTTP listener is only one member of it, and treating the
        // rest as non-loopback would rewrite a bind that was already unreachable.
        const uint32_t host = ntohl(reinterpret_cast<const sockaddr_in*>(sa)->sin_addr.s_addr);
        return (host >> 24) == 127;
    }
    if (sa->sa_family == AF_INET6) {
        if (len < static_cast<int>(sizeof(sockaddr_in6))) {
            return false;
        }
        const in6_addr& a = reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr;
        return std::memcmp(&a, &in6addr_loopback, sizeof a) == 0;
    }
    return false;
}

bool IsNamedNonLoopbackBind(const void* addr, int len) {
    if (addr == nullptr || len < static_cast<int>(sizeof(sockaddr))) {
        return false;
    }
    const sockaddr* sa = static_cast<const sockaddr*>(addr);
    if (sa->sa_family == AF_INET) {
        if (len < static_cast<int>(sizeof(sockaddr_in))) {
            return false;
        }
    } else if (sa->sa_family == AF_INET6) {
        if (len < static_cast<int>(sizeof(sockaddr_in6))) {
            return false;
        }
    } else {
        return false;
    }
    return !IsLoopbackBind(addr, len) && !IsWildcardBind(addr, len);
}

Outcome ClassifyBind(const void* addr, int len, void* out, size_t outCap) {
    if (IsLoopbackBind(addr, len)) {
        return Outcome::AlreadyLoopback;
    }
    if (!IsWildcardBind(addr, len)) {
        // Includes a null sockaddr, a length too short for its own family, a family we do not handle,
        // and a bind to a real non-loopback local interface address. The last one is deliberately NOT
        // rewritten: the caller named an interface, and silently moving a socket the game asked to put
        // on a specific address is a different intervention from removing a wildcard. ARCH-18 S6.4
        // makes it visible either way - the harness classifies the bound endpoint and any non-loopback
        // entry is asked-fail, so it becomes a finding rather than something this hook hides.
        return Outcome::PassedThrough;
    }
    const sockaddr* sa = static_cast<const sockaddr*>(addr);
    if (sa->sa_family == AF_INET) {
        if (outCap < sizeof(sockaddr_in)) {
            return Outcome::PassedThrough;
        }
        sockaddr_in v4{};
        std::memcpy(&v4, sa, sizeof v4);
        v4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        std::memcpy(out, &v4, sizeof v4);
        return Outcome::Rewritten;
    }
    if (outCap < sizeof(sockaddr_in6)) {
        return Outcome::PassedThrough;
    }
    sockaddr_in6 v6{};
    std::memcpy(&v6, sa, sizeof v6);
    v6.sin6_addr = in6addr_loopback;
    std::memcpy(out, &v6, sizeof v6);
    return Outcome::Rewritten;
}

ArmState Arm(ArmTrigger trigger, Mode mode, const char* modeSource) {
    if (g.attempted) {
        PublishCounters();
        return g.report.state;
    }
    g.attempted = true;
    g.report.trigger = trigger;
    g.report.mode = mode;
    g.report.modeSource = modeSource;
    g.report.hooksRequested = 1;

    // One arm per process. Two copies of this module would each patch the same prologue: MinHook
    // refuses the duplicate, and the second copy would then report Failed over a process that is in
    // fact guarded - a report that misleads in the safe-looking direction.
    wchar_t soleName[64];
    _snwprintf_s(soleName, sizeof soleName / sizeof soleName[0], _TRUNCATE,
                 L"Local\\echoreplay.inbound.%lu", GetCurrentProcessId());
    g.soleInstance = CreateMutexW(nullptr, TRUE, soleName);
    if (g.soleInstance != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g.soleInstance);
        g.soleInstance = nullptr;
        g.report.state = ArmState::Failed;
        log::Direct(LogLevel::Error, "inbound",
                    "a second loopback-only hook is already armed in this process; refusing rather "
                    "than reporting failed over a process that is already guarded. Consume "
                    "replay::inbound rather than carrying a second copy");
        return g.report.state;
    }

    const uint64_t startMs = GetTickCount64();

    if (!hookmgr::Initialized()) {
        const MH_STATUS rc = MH_Initialize();
        if (rc != MH_OK && rc != MH_ERROR_ALREADY_INITIALIZED) {
            g.report.state = ArmState::Failed;
            log::Direct(LogLevel::Error, "inbound",
                        "MH_Initialize returned %s; the offline session is NOT contained",
                        MH_StatusToString(rc));
            return g.report.state;
        }
    }

    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    if (ws2 == nullptr) {
        g.report.state = ArmState::Failed;
        log::Direct(LogLevel::Error, "inbound",
                    "ws2_32.dll is not loaded; the offline session is NOT contained");
        return g.report.state;
    }

    // By NAME here even though echovr.exe imports bind BY ORDINAL (2). GetProcAddress resolves both
    // spellings to the same export address, and MinHook rewrites the prologue AT that address - so an
    // ordinal IAT slot filled by the loader points at the very bytes we patched and still lands on the
    // trampoline. An IAT-by-name patch is what would have missed this, which is why we do not do one.
    void* target = reinterpret_cast<void*>(GetProcAddress(ws2, "bind"));
    if (target == nullptr) {
        g.report.state = ArmState::Failed;
        log::Direct(LogLevel::Error, "inbound", "bind not found in ws2_32.dll; NOT contained");
        return g.report.state;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookBind),
                      reinterpret_cast<void**>(&g_realBind)) != MH_OK) {
        g.report.state = ArmState::Failed;
        log::Direct(LogLevel::Error, "inbound", "MH_CreateHook failed for ws2_32!bind; NOT contained");
        return g.report.state;
    }
    // Queued and applied, the same one-freeze path the egress blocker takes: MH_EnableHook suspends
    // every other thread in the process to rewrite a prologue, and that freeze is exactly why this may
    // never run under the loader lock.
    if (MH_QueueEnableHook(target) != MH_OK || MH_ApplyQueued() != MH_OK) {
        MH_RemoveHook(target);
        g.report.state = ArmState::Failed;
        log::Direct(LogLevel::Error, "inbound", "enabling ws2_32!bind failed; NOT contained");
        return g.report.state;
    }

    g.target = target;
    g.report.hooksLive = 1;
    g.report.state = ArmState::Armed;
    g.report.armed = true;
    g.enforcing.store(1, std::memory_order_relaxed);

    const uint64_t nowMs = GetTickCount64();
    const uint64_t attachMs = bootstrap::AttachTickMs();
    g.report.armedAtMs = (attachMs != 0 && nowMs >= attachMs) ? (nowMs - attachMs) : 0;
    g.report.armDurationMs = nowMs - startMs;

    // The line ARCH-18 S7.1 reads LoopbackOnlyArmed out of. It names the key VALUE and its SOURCE,
    // because "armed" alone cannot be checked against what the launch asked for.
    log::Direct(LogLevel::Info, "inbound",
                "loopback-only is ARMED (%s): net.loopback_only=%s from %s. Every wildcard bind in "
                "this process is rewritten to loopback, so the offline session has no endpoint a "
                "remote peer can reach. armedAtMs=%llu",
                ArmTriggerName(trigger), ModeName(mode),
                modeSource != nullptr ? modeSource : "unknown",
                static_cast<unsigned long long>(g.report.armedAtMs));
    return g.report.state;
}

void RecordNotArmed(ArmTrigger trigger, Mode mode, const char* modeSource) {
    g.report.trigger = trigger;
    g.report.mode = mode;
    g.report.modeSource = modeSource;
    g.report.armed = false;
    // Not marked attempted: a session that becomes offline later is expected to arm at that point, and
    // a flag set here would silently refuse it.
    log::Direct(trigger == ArmTrigger::DisabledByConfig ? LogLevel::Warn : LogLevel::Info, "inbound",
                "loopback-only is NOT armed (%s): net.loopback_only=%s from %s. This process may bind "
                "a wildcard address, which is correct for online play and is never-asked rather than a "
                "pass",
                ArmTriggerName(trigger), ModeName(mode),
                modeSource != nullptr ? modeSource : "unknown");
}

const Report& Current() {
    PublishCounters();
    return g.report;
}

VerdictInput SelfView() {
    PublishCounters();
    const Report& r = g.report;
    VerdictInput in{};
    in.armed = r.armed;
    // A bind we classified is an endpoint we read. Zero of them is could-not-ask, never a pass: an
    // armed hook that saw nothing is indistinguishable from one whose trampoline was never reached.
    in.endpointsRead = (r.rewritten + r.alreadyLoopback + r.passedThrough + r.refused) > 0;
    in.boundNonLoopback = static_cast<uint32_t>(r.namedNonLoopback);
    in.inboundNonLoopback = 0;
    in.rewritten = r.rewritten;
    in.alreadyLoopback = r.alreadyLoopback;
    return in;
}

void LogReport(const char* occasion) {
    const Verdict verdict = Classify(SelfView());
    const Report& r = g.report;
    // Every parsed field is a key=value with no spaces in the value and they all precede the full stop;
    // modeSource is a sentence, so it goes last where a reader can take the rest of the line.
    log::Direct(LogLevel::Info, "inbound",
                "loopback-only report (%s): armed=%s trigger=%s rewritten=%llu alreadyLoopback=%llu "
                "passedThrough=%llu namedNonLoopback=%llu refused=%llu armedAtMs=%llu verdict=%s. "
                "net.loopback_only=%s from %s",
                occasion != nullptr ? occasion : "unspecified",
                r.armed ? "true" : "false",
                ArmTriggerName(r.trigger),
                static_cast<unsigned long long>(r.rewritten),
                static_cast<unsigned long long>(r.alreadyLoopback),
                static_cast<unsigned long long>(r.passedThrough),
                static_cast<unsigned long long>(r.namedNonLoopback),
                static_cast<unsigned long long>(r.refused),
                static_cast<unsigned long long>(r.armedAtMs),
                VerdictName(verdict),
                ModeName(r.mode),
                r.modeSource != nullptr ? r.modeSource : "unknown");
}

void NoteCaptureCloseServiced() {
    LogReport("close-request");
}

bool MaybeLogHeartbeat(uint64_t nowMs) {
    if (!g.report.armed) {
        return false;
    }
    if (!g.heartbeatScheduled) {
        g.heartbeatScheduled = true;
        g.heartbeatDueMs = nowMs + kHeartbeatIntervalMs;
        return false;
    }
    if (nowMs < g.heartbeatDueMs) {
        return false;
    }
    LogReport("heartbeat");
    g.heartbeatDueMs = nowMs + kHeartbeatIntervalMs;
    return true;
}

void ResetForTest() {
    if (g.target != nullptr) {
        MH_DisableHook(g.target);
        MH_RemoveHook(g.target);
        g.target = nullptr;
    }
    if (g.soleInstance != nullptr) {
        ReleaseMutex(g.soleInstance);
        CloseHandle(g.soleInstance);
        g.soleInstance = nullptr;
    }
    g.report = Report{};
    g.rewritten.store(0);
    g.alreadyLoopback.store(0);
    g.passedThrough.store(0);
    g.namedNonLoopback.store(0);
    g.refused.store(0);
    g.reported.store(0);
    g.enforcing.store(0);
    g.heartbeatDueMs = 0;
    g.heartbeatScheduled = false;
    g.attempted = false;
    g_realBind = nullptr;
}

}  // namespace inbound
}  // namespace replay
