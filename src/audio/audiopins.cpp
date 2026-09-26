#include "audio/audiopins.h"

#include <string.h>

namespace replay {
namespace audio {
namespace {

#pragma pack(push, 1)
struct PinBlockHeader {
    uint16_t version;
    uint16_t sectionCount;
};
struct PinSectionHeader {
    uint16_t tag;
    uint32_t byteLen;
};
#pragma pack(pop)
static_assert(sizeof(PinBlockHeader) == 4, "pin block header is 4 bytes");
static_assert(sizeof(PinSectionHeader) == 6, "pin section header is 6 bytes");

class Writer {
public:
    Writer(void* dst, size_t cap) : dst_(static_cast<uint8_t*>(dst)), cap_(cap) {}

    void Raw(const void* src, size_t n) {
        if (!ok_) {
            return;
        }
        if (at_ + n > cap_) {
            ok_ = false;
            return;
        }
        memcpy(dst_ + at_, src, n);
        at_ += n;
    }

    // Sections carry their length up front because it is always known: every one is an array of a
    // fixed-stride record, so nothing has to be backfilled and nothing can be counted twice.
    void Section(uint16_t tag, const void* data, size_t bytes) {
        PinSectionHeader h{tag, static_cast<uint32_t>(bytes)};
        Raw(&h, sizeof h);
        Raw(data, bytes);
        ++sections_;
    }

    size_t Finish() {
        if (!ok_ || at_ < sizeof(PinBlockHeader)) {
            return 0;
        }
        PinBlockHeader h{kPinVersion, sections_};
        memcpy(dst_, &h, sizeof h);
        return at_;
    }

    void Reserve() {
        PinBlockHeader h{kPinVersion, 0};
        Raw(&h, sizeof h);
    }

private:
    uint8_t* dst_;
    size_t cap_;
    size_t at_ = 0;
    uint16_t sections_ = 0;
    bool ok_ = true;
};

}  // namespace

const char* PinDecodeName(PinDecode d) {
    switch (d) {
        case PinDecode::Ok: return "ok";
        case PinDecode::TooShort: return "the pin block is shorter than its header";
        case PinDecode::UnsupportedVersion: return "the pin block version is not one this build reads";
        case PinDecode::SectionOverrunsBlock: return "a section length runs past the end of the block";
        case PinDecode::RaggedSection: return "a section length is not a whole number of records";
        case PinDecode::HistoryTruncated: return "the container history is larger than this build holds";
        default: return "unknown";
    }
}

size_t EncodePins(const AudioPins& pins, void* dst, size_t cap) {
    if (dst == nullptr) {
        return 0;
    }
    Writer w(dst, cap);
    w.Reserve();

    // First section, and written even when every other one is absent: a block that carries
    // nothing still has to say why it carries nothing.
    {
        PinSourceHeader src{};
        src.reason = static_cast<uint8_t>(pins.source);
        w.Section(kPinSource, &src, sizeof src);
    }

    if (pins.stateCount != 0) {
        w.Section(kPinStates, pins.states, pins.stateCount * sizeof(AudioStatePair));
    }
    if (pins.switchCount != 0) {
        w.Section(kPinSwitches, pins.switches, pins.switchCount * sizeof(AudioSwitchTriple));
    }
    if (pins.loopCount != 0) {
        w.Section(kPinLoops, pins.loops, pins.loopCount * sizeof(AudioLoopPin));
    }

    // The random section is written even when the history is empty, because "we asked and Wwise
    // gave us nothing" and "we never asked" are different files and a reader must be able to tell
    // them apart. seedValid carries which.
    {
        uint8_t buf[sizeof(PinRandomHeader) + AudioPins::kMaxHistory];
        PinRandomHeader h{};
        h.seed = pins.seed;
        h.seedValid = pins.seedValid ? 1u : 0u;
        h.historyBytes = pins.historyBytes;
        memcpy(buf, &h, sizeof h);
        if (pins.historyBytes != 0) {
            memcpy(buf + sizeof h, pins.history, pins.historyBytes);
        }
        w.Section(kPinRandom, buf, sizeof h + pins.historyBytes);
    }

    w.Section(kPinCounts, &pins.totals, sizeof(PinTotals));
    return w.Finish();
}

PinDecode DecodePins(const void* src, size_t bytes, AudioPins* out, PinDecodeReport* report) {
    PinDecodeReport local;
    PinDecodeReport& rep = report != nullptr ? *report : local;
    rep = PinDecodeReport{};

    if (out == nullptr) {
        rep.verdict = PinDecode::TooShort;
        return rep.verdict;
    }
    *out = AudioPins{};

    const auto* p = static_cast<const uint8_t*>(src);
    if (p == nullptr || bytes < sizeof(PinBlockHeader)) {
        rep.verdict = PinDecode::TooShort;
        return rep.verdict;
    }
    PinBlockHeader head{};
    memcpy(&head, p, sizeof head);
    if (head.version != kPinVersion) {
        rep.verdict = PinDecode::UnsupportedVersion;
        return rep.verdict;
    }

    size_t at = sizeof head;
    for (uint16_t i = 0; i < head.sectionCount; ++i) {
        if (at + sizeof(PinSectionHeader) > bytes) {
            rep.verdict = PinDecode::SectionOverrunsBlock;
            return rep.verdict;
        }
        PinSectionHeader sh{};
        memcpy(&sh, p + at, sizeof sh);
        at += sizeof sh;
        if (at + sh.byteLen > bytes) {
            rep.verdict = PinDecode::SectionOverrunsBlock;
            return rep.verdict;
        }
        const uint8_t* body = p + at;
        at += sh.byteLen;

        switch (sh.tag) {
            case kPinStates: {
                if (sh.byteLen % sizeof(AudioStatePair) != 0) {
                    rep.verdict = PinDecode::RaggedSection;
                    return rep.verdict;
                }
                const uint32_t n = sh.byteLen / sizeof(AudioStatePair);
                const uint32_t take = n < AudioPins::kMaxStates ? n : AudioPins::kMaxStates;
                memcpy(out->states, body, take * sizeof(AudioStatePair));
                out->stateCount = take;
                rep.droppedRecords += n - take;
                break;
            }
            case kPinSwitches: {
                if (sh.byteLen % sizeof(AudioSwitchTriple) != 0) {
                    rep.verdict = PinDecode::RaggedSection;
                    return rep.verdict;
                }
                const uint32_t n = sh.byteLen / sizeof(AudioSwitchTriple);
                const uint32_t take = n < AudioPins::kMaxSwitches ? n : AudioPins::kMaxSwitches;
                memcpy(out->switches, body, take * sizeof(AudioSwitchTriple));
                out->switchCount = take;
                rep.droppedRecords += n - take;
                break;
            }
            case kPinLoops: {
                if (sh.byteLen % sizeof(AudioLoopPin) != 0) {
                    rep.verdict = PinDecode::RaggedSection;
                    return rep.verdict;
                }
                const uint32_t n = sh.byteLen / sizeof(AudioLoopPin);
                const uint32_t take = n < AudioPins::kMaxLoops ? n : AudioPins::kMaxLoops;
                memcpy(out->loops, body, take * sizeof(AudioLoopPin));
                out->loopCount = take;
                rep.droppedRecords += n - take;
                break;
            }
            case kPinRandom: {
                if (sh.byteLen < sizeof(PinRandomHeader)) {
                    rep.verdict = PinDecode::RaggedSection;
                    return rep.verdict;
                }
                PinRandomHeader rh{};
                memcpy(&rh, body, sizeof rh);
                const uint32_t avail = sh.byteLen - static_cast<uint32_t>(sizeof rh);
                if (rh.historyBytes > avail) {
                    rep.verdict = PinDecode::RaggedSection;
                    return rep.verdict;
                }
                if (rh.historyBytes > AudioPins::kMaxHistory) {
                    // Refused rather than clipped: a partial container history restored into Wwise
                    // pins the wrong variation and looks exactly like a working pin.
                    rep.verdict = PinDecode::HistoryTruncated;
                    return rep.verdict;
                }
                out->seed = rh.seed;
                out->seedValid = rh.seedValid != 0;
                out->historyBytes = rh.historyBytes;
                memcpy(out->history, body + sizeof rh, rh.historyBytes);
                break;
            }
            case kPinCounts: {
                if (sh.byteLen != sizeof(PinTotals)) {
                    rep.verdict = PinDecode::RaggedSection;
                    return rep.verdict;
                }
                memcpy(&out->totals, body, sizeof(PinTotals));
                break;
            }
            case kPinSource: {
                if (sh.byteLen != sizeof(PinSourceHeader)) {
                    rep.verdict = PinDecode::RaggedSection;
                    return rep.verdict;
                }
                PinSourceHeader src{};
                memcpy(&src, body, sizeof src);
                // A code this build does not know is kept verbatim rather than mapped onto
                // one it does; PinSourceName says "unknown" for it.
                out->source = static_cast<PinSource>(src.reason);
                break;
            }
            default:
                // Skip by length AND count. A reader that quietly ignored a section it did not
                // understand could not tell a file that carries nothing from one it cannot read.
                ++rep.unknownSections;
                rep.unknownBytes += sh.byteLen;
                break;
        }
    }

    rep.verdict = PinDecode::Ok;
    return rep.verdict;
}

const char* PinSourceName(PinSource s) {
    switch (s) {
        case PinSource::Bound:
            return "bound";
        case PinSource::NoAudioState:
            return "no audio state";
        case PinSource::Unstated:
            return "unstated";
    }
    return "unknown";
}

}  // namespace audio
}  // namespace replay
