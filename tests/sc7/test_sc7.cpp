// P12's control suite for the SC7 in-process reader.
//
// The reader's whole job is to be able to say FAIL, and the only way to know it can is to plant a
// difference and watch it come back with the right RVA. Every case below drives sc7::Compare against two
// synthetic .text buffers, so none of them needs a game, a mapping or a hook.
//
// ★ The suite is built so a plausible weakening of the comparison turns something red:
//   - a run that stops one page short of the end     -> LastPagePlantIsFound
//   - a reloc field masked instead of delta-checked  -> RelocFieldHoldingSomethingElse
//   - a patch window covering only the target itself -> HotPatchDisposition
//   - a denominator taken from the site table        -> LedgerEmptyWithADetour
//   - a later refusal overwriting an earlier FAIL    -> FailThenRefusal
//
// ★ No plant offset is derived from the function under test. The hot-patch case in particular takes its
// -5 from MinHook's hook.c, because a fixture that moves with the window cannot detect the window moving.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <doctest.h>
#include <windows.h>

#include "bootstrap/config.h"
#include "generated/facts_generated.h"
#include "bootstrap/jsonesc.h"
#include "bootstrap/sc7.h"

using namespace replay;

namespace {

constexpr size_t kSpan = 8192;          // two pages, so "the last page" is a real place
constexpr uint64_t kTextRva = 0x1000;
constexpr int64_t kDelta = 0x10000000;
constexpr uint64_t kSiteRva = kTextRva + 0x800;

struct Fixture {
    uint8_t disk[kSpan];
    uint8_t live[kSpan];
    uint32_t relocs[4];

    Fixture() {
        // Not zeros: a buffer of zeros makes memcmp agree for the wrong reason, and a planted byte in it
        // would be found by a comparison that only looks for non-zero.
        for (size_t i = 0; i < kSpan; ++i) {
            disk[i] = static_cast<uint8_t>((i * 31u + 7u) & 0xFF);
        }
        std::memcpy(live, disk, kSpan);
        std::memset(relocs, 0, sizeof relocs);
    }

    sc7::ImageView View(size_t relocCount = 0) const {
        sc7::ImageView v = {};
        v.disk = disk;
        v.live = live;
        v.len = kSpan;
        v.textRva = kTextRva;
        v.relocDelta = kDelta;
        v.relocOffsets = relocs;
        v.relocCount = relocCount;
        return v;
    }

    void PlantJump(size_t offset) {
        live[offset] = 0xE9;
        std::memcpy(live + offset + 1, "\x11\x22\x33\x44", 4);
    }

    void PlantRelocField(size_t offset, uint64_t diskValue, uint64_t liveValue) {
        std::memcpy(disk + offset, &diskValue, sizeof diskValue);
        std::memcpy(live + offset, &liveValue, sizeof liveValue);
        relocs[0] = static_cast<uint32_t>(offset);
    }
};

sc7::SiteView VoipSite() {
    sc7::SiteView s = {};
    s.factId = "VOIP.RECV";
    s.row = "2.3";
    s.rva = kSiteRva;
    s.armed = true;
    return s;
}

// The `"rva": "0x..."` field a record carries for one address, built from the address rather than
// transcribed beside it.
std::string RvaText(uint64_t rva) {
    char text[48];
    std::snprintf(text, sizeof text, "\"rva\": \"0x%llX\"", static_cast<unsigned long long>(rva));
    return text;
}

void Declare(sc7::Sc7Result& r, const char* row) {
    std::snprintf(r.expected[r.expectedCount].id, sc7::kRowChars, "%s", row);
    ++r.expectedCount;
}

bool IsDeferred(const sc7::Sc7Result& r, const char* id) {
    for (uint32_t i = 0; i < r.deferredCount; ++i) {
        if (std::strcmp(r.deferred[i].id, id) == 0) {
            return true;
        }
    }
    return false;
}

bool HasRow(const sc7::Row* rows, uint32_t n, const char* id) {
    for (uint32_t i = 0; i < n; ++i) {
        if (std::strcmp(rows[i].id, id) == 0) {
            return true;
        }
    }
    return false;
}

std::wstring TempDir() {
    wchar_t base[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, base);
    REQUIRE(n != 0);
    std::wstring dir(base, n);
    dir += L"replay_sc7_test";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::string ReadAll(const std::wstring& path) {
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return std::string();
    }
    std::string text;
    char chunk[4096];
    DWORD got = 0;
    while (ReadFile(f, chunk, sizeof chunk, &got, nullptr) && got > 0) {
        text.append(chunk, got);
    }
    CloseHandle(f);
    return text;
}

}  // namespace

TEST_CASE("an undeclared three-byte difference is named, not counted") {
    Fixture fx;
    fx.live[0x500] ^= 0xFF;
    fx.live[0x501] ^= 0xFF;
    fx.live[0x502] ^= 0xFF;

    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    const sc7::SiteView site = VoipSite();
    fx.PlantJump(0x800);   // the declared detour, so the denominator is satisfied and only the plant fails
    const sc7::ImageView image = fx.View();
    sc7::Compare(image, &site, 1, r);
    sc7::Decide(r);

    CHECK(r.verdict == sc7::Verdict::Fail);
    REQUIRE(r.unattributedTotal == 1);
    REQUIRE(r.unattributedCount == 1);
    CHECK(r.unattributed[0].rva == kTextRva + 0x500);
    CHECK(r.unattributed[0].len == 3);
    CHECK(r.unattributed[0].before[0] == fx.disk[0x500]);
    CHECK(r.unattributed[0].after[0] == fx.live[0x500]);
    // The declared row is still observed: a failure elsewhere must not swallow the attribution.
    CHECK(HasRow(r.observed, r.observedCount, "2.3"));
}

TEST_CASE("a declared detour at a registered site passes and is attributed to its row") {
    Fixture fx;
    fx.PlantJump(0x800);

    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    const sc7::SiteView site = VoipSite();
    sc7::Compare(fx.View(), &site, 1, r);
    sc7::Decide(r);

    CHECK(r.verdict == sc7::Verdict::Pass);
    CHECK(r.unattributedTotal == 0);
    REQUIRE(r.observedCount == 1);
    CHECK(std::string(r.observed[0].id) == "2.3");
    REQUIRE(r.detailCount == 1);
    CHECK(r.detail[0].rva == kSiteRva);
    CHECK(std::string(r.detail[0].factId) == "VOIP.RECV");
}

TEST_CASE("MinHook's hot-patch disposition is inside the window") {
    // MinHook writes the reaching jump five bytes BEFORE the target when the target has a hot-patch
    // area, and a two-byte short jump at the target itself. A window anchored only at the target reads
    // that as an undeclared patch - SC7 failing on its own success.
    //
    // The 5 is MinHook's, not this reader's: third_party/minhook/src/hook.c:397, `pPatchTarget -=
    // sizeof(JMP_REL)` under #ifdef _M_X64 with trampoline.h:31's #pragma pack(push,1) making JMP_REL
    // exactly 5 bytes. It must NOT come from sc7::PatchWindowBefore(), or the fixture moves with the
    // function under test and the case cannot fail (S9.6).
    constexpr size_t kMinHookPatchAboveBackup = 5;
    Fixture fx;
    fx.PlantJump(0x800 - kMinHookPatchAboveBackup);
    fx.live[0x800] = 0xEB;
    fx.live[0x801] = 0xF9;

    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    const sc7::SiteView site = VoipSite();
    sc7::Compare(fx.View(), &site, 1, r);
    sc7::Decide(r);

    CHECK(r.verdict == sc7::Verdict::Pass);
    CHECK(r.unattributedTotal == 0);
    CHECK(HasRow(r.observed, r.observedCount, "2.3"));
}

TEST_CASE("the patch window is MinHook's two dispositions, to the byte") {
    // A known-answer control, so the window cannot be quietly retyped. The numbers are read off MinHook's
    // source, not off this reader: trampoline.h:31 packs the structs to 1, hook.c:397-401 backs the patch
    // target up by sizeof(JMP_REL) = 5 and extends it by sizeof(JMP_REL_SHORT) = 2.
    CHECK(sc7::PatchWindowBefore() == 5);
    CHECK(sc7::PatchWindowAfter() == 5 + 2);
}

TEST_CASE("a patch one byte beyond MinHook's reach is not excused by the window") {
    // The other side of the same claim: the window is as wide as the two dispositions and no wider, so a
    // window widened "to be safe" turns this red.
    Fixture fx;
    fx.PlantJump(0x800);
    fx.live[0x800 - 6] ^= 0xFF;

    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    const sc7::SiteView site = VoipSite();
    sc7::Compare(fx.View(), &site, 1, r);
    sc7::Decide(r);

    CHECK(r.verdict == sc7::Verdict::Fail);
    REQUIRE(r.unattributedTotal == 1);
    CHECK(r.unattributed[0].rva == kSiteRva - 6);
}

TEST_CASE("the window's blind spot is counted, not assumed away") {
    // The window excuses 12 bytes per site; the widest disposition MinHook writes is 7. The difference is
    // a hole a patch could sit in, and the packet's own standard is that an excused set is published.
    Fixture fx;
    fx.PlantJump(0x800);

    sc7::SiteView armed = VoipSite();
    sc7::Sc7Result r = {};
    sc7::Compare(fx.View(), &armed, 1, r);
    CHECK(r.windowBlindBytes == 12 - 7);

    sc7::SiteView idle = VoipSite();
    idle.armed = false;
    sc7::Sc7Result unarmed = {};
    sc7::Compare(fx.View(), &idle, 1, unarmed);
    CHECK(unarmed.windowBlindBytes == 12);   // nothing was written there, so the whole window is blind
}

TEST_CASE("a relocated field differing by exactly the load delta is excused and counted") {
    Fixture fx;
    fx.PlantJump(0x800);
    const uint64_t onDisk = 0x0000000140001000ull;
    fx.PlantRelocField(0x600, onDisk, onDisk + static_cast<uint64_t>(kDelta));

    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    const sc7::SiteView site = VoipSite();
    sc7::Compare(fx.View(1), &site, 1, r);
    sc7::Decide(r);

    CHECK(r.verdict == sc7::Verdict::Pass);
    CHECK(r.unattributedTotal == 0);
    CHECK(r.relocBytesSkipped == sizeof(uint64_t));
    CHECK(r.relocFieldsInText == 1);
}

TEST_CASE("a relocated field holding anything else is reported, not masked") {
    Fixture fx;
    fx.PlantJump(0x800);
    const uint64_t onDisk = 0x0000000140001000ull;
    fx.PlantRelocField(0x600, onDisk, onDisk + static_cast<uint64_t>(kDelta) + 1);

    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    const sc7::SiteView site = VoipSite();
    sc7::Compare(fx.View(1), &site, 1, r);
    sc7::Decide(r);

    CHECK(r.verdict == sc7::Verdict::Fail);
    // Reported as one qword, not as the two byte-runs the two values happen to disagree on.
    REQUIRE(r.unattributedTotal == 1);
    CHECK(r.unattributed[0].rva == kTextRva + 0x600);
    CHECK(r.unattributed[0].len == sizeof(uint64_t));
    CHECK(r.relocBytesSkipped == 0);
}

TEST_CASE("a plant in the last page is found") {
    // The arm that catches a comparison that stops short. A run that walked all but the final page would
    // pass every case above and this one alone turns red.
    Fixture fx;
    fx.PlantJump(0x800);
    const size_t at = kSpan - 3;
    fx.live[at] ^= 0xFF;
    fx.live[at + 1] ^= 0xFF;
    fx.live[at + 2] ^= 0xFF;

    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    const sc7::SiteView site = VoipSite();
    sc7::Compare(fx.View(), &site, 1, r);
    sc7::Decide(r);

    CHECK(r.verdict == sc7::Verdict::Fail);
    REQUIRE(r.unattributedTotal == 1);
    CHECK(r.unattributed[0].rva == kTextRva + at);
    CHECK(r.unattributed[0].len == 3);
}

TEST_CASE("a declared row with no detour is a failure, not a pass") {
    Fixture fx;   // nothing planted at all
    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    const sc7::SiteView site = VoipSite();
    sc7::Compare(fx.View(), &site, 1, r);
    sc7::Decide(r);

    CHECK(r.verdict == sc7::Verdict::Fail);
    CHECK(r.observedCount == 0);
}

TEST_CASE("an empty ledger with a detour standing in .text is FAIL, never PASS") {
    // S9.12. The denominator comes from the declaration; a reader that took it from the site table would
    // report a tidy PASS here, because expected and observed would agree by construction.
    Fixture fx;
    fx.PlantJump(0x800);

    sc7::Sc7Result r = {};
    const sc7::SiteView site = VoipSite();
    r.armedLedgerSites = 1;
    sc7::Compare(fx.View(), &site, 1, r);
    sc7::Decide(r);

    CHECK(r.expectedCount == 0);
    CHECK(r.verdict == sc7::Verdict::Fail);
    CHECK(std::string(r.reason).find("ledger-empty") != std::string::npos);
    CHECK_FALSE(r.refused);
}

TEST_CASE("an empty ledger FAILs on a detour no deferral excuses, whatever else is deferred") {
    // AUDIT_K1 K1-2, reproduced as the auditor planted it. Row 7.7 is observed, declared by nobody
    // and deferred by nobody; row 9.9 is deferred and has nothing to do with it. Testing the
    // deferral first returned CouldNotAsk here, so a genuine undeclared detour rendered NOT-RUN --
    // H-2's defect in the harsher direction, twenty lines from H-2's own fix.
    sc7::Sc7Result r = {};
    std::snprintf(r.observed[0].id, sc7::kRowChars, "7.7");
    r.observedCount = 1;
    std::snprintf(r.deferred[0].id, sc7::kRowChars, "9.9");
    std::snprintf(r.deferred[0].key, sizeof r.deferred[0].key, "capture.voip");
    std::snprintf(r.deferred[0].reason, sizeof r.deferred[0].reason, "provider-still-deciding");
    r.deferredCount = 1;
    r.armedLedgerSites = 1;

    sc7::Decide(r);

    REQUIRE(r.expectedCount == 0);
    CHECK(r.verdict == sc7::Verdict::Fail);
    CHECK_FALSE(r.refused);
    CHECK(std::string(r.reason).find("ledger-empty") == 0);
    CHECK(std::string(r.reason).find("1 of them") != std::string::npos);
    // The FAIL is the detour's, not the armed site's: an armed site with nothing observed is still
    // a refusal while a row is deferred, which is the case the suite already pins below.
    CHECK(std::string(r.reason).find("ledger-deferred") == std::string::npos);

    SUBCASE("the same detour AT the deferred row is a refusal, because deferral excuses that row") {
        sc7::Sc7Result q = {};
        std::snprintf(q.observed[0].id, sc7::kRowChars, "9.9");
        q.observedCount = 1;
        q.deferred[0] = r.deferred[0];
        q.deferredCount = 1;
        q.armedLedgerSites = 1;
        sc7::Decide(q);
        CHECK(q.verdict == sc7::Verdict::CouldNotAsk);
        CHECK(q.refused);
        CHECK(std::string(q.reason).find("ledger-deferred") == 0);
    }

    SUBCASE("one undeferred detour among deferred ones still decides it") {
        sc7::Sc7Result q = {};
        std::snprintf(q.observed[0].id, sc7::kRowChars, "9.9");
        std::snprintf(q.observed[1].id, sc7::kRowChars, "7.7");
        q.observedCount = 2;
        q.deferred[0] = r.deferred[0];
        q.deferredCount = 1;
        sc7::Decide(q);
        CHECK(q.verdict == sc7::Verdict::Fail);
        CHECK(std::string(q.reason).find("1 of them") != std::string::npos);
    }
}

TEST_CASE("an empty ledger with nothing installed is COULD-NOT-ASK, and says so") {
    sc7::Sc7Result r = {};
    sc7::Decide(r);

    CHECK(r.verdict == sc7::Verdict::CouldNotAsk);
    CHECK(r.refused);
    CHECK(std::string(r.reason).find("ledger-empty") != std::string::npos);
    CHECK(std::string(sc7::VerdictName(r.verdict)) == "COULD-NOT-ASK");
}

TEST_CASE("a refusal reached before the comparison stays a refusal") {
    sc7::Sc7Result r = {};
    r.refused = true;
    std::snprintf(r.reason, sizeof r.reason, "the on-disk image could not be opened for reading");
    Declare(r, "2.3");
    sc7::Decide(r);
    CHECK(r.verdict == sc7::Verdict::CouldNotAsk);
}

TEST_CASE("the shipping default declares the VOIP row and the offline lock") {
    // P12's stated control, and the reason RULE 0 is satisfiable at all: if this were empty, SC7's
    // runtime half would be a gate nothing can fail.
    config::ResetForTest();
    sc7::Sc7Result r = {};
    sc7::DeclaredRows(r);
    // Four rows since ARCH-22 defined 2.6: 2.4 is the offline lock's detour at 0x1042B0, 2.5 is
    // the in-frame draw site at 0x1415483B0, and 2.6 is the CAkSinkXAudio2::Term lifetime detour at
    // AUDIO.SINK_TERM. SC7 reads each row off its key - `capture.voip`, `bootstrap.offline_bit_lock`,
    // `ui.inframe_draw`, `audio.sink_term_hook` - rather than from a copy here, so the key IS the
    // declaration.
    REQUIRE(r.expectedCount == 4);
    CHECK(HasRow(r.expected, r.expectedCount, "2.3"));
    CHECK(HasRow(r.expected, r.expectedCount, "2.4"));
    CHECK(HasRow(r.expected, r.expectedCount, "2.5"));
    CHECK(HasRow(r.expected, r.expectedCount, "2.6"));

    SUBCASE("turning both rows off empties the denominator rather than passing it") {
        const char* none = "{\"capture.voip\": false, \"bootstrap.offline_bit_lock\": false, "
                           "\"ui.inframe_draw\": false, \"audio.sink_term_hook\": false}";
        REQUIRE(config::LoadFromTextForTest(none, std::strlen(none)));
        sc7::Sc7Result off = {};
        sc7::DeclaredRows(off);
        CHECK(off.expectedCount == 0);
        config::ResetForTest();
    }

    SUBCASE("each row leaves the denominator on its own key alone") {
        // Two keys, two rows: without this a single stuck row would still satisfy the count above.
        const char* noVoip = "{\"capture.voip\": false}";
        REQUIRE(config::LoadFromTextForTest(noVoip, std::strlen(noVoip)));
        sc7::Sc7Result a = {};
        sc7::DeclaredRows(a);
        CHECK_FALSE(HasRow(a.expected, a.expectedCount, "2.3"));
        CHECK(HasRow(a.expected, a.expectedCount, "2.4"));
        config::ResetForTest();

        const char* noLock = "{\"bootstrap.offline_bit_lock\": false}";
        REQUIRE(config::LoadFromTextForTest(noLock, std::strlen(noLock)));
        sc7::Sc7Result b = {};
        sc7::DeclaredRows(b);
        CHECK(HasRow(b.expected, b.expectedCount, "2.3"));
        CHECK_FALSE(HasRow(b.expected, b.expectedCount, "2.4"));
        CHECK(HasRow(b.expected, b.expectedCount, "2.5"));
        config::ResetForTest();

        const char* noDraw = "{\"ui.inframe_draw\": false, \"audio.sink_term_hook\": false}";
        REQUIRE(config::LoadFromTextForTest(noDraw, std::strlen(noDraw)));
        sc7::Sc7Result c = {};
        sc7::DeclaredRows(c);
        CHECK(HasRow(c.expected, c.expectedCount, "2.3"));
        CHECK(HasRow(c.expected, c.expectedCount, "2.4"));
        CHECK_FALSE(HasRow(c.expected, c.expectedCount, "2.5"));
        config::ResetForTest();
    }

    SUBCASE("the requested tick.mode is not the row's answer; the published one is") {
        // What the file asks for is the INPUT to the tick module's resolution, not the resolution. Row 1
        // follows the mode that is actually running, so the file's value alone declares nothing.
        const char* hook = "{\"tick.mode\": \"hook\"}";
        REQUIRE(config::LoadFromTextForTest(hook, std::strlen(hook)));
        sc7::Sc7Result requested = {};
        sc7::DeclaredRows(requested);
        CHECK_FALSE(HasRow(requested.expected, requested.expectedCount, "1"));
        CHECK(IsDeferred(requested, "1"));

        REQUIRE(config::SetEffectiveString("playback.host_netvar_hook", "off"));   // bootstrap, before any pass
        REQUIRE(config::SetEffectiveString("playback.host_score_hook", "off"));
        REQUIRE(config::SetEffectiveString("tick.mode", "hook"));
        sc7::Sc7Result withHook = {};
        sc7::DeclaredRows(withHook);
        CHECK(HasRow(withHook.expected, withHook.expectedCount, "1"));
        CHECK(HasRow(withHook.expected, withHook.expectedCount, "2.3"));
        CHECK(withHook.deferredCount == 0);
        config::ResetForTest();

        // The same publish the other way: `auto` resolved to the zero-hook slot claim, so row 1 is
        // ASSERTED absent rather than merely missing, and 2.3 is still there to prove the walk ran.
        const char* automatic = "{\"tick.mode\": \"auto\"}";
        REQUIRE(config::LoadFromTextForTest(automatic, std::strlen(automatic)));
        REQUIRE(config::SetEffectiveString("playback.host_netvar_hook", "off"));   // bootstrap, before any pass
        REQUIRE(config::SetEffectiveString("playback.host_score_hook", "off"));
        REQUIRE(config::SetEffectiveString("tick.mode", "slot"));
        sc7::Sc7Result withSlot = {};
        sc7::DeclaredRows(withSlot);
        CHECK_FALSE(HasRow(withSlot.expected, withSlot.expectedCount, "1"));
        CHECK(HasRow(withSlot.expected, withSlot.expectedCount, "2.3"));
        CHECK(withSlot.deferredCount == 0);
        config::ResetForTest();
    }

    SUBCASE("no Level-0 or Level-1X row ever enters the denominator") {
        // Level 0 touches no engine code and 1X patches this process, not echovr.exe's .text. Either one
        // in the denominator would be a row that can never be observed - a permanent FAIL.
        // Turned ON, so the exclusion has a live subject: `capture.trap` claims 1X.3 (the nine kernel32
        // detours) and is default-off, and a loop over a set that never contained a 1X row would pass
        // whatever the exclusion did.
        const char* trapOn = "{\"capture.trap\": true}";
        REQUIRE(config::LoadFromTextForTest(trapOn, std::strlen(trapOn)));
        sc7::Sc7Result all = {};
        sc7::DeclaredRows(all);
        REQUIRE(all.expectedCount > 0);
        for (uint32_t i = 0; i < all.expectedCount; ++i) {
            const std::string row = all.expected[i].id;
            CHECK(row.compare(0, 2, "0.") != 0);
            CHECK(row.compare(0, 2, "1X") != 0);
        }
        CHECK_FALSE(HasRow(all.expected, all.expectedCount, "1X.3"));
        CHECK_FALSE(IsDeferred(all, "1X.3"));
        config::ResetForTest();
    }
}

TEST_CASE("a Level-2 row that patches no .text stays out of the detour denominator") {
    // Row 2.5b (`ui.gate_scope`) opens the engine's GUI suppression gate through the engine's own
    // setters and restores a `.data` byte. It registers no hookmgr site, so it can never appear in
    // `declared_sites` - and while the denominator was chosen by the row's SPELLING it entered the
    // expected set anyway, which is why SC7 published expected_rows 4 against declared_sites 3 on
    // every launch that had the scope on. Both populations read the same declaration now.
    config::ResetForTest();
    const ConfigKey* scope = FindConfigKey("ui.gate_scope");
    const ConfigKey* draw = FindConfigKey("ui.inframe_draw");
    REQUIRE(scope != nullptr);
    REQUIRE(draw != nullptr);
    CHECK(std::strcmp(scope->ledgerRow, "2.5b") == 0);
    CHECK((scope->flags & kConfigTextDetour) == 0u);
    CHECK((draw->flags & kConfigTextDetour) != 0u);

    // The row is declared ON here, so its absence below is the mechanism talking rather than the
    // value: an exclusion that only held for a row nobody asked for would be vacuous.
    REQUIRE(config::RowDeclarationState("ui.gate_scope") == config::RowState::Declared);
    sc7::Sc7Result on = {};
    sc7::DeclaredRows(on);
    CHECK_FALSE(HasRow(on.expected, on.expectedCount, "2.5b"));
    CHECK_FALSE(IsDeferred(on, "2.5b"));
    CHECK(on.expectedCount == 4);

    SUBCASE("turning the row off moves nothing, which a value-driven exclusion could not manage") {
        const char* off = "{\"ui.gate_scope\": false}";
        REQUIRE(config::LoadFromTextForTest(off, std::strlen(off)));
        REQUIRE(config::RowDeclarationState("ui.gate_scope") == config::RowState::NotDeclared);
        sc7::Sc7Result cleared = {};
        sc7::DeclaredRows(cleared);
        CHECK(cleared.expectedCount == 4);
        CHECK(HasRow(cleared.expected, cleared.expectedCount, "2.5"));
        config::ResetForTest();
    }
}

TEST_CASE("a detour observed at a deferred row refuses; it never renders as a pass") {
    // AUDIT_H H-2. The deferred row is out of both denominators, which is right -- scoring it would
    // answer the question the pass just said it could not put. But a detour IS standing in .text at that
    // row's site, so `missing == 0 && extra == 0` is not a pass; it is could-not-ask. Decide is pure over
    // Sc7Result, so the state is built here rather than measured.
    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    std::snprintf(r.observed[0].id, sc7::kRowChars, "2.3");
    std::snprintf(r.observed[1].id, sc7::kRowChars, "1");
    r.observedCount = 2;
    std::snprintf(r.deferred[0].id, sc7::kRowChars, "1");
    std::snprintf(r.deferred[0].key, sizeof r.deferred[0].key, "tick.mode");
    std::snprintf(r.deferred[0].reason, sizeof r.deferred[0].reason, "tick-mode-not-yet-published");
    r.deferredCount = 1;

    sc7::Decide(r);

    CHECK(r.verdict == sc7::Verdict::CouldNotAsk);
    CHECK(r.refused);
    CHECK(std::string(r.reason).find("ledger-deferred-observed") != std::string::npos);
    // The count is in the reason, so a consumer can see how many rows were unanswerable.
    CHECK(std::string(r.reason).find("1 detour(s)") != std::string::npos);

    SUBCASE("the same shape WITHOUT the observed detour still passes, so the refusal is the detour's") {
        sc7::Sc7Result q = {};
        Declare(q, "2.3");
        std::snprintf(q.observed[0].id, sc7::kRowChars, "2.3");
        q.observedCount = 1;
        std::snprintf(q.deferred[0].id, sc7::kRowChars, "1");
        std::snprintf(q.deferred[0].key, sizeof q.deferred[0].key, "tick.mode");
        std::snprintf(q.deferred[0].reason, sizeof q.deferred[0].reason, "tick-mode-not-yet-published");
        q.deferredCount = 1;
        sc7::Decide(q);
        CHECK(q.verdict == sc7::Verdict::Pass);
        CHECK_FALSE(q.refused);
    }

    SUBCASE("a real failure elsewhere still outranks it -- FAIL is not softened into could-not-ask") {
        sc7::Sc7Result q = {};
        Declare(q, "2.3");
        Declare(q, "0.1");
        std::snprintf(q.observed[0].id, sc7::kRowChars, "2.3");
        std::snprintf(q.observed[1].id, sc7::kRowChars, "1");
        q.observedCount = 2;
        std::snprintf(q.deferred[0].id, sc7::kRowChars, "1");
        std::snprintf(q.deferred[0].key, sizeof q.deferred[0].key, "tick.mode");
        std::snprintf(q.deferred[0].reason, sizeof q.deferred[0].reason, "tick-mode-not-yet-published");
        q.deferredCount = 1;
        sc7::Decide(q);   // row 0.1 is declared and has no detour: missing == 1
        CHECK(q.verdict == sc7::Verdict::Fail);
    }
}

TEST_CASE("the written report names the failing range and survives a read-back") {
    Fixture fx;
    fx.PlantJump(0x800);
    fx.live[0x500] ^= 0xFF;

    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    const sc7::SiteView site = VoipSite();
    sc7::Compare(fx.View(), &site, 1, r);
    sc7::Decide(r);
    std::snprintf(r.pass, sizeof r.pass, "unit");
    std::snprintf(r.diskMd5, sizeof r.diskMd5, "caa05ee3bca2f5df0809994e93e2cc05");

    const std::wstring dir = TempDir();
    REQUIRE(sc7::WriteJson(dir.c_str(), r));
    const std::string json = ReadAll(dir + L"\\sc7_text_diff.json");

    CHECK(json.find("\"verdict\": \"FAIL\"") != std::string::npos);
    CHECK(json.find("\"writer\": \"sc7-p12\"") != std::string::npos);
    CHECK(json.find("\"expected_rows\": [\"2.3\"]") != std::string::npos);
    CHECK(json.find("\"observed_rows\": [\"2.3\"]") != std::string::npos);
    CHECK(json.find("0x1500") != std::string::npos);          // the planted range's RVA
    CHECK(json.find("\"unattributed_total\": 1") != std::string::npos);
    CHECK(json.find("caa05ee3bca2f5df0809994e93e2cc05") != std::string::npos);

    SUBCASE("a refusal is written as a file, because could-not-ask and never-asked are two rows") {
        sc7::Sc7Result refused = {};
        sc7::Decide(refused);
        std::snprintf(refused.pass, sizeof refused.pass, "unit");
        REQUIRE(sc7::WriteJson(dir.c_str(), refused));
        const std::string text = ReadAll(dir + L"\\sc7_text_diff.json");
        CHECK(text.find("\"refused\":") != std::string::npos);
        CHECK(text.find("\"verdict\": \"COULD-NOT-ASK\"") != std::string::npos);
        CHECK(text.find("expected_rows") == std::string::npos);
    }

    SUBCASE("a refusal omits expected_rows entirely - it does not manufacture an empty one") {
        // P12: the reader never hands its consumer a vacuous denominator. An empty set is a claim
        // ("nothing was declared") and a refusal is in no position to make it; the key is simply absent
        // and `refused` is what the harness reads first. The declaration still appears under
        // declared_rows, which is nobody's denominator, so the refusal remains auditable.
        sc7::Sc7Result refused = {};
        Declare(refused, "2.3");
        refused.refused = true;
        std::snprintf(refused.reason, sizeof refused.reason, "the mapped .text span is not readable");
        sc7::Decide(refused);
        std::snprintf(refused.pass, sizeof refused.pass, "unit");
        REQUIRE(sc7::WriteJson(dir.c_str(), refused));
        const std::string text = ReadAll(dir + L"\\sc7_text_diff.json");
        CHECK(text.find("expected_rows") == std::string::npos);
        CHECK(text.find("\"expected_rows\": []") == std::string::npos);
        CHECK(text.find("\"declared_rows\": [\"2.3\"]") != std::string::npos);
        CHECK(text.find("not readable") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------------------------------
// The run history: a FAIL is sticky for the life of the process
// ---------------------------------------------------------------------------------------------------

namespace {

sc7::Sc7Result FailingPass(const char* name) {
    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    std::snprintf(r.observed[r.observedCount++].id, sc7::kRowChars, "2.3");
    r.unattributedTotal = 1;
    r.unattributedCount = 1;
    r.unattributed[0].rva = kTextRva + 0x500;
    r.unattributed[0].len = 3;
    r.verdict = sc7::Verdict::Fail;
    std::snprintf(r.reason, sizeof r.reason, "1 undeclared range in .text");
    std::snprintf(r.pass, sizeof r.pass, "%s", name);
    return r;
}

sc7::Sc7Result RefusingPass(const char* name) {
    sc7::Sc7Result r = {};
    r.refused = true;
    std::snprintf(r.reason, sizeof r.reason, "the mapped .text span is not readable");
    sc7::Decide(r);
    std::snprintf(r.pass, sizeof r.pass, "%s", name);
    return r;
}

}  // namespace

TEST_CASE("a FAIL then a refusal still reports FAIL - a transient read failure is not innocence") {
    // Both passes write the SAME file. Post-init finds an undeclared patch; thirty seconds later the
    // read of echovr.exe transiently fails. If the refusal were written as-is, the harness would render
    // NOT-RUN and the launch that saw a patch would be recorded as never having asked.
    sc7::ResetForTest();
    sc7::Sc7Result first = FailingPass("post-init");
    sc7::RecordPass(first);
    REQUIRE(first.verdict == sc7::Verdict::Fail);

    sc7::Sc7Result second = RefusingPass("late-tick");
    sc7::RecordPass(second);

    CHECK(second.verdict == sc7::Verdict::Fail);
    CHECK_FALSE(second.refused);                       // or the harness renders NOT-RUN from `refused`
    CHECK(second.unattributedTotal == 1);              // the evidence rides along with the word
    CHECK(second.unattributed[0].rva == kTextRva + 0x500);
    CHECK(HasRow(second.expected, second.expectedCount, "2.3"));
    CHECK(std::string(second.reason).find("undeclared range") != std::string::npos);

    REQUIRE(second.passCount == 2);
    CHECK(second.passes[0].verdict == sc7::Verdict::Fail);
    CHECK(second.passes[1].verdict == sc7::Verdict::CouldNotAsk);
    CHECK(std::string(second.passes[1].reason).find("not readable") != std::string::npos);

    SUBCASE("and the file the harness reads says so, with the refusal alongside") {
        const std::wstring dir = TempDir();
        REQUIRE(sc7::WriteJson(dir.c_str(), second));
        const std::string text = ReadAll(dir + L"\\sc7_text_diff.json");
        CHECK(text.find("\"verdict\": \"FAIL\"") != std::string::npos);
        CHECK(text.find("\"refused\":") == std::string::npos);
        CHECK(text.find("\"expected_rows\": [\"2.3\"]") != std::string::npos);
        CHECK(text.find("\"verdict\": \"COULD-NOT-ASK\"") != std::string::npos);   // inside passes[]
    }
    sc7::ResetForTest();
}

TEST_CASE("a FAIL then a PASS still reports FAIL - a patch since removed was still installed") {
    sc7::ResetForTest();
    sc7::Sc7Result first = FailingPass("post-init");
    sc7::RecordPass(first);

    sc7::Sc7Result second = {};
    Declare(second, "2.3");
    std::snprintf(second.observed[second.observedCount++].id, sc7::kRowChars, "2.3");
    second.verdict = sc7::Verdict::Pass;
    std::snprintf(second.pass, sizeof second.pass, "late-tick");
    sc7::RecordPass(second);

    CHECK(second.verdict == sc7::Verdict::Fail);
    CHECK(second.unattributedTotal == 1);
    REQUIRE(second.passCount == 2);
    CHECK(second.passes[1].verdict == sc7::Verdict::Pass);
    sc7::ResetForTest();
}

TEST_CASE("a refusal that follows nothing is still a refusal") {
    // The stickiness must not manufacture a FAIL out of a launch that only ever refused.
    sc7::ResetForTest();
    sc7::Sc7Result only = RefusingPass("post-init");
    sc7::RecordPass(only);

    CHECK(only.verdict == sc7::Verdict::CouldNotAsk);
    CHECK(only.refused);
    REQUIRE(only.passCount == 1);
    sc7::ResetForTest();
}

TEST_CASE("one pass over a .text-sized span costs what the design budgeted for it") {
    // The subject's real size, not a round number: echovr.exe's .text section header reports
    // VirtualSize = 23 859 655 (22.75 MiB) on the pinned build, and VirtualSize is what the reader
    // compares because it is smaller than SizeOfRawData here. Measuring a 10 MiB fixture and publishing
    // the result as the cost of a pass understates it by 2.4x. The reader runs on the bootstrap worker,
    // never on the game tick, but "off the tick" is not a licence to be unbounded. This measures the
    // comparison half; the disk read is reported separately.
    constexpr size_t kTextSized = 23859655u;
    uint8_t* disk = static_cast<uint8_t*>(VirtualAlloc(nullptr, kTextSized * 2, MEM_COMMIT | MEM_RESERVE,
                                                       PAGE_READWRITE));
    REQUIRE(disk != nullptr);
    uint8_t* live = disk + kTextSized;
    for (size_t i = 0; i < kTextSized; ++i) {
        disk[i] = static_cast<uint8_t>((i * 31u + 7u) & 0xFF);
    }
    std::memcpy(live, disk, kTextSized);
    live[kTextSized / 2] ^= 0xFF;

    sc7::ImageView image = {};
    image.disk = disk;
    image.live = live;
    image.len = kTextSized;
    image.textRva = kTextRva;

    LARGE_INTEGER freq = {};
    LARGE_INTEGER started = {};
    LARGE_INTEGER finished = {};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&started);
    sc7::Sc7Result r = {};
    sc7::Compare(image, nullptr, 0, r);
    QueryPerformanceCounter(&finished);
    VirtualFree(disk, 0, MEM_RELEASE);

    const long long us = (finished.QuadPart - started.QuadPart) * 1000000 / freq.QuadPart;
    MESSAGE("22.75 MiB (echovr.exe .text VirtualSize) compare: " << us << " us");
    CHECK(r.unattributedTotal == 1);
    CHECK(us < 2000000);
}

TEST_CASE("the post-init pass cannot ask row 1, and asking must not be what breaks it") {
    // The ruling: tick.mode is resolved on the claim thread, so at post-init the effective value is not
    // published yet and row 1's outcome does not exist. SC7 says so instead of scoring it - and SC7's own
    // read must not be what refuses the publish it is waiting for.
    config::ResetForTest();
    // Row 2.8 is resolved by bootstrap before any module initialises, so it has answered by every pass.
    REQUIRE(config::SetEffectiveString("playback.host_netvar_hook", "off"));
    REQUIRE(config::SetEffectiveString("playback.host_score_hook", "off"));

    sc7::Sc7Result postInit = {};
    sc7::DeclaredRows(postInit);
    CHECK_FALSE(HasRow(postInit.expected, postInit.expectedCount, "1"));
    // TWO rows defer on an unpublished tick.mode: row 1 (tick.mode) and row 2.7 (pawn.reply_drain,
    // value-gated on the resolved tick mode -- DIAG-DRAIN-NOOP Q3).
    REQUIRE(postInit.deferredCount == 2);
    CHECK(std::string(postInit.deferred[0].id) == "1");
    CHECK(std::string(postInit.deferred[0].key) == "tick.mode");
    CHECK(std::string(postInit.deferred[0].reason) == "tick-mode-not-yet-published");
    CHECK(std::string(postInit.deferred[1].id) == "2.7");
    CHECK(std::string(postInit.deferred[1].key) == "pawn.reply_drain");
    // 2.3 is a bool row: it answers at every pass, so the deferral is row 1's and not a blanket refusal.
    CHECK(HasRow(postInit.expected, postInit.expectedCount, "2.3"));

    // ★ The whole point. A pass that has already walked every key must leave the window open.
    REQUIRE(config::SetEffectiveString("tick.mode", "slot"));

    sc7::Sc7Result lateTick = {};
    sc7::DeclaredRows(lateTick);
    CHECK(lateTick.deferredCount == 0);
    CHECK_FALSE(HasRow(lateTick.expected, lateTick.expectedCount, "1"));
    CHECK(HasRow(lateTick.expected, lateTick.expectedCount, "2.3"));
    config::ResetForTest();
}

TEST_CASE("row 2.8 is expected exactly when bootstrap resolved the setter detours to hook") {
    // The six CR15NetVarCS setter sites share the row, so the denominator moves by one row, not six.
    for (const char* resolved : {"hook", "off"}) {
        CAPTURE(resolved);
        config::ResetForTest();
        sc7::Sc7Result before = {};
        sc7::DeclaredRows(before);
        CHECK_FALSE(HasRow(before.expected, before.expectedCount, "2.8"));
        REQUIRE(before.deferredCount == 4);
        CHECK(std::string(before.deferred[2].id) == "2.8");
        CHECK(std::string(before.deferred[2].key) == "playback.host_netvar_hook");

        REQUIRE(config::SetEffectiveString("playback.host_netvar_hook", resolved));
        sc7::Sc7Result after = {};
        sc7::DeclaredRows(after);
        CHECK(after.deferredCount == 3);
        CHECK(HasRow(after.expected, after.expectedCount, "2.8") == (std::string(resolved) == "hook"));
        CHECK(after.expectedCount == before.expectedCount + (std::string(resolved) == "hook" ? 1u : 0u));
    }
    config::ResetForTest();
}

TEST_CASE("row 2.9 is expected exactly when bootstrap resolved the OnScoreCB detour to hook") {
    for (const char* resolved : {"hook", "off"}) {
        CAPTURE(resolved);
        config::ResetForTest();
        REQUIRE(config::SetEffectiveString("playback.host_netvar_hook", "off"));
        sc7::Sc7Result before = {};
        sc7::DeclaredRows(before);
        CHECK_FALSE(HasRow(before.expected, before.expectedCount, "2.9"));
        REQUIRE(before.deferredCount == 3);
        CHECK(std::string(before.deferred[2].id) == "2.9");
        CHECK(std::string(before.deferred[2].key) == "playback.host_score_hook");

        REQUIRE(config::SetEffectiveString("playback.host_score_hook", resolved));
        sc7::Sc7Result after = {};
        sc7::DeclaredRows(after);
        CHECK(after.deferredCount == 2);
        CHECK(HasRow(after.expected, after.expectedCount, "2.9") == (std::string(resolved) == "hook"));
        CHECK(HasRow(after.expected, after.expectedCount, "2.8") == false);
        CHECK(after.expectedCount == before.expectedCount + (std::string(resolved) == "hook" ? 1u : 0u));
    }
    config::ResetForTest();
}

TEST_CASE("a deferred row is scored on neither side, and the report names it") {
    config::ResetForTest();
    REQUIRE(config::SetEffectiveString("playback.host_netvar_hook", "off"));
    REQUIRE(config::SetEffectiveString("playback.host_score_hook", "off"));
    sc7::Sc7Result r = {};
    sc7::DeclaredRows(r);
    // Row 1 (tick.mode) and row 2.7 (pawn.reply_drain) both defer until tick.mode publishes -- Q3.
    REQUIRE(r.deferredCount == 2);

    SUBCASE("its detour standing in .text is neither an undeclared detour nor a pass") {
        // Two claims, and the second is AUDIT_H H-2. Without the exclusion this is FAIL "1 undeclared
        // detour(s)" - a confident answer to the exact question the pass just said it could not ask.
        // With the exclusion alone it was PASS, which is the same could-not-ask rendered as
        // asked-and-answered. It is neither: the row stays out of `extra`, and a detour standing at it
        // refuses.
        std::snprintf(r.observed[r.observedCount++].id, sc7::kRowChars, "1");
        std::snprintf(r.observed[r.observedCount++].id, sc7::kRowChars, "2.3");
        std::snprintf(r.observed[r.observedCount++].id, sc7::kRowChars, "2.4");
        std::snprintf(r.observed[r.observedCount++].id, sc7::kRowChars, "2.5");
        std::snprintf(r.observed[r.observedCount++].id, sc7::kRowChars, "2.6");
        sc7::Decide(r);
        CHECK(r.verdict == sc7::Verdict::CouldNotAsk);
        CHECK(r.refused);
        CHECK(std::string(r.reason).find("ledger-deferred-observed") == 0);
        CHECK(std::string(r.reason).find("undeclared") == std::string::npos);
    }

    SUBCASE("a denominator that is empty only because of the deferral is a refusal, not a FAIL") {
        const char* only = "{\"capture.voip\": false, \"bootstrap.offline_bit_lock\": false, "
                           "\"ui.inframe_draw\": false, \"audio.sink_term_hook\": false}";
        REQUIRE(config::LoadFromTextForTest(only, std::strlen(only)));
        config::SetEffectiveString("playback.host_netvar_hook", "off");   // refused if the load kept it
        config::SetEffectiveString("playback.host_score_hook", "off");
        sc7::Sc7Result off = {};
        sc7::DeclaredRows(off);
        REQUIRE(off.expectedCount == 0);
        // tick.mode and pawn.reply_drain both defer (the route is still on here, only the text-detour
        // bool rows were turned off), so the empty denominator is two deferrals, not one -- Q3.
        REQUIRE(off.deferredCount == 2);
        off.armedLedgerSites = 1;
        sc7::Decide(off);
        CHECK(off.verdict == sc7::Verdict::CouldNotAsk);
        CHECK(std::string(off.reason).find("ledger-deferred") == 0);
    }

    SUBCASE("the JSON carries the row, the key and the reason") {
        sc7::Decide(r);
        std::snprintf(r.pass, sizeof r.pass, "post-init");
        const std::wstring dir = TempDir();
        REQUIRE(sc7::WriteJson(dir.c_str(), r));
        const std::string json = ReadAll(dir + L"\\sc7_text_diff.json");
        CHECK(json.find("\"deferred_rows\": [") != std::string::npos);
        CHECK(json.find("\"row\": \"1\", \"key\": \"tick.mode\", "
                        "\"reason\": \"tick-mode-not-yet-published\"") != std::string::npos);
        // Excluded from both denominators, not quietly added to one of them.
        CHECK(json.find("\"declared_rows\": [\"2.6\", \"2.3\", \"2.5\", \"2.4\"]") != std::string::npos);
        CHECK(json.find("\"expected_rows\": [\"2.6\", \"2.3\", \"2.5\", \"2.4\"]") != std::string::npos);
    }
    config::ResetForTest();
}

// ---------------------------------------------------------------------------------------------------
// The third-party ledger
//
// L5 (2026-09-02, runs/20260902-071934Z/launch-01) was the first launch that ever ran this reader in a
// real process. Our own row matched; thirteen ranges were left over, and every fixture below takes its
// RVAs, lengths and bytes verbatim from that log. Nothing here is computed by the code under test.
//
// ★ The rlay spans are what `ScanRlayPatchTable` reads out of the shipped 191,488-byte rlay.dll
// (2024-02-03, plugins\rlay.dll): twelve `add rbx,<rva>` byte patches and twelve `add rax,<rva>` detour
// targets. Twelve of L5's thirteen ranges fall inside them. THE THIRTEENTH, 0x1042B0, DOES NOT: that
// RVA does not occur anywhere in rlay.dll, in any encoding, so this reader must not call it rlay's.
// ---------------------------------------------------------------------------------------------------

namespace {

struct LiveRange {
    uint64_t rva;
    uint32_t len;
    const char* after;
};

// L5-767b9b6.log lines 187-199.
const LiveRange kL5[] = {
    {0x000EAEF0, 7, "\xE9\x63\x54\xFF\xBF\xCC\xCC"},
    {0x000FDE0F, 4, "\xCD\x02\x00\x00"},
    {0x000FEA00, 5, "\xE9\x73\x17\xFE\xBF"},
    {0x001042B0, 5, "\xE9\x1F\xCD\xEE\xFF"},
    {0x00116720, 8, "\xE9\xB3\x9A\xFC\xBF\xCC\xCC\xCC"},
    {0x0011690D, 1, "\xEB"},
    {0x00179EB0, 5, "\xE9\xE3\x63\xF6\xBF"},
    {0x0017F0B1, 1, "\x75"},
    {0x001AC83E, 3, "\xE9\x92\x00"},
    {0x001B8650, 5, "\xE9\xE3\x7B\xF2\xBF"},
    {0x001F60C0, 5, "\xE9\x33\xA2\xEE\xBF"},
    {0x005105F0, 5, "\xE9\xC3\xFD\xBC\xBF"},
    {0x00A7C686, 2, "\xD6\x17"},
};

// The one L5 range rlay.dll does not declare.
constexpr uint64_t kOrphan = 0x001042B0;

// rlay.dll's own declaration, as ScanRlayPatchTable reads it from the installed build.
sc7::ThirdPartySet RlaySet(bool present, bool tableRead = true) {
    static const sc7::ThirdPartyPatch spans[] = {
        {0x000D30D0, 8}, {0x000D31B0, 8}, {0x000EAEF0, 8}, {0x000EBE70, 8}, {0x000FDE0E, 5},
        {0x000FEA00, 8}, {0x000FFA58, 4}, {0x000FFB0E, 2}, {0x00116720, 8}, {0x0011690D, 2},
        {0x00116F3D, 6}, {0x001580C3, 40}, {0x00179EB0, 8}, {0x0017F0B1, 2}, {0x0017F77B, 2},
        {0x0017F817, 2}, {0x0017F823, 2}, {0x001A89F0, 8}, {0x001AC83E, 6}, {0x001B8650, 8},
        {0x001F60C0, 8}, {0x005105F0, 8}, {0x005FE290, 8}, {0x00A7C685, 5},
    };
    sc7::ThirdPartySet set = {};
    std::snprintf(set.name, sizeof set.name, "rlay");
    std::snprintf(set.provenance, sizeof set.provenance, "24 span(s) read from rlay.dll's own code");
    set.present = present;
    set.tableRead = tableRead;
    if (tableRead) {
        set.patchCount = static_cast<uint32_t>(_countof(spans));
        std::memcpy(set.patches, spans, sizeof spans);
    }
    return set;
}

void Plant(sc7::Sc7Result& r, uint64_t rva, uint32_t len, const char* after) {
    sc7::Range& range = r.unattributed[r.unattributedCount++];
    std::memset(&range, 0, sizeof range);
    range.rva = rva;
    range.len = len;
    range.sampled = len < sc7::kSampleBytes ? len : sc7::kSampleBytes;
    if (after != nullptr) {
        std::memcpy(range.after, after, range.sampled);
    }
    ++r.unattributedTotal;
}

// Plants L5's thirteen, or the twelve rlay accounts for.
void PlantL5(sc7::Sc7Result& r, bool includeOrphan) {
    for (size_t i = 0; i < _countof(kL5); ++i) {
        if (!includeOrphan && kL5[i].rva == kOrphan) {
            continue;
        }
        Plant(r, kL5[i].rva, kL5[i].len, kL5[i].after);
    }
}

void Observe(sc7::Sc7Result& r, const char* row) {
    std::snprintf(r.observed[r.observedCount].id, sc7::kRowChars, "%s", row);
    ++r.observedCount;
}

const sc7::Range* RangeAt(const sc7::Sc7Result& r, uint64_t rva) {
    for (uint32_t i = 0; i < r.unattributedCount; ++i) {
        if (r.unattributed[i].rva == rva) {
            return &r.unattributed[i];
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("a byte-patch site is read out of rlay.dll's own instructions") {
    // Verbatim from rlay.dll file offset 0xF82: mov edx,5 ; mov [rbp-2Ch],0002CDE8h ; add rbx,0FDE0Eh ;
    // mov [rbp-28h],0 ; mov rcx,rbx ; call [WriteBytes]. The site rlay implements -offline with.
    const uint8_t code[] = {
        0x1D, 0x89, 0xA2, 0x02, 0x00, 0x4C, 0x8D, 0x4D, 0xC0, 0xBA, 0x05, 0x00, 0x00, 0x00, 0xC7, 0x45,
        0xD4, 0xE8, 0xCD, 0x02, 0x00, 0x48, 0x81, 0xC3, 0x0E, 0xDE, 0x0F, 0x00, 0xC6, 0x45, 0xD8, 0x00,
        0x48, 0x8B, 0xCB, 0x44, 0x8D, 0x42, 0x3B, 0xFF, 0x15, 0x59, 0xC4, 0x01, 0x00, 0x33, 0xFF, 0x85,
    };
    sc7::ThirdPartyPatch out[8] = {};
    uint32_t dropped = 1;
    const size_t n = sc7::ScanRlayPatchTable(code, sizeof code, 0xC00000, out, _countof(out), &dropped);
    REQUIRE(n == 1);
    CHECK(out[0].rva == 0xFDE0E);
    CHECK(out[0].len == 5);
    CHECK(dropped == 0);
}

TEST_CASE("a detour target is read out of rlay.dll's own instructions") {
    // Verbatim from rlay.dll file offset 0x440: mov rax,[base] ; add rax,0FEA00h ; mov [g_target],rax.
    const uint8_t code[] = {
        0x48, 0x8B, 0x05, 0xC9, 0xAD, 0x02, 0x00, 0x48, 0x05, 0x00, 0xEA, 0x0F, 0x00, 0x48, 0x89, 0x05,
        0x94, 0xAD, 0x02, 0x00, 0xC3, 0xCC, 0xCC, 0xCC,
    };
    sc7::ThirdPartyPatch out[8] = {};
    const size_t n = sc7::ScanRlayPatchTable(code, sizeof code, 0xC00000, out, _countof(out), nullptr);
    REQUIRE(n == 1);
    CHECK(out[0].rva == 0xFEA00);
    CHECK(out[0].len == 8);
}

TEST_CASE("an address that never reaches rlay's writer is not a declared patch") {
    uint8_t code[] = {
        0x1D, 0x89, 0xA2, 0x02, 0x00, 0x4C, 0x8D, 0x4D, 0xC0, 0xBA, 0x05, 0x00, 0x00, 0x00, 0xC7, 0x45,
        0xD4, 0xE8, 0xCD, 0x02, 0x00, 0x48, 0x81, 0xC3, 0x0E, 0xDE, 0x0F, 0x00, 0xC6, 0x45, 0xD8, 0x00,
        0x48, 0x8B, 0xCB, 0x44, 0x8D, 0x42, 0x3B, 0xFF, 0x15, 0x59, 0xC4, 0x01, 0x00, 0x33, 0xFF, 0x85,
    };
    sc7::ThirdPartyPatch out[8] = {};
    code[33] = 0x90;   // the mov rcx,rbx that hands the address over
    CHECK(sc7::ScanRlayPatchTable(code, sizeof code, 0xC00000, out, _countof(out), nullptr) == 0);
    // ...and a constant that cannot be an RVA inside the span SC7 compares is not one either.
    code[33] = 0x8B;
    CHECK(sc7::ScanRlayPatchTable(code, sizeof code, 0x1000, out, _countof(out), nullptr) == 0);
}

TEST_CASE("the twelve ranges rlay declares are attributed to it, and L5's thirteenth is named") {
    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    Observe(r, "2.3");
    PlantL5(r, true);
    const sc7::ThirdPartySet rlay = RlaySet(true);
    sc7::Attribute(r, &rlay, 1);
    sc7::Decide(r);

    CHECK(r.attributedRanges == 12);
    CHECK(r.thirdParty[0].observed == 12);
    CHECK(r.thirdParty[0].declared == 24);
    CHECK(r.unownedTotal == 1);
    // The whole point of the exercise: a launch may not be red forever because a coexisting mod patched
    // the game - and may not be green because one did.
    CHECK(r.verdict == sc7::Verdict::Fail);
    CHECK(std::string(r.reason).find("no third party accounts for") != std::string::npos);
    const sc7::Range* orphan = RangeAt(r, kOrphan);
    REQUIRE(orphan != nullptr);
    CHECK(std::string(orphan->owner).empty());
    const sc7::Range* claimed = RangeAt(r, 0x000EAEF0);
    REQUIRE(claimed != nullptr);
    CHECK(std::string(claimed->owner) == "rlay");
}

TEST_CASE("L5's record shape passes once row 2.4 claims the offline lock's detour") {
    // The retroactive proof for DESIGN S4.4 row 2.4 (commit 80998d4), against the artifact it was
    // written for: L5's thirteen ranges, twelve rlay's and 0x1042B0 ours. The lock's site is REAL here -
    // the fixture's .text is rebased so offset 0x2B0 IS RVA 0x1042B0, and the five bytes on each side are
    // the ones L5 measured - so Compare has to subtract it rather than a stand-in.
    Fixture fx;
    std::memcpy(fx.disk + 0x2B0, "\x40\x53\x48\x83\xEC", 5);
    std::memcpy(fx.live + 0x2B0, "\xE9\x1F\xCD\xEE\xFF", 5);
    fx.PlantJump(0x800);
    sc7::ImageView image = fx.View();
    image.textRva = 0x00104000;

    sc7::SiteView sites[2] = {};
    sites[0].factId = "VOIP.RECV";
    sites[0].row = "2.3";
    sites[0].rva = 0x00104800;
    sites[0].armed = true;
    // What GatherSites publishes from `RowForKey("bootstrap.offline_bit_lock")`.
    sites[1].factId = "OFFLINELOCK.TEARDOWN";
    sites[1].row = "2.4";
    sites[1].rva = 0x001042B0;
    sites[1].armed = true;

    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    Declare(r, "2.4");
    sc7::Compare(image, sites, 2, r);
    CHECK(r.unattributedTotal == 0);
    CHECK(HasRow(r.observed, r.observedCount, "2.3"));
    CHECK(HasRow(r.observed, r.observedCount, "2.4"));

    PlantL5(r, false);   // the twelve rlay wrote, which the lock's row says nothing about
    const sc7::ThirdPartySet rlay = RlaySet(true);
    sc7::Attribute(r, &rlay, 1);
    sc7::Decide(r);

    CHECK(r.verdict == sc7::Verdict::Pass);
    CHECK(r.unownedTotal == 0);
    CHECK(r.attributedRanges == 12);
    CHECK(r.thirdParty[0].observed == 12);
    CHECK(r.thirdParty[0].declared == 24);
    CHECK(RangeAt(r, kOrphan) == nullptr);

    const std::wstring dir = TempDir();
    std::snprintf(r.pass, sizeof r.pass, "post-init");
    REQUIRE(sc7::WriteJson(dir.c_str(), r));
    const std::string passed = ReadAll(dir + L"\\sc7_text_diff.json");
    CHECK(passed.find("\"expected_rows\": [\"2.3\", \"2.4\"]") != std::string::npos);
    CHECK(passed.find("\"unowned_total\": 0") != std::string::npos);
    // It appears in `detail[]` as the site row 2.4 accounts for, and in no unattributed range.
    CHECK(passed.find("0x1042B0") != std::string::npos);
    CHECK(passed.find("\"rva\": \"0x1042B0\", \"len\": 5") == std::string::npos);

    SUBCASE("without the row, the same bytes are the FAIL L5 actually got") {
        // The control that makes the case above mean something: strip the lock's site - which is what
        // `bootstrap.offline_bit_lock` claiming no row produced - and 0x1042B0 is undeclared again.
        // Decide's reason carries counts, so the RVA is named where a reader meets it: the record.
        sc7::Sc7Result before = {};
        Declare(before, "2.3");
        sc7::Compare(image, sites, 1, before);
        PlantL5(before, false);
        sc7::Attribute(before, &rlay, 1);
        sc7::Decide(before);

        CHECK(before.verdict == sc7::Verdict::Fail);
        CHECK(before.unownedTotal == 1);
        const sc7::Range* orphan = RangeAt(before, kOrphan);
        REQUIRE(orphan != nullptr);
        CHECK(std::string(orphan->owner).empty());

        std::snprintf(before.pass, sizeof before.pass, "post-init");
        REQUIRE(sc7::WriteJson(dir.c_str(), before));
        const std::string failed = ReadAll(dir + L"\\sc7_text_diff.json");
        CHECK(failed.find("\"rva\": \"0x1042B0\", \"len\": 5") != std::string::npos);
        CHECK(failed.find("\"unowned_total\": 1") != std::string::npos);
    }
}

TEST_CASE("rlay's own ranges pass, and the report still shows every one of them") {
    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    Observe(r, "2.3");
    PlantL5(r, false);
    const sc7::ThirdPartySet rlay = RlaySet(true);
    sc7::Attribute(r, &rlay, 1);
    sc7::Decide(r);

    CHECK(r.verdict == sc7::Verdict::Pass);
    CHECK(r.unownedTotal == 0);
    CHECK(r.attributedRanges == 12);
    CHECK(std::string(r.reason).find("12 range(s) attributed to rlay (loaded)") != std::string::npos);
    // Attributed is not deleted: the ranges are all still in the record, each carrying its owner.
    REQUIRE(r.unattributedCount == 12);
    for (uint32_t i = 0; i < r.unattributedCount; ++i) {
        CHECK(std::string(r.unattributed[i].owner) == "rlay");
    }
}

TEST_CASE("one range no third party declares fails the pass and is named") {
    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    Observe(r, "2.3");
    PlantL5(r, false);
    Plant(r, 0x00300000, 5, "\xE9\x01\x02\x03\x04");
    const sc7::ThirdPartySet rlay = RlaySet(true);
    sc7::Attribute(r, &rlay, 1);
    sc7::Decide(r);

    CHECK(r.verdict == sc7::Verdict::Fail);
    CHECK(r.unownedTotal == 1);
    const sc7::Range* extra = RangeAt(r, 0x00300000);
    REQUIRE(extra != nullptr);
    CHECK(std::string(extra->owner).empty());
}

TEST_CASE("rlay's ranges with rlay absent are somebody else's patch, and it says whose bytes") {
    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    Observe(r, "2.3");
    PlantL5(r, false);
    const sc7::ThirdPartySet rlay = RlaySet(false);
    sc7::Attribute(r, &rlay, 1);
    sc7::Decide(r);

    CHECK(r.verdict == sc7::Verdict::Fail);
    CHECK(r.attributedRanges == 0);
    CHECK(r.thirdParty[0].observed == 0);
    CHECK(r.thirdParty[0].unmatchedCount == 12);
    const std::string why(r.reason);
    CHECK(why.find("third-party-absent") != std::string::npos);
    CHECK(why.find("0xEAEF0") != std::string::npos);
}

TEST_CASE("attribution is containment, so a range that spills past a declared span is not excused") {
    // rlay declares 8 bytes at the detour target 0xFEA00 and 2 at 0x17F0B1. A range that starts before
    // one, or runs past the end of one, is a different patch and is reported as one. This case is what
    // turns red if the match is loosened from containment to overlap - or to anything at all.
    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    Observe(r, "2.3");
    Plant(r, 0x000FEA00, 9, nullptr);
    Plant(r, 0x000FE9FF, 4, nullptr);
    Plant(r, 0x0017F0B2, 2, nullptr);
    const sc7::ThirdPartySet rlay = RlaySet(true);
    sc7::Attribute(r, &rlay, 1);
    sc7::Decide(r);

    CHECK(r.attributedRanges == 0);
    CHECK(r.unownedTotal == 3);
    CHECK(r.verdict == sc7::Verdict::Fail);
}

TEST_CASE("a range the cap dropped is unowned, because a cap is not an attribution") {
    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    Observe(r, "2.3");
    PlantL5(r, false);
    r.unattributedTotal += 3;   // three more were found than the record could hold
    const sc7::ThirdPartySet rlay = RlaySet(true);
    sc7::Attribute(r, &rlay, 1);
    sc7::Decide(r);

    CHECK(r.attributedRanges == 12);
    CHECK(r.unownedTotal == 3);
    CHECK(r.verdict == sc7::Verdict::Fail);
}

TEST_CASE("a loaded third party whose declaration did not parse cannot answer, so the pass refuses") {
    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    Observe(r, "2.3");
    Plant(r, 0x000FEA00, 5, nullptr);
    const sc7::ThirdPartySet rlay = RlaySet(true, false);
    sc7::Attribute(r, &rlay, 1);
    sc7::Decide(r);

    CHECK(r.verdict == sc7::Verdict::CouldNotAsk);
    CHECK(r.refused);
    CHECK(std::string(r.reason).find("third-party-unreadable") != std::string::npos);
}

TEST_CASE("the report carries the party ledger and every range's owner") {
    sc7::ResetForTest();
    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    Observe(r, "2.3");
    PlantL5(r, true);
    const sc7::ThirdPartySet rlay = RlaySet(true);
    sc7::Attribute(r, &rlay, 1);
    sc7::Decide(r);
    std::snprintf(r.pass, sizeof r.pass, "post-init");

    const std::wstring dir = TempDir();
    REQUIRE(sc7::WriteJson(dir.c_str(), r));
    const std::string json = ReadAll(dir + L"\\sc7_text_diff.json");
    CHECK(json.find("\"name\": \"rlay\"") != std::string::npos);
    CHECK(json.find("\"present\": true") != std::string::npos);
    CHECK(json.find("\"declared\": 24") != std::string::npos);
    CHECK(json.find("\"observed\": 12") != std::string::npos);
    CHECK(json.find("\"unowned_total\": 1") != std::string::npos);
    CHECK(json.find("\"attributed_ranges\": 12") != std::string::npos);
    CHECK(json.find("\"owner\": \"rlay\"") != std::string::npos);
    // The orphan is in the document, named, owned by nobody.
    CHECK(json.find("\"rva\": \"0x1042B0\", \"len\": 5") != std::string::npos);
    CHECK(json.find("\"owner\": \"\"") != std::string::npos);
    sc7::ResetForTest();
}

// ---------------------------------------------------------------------------------------------------
// JSON escaping. L5 (`20260902-081617Z`) wrote a `sc7_text_diff.json` no parser would read: the module
// path went into the document raw, so `\P` sat at offset 2156. The quiet half is worse - `\r` in
// `ready-at-dawn` and `\b` in `bin` are VALID escapes, so a lenient reader would have handed its
// consumer a corrupted path instead of refusing the file.
//
// These cases assert the escaped bytes are in the document AND that the raw bytes are not: the second
// half is what turns red if any emitter ever puts a `%s` back inside a JSON string. tests/sc7/check.ps1
// takes the same two artifacts to a real parser and reads the paths back.
// ---------------------------------------------------------------------------------------------------

namespace {

// A per-case artifact directory, because WriteJson always writes the same filename.
std::wstring ArtifactDir(const wchar_t* leaf) {
    std::wstring dir = TempDir();
    dir += L"\\";
    dir += leaf;
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// Recovered verbatim from the broken L5 artifact, so this is the recorded input and not a plausible
// re-typing of it.
constexpr const char* kL5Provenance =
    "25 span(s) read from C:\\Program Files\\Oculus\\Software\\Software\\ready-at-dawn-echo-arena"
    "\\bin\\win10\\plugins\\rlay.dll's own patch-install code";

// Every class the policy names, in one path: an invalid escape (`\P`), two valid ones that corrupt
// silently (`\r`, `\b`), a quote, a tab, a control byte with no short form, and a UTF-8 e-acute.
constexpr const char* kHostileProvenance =
    "C:\\Program Files\\ready-at-dawn\\bin\\say \"hi\"\ta\x01" "b caf\xc3\xa9";

// The scan that refuses L5's document: every backslash either begins a legal escape or is the
// second half of one. An escaped backslash followed by P is legal; the lone P escape L5 emitted
// is not, and this is the assertion that reds if any value goes in raw again.
bool EveryEscapeIsLegal(const std::string& s, size_t* badAt) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\') {
            continue;
        }
        if (i + 1 >= s.size() || std::strchr("\"\\/bfnrtu", s[i + 1]) == nullptr) {
            *badAt = i;
            return false;
        }
        ++i;
    }
    return true;
}

}  // namespace

TEST_CASE("every escape class goes through the one routine") {
    char buf[256];
    const char* in = "a\\b\"c\td\ne\rf\bg\fh\x01" "i\x7f" "\xc3\xa9";
    const size_t n = json::AppendEscaped(buf, sizeof buf, 0, in);
    CHECK(std::string(buf, n) ==
          "a\\\\b\\\"c\\td\\ne\\rf\\bg\\fh\\u0001i\x7f\xc3\xa9");
    CHECK(buf[n] == '\0');
}

TEST_CASE("a value that does not fit reports saturation instead of writing a prefix") {
    char buf[8];
    std::memset(buf, 'Z', sizeof buf);
    // The six-char  form and the terminator own the last byte, so at 1 it exactly fits.
    CHECK(json::AppendEscaped(buf, sizeof buf, 1, "\x01") == 7);
    CHECK(std::string(buf + 1) == "\\u0001");
    // At 2 it does not fit. AUDIT_L5FIX W1C-2: the answer is `cap`, not a shorter cursor - a caller
    // that cannot tell "wrote it all" from "wrote some of it" writes a truncated document and calls
    // it a report.
    std::memset(buf, 'Z', sizeof buf);
    CHECK(json::AppendEscaped(buf, sizeof buf, 2, "\x01") == sizeof buf);
    CHECK(buf[2] == '\0');   // no partial escape left behind either
    // And saturation sticks: every later append is a no-op, so the signal survives to the end of the
    // document rather than being cleared by the next field that happens to fit.
    CHECK(json::AppendEscaped(buf, sizeof buf, sizeof buf, "a") == sizeof buf);
}

TEST_CASE("an ill-formed UTF-8 sequence is replaced, not passed through") {
    char buf[256];
    // Well-formed first: two-, three- and four-byte characters survive byte-exact.
    const char* good = "\xc3\xa9" "\xe2\x82\xac" "\xf0\x9f\x92\xa9";
    CHECK(std::string(buf, json::AppendEscaped(buf, sizeof buf, 0, good)) == good);
    // AUDIT_L5FIX W1C-3. \xff and \xfe are not lead bytes at all; \xc3 at the end is a truncated
    // sequence. One U+FFFD per maximal subpart, and the document stays valid whatever the input was.
    CHECK(std::string(buf, json::AppendEscaped(buf, sizeof buf, 0, "a\xff\xfe" "b\xc3")) ==
          "a\\ufffd\\ufffdb\\ufffd");
    // Overlong, surrogate and out-of-range forms are ill-formed even though their lead byte is legal.
    CHECK(std::string(buf, json::AppendEscaped(buf, sizeof buf, 0, "\xc0\xaf")) == "\\ufffd\\ufffd");
    CHECK(std::string(buf, json::AppendEscaped(buf, sizeof buf, 0, "\xed\xa0\x80")) ==
          "\\ufffd\\ufffd\\ufffd");
    CHECK(std::string(buf, json::AppendEscaped(buf, sizeof buf, 0, "\xf4\x90\x80\x80")) ==
          "\\ufffd\\ufffd\\ufffd\\ufffd");
    // A three-byte character missing its last continuation is ONE ruined character, not two.
    CHECK(std::string(buf, json::AppendEscaped(buf, sizeof buf, 0, "\xe2\x82" "x")) == "\\ufffdx");
}

TEST_CASE("an embedded NUL is a control character when the length says so") {
    char buf[64];
    const char in[] = "a\0b";
    // AUDIT_L5FIX W1C-4. The const char* form stops at the NUL - that is what the type means, and the
    // caller chose it - so a caller holding a byte range must say how many bytes it holds.
    CHECK(std::string(buf, json::AppendEscaped(buf, sizeof buf, 0, in)) == "a");
    CHECK(std::string(buf, json::AppendEscapedN(buf, sizeof buf, 0, in, sizeof in - 1)) ==
          "a\\u0000b");
}

TEST_CASE("the L5 provenance path is escaped in the document it broke") {
    sc7::ResetForTest();
    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    Observe(r, "2.3");
    PlantL5(r, true);
    sc7::ThirdPartySet rlay = RlaySet(true);
    std::snprintf(rlay.provenance, sizeof rlay.provenance, "%s", kL5Provenance);
    REQUIRE(std::string(rlay.provenance) == kL5Provenance);   // not silently truncated by the field
    sc7::Attribute(r, &rlay, 1);
    sc7::Decide(r);
    std::snprintf(r.pass, sizeof r.pass, "late-tick");

    const std::wstring dir = ArtifactDir(L"jsonesc-l5");
    REQUIRE(sc7::WriteJson(dir.c_str(), r));
    const std::string json = ReadAll(dir + L"\\sc7_text_diff.json");

    CHECK(json.find("C:\\\\Program Files\\\\Oculus") != std::string::npos);
    CHECK(json.find("ready-at-dawn-echo-arena\\\\bin\\\\win10\\\\plugins\\\\rlay.dll") !=
          std::string::npos);
    // The bytes that broke L5. A single backslash before P, r or b anywhere in the document means an
    // emitter is interpolating raw again.
    CHECK(json.find("C:\\Program") == std::string::npos);
    size_t badAt = 0;
    CHECK_MESSAGE(EveryEscapeIsLegal(json, &badAt), "invalid escape at offset ", badAt);
    CHECK(json.find("dawn\\bin") == std::string::npos);
    sc7::ResetForTest();
}

TEST_CASE("a provenance path holding every hostile class survives the document") {
    sc7::ResetForTest();
    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    Observe(r, "2.3");
    PlantL5(r, true);
    sc7::ThirdPartySet rlay = RlaySet(true);
    std::snprintf(rlay.provenance, sizeof rlay.provenance, "%s", kHostileProvenance);
    REQUIRE(std::string(rlay.provenance) == kHostileProvenance);
    sc7::Attribute(r, &rlay, 1);
    sc7::Decide(r);
    // The reason and the pass name are free text too, and they are written by the same emitter.
    std::snprintf(r.pass, sizeof r.pass, "late-tick");
    std::snprintf(r.reason, sizeof r.reason, "%s", "quote \" tab \t bell \x07 done");

    const std::wstring dir = ArtifactDir(L"jsonesc-hostile");
    REQUIRE(sc7::WriteJson(dir.c_str(), r));
    const std::string json = ReadAll(dir + L"\\sc7_text_diff.json");

    CHECK(json.find("say \\\"hi\\\"\\ta\\u0001b caf\xc3\xa9") != std::string::npos);
    CHECK(json.find("quote \\\" tab \\t bell \\u0007 done") != std::string::npos);
    // A literal tab or quote inside a value ends the string for every parser there is.
    CHECK(json.find("say \"hi\"") == std::string::npos);
    size_t badAt = 0;
    CHECK_MESSAGE(EveryEscapeIsLegal(json, &badAt), "invalid escape at offset ", badAt);
    CHECK(json.find("a\x01" "b") == std::string::npos);
    sc7::ResetForTest();
}

namespace {

// Every array at its cap and every free-text field full. `byte` is what the text is made of: 0x01
// escapes to six characters, plain ASCII to one, which is the whole difference between a record that
// fits and one that does not.
void FillToCapacity(sc7::Sc7Result& r, char byte) {
    const auto fill = [byte](char* dst, size_t cap) {
        std::memset(dst, byte, cap - 1);
        dst[cap - 1] = '\0';
    };
    r.verdict = sc7::Verdict::Fail;
    fill(r.reason, sizeof r.reason);
    std::snprintf(r.pass, sizeof r.pass, "late-tick");
    fill(r.probeDirFallback, sizeof r.probeDirFallback);
    std::snprintf(r.diskMd5, sizeof r.diskMd5, "%s", "0123456789abcdef0123456789abcdef");

    r.expectedCount = r.observedCount = r.deferredCount = r.detailCount = sc7::kMaxRows;
    for (size_t i = 0; i < sc7::kMaxRows; ++i) {
        fill(r.expected[i].id, sizeof r.expected[i].id);
        fill(r.observed[i].id, sizeof r.observed[i].id);
        fill(r.deferred[i].id, sizeof r.deferred[i].id);
        fill(r.deferred[i].key, sizeof r.deferred[i].key);
        fill(r.deferred[i].reason, sizeof r.deferred[i].reason);
        fill(r.detail[i].row, sizeof r.detail[i].row);
        fill(r.detail[i].factId, sizeof r.detail[i].factId);
        r.detail[i].rva = 0x1400ABCDEull;
        r.detail[i].len = 12;
        r.detail[i].sampled = sc7::kSampleBytes;
        std::memset(r.detail[i].after, 0xCC, sizeof r.detail[i].after);
    }
    r.unattributedCount = sc7::kMaxRanges;
    r.unattributedTotal = sc7::kMaxRanges;
    for (size_t i = 0; i < sc7::kMaxRanges; ++i) {
        r.unattributed[i].rva = 0x1400ABCDEull + i;
        r.unattributed[i].len = 12;
        r.unattributed[i].sampled = sc7::kSampleBytes;
        std::memset(r.unattributed[i].before, 0xAA, sizeof r.unattributed[i].before);
        std::memset(r.unattributed[i].after, 0xBB, sizeof r.unattributed[i].after);
        fill(r.unattributed[i].owner, sizeof r.unattributed[i].owner);
    }
    r.thirdPartyCount = sc7::kMaxParties;
    for (size_t i = 0; i < sc7::kMaxParties; ++i) {
        fill(r.thirdParty[i].name, sizeof r.thirdParty[i].name);
        fill(r.thirdParty[i].provenance, sizeof r.thirdParty[i].provenance);
        r.thirdParty[i].present = true;
        r.thirdParty[i].tableRead = true;
        r.thirdParty[i].unmatchedCount = sc7::kMaxRanges;
        for (size_t k = 0; k < sc7::kMaxRanges; ++k) {
            r.thirdParty[i].unmatched[k] = 0x1400ABCDEull + k;
        }
    }
    r.passCount = sc7::kMaxPasses;
    for (size_t i = 0; i < sc7::kMaxPasses; ++i) {
        fill(r.passes[i].pass, sizeof r.passes[i].pass);
        fill(r.passes[i].reason, sizeof r.passes[i].reason);
        r.passes[i].verdict = sc7::Verdict::Fail;
    }
}

}  // namespace

// AUDIT_L5FIX W1C-2, the attack the auditor built: a maximal record used to be written CLAMPED to
// 32767 bytes with WriteJson returning true, which is L5's failure class exactly - a document no
// parser will read, produced by the reader whose job is to stop producing them.
TEST_CASE("a report that does not fit is refused, and never written as a prefix") {
    sc7::ResetForTest();
    sc7::Sc7Result r = {};
    FillToCapacity(r, '\x01');

    const std::wstring dir = ArtifactDir(L"jsonesc-overflow");
    CHECK_FALSE(sc7::WriteJson(dir.c_str(), r));
    const std::string json = ReadAll(dir + L"\\sc7_text_diff.json");

    // What is on disk is the refusal, not the first 32767 bytes of the report.
    CHECK(json.size() < 1024);
    CHECK(json.find("\"verdict\": \"COULD-NOT-ASK\"") != std::string::npos);
    CHECK(json.find("\"refused\": \"the sc7 report does not fit in the 32 KB document buffer") !=
          std::string::npos);
    CHECK(json.find("\\u0001") == std::string::npos);   // none of the record leaked through
    CHECK(json.size() >= 2);
    CHECK(json[json.size() - 1] == '\n');
    CHECK(json[json.size() - 2] == '}');
    size_t badAt = 0;
    CHECK_MESSAGE(EveryEscapeIsLegal(json, &badAt), "invalid escape at offset ", badAt);
    sc7::ResetForTest();
}

// The control the refusal needs: the same record, same field count, text that escapes 1:1 instead of
// 6:1, fits and is written. Without it "always refuses" would pass the case above.
TEST_CASE("the same maximal record in plain ASCII still fits and is written") {
    sc7::ResetForTest();
    sc7::Sc7Result r = {};
    FillToCapacity(r, 'x');

    const std::wstring dir = ArtifactDir(L"jsonesc-overflow-ascii");
    CHECK(sc7::WriteJson(dir.c_str(), r));
    const std::string json = ReadAll(dir + L"\\sc7_text_diff.json");
    CHECK(json.size() > 8192);
    CHECK(json.find("\"refused\"") == std::string::npos);
    size_t badAt = 0;
    CHECK_MESSAGE(EveryEscapeIsLegal(json, &badAt), "invalid escape at offset ", badAt);
    sc7::ResetForTest();
}

// ---------------------------------------------------------------------------------------------------
// AUDIT_L5FIX_R2 W1C-R2-1: the overflow boundary, SWEPT
// ---------------------------------------------------------------------------------------------------
//
// The cases above pin one record against one cap, and a fixed cap lets a record land the overflow on
// exactly one emission -- whichever one that record happens to run out of room on. So they could not
// see the half of the fix that mattered: this document's LAST emission is a plain format literal, and
// under a scheme where each helper had to propagate saturation itself, a helper that clamped instead
// wrote a truncated document that WriteJson reported as a complete report. The auditor's sweep found
// 29 of them, first at cap 10456, while all 43 shipped cases and 333 assertions stayed green.
//
// Sweeping the cap by 1 is what makes the boundary a property rather than a sample: at cap c the first
// emission that does not fit is a function of c, so stepping c from below the document's length to
// above it walks the boundary through every emission in the document, in order, the final literal
// included. The claim asserted at every point is the one WriteJson owes its consumer -- a complete
// document or a named refusal, never a prefix.
//
// "Complete" is checked by byte-identity against the document WriteJson itself put on disk at the
// shipped cap, which is stronger than any structural test: the cap is not an input to the CONTENT, so
// a document that differs from the reference by so much as a byte is a document the cap changed, and
// the only way the cap can change it is by cutting it short. tests/sc7/check.ps1 takes that same
// reference document, and the refusal, to a real JSON parser.

namespace {

struct SweepTally {
    size_t complete = 0;
    size_t refused = 0;
    size_t empty = 0;           // not even the refusal fits - the honest answer at a tiny cap
    size_t truncated = 0;       // reported complete, but not the document WriteJson wrote
    size_t regressed = 0;       // a cap above one that completed went back to refusing
    size_t overrun = 0;         // a length that does not leave room for the terminator
    size_t unterminated = 0;    // a document not ending `}\n`
    size_t refusalVariants = 0; // the refusal is one document; the cap must not reshape it
    size_t firstBadCap = 0;
    size_t boundaryCap = 0;     // the smallest cap at which the whole report fits
    std::string refusal;
};

// `reference` is what WriteJson wrote for this record at the shipped cap, or empty when the record
// does not fit there at all - in which case no cap below it may report a complete document either.
SweepTally SweepCaps(const sc7::Sc7Result& r, size_t lo, size_t hi, const std::string& reference) {
    SweepTally t;
    std::vector<char> buf(hi + 1);
    for (size_t cap = lo; cap <= hi; ++cap) {
        // Not zeros: an untouched byte has to be visible, so a length that overstates what was written
        // shows up as filler in the document rather than as a NUL that flatters it.
        std::memset(buf.data(), '~', buf.size());
        bool refused = false;
        const size_t len = sc7::SerializeForTest(r, buf.data(), cap, &refused);
        const bool late = t.boundaryCap != 0;
        if (len == 0) {
            ++t.empty;
            if (late && ++t.regressed == 1) { t.firstBadCap = cap; }
            continue;
        }
        if (len >= cap) {
            ++t.overrun;
            if (t.firstBadCap == 0) { t.firstBadCap = cap; }
            continue;
        }
        const std::string doc(buf.data(), len);
        if (len < 2 || doc[len - 1] != '\n' || doc[len - 2] != '}') {
            ++t.unterminated;
            if (t.firstBadCap == 0) { t.firstBadCap = cap; }
        }
        if (refused) {
            ++t.refused;
            if (t.refusal.empty()) {
                t.refusal = doc;
            } else if (t.refusal != doc) {
                ++t.refusalVariants;
            }
            if (late && ++t.regressed == 1) { t.firstBadCap = cap; }
            continue;
        }
        ++t.complete;
        if (doc != reference) {
            if (++t.truncated == 1) { t.firstBadCap = cap; }
        } else if (t.boundaryCap == 0) {
            t.boundaryCap = cap;
        }
    }
    return t;
}

}  // namespace

TEST_CASE("every cap writes the whole report or refuses - the boundary is swept, not sampled") {
    sc7::ResetForTest();
    sc7::Sc7Result r = {};
    FillToCapacity(r, 'x');

    // The reference is produced by the shipping path, not by this case: WriteJson at the shipped cap,
    // read back off disk.
    const std::wstring dir = ArtifactDir(L"sweep-complete");
    REQUIRE(sc7::WriteJson(dir.c_str(), r));
    const std::string reference = ReadAll(dir + L"\\sc7_text_diff.json");
    REQUIRE(reference.size() > 8192);
    REQUIRE(reference[reference.size() - 1] == '\n');
    REQUIRE(reference[reference.size() - 2] == '}');
    size_t badAt = 0;
    REQUIRE(EveryEscapeIsLegal(reference, &badAt));

    // 64..20000 is the range the audit swept. It is extended past this record's own length because
    // that is what puts the closing literal on the boundary - below the document's length the last
    // emission is never the one that overflows.
    const size_t hi = reference.size() + 64 > 20000 ? reference.size() + 64 : 20000;
    const SweepTally t = SweepCaps(r, 64, hi, reference);

    CHECK_MESSAGE(t.truncated == 0, "truncated documents reported as complete: ", t.truncated,
                  " first at cap ", t.firstBadCap);
    CHECK_MESSAGE(t.regressed == 0, "caps above the boundary that stopped completing, first at ",
                  t.firstBadCap);
    CHECK_MESSAGE(t.overrun == 0, "documents with no room for a terminator, first at ", t.firstBadCap);
    CHECK_MESSAGE(t.unterminated == 0, "documents not ending }\\n, first at cap ", t.firstBadCap);
    CHECK(t.refusalVariants == 0);

    // Neither arm may be vacuous, and the boundary is exact: a document of L bytes needs L + 1, the
    // last byte of a buffer belonging to the terminator. Anything else here is an off-by-one.
    CHECK(t.boundaryCap == reference.size() + 1);
    CHECK(t.complete == hi - reference.size());
    CHECK(t.refused + t.empty == reference.size() + 1 - 64);
    CHECK(t.refused > 1000);
    CHECK(t.empty > 0);

    sc7::ResetForTest();
}

TEST_CASE("a record that fits at no cap in the sweep refuses at every one of them") {
    sc7::ResetForTest();
    sc7::Sc7Result r = {};
    FillToCapacity(r, '\x01');   // 6:1 escapes - the same record, too big for the shipped buffer

    const std::wstring dir = ArtifactDir(L"sweep-refusal");
    CHECK_FALSE(sc7::WriteJson(dir.c_str(), r));

    // No reference: the record has no complete document at 32 KB, so it has none at 20000 either, and
    // any cap that returns one is returning a prefix.
    const SweepTally t = SweepCaps(r, 64, 20000, std::string());

    CHECK_MESSAGE(t.truncated == 0, "prefixes reported as complete: ", t.truncated, " first at cap ",
                  t.firstBadCap);
    CHECK(t.complete == 0);
    CHECK(t.overrun == 0);
    CHECK(t.unterminated == 0);
    CHECK(t.refusalVariants == 0);
    CHECK(t.refused > 19000);
    CHECK(t.empty > 0);
    CHECK(t.refused + t.empty == 20000 + 1 - 64);

    // The one refusal document every one of those caps produced.
    CHECK(t.refusal.find("\"verdict\": \"COULD-NOT-ASK\"") != std::string::npos);
    CHECK(t.refusal.find("\"refused\": \"the sc7 report does not fit in the 32 KB document buffer") !=
          std::string::npos);
    CHECK(t.refusal.find("\\u0001") == std::string::npos);   // none of the record leaked through
    size_t badAt = 0;
    CHECK_MESSAGE(EveryEscapeIsLegal(t.refusal, &badAt), "invalid escape at offset ", badAt);

    sc7::ResetForTest();
}

TEST_CASE("the same sweep over a small real record puts the boundary in the low emissions") {
    sc7::ResetForTest();
    sc7::Sc7Result r = {};
    Declare(r, "2.3");
    Observe(r, "2.3");
    PlantL5(r, true);
    sc7::ThirdPartySet rlay = RlaySet(true);
    sc7::Attribute(r, &rlay, 1);
    sc7::Decide(r);
    std::snprintf(r.pass, sizeof r.pass, "late-tick");

    const std::wstring dir = ArtifactDir(L"sweep-small");
    const bool wrote = sc7::WriteJson(dir.c_str(), r);
    REQUIRE(wrote);
    const std::string reference = ReadAll(dir + L"\\sc7_text_diff.json");
    REQUIRE(reference.size() > 256);
    REQUIRE(reference.size() < 8192);   // small enough that most of the sweep is above the boundary

    const SweepTally t = SweepCaps(r, 64, 20000, reference);

    CHECK_MESSAGE(t.truncated == 0, "truncated documents reported as complete: ", t.truncated,
                  " first at cap ", t.firstBadCap);
    CHECK(t.regressed == 0);
    CHECK(t.overrun == 0);
    CHECK(t.unterminated == 0);
    CHECK(t.refusalVariants == 0);
    CHECK(t.boundaryCap == reference.size() + 1);
    CHECK(t.complete == 20000 - reference.size());
    CHECK(t.refused + t.empty == reference.size() + 1 - 64);

    sc7::ResetForTest();
}

// AUDIT_L5_RUN3 M4 (§12 item 6). `AUDIT_W1-C_SC7.md#3` demanded `reloc_delta != 0` because "0 alone
// does not prove the walk ran". The demand cannot be met by a launch: relocDelta is base - ImageBase,
// a property of where Windows mapped echovr.exe, not of whether SC7 walked anything -- and L5 #3 came
// back `reloc_delta 0` WITH `reloc_fields_in_text 0`, which is the same record a walk that never ran
// would leave. The fix is an encoding, not a band: the record now also says how many base-relocation
// blocks the walk consumed and how many bytes the image DECLARES for that directory, so the three
// causes of a zero field count produce three different documents.
TEST_CASE("a zero reloc-field count is decidable: the walk's liveness is in the record, not in the delta") {
    Fixture fx;

    struct Case {
        const char* name;
        uint32_t    dirBytes;
        uint32_t    blocks;
        size_t      fields;
    };
    // 1. the image declares no relocation directory at all; 2. it declares one and the walk did not
    // run; 3. it declares one, the walk consumed every block, and .text holds no DIR64 field -- which
    // is what echovr.exe's own record should look like.
    const Case cases[3] = {
        {"no directory",   0,    0, 0},
        {"walk never ran", 4096, 0, 0},
        {"walked, none",   4096, 9, 0},
    };

    std::string documents[3];
    for (int i = 0; i < 3; ++i) {
        sc7::ImageView v = fx.View(cases[i].fields);
        v.relocDirBytes = cases[i].dirBytes;
        v.relocBlocksWalked = cases[i].blocks;

        sc7::Sc7Result r = {};
        sc7::Compare(v, nullptr, 0, r);
        CHECK(r.relocFieldsInText == 0);
        CHECK(r.relocDirBytes == cases[i].dirBytes);
        CHECK(r.relocBlocksWalked == cases[i].blocks);

        std::vector<char> buf(64 * 1024, '~');
        bool refused = false;
        const size_t len = sc7::SerializeForTest(r, buf.data(), buf.size(), &refused);
        REQUIRE(len > 0);
        REQUIRE_FALSE(refused);
        documents[i].assign(buf.data(), len);

        char expectBlocks[64];
        char expectDir[64];
        std::snprintf(expectBlocks, sizeof expectBlocks, "\"reloc_blocks_walked\": %u", cases[i].blocks);
        std::snprintf(expectDir, sizeof expectDir, "\"reloc_dir_bytes\": %u", cases[i].dirBytes);
        INFO("case: " << cases[i].name);
        CHECK(documents[i].find(expectBlocks) != std::string::npos);
        CHECK(documents[i].find(expectDir) != std::string::npos);
        // The field the old row leaned on is identical in all three, which is the whole point.
        CHECK(documents[i].find("\"reloc_fields_in_text\": 0") != std::string::npos);
    }

    // Pairwise distinct: a reader can tell the three apart from the document alone. Without the two new
    // fields these three runs serialise to the same bytes, and that is the miss AUDIT_W1-C_SC7.md#3
    // tried to catch with a band on a field that cannot carry the answer.
    CHECK(documents[0] != documents[1]);
    CHECK(documents[1] != documents[2]);
    CHECK(documents[0] != documents[2]);
}

// AUDIT_L5_RUN3 M4 / the design notes `window_blind_bytes == 5` was a literal that section 8.3e
// invalidated by adding a second in-.text row, and the amendment's replacement formula
// (`5 * declared_sites`, marked INFERRED) is right only while every declared site is ARMED. What
// Compare actually computes, per site, is `kWindowBefore + kWindowAfter - (armed ? kMinHookMaxPatch
// : 0)` -- 5 for an armed site and 12 for an unarmed one. This case pins that, and the second row is
// the one that separates the two formulae.
TEST_CASE("the window's blind spot is per-site and depends on arming, not on the site count alone") {
    Fixture fx;
    const size_t blind = sc7::PatchWindowBefore() + sc7::PatchWindowAfter();
    CHECK(blind == 12);

    struct Case { uint32_t declared; uint32_t armed; uint32_t expect; };
    // Row 2 is L5 #3's own record: declared_sites 2, armed_ledger_sites 2, window_blind_bytes 10.
    // Rows 5 and 6 are the shipping configuration ARCH-15 leaves behind: rows 2.3, 2.4 and 2.5 all
    // declared, so 15 when all three are armed and 22 when the in-frame draw's site is registered and
    // refused - 5 * armed + 12 * (declared - armed), the formula the harness reads.
    const Case cases[6] = {
        {1, 1, 5}, {2, 2, 10}, {2, 1, 17}, {2, 0, 24}, {3, 3, 15}, {3, 2, 22},
    };
    for (int i = 0; i < 6; ++i) {
        sc7::SiteView sites[4] = {};
        for (uint32_t s = 0; s < cases[i].declared; ++s) {
            sites[s].factId = "SITE";
            sites[s].row = "2.3";
            sites[s].rva = kSiteRva + s * 0x40;
            sites[s].armed = s < cases[i].armed;
        }
        sc7::Sc7Result r = {};
        sc7::Compare(fx.View(), sites, cases[i].declared, r);
        INFO("declared " << cases[i].declared << " armed " << cases[i].armed);
        CHECK(r.windowBlindBytes == cases[i].expect);
    }
    // 5 * declared_sites agrees with the reader only on the all-armed rows, which is why the band has
    // to carry the arming term: at declared 2 / armed 1 it says 10 and the reader says 17.
    CHECK(cases[2].expect != 5 * cases[2].declared);
    CHECK(cases[5].expect != 5 * cases[5].declared);
}

// ARCH-15 row 2.5. W2-F installs a 5-byte relative jump at GUI.ENGINE_HEARTBEAT_BEFORE
// (VA 0x1415483B0, facts.tsv:451, prologue guard :452) through hookmgr with config key
// `ui.inframe_draw`; W1-C owns the ledger row that accounts for it. The site is REAL here - the
// fixture's .text is rebased so buffer offset 0x200 IS the RVA of that VA at the image's own base - so
// Compare has to subtract the address the ledger names rather than a stand-in.
TEST_CASE("row 2.5 accounts for the in-frame draw detour at 0x1415483B0") {
    // From the registry, never transcribed: a fact that moves moves this test with it.
    // The PE OptionalHeader.ImageBase every PC_VA in the registry is stated against: a property of the
    // header, not a fact row, so there is no registry value to name here.
    constexpr uint64_t kImageBase = 0x140000000ull;  // facts-ok: GUI.ENGINE_HEARTBEAT_BEFORE the base its PC_VA is stated against
    constexpr uint64_t kInFrameRva = facts::GUI_ENGINE_HEARTBEAT_BEFORE - kImageBase;
    Fixture fx;
    // The first five bytes of GUI.ENGINE_HEARTBEAT_BEFORE.PROLOGUE, which are the first five the row's
    // guard checks and the five a 5-byte relative jump overwrites.
    std::memcpy(fx.disk + 0x200, facts::GUI_ENGINE_HEARTBEAT_BEFORE_PROLOGUE, 5);
    std::memcpy(fx.live + 0x200, facts::GUI_ENGINE_HEARTBEAT_BEFORE_PROLOGUE, 5);
    sc7::ImageView image = fx.View();
    image.textRva = kInFrameRva - 0x200;

    sc7::SiteView site = {};
    site.factId = "GUI.ENGINE_HEARTBEAT_BEFORE";
    site.row = "2.5";
    site.rva = kInFrameRva;
    site.armed = true;

    SUBCASE("the shipping default declares 2.5, and the key alone is what declares it") {
        config::ResetForTest();
        sc7::Sc7Result on = {};
        sc7::DeclaredRows(on);
        CHECK(HasRow(on.expected, on.expectedCount, "2.5"));

        const char* off = "{\"ui.inframe_draw\": false, \"audio.sink_term_hook\": false}";
        REQUIRE(config::LoadFromTextForTest(off, std::strlen(off)));
        sc7::Sc7Result d = {};
        sc7::DeclaredRows(d);
        CHECK_FALSE(HasRow(d.expected, d.expectedCount, "2.5"));
        CHECK_FALSE(IsDeferred(d, "2.5"));
        CHECK(HasRow(d.expected, d.expectedCount, "2.3"));   // the walk ran; 2.5 is absent, not skipped
        config::ResetForTest();
    }

    SUBCASE("installed and armed: the jump is attributed to row 2.5 and nothing is unowned") {
        fx.PlantJump(0x200);
        sc7::Sc7Result r = {};
        Declare(r, "2.5");
        sc7::Compare(image, &site, 1, r);
        CHECK(r.unattributedTotal == 0);
        CHECK(HasRow(r.observed, r.observedCount, "2.5"));
        REQUIRE(r.detailCount == 1);
        CHECK(r.detail[0].rva == kInFrameRva);
        CHECK(std::string(r.detail[0].row) == "2.5");
        CHECK(r.windowBlindBytes == 5);
        sc7::Decide(r);
        CHECK(r.verdict == sc7::Verdict::Pass);
        CHECK(r.unownedTotal == 0);

        const std::wstring dir = TempDir();
        std::snprintf(r.pass, sizeof r.pass, "post-init");
        REQUIRE(sc7::WriteJson(dir.c_str(), r));
        const std::string json = ReadAll(dir + L"\\sc7_text_diff.json");
        CHECK(json.find("\"expected_rows\": [\"2.5\"]") != std::string::npos);
        CHECK(json.find("\"unowned_total\": 0") != std::string::npos);
        CHECK(json.find(RvaText(kInFrameRva)) != std::string::npos);
    }

    SUBCASE("key off: declared, not armed - the whole window is blind and nothing is observed") {
        // hookmgr registers the site whatever the key says (RefusedByConfig), so GatherSites still
        // publishes it, unarmed. An unarmed site accounts for none of its window, which is the 12 the
        // record has to carry rather than the 5 an armed one earns.
        site.armed = false;
        sc7::Sc7Result r = {};
        Declare(r, "2.3");
        sc7::Compare(image, &site, 1, r);
        CHECK(r.unattributedTotal == 0);
        CHECK(r.observedCount == 0);
        CHECK(r.windowBlindBytes == 12);
    }

    SUBCASE("key off and somebody else wrote at that VA: FAIL, and the record names the address") {
        // The bytes are not ours - with the key off we installed nothing - so the row is observed
        // where no enabled ledger row declares it. That is an undeclared detour, and it is a FAIL.
        site.armed = false;
        fx.PlantJump(0x200);
        sc7::Sc7Result r = {};
        Declare(r, "2.3");
        std::snprintf(r.observed[r.observedCount++].id, sc7::kRowChars, "2.3");
        sc7::Compare(image, &site, 1, r);
        sc7::Decide(r);
        CHECK(r.verdict == sc7::Verdict::Fail);
        CHECK(std::string(r.reason).find("1 undeclared detour(s)") != std::string::npos);

        const std::wstring dir = TempDir();
        std::snprintf(r.pass, sizeof r.pass, "post-init");
        REQUIRE(sc7::WriteJson(dir.c_str(), r));
        CHECK(ReadAll(dir + L"\\sc7_text_diff.json").find(RvaText(kInFrameRva)) !=
              std::string::npos);
    }

    SUBCASE("MUTANT: the key claims no ledger row, and the same jump is unowned") {
        // `RowForKey` answers null when the key's third argument is nullptr, `RowIsInText` refuses null
        // and GatherSites then publishes no site at all. This is L5 #3's failure class - a 5-byte jump
        // standing in .text with nothing to attribute it to - reproduced at row 2.5's own address.
        fx.PlantJump(0x200);
        sc7::Sc7Result r = {};
        Declare(r, "2.3");
        std::snprintf(r.observed[r.observedCount++].id, sc7::kRowChars, "2.3");
        sc7::Compare(image, nullptr, 0, r);
        sc7::Decide(r);
        CHECK(r.verdict == sc7::Verdict::Fail);
        CHECK(r.unownedTotal == 1);
        REQUIRE(r.unattributedCount == 1);
        CHECK(r.unattributed[0].rva == kInFrameRva);
        CHECK(r.unattributed[0].len == 5);
        CHECK(std::string(r.unattributed[0].owner).empty());
    }

    SUBCASE("MUTANT: the prologue guard refused, so a declared row stands with no detour") {
        // hookmgr answers RefusedByPrologue when the site does not hold the recorded bytes: the site is
        // registered, unarmed, and nothing is written. A declared row with no detour is a FAIL, never a
        // pass earned by the absence of evidence.
        site.armed = false;
        sc7::Sc7Result r = {};
        Declare(r, "2.5");
        sc7::Compare(image, &site, 1, r);
        sc7::Decide(r);
        CHECK(r.verdict == sc7::Verdict::Fail);
        CHECK(std::string(r.reason).find("1 declared row(s) with no detour") == 0);
    }

    SUBCASE("L5 #3's record judges identically: row 2.5 is not imposed on a launch that ran without it") {
        // Retroactive control. L5 #3 predates the row; its record declares 2.3 and 2.4 and carries no
        // site at 0x15483B0. Nothing in this packet changes that verdict - the declaration follows the
        // config the launch ran with, and a launch with `ui.inframe_draw` off declares no 2.5.
        sc7::SiteView lock = {};
        lock.factId = "OFFLINELOCK.TEARDOWN";
        lock.row = "2.4";
        lock.rva = image.textRva + 0x600;
        lock.armed = true;
        fx.PlantJump(0x600);

        sc7::Sc7Result r = {};
        Declare(r, "2.4");
        sc7::Compare(image, &lock, 1, r);
        sc7::Decide(r);
        CHECK(r.verdict == sc7::Verdict::Pass);
        CHECK(r.unownedTotal == 0);
        CHECK_FALSE(HasRow(r.observed, r.observedCount, "2.5"));
    }
}
