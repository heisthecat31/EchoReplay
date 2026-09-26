#include "audio/sinkvoice.h"

#include <stdio.h>

#include "audio/voicelifetime.h"

#include "audio/memprobe.h"
#include "bootstrap/buildguard.h"

namespace replay {
namespace audio {
namespace {

using namespace replay::audio_facts;

// Proof 2's plausibility window. Deliberately wide: this is asking "did we read a voice or did we
// read whatever happens to be at +0x68", not "is this device configured the way we expect". A
// narrow window here would refuse a legitimate 96 kHz or 7.1 endpoint.
constexpr uint32_t kMinSampleRate = 8000;
constexpr uint32_t kMaxSampleRate = 192000;
constexpr uint32_t kMinChannels = 1;
constexpr uint32_t kMaxChannels = 8;

bool ReadVoiceDetails(XAudio2SourceVoice* voice, const XAudio2SourceVoiceVtbl* vtbl,
                      uint32_t version, uint32_t* channels, uint32_t* rate) {
    // Zero first: the callee writes only the fields its own version declares, and a stale stack
    // value read back as a sample rate would pass the plausibility window by accident.
    uint32_t buf[4] = {0, 0, 0, 0};
    vtbl->GetVoiceDetails(voice, buf);
    if (version >= 2) {                 // XAudio2_8 and _9 carry ActiveFlags at index 1
        *channels = buf[2];
        *rate = buf[3];
    } else {                            // XAudio2_7 does not
        *channels = buf[1];
        *rate = buf[2];
    }
    return true;
}

// Applies both S10.4.2 proofs to one sink. `expectedIface` is the value the exported
// GetWwiseXAudio2Interface returned; a null one is not a pass, it is a refusal.
SinkVerdict ProveSink(uintptr_t sink, const void* expectedIface, uint32_t version,
                      ProvenSink* out) {
    // Read BEFORE the proof's own two calls, and carried out in ProvenSink. A teardown that starts
    // between here and the last of them makes this proof stale, and the VoiceCall below is what
    // says so -- the proof is not allowed to trust the pointer it has just read any more than a
    // later use is.
    const uint64_t generation = SinkGeneration(sink);
    if (!Readable(reinterpret_cast<const void*>(sink), kSinkSize)) {
        return SinkVerdict::SinkUnreadable;
    }

    // Proof 1. The sink holds the IXAudio2 interface at one of two fields, and CAkSinkXAudio2::Term
    // calls IUnknown::Release through the first of them -- which is what says it is a COM interface
    // and not an unrelated pointer.
    const void* ifaceA = *reinterpret_cast<void* const*>(sink + kSinkIfaceOffA);
    const void* ifaceB = *reinterpret_cast<void* const*>(sink + kSinkIfaceOffB);
    if (expectedIface == nullptr || (ifaceA != expectedIface && ifaceB != expectedIface)) {
        return SinkVerdict::IfaceMismatch;
    }

    auto* voice = *reinterpret_cast<XAudio2SourceVoice* const*>(sink + kSinkVoiceOff);
    if (voice == nullptr) {
        return SinkVerdict::NoVoice;     // a device rebuild is in flight; E-A1 re-arms us
    }
    if (!Readable(voice, sizeof(void*))) {
        return SinkVerdict::VoiceUnreadable;
    }
    // Loaded ONCE and used everywhere below, including by the caller through ProvenSink::vtbl. A
    // second load could see a different value than the one this proof read, which is the whole
    // defect one level up: proving one pointer and calling through another.
    const XAudio2SourceVoiceVtbl* vtbl = voice->vtbl;
    if (!Readable(vtbl, sizeof(XAudio2SourceVoiceVtbl))) {
        return SinkVerdict::VoiceUnreadable;
    }

    ProvenSink candidate;
    candidate.sink = sink;
    candidate.voice = voice;
    candidate.vtbl = vtbl;
    candidate.generation = generation;

    uint32_t channels = 0;
    uint32_t rate = 0;
    {
        VoiceCall call(candidate);
        if (!call.Ok()) {
            return SinkVerdict::VoiceGone;
        }
        ReadVoiceDetails(voice, call.Vtbl(), version, &channels, &rate);
    }
    if (channels < kMinChannels || channels > kMaxChannels || rate < kMinSampleRate ||
        rate > kMaxSampleRate) {
        return SinkVerdict::DetailsImplausible;
    }

    // Proof 2's second half: the field must hold something XAudio2 could hold. It must NOT be
    // required to equal anything we commanded -- see CheckVoiceRatioDomain. The reading is written
    // out before the refusal so the caller can report the operand and not just the word.
    float ratio = 0.0f;
    {
        VoiceCall call(candidate);
        if (!call.Ok()) {
            return SinkVerdict::VoiceGone;
        }
        call.Vtbl()->GetFrequencyRatio(voice, &ratio);
    }
    out->frequencyRatio = ratio;
    const SinkVerdict ratioVerdict = CheckVoiceRatioDomain(ratio);
    if (ratioVerdict != SinkVerdict::Ok) {
        return ratioVerdict;
    }

    out->sink = sink;
    out->voice = voice;
    out->vtbl = vtbl;
    out->inputChannels = channels;
    out->inputSampleRate = rate;
    // Inside kSinkSize, which Readable() checked above, and read as data -- no call, no vtable. It
    // is masked to the 0..7 Init itself derives rather than trusted: this row is tier U, it drives
    // nothing but a log line, and a wrong offset must print a wrong index, never an unbounded one.
    out->outputIndex =
        *reinterpret_cast<const uint32_t*>(sink + kSinkOutputIndexOff) & kSinkOutputIndexMask;
    out->generation = generation;
    return SinkVerdict::Ok;
}

}  // namespace

const char* SinkVerdictName(SinkVerdict v) {
    switch (v) {
        case SinkVerdict::Ok: return "ok";
        case SinkVerdict::NoImage: return "main image not resolvable";
        case SinkVerdict::NoSinkList: return "no Wwise XAudio2 sink (-noaudio, or Wwise is not up)";
        case SinkVerdict::SinkUnreadable: return "the sink pointer is not committed memory";
        case SinkVerdict::IfaceMismatch:
            return "neither sink+0x50 nor sink+0x58 is the exported XAudio2 interface";
        case SinkVerdict::NoVoice: return "sink+0x68 is null; a device rebuild is in flight";
        case SinkVerdict::VoiceUnreadable: return "the source voice or its vtable is not readable";
        case SinkVerdict::UnknownXAudio2Version:
            return "the XAudio2 version selector is not 1, 2 or 3";
        case SinkVerdict::XAudio2ModuleMismatch:
            return "the XAudio2 version selector names a runtime that is not the one mapped, so "
                   "GetVoiceDetails would be read with the wrong layout";
        case SinkVerdict::DetailsImplausible:
            return "GetVoiceDetails reported an implausible sample rate or channel count";
        case SinkVerdict::RatioOutOfDomain:
            return "GetFrequencyRatio returned a value outside XAudio2's own ratio range, so this "
                   "is not the frequency ratio field";
        case SinkVerdict::VoiceGone:
            return "sink gone: the source voice was torn down between the proof and the use";
        case SinkVerdict::Count:
            break;
    }
    return "unknown";
}

const char* XAudio2ModuleCheckName(XAudio2ModuleCheck c) {
    switch (c) {
        case XAudio2ModuleCheck::NotChecked: return "not checked";
        case XAudio2ModuleCheck::Agrees: return "agrees with the mapped runtime";
        case XAudio2ModuleCheck::Mismatch: return "DISAGREES with the mapped runtime";
        case XAudio2ModuleCheck::NoneMapped:
            return "no XAudio2 runtime is mapped under its own name, so the selector is neither "
                   "confirmed nor contradicted";
        default: return "unknown";
    }
}

uint32_t MappedXAudio2Modules() {
    uint32_t mask = 0;
#if defined(_WIN32)
    if (GetModuleHandleW(L"XAudio2_7.dll") != nullptr) {
        mask |= kXAudio2Mapped27;
    }
    if (GetModuleHandleW(L"XAudio2_8.dll") != nullptr) {
        mask |= kXAudio2Mapped28;
    }
    if (GetModuleHandleW(L"XAudio2_9.dll") != nullptr) {
        mask |= kXAudio2Mapped29;
    }
#endif
    return mask;
}

XAudio2ModuleCheck ClassifyXAudio2Module(uint32_t selector, uint32_t mappedMask) {
    if (selector < 1 || selector > 3) {
        return XAudio2ModuleCheck::NotChecked;
    }
    if (mappedMask == 0) {
        return XAudio2ModuleCheck::NoneMapped;
    }
    // 1 -> _7, 2 -> _8, 3 -> _9, which is the selector's own encoding and not a second table.
    const uint32_t wanted = 1u << (selector - 1);
    return (mappedMask & wanted) != 0 ? XAudio2ModuleCheck::Agrees : XAudio2ModuleCheck::Mismatch;
}

const char* SinkWaitOutcomeName(SinkWaitOutcome outcome) {
    switch (outcome) {
        case SinkWaitOutcome::Proven: return "proven";
        case SinkWaitOutcome::NeverAppeared: return "never-appeared";
        case SinkWaitOutcome::Refused: return "refused";
        default: return "unknown";
    }
}

SinkWaitOutcome ClassifySinkWait(size_t proven, SinkVerdict lastRefusal) {
    if (proven > 0) {
        return SinkWaitOutcome::Proven;
    }
    // Every verdict is named, and there is no default: a new SinkVerdict has to be classified here
    // deliberately rather than inheriting whichever arm the last author happened to write.
    switch (lastRefusal) {
        // The sink was not there to ask. Ok with nothing proven belongs here too -- nothing was
        // refused and nothing was proven, which is an empty answer and not a failed one.
        case SinkVerdict::Ok:
        case SinkVerdict::NoImage:
        case SinkVerdict::NoSinkList:
        case SinkVerdict::UnknownXAudio2Version:
            return SinkWaitOutcome::NeverAppeared;
        // A sink object exists and something about it contradicts the design. SinkUnreadable and
        // NoVoice are in this arm because both require a non-null list head: a chain that is still
        // being built passes through them, which is why the caller must hold them to a deadline
        // rather than to a single poll, but one that is still in them at the deadline is a finding.
        case SinkVerdict::SinkUnreadable:
        case SinkVerdict::IfaceMismatch:
        case SinkVerdict::NoVoice:
        case SinkVerdict::VoiceUnreadable:
        case SinkVerdict::XAudio2ModuleMismatch:
        case SinkVerdict::DetailsImplausible:
        case SinkVerdict::RatioOutOfDomain:
        case SinkVerdict::VoiceGone:
            return SinkWaitOutcome::Refused;
        case SinkVerdict::Count:
            break;
    }
    return SinkWaitOutcome::Refused;
}

size_t ProbeSinks(const AkExports& ak, SinkChain* out) {
    if (out == nullptr) {
        return 0;
    }
    *out = SinkChain{};

    const void* listSlot = buildguard::MappedAddress(kSinkListPtrVa);
    const void* versionSlot = buildguard::MappedAddress(kXAudio2VersionSelectorVa);
    if (listSlot == nullptr || versionSlot == nullptr) {
        out->firstRefusal = SinkVerdict::NoImage;
        return 0;
    }
    if (!Readable(listSlot, sizeof(void*)) || !Readable(versionSlot, sizeof(uint32_t))) {
        out->firstRefusal = SinkVerdict::NoSinkList;
        return 0;
    }

    // Everything readable is read BEFORE the first refusal can return, because each of these three
    // answers a different question and the refusal ordering used to throw two of them away: a run
    // that refused on the selector reported an empty module mask and a zero list head that nobody
    // had looked at. The ordering of the refusals below is unchanged -- only the reads moved up.
    const uint32_t version = *static_cast<const uint32_t*>(versionSlot);
    out->versionSelector = version;
    out->sinkListHead = *reinterpret_cast<const uintptr_t*>(listSlot);
    out->mappedXAudio2 = MappedXAudio2Modules();
    out->mappedQueried = true;

    if (version < 1 || version > 3) {
        out->firstRefusal = SinkVerdict::UnknownXAudio2Version;
        return 0;
    }

    // A6. Refused only on a positive disagreement: NoneMapped is not evidence against the selector
    // and must not disable rate control over a packaging difference nobody has seen. It is carried
    // out in the chain instead, so a caller that treats "we could not check" as "checked" is doing
    // that visibly.
    out->moduleCheck = ClassifyXAudio2Module(version, out->mappedXAudio2);
    if (out->moduleCheck == XAudio2ModuleCheck::Mismatch) {
        out->firstRefusal = SinkVerdict::XAudio2ModuleMismatch;
        return 0;
    }

    uintptr_t sink = out->sinkListHead;
    if (sink == 0) {
        out->firstRefusal = SinkVerdict::NoSinkList;     // E-A2: -noaudio disables cleanly
        return 0;
    }

    // GetWwiseXAudio2Interface is the only thing that can supply proof 1's expected value. Without
    // it there is no proof, and no proof means no rate control -- not "assume it is fine".
    const void* expectedIface = ak.GetWwiseXAudio2Interface ? ak.GetWwiseXAudio2Interface() : nullptr;

    for (size_t i = 0; i < SinkChain::kMax && sink != 0; ++i) {
        ProvenSink proven;
        const SinkVerdict verdict = ProveSink(sink, expectedIface, version, &proven);
        if (verdict == SinkVerdict::Ok) {
            out->sinks[out->count++] = proven;
        } else {
            ++out->rejected;
            if (out->firstRefusal == SinkVerdict::Ok) {
                out->firstRefusal = verdict;
                out->firstRefusalRatio = proven.frequencyRatio;
            }
        }

        if (!Readable(reinterpret_cast<const void*>(sink + kSinkNextOff), sizeof(uintptr_t))) {
            break;
        }
        const uintptr_t next = *reinterpret_cast<const uintptr_t*>(sink + kSinkNextOff);
        if (next == sink) {
            break;      // a self-link is a terminator in some builds and a bug in none of them
        }
        sink = next;
    }
    return out->count;
}

// Cumulative over the process, because the census is a rate and its denominator is the session.
// All five run on the game thread that drives the transport, like the write table below.
struct RatioCounters {
    size_t clampedAtMax = 0;
    size_t foreignWriters = 0;
    size_t retiredOwnTerm = 0;
    size_t retiredChainAbsent = 0;
    size_t adoptNoticesDropped = 0;
};
static RatioCounters g_ratioCounters;

struct AdoptNoticeTable {
    SinkAdoptNotice notices[kMaxAdoptNotices];
    size_t count = 0;
};
static AdoptNoticeTable g_adoptNotices;

static void NoteSinkAdopt(const ProvenSink& sink, float maxRatio) {
    if (g_adoptNotices.count >= kMaxAdoptNotices) {
        ++g_ratioCounters.adoptNoticesDropped;
        return;
    }
    g_adoptNotices.notices[g_adoptNotices.count++] =
        SinkAdoptNotice{sink.sink,           sink.voice, sink.outputIndex,
                        sink.inputChannels,  sink.inputSampleRate, maxRatio};
}

size_t DrainSinkAdoptNotices(SinkAdoptNotice* out, size_t max) {
    const size_t fits = out == nullptr ? 0 : max;
    const size_t n = g_adoptNotices.count < fits ? g_adoptNotices.count : fits;
    for (size_t i = 0; i < n; ++i) {
        out[i] = g_adoptNotices.notices[i];
    }
    g_ratioCounters.adoptNoticesDropped += g_adoptNotices.count - n;
    g_adoptNotices = AdoptNoticeTable();
    return n;
}

const VoiceRatio* VoiceRatioLedger::Find(const void* voice) const {
    for (size_t i = 0; i < count_; ++i) {
        if (entries_[i].voice == voice) {
            return &entries_[i];
        }
    }
    return nullptr;
}

VoiceRatio* VoiceRatioLedger::FindMutable(const void* voice) {
    for (size_t i = 0; i < count_; ++i) {
        if (entries_[i].voice == voice) {
            return &entries_[i];
        }
    }
    return nullptr;
}

VoiceRatio* VoiceRatioLedger::Adopt(const ProvenSink& sink) {
    for (size_t i = 0; i < count_; ++i) {
        if (entries_[i].voice == sink.voice) {
            entries_[i].present = true;
            entries_[i].generation = sink.generation;
            return &entries_[i];
        }
    }
    size_t slot = count_;
    if (slot >= SinkChain::kMax) {
        // Full, because absences no longer free a slot. Reuse an absent entry with nothing of ours
        // outstanding -- its baseline has already been put back, so losing it costs nothing.
        slot = SinkChain::kMax;
        for (size_t i = 0; i < count_; ++i) {
            if (!entries_[i].present && entries_[i].commanded == entries_[i].baseline) {
                slot = i;
                break;
            }
        }
        if (slot == SinkChain::kMax) {
            return nullptr;
        }
        ++g_ratioCounters.retiredChainAbsent;
    } else {
        ++count_;
    }
    VoiceRatio& e = entries_[slot];
    e = VoiceRatio{};
    e.voice = sink.voice;
    e.generation = sink.generation;
    NoteSinkAdopt(sink, e.maxRatio);
    return &e;
}

void VoiceRatioLedger::Observe(const void* voice, float ratio) {
    for (size_t i = 0; i < count_; ++i) {
        if (entries_[i].voice == voice) {
            entries_[i].prevCommanded = entries_[i].commanded;
            entries_[i].commanded = ratio;
            entries_[i].observed = ratio;
            return;
        }
    }
}

void VoiceRatioLedger::RetainOnly(const SinkChain& chain) {
    size_t kept = 0;
    for (size_t i = 0; i < count_; ++i) {
        VoiceRatio entry = entries_[i];
        const ProvenSink* sink = nullptr;
        for (size_t j = 0; j < chain.count; ++j) {
            if (chain.sinks[j].voice == entry.voice) {
                sink = &chain.sinks[j];
                break;
            }
        }
        if (sink != nullptr && sink->generation != entry.generation) {
            // THIS sink went through Term since the row was made -- the stamp is per sink now, so
            // no other sink's teardown can reach this comparison. Before S1 it could, and one
            // teardown anywhere retired every live voice's row at once.
            ++g_ratioCounters.retiredOwnTerm;
            continue;
        }
        entry.present = sink != nullptr;
        entries_[kept++] = entry;
    }
    count_ = kept;
}

void VoiceRatioLedger::Forget(const void* voice) {
    for (size_t i = 0; i < count_; ++i) {
        if (entries_[i].voice != voice) {
            continue;
        }
        for (size_t j = i + 1; j < count_; ++j) {
            entries_[j - 1] = entries_[j];
        }
        --count_;
        return;
    }
}

void VoiceRatioLedger::Reset() {
    count_ = 0;
}

VoiceRatioLedger& SinkRatioLedger() {
    static VoiceRatioLedger ledger;
    return ledger;
}

bool RatioIsOurs(const VoiceRatio& entry, float live) {
    return NearlyEqualRatio(live, entry.commanded) || NearlyEqualRatio(live, entry.prevCommanded);
}

// One place writes the three ratio fields, because they are one fact: what we asked for, what we
// asked for last time, and what the field read afterwards. Splitting them is how `observed` came to
// hold a value nobody had commanded.
// It is also the one place a pending drift verdict can be invalidated, because a write is the only
// thing that makes the streak's samples describe a question nobody is asking any more. A rate
// change writes every voice it reaches, so "a rate change invalidates the verdict" needs no second
// mechanism -- and a voice the rate change could NOT reach keeps its streak, which is the voice the
// check is for.
static void RecordCommand(VoiceRatio* record, float commanded, float readBack) {
    record->prevCommanded = record->commanded;
    record->commanded = commanded;
    record->observed = readBack;
    record->driftStreak = 0;   // lastRead goes with it: the streak gate is what reads it
}

bool NearlyEqualRatio(float a, float b) {
    const float diff = a > b ? a - b : b - a;
    const float mag = a > b ? a : b;
    const float scale = mag > 1.0f ? mag : 1.0f;
    return diff <= kRatioMatchTolerance * scale;
}

const XAudio2SourceVoiceVtbl* LiveVtbl(const ProvenSink& proven) {
    XAudio2SourceVoice* voice = proven.voice;
    if (voice == nullptr || proven.vtbl == nullptr) {
        return nullptr;
    }
    const XAudio2SourceVoiceVtbl* live = voice->vtbl;
    // Not `live != nullptr`: equality with the proven vtable subsumes the null test and refuses the
    // rebuilt-at-the-same-address voice too, which a null test would wave through.
    return live == proven.vtbl ? live : nullptr;
}

// The newest undrained write per sink. Bounded by the chain itself -- one rate change touches each
// sink at most once, so a full table means the drain is behind, not that the chain grew. No lock,
// because every write and every drain run on the game thread that drives the transport.
struct RatioWriteTable {
    SinkRatioWrite writes[SinkChain::kMax];
    size_t count = 0;
    SinkRatioWriteCounts counts;
};
static RatioWriteTable g_ratioWrites;

static void NoteSinkRatioWrite(uintptr_t sink, float rate, float baseline, float written,
                               float readBack) {
    const SinkRatioWrite w{sink, rate, baseline, written, readBack};
    for (size_t i = 0; i < g_ratioWrites.count; ++i) {
        if (g_ratioWrites.writes[i].sink == sink) {
            ++g_ratioWrites.counts.superseded;
            g_ratioWrites.writes[i] = w;
            return;
        }
    }
    if (g_ratioWrites.count >= SinkChain::kMax) {
        ++g_ratioWrites.counts.dropped;
        return;
    }
    g_ratioWrites.writes[g_ratioWrites.count++] = w;
}

size_t DrainSinkRatioWrites(SinkRatioWrite* out, size_t max, SinkRatioWriteCounts* counts) {
    const size_t fits = out == nullptr ? 0 : max;
    const size_t n = g_ratioWrites.count < fits ? g_ratioWrites.count : fits;
    for (size_t i = 0; i < n; ++i) {
        out[i] = g_ratioWrites.writes[i];
    }
    if (counts != nullptr) {
        *counts = g_ratioWrites.counts;
        // A record the caller had no room for is as gone as one the table refused, and it is the
        // same finding: the report is short of the writes that were made.
        counts->dropped += (uint32_t)(g_ratioWrites.count - n);
    }
    g_ratioWrites = RatioWriteTable();
    return n;
}

SinkVerdict CheckVoiceRatioDomain(float ratio) {
    // Written as a positive range test so a NaN -- which compares false against everything -- is
    // refused rather than admitted by a negated comparison.
    if (!(ratio >= kXAudio2MinFreqRatio && ratio <= kXAudio2MaxFreqRatio)) {
        return SinkVerdict::RatioOutOfDomain;
    }
    return SinkVerdict::Ok;
}

size_t ApplyRatioToChain(const SinkChain& chain, float rate, VoiceRatioLedger& ledger,
                         int32_t* firstError, size_t* voicesGone, size_t* clampedToFloor) {
    if (firstError != nullptr) {
        *firstError = 0;
    }
    if (voicesGone != nullptr) {
        *voicesGone = 0;
    }
    if (clampedToFloor != nullptr) {
        *clampedToFloor = 0;
    }
    if (!AudioWritesEnabled()) {
        return 0;   // `audio.writes_enabled = false`: slow-mo pitch is simply not applied
    }
    size_t clamped = 0;
    ledger.RetainOnly(chain);
    size_t applied = 0;
    size_t gone = 0;
    for (size_t i = 0; i < chain.count; ++i) {
        XAudio2SourceVoice* voice = chain.sinks[i].voice;
        // Re-proved before EACH of the three calls, not once for the group: the voice can go away
        // between the read and the write as easily as before the read, and a vtable that was live
        // one call ago is not evidence about this one.
        VoiceCall call1(chain.sinks[i]);
        const XAudio2SourceVoiceVtbl* vtbl = call1.Vtbl();
        if (vtbl == nullptr) {
            ++gone;
            continue;
        }
        // ★ NOTHING IS READ TO DECIDE THE TARGET. The baseline is the constant the engine created
        // this voice with; the only thing a pre-write read ever contributed was our own residue
        // becoming "the engine's pitch". The row is taken first because taking it is now free of
        // any reading -- there is no ordering hazard left for it to sit behind.
        VoiceRatio* record = ledger.Adopt(chain.sinks[i]);
        if (record == nullptr) {
            continue;   // the table is full of voices with our writes still outstanding
        }
        // Clamped up to the floor, never skipped. Refusing here reported a call that was never made
        // as "refused by every sink" and latched the module unavailable for the session; the device
        // can still play at its own minimum, and the label says that is what it is doing.
        float target = record->baseline * rate;
        if (target < kXAudio2MinFreqRatio) {
            target = kXAudio2MinFreqRatio;
            ++clamped;
            if (firstError != nullptr && *firstError == 0) {
                *firstError = kRatioBelowDeviceMinimum;
            }
        } else if (target > record->maxRatio) {
            // The voice cannot hold it, so we do not ask for it. XAudio2 would clamp silently and
            // every later comparison would be against a number the device never had.
            target = record->maxRatio;
            ++g_ratioCounters.clampedAtMax;
        }
        // Already there, by our own ledger: no call, and still a success. At 1.0x with every voice
        // confirmed at 1.0 this is the whole of the function, which is the "src/audio writes
        // nothing at 1x" rule holding per voice rather than per module.
        if (NearlyEqualRatio(record->commanded, target) &&
            NearlyEqualRatio(record->observed, target)) {
            ++applied;
            continue;
        }

        VoiceCall call4(chain.sinks[i]);
        vtbl = call4.Vtbl();
        if (vtbl == nullptr) {
            ++gone;
            continue;
        }
        const int32_t hr = vtbl->SetFrequencyRatio(voice, target, kXAudio2CommitNow);
        if (hr < 0) {
            if (firstError != nullptr && (*firstError == 0 || *firstError == kRatioBelowDeviceMinimum)) {
                *firstError = hr;   // a real refusal outranks the floor notice
            }
            continue;
        }
        // Read back for the LOG, never for the ledger. OBSERVED IN THIS TITLE, not XAudio2's
        // documented contract: kXAudio2CommitNow is an immediate operation set and the vendor says
        // Get returns what Set stored, yet this chain returns the PREVIOUS command (the owner's log
        // holds four rows of `wrote 0.0500, voice reads 0.1000`). Whoever is behind that -- a
        // wrapped voice, a proxy -- storing the reading as "what we left in the field" is the whole
        // of the geometric ratchet, and trusting `commanded` is correct under both models.
        float readBack = target;
        VoiceCall call5(chain.sinks[i]);
        vtbl = call5.Vtbl();
        if (vtbl != nullptr) {
            vtbl->GetFrequencyRatio(voice, &readBack);
        }
        // The one thing a read-back can still establish about the voice itself: that it held more
        // than we thought it could. Raised only upward, and only by a value inside XAudio2's own
        // domain, so the model is falsifiable by the device rather than by a wrong reading.
        if (CheckVoiceRatioDomain(readBack) == SinkVerdict::Ok && readBack > record->maxRatio &&
            !NearlyEqualRatio(readBack, record->maxRatio)) {
            record->maxRatio = readBack;
        }
        RecordCommand(record, target, readBack);
        NoteSinkRatioWrite(chain.sinks[i].sink, rate, record->baseline, target, readBack);
        ++applied;
    }
    if (voicesGone != nullptr) {
        *voicesGone = gone;
    }
    if (clampedToFloor != nullptr) {
        *clampedToFloor = clamped;
    }
    return applied;
}

size_t CheckChainDrift(const SinkChain& chain, VoiceRatioLedger& ledger, float rate,
                       SinkRatioDrift* worst) {
    size_t drifted = 0;
    float worstGap = 0.0f;
    for (size_t i = 0; i < chain.count; ++i) {
        VoiceRatio* entry = ledger.FindMutable(chain.sinks[i].voice);
        if (entry == nullptr || entry->generation != chain.sinks[i].generation) {
            continue;   // never ours, or a different voice at the same address
        }
        VoiceCall call(chain.sinks[i]);
        const XAudio2SourceVoiceVtbl* vtbl = call.Vtbl();
        if (vtbl == nullptr) {
            continue;
        }
        float live = 0.0f;
        vtbl->GetFrequencyRatio(chain.sinks[i].voice, &live);
        if (CheckVoiceRatioDomain(live) != SinkVerdict::Ok) {
            continue;
        }
        float expected = entry->baseline * rate;
        if (expected < kXAudio2MinFreqRatio) {
            expected = kXAudio2MinFreqRatio;   // the same clamp the apply makes, or every floored
        } else if (expected > entry->maxRatio) {
            expected = entry->maxRatio;        // and the same ceiling, or every clamped voice does
        }
        if (NearlyEqualRatio(live, expected)) {
            entry->driftStreak = 0;
            entry->lastRead = live;
            // The device has confirmed the ask. Recording it is what lets the next apply skip the
            // write entirely, which is the whole of "at 1x we touch nothing".
            entry->observed = live;
            continue;
        }
        // The read is one processing pass behind in this title, so the previous ask coming back is
        // the instrument, not the voice. Scoring it was two of the nine drift lines in the owner's
        // log and a CommandPlanned() on every one of them -- so one sample of it is forgiven. It is
        // forgiven ONCE, by the streak below and not by a branch of its own: a check runs every
        // kDriftCheckPeriodMs, which is many processing passes, so the same value still standing on
        // the next check is a write that did not take, whoever put the value there. Forgiving it for
        // as long as it happens to equal one of our own numbers is how a voice stays at 0.10x
        // through every later restore with nothing in the log.
        if (NearlyEqualRatio(live, entry->lastRead) && entry->driftStreak > 0) {
            if (entry->driftStreak < 255) {
                ++entry->driftStreak;
            }
        } else {
            entry->driftStreak = 1;
            entry->lastRead = live;
        }
        if (entry->driftStreak < 2) {
            continue;   // one sample is the lag; two of the same wrong value is a writer
        }
        // The same wrong number twice running. It is recorded as what the field READS -- never as
        // a baseline -- and recording it is also what re-arms the write the settled check would
        // otherwise skip. Only a value that is nobody's command names a foreign writer; one of our
        // own, still standing, is our write not taking, and the counter must not confuse them.
        // Neither is the shipped 1.0 (DIAG-PITCH-2 Corollary B names it): that is the value the
        // engine CREATES these voices with, so finding it is a voice we did not see rebuilt, not a
        // second writer. It is still drift -- the mix is not where the transport says it is.
        if (entry->driftStreak == 2 && !RatioIsOurs(*entry, live) &&
            !NearlyEqualRatio(live, kShippedFrequencyRatio)) {
            ++g_ratioCounters.foreignWriters;
        }
        entry->observed = live;
        ++drifted;
        const float gap = live > expected ? live - expected : expected - live;
        if (worst != nullptr && gap > worstGap) {
            worstGap = gap;
            *worst = SinkRatioDrift{chain.sinks[i].sink, expected, live};
        }
    }
    return drifted;
}

SinkRatioCensus SinkRatioCensusNow(const VoiceRatioLedger& ledger, float rate) {
    SinkRatioCensus c;
    c.voices = ledger.Count();
    for (size_t i = 0; i < ledger.Count(); ++i) {
        const VoiceRatio& e = ledger.At(i);
        if (!NearlyEqualRatio(e.baseline, kShippedFrequencyRatio)) {
            ++c.relearned;
        }
        size_t slot = c.distinctBaselines;
        for (size_t j = 0; j < c.distinctBaselines; ++j) {
            if (NearlyEqualRatio(c.baselines[j], e.baseline)) {
                slot = j;
                break;
            }
        }
        if (slot == c.distinctBaselines) {
            c.baselines[slot] = e.baseline;
            ++c.distinctBaselines;
        }
        ++c.baselineCounts[slot];
    }
    c.outstanding = OutstandingVoices(ledger, rate);
    c.clampedAtMax = g_ratioCounters.clampedAtMax;
    c.foreignWriters = g_ratioCounters.foreignWriters;
    c.retiredOwnTerm = g_ratioCounters.retiredOwnTerm;
    c.retiredChainAbsent = g_ratioCounters.retiredChainAbsent;
    c.adoptNoticesDropped = g_ratioCounters.adoptNoticesDropped;
    c.generation = VoiceGeneration();
    return c;
}

size_t FormatSinkRatioCensus(char* out, size_t cap, const SinkRatioCensus& c) {
    if (out == nullptr || cap == 0) {
        return 0;
    }
    char set[128];
    size_t used = 0;
    set[0] = '\0';
    for (size_t i = 0; i < c.distinctBaselines && used + 1 < sizeof set; ++i) {
        const int n = snprintf(set + used, sizeof set - used, "%s%.4f x%zu", i == 0 ? "" : ", ",
                               static_cast<double>(c.baselines[i]), c.baselineCounts[i]);
        if (n < 0) {
            break;
        }
        used += static_cast<size_t>(n);
    }
    const int written = snprintf(
        out, cap,
        "sink ratio census: %zu voice(s) -- baselines {%s}, relearned %zu, clamped-at-max %zu, "
        "foreign-writer %zu, retired %zu (own-term %zu, chain-absent %zu), outstanding %zu, "
        "generation %llu",
        c.voices, set, c.relearned, c.clampedAtMax, c.foreignWriters,
        c.retiredOwnTerm + c.retiredChainAbsent, c.retiredOwnTerm, c.retiredChainAbsent,
        c.outstanding, static_cast<unsigned long long>(c.generation));
    return written < 0 ? 0 : static_cast<size_t>(written);
}


size_t RestoreChainRatios(const SinkChain& chain, VoiceRatioLedger& ledger, int32_t* firstError,
                          size_t* voicesGone, bool forget) {
    if (firstError != nullptr) {
        *firstError = 0;
    }
    if (voicesGone != nullptr) {
        *voicesGone = 0;
    }
    if (!AudioWritesEnabled()) {
        return 0;   // nothing was ever commanded, so there is nothing of ours to put back
    }
    size_t restored = 0;
    for (size_t i = 0; i < chain.count; ++i) {
        XAudio2SourceVoice* voice = chain.sinks[i].voice;
        const VoiceRatio* entry = ledger.Find(voice);
        if (entry == nullptr) {
            continue;   // never commanded by us; there is nothing of ours to undo
        }
        // Copied out before any Forget below can move the entries under it. It is the shipped
        // constant, always -- RestoreChainRatios writing `entry->baseline` and ApplyRatioToChain
        // composing against it are now the same number by construction, which is what makes a
        // return to 1.0x exactly 1.0 for every voice however the session got here.
        const float baseline = entry->baseline;
        // Already there and confirmed there by the field itself: no call. The row still goes when
        // the module is handing the sink back, because a voice at its shipped ratio with no row is
        // the correct end state, not a leak.
        if (NearlyEqualRatio(entry->commanded, baseline) &&
            NearlyEqualRatio(entry->observed, baseline)) {
            if (forget) {
                ledger.Forget(voice);
            }
            ++restored;
            continue;
        }
        // A destroyed voice needs nothing put back, and the ledger drops it below either way.
        VoiceCall call2(chain.sinks[i]);
        const XAudio2SourceVoiceVtbl* vtbl = call2.Vtbl();
        if (vtbl == nullptr) {
            if (voicesGone != nullptr) {
                ++*voicesGone;
            }
            if (forget) {
                ledger.Forget(voice);
            }
            continue;
        }
        const int32_t hr = vtbl->SetFrequencyRatio(voice, baseline, kXAudio2CommitNow);
        if (hr < 0) {
            if (firstError != nullptr && *firstError == 0) {
                *firstError = hr;
            }
            continue;
        }
        // Read back like the apply does, and for the same reason: the value the log prints has to
        // be the one in the voice. Reporting the number we asked for as the number it holds is how
        // a restore that XAudio2 clamped -- or that a second writer moved afterwards -- reads as a
        // sink safely back at the engine's pitch.
        float readBack = baseline;
        VoiceCall call6(chain.sinks[i]);
        if (const XAudio2SourceVoiceVtbl* back = call6.Vtbl()) {
            back->GetFrequencyRatio(voice, &readBack);
        }
        NoteSinkRatioWrite(chain.sinks[i].sink, 1.0f, baseline, baseline, readBack);
        // Forgotten only after the command went out, and only for the voice it reached: an entry
        // dropped first would leave the voice at the replay's rate with nothing left that knows
        // what to put back, and dropping the whole table does that to every voice this pass
        // missed.
        if (forget) {
            ledger.Forget(voice);
        } else {
            ledger.Observe(voice, baseline);
        }
        ++restored;
    }
    return restored;
}

size_t OutstandingVoices(const VoiceRatioLedger& ledger, float rate) {
    size_t outstanding = 0;
    for (size_t i = 0; i < ledger.Count(); ++i) {
        const VoiceRatio& entry = ledger.At(i);
        float want = entry.baseline * rate;
        if (want < kXAudio2MinFreqRatio) {
            want = kXAudio2MinFreqRatio;   // the apply's own clamp, or a floored voice never settles
        } else if (want > entry.maxRatio) {
            // And the apply's own ceiling. Against kXAudio2MaxFreqRatio this compared an ask of
            // 1.0039 that the device had cut to 1.0000 and reported the voice as never settling.
            want = entry.maxRatio;
        }
        if (!NearlyEqualRatio(entry.commanded, want)) {
            ++outstanding;
        }
    }
    return outstanding;
}

size_t ApplyFrequencyRatio(const AkExports& ak, float rate, int32_t* firstError,
                           size_t* voicesGone) {
    if (firstError != nullptr) {
        *firstError = 0;
    }
    if (voicesGone != nullptr) {
        *voicesGone = 0;
    }
    SinkChain chain;
    if (ProbeSinks(ak, &chain) == 0) {
        return 0;
    }
    return ApplyRatioToChain(chain, rate, SinkRatioLedger(), firstError, voicesGone);
}

size_t RestoreFrequencyRatio(const AkExports& ak, int32_t* firstError, bool forget) {
    if (firstError != nullptr) {
        *firstError = 0;
    }
    SinkChain chain;
    if (ProbeSinks(ak, &chain) == 0) {
        return 0;
    }
    return RestoreChainRatios(chain, SinkRatioLedger(), firstError, nullptr, forget);
}

bool ReadChainSamplesPlayed(const SinkChain& chain, uint64_t* samplesPlayed,
                            uint32_t* buffersQueued, size_t* voicesGone) {
    if (voicesGone != nullptr) {
        *voicesGone = 0;
    }
    uint64_t samples = 0;
    uint32_t queued = 0;
    size_t live = 0;
    for (size_t i = 0; i < chain.count; ++i) {
        VoiceCall call3(chain.sinks[i]);
        const XAudio2SourceVoiceVtbl* vtbl = call3.Vtbl();
        if (vtbl == nullptr) {
            if (voicesGone != nullptr) {
                ++*voicesGone;
            }
            continue;
        }
        XAudio2VoiceState state{};
        vtbl->GetState(chain.sinks[i].voice, &state, 0);
        samples += state.samplesPlayed;
        queued += state.buffersQueued;
        ++live;
    }
    // No live voice is not a reading of zero. Reporting success here would hand the A/V bias a
    // queue depth of 0 and the O4 oracle a samplesPlayed that never advances -- both of which read
    // as measurements of a working device rather than as the absence of one.
    if (live == 0) {
        return false;
    }
    if (samplesPlayed != nullptr) {
        *samplesPlayed = samples;
    }
    if (buffersQueued != nullptr) {
        *buffersQueued = queued;
    }
    return true;
}

bool ReadSamplesPlayed(const AkExports& ak, uint64_t* samplesPlayed, uint32_t* buffersQueued,
                       size_t* voicesGone) {
    SinkChain chain;
    if (ProbeSinks(ak, &chain) == 0) {
        if (voicesGone != nullptr) {
            *voicesGone = 0;
        }
        return false;
    }
    return ReadChainSamplesPlayed(chain, samplesPlayed, buffersQueued, voicesGone);
}

}  // namespace audio
}  // namespace replay
