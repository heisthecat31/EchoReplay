#include "bootstrap/buildguard.h"
#include "bootstrap/pageprobe.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "bootstrap/log.h"
#include "bootstrap/md5.h"
#include "bootstrap/paths.h"
#include "generated/facts_generated.h"

namespace replay {
namespace {

// The gold master, from R11's PE inspection and re-checked against the file by tests/ledgerkeys on every
// gate run. It is not a facts.tsv row because the registry's types are u64, u32 and bytes20 - a 32-digit
// digest and a 29-byte string are not representable in any of them, and a row that had to be split into
// halves to fit would be exactly the "one truth, two ids" drift S4.6 forbids. The check compares this
// against the real binary AND against tools/gen_facts.py's own pin, so two sources have to agree.
const char kExpectedMd5[] = "caa05ee3bca2f5df0809994e93e2cc05";
const char kVersionString[] = "RAD_Version: goldmaster631547";

struct Site {
    const char*    factId;
    uint64_t       pcVa;
    const uint8_t* prologue;
    size_t         prologueLen;
};

#define REPLAY_BUILD_SITE(ID) {#ID, facts::ID, facts::ID##_PROLOGUE, sizeof facts::ID##_PROLOGUE},

const Site kSites[] = {
#include "bootstrap/build_sites.inc"
};

#undef REPLAY_BUILD_SITE

constexpr size_t kSiteCount = sizeof kSites / sizeof kSites[0];

// ★ Pinned, not floored. The set is the in-memory half of the build fingerprint, and it could
// previously be cut from ten sites to six with every check still green. Removing one is a real weakening
// and has to be a deliberate edit in two places.
static_assert(kSiteCount == 10, "build_sites.inc changed size; tests/ledgerkeys pins the same number");

struct State {
    bool          checked;
    BuildVerdict  verdict;
    BuildIdentity identity;
};

State g_state;

// Relaxed because nothing orders on it: it is read by tests and by the cost instrumentation, and
// Readable is called from the game thread and the worker both.
std::atomic<uint64_t> g_readableProbes{0};

}  // namespace

// True when [p, p+len) is committed, readable and not a guard page. The image is mapped by the loader
// and its sections are readable, but a wrong build can put a fact's address in a hole - and faulting
// inside a check whose whole job is to refuse safely would be the joke of the packet.
bool Readable(const void* p, size_t len) {
    g_readableProbes.fetch_add(1, std::memory_order_relaxed);
    // Zero length is not "readable", it is "nothing was asked". Returning true here is what let
    // VerifySite agree with an empty expectation, so the vacuous answer is refused at the source too.
    if (p == nullptr || len == 0) {
        return false;
    }
    return PageSpanReadable(p, len);
}

uint64_t ReadableProbeCount() { return g_readableProbes.load(std::memory_order_relaxed); }

namespace {

// Latched: the exe's base cannot move while the process lives, and MappedAddress asks for it twice on
// every call from a per-frame UI site (PERF-1).
std::atomic<const uint8_t*> g_mainImageBase{nullptr};

const uint8_t* MainImageBase() {
    const uint8_t* base = g_mainImageBase.load(std::memory_order_relaxed);
    if (base == nullptr) {
        base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        g_mainImageBase.store(base, std::memory_order_relaxed);
    }
    return base;
}

// facts.tsv records PC virtual addresses against the image base the binary declares. The loader may map
// it somewhere else, so every address is rebased before it is read; a guard that assumed the preferred
// base would refuse a correct build the moment ASLR moved it.
const uint8_t* Rebase(uint64_t pcVa) {
    const uint8_t* base = MainImageBase();
    if (base == nullptr || pcVa < facts::PE_IMAGE_BASE) {
        return nullptr;
    }
    return base + (pcVa - facts::PE_IMAGE_BASE);
}

// Counts occurrences of the version string in the mapped image's initialised-data sections. The string
// occurs exactly once in the shipped binary (verified against the file), so "exactly one" is a stronger
// statement than "at least one": a build that carried it twice would not be this build.
uint32_t CountVersionString(uint32_t* sectionsScanned) {
    *sectionsScanned = 0;
    const uint8_t* base = MainImageBase();
    if (base == nullptr) {
        return 0;
    }
    if (!Readable(base, sizeof(IMAGE_DOS_HEADER))) {
        return 0;
    }
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return 0;
    }
    const IMAGE_NT_HEADERS64* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (!Readable(nt, sizeof *nt) || nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }

    const size_t needle = sizeof kVersionString - 1;
    uint32_t hits = 0;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!Readable(section, sizeof *section)) {
            break;
        }
        if ((section->Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA) == 0) {
            continue;
        }
        const size_t size = section->Misc.VirtualSize != 0 ? section->Misc.VirtualSize
                                                           : section->SizeOfRawData;
        const uint8_t* start = base + section->VirtualAddress;
        if (size < needle || !Readable(start, size)) {
            continue;
        }
        ++*sectionsScanned;
        const uint8_t* limit = start + size - needle;
        for (const uint8_t* p = start; p <= limit; ++p) {
            if (*p == static_cast<uint8_t>(kVersionString[0]) &&
                std::memcmp(p, kVersionString, needle) == 0) {
                ++hits;
            }
        }
    }
    return hits;
}

}  // namespace

bool LooksLikeForeignHook(const uint8_t* actual, size_t len) {
    if (actual == nullptr || len == 0) {
        return false;
    }
    if (actual[0] == 0xE9u || actual[0] == 0xEBu) {
        return true;   // JMP rel32, or a short JMP into a patch written above the function
    }
    if (len >= 2 && actual[0] == 0xFFu && actual[1] == 0x25u) {
        return true;   // indirect JMP through a pointer
    }
    if (len >= 2 && actual[0] == 0x48u && actual[1] == 0xB8u) {
        return true;   // mov rax, imm64 - a 64-bit trampoline preamble
    }
    return false;
}

// Without this, "the bytes differ" has two indistinguishable meanings and the guard cannot answer either
// of its questions correctly. rlay.dll, which shares plugins\ with us, is a Detours DLL: its patches
// start with a branch, and that is what tells a foreign hook apart from a genuinely different binary.
// Decodes the branch a foreign hook wrote and names the module that owns its destination.
//
// ★ This is what lets ONE run tell "another mod hooked this" from "wrong executable".
// Before it, the two were only distinguishable by running twice - once with the other plugin displaced -
// which is a property of how the sweep was driven, not of the guard. A user with rlay.dll installed and
// no second run was told "wrong build" about a build that was fine.
const char* OwningModuleOfBranch(const uint8_t* at, size_t len, char* out, size_t outCap) {
    const uint8_t* dest = nullptr;
    if (len >= 5 && at[0] == 0xE9u) {
        int32_t rel = 0;
        std::memcpy(&rel, at + 1, sizeof rel);
        dest = at + 5 + rel;
    } else if (len >= 2 && at[0] == 0xEBu) {
        dest = at + 2 + static_cast<int8_t>(at[1]);
    } else if (len >= 6 && at[0] == 0xFFu && at[1] == 0x25u) {
        int32_t rel = 0;
        std::memcpy(&rel, at + 2, sizeof rel);
        const uint8_t* slot = at + 6 + rel;
        if (Readable(slot, sizeof(void*))) {
            std::memcpy(&dest, slot, sizeof dest);
        }
    } else if (len >= 10 && at[0] == 0x48u && at[1] == 0xB8u) {
        uint64_t imm = 0;
        std::memcpy(&imm, at + 2, sizeof imm);
        dest = reinterpret_cast<const uint8_t*>(imm);
    }
    if (dest == nullptr) {
        return nullptr;
    }
    HMODULE owner = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(dest), &owner) ||
        owner == nullptr) {
        return nullptr;
    }
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(owner, path, sizeof path) == 0) {
        return nullptr;
    }
    const char* leaf = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '\\' || *p == '/') {
            leaf = p + 1;
        }
    }
    // A trampoline landing back inside echovr.exe is not a foreign module; say so rather than naming
    // the game as the culprit.
    if (owner == GetModuleHandleW(nullptr)) {
        return nullptr;
    }
    std::snprintf(out, outCap, "%s", leaf);
    return out;
}

const char* ClassifyMismatch(const uint8_t* actual, size_t len) {
    if (actual == nullptr || len == 0) {
        return "nothing was compared";
    }
    if (actual[0] == 0xE9u) {
        return "another module has hooked this site (JMP rel32 - the shape Detours and MinHook write)";
    }
    if (actual[0] == 0xEBu) {
        return "another module has hooked this site (short JMP into a patch above the function)";
    }
    if (len >= 2 && actual[0] == 0xFFu && actual[1] == 0x25u) {
        return "another module has hooked this site (indirect JMP through a pointer)";
    }
    if (len >= 2 && actual[0] == 0x48u && actual[1] == 0xB8u) {
        return "another module has hooked this site (mov rax, imm64 trampoline preamble)";
    }
    if (actual[0] == 0xCCu) {
        return "a debugger breakpoint is set on this site";
    }
    return "not a branch, so not a hook this classifier recognises - the bytes are simply different";
}

const char* ExpectedMd5() { return kExpectedMd5; }
const char* ExpectedVersionString() { return kVersionString; }

BuildVerdict EvaluateBuild(const BuildIdentity& id, char* why, size_t whyCap) {
    // Order matters: report the most specific true statement. "the exe is a different build" is a better
    // diagnosis than "site 4 disagreed", and both are better than a bare refusal.
    if (!id.md5Read) {
        std::snprintf(why, whyCap, "could not read the main module to hash it; refusing rather than "
                                   "guessing which build this is");
        return BuildVerdict::RefuseUnreadable;
    }
    if (std::strcmp(id.md5, kExpectedMd5) != 0) {
        std::snprintf(why, whyCap,
                      "this is not the build every address in the registry was read from: md5 %s, "
                      "expected %s (%llu bytes)", id.md5, kExpectedMd5,
                      static_cast<unsigned long long>(id.fileSize));
        return BuildVerdict::RefuseWrongBuild;
    }
    if (!id.imageScanned) {
        std::snprintf(why, whyCap, "could not scan the loaded image for the build string");
        return BuildVerdict::RefuseUnreadable;
    }
    if (id.versionStringCount != 1u) {
        std::snprintf(why, whyCap,
                      "the loaded image holds \"%s\" %u time(s); the gold master holds it exactly once",
                      kVersionString, id.versionStringCount);
        return BuildVerdict::RefuseWrongBuild;
    }
    // ★ Past this point the build is IDENTIFIED. md5 names the file and the build string
    // names the mapped image; nothing a prologue sweep can say changes which executable this is.
    //
    // The sweep is reported, never fatal. Orchestrator ruling 2026-08-28: refusing to load because
    // another mod had hooked one of these sites broke "coexist, never replace" and SC9, and bought
    // nothing - a foreign detour is a site-integrity fact, and site integrity gates one hook, not the
    // load. VerifySite still refuses any individual site we are about to patch.
    if (id.siteMismatches == 0u) {
        std::snprintf(why, whyCap, "md5 %s, build string present once, %u/%u fingerprint sites match",
                      id.md5, id.siteCount, id.siteCount);
    } else if (id.siteUnexplained == 0u) {
        std::snprintf(why, whyCap,
                      "md5 %s, build string present once. %u of %u fingerprint site(s) carry another "
                      "module's hook (first %s) - coexisting, not refusing",
                      id.md5, id.siteForeignDetours, id.siteCount, id.firstMismatch);
    } else {
        // Identified, but something changed those bytes that does not look like a hook. Worth saying
        // loudly; still not grounds to refuse a build the md5 has already identified.
        std::snprintf(why, whyCap,
                      "md5 %s, build string present once. %u of %u fingerprint site(s) differ, %u of them "
                      "not recognisable as a hook (first %s) - loading anyway; the md5 is decisive",
                      id.md5, id.siteMismatches, id.siteCount, id.siteUnexplained, id.firstMismatch);
    }
    return BuildVerdict::Accept;
}

namespace buildguard {

BuildIdentity Inspect() {
    BuildIdentity id{};

    wchar_t exePath[kPathMax];
    const DWORD written = GetModuleFileNameW(nullptr, exePath, kPathMax);
    if (written != 0 && written < kPathMax) {
        exePath[written] = L'\0';
        id.md5Read = Md5File(exePath, id.md5, &id.fileSize);
    }

    uint32_t sections = 0;
    id.versionStringCount = CountVersionString(&sections);
    id.imageScanned = sections > 0;

    id.siteCount = static_cast<uint32_t>(kSiteCount);
    for (size_t i = 0; i < kSiteCount; ++i) {
        const Site& site = kSites[i];
        const uint8_t* at = Rebase(site.pcVa);
        const bool readable = at != nullptr && Readable(at, site.prologueLen);
        if (!readable || std::memcmp(at, site.prologue, site.prologueLen) != 0) {
            if (id.siteMismatches == 0u) {
                std::snprintf(id.firstMismatch, sizeof id.firstMismatch, "%s @ 0x%llX", site.factId,
                              static_cast<unsigned long long>(site.pcVa));
            }
            ++id.siteMismatches;
            if (readable && LooksLikeForeignHook(at, site.prologueLen)) {
                ++id.siteForeignDetours;
            } else {
                ++id.siteUnexplained;
            }
            // ★ Loud, and not fatal. Not one of these ten is a site this project hooks -
            // they are a fingerprint - so somebody else owning one costs a log line and nothing else. It
            // is still worth saying: it is a foreign patch inside the process we are about to run in.
            char owner[MAX_PATH] = {};
            const char* named =
                readable ? OwningModuleOfBranch(at, site.prologueLen, owner, sizeof owner) : nullptr;
            log::Direct(LogLevel::Warn, "buildguard",
                        "%s @ 0x%llX does not hold its recorded prologue: %s%s%s. This site is a build "
                        "fingerprint and not one we hook, so it does not stop us loading.",
                        site.factId, static_cast<unsigned long long>(site.pcVa),
                        readable ? ClassifyMismatch(at, site.prologueLen) : "not readable in this image",
                        named != nullptr ? " - the hook belongs to " : "",
                        named != nullptr ? named : "");
        }
    }
    id.sitesChecked = true;
    return id;
}

BuildVerdict Check() {
    if (g_state.checked) {
        return g_state.verdict;
    }
    const BuildIdentity id = Inspect();
    char why[512];
    g_state.verdict = EvaluateBuild(id, why, sizeof why);
    g_state.identity = id;
    g_state.checked = true;

    if (g_state.verdict == BuildVerdict::Accept) {
        log::Direct(LogLevel::Info, "buildguard", "build accepted: %s", why);
    } else {
        // Synchronous, and Error level: this is the line that has to survive whatever happens next, and
        // it is the whole diagnosis when someone reports "the plugin does nothing".
        log::Direct(LogLevel::Error, "buildguard", "REFUSED - %s", why);
        log::Direct(LogLevel::Error, "buildguard",
                    "nothing will be installed and no engine memory will be touched. This plugin is "
                    "built for one build of echovr.exe and it is not this one.");
    }
    return g_state.verdict;
}

BuildVerdict Verdict() {
    return g_state.checked ? g_state.verdict : BuildVerdict::RefuseUnreadable;
}

bool Passed() {
    return g_state.checked && g_state.verdict == BuildVerdict::Accept;
}

const BuildIdentity& LastIdentity() { return g_state.identity; }

bool VerifySite(const char* factId, uint64_t pcVa, const uint8_t* expected, size_t expectedLen) {
    if (factId == nullptr) {
        factId = "<unnamed site>";
    }
    // ★ A guard whose degenerate input reads as agreement is not a guard. Readable(p, 0) is
    // vacuously true and memcmp(a, b, 0) is zero, so without this the function would report "this site
    // holds the recorded bytes" having compared none of them - and hookmgr::Create's ladder would fall
    // straight through to MH_CreateHook and patch a site nobody checked. Every caller here is Wave-2 code
    // passing a caller-supplied length, so the degenerate case is reachable from outside this packet.
    if (expected == nullptr || expectedLen == 0) {
        log::Direct(LogLevel::Error, "buildguard",
                    "%s @ 0x%llX: refusing this site - asked to verify %llu byte(s) against %s. A check "
                    "of zero bytes is not a check.",
                    factId, static_cast<unsigned long long>(pcVa),
                    static_cast<unsigned long long>(expectedLen),
                    expected == nullptr ? "a null expectation" : "a real expectation");
        return false;
    }
    const uint8_t* at = Rebase(pcVa);
    if (at == nullptr || !Readable(at, expectedLen)) {
        log::Direct(LogLevel::Error, "buildguard", "%s @ 0x%llX is not readable in this image", factId,
                    static_cast<unsigned long long>(pcVa));
        return false;
    }
    if (std::memcmp(at, expected, expectedLen) == 0) {
        return true;
    }
    // S4.5.3 rule 4: another tool may have got here first. MinHook trampolines chain, a raw byte patch
    // does not, and we cannot tell which this is - so we refuse the site and say so, rather than writing
    // over somebody else's work and taking their tool down with ours.
    char owner[MAX_PATH] = {};
    char whose[320];
    const char* named = OwningModuleOfBranch(at, expectedLen, owner, sizeof owner);
    if (named != nullptr) {
        std::snprintf(whose, sizeof whose, "%s, and the hook belongs to %s",
                      ClassifyMismatch(at, expectedLen), named);
    } else {
        std::snprintf(whose, sizeof whose, "%s", ClassifyMismatch(at, expectedLen));
    }

    char got[80];
    char want[80];
    size_t g = 0;
    size_t w = 0;
    const size_t show = expectedLen < 8 ? expectedLen : 8;
    for (size_t i = 0; i < show; ++i) {
        g += static_cast<size_t>(std::snprintf(got + g, sizeof got - g, "%02X ", at[i]));
        w += static_cast<size_t>(std::snprintf(want + w, sizeof want - w, "%02X ", expected[i]));
    }
    log::Direct(LogLevel::Warn, "buildguard",
                "%s @ 0x%llX starts %s- expected %s- refusing this site: %s. We never overwrite one.",
                factId, static_cast<unsigned long long>(pcVa), got, want, whose);
    return false;
}

// ★ PERF-1. The headers this validates cannot change while the image is mapped, but it re-read and
// re-probed them on every call -- two VirtualQuery each, and the in-frame UI site alone resolves 14 to 18
// constant VAs per rendered frame. VirtualQuery is NtQueryVirtualMemory and takes the process
// address-space lock, so this was ~30 of those per frame for an answer that was decided at load. The
// latch is published with a release store after `end` is written, and read back under an acquire, so a
// second thread never sees a non-zero `end` with a stale base.
std::atomic<const uint8_t*> g_imageEnd{nullptr};
const uint8_t*              g_imageEndBase = nullptr;

const void* MappedAddress(uint64_t pcVa) {
    const uint8_t* at = Rebase(pcVa);
    if (at == nullptr) {
        return nullptr;
    }
    const uint8_t* base = MainImageBase();
    const uint8_t* end = g_imageEnd.load(std::memory_order_acquire);
    if (end != nullptr && g_imageEndBase == base) {
        return at < end ? at : nullptr;
    }
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!Readable(base, sizeof *dos) || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return nullptr;
    }
    const IMAGE_NT_HEADERS64* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (!Readable(nt, sizeof *nt) || nt->Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }
    g_imageEndBase = base;
    g_imageEnd.store(base + nt->OptionalHeader.SizeOfImage, std::memory_order_release);
    return (at < base + nt->OptionalHeader.SizeOfImage) ? at : nullptr;
}

size_t SiteCount() { return kSiteCount; }

const char* SiteFactId(size_t i) { return i < kSiteCount ? kSites[i].factId : nullptr; }

void SetVerdictForTest(BuildVerdict verdict) {
    g_state.verdict = verdict;
    g_state.checked = true;
}

void ResetForTest() {
    g_state.checked = false;
    g_state.verdict = BuildVerdict::RefuseUnreadable;
    g_state.identity = BuildIdentity{};
}

}  // namespace buildguard
}  // namespace replay
