#pragma once

// W2-G. The notes sidecar of DESIGN 5.9.2: user bookmarks and the last playhead position, in
// `<name>.echodemo.notes.json` beside the capture.
//
// * WHY IT IS A SIDECAR AND NOT A SECTION. 5.9.2: "A .echodemo is immutable once written." That is
// what keeps the content hash meaningful and makes sharing a capture a copy rather than an export.
// Bookmarks are the one thing a viewer produces about a file it must not modify.
//
// * WHY THIS PACKET OWNS IT. DESIGN 13's table gives it to nobody, and `mark` is one of 8.5.2's
// commands -- so without an owner that command refuses forever. Orchestrator ruling: it is
// playback-time control state, so it lives here. The architect has been told so 13 can record it.
//
// * WHAT IT HOLDS, AND WHAT THIS HEADER USED TO CLAIM IT HOLDS. Bookmarks and the resume point.
// This comment previously also advertised "per-file view settings"; there was never a field for
// them, so there was nothing to read and nothing to write. The claim is withdrawn rather than
// answered with a member nobody sets -- a stored value with no writer and no reader is the defect
// this packet was rejected for, not the fix for it.
//
// * THE WRITER AND THE READER ARE ONE PAIR AND ARE TESTED AS ONE. A label is user text: it can hold
// a quote, a backslash, a tab or a name in any script. An unescaped writer produces a file that
// stops parsing, silently, because nothing reads it again until the next session -- so the round
// trip is the test, not the writer alone. A label the WRITER can emit and the READER would refuse
// is the same defect wearing a different hat, so such a label is refused at AddBookmark instead of
// being written.
//
// * A DAMAGED SIDECAR IS REFUSED WHOLE, never half-applied. Restoring some bookmarks and dropping
// others with no diagnosis is worse than restoring none and saying the file is damaged.
//
// ★★★ ABSENT IS NOT THE SAME AS UNREADABLE, AND THE DIFFERENCE IS SOMEBODY'S DATA. Load used to
// answer one bool plus `fileMissing_`, and every open failure -- a sharing violation, a deny-read
// ACE, descriptor exhaustion -- set fileMissing_. `mark`'s only protection was "not there? then
// this is the first mark", so a sidecar that could not be READ was replaced by a fresh one holding
// a single bookmark, and a week of the owner's marks went with it. LoadResult now separates the
// six outcomes, and exactly ONE of them (Absent) authorises a caller to write a new file.

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "control/command.h"

namespace replay {
namespace control {
namespace notes {

// Bounded because this is parsed from a file on disk that anything could have written. A cap that
// is reported when it is reached is a refusal; an unbounded vector is a length field from an
// untrusted file sizing an allocation.
inline constexpr size_t kMaxBookmarks = 256;
inline constexpr size_t kMaxLabelChars = 128;

static_assert(kMaxLabelChars == kMaxMarkLabelChars,
              "the parser's label cap and the sidecar's must be one number, or `mark` accepts a "
              "label the sidecar then refuses");

// The format this build writes and the only one it reads. A file carrying anything else is
// refused: `version` used to be written and then skipped as an unknown key on read, so a future
// format-2 file with its times in nanoseconds would have been read as format 1 and every bookmark
// would have landed a thousand times off.
//
// ★★★ 2 BECAUSE THE AXIS UNDER `tUs` CHANGED, AND NOTHING ELSE IN THE DOCUMENT DID. `mark` records
// IReplayTransport::PlayheadUs(). Until commit 3f7ca26 that was absolute source time; the ruling
// there made everything crossing the transport 0-based (microseconds since the start of playback)
// and REMOVED StartUs(), so no seam publishes a capture origin any more. A format-1 file therefore
// holds times on an axis this build cannot even name, off by the capture's t0 -- and the number is
// not recoverable, which is why kFormatVersion1Absolute is refused rather than migrated. The two
// formats are byte-indistinguishable apart from this field: reading the version is the ONLY thing
// that can tell them apart, and a bookmark placed t0 out is not a visible error, it is a bookmark
// pointing at the wrong moment of a match.
inline constexpr int kFormatVersion = 2;

// The pre-3f7ca26 format. Named rather than spelled `1` at the comparison so the refusal can say
// which format it met, and so this constant is the one place the old axis is described.
inline constexpr int kFormatVersion1Absolute = 1;

// ★★★ THE SIDECAR NAMES THE CAPTURE IT BELONGS TO, because it outlives it. L3-RUN-3 F3: a fixture
// was REGENERATED between two launches -- different length, different md5, different capture -- and
// its sidecar carried four bookmarks and a `lastPlayheadUs` over onto the new file. A probe row
// flipped from asked-fail to could-not-ask on state a previous launch had left behind, and the
// owner would have been sent to a moment that does not exist in the capture he is watching.
//
// The identity is not a version question, which is why the format number does not move: what `tUs`
// MEANS is unchanged. It is additive, and its ABSENCE is decidable -- a document with no `identity`
// is Unidentified and is refused, never adopted -- so no older sidecar can be read as a bound one.
//
// `demoUuid` is the real answer (ENG-W2A-R10 F7: minted per open, so one match is one identity),
// and size and `t0Us` come with it because they are already in the header and a capture that
// differs in either is not the same bytes. A capture written before F7 carries sixteen zero bytes
// there; for those, and ONLY those, the identity is the md5 of the first 64 KiB -- the same MD5 the
// rest of this build identifies files with, not a second copy of the algorithm.
inline constexpr size_t kLegacyDigestBytes = 64u * 1024u;

struct CaptureIdentity {
    uint8_t uuid[16] = {0};         // FileHeaderDisk::demoUuid, byte for byte
    uint8_t headDigest[16] = {0};   // md5 of the first kLegacyDigestBytes -- ONLY when uuid is zero
    int64_t sizeBytes = 0;
    int64_t t0Us = 0;

    bool IsLegacy() const;          // all sixteen uuid bytes are zero

    // Every recorded field must agree. A regenerated capture differs in the uuid alone, so uuid
    // would do -- but a sidecar that survives because we only asked the easy question is the shape
    // of this defect, and size and t0Us cost nothing to compare.
    bool Matches(const CaptureIdentity& other) const;
};

// Reads the capture's own file header. False when it cannot be opened, is shorter than a header, or
// does not carry the .echodemo magic -- an identity we could not read is not one that matches
// anything, and the caller must refuse rather than fall back to "close enough".
bool IdentityForCapture(const wchar_t* demoPath, CaptureIdentity* out);

struct Bookmark {
    int64_t tUs = 0;
    wchar_t label[kMaxLabelChars] = {0};
};

// Why a bookmark was refused. One reason, never a disjunction: the code knows which limit was hit,
// and a message offering the caller two possibilities is the code declining to say.
enum class AddResult : uint8_t {
    Ok = 0,
    Full,            // the sidecar already holds kMaxBookmarks
    LabelTooLong,    // longer than kMaxLabelChars - 1
    LabelMalformed,  // unpaired surrogate: writable, but this file's own reader would refuse it
};

const char* AddResultName(AddResult);

// Why a sidecar could not be read. ★ Absent is the ONLY value that permits a caller to write a new
// file over that path; everything else means "we do not know what is in there".
enum class LoadResult : uint8_t {
    Ok = 0,
    Absent,          // ENOENT, and nothing else
    Unreadable,      // it exists and the open or the read failed: sharing, ACL, descriptors, I/O
    Damaged,         // it opened and does not parse, or it is empty
    UnknownVersion,  // it parses and its `version` is neither kFormatVersion nor one we know of
    // ★ A KNOWN OLD FORMAT IS NOT AN UNKNOWN ONE, and the owner needs the difference: this file is
    // his, this build wrote it, and it is refused because the meaning of `tUs` changed under it.
    // "Some version I cannot read" and "the version I wrote last month, on the old time axis" would
    // send him to two different places. Appended last so the numeric values Snapshot::notesState
    // publishes over HTTP keep meaning what they meant.
    AbsoluteAxisVersion,
    // ★ THE THREE ANSWERS L3-RUN-3 F3 NEEDED, and they are three because they are three different
    // situations for the caller. Unidentified: it parses, it is our format, and it names no capture
    // -- so nothing can prove it belongs to this one. StaleIdentity: it names a DIFFERENT capture
    // and it has been renamed aside, so the path is free and nothing was destroyed. Held: the same,
    // except the rename failed -- the file is still sitting there and writing over it would lose
    // somebody's bookmarks, which is the whole reason this enum has six values already.
    Unidentified,
    StaleIdentity,
    StaleIdentityHeld,
};

// The numbers are published: Snapshot::notesState goes out over HTTP as `"notesState":%d`. An
// insertion in the middle of the enum compiles, passes every test and changes what `3` means to
// every client, so the values are pinned here rather than left to the order of the list.
static_assert(static_cast<int>(LoadResult::Ok) == 0);
static_assert(static_cast<int>(LoadResult::Absent) == 1);
static_assert(static_cast<int>(LoadResult::Unreadable) == 2);
static_assert(static_cast<int>(LoadResult::Damaged) == 3);
static_assert(static_cast<int>(LoadResult::UnknownVersion) == 4);
static_assert(static_cast<int>(LoadResult::AbsoluteAxisVersion) == 5);
static_assert(static_cast<int>(LoadResult::Unidentified) == 6);
static_assert(static_cast<int>(LoadResult::StaleIdentity) == 7);
static_assert(static_cast<int>(LoadResult::StaleIdentityHeld) == 8);

const char* LoadResultName(LoadResult);

// ★ THE ONE QUESTION `mark` HAS TO GET RIGHT, as a predicate rather than as a condition spelled out
// at the call site. Five of the six outcomes mean "something is there and we could not read it",
// and creating a new sidecar over any of them destroys whatever it held. It is a function so the
// answer can be swept over every enumerator by a test, which a `!= Ok && != Absent` written inline
// at one call site cannot be.
// ★ TWO, and the second one is only reachable after the old file has been MOVED. LoadFor returns
// StaleIdentity exclusively when the rename aside succeeded, so the path it names now holds nothing
// and a new sidecar destroys nothing; when the rename fails the answer is StaleIdentityHeld, which
// is not on this list. Everything else still means "something is there and we could not read it".
inline bool MayCreateNew(LoadResult r) {
    return r == LoadResult::Absent || r == LoadResult::StaleIdentity;
}

// Why a save failed, so `mark` can tell a length problem from a permission problem. The old
// message said "could not write <path>" for both, which sends the owner to look at ACLs for a path
// that is simply too long.
enum class SaveResult : uint8_t {
    Ok = 0,
    PathTooLong,     // the sidecar path, or its temporary, would not fit MAX_PATH
    OpenFailed,
    WriteFailed,     // including a delayed write that only surfaced at close
    ReplaceFailed,
};

const char* SaveResultName(SaveResult);

// `<demoPath>.notes.json`, refused rather than shortened when it does not fit.
//
// ★ THE CEILING IS MAX_PATH, NOT `cap`. The check used to be against the caller's buffer (520),
// which let a 245-character capture path produce a sidecar path that every _wfopen_s in this file
// then refused -- `mark` was permanently dead on that capture with a permission-shaped message.
// The room for Save()'s `.tmp` suffix is counted here too, because Save appends it after every
// length check anyone else could make.
bool SidecarPathFor(const wchar_t* demoPath, wchar_t* out, size_t cap);

// The longest capture path that can carry a sidecar, so a refusal can name the number.
size_t MaxDemoPathChars();

class Sidecar {
public:
    void Clear();

    AddResult AddBookmark(int64_t tUs, const wchar_t* label);

    size_t          BookmarkCount() const { return count_; }
    // Null past the end. It used to fold an out-of-range index to 0, which on an empty sidecar
    // handed back a zeroed Bookmark that reads as a real bookmark at t=0 with an empty label:
    // bounds-safe and answer-unsafe.
    const Bookmark* BookmarkAt(size_t i) const;

    void    SetLastPlayheadUs(int64_t us) { lastPlayheadUs_ = us; }
    int64_t LastPlayheadUs() const { return lastPlayheadUs_; }

    void Serialise(std::string* out) const;

    // Replaces the whole contents. Leaves the sidecar empty rather than partly filled.
    LoadResult Parse(const char* text, size_t len);

    // The file reader, and only that: it answers what is in the sidecar, not whose it is. Every
    // caller that has a capture in hand must use LoadFor instead -- this one cannot tell a bookmark
    // set belonging to the file being played from one belonging to the file that used to be there.
    LoadResult Load(const wchar_t* path);

    // ★ THE ONE THE PRODUCT USES. Reads, then refuses unless the sidecar names THIS capture; on a
    // mismatch the sidecar is left empty (never another capture's bookmarks, never another
    // capture's resume point) and the file is renamed to `<path>.stale-<uuid>` rather than deleted,
    // because it is the owner's data and it belongs to a capture that may still exist.
    LoadResult LoadFor(const wchar_t* path, const CaptureIdentity& expect);

    const CaptureIdentity& Identity() const { return identity_; }
    void SetIdentity(const CaptureIdentity& id) { identity_ = id; }

    // Writes through a temporary and replaces atomically. A torn write here loses every bookmark a
    // user has made, and the loss is only discovered a session later.
    SaveResult Save(const wchar_t* path) const;

private:
    Bookmark bookmarks_[kMaxBookmarks];
    size_t   count_ = 0;
    int64_t  lastPlayheadUs_ = 0;
    CaptureIdentity identity_;
};

}  // namespace notes
}  // namespace control
}  // namespace replay
