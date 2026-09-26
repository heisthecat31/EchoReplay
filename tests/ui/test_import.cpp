// FORMATS-P6. The legacy-replay half of the viewer, without the panel: the cache key and path, the
// listing of .echoreplay/.tape beside captures, and the conversion worker's lifecycle against fake
// converters. The panel's own funnel is exercised in test_panel.cpp.

#include "doctest.h"

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <new>
#include <string>
#include <vector>

#include "bootstrap/paths.h"
#include "foundation/demofile/demofile.h"
#include "foundation/demofile/format.h"
#include "import/convert.h"
#include "ui/demoindex.h"
#include "ui/legacyimport.h"

using namespace replay;
namespace legacy = replay::ui::legacy;

namespace {

class ScratchDir {
public:
    ScratchDir() {
        wchar_t base[MAX_PATH];
        ::GetTempPathW(MAX_PATH, base);
        static unsigned counter = 0;
        _snwprintf_s(dir_, MAX_PATH, _TRUNCATE, L"%sfmt_p6_%lu_%u", base, ::GetCurrentProcessId(),
                     counter++);
        ::CreateDirectoryW(dir_, nullptr);
    }
    ~ScratchDir() { Remove(dir_); }
    const wchar_t* Path() const { return dir_; }
    std::wstring File(const wchar_t* leaf) const { return std::wstring(dir_) + L"\\" + leaf; }

private:
    static void Remove(const wchar_t* dir) {
        WIN32_FIND_DATAW fd = {};
        HANDLE h = ::FindFirstFileW((std::wstring(dir) + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                const std::wstring child = std::wstring(dir) + L"\\" + fd.cFileName;
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    Remove(child.c_str());
                } else {
                    ::DeleteFileW(child.c_str());
                }
            } while (::FindNextFileW(h, &fd) != 0);
            ::FindClose(h);
        }
        ::RemoveDirectoryW(dir);
    }
    wchar_t dir_[MAX_PATH] = {};
};

bool Exists(const std::wstring& path) {
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool WriteBytes(const std::wstring& path, const char* bytes) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = ::WriteFile(h, bytes, static_cast<DWORD>(strlen(bytes)), &wrote, nullptr);
    ::CloseHandle(h);
    return ok != 0;
}

// Days back from a fixed instant, so listing order is decided by the test and not by the disk.
void SetMtime(const std::wstring& path, int daysAgo) {
    HANDLE h = ::CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    const uint64_t t = 133500000000000000ull - static_cast<uint64_t>(daysAgo) * 864000000000ull;
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(t);
    ft.dwHighDateTime = static_cast<DWORD>(t >> 32);
    REQUIRE(::SetFileTime(h, nullptr, nullptr, &ft) != 0);
    ::CloseHandle(h);
}

// A minimal real capture, written by the shipped DemoWriter: the same thing a converter emits as
// far as the viewer can tell.
bool WriteDemo(const wchar_t* path) {
    static FieldDef field{};
    static SchemaDef schema{};
    static PlayerRecord player{};
    Manifest m{};
    m.formatMajor = kFormatMajor;
    m.formatMinor = kFormatMinor;
    m.t0Us = 1700000000000000ull;
    m.levelId = 0x4321;
    m.gametypeId = 0x6A7E000000008765ull;
    m.keyframeIntervalUs = 2000000;
    m.captureRole = 1;
    m.startTimeEpoch = 1700000000ull;
    field = FieldDef{};
    field.id = 1;
    field.cls = FieldClass::Raw;
    field.rawBits = 64;
    field.prov = FieldProv::Pc;
    schema = SchemaDef{};
    schema.id = 1;
    schema.retention = Retention::All;
    schema.required = true;
    schema.fieldCount = 1;
    schema.fields = &field;
    m.schemaCount = 1;
    m.schemas = &schema;
    player = PlayerRecord{};
    player.xplatformId = 0x1234;
    memcpy(player.displayName, "fixture", 8);
    memcpy(player.displayNameFull, "fixture", 8);
    player.userIndex = 1;
    m.playerCount = 1;
    m.players = &player;

    DemoWriter w;
    if (!w.Open(path, m)) return false;
    const uint64_t tick[4] = {m.t0Us, m.t0Us + 22000, m.t0Us + 44000, m.t0Us + 66000};
    w.BeginChapter(kSectionCore, m.t0Us);
    w.WriteStreamBlock(kStreamTick, 1, tick, sizeof tick, 4, 4);
    w.EndChapter(m.t0Us + 2000000ull);
    w.Close();
    return w.LastError() == nullptr;
}

import::Result Failed(import::ResultCode code, const char* message) {
    import::Result r{};
    r.ok = false;
    r.code = code;
    snprintf(r.message, sizeof r.message, "%s", message);
    return r;
}

import::Result Succeeded() {
    import::Result r{};
    r.ok = true;
    return r;
}

// ---- fake converters. Each records what the job handed it -------------------------------------

std::atomic<uint32_t> g_calls{0};
std::atomic<DWORD> g_threadId{0};
std::atomic<uint64_t> g_lastMaxModelBytes{0};
std::wstring g_lastOut;
std::wstring g_lastReport;

void Record(const wchar_t* out, const import::ConvertOptions& options) {
    g_calls.fetch_add(1);
    g_threadId.store(::GetCurrentThreadId());
    g_lastMaxModelBytes.store(options.maxModelBytes);
    g_lastOut = out;
    g_lastReport = options.reportPath != nullptr ? options.reportPath : L"";
}

constexpr uint32_t kHalfwayPermille = 500;

import::Result ConvertOk(const wchar_t*, const wchar_t* out, const import::ConvertOptions& options,
                         import::ConvertReport&, import::Progress* progress) {
    Record(out, options);
    progress->permille.store(kHalfwayPermille);
    if (!WriteDemo(out)) return Failed(import::ResultCode::Io, "fixture writer failed");
    WriteBytes(options.reportPath, "{}");
    progress->permille.store(990);
    return Succeeded();
}

// A refusal after writing some output, the way a real stage fails half way.
// Writes a part and a report, then throws what an allocation failure deep in the pipeline throws.
import::Result ConvertThrowsBadAlloc(const wchar_t*, const wchar_t* out, const import::ConvertOptions& options,
                                     import::ConvertReport&, import::Progress*) {
    Record(out, options);
    WriteBytes(out, "half a file");
    WriteBytes(options.reportPath, "{}");
    throw std::bad_alloc();
}

import::Result ConvertThrowsAnything(const wchar_t*, const wchar_t* out, const import::ConvertOptions& options,
                                     import::ConvertReport&, import::Progress*) {
    Record(out, options);
    WriteBytes(out, "half a file");
    throw 7;
}

import::Result ConvertRefuses(const wchar_t*, const wchar_t* out,
                              const import::ConvertOptions& options, import::ConvertReport&,
                              import::Progress*) {
    Record(out, options);
    WriteBytes(out, "half a file");
    WriteBytes(options.reportPath, "{}");
    return Failed(import::ResultCode::UnsupportedMap, "the replay is in mpl_lobby_b, not an arena");
}

import::Result ConvertGarbage(const wchar_t*, const wchar_t* out,
                              const import::ConvertOptions& options, import::ConvertReport&,
                              import::Progress*) {
    Record(out, options);
    WriteBytes(out, "this is not an ECHODEMO");
    return Succeeded();
}

// Holds until cancelled, as a long conversion does; gives up after ten seconds so a broken Cancel
// fails the test instead of hanging it.
import::Result ConvertUntilCancelled(const wchar_t*, const wchar_t* out,
                                     const import::ConvertOptions& options, import::ConvertReport&,
                                     import::Progress* progress) {
    Record(out, options);
    WriteBytes(out, "partial");
    progress->permille.store(250);
    for (int i = 0; i < 10000; ++i) {
        if (progress->cancel.load()) {
            return Failed(import::ResultCode::Cancelled, "cancelled");
        }
        ::Sleep(1);
    }
    return Failed(import::ResultCode::Io, "never cancelled");
}

// Ignores cancel for half a second, then succeeds.
import::Result ConvertSlowly(const wchar_t* in, const wchar_t* out,
                             const import::ConvertOptions& options, import::ConvertReport& report,
                             import::Progress* progress) {
    ::Sleep(500);
    return ConvertOk(in, out, options, report, progress);
}

void ResetFake() {
    g_calls.store(0);
    g_threadId.store(0);
    g_lastOut.clear();
    g_lastReport.clear();
}

bool WaitFinished(legacy::ConversionJob& job) {
    for (int i = 0; i < 10000 && !job.Finished(); ++i) ::Sleep(1);
    return job.Finished();
}

struct JobFixture {
    ScratchDir dir;
    std::wstring source = dir.File(L"rec_2025-08-03_17-15-57.echoreplay");
    std::wstring cache;
    legacy::ConversionJob job;

    JobFixture() {
        REQUIRE(WriteBytes(source, "PK not really a zip"));
        uint64_t size = 0;
        uint64_t mtime = 0;
        REQUIRE(legacy::StatFile(source.c_str(), &size, &mtime));
        wchar_t path[MAX_PATH];
        REQUIRE(legacy::CachePathFor(dir.Path(), source.c_str(), size, mtime, path, MAX_PATH));
        cache = path;
        ResetFake();
    }
    ~JobFixture() { job.Stop(INFINITE); }

    legacy::Outcome Run(legacy::ConvertFn fn) {
        const char* why = nullptr;
        REQUIRE(job.Start(source.c_str(), cache.c_str(), fn, &why));
        REQUIRE(WaitFinished(job));
        legacy::Outcome outcome;
        REQUIRE(job.Collect(&outcome));
        return outcome;
    }
};

}  // namespace

TEST_CASE("P6 legacy: only .echoreplay and .tape are legacy sources, in any case") {
    CHECK(legacy::IsLegacySource(L"C:\\x\\a.echoreplay"));
    CHECK(legacy::IsLegacySource(L"C:\\x\\a.ECHOREPLAY"));
    CHECK(legacy::IsLegacySource(L"a.tape"));
    CHECK(legacy::IsLegacySource(L"a.Tape"));
    CHECK_FALSE(legacy::IsLegacySource(L"a.echodemo"));
    CHECK_FALSE(legacy::IsLegacySource(L"a.tape.echodemo"));
    CHECK_FALSE(legacy::IsLegacySource(L"a.tapes"));
    CHECK_FALSE(legacy::IsLegacySource(L"tape"));
    CHECK_FALSE(legacy::IsLegacySource(nullptr));
}

// Vectors from the reference implementation (python xxhash 3.x), covering the short, the 4-byte,
// the 8-byte and the 32-byte-stripe paths and a non-zero seed.
TEST_CASE("P6 legacy: Xxh64 matches the reference implementation") {
    CHECK(legacy::Xxh64("", 0, 0) == 0xEF46DB3751D8E999ull);
    CHECK(legacy::Xxh64("a", 1, 0) == 0xD24EC4F1A98C6E5Bull);
    uint8_t data[100];
    for (int i = 0; i < 100; ++i) data[i] = static_cast<uint8_t>(i);
    struct Vector {
        size_t len;
        uint64_t seed0, seedGolden;
    };
    const Vector vectors[] = {
        {3, 0xE5C7BB4533BC65DDull, 0x47AA45ED39ED5541ull},
        {4, 0xFFCED8604453CC1Eull, 0xB8F1A51893FA14E6ull},
        {8, 0x884A173614B81B8Dull, 0xCB74EF9D044DCF44ull},
        {20, 0xA3E3FBE96798D8ACull, 0x2825DDBA4ED64452ull},
        {31, 0xC346D2B59B4D8EE1ull, 0x71969C89A30A986Aull},
        {32, 0xCBF59C5116FF32B4ull, 0x7181354774E0D600ull},
        {33, 0x0C535D1ACAFB8EADull, 0x57BD8AD0A0390E6Dull},
        {64, 0xF7C67301DB6713F0ull, 0x6E545403B828CF8Bull},
        {100, 0x6AC1E58032166597ull, 0x8832442A88284F11ull},
    };
    for (const Vector& v : vectors) {
        CAPTURE(v.len);
        CHECK(legacy::Xxh64(data, v.len, 0) == v.seed0);
        CHECK(legacy::Xxh64(data, v.len, 0x9E3779B1ull) == v.seedGolden);
    }
}

TEST_CASE("P6 legacy: the cache key is xxh64 over size, mtime and converter version") {
    // struct.pack('<QQI', 2154321, 0x01DC1234ABCD5678, 1) through the reference xxh64.
    CHECK(legacy::CacheKey(2154321, 0x01DC1234ABCD5678ull, 1) == 0x9D1626D821166222ull);
    const uint64_t base = legacy::CacheKey(100, 200, 1);
    CHECK(legacy::CacheKey(101, 200, 1) != base);   // the file grew
    CHECK(legacy::CacheKey(100, 201, 1) != base);   // the file was rewritten
    CHECK(legacy::CacheKey(100, 200, 2) != base);   // the converter changed
}

TEST_CASE("P6 legacy: the cache path is <captureDir>\\imported\\<leaf>.<key16>.echodemo") {
    wchar_t out[MAX_PATH];
    wchar_t want[MAX_PATH];
    const uint64_t key = legacy::CacheKey(5, 6, import::kConverterVersion);

    REQUIRE(legacy::CachePathFor(L"C:\\cap", L"D:\\Spark\\replays\\rec_1.echoreplay", 5, 6, out,
                                 MAX_PATH));
    _snwprintf_s(want, MAX_PATH, _TRUNCATE, L"C:\\cap\\imported\\rec_1.echoreplay.%016llx.echodemo",
                 static_cast<unsigned long long>(key));
    CHECK(std::wstring(out) == want);

    // A trailing separator is not doubled, and the extension is kept as written.
    REQUIRE(legacy::CachePathFor(L"C:\\cap\\", L"D:\\x\\sample.v2.TAPE", 5, 6, out, MAX_PATH));
    _snwprintf_s(want, MAX_PATH, _TRUNCATE, L"C:\\cap\\imported\\sample.v2.TAPE.%016llx.echodemo",
                 static_cast<unsigned long long>(key));
    CHECK(std::wstring(out) == want);

    // The same stem as .echoreplay and as .tape, same size and time: two caches, not one.
    wchar_t tape[MAX_PATH];
    REQUIRE(legacy::CachePathFor(L"C:\\cap", L"D:\\x\\rec_1.echoreplay", 5, 6, out, MAX_PATH));
    REQUIRE(legacy::CachePathFor(L"C:\\cap", L"D:\\x\\rec_1.tape", 5, 6, tape, MAX_PATH));
    CHECK(std::wstring(out) != tape);

    // A path that does not fit is refused, never truncated into a different file.
    wchar_t small[24];
    CHECK_FALSE(legacy::CachePathFor(L"C:\\cap", L"D:\\x\\rec_1.echoreplay", 5, 6, small, 24));
    CHECK(small[0] == L'\0');
    CHECK_FALSE(legacy::CachePathFor(L"", L"D:\\x\\rec_1.echoreplay", 5, 6, out, MAX_PATH));
    CHECK_FALSE(legacy::CachePathFor(L"C:\\cap", L"D:\\x\\.tape", 5, 6, out, MAX_PATH));
}

TEST_CASE("P6 legacy: import.dirs is a ';' list; set empty it names nothing") {
    const std::vector<std::wstring> dirs = ResolveImportDirs(" C:\\a ;;D:\\b\\;  ;E:\\c d");
    REQUIRE(dirs.size() == 3);
    CHECK(dirs[0] == L"C:\\a");
    CHECK(dirs[1] == L"D:\\b\\");
    CHECK(dirs[2] == L"E:\\c d");
    CHECK(ResolveImportDirs("").empty());
}

TEST_CASE("P6 legacy: unset import.dirs is Spark's folder, and nothing at all under a test redirect") {
    // The unset row is the only arm that reads the machine; a test process redirects it to none
    // so no suite ever lists the owner's real Spark folder.
    ScratchDir dir;
    ::CreateDirectoryW(dir.File(L"plugins").c_str(), nullptr);
    SetPathsForTest(dir.File(L"plugins\\EchoReplay.dll").c_str());
    CHECK(ResolveImportDirs(nullptr).empty());
}

TEST_CASE("P6 legacy: the index lists legacy sources beside captures, newest first") {
    ScratchDir capture;
    ScratchDir spark;
    REQUIRE(WriteDemo(capture.File(L"native.echodemo").c_str()));
    REQUIRE(WriteBytes(capture.File(L"local.tape"), "tape"));
    REQUIRE(WriteBytes(capture.File(L"notes.txt"), "ignored"));
    REQUIRE(WriteBytes(spark.File(L"spark.echoreplay"), "zip"));
    REQUIRE(WriteBytes(spark.File(L"SHOUTY.ECHOREPLAY"), "zip"));
    // An import dir lists legacy sources only: a capture that happens to sit there is not ours.
    REQUIRE(WriteDemo(spark.File(L"stray.echodemo").c_str()));
    SetMtime(capture.File(L"native.echodemo"), 3);
    SetMtime(capture.File(L"local.tape"), 1);
    SetMtime(spark.File(L"spark.echoreplay"), 2);
    SetMtime(spark.File(L"SHOUTY.ECHOREPLAY"), 4);

    ui::DemoIndex index;
    // The capture dir named again (with a trailing slash) is not listed twice, and a folder that
    // does not exist does not fail the scan.
    index.SetImportDirs({spark.Path(), std::wstring(capture.Path()) + L"\\",
                         capture.File(L"no_such_folder")});
    REQUIRE(index.Scan(capture.Path()));
    REQUIRE(index.Count() == 4);
    CHECK(std::string(index.DisplayName(0)) == "local.tape");
    CHECK(std::string(index.DisplayName(1)) == "spark.echoreplay");
    CHECK(std::string(index.DisplayName(2)) == "native.echodemo");
    CHECK(std::string(index.DisplayName(3)) == "SHOUTY.ECHOREPLAY");
    CHECK(index.IsLegacy(0));
    CHECK(index.IsLegacy(1));
    CHECK_FALSE(index.IsLegacy(2));
    CHECK(index.IsLegacy(3));
    CHECK(std::wstring(index.Path(1)) == spark.File(L"spark.echoreplay"));
    CHECK(index.At(1).sizeBytes == 3);   // from the directory entry, not from opening the file
}

TEST_CASE("P6 legacy: a legacy row is never opened, and without a cache it claims nothing") {
    ScratchDir capture;
    const std::wstring source = capture.File(L"locked.echoreplay");
    REQUIRE(WriteBytes(source, "zip bytes"));
    // Held with no sharing at all: any open of the source from here on fails, and would show up
    // as an Unreadable row.
    HANDLE hold = ::CreateFileW(source.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(hold != INVALID_HANDLE_VALUE);

    ui::DemoIndex index;
    REQUIRE(index.Scan(capture.Path()));
    REQUIRE(index.Count() == 1);
    CHECK(index.ReadRow(0));
    CHECK(index.RowRead(0));
    CHECK(index.Fault(0) == ui::RowFault::None);
    CHECK_FALSE(index.LegacyCached(0));
    CHECK_FALSE(index.LoadManifest(0));
    CHECK_FALSE(index.ManifestLoaded(0));
    CHECK(index.Fault(0) == ui::RowFault::None);
    CHECK(index.At(0).manifest == nullptr);
    ::CloseHandle(hold);
}

TEST_CASE("P6 legacy: with a cache present the row shows the cached capture's summary") {
    ScratchDir capture;
    const std::wstring source = capture.File(L"match.tape");
    REQUIRE(WriteBytes(source, "tape bytes"));
    uint64_t size = 0;
    uint64_t mtime = 0;
    REQUIRE(legacy::StatFile(source.c_str(), &size, &mtime));
    wchar_t cache[MAX_PATH];
    REQUIRE(legacy::CachePathFor(capture.Path(), source.c_str(), size, mtime, cache, MAX_PATH));
    ::CreateDirectoryW(capture.File(L"imported").c_str(), nullptr);
    REQUIRE(WriteDemo(cache));

    ui::DemoIndex index;
    REQUIRE(index.Scan(capture.Path()));
    // The imported\ folder is not scanned: its files are reached through their source rows only.
    REQUIRE(index.Count() == 1);
    CHECK(index.ReadRow(0));
    CHECK(index.LegacyCached(0));
    REQUIRE(index.LoadManifest(0));
    CHECK(index.At(0).playerCount == 1);
    CHECK(index.At(0).manifest->levelId == 0x4321);
    CHECK(index.At(0).durationUs > 0);
    CHECK(std::wstring(index.Path(0)) == source);   // loading the row still means the source
}

TEST_CASE("P6 job: a conversion runs on its own thread and lands in the cache") {
    JobFixture fx;
    const legacy::Outcome outcome = fx.Run(&ConvertOk);
    CHECK(outcome.ok);
    CHECK_FALSE(outcome.cacheHit);
    CHECK_FALSE(outcome.cancelled);
    CHECK(outcome.message[0] == '\0');
    CHECK(g_calls.load() == 1);
    CHECK(g_threadId.load() != ::GetCurrentThreadId());
    // Converted beside the cache, then moved into place: a crash mid-conversion can never leave a
    // file at the cache path.
    CHECK(g_lastOut == fx.cache + L".part");
    CHECK(g_lastReport == fx.cache + L".import.json");
    CHECK(Exists(fx.cache));
    CHECK_FALSE(Exists(fx.cache + L".part"));
    CHECK(Exists(fx.cache + L".import.json"));
    CHECK_FALSE(fx.job.Busy());
}

TEST_CASE("P6 job: a clean cache is a hit and the converter is not called") {
    JobFixture fx;
    REQUIRE(fx.Run(&ConvertOk).ok);
    ResetFake();
    const legacy::Outcome again = fx.Run(&ConvertOk);
    CHECK(again.ok);
    CHECK(again.cacheHit);
    CHECK(g_calls.load() == 0);
}

TEST_CASE("P6 job: a cache that does not open cleanly is converted again") {
    JobFixture fx;
    ::CreateDirectoryW(fx.dir.File(L"imported").c_str(), nullptr);
    REQUIRE(WriteBytes(fx.cache, "torn"));
    const legacy::Outcome outcome = fx.Run(&ConvertOk);
    CHECK(outcome.ok);
    CHECK_FALSE(outcome.cacheHit);
    CHECK(g_calls.load() == 1);
    CHECK(legacy::CacheOpensCleanly(fx.cache.c_str()));
}

TEST_CASE("P6 job: a refusal is carried verbatim and leaves no file behind") {
    JobFixture fx;
    const legacy::Outcome outcome = fx.Run(&ConvertRefuses);
    CHECK_FALSE(outcome.ok);
    CHECK_FALSE(outcome.cancelled);
    CHECK(std::string(outcome.message) == "the replay is in mpl_lobby_b, not an arena");
    CHECK_FALSE(Exists(fx.cache));
    CHECK_FALSE(Exists(fx.cache + L".part"));
    CHECK_FALSE(Exists(fx.cache + L".import.json"));
}

TEST_CASE("P6 job: output the reader refuses is not cached, whatever the converter said") {
    JobFixture fx;
    const legacy::Outcome outcome = fx.Run(&ConvertGarbage);
    CHECK_FALSE(outcome.ok);
    CHECK(std::string(outcome.message).find("does not open") != std::string::npos);
    CHECK_FALSE(Exists(fx.cache));
    CHECK_FALSE(Exists(fx.cache + L".part"));
}

TEST_CASE("P6 job: cancel stops the converter and nothing is kept") {
    JobFixture fx;
    const char* why = nullptr;
    REQUIRE(fx.job.Start(fx.source.c_str(), fx.cache.c_str(), &ConvertUntilCancelled, &why));
    for (int i = 0; i < 5000 && fx.job.Permille() != 250; ++i) ::Sleep(1);
    REQUIRE(fx.job.Permille() == 250);
    CHECK_FALSE(fx.job.Finished());

    // A second job while this one runs is refused, and says why.
    legacy::ConversionJob& job = fx.job;
    CHECK_FALSE(job.Start(fx.source.c_str(), fx.cache.c_str(), &ConvertOk, &why));
    REQUIRE(why != nullptr);
    CHECK(std::string(why) == "a conversion is already running");
    legacy::Outcome early;
    CHECK_FALSE(job.Collect(&early));

    job.Cancel();
    REQUIRE(WaitFinished(job));
    legacy::Outcome outcome;
    REQUIRE(job.Collect(&outcome));
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.cancelled);
    CHECK_FALSE(Exists(fx.cache));
    CHECK_FALSE(Exists(fx.cache + L".part"));
    CHECK_FALSE(job.Busy());
}

TEST_CASE("P6 job: Stop gives up after its budget and the worker can still be collected") {
    JobFixture fx;
    const char* why = nullptr;
    REQUIRE(fx.job.Start(fx.source.c_str(), fx.cache.c_str(), &ConvertSlowly, &why));
    CHECK_FALSE(fx.job.Stop(10));
    CHECK(fx.job.Busy());
    CHECK(fx.job.Stop(10000));
    CHECK_FALSE(fx.job.Busy());
}

TEST_CASE("P6 job: a successful conversion removes only its own stale cache siblings") {
    JobFixture fx;
    const std::wstring imported = fx.dir.File(L"imported");
    ::CreateDirectoryW(imported.c_str(), nullptr);
    const std::wstring stale = imported + L"\\rec_2025-08-03_17-15-57.echoreplay.0123456789abcdef.echodemo";
    const std::wstring other = imported + L"\\rec_2025-08-03_17-15-57.tape.0123456789abcdef.echodemo";
    const std::wstring notKey = imported + L"\\rec_2025-08-03_17-15-57.echoreplay.notakey000000000.echodemo";
    REQUIRE(WriteBytes(stale, "old"));
    REQUIRE(WriteBytes(stale + L".import.json", "{}"));
    REQUIRE(WriteBytes(other, "someone else's"));
    REQUIRE(WriteBytes(notKey, "not a cache name"));
    REQUIRE(fx.Run(&ConvertOk).ok);
    CHECK_FALSE(Exists(stale));
    CHECK_FALSE(Exists(stale + L".import.json"));
    CHECK(Exists(other));
    CHECK(Exists(notKey));
    CHECK(Exists(fx.cache));
}

TEST_CASE("M1 job: an exception in the converter becomes the refusal, cleans up, and the thread survives") {
    {
        JobFixture fx;
        const legacy::Outcome outcome = fx.Run(&ConvertThrowsBadAlloc);
        CHECK_FALSE(outcome.ok);
        CHECK(std::string(outcome.message) == "the conversion ran out of memory");
        CHECK_FALSE(Exists(fx.cache));
        CHECK_FALSE(Exists(fx.cache + L".part"));
        CHECK_FALSE(Exists(fx.cache + L".import.json"));
        // The same job takes the next file.
        CHECK(fx.Run(&ConvertOk).ok);
        CHECK(Exists(fx.cache));
    }
    JobFixture fx;
    const legacy::Outcome outcome = fx.Run(&ConvertThrowsAnything);
    CHECK_FALSE(outcome.ok);
    CHECK(std::string(outcome.message) == "the conversion failed with an unexpected error");
    CHECK_FALSE(Exists(fx.cache + L".part"));
}

TEST_CASE("M2 job: the worker converts under the in-game model budget") {
    JobFixture fx;
    REQUIRE(fx.Run(&ConvertOk).ok);
    CHECK(g_lastMaxModelBytes.load() == legacy::kInGameModelBytes);
    CHECK(legacy::kInGameModelBytes > 0);
}

TEST_CASE("S1 cache: the imported folder is trimmed least recently used first, never the new file") {
    ScratchDir dir;
    const std::wstring imported = dir.File(L"imported");
    ::CreateDirectoryW(imported.c_str(), nullptr);
    const std::wstring oldest = imported + L"\\a.tape.0123456789abcdef.echodemo";
    const std::wstring middle = imported + L"\\b.echoreplay.0123456789abcdef.echodemo";
    const std::wstring newest = imported + L"\\c.echoreplay.0123456789abcdef.echodemo";
    const std::wstring stranger = imported + L"\\notes.echodemo";
    const std::string kb(1024, 'x');
    for (const std::wstring* p : {&oldest, &middle, &newest, &stranger}) REQUIRE(WriteBytes(*p, kb.c_str()));
    REQUIRE(WriteBytes(oldest + L".import.json", "{}"));
    SetMtime(oldest, 3);
    SetMtime(middle, 2);
    SetMtime(newest, 1);
    SetMtime(stranger, 9);

    CHECK(legacy::TrimCache(imported.c_str(), 3 * 1024, oldest.c_str()) == 0);   // at the cap: nothing
    // Over the cap: the oldest is the one being kept, so the next oldest goes.
    CHECK(legacy::TrimCache(imported.c_str(), 2 * 1024, oldest.c_str()) == 1);
    CHECK(Exists(oldest));
    CHECK_FALSE(Exists(middle));
    CHECK(Exists(newest));
    CHECK(Exists(stranger));   // not a cache name, never counted or touched
    CHECK(legacy::TrimCache(imported.c_str(), 1024, newest.c_str()) == 1);
    CHECK_FALSE(Exists(oldest));
    CHECK_FALSE(Exists(oldest + L".import.json"));
    CHECK(Exists(newest));
    CHECK(legacy::TrimCache(imported.c_str(), 0, nullptr) == 0);   // 0 turns trimming off
    CHECK(Exists(newest));
}
