#include <algorithm>
#include <string.h>
#include <vector>

#include "capture/roster.h"
#include "import/inferred.h"
#include "import/timeline.h"
#include "ui/levelname.h"

namespace replay::import {
namespace {

const char* const kPartNames[kPartCount] = {"head", "body", "lhand", "rhand", "disc"};
constexpr const char* kArenaMap = "mpl_arena_a";

Result CheckBasisTrips(const SourceMatch& match) {
    for (int part = 0; part < kPartCount; ++part) {
        const uint64_t samples = part == kPartDisc ? match.frames.size() : match.players.size();
        const uint64_t trips = match.stats.basisTrips[part];
        if (static_cast<double>(trips) > 0.001 * static_cast<double>(samples)) {
            return Refuse(ResultCode::Basis, "mirror guard tripped on %llu of %llu %s samples, first at source frame %u",
                          static_cast<unsigned long long>(trips), static_cast<unsigned long long>(samples),
                          kPartNames[part], match.stats.basisFirstTripFrame[part]);
        }
    }
    return Success();
}

Result GateMap(SourceMatch& match) {
    size_t count = 0;
    const ui::LevelNameRow* rows = ui::LevelNameTable(&count);
    for (size_t i = 0; i < count && strcmp(match.mapName, kArenaMap) == 0; ++i) {
        if (strcmp(rows[i].name, kArenaMap) == 0) {
            match.mapSymbol = rows[i].id;
            return Success();
        }
    }
    return Refuse(ResultCode::UnsupportedMap, "map %s is not supported; only %s converts", match.mapName, kArenaMap);
}

struct Segment {
    uint32_t begin, end;
};

std::vector<Segment> SessionSegments(const SourceMatch& match) {
    std::vector<Segment> segments;
    for (uint32_t i = 0; i < match.frames.size(); ++i) {
        if (i == 0 || match.frames[i].session != match.frames[i - 1].session) segments.push_back({i, i});
        segments.back().end = i + 1;
    }
    return segments;
}

int64_t MedianPositiveStep(const std::vector<int64_t>& stamps) {
    std::vector<int64_t> steps;
    for (size_t i = 1; i < stamps.size(); ++i) {
        if (stamps[i] > stamps[i - 1]) steps.push_back(stamps[i] - stamps[i - 1]);
    }
    if (steps.empty()) return 0;
    std::nth_element(steps.begin(), steps.begin() + steps.size() / 2, steps.end());
    return steps[steps.size() / 2];
}

// Keeps file order. More than 1% repaired means the stamps are jitter rather than a clock, so the
// segment is re-timed on the nominal period, keeping only the real gaps.
void RepairTime(SourceMatch& match, Segment range, ConvertReport& report) {
    std::vector<int64_t> raw(range.end - range.begin);
    for (uint32_t i = range.begin; i < range.end; ++i) raw[i - range.begin] = match.frames[i].tUs;

    uint32_t repaired = 0;
    for (uint32_t i = range.begin + 1; i < range.end; ++i) {
        const int64_t floor = match.frames[i - 1].tUs + kRepairStepUs;
        if (match.frames[i].tUs < floor) {
            match.frames[i].tUs = floor;
            ++repaired;
        }
    }
    report.repairedStamps = repaired;
    if (static_cast<uint64_t>(repaired) * 100 <= raw.size()) return;

    int64_t period = match.nominalHz != 0 ? 1000000 / match.nominalHz : MedianPositiveStep(raw);
    if (period <= 0) period = kRepairStepUs;
    report.retimedWhole = true;
    for (uint32_t i = range.begin + 1; i < range.end; ++i) {
        const int64_t rawStep = raw[i - range.begin] - raw[i - range.begin - 1];
        match.frames[i].tUs = match.frames[i - 1].tUs + (rawStep > kGapUs ? rawStep : period);
    }
}

Segment TrimIsolatedEnds(const SourceMatch& match, Segment range, ConvertReport& report) {
    while (range.end - range.begin >= 2 &&
           match.frames[range.begin + 1].tUs - match.frames[range.begin].tUs > kIsolatedEndUs) {
        ++range.begin;
        ++report.leadInDropped;
    }
    while (range.end - range.begin >= 2 &&
           match.frames[range.end - 1].tUs - match.frames[range.end - 2].tUs > kIsolatedEndUs) {
        --range.end;
        ++report.tailDropped;
    }
    return range;
}

// Rebuilds players and events for frames [begin, end) only, re-slicing the frames.
void KeepFrames(SourceMatch& match, Segment range) {
    std::vector<SrcFrame> frames(match.frames.begin() + range.begin, match.frames.begin() + range.end);
    std::vector<SrcPlayer> players;
    std::vector<SrcEvent> events;
    for (SrcFrame& f : frames) {
        const uint32_t firstPlayer = static_cast<uint32_t>(players.size());
        players.insert(players.end(), match.players.begin() + f.firstPlayer,
                       match.players.begin() + f.firstPlayer + f.playerCount);
        f.firstPlayer = firstPlayer;
        const uint32_t firstEvent = static_cast<uint32_t>(events.size());
        for (uint32_t e = f.firstEvent; e < f.firstEvent + f.eventCount; ++e) {
            SrcEvent event = match.events[e];
            event.tUs = f.tUs;  // stamped at its frame, on the repaired axis
            events.push_back(event);
        }
        f.firstEvent = firstEvent;
    }
    match.frames = std::move(frames);
    match.players = std::move(players);
    match.events = std::move(events);
}

void HoldEmptyStatus(SourceMatch& match) {
    GameStatus last = GameStatus::Empty;
    for (SrcFrame& f : match.frames) {
        if (f.status == GameStatus::Empty) {
            f.status = last;
        } else {
            last = f.status;
        }
    }
}

// File slots in first-appearance order, keyed by userId (D4). The reader's roster row stands in for
// a player whose source carried no userId.
Result RebuildRoster(SourceMatch& match) {
    std::vector<uint16_t> oldToNew(match.roster.size(), kNoSlot);
    std::vector<SrcIdentity> roster;
    for (const SrcFrame& f : match.frames) {
        for (uint32_t i = f.firstPlayer; i < f.firstPlayer + f.playerCount; ++i) {
            SrcPlayer& p = match.players[i];
            if (p.rosterIndex >= oldToNew.size()) {
                return Refuse(ResultCode::Corrupt, "a player sample names roster row %u of %zu", p.rosterIndex,
                              match.roster.size());
            }
            uint16_t& slot = oldToNew[p.rosterIndex];
            if (slot == kNoSlot) {
                const SrcIdentity& identity = match.roster[p.rosterIndex];
                for (size_t r = 0; r < roster.size() && identity.userId != kNoUser; ++r) {
                    if (roster[r].userId == identity.userId) slot = static_cast<uint16_t>(r);
                }
                if (slot == kNoSlot) {
                    if (roster.size() >= replay::capture::kRosterMaxRows) {
                        return Refuse(ResultCode::Roster, "more than %u distinct players", replay::capture::kRosterMaxRows);
                    }
                    slot = static_cast<uint16_t>(roster.size());
                    roster.push_back(identity);
                    roster.back().team = p.team;
                    roster.back().firstSeenUs = f.tUs;
                }
            }
            p.rosterIndex = slot;
            roster[slot].lastSeenUs = f.tUs;
        }
    }
    match.roster = std::move(roster);
    return Success();
}

}  // namespace

Result Normalise(SourceMatch& match, const ConvertOptions& options, ConvertReport& report) {
    report.kind = match.kind;
    report.source = match.stats;

    if (match.stats.lines != 0 && static_cast<uint64_t>(match.stats.badLines) * 100 > match.stats.lines) {
        return Refuse(ResultCode::Corrupt, "%u of %u source lines are unreadable", match.stats.badLines,
                      match.stats.lines);
    }
    Result r = GateMap(match);
    if (!r.ok) return r;
    r = CheckBasisTrips(match);
    if (!r.ok) return r;
    if (match.frames.empty()) return Refuse(ResultCode::TooShort, "the source holds no frames");

    const std::vector<Segment> segments = SessionSegments(match);
    report.sessionsFound = static_cast<uint32_t>(segments.size());
    uint32_t chosen = 0;
    if (options.session >= 0) {
        if (static_cast<uint32_t>(options.session) >= segments.size()) {
            return Refuse(ResultCode::BadArguments, "session %d was requested; the source has %zu", options.session,
                          segments.size());
        }
        chosen = static_cast<uint32_t>(options.session);
    } else {
        for (uint32_t s = 1; s < segments.size(); ++s) {
            if (segments[s].end - segments[s].begin > segments[chosen].end - segments[chosen].begin) chosen = s;
        }
    }
    report.sessionChosen = chosen;

    RepairTime(match, segments[chosen], report);
    KeepFrames(match, TrimIsolatedEnds(match, segments[chosen], report));
    report.framesKept = static_cast<uint32_t>(match.frames.size());

    const int64_t durationUs = match.frames.back().tUs - match.frames.front().tUs;
    if (durationUs < kMinDurationUs) {
        return Refuse(ResultCode::TooShort, "only %.2f s of frames remain after trimming; 2 s are needed",
                      static_cast<double>(durationUs) / 1e6);
    }
    for (size_t i = 1; i < match.frames.size(); ++i) {
        const int64_t step = match.frames[i].tUs - match.frames[i - 1].tUs;
        if (step > kGapUs) report.gapList.push_back({match.frames[i - 1].tUs - match.frames.front().tUs, step});
    }
    report.gaps = static_cast<uint32_t>(report.gapList.size());
    report.requiresAllowGaps = report.gaps != 0;

    HoldEmptyStatus(match);
    return RebuildRoster(match);
}

}  // namespace replay::import
