#include <windows.h>

#include <string.h>

#include <string>
#include <vector>

#include "foundation/demofile/demofile.h"
#include "foundation/demofile/format.h"
#include "foundation/demofile/manifest_io.h"
// zstd 1.3.8 keeps ZSTD_compress2 and the parameter API behind this; we link it statically.
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

namespace replay {
namespace {

constexpr int kZstdLevel = 3;  // S5.6 step 6
constexpr uint32_t kWriterVersion = 1;

struct BlockSpan {
    uint32_t begin;  // offset of the 16-byte header inside the payload builder
    uint32_t end;
};

struct Deficit {
    uint16_t stream;
    uint32_t missing;
};

}  // namespace

struct DemoWriter::Impl {
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring path;
    FileHeaderDisk header{};
    ManifestStore manifest;
    std::vector<uint8_t> manifestBytes;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> frame;
    std::vector<uint8_t> compressed;
    std::vector<BlockSpan> blocks;
    std::vector<IndexEntryDisk> index;
    std::vector<RoundMarker> rounds;
    std::vector<uint8_t> retention;  // by schema id; 0xFF means the manifest does not carry it
    std::vector<Deficit> deficits;
    std::vector<uint16_t> gapsNamedByCaller;

    // The stats records this chapter has collected, already in disk form, and the per-user clock that
    // spans the whole file. The cursor is per file and not per chapter: time may not go backwards for
    // a user across a chapter boundary either.
    std::vector<uint8_t> statsPending;
    uint32_t statsPendingCount = 0;
    PlayerStatsCursor statsCursor{};
    ZSTD_CCtx* cctx = nullptr;

    bool inChapter = false;
    uint16_t chapterSection = 0;
    uint64_t chapterBeginUs = 0;
    uint64_t lastEndUs = 0;
    uint32_t seq = 0;
    uint64_t observedFlags = 0;
    uint64_t offset = 0;

    WriterStats stats{};
    const char* lastError = nullptr;

    void Fail(const char* why) { lastError = why; }

    bool WriteAt(const void* data, uint32_t bytes) {
        DWORD done = 0;
        if (!WriteFile(file, data, bytes, &done, nullptr) || done != bytes) {
            Fail("write failed");
            return false;
        }
        offset += bytes;
        stats.bytesWritten += bytes;
        return true;
    }

    uint8_t RetentionOf(uint16_t schemaId) const {
        return schemaId < retention.size() ? retention[schemaId] : 0xFFu;
    }

    void AppendBlock(uint16_t streamId, uint16_t schemaId, const void* data, uint32_t bytes,
                     uint32_t observed, uint32_t stored) {
        StreamBlockDisk h{};
        h.streamId = streamId;
        h.schemaId = schemaId;
        h.byteLen = bytes;
        h.observed = observed;
        h.stored = stored;
        const uint32_t begin = static_cast<uint32_t>(payload.size());
        const uint8_t* hp = reinterpret_cast<const uint8_t*>(&h);
        payload.insert(payload.end(), hp, hp + sizeof(h));
        if (bytes) {
            const uint8_t* dp = static_cast<const uint8_t*>(data);
            payload.insert(payload.end(), dp, dp + bytes);
        }
        blocks.push_back(BlockSpan{begin, static_cast<uint32_t>(payload.size())});
    }

    void NoteDeficit(uint16_t streamId, uint32_t missing) {
        for (Deficit& d : deficits) {
            if (d.stream == streamId) {
                d.missing += missing;
                return;
            }
        }
        deficits.push_back(Deficit{streamId, missing});
    }

    // A GAP or DROPPED block the caller supplied names the streams it accounts for; those streams do not
    // get a second, writer-authored record for the same loss.
    void HarvestCallerGaps(const void* data, uint32_t bytes) {
        const uint32_t count = bytes / sizeof(GapRecordDisk);
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (uint32_t i = 0; i < count; ++i) {
            GapRecordDisk g{};
            memcpy(&g, p + i * sizeof(GapRecordDisk), sizeof(g));
            gapsNamedByCaller.push_back(g.streamId);
        }
    }

    bool CallerNamed(uint16_t streamId) const {
        for (uint16_t s : gapsNamedByCaller) {
            if (s == streamId) {
                return true;
            }
        }
        return false;
    }

    void HarvestRounds(const void* data, uint32_t bytes) {
        const uint32_t count = bytes / sizeof(MarkRecordDisk);
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (uint32_t i = 0; i < count; ++i) {
            MarkRecordDisk d{};
            memcpy(&d, p + i * sizeof(MarkRecordDisk), sizeof(d));
            rounds.push_back(RoundMarker{d.tUs, d.kind, d.index, d.payload});
        }
    }

    bool EmitChapterFrame(uint16_t section, uint64_t tBeginUs, uint64_t tEndUs, const uint8_t* body,
                          uint32_t bodyBytes, uint16_t streamCount) {
        if (index.size() >= kMaxChapters) {
            Fail("chapter limit reached; the file must be closed");
            return false;
        }
        // EndChapter groups blocks under the cap and a frame that grew is stored raw, so neither length
        // can exceed it. The reader refuses a head that claims otherwise; checking it on this side too
        // is what keeps the two from drifting into a file only one of them believes in.
        if (bodyBytes > kMaxChapterPayload) {
            Fail("chapter payload exceeds the cap");
            return false;
        }
        compressed.resize(ZSTD_compressBound(bodyBytes));
        uint16_t codec = kCodecZstd;
        size_t packed = ZSTD_compress2(cctx, compressed.data(), compressed.size(), body, bodyBytes);
        if (ZSTD_isError(packed) || packed >= bodyBytes) {
            codec = kCodecRaw;
            packed = bodyBytes;
        }
        const uint8_t* stored = (codec == kCodecZstd) ? compressed.data() : body;

        ChapterHeadDisk h{};
        h.magic = kChapterMagic;
        h.section = section;
        h.codec = codec;
        h.payloadLen = static_cast<uint32_t>(packed);
        h.payloadCrc32 = Crc32(stored, packed);
        h.tBeginUs = tBeginUs;
        h.tEndUs = tEndUs;
        h.seq = seq;
        h.uncompressedLen = bodyBytes;
        h.streamCount = streamCount;
        h.reserved = 0;
        h.headCrc32 = Crc32(&h, kChapterHeadCrcCover);

        IndexEntryDisk e{};
        e.tBeginUs = tBeginUs;
        e.tEndUs = tEndUs;
        e.offset = offset;
        e.len = h.payloadLen;
        e.section = section;
        e.seq = static_cast<uint16_t>(seq);

        // One WriteFile per chapter, and nothing is buffered across chapters: after a TerminateProcess
        // the file is a prefix of itself, so the torn tail costs one chapter and never a batch (S5.8).
        frame.resize(kChapterHeadBytes + packed);
        memcpy(frame.data(), &h, sizeof(h));
        if (packed) {
            memcpy(frame.data() + kChapterHeadBytes, stored, packed);
        }
        if (!WriteAt(frame.data(), static_cast<uint32_t>(frame.size()))) {
            return false;
        }
        index.push_back(e);
        ++seq;
        ++stats.chaptersWritten;
        return true;
    }
};

DemoWriter::DemoWriter() : impl_(new Impl) {}

DemoWriter::~DemoWriter() {
    Close();
    delete impl_;
}

bool DemoWriter::Open(const wchar_t* path, const Manifest& manifest) {
    Impl& s = *impl_;
    if (s.file != INVALID_HANDLE_VALUE) {
        s.Fail("already open");
        return false;
    }
    s.stats = WriterStats{};
    s.lastError = nullptr;
    s.index.clear();
    s.rounds.clear();
    s.seq = 0;
    s.offset = 0;
    s.observedFlags = 0;
    s.inChapter = false;
    s.statsPending.clear();
    s.statsPendingCount = 0;
    s.statsCursor = PlayerStatsCursor{};

    // S5.6.2 rule 1: a sample-dropping transform must not be expressible in the schema. Retention::Edge
    // was one -- it licensed storing fewer records than the engine numbered, and the format carries no
    // per-record index or run boundary, so no reader can tell "omitted because unchanged" from "omitted
    // because dropped". Refusing it here is the only place the container can enforce that, and it costs
    // nothing: no capture has started, so no data is at risk. A stream whose producer is genuinely
    // change-triggered has no samples between edges, so its observed already equals its stored and it
    // does not need Edge to say so.
    for (uint16_t i = 0; i < manifest.schemaCount; ++i) {
        if (manifest.schemas[i].retention != Retention::All) {
            s.Fail("a schema declares a retention other than ALL; sample dropping is not writable");
            return false;
        }
    }

    s.manifest.CopyFrom(manifest);
    SerializeManifest(s.manifest.View(), s.manifestBytes);

    s.retention.assign(1, 0xFFu);
    for (uint16_t i = 0; i < manifest.schemaCount; ++i) {
        const SchemaId id = manifest.schemas[i].id;
        if (id >= s.retention.size()) {
            s.retention.resize(size_t(id) + 1, 0xFFu);
        }
        s.retention[id] = static_cast<uint8_t>(manifest.schemas[i].retention);
    }

    s.cctx = ZSTD_createCCtx();
    if (!s.cctx) {
        s.Fail("ZSTD_createCCtx failed");
        return false;
    }
    ZSTD_CCtx_setParameter(s.cctx, ZSTD_c_compressionLevel, kZstdLevel);
    // The frame checksum is what lets a reader tell "this file is damaged" from "we sized a buffer
    // wrong" without guessing (S5.8).
    ZSTD_CCtx_setParameter(s.cctx, ZSTD_c_checksumFlag, 1);

    s.path = path;
    s.file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s.file == INVALID_HANDLE_VALUE) {
        s.Fail("CreateFileW failed");
        ZSTD_freeCCtx(s.cctx);
        s.cctx = nullptr;
        return false;
    }

    const Manifest& m = s.manifest.View();
    FileHeaderDisk& h = s.header;
    h = FileHeaderDisk{};
    memcpy(h.magic, kFileMagic, sizeof(h.magic));
    h.formatMajor = kFormatMajor;
    h.formatMinor = kFormatMinor;
    h.flags = m.flags | kFlagOpen;
    h.trailerOffset = 0;
    h.firstChapterOffset = kFileHeaderBytes;
    h.t0Us = m.t0Us;
    memcpy(h.demoUuid, &m.demoUuidLo, 8);
    memcpy(h.demoUuid + 8, &m.demoUuidHi, 8);
    memcpy(h.echovrMd5, m.echovrMd5, sizeof(h.echovrMd5));
    h.buildStringHash = m.buildStringHash;
    h.contentHash = m.contentHash;
    h.captureRole = m.captureRole;
    h.writerVersion = kWriterVersion;
    h.authorityFlags = m.authorityFlags;
    h.headerCrc32 = HeaderCrc(h);
    if (!s.WriteAt(&h, sizeof(h))) {
        Close();
        return false;
    }

    BeginChapter(kSectionManifest, m.t0Us);
    WriteStreamBlock(kStreamManifest, 0, s.manifestBytes.data(), s.manifestBytes.size(), 1, 1);
    EndChapter(m.t0Us);
    return s.lastError == nullptr;
}

void DemoWriter::BeginChapter(uint16_t section, uint64_t tBeginUs) {
    Impl& s = *impl_;
    if (s.file == INVALID_HANDLE_VALUE) {
        s.Fail("BeginChapter on a closed writer");
        return;
    }
    if (s.inChapter) {
        // Never drop what is already staged; close it at the only timestamp we have and say so.
        s.Fail("BeginChapter while a chapter was open");
        EndChapter(tBeginUs);
    }
    s.payload.clear();
    s.blocks.clear();
    s.deficits.clear();
    s.gapsNamedByCaller.clear();
    s.inChapter = true;
    s.chapterSection = section;
    s.chapterBeginUs = tBeginUs;
}

void DemoWriter::WriteStreamBlock(uint16_t streamId, uint16_t schemaId, const void* data, size_t bytes,
                                  uint32_t observed, uint32_t stored) {
    Impl& s = *impl_;
    if (!s.inChapter) {
        s.Fail("WriteStreamBlock outside a chapter");
        ++s.stats.blocksOutsideChapter;
        return;
    }
    if (bytes > 0xFFFFFFFFull) {
        s.Fail("stream block larger than 4 GiB");
        return;
    }
    const uint32_t n = static_cast<uint32_t>(bytes);

    // `observed` is the engine's own numbering for this span; `stored` is what reached the file. A block
    // claiming to have stored more than the engine numbered is a caller bug, and it is written through
    // verbatim: clamping it to `stored` made the block agree with itself, so the fault survived only in
    // this process's stats and `echodemo verify` could not fail on the one class of fault the pair
    // exists to expose. One increment per offending block is this writer's per-block record of it; on
    // disk the block's own header pair is, and a reader decides it with `observed < stored`.
    if (observed < stored) {
        ++s.stats.observedBelowStored;
        s.Fail("observed < stored: the counter is not engine-sourced");
    }

    if (StreamCarriesSamples(streamId) && s.RetentionOf(schemaId) == 0xFFu) {
        ++s.stats.unknownSchemaBlocks;
        s.Fail("stream block cites a schema the manifest does not carry");
    }

    if (kStreamBlockBytes + uint64_t(n) > kMaxChapterPayload) {
        // Nothing can carry it: one chapter is the recovery unit and the cap is what bounds a scan.
        ++s.stats.oversizeBlocksRefused;
        s.Fail("stream block exceeds one chapter");
        s.NoteDeficit(streamId, stored);
        return;
    }

    // Both layouts are fixed by S5.7 and S5.6.2. A block that is not a whole number of records is not
    // one of them, so it is stored verbatim and left unparsed rather than misread.
    if (streamId == kStreamGap || streamId == kStreamDropped) {
        if (n % sizeof(GapRecordDisk) == 0) {
            s.HarvestCallerGaps(data, n);
        } else {
            s.Fail("a GAP block is not a whole number of records");
        }
    } else if (streamId == kStreamMark) {
        if (n % sizeof(MarkRecordDisk) == 0) {
            s.HarvestRounds(data, n);
        } else {
            s.Fail("an S_MARK block is not a whole number of records");
        }
    } else if (streamId == kStreamGameState) {
        if (n % sizeof(GameStateRecordDisk) != 0) {
            s.Fail("an S_GAMESTATE block is not a whole number of records");
        }
    }

    // Every sample-carrying stream, with no qualifier: S5.6.2 rule 3 has none either.
    if (StreamCarriesSamples(streamId) && observed > stored) {
        s.NoteDeficit(streamId, observed - stored);
    }

    if (streamId == kStreamVoip) {
        s.observedFlags |= kFlagHasVoip;
    } else if (streamId == kStreamAsSeen) {
        s.observedFlags |= kFlagHasAsSeen;
    } else if (streamId == kStreamOracle) {
        s.observedFlags |= kFlagHasOracle;
    }

    s.AppendBlock(streamId, schemaId, data, n, observed, stored);
}

void DemoWriter::EndChapter(uint64_t tEndUs) {
    Impl& s = *impl_;
    if (!s.inChapter) {
        s.Fail("EndChapter without BeginChapter");
        return;
    }
    s.inChapter = false;

    // The stats this chapter collected, as one block. It goes in before the auto-gap records so the
    // chapter's content still precedes its bookkeeping, exactly as a caller-written block does.
    if (!s.statsPending.empty()) {
        s.AppendBlock(kStreamPlayerStats, 0, s.statsPending.data(),
                      static_cast<uint32_t>(s.statsPending.size()), s.statsPendingCount,
                      s.statsPendingCount);
        s.statsPending.clear();
        s.statsPendingCount = 0;
    }

    // S5.6.2: a file that lost samples says so, in the chapter, with the range. If the caller did not
    // record the loss itself, the writer records it -- silence is the one outcome that is not allowed.
    std::vector<GapRecordDisk> autoGaps;
    for (const Deficit& d : s.deficits) {
        if (d.missing == 0 || s.CallerNamed(d.stream)) {
            continue;
        }
        GapRecordDisk g{};
        g.tUs = s.chapterBeginUs;
        g.streamId = d.stream;
        g.reason = kGapEngineSequence;
        g.firstMissing = 0;
        g.count = d.missing;
        autoGaps.push_back(g);
    }
    if (!autoGaps.empty()) {
        const uint32_t bytes = static_cast<uint32_t>(autoGaps.size() * sizeof(GapRecordDisk));
        s.AppendBlock(kStreamGap, 0, autoGaps.data(), bytes, static_cast<uint32_t>(autoGaps.size()),
                      static_cast<uint32_t>(autoGaps.size()));
        s.stats.gapRecordsEmitted += static_cast<uint32_t>(autoGaps.size());
    }

    if (s.chapterSection == kSectionVoip) {
        s.observedFlags |= kFlagHasVoip;
    } else if (s.chapterSection == kSectionAsSeen) {
        s.observedFlags |= kFlagHasAsSeen;
    } else if (s.chapterSection == kSectionOracle) {
        s.observedFlags |= kFlagHasOracle;
    }

    if (tEndUs < s.chapterBeginUs) {
        tEndUs = s.chapterBeginUs;  // E-C8: a stamp that goes backwards is data, not a reason to lie
    }
    s.lastEndUs = tEndUs;

    // Group blocks so no chapter's payload exceeds the cap. Compression only ever shrinks a payload
    // here -- a frame that grew is stored raw -- so grouping on the uncompressed size is sufficient.
    size_t first = 0;
    while (first < s.blocks.size()) {
        size_t last = first;
        uint32_t bytes = 0;
        while (last < s.blocks.size()) {
            const uint32_t blockBytes = s.blocks[last].end - s.blocks[last].begin;
            // ChapterFrame::stream_count is uint16_t, so a group stops there as well as at the cap.
            if (last > first && (bytes + blockBytes > kMaxChapterPayload || last - first >= 0xFFFFu)) {
                break;
            }
            bytes += blockBytes;
            ++last;
        }
        const uint32_t begin = s.blocks[first].begin;
        if (!s.EmitChapterFrame(s.chapterSection, s.chapterBeginUs, tEndUs, s.payload.data() + begin, bytes,
                                static_cast<uint16_t>(last - first))) {
            return;
        }
        first = last;
    }
    if (s.blocks.empty()) {
        static const uint8_t kNothing = 0;
        s.EmitChapterFrame(s.chapterSection, s.chapterBeginUs, tEndUs, &kNothing, 0, 0);
    }
}

void DemoWriter::Close() {
    Impl& s = *impl_;
    if (s.file == INVALID_HANDLE_VALUE) {
        return;
    }
    if (s.inChapter) {
        EndChapter(s.chapterBeginUs);  // E-C12
    }

    const uint64_t trailerOffset = s.offset;
    std::vector<uint8_t> indexBytes(s.index.size() * sizeof(IndexEntryDisk));
    if (!s.index.empty()) {
        memcpy(indexBytes.data(), s.index.data(), indexBytes.size());
    }

    // S5.7: the round table is mirrored into the trailer manifest so the browser can list rounds
    // without touching the stream. A session that wrote no S_MARK block keeps whatever table Open was
    // given, so rewriting a file through this writer cannot erase its rounds.
    if (!s.rounds.empty()) {
        s.manifest.SetRounds(s.rounds.data(), s.rounds.size());
    }
    std::vector<uint8_t> trailerManifest;
    SerializeManifest(s.manifest.View(), trailerManifest);

    const uint64_t tBegin = s.index.empty() ? s.header.t0Us : s.index.front().tBeginUs;
    BeginChapter(kSectionIndex, tBegin);
    WriteStreamBlock(kStreamIndex, 0, indexBytes.data(), indexBytes.size(),
                     static_cast<uint32_t>(s.index.size()), static_cast<uint32_t>(s.index.size()));
    WriteStreamBlock(kStreamManifest, 0, trailerManifest.data(), trailerManifest.size(), 1, 1);
    EndChapter(s.lastEndUs);

    FileFooterDisk f{};
    memcpy(f.magic, kFooterMagic, sizeof(f.magic));
    f.trailerOffset = trailerOffset;
    f.tEndUs = s.lastEndUs;
    f.totalBytes = s.offset + kFooterBytes;
    f.chapterCount = static_cast<uint32_t>(s.index.size());
    f.flags = static_cast<uint32_t>((s.header.flags | s.observedFlags) & ~kFlagOpen);
    f.footerCrc32 = Crc32(&f, 44);
    s.WriteAt(&f, sizeof(f));

    // The only rewrite in the file's life (S5.8). If it does not happen, the recovery path covers it.
    s.header.flags = (s.header.flags | s.observedFlags) & ~kFlagOpen;
    s.header.trailerOffset = trailerOffset;
    s.header.headerCrc32 = HeaderCrc(s.header);
    LARGE_INTEGER zero{};
    if (SetFilePointerEx(s.file, zero, nullptr, FILE_BEGIN)) {
        DWORD done = 0;
        if (!WriteFile(s.file, &s.header, sizeof(s.header), &done, nullptr) || done != sizeof(s.header)) {
            s.Fail("header rewrite failed");
        }
    } else {
        s.Fail("seek to header failed");
    }

    CloseHandle(s.file);
    s.file = INVALID_HANDLE_VALUE;
    if (s.cctx) {
        ZSTD_freeCCtx(s.cctx);
        s.cctx = nullptr;
    }
}

bool DemoWriter::WritePlayerStats(const PlayerStatsRecord& r) {
    Impl& s = *impl_;
    if (!s.inChapter) {
        s.Fail("WritePlayerStats outside a chapter");
        return false;
    }
    if (r.xplatformId == 0 && r.userIndex == kPlayerStatsNoIndex) {
        s.Fail("a player stats record identifies no player");
        return false;
    }
    if ((r.fill & ~kPlayerStatsFillAll) != 0) {
        s.Fail("a player stats record's fill mask has an undefined bit");
        return false;
    }
    if (!PlayerStatsNoteTime(s.statsCursor, r.xplatformId, r.engineTimeUs)) {
        s.Fail("player stats engine time went backwards for a user, or the file names more users "
               "than a match can hold");
        return false;
    }

    PlayerStatsRecordDisk d{};
    PlayerStatsToDisk(r, d);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&d);
    s.statsPending.insert(s.statsPending.end(), p, p + sizeof(d));
    ++s.statsPendingCount;
    return true;
}

bool DemoWriter::SetRoundTable(const RoundRow* rows, size_t count, MarkerSource source) {
    Impl& s = *impl_;
    if (s.file == INVALID_HANDLE_VALUE) {
        s.Fail("SetRoundTable on a closed writer");
        return false;
    }
    if (!s.manifest.SetRoundTable(rows, count, source)) {
        s.Fail("round table exceeds the manifest's row count field");
        return false;
    }
    return true;
}

bool DemoWriter::SetGametypeId(uint64_t gametypeId) {
    Impl& s = *impl_;
    if (s.file == INVALID_HANDLE_VALUE) {
        s.Fail("SetGametypeId on a closed writer");
        return false;
    }
#if !defined(SHIP_MUTATE_NO_LATE_GAMETYPE)
    s.manifest.View().gametypeId = gametypeId;
#endif
    return true;
}

bool DemoWriter::IsOpen() const { return impl_->file != INVALID_HANDLE_VALUE; }
const char* DemoWriter::LastError() const { return impl_->lastError; }
WriterStats DemoWriter::Stats() const { return impl_->stats; }

}  // namespace replay
