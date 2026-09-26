// The child process for the registration lock's deadline, and for its mutation control.
//
// src/engine/wiretap.cpp reproduces the engine's own CR15NetGame::Listen sequence, including its
// spin on the netgame's listener lock. The engine can afford an unbounded spin because it only
// ever takes that lock from threads it schedules. We take it from the GAME THREAD -- capture::Tick
// calls RegisterTaps -- while a thread we do not own may hold it, so until 2026-09-02 a contended
// or never-released lock hung the game thread with no log and no refusal.
//
// This host holds the real lock and watches what Listen does about it. Nothing is simulated: the
// image is echovr.exe (engineimage.cpp), the broadcaster is built by the engine's own container
// code (synthengine.cpp), and the lock is the one at CR15NetGame + NETGAME.LISTENLOCK_OFF that the
// shipping path dereferences.
//
// Two targets, one source, one difference: wiretap_lockhost with the shipped wiretap.cpp, and
// wiretap_lockhost_unbounded with W1L_MUTATE_UNBOUNDED_LOCK, which restores the spin the deadline
// replaced. The first must exit kOk; the second must not exit at all. A deadline that has only
// ever been met has not been shown to be what stops the wait.

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "engine/wiretap.h"
#include "engine/wiretap_facts.h"

#include "engineimage.h"
#include "synthengine.h"
#include "../local_inputs.h"

namespace {

using namespace replay::wiretap_facts;

// Distinct from every code the CRT or a Windows fault would produce on its own, so "the child died
// some other way" cannot be read as any of these verdicts.
constexpr int kOk             = 50;
constexpr int kNoEngineImage  = 51;  // no echovr.exe: nothing was tested, and that is not a pass
constexpr int kNoBroadcaster  = 52;
constexpr int kUnavailable    = 53;
constexpr int kBaselineRefused = 54;  // the uncontended registration failed, so nothing after it means anything
constexpr int kNoRefusal      = 55;  // the held lock did not stop the registration
constexpr int kRefusalTooSlow = 56;  // it refused, but far outside the deadline it advertises
constexpr int kWrongDiagnostic = 57;
constexpr int kNotCounted     = 58;
constexpr int kSlotLeaked     = 59;  // the refused tap did not go back to the table
constexpr int kEngineTouched  = 60;  // a node for the refused symbol exists: a partial registration
constexpr int kLockStolen     = 61;  // the refusal released a lock it never took
constexpr int kNoRecovery     = 62;  // the retry after the release did not go through
constexpr int kHolderStalled  = 63;  // the scheduler never gave the holder a short enough window
constexpr int kProceedRefused = 64;  // the lock came free inside the deadline and Listen still refused

// The advertised bound is 2 ms. Ten times that is still three orders of magnitude short of the
// unbounded case, so it separates "bounded" from "not bounded" without failing on a scheduler hiccup.
constexpr double kMaxRefusalUs = 20000.0;
constexpr double kDeadlineUs   = 2000.0;

// Case C holds the lock for a window well inside the deadline. It is a busy wait, not a Sleep:
// Sleep's granularity is coarser than the whole budget.
constexpr double kHoldUs       = 300.0;
constexpr int    kHoldAttempts = 5;


double gQpcUsPerTick = 0.0;

uint64_t Now() {
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart);
}

double ElapsedUs(uint64_t from, uint64_t to) { return double(to - from) * gQpcUsPerTick; }

void Sink(const replay::WireMessage&, void*) {}

std::wstring EnginePath() { return replay_test::FindEchoVrExeW(); }

// A registered node carries the symbol it was registered for, so "did the refused Listen leave
// anything behind" is answerable directly rather than through a pool count that a cap or a
// preallocation could hold constant either way.
bool NodeCarriesSymbol(void* broadcaster, uint64_t symbol) {
    const uint64_t count = w1l::ListenerNodeCount(broadcaster);
    for (uint32_t i = 0; i < count; ++i) {
        w1l::ListenerNode node{};
        if (w1l::ReadListenerNode(broadcaster, i, node) && node.symbol == symbol) {
            return true;
        }
    }
    return false;
}

struct Holder {
    volatile long* lock;
    volatile LONG  ready;
    volatile LONG  go;
    uint64_t       holdTicks;
    double         heldUs;
};

DWORD WINAPI HoldBriefly(void* param) {
    auto* h = static_cast<Holder*>(param);
    *h->lock = 1;
    InterlockedExchange(&h->ready, 1);
    while (InterlockedCompareExchange(&h->go, 0, 0) == 0) {
        YieldProcessor();
    }
    const uint64_t start = Now();
    while (Now() - start < h->holdTicks) {
        YieldProcessor();
    }
    const uint64_t end = Now();
    *h->lock = 0;
    h->heldUs = ElapsedUs(start, end);
    return 0;
}

int Run() {
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    gQpcUsPerTick = 1000000.0 / double(freq.QuadPart);

    const uint64_t setupStart = Now();

    w1l::EngineImage image;
    std::string      error;
    if (!image.Map(EnginePath().c_str(), error)) {
        std::printf("LOCKHOST could not map echovr.exe: %s\n", error.c_str());
        return kNoEngineImage;
    }

    w1l::SynthNetGame game;
    if (!game.Build(image, error)) {
        std::printf("LOCKHOST could not build the broadcaster: %s\n", error.c_str());
        return kNoBroadcaster;
    }

    void* const netGame     = game.NetGame();
    void* const broadcaster = game.Broadcaster();
    auto* const lock        = *reinterpret_cast<volatile long**>(
        static_cast<unsigned char*>(netGame) + kListenLockOff);

    replay::IWiretap* const tap =
        replay::WiretapCreateAt(netGame, reinterpret_cast<const void*>(image.Base()));
    if (tap == nullptr || !tap->Available()) {
        std::printf("LOCKHOST wiretap unavailable: %s\n", tap ? tap->Diagnostic() : "allocation failed");
        return kUnavailable;
    }
    std::printf("LOCKHOST setup %.1f ms; lock=%p\n", ElapsedUs(setupStart, Now()) / 1000.0,
                static_cast<void*>(const_cast<long*>(lock)));

    // A. The uncontended registration, so everything below is measured against a path that works.
    if (tap->Listen(kNamePlayerStateMsg, &Sink, nullptr) == replay::IWiretap::kInvalidHandle) {
        std::printf("LOCKHOST baseline Listen refused: %s\n", tap->Diagnostic());
        return kBaselineRefused;
    }

    // B. The lock held and never released -- the shipping hazard, reproduced exactly.
    *lock = 1;
    const uint64_t t0     = Now();
    const uint16_t handle = tap->Listen(kNameComponentEvent, &Sink, nullptr);
    const double   us     = ElapsedUs(t0, Now());
    std::printf("LOCKHOST held forever: handle=0x%04X after %.1f us; %s\n", handle, us,
                tap->Diagnostic());

    if (handle != replay::IWiretap::kInvalidHandle) {
        *lock = 0;
        return kNoRefusal;
    }
    if (us > kMaxRefusalUs) {
        *lock = 0;
        return kRefusalTooSlow;
    }
    if (std::strstr(tap->Diagnostic(), "wiretap-lock-timeout") == nullptr) {
        *lock = 0;
        return kWrongDiagnostic;
    }
    if (tap->LockTimeouts() != 1) {
        *lock = 0;
        return kNotCounted;
    }
    if (tap->ActiveCount() != 1) {
        *lock = 0;
        return kSlotLeaked;
    }
    if (NodeCarriesSymbol(broadcaster, kSymComponentEvent)) {
        *lock = 0;
        return kEngineTouched;
    }
    // The sharpest of the lot: a refusal that "released" the lock would have handed the holder's
    // lock to whoever asked next.
    if (*lock != 1) {
        return kLockStolen;
    }

    *lock = 0;
    if (tap->Listen(kNameComponentEvent, &Sink, nullptr) == replay::IWiretap::kInvalidHandle) {
        std::printf("LOCKHOST no recovery after the release: %s\n", tap->Diagnostic());
        return kNoRecovery;
    }
    if (!NodeCarriesSymbol(broadcaster, kSymComponentEvent)) {
        return kNoRecovery;
    }

    // C. The lock released well inside the deadline: the wait must be a wait, not a refusal.
    //
    // Both halves are measured, because a stall of either thread makes an attempt say nothing about
    // the deadline. An attempt whose holder overran, or whose Listen was itself descheduled past the
    // deadline, is retried rather than scored; only a clean window is an answer.
    for (int attempt = 1; attempt <= kHoldAttempts; ++attempt) {
        Holder h{};
        h.lock      = lock;
        h.holdTicks = uint64_t(kHoldUs / gQpcUsPerTick);

        HANDLE thread = CreateThread(nullptr, 0, HoldBriefly, &h, 0, nullptr);
        if (thread == nullptr) {
            return kHolderStalled;
        }
        while (InterlockedCompareExchange(&h.ready, 0, 0) == 0) {
            YieldProcessor();
        }
        InterlockedExchange(&h.go, 1);

        const uint64_t c0 = Now();
        const uint16_t hc = tap->Listen(kNameDeltaUnreliableVars, &Sink, nullptr);
        const double   cUs = ElapsedUs(c0, Now());
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);

        std::printf("LOCKHOST brief hold attempt %d: held %.1f us, Listen %.1f us, handle=0x%04X\n",
                    attempt, h.heldUs, cUs, hc);

        if (hc != replay::IWiretap::kInvalidHandle) {
            if (!NodeCarriesSymbol(broadcaster, kSymDeltaUnreliableVars)) {
                return kProceedRefused;
            }
            return kOk;
        }
        if (h.heldUs < kDeadlineUs && cUs < kDeadlineUs) {
            // It refused inside its own deadline with the lock free for most of the window. That is
            // the wiretap being wrong, not the scheduler.
            return kProceedRefused;
        }
    }
    return kHolderStalled;
}

}  // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const int code = Run();
    std::printf("LOCKHOST exit=%d\n", code);
    return code;
}
