#pragma once

// W3-C. The MinHook detour on VOIP.RECEIVE, and the seam it hands frames through.
//
// ---------------------------------------------------------------------------------------------
// ★ THE TAP DOES NOT STAMP, AND THAT IS DELIBERATE.
//
// Every other capture tap receives its time from its caller -- OnNetvarMessage takes a stampUs,
// OnTapMessage takes the sender's stamp out of the message. Voice has no wire stamp we can see, so
// the honest choice is between inventing a second clock in here and letting the capture worker,
// which already owns the recording's time axis, stamp it. A second clock measuring the same frames
// as the first is how a drift nobody can attribute gets introduced, so this tap hands over
// {speaker, sequence, payload} and nothing else.
//
// ★ AND IT RECORDS BEFORE THE ENGINE'S GATES, NOT AFTER.
//
// VOIP.RECEIVE refuses a frame when the local viewer has muted that player. That is a property of
// the person watching, not of the match, and baking it into the file would make the recording
// unrepairable: a replay applies the CURRENT viewer's mute list through the same engine path, so a
// frame dropped at capture is gone for everyone forever while a frame kept is still muted for
// anyone who wants it muted. The privacy control is `capture.voip` (ledger row 2.3) and
// `echodemo strip --voip`, both of which remove the whole section rather than a subset of it.
//
// ★ AND IT LATCHES `this`, WHICH IS THE CONTROL ON THE PLAYBACK ROUTE.
//
// Playback cannot latch anything -- no network delivers, so no detour fires -- and must reach the
// receiver through the component-system lookup. LatchedCs() is the only place the two routes can be
// compared against ground truth, and runs/queue/payload/W3-C.json's first read is exactly that
// comparison. It is recorded here so the comparison is possible, not because injection uses it.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include "bootstrap/hookmgr.h"

namespace replay {

class ILog;

namespace audio {
namespace voip {

// What the detour hands the capture worker. `speakerSlot` is the engine's SR15NetUserId --
// {u16 userSlot, u16 generation} -- which is what capture::VoipRec's u32 field carries: its low
// half is the manifest's durable slot and its high half is the run-local witness a mismatch shows
// up in. kUnresolvedSpeaker means the CI could not be read; it is NEVER a slot.
constexpr uint32_t kUnresolvedSpeaker = 0xFFFFFFFFu;

// True when the frame was accepted. A false return is counted as `droppedSinkRefused` and the frame
// is dropped rather than retried: the engine is mid-call and the payload buffer is the caller's.
using VoipSink = bool (*)(uint32_t speakerSlot, uint16_t seq, const void* payload, uint32_t bytes,
                          void* ctx);

// Installed by whoever owns the recording's ring and time axis (capture). Absent by default, and
// its absence is COUNTED rather than assumed: a tap that silently dropped everything because
// nobody had wired it would be indistinguishable from a match with no talking.
void SetVoipSink(VoipSink sink, void* ctx);

// ★ Totals, not just what was stored. A capture that saw 900 frames and stored 512 must not
// produce a file indistinguishable from one where 512 was the whole truth, so every disposition
// has its own counter and `observed` is the sum of all of them.
struct VoipCaptureStats {
    uint64_t observed = 0;
    uint64_t stored = 0;
    uint64_t droppedNoSink = 0;
    uint64_t droppedSinkRefused = 0;
    uint64_t droppedOversize = 0;      // length past kMaxPayload; never truncated
    uint64_t droppedNoPayload = 0;     // a non-zero length with an unreadable buffer
    uint64_t droppedNoHeader = 0;      // the sequence word at header+4 could not be read
    uint64_t unresolvedSpeaker = 0;    // stored, but with kUnresolvedSpeaker
    // Over ACCEPTED lengths only; the refused ones are their own number below. `min` is
    // 0xFFFFFFFF until the first accepted frame -- the absence of a measurement, which must not
    // read as a measurement of zero.
    uint32_t maxPayloadSeen = 0;
    uint32_t minPayloadSeen = 0xFFFFFFFFu;
    uint32_t maxLengthRefused = 0;     // the largest length we turned away, for the parked read
};

VoipCaptureStats CaptureStats();

// The `this` the detour observed, and the last handle it was called with. Null / zero until a
// frame has arrived, which is a real state and not an error.
void*    LatchedCs();
uint32_t LatchedHandle();

// Builds the trampoline. Safe from DllMain; Arm separately on the worker thread, as B-4 requires.
// The site is guarded by VOIP.RECEIVE's recorded 20-byte prologue, so another mod already sitting
// on it is a refusal and a log line rather than an overwrite.
HookStatus CreateVoipCapture();
HookStatus ArmVoipCapture();
HookStatus VoipCaptureStatus();

// The original VOIP.RECEIVE, past our own detour. Null until CreateVoipCapture has built a
// trampoline.
//
// ★★ INJECTION MUST GO THROUGH THIS WHENEVER THE CAPTURE HOOK EXISTS. The site address IS our
// detour once the hook is armed, so injecting through it re-enters the tap and records every
// injected frame as though the network had delivered it -- a loop that, in the one session that
// both records and replays, writes the replay's own voice back into the recording. Playing through
// the trampoline is the difference between replaying voice and duplicating it.
void* ReceiveVoipOriginal();

// The fact id the hook is registered under, so a report can name the site rather than describe it.
extern const char* const kVoipReceiveFactId;

// Test seam. Drives the detour body with synthetic arguments and a caller-supplied component
// system, without a hook, a game or a trampoline -- so the resolution, the counters and every
// refusal path are exercised offline. Returns what the detour decided.
enum class TapVerdict : uint8_t {
    Stored = 0,
    NoSink,
    SinkRefused,
    Oversize,
    NoPayload,
    NoHeader,
    StoredUnresolved,
};
const char* TapVerdictText(TapVerdict);
TapVerdict TapForTest(void* cs, uint32_t handle, const void* header, const void* payload,
                      uint32_t length);
void ResetForTest();

}  // namespace voip
}  // namespace audio
}  // namespace replay
