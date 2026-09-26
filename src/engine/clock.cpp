// W2-D. The replay clock: the piece that makes the transport's decisions change how fast the world
// runs. Ledger rows 0.4 (transport.clock_writes) and 0.13 (transport.watchdog_bracket).
//
// =================================================================================================
// THE MECHANISM, IN FOUR LINES, AND WHY IT IS NOT THE ONE the design WRITES DOWN.
//
//   rate       CLOCK.TIMESCALE_OFF (+0x38), a plain float. 1.0 normal, 0.1 tenth, 4.0 quad.
//   pause      CLOCK.FLAGS_OFF (+0x50) bit 1 -- short-circuits the heartbeat to a raw delta of 0.
//   frame-step CLOCK.FLAGS_OFF bit 2, self-clearing, taking CLOCK.STEPONCE_OFF (+0x98) as the delta.
//   floor      CLOCK.MINELAPSED_OFF (+0x58) = 1, written BEFORE the pause bit is ever set.
//
// S7.3.1 writes the external step to +0x90 instead. That is retired by the registry, not by taste:
// CLOCK.EXTSTEP_PERFRAME_WRITE_VA records that TICK.UPDATE_CLOCK rewrites +0x90 every frame on one
// path and clears CLOCK.EXTSTEP_ENABLE_BIT every frame on the other, so an externally written step
// does not survive one frame either way. The timescale is not rewritten by anything -- see below --
// and it scales all three of a frame's heartbeats, so the beats keep PARTITIONING the frame.
//
// =================================================================================================
// WHAT THIS PACKET ESTABLISHED OFFLINE, BECAUSE IT DECIDES WHETHER PAUSE IS SHIPPABLE.
//
// sub_1400CD8F0 is CClock's constructor -- it stores CLOCK.VTABLE into [this+0] as its fourth
// instruction -- and it installs:
//
//     +0x38 = 1.0f      +0x50 = 0     +0x58 = 0     +0x60 = 10,000,000     +0x90 = +0x98 = 33333
//
// ★★★ CLOCK.MINELAPSED_OFF IS ZERO, and an image-wide sweep of every store to +0x58 found the
// constructor to be its only writer among the CClock family. The design reason 2 and the
// registry row both record this value as UNMEASURED and made every pause design conditional on it.
// It is measured now, and it came back on the DANGEROUS side:
//
//     paused, no step armed   ->  raw = 0                                (0x1400CF406)
//     pre-multiply clamp      ->  clamp(0, 0, 66666) = 0                 (0x1400CF5CE..0x1400CF5DD)
//     scale and round         ->  round(0.0f * scale) = 0
//     post-multiply clamp     ->  clamp(0, 0, 66666) = 0                 (0x1400CF6B0..0x1400CF6BA)
//     +0x68 = 0  ->  +0x3C = 0.0f  ->  +0x40 = 1.0f / 0.0f = +inf        (0x1400CF6FB)
//
// So the ENGINE'S OWN PAUSE BIT PRODUCES +inf on a stock clock, and CLOCK.RECIP_DIVIDE_VA's warning
// is not hypothetical. Writing the 1 us floor first makes a zero scaled delta unrepresentable
// anywhere in the engine, which is what the design ratified when it said the floor "retires
// the whole hazard class rather than avoiding it at one site". It is a RAISE from 0, never a
// lowering, so it cannot floor away a slow-motion rate -- the other half of that hazard, which the
// same measurement refutes: with a minimum of 0, slow motion is not clamped at all.
//
// ★ The consequence for shipping is a REFUSAL, not a caveat: if the floor cannot be written, this
// module does not set the pause bit. ClockRefusal::NoFloor.
//
// =================================================================================================
// WHO ELSE WRITES THESE FIELDS. Enumerated rather than assumed, because "I read this function" does
// not cover what it calls, and a field somebody else rewrites per frame is not a control.
//
//   +0x38  the constructor, once, and nothing else in the image. Uncontended.
//   +0x58  the constructor, once, and nothing else in the image. Uncontended.
//   +0x50  read-modify-written by TICK.UPDATE_CLOCK (clears bits 3 and 4 every frame) and by
//          `or [rax+50h],1` / `and [rax+50h],~1` elsewhere. NOTHING in the image touches bit 1 or
//          bit 2, which is why the registry says they are ours and nothing will contend for them --
//          and why release CLEARS OUR TWO BITS rather than writing the snapshotted word back. A
//          whole-word restore would clobber every bit the engine has changed since we looked.
//   +0x60  CLOCK.SETMAXELAPSED_US, on all THREE clocks in one call. We do not write it: raising it
//          would also raise the engine's hitch clamp, and the transport micro-seeks above the
//          ceiling instead. We only read it.
// =================================================================================================

#include "engine/clock.h"

#include "bootstrap/buildguard.h"
#include "bootstrap/pageprobe.h"
#include "bootstrap/livesafety.h"
#include "bootstrap/services.h"
#include "engine/clock_facts.h"
#include "engine/clock_policy.h"
#include "engine/tick.h"
#include "generated/facts_generated.h"
#include "playback/core/transport.h"

#include <windows.h>

#include <string.h>

namespace replay {
namespace clock {
namespace {

using namespace clock_facts;

const char* const kReplayModuleName = "clock";

// A wall interval between two of our ticks longer than this is not a frame and not evidence about
// one. Generous on purpose: its job is to catch a broken QPC, not to police hitches -- a real hitch
// is a correct measurement of a slow frame and the frame-span check below already excludes it.
const uint64_t kMaxWallSpanUs = 60ull * 1000000ull;

// ------------------------------------------------------------------------------------------------
// Mapped-image helpers. ★ echovr.exe is DYNAMICBASE + HIGH_ENTROPY_VA and this has bitten five
// packets, with a SILENT failure: a preferred-base VA dereferences unmapped memory and the read
// comes back false, which reads as "the engine isn't ready yet". Everything here goes through
// buildguard, which rebases and range-checks in one place.
// ------------------------------------------------------------------------------------------------

struct ImageSpan {
    const uint8_t* base = nullptr;
    size_t size = 0;
    bool Contains(const void* p) const {
        const uint8_t* q = static_cast<const uint8_t*>(p);
        return base != nullptr && q >= base && q < base + size;
    }
};

ImageSpan MainImageSpan() {
    ImageSpan s;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (base == nullptr) {
        return s;
    }
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return s;
    }
    const IMAGE_NT_HEADERS64* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return s;
    }
    s.base = base;
    s.size = nt->OptionalHeader.SizeOfImage;
    return s;
}

// A CClock instance lives on the heap, not in the image, so it cannot be range-checked against the
// module. This asks the OS instead, once per take -- never on the tick path.
bool CommittedWith(const void* p, size_t bytes, DWORD protections) {
    if (p == nullptr) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT) {
        return false;
    }
    if ((mbi.Protect & protections) == 0 || (mbi.Protect & PAGE_GUARD) != 0) {
        return false;
    }
    const uint8_t* end = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    return static_cast<const uint8_t*>(p) + bytes <= end;
}

constexpr DWORD kWritableProtections =
    PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

// Writable still has to ask the memory manager -- a read touch cannot prove a page takes a store.
// Readable does not, and TopUpClocks/AddClock/IsCClock run it on the tick path (PERF-4).
bool Readable(const void* p, size_t bytes) { return PageSpanReadable(p, bytes); }

bool Writable(const void* p, size_t bytes) { return CommittedWith(p, bytes, kWritableProtections); }

const void* Deref(uint64_t pcVa) {
    const void* slot = buildguard::MappedAddress(pcVa);
    if (slot == nullptr || !Readable(slot, sizeof(void*))) {
        return nullptr;
    }
    return *reinterpret_cast<void* const volatile*>(slot);
}

// ------------------------------------------------------------------------------------------------
// One CClock we drive.
//
// ★★ THE TYPE CHECK IS A PROPERTY CHECK, NOT AN IDENTITY CHECK. An object is treated as a CClock
// only when the qword at [obj+0] points at a table whose slot +8 holds TICK.HEARTBEAT_BEFORE --
// that is, when this object is stepped by the very function whose clamp order every offset here was
// derived from. Comparing the table POINTER against CLOCK.VTABLE would be an identity check, and a
// derived class with its own table would fail it while behaving identically. The vtable identity is
// still recorded, as a second witness, because two witnesses that disagree is information.
//
// This is what makes CLOCK.THIRD_PTR safe to write. The registry knows it only as an address
// SetMaxElapsedUS stores +0x60 to; its layout at +0x38 and +0x50 is an INFERENCE from one field.
// The check turns that inference into a runtime fact, or refuses the clock.
// ------------------------------------------------------------------------------------------------
struct ClockRef {
    const char* name = "";
    volatile uint8_t* p = nullptr;
    bool verified = false;
    bool vtableIdentityMatched = false;

    // What we found before we touched anything, so release can put it back.
    uint32_t snapTimescaleBits = 0;
    uint64_t snapMinElapsed = 0;
    bool snapped = false;

    template <typename T>
    T Load(uint32_t off) const {
        T v;
        memcpy(&v, const_cast<const uint8_t*>(p) + off, sizeof(T));
        return v;
    }
    template <typename T>
    void Store(uint32_t off, T v) {
        memcpy(const_cast<uint8_t*>(p) + off, &v, sizeof(T));
    }
};

bool IsCClock(const void* obj, const ImageSpan& img, bool& identityOut) {
    identityOut = false;
    if (!Readable(obj, sizeof(void*))) {
        return false;
    }
    const void* vt = *reinterpret_cast<void* const volatile*>(obj);
    if (!img.Contains(vt)) {
        return false;
    }
    const void* wantVtable = buildguard::MappedAddress(kVtableVa);
    identityOut = (wantVtable != nullptr && vt == wantVtable);

    const uint8_t* slot = static_cast<const uint8_t*>(vt) + kVtblHeartbeatOff;
    if (!img.Contains(slot) || !img.Contains(slot + sizeof(void*) - 1)) {
        return false;
    }
    const void* heartbeat = *reinterpret_cast<void* const volatile*>(slot);
    const void* wantHeartbeat = buildguard::MappedAddress(facts::TICK_HEARTBEAT_BEFORE);
    return wantHeartbeat != nullptr && heartbeat == wantHeartbeat;
}

// ------------------------------------------------------------------------------------------------

struct State {
    ReplayServices* svc = nullptr;
    ILog* log = nullptr;

    ClockRefusal refusal = ClockRefusal::Disabled;
    bool enabled = false;
    bool bracketEnabled = false;
    bool floorEnabled = false;
    bool pauseEnabled = false;
    bool stepEnabled = false;
    bool mirrorEnabled = false;

    ImageSpan img;

    ClockRef clocks[3];
    uint32_t clockCount = 0;
    ClockRef* measured = nullptr;   // the global: the one CNSProvider::Update reads
    bool held = false;
    bool everHeld = false;   // a take succeeded at some point; what Release() had to put back
    bool released = false;   // Release() has run; the take below must not undo it next frame
    bool idleLogged = false; // the "not engaged" line is one per idle spell, not one per frame
    SessionWatch session;

    // TopUpClocks re-offers an unstepped clock every tick; this is what stops the refusal being
    // retold every tick. The counts it keeps are copied onto the status line below.
    clock::UnsteppedReporter unstepped;

    // Commanded state. Written by the interface, consumed once per frame by Apply.
    float commandedRate = 1.0f;
    bool commandedPaused = true;
    uint64_t durationUs = 0;
    int64_t playheadUs = 0;
    uint64_t pendingStepUs = 0;
    bool stepArmedLastFrame = false;
    // The pre-roll owns `commandedRate`/`commandedPaused` while it runs; the mirror below hands
    // them straight back the frame it ends.
    bool preRollActive = false;

    // What is actually in the register, so a write only happens on a change.
    float writtenTimescale = 0.0f;
    bool writtenPaused = false;
    bool timescaleValid = false;

    uint64_t maxElapsedUs = facts::CLOCK_MAXELAPSED_US;
    bool maxElapsedLive = false;
    uint64_t minElapsedAtTake = 0;

    // Measurement.
    RateMeter rate;
    RateMeter period;
    bool haveSample = false;
    uint64_t prevSimUs = 0;
    uint64_t prevQpc = 0;
    uint32_t prevFrameIndex = 0;
    uint64_t qpcFreq = 0;
    uint64_t lastStateReportQpc = 0;
    uint64_t lastGoodSimDeltaUs = 0;
    bool cappedByCeiling = false;

    // Playhead-move inference.
    bool havePrevPlayhead = false;
    int64_t prevPlayheadUs = 0;

    // Watchdog.
    void* watchdog = nullptr;
    bool watchdogVerified = false;
    LONG burstDepth = 0;

    ClockStatus st = {};
};

State g;

uint64_t Qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return static_cast<uint64_t>(v.QuadPart);
}

// ------------------------------------------------------------------------------------------------
// Taking and releasing the clock.
// ------------------------------------------------------------------------------------------------

bool VerifySites() {
    // The two functions this module's whole arithmetic rests on. We call neither -- but every offset
    // and every clamp order here was read out of TICK.HEARTBEAT_BEFORE, and the claim that our two
    // flag bits survive the frame is read out of TICK.UPDATE_CLOCK's read-modify-writes. If either
    // is not the bytes we measured, those derivations describe a different program.
    if (!buildguard::VerifySite("TICK.HEARTBEAT_BEFORE", facts::TICK_HEARTBEAT_BEFORE,
                                facts::TICK_HEARTBEAT_BEFORE_PROLOGUE,
                                sizeof facts::TICK_HEARTBEAT_BEFORE_PROLOGUE)) {
        return false;
    }
    if (!buildguard::VerifySite("TICK.UPDATE_CLOCK", facts::TICK_UPDATE_CLOCK,
                                facts::TICK_UPDATE_CLOCK_PROLOGUE,
                                sizeof facts::TICK_UPDATE_CLOCK_PROLOGUE)) {
        return false;
    }
    return true;
}

void AddClock(const char* name, const void* obj) {
    if (obj == nullptr || g.clockCount >= 3) {
        return;
    }
    for (uint32_t i = 0; i < g.clockCount; ++i) {
        if (const_cast<const uint8_t*>(g.clocks[i].p) == obj) {
            return;   // the same object reached two ways is one clock, not two
        }
    }
    bool identity = false;
    if (!IsCClock(obj, g.img, identity)) {
        const bool say = g.unstepped.ShouldLog(name, obj);
        g.st.unsteppedReports = g.unstepped.Reports();
        g.st.unsteppedSuppressed = g.unstepped.Suppressed();
        if (say && g.log != nullptr) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "%s at %p is not stepped by TICK.HEARTBEAT_BEFORE; not writing it. This "
                         "is re-offered every tick, so further identical reports are counted on "
                         "the status line, not logged", name, obj);
        }
        return;
    }
    g.unstepped.Accepted(name);
    ClockRef& c = g.clocks[g.clockCount++];
    c.name = name;
    c.p = static_cast<volatile uint8_t*>(const_cast<void*>(obj));
    c.verified = true;
    c.vtableIdentityMatched = identity;
}

// Resolve the clocks. Returns false while the engine has not built them yet, which is a normal
// early-frame answer and not a refusal.
bool Resolve() {
    g.clockCount = 0;
    g.measured = nullptr;

    const void* global = Deref(facts::CLOCK_GLOBAL_PTR);
    if (global == nullptr) {
        return false;
    }
    AddClock("global", global);
    if (g.clockCount == 0) {
        g.refusal = ClockRefusal::NotACClock;
        return false;
    }
    // The one CNSProvider::Update reads, so the one every measurement here is taken from
    // (NSPROVIDER.UPDATE_DELTA_READ_VA). Scaling the wrong clock produces a plugin that appears to
    // configure the clock and changes nothing observable.
    g.measured = &g.clocks[0];

    AddClock("third", Deref(facts::CLOCK_THIRD_PTR));

    const void* game = Deref(facts::GAME_SINGLETON_VA);
    if (game != nullptr && Readable(static_cast<const uint8_t*>(game) + facts::GAME_CLOCK_PTR_OFF,
                                   sizeof(void*))) {
        AddClock("game", *reinterpret_cast<void* const volatile*>(
                             static_cast<const uint8_t*>(game) + facts::GAME_CLOCK_PTR_OFF));
    }
    return true;
}

void ReadMaxElapsed() {
    if (g.measured == nullptr) {
        return;
    }
    const uint64_t live = g.measured->Load<uint64_t>(facts::CLOCK_MAXELAPSED_OFF);
    // A zero here would make every plausibility bound zero and reject every sample forever, so the
    // constant substitutes -- and says that it did, because a substituted number reported as a live
    // reading is how a wrong ceiling becomes an unexplained one.
    if (live == 0) {
        g.maxElapsedUs = facts::CLOCK_MAXELAPSED_US;
        g.maxElapsedLive = false;
    } else {
        g.maxElapsedUs = live;
        g.maxElapsedLive = true;
    }
}

uint32_t DrivenClocks() {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g.clockCount; ++i) {
        n += g.clocks[i].verified ? 1u : 0u;
    }
    return n;
}

struct MinElapsedField {
    ClockRef& c;
    void Store(uint64_t v) { c.Store<uint64_t>(facts::CLOCK_MINELAPSED_OFF, v); }
    uint64_t Load() const { return c.Load<uint64_t>(facts::CLOCK_MINELAPSED_OFF); }
};

bool InstallFloor(ClockRef& c, uint64_t& readBack) {
    MinElapsedField f{c};
    return clock::InstallFloor(f, kFloorUs, c.snapMinElapsed, readBack);
}

// ★ A CLOCK WHOSE FLOOR DID NOT HOLD IS NOT DRIVEN. Apply() sets the pause bit on every clock it
// drives, and on a zero minimum that is the scaled delta 0 / reciprocal +inf outcome the floor
// exists to prevent. The clock stays in the array, un-verified, so AddClock's identity check keeps
// finding it and the next tick does not re-add, re-write and re-refuse it. InstallFloor has already
// put its snapshot back.
void DropUndrivable(ClockRef& c, uint64_t readBack) {
    c.verified = false;
    if (g.log != nullptr) {
        g.log->Write(LogLevel::Error, kReplayModuleName,
                     "wrote %llu to the %s clock's CLOCK.MINELAPSED_OFF and read back %llu; that "
                     "clock is NOT driven, because the pause bit on a zero minimum yields +inf; "
                     "now driving %u of %u",
                     (unsigned long long)kFloorUs, c.name, (unsigned long long)readBack,
                     DrivenClocks(), g.clockCount);
    }
}

bool Take() {
    if (g.held) {
        return true;
    }
    if (g.released) {
        // The refusal is left alone: Released was stamped by ReleaseHeld() if there was a take to
        // release, and otherwise it still names why there never was one.
        return false;
    }
    if (!buildguard::Passed()) {
        g.refusal = ClockRefusal::BuildGuard;
        return false;
    }
    g.img = MainImageSpan();
    if (g.img.base == nullptr) {
        g.refusal = ClockRefusal::NoModule;
        return false;
    }
    if (!VerifySites()) {
        g.refusal = ClockRefusal::SiteMismatch;
        return false;
    }
    if (!Resolve()) {
        if (g.refusal != ClockRefusal::NotACClock) {
            g.refusal = ClockRefusal::NoGlobalClock;
        }
        return false;
    }

    for (uint32_t i = 0; i < g.clockCount; ++i) {
        ClockRef& c = g.clocks[i];
        c.snapTimescaleBits = c.Load<uint32_t>(facts::CLOCK_TIMESCALE_OFF);
        c.snapMinElapsed = c.Load<uint64_t>(facts::CLOCK_MINELAPSED_OFF);
        c.snapped = true;
    }
    g.minElapsedAtTake = g.measured->snapMinElapsed;

    // ★ THE FLOOR GOES DOWN BEFORE ANYTHING ELSE, AND ITS FAILURE IS A REFUSAL. Until +0x58 is
    // non-zero, setting the pause bit drives the scaled delta to 0 and the reciprocal to +inf -- and
    // the offline measurement says +0x58 IS zero on a stock clock. Pause without the floor is not a
    // degraded feature, it is a crash we chose.
    // Read back PER CLOCK, not once from the global: Apply() sets the pause bit on every clock in
    // the array, so a floor that held on the global and not on the third would pause the third
    // into +inf. The global's failure is the whole take refused; another clock's failure drops
    // that clock and drives the rest.
    if (g.floorEnabled) {
        for (uint32_t i = 0; i < g.clockCount; ++i) {
            ClockRef& c = g.clocks[i];
            uint64_t back = 0;
            if (InstallFloor(c, back)) {
                continue;
            }
            if (&c != g.measured) {
                DropUndrivable(c, back);
                continue;
            }
            // ★ THE REFUSAL PATH RESTORES WHAT IT WROTE. Without this the floor stays installed on
            // every clock whose write DID take, ReleaseHeld() cannot undo it (it returns on !held),
            // and the next take snapshots our own 1 as if it were the engine's -- so the field can
            // never be put back for the rest of the process. An exit path that leaks is still a
            // leak when it is the unlikely one.
            for (uint32_t j = 0; j < g.clockCount; ++j) {
                ClockRef& other = g.clocks[j];
                if (other.snapped) {
                    other.Store<uint64_t>(facts::CLOCK_MINELAPSED_OFF, other.snapMinElapsed);
                }
            }
            g.refusal = ClockRefusal::NoFloor;
            if (g.log != nullptr) {
                g.log->Write(LogLevel::Error, kReplayModuleName,
                             "wrote %llu to CLOCK.MINELAPSED_OFF and read back %llu; refusing to "
                             "pause, because the pause bit yields a zero scaled delta and +inf "
                             "without it",
                             (unsigned long long)kFloorUs, (unsigned long long)back);
            }
            return false;
        }
    }

    ReadMaxElapsed();
    g.timescaleValid = false;
    g.writtenPaused = false;
    g.held = true;
    g.everHeld = true;
    g.refusal = ClockRefusal::None;

    if (g.log != nullptr) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "took %u CClock(s); CLOCK.MINELAPSED_OFF was %llu, now %llu; "
                     "CLOCK.MAXELAPSED_OFF %llu (%s)",
                     DrivenClocks(), (unsigned long long)g.minElapsedAtTake,
                     (unsigned long long)(g.floorEnabled ? kFloorUs : g.minElapsedAtTake),
                     (unsigned long long)g.maxElapsedUs,
                     g.maxElapsedLive ? "live" : "CLOCK.MAXELAPSED_US substituted");
    }
    return true;
}

// Puts every register back and drops the hold. `terminal` is the difference between the interface's
// Release() -- stop for the rest of the process -- and a replay being unloaded, after which a second
// file loaded in the same session must be able to take the clock again. Everything the two share is
// the restore itself, so there is one loop rather than two that can drift apart.
constexpr RegisterLayout kRestoreLayout = {facts::CLOCK_TIMESCALE_OFF, facts::CLOCK_MINELAPSED_OFF,
                                           facts::CLOCK_FLAGS_OFF, kPauseBit | kStepOnceBit};
static_assert(facts::CLOCK_MINELAPSED_OFF > facts::CLOCK_TIMESCALE_OFF &&
                  facts::CLOCK_MINELAPSED_OFF > facts::CLOCK_FLAGS_OFF,
              "ClockAlive spans up to the highest register the restore stores");

// The engine frees its CClocks when it shuts down and the pointers here were cached at take time.
// Launch 07:37 wrote the saved 1.0f timescale through one of them 2 s after LogOut and took an
// unhandled 0xC0000005. One VirtualQuery per clock per restore -- never on the tick path.
bool ClockAlive(const void* clock) {
    bool identity = false;
    return Writable(clock, facts::CLOCK_MINELAPSED_OFF + sizeof(uint64_t)) &&
           IsCClock(clock, g.img, identity);
}

void RestoreHeld(bool terminal) {
    // The replay went away under the pre-roll: it has nothing left to prepare for, and leaving the
    // flag set would make the next load's first mirror a no-op.
    g.preRollActive = false;
    // A module that never registered has nothing to put back, and saying "released" would erase the
    // Disabled refusal the panel prints. Shutdown() reaches this on every process, enabled or not.
    if (!g.enabled) {
        return;
    }
    // ★ RELEASE BEFORE THE FIRST TAKE STILL MEANS STOP. The restore loop below has nothing to do,
    // but the latch is the half that matters: without it a caller who releases at frame 0 gets a
    // module that takes the clock on frame 1 and drives it for the rest of the session.
    const bool wasHeld = g.held;
    const bool alreadyReleased = g.released;
    uint32_t restored = 0;
    uint32_t gone = 0;
    for (uint32_t i = 0; wasHeld && i < g.clockCount; ++i) {
        ClockRef& c = g.clocks[i];
        if (!c.verified || !c.snapped) {
            continue;
        }
        const SavedRegisters saved = {c.snapTimescaleBits, c.snapMinElapsed};
        if (RestoreRegisters(const_cast<uint8_t*>(c.p), kRestoreLayout, saved, &ClockAlive)) {
            ++restored;
        } else {
            ++gone;
        }
    }
    if (gone != 0 && g.log != nullptr) {
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "%u of %u held CClock(s) are no longer CClocks -- the engine freed them before "
                     "this restore; left alone rather than written",
                     gone, restored + gone);
    }
    g.held = false;
    if (!terminal) {
        // An unload is not a release: the latch below would make the next file's playback silently
        // inert for the rest of the session. Idle is what stops the re-take, and it lifts by itself
        // the moment DurationUs() is non-zero again.
        ++g.st.disengages;
        g.refusal = ClockRefusal::NotEngaged;
        g.timescaleValid = false;
        g.writtenPaused = false;
        g.stepArmedLastFrame = false;
        g.pendingStepUs = 0;
        g.havePrevPlayhead = false;
        g.rate.Reset();
        if (g.log != nullptr && wasHeld) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "replay unloaded or session ended: handed back %u CClock(s); timescale, "
                         "floor and our two flag bits restored. The clock is taken again when a "
                         "replay is loaded in a running match",
                         restored);
        }
        return;
    }
    // ★ AND STAY RELEASED. Clearing the bits is not a restore if the next tick takes the clock
    // again: Tick's gate was keyed on `held` alone, so one frame later Take() re-wrote the floor
    // over the value restored above and Apply() re-set the pause bit from a commandedPaused nobody
    // had changed. Launch 10 measured both -- flags 0x2 and minElapsed 1 against 0 at take -- and
    // the bits above were never the missing write; this latch is.
    g.released = true;
    // ★ RELEASED IS THE REASON ONLY WHEN THERE WAS A TAKE TO RELEASE. An enabled module that never
    // got the clock -- NoGlobalClock, SiteMismatch, BuildGuard, NotACClock, NoFloor -- keeps that
    // reason; stamping Released over it would erase the one diagnosis the NOT-RUN population has,
    // and would let the probe's "stopped after release" read ok for a clock nobody ever took.
    // Status().released carries the latch on its own.
    g.refusal = clock::RefusalAfterRelease(wasHeld, g.refusal);
    g.timescaleValid = false;
    g.writtenPaused = false;
    g.stepArmedLastFrame = false;
    g.pendingStepUs = 0;
    if (g.log != nullptr && !alreadyReleased) {
        if (wasHeld) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "released %u CClock(s); timescale, floor and our two flag bits restored, "
                         "and this is terminal -- the clock is not taken again in this process. "
                         "%llu unstepped-clock report(s), %llu of them counted rather than logged",
                         g.clockCount,
                         static_cast<unsigned long long>(g.st.unsteppedReports),
                         static_cast<unsigned long long>(g.st.unsteppedSuppressed));
        } else {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "released before any clock was taken; nothing was written, and none will "
                         "be taken in this process");
        }
    }
}

void ReleaseHeld() { RestoreHeld(true); }

// ------------------------------------------------------------------------------------------------
// The measurement. Every difference below is classified before it is used.
// ------------------------------------------------------------------------------------------------

void Measure(const TickInfo& info) {
    const uint64_t nowQpc = Qpc();

    if (!g.haveSample) {
        g.haveSample = true;
        g.prevSimUs = info.simTimeUs;
        g.prevQpc = nowQpc;
        g.prevFrameIndex = info.frameIndex;
        return;
    }

    // u32 subtraction, and the wrap is the right arithmetic: the engine's counter only increments,
    // so the difference is the number of frames since our last tick however the value is spelled.
    const uint32_t frames = info.frameIndex - g.prevFrameIndex;

    SimBounds bounds;
    bounds.maxElapsedUs = g.maxElapsedUs;
    bounds.maxBeatsPerFrame = kMaxBeatsPerFrame;

    uint64_t simDelta = 0;
    const SampleKind sk = ClassifySim(g.prevSimUs, info.simTimeUs, frames, bounds, simDelta);
    uint64_t wallDelta = 0;
    const SampleKind wk = ClassifyWall(g.prevQpc, nowQpc, g.qpcFreq, kMaxWallSpanUs, wallDelta);

    g.prevSimUs = info.simTimeUs;
    g.prevQpc = nowQpc;
    g.prevFrameIndex = info.frameIndex;

    // The frame period is a property of the display and has nothing to do with the rate, so it is
    // metered separately and is NOT reset when the rate changes. Folding the two would make the
    // fast-forward ceiling read "unknown" for ten frames after every transport command.
    if (wk == SampleKind::Normal && frames == 1) {
        g.period.Feed(0, wallDelta);
    } else {
        g.period.Reject();
    }

    // ★★ THE FRAME-SPAN GATE, and it is the one that keeps a level load out of the rate. Our tick
    // does not run during a load; the next sample then spans eight seconds of wall time against a
    // sim advance the engine clamped to almost nothing, and the ratio is a perfectly arithmetically
    // correct measurement OF THE WRONG POPULATION. A frame delta is only a frame delta when exactly
    // one frame separates the two samples.
    if (frames != 1) {
        ++g.st.skippedFrameSamples;
        g.rate.Reject();
        return;
    }
    if (sk == SampleKind::Regressed) {
        // ★★★ THE ENGINE RESET A CLOCK. W1-D's frameDeltaUs reports this case as ZERO -- correctly,
        // since it cannot produce a meaningful difference -- and zero is exactly what a legitimately
        // paused frame produces, and what a fully floored clock produces. Three causes, one value.
        // This module keeps its own previous sample precisely so the three stay distinguishable,
        // and it is why the rejection is COUNTED rather than folded into a plausible reading.
        ++g.st.simRegressions;
        g.rate.Reject();
        return;
    }
    if (sk == SampleKind::Implausible) {
        ++g.st.simImplausible;
        g.rate.Reject();
        return;
    }
    if (wk != SampleKind::Normal) {
        // Includes Stalled: two ticks inside one QPC period give a zero denominator, and a rate is
        // not defined without one. Counted, not clamped to something that looks like an answer.
        ++g.st.wallRejected;
        g.rate.Reject();
        return;
    }

    g.rate.Feed(simDelta, wallDelta);
    g.lastGoodSimDeltaUs = simDelta;
}

// ------------------------------------------------------------------------------------------------
// The write. One place, once per frame, on the tick thread -- so "a rate change lands on beats 2
// and 3 of the frame it was written in" is a statement about one instruction, not about whatever
// thread happened to call SetRate.
// ------------------------------------------------------------------------------------------------

void Apply(const TickInfo& info) {
    double period = 0.0;
    const double ceiling =
        g.period.FramePeriodUs(period) ? Ceiling(g.maxElapsedUs, period) : 0.0;

    const TimescalePlan plan = PlanTimescale(g.commandedRate, ceiling);
    g.cappedByCeiling = plan.cappedByCeiling;

    const bool wantPaused = g.pauseEnabled && g.commandedPaused;

    // ★ ARM FOR EXACTLY ONE FRAME. The step bit fires on the next BEAT, not the next frame, and
    // there are two or three beats per frame -- so a bit left set across frames steps two or three
    // times. It is set in one frame's write and CLEARED in the next one's, which also means a bit
    // the engine did not consume (a clock nothing heartbeats) cannot fire later.
    const StepArmPlan sp =
        PlanStepArm(g.stepEnabled, wantPaused, g.pendingStepUs, g.stepArmedLastFrame, g.maxElapsedUs);
    const uint64_t armUs = sp.armUs;
    if (sp.refused) {
        ++g.st.stepsRefusedBusy;
    }
    if (!sp.carry) {
        g.pendingStepUs = 0;
    }

    for (uint32_t i = 0; i < g.clockCount; ++i) {
        ClockRef& c = g.clocks[i];
        if (!c.verified) {
            continue;
        }
        if (!g.timescaleValid || plan.value != g.writtenTimescale) {
            c.Store<float>(facts::CLOCK_TIMESCALE_OFF, plan.value);
        }
        if (armUs != 0) {
            // +0x98 first: the beat that consumes the bit reads the step in the same breath, and
            // there is no cost to being right about the order.
            c.Store<uint64_t>(facts::CLOCK_STEPONCE_OFF, armUs);
        }
        uint64_t flags = c.Load<uint64_t>(facts::CLOCK_FLAGS_OFF);
        flags = wantPaused ? (flags | kPauseBit) : (flags & ~kPauseBit);
        flags = armUs != 0 ? (flags | kStepOnceBit) : (flags & ~kStepOnceBit);
        c.Store<uint64_t>(facts::CLOCK_FLAGS_OFF, flags);
    }

    g.writtenTimescale = plan.value;
    g.timescaleValid = true;
    g.writtenPaused = wantPaused;
    g.stepArmedLastFrame = (armUs != 0);
    if (armUs != 0) {
        ++g.st.stepsArmed;
    }

    g.st.ceiling = ceiling;
    g.st.framePeriodKnown = g.period.FramePeriodUs(period);
    g.st.framePeriodUs = period;
    (void)info;
}

// ------------------------------------------------------------------------------------------------
// The transport mirror. The transport is the single source of truth for transport state; the clock
// is its slave. Mirroring means no caller has to remember to tell the clock anything, which is the
// failure mode a "call SetRate when you call Play" contract would have.
// ------------------------------------------------------------------------------------------------

void MirrorTransport() {
    if (!g.mirrorEnabled || g.svc == nullptr || g.svc->transport == nullptr) {
        return;
    }
    IReplayTransport* t = g.svc->transport;
    const TransportState state = t->State();
    // ★ REVERSE IS ENGINE-PAUSED. The engine has no backwards dt: its clock marches forward while
    // the playhead runs backward, so every dt-driven smoothing and error-correction arm integrates
    // AWAY from the pose we just published -- the disc leaving the hand the owner is holding it in.
    // Holding the pause bit makes reverse mean "republish state, do not integrate", which is what
    // the MoveKind::Reverse comment below already specifies for a reverse scrub.
    // ★ AND SO IS A DRAG. A scrub keeps the pre-drag signed rate on purpose, so before EngineHeld
    // learned the state a drag read as "playing": the pause bit was cleared and the move classifier
    // below was skipped, and player bodies -- which only re-latch on a beat -- never moved.
    const bool paused = EngineHeld(state, t->Rate());

    g.durationUs = t->DurationUs();
    const int64_t ph = ClampPlayhead(t->PlayheadUs(), g.durationUs);

    // ★ A commanded change invalidates the window. The samples in the meter are evidence about the
    // OLD rate; blending them into the new one produces a number that is not the rate we are running
    // and not the rate we were. Reset reports UNKNOWN until a fresh sample lands, which is honest,
    // and it is the whole reason Achieved() can return false.
    // ★ THE PRE-ROLL IS THE ONE STATE THE MIRROR DOES NOT IMPOSE. The transport is paused on
    // purpose -- the playhead is pinned at the frame the viewer will see first -- and mirroring
    // that would re-freeze the engine every frame, which is exactly what the pre-roll is for.
    if (!g.preRollActive) {
        if (t->Rate() != g.commandedRate || paused != g.commandedPaused) {
            g.rate.Reset();
        }
        g.commandedRate = t->Rate();
        g.commandedPaused = paused;
    }

    if (!g.preRollActive && g.havePrevPlayhead && paused) {
        uint64_t step = 0;
        const MoveKind mk = ClassifyPlayheadMove(g.prevPlayheadUs, ph, g.maxElapsedUs, step);
        switch (mk) {
            case MoveKind::Reverse:
                // Still not approximated by a forward step of the move's size -- dt is always
                // positive (S7.5.5) -- but it does earn the floor-sized beat StepForMove gives it.
                ++g.st.movesReverse;
                break;
            case MoveKind::Jump:
                ++g.st.movesJump;
                break;
            case MoveKind::ForwardStep:
            case MoveKind::None:
                break;
        }
        const uint64_t beat = StepForMove(mk, step, kFloorUs);
        if (beat != 0) {
            g.pendingStepUs = beat;
        }
    }
    g.prevPlayheadUs = ph;
    g.havePrevPlayhead = true;
    g.playheadUs = ph;
}

// ------------------------------------------------------------------------------------------------

class Clock final : public IReplayClock {
public:
    void SetStepUs(uint64_t stepUs) override {
        // Never zero. B.5 says so and CLOCK.RECIP_DIVIDE_VA says why: the raw delta a step-once beat
        // takes is +0x98 verbatim, and a zero there is the same +inf as a zero timescale.
        if (stepUs == 0) {
            stepUs = kFloorUs;
        }
        if (stepUs > g.maxElapsedUs && g.maxElapsedUs != 0) {
            // Longer than the engine can step: it would be clamped to the maximum and the rest
            // discarded silently, so the truthful thing is to command what will actually happen.
            stepUs = g.maxElapsedUs;
        }
        g.pendingStepUs = stepUs;
    }

    void SetPlayhead(int64_t tUs) override { g.playheadUs = ClampPlayhead(tUs, g.durationUs); }
    int64_t Playhead() const override { return g.playheadUs; }

    void EnterBurst() override {
        if (!g.bracketEnabled || g.watchdog == nullptr) {
            return;
        }
        // The only place the engine's refcount is RAISED. Gating it here is what makes ExitBurst
        // and the leak drain in Tick() provably inert in a live session: both are guarded by
        // g.burstDepth, and nothing else increments it.
        if (!livesafety::Allowed(livesafety::Feature::WatchdogBracket)) {
            return;
        }
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(
            static_cast<uint8_t*>(g.watchdog) + facts::WATCHDOG_REFCOUNT_OFF));
        InterlockedIncrement(&g.burstDepth);
        ++g.st.burstsEntered;
    }

    void PetWatchdog() override {
        if (!g.bracketEnabled || g.watchdog == nullptr || !g.watchdogVerified) {
            return;
        }
        using TickFnT = void(__fastcall*)(void*);
        const void* fn = buildguard::MappedAddress(facts::WATCHDOG_TICK);
        if (fn == nullptr) {
            return;
        }
        // Both mitigations, as S7.4.6 specifies: the refcount bracket AND the stock Tick from inside
        // the loop, so the watchdog stays armed against a genuine hang instead of being disabled.
        reinterpret_cast<TickFnT>(const_cast<void*>(fn))(g.watchdog);
        ++g.st.watchdogPets;
    }

    void ExitBurst() override {
        if (!g.bracketEnabled || g.watchdog == nullptr) {
            return;
        }
        // Never below zero, and never more decrements than increments: the refcount is the engine's,
        // and driving it negative would disable the watchdog for the rest of the process.
        if (InterlockedCompareExchange(&g.burstDepth, 0, 0) <= 0) {
            return;
        }
        InterlockedDecrement(&g.burstDepth);
        InterlockedDecrement(reinterpret_cast<volatile LONG*>(
            static_cast<uint8_t*>(g.watchdog) + facts::WATCHDOG_REFCOUNT_OFF));
    }

    uint64_t LastEngineDeltaUs() const override { return g.lastGoodSimDeltaUs; }

    void SetRate(float rate) override {
        if (rate != g.commandedRate) {
            g.rate.Reset();
        }
        g.commandedRate = rate;
    }
    void SetPaused(bool paused) override {
        if (paused != g.commandedPaused) {
            g.rate.Reset();
        }
        g.commandedPaused = paused;
    }
    void SetDurationUs(uint64_t durationUs) override { g.durationUs = durationUs; }

    void SetPreRoll(bool active, float rate) override {
        if (active) {
            if (!g.preRollActive || rate != g.commandedRate) {
                g.rate.Reset();
            }
            g.preRollActive = true;
            g.commandedRate = rate;
            g.commandedPaused = false;
            return;
        }
        if (!g.preRollActive) return;
        g.preRollActive = false;
        g.rate.Reset();
        // Not restored by hand: MirrorTransport re-reads the transport on the next tick and writes
        // whatever it says, so there is one owner of the commanded pair and no snapshot to go stale.
        // Until that tick the pair still reads the pre-roll's, and Apply has not run in between.
    }

    void Release() override {
        g.preRollActive = false;
        ReleaseHeld();
    }

    ClockStatus Status() const override {
        ClockStatus s = g.st;
        s.refusal = g.refusal;
        s.engaged = g.svc != nullptr && g.svc->transport != nullptr &&
                    g.svc->transport->DurationUs() > 0;
        s.held = g.held;
        s.everHeld = g.everHeld;
        s.released = g.released;
        s.paused = g.writtenPaused;
        s.commandedRate = g.commandedRate;
        s.writtenTimescale = g.writtenTimescale;
        s.cappedByCeiling = g.cappedByCeiling;
        s.rateKnown = g.rate.Achieved(s.achievedRate);
        if (!s.rateKnown) {
            s.achievedRate = 0.0;
        }
        s.framePeriodKnown = g.period.FramePeriodUs(s.framePeriodUs);
        if (!s.framePeriodKnown) {
            s.framePeriodUs = 0.0;
        }
        s.ceiling = s.framePeriodKnown ? Ceiling(g.maxElapsedUs, s.framePeriodUs) : 0.0;
        s.maxElapsedUs = g.maxElapsedUs;
        s.maxElapsedLive = g.maxElapsedLive;
        s.minElapsedUsAtTake = g.minElapsedAtTake;
        s.floorUs = g.floorEnabled ? kFloorUs : 0;
        s.clocksFound = g.clockCount;
        s.clocksWritten = g.held ? DrivenClocks() : 0;
        s.rateSamples = g.rate.Samples();
        s.rateRejectRun = g.rate.RejectRun();
        s.watchdogBound = g.watchdogVerified;
        return s;
    }
};

Clock g_clock;

void BindWatchdog() {
    if (g.watchdog != nullptr || !g.bracketEnabled) {
        return;
    }
    const void* game = Deref(facts::GAME_SINGLETON_VA);
    if (game == nullptr) {
        return;
    }
    const uint8_t* slot = static_cast<const uint8_t*>(game) + facts::WATCHDOG_INSTANCE_OFF;
    if (!Readable(slot, sizeof(void*))) {
        return;
    }
    void* wd = *reinterpret_cast<void* const volatile*>(slot);
    if (wd == nullptr || !Readable(wd, facts::WATCHDOG_STAMP_OFF + sizeof(uint32_t))) {
        return;
    }
    // We CALL WATCHDOG.TICK, so its prologue is guarded before the first call rather than after a
    // crash. The refcount is a plain data write and needs no site check, but the call does.
    g.watchdogVerified = buildguard::VerifySite("WATCHDOG.TICK", facts::WATCHDOG_TICK,
                                                facts::WATCHDOG_TICK_PROLOGUE,
                                                sizeof facts::WATCHDOG_TICK_PROLOGUE);
    g.watchdog = wd;
}

// ★ THE THREE CLOCKS ARE NOT ALL BUILT AT THE SAME MOMENT. CClock's constructor has three call
// sites and CR15Game's is reached from CR15Game::Initialize, so a take that happens early finds the
// global and not the others. Fixing the set at take time would leave part of the engine running at
// wall rate for the whole session -- half the world in slow motion and half not, which reads as a
// physics bug rather than as a clock we never found.
void TopUpClocks() {
    const uint32_t before = g.clockCount;
    if (before >= clock_facts::kInstanceCount) {
        return;
    }
    AddClock("third", Deref(facts::CLOCK_THIRD_PTR));
    const void* game = Deref(facts::GAME_SINGLETON_VA);
    if (game != nullptr && Readable(static_cast<const uint8_t*>(game) + facts::GAME_CLOCK_PTR_OFF,
                                   sizeof(void*))) {
        AddClock("game", *reinterpret_cast<void* const volatile*>(
                             static_cast<const uint8_t*>(game) + facts::GAME_CLOCK_PTR_OFF));
    }
    for (uint32_t i = before; i < g.clockCount; ++i) {
        ClockRef& c = g.clocks[i];
        c.snapTimescaleBits = c.Load<uint32_t>(facts::CLOCK_TIMESCALE_OFF);
        c.snapMinElapsed = c.Load<uint64_t>(facts::CLOCK_MINELAPSED_OFF);
        c.snapped = true;
        // Same read-back as Take(): this clock is about to receive the pause bit from Apply(), and
        // the +inf hazard does not care that it arrived late.
        uint64_t back = 0;
        if (g.floorEnabled && !InstallFloor(c, back)) {
            DropUndrivable(c, back);
        }
    }
    if (g.clockCount != before) {
        // The next Apply must write the timescale even if the planned value has not changed: a
        // clock that has just appeared is still carrying the constructor's 1.0f.
        g.timescaleValid = false;
        if (g.log != nullptr) {
            g.log->Write(LogLevel::Info, kReplayModuleName, "picked up %u further CClock(s); now driving %u",
                         g.clockCount - before, DrivenClocks());
        }
    }
}

// ★ PERF-3'S INSTRUMENT. TICK.HEARTBEAT_BEFORE limits the frame itself when +0x50 carries 0x10: it
// compares the elapsed microseconds against +0x90 and, when the frame came in early, spins
// (sub_1401CE0B0), yields (bit 0x20) or Sleeps (bit 0x40) for the difference. A capped frame rate
// and a saturated main thread look identical from outside the process, so the only honest way to
// say whether the engine is limiting this launch is to read the bits it decides on. Read-only, once
// a second, at the cost of four loads.
void ReportEngineClockState() {
    if (g.log == nullptr || g.clockCount == 0) {
        return;
    }
    const uint64_t now = Qpc();
    if (g.lastStateReportQpc != 0 && now - g.lastStateReportQpc < uint64_t(g.qpcFreq)) {
        return;
    }
    g.lastStateReportQpc = now;
    for (uint32_t i = 0; i < g.clockCount; ++i) {
        const ClockRef& c = g.clocks[i];
        if (!c.verified) {
            continue;
        }
        const uint64_t flags = c.Load<uint64_t>(facts::CLOCK_FLAGS_OFF);
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "engine clock [%s]: flags=0x%llX (limiter %s, fixedstep %s, pause %s) "
                     "step=%llu us floor=%llu us ceiling=%llu us",
                     c.name, (unsigned long long)flags,
                     (flags & 0x10ull) != 0 ? "ON" : "off",
                     (flags & 0x08ull) != 0 ? "ON" : "off",
                     (flags & kPauseBit) != 0 ? "ON" : "off",
                     (unsigned long long)c.Load<uint64_t>(facts::CLOCK_EXTSTEP_OFF),
                     (unsigned long long)c.Load<uint64_t>(facts::CLOCK_MINELAPSED_OFF),
                     (unsigned long long)c.Load<uint64_t>(facts::CLOCK_MAXELAPSED_OFF));
    }
}

// The read PawnProvision::SessionBlocker makes: the state word is the CR15NetGame's first dword,
// and `netGame` is the pointer this tick was handed, null outside a session.
bool InMatch(const void* netGame) {
    if (netGame == nullptr) {
        return false;
    }
    uint32_t state = 0;
    memcpy(&state, netGame, sizeof state);
    return state == facts::NETGAME_STATE_INMATCH;
}

void Tick(const TickInfo& info) {
    if (!g.enabled) {
        return;
    }
    BindWatchdog();

    // ★ A burst still open when our tick runs is a leak -- a crash or an early return inside the
    // caller's loop -- and an un-decremented refcount disables the deadlock monitor for the rest of
    // the process. A burst is a tight loop inside one function: it is opened and closed by the same
    // code, within one frame, so one that is still open here has outlived its frame. Closing it is
    // what bounds the exposure to a single frame, which is what S7.4.6 promises the bracket costs.
    //
    // Keyed on our own tick sequence rather than on the engine frame index, because a module that
    // ticks BEFORE us would record the previous frame's index and be reported as a leak on the very
    // frame it opened in.
    //
    // Above the EngineClock gate on purpose: a leaked bracket must be closed even on the tick that
    // decides to stop driving. It is still inert in a live session, because the depth it drains is
    // only ever raised by EnterBurst, which asks livesafety::Feature::WatchdogBracket.
    if (InterlockedCompareExchange(&g.burstDepth, 0, 0) > 0) {
        while (InterlockedCompareExchange(&g.burstDepth, 0, 0) > 0) {
            InterlockedDecrement(&g.burstDepth);
            if (g.watchdog != nullptr) {
                InterlockedDecrement(reinterpret_cast<volatile LONG*>(
                    static_cast<uint8_t*>(g.watchdog) + facts::WATCHDOG_REFCOUNT_OFF));
            }
        }
        ++g.st.burstsLeaked;
    }

    // ★ NOTHING BELOW THIS LINE MAY RUN AFTER Release(). Take() latches too, and on its own would
    // be enough today because a failed take returns from here -- but TopUpClocks() writes the floor
    // into a clock that appears late, so the drive section is stopped as a section rather than
    // through one function's return value.
    // ★ RECORDER-2 part A / L2. The engine's world clock is the single most destructive thing this
    // plugin can write, and until now the only term protecting a live match was "no file is
    // loaded". The verdict is fixed for the life of the process, so an early return here cannot
    // strand a clock we took: an uncontained process never took one.
    if (!livesafety::Allowed(livesafety::Feature::EngineClock)) {
        ++g.st.idleTicks;
        if (!g.idleLogged && g.log != nullptr) {
            g.idleLogged = true;
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "clock inert: %s -- no take, no timescale, no pause bit",
                         livesafety::VerdictReason(livesafety::Current()));
        }
        return;
    }

    IReplayTransport* const t = (g.svc != nullptr) ? g.svc->transport : nullptr;
    const uint64_t loadedUs = (t != nullptr) ? t->DurationUs() : 0;
    const uint32_t loadSerial = (t != nullptr) ? t->LoadSerial() : 0;
    const bool sessionEnded = g.session.Ended(loadSerial, InMatch(info.netGame));
    switch (GateForSession(t != nullptr, loadedUs, g.held, g.released, sessionEnded)) {
        case ClockGate::Stopped:
            return;
        case ClockGate::Disengage:
            RestoreHeld(false);
            return;
        case ClockGate::Idle:
            ++g.st.idleTicks;
            // ★ A module that never took the clock keeps the reason it never did: SiteMismatch and
            // NoGlobalClock outlive an unload, and stamping over them would erase the only diagnosis
            // the NOT-RUN population carries.
            if (g.refusal == ClockRefusal::None) {
                g.refusal = ClockRefusal::NotEngaged;
            }
            if (!g.idleLogged && g.log != nullptr) {
                g.idleLogged = true;
                g.log->Write(LogLevel::Info, kReplayModuleName,
                             "not engaged -- clocks untouched; no replay is loaded in a running "
                             "match, so the engine's clock runs at the engine's own rate");
            }
            return;
        case ClockGate::Drive:
            break;
    }
    g.idleLogged = false;
    if (!g.held && !Take()) {
        return;
    }
    TopUpClocks();
    ReadMaxElapsed();
    Measure(info);
    MirrorTransport();
    Apply(info);
    ReportEngineClockState();
}

void ModuleTick(const TickInfo& info) { Tick(info); }

bool Init(ReplayServices* services) {
    REPLAY_REQUIRE(services, log);
    REPLAY_REQUIRE(services, config);
    g.svc = services;
    g.log = services->log;

    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g.qpcFreq = static_cast<uint64_t>(f.QuadPart);

    g.rate.Reset();
    g.period.Reset();
    // `g` outlives Shutdown(). A second Init() in one process would otherwise start with the
    // release latch set and a refusal reading "released" for a clock this run never touched.
    g.held = false;
    g.everHeld = false;
    g.released = false;
    g.idleLogged = false;
    g.preRollActive = false;
    g.session = SessionWatch{};
    g.clockCount = 0;
    g.measured = nullptr;

    const IConfig* cfg = services->config;
    g.enabled = cfg->GetBool("transport.clock_writes", true) &&
                !cfg->GetBool("bootstrap.minimal", false);
    g.bracketEnabled = cfg->GetBool("transport.watchdog_bracket", true);
    g.floorEnabled = g.enabled && cfg->GetBool("clock.write_floor", true);
    g.pauseEnabled = g.enabled && g.floorEnabled && cfg->GetBool("clock.pause_bit", true);
    g.stepEnabled = g.pauseEnabled && cfg->GetBool("clock.frame_step", true);
    g.mirrorEnabled = g.enabled && cfg->GetBool("clock.mirror_transport", true);
    g.refusal = g.enabled ? ClockRefusal::None : ClockRefusal::Disabled;

    // ★ PAUSE IS GATED ON THE FLOOR, IN THE CODE AND NOT IN A COMMENT. CLOCK.MINELAPSED_OFF is 0 on
    // a stock clock, and with a zero minimum the engine's own pause bit drives the scaled delta to 0
    // and CLOCK.RECIP_OFF to +inf. Switching the floor off therefore switches pause off with it,
    // rather than leaving a switch whose off position crashes the game.
    if (g.enabled && !g.floorEnabled) {
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "clock.write_floor is off, so pause is off too: without the 1 us floor the "
                     "pause bit yields a zero scaled delta and 1.0f/0.0f = +inf in CLOCK.RECIP_OFF");
    }

    // ★★ A CLOCK THAT IS SWITCHED OFF DOES NOT REGISTER, and this is not tidiness. W2-F's panel
    // draws "no replay clock has registered, so the engine's own clock still runs at real time"
    // exactly when ReplayServices::clock is null. Registering an inert clock would delete that line
    // and put nothing in its place -- the rate would appear to move the world and would not, which
    // is the silent-inertness shape this project keeps paying for. Null is the honest answer here.
    if (!g.enabled) {
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "transport.clock_writes is off; NOT registering ReplayServices::clock, so the "
                     "panel keeps saying the rate applies to the playhead only -- which is true");
        return true;
    }

    services->clock = &g_clock;
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "registered; timescale%s%s%s, ledger 0.4 and 0.13",
                 g.pauseEnabled ? " + pause" : "", g.stepEnabled ? " + frame-step" : "",
                 g.bracketEnabled ? " + watchdog bracket" : "");
    return true;
}

void Shutdown() {
    ReleaseHeld();
    g.svc = nullptr;
    g.log = nullptr;
}

// After playback (300), so ReplayServices::transport is already published when the first mirror
// runs. The mirror re-reads the pointer every frame, so this is tidiness rather than a dependency.
REPLAY_MODULE("clock", 310, Init, ModuleTick, Shutdown);

}  // namespace

IReplayClock* Get() { return &g_clock; }

}  // namespace clock

const char* ClockRefusalName(ClockRefusal r) {
    switch (r) {
        case ClockRefusal::None: return "none";
        case ClockRefusal::Disabled: return "disabled";
        case ClockRefusal::NoModule: return "no-module";
        case ClockRefusal::BuildGuard: return "build-guard";
        case ClockRefusal::SiteMismatch: return "site-mismatch";
        case ClockRefusal::NoGlobalClock: return "no-global-clock";
        case ClockRefusal::NotACClock: return "not-a-cclock";
        case ClockRefusal::NoFloor: return "no-floor";
        case ClockRefusal::Released: return "released";
        case ClockRefusal::NotEngaged: return "not-engaged";
        default: return "?";
    }
}

}  // namespace replay
