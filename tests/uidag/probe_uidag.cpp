// W1-M's in-game probe: the five reads that decide whether the component-system update DAG can carry
// our UI callback, and where. Queued through runs\queue\W1-M.probe.json; this packet never launches
// the game.
//
// ★ NO BOOTSTRAP, NO REPLAY_MODULE, NO MINHOOK, NO EXPORTS, AND NOTHING IS DETOURED.
//
// Every number below is a READ. The probe installs no hook, patches no byte, calls no engine function
// and enters no session. That is not modesty: the questions are all "what does the engine already do",
// and an instrument that changes the schedule cannot answer them.
//
// ★ WHY THE VERDICT IS FILED FROM A STATIC INITIALISER. `coexist::ClaimInstance` refuses every module
// after the first in a multi-probe launch, and a probe that files from a module Init then writes
// nothing at all -- five probes filed nothing on one sweep for exactly that. This DLL has no bootstrap
// to be refused, but the same discipline is kept anyway, because the failure it prevents is silent:
// the PROVISIONAL record is written from a static initialiser, before DllMain, before any other
// module's claim, and before the sampler exists. Killed mid-measurement, the file still says "I loaded
// and got this far", which the harness reads as LOADED-then-killed rather than never-loaded.
//
// ★ EVERY ENGINE READ GOES THROUGH ReadProcessMemory ON OUR OWN PROCESS. The addresses come from the
// engine's own instructions, but a game space can be freed between the sample that found it and the
// sample that reads it, and a TLS array is only as long as its thread needed. RPM is kernel-validated:
// a stale or short pointer returns false instead of taking the process down. A probe that can crash
// the host is worse than no probe, because the launch that carried it is spent either way.
//
// ★ THE THREE-VALUED OUTCOME. Each check is Ok, Failed, or UNDETERMINED, and `checked` counts only the
// first two. A pass that never ran, a game space that never existed and a thread list with no engine
// context are all undetermined, not passes and not failures: reporting an undetermined check as ok is
// a false all-clear and as failed is a false accusation. A probe that determines nothing therefore
// reports checked=0, which the harness already treats as "no verdict produced" -- the honest answer.
//
// WHAT IT ESTABLISHES
//   R1  the task-dispatch flag: for every thread carrying an engine context, *(u32*)(ctx + 0x730).
//       Non-zero anywhere means CS callbacks can be dispatched to CTaskKernel workers and ImGui from a
//       registered callback is unsafe. C11 exists so a clean R1 cannot come from an empty thread list.
//   R2  bit 23 of *(u32*)(game + 0x828). Set means the WHOLE stage-0..3 component-system pass is
//       skipped, which would invert docs/spikes/W1-M.md S3.2.3's "register at band 3" recommendation.
//   R4  the pass window at space+0xC3A/+0xC3C, sampled: which of the seven band arms are ever driven,
//       and whether any value outside the seven appears (which would mean the static reading is wrong).
//   R5  the scheme record count at scheme+0x610, first and last. Growth is late registration, observed.
//
// WHAT IT DOES NOT ESTABLISH
//   R3, the ordering of NRadGui::EndFrame against the stage-6 pass, is NOT here. It was settled
//   statically instead -- EndFrame's only reachable call site is the vtable slot +0x88 call at
//   0x140509518, between the stage-5 and stage-6 driver calls -- and confirming it in vivo would need
//   detours on two game functions. This probe declines to install them. See S3.2.3.
//
//   Nothing here says which thread runs the frame loop. R1 is answered over ALL threads precisely so
//   that it does not have to.

#include <windows.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "generated/facts_generated.h"

namespace {

// ---------------------------------------------------------------------------------------------------
// Sites in echovr.exe, each carried with the bytes that stand at it. The probe does not trust these
// addresses: it compares the mapped bytes against the window first, and every constant it then uses is
// DECODED OUT OF THOSE BYTES rather than written down a second time. A recompiled game fails the byte
// comparison and the dependent checks go undetermined; it cannot silently read a wrong offset.
// ---------------------------------------------------------------------------------------------------

struct Site {
    const char*   what;
    uint64_t      va;
    const uint8_t bytes[32];
    size_t        len;
};

// 0x140517967, in the per-stage fan-out sub_140517920:
//   mov eax,[rbx+828h] / shr rax,17h / test al,1 / jnz  -- the stage-0..3 pass gate
constexpr Site kGateSite = {
    "stage-0..3 pass gate",
    0x140517967ull,
    {0x8B, 0x83, 0x28, 0x08, 0x00, 0x00, 0x48, 0xC1, 0xE8, 0x17, 0xA8, 0x01, 0x75, 0x0F, 0x48, 0x8B,
     0x03, 0x48, 0x8B, 0xD7},
    20};

// 0x140505282, in the runner sub_140505260:
//   mov ecx,[rip+TlsIndex] / call TlsGetValue / mov r8,[rax] / mov r13d,[r8+730h]
//   / movzx eax,word [rbp+0C38h] / imul r15,rax,78h
// One window, five constants: the TLS index global, the DOUBLE indirection (the TLS value is a pointer
// TO the context, not the context), the dispatch-flag offset, the scheme-index offset, and the stride.
constexpr Site kTlsSite = {
    "runner TLS block",
    0x140505282ull,
    {0x8B, 0x0D, 0x58, 0x3C, 0xB9, 0x01, 0xE8, 0x03, 0x8E, 0xCC, 0xFF, 0x4C, 0x8B, 0x00, 0x45, 0x8B,
     0xA8, 0x30, 0x07, 0x00, 0x00, 0x0F, 0xB7, 0x85, 0x38, 0x0C, 0x00, 0x00, 0x4C, 0x6B, 0xF8, 0x78},
    32};

// 0x140518F60 and 0x140518ECE, in the stage driver sub_140518E60: the game-space list and its count.
constexpr Site kListSite = {
    "game-space list base",
    0x140518F60ull,
    {0x48, 0x8B, 0x87, 0x50, 0x1F, 0x00, 0x00, 0x48, 0x8B, 0x44, 0x08, 0x08, 0x4B, 0x89, 0x04, 0xEC,
     0x49, 0xFF, 0xC5, 0x4C},
    20};
constexpr Site kCountSite = {
    "game-space list count",
    0x140518ECEull,
    {0x48, 0x8B, 0x9F, 0x80, 0x1F, 0x00, 0x00, 0x48, 0x89, 0x5C, 0x24, 0x50, 0x45, 0x33, 0xFF, 0x48,
     0x85, 0xDB, 0x75, 0x0F},
    20};

constexpr uint32_t kBandCount = 7;
constexpr uint32_t kBandSpan = 0x2000u;
constexpr uint32_t kBandTop = 0x1FFEu;

// How long to wait for a frame loop, and how long to watch it once it is turning. The wait is bounded
// by the harness deadline anyway; the sample window is short because the pass window changes several
// times per frame, so three seconds is hundreds of frames and the launch is not held open for it.
constexpr DWORD kWaitForSpacesMs = 90000;
constexpr DWORD kSampleMs = 3000;
constexpr DWORD kPollSleepMs = 1;

constexpr size_t kMaxSpaces = 16;
constexpr size_t kMaxWindowValues = 24;
constexpr size_t kMaxThreads = 96;

// ---------------------------------------------------------------------------------------------------

enum class Outcome { Undetermined, Ok, Failed };

struct Check {
    const char* what;
    Outcome     outcome;
    char        note[160];
};

constexpr size_t kMaxChecks = 20;

struct WindowValue {
    uint32_t value;
    uint64_t seen;
};

struct ThreadReading {
    DWORD    tid;
    uint64_t ctx;
    uint32_t flag;
    bool     flagReadable;
};

struct State {
    Check  checks[kMaxChecks];
    size_t checkCount;

    const uint8_t* base;
    uint64_t       imageSize;

    // decoded from the guarded windows
    uint32_t gateOff;
    uint32_t gateBit;
    uint64_t tlsIndexVa;
    uint32_t ctxFlagOff;
    uint32_t schemeIdxOff;
    uint32_t schemeStride;
    uint32_t listOff;
    uint32_t countOff;

    WindowValue windows[kMaxWindowValues];
    size_t      windowCount;
    uint64_t    windowSamples;
    bool        bandDriven[kBandCount];
    uint64_t    outOfBandValues;
    uint32_t    firstOutOfBand;

    uint64_t gateSamples;
    uint64_t gateSetSamples;

    uint64_t firstRecordCount;
    uint64_t lastRecordCount;
    bool     recordCountSeen;
    uint16_t schemeIdxSeen;

    ThreadReading threads[kMaxThreads];
    size_t        threadCount;
    size_t        threadsWithContext;
    size_t        threadsWithNonZeroFlag;
    bool          threadScanRan;

    uint64_t spaceSamples;
    size_t   spacesSeen;
    uint64_t gameSeenAtMs;

    HMODULE self;
    char    stem[64];
};

State g;

// ---------------------------------------------------------------------------------------------------
// Reads. Every one of them is bounds-checked or kernel-checked, and none of them can raise.
// ---------------------------------------------------------------------------------------------------

bool ReadSelf(uint64_t address, void* out, size_t bytes) {
    if (address == 0 || bytes == 0) {
        return false;
    }
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, bytes,
                             &got) != 0 &&
           got == bytes;
}

// The registry records addresses against the base the binary declares; GAME_SINGLETON_VA and its RVA
// are the one pair that names that base, so the rebase is derived rather than assumed.
const uint8_t* MapVa(uint64_t recordedVa, size_t bytes) {
    if (g.base == nullptr) {
        return nullptr;
    }
    const uint64_t recordedBase = facts::GAME_SINGLETON_VA - facts::GAME_SINGLETON_RVA;
    if (recordedVa < recordedBase) {
        return nullptr;
    }
    const uint64_t rva = recordedVa - recordedBase;
    if (rva + bytes > g.imageSize) {
        return nullptr;
    }
    return g.base + rva;
}

uint32_t U32At(const uint8_t* p, size_t off) {
    uint32_t v = 0;
    memcpy(&v, p + off, sizeof v);
    return v;
}

int32_t I32At(const uint8_t* p, size_t off) {
    int32_t v = 0;
    memcpy(&v, p + off, sizeof v);
    return v;
}

// ---------------------------------------------------------------------------------------------------

Check* AddCheck(const char* what) {
    if (g.checkCount >= kMaxChecks) {
        return nullptr;
    }
    Check* c = &g.checks[g.checkCount++];
    c->what = what;
    c->outcome = Outcome::Undetermined;
    c->note[0] = '\0';
    return c;
}

void Settle(Check* c, bool ok, const char* fmt, ...) {
    if (c == nullptr) {
        return;
    }
    c->outcome = ok ? Outcome::Ok : Outcome::Failed;
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(c->note, sizeof c->note, _TRUNCATE, fmt, args);
    va_end(args);
}

void LeaveUndetermined(Check* c, const char* fmt, ...) {
    if (c == nullptr) {
        return;
    }
    c->outcome = Outcome::Undetermined;
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(c->note, sizeof c->note, _TRUNCATE, fmt, args);
    va_end(args);
}

bool SiteMatches(const Site& s) {
    const uint8_t* mapped = MapVa(s.va, s.len);
    return mapped != nullptr && memcmp(mapped, s.bytes, s.len) == 0;
}

// ---------------------------------------------------------------------------------------------------
// R1. The dispatch flag, read for EVERY thread rather than for a thread we would have had to identify.
//
// The runner does `*(u32*)(*(void**)TlsGetValue(idx) + 0x730)` -- note the DOUBLE indirection, which is
// decoded from kTlsSite's `mov r8,[rax]` and is the easy thing to get wrong. TlsGetValue reads the
// CALLING thread's slot, so it cannot answer for another thread; the TEB's ThreadLocalStoragePointer
// is the same array the API indexes, reached through NtQueryInformationThread.
// ---------------------------------------------------------------------------------------------------

typedef LONG(NTAPI* NtQueryInformationThreadFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);

struct ThreadBasicInformationBlock {
    LONG      ExitStatus;
    PVOID     TebBaseAddress;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR UniqueThreadId;
    ULONG_PTR AffinityMask;
    LONG      Priority;
    LONG      BasePriority;
};

constexpr ULONG kThreadBasicInformation = 0;
constexpr size_t kTebTlsPointerOffset = 0x58;  // x64 TEB::ThreadLocalStoragePointer

void ScanThreads(uint32_t tlsIndex) {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return;
    }
    const NtQueryInformationThreadFn query = reinterpret_cast<NtQueryInformationThreadFn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryInformationThread")));
    if (query == nullptr) {
        return;
    }
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    const DWORD me = GetCurrentProcessId();
    THREADENTRY32 entry;
    entry.dwSize = sizeof entry;
    if (Thread32First(snap, &entry)) {
        do {
            if (entry.th32OwnerProcessID != me || g.threadCount >= kMaxThreads) {
                continue;
            }
            const HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
            if (th == nullptr) {
                continue;
            }
            ThreadBasicInformationBlock tbi = {};
            ULONG returned = 0;
            uint64_t ctx = 0;
            uint32_t flag = 0;
            bool flagReadable = false;
            if (query(th, kThreadBasicInformation, &tbi, sizeof tbi, &returned) >= 0 &&
                tbi.TebBaseAddress != nullptr) {
                uint64_t tlsArray = 0;
                if (ReadSelf(reinterpret_cast<uint64_t>(tbi.TebBaseAddress) + kTebTlsPointerOffset,
                             &tlsArray, sizeof tlsArray) &&
                    tlsArray != 0) {
                    uint64_t slot = 0;
                    // The array is only as long as this thread has needed; a short one fails the read
                    // rather than handing back a neighbouring allocation.
                    if (ReadSelf(tlsArray + 8ull * tlsIndex, &slot, sizeof slot) && slot != 0) {
                        if (ReadSelf(slot, &ctx, sizeof ctx) && ctx != 0) {
                            flagReadable = ReadSelf(ctx + g.ctxFlagOff, &flag, sizeof flag);
                        }
                    }
                }
            }
            CloseHandle(th);

            // Record every thread once, and refresh a thread already seen: a context can appear later.
            size_t at = kMaxThreads;
            for (size_t i = 0; i < g.threadCount; ++i) {
                if (g.threads[i].tid == entry.th32ThreadID) {
                    at = i;
                    break;
                }
            }
            if (at == kMaxThreads) {
                at = g.threadCount++;
                g.threads[at].tid = entry.th32ThreadID;
                g.threads[at].ctx = 0;
                g.threads[at].flag = 0;
                g.threads[at].flagReadable = false;
            }
            if (ctx != 0) {
                g.threads[at].ctx = ctx;
            }
            if (flagReadable) {
                // Sticky in the unsafe direction: a flag seen non-zero once is a thread that CAN take
                // the task path, and a later zero does not undo that.
                if (!g.threads[at].flagReadable || flag != 0) {
                    g.threads[at].flag = flag;
                }
                g.threads[at].flagReadable = true;
            }
        } while (Thread32Next(snap, &entry));
    }
    CloseHandle(snap);
    g.threadScanRan = true;

    g.threadsWithContext = 0;
    g.threadsWithNonZeroFlag = 0;
    for (size_t i = 0; i < g.threadCount; ++i) {
        if (g.threads[i].ctx != 0 && g.threads[i].flagReadable) {
            ++g.threadsWithContext;
            if (g.threads[i].flag != 0) {
                ++g.threadsWithNonZeroFlag;
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------

void RecordWindow(uint32_t value) {
    ++g.windowSamples;
    for (size_t i = 0; i < g.windowCount; ++i) {
        if (g.windows[i].value == value) {
            ++g.windows[i].seen;
            return;
        }
    }
    if (g.windowCount < kMaxWindowValues) {
        g.windows[g.windowCount].value = value;
        g.windows[g.windowCount].seen = 1;
        ++g.windowCount;
    }
    const uint32_t lo = value & 0xFFFFu;
    const uint32_t hi = value >> 16;
    for (uint32_t b = 0; b < kBandCount; ++b) {
        if (lo == b * kBandSpan && hi == b * kBandSpan + kBandTop) {
            g.bandDriven[b] = true;
            return;
        }
    }
    ++g.outOfBandValues;
    if (g.outOfBandValues == 1) {
        g.firstOutOfBand = value;
    }
}

uint64_t GameFromSingleton() {
    uint64_t game = 0;
    const uint8_t* cell = MapVa(facts::GAME_SINGLETON_VA, sizeof game);
    if (cell == nullptr) {
        return 0;
    }
    if (!ReadSelf(reinterpret_cast<uint64_t>(cell), &game, sizeof game)) {
        return 0;
    }
    return game;
}

size_t CollectSpaces(uint64_t game, uint64_t* out, size_t max) {
    if (game == 0) {
        return 0;
    }
    uint64_t list = 0;
    uint64_t count = 0;
    if (!ReadSelf(game + g.listOff, &list, sizeof list) ||
        !ReadSelf(game + g.countOff, &count, sizeof count)) {
        return 0;
    }
    if (list == 0 || count == 0 || count > 4096) {
        return 0;
    }
    size_t taken = 0;
    for (uint64_t i = 0; i < count && taken < max; ++i) {
        uint64_t space = 0;
        if (ReadSelf(list + 16ull * i + 8ull, &space, sizeof space) && space != 0) {
            out[taken++] = space;
        }
    }
    return taken;
}

// ---------------------------------------------------------------------------------------------------
// The verdict file.
// ---------------------------------------------------------------------------------------------------

void EscapeInto(char* dst, size_t dstBytes, const char* src) {
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0' && o + 2 < dstBytes; ++i) {
        const char ch = src[i];
        if (ch == '\\' || ch == '"') {
            dst[o++] = '\\';
            dst[o++] = ch;
        } else if (ch >= 0x20) {
            dst[o++] = ch;
        }
    }
    dst[o] = '\0';
}

bool ProbeRoot(char* out, size_t bytes) {
    const DWORD n = GetEnvironmentVariableA("REPLAY_PROBE_ROOT", out, static_cast<DWORD>(bytes));
    return n != 0 && n < bytes;
}

void ProbeId(char* out, size_t bytes) {
    // ★ The most restrictive fallback. An id we did not recognise must not silently become a mode with
    // a longer window or a wider scope, so the default names itself as the unresolved case.
    strncpy_s(out, bytes, "W1-M-uidag-unresolved-id", _TRUNCATE);
    char ids[512] = {0};
    if (GetEnvironmentVariableA("REPLAY_PROBE_IDS", ids, sizeof ids) == 0) {
        return;
    }
    char key[80];
    _snprintf_s(key, sizeof key, _TRUNCATE, "%s=", g.stem);
    const char* found = strstr(ids, key);
    if (found == nullptr) {
        return;
    }
    const char* value = found + strlen(key);
    size_t n = strcspn(value, ";");
    if (n == 0) {
        return;
    }
    if (n >= bytes) {
        n = bytes - 1;
    }
    memcpy(out, value, n);
    out[n] = '\0';
}

void WriteVerdictFile(bool provisional, const char* verdict, size_t checked, size_t undetermined,
                      const char* detail) {
    char root[MAX_PATH];
    if (!ProbeRoot(root, sizeof root)) {
        return;  // not in a sweep; never scatter files into the owner's install
    }
    char dir[MAX_PATH];
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", root, g.stem);
    CreateDirectoryA(dir, nullptr);

    char id[128];
    ProbeId(id, sizeof id);

    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(sizeof cwd, cwd);
    char cwdEscaped[MAX_PATH * 2];
    EscapeInto(cwdEscaped, sizeof cwdEscaped, cwd);

    char detailEscaped[4096];
    EscapeInto(detailEscaped, sizeof detailEscaped, detail);

    char path[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\done.json", dir);
    FILE* fh = nullptr;
    if (fopen_s(&fh, path, "wb") != 0 || fh == nullptr) {
        return;
    }
    fprintf(fh,
            "{ \"probe\": \"%s\", \"verdict\": \"%s\", \"checked\": %llu,\n"
            "  \"undetermined\": %llu, \"provisional\": %s, \"cwd\": \"%s\",\n"
            "  \"detail\": \"%s\" }\n",
            id, verdict, (unsigned long long)checked, (unsigned long long)undetermined,
            provisional ? "true" : "false", cwdEscaped, detailEscaped);
    fclose(fh);
}

void FileProvisional() {
    WriteVerdictFile(true, "NOT-RUN", 0, 0,
                     "provisional record written from a static initialiser, before DllMain and before "
                     "any other module's instance claim. The sampler has not run yet. If this record "
                     "is still here at the deadline the probe LOADED and was killed mid-measurement, "
                     "which is a different fact from never having loaded.");
}

// ---------------------------------------------------------------------------------------------------

DWORD WINAPI SamplerMain(LPVOID) {
    // A short settle before the first read. The thread is created from DllMain, so it starts while the
    // loader is still mapping the rest of the plugins folder; a sampler that begins in that window can
    // read a half-built world and would report it as measurement. It also makes the provisional record
    // observable from outside, which is how tests/uidag/check.ps1 proves it was filed first.
    Sleep(50);

    Check* cGate = AddCheck("the stage-0..3 gate site holds the recorded bytes");
    Check* cTls = AddCheck("the runner TLS block holds the recorded bytes");
    Check* cList = AddCheck("the game-space list sites hold the recorded bytes");
    Check* cDerived = AddCheck("constants decoded from those bytes agree with the fact registry");
    Check* cArms = AddCheck("the seven band arms this probe watches for match the recorded arms");
    Check* cGame = AddCheck("the CR15Game singleton became non-null");
    Check* cSpaces = AddCheck("at least one game space was observed");
    Check* cR2 = AddCheck("R2: bit 23 of game+0x828 is CLEAR, so the stage-0..3 pass is not skipped");
    Check* cR4Known = AddCheck("R4: every observed pass window is one of the seven recorded arms");
    Check* cR4Band3 = AddCheck("R4: band 3 (0x6000..0x7FFE) was observed to be driven");
    Check* cR4Band6 = AddCheck("R4: band 6 (0xC000..0xDFFE) was observed to be driven");
    Check* cR1 = AddCheck("R1: every thread with an engine context reports ctx+0x730 == 0");
    Check* cR1Live = AddCheck("R1 is not vacuous: at least one thread carried an engine context");
    Check* cR5 = AddCheck("R5: the scheme record count was non-zero");
    Check* cR5Grow = AddCheck("R5: the scheme record count never shrank");

    g.base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (g.base != nullptr) {
        const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g.base);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            const IMAGE_NT_HEADERS64* nt =
                reinterpret_cast<const IMAGE_NT_HEADERS64*>(g.base + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                g.imageSize = nt->OptionalHeader.SizeOfImage;
            }
        }
    }

    const bool gateOk = SiteMatches(kGateSite);
    const bool tlsOk = SiteMatches(kTlsSite);
    const bool listOk = SiteMatches(kListSite) && SiteMatches(kCountSite);
    Settle(cGate, gateOk, "%s at 0x%llX", gateOk ? "matched" : "MISMATCH", kGateSite.va);
    Settle(cTls, tlsOk, "%s at 0x%llX", tlsOk ? "matched" : "MISMATCH", kTlsSite.va);
    Settle(cList, listOk, "%s at 0x%llX and 0x%llX", listOk ? "matched" : "MISMATCH", kListSite.va,
           kCountSite.va);

    if (!gateOk || !tlsOk || !listOk) {
        LeaveUndetermined(cDerived, "a guarded site did not match, so nothing was decoded from it");
        LeaveUndetermined(cGame, "no offsets to read the singleton's spaces with");
        LeaveUndetermined(cSpaces, "no offsets");
        LeaveUndetermined(cR2, "the gate offset could not be decoded from the image");
        LeaveUndetermined(cR4Known, "no offsets");
        LeaveUndetermined(cR4Band3, "no offsets");
        LeaveUndetermined(cR4Band6, "no offsets");
        LeaveUndetermined(cR1, "the TLS index could not be decoded from the image");
        LeaveUndetermined(cR1Live, "the TLS index could not be decoded from the image");
        LeaveUndetermined(cR5, "no offsets");
        LeaveUndetermined(cR5Grow, "no offsets");
    }

    // The seven arms this probe watches for, built the way the driver builds them. Asserting the two
    // ends against the registry is a known-answer control on the probe's own arithmetic: get the
    // formula wrong and this fails here rather than silently reporting "no band was ever driven".
    const uint32_t arm0 = (kBandTop << 16) | 0u;
    const uint32_t arm6 = (((6u * kBandSpan) + kBandTop) << 16) | (6u * kBandSpan);
    const bool armsOk =
        arm0 == facts::CS_PASS_WINDOW_BAND0 && arm6 == facts::CS_PASS_WINDOW_BAND6;
    Settle(cArms, armsOk, "arm0=0x%08X (registry 0x%08X) arm6=0x%08X (registry 0x%08X)", arm0,
           facts::CS_PASS_WINDOW_BAND0, arm6, facts::CS_PASS_WINDOW_BAND6);

    if (gateOk && tlsOk && listOk) {
        const uint8_t* gate = MapVa(kGateSite.va, kGateSite.len);
        const uint8_t* tls = MapVa(kTlsSite.va, kTlsSite.len);
        const uint8_t* lst = MapVa(kListSite.va, kListSite.len);
        const uint8_t* cnt = MapVa(kCountSite.va, kCountSite.len);
        g.gateOff = U32At(gate, 2);
        g.gateBit = gate[9];
        g.tlsIndexVa = kTlsSite.va + 6ull + static_cast<int64_t>(I32At(tls, 2));
        g.ctxFlagOff = U32At(tls, 17);
        g.schemeIdxOff = U32At(tls, 24);
        g.schemeStride = tls[31];
        g.listOff = U32At(lst, 3);
        g.countOff = U32At(cnt, 3);

        const bool derivedOk = g.schemeStride == facts::CS_SCHEME_STRIDE && g.gateBit == 23 &&
                               g.gateOff == 0x828u && g.ctxFlagOff == 0x730u &&
                               g.schemeIdxOff == 0xC38u && g.listOff == 0x1F50u &&
                               g.countOff == 0x1F80u;
        Settle(cDerived, derivedOk,
               "gate=+0x%X bit=%u tlsIndexVa=0x%llX ctxFlag=+0x%X schemeIdx=+0x%X stride=0x%X "
               "list=+0x%X count=+0x%X",
               g.gateOff, g.gateBit, g.tlsIndexVa, g.ctxFlagOff, g.schemeIdxOff, g.schemeStride,
               g.listOff, g.countOff);

        uint32_t tlsIndex = 0;
        const uint8_t* tlsIndexCell = MapVa(g.tlsIndexVa, sizeof tlsIndex);
        const bool tlsIndexOk =
            tlsIndexCell != nullptr &&
            ReadSelf(reinterpret_cast<uint64_t>(tlsIndexCell), &tlsIndex, sizeof tlsIndex) &&
            tlsIndex != TLS_OUT_OF_INDEXES;

        // Wait for a frame loop. Spaces exist only once the game has built one, and on a profile that
        // never gets there every downstream check must say so rather than pass on an empty sample.
        const ULONGLONG started = GetTickCount64();
        uint64_t spaces[kMaxSpaces];
        size_t spaceCount = 0;
        uint64_t game = 0;
        while (GetTickCount64() - started < kWaitForSpacesMs) {
            game = GameFromSingleton();
            if (game != 0) {
                if (g.gameSeenAtMs == 0) {
                    g.gameSeenAtMs = GetTickCount64() - started;
                }
                spaceCount = CollectSpaces(game, spaces, kMaxSpaces);
                if (spaceCount != 0) {
                    break;
                }
            }
            Sleep(10);
        }
        if (game != 0) {
            Settle(cGame, true, "0x%llX, first seen %llu ms into the wait", (unsigned long long)game,
                   (unsigned long long)g.gameSeenAtMs);
        } else {
            Settle(cGame, false, "still null after %lu ms", (unsigned long)kWaitForSpacesMs);
        }
        g.spacesSeen = spaceCount;
        Settle(cSpaces, spaceCount != 0, "%llu space(s) in the driver's list",
               (unsigned long long)spaceCount);

        if (spaceCount == 0) {
            LeaveUndetermined(cR2, "no frame loop was reached, so the gate was never sampled");
            LeaveUndetermined(cR4Known, "no game space, so no pass window exists to sample");
            LeaveUndetermined(cR4Band3, "no game space");
            LeaveUndetermined(cR4Band6, "no game space");
            LeaveUndetermined(cR5, "no game space");
            LeaveUndetermined(cR5Grow, "no game space");
        } else {
            uint16_t schemeIdx = 0;
            if (ReadSelf(spaces[0] + g.schemeIdxOff, &schemeIdx, sizeof schemeIdx)) {
                g.schemeIdxSeen = schemeIdx;
            }
            const ULONGLONG sampleUntil = GetTickCount64() + kSampleMs;
            ULONGLONG nextThreadScan = GetTickCount64();
            while (GetTickCount64() < sampleUntil) {
                const uint64_t nowGame = GameFromSingleton();
                if (nowGame != 0) {
                    uint32_t flags = 0;
                    if (ReadSelf(nowGame + g.gateOff, &flags, sizeof flags)) {
                        ++g.gateSamples;
                        if (((flags >> g.gateBit) & 1u) != 0) {
                            ++g.gateSetSamples;
                        }
                    }
                    spaceCount = CollectSpaces(nowGame, spaces, kMaxSpaces);
                }
                for (size_t i = 0; i < spaceCount; ++i) {
                    uint32_t window = 0;
                    if (ReadSelf(spaces[i] + facts::CS_PASS_WINDOW_LO_OFF, &window, sizeof window)) {
                        RecordWindow(window);
                    }
                    uint16_t idx = 0;
                    uint64_t count = 0;
                    if (ReadSelf(spaces[i] + g.schemeIdxOff, &idx, sizeof idx) &&
                        ReadSelf(spaces[i] + 1ull * g.schemeStride * idx + facts::CS_SCHEME_COUNT_OFF,
                                 &count, sizeof count) &&
                        count != 0 && count < 100000) {
                        if (!g.recordCountSeen) {
                            g.firstRecordCount = count;
                            g.recordCountSeen = true;
                        }
                        g.lastRecordCount = count;
                    }
                }
                ++g.spaceSamples;
                if (tlsIndexOk && GetTickCount64() >= nextThreadScan) {
                    ScanThreads(tlsIndex);
                    nextThreadScan = GetTickCount64() + kSampleMs / 3;
                }
                Sleep(kPollSleepMs);
            }

            Settle(cR2, g.gateSetSamples == 0,
                   "%llu of %llu samples had bit %u SET (set means the whole pass is skipped)",
                   (unsigned long long)g.gateSetSamples, (unsigned long long)g.gateSamples,
                   g.gateBit);
            if (g.gateSamples == 0) {
                LeaveUndetermined(cR2, "the gate word was never readable");
            }

            if (g.windowSamples == 0) {
                LeaveUndetermined(cR4Known, "the pass window was never readable");
                LeaveUndetermined(cR4Band3, "the pass window was never readable");
                LeaveUndetermined(cR4Band6, "the pass window was never readable");
            } else {
                Settle(cR4Known, g.outOfBandValues == 0,
                       "%llu of %llu samples were outside the seven arms (first 0x%08X)",
                       (unsigned long long)g.outOfBandValues, (unsigned long long)g.windowSamples,
                       g.firstOutOfBand);
                Settle(cR4Band3, g.bandDriven[3], "band 3 %s over %llu samples",
                       g.bandDriven[3] ? "seen" : "NEVER seen", (unsigned long long)g.windowSamples);
                Settle(cR4Band6, g.bandDriven[6], "band 6 %s over %llu samples",
                       g.bandDriven[6] ? "seen" : "NEVER seen", (unsigned long long)g.windowSamples);
            }

            if (!g.recordCountSeen) {
                LeaveUndetermined(cR5, "no scheme record count was readable");
                LeaveUndetermined(cR5Grow, "no scheme record count was readable");
            } else {
                Settle(cR5, g.firstRecordCount != 0, "%llu records at scheme %u",
                       (unsigned long long)g.firstRecordCount, g.schemeIdxSeen);
                Settle(cR5Grow, g.lastRecordCount >= g.firstRecordCount, "%llu -> %llu",
                       (unsigned long long)g.firstRecordCount, (unsigned long long)g.lastRecordCount);
            }
        }

        if (!tlsIndexOk) {
            LeaveUndetermined(cR1, "the TLS index global was not readable");
            LeaveUndetermined(cR1Live, "the TLS index global was not readable");
        } else if (!g.threadScanRan) {
            LeaveUndetermined(cR1, "the thread scan never ran");
            LeaveUndetermined(cR1Live, "the thread scan never ran");
        } else {
            Settle(cR1Live, g.threadsWithContext != 0,
                   "%llu of %llu threads carried a readable engine context",
                   (unsigned long long)g.threadsWithContext, (unsigned long long)g.threadCount);
            if (g.threadsWithContext == 0) {
                LeaveUndetermined(
                    cR1, "no thread carried an engine context, so an all-zero result would be vacuous");
            } else {
                Settle(cR1, g.threadsWithNonZeroFlag == 0,
                       "%llu of %llu context-carrying threads had a NON-ZERO dispatch flag",
                       (unsigned long long)g.threadsWithNonZeroFlag,
                       (unsigned long long)g.threadsWithContext);
            }
        }
    }

    // ------------------------------------------------------------------------------------------
    // Fold. `checked` counts only what was DETERMINED; undetermined checks are reported separately
    // and by name, because a probe that guesses in either direction is worse than one that says so.
    // ------------------------------------------------------------------------------------------
    size_t ok = 0;
    size_t failed = 0;
    size_t undetermined = 0;
    char detail[4000] = {0};
    for (size_t i = 0; i < g.checkCount; ++i) {
        const Check& c = g.checks[i];
        if (c.outcome == Outcome::Ok) {
            ++ok;
        } else if (c.outcome == Outcome::Failed) {
            ++failed;
            char one[300];
            _snprintf_s(one, sizeof one, _TRUNCATE, "%sFAILED: %s [%s]", failed > 1 ? "; " : "",
                        c.what, c.note);
            strncat_s(detail, sizeof detail, one, _TRUNCATE);
        } else {
            ++undetermined;
        }
    }
    for (size_t i = 0; i < g.checkCount; ++i) {
        const Check& c = g.checks[i];
        if (c.outcome != Outcome::Undetermined) {
            continue;
        }
        char one[300];
        _snprintf_s(one, sizeof one, _TRUNCATE, "%sCOULD-NOT-DETERMINE: %s [%s]",
                    (failed != 0 || i != 0) ? " | " : "", c.what, c.note);
        strncat_s(detail, sizeof detail, one, _TRUNCATE);
    }

    char tail[1600];
    _snprintf_s(tail, sizeof tail, _TRUNCATE,
                " | MEASURED game=0x%llX spaces=%llu scheme=%u records=%llu..%llu gateSamples=%llu "
                "gateSet=%llu windowSamples=%llu distinctWindows=%llu bands=[%c%c%c%c%c%c%c] "
                "threads=%llu withCtx=%llu nonZeroFlag=%llu"
                " | a provisional record was filed first, from a static initialiser"
                " | ESTABLISHES: R1 the task-dispatch flag over every thread carrying an engine "
                "context; R2 the stage-0..3 gate bit; R4 which band arms are driven, with an "
                "out-of-band counter that would falsify the static reading; R5 the record count and "
                "whether it grew. DOES NOT ESTABLISH: R3, the EndFrame/stage-6 ordering, which was "
                "settled statically instead and would need detours on two game functions to confirm "
                "in vivo; and nothing about WHICH thread runs the frame loop -- R1 is answered over "
                "all threads so it does not have to be.",
                (unsigned long long)GameFromSingleton(), (unsigned long long)g.spacesSeen,
                g.schemeIdxSeen, (unsigned long long)g.firstRecordCount,
                (unsigned long long)g.lastRecordCount, (unsigned long long)g.gateSamples,
                (unsigned long long)g.gateSetSamples, (unsigned long long)g.windowSamples,
                (unsigned long long)g.windowCount, g.bandDriven[0] ? '0' : '.',
                g.bandDriven[1] ? '1' : '.', g.bandDriven[2] ? '2' : '.', g.bandDriven[3] ? '3' : '.',
                g.bandDriven[4] ? '4' : '.', g.bandDriven[5] ? '5' : '.', g.bandDriven[6] ? '6' : '.',
                (unsigned long long)g.threadCount, (unsigned long long)g.threadsWithContext,
                (unsigned long long)g.threadsWithNonZeroFlag);
    strncat_s(detail, sizeof detail, tail, _TRUNCATE);

    const char* verdict = failed != 0 ? "FAIL" : (undetermined != 0 ? "NOT-RUN" : "PASS");
    WriteVerdictFile(false, verdict, ok + failed, undetermined, detail);
    return 0;
}

void StartSampler() {
    const HANDLE th = CreateThread(nullptr, 0, SamplerMain, nullptr, 0, nullptr);
    if (th != nullptr) {
        CloseHandle(th);
    }
}

void PinSelfAndNameIt() {
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&StartSampler), &g.self);
    char dll[MAX_PATH] = {0};
    GetModuleFileNameA(g.self, dll, sizeof dll);
    const char* leaf = strrchr(dll, '\\');
    leaf = leaf != nullptr ? leaf + 1 : dll;
    strncpy_s(g.stem, sizeof g.stem, leaf, _TRUNCATE);
    if (char* dot = strrchr(g.stem, '.')) {
        *dot = '\0';
    }
}

// ★ The static initialiser. It pins the module, learns its own name and files the PROVISIONAL record.
// It starts no thread and waits for nothing: this runs under the Windows loader lock.
struct EarlyFiling {
    EarlyFiling();
};

EarlyFiling::EarlyFiling() {
    PinSelfAndNameIt();
#if !defined(W1M_MUTANT_FILE_FROM_MODULE_INIT)
    FileProvisional();
#endif
}

const EarlyFiling g_earlyFiling;

}  // namespace

#if defined(W1M_MUTANT_FILE_FROM_MODULE_INIT)

// ★ THE MUTANT. Not a deletion of the fix -- a faithful reproduction of the shape the fix replaced:
// the verdict is filed from a bootstrap module's Init, which never runs when coexist::ClaimInstance
// refuses this module because another copy claimed first. tests/uidag/check.ps1 loads a claimer ahead
// of it and requires that this build files NOTHING while the real one files. Without that, "the fix
// works" would rest on the fix never having been tested against the condition it exists for.
#include "bootstrap/services.h"

namespace {
bool MutantInit(replay::ReplayServices*) {
    FileProvisional();
    StartSampler();
    return true;
}
}  // namespace

REPLAY_MODULE("w1m-uidag-mutant", 900, MutantInit, nullptr, nullptr);

#else

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // The sampler is created here rather than in the static initialiser: CreateThread under the
        // loader lock is legal only because we never wait on the result, and DllMain is where that
        // contract is visible. The new thread blocks on its own DLL_THREAD_ATTACH until this returns.
        StartSampler();
    }
    return TRUE;
}

#endif
