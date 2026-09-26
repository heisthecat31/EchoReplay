#pragma once

// The XAudio2 source voice under Wwise's sink, and the two proofs S10.4.2 requires before anyone
// is allowed to command it.
//
// ---------------------------------------------------------------------------------------------
// THE MECHANISM IN ONE LINE. CAkSinkXAudio2::IsDataNeeded returns
// `uNumRefillsInVoice - m_uBuffersInUse` and Wwise renders exactly that many frames per pass, so
// XAudio2's consumption rate gates the entire sound engine, upper and lower. SetFrequencyRatio(r)
// on the source voice therefore slows or speeds pitch, tempo, RTPC interpolation, fades, envelopes,
// reverb tails and music transitions together and coherently. Wwise 2019.1 added
// SetOfflineRendering to formalise this; its absence from this 2018 build never meant the clock
// was uncontrollable.
//
// ---------------------------------------------------------------------------------------------
// ★ THE VTABLE IS DECLARED, NOT OFFSET. The design forbids this packet from writing 0xD0
// by hand, and the reason is not tidiness: a hand-carried slot number that disagrees with the
// interface is a call through a real function pointer into the wrong method, which surfaces as a
// crash or as silence rather than as a compile error. So the vtable below is written out in the
// vendor's slot ORDER and the compiler places SetFrequencyRatio. The two facts the sink itself
// gives us -- DestroyVoice at +0x90 and IUnknown::Release at +0x10, both read out of
// CAkSinkXAudio2::Term -- are static_asserted against that layout, so the declaration is checked
// against the shipped bytes rather than trusted.
//
// ★ THE VOICE POINTER IS NEVER CACHED. ReplaceOutput re-enters CreateSourceVoice asynchronously
// and tears the device down unconditionally first, so any pointer read from sink+0x68 is
// invalidated by a device change or a hard mute -- and ReplaceOutput RETURNS BEFORE the new voice
// exists, so a pointer captured right after it returns may be the old one or null. Everything here
// re-reads the sink chain at each use.

#include <stddef.h>
#include <stdint.h>

#include "audio/akbind.h"
#include "audio/audio_facts.h"

namespace replay {
namespace audio {

using replay::audio_facts::kVoiceDestroyVoiceOff;

// XAUDIO2_VOICE_STATE. Stable across 2.7, 2.8 and 2.9.
struct XAudio2VoiceState {
    void* currentBufferContext;
    uint32_t buffersQueued;
    uint64_t samplesPlayed;
};

// XAUDIO2_VOICE_DETAILS, and this is the one struct whose shape moved: 2.8 and 2.9 carry an
// ActiveFlags dword that 2.7 does not. AUDIO.XAUDIO2_VER_SELECTOR says which runtime loaded, and
// SinkProbe reads it rather than guessing -- an unknown selector value refuses instead of picking.
struct XAudio2VoiceDetails28 {
    uint32_t creationFlags, activeFlags, inputChannels, inputSampleRate;
};
struct XAudio2VoiceDetails27 {
    uint32_t creationFlags, inputChannels, inputSampleRate;
};

// IXAudio2SourceVoice's vtable, in slot order. Slots this code does not call are void* on purpose:
// naming a method we never invoke would invite someone to invoke it through a signature nobody
// checked. The names are kept so the layout reads against the vendor header.
struct XAudio2SourceVoiceVtbl {
    // ---- IXAudio2Voice, slots 0..18 ----
    void(__stdcall* GetVoiceDetails)(void* self, void* out);   //  0
    void* SetOutputVoices;                                     //  1
    void* SetEffectChain;                                      //  2
    void* EnableEffect;                                        //  3
    void* DisableEffect;                                       //  4
    void* GetEffectState;                                      //  5
    void* SetEffectParameters;                                 //  6
    void* GetEffectParameters;                                 //  7
    void* SetFilterParameters;                                 //  8
    void* GetFilterParameters;                                 //  9
    void* SetOutputFilterParameters;                           // 10
    void* GetOutputFilterParameters;                           // 11
    void* SetVolume;                                           // 12
    void* GetVolume;                                           // 13
    void* SetChannelVolumes;                                   // 14
    void* GetChannelVolumes;                                   // 15
    void* SetOutputMatrix;                                     // 16
    void* GetOutputMatrix;                                     // 17
    void(__stdcall* DestroyVoice)(void* self);                 // 18
    // ---- IXAudio2SourceVoice, slots 19..28 ----
    void* Start;                                               // 19
    void* Stop;                                                // 20
    void* SubmitSourceBuffer;                                  // 21
    void* FlushSourceBuffers;                                  // 22
    void* Discontinuity;                                       // 23
    void* ExitLoop;                                            // 24
    // 2.7 declares GetState(state) and 2.8+ GetState(state, flags). On x64 there is one calling
    // convention and the callee ignores a register argument it does not declare, so the two-argument
    // form is correct for both and passing 0 is what 2.8's XAUDIO2_VOICE_NOSAMPLESPLAYED-clear means.
    void(__stdcall* GetState)(void* self, XAudio2VoiceState* out, uint32_t flags);  // 25
    int32_t(__stdcall* SetFrequencyRatio)(void* self, float ratio, uint32_t operationSet);  // 26
    void(__stdcall* GetFrequencyRatio)(void* self, float* out);                     // 27
    void* SetSourceSampleRate;                                 // 28
};

constexpr uint32_t kXAudio2CommitNow = 0;
// XAudio2's own clamp on SetFrequencyRatio's low end. Below this the call is refused, not scaled,
// so the rate ladder's floor is this and not zero.
constexpr float kXAudio2MinFreqRatio = 1.0f / 1024.0f;
// XAUDIO2_MAX_FREQ_RATIO. The interface's own ceiling, not a policy one: what a voice may actually
// hold. A reading outside [min, max] is not a frequency ratio at all, which is the layout claim
// this bound exists to falsify -- the value the sink happens to ship with is not that claim.
constexpr float kXAudio2MaxFreqRatio = 1024.0f;
// What CAkSinkXAudio2's voice reads before anyone commands it: CreateSourceVoice is called with
// MaxFrequencyRatio = 1.0f and the game never calls SetFrequencyRatio (DESIGN S10.4.1). It is the
// expected baseline, and expected is all it is -- see VoiceRatio.
constexpr float kShippedFrequencyRatio = 1.0f;

// Not an HRESULT the device returned, and not a refusal: the NOTICE that `baseline * rate` fell
// under the device's own floor and was clamped up to it, so the label can say so. Refusing instead
// -- which this used to do -- turned a rate the device could still serve into "SetFrequencyRatio
// was refused by every sink" for the rest of the session, about a call that was never made.
// Negative so it travels the same `firstError` channel, and outside the HRESULT facility range so
// it can never be mistaken for one.
constexpr int32_t kRatioBelowDeviceMinimum = -1;
// How close a reading has to be to count as the value we left in the field. XAudio2 stores what
// SetFrequencyRatio took, and the value that comes back out is not required to be the same bits;
// 1e-4 relative is 0.17 cents, inaudible, and far below any difference a second writer would make.
constexpr float kRatioMatchTolerance = 1.0e-4f;
bool NearlyEqualRatio(float a, float b);

struct XAudio2SourceVoice {
    const XAudio2SourceVoiceVtbl* vtbl;
};

// Why a sink was refused. Never "unavailable" on its own: S10.4.5's third row has to print a
// reason, and a reason that does not distinguish -noaudio from a layout mismatch is not one.
enum class SinkVerdict : uint8_t {
    Ok = 0,
    NoImage,             // GetModuleHandleW(nullptr) failed
    NoSinkList,          // AUDIO.SINK_LIST_PTR is null: -noaudio, or Wwise is not up yet (E-A2)
    SinkUnreadable,      // the sink pointer does not point at committed memory
    IfaceMismatch,       // proof 1: neither sink+0x50 nor +0x58 is the exported XAudio2 interface
    NoVoice,             // sink+0x68 is null; a device rebuild is in flight
    VoiceUnreadable,
    UnknownXAudio2Version,   // AUDIO.XAUDIO2_VER_SELECTOR is not 1, 2 or 3
    XAudio2ModuleMismatch,   // the selector names a version whose DLL is not the one mapped
    DetailsImplausible,  // proof 2: sample rate or channel count out of range
    RatioOutOfDomain,    // proof 2: GetFrequencyRatio returned a value XAudio2 cannot hold
    VoiceGone,           // it stopped being the voice ProveSink proved, between the proof and a use
    Count
};
// ★ A CLOSED SET, PUBLISHED VERBATIM. W2-R's rows print these strings as the reason a replay has
// no rate control, so they are an interface: total over the enum (no default arm -- a new verdict
// is a compile error here, never a silent "unknown"), and every one of them names a distinct
// cause. tests/audio asserts both properties over 0..Count.
const char* SinkVerdictName(SinkVerdict);

// ---- A6: does the selector agree with the runtime that is actually mapped ---------------------
//
// AUDIO.XAUDIO2_VER_SELECTOR decides how XAUDIO2_VOICE_DETAILS is read, and that is the ONE struct
// whose shape moved between 2.7 and 2.8. A selector that says 3 while XAudio2_7.dll is the mapped
// runtime reads `channels` out of the ActiveFlags slot and `rate` out of `channels` -- both land
// inside the plausibility window often enough to pass, so the failure is silent and the rate ladder
// is then commanded against numbers nobody measured. Range-checking the selector to 1..3 does not
// touch this: 3 is in range and still wrong.
//
// So the selector is checked against the loader, which is the one authority on what is mapped.
enum class XAudio2ModuleCheck : uint8_t {
    NotChecked,   // the selector was out of range; UnknownXAudio2Version owns that verdict
    Agrees,       // the module the selector names is mapped
    Mismatch,     // it is not, and a DIFFERENT XAudio2 is -- the layout assumption is wrong
    NoneMapped,   // none of the three is mapped under its own name: not disproved, not proved
};
const char* XAudio2ModuleCheckName(XAudio2ModuleCheck);

constexpr uint32_t kXAudio2Mapped27 = 1u << 0;
constexpr uint32_t kXAudio2Mapped28 = 1u << 1;
constexpr uint32_t kXAudio2Mapped29 = 1u << 2;

// GetModuleHandleW on the three names. Never LoadLibrary: the question is what this process already
// has, and loading one to find out would change the answer.
uint32_t MappedXAudio2Modules();

// Pure, and separated from the query so every combination is drivable on a machine with no game --
// including the ones a real process will not produce, which are the ones a wrong table would hide in.
XAudio2ModuleCheck ClassifyXAudio2Module(uint32_t selector, uint32_t mappedMask);

// One sink, proven. `voice` is valid only for as long as the caller holds no lock and issues no
// ReplaceOutput; re-run the probe rather than storing it.
struct ProvenSink {
    uintptr_t sink = 0;
    XAudio2SourceVoice* voice = nullptr;
    // The vtable ProveSink accepted. Kept so every later use can compare the voice's CURRENT
    // vtable against it: both ways this object can stop being the voice that was proven -- a
    // teardown that zeroes the field, and a rebuild that puts a different object at the same
    // address -- are visible in this one word, and neither is visible in the voice pointer.
    const XAudio2SourceVoiceVtbl* vtbl = nullptr;
    uint32_t inputChannels = 0;
    uint32_t inputSampleRate = 0;
    float frequencyRatio = 0.0f;
    // Which of the engine's up to eight outputs this sink is. CAkSinkXAudio2::Init derives the new
    // sink's slot as `(head->[0xC0] + 1) & 7`, so the field is an index in 0..7 and nothing more --
    // it does not name a device and this module never claims it does. It is printed at adopt time
    // to answer the one question the owner's log cannot: whether the sinks we pitch include the one
    // that is audible.
    uint32_t outputIndex = 0;
    // SinkGeneration(sink) as of the proof (ARCH-22 S3.1, narrowed by PITCH-FIX-2 S1). The vtable
    // identity above cannot see a sink torn down and rebuilt at the same address with the same
    // vtable -- sub_14139A640 does exactly that -- and this word can: the Term detour stamps the
    // sink it is tearing down, so a proof taken before THIS sink's teardown never matches a use
    // after it, and another sink's teardown does not move it at all.
    uint64_t generation = 0;
};

// The whole sink chain, proven, newest read. IsDataNeeded takes the minimum over every sink, so a
// caller that throttled only the first would starve any second output that happens to exist.
struct SinkChain {
    static constexpr size_t kMax = 8;   // AUDIO.SINK_SIZE bounds a sink at 224 bytes; 8 is headroom
    ProvenSink sinks[kMax];
    size_t count = 0;
    size_t rejected = 0;                // sinks that failed a proof, counted rather than skipped
    SinkVerdict firstRefusal = SinkVerdict::Ok;
    // The reading that produced `firstRefusal`, when the refusal was about a value rather than a
    // pointer. A diagnostic that names a comparison and not its operands cannot be diagnosed from
    // a launch: six runs re-derived this module from source because the log carried only the word.
    float firstRefusalRatio = 0.0f;
    // Reported as data even when the chain proved, because NoneMapped is a third answer: it is not
    // agreement and it is not disagreement, and collapsing it into either is how "we could not
    // establish it" gets read as "it holds".
    uint32_t versionSelector = 0;
    uint32_t mappedXAudio2 = 0;
    // Whether the loader was asked at all. A mask of 0 from a query that never ran and a mask of 0
    // from one that did are opposite findings -- "the selector is unset AND XAudio2_9 is mapped"
    // says the sink is mid-construction; "unset and nothing mapped" says it never started.
    bool mappedQueried = false;
    // AUDIO.SINK_LIST_PTR as read, recorded before any refusal can return. Whether a sink object
    // exists at all is the single most informative datum here and the refusal ordering used to
    // discard it.
    uintptr_t sinkListHead = 0;
    XAudio2ModuleCheck moduleCheck = XAudio2ModuleCheck::NotChecked;
};

// Reads AUDIO.SINK_LIST_PTR through the mapped image, walks SINK_NEXT_OFF, and applies both
// S10.4.2 proofs to each. `ak.GetWwiseXAudio2Interface` supplies proof 1's expected value; without
// it the probe refuses rather than skipping the proof, because a proof that can be skipped is not
// one. Returns the number of sinks that passed.
size_t ProbeSinks(const AkExports& ak, SinkChain* out);

// ---- the one guarded load ---------------------------------------------------------------------
//
// * EVERY CALL THROUGH A FOREIGN VTABLE GOES THROUGH HERE. The voice belongs to Wwise, not to us:
// CAkSinkXAudio2 creates it and destroys it from Wwise's own thread whenever the device is rebuilt,
// so a pointer ProveSink accepted microseconds ago can already be a torn-down object by the time a
// loop reaches it. L3 #8 crashed on exactly that -- `call [rax+0C8h]` with the vtable field read
// back as 0, in a warm-up that re-entered the read every single tick.
//
// Returns the voice's CURRENT vtable, and only while it is still the one that was proven. A zeroed
// field fails the null test; a voice rebuilt at the same address fails the identity test, which is
// the half a null check alone cannot do.
//
// Two loads and two compares, and deliberately NO Readable(): this runs per voice per tick and
// VirtualQuery on the tick path would cost more than the call it guards. The page proof belongs to
// ProveSink, which ran in the same call. What this cannot survive is the page being decommitted
// between that proof and this load -- that residue needs a Wwise lifetime hook, not a wider check.
const XAudio2SourceVoiceVtbl* LiveVtbl(const ProvenSink& proven);

// ---- what a sink that did not prove actually means -------------------------------------------
//
// CAkSinkXAudio2::Init is what writes both AUDIO.XAUDIO2_VER_SELECTOR and AUDIO.SINK_LIST_PTR, and
// both live in the zero-initialised tail of .data, so before that constructor runs they read 0 --
// "never written", not "written with nothing". A caller that asks once, early, and scores the
// answer has not measured a sink; it has measured its own timing. Two states cannot say that, so:
//
//   Proven        -- measure, and pass or fail on the measurement.
//   NeverAppeared -- the sink was not there to ask. NOT-RUN. Never a failed sink proof.
//   Refused       -- the sink IS there and disagrees with the design. That is a real failure.
//
// Pure, and separated from the wait for the same reason ClassifyXAudio2Module is: every row is
// drivable on a machine with no game, including the ones a real process will not produce.
enum class SinkWaitOutcome : uint8_t {
    Proven,
    NeverAppeared,
    Refused,
};
const char* SinkWaitOutcomeName(SinkWaitOutcome);
SinkWaitOutcome ClassifySinkWait(size_t proven, SinkVerdict lastRefusal);

// ---- per-voice ratio bookkeeping -------------------------------------------------------------
//
// ★ THE REPLAY'S RATE IS A MULTIPLIER, NOT A VALUE. Whatever a source voice reads the first time
// it is proven is the ENGINE's ratio for that voice; the rate composes with it, so the value
// commanded is `baseline * rate` and 1.0x puts the baseline back exactly. Overwriting the field
// with the rate would retune any voice the engine had pitched -- and, because the field is then no
// longer the value it shipped with, would make the voice fail its OWN proof on the very next
// probe. That is not hypothetical: an "exactly 1.0f" proof plus a command that overwrote it is
// why rate control worked once per session and then reported `GetFrequencyRatio is not exactly
// 1.0f` forever (L3-RUN-4 launch 2: `slowed with picture` at 0.50x, `unavailable` at every rate
// after it).
// ★ AND THE BASELINE IS A CONSTANT, NOT A MEASUREMENT (PITCH-FIX-2 S2, the invariant). Everything
// above describes a design that LEARNED the baseline by reading the voice, and every version of
// that design ratchets, because the reading is our own residue. The engine settles it: a
// CAkSinkXAudio2 source voice is created with MaxFrequencyRatio = 1.0f (AUDIO.SINK_MAXFREQRATIO,
// read out of echovr.exe) and no call site in the image ever invokes SetFrequencyRatio on it. Its
// ratio is 1.0 for the whole life of the voice unless WE move it. So `baseline` is
// kShippedFrequencyRatio, always, on first sight and after every teardown, and a read-back can
// never set it -- not once, not as a fallback, not for a voice we have never met.
//
// The owner's 2026-09-20 log is what the learned baseline costs: `0.1000 -> 0.0500 -> 0.0125`,
// the 0.29-0.31 cluster that outlived five minutes and six sinks, and a fast-forward that wrote
// 0.0373 under a panel reading "audio stays at 1.0x".
//
// A read-back is still taken, and it is still real information, for exactly two things: the log
// line the owner reads, and DETECTING a value that is neither ours nor the shipped one. In this
// title the read is ONE PROCESSING PASS BEHIND (the log prints `wrote 0.5000, voice reads 0.2500`
// inline), so a single disagreeing sample is never drift -- see CheckChainDrift.
struct VoiceRatio {
    const void* voice = nullptr;
    float baseline = kShippedFrequencyRatio;   // the engine's own ratio: the constant, never a read
    float commanded = kShippedFrequencyRatio;  // what WE last asked this voice for
    float prevCommanded = kShippedFrequencyRatio;   // the ask before it: a read-back may be a pass behind
    float observed = kShippedFrequencyRatio;   // what the field read after the last command
    // What this voice can actually hold. Initialised from the design fact and raised only by a
    // read-back that PROVES a higher value took, so an ask above it is clamped by us, counted, and
    // never scored as drift -- the seven `expected 1.00xx found 1.0000` lines and all eight rail
    // disengages in the owner's log are one voice clamping 1.0039 to 1.0 and us calling it drift.
    float maxRatio = kShippedFrequencyRatio;
    // The previous drift check's reading, and how many consecutive checks have found the same
    // wrong value. Two are required before a voice is judged, because one is the lag.
    float lastRead = kShippedFrequencyRatio;
    uint8_t driftStreak = 0;
    uint64_t generation = 0;                   // SinkGeneration() of the sink this describes
    bool present = true;                       // in the newest proven chain
};

// True when `live` is a value this module PUT IN THE FIELD: the current ask, or the one before it
// (the device applies on its own pass, so a read can be one command behind). Deliberately not
// `observed`: that is what we READ, and in this title the read is a pass late, so a value some
// other writer left behind lands in `observed` on the very next command. Calling it ours from then
// on is how a foreign number launders itself into the ledger.
bool RatioIsOurs(const VoiceRatio& entry, float live);

// Bounded by the sink chain itself: a voice no longer in the chain has been destroyed, and its
// record says nothing about whatever Wwise builds at the same address next.
class VoiceRatioLedger {
public:
    const VoiceRatio* Find(const void* voice) const;
    VoiceRatio* FindMutable(const void* voice);
    // ★ NO BASELINE ARGUMENT. There is nothing for a caller to supply: the baseline of a Wwise sink
    // voice is kShippedFrequencyRatio by construction, and a parameter here is a place for a
    // reading to get in. Null when the table is full of voices with writes still outstanding.
    VoiceRatio* Adopt(const ProvenSink& sink);
    void Observe(const void* voice, float ratio);
    // Marks every entry absent from `chain` rather than erasing it, and retires the ones whose
    // voice reappeared under a new generation. See the note on VoiceRatio.
    void RetainOnly(const SinkChain& chain);
    // Drops one entry. What a restore that reached this voice owes the ledger, and nothing more:
    // clearing the whole table would take the baselines of the voices the same restore MISSED.
    void Forget(const void* voice);
    void Reset();
    size_t Count() const { return count_; }
    const VoiceRatio& At(size_t i) const { return entries_[i]; }

private:
    VoiceRatio entries_[SinkChain::kMax];
    size_t count_ = 0;
};

// The process-wide ledger the shipped probe and the shipped command both consult.
VoiceRatioLedger& SinkRatioLedger();

// Proof 2's second half, pure, so every case is drivable with no game.
//
// This is the WHOLE of the ratio proof, and the reduction is deliberate. The layout claim -- that
// this float is the frequency-ratio field -- is falsified by a reading XAudio2 could not hold, and
// by nothing else. Requiring the field to still read what WE last commanded is a behavioural
// expectation about who else writes it, and as a gate it is unsatisfiable: a voice the engine
// rebuilt reads 1.0f, which is neither our observation nor a baseline the ledger snapshotted from
// one of our own commands, so the sink is refused for having gone back to normal -- permanently,
// because every re-arm re-reads the same voice. Whoever wrote the field, the value in it is the
// engine's ratio now, and ApplyRatioToChain composes with it instead of refusing it.
SinkVerdict CheckVoiceRatioDomain(float ratio);

// Commands `kShippedFrequencyRatio * rate`, clamped into [kXAudio2MinFreqRatio, the voice's own
// maxRatio], on every voice in `chain`, and reads each value back FOR THE LOG. Returns the number
// that accepted it -- including the voices already sitting at the value, which are counted and not
// written: at 1.0x with every voice confirmed at 1.0 this function issues no call at all.
// `voicesGone` counts the sinks skipped because LiveVtbl refused them -- the caller needs those
// apart from the ones that refused the command, because they mean the chain is stale rather than
// that the device said no.
// A product under kXAudio2MinFreqRatio is CLAMPED to the floor and commanded, not skipped: the
// voice ends up as slow as the device can go, `firstError` carries kRatioBelowDeviceMinimum as a
// notice, and the caller's label says so. `clampedToFloor` counts them.
size_t ApplyRatioToChain(const SinkChain& chain, float rate, VoiceRatioLedger& ledger,
                         int32_t* firstError, size_t* voicesGone = nullptr,
                         size_t* clampedToFloor = nullptr);

// What the chain reads against what the ledger says we commanded. `rate` is the replay rate the
// module believes is in force, so the expectation per voice is `baseline * rate`. Voices we have
// never commanded are not judged -- their ratio is the engine's business. Returns the number that
// differ by more than kRatioMatchTolerance.
struct SinkRatioDrift {
    uintptr_t sink = 0;
    float expected = 0.0f;
    float live = 0.0f;
};
// ★ LAG-AWARE, AND IT TAKES THE LEDGER BY REFERENCE FOR THAT REASON (PITCH-FIX-2 S4). The read is
// one processing pass behind in this title, so a reading equal to the ask BEFORE the current one is
// not drift, and a single disagreeing sample is never enough: the same wrong value has to come back
// twice running. The streak lives in the row, which is why this is not a const method -- a drift
// check that could not remember its last sample had to adjudicate on one, and adjudicating on one
// manufactured drift at every rate change, pushed the closed loop to its rail, and disengaged it.
// `expected` is clamped to the voice's own maxRatio, so a clamp is never scored as a difference.
// ★ AND `rate` IS THE CURRENT INTENT, NOT THE LAST ASK THAT WAS FULLY DELIVERED (AUDIO-FIX-3 A).
// A rate change that reached only part of the chain used to leave the caller's `rate` at the old
// value, and then every voice that DID take the new one read as drift: `expected 1.0001 found
// 0.1000`, four of the seven drift lines in the 2026-09-21 session, every one of them about our own
// correct write. The caller owns the other half of that: it does not run this check inside
// kRatioReadLagMs of one of its own rate changes, because in that window the field still holds the
// previous ask.
size_t CheckChainDrift(const SinkChain& chain, VoiceRatioLedger& ledger, float rate,
                       SinkRatioDrift* worst);

// ---- what the module can prove about its own baselines ----------------------------------------
//
// DIAG-PITCH-2 S6.5's proving line, as data. `relearned` is the invariant, counted over the LIVE
// rows rather than asserted: it is the number of baselines that are not the shipped constant, and
// under S2 there is no code path that can make it non-zero. Printing it is what makes that
// checkable from a log instead of from a code reading.
struct SinkRatioCensus {
    size_t voices = 0;
    float baselines[SinkChain::kMax] = {};
    size_t baselineCounts[SinkChain::kMax] = {};
    size_t distinctBaselines = 0;
    size_t relearned = 0;
    size_t outstanding = 0;
    size_t clampedAtMax = 0;      // cumulative: asks the voice's MaxFrequencyRatio cut down
    size_t foreignWriters = 0;    // cumulative: readings that were nobody's ask and nobody's clamp
    size_t retiredOwnTerm = 0;    // cumulative: rows dropped because THAT sink went through Term
    size_t retiredChainAbsent = 0;   // cumulative: rows reused because the voice left the chain
    size_t adoptNoticesDropped = 0;
    uint64_t generation = 0;      // VoiceGeneration(): how many Terms this process has seen, plus 1
};
SinkRatioCensus SinkRatioCensusNow(const VoiceRatioLedger& ledger, float rate);
// One line, in the shape DIAG-PITCH-2 S6.5 specified. Returns the length written.
size_t FormatSinkRatioCensus(char* out, size_t cap, const SinkRatioCensus& census);

// Once per voice, when the ledger first meets it. The output index answers the one question the
// owner's log cannot -- whether the sinks we pitch include the audible one -- and the channel
// count and sample rate are the only other things about the output this module has proven.
struct SinkAdoptNotice {
    uintptr_t sink = 0;
    const void* voice = nullptr;
    uint32_t outputIndex = 0;
    uint32_t channels = 0;
    uint32_t sampleRate = 0;
    float maxRatio = kShippedFrequencyRatio;
};
size_t DrainSinkAdoptNotices(SinkAdoptNotice* out, size_t max);
constexpr size_t kMaxAdoptNotices = 16;

// True for a voice the ledger says is not at `baseline * rate` -- i.e. one whose last write did not
// land, whether it was refused, or never attempted because the probe found no sinks. Pure ledger
// arithmetic: no probe, no COM call, so the reconciler can ask this every tick.
size_t OutstandingVoices(const VoiceRatioLedger& ledger, float rate);

// Puts every tracked voice in `chain` back to the ratio the engine had. Returns the number
// restored. `forget` retires the entries this call PUT BACK (and the ones whose voice is gone),
// which is right when the module is handing the sink back for good; the voices a failed probe or a
// refused write never reached keep their baselines either way, because a baseline nothing in the
// process still holds is a voice left at the replay's pitch for the rest of its life.
size_t RestoreChainRatios(const SinkChain& chain, VoiceRatioLedger& ledger, int32_t* firstError,
                          size_t* voicesGone = nullptr, bool forget = true);

// Re-probes, then composes. `rate` is the replay's rate, not a ratio: 1.0f means "the pitch the
// engine chose", which is why this is still one call for every caller. Re-probes internally: this
// is the call site the "never cache the voice" rule exists for. `firstError` gets the HRESULT of
// the first voice that refused.
size_t ApplyFrequencyRatio(const AkExports& ak, float rate, int32_t* firstError,
                           size_t* voicesGone = nullptr);

// Re-probes, then restores. Called when the module gives the sink back, so the game is left at the
// pitch it shipped with rather than at whatever rate the replay stopped on -- and, with
// `forget = false`, at every return to 1.0x, where the restore has to be ABSOLUTE. Commanding
// `baseline * 1.0` instead only re-asserts whatever the ledger currently believes the baseline is.
size_t RestoreFrequencyRatio(const AkExports& ak, int32_t* firstError, bool forget = true);

// ---- what was actually written, per sink voice ------------------------------------------------
//
// ★ ONE SLOT FOR A WHOLE CHAIN IS AN INSTRUMENT THAT LIES. This used to be a single global
// LastSinkRatioWrite holding the FIRST voice a command reached, and the restore wrote 1.0 over it
// -- so a chain of two voices reported one of them, a slow-down that landed on the second voice
// reported nothing, and a restore of one voice erased another voice's baseline from the log while
// leaving the sink itself pitched. A log that cannot name which voice it is about cannot answer
// "is the audio still slowed", which is the only question it exists for.
//
// So a record per sink, and the reader drains them: one line per rate change per sink voice.
struct SinkRatioWrite {
    uintptr_t sink = 0;       // the sink object the voice hangs off -- the id the log line carries
    float rate = 0.0f;        // the replay rate this composed with; 1.0 on a restore
    float baseline = 0.0f;    // the engine's own ratio for this voice, as of this command
    float written = 0.0f;     // what SetFrequencyRatio was asked for
    float readBack = 0.0f;    // what GetFrequencyRatio returned afterwards: a clamp shows here
};

// What the coalescing swallowed since the last drain. Counted rather than dropped silently,
// because a command stream faster than the drain is exactly the case the single slot used to hide.
struct SinkRatioWriteCounts {
    uint32_t superseded = 0;   // a newer write for the same sink replaced one not yet drained
    uint32_t dropped = 0;      // more distinct sinks wrote than there are slots to hold them
};

// Drains the newest undrained write PER SINK into `out` (at most `max`, and the table holds at
// most SinkChain::kMax), clearing the table. Returns how many were written; `counts`, when given,
// reports what was coalesced away. Newest-per-sink is the bound: a pathological stream of commands
// between two drains costs one line per sink voice, not one per command.
size_t DrainSinkRatioWrites(SinkRatioWrite* out, size_t max, SinkRatioWriteCounts* counts = nullptr);

// The read half, over a chain the caller already proved. Separated from the probe so the availability
// state machine above it is drivable with no game -- and so the torn-down-voice path, which is the
// one that crashed, has a case rather than a hope. False when no voice in the chain was still live:
// a sum over nothing is not a reading of zero, and feeding one to the bias reads as a measurement.
bool ReadChainSamplesPlayed(const SinkChain& chain, uint64_t* samplesPlayed, uint32_t* buffersQueued,
                            size_t* voicesGone = nullptr);

// O4b: XAudio2's own domain, below Wwise entirely. Sums samplesPlayed over the proven sinks so a
// device that is not consuming at the commanded rate is distinguishable from a Wwise that is
// starved upstream (S12.4). Returns false when no sink could be proven.
bool ReadSamplesPlayed(const AkExports& ak, uint64_t* samplesPlayed, uint32_t* buffersQueued,
                       size_t* voicesGone = nullptr);

// ---- the layout cross-check ------------------------------------------------------------------
// ★ The first assert is the load-bearing one, and it is a real comparison of two independent
// derivations: the left side is where the compiler placed slot 18 given the vendor's declaration
// order, and the right side is a displacement decoded out of the shipped bytes of
// CAkSinkXAudio2::Term, where the sink calls DestroyVoice on its own mastering voice. They agree
// only if IXAudio2Voice really has 19 slots in this order, which is the assumption every later
// slot inherits. Delete a slot above and this stops compiling.
static_assert(offsetof(XAudio2SourceVoiceVtbl, DestroyVoice) == audio_facts::kVoiceDestroyVoiceOff,
              "the declared slot order must place DestroyVoice where the sink's own call finds it");
// The second is a TRIPWIRE, not a second derivation, and saying so matters: the game never calls
// SetFrequencyRatio, so there is no encoded operand in the image to compare against. This restates
// The design's slot so that reordering the declaration above is a compile error rather than
// a silent change of which method the rate ladder dispatches to.
static_assert(offsetof(XAudio2SourceVoiceVtbl, SetFrequencyRatio) == 0xD0,
              "SetFrequencyRatio is slot 26; 0xD0 is written here to be CHECKED, never to be used");
// The read half, pinned for the same reason and with the same standing: the ledger's whole
// read-back model rests on what THIS slot returns, so a reordered declaration must be a compile
// error rather than a quiet change of which method the drift check interrogates.
static_assert(offsetof(XAudio2SourceVoiceVtbl, GetFrequencyRatio) == 0xD8,
              "GetFrequencyRatio is slot 27, immediately above SetFrequencyRatio's 0xD0");
static_assert(sizeof(XAudio2SourceVoiceVtbl) == 29 * sizeof(void*),
              "IXAudio2SourceVoice has 29 slots: 19 from IXAudio2Voice plus 10 of its own");

}  // namespace audio
}  // namespace replay
