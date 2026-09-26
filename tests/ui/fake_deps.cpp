// W2-F. The seams panel.cpp reaches outside src/ui, stood in for so an offline test does not have
// to drag the playback module and the VOIP module in behind them.
//
// The residue accessors are stubbed because nothing here is about the battery -- the panel's
// rendering of it is W2-B's own gate's subject, and linking playback would make a UI test fail for
// a reason that has nothing to do with the UI. `consulted` is false, which is the honest "we could
// not ask" the panel renders as UNKNOWN.
//
// SetVoipDemoPath is stood in for so its CALL can be observed. The real one lives in
// voip_module.cpp beside a REPLAY_MODULE registrar and the VOIP.RECEIVE detour; what this test has
// to prove is that the panel makes the call at all, which is precisely what nobody did.

#include "audio/voip/voipplay.h"
#include "playback/core/entry_gametype.h"
#include "playback/core/playback.h"

#include <string>

namespace {
std::wstring g_voipPath;
uint32_t g_voipCalls = 0;
uint32_t g_voipClears = 0;
replay::audio::voip::VoipPlayStats g_voipStats;
uint64_t g_ownerStateGametype = 0;
uint32_t g_ownerStateReads = 0;
}  // namespace

namespace uifake {

const wchar_t* VoipDemoPath() { return g_voipPath.c_str(); }
uint32_t VoipPathCalls() { return g_voipCalls; }
uint32_t VoipPathClears() { return g_voipClears; }
replay::audio::voip::VoipPlayStats& VoipStatsForTest() { return g_voipStats; }
void SetOwnerStateGametype(uint64_t gametype) {
    g_ownerStateGametype = gametype;
    g_ownerStateReads = 0;
}
uint32_t OwnerStateReads() { return g_ownerStateReads; }
void ResetVoip() {
    g_voipPath.clear();
    g_voipCalls = 0;
    g_voipClears = 0;
    g_voipStats = replay::audio::voip::VoipPlayStats();
}

}  // namespace uifake

namespace replay {
namespace playback {

ResidueStatus ResidueSnapshot() { return ResidueStatus(); }
uint64_t OwnerStateGametypeOfFile(const wchar_t*) {
    ++g_ownerStateReads;
    return g_ownerStateGametype;
}
size_t UnavailableResidueIds(const char**, size_t) { return 0; }
const char* ResidueDiagnostic(const char*) { return nullptr; }

}  // namespace playback

namespace audio {
namespace voip {

void SetVoipDemoPath(const wchar_t* path) {
    if (path == nullptr || path[0] == L'\0') {
        g_voipPath.clear();
        ++g_voipClears;
        return;
    }
    g_voipPath = path;
    ++g_voipCalls;
}

// The player's counters, as the voice row reads them. Static and test-settable, so the row can be
// shown printing a number the player produced rather than one it made up.
const VoipPlayStats& PlaybackStats() { return g_voipStats; }

}  // namespace voip
}  // namespace audio
}  // namespace replay
