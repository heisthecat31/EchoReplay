#pragma once
// Recorded voice: each player's Opus frames (48 kHz mono, 20 ms) decoded with Windows' own Opus decoder
// (Media Foundation, MSOpusDecoder) and played through XAudio2, one voice per speaker, in step with playback.

#include <string>

#include "replay.h"

namespace erv::voice {

bool Init();                       // false: no audio device or no Opus decoder (Why() says which)
const std::string& Why();
bool Ready();

// Call once a frame. `t` is the part's local time; `speed` the playback rate (voice plays from 0.5x to 2x,
// forwards only). Frames between the last call and `t` are decoded and queued per speaker; a jump in time
// (a seek, a part change) drops what is queued and resets the decoders.
// `ear` / `right`: the camera, for positional voice (each speaker panned and faded by where their head is).
void Update(const Part& part, double t, double speed, bool playing, V3 ear, V3 right);
extern bool positional;
void Stop();

// Mixer.
extern float volume;               // 0..2
extern bool muted;
void SetPlayerMuted(uint32_t slot, bool muted);
bool PlayerMuted(uint32_t slot);
// Per player loudness in the last ~100 ms (0..1), for a level meter.
float Level(uint32_t slot);

// Testing: decode every frame of one player to a 16-bit wav. Returns frames decoded (-1 on failure).
int DecodeToWav(const VoiceTrack& p, const std::wstring& wav, std::string& why);

}  // namespace erv::voice
