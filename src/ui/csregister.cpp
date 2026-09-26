#include "ui/csregister.h"

#include <windows.h>

#include <atomic>
#include <stdarg.h>
#include <stdio.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/livesafety.h"
#include "bootstrap/log.h"
#include "bootstrap/tableguard.h"
#include "generated/facts_generated.h"

namespace replay {
namespace ui {
namespace csregister {
namespace {

using tableguard::TableCheck;
using tableguard::TableRaw;
using tableguard::TableVerdict;

const char* const kReplayModuleName = "ui";

// FnRegisterUpdate (csregister.h) is CncaGameSpace::RegisterUpdate, no-dependency-expansion form
// (0x140506810). Arguments 4 and 5 are debug labels that no consumer reads -- proved twice, by the
// 43-instruction PC body never referencing them and by the ARM build's mangled name taking the
// same tail and reading neither. We pass them anyway, because they cost nothing and a dump of the
// record table is easier to read with them there.

// The 24-byte member-function-pointer record the registrar copies. MSVC encodes a non-virtual,
// single-inheritance member pointer as the bare code address with a zero this-adjustor, and the
// runner reads exactly two of these fields: the code address at record+0x10 and a SIGNED int32
// adjustor at record+0x18. There is no virtual-PMF branch on PC -- the call is unconditional -- so
// this flat form is precisely what the runner wants.
struct Pmf24 {
    void* fn;
    uint64_t word1;
    uint64_t word2;
};
static_assert(sizeof(Pmf24) == 24, "the registrar copies 24 bytes");

// The MAILBOX. Everything the band-3 callback writes lives here, and it is the only state the
// callback touches. No lock, no allocation, no engine call, no ImGui: the callback runs on a
// CTaskKernel worker (L5-RUN-3 F6 measured 4681 of 4681 entries off-thread) where every one of
// those is either unsafe or forbidden.
//
// The four payload fields are one record and must be read as one, so they are published under a
// seqlock: even means stable, odd means a publish is in flight. They are atomics rather than plain
// members because a plain-member seqlock is a data race by the letter of the standard even where
// it works; relaxed loads compile to the same instructions, and the reader still needs the
// sequence check, so nothing about the torn-read control is weakened by making them atomic.
struct Mailbox {
    std::atomic<uint64_t> seq{0};

    std::atomic<uint64_t> sequence{0};
    std::atomic<uint64_t> monotonicNs{0};
    std::atomic<uint32_t> threadId{0};
    std::atomic<uint32_t> orderKey{0};

    std::atomic<uint64_t> invocations{0};
    std::atomic<uint64_t> offThread{0};
    std::atomic<uint64_t> registrationInvocations{0};
    std::atomic<uint64_t> staleInvocations{0};
    std::atomic<void*> lastStaleDispatch{nullptr};
    std::atomic<uint64_t> pending{0};
    std::atomic<uint64_t> contention{0};
    std::atomic<uint64_t> tornReads{0};

    // Read by the callback, written by the owner thread. Atomic for the same reason as the payload.
    std::atomic<void*> registeredCs{nullptr};
};

// ** THE PUBLISHED LINE'S BUDGET, ONE DECLARED BOUND PER PART (L5-RUN-5 G1).
// kSuffixMax is checked against kSuffixFormat at its widest by the tests/ui case
// `the published suffix fits its declared budget with every field at its widest`, which
// formats THIS constant -- not a copy of it -- with UINT64_MAX/ULONG_MAX everywhere.
constexpr size_t kDiagnosticMax = 512;   // the status prose
constexpr size_t kPremiseMax = 256;      // the ARCH-15 S7 premise sentence, when it is present
// The five fixed groups W2-R parses, sized from what they can RENDER rather than from a line
// somebody once measured: a bound that fits this launch's numbers is the same defect one order of
// magnitude away. The test below formats kSuffixFormat with every field at its maximum and reds if
// this sum is short.
constexpr size_t kSuffixConversions = 25 * 20   // twenty-five %llu at UINT64_MAX
                                    + 4 * 10    // two %u and two %lu at their maximum
                                    + 16        // the longest reason= word
                                    + 16        // the longest hoversource= word
                                    + 2         // installed=, as -1 if it is ever signed
                                    + 2;        // scope=, likewise
constexpr size_t kSuffixText = 452;             // the literal text between the conversions
constexpr size_t kSuffixMax = kSuffixConversions + kSuffixText;

// The fixed suffix W2-R reads, in ONE place, so that the test which sizes the buffer and the code
// which fills it cannot hold two different formats (BRIEF_COMMON S9: a checker with its own copy
// of a format certifies broken files).
const char* const kSuffixFormat =
    " [ticks owner=%llu draws=%llu] [inframe entries=%llu draws=%llu refused=%llu "
    "nobyte=%llu nolatch=%llu noctx=%llu facade=%llu closed=%llu fallback=%llu "
    "installed=%d] [stage0 frameopen samples=%llu nonzero=%llu lasttick=%llu] "
    "[facade nodraw=%llu beginfalse=%llu reason=%s bound=%u/%u] "
    "[gate scope=%d unresolved=%llu alreadyopen=%llu scopeentries=%llu scoperestores=%llu "
    "changed=%llu unmapped=%llu hovered=%llu hoversource=%s drawnanostotal=%llu "
    "drawnanosmax=%llu] "
    "[threads owner=%lu callback=%lu]";

struct State {
    DrawFn draw = nullptr;
    bool enabled = false;
    bool pinned = false;
    Report report;
    Mailbox mb;
    // The thread our stage-0 tick actually ran on. MEASURED at OnTick from the caller's own
    // GetCurrentThreadId (ui_module.cpp), never assumed: src/engine/tick.cpp dispatches on "the
    // game's main thread, inside the frame" and OnTick is called from that dispatch, so the tick's
    // own thread is what "owner" means here and nothing else needs to know its name.
    std::atomic<uint32_t> ownerThreadId{0};
    void* registeredSpace = nullptr;
    // The component system the CURRENT registration was made with. Written only in lockstep with
    // registeredSpace, because the attribution target belongs to a registration and not to a space.
    void* registeredCs = nullptr;
    void* lastSpace = nullptr;           // the last non-null space OnTick saw, registered or not
    TableRaw csSnapshot;                 // the component-system header at registration
    uint64_t ticksSinceRegistration = 0;
    uint64_t ticksSinceInvocation = 0;      // owner ticks with no new callback entry
    uint64_t ticksSinceMarshalledDraw = 0;  // owner ticks on which the panel was not drawn
    uint64_t invocationsAtLastTick = 0;
    // Cleared at the top of every OnTick and set by the draw: the frame gets exactly one panel.
    bool drewThisTick = false;
    // THE INLINE PREFERENCE (AUDIT_F3I F3I-5). An inline band-3 dispatch is the slot the engine
    // opened its GUI frame for, so when the engine is dispatching us inline the owner tick stands
    // aside and lets the dispatch draw. It cannot be known DURING the tick -- the dispatch comes
    // later in the same frame -- so the previous tick answers for this one: one tick of history,
    // which is exactly the one tick of slack ARCH-13 s2.3's coverage rule allows.
    bool inlineDispatchThisTick = false;
    bool inlineDispatchLastTick = false;
    bool deferredThisTick = false;
    // Leg 3 is credited at most once per tick, because it is a fact about the FRAME: the frame
    // this tick drew carried band-3 entries. Five entries folded into one frame are one frame.
    bool marshalledCreditedThisTick = false;
    FnLookupBySymbol testLookup = nullptr;
    FnRegisterUpdate testRegister = nullptr;
    void (*publishHook)() = nullptr;
    void (*readHook)() = nullptr;
    // The live refusal is retried every tick by design (the verdict can change mid-session), so the
    // line it prints is latched to one per process rather than one per tick.
    bool liveRegisterRefused = false;
    char diagnostic[kDiagnosticMax] = {};
    // The diagnostic plus the fixed suffix. Separate from `diagnostic` so that reading the line
    // twice cannot append the suffix twice, and so that the branches below stay unaware of it.
    //
    // ** SIZED FROM ITS INPUTS, NOT GUESSED (L5-RUN-5 G1). At 640 bytes this buffer silently cut
    // the published line at byte 639 in exactly the branches that report trouble: the
    // `INVOKED, NEVER DRAWN` / `MARSHAL STALE` prose alone needs 540 and the suffix a further 209,
    // so the `[inframe ...]`, `[stage0 ...]` and `[threads ...]` groups -- the machine-read
    // contract -- were deleted from the one launch they were built for, and `_TRUNCATE` said
    // nothing about it. The bound is now the sum of the three parts that go into the line, each of
    // which is itself a declared bound, and a line that still would not fit REFUSES BY NAME below
    // rather than losing its tail.
    char published[kDiagnosticMax + kPremiseMax + kSuffixMax] = {};
};

// ★ Static storage duration, never freed, and that is load-bearing rather than convenient.
// Registration is one-way: the engine calls ThunkDraw for the life of the game space, including
// after Disable(). If this lived anywhere with a shorter life the callback would read freed memory
// on the first frame after shutdown.
State g;

// The attribution target is bound to a REGISTRATION. Clearing registeredSpace without clearing this
// would leave the space we have left as the CURRENT target: its dispatches would be counted as the
// new registration's and would raise Running while we are registered nowhere (AUDIT_W2-F_R3 f1).
void SetRegistration(void* space, void* cs) {
    g.registeredSpace = space;
    g.registeredCs = cs;
    // Published before the counter is cleared: the callback reads this to decide which counter to
    // bump, and clearing first would let a dispatch arriving in between be attributed to the
    // registration we are abandoning.
    g.mb.registeredCs.store(cs, std::memory_order_relaxed);
    g.mb.registrationInvocations.store(0, std::memory_order_relaxed);
}

uint64_t MonotonicNs() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        ::QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    ::QueryPerformanceCounter(&c);
    // Split so the multiply cannot overflow: QPC counts since boot, and c * 1e9 would not fit.
    return static_cast<uint64_t>(c.QuadPart / freq) * 1000000000ull +
           static_cast<uint64_t>((c.QuadPart % freq) * 1000000000ll / freq);
}

// Publishes one invocation record under the seqlock. A publish that loses the even->odd claim to a
// concurrent dispatch gives up its DETAIL rather than interleaving with the other's: the entry is
// already counted, and a torn record is worse than a missing one.
void Publish(uint64_t sequence, uint32_t tid) {
    uint64_t s = g.mb.seq.load(std::memory_order_relaxed);
    if ((s & 1u) != 0 || !g.mb.seq.compare_exchange_strong(s, s + 1, std::memory_order_acquire,
                                                           std::memory_order_relaxed)) {
        g.mb.contention.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g.mb.sequence.store(sequence, std::memory_order_relaxed);
    g.mb.monotonicNs.store(MonotonicNs(), std::memory_order_relaxed);
    g.mb.threadId.store(tid, std::memory_order_relaxed);
    g.mb.orderKey.store(kOrderKey, std::memory_order_relaxed);
    if (g.publishHook != nullptr) g.publishHook();
    g.mb.seq.store(s + 2, std::memory_order_release);
}

// Bounded, because this runs on the game thread: a reader that spins until the writer is done is a
// reader that can hang a frame. Sixty-four attempts, and then it says it could not.
bool ReadMailbox(Invocation* out) {
    for (int attempt = 0; attempt < 64; ++attempt) {
        const uint64_t a = g.mb.seq.load(std::memory_order_acquire);
        if ((a & 1u) != 0) continue;
        out->sequence = g.mb.sequence.load(std::memory_order_relaxed);
        out->monotonicNs = g.mb.monotonicNs.load(std::memory_order_relaxed);
        out->threadId = g.mb.threadId.load(std::memory_order_relaxed);
        out->orderKey = static_cast<uint16_t>(g.mb.orderKey.load(std::memory_order_relaxed));
        // The reader's own window: a writer that starts AND finishes here leaves the sequence even
        // again, so the entry check above cannot see it and only the re-check below can.
        if (g.readHook != nullptr) g.readHook();
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g.mb.seq.load(std::memory_order_relaxed) == a) return true;
    }
    g.mb.tornReads.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// The ONLY place g.draw is ever called, and it refuses to run anywhere but the owner thread.
//
// ★★ THE PANEL IS DRAWN ON EVERY OWNER TICK WHILE ENABLED, WHETHER OR NOT A BAND-3 ENTRY ARRIVED
// (AUDIT_F3E F3E-4). The old `if (pending == 0) return;` made the panel's cadence the band-3
// dispatcher's -- an asynchronous CTaskKernel task measured at ~26.8/s on L5 re-run 3 -- rather
// than the frame's, and ImGui is immediate mode: a frame with no panel::Draw has NO panel in it.
// So the mailbox is drained for ordering and telemetry, and then the draw runs regardless.
//
// ★ EXACTLY ONCE PER OWNER TICK, AND THE INLINE DISPATCH IS PREFERRED (AUDIT_F3I F3I-5). Two sites
// can reach the draw in one frame: our stage-0 tick, and a band-3 dispatch the engine ran INLINE on
// the owner thread. The inline one is inside the GUI frame the band-3 slot was registered for, so
// it is the one that must land -- but it arrives AFTER the tick, so the tick cannot see it coming
// and can only stand aside on the previous tick's evidence. Hence: a tick that saw an inline
// dispatch last tick defers; the inline dispatch this tick draws. A dispatcher that switches from
// inline to asynchronous costs exactly one frame (counted in inlineDeferralsUnfilled) before the
// tick takes the draw back.
enum class DrawSite { OwnerTick, InlineDispatch };

// The band-3-consumed subset of the draws (ARCH-14 3c4e669, DESIGN S8.5.4a F-B). ownerDraws is
// every draw and is S8.5.4a's leg 3; marshalledDraws counts only the frames that carried at
// least one band-3 entry, and it is a DIAGNOSTIC THAT FEEDS NO GATE -- the number that says
// whether the band-3 path ever reached a frame, on a module that since F3I-4 draws whether or
// not anything was ever registered. At most once per tick: it is a property of the frame, not
// of the number of entries folded into it.
void CreditMarshalledDraw(uint64_t pending) {
    if (pending == 0 || g.marshalledCreditedThisTick) return;
    g.marshalledCreditedThisTick = true;
    ++g.report.marshalledDraws;
}

void DrainMarshal(DrawSite site) {
    const uint32_t owner = g.ownerThreadId.load(std::memory_order_relaxed);
    if (owner == 0 || ::GetCurrentThreadId() != owner) {
        ++g.report.wrongThreadDrains;
        g.report.status = Status::WrongThread;
        return;
    }
    // Recorded before the enable test: whether the engine dispatches us inline is a fact about the
    // engine, and it must not become unknowable because the panel happens to be closed.
    if (site == DrawSite::InlineDispatch) g.inlineDispatchThisTick = true;
    if (!g.enabled || g.draw == nullptr) return;
    const uint64_t pending = g.mb.pending.exchange(0, std::memory_order_acquire);
    if (pending != 0) {
        Invocation last;
        if (ReadMailbox(&last)) g.report.lastInvocation = last;
        g.report.drainedInvocations += pending;
        // A drain that found work has just seen an entry, so the silence counter restarts here.
        // Done on this side rather than in the callback: both are owner-thread-only, and a worker
        // writing these would be the one race the mailbox exists to prevent.
        g.ticksSinceInvocation = 0;
        g.invocationsAtLastTick = g.mb.invocations.load(std::memory_order_relaxed);
    }
    if (g.drewThisTick) {
        // This frame's panel is already emitted; none of these entries gets one of its own --
        // but the frame that drew is the frame that CARRIES them, so leg 3 is credited here
        // too (AUD-ARCH F-B). An inline dispatch suppressed by the tick's own draw still
        // reached the screen.
        g.report.coalescedInvocations += pending;
        CreditMarshalledDraw(pending);
        return;
    }
    if (site == DrawSite::OwnerTick && g.inlineDispatchLastTick) {
        // Standing aside for the inline dispatch this frame is expected to bring. The entries are
        // drained either way -- they are counted here and the dispatch's own draw carries them.
        g.report.coalescedInvocations += pending;
        g.deferredThisTick = true;
        ++g.report.inlineDeferrals;
        return;
    }
    if (pending > 1) g.report.coalescedInvocations += pending - 1;
    CreditMarshalledDraw(pending);
    if (site == DrawSite::InlineDispatch) ++g.report.inlineDraws;
    g.ticksSinceMarshalledDraw = 0;
    // Set before the attempt, not after: it means "this tick has had its one shot", and a refused
    // attempt has still had it. Retrying inside the same tick would draw twice on the frame where
    // the byte happened to flip.
    g.drewThisTick = true;

    // ★ ownerDraws IS CREDITED ONLY WHEN THE PANEL ACTUALLY REACHED A FRAME (ARCH-15 S4.1,
    // AUDIT_F3J F3J-4). It used to be incremented here unconditionally, and since every draw at
    // this site is refused with the GUI frame shut, it counted refusals -- and leg 3 read 1:1 with
    // ownerTicks on a build that drew nothing. AttemptFallbackDraw does the three-load precondition
    // and honours Begin's own answer; a refusal lands in inframe::refusedDraws with its reason
    // named, and does NOT land here. It also stands down entirely while the row-2.5 detour is
    // armed, which is the normal case: then this counter is 0 and inFrameDraws carries the panel.
    if (inframe::AttemptFallbackDraw()) ++g.report.ownerDraws;
}

// The callback the engine's update DAG invokes. `opaqueThis` is rec.cs + the record's signed
// adjustor -- the engine's own CComponentSystem, not ours. It is never dereferenced.
//
// IT DRAWS NOTHING. L5-RUN-3 F6 measured 4681 of 4681 entries on a CTaskKernel worker thread: the
// runner dispatches the node as a task instead of calling it inline when a per-thread flag is set,
// and ImGui is not thread-safe. So this records the entry and returns, and the owner thread runs
// the draw the band-3 slot was registered for. The one case where the runner does call us inline
// is handled at the bottom: that IS the owner thread, inside the open GUI frame, which is the whole
// reason this packet registers at band 3, and draining there does not give it up.
void ThunkDraw(void* opaqueThis) {
    const uint64_t sequence = g.mb.invocations.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint32_t tid = ::GetCurrentThreadId();
    const uint32_t owner = g.ownerThreadId.load(std::memory_order_relaxed);
    if (tid != owner) g.mb.offThread.fetch_add(1, std::memory_order_relaxed);

    // WHICH registration dispatched us. Registration is one-way (see the header), so a space we
    // have left goes on calling this for the life of that space -- and the lifetime invocation
    // total therefore cannot say whether the CURRENT registration has ever run. `opaqueThis` is
    // rec.cs plus the record's signed adjustor, and the adjustor is ours: Pmf24 is zero-initialised
    // and only .fn is set, so word1 is 0 and the engine hands back exactly the CComponentSystem we
    // passed the registrar. A pointer compare, never a dereference.
    void* const cs = g.mb.registeredCs.load(std::memory_order_relaxed);
    if (cs != nullptr && opaqueThis == cs) {
        g.mb.registrationInvocations.fetch_add(1, std::memory_order_relaxed);
    } else {
        // Not a fault on its own -- it is the old space still running -- but it must never confirm
        // the new one, and if it is the ONLY thing arriving then either a second space is
        // dispatching our record or the adjustor assumption above is wrong. Both are worth a
        // number rather than a silent Running.
        g.mb.staleInvocations.fetch_add(1, std::memory_order_relaxed);
        g.mb.lastStaleDispatch.store(opaqueThis, std::memory_order_relaxed);
    }

    Publish(sequence, tid);
    // Released last, so an owner thread that sees the backlog also sees the record behind it.
    g.mb.pending.fetch_add(1, std::memory_order_release);

    if (tid == owner) DrainMarshal(DrawSite::InlineDispatch);
}

void PinSelf() {
    if (g.pinned) return;
    HMODULE self = nullptr;
    // Address-of a function in our own image, so this finds us wherever we were loaded from.
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                             reinterpret_cast<LPCWSTR>(&ThunkDraw), &self) != 0) {
        g.pinned = true;
        return;
    }
    // Not fatal, but it must not pass unremarked: without the pin, a FreeLibrary on this DLL leaves
    // the engine calling ThunkDraw at an unmapped address on its very next frame.
    log::Direct(LogLevel::Warn, kReplayModuleName,
                "could not pin the module (GetModuleHandleExW %lu); registration is still one-way, "
                "so an unload while a game space lives would call into freed code",
                ::GetLastError());
}

void SetDiagnostic(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g.diagnostic, sizeof g.diagnostic, fmt, ap);
    va_end(ap);
}

// The component-system table's offset on the game space. The header layout inside it is
// tableguard's.
const uint32_t kCsTableOff = facts::CS_TABLE_OFF;
const uint64_t kCsEntryStride = 16u;

// Where the registrar puts the PMF inside a CS.UPDATE_RECORD_SIZE-byte record, from its own
// store: movups [rax+rcx*8-38h], xmm0 (0F 11 44 C8 C8) at 0x140506882 with rax = table base and
// rcx = 9 * count AFTER the append, so the record starts 0x48 below and the PMF's first 16 bytes
// land at record + 0x10; the component system goes to record + 0 (mov [rax+rcx*8-48h], rdi at
// 0x140506887). The runner reads the code address back from the same +0x10. Submitted as
// CS.UPDATE_RECORD_PMF_OFF in the W2-F fact drop.
const uint32_t kRecordPmfOff = 0x10u;

// One refusal line per space, then one every kLogEveryDeferrals, so a three-second load window
// leaves evidence without burying the log in it.
const uint64_t kLogEveryDeferrals = 600u;

uint32_t SchemeTableOff() {
    return static_cast<uint32_t>(facts::CS_SCHEME_STRIDE) * kSchemeIndex +
           facts::CS_SCHEME_TABLE_OFF;
}

TableCheck CheckComponentSystemTable(const void* gameSpace) {
    return tableguard::CheckSpaceTable(gameSpace, kCsTableOff, kCsEntryStride, false);
}

TableCheck CheckUpdateRecordTable(const void* gameSpace, bool emptyIsReady) {
    return tableguard::CheckSpaceTable(gameSpace, SchemeTableOff(), facts::CS_UPDATE_RECORD_SIZE,
                                       emptyIsReady);
}

void NoteRefusal(Refusal what, const TableCheck& check) {
    ++g.report.deferrals;
    if (g.report.registrations == 0) ++g.report.deferralsBeforeFirst;
    g.report.lastRefusal = what;
    g.report.lastRefused = check;
    g.report.status = Status::SpaceNotReady;
    char table[256];
    tableguard::FormatTable(check, table, sizeof table);
    SetDiagnostic(
        "deferred: the game space's %s (%s). The engine builds this table in place and has no "
        "built-flag to ask, so we retry on the next tick rather than call into it. Refused %llu "
        "tick(s) so far.",
        RefusalName(what), table, static_cast<unsigned long long>(g.report.deferrals));
    if (g.report.deferrals == 1 || g.report.deferrals % kLogEveryDeferrals == 0) {
        log::Direct(LogLevel::Warn, kReplayModuleName, "%s", g.diagnostic);
    }
}

// (a) of the epoch: the index of our record in the space's update-record table, or -1. A table
// that is not Ready to search answers -1 as well, and that is the point rather than a shortcut:
// the engine's constructor zeroes the count, so "nothing to search" and "our record is gone" are
// the same observation. The check has probed capacity * stride bytes from base, so the scan below
// reads memory the memory manager has already vouched for.
int64_t OurRecordIndex(const void* gameSpace, TableCheck* rec) {
    *rec = CheckUpdateRecordTable(gameSpace, false);
    if (rec->verdict != TableVerdict::Ready) return -1;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(rec->raw.base));
    const void* ours = reinterpret_cast<const void*>(&ThunkDraw);
    for (uint64_t i = 0; i < rec->raw.count; ++i) {
        const void* fn = *reinterpret_cast<void* const volatile*>(
            base + i * facts::CS_UPDATE_RECORD_SIZE + kRecordPmfOff);
        if (fn == ours) return static_cast<int64_t>(i);
    }
    return -1;
}

// The five-field line, once per pointer change. `ours >= 0` on a pointer we have never registered
// into is F3 seen directly: the block's previous life is still in it.
void NoteSpaceChange(const void* from, void* to) {
    TableCheck rec;
    const int64_t ours = OurRecordIndex(to, &rec);
    Report::SpaceChange& c = g.report.lastSpaceChange;
    c.gameSpace = to;
    c.cs = CheckComponentSystemTable(to);
    c.rec = rec;
    c.ourIndex = ours;
    c.invocationsAtChange = g.report.invocations;
    ++g.report.spacesSeen;
    if (from != nullptr) ++g.report.spaceChanges;
    // FormatTable rather than the raw triple: it carries the verdict and the reason, so a header we
    // could not read is not printed as a table full of zeroes (AUDIT_W2-F_R2 R2-3).
    char csTable[256];
    char recTable[256];
    tableguard::FormatTable(c.cs, csTable, sizeof csTable);
    tableguard::FormatTable(rec, recTable, sizeof recTable);
    log::Direct(LogLevel::Info, kReplayModuleName,
                "csregister: space %p -> %p  cs(%s)  rec(%s)  ours=%lld  inv=%llu", from, to,
                csTable, recTable, static_cast<long long>(ours),
                static_cast<unsigned long long>(g.report.invocations));
}

// Every tick on the registered space. The pointer compare that brought us here is the fast path
// only; whether the registration is live is read off the registration itself.
void ValidateRegistration(void* gameSpace) {
    TableCheck rec;
    const int64_t ours = OurRecordIndex(gameSpace, &rec);
    if (ours < 0) {
        ++g.report.staleDetected;
        g.report.status = Status::Stale;
        SetRegistration(nullptr, nullptr);
        char table[256];
        tableguard::FormatTable(rec, table, sizeof table);
        SetDiagnostic(
            "STALE: our record is no longer in game space %p's update-record table (%s) -- the "
            "engine rebuilt the space under the same pointer after we registered into its "
            "previous occupant. Detected %llu time(s); re-registering on the next tick.",
            gameSpace, table, static_cast<unsigned long long>(g.report.staleDetected));
        log::Direct(LogLevel::Warn, kReplayModuleName, "%s", g.diagnostic);
        return;
    }

    // (b): reported, never acted on alone. A live space can grow its table, and a re-append into a
    // space that still holds our record would put a second record beside the first. Counted once
    // per move, not once per tick, so the number is the number of moves.
    const TableRaw cs = CheckComponentSystemTable(gameSpace).raw;
    if (cs.base != g.csSnapshot.base || cs.capacity != g.csSnapshot.capacity ||
        cs.count != g.csSnapshot.count) {
        ++g.report.headerMoved;
        g.csSnapshot = cs;
    }

    // Per REGISTRATION, not per module lifetime: a dispatch from a space we have left is counted in
    // staleInvocations and confirms nothing here (AUDIT_W2-F_R2 R2-2).
    if (g.mb.registrationInvocations.load(std::memory_order_relaxed) == 0) {
        ++g.report.unconfirmedTicks;
        ++g.ticksSinceRegistration;
        if (g.ticksSinceRegistration >= kUnconfirmedTicks && g.report.status == Status::Registered) {
            g.report.status = Status::Unconfirmed;
        }
    }
}

bool TryRegister(void* gameSpace) {
    // THE GUARD THAT WAS MISSING, and it runs FIRST. The prologue guards below verify the code we
    // are about to call; these verify the two objects we are about to hand it, which is the half
    // that crashed. Object before code, so that a build whose guard bytes cannot even be read
    // still cannot be reached with an unbuilt space -- and so this guard is drivable from a test.
    const TableCheck cs = CheckComponentSystemTable(gameSpace);
    // Never call the engine's lookup on a table the engine has not sorted: with bit 0 set it
    // sorts in place before searching, and on a stale block that is a write into freed memory.
    // The engine's own per-frame lookups sort a live table inside the frame that builds it, so a
    // table that stays unsorted for many ticks is not a live table, and unsortedDeferrals says so.
    //
    // Asked BEFORE the generic not-Ready arm below, and keeping its own bit test. W1-C's tableguard
    // is about to answer this case itself as NotReady/SortPending (AUD-F3B F3B-1); with the generic
    // arm first, that specific verdict would be swallowed and this refusal would stop being
    // attributed to UnsortedTable. Testing the bit here makes the site behave identically before
    // and after that guard lands, and depends on no enumerator the current header lacks. The bit is
    // read only on a header tableguard actually read: on Unreadable the raw fields are zeroed, and
    // a flags word of zero is evidence of nothing.
    if (cs.verdict != TableVerdict::Unreadable && (cs.raw.flags & 1u) != 0) {
        ++g.report.unsortedDeferrals;
        NoteRefusal(Refusal::UnsortedTable, cs);
        return false;
    }
    if (cs.verdict != TableVerdict::Ready) {
        NoteRefusal(Refusal::ComponentSystemTable, cs);
        return false;
    }

    // The registrar appends into the update-record table and reallocates it, so it is the second
    // object this function hands to engine code and it gets the same test. An empty one is legal
    // here -- an append does not need anything to already be there.
    const TableCheck rec = CheckUpdateRecordTable(gameSpace, true);
    if (rec.verdict != TableVerdict::Ready) {
        NoteRefusal(Refusal::UpdateRecordTable, rec);
        return false;
    }

    FnLookupBySymbol lookup = g.testLookup;
    FnRegisterUpdate reg = g.testRegister;
    if (lookup == nullptr || reg == nullptr) {
        const void* regMapped = buildguard::MappedAddress(facts::CS_GAMESPACE_REGISTER_UPDATE);
        const void* lookupMapped = buildguard::MappedAddress(facts::CS_LOOKUP_BY_SYMBOL);
        if (regMapped == nullptr || lookupMapped == nullptr ||
            !buildguard::VerifySite("CS.GAMESPACE_REGISTER_UPDATE",
                                    facts::CS_GAMESPACE_REGISTER_UPDATE,
                                    facts::CS_GAMESPACE_REGISTER_UPDATE_PROLOGUE,
                                    sizeof facts::CS_GAMESPACE_REGISTER_UPDATE_PROLOGUE) ||
            !buildguard::VerifySite("CS.LOOKUP_BY_SYMBOL", facts::CS_LOOKUP_BY_SYMBOL,
                                    facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE,
                                    sizeof facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE)) {
            g.report.status = Status::RegistrarUnavailable;
            SetDiagnostic(
                "the component-system registrar or its symbol lookup did not pass its prologue "
                "guard, so the panel has no draw point. Another tool may have hooked it.");
            return false;
        }
        lookup = reinterpret_cast<FnLookupBySymbol>(const_cast<void*>(lookupMapped));
        reg = reinterpret_cast<FnRegisterUpdate>(const_cast<void*>(regMapped));
    }

    void* csObject = lookup(gameSpace, facts::CS_SYM_R14NETPHYSICS);
    if (csObject == nullptr) {
        // Every stock registration site tests exactly this and skips. The component system is
        // simply not in this space; it is not an error, and it may be present in the next one.
        g.report.status = Status::NoComponentSystem;
        SetDiagnostic(
            "no r14NetPhysics component system in this game space, so there is nothing to hang the "
            "update on. Stock registrations skip on the same test.");
        return false;
    }

    PinSelf();

    Pmf24 pmf = {};
    pmf.fn = reinterpret_cast<void*>(&ThunkDraw);

    const uint64_t before = rec.raw.count;
    reg(gameSpace, csObject, &pmf, "Update", "echoreplayUI", kOrderKey, kTiebreak, nullptr,
        kSchemeIndex);
    const TableCheck afterCheck = CheckUpdateRecordTable(gameSpace, true);
    const uint64_t after =
        afterCheck.verdict != TableVerdict::Unreadable ? afterCheck.raw.count : before;

    g.report.recordsBefore = before;
    g.report.recordsAfter = after;
    g.report.gameSpace = gameSpace;
    ++g.report.registrations;

    // Recorded as registered only once the record count has moved. Set before the test, a space
    // the registrar did not accept was never retried.
    if (after <= before) {
        // The registrar has no failure path, so this should be unreachable -- which is exactly why
        // it is worth saying out loud rather than assuming.
        g.report.status = Status::RegistrarUnavailable;
        SetDiagnostic(
            "the registrar returned but the record count did not move (%llu -> %llu). The registrar "
            "has no failure path, so this means the scheme layout is not what facts.tsv describes.",
            static_cast<unsigned long long>(before), static_cast<unsigned long long>(after));
        if (g.report.registrations == 1 || g.report.registrations % kLogEveryDeferrals == 0) {
            log::Direct(LogLevel::Error, kReplayModuleName, "%s", g.diagnostic);
        }
        return false;
    }

    // Only a completed registration sets the attribution target: set above the test, a registrar
    // call that did not move the count would leave the previous registration's count in place and
    // the next stale dispatch would raise Running over RegistrarUnavailable.
    SetRegistration(gameSpace, csObject);
    g.report.componentSystem = csObject;
    g.csSnapshot = cs.raw;
    g.ticksSinceRegistration = 0;
    g.report.status = Status::Registered;
    SetDiagnostic(
        "registered at band 3 (orderKey 0x%04X) on game space %p; records %llu -> %llu, after "
        "deferring %llu tick(s) while the space was still being built",
        kOrderKey, gameSpace, static_cast<unsigned long long>(before),
        static_cast<unsigned long long>(after),
        static_cast<unsigned long long>(g.report.deferralsBeforeFirst));
    log::Direct(LogLevel::Info, kReplayModuleName, "%s", g.diagnostic);
    return true;
}

}  // namespace

const char* StatusName(Status s) {
    switch (s) {
        case Status::NotRegistered: return "NotRegistered";
        case Status::NoGameSpace: return "NoGameSpace";
        case Status::SpaceNotReady: return "SpaceNotReady";
        case Status::NoComponentSystem: return "NoComponentSystem";
        case Status::RegistrarUnavailable: return "RegistrarUnavailable";
        case Status::Registered: return "Registered";
        case Status::Running: return "Running";
        case Status::WrongThread: return "WrongThread";
        case Status::Stale: return "Stale";
        case Status::Unconfirmed: return "Unconfirmed";
        case Status::NetGameUnreadable: return "NetGameUnreadable";
        case Status::Invoked: return "Invoked";
        case Status::CallbackSilent: return "CallbackSilent";
        case Status::MarshalStale: return "MarshalStale";
        case Status::LiveInert: return "LiveInert";
    }
    return "<invalid>";
}

const char* RefusalName(Refusal r) {
    switch (r) {
        case Refusal::None: return "none";
        case Refusal::ComponentSystemTable: return "component-system table is not ready";
        case Refusal::UpdateRecordTable: return "update-record table is not ready";
        case Refusal::UnsortedTable:
            return "component-system table is built but unsorted (the lookup would sort it in "
                   "place)";
    }
    return "<invalid>";
}

void SetDrawFn(DrawFn fn) {
    g.draw = fn;
    // The row-2.5 site draws the same panel. Wiring it here rather than at the module keeps the two
    // sites from being handed different draw functions by an edit that only remembered one.
    inframe::SetDrawFn(fn);
}

void OnTick(void* netGame, uint32_t currentThreadId) {
    g.report.mainThreadId = currentThreadId;
    g.ownerThreadId.store(currentThreadId, std::memory_order_relaxed);
    // The row-2.5 site refuses to draw on any other thread, and this is where "any other" gets its
    // meaning. Stamped from the same value as our own owner id, so the two sites cannot disagree.
    inframe::NoteOwnerThread(currentThreadId);

    // The denominator marshalledDraws is read against (AUDIT_F3E F3E#2): with the render-cadence
    // invariant the two are equal on a launch whose panel was enabled throughout, so a ratio under
    // 1.0 is a measured frame drop rather than an argument about one.
    ++g.report.ownerTicks;
    // ** ARCH-15 S7 / RES-W1M-C0 S6, measured rather than argued: is the engine's GUI frame byte
    // set HERE, at the top of the stage-0 drain -- the place the panel used to be drawn from? S1.4
    // says it is 0 on every tick forever, and that is the premise the whole row-2.5 site rests on.
    // Taken before any of this tick's work, so it is the byte as the drain would have found it,
    // and stamped with the tick number just assigned -- 1-based, so a lasttick of 0 means never.
    inframe::SampleFrameOpenAtOwnerTick(g.report.ownerTicks);
    // The previous tick, closed out before the new one opens. A tick that stood aside for an inline
    // dispatch that never came is the one frame the inline preference costs, and it is a number
    // rather than a silent gap.
    if (g.deferredThisTick && !g.drewThisTick) ++g.report.inlineDeferralsUnfilled;
    g.inlineDispatchLastTick = g.inlineDispatchThisTick;
    g.inlineDispatchThisTick = false;
    g.deferredThisTick = false;
    // A new tick, so the frame's single draw has not happened yet.
    g.drewThisTick = false;
    g.marshalledCreditedThisTick = false;

    // Liveness is measured in owner ticks, so it is counted here and nowhere else. Both counters
    // advance before the drain: a tick that runs the draw resets ticksSinceMarshalledDraw below.
    const uint64_t seen = g.mb.invocations.load(std::memory_order_relaxed);
    if (seen != g.invocationsAtLastTick) {
        g.invocationsAtLastTick = seen;
        g.ticksSinceInvocation = 0;
    } else {
        ++g.ticksSinceInvocation;
    }
    ++g.ticksSinceMarshalledDraw;

    // Before the registration work, and outside every early return below: the engine keeps
    // dispatching our record for the life of the space it was registered into, so a tick that
    // finds no netgame at all can still owe the panel a frame.
    DrainMarshal(DrawSite::OwnerTick);

    if (netGame == nullptr) {
        if (g.registeredSpace == nullptr) g.report.status = Status::NoGameSpace;
        return;
    }
    // NETGAME.CS_SPACE_OFF, not IDENT.NETGAME_GAMESPACE_OFF: +0x8 is the CR15Game back-pointer and
    // its +0x420 is a render-target budget block, which is why this site read NotReady forever
    // (ARCH-10 section 1). A real CR15NetGame is wider than this slot, so a slot we cannot read is
    // not a netGame -- a distinct answer from a netGame that carries no space yet.
    const void* const slot = static_cast<const uint8_t*>(netGame) + facts::NETGAME_CS_SPACE_OFF;
    if (!Readable(slot, sizeof(void*))) {
        if (g.registeredSpace == nullptr) g.report.status = Status::NetGameUnreadable;
        return;
    }
    void* gameSpace = *static_cast<void* const*>(slot);
    if (gameSpace == nullptr) {
        if (g.registeredSpace == nullptr) g.report.status = Status::NoGameSpace;
        return;
    }
    if (gameSpace != g.lastSpace) {
        // A new game space. The old registration died with the old space and there is no
        // unregister call on PC, so what follows is an append into the new one, not a move.
        NoteSpaceChange(g.lastSpace, gameSpace);
        g.lastSpace = gameSpace;
        SetRegistration(nullptr, nullptr);
    }
    if (g.registeredSpace != nullptr) {
        ValidateRegistration(gameSpace);
        return;
    }
    // Nothing registered here yet, a refusal being retried, or a Stale registration being
    // replaced under the same pointer: all three are the same call, gated by the same predicate.
    //
    // ★ THE LAST BAND-3 WRITE (RECORDER-3). Registering appends a 72-byte record into the SPACE's
    // own update-record table and hands the engine a lookup that sorts the component-system table
    // IN PLACE -- two writes into a space that, in a live online match, belongs to everyone in it.
    // The gate is here rather than inside TryRegister so that neither write is reached: the lookup
    // is the first thing TryRegister does. The panel does not depend on any of it (Enable() below
    // is this module's own open state), so a refused live session draws exactly as a registered one
    // and only loses compositing order and the dispatch telemetry.
    if (!livesafety::Allowed(livesafety::Feature::GameSpaceRegister)) {
        g.report.status = Status::LiveInert;
        if (!g.liveRegisterRefused) {
            g.liveRegisterRefused = true;
            log::Direct(LogLevel::Info, kReplayModuleName,
                        "live-safety: gamespace-register=inert(%s) -- refused at the site; the "
                        "panel draws without it",
                        livesafety::VerdictName(livesafety::Current()));
        }
        return;
    }
    TryRegister(gameSpace);
}

// ★★ THE ENABLE IS THIS MODULE'S OWN OPEN STATE, NOT THE ENGINE'S REGISTRATION (AUDIT_F3I F3I-4).
// It used to be set inside TryRegister's success path, which made the whole viewer conditional on a
// band-3 hook that may never land -- and a launch where the hook fails would then show the owner
// nothing at all. Registration buys compositing ORDER and telemetry; it does not buy the panel.
// So: the module opens the panel, the owner tick draws it, and Status reports separately whether
// the engine ever dispatched us.
//
// The window baseline is stamped on the closed -> open TRANSITION only. Re-enabling an already-open
// panel must not restart the measurement window under the reader (ARCH-13 s2.3 scores a difference
// across this stamp), and a second Enable() is otherwise a no-op.
void Enable() {
    if (g.enabled) return;
    g.enabled = true;
    g.report.ownerTicksAtEnable = g.report.ownerTicks;
    g.report.ownerDrawsAtEnable = g.report.ownerDraws;
    g.report.marshalledDrawsAtEnable = g.report.marshalledDraws;
    g.report.enableStamps += 1;
}

void Disable() {
    g.enabled = false;
    // Deliberately not clearing g.draw or g.registeredSpace: the engine still owns a pointer to
    // ThunkDraw and will keep calling it. `enabled` is the one flag it reads.
}

// DESIGN S8.5.4a's definition of Running, VERBATIM -- `registrations >= 1 AND invocations > 0 AND
// inFrameDraws > 0` (leg 3 re-ruled by ARCH-15 S4.2, replacing ARCH-14 3c4e669's ownerDraws) -- and nothing else (AUDIT_F3E F3E-1: the code used to score leg 2 as
// `registrationInvocations != 0`, which disagrees with the design on the only record we have,
// L5 re-run 3's `invocations=4681 registrationInvocations=0`, and folded a recency term the design
// does not have into the word). Scored where the Report is READ rather than on the tick, because
// one of the failures IS the owner thread not ticking, and a verdict computed only on a tick could
// never report it. Nothing here mutates state.
Status ScoreLiveness(const Report& r) {
    // LEG 1. Anything but a live registration is leg 1's answer and leg 1 already named it.
    if (r.registrations == 0) return r.status;
    if (r.status != Status::Registered && r.status != Status::Unconfirmed) return r.status;
    // LEG-1 RUNG L1 (ARCH-13 844d21b s1.2). `invocations` is a LIFETIME total and registration is
    // one-way on PC, so a game space we have left goes on dispatching our old record forever and
    // that total cannot tell this registration from a dead one. `registrationInvocations` is the
    // only counter that can, and ARCH-13 puts it HERE -- on the leg-1 ladder -- and nowhere in the
    // three legs. So a registration that has gone kUnconfirmedTicks without a dispatch OF ITS OWN
    // stays Unconfirmed however busy the space we left is, and recovers the moment one arrives.
    if (r.status == Status::Unconfirmed && r.registrationInvocations == 0) return r.status;
    // LEG 2, EXACTLY AS S8.5.4a WRITES IT (ARCH-13 row L2): the engine has invoked us, on whatever
    // thread it chose. On the only dataset we have -- L5 re-run 3, invocations=4681
    // registrationInvocations=0 -- this leg is SATISFIED, which is the point of the L1 split.
    if (r.invocations == 0) return r.status;
    // LEG 3. Nothing of ours has been accepted inside the engine's GUI frame -- so the row-2.5 site
    // has not run, or the panel was closed or refused at every entry. S8.5.4a's own name for legs
    // 1+2 without leg 3.
    //
    // ** LEG 3 IS inFrameDraws (ARCH-15 S4.1/S4.2, re-ruling ARCH-14 3c4e669), THE SAME TERM
    // runningPerDesign USES, AND IT IS SET IN ONE PLACE. It was ownerDraws, and
    // ownerDraws has exactly one increment site -- behind AttemptFallbackDraw, which stands down
    // whenever the row-2.5 detour is armed. The detour armed IS the shipped build, so this line
    // returned Invoked forever on a build that was drawing perfectly, and MarshalStale,
    // CallbackSilent and Running below it were all unreachable. Two fields of one Report scoring
    // the same leg differently is the defect; there is now one term. ownerDraws keeps its narrowed
    // meaning (fallback draws that reached a frame) and gates nothing. marshalledDraws, the
    // band-3-consumed subset, is likewise a DIAGNOSTIC that feeds no gate.
    if (r.inFrameDraws == 0) return Status::Invoked;

    // All three legs hold: by S8.5.4a this session IS Running. What follows are SEPARATE named
    // states over a Running session that has since gone quiet -- deliberately not extra legs and
    // never folded into the definition above (F3E-1). Both are instruments the design does not
    // specify; ARCH-13 ratifies them or drops them, and dropping them changes only these two
    // lines. A stopped owner tick cannot be measured in owner ticks, so the backlog is what
    // detects it; ticks that run without drawing are the other direction (disabled, or the draw
    // function never installed).
    if (r.pendingInvocations >= kStarvedPending) return Status::MarshalStale;
    if (g.ticksSinceMarshalledDraw >= kLivenessTicks) return Status::MarshalStale;
    if (g.ticksSinceInvocation >= kLivenessTicks) return Status::CallbackSilent;
    return Status::Running;
}

Report Status_() {
    Report r = g.report;
    r.invocations = g.mb.invocations.load(std::memory_order_relaxed);
    r.offThreadInvocations = g.mb.offThread.load(std::memory_order_relaxed);
    r.registrationInvocations = g.mb.registrationInvocations.load(std::memory_order_relaxed);
    r.staleInvocations = g.mb.staleInvocations.load(std::memory_order_relaxed);
    r.lastStaleDispatch = g.mb.lastStaleDispatch.load(std::memory_order_relaxed);
    r.pendingInvocations = g.mb.pending.load(std::memory_order_relaxed);
    r.mailboxContention = g.mb.contention.load(std::memory_order_relaxed);
    r.mailboxTornReads = g.mb.tornReads.load(std::memory_order_relaxed);
    r.lastCallbackThreadId = g.mb.threadId.load(std::memory_order_relaxed);
    r.mainThreadId = g.ownerThreadId.load(std::memory_order_relaxed);

    const inframe::Counters& ic = inframe::Read();
    r.frameSiteEntries = ic.frameSiteEntries;
    r.inFrameDraws = ic.inFrameDraws;
    r.refusedDraws = ic.refusedDraws;
    r.refusedNoFrameByte = ic.refusedNoFrameByte;
    r.refusedNoLatch = ic.refusedNoLatch;
    r.refusedNoContext = ic.refusedNoContext;
    r.refusedByFacade = ic.refusedByFacade;
    r.refusedNoDraw = ic.refusedNoDraw;
    r.refusedBeginFalse = ic.refusedBeginFalse;
    r.beginRefusal = ic.beginRefusal;
    r.facadeBoundEntries = ic.facadeBoundEntries;
    r.facadeEntryCount = ic.facadeEntryCount;
    r.gateScopeEnabled = inframe::GateScopeEnabled();
    r.gateUnresolved = ic.gateUnresolved;
    r.gateAlreadyOpen = ic.gateAlreadyOpen;
    r.suppressScopeEntries = ic.suppressScopeEntries;
    r.suppressScopeRestores = ic.suppressScopeRestores;
    r.suppressScopeChanged = ic.suppressScopeChanged;
    r.suppressSetterUnmapped = ic.suppressSetterUnmapped;
    r.panelHoveredFrames = ic.panelHoveredFrames;
    r.drawNanosTotal = ic.drawNanosTotal;
    r.drawNanosMax = ic.drawNanosMax;
    r.firstDrawNanos = ic.firstDrawNanos;
    r.drawNanosBudget = inframe::kDrawNanosBudgetNs;
    r.imguiNanos = ic.imguiNanos;
    r.drawNanosP99 = ic.drawNanosP99;
    r.drawNanosP999 = ic.drawNanosP999;
    r.drawNanosSamples = ic.drawNanosSamples;
    r.drawNanosOverHistogram = ic.drawNanosOverHistogram;
    r.guiItemsPerFrame = ic.guiItemsPerFrame;
    r.guiGlyphsPerFrame = ic.guiGlyphsPerFrame;
    r.guiStreamFrames = ic.guiStreamFrames;
    r.browserScanNanos = ic.browserScanNanos;
    r.metadataReadNanos = ic.metadataReadNanos;
    r.textFormatNanos = ic.textFormatNanos;
    r.browserIoOnDrawThread = ic.browserIoOnDrawThread;
    r.panelClosedEntries = ic.panelClosedEntries;
    r.offThreadSiteEntries = ic.offThreadSiteEntries;
    r.frameOpenAtOwnerTickSamples = ic.frameOpenAtOwnerTickSamples;
    r.frameOpenAtOwnerTickNonZero = ic.frameOpenAtOwnerTickNonZero;
    r.frameOpenAtOwnerTickLastNonZeroTick = ic.frameOpenAtOwnerTickLastNonZeroTick;
    // ARCH-20 S5's block, carried across whole. It is NOT on the published text line: the line
    // that renders these fields is W2-R's own, built in observer_rows.h from this struct, and a
    // second copy of that format here is exactly the defect that certifies broken files.
    r.guiGateSampled = ic.guiGateSampled;
    r.guiGateAddrResolved = ic.guiGateAddrResolved;
    for (size_t i = 0; i < sizeof r.guiGateUnresolved; ++i) {
        r.guiGateUnresolved[i] = ic.guiGateUnresolved[i];
    }
    for (size_t i = 0; i < sizeof r.guiImguiBaseReason; ++i) {
        r.guiImguiBaseReason[i] = ic.guiImguiBaseReason[i];
    }
    r.guiGateSamples = ic.guiGateSamples;
    r.guiSuppressed = ic.guiSuppressed;
    r.guiHotkeyEnable = ic.guiHotkeyEnable;
    r.guiReentryDepth = ic.guiReentryDepth;
    r.guiWindowBudget = ic.guiWindowBudget;
    r.guiViewportsEnabled = ic.guiViewportsEnabled;
    r.guiBeginBalanceDelta = ic.guiBeginBalanceDelta;
    r.guiImguiBaseValid = ic.guiImguiBaseValid;
    r.guiAtlasDwordA = ic.guiAtlasDwordA;
    r.guiAtlasDwordB = ic.guiAtlasDwordB;
    r.guiAtlasExpectA = ic.guiAtlasExpectA;
    r.guiAtlasExpectB = ic.guiAtlasExpectB;
    r.guiImguiFrameCountDelta = ic.guiImguiFrameCountDelta;
    r.guiRenderDrawListsFnPresent = ic.guiRenderDrawListsFnPresent;
    r.guiRenderedCmdListsCount = ic.guiRenderedCmdListsCount;
    r.guiRenderedTotalVtxCount = ic.guiRenderedTotalVtxCount;
    r.fallbackEntries = ic.fallbackEntries;
    r.inframeInstalled = inframe::Installed();
    r.inframeStatus = inframe::InstallStatus();

    // DESIGN S8.5.4a's three parts, verbatim and independent of the word ScoreLiveness picks.
    // ★ LEG 3 IS inFrameDraws, NOT ownerDraws (ARCH-15 S4.1/S4.2 re-ruling ARCH-14 3c4e669). The
    // old term counted draw ATTEMPTS at a site where every attempt is refused, so it was satisfied
    // on a build that put nothing on screen. inFrameDraws cannot be non-zero unless a widget call
    // was accepted inside the engine's own GUI frame, which is exactly what the owner's "embedded"
    // buys over an overlay.
    r.runningPerDesign = r.registrations >= 1 && r.invocations > 0 && r.inFrameDraws > 0;
    r.status = ScoreLiveness(r);
    return r;
}

const char* Diagnostic() {
    const Report r = Status_();
    if (r.status == Status::Unconfirmed) {
        // ★ The two silent traps present identically, so both are named rather than guessed
        // between. This is the sentence the parked read w2f-band3-registration-lands resolves.
        SetDiagnostic(
            "UNCONFIRMED: registered (records %llu -> %llu), our record is still in the table, "
            "and the callback has not fired FOR THIS REGISTRATION in %llu tick(s) (%llu dispatch(es) "
            "did arrive from a component system we are not registered with, last %p). Two known "
            "causes look identical from here: bit 23 of *(uint32_t*)(game + 0x828) skips the whole "
            "stage 0-3 component-system pass, or the order key landed outside a band. The key we "
            "used, 0x%04X, is inside band 3, so the gate bit is the likelier of the two.",
            static_cast<unsigned long long>(g.report.recordsBefore),
            static_cast<unsigned long long>(g.report.recordsAfter),
            static_cast<unsigned long long>(g.ticksSinceRegistration),
            static_cast<unsigned long long>(g.report.staleInvocations),
            g.report.lastStaleDispatch, kOrderKey);
    } else if (r.status == Status::Registered && r.registrationInvocations == 0) {
        SetDiagnostic(
            "registered (records %llu -> %llu) on game space %p; provisional until the callback "
            "fires. %llu tick(s) so far; Unconfirmed after %llu.",
            static_cast<unsigned long long>(g.report.recordsBefore),
            static_cast<unsigned long long>(g.report.recordsAfter), g.report.gameSpace,
            static_cast<unsigned long long>(g.ticksSinceRegistration),
            static_cast<unsigned long long>(kUnconfirmedTicks));
    } else if (r.status == Status::Invoked || r.status == Status::MarshalStale) {
        SetDiagnostic(
            "%s: the band-3 callback is being dispatched (%llu entries, %llu of them "
            "off the owner thread %lu, last on thread %lu) and the owner thread has run %llu "
            "draw(s), %llu of them carrying band-3 entries, over %llu owner tick(s), the last "
            "%llu tick(s) ago, with %llu "
            "entry(s) still undrained. The panel is drawn on every owner tick while enabled and "
            "the callback records and returns by design -- ImGui is not thread-safe -- so nothing "
            "is on screen. Either our stage-0 tick has stopped, or the panel is disabled, or the "
            "draw callback was never installed.",
            r.status == Status::Invoked ? "INVOKED, NEVER DRAWN" : "MARSHAL STALE",
            static_cast<unsigned long long>(r.invocations),
            static_cast<unsigned long long>(r.offThreadInvocations),
            static_cast<unsigned long>(r.mainThreadId),
            static_cast<unsigned long>(r.lastCallbackThreadId),
            static_cast<unsigned long long>(r.inFrameDraws),
            static_cast<unsigned long long>(r.marshalledDraws),
            static_cast<unsigned long long>(r.ownerTicks),
            static_cast<unsigned long long>(g.ticksSinceMarshalledDraw),
            static_cast<unsigned long long>(r.pendingInvocations));
    } else if (r.status == Status::CallbackSilent) {
        SetDiagnostic(
            "CALLBACK SILENT: the registration was dispatched %llu time(s) and then stopped -- no "
            "entry at all in the last %llu owner tick(s), while %llu draw(s) have run. "
            "The record is still in the space's table, so this is the engine no longer running the "
            "band-3 pass, not a registration we have lost.",
            static_cast<unsigned long long>(r.registrationInvocations),
            static_cast<unsigned long long>(g.ticksSinceInvocation),
            static_cast<unsigned long long>(r.inFrameDraws));
    } else if (r.status == Status::WrongThread) {
        SetDiagnostic(
            "WRONG THREAD: the draw path was entered on thread %lu while the owner thread is %lu, "
            "%llu time(s). After the marshal the callback draws nothing and only the owner thread "
            "drains, so this cannot happen without a caller reaching the drain from somewhere else. "
            "An off-thread DISPATCH is normal and is the %llu of %llu counted in "
            "offThreadInvocations.",
            static_cast<unsigned long>(::GetCurrentThreadId()),
            static_cast<unsigned long>(r.mainThreadId),
            static_cast<unsigned long long>(r.wrongThreadDrains),
            static_cast<unsigned long long>(r.offThreadInvocations),
            static_cast<unsigned long long>(r.invocations));
    } else if (r.status == Status::Running) {
        SetDiagnostic(
            "running: %llu dispatch(es) for this registration (%llu of %llu entries off the owner "
            "thread), the panel drawn %llu time(s) on owner thread %lu; %llu entry(s) coalesced, "
            "%llu undrained, last entry seq %llu from thread %lu at %llu ns, order key 0x%04X.",
            static_cast<unsigned long long>(r.registrationInvocations),
            static_cast<unsigned long long>(r.offThreadInvocations),
            static_cast<unsigned long long>(r.invocations),
            static_cast<unsigned long long>(r.marshalledDraws),
            static_cast<unsigned long>(r.mainThreadId),
            static_cast<unsigned long long>(r.coalescedInvocations),
            static_cast<unsigned long long>(r.pendingInvocations),
            static_cast<unsigned long long>(r.lastInvocation.sequence),
            static_cast<unsigned long>(r.lastInvocation.threadId),
            static_cast<unsigned long long>(r.lastInvocation.monotonicNs),
            r.lastInvocation.orderKey);
    } else if (g.diagnostic[0] == '\0') {
        SetDiagnostic("%s", StatusName(r.status));
    }
    // ** THE PREMISE FALSIFIED IS LOUD, NOT A COUNTER SOMEBODY HAS TO GO LOOKING FOR (ARCH-15 S7).
    // The prediction is 0 non-zero samples; anything else means the owner tick CAN reach an open
    // frame and the reasoning that picked the row-2.5 site needs re-reading. It is prepended to the
    // sentence rather than folded into a status word, because it is not a failure of ours.
    char premise[kPremiseMax];
    premise[0] = '\0';
    if (r.frameOpenAtOwnerTickNonZero != 0) {
        _snprintf_s(premise, sizeof premise, _TRUNCATE,
                    " ** GUI FRAME OPEN AT STAGE 0: %llu of %llu owner-tick sample(s) found "
                    "GUI.FRAME_OPEN_BYTE non-zero, last on owner tick %llu. ARCH-15 S1.4 predicts "
                    "0 of them.",
                    static_cast<unsigned long long>(r.frameOpenAtOwnerTickNonZero),
                    static_cast<unsigned long long>(r.frameOpenAtOwnerTickSamples),
                    static_cast<unsigned long long>(r.frameOpenAtOwnerTickLastNonZeroTick));
    }
    // The fixed suffix, on every line without exception. See the header for the grammar W2-R parses.
    // `[stage0 ...]` and `[facade ...]` are their OWN groups, placed after `[inframe ...]` and
    // before the threads tail: the inframe group is matched field for field by W2-R
    // (tests/replay/observer_rows.h sscanf, ten fields, ending `installed=%d]`) and widening it
    // would break that parse silently, while `[threads owner=<n> callback=<n>]` must stay LAST
    // because observer_rows.h:1662 reads a line not ending in it as a broken contract.
    char suffix[kSuffixMax];
    const int suffixLen = _snprintf_s(
        suffix, sizeof suffix, _TRUNCATE, kSuffixFormat,
        static_cast<unsigned long long>(r.ownerTicks),
        static_cast<unsigned long long>(r.ownerDraws),
        static_cast<unsigned long long>(r.frameSiteEntries),
        static_cast<unsigned long long>(r.inFrameDraws),
        static_cast<unsigned long long>(r.refusedDraws),
        static_cast<unsigned long long>(r.refusedNoFrameByte),
        static_cast<unsigned long long>(r.refusedNoLatch),
        static_cast<unsigned long long>(r.refusedNoContext),
        static_cast<unsigned long long>(r.refusedByFacade),
        static_cast<unsigned long long>(r.panelClosedEntries),
        static_cast<unsigned long long>(r.fallbackEntries),
        r.inframeInstalled ? 1 : 0,
        static_cast<unsigned long long>(r.frameOpenAtOwnerTickSamples),
        static_cast<unsigned long long>(r.frameOpenAtOwnerTickNonZero),
        static_cast<unsigned long long>(r.frameOpenAtOwnerTickLastNonZeroTick),
        static_cast<unsigned long long>(r.refusedNoDraw),
        static_cast<unsigned long long>(r.refusedBeginFalse),
        inframe::BeginRefusalName(r.beginRefusal),
        r.facadeBoundEntries, r.facadeEntryCount,
        r.gateScopeEnabled ? 1 : 0,
        static_cast<unsigned long long>(r.gateUnresolved),
        static_cast<unsigned long long>(r.gateAlreadyOpen),
        static_cast<unsigned long long>(r.suppressScopeEntries),
        static_cast<unsigned long long>(r.suppressScopeRestores),
        static_cast<unsigned long long>(r.suppressScopeChanged),
        static_cast<unsigned long long>(r.suppressSetterUnmapped),
        static_cast<unsigned long long>(r.panelHoveredFrames),
        inframe::PanelHoverSourceName(),
        static_cast<unsigned long long>(r.drawNanosTotal),
        static_cast<unsigned long long>(r.drawNanosMax),
        static_cast<unsigned long>(r.mainThreadId),
        static_cast<unsigned long>(r.lastCallbackThreadId));

    // ** A CONTRACT LINE THAT DOES NOT FIT REFUSES BY NAME; IT NEVER LOSES ITS TAIL (L5-RUN-5 G1).
    // The prose is for a human and the suffix is for a parser, so when only one of them can be
    // published it is the prose that goes -- and it says so, in words, where the prose was.
    if (suffixLen < 0) {
        // The suffix overran its own declared bound. Nothing here can be salvaged into a parseable
        // line, so the line names the bound it exceeded instead of publishing a partial group that
        // a reader would score field by field.
        _snprintf_s(g.published, sizeof g.published, _TRUNCATE,
                    "DIAGNOSTIC SUFFIX DOES NOT FIT ITS %zu-BYTE BUDGET: the [ticks ...] "
                    "[inframe ...] [stage0 ...] [facade ...] [gate ...] [threads ...] groups are "
                    "NOT on this "
                    "line and no field of them may be read as absent-means-zero. %s",
                    kSuffixMax, g.diagnostic);
        return g.published;
    }
    if (_snprintf_s(g.published, sizeof g.published, _TRUNCATE, "%s%s%s", g.diagnostic, premise,
                    suffix) < 0) {
        _snprintf_s(g.published, sizeof g.published, _TRUNCATE,
                    "DIAGNOSTIC PROSE DROPPED (it and the premise exceeded the %zu-byte line "
                    "budget): the status sentence is NOT on this line; the groups after this are "
                    "complete.%s",
                    sizeof g.published, suffix);
    }
    return g.published;
}

const char* PublishedSuffixFormatForTest() { return kSuffixFormat; }
size_t PublishedSuffixBudgetForTest() { return kSuffixMax; }
size_t PublishedLineBudgetForTest() { return sizeof g.published; }

void ResetForTest() {
    g.draw = nullptr;
    g.enabled = false;
    g.report = Report();
    g.mb.seq.store(0, std::memory_order_relaxed);
    g.mb.sequence.store(0, std::memory_order_relaxed);
    g.mb.monotonicNs.store(0, std::memory_order_relaxed);
    g.mb.threadId.store(0, std::memory_order_relaxed);
    g.mb.orderKey.store(0, std::memory_order_relaxed);
    g.mb.invocations.store(0, std::memory_order_relaxed);
    g.mb.offThread.store(0, std::memory_order_relaxed);
    g.mb.staleInvocations.store(0, std::memory_order_relaxed);
    g.mb.lastStaleDispatch.store(nullptr, std::memory_order_relaxed);
    g.mb.pending.store(0, std::memory_order_relaxed);
    g.mb.contention.store(0, std::memory_order_relaxed);
    g.mb.tornReads.store(0, std::memory_order_relaxed);
    g.ownerThreadId.store(0, std::memory_order_relaxed);
    g.ticksSinceInvocation = 0;
    g.ticksSinceMarshalledDraw = 0;
    g.invocationsAtLastTick = 0;
    g.publishHook = nullptr;
    g.readHook = nullptr;
    g.drewThisTick = false;
    g.inlineDispatchThisTick = false;
    g.inlineDispatchLastTick = false;
    g.deferredThisTick = false;
    g.marshalledCreditedThisTick = false;
    g.published[0] = '\0';
    SetRegistration(nullptr, nullptr);
    g.lastSpace = nullptr;
    g.csSnapshot = TableRaw();
    g.ticksSinceRegistration = 0;
    g.testLookup = nullptr;
    g.testRegister = nullptr;
    g.diagnostic[0] = '\0';
    g.liveRegisterRefused = false;
    // g.pinned is deliberately left alone: a pin cannot be undone.
}

void SetEngineForTest(FnLookupBySymbol lookup, FnRegisterUpdate registrar) {
    g.testLookup = lookup;
    g.testRegister = registrar;
}

bool ReadInvocationForTest(Invocation* out) { return ReadMailbox(out); }

void SetPublishHookForTest(void (*hook)()) { g.publishHook = hook; }

void SetReadHookForTest(void (*hook)()) { g.readHook = hook; }

}  // namespace csregister
}  // namespace ui
}  // namespace replay
