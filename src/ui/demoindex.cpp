#include "ui/demoindex.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "bootstrap/buildguard.h"
#include "bootstrap/log.h"
#include "bootstrap/paths.h"
#include "foundation/demofile/demofile.h"
#include "foundation/demofile/format.h"
#include "ui/legacyimport.h"

namespace replay {
namespace ui {
namespace {

const char* const kReplayModuleName = "ui";

uint64_t NowMicros() {
    LARGE_INTEGER f, c;
    if (!::QueryPerformanceFrequency(&f) || f.QuadPart == 0) return 0;
    ::QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart) * 1000000ull / static_cast<uint64_t>(f.QuadPart);
}

std::string Narrow(const wchar_t* w) {
    if (w == nullptr) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string out(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
    return out;
}

bool ReadAt(HANDLE h, uint64_t offset, void* buf, uint32_t bytes) {
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (::SetFilePointerEx(h, li, nullptr, FILE_BEGIN) == 0) return false;
    DWORD got = 0;
    if (::ReadFile(h, buf, bytes, &got, nullptr) == 0) return false;
    return got == bytes;
}

// The md5 the whole registry was measured against, as 16 raw bytes, so a capture taken on another
// build can be badged rather than silently trusted.
bool ExpectedMd5Bytes(uint8_t out[16]) {
    const char* hex = ExpectedMd5();
    if (hex == nullptr || strlen(hex) != 32) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < 16; ++i) {
        const int hi = nibble(hex[i * 2]);
        const int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

bool EndsWithIgnoringCase(const wchar_t* s, const wchar_t* suffix) {
    const size_t n = wcslen(s);
    const size_t k = wcslen(suffix);
    return n >= k && _wcsicmp(s + n - k, suffix) == 0;
}

bool SameDir(std::wstring a, std::wstring b) {
    while (!a.empty() && (a.back() == L'\\' || a.back() == L'/')) a.pop_back();
    while (!b.empty() && (b.back() == L'\\' || b.back() == L'/')) b.pop_back();
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

}  // namespace

const char* BuildMatchName(BuildMatch m) {
    switch (m) {
        case BuildMatch::NotChecked: return "NotChecked";
        case BuildMatch::Matches: return "Matches";
        case BuildMatch::Mismatch: return "Mismatch";
        case BuildMatch::NotRecorded: return "NotRecorded";
    }
    return "<invalid>";
}

BuildMatch ClassifyBuildMatch(const uint8_t recordedMd5[16], const uint8_t* expectedMd5) {
    if (expectedMd5 == nullptr) return BuildMatch::NotChecked;
    static const uint8_t kUnrecorded[16] = {};
    if (memcmp(recordedMd5, kUnrecorded, 16) == 0) return BuildMatch::NotRecorded;
    return memcmp(recordedMd5, expectedMd5, 16) != 0 ? BuildMatch::Mismatch : BuildMatch::Matches;
}

const char* RowFaultName(RowFault f) {
    switch (f) {
        case RowFault::None: return "None";
        case RowFault::Unreadable: return "Unreadable";
        case RowFault::ShortFile: return "ShortFile";
        case RowFault::BadMagic: return "BadMagic";
        case RowFault::HeaderCrc: return "HeaderCrc";
        case RowFault::NoFooter: return "NoFooter";
        case RowFault::ManifestRefused: return "ManifestRefused";
    }
    return "<invalid>";
}

DemoIndex::DemoIndex() = default;
DemoIndex::~DemoIndex() = default;

bool DemoIndex::ReadStage1(Row& row) {
    row.stage1Attempted = true;
    fileio::Note();
    if (row.legacy) {
        // The source is never opened here. Without a cached conversion the row has its name, size
        // and time and nothing else, and says so through LegacyCached().
        row.cached = !row.readPath.empty() &&
                     ::GetFileAttributesW(row.readPath.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (!row.cached) return true;
    }
    HANDLE h = ::CreateFileW(row.readPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        row.fault = RowFault::Unreadable;
        char buf[128];
        snprintf(buf, sizeof buf, "CreateFileW failed, error %lu", ::GetLastError());
        row.faultDetail = buf;
        return false;
    }

    LARGE_INTEGER size;
    if (::GetFileSizeEx(h, &size) == 0 ||
        static_cast<uint64_t>(size.QuadPart) < kFileHeaderBytes + kFooterBytes) {
        row.fault = RowFault::ShortFile;
        row.faultDetail = "smaller than a 128-byte header plus a 48-byte footer";
        ::CloseHandle(h);
        return false;
    }
    row.summary.sizeBytes = static_cast<uint64_t>(size.QuadPart);

    FileHeaderDisk hdr = {};
    if (!ReadAt(h, 0, &hdr, sizeof hdr)) {
        row.fault = RowFault::Unreadable;
        row.faultDetail = "the first 128 bytes could not be read";
        ::CloseHandle(h);
        return false;
    }
    if (memcmp(hdr.magic, kFileMagic, sizeof kFileMagic) != 0) {
        row.fault = RowFault::BadMagic;
        row.faultDetail = "not an ECHODEMO file";
        ::CloseHandle(h);
        return false;
    }
    if (HeaderCrc(hdr) != hdr.headerCrc32) {
        // Listed anyway, with the fault shown. A damaged header still tells us the file exists, and
        // hiding it would leave the user staring at a folder that has their match in it.
        row.fault = RowFault::HeaderCrc;
        row.faultDetail = "the header CRC does not match; the values shown may be wrong";
    }

    row.summary.flags = static_cast<uint32_t>(hdr.flags);

    uint8_t expected[16];
    row.buildMatch = ClassifyBuildMatch(hdr.echovrMd5, ExpectedMd5Bytes(expected) ? expected
                                                                                 : nullptr);
    // else: left NotChecked. Not "matches" -- we never compared anything.

    FileFooterDisk foot = {};
    const uint64_t footAt = row.summary.sizeBytes - kFooterBytes;
    if (ReadAt(h, footAt, &foot, sizeof foot) &&
        memcmp(foot.magic, kFooterMagic, sizeof kFooterMagic) == 0 &&
        Crc32(&foot, 44) == foot.footerCrc32) {
        row.summary.durationUs = foot.tEndUs > hdr.t0Us ? foot.tEndUs - hdr.t0Us : 0;
    } else if (row.fault == RowFault::None) {
        // No usable footer means the capture was not closed cleanly. The duration is unknown, and
        // it stays unknown: guessing it from the file size would put a plausible wrong number in
        // front of someone about to scrub to a timestamp that is not there.
        row.fault = RowFault::NoFooter;
        row.faultDetail = "no valid footer: the capture was not closed cleanly, so its duration is "
                          "unknown until the file is opened";
    }

    ::CloseHandle(h);
    return row.fault == RowFault::None;
}

bool DemoIndex::Scan(const wchar_t* dir) {
    rows_.clear();
    lastScanMicros_ = 0;
    lastScanFiles_ = 0;
    lastScanError_ = 0;
    if (dir == nullptr) {
        lastScanError_ = ERROR_INVALID_NAME;
        return false;
    }

    const uint64_t t0 = NowMicros();
    scanDir_ = dir;
    uint32_t err = 0;
    if (!ListDir(scanDir_, true, &err)) {
        // "no replays found" and "the replay folder could not be read" send a user to completely
        // different places, so the second is a failure with its OS code, never an empty listing.
        rows_.clear();
        lastScanMicros_ = NowMicros() - t0;
        lastScanError_ = err;
        return false;
    }
    for (const std::wstring& importDir : importDirs_) {
        if (SameDir(importDir, scanDir_)) continue;
        uint32_t importErr = 0;
        // Spark's default folder is simply absent on a machine that never ran Spark; that is not
        // worth a line per rescan. Anything else is.
        if (!ListDir(importDir, false, &importErr) && importErr != ERROR_PATH_NOT_FOUND &&
            importErr != ERROR_FILE_NOT_FOUND) {
            log::Direct(LogLevel::Warn, kReplayModuleName,
                        "browser scan: import folder %s could not be read (error %lu)",
                        Narrow(importDir.c_str()).c_str(), static_cast<unsigned long>(importErr));
        }
    }

    std::sort(rows_.begin(), rows_.end(),
              [](const Row& a, const Row& b) { return a.summary.mtime > b.summary.mtime; });
    // The borrowed pointers must survive the sort, so they are bound after the vector has stopped
    // moving.
    RebindBorrowedPointers();

    lastScanFiles_ = rows_.size();
    lastScanMicros_ = NowMicros() - t0;
    log::Direct(LogLevel::Info, kReplayModuleName, "browser scan: %zu file(s) in %llu us (%s)",
                rows_.size(), static_cast<unsigned long long>(lastScanMicros_), Narrow(dir).c_str());
    return true;
}

bool DemoIndex::ListDir(const std::wstring& dir, bool withCaptures, uint32_t* osError) {
    std::wstring prefix(dir);
    if (!prefix.empty() && prefix.back() != L'\\' && prefix.back() != L'/') prefix += L'\\';

    WIN32_FIND_DATAW fd = {};
    fileio::Note();
    HANDLE find = ::FindFirstFileW((prefix + L"*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_NO_MORE_FILES) return true;
        *osError = static_cast<uint32_t>(err);
        return false;
    }
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        const bool legacy = legacy::IsLegacySource(fd.cFileName);
        if (!legacy && !(withCaptures && EndsWithIgnoringCase(fd.cFileName, L".echodemo"))) continue;
        Row row;
        row.path = prefix + fd.cFileName;
        row.displayName = Narrow(fd.cFileName);
        row.summary.mtime = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                            fd.ftLastWriteTime.dwLowDateTime;
        if (legacy) {
            row.legacy = true;
            row.summary.sizeBytes =
                (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            wchar_t cachePath[kPathMax];
            if (legacy::CachePathFor(scanDir_.c_str(), row.path.c_str(), row.summary.sizeBytes,
                                     row.summary.mtime, cachePath, kPathMax)) {
                row.readPath = cachePath;
            }
        } else {
            row.readPath = row.path;
        }
        rows_.push_back(std::move(row));
    } while (::FindNextFileW(find, &fd) != 0);
    ::FindClose(find);
    return true;
}

void DemoIndex::SetImportDirs(std::vector<std::wstring> dirs) { importDirs_ = std::move(dirs); }

bool DemoIndex::IsLegacy(size_t i) const { return i < rows_.size() && rows_[i].legacy; }

bool DemoIndex::LegacyCached(size_t i) const {
    return i < rows_.size() && rows_[i].legacy && rows_[i].cached;
}

size_t DemoIndex::AddOne(const wchar_t* path) {
    if (path == nullptr || path[0] == L'\0') return rows_.size();
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (_wcsicmp(rows_[i].path.c_str(), path) == 0) {
            ReadRow(i);
            return i;
        }
    }

    Row row;
    row.path = path;
    row.readPath = path;
    const wchar_t* leaf = row.path.c_str();
    for (const wchar_t* p = leaf; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/') leaf = p + 1;
    }
    row.displayName = Narrow(leaf);

    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (::GetFileAttributesExW(path, GetFileExInfoStandard, &fad) != 0) {
        row.summary.mtime = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                            fad.ftLastWriteTime.dwLowDateTime;
    }
    ReadStage1(row);

    // Appended, never re-sorted: a rescan is the only thing that reorders rows, so an index a
    // caller is holding across this call stays pointing at the same capture.
    rows_.push_back(std::move(row));
    RebindBorrowedPointers();
    return rows_.size() - 1;
}

// ★ EVERY POINTER A DemoSummary BORROWS FROM ITS OWN Row, REBOUND IN ONE PLACE.
//
// A push_back that reallocates move-constructs every Row into new storage, and `summary.manifest`
// is the address of the Manifest SUBOBJECT inside a Row -- so it moves with the Row while
// `summary` keeps the old address. This loop used to repair `summary.path` alone, which left the
// browser reading `s.manifest->levelId` through freed memory on the frame after the owner opened a
// capture from outside the replay folder.
//
// It is a function rather than two loops because the two call sites are the two places rows_ can
// move, and the defect was exactly that one of them knew about a pointer the other did not.
// `manifest` is bound from manifestLoaded rather than from its current value: a row that has not
// been read has no manifest, and NULL is what the browser's "not checked" state is keyed on.
void DemoIndex::RebindBorrowedPointers() {
    for (Row& r : rows_) {
        r.summary.path = r.path.c_str();
        r.summary.manifest = r.manifestLoaded ? &r.manifest.View() : nullptr;
    }
}

size_t DemoIndex::Count() const { return rows_.size(); }

bool DemoIndex::ReadRow(size_t i) {
    if (i >= rows_.size()) return false;
    Row& row = rows_[i];
    if (!row.stage1Attempted) ReadStage1(row);
    return row.fault == RowFault::None;
}

bool DemoIndex::RowRead(size_t i) const { return i < rows_.size() && rows_[i].stage1Attempted; }

const DemoSummary& DemoIndex::At(size_t i) const {
    return i < rows_.size() ? rows_[i].summary : empty_;
}

void DemoIndex::Close() {
    rows_.clear();
    lastScanFiles_ = 0;
    lastScanMicros_ = 0;
}

bool DemoIndex::LoadManifest(size_t i) {
    fileio::Note();
    if (i >= rows_.size()) return false;
    ReadRow(i);
    Row& row = rows_[i];
    if (row.manifestAttempted) return row.manifestLoaded;
    if (row.legacy && !row.cached) {
        row.manifestAttempted = true;
        return false;
    }

    // ★ THE ATTEMPT IS RECORDED BEFORE IT IS MADE, and it is a SEPARATE flag from success.
    //
    // Recorded before, because the browser asks for the manifest of every visible row: if a failed
    // read left nothing behind, one damaged capture in the folder would be re-opened and re-parsed
    // on every frame, forever. The symptom is a browser that feels slow with no visible cause.
    //
    // Separate from success, because ManifestLoaded() is what the browser uses to decide between
    // printing "3 rounds, 8 players" and printing dashes. Reusing one flag for both would make a
    // damaged capture claim it had zero rounds and zero players, which is a different statement
    // from "we could not read this" and the wrong one.
    row.manifestAttempted = true;

    DemoReader reader;
    if (!reader.Open(row.readPath.c_str())) {
        row.fault = RowFault::ManifestRefused;
        const char* why = reader.RefusalReason();
        row.faultDetail = why != nullptr ? why : "the reader refused without naming a reason";
        return false;
    }

    row.manifest.CopyFrom(reader.GetManifest());
    row.manifest.Rebind();
    const Manifest& m = row.manifest.View();
    row.summary.manifest = &row.manifest.View();
    row.summary.roundCount = m.roundCount;
    row.summary.playerCount = m.playerCount;
    if (row.summary.durationUs == 0) {
        // The footer was missing, so the duration comes from the last chapter the reader actually
        // indexed -- which is the truthful answer for a torn file, not a guess.
        const size_t n = reader.ChapterCount();
        if (n > 0) {
            const ChapterKey k = reader.ChapterAt(n - 1);
            row.summary.durationUs = k.tEndUs > m.t0Us ? k.tEndUs - m.t0Us : 0;
        }
    }
    if (reader.WasRecovered() && row.fault == RowFault::None) {
        row.fault = RowFault::NoFooter;
        row.faultDetail = "opened by recovery scan: the index was rebuilt from the chapters";
    }
    row.manifestLoaded = true;
    reader.Close();
    return true;
}

bool DemoIndex::ManifestLoaded(size_t i) const {
    return i < rows_.size() && rows_[i].manifestLoaded;
}

RowFault DemoIndex::Fault(size_t i) const {
    return i < rows_.size() ? rows_[i].fault : RowFault::Unreadable;
}

const char* DemoIndex::FaultDetail(size_t i) const {
    if (i >= rows_.size()) return "row index out of range";
    if (rows_[i].fault == RowFault::None) return "";
    return rows_[i].faultDetail.empty() ? "no detail was recorded, which is itself a defect"
                                        : rows_[i].faultDetail.c_str();
}

BuildMatch DemoIndex::BuildMatchOf(size_t i) const {
    return i < rows_.size() ? rows_[i].buildMatch : BuildMatch::NotChecked;
}

const wchar_t* DemoIndex::Path(size_t i) const {
    return i < rows_.size() ? rows_[i].path.c_str() : nullptr;
}

const char* DemoIndex::DisplayName(size_t i) const {
    return i < rows_.size() ? rows_[i].displayName.c_str() : "";
}

uint64_t DemoIndex::LastScanMicros() const { return lastScanMicros_; }
size_t DemoIndex::LastScanFileCount() const { return lastScanFiles_; }
uint32_t DemoIndex::LastScanError() const { return lastScanError_; }

}  // namespace ui
}  // namespace replay
