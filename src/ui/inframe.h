#pragma once

// W2-F. The row-2.5 draw site: our panel, drawn INSIDE the engine's own GUI frame.
//
// ---------------------------------------------------------------------------------------------
// * WHAT THIS FIXES, and it is not a cosmetic problem.
//
// Until this file existed the panel was drawn from our stage-0 owner tick, and ARCH-15 S1.4 proves
// every one of those draws was REFUSED: `NRadGui::Begin` opens with `cmp cs:byte_1420DAF2F, 0 / jz`
// and returns having produced no state, as do 38 further facade entry points. So `ownerDraws` --
// leg 3 of `Running` and the numerator of DL-07's coverage half -- counted refusals, and was 100%
// satisfiable on a build that drew nothing, forever. That is a gate that cannot go red on the
// failure it was written for.
//
// The site here is `GUI.ENGINE_HEARTBEAT_BEFORE` (`0x1415483B0`), detoured through `hookmgr` and
// drawn on the way OUT. ARCH-15 S2 scores it against three rivals: it is the only candidate whose
// "inside the frame", "owner thread" and "once per frame" columns are all VERIFIED rather than
// inferred, the only one that survives the bit-6-SET tail, and it is independent of band-3
// registration and of bit 23 of `game+0x828`. Its 6 stolen bytes (`push rbx` + `sub rsp,0x20` -- MinHook copies
// whole instructions until it has 5) contain no RIP-relative operand
// and no branch, so the trampoline is a straight copy.
//
// * THE PRECONDITION IS THREE LOADS, NOT ONE (ARCH-15 S2.1). `StartFrame`'s GUI.WINDOW_GATE
// early-out jumps PAST the frame-byte write without clearing it, so a stale 1 from a frame whose
// `EndFrame` never ran is reachable and drawing on it appends widgets to a frame ImGui never began.
// The latch is the discriminator: every path reaching the byte=1 write leaves it non-zero, every
// path skipping that write leaves it zero. The context pointer is what `Begin` itself tests first.
//
// * A DRAW ATTEMPTED WITH THE BYTE 0 IS A NAMED REFUSAL, NEVER A SILENT NO-OP (ARCH-15 S4.5).
// `refusedDraws` is that name and it is published even when it is 0, so that its ABSENCE from a
// record means "not asked" and its zero means "asked, none". Its three-way breakdown exists so the
// reason is read rather than guessed.
//
// * AND `Begin`'s ANSWER IS PART OF THE PRECONDITION (AUDIT_F3J F3J-4). The three loads say the
// frame should accept us; `Begin` returning false says it did not. A draw whose `Begin` returned
// false emitted nothing and must count as `refusedDraws` -- reading the byte and then ignoring the
// facade's own answer would reinstate the exact defect this file exists to remove, one layer down.
//
// ---------------------------------------------------------------------------------------------
// * THE PUBLISHED INVARIANT, and the one term ARCH-15 S4.3 did not have a bucket for.
//
//     frameSiteEntries + fallbackEntries == inFrameDraws + refusedDraws + panelClosedEntries
//
// S4.3 froze `frameSiteEntries == inFrameDraws + refusedDraws`. That holds verbatim whenever the
// detour is installed (so `fallbackEntries == 0`) and the panel was open at every entry. The two
// extra terms are the residue it did not name, and they are PUBLISHED rather than absorbed:
//   - `panelClosedEntries` -- the site ran and the panel was not showing. Not a refusal: nobody
//     asked for a draw. Folding it into `refusedDraws` would make a deliberately closed panel read
//     as DL-07's `FrameClosedAtSite`, which is a negative answer to a question never put.
//   - `fallbackEntries` -- owner-tick attempts, which exist only while the detour is NOT armed.
// A record where the invariant does not hold is an instrument fault, not a measurement.
//
// `offThreadSiteEntries` sits OUTSIDE that identity on purpose: an entry taken on a thread that is
// not the owner's is forwarded to the engine and never offered a draw, so it is neither a draw nor
// a refusal, and it is kept out of `frameSiteEntries` because that counter is leg 3's denominator.
//
// * BOTH `panelClosedEntries` AND `offThreadSiteEntries` ARE W2-F EXTENSIONS OF ARCH-15 S4.3, and
// they are published here pending the architect's ratification (AUDIT_C1 F2/F7). S4.3's written
// form `frameSiteEntries == inFrameDraws + refusedDraws` is FALSE on any window in which the panel
// was closed at any entry; the form above is the one that holds. The `[inframe ...]` text line in
// src/ui/csregister.h stays FROZEN at S4.3's ten fields -- W2-R matches it byte for byte -- so
// `offThreadSiteEntries` reaches a reader through csregister::Report and not through that line.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

#include "bootstrap/hookmgr.h"

namespace replay {
namespace ui {
namespace inframe {

// ARCH-15 S5.0, frozen. The three engine globals, resolved through buildguard::MappedAddress() from
// the registry names facts::GUI_FRAME_OPEN_BYTE, facts::GUI_STARTFRAME_LATCH and
// facts::GUI_CONTEXT_PTR (merged by W0-2 in 903d3ba). Never transcribed and never copied locally.
struct FramePrecondition {
    bool frameOpenByte;      // *(u8*)GUI.FRAME_OPEN_BYTE  != 0
    bool startFrameLatch;    // *(u8*)GUI.STARTFRAME_LATCH != 0
    bool contextPtr;         // *(u64*)GUI.CONTEXT_PTR     != 0
    bool Ok() const { return frameOpenByte && startFrameLatch && contextPtr; }
};

// Three loads, no call, no allocation, no lock, on the thread that will later write them. Reading a
// byte the engine writes twice per frame from that same thread cannot perturb its scheduling.
// Every term is false when the address does not resolve -- unknown is not "present".
FramePrecondition Sample();

// ---- THE GUI SUPPRESSION GATE (ARCH-20 R1) ----------------------------------------------------
//
// The three loads above say the engine's GUI FRAME is open. They do not say the gate into
// NRadGui::Begin is open, and it is not: Begin's full precondition is
//
//     qword_1420DB978 && byte_1420DAF2F && ( dword_1420DB98C || (!byte_1420DAF2C && budget <= 0) )
//
// and `byte_1420DAF2C` is SET in retail. ARCH-20 s0.1 settles the two things that make R1 the
// route: the second disjunct is externally satisfiable (`dword_1420DB988 <= 0` holds process-wide,
// one writer in the whole image), so clearing that one byte is SUFFICIENT and not merely
// necessary; and the engine ships both the opener and the closer as pure setters, so we patch no
// code, write no engine data ourselves, and leave echovr.exe byte-identical.
//
// ** THE GATE IS SAMPLED, NEVER ASSUMED. Four loads, published whether or not they are acted on:
// `budget` and `reentry` are read so ARCH-20's claim about them is TESTABLE against a running
// build, and acting on either would be acting on an unmeasured constant.
struct GateState {
    bool    resolved;      // all four addresses mapped -- unknown is not "the gate is open"
    bool    suppressed;    // *(u8*)GUI.SUPPRESS_BYTE      != 0   -- 0x1420DAF2C
    int32_t reentry;       // *(i32*)GUI.REENTRY_DEPTH             -- 0x1420DB98C, read not acted on
    int32_t budget;        // *(i32*)GUI.WINDOW_BUDGET             -- 0x1420DB988, read not acted on
    uint8_t hotkeyEnable;  // *(u8*)GUI.HOTKEY_ENABLE_BYTE         -- 0x142025F6B
};

// The per-frame draw ceiling ARCH-20 s2.6 states as a number: "drawNanosMax > 200000 is a RED row"
// (docs/spikes/ARCH-20-GUI-GATE.md:259). It lives here, and it travels in the record beside the
// measurement it scores, so no offline reader keeps a second copy of the answer and no record is
// ever scored against a ceiling the build that produced it did not have.
inline constexpr uint64_t kDrawNanosBudgetNs = 200000;

// Four loads, no call, no allocation, on the owner thread. Every term is false or zero when its
// address does not resolve, and `resolved` is what tells those apart from a real zero.
GateState SampleGate();

// The value `guiRenderedCmdListsCount` and `guiRenderedTotalVtxCount` carry when nobody could ask.
// A vertex count is never negative, so this cannot be mistaken for a frame that drew nothing.
inline constexpr int32_t kGuiCountUnasked = -1;

// Take ARCH-20 S6.3(a)'s geometry off the engine's ImDrawData, if this instant is one at which it
// can be taken. The struct is only filled between a frame's EndFrame and the next NewFrame, so
// this is called from BOTH places our code runs -- the owner tick, ahead of the frame, and the
// in-frame draw site -- and latches the last reading that said Valid and agreed with itself.
// Costs two loads and five dword reads; safe to call every tick and every frame.
void SampleGuiDrawData();

// Is `ui.gate_scope` on? ★ DEFAULT TRUE since packet W2-F-C3. ARCH-20 s6.0 shipped it default
// FALSE and s6.3 named the two ways out; (b), "a look or a screenshot showing the panel", was met
// by run 20260904-132534Z -- the run\'s screenshot L5-RUN-7-run2.png shows the panel over the
// loaded arena and L5-RUN-7-run1.png is the same build minutes earlier with the scope off and no
// panel at all. On that run the scope was entered 5102 times, restored 5102 times, changed=0, and
// SC7 measured zero .text difference. Set the row false to get the retail gate back: neither
// engine setter is called and no byte is written.
//
// It reads the CONFIG ROW and nothing else. The ECHOREPLAY_UI_GATE_SCOPE override is declared on
// the row (kConfigEnvOverride) so the ledger dump resolves the same value this does -- a second
// parser here is what made the launch record say `disabled` about an armed run.
bool GateScopeEnabled();

// ★ RAII, DESTRUCTOR-ONLY, AND THE DESTRUCTOR IS THE WHOLE POINT. The scope opens the gate with the
// engine's own CLEAR_SUPPRESSED and closes it with the engine's own SET_SUPPRESSED, and the close
// has to happen on EVERY return path out of the draw -- an early return, or a throw out of a
// widget -- because a scope that leaked would leave the engine's debug GUI on for the rest of the
// process.
//
// ** `a1` IS THE OBSERVED BYTE, and that is a live mechanism rather than tidiness. The engine runs
// an edge-toggle of the very byte this scope moves, at 0x140F9293A-0x140F92967, every frame before
// our site, gated on the hotkey-enable byte CLEAR_SUPPRESSED's argument writes. Passing anything
// but the value we just read would hand the operator's keyboard a GUI toggle they did not have
// (ARCH-20 s1.2.2).
//
// ** COMPARE BEFORE RESTORE. The byte is re-read at close. If it is not where the scope left it,
// something outside us moved it: that is LOGGED BY NAME and counted, and the observed value is
// restored anyway -- the alternative is to leave the gate open on a guess about who else is
// writing it.
//
// ** AN UNMAPPED OR MUTATED SETTER IS A REFUSAL, NOT A HALF-SCOPE. If CLEAR_SUPPRESSED did not
// bind, `Opened()` is false, nothing is called, and the draw proceeds exactly as it does with the
// feature off. There is no path on which one half of the pair runs.
class GateScope {
public:
    explicit GateScope(const GateState& gate);
    ~GateScope();
    GateScope(const GateScope&) = delete;
    GateScope& operator=(const GateScope&) = delete;
    // True when this scope actually cleared the byte and owes a restore.
    bool Opened() const { return opened_; }

private:
    bool    opened_;
    uint8_t hotkeyEnable_;
};

// ** WHY gui::Begin SAID NO, as far as THIS side of the facade can actually tell (L5-RUN-5 G2).
// Named rather than guessed: the facade binds no IsWindowCollapsed and no frame counter, so
// "collapsed" and "clipped" are NOT readable from here and are not claimed. `BoundNoFrame` is the
// honest residual -- every entry the panel calls is bound, the placement was issued, and Begin
// still produced no window state for us. Collapsed, clipped, and a Begin outside the facade's own
// NewFrame/EndFrame pairing all land there; separating them needs a facade entry this build does
// not have, and inventing a word for one of the three would be a guess published as a fact.
enum class BeginRefusal : uint8_t {
    None = 0,        // Begin has not refused since the last reset
    BeginUnbound,    // gui::Begin has no bound entry: it returns false without calling anything
    NoPlacement,     // SetNextWindowPos/Size are unbound, so the window was never given geometry
    BoundNoFrame,    // everything we call is bound and Begin still said no
};

// Stable lowercase single-token names. These are the `reason=` vocabulary of the published
// `[facade ...]` group; W2-R reads them, so they do not change without the reader changing.
const char* BeginRefusalName(BeginRefusal r);

// Called by the panel from the false branch of its own Begin and from nowhere else. The panel is
// the only caller that can see the facade's binding state at the moment Begin answered.
void NoteBeginRefusal(BeginRefusal reason, uint32_t boundEntries, uint32_t entryCount);

struct Counters {
    uint64_t frameSiteEntries;     // row-2.5 detour entries
    uint64_t inFrameDraws;         // precondition held, panel showing, Begin accepted, End balanced
    uint64_t refusedDraws;         // the NAMED refusal: precondition failed, or Begin said no
    uint64_t refusedNoFrameByte;   // breakdown, so the reason is not guessed
    uint64_t refusedNoLatch;
    uint64_t refusedNoContext;
    // The aggregate the frozen ten-field `[inframe ...]` text group publishes as `facade=`. It is
    // the SUM of the two fields below and has no storage of its own -- one fact, one writer.
    uint64_t refusedByFacade;
    // ** THE TWO WAYS THAT SUM IS REACHED, TOLD APART (L5-RUN-5 G2). One counter could not say
    // whether a build had forgotten to wire the panel or whether the facade had turned us down,
    // and on a launch that read facade=3975 that is the whole question.
    uint64_t refusedNoDraw;        // the frame was open and no draw fn had been set: OUR wiring
    uint64_t refusedBeginFalse;    // the panel ran and its gui::Begin returned false
    // The panel's own reading of the LAST such Begin, and what the facade had resolved at the time.
    BeginRefusal beginRefusal;
    uint32_t facadeBoundEntries;   // gui::BoundCount() as the panel saw it
    uint32_t facadeEntryCount;     // gui::Entry::Count, so a reader needs no second copy of it
    uint64_t panelClosedEntries;   // site ran, panel not showing -- not asked, so not refused
    uint64_t fallbackEntries;      // owner-tick attempts, non-zero only while the detour is unarmed
    uint64_t offThreadSiteEntries; // site entries on a thread that is NOT the owner's: the engine's
                                   // own call is forwarded, no draw is attempted

    // ** THE STAGE-0 SAMPLE (ARCH-15 S7, RES-W1M-C0 S6). GUI.FRAME_OPEN_BYTE read once per owner
    // tick, at the top of the stage-0 drain -- the place the panel USED to be drawn from. ARCH-15
    // S1.4's whole case for this packet is that the byte is 0 there, on every tick, forever; that
    // claim was argued from the disassembly and never measured on a running build. These three
    // fields measure it. PREDICTION: nonZero == 0. A non-zero sample does not break anything, it
    // falsifies the premise the site was chosen on, so it is LOUD in the diagnostic rather than
    // merely counted.
    //
    // `samples` counts ticks on which the byte could be ASKED: an address that does not resolve is
    // not a sample of 0, and `samples` below the owner-tick count is how a reader sees that.
    uint64_t frameOpenAtOwnerTickSamples;
    uint64_t frameOpenAtOwnerTickNonZero;
    uint64_t frameOpenAtOwnerTickLastNonZeroTick;   // owner-tick index of the last non-zero, 0 if none

    // ** THE ARCH-20 GATE. Every one of these is zero on a build with ui.gate_scope off,
    // which is the default, and that is the point: a record with all of them zero and scope=0 is a
    // build that behaved exactly like retail.
    //
    // The published invariant of the header's opening section GAINS A TERM here:
    //
    //   frameSiteEntries + fallbackEntries
    //       == inFrameDraws + refusedDraws + panelClosedEntries + gateUnresolved
    //
    // `gateUnresolved` is the residue: the frame was open, the panel had a draw function, and the
    // GATE could not be ASKED. That is neither a draw nor a refusal by the facade -- nobody got as
    // far as asking the facade -- so folding it into `refusedDraws` would file a missing address
    // as a refusal the frame issued. Missing data must not become a confident answer.
    uint64_t gateUnresolved;         // the gate could not be sampled: no draw, no scope, ever
    uint64_t gateAlreadyOpen;        // the byte was already 0: draw with NO scope (ARCH-20 s2.1.2)
    uint64_t suppressScopeEntries;   // CLEAR_SUPPRESSED calls made
    uint64_t suppressScopeRestores;  // SET_SUPPRESSED calls made. MUST equal the line above in
                                     // every record; an inequality is an instrument fault.
    uint64_t suppressScopeChanged;   // closes where the byte was NOT where the scope left it
    uint64_t suppressSetterUnmapped; // scopes refused because a setter did not bind or guard

    // ** THE ARCH-20 S5 READ SIDE, which W2-R's `ui-gui-begin-gate` row is the reader
    // of. One pair of samples per entry that reaches the draw -- before it and after it -- of
    // engine `.data` this plugin never writes, plus the four ImGui context fields that mean
    // nothing behind a base which has not proved itself.
    //
    // ** IT RUNS ON BOTH SIDES OF THE ui.gate_scope FLIP, deliberately. S6.3(a) compares
    // `renderedTotalVtxCount` BETWEEN a gate_scope=false run and a gate_scope=true run, so a field
    // only the second run published is half a comparison. Reading engine memory is not the
    // behaviour the default-false protects: no setter is called and no byte is written.
    //
    // ** THE FOUR CONTEXT FIELDS ARE ZERO WHEN THE BASE DID NOT PROVE OUT, AND THE ROW PRINTS THEM
    // AS could-not-ask. The producer never invents a number for a base it could not verify, and it
    // publishes the OBSERVED atlas pair beside the .rdata pair so the reader can re-run term 3
    // instead of trusting `guiImguiBaseValid`.
    uint64_t guiGateSamples;         // sample pairs taken: entries that reached the draw
    bool     guiGateSampled;         // the read side is compiled into this build at all
    bool     guiGateAddrResolved;    // every .data and .rdata address of the last pair resolved
    char     guiGateUnresolved[160]; // the fact id that did not, when one did not
    uint32_t guiSuppressed;          // GUI.SUPPRESS_BYTE, BEFORE the draw
    uint32_t guiHotkeyEnable;        // GUI.HOTKEY_ENABLE_BYTE
    int32_t  guiReentryDepth;        // GUI.REENTRY_DEPTH, BEFORE
    int32_t  guiWindowBudget;        // GUI.WINDOW_BUDGET, BEFORE
    int32_t  guiViewportsEnabled;    // GUI.VIEWPORTS_ENABLED
    int32_t  guiBeginBalanceDelta;   // GUI.BEGIN_BALANCE after minus before -- the gate
                                     // discriminator, because the engine increments it on the
                                     // FIRST instruction past the gate
    bool     guiImguiBaseValid;      // ARCH-20 S5.1, ALL THREE terms
    char     guiImguiBaseReason[192];  // which term failed, when one did
    uint32_t guiAtlasDwordA;         // OBSERVED at io.Fonts+0x20
    uint32_t guiAtlasDwordB;         // OBSERVED at io.Fonts+0x24
    uint32_t guiAtlasExpectA;        // GUI.ATLAS_CONST_A, read from .rdata AT RUN TIME
    uint32_t guiAtlasExpectB;        // GUI.ATLAS_CONST_B, read from .rdata AT RUN TIME
    int32_t  guiImguiFrameCountDelta;      // ctx+0x1734, this entry minus the previous one

    // ** ctx+0xC0 != 0, AND A 0 HERE IS NOT EVIDENCE ABOUT PIXELS. `RenderDrawListsFn` is the
    // callback ImGui obsoleted in 1.60 and stopped calling; on an engine whose ImGui carries an
    // ImDrawData with a CmdLists VECTOR (which the layout in ui_facts.h says this one does) the
    // field is dead and reads 0 in a build that renders perfectly. It is published because it is a
    // real read of a real address; S6.3(a)'s `== 1` term rests on it and should not.
    uint32_t guiRenderDrawListsFnPresent;

    // ** S6.3(a)'s NUMBER, and its refusal. Read off ImDrawData (ui_facts.h's layout) and latched
    // only from a struct that says Valid and passes DrawDataLooksSane. `kGuiCountUnasked` (-1) is
    // could-not-ask: the base did not prove out, or the struct never agreed with itself, or no
    // sample ever landed in the window between EndFrame and the next NewFrame. A count is never
    // negative, so the refusal cannot be read as a measurement -- which is precisely what the 0
    // these two used to publish did on the run whose panel is in L5-RUN-7-run2.png.
    int32_t  guiRenderedCmdListsCount;
    int32_t  guiRenderedTotalVtxCount;
    uint64_t guiDrawDataLatches;    // samples that landed in the window and were taken
    uint64_t guiDrawDataRefused;    // samples the struct check rejected -- a wrong base reads here
    uint64_t guiDrawDataNotReady;   // samples between NewFrame and EndFrame: nothing to read yet

    // ** THE DECLARED, ACCEPTED SIDE EFFECT (ARCH-20 s2.4/F3). The suppression byte is restored
    // exactly; ImGui's hovered/active window and io.WantCaptureMouse are NOT, and the engine reads
    // both after our scope closes. `panelHoveredFrames` exists so that change is COUNTED rather
    // than inferred.
    //
    // ★ AND IT IS ZERO BECAUSE NOTHING IN THIS BINARY CAN ANSWER THE QUESTION, WHICH IS WHY THE
    // SOURCE IS PUBLISHED BESIDE IT. This build's NRadGui facade has no IsWindowHovered, no
    // IsWindowCollapsed and no mouse-position query -- established from the linked vocabulary in
    // src/ui/ui_facts.h and re-checked against the image (zero occurrences of "IsWindowCollapsed")
    // -- and ARCH-20 s3.1 ratifies no io offset for WantCaptureMouse. So a reader must not be able
    // to read this zero as "the panel was never hovered": `PanelHoverSourceName()` answers "none"
    // and is published in the same group, and until a ratified source exists this counter has no
    // production writer. Inventing one would be a guess published as a fact.
    uint64_t panelHoveredFrames;

    // ** THE BUDGET, MEASURED RATHER THAN ASSERTED (ARCH-20 s2.6). QueryPerformanceCounter around
    // the panel's draw call. `drawNanosMax > 200000` is a RED row: 0.2 ms is the per-frame budget
    // at 90 Hz, and a mean would hide the one frame that blew it.
    // `firstDrawNanos` is the first timed draw and is EXCLUDED from the maximum: it creates the
    // window, rasterises the panel's glyphs into the font atlas and loads the GUI's settings, once
    // per process. Counting it as a per-frame cost is what made L5-RUN-7 report 629300 ns against
    // a 21500 ns mean. It is still inside drawNanosTotal.
    uint64_t drawNanosTotal;
    uint64_t drawNanosMax;
    uint64_t firstDrawNanos;

    // ---- WHERE drawNanosMax WENT (W2-F DRAWSTALL) ------------------------------------------
    // drawNanosMax on its own names a stall and attributes it to nothing. These four split the
    // draw into the phases that can carry it, so a tail is read as a phase rather than argued
    // about. All four are MAXIMA, and the first draw is excluded from them exactly as it is from
    // drawNanosMax.
    //
    // imguiNanos is measured INSIDE panel::Draw and is a per-frame cost. The other three are the
    // worst SNAPSHOT BUILD the browser worker has done -- they are NOT frame time and must never
    // be added to imguiNanos; they are published here because a folder scan that costs 40 ms is
    // still worth reading, and the whole point of the fix is that it is not charged to a frame.
    uint64_t imguiNanos;           // gui:: calls inside panel::Draw
    uint64_t browserScanNanos;     // worker: the directory enumeration
    uint64_t metadataReadNanos;    // worker: per-file header, footer and manifest reads
    uint64_t textFormatNanos;      // worker: building every string the browser draws
    // DemoIndex entry points that opened a file on the thread panel::Draw runs on. The fix is that
    // this is 0; a build that regresses says so in the record instead of in a frame time.
    uint64_t browserIoOnDrawThread;

    // ---- THE SHAPE OF THE DISTRIBUTION, NOT JUST ITS TOP (W2-F DRAW4) -----------------------
    // The budget row still scores drawNanosMax, because one frame over the ceiling is a stutter
    // the owner sees and a mean would hide it. These say whether that max is a lone outlier or
    // what every frame costs, which is the difference between a scheduler artefact and a defect.
    //
    // They are HISTOGRAM UPPER EDGES: the true percentile lies in (x - kDrawNanosBucketNs, x].
    // O(1) per frame -- one shift and one relaxed increment, no reservoir and no sort.
    // drawNanosOverHistogram counts frames above the top edge; they are scored in the top bucket,
    // so a percentile equal to kDrawNanosHistogramTopNs means "at least this", never "exactly".
    uint64_t drawNanosP99;
    uint64_t drawNanosP999;
    uint64_t drawNanosSamples;
    uint64_t drawNanosOverHistogram;

    // ---- WHAT THE FRAME HANDED ImGui (W2-F DRAW4) --------------------------------------------
    // The stream, not the time it took: widgets issued and characters ImGui was asked to MEASURE
    // (everything from "##" on is an ID and is not measured, which is the rule ImGui itself uses).
    // The engine's per-widget constant is not ours to change; the stream is, and this is the
    // number a launch can compare against the offline bench directly.
    //
    // Counted ONLY on frames that drew the browser page, whose every string is built on the tick
    // and carries its own length -- so the instrument runs no strlen inside the bracket it is
    // measuring. guiStreamFrames is the denominator: 0 means no browser frame was drawn and the
    // two maxima are could-not-ask, NOT "the browser costs nothing".
    uint64_t guiItemsPerFrame;
    uint64_t guiGlyphsPerFrame;
    uint64_t guiStreamFrames;

    // The TRANSPORT page, counted separately (W2-F PANEL4). Folding it into the three above
    // would leave one pair of maxima that could not say WHICH page cost that much, and would
    // silently widen a denominator documented as browser frames. The page the owner plays on
    // gets its own budget or it has none, and it had none.
    uint64_t transportItemsPerFrame;
    uint64_t transportGlyphsPerFrame;
    uint64_t transportStreamFrames;
};

// The draw-time histogram's geometry, published so a reader can tell a percentile's resolution
// from its value and so the bench asserts the same geometry the product uses.
constexpr uint64_t kDrawNanosBucketNs = 1024;
constexpr size_t kDrawNanosBuckets = 512;
constexpr uint64_t kDrawNanosHistogramTopNs = kDrawNanosBucketNs * kDrawNanosBuckets;

// Lock-free read of relaxed atomics into caller-thread storage.
const Counters& Read();

// True once the row-2.5 detour is ARMED -- the prologue guard passed, the config row was declared,
// and MinHook enabled it. This is the switch the owner-tick fallback stands down on, so it must
// mean "the detour will run", not "the detour was accepted".
bool Installed();

// Why it is not installed, in hookmgr's own vocabulary. Absent before Install() has been called.
HookStatus InstallStatus();

// The thread the draw is allowed on, stamped by csregister's owner tick (which is the authority on
// what "owner" means). Until it has been stamped the first site entry latches its own thread, so
// the guard is live before the first OnTick rather than open for one frame. Idempotent, and cheap
// enough to call every tick.
void NoteOwnerThread(uint32_t threadId);

// One read of GUI.FRAME_OPEN_BYTE for the owner tick numbered `tickIndex` (1-based, so 0 can mean
// "never"). Called from the top of csregister's stage-0 drain, through the same MappedAddress path
// and the same test probe as the site's own precondition -- a second copy of the address, or a
// second way of reading it, would certify the premise against itself.
void SampleFrameOpenAtOwnerTick(uint64_t tickIndex);

// panel::Draw's own cost inside the facade, taken by the panel because it is the only code that
// knows which of its calls are gui:: ones. Called from inside the timed draw, so the first draw is
// still identifiable: it is the one where no draw has yet been timed.
void NoteImguiNanos(uint64_t nanos);

// One browser snapshot build, off the owner thread. Maxima, so the worst build is what is read.
void NoteSnapshotPhases(uint64_t scanNanos, uint64_t metadataNanos, uint64_t formatNanos);

// What one browser frame handed the facade, counted by the only code that knows which of its
// calls are widgets: the panel. Every browser frame is counted, the first one included -- the
// first draw is excluded from the TIMES because it builds the font atlas, and an atlas is not a
// widget; the stream it issues is the stream every later frame issues.
void NoteGuiStream(uint32_t items, uint32_t glyphs);

// One frame's answer to "was the cursor over the replay window?", from the caller that has a cursor
// to answer it with. It is what gives `panelHoveredFrames` a production writer and
// PanelHoverSourceName() a word other than "none".
void NoteCursorOverPanel(bool over);

// The same, for one transport-page frame. Its strings are built in the frame that draws them,
// so the panel measures each label as it hands it over rather than carrying lengths in.
void NoteTransportGuiStream(uint32_t items, uint32_t glyphs);

// What the panel's draw call returns. Three states, because "the panel is closed" and "the facade
// refused us" are different answers and a two-state bool cannot carry both.
enum class DrawOutcome : uint8_t {
    Drawn,           // Begin returned true and the widgets were emitted
    NotShowing,      // the panel is closed, or the facade is not bound -- nobody asked
    FacadeRefused,   // Begin returned false: the frame did not accept us after all
};
using DrawFn = DrawOutcome (*)();
void SetDrawFn(DrawFn);

// Worker thread, after the build guard has run. Creates and arms the row-2.5 detour. Refused, by
// name and in the log, on an unrecognised build, an undeclared `ui.inframe_draw`, or a prologue
// that is not the recorded one (plugins\ is shared -- differing bytes are how another mod announces
// itself, and we never overwrite somebody else's patch).
bool Install();

// The owner-tick attempt, which is the fallback and runs ONLY while the detour is unarmed. It goes
// through the same precondition and the same counters, so a fallback draw taken with the frame shut
// lands in `refusedDraws` and cannot be certified by `ownerDraws`.
// Returns true when the panel was actually drawn.
bool AttemptFallbackDraw();

// ---- test seams -------------------------------------------------------------------------------

// Where `panelHoveredFrames` gets its answer, as a single lowercase token published beside the
// count. "none" means the question could not be asked at all, and a zero above it is therefore not
// a measurement. W2-R reads this word, so it does not change without the reader changing.
const char* PanelHoverSourceName();

void ResetForTest();

// Read the three globals out of caller-supplied storage instead of the mapped image. Any pointer
// may be null, which reads as that term being false. Passing all three null restores the image.
void SetProbeForTest(const uint8_t* frameOpenByte, const uint8_t* startFrameLatch,
                     const void* const* contextPtr);

// The same seam for the four gate globals, so SampleGate's four arms are driven rather than
// assumed. Any pointer may be null, which reads as that term unresolved. All four null restores
// the image.
void SetGateProbeForTest(const uint8_t* suppressByte, const int32_t* reentry, const int32_t* budget,
                         const uint8_t* hotkeyEnable);

// The same seam for the ARCH-20 S5 read side. `imguiContextSlot` stands in for the `.data` slot at
// GUI.IMGUI_CONTEXT and is a pointer TO the pointer, because the production read is a load and not
// a lea -- a seam that handed the context directly would make the B1 defect untestable. Any
// pointer may be null, which reads as that address unresolved; all null restores the image.
void SetGuiProbeForTest(const void* const* imguiContextSlot, const uint32_t* atlasConstA,
                        const uint32_t* atlasConstB, const int32_t* viewportsEnabled,
                        const int32_t* beginBalance);

// Force `ui.gate_scope` on (1) or off (0) for one test; -1 restores the configured answer. The
// DEFAULT-TRUE case is a test in its own right and must go red if somebody flips the default back
// without flipping W2-F-C3's evidence with it.
void SetGateScopeForTest(int onOffOrMinusOne);

// Drive the real Create/Arm ladder against a synthetic victim function, so the prologue guard and
// the config gate are exercised rather than assumed. `configKey` is the row's key; production
// passes "ui.inframe_draw".
HookStatus InstallForTest(const char* factId, uint64_t pcVa, const uint8_t* prologue,
                          size_t prologueLen, const char* configKey);

// One entry at the row-2.5 site, exactly as the armed detour performs it (trampoline first, then
// the draw). `original` stands in for the engine's HeartbeatBefore.
using OriginalFn = uint64_t(__fastcall*)(void*);
void CallSiteForTest(OriginalFn original, void* engine);

// Pretend the detour is armed without one, so the fallback's stand-down can be driven both ways.
void SetInstalledForTest(bool installed);

}  // namespace inframe
}  // namespace ui
}  // namespace replay
