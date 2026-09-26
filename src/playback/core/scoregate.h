#pragma once

// DISC-BIND. One entry detour on CR15NetGame::OnScoreCB, sub_140185DF0 (rules, msg) -> 0.
//
// A replayed disc is a real body on the host, and the host's goal detection (CR15FlagCS::Update, a
// swept segment, not role-gated) scores it: delegate_OnFlagBaseScore -> OnScoreCB -> stats, the
// rules block's scorer fields, a reliable send, rules+0xC := 3, IncrementScore -- and one frame
// later the arena script resets the disc and teleports every player. While a replay is loaded the
// callback returns here without reaching the original, so everything upstream runs as recorded and
// nothing downstream learns a goal happened. The original has one exit and it
// returns 0 (xor eax,eax at 0x1401867B9), so 0 is what a swallowed call returns.
//
// Installed once at Init on the worker, only in a contained process, and never removed; what the
// detour does is one atomic mode word.

#include <stddef.h>
#include <stdint.h>

#include "bootstrap/hookmgr.h"

namespace replay {
namespace playback {
namespace scoregate {

inline constexpr char kLedgerKey[] = "playback.host_score_hook";
inline constexpr char kFactId[] = "NETGAME.ON_SCORE_CB";

enum class Mode : uint32_t {
    PassThrough,   // nothing loaded, or not in a match: a load and a branch
    Observe,       // count the host's score callbacks, swallow nothing
    Swallow,
};

using OnScoreFn = int64_t (__fastcall*)(void* rules, void* msg);

// Worker thread, once, at Init. Does nothing unless the ledger key resolved to "hook", which
// bootstrap publishes only for a contained process with the disc drive on.
bool Install();
bool Installed();

// Tick thread, every loaded frame; the playhead is what a swallowed score is logged against.
void Publish(Mode mode, int64_t playheadUs);
Mode CurrentMode();

// ---- the decision -----------------------------------------------------------------------------
//
// ★ SHUT FOR THE WHOLE LOAD, NOT WHILE THE DISC IS THE REPLAY'S. Gating on the disc bind left the
// gate open after every host round_over until the re-bind (~20 s) and at main_loop_enter; a host
// goal there counts, and in overtime it ends the match and returns the host to the lobby
// (DIAG-GAMEMODE-GOALCOLOR A.2). Nothing the host scores while a replay is loaded is a goal anyone
// played, so only the live-safety predicate and the load decide. Pure, so each state is a test.
struct GateState {
    bool contained = false;   // livesafety: this process is contained offline playback
    bool loaded = false;      // a replay is loaded
    bool gate_on = true;      // playback.disc_score_gate
};

struct Decision {
    Mode        mode = Mode::PassThrough;
    const char* why = "";   // program-lifetime; the mode-edge line's reason, compared by pointer
};

Decision Decide(const GateState& s);
const char* ModeName(Mode m);

// Score callbacks since the last drain, and the playhead the newest swallowed one arrived at.
struct Counts {
    uint32_t swallowed = 0;
    uint32_t passed = 0;
    int64_t  lastSwallowedPlayheadUs = 0;
};
void DrainCounts(Counts& out);

// ---- test seams -----------------------------------------------------------------------------------

using HookCreateFn = HookStatus (*)(const char*, uint64_t, const uint8_t*, size_t, void*, void**,
                                    const char*);
using HookArmFn = HookStatus (*)(const char*);
using VerifySiteFn = bool (*)(const char*, uint64_t, const uint8_t*, size_t);
void SetHookAccessForTest(HookCreateFn create, HookArmFn arm, VerifySiteFn verify);

OnScoreFn DetourForTest();
void SetOriginalForTest(OnScoreFn original);
void ResetForTest();

}  // namespace scoregate
}  // namespace playback
}  // namespace replay
