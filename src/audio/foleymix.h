#pragma once

// AUDIO-CUTOFF-FIX. Why far player foley is audible in a replay and silent in a live spectate.
//
// Every remote-foley event in the arena bank (wrist thrusters 1721745643, grab/attach 3303547815,
// the per-pawn bed 771335902, 746861552, ...) resolves to a Wwise SWITCH CONTAINER keyed on the
// global STATE group 3126088779 with two children (bank 0xB2AFE679127B4904, HIRC parsed offline):
//
//   state "false" (2452206122)  the in-headset child: attenuation reaches SILENCE at 10 m for the
//                               thrusters, 7 m for grabs, 15 m for the per-pawn beds.
//   state "true"  (3053630529)  the broadcast child: 40 m curves that FLOOR at -19 dB, a bus
//                               override and two aux sends -- "far away, echoy, never silent".
//
// The game sets that state from the level script c7a0b2702adf162b, which posts bank event
// 4148162267 (SetState -> true) on delegate_onspectatorcamerasystemenabled and 2756389524
// (-> false) on delegate_onspectatorcamerasystemdisabled. The engine raises those from
// CAM.SPECTATOR_ENABLE_SYSTEM / CAM.SPECTATOR_DISABLE_SYSTEM, chosen each in-match tick for a
// spectator-team user by the Oculus session status (sub_1407305C0: headset mounted -> DISABLE).
// A person spectating a live match in the headset therefore hears the "false" child; a replay
// runs the plugin's spectator camera rig with the system enabled and hears the "true" child.
//
// The bank action sets the state inside Wwise and never passes through CSoundPlayer::SetState, so
// it is invisible to the engine's state memo, to the pin block and to every census taken so far.
// This hold feeds Wwise the value a live headset spectator has, through the same SetState the
// bank action performs, and re-asserts it if the engine's own enable/disable flips it back.

#include <stdint.h>

#include "bootstrap/services.h"
#include "engine/audiostate.h"

namespace replay::audio {

constexpr uint32_t kFoleyMixStateGroup = 3126088779u;
constexpr uint32_t kFoleyMixHeadsetState = 2452206122u;    // Wwise FNV-1 of "false"
constexpr uint32_t kFoleyMixBroadcastState = 3053630529u;  // Wwise FNV-1 of "true"
constexpr uint64_t kFoleyMixPollMs = 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale), not the CR15Game vtable slot

enum class FoleyMix : uint8_t {
    Engine,     // leave the state to the engine (the pre-fix behaviour)
    Headset,    // hold "false": what a live spectator wearing the headset hears
    Broadcast,  // hold "true": the spectator-stream mix
};

// "engine" | "headset" | "broadcast", case-sensitive. Returns false and leaves *out untouched for
// anything else, so a typo in config.json is refused rather than silently mapped.
bool ParseFoleyMix(const char* text, FoleyMix* out);
const char* FoleyMixName(FoleyMix mix);

constexpr const char* kFoleyMixKey = "audio.foley_mix";

// The `audio.foley_mix` row as the config holds it. False, with *out = Engine, when the word is not one
// of the three; *text is the word read either way.
bool FoleyMixFromConfig(const IConfig* cfg, FoleyMix* out, const char** text);

enum class FoleyMixOutcome : uint8_t {
    Idle,       // not due, or mix == Engine
    Held,       // Wwise already answered the wanted state
    Corrected,  // Wwise answered something else and SetState succeeded
    Refused,    // no binding, GetState failed, or SetState failed
};

struct FoleyMixReport {
    uint32_t polls = 0;
    uint32_t corrections = 0;
    uint32_t refusals = 0;
    uint32_t lastSeen = 0;  // the value Wwise answered on the last poll that was not refused
};

class FoleyMixHold {
public:
    explicit FoleyMixHold(FoleyMix mix, uint64_t pollIntervalMs = kFoleyMixPollMs);

    FoleyMix Mix() const { return mix_; }
    uint32_t WantedState() const;

    // Drive once per tick while a replay is loaded and engine writes are allowed. The first call
    // after construction or Reset polls immediately; later calls poll once per interval.
    FoleyMixOutcome Tick(IAudioState* state, uint64_t nowMs);

    // Forget the poll clock so the next Tick asserts at once (a demo was unloaded or loaded).
    void Reset();

    const FoleyMixReport& Report() const { return report_; }

private:
    FoleyMix mix_;
    uint64_t pollIntervalMs_;
    uint64_t nextPollMs_ = 0;
    bool polled_ = false;
    FoleyMixReport report_{};
};

}  // namespace replay::audio
