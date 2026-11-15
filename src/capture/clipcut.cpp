#include "capture/clipcut.h"

#include <string.h>

#include <vector>

#include "capture/keyframe.h"
#include "capture/manifestbuild.h"
#include "capture/streamcodec.h"
#include "foundation/bitio/bitio.h"
#include "foundation/demofile/demofile.h"

namespace replay {
namespace capture {
namespace {

struct CarriedEvent {
    uint64_t dst;
    uint16_t typeIndex;
    std::vector<uint8_t> body;
};

// The file's own tap table out of a chapter's keyframe (same reading as playback's TapIndexForName,
// kept here so the capture library does not depend on playback).
uint16_t IdentityIndexIn(const DecodedChapter& dc) {
    const StreamView* sv = dc.Find(kStreamKey);
    if (!sv || !sv->records || sv->bytes <= kBlockPrefixBytes) return kTapTypeIndexNone;
    static uint8_t blob[64 * 1024];
    bitio::BitReader r(sv->records + kBlockPrefixBytes, sv->bytes - kBlockPrefixBytes);
    uint16_t blobLen = 0;
    if (!DecodeRawRecord(r, blob, sizeof blob, blobLen) || !blobLen) return kTapTypeIndexNone;
    KeyframeReader kr;
    if (!kr.Open(blob, blobLen)) return kTapTypeIndexNone;
    const size_t want = sizeof(kIdentitySnapshotTapName) - 1;
    uint16_t blockId = 0;
    const uint8_t* data = nullptr;
    uint32_t len = 0;
    while (kr.Next(blockId, data, len)) {
        if (blockId != kKeyBlockTapTable || len < sizeof(uint16_t)) continue;
        uint16_t rows = 0;
        memcpy(&rows, data, sizeof rows);
        uint32_t at = sizeof(uint16_t);
        for (uint16_t i = 0; i < rows; ++i) {
            if (at + 3 * sizeof(uint16_t) > len) break;
            uint16_t index = 0, nameLen = 0;
            memcpy(&index, data + at, sizeof index);
            memcpy(&nameLen, data + at + 2 * sizeof(uint16_t), sizeof nameLen);
            at += 3 * sizeof(uint16_t);
            if (at + nameLen > len) break;
            if (nameLen == want && memcmp(data + at, kIdentitySnapshotTapName, want) == 0) return index;
            at += nameLen;
        }
    }
    return kTapTypeIndexNone;
}

bool InWindow(const ChapterKey& k, uint64_t fromUs, uint64_t toUs) {
    if (k.tEndUs < fromUs) return false;
    if (toUs != 0 && k.tBeginUs > toUs) return false;
    return true;
}

// Re-encodes `orig` (an S_EVENT block) with `carried` in front, every carried row stamped `stampUs`.
bool MergeEvents(const StreamView* orig, const std::vector<CarriedEvent>& carried, uint64_t stampUs,
                 std::vector<uint8_t>& out, uint32_t& records) {
    static uint8_t body[kMaxEventBodyBytes];
    size_t cap = kBlockPrefixBytes + 64;
    for (const auto& c : carried) cap += c.body.size() + 32;
    if (orig) cap += orig->bytes * 2 + 64;
    out.assign(cap, 0);
    bitio::BitWriter w(out.data() + kBlockPrefixBytes, cap - kBlockPrefixBytes);
    EventEncoder enc;
    enc.Reset();
    records = 0;
    for (const auto& c : carried) {
        EventMessage m{};
        m.stampUs = stampUs;
        m.dst = c.dst;
        m.typeIndex = c.typeIndex;
        m.bodyBytes = static_cast<uint16_t>(c.body.size());
        m.body = c.body.data();
        if (!enc.Encode(w, m)) return false;
        ++records;
    }
    if (orig && orig->records && orig->bytes > kBlockPrefixBytes) {
        bitio::BitReader r(orig->records + kBlockPrefixBytes, orig->bytes - kBlockPrefixBytes);
        EventDecoder dec;
        dec.Reset();
        for (uint32_t k = 0; k < orig->recordCount; ++k) {
            EventMessage m{};
            if (!dec.Decode(r, m, body, sizeof body)) return false;
            if (!enc.Encode(w, m)) return false;
            ++records;
        }
    }
    if (w.Refused()) return false;
    const uint32_t bits = static_cast<uint32_t>(w.BitsWritten());
    memcpy(out.data(), &bits, sizeof bits);
    out.resize(kBlockPrefixBytes + (bits + 7u) / 8u);
    return true;
}

}  // namespace

uint64_t RecordingEndUs(const wchar_t* path) {
    DemoReader r;
    if (!r.Open(path) || r.ChapterCount() == 0) return 0;
    uint64_t end = 0;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        const ChapterKey k = r.ChapterAt(i);
        if (k.section == kSectionCore && k.tEndUs > end) end = k.tEndUs;
    }
    return end;
}

uint64_t RecordingBeginUs(const wchar_t* path) {
    DemoReader r;
    if (!r.Open(path)) return 0;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        const ChapterKey k = r.ChapterAt(i);
        if (k.section == kSectionCore) return k.tBeginUs;
    }
    return 0;
}

ClipResult ClipRange(const wchar_t* in, const wchar_t* out, uint64_t fromUs, uint64_t toUs) {
    ClipResult res{};
    static uint8_t body[kMaxEventBodyBytes];

    DemoReader reader;
    if (!reader.Open(in)) {
        res.error = reader.RefusalReason();
        return res;
    }
    res.chaptersIn = static_cast<uint32_t>(reader.ChapterCount());

    // The CORE chapter holding fromUs is the first one kept: it starts on a keyframe.
    size_t firstCore = SIZE_MAX;
    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        const ChapterKey k = reader.ChapterAt(i);
        if (k.section == kSectionCore && InWindow(k, fromUs, toUs)) {
            firstCore = i;
            break;
        }
    }
    if (firstCore == SIZE_MAX) {
        res.error = "the recording has no CORE chapter in that window";
        return res;
    }
    const uint64_t startUs = reader.ChapterAt(firstCore).tBeginUs;

    // Pass 1: every identity event before the window, oldest first.
    std::vector<CarriedEvent> carried;
    uint16_t tapIndex = kTapTypeIndexNone;
    for (size_t i = 0; i < firstCore; ++i) {
        const ChapterKey k = reader.ChapterAt(i);
        if (k.section != kSectionCore) continue;
        DecodedChapter dc{};
        if (!reader.DecodeChapter(i, dc)) continue;
        const uint16_t here = IdentityIndexIn(dc);
        if (here != kTapTypeIndexNone) tapIndex = here;
        if (tapIndex == kTapTypeIndexNone) continue;
        const StreamView* sv = dc.Find(kStreamEvent);
        if (!sv || !sv->records || sv->bytes <= kBlockPrefixBytes) continue;
        bitio::BitReader r(sv->records + kBlockPrefixBytes, sv->bytes - kBlockPrefixBytes);
        EventDecoder dec;
        dec.Reset();
        for (uint32_t n = 0; n < sv->recordCount; ++n) {
            EventMessage m{};
            if (!dec.Decode(r, m, body, sizeof body)) break;
            if (m.typeIndex != tapIndex) continue;
            CarriedEvent c;
            c.dst = m.dst;
            c.typeIndex = m.typeIndex;
            c.body.assign(m.body, m.body + m.bodyBytes);
            carried.push_back(std::move(c));
        }
    }

    Manifest m = reader.GetManifest();
    DemoWriter writer;
    if (!writer.Open(out, m)) {
        res.error = writer.LastError() ? writer.LastError() : "cannot open the output";
        return res;
    }

    bool firstWritten = false;
    std::vector<uint8_t> merged;
    for (size_t i = firstCore; i < reader.ChapterCount(); ++i) {
        const ChapterKey k = reader.ChapterAt(i);
        if (k.section == kSectionManifest || k.section == kSectionIndex) continue;
        if (!InWindow(k, startUs, toUs)) {
            if (toUs != 0 && k.tBeginUs > toUs && k.section == kSectionCore) break;
            continue;
        }
        DecodedChapter dc{};
        if (!reader.DecodeChapter(i, dc)) {
            ++res.undecodable;
            continue;
        }
        writer.BeginChapter(k.section, k.tBeginUs);
        const bool injectHere = !firstWritten && k.section == kSectionCore && !carried.empty();
        bool eventsWritten = false;
        for (uint16_t b = 0; b < dc.streamCount; ++b) {
            const StreamView& v = dc.streams[b];
            if (injectHere && v.id == kStreamEvent) {
                uint32_t recs = 0;
                if (MergeEvents(&v, carried, k.tBeginUs, merged, recs)) {
                    writer.WriteStreamBlock(v.id, v.schema, merged.data(), merged.size(), recs, recs);
                    res.identityCarried = static_cast<uint32_t>(carried.size());
                } else {
                    writer.WriteStreamBlock(v.id, v.schema, v.records, v.bytes, v.observedCount,
                                            v.storedCount);
                }
                eventsWritten = true;
                continue;
            }
            writer.WriteStreamBlock(v.id, v.schema, v.records, v.bytes, v.observedCount, v.storedCount);
        }
        if (injectHere && !eventsWritten) {
            uint32_t recs = 0;
            if (MergeEvents(nullptr, carried, k.tBeginUs, merged, recs)) {
                writer.WriteStreamBlock(kStreamEvent, kSchemaEvent, merged.data(), merged.size(), recs,
                                        recs);
                res.identityCarried = static_cast<uint32_t>(carried.size());
            }
        }
        writer.EndChapter(k.tEndUs);
        if (k.section == kSectionCore) {
            if (!firstWritten) res.firstUs = k.tBeginUs;
            firstWritten = true;
            if (k.tEndUs > res.lastUs) res.lastUs = k.tEndUs;
        }
        ++res.chaptersOut;
    }
    writer.Close();
    if (writer.LastError()) {
        res.error = writer.LastError();
        return res;
    }
    if (res.chaptersOut == 0) {
        res.error = "wrote nothing";
        return res;
    }

    DemoReader check;
    if (!check.Open(out)) {
        res.error = check.RefusalReason();
        return res;
    }
    for (size_t i = 0; i < check.ChapterCount(); ++i) {
        DecodedChapter dc{};
        if (!check.DecodeChapter(i, dc)) {
            res.error = "the clip has a chapter that will not decode";
            return res;
        }
    }
    res.ok = true;
    return res;
}

}  // namespace capture
}  // namespace replay
