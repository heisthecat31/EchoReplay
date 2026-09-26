// W2-G. The notes sidecar, in memory and ON DISK.
//
// ★★★ WHY THIS FILE EXISTS. The sidecar's file layer had never been executed by a test. The build
// classified replay_control_core as "pure", the word was read as "nothing here has side effects",
// and 37 green cases in test_control.cpp called Serialise and Parse and never once called Save or
// Load. Underneath that green gate, `mark` was replacing the owner's bookmark file whenever the old
// one could not be READ -- a sharing violation, a deny-read ACE and descriptor exhaustion all
// reported "there is no sidecar yet", which is the one answer that authorises writing a new one.
//
// So every arm below that can touch a file DOES touch a file, in a real temporary directory.

#include "doctest.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <string>

#include "control/notes.h"
#include "foundation/demofile/format.h"

using namespace replay::control;

namespace {

// The version field this build writes. A document meant to BE ours spells it this way rather than
// repeating a number: three cases that were not about versions at all carried a literal 1, and the
// bump to 2 turned them into tests of a format nothing writes any more.
std::string CurrentVersionField() {
    return "\"version\":" + std::to_string(notes::kFormatVersion);
}

// The identity block a document meant to be OURS carries. A sidecar without one is Unidentified --
// deliberately, because that is what every sidecar written before the L3-RUN-3 F3 fix looks like --
// so a case that is not about identity still has to say which capture it belongs to.
std::string IdentityField(const std::string& uuidHex, long long sizeBytes, long long t0Us) {
    return "\"identity\":{\"uuid\":\"" + uuidHex + "\",\"head\":\"" + std::string(32, '0') +
           "\",\"sizeBytes\":" + std::to_string(sizeBytes) + ",\"t0Us\":" + std::to_string(t0Us) + "}";
}

// A temporary directory of its own per test case, removed on the way out. Named after the case so
// a failure leaves something identifiable behind.
class TempDir {
public:
    explicit TempDir(const wchar_t* tag) {
        wchar_t base[MAX_PATH] = {0};
        const DWORD n = GetTempPathW(MAX_PATH, base);
        REQUIRE_MESSAGE(n != 0, "GetTempPathW failed");
        static unsigned counter = 0;
        swprintf_s(path_, L"%sw2g_notes_%s_%lu_%u", base, tag, GetCurrentProcessId(), ++counter);
        REQUIRE_MESSAGE(CreateDirectoryW(path_, nullptr) != 0,
                        "could not create the temporary directory");
    }

    ~TempDir() {
        WIN32_FIND_DATAW find;
        std::wstring glob(path_);
        glob.append(L"\\*");
        HANDLE h = FindFirstFileW(glob.c_str(), &find);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(find.cFileName, L".") == 0 || wcscmp(find.cFileName, L"..") == 0) {
                    continue;
                }
                std::wstring victim(path_);
                victim.append(L"\\").append(find.cFileName);
                DeleteFileW(victim.c_str());
            } while (FindNextFileW(h, &find));
            FindClose(h);
        }
        RemoveDirectoryW(path_);
    }

    std::wstring File(const wchar_t* name) const {
        std::wstring p(path_);
        p.append(L"\\").append(name);
        return p;
    }

    const wchar_t* Path() const { return path_; }

private:
    wchar_t path_[MAX_PATH] = {0};
};

void WriteBytes(const std::wstring& path, const char* data, size_t len) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE_MESSAGE(h != INVALID_HANDLE_VALUE, "could not create the test file");
    DWORD wrote = 0;
    if (len != 0) {
        WriteFile(h, data, static_cast<DWORD>(len), &wrote, nullptr);
    }
    CloseHandle(h);
    REQUIRE(wrote == static_cast<DWORD>(len));
}

std::string ReadBytes(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return std::string();
    }
    std::string out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got != 0) {
        out.append(buf, got);
    }
    CloseHandle(h);
    return out;
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The distinction the owner's bookmarks depend on
// ---------------------------------------------------------------------------------------------

TEST_CASE("only Absent authorises writing a new sidecar") {
    // The predicate `mark` asks before it decides to create a file, over every value it can be
    // handed. Five of the six must be no: each of them means "something is there and we could not
    // read it", and creating over any of them is the data loss.
    CHECK(notes::MayCreateNew(notes::LoadResult::Absent));
    CHECK_FALSE(notes::MayCreateNew(notes::LoadResult::Ok));
    CHECK_FALSE(notes::MayCreateNew(notes::LoadResult::Unreadable));
    CHECK_FALSE(notes::MayCreateNew(notes::LoadResult::Damaged));
    CHECK_FALSE(notes::MayCreateNew(notes::LoadResult::UnknownVersion));
    // A format-1 file is the owner's own bookmarks. Refusing to read them and refusing to write
    // over them are one decision: unreadable is not absent, whoever wrote it.
    CHECK_FALSE(notes::MayCreateNew(notes::LoadResult::AbsoluteAxisVersion));
    // A sidecar that names no capture, and one that names a different capture and could not be
    // moved out of the way, are both still SOMETHING sitting at that path.
    CHECK_FALSE(notes::MayCreateNew(notes::LoadResult::Unidentified));
    CHECK_FALSE(notes::MayCreateNew(notes::LoadResult::StaleIdentityHeld));
    // The one addition: LoadFor only answers this after the old file has been renamed aside, so
    // the path is empty and a new sidecar costs nobody a bookmark.
    CHECK(notes::MayCreateNew(notes::LoadResult::StaleIdentity));
}

TEST_CASE("a sidecar that cannot be read is Unreadable, not Absent, and is left untouched") {
    TempDir dir(L"sharing");
    const std::wstring sidecar = dir.File(L"match.echodemo.notes.json");

    notes::Sidecar original;
    REQUIRE(original.AddBookmark(1000, L"first") == notes::AddResult::Ok);
    REQUIRE(original.AddBookmark(2000, L"second") == notes::AddResult::Ok);
    original.SetLastPlayheadUs(2000);
    REQUIRE(original.Save(sidecar.c_str()) == notes::SaveResult::Ok);

    const std::string before = ReadBytes(sidecar);
    REQUIRE_MESSAGE(before.size() > 0, "the sidecar under test was not written");

    // ★ THE PRECONDITION IS A REQUIRE, NOT A CHECK. If this handle does not open with sharing
    // denied, the case measures nothing at all -- Load would simply succeed and the arm would go
    // green on the ordinary path.
    HANDLE lock = CreateFileW(sidecar.c_str(), GENERIC_READ, 0 /* deny all sharing */, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE_MESSAGE(lock != INVALID_HANDLE_VALUE,
                    "could not take an exclusive handle; the sharing violation was never produced");

    notes::Sidecar reader;
    const notes::LoadResult got = reader.Load(sidecar.c_str());
    CHECK(got == notes::LoadResult::Unreadable);
    CHECK_FALSE(got == notes::LoadResult::Absent);
    CHECK_FALSE(notes::MayCreateNew(got));

    CloseHandle(lock);

    // Nothing wrote to it while it was locked, and nothing may write to it afterwards on the
    // strength of that read: the bytes are the assertion, not the return value.
    const std::string after = ReadBytes(sidecar);
    CHECK(after == before);

    notes::Sidecar recheck;
    REQUIRE(recheck.Load(sidecar.c_str()) == notes::LoadResult::Ok);
    CHECK(recheck.BookmarkCount() == 2);
}

TEST_CASE("the six outcomes of reading a sidecar are six different answers") {
    TempDir dir(L"outcomes");

    SUBCASE("absent") {
        notes::Sidecar s;
        CHECK(s.Load(dir.File(L"nothing.echodemo.notes.json").c_str()) == notes::LoadResult::Absent);
    }
    SUBCASE("zero bytes") {
        const std::wstring p = dir.File(L"empty.echodemo.notes.json");
        WriteBytes(p, "", 0);
        notes::Sidecar s;
        CHECK(s.Load(p.c_str()) == notes::LoadResult::Damaged);
    }
    SUBCASE("truncated") {
        const std::wstring p = dir.File(L"cut.echodemo.notes.json");
        const std::string text = "{" + CurrentVersionField() + ",\"bookmarks\":[{\"tUs\":5,\"lab";
        WriteBytes(p, text.data(), text.size());
        notes::Sidecar s;
        CHECK(s.Load(p.c_str()) == notes::LoadResult::Damaged);
    }
    SUBCASE("a version this build does not write") {
        const std::wstring p = dir.File(L"v99.echodemo.notes.json");
        const char* text = "{\"version\":99,\"lastPlayheadUs\":7,\"bookmarks\":[{\"tUs\":5,\"label\":\"x\"}]}";
        WriteBytes(p, text, strlen(text));
        notes::Sidecar s;
        // Not Ok, and specifically not "version 1 with a bookmark at 5 us": a format 2 that
        // recorded nanoseconds would land every bookmark a thousand times off.
        CHECK(s.Load(p.c_str()) == notes::LoadResult::UnknownVersion);
        CHECK(s.BookmarkCount() == 0);
    }
    SUBCASE("the format this build wrote before the axis changed") {
        const std::wstring p = dir.File(L"v1.echodemo.notes.json");
        const char* text = "{\"version\":1,\"lastPlayheadUs\":7,\"bookmarks\":[{\"tUs\":5,\"label\":\"x\"}]}";
        WriteBytes(p, text, strlen(text));
        notes::Sidecar s;
        // Its own answer, not "unknown": this file is the owner's, and what to do about it differs.
        CHECK(s.Load(p.c_str()) == notes::LoadResult::AbsoluteAxisVersion);
        CHECK(s.BookmarkCount() == 0);
    }
    SUBCASE("no version at all") {
        const std::wstring p = dir.File(L"nov.echodemo.notes.json");
        const char* text = "{\"lastPlayheadUs\":7,\"bookmarks\":[{\"tUs\":5,\"label\":\"x\"}]}";
        WriteBytes(p, text, strlen(text));
        notes::Sidecar s;
        CHECK(s.Load(p.c_str()) == notes::LoadResult::UnknownVersion);
    }
    SUBCASE("ours") {
        const std::wstring p = dir.File(L"ok.echodemo.notes.json");
        notes::Sidecar w;
        REQUIRE(w.AddBookmark(5, L"x") == notes::AddResult::Ok);
        REQUIRE(w.Save(p.c_str()) == notes::SaveResult::Ok);
        notes::Sidecar s;
        CHECK(s.Load(p.c_str()) == notes::LoadResult::Ok);
        CHECK(s.BookmarkCount() == 1);
    }
}

// ---------------------------------------------------------------------------------------------
// The axis that changed underneath `tUs`
// ---------------------------------------------------------------------------------------------

// ★ THE ARTIFACT IS NOT TYPED OUT, IT IS GENERATED. `mark` records IReplayTransport::PlayheadUs(),
// which was absolute source time until 3f7ca26 and is 0-based playback time after it. The two
// documents are identical apart from `version`, so the file below is what this build's own writer
// produces with that one field put back -- the real pre-bump artifact, not a recollection of it.
// The unmodified document is loaded too, as the near-miss control: if it did not read as Ok the
// refusal above would be proving something other than the version.
TEST_CASE("a sidecar from before the 0-based axis is refused rather than read onto the new one") {
    TempDir dir(L"axis");

    notes::Sidecar w;
    REQUIRE(w.AddBookmark(1800000000, L"great save") == notes::AddResult::Ok);
    w.SetLastPlayheadUs(1805000000);
    std::string current;
    w.Serialise(&current);

    const std::string mine = "\"version\": " + std::to_string(notes::kFormatVersion);
    const std::string theirs = "\"version\": " + std::to_string(notes::kFormatVersion1Absolute);
    const size_t at = current.find(mine);
    // A REQUIRE, because a writer that stopped spelling the field this way would turn the arm
    // below into a test of two identical documents that both pass.
    REQUIRE(at != std::string::npos);
    std::string old = current;
    old.replace(at, mine.size(), theirs);

    const std::wstring pOld = dir.File(L"pre-axis.echodemo.notes.json");
    const std::wstring pNew = dir.File(L"post-axis.echodemo.notes.json");
    WriteBytes(pOld, old.data(), old.size());
    WriteBytes(pNew, current.data(), current.size());

    // A decoy first, so "0 bookmarks after the refusal" cannot be the default-constructed state a
    // Load that did nothing at all would also leave behind.
    notes::Sidecar readOld;
    REQUIRE(readOld.AddBookmark(42, L"decoy") == notes::AddResult::Ok);
    readOld.SetLastPlayheadUs(43);
    const notes::LoadResult got = readOld.Load(pOld.c_str());
    CHECK(got == notes::LoadResult::AbsoluteAxisVersion);
    // The bookmark is not adopted: 1800000000 us means a different moment on each axis, and the
    // error is invisible -- a marker in the wrong place still draws like a marker.
    CHECK(readOld.BookmarkCount() == 0);
    CHECK(readOld.LastPlayheadUs() == 0);
    // And `mark` does not then write a fresh file over the owner's bookmarks.
    CHECK_FALSE(notes::MayCreateNew(got));
    // The two refusals do not render identically: one is "delete it", the other is "upgrade".
    CHECK(strcmp(notes::LoadResultName(got),
                 notes::LoadResultName(notes::LoadResult::UnknownVersion)) != 0);

    notes::Sidecar readNew;
    REQUIRE(readNew.Load(pNew.c_str()) == notes::LoadResult::Ok);
    CHECK(readNew.BookmarkCount() == 1);
    CHECK(readNew.BookmarkAt(0)->tUs == 1800000000);
    CHECK(readNew.LastPlayheadUs() == 1805000000);
}

// ★ THE VERSION FIELD IS THE ONLY THING THAT TELLS THE TWO AXES APART, so a reader where the last
// copy of a key wins is one appended `"version": 2` away from adopting absolute times onto the
// 0-based axis. Every fixture here is this build's own writer output plus one inserted key, and
// the untouched document is loaded in the same case so the refusal is caused by the repeat alone.
TEST_CASE("a sidecar that names a key twice is refused, whatever the second copy says") {
    TempDir dir(L"dupkey");

    notes::Sidecar w;
    REQUIRE(w.AddBookmark(1800000000, L"great save") == notes::AddResult::Ok);
    w.SetLastPlayheadUs(1805000000);
    std::string current;
    w.Serialise(&current);

    const std::string mine = "\"version\": " + std::to_string(notes::kFormatVersion);
    const std::string theirs = "\"version\": " + std::to_string(notes::kFormatVersion1Absolute);
    const size_t at = current.find(mine);
    REQUIRE(at != std::string::npos);
    std::string old = current;
    old.replace(at, mine.size(), theirs);

    // Insert one member before the closing brace of the top-level object, or of the first bookmark.
    auto withAppended = [](std::string doc, const std::string& member) {
        const size_t close = doc.rfind('}');
        REQUIRE(close != std::string::npos);
        doc.insert(close, ", " + member);
        return doc;
    };
    auto withBookmarkField = [](std::string doc, const std::string& member) {
        const size_t open = doc.find("\"tUs\"");
        REQUIRE(open != std::string::npos);
        doc.insert(open, member + ", ");
        return doc;
    };

    // The reopened hole: a format-1 file with a trailing `"version": 2`.
    const std::string reversioned = withAppended(old, mine);
    REQUIRE(reversioned.size() == old.size() + mine.size() + 2);
    // And the mirror: a format-2 file with a trailing `"version": 1`, which must not become the
    // axis refusal either -- the file is not "the old format", it is a file that lies twice.
    const std::string downgraded = withAppended(current, theirs);

    struct Fixture {
        std::string name;
        std::string text;
        notes::LoadResult expected;
    };
    const Fixture fixtures[] = {
        {"format1-plus-version2", reversioned, notes::LoadResult::Damaged},
        {"format2-plus-version1", downgraded, notes::LoadResult::Damaged},
        {"twice-lastPlayheadUs", withAppended(current, "\"lastPlayheadUs\": 7"), notes::LoadResult::Damaged},
        {"twice-bookmarks", withAppended(current, "\"bookmarks\": []"), notes::LoadResult::Damaged},
        {"twice-tUs", withBookmarkField(current, "\"tUs\": 7"), notes::LoadResult::Damaged},
        {"twice-label", withBookmarkField(current, "\"label\": \"x\""), notes::LoadResult::Damaged},
        // Near-miss controls: the same bytes without the repeat.
        {"format2-untouched", current, notes::LoadResult::Ok},
        {"format1-untouched", old, notes::LoadResult::AbsoluteAxisVersion},
    };
    for (const Fixture& f : fixtures) {
        // A std::string, not a const char*: doctest stringifies a char pointer as its address, so
        // every red line named the fixture by a pointer and which of the eight had failed had to be
        // counted out. Measured -- INFO("fixture: " << f.name) prints the address too.
        INFO("fixture: " << f.name);
        const std::wstring wide(f.name.begin(), f.name.end());
        const std::wstring p = dir.File((wide + L".echodemo.notes.json").c_str());
        WriteBytes(p, f.text.data(), f.text.size());
        notes::Sidecar s;
        REQUIRE(s.AddBookmark(42, L"decoy") == notes::AddResult::Ok);
        s.SetLastPlayheadUs(43);
        const notes::LoadResult got = s.Load(p.c_str());
        CHECK(got == f.expected);
        if (f.expected == notes::LoadResult::Ok) {
            CHECK(s.BookmarkCount() == 1);
            CHECK(s.BookmarkAt(0)->tUs == 1800000000);
            CHECK(s.LastPlayheadUs() == 1805000000);
        } else {
            // Nothing from a refused document is adopted, and nothing may be written over it.
            CHECK(s.BookmarkCount() == 0);
            CHECK(s.LastPlayheadUs() == 0);
            CHECK_FALSE(notes::MayCreateNew(got));
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------------------------

TEST_CASE("a saved sidecar is on disk, reads back whole, and leaves no temporary behind") {
    TempDir dir(L"save");
    const std::wstring sidecar = dir.File(L"m.echodemo.notes.json");

    notes::Sidecar s;
    REQUIRE(s.AddBookmark(12500000, L"great save") == notes::AddResult::Ok);
    REQUIRE(s.AddBookmark(0, nullptr) == notes::AddResult::Ok);
    s.SetLastPlayheadUs(4242);
    REQUIRE(s.Save(sidecar.c_str()) == notes::SaveResult::Ok);
    REQUIRE(Exists(sidecar));

    notes::Sidecar back;
    REQUIRE(back.Load(sidecar.c_str()) == notes::LoadResult::Ok);
    REQUIRE(back.BookmarkCount() == 2);
    CHECK(back.LastPlayheadUs() == 4242);
    REQUIRE(back.BookmarkAt(0) != nullptr);
    CHECK(back.BookmarkAt(0)->tUs == 12500000);
    CHECK(wcscmp(back.BookmarkAt(0)->label, L"great save") == 0);
    REQUIRE(back.BookmarkAt(1) != nullptr);
    CHECK(back.BookmarkAt(1)->tUs == 0);
    CHECK(back.BookmarkAt(1)->label[0] == 0);

    // The atomic-replace temporary is not left lying beside the capture on the success path.
    WIN32_FIND_DATAW find;
    std::wstring glob(dir.Path());
    glob.append(L"\\*.tmp");
    HANDLE h = FindFirstFileW(glob.c_str(), &find);
    CHECK(h == INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
    }
}

TEST_CASE("a save that cannot open its temporary reports that, and does not report success") {
    TempDir dir(L"nodir");
    // A directory that does not exist. The old code answered one bool for this and for a path that
    // was simply too long, and `mark` printed a permission-shaped message for both.
    const std::wstring sidecar = dir.File(L"missing\\m.echodemo.notes.json");
    notes::Sidecar s;
    REQUIRE(s.AddBookmark(1, L"x") == notes::AddResult::Ok);
    const notes::SaveResult wrote = s.Save(sidecar.c_str());
    CHECK(wrote == notes::SaveResult::OpenFailed);
    CHECK_FALSE(wrote == notes::SaveResult::Ok);
    CHECK_FALSE(Exists(sidecar));
}

TEST_CASE("a rewrite replaces the file rather than appending to or truncating it") {
    TempDir dir(L"rewrite");
    const std::wstring sidecar = dir.File(L"m.echodemo.notes.json");

    notes::Sidecar first;
    REQUIRE(first.AddBookmark(1, L"one") == notes::AddResult::Ok);
    REQUIRE(first.Save(sidecar.c_str()) == notes::SaveResult::Ok);

    notes::Sidecar second;
    REQUIRE(second.Load(sidecar.c_str()) == notes::LoadResult::Ok);
    REQUIRE(second.AddBookmark(2, L"two") == notes::AddResult::Ok);
    REQUIRE(second.Save(sidecar.c_str()) == notes::SaveResult::Ok);

    notes::Sidecar back;
    REQUIRE(back.Load(sidecar.c_str()) == notes::LoadResult::Ok);
    REQUIRE(back.BookmarkCount() == 2);
    CHECK(back.BookmarkAt(1)->tUs == 2);
}

// ---------------------------------------------------------------------------------------------
// Path length -- the ceiling that was measured against the wrong number
// ---------------------------------------------------------------------------------------------

TEST_CASE("a capture path at the stated ceiling works, and one character more is refused") {
    TempDir dir(L"len");
    const size_t ceiling = notes::MaxDemoPathChars();
    CHECK(ceiling < MAX_PATH);

    const size_t dirLen = wcslen(dir.Path()) + 1;   // + the separator
    REQUIRE_MESSAGE(dirLen + 16 < ceiling,
                    "the temporary directory is too deep for this arm to build a path at the "
                    "ceiling; nothing was measured");

    std::wstring name(ceiling - dirLen, L'a');
    const std::wstring demo = std::wstring(dir.Path()) + L"\\" + name;
    REQUIRE(demo.size() == ceiling);

    wchar_t sidecar[kMaxPathChars];
    REQUIRE_MESSAGE(notes::SidecarPathFor(demo.c_str(), sidecar, kMaxPathChars),
                    "a capture path at the stated ceiling was refused");

    // It must be openable in both directions, which is the half the old check never covered: the
    // ceiling used to be the caller's buffer size, so SidecarPathFor said yes and every _wfopen_s
    // in the file then said no.
    notes::Sidecar s;
    REQUIRE(s.AddBookmark(1, L"x") == notes::AddResult::Ok);
    CHECK(s.Save(sidecar) == notes::SaveResult::Ok);
    notes::Sidecar back;
    CHECK(back.Load(sidecar) == notes::LoadResult::Ok);
    DeleteFileW(sidecar);

    const std::wstring tooLong = demo + L"a";
    CHECK_FALSE(notes::SidecarPathFor(tooLong.c_str(), sidecar, kMaxPathChars));
}

// ---------------------------------------------------------------------------------------------
// Labels: what the writer emits, the reader must accept
// ---------------------------------------------------------------------------------------------

TEST_CASE("a label the reader would refuse is refused at the door instead of written") {
    // An unpaired surrogate was written as a bare \uD83D escape and refused on read, so Save then
    // Load answered "damaged" and that capture's sidecar was refused for good.
    const wchar_t unpaired[] = {0x0041, 0xD83D, 0x0042, 0};
    notes::Sidecar s;
    CHECK(s.AddBookmark(1, unpaired) == notes::AddResult::LabelMalformed);
    CHECK(s.BookmarkCount() == 0);

    // The proper pair is text and still round trips, so the guard is not just refusing everything
    // above the BMP.
    const wchar_t pair[] = {0xD83D, 0xDE00, 0};
    TempDir dir(L"surrogate");
    const std::wstring p = dir.File(L"m.echodemo.notes.json");
    notes::Sidecar ok;
    REQUIRE(ok.AddBookmark(1, pair) == notes::AddResult::Ok);
    REQUIRE(ok.Save(p.c_str()) == notes::SaveResult::Ok);
    notes::Sidecar back;
    REQUIRE(back.Load(p.c_str()) == notes::LoadResult::Ok);
    REQUIRE(back.BookmarkCount() == 1);
    CHECK(wcscmp(back.BookmarkAt(0)->label, pair) == 0);
}

TEST_CASE("labels with JSON metacharacters and non-ASCII text round trip through a real file") {
    TempDir dir(L"labels");
    const std::wstring p = dir.File(L"m.echodemo.notes.json");

    const wchar_t* nasty = L"he said \"go\" \\ then\tstopped";
    const wchar_t* accented = L"\x00e9quipe \x30c6\x30b9\x30c8";
    const wchar_t control[] = {L'a', 0x0001, L'b', 0};

    notes::Sidecar s;
    REQUIRE(s.AddBookmark(1, nasty) == notes::AddResult::Ok);
    REQUIRE(s.AddBookmark(2, accented) == notes::AddResult::Ok);
    REQUIRE(s.AddBookmark(3, control) == notes::AddResult::Ok);
    REQUIRE(s.Save(p.c_str()) == notes::SaveResult::Ok);

    notes::Sidecar back;
    REQUIRE_MESSAGE(back.Load(p.c_str()) == notes::LoadResult::Ok,
                    "the file this writer produced did not survive its own reader");
    REQUIRE(back.BookmarkCount() == 3);
    CHECK(wcscmp(back.BookmarkAt(0)->label, nasty) == 0);
    CHECK(wcscmp(back.BookmarkAt(1)->label, accented) == 0);
    CHECK(wcscmp(back.BookmarkAt(2)->label, control) == 0);
}

TEST_CASE("the label cap is one number, reported as one reason") {
    std::wstring tooLong(notes::kMaxLabelChars, L'x');   // one over, counting the terminator
    notes::Sidecar s;
    CHECK(s.AddBookmark(1, tooLong.c_str()) == notes::AddResult::LabelTooLong);

    std::wstring atCap(notes::kMaxLabelChars - 1, L'x');
    CHECK(s.AddBookmark(1, atCap.c_str()) == notes::AddResult::Ok);

    // The parser's cap and this one are the same constant, or `mark` accepts what the sidecar
    // refuses. notes.h static_asserts it; this states it where a reader will see it.
    CHECK(notes::kMaxLabelChars == kMaxMarkLabelChars);
}

// ---------------------------------------------------------------------------------------------
// Bounds
// ---------------------------------------------------------------------------------------------

TEST_CASE("the bookmark list is capped, and says which cap it hit") {
    notes::Sidecar s;
    size_t added = 0;
    while (s.AddBookmark(static_cast<int64_t>(added), L"x") == notes::AddResult::Ok) {
        ++added;
        REQUIRE(added <= notes::kMaxBookmarks + 1);
    }
    CHECK(added == notes::kMaxBookmarks);
    CHECK(s.BookmarkCount() == notes::kMaxBookmarks);
    CHECK(s.AddBookmark(1, L"x") == notes::AddResult::Full);
}

TEST_CASE("an index past the end is nothing, not bookmark zero") {
    notes::Sidecar empty;
    CHECK(empty.BookmarkAt(0) == nullptr);
    CHECK(empty.BookmarkAt(1) == nullptr);
    CHECK(empty.BookmarkAt(notes::kMaxBookmarks) == nullptr);

    notes::Sidecar one;
    REQUIRE(one.AddBookmark(999, L"only") == notes::AddResult::Ok);
    REQUIRE(one.BookmarkAt(0) != nullptr);
    CHECK(one.BookmarkAt(0)->tUs == 999);
    // The old fold-to-zero answered this with bookmark 0, which reads as a real bookmark.
    CHECK(one.BookmarkAt(1) == nullptr);
}

TEST_CASE("a 256-bookmark sidecar survives a full round trip on disk") {
    TempDir dir(L"full");
    const std::wstring p = dir.File(L"m.echodemo.notes.json");

    notes::Sidecar s;
    for (size_t i = 0; i < notes::kMaxBookmarks; ++i) {
        wchar_t label[32];
        swprintf_s(label, L"mark %zu", i);
        REQUIRE(s.AddBookmark(static_cast<int64_t>(i) * 1000, label) == notes::AddResult::Ok);
    }
    s.SetLastPlayheadUs(123456);
    REQUIRE(s.Save(p.c_str()) == notes::SaveResult::Ok);

    notes::Sidecar back;
    REQUIRE(back.Load(p.c_str()) == notes::LoadResult::Ok);
    REQUIRE(back.BookmarkCount() == notes::kMaxBookmarks);
    CHECK(back.LastPlayheadUs() == 123456);
    CHECK(back.BookmarkAt(notes::kMaxBookmarks - 1)->tUs ==
          static_cast<int64_t>(notes::kMaxBookmarks - 1) * 1000);
    CHECK(back.BookmarkAt(notes::kMaxBookmarks) == nullptr);
}

TEST_CASE("a file holding more bookmarks than the cap is refused whole") {
    TempDir dir(L"overcap");
    const std::wstring p = dir.File(L"m.echodemo.notes.json");

    std::string text = "{" + CurrentVersionField() + ",\"lastPlayheadUs\":0,\"bookmarks\":[";
    for (size_t i = 0; i <= notes::kMaxBookmarks; ++i) {
        if (i != 0) {
            text.append(",");
        }
        text.append("{\"tUs\":1,\"label\":\"x\"}");
    }
    text.append("]}");
    WriteBytes(p, text.data(), text.size());

    notes::Sidecar s;
    // Keeping the first 256 of 257 would silently lose one of the owner's own notes.
    CHECK(s.Load(p.c_str()) == notes::LoadResult::Damaged);
    CHECK(s.BookmarkCount() == 0);
}

// ---------------------------------------------------------------------------------------------
// The paths that never reach a file
// ---------------------------------------------------------------------------------------------

TEST_CASE("the sidecar path sits beside the capture and never inside it") {
    wchar_t out[kMaxPathChars];
    REQUIRE(notes::SidecarPathFor(L"C:\\r\\match.echodemo", out, kMaxPathChars));
    CHECK(wcscmp(out, L"C:\\r\\match.echodemo.notes.json") == 0);

    REQUIRE(notes::SidecarPathFor(L"match.echodemo", out, kMaxPathChars));
    CHECK(wcscmp(out, L"match.echodemo.notes.json") == 0);

    CHECK_FALSE(notes::SidecarPathFor(L"", out, kMaxPathChars));
    CHECK_FALSE(notes::SidecarPathFor(L"C:\\r\\match.echodemo", out, 8));
}

TEST_CASE("a damaged document is refused rather than half-read") {
    notes::Sidecar s;
    CHECK(s.Parse("", 0) == notes::LoadResult::Damaged);
    CHECK(s.Parse("{", 1) == notes::LoadResult::Damaged);
    CHECK(s.Parse("not json at all", 15) == notes::LoadResult::Damaged);
    CHECK(s.BookmarkCount() == 0);

    const std::string empty = "{" + CurrentVersionField() + "," +
                              IdentityField(std::string(32, 'a'), 4096, 42) +
                              ",\"lastPlayheadUs\":0,\"bookmarks\":[]}";
    CHECK(s.Parse(empty.data(), empty.size()) == notes::LoadResult::Ok);
    CHECK(s.BookmarkCount() == 0);
}

TEST_CASE("every outcome has a sentence, and no two share one") {
    // `mark`, `marks` and `diag` all reach the owner through this one function, so an outcome it
    // does not name arrives as the fallback "not read": six answers rendering as five is the same
    // defect as five answers being computed as one.
    const notes::LoadResult loads[] = {notes::LoadResult::Ok,
                                       notes::LoadResult::Absent,
                                       notes::LoadResult::Unreadable,
                                       notes::LoadResult::Damaged,
                                       notes::LoadResult::UnknownVersion,
                                       notes::LoadResult::AbsoluteAxisVersion,
                                       notes::LoadResult::Unidentified,
                                       notes::LoadResult::StaleIdentity,
                                       notes::LoadResult::StaleIdentityHeld};
    constexpr size_t kCount = sizeof loads / sizeof loads[0];
    for (size_t i = 0; i < kCount; ++i) {
        REQUIRE(notes::LoadResultName(loads[i]) != nullptr);
        CHECK(notes::LoadResultName(loads[i])[0] != 0);
        CHECK(strcmp(notes::LoadResultName(loads[i]), "not read") != 0);
        for (size_t j = i + 1; j < kCount; ++j) {
            CHECK(strcmp(notes::LoadResultName(loads[i]), notes::LoadResultName(loads[j])) != 0);
        }
    }
    CHECK(strcmp(notes::AddResultName(notes::AddResult::Full),
                 notes::AddResultName(notes::AddResult::LabelTooLong)) != 0);
    CHECK(strcmp(notes::SaveResultName(notes::SaveResult::PathTooLong),
                 notes::SaveResultName(notes::SaveResult::OpenFailed)) != 0);
}

// ---------------------------------------------------------------------------------------------
// L3-RUN-3 F3: a sidecar outlives the capture it names, and must not be applied to its successor
// ---------------------------------------------------------------------------------------------

namespace {

// A capture that is nothing but a real .echodemo file header plus filler, which is all
// IdentityForCapture reads. Written through the loader's own struct so a field moving in format.h
// moves here too.
bool WriteDemo(const std::wstring& path, const uint8_t uuid[16], uint64_t t0Us, size_t fillerBytes,
               uint8_t fillerByte) {
    replay::FileHeaderDisk head;
    memset(&head, 0, sizeof head);
    memcpy(head.magic, replay::kFileMagic, sizeof head.magic);
    head.formatMajor = replay::kFormatMajor;
    head.formatMinor = replay::kFormatMinor;
    head.t0Us = t0Us;
    memcpy(head.demoUuid, uuid, sizeof head.demoUuid);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || f == nullptr) {
        return false;
    }
    bool ok = fwrite(&head, 1, sizeof head, f) == sizeof head;
    for (size_t i = 0; ok && i < fillerBytes; ++i) {
        ok = fputc(fillerByte, f) != EOF;
    }
    return fclose(f) == 0 && ok;
}

// The one aside file in a directory, or an empty string. The name carries the identity of the
// capture the sidecar belonged to, which is why it is found by pattern rather than spelled out.
std::wstring FindAside(const std::wstring& sidecar) {
    std::wstring glob(sidecar);
    glob.append(L".stale-*");
    WIN32_FIND_DATAW find;
    HANDLE h = FindFirstFileW(glob.c_str(), &find);
    if (h == INVALID_HANDLE_VALUE) {
        return std::wstring();
    }
    std::wstring dir(sidecar, 0, sidecar.find_last_of(L'\\') + 1);
    std::wstring found = dir + find.cFileName;
    FindClose(h);
    return found;
}

// The filler beside the header. The regenerated capture must be a DIFFERENT length, so its size is
// derived from the first rather than written out as a second bare number.
constexpr size_t kFillerBytes = 4096;
constexpr size_t kRegeneratedFillerBytes = kFillerBytes * 2;

const uint8_t kZeroUuid[16] = {0};
const uint8_t kUuidA[16] = {0x9a, 0x11, 0x4e, 0x7c, 0x00, 0x01, 0x42, 0x00,
                            0x8b, 0x33, 0xde, 0xad, 0xbe, 0xef, 0x00, 0x11};
const uint8_t kUuidB[16] = {0x9a, 0x11, 0x4e, 0x7c, 0x00, 0x01, 0x42, 0x00,
                            0x8b, 0x33, 0xde, 0xad, 0xbe, 0xef, 0x00, 0x12};

}  // namespace

TEST_CASE("bookmarks survive a plain re-open of the same capture") {
    TempDir dir(L"same");
    const std::wstring demo = dir.File(L"match.echodemo");
    REQUIRE(WriteDemo(demo, kUuidA, 1700000000000000ull, kFillerBytes, 0x5A));

    wchar_t sidecar[MAX_PATH * 2] = {0};
    REQUIRE(notes::SidecarPathFor(demo.c_str(), sidecar, MAX_PATH * 2));

    notes::CaptureIdentity id;
    REQUIRE(notes::IdentityForCapture(demo.c_str(), &id));

    notes::Sidecar writer;
    writer.SetIdentity(id);
    REQUIRE(writer.AddBookmark(2133312, L"first goal") == notes::AddResult::Ok);
    writer.SetLastPlayheadUs(2255533);
    REQUIRE(writer.Save(sidecar) == notes::SaveResult::Ok);

    // The re-open: the identity is read from the capture again, exactly as RefreshNotes does.
    notes::CaptureIdentity reopened;
    REQUIRE(notes::IdentityForCapture(demo.c_str(), &reopened));
    notes::Sidecar reader;
    const notes::LoadResult got = reader.LoadFor(sidecar, reopened);
    CHECK_MESSAGE(got == notes::LoadResult::Ok, notes::LoadResultName(got));
    REQUIRE(reader.BookmarkCount() == 1);
    CHECK(reader.BookmarkAt(0)->tUs == 2133312);
    CHECK(wcscmp(reader.BookmarkAt(0)->label, L"first goal") == 0);
    CHECK(reader.LastPlayheadUs() == 2255533);
    CHECK_FALSE(Exists(FindAside(sidecar)));
}

TEST_CASE("a regenerated capture refuses the old sidecar by name and starts empty") {
    TempDir dir(L"regen");
    const std::wstring demo = dir.File(L"W2-G-fixture.echodemo");
    REQUIRE(WriteDemo(demo, kUuidA, 1700000000000000ull, kFillerBytes, 0x5A));

    wchar_t sidecar[MAX_PATH * 2] = {0};
    REQUIRE(notes::SidecarPathFor(demo.c_str(), sidecar, MAX_PATH * 2));

    notes::CaptureIdentity first;
    REQUIRE(notes::IdentityForCapture(demo.c_str(), &first));
    notes::Sidecar writer;
    writer.SetIdentity(first);
    REQUIRE(writer.AddBookmark(2133312, L"launch one") == notes::AddResult::Ok);
    REQUIRE(writer.AddBookmark(2255533, L"launch one again") == notes::AddResult::Ok);
    writer.SetLastPlayheadUs(2255533);
    REQUIRE(writer.Save(sidecar) == notes::SaveResult::Ok);

    // F3's own scenario: the fixture is regenerated in place -- new uuid, new length -- and the
    // sidecar beside it is untouched.
    REQUIRE(WriteDemo(demo, kUuidB, 1700000000000001ull, kRegeneratedFillerBytes, 0x77));
    notes::CaptureIdentity second;
    REQUIRE(notes::IdentityForCapture(demo.c_str(), &second));
    REQUIRE_FALSE(first.Matches(second));

    notes::Sidecar reader;
    const notes::LoadResult got = reader.LoadFor(sidecar, second);
    CHECK_MESSAGE(got == notes::LoadResult::StaleIdentity, notes::LoadResultName(got));
    // The refusal is reachable by the name the report asks for, not only by an enum value.
    CHECK(strstr(notes::LoadResultName(got), "notes-stale-identity") != nullptr);

    // Nothing of the old capture's is applied to the new one: not a bookmark, not the resume point.
    CHECK(reader.BookmarkCount() == 0);
    CHECK(reader.LastPlayheadUs() == 0);

    // Renamed aside, not deleted: the old marks are still readable where they were put.
    CHECK_FALSE(Exists(sidecar));
    const std::wstring aside = FindAside(sidecar);
    REQUIRE_FALSE(aside.empty());
    notes::Sidecar rescued;
    REQUIRE(rescued.Load(aside.c_str()) == notes::LoadResult::Ok);
    CHECK(rescued.BookmarkCount() == 2);
    CHECK(rescued.LastPlayheadUs() == 2255533);

    // And the path is free, so the new capture can be marked -- the feature, not just the refusal.
    CHECK(notes::MayCreateNew(got));
    notes::Sidecar fresh;
    fresh.SetIdentity(second);
    REQUIRE(fresh.AddBookmark(500, L"launch two") == notes::AddResult::Ok);
    REQUIRE(fresh.Save(sidecar) == notes::SaveResult::Ok);
    notes::Sidecar back;
    REQUIRE(back.LoadFor(sidecar, second) == notes::LoadResult::Ok);
    CHECK(back.BookmarkCount() == 1);
}

TEST_CASE("a capture written before demoUuid existed is identified by its first 64 KiB") {
    TempDir dir(L"legacy");
    const std::wstring demo = dir.File(L"old.echodemo");
    REQUIRE(WriteDemo(demo, kZeroUuid, 42, 4096, 0x11));

    notes::CaptureIdentity legacy;
    REQUIRE(notes::IdentityForCapture(demo.c_str(), &legacy));
    CHECK(legacy.IsLegacy());
    // The digest is the identity here, so it has to have been computed.
    bool anyDigestByte = false;
    for (size_t i = 0; i < 16; ++i) {
        anyDigestByte = anyDigestByte || legacy.headDigest[i] != 0;
    }
    CHECK(anyDigestByte);

    notes::Sidecar writer;
    writer.SetIdentity(legacy);
    REQUIRE(writer.AddBookmark(9000, L"old mark") == notes::AddResult::Ok);
    wchar_t sidecar[MAX_PATH * 2] = {0};
    REQUIRE(notes::SidecarPathFor(demo.c_str(), sidecar, MAX_PATH * 2));
    REQUIRE(writer.Save(sidecar) == notes::SaveResult::Ok);

    notes::Sidecar same;
    CHECK(same.LoadFor(sidecar, legacy) == notes::LoadResult::Ok);
    CHECK(same.BookmarkCount() == 1);

    // Regenerated with the same zero uuid, the same length and the same t0Us, and different
    // CONTENT. uuid, size and t0Us all agree; only the digest can tell these two apart.
    REQUIRE(WriteDemo(demo, kZeroUuid, 42, 4096, 0x22));
    notes::CaptureIdentity other;
    REQUIRE(notes::IdentityForCapture(demo.c_str(), &other));
    CHECK(other.sizeBytes == legacy.sizeBytes);
    CHECK(other.t0Us == legacy.t0Us);
    CHECK(memcmp(other.headDigest, legacy.headDigest, 16) != 0);
    CHECK_FALSE(legacy.Matches(other));

    notes::Sidecar refused;
    const notes::LoadResult got = refused.LoadFor(sidecar, other);
    CHECK_MESSAGE(got == notes::LoadResult::StaleIdentity, notes::LoadResultName(got));
    CHECK(refused.BookmarkCount() == 0);
    REQUIRE_FALSE(FindAside(sidecar).empty());
}

TEST_CASE("a sidecar that names no capture is refused rather than adopted") {
    TempDir dir(L"unbound");
    const std::wstring demo = dir.File(L"match.echodemo");
    REQUIRE(WriteDemo(demo, kUuidA, 7, 1024, 0x33));
    wchar_t sidecar[MAX_PATH * 2] = {0};
    REQUIRE(notes::SidecarPathFor(demo.c_str(), sidecar, MAX_PATH * 2));

    // Exactly what the sidecars written before this change look like: our version, our keys, and
    // no statement about which capture they belong to.
    const std::string doc = "{" + CurrentVersionField() +
                            ",\"lastPlayheadUs\":2255533,\"bookmarks\":[{\"tUs\":2133312,"
                            "\"label\":\"somebody else\"}]}";
    FILE* f = nullptr;
    REQUIRE(_wfopen_s(&f, sidecar, L"wb") == 0);
    REQUIRE(f != nullptr);
    REQUIRE(fwrite(doc.data(), 1, doc.size(), f) == doc.size());
    REQUIRE(fclose(f) == 0);

    notes::Sidecar raw;
    CHECK(raw.Load(sidecar) == notes::LoadResult::Unidentified);

    notes::CaptureIdentity id;
    REQUIRE(notes::IdentityForCapture(demo.c_str(), &id));
    notes::Sidecar reader;
    const notes::LoadResult got = reader.LoadFor(sidecar, id);
    CHECK_MESSAGE(got == notes::LoadResult::StaleIdentity, notes::LoadResultName(got));
    CHECK(reader.BookmarkCount() == 0);
    CHECK(reader.LastPlayheadUs() == 0);
    REQUIRE_FALSE(FindAside(sidecar).empty());
}

TEST_CASE("a capture whose header cannot be read yields no identity at all") {
    TempDir dir(L"noheader");
    const std::wstring missing = dir.File(L"nothing.echodemo");
    notes::CaptureIdentity id;
    CHECK_FALSE(notes::IdentityForCapture(missing.c_str(), &id));

    // A file that is not an .echodemo is not one we can bind a sidecar to either.
    const std::wstring foreign = dir.File(L"notes.txt");
    FILE* f = nullptr;
    REQUIRE(_wfopen_s(&f, foreign.c_str(), L"wb") == 0);
    REQUIRE(f != nullptr);
    const char junk[256] = {0};
    REQUIRE(fwrite(junk, 1, sizeof junk, f) == sizeof junk);
    REQUIRE(fclose(f) == 0);
    CHECK_FALSE(notes::IdentityForCapture(foreign.c_str(), &id));
}
