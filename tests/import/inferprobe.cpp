// import_inferprobe: ReadSource -> Normalise -> Infer on one legacy file with every rule enabled and
// any threshold overridden, printing what Infer produced. It is P7's instrument (tools/import/
// validate_infer.py): native captures reduced to API-like files go in, the deleted truth is scored
// against this output. Not a test.
//
//   import_inferprobe <source> [--set name=value ...]
//
// Output lines, keyed by the source's userid and game_clock (the reducer writes capture seconds there):
//   S,<clock>,<userid>,<bits28>,<heldLUser>,<heldRUser>,<trigL>,<trigR>
//   T,<clock>,<victimUser>,<puncherUser>,<duration>,<flags>
//   G,<clock>,<team>,<points>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <vector>

#include "import/inferred.h"
#include "import/reader.h"

using namespace replay::import;

namespace {

struct Knob {
    const wchar_t* name;
    float* f;
    uint8_t* u8;
};

bool Override(InferFacts& facts, const wchar_t* assignment) {
    const Knob knobs[] = {
        {L"movingAttachedMinSpeed", &facts.movingAttachedMinSpeed, nullptr},
        {L"boostWindowS", &facts.boostWindowS, nullptr},
        {L"boostMinGainMps", &facts.boostMinGainMps, nullptr},
        {L"boostMinPeakAccel", &facts.boostMinPeakAccel, nullptr},
        {L"boostMaxTurnDeg", &facts.boostMaxTurnDeg, nullptr},
        {L"brakeMaxAccel", &facts.brakeMaxAccel, nullptr},
        {L"brakeMinFrames", nullptr, &facts.brakeMinFrames},
        {L"brakeMaxSpeedRatio", &facts.brakeMaxSpeedRatio, nullptr},
        {L"brakeMaxTurnDeg", &facts.brakeMaxTurnDeg, nullptr},
        {L"thrustMinAccel", &facts.thrustMinAccel, nullptr},
        {L"thrustMaxAccel", &facts.thrustMaxAccel, nullptr},
        {L"thrustMinFrames", nullptr, &facts.thrustMinFrames},
        {L"thrustMinCos", &facts.thrustMinCos, nullptr},
        {L"punchMinHandSpeed", &facts.punchMinHandSpeed, nullptr},
        {L"punchMinFrames", nullptr, &facts.punchMinFrames},
        {L"stunStatWindowS", &facts.stunStatWindowS, nullptr},
        {L"stunNearRadiusM", &facts.stunNearRadiusM, nullptr},
    };
    const wchar_t* eq = wcschr(assignment, L'=');
    if (!eq) return false;
    for (const Knob& k : knobs) {
        if (wcsncmp(assignment, k.name, size_t(eq - assignment)) != 0 || k.name[eq - assignment] != 0) continue;
        const double value = wcstod(eq + 1, nullptr);
        if (k.f) *k.f = float(value);
        if (k.u8) *k.u8 = uint8_t(value);
        return true;
    }
    return false;
}

uint64_t UserOf(const SourceMatch& m, uint16_t slot) {
    return slot < m.roster.size() ? m.roster[slot].userId : 0;
}

// Frame clock at a source time; rows are sorted, frames are sorted.
float ClockAt(const SourceMatch& m, int64_t tUs) {
    size_t lo = 0, hi = m.frames.size();
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        (m.frames[mid].tUs <= tUs ? lo : hi) = mid;
    }
    const SrcFrame& f = m.frames[lo];
    return f.gameClock + float(double(tUs - f.tUs) / 1e6);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: import_inferprobe <source> [--set name=value ...]\n");
        return 2;
    }
    InferFacts facts = kInferFacts;
    for (RuleGate& g : facts.rules) g.enabled = true;
    for (int i = 2; i < argc; ++i) {
        if (wcscmp(argv[i], L"--set") == 0 && i + 1 < argc && Override(facts, argv[i + 1])) {
            ++i;
            continue;
        }
        fwprintf(stderr, L"unknown argument %ls\n", argv[i]);
        return 2;
    }

    SourceMatch m;
    ConvertReport report{};
    Result r = ReadSource(argv[1], m, nullptr);
    if (r.ok) r = Normalise(m, ConvertOptions{}, report);
    InferredTracks tracks;
    if (r.ok) r = Infer(m, facts, tracks, report);
    if (!r.ok) {
        fprintf(stderr, "%s: %s\n", ResultCodeName(r.code), r.message);
        return 2;
    }
    fprintf(stderr, "frames=%u repaired=%u retimed=%d gaps=%u\n", report.framesKept, report.repairedStamps,
            int(report.retimedWhole), report.gaps);

    for (const SrcFrame& f : m.frames) {
        for (uint32_t i = f.firstPlayer; i < f.firstPlayer + f.playerCount; ++i) {
            const SampleInference& s = tracks.samples[i];
            printf("S,%.4f,%llu,%u,%llu,%llu,%d,%d\n", f.gameClock, (unsigned long long)m.players[i].userId, s.bits28,
                   (unsigned long long)(s.holdLSlot == kNoSlot ? 0 : UserOf(m, s.holdLSlot)),
                   (unsigned long long)(s.holdRSlot == kNoSlot ? 0 : UserOf(m, s.holdRSlot)), int(s.trigL),
                   int(s.trigR));
        }
    }
    for (const StunRow& s : tracks.stuns) {
        printf("T,%.4f,%llu,%llu,%.3f,%u\n", ClockAt(m, s.tUs), (unsigned long long)UserOf(m, s.victimSlot),
               (unsigned long long)UserOf(m, s.puncherSlot), s.durationS, unsigned(s.flags));
    }
    for (const GoalRow& g : tracks.goals) printf("G,%.4f,%u,%u\n", ClockAt(m, g.tUs), unsigned(g.team), unsigned(g.points));
    return 0;
}
