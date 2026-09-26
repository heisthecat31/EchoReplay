#include "ui/legacyimport.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <algorithm>
#include <exception>
#include <new>
#include <vector>

#include "bootstrap/log.h"
#include "foundation/demofile/demofile.h"

namespace replay {
namespace ui {
namespace legacy {
namespace {

const char* const kReplayModuleName = "ui";

constexpr uint64_t kPrime1 = 0x9E3779B185EBCA87ull;
constexpr uint64_t kPrime2 = 0xC2B2AE3D27D4EB4Full;
constexpr uint64_t kPrime3 = 0x165667B19E3779F9ull;
constexpr uint64_t kPrime4 = 0x85EBCA77C2B2AE63ull;
constexpr uint64_t kPrime5 = 0x27D4EB2F165667C5ull;

uint64_t Rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

uint64_t Read64(const uint8_t* p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

uint32_t Read32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

uint64_t Round(uint64_t acc, uint64_t input) {
    acc += input * kPrime2;
    return Rotl(acc, 31) * kPrime1;
}

uint64_t MergeRound(uint64_t acc, uint64_t val) {
    acc ^= Round(0, val);
    return acc * kPrime1 + kPrime4;
}

bool EndsWithIgnoringCase(const wchar_t* s, const wchar_t* suffix) {
    const size_t n = wcslen(s);
    const size_t k = wcslen(suffix);
    return n >= k && _wcsicmp(s + n - k, suffix) == 0;
}

const wchar_t* Leaf(const wchar_t* path) {
    const wchar_t* leaf = path;
    for (const wchar_t* p = path; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/') leaf = p + 1;
    }
    return leaf;
}

std::wstring ParentDir(const std::wstring& path) {
    const size_t cut = path.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : path.substr(0, cut);
}

bool IsLowerHex16(const wchar_t* s) {
    for (int i = 0; i < 16; ++i) {
        const wchar_t c = s[i];
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    }
    return true;
}

// The same source converted under an older key (it was edited, or the converter changed) leaves
// its previous cache file behind; at 20-30 MB per ten minutes they are not left to pile up. Only
// names of exactly `<stem>.<key16>.echodemo` are touched. A sibling held open by the player fails
// to delete and is simply kept.
void PruneStaleSiblings(const std::wstring& cachePath) {
    const std::wstring dir = ParentDir(cachePath);
    const wchar_t* leaf = Leaf(cachePath.c_str());
    const size_t leafLen = wcslen(leaf);
    static const size_t kTailLen = 1 + 16 + 9;   // ".<key16>.echodemo"
    if (leafLen <= kTailLen) return;
    const std::wstring stem(leaf, leafLen - kTailLen);

    WIN32_FIND_DATAW fd = {};
    HANDLE find = ::FindFirstFileW((dir + L"\\" + stem + L".*.echodemo").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    uint32_t removed = 0;
    do {
        const wchar_t* name = fd.cFileName;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        if (wcslen(name) != leafLen || _wcsicmp(name, leaf) == 0) continue;
        if (_wcsnicmp(name, stem.c_str(), stem.size()) != 0 || name[stem.size()] != L'.') continue;
        if (!IsLowerHex16(name + stem.size() + 1)) continue;
        if (_wcsicmp(name + stem.size() + 17, L".echodemo") != 0) continue;
        const std::wstring stale = dir + L"\\" + name;
        if (::DeleteFileW(stale.c_str()) != 0) {
            ::DeleteFileW((stale + L".import.json").c_str());
            ++removed;
        }
    } while (::FindNextFileW(find, &fd) != 0);
    ::FindClose(find);
    if (removed > 0) {
        log::Direct(LogLevel::Info, kReplayModuleName,
                    "import: removed %u stale cache file(s) of %ls", removed, stem.c_str());
    }
}

bool IsCacheName(const wchar_t* name) {
    static const size_t kTailLen = 1 + 16 + 9;   // ".<key16>.echodemo"
    const size_t len = wcslen(name);
    return len > kTailLen && name[len - kTailLen] == L'.' && IsLowerHex16(name + len - kTailLen + 1) &&
           _wcsicmp(name + len - 9, L".echodemo") == 0;
}

// A cache hit is a use: the trim is least recently used first, by last-write time.
void MarkUsed(const std::wstring& path) {
    HANDLE h = ::CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME now;
    ::GetSystemTimeAsFileTime(&now);
    ::SetFileTime(h, nullptr, nullptr, &now);
    ::CloseHandle(h);
}

}  // namespace

bool IsLegacySource(const wchar_t* path) {
    return path != nullptr &&
           (EndsWithIgnoringCase(path, L".echoreplay") || EndsWithIgnoringCase(path, L".tape"));
}

uint64_t Xxh64(const void* data, size_t len, uint64_t seed) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    const uint8_t* const end = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = seed + kPrime1 + kPrime2;
        uint64_t v2 = seed + kPrime2;
        uint64_t v3 = seed;
        uint64_t v4 = seed - kPrime1;
        const uint8_t* const limit = end - 32;
        do {
            v1 = Round(v1, Read64(p));
            v2 = Round(v2, Read64(p + 8));
            v3 = Round(v3, Read64(p + 16));
            v4 = Round(v4, Read64(p + 24));
            p += 32;
        } while (p <= limit);
        h = Rotl(v1, 1) + Rotl(v2, 7) + Rotl(v3, 12) + Rotl(v4, 18);
        h = MergeRound(h, v1);
        h = MergeRound(h, v2);
        h = MergeRound(h, v3);
        h = MergeRound(h, v4);
    } else {
        h = seed + kPrime5;
    }
    h += static_cast<uint64_t>(len);
    while (end - p >= 8) {
        h ^= Round(0, Read64(p));
        h = Rotl(h, 27) * kPrime1 + kPrime4;
        p += 8;
    }
    if (end - p >= 4) {
        h ^= static_cast<uint64_t>(Read32(p)) * kPrime1;
        h = Rotl(h, 23) * kPrime2 + kPrime3;
        p += 4;
    }
    while (p < end) {
        h ^= static_cast<uint64_t>(*p) * kPrime5;
        h = Rotl(h, 11) * kPrime1;
        ++p;
    }
    h ^= h >> 33;
    h *= kPrime2;
    h ^= h >> 29;
    h *= kPrime3;
    h ^= h >> 32;
    return h;
}

uint64_t CacheKey(uint64_t sourceSize, uint64_t sourceMtime, uint32_t converterVersion) {
    uint8_t bytes[20];
    memcpy(bytes, &sourceSize, 8);
    memcpy(bytes + 8, &sourceMtime, 8);
    memcpy(bytes + 16, &converterVersion, 4);
    return Xxh64(bytes, sizeof bytes, 0);
}

bool CachePathFor(const wchar_t* captureDir, const wchar_t* sourcePath, uint64_t sourceSize,
                  uint64_t sourceMtime, wchar_t* out, size_t cap) {
    if (out == nullptr || cap == 0) return false;
    out[0] = L'\0';
    if (captureDir == nullptr || captureDir[0] == L'\0' || sourcePath == nullptr) return false;
    const wchar_t* leaf = Leaf(sourcePath);
    const size_t stemLen = wcslen(leaf);
    if (stemLen == 0 || leaf[0] == L'.') return false;
    const size_t dirLen = wcslen(captureDir);
    const bool slash = captureDir[dirLen - 1] == L'\\' || captureDir[dirLen - 1] == L'/';
    const int wrote = _snwprintf_s(
        out, cap, _TRUNCATE, L"%s%simported\\%.*s.%016llx.echodemo", captureDir,
        slash ? L"" : L"\\", static_cast<int>(stemLen), leaf,
        static_cast<unsigned long long>(
            CacheKey(sourceSize, sourceMtime, import::kConverterVersion)));
    if (wrote < 0) {
        out[0] = L'\0';
        return false;
    }
    return true;
}

bool StatFile(const wchar_t* path, uint64_t* sizeBytes, uint64_t* mtime) {
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (path == nullptr || ::GetFileAttributesExW(path, GetFileExInfoStandard, &fad) == 0 ||
        (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    *sizeBytes = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    *mtime = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
             fad.ftLastWriteTime.dwLowDateTime;
    return true;
}

bool CacheOpensCleanly(const wchar_t* cachePath) {
    if (::GetFileAttributesW(cachePath) == INVALID_FILE_ATTRIBUTES) return false;
    DemoReader reader;
    if (!reader.Open(cachePath)) return false;
    const bool clean = !reader.WasRecovered();
    reader.Close();
    return clean;
}

uint32_t TrimCache(const wchar_t* importedDir, uint64_t capBytes, const wchar_t* keepPath) {
    if (importedDir == nullptr || capBytes == 0) return 0;
    struct Entry {
        std::wstring path;
        uint64_t bytes;
        uint64_t lastWrite;
    };
    const std::wstring dir = importedDir;
    std::vector<Entry> entries;
    uint64_t total = 0;
    WIN32_FIND_DATAW fd = {};
    HANDLE find = ::FindFirstFileW((dir + L"\\*.echodemo").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || !IsCacheName(fd.cFileName)) continue;
        const uint64_t bytes = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        const uint64_t lastWrite = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                                   fd.ftLastWriteTime.dwLowDateTime;
        entries.push_back({dir + L"\\" + fd.cFileName, bytes, lastWrite});
        total += bytes;
    } while (::FindNextFileW(find, &fd) != 0);
    ::FindClose(find);
    if (total <= capBytes) return 0;

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.lastWrite < b.lastWrite; });
    uint32_t removed = 0;
    for (const Entry& e : entries) {
        if (total <= capBytes) break;
        if (keepPath != nullptr && _wcsicmp(e.path.c_str(), keepPath) == 0) continue;
        if (::DeleteFileW(e.path.c_str()) == 0) continue;
        ::DeleteFileW((e.path + L".import.json").c_str());
        total -= e.bytes;
        ++removed;
    }
    if (removed > 0) {
        log::Direct(LogLevel::Info, kReplayModuleName,
                    "import: trimmed %u least recently used cache file(s); %llu MB remain (cap %llu MB)",
                    removed, static_cast<unsigned long long>(total >> 20),
                    static_cast<unsigned long long>(capBytes >> 20));
    }
    return removed;
}

bool ConversionJob::Start(const wchar_t* sourcePath, const wchar_t* cachePath, ConvertFn convert,
                          const char** why) {
    const auto refuse = [why](const char* reason) {
        if (why != nullptr) *why = reason;
        return false;
    };
    if (thread_ != nullptr) return refuse("a conversion is already running");
    if (sourcePath == nullptr || cachePath == nullptr || convert == nullptr) {
        return refuse("no source, cache path or converter was given");
    }
    source_ = sourcePath;
    cache_ = cachePath;
    convert_ = convert;
    outcome_ = Outcome{};
    report_ = import::ConvertReport{};
    progress_.permille.store(0, std::memory_order_relaxed);
    progress_.cancel.store(false, std::memory_order_relaxed);
    finished_.store(false, std::memory_order_relaxed);
    thread_ = ::CreateThread(nullptr, 0, &ConversionJob::ThreadMain, this, 0, nullptr);
    if (thread_ == nullptr) return refuse("the conversion thread could not be created");
    return true;
}

bool ConversionJob::Collect(Outcome* out) {
    if (thread_ == nullptr || !Finished()) return false;
    ::WaitForSingleObject(thread_, INFINITE);
    ::CloseHandle(thread_);
    thread_ = nullptr;
    finished_.store(false, std::memory_order_relaxed);
    if (out != nullptr) *out = outcome_;
    return true;
}

bool ConversionJob::Stop(uint32_t waitMs) {
    if (thread_ == nullptr) return true;
    Cancel();
    if (::WaitForSingleObject(thread_, waitMs) != WAIT_OBJECT_0) return false;
    return Collect(nullptr);
}

unsigned long __stdcall ConversionJob::ThreadMain(void* self) {
    static_cast<ConversionJob*>(self)->Run();
    return 0;
}

void ConversionJob::Run() {
    // Below normal: the game's own threads come first, and the conversion simply takes longer.
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const ULONGLONG t0 = ::GetTickCount64();
    log::Direct(LogLevel::Info, kReplayModuleName, "import worker (thread %lu): %ls -> %ls",
                ::GetCurrentThreadId(), source_.c_str(), cache_.c_str());
    // Nothing may escape this thread: an exception here would reach std::terminate and end the game.
    try {
        if (CacheOpensCleanly(cache_.c_str())) {
            outcome_.ok = true;
            outcome_.cacheHit = true;
            MarkUsed(cache_);
        } else {
            Convert();
        }
    } catch (const std::bad_alloc&) {
        Abandon("the conversion ran out of memory");
    } catch (const std::exception& e) {
        char what[160];
        snprintf(what, sizeof what, "the conversion failed: %s", e.what());
        Abandon(what);
    } catch (...) {
        Abandon("the conversion failed with an unexpected error");
    }
    outcome_.cancelled = progress_.cancel.load(std::memory_order_relaxed);
    outcome_.elapsedMs = ::GetTickCount64() - t0;
    log::Direct(outcome_.ok ? LogLevel::Info : LogLevel::Warn, kReplayModuleName,
                "import worker (thread %lu): %s in %llu ms%s%s", ::GetCurrentThreadId(),
                outcome_.cacheHit ? "cache hit" : outcome_.ok ? "converted" : "failed",
                static_cast<unsigned long long>(outcome_.elapsedMs),
                outcome_.message[0] != '\0' ? ": " : "", outcome_.message);
    finished_.store(true, std::memory_order_release);
}

// After a success the only work left is housekeeping, so a throw there keeps the converted file.
void ConversionJob::Abandon(const char* message) noexcept {
    if (outcome_.ok) return;
    outcome_.cacheHit = false;
    ::DeleteFileW((cache_ + L".part").c_str());
    ::DeleteFileW(cache_.c_str());
    ::DeleteFileW((cache_ + L".import.json").c_str());
    snprintf(outcome_.message, sizeof outcome_.message, "%s", message);
}

void ConversionJob::Convert() {
    const std::wstring dir = ParentDir(cache_);
    if (::CreateDirectoryW(dir.c_str(), nullptr) == 0 && ::GetLastError() != ERROR_ALREADY_EXISTS) {
        snprintf(outcome_.message, sizeof outcome_.message,
                 "the cache folder %ls could not be created (error %lu)", dir.c_str(),
                 ::GetLastError());
        return;
    }
    // Whatever is at the cache path did not open cleanly, so it is not worth keeping.
    ::DeleteFileW(cache_.c_str());

    const std::wstring part = cache_ + L".part";
    const std::wstring reportPath = cache_ + L".import.json";
    import::ConvertOptions options;
    options.reportPath = reportPath.c_str();
    options.maxModelBytes = kInGameModelBytes;
    const import::Result r = convert_(source_.c_str(), part.c_str(), options, report_, &progress_);
    if (!r.ok) {
        ::DeleteFileW(part.c_str());
        ::DeleteFileW(reportPath.c_str());
        snprintf(outcome_.message, sizeof outcome_.message, "%s",
                 r.message[0] != '\0' ? r.message : "the converter refused without a reason");
        return;
    }
    if (::MoveFileExW(part.c_str(), cache_.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        const DWORD err = ::GetLastError();
        ::DeleteFileW(part.c_str());
        ::DeleteFileW(reportPath.c_str());
        snprintf(outcome_.message, sizeof outcome_.message,
                 "the converted file could not be moved into the cache (error %lu)", err);
        return;
    }
    // A cache file that the player would then refuse is worse than none: every later open would
    // hit it and fail the same way instead of converting again.
    if (!CacheOpensCleanly(cache_.c_str())) {
        ::DeleteFileW(cache_.c_str());
        ::DeleteFileW(reportPath.c_str());
        snprintf(outcome_.message, sizeof outcome_.message,
                 "the converter reported success but its output does not open as a capture");
        return;
    }
    outcome_.ok = true;
    PruneStaleSiblings(cache_);
    TrimCache(dir.c_str(), cacheCapBytes_, cache_.c_str());
}

}  // namespace legacy
}  // namespace ui
}  // namespace replay
