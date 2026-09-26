#include "bootstrap/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

#include "bootstrap/config.h"
#include "bootstrap/paths.h"

namespace replay {

LogRing::LogRing() : slots_{}, head_(0), tail_(0), dropped_(0) {
    for (size_t i = 0; i < kSlots; ++i) {
        slots_[i].v0.store(-1, std::memory_order_relaxed);
        slots_[i].v1.store(-1, std::memory_order_relaxed);
    }
}

void LogRing::Push(LogLevel level, const char* module, const char* text, size_t len, uint64_t tickMs) {
    if (len > kLogLineMax) {
        len = kLogLineMax;
    }
    const long long seq = head_.fetch_add(1, std::memory_order_relaxed);
    Slot& s = slots_[static_cast<size_t>(seq) % kSlots];

    // Seqlock: claim, fence, fill, fence, publish. A reader that sees v1 == seq and then reads v0 != seq
    // knows a producer lapped it mid-copy and discards the record instead of emitting a torn line.
    s.v0.store(seq, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    s.tickMs = tickMs;
    s.module = module;
    s.level = static_cast<uint32_t>(level);
    s.len = static_cast<uint32_t>(len);
    for (size_t i = 0; i < len; ++i) {
        s.text[i] = text[i];
    }

    std::atomic_thread_fence(std::memory_order_release);
    s.v1.store(seq, std::memory_order_relaxed);
}

bool LogRing::Pop(LogLevel* level, const char** module, char* out, size_t cap, size_t* len,
                  uint64_t* tickMs) {
    for (;;) {
        const long long head = head_.load(std::memory_order_acquire);
        if (tail_ >= head) {
            return false;
        }
        // Lapped while we were away: everything older than the last kSlots records is gone.
        if (head - tail_ > static_cast<long long>(kSlots)) {
            dropped_ += static_cast<uint64_t>(head - tail_) - kSlots;
            tail_ = head - static_cast<long long>(kSlots);
        }

        Slot& s = slots_[static_cast<size_t>(tail_) % kSlots];
        const long long published = s.v1.load(std::memory_order_acquire);
        if (published != tail_) {
            if (published > tail_) {
                ++dropped_;   // overwritten before we got to it
                ++tail_;
                continue;
            }
            return false;     // the producer that claimed this slot has not finished writing yet
        }

        size_t n = s.len;
        if (n > cap) {
            n = cap;
        }
        const LogLevel lvl = static_cast<LogLevel>(s.level);
        const char* mod = s.module;
        const uint64_t ms = s.tickMs;
        for (size_t i = 0; i < n; ++i) {
            out[i] = s.text[i];
        }

        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.v0.load(std::memory_order_relaxed) != tail_) {
            ++dropped_;       // a producer lapped us during the copy; the bytes are not trustworthy
            ++tail_;
            continue;
        }

        *level = lvl;
        *module = mod;
        *len = n;
        *tickMs = ms;
        ++tail_;
        return true;
    }
}

namespace log {
namespace {

struct State {
    LogRing  ring;
    LogLevel level;
    bool     attached;
    bool     openFailed;
    wchar_t  redirect[kPathMax];
    bool     redirecting;
    // Wall clock is sampled once, and every line's timestamp is that baseline plus a tick delta.
    // GetTickCount64 reads shared user memory; GetLocalTime does not belong on the tick path.
    uint64_t baseTickMs;
    FILETIME baseFileTime;
    LONG     ledgerDumped;
    // Held for the whole open-write-close of one file visit. Uncontended on the worker; the only other
    // taker is an ERROR line, which also has to Pop the ring and Pop has a single-consumer tail.
    SRWLOCK  fileLock;
    bool     deferred;          // the drain worker is running, so Direct may queue instead of blocking
    LONG     opens;             // CreateFileW calls, for the "no file open per line" test
    uint64_t reportedDropped;
    // What logging costs the thread that logs, so the playback cost meter can print a `log=` figure
    // instead of everyone arguing about it from line counts again.
    std::atomic<int64_t> costTicks;
};

// Namespace scope, not a function-local static. A magic static takes a guard lock on first use, and the
// first use here is the attach line, which runs under the Windows loader lock. The CRT runs this object's
// dynamic initialiser inside _DllMainCRTStartup before it calls DllMain, so by then it is simply built.
State g_state;
State& S() { return g_state; }

struct CostCharge {
    CostCharge() { QueryPerformanceCounter(&t0); }
    ~CostCharge() {
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        S().costTicks.fetch_add(t1.QuadPart - t0.QuadPart, std::memory_order_relaxed);
    }
    LARGE_INTEGER t0;
};

const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

const wchar_t* TargetPath() {
    State& s = S();
    if (s.redirecting) {
        return s.redirect;
    }
    return Resolved() ? LogPath() : nullptr;
}

HANDLE OpenForAppend() {
    const wchar_t* path = TargetPath();
    if (path == nullptr) {
        return INVALID_HANDLE_VALUE;
    }
    // The uninstall rule is enforced here rather than remembered: the log is one of exactly two paths
    // this plugin may create (S4.5.3 rule 5). A redirect is a test fixture and is exempt by construction.
    if (!S().redirecting && !IsWritablePath(path)) {
        return INVALID_HANDLE_VALUE;
    }
    InterlockedIncrement(&S().opens);
    return CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// An append-only log across every session this install has ever run reached 133 MB in the owner's Oculus
// folder, which makes every open of it slower than the last. The archive goes under the state directory
// because IsWritablePath admits exactly that and the log file itself; a sibling `.1` would be a third path.
constexpr uint64_t kRotateBytes = 32ull * 1024 * 1024;   // facts-ok: a byte cap, not a registry value

void RotateIfOversized(uint64_t capBytes) {
    const wchar_t* path = LogPath();
    if (!Resolved() || path == nullptr) {
        return;
    }
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info)) {
        return;
    }
    const uint64_t size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size < capBytes) {
        return;
    }
    wchar_t archive[kPathMax];
    if (_snwprintf_s(archive, kPathMax, _TRUNCATE, L"%s\\echoreplay.prev.log", StateDir()) < 0) {
        return;
    }
    CreateDirectoryW(StateDir(), nullptr);
    MoveFileExW(path, archive, MOVEFILE_REPLACE_EXISTING);
}

// Renders "HH:MM:SS.mmm LEVEL [module] text" into `out`. Returns the byte count, never more than cap.
size_t Render(char* out, size_t cap, LogLevel level, const char* module, uint64_t tickMs,
              const char* text, size_t textLen) {
    State& s = S();
    ULARGE_INTEGER base;
    base.LowPart = s.baseFileTime.dwLowDateTime;
    base.HighPart = s.baseFileTime.dwHighDateTime;
    const uint64_t deltaMs = (tickMs >= s.baseTickMs) ? (tickMs - s.baseTickMs) : 0;
    base.QuadPart += deltaMs * 10000ull;   // FILETIME ticks are 100 ns

    FILETIME ft;
    ft.dwLowDateTime = base.LowPart;
    ft.dwHighDateTime = base.HighPart;
    SYSTEMTIME utc;
    SYSTEMTIME local;
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
        local = SYSTEMTIME{};
    }

    const int head = std::snprintf(out, cap, "%02u:%02u:%02u.%03u %s [%s] ", local.wHour, local.wMinute,
                                   local.wSecond, local.wMilliseconds, LevelName(level),
                                   module != nullptr ? module : "-");
    if (head < 0) {
        return 0;
    }
    size_t n = static_cast<size_t>(head);
    if (n > cap) {
        n = cap;
    }
    for (size_t i = 0; i < textLen && n + 2 < cap; ++i, ++n) {
        out[n] = text[i];
    }
    if (n + 2 <= cap) {
        out[n++] = '\r';
        out[n++] = '\n';
    }
    return n;
}

void WriteBytes(HANDLE file, const char* bytes, size_t n) {
    DWORD written = 0;
    WriteFile(file, bytes, static_cast<DWORD>(n), &written, nullptr);
}

// Caller holds fileLock. Opens the file only if the ring has something, so an idle drain is one
// uncontended lock acquire. Pop has a single-consumer tail; the lock is what makes that still true now
// that an ERROR line can drain from the frame thread.
void DrainLocked() {
    State& s = S();
    LogLevel level = LogLevel::Info;
    const char* module = nullptr;
    char text[kLogLineMax];
    size_t len = 0;
    uint64_t tickMs = 0;

    if (!s.ring.Pop(&level, &module, text, sizeof text, &len, &tickMs)) {
        return;
    }
    HANDLE file = OpenForAppend();
    if (file == INVALID_HANDLE_VALUE) {
        s.openFailed = true;
        return;
    }
    char line[kLogLineMax * 2];
    do {
        const size_t rendered = Render(line, sizeof line, level, module, tickMs, text, len);
        WriteBytes(file, line, rendered);
    } while (s.ring.Pop(&level, &module, text, sizeof text, &len, &tickMs));

    const uint64_t dropped = s.ring.Dropped();
    if (dropped != s.reportedDropped) {
        char note[kLogLineMax];
        const int n = std::snprintf(note, sizeof note,
                                    "%llu log line(s) dropped so far: the ring overflowed between drains",
                                    static_cast<unsigned long long>(dropped));
        if (n > 0) {
            s.reportedDropped = dropped;
            const size_t rendered = Render(line, sizeof line, LogLevel::Warn, "log", GetTickCount64(),
                                           note, static_cast<size_t>(n));
            WriteBytes(file, line, rendered);
        }
    }
    CloseHandle(file);
}

// Synchronous: queued lines first, then this one, so the file keeps call order even though the caller
// jumped the ring.
void WriteThrough(LogLevel level, const char* module, const char* text, size_t len) {
    State& s = S();
    AcquireSRWLockExclusive(&s.fileLock);
    DrainLocked();
    HANDLE file = OpenForAppend();
    if (file == INVALID_HANDLE_VALUE) {
        s.openFailed = true;
    } else {
        char line[kLogLineMax * 2];
        const size_t rendered = Render(line, sizeof line, level, module, GetTickCount64(), text, len);
        WriteBytes(file, line, rendered);
        CloseHandle(file);
    }
    ReleaseSRWLockExclusive(&s.fileLock);
}

class LogImpl final : public ILog {
public:
    void Write(LogLevel level, const char* module, const char* fmt, ...) override {
        if (!Enabled(level)) {
            return;
        }
        CostCharge charge;
        char text[kLogLineMax];
        va_list args;
        va_start(args, fmt);
        const int n = std::vsnprintf(text, sizeof text, fmt, args);
        va_end(args);
        if (n < 0) {
            return;
        }
        size_t len = static_cast<size_t>(n);
        if (len >= sizeof text) {
            len = sizeof text - 1;   // truncate rather than grow, per the contract
        }
        S().ring.Push(level, module, text, len, GetTickCount64());
    }

    bool Enabled(LogLevel level) const override { return level >= S().level; }

    void DumpLedger() override;
};

LogImpl g_interface;

}  // namespace

bool Attach(void* selfModule) {
    State& s = S();
    s.level = LogLevel::Info;
    s.baseTickMs = GetTickCount64();
    GetSystemTimeAsFileTime(&s.baseFileTime);
    InitializeSRWLock(&s.fileLock);

    if (!Resolve(selfModule)) {
        s.openFailed = true;
        return false;
    }
    RotateIfOversized(kRotateBytes);
    DirectAlways(LogLevel::Info, "boot", "attached");
    s.attached = !s.openFailed;
    return s.attached;
}

void DirectAlways(LogLevel level, const char* module, const char* text) {
    WriteThrough(level, module, text, std::strlen(text));
}

void Direct(LogLevel level, const char* module, const char* fmt, ...) {
    // log.level means what it says on this path too. Without this, "log.level": "error" still wrote every
    // routine boot, config and coexist line, which is not what someone quieting the log asked for. The
    // attach line goes through DirectAlways instead, because it is the diagnosis rather than a message.
    if (level < S().level) {
        return;
    }
    CostCharge charge;
    char text[kLogLineMax];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    if (n < 0) {
        return;
    }
    size_t len = static_cast<size_t>(n);
    if (len >= sizeof text) {
        len = sizeof text - 1;
    }

    // ★ PERF-1. This used to be CreateFileW + WriteFile + CloseHandle per line, on whichever thread
    // called it -- and 44 of its callers are on the game's frame thread. Appending to a multi-megabyte
    // file in the Oculus install directory costs 0.2-5 ms an open under Defender, so those lines were
    // frame hitches. Once the drain worker exists they go into the same ring ILog::Write uses, with the
    // timestamp taken here at the call site. ERROR keeps the synchronous path: the question the last
    // line before a fault answers is what was happening at the fault, and there is no crash handler to
    // flush the ring for it.
    if (S().deferred && level < LogLevel::Error) {
        S().ring.Push(level, module, text, len, GetTickCount64());
        return;
    }
    WriteThrough(level, module, text, len);
}

ILog* Interface() {
    return &g_interface;
}

void Drain() {
    State& s = S();
    AcquireSRWLockExclusive(&s.fileLock);
    DrainLocked();
    ReleaseSRWLockExclusive(&s.fileLock);
}

void Shutdown() {
    S().deferred = false;   // nothing will drain after this, so the last lines write straight through
    Drain();
    const uint64_t dropped = S().ring.Dropped();
    if (dropped > S().reportedDropped) {
        Direct(LogLevel::Warn, "log", "%llu log line(s) were dropped: the worker could not keep up",
               static_cast<unsigned long long>(dropped));
    }
}

void SetDeferred(bool on) { S().deferred = on; }
bool Deferred() { return S().deferred; }
uint64_t FileOpenCount() { return static_cast<uint64_t>(S().opens); }

void RotateForTest(uint64_t capBytes) { RotateIfOversized(capBytes); }

uint64_t CostTicksTake() {
    const int64_t t = S().costTicks.exchange(0, std::memory_order_relaxed);
    return t > 0 ? static_cast<uint64_t>(t) : 0;
}

// Deliberately not in State: this is read from the tick thread by every chatter block and written
// once at startup, and giving it its own atomic keeps it off the lock the file path takes.
// 0 = unthrottled, which is what a process that never loaded a config gets: every unit test keeps
// the line counts it was written against, and only the plugin -- which always loads config --
// applies `log.chatter_period_ms`.
std::atomic<uint32_t> g_chatterPeriodMs{0};

void SetChatterPeriodMs(uint32_t ms) { g_chatterPeriodMs.store(ms, std::memory_order_relaxed); }
uint32_t ChatterPeriodMs() { return g_chatterPeriodMs.load(std::memory_order_relaxed); }

bool ChatterDue(uint64_t& last) {
    const uint32_t period = g_chatterPeriodMs.load(std::memory_order_relaxed);
    if (period == 0u) return true;
    // Tick 0 is a real time, so `last == 0` cannot mean "never printed" on its own; the first
    // call stores now+1 offsets nothing and would still print again a period later. Storing the
    // tick and treating 0 as never is right because GetTickCount64 is 0 only for the first
    // millisecond after boot, and a plugin is not loaded then.
    const uint64_t now = GetTickCount64();
    if (last != 0u && now - last < period) return false;
    last = now;
    return true;
}

void SetLevel(LogLevel level) { S().level = level; }
LogLevel Level() { return S().level; }
uint64_t Dropped() { return S().ring.Dropped(); }

void RedirectForTest(const wchar_t* path) {
    State& s = S();
    s.baseTickMs = GetTickCount64();
    GetSystemTimeAsFileTime(&s.baseFileTime);
    size_t i = 0;
    for (; path[i] != L'\0' && i + 1 < kPathMax; ++i) {
        s.redirect[i] = path[i];
    }
    s.redirect[i] = L'\0';
    s.redirecting = true;
    s.openFailed = false;
    s.opens = 0;
    s.reportedDropped = s.ring.Dropped();
    InitializeSRWLock(&s.fileLock);
}

void EndRedirectForTest() { S().redirecting = false; }

namespace {

// S4.4: "Every row is disableable by a config key, and the log prints the enabled set at startup."
// Printing the DISABLED rows as well is what makes SC7 checkable from the log alone - a .text diff has
// to be compared against the configuration that produced it, and this is that configuration.
void LogImpl::DumpLedger() {
    State& s = S();
    if (InterlockedExchange(&s.ledgerDumped, 1) != 0) {
        return;
    }
    const IConfig* cfg = config::Interface();
    unsigned enabled = 0;
    unsigned disabled = 0;
    unsigned valued = 0;
    for (size_t i = 0; i < ConfigKeyCount(); ++i) {
        const ConfigKey& k = ConfigKeys()[i];
        if (k.ledgerRow == nullptr) {
            continue;
        }
        if (k.type != ConfigType::Bool) {
            // A mode string is not an on/off row and must not be tallied as one: SC7 derives its
            // expected detour count from the enabled set, so a miscounted row is a miscounted gate.
            //
            // ★ THE INVARIANT: nothing may read a string row's EFFECTIVE value
            // before that row's provider has published it. This dump runs in bootstrap init, ahead of
            // every provider, and config refuses a publish once the key has been read -- so reading
            // the effective value here closed tick.mode's publish window for the whole process. On
            // L5 #3 the slot claim SUCCEEDED and its publish was refused after the fact, row 1 stayed
            // at `auto` forever, and SC7 deferred it in every pass. The request is the only value
            // that exists at this point and asking for it does not close the window; the resolution
            // is logged by SetEffectiveString itself, at the moment the provider publishes it.
            const char* requested = config::RequestedString(k.key);
            Write(LogLevel::Info, "ledger", "row %-5s %-32s requested = %s (resolved by its provider)",
                  k.ledgerRow, k.key, requested != nullptr ? requested : "");
            ++valued;
            continue;
        }
        const bool on = cfg->GetBool(k.key, k.boolDefault);
        Write(LogLevel::Info, "ledger", "row %-5s %-32s %s%s", k.ledgerRow, k.key,
              on ? "ENABLED" : "disabled",
              (k.flags & kConfigPinnedTrue) != 0 ? " (not refusable)" : "");
        if (on) {
            ++enabled;
        } else {
            ++disabled;
        }
    }
    Write(LogLevel::Info, "ledger",
          "%u intervention-ledger row(s) enabled, %u disabled, %u set by value", enabled, disabled,
          valued);
}

}  // namespace
}  // namespace log
}  // namespace replay
