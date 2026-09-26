#include <windows.h>

#include <string.h>

#include <vector>

#include "foundation/demofile/demofile.h"
#include "foundation/demofile/format.h"
#include "foundation/demofile/manifest_io.h"
// zstd 1.3.8 keeps ZSTD_compress2 and the parameter API behind this; we link it statically.
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"
#include "zstd_errors.h"

namespace replay {

const char* ChapterFaultName(ChapterFault f) {
    switch (f) {
        case ChapterFault::None: return "ok";
        case ChapterFault::OutOfRange: return "chapter index out of range";
        case ChapterFault::ShortRead: return "chapter truncated on disk";
        case ChapterFault::PayloadCrc: return "payload_crc32 mismatch (file damaged)";
        case ChapterFault::ZstdCorrupt: return "zstd frame damaged (file damaged)";
        case ChapterFault::ZstdChecksum: return "zstd checksum_wrong (file damaged)";
        case ChapterFault::ZstdSizeMismatch: return "zstd dstSize_tooSmall (writer bug, not damage)";
        case ChapterFault::BlockOverrun: return "stream block overruns the payload";
        case ChapterFault::IndexMismatch: return "the index entry disagrees with the frame it points at";
        default: return "?";
    }
}

struct DemoReader::Impl {
    HANDLE file = INVALID_HANDLE_VALUE;
    uint64_t fileSize = 0;
    FileHeaderDisk header{};
    FileFooterDisk footer{};
    ManifestStore manifest;
    std::vector<IndexEntryDisk> index;
    std::vector<uint32_t> bySection[kSectionCount];
    std::vector<uint8_t> raw;
    std::vector<uint8_t> decoded[2];
    std::vector<StreamView> views[2];
    std::vector<uint8_t> scanWindow;
    int nextBuf = 0;
    ZSTD_DCtx* dctx = nullptr;
    bool recovered = false;
    const char* refusal = nullptr;
    ChapterFault fault = ChapterFault::None;
    ReaderStats stats{};

    bool ReadAt(uint64_t off, void* dst, uint32_t n) {
        if (off + n > fileSize) {
            return false;
        }
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(off);
        if (!SetFilePointerEx(file, li, nullptr, FILE_BEGIN)) {
            return false;
        }
        uint8_t* p = static_cast<uint8_t*>(dst);
        uint32_t got = 0;
        while (got < n) {
            DWORD chunk = 0;
            if (!ReadFile(file, p + got, n - got, &chunk, nullptr) || chunk == 0) {
                return false;
            }
            got += chunk;
        }
        return true;
    }

    bool ReadChapterHead(uint64_t off, ChapterHeadDisk& h) {
        if (!ReadAt(off, &h, sizeof(h))) {
            return false;
        }
        if (h.magic != kChapterMagic) {
            return false;
        }
        if (Crc32(&h, kChapterHeadCrcCover) != h.headCrc32) {
            return false;
        }
        // Both lengths size a buffer, and a head_crc32 only proves the head is self-consistent -- anyone
        // who can edit the file can recompute it. The writer never emits a chapter whose payload exceeds
        // the cap in either form (EndChapter groups blocks under it and stores raw when zstd would grow
        // the frame), so a larger claim is not a chapter we wrote and never reaches a resize.
        if (h.payloadLen > kMaxChapterPayload || h.uncompressedLen > kMaxChapterPayload) {
            return false;
        }
        if (h.codec != kCodecZstd && h.codec != kCodecRaw) {
            return false;
        }
        return off + kChapterHeadBytes + h.payloadLen <= fileSize;
    }

    // A head that fails its CRC is one damaged chapter, not the end of the file. S5.8 stops the scan
    // there, which is right for a torn tail and wrong in the middle: the chapters after it are intact
    // and independently framed. Resync finds the next frame so the damage costs exactly one chapter.
    // The search is bounded by one chapter: a valid frame after a damaged head can be at most
    // kChapterHeadBytes + kMaxChapterPayload away, so scanning further would only cost time.
    bool Resync(uint64_t from, uint64_t& out) {
        constexpr uint32_t kWindow = 64u << 10;
        const uint64_t limit = from + kChapterHeadBytes + kMaxChapterPayload;
        scanWindow.resize(kWindow);
        uint64_t at = from;
        while (at + kChapterHeadBytes <= fileSize && at < limit) {
            const uint32_t want = static_cast<uint32_t>(
                (fileSize - at) < kWindow ? (fileSize - at) : kWindow);
            if (!ReadAt(at, scanWindow.data(), want)) {
                return false;
            }
            if (want < kChapterHeadBytes) {
                return false;
            }
            for (uint32_t i = 0; i + kChapterHeadBytes <= want; ++i) {
                uint32_t magic;
                memcpy(&magic, scanWindow.data() + i, sizeof(magic));
                if (magic != kChapterMagic) {
                    continue;
                }
                ChapterHeadDisk h{};
                if (ReadChapterHead(at + i, h)) {
                    out = at + i;
                    return true;
                }
            }
            // Overlap by one header so a frame straddling the window boundary is not missed.
            at += (want >= kChapterHeadBytes) ? (want - kChapterHeadBytes + 1) : want;
        }
        return false;
    }

    void Reindex() {
        for (uint32_t s = 0; s < kSectionCount; ++s) {
            bySection[s].clear();
        }
        for (uint32_t i = 0; i < index.size(); ++i) {
            const uint16_t s = index[i].section;
            if (s < kSectionCount) {
                bySection[s].push_back(i);
            }
        }
        stats.chaptersIndexed = static_cast<uint32_t>(index.size());
    }
};

DemoReader::DemoReader() : impl_(new Impl) {}

DemoReader::~DemoReader() {
    Close();
    delete impl_;
}

void DemoReader::Close() {
    Impl& s = *impl_;
    if (s.file != INVALID_HANDLE_VALUE) {
        CloseHandle(s.file);
        s.file = INVALID_HANDLE_VALUE;
    }
    if (s.dctx) {
        ZSTD_freeDCtx(s.dctx);
        s.dctx = nullptr;
    }
}

bool DemoReader::Open(const wchar_t* path) {
    Impl& s = *impl_;
    Close();
    // A refusal names its cause and leaves nothing open; RefusalReason() outlives the handle.
    auto refuse = [&](const char* why) {
        s.refusal = why;
        Close();
        return false;
    };
    s.index.clear();
    s.recovered = false;
    s.refusal = nullptr;
    s.fault = ChapterFault::None;
    s.stats = ReaderStats{};

    s.file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s.file == INVALID_HANDLE_VALUE) {
        return refuse("cannot open the file");
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(s.file, &size)) {
        return refuse("cannot size the file");
    }
    s.fileSize = static_cast<uint64_t>(size.QuadPart);

    if (!s.ReadAt(0, &s.header, sizeof(s.header))) {
        return refuse("shorter than a FileHeader");
    }
    if (memcmp(s.header.magic, kFileMagic, sizeof(kFileMagic)) != 0) {
        return refuse("not an .echodemo (magic mismatch)");
    }
    if (HeaderCrc(s.header) != s.header.headerCrc32) {
        return refuse("FileHeader crc32 mismatch");
    }
    if (s.header.formatMajor != kFormatMajor) {
        // S5.9: a reader that meets an unknown format_major refuses and says so; it never guesses.
        return refuse("unknown format_major");
    }

    s.dctx = ZSTD_createDCtx();
    if (!s.dctx) {
        return refuse("ZSTD_createDCtx failed");
    }

    if (s.fileSize >= kFileHeaderBytes + kFooterBytes) {
        FileFooterDisk f{};
        if (s.ReadAt(s.fileSize - kFooterBytes, &f, sizeof(f)) &&
            memcmp(f.magic, kFooterMagic, sizeof(kFooterMagic)) == 0 && Crc32(&f, 44) == f.footerCrc32) {
            s.footer = f;
            s.stats.footerPresent = true;
        }
    }

    const bool clean = (s.header.flags & kFlagOpen) == 0 && s.header.trailerOffset >= kFileHeaderBytes;
    bool haveIndex = false;
    if (clean) {
        ChapterHeadDisk h{};
        if (s.ReadChapterHead(s.header.trailerOffset, h) && h.section == kSectionIndex) {
            DecodedChapter trailer{};
            s.index.push_back(IndexEntryDisk{h.tBeginUs, h.tEndUs, s.header.trailerOffset, h.payloadLen,
                                             h.section, static_cast<uint16_t>(h.seq)});
            s.Reindex();
            if (DecodeChapter(0, trailer)) {
                const StreamView* iv = trailer.Find(kStreamIndex);
                const StreamView* mv = trailer.Find(kStreamManifest);
                if (iv && iv->bytes % sizeof(IndexEntryDisk) == 0) {
                    const size_t n = iv->bytes / sizeof(IndexEntryDisk);
                    std::vector<IndexEntryDisk> entries(n);
                    if (n) {
                        memcpy(entries.data(), iv->records, iv->bytes);
                    }
                    // The trailer's snapshot is preferred because it carries the round table mirrored
                    // at close (S5.7). Both blocks live in one chapter today, but a very long session
                    // can push the manifest into the trailer's continuation frame, so its absence falls
                    // back to the MANIFEST chapter the index already points at rather than to a scan.
                    // A manifest block that is PRESENT and malformed is a different thing from one that
                    // is absent, and only absence licenses the fallback. Falling back on a malformed
                    // block would answer from the opening MANIFEST chapter, which was written before the
                    // round table existed -- turning a corrupt table into a silent "not recorded".
                    const char* err = nullptr;
                    bool haveManifest = false;
                    if (mv) {
                        haveManifest = DeserializeManifest(mv->records, mv->bytes, s.manifest, &err);
                        if (!haveManifest) {
                            return refuse(err ? err : "MANIFEST: the trailer manifest is malformed");
                        }
                    }
                    s.index.swap(entries);
                    s.Reindex();
                    for (uint32_t i : s.bySection[kSectionManifest]) {
                        if (haveManifest) {
                            break;
                        }
                        DecodedChapter c{};
                        const StreamView* m2 = DecodeChapter(i, c) ? c.Find(kStreamManifest) : nullptr;
                        haveManifest = m2 && DeserializeManifest(m2->records, m2->bytes, s.manifest, &err);
                    }
                    if (haveManifest) {
                        s.stats.fastPath = true;
                        s.stats.trailerPresent = true;
                        haveIndex = true;
                    }
                }
            }
            if (!haveIndex) {
                s.index.clear();
            }
        }
    }

    if (!haveIndex) {
        // Recovery scan (S5.8). Everything it records is playable; the payload CRC is checked lazily.
        s.recovered = true;
        uint64_t at = s.header.firstChapterOffset;
        if (at < kFileHeaderBytes || at >= s.fileSize) {
            at = kFileHeaderBytes;
        }
        while (at + kChapterHeadBytes <= s.fileSize) {
            ChapterHeadDisk h{};
            if (!s.ReadChapterHead(at, h)) {
                uint64_t next = 0;
                if (!s.Resync(at + 1, next)) {
                    s.stats.truncatedTailBytes = s.fileSize - at;
                    break;
                }
                s.stats.skippedBytes += next - at;
                ++s.stats.resyncs;
                at = next;
                continue;
            }
            if (h.section != kSectionIndex) {
                s.index.push_back(IndexEntryDisk{h.tBeginUs, h.tEndUs, at, h.payloadLen, h.section,
                                                 static_cast<uint16_t>(h.seq)});
            } else {
                s.stats.trailerPresent = true;
            }
            at += kChapterHeadBytes + h.payloadLen;
        }
        s.Reindex();

        // The manifest is required to play (S5.4), so its absence is a refusal that names itself.
        bool gotManifest = false;
        const char* manifestErr = nullptr;
        for (uint32_t i : s.bySection[kSectionManifest]) {
            DecodedChapter c{};
            if (!DecodeChapter(i, c)) {
                continue;
            }
            const StreamView* mv = c.Find(kStreamManifest);
            const char* err = nullptr;
            if (mv && DeserializeManifest(mv->records, mv->bytes, s.manifest, &err)) {
                gotManifest = true;
                break;
            }
            if (mv && err) {
                manifestErr = err;  // a block we found and could not read names its own fault
            }
        }
        if (!gotManifest) {
            return refuse(manifestErr ? manifestErr : "MANIFEST chapter missing or damaged");
        }
    }

    s.Reindex();
    s.manifest.FillFromHeader(s.header);
    s.manifest.Rebind();
    return true;
}

const Manifest& DemoReader::GetManifest() const { return impl_->manifest.View(); }

size_t DemoReader::ChapterCount() const { return impl_->index.size(); }

ChapterKey DemoReader::ChapterAt(size_t i) const {
    const Impl& s = *impl_;
    if (i >= s.index.size()) {
        return ChapterKey{0, 0, 0xFFFFu, 0};
    }
    const IndexEntryDisk& e = s.index[i];
    return ChapterKey{e.tBeginUs, e.tEndUs, e.section, e.seq};
}

uint64_t DemoReader::ChapterOffset(size_t i) const {
    return i < impl_->index.size() ? impl_->index[i].offset : 0;
}

uint32_t DemoReader::ChapterPayloadLen(size_t i) const {
    return i < impl_->index.size() ? impl_->index[i].len : 0;
}

uint64_t DemoReader::FileFlags() const { return impl_->header.flags; }

size_t DemoReader::FindChapter(uint64_t tUs, uint16_t section) const {
    const Impl& s = *impl_;
    if (section >= kSectionCount || s.bySection[section].empty()) {
        return static_cast<size_t>(-1);
    }
    const std::vector<uint32_t>& list = s.bySection[section];
    size_t lo = 0, hi = list.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (s.index[list[mid]].tBeginUs <= tUs) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return static_cast<size_t>(-1);
    }
    return list[lo - 1];
}

bool DemoReader::DecodeChapter(size_t i, DecodedChapter& out) {
    Impl& s = *impl_;
    s.fault = ChapterFault::None;
    if (i >= s.index.size()) {
        s.fault = ChapterFault::OutOfRange;
        return false;
    }
    const IndexEntryDisk& e = s.index[i];

    ChapterHeadDisk h{};
    if (!s.ReadChapterHead(e.offset, h)) {
        s.fault = ChapterFault::ShortRead;
        return false;
    }
    // The index is derived from these frames rather than a second copy of them (S5.2), so the two
    // agreeing is a checkable property and not a formality.
    if (h.section != e.section || h.payloadLen != e.len || h.tBeginUs != e.tBeginUs) {
        s.fault = ChapterFault::IndexMismatch;
        return false;
    }

    s.raw.resize(h.payloadLen ? h.payloadLen : 1);
    if (h.payloadLen && !s.ReadAt(e.offset + kChapterHeadBytes, s.raw.data(), h.payloadLen)) {
        s.fault = ChapterFault::ShortRead;
        return false;
    }
    if (Crc32(s.raw.data(), h.payloadLen) != h.payloadCrc32) {
        s.fault = ChapterFault::PayloadCrc;
        return false;
    }

    const int slot = s.nextBuf;
    std::vector<uint8_t>& body = s.decoded[slot];
    std::vector<StreamView>& views = s.views[slot];

    if (h.codec == kCodecZstd) {
        body.resize(h.uncompressedLen ? h.uncompressedLen : 1);
        const size_t rc = ZSTD_decompressDCtx(s.dctx, body.data(), h.uncompressedLen, s.raw.data(),
                                              h.payloadLen);
        if (ZSTD_isError(rc)) {
            // S5.8 requires these apart: checksum_wrong means the bytes on disk are damaged, and
            // dstSize_tooSmall means we sized the destination wrong. Reporting our bug as corruption
            // would send someone looking at the wrong file.
            switch (ZSTD_getErrorCode(rc)) {
                case ZSTD_error_checksum_wrong:
                    s.fault = ChapterFault::ZstdChecksum;
                    break;
                case ZSTD_error_dstSize_tooSmall:
                    s.fault = ChapterFault::ZstdSizeMismatch;
                    break;
                default:
                    s.fault = ChapterFault::ZstdCorrupt;
                    break;
            }
            return false;
        }
        if (rc != h.uncompressedLen) {
            s.fault = ChapterFault::ZstdSizeMismatch;
            return false;
        }
    } else {
        if (h.uncompressedLen != h.payloadLen) {
            s.fault = ChapterFault::BlockOverrun;
            return false;
        }
        body.assign(s.raw.begin(), s.raw.begin() + h.payloadLen);
    }

    const uint32_t bodyBytes = h.uncompressedLen;
    views.clear();
    uint32_t at = 0;
    for (uint16_t b = 0; b < h.streamCount; ++b) {
        if (at + kStreamBlockBytes > bodyBytes) {
            s.fault = ChapterFault::BlockOverrun;
            return false;
        }
        StreamBlockDisk bh{};
        memcpy(&bh, body.data() + at, sizeof(bh));
        at += kStreamBlockBytes;
        if (uint64_t(at) + bh.byteLen > bodyBytes) {
            s.fault = ChapterFault::BlockOverrun;
            return false;
        }
        StreamView v{};
        v.id = bh.streamId;
        v.schema = bh.schemaId;
        // The 16-byte block header carries no separate record count: `stored` IS the number of records
        // the writer placed in this block (S5.2 / S5.6.2).
        v.recordCount = bh.stored;
        v.observedCount = bh.observed;
        v.storedCount = bh.stored;
        v.records = body.data() + at;
        v.bytes = bh.byteLen;
        views.push_back(v);
        at += bh.byteLen;
    }

    out.key = ChapterKey{h.tBeginUs, h.tEndUs, h.section, static_cast<uint16_t>(h.seq)};
    out.streamCount = static_cast<uint16_t>(views.size());
    out.streams = views.empty() ? nullptr : views.data();
    s.nextBuf ^= 1;
    return true;
}

bool DemoReader::WasRecovered() const { return impl_->recovered; }
const char* DemoReader::RefusalReason() const { return impl_->refusal; }
ChapterFault DemoReader::LastFault() const { return impl_->fault; }
ReaderStats DemoReader::Stats() const { return impl_->stats; }

}  // namespace replay
