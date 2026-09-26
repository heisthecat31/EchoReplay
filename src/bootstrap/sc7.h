// C8 / P12: the in-process half of SC7.
//
// SC7 has two halves. The harness owns one - the on-disk md5 of echovr.exe, which it can take before and
// after a launch. Nobody owns the other: what the MAPPED image looks like while the game runs. That half
// has been NOT-RUN on every launch in this project's history, because it cannot be computed from outside
// the process, and "0 violations" has therefore always been a statement about a file rather than about
// what executed.
//
// This is the missing half. It compares the live `.text` of echovr.exe against the same bytes on disk,
// subtracts the interventions the ledger DECLARES (config_keys.inc's Level-1 and Level-2 rows, matched to
// hookmgr's registered sites), and reports what is left over.
//
// ★ THE DENOMINATOR IS NOT OURS TO SUPPLY. §9.12: a denominator produced by the work being measured
// cannot fail. `expected_rows` is read from config_keys.inc and the config file - the declaration - and
// `observed_rows` from the bytes. If the declaration is empty while a detour is standing in .text, that is
// FAIL ("ledger-empty"), never a PASS earned by declaring nothing.
//
// ★ RELOCATIONS: DELTA-CHECKED, NOT MASKED. echovr.exe is DYNAMICBASE + HIGH_ENTROPY_VA, so every
// absolute address the image embeds differs between disk and memory by the load delta. The cheap answer
// is to mask every byte a .reloc entry covers and never look at it. We do not: a masked field is a hole a
// patch can hide in, and .text relocs are exactly where a jump table lives. Instead each differing field
// covered by an IMAGE_REL_BASED_DIR64 entry is EXCUSED ONLY IF live == disk + delta. A relocated field
// that holds anything else is reported like any other difference. `reloc_bytes_skipped` counts what was
// excused, so the size of the blind spot is published rather than assumed to be zero.
//
// Four-valued, per §9.1: PASS (no undeclared difference), FAIL (every undeclared range listed with its
// RVA, length and first 16 bytes both sides), COULD-NOT-ASK (a refusal, with the reason, written to the
// file so the harness renders NOT-RUN with a cause) - and never a silent never-asked, which is what an
// absent file already means.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace replay {
namespace sc7 {

constexpr size_t kMaxRows = 16;      // hookmgr's own table is 16 sites; a row per site is the ceiling
constexpr size_t kMaxRanges = 32;    // enough to characterise a failure; the total is reported separately
constexpr size_t kRowChars = 8;      // "1X.2" and room
constexpr size_t kSampleBytes = 16;  // the first 16 bytes of a differing range, both sides
constexpr size_t kPartyChars = 16;   // "rlay" and room for the next one

struct Row {
    char id[kRowChars];
};

// One undeclared differing span. `rva` is relative to the image base, so it is comparable against the
// registry and against a disassembler, neither of which knows where the loader put us today.
struct Range {
    uint64_t rva;
    uint32_t len;
    uint8_t  before[kSampleBytes];   // on disk
    uint8_t  after[kSampleBytes];    // live
    uint32_t sampled;                // how many of the 16 are meaningful
    char     owner[kPartyChars];     // the third party whose own declaration covers it, or "" for nobody
};

// ---------------------------------------------------------------------------------------------------
// The third-party ledger
// ---------------------------------------------------------------------------------------------------
//
// The first live launch (L5) put 13 undeclared ranges in .text and every one of them was somebody else's
// patch: `rlay.dll` (EchoRelay), the mod the design says we COEXIST with (SC9). A reader that cannot tell
// "our undeclared detour" from "a known third party's" fails every launch forever; a reader that shrugs
// them off is S9.1's two-state gate. So a foreign range is neither ignored nor lumped in with ours - it
// is ATTRIBUTED, by name, against a set the third party itself declares.
//
// ★ THE SET IS DERIVED FROM rlay.dll, NOT TYPED HERE. `ScanRlayPatchTable` reads the loaded module's own
// patch-install code, so the ledger follows whatever version is installed rather than a list that goes
// stale silently. Two shapes carry every site this build installs (both verified byte-for-byte against
// the shipped 191,488-byte rlay.dll):
//   - a byte patch:    mov edx,<len> ... add rbx,<rva> ; mov rcx,rbx ; call [WriteBytes]
//   - a detour target: mov rax,[base] ; add rax,<rva> ; mov [g_target],rax
// A range is attributed only if it is FULLY CONTAINED in one of those spans and rlay.dll is LOADED. A
// range that matches the set with rlay absent is a FAIL naming it - somebody else patched those bytes -
// and a range nobody declares is a FAIL naming it too. Attribution is never a pass by omission: every
// attributed range stays in the report with its owner, and the per-party counts are published.
struct ThirdPartyPatch {
    uint64_t rva;
    uint32_t len;   // 0 when the site was found but its length could not be read: it attributes nothing
};

constexpr size_t kMaxParties = 2;
constexpr size_t kMaxPartyPatches = 64;   // rlay declares 24 today; room for it to grow before the cap
constexpr size_t kProvenanceChars = 160;

// One third party's declaration, as its own image states it.
struct ThirdPartySet {
    char            name[kPartyChars];
    bool            present;     // its module is loaded in this process
    bool            tableRead;   // its declaration was successfully derived (patchCount may still be 0)
    char            provenance[kProvenanceChars];
    ThirdPartyPatch patches[kMaxPartyPatches];
    uint32_t        patchCount;
    uint32_t        droppedPatches;   // sites found past the cap: the set is incomplete and says so
};

// What attribution concluded about one party. `unmatched` is the ranges that fall inside this party's
// declared spans while the party is NOT loaded - the "somebody else patched rlay's sites" case, which is
// a FAIL that has to name the bytes rather than a count.
struct ThirdPartyView {
    char     name[kPartyChars];
    bool     present;
    bool     tableRead;
    char     provenance[kProvenanceChars];
    uint32_t declared;
    uint32_t observed;
    uint64_t unmatched[kMaxRanges];
    uint32_t unmatchedCount;
};

// One registered site whose bytes differ - the attributed half of the same measurement.
struct SiteDetail {
    char     row[kRowChars];
    char     factId[64];
    uint64_t rva;
    uint32_t len;
    uint8_t  after[kSampleBytes];
    uint32_t sampled;
};

// A row this pass could not ask about. A string row is declared by its provider's PUBLISHED resolution
// (config::RowDeclarationState), so before the provider decides, the row is neither expected nor
// undeclared: it is kept out of both denominators and listed here instead. `reason` is the key rendered
// as one hyphenated token, so `tick.mode` unpublished reads `tick-mode-not-yet-published`.
//
// AUDIT_G G-13, for the next reader who asks what the exclusion costs: the exclusion matches on the ROW
// ID, not on who installed the detour. Our own code cannot exploit that -- hookmgr::Create asks the same
// predicate, so an Unresolved row never got its detour installed -- but a FOREIGN detour sitting at that
// row's site during the post-init window (the rlay.dll collision of S13.4) is excused from `extra`
// rather than reported as undeclared. It stays visible two ways: the `late-tick` pass scores it, and
// deferred_rows names the exclusion in the document. Since AUDIT_H H-2 it is also no longer free -- a
// deferred row with an observed detour refuses instead of passing.
struct DeferredRow {
    char id[kRowChars];
    char key[48];
    char reason[80];
};

enum class Verdict : uint8_t { Pass, Fail, CouldNotAsk };

const char* VerdictName(Verdict v);

constexpr size_t kMaxPasses = 4;   // post-init and late-tick, with room

// What one pass said, kept verbatim even when the run's verdict is no longer that pass's word. A FAIL is
// sticky for the process: neither a later PASS nor a later refusal can take it back, because the launch
// still saw the patch. The later pass is recorded here so the refusal is auditable rather than erased.
struct PassRecord {
    char    pass[24];
    Verdict verdict;
    char    reason[192];
};

struct Sc7Result {
    bool     refused;
    char     reason[192];
    Verdict  verdict;

    Row      expected[kMaxRows];
    uint32_t expectedCount;
    Row      observed[kMaxRows];
    uint32_t observedCount;

    // Neither expected nor observed-as-undeclared: excluded from both sides of the count.
    DeferredRow deferred[kMaxRows];
    uint32_t    deferredCount;

    Range    unattributed[kMaxRanges];
    uint32_t unattributedCount;      // what fits in the array
    uint32_t unattributedTotal;      // what was found, so a cap is visible as a cap

    // Of `unattributed`, who owns what. `unownedTotal` is the number this reader can put a name to
    // nobody - it INCLUDES the ranges the cap dropped, because a range that was never recorded cannot
    // have been attributed and a cap must not read as an attribution.
    ThirdPartyView thirdParty[kMaxParties];
    uint32_t       thirdPartyCount;
    uint32_t       attributedRanges;
    uint32_t       unownedTotal;

    SiteDetail detail[kMaxRows];
    uint32_t   detailCount;

    uint32_t relocBytesSkipped;
    uint32_t relocFieldsInText;
    // RelocFieldsInText == 0 has three causes -- the image declares no relocation
    // directory, the walk read it and .text holds no DIR64 field, or the walk never ran -- and the
    // count alone cannot tell them apart, so a launch could not say whether the delta-check was
    // exercised or merely absent. These two encode it: dirBytes is what the image DECLARES, and
    // blocksWalked is what the walk actually consumed. blocksWalked > 0 with fieldsInText == 0 is
    // "walked, found none"; blocksWalked == 0 with dirBytes > 0 is a walk that did not run.
    uint32_t relocBlocksWalked;
    uint32_t relocDirBytes;
    uint32_t declaredSites;          // registered Level-1/2 sites mapped inside the compared span
    uint32_t armedLedgerSites;       // of those, the ones actually armed
    // The patch window's own blind spot, counted the way relocBytesSkipped is rather than assumed away:
    // the window is 12 bytes per site but MinHook writes at most 7 of them (patchAbove) and an unarmed
    // site is written not at all, so this is how many bytes per launch the window would excuse that no
    // disposition ever produces.
    uint32_t windowBlindBytes;

    char     diskMd5[33];
    uint64_t textRva;
    uint64_t textBytes;              // how many bytes were compared
    int64_t  relocDelta;
    uint64_t elapsedUs;
    char     pass[24];               // "post-init" | "late-tick"

    // Every pass this process has run, oldest first, filled by RecordPass.
    PassRecord passes[kMaxPasses];
    uint32_t   passCount;

    // Why the report went somewhere other than the harness's artifact directory, or "" when it did not.
    char     probeDirFallback[64];
};

// The patch window around a registered site, derived from MinHook's own instruction structs rather than
// typed as a number. MinHook writes either a 5-byte relative jump AT the target, or - when the target has
// a hot-patch area - a relative jump 5 bytes BEFORE it plus a 2-byte short jump at the target itself.
// Both dispositions have to be inside the window or an armed hook reads as an undeclared patch.
size_t PatchWindowBefore();
size_t PatchWindowAfter();

// The pure core, exposed so it can be driven against synthetic buffers with no game in the process.
// `disk` and `live` are the same span of .text from the two sources; `relocOffsets` are the offsets
// WITHIN that span of IMAGE_REL_BASED_DIR64 fields, ascending.
struct ImageView {
    const uint8_t*  disk;
    const uint8_t*  live;
    size_t          len;
    uint64_t        textRva;
    int64_t         relocDelta;
    const uint32_t* relocOffsets;
    size_t          relocCount;
    uint32_t        relocBlocksWalked;  // base-relocation blocks the walk consumed
    uint32_t        relocDirBytes;      // IMAGE_DIRECTORY_ENTRY_BASERELOC Size, as the image declares it
};

// A registered site as the comparison sees it: where it is, and which ledger row owns it.
struct SiteView {
    const char* factId;
    const char* row;
    uint64_t    rva;
    bool        armed;
};

// Fills observed / unattributed / detail / relocBytesSkipped from the two buffers. Pure: no I/O, no
// globals, no allocation. Does not decide the verdict - Decide does, from this plus the declaration.
void Compare(const ImageView& image, const SiteView* sites, size_t siteCount, Sc7Result& out);

// Reads `add rbx,<rva>` / `add rax,<rva>` patch-install sites out of a copy of rlay.dll's `.text`.
// `maxRva` is the extent of the span SC7 compares, so a constant that could not be an RVA in it is not
// mistaken for one. Pure, and exposed because the only honest control for a scanner over somebody else's
// compiled code is real bytes from that code. Returns how many spans were written to `out`;
// `*dropped` counts the ones past the cap.
size_t ScanRlayPatchTable(const uint8_t* text, size_t len, uint64_t maxRva, ThirdPartyPatch* out,
                          size_t cap, uint32_t* dropped);

// Fills `out` with every third party this process knows how to attribute to, each from its own module.
// `maxRva` bounds the RVAs a declaration may name. Returns how many sets were written.
size_t GatherThirdParties(ThirdPartySet* out, size_t cap, uint64_t maxRva);

// Pure. Gives each undeclared range an owner, or leaves it with none. Runs between Compare and Decide:
// Compare subtracts OUR sites, this subtracts the third parties', and whatever is still standing is what
// Decide fails on. A range is owned only when it lies wholly inside a declared span of a party that is
// actually loaded.
void Attribute(Sc7Result& out, const ThirdPartySet* parties, size_t count);

// The denominator, taken from the declaration and from nothing else: every config_keys.inc row that can
// only be a patch in echovr.exe's .text (Level 1 and Level 2) whose config key is on. It never consults
// the site table or the image, which is what stops the thing being measured from supplying the number it
// is measured against (S9.12) - and it makes P12's control ("the shipping default declares exactly row
// 2.3") answerable without a game. A row whose declaration is not knowable yet lands in `deferred`
// instead of either denominator - see DeferredRow.
void DeclaredRows(Sc7Result& out);

// Pure. Turns the declaration and the measurement into one of the three words. `armedLedgerDetours` is
// how many Level-1/2 sites are actually armed, which is what makes an empty declaration a FAIL rather
// than a shrug.
void Decide(Sc7Result& out);

// Gathers the real inputs - the mapped module, the file behind it, its .reloc table, hookmgr's sites and
// the config - and calls Compare + Decide. Returns false when it refused; `out.reason` says why either
// way. Worker thread only: it reads and hashes tens of megabytes.
bool ComputeTextDiff(Sc7Result& out);

// Writes `<dir>\sc7_text_diff.json`. The directory is the caller's to choose because the harness names
// it (REPLAY_PROBE_DIRS); this is the probe artifact protocol, not plugin state, which is why it does
// not go through paths::IsWritablePath.
bool WriteJson(const wchar_t* dir, const Sc7Result& result);

// Folds one pass's measurement into the run history and rewrites what the file will say. A FAIL seen on
// any pass stands for the life of the process: a later PASS does not retract it (the patch may since have
// been removed, which is exactly the case worth reporting) and neither does a later REFUSAL (a transient
// read failure is not evidence of innocence - it is no evidence at all). When an earlier FAIL is carried
// forward the whole failing measurement is republished, ranges and rows included, so the file that says
// FAIL also shows what failed; `passes` carries this pass's own word and reason. Exposed so the sequence
// FAIL-then-refusal can be driven without a game.
void RecordPass(Sc7Result& result);

// The shipping path. Computes, folds through RecordPass, writes to this launch's artifact directory if
// the harness named one and to the state directory otherwise, and logs the verdict and the cost. Called
// from the bootstrap worker after every module's Init, and once again at a late tick - never from DllMain
// and never from the game tick.
void RunPass(const char* passName);

void ResetForTest();

// Exactly the bytes WriteJson would put on disk for `result` at buffer capacity `cap`: the report when
// it fits, the named COULD-NOT-ASK refusal when it does not, 0 when not even the refusal fits.
// `refused` (optional) says which. `cap` is a parameter here only so the OVERFLOW BOUNDARY CAN BE
// SWEPT: AUDIT_L5FIX_R2 W1C-R2-1 is that a fixed 32 KB cap lets one record land the boundary in one
// place, so whichever emission that record happens to overflow on is the only one ever tested --
// while sweeping the cap by 1 walks the boundary through every emission in the document, the final
// format literal included. No shipping caller passes anything but kJsonCap.
size_t SerializeForTest(const Sc7Result& result, char* buf, size_t cap, bool* refused);

}  // namespace sc7
}  // namespace replay
