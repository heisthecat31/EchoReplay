#include "bootstrap/mainprof.h"

#include <windows.h>

#include <atomic>
#include <stdio.h>
#include <stdlib.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/config.h"
#include "bootstrap/log.h"

namespace replay {
namespace mainprof {
namespace {

constexpr uint32_t kBinShift = 12;          // 4 KB per bin
constexpr uint32_t kMaxBins = 16384;        // 64 MB of image, which covers echovr.exe's 35 MB facts-ok: SESSION.FLAG_BIT_ACTIVE 16384 is this profiler's bin count, not the session flag bit
constexpr uint32_t kMaxModules = 48;
constexpr uint32_t kSampleIntervalMs = 2;   // ~500 Hz
constexpr uint32_t kReportEveryMs = 5000;
constexpr uint32_t kMaxExports = 4096;
constexpr uint32_t kNamePool = 160 * 1024;
constexpr uint32_t kStackWords = 384;       // 3 KB of stack scanned for the calling frame facts-ok: IDENT.USER_ACTOR_GUID_OFF 384 is a word count, not a struct offset
constexpr uint32_t kMaxPairs = 512;
constexpr uint32_t kCallerShift = 8;   // 256 B, fine enough to name the calling function
constexpr uint32_t kMaxExcCodes = 16;
constexpr uint32_t kChainDepth = 3;
constexpr uint32_t kMaxChains = 384;  // facts-ok: IDENT.USER_ACTOR_GUID_OFF 384 is this profiler's chain-table capacity, not a struct offset

struct ModuleBucket {
    uintptr_t base = 0;
    uintptr_t size = 0;
    uint32_t  hits = 0;
    bool      system = false;
    wchar_t   name[64] = {};
};

// The frame the stack scan is looking for is the one that decided to make the call, not the Win32
// wrapper that forwarded it: a VirtualQuery reported as "called by KERNELBASE" names nobody.
bool IsSystemForwarder(const wchar_t* leaf) {
    static const wchar_t* kNames[] = {
        L"ntdll.dll",      L"kernelbase.dll", L"kernel32.dll", L"win32u.dll",
        L"ucrtbase.dll",   L"msvcp_win.dll",  L"user32.dll",   L"sechost.dll",
        L"rpcrt4.dll",     L"combase.dll",    L"gdi32.dll",    L"gdi32full.dll",
    };
    for (const wchar_t* candidate : kNames) {
        uint32_t i = 0;
        for (;; ++i) {
            const wchar_t a = leaf[i] >= L'A' && leaf[i] <= L'Z' ? wchar_t(leaf[i] + 32) : leaf[i];
            const wchar_t b = candidate[i];
            if (a != b) break;
            if (a == 0) return true;
        }
    }
    return false;
}

struct ExportEntry {
    uint32_t rva;
    uint32_t nameOff;
};

// One (ntdll function, calling module, caller 4 KB window) triple and how often it was sampled.
struct Pair {
    uint16_t exportIdx;
    uint16_t moduleIdx;
    uint32_t callerBin;
    uint32_t hits;
};

// Readable() is itself a frame in probe_replay.dll, so one return address names the probe and not
// the code that asked for it. Three name the site.
struct Chain {
    uint16_t exportIdx;
    uint32_t hits;
    uintptr_t at[kChainDepth];
};

struct State {
    std::atomic<uint32_t> frameThread{0};
    std::atomic<uint32_t> running{0};
    HANDLE   thread = nullptr;
    uintptr_t imageBase = 0;
    uint32_t bins[kMaxBins] = {};
    uint32_t binTotal = 0;
    uintptr_t ntdllBase = 0;
    uint32_t ntTotal = 0;
    ModuleBucket modules[kMaxModules];
    uint32_t moduleCount = 0;
    uintptr_t moduleLow = 0;
    uintptr_t moduleHigh = 0;
    uint32_t suspendFailures = 0;

    ExportEntry exports[kMaxExports];
    uint32_t exportCount = 0;
    char names[kNamePool];
    uint32_t nameUsed = 0;
    uint32_t exportHits[kMaxExports] = {};

    Pair pairs[kMaxPairs];
    uint32_t pairCount = 0;
    uint32_t pairOverflow = 0;
    Chain chains[kMaxChains];
    uint32_t chainCount = 0;
    uint32_t chainOverflow = 0;

    uintptr_t stackBase = 0;
    uintptr_t stackLimit = 0;
    uint64_t  stackCopy[kStackWords] = {};
    uint32_t  noCaller = 0;
    uint64_t  lastProbeCount = 0;

    PVOID vehHandle = nullptr;
    std::atomic<uint32_t> excTotal{0};
    std::atomic<uint32_t> excCode[kMaxExcCodes];
    std::atomic<uint32_t> excCount[kMaxExcCodes];
};

State g;

// ---------------------------------------------------------------- modules

void RefreshModules() {
    using EnumFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);
    static EnumFn enumModules = reinterpret_cast<EnumFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")));
    if (enumModules == nullptr) {
        return;
    }
    HMODULE mods[kMaxModules * 4] = {};
    DWORD needed = 0;
    if (!enumModules(GetCurrentProcess(), mods, sizeof mods, &needed)) {
        return;
    }
    const uint32_t count = needed / sizeof(HMODULE);
    for (uint32_t i = 0; i < count && g.moduleCount < kMaxModules; ++i) {
        const uintptr_t base = uintptr_t(mods[i]);
        bool known = false;
        for (uint32_t k = 0; k < g.moduleCount; ++k) {
            if (g.modules[k].base == base) { known = true; break; }
        }
        if (known) {
            continue;
        }
        ModuleBucket& m = g.modules[g.moduleCount++];
        m.base = base;
        m.hits = 0;
        const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        m.size = 0x1000;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            const IMAGE_NT_HEADERS64* nt =
                reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                m.size = nt->OptionalHeader.SizeOfImage;
            }
        }
        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameW(reinterpret_cast<HMODULE>(base), path, MAX_PATH) != 0) {
            const wchar_t* leaf = path;
            for (const wchar_t* p = path; *p != 0; ++p) {
                if (*p == L'\\' || *p == L'/') leaf = p + 1;
            }
            for (uint32_t c = 0; c + 1 < 64 && leaf[c] != 0; ++c) m.name[c] = leaf[c];
            m.system = IsSystemForwarder(m.name);
        }
    }
    g.moduleLow = 0;
    g.moduleHigh = 0;
    for (uint32_t i = 0; i < g.moduleCount; ++i) {
        if (g.moduleLow == 0 || g.modules[i].base < g.moduleLow) g.moduleLow = g.modules[i].base;
        if (g.modules[i].base + g.modules[i].size > g.moduleHigh) {
            g.moduleHigh = g.modules[i].base + g.modules[i].size;
        }
    }
}

int ModuleIndexOf(uintptr_t addr) {
    if (addr < g.moduleLow || addr >= g.moduleHigh) {
        return -1;
    }
    for (uint32_t i = 0; i < g.moduleCount; ++i) {
        if (addr >= g.modules[i].base && addr < g.modules[i].base + g.modules[i].size) {
            return int(i);
        }
    }
    return -1;
}

// ---------------------------------------------------------------- exports

int CompareExports(const void* a, const void* b) {
    const uint32_t ra = static_cast<const ExportEntry*>(a)->rva;
    const uint32_t rb = static_cast<const ExportEntry*>(b)->rva;
    return ra < rb ? -1 : (ra > rb ? 1 : 0);
}

void LoadExports(uintptr_t base) {
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos == nullptr || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    const IMAGE_NT_HEADERS64* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }
    const DWORD dirRva =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    if (dirRva == 0) {
        return;
    }
    const IMAGE_EXPORT_DIRECTORY* ed =
        reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dirRva);
    const DWORD* funcs = reinterpret_cast<const DWORD*>(base + ed->AddressOfFunctions);
    const DWORD* names = reinterpret_cast<const DWORD*>(base + ed->AddressOfNames);
    const WORD* ords = reinterpret_cast<const WORD*>(base + ed->AddressOfNameOrdinals);
    for (DWORD i = 0; i < ed->NumberOfNames && g.exportCount < kMaxExports; ++i) {
        const WORD ord = ords[i];
        if (ord >= ed->NumberOfFunctions) {
            continue;
        }
        const char* name = reinterpret_cast<const char*>(base + names[i]);
        uint32_t len = 0;
        while (name[len] != 0 && len < 47) ++len;
        if (g.nameUsed + len + 1 >= kNamePool) {
            break;
        }
        ExportEntry& e = g.exports[g.exportCount++];
        e.rva = funcs[ord];
        e.nameOff = g.nameUsed;
        for (uint32_t c = 0; c < len; ++c) g.names[g.nameUsed + c] = name[c];
        g.names[g.nameUsed + len] = 0;
        g.nameUsed += len + 1;
    }
    qsort(g.exports, g.exportCount, sizeof(ExportEntry), &CompareExports);
}

// The nearest export at or below rva: with ntdll's ~2400 named exports this names the containing
// function for everything but the static helpers, which land on the exported entry that precedes
// them.
int ExportIndexFor(uint32_t rva) {
    if (g.exportCount == 0 || rva < g.exports[0].rva) {
        return -1;
    }
    uint32_t lo = 0, hi = g.exportCount - 1;
    while (lo < hi) {
        const uint32_t mid = (lo + hi + 1) / 2;
        if (g.exports[mid].rva <= rva) lo = mid; else hi = mid - 1;
    }
    return int(lo);
}

// ---------------------------------------------------------------- stack

// x64 call sites end in E8 rel32, FF /2 (2-7 bytes) or 9A. Requiring one of those in front of a
// candidate throws out the stale pointers and spilled function addresses a raw stack scan picks up.
bool ScanCallSite(uintptr_t addr) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(addr);
    if (p[-5] == 0xE8) {
        return true;
    }
    for (int back = 2; back <= 7; ++back) {
        if (p[-back] == 0xFF) {
            const unsigned char modrm = p[-back + 1];
            if (((modrm >> 3) & 7) == 2 || ((modrm >> 3) & 7) == 3) {
                return true;
            }
        }
    }
    return false;
}

// A section that is mapped but not readable would fault the sampler and take the game with it.
bool LooksLikeReturnAddress(uintptr_t addr) {
    __try {
        return ScanCallSite(addr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RecordChain(int exportIdx, const uintptr_t* at) {
    for (uint32_t i = 0; i < g.chainCount; ++i) {
        Chain& c = g.chains[i];
        if (c.exportIdx != uint16_t(exportIdx)) {
            continue;
        }
        bool same = true;
        for (uint32_t k = 0; k < kChainDepth; ++k) {
            if (c.at[k] != at[k]) { same = false; break; }
        }
        if (same) {
            ++c.hits;
            return;
        }
    }
    if (g.chainCount >= kMaxChains) {
        ++g.chainOverflow;
        return;
    }
    Chain& c = g.chains[g.chainCount++];
    c.exportIdx = uint16_t(exportIdx);
    c.hits = 1;
    for (uint32_t k = 0; k < kChainDepth; ++k) c.at[k] = at[k];
}

void RecordPair(int exportIdx, int moduleIdx, uint32_t callerBin) {
    for (uint32_t i = 0; i < g.pairCount; ++i) {
        Pair& p = g.pairs[i];
        if (p.exportIdx == uint16_t(exportIdx) && p.moduleIdx == uint16_t(moduleIdx) &&
            p.callerBin == callerBin) {
            ++p.hits;
            return;
        }
    }
    if (g.pairCount >= kMaxPairs) {
        ++g.pairOverflow;
        return;
    }
    Pair& p = g.pairs[g.pairCount++];
    p.exportIdx = uint16_t(exportIdx);
    p.moduleIdx = uint16_t(moduleIdx);
    p.callerBin = callerBin;
    p.hits = 1;
}

// ---------------------------------------------------------------- exceptions

LONG CALLBACK CountException(EXCEPTION_POINTERS* info) {
    g.excTotal.fetch_add(1, std::memory_order_relaxed);
    const uint32_t code = info->ExceptionRecord->ExceptionCode;
    for (uint32_t i = 0; i < kMaxExcCodes; ++i) {
        uint32_t seen = g.excCode[i].load(std::memory_order_relaxed);
        if (seen == code) {
            g.excCount[i].fetch_add(1, std::memory_order_relaxed);
            return EXCEPTION_CONTINUE_SEARCH;
        }
        if (seen == 0) {
            uint32_t expected = 0;
            if (g.excCode[i].compare_exchange_strong(expected, code,
                                                     std::memory_order_relaxed)) {
                g.excCount[i].fetch_add(1, std::memory_order_relaxed);
                return EXCEPTION_CONTINUE_SEARCH;
            }
            if (g.excCode[i].load(std::memory_order_relaxed) == code) {
                g.excCount[i].fetch_add(1, std::memory_order_relaxed);
                return EXCEPTION_CONTINUE_SEARCH;
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- report

const char* ExportName(int idx) {
    if (idx < 0 || uint32_t(idx) >= g.exportCount) {
        return "?";
    }
    return g.names + g.exports[idx].nameOff;
}

const wchar_t* ModuleName(int idx) {
    if (idx < 0 || uint32_t(idx) >= g.moduleCount) {
        return L"(unknown)";
    }
    return g.modules[idx].name;
}

// Runs on the sampler's own thread, with nothing suspended.
void Report(uint32_t windowMs) {
    RefreshModules();
    if (g.binTotal == 0 && g.ntTotal == 0) {
        return;
    }
    char line[640];  // facts-ok: SPECTATOR.CI_STRIDE the size of this snprintf target, not the CR15EchoUnitCS element stride
    int n = snprintf(line, sizeof line, "mainprof: %u in echovr.exe, %u in ntdll; by module:",
                     g.binTotal, g.ntTotal);
    for (uint32_t i = 0; i < g.moduleCount && n > 0 && n < int(sizeof line) - 48; ++i) {
        if (g.modules[i].hits == 0) {
            continue;
        }
        n += snprintf(line + n, sizeof line - size_t(n), " %ls=%u", g.modules[i].name,
                      g.modules[i].hits);
    }
    log::Direct(LogLevel::Info, "mainprof", "%s (suspend failures %u)", line, g.suspendFailures);

    const uint32_t exc = g.excTotal.exchange(0, std::memory_order_relaxed);
    char excLine[256];
    int en = snprintf(excLine, sizeof excLine, "mainprof: first-chance exceptions %u in %u ms:",
                      exc, windowMs);
    for (uint32_t i = 0; i < kMaxExcCodes && en > 0 && en < int(sizeof excLine) - 32; ++i) {
        const uint32_t code = g.excCode[i].load(std::memory_order_relaxed);
        if (code == 0) {
            break;
        }
        const uint32_t hits = g.excCount[i].exchange(0, std::memory_order_relaxed);
        if (hits != 0) {
            en += snprintf(excLine + en, sizeof excLine - size_t(en), " 0x%08X=%u", code, hits);
        }
    }
    const uint64_t probes = ReadableProbeCount();
    log::Direct(LogLevel::Info, "mainprof", "%s; plugin Readable() probes %llu in the window",
                excLine, static_cast<unsigned long long>(probes - g.lastProbeCount));
    g.lastProbeCount = probes;

    // The ntdll functions the frame thread was inside, by export, with the module that called in.
    for (uint32_t round = 0; round < 15; ++round) {
        uint32_t best = 0;
        int bestAt = -1;
        for (uint32_t i = 0; i < g.exportCount; ++i) {
            if (g.exportHits[i] > best) { best = g.exportHits[i]; bestAt = int(i); }
        }
        if (bestAt < 0) {
            break;
        }
        g.exportHits[bestAt] = 0;
        char callers[448];  // facts-ok: GAME.VTBL_RENDER_OFF the size of this snprintf target, not a vtable slot offset
        int cn = snprintf(callers, sizeof callers, "callers:");
        for (uint32_t round2 = 0; round2 < 5; ++round2) {
            uint32_t bestPair = 0, bestPairAt = 0;
            bool found = false;
            for (uint32_t i = 0; i < g.pairCount; ++i) {
                if (g.pairs[i].exportIdx == uint16_t(bestAt) && g.pairs[i].hits > bestPair) {
                    bestPair = g.pairs[i].hits;
                    bestPairAt = i;
                    found = true;
                }
            }
            if (!found || cn <= 0 || cn >= int(sizeof callers) - 48) {
                break;
            }
            Pair& p = g.pairs[bestPairAt];
            cn += snprintf(callers + cn, sizeof callers - size_t(cn), " %ls+0x%X(%u)",
                           ModuleName(int(p.moduleIdx)), p.callerBin << kCallerShift, p.hits);
            p.hits = 0;
        }
        log::Direct(LogLevel::Info, "mainprof", "  ntdll %-44s %5u  %5.1f%%  %s",
                    ExportName(bestAt), best, 100.0 * double(best) / double(g.ntTotal ? g.ntTotal : 1),
                    callers);
    }
    for (uint32_t round = 0; round < 14; ++round) {
        uint32_t best = 0, bestAt = 0;
        for (uint32_t i = 0; i < g.chainCount; ++i) {
            if (g.chains[i].hits > best) { best = g.chains[i].hits; bestAt = i; }
        }
        if (best == 0) {
            break;
        }
        Chain& c = g.chains[bestAt];
        c.hits = 0;
        char where[320];  // facts-ok: PHYS.CI_GATE_OFF the size of this snprintf target, not the physics gate offset
        int wn = 0;
        for (uint32_t k = 0; k < kChainDepth && wn >= 0 && wn < int(sizeof where) - 40; ++k) {
            const int m = ModuleIndexOf(c.at[k]);
            if (m < 0) {
                break;
            }
            wn += snprintf(where + wn, sizeof where - size_t(wn), " %ls+0x%llX", ModuleName(m),
                           static_cast<unsigned long long>(c.at[k] - g.modules[m].base));
        }
        log::Direct(LogLevel::Info, "mainprof", "  chain %5u x %-28s%s", best,
                    ExportName(int(c.exportIdx)), where);
    }
    g.chainCount = 0;
    if (g.noCaller != 0) {
        log::Direct(LogLevel::Info, "mainprof", "  (no caller frame found in %u sample(s))",
                    g.noCaller);
    }

    // The hottest 4 KB windows of the image, as RVAs the offline database resolves to functions.
    for (uint32_t round = 0; round < 10; ++round) {
        uint32_t best = 0, bestAt = 0;
        for (uint32_t i = 0; i < kMaxBins; ++i) {
            if (g.bins[i] > best) { best = g.bins[i]; bestAt = i; }
        }
        if (best == 0) {
            break;
        }
        g.bins[bestAt] = 0;
        log::Direct(LogLevel::Info, "mainprof", "  rva 0x%06X..0x%06X  %u sample(s)  %.1f%%",
                    bestAt << kBinShift, ((bestAt + 1) << kBinShift) - 1, best,
                    100.0 * double(best) / double(g.binTotal ? g.binTotal : 1));
    }
    for (uint32_t i = 0; i < kMaxBins; ++i) g.bins[i] = 0;
    for (uint32_t i = 0; i < g.exportCount; ++i) g.exportHits[i] = 0;
    g.pairCount = 0;
    g.pairOverflow = 0;
    g.noCaller = 0;
    g.binTotal = 0;
    g.ntTotal = 0;
    for (uint32_t i = 0; i < g.moduleCount; ++i) g.modules[i].hits = 0;
}

void LoadStackBounds(HANDLE thread) {
    struct BasicInfo {
        LONG ExitStatus;
        PVOID TebBaseAddress;
        ULONG_PTR UniqueProcessId;
        ULONG_PTR UniqueThreadId;
        ULONG_PTR AffinityMask;
        LONG Priority;
        LONG BasePriority;
    };
    using QueryFn = LONG(WINAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    QueryFn query = reinterpret_cast<QueryFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    if (query == nullptr) {
        return;
    }
    BasicInfo info = {};
    if (query(thread, 0 /* ThreadBasicInformation */, &info, sizeof info, nullptr) < 0 ||
        info.TebBaseAddress == nullptr) {
        return;
    }
    const uintptr_t teb = uintptr_t(info.TebBaseAddress);
    g.stackBase = *reinterpret_cast<uintptr_t*>(teb + 0x08);
    g.stackLimit = *reinterpret_cast<uintptr_t*>(teb + 0x10);
}

DWORD WINAPI SamplerMain(LPVOID) {
    uint32_t tid = 0;
    for (uint32_t waited = 0; waited < 600000 / kSampleIntervalMs; ++waited) {
        tid = g.frameThread.load(std::memory_order_acquire);
        if (tid != 0 || g.running.load(std::memory_order_relaxed) == 0) break;
        Sleep(20);
    }
    if (tid == 0) {
        log::Direct(LogLevel::Warn, "mainprof", "no frame thread ever reported; not sampling");
        return 0;
    }
    const HANDLE th = OpenThread(
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (th == nullptr) {
        log::Direct(LogLevel::Warn, "mainprof", "OpenThread(%u) failed (%lu); not sampling", tid,
                    GetLastError());
        return 0;
    }
    RefreshModules();
    LoadExports(g.ntdllBase);
    LoadStackBounds(th);
    log::Direct(LogLevel::Info, "mainprof",
                "sampling the frame thread (tid %u) every %u ms; image base %p; ntdll %u export(s); "
                "stack %p..%p", tid, kSampleIntervalMs, reinterpret_cast<void*>(g.imageBase),
                g.exportCount, reinterpret_cast<void*>(g.stackLimit),
                reinterpret_cast<void*>(g.stackBase));

    uint32_t sinceReport = 0;
    while (g.running.load(std::memory_order_relaxed) != 0) {
        Sleep(kSampleIntervalMs);
        sinceReport += kSampleIntervalMs;

        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_CONTROL;
        uintptr_t rip = 0;
        uint32_t words = 0;
        if (SuspendThread(th) != DWORD(-1)) {
            if (GetThreadContext(th, &ctx)) {
                rip = uintptr_t(ctx.Rip);
                const uintptr_t rsp = uintptr_t(ctx.Rsp);
                // Copying under suspension is the only safe way to read the frame thread's stack,
                // and it is clamped to the TEB's own bounds so it can never touch a guard page.
                if (g.stackBase != 0 && rsp >= g.stackLimit && rsp < g.stackBase) {
                    uintptr_t end = rsp + kStackWords * 8;
                    if (end > g.stackBase) end = g.stackBase;
                    words = uint32_t((end - rsp) / 8);
                    for (uint32_t i = 0; i < words; ++i) {
                        g.stackCopy[i] = *reinterpret_cast<const uint64_t*>(rsp + i * 8);
                    }
                }
            }
            ResumeThread(th);
        } else if (g.suspendFailures < 0xFFFFFFFFu) {  // facts-ok: GOAL.USERID_UNSET a u32 saturation cap on a failure counter, not the unset-user sentinel
            ++g.suspendFailures;
        }
        if (rip == 0) {
            continue;
        }

        // Classification happens with the thread running again: it touches the module table and
        // the loader, and the one rule this profiler cannot break is holding a lock the suspended
        // thread owns.
        const int mod = ModuleIndexOf(rip);
        if (mod < 0) {
            continue;
        }
        ++g.modules[mod].hits;
        const uintptr_t base = g.modules[mod].base;
        if (base == g.imageBase) {
            const uint32_t bin = uint32_t((rip - base) >> kBinShift);
            if (bin < kMaxBins) {
                ++g.bins[bin];
                ++g.binTotal;
            }
        } else if (base == g.ntdllBase && g.ntdllBase != 0) {
            ++g.ntTotal;
            const int idx = ExportIndexFor(uint32_t(rip - base));
            if (idx >= 0) {
                ++g.exportHits[idx];
                int callerMod = -1;
                uintptr_t caller = 0;
                uintptr_t chain[kChainDepth] = {};
                uint32_t found = 0;
                for (uint32_t i = 0; i < words && found < kChainDepth; ++i) {
                    const uintptr_t v = uintptr_t(g.stackCopy[i]);
                    const int m = ModuleIndexOf(v);
                    // Below the first section there is no code, and the bytes in front of a
                    // candidate that close to the base are outside the mapping.
                    if (m < 0 || g.modules[m].system || v - g.modules[m].base < 0x1000) {
                        continue;
                    }
                    if (!LooksLikeReturnAddress(v)) {
                        continue;
                    }
                    if (found == 0) {
                        callerMod = m;
                        caller = v;
                    }
                    chain[found++] = v;
                }
                if (callerMod >= 0) {
                    RecordPair(idx, callerMod,
                               uint32_t((caller - g.modules[callerMod].base) >> kCallerShift));
                    RecordChain(idx, chain);
                } else {
                    ++g.noCaller;
                }
            }
        }

        if (sinceReport >= kReportEveryMs) {
            Report(sinceReport);
            sinceReport = 0;
        }
    }
    CloseHandle(th);
    return 0;
}

}  // namespace

void NoteFrameThread() {
    if (g.frameThread.load(std::memory_order_relaxed) == 0) {
        g.frameThread.store(GetCurrentThreadId(), std::memory_order_release);
    }
}

bool Start() {
    if (!config::Interface()->GetBool("bootstrap.profiler", false)) {
        return false;
    }
    g.imageBase = uintptr_t(GetModuleHandleW(nullptr));
    g.ntdllBase = uintptr_t(GetModuleHandleW(L"ntdll.dll"));
    g.running.store(1, std::memory_order_relaxed);
    g.vehHandle = AddVectoredExceptionHandler(0, &CountException);
    g.thread = CreateThread(nullptr, 0, &SamplerMain, nullptr, 0, nullptr);
    if (g.thread == nullptr) {
        g.running.store(0, std::memory_order_relaxed);
        if (g.vehHandle != nullptr) {
            RemoveVectoredExceptionHandler(g.vehHandle);
            g.vehHandle = nullptr;
        }
        return false;
    }
    log::Direct(LogLevel::Warn, "mainprof",
                "bootstrap.profiler is ON: the engine's frame thread is being suspended %u times a "
                "second to read its instruction pointer. Diagnostic only",
                1000 / kSampleIntervalMs);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF milliseconds per second, turning the sample interval into a rate
    return true;
}

void Stop() {
    g.running.store(0, std::memory_order_relaxed);
    if (g.thread != nullptr) {
        WaitForSingleObject(g.thread, 2000);
        CloseHandle(g.thread);
        g.thread = nullptr;
    }
    if (g.vehHandle != nullptr) {
        RemoveVectoredExceptionHandler(g.vehHandle);
        g.vehHandle = nullptr;
    }
}

}  // namespace mainprof
}  // namespace replay
