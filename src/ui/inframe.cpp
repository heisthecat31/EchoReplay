#include "ui/inframe.h"

#include "ui/demoindex.h"

#include <windows.h>

#include <atomic>
#include <ratio>

#include <string.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/config.h"
#include "bootstrap/log.h"
#include "generated/facts_generated.h"
#include "ui/gui.h"
#include "ui/ui_facts.h"

namespace replay {
namespace ui {
namespace inframe {
namespace {

const char* const kModuleName = "ui";

// A string literal into a fixed field. A null source is the EMPTY string and never a stale one:
// the reader tells "the producer named none" from a name, and cannot be handed last entry's.
void CopyFixed(char* dst, size_t n, const char* src) {
    size_t i = 0;
    if (src != nullptr) {
        for (; i + 1 < n && src[i] != '\0'; ++i) dst[i] = src[i];
    }
    dst[i] = '\0';
}
const char* const kFactId = "GUI.ENGINE_HEARTBEAT_BEFORE";
const char* const kConfigKey = "ui.inframe_draw";
const char* const kGateScopeKey = "ui.gate_scope";

// Must equal config_keys.inc's declared default for this row. Handing GetBool a different one
// makes config log a default-drift warning, which is the check that these two cannot separate.
const bool kGateScopeDefault = true;

// The runner's override, ECHOREPLAY_UI_GATE_SCOPE, is NOT parsed here any more. It is declared on
// the row (kConfigEnvOverride) and resolved by config, so the ledger dump and this feature read
// one value. It is an ENVIRONMENT VARIABLE and never a command-line switch: an unregistered switch
// makes the engine discard its whole command line, -offline and every host pin with it

// The engine's HeartbeatBefore takes one pointer in rcx. It is DECLARED as returning a 64-bit value
// even though nothing has established that it does: a void-returning detour that calls a
// value-returning original leaves rax holding our own garbage, and forwarding the register costs
// nothing and cannot be wrong in the other direction.
using HeartbeatFn = uint64_t(__fastcall*)(void*);

struct State {
    std::atomic<uint64_t> frameSiteEntries{0};
    std::atomic<uint64_t> inFrameDraws{0};
    std::atomic<uint64_t> refusedDraws{0};
    std::atomic<uint64_t> refusedNoFrameByte{0};
    std::atomic<uint64_t> refusedNoLatch{0};
    std::atomic<uint64_t> refusedNoContext{0};
    std::atomic<uint64_t> refusedNoDraw{0};
    std::atomic<uint64_t> refusedBeginFalse{0};
    std::atomic<uint8_t> beginRefusal{static_cast<uint8_t>(BeginRefusal::None)};
    std::atomic<uint32_t> facadeBoundEntries{0};
    std::atomic<uint32_t> facadeEntryCount{0};
    std::atomic<uint64_t> panelClosedEntries{0};
    std::atomic<uint64_t> fallbackEntries{0};
    std::atomic<uint64_t> offThreadSiteEntries{0};
    std::atomic<uint64_t> frameOpenSamples{0};
    std::atomic<uint64_t> frameOpenNonZero{0};
    std::atomic<uint64_t> frameOpenLastNonZeroTick{0};

    std::atomic<DrawFn> draw{nullptr};
    std::atomic<bool> installed{false};
    // Written by the install worker thread and read by the ENGINE thread inside the detour, which
    // dereferences `original`. Every other cross-thread field here is an atomic and
    // these two were the ones the hot path touches, so they are not left to x64's good manners.
    std::atomic<HookStatus> installStatus{HookStatus::Absent};
    std::atomic<HeartbeatFn> original{nullptr};

    // 0 until csregister's owner tick stamps it or the first site entry latches its own.
    std::atomic<uint32_t> ownerThreadId{0};

    // Test probe. Null means "read the mapped image", which is the production path.
    const uint8_t* probeFrameOpen = nullptr;
    const uint8_t* probeLatch = nullptr;
    const void* const* probeContext = nullptr;
    bool probeSet = false;

    std::atomic<uint64_t> gateUnresolved{0};
    std::atomic<uint64_t> gateAlreadyOpen{0};
    std::atomic<uint64_t> suppressScopeEntries{0};
    std::atomic<uint64_t> suppressScopeRestores{0};
    std::atomic<uint64_t> suppressScopeChanged{0};
    std::atomic<uint64_t> suppressSetterUnmapped{0};
    std::atomic<uint64_t> panelHoveredFrames{0};
    std::atomic<bool> hoverAsked{false};
    std::atomic<uint64_t> drawNanosTotal{0};
    std::atomic<uint64_t> drawNanosMax{0};
    // The first timed draw, published on its own and kept OUT of the maximum. It is not a per-frame
    // cost: it is the frame that builds the window, rasterises every glyph the panel names into the
    // font atlas and reads the GUI's settings, and it happens once per process. Averaging it away
    // would hide it and letting it set the maximum reports a 0.63 ms per-frame budget breach that
    // no frame after the first can reproduce (L5-RUN-7 s4.4: max 629300 ns, mean 21500 ns).
    std::atomic<uint64_t> firstDrawNanos{0};
    std::atomic<uint64_t> imguiNanosMax{0};
    std::atomic<uint64_t> browserScanNanosMax{0};
    std::atomic<uint64_t> metadataReadNanosMax{0};
    std::atomic<uint64_t> textFormatNanosMax{0};
    std::atomic<uint64_t> timedDraws{0};
    // The distribution behind drawNanosMax. One shift and one relaxed increment per frame; the
    // percentiles are walked out of it at Read() time, which happens once per launch record.
    std::atomic<uint32_t> drawNanosHist[kDrawNanosBuckets] = {};
    std::atomic<uint64_t> drawNanosSamples{0};
    std::atomic<uint64_t> drawNanosOverHistogram{0};
    // What the browser page handed the facade. Maxima, with their own denominator so that zero
    // items cannot be read as a free frame when what happened is that no browser frame was drawn.
    std::atomic<uint64_t> guiItemsMax{0};
    std::atomic<uint64_t> guiGlyphsMax{0};
    std::atomic<uint64_t> guiStreamFrames{0};
    std::atomic<uint64_t> transportItemsMax{0};
    std::atomic<uint64_t> transportGlyphsMax{0};
    std::atomic<uint64_t> transportStreamFrames{0};

    // -1 = ask the configuration. Only a test writes anything else.
    std::atomic<int> gateScopeOverride{-1};
    // -1 = not asked yet. The configured answer, cached because this is read once per frame and the
    // lookup is by string; ResetForTest clears it so a test can drive the row both ways.
    std::atomic<int> gateScopeCached{-1};

    const uint8_t* probeSuppress = nullptr;
    const int32_t* probeReentry = nullptr;
    const int32_t* probeBudget = nullptr;
    const uint8_t* probeHotkey = nullptr;
    bool gateProbeSet = false;

    // The ARCH-20 S5 read side. Written on the owner thread at the row-2.5 site, read by whichever
    // thread calls Snapshot. The two string fields are literals, so publishing one is a pointer
    // store and there is no text to tear.
    std::atomic<uint64_t> guiGateSamples{0};
    std::atomic<const char*> guiUnresolvedId{nullptr};
    std::atomic<const char*> guiBaseReason{nullptr};
    std::atomic<uint32_t> guiSuppressed{0};
    std::atomic<uint32_t> guiHotkeyEnable{0};
    std::atomic<int32_t> guiReentryDepth{0};
    std::atomic<int32_t> guiWindowBudget{0};
    std::atomic<int32_t> guiViewportsEnabled{0};
    std::atomic<int32_t> guiBeginBalanceDelta{0};
    std::atomic<uint32_t> guiBaseValid{0};
    std::atomic<uint32_t> guiAtlasDwordA{0};
    std::atomic<uint32_t> guiAtlasDwordB{0};
    std::atomic<uint32_t> guiAtlasExpectA{0};
    std::atomic<uint32_t> guiAtlasExpectB{0};
    std::atomic<int32_t> guiFrameCountDelta{0};
    std::atomic<uint32_t> guiRenderDrawListsFnPresent{0};
    // ★ -1 IS COULD-NOT-ASK AND IS THE STARTING VALUE. A count is never negative, so no reader can
    // mistake the refusal for a measurement -- which is exactly what the 0 these two used to
    // publish did, on the run whose panel is in L5-RUN-7-run2.png. They hold a number only after
    // SampleGuiDrawData has seen an ImDrawData that is marked Valid AND agrees with itself.
    std::atomic<int32_t> guiRenderedCmdLists{kGuiCountUnasked};
    std::atomic<int32_t> guiRenderedTotalVtx{kGuiCountUnasked};
    std::atomic<uint64_t> guiDrawDataLatches{0};
    std::atomic<uint64_t> guiDrawDataRefused{0};
    std::atomic<uint64_t> guiDrawDataNotReady{0};
    // The previous entry's frame counter. Owner thread only, and cleared whenever the base stops
    // proving out: a delta across a gap in which we were reading some other object is not a delta.
    int32_t guiPrevFrameCount = 0;
    bool guiHavePrevFrameCount = false;
    const void* const* probeImguiSlot = nullptr;
    const uint32_t* probeAtlasA = nullptr;
    const uint32_t* probeAtlasB = nullptr;
    const int32_t* probeViewports = nullptr;
    const int32_t* probeBeginBalance = nullptr;
    bool guiProbeSet = false;
};

State g;

void Bump(std::atomic<uint64_t>& c) { c.fetch_add(1, std::memory_order_relaxed); }

// A maximum that several threads may raise. The browser worker and the draw thread both reach
// these, so a plain compare-and-store would lose the larger of two concurrent updates -- and the
// one it loses is, by construction, the tail we are trying to name.
void RaiseMax(std::atomic<uint64_t>& slot, uint64_t value) {
    uint64_t seen = slot.load(std::memory_order_relaxed);
    while (value > seen && !slot.compare_exchange_weak(seen, value, std::memory_order_relaxed,
                                                       std::memory_order_relaxed)) {
    }
}

// QueryPerformanceFrequency is fixed for the life of the process, so it is read once. A zero or
// failed frequency makes the measurement UNAVAILABLE rather than infinite: the counters stay put
// and drawNanosMax cannot be read as a fast frame that never happened.
uint64_t QpcTicksToNanos(uint64_t ticks) {
    static const int64_t freq = [] {
        LARGE_INTEGER f{};
        return ::QueryPerformanceFrequency(&f) ? f.QuadPart : 0;
    }();
    if (freq <= 0) return 0;
    // 128-bit-free and overflow-free for any plausible frame: ticks/freq seconds, then the
    // remainder scaled, so a 10 MHz counter cannot overflow at ticks * 1e9.
    const uint64_t whole = ticks / static_cast<uint64_t>(freq);
    const uint64_t rest = ticks % static_cast<uint64_t>(freq);
    return whole * 1000000000ull + (rest * 1000000000ull) / static_cast<uint64_t>(freq);
}

// One draw, timed. The clock reads bracket the call and nothing else, so what is published is the
// panel's cost and not our bookkeeping.
DrawOutcome TimedDraw(DrawFn draw) {
    LARGE_INTEGER before{};
    LARGE_INTEGER after{};
    const bool timed = ::QueryPerformanceCounter(&before) != 0;
    const DrawOutcome outcome = draw();
    if (timed && ::QueryPerformanceCounter(&after) != 0 && after.QuadPart >= before.QuadPart) {
        const uint64_t nanos =
            QpcTicksToNanos(static_cast<uint64_t>(after.QuadPart - before.QuadPart));
        g.drawNanosTotal.fetch_add(nanos, std::memory_order_relaxed);
        // The first timed draw is published on its own and does not compete for the maximum. It is
        // the once-per-process frame that creates the window, rasterises the panel's glyphs into
        // the font atlas and loads the GUI's settings; scoring it as a per-frame cost is what made
        // L5-RUN-7 report a 629300 ns budget breach against a 21500 ns mean. It is still inside
        // drawNanosTotal, so nothing has been hidden -- it has been named.
        if (g.timedDraws.fetch_add(1, std::memory_order_relaxed) == 0) {
            g.firstDrawNanos.store(nanos, std::memory_order_relaxed);
        } else {
            uint64_t seen = g.drawNanosMax.load(std::memory_order_relaxed);
            while (nanos > seen &&
                   !g.drawNanosMax.compare_exchange_weak(seen, nanos, std::memory_order_relaxed,
                                                         std::memory_order_relaxed)) {
            }
            // The same frames the maximum is taken over, bucketed. A frame above the top edge is
            // scored in the top bucket AND counted, so the percentile it produces is a lower
            // bound that says so rather than a number that quietly saturates.
            const bool over = nanos >= kDrawNanosHistogramTopNs;
            const size_t bucket =
                over ? kDrawNanosBuckets - 1 : static_cast<size_t>(nanos / kDrawNanosBucketNs);
            g.drawNanosHist[bucket].fetch_add(1, std::memory_order_relaxed);
            g.drawNanosSamples.fetch_add(1, std::memory_order_relaxed);
            if (over) g.drawNanosOverHistogram.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return outcome;
}

// Counting the refusal AND its reason. Every failing term is credited, not just the first: with
// only the first one, a frame that lost the context pointer and the latch together would read as a
// latch problem and the context would never appear in any record.
void CountRefusal(const FramePrecondition& p) {
    if (!p.frameOpenByte) Bump(g.refusedNoFrameByte);
    if (!p.startFrameLatch) Bump(g.refusedNoLatch);
    if (!p.contextPtr) Bump(g.refusedNoContext);
    Bump(g.refusedDraws);
}

bool Finish(DrawOutcome outcome);

// ---- ARCH-20 S5, the read side -----------------------------------------------------------------
//
// Engine memory sampled where GateScope reads its bytes: the owner thread, inside the frame. It
// writes nothing and calls nothing.
//
// ** THE BASE IS PROVEN BEFORE IT IS USED, ALL THREE TERMS (S5.1). GUI.IMGUI_CONTEXT holds a
// POINTER and is not the object; io.Fonts must be reachable behind the loaded value; and the two
// dwords the engine's own sub_140F922D0 stores at io.Fonts+0x20/+0x24 must equal GUI.ATLAS_CONST_A
// and _B READ FROM .rdata AT RUN TIME. Term 3 is the one a wrong base cannot satisfy, and it is
// the term AUDIT_ARCH_19_20 B1 needed: an unrelated live global read as the context yields
// perfectly plausible dwords for everything else.
struct GuiContextRead {
    uint32_t atlasA;
    uint32_t atlasB;
    int32_t frameCount;
    uint32_t renderDrawListsFnPresent;
};

// ImDrawData, read as a struct rather than as two loose offsets. See ui_facts.h for the layout,
// which is derived from the registry rows and asserted there.
struct DrawDataRead {
    uint8_t valid;
    const void* cmdLists;
    int32_t cmdListsCount;
    int32_t totalVtx;
    int32_t totalIdx;
};

bool ReadDrawData(const void* base, DrawDataRead* out) {
    __try {
        const unsigned char* const d =
            static_cast<const unsigned char*>(base) + ui_facts::kImguiDrawDataOff;
        out->valid = *reinterpret_cast<const volatile uint8_t*>(d + ui_facts::kDrawDataValidOff);
        out->cmdLists =
            *reinterpret_cast<void* const volatile*>(d + ui_facts::kDrawDataCmdListsPtrOff);
        out->cmdListsCount = *reinterpret_cast<const volatile int32_t*>(
            d + ui_facts::kDrawDataCmdListsCountOff);
        out->totalVtx =
            *reinterpret_cast<const volatile int32_t*>(d + ui_facts::kDrawDataTotalVtxOff);
        out->totalIdx =
            *reinterpret_cast<const volatile int32_t*>(d + ui_facts::kDrawDataTotalIdxOff);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// THE STRUCT CHECKED AGAINST ITSELF, which is what stands in for re-deriving the base at run time.
// Five terms, each of which unrelated memory has to satisfy simultaneously: a bool that is a bool,
// a plausible list count, a CmdLists slot that is an aligned non-null pointer exactly when there is
// a list to point at (the term that says this really is the layout ui_facts.h asserts, since the
// 1.86+ layout has an int there), indices never fewer than vertices (ImGui emits six per quad
// against four), and the two counts agreeing about whether the frame is empty. A base that points
// at padding fails this; the reader then publishes could-not-ask rather than the confident 0 that a
// wrong offset hands out for free.
bool DrawDataLooksSane(const DrawDataRead& d) {
    if (d.valid > 1) return false;
    if (d.cmdListsCount < 0 || d.cmdListsCount > ui_facts::kDrawDataMaxCmdLists) return false;
    const uintptr_t lists = reinterpret_cast<uintptr_t>(d.cmdLists);
    if (d.cmdListsCount > 0 && (lists == 0 || (lists & 0x7u) != 0)) return false;
    if (d.totalIdx < 0 || d.totalVtx < 0) return false;
    if (d.totalIdx < d.totalVtx) return false;
    return d.cmdListsCount == 0 ? d.totalVtx == 0 : d.totalVtx > 0;
}

// Every load here goes through a pointer the engine owns and we have not proved. The frame costs
// nothing on x64 when nothing faults (src/engine/session.cpp uses the same shape); the alternative
// -- a VirtualQuery per field per entry -- puts a syscall in the frame loop.
bool ReadGuiContext(const void* base, GuiContextRead* out) {
    __try {
        const unsigned char* const p = static_cast<const unsigned char*>(base);
        const void* const fonts = *reinterpret_cast<void* const volatile*>(
            p + ui_facts::kImguiIoOff + ui_facts::kImguiFontsIoOff);
        if (fonts == nullptr) return false;
        const unsigned char* const f = static_cast<const unsigned char*>(fonts);
        out->atlasA = *reinterpret_cast<const volatile uint32_t*>(f + ui_facts::kAtlasDwordAOff);
        out->atlasB = *reinterpret_cast<const volatile uint32_t*>(f + ui_facts::kAtlasDwordBOff);
        out->frameCount =
            *reinterpret_cast<const volatile int32_t*>(p + ui_facts::kImguiFrameCountOff);
        out->renderDrawListsFnPresent =
            *reinterpret_cast<void* const volatile*>(p + ui_facts::kImguiRenderDrawListsFnOff) !=
                    nullptr
                ? 1u
                : 0u;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// What the BEFORE half hands the AFTER half. The gate state travels with it so the entry samples
// the four gate globals ONCE and the scope decision is made on the same reading the row publishes.
struct GuiSample {
    GateState gate;
    bool balanceResolved;
    int32_t balanceBefore;
};

GuiSample BeginGuiSample() {
    GuiSample s{SampleGate(), false, 0};

    const int32_t* viewports = nullptr;
    const int32_t* balance = nullptr;
    const uint32_t* atlasA = nullptr;
    const uint32_t* atlasB = nullptr;
    const void* const* slot = nullptr;
    if (g.guiProbeSet) {
        viewports = g.probeViewports;
        balance = g.probeBeginBalance;
        atlasA = g.probeAtlasA;
        atlasB = g.probeAtlasB;
        slot = g.probeImguiSlot;
    } else {
        viewports =
            static_cast<const int32_t*>(buildguard::MappedAddress(ui_facts::kViewportsEnabledVA));
        balance = static_cast<const int32_t*>(buildguard::MappedAddress(ui_facts::kBeginBalanceVA));
        atlasA = static_cast<const uint32_t*>(buildguard::MappedAddress(ui_facts::kAtlasConstAVA));
        atlasB = static_cast<const uint32_t*>(buildguard::MappedAddress(ui_facts::kAtlasConstBVA));
        slot = static_cast<const void* const*>(
            buildguard::MappedAddress(ui_facts::kImguiContextSlotVA));
    }

    // The first address that did not resolve is NAMED, and nothing under it is published as a
    // number. An unresolved address read as 0 is the defect this whole row exists to refuse.
    const char* unresolved = nullptr;
    if (!s.gate.resolved) {
        unresolved =
            "GUI.SUPPRESS_BYTE / GUI.REENTRY_DEPTH / GUI.WINDOW_BUDGET / GUI.HOTKEY_ENABLE_BYTE";
    } else if (viewports == nullptr) {
        unresolved = "GUI.VIEWPORTS_ENABLED";
    } else if (balance == nullptr) {
        unresolved = "GUI.BEGIN_BALANCE";
    } else if (slot == nullptr) {
        unresolved = "GUI.IMGUI_CONTEXT";
    } else if (atlasA == nullptr) {
        unresolved = "GUI.ATLAS_CONST_A";
    } else if (atlasB == nullptr) {
        unresolved = "GUI.ATLAS_CONST_B";
    }
    g.guiUnresolvedId.store(unresolved, std::memory_order_relaxed);
    if (unresolved != nullptr) return s;

    g.guiSuppressed.store(s.gate.suppressed ? 1u : 0u, std::memory_order_relaxed);
    g.guiHotkeyEnable.store(s.gate.hotkeyEnable, std::memory_order_relaxed);
    g.guiReentryDepth.store(s.gate.reentry, std::memory_order_relaxed);
    g.guiWindowBudget.store(s.gate.budget, std::memory_order_relaxed);
    g.guiViewportsEnabled.store(*static_cast<const volatile int32_t*>(viewports),
                                std::memory_order_relaxed);

    s.balanceResolved = true;
    s.balanceBefore = *static_cast<const volatile int32_t*>(balance);

    // Term 3's expected pair is READ FROM .rdata HERE, never a literal in this file: a constant
    // typed in would turn the control into a comparison against our own copy of the answer.
    const uint32_t expectA = *static_cast<const volatile uint32_t*>(atlasA);
    const uint32_t expectB = *static_cast<const volatile uint32_t*>(atlasB);
    g.guiAtlasExpectA.store(expectA, std::memory_order_relaxed);
    g.guiAtlasExpectB.store(expectB, std::memory_order_relaxed);

    const void* const base = *static_cast<const void* const volatile*>(slot);
    GuiContextRead ctx = {0, 0, 0, 0};
    const char* reason = nullptr;
    if (base == nullptr) {
        reason =
            "term 1: the pointer at GUI.IMGUI_CONTEXT is null -- that address HOLDS the context "
            "pointer and is not the context";
    } else if (!ReadGuiContext(base, &ctx)) {
        reason =
            "term 2: io.Fonts at GUI.IMGUI_IO_OFF + GUI.IMGUI_FONTS_IO_OFF behind that pointer was "
            "null or did not read";
    } else if (ctx.atlasA != expectA || ctx.atlasB != expectB) {
        reason =
            "term 3: the dwords at io.Fonts+0x20/+0x24 are not GUI.ATLAS_CONST_A/B as read from "
            ".rdata -- the pointer leads to some other object";
    }
    // The OBSERVED pair is published whatever the outcome, because the row re-runs term 3 on it.
    g.guiAtlasDwordA.store(ctx.atlasA, std::memory_order_relaxed);
    g.guiAtlasDwordB.store(ctx.atlasB, std::memory_order_relaxed);
    g.guiBaseReason.store(reason, std::memory_order_relaxed);
    if (reason != nullptr) {
        // Cleared, not left standing. The four context-relative fields have no value behind an
        // unproven base, and a number kept from an entry where the base did prove out would let
        // that entry answer for this one. The row prints them as could-not-ask.
        g.guiBaseValid.store(0, std::memory_order_relaxed);
        g.guiFrameCountDelta.store(0, std::memory_order_relaxed);
        g.guiRenderDrawListsFnPresent.store(0, std::memory_order_relaxed);
        g.guiRenderedCmdLists.store(kGuiCountUnasked, std::memory_order_relaxed);
        g.guiRenderedTotalVtx.store(kGuiCountUnasked, std::memory_order_relaxed);
        g.guiHavePrevFrameCount = false;
        return s;
    }

    g.guiBaseValid.store(1, std::memory_order_relaxed);
    g.guiFrameCountDelta.store(
        g.guiHavePrevFrameCount ? ctx.frameCount - g.guiPrevFrameCount : 0,
        std::memory_order_relaxed);
    g.guiPrevFrameCount = ctx.frameCount;
    g.guiHavePrevFrameCount = true;
    g.guiRenderDrawListsFnPresent.store(ctx.renderDrawListsFnPresent, std::memory_order_relaxed);
    // The geometry is latched here too, not only at the owner tick. This site runs INSIDE the
    // engine's frame, so the ImDrawData it can see is the one EndFrame last filled -- and the
    // latch takes it only when it says Valid, so a struct NewFrame has already cleared is a
    // not-ready, never a zero.
    SampleGuiDrawData();
    return s;
}

void EndGuiSample(const GuiSample& s) {
    // BeginGuiSample already named the address that did not resolve. Half a pair is not a sample.
    if (!s.balanceResolved) return;
    const int32_t* const balance =
        g.guiProbeSet ? g.probeBeginBalance
                      : static_cast<const int32_t*>(
                            buildguard::MappedAddress(ui_facts::kBeginBalanceVA));
    if (balance == nullptr) {
        g.guiUnresolvedId.store("GUI.BEGIN_BALANCE (at the close of the pair)",
                                std::memory_order_relaxed);
        return;
    }
    const int32_t after = *static_cast<const volatile int32_t*>(balance);
    g.guiBeginBalanceDelta.store(after - s.balanceBefore, std::memory_order_relaxed);
    // Counted only once BOTH halves are in. This is the row's denominator, and a half-pair in it
    // would divide a delta by a sample that has no delta.
    g.guiGateSamples.fetch_add(1, std::memory_order_relaxed);
}

// One draw attempt, wherever it was issued from. Returns true when widgets actually reached the
// frame. Every exit either draws or names its refusal; there is no silent no-op arm.
bool Attempt() {
    const FramePrecondition p = Sample();
    if (!p.Ok()) {
        CountRefusal(p);
        return false;
    }
    const DrawFn draw = g.draw.load(std::memory_order_acquire);
    if (draw == nullptr) {
        // The frame was open and we had nothing to put in it. That is our fault, not the frame's,
        // and it is still a refused draw -- a build that forgot to wire the panel must not read as
        // a build whose panel was merely closed. Counted apart from a facade refusal since
        // L5-RUN-5 G2: the aggregate could not tell the two apart and the answer mattered.
        Bump(g.refusedNoDraw);
        Bump(g.refusedDraws);
        return false;
    }
    // ARCH-20 S5's read side, and the only thing it does to the engine is READ it. It takes the
    // BEFORE half of the sample pair here and the AFTER half on every path that draws, on both
    // sides of the ui.gate_scope flip -- S6.3(a) subtracts one run's renderedTotalVtxCount from
    // the other's, and a field only one run published is half a comparison.
    const GuiSample sample = BeginGuiSample();

    // ** WITH ui.gate_scope OFF, NO ENGINE SETTER IS CALLED AND NO BYTE IS WRITTEN. No scope is
    // entered, none of the gate counters moves, and the build behaves as it did before this
    // packet: the default-false is a real safety position and not a label.
    if (!GateScopeEnabled()) {
        const DrawOutcome outcome = TimedDraw(draw);
        EndGuiSample(sample);
        return Finish(outcome);
    }

    const GateState gate = sample.gate;
    if (!gate.resolved) {
        // ** THE FLIP CHANGED WHAT THIS ARM COSTS, SO IT CHANGED WHAT IT DOES.
        // The gate could not be ASKED, so NO SCOPE is entered -- a scope whose close we could not
        // run would leave the engine's byte cleared, and that is still forbidden. But refusing the
        // DRAW as well was affordable only while the row was opt-in: with it default-true, any
        // build whose four gate addresses do not map -- a test host, a launch where buildguard
        // refused a site -- would lose the panel entirely, which is the one thing this feature
        // exists to deliver. Draw with no scope: byte for byte the behaviour ui.gate_scope=false
        // has always had, no engine setter called, and the refusal is COUNTED rather than silent.
        Bump(g.gateUnresolved);
        const DrawOutcome outcome = TimedDraw(draw);
        EndGuiSample(sample);
        return Finish(outcome);
    }
    if (!gate.suppressed) {
        // The byte is already 0: the engine's own hotkey toggle can do this between frames, so
        // this arm is ordinary rather than exotic. Draw with NO scope -- entering one would END by
        // suppressing a GUI that was on, which is a change we were never asked to make.
        Bump(g.gateAlreadyOpen);
        const DrawOutcome outcome = TimedDraw(draw);
        EndGuiSample(sample);
        return Finish(outcome);
    }

    // The scope is closed BEFORE the counters are read, so the engine's byte is back where we
    // found it at the earliest moment it can be, not at the end of our bookkeeping.
    DrawOutcome outcome = DrawOutcome::NotShowing;
    {
        GateScope scope(gate);
        outcome = TimedDraw(draw);
    }
    // After the scope closed, so the BEGIN_BALANCE delta spans exactly the window our draw ran in.
    EndGuiSample(sample);
    return Finish(outcome);
}

bool Finish(DrawOutcome outcome) {
    switch (outcome) {
        case DrawOutcome::Drawn:
            Bump(g.inFrameDraws);
            return true;
        case DrawOutcome::FacadeRefused:
            Bump(g.refusedBeginFalse);
            Bump(g.refusedDraws);
            return false;
        case DrawOutcome::NotShowing:
        default:
            Bump(g.panelClosedEntries);
            return false;
    }
}

// The row-2.5 site. The trampoline runs FIRST and the draw goes on the way out, so our window is
// created inside the frame this very call opened and no engine work is displaced.
//
// THE OWNER THREAD IS CHECKED HERE, NOT ASSUMED. ARCH-15 S2 establishes that the only
// way into 0x1415483B0 is UpdateEngine(0) -- no direct call xref anywhere in .text, one vtable slot
// -- and that is the thread our stage-0 tick runs on. It is still an assumption about somebody
// else's engine, and ImGui is not thread-safe, so an entry from any other thread FORWARDS THE
// ENGINE'S OWN CALL (never ours to withhold) and draws nothing. Those entries are counted under
// their own name instead of being folded into frameSiteEntries: that counter is leg 3's
// denominator, and an entry never offered a draw must not sit in a coverage ratio's denominator.
uint64_t __fastcall HeartbeatBeforeDetour(void* engine) {
    const HeartbeatFn original = g.original.load(std::memory_order_acquire);
    const uint64_t result = original != nullptr ? original(engine) : 0;

    uint32_t owner = 0;
    const uint32_t self = ::GetCurrentThreadId();
    if (!g.ownerThreadId.compare_exchange_strong(owner, self, std::memory_order_relaxed,
                                                 std::memory_order_relaxed) &&
        owner != self) {
        Bump(g.offThreadSiteEntries);
        return result;
    }

    Bump(g.frameSiteEntries);
    Attempt();
    return result;
}

}  // namespace

FramePrecondition Sample() {
    FramePrecondition p{false, false, false};

    if (g.probeSet) {
        p.frameOpenByte = g.probeFrameOpen != nullptr && *g.probeFrameOpen != 0;
        p.startFrameLatch = g.probeLatch != nullptr && *g.probeLatch != 0;
        p.contextPtr = g.probeContext != nullptr && *g.probeContext != nullptr;
        return p;
    }

    const void* const frameByte = buildguard::MappedAddress(facts::GUI_FRAME_OPEN_BYTE);
    const void* const latch = buildguard::MappedAddress(facts::GUI_STARTFRAME_LATCH);
    const void* const context = buildguard::MappedAddress(facts::GUI_CONTEXT_PTR);

    // An address that did not resolve reads as false on its own term. Missing data must not become
    // a confident "the frame is open".
    if (frameByte != nullptr) {
        p.frameOpenByte = *static_cast<const volatile uint8_t*>(frameByte) != 0;
    }
    if (latch != nullptr) {
        p.startFrameLatch = *static_cast<const volatile uint8_t*>(latch) != 0;
    }
    if (context != nullptr) {
        p.contextPtr = *static_cast<const volatile uint64_t*>(context) != 0;
    }
    return p;
}

GateState SampleGate() {
    GateState s{false, false, 0, 0, 0};

    const uint8_t* suppress = nullptr;
    const int32_t* reentry = nullptr;
    const int32_t* budget = nullptr;
    const uint8_t* hotkey = nullptr;
    if (g.gateProbeSet) {
        suppress = g.probeSuppress;
        reentry = g.probeReentry;
        budget = g.probeBudget;
        hotkey = g.probeHotkey;
    } else {
        suppress = static_cast<const uint8_t*>(buildguard::MappedAddress(ui_facts::kSuppressByteVA));
        reentry = static_cast<const int32_t*>(buildguard::MappedAddress(ui_facts::kReentryDepthVA));
        budget = static_cast<const int32_t*>(buildguard::MappedAddress(ui_facts::kWindowBudgetVA));
        hotkey =
            static_cast<const uint8_t*>(buildguard::MappedAddress(ui_facts::kHotkeyEnableByteVA));
    }
    // All four or none. A gate sampled from three of its four addresses is a gate whose restore
    // argument we would be guessing, and the guess is the operator's hotkey.
    if (suppress == nullptr || reentry == nullptr || budget == nullptr || hotkey == nullptr) {
        return s;
    }

    s.resolved = true;
    s.suppressed = *static_cast<const volatile uint8_t*>(suppress) != 0;
    s.reentry = *static_cast<const volatile int32_t*>(reentry);
    s.budget = *static_cast<const volatile int32_t*>(budget);
    s.hotkeyEnable = *static_cast<const volatile uint8_t*>(hotkey);
    return s;
}

// ★ WHERE THE PIXEL COUNT ACTUALLY COMES FROM (ARCH-20 S6.3(a), rebuilt after L5-RUN-7).
//
// The engine clears ImDrawData at NewFrame and fills it at EndFrame, so the struct is only
// readable as a measurement in the window between one frame's EndFrame and the next frame's
// NewFrame. A reader that samples anywhere else sees a cleared struct and, if it also ignores the
// `Valid` flag, publishes a confident 0. This is called from both places our code runs -- the
// owner tick, which precedes the frame, and the in-frame draw site -- and LATCHES the last
// reading that said Valid and passed DrawDataLooksSane. Whichever of the two lands in the window
// answers; if neither ever does, the two counts stay at could-not-ask and say so.
void SampleGuiDrawData() {
    // The base's three terms were proved by BeginGuiSample against the atlas pair. This reader
    // does not re-prove them and must not run without them: an unproved pointer plus a struct
    // check is a weaker control than the one ARCH-20 S5.1 already provides.
    if (g.guiBaseValid.load(std::memory_order_relaxed) != 1u) return;

    const void* const* slot =
        g.guiProbeSet ? g.probeImguiSlot
                      : static_cast<const void* const*>(
                            buildguard::MappedAddress(ui_facts::kImguiContextSlotVA));
    if (slot == nullptr) return;
    const void* const base = *static_cast<const void* const volatile*>(slot);
    if (base == nullptr) return;

    DrawDataRead d = {0, 0, 0, 0, 0};
    if (!ReadDrawData(base, &d) || !DrawDataLooksSane(d)) {
        Bump(g.guiDrawDataRefused);
        return;
    }
    if (d.valid == 0) {
        // Between NewFrame and EndFrame. Not a zero-vertex frame -- a frame whose geometry has not
        // been assembled yet, and the previous latched reading stands.
        Bump(g.guiDrawDataNotReady);
        return;
    }
    g.guiRenderedCmdLists.store(d.cmdListsCount, std::memory_order_relaxed);
    g.guiRenderedTotalVtx.store(d.totalVtx, std::memory_order_relaxed);
    Bump(g.guiDrawDataLatches);
}

bool GateScopeEnabled() {
    const int forced = g.gateScopeOverride.load(std::memory_order_relaxed);
    if (forced >= 0) return forced != 0;

    // ONE source of truth, and the ledger reads the same one. The environment override that used
    // to be parsed here is resolved by config against kConfigEnvOverride, so the launch record
    // cannot print `ui.gate_scope disabled` about a run whose scope was armed.
    // The default handed in must match config_keys.inc's, and config warns if it drifts.
    int cached = g.gateScopeCached.load(std::memory_order_acquire);
    if (cached < 0) {
        cached = config::Interface()->GetBool(kGateScopeKey, kGateScopeDefault) ? 1 : 0;
        g.gateScopeCached.store(cached, std::memory_order_release);
    }
    return cached != 0;
}

GateScope::GateScope(const GateState& gate) : opened_(false), hotkeyEnable_(gate.hotkeyEnable) {
    if (!gate.resolved || !gate.suppressed) return;
    if (!gui::ClearSuppressed(gate.hotkeyEnable)) {
        // Refused by name. The opener did not bind or did not pass its guard, so no scope exists
        // and the closer will never run -- there is no state to be left half-changed.
        Bump(g.suppressSetterUnmapped);
        log::Direct(LogLevel::Warn, kModuleName,
                    "GUI.CLEAR_SUPPRESSED did not bind: the suppression gate stays shut and the "
                    "panel draws exactly as it does with ui.gate_scope off");
        return;
    }
    opened_ = true;
    Bump(g.suppressScopeEntries);
}

GateScope::~GateScope() {
    if (!opened_) return;

    // Compare before restore. We left the byte at 0; anything else means somebody outside this
    // scope wrote it, and that gets a name rather than a silent overwrite.
    const GateState now = SampleGate();
    if (!now.resolved || now.suppressed) {
        Bump(g.suppressScopeChanged);
        log::Direct(LogLevel::Warn, kModuleName,
                    "GUI.SUPPRESS_BYTE was not where this scope left it at close (resolved=%d "
                    "suppressed=%d); restoring the value observed at open anyway",
                    now.resolved ? 1 : 0, now.suppressed ? 1 : 0);
    }
    if (!gui::SetSuppressed()) {
        // The opener bound and the closer did not. Nothing can restore the byte from here, so it
        // is said out loud: this is the one state this design must never reach quietly.
        Bump(g.suppressSetterUnmapped);
        log::Direct(LogLevel::Error, kModuleName,
                    "GUI.SET_SUPPRESSED did not bind after GUI.CLEAR_SUPPRESSED ran: the engine's "
                    "GUI suppression byte is LEFT CLEARED for the rest of this process");
        return;
    }
    Bump(g.suppressScopeRestores);
}

const char* PanelHoverSourceName() {
    // The facade still binds no hover query. The BAR does not need one: it reads the Win32 cursor
    // itself (that is what its drag is built on) and compares it with the rect it placed the window
    // at, so once it has sampled once, a zero above this word is a measurement -- the cursor was
    // not over the window -- and not an unasked question. LAUNCH-UI3 §3 was misled by the old
    // unconditional "none" while every click it made worked.
    return g.hoverAsked.load(std::memory_order_relaxed) ? "win32cursor" : "none";
}

void NoteCursorOverPanel(bool over) {
    g.hoverAsked.store(true, std::memory_order_relaxed);
    if (over) Bump(g.panelHoveredFrames);
}

const char* BeginRefusalName(BeginRefusal r) {
    switch (r) {
        case BeginRefusal::BeginUnbound: return "beginunbound";
        case BeginRefusal::NoPlacement:  return "noplacement";
        case BeginRefusal::BoundNoFrame: return "boundnoframe";
        case BeginRefusal::None:         return "none";
    }
    // An enumerator nobody has named yet must not read as "none" -- that is a verdict it has not
    // earned. It gets its own word, and the reader can see the vocabulary is behind the enum.
    return "unnamed";
}

// Called from inside the draw, before TimedDraw has counted it, so timedDraws == 0 identifies the
// first draw -- the one that builds the font atlas and is excluded from every maximum here for the
// same reason firstDrawNanos is excluded from drawNanosMax.
void NoteImguiNanos(uint64_t nanos) {
    if (g.timedDraws.load(std::memory_order_relaxed) == 0) return;
    RaiseMax(g.imguiNanosMax, nanos);
}

void NoteGuiStream(uint32_t items, uint32_t glyphs) {
    // Every frame, the first one included: the first draw is excluded from the TIMES because it
    // builds the font atlas, and a font atlas is not a widget. The stream it issues is the stream
    // every later frame issues, and excluding it would leave a launch that drew once with no
    // stream at all.
    RaiseMax(g.guiItemsMax, items);
    RaiseMax(g.guiGlyphsMax, glyphs);
    g.guiStreamFrames.fetch_add(1, std::memory_order_relaxed);
}

void NoteTransportGuiStream(uint32_t items, uint32_t glyphs) {
    RaiseMax(g.transportItemsMax, items);
    RaiseMax(g.transportGlyphsMax, glyphs);
    g.transportStreamFrames.fetch_add(1, std::memory_order_relaxed);
}

void NoteSnapshotPhases(uint64_t scanNanos, uint64_t metadataNanos, uint64_t formatNanos) {
    RaiseMax(g.browserScanNanosMax, scanNanos);
    RaiseMax(g.metadataReadNanosMax, metadataNanos);
    RaiseMax(g.textFormatNanosMax, formatNanos);
}

void NoteBeginRefusal(BeginRefusal reason, uint32_t boundEntries, uint32_t entryCount) {
    g.beginRefusal.store(static_cast<uint8_t>(reason), std::memory_order_relaxed);
    g.facadeBoundEntries.store(boundEntries, std::memory_order_relaxed);
    g.facadeEntryCount.store(entryCount, std::memory_order_relaxed);
}

const Counters& Read() {
    static thread_local Counters c;
    c.frameSiteEntries = g.frameSiteEntries.load(std::memory_order_relaxed);
    c.inFrameDraws = g.inFrameDraws.load(std::memory_order_relaxed);
    c.refusedDraws = g.refusedDraws.load(std::memory_order_relaxed);
    c.refusedNoFrameByte = g.refusedNoFrameByte.load(std::memory_order_relaxed);
    c.refusedNoLatch = g.refusedNoLatch.load(std::memory_order_relaxed);
    c.refusedNoContext = g.refusedNoContext.load(std::memory_order_relaxed);
    c.refusedNoDraw = g.refusedNoDraw.load(std::memory_order_relaxed);
    c.refusedBeginFalse = g.refusedBeginFalse.load(std::memory_order_relaxed);
    // Derived here and stored nowhere, so the published aggregate and its breakdown cannot drift.
    c.refusedByFacade = c.refusedNoDraw + c.refusedBeginFalse;
    c.beginRefusal = static_cast<BeginRefusal>(g.beginRefusal.load(std::memory_order_relaxed));
    c.facadeBoundEntries = g.facadeBoundEntries.load(std::memory_order_relaxed);
    c.facadeEntryCount = g.facadeEntryCount.load(std::memory_order_relaxed);
    c.panelClosedEntries = g.panelClosedEntries.load(std::memory_order_relaxed);
    c.fallbackEntries = g.fallbackEntries.load(std::memory_order_relaxed);
    c.offThreadSiteEntries = g.offThreadSiteEntries.load(std::memory_order_relaxed);
    c.frameOpenAtOwnerTickSamples = g.frameOpenSamples.load(std::memory_order_relaxed);
    c.frameOpenAtOwnerTickNonZero = g.frameOpenNonZero.load(std::memory_order_relaxed);
    c.frameOpenAtOwnerTickLastNonZeroTick =
        g.frameOpenLastNonZeroTick.load(std::memory_order_relaxed);
    c.gateUnresolved = g.gateUnresolved.load(std::memory_order_relaxed);
    c.gateAlreadyOpen = g.gateAlreadyOpen.load(std::memory_order_relaxed);
    c.suppressScopeEntries = g.suppressScopeEntries.load(std::memory_order_relaxed);
    c.suppressScopeRestores = g.suppressScopeRestores.load(std::memory_order_relaxed);
    c.suppressScopeChanged = g.suppressScopeChanged.load(std::memory_order_relaxed);
    c.suppressSetterUnmapped = g.suppressSetterUnmapped.load(std::memory_order_relaxed);
    c.panelHoveredFrames = g.panelHoveredFrames.load(std::memory_order_relaxed);
    c.drawNanosTotal = g.drawNanosTotal.load(std::memory_order_relaxed);
    c.drawNanosMax = g.drawNanosMax.load(std::memory_order_relaxed);
    c.firstDrawNanos = g.firstDrawNanos.load(std::memory_order_relaxed);
    c.drawNanosSamples = g.drawNanosSamples.load(std::memory_order_relaxed);
    c.drawNanosOverHistogram = g.drawNanosOverHistogram.load(std::memory_order_relaxed);
    // The percentile is the UPPER EDGE of the first bucket whose cumulative count reaches the rank
    // asked for -- one pass over 512 counters, here, never in a frame. Reporting the upper edge is
    // what makes it safe to compare against a ceiling: it never understates.
    const auto drawPercentile = [](uint64_t perMille) -> uint64_t {
        if (c.drawNanosSamples == 0) return 0;
        // Per-mille, spelled as the ratio it is: a literal 1000 here is also the value of
        // GAME.VTBL_SETFIXEDTIMESTEP_OFF, and a number that collides with a fact is a number the
        // next reader has to disambiguate.
        constexpr uint64_t kWhole = std::milli::den;
        const uint64_t rank = (c.drawNanosSamples * perMille + kWhole - 1) / kWhole;
        uint64_t seen = 0;
        for (size_t i = 0; i < kDrawNanosBuckets; ++i) {
            seen += g.drawNanosHist[i].load(std::memory_order_relaxed);
            if (seen >= rank) return (static_cast<uint64_t>(i) + 1) * kDrawNanosBucketNs;
        }
        return kDrawNanosHistogramTopNs;
    };
    c.drawNanosP99 = drawPercentile(990);
    c.drawNanosP999 = drawPercentile(999);
    c.guiItemsPerFrame = g.guiItemsMax.load(std::memory_order_relaxed);
    c.guiGlyphsPerFrame = g.guiGlyphsMax.load(std::memory_order_relaxed);
    c.guiStreamFrames = g.guiStreamFrames.load(std::memory_order_relaxed);
    c.transportItemsPerFrame = g.transportItemsMax.load(std::memory_order_relaxed);
    c.transportGlyphsPerFrame = g.transportGlyphsMax.load(std::memory_order_relaxed);
    c.transportStreamFrames = g.transportStreamFrames.load(std::memory_order_relaxed);
    c.imguiNanos = g.imguiNanosMax.load(std::memory_order_relaxed);
    c.browserScanNanos = g.browserScanNanosMax.load(std::memory_order_relaxed);
    c.metadataReadNanos = g.metadataReadNanosMax.load(std::memory_order_relaxed);
    c.textFormatNanos = g.textFormatNanosMax.load(std::memory_order_relaxed);
    c.browserIoOnDrawThread = fileio::OnDrawThread();
    c.guiDrawDataLatches = g.guiDrawDataLatches.load(std::memory_order_relaxed);
    c.guiDrawDataRefused = g.guiDrawDataRefused.load(std::memory_order_relaxed);
    c.guiDrawDataNotReady = g.guiDrawDataNotReady.load(std::memory_order_relaxed);

    c.guiGateSamples = g.guiGateSamples.load(std::memory_order_relaxed);
    // Constant true HERE, and that is exactly what it says: the ARCH-20 S5 read side is compiled
    // into this build. It is false only on a record written by a build without it, which is the
    // one thing a reader cannot tell from a zero -- `guiGateSamples == 0` is the separate arm for
    // a site that was never entered.
    c.guiGateSampled = true;
    const char* const unresolved = g.guiUnresolvedId.load(std::memory_order_relaxed);
    c.guiGateAddrResolved = unresolved == nullptr;
    CopyFixed(c.guiGateUnresolved, sizeof c.guiGateUnresolved, unresolved);
    CopyFixed(c.guiImguiBaseReason, sizeof c.guiImguiBaseReason,
              g.guiBaseReason.load(std::memory_order_relaxed));
    c.guiSuppressed = g.guiSuppressed.load(std::memory_order_relaxed);
    c.guiHotkeyEnable = g.guiHotkeyEnable.load(std::memory_order_relaxed);
    c.guiReentryDepth = g.guiReentryDepth.load(std::memory_order_relaxed);
    c.guiWindowBudget = g.guiWindowBudget.load(std::memory_order_relaxed);
    c.guiViewportsEnabled = g.guiViewportsEnabled.load(std::memory_order_relaxed);
    c.guiBeginBalanceDelta = g.guiBeginBalanceDelta.load(std::memory_order_relaxed);
    c.guiImguiBaseValid = g.guiBaseValid.load(std::memory_order_relaxed) != 0;
    c.guiAtlasDwordA = g.guiAtlasDwordA.load(std::memory_order_relaxed);
    c.guiAtlasDwordB = g.guiAtlasDwordB.load(std::memory_order_relaxed);
    c.guiAtlasExpectA = g.guiAtlasExpectA.load(std::memory_order_relaxed);
    c.guiAtlasExpectB = g.guiAtlasExpectB.load(std::memory_order_relaxed);
    c.guiImguiFrameCountDelta = g.guiFrameCountDelta.load(std::memory_order_relaxed);
    c.guiRenderDrawListsFnPresent = g.guiRenderDrawListsFnPresent.load(std::memory_order_relaxed);
    c.guiRenderedCmdListsCount = g.guiRenderedCmdLists.load(std::memory_order_relaxed);
    c.guiRenderedTotalVtxCount = g.guiRenderedTotalVtx.load(std::memory_order_relaxed);
    return c;
}

bool Installed() { return g.installed.load(std::memory_order_acquire); }

HookStatus InstallStatus() { return g.installStatus.load(std::memory_order_relaxed); }

void SampleFrameOpenAtOwnerTick(uint64_t tickIndex) {
    // Deliberately NOT Sample(): the latch and the context say whether a draw would be accepted,
    // and the question here is only whether the ENGINE'S frame byte is set at stage 0. Same
    // address, same probe seam, one load.
    const uint8_t* byte = nullptr;
    if (g.probeSet) {
        byte = g.probeFrameOpen;
    } else {
        byte = static_cast<const uint8_t*>(buildguard::MappedAddress(facts::GUI_FRAME_OPEN_BYTE));
    }
    // Could not ask. Not a sample of 0 -- samples below the owner-tick count is the reader's cue.
    if (byte == nullptr) return;

    const bool open = *static_cast<const volatile uint8_t*>(byte) != 0;
    g.frameOpenSamples.fetch_add(1, std::memory_order_relaxed);
    if (open) {
        g.frameOpenNonZero.fetch_add(1, std::memory_order_relaxed);
        g.frameOpenLastNonZeroTick.store(tickIndex, std::memory_order_relaxed);
    }
}

void NoteOwnerThread(uint32_t threadId) {
    // The stamp WINS over the latch: csregister's owner tick is the authority on which thread the
    // draw is allowed on, and a latch taken before the first tick is only a stand-in for it.
    if (threadId != 0) g.ownerThreadId.store(threadId, std::memory_order_relaxed);
}

void SetDrawFn(DrawFn fn) { g.draw.store(fn, std::memory_order_release); }

namespace {

// Shared by Install() and InstallForTest() so the production row and the tests go through one
// ladder. A reimplemented install in the test would be free to get the guard right while the
// product had it wrong.
HookStatus CreateAndArm(const char* factId, uint64_t pcVa, const uint8_t* prologue,
                        size_t prologueLen, const char* configKey) {
    void* trampoline = nullptr;
    const HookStatus created =
        hookmgr::Create(factId, pcVa, prologue, prologueLen,
                        reinterpret_cast<void*>(&HeartbeatBeforeDetour), &trampoline, configKey);
    if (created != HookStatus::Created) {
        g.installStatus.store(created, std::memory_order_relaxed);
        g.installed.store(false, std::memory_order_release);
        log::Direct(LogLevel::Warn, kModuleName,
                    "in-frame draw site %s not created: %s -- the panel will fall back to the owner "
                    "tick, where the GUI frame is shut and every draw is refused",
                    factId, HookStatusName(created));
        return created;
    }
    g.original.store(reinterpret_cast<HeartbeatFn>(trampoline), std::memory_order_release);

    const HookStatus armed = hookmgr::Arm(factId);
    g.installStatus.store(armed, std::memory_order_relaxed);
    g.installed.store(armed == HookStatus::Armed, std::memory_order_release);
    if (armed != HookStatus::Armed) {
        log::Direct(LogLevel::Warn, kModuleName, "in-frame draw site %s not armed: %s", factId,
                    HookStatusName(armed));
    }
    return armed;
}

}  // namespace

bool Install() {
    return CreateAndArm(kFactId, facts::GUI_ENGINE_HEARTBEAT_BEFORE,
                        facts::GUI_ENGINE_HEARTBEAT_BEFORE_PROLOGUE,
                        sizeof facts::GUI_ENGINE_HEARTBEAT_BEFORE_PROLOGUE,
                        kConfigKey) == HookStatus::Armed;
}

bool AttemptFallbackDraw() {
    // The fallback exists for the build where the detour did not arm. With it armed, a second draw
    // in the same frame would emit the window twice and unbalance nothing but the reader's trust in
    // "once per frame".
    if (Installed()) return false;
    Bump(g.fallbackEntries);
    return Attempt();
}

void ResetForTest() {
    g.frameSiteEntries.store(0, std::memory_order_relaxed);
    g.inFrameDraws.store(0, std::memory_order_relaxed);
    g.refusedDraws.store(0, std::memory_order_relaxed);
    g.refusedNoFrameByte.store(0, std::memory_order_relaxed);
    g.refusedNoLatch.store(0, std::memory_order_relaxed);
    g.refusedNoContext.store(0, std::memory_order_relaxed);
    g.refusedNoDraw.store(0, std::memory_order_relaxed);
    g.refusedBeginFalse.store(0, std::memory_order_relaxed);
    g.beginRefusal.store(static_cast<uint8_t>(BeginRefusal::None), std::memory_order_relaxed);
    g.facadeBoundEntries.store(0, std::memory_order_relaxed);
    g.facadeEntryCount.store(0, std::memory_order_relaxed);
    g.panelClosedEntries.store(0, std::memory_order_relaxed);
    g.fallbackEntries.store(0, std::memory_order_relaxed);
    g.offThreadSiteEntries.store(0, std::memory_order_relaxed);
    g.frameOpenSamples.store(0, std::memory_order_relaxed);
    g.frameOpenNonZero.store(0, std::memory_order_relaxed);
    g.frameOpenLastNonZeroTick.store(0, std::memory_order_relaxed);
    g.draw.store(nullptr, std::memory_order_relaxed);
    g.installed.store(false, std::memory_order_relaxed);
    g.installStatus.store(HookStatus::Absent, std::memory_order_relaxed);
    g.original.store(nullptr, std::memory_order_relaxed);
    g.ownerThreadId.store(0, std::memory_order_relaxed);
    g.probeFrameOpen = nullptr;
    g.probeLatch = nullptr;
    g.probeContext = nullptr;
    g.probeSet = false;
    g.gateUnresolved.store(0, std::memory_order_relaxed);
    g.gateAlreadyOpen.store(0, std::memory_order_relaxed);
    g.suppressScopeEntries.store(0, std::memory_order_relaxed);
    g.suppressScopeRestores.store(0, std::memory_order_relaxed);
    g.suppressScopeChanged.store(0, std::memory_order_relaxed);
    g.suppressSetterUnmapped.store(0, std::memory_order_relaxed);
    g.panelHoveredFrames.store(0, std::memory_order_relaxed);
    g.hoverAsked.store(false, std::memory_order_relaxed);
    g.drawNanosTotal.store(0, std::memory_order_relaxed);
    g.drawNanosMax.store(0, std::memory_order_relaxed);
    g.firstDrawNanos.store(0, std::memory_order_relaxed);
    g.imguiNanosMax.store(0, std::memory_order_relaxed);
    g.browserScanNanosMax.store(0, std::memory_order_relaxed);
    g.metadataReadNanosMax.store(0, std::memory_order_relaxed);
    g.textFormatNanosMax.store(0, std::memory_order_relaxed);
    g.timedDraws.store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < kDrawNanosBuckets; ++i) {
        g.drawNanosHist[i].store(0, std::memory_order_relaxed);
    }
    g.drawNanosSamples.store(0, std::memory_order_relaxed);
    g.drawNanosOverHistogram.store(0, std::memory_order_relaxed);
    g.guiItemsMax.store(0, std::memory_order_relaxed);
    g.guiGlyphsMax.store(0, std::memory_order_relaxed);
    g.guiStreamFrames.store(0, std::memory_order_relaxed);
    g.transportItemsMax.store(0, std::memory_order_relaxed);
    g.transportGlyphsMax.store(0, std::memory_order_relaxed);
    g.transportStreamFrames.store(0, std::memory_order_relaxed);
    g.gateScopeCached.store(-1, std::memory_order_relaxed);
    g.gateScopeOverride.store(-1, std::memory_order_relaxed);
    g.probeSuppress = nullptr;
    g.probeReentry = nullptr;
    g.probeBudget = nullptr;
    g.probeHotkey = nullptr;
    g.gateProbeSet = false;
    g.guiGateSamples.store(0, std::memory_order_relaxed);
    g.guiUnresolvedId.store(nullptr, std::memory_order_relaxed);
    g.guiBaseReason.store(nullptr, std::memory_order_relaxed);
    g.guiSuppressed.store(0, std::memory_order_relaxed);
    g.guiHotkeyEnable.store(0, std::memory_order_relaxed);
    g.guiReentryDepth.store(0, std::memory_order_relaxed);
    g.guiWindowBudget.store(0, std::memory_order_relaxed);
    g.guiViewportsEnabled.store(0, std::memory_order_relaxed);
    g.guiBeginBalanceDelta.store(0, std::memory_order_relaxed);
    g.guiBaseValid.store(0, std::memory_order_relaxed);
    g.guiAtlasDwordA.store(0, std::memory_order_relaxed);
    g.guiAtlasDwordB.store(0, std::memory_order_relaxed);
    g.guiAtlasExpectA.store(0, std::memory_order_relaxed);
    g.guiAtlasExpectB.store(0, std::memory_order_relaxed);
    g.guiFrameCountDelta.store(0, std::memory_order_relaxed);
    g.guiRenderDrawListsFnPresent.store(0, std::memory_order_relaxed);
    g.guiRenderedCmdLists.store(kGuiCountUnasked, std::memory_order_relaxed);
    g.guiRenderedTotalVtx.store(kGuiCountUnasked, std::memory_order_relaxed);
    g.guiDrawDataLatches.store(0, std::memory_order_relaxed);
    g.guiDrawDataRefused.store(0, std::memory_order_relaxed);
    g.guiDrawDataNotReady.store(0, std::memory_order_relaxed);
    g.guiPrevFrameCount = 0;
    g.guiHavePrevFrameCount = false;
    g.probeImguiSlot = nullptr;
    g.probeAtlasA = nullptr;
    g.probeAtlasB = nullptr;
    g.probeViewports = nullptr;
    g.probeBeginBalance = nullptr;
    g.guiProbeSet = false;
}

void SetGuiProbeForTest(const void* const* imguiContextSlot, const uint32_t* atlasConstA,
                        const uint32_t* atlasConstB, const int32_t* viewportsEnabled,
                        const int32_t* beginBalance) {
    g.probeImguiSlot = imguiContextSlot;
    g.probeAtlasA = atlasConstA;
    g.probeAtlasB = atlasConstB;
    g.probeViewports = viewportsEnabled;
    g.probeBeginBalance = beginBalance;
    g.guiProbeSet = imguiContextSlot != nullptr || atlasConstA != nullptr ||
                    atlasConstB != nullptr || viewportsEnabled != nullptr ||
                    beginBalance != nullptr;
}

void SetGateProbeForTest(const uint8_t* suppressByte, const int32_t* reentry, const int32_t* budget,
                         const uint8_t* hotkeyEnable) {
    g.probeSuppress = suppressByte;
    g.probeReentry = reentry;
    g.probeBudget = budget;
    g.probeHotkey = hotkeyEnable;
    g.gateProbeSet = suppressByte != nullptr || reentry != nullptr || budget != nullptr ||
                     hotkeyEnable != nullptr;
}

void SetGateScopeForTest(int onOffOrMinusOne) {
    g.gateScopeOverride.store(onOffOrMinusOne, std::memory_order_relaxed);
}

void SetProbeForTest(const uint8_t* frameOpenByte, const uint8_t* startFrameLatch,
                     const void* const* contextPtr) {
    g.probeFrameOpen = frameOpenByte;
    g.probeLatch = startFrameLatch;
    g.probeContext = contextPtr;
    g.probeSet = frameOpenByte != nullptr || startFrameLatch != nullptr || contextPtr != nullptr;
}

HookStatus InstallForTest(const char* factId, uint64_t pcVa, const uint8_t* prologue,
                          size_t prologueLen, const char* configKey) {
    return CreateAndArm(factId, pcVa, prologue, prologueLen, configKey);
}

void CallSiteForTest(OriginalFn original, void* engine) {
    HeartbeatFn const saved = g.original.load(std::memory_order_relaxed);
    g.original.store(reinterpret_cast<HeartbeatFn>(original), std::memory_order_release);
    HeartbeatBeforeDetour(engine);
    g.original.store(saved, std::memory_order_release);
}

void SetInstalledForTest(bool installed) {
    g.installed.store(installed, std::memory_order_release);
}

}  // namespace inframe
}  // namespace ui
}  // namespace replay
