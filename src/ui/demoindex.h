#pragma once

// W2-F. Appendix B.0's IDemoIndex -- the browser's read-only view of a folder of captures.
//
// B.0 declares it and names W1-B as its owner; nothing implements it, and the browser cannot exist
// without one. This is that implementation, published into ReplayServices::demoIndex ONLY IF THAT
// SLOT IS NULL, so a later W1-B implementation takes precedence without a fight.
//
// ---------------------------------------------------------------------------------------------
// * THE SCAN IS A LISTING PLUS TWO PER-ROW STAGES, AND THE REASON IS SC11's ONE-SECOND BUDGET FOR
// 200 FILES -- spent a few rows per game frame, never in one.
//
// Scan enumerates the directory: name and mtime per file, newest first, nothing opened.
//
// Stage 1, per row and on demand (ReadRow), reads 128 bytes of header and 48 bytes of footer --
// two small reads at known offsets, no decompression, no chapter walk. That yields the size,
// duration, every §8.5.3 badge that comes from the header flag word, and the CONTENT MISMATCH
// badge, which is the file's recorded echovrMd5 against the build we are running.
//
// Stage 2 opens the file properly and reads its MANIFEST for the round table, the player list and
// the score. It is per-file and on demand, driven by what the browser is actually showing, because
// 200 manifest decodes is not a listing, it is a load.
//
// * A ZERO THAT MEANS "NOT LOADED YET" IS A LIE THE BROWSER WOULD TELL EVERY TIME IT OPENED.
// DemoSummary is frozen and has no room for "unknown", so roundCount and playerCount would sit at
// 0 for every un-stage-2'd row and read as "this match had no rounds and no players". The extra
// state lives in a parallel Row, ManifestLoaded(i) exposes it, and the browser prints a dash rather
// than a number it does not have.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <string>
#include <vector>

#include "foundation/demofile/demotypes.h"
#include "foundation/demofile/manifest_io.h"

namespace replay {

// B.0, verbatim. Declared here because nothing else in the tree declares it yet; when W1-B lands
// its own, this definition is deleted and the include changes, and no consumer of the interface
// has to change with it.
struct DemoSummary {
    const wchar_t* path;
    uint64_t sizeBytes, mtime, durationUs;
    uint16_t roundCount, playerCount;
    uint8_t blueScore, orangeScore;
    uint32_t flags;
    const Manifest* manifest;
};

class IDemoIndex {
public:
    virtual bool Scan(const wchar_t* dir) = 0;
    virtual size_t Count() const = 0;
    virtual const DemoSummary& At(size_t) const = 0;
    virtual void Close() = 0;

protected:
    ~IDemoIndex() = default;
};

namespace ui {

// Why a file in the folder is not listed, or is listed with less than it should have. Every value
// is shown to the user beside the row rather than turned into a silent omission -- a capture that
// vanished from the browser because its footer was torn is the worst possible way to learn that.
enum class RowFault : uint8_t {
    None = 0,
    Unreadable,       // the file could not be opened
    ShortFile,        // smaller than a header plus a footer
    BadMagic,         // not an ECHODEMO at all
    HeaderCrc,        // the 128-byte header does not check out
    NoFooter,         // no footer: the capture was not closed cleanly
    ManifestRefused,  // stage 2 opened it and the reader refused; RefusalReason is carried
};

const char* RowFaultName(RowFault);

// Whether this capture was recorded against the echovr.exe we are running.
//
// * THREE VALUES, NOT A BOOL, and the third is the one that matters. A bool answers "mismatch?"
// with false both when the builds MATCH and when we never managed to check -- and those must not
// render the same, because one is a clean bill of health and the other is an absence of evidence.
// W2-B's ActiveModeLabel makes the same distinction from the other side ("build not checked" is a
// separate string from a match), so collapsing it here would put the two ends of the same seam in
// disagreement on screen.
enum class BuildMatch : uint8_t {
    NotChecked = 0,   // the expected md5 was unavailable or unparseable; we do not know
    Matches,
    Mismatch,
    NotRecorded,      // the capture carries no fingerprint at all; nothing to compare against
};

const char* BuildMatchName(BuildMatch);

// The badge decision itself, lifted out of the file scan so it can be exercised without a file.
//
// * ABSENT IS NOT DIFFERENT, AND THAT IS THE WHOLE POINT. A header carrying sixteen zero bytes
// never recorded which echovr.exe it was made against -- no shipped recorder writes one today --
// and reading that as "recorded against a DIFFERENT exe" put a full-width CONTENT MISMATCH banner
// across a capture made minutes earlier on this machine, against the build that was running
// (L3-RUN-10 F3). A banner that fires on every capture teaches its reader to ignore the one that
// matters. A recorded fingerprint that genuinely differs still banners.
//
// `expectedMd5` is null when this build's own fingerprint is unavailable: then nothing was compared
// and the answer is NotChecked -- not a match.
BuildMatch ClassifyBuildMatch(const uint8_t recordedMd5[16], const uint8_t* expectedMd5);

// * WHICH THREAD OPENED A FILE, COUNTED RATHER THAN ASSERTED IN A COMMENT. Every DemoIndex entry
// point that can open a file reports here. NoteDrawThread() names the thread panel::Draw runs on,
// and FileIoOnDrawThread() is the number of those calls that happened on it -- which must be 0 for
// the life of the process. A comment saying "Draw does no I/O" cannot fail; this can.
namespace fileio {

// Header-only, because the targets that read these counters are not all the targets that link
// demoindex.cpp: probe_ui links the in-frame site without the index behind it, and a counter that
// only some of the builds can see is a counter some of the builds cannot check.
inline std::atomic<uint64_t>& OpenCount() {
    static std::atomic<uint64_t> v{0};
    return v;
}
inline std::atomic<uint64_t>& DrawThreadOpenCount() {
    static std::atomic<uint64_t> v{0};
    return v;
}
// The draw thread identifies ITSELF rather than being matched against a stored id: a thread-local
// flag cannot be confused by a thread id the OS reused after a thread died.
inline bool& CallerIsDrawThread() {
    static thread_local bool v = false;
    return v;
}

inline void Note() {
    OpenCount().fetch_add(1, std::memory_order_relaxed);
    if (CallerIsDrawThread()) DrawThreadOpenCount().fetch_add(1, std::memory_order_relaxed);
}

inline void NoteDrawThread() { CallerIsDrawThread() = true; }
inline uint64_t Count() { return OpenCount().load(std::memory_order_relaxed); }
inline uint64_t OnDrawThread() { return DrawThreadOpenCount().load(std::memory_order_relaxed); }

inline void ResetForTest() {
    OpenCount().store(0, std::memory_order_relaxed);
    DrawThreadOpenCount().store(0, std::memory_order_relaxed);
    CallerIsDrawThread() = false;
}

}  // namespace fileio

class DemoIndex final : public IDemoIndex {
public:
    DemoIndex();
    ~DemoIndex();

    // Lists *.echodemo in `dir`, newest first. Stage 1 only. False if the directory could not be
    // enumerated at all; a directory with unreadable files in it still succeeds, and each bad file
    // is a row with a fault rather than an absence.
    //
    // Legacy sources (.echoreplay, .tape) in `dir` and in every import dir are listed beside the
    // captures (DESIGN-FORMATS 7.8). A legacy file itself is never opened: stage 1 and stage 2 read
    // its cached conversion under `<dir>\imported\` when one exists, and nothing otherwise. An
    // import dir that is absent or unreadable does not fail the scan.
    bool Scan(const wchar_t* dir) override;

    // The extra folders the next Scan lists legacy sources from. A folder equal to the scanned
    // directory is not listed twice.
    void SetImportDirs(std::vector<std::wstring> dirs);

    // A .echoreplay/.tape row. Its Path() is the source; its header, flags and manifest are the
    // cached conversion's, and LegacyCached() says whether stage 1 found one.
    bool IsLegacy(size_t i) const;
    bool LegacyCached(size_t i) const;
    size_t Count() const override;
    const DemoSummary& At(size_t) const override;
    void Close() override;

    // Stage 1 for one row. Scan lists without opening anything -- 200 CreateFileW calls inside one
    // game frame is a visible stall on a cold disk -- so the header read is per row, on demand, and
    // the panel spreads it over ticks. Idempotent. False when the row index is out of range or the
    // header did not parse cleanly; Fault() says which. Until it has run, the row's flags, size
    // and duration are unset and RowRead() is false: a consumer that prints them anyway prints
    // zeros about a file nobody has opened.
    bool ReadRow(size_t i);
    bool RowRead(size_t i) const;

    // Stage 2 for one row, running stage 1 first if nothing has. Idempotent, and cheap after the
    // first call. False when the row index is out of range or the reader refused; the fault and
    // the refusal text are recorded either way.
    bool LoadManifest(size_t i);

    // Appends one capture that Scan did not find, and runs both stages on it. The -replayviewer
    // path is routinely OUTSIDE the replay folder, and without this the browser's manifest -- and
    // therefore the timeline's round markers -- would be missing for the single most common way a
    // capture gets opened. Returns the row index, or Count() if the file could not be read. An
    // already-listed path returns its existing index rather than a duplicate row.
    size_t AddOne(const wchar_t* path);

    bool ManifestLoaded(size_t i) const;
    RowFault Fault(size_t i) const;
    const char* FaultDetail(size_t i) const;      // never empty when Fault() != None
    BuildMatch BuildMatchOf(size_t i) const;       // recorded echovrMd5 against this build
    const wchar_t* Path(size_t i) const;
    const char* DisplayName(size_t i) const;       // the file name, UTF-8, for the facade

    // How long the last Scan took, and over how many files. SC11 is a budget, and a budget nobody
    // measures is a wish.
    uint64_t LastScanMicros() const;
    size_t LastScanFileCount() const;

    // What the OS said when the last Scan failed, and 0 when it did not. "the replay folder could
    // not be read" without this code cannot tell a missing folder from a denied one, and those two
    // send a user to completely different places.
    uint32_t LastScanError() const;

private:
    struct Row {
        std::wstring path;
        // The file the stages read: `path` itself, or a legacy row's cache path.
        std::wstring readPath;
        bool legacy = false;
        bool cached = false;
        std::string displayName;
        ManifestStore manifest;
        DemoSummary summary{};
        RowFault fault = RowFault::None;
        std::string faultDetail;
        bool stage1Attempted = false;
        bool manifestAttempted = false;
        bool manifestLoaded = false;
        BuildMatch buildMatch = BuildMatch::NotChecked;
    };

    bool ReadStage1(Row& row);
    // Appends one row per matching file in `dir`. False with the OS error when it cannot be read.
    bool ListDir(const std::wstring& dir, bool withCaptures, uint32_t* osError);
    // Re-points every pointer a DemoSummary borrows from its own Row. Called after anything that
    // can move rows_; see the definition for what a half-done version of it costs.
    void RebindBorrowedPointers();

    std::vector<Row> rows_;
    std::vector<std::wstring> importDirs_;
    std::wstring scanDir_;
    DemoSummary empty_{};
    uint64_t lastScanMicros_ = 0;
    size_t lastScanFiles_ = 0;
    uint32_t lastScanError_ = 0;
};

}  // namespace ui
}  // namespace replay
