#pragma once

#include <stddef.h>
#include <stdint.h>

#include "foundation/demofile/demotypes.h"

// Appendix B.2, frozen. The concrete types add non-virtual accessors for things the frozen signatures
// have no room for -- a writer error channel and the reader's recovery detail -- and nothing else.

namespace replay {

class IDemoWriter {
public:
    virtual ~IDemoWriter() = default;
    virtual bool Open(const wchar_t* path, const Manifest&) = 0;
    // observed/stored are S5.6.2's completeness counters; `observed` MUST come from the engine's own
    // numbering (frame index, wire sequence, history-list depth) and never from the sampler's own loop.
    virtual void BeginChapter(uint16_t section, uint64_t tBeginUs) = 0;
    virtual void WriteStreamBlock(uint16_t streamId, uint16_t schemaId, const void* data, size_t bytes,
                                  uint32_t observed, uint32_t stored) = 0;
    virtual void EndChapter(uint64_t tEndUs) = 0;
    virtual void Close() = 0;  // INDEX, footer, then the single header rewrite
};

class IDemoReader {
public:
    virtual ~IDemoReader() = default;
    virtual bool Open(const wchar_t* path) = 0;  // fast path or recovery scan, transparently
    virtual const Manifest& GetManifest() const = 0;
    virtual size_t ChapterCount() const = 0;
    virtual ChapterKey ChapterAt(size_t i) const = 0;
    virtual size_t FindChapter(uint64_t tUs, uint16_t section) const = 0;
    // `out` and everything it points at stay valid across exactly one further successful DecodeChapter
    // on this reader, and are overwritten by the second. Two chapters live at once, never three -- see
    // the lifetime note on DecodedChapter in demotypes.h.
    virtual bool DecodeChapter(size_t i, DecodedChapter& out) = 0;
    virtual bool WasRecovered() const = 0;
    virtual const char* RefusalReason() const = 0;  // non-null iff Open failed; names the cause
};

// Why a chapter could not be decoded. S5.8 needs checksum_wrong (the file is damaged) kept apart from
// dstSize_tooSmall (we sized a buffer wrong); collapsing them would report our own bug as corruption.
enum class ChapterFault : uint8_t {
    None = 0,
    OutOfRange,
    ShortRead,
    PayloadCrc,       // the chapter is damaged
    ZstdCorrupt,      // zstd agrees the frame is damaged
    ZstdChecksum,     // zstd's own content checksum failed: damaged, and our CRC missed it
    ZstdSizeMismatch, // dstSize_tooSmall or a short decode: OUR bug, not the file's
    BlockOverrun,     // a stream block claims more bytes than the payload holds
    IndexMismatch,    // the index entry and the frame it points at describe different chapters
};

const char* ChapterFaultName(ChapterFault f);

struct WriterStats {
    uint32_t chaptersWritten;
    uint32_t gapRecordsEmitted;      // deficits the writer had to record itself
    uint32_t observedBelowStored;    // blocks whose counters were impossible; a caller bug
    uint32_t unknownSchemaBlocks;    // a block citing a schema the manifest does not carry
    uint32_t oversizeBlocksRefused;  // a single block larger than one chapter
    uint32_t blocksOutsideChapter;   // WriteStreamBlock with no chapter open
    uint64_t bytesWritten;
};

class DemoWriter final : public IDemoWriter {
public:
    DemoWriter();
    ~DemoWriter() override;

    bool Open(const wchar_t* path, const Manifest&) override;
    void BeginChapter(uint16_t section, uint64_t tBeginUs) override;
    void WriteStreamBlock(uint16_t streamId, uint16_t schemaId, const void* data, size_t bytes,
                          uint32_t observed, uint32_t stored) override;
    void EndChapter(uint64_t tEndUs) override;
    void Close() override;

    // ARCH-21's MANIFEST round table, which is only decided at close: Appendix B.2's frozen signatures
    // have no room for it, so it is a non-virtual accessor like the writer's error channel. Call it
    // before Close; the rows land in the TRAILER manifest, which is the copy a reader trusts. `source`
    // is recorded even for an empty table -- a file whose writer chose `None` and a file written before
    // this block existed are different answers and must not read the same.
    bool SetRoundTable(const RoundRow* rows, size_t count, MarkerSource source);

    // The gametype, when it only became known after Open. Same seam and same rule as the round
    // table: call it before Close and it lands in the TRAILER manifest, the copy a reader trusts.
    bool SetGametypeId(uint64_t gametypeId);

    // One player's live match stats at one instant (ARCH-21 MAJOR-B2). Call it whenever a value moves
    // and once per player at every keyframe; the records buffer inside the open chapter and land as one
    // S_PLAYER_STATS block when it ends, so a reader that starts at any chapter has a full scoreboard.
    // False, with LastError() naming the reason, when the record cannot be written: no chapter is open,
    // it identifies no player, its fill mask has a bit this format does not define, or its engine time
    // is behind the last record for the same user -- which would make the stream unsearchable.
    bool WritePlayerStats(const PlayerStatsRecord& r);

    bool IsOpen() const;
    const char* LastError() const;
    WriterStats Stats() const;

private:
    struct Impl;
    Impl* impl_;
};

struct ReaderStats {
    uint32_t chaptersIndexed;
    uint32_t resyncs;          // head corruptions the scan stepped over instead of stopping at
    uint64_t skippedBytes;     // bytes the scan could not attribute to a chapter
    uint64_t truncatedTailBytes;
    bool fastPath;             // the trailer index was read; no scan was needed
    bool trailerPresent;
    bool footerPresent;
};

class DemoReader final : public IDemoReader {
public:
    DemoReader();
    ~DemoReader() override;

    bool Open(const wchar_t* path) override;
    const Manifest& GetManifest() const override;
    size_t ChapterCount() const override;
    ChapterKey ChapterAt(size_t i) const override;
    size_t FindChapter(uint64_t tUs, uint16_t section) const override;
    bool DecodeChapter(size_t i, DecodedChapter& out) override;
    bool WasRecovered() const override;
    const char* RefusalReason() const override;

    void Close();
    ChapterFault LastFault() const;
    ReaderStats Stats() const;
    uint64_t ChapterOffset(size_t i) const;
    uint32_t ChapterPayloadLen(size_t i) const;
    uint64_t FileFlags() const;

private:
    struct Impl;
    Impl* impl_;
};

// Rewrites `in` to `out` without the listed sections, clearing the matching header flags and rebuilding
// the index, the trailer and the footer. `sectionMask` is a bitmask of (1 << SectionId).
// S5.4.6 control 2: the privacy affordance, and it is verified rather than assumed -- the result is
// re-opened and re-scanned, and the call fails if a single byte of a stripped section survives.
struct StripResult {
    uint32_t chaptersIn;
    uint32_t chaptersOut;
    uint32_t chaptersDropped;
    uint32_t chaptersUndecodable;  // dropped because they could not be inspected; reported, never silent
    uint32_t strippedBlocksInKeptChapters;
    uint64_t bytesRemoved;
    bool verified;
    const char* error;
};

StripResult StripSections(const wchar_t* in, const wchar_t* out, uint32_t sectionMask);

}  // namespace replay
