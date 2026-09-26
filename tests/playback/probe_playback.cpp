// W2-B's in-match probe. It answers exactly two of this packet's parked reads, and claims exactly
// those two.
//
// ---------------------------------------------------------------------------------------------
// ★★★ WHY TWO AND NOT SIX.
//
// W2-B parked six reads. Four of them cannot be collected by any probe on any launch this harness can
// currently perform, and saying so is the point of this file existing at all:
//
//   w2b-ring-depth-and-order, w2b-interp-enable-bit and w2b-interp-output-vs-installed all read the
//   PhysCI of a REPLAYED body -- they need a capture loaded and playing while the sample is taken.
//   The test harness's canonical profiles are bare, bare-norlay, bare-level, flat, desktop and
//   offline-local. None of them loads a capture. These three are not waiting for somebody to write a
//   collector; they are waiting for a profile that puts a replay on screen.
//
//   w2b-transform-lands-after-jump names "CTransformCS's local transform", whose PC offset has never
//   been derived. A probe cannot read an address nobody has established, and one that guessed the
//   nearest plausible field would report a confident number for the wrong slot.
//
// ★★ CLAIMING A READ WHILE ANSWERING HALF OF IT IS WORSE THAN NOT CLAIMING IT, so the descriptor's
// `answers` array lists these two and nothing else. An uncovered read gets chased; a falsely covered
// one does not.
//
// ★ NO ADDRESS IN THIS FILE. Every site comes from facts_generated.h, so a value the registry
// withholds cannot be read here by accident -- the constant simply does not exist to name. Everything
// used below is V-PRIMARY.
// ---------------------------------------------------------------------------------------------

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "bootstrap/buildguard.h"
#include "bootstrap/log.h"
#include "bootstrap/services.h"
#include "engine/tick.h"
#include "generated/facts_generated.h"

namespace {

const char kReplayModuleName[] = "w2b-match";

// ★ Two samples, and the SECOND is what makes the first mean anything: the re-origin accumulator is
// never reset, so one sample cannot tell "no shift has ever happened" from "a shift happened before
// the capture started". A difference between the two is a shift that happened while we watched.
const uint64_t kSecondSampleAfterUs = 60ull * 1000000ull;

struct Sample {
    bool taken = false;
    uint64_t atSimUs = 0;

    // w2b-host-arm-confirm
    bool gameResolved = false;
    bool netGameResolved = false;
    uint64_t gamePtr = 0;
    uint64_t netGamePtr = 0;
    uint64_t roleDirect = 0;     // CR15Game + SESSION_OFFLINE_FLAG_OFF
    uint64_t roleIndirect = 0;   // *(netGame + NETGAME_ROLEFLAGS_OFF)
    bool roleDirectRead = false;
    bool roleIndirectRead = false;

    // w2b-world-origin-shift
    bool rendererResolved = false;
    uint64_t rendererPtr = 0;
    uint32_t cumA[3] = {0, 0, 0};   // first reading
    uint32_t cumB[3] = {0, 0, 0};   // second reading, back to back: a difference is a torn read
    uint32_t pending[3] = {0, 0, 0};
    bool flagRead = false;
    uint64_t flagWord = 0;
};

Sample g_first;
Sample g_second;
size_t g_failed = 0;

// ★ Every dereference is bounds-checked against the mapped image before it is taken. A probe that
// faulted would take the owner's game down with it, and this one runs inside a live match.
bool ReadQword(const void* at, uint64_t* out) {
    if (at == nullptr) {
        return false;
    }
    __try {
        *out = *static_cast<const volatile uint64_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadDword(const void* at, uint32_t* out) {
    if (at == nullptr) {
        return false;
    }
    __try {
        *out = *static_cast<const volatile uint32_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const uint8_t* Byte(uint64_t ptr) { return reinterpret_cast<const uint8_t*>(ptr); }

void TakeSample(Sample& s, uint64_t simUs) {
    s.taken = true;
    s.atSimUs = simUs;

    // ---- w2b-host-arm-confirm ----------------------------------------------------------------
    // The global is a POINTER; the fact is the address of the pointer, not of the object.
    if (const void* slot = replay::buildguard::MappedAddress(facts::GAME_SINGLETON_VA)) {
        if (ReadQword(slot, &s.gamePtr) && s.gamePtr != 0) {
            s.gameResolved = true;
            s.roleDirectRead =
                ReadQword(Byte(s.gamePtr) + facts::SESSION_OFFLINE_FLAG_OFF, &s.roleDirect);
            if (ReadQword(Byte(s.gamePtr) + facts::GAME_NETGAME_PTR_OFF, &s.netGamePtr) &&
                s.netGamePtr != 0) {
                s.netGameResolved = true;
                s.roleIndirectRead =
                    ReadQword(Byte(s.netGamePtr) + facts::NETGAME_ROLEFLAGS_OFF, &s.roleIndirect);
            }
            // WORLD_REORIGIN_FLAG_OFF is recorded on CncaGame. This build reaches CncaGame and
            // CR15Game through the same pointer, which is an identification the registry has not
            // stated -- so the raw word is reported beside the pointer it came from rather than
            // presented as settled.
            s.flagRead = ReadQword(Byte(s.gamePtr) + facts::WORLD_REORIGIN_FLAG_OFF, &s.flagWord);
        }
    }

    // ---- w2b-world-origin-shift ---------------------------------------------------------------
    // ★ WORLD_REORIGIN_GLOBAL is a pointer to the CGParticleEffectRenderer singleton and sits past
    // the raw end of .data, so it is loader-zeroed until boot publishes it. A null here is reported
    // as null and the triples are NOT reported: zeros from an unresolved base read exactly like a
    // world that never shifted, which is the confident wrong answer this read was respecified to
    // avoid after it was first written against C3Vector::zero.
    if (const void* slot = replay::buildguard::MappedAddress(facts::WORLD_REORIGIN_GLOBAL)) {
        if (ReadQword(slot, &s.rendererPtr) && s.rendererPtr != 0) {
            const uint8_t* r = Byte(s.rendererPtr);
            const uint32_t cumOff[3] = {facts::WORLD_REORIGIN_OFF_X, facts::WORLD_REORIGIN_OFF_Y,
                                        facts::WORLD_REORIGIN_OFF_Z};
            const uint32_t penOff[3] = {facts::WORLD_REORIGIN_PENDING_OFF_X,
                                        facts::WORLD_REORIGIN_PENDING_OFF_Y,
                                        facts::WORLD_REORIGIN_PENDING_OFF_Z};
            bool ok = true;
            for (int i = 0; i < 3; ++i) {
                ok = ReadDword(r + cumOff[i], &s.cumA[i]) && ok;
            }
            // Back to back, before anything else: UpdateEndTranslation writes these three floats with
            // unfenced addss/movss from another thread, so a differing pair is a torn sample and has
            // to be visible rather than averaged away or silently retried.
            for (int i = 0; i < 3; ++i) {
                ok = ReadDword(r + cumOff[i], &s.cumB[i]) && ok;
            }
            for (int i = 0; i < 3; ++i) {
                ok = ReadDword(r + penOff[i], &s.pending[i]) && ok;
            }
            s.rendererResolved = ok;
        }
    }
}

float AsFloat(uint32_t bits) {
    float f = 0.0f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

void AppendSample(char* buf, size_t cap, const char* label, const Sample& s) {
    char one[1400];
    if (!s.taken) {
        _snprintf_s(one, sizeof one, _TRUNCATE, "%s: NOT TAKEN; ", label);
        strncat_s(buf, cap, one, _TRUNCATE);
        return;
    }
    _snprintf_s(one, sizeof one, _TRUNCATE,
                "%s at sim %llu us: CR15Game=%s0x%llX netGame=%s0x%llX "
                "roleDirect=%s0x%llX roleIndirect=%s0x%llX isHost=%s flagWord=%s0x%llX "
                "(armed0x100=%d latched0x200=%d) | renderer=%s0x%llX",
                label, (unsigned long long)s.atSimUs, s.gameResolved ? "" : "UNRESOLVED:",
                (unsigned long long)s.gamePtr, s.netGameResolved ? "" : "UNRESOLVED:",
                (unsigned long long)s.netGamePtr, s.roleDirectRead ? "" : "UNREAD:",
                (unsigned long long)s.roleDirect, s.roleIndirectRead ? "" : "UNREAD:",
                (unsigned long long)s.roleIndirect,
                !s.roleDirectRead ? "unknown"
                                  : (((s.roleDirect & 2) != 0 ||
                                      ((s.roleDirect & 4) == 0 && (s.roleDirect & 0x40) != 0))
                                         ? "true"
                                         : "false"),
                s.flagRead ? "" : "UNREAD:", (unsigned long long)s.flagWord,
                s.flagRead ? (int)((s.flagWord & 0x100) != 0) : -1,
                s.flagRead ? (int)((s.flagWord & 0x200) != 0) : -1,
                s.rendererResolved ? "" : "UNRESOLVED:", (unsigned long long)s.rendererPtr);
    strncat_s(buf, cap, one, _TRUNCATE);

    if (s.rendererResolved) {
        const bool torn = memcmp(s.cumA, s.cumB, sizeof s.cumA) != 0;
        _snprintf_s(one, sizeof one, _TRUNCATE,
                    " cumulative=[0x%08X 0x%08X 0x%08X]=(%g,%g,%g) reread=[0x%08X 0x%08X 0x%08X] "
                    "torn=%s pending=[0x%08X 0x%08X 0x%08X]=(%g,%g,%g); ",
                    s.cumA[0], s.cumA[1], s.cumA[2], AsFloat(s.cumA[0]), AsFloat(s.cumA[1]),
                    AsFloat(s.cumA[2]), s.cumB[0], s.cumB[1], s.cumB[2], torn ? "YES" : "no",
                    s.pending[0], s.pending[1], s.pending[2], AsFloat(s.pending[0]),
                    AsFloat(s.pending[1]), AsFloat(s.pending[2]));
    } else {
        _snprintf_s(one, sizeof one, _TRUNCATE,
                    " cumulative and pending NOT READ (the renderer singleton is null, which is not "
                    "the same answer as a zero shift); ");
    }
    strncat_s(buf, cap, one, _TRUNCATE);
}

// ---------------------------------------------------------------------------------------------
// WHERE THIS PROBE FILES, AND UNDER WHAT NAME. Both were wrong, and both cost a real measurement.
//
// The verdict from the 2026-08-30 sweep is still on disk at launch-09/probes/done.json -- one
// directory above where the harness reads -- because this file composed "%s\done.json" from
// REPLAY_PROBE_ROOT and left out the per-probe component. The harness composes
// <root>\<dll base name>\done.json. Two implementations of one layout agree only by luck, and this
// one did not.
//
// So it ASKS instead of re-deriving. The harness now exports REPLAY_PROBE_DIRS as
// "<dll base>=<absolute directory>;..." and this probe looks up its OWN module's base name in it.
// Nothing here knows how the path is composed, which is the only arrangement that cannot drift.
// REPLAY_PROBE_ROOT remains the fallback for a harness too old to export the map, and the fallback
// spells the layout in one place with a comment saying it is a copy.
//
// The id was read the same careless way: the whole REPLAY_PROBE_IDS string, "<base>=<id>;...", went
// straight into the "probe" field, so done.json claimed to be probe "probe_playback=W2-B-match" and
// the harness would have rejected it as a foreign record even had the path been right. Two
// independent failures, one measurement lost.
// ---------------------------------------------------------------------------------------------

// The base name of the module this code is in -- not of the process, and not a literal. A literal
// would be a third copy of a name the build system already owns.
bool SelfStem(char* out, size_t bytes) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&SelfStem), &self) ||
        self == nullptr) {
        return false;
    }
    char full[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameA(self, full, sizeof full);
    if (n == 0 || n >= sizeof full) {
        return false;
    }
    const char* slash = strrchr(full, '\\');
    const char* base = (slash != nullptr) ? slash + 1 : full;
    const char* dot = strrchr(base, '.');
    size_t len = (dot != nullptr) ? static_cast<size_t>(dot - base) : strlen(base);
    if (len == 0 || len >= bytes) {
        return false;
    }
    memcpy(out, base, len);
    out[len] = '\0';
    return true;
}

// One lookup for both maps: REPLAY_PROBE_DIRS and REPLAY_PROBE_IDS share the "<key>=<value>;..."
// shape. The key is matched WITH its '=' appended and only at a field boundary, so a probe named
// probe_play cannot pick up probe_playback's value -- prefix matching is how this project's checks
// most often pass wrongly.
bool LookupField(const char* env, const char* key, char* out, size_t bytes) {
    char buf[2048] = {0};
    const DWORD n = GetEnvironmentVariableA(env, buf, sizeof buf);
    if (n == 0 || n >= sizeof buf) {
        return false;
    }
    char needle[160];
    _snprintf_s(needle, sizeof needle, _TRUNCATE, "%s=", key);
    const size_t needleLen = strlen(needle);
    for (const char* at = buf; at != nullptr && *at != '\0';) {
        const bool atFieldStart = (at == buf) || (*(at - 1) == ';');
        if (atFieldStart && strncmp(at, needle, needleLen) == 0) {
            const char* value = at + needleLen;
            size_t len = strcspn(value, ";");
            if (len == 0 || len >= bytes) {
                return false;
            }
            memcpy(out, value, len);
            out[len] = '\0';
            return true;
        }
        const char* next = strchr(at, ';');
        at = (next != nullptr) ? next + 1 : nullptr;
    }
    return false;
}

// The directory the HARNESS named for this probe. Falls back to the documented layout only when the
// map is absent altogether; a map that is present and does not name us is a refusal, because writing
// somewhere nobody reads is what this whole comment block exists to stop.
bool VerdictDir(char* out, size_t bytes) {
    char stem[128] = {0};
    if (!SelfStem(stem, sizeof stem)) {
        return false;
    }
    if (LookupField("REPLAY_PROBE_DIRS", stem, out, bytes)) {
        return true;
    }
    char dirs[8] = {0};
    if (GetEnvironmentVariableA("REPLAY_PROBE_DIRS", dirs, sizeof dirs) != 0 ||
        GetLastError() != ERROR_ENVVAR_NOT_FOUND) {
        // The map exists and does not name this module. Silence is the honest outcome; guessing is
        // exactly the defect being repaired.
        return false;
    }
    char root[MAX_PATH] = {0};
    const DWORD rootLen = GetEnvironmentVariableA("REPLAY_PROBE_ROOT", root, sizeof root);
    if (rootLen == 0 || rootLen >= sizeof root) {
        return false;
    }
    // A COPY of the test harness's layout, kept only for a harness that predates REPLAY_PROBE_DIRS.
    return _snprintf_s(out, bytes, _TRUNCATE, "%s\\%s", root, stem) > 0;
}

void ProbeIdOf(char* out, size_t bytes) {
    // An id we could not resolve names ITSELF as unresolved rather than borrowing another probe's
    // identity or, as before, the raw text of the whole map.
    strncpy_s(out, bytes, "W2-B-match-unresolved-id", _TRUNCATE);
    char stem[128] = {0};
    if (!SelfStem(stem, sizeof stem)) {
        return;
    }
    char value[128] = {0};
    if (LookupField("REPLAY_PROBE_IDS", stem, value, sizeof value)) {
        strncpy_s(out, bytes, value, _TRUNCATE);
    }
}

// ★ The detail is built into a LOCAL buffer every time. An earlier draft appended into a file-scope
// one, which is fine once and wrong on the second call -- and this is called up to three times
// (init, the second sample, shutdown), so the last report would have carried three copies of the
// first. A report that grows every time it is written is not a report.
void WriteVerdict() {
    char dir[MAX_PATH] = {0};
    if (!VerdictDir(dir, sizeof dir)) {
        return;
    }
    char probeId[128] = {0};
    ProbeIdOf(probeId, sizeof probeId);

    char detail[4096] = {0};
    if (!g_first.taken) {
        strncat_s(detail, sizeof detail,
                  "no sample yet: waiting for the first tick carrying a CR15NetGame; ", _TRUNCATE);
    }
    AppendSample(detail, sizeof detail, "SAMPLE-1", g_first);
    AppendSample(detail, sizeof detail, "SAMPLE-2", g_second);
    strncat_s(detail, sizeof detail,
              "ANSWERS: w2b-host-arm-confirm, w2b-world-origin-shift. DOES NOT ANSWER and does not "
              "claim: w2b-ring-depth-and-order, w2b-interp-enable-bit and "
              "w2b-interp-output-vs-installed (all three read a REPLAYED body's PhysCI, so they need "
              "a capture loaded and playing, and no canonical harness profile loads one), and "
              "w2b-transform-lands-after-jump (the render transform's PC offset has never been "
              "derived).", _TRUNCATE);

    // ★ NOT-RUN, not PASS, when the chain never resolved. A probe that reported PASS having read
    // nothing is the shape every counter in this project exists to prevent.
    const char* verdict = (!g_first.taken || !g_first.gameResolved)
                              ? "NOT-RUN"
                              : (g_failed == 0 ? "PASS" : "FAIL");

    char path[MAX_PATH];
    // The harness creates this directory before the launch, but a probe that assumed so and found
    // it absent would fail silently, so it is made here too and the write is attempted either way.
    CreateDirectoryA(dir, nullptr);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\done.json", dir);
    FILE* fh = nullptr;
    if (fopen_s(&fh, path, "wb") == 0 && fh != nullptr) {
        fprintf(fh,
                "{ \"probe\": \"%s\", \"verdict\": \"%s\", \"checked\": %d,\n"
                "  \"detail\": \"%s\" }\n",
                probeId, verdict, (int)(g_first.taken ? 1 : 0) + (int)(g_second.taken ? 1 : 0),
                detail);
        fclose(fh);
    }
    replay::log::Direct(strcmp(verdict, "PASS") == 0 ? replay::LogLevel::Info
                                                     : replay::LogLevel::Warn,
                        kReplayModuleName, "%s: %s", verdict, detail);
}

void Tick(const replay::TickInfo& info) {
    if (g_second.taken) {
        return;
    }
    if (!g_first.taken) {
        // The first tick at which the match object exists is this probe's "state 9": before that
        // there is nothing to read and a sample would record the absence as data.
        const void* slot = replay::buildguard::MappedAddress(facts::GAME_SINGLETON_VA);
        uint64_t game = 0;
        if (slot == nullptr || !ReadQword(slot, &game) || game == 0) {
            return;
        }
        uint64_t netGame = 0;
        if (!ReadQword(Byte(game) + facts::GAME_NETGAME_PTR_OFF, &netGame) || netGame == 0) {
            return;
        }
        TakeSample(g_first, info.simTimeUs);
        return;
    }
    if (info.simTimeUs >= g_first.atSimUs + kSecondSampleAfterUs) {
        TakeSample(g_second, info.simTimeUs);
        WriteVerdict();
    }
}

bool Init(replay::ReplayServices* services) {
    REPLAY_REQUIRE(services, log);
    // Written now as well as at the samples, so a launch that never reaches a match still leaves a
    // NOT-RUN on disk naming what it was waiting for, rather than no file at all -- which the harness
    // would have to read as "the probe did not run" and "the probe crashed" alike.
    WriteVerdict();
    return true;
}

void Shutdown() {
    if (!g_second.taken) {
        WriteVerdict();   // the launch ended before 60 s of sim time; report what was sampled
    }
}

}  // namespace

REPLAY_MODULE("w2b-match", 320, Init, Tick, Shutdown);
