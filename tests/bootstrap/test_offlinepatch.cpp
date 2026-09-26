// The in-process -offline -windowed (bootstrap/offlinepatch.h), driven over a buffer laid out like the
// image: the same classify / plan / write path and the same vtable wrapper the game gets, with a fake
// CLI.APPLY in the slot. The one thing only a launch can show is that the engine then hosts the level.
//
// No commas in test case names: doctest reads a comma in --test-case as a filter separator.

#include <doctest.h>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bootstrap/bootstrap.h"
#include "bootstrap/livesafety.h"
#include "bootstrap/offlinepatch.h"
#include "bootstrap/offlinepatch_facts.h"

namespace op = replay::offlinepatch;
namespace pf = replay::offlinepatch::facts;
using op::Plan;
using op::SiteState;
using op::Status;

namespace {

uint64_t Rva(size_t i) { return pf::kSites[i].va - pf::kPreferredImageBase; }

// Committed but untouched pages cost nothing, so the buffer spans the highest site like the image does.
struct FakeImage {
    uint8_t* base = nullptr;
    size_t size = 0;

    FakeImage() {
        for (size_t i = 0; i < pf::kSiteCount; ++i) {
            const size_t end = static_cast<size_t>(Rva(i)) + pf::kSites[i].len;
            if (end > size) size = end;
        }
        size += 64;
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        REQUIRE(base != nullptr);
    }
    ~FakeImage() { VirtualFree(base, 0, MEM_RELEASE); }

    uint8_t* At(size_t i) const { return base + Rva(i); }
    void Fill(bool patched) const {
        for (size_t i = 0; i < pf::kSiteCount; ++i) {
            std::memcpy(At(i), patched ? pf::kSites[i].patched : pf::kSites[i].original, pf::kSites[i].len);
        }
    }
    bool Holds(size_t i, const char* bytes) const {
        return std::memcmp(At(i), bytes, pf::kSites[i].len) == 0;
    }
    bool AllHold(bool patched) const {
        for (size_t i = 0; i < pf::kSiteCount; ++i) {
            if (!Holds(i, patched ? pf::kSites[i].patched : pf::kSites[i].original)) return false;
        }
        return true;
    }
};

struct Reset {
    Reset() { op::ResetForTest(); }
    ~Reset() { op::ResetForTest(); }
};

size_t SiteIndex(const char* name) {
    for (size_t i = 0; i < pf::kSiteCount; ++i) {
        if (std::strcmp(pf::kSites[i].name, name) == 0) return i;
    }
    FAIL("no site named " << name);
    return 0;
}

int32_t Rel32(const char* bytes, size_t at) {
    int32_t v = 0;
    std::memcpy(&v, bytes + at, sizeof v);
    return v;
}

// E8 rel32 at `va`: the callee.
uint64_t CallTarget(uint64_t va, const char* bytes) {
    REQUIRE(static_cast<uint8_t>(bytes[0]) == 0xE8);
    return va + 5 + static_cast<int64_t>(Rel32(bytes, 1));
}

// The fake CLI.APPLY the slot is handed. `actAsRlay` makes it write the patched bytes the way rlay's
// detour does from inside the real call.
struct FakeApply {
    int calls = 0;
    uint8_t* lastGame = nullptr;
    uint64_t flagsSeen = 0;
    const FakeImage* rlayImage = nullptr;
};
FakeApply g_fake;

int64_t FakeCliApply(uint8_t* game) {
    ++g_fake.calls;
    g_fake.lastGame = game;
    g_fake.flagsSeen = *reinterpret_cast<uint64_t*>(game + pf::kGameFlagsOff);
    if (g_fake.rlayImage != nullptr) g_fake.rlayImage->Fill(true);
    return 42;
}

using SlotFn = int64_t (*)(uint8_t*);

}  // namespace

TEST_CASE("offline patch: every site decodes to the target the report names") {
    const size_t boot = SiteIndex("boot-offline-match");
    CHECK(CallTarget(pf::kSites[boot].va, pf::kSites[boot].original) == 0x1400FDA70ull);  // facts-ok: SESSION.BEGIN_MULTIPLAYER PC_PRESENT, so gen_facts emits no value
    CHECK(CallTarget(pf::kSites[boot].va, pf::kSites[boot].patched) == ::facts::SESSION_CREATE_OFFLINE_MATCH_LH);

    const size_t tutorial = SiteIndex("tutorial-redirect");
    CHECK(CallTarget(pf::kSites[tutorial].va, pf::kSites[tutorial].original) == 0x1400FDEB0ull);
    CHECK(CallTarget(pf::kSites[tutorial].va, pf::kSites[tutorial].patched) == 0x1400FDE60ull);

    // jnz rel32 (6 bytes) and the jmp rel32 (5 bytes) that replaces it land on the same instruction.
    const size_t login = SiteIndex("login-failure-skip");
    const char* jnz = pf::kSites[login].original;
    const char* jmp = pf::kSites[login].patched;
    REQUIRE(static_cast<uint8_t>(jnz[0]) == 0x0F);
    REQUIRE(static_cast<uint8_t>(jnz[1]) == 0x85);
    REQUIRE(static_cast<uint8_t>(jmp[0]) == 0xE9);
    CHECK(pf::kSites[login].va + 6 + Rel32(jnz, 2) == pf::kSites[login].va + 5 + Rel32(jmp, 1));

    // je -> jne, same displacement.
    const size_t host = SiteIndex("offline-host-branch");
    CHECK(static_cast<uint8_t>(pf::kSites[host].original[0]) == 0x74);
    CHECK(static_cast<uint8_t>(pf::kSites[host].patched[0]) == 0x75);
    CHECK(pf::kSites[host].original[1] == pf::kSites[host].patched[1]);

    for (size_t i = 0; i < pf::kSiteCount; ++i) {
        CHECK(pf::kSites[i].len <= pf::kMaxSiteLen);
        CHECK(std::memcmp(pf::kSites[i].original, pf::kSites[i].patched, pf::kSites[i].len) != 0);
    }
}

TEST_CASE("offline patch: the originals are the shipping echovr.exe's bytes") {
    char path[MAX_PATH] =
        R"(C:\Program Files\Oculus\Software\Software\ready-at-dawn-echo-arena\bin\win10\echovr.exe)";
    GetEnvironmentVariableA("REPLAY_ECHOVR_EXE", path, sizeof path);
    FILE* f = nullptr;
    REQUIRE_MESSAGE(fopen_s(&f, path, "rb") == 0, "echovr.exe is needed for this check: " << path);
    std::vector<uint8_t> file;
    fseek(f, 0, SEEK_END);
    file.resize(static_cast<size_t>(ftell(f)));
    fseek(f, 0, SEEK_SET);
    REQUIRE(fread(file.data(), 1, file.size(), f) == file.size());
    fclose(f);

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    CHECK(nt->FileHeader.TimeDateStamp == pf::kTimeDateStamp);
    CHECK(nt->OptionalHeader.ImageBase == pf::kPreferredImageBase);

    const auto* sections = IMAGE_FIRST_SECTION(nt);
    auto fileBytes = [&](uint64_t va) -> const uint8_t* {
        const uint64_t rva = va - pf::kPreferredImageBase;
        for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
            const auto& sec = sections[s];
            if (rva >= sec.VirtualAddress && rva < sec.VirtualAddress + sec.SizeOfRawData) {
                return file.data() + sec.PointerToRawData + (rva - sec.VirtualAddress);
            }
        }
        return nullptr;
    };
    for (size_t i = 0; i < pf::kSiteCount; ++i) {
        const uint8_t* on = fileBytes(pf::kSites[i].va);
        REQUIRE(on != nullptr);
        INFO(pf::kSites[i].name);
        CHECK(op::ClassifySite(on, i) == SiteState::Original);
    }
    // The slot the wrapper takes holds CLI.APPLY on disk too (a DIR64 the loader rebases).
    uint64_t slot = 0;
    std::memcpy(&slot, fileBytes(pf::kGameVtable), sizeof slot);
    CHECK(slot == pf::kCliApply);
}

TEST_CASE("offline patch: site classification and the plan") {
    FakeImage img;
    img.Fill(false);
    CHECK(op::ClassifySite(img.At(0), 0) == SiteState::Original);
    std::memcpy(img.At(0), pf::kSites[0].patched, pf::kSites[0].len);
    CHECK(op::ClassifySite(img.At(0), 0) == SiteState::Patched);
    img.At(0)[1] ^= 0xFF;
    CHECK(op::ClassifySite(img.At(0), 0) == SiteState::Foreign);

    const SiteState o = SiteState::Original, p = SiteState::Patched, x = SiteState::Foreign;
    const SiteState allOriginal[] = {o, o, o, o};
    const SiteState allPatched[] = {p, p, p, p};
    const SiteState mixed[] = {o, p, o, o};
    const SiteState oneForeign[] = {o, o, x, o};
    const SiteState patchedForeign[] = {p, p, p, x};
    CHECK(op::PlanSites(allOriginal, 4) == Plan::Write);
    CHECK(op::PlanSites(allPatched, 4) == Plan::AlreadyApplied);
    CHECK(op::PlanSites(mixed, 4) == Plan::Refuse);
    CHECK(op::PlanSites(oneForeign, 4) == Plan::Refuse);
    CHECK(op::PlanSites(patchedForeign, 4) == Plan::Refuse);
    CHECK(op::PlanSites(allOriginal, 0) == Plan::Refuse);
}

TEST_CASE("offline patch: writes every site then is idempotent") {
    Reset reset;
    FakeImage img;
    img.Fill(false);
    CHECK(op::PatchImage(img.base) == Status::Applied);
    CHECK(img.AllHold(true));
    CHECK(op::Current() == Status::Applied);

    op::Span spans[8];
    REQUIRE(op::WrittenSpans(spans, 8) == pf::kSiteCount);
    for (size_t i = 0; i < pf::kSiteCount; ++i) {
        CHECK(spans[i].rva == Rva(i));
        CHECK(spans[i].len == pf::kSites[i].len);
    }

    // A second pass finds its own bytes: nothing rewritten, still ours.
    CHECK(op::PatchImage(img.base) == Status::Applied);
    CHECK(img.AllHold(true));
    CHECK(op::WrittenSpans(spans, 8) == pf::kSiteCount);
}

TEST_CASE("offline patch: rlay already applied means nothing is written") {
    Reset reset;
    FakeImage img;
    img.Fill(true);
    CHECK(op::PatchImage(img.base) == Status::AlreadyApplied);
    CHECK(img.AllHold(true));
    op::Span spans[8];
    CHECK(op::WrittenSpans(spans, 8) == 0);
    CHECK(op::OfflineHostExpected(op::Current()));
}

TEST_CASE("offline patch: one foreign byte refuses every site") {
    for (size_t bad = 0; bad < pf::kSiteCount; ++bad) {
        Reset reset;
        FakeImage img;
        img.Fill(false);
        img.At(bad)[0] ^= 0x01;
        INFO(pf::kSites[bad].name);
        CHECK(op::PatchImage(img.base) == Status::RefusedBytes);
        for (size_t i = 0; i < pf::kSiteCount; ++i) {
            if (i != bad) CHECK(img.Holds(i, pf::kSites[i].original));
        }
        op::Span spans[8];
        CHECK(op::WrittenSpans(spans, 8) == 0);
        CHECK_FALSE(op::OfflineHostExpected(op::Current()));
    }
    // Half rlay's, half stock: also somebody else's state.
    Reset reset;
    FakeImage img;
    img.Fill(false);
    std::memcpy(img.At(0), pf::kSites[0].patched, pf::kSites[0].len);
    CHECK(op::PatchImage(img.base) == Status::RefusedBytes);
    CHECK(img.Holds(1, pf::kSites[1].original));
}

TEST_CASE("offline patch: the slot wrapper sets windowed before the original and patches after") {
    Reset reset;
    FakeImage img;
    img.Fill(false);
    std::vector<uint8_t> game(pf::kGameFlagsOff + 16, 0);
    void* slot = reinterpret_cast<void*>(&FakeCliApply);
    g_fake = {};

    REQUIRE(op::InstallAt(&slot, reinterpret_cast<void*>(&FakeCliApply), img.base));
    CHECK(slot != reinterpret_cast<void*>(&FakeCliApply));
    CHECK(op::Current() == Status::Pending);
    CHECK(op::OfflineHostExpected(op::Current()));
    CHECK(img.AllHold(false));

    // A second install is a no-op, not a wrapper that calls itself.
    REQUIRE(op::InstallAt(&slot, reinterpret_cast<void*>(&FakeCliApply), img.base));

    CHECK(reinterpret_cast<SlotFn>(slot)(game.data()) == 42);
    CHECK(g_fake.calls == 1);
    CHECK(g_fake.lastGame == game.data());
    CHECK((g_fake.flagsSeen & pf::kWindowedBit) != 0);
    CHECK(img.AllHold(true));
    CHECK(op::Current() == Status::Applied);

    // CLI.APPLY again: a pass-through, not a second decision.
    img.Fill(false);
    CHECK(reinterpret_cast<SlotFn>(slot)(game.data()) == 42);
    CHECK(g_fake.calls == 2);
    CHECK(img.AllHold(false));
}

TEST_CASE("offline patch: through the slot rlay's own -offline wins and ours writes nothing") {
    Reset reset;
    FakeImage img;
    img.Fill(false);
    std::vector<uint8_t> game(pf::kGameFlagsOff + 16, 0);
    void* slot = reinterpret_cast<void*>(&FakeCliApply);
    g_fake = {};
    g_fake.rlayImage = &img;

    REQUIRE(op::InstallAt(&slot, reinterpret_cast<void*>(&FakeCliApply), img.base));
    reinterpret_cast<SlotFn>(slot)(game.data());
    CHECK(op::Current() == Status::AlreadyApplied);
    op::Span spans[8];
    CHECK(op::WrittenSpans(spans, 8) == 0);
    g_fake.rlayImage = nullptr;
}

TEST_CASE("offline patch: a slot somebody else owns is refused and left alone") {
    Reset reset;
    FakeImage img;
    img.Fill(false);
    int other = 0;
    void* slot = &other;
    CHECK_FALSE(op::InstallAt(&slot, reinterpret_cast<void*>(&FakeCliApply), img.base));
    CHECK(slot == &other);
    CHECK(op::Current() == Status::RefusedSlot);
    CHECK_FALSE(op::OfflineHostExpected(op::Current()));
}

TEST_CASE("offline patch: never in a live session and only on the replay trigger") {
    namespace bs = replay::bootstrap;
    CHECK(replay::livesafety::OfflinePatchAllowed(true, false));
    CHECK_FALSE(replay::livesafety::OfflinePatchAllowed(true, true));
    CHECK_FALSE(replay::livesafety::OfflinePatchAllowed(false, true));
    CHECK_FALSE(replay::livesafety::OfflinePatchAllowed(false, false));

    auto allowed = [](const wchar_t* line, bool open, bool rlay) {
        const bs::SessionMode mode = bs::ClassifySession(line, open, rlay);
        return replay::livesafety::OfflinePatchAllowed(
            mode == bs::SessionMode::Replay, bs::IsLiveOnlineExempt(mode, bs::HasSwitch(line, L"-offline")));
    };
    // The owner's live spectate: no trigger, with or without rlay.
    CHECK_FALSE(allowed(L"echovr.exe -spectatorstream", false, false));
    CHECK_FALSE(allowed(L"echovr.exe -spectatorstream", false, true));
    // An rlay offline launch with no replay requested is rlay's business, not ours.
    CHECK_FALSE(allowed(L"echovr.exe -offline -windowed", false, true));
    // The trigger, with and without rlay, old arguments or new.
    CHECK(allowed(L"echovr.exe -level mpl_arena_a -lobbyteam 2", true, false));
    CHECK(allowed(L"echovr.exe -offline -windowed -level mpl_arena_a", true, true));
    // The retired switch is refused whatever else is set.
    CHECK_FALSE(allowed(L"echovr.exe -replayviewer x.echoreplay", true, false));
}

TEST_CASE("offline patch: the process install follows the environment and refuses a foreign build") {
    Reset reset;
    wchar_t saved[1024] = {};
    const DWORD had = GetEnvironmentVariableW(L"ECHOREPLAY_OPEN", saved, 1024);

    SetEnvironmentVariableW(L"ECHOREPLAY_OPEN", nullptr);
    CHECK_FALSE(op::InstallForThisProcess());
    CHECK(op::Current() == Status::NotRequested);

    // Requested, but this test host is not echovr.exe: the build check refuses before any slot is read.
    SetEnvironmentVariableW(L"ECHOREPLAY_OPEN", L"C:\\nowhere\\x.echodemo");
    CHECK_FALSE(op::InstallForThisProcess());
    CHECK(op::Current() == Status::RefusedBuild);

    SetEnvironmentVariableW(L"ECHOREPLAY_OPEN", had != 0 && had < 1024 ? saved : nullptr);
}
