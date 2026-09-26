#pragma once

// Hand-built SourceMatch values for the Normalise/Infer suites: what a reader would produce, one
// frame and one player sample at a time.

#include <stdio.h>
#include <string.h>

#include "import/sourcemodel.h"

namespace import_test {

using namespace replay::import;

constexpr int64_t kStep30Hz = 1000000 / 30;

class MatchBuilder {
public:
    explicit MatchBuilder(SourceKind kind = SourceKind::EchoReplay) {
        match.kind = kind;
        snprintf(match.mapName, sizeof match.mapName, "%s", "mpl_arena_a");
        match.sessions.push_back(SrcSession{"session-0"});
    }

    // Returns the reader's roster row, which Normalise re-keys into a file slot.
    uint16_t Identity(uint64_t userId, const char* name, uint8_t team) {
        SrcIdentity id{};
        id.userId = userId;
        snprintf(id.name, sizeof id.name, "%s", name);
        id.number = id.level = kNotInFrame;
        id.team = team;
        match.roster.push_back(id);
        return static_cast<uint16_t>(match.roster.size() - 1);
    }

    SrcFrame& Frame(int64_t tUs, uint16_t session = 0) {
        SrcFrame f{};
        f.tUs = tUs;
        f.session = session;
        f.status = GameStatus::Playing;
        f.disc.holderApiSlot = kNotInFrame;
        f.disc.pose.q[3] = 1.0f;
        f.firstPlayer = static_cast<uint32_t>(match.players.size());
        f.firstEvent = static_cast<uint32_t>(match.events.size());
        match.frames.push_back(f);
        return match.frames.back();
    }

    // Appended to the latest frame. Identity orientation everywhere: forward +z, left +x, up +y.
    SrcPlayer& Player(uint16_t rosterRow, uint8_t apiSlot, float x = 0.0f, float z = 0.0f) {
        SrcPlayer p{};
        p.userId = match.roster[rosterRow].userId;
        p.rosterIndex = rosterRow;
        p.team = match.roster[rosterRow].team;
        p.apiSlot = apiSlot;
        Pose* poses[] = {&p.head, &p.body, &p.lhand, &p.rhand};
        for (Pose* pose : poses) {
            pose->pos[0] = x;
            pose->pos[1] = 1.0f;
            pose->pos[2] = z;
            pose->q[3] = 1.0f;
        }
        p.lhand.pos[0] += 0.3f;
        p.rhand.pos[0] -= 0.3f;
        p.holdLSlot = p.holdRSlot = kNotInFrame;
        p.number = p.level = kNotInFrame;
        match.players.push_back(p);
        ++match.frames.back().playerCount;
        return match.players.back();
    }

    SrcEvent& Event(SrcEventKind kind) {
        SrcEvent e{};
        e.tUs = match.frames.back().tUs;
        e.kind = kind;
        match.events.push_back(e);
        ++match.frames.back().eventCount;
        return match.events.back();
    }

    SourceMatch match{};
};

}  // namespace import_test
