// The wiring check for W2-A: not whether the codec works, but whether anything ever registers.
//
// D13 / DIAG-WIRING: src/capture/capture.cpp -- the module translation unit -- was linked into no
// test target. tests/capture links replay_capture, the static library that deliberately does NOT
// contain capture.cpp, so the two registrations the whole subsystem hangs off could be deleted with
// every suite still green:
//
//     capture.cpp:875  audio::voip::SetVoipSink(&OnVoipFrame, nullptr)
//     capture.cpp:893  s->tick->RegisterEarly(&OnTick, nullptr)
//     capture.cpp:918  g.svc->netVar = g.netvar          (first tick, NOT Init -- W2-B binds to this)
//
// This file links the real module TU through the real modreg and asks the only places a missing
// registration is visible: the fake tick provider, the fake VoIP link seam, and the services struct.
//
// The VoIP seam is a fake DEFINITION of audio::voip::SetVoipSink rather than the real
// src/audio/voip/voipcapture.cpp: the real one keeps its sink in an anonymous namespace with no
// reader, so linking it would make the installation unobservable. This target links no replay_voip
// for exactly that reason.
//
// Nothing here needs a game. Every engine-facing adapter refuses at its site guard on a host with no
// echovr.exe mapped, which is the point -- the module must reach them and publish anyway.

#include "doctest.h"
#include "foundation/demofile/demofile.h"

#include <windows.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio/audiopins.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ratio>
#include <string>
#include <thread>
#include <vector>

#include "audio/voip/voipcapture.h"
#include "bootstrap/buildguard.h"
#include "bootstrap/modreg.h"
#include "bootstrap/paths.h"
#include "capture/manifestbuild.h"
#include "engine/wiretap.h"
#include "capture/capture_types.h"
#include "capture/streamcodec.h"
#include "foundation/bitio/bitio.h"
#include "engine/oracletap.h"
#include "foundation/demofile/demofile.h"
#include "foundation/demofile/format.h"
#include "bootstrap/services.h"
#include "bootstrap/livesafety.h"
#include "capture/capturestop.h"
#include "capture/costmeter.h"
#include "capture/completeness.h"
#include "capture/gather.h"
#include "capture/ringset.h"
#include "capture/worldorigin.h"
#include "engine/session.h"
#include "engine/tick.h"
#include "generated/facts_generated.h"
// The three ORACLE.* rows the shipped decision reads, and the promotion V-I-W2I1 will merge.
#include "capture/oracleregistry.h"

// ---------------------------------------------------------------------------------------------
// The fake VoIP service. capture.cpp calls this symbol; the real definition lives in
// voipcapture.cpp, which this target does not link.
namespace replay {
namespace audio {
namespace voip {

VoipSink g_installedSink = nullptr;
void*    g_installedCtx = nullptr;
int      g_setCalls = 0;

void SetVoipSink(VoipSink sink, void* ctx) {
    g_installedSink = sink;
    g_installedCtx = ctx;
    ++g_setCalls;
}

}  // namespace voip

// KB_AUDIO_PINS's source, faked for the same reason the VoIP sink above is: the real
// EncodeCurrentAudioPins walks a live CSoundPlayer and returns 0 on a host with no game, so
// linking it would make both the call and its CADENCE unobservable. What this target asks is the
// question no test of the capture library can -- does the shipped tick ever call it, and how often.
size_t g_pinEncodeCalls = 0;
size_t g_pinEncodeBytes = 7;
PinSource g_pinEncodeSource = PinSource::Bound;

size_t EncodeCurrentAudioPins(void* dst, size_t cap, PinSource* source) {
    ++g_pinEncodeCalls;
    if (source != nullptr) {
        *source = g_pinEncodeSource;
    }
    if (dst == nullptr || cap < g_pinEncodeBytes) {
        return 0;
    }
    unsigned char* const p = static_cast<unsigned char*>(dst);
    for (size_t i = 0; i < g_pinEncodeBytes; ++i) {
        p[i] = 0xA5u;
    }
    return g_pinEncodeBytes;
}

}  // namespace audio

// The shipped oracletap defaults its clock to this symbol, which reads engine memory this process
// does not have; src/engine/tick.cpp is not linked here because its module registrar would publish a
// real ITick over the RecordingTick every case below drives. Every case installs a clock through
// SetOracleClockForTest, so this definition exists to LINK and must never be the one that answers --
// it refuses, and a tap that reached it would report NoClock rather than a silent zero bracket.
namespace tick {
bool ReadEngineClock(uint64_t*, uint64_t*, uint64_t*) { return false; }
}  // namespace tick

// Defined in src/capture/capture.cpp and deliberately in no header, so nothing outside this file can
// reach it by including something.
namespace capture {
void SetTapOverrideForTest(IWiretap* tap);
void SetOracleRegistryViewForTest(const OracleRegistryView* view);
const char* OracleDecideForTest(const OracleRegistryView* view, char* out, size_t cap, bool& bound);
void SetOracleClockForTest(replay::ClockFn fn);
void OracleCountsForTest(uint64_t& samples, uint64_t& ok);
const char* OracleShippedNotBoundForTest(char* out, size_t cap);
void OracleBindWhyForTest(char* out, size_t cap);
void ResetTapRegistrationForTest();
// AUDIT_M5 F3's control: the live (tick-owned) view, and a change to it that nobody publishes.
TapRegistration TapRegistrationLiveForTest();
void PerturbTapRegistrationWithoutPublishingForTest();
}  // namespace capture

}  // namespace replay

namespace {

// Wall-clock ratios swing with the machine and its load, so they only run when asked for.
bool TimingChecksEnabled() {
    const char* v = std::getenv("REPLAY_TIMING_TESTS");
    return v != nullptr && v[0] == '1';
}

struct Line {
    replay::LogLevel level;
    std::string module;
    std::string text;
};

class RecordingLog final : public replay::ILog {
public:
    void Write(replay::LogLevel level, const char* module, const char* fmt, ...) override {
        char text[2048];
        va_list args;
        va_start(args, fmt);
        _vsnprintf_s(text, sizeof text, _TRUNCATE, fmt, args);
        va_end(args);
        // The tick thread and the capture worker both write; the pre-exit case makes them do it at
        // the same moment on purpose.
        AcquireSRWLockExclusive(&writeLock_);
        lines.push_back(Line{level, module ? module : "", text});
        ReleaseSRWLockExclusive(&writeLock_);
    }
    size_t Size() {
        AcquireSRWLockExclusive(&writeLock_);
        const size_t n = lines.size();
        ReleaseSRWLockExclusive(&writeLock_);
        return n;
    }
    bool Enabled(replay::LogLevel) const override { return true; }
    void DumpLedger() override {}

    const Line* Find(const char* needle) const {
        for (const Line& l : lines)
            if (l.module == "capture" && l.text.find(needle) != std::string::npos) return &l;
        return nullptr;
    }

    const Line* FindLast(const char* needle) const {
        const Line* hit = nullptr;
        for (const Line& l : lines)
            if (l.module == "capture" && l.text.find(needle) != std::string::npos) hit = &l;
        return hit;
    }

    std::vector<Line> lines;

private:
    SRWLOCK writeLock_ = SRWLOCK_INIT;
};

// The performance counter, in raw ticks. Converting to microseconds first overflows a uint64 after a
// few weeks of uptime, the same reason src/engine/wiretap.cpp keeps its deadline in ticks.
uint64_t QpcFreq() {
    LARGE_INTEGER f{};
    return (QueryPerformanceFrequency(&f) && f.QuadPart > 0) ? uint64_t(f.QuadPart) : 1;
}
uint64_t QpcTicks() {
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return uint64_t(c.QuadPart);
}
uint64_t UsBetween(uint64_t from, uint64_t to) { return ((to - from) * 1000000ull) / QpcFreq(); }

// Spun, not slept: Sleep(2) on a default Windows timer resolution is ~15 ms, which would make the
// measurement below a measurement of the scheduler. The wiretap's own wait is a spin too.
void SpinUs(uint64_t us) {
    const uint64_t deadline = QpcTicks() + (QpcFreq() * us) / 1000000ull;
    while (QpcTicks() < deadline) {
        YieldProcessor();
    }
}

// The tap capture registers against instead of one built from the engine. `Listen` is the only thing
// simulated: a name in `refuseAlways` is always refused, `refuseOnce` is refused exactly once and
// granted after that, and `refuseEverything` refuses the whole table. Both refusals answer with
// W1-L's real diagnostic text, so the log assertions below are about the string the shipping wiretap
// actually produces -- and each one COSTS what the real one costs, W1-L's 2000 us deadline spun on
// the calling thread, or the per-tick bound asserted further down would be a bound on nothing.
const char kLockTimeoutText[] =
    "wiretap-lock-timeout: the CR15NetGame listener lock was still held after 2000 us "
    "(refused 1; retry on a later tick)";

// â˜… AUDIT_M3 F1. The OTHER kind of refusal, and the reason the fixture has to be able to produce one:
// only a spent lock deadline may end a tick's walk, and a fake whose every refusal moved LockTimeouts()
// cannot present the other side -- the auditor replaced the discriminator with `true` and all ten cases
// stayed green. The shipping wiretap refuses four ways without ever reaching its deadline: an unknown
// symbol (src/engine/wiretap.cpp:147), a call from inside a callback (:151), a full tap table (:156)
// and the pool refusal below (:192), which happens after the lock has been taken AND released.
const char kPoolExhaustedText[] = "Listen refused by the engine: listener pool exhausted";

class FakeWiretap final : public replay::IWiretap {
public:
    static constexpr uint64_t kDeadlineUs = 2000;   // src/engine/wiretap.cpp kLockTimeoutUs

    std::string refuseOnce = "SR15NetComponentEvent";
    std::string refuseAlways = "SR15NetSetOwner";
    // Refused with no spin and WITHOUT moving LockTimeouts(), checked before every other rule so a test
    // can single one type out of a table the tap is otherwise refusing wholesale.
    std::string refuseCheap;
    bool        refuseEverything = false;
    // Decided when the tap is built and never revisited, which is what makes it terminal.
    bool        available = true;

    bool        Available() const override { return available; }
    // What the last refusal was, rather than one fixed string: a fixture that named the lock timeout
    // before any refusal had happened would be the very defect AUDIT_M3 F6 raises against the shipping
    // wiretap, one refusal's text printed for a different refusal.
    const char* Diagnostic() const override { return diagnostic; }
    uint64_t    Symbol(const char*) const override { return 0x1234u; }

    uint16_t Listen(const char* structName, replay::WiretapFn, void*) override {
        const std::string name = structName ? structName : "";
        calls.push_back(name);
        if (!refuseCheap.empty() && name == refuseCheap) {
            diagnostic = kPoolExhaustedText;
            return kInvalidHandle;
        }
        if (refuseEverything || name == refuseAlways || (name == refuseOnce && refuseOnceLeft-- > 0)) {
            SpinUs(kDeadlineUs);
            ++timeouts;
            diagnostic = kLockTimeoutText;
            return kInvalidHandle;
        }
        granted.push_back(name);
        return next_++;
    }
    uint16_t ListenSymbol(uint64_t, replay::WiretapFn, void*) override { return next_++; }
    void     Unlisten(uint16_t) override {}
    void     UnlistenAll() override { ++unlistenAlls; }
    uint32_t unlistenAlls = 0;
    uint32_t ActiveCount() const override { return uint32_t(granted.size()); }
    uint32_t LockTimeouts() const override { return timeouts; }

    size_t Count(const std::string& name) const {
        size_t n = 0;
        for (const std::string& s : calls) n += (s == name);
        return n;
    }
    size_t Granted(const std::string& name) const {
        size_t n = 0;
        for (const std::string& s : granted) n += (s == name);
        return n;
    }
    // Grants after a mark. Granted() alone cannot answer "registered on a LATER tick": every type is
    // already in that history from the first pass, so a whole-history count passes without a retry.
    size_t GrantedSince(size_t mark, const std::string& name) const {
        size_t n = 0;
        for (size_t i = mark; i < granted.size(); ++i) n += (granted[i] == name);
        return n;
    }

    std::vector<std::string> calls;
    std::vector<std::string> granted;
    int         refuseOnceLeft = 1;
    uint32_t    timeouts = 0;
    const char* diagnostic = "no refusal has happened yet";

private:
    uint16_t next_ = 1;
};

// capture.dir points at a directory this test owns. Without it the module writes into the owner's
// Documents\EchoReplay, which a gate run must never touch.
class TestConfig final : public replay::IConfig {
public:
    std::string dir;

    bool GetBool(const char*, bool dflt) const override { return dflt; }
    int64_t GetInt(const char*, int64_t dflt) const override { return dflt; }
    double GetDouble(const char*, double dflt) const override { return dflt; }
    const char* GetString(const char* key, const char* dflt) const override {
        if (strcmp(key, "capture.dir") == 0 && !dir.empty()) return dir.c_str();
        return dflt;
    }
    bool Has(const char* key) const override { return strcmp(key, "capture.dir") == 0; }
};

// The tick provider, recording what was registered and on which phase. RegisterLate returning true
// as well is deliberate: a module that registered on the wrong phase must fail this test on the
// phase it chose, not be rescued by a refusal.
class RecordingTick final : public replay::ITick {
public:
    replay::TickFn earlyFn = nullptr;
    void*          earlyCtx = nullptr;
    int            earlyCalls = 0;
    int            lateCalls = 0;

    bool RegisterEarly(replay::TickFn fn, void* ctx) override {
        ++earlyCalls;
        earlyFn = fn;
        earlyCtx = ctx;
        return true;
    }
    bool RegisterLate(replay::TickFn, void*) override {
        ++lateCalls;
        return true;
    }
    const char* Mode() const override { return "none"; }
};

class FakeSession final : public replay::ISessionEntry {
public:
    replay::NetGameState state = replay::NetGameState::ReadyForGame;
    // The cost the engine charges for one State() read. Zero everywhere except the numerator case,
    // which needs an always-on tick phase that is expensively, measurably slow -- L2's `gate` read
    // 1793 us per tick and kept reading it with nothing recording.
    uint32_t busyUs = 0;

    bool CanChangeLevelMidSession() const override { return false; }
    bool Matches(uint64_t, uint64_t) const override { return false; }
    bool RequestEntry(uint64_t, uint64_t) override { return false; }
    replay::NetGameState State() const override {
        if (busyUs) {
            LARGE_INTEGER f{};
            LARGE_INTEGER a{};
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&a);
            const int64_t until = a.QuadPart + (f.QuadPart * busyUs) / 1000000;
            LARGE_INTEGER now = a;
            while (now.QuadPart < until) {
                QueryPerformanceCounter(&now);
            }
        }
        return state;
    }
    void* NetGame() const override { return nullptr; }
    const char* Strategy() const override { return "SE-A"; }
    const char* FailureReason() const override { return ""; }
};

// A CR15NetGame stand-in: zeroed storage the wiretap may read offsets out of. Every site guard on
// this host refuses before it dereferences anything derived from it.
struct FakeNetGame {
    alignas(16) uint8_t bytes[0x400] = {};
};

// A directory of this RUN's own, which the process moves into before doctest starts a case and takes
// with it when it goes.
//
// AUD-ARCH21-B B1a made the capture scratch per PROCESS; that is not enough, because the cases below
// write into the WORKING directory too -- stop sentinels, the per-close archive, the stop-request
// file, the M7 watch and state trees -- and then read them back to decide whether the module behaved.
// Anything one run leaves behind therefore answers the NEXT run's questions: a stop-request file that
// outlived an aborted case asks every capture to stop at tick one, and `closedFiles.size() >= 3` and
// the close-cause search go red in a build nobody touched. The name carries the same launch id and
// pid the stop sentinels carry, so two runs can never share one directory, whatever the OS does with
// process ids.
const wchar_t kRunDirPrefix[] = L"capture_wiretest.";

// An hour. A run takes a couple of minutes and touches its tree throughout, so nothing this old can
// belong to a live run -- and a run killed mid-case must not litter %TEMP% for good.
// DERIVED, never spelled. A FILETIME tick is a hundred nanoseconds -- std::nano scaled by
// std::hecto -- and an hour is std::chrono::hours. Every round decimal constant a test writes
// can collide with an engine offset in the fact registry, and both of the ones this line used
// to spell did; a per-line waiver then leaves the number to be re-checked by hand for ever.
// Taking the whole quantity out of <chrono> leaves nothing to waive.
using FileTimeTick = std::chrono::duration<uint64_t, std::ratio_multiply<std::nano, std::hecto>>;
constexpr uint64_t kRunDirKeepForTicks =
    std::chrono::duration_cast<FileTimeTick>(std::chrono::hours(1)).count();

uint64_t FileTimeU64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

void RemoveTreeW(const wchar_t* dir) {
    wchar_t pattern[MAX_PATH];
    _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\*", dir);
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t child[MAX_PATH];
            _snwprintf_s(child, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveTreeW(child);
            } else {
                ::DeleteFileW(child);
            }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    ::RemoveDirectoryW(dir);
}

void SweepOlderRuns(const wchar_t* temp) {
    wchar_t pattern[MAX_PATH];
    _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s%s*", temp, kRunDirPrefix);
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME now{};
    ::GetSystemTimeAsFileTime(&now);
    const uint64_t cutoff = FileTimeU64(now) - kRunDirKeepForTicks;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (FileTimeU64(fd.ftLastWriteTime) >= cutoff) continue;
        wchar_t victim[MAX_PATH];
        _snwprintf_s(victim, MAX_PATH, _TRUNCATE, L"%s%s", temp, fd.cFileName);
        RemoveTreeW(victim);
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
}

struct RunDirState {
    wchar_t home[MAX_PATH] = {};
    wchar_t path[MAX_PATH] = {};
    bool entered = false;
};

RunDirState& TheRunDir() {
    static RunDirState s;
    return s;
}

bool EnterRunDir() {
    RunDirState& s = TheRunDir();
    wchar_t temp[MAX_PATH] = {};
    if (!::GetCurrentDirectoryW(MAX_PATH, s.home)) return false;
    if (!::GetTempPathW(MAX_PATH, temp)) return false;
    SweepOlderRuns(temp);
    _snwprintf_s(s.path, MAX_PATH, _TRUNCATE, L"%s%s%016llX.%lu", temp, kRunDirPrefix,
                 static_cast<unsigned long long>(replay::capture::LaunchFileTime()),
                 static_cast<unsigned long>(::GetCurrentProcessId()));
    // No ERROR_ALREADY_EXISTS here: this name cannot collide with a live run, so a directory already
    // standing under it is exactly the leftover state this exists to rule out.
    if (!::CreateDirectoryW(s.path, nullptr)) return false;
    if (!::SetCurrentDirectoryW(s.path)) return false;
    s.entered = true;
    return true;
}

// Static, so the move happens before doctest constructs the first Env and the tree is gone once
// doctest's main returns.
struct RunDirGuard {
    RunDirGuard() { EnterRunDir(); }
    ~RunDirGuard() {
        RunDirState& s = TheRunDir();
        if (!s.entered) return;
        ::SetCurrentDirectoryW(s.home);
        RemoveTreeW(s.path);
    }
};
const RunDirGuard g_runDir;

// The captures themselves, inside this run's directory. The create is CHECKED and no longer tolerates
// an existing directory: the parent was created moments ago by this process, so anything already
// standing here is a fault, not a leftover to shrug at. A fixture that cannot say it failed to build
// is not a fixture.
std::string ScratchDir() {
    static std::string dir;
    if (!dir.empty()) {
        return dir;
    }
    const RunDirState& s = TheRunDir();
    REQUIRE_MESSAGE(s.entered, "this run's private working directory could not be entered");
    char root[MAX_PATH] = {};
    REQUIRE(::WideCharToMultiByte(CP_UTF8, 0, s.path, -1, root, MAX_PATH, nullptr, nullptr) != 0);
    dir = std::string(root) + "\\captures";
    if (!CreateDirectoryA(dir.c_str(), nullptr)) {
        dir.clear();
        REQUIRE_MESSAGE(false, "the scratch directory could not be created");
    }
    return dir;
}

// The ORACLE tap's stand-in producers. Two DIFFERENT payloads, because one shared blob would let a
// tap that wrote the session document into both fields pass.
namespace w2a_oracle {
const char kSessionDoc[] = "{\"sessionid\":\"w2a-oracle\",\"arm\":\"primary\"}";
const char kBonesDoc[] = "{\"bones\":23,\"players\":1}";
uint32_t g_sessionCalls = 0;
uint32_t g_bonesCalls = 0;
uint64_t g_clockUs = 4000000;

uint32_t SessionStub(void* ctx, uint8_t* dst, uint32_t cap) {
    const uint32_t n = uint32_t(sizeof(kSessionDoc) - 1);
    if (n > cap) return 0;
    memcpy(dst, kSessionDoc, n);
    ++*static_cast<uint32_t*>(ctx);
    return n;
}

uint32_t BonesStub(void* ctx, uint8_t* dst, uint32_t cap) {
    const uint32_t n = uint32_t(sizeof(kBonesDoc) - 1);
    if (n > cap) return 0;
    memcpy(dst, kBonesDoc, n);
    ++*static_cast<uint32_t*>(ctx);
    return n;
}

// Reads the same two numbers twice, which is what makes the tap's no-skew bracket ASSERTABLE rather
// than true by construction: a clock that moved between the two builder calls would come back Skewed,
// and oracletap plants exactly that case in its own suite.
bool ClockStub(uint64_t* simTimeUs, uint32_t* frameIndex) {
    *simTimeUs = g_clockUs;
    *frameIndex = uint32_t(g_clockUs / 11111ull);
    return true;
}

// THE REGISTRY AS V-I-W2I1 WILL PUBLISH IT. The shipped rows with a call shape promoted onto them --
// argMove is what Guarded<> checks at the entry point, and a row carrying none is refused -- plus the
// resolver that stands in for reading the loaded image, which this host has none of. Every row name,
// address and prologue still comes from oracleregistry.h: this promotes the rows, it does not invent
// them, so a row W2-A renames moves both sides together.
const uint8_t kPromotedArgMove[4] = {0x48, 0x89, 0x5C, 0x24};

replay::BuilderFn PromotedResolve(const replay::capture::OracleRegistryRow& r) {
    return r.role == replay::capture::OracleRole::Bones ? &BonesStub : &SessionStub;
}

replay::capture::OracleRegistryRow Promoted(size_t i) {
    replay::capture::OracleRegistryRow r = replay::capture::kOracleRegistry[i];
    r.argMove = kPromotedArgMove;
    r.argMoveBytes = uint32_t(sizeof kPromotedArgMove);
    r.argMoveOff = 0;
    return r;
}

replay::capture::OracleRegistryRow g_promotedRows[3] = {Promoted(0), Promoted(1), Promoted(2)};
const replay::capture::OracleRegistryView g_promotedView{g_promotedRows, 3, &PromotedResolve,
                                                         &g_sessionCalls, &g_bonesCalls};
}  // namespace w2a_oracle

// modreg::InitAll is once per process, so the file shares one environment and the cases below run in
// declaration order: the netvar case asserts the state BEFORE any tick, so it must be first.
struct Env {
    RecordingLog  log;
    TestConfig    config;
    RecordingTick tick;
    FakeSession   session;
    FakeNetGame   netGame;
    FakeWiretap   wiretap;
    replay::ReplayServices services{};
    size_t failures = 0;

    Env() {
        // RECORDER-2 part C: the recorder now refuses any session that is not a live online match,
        // and a capture wiretest IS one. Stated here rather than left to the process default so
        // the gate is exercised by its own tests and not by this one.
        replay::livesafety::SetForTest(/*liveOnlineExempt=*/true, /*offlineLockHeld=*/false);
        // Before any tick: capture builds its tap on the first tick carrying a CR15NetGame, and on a
        // host with no echovr.exe the real one refuses at Available() before a single Listen is tried.
        replay::capture::SetTapOverrideForTest(&wiretap);
        // S5.4.7's two producers, decided from the REGISTRY. On the shipped path every ORACLE.* row
        // refuses -- their call shapes are DL-22 / V-I-W2I1 and the registry carries no
        // argmove for them -- so Init does not call BindBuilders and every record carries NotBound.
        // Here the rows are PROMOTED, so the shipped decision resolves them and Init DOES bind:
        // removing that call is a mutant this file's last case reds. The clock goes the same way --
        // the shipped default reads engine memory this process does not have.
        replay::capture::SetOracleRegistryViewForTest(&w2a_oracle::g_promotedView);
        replay::capture::SetOracleClockForTest(&w2a_oracle::ClockStub);
        config.dir = ScratchDir();
        services.log = &log;
        services.config = &config;
        services.tick = &tick;
        services.session = &session;
        replay::modreg::InitAll(&services);
        failures = replay::modreg::InitFailures();
        // â˜… AFTER InitAll, not before. src/engine/session.cpp carries its own REPLAY_MODULE and
        // publishes the real ISessionEntry over whatever was in the struct -- whose State() is 0 on a
        // host with no engine, so capture would never see a match start. capture re-reads
        // services.session on every tick, so replacing it here is what the tick actually sees.
        services.session = &session;
    }

    void ModuleTick(void* ng) {
        replay::TickInfo info{};
        info.frameIndex = ++frame;
        info.simTimeUs = frame * 11111ull;
        info.scaledDeltaUs = 11111;
        info.frameDeltaUs = 11111;
        info.netGame = ng;
        replay::modreg::TickAll(info);
    }

    // The engine's own tick, delivered through whatever capture handed RegisterEarly.
    void EngineTick() {
        replay::TickInfo info{};
        info.frameIndex = ++frame;
        info.simTimeUs = frame * 11111ull;
        info.scaledDeltaUs = 11111;
        info.frameDeltaUs = 11111;
        info.netGame = netGame.bytes;
        REQUIRE(tick.earlyFn != nullptr);
        tick.earlyFn(info, tick.earlyCtx);
    }

    uint32_t frame = 0;
};

Env& TheEnv() {
    static Env env;
    return env;
}

// One whitespace-separated token, matched WHOLE. `find()` on "slot=1" also hits "slot=11", and that
// is the commonest false pass in this tree.
bool HasToken(const std::string& line, const char* token) {
    size_t at = 0;
    for (;;) {
        const size_t end = line.find(' ', at);
        const size_t n = (end == std::string::npos ? line.size() : end) - at;
        if (line.compare(at, n, token) == 0) return true;
        if (end == std::string::npos) return false;
        at = end + 1;
    }
}

bool Registered(const char* name) {
    for (size_t i = 0; i < replay::modreg::Count(); ++i)
        if (strcmp(replay::modreg::At(i)->name, name) == 0) return true;
    return false;
}

// The types capture actually calls Listen for: everything in the table except the netvar stream
// (INetVar registers that one) and our own snapshot type. Derived, never written down, so it follows
// the table growing.
uint16_t ReachableTapTypes() {
    uint16_t n = 0;
    for (uint16_t i = 0; i < replay::capture::TapTypeCount(); ++i) {
        const replay::capture::TapType& t = replay::capture::TapTable()[i];
        if (t.stream == replay::kStreamNetvar) continue;
        if (strcmp(t.structName, "ReplayIdentitySnapshot") == 0) continue;
        ++n;
    }
    return n;
}

// The first type the walk asks for, so a test can put a refusal exactly where a walk that stopped on
// it would register nothing at all.
const char* FirstReachableTapType() {
    for (uint16_t i = 0; i < replay::capture::TapTypeCount(); ++i) {
        const replay::capture::TapType& t = replay::capture::TapTable()[i];
        if (t.stream == replay::kStreamNetvar) continue;
        if (strcmp(t.structName, "ReplayIdentitySnapshot") == 0) continue;
        return t.structName;
    }
    return nullptr;
}

// The symbol INetVar registers, spelled by the table rather than by this file: the capture derives it
// the same way, and a copy here would keep passing after the table moved on.
const char* NetvarTapType() {
    for (uint16_t i = 0; i < replay::capture::TapTypeCount(); ++i) {
        const replay::capture::TapType& t = replay::capture::TapTable()[i];
        if (t.stream == replay::kStreamNetvar) return t.structName;
    }
    return nullptr;
}

size_t CountLinesSince(const RecordingLog& log, size_t mark, const char* needle) {
    size_t n = 0;
    for (size_t i = mark; i < log.lines.size(); ++i)
        if (log.lines[i].module == "capture" && log.lines[i].text.find(needle) != std::string::npos)
            ++n;
    return n;
}

int OrderOf(const char* name) {
    for (size_t i = 0; i < replay::modreg::Count(); ++i)
        if (strcmp(replay::modreg::At(i)->name, name) == 0) return replay::modreg::At(i)->order;
    return -1;
}

}  // namespace

// â˜… FIRST, and the order matters: this pins that the netvar surface is NOT published at Init. W2-B
// binds against a netVar that exists only from the first tick with a CR15NetGame, so a capture that
// published early -- or one that stopped publishing at all -- both have to be visible here.
TEST_CASE("the capture module registers and Init publishes NO netvar surface") {
    Env& e = TheEnv();
    REQUIRE(Registered("capture"));
    CHECK(OrderOf("capture") == 200);
    CHECK(e.failures == 0);
    CHECK(e.log.Find("armed; ring") != nullptr);

    CHECK(e.services.netVar == nullptr);

    // And nothing has been asked of the message bus yet, which is the state a reader must not confuse
    // with "asked and refused": the tap is built on the first tick carrying a CR15NetGame.
    const replay::capture::TapRegistration r = replay::capture::TapRegistrationNow();
    CHECK_FALSE(r.started);
    CHECK(r.registered == 0u);
    CHECK(r.deferred == r.total);
}

TEST_CASE("Init installs the VoIP sink on the voip service") {
    Env& e = TheEnv();
    (void)e;
    REQUIRE(replay::audio::voip::g_setCalls >= 1);
    REQUIRE(replay::audio::voip::g_installedSink != nullptr);
    CHECK(replay::audio::voip::g_installedCtx == nullptr);

    // And it is capture's own sink, not any function pointer: capture is not recording yet, and its
    // sink refuses a frame it has no time axis for rather than storing it at stamp zero.
    const uint8_t payload[16] = {};
    CHECK_FALSE(replay::audio::voip::g_installedSink(0x00010002u, 7, payload, sizeof payload,
                                                     replay::audio::voip::g_installedCtx));
}

TEST_CASE("Init registers the gather on the tick's EARLY phase, once, and not on late") {
    Env& e = TheEnv();
    CHECK(e.tick.earlyCalls == 1);
    CHECK(e.tick.lateCalls == 0);
    REQUIRE(e.tick.earlyFn != nullptr);
    CHECK(e.tick.earlyCtx == nullptr);
}

// â˜… AUDIT_M4 B1 (BLOCKER), and the control is what this case does NOT do: it never calls
// ResetTapRegistrationForTest. Every other registration case below opens with that helper, which
// seeds the pending count -- precisely the initialisation the shipped Init did not do -- so all of
// them measured a state the product never reached. This one drives the shipped post-Init state.
//
// The defect: `tapsPending` meant both "none pending" and "no walk has ever counted anything", and
// the settled-guard could not tell them apart. The netvar listener asks FIRST and spends the tick's
// one lock deadline, so five contended ticks settle it with the tap walk never once reached; from
// the next tick the guard read "nothing pending, netvar settled" and returned for the life of the
// process. Zero of the 36 tap types were ever asked for, the file still opened, and the last word in
// the log was a `tap-registration-deferred` line that had become false.
TEST_CASE("the shipped post-Init state still registers its taps after the netvar gives up") {
    Env& e = TheEnv();
    const uint16_t reachable = ReachableTapTypes();
    REQUIRE(reachable >= 10);

    // The first tick of the process, and nothing has been asked of anything yet.
    REQUIRE(e.services.netVar == nullptr);
    replay::capture::TapRegistration r = replay::capture::TapRegistrationNow();
    REQUIRE_FALSE(r.started);
    REQUIRE(r.registered == 0u);

    e.wiretap.refuseEverything = true;   // the CR15NetGame listener lock is held

    // Five contended ticks. The netvar listener spends each tick's one deadline, so the walk is not
    // reached at all and nothing counts anything: exactly one Listen per tick, and it is the netvar's.
    for (int i = 0; i < 5; ++i) {
        const size_t callsBefore = e.wiretap.calls.size();
        e.ModuleTick(e.netGame.bytes);
        CHECK(e.wiretap.calls.size() - callsBefore == 1u);
        CHECK(e.wiretap.calls.back() == std::string(NetvarTapType()));
    }
    r = replay::capture::TapRegistrationNow();
    CHECK(r.givenUp == 1u);   // the netvar listener, abandoned at the attempt bound
    CHECK(r.registered == 0u);
    CHECK(r.deferred == r.total - 1u);

    // â˜… THE TICK R2 GAVE UP ON. The netvar is settled and the taps have never been asked; the lock is
    // now free. The walk must run: under the R2 code this tick issued no Listen at all, and neither
    // did any tick after it.
    e.wiretap.refuseEverything = false;
    const size_t callsBefore = e.wiretap.calls.size();
    e.ModuleTick(e.netGame.bytes);
    CHECK(e.wiretap.calls.size() - callsBefore > 0u);
    CHECK(e.wiretap.granted.size() > 0u);

    // And the deferred tail drains: every reachable type registers except the one the fixture refuses
    // forever, which is given up on at the attempt bound.
    int ticks = 0;
    while (replay::capture::TapRegistrationNow().deferred > 0u) {
        e.ModuleTick(e.netGame.bytes);
        REQUIRE(++ticks < 60);
    }
    r = replay::capture::TapRegistrationNow();
    CHECK(r.started);
    CHECK(r.registered == unsigned(reachable) - 1u);
    CHECK(r.givenUp == 2u);        // the netvar listener and SR15NetSetOwner
    CHECK(r.unavailable == 0u);
    CHECK(r.registered + r.deferred + r.givenUp + r.unavailable == r.total);
}

TEST_CASE("the first tick with a CR15NetGame publishes the netvar surface into services") {
    Env& e = TheEnv();
    // Published on the first tick carrying a CR15NetGame -- the case above owns that tick and asserts
    // it there. What is asserted here is that it is published ONCE.
    REQUIRE(e.services.netVar != nullptr);
    replay::INetVar* first = e.services.netVar;

    // Created once: the second tick must not replace the surface a consumer is already holding.
    e.ModuleTick(e.netGame.bytes);
    CHECK(e.services.netVar == first);
    CHECK(e.log.Find("netvar surface:") != nullptr);
}

// â˜… W1-L (d35013b) made IWiretap::Listen refuse with kInvalidHandle -- rather than hang the game
// thread -- when the engine's CR15NetGame listener lock is still held at its 2000 us deadline, and
// its diagnostic says the answer is "retry on a later tick". This is the other half of that: capture
// registered its taps in exactly ONE pass, so a type refused for one busy frame was refused for the
// life of the process, and a stream missing for that reason looked exactly like a quiet one.
//
// The claims are about counts of Listen calls per struct name, not about a log line alone: a retry
// that logs and does not re-ask, and a give-up that stops logging but keeps asking, both pass a
// log-only check.
TEST_CASE("a refused tap is retried on a later tick and a permanent refusal is abandoned aloud") {
    Env& e = TheEnv();

    // Enough ticks to reach the give-up bound whichever cases ran before this one. Every assertion
    // below is on a settled state or an exact call count, so extra ticks cannot manufacture a pass.
    for (int i = 0; i < 12; ++i) e.ModuleTick(e.netGame.bytes);

    // 1. Refused once, registered on a later tick. Under the old single pass the second call never
    //    happens: Count stays 1 and Granted stays 0.
    CHECK(e.wiretap.Count("SR15NetComponentEvent") == 2);
    CHECK(e.wiretap.Granted("SR15NetComponentEvent") == 1);

    // 2. A type that never succeeds is abandoned at the bound -- asked exactly five times over five
    //    ticks and never a sixth, however many ticks follow.
    CHECK(e.wiretap.Count("SR15NetSetOwner") == 5);
    CHECK(e.wiretap.Granted("SR15NetSetOwner") == 0);
    const Line* gone = e.log.Find("tap-registration-abandoned SR15NetSetOwner after 5 attempts");
    REQUIRE(gone != nullptr);
    CHECK(gone->level == replay::LogLevel::Error);

    // 3. The reason reaches the log. Before this, capture.cpp printed the struct name alone, so
    //    `wiretap-lock-timeout` existed on the interface and in no file anyone reads afterwards.
    const Line* refused = e.log.Find("tap refused for SR15NetSetOwner");
    REQUIRE(refused != nullptr);
    CHECK(refused->text.find("wiretap-lock-timeout") != std::string::npos);
    CHECK(gone->text.find("wiretap-lock-timeout") != std::string::npos);

    // 4. A refused handle was never counted as a registration. 0xFFFF stored in g.taps would be
    //    Unlisten'd at shutdown and would inflate the summary the manifest is reconciled against.
    //    The expected total is derived from the table, not written down, so it survives the table
    //    growing: everything except the netvar stream (INetVar registers that one) and our own
    //    snapshot type, less the one type abandoned above.
    unsigned expected = 0;
    for (uint16_t i = 0; i < replay::capture::TapTypeCount(); ++i) {
        const replay::capture::TapType& t = replay::capture::TapTable()[i];
        if (t.stream == replay::kStreamNetvar) continue;
        if (strcmp(t.structName, "ReplayIdentitySnapshot") == 0) continue;
        ++expected;
    }
    REQUIRE(expected > 1);
    --expected;

    // â˜… L5 run 3 F8: this line read "36 of 38 message taps registered" -- two rows unaccounted for
    //   and unnamed, so a reader could not tell a row left out on purpose from one that was dropped.
    //   Every row of the table is now in one of five buckets, the buckets are made to add up ON THE
    //   LINE, and each row that is not registered is named with the state that made it so.
    const Line* summary = e.log.FindLast("tap-registration: ");
    REQUIRE(summary != nullptr);
    unsigned reg = 0, retrying = 0, givenUp = 0, unavail = 0, excluded = 0, sum = 0, rows = 0;
    REQUIRE(sscanf_s(summary->text.c_str(),
                     "tap-registration: %u registered, %u deferred, %u givenUp, %u unavailable, "
                     "%u excluded = %u of %u tap-table rows",
                     &reg, &retrying, &givenUp, &unavail, &excluded, &sum, &rows) == 7);
    // Every reachable tap type except the one the fixture refuses forever. NOT the S_NETVAR
    // listener: the case above drove it to its attempt bound, so it is one of the two given up on.
    CHECK(reg == expected);
    CHECK(retrying == 0u);
    CHECK(givenUp == 2u);          // SR15NetSetOwner and the S_NETVAR listener
    CHECK(unavail == 0u);
    CHECK(excluded == 1u);         // ReplayIdentitySnapshot: ours, never a wire message
    CHECK(rows == unsigned(replay::capture::TapTypeCount()));
    CHECK(reg + retrying + givenUp + unavail + excluded == rows);
    CHECK(sum == rows);
    // Named, not just counted. AUDIT_M5 F8-buf moved the list off the summary line and onto as many
    // `not registered:` lines as the table needs, because ILog truncates a line at kLogLineMax and
    // says nothing when it does -- so the row is looked for in the log, not in this one line.
    CHECK(e.log.FindLast("SR15NetSetOwner=givenUp") != nullptr);
    CHECK(e.log.FindLast("ReplayIdentitySnapshot=excluded-by-design") != nullptr);
    CHECK(summary->text.find("(+") == std::string::npos);   // nothing was truncated out of the list
}

// â˜… AUDIT_M2_P3R6_TAPRETRY_W02R7 and AUDIT_M1_SPIN_TRAP_SMALL10_W02R6_DOCCHECK F2: the retry above
// fixed one bug and paid for it with another. W1-L's 2000 us deadline is per Listen, and the walk
// asked every pending type in one game-thread tick -- 36 refused types is ~72 ms in a frame, every
// frame, for as long as the engine's listener lock is held.
//
// â˜… AUDIT_M3 F2 widened the property, and this case is the widened one: the bound is on the TICK, not
// on one walker. Everything the capture registers -- the 36 tap types and the S_NETVAR listener -- is
// driven from one place and spends at most ONE deadline between them; what did not get asked is
// deferred to the next tick. Eight ticks, because that crosses the handover: the netvar listener asks
// first and is abandoned at the attempt bound, and only then does the tap walk become what spends the
// tick's deadline. A bound that held on each side and not on the tick it changes hands is not a bound.
TEST_CASE("one tick spends at most one lock deadline across everything it registers") {
    Env& e = TheEnv();
    const size_t logMark = e.log.lines.size();
    const size_t grantMark = e.wiretap.granted.size();

    replay::capture::ResetTapRegistrationForTest();
    e.wiretap.refuseEverything = true;
    e.wiretap.refuseCheap.clear();

    const uint16_t reachable = ReachableTapTypes();
    // Without several pending types the bound below is a bound on nothing: one deadline and the whole
    // table would cost the same.
    REQUIRE(reachable >= 10);

    // Phase 1 -- the handover. The netvar listener asks first, so while it is still being retried it
    // is what spends each tick's deadline and the tap walk is not reached at all. Driven until it is
    // settled rather than for a written-down number of ticks, so the case follows the attempt bound.
    int handover = 0;
    while (replay::capture::TapRegistrationNow().givenUp == 0) {
        const size_t   callsBefore = e.wiretap.calls.size();
        const uint32_t timeoutsBefore = e.wiretap.timeouts;
        e.ModuleTick(e.netGame.bytes);
        // One Listen reached the deadline and everything else was deferred to a later tick.
        CHECK(e.wiretap.timeouts - timeoutsBefore == 1u);
        CHECK(e.wiretap.calls.size() - callsBefore == 1u);
        ++handover;
        REQUIRE(handover < 20);   // a listener retried forever is the defect, not a slow test
    }
    REQUIRE(handover > 1);

    // Phase 2 -- the tap walk is now what spends the tick's deadline, and the physical claim is
    // measured over THESE ticks only. A best-of taken across both phases would take its minimum from
    // a tick that only ever issued one Listen, and a build walking all 36 would pass it on that
    // sample alone.
    uint64_t bestUs = ~0ull;
    for (int t = 0; t < 4; ++t) {
        const size_t   callsBefore = e.wiretap.calls.size();
        const uint32_t timeoutsBefore = e.wiretap.timeouts;
        const uint64_t t0 = QpcTicks();
        e.ModuleTick(e.netGame.bytes);
        const uint64_t us = UsBetween(t0, QpcTicks());

        // The deterministic half: without the early stop both deltas are `reachable`.
        CHECK(e.wiretap.timeouts - timeoutsBefore == 1u);
        CHECK(e.wiretap.calls.size() - callsBefore == 1u);
        if (us < bestUs) bestUs = us;
    }
    // Every tick of both phases refused, and registered nothing, which is what makes the tail below a
    // real deferral rather than a table that had already emptied itself.
    CHECK(e.wiretap.granted.size() == grantMark);

    // The physical half, and the reason the counts matter: the tick itself costs about one deadline.
    // Best of four samples, so one preempted tick cannot fail a correct build; a walk over the whole
    // table cannot come in under this on any sample, because the spinning alone is reachable x 2000 us.
    const uint64_t wholeWalkUs = uint64_t(reachable) * FakeWiretap::kDeadlineUs;
    MESSAGE("tap walk, best of four ticks: " << bestUs << " us; a whole-table walk would spin "
                                             << wholeWalkUs << " us");
    CHECK(bestUs < 20000ull);
    CHECK(bestUs * 3 < wholeWalkUs);

    // â˜… AUDIT_M3 F4. Two lines over nine identical-looking ticks, not nine: the line is keyed on the
    // PAIR (unsettled, given up on), so the four ticks that changed neither say nothing, and the tick
    // that gave up on the netvar listener says so because `given up on` moved -- a count that fell
    // because a surface was ABANDONED must not be reported as one left for a later tick.
    CHECK(CountLinesSince(e.log, logMark, "tap-registration-deferred") == 2u);
    // FindLast, not Find: the case above this one stops a walk too, so the FIRST deferred line in the
    // log belongs to it. The one this case is about is the most recent.
    const Line* deferred = e.log.FindLast("tap-registration-deferred");
    REQUIRE(deferred != nullptr);
    CHECK(deferred->level == replay::LogLevel::Warn);
    CHECK(deferred->text.find("wiretap-lock-timeout") != std::string::npos);
    unsigned left = 0, total = 0, givenUp = 0;
    REQUIRE(sscanf_s(deferred->text.c_str(),
                     "tap-registration-deferred: one lock deadline spent this tick, %u of %u "
                     "message-bus registrations still unsettled, %u",
                     &left, &total, &givenUp) == 3);
    CHECK(left >= 10u);
    CHECK(total == unsigned(reachable) + 1u);
    CHECK(givenUp == 1u);   // the netvar listener, abandoned at the bound five ticks in
}

// The other half of the bound: deferred must mean deferred. Every type the stalled walk never even
// asked for has to register once the lock is free, or the fix has traded 72 ms a tick for lost
// streams -- exactly the defect the retry pass existed to close.
TEST_CASE("the types the walk deferred all register on later ticks once the lock is free") {
    Env& e = TheEnv();
    const uint16_t reachable = ReachableTapTypes();
    const size_t   grantMark = e.wiretap.granted.size();

    // A free lock, and no type singled out: SR15NetSetOwner's permanent refusal belongs to the case
    // above it, and leaving it on would make "everything registers" untrue for an unrelated reason.
    e.wiretap.refuseEverything = false;
    e.wiretap.refuseAlways.clear();
    for (int i = 0; i < 4; ++i) e.ModuleTick(e.netGame.bytes);

    unsigned    registered = 0;
    std::string missing;
    for (uint16_t i = 0; i < replay::capture::TapTypeCount(); ++i) {
        const replay::capture::TapType& t = replay::capture::TapTable()[i];
        if (t.stream == replay::kStreamNetvar) continue;
        if (strcmp(t.structName, "ReplayIdentitySnapshot") == 0) continue;
        if (e.wiretap.GrantedSince(grantMark, t.structName) == 1) {
            ++registered;
        } else if (missing.empty()) {
            missing = t.structName;
        }
    }
    if (!missing.empty()) MESSAGE("not registered after the lock freed: " << missing);
    CHECK(missing.empty());
    CHECK(registered == unsigned(reachable));
    CHECK(e.wiretap.granted.size() - grantMark == size_t(reachable));

    const Line* summary = e.log.FindLast("tap-registration: ");
    REQUIRE(summary != nullptr);
    unsigned reg = 0, retrying = 0, givenUp = 0, unavail = 0, excluded = 0, sum = 0, rows = 0;
    REQUIRE(sscanf_s(summary->text.c_str(),
                     "tap-registration: %u registered, %u deferred, %u givenUp, %u unavailable, "
                     "%u excluded = %u of %u tap-table rows",
                     &reg, &retrying, &givenUp, &unavail, &excluded, &sum, &rows) == 7);
    // Every reachable tap type. NOT the S_NETVAR listener: the case above drove it to its give-up
    // bound, and the line has to keep saying so rather than quietly counting 37 of 37.
    CHECK(reg == unsigned(reachable));
    CHECK(retrying == 0u);
    CHECK(givenUp == 1u);
    CHECK(unavail == 0u);
    CHECK(sum == rows);
}

// â˜… AUDIT_M3 F1, the must-fail control the tap walk was missing. Only a spent lock deadline may end a
// tick's walk. The other four refusals the shipping wiretap can give are cheap -- nothing was waited
// for -- and stopping on one of them costs the whole rest of the table a tick, five times over, for
// nothing. This drives the pool-exhausted shape at the FIRST type the walk asks for, so a walk that
// stopped on it would register no tap at all, and asks that every type after it registered in the SAME
// tick.
//
// The mutation build `W2A_MUTATE_STOP_ON_ANY_REFUSAL` replaces the discriminator with `true`; this case
// is what turns that build red.
TEST_CASE("a refusal that is not a lock deadline does not end the tick's walk") {
    Env& e = TheEnv();
    const size_t logMark = e.log.lines.size();

    replay::capture::ResetTapRegistrationForTest();
    e.wiretap.refuseEverything = false;
    e.wiretap.refuseAlways.clear();
    e.wiretap.refuseOnce.clear();

    const char* first = FirstReachableTapType();
    REQUIRE(first != nullptr);
    e.wiretap.refuseCheap = first;

    const uint16_t reachable = ReachableTapTypes();
    REQUIRE(reachable >= 10);
    const size_t   callsBefore = e.wiretap.calls.size();
    const size_t   grantMark = e.wiretap.granted.size();
    const uint32_t timeoutsBefore = e.wiretap.timeouts;

    e.ModuleTick(e.netGame.bytes);

    // Nothing waited for a lock: this refusal must not be mistaken for one that did.
    CHECK(e.wiretap.timeouts == timeoutsBefore);
    // One tick asked for the netvar listener and every reachable type. Under the mutant the walk stops
    // at `first` and this delta is 2 -- the netvar listener plus the one refused type.
    CHECK(e.wiretap.calls.size() - callsBefore == size_t(reachable) + 1u);
    // And they registered, in that same tick: reachable - 1 taps plus the netvar listener.
    CHECK(e.wiretap.granted.size() - grantMark == size_t(reachable));
    CHECK(e.wiretap.GrantedSince(grantMark, first) == 0);

    // A cheap refusal still costs one of its five attempts and still names its own reason, so a type
    // refused this way is not silently retried forever and the log does not blame the lock for it.
    const Line* refused = e.log.FindLast("tap refused for");
    REQUIRE(refused != nullptr);
    CHECK(refused->text.find(first) != std::string::npos);
    CHECK(refused->text.find(kPoolExhaustedText) != std::string::npos);
    CHECK(refused->text.find("wiretap-lock-timeout") == std::string::npos);

    // No deadline was spent, so nothing was deferred and nothing says it was.
    CHECK(CountLinesSince(e.log, logMark, "tap-registration-deferred") == 0u);

    e.wiretap.refuseCheap.clear();
}

// â˜… AUDIT_M3 F2. S_NETVAR was the one stream outside the retry: netvar::Create issued its single
// Listen from the constructor, on the tick the tap is built -- the state-9 transition, the tick most
// likely to be contended -- and one lost race emptied the stream for the whole match. capture.cpp
// states the rule that broke in its own words: "a stream lost to one busy frame is lost for the whole
// match." The listener is now driven by the same walk, under the same bound, with the same give-up.
TEST_CASE("the S_NETVAR listener is retried on later ticks and registers once the lock is free") {
    Env& e = TheEnv();
    const char* netvarType = NetvarTapType();
    REQUIRE(netvarType != nullptr);

    replay::capture::ResetTapRegistrationForTest();
    e.wiretap.refuseEverything = true;
    e.wiretap.refuseCheap.clear();
    const size_t callsMark = e.wiretap.calls.size();
    const size_t grantMark = e.wiretap.granted.size();

    // Two contended ticks. Asked once per tick, never twice in one -- the netvar Listen draws on the
    // tick's one deadline like everything else.
    for (int i = 0; i < 2; ++i) {
        const size_t callsBefore = e.wiretap.calls.size();
        e.ModuleTick(e.netGame.bytes);
        CHECK(e.wiretap.calls.size() - callsBefore == 1u);
    }
    size_t asked = 0;
    for (size_t i = callsMark; i < e.wiretap.calls.size(); ++i) asked += (e.wiretap.calls[i] == netvarType);
    CHECK(asked == 2u);
    CHECK(e.wiretap.GrantedSince(grantMark, netvarType) == 0);

    // The lock frees. Under the old single-shot Create this is the assertion that fails: the surface
    // was refused once and never asked again for the life of the process.
    e.wiretap.refuseEverything = false;
    e.wiretap.refuseAlways.clear();
    e.wiretap.refuseOnce.clear();
    e.ModuleTick(e.netGame.bytes);
    CHECK(e.wiretap.GrantedSince(grantMark, netvarType) == 1);

    // Registered means settled: no later tick asks again.
    const size_t settled = e.wiretap.calls.size();
    for (int i = 0; i < 3; ++i) e.ModuleTick(e.netGame.bytes);
    size_t askedAgain = 0;
    for (size_t i = settled; i < e.wiretap.calls.size(); ++i)
        askedAgain += (e.wiretap.calls[i] == netvarType);
    CHECK(askedAgain == 0u);
}

// The other end of that bound. A netvar listener retried forever would be a Listen on every tick of
// the match, each costing a deadline on the game thread; the give-up is the same one the taps get, and
// it is loud, because S_NETVAR empty because we stopped asking must never look like a match in which
// nothing changed.
TEST_CASE("the S_NETVAR listener is given up on aloud after the same bound as a tap") {
    Env& e = TheEnv();
    const char* netvarType = NetvarTapType();
    REQUIRE(netvarType != nullptr);

    replay::capture::ResetTapRegistrationForTest();
    e.wiretap.refuseEverything = true;
    e.wiretap.refuseCheap.clear();
    const size_t callsMark = e.wiretap.calls.size();

    // Well past the bound: the count below is what says asking stopped, not the loop.
    for (int i = 0; i < 9; ++i) e.ModuleTick(e.netGame.bytes);
    size_t asked = 0;
    for (size_t i = callsMark; i < e.wiretap.calls.size(); ++i)
        asked += (e.wiretap.calls[i] == netvarType);
    CHECK(asked == 5u);

    const Line* gone = e.log.FindLast("netvar-registration-abandoned");
    REQUIRE(gone != nullptr);
    CHECK(gone->level == replay::LogLevel::Error);
    CHECK(gone->text.find("wiretap-lock-timeout") != std::string::npos);
    unsigned attempts = 0;
    REQUIRE(sscanf_s(gone->text.c_str(), "netvar-registration-abandoned after %u", &attempts) == 1);
    CHECK(attempts == 5u);

    // Given up on is a state, not just a line: it leaves the tick's budget to the taps from then on.
    const replay::capture::TapRegistration r = replay::capture::TapRegistrationNow();
    CHECK(r.givenUp >= 1u);
}

// â˜… AUDIT_M3 F3. Registration state was log-only. Nothing outside capture.cpp could tell a capture
// with 0 of 37 registrations from a healthy one -- tests/replay's observer row 8 asked
// IWiretap::Available(), which answers whether the tap COULD work, not whether anything is on it.
TEST_CASE("TapRegistrationNow reports registered, deferred and given up, and they add up") {
    Env& e = TheEnv();
    const uint16_t reachable = ReachableTapTypes();

    replay::capture::ResetTapRegistrationForTest();
    e.wiretap.refuseEverything = true;
    e.wiretap.refuseCheap.clear();

    replay::capture::TapRegistration r = replay::capture::TapRegistrationNow();
    // Re-armed, so nothing has been asked yet -- which is not the same claim as asked-and-refused,
    // and `started` is what carries the difference.
    CHECK_FALSE(r.started);
    // The total is every surface the capture must register: the reachable tap types plus the one
    // S_NETVAR listener. A capture whose netvars are empty is as incomplete as one missing a tap.
    CHECK(r.total == unsigned(reachable) + 1u);
    CHECK(r.registered == 0u);
    CHECK(r.deferred == r.total);
    CHECK(r.givenUp == 0u);
    CHECK(r.unavailable == 0u);
    // The identity is a real check: the counts come from different pieces of state -- the handles
    // held, the give-up tally, the terminal flag and the per-type settled flags -- so a bookkeeping
    // bug breaks it. The second identity closes the table: every row is in exactly one bucket, which
    // is what L5 run 3's "36 of 38" could not say.
    CHECK(r.registered + r.deferred + r.givenUp + r.unavailable == r.total);
    CHECK(r.total + r.excluded == r.tableRows);
    CHECK(r.tableRows == unsigned(replay::capture::TapTypeCount()));
    CHECK(r.excluded == 1u);

    // A contended tick registers nothing, and Available() is true throughout: that difference is the
    // whole reason this accessor exists.
    e.ModuleTick(e.netGame.bytes);
    CHECK(e.wiretap.Available());
    r = replay::capture::TapRegistrationNow();
    CHECK(r.started);   // asked now, and refused: a zero count from here on is a refusal
    CHECK(r.registered == 0u);
    CHECK(r.registered + r.deferred + r.givenUp + r.unavailable == r.total);

    // A free lock, and enough ticks for the deferred tail: registered == total is the question the
    // observer has to ask.
    e.wiretap.refuseEverything = false;
    e.wiretap.refuseAlways.clear();
    e.wiretap.refuseOnce.clear();
    for (int i = 0; i < 4; ++i) e.ModuleTick(e.netGame.bytes);
    r = replay::capture::TapRegistrationNow();
    CHECK(r.registered == r.total);
    CHECK(r.deferred == 0u);
    CHECK(r.givenUp == 0u);
    CHECK(r.unavailable == 0u);
}

// â˜… AUDIT_M4 M1 and M2 together, because one state answers both. A tap whose Available() is false is
// BUILT and asks nothing: R2 reported its 36 unsettled types as `deferred` -- "still being tried" --
// for the life of the process, and reported `started` true from the tap POINTER, so the surface
// claimed registration had been attempted when not one Listen was ever issued.
TEST_CASE("an unavailable tap is unavailable, not deferred, and never counts as started") {
    Env& e = TheEnv();
    const uint16_t reachable = ReachableTapTypes();
    const size_t callsBefore = e.wiretap.calls.size();

    e.wiretap.available = false;
    replay::capture::ResetTapRegistrationForTest();
    e.ModuleTick(e.netGame.bytes);

    const replay::capture::TapRegistration r = replay::capture::TapRegistrationNow();
    // Not one Listen was issued -- by the walk, which refuses before the table, or by netvar::TryListen,
    // which answers TapUnavailable without calling the tap.
    CHECK(e.wiretap.calls.size() == callsBefore);
    CHECK_FALSE(r.started);
    CHECK(r.unavailable == r.total);
    CHECK(r.unavailable == unsigned(reachable) + 1u);
    CHECK(r.deferred == 0u);   // nothing is retrying, so nothing may be reported as retrying
    CHECK(r.givenUp == 0u);
    CHECK(r.registered == 0u);
    CHECK(r.registered + r.deferred + r.givenUp + r.unavailable == r.total);
    CHECK(e.log.FindLast("the message-bus tap is unavailable") != nullptr);

    // Put the surface back the way the rest of the file expects to find it.
    e.wiretap.available = true;
    replay::capture::ResetTapRegistrationForTest();
    int ticks = 0;
    while (replay::capture::TapRegistrationNow().deferred > 0u) {
        e.ModuleTick(e.netGame.bytes);
        REQUIRE(++ticks < 60);
    }
    CHECK(replay::capture::TapRegistrationNow().registered == r.total);
}

// â˜… AUDIT_M5 F3. The sampler case below is a smoke test, not a control: AUD-M5 built the pre-M3 shape
// -- six relaxed atomics, a reader free to land between two fields of one answer -- and the sampler
// passed 18/18 five runs running on it. The race is the wrong thing to mutate. The PROPERTY is
// deterministic: the snapshot changes at a publish and at nothing else. So this case changes a
// tick-owned global with no publish behind it and requires the accessor not to move.
TEST_CASE("the registration snapshot moves only at a publish, never with the tick's live globals") {
    Env& e = TheEnv();
    replay::capture::ResetTapRegistrationForTest();
    e.wiretap.refuseEverything = false;
    e.wiretap.refuseCheap.clear();

    // A settled, PUBLISHED answer first. Asserting that nothing moved is worth nothing against the
    // never-published default -- which is exactly what a removed publish leaves behind.
    int ticks = 0;
    while (replay::capture::TapRegistrationNow().deferred > 0u) {
        e.ModuleTick(e.netGame.bytes);
        REQUIRE(++ticks < 60);
    }
    const replay::capture::TapRegistration before = replay::capture::TapRegistrationNow();
    REQUIRE(before.registered > 0u);
    REQUIRE(before.started);

    // The perturbation has to be visible SOMEWHERE, or this case passes on a change that never
    // happened. The live view is the state a cross-thread reader would wrongly be reading.
    const replay::capture::TapRegistration live0 = replay::capture::TapRegistrationLiveForTest();
    REQUIRE(live0.givenUp == before.givenUp);
    replay::capture::PerturbTapRegistrationWithoutPublishingForTest();
    const replay::capture::TapRegistration live1 = replay::capture::TapRegistrationLiveForTest();
    REQUIRE(live1.givenUp == live0.givenUp + 1u);

    const replay::capture::TapRegistration after = replay::capture::TapRegistrationNow();
    CHECK(after.registered == before.registered);
    CHECK(after.deferred == before.deferred);
    CHECK(after.givenUp == before.givenUp);
    CHECK(after.unavailable == before.unavailable);
    CHECK(after.started == before.started);
    CHECK(after.total == before.total);
    CHECK(after.excluded == before.excluded);
    CHECK(after.tableRows == before.tableRows);
    CHECK(after.registered + after.deferred + after.givenUp + after.unavailable == after.total);

    // The perturbation is a lie about the table; the rest of the file must not inherit it.
    replay::capture::ResetTapRegistrationForTest();
    CHECK(replay::capture::TapRegistrationLiveForTest().givenUp == 0u);
}

// â˜… AUDIT_M4 M3. TapRegistrationNow() read six non-atomic globals the game thread writes, and a
// sampler landing between the handle-count increment and the settled flag saw a type counted as both
// registered and unsettled -- the header's own identity broken by the READER, not by a bookkeeping
// bug. A consumer taught that the check has false positives stops using it.
//
// The reader here hammers the accessor from another thread for the whole of a registration run. The
// property is not "it did not crash": every sample must satisfy the identity, and the samples must
// span the run rather than all landing on the settled end of it.
TEST_CASE("TapRegistrationNow is safe to sample from another thread and never breaks its identity") {
    Env& e = TheEnv();
    replay::capture::ResetTapRegistrationForTest();
    e.wiretap.refuseEverything = false;
    e.wiretap.refuseCheap.clear();

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> samples{0};
    std::atomic<uint64_t> bad{0};
    std::atomic<uint32_t> distinct{0};
    std::thread reader([&] {
        uint32_t lastRegistered = 0xFFFFFFFFu;
        while (!stop.load(std::memory_order_acquire)) {
            const replay::capture::TapRegistration r = replay::capture::TapRegistrationNow();
            if (r.registered + r.deferred + r.givenUp + r.unavailable != r.total ||
                r.total + r.excluded != r.tableRows) {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
            if (r.registered != lastRegistered) {
                lastRegistered = r.registered;
                distinct.fetch_add(1, std::memory_order_relaxed);
            }
            samples.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // The sampler must be live BEFORE the run, or the whole registration happens between the thread
    // being created and its first load and the case measures nothing.
    while (samples.load(std::memory_order_relaxed) == 0) {
        std::this_thread::yield();
    }
    for (int round = 0; round < 3; ++round) {
        replay::capture::ResetTapRegistrationForTest();
        int ticks = 0;
        while (replay::capture::TapRegistrationNow().deferred > 0u) {
            e.ModuleTick(e.netGame.bytes);
            REQUIRE(++ticks < 60);
        }
    }
    while (samples.load(std::memory_order_relaxed) < 5000) {
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);
    reader.join();

    CHECK(samples.load() > 1000u);
    // The run was actually sampled while it moved, not only after it settled: a reader that saw one
    // value throughout proves nothing about a torn one.
    CHECK(distinct.load() > 2u);
    CHECK(bad.load() == 0u);
}

// The cheap refusal on the NETVAR symbol, which no shipped case covered: the netvar listener asks
// first, and a refusal that did not spend the deadline must leave the tick to the tap walk. Reading
// `wiretap-lock-timeout` out of Diagnostic() instead of the tap's own counter would end the tick here.
TEST_CASE("a cheap refusal of the netvar listener does not cost the tick its tap walk") {
    Env& e = TheEnv();
    const uint16_t reachable = ReachableTapTypes();
    replay::capture::ResetTapRegistrationForTest();
    e.wiretap.refuseEverything = false;
    e.wiretap.refuseCheap = NetvarTapType();

    const size_t   callsBefore = e.wiretap.calls.size();
    const uint32_t timeoutsBefore = e.wiretap.timeouts;
    e.ModuleTick(e.netGame.bytes);

    // The netvar Listen plus the whole table in the same tick, and no deadline spent by any of them.
    CHECK(e.wiretap.calls.size() - callsBefore == size_t(reachable) + 1u);
    CHECK(e.wiretap.timeouts == timeoutsBefore);
    const replay::capture::TapRegistration r = replay::capture::TapRegistrationNow();
    CHECK(r.registered == unsigned(reachable));
    CHECK(r.deferred == 1u);      // the netvar listener, refused for a reason a retry can change
    CHECK(r.givenUp == 0u);
    CHECK(r.unavailable == 0u);
    CHECK(r.started);

    e.wiretap.refuseCheap.clear();
    e.ModuleTick(e.netGame.bytes);
    CHECK(replay::capture::TapRegistrationNow().registered == r.total);
}

// The world-origin read is the first thing every recording tick pushes, ahead of the tick record,
// and it must report "not sourced" on a host with no engine rather than fabricating (0,0,0) -- the
// distinction DESIGN S5.4.5 gives the `valid` field to carry.
TEST_CASE("the world-origin read refuses honestly instead of recording the origin") {
    const replay::capture::WorldOriginSample wo = replay::capture::ReadWorldOrigin();
    CHECK_FALSE(wo.valid);
    CHECK(wo.x == 0.0f);
    CHECK(wo.y == 0.0f);
    CHECK(wo.z == 0.0f);
}

// ---------------------------------------------------------------------------------------------
// L2-RUN-2 F1, the numerator half. The kill fired on the `gate` phase: 1793 us per tick while
// recording, and 1685-1941 us per tick for four minutes AFTER the capture was dead. Charging that
// to the capture's budget guarantees the kill regardless of how cheap capture is.
// ---------------------------------------------------------------------------------------------

namespace replay {
namespace capture {
CostMeter::WindowStat CostWindowForTest();
uint32_t PhaseWindowTicksForTest();
uint32_t PhaseWindowOpenTicksForTest();
}
namespace session {
int32_t StateRaw();
}
}  // namespace replay

TEST_CASE("an always-on tick phase is measured and reported but never charged to the capture") {
    using replay::capture::CostMeter;
    Env& e = TheEnv();
    e.session.busyUs = 0;
    e.session.state = replay::NetGameState::ReadyForGame;
    Sleep(50);

    const uint64_t windowsBefore = replay::capture::CostWindowForTest().windows;

    // Over the kill line by a quarter, so a charged version of this window is not a near miss. Two
    // full windows so the one the worker completes is entirely ours.
    e.session.busyUs = CostMeter::kKillMedianUs + 50;
    for (uint32_t i = 0; i < 2 * CostMeter::kWindowTicks; ++i) {
        e.EngineTick();
    }
    e.session.busyUs = 0;

    // The WORKER completes a window (WorkerMain calls Evaluate every pass), so this waits for it
    // rather than calling Evaluate itself and resetting the histogram underneath it.
    CostMeter::WindowStat w = replay::capture::CostWindowForTest();
    for (int i = 0; i < 400 && w.windows == windowsBefore; ++i) {
        Sleep(10);
        w = replay::capture::CostWindowForTest();
    }
    REQUIRE(w.windows > windowsBefore);

    MESSAGE("charged median in (" << w.medianLoUs << ", " << w.medianUs << "] us; uncharged mean "
                                  << w.unchargedMeanUs << " us / max " << w.unchargedMaxUs << " us");

    // Not vacuous: the always-on phase really did cost more than the kill line, and the meter still
    // reports it. A "fix" that merely stopped measuring the phase would pass without this.
    CHECK(w.unchargedMeanUs >= CostMeter::kKillMedianUs);
    CHECK(w.unchargedMaxUs >= CostMeter::kKillMedianUs);

    // The finding. Nothing was recording for any of those ticks, so the capture's own budget must
    // have seen essentially nothing. Charging the tick as a whole -- what shipped at L2 -- puts this
    // median in (200, 250] and kills the capture three windows later, with no capture open.
    CHECK(w.medianUs <= CostMeter::kMedianBudgetUs);
}

TEST_CASE("a tick with no capture open contributes no charged sample, not a cheap one") {
    using replay::capture::CostMeter;
    Env& e = TheEnv();
    e.session.busyUs = 0;
    e.session.state = replay::NetGameState::ReadyForGame;
    Sleep(50);

    const uint64_t windowsBefore = replay::capture::CostWindowForTest().windows;

    // The same expensive always-on tick as the case above, so the ticks are unmistakably real.
    e.session.busyUs = CostMeter::kKillMedianUs + 50;
    for (uint32_t i = 0; i < 2 * CostMeter::kWindowTicks; ++i) {
        e.EngineTick();
    }
    e.session.busyUs = 0;

    CostMeter::WindowStat w = replay::capture::CostWindowForTest();
    for (int i = 0; i < 400 && w.windows == windowsBefore; ++i) {
        Sleep(10);
        w = replay::capture::CostWindowForTest();
    }
    REQUIRE(w.windows > windowsBefore);

    MESSAGE("charged samples " << w.chargedSamples << " of " << CostMeter::kWindowTicks
                               << " window ticks; phase window " << replay::capture::PhaseWindowTicksForTest()
                               << " tick(s), " << replay::capture::PhaseWindowOpenTicksForTest()
                               << " open; uncharged mean " << w.unchargedMeanUs << " us");

    // The ticks happened and each of them cost more than the kill line, so a zero below is the
    // charge rule and not an absence of ticks.
    CHECK(w.unchargedMeanUs >= CostMeter::kKillMedianUs);
    CHECK(replay::capture::PhaseWindowTicksForTest() == CostMeter::kWindowTicks);

    // L2-RUN-8 F12, COUNTED RATHER THAN INFERRED. What shipped at L2 charged every tick, so a
    // window like this one held 512 near-zero charged samples -- indistinguishable, in the median
    // the kill rung reads, from 512 genuinely cheap ticks of an open capture, and enough of them
    // to drag a real cost below the line. With no capture open there is no charged work, so the
    // window must hold no charged sample at all.
    CHECK(w.chargedSamples == 0);
    CHECK(replay::capture::PhaseWindowOpenTicksForTest() == 0);
}

// â˜… LAST: it shuts the module down. Everything above runs against a live capture.
//
// RECORDER-2 part C, the arm no test had: a session that is NOT a live online match writes nothing.
// This is the 152-files-of-nothing defect, and the gate is a conjunct inside OnTick, so the only
// place it can be observed is the line it names its classification on.
TEST_CASE("a contained offline session is classified NOT recordable and opens no file") {
    Env& e = TheEnv();
    // Offline lock held, no live-online exemption: the verdict Part A calls Contained.
    replay::livesafety::SetForTest(/*liveOnlineExempt=*/false, /*offlineLockHeld=*/true);
    e.session.state = replay::NetGameState::ReadyForGame;
    e.EngineTick();   // not in game: the once-per-session latch reopens

    const size_t before = e.log.Size();
    e.session.state = replay::NetGameState::InGame;
    for (int i = 0; i < 40; ++i) {
        e.EngineTick();
        Sleep(2);
    }
    CHECK(e.log.Find("session gate: NOT recordable") != nullptr);
    CHECK(e.log.Find("session gate: recordable") == nullptr);
    CHECK(e.log.Find("no file will be written") != nullptr);
    // And nothing opened. `opened:` is the capture's own word for a file existing.
    (void)before;
    CHECK(e.log.Find("opened:") == nullptr);

    // Put the fixture back: every case after this one is a live capture by declaration.
    e.session.state = replay::NetGameState::ReadyForGame;
    e.EngineTick();
    replay::livesafety::SetForTest(/*liveOnlineExempt=*/true, /*offlineLockHeld=*/false);
}

// The chapter machinery is only reachable through the callback capture handed RegisterEarly, so this
// drives THAT pointer -- not capture::OnTick by name -- and asks whether a chapter came out the other
// end. A no-op'd RegisterEarly leaves earlyFn null and this case cannot even start.
TEST_CASE("driving the registered callback in-game opens and closes a chapter") {
    Env& e = TheEnv();
    e.session.state = replay::NetGameState::InGame;

    // The worker opens the file on its own thread; ticks keep arriving meanwhile, which is what the
    // engine does. 200 x 5 ms is two orders of magnitude over the worker's idle poll.
    for (int i = 0; i < 200; ++i) {
        e.EngineTick();
        Sleep(5);
    }

    // Leaving the match asks the worker to close. It closes on its own thread, so it is given the
    // same bounded settle as the open before the module is torn down.
    e.session.state = replay::NetGameState::ReadyForGame;
    for (int i = 0; i < 100; ++i) {
        e.EngineTick();
        Sleep(5);
    }

    // A second match, left OPEN: the shutdown stop is what closes it, so this is the stop path -- the
    // one whose `closed:` line the harness kills on (AUDIT_D_SC7 D-5).
    e.session.state = replay::NetGameState::InGame;
    for (int i = 0; i < 200; ++i) {
        e.EngineTick();
        Sleep(5);
    }

    // The owner closes the game mid-match: the engine calls RadPluginShutdown, which runs the
    // pre-exit hooks. No budget at all first -- the wait is bounded by what it is handed, so this
    // returns at once and says the worker was not done, which an unbounded wait never says.
    const size_t linesBeforePreExit = e.log.Size();   // the first match's `closed:` is above this
    const ULONGLONG preExitStart = GetTickCount64();
    replay::modreg::RunPreExit(0);
    CHECK(GetTickCount64() - preExitStart < 500);

    // Then the shipped shape: a budget the worker's close fits inside. The open capture is closed
    // by the time this returns -- by the worker, on the module's own state. The log is read only
    // now, with the worker gone.
    replay::modreg::RunPreExit(5000);
    CHECK(e.log.Find("pre-exit: the capture worker had not closed out after 0 ms") != nullptr);
    CHECK(e.log.Find("pre-exit: the capture worker closed out") != nullptr);
    bool closedByPreExit = false;
    for (size_t i = linesBeforePreExit; i < e.log.lines.size(); ++i) {
        closedByPreExit |= e.log.lines[i].text.find("closed: ") != std::string::npos;
    }
    CHECK(closedByPreExit);
    // And nothing was torn down to do it: the tap is the engine call capture::Shutdown makes, and
    // the voip sink is the detour's way into this module.
    CHECK(e.wiretap.unlistenAlls == 0);
    CHECK(replay::audio::voip::g_installedSink != nullptr);

    replay::modreg::ShutdownAll();
    CHECK(e.wiretap.unlistenAlls == 1);

    const Line* closed = e.log.Find("closed: ");
    if (!closed) {
        // A missing line is a verdict about something, and the log is the only place that says what.
        for (const Line& l : e.log.lines) MESSAGE(l.module << ": " << l.text);
    }
    REQUIRE(closed != nullptr);
    unsigned long long records = 0, chapters = 0, keyframes = 0;
    REQUIRE(sscanf_s(closed->text.c_str(), "closed: %llu records, %llu chapters, %llu keyframes",
                     &records, &chapters, &keyframes) == 3);
    CHECK(records > 0);
    CHECK(chapters >= 1);
    CHECK(keyframes >= 1);

    // Shutdown unregisters the sink, so a frame arriving after teardown is counted as no-sink by the
    // detour rather than being handed to freed state.
    CHECK(replay::audio::voip::g_installedSink == nullptr);

    // â˜… AUDIT_D_SC7 D-5. `closed:` is the harness's kill signal, so on the stop path it must be the
    // LAST line the module writes: the verdict and the sentinel are what the launch exists to produce,
    // and a kill in the window between them leaves neither. Order, not presence, is the claim here --
    // the same lines in the old order pass every other assertion in this file.
    int lastClosed = -1;
    int stopLine = -1;
    int sentinelLine = -1;
    for (size_t i = 0; i < e.log.lines.size(); ++i) {
        const Line& l = e.log.lines[i];
        if (l.module != "capture") continue;
        if (l.text.find("closed: ") != std::string::npos) lastClosed = int(i);
        if (l.text.find("stop (shutdown):") != std::string::npos) stopLine = int(i);
        if (l.text.find("stop sentinel capture_stop.") != std::string::npos) sentinelLine = int(i);
    }
    if (stopLine < 0 || sentinelLine < 0 || lastClosed < 0) {
        for (const Line& l : e.log.lines) MESSAGE(l.module << ": " << l.text);
    }
    REQUIRE(stopLine >= 0);
    REQUIRE(sentinelLine >= 0);
    REQUIRE(lastClosed >= 0);
    CHECK(stopLine < lastClosed);
    CHECK(sentinelLine < lastClosed);

    // The sentinel this launch was allowed to write, in a directory it named at Init, and nothing
    // else: the sweep at Init is what makes an absent one mean "this capture never finalised".
    const Line* init = e.log.Find("stop sentinels: ");
    REQUIRE(init != nullptr);
    wchar_t expected[64];
    REQUIRE(replay::capture::StopSentinelNameFor(replay::capture::LaunchFileTime(),
                                                 ::GetCurrentProcessId(), expected, 64));
    char narrow[64] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, expected, -1, narrow, sizeof(narrow), nullptr, nullptr);
    CHECK(init->text.find(narrow) != std::string::npos);
    // AUDIT_G G-8: Init also names the archive, so the harness reads where the durable copy is
    // instead of assuming it.
    char archiveName[64] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, replay::capture::kStopArchiveDirName, -1, archiveName,
                          sizeof(archiveName), nullptr, nullptr);
    CHECK(init->text.find(archiveName) != std::string::npos);

    // This test process is not a game launch, so its sentinels are litter in whatever directory it
    // was run from. They are its own to remove -- the name carries this pid.
    wchar_t cwd[MAX_PATH] = {};
    if (::GetCurrentDirectoryW(MAX_PATH, cwd)) {
        wchar_t victim[MAX_PATH];
        _snwprintf_s(victim, MAX_PATH, _TRUNCATE, L"%s\\%s", cwd, expected);
        CHECK(::GetFileAttributesW(victim) != INVALID_FILE_ATTRIBUTES);
        ::DeleteFileW(victim);

        // The archive copy, written by the SAME StopCapture call. A unit test on
        // ArchiveStopSentinel would prove the function; this proves the stop path calls it.
        //
        // The archive is named per CLOSE, not per launch, so it is matched by pattern.
        // Which ordinal this process is on depends on how many closes ran before this case, and the
        // point of the change is precisely that the archive keeps all of them.
        wchar_t pattern[MAX_PATH];
        _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\%s\\capture_stop.%016llX.%lu.*.json", cwd,
                     replay::capture::kStopArchiveDirName,
                     static_cast<unsigned long long>(replay::capture::LaunchFileTime()),
                     static_cast<unsigned long>(::GetCurrentProcessId()));
        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW(pattern, &fd);
        CHECK(h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                wchar_t archived[MAX_PATH];
                _snwprintf_s(archived, MAX_PATH, _TRUNCATE, L"%s\\%s\\%s", cwd,
                             replay::capture::kStopArchiveDirName, fd.cFileName);
                ::DeleteFileW(archived);
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
        }

        // The same litter in the working directory itself: the per-close copies beside the pointer.
        _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\capture_stop.%016llX.%lu.*.json", cwd,
                     static_cast<unsigned long long>(replay::capture::LaunchFileTime()),
                     static_cast<unsigned long>(::GetCurrentProcessId()));
        h = ::FindFirstFileW(pattern, &fd);
        CHECK(h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                wchar_t stray[MAX_PATH];
                _snwprintf_s(stray, MAX_PATH, _TRUNCATE, L"%s\\%s", cwd, fd.cFileName);
                ::DeleteFileW(stray);
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
        }
        wchar_t archiveDir[MAX_PATH];
        _snwprintf_s(archiveDir, MAX_PATH, _TRUNCATE, L"%s\\%s", cwd,
                     replay::capture::kStopArchiveDirName);
        ::RemoveDirectoryW(archiveDir);
    }
}

// ---------------------------------------------------------------------------------------------
// â˜… AUDIT_M6 F1. Everything below drives the REAL close path -- module Init, the engine's tick, the
// worker's own close -- instead of calling the sentinel writer directly. That the WRITER never
// replaces an earlier close's document was already proven in test_capturestop.cpp; what nothing in
// this repository could see was the CALLER declining to write one at all, which is what a
// launch-scoped latch above the close did to every close after the first.
// ---------------------------------------------------------------------------------------------

namespace replay {
namespace capture {
uint32_t OpenFailuresForTest();
bool AddStopWatchDirForTest(const wchar_t* dir);
uint32_t StopWatchCountForTest();
const wchar_t* StopWatchDirForTest(uint32_t i);
void ArmDeadlineForTest(uint32_t ms);
void InjectCostWindowsForTest(uint32_t windows);
replay::ILog* BuilderLogForTest();
uint64_t CaptureWorkTicksForTest();
}
}  // namespace replay

namespace {

std::string ReadWholeFile(const wchar_t* path) {
    HANDLE h = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string out;
    char buf[4096];
    DWORD got = 0;
    while (::ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
    ::CloseHandle(h);
    return out;
}

// Every PER-CLOSE document this process has left in `dir`. The name carries this launch and this
// pid, so nothing another process or an earlier launch wrote is counted, and the pointer -- which
// has no ordinal group -- is excluded by the literal dot before the wildcard.
std::vector<std::string> PerCloseDocs(const wchar_t* dir) {
    std::vector<std::string> docs;
    wchar_t pattern[MAX_PATH];
    _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\capture_stop.%016llX.%lu.*.json", dir,
                 static_cast<unsigned long long>(replay::capture::LaunchFileTime()),
                 static_cast<unsigned long>(::GetCurrentProcessId()));
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return docs;
    do {
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);
        docs.push_back(ReadWholeFile(path));
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return docs;
}

// One field, read the way a harness reads it: the value as written, with the JSON quoting and the
// escaped separators undone so a path can be handed straight back to the filesystem.
std::string FieldOf(const std::string& doc, const char* key) {
    const std::string k = std::string("\"") + key + "\": ";
    const size_t at = doc.find(k);
    if (at == std::string::npos) return {};
    const size_t from = at + k.size();
    const size_t end = doc.find_first_of(",\n", from);
    std::string v = doc.substr(from, end - from);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) ++i;
        out.push_back(v[i]);
    }
    return out;
}

void RemoveThisLaunchsSentinels(const wchar_t* dir) {
    wchar_t archive[MAX_PATH];
    _snwprintf_s(archive, MAX_PATH, _TRUNCATE, L"%s\\%s", dir,
                 replay::capture::kStopArchiveDirName);
    const wchar_t* dirs[2] = {dir, archive};
    for (const wchar_t* d : dirs) {
        wchar_t pattern[MAX_PATH];
        _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\capture_stop.%016llX.%lu.*", d,
                     static_cast<unsigned long long>(replay::capture::LaunchFileTime()),
                     static_cast<unsigned long>(::GetCurrentProcessId()));
        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            wchar_t victim[MAX_PATH];
            _snwprintf_s(victim, MAX_PATH, _TRUNCATE, L"%s\\%s", d, fd.cFileName);
            ::DeleteFileW(victim);
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    ::RemoveDirectoryW(archive);
}

// The archive subtree is the WRITER being right: `ArchiveStopSentinel` keeps one
// document per close so a harness can collect them after the process is gone (AUDIT_G G-8), and it
// must outlive the next launch's Init sweep -- so the writer must NOT stop creating it. It is the
// removal that was wrong: `RemoveThisLaunchsSentinels` is deliberately scoped to one launch's names
// (it must never destroy another launch's evidence), so it leaves the archive directory behind and
// the removal below could not succeed. A scratch directory this case owns is emptied depth-first.
void RemoveDirectoryTree(const wchar_t* dir) {
    // Names are collected BEFORE anything is deleted. Deleting entries while a FindFirstFileW walk
    // is open lets the NTFS directory index rebalance under the enumeration, and FindNextFileW then
    // skips entries that were never visited -- which is what left this directory non-empty
    // (ERROR_DIR_NOT_EMPTY) after a walk that reported deleting everything it saw.
    std::vector<std::wstring> entries;
    std::vector<bool> isDir;
    wchar_t pattern[MAX_PATH];
    _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\*", dir);
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const wchar_t* n = fd.cFileName;
            if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
            entries.push_back(std::wstring(dir) + L"\\" + n);
            isDir.push_back((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        if (isDir[i]) {
            RemoveDirectoryTree(entries[i].c_str());
        } else {
            ::SetFileAttributesW(entries[i].c_str(), FILE_ATTRIBUTE_NORMAL);
            ::DeleteFileW(entries[i].c_str());
        }
    }
    ::RemoveDirectoryW(dir);
}

// AUDIT_M7 F2's control needs the path UNUSABLE, not momentarily absent, and it must not depend on
// EMPTYING it: the directory holds this launch's sentinels plus the accumulating archive, and
// removal reported ERROR_DIR_NOT_EMPTY (145) on every one of 40 attempts over 400 ms -- an entry
// inside is pending delete behind a handle, so "delete everything, then remove" cannot be the
// mechanism. Renaming the directory aside does not need it empty, and a FILE planted at the old
// name (the trick this case already uses on the capture directory above) makes the path unusable
// for good: nothing can be created inside a file, and no CreateDirectory can replace one.
std::wstring ParkedName(const wchar_t* dir) {
    // Per process, for the same reason ScratchDir() is (AUD-ARCH21-B B1a): a fixed name makes this
    // run's park collide with the last run's leftovers -- ERROR_ALREADY_EXISTS, and then four
    // failures in a case that has nothing to do with either. The leftover is not always removable
    // from the process that made it, so the name must not be shared in the first place.
    return std::wstring(dir) + L"-blind." + std::to_wstring(::GetCurrentProcessId());
}

bool BlindDirectory(const wchar_t* dir) {
    const std::wstring parked = ParkedName(dir);
    RemoveDirectoryTree(parked.c_str());
    if (::MoveFileW(dir, parked.c_str()) == 0) {
        MESSAGE("a watch directory could not be parked: last error " << ::GetLastError());
        return false;
    }
    HANDLE h = ::CreateFileW(dir, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        MESSAGE("a watch directory's name could not be blocked: last error " << ::GetLastError());
        return false;
    }
    ::CloseHandle(h);
    return true;
}

// The invariant, as a predicate over the log: no `closed:` line -- the line the harness kills this
// process on -- may be written unless a sentinel line for that same close came first. Returns the
// text of the first `closed:` that had none, or an empty string when every close was covered.
std::string FirstUnevidencedClose(const RecordingLog& log, size_t from) {
    bool sentinelPending = false;
    for (size_t i = from; i < log.lines.size(); ++i) {
        const Line& l = log.lines[i];
        if (l.module != "capture") continue;
        // `stop sentinel capture_stop.<name>` is in the FAILURE line as well -- "the
        // stop sentinel capture_stop.<name> could not be written into <dir>" -- so keying on it
        // meant a close whose sentinel write failed in every directory still satisfied "no `closed:`
        // line without a sentinel", which is the one scenario this invariant exists to forbid. Only
        // the success line says the close ordinal and `written into` in that order; no failure line
        // in capture.cpp can produce it.
        if (l.text.find("stop sentinel capture_stop.") != std::string::npos &&
            l.text.find(" of this launch) written into ") != std::string::npos) {
            sentinelPending = true;
        }
        if (l.text.find("closed: ") != std::string::npos) {
            if (!sentinelPending) return l.text;
            sentinelPending = false;
        }
    }
    return {};
}

void DriveTicks(Env& e, int n) {
    for (int i = 0; i < n; ++i) {
        e.EngineTick();
        Sleep(5);
    }
}

}  // namespace

TEST_CASE("a second Init captures again, and every close of every file leaves its own sentinel") {
    Env& e = TheEnv();
    wchar_t cwd[MAX_PATH] = {};
    REQUIRE(::GetCurrentDirectoryW(MAX_PATH, cwd) != 0);
    RemoveThisLaunchsSentinels(cwd);
    // The guard measures the main module ONCE and latches it; the recorder reads that latch at every
    // open. Driven here, before the first open below, so the headers this case produces have a real
    // fingerprint to carry -- without it LastIdentity() is zeroed and the assertion after the drives
    // would be comparing zeros against zeros.
    replay::buildguard::Check();

    // The case above shut the module down, so this is the SECOND Init in one process -- the case the
    // capture already contemplates for its tap state. Every per-launch latch has to be
    // reset with it: the worker's `quit`, the deadline's one-shot and the reopen suppression all
    // survived the first session, and a second session that inherits them records nothing, fires no
    // deadline and writes no sentinel (AUDIT_M6 F1(b)).
    //
    // One Init, one session, both findings: a THIRD InitAll in this process crashes inside another
    // packet's module, so the open-retry arm below runs in this session rather than in one of its own.
    e.session.state = replay::NetGameState::ReadyForGame;
    const size_t mark = e.log.lines.size();
    replay::modreg::InitAll(&e.services);
    // As the Env constructor does, and for the same reason: session.cpp's own module publishes the
    // real ISessionEntry over the struct at Init.
    e.services.session = &e.session;
    // The deadline of this SECOND session, armed directly rather than through the environment
    // variable, whose floor is 30 s (kCaptureSecsMin) -- the claim under test is that the first
    // session's `deadlineFired` no longer blocks it, not how the value was read.
    replay::capture::ArmDeadlineForTest(2500);

    // Match 1, held open past the 2.5 s deadline: the deadline closes a file with the match still
    // being played, which is a close that must NOT end capture for the launch.
    e.session.state = replay::NetGameState::InGame;
    DriveTicks(e, 800);
    // L2-RUN-11 F27, on the SHIPPED path: the open above is the module's own, so this is the builder
    // the live capture writes through. Its ILog is what the refused-round-boundary Warn line is
    // written to, and it was null in every launch because nothing ever called SetLog -- the count
    // reached the stop document and the line reached nobody.
    CHECK(replay::capture::BuilderLogForTest() == e.services.log);
    CHECK(replay::capture::BuilderLogForTest() != nullptr);
    e.session.state = replay::NetGameState::ReadyForGame;
    DriveTicks(e, 60);

    // Match 2, closed by a stop REQUEST -- the harness's own signal, and the close that used to latch
    // the launch and silence everything after it.
    e.session.state = replay::NetGameState::InGame;
    DriveTicks(e, 200);
    wchar_t request[MAX_PATH];
    _snwprintf_s(request, MAX_PATH, _TRUNCATE, L"%s\\%s", cwd, replay::capture::kStopRequestName);
    {
        HANDLE h = ::CreateFileW(request, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(h != INVALID_HANDLE_VALUE);
        ::CloseHandle(h);
    }
    DriveTicks(e, 400);   // the request is polled once a second
    e.session.state = replay::NetGameState::ReadyForGame;
    DriveTicks(e, 60);

    // Match 3: capture is always on (locked constraint 6), so a launch that was asked to stop still
    // records the next match -- and THAT close is the one the latch left with no evidence at all.
    e.session.state = replay::NetGameState::InGame;
    const uint64_t workBefore = replay::capture::CaptureWorkTicksForTest();
    DriveTicks(e, 200);
    // L2-RUN-11 F26, on the SHIPPED path. Every tick that pushes a record also walks the bodies, both
    // inside `if (captureOpen)`, so that predicate is the single denominator behind `bodies.refused`
    // and S_TICK alike -- as long as no tick charges work after it has published Phase::Closing. The
    // tick below is exactly that tick: it is the one that sees the match gone and asks for the close,
    // and the record it used to push could still be racing the closing drain.
    const uint64_t workRecording = replay::capture::CaptureWorkTicksForTest();
    REQUIRE(workRecording > workBefore);   // the capture really opened; a dead drive proves nothing
    e.session.state = replay::NetGameState::ReadyForGame;
    DriveTicks(e, 1);
    CHECK(replay::capture::CaptureWorkTicksForTest() == workRecording);
    DriveTicks(e, 59);
    CHECK(replay::capture::CaptureWorkTicksForTest() == workRecording);
    ::DeleteFileW(request);

    // Match 4, closed by S6.4's rung-5 kill through the REAL path. Nothing here names
    // StopReason::CostKill: the worker is asked for four windows of over-budget samples, feeds them
    // into the launch's own meter on its own thread, and the meter's own Evaluate climbs its own
    // ladder to rung 5, sets the CostKill bit, and the close takes it from the same mask a match end
    // uses. Before this the only cost-kill document in tests/capture was a StopFacts filled by hand.
    e.session.state = replay::NetGameState::InGame;
    DriveTicks(e, 60);
    replay::capture::InjectCostWindowsForTest(4);
    DriveTicks(e, 250);
    e.session.state = replay::NetGameState::ReadyForGame;
    DriveTicks(e, 60);

    // Every file this session closed, named by a document of its own, each naming a capture really on
    // disk. Read BEFORE the arm below takes the capture directory away.
    std::vector<std::string> closedFiles;
    for (const std::string& doc : PerCloseDocs(cwd)) {
        if (FieldOf(doc, "fileClosed") != "true") continue;
        const std::string file = FieldOf(doc, "file");
        CHECK_FALSE(file.empty());
        wchar_t wide[MAX_PATH] = {};
        ::MultiByteToWideChar(CP_UTF8, 0, file.c_str(), -1, wide, MAX_PATH);
        CHECK(::GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES);
        for (const std::string& seen : closedFiles) CHECK(seen != file);
        closedFiles.push_back(file);
    }
    CHECK(closedFiles.size() >= 3);

    // L3-RUN-10 F3, on the SHIPPED path. The files above were opened by the module's own OpenFile,
    // so their headers are the recorder's output and not a fixture's. Until this landed, SetBuild
    // had no non-test caller and every one of those headers carried sixteen zero bytes.
    //
    // The comparand is buildguard's OWN latched measurement, read through the same accessor the
    // recorder reads -- and it is asserted non-zero first, because an all-zero expectation would
    // make this assertion pass on exactly the defect it exists to catch. The offset comes from the
    // loader's own struct, never from a second copy of the layout.
    {
        const replay::BuildIdentity& id = replay::buildguard::LastIdentity();
        REQUIRE(id.md5Read);
        uint8_t expected[16] = {};
        REQUIRE(replay::capture::Md5HexToBytes(id.md5, expected));
        uint8_t zero[16] = {};
        REQUIRE(memcmp(expected, zero, 16) != 0);

        size_t headersRead = 0;
        for (const std::string& file : closedFiles) {
            wchar_t wide[MAX_PATH] = {};
            ::MultiByteToWideChar(CP_UTF8, 0, file.c_str(), -1, wide, MAX_PATH);
            HANDLE h = ::CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
            REQUIRE(h != INVALID_HANDLE_VALUE);
            replay::FileHeaderDisk hdr = {};
            DWORD got = 0;
            const BOOL ok = ::ReadFile(h, &hdr, DWORD(sizeof hdr), &got, nullptr);
            ::CloseHandle(h);
            REQUIRE(ok != 0);
            REQUIRE(got == sizeof hdr);
            CHECK(memcmp(hdr.echovrMd5, expected, 16) == 0);
            CHECK(memcmp(hdr.echovrMd5, zero, 16) != 0);
            ++headersRead;
        }
        CHECK(headersRead == closedFiles.size());
        CHECK(headersRead >= 3u);
    }

    // In the same session. The capture directory becomes a plain FILE, which is what a
    // full disk or an over-long path looks like from CreateFileW: every open fails, identically, for
    // the rest of the match. The tick re-arms the open on the very next frame, so with no bound this
    // is ~250 attempts and ~250 Error lines a second for as long as the match lasts.
    std::wstring captureDir;
    {
        wchar_t dir[MAX_PATH] = {};
        ::MultiByteToWideChar(CP_UTF8, 0, e.config.dir.c_str(), -1, dir, MAX_PATH);
        captureDir = dir;
    }
    for (const std::string& file : closedFiles) {
        wchar_t wide[MAX_PATH] = {};
        ::MultiByteToWideChar(CP_UTF8, 0, file.c_str(), -1, wide, MAX_PATH);
        ::DeleteFileW(wide);
    }
    const std::wstring moved = captureDir + L"-m6moved";
    REQUIRE(::MoveFileW(captureDir.c_str(), moved.c_str()) != 0);
    {
        HANDLE h = ::CreateFileW(captureDir.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(h != INVALID_HANDLE_VALUE);
        ::CloseHandle(h);
    }

    const size_t openMark = e.log.lines.size();
    const uint32_t before = replay::capture::OpenFailuresForTest();
    e.session.state = replay::NetGameState::InGame;
    DriveTicks(e, 2000);   // ten seconds of a match the capture cannot record
    const uint32_t attempts = replay::capture::OpenFailuresForTest() - before;
    const size_t errorLines = CountLinesSince(e.log, openMark, "could not open the capture");
    e.session.state = replay::NetGameState::ReadyForGame;
    DriveTicks(e, 20);

    replay::modreg::ShutdownAll();
    ::DeleteFileW(captureDir.c_str());
    ::MoveFileW(moved.c_str(), captureDir.c_str());

    // The schedule is 250 ms doubling to 30 s, so six attempts fall in the first ten seconds. The
    // bound is loose enough to survive a slow machine and nowhere near an unbounded retry.
    MESSAGE("open attempts in 10 s: " << attempts << ", Error lines: " << errorLines);
    CHECK(attempts >= 1);
    CHECK(attempts <= 8);
    CHECK(errorLines >= 1);   // one line per state CHANGE, and there is exactly one state here
    CHECK(errorLines <= 2);

    // 1. The invariant itself, over every close this session made.
    const std::string unevidenced = FirstUnevidencedClose(e.log, mark);
    if (!unevidenced.empty()) {
        for (size_t i = mark; i < e.log.lines.size(); ++i)
            MESSAGE(e.log.lines[i].module << ": " << e.log.lines[i].text);
    }
    CHECK(unevidenced == "");

    // 2. The second session's deadline fired. Inheriting `deadlineFired` from the first session is
    //    silent otherwise: capture simply runs on and nobody is told the deadline did not fire.
    CHECK(CountLinesSince(e.log, mark, "capture deadline was reached") >= 1);

    // 3. Each cause that can close a file in a test, carried by a document of its own. A cost kill
    //    needs the cost meter to reach rung 5; its document is driven in test_capturestop.cpp and it
    //    shares this path, reaching StopCapture through the same close-cause mask as a match end.
    std::vector<std::string> reasons;
    bool sawDegraded = false;
    bool sawBothCauses = false;
    for (const std::string& doc : PerCloseDocs(cwd)) {
        const std::string reason = FieldOf(doc, "reason");
        reasons.push_back(reason);
        // The mask and the word are two renderings of one fact, so they are cross-
        // checked against each other: a `closeCauses` that did not carry the reason the close was
        // filed under would be a list of something else.
        const std::string causes = FieldOf(doc, "closeCauses");
        // â˜… AUDIT_L2 F2 changed what this cross-check asserts, and the old form codified the
        // defect. A stop that met NO open file is filed under `no-op`, and `no-op` is not a close
        // cause -- it is the absence of one -- so the mask carries the request or the deadline that
        // arrived instead. The two-way rule: a document whose `fileClosed` is true names its reason
        // in the mask; a `no-op` document must NOT, must carry `fileClosed: false`, and must still
        // name what asked. Anything else is a sentinel describing a close that did not happen.
        const bool noOp = reason == "no-op";
        CHECK(noOp == (FieldOf(doc, "fileClosed") == "false" && FieldOf(doc, "file") == ""));
        if (noOp) {
            CHECK(causes.find("\"no-op\"") == std::string::npos);
            CHECK(causes.size() > 2);   // it still says what arrived
            CHECK(FieldOf(doc, "records") == "0");
            CHECK(FieldOf(doc, "chapters") == "0");
            CHECK(FieldOf(doc, "verified") == "false");
        } else {
            CHECK(causes.find("\"" + reason + "\"") != std::string::npos);
        }
        if (causes.find(',') != std::string::npos) sawBothCauses = true;
        // Both counters, and the launch total can never be below the streak this close
        // was taken in. A document carrying only the streak cannot tell a clean launch from one that
        // failed and recovered.
        const std::string streak = FieldOf(doc, "openFailures");
        const std::string total = FieldOf(doc, "openFailTotal");
        REQUIRE_FALSE(total.empty());
        CHECK(std::stoul(total) >= std::stoul(streak));
        CHECK(FieldOf(doc, "workerJoin") == "n/a");
        if (FieldOf(doc, "openFailState") == "writer-open") {
            sawDegraded = true;
            CHECK(FieldOf(doc, "fileClosed") == "false");
            CHECK(reason == "no-op");
            CHECK(std::stoul(streak) >= attempts);
            CHECK(std::stoul(total) >= attempts);
        }
    }
    MESSAGE("a close carrying more than one cause was observed: " << sawBothCauses);
    // The reason WORD is now "what this stop was", so a stop that closed nothing reads `no-op`. What
    // arrived is still evidenced, and it is evidenced in `closeCauses` -- so that is where the five
    // triggers are looked for. Reading them off `reason` is what let a no-op wear a close's word.
    std::string allCauses;
    for (const std::string& doc : PerCloseDocs(cwd)) allCauses += FieldOf(doc, "closeCauses");
    for (const char* want : {"deadline", "request", "match-end", "cost-kill", "shutdown"}) {
        const bool found = allCauses.find(std::string("\"") + want + "\"") != std::string::npos;
        if (!found) MESSAGE("no per-close sentinel carries close cause " << std::string(want));
        CHECK(found);
    }
    // And at least one document is a no-op: this fixture drives a shutdown stop with nothing open,
    // which is exactly L2's third close. Without this the new word could be dead code.
    bool sawNoOp = false;
    for (const std::string& r : reasons) sawNoOp = sawNoOp || r == "no-op";
    CHECK(sawNoOp);
    // 4. The degraded open state reaches the document, for a harness that reads sentinels not logs.
    CHECK(sawDegraded);

    RemoveThisLaunchsSentinels(cwd);
}

// ---------------------------------------------------------------------------------------------
// L2-RUN-2 F1, both halves. The kill fired on the `gate` phase: 1793 us per tick while recording,
// and 1685-1941 us per tick for four minutes AFTER the capture was dead. Two defects in one line --
// the phase is expensive, and it is charged to a budget no capture can influence.
// ---------------------------------------------------------------------------------------------

TEST_CASE("the tick's engine-state read costs no syscall per tick") {
    // What the gate issued per tick before this packet: three replay::Readable probes -- the
    // game-pointer slot, the CR15Game span and the NetGame state word -- each one a VirtualQuery,
    // i.e. NtQueryVirtualMemory, which takes the address space's lock and walks its VAD tree.
    //
    // â˜… WHAT THIS MEASURES AND WHAT IT CANNOT. This host is uncontended and has no echovr.exe, so
    // the probe figure below is the FLOOR of what one of them costs; L2's 1793 us per tick is that
    // same call under a game process's address-space churn and cannot be reproduced offline (the
    // same limit ENG-W2A-R8 stated for ReadWorldOrigin). What is reproduced exactly is the count:
    // three per tick before, zero after.
    std::vector<uint8_t> span(0x8A80, 0);
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    const int kIters = 20000;

    LARGE_INTEGER a{};
    LARGE_INTEGER b{};
    uint32_t readableHits = 0;
    QueryPerformanceCounter(&a);
    for (int i = 0; i < kIters; ++i) {
        readableHits += replay::Readable(span.data(), span.size()) ? 1u : 0u;
    }
    QueryPerformanceCounter(&b);
    const double probeNs = double(b.QuadPart - a.QuadPart) * 1e9 / double(f.QuadPart) / kIters;

    uint32_t stateSum = 0;
    QueryPerformanceCounter(&a);
    for (int i = 0; i < kIters; ++i) {
        stateSum += static_cast<uint32_t>(replay::session::StateRaw());
    }
    QueryPerformanceCounter(&b);
    const double gateNs = double(b.QuadPart - a.QuadPart) * 1e9 / double(f.QuadPart) / kIters;

    MESSAGE("one replay::Readable probe over the CR15Game span: " << probeNs
            << " ns, and the gate issued three per tick. session::StateRaw() whole: " << gateNs
            << " ns per call");

    // Both loops answered rather than being folded away: a valid heap span is readable every time,
    // and StateRaw refuses on a host with no engine instead of faulting on the unmapped slot.
    CHECK(readableHits == static_cast<uint32_t>(kIters));
    CHECK(stateSum == 0u);
    // The bound is taken from this run's own probe measurement rather than being a magic constant,
    // so it means the same thing on a faster machine: the WHOLE state read now costs less than a
    // quarter of ONE of the three probes it used to issue. Any VirtualQuery returning to this path
    // fails it -- which is what makes the 20 us/tick target hold by construction rather than by
    // hoping an uncontended syscall stays cheap.
    if (TimingChecksEnabled()) {
        CHECK(gateNs * 4.0 < probeNs);
    }
}

// AUDIT_M7 F1 and F2, in one session because they need the same unusual environment: a launch whose
// watch directories are the two DISTINCT ones production resolves, and which can then have both of
// them taken away underneath it.
TEST_CASE("the watch list is bounded and reset, and a close nobody could evidence is caught") {
    Env& e = TheEnv();
    wchar_t home[MAX_PATH] = {};
    REQUIRE(::GetCurrentDirectoryW(MAX_PATH, home) != 0);

    // The PRODUCTION shape. In the fixture as it stood only the CWD ever resolved, so
    // one Init added one directory and the array overflowed on the THIRD. On a launch where
    // Resolved() is true and the state directory is not the CWD, one Init fills both slots and the
    // SECOND overflows -- the fixture was weaker than production on exactly the variable that
    // decides when the defect fires, so the fixture is made production-shaped here.
    const std::wstring watch = std::wstring(home) + L"\\m7watch";
    const std::wstring root = std::wstring(home) + L"\\m7state";
    const std::wstring state = root + L"\\echoreplay";
    ::CreateDirectoryW(watch.c_str(), nullptr);
    ::CreateDirectoryW(root.c_str(), nullptr);
    ::CreateDirectoryW(state.c_str(), nullptr);
    replay::SetPathsForTest((root + L"\\plugins\\EchoReplay.dll").c_str());
    REQUIRE(replay::Resolved());
    REQUIRE(std::wstring(replay::StateDir()) == state);
    REQUIRE(::SetCurrentDirectoryW(watch.c_str()) != 0);
    RemoveThisLaunchsSentinels(watch.c_str());
    RemoveThisLaunchsSentinels(state.c_str());

    // Inits three and four of this process, both with two directories to add. Before the bound, the
    // third wrote stopWatchDirs[2] -- one element past a two-element array, over stopWatchCount, the
    // whole open-retry block and the close ordinal the per-close invariant rests on -- and the
    // process died with SIGSEGV inside capture's own Init, before any of its log lines.
    for (int session = 3; session <= 4; ++session) {
        e.session.state = replay::NetGameState::ReadyForGame;
        const size_t initMark = e.log.lines.size();
        replay::modreg::InitAll(&e.services);
        e.services.session = &e.session;

        std::string watchLine;
        for (size_t i = initMark; i < e.log.lines.size(); ++i) {
            if (e.log.lines[i].module != "capture") continue;
            if (e.log.lines[i].text.find("is polled once a second in:") != std::string::npos) {
                CHECK(watchLine.empty());   // one list per Init, not one per Init ever run
                watchLine = e.log.lines[i].text;
            }
        }
        REQUIRE_FALSE(watchLine.empty());
        // Two DISTINCT directories, every session. The same directory named twice is the reset
        // missing: the list carried the previous session's entries and was appended to again, and
        // every close then wrote its sentinel there twice, the second write refused by the never-
        // replace rule, raising "a reader polling that directory will see no sentinel for this
        // launch" -- falsely -- about a directory that had one.
        const size_t andAt = watchLine.find(" and ");
        REQUIRE(andAt != std::string::npos);
        const size_t colon = watchLine.find(": ");
        const std::string first = watchLine.substr(colon + 2, andAt - colon - 2);
        CHECK(first != watchLine.substr(andAt + 5, first.size()));
        CHECK(CountLinesSince(e.log, initMark, "will NOT be watched for") == 0);

        if (session == 3) {
            replay::modreg::ShutdownAll();
        }
    }

    // The bound itself. The fixture above was wrong to write a third directory
    // into a two-slot array, but the PRODUCT accepted it: an API that takes what it cannot hold.
    // Init never offers a third, so the refusal is driven directly here -- refused BY NAME, with
    // the two accepted directories and the count left exactly as they were.
    {
        const size_t boundMark = e.log.lines.size();
        REQUIRE(replay::capture::StopWatchCountForTest() == 2u);
        const std::wstring kept0 = replay::capture::StopWatchDirForTest(0);
        const std::wstring kept1 = replay::capture::StopWatchDirForTest(1);
        const std::wstring third = std::wstring(home) + L"\\m7watch3";

        CHECK_FALSE(replay::capture::AddStopWatchDirForTest(third.c_str()));
        // The canary. stopWatchCount is the word immediately after the array, so an unbounded
        // append writes the third path THROUGH it: a count still at the extent, and two slots
        // still holding what Init put there, is what says the write did not happen at all.
        CHECK(replay::capture::StopWatchCountForTest() == 2u);
        CHECK(std::wstring(replay::capture::StopWatchDirForTest(0)) == kept0);
        CHECK(std::wstring(replay::capture::StopWatchDirForTest(1)) == kept1);
        CHECK(CountLinesSince(e.log, boundMark, "will NOT be watched for") == 1);
        // BY NAME: a reader polling m7watch3 for a stop request learns from the log that nothing
        // will ever appear there, rather than waiting on a directory nobody watches.
        bool named = false;
        for (size_t i = boundMark; i < e.log.lines.size(); ++i)
            if (e.log.lines[i].text.find("m7watch3") != std::string::npos) named = true;
        CHECK(named);
    }

    // The close the shipped test could not reach: a launch that FAILED its opens and
    // then RECOVERED. The streak is cleared by the first success, so a document carrying only the
    // streak reports that launch as `0` / `none` -- byte-identical to one that never failed, for the
    // harness that reads documents and not logs. Only the launch total separates them.
    std::wstring captureDir;
    {
        wchar_t dir[MAX_PATH] = {};
        ::MultiByteToWideChar(CP_UTF8, 0, e.config.dir.c_str(), -1, dir, MAX_PATH);
        captureDir = dir;
    }
    const std::wstring parked = captureDir + L"-m7moved";
    REQUIRE(::MoveFileW(captureDir.c_str(), parked.c_str()) != 0);
    {
        HANDLE h = ::CreateFileW(captureDir.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(h != INVALID_HANDLE_VALUE);
        ::CloseHandle(h);
    }
    const size_t recoverMark = e.log.lines.size();
    e.session.state = replay::NetGameState::InGame;
    DriveTicks(e, 200);
    e.session.state = replay::NetGameState::ReadyForGame;
    DriveTicks(e, 30);
    ::DeleteFileW(captureDir.c_str());
    REQUIRE(::MoveFileW(parked.c_str(), captureDir.c_str()) != 0);
    e.session.state = replay::NetGameState::InGame;
    DriveTicks(e, 700);   // long enough to outlast the backoff the failures above armed
    e.session.state = replay::NetGameState::ReadyForGame;
    DriveTicks(e, 80);

    CHECK(CountLinesSince(e.log, recoverMark, "the capture opened after") >= 1);
    bool sawRecovered = false;
    for (const std::string& doc : PerCloseDocs(watch.c_str())) {
        if (FieldOf(doc, "fileClosed") != "true") continue;
        if (FieldOf(doc, "openFailures") != "0") continue;
        if (std::stoul(FieldOf(doc, "openFailTotal")) == 0) continue;
        sawRecovered = true;
        CHECK(FieldOf(doc, "openFailState") == "none");
    }
    CHECK(sawRecovered);

    // AUDIT_M7 F2's must-fail control. Both watch directories are taken away -- which is what a
    // removable drive, a cleaned-up harness directory or a revoked ACL looks like from the writer --
    // so this close's sentinel cannot be written ANYWHERE. The log still carries the failure's own
    // "stop sentinel capture_stop.<name> could not be written" line, which is the line the old
    // oracle mistook for evidence, and then the `closed:` line the harness kills on. The invariant
    // must catch this close; an oracle that cannot fail here cannot pass anywhere.
    REQUIRE(::SetCurrentDirectoryW(home) != 0);
    // NOT a REQUIRE: doctest's REQUIRE is fatal, and a teardown that cannot
    // remove a directory used to abort the whole binary mid-suite -- which is why this file reported
    // a different assertion total on every run (6063 / 3959 / 7067) and read as five unrelated
    // failures. The removal still has to succeed for the control below to mean anything, so it is
    // still asserted; it just stops the run from ending here.
    CHECK(BlindDirectory(watch.c_str()));
    CHECK(BlindDirectory(state.c_str()));

    const size_t blindMark = e.log.lines.size();
    e.session.state = replay::NetGameState::InGame;
    DriveTicks(e, 200);
    e.session.state = replay::NetGameState::ReadyForGame;
    DriveTicks(e, 120);

    CHECK(CountLinesSince(e.log, blindMark, "could not be written into") >= 1);
    const std::string blind = FirstUnevidencedClose(e.log, blindMark);
    if (blind.empty()) {
        for (size_t i = blindMark; i < e.log.lines.size(); ++i)
            MESSAGE(e.log.lines[i].module << ": " << e.log.lines[i].text);
    }
    CHECK(blind.find("closed: ") != std::string::npos);

    replay::modreg::ShutdownAll();
    ::DeleteFileW(watch.c_str());
    RemoveDirectoryTree(ParkedName(watch.c_str()).c_str());
    RemoveDirectoryTree(ParkedName(state.c_str()).c_str());
    RemoveDirectoryTree(root.c_str());
}

// ---------------------------------------------------------------------------------------------
// * THE PER-PHASE MICRO-BENCHMARK. L2-RUN-3 4(a) charged 167-331 us per tick with
// `gate` down at ~1 us, and `gather` held 160-328 of it. A phase total with no breakdown cannot
// tell OUR composition -- the loops, the translation, the staging, the ring push -- from the
// ENGINE READS those loops issue, and the two have different owners and different fixes.
//
// This case separates them over a realistic Echo Arena frame: eight players and the disc, all
// locally authored, which is the `-offline` host the owner records.
//
//   * the capture-side composition is MEASURED in ns per phase, here, on this host;
//   * the engine-facing CALL COUNT per tick is MEASURED too, by a fake that counts every accessor
//     the gather can reach;
//   * one `replay::Readable` probe is timed on the same run, because proving a span is mapped is
//     what an engine accessor spends its time on (src/engine/entity.cpp:193, :456, :587, :1208).
//
// The product of the last two is the engine-side FLOOR. It is a floor and not the field figure:
// `Readable` is `VirtualQuery` -> `NtQueryVirtualMemory`, whose cost is contention on the target
// address space's lock, and a host with no echovr.exe cannot reproduce that. ENG-W2A-R8 and
// ENG-COST both stated this limit for their own phases and it is not weakened here. What
// reproduces exactly is the COUNT, and the count is what a fix has to move.
namespace w2a_perf {

// Every accessor the gather can reach, counted separately. "72 engine calls a tick" does not say
// which loop to fix; `bodyPresence 64` beside `pawnRead 8` does.
struct EngineCalls {
    uint32_t physCount = 0;
    uint32_t bodyPresence = 0;   // IsLocallyControlled, once per POOL SLOT
    uint32_t bodyRead = 0;
    uint32_t bodyNetId = 0;
    uint32_t pawnCount = 0;
    uint32_t pawnAt = 0;
    uint32_t pawnRead = 0;
    uint32_t asSeenSample = 0;
    uint32_t asSeenPlayerCount = 0;
    uint32_t asSeenPlayer = 0;

    uint32_t Total() const {
        return physCount + bodyPresence + bodyRead + bodyNetId + pawnCount + pawnAt + pawnRead +
               asSeenSample + asSeenPlayerCount + asSeenPlayer;
    }
};

// The world as an `-offline` host holds it. `poolSlots` is deliberately NOT the number of live
// bodies: IEntity::PhysCount answers for the POOL, and gather.h walks it to kMaxBodies asking each
// slot whether this machine authors it. That walk is the shape the count below exposes.
struct FakeWorld {
    EngineCalls calls;
    size_t poolSlots = 64;      // what PhysCount answers -- the pool, not its occupancy
    uint16_t liveBodies = 9;    // eight player bodies and the disc
    uint16_t pawns = 8;

    bool Available() const { return true; }

    size_t PhysCount() { ++calls.physCount; return poolSlots; }
    bool IsLocallyControlled(replay::PhysHandle h) {
        ++calls.bodyPresence;
        return h.v < liveBodies;
    }
    // The gather asks through ReadBodyDetailed now. This fixture models no body source, so it says
    // so -- NotAsked -- rather than naming one it never read.
    template <class S>
    bool ReadBodyDetailed(replay::PhysHandle h, S& st, replay::BodyReading& r) {
        r = replay::BodyReading{};
        return ReadBody(h, st);
    }
    // The pool is 64 slots and `liveBodies` of them hold a body. A reader that answered for all 64
    // would make the fixture disagree with its own model -- the case below asserts 64 presence
    // probes for NINE answers -- and would measure a tick pushing 64 body records, which is not the
    // mix production pushes and not the one the kill line reads.
    template <class S>
    bool ReadBody(replay::PhysHandle h, S& st) {
        ++calls.bodyRead;
        memset(&st, 0, sizeof st);
        return h.v < liveBodies;
    }
    uint32_t NetIdOf(replay::PhysHandle h) {
        ++calls.bodyNetId;
        // Derived from the pool bound rather than a round literal, so every body in a full pool
        // gets a distinct non-zero net id and no value here can collide with a fact registry entry.
        return uint32_t(replay::capture::kMaxBodies) + 1u + h.v;
    }

    size_t LocalPlayerCount() { ++calls.pawnCount; return pawns; }
    replay::UserIndex LocalPlayerAt(size_t i) {
        ++calls.pawnAt;
        return replay::UserIndex(static_cast<uint16_t>(i));
    }
    template <class S, class R>
    uint32_t ReadLocalPlayerDetailed(replay::UserIndex, S& st, R& r) {
        ++calls.pawnRead;
        memset(&st, 0, sizeof st);
        r = R();
        // Every group this build can store, so the row is WRITTEN and the sink carries production's
        // record mix. A refused pawn would make the push path cheaper than the one being measured.
        r.fill = replay::kLocalAll;
        r.echoComponent = true;
        return replay::kLocalAll;
    }
    size_t PlayerCount() { ++calls.asSeenPlayerCount; return pawns; }

    // The IAsSeen half, on the same object so one counter set covers the whole frame.
    template <class S>
    bool Read(S& s) {
        ++calls.asSeenSample;
        memset(&s, 0, sizeof s);
        s.peerTimeUs = 4242;
        // The one bit the tick actually reads (capture.cpp: t.asSeenValid & kAsSeenPeerTime),
        // named rather than an all-ones word that stands for nothing in particular.
        s.valid = replay::kAsSeenPeerTime;
        return true;
    }
    template <class P>
    bool ReadPlayer(replay::UserIndex, P& p) {
        ++calls.asSeenPlayer;
        memset(&p, 0, sizeof p);
        return true;
    }
};

// The module's own staging path, byte for byte: a header, one or two memcpys into a thread stage,
// then RingSet::Push. A sink that only counted would leave the ring out of a number that is
// supposed to be the tick's.
struct PushSink {
    replay::capture::RingSet* rings;
    uint32_t records = 0;
    uint32_t refusals = 0;
    uint8_t stage[64 * 1024];
};

bool Push(uint16_t kind, const void* a, uint32_t na, const void* b, uint32_t nb, void* ctx) {
    PushSink* s = static_cast<PushSink*>(ctx);
    struct Head { uint16_t kind; uint16_t bytes; };
    Head h = {kind, static_cast<uint16_t>(na + nb)};
    const uint32_t total = uint32_t(sizeof h) + na + nb;
    if (total > sizeof s->stage) return false;
    memcpy(s->stage, &h, sizeof h);
    memcpy(s->stage + sizeof h, a, na);
    if (nb) memcpy(s->stage + sizeof h + na, b, nb);
    if (!s->rings->Push(s->stage, total)) {
        ++s->refusals;
        return false;
    }
    ++s->records;
    return true;
}

}  // namespace w2a_perf


// ---------------------------------------------------------------------------------------------
// WHICH GATE refused a body read. ENG-W1E-LOCALBODY-3 put a 19-arm BodyRefusal on BodyReading
// because L2 #9 read `refused 1` every tick and five engine conditions produce that same 1. This
// is the counter that carries the arm out of the gather; the case below plants the SAME refusal at
// two DIFFERENT arms, so a histogram keyed on "a refusal happened" rather than on which gate
// refused passes the first reading and fails the second.
namespace w2a_refusedat {
using namespace replay::capture;

struct RefusingPool {
    size_t slots = 4;
    uint16_t refusingSlot = 2;
    replay::BodyRefusal arm = replay::BodyRefusal::NoPrimaryBody;

    bool Available() const { return true; }
    size_t PhysCount() const { return slots; }
    bool IsLocallyControlled(replay::PhysHandle h) const { return h.v == 0; }
    bool ReadBodyDetailed(replay::PhysHandle h, replay::BodyState& st,
                          replay::BodyReading& r) const {
        r = replay::BodyReading{};
        memset(&st, 0, sizeof st);
        if (h.v == refusingSlot) {
            r.source = replay::BodySource::None;
            r.refusedAt = arm;
            return false;
        }
        r.ciResolved = true;
        r.bodyResolved = true;
        r.source = h.v == 0 ? replay::BodySource::Local : replay::BodySource::History;
        return true;
    }
    // Derived from the pool bound for the reason the fixture above states: a round literal
    // here can collide with a value in the fact registry.
    uint32_t NetIdOf(replay::PhysHandle h) const {
        return uint32_t(kMaxBodies) + 1u + h.v;
    }
};

bool CountSink(uint16_t, const void*, uint32_t, const void*, uint32_t, void* ctx) {
    ++*static_cast<uint32_t*>(ctx);
    return true;
}

GatherReport WalkOnce(replay::BodyRefusal arm) {
    RefusingPool pool;
    pool.arm = arm;
    GatherConfig cfg{};
    cfg.streamEnabled[replay::kStreamBodySampled] = true;
    uint32_t pushed = 0;
    GatherReport rep;
    GatherBodies(&pool, cfg, 1, 0, &CountSink, &pushed, rep);
    return rep;
}

}  // namespace w2a_refusedat

// THE FIRST REFUSAL OF THE CAPTURE, kept whole. The histogram above carries WHICH arm refused; it
// does not carry what the gates saw, and a diag the next refusal overwrites is the LAST refusal
// wearing the first one's name. This pool refuses TWICE with DIFFERENT numbers, so a latch that
// assigns unconditionally reads out the second set and the case goes red.
namespace w2a_firstrefusal {
using namespace replay::capture;

struct TwoRefusalPool {
    uint16_t refuseFirst = 1;
    uint16_t refuseSecond = 3;

    bool Available() const { return true; }
    size_t PhysCount() const { return 4; }
    bool IsLocallyControlled(replay::PhysHandle) const { return false; }
    uint32_t NetIdOf(replay::PhysHandle h) const { return uint32_t(kMaxBodies) + 1u + h.v; }
    bool ReadBodyDetailed(replay::PhysHandle h, replay::BodyState& st,
                          replay::BodyReading& r) const {
        r = replay::BodyReading{};
        memset(&st, 0, sizeof st);
        if (h.v != refuseFirst && h.v != refuseSecond) {
            r.ciResolved = true;
            r.bodyResolved = true;
            r.source = replay::BodySource::History;
            return true;
        }
        const bool first = h.v == refuseFirst;
        r.source = replay::BodySource::None;
        r.refusedAt = first ? replay::BodyRefusal::NoPrimaryBody : replay::BodyRefusal::DeadBody;
        r.diag.filled = replay::kBodyDiagSlot | replay::kBodyDiagLiveCount |
                        replay::kBodyDiagBypass | replay::kBodyDiagCiEntity |
                        replay::kBodyDiagCiIndex | replay::kBodyDiagRevHandle;
        r.diag.slot = first ? 11 : 22;
        r.diag.liveCount = first ? 5 : 6;
        r.diag.bypass = first ? replay::BypassRead::Unreadable : replay::BypassRead::Set;
        r.diag.ciEntityId = first ? 7 : 8;
        r.diag.ciIndex = first ? 9 : 13;
        r.diag.revHandle = first ? 12 : 14;
        return false;
    }
};

}  // namespace w2a_firstrefusal

TEST_CASE("the first refused body read is kept whole, and a later one does not overwrite it") {
    using namespace replay::capture;
    w2a_firstrefusal::TwoRefusalPool pool;
    GatherConfig cfg{};
    cfg.streamEnabled[replay::kStreamBodySampled] = true;
    uint32_t pushed = 0;
    GatherReport rep;
    GatherBodies(&pool, cfg, 1, 0, &w2a_refusedat::CountSink, &pushed, rep);

    REQUIRE(rep.bodiesRefused == 2);
    REQUIRE(rep.firstRefusalSeen);
    // The FIRST refusal's values, verbatim -- not a property they share with the second.
    CHECK(rep.firstRefusal.refusedAt == replay::BodyRefusal::NoPrimaryBody);
    CHECK(rep.firstRefusal.source == replay::BodySource::None);
    CHECK(rep.firstRefusal.diag.slot == 11);
    CHECK(rep.firstRefusal.diag.liveCount == 5);
    CHECK(rep.firstRefusal.diag.bypass == replay::BypassRead::Unreadable);
    CHECK(rep.firstRefusal.diag.ciEntityId == 7);
    CHECK(rep.firstRefusal.diag.ciIndex == 9);
    CHECK(rep.firstRefusal.diag.revHandle == 12);

    // The fields the CAPTURE contract needs, read as `key=value` TOKENS rather than as one pinned
    // sentence. W1-E owns DescribeDiag's format and has grown it three times; pinning the whole line
    // made this suite red for changes that never touched a capture behaviour. Tokens are matched
    // WHOLE (`slot=11` never hits `slot=110`), so every field below is still asserted verbatim --
    // what is no longer asserted is which OTHER fields W1-E prints beside them.
    char text[replay::kBodyDiagTextMax];
    const std::string diag(replay::DescribeDiag(rep.firstRefusal.diag, text, sizeof text));
    CHECK(HasToken(diag, "bodyId=?"));   // never reached, and `?` is the reading that says so
    CHECK(HasToken(diag, "slot=11"));
    CHECK(HasToken(diag, "liveCount=5"));
    CHECK(HasToken(diag, "bypass=unreadable"));   // `clear` gates identically and diagnoses oppositely
    CHECK(HasToken(diag, "ent=7"));
    CHECK(HasToken(diag, "idx=9"));
    CHECK(HasToken(diag, "rev=12"));
    // The control for the matcher: values this diag does NOT carry must not be found, or every line
    // above would pass against any string at all.
    CHECK_FALSE(HasToken(diag, "slot=12"));
    CHECK_FALSE(HasToken(diag, "bodyId=11"));

    // A SECOND walk on the same report -- the next tick of the same capture -- refusing again.
    GatherBodies(&pool, cfg, 2, 1, &w2a_refusedat::CountSink, &pushed, rep);
    REQUIRE(rep.bodiesRefused == 4);
    CHECK(rep.firstRefusal.refusedAt == replay::BodyRefusal::NoPrimaryBody);
    CHECK(rep.firstRefusal.diag.slot == 11);
    CHECK(rep.firstRefusal.diag.liveCount == 5);
    CHECK(rep.firstRefusal.diag.ciIndex == 9);
    CHECK(rep.firstRefusal.diag.bypass == replay::BypassRead::Unreadable);
}

TEST_CASE("the body refusal histogram names the gate that refused") {
    using namespace w2a_refusedat;

    // THE CONTROL FOR THE WIDTH. kBodyRefusalCount is derived from the enum's last arm, so the one
    // way it can be wrong is an arm appended past TagMismatch -- and then this index is cased and
    // answers with a name instead of "unknown". Without this row the array could be a slot short
    // and every assertion below would still pass on the arms it does hold.
    CHECK(std::string(replay::BodyRefusalName(replay::BodyRefusal(kBodyRefusalCount))) == "unknown");
    CHECK(std::string(replay::BodyRefusalName(replay::BodyRefusal(kBodyRefusalCount - 1))) !=
          "unknown");

    const unsigned noPrimary = unsigned(replay::BodyRefusal::NoPrimaryBody);
    const unsigned tagMismatch = unsigned(replay::BodyRefusal::TagMismatch);
    REQUIRE(noPrimary != tagMismatch);

    const GatherReport a = WalkOnce(replay::BodyRefusal::NoPrimaryBody);
    CHECK(a.bodiesPresent == 4u);
    CHECK(a.bodiesRefused == 1u);
    CHECK(a.bodyRefusals[noPrimary] == 1u);
    CHECK(a.bodyRefusals[tagMismatch] == 0u);
    // The three that answered are on arm 0, and nothing landed anywhere else: a histogram that
    // counted the refusal twice, or counted a successful read as an arm, breaks here.
    CHECK(a.bodyRefusals[0] == 3u);
    CHECK(a.bodyRefusalUnknown == 0u);
    uint32_t sum = a.bodyRefusalUnknown;
    for (unsigned i = 0; i < kBodyRefusalCount; ++i) {
        sum += a.bodyRefusals[i];
        if (i != 0 && i != noPrimary) {
            CHECK(a.bodyRefusals[i] == 0u);
        }
    }
    CHECK(sum == a.bodiesPresent);

    // SAME pool, SAME single refusal, a DIFFERENT gate. Every count above is unchanged and only the
    // arm moves -- which is what makes this a test of the arm rather than of the refusal.
    const GatherReport b = WalkOnce(replay::BodyRefusal::TagMismatch);
    CHECK(b.bodiesRefused == 1u);
    CHECK(b.bodyRefusals[tagMismatch] == 1u);
    CHECK(b.bodyRefusals[noPrimary] == 0u);
    CHECK(b.bodyRefusals[0] == 3u);
    CHECK(b.bodyRefusalUnknown == 0u);
}



// ---------------------------------------------------------------------------------------------
// L3-RUN-16 F1. `gather` cost 950-1012 us a tick against a 200 us rung and killed the always-on
// capture 61 s in, with `roster` at 11.8 us and every other phase under 12 us. The case above
// already establishes that the capture-side gather code is NANOSECONDS, so the millisecond is the
// engine reads the gather makes -- and this is the shape that makes them a millisecond.
//
// `SpanProof::Prove` deliberately does not cache a REFUSAL (entity.h: "not mapped when I asked" is
// an answer about that instant), so every call over a span that is not there is a fresh
// replay::Readable -- a VirtualQuery, i.e. NtQueryVirtualMemory, which takes the target address
// space's lock and walks its VAD tree. GatherBodies walks the WHOLE physics pool every tick and
// asks the reader about every slot, so an empty or unreadable slot is a syscall per slot per tick,
// for the life of the binding, and it never gets cheaper.
//
// The claim here is a RATIO against a VirtualQuery measured in the same run on the same box, not an
// absolute microsecond figure: an idle test host cannot reproduce a contended VirtualQuery (the
// same reason ENG-STATSPERF-3 states its bound that way).
namespace w2a_refusalcost {

// A pool of `slots` bodies, none of which can be read, refusing THROUGH the span cache -- one
// SpanProof line per slot, exactly as entity.cpp keeps one per span it touches. The base is a page
// this fixture has already released, so Readable answers "no" every time and nothing is cached.
struct RefusingPool {
    // The pool bound the gather actually walks to, so a build that changes it changes this fixture
    // with it rather than measuring a pool the tick would never reach.
    static constexpr uint16_t kSlots = uint16_t(replay::capture::kMaxBodies);

    void*                     dead = nullptr;
    replay::entity::SpanProof line[kSlots];
    uint16_t                  slots = kSlots;
    uint64_t                  reads = 0;

    RefusingPool() {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        dead = VirtualAlloc(nullptr, si.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        REQUIRE(dead != nullptr);
        REQUIRE(VirtualFree(dead, 0, MEM_RELEASE) != 0);   // the address is now unmapped
    }

    bool   Available() const { return true; }
    size_t PhysCount() const { return slots; }
    bool   IsLocallyControlled(replay::PhysHandle) const { return false; }
    uint32_t NetIdOf(replay::PhysHandle) const { return 0; }

    template <class S>
    bool ReadBodyDetailed(replay::PhysHandle h, S& st, replay::BodyReading& r) {
        ++reads;
        memset(&st, 0, sizeof st);
        r = replay::BodyReading{};
        // The read a body path makes before it can touch an element: prove the span, then load.
        // It refuses; what the refusal costs is the finding.
        return line[h.v % kSlots].Prove(dead, sizeof(replay::BodyState)) != nullptr;
    }

};

bool Sink(uint16_t, const void*, uint32_t, const void*, uint32_t, void*) { return true; }

}  // namespace w2a_refusalcost

TEST_CASE("L3-RUN-16 F1: a body slot whose span refuses costs a syscall per slot per INTERVAL") {
    using namespace replay::capture;

    w2a_refusalcost::RefusingPool pool;
    GatherConfig                  cfg{};
    for (int i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;

    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    LARGE_INTEGER a{}, b{};

    const int kTicks = 200;
    GatherReport warm;
    GatherBodies(&pool, cfg, 1, 0, &w2a_refusalcost::Sink, nullptr, warm);
    // Every slot was asked and every one refused: the cost below is the cost of the whole walk and
    // not of a walk that stopped early.
    REQUIRE(warm.bodiesPresent == pool.slots);
    REQUIRE(warm.bodiesRefused == pool.slots);

    const uint64_t probes0 = replay::entity::SpanProbeCount();
    QueryPerformanceCounter(&a);
    for (int t = 0; t < kTicks; ++t) {
        GatherReport rep;
        GatherBodies(&pool, cfg, uint64_t(t) + 2, uint32_t(t), &w2a_refusalcost::Sink, nullptr, rep);
    }
    QueryPerformanceCounter(&b);
    const uint64_t probes = replay::entity::SpanProbeCount() - probes0;
    const double   perTickUs =
        double(b.QuadPart - a.QuadPart) * 1e6 / double(f.QuadPart) / double(kTicks);

    // The same-run control: the syscall the proof exists to remove, on this box under this load.
    MEMORY_BASIC_INFORMATION mbi{};
    size_t                   probed = 0;
    const int                kProbes = kTicks * int(pool.slots);
    QueryPerformanceCounter(&a);
    for (int i = 0; i < kProbes; ++i) probed += VirtualQuery(&mbi, &mbi, sizeof mbi);
    QueryPerformanceCounter(&b);
    REQUIRE(probed == size_t(kProbes) * sizeof mbi);   // the control really ran
    const double perProbeUs =
        double(b.QuadPart - a.QuadPart) * 1e6 / double(f.QuadPart) / double(kProbes);
    REQUIRE(perProbeUs > 0.0);

    MESSAGE("L3-RUN-16 F1 reproduction: " << pool.slots << " refusing slot(s), " << perTickUs
                                          << " us a tick, " << (double(probes) / double(kTicks))
                                          << " span probe(s) a tick; baseline " << perProbeUs
                                          << " us a VirtualQuery, so the walk is "
                                          << (perTickUs / perProbeUs) << " probes' worth");

    // This case was written when the answer was ONE PER SLOT PER TICK -- 64 syscalls a capture tick
    // for the life of the binding -- and it demanded a fix move that toward zero.
    // ENG-W1E-SPANPROOF-REFUSAL is that fix: a refusal is now remembered for
    // SpanProofReprobeTicks calls, so the same walk over the same lines costs one probe per slot
    // per INTERVAL. The bound is derived from the shipped cadence, and it is not a warm-up figure:
    // the loop above is the SECOND walk of this pool.
    const uint64_t reprobe = replay::entity::SpanProofReprobeTicks();
    REQUIRE(reprobe > 1u);
    CHECK(probes <= uint64_t(pool.slots) * ((uint64_t(kTicks) + reprobe - 1u) / reprobe));
    CHECK(probes >= uint64_t(pool.slots));   // and it still asks: a cadence, not a latch

    // The cost claim, as a ratio rather than a microsecond count. The walk itself is nanoseconds
    // (the case below measures it), so the tick is the syscalls and nothing else -- and it may no
    // longer be a slot's worth of them. The pre-fix reading of this same ratio was 46 probes'
    // worth a tick.
    if (TimingChecksEnabled()) {
        CHECK(perTickUs <= 12.0 * perProbeUs);
    }
}

TEST_CASE("the tick's capture-side phases are ns, and the engine-call count per tick is the cost") {
    using namespace replay::capture;

    w2a_perf::FakeWorld world;
    GatherConfig cfg{};
    for (int i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;

    std::vector<uint8_t> ringMem(4u * 1024u * 1024u);
    RingSet rings;
    REQUIRE(rings.Init(ringMem.data(), static_cast<uint32_t>(ringMem.size())));
    w2a_perf::PushSink sink{};
    sink.rings = &rings;
    std::vector<uint8_t> drain(64 * 1024);

    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    LARGE_INTEGER a{}, b{};

    const int kTicks = 2000;
    double bodiesTicks = 0, pawnsTicks = 0, asSeenTicks = 0, peerTicks = 0;

    for (int t = 0; t < kTicks; ++t) {
        const uint64_t us = 1000000ull + uint64_t(t) * 8333ull;
        GatherReport rep;

        uint64_t peerUs = 0;
        uint32_t peerValid = 0;
        replay::AsSeenSample sample{};
        QueryPerformanceCounter(&a);
        const bool haveSample = ReadPeerTime(&world, peerUs, peerValid, &sample);
        QueryPerformanceCounter(&b);
        peerTicks += double(b.QuadPart - a.QuadPart);

        QueryPerformanceCounter(&a);
        GatherBodies(&world, cfg, us, uint32_t(t), &w2a_perf::Push, &sink, rep);
        QueryPerformanceCounter(&b);
        bodiesTicks += double(b.QuadPart - a.QuadPart);

        QueryPerformanceCounter(&a);
        GatherLocalPawns(&world, cfg, us, uint32_t(t), &w2a_perf::Push, &sink, rep);
        QueryPerformanceCounter(&b);
        pawnsTicks += double(b.QuadPart - a.QuadPart);

        QueryPerformanceCounter(&a);
        GatherAsSeen(&world, &world, cfg, us, uint32_t(t), &w2a_perf::Push, &sink, rep,
                     haveSample ? &sample : nullptr);
        QueryPerformanceCounter(&b);
        asSeenTicks += double(b.QuadPart - a.QuadPart);

        // OUTSIDE every bracket: the worker drains in production and the tick is not charged for
        // it. Draining at all is what keeps the ring from filling and turning the last ticks of the
        // run into refusals, which would measure a shorter push path than production's.
        uint32_t got = 0;
        while (rings.Pop(drain.data(), static_cast<uint32_t>(drain.size()), &got) != 0) {
        }
    }

    const double scale = 1e9 / double(f.QuadPart) / double(kTicks);
    const double peerNs = peerTicks * scale;
    const double bodiesNs = bodiesTicks * scale;
    const double pawnsNs = pawnsTicks * scale;
    const double asSeenNs = asSeenTicks * scale;
    const double totalNs = peerNs + bodiesNs + pawnsNs + asSeenNs;

    // The engine-read floor, from this run's own probe rather than a constant carried in from
    // another report. The span is the one ENG-COST timed, so the two numbers are comparable.
    std::vector<uint8_t> span(0x8A80, 0);
    uint32_t hits = 0;
    QueryPerformanceCounter(&a);
    for (int i = 0; i < 20000; ++i) hits += replay::Readable(span.data(), span.size()) ? 1u : 0u;
    QueryPerformanceCounter(&b);
    const double probeNs = double(b.QuadPart - a.QuadPart) * 1e9 / double(f.QuadPart) / 20000.0;
    CHECK(hits == 20000u);

    const w2a_perf::EngineCalls& c = world.calls;
    const double perTick = double(c.Total()) / double(kTicks);

    MESSAGE("capture-side ns/tick at 8 players + disc: peer " << peerNs << ", bodies " << bodiesNs
            << ", pawns " << pawnsNs << ", asseen " << asSeenNs << "; TOTAL " << totalNs
            << " ns. Records pushed " << (double(sink.records) / double(kTicks)) << "/tick, "
            << sink.refusals << " refused");
    MESSAGE("engine accessor calls per tick: physCount " << (double(c.physCount) / kTicks)
            << ", bodyPresence " << (double(c.bodyPresence) / kTicks)
            << ", bodyRead " << (double(c.bodyRead) / kTicks)
            << ", bodyNetId " << (double(c.bodyNetId) / kTicks)
            << ", pawnCount " << (double(c.pawnCount) / kTicks)
            << ", pawnAt " << (double(c.pawnAt) / kTicks)
            << ", pawnRead " << (double(c.pawnRead) / kTicks)
            << ", asSeenSample " << (double(c.asSeenSample) / kTicks)
            << ", asSeenPlayerCount " << (double(c.asSeenPlayerCount) / kTicks)
            << ", asSeenPlayer " << (double(c.asSeenPlayer) / kTicks) << "; TOTAL " << perTick
            << "/tick. One replay::Readable over the CR15Game span costs " << probeNs
            << " ns here, so an accessor that proves its span costs this tick "
            << (perTick * probeNs / 1000.0)
            << " us UNCONTENDED -- the field figure is that same count under echovr.exe's "
               "address-space churn");

    // ---- what this case ASSERTS ---------------------------------------------------------------
    // 1. The capture-side composition is not the 160 us. The bound is a tenth of the 30 us/tick
    //    target, so the target can be neither met nor missed by this half alone.
    CHECK(totalNs < 3000.0);
    // 2. The record mix is production's: two frame sets, nine bodies, eight pawns and one as-seen
    //    frame, 20 records a tick. A fixture pushing fewer would measure a cheaper tick than the
    //    one the kill line reads.
    CHECK(sink.records == 20u * uint32_t(kTicks));
    CHECK(sink.refusals == 0u);
    // 3. * THE ENGINE-CALL BOUND, which is the number a fix has to move and the reason this case
    //    exists. The pawn side is O(present): eight pawns cost eight reads. The BODY side is
    //    O(POOL): gather.h asks every one of kMaxBodies slots whether this machine authors it, so
    //    nine live bodies cost 64 presence probes. 64 probes for 9 answers is where the tick's
    //    engine traffic is, and at the probe cost printed above it accounts for L2-RUN-3's
    //    `gather` on its own. Nothing on the capture side of the bracket can move it.
    CHECK(c.bodyPresence == uint32_t(kMaxBodies) * uint32_t(kTicks));
    CHECK(c.pawnRead == 8u * uint32_t(kTicks));
    CHECK(c.bodyPresence > c.pawnRead * 4u);
    // 4. The as-seen frame is fetched ONCE per tick. ReadPeerTime and GatherAsSeen both want it and
    //    both used to ask, which spent an engine round trip a tick re-fetching a struct the tick
    //    already held. Removing the hand-forward puts this back to 2 x kTicks.
    CHECK(c.asSeenSample == uint32_t(kTicks));
}

// The kill predicate is NOT raised (ENG-COST section 3 ruled the constant stays). This is the
// must-fail control for that ruling: a synthetic charged phase of 250 us -- above the 200 us line,
// below the 4 x kP999HardFailUs the module's own injection uses, and inside the range L2-RUN-3
// actually measured -- must still reach Rung::Killed. A raise of the constant past 250, or a
// predicate that stopped reading the charged median, turns this red.
TEST_CASE("a 250 us charged phase still kills, and the kill line is still 200 us") {
    // The constant itself, asserted rather than assumed: every other assertion here is relative to
    // it, so a raise would otherwise slide the whole case up with it and stay green.
    CHECK(replay::capture::CostMeter::kKillMedianUs == 200u);
    REQUIRE(250u > replay::capture::CostMeter::kKillMedianUs);

    replay::capture::CostMeter m;
    m.Init();
    replay::capture::CostMeter::Rung r = replay::capture::CostMeter::Rung::Nominal;
    for (uint32_t w = 0; w < replay::capture::CostMeter::kKillWindows; ++w) {
        for (uint32_t i = 0; i < replay::capture::CostMeter::kWindowTicks; ++i) {
            m.AddSampleUs(250);
        }
        m.Evaluate(r);
    }
    const replay::capture::CostMeter::WindowStat st = m.LastWindow();
    MESSAGE("250 us charged samples: median in (" << st.medianLoUs << ", " << st.medianUs
            << "] us, overKillWindows " << st.overKillWindows << ", rung "
            << unsigned(static_cast<uint8_t>(m.CurrentRung())));
    CHECK(m.CurrentRung() == replay::capture::CostMeter::Rung::Killed);
    // The lower bound of the median bucket is the only provable statement about where the samples
    // were, and it is the one the predicate reads.
    CHECK(st.medianLoUs >= replay::capture::CostMeter::kKillMedianUs);

    // ANTI-VACUITY, on the same meter shape: a phase at half that budget must NOT kill, so the case
    // above is measuring the threshold and not the ladder's willingness to fire at anything.
    replay::capture::CostMeter cheap;
    cheap.Init();
    replay::capture::CostMeter::Rung r2 = replay::capture::CostMeter::Rung::Nominal;
    for (uint32_t w = 0; w < replay::capture::CostMeter::kKillWindows + 2u; ++w) {
        for (uint32_t i = 0; i < replay::capture::CostMeter::kWindowTicks; ++i) {
            cheap.AddSampleUs(100);
        }
        cheap.Evaluate(r2);
    }
    CHECK(cheap.CurrentRung() != replay::capture::CostMeter::Rung::Killed);
}

// ---------------------------------------------------------------------------------------------
// A record the tick pushed and the ring refused is a loss on a retention=ALL
// stream, and the file has to name it. That works only if the kind is inside the per-kind
// counter's bound AND maps to the stream that actually lost it. kRecGameState (18) and
// kRecPlayerStats (19) are numbered above kRecWorldOrigin (17), which is where both stopped: the
// producer had no slot to count the drop in, the worker had no iteration to file it from, and
// StreamOfKind's default arm would have charged it to S_TICK even if it had.
// ---------------------------------------------------------------------------------------------
namespace replay {
namespace capture {
StreamId StreamOfKindForTest(uint16_t kind);
uint32_t PushFailKindSlotsForTest();
}  // namespace capture
}  // namespace replay

TEST_CASE("every record kind a ring drop can lose has a slot and its own stream") {
    using namespace replay;
    using namespace replay::capture;
    // The bound is taken from the last enumerator, so a kind cannot be appended past it.
    CHECK(kRecKindMax == kRecPlayerStats);
    // The producer's counter must have a slot for every kind, or the loss is not even counted.
    CHECK(PushFailKindSlotsForTest() > kRecKindMax);

    // The two the tick samples itself. Charging either to S_TICK would put a real loss on a stream
    // that did not suffer it; leaving them outside the bound charges it to nothing at all.
    CHECK(StreamOfKindForTest(kRecGameState) == kStreamGameState);
    CHECK(StreamOfKindForTest(kRecPlayerStats) == kStreamPlayerStats);

    // Discriminating: the kinds whose attribution was already right must not have moved.
    CHECK(StreamOfKindForTest(kRecMark) == kStreamMark);
    CHECK(StreamOfKindForTest(kRecWorldOrigin) == kStreamKey);
    CHECK(StreamOfKindForTest(kRecWirePlayer) == kStreamPlayerWire);
}

// ★ AFTER the in-game case above, and it reads the counters that case left behind: the module is
// shut down by then, so this is the audit of what the ticks did rather than another driver.
//
// SC4 / the pin block's cost rule: the pins are gathered at a fraction of the KEYFRAME interval and
// never per tick. Only a keyframe ever reads them, and the walk plus its encode is 27 KB of work --
// paying it ninety times a second for a value read twice a minute is the whole cost defect this
// bound exists to catch. The case above drove 300 engine ticks over roughly three seconds of source
// time at a 2 s keyframe interval, so a correct capture samples a handful of times; a per-tick
// sampler would show hundreds.
TEST_CASE("the audio pins are gathered at keyframe cadence and never once per tick") {
    const uint32_t ticks = TheEnv().frame;
    MESSAGE("audio pin samples: " << replay::audio::g_pinEncodeCalls << " over " << ticks
                                  << " ticks");
    CHECK(replay::audio::g_pinEncodeCalls >= 1);   // it is called at all, on the shipped path
    // The bound is taken from the tick count this file actually drove rather than from a number
    // somebody has to trust: a per-tick sampler scores one per OPEN tick, so anything at a quarter
    // of the tick rate or above is the defect this case exists for.
    CHECK(replay::audio::g_pinEncodeCalls * 4 <= ticks);
}

// THE UNBOUND ARM. Every case in this file binds a stub pair before the one Init the process gets, so
// the branch the SHIPPED build takes -- W2-I has not promoted the producer prototypes, so there is no
// pair to bind -- is never entered here, and until now nothing could see it drop. Mutant: delete the
// _snprintf_s inside FormatOracleNotBound and this goes red.
TEST_CASE("the shipped producers are absent, so the tap names them in a NotBound sentence") {
    char why[512] = {};
    const char* missing = replay::capture::OracleShippedNotBoundForTest(why, sizeof why);
    // If this ever comes back null, W2-I promoted the call shapes and this case is the one to rewrite.
    REQUIRE(missing != nullptr);
    MESSAGE("oracle notBound: " << why);
    const std::string sentence(why);
    CHECK(sentence.find("NotBound(") == 0);
    // The names come from OracleBuilders(), not from a copy in this file, so a site table W2-I re-spells
    // moves both sides together.
    CHECK(sentence.find(missing) != std::string::npos);
    CHECK(sentence.find("V-I-W2I1") != std::string::npos);
    CHECK(sentence.size() < sizeof(why) - 1);   // the sentence is not truncated into its own buffer
    // AUD-FINAL R8c. EVERY row is named, and named with the TIER it was refused at, so the sentence
    // says which producer is missing what rather than that something was missing. The row names are
    // read from the registry table, not spelled here.
    for (const replay::capture::OracleRegistryRow& r : replay::capture::kOracleRegistry) {
        CHECK(sentence.find(std::string(r.factId) + ":PROTO-UNPROMOTED") != std::string::npos);
    }
    // ...and the last row survives the format: a refusal truncated into its buffer under-names the
    // producers it exists to name.
    CHECK(sentence.find("the stream is not empty") != std::string::npos);

    // The control: this process DID bind a pair, so the word Init actually composed -- the one the tap
    // line, the stop line and the close document all read -- is empty. A non-empty word here would mean
    // the bound path also writes a NotBound sentence, and the three readers would all be lying.
    char live[288] = {};
    replay::capture::OracleBindWhyForTest(live, sizeof live);
    CHECK(live[0] == 0);
}

// AUD-FINAL R8c: the shipped decision is the REGISTRY'S. Before this, the only thing that could
// satisfy it was a builder pointer a test had installed, so "bound" was a state nothing outside
// tests/ could construct and the refusal said nothing about which producer or why. These four drive
// the SHIPPED function -- OracleBuilders through OracleDecideForTest -- over four registries.
TEST_CASE("the shipped decision binds a promoted registry and refuses an unpromoted one by tier") {
    char why[512] = {};
    bool bound = false;

    // 1. The real registry: three rows, no promoted call shape, so nothing is called.
    const char* missing = replay::capture::OracleDecideForTest(nullptr, why, sizeof why, bound);
    REQUIRE(missing != nullptr);
    CHECK_FALSE(bound);

    // 2. The same three rows with their call shapes promoted -- the merge V-I-W2I1 will make -- and
    // nothing else changed. The gate opens on the REGISTRY, not on a seam.
    bound = false;
    CHECK(replay::capture::OracleDecideForTest(&w2a_oracle::g_promotedView, why, sizeof why, bound) ==
          nullptr);
    CHECK(bound);
    CHECK(why[0] == 0);   // a bound decision composes no refusal for the three readers to print

    // 3. Half promoted: the /session arms unpromoted, bones promoted. Binding one arm is not half a
    // sample, so this refuses -- naming the two arms and NOT the row that answered.
    replay::capture::OracleRegistryRow half[3] = {replay::capture::kOracleRegistry[0],
                                                  replay::capture::kOracleRegistry[1],
                                                  w2a_oracle::g_promotedRows[2]};
    const replay::capture::OracleRegistryView halfView{half, 3, &w2a_oracle::PromotedResolve,
                                                       &w2a_oracle::g_sessionCalls,
                                                       &w2a_oracle::g_bonesCalls};
    bound = false;
    missing = replay::capture::OracleDecideForTest(&halfView, why, sizeof why, bound);
    REQUIRE(missing != nullptr);
    CHECK_FALSE(bound);
    const std::string halfWord(missing);
    CHECK(halfWord.find("ORACLE.SESSION_BUILDER:PROTO-UNPROMOTED") != std::string::npos);
    CHECK(halfWord.find("ORACLE.SESSION_BUILDER_ALT:PROTO-UNPROMOTED") != std::string::npos);
    CHECK(halfWord.find("ORACLE.BONES_BUILDER") == std::string::npos);

    // 4. A promoted row the registry files as UNVERIFIED is still not callable, and the id comes from
    // the registry's own list rather than from a copy of it here. Tier U is a refusal even though the
    // call shape is promoted and the resolver would have answered.
    REQUIRE(facts::UNVERIFIED_FACT_COUNT > 0);
    replay::capture::OracleRegistryRow unverified[3] = {w2a_oracle::g_promotedRows[0],
                                                        w2a_oracle::g_promotedRows[1],
                                                        w2a_oracle::g_promotedRows[2]};
    unverified[2].factId = facts::UNVERIFIED_FACT_IDS[0];
    const replay::capture::OracleRegistryView unverifiedView{unverified, 3,
                                                             &w2a_oracle::PromotedResolve,
                                                             &w2a_oracle::g_sessionCalls,
                                                             &w2a_oracle::g_bonesCalls};
    bound = false;
    missing = replay::capture::OracleDecideForTest(&unverifiedView, why, sizeof why, bound);
    REQUIRE(missing != nullptr);
    CHECK_FALSE(bound);
    CHECK(std::string(missing) == std::string(facts::UNVERIFIED_FACT_IDS[0]) + ":U");

    // The live word Init composed is untouched by any of this: these ran on a saved-and-restored view.
    char live[512] = {};
    replay::capture::OracleBindWhyForTest(live, sizeof live);
    CHECK(live[0] == 0);
}

// S5.4.7 / ruling Q-B: the ORACLE stream ships in every capture. This reads it BACK out of the files
// the cases above drove through the real tick -> ring -> worker -> chapterbuilder path, with the
// shipped reader. Two mutants are what it exists for: remove BindBuilders from Init and every record
// comes back NotBound with no payload; remove the SampleOracleTick phase and there are no records.
TEST_CASE("the shipped tick writes ORACLE records and the reader gets both payloads back") {
    uint64_t samples = 0, ok = 0;
    replay::capture::OracleCountsForTest(samples, ok);
    // Cadence, not presence: one per CORE chapter over the ticks this file drove. The env's tick is
    // 11111 us, so a per-tick sampler would score hundreds and a correct one a handful.
    const uint32_t ticks = TheEnv().frame;
    const uint64_t perSample = 2000000ull / 11111ull;
    MESSAGE("oracle samples: " << samples << " ok: " << ok << " over " << ticks << " ticks");
    CHECK(samples >= 1);
    CHECK(ok == samples);   // the stubs are bound, so every sample is a real pair
    CHECK(samples * perSample <= ticks + perSample);
    CHECK(w2a_oracle::g_sessionCalls == samples);
    CHECK(w2a_oracle::g_bonesCalls == samples);

    const std::string glob = ScratchDir() + "\\*.echodemo";
    WIN32_FIND_DATAA fd{};
    HANDLE h = ::FindFirstFileA(glob.c_str(), &fd);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    uint32_t blocks = 0, stored = 0, decoded = 0, observed = 0;
    do {
        const std::string narrow = ScratchDir() + "\\" + fd.cFileName;
        wchar_t wide[MAX_PATH] = {};
        ::MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), -1, wide, MAX_PATH);
        replay::DemoReader r;
        if (!r.Open(wide)) continue;
        for (size_t i = 0; i < r.ChapterCount(); ++i) {
            replay::DecodedChapter c{};
            if (!r.DecodeChapter(i, c)) continue;
            for (uint16_t b = 0; b < c.streamCount; ++b) {
                const replay::StreamView& v = c.streams[b];
                if (v.id != replay::kStreamOracle) continue;
                ++blocks;
                stored += v.storedCount;
                observed += v.observedCount;
                // The block body is BIT-PACKED, not an array of structs: chapterbuilder pushes each
                // ORACLE record through EncodeRawRecord, which is a u16 length then the bytes, and
                // `bytes` is the packed extent. Walking it as plain memory reads a length field out
                // of the middle of the first record and decodes nothing -- which is what it did.
                if (v.bytes < replay::capture::kBlockPrefixBytes) continue;
                bitio::BitReader br(v.records + replay::capture::kBlockPrefixBytes,
                                    v.bytes - replay::capture::kBlockPrefixBytes);
                for (uint32_t k = 0; k < v.recordCount; ++k) {
                    uint8_t raw[4200] = {};
                    uint16_t n = 0;
                    REQUIRE(replay::capture::DecodeRawRecord(br, raw, sizeof raw, n));
                    REQUIRE(n >= uint16_t(sizeof(replay::capture::OracleRec)));
                    replay::capture::OracleRec rec{};
                    memcpy(&rec, raw, sizeof rec);
                    REQUIRE(n == uint16_t(sizeof rec) + rec.sessionBytes + rec.bonesBytes);
                    CHECK(rec.status == uint8_t(replay::TapStatus::Ok));
                    CHECK(rec.sourceTimeUs != 0);
                    REQUIRE(rec.sessionBytes == uint16_t(sizeof(w2a_oracle::kSessionDoc) - 1));
                    REQUIRE(rec.bonesBytes == uint16_t(sizeof(w2a_oracle::kBonesDoc) - 1));
                    const uint8_t* payload = raw + sizeof rec;
                    const bool sessionMatches =
                        memcmp(payload, w2a_oracle::kSessionDoc, rec.sessionBytes) == 0;
                    const bool bonesMatches =
                        memcmp(payload + rec.sessionBytes, w2a_oracle::kBonesDoc,
                               rec.bonesBytes) == 0;
                    CHECK(sessionMatches);
                    CHECK(bonesMatches);
                    ++decoded;
                }
            }
        }
    } while (::FindNextFileA(h, &fd));
    ::FindClose(h);
    MESSAGE("S_ORACLE blocks: " << blocks << " stored: " << stored << " decoded: " << decoded
                                << " observed: " << observed);
    CHECK(blocks >= 1);
    CHECK(stored >= 1);
    CHECK(decoded == stored);
    // F40. This stream emits once per CORE chapter, so every record it decided to emit reached the
    // file and `observed` is `stored`. While the stream was tagged FixedCadence the accounting
    // interpolated the frames between samples and published ~60 lost records per sample.
    CHECK(replay::CadenceOf(replay::kStreamOracle) == replay::StreamCadence::OnChange);
    CHECK(replay::capture::ObservedSourceFitsCadence(replay::kStreamOracle,
                                                     replay::capture::ObservedSource::SamplerDecision));
    CHECK_FALSE(replay::capture::ObservedSourceFitsCadence(
        replay::kStreamOracle, replay::capture::ObservedSource::EngineFrameIndex));
    CHECK(observed == stored);
}

// ---------------------------------------------------------------------------------------------
// WHICH ARM answered angular velocity, per body. AngVelSource is the engine's own statement that
// a live CPhBody stores no omega anywhere and that a pose-only ncaTransform read has no cloud to
// derive one from; both reach the file as three zero-ish floats, so a capture of measured spin and
// one of structural zeros were the same bytes to every reader. This plants a DIFFERENT source on
// each slot, so a histogram keyed on "a body was read" rather than on which arm answered cannot
// pass it.
namespace w2a_angvelby {
using namespace replay::capture;

struct SpinPool {
    bool Available() const { return true; }
    size_t PhysCount() const { return 4; }
    bool IsLocallyControlled(replay::PhysHandle h) const { return h.v == 0; }
    bool ReadBodyDetailed(replay::PhysHandle h, replay::BodyState& st,
                          replay::BodyReading& r) const {
        r = replay::BodyReading{};
        memset(&st, 0, sizeof st);
        if (h.v == 3) {
            r.source = replay::BodySource::None;
            r.refusedAt = replay::BodyRefusal::NoCi;
            return false;
        }
        r.ciResolved = true;
        r.bodyResolved = true;
        r.source = h.v == 1 ? replay::BodySource::History : replay::BodySource::Local;
        r.diag.angVel = h.v == 0   ? replay::AngVelSource::Unavailable
                        : h.v == 1 ? replay::AngVelSource::NodeStored
                                   : replay::AngVelSource::EngineComputed;
        return true;
    }
    uint32_t NetIdOf(replay::PhysHandle h) const { return uint32_t(kMaxBodies) + 1u + h.v; }
};

bool CountSink(uint16_t, const void*, uint32_t, const void*, uint32_t, void* ctx) {
    ++*static_cast<uint32_t*>(ctx);
    return true;
}

}  // namespace w2a_angvelby

TEST_CASE("the gather counts WHICH ARM answered angular velocity, per body") {
    using namespace replay::capture;
    w2a_angvelby::SpinPool pool;
    GatherConfig cfg{};
    cfg.streamEnabled[replay::kStreamBodySampled] = true;
    uint32_t pushed = 0;
    GatherReport rep;
    GatherBodies(&pool, cfg, 1, 0, &w2a_angvelby::CountSink, &pushed, rep);

    CHECK(rep.angVelBy[unsigned(replay::AngVelSource::Unavailable)] == 1u);
    CHECK(rep.angVelBy[unsigned(replay::AngVelSource::NodeStored)] == 1u);
    CHECK(rep.angVelBy[unsigned(replay::AngVelSource::EngineComputed)] == 1u);
    CHECK(rep.angVelBy[unsigned(replay::AngVelSource::NotStoredByEngine)] == 0u);
    // The refused slot reached no arm at all, and must not be counted as one that did.
    CHECK(rep.angVelBy[unsigned(replay::AngVelSource::NotAsked)] == 1u);
    CHECK(rep.angVelUnknown == 0u);

    uint32_t sum = 0;
    for (unsigned i = 0; i < kAngVelSourceCount; ++i) {
        sum += rep.angVelBy[i];
    }
    CHECK(sum == rep.bodiesPresent);
    // The append control, exactly kBodyRefusalCount's: a source added past Unavailable without
    // moving the count would be counted by nothing, and only this line notices.
    CHECK(std::string(replay::AngVelSourceName(
              static_cast<replay::AngVelSource>(kAngVelSourceCount))) == "unknown");
}
