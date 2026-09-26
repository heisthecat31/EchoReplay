#pragma once
// THE PLAYBACK SIDE OF A "COMPONENT" EVENT -- the launch tubes and their light rigs.
//
// The recorder already stored SR15NetComponentEvent verbatim in S_EVENT (5,429 rows in the capture
// in hand, every one exactly 24 bytes).  Nothing decoded them, so nothing in the level responded
// during playback.  This file decodes those rows and hands them back to the ENGINE'S OWN receive
// handler, CR15NetGame::ComponentEventCB, with the recorded bytes verbatim.  It animates nothing,
// plays no sound and synthesises no field: the tube's own script does all of it.
//
// THE ALLOW-LIST KEY IS THE (scriptId, eventSymbol) PAIR, NOT THE scriptId.  One recorded scriptId
// covers both a safe listener and an unsafe one: 6aea99eeb2bab6c4 + 6e0a.../627e... reaches only the
// tube light rig 039b5a245c43ed2e, whose entire node set is lights, models, transforms and lists,
// while 6aea99eeb2bab6c4 + 7e0e... reaches cea83dc626035076, which forces a player's grab release
// and then re-sends to self-and-all-peers from inside the script graph, where clearing the wire's
// relay bit cannot reach it.  Filtering on symbol alone is wrong for the mirror-image reason.  Both
// halves are load-bearing; see RE-COMPONENT-EVENT-ENTRY.md S3.4a/S3.5.
//
// WHAT IS REFUSED IS COUNTED, NOT IGNORED.  Every row that is not on the list is tallied per
// (scriptId, symbol) and printed once per load, so widening the list later is an evidence-driven
// decision rather than a guess about a name.
//
// TIME AXIS.  Same as the goal feed: an S_EVENT row's stampUs is the tap's ARRIVAL axis and is not
// comparable with the playhead, so rows are placed inside their chapter's [tBegin, tEnd) by where
// their arrival stamp falls between the first and last arrival stamp of that chapter's events.  The
// resulting stamps are on the same absolute axis as the presented playhead.
//
// SCRUB.  An event is an edge with no level half at all -- there is nothing to restore, because the
// state it leaves lives inside a script graph the plugin does not touch.  So: fire only on a FORWARD
// crossing under the transport's own power, never in reverse, never on a seek (no catch-up burst),
// never while the pre-roll owns the clock, at most once per crossing.

#include <stddef.h>
#include <stdint.h>

#include "foundation/demofile/demofile.h"

namespace replay {
namespace playback {
namespace componentfeed {

inline constexpr uint32_t kBodyBytes = 24;
// Allow-listed rows only are stored.  The whole capture admits four; the cap exists so a corrupt tap
// index cannot turn the store into the payload.
inline constexpr uint32_t kMaxRows = 256;
// Distinct (scriptId, symbol) pairs the refusal table holds before it starts counting overflow.  The
// capture in hand has twenty pairs across five script ids.
inline constexpr uint32_t kMaxPairs = 64;
// The rate-limit safety net.  Today's list admits four rows in a whole file, so this can only matter
// if the list is ever widened; the remainder is carried to the next tick in order, never dropped.
inline constexpr uint32_t kMaxFiresPerTick = 8;

// One (scriptId, symbol) pair and how many rows carried it.  `allowed` is the verdict the list gave,
// so the load line can print the refusals without a second pass.
struct PairTally {
    uint64_t script_id = 0;
    uint64_t symbol = 0;
    uint32_t rows = 0;
    bool allowed = false;
};

struct ScanCensus {
    uint32_t event_rows = 0;      // S_EVENT rows walked
    uint32_t tap_chapters = 0;    // chapters whose keyframe carried a tap table naming this message
    uint32_t component_rows = 0;  // rows that were this tap's
    uint32_t rows = 0;            // allow-listed rows stored
    uint32_t refused_rows = 0;    // the right tap, refused by the decoder (short body)
    uint32_t denied_rows = 0;     // decoded, not on the allow-list
    uint32_t dropped_rows = 0;    // over the row cap
    uint32_t placed_rows = 0;     // rows the arrival interpolation moved off the chapter begin
    uint32_t unpaired_rows = 0;   // a return sequence with no launch, or a launch with no return
    uint32_t pair_count = 0;
    uint32_t pair_overflow = 0;
    uint16_t tap = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    const char* first_drop = nullptr;  // program-lifetime; names the bound that was hit
    PairTally pairs[kMaxPairs];
};

// One decoded row, kept alongside its verbatim bytes: the fields are for the log, the allow-list and
// the tests; the bytes are what the engine is handed.
struct Row {
    uint64_t t_us = 0;
    uint64_t symbol = 0;
    uint64_t script_id = 0;
    uint16_t handle = 0;
    uint16_t flags = 0;
    uint32_t param = 0;
    uint8_t body[kBodyBytes] = {};
};

// The verdict the (scriptId, symbol) list gives a row.  `what` is program-lifetime and goes in the
// log; `resets` marks the rig's own return-to-base event, which is the half a launch must be paired
// with before either is allowed to fire.
struct Verdict {
    bool allowed = false;
    bool resets = false;
    const char* what = "not on the allow-list";
};
Verdict Classify(uint64_t scriptId, uint64_t symbol);

class RecordedEvents {
public:
    void Reset();

    // `arrivalUs` is the row's S_EVENT stamp; EndChapter turns it into a playhead-axis time.
    bool Offer(uint64_t arrivalUs, const void* body, uint32_t len, ScanCensus& census);

    void BeginChapter();
    void EndChapter(uint64_t tBeginUs, uint64_t tEndUs, uint64_t firstArrivalUs,
                    uint64_t lastArrivalUs, ScanCensus& census);
    // Refuses a launch with no later return on the same handle, and a return with no earlier launch:
    // the light rig walks a segment list, so half a sequence strands it lit and parented.  Run once,
    // after the last chapter.
    void DropUnpairedRows(ScanCensus& census);

    uint32_t Count() const { return count_; }
    const Row& At(uint32_t i) const { return rows_[i < count_ ? i : 0]; }
    uint32_t CountInSpan(int64_t beginUs, int64_t endUs) const;

    // The first row strictly after `tUs`, as an index; Count() when there is none.
    uint32_t FirstAfter(int64_t tUs) const;

private:
    Row rows_[kMaxRows];
    uint32_t count_ = 0;
    uint32_t chapter_first_ = 0;
};

// Walks every chapter's S_EVENT and fills `into` from the component-event tap, resolving the type
// index out of the FILE'S OWN keyframe tap table -- never a compiled-in index.  Returns rows stored.
uint32_t ScanRecordedEvents(IDemoReader& reader, RecordedEvents& into, ScanCensus& census);

RecordedEvents& PlaybackRecordedEvents();
ScanCensus& PlaybackEventCensus();

// One line per refused (scriptId, symbol) pair, once per load.  This is the evidence a later
// widening of the allow-list has to be argued from, so it is printed even when nothing fires.
void LogPairCensus(const ScanCensus& census);

// ---------------------------------------------------------------------------------------------
// The feed.  Everything below reaches the engine.
// ---------------------------------------------------------------------------------------------

// CR15NetGame::ComponentEventCB(netGame, &smsg).
using EntryFn = char (*)(void* netGame, void* smsg);
// CR15NetIdCS::Actor(&out16, handle) -- the engine's own index/generation/live check.
using ResolveActorFn = void* (*)(void* out16, uint16_t handle);

struct Handlers {
    EntryFn entry = nullptr;
    ResolveActorFn resolve = nullptr;
    // The engine's local peer id, LOADED not baked: the handler stamps SComponentEvent+36 with
    // 0xFFFFFFFE when the SMsg's sender equals it, which is what a locally-raised event looks like.
    const uint64_t* self_peer = nullptr;
    // The CR15NetIdCS pool pointer, for the probe line's idx/gen/count/live only.
    const uint64_t* pool = nullptr;
};
// Test seam: fakes that record what they were handed.  Null restores the bind against the mapped
// image, which on a machine with no game resolves to nothing and refuses by name.
void InjectHandlersForTest(const Handlers* h);

// THE SCRIPT-ID EQUALITY, AS A SEAM.  A row should only reach the component the recording aimed it
// at, and the sharpest form of that is "the target's script id equals the recorded one".  The
// CR15NetIdCS pool does not carry a script id (RE S2.3), so there is no engine source for it today
// and the default resolver is null -- the check reports `unchecked` rather than passing silently.
// The equality that IS performed is the engine's own: the graph's event node matches the scriptId
// the handler carries in SComponentEvent+24, so a handle that resolved to the wrong actor produces a
// no-op rather than a misdelivery.  Inject a resolver and a mismatch refuses the raise.
using ScriptIdOfActorFn = bool (*)(void* ctx, uint64_t space, uint64_t actorRef, uint64_t& scriptId);
void InjectScriptIdResolverForTest(ScriptIdOfActorFn fn, void* ctx);

struct ApplyCensus {
    uint32_t fires = 0;              // forward crossings that reached the engine
    uint32_t seeks_suppressed = 0;   // jumps that re-based the cursor without firing
    uint32_t holds = 0;              // ticks skipped because the pre-roll owns the clock
    uint32_t reverse_skipped = 0;    // sweeps where the playhead moved backwards
    uint32_t refused_handle = 0;     // the engine's resolve said the handle is not valid here
    uint32_t refused_script_id = 0;  // the injected resolver disagreed with the recorded script id
    uint32_t refused_identity = 0;   // the handle resolved to a different actor than it first did
    uint32_t refused_pair = 0;       // reached the dispatcher without being on the allow-list
    uint32_t unpaired_skipped = 0;   // a return whose launch this pass never fired
    uint32_t deferred = 0;           // rows carried to the next tick by the rate limit
    uint32_t reentrant = 0;          // the shared engine-call latch was already held
    uint32_t faulted = 0;
    const char* refusal = nullptr;  // program-lifetime; why the last sweep did nothing
};

void Arm(bool on);
bool Armed();
// Reporting only: the fire lines speak the playhead's axis rather than the file's absolute one.
void SetPlayheadOriginUs(int64_t originUs);

// A discontinuity the user caused.  The next sweep re-bases onto the new playhead and fires nothing:
// a tube that launches at a seek destination is worse than a tube that stays still.
void NoteSeek();

// Forgets what was fired and where the playhead was, without touching the engine.
void Forget();

// One stage-3 tail pass, on the engine tick thread -- and only there: the raise takes the event DB's
// lock through a per-thread counter read out of engine TLS, so no other thread may call this.
// `tUs` is the presented playhead, absolute.  `hold` is true while the pre-roll owns the clock.
void Sweep(void* netGame, int64_t tUs, bool hold);

const ApplyCensus& Census();
void ResetCensusForTest();

}  // namespace componentfeed
}  // namespace playback
}  // namespace replay
