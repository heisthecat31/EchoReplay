// The gametype id the capture records.
//
// OpenParams::gametypeId was declared, read twice (SetLevel and BuildFileName) and assigned
// nowhere, so every file ever written carried manifest.gametypeId == 0 and every capture's
// filename claimed gametype 0 -- the same shape of defect as the levelId one beside it.
//
// The engine's own read, inside SESSION.SPECTATE_CREATE (0x14015A050), is two instructions:
//
//     0x14015A054   mov rdx, [rcx+8]         <- the CR15Game hanging off the CR15NetGame
//     0x14015A06F   mov rax, [rdx+8568h]     <- GAME.GAMETYPE_HASH_OFF, the gametype hash
//
// and it stores what it loaded into SessionParams+0x08 (SESSION.PARAMS_GAMETYPE_OFF). Both
// displacements come out of the registry: the first as facts::IDENT_NETGAME_GAMESPACE_OFF,
// which is the decisive twin of the withheld NETGAME.GAME_PTR_OFF row carrying the same 0x8,
// and the second as facts::GAME_GAMETYPE_HASH_OFF.
//
// The engine's load is a qword and so is this read: a gametype is a whole CSymbol64, and the
// engine compares all 64 bits. The u32 typing of the two offset facts is the width of the OFFSET,
// not of the field behind it. Builds that read four bytes recorded the hash's low dword (or
// 0xFFFFFFFF), which playback resolves (foundation/gametype.h).
//
// On the spectate path the field can still hold all-ones at open. That is recorded as read, and
// the capture replaces it at close with the gametype the lobby's owner-state event stated.
//
// Read at open on the worker thread, beside the level id and the roster scan -- never on the
// armed tick. Every hop is span-proved. A chain that cannot be read records 0 and says which hop
// refused; it never refuses the capture.

#ifndef REPLAY_CAPTURE_GAMETYPEID_H
#define REPLAY_CAPTURE_GAMETYPEID_H

#include <stdint.h>
#include <string.h>

#include "capture/levelid.h"
#include "foundation/gametype.h"
#include "generated/facts_generated.h"

namespace replay {
namespace capture {

// Which hop answered, so a zero gametype is never just "unavailable".
enum class GametypeIdWhy : uint8_t {
    NoNetGame = 0,   // no CR15NetGame -- no session to read a gametype out of
    NoGame,          // the CR15NetGame carries no CR15Game at +0x8 yet
    GameUnreadable,  // the CR15Game is there and its +0x8568 is not committed memory
    Ok,
};

struct GametypeIdRead {
    uint64_t id = 0;
    GametypeIdWhy why = GametypeIdWhy::NoNetGame;
    void* game = nullptr;
};

inline const char* GametypeIdWhyText(GametypeIdWhy w) {
    switch (w) {
        case GametypeIdWhy::NoNetGame:      return "no CR15NetGame -- no session is loaded";
        case GametypeIdWhy::NoGame:         return "the CR15NetGame carries no CR15Game yet";
        case GametypeIdWhy::GameUnreadable: return "the CR15Game's gametype hash is not readable";
        case GametypeIdWhy::Ok:             return "read";
    }
    return "unstated";
}

// The two hops off the CR15NetGame, named here rather than spelled at the use site.
inline constexpr uint32_t kGamePtrOff = facts::IDENT_NETGAME_GAMESPACE_OFF;
inline constexpr uint32_t kGametypeHashOff = facts::GAME_GAMETYPE_HASH_OFF;

// The walk. Pure: everything it touches arrives in its arguments.
inline GametypeIdRead ReadGametypeIdFrom(void* netGame) {
    GametypeIdRead r;
    if (!netGame) return r;

    void* const game = DerefGuarded(netGame, kGamePtrOff);
    if (!game) {
        r.why = GametypeIdWhy::NoGame;
        return r;
    }
    r.game = game;

    const uint8_t* const hashAt = static_cast<const uint8_t*>(game) + kGametypeHashOff;
#if defined(SHIP_MUTATE_GAMETYPE_LOW_DWORD)
    uint32_t hash = 0;
#else
    uint64_t hash = 0;
#endif
    if (!Readable(hashAt, sizeof hash)) {
        r.why = GametypeIdWhy::GameUnreadable;
        return r;
    }
    memcpy(&hash, hashAt, sizeof hash);
    r.id = hash;
    r.why = GametypeIdWhy::Ok;
    return r;
}

// What the open path calls: fills `out` with the gametype, or leaves it 0 and hands back the hop
// that refused so the caller can say so in one line. Returns true when `out` carries a real id.
inline bool FillGametypeId(uint64_t& out, void* netGame, GametypeIdRead& r) {
    r = ReadGametypeIdFrom(netGame);
    if (r.why != GametypeIdWhy::Ok) return false;
#if !defined(W2A_MUTATE_NO_GAMETYPEID)
    // Mutation control: dropping this one assignment is exactly the pre-fix state -- every file on
    // disk carrying manifest.gametypeId == 0 with nothing else looking wrong.
    out = r.id;
#endif
    return true;
}

// The gametype to file at close in place of the one read at open, or 0 to keep it. A whole
// gametype read at open is the engine's own word and stands; anything else yields to what the
// lobby's owner-state event stated.
inline uint64_t LateGametype(uint64_t readAtOpen, uint64_t ownerStateGametype) {
    if (gametype::IsFull(readAtOpen) || !gametype::IsFull(ownerStateGametype)) return 0;
    return ownerStateGametype;
}

}  // namespace capture
}  // namespace replay

#endif  // REPLAY_CAPTURE_GAMETYPEID_H
