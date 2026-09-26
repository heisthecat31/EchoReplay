#pragma once

// W2-F. Registering our draw callback as a band-3 component-system update.
//
// ---------------------------------------------------------------------------------------------
// * WHY WE REGISTER AT ALL, when W1-M section 4 recommends drawing from the tick we already own.
//
// That recommendation rests on the GUI frame being open at our stage-0 tick. Two other verified
// results say it is not. W1-D places NRadGui::StartFrame at beat 3 -- inside stage 0 but AFTER our
// tick -- and RES-W1M-C0 s7 corrects W1-M round 2's reading of where the frame closes:
// NRadGui::EndFrame runs in the RENDER PASS, reached through the CEngine vtable slot at +0x88, not
// in a band pass between bands 5 and 6. The correction does not rescue our tick: it still leaves no
// open frame at stage 0, and DESIGN 8.5.3's "the frame is open because the last frame opened it" is
// false under either reading.
//
// ARCH-15 S1.4 then settled it by measurement rather than inference. At our stage-0 tick
// GUI.FRAME_OPEN_BYTE is 0, so NRadGui::Begin and 38 further facade entry points take their
// `cmp cs:byte_1420DAF2F, 0 / jz` early return and our draw produces no state at all. THE DRAW HAS
// THEREFORE MOVED OFF THIS TICK -- see src/ui/inframe.h, ledger row 2.5. What remains here is the
// registration, its telemetry, and a fallback draw that runs only while that detour is unarmed.
//
// The registration is still worth having, and ARCH-15 S2 says exactly what it is worth. The band-3
// PASS at 0x14051797E is inside the open frame, and the very next vtable call at 0x14051798D
// reaches CR15NetGame::ProcessUI. But the band-3 CALLBACK is not inside it: L5 re-run 3 measured
// 4681 of 4681 entries on a CTaskKernel worker, and a worker thread's execution window is not
// bounded by the pass's return. So band 3 buys COMPOSITING ORDER AND TELEMETRY -- which is what
// marshalledDraws reports -- and it does not buy the draw. The draw is row 2.5's.
//
// * THE FOUR WAYS THIS SILENTLY DOES NOTHING, and what each costs.
//   1. orderKey 0x7FFF, 0x1FFF, 0xDFFF and friends fall in NO band and are never executed, while
//      sorting happily among their neighbours. We use 0x7FFE, the last VALID band-3 key.
//   2. Bit 23 of *(uint32_t*)(game + 0x828) skips exactly the band call at [rax+190h] -- the one
//      that runs our registered update -- and NOT the whole stage 0-3 pass, which continues either
//      way (RES-W1M-C0 s7, correcting this comment's earlier claim). The narrower reading makes it
//      a sharper suspect rather than a weaker one: it silences this callback specifically. We
//      cannot read that word from a pointer this packet has derived, so we do not claim to have
//      checked it -- we name it as one of the two candidate causes when the callback never fires.
//   3. CS.LOOKUP_BY_SYMBOL returning null means the component system is not in this space, and
//      every stock site skips its registration in that case. So do we.
//   4. The registrar has no failure path and no return value, so "did it land" is answered by the
//      record count at scheme+0x610 moving, not by the call returning.
//   Reporting only "the callback fired N times" would make 1, 2 and 3 indistinguishable. Status()
//   carries the count before, the count after, and the invocation total, so they are not.
//
// * TWO HARD RULES THAT ARE NOT STYLE.
//   REGISTER FROM THE STAGE-0 TICK, NEVER FROM INSIDE A DAG CALLBACK. The record table grows by
//   REALLOCATION and the runner re-reads its base after the callback returns, so registering from
//   inside one frees the array the runner is standing on. There is no lock anywhere in the
//   registrar. Our tick is the CNSProvider slot, which is not a DAG node.
//   REGISTRATION IS ONE-WAY. W1-M looked for a PC counterpart to CncaGameSpace::UnregisterUpdate
//   and did not find one. So the engine will keep calling our callback for the life of the game
//   space, including after our module has shut down. Two consequences are handled here rather than
//   hoped about: the callback's state has static storage duration and is never freed, so entering
//   it after shutdown reads a live `enabled` flag and returns; and the module PINS ITSELF with
//   GET_MODULE_HANDLE_EX_FLAG_PIN before the first registration, because a FreeLibrary on us would
//   otherwise leave the engine calling into unmapped code on its next frame.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

#include "bootstrap/tableguard.h"
#include "ui/inframe.h"

namespace replay {
namespace ui {
namespace csregister {

// Band 3 is 0x6000-0x7FFE. The band scan admits (uint16_t)(key - base) <= 0x1FFE and the driver
// stops at key > hi, so 0x7FFE is the last key that runs and 0x7FFF is the trap.
inline constexpr uint16_t kOrderKey = 0x7FFEu;
inline constexpr uint16_t kTiebreak = 0x7FFEu;
inline constexpr uint16_t kSchemeIndex = 0u;

// Ticks a registration may sit with no invocation before it is reported as Unconfirmed rather
// than Registered: about five seconds at the engine's 60 Hz Update slot. Not measured against a
// stock scheme's first-dispatch latency; CSREGISTER-EPOCH section 2 says replace it if one is.
inline constexpr uint64_t kUnconfirmedTicks = 300u;

// Owner ticks a live dispatch may go unseen, or a marshalled draw un-run, before Running is
// withdrawn. 60 is one second at the engine's 60 Hz Update slot, and the band-3 pass runs once per
// frame, so a whole second of either is not a hiccup.
inline constexpr uint64_t kLivenessTicks = 60u;

// Callback entries recorded but not yet drained. The owner thread drains every tick, so a backlog
// this deep means it has stopped ticking -- which is the one staleness case that cannot be
// measured in owner ticks, because there are none.
inline constexpr uint64_t kStarvedPending = 60u;

// Why we are not drawing, when we are not. Every value is a distinct cause with a distinct fix.
enum class Status : uint8_t {
    NotRegistered = 0,  // no attempt yet -- no netgame, or the facade never bound
    NoGameSpace,        // netGame + NETGAME.CS_SPACE_OFF (+0x2B08) was null
    SpaceNotReady,      // the space exists but its tables are not built; WE WILL RETRY EVERY TICK
    NoComponentSystem,  // CS.LOOKUP_BY_SYMBOL returned null for this space
    RegistrarUnavailable,  // the registrar or the lookup failed its prologue guard
    Registered,         // the record count moved; waiting for the first invocation (PROVISIONAL)
    Running,            // DESIGN S8.5.4a's THREE LEGS, VERBATIM AND NOTHING ELSE:
                        // registrations >= 1 AND invocations > 0 AND inFrameDraws > 0 (leg 3
                        // re-ruled by ARCH-15 S4.2; it was ownerDraws, which counted
                        // refusals). Each leg
                        // is separately published, so a reader can score the word from the parts.
                        // No recency term is folded in here (AUDIT_F3E F3E-1): a session that drew
                        // and then went quiet is still Running by the definition, and the going-
                        // quiet is MarshalStale / CallbackSilent below -- separate words, so the
                        // ratified definition and the instrument cannot be confused for each other.
    WrongThread,        // the DRAW path was entered off the owner thread. After the marshal this
                        // must never happen -- the callback no longer draws -- so it is an
                        // internal-invariant alarm, not the ordinary off-thread dispatch. An
                        // off-thread DISPATCH is normal and counted in offThreadInvocations.
    Stale,              // our record is gone from the space's table: the engine rebuilt the space
                        // under the same pointer. Re-registers next tick.
    Unconfirmed,        // Registered, our record still present, nothing has fired for
                        // kUnconfirmedTicks: either the engine never runs this scheme here or the
                        // record sits in memory nothing dispatches from. NOT a form of Registered.
    NetGameUnreadable,  // the netgame pointer is non-null but +0x2B08 is not mapped, so the space
                        // slot could not be ASKED. Appended rather than placed beside NoGameSpace
                        // so the existing values keep their numbers. NOT a form of NoGameSpace:
                        // "no space yet" is a state a live netgame passes through, this is not a
                        // netgame.
    Invoked,            // LEGS 1+2 WITHOUT 3, and this is DESIGN S8.5.4a's own word for it: the
                        // registration landed and the engine has invoked us, and the owner thread
                        // has never once drawn (ARCH-15 S4.2: leg 3 is inFrameDraws, so this word
                        // means no widget call of ours has been ACCEPTED inside the engine's own
                        // GUI frame). The row-2.5 site has not run, or the panel is closed, or
                        // every entry was refused. Running is MODULE LIVENESS and is
                        // NOT evidence of a visible panel, so neither is this word evidence of an
                        // invisible one: whether the band-3 path reached a frame is the separate
                        // marshalledDraws diagnostic, which gates nothing. Was named MarshalStarved before AUDIT_F3E F3E-1; the ratified
                        // word wins, because probe artifacts are matched against the design.
    CallbackSilent,     // LEG 2, STOPPED. The dispatch worked and then dried up: no callback entry
                        // at all in the last kLivenessTicks owner ticks. Distinct from Unconfirmed,
                        // which is leg 2 NEVER -- a registration that has not once been dispatched,
                        // and whose two candidate causes are the band trap and the gate bit.
                        // NOT a leg of Running: it is a recency instrument over a session all
                        // three of whose legs hold, reported as its own word (F3E-1).
    MarshalStale,       // The three legs hold and the PANEL HAS STOPPED BEING EMITTED: either the
                        // backlog has grown past kStarvedPending (entries arriving with nothing
                        // draining them -- the owner tick has stopped, and a stopped tick cannot be
                        // counted in owner ticks) or kLivenessTicks owner ticks have run without a
                        // draw (disabled, or no draw function installed). Also not a leg of
                        // Running, and never folded into it (F3E-1); ARCH-13 ratifies or drops it.
    LiveInert,          // livesafety refused Feature::GameSpaceRegister, so this process never
                        // asked the engine for anything: no lookup, no record appended into a
                        // space that belongs to other people's live match. Appended last so the
                        // existing values keep their numbers. NOT a failure -- it is the gate
                        // working, and the panel draws without it (see Enable()).
};

const char* StatusName(Status);

// ---------------------------------------------------------------------------------------------
// * THE GAME SPACE IS CHECKED BEFORE ANY ENGINE ROUTINE INDEXES IT (DIAG-CRASH launch-12).
//
// A CR15NetGame is published with a game space at NETGAME.CS_SPACE_OFF (+0x2B08) before that space
// is built -- +0x8 is the CR15Game that owns the netgame, not a space (ARCH-10 section 1) -- and the engine
// guards its tables by ORDERING, not by a built-flag (CS.GAMESPACE_HAS_NO_BUILT_FLAG) -- a
// protection an outside tick does not inherit. The predicate is W1-C's tableguard::CheckSpaceTable,
// the constructor's own postcondition read back out of the space, shared with every site that
// hands a game-space table to the engine. Two tables are checked here: the component-system table
// at +0x420 (searched by CS.LOOKUP_BY_SYMBOL) and scheme 0's update-record table (appended to by
// the registrar). Which one refused, and the check's own verdict and raw fields, are on the Report.
// ---------------------------------------------------------------------------------------------

// What a deferral was refused on. None is 0 so a Report nobody has driven cannot name a reason.
enum class Refusal : uint8_t {
    None = 0,
    ComponentSystemTable,  // the table CS.LOOKUP_BY_SYMBOL would search is not Ready
    UpdateRecordTable,     // the table the registrar would append to is not Ready
    UnsortedTable,         // both are Ready, but bit 0 of the flag word is set: the engine has not
                           // sorted the table and the lookup would sort it in place, a write we
                           // never cause
};

const char* RefusalName(Refusal);

// One entry into the band-3 callback, as the callback recorded it. The callback writes this from
// whatever thread the update runner chose; the owner thread reads it back under a seqlock, so a
// reader either gets one invocation's four fields or gets told it could not.
struct Invocation {
    uint64_t sequence = 0;     // 1-based ordinal of the callback entry; 0 = nothing recorded yet
    uint64_t monotonicNs = 0;  // QPC at the entry, so two invocations can be ordered in time
    uint32_t threadId = 0;     // the thread the engine dispatched on
    uint16_t orderKey = 0;     // the band-3 key the dispatched record was registered with
};

struct Report {
    Status status = Status::NotRegistered;
    uint64_t recordsBefore = 0;   // scheme+0x610 immediately before the registrar call
    uint64_t recordsAfter = 0;    // ... and immediately after
    uint64_t invocations = 0;     // callback entries, whatever thread, for the module's lifetime
    uint64_t offThreadInvocations = 0;
    // The epoch's Running invariant is per REGISTRATION, so the lifetime total cannot enforce it:
    // registration is one-way, so a space we have left keeps dispatching our record, and one such
    // dispatch would otherwise confirm a registration in a different space that has never run.
    // ThunkDraw tells them apart by the CComponentSystem the engine hands back (see csregister.cpp).
    uint64_t registrationInvocations = 0;  // dispatches belonging to the CURRENT registration
    uint64_t staleInvocations = 0;         // dispatches from a registration we no longer hold
    const void* lastStaleDispatch = nullptr;  // the opaqueThis of the last of those
    uint32_t mainThreadId = 0;    // as seen from the stage-0 tick
    uint32_t lastCallbackThreadId = 0;
    uint32_t registrations = 0;   // one per game space we have registered into
    const void* gameSpace = nullptr;
    const void* componentSystem = nullptr;   // of the last COMPLETED registration

    // Deferral is a THIRD outcome, not a quiet form of failure. A registration that never happens
    // and says nothing is worse than the crash it replaced, because nobody notices it.
    uint64_t deferrals = 0;             // ticks refused because a table was not built yet
    uint64_t deferralsBeforeFirst = 0;  // ... of those, the ones before the first registration
    Refusal lastRefusal = Refusal::None;
    tableguard::TableCheck lastRefused;  // the check's verdict, reason and raw fields at the refusal

    // ---------------------------------------------------------------------------------------------
    // * THE EPOCH (CSREGISTER-EPOCH, AUDIT_W2-F_AVFIX F3). A game space can be torn down and rebuilt
    //   at the SAME address, and the block's previous life leaves five fields that pass every
    //   readiness leg. A registration into that block is wiped by the real constructor, and a
    //   pointer compare never notices. So the pointer is never again the whole test: every tick
    //   on a registered space, the update-record table is re-read and scanned for our own PMF.
    //   Present: live. Absent, or the table no longer Ready: Stale, and the next tick re-registers
    //   into the same pointer. The component-system header triple is compared to its snapshot as
    //   a second, reported-only signal (a live table can grow).
    // ---------------------------------------------------------------------------------------------
    uint64_t staleDetected = 0;      // times our record went from present to absent: F3 OBSERVED
    uint64_t headerMoved = 0;        // times the cs header triple differed while our record held
    uint64_t unsortedDeferrals = 0;  // ticks the lookup was refused on an unsorted table
    uint64_t unconfirmedTicks = 0;   // ticks spent Registered/Unconfirmed with no invocation
    uint64_t spaceChanges = 0;       // non-null -> different non-null pointer changes seen
    // Every distinct game space OnTick has seen, INCLUDING the first. spaceChanges deliberately
    // skips the first transition (null -> the first space is not a change), which makes
    // "registrations == spaceChanges" a clause that cannot hold on a session with one space and
    // one registration: 1 != 0, always. The epoch clause is
    // "registrations == spacesSeen", and that one both passes and fails: it holds when every space
    // we saw took a registration, and breaks when one was seen and refused.
    uint64_t spacesSeen = 0;

    // ---------------------------------------------------------------------------------------------
    // * THE MARSHAL (L5-RUN-3 F6). The band-3 callback is dispatched on a CTaskKernel WORKER thread,
    //   so it may not touch ImGui or any engine UI state. It therefore draws NOTHING: it records the
    //   entry into the mailbox above and returns. The owner thread -- the thread our stage-0 tick
    //   runs on, measured at OnTick, never assumed -- drains the mailbox and runs the draw.
    //
    // ★★ THE RENDER-CADENCE INVARIANT (AUDIT_F3E F3E-4). WHILE ENABLED, THE PANEL IS DRAWN ON
    //   EVERY OWNER TICK -- EXACTLY ONCE, AND WHETHER OR NOT A BAND-3 ENTRY ARRIVED. The panel is
    //   ImGui immediate mode: a frame that does not call panel::Draw has no panel in it, not a
    //   stale one, and panel::Draw has exactly one non-test caller, reached only from here. The
    //   band-3 mailbox therefore informs ORDERING and TELEMETRY -- it must never gate the draw, or
    //   the panel's cadence becomes an asynchronous CTaskKernel task's rather than the frame's and
    //   the window blinks. Exactly once per tick, because emitting the panel N times into one
    //   frame stacks N of it.
    //
    // ★★ AND THE ONE DRAW PREFERS THE INLINE DISPATCH (AUDIT_F3I F3I-5). When the runner calls the
    //   band-3 record INLINE on the owner thread, that call IS inside the GUI frame the slot was
    //   registered for, so it -- not the stage-0 tick -- is the draw that lands in the compositing
    //   slot, and it must not be suppressed. The tick cannot see the dispatch coming (it arrives
    //   later in the same frame), so the preference is carried one tick: a tick that saw an inline
    //   dispatch stands aside on the NEXT tick and the dispatch draws. `inlineDraws` counts the
    //   draws taken in that slot, `inlineDeferrals` the ticks that stood aside, and
    //   `inlineDeferralsUnfilled` the ticks that stood aside for a dispatch that did not come --
    //   one such frame per switch from inline to asynchronous dispatch, and no more.
    //
    //   An invocation is never dropped without a number: every recorded entry is either drained
    //   (drainedInvocations) or still queued (pendingInvocations), and the entries that were
    //   folded into a tick's single draw rather than getting one of their own are
    //   coalescedInvocations.
    // ---------------------------------------------------------------------------------------------
    // S8.5.4a's three legs scored VERBATIM from the parts -- `registrations >= 1 AND
    // invocations > 0 AND inFrameDraws > 0` (leg 3 re-ruled by ARCH-15 S4.2) -- published as its
    // own fact, and scored by the SAME term ScoreLiveness uses, never a second copy of the ladder.
    // This is ARCH-13's
    // FIRST BAND: the second band (MarshalStale / CallbackSilent) is reachable only once all three
    // legs have held, so `runningPerDesign` stays true under a second-band word and a reader can
    // tell "never got there" from "got there and went quiet" without parsing a status name.
    bool runningPerDesign = false;
    uint64_t ownerTicks = 0;             // OnTick calls on the owner thread. The denominator for
                                         // ownerDraws: the two are equal on a launch where the
                                         // panel was open throughout, and ownerDraws well under
                                         // ownerTicks is a measured frame drop (F3E#2). Scored
                                         // across the enable stamp, never on the lifetime
                                         // totals -- see ownerTicksAtEnable (F3I-3).
    // ★★ TWO DRAW COUNTERS, AND THEY ANSWER DIFFERENT QUESTIONS (AUD-ARCH F-B). Before the
    // render-cadence ruling the drain drew only when it had band-3 entries, so ONE counter meant
    // both "the panel is on screen" and "the band-3 path delivered". F3E-4 made the draw
    // unconditional and silently widened it to the first sense only, which left DESIGN S8.5.4a's
    // leg 3 asserting something weaker than it was written to assert -- and after F3I-4 the panel
    // draws from the first tick whether or not anything was ever registered, so leg 3 would have
    // been satisfied on a launch where band 3 never ran at all.
    //
    //   ownerDraws       -- owner ticks on which the FALLBACK draw was issued and the panel
    //                       actually reached a frame. ★ IT IS NO LONGER LEG 3 AND NO LONGER A
    //                       COVERAGE NUMERATOR (ARCH-15 S4.1). It used to count ATTEMPTS, and S1.4
    //                       proves every attempt at this site is refused, so it read 1:1 with
    //                       ownerTicks on a build that drew nothing -- a green light on the exact
    //                       failure it existed to catch. Leg 3 and the coverage half are now
    //                       inframe::inFrameDraws over inframe::frameSiteEntries, and the refusals
    //                       this counter used to absorb are named by inframe::refusedDraws. It
    //                       stays 0 while the row-2.5 detour is armed, because the fallback stands
    //                       down rather than drawing the window a second time.
    //   marshalledDraws  -- the subset of the drawing ticks that CONSUMED at least one band-3
    //                       entry. A DIAGNOSTIC, feeding no gate: it says whether the band-3 path
    //                       ever reached a frame, and ARCH-15 S4.2 keeps it published for the
    //                       compositing-order risk it speaks to.
    uint64_t ownerDraws = 0;             // fallback ticks on which the panel reached a frame
    uint64_t marshalledDraws = 0;        // ...of those, the ones that carried band-3 entries

    // ★ THE ROW-2.5 COUNTERS (ARCH-15 S4.3), mirrored here so one Report carries the whole picture
    // and W2-R parses one record rather than two. The definitions and the published invariant are
    // in src/ui/inframe.h; these are copies taken at the same instant as everything above.
    //
    // refusedDraws is published EVEN WHEN IT IS 0. Its absence from a record means "not asked" and
    // its zero means "asked, none" -- two states a field that only appears when non-zero cannot
    // tell apart (ARCH-15 S4.5).
    uint64_t frameSiteEntries = 0;       // row-2.5 detour entries
    uint64_t inFrameDraws = 0;           // ...that drew into the open frame. S8.5.4a's LEG 3
    uint64_t refusedDraws = 0;           // ...that were refused, by name
    uint64_t refusedNoFrameByte = 0;
    uint64_t refusedNoLatch = 0;
    uint64_t refusedNoContext = 0;
    uint64_t refusedByFacade = 0;        // = refusedNoDraw + refusedBeginFalse
    // ** THE TWO HALVES OF THAT SUM (L5-RUN-5 G2), plus the facade's own reason for the second.
    // The aggregate stays because W2-R's frozen ten-field `[inframe ...]` parse reads it; these are
    // published in the SEPARATE `[facade ...]` group so that parse is untouched.
    uint64_t refusedNoDraw = 0;
    uint64_t refusedBeginFalse = 0;
    inframe::BeginRefusal beginRefusal = inframe::BeginRefusal::None;
    uint32_t facadeBoundEntries = 0;
    uint32_t facadeEntryCount = 0;

    // ** THE ARCH-20 GATE. Published in their OWN `[gate ...]` group, placed after
    // `[facade ...]` and before the threads tail, so W2-R's frozen ten-field `[inframe ...]` parse
    // is untouched. `scope` is the config row's effective value: without it a reader cannot tell a
    // build that never opened the gate from one that opened it and found nothing to do.
    bool     gateScopeEnabled = false;
    uint64_t gateUnresolved = 0;
    uint64_t gateAlreadyOpen = 0;
    uint64_t suppressScopeEntries = 0;
    uint64_t suppressScopeRestores = 0;
    uint64_t suppressScopeChanged = 0;
    uint64_t suppressSetterUnmapped = 0;
    // Zero with `hoversource=none` beside it means the question could not be asked; see
    // src/ui/inframe.h. It is NOT a measurement of "never hovered".
    uint64_t panelHoveredFrames = 0;
    uint64_t drawNanosTotal = 0;
    uint64_t drawNanosMax = 0;
    // The FIRST timed draw, excluded from drawNanosMax because it builds the caches every
    // later draw reuses. Published so the exclusion can be audited: a reader that cannot see
    // the excluded number cannot tell a warm-up from a laundered tail.
    uint64_t firstDrawNanos = 0;
    // inframe::kDrawNanosBudgetNs as THIS build held it. 0 means the field is absent.
    uint64_t drawNanosBudget = 0;
    // The split of the draw, so a breach names a phase instead of a number. imguiNanos is frame
    // time; the three browser numbers are the worst off-thread snapshot build and are NOT frame
    // time. browserIoOnDrawThread is 0 in a build where Draw opens no file, which is the fix.
    uint64_t imguiNanos = 0;
    uint64_t browserScanNanos = 0;
    uint64_t metadataReadNanos = 0;
    uint64_t textFormatNanos = 0;
    uint64_t browserIoOnDrawThread = 0;
    // The distribution behind drawNanosMax, so the final audit can tell a scheduler outlier from
    // the shape of every frame. Histogram upper edges: the true value is inside
    // (x - 1024, x] ns. The budget row still scores drawNanosMax and nothing else.
    uint64_t drawNanosP99 = 0;
    uint64_t drawNanosP999 = 0;
    uint64_t drawNanosSamples = 0;
    uint64_t drawNanosOverHistogram = 0;
    // What a browser frame handed the engine's ImGui: widgets issued, and characters ImGui was
    // asked to MEASURE. The engine's per-widget constant is not ours; this is the half that is.
    // ** guiStreamFrames == 0 means NO BROWSER FRAME WAS DRAWN and the two numbers beside it are
    // could-not-ask. They are never "the browser cost nothing".
    uint64_t guiItemsPerFrame = 0;
    uint64_t guiGlyphsPerFrame = 0;
    uint64_t guiStreamFrames = 0;
    uint64_t panelClosedEntries = 0;     // site ran, panel closed -- not asked, so not refused
    // ** Site entries on a thread that is NOT the owner's. The engine's own call is
    // forwarded; no draw is attempted, and they are NOT in frameSiteEntries, which is leg 3's
    // denominator. Published here and deliberately NOT in the frozen `[inframe ...]` text line
    // below, which W2-R matches byte for byte -- the line moves only when the architect ratifies
    // the wider form. 0 is "the site ran only on the owner thread", which is what the image says.
    uint64_t offThreadSiteEntries = 0;
    // ** THE STAGE-0 FRAME BYTE, SAMPLED ONCE PER OWNER TICK (ARCH-15 S7 / RES-W1M-C0 S6). The
    // premise the row-2.5 site was chosen on, measured on the running build instead of argued from
    // the disassembly: S1.4 predicts frameOpenAtOwnerTickNonZero == 0 for every launch. A non-zero
    // one is LOUD in Diagnostic() and is not a fault of ours -- it means the owner tick can reach
    // an open frame after all. `Samples` counts the ticks the byte could be ASKED on, so
    // `Samples` < `ownerTicks` says the address stopped resolving rather than that it read 0.
    uint64_t frameOpenAtOwnerTickSamples = 0;
    uint64_t frameOpenAtOwnerTickNonZero = 0;
    uint64_t frameOpenAtOwnerTickLastNonZeroTick = 0;
    // ** ARCH-20 S5 -- the `ui-gui-begin-gate` field block. W2-R's row is the READER of
    // every field here and re-derives none of them; observer.cpp copies them out of THIS
    // struct, so a Report without them fails to compile rather than writing a confident zero.
    //
    // The four context-relative fields are 0 whenever `guiImguiBaseValid` is false, and the row
    // prints them as could-not-ask. The observed atlas pair sits beside the `.rdata` pair on
    // purpose: the reader re-runs S5.1's term 3 instead of trusting the bool.
    bool guiGateSampled = false;
    bool guiGateAddrResolved = false;
    char guiGateUnresolved[160] = {};
    uint64_t guiGateSamples = 0;
    uint32_t guiSuppressed = 0;
    uint32_t guiHotkeyEnable = 0;
    int32_t guiReentryDepth = 0;
    int32_t guiWindowBudget = 0;
    int32_t guiViewportsEnabled = 0;
    int32_t guiBeginBalanceDelta = 0;
    bool guiImguiBaseValid = false;
    char guiImguiBaseReason[192] = {};
    uint32_t guiAtlasDwordA = 0;
    uint32_t guiAtlasDwordB = 0;
    uint32_t guiAtlasExpectA = 0;
    uint32_t guiAtlasExpectB = 0;
    int32_t guiImguiFrameCountDelta = 0;
    uint32_t guiRenderDrawListsFnPresent = 0;
    int32_t guiRenderedCmdListsCount = 0;
    int32_t guiRenderedTotalVtxCount = 0;
    uint64_t fallbackEntries = 0;        // owner-tick attempts; 0 while the detour is armed
    bool inframeInstalled = false;       // the row-2.5 detour is armed
    HookStatus inframeStatus = HookStatus::Absent;   // why, when it is not
    // ★ THE WINDOW BASELINE (AUDIT_F3I F3I-3). ARCH-13 s2.3's coverage rule is `drawsAfter >=
    // ownerTicksAfter - 1` ACROSS A STAMP, and ownerTicks counts from the module's first tick --
    // including every tick before the panel was ever opened. Scored on the lifetime totals, a
    // launch that sat in the menu for 600 ticks and then drew on all 3400 of the rest reads as 599
    // missing frames. These are the counters AT THE STAMP, so a reader subtracts and scores the
    // window that was actually measured. Stamped on the closed -> open transition only.
    uint64_t ownerTicksAtEnable = 0;
    uint64_t ownerDrawsAtEnable = 0;
    uint64_t marshalledDrawsAtEnable = 0;
    uint64_t enableStamps = 0;           // closed -> open transitions; 0 means never opened
    uint64_t inlineDraws = 0;            // draws taken by an inline band-3 dispatch
    uint64_t inlineDeferrals = 0;        // owner ticks that stood aside for the inline slot
    uint64_t inlineDeferralsUnfilled = 0;  // ...and the dispatch never came, so the frame had none
    uint64_t drainedInvocations = 0;     // recorded entries taken out of the mailbox
    uint64_t coalescedInvocations = 0;   // drained entries folded into a draw that was not their
                                         // own -- always <= drainedInvocations
    uint64_t pendingInvocations = 0;     // recorded and not yet drained
    uint64_t mailboxContention = 0;      // publishes that lost the seqlock to a concurrent one:
                                         // the entry is still counted, only its detail is dropped
    uint64_t mailboxTornReads = 0;       // reads that never saw a stable record ★ must stay 0
    uint64_t wrongThreadDrains = 0;      // drain attempts off the owner thread ★ must stay 0
    Invocation lastInvocation;           // the last record the owner thread read whole
    struct SpaceChange {
        const void* gameSpace = nullptr;  // the space the change was TO
        // The two checks THEMSELVES, not their raw fields: on the first tick of a space whose
        // header is not mapped yet both answer Unreadable over a zeroed TableRaw, and a line
        // carrying only the numbers cannot be told from a genuinely empty readable space. That
        // first tick is precisely what the epoch is about, so the verdict travels with the fields.
        tableguard::TableCheck cs;         // its component-system table at +0x420, as first seen
        tableguard::TableCheck rec;        // its update-record table, as first seen
        int64_t ourIndex = -1;             // our record already in it (-1 = no): a stale block
        uint64_t invocationsAtChange = 0;
    } lastSpaceChange;
};

// What the callback should do when it runs. Called on the engine main thread, inside the open GUI
// frame. Installed once, before Register.
// One draw function serves both sites, so SetDrawFn wires the row-2.5 detour as well as this
// fallback and the two can never be given different panels.
using DrawFn = inframe::DrawFn;
void SetDrawFn(DrawFn);

// Call from the stage-0 tick with TickInfo::netGame. Registers on the first tick that has a game
// space, and again if the game space pointer changes -- registration dies with the space, so a
// level change needs a new one. Cheap and idempotent on every other tick.
void OnTick(void* netGame, uint32_t currentThreadId);

// ★ Opens the panel. This -- the UI module's own open/close state -- is what gates the draw, and
// band-3 registration is NOT (AUDIT_F3I F3I-4): if the engine hook never lands the owner still gets
// the viewer, drawn on every owner tick, and Status says separately that the compositing slot was
// never confirmed. Stamps the measurement window's baseline on the closed -> open transition; a
// second call while already open is a no-op.
void Enable();

// Closes the panel and marks the callback inert. The engine keeps calling it; it returns
// immediately.
void Disable();

Report Status_();

// A single sentence naming the state and, when nothing is drawing, the candidate causes. Never
// empty, and never "unavailable" on its own.
//
// ★ FIXED GRAMMAR FOR THE OBSERVER. The line
// ALWAYS ends with exactly these two groups, in this order, separated by one space:
//
//     " [ticks owner=<decimal> draws=<decimal>]"
//     " [inframe entries=<d> draws=<d> refused=<d> nobyte=<d> nolatch=<d> noctx=<d> facade=<d>"
//     " closed=<d> fallback=<d> installed=<0|1>]"
//     " [stage0 frameopen samples=<d> nonzero=<d> lasttick=<d>]"
//     " [threads owner=<decimal> callback=<decimal>]"
//
// `ticks owner` is Report::ownerTicks -- every OnTick on the owner thread -- and `ticks draws` is
// Report::ownerDraws, the owner ticks the panel was drawn on. That is the counter this group has
// always carried and the split at AUD-ARCH F-B did not move it: `draws=` is the panel-on-screen
// count, NOT the band-3-consumed subset (Report::marshalledDraws). They are the numerator and the
// denominator of ARCH-13 s2.3's coverage rule (`drawsAfter >= ownerTicksAfter - 1`), which is a
// DIFFERENCE across a stamp, so a reader that wants the row samples this line twice and
// subtracts; the line itself carries the running totals and never a difference. Both are unsigned
// decimal with no separators and no padding, and neither is ever omitted. The threads group stays
// LAST so a parser anchored on its tail keeps working.
//
// ★ THE `inframe` GROUP IS NEW AND IT IS THE ONE THAT MEANS `EMBEDDED` (ARCH-15 S4.2/S4.3). It is
// inserted BETWEEN the two existing groups precisely so the threads tail anchor still holds. Every
// field maps 1:1 onto src/ui/inframe.h's Counters plus the install flag, in that fixed order, and
// EVERY ONE IS PRINTED EVERY TIME including when it is 0 -- `refused=0` is "asked, none" and a
// missing `refused=` is "not asked", which is the distinction the whole row exists to carry.
//
//   entries    frameSiteEntries      draws     inFrameDraws        refused   refusedDraws
//   nobyte     refusedNoFrameByte    nolatch   refusedNoLatch      noctx     refusedNoContext
//   facade     refusedByFacade       closed    panelClosedEntries  fallback  fallbackEntries
//   installed  1 when the row-2.5 detour is armed, 0 otherwise
//
// ** THE `stage0` GROUP (ARCH-15 S7) IS ITS OWN GROUP, NOT A WIDENING OF `inframe`. W2-R matches
// the inframe group field for field (sscanf, ten fields, ending `installed=%d]`), so a tenth-and-
// a-half field there would break that parse silently; this one is appended after it and before the
// threads tail, which stays last. `samples` is the owner ticks the byte could be READ on, `nonzero`
// the ones it was set on -- ARCH-15 S1.4 predicts 0 forever -- and `lasttick` the owner-tick index
// of the last such sample, 0 when there has been none. A non-zero `nonzero` also prepends a loud
// sentence to the diagnostic: it falsifies the premise the site was chosen on.
//
// `draws=` inside this group is NOT the same counter as `draws=` in the ticks group. The ticks one
// is ownerDraws, the fallback's; this one is inFrameDraws, leg 3. A parser must key on the group.
//
// naming both thread ids: `owner` is the thread our stage-0 tick ran on (the only thread the draw
// is allowed on) and `callback` is the thread the engine last dispatched the band-3 record on, 0
// if it never has. Both already existed on the Report and neither reached an artifact, so the
// DIRECTION of an off-thread dispatch could not be read back from a launch. W2-R's detail field
// carries this line verbatim; the suffix is the last 40-ish bytes of it and is parseable on its
// own with the pattern above.
const char* Diagnostic();

void ResetForTest();

// ** THE PUBLISHED SUFFIX'S FORMAT AND ITS DECLARED BUDGET (L5-RUN-5 G1). Test-only, and the
// reason they exist is that a test which sizes a buffer against ITS OWN copy of the format
// certifies whatever the format has since become. The budget test formats the string this returns.
const char* PublishedSuffixFormatForTest();
size_t PublishedSuffixBudgetForTest();
size_t PublishedLineBudgetForTest();

// The two engine routines TryRegister calls, in the shapes the engine gives them. A test stands
// them in with SetEngineForTest so the whole path from OnTick to the record scan runs against a
// synthetic space; only the prologue guards and the address resolution are bypassed, because
// neither exists outside echovr.exe. Both null (the reset state) restores the real resolution.
using FnLookupBySymbol = void* (*)(void* gameSpace, uint64_t symbol);
using FnRegisterUpdate = void (*)(void* gameSpace, void* cs, const void* pmf24,
                                  const char* phaseName, const char* csName, uint16_t orderKey,
                                  uint16_t tiebreak, const void* deps, uint16_t schemeIdx);
void SetEngineForTest(FnLookupBySymbol lookup, FnRegisterUpdate registrar);

// One bounded seqlock read of the mailbox. False means no stable record was seen -- which is the
// answer while a publish is in flight, and the reason a torn record can never be reported as a
// whole one. A test uses this with SetPublishHookForTest below to drive that window on purpose.
bool ReadInvocationForTest(Invocation* out);

// Called by the callback's publish while the record is HALF WRITTEN and the seqlock is odd. This
// is the only way to stand inside that window deterministically, and without it the seqlock is a
// mechanism no test can red. Null (the reset state) is the shipping behaviour.
void SetPublishHookForTest(void (*hook)());

// Called by a mailbox READ after it has copied the payload and before it re-checks the sequence.
// The seqlock has two guards -- the entry check that the sequence is even, and this completion
// re-check that it has not moved -- and a hook that stands inside the publish window only ever
// exercises the first (AUDIT_F3E F3E-3, whose mutant on the re-check survived the whole suite).
// A hook that publishes here advances the sequence EVEN -> EVEN+2 between the reader's two reads,
// which is the in-vivo tear: a writer that starts and finishes inside the read. Null (the reset
// state) is the shipping behaviour.
void SetReadHookForTest(void (*hook)());

}  // namespace csregister
}  // namespace ui
}  // namespace replay
