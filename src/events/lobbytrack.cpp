#include "events/lobbytrack.h"

#include <string.h>

#include "events/events_facts.h"

namespace replay {
namespace events {
namespace {

using namespace replay::events_facts;

void CopyStr(char (&dst)[37], const StrView& s) {
    const uint32_t n = s.n < 36 ? s.n : 36;
    memcpy(dst, s.p, n);
    dst[n] = '\0';
}

}  // namespace

LobbyKind LobbyKindForName(const char* structName) {
    if (structName == nullptr) {
        return LobbyKind::Other;
    }
    struct Row { const char* name; LobbyKind kind; };
    static const Row kRows[] = {
        {"SNSLobbyEntrantStatev2", LobbyKind::EntrantState},
        {"SNSLobbyEntrantRemoved", LobbyKind::EntrantRemoved},
        {"SNSLobbyEntrantTeamChanged", LobbyKind::EntrantTeamChanged},
        {"SNSLobbyOwnerState", LobbyKind::OwnerState},
        {"SR15NetIdsResponse", LobbyKind::NetIds},
        {"SNSRefreshProfileFromServer", LobbyKind::ProfileFromServer},
        {"SR15NetReliableStatUpdate", LobbyKind::StatUpdate},
        {"SR15NetReliableTeamStatUpdate", LobbyKind::TeamStatUpdate},
    };
    for (const Row& r : kRows) {
        if (strcmp(r.name, structName) == 0) {
            return r.kind;
        }
    }
    return LobbyKind::Other;
}

void LobbyTracker::Reset() {
    memset(rows_, 0, sizeof rows_);
    memset(netIds_, 0, sizeof netIds_);
    count_ = 0;
    netIdCount_ = 0;
    stats_ = Stats{};
}

EntrantRow* LobbyTracker::FindByAccount(uint64_t platformCode, uint64_t accountId) {
    for (uint16_t i = 0; i < count_; ++i) {
        if (rows_[i].accountId == accountId && rows_[i].platformCode == platformCode) {
            return &rows_[i];
        }
    }
    return nullptr;
}

EntrantRow* LobbyTracker::MutableByIndex(uint16_t entrantIndex) {
    for (uint16_t i = 0; i < count_; ++i) {
        if (rows_[i].active && rows_[i].entrantIndex == entrantIndex) {
            return &rows_[i];
        }
    }
    return nullptr;
}

const EntrantRow* LobbyTracker::FindByIndex(uint16_t entrantIndex) const {
    for (uint16_t i = 0; i < count_; ++i) {
        if (rows_[i].active && rows_[i].entrantIndex == entrantIndex) {
            return &rows_[i];
        }
    }
    return nullptr;
}

uint32_t LobbyTracker::Note(LobbyKind kind, const void* body, uint32_t bytes, uint64_t tUs,
                            capture::MarkRec* out, uint32_t cap) {
    uint32_t n = 0;
    auto emit = [&](uint16_t markKind, uint16_t index, uint64_t payload) {
        if (n < cap) {
            out[n].tUs = tUs;
            out[n].kind = markKind;
            out[n].index = index;
            out[n].payload = payload;
            ++n;
            ++stats_.marksEmitted;
        }
    };

    switch (kind) {
        case LobbyKind::EntrantState: {
            EntrantState e{};
            if (!DecodeEntrantState(body, bytes, &e)) {
                ++stats_.bodiesRefused;
                break;
            }
            ++stats_.bodiesSeen;

            EntrantRow* row = FindByAccount(e.platformCode, e.accountId);
            if (row == nullptr) {
                if (count_ >= kMaxEntrants) {
                    // ★ Counted, never silently dropped. A capped table that overflows quietly moves
                    // attribution rather than losing it -- the totals stay plausible while the rows
                    // belong to the wrong people. Reset() plus this counter is what a reader checks.
                    ++stats_.overflow;
                    break;
                }
                row = &rows_[count_++];
                memset(row, 0, sizeof *row);
                row->platformCode = e.platformCode;
                row->accountId = e.accountId;
                row->team = kTeamNone;
                row->firstSeenUs = tUs;
            }

            // The slot this entrant now occupies. Two rows answering FindByIndex would make an index
            // lookup pick one of two accounts, so any OTHER active holder of this slot is evicted and
            // the collision counted. The host does not compact slots, so a reused index is expected;
            // two live claims on one index at the same instant is not, and is worth a number.
            if (EntrantRow* other = MutableByIndex(e.entrantIndex)) {
                if (other != row) {
                    ++stats_.indexCollisions;
                    other->active = false;
                }
            }

            const bool wasActive = row->active;
            const uint16_t prevTeam = row->team;
            row->entrantIndex = e.entrantIndex;
            row->generation = e.generation;
            row->marker = e.marker;
            row->sessionId = e.sessionId;
            row->lastSeenUs = tUs;
            row->active = true;

            // A keepalive push carries the same header with no profile tail, and the host's own
            // placeholder forward carries an EMPTY 36-byte name field until the entrant's real push
            // arrives. Neither may downgrade what we already hold: an entrant who was named must not
            // become nameless because the next 20 ms body was thin.
            if (e.displayName.n != 0 || row->displayName[0] == '\0') {
                CopyStr(row->displayName, e.displayName);
            }
            if (e.platformStr.n != 0 || row->platformStr[0] == '\0') {
                CopyStr(row->platformStr, e.platformStr);
            }
            row->hasProfileJson = row->hasProfileJson || e.hasProfileJson;

            if (!wasActive) {
                row->team = e.team;
                ++stats_.joins;
                emit(capture::kMarkJoin, e.entrantIndex, e.accountId);
                // A body that arrives already carrying a team states the entrant's side at the moment
                // they became visible. That is part of the JOIN, not a change, so no TEAM_CHANGE is
                // emitted for it -- otherwise every join would be followed by a phantom switch.
            } else if (e.team != prevTeam) {
                row->team = e.team;
                ++stats_.teamChanges;
                emit(capture::kMarkTeamChange, e.entrantIndex, PackTeams(prevTeam, e.team));
            }
            break;
        }

        case LobbyKind::EntrantTeamChanged: {
            EntrantTeamChanged t{};
            if (!DecodeEntrantTeamChanged(body, bytes, &t)) {
                ++stats_.bodiesRefused;
                break;
            }
            ++stats_.bodiesSeen;
            const uint16_t idx = static_cast<uint16_t>(t.entrantIndex);
            EntrantRow* row = MutableByIndex(idx);
            if (row == nullptr) {
                // The message names a slot, and this is a slot nobody has claimed in our view. There is
                // no account to attribute it to, so it is recorded as a marker on the slot and NOT as
                // a roster row invented from an index -- an identity guessed from a slot is exactly
                // what the key rule above exists to prevent.
                ++stats_.teamChanges;
                emit(capture::kMarkTeamChange, idx, PackTeams(kTeamNone, t.team));
                break;
            }
            if (row->team != t.team) {
                const uint16_t prev = row->team;
                row->team = t.team;
                ++stats_.teamChanges;
                emit(capture::kMarkTeamChange, idx, PackTeams(prev, t.team));
            }
            break;
        }

        case LobbyKind::EntrantRemoved: {
            uint64_t slot = 0;
            if (!DecodeEntrantRemoved(body, bytes, &slot)) {
                ++stats_.bodiesRefused;
                break;
            }
            ++stats_.bodiesSeen;
            const uint16_t idx = static_cast<uint16_t>(slot);
            EntrantRow* row = MutableByIndex(idx);
            if (row == nullptr) {
                ++stats_.removedUnknownSlot;
                break;
            }
            row->active = false;
            row->lastSeenUs = tUs;
            ++stats_.leaves;
            emit(capture::kMarkLeave, idx, row->accountId);
            break;
        }

        case LobbyKind::NetIds: {
            NetIdSpace s{};
            if (!DecodeNetIds(body, bytes, &s) || s.count == 0) {
                ++stats_.bodiesRefused;
                break;
            }
            ++stats_.bodiesSeen;

            NetIdRecord* rec = nullptr;
            for (uint16_t i = 0; i < netIdCount_ && rec == nullptr; ++i) {
                if (netIds_[i].space == s.space) {
                    rec = &netIds_[i];
                }
            }
            if (rec != nullptr) {
                // The same space re-sent replaces its row; the entry total must not accumulate two
                // copies of one space, or "how many slots does this file map" answers with a rate.
                stats_.netIdEntries -= rec->count;
            } else {
                if (netIdCount_ >= kMaxNetIdSpaces) {
                    ++stats_.overflow;
                    break;
                }
                rec = &netIds_[netIdCount_++];
            }

            uint16_t lo = NetSlotAt(s, 0), hi = lo;
            bool contiguous = true;
            for (uint16_t i = 1; i < s.count; ++i) {
                const uint16_t v = NetSlotAt(s, i);
                if (v != static_cast<uint16_t>(NetSlotAt(s, i - 1) + 1)) {
                    contiguous = false;
                }
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
            uint16_t distinct = 0;
            for (uint16_t i = 0; i < s.count; ++i) {
                bool dup = false;
                for (uint16_t j = 0; j < i && !dup; ++j) {
                    dup = NetIdAt(s, j) == NetIdAt(s, i);
                }
                if (!dup) ++distinct;
            }
            rec->space = s.space;
            rec->count = s.count;
            rec->firstSlot = lo;
            rec->lastSlot = hi;
            rec->contiguous = contiguous;
            rec->distinctIds = distinct;
            stats_.netIdSpaces = netIdCount_;
            stats_.netIdEntries += s.count;
            break;
        }

        case LobbyKind::OwnerState:
        case LobbyKind::ProfileFromServer:
        case LobbyKind::StatUpdate:
        case LobbyKind::TeamStatUpdate:
            // Decoded by lobbymsg.h and stored whole by the capture; none of them is a marker edge, so
            // the tracker only counts them. Reporting a body it read but drew nothing from is what
            // keeps "we saw none" and "we saw them and they said nothing" apart.
            ++stats_.bodiesSeen;
            break;

        case LobbyKind::Other:
            break;
    }
    return n;
}

}  // namespace events
}  // namespace replay
