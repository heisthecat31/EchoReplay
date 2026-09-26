#pragma once

// S_VOIP -- the replay-side header stub and the injection contract W3-C builds on.
//
// ==============================================================================================
// THE RECORD IS `capture::VoipRec`, AND THIS HEADER NO LONGER STATES A SECOND ONE
// ==============================================================================================
//
// This file used to carry an 18-byte `VoipRecordHeader` with its own encoder and cursor. It was a
// second definition of one on-disk record: `chapterbuilder` writes W2-A's 16-byte
// `capture::VoipRec` (src/capture/capture_types.h), the accounting counts that, and the codec here
// was exercised by nothing but its own tests.
//
// ★ ORCHESTRATOR RULING: `VoipRec` is the format. The definition the shipped writer writes beats
// one only its author's tests read, so `VoipRecordHeader`, `EncodeVoipRecord`, `NextVoipRecord` and
// `VoipReadCursor` are DELETED rather than labelled scratch -- scratch code in this tree gets
// adopted later by someone who does not know it was scratch.
//
// ★★ AND THE FIELD THAT DECIDES CORRECTNESS SURVIVES INTACT. `VoipRec::speakerSlot` is a `u32`
// carrying an `SR15NetUserId` -- `{u16 userSlot, u16 generation}` -- and BOTH HALVES ARE
// LOAD-BEARING. The engine's VoIP receiver selects its target from the pool handle's LOW WORD ALONE
// and never validates the generation, so a handle recorded in one session resolves in a later
// session to whoever now holds that slot: voice from the wrong player, no crash, every test green.
// The generation is what makes that detectable, so the recorded id is stored whole and the live
// handle is re-derived from the running tables at injection time (W3-C's voipplay.cpp does exactly
// this; the recorded generation survives as the diagnostic that says the two disagreed).
//
// ==============================================================================================
// THE INJECTION POINT, RECONCILED WITH W3-C's READING
// ==============================================================================================
//
// The capture point is the injection point: `CR15NetVoipReceiverCS::ReceiveVoip` (VOIP.RECEIVE).
// Replaying there inherits the jitter buffer, the Loudness Gator, the mute rules and 3D positional
// placement for free.
//
// ★ THIS HEADER PREVIOUSLY GUESSED AT TWO THINGS AND W3-C SETTLED BOTH. It named two candidate
// routes to the receiver pointer -- IEntity's component lookup on the local actor, or latching the
// `this` our own capture detour observes -- and it called the second argument a `talkerKey`.
// docs/spikes/W3-C.md S1 resolves it from `libr15.so`'s retained symbol:
//
//     CR15NetVoipReceiverCS::ReceiveVoip(SCompactPoolHandle, SR15NetUserVoipEvent const&,
//                                        void const*, unsigned int)
//
//   * `this` is the component SYSTEM, one per gamespace, reached through
//     `CncaGameSpace::FindComponentSystem` on symbol VOIP.RECEIVER_CS_SYMBOL
//     (0x7A06E9E827234EB0) -- NEITHER of the routes named here. It needs no detour, which is what
//     matters during a replay: no network delivers, so a detour would never fire and a latched
//     pointer would never exist.
//   * argument two is an `SCompactPoolHandle` -- `{u16 poolIndex, u16 generation}` -- not an opaque
//     key. Same 32 bits, same ABI, different meaning: it is re-derived per injection from the live
//     pool tables and is never replayed from the file.
//
// The parameter below is therefore spelled `handle`, and the parked read that asked for the
// detour-latched pointer is withdrawn -- W3-C's own `voip-latched-this-vs-resolved-cs` carries the
// remaining cross-check.
//
// ★ THE HEADER STUB IS SIX BYTES, AND IT IS A READ RATHER THAN A GUESS. Across the whole function
// the third argument is dereferenced at exactly one displacement -- `movzx eax, word ptr [r13+4]`,
// six times, and nothing else. tests/audio's recvguard arm disassembles VOIP.RECEIVE out of the
// shipped binary and asserts that the set of displacements read off that register is exactly {4};
// plant a read at +8 and it goes red. So the replay header is a zeroed stub with the sequence word
// at +4, and no part of the real packet header has to be stored.
//
// ★ ON THE PAYLOAD BOUND. The design's prose says "<= 256 B" while its record spells the
// length `u8`, which cannot hold 256 -- the stated bound is exactly the value the field cannot
// represent. The engine takes the length as a `u32` from its caller with no static cap anywhere, so
// kMaxPayload below is a bound past which a length is evidence that we are reading something other
// than a voice frame; frames past it are DROPPED AND COUNTED, never truncated.

#include <stddef.h>
#include <stdint.h>

namespace replay {
namespace audio {

// Four times the largest payload S5.4.6 describes. Not a belief about the codec -- a bound past
// which a length is evidence that we are reading something other than a voice frame.
constexpr uint32_t kMaxPayload = 1024;

// The stream's own cadence: 48 kHz in 960-sample / 20 ms frames, so fifty packets a second. It is a
// property of the stream and not part of the deleted record format, and it has a real consumer --
// voipplay.h derives kMaxContiguousSpanUs from it rather than picking a duration, which is what
// makes that threshold move with the codec instead of drifting away from it.
constexpr uint32_t kVoipPacketsPerSecond = 50;

// The jitter buffer's sequence modulus. ReceiveVoip masks the header word with 0x3FF everywhere it
// touches it, so storing more than ten bits would store noise.
constexpr uint16_t kSeqMask = 0x3FF;

// ---- the replay-side header stub ---------------------------------------------------------------
// Sixteen zeroed bytes with the sequence at +4. Only +4 is read; the rest is zeroed rather than left
// uninitialised so that if this build's reading is ever wrong, the callee reads a zero and not a
// stack value that happens to look plausible.
struct VoipHeaderStub {
    uint8_t bytes[16];
};
VoipHeaderStub MakeHeaderStub(uint16_t seq);

// ---- injection ----------------------------------------------------------------------------------
// ★ OWNERSHIP. W3-C owns src/audio/voip/: the MinHook detour on VOIP.RECEIVE that captures, and the
// resolution of the receiver component system at playback. This header is the seam between us: W2-H
// fixes the stub and the call shape; W3-C supplies the two pointers, the hook, and the per-injection
// handle it re-derives from the live pool tables.
struct VoipInjector {
    // VOIP.RECEIVE, guarded by its 20-byte prologue before it is called. `handle` is an
    // SCompactPoolHandle passed by value in a register, which is why it is spelled as a u32 here.
    void (*receiveVoip)(void* receiver, uint32_t handle, const void* header, const void* payload,
                        uint32_t length) = nullptr;
    // The CR15NetVoipReceiverCS component SYSTEM for the live gamespace. Run-local, so it can never
    // come from the file, and it must be re-resolved after any session change.
    void* receiver = nullptr;

    bool Ready() const { return receiveVoip != nullptr && receiver != nullptr; }
};

// Hands one recorded frame back to the engine at its own injection point. False when the injector
// is not ready or the length is implausible; it never partially injects.
bool InjectVoip(const VoipInjector&, uint32_t handle, uint16_t seq, const void* payload,
                uint16_t length);

}  // namespace audio
}  // namespace replay
