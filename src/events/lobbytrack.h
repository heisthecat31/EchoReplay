#pragma once

// W3-E. The entrant roster, and S5.7's JOIN / LEAVE / TEAM_CHANGE markers derived from it.
//
// ---------------------------------------------------------------------------------------------
// ★ THE MARKERS IN A FILE STAY REPRODUCIBLE FROM THE FILE. markers.h states that rule for the six
// netvar-derived kinds and this keeps it for the three lobby-derived ones: every input is an S_EVENT
// body the capture already stores verbatim, so a reader that disagrees with our table can recompute
// it and see which of us is wrong. Nothing here observes engine state.
//
// ★★ THE ROSTER IS KEYED ON THE XPlatformId, NOT ON THE ENTRANT INDEX. The index is a lobby SLOT --
// the host re-stamps it into every forwarded copy of a body, and libr15's EntrantRemovedCB frees a
// slot in place without compacting, so the slot is reused by the next join. Keying on it would let a
// newcomer inherit the departed player's name, team and loadout. The account is the identity; the
// slot is where the identity is currently sitting, and both are stored.
//
// ★ A FIRST SIGHTING IS A JOIN HERE, WHICH IS THE OPPOSITE OF markers.h's NETVAR RULE, and the
// difference is real rather than an inconsistency. A netvar's first value tells you nothing about when
// it was set, so treating it as an edge invents events. An SNSLobbyEntrantStatev2 IS the entrant
// asserting presence, and the host forwards every existing entrant's copy to a joiner -- so the first
// one we see is the moment that entrant entered OUR view, which is exactly what a replay timeline
// wants to show. A capture that starts mid-match therefore opens with a JOIN per present player, and
// that is the honest reading, not an artefact.
//
// ★ AND THE CLIENT RE-SENDS ITS OWN STATE ABOUT EVERY 20 ms. Emitting a marker per body would bury
// the timeline in thousands of duplicate JOINs, so every kind here is edge-triggered against stored
// state and Stats reports how many bodies were seen against how many markers came out. A ratio of one
// is the bug this note exists to make visible.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

#include "capture/markers.h"  // MarkKind and MarkRec: ONE definition of the S_MARK numbering
#include "events/lobbymsg.h"

namespace replay {
namespace events {

// What a tap-table row means to this tracker. Resolved from the message's STRUCT NAME rather than from
// its index in W2-A's table: an index is positional, and inserting a row above would silently re-aim
// every one of these without changing a line of this file.
enum class LobbyKind : uint8_t {
    Other = 0,
    EntrantState,
    EntrantRemoved,
    EntrantTeamChanged,
    OwnerState,
    NetIds,
    ProfileFromServer,
    StatUpdate,
    TeamStatUpdate,
};
LobbyKind LobbyKindForName(const char* structName);

// One entrant. `displayName` is 37 bytes because the engine's field is 36 and NUL-termination is ours:
// libr15's EntrantStateCB compares it as a 36-byte CSysString, so a 36-character name is legal and
// carries no terminator on the wire.
//
// ★ Appendix B.0's PlayerRecord declares displayName as char[32] and platformStr as char[32]. Both are
// NARROWER THAN THE ENGINE FIELD, which is INV-W1 (S5.4.1a) -- the same defect class as the alignment
// byte, arriving in two more fields. This struct carries the full width so the truncation cannot
// happen on the way in; the widening of B.0 is RAISED to the orchestrator, not worked around here.
struct EntrantRow {
    uint64_t platformCode;
    uint64_t accountId;
    uint64_t sessionId;
    char displayName[37];
    char platformStr[37];
    uint16_t entrantIndex;
    uint16_t generation;
    uint16_t team;         // events_facts::kTeamNone (0xFFFF) means NO TEAM
    uint16_t marker;
    uint64_t firstSeenUs;
    uint64_t lastSeenUs;
    bool active;
    bool hasProfileJson;   // a rich body was seen; a keepalive push never clears this
};

class LobbyTracker {
public:
    static constexpr uint16_t kMaxEntrants = 32;

    struct Stats {
        uint32_t bodiesSeen;        // decodable lobby bodies fed in
        uint32_t bodiesRefused;     // fed in and refused by the decoder: a framing surprise, reported
        uint32_t marksEmitted;
        uint32_t joins, leaves, teamChanges;
        uint32_t indexCollisions;   // an index claimed by a second ACTIVE account; see Note()
        uint32_t overflow;          // entrants beyond kMaxEntrants, counted rather than dropped quietly
        uint32_t removedUnknownSlot;// a LEAVE for a slot no active entrant holds
        uint32_t netIdSpaces;       // distinct SR15NetIdsResponse id-space keys observed
        uint32_t netIdEntries;
    };

    void Reset();

    // Feeds one S_EVENT body. Emits at most `cap` markers into `out` and returns how many. `tUs` is the
    // record's own source time, so a marker lands at the instant its edge did.
    uint32_t Note(LobbyKind kind, const void* body, uint32_t bytes, uint64_t tUs,
                  capture::MarkRec* out, uint32_t cap);

    uint16_t EntrantCount() const { return count_; }
    const EntrantRow& EntrantAt(uint16_t i) const { return rows_[i]; }
    // Null when no entrant holds that slot right now.
    const EntrantRow* FindByIndex(uint16_t entrantIndex) const;

    // The two id spaces, kept apart. A slot number alone does not identify an actor: it identifies one
    // inside ONE space, and there are two.
    struct NetIdRecord {
        uint64_t space;
        uint16_t count;
        uint16_t firstSlot, lastSlot;
        bool contiguous;
        uint16_t distinctIds;
    };
    uint16_t NetIdSpaceCount() const { return netIdCount_; }
    const NetIdRecord& NetIdSpaceAt(uint16_t i) const { return netIds_[i]; }

    const Stats& GetStats() const { return stats_; }

    // TEAM_CHANGE packs both teams into MarkRec::payload, because MarkRec has one payload word and a
    // team change is two values. The entrant is named by MarkRec::index, which resolves against the
    // roster; JOIN and LEAVE put the account id in the payload so a marker alone still names a player
    // when the roster row is missing.
    static uint64_t PackTeams(uint16_t prev, uint16_t now) {
        return (static_cast<uint64_t>(prev) << 16) | now;
    }
    static uint16_t PrevTeam(uint64_t payload) { return static_cast<uint16_t>(payload >> 16); }
    static uint16_t NowTeam(uint64_t payload) { return static_cast<uint16_t>(payload); }

private:
    EntrantRow rows_[kMaxEntrants] = {};
    uint16_t count_ = 0;
    static constexpr uint16_t kMaxNetIdSpaces = 4;
    NetIdRecord netIds_[kMaxNetIdSpaces] = {};
    uint16_t netIdCount_ = 0;
    Stats stats_ = {};

    EntrantRow* FindByAccount(uint64_t platformCode, uint64_t accountId);
    EntrantRow* MutableByIndex(uint16_t entrantIndex);
};

}  // namespace events
}  // namespace replay
