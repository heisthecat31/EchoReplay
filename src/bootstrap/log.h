// B-1: the first-statement file log.
//
// Two write paths, because they answer two different questions.
//
//   Direct   - synchronous, used for the attach line, the build-guard verdict and any refusal. The loader
//              discards LoadLibraryA's return and logs nothing (R11 S0.2), so a line that is still sitting
//              in a buffer when the process dies has told nobody anything. These are a handful of lines at
//              startup, never on the tick path.
//   Ring     - ILog::Write. Wait-free append into a fixed ring the worker drains; no allocation, no lock,
//              no syscall, so it is legal from the tick thread under S6.4's budget. Overflow overwrites
//              the oldest record and increments a counter that is reported, because a silently short log
//              is worse than a log that says how much it lost.
#pragma once

#include <stdint.h>
#include <stddef.h>

#include <atomic>

#include "bootstrap/services.h"

namespace replay {

// The contract's truncation limit (B.12): a line is cut at this many bytes rather than growing a buffer.
constexpr size_t kLogLineMax = 512;

class LogRing {
public:
    LogRing();

    // Wait-free. Safe from any thread, including the tick thread.
    void Push(LogLevel level, const char* module, const char* text, size_t len, uint64_t tickMs);

    // Single consumer (the worker). Returns false when nothing is ready yet.
    bool Pop(LogLevel* level, const char** module, char* out, size_t cap, size_t* len, uint64_t* tickMs);

    uint64_t Pushed() const { return static_cast<uint64_t>(head_.load(std::memory_order_relaxed)); }
    uint64_t Dropped() const { return dropped_; }

private:
    // 256 lines of slack between drains. The worker drains far more often than that fills, and the
    // overflow path is counted rather than blocking, which is what keeps Push wait-free.
    static constexpr size_t kSlots = 256;

    struct Slot {
        std::atomic<long long> v0;      // seqlock: claimed
        uint64_t               tickMs;
        const char*            module;  // a string literal; never copied, never freed
        uint32_t               level;
        uint32_t               len;
        char                   text[kLogLineMax];
        std::atomic<long long> v1;      // seqlock: published
    };

    Slot                   slots_[kSlots];
    std::atomic<long long> head_;
    long long              tail_;
    uint64_t               dropped_;
};

namespace log {

// The very first statement of DLL_PROCESS_ATTACH. Writes the attach line synchronously and resolves the
// paths everything else derives from. Returns false if the log file could not be opened at all, which is
// the one failure this system cannot report through itself.
bool Attach(void* selfModule);

// Subject to log.level. Synchronous until the drain worker exists and for LogLevel::Error, which is what
// makes a refusal or a last-line-before-a-fault reach the disk; otherwise it queues on the same ring
// ILog::Write uses, because 44 of its callers are on the game's frame thread (PERF-1).
void Direct(LogLevel level, const char* module, const char* fmt, ...);

// Synchronous and NOT subject to log.level. Exactly one caller: the attach line, which is the only
// evidence this DLL ever ran and must not be suppressible by a setting.
void DirectAlways(LogLevel level, const char* module, const char* text);

// The ILog the modules get. Never null.
ILog* Interface();

// Worker-thread side. Drain() writes whatever the ring holds; it is the only reader.
void Drain();
void Shutdown();

// Bootstrap turns this on once the drain worker is running and off again at shutdown. While it is off
// every Direct line writes through, which is the property the boot and refusal lines were relying on.
void SetDeferred(bool on);
bool Deferred();

// Several status blocks reprint themselves verbatim many times a second for as
// long as playback runs, which is how a 2.76 MB log became 17.28 MB in 14.6 minutes. Each of those
// blocks now asks this before it formats ANYTHING -- the formatting is the cost, not the write --
// and `last` is the caller's own storage, so no two blocks share a window. Returns true the first
// time and then at most once per `log.chatter_period_ms`; a period of 0 restores every-pass
// printing, which is what a diagnostic launch sets it to.
bool ChatterDue(uint64_t& last);
void SetChatterPeriodMs(uint32_t ms);
uint32_t ChatterPeriodMs();

void     SetLevel(LogLevel level);
LogLevel Level();
uint64_t Dropped();

// CreateFileW calls made by this logger. The "no file open per line" test reads it across N ring lines.
uint64_t FileOpenCount();

// QueryPerformanceCounter ticks spent inside Write/Direct since the last call, and zeroes the counter.
// The playback cost meter turns this into the `log=` field of its per-second line.
uint64_t CostTicksTake();

// Test seam: send both paths to this file instead of the resolved install path, and stop redirecting.
// The plugin never calls these.
void RedirectForTest(const wchar_t* path);
void EndRedirectForTest();

// Attach's startup rotation, with the size cap as an argument so a test does not have to write 32 MB.
// Always acts on the resolved log path, never on a redirect.
void RotateForTest(uint64_t capBytes);

}  // namespace log
}  // namespace replay
