#include "audio/voip/voipplay.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include "audio/memprobe.h"
#include "audio/voip/voip_facts.h"
#include "audio/voip/voipcapture.h"
#include "audio/voipformat.h"
#include "bootstrap/buildguard.h"
#include "bootstrap/livesafety.h"
#include "bootstrap/services.h"
#include "capture/streamcodec.h"
#include "foundation/demofile/demotypes.h"
#include "playback/core/windowbuild.h"

namespace replay {
namespace audio {
namespace voip {
namespace {

// Voice is a real-time codec stream. Half speed does not make half-speed speech, it makes a
// decoder fed at the wrong rate, and reverse has no meaning at all. The window is tight rather
// than generous because the transport's own 1.0 is exact.
constexpr float kRateTolerance = 0.01f;

void* g_testEntry = nullptr;
const void* g_testProvider = nullptr;

bool RateIsRealTime(float rate) {
    const float d = rate - 1.0f;
    return d > -kRateTolerance && d < kRateTolerance;
}


// Whether the game space our pool slots were bound against has been REPLACED, as opposed to merely
// looking different this frame. Any key move is enough to re-ask the engine -- that costs a lookup.
// Throwing the owned slots away costs more: they stay Connect'd in a live session with nobody left
// to release them, so only a term that names a different space does it. A space field that goes
// unmapped and comes back is the same space and must not.
bool SpaceReplaced(const VoipSpaceKey& bound, const VoipSpaceKey& now) {
    if (bound.netGame != now.netGame) {
        return true;
    }
    if (!(bound.bind == now.bind)) {
        return true;
    }
    if (bound.readable && now.readable && bound.space != now.space) {
        return true;
    }
    // ★ THE TERM THE EPOCH CANNOT SUPPLY (AUD-F3F F3F-3). entity_module advances the bind epoch only
    // when its digest of the system table's contents differs, so a space rebuilt at the SAME address
    // whose table reconstructs identically moves nothing above this line -- while owned_[].poolIndex
    // was issued by ONE receiver instance's pool and is garbage against the next (engine/entity.h:
    // 231-233). The table block's base is what a rebuild reallocates. Asked only when BOTH keys read
    // it, the same rule the space pointer follows.
    //
    // ★ WHICH OF THE TWO MISTAKES THIS TERM CHOOSES. It also moves when the table merely GROWS under
    // the same space, and that is a false positive: we drop the claim WITHOUT Disconnect, so at most
    // kMaxOwned pool slots stay connected carrying the recorded talker's id, and the next TryBind for
    // that speaker reads AlreadyBound and the voice keeps flowing. The other direction hands
    // Disconnect a component system that has been freed. A bounded slot leak is not a memory write.
    return bound.tableRead && now.tableRead && bound.tableBase != now.tableBase;
}

}  // namespace

bool VoipPlayer::Load(const wchar_t* path, ILog* log, void* liveNetGame) {
    Unload(liveNetGame);
    if (path == nullptr) {
        return false;
    }
    if (!reader_.Open(path)) {
        if (log != nullptr) {
            log->Write(LogLevel::Warn, "voip", "voice section not opened: %s",
                       reader_.RefusalReason() != nullptr ? reader_.RefusalReason() : "unknown");
        }
        return false;
    }
    chapter_ = new (std::nothrow) uint8_t[kChapterBufferBytes];
    if (chapter_ == nullptr) {
        return false;
    }
    loaded_ = true;
    voiceChapters_ = 0;
    for (size_t i = 0; i < reader_.ChapterCount(); ++i) {
        if (reader_.ChapterAt(i).section == kSectionVoip) {
            ++voiceChapters_;
        }
    }
    if (log != nullptr) {
        log->Write(LogLevel::Info, "voip", "%zu voice chapter(s) in %ls",
                   voiceChapters_, path);
    }
    return true;
}

// ★ RELEASES ONLY WHAT WE BOUND. A slot the session already owned was never recorded here, so it
// cannot be released by us -- a replay that hands the owner's next real match a pool of
// disconnected talkers is worse than one with no voice.
void VoipPlayer::ReleaseOwned(void* liveNetGame) {
    // ★ THE LAST GUARD, AND ON THIS PATH THE ONLY ONE (AUD-F3F F3F-4). Unload() reaches here from
    // VoipShutdown and from SetVoipDemoPath(nullptr) -- the browser picking another capture -- and
    // neither of those goes through a tick, so the observation Advance makes cannot be relied on to
    // have run since the space was replaced. `boundSite_.Valid()` below is a non-null pointer test
    // and a freed receiver is still a non-null pointer, so it can never see this.
    //
    // AND IT IS ASKED THROUGH THE CALLER'S LIVE netGame, NOT THE ONE WE BOUND (AUD-F3I F3I-1). The
    // live netGame is current without a tick because the caller has just asked the session for it;
    // nullptr is "cannot ask" and a probe that refuses leaves the terms unread, and both of those
    // compare as a change and abandon.
    //
    // ★ BUT THE KEY IS THE EARLY-OUT, NOT THE GUARD (AUD-F3K F3K-1). When the live netGame IS the
    // bound one -- the ordinary teardown -- every term of the key is read back through the same
    // pointer, so a system table rebuilt in place moves NOTHING: not the space pointer, not the
    // block base, and not the bind epoch, which is a snapshot a tick maintains and no tick has
    // happened. That is entity.h:243's third case and it reads identical in all four terms. So
    // before any write we re-establish the receiver's identity against the live table -- one engine
    // lookup per teardown, stated as this packet's invariant in voipsite.h -- and release ONLY if
    // the engine hands back the pointer we bound. The key still runs first and still short-circuits
    // it: where the key already says the space is gone, entering the engine through it is the last
    // thing we want to do.
    if (ownedCount_ != 0) {
        bool retryable = false;
        const bool keyMoved = SpaceReplaced(boundKey_, VoipSpaceKeyOf(liveNetGame));
        const bool sameReceiver =
            !keyMoved && SameReceiverStillLive(liveNetGame, boundSite_, &retryable);
        if (!sameReceiver) {
            // ★ ONE OF THESE FOUR REFUSALS MEANS "NOT YET" (AUD-F3L F3L-2). SortPending says a
            // component system registered since the last lookup and the next lookup clears it --
            // the space is alive, the netGame is ours, the key is unmoved, and the receiver is very
            // probably still the one we bound. Abandoning there is permanent on a transient fact,
            // and BindTalker answers AlreadyBound for a slot we let go of, so nothing ever reclaims
            // it: up to kMaxOwned receiver instances stay Connected for the life of that gamespace
            // and a real player then meets NoFreeInstance. Both callers that can reach this with a
            // next tick park the claim instead; the terminating one never gets here.
            if (retryable && pendingRelease_.count == 0) {
                pendingRelease_.site = boundSite_;
                pendingRelease_.key = boundKey_;
                pendingRelease_.count = ownedCount_;
                pendingRelease_.attempts = 0;
                for (size_t i = 0; i < ownedCount_; ++i) {
                    pendingRelease_.owned[i] = owned_[i];
                }
                stats_.releaseDeferred += static_cast<uint32_t>(ownedCount_);
            } else if (retryable) {
                // A claim is already parked. Taking the new one would drop the old, and there is no
                // ordering in which the second is more redeemable than the first, so the newcomer is
                // the one abandoned -- promptly and under its own name rather than silently.
                stats_.abandonedOnSortPending += static_cast<uint32_t>(ownedCount_);
                stats_.abandonedOnSessionChange += static_cast<uint32_t>(ownedCount_);
            } else {
                if (!keyMoved) {
                    stats_.abandonedOnIdentity += static_cast<uint32_t>(ownedCount_);
                }
                stats_.abandonedOnSessionChange += static_cast<uint32_t>(ownedCount_);
            }
            ownedCount_ = 0;
        }
    }
    ReleaseSlots(boundSite_, owned_, ownedCount_);
    ownedCount_ = 0;
    bindAttempted_ = 0;
    boundSite_ = VoipSite{};
    boundKey_ = VoipSpaceKey{};
}

void VoipPlayer::ReleaseSlots(const VoipSite& site, const OwnedSlot* slots, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (!site.Valid()) {
            ++stats_.releaseFailed;
            continue;
        }
        switch (UnbindTalker(site, slots[i].poolIndex, slots[i].userId)) {
            case Release::Released:
                ++stats_.released;
                break;
            case Release::PoolMoved:
                // NOT a leak, and the reason it has its own counter: the pool slot changed hands
                // while our receiver stayed put, so the talker at that index is somebody else's --
                // very possibly a live player in the owner's real match. No write was made.
                ++stats_.abandonedOnPool;
                break;
            default:
                // Counted, never swallowed: a slot we could not give back is exactly the leak this
                // whole list exists to prevent, and silence about it would hide the one case that
                // reaches the owner's live session.
                ++stats_.releaseFailed;
                break;
        }
    }
}

// `reason` is the subset counter, or null when the total is the only honest thing to say.
void VoipPlayer::AbandonPending(uint32_t* reason) {
    if (reason != nullptr) {
        *reason += static_cast<uint32_t>(pendingRelease_.count);
    }
    stats_.abandonedOnSessionChange += static_cast<uint32_t>(pendingRelease_.count);
    pendingRelease_ = PendingRelease{};
}

void VoipPlayer::RetryPendingRelease(void* liveNetGame) {
    if (pendingRelease_.count == 0) {
        return;
    }
    ++pendingRelease_.attempts;
    stats_.releaseRetries += 1;
    if (SpaceReplaced(pendingRelease_.key, VoipSpaceKeyOf(liveNetGame))) {
        // The space went while the claim was parked. Releasing through it now is the write through
        // a freed receiver this whole path exists to avoid.
        AbandonPending(nullptr);
        return;
    }
    bool retryable = false;
    if (!SameReceiverStillLive(liveNetGame, pendingRelease_.site, &retryable)) {
        if (retryable && pendingRelease_.attempts < kMaxReleaseAttempts) {
            return;
        }
        AbandonPending(retryable ? &stats_.abandonedOnSortPending : &stats_.abandonedOnIdentity);
        return;
    }
    ReleaseSlots(pendingRelease_.site, pendingRelease_.owned, pendingRelease_.count);
    pendingRelease_ = PendingRelease{};
}

void VoipPlayer::UnloadAtTermination() {
    stats_.abandonedAtTermination +=
        static_cast<uint32_t>(ownedCount_) + static_cast<uint32_t>(pendingRelease_.count);
    ownedCount_ = 0;
    bindAttempted_ = 0;
    boundSite_ = VoipSite{};
    boundKey_ = VoipSpaceKey{};
    pendingRelease_ = PendingRelease{};
    DropBuffers();
}

// ★ THE PARKED CLAIM AND THE NEW CAPTURE CAN NAME ONE INSTANCE (AUD-F3M F3M-1). Parking leaves the
// instance CONNECTED under the live user id, so the next capture's frames for that talker resolve
// straight to it -- no bind is attempted, nothing is recorded, and when the sort settles the retry
// disconnects the instance that capture is speaking through. Every term the retry checks agrees,
// because everything it checks IS still true: same receiver, same slot, same talker. Only ownership
// moved, so ownership is what has to be corrected.
//
// The parked copy is also what makes this safe to record. A live instance we cannot prove we bound
// is the SESSION's -- very possibly a real player's -- and recording that as ours would disconnect
// them at teardown. A matching parked entry is the only proof we have, and nothing else here
// confers ownership.
void VoipPlayer::ReclaimFromPending(uint16_t poolIndex, uint32_t liveUserId) {
    if (pendingRelease_.count == 0 || pendingRelease_.site.cs != site_.cs ||
        pendingRelease_.site.netGame != site_.netGame) {
        // A claim parked against a different receiver names a different pool, so its indices mean
        // nothing here.
        return;
    }
    for (size_t i = 0; i < pendingRelease_.count; ++i) {
        if (pendingRelease_.owned[i].poolIndex != poolIndex ||
            pendingRelease_.owned[i].userId != liveUserId) {
            continue;
        }
        if (ownedCount_ == 0) {
            boundSite_ = site_;
            boundKey_ = siteKey_;
        }
        if (ownedCount_ < kMaxOwned) {
            owned_[ownedCount_++] = pendingRelease_.owned[i];
        }
        // If the live claim is already full the slot cannot be recorded and stays connected until
        // the space goes. The parked entry is dropped anyway, deliberately: cutting the voice this
        // capture is routing right now is the worse of the two outcomes.
        for (size_t j = i + 1; j < pendingRelease_.count; ++j) {
            pendingRelease_.owned[j - 1] = pendingRelease_.owned[j];
        }
        --pendingRelease_.count;
        ++stats_.abandonedOnRebind;
        if (pendingRelease_.count == 0) {
            pendingRelease_ = PendingRelease{};
        }
        return;
    }
}

// Demand-driven: a speaker is bound the first time one of their frames finds no live instance.
// That needs no pre-scan of the file and handles a talker who first speaks halfway through it.
bool VoipPlayer::TryBind(uint32_t liveUserId) {
    // The one place a talker slot is CLAIMED. VOIP.CONNECT builds a decoder in the live receiver's
    // pool, and the lookup on the way there sorts the gamespace table in place -- both engine
    // writes, in a pool whose other entries are real people in the owner's match. Releases stay
    // ungated on purpose: owned_ can only hold what this claim put there, so an uncontained
    // process has nothing to give back and the balance is preserved.
    if (!livesafety::Allowed(livesafety::Feature::VoipTalkerSlots)) {
        ++stats_.claimRefusedUncontained;
        return false;
    }
    const uint16_t slot = SlotOfUserId(liveUserId);
    if (slot >= 32u || (bindAttempted_ & (1u << slot)) != 0) {
        return false;
    }
    bindAttempted_ |= 1u << slot;

    uint16_t poolIndex = 0;
    stats_.lastBind = BindTalker(site_, liveUserId, &poolIndex);
    switch (stats_.lastBind) {
        case Bind::AlreadyBound:
            ++stats_.alreadyBound;
            return true;
        case Bind::Bound:
            ++stats_.bound;
            boundSite_ = site_;
            boundKey_ = siteKey_;
            if (ownedCount_ < kMaxOwned) {
                owned_[ownedCount_++] = OwnedSlot{poolIndex, liveUserId};
            } else {
                // We bound it and cannot record that we did, so we could never release it.
                // Give it straight back rather than leak it.
                const OwnedSlot giveBack{poolIndex, liveUserId};
                ReleaseSlots(boundSite_, &giveBack, 1);
                return false;
            }
            return true;
        default:
            ++stats_.bindRefused;
            return false;
    }
}

void VoipPlayer::Unload(void* liveNetGame) {
    ReleaseOwned(liveNetGame);
    DropBuffers();
}

// The half of Unload that touches nothing outside this object, so the terminating teardown can
// share it without sharing the engine call.
void VoipPlayer::DropBuffers() {
    delete[] chapter_;
    chapter_ = nullptr;
    chapterBytes_ = 0;
    chapterRecords_ = 0;
    chapterIndex_ = kNoChapter;
    consumed_ = 0;
    pendingValid_ = false;
    haveSpan_ = false;
    loaded_ = false;
    voiceChapters_ = 0;
    site_ = VoipSite{};
    siteKey_ = VoipSpaceKey{};
}

// Everything a replaced game space obliges us to do that has nothing to do with delivering audio:
// let go of pool slots issued by a receiver that no longer exists, and stop holding a resolved site
// whose key has moved. Split out of Advance's real-time block so it can run at any rate -- see the
// note at the top of Advance.
void VoipPlayer::ObserveSpace(const VoipSpaceKey& key) {
    // The receiver system our slots were bound against no longer exists. Let go of the claim
    // WITHOUT calling Disconnect: that call would be a write through a freed component system.
    if (ownedCount_ != 0 && SpaceReplaced(boundKey_, key)) {
        stats_.abandonedOnSessionChange += static_cast<uint32_t>(ownedCount_);
        ownedCount_ = 0;
        bindAttempted_ = 0;
        boundSite_ = VoipSite{};
        boundKey_ = VoipSpaceKey{};
    }
    if (site_.Valid() && !(siteKey_ == key)) {
        // Dropped HERE rather than left for the re-resolve to overwrite. If the re-resolve is
        // throttled, or refuses, this frame must have NO receiver rather than the one whose space
        // has just gone -- overwriting on success only would keep serving it meanwhile.
        ++stats_.sitesDroppedOnEpoch;
        site_ = VoipSite{};
    }
}

void VoipPlayer::OnDiscontinuity() {
    ++stats_.discontinuities;
    pendingValid_ = false;
    haveSpan_ = false;
    chapterIndex_ = kNoChapter;
    // ★ The receiver is dropped too, not just the cursor. A seek can cross a session boundary in a
    // -replayviewer flow, and a cached component system from the previous gamespace is a pointer
    // into freed memory that would still look plausible. INV-P: invalidate, never update.
    site_ = VoipSite{};
    siteKey_ = VoipSpaceKey{};
    // The site is gone, so the throttle's subject is gone with it: a seek must be able to resolve
    // again at once, and it cannot re-enter the engine per frame because the next attempt either
    // succeeds or re-arms the throttle.
    resolveRefused_ = false;
}

// True when asking the engine again could produce a different answer, or when enough playhead has
// passed that a component system registering late is worth one more call. Reads two fields through
// the memory probe; calls nothing.
bool VoipPlayer::ResolveDue(const VoipSpaceKey& key, int64_t toUs) const {
    if (!resolveRefused_ || !(key == refusedKey_)) {
        return true;
    }
    // Backwards cannot reach here -- a reverse span is a discontinuity, which clears the throttle
    // -- but a negative elapsed is treated as due rather than as a wait of two hundred centuries.
    const int64_t since = toUs - refusedAtUs_;
    return since < 0 || since >= kResolveRetryUs;
}

size_t VoipPlayer::VoiceChapterAt(int64_t tUs) const {
    if (!loaded_) {
        return kNoChapter;
    }
    const size_t at = reader_.FindChapter(tUs < 0 ? 0 : static_cast<uint64_t>(tUs), kSectionVoip);
    return at < reader_.ChapterCount() ? at : kNoChapter;
}

bool VoipPlayer::EnsureChapter(int64_t tUs) {
    const size_t want = VoiceChapterAt(tUs);
    if (want == kNoChapter) {
        return false;
    }
    if (want == chapterIndex_) {
        return true;
    }
    DecodedChapter dc{};
    if (!reader_.DecodeChapter(want, dc)) {
        return false;
    }
    const StreamView* view = dc.Find(kStreamVoip);
    if (view == nullptr) {
        ClearChapter(want);
        return true;
    }
    if (view->bytes > kChapterBufferBytes || view->bytes <= capture::kBlockPrefixBytes) {
        // Refused whole. Half a chapter of voice is half the speakers in the room going silent for
        // two seconds with nothing said about it.
        if (view->bytes > kChapterBufferBytes) {
            ++stats_.chaptersRefused;
        }
        ClearChapter(want);
        return true;
    }
    // Copied rather than borrowed. A StreamView survives exactly one further DecodeChapter on the
    // same reader; holding one across frames is the use-after-free that does not fail on two.
    memcpy(chapter_, view->records, view->bytes);
    memcpy(&declaredBits_, chapter_, sizeof declaredBits_);
    chapterBytes_ = view->bytes - capture::kBlockPrefixBytes;
    chapterRecords_ = view->recordCount;
    chapterIndex_ = want;
    cursor_ = bitio::BitReader(chapter_ + capture::kBlockPrefixBytes, chapterBytes_);
    consumed_ = 0;
    pendingValid_ = false;
    blockVerified_ = false;
    ++stats_.chaptersLoaded;
    return true;
}

// A chapter with no voice block, or one this player will not load. Named rather than repeated so
// the two callers cannot drift into leaving different halves of the cursor behind.
void VoipPlayer::ClearChapter(size_t index) {
    chapterIndex_ = index;
    chapterBytes_ = 0;
    chapterRecords_ = 0;
    declaredBits_ = 0;
    blockVerified_ = true;
    cursor_ = bitio::BitReader(nullptr, 0);
    consumed_ = 0;
    pendingValid_ = false;
}

// Called once a chapter's records are exhausted: the decoder must have consumed exactly the bit
// count the encoder declared. Reusing W2-B's verdict rather than restating its rule.
void VoipPlayer::VerifyBlock() {
    if (blockVerified_ || consumed_ < chapterRecords_) {
        return;
    }
    blockVerified_ = true;
    if (playback::VerifyBlockConsumed(declaredBits_, cursor_.BitsRead(), cursor_.Overrun()).ok) {
        ++stats_.blocksVerified;
    } else {
        ++stats_.blocksBitMismatch;
    }
}

bool VoipPlayer::NextRecord(capture::VoipRec* rec, const uint8_t** payload) {
    if (consumed_ >= chapterRecords_ || chapterBytes_ == 0) {
        return false;
    }
    static thread_local uint8_t buf[sizeof(capture::VoipRec) + kMaxPayload];
    uint16_t n = 0;
    if (!capture::DecodeRawRecord(cursor_, buf, sizeof buf, n)) {
        // A stream that will not decode is finished, not skipped: the framing is sequential and
        // there is no resynchronisation point to skip to.
        consumed_ = chapterRecords_;
        ++stats_.recordsMalformed;
        return false;
    }
    ++consumed_;
    if (n < sizeof(capture::VoipRec)) {
        ++stats_.recordsMalformed;
        return false;
    }
    memcpy(rec, buf, sizeof(capture::VoipRec));
    if (static_cast<size_t>(rec->bodyBytes) + sizeof(capture::VoipRec) != n) {
        ++stats_.recordsMalformed;
        return false;
    }
    *payload = buf + sizeof(capture::VoipRec);
    ++stats_.recordsRead;
    return true;
}

void VoipPlayer::ReseatCursor(int64_t afterUs) {
    if (chapterBytes_ == 0) {
        return;
    }
    cursor_ = bitio::BitReader(chapter_ + capture::kBlockPrefixBytes, chapterBytes_);
    consumed_ = 0;
    // A re-seat restarts the SAME reader at bit zero, so a later pass to the end of the block still
    // consumes exactly what the encoder declared. Leaving this true was a real defect: it switched
    // the verification off for every chapter, and the counter that says so read zero while the
    // round trip otherwise looked perfect.
    blockVerified_ = false;
    pendingValid_ = false;
    capture::VoipRec rec{};
    const uint8_t* payload = nullptr;
    while (NextRecord(&rec, &payload)) {
        if (static_cast<int64_t>(rec.tUs) > afterUs) {
            pending_ = rec;
            const uint16_t n = rec.bodyBytes <= kMaxPayload ? rec.bodyBytes
                                                            : static_cast<uint16_t>(kMaxPayload);
            memcpy(pendingPayload_, payload, n);
            pendingValid_ = true;
            return;
        }
    }
}

void VoipPlayer::SetEntryPointForTest(void* receiveVoip, const void* provider) {
    g_testEntry = receiveVoip;
    g_testProvider = provider;
}

void* VoipPlayer::EntryPoint() {
    if (g_testEntry != nullptr) {
        return g_testEntry;
    }
    // ★★ THROUGH OUR OWN TRAMPOLINE WHENEVER THE CAPTURE HOOK EXISTS. Once armed, the site address
    // IS our detour: injecting through it would re-enter the tap and record every replayed frame as
    // though the network had delivered it. In the one session that both records and replays -- the
    // spectator flow -- that writes the replay's own voice back into the recording.
    void* original = ReceiveVoipOriginal();
    if (original != nullptr) {
        return original;
    }
    if (!buildguard::VerifySite(kVoipReceiveFactId, voip_facts::kReceiveVa,
                                voip_facts::kReceivePrologue, 20)) {
        return nullptr;
    }
    return const_cast<void*>(buildguard::MappedAddress(voip_facts::kReceiveVa));
}

VoipPlayer::InjectOutcome VoipPlayer::InjectOneForTest(const VoipSite& site, const void* provider,
                                                       void* receiveVoip,
                                                       const capture::VoipRec& rec,
                                                       const void* payload) {
    InjectOutcome out;
    if (rec.speakerSlot == kUnresolvedSpeaker) {
        out.resolve = Resolve::NoSuchTalker;
        return out;
    }
    uint32_t liveId = 0;
    if (!UserIdOf(site.netGame, SlotOfUserId(rec.speakerSlot), &liveId, &out.resolve)) {
        return out;
    }
    const uint8_t* ci = nullptr;
    if (!HandleOf(site, liveId, &out.handle, &ci, &out.resolve)) {
        return out;
    }
    out.gate = InspectGates(site, ci, provider);
    if (out.gate != Gate::Open) {
        return out;
    }
    VoipInjector injector;
    injector.receiver = site.cs;
    injector.receiveVoip = reinterpret_cast<decltype(injector.receiveVoip)>(receiveVoip);
    out.injected = InjectVoip(injector, out.handle, rec.frameCtr, payload, rec.bodyBytes);
    return out;
}

size_t VoipPlayer::Advance(int64_t toUs, float rate, void* netGame) {
    if (!loaded_ || chapter_ == nullptr) {
        ++stats_.refusedNoFile;
        return 0;
    }

    // ★ THE SPACE KEY IS BUILT ONCE PER TICK, AT EVERY RATE, ABOVE EVERY EARLY RETURN (AUD-F3F
    // F3F-4). It used to be built inside `if (realTime)`, which sits BELOW the `toUs <= fromUs`
    // return -- so while the transport was paused, scrubbing, or at any rate but 1.0x, a game space
    // replaced underneath us was never observed at all and the owned pool slots went on pointing
    // into a receiver that had gone. Rate gates what we DELIVER; it must not gate what we NOTICE.
    // voip_module's VoipTick calls this every engine tick with no rate gate of its own, so this line
    // IS the per-tick check.
    //
    // Built once and reused below, because building it twice is two Readable() probes that a
    // protection change landing between them can make disagree (AUD-F3D F3D-3 next door).
    const VoipSpaceKey key = VoipSpaceKeyOf(netGame);
    ObserveSpace(key);

    // The first call establishes the span and delivers nothing. It is its own state, not a
    // zero-length step: folding it into skippedNoStep would put "we have only just started" in the
    // same counter as "the playhead is paused".
    if (!haveSpan_) {
        haveSpan_ = true;
        lastToUs_ = toUs;
        return 0;
    }

#if defined(W3C_MUTANT_DISCONTINUITY)
    // The mutant restores the defect: the guard never fires. tests/voip/check.ps1 builds this and
    // REQUIRES the backwards-seek arm to go red, because an arm that cannot fail is not a control
    // -- and "no discontinuity was reported" passes just as well when the code is unreachable as
    // when it is correct, which is exactly how the original survived.
#else
    // ★ BACKWARDS IS EXACT AND NEEDS NO THRESHOLD, and it is the case that matters: seeking back is
    // the first thing anyone does. A forward jump needs one, because a span nobody bounded would
    // hand the engine every frame it covers in a single tick -- the whole rest of the file at once.
    //
    // ★★ AND THE JUMP ITSELF DELIVERS NOTHING. Returning here is what makes the span the seek
    // crossed disappear rather than get played: carrying on with the old span start would reseat
    // the cursor to where the playhead USED to be and then inject everything between, which is
    // precisely the backlog dump the threshold exists to prevent. The next call plays forward from
    // the new position with a cursor and a receiver that were both dropped here.
    if (toUs < lastToUs_ || toUs - lastToUs_ > kMaxContiguousSpanUs) {
        OnDiscontinuity();
        haveSpan_ = true;   // OnDiscontinuity clears it; the span restarts at the new playhead
        lastToUs_ = toUs;
        return 0;
    }
#endif

    const int64_t fromUs = lastToUs_;
    lastToUs_ = toUs;

    if (toUs <= fromUs) {
        ++stats_.skippedNoStep;
        return 0;
    }

    const size_t chapterAtTo = VoiceChapterAt(toUs);
    if (chapterAtTo == kNoChapter) {
        ++stats_.refusedNoFile;
        return 0;
    }

    const bool realTime = RateIsRealTime(rate);

    // The receiver is resolved once per session, not once per frame, and re-resolved whenever the
    // netGame it was resolved against changes -- which is what makes a session boundary invalidate
    // it rather than leave a stale pointer that still dereferences.
    if (realTime) {
        // The key built at the top of this function is the one used here: the abandon and the site
        // drop it already drove are the rate-independent half, and this is the half that is not --
        // RESOLVING is 1.0x only, because CS.LOOKUP_BY_SYMBOL sorts the table in place and a rate
        // that delivers no voice must not pay for it. `netGame` alone could not see any of this: a
        // game space torn down and rebuilt at the SAME address reads identically in
        // {netGame, space, readable} (AUD-F3D F3D-2), which is what the key's other terms are for.
        if (!site_.Valid()) {
            if (ResolveDue(key, toUs)) {
                ++stats_.resolveAttempts;
                // ★ The reason is recorded on the ATTEMPT, so every distinct refusal -- and the
                // NoGameSpace / GameSpaceUnreadable split in particular -- still reaches the status
                // line once per epoch. What the throttle suppresses is the repetition, not the
                // answer: a suppressed frame leaves the reason from this attempt standing.
                resolveRefused_ = !ResolveVoipSite(netGame, &site_, &stats_.lastResolve);
                siteKey_ = key;
                refusedKey_ = key;
                refusedAtUs_ = toUs;
            }
            if (!site_.Valid()) {
                ++stats_.refusedNoSite;
                // The cursor still advances below: falling behind and then dumping a backlog when
                // the receiver appears would inject a second of speech in one frame.
            }
        }
    }

    void* entry = realTime && site_.Valid() ? EntryPoint() : nullptr;
    if (realTime && site_.Valid() && entry == nullptr) {
        ++stats_.refusedBuild;
    }
    const void* provider = nullptr;
    if (entry != nullptr) {
        if (g_testProvider != nullptr) {
            provider = g_testProvider;
        } else {
            const void* gate = buildguard::MappedAddress(voip_facts::kProviderGateVa);
            if (gate != nullptr && Readable(gate, sizeof(void*))) {
                provider = *reinterpret_cast<void* const*>(gate);
            }
        }
    }

    // ★ A SPAN THAT CROSSES INTO THE NEXT CHAPTER FINISHES THIS ONE FIRST. Switching straight to the
    // chapter that holds toUs dropped this chapter's records in (fromUs, its end] -- one frame per
    // talker at most boundaries at 45-90 Hz ticks, and the engine's receiver then holds that talker
    // for a missing sequence number (up to half a second of silence and a burst) every two seconds.
    size_t injected = 0;
    if (chapterIndex_ != kNoChapter && chapterIndex_ != chapterAtTo) {
        injected += DeliverThrough(toUs, realTime, entry, provider, netGame);
    }
    if (!EnsureChapter(toUs)) {
        ++stats_.refusedNoFile;
        return injected;
    }
    if (!pendingValid_ && consumed_ == 0) {
        ReseatCursor(fromUs);
    }
    return injected + DeliverThrough(toUs, realTime, entry, provider, netGame);
}

size_t VoipPlayer::DeliverThrough(int64_t toUs, bool realTime, void* entry, const void* provider,
                                  void* netGame) {
    size_t injected = 0;
    for (;;) {
        capture::VoipRec rec{};
        const uint8_t* payload = nullptr;
        if (pendingValid_) {
            rec = pending_;
            payload = pendingPayload_;
        } else if (!NextRecord(&rec, &payload)) {
            VerifyBlock();
            break;
        }
        const int64_t t = static_cast<int64_t>(rec.tUs);
        if (t > toUs) {
            if (!pendingValid_) {
                pending_ = rec;
                const uint16_t n = rec.bodyBytes <= kMaxPayload
                                       ? rec.bodyBytes
                                       : static_cast<uint16_t>(kMaxPayload);
                memcpy(pendingPayload_, payload, n);
                pendingValid_ = true;
            }
            break;
        }
        pendingValid_ = false;

        if (!realTime) {
            ++stats_.skippedRate;
            continue;
        }
        if (entry == nullptr) {
            continue;   // already counted as refusedNoSite or refusedBuild
        }
        if (rec.bodyBytes > kMaxPayload) {
            ++stats_.refusedOversize;
            continue;
        }
        if (rec.speakerSlot == kUnresolvedSpeaker) {
            ++stats_.refusedUnresolved;
            continue;
        }
        uint16_t liveSlot = SlotOfUserId(rec.speakerSlot);
        if (liveSlotMap_ != nullptr && !liveSlotMap_(liveSlot, &liveSlot, liveSlotCtx_)) {
            ++stats_.refusedNotSeated;
            continue;
        }
        uint32_t liveId = 0;
        if (!UserIdOf(netGame, liveSlot, &liveId, &stats_.lastResolve)) {
            ++stats_.refusedNoTalker;
            continue;
        }
        uint32_t handle = 0;
        const uint8_t* ci = nullptr;
        if (!HandleOf(site_, liveId, &handle, &ci, &stats_.lastResolve)) {
            // No live instance carries this speaker. Bind one through the engine's own Connect --
            // once per slot per load -- and retry this frame rather than the next, so the first
            // word a talker says is not the one that pays for the binding.
            if (!TryBind(liveId) ||
                !HandleOf(site_, liveId, &handle, &ci, &stats_.lastResolve)) {
                ++stats_.refusedNoTalker;
                continue;
            }
        }
        // ★ AND THE INSTANCE WE ARE ABOUT TO SPEAK THROUGH MAY BE ONE A PARKED RELEASE STILL
        // HOLDS (AUD-F3M F3M-1). This is the point the hazard actually passes through, and not
        // TryBind: an instance left connected by a park RESOLVES, so HandleOf answers on the line
        // above and no bind is attempted at all. Delivering through it is what makes it this
        // capture's, so ownership moves here. Free when nothing is parked, which is every tick of
        // an ordinary run.
        ReclaimFromPending(static_cast<uint16_t>(handle), liveId);
        stats_.lastGate = InspectGates(site_, ci, provider);
        if (stats_.lastGate != Gate::Open) {
            ++stats_.refusedGate;
            continue;
        }
        VoipInjector injector;
        injector.receiver = site_.cs;
        injector.receiveVoip = reinterpret_cast<decltype(injector.receiveVoip)>(entry);
        if (InjectVoip(injector, handle, rec.frameCtr, payload, rec.bodyBytes)) {
            ++stats_.injected;
            ++injected;
        } else {
            // The contract refused arguments this loop already vetted, so this counter existing
            // and staying at zero is the statement; folding it into refusedOversize would make a
            // real disagreement look like a long packet.
            ++stats_.refusedInjector;
        }
    }
    return injected;
}

const char* VoipPlayer::StatusLine(char* buf, size_t cap) const {
    if (buf == nullptr || cap == 0) {
        return "";
    }
    if (!loaded_) {
        snprintf(buf, cap, "no demo loaded");
    } else if (voiceChapters_ == 0) {
        snprintf(buf, cap, "this recording carries no voice section");
    } else if (stats_.refusedNoSite != 0 && stats_.injected == 0) {
        snprintf(buf, cap, "receiver did not resolve: %s", ResolveText(stats_.lastResolve));
    } else if (stats_.refusedGate != 0 && stats_.injected == 0) {
        snprintf(buf, cap, "the engine would refuse every frame -- %s", GateText(stats_.lastGate));
    } else if (stats_.refusedNotSeated != 0 && stats_.injected == 0) {
        snprintf(buf, cap, "no recorded speaker is seated in this session yet");
    } else if (stats_.refusedNoTalker != 0 && stats_.injected == 0) {
        snprintf(buf, cap, "no live receiver for any recorded speaker: %s",
                 ResolveText(stats_.lastResolve));
    } else {
        snprintf(buf, cap, "%llu of %llu voice frames injected",
                 static_cast<unsigned long long>(stats_.injected),
                 static_cast<unsigned long long>(stats_.recordsRead));
    }
    return buf;
}

}  // namespace voip
}  // namespace audio
}  // namespace replay
