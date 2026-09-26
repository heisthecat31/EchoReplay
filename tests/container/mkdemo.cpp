// Writes a scripted .echodemo. Three roles, one binary: the fixture generator for the gate script, the
// process SC8 kills part-way through, and the producer of the malformed fixtures the negative arms need.

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>

#include <vector>

#include "foundation/demofile/format.h"
#include "foundation/demofile/manifest_io.h"
#include "synthcap.h"
#include "zstd.h"

using namespace replay;

namespace {

bool ReadWhole(const wchar_t* path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    const bool ok = out.empty() ||
                    (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) &&
                     got == out.size());
    CloseHandle(h);
    return ok;
}

bool WriteWhole(const wchar_t* path, const uint8_t* p, size_t n) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD done = 0;
    const bool ok = n == 0 || (WriteFile(h, p, static_cast<DWORD>(n), &done, nullptr) && done == n);
    CloseHandle(h);
    return ok;
}

void Append(std::vector<uint8_t>& out, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
}

// Rewrites a file so its MANIFEST chapter declares Retention::Edge on one schema, which DemoWriter now
// refuses to author. It exists so `echodemo verify`'s arm against a non-conforming writer is executed by
// something rather than trusted: a check no test reaches is the defect F2 recorded.
//
// The patched chapter changes length, so every later offset would move. Rather than fix them up, the
// trailer is dropped and the header is left OPEN -- the reader then recovery-scans, which is exactly how
// it would meet a file from a writer it does not know.
bool WriteLegacyEdge(const wchar_t* path, const synth::Config& cfg, uint16_t schemaId) {
    std::vector<uint8_t> file;
    if (!ReadWhole(path, file) || file.size() < kFileHeaderBytes) {
        return false;
    }

    Manifest edge{};
    synth::BuildManifest(cfg, edge);
    bool found = false;
    std::vector<SchemaDef> schemas(edge.schemas, edge.schemas + edge.schemaCount);
    for (SchemaDef& d : schemas) {
        if (d.id == schemaId) {
            d.retention = Retention::Edge;
            found = true;
        }
    }
    if (!found) {
        printf("no schema %u to relabel\n", schemaId);
        return false;
    }
    edge.schemas = schemas.data();
    std::vector<uint8_t> edgeBytes;
    SerializeManifest(edge, edgeBytes);

    std::vector<uint8_t> out;
    out.reserve(file.size());
    FileHeaderDisk h{};
    memcpy(&h, file.data(), sizeof(h));
    h.flags |= kFlagOpen;
    h.trailerOffset = 0;
    h.headerCrc32 = HeaderCrc(h);
    Append(out, &h, sizeof(h));

    std::vector<uint8_t> body, packed;
    bool patched = false;
    uint64_t at = kFileHeaderBytes;
    while (at + kChapterHeadBytes <= file.size()) {
        ChapterHeadDisk c{};
        memcpy(&c, file.data() + at, sizeof(c));
        if (c.magic != kChapterMagic || Crc32(&c, kChapterHeadCrcCover) != c.headCrc32 ||
            at + kChapterHeadBytes + c.payloadLen > file.size()) {
            break;
        }
        const uint64_t frame = kChapterHeadBytes + c.payloadLen;
        const uint8_t* payload = file.data() + at + kChapterHeadBytes;

        if (c.section == kSectionIndex) {
            at += frame;  // the index would be stale; the reader rebuilds it by scanning
            continue;
        }
        if (c.section != kSectionManifest || patched) {
            Append(out, file.data() + at, static_cast<size_t>(frame));
            at += frame;
            continue;
        }

        body.resize(c.uncompressedLen);
        if (c.codec == kCodecZstd) {
            if (ZSTD_isError(ZSTD_decompress(body.data(), body.size(), payload, c.payloadLen))) {
                return false;
            }
        } else {
            memcpy(body.data(), payload, c.payloadLen);
        }
        StreamBlockDisk sb{};
        if (body.size() < kStreamBlockBytes) {
            return false;
        }
        memcpy(&sb, body.data(), sizeof(sb));
        if (sb.streamId != kStreamManifest || sb.byteLen != body.size() - kStreamBlockBytes ||
            sb.byteLen != edgeBytes.size()) {
            printf("the manifest chapter is not the shape this fixture expects\n");
            return false;
        }
        memcpy(body.data() + kStreamBlockBytes, edgeBytes.data(), edgeBytes.size());

        packed.resize(ZSTD_compressBound(body.size()));
        size_t n = ZSTD_compress(packed.data(), packed.size(), body.data(), body.size(), 3);
        const uint8_t* stored = packed.data();
        c.codec = kCodecZstd;
        if (ZSTD_isError(n) || n >= body.size()) {
            c.codec = kCodecRaw;
            n = body.size();
            stored = body.data();
        }
        c.payloadLen = static_cast<uint32_t>(n);
        c.payloadCrc32 = Crc32(stored, n);
        c.headCrc32 = Crc32(&c, kChapterHeadCrcCover);
        Append(out, &c, sizeof(c));
        Append(out, stored, n);
        patched = true;
        at += frame;
    }
    if (!patched) {
        printf("no MANIFEST chapter found\n");
        return false;
    }

    return WriteWhole(path, out.data(), out.size());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        printf("mkdemo <path> [--chapters N] [--sleep-ms N] [--divisor N] [--divisor-from N]\n"
               "               [--divisor-asseen] [--dishonest-observed] [--lie-stored-from N]\n"
               "               [--legacy-edge N]\n"
               "               [--no-voip] [--no-asseen] [--no-oracle]\n"
               "               [--roster name,name,...]\n");
        return 2;
    }
    synth::Config cfg;
    cfg.chapters = 12;
    int legacyEdgeSchema = -1;
    for (int i = 2; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--chapters") && i + 1 < argc) {
            cfg.chapters = static_cast<uint32_t>(_wtoi(argv[++i]));
        } else if (!wcscmp(argv[i], L"--sleep-ms") && i + 1 < argc) {
            cfg.sleepMsPerChapter = static_cast<uint32_t>(_wtoi(argv[++i]));
        } else if (!wcscmp(argv[i], L"--divisor") && i + 1 < argc) {
            cfg.sampleDivisor = static_cast<uint32_t>(_wtoi(argv[++i]));
        } else if (!wcscmp(argv[i], L"--divisor-from") && i + 1 < argc) {
            cfg.divisorFromChapter = static_cast<uint32_t>(_wtoi(argv[++i]));
        } else if (!wcscmp(argv[i], L"--divisor-asseen")) {
            cfg.divisorThinsAsSeen = true;
        } else if (!wcscmp(argv[i], L"--dishonest-observed")) {
            cfg.honestObserved = false;
        } else if (!wcscmp(argv[i], L"--lie-stored-from") && i + 1 < argc) {
            cfg.lieStoredFromChapter = static_cast<uint32_t>(_wtoi(argv[++i]));
        } else if (!wcscmp(argv[i], L"--legacy-edge") && i + 1 < argc) {
            legacyEdgeSchema = _wtoi(argv[++i]);
        } else if (!wcscmp(argv[i], L"--tapreg") && i + 1 < argc) {
            const wchar_t* mode = argv[++i];
            if (!wcscmp(mode, L"present")) {
                cfg.tapReg = synth::kTapRegPresent;
            } else if (!wcscmp(mode, L"corrupt")) {
                cfg.tapReg = synth::kTapRegCorrupt;
            } else if (!wcscmp(mode, L"table-skew")) {
                cfg.tapReg = synth::kTapRegTableSkew;
            } else if (!wcscmp(mode, L"off")) {
                cfg.tapReg = synth::kTapRegOff;
            } else {
                printf("unknown --tapreg mode %ls\n", mode);
                return 2;
            }
        } else if (!wcscmp(argv[i], L"--roster") && i + 1 < argc) {
            cfg.playerNames.clear();
            const std::wstring list(argv[++i]);
            for (size_t at = 0; at <= list.size();) {
                const size_t comma = list.find(L',', at);
                const size_t end = comma == std::wstring::npos ? list.size() : comma;
                std::string one;
                for (size_t k = at; k < end; ++k) {
                    one.push_back(list[k] < 128 ? static_cast<char>(list[k]) : '?');
                }
                cfg.playerNames.push_back(one);
                if (comma == std::wstring::npos) {
                    break;
                }
                at = comma + 1;
            }
            for (const std::string& one : cfg.playerNames) {
                if (one.empty()) {
                    printf("--roster names must not be empty\n");
                    return 2;
                }
            }
            cfg.playerCount = static_cast<uint32_t>(cfg.playerNames.size());
        } else if (!wcscmp(argv[i], L"--no-voip")) {
            cfg.voip = false;
        } else if (!wcscmp(argv[i], L"--no-asseen")) {
            cfg.asSeen = false;
        } else if (!wcscmp(argv[i], L"--no-oracle")) {
            cfg.oracle = false;
        } else if (!wcscmp(argv[i], L"--oracle-variable")) {
            cfg.oracleVariableWidth = true;
        } else {
            printf("unknown option %ls\n", argv[i]);
            return 2;
        }
    }

    WriterStats stats{};
    uint32_t chapters = 0;
    if (!synth::Write(argv[1], cfg, &stats, &chapters)) {
        printf("write failed\n");
        return 1;
    }
    if (legacyEdgeSchema >= 0 &&
        !WriteLegacyEdge(argv[1], cfg, static_cast<uint16_t>(legacyEdgeSchema))) {
        printf("legacy-edge rewrite failed\n");
        return 1;
    }
    printf("MKDEMO chapters=%u bytes=%llu gaps=%u\n", chapters,
           static_cast<unsigned long long>(stats.bytesWritten), stats.gapRecordsEmitted);
    return 0;
}
