#pragma once

// Mirrors the W2-H fact drop, which W0-2 merges into facts/facts.tsv. Once that merge has
// happened, configure with -DAUDIO_FACTS_MERGED and every value below becomes a static_assert
// against the generated registry instead of a second copy of it.
//
// ★ WHAT IS NOT HERE, AND WHY THAT IS THE POINT. Every AK:: entry point this subsystem calls is a
// PE export resolved by name in akbind.cpp. None of them appears here or in the fact drop.
// The design's "the control surface is offset-free" is a property to be preserved, not a
// slogan: adding an address for an exported function would create a second way to reach it that
// nobody would notice had gone stale.
//
// The values below are the ones with no export behind them. Sixteen of them were already in
// facts.tsv when this packet started -- W1-G's static sweep landed them -- and those are cited from
// facts:: rather than copied, because a fact that exists is never transcribed.

#include <stdint.h>

#include "generated/facts_generated.h"

namespace replay::audio_facts {

// ---- cited from the registry; NOT copied -----------------------------------------------------
inline constexpr uint64_t kImageBase = facts::PE_IMAGE_BASE;

// The sink chain. AUDIO.SINK_LIST_PTR holds the first CAkSinkXAudio2; SINK_NEXT_OFF chains them.
// IsDataNeeded takes the MINIMUM over the whole list, so throttling one sink starves the others --
// which is why this is a walk and not a single read (S10.4.1).
inline constexpr uint64_t kSinkListPtrVa = facts::AUDIO_SINK_LIST_PTR;
inline constexpr uint32_t kSinkNextOff = facts::AUDIO_SINK_NEXT_OFF;
inline constexpr uint32_t kSinkIfaceOffA = facts::AUDIO_SINK_IFACE_OFF_A;      // 0x50
inline constexpr uint32_t kSinkIfaceOffB = facts::AUDIO_SINK_IFACE_OFF_B;      // 0x58
inline constexpr uint32_t kSinkMasteringOff = facts::AUDIO_SINK_MASTERING_OFF;  // 0x60
inline constexpr uint32_t kSinkVoiceOff = facts::AUDIO_SINK_VOICE_OFF;          // 0x68
inline constexpr uint32_t kSinkBuffersInUseOff = facts::AUDIO_SINK_BUFFERSINUSE_OFF;
// Row 2.6's site. CAkSinkXAudio2::Term, and the twenty prologue bytes hookmgr's guard compares
// the mapped image against. BOTH come from the registry: an address transcribed into C++ would be
// a second copy nobody would notice had gone stale, and a wrong byte here would be a detour written
// into the middle of an instruction rather than an install refusal.
inline constexpr uint64_t kSinkTermVa = facts::AUDIO_SINK_TERM;
inline constexpr const uint8_t* kSinkTermPrologue = facts::AUDIO_SINK_TERM_PROLOGUE;
inline constexpr size_t kSinkTermPrologueLen = sizeof facts::AUDIO_SINK_TERM_PROLOGUE;

inline constexpr uint32_t kSinkSize = facts::AUDIO_SINK_SIZE;                   // 0xE0, the bound

// The output slot CAkSinkXAudio2::Init gives a new sink: it reads the list head's field at 0xC0,
// adds one and masks to three bits, so the field is an index in 0..7 and names no device. NOT A
// REGISTRY ROW, and the difference is deliberate -- it is derived in DIAG-PITCH-2 S1 from the Init
// decompile and nobody has re-read it out of echovr.exe, so it would be withheld as PC_PRESENT
// even if it were contributed. It steers nothing: its one consumer prints it at adopt time so a
// clean ratio census with a still-unhappy owner can be answered by asking whether the sinks we
// pitch include the audible one. The mask is applied by the reader, so a wrong offset can print a
// wrong index and never an unbounded one.
inline constexpr uint32_t kSinkOutputIndexOff = 0xC0u;   // disp32 in sub_1414013D0, the `& 7` site
inline constexpr uint32_t kSinkOutputIndexMask = 0x7u;
static_assert(kSinkOutputIndexOff + sizeof(uint32_t) <= kSinkSize,
              "the output index must lie inside the bound ProveSink proves readable");

// Which XAudio2 the process loaded: 3 = XAudio2_9, 2 = _8, 1 = _7. It decides how
// XAUDIO2_VOICE_DETAILS is laid out, which is the one struct whose shape differs between them.
inline constexpr uint64_t kXAudio2VersionSelectorVa = facts::AUDIO_XAUDIO2_VER_SELECTOR;

// The buffering S10.4.1a's A/V bias is computed from. Both are read back at RUN TIME from
// GetAudioSettings and GetState; these are the shipped defaults, used only to sanity-bound a
// runtime reading, never as the reading itself.
inline constexpr uint32_t kShippedSamplesPerFrame = facts::AUDIO_SAMPLESPERFRAME;   // 512
inline constexpr uint32_t kShippedRefillsInVoice = facts::AUDIO_NUMREFILLSINVOICE;  // 8

// N-16's memo. SetState writes {u32 group, u32 value} pairs into a sorted vector at
// CSoundPlayer + MEMO_CACHE_OFF and returns success without calling Wwise when the pair is already
// there, which is what makes a backwards seek permanently wrong.
inline constexpr uint32_t kMemoCacheOff = facts::SOUND_MEMO_CACHE_OFF;   // 0x338

// ---- pending in the W2-H fact drop -------------------------------------------------------
// Each was decoded out of echovr.exe by tools/w2h/gen_w2h_facts.py, which aborts rather than emit
// a row whose decode disagrees with the value. The comment names the site so the two can be
// compared without opening the drop.

// CSoundPlayer* = *(void**)( *(void**)kSoundSystemsPtrVa + kSoundPlayerSlotOff ).
//
// ★ CITED, NOT RESTATED. 0x1420A0470 is ONE global -- the CEngine pointer -- read by five
// `mov rax, cs:qword_1420A0470` sites; the camera node manager indexes it at one slot and
// CSoundPlayer at kSoundPlayerSlotOff. It was already in the registry, so W2-H's own
// SOUND.SYSTEMS_PTR_VA row was a second id for one fact and is withdrawn: one fact, one id.
// This line was a bare literal until the drop became readable, and the duplicate could not
// be seen while nothing in that drop reached a guard.
inline constexpr uint64_t kSoundSystemsPtrVa = facts::ENGINE_CENGINE_PTR;
inline constexpr uint32_t kSoundPlayerSlotOff = 0x2CF80u;       // disp32 @0x14106A633

// The memo container: data pointer at +0, live count at +kMemoCountOff, 8-byte entries.
inline constexpr uint32_t kMemoCountOff = 0x30u;    // disp8 @0x14020EE52 (mov rbp,[rsi+30h])
inline constexpr uint32_t kMemoEntrySize = 8u;      // lea rdx,[rax+r14*8] in the same insert path

// The per-emitter switch memo hangs off the emitter record the lookup returns, and has the same
// container shape as the global one -- which is why one walker serves both.
inline constexpr uint64_t kEmitterLookup = 0x140208FD0ull;      // funcstart
inline constexpr uint32_t kEmitterSwitchMemoOff = 0x78u;        // disp8 @0x14020EFDD
inline constexpr uint32_t kEmitterMapOff = 0x60u;               // disp8 @0x140209024
inline constexpr uint32_t kEmitterMapCountOff = 0x90u;          // disp32 @0x140209018
inline constexpr uint32_t kEmitterMapStride = 16u;              // mov eax,[rdi+rax*8] with rax = idx*2
inline constexpr uint32_t kEmitterLockOff = 0x58u;              // disp8 @0x140208FE8

// Nonzero and SOUND.SETSTATE / SOUND.SETSWITCH return SUCCESS without touching Wwise. A reconcile
// that ran against this set would report every write as having taken and change nothing.
inline constexpr uint64_t kSoundDisableFlagVa = 0x14201F2E0ull;  // riprel @0x14020EE1B

// CSoundCS::SaveLoadGlobal. Not called; cited because its load arm is the engine's own answer to
// "what is the complete global audio state", and this design's pin block carries exactly that set.
inline constexpr uint64_t kSaveLoadGlobal = 0x140401630ull;

// VOIP.RECEIVE's third argument is dereferenced at exactly one displacement, +4, as a 16-bit read,
// at all six of its sites. That is what makes a six-byte replay header sufficient.
inline constexpr uint32_t kVoipRecvSeqOff = 4u;      // disp8 @0x140D377E3
inline constexpr uint16_t kVoipSeqMask = 0x3FFu;     // the jitter buffer's sequence modulus

// exported GetAudioSettings writes uNumSamplesPerFrame at +0 and uNumSamplesPerSecond at +4.
inline constexpr uint32_t kAkAudioSettingsRateOff = 4u;  // disp8 @0x14138D23E

// The two XAudio2 vtable slots the sink itself calls, read out of CAkSinkXAudio2::Term. They are
// here to be COMPARED against the vtable struct sinkvoice.h declares from the vendor's slot order:
// two derivations, one static_assert. The design forbids reintroducing a hand-written 0xD0,
// and this is what makes that enforceable rather than a promise.
inline constexpr uint32_t kVoiceDestroyVoiceOff = 0x90u;   // disp32 @0x141401F3D
inline constexpr uint32_t kUnknownReleaseOff = 0x10u;      // disp8  @0x141401F53

}  // namespace replay::audio_facts
