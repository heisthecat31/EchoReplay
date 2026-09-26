#pragma once

// FORMATS-P6 (DESIGN-FORMATS 7.8). Opening a legacy replay (.echoreplay, .tape) converts it once into
// a cached .echodemo under `<captureDir>\imported\`, on one worker thread; the game thread only polls.
// The cached file is then loaded through the ordinary LoadPath.

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <string>

#include "import/convert.h"

namespace replay {
namespace ui {
namespace legacy {

using ConvertFn = import::Result (*)(const wchar_t* inPath, const wchar_t* outPath,
                                     const import::ConvertOptions& options,
                                     import::ConvertReport& report, import::Progress* progress);

// import::Convert once the converter library is complete; until then one that refuses with a
// sentence saying so. Chosen at configure time by src/ui/CMakeLists.txt.
ConvertFn LinkedConverter();

// True for a path ending in .echoreplay or .tape, in any case.
bool IsLegacySource(const wchar_t* path);

uint64_t Xxh64(const void* data, size_t len, uint64_t seed);

// xxh64 over (size, last-write time, converter version), little-endian, seed 0. Content is not
// hashed: a 570 MB source read on every open is what the key exists to avoid (DESIGN-FORMATS D11).
uint64_t CacheKey(uint64_t sourceSize, uint64_t sourceMtime, uint32_t converterVersion);

// `<captureDir>\imported\<leaf>.<key16>.echodemo`, key16 being CacheKey in 16 lowercase hex
// digits over kConverterVersion. The leaf keeps its extension, so X.echoreplay and X.tape never
// share a name. False, with `out` empty, when the result does not fit `cap`.
bool CachePathFor(const wchar_t* captureDir, const wchar_t* sourcePath, uint64_t sourceSize,
                  uint64_t sourceMtime, wchar_t* out, size_t cap);

// Size and last-write time as a FILETIME value. False when the file cannot be stat'ed.
bool StatFile(const wchar_t* path, uint64_t* sizeBytes, uint64_t* mtime);

// The shipped reader opens it on its fast path: header, index and footer all intact. A file the
// reader could only recover by scanning chapters is not a cache hit; it is reconverted.
bool CacheOpensCleanly(const wchar_t* cachePath);

// The in-game conversion's ceiling on the source model (import::ConvertOptions::maxModelBytes).
constexpr uint64_t kInGameModelBytes = uint64_t(160) << 20;

// `import.cache_mb`: the imported folder is trimmed, least recently used first, to this size after
// each conversion. 0 or less turns trimming off.
constexpr const char* kImportCacheMbKey = "import.cache_mb";
constexpr double kDefaultImportCacheMb = 2048;

// Deletes `<name>.<key16>.echodemo` files in `importedDir` (with their .import.json), oldest last
// write first, until the folder's cache files total at most `capBytes`. `keepPath` and files the
// player holds open are never removed. Returns how many were removed.
uint32_t TrimCache(const wchar_t* importedDir, uint64_t capBytes, const wchar_t* keepPath);

struct Outcome {
    bool ok = false;
    bool cacheHit = false;     // nothing was converted; the cache already opened cleanly
    bool cancelled = false;    // Cancel() was called; an ok outcome means the file finished anyway
    uint64_t elapsedMs = 0;
    char message[256] = {};    // the converter's sentence, or ours; empty when ok
};

// One conversion at a time. Start, then poll Finished() from the game thread and Collect() once it
// is true. The worker checks the cache, converts into `<cache>.part`, and renames that into place
// only when the converter succeeded and the result opens cleanly; every failure deletes the part.
class ConversionJob {
public:
    ConversionJob() = default;
    ConversionJob(const ConversionJob&) = delete;
    ConversionJob& operator=(const ConversionJob&) = delete;

    // False when a job is still running or uncollected, or when the thread could not be created;
    // `why` then says which.
    bool Start(const wchar_t* sourcePath, const wchar_t* cachePath, ConvertFn convert,
               const char** why);
    bool Busy() const { return thread_ != nullptr; }
    bool Finished() const { return finished_.load(std::memory_order_acquire); }
    uint32_t Permille() const { return progress_.permille.load(std::memory_order_relaxed); }
    void Cancel() { progress_.cancel.store(true, std::memory_order_relaxed); }

    // Joins a finished worker and hands back its outcome. False while it runs, or when idle.
    bool Collect(Outcome* out);

    // Cancels, then waits up to `waitMs` for the worker. False when it outlived the wait: it is
    // left running with everything it touches, for process exit to end.
    bool Stop(uint32_t waitMs);

    const std::wstring& SourcePath() const { return source_; }
    const std::wstring& CachePath() const { return cache_; }

    // Only while idle; the next Start's worker reads it. 0 turns trimming off.
    void SetCacheCapBytes(uint64_t bytes) { cacheCapBytes_ = bytes; }

private:
    static unsigned long __stdcall ThreadMain(void* self);
    void Run();
    void Convert();
    void Abandon(const char* message) noexcept;

    void* thread_ = nullptr;
    std::atomic<bool> finished_{false};
    import::Progress progress_;
    import::ConvertReport report_{};
    ConvertFn convert_ = nullptr;
    std::wstring source_;
    std::wstring cache_;
    Outcome outcome_;
    uint64_t cacheCapBytes_ = uint64_t(kDefaultImportCacheMb) << 20;
};

}  // namespace legacy
}  // namespace ui
}  // namespace replay
