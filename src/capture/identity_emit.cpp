#include "capture/identity_emit.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "engine/identity_access.h"   // the Refusal enumerators; nothing here calls into the engine

namespace replay {
namespace capture {

const char* IdentityEmitRefusalText(IdentityEmitRefusal r) {
    switch (r) {
        case IdentityEmitRefusal::None:           return "none";
        case IdentityEmitRefusal::NoTapType:      return "IdentityEmitNoTapType";
        case IdentityEmitRefusal::StreamDisabled: return "IdentityEmitStreamDisabled";
        case IdentityEmitRefusal::ScratchTooSmall:return "IdentityEmitScratchTooSmall";
        case IdentityEmitRefusal::WriterRefused:  return "IdentityEmitWriterRefused";
        case IdentityEmitRefusal::SinkRefused:    return "IdentityEmitSinkRefused";
        case IdentityEmitRefusal::Count:          break;
    }
    return "IdentityEmitRefusalUnnamed";
}

const char* ProfileRefusalKindText(ProfileRefusalKind k) {
    switch (k) {
        case ProfileRefusalKind::Deterministic: return "DETERMINISTIC";
        case ProfileRefusalKind::Transient:     return "TRANSIENT";
        case ProfileRefusalKind::NoSource:      return "NO-SOURCE";
    }
    return "UNNAMED";
}

// No `default:` and C4062 promoted to an error, so a Refusal added to the engine's enum stops this
// file compiling until somebody decides which policy it gets.
#pragma warning(push)
#pragma warning(error : 4062)
ProfileRefusalKind ClassifyProfileRefusal(identity_access::Refusal r) {
    using identity_access::Refusal;
    switch (r) {
        // The serialiser met a node THIS BUILD'S read API cannot express. The same document walks
        // to the same node every time.
        case Refusal::JsonDepthLimit:
        case Refusal::JsonNodeLimit:
        case Refusal::JsonCycle:
        case Refusal::JsonArrayNotWalkable:
        case Refusal::JsonNumberNotExact:
        case Refusal::JsonUnknownKind:
        case Refusal::JsonStringNotUtf8:
        // Longer than the file's compile-time cap: the same document sizes the same every time, and
        // the sizing IS a whole-document walk.
        case Refusal::ShortBuffer:
            return ProfileRefusalKind::Deterministic;

        // Not yet, not readable right now, or not a statement about the document at all.
        case Refusal::None:
        case Refusal::NoNetGame:
        case Refusal::BuildMismatch:
        case Refusal::SlotOutOfRange:
        case Refusal::NoSuchEntrant:
        case Refusal::ReplayBand:
        case Refusal::EmptyProfile:
        // RECORDER-3 F1. Both of the second storage's own words are "not yet": the lobby record is
        // written by a message, and a message that has not arrived is the ordinary state at t=0.
        case Refusal::EntrantRecordUnreadable:
        case Refusal::EntrantRecordEmptyNode:
        case Refusal::NoLoadoutInstance:
        case Refusal::NoActorComponent:
        case Refusal::NotAuthoritative:
        case Refusal::ShortInstance:
        case Refusal::NoSlotNameTable:
        case Refusal::SlotNameTooLong:
        case Refusal::NameNotResolved:
        case Refusal::NoReleasePath:
        case Refusal::RosterUnreadable:
        case Refusal::NoJsonWriter:
        case Refusal::JsonNoRoot:
        case Refusal::JsonNodeUnreadable:
        case Refusal::JsonChildCountDisagrees:
        // Also carries entity_json's StringTooLong, which IS the document's; the merged word cannot
        // tell them apart, so it takes the policy that loses nothing.
        case Refusal::JsonStringUnreadable:
        case Refusal::JsonTreeChanged:
        case Refusal::JsonNoSuchKey:
        case Refusal::StatsNoFillMask:
        case Refusal::StatsUnreadable:
        case Refusal::EmoteNoSystem:
        case Refusal::EmoteUnreadable:
        case Refusal::EmoteVtableUnreadable:
        case Refusal::EmoteNoSeam:
        case Refusal::ProfileReadOnly:
        case Refusal::ProfileNotReadBack:
        case Refusal::LoadoutsUnreadable:
        case Refusal::ProfileNotAttached:
        case Refusal::TeamNotApplied:
        case Refusal::TeamNotRecorded:
        case Refusal::EngineCallReentrant:
        case Refusal::AppliedSlotsUnstable:
        // ENG-NAMES-2's five. All playback-side words this capture path cannot raise, and the rule
        // above is that everything which is not a property of the DOCUMENT is Transient -- a
        // misjudged word costs a re-ask and never an entrant's profile.
        case Refusal::ProfileNotRecorded:
        case Refusal::ProfileNotParsed:
        case Refusal::LoadoutNotRecorded:
        case Refusal::SpectatorRow:
        case Refusal::ReasonNotRecorded:
        case Refusal::LoadoutStepOff:
        // ENG-LEAVE-1's five. All of them are answers ReleaseReplayUser gives on the playback side;
        // this capture path never releases an entrant and cannot raise any of them.
        case Refusal::ReleaseNotContained:
        case Refusal::NotOurEntrant:
        case Refusal::EntrantIsLocal:
        case Refusal::ReleaseCallRefused:
        case Refusal::EntrantStillSeated:
            return ProfileRefusalKind::Transient;
    }
    return ProfileRefusalKind::Transient;   // a value outside the enum: a source's bug, not a verdict
}
#pragma warning(pop)

void MaskIdentifier(const char* in, char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!in || in[0] == 0) {
        // An empty name is a fact about the read, so it gets a word rather than an empty field that
        // a reader would take for a masking bug.
        _snprintf_s(out, cap, _TRUNCATE, "(unnamed)");
        return;
    }
    size_t n = 0;
    while (in[n] != 0) ++n;
    _snprintf_s(out, cap, _TRUNCATE, "%c***(%u)", in[0], unsigned(n));
}

uint64_t IdentityRowSignature(const IdentityEntrant& e, const IdentityLoadoutSlot* slots,
                              uint16_t slotCount) {
    uint64_t h = RosterFold(kRosterFoldSeed, &e, sizeof e);
    if (slots) h = RosterFold(h, slots, sizeof(IdentityLoadoutSlot) * slotCount);
    return h;
}

uint64_t IdentityEmitter::RetryNowUs() const {
    return retryClock_ ? retryClock_(retryClockCtx_) : GetTickCount64() * 1000ull;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
}

// A clock reading EARLIER than a time it stamped is not the clock that stamped it, and waiting out
// the difference can outlast the match; both waits below read that as over.
static bool WaitedUs(uint64_t nowUs, uint64_t sinceUs, uint64_t waitUs) {
#ifdef CAPCOST_MUTATE_TRUST_EARLIER_CLOCK
    return nowUs >= sinceUs + waitUs;
#else
    return nowUs < sinceUs || nowUs - sinceUs >= waitUs;
#endif
}

IdentityEmitter::Retry IdentityEmitter::RetryDue(uint16_t row, uint16_t userIndex,
                                                 const IRosterSource* src, uint64_t nowUs) const {
    if (!src || profileRefused_[row] == kNotRefused) return Retry::No;
    if (profileRefused_[row] == kRefusedNoSource) return Retry::Now;
#ifndef CAPCOST_MUTATE_IGNORE_DOC_TOKEN
    if (src->ProfileDocToken(userIndex) != docToken_[row]) return Retry::Now;
#endif
#ifdef CAPCOST_MUTATE_RETRY_EVERY_SCAN
    return Retry::Now;   // the field defect: every refusal re-offered on every scan
#else
    if (profileRefused_[row] != kRefusedTransient) return Retry::No;
    return WaitedUs(nowUs, latchedAtUs_[row], retryDelayUs_[row]) ? Retry::WhenRosterAllows
                                                                  : Retry::No;
#endif
}

bool IdentityEmitter::RosterGapOpen(uint64_t nowUs) const {
#ifdef CAPCOST_MUTATE_NO_ROSTER_GAP
    return true;
#else
    return !rosterReasked_ || WaitedUs(nowUs, rosterReaskedAtUs_, kProfileRetryRosterGapUs);
#endif
}

void IdentityEmitter::LatchRefusal(uint16_t row, ProfileRefusalKind kind, uint64_t docToken,
                                   uint64_t nowUs) {
    if (kind == ProfileRefusalKind::Transient) {
        const bool sameQuestion =
            profileRefused_[row] == kRefusedTransient && docToken_[row] == docToken;
#ifdef CAPCOST_MUTATE_NO_BACKOFF
        retryDelayUs_[row] = kProfileRetryFirstDelayUs;
#else
        retryDelayUs_[row] = sameQuestion ? retryDelayUs_[row] * 2 : kProfileRetryFirstDelayUs;
#endif
#ifndef CAPCOST_MUTATE_NO_BACKOFF_CAP
        if (retryDelayUs_[row] > kProfileRetryMaxDelayUs) {
            retryDelayUs_[row] = kProfileRetryMaxDelayUs;
        }
#endif
        latchedAtUs_[row] = nowUs;
    }
    profileRefused_[row] = kind == ProfileRefusalKind::Deterministic ? kRefusedDeterministic
                         : kind == ProfileRefusalKind::Transient     ? kRefusedTransient
                                                                     : kRefusedNoSource;
    docToken_[row] = docToken;
}

void IdentityEmitter::EmitChanged(const RosterTable& table, const IRosterSource* profileSrc,
                                  uint64_t tUs, uint64_t contentHash,
                                  const IdentityEmitScratch& scratch, uint16_t maxProfileReads,
                                  IdentityBlockSink sink, void* sinkCtx, IdentityCensusSink census,
                                  void* censusCtx, IdentityEmitReport& out) {
    out = IdentityEmitReport{};
    if (!sink) return;
    if (!scratch.block || scratch.blockCap < kIdentityBlockOneEntrantCap) {
        ++out.refusals[size_t(IdentityEmitRefusal::ScratchTooSmall)];
        return;
    }

    uint16_t profileReads = 0;
    // Whose turn this CALL has already spent. Without it a row refused in pass 0 is retryable by the
    // time pass 1 reaches it and its document is read twice in one pass -- the budget spent on the
    // question it just answered.
    uint8_t touched[kRosterMaxRows] = {};
    // Decided once per row per call, in pass 0: the token read behind it is a source call, and the
    // two passes must not disagree about a row.
    enum : uint8_t { kNotDue = 0, kDueNow, kDueBackoff };
    uint8_t retryDue[kRosterMaxRows] = {};
    const uint16_t rows = table.Count();
#ifdef CAPCOST_MUTATE_RETRY_ON_CAPTURE_AXIS
    const uint64_t nowUs = tUs;   // the open writes sim time here and the tick compares peer time
#else
    const uint64_t nowUs = RetryNowUs();
#endif
    // The one back-off re-ask this call may make: of the rows whose own back-off has run out, the
    // one refused longest ago, so a lobby sharing one refusal word takes turns instead of the low
    // rows being asked forever.
    const bool rosterGapOpen = RosterGapOpen(nowUs);
    const uint16_t kNoRow = kRosterMaxRows;
    uint16_t backoffRow = kNoRow;

    // ★ TWO PASSES, and the order is the fix for AUD-W2-A-2 F10. The per-tick document budget is one
    // read; a row whose profile the engine keeps refusing is re-offered every scan (F7), and in one
    // pass the retry at a low index spent that read before any first-time row was reached -- so a
    // persistently refused profile starved every later entrant out of the file for the whole match,
    // with `deferredToNextScan` climbing forever. Rows that have something NEW to say go first; a
    // retry only gets the read when no first-time row is waiting for it.
    for (int pass = 0; pass < 2; ++pass) {
#ifdef W2A2_MUTATE_RETRY_STARVES_BUDGET
        if (pass == 1) break;   // one pass, retries competing with first emits at their own index
#endif
        if (pass == 1 && backoffRow != kNoRow) {
            retryDue[backoffRow] = kDueBackoff;   // pass 0 counted it as waiting before the choice
            --out.unchanged;
            --out.retryWaiting;
        }
    for (uint16_t i = 0; i < rows && i < kRosterMaxRows; ++i) {
        const RosterRow& row = table.At(i);
        if (!row.active) continue;
        if (pass == 0) ++out.considered;

        IdentityEntrant fields{};
        IdentityLoadoutSlot slots[kIdentitySlotCount];
        IdentityEntrantFromRosterRow(row, fields, slots);
#ifdef W2A2_MUTATE_TRUNCATION_SILENT
        // AUD-W2-C-2 F2's defect, made explicit: the component was wider than the format and the
        // block says nothing, so twenty of twenty-one reads back as a complete loadout.
        fields.flags &= uint8_t(~kIdentityEntrantSlotsTruncated);
#endif
#ifdef W2A2_MUTATE_DROP_PAIRS
        // The item without the key it sits in: what ReadAppliedItems recorded before W2-C-2, and
        // what leaves POSITION as the whole of a recording's slot evidence.
        for (uint16_t s = 0; s < kIdentitySlotCount; ++s) slots[s].slotKey = 0;
#endif

        const uint64_t signature = IdentityRowSignature(fields, slots, kIdentitySlotCount);
        const bool contentMoved = !haveSignature_[i] || signature_[i] != signature;

        // The signature is folded BEFORE the profile flags exist, so a row whose
        // document was refused and has since become readable folds identically -- and a latched
        // ProfileRefused would stand for the rest of the match on a profile the engine now hands
        // over. So a refused row is re-offered; F10 is why it is offered SECOND.
        //
        // A refusal with no source at all is not retried until a source appears: `profileSrc` null
        // refuses every row on every scan, and re-asking a question that has no one to answer it
        // would spend the budget forever. One refusal per row, then settled.
        //
        // CAP-COST. "Re-offered" is no longer "every scan": RetryDue is the policy, and a row that is
        // refused and not due costs this fold and one token read, never a document walk.
        if (pass == 0 && !contentMoved) {
            const Retry retry = RetryDue(i, row.userIndex, profileSrc, nowUs);
            retryDue[i] = retry == Retry::Now ? kDueNow : kNotDue;
            if (retry == Retry::WhenRosterAllows && rosterGapOpen) {
#ifdef CAPCOST_MUTATE_RETRY_LOWEST_ROW_FIRST
                if (backoffRow == kNoRow) backoffRow = i;
#else
                if (backoffRow == kNoRow || latchedAtUs_[i] < latchedAtUs_[backoffRow]) {
                    backoffRow = i;
                }
#endif
            }
            if (!retryDue[i]) {
                ++out.unchanged;   // counted once, in the pass that considered it
                if (profileRefused_[i] != kNotRefused) ++out.retryWaiting;
            }
#ifdef CAPCOST_MUTATE_RETRY_TAKES_BUDGET_FIRST
            // AUD-W2-A-2 F10's defect: the re-ask spends the read at its own index, in pass 0.
            if (backoffRow == i) {
                backoffRow = kNoRow;
                retryDue[i] = kDueBackoff;
                --out.unchanged;
                --out.retryWaiting;
            } else
#endif
            continue;
        }
        const bool retryable = retryDue[i] != kNotDue;
        if (pass == 1 && (contentMoved || !retryable || touched[i])) {
            continue;   // pass 0 either handled it or deferred it; it is not a retry
        }
        touched[i] = 1;

        // THE PER-TICK BUDGET.  A profile document is a full CJson walk and this
        // runs in the armed tick's roster bracket, so one call serialises at most `maxProfileReads`
        // of them.  With a budget of one and N rows moving at once the LAST of them waits N-1 scans
        // -- N-1 seconds at the roster cadence, eleven for a full lobby, not "one cadence" (F11);
        // not deferring puts N documents on the one tick a round transition moves every entrant on,
        // the bracket costmeter.cpp records as having already closed a capture.  The signature is
        // NOT latched here, so the row is offered again next scan rather than lost.
        if (profileReads >= maxProfileReads) {
            // A retry that misses the budget is not a deferred ENTRANT -- it already has a block on
            // disk -- but it is not nothing either, and counting it nowhere left `considered` bigger
            // than the sum of the buckets.
            if (pass == 0) {
                ++out.deferred;
            } else {
                ++out.retryDeferred;
            }
            continue;
        }
        ++profileReads;
        if (retryDue[i] == kDueBackoff) {
            rosterReasked_ = true;
            rosterReaskedAtUs_ = nowUs;
        }

        // THE PROFILE DOCUMENT, read only for a row whose identity moved.  `written` is the length of
        // the WHOLE document on every path and the short arm copies nothing, so a document past the
        // file's cap is refused with its true length instead of reaching the block cut in half.
        //
        // The OUTCOME is recorded in the block.  A document the engine refused and one
        // the entrant does not have are both zero bytes, and only these two bits tell them apart.
        Blob json{nullptr, 0};
        const char* profileWhy = nullptr;
        uint32_t profileDocBytes = 0;
        bool profileRefused = false;
        identity_access::Refusal refusal = identity_access::Refusal::None;
        // Read BEFORE the ask, so the token latched beside a refusal names the document refused.
        const uint64_t docToken = profileSrc ? profileSrc->ProfileDocToken(row.userIndex) : 0;
        if (profileSrc && scratch.json && scratch.jsonCap) {
            size_t written = 0;
            bool truncated = false;
            const char* why = nullptr;
            EntrantJsonOutcome outcome{};
            if (profileSrc->EntrantJson(row.userIndex, scratch.json, scratch.jsonCap, written,
                                        truncated, why, refusal, &outcome)) {
                json.p = scratch.json;
                json.n = uint32_t(written);
                if (outcome.source == ProfileSource::EntrantRecord) {
                    fields.flags |= kIdentityEntrantProfileFromEntrantRec;
                }
                if (outcome.lossyNumber) fields.flags |= kIdentityEntrantProfileLossyNumber;
                if (everRefused_[i]) fields.flags |= kIdentityEntrantProfileSupersedes;
            } else {
                profileRefused = true;
                profileWhy = why ? why : "unnamed";
                profileDocBytes = uint32_t(written);
            }
        } else {
            profileRefused = true;
            profileWhy = profileSrc ? "no profile buffer" : "no identity source";
        }
        fields.flags |= kIdentityEntrantProfileAsked;
        ProfileRefusalKind refusalKind = ProfileRefusalKind::NoSource;
        if (profileRefused) {
            ++out.jsonRefused;
            out.lastJsonWhy = profileWhy;
            everRefused_[i] = 1;
#ifndef W2A2_MUTATE_PROFILE_REFUSAL_SILENT
            fields.flags |= kIdentityEntrantProfileRefused;
#endif
#ifdef CAPCOST_MUTATE_ALL_TRANSIENT
            if (profileSrc) refusalKind = ProfileRefusalKind::Transient;
#else
            if (profileSrc) refusalKind = ClassifyProfileRefusal(refusal);
#endif
            if (refusalSink_ && notedRefusal_[i] != uint8_t(refusal) + 1) {
                notedRefusal_[i] = uint8_t(uint8_t(refusal) + 1);
                IdentityProfileRefusalNote note{};
                note.rowIndex = i;
                note.userSlot = row.userSlot;
                note.userIndex = row.userIndex;
                note.refusal = refusal;
                note.kind = refusalKind;
                note.why = profileWhy;
                note.docBytes = profileDocBytes;
                MaskIdentifier(row.displayName, note.nameMasked, sizeof note.nameMasked);
                refusalSink_(note, refusalSinkCtx_);
            }
        } else {
            notedRefusal_[i] = 0;
        }

        // Still refused, and nothing else moved: there is no second, identical block to write.
        if (!contentMoved && profileRefused) {
            ++out.profileRetried;
            ++out.unchanged;
            LatchRefusal(i, refusalKind, docToken, nowUs);
            continue;
        }

        IdentityBlockEntrantIn in{};
        in.fields = fields;
        in.slots = slots;
        in.profileJson = json;

        IdentityBlockWriter w(scratch.block, scratch.blockCap, tUs, contentHash);
        if (!w.Add(in)) {
            ++out.refusals[size_t(IdentityEmitRefusal::WriterRefused)];
            continue;
        }
        size_t bytes = 0;
        if (!w.Finish(bytes) || bytes == 0) {
            ++out.refusals[size_t(IdentityEmitRefusal::WriterRefused)];
            continue;
        }
#ifdef W2A2_MUTATE_EMIT_V1_VERSION
        // A producer that keeps writing the old version word while writing the new records: the
        // block then claims v1's 16-byte stride over 24-byte records, so every reader of it
        // misparses -- and the slot keys it does carry are unreachable by rule.
        {
            const uint32_t v = kIdentityBlockVersionV1;
            memcpy(scratch.block + offsetof(IdentitySnapshotHeader, version), &v, sizeof v);
        }
#endif
#ifdef W2A2_MUTATE_STRIDE_FROM_SIZEOF
        // The stride published from a COMPILED sizeof of the record this build's previous version
        // had, rather than from the bytes actually written.  This is the hazard s14.8b 3b names: a
        // trailing offset derived from a sizeof lands inside the profile JSON.
        {
            const uint16_t stride = kIdentitySlotStrideV1;
            memcpy(scratch.block + offsetof(IdentitySnapshotHeader, loadoutSlotStride), &stride,
                   sizeof stride);
        }
#endif

#ifndef W2A2_MUTATE_EMIT_NEVER
        const bool pushed = sink(scratch.block, uint32_t(bytes), tUs, sinkCtx);
#else
        const bool pushed = false;   // the block is built and never leaves the process
#endif
        if (!pushed) {
            ++out.refusals[size_t(IdentityEmitRefusal::SinkRefused)];
            // The signature is NOT latched: a row whose block the ring refused is retried on the next
            // scan rather than counted as published for the rest of the match.
            continue;
        }
        signature_[i] = signature;
        haveSignature_[i] = 1;
        if (profileRefused) {
            LatchRefusal(i, refusalKind, docToken, nowUs);
        } else {
            profileRefused_[i] = kNotRefused;
        }
        ++out.emitted;

        const bool truncationKnown = (fields.flags & kIdentityEntrantSlotsKnown) != 0;
        const bool truncated = (fields.flags & kIdentityEntrantSlotsTruncated) != 0;
        if (!truncationKnown) {
            ++out.truncationUnknown;
        } else if (truncated) {
            ++out.truncated;
        }

        uint16_t withKey = 0;
        for (uint16_t s = 0; s < kIdentitySlotCount; ++s) {
            if (slots[s].slotKey != 0) ++withKey;
        }
        if (withKey == 0) ++out.keyless;

        if (census) {
            IdentityEmitCensus line{};
            line.rowIndex = i;
            line.userSlot = row.userSlot;
            line.userIndex = row.userIndex;
            line.slotsWithKey = withKey;
            line.filledPairCount = fields.filledPairCount;
            line.jsonBytes = json.n;
            line.blockBytes = uint32_t(bytes);
            line.slotsAvailable =
                (row.fill & kRosterFillLoadoutAvail) ? row.loadoutSlotsAvailable : 0;
            line.truncationKnown = truncationKnown ? 1u : 0u;
            line.truncated = truncated ? 1u : 0u;
            line.profileAsked = 1u;
            line.profileRefused = profileRefused ? 1u : 0u;
            line.profileWhy = profileWhy;
            // The version and the slot count are READ BACK OUT OF THE BYTES that were pushed, not
            // restated from the constants that were meant to be written: a census that prints its
            // own intention cannot report a producer that wrote something else.
            IdentitySnapshotHeader h{};
            memcpy(&h, scratch.block, sizeof h);
            line.version = h.version;
            line.slotsWritten = h.loadoutSlotCount;
            MaskIdentifier(row.displayName, line.nameMasked, sizeof line.nameMasked);
            census(line, censusCtx);
        }
    }
    }
}

}  // namespace capture
}  // namespace replay
