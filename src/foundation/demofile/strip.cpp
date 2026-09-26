#include <string.h>

#include <vector>

#include "foundation/demofile/demofile.h"
#include "foundation/demofile/format.h"

namespace replay {
namespace {

// A stripped section takes its stream with it, so a block that leaked into another section is caught too.
uint16_t StreamOfSection(uint16_t section) {
    switch (section) {
        case kSectionVoip: return kStreamVoip;
        case kSectionAsSeen: return kStreamAsSeen;
        case kSectionOracle: return kStreamOracle;
        default: return 0;
    }
}

uint64_t FlagOfSection(uint16_t section) {
    switch (section) {
        case kSectionVoip: return kFlagHasVoip;
        case kSectionAsSeen: return kFlagHasAsSeen;
        case kSectionOracle: return kFlagHasOracle;
        default: return 0;
    }
}

bool IsStrippedStream(uint32_t sectionMask, uint16_t stream) {
    for (uint16_t sec = 0; sec < kSectionCount; ++sec) {
        if ((sectionMask & (1u << sec)) && StreamOfSection(sec) == stream && stream != 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

StripResult StripSections(const wchar_t* in, const wchar_t* out, uint32_t sectionMask) {
    StripResult r{};

    // MANIFEST, CORE and INDEX are what a file needs to play; refusing here stops "strip" from becoming
    // a way to produce an unplayable file by accident.
    if (sectionMask & ((1u << kSectionManifest) | (1u << kSectionCore) | (1u << kSectionIndex))) {
        r.error = "MANIFEST, CORE and INDEX cannot be stripped";
        return r;
    }
    if (sectionMask == 0) {
        r.error = "no section selected";
        return r;
    }

    DemoReader reader;
    if (!reader.Open(in)) {
        r.error = reader.RefusalReason();
        return r;
    }
    r.chaptersIn = static_cast<uint32_t>(reader.ChapterCount());

    Manifest m = reader.GetManifest();
    for (uint16_t sec = 0; sec < kSectionCount; ++sec) {
        if (sectionMask & (1u << sec)) {
            m.flags &= ~static_cast<uint32_t>(FlagOfSection(sec));
        }
    }

    DemoWriter writer;
    if (!writer.Open(out, m)) {
        r.error = writer.LastError() ? writer.LastError() : "cannot open the output";
        return r;
    }

    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        const ChapterKey key = reader.ChapterAt(i);
        if (key.section < kSectionCount && (sectionMask & (1u << key.section))) {
            ++r.chaptersDropped;
            r.bytesRemoved += kChapterHeadBytes + reader.ChapterPayloadLen(i);
            continue;
        }
        if (key.section == kSectionManifest) {
            continue;  // the writer emitted its own from the manifest above
        }

        DecodedChapter c{};
        if (!reader.DecodeChapter(i, c)) {
            // We cannot see inside it, so we cannot promise it holds no voice. For a privacy control
            // that promise is the whole point, and the count says what it cost.
            ++r.chaptersUndecodable;
            ++r.chaptersDropped;
            r.bytesRemoved += kChapterHeadBytes + reader.ChapterPayloadLen(i);
            continue;
        }

        writer.BeginChapter(key.section, key.tBeginUs);
        for (uint16_t b = 0; b < c.streamCount; ++b) {
            const StreamView& v = c.streams[b];
            if (IsStrippedStream(sectionMask, v.id)) {
                ++r.strippedBlocksInKeptChapters;
                continue;
            }
            writer.WriteStreamBlock(v.id, v.schema, v.records, v.bytes, v.observedCount, v.storedCount);
        }
        writer.EndChapter(key.tEndUs);
        ++r.chaptersOut;
    }

    writer.Close();
    if (writer.Stats().chaptersWritten == 0) {
        r.error = "wrote nothing";
        return r;
    }
    if (writer.LastError()) {
        r.error = writer.LastError();
        return r;
    }

    // The affordance is only real if it is checked. Re-open the result and look for a survivor.
    DemoReader check;
    if (!check.Open(out)) {
        r.error = check.RefusalReason();
        return r;
    }
    for (size_t i = 0; i < check.ChapterCount(); ++i) {
        const ChapterKey key = check.ChapterAt(i);
        if (key.section < kSectionCount && (sectionMask & (1u << key.section))) {
            r.error = "a stripped section survived in the output";
            return r;
        }
        DecodedChapter c{};
        if (!check.DecodeChapter(i, c)) {
            r.error = "the output has a chapter that will not decode";
            return r;
        }
        for (uint16_t b = 0; b < c.streamCount; ++b) {
            if (IsStrippedStream(sectionMask, c.streams[b].id)) {
                r.error = "a stripped stream survived in the output";
                return r;
            }
        }
    }
    for (uint16_t sec = 0; sec < kSectionCount; ++sec) {
        if ((sectionMask & (1u << sec)) && (check.FileFlags() & FlagOfSection(sec)) != 0) {
            r.error = "a stripped section's header flag is still set";
            return r;
        }
    }
    r.verified = true;
    return r;
}

}  // namespace replay
