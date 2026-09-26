// W2-D. The collector for the seven parked reads in runs/queue/payload/W2-D.json.
//
// =================================================================================================
// THE ONE THAT MATTERS IS clock-paused-scaled-delta, AND IT IS THE ACCEPTANCE FOR DL-05.
//
// A stock CClock leaves CLOCK.MINELAPSED_OFF (+0x58) at ZERO -- sub_1400CD8F0 sets it and nothing
// else in the image writes it -- and with a zero minimum the engine's OWN pause bit is a live
// divide by zero:
//
//     paused, no step armed  ->  raw = 0                        (0x1400CF406)
//     clamp(0, min=0, max)   ->  0                              (0x1400CF5CE..0x1400CF5DD)
//     round(0.0f * scale)    ->  0
//     clamp again            ->  0                              (0x1400CF6B0..0x1400CF6BA)
//     +0x68 = 0  ->  +0x3C = 0.0f  ->  +0x40 = 1.0f/0.0f = +inf (0x1400CF6FB)
//
// src/engine/clock.cpp writes a 1 us floor BEFORE it ever sets the pause bit, and REFUSES to pause
// if the floor does not take (ClockRefusal::NoFloor). ★★★ Nothing has ever confirmed that in a
// running engine. This probe pauses through the shipping IReplayClock and asserts, over consecutive
// frames, that +0x68 is NEVER 0 and +0x40 is NEVER 0x7F800000.
//
// ★★ AND IT CANNOT PASS VACUOUSLY. A finite reciprocal on a clock that is not paused proves nothing
// -- it is what an untouched engine does all day. Every sample therefore records CLOCK.FLAGS_OFF
// alongside the two values, and the check is UNDETERMINED unless the pause bit was observed SET on
// every sample it scored. The discriminating question is not "is +0x40 finite" but "is +0x40 finite
// WHILE BIT 1 IS SET", and only the second is evidence about DL-05.
//
// ★ The red arm is deliberately NOT constructed in vivo. Making this check fail on demand means
// writing 0 back into +0x58 and pausing, which injects +inf into a running physics engine on the
// owner's machine. It is refused. The arithmetic's red arm is compiled and run offline in
// tests/clock/test_clock.cpp; what this probe adds is the live reading, and the vacuity guard above
// is what keeps that reading honest.
//
// =================================================================================================
// WHY THE CENSUS RUNS FROM A MODULE AT ORDER 305.
//
// Three of the reads ask what the engine left in the clock BEFORE we touched it, and this module
// takes the clock on its first tick. modreg ticks modules in ascending order within one stage-0
// tick, and the clock module is ORDER 310 -- so a module at 305 observes the clock untouched on the
// very first tick, and every frame after that observes the state our own write produced one frame
// earlier. That ordering is the whole reason the at-rest census is trustworthy; reading from a
// worker thread would race the first Apply and could not say which side of it the sample came from.
//
// ★ NO TRANSPORT IS LINKED, and the scope statement says so. ReplayServices::transport is null here,
// so IReplayClock's mirror is inert and this probe drives the clock through its own public API. The
// REGISTER STATE under test is identical either way -- pause is CLOCK.FLAGS_OFF bit 1 whoever asked
// for it -- but "the transport's Pause() reached the clock" is NOT established by this run, and is
// not claimed.
// =================================================================================================

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bootstrap/bootstrap.h"
#include "bootstrap/buildguard.h"
#include "bootstrap/offlinelock.h"
#include "bootstrap/services.h"
#include "engine/clock.h"
#include "engine/clock_facts.h"
#include "engine/tick.h"
#include "generated/facts_generated.h"

#include "release_check.h"

namespace {

using namespace replay::clock_facts;

// f32 +inf. The value CLOCK.RECIP_OFF takes when the scaled delta reaches zero, and the single
// number this probe exists to prove absent.
const uint32_t kF32PositiveInf = 0x7F800000u;

const uint32_t kPausedSamples = 16;   // >= the ten the parked read asks for
const uint32_t kRateCells = 5;
const uint32_t kFramesPerRateCell = 120;
const uint32_t kSettleFrames = 30;

const float kRates[kRateCells] = {0.1f, 0.25f, 1.0f, 2.0f, 4.0f};

template <typename T>
T Load(uintptr_t p) {
    T v;
    memcpy(&v, reinterpret_cast<const void*>(p), sizeof(T));
    return v;
}

struct ClockCensus {
    bool     read;
    uint64_t ptr;
    uint64_t vtable;
    uint64_t slot8;          // vtable[+8]; must equal TICK.HEARTBEAT_BEFORE rebased
    bool     isCClock;
    uint32_t timescaleBits;
    uint64_t flags;
    uint64_t minElapsed;
    uint64_t maxElapsed;
    uint64_t scaledDelta;
    uint32_t recipBits;
    uint64_t simTime;
    uint64_t rawLastBeat;    // +0x78, the raw microseconds of the last heartbeat
};

struct PausedSample {
    uint32_t frame;
    uint64_t scaledDelta;
    uint32_t recipBits;
    uint64_t flags;
    uint64_t simTime;
};

struct RateCell {
    float    commanded;
    uint32_t writtenBits;
    bool     achievedKnown;
    double   achieved;
    double   framePeriodUs;
    double   ceiling;
};

struct Census {
    // Preconditions.
    bool buildOk;
    bool sitesVerified;
    char failedSite[128];
    bool tickFired;
    uint32_t ticks;
    uint64_t moduleBase;

    // The guard this probe DECLARES, read from shared memory so it is a statement about the
    // PROCESS and not about whether this module won the install race.
    char lockState[48];
    bool guardHeld;

    // Phase 0, taken at ORDER 305 on the FIRST tick, before the clock module has written anything.
    bool     atRestTaken;
    ClockCensus global, third, game;
    uint64_t heartbeatBeforeVA;   // what slot8 must equal
    uint64_t clockVtableVA;       // the second, weaker witness

    // Phase 2.
    uint32_t pausedCount;
    PausedSample paused[kPausedSamples];
    bool     everZeroDelta;
    bool     everInfRecip;
    bool     pauseBitSetOnEverySample;

    // Phase 3.
    bool     stepArmed;
    uint64_t simBeforeStep;
    uint64_t simAfterStep;
    uint64_t stepCommandedUs;

    // Phase 4.
    uint32_t rateCells;
    RateCell rates[kRateCells];

    // Phase 5.
    bool     lateTaken;
    uint64_t minElapsedLate;
    uint32_t lateFrame;

    // Phase 6.
    bool     released;
    bool     registersReadAfterRelease;
    uint32_t timescaleAfterRelease;
    uint64_t flagsAfterRelease;
    uint64_t minElapsedAfterRelease;
    // The module's own account of itself AFTER the release, kept apart from the phase-5 reading:
    // clear bits are also what a module that has not re-taken YET looks like, and "held" is the
    // field that tells the two apart.
    bool     releaseStatusRead;
    bool     heldAfterRelease;
    bool     everHeldAfterRelease;   // a take preceded the release: what "restored" is about
    bool     latchedAfterRelease;    // Status().released: the module says it will not re-take
    uint32_t refusalAfterRelease;
    char     refusalNameAfterRelease[32];

    // The module's own account of itself.
    bool     statusRead;
    uint32_t refusal;
    char     refusalName[32];
    bool     held;
    uint64_t minElapsedAtTake;
    uint64_t statusMaxElapsed;
    bool     maxElapsedLive;
    uint32_t clocksFound;
    uint32_t clocksWritten;
    uint64_t simRegressions;
    uint64_t simImplausible;
    uint64_t wallRejected;
    uint64_t skippedFrameSamples;
    uint64_t stepsArmedTotal;

    // The DECLARED completion point: W2-D.probe.json says completesAt "match". Sampled every tick,
    // because a match arriving late still answers the question and the probe must not have to guess.
    bool     matchSeen;
    uint32_t matchTick;

    bool complete;
};

struct State {
    Census census;
    HMODULE self;
    std::atomic<bool> bootstrapOk{false};
    std::atomic<bool> done{false};
    uint32_t phase;
    uint32_t phaseFrame;
    uint32_t rateIndex;
};

State g;

// ------------------------------------------------------------------------------------------------
// Artifact plumbing. Same shape as tests/camera/probe_teamgate.cpp, and for the same reason: the
// verdict is filed from a STATIC INITIALISER before anything can refuse this module, because in a
// multi-probe launch the instance claim refuses every DLL after the alphabetically first and five
// probes once filed nothing for exactly that reason.
// ------------------------------------------------------------------------------------------------

void JsonEscape(char* out, size_t cap, const char* in) {
    size_t o = 0;
    for (size_t i = 0; in != nullptr && in[i] != 0 && o + 2 < cap; ++i) {
        const unsigned char ch = static_cast<unsigned char>(in[i]);
        if (ch == '"' || ch == 0x5C) {
            out[o++] = static_cast<char>(0x5C);
            out[o++] = static_cast<char>(ch);
        } else if (ch < 0x20) {
            out[o++] = ' ';
        } else {
            out[o++] = static_cast<char>(ch);
        }
    }
    out[o < cap ? o : cap - 1] = 0;
}

void DllStem(char* out, size_t cap) {
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(g.self, path, sizeof path);
    const char* slash = strrchr(path, static_cast<char>(0x5C));
    const char* name = slash != nullptr ? slash + 1 : path;
    strncpy_s(out, cap, name, _TRUNCATE);
    if (char* dot = strrchr(out, '.')) {
        *dot = 0;
    }
}

void ProbeDir(char* out, size_t cap) {
    char root[MAX_PATH] = {0};
    GetEnvironmentVariableA("REPLAY_PROBE_ROOT", root, sizeof root);
    char stem[MAX_PATH];
    DllStem(stem, sizeof stem);
    _snprintf_s(out, cap, _TRUNCATE, "%s%c%s", root, static_cast<char>(0x5C), stem);
    CreateDirectoryA(out, nullptr);
}

// "W2-D-unknown" is the fallback and never a real id: a done.json whose `probe` does not match the
// plan reads as NOT-RUN, which is the correct meaning of "this DLL could not tell which probe it was".
void ProbeId(char* out, size_t cap) {
    strncpy_s(out, cap, "W2-D-unknown", _TRUNCATE);
    char ids[1024] = {0};
    GetEnvironmentVariableA("REPLAY_PROBE_IDS", ids, sizeof ids);
    char stem[MAX_PATH];
    DllStem(stem, sizeof stem);
    for (char* entry = ids; *entry != 0;) {
        char* end = strchr(entry, ';');
        if (end != nullptr) {
            *end = 0;
        }
        if (char* eq = strchr(entry, '=')) {
            *eq = 0;
            if (_stricmp(entry, stem) == 0) {
                strncpy_s(out, cap, eq + 1, _TRUNCATE);
                return;
            }
        }
        if (end == nullptr) {
            break;
        }
        entry = end + 1;
    }
}

void OpenArtifact(FILE** fh, const char* name) {
    char dir[MAX_PATH];
    ProbeDir(dir, sizeof dir);
    char path[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s%c%s", dir, static_cast<char>(0x5C), name);
    if (fopen_s(fh, path, "wb") != 0) {
        *fh = nullptr;
    }
}

void WriteAttached(const char* id) {
    FILE* fh = nullptr;
    OpenArtifact(&fh, "attached.json");
    if (fh != nullptr) {
        fprintf(fh,
                "{\"probe\":\"%s\",\"event\":\"static-initialiser-ran\",\"beforeWorker\":true,"
                "\"tickMs\":%llu}\n",
                id, static_cast<unsigned long long>(GetTickCount64()));
        fclose(fh);
    }
}

void WriteProvisional(const char* id) {
    FILE* fh = nullptr;
    OpenArtifact(&fh, "done.json");
    if (fh != nullptr) {
        fprintf(fh,
                "{\"probe\":\"%s\",\"verdict\":\"NOT-RUN\",\"checked\":0,\"provisional\":true,"
                "\"detail\":\"attached and waiting for the stage-0 tick; this file is provisional "
                "and is overwritten by the real verdict. If you are reading it, the process was "
                "killed first or no tick source ever went live.\"}\n",
                id);
        fclose(fh);
    }
}

// ------------------------------------------------------------------------------------------------
// The collection.
// ------------------------------------------------------------------------------------------------

// ★ Every static address goes through buildguard::MappedAddress, which rebases and range-checks in
// one place. echovr.exe is DYNAMICBASE + HIGH_ENTROPY_VA and a raw dereference of a preferred-base
// VA reads unmapped memory, SILENTLY -- three clock reads came back false exactly that way and the
// false was read as "the engine isn't ready yet".
const void* Mapped(uint64_t pcVa) { return replay::buildguard::MappedAddress(pcVa); }

bool Readable(const void* p, size_t bytes) {
    if (p == nullptr) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT) {
        return false;
    }
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & ok) == 0 || (mbi.Protect & PAGE_GUARD) != 0) {
        return false;
    }
    return static_cast<const uint8_t*>(p) + bytes <=
           static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
}

void ReadClock(ClockCensus* c, uintptr_t obj) {
    memset(c, 0, sizeof *c);
    c->ptr = obj;
    if (obj == 0 || !Readable(reinterpret_cast<const void*>(obj), 0xA8)) {
        return;
    }
    c->read = true;
    c->vtable = Load<uint64_t>(obj);
    if (Readable(reinterpret_cast<const void*>(c->vtable + kVtblHeartbeatOff), sizeof(uint64_t))) {
        c->slot8 = Load<uint64_t>(static_cast<uintptr_t>(c->vtable) + kVtblHeartbeatOff);
    }
    c->isCClock = (c->slot8 != 0 &&
                   c->slot8 == reinterpret_cast<uint64_t>(Mapped(facts::TICK_HEARTBEAT_BEFORE)));
    c->timescaleBits = Load<uint32_t>(obj + facts::CLOCK_TIMESCALE_OFF);
    c->flags         = Load<uint64_t>(obj + facts::CLOCK_FLAGS_OFF);
    c->minElapsed    = Load<uint64_t>(obj + facts::CLOCK_MINELAPSED_OFF);
    c->maxElapsed    = Load<uint64_t>(obj + facts::CLOCK_MAXELAPSED_OFF);
    c->scaledDelta   = Load<uint64_t>(obj + facts::CLOCK_SCALEDDELTA_OFF);
    c->recipBits     = Load<uint32_t>(obj + facts::CLOCK_RECIP_OFF);
    c->simTime       = Load<uint64_t>(obj + facts::CLOCK_ACCUM1_OFF);
    c->rawLastBeat   = Load<uint64_t>(obj + 0x78);
}

uintptr_t GlobalClock() {
    const void* slot = Mapped(facts::CLOCK_GLOBAL_PTR);
    return (slot != nullptr && Readable(slot, sizeof(void*)))
               ? static_cast<uintptr_t>(Load<uint64_t>(reinterpret_cast<uintptr_t>(slot)))
               : 0;
}

uintptr_t ThirdClock() {
    const void* slot = Mapped(facts::CLOCK_THIRD_PTR);
    return (slot != nullptr && Readable(slot, sizeof(void*)))
               ? static_cast<uintptr_t>(Load<uint64_t>(reinterpret_cast<uintptr_t>(slot)))
               : 0;
}

uintptr_t GameClock() {
    const void* slot = Mapped(facts::GAME_SINGLETON_VA);
    if (slot == nullptr || !Readable(slot, sizeof(void*))) {
        return 0;
    }
    const uintptr_t game = static_cast<uintptr_t>(Load<uint64_t>(reinterpret_cast<uintptr_t>(slot)));
    if (game == 0 || !Readable(reinterpret_cast<const void*>(game + facts::GAME_CLOCK_PTR_OFF),
                               sizeof(void*))) {
        return 0;
    }
    return static_cast<uintptr_t>(Load<uint64_t>(game + facts::GAME_CLOCK_PTR_OFF));
}

// The declared completion condition, read the way W1-E's in-match probe reads it: CR15Game with a
// live CR15NetGame is what "a match" means to every other probe in this sweep, so the two agree on
// the fact rather than each inventing one.
uintptr_t NetGame() {
    const void* slot = Mapped(facts::GAME_SINGLETON_VA);
    if (slot == nullptr || !Readable(slot, sizeof(void*))) {
        return 0;
    }
    const uintptr_t game = static_cast<uintptr_t>(Load<uint64_t>(reinterpret_cast<uintptr_t>(slot)));
    if (game == 0 || !Readable(reinterpret_cast<const void*>(game + facts::GAME_NETGAME_PTR_OFF),
                               sizeof(void*))) {
        return 0;
    }
    return static_cast<uintptr_t>(Load<uint64_t>(game + facts::GAME_NETGAME_PTR_OFF));
}

bool VerifySites(Census* c) {
    struct Site {
        const char* id;
        uint64_t va;
        const uint8_t* want;
        size_t len;
    };
    const Site sites[] = {
        {"TICK.HEARTBEAT_BEFORE", facts::TICK_HEARTBEAT_BEFORE, facts::TICK_HEARTBEAT_BEFORE_PROLOGUE,
         sizeof facts::TICK_HEARTBEAT_BEFORE_PROLOGUE},
        {"TICK.UPDATE_CLOCK", facts::TICK_UPDATE_CLOCK, facts::TICK_UPDATE_CLOCK_PROLOGUE,
         sizeof facts::TICK_UPDATE_CLOCK_PROLOGUE},
        {"CLOCK.CTOR", kCtorVa, kCtorPrologue, sizeof kCtorPrologue},
    };
    for (size_t i = 0; i < sizeof sites / sizeof sites[0]; ++i) {
        const void* at = Mapped(sites[i].va);
        if (at == nullptr || !Readable(at, sites[i].len) ||
            memcmp(at, sites[i].want, sites[i].len) != 0) {
            strncpy_s(c->failedSite, sizeof c->failedSite, sites[i].id, _TRUNCATE);
            return false;
        }
    }
    // Known-answer control, on the bytes actually mapped in this image: the same comparison one byte
    // along must REFUSE. Without it every match above could be a comparison that cannot fail.
    const uint8_t* ctor = static_cast<const uint8_t*>(Mapped(kCtorVa));
    if (ctor != nullptr && Readable(ctor + 1, sizeof kCtorPrologue) &&
        memcmp(ctor + 1, kCtorPrologue, sizeof kCtorPrologue) == 0) {
        strncpy_s(c->failedSite, sizeof c->failedSite,
                  "CONTROL DID NOT REFUSE: CLOCK.CTOR matched one byte along", _TRUNCATE);
        return false;
    }
    return true;
}

void ReadStatus(Census* c) {
    replay::IReplayClock* clk = replay::clock::Get();
    if (clk == nullptr) {
        return;
    }
    const replay::ClockStatus s = clk->Status();
    c->statusRead = true;
    c->refusal = static_cast<uint32_t>(s.refusal);
    strncpy_s(c->refusalName, sizeof c->refusalName, replay::ClockRefusalName(s.refusal), _TRUNCATE);
    c->held = s.held;
    c->minElapsedAtTake = s.minElapsedUsAtTake;
    c->statusMaxElapsed = s.maxElapsedUs;
    c->maxElapsedLive = s.maxElapsedLive;
    c->clocksFound = s.clocksFound;
    c->clocksWritten = s.clocksWritten;
    c->simRegressions = s.simRegressions;
    c->simImplausible = s.simImplausible;
    c->wallRejected = s.wallRejected;
    c->skippedFrameSamples = s.skippedFrameSamples;
    c->stepsArmedTotal = s.stepsArmed;
}

// ORDER 305: one tick before the clock module at 310, so phase 0 sees the clock as the engine left
// it and every later phase sees what our own write produced one frame earlier.
void ProbeTick(const replay::TickInfo& info) {
    Census& c = g.census;
    if (c.complete) {
        return;
    }
    ++c.ticks;
    c.tickFired = true;

    if (!c.buildOk) {
        c.buildOk = replay::buildguard::Passed();
        if (!c.buildOk) {
            return;
        }
        c.sitesVerified = VerifySites(&c);
        c.moduleBase = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        c.heartbeatBeforeVA = reinterpret_cast<uint64_t>(Mapped(facts::TICK_HEARTBEAT_BEFORE));
        c.clockVtableVA = reinterpret_cast<uint64_t>(Mapped(kVtableVa));
    }
    if (!c.sitesVerified) {
        c.complete = true;
        return;
    }

    // Before the phase machine, so it is sampled on every tick this module sees rather than only in
    // the phases that happen to read a pointer.
    if (!c.matchSeen && NetGame() != 0) {
        c.matchSeen = true;
        c.matchTick = c.ticks;
    }

    replay::IReplayClock* clk = replay::clock::Get();
    ++g.phaseFrame;

    switch (g.phase) {
        case 0: {
            // ★ THE AT-REST CENSUS, and it is only trustworthy because of the module order. The
            // clock module has not ticked yet this frame, so nothing below has been written by us.
            ReadClock(&c.global, GlobalClock());
            ReadClock(&c.third, ThirdClock());
            ReadClock(&c.game, GameClock());
            c.atRestTaken = c.global.read;
            if (!c.atRestTaken) {
                // The engine has not built the global clock yet. Stay in phase 0 rather than
                // recording a census of nothing.
                return;
            }
            g.phase = 1;
            g.phaseFrame = 0;
            break;
        }
        case 1: {
            // Let the clock module take, write the floor and settle. Paused is its default state
            // with no transport present, so no command is needed to reach the subject.
            if (clk != nullptr) {
                clk->SetPaused(true);
            }
            if (g.phaseFrame >= kSettleFrames) {
                ReadStatus(&c);
                g.phase = 2;
                g.phaseFrame = 0;
            }
            break;
        }
        case 2: {
            // ★★★ clock-paused-scaled-delta. +0x68 must never be 0; +0x40 must never be +inf; and
            // the pause bit must be observed SET on every sample, or the reading is about an
            // unpaused clock and proves nothing.
            const uintptr_t obj = GlobalClock();
            if (obj != 0 && c.pausedCount < kPausedSamples) {
                PausedSample& s = c.paused[c.pausedCount++];
                s.frame = info.frameIndex;
                s.scaledDelta = Load<uint64_t>(obj + facts::CLOCK_SCALEDDELTA_OFF);
                s.recipBits = Load<uint32_t>(obj + facts::CLOCK_RECIP_OFF);
                s.flags = Load<uint64_t>(obj + facts::CLOCK_FLAGS_OFF);
                s.simTime = Load<uint64_t>(obj + facts::CLOCK_ACCUM1_OFF);
                if (s.scaledDelta == 0) c.everZeroDelta = true;
                if (s.recipBits == kF32PositiveInf) c.everInfRecip = true;
            }
            if (c.pausedCount >= kPausedSamples) {
                c.pauseBitSetOnEverySample = true;
                for (uint32_t i = 0; i < c.pausedCount; ++i) {
                    if ((c.paused[i].flags & kPauseBit) == 0) {
                        c.pauseBitSetOnEverySample = false;
                    }
                }
                g.phase = 3;
                g.phaseFrame = 0;
            }
            break;
        }
        case 3: {
            // clock-step-once-beat-count. One arm, one frame. The expected advance is the commanded
            // step plus one or two floor-sized beats -- NOT exactly the step, and a check that
            // demanded exactness would fail on correct behaviour.
            const uintptr_t obj = GlobalClock();
            if (g.phaseFrame == 1 && obj != 0 && clk != nullptr) {
                c.simBeforeStep = Load<uint64_t>(obj + facts::CLOCK_ACCUM1_OFF);
                c.stepCommandedUs = 16666;
                clk->SetStepUs(c.stepCommandedUs);
                c.stepArmed = true;
            } else if (g.phaseFrame >= 4 && obj != 0) {
                c.simAfterStep = Load<uint64_t>(obj + facts::CLOCK_ACCUM1_OFF);
                g.phase = 4;
                g.phaseFrame = 0;
                g.rateIndex = 0;
            }
            break;
        }
        case 4: {
            // clock-rate-round-trip. No transport is linked, so the mirror is inert and a rate set
            // here is not overwritten -- which is exactly why this probe can drive it at all.
            if (clk == nullptr) {
                g.phase = 5;
                g.phaseFrame = 0;
                break;
            }
            if (g.phaseFrame == 1) {
                clk->SetPaused(false);
                clk->SetRate(kRates[g.rateIndex]);
            } else if (g.phaseFrame >= kFramesPerRateCell) {
                const replay::ClockStatus s = clk->Status();
                const uintptr_t obj = GlobalClock();
                RateCell& cell = c.rates[c.rateCells++];
                cell.commanded = kRates[g.rateIndex];
                cell.writtenBits = obj != 0 ? Load<uint32_t>(obj + facts::CLOCK_TIMESCALE_OFF) : 0;
                cell.achievedKnown = s.rateKnown;
                cell.achieved = s.achievedRate;
                cell.framePeriodUs = s.framePeriodUs;
                cell.ceiling = s.ceiling;
                ++g.rateIndex;
                g.phaseFrame = 0;
                if (g.rateIndex >= kRateCells) {
                    clk->SetRate(1.0f);
                    clk->SetPaused(true);
                    g.phase = 5;
                }
            }
            break;
        }
        case 5: {
            // clock-min-elapsed-late. We wrote 1 into +0x58 at take; reading 1 here says NOTHING
            // ELSE has written it since, which is the question the pair exists to answer. Any other
            // value means a writer exists that the offset-keyed offline sweep could not see.
            if (g.phaseFrame >= 300) {
                const uintptr_t obj = GlobalClock();
                if (obj != 0) {
                    c.minElapsedLate = Load<uint64_t>(obj + facts::CLOCK_MINELAPSED_OFF);
                    c.lateTaken = true;
                    c.lateFrame = info.frameIndex;
                }
                ReadStatus(&c);
                g.phase = 6;
                g.phaseFrame = 0;
            }
            break;
        }
        case 6: {
            // Put it back. A probe that leaves the engine paused with our floor installed would
            // hand the next thing in this launch a clock nobody configured.
            if (clk != nullptr && !c.released) {
                clk->Release();
                c.released = true;
            } else if (c.released) {
                // ★ ONE TICK LATER, DELIBERATELY. Reading in the same tick as the Release() call
                // would read before the module's next tick had a chance to undo it, and the defect
                // this pair exists to catch is exactly a release that the next tick undoes.
                const uintptr_t obj = GlobalClock();
                if (obj != 0) {
                    c.timescaleAfterRelease = Load<uint32_t>(obj + facts::CLOCK_TIMESCALE_OFF);
                    c.flagsAfterRelease = Load<uint64_t>(obj + facts::CLOCK_FLAGS_OFF);
                    c.minElapsedAfterRelease = Load<uint64_t>(obj + facts::CLOCK_MINELAPSED_OFF);
                    c.registersReadAfterRelease = true;
                }
                if (replay::IReplayClock* self = replay::clock::Get()) {
                    const replay::ClockStatus s = self->Status();
                    c.releaseStatusRead = true;
                    c.heldAfterRelease = s.held;
                    c.everHeldAfterRelease = s.everHeld;
                    c.latchedAfterRelease = s.released;
                    c.refusalAfterRelease = static_cast<uint32_t>(s.refusal);
                    strncpy_s(c.refusalNameAfterRelease, sizeof c.refusalNameAfterRelease,
                              replay::ClockRefusalName(s.refusal), _TRUNCATE);
                }
                c.complete = true;
                g.done.store(true, std::memory_order_release);
            }
            break;
        }
        default:
            break;
    }
}

struct Check {
    const char* name;
    const char* outcome;
    const char* detail;
};

void CollectGuard(Census* c) {
    // Read out of SHARED MEMORY, so this is what the hook observed for the PROCESS, not whether
    // this module won the install race. AlreadyLockedInProcess is a SUCCESS: every probe DLL links
    // the library, MinHook refuses the duplicates, and a module reporting that is describing a
    // process that IS locked.
    const replay::offlinelock::LockState st = replay::offlinelock::Current().state;
    strncpy_s(c->lockState, sizeof c->lockState, replay::offlinelock::LockStateName(st), _TRUNCATE);
    c->guardHeld = (st == replay::offlinelock::LockState::Locked ||
                    st == replay::offlinelock::LockState::AlreadyLockedInProcess);
}

void WriteVerdict(const char* id) {
    Census& c = g.census;
    CollectGuard(&c);

    // 13 `checks[n++]` sites follow; the slack is here so adding one is not a stack write past
    // the end of this array. Count them before you add the fourteenth.
    Check checks[16];
    uint32_t n = 0;

    checks[n++] = {"offline-bit-lock-held", c.guardHeld ? "ok" : "failed",
                   c.guardHeld ? c.lockState
                               : "DECLARED guards:[offline-bit-lock] but the process is NOT "
                                 "protected -- treat this launch as unprotected"};
    checks[n++] = {"build-is-echovr", c.buildOk ? "ok" : "undetermined",
                   c.buildOk ? "the mapped main module is the measured echovr.exe"
                             : "not the expected build -- every reading below is void, and this is "
                               "NOT a defect report"};
    checks[n++] = {"sites-verify", !c.buildOk ? "undetermined" : (c.sitesVerified ? "ok" : "failed"),
                   c.sitesVerified
                       ? "TICK.HEARTBEAT_BEFORE, TICK.UPDATE_CLOCK and CLOCK.CTOR hold their "
                         "registry bytes after rebase, and the one-byte-along control refused"
                       : (c.buildOk ? c.failedSite : "not evaluated: wrong build")};
    checks[n++] = {"tick-fired", c.tickFired ? "ok" : "undetermined",
                   c.tickFired ? "the stage-0 tick reached this module"
                               : "no tick source ever went live, so nothing was measured"};
    checks[n++] = {"at-rest-census",
                   !c.tickFired ? "undetermined" : (c.atRestTaken ? "ok" : "undetermined"),
                   c.atRestTaken ? "the global clock was read at ORDER 305 on the first tick, "
                                   "before the clock module at 310 wrote anything"
                                 : "CLOCK.GLOBAL_PTR was still null every frame"};
    checks[n++] = {"global-is-a-cclock",
                   !c.atRestTaken ? "undetermined" : (c.global.isCClock ? "ok" : "failed"),
                   c.global.isCClock
                       ? "vtable slot +8 on CLOCK.GLOBAL_PTR is TICK.HEARTBEAT_BEFORE, so the "
                         "clamp order every offset here was derived from applies to this object"
                       : "the object at CLOCK.GLOBAL_PTR is NOT stepped by TICK.HEARTBEAT_BEFORE"};
    checks[n++] = {"clock-took",
                   !c.statusRead ? "undetermined" : (c.held ? "ok" : "failed"),
                   c.held ? "IReplayClock holds the clock" : c.refusalName};

    // ★★★ THE ONE. And the vacuity guard is the first condition, not an afterthought: a finite
    // reciprocal on an unpaused clock is what an untouched engine produces all day.
    const char* pauseOutcome;
    const char* pauseDetail;
    if (c.pausedCount < kPausedSamples) {
        pauseOutcome = "undetermined";
        pauseDetail = "fewer than the required consecutive paused samples were taken";
    } else if (!c.pauseBitSetOnEverySample) {
        pauseOutcome = "undetermined";
        pauseDetail = "CLOCK.FLAGS_OFF bit 1 was NOT set on every sample, so these readings are "
                      "about a clock that was not paused and say nothing about DL-05";
    } else if (c.everZeroDelta || c.everInfRecip) {
        pauseOutcome = "failed";
        pauseDetail = "PAUSED WITH A ZERO SCALED DELTA OR AN INFINITE RECIPROCAL: the 1 us floor "
                      "did not take, and every consumer of CLOCK.RECIP_OFF is receiving +inf";
    } else {
        pauseOutcome = "ok";
        pauseDetail = "over consecutive frames with CLOCK.FLAGS_OFF bit 1 SET BY THIS PROBE through "
                      "IReplayClock -- the engine did not set it -- the ENGINE's own per-frame update "
                      "produced neither a 0 in CLOCK.SCALEDDELTA_OFF nor 0x7F800000 in "
                      "CLOCK.RECIP_OFF. The register state under test is the same whoever asked for "
                      "the bit, and the arithmetic under test is the engine's; who set the bit is "
                      "not, and the summary of this result must say so";
    }
    checks[n++] = {"paused-delta-never-zero", pauseOutcome, pauseDetail};

    const bool lateOk = c.lateTaken && c.minElapsedLate == kFloorUs;
    checks[n++] = {"min-elapsed-unwritten-by-anyone-else",
                   !c.lateTaken ? "undetermined" : (lateOk ? "ok" : "failed"),
                   !c.lateTaken
                       ? "the late sample was not reached"
                       : (lateOk ? "the 1 us floor we wrote was still there hundreds of frames "
                                   "later, so nothing else writes CLOCK.MINELAPSED_OFF -- which "
                                   "the offline sweep could not establish, being keyed on the "
                                   "[reg+58h] displacement"
                                 : "CLOCK.MINELAPSED_OFF changed under us: a writer exists that the "
                                   "offline displacement sweep cannot see")};

    // ★ THE THREE RELEASE CHECKS, EVALUATED BY tests/clock/release_check.h SO THE OFFLINE SUITE CAN
    // RUN THE SAME CODE AGAINST LAUNCH 10'S RECORDED NUMBERS. Their details are built from what was
    // read, not from what was expected: the version these replace printed the same sentence for ok
    // and for failed, which is how a FAIL came to be reported in words that asserted a PASS.
    replay::clockcheck::ReleaseReading rr;
    // "Ever held" has two witnesses: the phase-5 status (held=true before the release) and the
    // post-release status (everHeld). Either alone establishes it; launch 10 recorded only the first.
    rr.everHeldRead = c.statusRead || c.releaseStatusRead;
    rr.everHeld = (c.statusRead && c.held) || (c.releaseStatusRead && c.everHeldAfterRelease);
    rr.released = c.released;
    rr.registersRead = c.registersReadAfterRelease;
    rr.flags = c.flagsAfterRelease;
    rr.minElapsedUs = c.minElapsedAfterRelease;
    rr.minElapsedAtTakeRead = c.statusRead;
    rr.minElapsedAtTakeUs = c.minElapsedAtTake;
    rr.statusRead = c.releaseStatusRead;
    rr.refusal = static_cast<replay::ClockRefusal>(c.refusalAfterRelease);
    rr.held = c.heldAfterRelease;
    rr.latched = c.latchedAfterRelease;

    char flagsDetail[256], floorDetail[256], stoppedDetail[256];
    const replay::clockcheck::Outcome flagsOutcome =
        replay::clockcheck::FlagsRestored(rr, flagsDetail, sizeof flagsDetail);
    const replay::clockcheck::Outcome floorOutcome =
        replay::clockcheck::FloorRestored(rr, floorDetail, sizeof floorDetail);
    const replay::clockcheck::Outcome stoppedOutcome =
        replay::clockcheck::StoppedAfterRelease(rr, stoppedDetail, sizeof stoppedDetail);

    checks[n++] = {"restored-on-release", replay::clockcheck::OutcomeName(flagsOutcome), flagsDetail};
    checks[n++] = {"floor-restored-on-release", replay::clockcheck::OutcomeName(floorOutcome),
                   floorDetail};
    checks[n++] = {"stopped-after-release", replay::clockcheck::OutcomeName(stoppedOutcome),
                   stoppedDetail};

    // ★ LAST, AND IT IS ABOUT THE RUN RATHER THAN THE CLOCK: did this probe get to the point it
    // declared it would answer at? A reading taken before it is not wrong, but it is not the reading
    // that was queued, and the verdict word has to carry that or the summary will not.
    char completionDetail[512];
    const replay::clockcheck::Outcome completionOutcome = replay::clockcheck::CompletionReached(
        c.matchSeen, c.matchTick, c.ticks, completionDetail, sizeof completionDetail);
    checks[n++] = {"declared-completion-reached", replay::clockcheck::OutcomeName(completionOutcome),
                   completionDetail};

    bool anyFailed = false, anyUndetermined = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (strcmp(checks[i].outcome, "failed") == 0) anyFailed = true;
        // not-run is not a pass either: a release check with nothing to check keeps the verdict
        // off PASS the same way could-not-ask does, and its detail names the reason.
        if (strcmp(checks[i].outcome, "undetermined") == 0 ||
            strcmp(checks[i].outcome, "not-run") == 0) {
            anyUndetermined = true;
        }
    }
    const char* verdict = anyFailed ? "FAIL" : (anyUndetermined ? "COULD-NOT-DETERMINE" : "PASS");

    FILE* fh = nullptr;
    OpenArtifact(&fh, "done.json");
    if (fh == nullptr) {
        return;
    }
    fprintf(fh,
            "{\"probe\":\"%s\",\"verdict\":\"%s\",\"checked\":%u,\"provisional\":false,"
            "\"completesAt\":\"match\",\"completionReached\":%s,\"truncated\":%s,"
            "\"matchTick\":%u,"
            "\"bootstrapAttached\":%s,\"ticks\":%u,\"moduleBase\":\"0x%llX\",\n",
            id, verdict, n, c.matchSeen ? "true" : "false", c.matchSeen ? "false" : "true",
            c.matchTick, g.bootstrapOk.load(std::memory_order_acquire) ? "true" : "false",
            c.ticks, static_cast<unsigned long long>(c.moduleBase));

    fprintf(fh, " \"checks\":[");
    for (uint32_t i = 0; i < n; ++i) {
        char d[512];
        JsonEscape(d, sizeof d, checks[i].detail ? checks[i].detail : "");
        fprintf(fh, "%s{\"name\":\"%s\",\"outcome\":\"%s\",\"detail\":\"%s\"}", i ? "," : "",
                checks[i].name, checks[i].outcome, d);
    }
    fprintf(fh, "],\n");

    // The three clocks, raw. Every field the parked reads ask for, plus the module base and the
    // pointer actually dereferenced -- so a 0 meaning "the flag is clear" can be told apart from a
    // 0 meaning "I read unmapped memory".
    const ClockCensus* cl[3] = {&c.global, &c.third, &c.game};
    const char* names[3] = {"global", "third", "game"};
    fprintf(fh, " \"clocksAtRest\":[");
    for (int i = 0; i < 3; ++i) {
        const ClockCensus& q = *cl[i];
        fprintf(fh,
                "%s{\"name\":\"%s\",\"read\":%s,\"ptr\":\"0x%llX\",\"vtable\":\"0x%llX\","
                "\"vtableSlot8\":\"0x%llX\",\"isCClock\":%s,"
                "\"timescaleBits\":\"0x%08X\",\"flags\":\"0x%llX\",\"minElapsedUs\":%llu,"
                "\"maxElapsedUs\":%llu,\"scaledDeltaUs\":%llu,\"recipBits\":\"0x%08X\","
                "\"simTimeUs\":%llu,\"rawLastBeatUs\":%llu}",
                i ? "," : "", names[i], q.read ? "true" : "false",
                static_cast<unsigned long long>(q.ptr), static_cast<unsigned long long>(q.vtable),
                static_cast<unsigned long long>(q.slot8), q.isCClock ? "true" : "false",
                q.timescaleBits, static_cast<unsigned long long>(q.flags),
                static_cast<unsigned long long>(q.minElapsed),
                static_cast<unsigned long long>(q.maxElapsed),
                static_cast<unsigned long long>(q.scaledDelta), q.recipBits,
                static_cast<unsigned long long>(q.simTime),
                static_cast<unsigned long long>(q.rawLastBeat));
    }
    fprintf(fh, "],\n");

    fprintf(fh,
            " \"expected\":{\"heartbeatBeforeMapped\":\"0x%llX\",\"clockVtableMapped\":\"0x%llX\","
            "\"floorWeWriteUs\":%llu,\"f32PositiveInf\":\"0x%08X\"},\n",
            static_cast<unsigned long long>(c.heartbeatBeforeVA),
            static_cast<unsigned long long>(c.clockVtableVA),
            static_cast<unsigned long long>(kFloorUs), kF32PositiveInf);

    fprintf(fh, " \"pausedSamples\":[");
    for (uint32_t i = 0; i < c.pausedCount; ++i) {
        const PausedSample& s = c.paused[i];
        fprintf(fh,
                "%s{\"frame\":%u,\"scaledDeltaUs\":%llu,\"recipBits\":\"0x%08X\","
                "\"flags\":\"0x%llX\",\"pauseBitSet\":%s,\"simTimeUs\":%llu}",
                i ? "," : "", s.frame, static_cast<unsigned long long>(s.scaledDelta), s.recipBits,
                static_cast<unsigned long long>(s.flags),
                (s.flags & kPauseBit) ? "true" : "false",
                static_cast<unsigned long long>(s.simTime));
    }
    fprintf(fh, "],\n");

    fprintf(fh, " \"rateCells\":[");
    for (uint32_t i = 0; i < c.rateCells; ++i) {
        const RateCell& r = c.rates[i];
        fprintf(fh,
                "%s{\"commanded\":%.4f,\"writtenBits\":\"0x%08X\",\"achievedKnown\":%s,"
                "\"achieved\":%.6f,\"framePeriodUs\":%.2f,\"ceiling\":%.4f}",
                i ? "," : "", static_cast<double>(r.commanded), r.writtenBits,
                r.achievedKnown ? "true" : "false", r.achieved, r.framePeriodUs, r.ceiling);
    }
    fprintf(fh, "],\n");

    fprintf(fh,
            " \"step\":{\"armed\":%s,\"commandedUs\":%llu,\"simBefore\":%llu,\"simAfter\":%llu,"
            "\"advanceUs\":%lld},\n",
            c.stepArmed ? "true" : "false", static_cast<unsigned long long>(c.stepCommandedUs),
            static_cast<unsigned long long>(c.simBeforeStep),
            static_cast<unsigned long long>(c.simAfterStep),
            static_cast<long long>(c.simAfterStep - c.simBeforeStep));

    fprintf(fh,
            " \"minElapsed\":{\"atTakeUs\":%llu,\"takeRead\":%s,\"lateUs\":%llu,\"lateRead\":%s,"
            "\"lateFrame\":%u,\"afterReleaseUs\":%llu},\n",
            static_cast<unsigned long long>(c.minElapsedAtTake), c.statusRead ? "true" : "false",
            static_cast<unsigned long long>(c.minElapsedLate), c.lateTaken ? "true" : "false",
            c.lateFrame, static_cast<unsigned long long>(c.minElapsedAfterRelease));

    fprintf(fh,
            " \"status\":{\"read\":%s,\"refusal\":\"%s\",\"held\":%s,\"maxElapsedUs\":%llu,"
            "\"maxElapsedLive\":%s,\"clocksFound\":%u,\"clocksWritten\":%u,"
            "\"simRegressions\":%llu,\"simImplausible\":%llu,\"wallRejected\":%llu,"
            "\"skippedFrameSamples\":%llu,\"stepsArmed\":%llu},\n",
            c.statusRead ? "true" : "false", c.refusalName, c.held ? "true" : "false",
            static_cast<unsigned long long>(c.statusMaxElapsed), c.maxElapsedLive ? "true" : "false",
            c.clocksFound, c.clocksWritten, static_cast<unsigned long long>(c.simRegressions),
            static_cast<unsigned long long>(c.simImplausible),
            static_cast<unsigned long long>(c.wallRejected),
            static_cast<unsigned long long>(c.skippedFrameSamples),
            static_cast<unsigned long long>(c.stepsArmedTotal));

    fprintf(fh,
            " \"release\":{\"called\":%s,\"registersRead\":%s,\"timescaleBits\":\"0x%08X\","
            "\"flags\":\"0x%llX\",\"minElapsedUs\":%llu,\"statusRead\":%s,\"held\":%s,"
            "\"everHeld\":%s,\"released\":%s,\"refusal\":\"%s\"},\n",
            c.released ? "true" : "false", c.registersReadAfterRelease ? "true" : "false",
            c.timescaleAfterRelease, static_cast<unsigned long long>(c.flagsAfterRelease),
            static_cast<unsigned long long>(c.minElapsedAfterRelease),
            c.releaseStatusRead ? "true" : "false", c.heldAfterRelease ? "true" : "false",
            c.everHeldAfterRelease ? "true" : "false", c.latchedAfterRelease ? "true" : "false",
            c.releaseStatusRead ? c.refusalNameAfterRelease : "");

    fprintf(fh,
            " \"note\":\"NO TRANSPORT IS LINKED into this probe, so ReplayServices::transport is "
            "null, IReplayClock's mirror is inert, and the probe drives the clock through its own "
            "public API. The REGISTER state under test is identical either way -- pause is "
            "CLOCK.FLAGS_OFF bit 1 whoever asked for it -- but this run does NOT establish that "
            "IReplayTransport::Pause() reaches the clock, and does not claim to. The at-rest "
            "census is taken from a module at ORDER 305, one tick ahead of the clock module at "
            "310, which is what makes it a reading of the engine rather than of ourselves. "
            "minElapsed.lateUs is expected to be the floor WE wrote: reading it back unchanged is "
            "how this run establishes that nothing else writes CLOCK.MINELAPSED_OFF.\"}\n");
    fclose(fh);
}

DWORD WINAPI WorkerMain(LPVOID) {
    // Bounded by the descriptor's timeout; this loop only has to not outlive it. Overridable so the
    // offline contract test can exercise the no-tick path in a second instead of four minutes -- it
    // changes only how long we wait, never what is measured or how it is scored.
    uint64_t waitMs = 270000ull;
    char override[32] = {0};
    if (GetEnvironmentVariableA("REPLAY_PROBE_DEADLINE_MS", override, sizeof override) != 0) {
        const long long v = _atoi64(override);
        if (v > 0) {
            waitMs = static_cast<uint64_t>(v);
        }
    }
    const uint64_t deadline = GetTickCount64() + waitMs;
    while (GetTickCount64() < deadline && !g.done.load(std::memory_order_acquire)) {
        Sleep(250);
    }
    char id[128];
    ProbeId(id, sizeof id);
    WriteVerdict(id);
    return 0;
}

// The collector runs as a MODULE at order 305 -- one before the clock module at 310. See the header
// comment: that ordering is what makes the at-rest census a reading of the engine and not of us.
bool ProbeInit(replay::ReplayServices*) { return true; }
void ProbeShutdown() {}

struct AttachFiler {
    AttachFiler() {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&WriteProvisional), &g.self);
        char id[128];
        ProbeId(id, sizeof id);
        WriteAttached(id);
        WriteProvisional(id);
        if (HANDLE worker = CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr)) {
            CloseHandle(worker);
        }
    }
};
AttachFiler g_filer;

REPLAY_MODULE("probe-clock", 305, ProbeInit, ProbeTick, ProbeShutdown);

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(module);
            // ★ DECLARED guards:[offline-bit-lock], so the lock must actually be INSTALLED, not just
            // linked. bootstrap::Attach is what installs it -- linking replay_bootstrap puts the
            // code in the binary without ever running it, and two packets have been caught
            // declaring protection they never armed. WriteVerdict then FAILS the run if
            // offlinelock::Current().state does not report the process as protected, so the
            // declaration cannot outlive the thing it declares.
            g.bootstrapOk.store(replay::bootstrap::Attach(module), std::memory_order_release);
            return TRUE;
        case DLL_PROCESS_DETACH:
            replay::bootstrap::Detach(reserved != nullptr);
            break;
        default:
            break;
    }
    return TRUE;
}
