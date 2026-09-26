// The gametype a capture states in its own SNSLobbyOwnerState event.
//
// Captures written before the recorder kept the whole gametype qword carry an unusable manifest
// value, but the lobby's owner-state JSON on S_EVENT states the gametype at full width. This reads
// it back so such a capture can still be entered as what it was.

#ifndef REPLAY_PLAYBACK_CORE_ENTRY_GAMETYPE_H
#define REPLAY_PLAYBACK_CORE_ENTRY_GAMETYPE_H

#include <stdint.h>

namespace replay {

class IDemoReader;

namespace playback {

// The first whole gametype an SNSLobbyOwnerState row states, or 0 when the capture has none.
uint64_t OwnerStateGametype(IDemoReader& reader);

// The same, for a capture that is not open yet. 0 when the file cannot be read.
uint64_t OwnerStateGametypeOfFile(const wchar_t* path);

}  // namespace playback
}  // namespace replay

#endif  // REPLAY_PLAYBACK_CORE_ENTRY_GAMETYPE_H
