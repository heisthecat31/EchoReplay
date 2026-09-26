#include "playback/core/windowprefetch.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <new>

#include "foundation/demofile/demofile.h"

namespace replay {
namespace playback {
namespace winprefetch {
namespace {

uint64_t NowUs() {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return freq.QuadPart ? (uint64_t)((now.QuadPart * 1000000LL) / freq.QuadPart) : 0u;
}

// ★ A RAW HANDLE, LIKE EVERY OTHER WORKER IN THIS TREE, and the struct is never destroyed. Process
// exit can kill the worker where it is parked in cv_.wait, and a destructor running after that would
// tear down a mutex and a condition variable with a dead waiter on them -- which is terminate() and
// then __fastfail, at every process exit, in a shipping game.
struct Worker {
    HANDLE thread = nullptr;
    std::mutex m;
    std::condition_variable cv;

    // Guarded by m.
    bool stop = true;
    bool req_pending = false;    // a request posted and not yet taken
    bool decoding = false;       // taken and running; a new request is refused rather than queued
    size_t req_centre = 0;
    uint32_t req_serial = 0;
    uint64_t req_id = 0;
    Window* buffer = nullptr;    // BORROWED from the caller; never freed here

    DemoReader* reader = nullptr;
    BuildScratch* scratch = nullptr;
    const WireClocks* clocks = nullptr;
    const NetVarPrimer* primer = nullptr;

    // ★ THE HANDOVER, and it is one release-store against one acquire-load. `result` is written by
    // the worker BEFORE the store and read by the tick AFTER the load, and the worker cannot write
    // it again until the tick has collected -- Request refuses while `done` is set. That ordering is
    // the whole reason the frame path needs no lock.
    Result result;
    std::atomic<Window*> done{nullptr};

    std::atomic<bool> running{false};
    std::atomic<bool> has_buffer{false};

    std::atomic<uint64_t> requests{0};
    std::atomic<uint64_t> coalesced{0};
    std::atomic<uint64_t> decodes{0};
    std::atomic<uint64_t> refused{0};
    std::atomic<uint64_t> collected{0};
    std::atomic<uint64_t> decode_us{0};
};

alignas(Worker) unsigned char g_storage[sizeof(Worker)];
Worker& g_w = *new (g_storage) Worker;

DWORD WINAPI WorkerMain(LPVOID) {
    for (;;) {
        size_t centre = 0;
        uint32_t serial = 0;
        uint64_t id = 0;
        Window* w = nullptr;
        {
            std::unique_lock<std::mutex> lk(g_w.m);
            g_w.cv.wait(lk, [] { return g_w.stop || g_w.req_pending; });
            if (g_w.stop) {
                return 0;
            }
            centre = g_w.req_centre;
            serial = g_w.req_serial;
            id = g_w.req_id;
            w = g_w.buffer;
            g_w.req_pending = false;
            g_w.decoding = true;
        }

        BuildStats st;
        bool built = false;
        const uint64_t t0 = NowUs();
        if (w != nullptr && g_w.reader != nullptr && g_w.scratch != nullptr) {
            // The same function the tick calls, with this thread's own scratch. Nothing else differs,
            // which is what makes the two paths byte-identical rather than merely similar.
            built = BuildWindowRange(*g_w.reader, centre, *w, st, g_w.clocks, g_w.primer,
                                     *g_w.scratch);
        }
        const uint64_t dt = NowUs() - t0;

        {
            std::lock_guard<std::mutex> lk(g_w.m);
            g_w.result.window = w;
            g_w.result.stats = st;
            g_w.result.centre = centre;
            g_w.result.load_serial = serial;
            g_w.result.request_id = id;
            g_w.result.built = built;
            g_w.result.decode_us = dt;
            g_w.buffer = nullptr;
            g_w.decoding = false;
        }
        g_w.decodes.fetch_add(1, std::memory_order_relaxed);
        if (!built) {
            g_w.refused.fetch_add(1, std::memory_order_relaxed);
        }
        g_w.decode_us.fetch_add(dt, std::memory_order_relaxed);
        g_w.done.store(w, std::memory_order_release);
    }
}

}  // namespace

bool Start(const wchar_t* path, const WireClocks* clocks, const NetVarPrimer* primer,
           const char** why) {
    const char* unused = nullptr;
    if (why == nullptr) {
        why = &unused;
    }
    if (g_w.running.load(std::memory_order_acquire)) {
        *why = "the window prefetch worker is already running";
        return false;
    }
    if (path == nullptr || path[0] == 0) {
        *why = "no capture path to open a second reader on";
        return false;
    }

    DemoReader* reader = new (std::nothrow) DemoReader();
    if (reader == nullptr) {
        *why = "out of memory for the prefetch reader";
        return false;
    }
    if (!reader->Open(path)) {
        const char* r = reader->RefusalReason();
        *why = r != nullptr ? r : "the prefetch reader could not open the capture";
        delete reader;
        return false;
    }
    BuildScratch* scratch = CreateBuildScratch();
    if (scratch == nullptr) {
        reader->Close();
        delete reader;
        *why = "out of memory for the prefetch decode scratch";
        return false;
    }

    bool started = false;
    {
        std::lock_guard<std::mutex> lk(g_w.m);
        g_w.stop = false;
        g_w.req_pending = false;
        g_w.decoding = false;
        g_w.req_centre = 0;
        g_w.req_serial = 0;
        g_w.req_id = 0;
        g_w.buffer = nullptr;
        g_w.reader = reader;
        g_w.scratch = scratch;
        g_w.clocks = clocks;
        g_w.primer = primer;
        g_w.result = Result();
        g_w.thread = CreateThread(nullptr, 0, &WorkerMain, nullptr, 0, nullptr);
        started = g_w.thread != nullptr;
        if (!started) {
            g_w.stop = true;
            g_w.reader = nullptr;
            g_w.scratch = nullptr;
        }
    }
    g_w.done.store(nullptr, std::memory_order_release);
    g_w.has_buffer.store(false, std::memory_order_release);
    if (!started) {
        reader->Close();
        delete reader;
        DestroyBuildScratch(scratch);
        *why = "the prefetch worker thread could not be created";
        return false;
    }
    // Per worker life. The counters back a per-minute rate the operator reads against ONE load; a
    // second file's requests summed into the first file's would make that rate unreadable.
    ResetCountersForTest();
    g_w.running.store(true, std::memory_order_release);
    return true;
}

bool Running() { return g_w.running.load(std::memory_order_acquire); }

bool g_fail_next_stop = false;

void FailNextStopForTest() { g_fail_next_stop = true; }

bool Stop(unsigned wait_ms) {
    HANDLE thread = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_w.m);
        g_w.stop = true;
        thread = g_w.thread;
    }
    g_w.cv.notify_one();
    if (thread != nullptr) {
        // ★ A TIMEOUT FREES NOTHING. The worker is still inside a decode that reads the reader, the
        // scratch and the borrowed window; taking any of them away is a use-after-free in a thread
        // nobody can see. The caller is told it failed and must keep everything alive.
        if (WaitForSingleObject(thread, wait_ms) != WAIT_OBJECT_0) {
            return false;
        }
        CloseHandle(thread);
    }

    DemoReader* reader = nullptr;
    BuildScratch* scratch = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_w.m);
        g_w.thread = nullptr;
        g_w.req_pending = false;
        g_w.decoding = false;
        g_w.buffer = nullptr;
        reader = g_w.reader;
        scratch = g_w.scratch;
        g_w.reader = nullptr;
        g_w.scratch = nullptr;
        g_w.clocks = nullptr;
        g_w.primer = nullptr;
        g_w.result = Result();
    }
    g_w.done.store(nullptr, std::memory_order_release);
    g_w.has_buffer.store(false, std::memory_order_release);
    g_w.running.store(false, std::memory_order_release);
    if (reader != nullptr) {
        reader->Close();
        delete reader;
    }
    DestroyBuildScratch(scratch);
    if (g_fail_next_stop) {
        g_fail_next_stop = false;
        return false;   // the worker IS gone; the caller is asked to behave as though it were not
    }
    return true;
}

bool GiveBuffer(Window* w) {
    if (w == nullptr || !g_w.running.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lk(g_w.m);
    if (g_w.buffer != nullptr || g_w.done.load(std::memory_order_acquire) != nullptr) {
        return false;
    }
    g_w.buffer = w;
    g_w.has_buffer.store(true, std::memory_order_release);
    return true;
}

bool HasBuffer() { return g_w.has_buffer.load(std::memory_order_acquire); }

bool Request(size_t centre, uint32_t load_serial) {
    if (!g_w.running.load(std::memory_order_acquire)) {
        return false;
    }
    // A result nobody has collected owns the only buffer. Refusing here is what keeps the worker
    // from writing `result` while the tick is reading it.
    if (g_w.done.load(std::memory_order_acquire) != nullptr) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_w.m);
        if (g_w.buffer == nullptr || g_w.decoding) {
            return false;
        }
        if (g_w.req_pending) {
            // ★ LATEST WINS, and this is where a seek storm stops costing one decode per step: the
            // centre the drag has arrived at replaces the one it passed through, in place.
            g_w.coalesced.fetch_add(1, std::memory_order_relaxed);
        }
        g_w.req_centre = centre;
        g_w.req_serial = load_serial;
        ++g_w.req_id;
        g_w.req_pending = true;
    }
    g_w.requests.fetch_add(1, std::memory_order_relaxed);
    g_w.cv.notify_one();
    return true;
}

bool Busy() {
    if (!g_w.running.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lk(g_w.m);
    return g_w.req_pending || g_w.decoding;
}

bool Collect(Result& out) {
    Window* w = g_w.done.load(std::memory_order_acquire);
    if (w == nullptr) {
        return false;
    }
    out = g_w.result;
    g_w.done.store(nullptr, std::memory_order_release);
    g_w.has_buffer.store(false, std::memory_order_release);
    g_w.collected.fetch_add(1, std::memory_order_relaxed);
    return true;
}

Counters ReadCounters() {
    Counters c;
    c.requests = g_w.requests.load(std::memory_order_relaxed);
    c.coalesced = g_w.coalesced.load(std::memory_order_relaxed);
    c.decodes = g_w.decodes.load(std::memory_order_relaxed);
    c.refused = g_w.refused.load(std::memory_order_relaxed);
    c.collected = g_w.collected.load(std::memory_order_relaxed);
    c.decode_us = g_w.decode_us.load(std::memory_order_relaxed);
    return c;
}

void ResetCountersForTest() {
    g_w.requests.store(0, std::memory_order_relaxed);
    g_w.coalesced.store(0, std::memory_order_relaxed);
    g_w.decodes.store(0, std::memory_order_relaxed);
    g_w.refused.store(0, std::memory_order_relaxed);
    g_w.collected.store(0, std::memory_order_relaxed);
    g_w.decode_us.store(0, std::memory_order_relaxed);
}

}  // namespace winprefetch
}  // namespace playback
}  // namespace replay
