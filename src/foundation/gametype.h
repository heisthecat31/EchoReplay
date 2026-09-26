// Which gametype a capture should enter with.
//
// A gametype is a CSymbol64 of its name: a byte-wise CRC-64 over the lower-cased name, seed
// 0x7F536CD653B4364A, table row i = carry-less i * (0x95AC9329AC4BC9B5 << 1). The engine compares
// the full qword. Captures written before the recorder kept all 64 bits hold either the low dword
// of that hash or, on the spectate path, all-ones -- neither of which the engine recognizes.

#ifndef REPLAY_FOUNDATION_GAMETYPE_H
#define REPLAY_FOUNDATION_GAMETYPE_H

#include <stddef.h>
#include <stdint.h>

namespace replay {
namespace gametype {

inline constexpr uint64_t kSymbolSeed = 0x7F536CD653B4364Aull;
inline constexpr uint64_t kSymbolPoly = 0x95AC9329AC4BC9B5ull;

constexpr uint64_t SymbolTableRow(uint8_t index) {
    uint64_t row = 0;
    for (int bit = 0; bit < 8; ++bit) {
        if ((index >> bit) & 1) row ^= kSymbolPoly << (bit + 1);
    }
    return row;
}

constexpr uint64_t SymbolHash(const char* name) {
    uint64_t hash = kSymbolSeed;
    for (; *name != '\0'; ++name) {
        const char c = (*name >= 'A' && *name <= 'Z') ? static_cast<char>(*name + 32) : *name;
        hash = static_cast<uint8_t>(c) ^ (hash << 8) ^ SymbolTableRow(static_cast<uint8_t>(hash >> 56));
    }
    return hash;
}

struct Known {
    const char* name;
    uint64_t    id;
};

// Every echo_arena* / echo_combat* / social_2.0* name in the shipped binaries' symbol census.
inline constexpr Known kKnown[] = {
    {"echo_arena", SymbolHash("echo_arena")},
    {"echo_arena_private", SymbolHash("echo_arena_private")},
    {"echo_arena_practice_ai", SymbolHash("echo_arena_practice_ai")},
    {"echo_arena_private_ai", SymbolHash("echo_arena_private_ai")},
    {"echo_arena_public_ai", SymbolHash("echo_arena_public_ai")},
    {"echo_arena_first_match", SymbolHash("echo_arena_first_match")},
    {"echo_arena_npe", SymbolHash("echo_arena_npe")},
    {"echo_arena_tournament", SymbolHash("echo_arena_tournament")},
    {"echo_combat", SymbolHash("echo_combat")},
    {"echo_combat_private", SymbolHash("echo_combat_private")},
    {"echo_combat_practice_ai", SymbolHash("echo_combat_practice_ai")},
    {"echo_combat_private_ai", SymbolHash("echo_combat_private_ai")},
    {"echo_combat_public_ai", SymbolHash("echo_combat_public_ai")},
    {"echo_combat_first_match", SymbolHash("echo_combat_first_match")},
    {"echo_combat_tournament", SymbolHash("echo_combat_tournament")},
    {"social_2.0", SymbolHash("social_2.0")},
    {"social_2.0_private", SymbolHash("social_2.0_private")},
    {"social_2.0_npe", SymbolHash("social_2.0_npe")},
};

// Pinned against values read out of the engine, not out of SymbolHash: echo_arena is the gametype
// the real capture's SNSLobbyOwnerState states, the rest are the image's own name->hash pairs.
static_assert(kKnown[0].id == 0xCB60A4DE7E1CAF73ull, "echo_arena");
static_assert(kKnown[1].id == 0x09990965F4DB8C03ull, "echo_arena_private");
static_assert(kKnown[8].id == 0x3D5C3976578A321Aull, "echo_combat");
static_assert(kKnown[9].id == 0x33BBF6842DF97A3Full, "echo_combat_private");
static_assert(kKnown[15].id == 0x042D9CF9CFDDCF76ull, "social_2.0");

inline const char* NameOf(uint64_t id) {
    for (const Known& k : kKnown) {
        if (k.id == id) return k.name;
    }
    return nullptr;
}

// The pooled player-actor prefabs, so a class id read out of the level's own name table can be
// printed as a name and so the admission can ask the right pool how many actors are left.
// `remoteplayer_arena` is pinned against the id every captured game log shows CR15NetGame
// allocating ("[NETGAME] Player Actor Allocating '0xEF05DD95418C9973'").
inline constexpr Known kActorPrefabs[] = {
    {"remoteplayer_arena", SymbolHash("remoteplayer_arena")},
    {"remoteplayer_combat", SymbolHash("remoteplayer_combat")},
    {"player_arena", SymbolHash("player_arena")},
    {"player_combat", SymbolHash("player_combat")},
    {"bot_arena", SymbolHash("bot_arena")},
    {"bot_combat", SymbolHash("bot_combat")},
};
static_assert(kActorPrefabs[0].id == 0xEF05DD95418C9973ull, "remoteplayer_arena");

inline constexpr uint64_t kRemotePlayerArenaPrefab = kActorPrefabs[0].id;

inline const char* NameOfActorPrefab(uint64_t id) {
    for (const Known& k : kActorPrefabs) {
        if (k.id == id) return k.name;
    }
    return nullptr;
}

// A value the engine could have produced as a whole qword: not empty, not the all-ones the field
// holds before the lobby fills it, and not a widened 32-bit read.
constexpr bool IsFull(uint64_t id) {
    return id != ~0ull && (id >> 32) != 0;
}

enum class Rule : uint8_t {
    Manifest64,   // the recorded value is already a whole gametype
    LowDword,     // the recorded 32 bits are the low half of a known gametype
    OwnerState,   // the capture's own SNSLobbyOwnerState event states it
    Fallback,     // none of the above; the configured fallback name
    Unresolved,   // nothing recorded and no fallback: the engine's own gametype stands
};

inline const char* RuleName(Rule rule) {
    switch (rule) {
        case Rule::Manifest64: return "manifest-64bit";
        case Rule::LowDword:   return "low-dword-match";
        case Rule::OwnerState: return "owner-state-event";
        case Rule::Fallback:   return "fallback-name";
        case Rule::Unresolved: return "unresolved";
    }
    return "unknown";
}

struct Resolved {
    uint64_t id;
    Rule     rule;
};

// Two known gametypes sharing a low dword would make the match ambiguous; then it is no match.
inline uint64_t MatchLowDword(uint32_t low) {
    uint64_t found = 0;
    for (const Known& k : kKnown) {
        if (static_cast<uint32_t>(k.id) != low) continue;
        if (found != 0) return 0;
        found = k.id;
    }
    return found;
}

// `ownerState` is the gametype out of the capture's SNSLobbyOwnerState event, 0 when it has none.
// `fallback` is the configured fallback gametype, 0 when the fallback is off or did not resolve.
inline Resolved Resolve(uint64_t recorded, uint64_t ownerState, uint64_t fallback) {
    if (IsFull(recorded)) return {recorded, Rule::Manifest64};
    if (recorded != 0 && (recorded >> 32) == 0) {
        const uint64_t matched = MatchLowDword(static_cast<uint32_t>(recorded));
        if (matched != 0) return {matched, Rule::LowDword};
    }
    if (IsFull(ownerState)) return {ownerState, Rule::OwnerState};
    // A capture that recorded nothing at all never asked for a gametype; an old one that recorded
    // an unusable value did.
    if (recorded != 0 && fallback != 0) return {fallback, Rule::Fallback};
    return {recorded, Rule::Unresolved};
}

// Whether Resolve can settle `recorded` without the capture's events, so a caller can skip reading them.
inline bool NeedsOwnerState(uint64_t recorded) {
    return Resolve(recorded, 0, 0).rule == Rule::Unresolved;
}

// The "gametype" member of an SNSLobbyOwnerState JSON body. The engine prints the hash as a signed
// decimal, so -3791849610740453517 is 0xCB60A4DE7E1CAF73. `json` need not be terminated.
inline bool ParseOwnerStateGametype(const char* json, size_t bytes, uint64_t* out) {
    static constexpr char kKey[] = "\"gametype\"";
    constexpr size_t kKeyLen = sizeof(kKey) - 1;
    if (json == nullptr || out == nullptr) return false;
    for (size_t at = 0; at + kKeyLen <= bytes; ++at) {
        size_t i = 0;
        while (i < kKeyLen && json[at + i] == kKey[i]) ++i;
        if (i != kKeyLen) continue;

        size_t p = at + kKeyLen;
        while (p < bytes && (json[p] == ' ' || json[p] == '\t')) ++p;
        if (p >= bytes || json[p] != ':') return false;
        ++p;
        while (p < bytes && (json[p] == ' ' || json[p] == '\t')) ++p;
        const bool negative = p < bytes && json[p] == '-';
        if (negative) ++p;

        uint64_t magnitude = 0;
        size_t digits = 0;
        for (; p < bytes && json[p] >= '0' && json[p] <= '9'; ++p, ++digits) {
            const uint64_t digit = static_cast<uint64_t>(json[p] - '0');
            if (magnitude > (~0ull - digit) / 10) return false;
            magnitude = magnitude * 10 + digit;
        }
        if (digits == 0) return false;
        *out = negative ? (0 - magnitude) : magnitude;
        return true;
    }
    return false;
}

}  // namespace gametype
}  // namespace replay

#endif  // REPLAY_FOUNDATION_GAMETYPE_H
