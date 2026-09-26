#include "doctest.h"

#include <windows.h>

#include <stdint.h>
#include <stdlib.h>

#include <iterator>
#include <map>

#include "foundation/demofile/demofile.h"
#include "playback/core/window.h"
#include "playback/core/windowbuild.h"
#include "../local_inputs.h"

using namespace replay;
using namespace replay::playback;

// FIX-AUDIO-B. The live server relays each player's action word at CR15PlayerBroadcasterCS's send
// interval, and a live client applies every packet it receives. EmitDelegates diffs the RECORDED
// word, so it matches live exactly when the recording already holds one word per send -- which is
// what this case measures on a real capture, rather than assumes.
namespace {

constexpr const char* kCapture = "2026-09-24_12-59-23_576ED3F8428EBC4B_CB60A4DE7E1CAF73.echodemo";

// sub_140D03F20 stores (uint64)(int)(1000000.0 / 45.0) into BOTH of the intervals sub_140D6E980
// chooses between by connected-player count (+336 for <= 5, +344 for > 5): 45 Hz either way.
constexpr uint64_t kLiveSendIntervalUs = 1000000 / 45;
constexpr uint32_t kGeoAttachBits = (1u << 5) | (1u << 6);

bool OpenCapture(DemoReader& reader) {
    const wchar_t* home = _wgetenv(L"USERPROFILE");
    if (home == nullptr) return false;
    wchar_t path[1024];
    _snwprintf_s(path, _TRUNCATE, L"%s/Documents/EchoReplay/%S", home, kCapture);
    return reader.Open(path);
}

}  // namespace

TEST_CASE("FIX-AUDIO-B: the recorded action word is already at the live send cadence") {
    DemoReader reader;
    if (!OpenCapture(reader)) {
        // Absent skips, as in test_track_map.cpp, unless ECHOREPLAY_TEST_REQUIRE_CAPTURE=1.
        if (replay_test::MissingCaptureFails()) {
            FAIL_CHECK("absent or unopenable: %USERPROFILE%/Documents/EchoReplay/" << kCapture);
        } else {
            WARN_MESSAGE(false, "SKIPPED: absent or unopenable: ", kCapture);
        }
        return;
    }
    TrackScan scan;
    const char* why = "";
    REQUIRE_MESSAGE(ScanTrackSources(reader, scan, &why), why);

    std::map<uint16_t, std::map<uint64_t, uint32_t>> words;   // slot -> stamp -> word
    Window w;
    for (size_t c = 0; c < reader.ChapterCount(); c += 2) {   // a window spans three chapters
        BuildStats st;
        if (!BuildWindowRange(reader, c, w, st, &scan.clocks)) continue;
        for (size_t i = 0; i < w.PlayerCount(); ++i) {
            const PlayerTrack& tr = w.PlayerAt(i);
            const PlayerState* f = w.PlayerFrames(tr);
            const uint64_t* t = w.PlayerTimes(tr);
            for (size_t k = 0; k < tr.count; ++k) words[tr.user_slot][t[k]] = f[k].bits28;
        }
    }
    REQUIRE(words.size() == 8);

    uint64_t recordedEdges = 0, sendGridEdges = 0, sampleGaps = 0, gapTotalUs = 0;
    for (const auto& [slot, series] : words) {
        uint32_t prev = series.begin()->second;
        uint64_t prevT = series.begin()->first;
        for (const auto& [t, word] : series) {
            if ((prev ^ word) & kGeoAttachBits) ++recordedEdges;
            if (t != prevT) {
                ++sampleGaps;
                gapTotalUs += t - prevT;
            }
            prev = word;
            prevT = t;
        }
        // What a client would apply if it only saw the word at the server's send instants.
        auto at = series.begin();
        uint32_t held = at->second;
        for (uint64_t g = series.begin()->first; g <= series.rbegin()->first; g += kLiveSendIntervalUs) {
            while (std::next(at) != series.end() && std::next(at)->first <= g) ++at;
            if ((held ^ at->second) & kGeoAttachBits) ++sendGridEdges;
            held = at->second;
        }
    }
    const double meanGapUs = double(gapTotalUs) / double(sampleGaps);
    MESSAGE("hand-attached edges: recorded ", recordedEdges, ", on the 45 Hz send grid ",
            sendGridEdges, "; mean recorded word gap ", meanGapUs, " us over ", sampleGaps);
    // One word per send: arrivals jitter between client frames, but on average they come at the
    // send interval, so coalescing at the send rate has nothing left to merge.
    CHECK(meanGapUs >= kLiveSendIntervalUs * 0.9);
    CHECK(meanGapUs <= kLiveSendIntervalUs * 1.1);
    CHECK(double(sendGridEdges) >= 0.995 * double(recordedEdges));
    CHECK(sendGridEdges <= recordedEdges);
}
