// W1-E's IN-MATCH probe. Everything here needs a loaded match, a game space and a built component
// pool, so none of it can be answered in an attach window on a profile that never loads a match.
//
// ★ THE READ THIS EXISTS FOR. CR15PlayerBroadcasterCS::UpdateAfterPhysics has TWO arms and picks
// between them on CR15NetGame::IsBotUser. E-4's ReadLocalPlayer reads the CR15EchoUnitCS accessors,
// which is the arm the engine takes when IsBotUser is TRUE. For the local human the engine takes the
// other arm. If the EchoUnit component is not driven for a human, then the owner's own hands are
// being captured from a source the engine does not use for that entry -- and the whole point of the
// local-pawn work is the owner's own hands.
//
// That question cannot be settled from the binary, and it is not settled by "the numbers look
// plausible" either: a stationary player makes a stale read indistinguishable from a live one. So the
// probe samples the EchoUnit pose ALONGSIDE two signals that are known to be live on the human arm --
// the CR15TouchAnimBlendersCS trigger blends and the held-player ids -- over the same window. Triggers
// moving while the pose does not is the falsification; both moving together is the confirmation.
// Neither is inferable from one sample, which is why this is a window and not a read.
//
// ★ AND THE READ THAT MAKES EVERY OTHER READ ON THE SWEEP INTERPRETABLE. CModelCS's visibility dword
// is what CR15NetSpectatorCameraCS::EnableRemoteHeads and EnableRemoteBodies set. Without it,
// "nothing moved" and "nothing was drawn" are the same observation and lead to opposite fixes.
//
// ★ FOUR OUTCOMES, NOT TWO, AND THE VERDICT WORD IS ONE OF THEM. A read that could not be taken
// reports COULD-NOT-DETERMINE and is counted separately; `checked` counts only Ok and Failed, so a
// launch that never reaches a match reports checked=0 rather than a confident nothing. The counter
// then DECIDES the verdict, through probeverdict::Word -- for five verdict sites it did not, and
// launch 07 of the 20260830 sweep filed PASS with checked=6 over its own line "CR15Game resolved
// but its CR15NetGame is null -- no match". done.json also names the stage the run stopped at, so
// the checks that were never reached are visible instead of merely absent.
//
// ★ THE VERDICT IS FILED FROM A STATIC INITIALISER, before DllMain and before any bootstrap module's
// instance claim can refuse this module. A probe whose filing depends on being the first module
// alphabetically is a probe that files nothing on the sweep it was written for.

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>

#include "engine/localpawn_facts.h"
#include "engine/session_facts.h"
#include "generated/facts_generated.h"

#include "verdict_word.h"

namespace {

namespace lp = replay::localpawn_facts;

// ---------------------------------------------------------------------------------------------------
// Rebasing. echovr.exe is DYNAMICBASE with a 163,956-byte relocation directory, so every recorded VA
// is a PREFERRED-base address and casting one straight to a pointer is a wild dereference on any run
// that did not land at 0x140000000. This probe deliberately does NOT link replay_bootstrap -- that
// would pull in the module registry whose instance claim is the very thing the static-initialiser
// filing exists to route around -- so it rebases itself, in one place.
// ---------------------------------------------------------------------------------------------------
uintptr_t ImageDelta() {
    static const uintptr_t delta =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) -
        static_cast<uintptr_t>(facts::PE_IMAGE_BASE);
    return delta;
}

void* Mapped(uint64_t pcVa) {
    return pcVa ? reinterpret_cast<void*>(static_cast<uintptr_t>(pcVa) + ImageDelta()) : nullptr;
}

bool Readable(const void* p, size_t n) {
    if (p == nullptr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof mbi) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD bad = PAGE_NOACCESS | PAGE_GUARD;
    if ((mbi.Protect & bad) != 0) return false;
    const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return reinterpret_cast<uintptr_t>(p) + n <= end;
}

template <typename T>
bool Rd(const void* base, uint32_t off, T& out) {
    const uint8_t* p = static_cast<const uint8_t*>(base) + off;
    if (!Readable(p, sizeof(T))) return false;
    memcpy(&out, p, sizeof(T));
    return true;
}

void* Deref(const void* base, uint32_t off) {
    void* v = nullptr;
    return Rd<void*>(base, off, v) ? v : nullptr;
}

// ---------------------------------------------------------------------------------------------------
// Verdict accounting.
// ---------------------------------------------------------------------------------------------------
struct State {
    HMODULE self = nullptr;
    char stem[MAX_PATH] = {0};
    size_t ok = 0;
    size_t failed = 0;
    size_t undetermined = 0;
    // Set only by the last verdict site. Every earlier one is a run that stopped, and a stop is not
    // a pass however few checks failed before it.
    bool reachedEnd = false;
    char detail[6144] = {0};
} g;

void Note(const char* text) { strncat_s(g.detail, sizeof g.detail, text, _TRUNCATE); }

void NoteF(const char* fmt, ...) {
    char one[640];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(one, sizeof one, _TRUNCATE, fmt, ap);
    va_end(ap);
    Note(one);
}

void Check(bool cond, const char* what) {
    if (cond) {
        ++g.ok;
        return;
    }
    ++g.failed;
    NoteF("%sFAILED: %s", g.failed + g.undetermined > 1 ? " | " : "", what);
}

// A read that could not be taken is neither a pass nor a failure, and saying so is the whole point.
void Undetermined(const char* what) {
    ++g.undetermined;
    NoteF("%sCOULD-NOT-DETERMINE: %s", g.failed + g.undetermined > 1 ? " | " : "", what);
}

void EscapeInto(char* out, size_t bytes, const char* in) {
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 2 < bytes; ++i) {
        if (in[i] == '\\' || in[i] == '"') out[o++] = '\\';
        out[o++] = in[i];
    }
    out[o] = '\0';
}

bool ProbeRoot(char* out, size_t bytes) {
    const DWORD n = GetEnvironmentVariableA("REPLAY_PROBE_ROOT", out, static_cast<DWORD>(bytes));
    return n != 0 && n < bytes;
}

void ProbeId(char* out, size_t bytes) {
    strncpy_s(out, bytes, "W1-E-match", _TRUNCATE);
    char ids[512] = {0};
    GetEnvironmentVariableA("REPLAY_PROBE_IDS", ids, sizeof ids);
    const char* found = strstr(ids, "probe_entitymatch=");
    if (found == nullptr) return;
    const char* value = found + strlen("probe_entitymatch=");
    size_t n = strcspn(value, ";");
    if (n >= bytes) n = bytes - 1;
    memcpy(out, value, n);
    out[n] = '\0';
}

void WriteVerdictFile(bool provisional, const char* verdict, const char* stage, const char* detail) {
    char root[MAX_PATH];
    if (!ProbeRoot(root, sizeof root)) return;  // not in a sweep; never scatter files into the install
    char dir[MAX_PATH];
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", root, g.stem);
    CreateDirectoryA(dir, nullptr);

    char id[128];
    ProbeId(id, sizeof id);
    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(sizeof cwd, cwd);
    char cwdEscaped[MAX_PATH * 2];
    EscapeInto(cwdEscaped, sizeof cwdEscaped, cwd);
    char detailEscaped[sizeof g.detail * 2];
    EscapeInto(detailEscaped, sizeof detailEscaped, detail);

    char path[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\done.json", dir);
    FILE* fh = nullptr;
    if (fopen_s(&fh, path, "wb") != 0 || fh == nullptr) return;
    fprintf(fh,
            "{ \"probe\": \"%s\", \"verdict\": \"%s\", \"checked\": %llu,\n"
            "  \"failed\": %llu, \"undetermined\": %llu, \"stoppedAt\": \"%s\",\n"
            "  \"reachedLastCheck\": %s, \"provisional\": %s, \"cwd\": \"%s\",\n"
            "  \"detail\": \"%s\" }\n",
            id, verdict, (unsigned long long)(g.ok + g.failed), (unsigned long long)g.failed,
            (unsigned long long)g.undetermined, stage, g.reachedEnd ? "true" : "false",
            provisional ? "true" : "false", cwdEscaped, detailEscaped);
    fclose(fh);
}

// ★ THE FIX. Every scored exit files through here, so no site can spell its own verdict again.
void FileVerdict(const char* stage) {
    WriteVerdictFile(false,
                     replay::probeverdict::Word(g.ok, g.failed, g.undetermined, g.reachedEnd), stage,
                     g.detail);
}

void FileProvisional() {
    WriteVerdictFile(true, "NOT-RUN", "static-initialiser",
                     "provisional record written from a static initialiser, before DllMain and before "
                     "any other module's instance claim. The sampler has not run. If this record is "
                     "still here at the deadline the probe LOADED and was killed mid-measurement, "
                     "which is a different fact from never having loaded.");
}

// ---------------------------------------------------------------------------------------------------
// The engine-side reads.
// ---------------------------------------------------------------------------------------------------
using FindCsFn = void* (*)(void* gameSpace, uint64_t symbol);
using FindComponentFn = void* (*)(void* cs, void* out, uint64_t actorLo, uint64_t actorHi);
using TransformFn = void* (*)(void* cs, void* out, uint16_t handle);
using HandTransformFn = void* (*)(void* cs, void* out, uint16_t handle, uint32_t arm);
using UserActorFn = const void* (*)(void* netGame, void* out16, uint16_t slot);

struct Transform {
    float quat[4];
    float pos[3];
    float scale[3];
    float pad[2];
};

bool PrologueOk(uint64_t va, const uint8_t* want, size_t n) {
    const void* p = Mapped(va);
    return Readable(p, n) && memcmp(p, want, n) == 0;
}

uint16_t ComponentOnActor(FindComponentFn find, void* cs, const void* actor) {
    if (find == nullptr || cs == nullptr) return lp::kNoComponent;
    uint8_t scratch[40] = {0};
    uint64_t lo = 0, hi = 0;
    if (!Rd<uint64_t>(actor, 0, lo) || !Rd<uint64_t>(actor, 8, hi)) return lp::kNoComponent;
    find(cs, scratch, lo, hi);
    uint32_t id = 0;
    memcpy(&id, scratch, sizeof id);
    return static_cast<uint16_t>(id);
}

void* LeafElement(void* cs, uint32_t mapOff, uint32_t baseOff, uint32_t stride, uint16_t handle) {
    void* map = Deref(cs, mapOff);
    void* base = Deref(cs, baseOff);
    if (map == nullptr || base == nullptr) return nullptr;
    uint16_t idx = 0;
    if (!Rd<uint16_t>(map, 4u * handle, idx)) return nullptr;
    void* elem = static_cast<uint8_t*>(base) + static_cast<size_t>(stride) * idx;
    return Readable(elem, stride) ? elem : nullptr;
}

float Spread(const float* v, size_t n) {
    if (n == 0) return 0.0f;
    float lo = v[0], hi = v[0];
    for (size_t i = 1; i < n; ++i) {
        if (v[i] < lo) lo = v[i];
        if (v[i] > hi) hi = v[i];
    }
    return hi - lo;
}

constexpr size_t kSamples = 180;   // 3 s at 60 Hz, and the loop tolerates a slower tick
constexpr float kMoved = 1.0e-4f;  // the threshold below which a float is "did not move"

DWORD WINAPI SamplerMain(LPVOID) {
    // A settle before the first read. The thread is created from DllMain while the loader is still
    // mapping the rest of plugins\, and a sampler that starts in that window can read a half-built
    // world and would report it as measurement.
    Sleep(2000);

    // ---- 0. Every site this probe calls through, read out of the MAPPED image. ------------------
    // A detour here is executed by us, and a byte mismatch means the whole window is meaningless.
    struct Site { uint64_t va; const uint8_t* want; size_t n; const char* label; };
    const Site kCalled[] = {
        {facts::CS_LOOKUP_BY_SYMBOL, facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE,
         sizeof facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE, "CS_LOOKUP_BY_SYMBOL"},
        {lp::kFindActorComponent, lp::kFindActorComponentPrologue, lp::kGuardLen,
         "LOCAL.FIND_ACTOR_COMPONENT"},
        {lp::kEchoHeadTransform, lp::kEchoHeadTransformPrologue, lp::kGuardLen,
         "LOCAL.ECHOUNIT_HEAD_XF"},
        {lp::kEchoHandTransform, lp::kEchoHandTransformPrologue, lp::kGuardLen,
         "LOCAL.ECHOUNIT_HAND_XF"},
        {lp::kUserActor, lp::kUserActorPrologue, lp::kGuardLen, "VIS.USER_ACTOR"},
    };
    bool sitesOk = true;
    for (const Site& s : kCalled) {
        char what[192];
        _snprintf_s(what, sizeof what, _TRUNCATE, "%s: mapped bytes are the recorded ones (no detour)",
                    s.label);
        const bool ok = PrologueOk(s.va, s.want, s.n);
        Check(ok, what);
        sitesOk = sitesOk && ok;
    }
    // Known-answer control: the same comparison one byte along must REFUSE. Without it a memcmp that
    // always agreed would have passed every line above and reported a green it had not earned.
    {
        const void* p = Mapped(lp::kEchoHeadTransform);
        Check(!Readable(p, lp::kGuardLen + 1) ||
                  memcmp(static_cast<const uint8_t*>(p) + 1, lp::kEchoHeadTransformPrologue,
                         lp::kGuardLen) != 0,
              "the same comparison one byte along refuses (known-answer control)");
    }
    if (!sitesOk) {
        Note(" | the window was NOT sampled: a site this probe calls through did not match, so any "
             "measurement taken through it would be a measurement of somebody else's detour");
        FileVerdict("prologue-guard");
        return 0;
    }

    // ---- 1. The pointer chain to a game space, one link at a time. ------------------------------
    // GAME.PTR_VA is W1-F's, still pending in the W1-F fact drop rather than merged, and is cited
    // through engine/session_facts.h with that provenance. Every link is checked separately so a null
    // reports WHICH link was null rather than "the world was not ready".
    void* gameSlot = Mapped(replay::session_facts::kGamePtrVa);
    void* game = Readable(gameSlot, sizeof(void*)) ? *static_cast<void**>(gameSlot) : nullptr;
    void* netGame = game ? Deref(game, facts::GAME_NETGAME_PTR_OFF) : nullptr;
    // NETGAME.CS_SPACE_OFF, not IDENT.NETGAME_GAMESPACE_OFF: the latter is the CR15Game back-pointer
    // and every call below reaches CS.LOOKUP_BY_SYMBOL, which would index a graphics budget block as
    // a component-system table (ARCH-10 section 1).
    void* gameSpace = netGame ? Deref(netGame, facts::NETGAME_CS_SPACE_OFF) : nullptr;
    if (gameSpace == nullptr) {
        Undetermined(game == nullptr      ? "no CR15Game at GAME.PTR_VA -- the launch never reached a game"
                     : netGame == nullptr ? "CR15Game resolved but its CR15NetGame is null -- no match"
                                          : "CR15NetGame resolved but its game space is null");
        Note(" | ESTABLISHES NOTHING about the arm question: with no game space there is no component "
             "system to read, and a probe that reported PASS here would be reporting its own silence.");
        FileVerdict("game-space");
        return 0;
    }

    auto find = reinterpret_cast<FindCsFn>(Mapped(facts::CS_LOOKUP_BY_SYMBOL));
    auto findComp = reinterpret_cast<FindComponentFn>(Mapped(lp::kFindActorComponent));
    auto headXf = reinterpret_cast<TransformFn>(Mapped(lp::kEchoHeadTransform));
    auto handXf = reinterpret_cast<HandTransformFn>(Mapped(lp::kEchoHandTransform));
    auto userActor = reinterpret_cast<UserActorFn>(Mapped(lp::kUserActor));

    void* bcastCs = find(gameSpace, lp::kSymPlayerBroadcaster);
    void* echoCs = find(gameSpace, lp::kSymEchoUnit);
    void* touchCs = find(gameSpace, lp::kSymTouchAnimBlenders);
    void* modelCs = find(gameSpace, lp::kSymNcaModel);
    Check(bcastCs != nullptr, "CR15PlayerBroadcasterCS resolves by symbol");
    Check(echoCs != nullptr, "CR15EchoUnitCS resolves by symbol");
    if (bcastCs == nullptr || echoCs == nullptr) {
        FileVerdict("component-systems");
        return 0;
    }

    // ---- 2. The broadcaster pool IS the engine's list of locally-authored players. --------------
    uint16_t count = 0;
    Rd<uint16_t>(bcastCs, facts::PLAYER_CS_COUNT_OFF, count);
    Check(count >= 1,
          "the broadcaster pool has at least one entry (it is asserted to be the engine's own list of "
          "players this machine authors; an empty pool in a live match falsifies that)");
    void* elems = Deref(bcastCs, facts::PLAYER_CS_POOL_BASE_OFF);
    void* keys = Deref(bcastCs, lp::kBroadcasterKeysOff);
    if (count == 0 || elems == nullptr || keys == nullptr) {
        Undetermined("the broadcaster pool is empty or its arrays are null, so no entry could be sampled");
        FileVerdict("broadcaster-pool");
        return 0;
    }

    // Entry 0 is the one sampled. On a client that is the local human; on a host it is whichever
    // entry the engine put first, and the slot is reported so the reader is never guessing which.
    const void* actor = static_cast<const uint8_t*>(keys) + lp::kActorGuidSize * 0;
    const void* entry = static_cast<const uint8_t*>(elems) + lp::kBroadcasterCiStride * 0;
    uint32_t userId = 0;
    Rd<uint32_t>(entry, lp::kBroadcasterUserIdOff, userId);
    const uint16_t slot = static_cast<uint16_t>(userId);

    const uint16_t echoH = ComponentOnActor(findComp, echoCs, actor);
    Check(echoH != lp::kNoComponent,
          "CR15EchoUnitCS resolves on the actor the broadcaster pool names (the one step in the "
          "local-pawn chain with no static proof)");
    const uint16_t touchH =
        touchCs ? ComponentOnActor(findComp, touchCs, actor) : lp::kNoComponent;

    // ---- 3. The visibility read, per occupied slot. ---------------------------------------------
    // Without this, "nothing moved" and "nothing was drawn" are one observation.
    if (modelCs == nullptr || userActor == nullptr || netGame == nullptr) {
        Undetermined("CModelCS or CR15NetGame::UserActor did not resolve, so visibility is unknown "
                     "and NO other reading in this run should be taken as evidence that anything was "
                     "or was not rendered");
    } else {
        size_t resolved = 0, visible = 0;
        for (uint16_t s = 0; s < facts::ENTITY_USER_SLOT_TABLE_MAX; ++s) {
            uint8_t slotActor[lp::kActorGuidSize] = {0};
            userActor(netGame, slotActor, s);
            const uint16_t mh = ComponentOnActor(findComp, modelCs, slotActor);
            if (mh == lp::kNoComponent) continue;
            ++resolved;
            void* ci = LeafElement(modelCs, lp::kModelCsHandleMapOff, lp::kModelCsPoolBaseOff,
                                   lp::kModelCiStride, mh);
            uint32_t vis = 0;
            if (ci && Rd<uint32_t>(ci, lp::kModelCiVisibleOff, vis) && vis != 0) ++visible;
        }
        NoteF(" | VISIBILITY: %llu of %llu slots with a CModelCS have the visibility dword set",
              (unsigned long long)visible, (unsigned long long)resolved);
        Check(resolved >= 1,
              "at least one user slot has a CModelCS component (the field EnableRemoteHeads and "
              "EnableRemoteBodies set)");
        // Deliberately NOT a failure when nothing is visible: a spectator mode that hides everything
        // is a legitimate state. The point is that the number is now REPORTED, so a launch where
        // nothing appeared can be told apart from one where nothing ran.
    }

    // ---- 4. THE ARM QUESTION. -------------------------------------------------------------------
    if (echoH == lp::kNoComponent) {
        Undetermined("no CR15EchoUnitCS on the broadcaster entry's actor, so the arm question could "
                     "not be sampled at all -- which is itself a strong hint that ReadLocalPlayer is "
                     "reading the wrong arm for this entry");
    } else {
        float headX[kSamples], handLX[kSamples], handRX[kSamples];
        float trigL[kSamples], trigR[kSamples];
        size_t n = 0;
        for (; n < kSamples; ++n) {
            Transform head{}, hand{};
            headXf(echoCs, &head, echoH);
            headX[n] = head.pos[0] + head.pos[1] + head.pos[2];
            handXf(echoCs, &hand, echoH, 0);
            handLX[n] = hand.pos[0] + hand.pos[1] + hand.pos[2];
            handXf(echoCs, &hand, echoH, 1);
            handRX[n] = hand.pos[0] + hand.pos[1] + hand.pos[2];

            trigL[n] = 0.0f;
            trigR[n] = 0.0f;
            if (touchH != lp::kNoComponent) {
                if (void* ci = LeafElement(touchCs, lp::kCsHandleMapOff, lp::kCsPoolBaseOff,
                                           lp::kTouchAnimCiStride, touchH)) {
                    Rd<float>(ci, lp::kTouchAnimTrigLOff, trigL[n]);
                    Rd<float>(ci, lp::kTouchAnimTrigROff, trigR[n]);
                }
            }
            Sleep(16);
        }

        const float poseSpread =
            Spread(headX, n) + Spread(handLX, n) + Spread(handRX, n);
        const float trigSpread = Spread(trigL, n) + Spread(trigR, n);

        NoteF(" | ARM: slot=%u echoHandle=%u samples=%llu poseSpread=%.6f trigSpread=%.6f",
              unsigned(slot), unsigned(echoH), (unsigned long long)n, poseSpread, trigSpread);

        Check(poseSpread > kMoved,
              "the CR15EchoUnitCS head and hand transforms VARY across the window -- a stationary "
              "player makes a stale read look identical to a live one, so this asserts variance and "
              "not plausibility");

        if (touchH == lp::kNoComponent) {
            Undetermined("no CR15TouchAnimBlendersCS on this actor, so there is no independently-live "
                         "signal to compare the pose against; poseSpread alone cannot separate 'the "
                         "EchoUnit arm is not driven for a human' from 'the wearer held still'");
        } else if (trigSpread <= kMoved && poseSpread <= kMoved) {
            Undetermined("neither the pose nor the triggers moved: the wearer held still, and this "
                         "window says nothing about the arm question. Re-run with movement.");
        } else {
            // The discriminating pair. Triggers are read on the arm the engine uses for a human, so
            // triggers moving while the pose does not is the falsification the arm finding predicts.
            Check(!(trigSpread > kMoved && poseSpread <= kMoved),
                  "the EchoUnit pose moved while the trigger blends did -- if the triggers moved and "
                  "the pose did not, ReadLocalPlayer is reading a component the engine does not drive "
                  "for the local human and every local pose row is wrong");
        }
    }

    Note(" | ESTABLISHES: whether the CR15EchoUnitCS accessors E-4 reads are live for the entry the "
         "broadcaster pool names, judged by variance against an independently-live signal; whether "
         "that pool is non-empty in a match; whether the EchoUnit component resolves on its actor; "
         "and how many user slots have a model flagged visible. DOES NOT ESTABLISH that the EchoUnit "
         "pose EQUALS what the tracking arm would have produced -- that needs the 2192-stride input "
         "pool's symbol, which W1-E has not derived, and a moving pose here does not prove the two "
         "arms agree, only that this one is driven.");
    g.reachedEnd = true;
    FileVerdict("last-check");
    return 0;
}

void PinSelfAndNameIt() {
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&SamplerMain), &g.self);
    char dll[MAX_PATH] = {0};
    GetModuleFileNameA(g.self, dll, sizeof dll);
    const char* leaf = strrchr(dll, '\\');
    leaf = leaf != nullptr ? leaf + 1 : dll;
    strncpy_s(g.stem, sizeof g.stem, leaf, _TRUNCATE);
    if (char* dot = strrchr(g.stem, '.')) *dot = '\0';
}

// ★ The static initialiser. It pins the module, learns its own name and files the PROVISIONAL record.
// It starts no thread and waits for nothing: this runs under the Windows loader lock.
struct EarlyFiling {
    EarlyFiling() {
        PinSelfAndNameIt();
        FileProvisional();
    }
};

const EarlyFiling g_earlyFiling;

}  // namespace

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Created here rather than in the static initialiser: CreateThread under the loader lock is
        // legal only because we never wait on the result, and DllMain is where that contract is
        // visible. The new thread blocks on its own DLL_THREAD_ATTACH until this returns.
        const HANDLE th = CreateThread(nullptr, 0, SamplerMain, nullptr, 0, nullptr);
        if (th != nullptr) CloseHandle(th);
    }
    return TRUE;
}
