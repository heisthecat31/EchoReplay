// Round-trip of the real capture through the container (W1-B acceptance: synthetic AND real data).
//
// The corpus is the retail host/client capture R4 measured its replication basis from. It lives outside
// the repository, so its absence is reported as NOT RUN naming the path it looked for -- never folded
// into a pass.
//
// What this establishes: the container carries real bytes losslessly, and the compress-or-store-raw
// decision is exercised against real entropy rather than a PRNG. What it does not establish: anything
// about record semantics. A record is opaque to the container, which is Appendix B.2's contract;
// field-level fidelity is W1-A's O1 and W2-A's capture.

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <vector>

#include "foundation/demofile/demofile.h"
#include "foundation/demofile/format.h"
#include "synthcap.h"

using namespace replay;

namespace {

// The S5.4.5 record widths, cycled so blocks have the shape a real capture produces rather than one
// uniform size: a player frame and its body half, netvars, an event, a voice frame, an as-seen row.
const uint32_t kRecordWidths[] = {68, 68, 68, 27, 17, 17, 24, 96, 40};
const uint16_t kStreams[] = {kStreamPlayerWire, kStreamPlayerWire, kStreamPlayerWire,
                             kStreamBodyWire,   kStreamNetvar,     kStreamNetvar,
                             kStreamEvent,      kStreamVoip,       kStreamAsSeen};
const uint16_t kSchemas[] = {synth::kSchemaPlayer, synth::kSchemaPlayer, synth::kSchemaPlayer,
                             synth::kSchemaPlayer, synth::kSchemaNetvar, synth::kSchemaNetvar,
                             synth::kSchemaEvent,  synth::kSchemaVoip,   synth::kSchemaAsSeen};
const size_t kVariants = sizeof(kRecordWidths) / sizeof(kRecordWidths[0]);

bool ReadWhole(const wchar_t* path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t got = 0;
    while (got < out.size()) {
        const size_t left = out.size() - got;
        const DWORD want = static_cast<DWORD>(left > 0x100000u ? 0x100000u : left);
        DWORD chunk = 0;
        if (!ReadFile(h, out.data() + got, want, &chunk, nullptr) || chunk == 0) {
            break;
        }
        got += chunk;
    }
    CloseHandle(h);
    return got == out.size();
}

uint64_t FileSizeOf(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    CloseHandle(h);
    return static_cast<uint64_t>(size.QuadPart);
}

struct Block {
    size_t at, bytes;
    uint32_t records;
    uint16_t stream, schema;
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        printf("realdata <corpus.bin> <out.echodemo>\n");
        return 2;
    }
    std::vector<uint8_t> corpus;
    if (!ReadWhole(argv[1], corpus) || corpus.empty()) {
        printf("REALDATA-NOTRUN corpus absent or unreadable: %ls\n", argv[1]);
        return 3;
    }
    printf("corpus           : %ls (%zu bytes)\n", argv[1], corpus.size());

    synth::Config cfg;
    cfg.playerCount = 8;
    Manifest m{};
    synth::BuildManifest(cfg, m);

    std::vector<Block> plan;
    size_t at = 0;
    size_t variant = 0;
    while (at < corpus.size()) {
        const uint32_t width = kRecordWidths[variant % kVariants];
        size_t bytes = size_t(width) * 44;
        if (at + bytes > corpus.size()) {
            bytes = corpus.size() - at;
        }
        const uint32_t records = static_cast<uint32_t>(bytes / width);
        if (records == 0) {
            // The remainder is shorter than one record of this width. It goes in as single-byte records
            // so the round-trip covers the corpus exactly rather than almost.
            const uint32_t tail = static_cast<uint32_t>(corpus.size() - at);
            plan.push_back(Block{at, tail, tail, kStreamEvent, synth::kSchemaEvent});
            at += tail;
            break;
        }
        bytes = size_t(width) * records;
        plan.push_back(Block{at, bytes, records, kStreams[variant % kVariants],
                             kSchemas[variant % kVariants]});
        at += bytes;
        ++variant;
    }
    printf("blocks           : %zu covering all %zu bytes\n", plan.size(), at);

    DemoWriter writer;
    if (!writer.Open(argv[2], m)) {
        printf("cannot open the output\n");
        return 1;
    }
    const size_t blocksPerChapter = 12;
    for (size_t i = 0; i < plan.size(); i += blocksPerChapter) {
        const uint64_t tb = cfg.t0Us + (i / blocksPerChapter) * cfg.chapterSpanUs;
        writer.BeginChapter(kSectionCore, tb);
        for (size_t j = i; j < plan.size() && j < i + blocksPerChapter; ++j) {
            const Block& b = plan[j];
            writer.WriteStreamBlock(b.stream, b.schema, corpus.data() + b.at, b.bytes, b.records,
                                    b.records);
        }
        writer.EndChapter(tb + cfg.chapterSpanUs);
    }
    writer.Close();
    if (writer.LastError()) {
        printf("writer error: %s\n", writer.LastError());
        return 1;
    }

    DemoReader reader;
    if (!reader.Open(argv[2])) {
        printf("cannot re-open: %s\n", reader.RefusalReason());
        return 1;
    }
    if (reader.WasRecovered()) {
        printf("a cleanly closed file needed the recovery scan\n");
        return 1;
    }

    size_t next = 0;
    uint64_t recordsChecked = 0;
    uint64_t bytesChecked = 0;
    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        if (reader.ChapterAt(i).section != kSectionCore) {
            continue;
        }
        DecodedChapter c{};
        if (!reader.DecodeChapter(i, c)) {
            printf("chapter %zu: %s\n", i, ChapterFaultName(reader.LastFault()));
            return 1;
        }
        for (uint16_t b = 0; b < c.streamCount; ++b) {
            const StreamView& v = c.streams[b];
            if (next >= plan.size()) {
                printf("the file holds more blocks than were written\n");
                return 1;
            }
            const Block& want = plan[next];
            if (v.id != want.stream || v.bytes != want.bytes || v.storedCount != want.records) {
                printf("block %zu does not match what was written\n", next);
                return 1;
            }
            if (memcmp(corpus.data() + want.at, v.records, v.bytes) != 0) {
                printf("block %zu did not round-trip byte for byte\n", next);
                return 1;
            }
            recordsChecked += want.records;
            bytesChecked += want.bytes;
            ++next;
        }
    }
    if (next != plan.size()) {
        printf("only %zu of %zu blocks came back\n", next, plan.size());
        return 1;
    }

    const uint64_t onDisk = FileSizeOf(argv[2]);
    printf("chapters         : %zu\n", reader.ChapterCount());
    printf("bytes on disk    : %llu\n", static_cast<unsigned long long>(onDisk));
    // Measured over RAW wire bytes, which is not the S5.10 budget: that figure is for the delta-coded,
    // change-masked, bit-packed stream W2-A produces. This is the container's own contribution alone.
    printf("container ratio  : %.2fx (zstd-3 over raw wire bytes; NOT the S5.10 stream estimate)\n",
           onDisk ? double(bytesChecked) / double(onDisk) : 0.0);
    printf("REALDATA-BYTES %llu\n", static_cast<unsigned long long>(bytesChecked));
    printf("REALDATA-RECORDS %llu\n", static_cast<unsigned long long>(recordsChecked));
    DeleteFileW(argv[2]);
    return 0;
}
