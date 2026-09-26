// B-2: refuse to touch a build we did not measure.
//
// Every address, offset and prologue in facts/facts.tsv was read out of ONE file: echovr.exe md5
// caa05ee3bca2f5df0809994e93e2cc05, "RAD_Version: goldmaster631547". Applied to any other build they name
// arbitrary bytes, and the design is explicit about the outcome: "a mismatch is a clean refusal with
// a log line, never a corruption."
//
// ★ TWO DIFFERENT QUESTIONS, AND THEY MUST NOT SHARE AN ANSWER (orchestrator ruling, 2026-08-28).
//
//   "Is this the right build of echovr.exe?"  - a FINGERPRINT question, and it gates LOADING.
//       Answered by the md5 of the main module on disk, which identifies the build outright, plus the
//       "RAD_Version: goldmaster631547" string, which occurs exactly once in the shipped image and checks
//       the thing actually mapped rather than a file on disk. Two independent witnesses, both sufficient.
//
//   "Is this site safe for US to hook?"       - a SITE-INTEGRITY question, and it gates ONE HOOK.
//       Answered per site by VerifySite at install time. Its correct answer when somebody else already
//       owns the site is "refuse this hook and take the fallback", never "refuse to load".
//
// ★ WHY THIS IS SPLIT, PAID FOR ONCE. The startup sweep used to refuse the load on any prologue
// mismatch. In the 2026-08-28 sweep rlay.dll - a Detours DLL sharing plugins\ with us - had already
// hooked SESSION.STATE_MACHINE, so a correct build of the right executable was refused outright. That
// breaks the owner's "coexist, never replace" ruling and SC9, and it added nothing: the md5 had already
// answered the build question completely. Prologue sites import a false-negative source into build
// identity - any other mod's patch looks like the wrong executable - so they no longer gate loading.
//
// The sweep still runs, and it is still LOUD: every mismatch is logged with the byte classification that
// tells a foreign detour apart from a genuinely different binary. It is a report, not a veto.
//
// ★ There is no config key that turns this off. S4.4's rule is that every INTERVENTION is disableable;
// the guard is not an intervention, and a switch that lets a wrong build through would be the cheapest
// route past the one check that stands between a mismatched offset and someone's game.
//
// Note on identifying this build: echovr.exe has NO version resource - its .rsrc holds RT_ICON and
// RT_GROUP_ICON and nothing else - so GetFileVersionInfo would return nothing and a version check would
// silently pass on any build. That is why identity is md5 and a string.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace replay {

enum class BuildVerdict {
    Accept,
    RefuseWrongBuild,     // it is a different echovr.exe, or not echovr.exe at all
    RefuseUnreadable,     // we could not obtain the evidence, so we do not know - and do not proceed
};

// Everything the decision is made from, gathered separately so the decision itself is a pure function
// that a unit test can drive through every combination without a game present.
struct BuildIdentity {
    bool     md5Read;
    char     md5[33];             // lowercase hex of the main module file
    uint64_t fileSize;

    bool     imageScanned;
    uint32_t versionStringCount;  // occurrences of the RAD_Version string in the mapped image

    // The startup sweep. Reported, never fatal: these do not gate loading.
    bool     sitesChecked;
    uint32_t siteCount;
    uint32_t siteMismatches;
    uint32_t siteForeignDetours;  // mismatches whose first bytes are a branch - somebody hooked it
    uint32_t siteUnexplained;     // mismatches that do not look like a hook at all
    char     firstMismatch[128];  // fact id of the first site that disagreed, for the log line
};

// Pure. `why` receives a one-line explanation in every case, including Accept.
BuildVerdict EvaluateBuild(const BuildIdentity& id, char* why, size_t whyCap);

// The expected identity, exposed so tests/ledgerkeys can compare it against the real binary and against
// the registry's own pin rather than taking this header's word for it.
const char* ExpectedMd5();
const char* ExpectedVersionString();

// How a differing site differs. A branch at the front is somebody else's trampoline; anything else is
// unexplained. Without this, "wrong build" and "another mod got here first" are indistinguishable, and
// the guard cannot answer either question correctly. Never null.
const char* ClassifyMismatch(const uint8_t* actual, size_t len);
bool        LooksLikeForeignHook(const uint8_t* actual, size_t len);

// True when [p, p+len) is committed, readable and not a guard page, asked of the memory manager. False
// for a null pointer, a zero length (nothing was asked) and a span that wraps. The probe every check
// in this plugin uses before it dereferences engine memory; tableguard.h is built on it.
bool Readable(const void* p, size_t len);

// Every Readable call this process has made. Readable is a VirtualQuery and clock.cpp's contract
// for it is "once per take -- never on the tick path", so the number a per-tick budget is written
// against is the DIFFERENCE across a tick. Counts the calls the span cache makes too.
uint64_t ReadableProbeCount();

namespace buildguard {

// Gathers 1, 2 and 3 against the running process. Worker thread only: the md5 reads 35 MB.
BuildIdentity Inspect();

// Inspect + EvaluateBuild + log. Latches the verdict; every later caller gets the same answer.
BuildVerdict Check();
BuildVerdict Verdict();
bool         Passed();

// The evidence Check() decided on, kept so a caller can report what was actually observed rather than
// re-hashing 35 MB to ask again. Zeroed until Check() has run.
const BuildIdentity& LastIdentity();

// Per-site guard for a single address, for a caller about to hook or call it (S4.5.3 rule 4). `expected`
// is the fact's 20-byte prologue; `rvaBase` is the fact's PC_VA. Returns false and logs if the mapped
// bytes differ - which is also how another mod's hook at the same site is detected.
bool VerifySite(const char* factId, uint64_t pcVa, const uint8_t* expected, size_t expectedLen);

// facts.tsv records addresses against the image base the binary declares; the loader may map it
// somewhere else. This is the one place that rebases, so PE_IMAGE_BASE is dereferenced once and every
// other caller asks for a mapped pointer. Returns nullptr when the address is not in this image.
const void* MappedAddress(uint64_t pcVa);

// Where the site table came from, for the startup log and for the tests.
size_t      SiteCount();
const char* SiteFactId(size_t i);

void ResetForTest();

// Test seam. Sets the latched verdict as if Inspect() and EvaluateBuild() had reached it, so code
// DOWNSTREAM of the guard - hookmgr's refusal ladder above all - can be driven offline without a running
// game. This does not weaken the guard: EvaluateBuild has its own exhaustive table in tests/bootstrap and
// is not bypassed by anything here, and nothing outside tests/ calls this (grep SetVerdictForTest).
void SetVerdictForTest(BuildVerdict verdict);

}  // namespace buildguard
}  // namespace replay
