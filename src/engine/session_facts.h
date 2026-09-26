#pragma once

// Mirrors the W1-F fact drop, which W0-2 merges into facts/facts.tsv.  Once that merge has
// happened, configure with -DSESSION_FACTS_MERGED and every value below becomes a static_assert
// against the generated registry instead of a second copy of it.
//
// Nine of this packet's rows were ALREADY in facts/facts.tsv when W1-F derived them independently
// from the shipping bytes, and they agreed.  Those nine are cited from facts:: directly and appear
// nowhere in this file - a fact that exists is never transcribed.  docs/spikes/W1-F.md S9 lists
// the agreement.

#include <stdint.h>

#include "generated/facts_generated.h"

namespace replay::session_facts {

// ---- cited from the registry; NOT copied ---------------------------------------------------
// The offline entry point and its 20-byte guard, the state machine, the NetGame pointer offset,
// the SessionFlags word and its offline bit, and CMatSym::Hash for turning a level or gametype
// name into the u64 the engine stores.
inline constexpr uint64_t kEnterOfflineMatch   = facts::SESSION_CREATE_OFFLINE_MATCH;
inline constexpr uint64_t kEnterOfflineMatchLH = facts::SESSION_CREATE_OFFLINE_MATCH_LH;
inline constexpr uint64_t kStateMachine        = facts::SESSION_STATE_MACHINE;
inline constexpr uint64_t kNetGameInit         = facts::NETGAME_INIT;
inline constexpr uint64_t kMatSymHash          = facts::EVENT_MATSYM_HASH;
inline constexpr uint32_t kGameNetGameOff      = facts::GAME_NETGAME_PTR_OFF;
inline constexpr uint32_t kSessionFlagsOff     = facts::SESSION_OFFLINE_FLAG_OFF;
inline constexpr uint32_t kFlagBitOffline      = facts::SESSION_OFFLINE_FLAG_BIT;
inline constexpr uint32_t kStateInGame         = facts::SESSION_STATE_INGAME;

inline constexpr const uint8_t* kEnterOfflinePrologue = facts::SESSION_CREATE_OFFLINE_MATCH_PROLOGUE;
inline constexpr const uint8_t* kMatSymHashPrologue   = facts::EVENT_MATSYM_HASH_PROLOGUE;
inline constexpr size_t         kPrologueLen          = 20;

// ---- pending in the W1-F fact drop -----------------------------------------------------
inline constexpr uint64_t kGamePtrVa      = 0x1420A0478ull;  // GAME.PTR_VA
inline constexpr uint64_t kBootBranch     = 0x1401088D0ull;  // GAME.BOOT_BRANCH
inline constexpr uint64_t kSpectateCreate = 0x14015A050ull;  // SESSION.SPECTATE_CREATE
inline constexpr uint64_t kBeginMultiplay = 0x1400FDA70ull;  // SESSION.BEGIN_MULTIPLAYER

inline constexpr uint32_t kGameModeFlagsOff   = 0x7AE0u;  // GAME.MODE_FLAGS_OFF
inline constexpr uint32_t kGameLevelHashOff   = 0x0950u;  // GAME.LEVEL_HASH_OFF
inline constexpr uint32_t kGameGametypeOff    = 0x8568u;  // GAME.GAMETYPE_HASH_OFF
inline constexpr uint32_t kGameTeamOff        = 0x8560u;  // GAME.TEAM_OFF
inline constexpr uint32_t kGameHttpPortOff    = 0x855Eu;  // GAME.HTTPPORT_OFF
inline constexpr uint32_t kNetGameStateOff    = 0x0000u;  // NETGAME.STATE_OFF
inline constexpr uint32_t kNetGameGamePtrOff  = 0x0008u;  // NETGAME.GAME_PTR_OFF
inline constexpr uint32_t kGameBootStateOff   = 0x7B14u;  // GAME.BOOT_STATE_OFF

inline constexpr uint64_t kModeBitNoOvr      = 0x08000000ull;  // GAME.MODE_BIT_NOOVR
inline constexpr uint64_t kModeBitBootMp     = 0x02000000ull;  // GAME.MODE_BIT_BOOT_MP
inline constexpr uint64_t kModeBitGamespaces = 0x00000100ull;  // GAME.MODE_BIT_GAMESPACES

inline constexpr uint64_t kFlagBitOnline    = 0x0001ull;  // SESSION.FLAG_BIT_ONLINE
inline constexpr uint64_t kFlagBitHostAlt   = 0x0002ull;  // SESSION.FLAG_BIT_HOST_ALT
inline constexpr uint64_t kFlagBitDedicated = 0x0004ull;  // SESSION.FLAG_BIT_DEDICATED
inline constexpr uint64_t kFlagBitActive    = 0x4000ull;  // SESSION.FLAG_BIT_ACTIVE
// Bit 41. Every CR15NetGame connectivity decision reads `(flags & this) || !connectivityCheck()`
// and takes the no-network branch. Dead in this build: the only writer is a cmov against
// qword_1416CBC70, which is a .rdata constant zero with one reference in the whole image.
inline constexpr uint64_t kFlagBitNoNetwork = 0x20000000000ull;  // SESSION.FLAG_BIT_NO_NETWORK

inline constexpr uint32_t kParamsSize        = 0x48u;  // SESSION.PARAMS_SIZE
inline constexpr uint32_t kParamsGametypeOff = 0x08u;  // SESSION.PARAMS_GAMETYPE_OFF
inline constexpr uint32_t kParamsLevelOff    = 0x10u;  // SESSION.PARAMS_LEVEL_OFF

inline constexpr uint16_t kHttpPortHost    = 6719u;  // SESSION.HTTP_PORT_HOST
inline constexpr uint16_t kHttpPortDefault = 6721u;  // SESSION.HTTP_PORT_DEFAULT

// The command-line chain. Only the two WinMain call sites and the IAT slot are code this packet
// depends on; the rest of the row set exists so an auditor can retrace the argument.
inline constexpr uint64_t kCmdlineLoaderCallVa       = 0x14151D42Eull;  // CMDLINE.LOADER_CALL_VA
inline constexpr uint64_t kCmdlineGetCommandLineVa   = 0x14151D45Cull;  // CMDLINE.GETCMDLINE_CALL_VA
inline constexpr uint64_t kCmdlineIatGetCommandLineA = 0x1416C3340ull;  // CMDLINE.IAT_GETCOMMANDLINEA
inline constexpr uint64_t kCmdlineApply              = 0x140116720ull;  // CMDLINE.APPLY
inline constexpr uint64_t kCmdlineTokenize           = 0x1400D9000ull;  // CMDLINE.TOKENIZE

inline constexpr int32_t kStateLoadingLevel = 7;
inline constexpr int32_t kStateReadyForGame = 8;
inline constexpr int32_t kStateNoNetwork    = -98;

// 20 bytes at each entry point, read out of echovr.exe md5 caa05ee3bca2f5df0809994e93e2cc05.
// Checked before the first call: a wrong image base or a different build stops here instead of
// jumping into the middle of some other function.
inline constexpr uint8_t kSpectateCreatePrologue[kPrologueLen] = {
    0x48, 0x83, 0xEC, 0x78, 0x48, 0x8B, 0x51, 0x08, 0x33, 0xC0,
    0x48, 0x89, 0x44, 0x24, 0x60, 0xC7, 0x44, 0x24, 0x20, 0x00};
inline constexpr uint8_t kBeginMultiplayPrologue[kPrologueLen] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18,
    0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x80, 0x00};
inline constexpr uint8_t kBootBranchPrologue[kPrologueLen] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70,
    0x18, 0x48, 0x89, 0x78, 0x20, 0x55, 0x41, 0x54, 0x41, 0x55};

#ifdef SESSION_FACTS_MERGED
static_assert(kGamePtrVa == facts::GAME_PTR_VA);
static_assert(kBootBranch == facts::GAME_BOOT_BRANCH);
static_assert(kSpectateCreate == facts::SESSION_SPECTATE_CREATE);
static_assert(kBeginMultiplay == facts::SESSION_BEGIN_MULTIPLAYER);
static_assert(kGameModeFlagsOff == facts::GAME_MODE_FLAGS_OFF);
static_assert(kGameLevelHashOff == facts::GAME_LEVEL_HASH_OFF);
static_assert(kGameGametypeOff == facts::GAME_GAMETYPE_HASH_OFF);
static_assert(kGameTeamOff == facts::GAME_TEAM_OFF);
static_assert(kGameHttpPortOff == facts::GAME_HTTPPORT_OFF);
static_assert(kNetGameStateOff == facts::NETGAME_STATE_OFF);
static_assert(kNetGameGamePtrOff == facts::NETGAME_GAME_PTR_OFF);
static_assert(kModeBitNoOvr == facts::GAME_MODE_BIT_NOOVR);
static_assert(kModeBitBootMp == facts::GAME_MODE_BIT_BOOT_MP);
static_assert(kModeBitGamespaces == facts::GAME_MODE_BIT_GAMESPACES);
static_assert(kFlagBitOnline == facts::SESSION_FLAG_BIT_ONLINE);
static_assert(kFlagBitHostAlt == facts::SESSION_FLAG_BIT_HOST_ALT);
static_assert(kFlagBitDedicated == facts::SESSION_FLAG_BIT_DEDICATED);
static_assert(kFlagBitActive == facts::SESSION_FLAG_BIT_ACTIVE);
static_assert(kParamsSize == facts::SESSION_PARAMS_SIZE);
static_assert(kParamsGametypeOff == facts::SESSION_PARAMS_GAMETYPE_OFF);
static_assert(kParamsLevelOff == facts::SESSION_PARAMS_LEVEL_OFF);
static_assert(kHttpPortHost == facts::SESSION_HTTP_PORT_HOST);
static_assert(kHttpPortDefault == facts::SESSION_HTTP_PORT_DEFAULT);
#endif

// The host predicate the engine itself evaluates at NETGAME_HOST_PREDICATE_VA and again at the
// port selection: bit 1, or bit 6 with bit 2 clear.  Declared once here so the probe, the tests
// and the log all ask the same question.
// The offline predicate, which is the host predicate's decisive half: bit 6 set with bit 2 clear.
// Same shape the engine's port selection uses. Pure, so the ruling's authoritative signal can be
// tested exhaustively without a game.
constexpr bool IsOfflineFlags(uint64_t sessionFlags) {
    return (sessionFlags & (kFlagBitDedicated | kFlagBitOffline)) == kFlagBitOffline;
}

constexpr bool IsHostFlags(uint64_t sessionFlags) {
    return (sessionFlags & kFlagBitHostAlt) != 0 ||
           (sessionFlags & (kFlagBitDedicated | kFlagBitOffline)) == kFlagBitOffline;
}

}  // namespace replay::session_facts
