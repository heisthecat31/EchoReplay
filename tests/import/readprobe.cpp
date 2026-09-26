// import_readprobe: runs ReadSource over each file named on the command line and prints what it read,
// how long it took and the process's peak private memory. The P2 local check on the owner's files and
// the perf measurement both use it; it is not a CI test (the owner's files are never in the tree).
//
//   import_readprobe.exe [--runs N] file...

#include <windows.h>
#include <psapi.h>

#include <stdio.h>
#include <wchar.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <vector>

#include "import/reader.h"

using namespace replay::import;

namespace {

double PeakPrivateMb() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof counters);
    return double(counters.PeakPagefileUsage) / (1024.0 * 1024.0);
}

void Describe(const SourceMatch& m) {
    const SourceStats& s = m.stats;
    std::vector<int64_t> dts;
    for (size_t i = 1; i < m.frames.size(); ++i) dts.push_back(m.frames[i].tUs - m.frames[i - 1].tUs);
    std::sort(dts.begin(), dts.end());
    const auto pct = [&](double p) { return dts.empty() ? 0.0 : dts[size_t(p * double(dts.size() - 1))] / 1000.0; };
    int64_t maxGap = dts.empty() ? 0 : dts.back();
    size_t maxGapAt = 0;
    int64_t minStep = dts.empty() ? 0 : dts.front();
    for (size_t i = 1; i < m.frames.size(); ++i) {
        if (m.frames[i].tUs - m.frames[i - 1].tUs == maxGap) maxGapAt = i;
    }
    std::set<uint64_t> present;
    uint32_t maxPlayers = 0;
    for (const SrcFrame& f : m.frames) maxPlayers = std::max(maxPlayers, f.playerCount);
    for (const SrcPlayer& p : m.players) present.insert(p.userId);
    uint32_t eventsByKind[16] = {};
    for (const SrcEvent& e : m.events) {
        if (uint16_t(e.kind) < 16) ++eventsByKind[uint16_t(e.kind)];
    }
    const double span = m.frames.empty() ? 0.0 : (m.frames.back().tUs - m.frames.front().tUs) / 1e6;
    printf("  kind=%s map=%s sessions=%zu frames=%zu samples=%zu roster=%zu maxPerFrame=%u span=%.1fs nominalHz=%u\n",
           m.kind == SourceKind::Tape ? "tape" : "echoreplay", m.mapName, m.sessions.size(), m.frames.size(),
           m.players.size(), m.roster.size(), maxPlayers, span, m.nominalHz);
    printf("  dt ms: median=%.1f p90=%.1f min=%.1f max=%.1f (at frame %zu)\n", pct(0.5), pct(0.9), minStep / 1000.0,
           maxGap / 1000.0, maxGapAt);
    printf("  lines=%u bad=%u errCode=%u dup=%u bones=%u unresolved=%u truncated=%d zipEntries=%u\n", s.lines,
           s.badLines, s.errCodeFrames, s.duplicateFrames, s.bonesSkipped, s.unresolvedPlayerRefs, s.truncatedSource,
           s.zipEntries);
    printf("  basisTrips head=%u body=%u lhand=%u rhand=%u disc=%u\n", s.basisTrips[kPartHead], s.basisTrips[kPartBody],
           s.basisTrips[kPartLHand], s.basisTrips[kPartRHand], s.basisTrips[kPartDisc]);
    printf("  events goal=%u stun=%u join=%u left=%u switch=%u roundStart=%u roundEnd=%u matchEnd=%u thrown=%u caught=%u\n",
           eventsByKind[1], eventsByKind[2], eventsByKind[3], eventsByKind[4], eventsByKind[5], eventsByKind[6],
           eventsByKind[7], eventsByKind[8], eventsByKind[9], eventsByKind[10]);
    printf("  roster:");
    for (const SrcIdentity& id : m.roster) printf(" [%s t%u]", id.name, id.team);
    printf("\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    int runs = 1;
    int first = 1;
    if (argc > 2 && wcscmp(argv[1], L"--runs") == 0) {
        runs = _wtoi(argv[2]);
        first = 3;
    }
    if (first >= argc || runs < 1) {
        fprintf(stderr, "usage: import_readprobe [--runs N] file...\n");
        return 2;
    }
    int failures = 0;
    for (int i = first; i < argc; ++i) {
        wprintf(L"%ls\n", argv[i]);
        std::vector<double> ms;
        SourceMatch match;
        Result r = Success();
        for (int run = 0; run < runs && r.ok; ++run) {
            const auto t0 = std::chrono::steady_clock::now();
            r = ReadSource(argv[i], match, nullptr);
            ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        if (!r.ok) {
            printf("  REFUSED %s: %s\n", ResultCodeName(r.code), r.message);
            ++failures;
            continue;
        }
        std::sort(ms.begin(), ms.end());
        const double median = ms[ms.size() / 2];
        const double mb = double(match.stats.sourceBytes) / (1024.0 * 1024.0);
        Describe(match);
        printf("  time median=%.0f ms over %d run(s); %.1f MB file -> %.1f ms/MB; peak private %.0f MB\n", median, runs, mb,
               median / mb, PeakPrivateMb());
    }
    return failures == 0 ? 0 : 1;
}
