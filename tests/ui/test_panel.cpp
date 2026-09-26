// W2-F. The panel's WIRING, which is the half the audit found untested and therefore unwired.
//
// tests/ui/test_ui.cpp sweeps timeline.cpp's arithmetic and touches nothing else, so three
// deliverables shipped with no caller and nothing noticed: LoadPath never consulted ISessionEntry,
// ShowPanel/Toggle had no caller anywhere in src/, and SetCursorEnabled was bound and never called.
// Every case below fails if its call is removed again -- that is the point of the file.
//
// It drives the real panel.cpp and the real demoindex.cpp against real .echodemo files on disk, a
// fake ISessionEntry and a fake IReplayTransport. The ImGui facade is fake_gui.cpp; see its header
// for what that does and does not stand in for.

#include "doctest.h"

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <ratio>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "bootstrap/livesafety.h"
#include "bootstrap/log.h"
#include "bootstrap/paths.h"
#include "bootstrap/services.h"
#include "camera/camera_control.h"
#include "engine/session.h"
#include "fake_gui.h"
#include "scratch_capture_dir.h"
#include "foundation/demofile/demofile.h"
#include "foundation/demofile/format.h"
#include "generated/facts_generated.h"
#include "offlinelock_held.h"
#include "capture/markers.h"
#include "ui/timeline.h"
#include "playback/core/transport.h"
#include "ui/csregister.h"
#include "ui/inframe.h"
#include "probe_ui_outcome.h"
#include "ui/demoindex.h"
#include "ui/gui.h"
#include "ui/levelname.h"
#include "ui/panel.h"
#include "ui/uiapi.h"

using namespace replay;

namespace {

// ---- a scratch directory, removed with everything in it -------------------------------------

class TempDir {
public:
    TempDir() {
        wchar_t base[MAX_PATH];
        ::GetTempPathW(MAX_PATH, base);
        static unsigned counter = 0;
        _snwprintf_s(dir_, MAX_PATH, _TRUNCATE, L"%sw2f_panel_%lu_%u", base,
                     ::GetCurrentProcessId(), counter++);
        ::CreateDirectoryW(dir_, nullptr);
    }
    ~TempDir() { Remove(dir_); }

    const wchar_t* Path() const { return dir_; }

    std::wstring File(const wchar_t* leaf) const {
        return std::wstring(dir_) + L"\\" + leaf;
    }

    static void Remove(const wchar_t* dir) {
        std::wstring pattern = std::wstring(dir) + L"\\*";
        WIN32_FIND_DATAW fd = {};
        HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
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

private:
    wchar_t dir_[MAX_PATH] = {};
};

// ---- a real, minimal capture ------------------------------------------------------------------
//
// Written with the shipped DemoWriter so DemoIndex reads it exactly as it reads a recording: the
// level and gametype ids the entry decision turns on come out of a real MANIFEST, not out of a
// hand-made struct the reader never saw.

const RoundMarker* g_markerOverride = nullptr;
uint16_t g_markerOverrideCount = 0;
// The ARCH-21 round table -- a different count from the S_MARK edges above, and the one the
// browser row's "rounds" word means. Null leaves the manifest without a ROUNDS block, which is
// what every capture here that does not ask for one gets.
const replay::RoundRow* g_roundRowOverride = nullptr;
uint16_t g_roundRowOverrideCount = 0;

// A gametype stand-in as a whole qword. The load funnel resolves a recorded value whose high half
// is clear (a pre-64-bit capture) into something else, which is its own set of cases below; every
// other case wants a value that reaches the session exactly as recorded.
constexpr uint64_t Whole(uint64_t standIn) { return 0x6A7E000000000000ull | standIn; }

// How many players WriteCapture records. One by default; the playable-only filter's own case sets
// it to zero for the captures that are its subject.
uint16_t g_playerCountOverride = 1;

bool WriteCapture(const wchar_t* path, uint64_t levelId, uint64_t gametypeId, uint16_t rounds) {
    static RoundMarker markers[8];
    static FieldDef field{};
    static SchemaDef schema{};
    Manifest m{};
    m.formatMajor = kFormatMajor;
    m.formatMinor = kFormatMinor;
    m.t0Us = 1700000000000000ull;
    m.levelId = levelId;
    m.gametypeId = gametypeId;
    m.keyframeIntervalUs = 2000000;
    m.captureRole = 1;
    m.startTimeEpoch = 1700000000ull;
    if (rounds > 8) rounds = 8;
    for (uint16_t i = 0; i < rounds; ++i) {
        markers[i] = RoundMarker{};
        markers[i].tUs = m.t0Us + uint64_t(i + 1) * 1000000ull;
        markers[i].kind = 0;
        markers[i].index = static_cast<uint16_t>(i + 1);
    }
    m.roundCount = rounds;
    m.rounds = rounds > 0 ? markers : nullptr;
    if (g_markerOverride != nullptr) {
        m.roundCount = g_markerOverrideCount;
        m.rounds = g_markerOverride;
    }
    if (g_roundRowOverride != nullptr) {
        m.roundRowCount = g_roundRowOverrideCount;
        m.roundRows = g_roundRowOverride;
        m.markerSource = replay::MarkerSource::Local;
    }

    // One schema, so the tick block below is not a block citing a schema the manifest does not
    // carry -- the writer records that as a caller bug and this fixture is not here to be one.
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

    // MULTI-LOAD: one recorded player, because the browser now HIDES captures whose manifest says
    // nobody was in them -- most of the owner's folder is solo and lobby sessions. A fixture folder
    // of empty captures would make every browser case here assert against a filtered-out listing.
    // g_playerCountOverride writes a genuinely empty one where that is the subject.
    // One record per counted player. A single record under a count of 3 had the writer read two
    // records' worth of whatever static followed it -- Blob pointers included -- which crashed the
    // MULTI-LOAD case as soon as another file's statics moved the layout.
    static PlayerRecord players[8];
    const uint16_t playerCount = g_playerCountOverride < 8 ? g_playerCountOverride : 8;
    for (uint16_t i = 0; i < playerCount; ++i) {
        players[i] = PlayerRecord{};
        players[i].xplatformId = 0x1234 + i;
        memcpy(players[i].displayName, "fixture", 8);
        memcpy(players[i].displayNameFull, "fixture", 8);
        players[i].userSlot = i;
        players[i].userIndex = static_cast<uint16_t>(i + 1);
    }
    m.playerCount = playerCount;
    m.players = playerCount > 0 ? players : nullptr;

    DemoWriter w;
    if (!w.Open(path, m)) {
        fprintf(stderr, "WriteCapture: Open failed: %s\n",
                w.LastError() != nullptr ? w.LastError() : "?");
        return false;
    }
    const uint64_t tick[4] = {m.t0Us, m.t0Us + 22000, m.t0Us + 44000, m.t0Us + 66000};
    w.BeginChapter(kSectionCore, m.t0Us);
    w.WriteStreamBlock(kStreamTick, 1, tick, sizeof tick, 4, 4);
    w.EndChapter(m.t0Us + 2000000ull);
    w.Close();
    // Close writes the MANIFEST and INDEX chapters beside the CORE one, so the count is 3, not 1.
    if (w.Stats().chaptersWritten == 0 || w.LastError() != nullptr) {
        fprintf(stderr, "WriteCapture: %u chapter(s), last error %s\n", w.Stats().chaptersWritten,
                w.LastError() != nullptr ? w.LastError() : "(none)");
        return false;
    }
    return true;
}

// ---- the two engine seams the panel consults --------------------------------------------------

class FakeSession final : public ISessionEntry {
public:
    bool canChange = false;
    bool requestAccepts = true;
    // Why a false RequestEntry answered as it did. CannotEnter is the SETTLED word and is the
    // default here for the same reason it is the default on the interface: an implementation
    // written before the question existed tells its caller to stop rather than to spin.
    replay::EntryRefusal refusal = replay::EntryRefusal::CannotEnter;
    bool matches = true;
    uint64_t levelId = 0;
    uint64_t gametypeId = 0;

    uint32_t matchCalls = 0;
    uint32_t requestCalls = 0;
    uint64_t requestedLevel = 0;
    uint64_t requestedGametype = 0;

    bool CanChangeLevelMidSession() const override { return canChange; }

    bool Matches(uint64_t level, uint64_t gametype) const override {
        ++const_cast<FakeSession*>(this)->matchCalls;
        return matches && level == levelId && gametype == gametypeId;
    }

    bool RequestEntry(uint64_t level, uint64_t gametype) override {
        ++requestCalls;
        requestedLevel = level;
        requestedGametype = gametype;
        if (requestAccepts) {
            levelId = level;
            gametypeId = gametype;
            // Accepted, not performed: Matches only becomes true once the caller says the tick ran.
        }
        return requestAccepts;
    }

    NetGameState State() const override { return NetGameState::InGame; }
    void* NetGame() const override { return nullptr; }
    const char* Strategy() const override { return "SE-FAKE"; }
    const char* FailureReason() const override { return "SE-FAKE cannot change level after startup"; }
    replay::EntryRefusal LastRefusal() const override { return refusal; }
};

class FakeTransport final : public IReplayTransport {
public:
    bool loadAccepts = true;
    uint32_t loadCalls = 0;
    uint32_t unloadCalls = 0;
    uint32_t seekCalls = 0;
    int64_t lastSeekUs = -1;
    std::wstring lastPath;

    bool Load(const wchar_t* path) override {
        ++loadCalls;
        lastPath = path != nullptr ? path : L"";
        return loadAccepts;
    }
    void Unload() override { ++unloadCalls; }
    // Recorded, because "the refusal did not reach the transport" is the whole assertion: a
    // SetRate that returns false but still called Play has refused nothing.
    uint32_t playCalls = 0;
    float lastPlayRate = 0.0f;
    void Play(float r) override {
        ++playCalls;
        lastPlayRate = r;
    }
    void Pause() override {}
    // The seam the owner's "reversing or setting speed unpauses" needed: a rate change that is not
    // a play command. Counted separately so a Play that sneaks back in cannot pass as one.
    uint32_t setRateCalls = 0;
    float lastSetRate = 0.0f;
    void SetRate(float r) override {
        ++setRateCalls;
        lastSetRate = r;
    }
    void Step(int) override {}
    void Seek(int64_t tUs) override {
        ++seekCalls;
        lastSeekUs = tUs;
    }
    bool SeekRound(RoundNumber) override { return false; }
    void SetMode(bool) override {}
    TransportState state = TransportState::Pause;
    float rate = 1.0f;
    TransportState State() const override { return state; }
    float Rate() const override { return rate; }
    int64_t PlayheadUs() const override { return 0; }
    uint64_t DurationUs() const override { return 2000000; }
    const char* ActiveModeLabel() const override { return "fake"; }
};

// The camera row's counterpart. Records every SetMode so a test can say which selector reached
// the rig, and reports the last accepted mode back as the requested one, the way the real one does.
class FakeCamera final : public IReplayCameraControl {
public:
    CameraMode requested = CameraMode::Follow;
    std::vector<std::pair<CameraMode, int32_t>> sets;

    bool SetMode(CameraMode mode, int32_t num) override {
        sets.emplace_back(mode, num);
        requested = mode;
        return true;
    }
    CameraMode RequestedMode() const override { return requested; }
    bool ObservedMode(CameraMode*) const override { return false; }
    bool ObservedSelector(int32_t*, uint16_t*) const override { return false; }
    bool ActiveLevelCameraSet(uint16_t*) const override { return false; }
    bool BeginOracleTrack() override { return false; }
    void EndOracleTrack() override {}
    bool OracleTrackActive() const override { return false; }
    bool Available() const override { return true; }
    ControlRefusal LastRefusal() const override { return ControlRefusal::None; }
};

// ---- the fixture ------------------------------------------------------------------------------

// UI-3 moved every key this plugin reads into bar.cpp (F9 for the whole replay UI, F10 for the
// cursor alone), so the panel has no key of its own to drive. What these cases are about -- the
// toggle and the cursor claim that hangs off it -- is reached through the entry point the key
// itself reached: ui::Toggle.
void PressKey() { ui::Toggle(); }

struct Fixture {
    TempDir dir;
    // * NEVER ui::ReplayDirectory(). See scratch_capture_dir.h: asking the product where captures
    // live is how this suite put 34 fixture files in the owner's own capture folder, twice.
    std::wstring captureDir{testscratch::TestScratchCaptureDir(dir.Path())};
    testscratch::ScratchConfig config{captureDir};
    ui::DemoIndex index;
    ReplayServices services{};
    FakeSession session;
    FakeTransport transport;

    Fixture() {
        // Points paths:: at the scratch tree, so ReplayDirectory() and the log land there rather
        // than anywhere near a real install.
        const std::wstring self = dir.File(L"plugins\\echoreplay.dll");
        ::CreateDirectoryW(dir.File(L"plugins").c_str(), nullptr);
        SetPathsForTest(self.c_str());
        ::CreateDirectoryW(captureDir.c_str(), nullptr);

        services.config = &config;
        services.transport = &transport;
        services.session = &session;
        // The browser's filter is a process-global preference that outlives a case. Put it back to
        // what ships at every fixture, or a case that turns it off silently decides what the ones
        // after it are testing.
        ui::panel::SetPlayableOnlyForTest(true);
        g_playerCountOverride = 1;

        for (size_t i = 0; i < static_cast<size_t>(ui::gui::Entry::Count); ++i) {
            uifake::SetHave(static_cast<ui::gui::Entry>(i), true);
        }
        uifake::SetReady(true);
        uifake::SetWindowGate(true);
        uifake::Reset();

        uifake::ResetVoip();
        ui::panel::Init(&services, &index);
        // ★ UI-2 P2 (AC-2.2): the panel is opt-in now -- the bar is what a default session draws --
        // and every case below is about the panel's OWN content. Switching the key on here is what
        // makes this file the proof that `ui.debug_panel=true` restores it verbatim; without it the
        // cases would be asserting against a window the product deliberately no longer opens.
        ui::SetDebugPanel(true);
        // ...and back down, because switching the key on also brings the window up (a debug window
        // that answered "on" and stayed invisible reads as a broken switch) while every case below
        // starts from the panel's own "hidden until F9" state.
        ui::ShowPanel(false);
        // The browser has to scan the directory the fixtures are written to. If these two ever
        // part company the cases below are writing somewhere the panel does not read -- which,
        // historically, was the owner's Documents\\EchoReplay.
        REQUIRE(std::wstring(ui::ReplayDirectory()) == captureDir);
    }

    ~Fixture() {
        ui::panel::Shutdown();
    }

    // One tick plus one draw, the way ui_module drives them.
    void Frame(uint64_t frameDeltaUs = 16666) {
        ui::panel::Sample(frameDeltaUs);
        uifake::Reset();
        ui::panel::Draw();
    }
};

}  // namespace

// =================================================================================================
// B0 -- panel::Draw's own answer. The REAL Draw, not a stand-in (AUDIT_F3J F3J-4).
// =================================================================================================

// * THIS CASE EXISTS BECAUSE THE OTHER FIXTURES SUPPLY THE ANSWER. tests/ui/test_inframe.cpp and
// the marshal cases below both drive a stand-in draw function that forwards Begin correctly by
// construction, so a panel.cpp that went back to discarding Begin's return would survive every one
// of them. Only the shipped panel::Draw can falsify that, and only against a shut frame.
TEST_CASE("the real panel::Draw reports Begin's refusal rather than a draw") {
    Fixture f;
    ui::ShowPanel(true);

    static uint8_t frameByte = 1;
    uifake::SetFrameByte(&frameByte);
    struct Unbind { ~Unbind() { uifake::SetFrameByte(nullptr); } } unbind;

    ui::panel::Sample(16666);
    uifake::Reset();
    CHECK(ui::panel::Draw() == ui::inframe::DrawOutcome::Drawn);
    CHECK(uifake::BeginCount() == 1);

    frameByte = 0;
    uifake::Reset();
    CHECK(ui::panel::Draw() == ui::inframe::DrawOutcome::FacadeRefused);
    CHECK(uifake::BeginCount() == 1);
    CHECK(uifake::BeginRefusals() == 1);
    CHECK(uifake::EndCount() == 1);        // balanced even on the refused path (ARCH-15 S2.2 risk 2)
    CHECK_FALSE(uifake::WindowOpened());
    CHECK(uifake::Texts().empty());        // refused means NO state, not a window we failed to notice

    ui::ShowPanel(false);
    CHECK(ui::panel::Draw() == ui::inframe::DrawOutcome::NotShowing);
}

// ** WHY Begin SAID NO, published (L5-RUN-5 G2). The launch that read `facade=3975` with
// `nobyte=0 nolatch=0 noctx=0` had no way to name a cause, and a counter that cannot name one
// sends the next launch looking in the wrong place. Only what the facade can ACTUALLY be asked is
// named here: `boundnoframe` is the residual (collapsed OR clipped OR a Begin outside the facade's
// own NewFrame pairing), and it keeps its own word rather than borrowing one of the three.
TEST_CASE("the real panel::Draw names the reason Begin refused, and does not guess it") {
    Fixture f;
    ui::ShowPanel(true);

    static uint8_t frameByte = 0;
    uifake::SetFrameByte(&frameByte);
    struct Unbind { ~Unbind() { uifake::SetFrameByte(nullptr); } } unbind;

    ui::panel::Sample(16666);
    uifake::Reset();
    CHECK(ui::panel::Draw() == ui::inframe::DrawOutcome::FacadeRefused);

    // Everything panel::Draw calls IS bound in this fixture, so the readable causes are excluded
    // and the honest answer is the residual -- not "collapsed", which nothing here can see.
    const replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    CHECK(r.beginRefusal == replay::ui::inframe::BeginRefusal::BoundNoFrame);
    CHECK(strcmp(replay::ui::inframe::BeginRefusalName(r.beginRefusal), "boundnoframe") == 0);
    CHECK(r.facadeEntryCount == static_cast<uint32_t>(replay::ui::gui::Entry::Count));
    CHECK(r.facadeBoundEntries == replay::ui::gui::BoundCount());
    CHECK(strstr(replay::ui::csregister::Diagnostic(), "reason=boundnoframe bound=") != nullptr);

    ui::ShowPanel(false);
}

// =================================================================================================
// B1 -- the session-entry consult. Every case here fails if LoadPath stops asking.
// =================================================================================================

TEST_CASE("a capture that recorded no build fingerprint is not called a mismatch") {
    uint8_t expected[16];
    for (int i = 0; i < 16; ++i) expected[i] = static_cast<uint8_t>(0x10 + i);

    // ★ THE SHIPPED FALSE ALARM (L3-RUN-10 F3). Every capture this machine has written carries
    // sixteen zero bytes here -- nothing fills the field yet -- and comparing them byte-for-byte
    // banners CONTENT MISMATCH against the very build that recorded them.
    const uint8_t unrecorded[16] = {};
    CHECK(ui::ClassifyBuildMatch(unrecorded, expected) == ui::BuildMatch::NotRecorded);

    // ...and the banner that matters still fires.
    uint8_t other[16];
    memcpy(other, expected, sizeof other);
    other[15] = static_cast<uint8_t>(other[15] ^ 0xFF);
    CHECK(ui::ClassifyBuildMatch(other, expected) == ui::BuildMatch::Mismatch);
    CHECK(ui::ClassifyBuildMatch(expected, expected) == ui::BuildMatch::Matches);

    // No fingerprint of our own is not a match either: nothing was compared.
    CHECK(ui::ClassifyBuildMatch(expected, nullptr) == ui::BuildMatch::NotChecked);
}

TEST_CASE("a capture in a level this session cannot reach is REFUSED, not loaded") {
    Fixture fx;
    const std::wstring cap = fx.dir.File(L"other_level.echodemo");
    REQUIRE(WriteCapture(cap.c_str(), 0xAAAA, Whole(0xBBBB), 2));

    fx.session.levelId = 0x1111;      // the session is somewhere else
    fx.session.gametypeId = Whole(0x2222);
    fx.session.canChange = false;

    CHECK_FALSE(ui::LoadPath(cap.c_str()));

    // ★ THE ARM. Delete the consult from LoadPath and the transport is handed the file anyway --
    // which is exactly the shipped defect: the capture loads, the timeline populates, every control
    // works, and the world is the wrong level.
    CHECK(fx.transport.loadCalls == 0);
    CHECK(ui::LastEntryVerdict() == ui::EntryVerdict::Refused);
    CHECK(ui::LoadedPath() == nullptr);
    // Refused with a NAMED reason, not a bare false.
    CHECK(strstr(ui::LastEntryReason(), "cannot change level") != nullptr);
}

TEST_CASE("a capture whose level matches loads, and asks for no entry") {
    Fixture fx;
    const std::wstring cap = fx.dir.File(L"same_level.echodemo");
    REQUIRE(WriteCapture(cap.c_str(), 0x1111, Whole(0x2222), 3));

    fx.session.levelId = 0x1111;
    fx.session.gametypeId = Whole(0x2222);

    CHECK(ui::LoadPath(cap.c_str()));
    CHECK(fx.transport.loadCalls == 1);
    CHECK(fx.session.matchCalls > 0);          // it really did ask
    CHECK(fx.session.requestCalls == 0);       // and did not tear the session down for nothing
    CHECK(ui::LastEntryVerdict() == ui::EntryVerdict::Matches);
    CHECK(ui::RoundCount() == 3);
}

// ---- ENG-SHIP-PATH: the gametype the load funnel enters with ------------------------------------
namespace {

constexpr uint64_t kEchoArena = 0xCB60A4DE7E1CAF73ull;
constexpr uint64_t kEchoCombat = 0x3D5C3976578A321Aull;

// session.gametype_override switched off, everything else the scratch config's answer.
struct OverrideOffConfig final : testscratch::ScratchConfig {
    using testscratch::ScratchConfig::ScratchConfig;
    bool GetBool(const char* key, bool dflt) const override {
        if (strcmp(key, "session.gametype_override") == 0) return false;
        return testscratch::ScratchConfig::GetBool(key, dflt);
    }
};

std::string ReadWholeFile(const std::wstring& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Loads `recorded` into a session that is elsewhere and can change level; hands back what the
// session was asked to enter with, and the log the load wrote.
struct EntryProbe {
    uint64_t requested = 0;
    uint32_t ownerStateReads = 0;
    std::string log;
};

EntryProbe LoadRecordedGametype(Fixture& fx, const wchar_t* name, uint64_t recorded,
                                uint64_t ownerState) {
    const std::wstring cap = fx.dir.File(name);
    REQUIRE(WriteCapture(cap.c_str(), 0xFEED, recorded, 1));
    fx.session.levelId = 0x1;
    fx.session.gametypeId = Whole(0x2);
    fx.session.canChange = true;
    uifake::SetOwnerStateGametype(ownerState);

    const std::wstring logPath = fx.dir.File(L"entry_gametype.log");
    replay::log::RedirectForTest(logPath.c_str());
    CHECK(ui::LoadPath(cap.c_str()));
    replay::log::EndRedirectForTest();

    EntryProbe probe;
    probe.requested = fx.session.requestedGametype;
    probe.ownerStateReads = uifake::OwnerStateReads();
    probe.log = ReadWholeFile(logPath);
    uifake::SetOwnerStateGametype(0);
    return probe;
}

}  // namespace

TEST_CASE("entry gametype: a whole recorded gametype reaches the session untouched, file unread") {
    Fixture fx;
    // The owner state says ARENA; a whole recorded COMBAT must not even ask.
    const EntryProbe p = LoadRecordedGametype(fx, L"gt_whole.echodemo", kEchoCombat, kEchoArena);
    CHECK(p.requested == kEchoCombat);
    CHECK(p.ownerStateReads == 0);
    CHECK(p.log.find("entry gametype: rule manifest-64bit, recorded 0x3D5C3976578A321A -> "
                     "entering 0x3D5C3976578A321A (echo_combat)") != std::string::npos);
}

TEST_CASE("entry gametype: a recorded low dword enters as the whole hash -- combat stays combat") {
    Fixture fx;
    const EntryProbe p =
        LoadRecordedGametype(fx, L"gt_low.echodemo", kEchoCombat & 0xFFFFFFFFull, kEchoArena);  // facts-ok: PHYS.ID_DEAD a u32 mask over a packed id; PHYS.ID_DEAD is PC_PRESENT and GOAL.USERID_UNSET is unmerged, so neither emits a constant
    // THE ARM. Hand the session m->gametypeId again and this is 0x578A321A; let the fallback run
    // first and a combat capture enters as arena.
    CHECK(p.requested == kEchoCombat);
    CHECK(p.ownerStateReads == 0);
    CHECK(p.log.find("entry gametype: rule low-dword-match, recorded 0x00000000578A321A -> "
                     "entering 0x3D5C3976578A321A (echo_combat)") != std::string::npos);
}

TEST_CASE("entry gametype: the owner's real 0xFFFFFFFF is recovered from the capture's own event") {
    Fixture fx;
    // COMBAT from the event, so the arena fallback cannot be what answered.
    const EntryProbe p =
        LoadRecordedGametype(fx, L"gt_event.echodemo", 0x00000000FFFFFFFFull, kEchoCombat);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    CHECK(p.requested == kEchoCombat);
    CHECK(p.ownerStateReads == 1);
    CHECK(p.log.find("entry gametype: rule owner-state-event, recorded 0x00000000FFFFFFFF -> "
                     "entering 0x3D5C3976578A321A (echo_combat)") != std::string::npos);
}

TEST_CASE("entry gametype: with no event either, the fallback name; and the key switches it all off") {
    {
        Fixture fx;
        const EntryProbe p =
            LoadRecordedGametype(fx, L"gt_fallback.echodemo", 0x00000000FFFFFFFFull, 0);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
        CHECK(p.requested == kEchoArena);
        CHECK(p.ownerStateReads == 1);
        CHECK(p.log.find("entry gametype: rule fallback-name, recorded 0x00000000FFFFFFFF -> "
                         "entering 0xCB60A4DE7E1CAF73 (echo_arena)") != std::string::npos);
    }
    {
        Fixture fx;
        OverrideOffConfig off{fx.captureDir};
        fx.services.config = &off;
        ui::panel::Init(&fx.services, &fx.index);
        const EntryProbe p =
            LoadRecordedGametype(fx, L"gt_off.echodemo", 0x00000000FFFFFFFFull, kEchoCombat);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
        CHECK(p.requested == 0x00000000FFFFFFFFull);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
        CHECK(p.ownerStateReads == 0);
        CHECK(p.log.find("session.gametype_override is off") != std::string::npos);
        fx.services.config = &fx.config;
    }
}

TEST_CASE("a strategy that can change level gets RequestEntry, with the capture's own ids") {
    Fixture fx;
    const std::wstring cap = fx.dir.File(L"elsewhere.echodemo");
    REQUIRE(WriteCapture(cap.c_str(), 0xFEEDu, Whole(0xF00Du), 1));

    fx.session.levelId = 0x1;
    fx.session.gametypeId = Whole(0x2);
    fx.session.canChange = true;
    fx.session.requestAccepts = true;

    CHECK(ui::LoadPath(cap.c_str()));

    // ★ THE ARM for the only mechanism in the tree that puts the game into a capture's level.
    CHECK(fx.session.requestCalls == 1);
    CHECK(fx.session.requestedLevel == 0xFEEDu);
    CHECK(fx.session.requestedGametype == Whole(0xF00Du));
    CHECK(ui::LastEntryVerdict() == ui::EntryVerdict::Requested);
    CHECK(fx.transport.loadCalls == 1);
}

TEST_CASE("a refused entry request refuses the load too") {
    Fixture fx;
    const std::wstring cap = fx.dir.File(L"refused_entry.echodemo");
    REQUIRE(WriteCapture(cap.c_str(), 0x77, Whole(0x88), 0));

    fx.session.levelId = 0x1;
    fx.session.gametypeId = Whole(0x2);
    fx.session.canChange = true;
    fx.session.requestAccepts = false;

    CHECK_FALSE(ui::LoadPath(cap.c_str()));
    CHECK(fx.session.requestCalls == 1);
    CHECK(fx.transport.loadCalls == 0);
    CHECK(ui::LastEntryVerdict() == ui::EntryVerdict::RequestFailed);
}

TEST_CASE("a requested entry that never lands says so on screen rather than looking fine") {
    Fixture fx;
    const std::wstring cap = fx.dir.File(L"pending.echodemo");
    REQUIRE(WriteCapture(cap.c_str(), 0x33, Whole(0x44), 0));

    fx.session.levelId = 0x1;
    fx.session.gametypeId = Whole(0x2);
    fx.session.canChange = true;
    fx.session.matches = false;      // the tick never performs the entry

    REQUIRE(ui::LoadPath(cap.c_str()));
    CHECK(ui::LastEntryVerdict() == ui::EntryVerdict::Requested);

    // Thirty seconds of frames at 60 Hz, accumulated from frameDeltaUs rather than differenced from
    // an absolute clock -- the sim clock reads zero until the engine's own resolves.
    for (int i = 0; i < 1900; ++i) fx.Frame(16666);

    CHECK(strstr(ui::LastEntryReason(), "NOT this capture's world") != nullptr);
    CHECK(uifake::TextContains("WORLD"));
}

TEST_CASE("a requested entry that lands clears the warning") {
    Fixture fx;
    const std::wstring cap = fx.dir.File(L"lands.echodemo");
    REQUIRE(WriteCapture(cap.c_str(), 0x33, Whole(0x44), 0));

    fx.session.levelId = 0x1;
    fx.session.gametypeId = Whole(0x2);
    fx.session.canChange = true;
    fx.session.matches = false;

    REQUIRE(ui::LoadPath(cap.c_str()));
    CHECK(ui::LastEntryVerdict() == ui::EntryVerdict::Requested);
    fx.Frame();
    CHECK(ui::LastEntryVerdict() == ui::EntryVerdict::Requested);

    fx.session.matches = true;       // the tick performed it; RequestEntry already moved the ids
    fx.Frame();
    CHECK(ui::LastEntryVerdict() == ui::EntryVerdict::Matches);
    CHECK_FALSE(uifake::TextContains("WORLD"));
}

TEST_CASE("with no ISessionEntry the load is NOT CHECKED, and does not claim otherwise") {
    Fixture fx;
    fx.services.session = nullptr;
    const std::wstring cap = fx.dir.File(L"nosession.echodemo");
    REQUIRE(WriteCapture(cap.c_str(), 0x5, Whole(0x6), 0));

    CHECK(ui::LoadPath(cap.c_str()));
    CHECK(ui::LastEntryVerdict() == ui::EntryVerdict::NotChecked);
    CHECK(strstr(ui::LastEntryReason(), "UNKNOWN") != nullptr);
    fx.Frame();
    CHECK(uifake::TextContains("WORLD"));
}

TEST_CASE("a refusal leaves the capture that is playing alone") {
    Fixture fx;
    const std::wstring good = fx.dir.File(L"playing.echodemo");
    const std::wstring bad = fx.dir.File(L"unreachable.echodemo");
    REQUIRE(WriteCapture(good.c_str(), 0x1111, Whole(0x2222), 2));
    REQUIRE(WriteCapture(bad.c_str(), 0x9999, Whole(0x8888), 4));

    fx.session.levelId = 0x1111;
    fx.session.gametypeId = Whole(0x2222);
    REQUIRE(ui::LoadPath(good.c_str()));
    REQUIRE(ui::LoadedPath() != nullptr);
    const std::wstring loaded = ui::LoadedPath();
    const size_t rounds = ui::RoundCount();

    CHECK_FALSE(ui::LoadPath(bad.c_str()));
    // Nothing about the capture on screen may have moved: name, markers and path all still describe
    // the file that is still playing.
    CHECK(std::wstring(ui::LoadedPath()) == loaded);
    CHECK(ui::RoundCount() == rounds);
}

// =================================================================================================
// THE AXIS AT THE TRANSPORT SEAM.
//
// FakeTransport above answers PlayheadUs() with 0 and DurationUs() with a length, so it has no
// ORIGIN at all -- which is the same blindness as a fixture whose t0Us is zero, wearing a different
// hat. AxisTransport is the fake that HAS one.
//
// INV-AXIS (W2-B, 3f7ca26; stated as checkable clauses in playback/core/playback.h) rules this seam
// 0-BASED. Everything crossing IReplayTransport is microseconds since the start of playback, the
// file stays absolute, and the conversion is two members and no others:
//
//   playback_module.cpp:322  Seek()       SeekTo(tUs + Origin())         relative in, absolute down
//   playback_module.cpp:363  PlayheadUs() DisplayPlayheadUs() - Origin() absolute up, relative out
//
// AxisTransport models that SHAPE rather than those answers: it keeps an absolute playhead over the
// tick table's own span and converts at exactly those two members. So a panel that reintroduces a
// third conversion above the seam lands out of the file instead of agreeing by luck.
//
// What these four assert is that a click lands where it was aimed, which is true under either
// candidate axis -- that is why they were written before the axis was settled and kept once it was.
// They were skipped only while the two halves of the seam disagreed, because a fake modelling the
// old contract tests the fake. The origin is 1.7e15 us against a capture 6e8 long, so a wrong-axis
// answer is off by a thousand times the file's own length and cannot land inside it looking
// plausible.
// =================================================================================================

namespace {

class AxisTransport final : public IReplayTransport {
public:
    static constexpr int64_t kT0 = 1700000000000000ll;   // a real session's origin, not zero
    static constexpr int64_t kLength = 600000000ll;      // ten minutes
    // The origin has to be bigger than the whole capture or a wrong-axis answer still lands in
    // range and reads as plausible -- which is the shape that hid this defect behind 31 green cases.
    static_assert(kT0 > kLength, "a wrong-axis answer must fall outside [0, kLength]");

    // ABSOLUTE, like the tick table the real transport snaps into: it spans [kT0, kT0 + kLength].
    // Nothing outside this class reads it except an assertion asking where in the FILE a click went.
    int64_t absoluteUs = kT0;

    bool Load(const wchar_t*) override {
        absoluteUs = kT0;   // PlayheadUs() == 0: the first recorded frame
        return true;
    }
    void Unload() override {}
    void Play(float) override {}
    void Pause() override {}
    void Step(int) override {}
    // Conversion 1 of 2, then SnapToTarget's clamp into the recorded tick table.
    void Seek(int64_t tUs) override {
        const int64_t target = kT0 + tUs;
        absoluteUs = target < kT0 ? kT0 : (target > kT0 + kLength ? kT0 + kLength : target);
    }
    bool SeekRound(RoundNumber) override { return false; }
    void SetMode(bool) override {}
    TransportState State() const override { return TransportState::Pause; }
    float Rate() const override { return 1.0f; }
    int64_t PlayheadUs() const override { return absoluteUs - kT0; }   // conversion 2 of 2
    uint64_t DurationUs() const override { return kLength; }           // 0-BASED LENGTH
    const char* ActiveModeLabel() const override { return "axis"; }
};

}  // namespace

TEST_CASE("clicking the end of the scrub bar reaches the end of the capture") {
    Fixture fx;
    AxisTransport axis;
    fx.services.transport = &axis;
    const std::wstring cap = fx.dir.File(L"axis_end.echodemo");
    REQUIRE(WriteCapture(cap.c_str(), 0x1111, Whole(0x2222), 0));
    fx.session.levelId = 0x1111;
    fx.session.gametypeId = Whole(0x2222);
    REQUIRE(ui::LoadPath(cap.c_str()));

    fx.Frame();
    // The last cell of the whole-capture strip is the end of the capture, whatever axis that is.
    const uint32_t cells = ui::CellCount(690.0f, 6.0f);
    char last[24];
    snprintf(last, sizeof last, "##c%u", cells - 1);
    ui::panel::Sample(16666);
    uifake::Reset();
    uifake::ClickOnce(last);
    ui::panel::Draw();

    CHECK(axis.PlayheadUs() == AxisTransport::kLength);
    CHECK(axis.absoluteUs == AxisTransport::kT0 + AxisTransport::kLength);
}

TEST_CASE("SeekFraction(0.5) lands on the middle of the capture") {
    Fixture fx;
    AxisTransport axis;
    fx.services.transport = &axis;
    const std::wstring cap = fx.dir.File(L"axis_half.echodemo");
    REQUIRE(WriteCapture(cap.c_str(), 0x1111, Whole(0x2222), 0));
    fx.session.levelId = 0x1111;
    fx.session.gametypeId = Whole(0x2222);
    REQUIRE(ui::LoadPath(cap.c_str()));
    fx.Frame();

    REQUIRE(ui::SeekFraction(0.5));
    CHECK(axis.PlayheadUs() == AxisTransport::kLength / 2);
    CHECK(axis.absoluteUs == AxisTransport::kT0 + AxisTransport::kLength / 2);
}

TEST_CASE("The tick interval is published only once a frame has been measured") {
    Fixture fx;
    // The state that MEASURES: with no capture loaded the engine's clock is its own and
    // its delta IS the raw frame period, which is the row Sample() names first.
    fx.transport.state = TransportState::NotLoaded;
    uint64_t periodUs = 12345;
    // Nothing measured yet: FALSE, and the out-parameter is left alone, so a caller that ignored
    // the answer cannot pass its own initialiser off as a measurement.
    CHECK_FALSE(ui::TickIntervalUs(periodUs));
    CHECK(periodUs == 12345);
    CHECK(ui::RateCeiling() == 0.0f);

    fx.Frame(16666);
    REQUIRE(ui::TickIntervalUs(periodUs));
    CHECK(periodUs == 16666);
    // One measurement, two published numbers: the ceiling this panel reports is the one computed
    // from exactly this interval, so two consumers cannot be reading two different periods.
    CHECK(ui::RateCeiling() ==
          doctest::Approx(ui::RateCeiling(facts::CLOCK_MAXELAPSED_US, periodUs)));
}

TEST_CASE("a bookmark jump lands on that bookmark") {
    Fixture fx;
    AxisTransport axis;
    fx.services.transport = &axis;
    const std::wstring cap = fx.dir.File(L"axis_marks.echodemo");
    REQUIRE(WriteCapture(cap.c_str(), 0x1111, Whole(0x2222), 0));
    fx.session.levelId = 0x1111;
    fx.session.gametypeId = Whole(0x2222);
    REQUIRE(ui::LoadPath(cap.c_str()));

    // W2-G pushes UNCONVERTED: `mark` records exactly PlayheadUs(), so a bookmark is on whatever
    // axis the playhead is on -- 0-based since INV-AXIS. Three minutes into the capture.
    const int64_t markAt = 180000000ll;
    const ui::BookmarkView marks[] = {{markAt, "the goal"}};
    ui::SetBookmarks(ui::NotesState::Ok, marks, 1, 0);

    fx.Frame();
    ui::panel::Sample(16666);
    uifake::Reset();
    uifake::ClickOnce("##bm0");
    ui::panel::Draw();

    CHECK(axis.PlayheadUs() == markAt);
    CHECK(axis.absoluteUs == AxisTransport::kT0 + markAt);
}

TEST_CASE("the playhead renders inside the strip it is drawn on") {
    Fixture fx;
    AxisTransport axis;
    fx.services.transport = &axis;
    const std::wstring cap = fx.dir.File(L"axis_head.echodemo");
    REQUIRE(WriteCapture(cap.c_str(), 0x1111, Whole(0x2222), 0));
    fx.session.levelId = 0x1111;
    fx.session.gametypeId = Whole(0x2222);
    REQUIRE(ui::LoadPath(cap.c_str()));
    axis.Seek(AxisTransport::kLength / 2);
    fx.Frame();

    // ClassifyCells is handed g.playheadUs and the whole-capture Span together; if they are on
    // different axes the playhead is outside the span and no cell is ever classified as it.
    const uint32_t cells = ui::CellCount(690.0f, 6.0f);
    const ui::Span whole = {0, static_cast<int64_t>(AxisTransport::kLength)};
    ui::Marker none[1] = {};
    ui::CellClass classes[ui::kMaxCells] = {};
    ui::ClassifyCells(whole, cells, none, 0, axis.PlayheadUs(), true, classes);
    size_t playheadCells = 0;
    uint32_t playheadCell = cells;
    for (uint32_t i = 0; i < cells; ++i) {
        if (classes[i] == ui::CellClass::Playhead || classes[i] == ui::CellClass::MarkerPlayhead) {
            ++playheadCells;
            playheadCell = i;
        }
    }
    CHECK(playheadCells == 1);
    // Drawn is not enough: it has to be drawn where it was seeked. An axis that is wrong by less
    // than one cell still draws.
    CHECK(playheadCell == ui::CellOfUs(whole, cells, AxisTransport::kLength / 2));
}

// =================================================================================================
// The offline-bit-lock guard W2-F.probe.json declares. The probe itself only runs in a launch, so
// what is provable here is the PREDICATE it decides on -- and the predicate is where the dangerous
// mutant lives.
// =================================================================================================

TEST_CASE("only Locked and AlreadyLockedInProcess count as the offline lock being held") {
    using replay::offlinelock::LockState;
    using replay::uiprobe::OfflineLockHeld;

    // Held. The second is what a module that lost the install race reports about a process that IS
    // locked by another module, so refusing it would refuse a protected launch.
    CHECK(OfflineLockHeld(LockState::Locked));
    CHECK(OfflineLockHeld(LockState::AlreadyLockedInProcess));

    // ★ THE DISCRIMINATING SUBSET, and the reason this is a disjunction rather than
    // `state != NotAttempted`. Every RefusedBy* value is a state in which the lock was ATTEMPTED
    // and is NOT held: an inequality would pass all six of these and wave through an -offline
    // launch whose network-safety property was never installed.
    const LockState notHeld[] = {
        LockState::NotAttempted,    LockState::RefusedByConfig,  LockState::RefusedNoImage,
        LockState::RefusedNoSites,  LockState::RefusedSiteSpread, LockState::RefusedSelfTest,
        LockState::RefusedByMinHook,
    };
    size_t exercised = 0;
    for (LockState s : notHeld) {
        CHECK_FALSE(OfflineLockHeld(s));
        ++exercised;
    }
    CHECK(exercised == 7);

    // The whole enum, so a value added later cannot default to "held" unnoticed: exactly two of the
    // nine defined states are held, and nothing outside the range is.
    size_t held = 0;
    for (int i = 0; i <= static_cast<int>(LockState::RefusedByMinHook); ++i) {
        if (OfflineLockHeld(static_cast<LockState>(i))) ++held;
    }
    CHECK(held == 2);
    CHECK_FALSE(OfflineLockHeld(static_cast<LockState>(200)));
}

// =================================================================================================
// B3 -- replayed voice. The player waits on one call from whoever opened the file, and had none.
// =================================================================================================

TEST_CASE("a loaded capture's path reaches the voice player") {
    Fixture fx;
    const std::wstring cap = fx.dir.File(L"withvoice.echodemo");
    REQUIRE(WriteCapture(cap.c_str(), 0x1111, Whole(0x2222), 0));
    fx.session.levelId = 0x1111;
    fx.session.gametypeId = Whole(0x2222);

    CHECK(uifake::VoipPathCalls() == 0);
    REQUIRE(ui::LoadPath(cap.c_str()));

    // ★ THE ARM. Without this call VoipTick returns at its Loaded() check on every frame for the
    // life of the process: voice is recorded into the file and never comes back out.
    CHECK(uifake::VoipPathCalls() == 1);
    CHECK(std::wstring(uifake::VoipDemoPath()) == cap);

    ui::Unload();
    // The null arm of the same seam is the unload; voice must not outlive its capture.
    CHECK(uifake::VoipPathClears() == 1);
    CHECK(uifake::VoipDemoPath()[0] == L'\0');
}

TEST_CASE("voice is not handed a capture the session refused") {
    Fixture fx;
    const std::wstring cap = fx.dir.File(L"unreachable_voice.echodemo");
    REQUIRE(WriteCapture(cap.c_str(), 0x9999, Whole(0x9999), 0));
    fx.session.levelId = 0x1111;
    fx.session.gametypeId = Whole(0x2222);
    fx.session.canChange = false;

    CHECK_FALSE(ui::LoadPath(cap.c_str()));
    // Handing voice a file the rest of the system rejected would make the one thing that IS playing
    // the wrong capture's audio.
    CHECK(uifake::VoipPathCalls() == 0);
}

TEST_CASE("voice is not handed a capture the transport refused") {
    Fixture fx;
    const std::wstring cap = fx.dir.File(L"transport_refused.echodemo");
    REQUIRE(WriteCapture(cap.c_str(), 0x1111, Whole(0x2222), 0));
    fx.session.levelId = 0x1111;
    fx.session.gametypeId = Whole(0x2222);
    fx.transport.loadAccepts = false;

    CHECK_FALSE(ui::LoadPath(cap.c_str()));
    CHECK(uifake::VoipPathCalls() == 0);
    // After a refused Load what the transport still holds is unknown, so voice is dropped rather
    // than left playing over whatever survived.
    CHECK(uifake::VoipPathClears() == 1);
}

// =================================================================================================
// B4 -- the browser's use-after-free.
// =================================================================================================

TEST_CASE("AddOne rebinds the borrowed manifest when the row vector reallocates") {
    TempDir dir;
    const std::wstring cap = dir.File(L"first.echodemo");
    REQUIRE(WriteCapture(cap.c_str(), 0xC0FFEEull, Whole(0xD00Dull), 2));

    ui::DemoIndex index;
    REQUIRE(index.AddOne(cap.c_str()) == 0);
    REQUIRE(index.LoadManifest(0));
    REQUIRE(index.At(0).manifest != nullptr);

    // The manifest is a subobject of the Row that owns the summary, so their distance is fixed by
    // the layout. A reallocation moves the summary; a manifest pointer left behind at the old
    // address changes that distance, which is a check that does not depend on reading freed memory
    // and getting a plausible answer back.
    auto Distance = [&]() {
        return reinterpret_cast<const char*>(index.At(0).manifest) -
               reinterpret_cast<const char*>(&index.At(0));
    };
    const ptrdiff_t expected = Distance();
    const void* before = &index.At(0);
    const uint64_t level = index.At(0).manifest->levelId;

    // ★ FORCE the reallocation rather than trusting a size that happens not to grow: MSVC grows a
    // vector by 1.5x, so sixty-four appends cross several capacity boundaries from any start.
    for (int i = 0; i < 64; ++i) {
        wchar_t p[MAX_PATH];
        _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%s\\absent_%d.echodemo", dir.Path(), i);
        index.AddOne(p);
        CHECK(Distance() == expected);
    }

    // The arm is only worth anything if the storage actually moved under it.
    CHECK(&index.At(0) != before);
    CHECK(index.At(0).manifest->levelId == level);
    CHECK(index.Count() == 65);
}

TEST_CASE("a row whose manifest was never read borrows no manifest at all") {
    TempDir dir;
    ui::DemoIndex index;
    wchar_t p[MAX_PATH];
    _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%s\\missing.echodemo", dir.Path());
    REQUIRE(index.AddOne(p) == 0);
    // NULL is what the browser's "not checked" state is keyed on; a stale non-null here would make
    // an unread row render as a checked one.
    CHECK(index.At(0).manifest == nullptr);
    CHECK_FALSE(index.ManifestLoaded(0));
}

// =================================================================================================
// The owner's bookmarks. `mark` wrote a sidecar nothing in the product could ever read back; W2-G
// built the reader, and this is the half that puts them on the timeline he made them on.
// =================================================================================================

namespace {

// Loads a capture and returns with the panel drawing it, so a bookmark case starts where the owner
// would be: a capture open, the transport reporting a duration, the timeline up.
void LoadForBookmarks(Fixture& fx, const wchar_t* leaf) {
    const std::wstring cap = fx.dir.File(leaf);
    REQUIRE(WriteCapture(cap.c_str(), 0x1111, Whole(0x2222), 0));
    fx.session.levelId = 0x1111;
    fx.session.gametypeId = Whole(0x2222);
    REQUIRE(ui::LoadPath(cap.c_str()));
}

}  // namespace

TEST_CASE("with nothing pushed the panel says UNKNOWN, never 'no bookmarks'") {
    Fixture fx;
    LoadForBookmarks(fx, L"nomarks.echodemo");
    CHECK(ui::BookmarksState() == ui::NotesState::NotSupplied);
    fx.Frame();
    // ★ The whole point. "Nobody told us" rendering as "you have none" would tell the owner his
    // marks are gone, which is the false all-clear in the one place it costs him data he made.
    CHECK(uifake::TextContains("NOT SUPPLIED"));
    CHECK_FALSE(uifake::TextContains("none yet"));
}

TEST_CASE("pushed bookmarks are drawn, and clicking one seeks to it") {
    Fixture fx;
    LoadForBookmarks(fx, L"marked.echodemo");

    const ui::BookmarkView marks[] = {
        {250000, "first save"},
        {900000, ""},
        {1500000, "the goal"},
    };
    ui::SetBookmarks(ui::NotesState::Ok, marks, 3, 0);
    CHECK(ui::BookmarksState() == ui::NotesState::Ok);
    CHECK(ui::BookmarkCount() == 3);

    fx.Frame();
    // ★ THE ARM for the display half: drawn at all, numbered the way the console numbers them, and
    // carrying the owner's own words.
    CHECK(uifake::ButtonDrawn("B1"));
    CHECK(uifake::ButtonDrawn("B2"));
    CHECK(uifake::ButtonDrawn("B3"));
    CHECK(uifake::ButtonDrawn("first save"));
    CHECK(uifake::ButtonDrawn("the goal"));

    // ★ THE ARM for the jump. Without it the marks are a picture.
    ui::panel::Sample(16666);
    uifake::Reset();
    uifake::ClickOnce("##bm2");
    ui::panel::Draw();
    CHECK(fx.transport.lastSeekUs == 1500000);
}

TEST_CASE("the resume point is offered and never applied behind the owner's back") {
    Fixture fx;
    LoadForBookmarks(fx, L"resume.echodemo");
    ui::SetBookmarks(ui::NotesState::Ok, nullptr, 0, 0);

    const ui::BookmarkView one[] = {{100000, "x"}};
    ui::SetBookmarks(ui::NotesState::Ok, one, 1, 640000);
    fx.Frame();
    CHECK(uifake::ButtonDrawn("Resume"));
    // Loading did not move the playhead: a transport that seeks because a file said so is doing
    // something nobody asked for on that frame.
    CHECK(fx.transport.seekCalls == 0);

    ui::panel::Sample(16666);
    uifake::Reset();
    uifake::ClickOnce("##resume");
    ui::panel::Draw();
    CHECK(fx.transport.lastSeekUs == 640000);
}

TEST_CASE("a damaged sidecar is named, not rendered as empty") {
    Fixture fx;
    LoadForBookmarks(fx, L"damagedside.echodemo");
    ui::SetBookmarks(ui::NotesState::Damaged, nullptr, 0, 0);
    fx.Frame();
    CHECK(uifake::TextContains("does not parse"));
    CHECK(uifake::TextContains("left"));
    CHECK_FALSE(uifake::TextContains("none yet"));
}

TEST_CASE("an absent sidecar is a different sentence from a damaged one") {
    Fixture fx;
    LoadForBookmarks(fx, L"absentside.echodemo");
    ui::SetBookmarks(ui::NotesState::Absent, nullptr, 0, 0);
    fx.Frame();
    CHECK(uifake::TextContains("none yet"));
    CHECK_FALSE(uifake::TextContains("does not parse"));
}

TEST_CASE("bookmarks do not outlive the capture they belong to") {
    Fixture fx;
    LoadForBookmarks(fx, L"first_cap.echodemo");
    const ui::BookmarkView marks[] = {{250000, "mine"}};
    ui::SetBookmarks(ui::NotesState::Ok, marks, 1, 500000);
    REQUIRE(ui::BookmarkCount() == 1);

    // ★ Loading a second capture must not leave the first one's marks on the strip. That is the
    // wrong-file class LoadedPath() exists to prevent, arriving through a different door.
    LoadForBookmarks(fx, L"second_cap.echodemo");
    CHECK(ui::BookmarksState() == ui::NotesState::NotSupplied);
    CHECK(ui::BookmarkCount() == 0);
    fx.Frame();
    CHECK_FALSE(uifake::ButtonDrawn("mine"));
    CHECK(uifake::TextContains("NOT SUPPLIED"));

    ui::SetBookmarks(ui::NotesState::Ok, marks, 1, 500000);
    ui::Unload();
    CHECK(ui::BookmarksState() == ui::NotesState::NotSupplied);
    CHECK(ui::BookmarkCount() == 0);
}

TEST_CASE("more bookmarks than the panel draws are counted, not silently cut") {
    Fixture fx;
    LoadForBookmarks(fx, L"manymarks.echodemo");

    std::vector<ui::BookmarkView> many;
    for (int i = 0; i < 200; ++i) many.push_back(ui::BookmarkView{10000 + i * 5000, ""});
    ui::SetBookmarks(ui::NotesState::Ok, many.data(), many.size(), 0);

    // The supplied total is what the panel reports, not the number it managed to draw.
    CHECK(ui::BookmarkCount() == 200);
    fx.Frame();
    CHECK(uifake::TextContains("bookmarks (200)"));
    CHECK(uifake::TextContains("more bookmarks than this panel draws"));
}

TEST_CASE("a state carrying no table does not leave the previous table drawn") {
    Fixture fx;
    LoadForBookmarks(fx, L"swapstate.echodemo");
    const ui::BookmarkView marks[] = {{250000, "stale"}};
    ui::SetBookmarks(ui::NotesState::Ok, marks, 1, 0);
    REQUIRE(ui::BookmarkCount() == 1);

    // Ok-with-null and Damaged both carry nothing; neither may keep the old rows up.
    ui::SetBookmarks(ui::NotesState::Ok, nullptr, 5, 0);
    CHECK(ui::BookmarkCount() == 0);
    fx.Frame();
    CHECK_FALSE(uifake::ButtonDrawn("stale"));
}

// =================================================================================================
// O1 / O2 -- the switch, and the cursor it has to bring with it.
// =================================================================================================

TEST_CASE("the panel starts hidden and the toggle shows and hides it") {
    Fixture fx;
    CHECK(ui::PanelEnabled());
    CHECK_FALSE(ui::PanelVisible());

    fx.Frame();
    CHECK(uifake::BeginCount() == 0);   // nothing is drawn over ordinary gameplay

    PressKey();
    fx.Frame();
    CHECK(ui::PanelVisible());
    CHECK(uifake::BeginCount() == 1);

    // Drawing does not re-toggle it: only a press does.
    fx.Frame();
    CHECK(ui::PanelVisible());

    fx.Frame();
    CHECK(ui::PanelVisible());

    PressKey();
    fx.Frame();
    CHECK_FALSE(ui::PanelVisible());
    CHECK(uifake::BeginCount() == 0);
}

TEST_CASE("the panel takes the cursor while it is up and gives it back when it goes down") {
    Fixture fx;
    CHECK(uifake::CursorCalls() == 0);

    PressKey();
    ui::panel::Sample(16666);
    // ★ THE ARM for a control that was bound, guarded, counted -- and never called. Without the
    // call every button on this click-only panel is undrivable whenever the engine's cursor is off.
    CHECK(uifake::CursorCalls() == 1);
    CHECK(uifake::LastCursorArg() == 1);

    ui::panel::Sample(16666);
    CHECK(uifake::CursorCalls() == 1);   // no churn while nothing changed

    PressKey();
    ui::panel::Sample(16666);
    CHECK(uifake::CursorCalls() == 2);
    CHECK(uifake::LastCursorArg() == 0); // hidden again: the cursor is handed back
}

TEST_CASE("the cursor is released at shutdown rather than left on") {
    Fixture fx;
    ui::ShowPanel(true);
    ui::panel::Sample(16666);
    REQUIRE(uifake::LastCursorArg() == 1);
    ui::panel::Shutdown();
    CHECK(uifake::LastCursorArg() == 0);
}

TEST_CASE("an unbound SetCursorEnabled is reported rather than silently ignored") {
    Fixture fx;
    uifake::SetHave(ui::gui::Entry::SetCursorEnabled, false);
    ui::ShowPanel(true);
    fx.Frame();
    CHECK(uifake::TextContains("GUI.SETCURSORENABLED did not bind"));
    CHECK(uifake::CursorCalls() == 0);
    uifake::SetHave(ui::gui::Entry::SetCursorEnabled, true);
}

TEST_CASE("loading a capture brings the panel up") {
    Fixture fx;
    const std::wstring cap = fx.dir.File(L"shows.echodemo");
    REQUIRE(WriteCapture(cap.c_str(), 0x1111, Whole(0x2222), 0));
    fx.session.levelId = 0x1111;
    fx.session.gametypeId = Whole(0x2222);

    REQUIRE_FALSE(ui::PanelVisible());
    REQUIRE(ui::LoadPath(cap.c_str()));
    CHECK(ui::PanelVisible());
}

// =================================================================================================
// The browser preview -- the same decision, drawn as three states.
// =================================================================================================

TEST_CASE("the browser greys an unreachable row and marks an unchecked one") {
    Fixture fx;
    // The panel scans its own replay directory, so the captures go where it will look.
    ::CreateDirectoryW(fx.dir.File(L"echoreplay").c_str(), nullptr);
    wchar_t here[MAX_PATH], away[MAX_PATH], bad[MAX_PATH];
    _snwprintf_s(here, MAX_PATH, _TRUNCATE, L"%s\\a_here.echodemo", fx.captureDir.c_str());
    _snwprintf_s(away, MAX_PATH, _TRUNCATE, L"%s\\b_far.echodemo", fx.captureDir.c_str());
    _snwprintf_s(bad, MAX_PATH, _TRUNCATE, L"%s\\c_bad.echodemo", fx.captureDir.c_str());
    REQUIRE(WriteCapture(here, 0x1111, Whole(0x2222), 0));
    REQUIRE(WriteCapture(away, 0x9999, Whole(0x9999), 0));
    // A capture whose manifest can never be read: its level is unknowable, so it is unchecked
    // FOREVER rather than for a few frames -- which is the case that used to render as reachable.
    {
        HANDLE h = ::CreateFileW(bad, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        REQUIRE(h != INVALID_HANDLE_VALUE);
        char junk[256];
        memset(junk, 0x5A, sizeof junk);
        DWORD wrote = 0;
        ::WriteFile(h, junk, sizeof junk, &wrote, nullptr);
        ::CloseHandle(h);
    }

    fx.session.levelId = 0x1111;
    fx.session.gametypeId = Whole(0x2222);
    fx.session.canChange = false;

    ui::OpenBrowser();
    fx.Frame();
    // The listing is built off the owner thread now: the page exists one worker round trip later.
    REQUIRE(ui::panel::WaitForBrowserForTest(10000));
    for (int i = 0; i < 8; ++i) fx.Frame();

    CHECK(uifake::ButtonDrawn("a_here"));
    CHECK(uifake::ButtonDrawn("b_far"));
    CHECK(uifake::ButtonDrawn("c_bad"));
    // Refused is greyed; reachable is not; and "we never checked" is its own state rather than the
    // good one.
    CHECK(uifake::ButtonDisabled("b_far"));
    CHECK_FALSE(uifake::ButtonDisabled("a_here"));
    // The verdict rides the row's own line now -- one widget per row, whatever the verdict is.
    CHECK(uifake::ButtonDrawn("NOT CHECKED"));
}

TEST_CASE("clicking a browser row loads it through the same funnel") {
    Fixture fx;
    ::CreateDirectoryW(fx.dir.File(L"echoreplay").c_str(), nullptr);
    wchar_t row[MAX_PATH];
    _snwprintf_s(row, MAX_PATH, _TRUNCATE, L"%s\\clickme.echodemo", fx.captureDir.c_str());
    REQUIRE(WriteCapture(row, 0x4321, Whole(0x8765), 1));

    fx.session.levelId = 0x1;
    fx.session.gametypeId = Whole(0x2);
    fx.session.canChange = true;

    ui::OpenBrowser();
    fx.Frame();
    // The listing is built off the owner thread now: the page exists one worker round trip later.
    REQUIRE(ui::panel::WaitForBrowserForTest(10000));
    for (int i = 0; i < 4; ++i) fx.Frame();

    ui::panel::Sample(16666);
    uifake::Reset();
    uifake::ClickOnce("clickme");
    ui::panel::Draw();

    // The click is recorded by the draw and serviced by the next tick, where I/O lives.
    fx.Frame();
    CHECK(fx.transport.loadCalls == 1);
    CHECK(fx.session.requestCalls == 1);
    CHECK(fx.session.requestedLevel == 0x4321);
}

TEST_CASE("WriteDiagnostics carries the world verdict, the toggle key and the sidecar state") {
    Fixture fx;
    char buf[4096];
    const size_t n = ui::WriteDiagnostics(buf, sizeof buf);
    CHECK(n > 0);
    CHECK(strstr(buf, ui::PanelHotkeyName()) != nullptr);
    CHECK(strstr(buf, "world") != nullptr);
    CHECK(strstr(buf, "cursor") != nullptr);
    // "not supplied" and not "0 bookmarks": `diag` and the panel must give the same answer, and
    // the answer before anyone has pushed is that we do not know.
    CHECK(strstr(buf, "bookmarks") != nullptr);
    CHECK(strstr(buf, "not supplied") != nullptr);
}

TEST_CASE("diag reports the sidecar state it was actually handed") {
    Fixture fx;
    const ui::BookmarkView marks[] = {{1000, "a"}, {2000, "b"}};  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    ui::SetBookmarks(ui::NotesState::Ok, marks, 2, 4242);
    char buf[4096];
    REQUIRE(ui::WriteDiagnostics(buf, sizeof buf) > 0);
    CHECK(strstr(buf, "ok, 2 supplied") != nullptr);
    CHECK(strstr(buf, "resume point recorded") != nullptr);
}


// ---------------------------------------------------------------------------------------------
// The game-space readiness guard (DIAG-CRASH launch-12).
//
// The crash: OnTick handed CS.LOOKUP_BY_SYMBOL a CR15NetGame+8 that was non-null and UNBUILT, the
// engine's binary search read [base + count*8] with base 0x400000002000 and count 0x100000, and
// the game died with 0xC0000005. The predicate itself is W1-C's tableguard, and its legs are
// proved in tests/bootstrap/test_tableguard.cpp; what is proved HERE is the wiring -- that the
// shipping OnTick reaches it for both tables, refuses with a named reason, and keeps retrying.
// The fixtures carry the two values the minidump actually recorded.
namespace {

using replay::tableguard::TableVerdict;
using replay::tableguard::TableWhy;
using replay::ui::csregister::Refusal;

const uint64_t kDumpBase = 0x400000002000ull;   // R10 at the fault
const uint64_t kDumpCount = 0x100000ull;        // Rdx at the fault, [gameSpace+0x450]
// What sits at block +0x10 in a live process: the CAllocator pointer, a heap address. Planted so
// that reading +0x10 as the byte size -- the F1 defect -- gets a number no capacity can exceed.
const uint64_t kAllocatorLike = 0x000001E2A4C31F80ull;

// ARCH-10 section 3.4. The space slot is NETGAME.CS_SPACE_OFF, eleven kilobytes into the netgame, so the
// rig must be at least that wide -- a rig that still fits in four pointers is a rig the product's
// read never reaches.
const size_t kNetGameRigBytes = facts::NETGAME_CS_SPACE_OFF + sizeof(void*);
static_assert(kNetGameRigBytes >= 0x2B10u, "the rig must be wider than the space slot it plants");  // facts-ok: NETGAME.SPECTATORCAM_CS_OFF a test fixture value this case compares against, not an engine position

// A CR15NetGame-shaped buffer with both pointers the engine really carries: the CncaGameSpace at
// +0x2B08 and the CR15Game at +0x8. The CR15Game decoy holds the header L5 actually judged, so an
// accessor that goes back to +0x8 reads a plausible non-null object and refuses on it, exactly as
// the recorded run did, rather than reading a null nobody would misdiagnose.
struct NetGameRig {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(kNetGameRigBytes, 0);
    std::vector<uint8_t> gameDecoy = std::vector<uint8_t>(0x800, 0);

    NetGameRig() {
        uint8_t* const game = gameDecoy.data();
        *reinterpret_cast<uint64_t*>(game + facts::CS_TABLE_OFF + 0x00) = kDumpBase;
        *reinterpret_cast<uint64_t*>(game + facts::CS_TABLE_OFF + 0x08) = kDumpCount * 16;
        *reinterpret_cast<uint64_t*>(game + facts::CS_TABLE_OFF + 0x10) = kAllocatorLike;
        *reinterpret_cast<uint64_t*>(game + facts::CS_TABLE_OFF + 0x28) = kDumpCount;
        *reinterpret_cast<uint64_t*>(game + facts::CS_TABLE_OFF + 0x30) = kDumpCount;
        Put(facts::IDENT_NETGAME_GAMESPACE_OFF, game);
    }

    void* Ptr() { return bytes.data(); }
    void SetSpace(void* space) { Put(facts::NETGAME_CS_SPACE_OFF, space); }
    void SetGame(void* game) { Put(facts::IDENT_NETGAME_GAMESPACE_OFF, game); }

  private:
    void Put(uint32_t off, void* p) { *reinterpret_cast<void**>(bytes.data() + off) = p; }
};

}  // namespace

TEST_CASE("an unconsulted refusal is None, never a reason") {
    const replay::ui::csregister::Report fresh;
    CHECK(fresh.lastRefusal == Refusal::None);
    CHECK(fresh.lastRefused.verdict == TableVerdict::NotChecked);
    CHECK(fresh.deferrals == 0);
    // Every enumerator names itself; a new one added without a name shows up here rather than in a
    // log line nobody reads.
    const Refusal all[] = {Refusal::None, Refusal::ComponentSystemTable,
                           Refusal::UpdateRecordTable, Refusal::UnsortedTable};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; ++i) {
        CHECK(strcmp(replay::ui::csregister::RefusalName(all[i]), "<invalid>") != 0);
    }
    CHECK(strcmp(replay::ui::csregister::StatusName(
                     replay::ui::csregister::Status::SpaceNotReady),
                 "SpaceNotReady") == 0);
}

// The wiring, not just the arithmetic: OnTick is the shipping entry point, and the guard is only
// worth anything if OnTick reaches it. This builds a CR15NetGame-shaped stub whose game space
// carries the recorded crash values and drives the real OnTick.
TEST_CASE("OnTick defers on the recorded crash state instead of calling the engine") {
    replay::ui::csregister::ResetForTest();
    // A contained offline replay: the only process in which OnTick reaches the engine at all, and
    // so the only one in which these table refusals are the reason it did not register.
    replay::livesafety::SetForTest(/*liveOnlineExempt=*/false, /*offlineLockHeld=*/true);

    std::vector<uint8_t> space(0x800, 0);
    uint8_t* const gs = space.data();
    *reinterpret_cast<uint64_t*>(gs + 0x420) = kDumpBase;
    *reinterpret_cast<uint64_t*>(gs + 0x450) = kDumpCount;
    *reinterpret_cast<uint64_t*>(gs + 0x448) = kDumpCount;        // capacity agrees
    *reinterpret_cast<uint64_t*>(gs + 0x428) = kDumpCount * 16;   // and so does the block size
    *reinterpret_cast<uint64_t*>(gs + 0x430) = kAllocatorLike;    // the allocator pointer
    NetGameRig ng;
    ng.SetSpace(gs);

    replay::ui::csregister::OnTick(ng.Ptr(), ::GetCurrentThreadId());

    replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    CHECK(r.status == replay::ui::csregister::Status::SpaceNotReady);
    CHECK(r.lastRefusal == Refusal::ComponentSystemTable);
    CHECK(r.lastRefused.verdict == TableVerdict::NotReady);
    CHECK(r.lastRefused.why == TableWhy::ArrayUnreadable);
    CHECK(r.deferrals == 1);
    CHECK(r.deferralsBeforeFirst == 1);
    CHECK(r.registrations == 0);
    CHECK(r.lastRefused.raw.base == kDumpBase);
    CHECK(r.lastRefused.raw.count == kDumpCount);
    // The refusal says which numbers it saw, so one launch settles what a bare "did not crash"
    // never could.
    CHECK(strstr(replay::ui::csregister::Diagnostic(), "component-system table") != nullptr);
    CHECK(strstr(replay::ui::csregister::Diagnostic(), "400000002000") != nullptr);

    // It keeps trying: a deferral that never retries is a feature that silently never works.
    replay::ui::csregister::OnTick(ng.Ptr(), ::GetCurrentThreadId());
    CHECK(replay::ui::csregister::Status_().deferrals == 2);

    // The control: not every refusal is a deferral. A null netgame is a different answer and must
    // not move the counter.
    replay::ui::csregister::OnTick(nullptr, ::GetCurrentThreadId());
    r = replay::ui::csregister::Status_();
    CHECK(r.status == replay::ui::csregister::Status::NoGameSpace);
    CHECK(r.deferrals == 2);

    replay::ui::csregister::ResetForTest();
}

TEST_CASE("OnTick guards the registrar's own table, not only the one it searches") {
    replay::ui::csregister::ResetForTest();
    // A contained offline replay: the only process in which OnTick reaches the engine at all, and
    // so the only one in which these table refusals are the reason it did not register.
    replay::livesafety::SetForTest(/*liveOnlineExempt=*/false, /*offlineLockHeld=*/true);

    std::vector<uint8_t> space(0x800, 0);
    uint64_t csEntries[8] = {};
    uint8_t* const gs = space.data();
    // A component-system table that passes every leg...
    *reinterpret_cast<uint64_t*>(gs + 0x420) = static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(csEntries));
    *reinterpret_cast<uint64_t*>(gs + 0x428) = 4 * 16;
    *reinterpret_cast<uint64_t*>(gs + 0x430) = kAllocatorLike;
    *reinterpret_cast<uint64_t*>(gs + 0x448) = 4;
    *reinterpret_cast<uint64_t*>(gs + 0x450) = 4;
    // ... over an update-record table that does not. Scheme 0 starts at the game space itself, so
    // the record table lives at +0x5E0 and its count at +0x610.
    *reinterpret_cast<uint64_t*>(gs + 0x5E0) = kDumpBase;
    *reinterpret_cast<uint64_t*>(gs + 0x5E8) = kDumpCount * 72;
    *reinterpret_cast<uint64_t*>(gs + 0x5F0) = kAllocatorLike;
    *reinterpret_cast<uint64_t*>(gs + 0x608) = kDumpCount;
    *reinterpret_cast<uint64_t*>(gs + 0x610) = kDumpCount;
    NetGameRig ng;
    ng.SetSpace(gs);

    replay::ui::csregister::OnTick(ng.Ptr(), ::GetCurrentThreadId());

    const replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    CHECK(r.status == replay::ui::csregister::Status::SpaceNotReady);
    CHECK(r.lastRefusal == Refusal::UpdateRecordTable);
    CHECK(r.lastRefused.why == TableWhy::ArrayUnreadable);
    CHECK(strstr(replay::ui::csregister::Diagnostic(), "update-record table") != nullptr);

    replay::ui::csregister::ResetForTest();
}

// =================================================================================================
// AUDIT_W2-F_AVFIX F1 -- the byte size is read from block +0x08, and the leg that uses it is live.
// With the size read from +0x10 (the allocator pointer) no capacity ever exceeded it, so a block
// too small for its capacity passed. This plants exactly that: a real array, a block one byte
// short of its capacity, and an allocator-shaped pointer where the old offset looked.
// =================================================================================================

TEST_CASE("a block too small for its capacity is refused through the real field, not a pointer") {
    replay::ui::csregister::ResetForTest();
    // A contained offline replay: the only process in which OnTick reaches the engine at all, and
    // so the only one in which these table refusals are the reason it did not register.
    replay::livesafety::SetForTest(/*liveOnlineExempt=*/false, /*offlineLockHeld=*/true);

    std::vector<uint8_t> space(0x800, 0);
    uint64_t csEntries[8] = {};
    uint8_t* const gs = space.data();
    *reinterpret_cast<uint64_t*>(gs + 0x420) = static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(csEntries));
    *reinterpret_cast<uint64_t*>(gs + 0x428) = 4 * 16 - 1;
    *reinterpret_cast<uint64_t*>(gs + 0x430) = kAllocatorLike;
    *reinterpret_cast<uint64_t*>(gs + 0x448) = 4;
    *reinterpret_cast<uint64_t*>(gs + 0x450) = 4;
    NetGameRig ng;
    ng.SetSpace(gs);

    replay::ui::csregister::OnTick(ng.Ptr(), ::GetCurrentThreadId());

    const replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    CHECK(r.status == replay::ui::csregister::Status::SpaceNotReady);
    CHECK(r.lastRefused.why == TableWhy::CapacityOverBlock);
    CHECK(r.lastRefused.raw.bytes == 4 * 16 - 1);
    CHECK(strstr(replay::ui::csregister::Diagnostic(), "bytes=63 ") != nullptr);

    replay::ui::csregister::ResetForTest();
}

// =================================================================================================
// AUDIT_W2-F_AVFIX F2 -- an append target is not Ready on a zero count alone. The engine's grow
// path skips the realloc when count < capacity and writes a 72-byte record at base + 0, so a zero
// count over a garbage base and capacity is the audited defect one level down.
// =================================================================================================

TEST_CASE("OnTick refuses an empty update-record table over an unreadable base") {
    replay::ui::csregister::ResetForTest();
    // A contained offline replay: the only process in which OnTick reaches the engine at all, and
    // so the only one in which these table refusals are the reason it did not register.
    replay::livesafety::SetForTest(/*liveOnlineExempt=*/false, /*offlineLockHeld=*/true);

    std::vector<uint8_t> space(0x800, 0);
    uint64_t csEntries[8] = {};
    uint8_t* const gs = space.data();
    *reinterpret_cast<uint64_t*>(gs + 0x420) = static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(csEntries));
    *reinterpret_cast<uint64_t*>(gs + 0x428) = 4 * 16;
    *reinterpret_cast<uint64_t*>(gs + 0x430) = kAllocatorLike;
    *reinterpret_cast<uint64_t*>(gs + 0x448) = 4;
    *reinterpret_cast<uint64_t*>(gs + 0x450) = 4;
    *reinterpret_cast<uint64_t*>(gs + 0x5E0) = kDumpBase;
    *reinterpret_cast<uint64_t*>(gs + 0x5E8) = kDumpCount * 72;
    *reinterpret_cast<uint64_t*>(gs + 0x608) = kDumpCount;
    *reinterpret_cast<uint64_t*>(gs + 0x610) = 0;   // count 0: the leg F2 fell through
    NetGameRig ng;
    ng.SetSpace(gs);

    replay::ui::csregister::OnTick(ng.Ptr(), ::GetCurrentThreadId());

    const replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    CHECK(r.status == replay::ui::csregister::Status::SpaceNotReady);
    CHECK(r.lastRefusal == Refusal::UpdateRecordTable);
    CHECK(r.lastRefused.why == TableWhy::ArrayUnreadable);
    CHECK(strstr(replay::ui::csregister::Diagnostic(), "update-record table") != nullptr);

    replay::ui::csregister::ResetForTest();
}

// L3-RUN-9 F2, asserted through the shipped refusal sentence rather than the policy function
// alone: on a 30 Hz machine the measured ceiling is 66666/33334 = 1.99994, stated as 2.00x.
TEST_CASE("the 2x this build advertises at 30 Hz is not refused by its own sentence") {
    Fixture fx;
    fx.transport.state = TransportState::Play;
    fx.transport.rate = 1.0f;
    for (int i = 0; i < 400; ++i) ui::panel::Sample(33334);
    REQUIRE(ui::RateCeiling() > 0.0f);
    REQUIRE(ui::RateCeiling() < 2.0f);

    const char* why = "not written";
    CHECK(ui::RateIsAchievable(2.0, &why));
    CHECK(strcmp(why, "") == 0);

    const char* refused = nullptr;
    CHECK_FALSE(ui::RateIsAchievable(2.01, &refused));
    REQUIRE(refused != nullptr);
    CHECK(strstr(refused, "2.01x is above this machine's fast-forward ceiling of 2.00x") != nullptr);
}

// =================================================================================================
// M2 -- the fast-forward ceiling is measured through whatever rate is playing, not only 1x.
// "Click 2x while paused, then Play: the EMA never updates, g.ceiling stays
// 0.0 forever, CheckRate refuses nothing."
// =================================================================================================

namespace {
float ExpectedCeilingAt60Hz() { return ui::RateCeiling(facts::CLOCK_MAXELAPSED_US, 16666); }
}  // namespace

TEST_CASE("the ceiling is measured through a 2x play, not only at 1x") {
    Fixture fx;
    fx.transport.state = TransportState::Play;
    fx.transport.rate = 2.0f;
    REQUIRE(ExpectedCeilingAt60Hz() > 0.0f);

    SUBCASE("forward at 2x: the scaled delta is two frames, the raw period one") {
        for (int i = 0; i < 40; ++i) ui::panel::Sample(33332);
        CHECK(ui::RateCeiling() == doctest::Approx(ExpectedCeilingAt60Hz()));
    }
    SUBCASE("reverse at -2x measures the same period") {
        fx.transport.rate = -2.0f;
        for (int i = 0; i < 40; ++i) ui::panel::Sample(33332);
        CHECK(ui::RateCeiling() == doctest::Approx(ExpectedCeilingAt60Hz()));
    }
    SUBCASE("a paused frame is not a sample, whatever the rate says") {
        fx.transport.state = TransportState::Pause;
        for (int i = 0; i < 40; ++i) ui::panel::Sample(1);
        CHECK(ui::RateCeiling() == 0.0f);
    }
    SUBCASE("a delta at CLOCK.MAX_ELAPSED is the engine's clamp, not a measurement") {
        fx.transport.rate = 8.0f;
        for (int i = 0; i < 40; ++i) ui::panel::Sample(facts::CLOCK_MAXELAPSED_US);
        CHECK(ui::RateCeiling() == 0.0f);
    }

    // ★ L3-RUN-2 F3: ui::RateCeiling() was 0 for a whole launch. Transport() returns the services'
    // object, published from module init onward, so the sampler's unscaled branch (`t == nullptr`)
    // is reachable here and unreachable in the game -- the only live path needed Play, and a launch
    // that boots and does not play measured nothing. These drive the LIVE shape: a transport that
    // exists with nothing loaded. An unmeasurable ceiling must also name its own cause, because "0"
    // alone sent four transport rows to could-not-ask with nothing to act on.
    char diag[4096];

    SUBCASE("nothing loaded: our clock module is not engaged, so the delta IS the raw period") {
        fx.transport.state = TransportState::NotLoaded;
        for (int i = 0; i < 40; ++i) ui::panel::Sample(16666);
        CHECK(ui::RateCeiling() == doctest::Approx(ExpectedCeilingAt60Hz()));
    }
    SUBCASE("a sim clock that never advances is named, not left as a bare 0") {
        fx.transport.state = TransportState::NotLoaded;
        for (int i = 0; i < 40; ++i) ui::panel::Sample(0);
        CHECK(ui::RateCeiling() == 0.0f);
        ui::WriteDiagnostics(diag, sizeof diag);
        CHECK(strstr(diag, "sim clock is not advancing") != nullptr);
    }
    SUBCASE("and a state that cannot measure is a different answer from one that measured 0") {
        fx.transport.state = TransportState::Pause;
        for (int i = 0; i < 40; ++i) ui::panel::Sample(16666);
        CHECK(ui::RateCeiling() == 0.0f);
        ui::WriteDiagnostics(diag, sizeof diag);
        CHECK(strstr(diag, "not one tick has been in a state that measures") != nullptr);
    }
}


// =================================================================================================
// M6 -- the Pov/Follow selector is 1-based and the row cannot step it to 0.
// "lets the Pov/Follow selector reach 0, which the row itself documents as
// 1-based over two teams of five."
// =================================================================================================

namespace {
struct CameraFixture {
    Fixture fx;
    FakeCamera cam;
    CameraFixture() {
        ui::panel::SetCameraForTest(&cam);
        PressKey();   // the toggle brings the panel up with the browser closed
        fx.Frame();
        fx.Frame();
    }
    ~CameraFixture() { ui::panel::SetCameraForTest(nullptr); }
    void Click(const char* needle) {
        ui::panel::Sample(16666);
        uifake::Reset();
        uifake::ClickOnce(needle);
        ui::panel::Draw();
    }
};
}  // namespace

TEST_CASE("the player selector cannot step below 1 on the Pov and Follow rows") {
    CameraFixture c;
    c.cam.requested = CameraMode::Pov;
    c.Click("-##camdec");
    CHECK(c.cam.sets.empty());
    CHECK(uifake::TextContains("player 1 of 10"));

    c.cam.requested = CameraMode::Follow;
    c.Click("-##camdec");
    CHECK(c.cam.sets.empty());
}

TEST_CASE("the level index steps to 0, and a player mode chosen from there gets player 1") {
    CameraFixture c;
    c.cam.requested = CameraMode::Level;
    c.Click("-##camdec");
    REQUIRE(c.cam.sets.size() == 1);
    CHECK(c.cam.sets[0].first == CameraMode::Level);
    CHECK(c.cam.sets[0].second == 0);
    CHECK(uifake::TextContains("index 0 ->"));

    c.Click("##cam0");   // the Pov button
    REQUIRE(c.cam.sets.size() == 2);
    CHECK(c.cam.sets[1].first == CameraMode::Pov);
    CHECK(c.cam.sets[1].second == 1);
}

// =================================================================================================
// M8 -- listing a folder opens no file; headers are read a few per tick, or one on demand.
// "LoadMostRecent scans the folder synchronously -- 200 captures is 200
// CreateFileW plus 400 seek/read pairs -- and it runs on the tick."
// =================================================================================================

namespace {
bool SetMtime(const wchar_t* path, uint64_t hundredNs) {
    HANDLE h = ::CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(hundredNs);
    ft.dwHighDateTime = static_cast<DWORD>(hundredNs >> 32);
    const bool ok = ::SetFileTime(h, nullptr, nullptr, &ft) != 0;
    ::CloseHandle(h);
    return ok;
}

// One fixture mtime, ten million hundred-nanosecond ticks -- a second -- apart. It is a function
// rather than an expression at each call site so the folder and the wide-named row order
// themselves against the same clock, and so the literal has one home.
uint64_t FixtureMtime(uint64_t index) {
    return 132000000000000000ull + index * 10000000ull;  // facts-ok: CLOCK.CTOR_MAXELAPSED_INIT one second in FILETIME's 100 ns ticks, a file mtime and not a clock ceiling
}

// `count` captures in the panel's replay folder, f00 the oldest and the last one the newest.
void WriteFolder(Fixture& fx, int count) {
    ::CreateDirectoryW(fx.dir.File(L"echoreplay").c_str(), nullptr);
    for (int i = 0; i < count; ++i) {
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\f%02d.echodemo", fx.captureDir.c_str(), i);
        REQUIRE(WriteCapture(path, 0x4321, Whole(0x8765), 1));
        REQUIRE(SetMtime(path, FixtureMtime(static_cast<uint64_t>(i))));
    }
}

// The newest capture in the folder, with a name WIDER THAN THE WINDOW. Every other fixture here
// writes "f00.echodemo", which fits any budget, so without this the width cut and the name's
// two-ended fit are paths no offline frame executes.
void WriteWideNamedCapture(Fixture& fx, size_t nameChars) {
    std::wstring stem(nameChars, L'w');
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s.echodemo", fx.captureDir.c_str(),
                 stem.c_str());
    REQUIRE(WriteCapture(path, 0x4321, Whole(0x8765), 1));
    // Newest, so it is row 0 of the page the browser draws.
    REQUIRE(SetMtime(path, FixtureMtime(99)));
}
}  // namespace

TEST_CASE("a row wider than the window is cut, and the cut is inside the budget") {
    Fixture fx;
    WriteFolder(fx, 2);
    WriteWideNamedCapture(fx, 140);
    fx.session.canChange = true;

    ui::OpenBrowser();
    fx.Frame();
    REQUIRE(ui::panel::WaitForBrowserForTest(10000));
    uifake::Reset();
    fx.Frame();

    std::string wide;
    std::string narrow;
    for (const std::string& l : uifake::ButtonLabels()) {
        if (l.find("##row0") != std::string::npos) wide = l;
        if (l.find("##row1") != std::string::npos) narrow = l;
    }
    REQUIRE_FALSE(wide.empty());
    const size_t measured = wide.find("##");
    REQUIRE(measured != std::string::npos);
    // (0) ImGui measures the WHOLE label before it clips it, so a character past the right edge
    // costs what a visible one costs. Remove the cut and this is 140-odd, not 98.
    // EVENTS-1 4b: the cut now backs up to the last space within twelve characters, so a line that
    // does not fit ends on a whole word instead of mid-word. It still reaches the budget; it can
    // fall just short of it when a word boundary is near the end.
    CHECK(measured <= ui::panel::kRowCharBudget);
    CHECK(measured > ui::panel::kRowCharBudget - 13);
    // (1) The cut is MARKED, and the mark is inside the budget rather than three characters past
    // it -- a cut line that came back wider than its budget would make the budget unassertable.
    CHECK(wide.compare(measured - 3, 3, "...") == 0);
    // (2) The name is fitted from BOTH ends. A plain right-hand cut keeps 44 characters of a
    // timestamp and throws the extension away; real capture names differ at both ends.
    CHECK(wide.find(".echodemo") != std::string::npos);
    // (3) A short-named row is padded to the same column, which is what lines the columns up
    // without a separator widget between them.
    REQUIRE_FALSE(narrow.empty());
    CHECK(narrow.find("f01.echodemo") != std::string::npos);
    CHECK(narrow.find("rounds") == wide.find("rounds"));
}

TEST_CASE("LoadMostRecent lists the folder and opens only the row it loads") {
    Fixture fx;
    WriteFolder(fx, 3);
    fx.session.levelId = 0x1;
    fx.session.gametypeId = Whole(0x2);
    fx.session.canChange = true;

    CHECK(ui::LoadMostRecent());
    CHECK(fx.transport.loadCalls == 1);
    CHECK(fx.transport.lastPath.find(L"f02.echodemo") != std::wstring::npos);
    REQUIRE(fx.index.Count() == 3);
    CHECK(fx.index.RowRead(0));
    CHECK_FALSE(fx.index.RowRead(1));
    CHECK_FALSE(fx.index.RowRead(2));
}

namespace {

uint64_t QpcNanos() {
    static const int64_t freq = [] {
        LARGE_INTEGER f{};
        return ::QueryPerformanceFrequency(&f) ? f.QuadPart : 0;
    }();
    LARGE_INTEGER t{};
    if (freq <= 0 || !::QueryPerformanceCounter(&t)) return 0;
    const uint64_t ticks = static_cast<uint64_t>(t.QuadPart);
    return (ticks / static_cast<uint64_t>(freq)) * 1000000000ull +
           ((ticks % static_cast<uint64_t>(freq)) * 1000000000ull) / static_cast<uint64_t>(freq);
}

// Rows this frame. The facade's row buttons are the only labels carrying the "##row" the panel
// puts on them, so counting them counts what the browser actually listed.
size_t RowsDrawn() {
    size_t n = 0;
    for (const std::string& label : uifake::ButtonLabels()) {
        if (label.find("##row") != std::string::npos) ++n;
    }
    return n;
}

}  // namespace

// What one snapshot now carries: the page the pager asked for and nothing else, so the listing is
// whole however many captures the folder holds.
constexpr size_t kBrowserPageRows = ui::panel::kBrowserRowsPerPage;

TEST_CASE("the browser lists the whole page off the owner thread, and opens nothing past it") {
    Fixture fx;
    WriteFolder(fx, 30);
    fx.session.canChange = true;
    ui::panel::SetPlayableOnlyForTest(false);

    ui::OpenBrowser();
    fx.Frame();                                        // the tick hands the request to the worker
    REQUIRE(ui::panel::WaitForBrowserForTest(10000));
    fx.Frame();                                        // the tick takes the snapshot; the draw reads it

    REQUIRE(fx.index.Count() == 30);
    // The whole page is read before it is drawn, so no row is ever listed with badges nobody has
    // opened a file to earn. The old per-tick amortisation is what "reading..." used to mean.
    CHECK(fx.index.RowRead(kBrowserPageRows - 1));
    CHECK(fx.index.ManifestLoaded(kBrowserPageRows - 1));
    // Past the page the facade can show, nothing is opened at all -- with the playable-only filter
    // off, which is the state this case sets. The filter's own cost is a case of its own below.
    CHECK_FALSE(fx.index.RowRead(kBrowserPageRows));
    CHECK(uifake::ButtonDrawn("f29"));   // newest first: f29 is row 0
    CHECK(RowsDrawn() == ui::panel::kBrowserRowsPerPage);
    // The header counts the LISTING, not the cut: 30 captures, this page of them.
    CHECK(uifake::TextContains("of 30"));
    CHECK_FALSE(uifake::TextContains("more not shown"));
}

// ---- W2-F DRAWSTALL: the 1 653 400 ns tail ----------------------------------------------------
//
// L5-RUN-9 measured drawNanosMax = 1 653 400 ns against a 68 500 ns mean and a 500 ns first draw:
// a rare stall on a flat mean, which is the shape of a filesystem call and not of a per-frame cost
// that grew. This case drives the shape of that launch -- a page of real captures, a thousand
// frames, a rescan in the middle -- and asserts the two properties the fix is:
//
//   1. NOT ONE FILE IS OPENED ON THE THREAD THAT DRAWS. Counted by demoindex, not asserted in a
//      comment, and with a positive control: the same counter has to show the worker opening files,
//      or "zero on the draw thread" would pass on a run where nothing opened anything.
//   2. The worst single draw stays under the ARCH-20 ceiling this row is scored against.
TEST_CASE("1000 frames over 25 captures: no file I/O on the draw thread, and the draw stays in budget") {
    Fixture fx;
    // One more capture than the page can show, so the cut line is exercised too.
    WriteFolder(fx, static_cast<int>(kBrowserPageRows) + 1);
    // One row on the page is wider than the window, so the frame this case measures is a frame in
    // which the width cut actually fired.
    WriteWideNamedCapture(fx, 140);
    fx.session.canChange = true;
    ui::inframe::ResetForTest();
    ui::fileio::ResetForTest();

    ui::OpenBrowser();
    fx.Frame();
    REQUIRE(ui::panel::WaitForBrowserForTest(10000));
    fx.Frame();
    REQUIRE(RowsDrawn() == ui::panel::kBrowserRowsPerPage);

    // A thousand frames, seeded rather than transcribed: 0x3E8 is also the value of
    // GAME.VTBL_SETFIXEDTIMESTEP_OFF, and a literal that collides with a fact is a literal the
    // next reader has to disambiguate. std::milli::den is a thousand for the reason we want one --
    // it is how many of something there are in the next unit up.
    constexpr size_t kFrames = std::milli::den;
    std::vector<uint64_t> drawNanos;
    drawNanos.reserve(kFrames);
    uint64_t firstDrawNanos = 0;
    uint64_t totalDrawNanos = 0;
    size_t fewestRows = ui::panel::kBrowserRowsPerPage;
    uint64_t worstGlyphs = 0;
    uint64_t worstItems = 0;
    uint64_t totalGlyphs = 0;
    uint64_t totalItems = 0;
    for (size_t frame = 0; frame < kFrames; ++frame) {
        // The rescan the owner presses mid-session: the button sets a request, the tick hands it
        // to the worker, and the frames in between must not notice.
        if (frame == kFrames / 2) uifake::ClickOnce("Rescan##rescan");
        uifake::Reset();
        ui::panel::Sample(16666);
        const uint64_t before = QpcNanos();
        const ui::inframe::DrawOutcome outcome = ui::panel::Draw();
        const uint64_t after = QpcNanos();
        REQUIRE(outcome == ui::inframe::DrawOutcome::Drawn);
        const uint64_t nanos = after - before;
        if (frame == 0) {
            firstDrawNanos = nanos;   // excluded from the distribution, exactly as inframe excludes it
        } else {
            drawNanos.push_back(nanos);
            totalDrawNanos += nanos;
        }
        // ★ THE TORN-READ ASSERTION. The snapshot is swapped with one atomic exchange, so a frame
        // sees the whole of one listing or the whole of the previous one. A page that is being
        // filled while it is drawn shows up here as a frame with some rows and not others.
        const size_t rows = RowsDrawn();
        if (rows < fewestRows) fewestRows = rows;
        // ★ WHAT THE FRAME HANDED ImGui. Counted, not timed: this host's scheduler is in the
        // nanoseconds and is not in these.
        const uint64_t glyphs = uifake::GlyphsDrawn();
        const uint64_t items = uifake::ItemsDrawn();
        if (glyphs > worstGlyphs) worstGlyphs = glyphs;
        if (items > worstItems) worstItems = items;
        totalGlyphs += glyphs;
        totalItems += items;
    }
    REQUIRE(ui::panel::WaitForBrowserForTest(10000));

    std::sort(drawNanos.begin(), drawNanos.end());
    const uint64_t worstDrawNanos = drawNanos.back();
    const uint64_t p99 = drawNanos[drawNanos.size() * 99 / 100];
    const uint64_t meanDrawNanos = totalDrawNanos / drawNanos.size();
    INFO("draws: mean " << meanDrawNanos << " ns, p99 " << p99 << " ns, max " << worstDrawNanos
                        << " ns, first " << firstDrawNanos << " ns, budget "
                        << ui::inframe::kDrawNanosBudgetNs << " ns");

    // ** WHAT THIS HOST CAN AND CANNOT ASSERT ABOUT A SINGLE FRAME. The bracket has to be the one
    // TimedDraw uses, so fake_gui's own bookkeeping is inside it: it stores every label this page
    // produced in a std::vector<std::string>, which is 24 heap allocations a frame that NRadGui
    // does not make. Add an unpinned test process on a machine the owner is using, and the single
    // worst of a thousand draws is a measurement of this host's scheduler, not of the panel.
    //
    // The p99 and the mean are not: a stall that comes from touching the disk is not one frame in
    // a thousand, it is every frame that lists a folder, and both of those would carry it. The max
    // is printed rather than asserted, and the structural claims below -- no file opened on this
    // thread at all, no page drawn in halves -- are what actually make the stall impossible.
    CHECK(p99 < ui::inframe::kDrawNanosBudgetNs);
    CHECK(meanDrawNanos * 4 < ui::inframe::kDrawNanosBudgetNs);

    // ---- THE NUMBER THAT TRAVELS TO THE LAUNCH ------------------------------------------------
    //
    // L5 #10 measured a 51 224 ns mean in the game against this bench's 6 273 ns: an 8.2x
    // disagreement, because the bench's facade is a std::vector push and the game's is NRadGui's
    // ImGui. The bench cannot close that gap -- there is no ImGui in this tree to link -- so it
    // stops pretending the nanoseconds are the answer and scores the WORK instead. Items and
    // glyphs are exactly what the panel hands ImGui; the cost per item and per glyph is the
    // engine's and is the same for every frame the game draws. A frame that hands over N times
    // less work costs N times less there, whatever the constant turns out to be.
    //
    // The budget is the panel's own window: the lines it has room for, each cut to the width it
    // has room for. A draw that issues the whole LISTING instead of the page it can show blows it
    // by construction, and so does a draw that stops cutting its lines to the width the window has.
    constexpr uint64_t kGlyphBudget =
        static_cast<uint64_t>(ui::panel::kLinesOnScreen) * ui::panel::kRowCharBudget;
    INFO("facade work per frame: glyphs max " << worstGlyphs << " mean "
         << (totalGlyphs / kFrames) << " (budget " << kGlyphBudget << "), items max " << worstItems
         << " mean " << (totalItems / kFrames) << ", rows/page " << ui::panel::kBrowserRowsPerPage
         << ", chars/line " << ui::panel::kRowCharBudget);
    CHECK(worstGlyphs <= kGlyphBudget);
    // The positive control: a bench that drew nothing would also pass the line above.
    CHECK(worstGlyphs > ui::panel::kBrowserRowsPerPage);
    CHECK(worstItems > ui::panel::kBrowserRowsPerPage);

    // ---- THE STREAM, DERIVED FROM THE LAYOUT RATHER THAN OBSERVED FROM A RUN ------------------
    //
    // L5 #11: imguiNanos was 208 500 ns of a 209 100 ns draw. 99.7% of the frame is the engine's
    // ImGui issuing the widgets we name, and the per-widget constant is not ours to change. The
    // stream is. Twelve widgets is the two chrome buttons, the two-line header block, one Button
    // per row on the page and the two pager buttons -- and a row's verdict no longer adds one,
    // which is what made a page of refused rows cost twice what a page of loadable ones costs.
    CHECK(worstItems == ui::panel::kBrowserItemBudget);
    CHECK(ui::panel::kBrowserItemBudget == 13u);   // + MULTI-LOAD's playable-only toggle
    CHECK(worstGlyphs <= ui::panel::kBrowserGlyphBudget);
    CHECK(ui::panel::kBrowserGlyphBudget <= static_cast<size_t>(std::milli::den));
    // Not slack by accident: the page's widest row is a 140-character name cut to the width the
    // window has, so the worst frame is near its budget rather than nowhere near it.
    CHECK(worstGlyphs > ui::panel::kBrowserRowsPerPage * ui::panel::kRowCharBudget);

    // ** THE PRODUCT'S OWN COUNTER AND THE FAKE FACADE'S AGREE, AND THEY ARE ON OPPOSITE SIDES OF
    // THE FACADE: panel.cpp counts what it issues, fake_gui counts what it received. A published
    // stream that had drifted from the drawn one would have to disagree here. This is the number
    // the launch record carries, so it is the number that has to be checked against something.
    const ui::inframe::Counters& ic = ui::inframe::Read();
    // kFrames plus the two setup frames this case draws between ResetForTest and the loop: the
    // counter counts every browser frame, and saying so is cheaper than a counter that skips some.
    CHECK(ic.guiStreamFrames == kFrames + 2);
    CHECK(ic.guiItemsPerFrame == worstItems);
    CHECK(ic.guiGlyphsPerFrame == worstGlyphs);

    // The control: the counter counts. Without this, "zero on the draw thread" is also what a
    // broken counter says, and what a run that never opened a file says.
    CHECK(ui::fileio::Count() >= kBrowserPageRows);
    CHECK(ui::fileio::OnDrawThread() == 0u);
    // Every frame drew the whole page, never part of one.
    CHECK(fewestRows == ui::panel::kBrowserRowsPerPage);

    // The instrument agrees with the stopwatch: imguiNanos is measured inside Draw and cannot
    // exceed the wall time of the call that contains it.
    const ui::inframe::Counters& c = ui::inframe::Read();
    CHECK(c.imguiNanos <= worstDrawNanos + firstDrawNanos);
    CHECK(c.browserIoOnDrawThread == 0u);
    // The folder work happened, and it happened somewhere that is not a frame.
    CHECK(c.metadataReadNanos > 0u);
}

// ---- the epoch: CSREGISTER-EPOCH / AUDIT_W2-F_AVFIX F3 --------------------------------------
//
// These drive the SHIPPING OnTick and TryRegister against a synthetic game space and a stand-in
// engine: a lookup that answers a component system and a registrar that appends a 72-byte record
// exactly the way 0x140506810 does (cs at +0, the 24-byte PMF at +0x10, order key at +0x38, count
// at table+0x30 moving). Everything between OnTick and the record scan is the real code.
namespace {

const uint64_t kFakeRecordCapacity = 8;

// Where the fixture parks each space's own component-system pointer. No engine structure lives
// here; it is spare room inside the 0x800-byte space block, read only by FakeLookup.
constexpr uint32_t kFakeCsObjectOff = 0x700;

struct FakeEngine {
    uint64_t lookups = 0;
    uint64_t registrations = 0;
    void* lastPmfFn = nullptr;
    uint16_t lastOrderKey = 0;
};
FakeEngine g_engine;

// The component system is a property of the SPACE, not of the engine: two spaces answer with two
// different objects, which is what makes a dispatch attributable to one registration.
void* FakeLookup(void* space, uint64_t) {
    ++g_engine.lookups;
    return *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(space) + kFakeCsObjectOff);
}

void FakeRegister(void* space, void* cs, const void* pmf24, const char*, const char*,
                  uint16_t orderKey, uint16_t tiebreak, const void*, uint16_t schemeIdx) {
    ++g_engine.registrations;
    uint8_t* table = static_cast<uint8_t*>(space) + 0x5E0 + 0x78 * schemeIdx;
    uint64_t& count = *reinterpret_cast<uint64_t*>(table + 0x30);
    REQUIRE(count < kFakeRecordCapacity);   // the real one reallocates; the fixture never fills
    uint8_t* rec = reinterpret_cast<uint8_t*>(*reinterpret_cast<uint64_t*>(table)) + count * 72;
    memcpy(rec + 0x00, &cs, 8);
    memcpy(rec + 0x10, pmf24, 24);
    memcpy(rec + 0x38, &orderKey, 2);
    memcpy(rec + 0x3A, &tiebreak, 2);
    ++count;
    memcpy(&g_engine.lastPmfFn, pmf24, 8);
    g_engine.lastOrderKey = orderKey;
}

// The registrar returning without appending is the only producer of RegistrarUnavailable, and the
// shipped FakeRegister always appends, so the refusal path needs a registrar of its own.
void NoMoveRegister(void*, void*, const void*, const char*, const char*, uint16_t, uint16_t,
                    const void*, uint16_t) {
    ++g_engine.registrations;
}

// A CR15NetGame-shaped stub over a game space whose two tables are consistent and really mapped:
// the state F3's stale block imitates, and the state a live space is in.
struct SyntheticSpace {
    std::vector<uint8_t> space = std::vector<uint8_t>(0x800, 0);
    std::vector<uint8_t> csEntries = std::vector<uint8_t>(4 * 16, 0);
    std::vector<uint8_t> records = std::vector<uint8_t>(kFakeRecordCapacity * 72, 0);
    NetGameRig ng;
    uint64_t csObject = 0;
    // An allocator the shared check accepts: a pointer to an object whose first qword is a
    // readable vtable. Nothing here is ever called through it; the fake registrar never grows.
    uint64_t vtable[8] = {};
    uint64_t* allocator = vtable;

    SyntheticSpace() {
        uint8_t* gs = space.data();
        Plant(gs + 0x420, csEntries.data(), 4, 4, 16);
        Plant(gs + 0x5E0, records.data(), kFakeRecordCapacity, 0, 72);
        *reinterpret_cast<uint64_t*>(gs + kFakeCsObjectOff) =
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&csObject));
        ng.SetSpace(gs);
    }
    void Plant(uint8_t* table, void* base, uint64_t capacity, uint64_t count, uint64_t stride) {
        *reinterpret_cast<uint64_t*>(table + 0x00) =
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(base));
        *reinterpret_cast<uint64_t*>(table + 0x08) = capacity * stride;
        *reinterpret_cast<uint64_t*>(table + 0x10) =
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&allocator));
        *reinterpret_cast<uint64_t*>(table + 0x20) = 4;   // grow
        *reinterpret_cast<uint64_t*>(table + 0x28) = capacity;
        *reinterpret_cast<uint64_t*>(table + 0x30) = count;
    }
    uint64_t& CsCount() { return *reinterpret_cast<uint64_t*>(space.data() + 0x450); }
    uint64_t& CsFlags() { return *reinterpret_cast<uint64_t*>(space.data() + 0x458); }
    uint64_t& RecCount() { return *reinterpret_cast<uint64_t*>(space.data() + 0x610); }
    uint64_t& RecBase() { return *reinterpret_cast<uint64_t*>(space.data() + 0x5E0); }
    void Tick() { replay::ui::csregister::OnTick(ng.Ptr(), ::GetCurrentThreadId()); }
};

replay::ui::inframe::DrawOutcome NoDraw() {
    return replay::ui::inframe::DrawOutcome::Drawn;
}

// ★ THE ENGINE'S FRAME BYTE, AND WHY EVERY COUNTER CASE BELOW NOW GOES THROUGH IT (AUDIT_F3J
// F3J-5). ownerDraws is credited only when the draw reached an OPEN frame, so a fixture that did
// not model the frame byte would report every one of these cases as a refusal -- and, before this,
// reported every refusal as a draw. Armed OPEN by default, because these cases are about the
// mailbox and the drain; the shut-frame case is the one immediately after the coverage case, and
// tests/ui/test_inframe.cpp sweeps the byte exhaustively.
struct FrameGlobals {
    uint8_t frameOpenByte = 1;
    uint8_t startFrameLatch = 1;
    const void* contextPtr = nullptr;
};
FrameGlobals g_frame;

void ArmFrameOpen() {
    g_frame.frameOpenByte = 1;
    g_frame.startFrameLatch = 1;
    g_frame.contextPtr = &g_frame;
    replay::ui::inframe::SetProbeForTest(&g_frame.frameOpenByte, &g_frame.startFrameLatch,
                                         &g_frame.contextPtr);
    uifake::SetFrameByte(&g_frame.frameOpenByte);
}

void ArmFakeEngine() {
    g_engine = FakeEngine();
    // The registration path only exists in a contained offline replay -- csregister's OnTick
    // refuses it in any other process (livesafety::Feature::GameSpaceRegister). This binary never
    // installs the offline-bit lock, so the un-overridden verdict here is LockNotHeld; every case
    // that expects the engine to be reached must say which process it is. The gate itself is
    // driven by the live case below.
    replay::livesafety::SetForTest(/*liveOnlineExempt=*/false, /*offlineLockHeld=*/true);
    replay::ui::csregister::ResetForTest();
    replay::ui::inframe::ResetForTest();
    ArmFrameOpen();
    replay::ui::csregister::SetEngineForTest(&FakeLookup, &FakeRegister);
    replay::ui::csregister::SetDrawFn(&NoDraw);
    // The shipping order (src/ui/ui_module.cpp UiInit): the module installs the draw and OPENS
    // the panel before the first tick, without waiting on any registration (AUDIT_F3I F3I-4).
    replay::ui::csregister::Enable();
}

}  // namespace

// ARCH-10 P2. L5 row 1 read `status=SpaceNotReady invocations=0 registrations=0 deferrals=7032`
// because OnTick resolved the space through IDENT.NETGAME_GAMESPACE_OFF (+0x8), which is the
// CR15Game back-pointer, and judged its render-target budget block as a table header. The three
// cases below are the three answers that read differs on, and no two of them share a status.
TEST_CASE("the space comes from +0x2B08; the CR15Game at +0x8 is refused and told apart") {
    using replay::ui::csregister::Status;

    // The space is at NETGAME.CS_SPACE_OFF and the CR15Game decoy sits at +0x8 carrying the
    // recorded crash header. An accessor pointed back at +0x8 finds that decoy, refuses on it and
    // never registers -- this CHECK is what reds under that mutation.
    SyntheticSpace s;
    ArmFakeEngine();
    s.Tick();
    replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::Registered);
    CHECK(r.registrations == 1);
    CHECK(r.deferrals == 0);
    CHECK(r.gameSpace == s.space.data());
    CHECK(g_engine.lookups == 1);

    // The mirror: the same, real, ready space planted at +0x8 only. Not registered, and the reason
    // is the true one -- the slot was readable and empty, not "the tables are not built".
    ArmFakeEngine();
    NetGameRig mirror;
    mirror.SetGame(s.space.data());
    mirror.SetSpace(nullptr);
    replay::ui::csregister::OnTick(mirror.Ptr(), ::GetCurrentThreadId());
    r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::NoGameSpace);
    CHECK(r.registrations == 0);
    CHECK(r.deferrals == 0);
    CHECK(g_engine.lookups == 0);

    // And a pointer too narrow to carry the slot is a third answer: the question could not be
    // asked. Committed page, nothing after it, so the read is refused by the memory manager rather
    // than by a table verdict.
    void* const narrow = ::VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    REQUIRE(narrow != nullptr);
    ArmFakeEngine();
    replay::ui::csregister::OnTick(narrow, ::GetCurrentThreadId());
    r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::NetGameUnreadable);
    CHECK(r.registrations == 0);
    CHECK(r.deferrals == 0);
    CHECK(g_engine.lookups == 0);
    CHECK(strcmp(replay::ui::csregister::StatusName(Status::NetGameUnreadable), "<invalid>") != 0);
    ::VirtualFree(narrow, 0, MEM_RELEASE);

    replay::ui::csregister::ResetForTest();
}

// RECORDER-3's last band-3 write. Registering touches the LIVE space twice -- the registrar appends
// a 72-byte record into its update-record table, and the engine's lookup sorts the component-system
// table IN PLACE before searching it -- so a process that is not a contained offline replay must
// reach neither call. This drives the SHIPPING OnTick; the fake registrar and the fake lookup are
// the two witnesses, and the second half is the control that makes the zeros mean something.
TEST_CASE("live safety: a non-contained process opens the UI and registers nothing") {
    using replay::ui::csregister::Status;

    SyntheticSpace s;
    ArmFakeEngine();   // ... which also OPENS the panel, exactly as UiInit does, before any tick
    replay::livesafety::SetForTest(/*liveOnlineExempt=*/true, /*offlineLockHeld=*/false);
    REQUIRE(replay::livesafety::Allowed(replay::livesafety::Feature::GameSpaceRegister) == false);

    for (int i = 0; i < 4; ++i) s.Tick();

    replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    CHECK(r.ownerTicks == 4);            // the ticks really ran, so the zeros below are refusals
    CHECK(r.status == Status::LiveInert);
    CHECK(r.registrations == 0);
    CHECK(g_engine.registrations == 0);
    CHECK(g_engine.lookups == 0);        // the sorting lookup was not called either
    CHECK(s.RecCount() == 0);            // and nothing was appended into the space's own table
    CHECK(strcmp(replay::ui::csregister::StatusName(Status::LiveInert), "LiveInert") == 0);
    // The module stays open through all of it: the live session DRAWS on every owner tick, it just
    // does not register. And the report does not overclaim what it never confirmed.
    CHECK(r.ownerDraws == 4);
    CHECK(r.enableStamps == 1);
    CHECK(r.runningPerDesign == false);

    // THE CONTROL. Same space, same fixture, same tick -- only the verdict changes. It registers,
    // so the zeros above are this gate's doing and not an unready space or a disarmed fixture.
    replay::livesafety::SetForTest(/*liveOnlineExempt=*/false, /*offlineLockHeld=*/true);
    REQUIRE(replay::livesafety::Allowed(replay::livesafety::Feature::GameSpaceRegister) == true);
    s.Tick();
    r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::Registered);
    CHECK(r.registrations == 1);
    CHECK(g_engine.registrations == 1);
    CHECK(g_engine.lookups == 1);
    CHECK(s.RecCount() == 1);

    replay::livesafety::ResetForTest();
    replay::ui::csregister::ResetForTest();
}

TEST_CASE("F3: a space rebuilt under the same pointer is detected as Stale and re-registered") {
    using replay::ui::csregister::Status;
    SyntheticSpace s;
    ArmFakeEngine();

    s.Tick();
    replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    REQUIRE(r.status == Status::Registered);
    CHECK(r.registrations == 1);
    CHECK(r.recordsBefore == 0);
    CHECK(r.recordsAfter == 1);
    CHECK(g_engine.lookups == 1);
    CHECK(g_engine.lastOrderKey == replay::ui::csregister::kOrderKey);
    // Our record sits in the table, so a second tick on the same pointer is the fast path.
    s.Tick();
    r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::Registered);
    CHECK(r.staleDetected == 0);
    CHECK(r.registrations == 1);
    CHECK(r.spaceChanges == 0);

    // The engine's constructor runs on the block: a new base and a zero count, the pointer to the
    // space itself unchanged. Our record is in memory nothing dispatches from.
    std::vector<uint8_t> rebuilt(kFakeRecordCapacity * 72, 0);
    s.RecBase() = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(rebuilt.data()));
    s.RecCount() = 0;
    s.Tick();
    r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::Stale);
    CHECK(r.staleDetected == 1);
    CHECK(r.registrations == 1);   // detection is not a registration
    CHECK(strstr(replay::ui::csregister::Diagnostic(), "STALE") != nullptr);
    CHECK(strcmp(replay::ui::csregister::StatusName(Status::Stale), "Stale") == 0);

    // The next tick re-registers into the SAME pointer, and the fake engine sees the append.
    s.Tick();
    r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::Registered);
    CHECK(r.registrations == 2);
    CHECK(r.recordsBefore == 0);
    CHECK(r.recordsAfter == 1);
    CHECK(r.spaceChanges == 0);
    CHECK(g_engine.registrations == 2);
    CHECK(s.RecCount() == 1);

    // The engine dispatches the record on the main thread: Running, and the stale count stays.
    REQUIRE(g_engine.lastPmfFn != nullptr);
    reinterpret_cast<void (*)(void*)>(g_engine.lastPmfFn)(&s.csObject);
    s.Tick();
    r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::Running);
    CHECK(r.invocations == 1);
    CHECK(r.staleDetected == 1);

    replay::ui::csregister::ResetForTest();
}

TEST_CASE("the lookup is never called on a table the engine has not sorted") {
    using replay::ui::csregister::Status;
    SyntheticSpace s;
    ArmFakeEngine();
    s.CsFlags() = 1;   // bit 0: unsorted, so CS.LOOKUP_BY_SYMBOL would sort it in place

    s.Tick();
    replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::SpaceNotReady);
    CHECK(r.unsortedDeferrals == 1);
    CHECK(r.deferrals == 1);
    CHECK(r.lastRefusal == Refusal::UnsortedTable);
    CHECK(g_engine.lookups == 0);
    CHECK(g_engine.registrations == 0);
    CHECK(strstr(replay::ui::csregister::Diagnostic(), "unsorted") != nullptr);

    s.CsFlags() = 0;
    s.Tick();
    r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::Registered);
    CHECK(r.unsortedDeferrals == 1);
    CHECK(g_engine.lookups == 1);

    replay::ui::csregister::ResetForTest();
}

TEST_CASE("a live space growing its table is reported, not re-registered") {
    using replay::ui::csregister::Status;
    SyntheticSpace s;
    ArmFakeEngine();
    s.CsCount() = 3;   // room to grow inside the block

    s.Tick();
    REQUIRE(replay::ui::csregister::Status_().status == Status::Registered);
    s.CsCount() = 4;
    s.Tick();
    s.Tick();
    const replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::Registered);
    CHECK(r.headerMoved == 1);   // one move, however many ticks look at it
    CHECK(r.registrations == 1);
    CHECK(r.staleDetected == 0);
    CHECK(g_engine.registrations == 1);

    replay::ui::csregister::ResetForTest();
}

TEST_CASE("a registration nothing dispatches becomes Unconfirmed, which is not Registered") {
    using replay::ui::csregister::Status;
    SyntheticSpace s;
    ArmFakeEngine();

    s.Tick();
    REQUIRE(replay::ui::csregister::Status_().status == Status::Registered);
    for (uint64_t i = 0; i + 1 < replay::ui::csregister::kUnconfirmedTicks; ++i) s.Tick();
    replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::Registered);
    CHECK(r.unconfirmedTicks == replay::ui::csregister::kUnconfirmedTicks - 1);
    s.Tick();
    r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::Unconfirmed);
    CHECK(r.unconfirmedTicks == replay::ui::csregister::kUnconfirmedTicks);
    CHECK(r.registrations == 1);
    CHECK(r.staleDetected == 0);
    CHECK(strcmp(replay::ui::csregister::StatusName(Status::Unconfirmed), "Unconfirmed") == 0);
    CHECK(strstr(replay::ui::csregister::Diagnostic(), "UNCONFIRMED") != nullptr);

    // A late first dispatch still resolves it.
    reinterpret_cast<void (*)(void*)>(g_engine.lastPmfFn)(&s.csObject);
    s.Tick();
    CHECK(replay::ui::csregister::Status_().status == Status::Running);

    replay::ui::csregister::ResetForTest();
}

TEST_CASE("a space change is counted, logged with its five fields, and registered into anew") {
    using replay::ui::csregister::Status;
    SyntheticSpace a;
    ArmFakeEngine();
    a.Tick();
    REQUIRE(replay::ui::csregister::Status_().status == Status::Registered);
    replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    CHECK(r.spaceChanges == 0);
    CHECK(r.lastSpaceChange.gameSpace == a.space.data());
    CHECK(r.lastSpaceChange.ourIndex == -1);   // first seen before we registered into it

    SyntheticSpace b;
    b.CsCount() = 2;
    a.ng.SetSpace(b.space.data());
    a.Tick();
    r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::Registered);
    CHECK(r.spaceChanges == 1);
    CHECK(r.registrations == 2);
    CHECK(r.lastSpaceChange.gameSpace == b.space.data());
    CHECK(r.lastSpaceChange.cs.raw.count == 2);
    CHECK(r.lastSpaceChange.cs.raw.capacity == 4);
    CHECK(r.lastSpaceChange.rec.raw.count == 0);
    CHECK(r.lastSpaceChange.cs.verdict == replay::tableguard::TableVerdict::Ready);
    CHECK(r.lastSpaceChange.ourIndex == -1);
    CHECK(r.gameSpace == b.space.data());

    replay::ui::csregister::ResetForTest();
}

// AUDIT_W2-F_R2 R2-2, the auditor's scenario verbatim. CSREGISTER-EPOCH section 2: "a space is
// Running only when the engine has invoked ThunkDraw on the main thread at least once since the
// last registration into it." The lifetime invocation total cannot say that, and this is the case
// where the difference is the whole answer.
TEST_CASE("a dispatch from a space we have left does not confirm the registration in the new one") {
    using replay::ui::csregister::Status;
    SyntheticSpace a;
    ArmFakeEngine();
    a.Tick();
    REQUIRE(replay::ui::csregister::Status_().status == Status::Registered);

    // A's registration is confirmed by A's own component system.
    REQUIRE(g_engine.lastPmfFn != nullptr);
    void (*const dispatch)(void*) = reinterpret_cast<void (*)(void*)>(g_engine.lastPmfFn);
    dispatch(&a.csObject);
    a.Tick();
    replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    REQUIRE(r.status == Status::Running);
    CHECK(r.registrationInvocations == 1);
    CHECK(r.staleInvocations == 0);

    // The game space changes. Registration is one-way, so A goes on dispatching our record.
    SyntheticSpace b;
    b.CsCount() = 2;
    a.ng.SetSpace(b.space.data());
    a.Tick();
    r = replay::ui::csregister::Status_();
    REQUIRE(r.spaceChanges == 1);
    REQUIRE(r.registrations == 2);
    CHECK(r.registrationInvocations == 0);
    CHECK(r.componentSystem == &b.csObject);

    // A's dispatch arrives. It is a real invocation and it draws, but it belongs to A.
    dispatch(&a.csObject);
    r = replay::ui::csregister::Status_();
    // ★ ARCH-13 844d21b s1.2 ROWS L1/L2, AND THE CONSEQUENCE THEY CARRY, PINNED SO IT IS NOT
    // SILENT. Leg 2 is `invocations > 0` exactly as S8.5.4a writes it, and `invocations` is a
    // LIFETIME total: here every one of them belongs to the space we left, which goes on calling
    // us because registration is one-way, and B's own record has never been dispatched. All three
    // legs hold, so the word is Running -- and the counter that says whose dispatch it was is
    // published beside it and is what AUDIT_W2-F_R3 f1's protection now rests on (the leg-1 rung
    // at L1, and W2-R's row, which requires registrationInvocations > 0 to pass).
    CHECK(r.status == Status::Running);
    CHECK(r.runningPerDesign == true);
    CHECK(r.registrationInvocations == 0);
    CHECK(r.staleInvocations == 1);
    CHECK(r.invocations == 2);
    CHECK(r.registrationInvocations == 0);
    CHECK(r.staleInvocations == 1);
    CHECK(r.lastStaleDispatch == &a.csObject);

    // Nothing ever dispatches B's record: after K ticks B is Unconfirmed, not Running, and the
    // diagnostic names the stale dispatches rather than leaving them to be read as B's.
    for (uint64_t i = 0; i < replay::ui::csregister::kUnconfirmedTicks + 100; ++i) a.Tick();
    r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::Unconfirmed);
    CHECK(r.unconfirmedTicks == replay::ui::csregister::kUnconfirmedTicks + 100);
    CHECK(r.registrationInvocations == 0);
    CHECK(r.staleInvocations == 1);
    CHECK(strstr(replay::ui::csregister::Diagnostic(), "did arrive from a component system we are "
                                                       "not registered with") != nullptr);

    // B's own component system is the only thing that can confirm B.
    dispatch(&b.csObject);
    a.Tick();
    r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::Running);
    CHECK(r.registrationInvocations == 1);
    CHECK(r.staleInvocations == 1);
    CHECK(r.invocations == 3);

    replay::ui::csregister::ResetForTest();
}

// AUDIT_W2-F_R3 finding 1, the auditor's scenario verbatim. The window between a space change and
// the new registration is the case the deferred L5/L3 check exists for: while TryRegister is
// refusing, we are registered in NOTHING, and the space we left goes on dispatching our record.
TEST_CASE("dispatches during a refused re-registration are stale, and cannot read as Running") {
    using replay::ui::csregister::Status;
    SyntheticSpace a;
    ArmFakeEngine();
    a.Tick();
    REQUIRE(replay::ui::csregister::Status_().status == Status::Registered);
    REQUIRE(g_engine.lastPmfFn != nullptr);
    void (*const dispatch)(void*) = reinterpret_cast<void (*)(void*)>(g_engine.lastPmfFn);
    dispatch(&a.csObject);
    replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    REQUIRE(r.status == Status::Running);
    REQUIRE(r.registrationInvocations == 1);

    // Space B is still being built: its component-system table carries the unsorted bit, so
    // TryRegister refuses on every tick and there is no registration to attribute anything to.
    SyntheticSpace b;
    b.CsFlags() = 1;
    a.ng.SetSpace(b.space.data());
    for (int i = 0; i < 5; ++i) a.Tick();
    r = replay::ui::csregister::Status_();
    REQUIRE(r.status == Status::SpaceNotReady);
    REQUIRE(r.registrations == 1);
    REQUIRE(r.deferrals == 5);
    CHECK(r.registrationInvocations == 0);

    // A's record keeps firing. Every one of those belongs to a registration we no longer hold.
    for (int i = 0; i < 400; ++i) dispatch(&a.csObject);
    r = replay::ui::csregister::Status_();
    CHECK(r.status != Status::Running);
    CHECK(r.status == Status::SpaceNotReady);
    CHECK(r.invocations == 401);
    CHECK(r.registrationInvocations == 0);
    CHECK(r.staleInvocations == 400);
    CHECK(r.lastStaleDispatch == &a.csObject);

    // B becomes registerable. A's dispatches stay stale; only B's own component system confirms B.
    b.CsFlags() = 0;
    a.Tick();
    r = replay::ui::csregister::Status_();
    REQUIRE(r.registrations == 2);
    dispatch(&a.csObject);
    r = replay::ui::csregister::Status_();
    // ARCH-13 row L2 again: the word is the three legs and a stale space satisfies leg 2. What
    // must never be lost is WHOSE dispatch it was, and that is these two counters.
    CHECK(r.staleInvocations == 401);
    CHECK(r.registrationInvocations == 0);
    dispatch(&b.csObject);
    r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::Running);
    CHECK(r.registrationInvocations == 1);
    CHECK(r.staleInvocations == 401);

    replay::ui::csregister::ResetForTest();
}

// AUDIT_W2-F_R4 finding 1, the STATIC half of the R3 finding: SetRegistration sits BELOW the
// after <= before test in TryRegister, so a registrar call that returns without moving the record
// count must install no attribution target at all -- not the space we arrived in, not the one we
// left. Nothing else in the suite reaches RegistrarUnavailable, so nothing else sees that ordering.
TEST_CASE("AUDR4: a RegistrarUnavailable refusal does not install an attribution target") {
    using replay::ui::csregister::Status;
    SyntheticSpace a;
    ArmFakeEngine();
    a.Tick();
    REQUIRE(replay::ui::csregister::Status_().status == Status::Registered);
    void (*const dispatch)(void*) = reinterpret_cast<void (*)(void*)>(g_engine.lastPmfFn);
    dispatch(&a.csObject);
    replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    REQUIRE(r.status == Status::Running);
    REQUIRE(r.registrationInvocations == 1);

    // Space B is fully readable -- every early refusal is passed and the registrar IS called -- but
    // it returns without appending, so the record count does not move.
    SyntheticSpace b;
    replay::ui::csregister::SetEngineForTest(&FakeLookup, &NoMoveRegister);
    a.ng.SetSpace(b.space.data());
    a.Tick();
    r = replay::ui::csregister::Status_();
    REQUIRE(r.status == Status::RegistrarUnavailable);
    CHECK(r.registrationInvocations == 0);

    // B was never a completed registration, so B's own component system is stale too.
    dispatch(&b.csObject);
    r = replay::ui::csregister::Status_();
    CHECK(r.staleInvocations == 1);
    CHECK(r.registrationInvocations == 0);
    CHECK(r.status != Status::Running);
    CHECK(r.status == Status::RegistrarUnavailable);

    // And so is A's, the space we left.
    dispatch(&a.csObject);
    r = replay::ui::csregister::Status_();
    CHECK(r.staleInvocations == 2);
    CHECK(r.registrationInvocations == 0);
    CHECK(r.status == Status::RegistrarUnavailable);

    replay::ui::csregister::ResetForTest();
}

// AUDIT_W2-F_R2 R2-3. The space-change record is the evidence the deferred L5/L3 check reads, and
// the first tick of a space whose header is not mapped yet is exactly what the epoch is about.
TEST_CASE("a space change whose header could not be read is not recorded as an empty one") {
    using replay::tableguard::TableVerdict;
    using replay::tableguard::TableWhy;
    SyntheticSpace a;
    ArmFakeEngine();
    a.CsCount() = 0;   // built, mapped, and holding nothing: an EMPTY component-system table
    a.Tick();
    replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    CHECK(r.lastSpaceChange.cs.verdict == TableVerdict::Empty);
    CHECK(r.lastSpaceChange.cs.raw.count == 0);

    // Reserved, never committed: the header reads as nothing at all, and the raw fields it leaves
    // behind are the same zeroes the empty table above left.
    void* unmapped = ::VirtualAlloc(nullptr, 0x1000, MEM_RESERVE, PAGE_NOACCESS);
    REQUIRE(unmapped != nullptr);
    a.ng.SetSpace(unmapped);
    a.Tick();
    r = replay::ui::csregister::Status_();
    CHECK(r.lastSpaceChange.gameSpace == unmapped);
    CHECK(r.lastSpaceChange.cs.raw.count == 0);          // identical to the Empty case above ...
    CHECK(r.lastSpaceChange.cs.verdict == TableVerdict::Unreadable);   // ... and told apart by this
    CHECK(r.lastSpaceChange.cs.why == TableWhy::HeaderUnreadable);
    CHECK(r.lastSpaceChange.rec.verdict == TableVerdict::Unreadable);
    ::VirtualFree(unmapped, 0, MEM_RELEASE);

    replay::ui::csregister::ResetForTest();
}

// =================================================================================================
// M5 -- DESIGN 10.5's "mute voice off 1.0x". AUDIT_WAVE1B M5 / W2-G.md section 7: "no mute-voice
// control". The player refuses every rate but 1.0x forward by itself, so the panel's half is the
// state and the player's own count, drawn on every transport frame.
// =================================================================================================

TEST_CASE("the transport view says whether voice is live, and how much the player held back") {
    Fixture fx;
    PressKey();   // the toggle brings the panel up with the browser closed
    fx.Frame();

    fx.transport.state = TransportState::Play;
    fx.transport.rate = 1.0f;
    uifake::VoipStatsForTest().injected = 42;
    fx.Frame();
    CHECK(uifake::TextContains("voice: live at 1.0x (42 frame(s) injected)"));
    CHECK_FALSE(uifake::TextContains("muted off 1.0x"));

    fx.transport.rate = 2.0f;
    uifake::VoipStatsForTest().skippedRate = 7;
    fx.Frame();
    CHECK(uifake::TextContains("voice: muted off 1.0x (playing, rate 2.00x)"));
    CHECK(uifake::TextContains("7 frame(s) held back at rate"));
    CHECK_FALSE(uifake::TextContains("voice: live"));

    // Paused at 1.0x is not live either: the player delivers forward only.
    fx.transport.state = TransportState::Pause;
    fx.transport.rate = 1.0f;
    uifake::VoipStatsForTest().skippedNoStep = 3;
    fx.Frame();
    CHECK(uifake::TextContains("voice: muted off 1.0x (paused, rate 1.00x)"));
    CHECK(uifake::TextContains("3 while paused or scrubbing"));
}

// =================================================================================================
// Nothing loaded. W2-B gave the transport a word for "there is no file"; before it, a fileless
// transport answered Pause and every ui drive call ran against it -- SetRate reached Play(rate)
// and returned true, so the console printed a rate that had been applied to nothing.
// =================================================================================================

// =================================================================================================
// OWNER-WATCH-FIX Q2. "reversing or setting speed unpauses." Every rate and direction control had
// to go through Play() because IReplayTransport had no SetRate, and Play ends with SetPaused(false).
// =================================================================================================

TEST_CASE("a speed change while paused does not start playback") {
    Fixture fx;
    fx.transport.state = TransportState::Pause;
    fx.transport.rate = 1.0f;
    const uint32_t playsBefore = fx.transport.playCalls;

    const char* why = nullptr;
    REQUIRE(ui::SetRate(0.5, &why));
    CHECK(fx.transport.playCalls == playsBefore);
    CHECK(fx.transport.setRateCalls == 1);
    CHECK(fx.transport.lastSetRate == doctest::Approx(0.5f));

    // The control, and it is the half that keeps the guard from being "never call Play": the same
    // call on a RUNNING transport must still be a Play, or a speed change would silently stop it.
    fx.transport.state = TransportState::Play;
    REQUIRE(ui::SetRate(0.25, &why));
    CHECK(fx.transport.playCalls == playsBefore + 1);
    CHECK(fx.transport.lastPlayRate == doctest::Approx(0.25f));
    CHECK(fx.transport.setRateCalls == 1);
}

TEST_CASE("a negative speed while paused stays paused, and while playing keeps playing") {
    Fixture fx;
    fx.transport.state = TransportState::Pause;
    const char* why = nullptr;
    REQUIRE(ui::SetRate(-1.0, &why));
    CHECK(fx.transport.playCalls == 0);
    CHECK(fx.transport.lastSetRate == doctest::Approx(-1.0f));

    fx.transport.state = TransportState::Play;
    REQUIRE(ui::SetRate(-1.0, &why));
    CHECK(fx.transport.playCalls == 1);
    CHECK(fx.transport.lastPlayRate == doctest::Approx(-1.0f));
}

TEST_CASE("the panel names the state where it used to print a question mark") {
    Fixture fx;
    PressKey();   // the toggle brings the panel up with the browser closed
    fx.Frame();

    fx.transport.state = TransportState::NotLoaded;
    fx.Frame();
    CHECK(uifake::TextContains("no replay loaded"));

    // The control: the other states still name themselves, so this is a new case and not a
    // constant that would read the same however TransportStateName were wired.
    fx.transport.state = TransportState::Pause;
    fx.Frame();
    CHECK(uifake::TextContains("paused"));
    CHECK_FALSE(uifake::TextContains("no replay loaded"));
}

TEST_CASE("with no capture open, no drive command reaches the transport") {
    Fixture fx;
    fx.transport.state = TransportState::NotLoaded;
    const uint32_t before = ui::NotLoadedRefusals();

    const char* why = nullptr;
    CHECK_FALSE(ui::SetRate(2.0, &why));
    REQUIRE(why != nullptr);
    CHECK(strstr(why, "nothing loaded") != nullptr);

    CHECK_FALSE(ui::SeekUs(1000));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    CHECK_FALSE(ui::SeekFraction(0.5));
    CHECK_FALSE(ui::SeekRoundNumber(2));

    // The refusal is what the transport did NOT receive. A gate that returns false and forwards
    // anyway would pass every check above and still write a rate over no file.
    CHECK(fx.transport.playCalls == 0);
    CHECK(fx.transport.seekCalls == 0);
    CHECK(ui::NotLoadedRefusals() == before + 4);
}

TEST_CASE("the same calls go through once a capture is open") {
    // The control. Without it, a SetRate that refused everything unconditionally would pass the
    // test above, and the refusal would read as working while having disabled the feature.
    Fixture fx;
    fx.transport.state = TransportState::Pause;
    const uint32_t before = ui::NotLoadedRefusals();

    const char* why = nullptr;
    CHECK(ui::SetRate(2.0, &why));
    // It reaches the transport -- as a RATE, because this fixture is paused. OWNER-WATCH-FIX split
    // the two: the rate lands now, the playing starts when the user asks for it.
    CHECK(fx.transport.setRateCalls == 1);
    CHECK(fx.transport.lastSetRate == doctest::Approx(2.0f));
    CHECK(fx.transport.playCalls == 0);

    fx.transport.state = TransportState::Play;
    CHECK(ui::SetRate(2.0, &why));
    CHECK(fx.transport.playCalls == 1);
    CHECK(fx.transport.lastPlayRate == doctest::Approx(2.0f));
    fx.transport.state = TransportState::Pause;

    CHECK(ui::SeekUs(1000));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    CHECK(fx.transport.seekCalls == 1);
    CHECK(fx.transport.lastSeekUs == 1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot

    CHECK(ui::NotLoadedRefusals() == before);
}

// ---------------------------------------------------------------------------------------------
// THE MARSHAL (L5-RUN-3 F6). The band-3 callback is dispatched on a CTaskKernel worker thread --
// 4681 of 4681 entries in the field -- and ImGui is not thread-safe, so it draws nothing at all:
// it records the entry into a lock-free mailbox and the owner thread runs the draw.
//
// What these cases pin, and what each of them reds under:
//   * no draw ever happens on a thread that is not the owner   <- draw directly in the callback
//   * the marshalled draw does happen, once, on the owner      <- drop the drain from OnTick
//   * no invocation disappears without a number                <- coalesce without counting
//   * Running means all three legs, and each failure is named  <- fold a leg back into Running
//   * a half-written mailbox record is refused, not reported   <- read the payload without the
//                                                                 sequence check
// ---------------------------------------------------------------------------------------------
namespace {

uint32_t g_drawThread = 0;
uint64_t g_drawCalls = 0;

// The thread id is recorded rather than asserted here: doctest's assertions are not thread-safe,
// and a CHECK that fires on a worker is a CHECK whose failure may not be counted. The owner thread
// reads it back after the join, where the thread handle's wait gives the ordering for free.
replay::ui::inframe::DrawOutcome RecordingDraw() {
    g_drawThread = ::GetCurrentThreadId();
    ++g_drawCalls;
    // Asks the facade the way panel::Draw does and forwards its verdict, so a shut frame reaches
    // the counters as a refusal here too rather than only in test_inframe.cpp.
    bool open = true;
    const bool shown = replay::ui::gui::Begin("Echo Replay", &open, 0);
    replay::ui::gui::End();
    return shown ? replay::ui::inframe::DrawOutcome::Drawn
                 : replay::ui::inframe::DrawOutcome::FacadeRefused;
}

struct DispatchJob {
    void (*fn)(void*) = nullptr;
    void* arg = nullptr;
    uint32_t times = 0;
};

DWORD WINAPI DispatchOnWorker(void* p) {
    DispatchJob* const job = static_cast<DispatchJob*>(p);
    for (uint32_t i = 0; i < job->times; ++i) job->fn(job->arg);
    return 0;
}

// Runs the engine's dispatch on a REAL second thread and returns its id. A fake thread id passed
// to the callback would prove nothing: the refusal is keyed on GetCurrentThreadId, so only an
// actual other thread exercises it.
uint32_t DispatchFromWorker(void (*fn)(void*), void* arg, uint32_t times) {
    DispatchJob job;
    job.fn = fn;
    job.arg = arg;
    job.times = times;
    DWORD tid = 0;
    HANDLE h = ::CreateThread(nullptr, 0, &DispatchOnWorker, &job, 0, &tid);
    REQUIRE(h != nullptr);
    REQUIRE(::WaitForSingleObject(h, 30000) == WAIT_OBJECT_0);
    ::CloseHandle(h);
    return static_cast<uint32_t>(tid);
}

void (*ArmedDispatch())(void*) {
    REQUIRE(g_engine.lastPmfFn != nullptr);
    return reinterpret_cast<void (*)(void*)>(g_engine.lastPmfFn);
}

void ArmMarshalTest() {
    g_drawThread = 0;
    g_drawCalls = 0;
    ArmFakeEngine();
    // After ArmFakeEngine's ResetForTest+Enable, so the recording draw is the one that runs.
    replay::ui::csregister::SetDrawFn(&RecordingDraw);
}

// The READ-point hook (AUDIT_F3E F3E-3). The publish hook below stands inside the publish window
// with the seqlock ODD, which the entry check alone already rejects, so it exercises only the first
// of the seqlock's two guards -- the auditor's mutant on the completion re-check survived the whole
// suite. This hook stands between the reader's payload copy and its re-check and lets a WRITER run
// to completion there (even -> even+2), which is the in-vivo tear and the only thing the re-check
// can see. The publish runs on a real worker thread: an inline one would re-enter the drain from
// inside the read.
void (*g_readHookDispatch)(void*) = nullptr;
void* g_readHookArg = nullptr;
uint64_t g_readHookCalls = 0;
bool g_readHookOnce = false;

uint32_t DispatchFromWorker(void (*fn)(void*), void* arg, uint32_t times);

void PublishInsideRead() {
    ++g_readHookCalls;
    if (g_readHookOnce) replay::ui::csregister::SetReadHookForTest(nullptr);
    DispatchFromWorker(g_readHookDispatch, g_readHookArg, 1);
}

bool g_hookReadOk = true;
replay::ui::csregister::Invocation g_hookRead;
uint64_t g_hookOwnerSequence = 0;

// Stands inside the publish window, with the record half written and the seqlock odd. This is the
// only deterministic way to be there; two racing threads would make the control a coin toss.
void ReadInsidePublish() {
    g_hookReadOk = replay::ui::csregister::ReadInvocationForTest(&g_hookRead);
    g_hookOwnerSequence = replay::ui::csregister::Status_().lastInvocation.sequence;
}

}  // namespace

TEST_CASE("the band-3 callback draws on no thread; the owner thread draws exactly once") {
    using replay::ui::csregister::Report;
    using replay::ui::csregister::Status;

    ArmMarshalTest();
    SyntheticSpace s;
    const uint32_t owner = ::GetCurrentThreadId();
    s.Tick();
    REQUIRE(replay::ui::csregister::Status_().status == Status::Registered);

    // ★ AUDIT_F3I F3I-4. The registering tick has ALREADY drawn: the panel is open because this
    // module opened it, not because the registrar accepted the record.
    REQUIRE(g_drawCalls == 1);
    REQUIRE(g_drawThread == owner);

    const uint32_t worker = DispatchFromWorker(ArmedDispatch(), &s.csObject, 1);
    REQUIRE(worker != owner);

    // THE INVARIANT. Nothing was drawn on the worker: the count does not move across the
    // dispatch and the drawing thread is never the worker's. A callback that draws inline reds
    // this pair.
    CHECK(g_drawCalls == 1);
    CHECK(g_drawThread == owner);

    Report r = replay::ui::csregister::Status_();
    CHECK(r.invocations == 1);
    CHECK(r.offThreadInvocations == 1);
    CHECK(r.registrationInvocations == 1);
    CHECK(r.pendingInvocations == 1);
    // ★ AUD-ARCH F-B. The panel has drawn -- ownerDraws == 1 -- and leg 3 is still unmet,
    // because no frame has yet carried a band-3 entry. One counter could not say both.
    CHECK(r.ownerDraws == 1);
    CHECK(r.marshalledDraws == 0);
    CHECK(r.wrongThreadDrains == 0);
    // ARCH-14 3c4e669: leg 3 is ownerDraws, and this tick DREW -- so all three legs hold and the
    // word is Running, which ARCH-14 names MODULE LIVENESS and not evidence of a visible panel.
    // `Invoked` is now reachable only with the panel closed; the case below covers it.
    CHECK(r.status == Status::Running);

    s.Tick();

    CHECK(g_drawCalls == 2);
    CHECK(g_drawThread == owner);
    r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::Running);
    CHECK(r.ownerDraws == 2);
    // This frame carried the worker's entry, so leg 3 is met on it and only on it.
    CHECK(r.marshalledDraws == 1);
    CHECK(r.pendingInvocations == 0);
    CHECK(r.coalescedInvocations == 0);
    CHECK(r.wrongThreadDrains == 0);
    CHECK(r.mailboxTornReads == 0);
    // The record the worker published, read whole on the owner thread.
    CHECK(r.lastInvocation.sequence == 1);
    CHECK(r.lastInvocation.threadId == worker);
    CHECK(r.lastInvocation.orderKey == replay::ui::csregister::kOrderKey);
    CHECK(r.lastInvocation.monotonicNs != 0);

    // ★ AUDIT_F3E F3E-4. A tick with nothing pending still owes the panel its frame: ImGui is
    // immediate mode, so a frame that does not call panel::Draw has no panel in it at all.
    s.Tick();
    CHECK(g_drawCalls == 3);
    CHECK(replay::ui::csregister::Status_().ownerDraws == 3);
    // ...and leg 3 does NOT move on it: no entry arrived, so no frame carried one (AUD-ARCH F-B).
    CHECK(replay::ui::csregister::Status_().marshalledDraws == 1);
}

// ★★ THE RENDER-CADENCE INVARIANT (AUDIT_F3E F3E-4). Restoring `if (pending == 0) return;` to
// DrainMarshal reds this case on both counters: the panel would then be emitted only on the frames
// an asynchronous CTaskKernel task happened to fire on (~26.8/s measured on L5 re-run 3), and a
// panel that is absent from the other frames is a panel that blinks.
TEST_CASE("the panel is drawn on every owner tick, with no band-3 entry at all") {
    ArmMarshalTest();
    SyntheticSpace s;
    s.Tick();   // the registering tick, which draws like every other one (AUDIT_F3I F3I-4)
    REQUIRE(replay::ui::csregister::Status_().status ==
            replay::ui::csregister::Status::Registered);
    REQUIRE(g_drawCalls == 1);

    const uint64_t ticks = 100;
    for (uint64_t i = 0; i < ticks; ++i) s.Tick();

    const replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    CHECK(g_drawCalls == ticks + 1);
    CHECK(r.ownerDraws == ticks + 1);
    CHECK(r.ownerTicks == ticks + 1);
    // ★ AUD-ARCH F-B. Leg 3 is UNMET over 101 drawn frames, and that is the honest answer: the
    // owner is looking at the panel AND the band-3 path has never reached the screen. Before the
    // split one counter claimed both, so leg 3 passed on a session band 3 never touched.
    CHECK(r.marshalledDraws == 0);
    CHECK(r.runningPerDesign == false);
    // Not one band-3 entry arrived over any of them, which is the whole point: the mailbox informs
    // ordering and telemetry, and the frame's cadence is the tick's.
    CHECK(r.invocations == 0);
    CHECK(r.drainedInvocations == 0);
    CHECK(r.pendingInvocations == 0);
    // The denominator F3E#2 asks the launch for: every open tick drew, so the ratio is 1.0 --
    // not 1.0 minus the registering tick, because that tick drew too.
    CHECK(r.ownerDraws == r.ownerTicks);
}

// ★ THE SAME 101 TICKS WITH THE ENGINE'S FRAME SHUT (ARCH-15 S4.1, AUDIT_F3J F3J-4/5). This is the
// case the old fixture could not express: before the frame byte was modelled, `Begin` returned true
// unconditionally and ownerDraws counted 101 draws on a build where NRadGui accepted none of them.
// The counter above and the counter here must not be able to give the same answer.
TEST_CASE("a hundred ticks with the frame shut are a hundred REFUSALS, not a hundred draws") {
    ArmMarshalTest();
    SyntheticSpace s;
    s.Tick();   // the registering tick, drawn into a frame that is still open
    REQUIRE(replay::ui::csregister::Status_().registrations == 1);
    REQUIRE(g_drawCalls == 1);

    g_frame.frameOpenByte = 0;          // StartFrame never opened it this frame
    const uint64_t before = replay::ui::csregister::Status_().ownerDraws;

    const uint64_t ticks = 100;
    for (uint64_t i = 0; i < ticks; ++i) s.Tick();

    const replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    CHECK(r.ownerTicks == ticks + 1);
    CHECK(r.ownerDraws == before);                  // not one of the 100 is credited
    CHECK(r.fallbackEntries == ticks + 1);          // ...but every one of them was ATTEMPTED
    CHECK(r.refusedDraws == ticks);
    CHECK(r.refusedNoFrameByte == ticks);
    CHECK(r.inFrameDraws == before);
    CHECK(r.runningPerDesign == false);             // leg 3 is inFrameDraws and it did not move
    CHECK(r.frameSiteEntries == 0);                 // the detour is not armed in this fixture

    g_frame.frameOpenByte = 1;
}

// ★★ AUDIT_F3I F3I-5. csregister.h claimed the mailbox kept "the compositing slot in the one
// case where the engine dispatches us inline on the owner thread" -- and drewThisTick suppressed
// exactly that draw, so the only draw the tree ASSERTS is inside an open GUI frame was the one
// being dropped. The rule now matches the claim: the inline dispatch is the draw that lands, and
// the owner tick is the fallback for a tick that gets no inline dispatch. Both orders are here.
//
// MUTANT: delete the `site == DrawSite::OwnerTick && g.inlineDispatchLastTick` arm in
// DrainMarshal (the pre-F3I code) and ORDER 2 reds -- inlineDraws 0 and g_drawCalls 2.
TEST_CASE("the inline band-3 dispatch takes the draw, and the owner tick is the fallback") {
    using replay::ui::csregister::Report;

    ArmMarshalTest();
    SyntheticSpace s;
    s.Tick();
    void (*const dispatch)(void*) = ArmedDispatch();

    // ORDER 1 -- owner tick first, then inline. The frame already has its panel, so the dispatch
    // must not stack a second one: emitting it twice puts two windows in one frame.
    s.Tick();
    REQUIRE(g_drawCalls == 2);
    dispatch(&s.csObject);
    dispatch(&s.csObject);
    Report r = replay::ui::csregister::Status_();
    CHECK(g_drawCalls == 2);
    CHECK(r.ownerDraws == 2);
    CHECK(r.inlineDraws == 0);
    CHECK(r.invocations == 2);
    CHECK(r.coalescedInvocations == 2);
    CHECK(r.drainedInvocations == 2);
    CHECK(r.pendingInvocations == 0);
    // The frame that drew is the frame those entries were folded into, so leg 3 is met by it
    // (AUD-ARCH F-B) -- once for the frame, not once per entry.
    CHECK(r.marshalledDraws == 1);

    // ORDER 2 -- the engine having dispatched us inline, the next tick stands aside and the
    // dispatch draws. That call is inside the GUI frame the band-3 slot was registered for,
    // which is the whole reason this packet registers at band 3.
    s.Tick();
    CHECK(g_drawCalls == 2);        // the tick deferred: this frame has no panel yet
    dispatch(&s.csObject);
    r = replay::ui::csregister::Status_();
    CHECK(g_drawCalls == 3);
    CHECK(r.inlineDraws == 1);
    CHECK(r.ownerDraws == 3);
    CHECK(r.inlineDeferrals == 1);
    CHECK(r.inlineDeferralsUnfilled == 0);
    CHECK(r.marshalledDraws == 2);

    // Exactly once per tick still holds from this side too: a second inline dispatch in the
    // frame the first one drew is folded, not drawn.
    dispatch(&s.csObject);
    CHECK(g_drawCalls == 3);
    CHECK(replay::ui::csregister::Status_().inlineDraws == 1);

    // STEADY STATE. An engine that dispatches inline every frame gives every frame its panel from
    // the inline slot, and the tick goes on standing aside for it -- no frame is lost and no frame
    // gets two. Deferring is not a latch either: the dispatcher stopping costs one frame and the
    // tick takes the draw back (see the next case).
    s.Tick();
    CHECK(g_drawCalls == 3);
    dispatch(&s.csObject);
    CHECK(g_drawCalls == 4);
    r = replay::ui::csregister::Status_();
    CHECK(r.inlineDraws == 2);
    CHECK(r.ownerDraws == 4);
    CHECK(r.inlineDeferrals == 2);
    CHECK(r.inlineDeferralsUnfilled == 0);
}

// The price of preferring the inline slot, stated and bounded: a dispatcher that goes from
// inline to asynchronous costs ONE frame, once, and the frame is counted rather than lost
// silently. That is exactly the one tick of slack ARCH-13 s2.3 allows, which is why a
// dispatcher that flapped every tick would red the coverage rule -- correctly.
TEST_CASE("an inline dispatcher that stops costs exactly one frame, and it is counted") {
    using replay::ui::csregister::Report;

    ArmMarshalTest();
    SyntheticSpace s;
    s.Tick();
    void (*const dispatch)(void*) = ArmedDispatch();
    s.Tick();
    dispatch(&s.csObject);          // the engine dispatches inline...
    const uint64_t before = g_drawCalls;

    s.Tick();                       // ...so this tick stands aside for one that never comes
    CHECK(g_drawCalls == before);
    s.Tick();                       // and the tick takes the draw back on the very next one
    CHECK(g_drawCalls == before + 1);

    const Report r = replay::ui::csregister::Status_();
    CHECK(r.inlineDeferrals == 1);
    CHECK(r.inlineDeferralsUnfilled == 1);
    CHECK(probeui::DrawCoverageHolds(r.ownerDraws, r.ownerTicks) == true);
}

TEST_CASE("invocations a drain could not draw are counted, not dropped") {
    using replay::ui::csregister::Report;

    ArmMarshalTest();
    SyntheticSpace s;
    s.Tick();
    DispatchFromWorker(ArmedDispatch(), &s.csObject, 5);
    CHECK(g_drawCalls == 1);   // the registering tick's own draw, and nothing from the worker

    s.Tick();

    // One drain, one draw: five panels emitted into one frame would stack five windows. The four
    // that did not get their own draw are a number, which is the difference between coalescing
    // and losing.
    CHECK(g_drawCalls == 2);
    const Report r = replay::ui::csregister::Status_();
    CHECK(r.invocations == 5);
    CHECK(r.ownerDraws == 2);
    // One of the two frames carried the five entries, so leg 3 is credited once (AUD-ARCH F-B).
    CHECK(r.marshalledDraws == 1);
    CHECK(r.coalescedInvocations == 4);
    // The identity that holds at every pending: every entry is either drained or still queued.
    // (The two-term marshalledDraws + coalesced == invocations form is false as soon as a tick
    // draws with an empty mailbox, which is now every idle tick.)
    CHECK(r.drainedInvocations + r.pendingInvocations == r.invocations);
    CHECK(r.drainedInvocations == 5);
    CHECK(r.pendingInvocations == 0);
}

// An owner tick that has stopped cannot be measured in owner ticks -- there are none -- so the
// backlog is what detects it. This is MarshalStale, which is NOT a leg of Running (F3E-1): all
// three of S8.5.4a's legs hold here and the word says what the instrument, not the definition, saw.
TEST_CASE("an owner thread that stops ticking is MarshalStale, with the backlog named") {
    using replay::ui::csregister::Status;

    ArmMarshalTest();
    SyntheticSpace s;
    s.Tick();   // the one tick that registers -- without a registration there is no callback
    // One entry, carried into the next tick's frame. Leg 3 is marshalledDraws, the
    // band-3-consumed subset (AUD-ARCH F-B), so it needs a frame that CARRIED an entry -- a
    // frame alone is ownerDraws and says nothing about band 3.
    DispatchFromWorker(ArmedDispatch(), &s.csObject, 1);
    s.Tick();
    REQUIRE(replay::ui::csregister::Status_().marshalledDraws == 1);

    const uint32_t dispatched = 200;
    DispatchFromWorker(ArmedDispatch(), &s.csObject, dispatched);

    const replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::MarshalStale);
    CHECK(r.invocations == dispatched + 1);
    CHECK(r.pendingInvocations == dispatched);
    CHECK(r.marshalledDraws == 1);
    CHECK(r.ownerDraws == 2);
    CHECK(g_drawCalls == 2);
    CHECK(strcmp(replay::ui::csregister::StatusName(Status::MarshalStale), "MarshalStale") == 0);
    CHECK(strstr(replay::ui::csregister::Diagnostic(), "MARSHAL STALE") != nullptr);

    // Drainable, not broken: the backlog clears in one tick and the count survives it.
    s.Tick();
    const replay::ui::csregister::Report after = replay::ui::csregister::Status_();
    CHECK(after.status == Status::Running);
    CHECK(after.coalescedInvocations == dispatched - 1);
    CHECK(after.drainedInvocations == dispatched + 1);
    CHECK(after.pendingInvocations == 0);
}

// Legs 1 and 2 without leg 3 is S8.5.4a's `Invoked`. Leg 3 is ownerDraws (ARCH-14 3c4e669), and
// since AUDIT_F3I F3I-4 the draw is gated by the module's OWN open state -- so the one way to be
// here is a registered, dispatched module whose panel is closed (or whose draw was never
// installed). It is no longer reachable by a registration that simply has not been dispatched.
TEST_CASE("a registration dispatched with the panel closed is Invoked, not Running") {
    using replay::ui::csregister::Status;

    ArmMarshalTest();
    replay::ui::csregister::Disable();
    SyntheticSpace s;
    s.Tick();
    DispatchFromWorker(ArmedDispatch(), &s.csObject, 3);

    const replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::Invoked);
    CHECK(r.ownerDraws == 0);
    CHECK(r.marshalledDraws == 0);
    CHECK(r.ownerTicks == 1);
    CHECK(strcmp(replay::ui::csregister::StatusName(Status::Invoked), "Invoked") == 0);
    CHECK(strstr(replay::ui::csregister::Diagnostic(), "INVOKED, NEVER DRAWN") != nullptr);

    // ★ F3E-1. DESIGN S8.5.4a scores leg 2 as `invocations > 0` -- NOT registrationInvocations,
    // which is the form that disagreed with the design on the only record we have (L5 re-run 3:
    // invocations=4681 registrationInvocations=0). Open the panel and one tick makes all three
    // legs hold.
    replay::ui::csregister::Enable();
    s.Tick();
    const replay::ui::csregister::Report after = replay::ui::csregister::Status_();
    CHECK(after.status == Status::Running);
    CHECK(after.registrations >= 1);
    CHECK(after.invocations > 0);
    CHECK(after.ownerDraws > 0);
    // The band-3 diagnostic moved too, on this tick, because the frame carried the three
    // entries -- but it gates nothing (ARCH-14).
    CHECK(after.marshalledDraws == 1);
}

// =================================================================================================
// L5-RUN-5 G1. THE PUBLISHED LINE IS A CONTRACT AND ITS BUFFER IS SIZED FROM ITS INPUTS.
//
// On run #5 `published[640]` cut the line at byte 639 -- inside `[inframe ...]` -- in exactly the
// branch that reports trouble, and `_TRUNCATE` said nothing. The `[inframe ...]`, `[stage0 ...]`
// and `[threads ...]` groups vanished from the one launch they were built for, and three W2-R rows
// went could-not-ask on an artifact that had been asked correctly. The two cases below are the
// ones that would have caught it: one formats the LONGEST branch and parses every group back, the
// other sizes the budget against the format string itself at its widest.
// =================================================================================================

// The longest branch there is: the `INVOKED, NEVER DRAWN` prose (540 bytes with every conversion a
// single digit) AND the ARCH-15 S7 premise sentence AND the five fixed groups. Every group is read
// back with the same shape W2-R's parsers use, so a line that fits but has lost a field still reds.
TEST_CASE("the longest diagnostic branch publishes a COMPLETE line, every group parsed back") {
    using replay::ui::csregister::Status;

    ArmMarshalTest();
    replay::ui::csregister::Disable();

    // Arm the premise sentence too -- an open frame byte at the owner tick. It is the widest the
    // prose side ever gets, and on the shipped 640 it made the cut fall even earlier.
    static uint8_t frameOpen = 1;
    static uint8_t latch = 1;
    static const void* ctx = &frameOpen;
    replay::ui::inframe::SetProbeForTest(&frameOpen, &latch, &ctx);

    SyntheticSpace s;
    s.Tick();
    DispatchFromWorker(ArmedDispatch(), &s.csObject, 3);

    const replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    REQUIRE(r.status == Status::Invoked);
    REQUIRE(r.frameOpenAtOwnerTickNonZero > 0);          // the premise sentence is on the line

    const char* const line = replay::ui::csregister::Diagnostic();
    CHECK(strstr(line, "INVOKED, NEVER DRAWN") != nullptr);
    CHECK(strstr(line, "GUI FRAME OPEN AT STAGE 0") != nullptr);

    // ** THE BOUND ITSELF. This line is longer than the whole suffix budget on its own, so no
    // buffer sized for the prose plus a fixed guess at the groups can carry it -- which is exactly
    // what the shipped one was. It also has to fit the budget it declares, with room for the NUL.
    const size_t len = strlen(line);
    CHECK(len > replay::ui::csregister::PublishedSuffixBudgetForTest());
    CHECK(len < replay::ui::csregister::PublishedLineBudgetForTest() - 1);

    // Every group, read the way its reader reads it. `[inframe ...]` is the ten-field parse from
    // tests/replay/observer_rows.h:909; the other four are the groups W2-F publishes beside it.
    unsigned long long ticksOwner = 0, ticksDraws = 0;
    const char* at = strstr(line, " [ticks owner=");
    REQUIRE(at != nullptr);
    CHECK(sscanf_s(at, " [ticks owner=%llu draws=%llu]", &ticksOwner, &ticksDraws) == 2);

    unsigned long long e = 0, d = 0, ref = 0, nb = 0, nl = 0, nc = 0, fac = 0, cl = 0, fb = 0;
    int installed = -1;
    at = strstr(line, " [inframe entries=");
    REQUIRE(at != nullptr);
    CHECK(sscanf_s(at,
                   " [inframe entries=%llu draws=%llu refused=%llu nobyte=%llu nolatch=%llu "
                   "noctx=%llu facade=%llu closed=%llu fallback=%llu installed=%d]",
                   &e, &d, &ref, &nb, &nl, &nc, &fac, &cl, &fb, &installed) == 10);
    CHECK(installed >= 0);

    unsigned long long samples = 0, nonzero = 0, lasttick = 0;
    at = strstr(line, " [stage0 frameopen samples=");
    REQUIRE(at != nullptr);
    CHECK(sscanf_s(at, " [stage0 frameopen samples=%llu nonzero=%llu lasttick=%llu]", &samples,
                   &nonzero, &lasttick) == 3);
    CHECK(nonzero == r.frameOpenAtOwnerTickNonZero);

    unsigned long long nodraw = 0, beginfalse = 0;
    char reason[16] = {};
    unsigned bound = 0, entries = 0;
    at = strstr(line, " [facade nodraw=");
    REQUIRE(at != nullptr);
    CHECK(sscanf_s(at, " [facade nodraw=%llu beginfalse=%llu reason=%15[a-z] bound=%u/%u]", &nodraw,
                   &beginfalse, reason, static_cast<unsigned>(sizeof reason), &bound,
                   &entries) == 5);
    CHECK(nodraw + beginfalse == fac);          // the breakdown and the aggregate are one fact

    unsigned long owner = 0, callback = 0;
    at = strstr(line, " [threads owner=");
    REQUIRE(at != nullptr);
    CHECK(sscanf_s(at, " [threads owner=%lu callback=%lu]", &owner, &callback) == 2);
    // observer_rows.h:1662 reads a line that does not END in this group as a broken contract.
    CHECK(strchr(at + 1, '[') == at + 1);          // the group we are standing on...
    CHECK(strchr(at + 2, '[') == nullptr);          // ...and nothing after it

    replay::ui::inframe::ResetForTest();
}

// The budget is sized from the FORMAT, at its widest, not from a line somebody happened to see.
// Every %llu can render 20 digits and every %lu ten; a bound that only fits this launch's numbers
// is the same defect one order of magnitude away.
TEST_CASE("the published suffix fits its declared budget with every field at its widest") {
    const char* const fmt = replay::ui::csregister::PublishedSuffixFormatForTest();
    const size_t budget = replay::ui::csregister::PublishedSuffixBudgetForTest();
    REQUIRE(fmt != nullptr);

    std::vector<char> buf(budget);
    const unsigned long long m = ~0ull;
    const unsigned long ml = ~0ul;
    const unsigned mu = ~0u;
    const int wrote =
        _snprintf_s(buf.data(), buf.size(), _TRUNCATE, fmt, m, m, m, m, m, m, m, m, m, m, m, -1, m,
                    m, m, m, m, "unnamed", mu, mu, -1, m, m, m, m, m, m, m, "0123456789abcdef", m, m, ml, ml);
    CHECK(wrote > 0);                                   // -1 is _TRUNCATE saying it did not fit
    CHECK(static_cast<size_t>(wrote) < budget);

    // ...and the whole line's budget is the sum of its three declared parts, so no branch can
    // overrun it: prose + premise + suffix.
    CHECK(replay::ui::csregister::PublishedLineBudgetForTest() >= 512u + 256u + budget);
}

TEST_CASE("a dispatch that stops is CallbackSilent, which is neither Running nor Unconfirmed") {
    using replay::ui::csregister::Status;

    ArmMarshalTest();
    SyntheticSpace s;
    s.Tick();
    DispatchFromWorker(ArmedDispatch(), &s.csObject, 1);
    s.Tick();
    REQUIRE(replay::ui::csregister::Status_().status == Status::Running);

    for (uint64_t i = 0; i < replay::ui::csregister::kLivenessTicks; ++i) s.Tick();

    const replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::CallbackSilent);
    CHECK(r.registrationInvocations == 1);
    // The panel kept being drawn the whole time it was silent -- the dispatch is what stopped, and
    // the cadence of the panel is not the dispatcher's (F3E-4). That cadence is ownerDraws;
    // marshalledDraws stayed at the one frame that carried the one entry (AUD-ARCH F-B).
    CHECK(r.ownerDraws == 2 + replay::ui::csregister::kLivenessTicks);
    CHECK(r.marshalledDraws == 1);
    CHECK(strcmp(replay::ui::csregister::StatusName(Status::CallbackSilent), "CallbackSilent") == 0);
    CHECK(strstr(replay::ui::csregister::Diagnostic(), "CALLBACK SILENT") != nullptr);

    // The two silences are different questions with different answers. A registration that has
    // never once been dispatched is Unconfirmed and names the band trap and the gate bit; this one
    // WAS dispatched and stopped, so neither of those causes applies and it must not borrow them.
    CHECK(r.status != Status::Unconfirmed);

    // It recovers on the next dispatch rather than latching.
    DispatchFromWorker(ArmedDispatch(), &s.csObject, 1);
    s.Tick();
    CHECK(replay::ui::csregister::Status_().status == Status::Running);
}

TEST_CASE("a mailbox record read mid-publish is refused, never reported half written") {
    using replay::ui::csregister::Invocation;

    ArmMarshalTest();
    SyntheticSpace s;
    s.Tick();
    void (*const dispatch)(void*) = ArmedDispatch();

    dispatch(&s.csObject);   // on the owner thread: published and drained in one call
    REQUIRE(replay::ui::csregister::Status_().lastInvocation.sequence == 1);

    g_hookReadOk = true;
    g_hookOwnerSequence = 0;
    replay::ui::csregister::SetPublishHookForTest(&ReadInsidePublish);
    dispatch(&s.csObject);   // sequence 2; the hook reads with the seqlock odd
    replay::ui::csregister::SetPublishHookForTest(nullptr);

    // THE CONTROL. Without the sequence check this read succeeds and hands back sequence 2 beside
    // whatever thread id and timestamp the previous record left, which is the torn record.
    CHECK(g_hookReadOk == false);
    CHECK(replay::ui::csregister::Status_().mailboxTornReads == 1);
    // And what the owner could see meanwhile was the last WHOLE record, not the one being written.
    CHECK(g_hookOwnerSequence == 1);

    // The same read outside the window succeeds, so the refusal above is a refusal and not a read
    // that never works.
    Invocation whole;
    CHECK(replay::ui::csregister::ReadInvocationForTest(&whole) == true);
    CHECK(whole.sequence == 2);
    CHECK(whole.threadId == ::GetCurrentThreadId());
    CHECK(whole.orderKey == replay::ui::csregister::kOrderKey);
    CHECK(replay::ui::csregister::Status_().mailboxTornReads == 1);
}

// ★ AUDIT_F3E F3E-3. The seqlock has TWO guards and the control above covers only the entry one:
// a reader parked with the sequence odd is refused before it ever reaches the completion re-check,
// and the auditor's mutant (`return true;` in place of the re-check) survived all 683 assertions.
// The tear this covers is the in-vivo one -- a writer that starts AND finishes between the reader's
// two reads, leaving the sequence even again -- which only the re-check can see.
TEST_CASE("a writer that runs to completion INSIDE a read is caught by the completion re-check") {
    using replay::ui::csregister::Invocation;

    ArmMarshalTest();
    SyntheticSpace s;
    s.Tick();
    g_readHookDispatch = ArmedDispatch();
    g_readHookArg = &s.csObject;
    g_readHookCalls = 0;

    DispatchFromWorker(ArmedDispatch(), &s.csObject, 1);   // record 1, whole
    s.Tick();
    REQUIRE(replay::ui::csregister::Status_().lastInvocation.sequence == 1);
    const uint64_t tornBefore = replay::ui::csregister::Status_().mailboxTornReads;

    // ONE writer inside the window, then the coast is clear. The reader must NOT hand back the
    // record it copied before that writer ran: it must notice the sequence moved and read again.
    g_readHookOnce = true;
    replay::ui::csregister::SetReadHookForTest(&PublishInsideRead);
    Invocation got;
    const bool ok = replay::ui::csregister::ReadInvocationForTest(&got);
    replay::ui::csregister::SetReadHookForTest(nullptr);

    CHECK(ok == true);
    CHECK(g_readHookCalls == 1);
    // THE DISCRIMINATOR. Without the re-check this is 1 -- the record copied before the writer ran.
    CHECK(got.sequence == 2);
    CHECK(replay::ui::csregister::Status_().mailboxTornReads == tornBefore);

    // And a writer that never stops: the read is bounded, refuses, and is COUNTED. Without the
    // re-check the very first attempt returns a spliced record as whole and this counter stays put.
    g_readHookOnce = false;
    g_readHookCalls = 0;
    replay::ui::csregister::SetReadHookForTest(&PublishInsideRead);
    Invocation spliced;
    const bool refused = replay::ui::csregister::ReadInvocationForTest(&spliced);
    replay::ui::csregister::SetReadHookForTest(nullptr);

    CHECK(refused == false);
    CHECK(g_readHookCalls == 64);   // it retried, bounded -- it did not spin on the game thread
    CHECK(replay::ui::csregister::Status_().mailboxTornReads == tornBefore + 1);

    // The refusal is a refusal, not a read that never works.
    Invocation whole;
    CHECK(replay::ui::csregister::ReadInvocationForTest(&whole) == true);
    CHECK(whole.sequence == replay::ui::csregister::Status_().invocations);
}

// ★ AUDIT_F3E F3E-2 (BLOCKER). The probe scored `marshalledDraws == 0` as FAIL unconditionally,
// and its sampler broke the 150 s window on exactly that condition -- so it shortened the window
// it was judging and then recorded the short window as a verdict. Both decisions now live in
// probe_ui_outcome.h, where they can be red without a launch.
TEST_CASE("a window that never reached a drawable frame is could-not-ask, never FAIL") {
    using replay::ui::csregister::Status;

    // Censored: nothing was drawn AND no tick could have drawn. This is the reading DESIGN
    // S8.5.4a consequence 3 reserves, and scoring it FAIL is the blocker this case exists for.
    CHECK(probeui::DrawWindowIsCensored(0, 0) == true);
    CHECK(probeui::DrawWindowIsCensored(0, 1) == true);   // only the registering tick ran
    // ASKED AND FAILED, so the row is not unfailable: ticks ran, the panel was enabled by the
    // registration those ticks made, and nothing was drawn.
    CHECK(probeui::DrawWindowIsCensored(0, 2) == false);
    CHECK(probeui::DrawWindowIsCensored(0, 9000) == false);
    // Answered: a draw happened, so the question is not censored whatever else is true.
    CHECK(probeui::DrawWindowIsCensored(1, 0) == false);
    CHECK(probeui::DrawWindowIsCensored(9000, 9000) == false);

    // ★ ARCH-13 s2.3 item 1. EXISTENCE IS NOT COVERAGE: one draw in four thousand owner ticks is
    // the flickering panel the render-cadence ruling forbids, and it used to be a pass.
    CHECK(probeui::DrawCoverageHolds(1, 4000) == false);
    CHECK(probeui::DrawCoverageHolds(3999, 4000) == true);    // the one-tick slack, and no more
    CHECK(probeui::DrawCoverageHolds(3998, 4000) == false);
    CHECK(probeui::DrawCoverageHolds(4000, 4000) == true);
    CHECK(probeui::DrawCoverageHolds(0, 0) == true);          // cannot underflow at zero ticks
    CHECK(probeui::DrawCoverageHolds(0, 1) == true);          // the registering tick alone
    CHECK(probeui::DrawCoverageHolds(0, 2) == false);

    // The window must never be shortened by the condition it judges. These two ARE that condition.
    CHECK(probeui::SamplerMayBreakOn(Status::Unconfirmed, 0) == false);
    CHECK(probeui::SamplerMayBreakOn(Status::Invoked, 0) == false);
    // Not even once a draw has happened: neither state can hold with marshalledDraws > 0, and a
    // break that fires on the state alone is a break on the judged condition.
    CHECK(probeui::SamplerMayBreakOn(Status::Unconfirmed, 5) == false);
    CHECK(probeui::SamplerMayBreakOn(Status::Invoked, 5) == false);
    // These two are determinations: the callback fired, the panel drew, and then it stopped.
    CHECK(probeui::SamplerMayBreakOn(Status::CallbackSilent, 5) == true);
    CHECK(probeui::SamplerMayBreakOn(Status::MarshalStale, 5) == true);
    // ...but never on the strength of the word alone, with no draw behind it.
    CHECK(probeui::SamplerMayBreakOn(Status::CallbackSilent, 0) == false);
    CHECK(probeui::SamplerMayBreakOn(Status::MarshalStale, 0) == false);
    // A healthy run is waited out to the end of its window, not cut short.
    CHECK(probeui::SamplerMayBreakOn(Status::Running, 5) == false);
    CHECK(probeui::SamplerMayBreakOn(Status::Registered, 0) == false);
}

TEST_CASE("the epoch clause registrations == spacesSeen can both hold and break") {
    using replay::ui::csregister::Report;

    // ONE space, one registration: the clause HOLDS. spaceChanges is 0 here and always will be on
    // a single-space session, which is exactly why the clause cannot be written against it
    // 1 == 0 is never true, so that form could only ever fail.
    ArmMarshalTest();
    SyntheticSpace a;
    a.Tick();
    Report r = replay::ui::csregister::Status_();
    CHECK(r.registrations == 1);
    CHECK(r.spacesSeen == 1);
    CHECK(r.spaceChanges == 0);
    CHECK(r.registrations == r.spacesSeen);

    // A SECOND space: both move together, and the clause still holds.
    SyntheticSpace b;
    a.ng.SetSpace(b.space.data());
    a.Tick();
    r = replay::ui::csregister::Status_();
    CHECK(r.registrations == 2);
    CHECK(r.spacesSeen == 2);
    CHECK(r.spaceChanges == 1);
    CHECK(r.registrations == r.spacesSeen);

    // THE CONTROL, so the clause is not one that only ever passes. A space that is SEEN and
    // refused registration moves spacesSeen and not registrations, and the clause breaks.
    SyntheticSpace c;
    c.CsFlags() = 1;   // unsorted: the lookup would sort in place, so the site refuses
    a.ng.SetSpace(c.space.data());
    a.Tick();
    r = replay::ui::csregister::Status_();
    CHECK(r.spacesSeen == 3);
    CHECK(r.registrations == 2);
    CHECK(r.registrations != r.spacesSeen);
    CHECK(r.unsortedDeferrals == 1);
    CHECK(r.lastRefusal == Refusal::UnsortedTable);
}

TEST_CASE("every diagnostic line ends with both thread ids in the published grammar") {
    ArmMarshalTest();
    SyntheticSpace s;
    const uint32_t owner = ::GetCurrentThreadId();
    s.Tick();

    // The DIRECTION of an off-thread dispatch was unreadable from the artifacts
    // because neither id reached one. Both are on every line now, in one fixed shape.
    char expected[96];
    _snprintf_s(expected, sizeof expected, _TRUNCATE, " [threads owner=%lu callback=%lu]",
                static_cast<unsigned long>(owner), 0ul);
    const char* line = replay::ui::csregister::Diagnostic();
    CHECK(strstr(line, expected) != nullptr);
    // At the END of the line, not merely somewhere in it: a parser takes the last 40-odd bytes.
    CHECK(strlen(line) > strlen(expected));
    CHECK(strcmp(line + strlen(line) - strlen(expected), expected) == 0);

    const uint32_t worker = DispatchFromWorker(ArmedDispatch(), &s.csObject, 1);
    s.Tick();
    _snprintf_s(expected, sizeof expected, _TRUNCATE, " [threads owner=%lu callback=%lu]",
                static_cast<unsigned long>(owner), static_cast<unsigned long>(worker));
    line = replay::ui::csregister::Diagnostic();
    CHECK(strcmp(line + strlen(line) - strlen(expected), expected) == 0);
    // Reading it twice does not append the suffix twice.
    const size_t once = strlen(replay::ui::csregister::Diagnostic());
    CHECK(strlen(replay::ui::csregister::Diagnostic()) == once);
}

namespace {

// A registrar that returns without touching the record table -- the AUDIT_W2-F_AVFIX F5 shape, and
// the shape of every band-3 failure whose inside the plugin cannot see.
void RefusingRegister(void*, void*, const void*, const char*, const char*, uint16_t, uint16_t,
                      const void*, uint16_t) {}

}  // namespace

// ★★ AUDIT_F3I F3I-4 (BLOCKER). THE OWNER GETS THE VIEWER EVEN IF THE ENGINE HOOK NEVER LANDS.
// `g.enabled` used to have exactly one true-writer -- TryRegister's success path -- so a launch on
// which band-3 registration never landed drew NOTHING, and that is the difference between a replay
// viewer and no replay viewer. The enable is the UI module's own open state now; registration buys
// compositing order and telemetry.
//
// MUTANT for both cases below: put `g.enabled = true;` back in TryRegister's success path and empty
// Enable(), and both red on their draw counts (0 drawn instead of 40).
TEST_CASE("registration is never attempted and the panel is drawn on every owner tick anyway") {
    using replay::ui::csregister::Report;
    using replay::ui::csregister::Status;

    ArmMarshalTest();
    const uint32_t owner = ::GetCurrentThreadId();
    const uint64_t ticks = 40;
    // No netGame at all: OnTick returns before it ever reaches TryRegister, which is the menu, and
    // is also what a launch whose engine hook never fires looks like from in here.
    for (uint64_t i = 0; i < ticks; ++i) replay::ui::csregister::OnTick(nullptr, owner);

    const Report r = replay::ui::csregister::Status_();
    CHECK(r.registrations == 0);
    CHECK(r.status == Status::NoGameSpace);
    CHECK(g_drawCalls == ticks);
    CHECK(r.ownerDraws == ticks);
    CHECK(r.ownerTicks == ticks);
    CHECK(r.ownerDraws == r.ownerTicks);
    // And the report does not overclaim: leg 3 is unmet and runningPerDesign is false, because
    // band 3 never ran. The panel is on screen; the compositing slot was never confirmed.
    CHECK(r.marshalledDraws == 0);
    CHECK(r.runningPerDesign == false);
}

TEST_CASE("the registrar refuses the record and the panel is drawn on every owner tick anyway") {
    using replay::ui::csregister::Report;
    using replay::ui::csregister::Status;

    ArmMarshalTest();
    replay::ui::csregister::SetEngineForTest(&FakeLookup, &RefusingRegister);
    SyntheticSpace s;
    const uint64_t ticks = 40;
    for (uint64_t i = 0; i < ticks; ++i) s.Tick();

    const Report r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::RegistrarUnavailable);
    CHECK(r.registrations >= 1);
    CHECK(r.recordsAfter == r.recordsBefore);
    CHECK(g_drawCalls == ticks);
    CHECK(r.ownerDraws == ticks);
    CHECK(r.marshalledDraws == 0);
    CHECK(r.runningPerDesign == false);
}

// ★★ AUDIT_F3I F3I-3 (BLOCKER). ARCH-13 s2.3 scores `drawsAfter >= ownerTicksAfter - 1` ACROSS A
// STAMP, and ownerTicks is a LIFETIME total counted above every early return in OnTick. Scored on
// the lifetime totals, a launch that sat in the menu for 600 ticks and then drew on all 3400 of the
// rest files an asked-FAIL reading "599 unaccounted for" against a panel that drew on every frame
// it was allowed to. csregister publishes the baseline at the stamp; the probe subtracts.
//
// MUTANT: score the coverage rule on r.ownerDraws / r.ownerTicks (the pre-F3I probe) and the
// second CHECK below is what the launch would have filed.
TEST_CASE("the coverage rule is scored over the panel's own window, not the module's lifetime") {
    using replay::ui::csregister::Report;

    ArmMarshalTest();
    const uint32_t owner = ::GetCurrentThreadId();
    replay::ui::csregister::Disable();     // the menu: our tick is running, the browser is closed
    const uint64_t closed = 600;
    for (uint64_t i = 0; i < closed; ++i) replay::ui::csregister::OnTick(nullptr, owner);
    REQUIRE(g_drawCalls == 0);

    replay::ui::csregister::Enable();
    const uint64_t open = 3400;
    for (uint64_t i = 0; i < open; ++i) replay::ui::csregister::OnTick(nullptr, owner);

    const Report r = replay::ui::csregister::Status_();
    CHECK(r.ownerTicks == closed + open);
    CHECK(r.ownerDraws == open);
    CHECK(r.ownerTicksAtEnable == closed);
    CHECK(r.ownerDrawsAtEnable == 0);
    // Opened at init, closed for the menu, opened again: the stamp moved with the second open.
    CHECK(r.enableStamps == 2);

    // THE DEFECT, REPRODUCED: on the lifetime totals this launch reads as 599 missing frames.
    CHECK(probeui::DrawCoverageHolds(r.ownerDraws, r.ownerTicks) == false);
    // AND THE FIX: over the window the panel was actually open it drew on every single tick.
    const probeui::DrawWindow w = probeui::DrawWindowSinceEnable(r);
    CHECK(w.valid == true);
    CHECK(w.everOpened == true);
    CHECK(w.ownerTicks == open);
    CHECK(w.draws == open);
    CHECK(probeui::DrawCoverageHolds(w.draws, w.ownerTicks) == true);
    CHECK(probeui::DrawWindowIsCensored(w.draws, w.ownerTicks) == false);
}

// The two could-not-ask arms of the window, which are the reason it is a struct and not a pair of
// numbers: neither is a FAIL, and a two-state gate could not tell either of them from one.
TEST_CASE("a window that was never opened, or that was reset under the observer, is could-not-ask") {
    using replay::ui::csregister::Report;

    replay::ui::csregister::ResetForTest();
    const probeui::DrawWindow never = probeui::DrawWindowSinceEnable(replay::ui::csregister::Status_());
    CHECK(never.everOpened == false);
    CHECK(never.valid == false);
    CHECK(never.draws == 0);
    CHECK(never.ownerTicks == 0);

    // Counters BEHIND their own baseline: ARCH-13 s2.3 item 4, the module reset under the reader.
    Report reset = replay::ui::csregister::Status_();
    reset.enableStamps = 1;
    reset.ownerTicksAtEnable = 600;
    reset.ownerDrawsAtEnable = 600;
    reset.ownerTicks = 12;
    reset.ownerDraws = 12;
    const probeui::DrawWindow backwards = probeui::DrawWindowSinceEnable(reset);
    CHECK(backwards.everOpened == true);
    CHECK(backwards.valid == false);
}

// ---------------------------------------------------------------------------------------------
// ScoreLiveness scored leg 3 on ownerDraws while runningPerDesign -- another field of
// the same Report -- scored it on inFrameDraws, and ownerDraws' one increment site stands down
// whenever the row-2.5 detour is armed, which is the shipped configuration. So on the build that
// draws correctly the word was Invoked forever, and MarshalStale / CallbackSilent / Running below
// that line were all unreachable. Every other Running case in this file runs with the detour
// UNARMED, which is exactly why the suite was green on it, so this case arms it BEFORE the first
// tick: ownerDraws can never leave 0 here and only the site's own draws can satisfy leg 3.
//
// The mutant this case exists to red: put ScoreLiveness's leg 3 back on `r.ownerDraws`.
// ---------------------------------------------------------------------------------------------
namespace {

uint64_t __fastcall FrameSiteEntry(void*) { return 0; }

}  // namespace

TEST_CASE("with the row-2.5 detour armed, leg 3 is inFrameDraws and the word is Running") {
    using replay::ui::csregister::Status;
    ArmMarshalTest();
    replay::ui::inframe::SetInstalledForTest(true);   // before any tick, so the fallback never runs

    SyntheticSpace s;
    s.Tick();
    REQUIRE(replay::ui::csregister::Status_().registrations == 1);
    ArmedDispatch()(&s.csObject);                     // leg 2, dispatched on the owner thread
    s.Tick();

    replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    REQUIRE(r.invocations == 1);
    REQUIRE(r.ownerDraws == 0);        // the fallback stood down: the old leg 3 is frozen at 0
    REQUIRE(r.inFrameDraws == 0);
    CHECK(r.status == Status::Invoked);          // legs 1+2 without 3, and that IS the right word
    CHECK(r.runningPerDesign == false);

    for (int i = 0; i < 5; ++i) replay::ui::inframe::CallSiteForTest(&FrameSiteEntry, &s);

    r = replay::ui::csregister::Status_();
    CHECK(r.frameSiteEntries == 5);
    CHECK(r.inFrameDraws == 5);
    CHECK(r.ownerDraws == 0);                    // still 0, and it no longer decides anything
    CHECK(r.runningPerDesign == true);
    CHECK(r.status == Status::Running);          // <- Invoked when leg 3 goes back to ownerDraws
    CHECK(strstr(replay::ui::csregister::Diagnostic(), "running:") != nullptr);

    // ...and the band BELOW the three legs is reachable again, which it was not while leg 3 sat on
    // a counter that could never move. kLivenessTicks owner ticks with nothing dispatched is the
    // callback going quiet -- a different word from never having drawn. CallbackSilent rather than
    // MarshalStale because a tick that runs at all resets ticksSinceMarshalledDraw (csregister.cpp
    // line 260 is on the drawing path and pending==0 still reaches it), so of the two recency
    // instruments only the dispatch one can fire while OnTick is still being called.
    for (uint64_t i = 0; i <= replay::ui::csregister::kLivenessTicks; ++i) s.Tick();
    r = replay::ui::csregister::Status_();
    CHECK(r.status == Status::CallbackSilent);
    CHECK(r.runningPerDesign == true);           // the legs hold; this word is the recency one
    CHECK(strstr(replay::ui::csregister::Diagnostic(), "CALLBACK SILENT") != nullptr);

    replay::ui::inframe::SetInstalledForTest(false);
    replay::ui::csregister::ResetForTest();
}

// ==================================================================================================
// "NOT YET" IS NOT "NO" (P10-C s4, ENG-W1F-L3F1 s3).
//
// ★ THE PAIR IS THE TEST. The deferred case alone passes on a LoadPath that never fails at all, so
// the terminal case below runs the SAME fake with the SAME false return and the ONLY difference
// being LastRefusal(). One fixture cannot tell the two readings apart; two can.
//
// The mutant: read the refusal as `!= CannotEnter` instead of `== NotInLevelYet`, or drop the
// LastRefusal() consult entirely. The first reds the terminal case (NotAsked would defer forever);
// the second reds the deferred case.
TEST_CASE("a not-in-level-yet entry refusal DEFERS the load and keeps the capture") {
    Fixture fx;
    const std::wstring cap = fx.dir.File(L"deferred_entry.echodemo");
    REQUIRE(WriteCapture(cap.c_str(), 0xABCDu, Whole(0xDCBAu), 1));

    fx.session.levelId = 0x1;
    fx.session.gametypeId = Whole(0x2);
    fx.session.canChange = true;
    fx.session.requestAccepts = false;
    fx.session.refusal = replay::EntryRefusal::NotInLevelYet;

    // The session survives, so the load does too: the capture opens and the request is re-offered.
    CHECK(ui::LoadPath(cap.c_str()));
    CHECK(fx.session.requestCalls == 1);
    CHECK(ui::LastEntryVerdict() == ui::EntryVerdict::EntryDeferred);
    CHECK(ui::LastEntryVerdict() != ui::EntryVerdict::RequestFailed);
    CHECK(fx.transport.loadCalls == 1);
    CHECK(strstr(ui::LastEntryReason(), "DEFERRED") != nullptr);

    // ★ RE-OFFERED, NOT MERELY WAITED ON. NotInLevelYet is transient by definition; waiting for
    // Matches() without asking again waits forever on a session that became ready and was never
    // asked a second time.
    ui::panel::Sample(16000);
    CHECK(fx.session.requestCalls == 2);
    CHECK(ui::LastEntryVerdict() == ui::EntryVerdict::EntryDeferred);

    fx.session.requestAccepts = true;
    ui::panel::Sample(16000);
    CHECK(fx.session.requestCalls == 3);
    CHECK(ui::LastEntryVerdict() == ui::EntryVerdict::Requested);
}

TEST_CASE("any other entry refusal is terminal and still fails the load") {
    Fixture fx;
    const std::wstring cap = fx.dir.File(L"terminal_entry.echodemo");
    REQUIRE(WriteCapture(cap.c_str(), 0xABCDu, Whole(0xDCBAu), 1));

    fx.session.levelId = 0x1;
    fx.session.gametypeId = Whole(0x2);
    fx.session.canChange = true;
    fx.session.requestAccepts = false;
    fx.session.refusal = replay::EntryRefusal::CannotEnter;

    CHECK_FALSE(ui::LoadPath(cap.c_str()));
    CHECK(ui::LastEntryVerdict() == ui::EntryVerdict::RequestFailed);
    CHECK(fx.transport.loadCalls == 0);

    // And it is not re-offered: a settled no asked again every frame is a spin, not a retry.
    ui::panel::Sample(16000);
    CHECK(fx.session.requestCalls == 1);
}

// =================================================================================================
// THE ROUNDS STRIP, LABELLED BY KIND (W2-F PANEL4). Every button used to read "R" plus
// RoundMarker::index, and panel.cpp never copied RoundMarker::kind out of the manifest at all --
// so on a solo -offline host, where the engine's round counter never leaves 0, twelve markers of
// six kinds all rendered R0. L3-RUN-6.png is twelve identical R0 buttons.
//
// What each case reds under:
//   * the label sequence, byte-exact      <- drop one entry from the kind table (the kind then
//                                            reads as the unknown glyph, which is a different
//                                            string rather than a neighbour's)
//   * the transport page's widget count   <- remove the dedup (10 entries become 12)
//   * the tail                            <- drop the clamp (40 buttons, no "...+n")
// =================================================================================================

namespace {

using replay::capture::MarkKind;

RoundMarker Mark(uint64_t t0Us, uint32_t nth, uint16_t kind, uint16_t index) {
    RoundMarker m{};
    m.tUs = t0Us + static_cast<uint64_t>(nth + 1) * 40000ull;
    m.kind = kind;
    m.index = index;
    return m;
}

// The strip's own buttons, in draw order. The id suffix is the discriminator: the page also draws
// 115 cell buttons per strip and eight transport buttons, and a substring match on the glyph
// would collect those too.
std::string StripLabels() {
    std::string joined;
    for (const std::string& label : uifake::ButtonLabels()) {
        const size_t hash = label.find("##rj");
        if (hash == std::string::npos) continue;
        if (!joined.empty()) joined += "|";
        joined += label.substr(0, hash);
    }
    return joined;
}

constexpr uint64_t kFixtureT0Us = 1700000000000000ull;

// Measured off the drawn frame and pinned here. Exact rather than a ceiling: a ceiling
// survives the dedup being deleted, and the widget count is what catches that.
constexpr uint64_t kTransportItems12 = 300;  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF plain 300 as a count, size or pixel value, not the extrapolation-cap offset
// 1494 + 29: the fidelity banner a capture with no recorded fingerprint now draws is 29 characters
// longer than the CONTENT MISMATCH sentence it replaces (L3-RUN-10 F3). Nothing else in the frame
// changed -- the rate row draws the same labels when the ceiling is unknown.
constexpr uint64_t kTransportGlyphs12 = 1523;

// Twelve markers of six kinds, every index 0 -- the L3-RUN-6 shape, where the old label made all
// twelve read R0. Three consecutive goals are the dedup's subject; the two ROUND_BEGINs are not
// adjacent, so they must stay two buttons.
const RoundMarker kTwelve[12] = {
    Mark(kFixtureT0Us, 0, replay::capture::kMarkMatchBegin, 0),
    Mark(kFixtureT0Us, 1, replay::capture::kMarkRoundBegin, 0),
    Mark(kFixtureT0Us, 2, replay::capture::kMarkLaunchRelease, 0),
    Mark(kFixtureT0Us, 3, replay::capture::kMarkGoal, 0),
    Mark(kFixtureT0Us, 4, replay::capture::kMarkGoal, 0),
    Mark(kFixtureT0Us, 5, replay::capture::kMarkGoal, 0),
    Mark(kFixtureT0Us, 6, replay::capture::kMarkRoundEnd, 0),
    Mark(kFixtureT0Us, 7, replay::capture::kMarkRoundBegin, 0),
    Mark(kFixtureT0Us, 8, replay::capture::kMarkLaunchRelease, 0),
    Mark(kFixtureT0Us, 9, replay::capture::kMarkPauseBegin, 0),
    Mark(kFixtureT0Us, 10, replay::capture::kMarkPauseEnd, 0),
    Mark(kFixtureT0Us, 11, replay::capture::kMarkMatchEnd, 0),
};

const char kTwelveLabels[] = "MB|RB0|LR|Gx3|RE0|RB0|LR|PB|PE|ME";

// Opens a capture whose manifest carries exactly these markers, with the panel up and the browser
// closed, and draws one frame.
std::wstring OpenWithMarkers(Fixture& fx, const RoundMarker* marks, uint16_t count) {
    g_markerOverride = marks;
    g_markerOverrideCount = count;
    const std::wstring cap = fx.captureDir + L"\\kinds.echodemo";
    REQUIRE(WriteCapture(cap.c_str(), 0, 0, 1));
    g_markerOverride = nullptr;
    g_markerOverrideCount = 0;
    PressKey();
    fx.Frame();
    REQUIRE(ui::LoadPath(cap.c_str()));
    uifake::Reset();
    fx.Frame();
    return cap;
}

}  // namespace

TEST_CASE("BuildRoundStrip names the kind, carries a round number only where there is one, and"
          " collapses runs") {
    ui::RoundEntry entries[12];
    size_t clamped = 99;
    ui::Marker markers[12];
    for (size_t i = 0; i < 12; ++i) {
        markers[i].tUs = static_cast<int64_t>(kTwelve[i].tUs - kFixtureT0Us);
        markers[i].index = kTwelve[i].index;
        markers[i].kind = kTwelve[i].kind;
    }
    const size_t n = ui::BuildRoundStrip(markers, 12, entries, 12, &clamped);
    CHECK(n == 10);
    CHECK(clamped == 0);
    std::string joined;
    for (size_t i = 0; i < n; ++i) {
        if (i > 0) joined += "|";
        joined += entries[i].label;
    }
    CHECK(joined == kTwelveLabels);
    // The button seeks to the FIRST marker of its run, not to the run's position in the strip:
    // entry 3 is the three goals, which start at marker 3.
    CHECK(entries[3].sourceIndex == 3);
    CHECK(entries[3].runLength == 3);
    CHECK(entries[4].sourceIndex == 6);

    // The control on the table: every kind the format defines has its own glyph, and no two
    // share one. Without this, a table that returned the unknown glyph for everything would
    // still pass a sequence check written against it.
    std::vector<std::string> glyphs;
    for (uint16_t k = replay::capture::kMarkMatchBegin; k <= replay::capture::kMarkPhase; ++k) {
        const std::string g = ui::RoundGlyph(k);
        CHECK(g != "?");
        for (const std::string& seen : glyphs) CHECK(g != seen);
        glyphs.push_back(g);
    }
    CHECK(glyphs.size() == 16u);
    // A kind the file could carry and this build does not know says so, rather than borrowing
    // its neighbour's glyph. 0 is what a manifest field nobody wrote reads as.
    CHECK(std::string(ui::RoundGlyph(0)) == "?");
    CHECK(std::string(ui::RoundGlyph(replay::capture::kMarkPhase + 1)) == "?");
}

TEST_CASE("twelve markers of six kinds draw ten buttons, and the label sequence is exact") {
    Fixture fx;
    ui::inframe::ResetForTest();
    OpenWithMarkers(fx, kTwelve, 12);

    REQUIRE(ui::RoundsKnown());
    CHECK(ui::RoundCount() == 12u);
    CHECK(StripLabels() == kTwelveLabels);
    // The defect this replaces, stated so it cannot come back silently: not one button says R0.
    CHECK_FALSE(uifake::ButtonDrawn("R0##rj"));
    // Ten dedup'd entries fit under the clamp, so there is no tail.
    CHECK_FALSE(uifake::TextContains("...+"));

    // ** THE TRANSPORT PAGE'S STREAM, AS NUMBERS. Before this packet all three NoteGuiStream
    // calls were inside DrawBrowserView, so the page the owner plays on had no measured budget
    // at all and "the transport draw fits" was a claim nothing could fail. Exact, not <=: a
    // bound survives the dedup being deleted, and the count is the only thing that catches it.
    const ui::inframe::Counters& ic = ui::inframe::Read();
    CHECK(ic.transportStreamFrames == 2u);
    CHECK(ic.transportItemsPerFrame == kTransportItems12);
    CHECK(ic.transportGlyphsPerFrame == kTransportGlyphs12);
    // The strip is a real share of the page, not a rounding error on it: without this the two
    // numbers above could be pinned to a page the strip barely touches.
    CHECK(ic.transportItemsPerFrame > 0u);
}

TEST_CASE("forty markers clamp to twelve buttons and a tail that states the rest") {
    Fixture fx;
    ui::inframe::ResetForTest();
    // Forty markers, no two neighbours alike, so the dedup collapses nothing and the clamp is
    // the only thing between them and forty buttons.
    RoundMarker marks[40];
    for (uint32_t i = 0; i < 40; ++i) {
        const uint16_t kind = static_cast<uint16_t>(replay::capture::kMarkMatchBegin +
                                                    (i % replay::capture::kMarkPhase));
        marks[i] = Mark(kFixtureT0Us, i, kind, 0);
    }
    OpenWithMarkers(fx, marks, 40);
    REQUIRE(ui::RoundCount() == 40u);

    ui::RoundEntry entries[12];
    size_t clamped = 0;
    ui::Marker markers[40];
    for (size_t i = 0; i < 40; ++i) {
        markers[i].tUs = static_cast<int64_t>(marks[i].tUs - kFixtureT0Us);
        markers[i].index = marks[i].index;
        markers[i].kind = marks[i].kind;
    }
    // The tail's number is derived from the DEDUP'D count, not from 40: a capture whose forty
    // markers collapsed to fifteen has three more, not twenty-eight.
    const size_t shown = ui::BuildRoundStrip(markers, 40, entries, 12, &clamped);
    CHECK(shown == 12u);
    CHECK(clamped == 28u);

    size_t drawn = 0;
    for (const std::string& label : uifake::ButtonLabels()) {
        if (label.find("##rj") != std::string::npos) ++drawn;
    }
    CHECK(drawn == 12u);
    CHECK(uifake::TextContains("...+28"));

    // The stated bound for the whole strip, re-derived here from the code rather than quoted
    // from the design: one "rounds:" text, at most twelve buttons and at most one tail.
    CHECK(1u + 12u + 1u == ui::panel::kRoundStripItemBudget);
    // Widest label: a two-character glyph, a five-digit uint16 round number on the two kinds
    // that carry one, and "x" plus a run of at most the 64 markers the panel holds.
    CHECK(7u + 12u * 10u + 6u == ui::panel::kRoundStripGlyphBudget);
}

// The number the browser row prints under "rounds" is the round-TABLE row count, never the S_MARK
// edge count. The fixture keeps the two APART -- fourteen edges, one row, F-REAL-3's own numbers --
// because with them equal this case could not fail whichever field the panel read. Row 1 is the
// same folder's ordinary capture, written with no ROUNDS block at all: that file cannot answer, and
// a confident "0 rounds" there would be the shrug this project keeps finding.
TEST_CASE("the browser row's rounds word counts round-table rows, not S_MARK edges") {
    Fixture fx;
    ::CreateDirectoryW(fx.dir.File(L"echoreplay").c_str(), nullptr);

    RoundMarker marks[14];
    for (uint16_t i = 0; i < 14; ++i) {
        marks[i] = RoundMarker{};
        marks[i].tUs = kFixtureT0Us + uint64_t(i + 1) * 100000ull;
        marks[i].kind = replay::capture::kMarkGoal;
        marks[i].index = 0;
    }
    replay::RoundRow onlyRow{};
    onlyRow.number = 1;
    onlyRow.beginUs = kFixtureT0Us;
    onlyRow.endUs = kFixtureT0Us + 2000000ull;
    onlyRow.beginEngineUs = onlyRow.beginUs;
    onlyRow.endEngineUs = onlyRow.endUs;

    const std::wstring plain = fx.captureDir + L"\\plain.echodemo";
    REQUIRE(WriteCapture(plain.c_str(), 0x4321, Whole(0x8765), 2));
    REQUIRE(SetMtime(plain.c_str(), FixtureMtime(0)));

    g_markerOverride = marks;
    g_markerOverrideCount = 14;
    g_roundRowOverride = &onlyRow;
    g_roundRowOverrideCount = 1;
    const std::wstring cap = fx.captureDir + L"\\table.echodemo";
    const bool wrote = WriteCapture(cap.c_str(), 0x4321, Whole(0x8765), 1);
    g_markerOverride = nullptr;
    g_markerOverrideCount = 0;
    g_roundRowOverride = nullptr;
    g_roundRowOverrideCount = 0;
    REQUIRE(wrote);
    REQUIRE(SetMtime(cap.c_str(), FixtureMtime(99)));

    fx.session.canChange = true;
    ui::OpenBrowser();
    fx.Frame();
    REQUIRE(ui::panel::WaitForBrowserForTest(10000));
    uifake::Reset();
    fx.Frame();

    std::string rows[2];
    for (const std::string& l : uifake::ButtonLabels()) {
        if (l.find("##row0") != std::string::npos) rows[0] = l;
        if (l.find("##row1") != std::string::npos) rows[1] = l;
    }
    REQUIRE_FALSE(rows[0].empty());
    REQUIRE_FALSE(rows[1].empty());

    // The token immediately before the word, not a substring search: "14 rounds" contains no
    // "1 rounds", but a panel printing "114 rounds" would still be wrong.
    auto roundsToken = [](const std::string& label) {
        const size_t w = label.find(" rounds");
        REQUIRE(w != std::string::npos);
        const size_t sp = label.rfind(' ', w - 1);
        REQUIRE(sp != std::string::npos);
        return label.substr(sp + 1, w - sp - 1);
    };

    // Row 0 carries fourteen markers and exactly one round-table row.
    CHECK(roundsToken(rows[0]) == "1");
    // ...and the marker count is not smuggled in under the rounds word by another spelling.
    CHECK(rows[0].find("14 rounds") == std::string::npos);
    // Row 1's manifest parsed -- so this is not the not-loaded arm -- but it has no ROUNDS block.
    CHECK(rows[1].find("1 player") != std::string::npos);
    CHECK(roundsToken(rows[1]) == "?");
    CHECK(rows[1].find("0 rounds") == std::string::npos);
    CHECK(rows[1].find("2 rounds") == std::string::npos);
}

// ---- process exit (DIAG-EXIT-FASTFAIL) ---------------------------------------------------------

TEST_CASE("panel: the pre-exit hook stops the browser worker inside its budget, and twice is once") {
    Fixture fx;
    REQUIRE(ui::panel::BrowserWorkerRunningForTest());
    ui::OpenBrowser();
    fx.Frame();
    REQUIRE(ui::panel::WaitForBrowserForTest(10000));

    ui::panel::StopWorkerForExit(5000);
    CHECK_FALSE(ui::panel::BrowserWorkerRunningForTest());
    ui::panel::StopWorkerForExit(5000);
    CHECK_FALSE(ui::panel::BrowserWorkerRunningForTest());

    fx.Frame();   // a frame after the stop still draws; the Fixture's Shutdown then has nothing to join
}

TEST_CASE("panel: nothing in panel.cpp can terminate() at static destruction") {
    // The 0xc0000409 at exit was ~thread() on a joinable std::thread member of a global. The worker
    // is a raw handle in storage that is never destroyed; this pins both halves in the source, where
    // the hazard lives -- no launch is needed to reintroduce it.
    std::ifstream in(std::string(REPLAY_SOURCE_DIR) + "/src/ui/panel.cpp", std::ios::binary);
    REQUIRE(in.good());
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string src = buf.str();
    REQUIRE(src.size() > 10000);
    for (const char* banned : {"std::thread", "std::jthread", "<thread>", "std::future", "std::promise",
                               "std::async"}) {
        CAPTURE(banned);
        CHECK(src.find(banned) == std::string::npos);
    }
    CHECK(src.find("BrowserWorker& g_worker = *new (g_workerStorage) BrowserWorker;") !=
          std::string::npos);
}

// ---- UI-4: the whole listing is reachable, newest first ---------------------------------------

TEST_CASE("the browser lists every capture, newest first, and reads only the page it draws") {
    // ★ LAUNCH-UI3 §9: 152 captures, 24 offered, and the one he was watching was the 25th. The
    // worker now builds the page the pager asks for, so the listing is whole and only the rows on
    // screen are opened. Prev/Next themselves are driven at launch: this binary's fake facade does
    // not deliver a click to the pager buttons.
    Fixture fx;
    WriteFolder(fx, 60);
    fx.session.canChange = true;
    ui::panel::SetPlayableOnlyForTest(false);

    ui::OpenBrowser();
    fx.Frame();
    REQUIRE(ui::panel::WaitForBrowserForTest(10000));
    fx.Frame();
    REQUIRE(fx.index.Count() == 60);
    CHECK(uifake::ButtonDrawn("f59"));                     // newest first
    CHECK(uifake::TextContains("of 60"));                  // the LISTING, not the cut
    CHECK_FALSE(uifake::TextContains("more not shown"));
    CHECK(RowsDrawn() == ui::panel::kBrowserRowsPerPage);
    // Only the page is opened: 60 captures cost one enumeration and a page of header reads.
    CHECK(fx.index.RowRead(ui::panel::kBrowserRowsPerPage - 1));
    CHECK_FALSE(fx.index.RowRead(ui::panel::kBrowserRowsPerPage));
}

// ---- MULTI-LOAD: the playable-only filter ------------------------------------------------------
//
// 154 captures in the owner's folder, and most of them are solo and lobby sessions whose manifest
// records nobody: they load to an empty rail and they were seven pages of the pager. The filter
// hides them, COUNTS them in the sentence, and is one click to turn off -- the tree's rule is that
// a capture must never silently vanish from the browser (demoindex.h:66-69).

TEST_CASE("MULTI-LOAD: the browser hides the captures with no players and says how many") {
    Fixture fx;
    ::CreateDirectoryW(fx.dir.File(L"echoreplay").c_str(), nullptr);
    // Three playable captures and five empty ones, with the empty ones NEWEST so a filter that did
    // nothing would leave them on page 0 where the assertions below would see them.
    for (int i = 0; i < 3; ++i) {
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\p%02d.echodemo", fx.captureDir.c_str(), i);
        g_playerCountOverride = 3;
        REQUIRE(WriteCapture(path, 0x576ED3F8428EBC4Bull, Whole(0x8765), 1));
        REQUIRE(SetMtime(path, FixtureMtime(static_cast<uint64_t>(i))));
    }
    for (int i = 0; i < 5; ++i) {
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\e%02d.echodemo", fx.captureDir.c_str(), i);
        g_playerCountOverride = 0;
        REQUIRE(WriteCapture(path, 0x576ED3F8428EBC4Bull, Whole(0x8765), 1));
        REQUIRE(SetMtime(path, FixtureMtime(static_cast<uint64_t>(10 + i))));
    }
    g_playerCountOverride = 1;
    fx.session.canChange = true;

    const uint64_t drawIoBefore = ui::fileio::OnDrawThread();

    ui::panel::SetPlayableOnlyForTest(true);
    ui::OpenBrowser();
    fx.Frame();
    REQUIRE(ui::panel::WaitForBrowserForTest(10000));
    fx.Frame();

    REQUIRE(fx.index.Count() == 8);
    // Three rows drawn, five hidden, and the sentence carries both numbers.
    CHECK(RowsDrawn() == 3);
    CHECK(uifake::TextContains("of 3 playable"));
    CHECK(uifake::TextContains(", 5 hidden"));
    // ★ AND IT FITS. The browser is drawn at one content width, and this sentence used to run past
    // it and be cut mid-word on every default-width load (LAUNCH-MULTILOAD §3.1). The cut is what
    // the panel itself applies, so asserting on the line it kept is asserting on what is drawn.
    {
        const std::string header = ui::panel::HeaderLineForTest();
        CHECK(header.size() <= ui::panel::kRowCharBudget);
        CHECK(header.find("...") == std::string::npos);
        CHECK(header.find(", 5 hidden") != std::string::npos);
    }
    // The header still counts the FOLDER: the filter hides rows, it does not shrink the folder.
    CHECK(uifake::TextContains("8 capture(s)"));
    CHECK(uifake::ButtonDrawn("p02"));         // newest playable is row 0
    CHECK_FALSE(uifake::ButtonDrawn("e04"));   // newest of all, and hidden

    // ★ AND NOT ONE FILE OPENED ON THE DRAW THREAD. Classifying the whole folder is a manifest
    // read per file; every one of them happens on the browser worker, and this counter is the only
    // thing that can fail when that stops being true.
    CHECK(ui::fileio::OnDrawThread() == drawIoBefore);

    // The row says WHO and WHERE, not just how long: the level column is the one thing a row could
    // not say before, and it is what decides whether opening the capture re-enters the session.
    std::string playableRow;
    for (const std::string& label : uifake::ButtonLabels()) {
        if (label.find("p02") != std::string::npos) playableRow = label;
    }
    REQUIRE_FALSE(playableRow.empty());
    CHECK(playableRow.find("3 players") != std::string::npos);
    CHECK(playableRow.find("arena_a") != std::string::npos);

    // ---- the toggle shows them again, and nothing was deleted ----
    ui::panel::SetPlayableOnlyForTest(false);
    ui::OpenBrowser();   // re-opening asks the worker for a fresh snapshot
    fx.Frame();
    REQUIRE(ui::panel::WaitForBrowserForTest(10000));
    fx.Frame();
    // The unfiltered listing is eight and the page holds seven of them: the pager is back.
    CHECK(RowsDrawn() == ui::panel::kBrowserRowsPerPage);
    CHECK(uifake::TextContains("of 8"));
    CHECK_FALSE(uifake::TextContains("hidden"));
    CHECK(uifake::ButtonDrawn("e04"));
    CHECK(ui::fileio::OnDrawThread() == drawIoBefore);
}

TEST_CASE("MULTI-LOAD: a level hash the build knows is named, and one it does not is not invented") {
    char out[32];
    // The one hash the tree already pins against the real echovr.exe
    // (tests/session/test_session.cpp:387, `IdOfName(pe, \"mpl_arena_a\")`).
    ui::FormatLevel(true, 0x576ED3F8428EBC4Bull, out, sizeof out);
    CHECK(std::string(out) == "arena_a");
    CHECK(std::string(ui::LevelName(0x576ED3F8428EBC4Bull)) == "mpl_arena_a");
    // A level this build has never heard of prints its hash, not a guess: the row is what the
    // owner picks a capture by, and a wrong name there is worse than no name.
    ui::FormatLevel(true, 0x1122334455667788ull, out, sizeof out);
    CHECK(std::string(out) == "55667788");
    CHECK(ui::LevelName(0x1122334455667788ull) == nullptr);
    // A row whose manifest has not been read says nothing at all, the same dash the player and
    // round counts use for "not loaded yet".
    ui::FormatLevel(false, 0x576ED3F8428EBC4Bull, out, sizeof out);
    CHECK(std::string(out) == "-");
    // Every name in the table is distinct and none of them is empty.
    size_t n = 0;
    const ui::LevelNameRow* rows = ui::LevelNameTable(&n);
    CHECK(n >= 10);
    for (size_t i = 0; i < n; ++i) {
        REQUIRE(rows[i].name != nullptr);
        CHECK(rows[i].name[0] != '\0');
        CHECK(strlen(rows[i].name) - 4 <= ui::panel::kRowLevelChars);
        for (size_t j = i + 1; j < n; ++j) {
            CHECK(rows[i].id != rows[j].id);
            CHECK(strcmp(rows[i].name, rows[j].name) != 0);
        }
    }
}

// =================================================================================================
// FORMATS-P6 -- a legacy replay through the panel's own funnel: converted on a worker, then loaded
// through LoadPath like any capture. The converters are fakes; the real one is P2-P5's.
// =================================================================================================
namespace {

std::atomic<uint32_t> g_p6Calls{0};
std::atomic<DWORD> g_p6Thread{0};
std::atomic<bool> g_p6Release{false};
std::atomic<bool> g_p6SawCancel{false};

import::Result P6Result(bool ok, import::ResultCode code, const char* message) {
    import::Result r{};
    r.ok = ok;
    r.code = code;
    snprintf(r.message, sizeof r.message, "%s", message);
    return r;
}

// Holds at 50% until released or cancelled, then writes a capture in the session's own level.
import::Result P6ConvertHeld(const wchar_t*, const wchar_t* out, const import::ConvertOptions&,
                             import::ConvertReport&, import::Progress* progress) {
    g_p6Calls.fetch_add(1);
    g_p6Thread.store(::GetCurrentThreadId());
    progress->permille.store(500);
    for (int i = 0; i < 10000 && !g_p6Release.load(); ++i) {
        if (progress->cancel.load()) {
            g_p6SawCancel.store(true);
            return P6Result(false, import::ResultCode::Cancelled, "cancelled");
        }
        ::Sleep(1);
    }
    if (!WriteCapture(out, 0x1111, Whole(0x2222), 2)) {
        return P6Result(false, import::ResultCode::Io, "fixture writer failed");
    }
    return P6Result(true, import::ResultCode::Ok, "");
}

import::Result P6ConvertRefuses(const wchar_t*, const wchar_t*, const import::ConvertOptions&,
                                import::ConvertReport&, import::Progress*) {
    g_p6Calls.fetch_add(1);
    return P6Result(false, import::ResultCode::UnsupportedMap,
                    "the replay is in mpl_combat_dyson; only mpl_arena_a converts");
}

void WriteText(const std::wstring& path, const char* text) {
    std::ofstream(path, std::ios::binary) << text;
}

struct P6Fixture : Fixture {
    std::wstring source = captureDir + L"\\rec_2025-08-03_17-15-57.echoreplay";

    P6Fixture() {
        replay::livesafety::SetForTest(/*liveOnlineExempt=*/false, /*offlineLockHeld=*/true);
        session.levelId = 0x1111;
        session.gametypeId = Whole(0x2222);
        g_p6Calls.store(0);
        g_p6Thread.store(0);
        g_p6Release.store(false);
        g_p6SawCancel.store(false);
        ui::panel::SetConverterForTest(&P6ConvertHeld);
        WriteText(source, "PK not really a zip");
    }
    ~P6Fixture() {
        g_p6Release.store(true);
        ui::panel::SetConverterForTest(nullptr);
    }

    // Ticks until the worker has reported and the tick has collected it.
    bool FramesUntilIdle() {
        for (int i = 0; i < 5000; ++i) {
            Frame();
            if (!ui::panel::ImportBusyForTest()) return true;
            ::Sleep(1);
        }
        return false;
    }

    bool WaitForConverter() {
        for (int i = 0; i < 5000 && g_p6Calls.load() == 0; ++i) ::Sleep(1);
        ::Sleep(5);
        return g_p6Calls.load() == 1;
    }
};

bool StatusHas(const char* needle) {
    INFO("status: " << ui::panel::StatusForTest());
    return strstr(ui::panel::StatusForTest(), needle) != nullptr;
}

}  // namespace

TEST_CASE("P6: a legacy replay converts off the tick, shows progress, then loads through LoadPath") {
    P6Fixture fx;
    CHECK_FALSE(ui::LoadPath(fx.source.c_str()));   // nothing is loaded yet...
    CHECK(ui::ImportInProgress());                   // ...but the request was taken
    CHECK(fx.transport.loadCalls == 0);
    CHECK(ui::BrowserOpen());                        // the window the progress is drawn in
    REQUIRE(fx.WaitForConverter());
    CHECK(g_p6Thread.load() != ::GetCurrentThreadId());

    fx.Frame();
    CHECK(StatusHas("converting rec_2025-08-03_17-15-57.echoreplay: 50%"));
    CHECK(uifake::ButtonDrawn("Cancel conversion"));
    CHECK(fx.transport.loadCalls == 0);

    g_p6Release.store(true);
    REQUIRE(fx.FramesUntilIdle());
    CHECK_FALSE(ui::ImportInProgress());
    REQUIRE(fx.transport.loadCalls == 1);
    const std::wstring loaded = fx.transport.lastPath;
    CHECK(loaded.find(fx.captureDir + L"\\imported\\rec_2025-08-03_17-15-57.") == 0);
    REQUIRE(loaded.size() > 9);
    CHECK(loaded.compare(loaded.size() - 9, 9, L".echodemo") == 0);
    // The owner picked the .echoreplay, so that is the name on screen, not the cache file's.
    CHECK(StatusHas("loaded rec_2025-08-03_17-15-57.echoreplay"));
    CHECK(ui::RoundCount() == 2);

    // Opened again: the cache is hit, the converter is not called, and it loads.
    CHECK_FALSE(ui::LoadPath(fx.source.c_str()));
    REQUIRE(fx.FramesUntilIdle());
    CHECK(g_p6Calls.load() == 1);
    CHECK(fx.transport.loadCalls == 2);
    CHECK(fx.transport.lastPath == loaded);
}

TEST_CASE("P6: the browser lists legacy sources from import.dirs with the LEGACY badge") {
    P6Fixture fx;
    TempDir spark;
    WriteText(spark.File(L"spark_match.echoreplay"), "PK");
    const std::wstring sparkDir(spark.Path());
    fx.config.importDirs.assign(sparkDir.begin(), sparkDir.end());   // ASCII temp path
    fx.config.importDirsSet = true;
    ui::panel::Init(&fx.services, &fx.index);
    ui::panel::SetPlayableOnlyForTest(true);   // an unconverted row has no manifest, so it stays

    ui::OpenBrowser();
    fx.Frame();
    REQUIRE(ui::panel::WaitForBrowserForTest(10000));
    for (int i = 0; i < 4; ++i) fx.Frame();
    CHECK(uifake::ButtonDrawn("spark_match.echoreplay"));
    CHECK(uifake::ButtonDrawn("rec_2025-08-03_17-15-57.echoreplay"));
    // Name, size and time, and the badge, for the row that has not been converted yet.
    std::string sparkRow;
    for (const std::string& label : uifake::ButtonLabels()) {
        if (label.find("spark_match") != std::string::npos) sparkRow = label;
    }
    CHECK(sparkRow.find("LEGACY - converts on open") != std::string::npos);
    CHECK(sparkRow.find(" 0 KB") != std::string::npos);
    CHECK(sparkRow.find("rounds") == std::string::npos);

    // A click converts, exactly as a typed load does.
    ui::panel::Sample(16666);
    uifake::Reset();
    uifake::ClickOnce("spark_match");
    ui::panel::Draw();
    fx.Frame();
    CHECK(ui::ImportInProgress());
    g_p6Release.store(true);
    REQUIRE(fx.FramesUntilIdle());
    CHECK(fx.transport.loadCalls == 1);
    CHECK(fx.transport.lastPath.find(L"\\imported\\spark_match.") != std::wstring::npos);
}

TEST_CASE("P6: Unload during a conversion cancels it and loads nothing") {
    P6Fixture fx;
    CHECK_FALSE(ui::LoadPath(fx.source.c_str()));
    REQUIRE(fx.WaitForConverter());
    ui::Unload();
    CHECK_FALSE(ui::ImportInProgress());
    REQUIRE(fx.FramesUntilIdle());
    CHECK(g_p6SawCancel.load());
    CHECK(fx.transport.loadCalls == 0);
    CHECK(StatusHas("was cancelled; nothing was kept"));
}

TEST_CASE("P6: the Cancel button stops the conversion") {
    P6Fixture fx;
    CHECK_FALSE(ui::LoadPath(fx.source.c_str()));
    REQUIRE(fx.WaitForConverter());
    fx.Frame();
    ui::panel::Sample(16666);
    uifake::Reset();
    uifake::ClickOnce("Cancel conversion");
    ui::panel::Draw();
    REQUIRE(fx.FramesUntilIdle());
    CHECK(g_p6SawCancel.load());
    CHECK(fx.transport.loadCalls == 0);
}

TEST_CASE("P6: a second legacy load while one converts is refused; a capture load supersedes it") {
    P6Fixture fx;
    const std::wstring other = fx.captureDir + L"\\other.tape";
    WriteText(other, "tape");
    CHECK_FALSE(ui::LoadPath(fx.source.c_str()));
    REQUIRE(fx.WaitForConverter());

    CHECK_FALSE(ui::LoadPath(other.c_str()));
    CHECK(StatusHas("is still converting"));
    CHECK(g_p6Calls.load() == 1);

    const std::wstring native = fx.dir.File(L"native.echodemo");
    REQUIRE(WriteCapture(native.c_str(), 0x1111, Whole(0x2222), 1));
    CHECK(ui::LoadPath(native.c_str()));
    CHECK_FALSE(ui::ImportInProgress());
    REQUIRE(fx.FramesUntilIdle());
    CHECK(g_p6SawCancel.load());
    CHECK(fx.transport.loadCalls == 1);
    CHECK(fx.transport.lastPath == native);
}

TEST_CASE("P6: a converter refusal is shown in the window, and nothing is loaded") {
    P6Fixture fx;
    ui::panel::SetConverterForTest(&P6ConvertRefuses);
    CHECK_FALSE(ui::LoadPath(fx.source.c_str()));
    REQUIRE(fx.FramesUntilIdle());
    CHECK(fx.transport.loadCalls == 0);
    CHECK(StatusHas("could not convert rec_2025-08-03_17-15-57.echoreplay: the replay is in "
                    "mpl_combat_dyson; only mpl_arena_a converts"));
    CHECK_FALSE(ui::ImportInProgress());
}

TEST_CASE("P6: in a live session nothing is converted at all") {
    P6Fixture fx;
    replay::livesafety::SetForTest(/*liveOnlineExempt=*/true, /*offlineLockHeld=*/false);
    CHECK_FALSE(ui::LoadPath(fx.source.c_str()));
    CHECK_FALSE(ui::ImportInProgress());
    CHECK_FALSE(ui::panel::ImportBusyForTest());
    ::Sleep(20);
    CHECK(g_p6Calls.load() == 0);
    CHECK(StatusHas("load refused"));
    replay::livesafety::SetForTest(/*liveOnlineExempt=*/false, /*offlineLockHeld=*/true);
}

TEST_CASE("P6: a converter refusal reaches the status line as its sentence") {
    P6Fixture fx;
    ui::panel::SetConverterForTest(nullptr);
    CHECK_FALSE(ui::LoadPath(fx.source.c_str()));
    REQUIRE(fx.FramesUntilIdle());
    CHECK(fx.transport.loadCalls == 0);
    CHECK(StatusHas("does not link the legacy-replay converter"));
}
