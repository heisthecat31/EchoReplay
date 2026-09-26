#include "playback/core/entry_gametype.h"

#include <vector>

#include "capture/keyframe.h"
#include "capture/manifestbuild.h"
#include "capture/streamcodec.h"
#include "foundation/demofile/demofile.h"
#include "foundation/demofile/demotypes.h"
#include "foundation/gametype.h"
#include "playback/core/recorded_loadout.h"

namespace replay {
namespace playback {

namespace {
constexpr char kOwnerStateTapName[] = "SNSLobbyOwnerState";
}

uint64_t OwnerStateGametype(IDemoReader& reader) {
    std::vector<uint8_t> scratch(identity::kTapTableScratchBytes);
    std::vector<uint8_t> body(capture::kMaxEventBodyBytes);
    // The index comes from the file's own tap table, and stands until a later keyframe restates it.
    uint16_t tapIndex = capture::kTapTypeIndexNone;
    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        DecodedChapter dc;
        if (!reader.DecodeChapter(i, dc)) continue;
        const uint16_t here = identity::TapIndexForName(dc, kOwnerStateTapName,
                                                        sizeof(kOwnerStateTapName) - 1,
                                                        scratch.data(), scratch.size());
        if (here != capture::kTapTypeIndexNone) tapIndex = here;
        if (tapIndex == capture::kTapTypeIndexNone) continue;

        const StreamView* sv = dc.Find(kStreamEvent);
        if (!sv || !sv->records || sv->bytes <= capture::kBlockPrefixBytes) continue;
        bitio::BitReader r(sv->records + capture::kBlockPrefixBytes,
                           sv->bytes - capture::kBlockPrefixBytes);
        capture::EventDecoder dec;
        for (uint32_t k = 0; k < sv->recordCount; ++k) {
            capture::EventMessage m{};
            // A refused row leaves the reader off a record boundary; the rest of this chapter is lost.
            if (!dec.Decode(r, m, body.data(), body.size())) break;
            if (m.typeIndex != tapIndex) continue;
            uint64_t stated = 0;
            if (gametype::ParseOwnerStateGametype(reinterpret_cast<const char*>(m.body),
                                                  m.bodyBytes, &stated) &&
                gametype::IsFull(stated)) {
                return stated;
            }
        }
    }
    return 0;
}

uint64_t OwnerStateGametypeOfFile(const wchar_t* path) {
    if (path == nullptr || path[0] == L'\0') return 0;
    DemoReader reader;
    if (!reader.Open(path)) return 0;
    return OwnerStateGametype(reader);
}

}  // namespace playback
}  // namespace replay
