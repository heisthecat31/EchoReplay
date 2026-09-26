#pragma once
// THE PRODUCER of the identity block: what makes one, when, and what it says it did.
//
// ARCH-23 s14.8b specifies the block and W2-C-2 wrote its format, its writer and its first reader --
// and ENG-W2-C-2 s6 measured that NOTHING put one into the demo stream.  `grep -rn
// "kTapIdentitySnapshot|IdentitySnapshot" src/` found the tap-table row and the three structs and no
// producer, so every recording this project has taken names its entrants only through the manifest
// roster and carries no per-entrant loadout at all.  This header is that missing half.
//
// THE TRIGGER, stated because "emitted when it changes" is not a design until the change is named:
//   * The roster scan is already gated (roster.h): a tick asks the engine's entrant count, and the
//     full scan runs when that MOVES or when the cadence (kRosterRescanIntervalUs) expires.  A row
//     whose fingerprint matches is not re-read at all.
//   * This emitter runs after a full scan and folds each row's BLOCK CONTENT -- the IdentityEntrant
//     the block would carry plus its twenty slot records -- into one signature.  A block is written
//     only when that signature moves, so a quiet second costs one fold per row and pushes nothing.
//   * A LATE LOADOUT CHANGE therefore arrives within the cadence and not by luck: swapping an item
//     does not change the entrant count, so the signal gate holds, but the cadence forces a full
//     scan, the row's fingerprint (which folds the twenty item ids) moves, the row is re-read, the
//     signature moves and a new block is pushed.  The bound on how late that is, is the cadence the
//     caller passes to ScanRosterOnChange -- one second -- and it is a bound rather than a hope.
//   * What this does NOT detect is a profile document that changes while every identity field the
//     block carries stays put: the document is read only for a row whose signature moved, because a
//     serialisation of every entrant's CJson tree per second is the instrument perturbing the system
//     (the same reasoning capture.cpp's open-time fetch is written from).  Named here rather than
//     left to be discovered.
//
// ONE BLOCK PER ENTRANT.  The record rides the producer ring through PushRecord, which stages a
// record in a thread-local buffer; a whole-roster block carrying twelve 4 KB profile documents would
// not fit any bound a tick thread can hold, and cutting the documents to make it fit is exactly the
// loss the format exists to prevent.  Per entrant, the widest block is
// kIdentityBlockOneEntrantCap -- derived below, never chosen -- and a late change re-publishes only
// the entrant that moved.  A reader meets a sequence of one-entrant blocks and CompareLoadoutByKey
// works on each unchanged.

#include <stddef.h>
#include <stdint.h>

#include "capture/identity_block.h"
#include "capture/roster.h"

namespace replay {
namespace capture {

// The widest block one entrant can produce: the header, the entrant, its slot records AT THE CURRENT
// STRIDE, the length word and a whole profile document at the file's own cap.  Every term is a
// sizeof or an imported constant, so a struct that grows grows this with it instead of silently
// overrunning the stage that carries it.
// ... and it is THE SAME constant the stream's own bound is, imported rather than re-summed
// The producer's widest record and the format's maximum S_EVENT body are one fact,
// and two copies of it would drift the day either struct grows.
constexpr size_t kIdentityBlockOneEntrantCap = kMaxEventBodyBytes;
static_assert(kIdentityBlockOneEntrantCap ==
                  sizeof(IdentitySnapshotHeader) + sizeof(IdentityEntrant) +
                      sizeof(IdentityLoadoutSlot) * kIdentitySlotCount + sizeof(uint32_t) +
                      kRosterJsonCap,
              "the stream's event bound and one entrant's block must be the same arithmetic");

// WHY A BLOCK THAT SHOULD HAVE BEEN WRITTEN WAS NOT.  A count of "emitted" alone cannot separate a
// quiet roster from a producer that is failing every push, and those are opposite repairs.
enum class IdentityEmitRefusal : uint8_t {
    None = 0,
    NoTapType,       // the tap table has no row for this block, so no index can name it
    StreamDisabled,  // S_EVENT is off in this capture's builder config
    ScratchTooSmall, // the caller's buffer cannot hold one entrant's block
    WriterRefused,   // IdentityBlockWriter would not take the entrant
    SinkRefused,     // the ring would not take the record -- counted, and the row is RETRIED
    Count,
};
constexpr size_t kIdentityEmitRefusalCount = size_t(IdentityEmitRefusal::Count);

const char* IdentityEmitRefusalText(IdentityEmitRefusal r);

// WHETHER ASKING FOR A REFUSED PROFILE AGAIN CAN EVER HELP.  Every refusal used to be re-offered on
// every scan, and one entrant's JsonNumberNotExact -- a full CJson walk that can only end the same
// way -- was paid once a second until the cost meter closed the owner's recording 48 s into a match.
enum class ProfileRefusalKind : uint8_t {
    Deterministic,  // the document itself is one this build cannot serialise: asked once per document
    Transient,      // the answer may change while the document stays: re-asked, backing off
    NoSource,       // nobody to ask: re-asked when a source appears
};
const char* ProfileRefusalKindText(ProfileRefusalKind k);

// Deterministic or Transient, never NoSource.  Only a refusal that is a property of the DOCUMENT is
// Deterministic; everything else, including every word this path cannot raise today, is Transient,
// so a misjudged word costs time and never an entrant's profile.
ProfileRefusalKind ClassifyProfileRefusal(identity_access::Refusal r);

// A Transient refusal waits this long for its first re-ask, doubles on every further refusal of the
// same document, and stops doubling at the cap: 1, 2, 4, 8, 16, 32, 60, 60 ... seconds.
constexpr uint64_t kProfileRetryFirstDelayUs = 1000000;
constexpr uint64_t kProfileRetryMaxDelayUs = 60000000;

// ... and the whole roster shares ONE such re-ask per this long, oldest refusal first. Refusal words
// are correlated across entrants (four of five in the field shared one), so per-row schedules alone
// stack several document walks into one cost-meter window, and the field head-room is one walk per
// window. A window is 512 ticks, 17.6 s at the field's 29 ticks/s; this is the next round number
// above it. A first-time row and a replaced document are not re-asks and do not wait for it.
constexpr uint64_t kProfileRetryRosterGapUs = 20000000;

// Monotonic microseconds. The back-off runs on a clock of its own because the capture has two time
// axes -- the file opens on the process sim clock and ticks on the server's peer clock -- and a
// deadline written on one and compared on the other never arrives when the first is ahead.
typedef uint64_t (*IdentityRetryClock)(void* ctx);

// Told ONCE per entrant per refusal word, not once per scan: the line that says which policy the
// entrant's document is under for the rest of the capture.
struct IdentityProfileRefusalNote {
    uint16_t rowIndex;
    uint16_t userSlot;
    uint16_t userIndex;
    identity_access::Refusal refusal;
    ProfileRefusalKind kind;
    const char* why;   // the source's own text for it
    // What the source said the DOCUMENT is, when it said anything: ShortBuffer reports the full
    // length and copies nothing, so this is the number that sizes the buffer. RECORDER-5 F2: the
    // 09-19 capture refused eight profiles and the log named no length, so the only evidence for
    // how big a real profile is was the one that fit. Zero means the refusal carried no length.
    uint32_t docBytes;
    char nameMasked[kRosterNameCap];
};

// One line per block actually pushed, in the shape the capture log prints it.  The display name is
// masked HERE rather than at the log call: a census that depends on every caller remembering to mask
// publishes a real name the first time one forgets.
struct IdentityEmitCensus {
    uint16_t rowIndex;
    uint16_t userSlot;
    uint16_t userIndex;
    uint32_t version;        // what the block SAYS, read back out of the bytes that were pushed
    uint16_t slotsWritten;   // the header's loadoutSlotCount
    uint16_t slotsWithKey;   // of those, how many carry an engine key rather than a zero
    uint16_t filledPairCount;   // the entrant's own denominator: how many of the twenty are REAL
                                // pairs, or kIdentityPairCountUnknown when the scan could not ask
    uint32_t jsonBytes;
    uint32_t blockBytes;
    uint8_t  profileAsked;      // whether the document was asked for at all
    uint8_t  profileRefused;    // and whether the engine refused it -- never the same as absent
    const char* profileWhy;     // the engine's own word for that refusal, for the census line
    uint16_t slotsAvailable;    // what the engine's component held, when the scan could ask
    uint8_t  truncationKnown;   // whether it could ask at all -- see IdentityBlockReader::LoadoutTruncated
    uint8_t  truncated;         // and the answer: the component was wider than the format records
    char     nameMasked[kRosterNameCap];
};

// What one pass did.  `unchanged` and `emitted` are the two halves of "on change, not per tick", and
// EVERY row this call considered lands in exactly one of the counters below -- CountsBalance() is
// that claim in code.  A row that falls through all of them is a row the report cannot explain, and
// a reader adding the numbers up would silently lose it.
struct IdentityEmitReport {
    uint16_t considered = 0;   // active rows this pass looked at
    uint16_t emitted = 0;      // blocks pushed
    uint16_t unchanged = 0;    // rows whose block content had not moved
    uint16_t keyless = 0;      // rows emitted with no slot key at all -- recorded and empty
    uint16_t truncated = 0;     // rows whose component was wider than the twenty slots recorded
    uint16_t truncationUnknown = 0;   // rows the scan could not ask -- NEVER counted as complete
    uint16_t jsonRefused = 0;  // rows whose profile document the engine would not hand over
    uint16_t deferred = 0;     // rows whose block this pass did not build, to stay inside the
                               // per-tick document budget -- offered again next scan, never lost
    uint16_t retryDeferred = 0;   // ... and the same for a REFUSED row's retry, which is served only
                               // after every first-time row and so misses the budget more often. Its
                               // own counter because the two are different lags: a deferred entrant
                               // has never published, a deferred retry has a block on disk already
    uint16_t profileRetried = 0;  // rows re-read because their document had been REFUSED, and still
                                  // was: nothing new to say, so no second identical block. These are
                                  // served AFTER every first-time row, never ahead of one (F10)
    uint16_t retryWaiting = 0;    // refused rows NOT asked this pass -- settled as deterministic,
                                  // inside their back-off, or waiting for the roster's one re-ask.
                                  // Already counted in `unchanged`
    const char* lastJsonWhy = nullptr;   // the engine's own word for the most recent one
    uint32_t refusals[kIdentityEmitRefusalCount] = {};

    uint32_t RefusalTotal() const {
        uint32_t n = 0;
        for (size_t i = 0; i < kIdentityEmitRefusalCount; ++i) n += refusals[i];
        return n;
    }

    // Every considered row is in exactly one bucket.  ScratchTooSmall is not in the sum because it
    // refuses before any row is considered at all (`considered` is then 0), which is a statement
    // about the call rather than about a row.
    bool CountsBalance() const {
        const uint32_t placed = uint32_t(emitted) + unchanged + deferred + retryDeferred +
                                refusals[size_t(IdentityEmitRefusal::WriterRefused)] +
                                refusals[size_t(IdentityEmitRefusal::SinkRefused)];
        return placed == uint32_t(considered);
    }
};

// Keeps the first character and the true length and replaces the rest: enough for a census to tell
// two entrants apart and to say a name was read at all, not enough to publish who was playing.
void MaskIdentifier(const char* in, char* out, size_t cap);

// The buffers the emitter borrows.  It allocates NOTHING -- this runs on the game tick (S6.4) -- and
// the profile buffer is at the file's own cap so a document that would not fit the file is refused
// here with its true length rather than cut.
struct IdentityEmitScratch {
    uint8_t* block = nullptr;
    size_t   blockCap = 0;
    uint8_t* json = nullptr;
    size_t   jsonCap = 0;
};

// The ring, and the census line, as function pointers for the same reason gather.h's push is one:
// capture.cpp owns the ring and the log, and this file owns what a block contains and when.
typedef bool (*IdentityBlockSink)(const void* block, uint32_t bytes, uint64_t tUs, void* ctx);
typedef void (*IdentityCensusSink)(const IdentityEmitCensus& line, void* ctx);
typedef void (*IdentityProfileRefusalSink)(const IdentityProfileRefusalNote& note, void* ctx);

class IdentityEmitter {
public:
    // Per capture. The refusal sink is the caller's wiring, not capture state, and survives.
    void Reset() {
        for (size_t i = 0; i < kRosterMaxRows; ++i) {
            signature_[i] = 0;
            haveSignature_[i] = 0;
            profileRefused_[i] = 0;
            notedRefusal_[i] = 0;
            everRefused_[i] = 0;
            docToken_[i] = 0;
            latchedAtUs_[i] = 0;
            retryDelayUs_[i] = 0;
        }
        rosterReasked_ = false;
        rosterReaskedAtUs_ = 0;
    }

    // Tests only; without one the emitter reads the system's monotonic clock. `tUs` stamps blocks
    // and is never a retry clock.
    void SetRetryClock(IdentityRetryClock clock, void* ctx) {
        retryClock_ = clock;
        retryClockCtx_ = ctx;
    }

    void SetProfileRefusalSink(IdentityProfileRefusalSink sink, void* ctx) {
        refusalSink_ = sink;
        refusalSinkCtx_ = ctx;
    }

    // Call after a FULL roster scan: the table's rows are what the engine last said, and this decides
    // which of them the file does not yet carry.  `profileSrc` may be null, in which case the blocks
    // are written without their profile documents and every one of them is counted in `jsonRefused`
    // -- an absent document is never a zero-length one.
    // `maxProfileReads` bounds what ONE call can cost: a profile document is a full CJson walk, and
    // a round transition moves every entrant at once.  Rows past the budget are
    // DEFERRED -- their signature is not latched, so the next scan offers them again -- rather than
    // dropped.  The armed game tick passes 1; the worker-thread caller at file open passes the
    // table's width, because that path is off the tick and the file needs its whole roster at once.
    //
    // ★ WHAT DEFERRAL COSTS, stated exactly: with a budget of one and N rows moving
    // at once, the LAST of them waits N-1 scans, which at kRosterRescanIntervalUs is N-1 seconds --
    // eleven for a full twelve-entrant lobby, not "one cadence".  The rows are in the file and they
    // are correct; what lags is the moment they appear.  `deferred` is reported so a reader sees the
    // lag rather than inferring it, and the open-time caller takes the whole table precisely so a
    // file never STARTS in that state.
    void EmitChanged(const RosterTable& table, const IRosterSource* profileSrc, uint64_t tUs,
                     uint64_t contentHash, const IdentityEmitScratch& scratch,
                     uint16_t maxProfileReads, IdentityBlockSink sink, void* sinkCtx,
                     IdentityCensusSink census, void* censusCtx, IdentityEmitReport& out);

    // The signature this emitter last published for a row, and whether it published one at all. The
    // second is what makes "never emitted" and "emitted a block that folded to zero" different
    // states rather than one.
    bool PublishedSignature(uint16_t row, uint64_t& out) const {
        if (row >= kRosterMaxRows || !haveSignature_[row]) return false;
        out = signature_[row];
        return true;
    }

private:
    uint64_t signature_[kRosterMaxRows] = {};
    uint8_t  haveSignature_[kRosterMaxRows] = {};
    // Whether the block last written for this row said ProfileRefused, and WHY -- kept beside the
    // signature because the signature cannot see it: the flags are set after it is
    // folded. Each refusal state is its own retry policy (F10, CAP-COST): see ProfileRefusalKind.
    enum : uint8_t {
        kNotRefused = 0,
        kRefusedDeterministic = 1,
        kRefusedTransient = 2,
        kRefusedNoSource = 3,
    };
    enum class Retry : uint8_t {
        No,
        Now,              // a new question: a source appeared, or the document was replaced
        WhenRosterAllows, // the same question, and this row's back-off has run out
    };
    Retry RetryDue(uint16_t row, uint16_t userIndex, const IRosterSource* src, uint64_t nowUs) const;
    void LatchRefusal(uint16_t row, ProfileRefusalKind kind, uint64_t docToken, uint64_t nowUs);
    bool RosterGapOpen(uint64_t nowUs) const;
    uint64_t RetryNowUs() const;

    uint8_t  profileRefused_[kRosterMaxRows] = {};
    uint8_t  notedRefusal_[kRosterMaxRows] = {};   // refusal word last told to the sink, plus one
    // RECORDER-3 F2. A row that has EVER been refused a document was refused it at file-open too --
    // the manifest row was written from this same source, one scan earlier -- so the first block
    // that does carry one supersedes a header the format cannot rewrite.
    uint8_t  everRefused_[kRosterMaxRows] = {};
    // The document the latched refusal was ABOUT. A refusal of document X is no answer about a
    // document Y that replaces it, so a moved token re-opens the question whatever the kind.
    uint64_t docToken_[kRosterMaxRows] = {};
    uint64_t latchedAtUs_[kRosterMaxRows] = {};   // retry clock, when the transient latch was written
    uint64_t retryDelayUs_[kRosterMaxRows] = {};
    bool     rosterReasked_ = false;
    uint64_t rosterReaskedAtUs_ = 0;   // retry clock, the roster's last back-off re-ask
    IdentityRetryClock retryClock_ = nullptr;
    void* retryClockCtx_ = nullptr;
    IdentityProfileRefusalSink refusalSink_ = nullptr;
    void* refusalSinkCtx_ = nullptr;
};

// The block content a row would produce, folded.  Exposed because it is the emitter's whole change
// test: a test that computes it a second way would be certifying its own copy.
uint64_t IdentityRowSignature(const IdentityEntrant& e, const IdentityLoadoutSlot* slots,
                              uint16_t slotCount);

}  // namespace capture
}  // namespace replay
