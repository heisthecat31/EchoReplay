#include "bootstrap/offlinelock.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

#include <MinHook.h>

#include "bootstrap/config.h"
#include "bootstrap/log.h"
#include "generated/facts_generated.h"

namespace replay {
namespace offlinelock {
namespace {

constexpr uint64_t kBit = facts::SESSION_OFFLINE_FLAG_BIT;
constexpr uint32_t kFlagOff = facts::SESSION_OFFLINE_FLAG_OFF;

// The two clearing instructions the design expects. Not a threshold: finding a different number means
// this is not the image the registry describes, and the honest answer is to refuse rather than to hook
// whatever turned up.
constexpr uint32_t kExpectedSites = 2;

// ★ The instrument lives in shared memory, named per process, because the counters must describe the
// PROCESS and not the module that happens to be reading them.
//
// Every probe DLL links this library, so an offline-local launch runs Apply() three times over. Exactly
// one module can hold the trampoline; the others must still be able to answer "did the teardown run, and
// did the bit survive it", and the module that owns the hook is not necessarily the one that files the
// report. Counters in module statics would give each probe its own private zero.
struct Instrument {
    uint32_t version;
    volatile LONG installed;   // a module has the trampoline and these numbers mean something
    volatile LONG64 calls;
    volatile LONG64 clearsPrevented;
    volatile LONG64 callsBitAlreadyClear;
    volatile LONG64 callsBitUntouched;
    volatile LONG64 restoreFailed;
    volatile LONG64 functionVa;
};

constexpr uint32_t kInstrumentVersion = 1;

struct State {
    Report      report;
    Instrument  fallback;      // used only if the mapping could not be created
    Instrument* shared;
    HANDLE      mapping;
    bool        attempted;
    void*       hookTarget;
};

State g;

Instrument& Inst() { return (g.shared != nullptr) ? *g.shared : g.fallback; }

// Creates the per-process instrument, or opens the one another module already made. Returns true if
// THIS call created it, which is what elects the module that installs the trampoline.
bool ClaimInstrument() {
    wchar_t name[64];
    _snwprintf_s(name, sizeof name / sizeof name[0], _TRUNCATE,
                 L"Local\\echoreplay.offlinelock.%lu", GetCurrentProcessId());
    g.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                   sizeof(Instrument), name);
    if (g.mapping == nullptr) {
        g.report.instrumentShared = false;
        return true;   // no sharing available; behave as the sole module
    }
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    g.shared = static_cast<Instrument*>(
        MapViewOfFile(g.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Instrument)));
    if (g.shared == nullptr) {
        CloseHandle(g.mapping);
        g.mapping = nullptr;
        g.report.instrumentShared = false;
        return true;
    }
    g.report.instrumentShared = true;
    if (!existed) {
        g.shared->version = kInstrumentVersion;
    }
    return !existed;
}

using TeardownFn = uint64_t(__fastcall*)(void*);
TeardownFn g_realTeardown;

volatile uint64_t* FlagsOf(void* game) {
    return reinterpret_cast<volatile uint64_t*>(static_cast<uint8_t*>(game) + kFlagOff);
}

// ★ The lock. It restores the bit only if the bit was SET when the teardown was entered, which is what
// keeps it inert in a session that never went offline - there is nothing to put back, so nothing is
// written and normal online play is untouched.
//
// The restore is InterlockedOr64 rather than a read-modify-write of the value we already have: another
// thread may own this word between our two reads, and a plain store built from a stale copy would drop
// its write. The atomic's return value is also the measurement - it tells us whether the bit had in fact
// gone, so the counter is a consequence of the fix rather than a second guess at it.
uint64_t __fastcall HookSessionTeardown(void* game) {
    if (game == nullptr) {
        return g_realTeardown(game);
    }
    volatile uint64_t* flags = FlagsOf(game);
    const uint64_t before = *flags;
    const uint64_t result = g_realTeardown(game);

    Instrument& in = Inst();
    InterlockedIncrement64(&in.calls);
    if ((before & kBit) == 0) {
        InterlockedIncrement64(&in.callsBitAlreadyClear);
        return result;
    }
    const LONG64 prev = InterlockedOr64(reinterpret_cast<volatile LONG64*>(flags),
                                        static_cast<LONG64>(kBit));
    if ((static_cast<uint64_t>(prev) & kBit) == 0) {
        InterlockedIncrement64(&in.clearsPrevented);
        if ((*flags & kBit) == 0) {
            // The write did not take. Nothing in this design explains that, so it is reported rather
            // than retried: a lock that silently loops is worse than one that says it failed.
            InterlockedIncrement64(&in.restoreFailed);
        }
    } else {
        InterlockedIncrement64(&in.callsBitUntouched);
    }
    return result;
}

struct TextRange {
    const uint8_t* base;
    size_t         size;
    uint64_t       va;
};

bool MainModuleText(TextRange* out) {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (exe == nullptr) {
        return false;
    }
    const uint8_t* image = reinterpret_cast<const uint8_t*>(exe);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const IMAGE_NT_HEADERS64* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::memcmp(sec[i].Name, ".text", 5) == 0) {
            out->base = image + sec[i].VirtualAddress;
            out->size = sec[i].Misc.VirtualSize;
            out->va = reinterpret_cast<uint64_t>(out->base);
            return true;
        }
    }
    return false;
}

// x64 encodings: `REX.W [+B] 83 /4 disp32 imm8` and `REX.W [+B] 81 /4 disp32 imm32`, i.e. AND of a
// qword at [base + disp32] with a sign-extended immediate. mod must be 10 for a disp32, and rm 100
// would introduce a SIB byte and a different length, so it is skipped rather than mis-decoded.
constexpr uint8_t kRexW = 0x48;
constexpr uint8_t kRexWB = 0x49;
constexpr uint8_t kOpImm8 = 0x83;
constexpr uint8_t kOpImm32 = 0x81;
constexpr uint8_t kModrmAndDisp32 = 0xA0;   // mod=10, reg=100 (the /4 AND slot)
constexpr uint8_t kModrmRmSib = 0x04;

uint64_t ReadU32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof v);
    return v;
}

int32_t ReadI32(const uint8_t* p) {
    int32_t v = 0;
    std::memcpy(&v, p, sizeof v);
    return v;
}

// Emits `mov <reg>, rcx`, i.e. 8B /r with mod=11 and rm=rcx. REX.R extends the destination.
size_t EmitMovRegFromRcx(uint8_t* out, uint8_t reg) {
    out[0] = static_cast<uint8_t>(kRexW | ((reg >= 8) ? 0x04 : 0x00));
    out[1] = 0x8B;
    out[2] = static_cast<uint8_t>(0xC0 | ((reg & 7) << 3) | 0x01);
    return 3;
}

size_t EmitPush(uint8_t* out, uint8_t reg) {
    size_t n = 0;
    if (reg >= 8) {
        out[n++] = 0x41;
    }
    out[n++] = static_cast<uint8_t>(0x50 + (reg & 7));
    return n;
}

size_t EmitPop(uint8_t* out, uint8_t reg) {
    size_t n = 0;
    if (reg >= 8) {
        out[n++] = 0x41;
    }
    out[n++] = static_cast<uint8_t>(0x58 + (reg & 7));
    return n;
}

}  // namespace

const char* LockStateName(LockState state) {
    switch (state) {
        case LockState::NotAttempted:     return "not-attempted";
        case LockState::Locked:           return "locked";
        case LockState::AlreadyLockedInProcess: return "already-locked-in-process";
        case LockState::RefusedByConfig:  return "refused-by-config";
        case LockState::RefusedNoImage:   return "refused-no-image";
        case LockState::RefusedNoSites:   return "refused-no-sites";
        case LockState::RefusedSiteSpread:return "refused-site-spread";
        case LockState::RefusedSelfTest:  return "refused-self-test";
        case LockState::RefusedByMinHook: return "refused-by-minhook";
    }
    return "?";
}

const char* ExerciseName(Exercise state) {
    switch (state) {
        case Exercise::NotExercised:         return "not-exercised";
        case Exercise::RanWithNothingToHold: return "ran-with-nothing-to-hold";
        case Exercise::HeldUnderExercise:    return "held-under-exercise";
        case Exercise::Failed:               return "failed";
    }
    return "?";
}

size_t FindClearSites(const uint8_t* text, size_t len, uint64_t textVa, Site* out, size_t outCap) {
    size_t found = 0;
    if (text == nullptr || len < 8) {
        return 0;
    }
    for (size_t i = 0; i + 8 <= len; ++i) {
        const uint8_t rex = text[i];
        if (rex != kRexW && rex != kRexWB) {
            continue;
        }
        const uint8_t op = text[i + 1];
        if (op != kOpImm8 && op != kOpImm32) {
            continue;
        }
        const uint8_t modrm = text[i + 2];
        if ((modrm & 0xF8) != kModrmAndDisp32 || (modrm & 0x07) == kModrmRmSib) {
            continue;
        }
        if (ReadU32(text + i + 3) != kFlagOff) {
            continue;
        }
        uint64_t mask = 0;
        uint8_t length = 0;
        if (op == kOpImm8) {
            mask = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(text[i + 7])));
            length = 8;
        } else {
            if (i + 11 > len) {
                continue;
            }
            mask = static_cast<uint64_t>(static_cast<int64_t>(ReadI32(text + i + 7)));
            length = 11;
        }
        // An AND whose mask keeps the bit is not a clearer. This is what excludes the four
        // `and [reg+OFF], ~1` instructions that clear only the online bit.
        if ((mask & kBit) != 0) {
            continue;
        }
        if (found < outCap) {
            out[found].va = textVa + i;
            out[found].mask = static_cast<uint8_t>(mask & 0xFFu);
            out[found].baseReg = static_cast<uint8_t>((modrm & 0x07) | ((rex == kRexWB) ? 0x08 : 0x00));
            out[found].length = length;
        }
        ++found;
    }
    return found;
}

bool RunSiteOnScratch(const uint8_t* insn, size_t insnLen, uint8_t baseReg, uint64_t seed,
                      uint64_t* out) {
    if (insn == nullptr || out == nullptr || insnLen == 0 || insnLen > 16 || baseReg > 15) {
        return false;
    }
    // The object the instruction will address. It only ever touches [base + kFlagOff], but the whole
    // span is committed so a mis-decoded displacement faults here rather than somewhere interesting.
    const size_t scratchSize = kFlagOff + 64;
    uint8_t* scratch = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, scratchSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    uint8_t* stub = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (scratch == nullptr || stub == nullptr) {
        if (scratch != nullptr) VirtualFree(scratch, 0, MEM_RELEASE);
        if (stub != nullptr) VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }

    size_t n = 0;
    n += EmitPush(stub + n, baseReg);              // the base may be callee-saved
    n += EmitMovRegFromRcx(stub + n, baseReg);     // point it at the scratch object
    std::memcpy(stub + n, insn, insnLen);
    n += insnLen;
    n += EmitPop(stub + n, baseReg);
    stub[n++] = 0xC3;                              // ret
    FlushInstructionCache(GetCurrentProcess(), stub, n);

    *reinterpret_cast<uint64_t*>(scratch + kFlagOff) = seed;
    reinterpret_cast<void(__fastcall*)(void*)>(stub)(scratch);
    *out = *reinterpret_cast<uint64_t*>(scratch + kFlagOff);

    VirtualFree(stub, 0, MEM_RELEASE);
    VirtualFree(scratch, 0, MEM_RELEASE);
    return true;
}

LockState Apply() {
    if (g.attempted) {
        return g.report.state;
    }
    g.attempted = true;
    g.report.sitesExpected = kExpectedSites;

    if (!config::Interface()->GetBool("bootstrap.offline_bit_lock", true)) {
        g.report.state = LockState::RefusedByConfig;
        log::Direct(LogLevel::Warn, "offlinelock",
                    "disabled by config: the offline mode bit stays clearable for this run");
        return g.report.state;
    }

    // ★ Who installs. The module that CREATES the shared instrument installs the trampoline; the others
    // defer to it and report what it observed. A later module still scans, because "how many clearing
    // sites does this image have" is a question about the image and its answer belongs in every report -
    // but it does not try to hook a target that is already hooked, and it does not call that a refusal.
    const bool ours = ClaimInstrument();
    if (!ours && InterlockedCompareExchange(&Inst().installed, 0, 0) != 0) {
        g.report.state = LockState::AlreadyLockedInProcess;
        g.report.functionVa = static_cast<uint64_t>(InterlockedCompareExchange64(
            &Inst().functionVa, 0, 0));
        log::Direct(LogLevel::Info, "offlinelock",
                    "another module in this process already holds the lock on 0x%llX; deferring to it "
                    "and reporting its instrument rather than claiming a second one",
                    static_cast<unsigned long long>(g.report.functionVa));
        return g.report.state;
    }

    TextRange text{};
    if (!MainModuleText(&text)) {
        g.report.state = LockState::RefusedNoImage;
        log::Direct(LogLevel::Error, "offlinelock", "could not locate .text in the main module");
        return g.report.state;
    }
    g.report.scanBytes = static_cast<uint32_t>(text.size);

    const uint64_t scanStart = GetTickCount64();
    Site sites[kMaxSites];
    const size_t found = FindClearSites(text.base, text.size, text.va, sites, kMaxSites);
    g.report.scanMs = GetTickCount64() - scanStart;
    g.report.sitesFound = static_cast<uint32_t>(found);
    g.report.sitesRecorded = static_cast<uint32_t>(found < kMaxSites ? found : kMaxSites);
    for (size_t i = 0; i < g.report.sitesRecorded; ++i) {
        g.report.sites[i] = sites[i];
    }

    if (found == 0) {
        g.report.state = LockState::RefusedNoSites;
        log::Direct(LogLevel::Error, "offlinelock",
                    "no instruction in .text clears the offline bit; this is not the image the "
                    "registry describes, so nothing is hooked");
        return g.report.state;
    }
    if (found != kExpectedSites) {
        log::Direct(LogLevel::Warn, "offlinelock",
                    "expected %u clearing site(s) and found %llu; hooking the function they share, but "
                    "the surprise is the signal here",
                    kExpectedSites, static_cast<unsigned long long>(found));
    }

    // The function they live in, taken from the image's own unwind data rather than from a number we
    // wrote down. Every site must resolve to the same function: two different clearing routines would
    // need two hooks, and quietly locking one of them is the failure this refusal exists to prevent.
    uint64_t functionVa = 0;
    for (size_t i = 0; i < g.report.sitesRecorded; ++i) {
        DWORD64 imageBase = 0;
        RUNTIME_FUNCTION* rf = RtlLookupFunctionEntry(sites[i].va, &imageBase, nullptr);
        const uint64_t start = (rf != nullptr) ? (imageBase + rf->BeginAddress) : 0;
        if (start == 0 || (functionVa != 0 && start != functionVa)) {
            g.report.state = LockState::RefusedSiteSpread;
            log::Direct(LogLevel::Error, "offlinelock",
                        "the clearing sites do not share one function (site %llu at 0x%llX resolves to "
                        "0x%llX, expected 0x%llX); refusing rather than locking only part of the path",
                        static_cast<unsigned long long>(i),
                        static_cast<unsigned long long>(sites[i].va),
                        static_cast<unsigned long long>(start),
                        static_cast<unsigned long long>(functionVa));
            return g.report.state;
        }
        functionVa = start;
    }
    g.report.functionVa = functionVa;

    // ★ The known-answer control, on the bytes actually found in this image rather than on a
    // transcription of them. The located instruction MUST clear the bit, and the same harness MUST
    // report "held" when the one immediate that matters is flipped to preserve it. A harness that
    // answered the same way to both would prove nothing about either.
    const uint8_t* insn = reinterpret_cast<const uint8_t*>(sites[0].va);
    uint8_t preserving[16];
    std::memcpy(preserving, insn, sites[0].length);
    preserving[sites[0].length - 1] = static_cast<uint8_t>(preserving[sites[0].length - 1] | kBit);

    // Every bit set, so the result shows exactly which bits each mask clears rather than only the one
    // we are asking about.
    const uint64_t seed = ~0ull;
    uint64_t afterOriginal = 0;
    uint64_t afterPreserving = 0;
    const bool ranA = RunSiteOnScratch(insn, sites[0].length, sites[0].baseReg, seed, &afterOriginal);
    const bool ranB = RunSiteOnScratch(preserving, sites[0].length, sites[0].baseReg, seed,
                                       &afterPreserving);
    g.report.selfTestRan = ranA && ranB;
    g.report.selfTestClearedByOriginal = ranA && (afterOriginal & kBit) == 0;
    g.report.selfTestHeldWhenPreserved = ranB && (afterPreserving & kBit) != 0;
    if (!g.report.selfTestRan || !g.report.selfTestClearedByOriginal ||
        !g.report.selfTestHeldWhenPreserved) {
        g.report.state = LockState::RefusedSelfTest;
        log::Direct(LogLevel::Error, "offlinelock",
                    "the located bytes did not behave as the design says (ran=%d cleared=%d held=%d); "
                    "refusing to hook on an instrument that is not working",
                    static_cast<int>(g.report.selfTestRan),
                    static_cast<int>(g.report.selfTestClearedByOriginal),
                    static_cast<int>(g.report.selfTestHeldWhenPreserved));
        return g.report.state;
    }

    // Recorded, never fatal. A foreign detour on this function is the coexistence case the design chose
    // to accept, and MinHook relocates the prologue rather than overwriting whatever is there.
    const uint8_t* prologue = reinterpret_cast<const uint8_t*>(functionVa);
    g.report.foreignPrologue = prologue[0] == 0xE9 || prologue[0] == 0xEB ||
                               (prologue[0] == 0xFF && prologue[1] == 0x25) ||
                               (prologue[0] == 0x48 && prologue[1] == 0xB8);
    if (g.report.foreignPrologue) {
        log::Direct(LogLevel::Warn, "offlinelock",
                    "the teardown at 0x%llX already carries a foreign detour; chaining onto it",
                    static_cast<unsigned long long>(functionVa));
    }

    g.hookTarget = reinterpret_cast<void*>(functionVa);
    if (MH_CreateHook(g.hookTarget, reinterpret_cast<void*>(&HookSessionTeardown),
                      reinterpret_cast<void**>(&g_realTeardown)) != MH_OK ||
        MH_EnableHook(g.hookTarget) != MH_OK) {
        g.hookTarget = nullptr;
        g.report.state = LockState::RefusedByMinHook;
        log::Direct(LogLevel::Error, "offlinelock",
                    "MinHook refused the teardown at 0x%llX; the offline bit stays clearable",
                    static_cast<unsigned long long>(functionVa));
        return g.report.state;
    }

    g.report.state = LockState::Locked;
    g.report.installedHere = true;
    InterlockedExchange64(&Inst().functionVa, static_cast<LONG64>(functionVa));
    InterlockedExchange(&Inst().installed, 1);
    log::Direct(LogLevel::Info, "offlinelock",
                "locked: %u clearing site(s) found in %u byte(s) of .text in %llu ms, all inside the "
                "teardown at 0x%llX, which is now chained. The offline bit cannot be cleared by that "
                "path for the life of this process",
                g.report.sitesFound, g.report.scanBytes,
                static_cast<unsigned long long>(g.report.scanMs),
                static_cast<unsigned long long>(functionVa));
    return g.report.state;
}

const Report& Current() {
    Instrument& in = Inst();
    g.report.calls = static_cast<uint64_t>(InterlockedCompareExchange64(&in.calls, 0, 0));
    g.report.clearsPrevented =
        static_cast<uint64_t>(InterlockedCompareExchange64(&in.clearsPrevented, 0, 0));
    g.report.callsBitAlreadyClear =
        static_cast<uint64_t>(InterlockedCompareExchange64(&in.callsBitAlreadyClear, 0, 0));
    g.report.callsBitUntouched =
        static_cast<uint64_t>(InterlockedCompareExchange64(&in.callsBitUntouched, 0, 0));
    return g.report;
}

Exercise Exercised() {
    Instrument& in = Inst();
    if (InterlockedCompareExchange64(&in.restoreFailed, 0, 0) != 0) {
        return Exercise::Failed;
    }
    if (InterlockedCompareExchange64(&in.calls, 0, 0) == 0) {
        return Exercise::NotExercised;
    }
    if (InterlockedCompareExchange64(&in.clearsPrevented, 0, 0) != 0) {
        return Exercise::HeldUnderExercise;
    }
    return Exercise::RanWithNothingToHold;
}

void ReportLockedForTest() { g.report.state = LockState::Locked; }

void ResetForTest() {
    if (g.hookTarget != nullptr) {
        MH_DisableHook(g.hookTarget);
        MH_RemoveHook(g.hookTarget);
        g.hookTarget = nullptr;
    }
    if (g.shared != nullptr) {
        UnmapViewOfFile(g.shared);
        g.shared = nullptr;
    }
    if (g.mapping != nullptr) {
        CloseHandle(g.mapping);
        g.mapping = nullptr;
    }
    g.report = Report{};
    g.fallback = Instrument{};
    g.attempted = false;
    g_realTeardown = nullptr;
}

}  // namespace offlinelock
}  // namespace replay
