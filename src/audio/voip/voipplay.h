#pragma once

// W3-C. Replaying recorded voice through the engine's own receiver.
//
// ---------------------------------------------------------------------------------------------
// ★ THE VOICE SECTION IS READ BY ITS OWN READER, AND THAT IS THE FORMAT'S SHAPE RATHER THAN A
// WORKAROUND. S_VOIP lives in kSectionVoip precisely so it can be read, and STRIPPED, without
// touching the core section -- `echodemo strip --voip` rewrites a file with the section gone and
// the rest byte-identical. A reader that only ever asks for kSectionVoip therefore cannot disturb
// the state window, cannot be disturbed by it, and reads nothing at all from a stripped file.
//
// ★ VOICE IS NOT STATE, SO IT IS NOT SAMPLED. Every other stream answers "what was the value at
// time T"; a codec frame answers nothing at an arbitrary playhead. It is delivered forward, in
// order, at 50 packets a second, or it is not delivered. Advance() therefore hands over the
// records in (from, to] and refuses everything else -- reverse, scrub, and any rate that is not
// 1.0 -- with a counter for each refusal rather than a silence nobody can attribute.
//
// ★ AND THE RECORDED TALKER KEY IS NEVER REPLAYED. See voipsite.h: VOIP.RECEIVE selects the
// receiver instance from the handle's low word alone and validates no generation, so a recorded
// handle replayed into a later session succeeds into whoever now holds that pool slot. Every frame
// re-derives its handle from the LIVE tables, keyed on the recording's durable user slot; the
// recorded value's high half survives only as a diagnostic.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include "audio/voip/voipsite.h"
#include "audio/voipformat.h"
#include "capture/capture_types.h"
#include "foundation/bitio/bitio.h"
#include "foundation/demofile/demofile.h"

namespace replay {

class ILog;

namespace audio {
namespace voip {

// ★ Totals, and one counter per disposition. "no voice came out" has eleven distinct causes here
// and a single `injected` count would make all of them look the same; the shipped answer to "why is
// my replay silent" has to name one of these.
struct VoipPlayStats {
    uint64_t recordsRead = 0;         // decoded out of the file
    uint64_t injected = 0;            // handed to VOIP.RECEIVE with its gates open
    uint64_t skippedRate = 0;         // the transport is not at 1.0x forward
    uint64_t skippedNoStep = 0;       // paused, or the playhead did not move forward
    uint64_t refusedNoFile = 0;       // Load was never called, or the file has no voice section
    uint64_t refusedNoSite = 0;       // the receiver component system did not resolve
    // ★ ENGINE LOOKUPS, NOT FRAMES. refusedNoSite counts frames that produced no voice; this counts
    // the times we actually asked the engine. The two being equal is the defect this counter exists
    // to make visible: one CS.LOOKUP_BY_SYMBOL per frame is an engine table sort per frame on our
    // tick (ARCH-10 S7), and nothing in the frame counters can tell that apart from a cached answer.
    uint64_t resolveAttempts = 0;
    uint64_t refusedNoTalker = 0;     // the recorded speaker has no live receiver instance
    uint64_t refusedNotSeated = 0;    // the recorded speaker has no entrant in this session yet
    uint64_t refusedGate = 0;        // VOIP.RECEIVE would have returned at one of its own gates
    uint64_t refusedUnresolved = 0;   // the record carries kUnresolvedSpeaker
    uint64_t refusedOversize = 0;     // the record's length is past kMaxPayload
    uint64_t refusedBuild = 0;        // VOIP.RECEIVE does not hold the recorded prologue
    uint64_t refusedInjector = 0;     // the injection contract itself refused the arguments
    uint64_t chaptersLoaded = 0;
    uint64_t chaptersRefused = 0;     // a voice chapter larger than the buffer; never half-loaded
    uint64_t recordsMalformed = 0;    // a length that does not frame a VoipRec plus its payload
    uint64_t discontinuities = 0;
    // ★ Every voice block decoded is verified against the exact bit length the encoder declared, and
    // both halves are counted. A player that decodes blocks and verifies none is a player with the
    // check switched off, and the two numbers agreeing is what says it is on.
    uint32_t blocksVerified = 0;
    uint32_t blocksBitMismatch = 0;
    // Binding. `bound` is what we own and must release; `alreadyBound` is what the session owned
    // and we must NOT touch -- releasing one of those poisons the pool for live play, which is
    // worse than a replay with no voice.
    uint32_t bound = 0;
    uint32_t alreadyBound = 0;
    uint32_t claimRefusedUncontained = 0;  // livesafety::Feature::VoipTalkerSlots said no
    uint32_t bindRefused = 0;
    uint32_t released = 0;
    uint32_t releaseFailed = 0;
    // Slots abandoned because the session they belonged to is gone. Calling Disconnect on a
    // component system from a torn-down gamespace is a write through a freed pointer, so these are
    // dropped deliberately -- and counted, because "we let go of four slots" is a fact about the
    // run and not an absence of one.
    uint32_t abandonedOnSessionChange = 0;
    // The subset of the line above that ONLY the identity re-ask could see: every term of the space
    // key read identical and the engine still answered a different receiver, an unreadable space or
    // a table it would have had to sort. Counted apart because it is the only evidence that
    // SameReceiverStillLive did any work -- the key's own terms cannot produce it (AUD-F3K F3K-1).
    uint32_t abandonedOnIdentity = 0;
    // ★ THE SLOTS THE POOL RE-CHECK SPARED (AUD-F3L F3L-1). The receiver was the one we bound and
    // the write was still refused, because the pool slot our cached index names now carries somebody
    // else's SR15NetUserId. Folded into releaseFailed these would read as a leak; they are the
    // opposite -- a live talker in the owner's real match who was not kicked out of voice. This is
    // the only counter that can say the second half of the identity re-ask did any work.
    uint32_t abandonedOnPool = 0;
    // ★ THE RECOVERABLE REFUSAL, KEPT APART FROM THE PERMANENT ONES (AUD-F3L F3L-2). SortPending is
    // transient by construction -- the next lookup anyone makes clears bit 0 -- so a teardown that
    // meets it is told to come back, not to give up. This counts the ones that still ran out of
    // attempts; `releaseDeferred` counts the ones that were parked, `released` the ones a retry
    // then redeemed. A single abandon number would make "the table was busy for a moment" and "the
    // receiver has been freed" look like the same fact.
    uint32_t abandonedOnSortPending = 0;
    uint32_t releaseDeferred = 0;      // teardowns parked for a later tick
    uint32_t releaseRetries = 0;       // tick attempts spent on a parked teardown
    // ★ PARKED CLAIMS THE LIVE ONE TOOK BACK (AUD-F3M F3M-1). Parking leaves the instance
    // CONNECTED, so the next capture's first frame for that talker is handed the SAME instance and
    // answers AlreadyBound -- and the retry would then disconnect the voice that capture is routing.
    // This counts the parked COPIES dropped because ownership moved back to the live claim.
    // Deliberately NOT a subset of abandonedOnSessionChange: nothing was let go of and nothing
    // leaked, the slot is simply released by the new capture's teardown instead of by the old
    // capture's retry.
    uint32_t abandonedOnRebind = 0;
    // Slots dropped WITHOUT any engine call because the process is terminating (AUD-F3L F3L-3).
    // Not a leak and not a failure: nothing will ever read that pool again.
    uint32_t abandonedAtTermination = 0;
    // Receivers let go of because the space key moved under them -- including a game space rebuilt
    // at the SAME address, which the pointer terms alone cannot see (AUD-F3D F3D-2). Counted apart
    // from the frame counters because "we dropped a receiver" and "this frame had none" are
    // different facts and only the first says the epoch term did any work.
    uint32_t sitesDroppedOnEpoch = 0;
    Bind lastBind = Bind::NoSite;

    // The most recent reason, so a log line can say WHICH rather than how many.
    Resolve lastResolve = Resolve::Ok;
    Gate lastGate = Gate::Open;
};

class VoipPlayer {
public:
    VoipPlayer() = default;
    // Nothing live is in hand at destruction, so the last claim is abandoned rather than
    // written through a receiver whose space may already have gone (AUD-F3I F3I-1). This is the
    // termination form for the same reason: it is the one that makes no engine call at all, and a
    // destructor is never a place to enter somebody else's component system.
    ~VoipPlayer() { UnloadAtTermination(); }
    VoipPlayer(const VoipPlayer&) = delete;
    VoipPlayer& operator=(const VoipPlayer&) = delete;

    // Opens the demo's voice section. Allocates its chapter buffer here, once, off the tick.
    // False when the file will not open; a file with no voice section opens and plays nothing,
    // which is a different answer and is reported as such by HasVoice().
    // liveNetGame is the caller's CURRENT netGame, used to release anything the previous load
    // still owns; nullptr when the caller has none in hand.
    bool Load(const wchar_t* path, ILog* log, void* liveNetGame);
    // liveNetGame IS THE CALLER'S CURRENT ONE, NOT THE ONE WE BOUND (AUD-F3I F3I-1). Neither
    // teardown path goes through a tick, so nothing re-read through the bound pointer can have
    // moved. nullptr means "cannot ask", which compares as a change and abandons.
    void Unload(void* liveNetGame);

    // ★ THE TEARDOWN THAT TOUCHES THE ENGINE WITH NOTHING (AUD-F3L F3L-3). VoipShutdown is reached
    // ONLY through modreg::ShutdownAll(), which no shipping path calls since ENG-EXIT-FIX (Detach
    // only flushes the log at termination). It is written for the place ShutdownAll used to run:
    // the loader lock at DLL_PROCESS_DETACH, every other thread already killed wherever it stood.
    // A lookup that sorts an engine table, or a Disconnect into a
    // half-torn-down receiver, is illegal there -- and pointless, because nothing will read that
    // pool again. So this asks the session for nothing, enters the engine nowhere, drops the claim
    // and counts it. Unload(liveNetGame) stays the form for every caller that has a next tick.
    void UnloadAtTermination();

    // Retries a teardown parked because the receiver's table was mid-registration. Called once per
    // tick, above the load, by the module; a no-op when nothing is parked.
    void RetryPendingRelease(void* liveNetGame);
    bool Loaded() const { return loaded_; }
    bool HasVoice() const { return voiceChapters_ != 0; }

    // Injects every record in (the previous toUs, toUs], in file order. The first call establishes
    // the span and delivers nothing.
    //
    // ★★★ THERE IS NO `fromUs` PARAMETER, AND ITS ABSENCE IS THE FIX FOR A REAL DEFECT.
    //
    // This used to take one, and compared it against the player's own `lastToUs_` to detect a jump.
    // The only caller that ever existed passed its own mirror of that value -- `Advance(last, now)`
    // then `last = now` -- so the comparison was `x != x` on every call, forever, and the whole
    // discontinuity path was unreachable in production. Two packets found it independently. The
    // consequences were exactly what the comment on OnDiscontinuity warns about: a backwards seek
    // never re-seated the cursor, and the cached component-system pointer was never invalidated.
    //
    // A guard whose precondition is "the caller must not mirror my state" is not a guard, it is a
    // convention -- so the parameter is gone rather than documented. The player owns both ends of
    // the span, and the broken call is now unwritable rather than merely discouraged.
    size_t Advance(int64_t toUs, float rate, void* netGame);

    // Drops the cursor and the cached receiver. The next Advance re-seats both.
    void OnDiscontinuity();

    // A recording names each speaker by the user slot it held in the CAPTURING session. The entrant
    // replaying that player holds whatever slot THIS session gave it -- the viewer occupies one, so
    // every recorded slot N plays as live N+1 in an emulated join. Speaking through the recorded
    // slot unmapped puts each voice in the next player's head and the first one in the viewer's.
    // False means the player is not seated yet; that frame is refused and counted.
    using LiveSlotMap = bool (*)(uint16_t recordedSlot, uint16_t* liveSlot, void* ctx);
    // Null means the recorded slot IS the live slot: a session nobody was re-seated in.
    void SetLiveSlotMap(LiveSlotMap map, void* ctx) {
        liveSlotMap_ = map;
        liveSlotCtx_ = ctx;
    }

    const VoipPlayStats& Stats() const { return stats_; }

    // One line naming what playback is actually doing, for the module's log and the UI. Never
    // silent: it says "no voice section", "receiver did not resolve: <reason>", "gate: <gate>" or
    // the injected count.
    const char* StatusLine(char* buf, size_t cap) const;

    // Test seam: drives the injection decision for one record against a caller-supplied receiver
    // and entry point, with no file, no reader and no game.
    struct InjectOutcome {
        bool injected = false;
        Resolve resolve = Resolve::Ok;
        Gate gate = Gate::Open;
        uint32_t handle = 0;
    };
    static InjectOutcome InjectOneForTest(const VoipSite& site, const void* provider,
                                          void* receiveVoip, const capture::VoipRec& rec,
                                          const void* payload);

    // ★ Substitutes the ONE thing an offline test cannot have -- a mapped VOIP.RECEIVE -- and
    // nothing else. Every other step of Advance still runs: the chapter read, the span, the rate
    // gate, the resolution, the gates and InjectVoip itself. A seam that skipped the injection
    // branch would leave the branch that ships untested, which is the point of putting it here
    // rather than adding a dry-run mode.
    static void SetEntryPointForTest(void* receiveVoip, const void* provider);

private:
    size_t VoiceChapterAt(int64_t tUs) const;
    bool EnsureChapter(int64_t tUs);
    size_t DeliverThrough(int64_t toUs, bool realTime, void* entry, const void* provider,
                          void* netGame);
    void ReseatCursor(int64_t afterUs);
    bool NextRecord(capture::VoipRec* rec, const uint8_t** payload);
    bool ResolveDue(const VoipSpaceKey& key, int64_t toUs) const;
    void ClearChapter(size_t index);
    void VerifyBlock();
    void* EntryPoint();

    // One voice chapter is about two seconds. Sixteen talkers at fifty packets a second with a
    // 120-byte frame is about 435 KB of that, so a megabyte is headroom rather than a guess -- and
    // a chapter that does not fit is REFUSED WHOLE and counted, because half a chapter of voice is
    // half the speakers in the room going silent for two seconds with nothing said about it.
    // ★ The forward-jump threshold, derived from the stream's own packet rate rather than picked.
    // Twenty-five packets is half a second: thirty frames at 60 fps, far past any hitch that is not
    // a stall, and two orders below any seek. Its exact value is deliberately NOT load-bearing --
    // both outcomes are safe, because classifying a span as a jump re-seats to the new playhead and
    // classifying it as continuous delivers it. Being slightly wrong costs a few dropped frames,
    // never a backlog and never a stale pointer.
    static constexpr int64_t kMaxContiguousPackets = 25;
    static constexpr int64_t kMaxContiguousSpanUs =
        (1000000 / kVoipPacketsPerSecond) * kMaxContiguousPackets;

    static constexpr size_t kChapterBufferBytes = 1u << 20;
    static constexpr size_t kNoChapter = static_cast<size_t>(-1);

    DemoReader reader_;
    bool loaded_ = false;
    size_t voiceChapters_ = 0;

    uint8_t* chapter_ = nullptr;
    size_t chapterBytes_ = 0;
    uint32_t chapterRecords_ = 0;
    size_t chapterIndex_ = kNoChapter;

    bitio::BitReader cursor_{nullptr, 0};
    uint32_t consumed_ = 0;
    // ★ A stream block is [u32 exact bit length][packed records]. A BitReader built on
    // StreamView::records directly starts mid-header and the first decode refuses -- the defect
    // W2-B's round-trip caught one layer up. The prefix width is the codec's constant, cited.
    uint32_t declaredBits_ = 0;
    bool blockVerified_ = false;

    // One decoded record held back because its stamp is past the current span.
    bool pendingValid_ = false;
    capture::VoipRec pending_{};
    uint8_t pendingPayload_[kMaxPayload] = {};

    int64_t lastToUs_ = 0;
    bool haveSpan_ = false;
    LiveSlotMap liveSlotMap_ = nullptr;
    void* liveSlotCtx_ = nullptr;

    VoipSite site_;
    // The space epoch `site_` was resolved against. A held receiver is only served while the key
    // still reads the same, which is what makes a same-address rebuild drop it rather than keep
    // handing out a component system from the freed image.
    VoipSpaceKey siteKey_;

    // ★★★ A FAILED RESOLUTION IS NOT RETRIED PER FRAME, AND THAT IS A CORRECTNESS RULE RATHER THAN
    // A SAVING. CS.LOOKUP_BY_SYMBOL sorts the gamespace's component-system table IN PLACE ON THE
    // CALLING THREAD when CS.TABLE_FLAGS bit 0 is set (facts.tsv:1086's own note to callers, quoted
    // by ARCH-10 S7), so a resolve on every tick while the site is unresolved -- which is precisely
    // the window where a session is loading -- is an O(n log n) engine sort per tick on ours. The
    // cache used to short-circuit only AFTER success, so the expensive case was the unresolved one.
    //
    // The retry waits for the only thing that can change the answer: the netGame, or the game space
    // it carries. That key costs two reads and no engine call. A late-registering component system
    // would move neither, so there is also a floor cadence.
    //
    // ★ THE FLOOR IS PLAYHEAD MICROSECONDS, AND THE RULE IS THAT IT IS ONLY EVER CONSULTED ON THE
    // 1.0x PATH. Resolution sits inside RateIsRealTime, and voice is delivered at that rate or not
    // at all, so where this constant is read the playhead IS wall time to within a frame -- 20 s of
    // playhead is 20 s of clock, and not 80 s at quarter speed, because at quarter speed nothing is
    // resolved and nothing is delivered. A paused or reversed transport reaches neither: it never
    // retries, which is right rather than tolerated, since it is not asking for voice either. A
    // seek is a discontinuity, which drops the throttle, so the first frame after any jump re-asks
    // at once. The consequence worth knowing: a transport that sat paused resumes with whatever was
    // left of the floor rather than with a fresh one. (Whether the space has been REPLACED is a
    // different question and is asked at every rate -- see the top of Advance.)
    //
    // ★ WHERE THE NUMBER COMES FROM, and it is NOT "entity_module's own deferral cadence", which is
    // what this line used to claim (AUD-F3F m1). entity_module does not defer its retry at all: it
    // re-resolves on EVERY tick (RetryResolve, entity_module.cpp:388). 20 s is the throttle on the
    // line it LOGS while still refusing -- kLogEveryDeferrals = 600 ticks (entity_module.cpp:60-62,
    // read at entity_module.cpp:403), which ARCH-10 S7 quotes as "the [entity] line itself repeats
    // every 20 s". So what 20 s is here: the span after which the engine's own diagnostics stop
    // treating "still loading" as the likely explanation. It is a CEILING on the wait and not the
    // expected one -- the receiver registers inside the load window and the site is normally found
    // on the first ask after it.
    //
    // ★ WHY IT IS 40x W3-A's FLOOR FOR THE SAME ENGINE QUESTION (AUD-F3F m2). The two are not the
    // same measurement. residuesite.h's kCsRetryBinds = 60 counts BINDS, and at least one row binds
    // per frame with many binding per frame while the battery is armed, so its denominator grows
    // with the work: its guarantee is "one table sort per 61 binds", O(1/N) against the per-row cost
    // it is amortising, and 60 binds is often well under half a second. This consumer has no such
    // denominator. There is one player, one site, and at most one ask per tick, so a floor counted
    // the same way would bound the ENGINE's cost directly: 60 ticks would be a CS.LOOKUP_BY_SYMBOL
    // in-place table sort on the tick thread every half second, for as long as a session has no
    // receiver -- which is the whole of every match played with VoIP off, not just a load window.
    // The price of the spread is stated rather than hidden: a receiver registering LATER than the
    // load window costs up to 20 s of that talker's voice, once, and stats_.refusedNoSite counts
    // every frame it cost.
    static constexpr int64_t kResolveRetryUs = 20 * 1000000;
    VoipSpaceKey refusedKey_;
    int64_t refusedAtUs_ = 0;
    bool resolveRefused_ = false;

    VoipPlayStats stats_;

    // ★ EXACTLY WHAT WE BOUND, AND NOTHING ELSE. Teardown walks this list; a slot the session
    // already owned never enters it. Sized by ENTITY.USER_SLOT_TABLE_MAX because a recording
    // cannot carry more distinct speakers than the netGame has slots.
    struct OwnedSlot {
        uint16_t poolIndex;
        uint32_t userId;
    };
    static constexpr size_t kMaxOwned = 16;
    OwnedSlot owned_[kMaxOwned] = {};
    size_t ownedCount_ = 0;
    // The site the owned slots were bound against, held separately from the working one. A
    // discontinuity drops `site_`; it must not drop our claim on slots that are still live, and
    // teardown after a seek must still release them.
    VoipSite boundSite_;
    // The epoch the owned slots were bound under. Separate from siteKey_ because the site can be
    // re-resolved (and the key re-read) many times over a binding that is still perfectly good.
    VoipSpaceKey boundKey_;
    // One attempt per user slot per load. Without it a recording whose speaker has no free
    // instance would drive VOIP.CONNECT's guard fifty times a second for the length of the file.
    uint32_t bindAttempted_ = 0;

    // ★ A TEARDOWN THAT COULD NOT RUN YET, HELD ASIDE FROM THE LIVE CLAIM (AUD-F3L F3L-2). It is a
    // separate copy and not a flag over `owned_` on purpose: Load's implicit Unload can park a
    // release and then immediately bind NEW slots, and one array would make the retry walk the new
    // capture's claim along with the old one.
    //
    // ★ BUT SEPARATE ARRAYS ARE NOT SEPARATE INSTANCES (AUD-F3M F3M-1). The parked claim leaves
    // its instances CONNECTED, so a new capture speaking for the same talker is handed the very
    // instance this copy still names: it answers AlreadyBound and binds nothing of its own. What
    // keeps the retry off it is ReclaimFromPending, which moves that entry back into `owned_` the
    // moment a frame is delivered through the instance it names.
    struct PendingRelease {
        VoipSite site;
        VoipSpaceKey key;
        OwnedSlot owned[kMaxOwned] = {};
        size_t count = 0;
        uint32_t attempts = 0;
    };
    PendingRelease pendingRelease_;
    // Bounded so a table that never settles cannot make this a per-frame lookup. Eight ticks is
    // ~160ms at the 50Hz this module runs at -- far longer than the window between a registration
    // and the next lookup that clears bit 0, and short enough that the abandon is still prompt.
    static constexpr uint32_t kMaxReleaseAttempts = 8;

    void ReleaseOwned(void* liveNetGame);
    // Walks a claim and gives the slots back, counting each disposition. Shared by the immediate
    // teardown and the deferred retry so the two cannot drift apart.
    void ReleaseSlots(const VoipSite& site, const OwnedSlot* slots, size_t count);
    void AbandonPending(uint32_t* reason);
    void DropBuffers();
    bool TryBind(uint32_t liveUserId);
    // Takes a parked entry back into the live claim when the new capture turns out to be speaking
    // through the very instance that entry names. Ownership moves; nothing is written, nothing is
    // given up, and a run with nothing parked pays one comparison per delivered frame.
    void ReclaimFromPending(uint16_t poolIndex, uint32_t liveUserId);
    // What a replaced game space obliges us to do whatever the transport is doing. Advance runs it
    // once per tick above every early return; the rate gate below it covers delivery only.
    void ObserveSpace(const VoipSpaceKey& key);
};

// ---- module seams, implemented in voip_module.cpp --------------------------------------------
// The one line this packet needs from whoever owns the transport, mirroring the shape W2-H's
// SetAudioKeyframeIntervalUs already uses. Until it is called the module reports "no demo loaded",
// which is a state and not a silence.
void SetVoipDemoPath(const wchar_t* path);
const VoipPlayStats& PlaybackStats();
const char* PlaybackStatusLine(char* buf, size_t cap);

}  // namespace voip
}  // namespace audio
}  // namespace replay
