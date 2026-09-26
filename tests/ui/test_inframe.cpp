// W2-F. The row-2.5 in-frame draw site (ARCH-15), and the counters that replace ownerDraws.
//
// ★ WHAT EVERY CASE HERE IS GUARDING AGAINST. Before ARCH-15 the panel was drawn from the stage-0
// owner tick with the engine's GUI frame SHUT, so NRadGui::Begin refused it and ownerDraws counted
// refusals -- and no test in this directory could tell the difference, because tests/ui/fake_gui.cpp
// returned true from Begin unconditionally (AUDIT_F3J F3J-5: mutated to false, 19 of 76 cases
// reddened and not one draw-counter case moved). So the fixture here binds the fake facade to THE
// SAME BYTE the production precondition reads. A test that drove the two independently could pass
// with the product reading one byte and the panel drawing on another.
//
// The four mutants this file is written to catch, all four confirmed by hand (see ENG-W2F-C1.md):
//   1. draw without checking the frame byte     -> "a shut frame is a REFUSAL" reds
//   2. count a refused draw as inFrameDraws     -> the same case reds on the counter, not the flag
//   3. install without the prologue guard       -> "a site whose bytes differ" reds
//   4. fake Begin returns true unconditionally  -> "Begin's own refusal" reds

#include "doctest.h"

#include <windows.h>

#include <stdint.h>
#include <string.h>

#include <ratio>
#include <vector>

#include "bootstrap/buildguard.h"
#include "bootstrap/config.h"
#include "bootstrap/hookmgr.h"
#include "fake_gui.h"
#include "generated/facts_generated.h"
#include "ui/csregister.h"
#include "ui/inframe.h"

using namespace replay;

namespace {

// ---- the three engine globals, stood up in our own storage ------------------------------------

// The frame byte is shared with the fake facade on purpose (see the file header). The context is a
// pointer-sized slot because that is what GUI.CONTEXT_PTR is.
struct EngineGlobals {
    uint8_t frameOpenByte = 0;
    uint8_t startFrameLatch = 0;
    const void* contextPtr = nullptr;

    void Bind() {
        ui::inframe::SetProbeForTest(&frameOpenByte, &startFrameLatch, &contextPtr);
        uifake::SetFrameByte(&frameOpenByte);
    }
    void OpenFrame() {
        frameOpenByte = 1;
        startFrameLatch = 1;
        contextPtr = this;
    }
    void ShutFrame() { frameOpenByte = 0; }
};

// ---- a stand-in panel, so a counter case does not depend on panel.cpp's own state ---------------

int g_drawCalls = 0;
ui::inframe::DrawOutcome g_nextOutcome = ui::inframe::DrawOutcome::Drawn;

// Answers exactly as panel::Draw does: it asks the facade and forwards Begin's own verdict. The
// forwarding is the point -- a draw function that ignored Begin would report a refusal as a draw,
// which is AUDIT_F3J F3J-4.
ui::inframe::DrawOutcome FakePanelDraw() {
    ++g_drawCalls;
    if (g_nextOutcome == ui::inframe::DrawOutcome::NotShowing) {
        return ui::inframe::DrawOutcome::NotShowing;
    }
    bool open = true;
    const bool shown = ui::gui::Begin("Echo Replay", &open, 0);
    ui::gui::End();
    return shown ? ui::inframe::DrawOutcome::Drawn : ui::inframe::DrawOutcome::FacadeRefused;
}

// ---- the engine's HeartbeatBefore, faked -------------------------------------------------------

EngineGlobals* g_globals = nullptr;
int g_originalCalls = 0;

// The real one runs StartFrame five instructions in, so by the time it returns the frame byte is
// set. Our detour draws AFTER this returns, which is the whole reason the site was chosen.
uint64_t __fastcall HeartbeatOpensTheFrame(void* engine) {
    ++g_originalCalls;
    if (g_globals != nullptr) g_globals->OpenFrame();
    return reinterpret_cast<uint64_t>(engine);
}

// The site entered with the globals left exactly as the case set them.
uint64_t __fastcall HeartbeatNoop(void*) { ++g_originalCalls; return 0; }

uint64_t __fastcall HeartbeatLeavesFrameShut(void* engine) {
    ++g_originalCalls;
    if (g_globals != nullptr) g_globals->ShutFrame();
    return reinterpret_cast<uint64_t>(engine);
}

struct Fixture {
    EngineGlobals globals;

    Fixture() {
        ui::inframe::ResetForTest();
        ui::csregister::ResetForTest();
        uifake::SetReady(true);
        uifake::SetWindowGate(true);
        for (size_t i = 0; i < static_cast<size_t>(ui::gui::Entry::Count); ++i) {
            uifake::SetHave(static_cast<ui::gui::Entry>(i), true);
        }
        uifake::Reset();
        g_drawCalls = 0;
        g_originalCalls = 0;
        g_nextOutcome = ui::inframe::DrawOutcome::Drawn;
        g_globals = &globals;
        globals.Bind();
        ui::inframe::SetDrawFn(&FakePanelDraw);
    }

    ~Fixture() {
        g_globals = nullptr;
        uifake::SetFrameByte(nullptr);
        ui::inframe::SetProbeForTest(nullptr, nullptr, nullptr);
        ui::inframe::ResetForTest();
        ui::csregister::ResetForTest();
    }

    // One engine frame through the row-2.5 site, exactly as the armed detour performs it.
    void Frame(ui::inframe::OriginalFn original = &HeartbeatOpensTheFrame) {
        ui::inframe::CallSiteForTest(original, this);
    }

    const ui::inframe::Counters& C() const { return ui::inframe::Read(); }

    // The published invariant of src/ui/inframe.h. Every case asserts it, because a record where it
    // does not hold is an instrument fault and none of the other numbers can be believed.
    void CheckInvariant() const {
        const ui::inframe::Counters& c = C();
        CHECK(c.frameSiteEntries + c.fallbackEntries ==
              c.inFrameDraws + c.refusedDraws + c.panelClosedEntries);
    }
};

// ---- the install ladder's victim ---------------------------------------------------------------

int g_sink = 0;

__declspec(noinline) int InFrameVictim(int x) {
    g_sink = x;
    return x * 5 + 2;
}

uint64_t SyntheticPcVa(const void* address) {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(::GetModuleHandleW(nullptr));
    return facts::PE_IMAGE_BASE +
           static_cast<uint64_t>(reinterpret_cast<const uint8_t*>(address) - base);
}

std::vector<uint8_t> BytesAt(const void* address, size_t n) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(address);
    return std::vector<uint8_t>(p, p + n);
}

constexpr size_t kPrologueLen = sizeof facts::GUI_ENGINE_HEARTBEAT_BEFORE_PROLOGUE;

}  // namespace

// =================================================================================================
// A -- the precondition. Eight combinations, and the seven that must refuse.
// =================================================================================================

TEST_CASE("the precondition is three terms and every one of them can veto") {
    Fixture f;

    for (int bits = 0; bits < 8; ++bits) {
        ui::inframe::ResetForTest();
        f.globals.Bind();
        ui::inframe::SetDrawFn(&FakePanelDraw);
        g_drawCalls = 0;

        f.globals.frameOpenByte = (bits & 1) ? 1 : 0;
        f.globals.startFrameLatch = (bits & 2) ? 1 : 0;
        f.globals.contextPtr = (bits & 4) ? &f : nullptr;

        const ui::inframe::FramePrecondition p = ui::inframe::Sample();
        CHECK(p.frameOpenByte == ((bits & 1) != 0));
        CHECK(p.startFrameLatch == ((bits & 2) != 0));
        CHECK(p.contextPtr == ((bits & 4) != 0));
        CHECK(p.Ok() == (bits == 7));

        // The site itself, driven with those globals already in place, so the counters are scored
        // on the same eight states rather than on a summary of them.
        ui::inframe::CallSiteForTest(&HeartbeatNoop, &f);

        const ui::inframe::Counters& c = ui::inframe::Read();
        CHECK(c.frameSiteEntries == 1);
        if (bits == 7) {
            CHECK(c.inFrameDraws == 1);
            CHECK(c.refusedDraws == 0);
            CHECK(g_drawCalls == 1);
        } else {
            CHECK(c.inFrameDraws == 0);
            CHECK(c.refusedDraws == 1);
            CHECK(g_drawCalls == 0);   // refused BEFORE the facade was touched
            // The reason is recorded, not guessed, and every failing term is credited.
            CHECK(c.refusedNoFrameByte == ((bits & 1) ? 0u : 1u));
            CHECK(c.refusedNoLatch == ((bits & 2) ? 0u : 1u));
            CHECK(c.refusedNoContext == ((bits & 4) ? 0u : 1u));
        }
        f.CheckInvariant();
    }
}

// =================================================================================================
// B -- the site. One draw per frame, and a shut frame is a refusal rather than a silence.
// =================================================================================================

TEST_CASE("a HeartbeatBefore that opens the frame gets exactly one draw per frame") {
    Fixture f;

    for (int frame = 0; frame < 5; ++frame) f.Frame();

    CHECK(g_originalCalls == 5);      // the trampoline ran on every entry
    CHECK(f.C().frameSiteEntries == 5);
    CHECK(f.C().inFrameDraws == 5);
    CHECK(f.C().refusedDraws == 0);
    CHECK(g_drawCalls == 5);          // exactly one, never two, never none
    CHECK(uifake::BeginCount() == 5);
    CHECK(uifake::EndCount() == 5);   // ARCH-15 S2.2 risk 2: balanced on every return path
    f.CheckInvariant();
}

TEST_CASE("a shut frame is a REFUSAL with a named reason, never a silent no-op") {
    Fixture f;

    for (int frame = 0; frame < 3; ++frame) f.Frame(&HeartbeatLeavesFrameShut);

    CHECK(f.C().frameSiteEntries == 3);
    CHECK(f.C().inFrameDraws == 0);
    CHECK(f.C().refusedDraws == 3);
    CHECK(f.C().refusedNoFrameByte == 3);
    CHECK(g_drawCalls == 0);
    // ★ MUTANT 1. Remove the Sample()/Ok() test from inframe.cpp's Attempt and the draw runs on a
    // shut frame: refusedDraws falls to 0 and inFrameDraws rises to 3. Both halves red.
    // ★ MUTANT 2. Credit the refusal to inFrameDraws instead and the same two lines red.
    f.CheckInvariant();
}

TEST_CASE("refusedDraws is published at zero, so its absence can mean something else") {
    Fixture f;
    // Never entered at all. Zero is "asked, none"; a field that only appeared when non-zero could
    // not be told from "not asked", which is the four-state distinction ARCH-15 S4.5 turns on.
    CHECK(f.C().frameSiteEntries == 0);
    CHECK(f.C().refusedDraws == 0);
    CHECK(f.C().inFrameDraws == 0);

    const char* line = ui::csregister::Diagnostic();
    CHECK(strstr(line, "[inframe entries=0 draws=0 refused=0 ") != nullptr);
    CHECK(strstr(line, "installed=0]") != nullptr);
    // The threads group stays LAST so the observer's tail anchor survives the new group.
    CHECK(strstr(line, "[threads owner=") != nullptr);
    CHECK(strstr(line, "] [threads owner=") != nullptr);
}

TEST_CASE("Begin's own refusal is a refused draw, not a draw (F3J-4)") {
    Fixture f;

    // The three loads all say yes and the facade still says no. That is the case a precondition
    // check alone cannot see, and the case a caller that discards Begin's answer counts as a draw.
    f.globals.OpenFrame();

    // The facade is pointed at a byte of its own that is shut, while the precondition still reads
    // the engine globals that say the frame is open. Only Begin can catch this.
    static uint8_t facadeShut = 0;
    uifake::SetFrameByte(&facadeShut);

    ui::inframe::CallSiteForTest(&HeartbeatNoop, &f);

    CHECK(f.C().frameSiteEntries == 1);
    CHECK(g_drawCalls == 1);              // the draw was ATTEMPTED -- the precondition passed
    CHECK(uifake::BeginRefusals() == 1);
    CHECK(f.C().inFrameDraws == 0);
    CHECK(f.C().refusedDraws == 1);
    CHECK(f.C().refusedByFacade == 1);
    CHECK(f.C().refusedNoFrameByte == 0);   // the reason is the facade, not the byte
    // The half of the aggregate this is (L5-RUN-5 G2). A single refusedByFacade could not say
    // whether the facade turned us down or the build had never wired a draw fn, and on the launch
    // that read facade=3975 with the precondition satisfied that was the whole question.
    CHECK(f.C().refusedBeginFalse == 1);
    CHECK(f.C().refusedNoDraw == 0);
    // ★ MUTANT 4. Make fake_gui.cpp's Begin return true unconditionally and this case reds on
    // BeginRefusals, inFrameDraws and refusedDraws together.
    f.CheckInvariant();
}

// The OTHER half of the same aggregate, which is the half a launch cannot tell apart from the
// first one without this split. A build that never called SetDrawFn refuses every entry with the
// frame wide open -- and it is our defect, not the facade's.
TEST_CASE("a missing draw fn and a facade refusal are two different halves of facade=") {
    Fixture f;
    f.globals.OpenFrame();
    ui::inframe::SetDrawFn(nullptr);

    ui::inframe::CallSiteForTest(&HeartbeatNoop, &f);
    ui::inframe::CallSiteForTest(&HeartbeatNoop, &f);

    CHECK(f.C().frameSiteEntries == 2);
    CHECK(g_drawCalls == 0);                  // nothing to call: the refusal is ours
    CHECK(f.C().refusedNoDraw == 2);
    CHECK(f.C().refusedBeginFalse == 0);
    CHECK(f.C().refusedByFacade == 2);        // the aggregate W2-R's frozen `facade=` field reads
    CHECK(f.C().refusedDraws == 2);
    CHECK(f.C().inFrameDraws == 0);
    const char* const line = ui::csregister::Diagnostic();
    CHECK(strstr(line, "[inframe entries=2 draws=0 refused=2 ") != nullptr);
    CHECK(strstr(line, "facade=2 ") != nullptr);
    CHECK(strstr(line, "[facade nodraw=2 beginfalse=0 reason=none bound=") != nullptr);
    f.CheckInvariant();
}

TEST_CASE("a closed panel is not asked, so it is not refused") {
    Fixture f;
    g_nextOutcome = ui::inframe::DrawOutcome::NotShowing;

    for (int frame = 0; frame < 4; ++frame) f.Frame();

    CHECK(f.C().frameSiteEntries == 4);
    CHECK(f.C().panelClosedEntries == 4);
    CHECK(f.C().refusedDraws == 0);   // folding this into refusedDraws would read as FrameClosedAtSite
    CHECK(f.C().inFrameDraws == 0);
    f.CheckInvariant();
}

TEST_CASE("the site draws with band-3 registration dead") {
    Fixture f;
    // Nothing has been registered, no callback has ever fired, and csregister has no game space.
    // ARCH-15 S2's whole case for this site is that it is independent of all of that.
    const ui::csregister::Report before = ui::csregister::Status_();
    REQUIRE(before.registrations == 0);
    REQUIRE(before.invocations == 0);

    for (int frame = 0; frame < 3; ++frame) f.Frame();

    CHECK(f.C().inFrameDraws == 3);
    const ui::csregister::Report after = ui::csregister::Status_();
    CHECK(after.registrations == 0);
    CHECK(after.inFrameDraws == 3);
    CHECK(after.frameSiteEntries == 3);
    // Leg 3 is satisfied; the other two legs are not, so Running is still correctly withheld.
    CHECK_FALSE(after.runningPerDesign);
    f.CheckInvariant();
}

// =================================================================================================
// C -- the fallback, which is the only thing ownerDraws can still certify.
// =================================================================================================

TEST_CASE("with the detour unarmed the fallback counts refusals, not draws") {
    Fixture f;
    REQUIRE_FALSE(ui::inframe::Installed());

    f.globals.ShutFrame();
    for (int tick = 0; tick < 4; ++tick) CHECK_FALSE(ui::inframe::AttemptFallbackDraw());

    CHECK(f.C().fallbackEntries == 4);
    CHECK(f.C().frameSiteEntries == 0);      // the detour never ran
    CHECK(f.C().refusedDraws == 4);
    CHECK(f.C().refusedNoFrameByte == 4);
    CHECK(f.C().inFrameDraws == 0);
    CHECK(g_drawCalls == 0);
    f.CheckInvariant();
}

TEST_CASE("the fallback stands down while the detour is armed") {
    Fixture f;
    ui::inframe::SetInstalledForTest(true);
    f.globals.OpenFrame();

    for (int tick = 0; tick < 3; ++tick) CHECK_FALSE(ui::inframe::AttemptFallbackDraw());

    // Not a refusal and not a draw: the tick did not attempt anything, because the site did.
    CHECK(f.C().fallbackEntries == 0);
    CHECK(f.C().refusedDraws == 0);
    CHECK(f.C().inFrameDraws == 0);
    CHECK(g_drawCalls == 0);
    f.CheckInvariant();
}

TEST_CASE("ownerDraws is credited only when the fallback reached a frame") {
    Fixture f;
    f.globals.OpenFrame();
    CHECK(ui::inframe::AttemptFallbackDraw());
    CHECK(f.C().inFrameDraws == 1);
    CHECK(f.C().fallbackEntries == 1);

    f.globals.ShutFrame();
    CHECK_FALSE(ui::inframe::AttemptFallbackDraw());
    CHECK(f.C().inFrameDraws == 1);
    CHECK(f.C().refusedDraws == 1);
    f.CheckInvariant();
}

// =================================================================================================
// D -- the install ladder. The guard must REFUSE by name, not install and hope.
// =================================================================================================

TEST_CASE("a site whose bytes are not the recorded ones refuses the hook by name") {
    ui::inframe::ResetForTest();
    hookmgr::ResetForTest();
    config::ResetForTest();
    buildguard::ResetForTest();

    // The build guard cannot pass offline -- it needs the real echovr.exe -- so the verdict is set
    // directly and hookmgr's ladder is driven from there, the way tests/bootstrap does it. Without
    // this the build refusal outranks the prologue refusal and the guard is never exercised.
    buildguard::SetVerdictForTest(BuildVerdict::Accept);
    REQUIRE(buildguard::Passed());

    const uint64_t va = SyntheticPcVa(reinterpret_cast<const void*>(&InFrameVictim));
    const std::vector<uint8_t> real = BytesAt(reinterpret_cast<const void*>(&InFrameVictim),
                                              kPrologueLen);
    const std::vector<uint8_t> before = real;

    SUBCASE("the recorded bytes arm it") {
        const HookStatus s = ui::inframe::InstallForTest("W2F.INFRAME.VICTIM", va, real.data(),
                                                         real.size(), "capture.voip");
        REQUIRE(s == HookStatus::Armed);
        CHECK(ui::inframe::Installed());
        CHECK(ui::inframe::InstallStatus() == HookStatus::Armed);
        hookmgr::Shutdown();
    }

    SUBCASE("one wrong byte refuses it, and nothing is written") {
        std::vector<uint8_t> wrong = real;
        wrong[0] = static_cast<uint8_t>(wrong[0] ^ 0xFFu);

        const HookStatus s = ui::inframe::InstallForTest("W2F.INFRAME.WRONG", va, wrong.data(),
                                                         wrong.size(), "capture.voip");
        // ★ MUTANT 3. Install without passing the prologue through hookmgr::Create and this is
        // Armed instead of RefusedByPrologue, and the two Installed() checks below flip.
        CHECK(s == HookStatus::RefusedByPrologue);
        CHECK(hookmgr::Status("W2F.INFRAME.WRONG") == HookStatus::RefusedByPrologue);
        CHECK_FALSE(ui::inframe::Installed());
        CHECK(ui::inframe::InstallStatus() == HookStatus::RefusedByPrologue);
        CHECK(hookmgr::ArmedCount() == 0);

        // Refused must mean "did not write", not merely "returned a status".
        CHECK(BytesAt(reinterpret_cast<const void*>(&InFrameVictim), kPrologueLen) == before);
        CHECK(InFrameVictim(4) == 22);
    }

    SUBCASE("a row nobody declared is refused, and the fallback is what is left") {
        const HookStatus s = ui::inframe::InstallForTest("W2F.INFRAME.NOKEY", va, real.data(),
                                                         real.size(), "ui.inframe_draw.absent");
        CHECK(s == HookStatus::RefusedByConfig);
        CHECK_FALSE(ui::inframe::Installed());
    }

    hookmgr::ResetForTest();
    buildguard::ResetForTest();
    ui::inframe::ResetForTest();
}

// ---------------------------------------------------------------------------------------------
// The owner thread at the site was an assumption with no check and no counter: if the
// engine ever entered this vtable slot from another thread we would drive ImGui from it, and the
// record would look identical. These two cases are the check and its wiring.
// ---------------------------------------------------------------------------------------------
namespace {

Fixture* g_offThreadFixture = nullptr;

DWORD WINAPI EnterSiteOnWorker(void*) {
    // No doctest assertion on this thread: doctest's are not thread-safe, and the owner thread
    // reads every counter back after the join, where the wait gives the ordering for free.
    if (g_offThreadFixture != nullptr) g_offThreadFixture->Frame();
    return 0;
}

uint32_t EnterSiteFromWorker() {
    DWORD tid = 0;
    HANDLE h = ::CreateThread(nullptr, 0, &EnterSiteOnWorker, nullptr, 0, &tid);
    REQUIRE(h != nullptr);
    REQUIRE(::WaitForSingleObject(h, 30000) == WAIT_OBJECT_0);
    ::CloseHandle(h);
    return static_cast<uint32_t>(tid);
}

}  // namespace

TEST_CASE("a site entry on another thread forwards the engine's call and draws NOTHING") {
    Fixture f;
    g_offThreadFixture = &f;

    f.Frame();                                   // the owner thread, latched by this first entry
    REQUIRE(f.C().frameSiteEntries == 1);
    REQUIRE(f.C().inFrameDraws == 1);
    REQUIRE(f.C().offThreadSiteEntries == 0);

    const uint32_t worker = EnterSiteFromWorker();
    g_offThreadFixture = nullptr;

    REQUIRE(worker != ::GetCurrentThreadId());   // a real second thread, not a fabricated id
    CHECK(f.C().offThreadSiteEntries == 1);
    CHECK(g_originalCalls == 2);                 // the ENGINE's own call is never withheld
    CHECK(f.C().frameSiteEntries == 1);          // ...but the entry is not in leg 3's denominator
    CHECK(f.C().inFrameDraws == 1);
    CHECK(g_drawCalls == 1);
    f.CheckInvariant();
}

// The wiring, not the arithmetic: "owner" means whatever csregister's owner tick stamped, and
// OnTick takes that thread id as an argument, so a tick stamped with somebody else's id makes THIS
// thread the foreign one. Reds if the NoteOwnerThread call is dropped out of OnTick.
TEST_CASE("the site's idea of the owner thread comes from csregister's own tick") {
    Fixture f;
    f.Frame();
    REQUIRE(f.C().frameSiteEntries == 1);

    const uint32_t notThisThread = ::GetCurrentThreadId() ^ 1u;
    ui::csregister::OnTick(nullptr, notThisThread);

    f.Frame();
    CHECK(f.C().offThreadSiteEntries == 1);
    CHECK(f.C().frameSiteEntries == 1);
    CHECK(f.C().inFrameDraws == 1);
    CHECK(g_drawCalls == 1);

    // ...and the stamp is not a one-way trap: the real owner tick puts it back.
    ui::csregister::OnTick(nullptr, ::GetCurrentThreadId());
    f.Frame();
    CHECK(f.C().frameSiteEntries == 2);
    CHECK(f.C().inFrameDraws == 2);
    CHECK(f.C().offThreadSiteEntries == 1);
    f.CheckInvariant();
}

// ---------------------------------------------------------------------------------------------
// ARCH-15 S7 / RES-W1M-C0 S6. The premise the whole packet rests on -- GUI.FRAME_OPEN_BYTE is 0 at
// the top of the stage-0 drain, so a panel drawn from the owner tick is always refused -- was
// argued from the disassembly and never measured. Now every owner tick samples it. These cases
// pin both answers: the predicted 0, and the one that would falsify the premise.
// Mutants they red under: drop the sampler call out of csregister::OnTick (samples stays 0);
// count the sample instead of the byte (nonzero moves on a shut frame); forget the tick index.
// ---------------------------------------------------------------------------------------------
TEST_CASE("the stage-0 frame byte is sampled every owner tick, and the prediction is 0") {
    Fixture f;
    REQUIRE(f.globals.frameOpenByte == 0);        // the drain's own view: the frame is shut

    ui::csregister::OnTick(nullptr, ::GetCurrentThreadId());
    ui::csregister::OnTick(nullptr, ::GetCurrentThreadId());
    CHECK(f.C().frameOpenAtOwnerTickSamples == 2);
    CHECK(f.C().frameOpenAtOwnerTickNonZero == 0);
    CHECK(f.C().frameOpenAtOwnerTickLastNonZeroTick == 0);
    CHECK(strstr(ui::csregister::Diagnostic(), "GUI FRAME OPEN AT STAGE 0") == nullptr);
    CHECK(strstr(ui::csregister::Diagnostic(),
                 "[stage0 frameopen samples=2 nonzero=0 lasttick=0]") != nullptr);
}

TEST_CASE("a frame that IS open at stage 0 is counted, dated and said out loud") {
    Fixture f;
    ui::csregister::OnTick(nullptr, ::GetCurrentThreadId());
    f.globals.frameOpenByte = 1;                  // ARCH-15 S1.4 says this never happens
    ui::csregister::OnTick(nullptr, ::GetCurrentThreadId());
    f.globals.frameOpenByte = 0;
    ui::csregister::OnTick(nullptr, ::GetCurrentThreadId());

    CHECK(f.C().frameOpenAtOwnerTickSamples == 3);
    CHECK(f.C().frameOpenAtOwnerTickNonZero == 1);
    CHECK(f.C().frameOpenAtOwnerTickLastNonZeroTick == 2);   // the SECOND owner tick, not the last
    const char* const diag = ui::csregister::Diagnostic();
    CHECK(strstr(diag, "GUI FRAME OPEN AT STAGE 0: 1 of 3") != nullptr);
    CHECK(strstr(diag, "last on owner tick 2") != nullptr);
    CHECK(strstr(diag, "[stage0 frameopen samples=3 nonzero=1 lasttick=2]") != nullptr);
}

// An address that does not resolve is NOT a sample of 0: unknown and "asked, shut" are different
// answers, and samples < ownerTicks is how a reader tells them apart.
TEST_CASE("an unreadable frame byte is not counted as a sample") {
    Fixture f;
    ui::inframe::SetProbeForTest(nullptr, &f.globals.startFrameLatch, &f.globals.contextPtr);
    ui::csregister::OnTick(nullptr, ::GetCurrentThreadId());
    ui::csregister::OnTick(nullptr, ::GetCurrentThreadId());
    CHECK(f.C().frameOpenAtOwnerTickSamples == 0);
    CHECK(f.C().frameOpenAtOwnerTickNonZero == 0);
    CHECK(ui::csregister::Status_().ownerTicks == 2);
}

// ==================================================================================================
// THE ARCH-20 SUPPRESSION GATE.
//
// ★ WHAT THESE CASES ARE WRITTEN TO CATCH, one mutant per arm, each confirmed by hand:
//   1. drop the `if (!GateScopeEnabled())` early-out (i.e. ship the feature ON by accident)
//        -> "ui.gate_scope is FALSE by default and nothing touches the engine's byte" reds.
//   2. call SET_SUPPRESSED before the draw instead of after
//        -> "the scope clears, then draws, then restores, in that order" reds on the event string.
//   3. restore from a literal 0/1 instead of the observed byte, or pass a1 = 0
//        -> "CLEAR_SUPPRESSED is handed the byte that was observed" reds.
//   4. replace the destructor with an explicit call at the end of the draw
//        -> "a draw that throws still restores the byte" reds (and leaks the gate in production).
//   5. treat an unresolved gate as "not suppressed" and draw anyway
//        -> "a gate that cannot be sampled draws with NO scope" reds -- note the contract that
//           case pins is "no scope", not "no draw"; see the case itself.
//   6. enter a scope when the byte was already 0
//        -> "a byte that is already clear is drawn on with NO scope" reds -- and in production that
//           mutant SUPPRESSES a GUI the operator had turned on with the engine's own hotkey.
//   7. ignore ClearSuppressed's false return and count a scope anyway
//        -> "an unmapped setter is a refusal by name and no scope is entered" reds.
namespace {

int g_gateDraws = 0;
bool g_gateDrawThrows = false;

replay::ui::inframe::DrawOutcome GateDrawFn() {
    ++g_gateDraws;
    uifake::NoteGateEvent('D');
    if (g_gateDrawThrows) throw 7;
    return replay::ui::inframe::DrawOutcome::Drawn;
}

// A first frame that costs more than the whole per-frame budget, the way the real one does: the
// window, the font atlas and the settings file are built once. Spins rather than sleeps, because
// the thing under test is a QueryPerformanceCounter delta and a sleep does not have to be one.
int g_slowFirstDraws = 0;
replay::ui::inframe::DrawOutcome SlowFirstDrawFn() {
    if (g_slowFirstDraws++ == 0) {
        LARGE_INTEGER freq{};
        LARGE_INTEGER start{};
        LARGE_INTEGER now{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
        // 1 ms: five times the budget, and well clear of any timer granularity argument.
        const long long ticks = freq.QuadPart / std::milli::den;   // QPC ticks in 1 ms
        do {
            QueryPerformanceCounter(&now);
        } while (now.QuadPart - start.QuadPart < ticks);
    }
    return replay::ui::inframe::DrawOutcome::Drawn;
}

// One site entry with the frame open, the gate probe wired, and the fallback path armed so the
// draw goes through the SAME Attempt() the row-2.5 detour uses.
struct GateFixture {
    uint8_t frameOpen = 1;
    uint8_t latch = 1;
    const void* context = reinterpret_cast<const void*>(0x1);
    uint8_t suppress = 1;
    int32_t reentry = 0;
    int32_t budget = 0;
    uint8_t hotkey = 0;

    GateFixture() {
        replay::ui::inframe::ResetForTest();
        uifake::Reset();
        uifake::ResetGate();
        g_gateDraws = 0;
        g_gateDrawThrows = false;
        replay::ui::inframe::SetProbeForTest(&frameOpen, &latch, &context);
        replay::ui::inframe::SetGateProbeForTest(&suppress, &reentry, &budget, &hotkey);
        replay::ui::inframe::SetDrawFn(&GateDrawFn);
        replay::ui::inframe::SetInstalledForTest(false);
    }
    ~GateFixture() {
        replay::ui::inframe::SetGateScopeForTest(-1);
        replay::ui::inframe::SetGateProbeForTest(nullptr, nullptr, nullptr, nullptr);
        replay::ui::inframe::SetGuiProbeForTest(nullptr, nullptr, nullptr, nullptr, nullptr);
        replay::ui::inframe::SetProbeForTest(nullptr, nullptr, nullptr);
        replay::ui::inframe::ResetForTest();
    }
    void Draw() { replay::ui::inframe::AttemptFallbackDraw(); }
    const replay::ui::inframe::Counters& C() { return replay::ui::inframe::Read(); }
};

}  // namespace

// ★ THE DEFAULT, AND THIS CASE MUST GO RED IF ANYBODY FLIPS IT WITHOUT PACKET W2-F-C3. ARCH-20
// s6.0 shipped it default FALSE; s6.3(b) -- "a look or a screenshot showing the panel" -- was met by
// build is retail in behaviour -- no engine setter is called and not one gate counter moves.
TEST_CASE("ui.gate_scope is TRUE by default and the scope opens and restores the engine's byte") {
    GateFixture f;
    CHECK(replay::ui::inframe::GateScopeEnabled());

    f.suppress = 1;   // the retail state: the gate is SHUT and there is something to open
    f.Draw();

    CHECK(g_gateDraws == 1);
    CHECK(uifake::GateEvents() == "CDS");   // clear, draw, restore -- in that order
    CHECK(f.suppress == 1);                 // and the byte is back where it was found
    CHECK(f.C().suppressScopeEntries == 1);
    CHECK(f.C().suppressScopeRestores == 1);
    CHECK(f.C().gateUnresolved == 0);
    CHECK(f.C().gateAlreadyOpen == 0);
}

// The row false is still the retail build, and it is still one line of configuration away.
TEST_CASE("ui.gate_scope false is retail: no engine setter is called at all") {
    GateFixture f;
    replay::ui::inframe::SetGateScopeForTest(0);

    f.suppress = 1;
    f.Draw();

    CHECK(g_gateDraws == 1);            // the draw still happens; it is the gate that does not move
    CHECK(uifake::GateEvents() == "D"); // no C, no S -- the engine was not called at all
    CHECK(f.suppress == 1);
    CHECK(f.C().suppressScopeEntries == 0);
    CHECK(f.C().suppressScopeRestores == 0);
    CHECK(uifake::LastHotkeyArg() == -1);
}

// ** H7. The launch record printed `row 2.5b ui.gate_scope disabled` about a run
// whose scope was armed for 5102 frames, because the ledger read the config row while the feature
// read its own copy of an environment variable. There is now ONE reader: this asserts that what the
// feature answers is what the ledger's own expression answers, driven both ways.
TEST_CASE("GateScopeEnabled answers the config row itself, so the ledger cannot disagree") {
    const char* const kOff = "{\"ui.gate_scope\": false}";
    const char* const kOn = "{\"ui.gate_scope\": true}";
    for (int pass = 0; pass < 2; ++pass) {
        const bool want = pass != 0;
        const char* const text = want ? kOn : kOff;
        REQUIRE(replay::config::LoadFromTextForTest(text, strlen(text)));
        // The fixture's constructor calls inframe::ResetForTest, which drops the cached answer;
        // building it AFTER the configuration is what makes the row the thing being read.
        GateFixture f;

        // The ledger dump's own expression (src/bootstrap/log.cpp), and the feature's.
        const bool ledgerReads = replay::config::Interface()->GetBool("ui.gate_scope", true);
        CHECK(ledgerReads == want);
        CHECK(replay::ui::inframe::GateScopeEnabled() == want);

        f.suppress = 1;
        f.Draw();
        CHECK(uifake::GateEvents() == std::string(want ? "CDS" : "D"));
    }
    replay::config::ResetForTest();
}

TEST_CASE("the scope clears, then draws, then restores, in that order") {
    GateFixture f;
    replay::ui::inframe::SetGateScopeForTest(1);
    f.suppress = 1;

    f.Draw();

    // Three independent counters could not say the restore came AFTER the draw. The string can.
    CHECK(uifake::GateEvents() == "CDS");
    CHECK(f.C().suppressScopeEntries == 1);
    CHECK(f.C().suppressScopeRestores == 1);
    CHECK(f.C().gateAlreadyOpen == 0);
    CHECK(f.C().inFrameDraws == 1);
}

// ARCH-20 s1.2.2: a1 reaches GUI.HOTKEY_ENABLE_BYTE, and the engine's own edge-toggle at
// 0x140F9293A is gated on that byte. Writing anything but the observed value hands the operator's
// keyboard a GUI toggle they did not have.
TEST_CASE("CLEAR_SUPPRESSED is handed the hotkey byte that was observed, not a literal") {
    GateFixture f;
    replay::ui::inframe::SetGateScopeForTest(1);
    f.suppress = 1;
    f.hotkey = 0xA5;   // a value no literal in the source spells

    f.Draw();

    CHECK(uifake::LastHotkeyArg() == 0xA5);
}

TEST_CASE("a draw that throws still restores the byte") {
    GateFixture f;
    replay::ui::inframe::SetGateScopeForTest(1);
    f.suppress = 1;
    g_gateDrawThrows = true;

    CHECK_THROWS_AS(f.Draw(), int);

    CHECK(uifake::GateEvents() == "CDS");
    CHECK(f.C().suppressScopeEntries == 1);
    // The invariant of src/ui/inframe.h: entries and restores are equal in EVERY record, and an
    // inequality is an instrument fault rather than a measurement.
    CHECK(f.C().suppressScopeEntries == f.C().suppressScopeRestores);
}

// ** W2-F-C3 CHANGED THIS CONTRACT WITH THE DEFAULT. While ui.gate_scope was opt-in, an
// unaskable gate refused the draw too, and that cost nothing. Default-true made it cost the panel
// on every build whose four gate addresses do not map -- which is every unit-test host and any
// launch where buildguard refuses a site. The scope is still never entered (a scope whose close
// cannot run leaves the engine's byte cleared); the DRAW now happens, with no scope, which is
// exactly what ui.gate_scope=false has always done. `GateEvents() == "D"` is the term that says
// the engine was not called: a 'C' here would be the partial scope this arm exists to prevent.
TEST_CASE("a gate that cannot be sampled draws with NO scope") {
    GateFixture f;
    replay::ui::inframe::SetGateScopeForTest(1);
    // One of the four addresses missing. All four or none: a gate sampled from three of them is a
    // gate whose restore argument we would be guessing.
    replay::ui::inframe::SetGateProbeForTest(&f.suppress, &f.reentry, &f.budget, nullptr);

    f.Draw();

    CHECK(g_gateDraws == 1);
    CHECK(uifake::GateEvents() == std::string("D"));   // no C and no S: no engine setter ran
    CHECK(f.C().gateUnresolved == 1);                  // and the refusal is counted, not silent
    CHECK(f.C().refusedDraws == 0);
    CHECK(f.C().inFrameDraws == 1);
    CHECK(f.C().suppressScopeEntries == 0);
    CHECK(f.C().suppressScopeRestores == 0);
    CHECK(uifake::LastHotkeyArg() == -1);
}

// ARCH-20 s2.1.2 / F6: the engine's hotkey toggle can leave the byte clear between frames, so this
// arm is ordinary. Entering a scope here would END by suppressing a GUI that was ON.
TEST_CASE("a byte that is already clear is drawn on with NO scope") {
    GateFixture f;
    replay::ui::inframe::SetGateScopeForTest(1);
    f.suppress = 0;

    f.Draw();

    CHECK(g_gateDraws == 1);
    CHECK(uifake::GateEvents() == "D");
    CHECK(f.C().gateAlreadyOpen == 1);
    CHECK(f.C().suppressScopeEntries == 0);
    CHECK(f.C().suppressScopeRestores == 0);
}

TEST_CASE("an unmapped setter is a refusal by name and no scope is entered") {
    GateFixture f;
    replay::ui::inframe::SetGateScopeForTest(1);
    f.suppress = 1;
    uifake::SetSuppressSettersBound(false, true);   // CLEAR_SUPPRESSED did not bind

    f.Draw();

    CHECK(g_gateDraws == 1);              // the draw is not withheld; only the gate is
    CHECK(uifake::GateEvents() == "D");   // and NEITHER half ran -- never a half-scope
    CHECK(f.C().suppressSetterUnmapped == 1);
    CHECK(f.C().suppressScopeEntries == 0);
    CHECK(f.C().suppressScopeRestores == 0);
}

TEST_CASE("a byte moved under the scope is counted and the observed value is restored anyway") {
    GateFixture f;
    replay::ui::inframe::SetGateScopeForTest(1);
    f.suppress = 1;
    // The fake setters record rather than write, so the probe byte never changes: at close it still
    // reads 1, which is exactly "not where this scope left it".
    f.Draw();

    CHECK(f.C().suppressScopeChanged == 1);
    CHECK(f.C().suppressScopeRestores == 1);   // restored regardless, per ARCH-20 s2.3
    CHECK(uifake::GateEvents() == "CDS");
}

// ARCH-20 s2.6: the panel's draw is measured, not asserted. 0.2 ms at 90 Hz is the budget and a
// mean would hide the one frame that blew it, so the RED is on the MAXIMUM.
TEST_CASE("panel::Draw is measured and stays inside the 200000 ns budget over 1024 frames") {
    GateFixture f;
    replay::ui::inframe::SetGateScopeForTest(1);
    f.suppress = 1;

    // 1000 frames, and one of them takes the refusal path: an error frame is drawn from the same
    // site and its cost is in the same maximum, so a status line built the expensive way would
    // show up here rather than only in a launch.
    for (int i = 0; i < 1024; ++i) {
        f.frameOpen = (i == 500) ? 0 : 1;
        f.Draw();
    }
    f.frameOpen = 1;

    CHECK(g_gateDraws == 1023);
    CHECK(f.C().refusedNoFrameByte == 1);
    CHECK(f.C().drawNanosTotal > 0);
    CHECK(f.C().drawNanosMax > 0);
    CHECK(f.C().drawNanosMax <= 200000u);
    CHECK(f.C().drawNanosTotal >= f.C().drawNanosMax);
}

// ** L5-RUN-7 s4.4: drawNanosMax = 629300 ns against a mean of 21500 over 5102 frames. The first
// draw creates the window, rasterises the panel's glyphs into the font atlas and loads the GUI's
// settings, once per process; scoring it as a per-frame cost reports a budget breach no later
// frame can reproduce. It is published as firstDrawNanos instead -- named, not hidden.
//
// The first draw here is deliberately made to cost more than the whole budget. Put the first
// sample back into the maximum and this case goes red on the same number the launch reported.
TEST_CASE("the first draw is published on its own and does not set the per-frame maximum") {
    GateFixture f;
    replay::ui::inframe::SetGateScopeForTest(0);
    g_slowFirstDraws = 0;
    replay::ui::inframe::SetDrawFn(&SlowFirstDrawFn);

    for (int i = 0; i < 64; ++i) f.Draw();

    CHECK(g_slowFirstDraws == 64);
    CHECK(f.C().firstDrawNanos > 200000u);                       // the expensive one was measured
    CHECK(f.C().drawNanosMax <= 200000u);                        // ...and it is not the maximum
    CHECK(f.C().drawNanosMax < f.C().firstDrawNanos);
    CHECK(f.C().drawNanosTotal >= f.C().firstDrawNanos);         // still inside the total
}

// The zero this field publishes is NOT a measurement, and the word beside it is what says so.
TEST_CASE("panelHoveredFrames publishes its source, so its zero cannot read as never-hovered") {
    GateFixture f;
    CHECK(f.C().panelHoveredFrames == 0);
    CHECK(std::string(replay::ui::inframe::PanelHoverSourceName()) == "none");
}

// ---- ARCH-20 S5, the read side ----------------------------------------------------------------
//
// A fake ImGui context block, laid out at the offsets the registry ratifies: io at +8, io.Fonts at
// io+0x88, and the four context-relative fields the row reads. The atlas is a SEPARATE object
// reached through the Fonts pointer, because that indirection is the thing term 2 asks about.
struct FakeImguiContext {
    alignas(16) unsigned char block[0x18B0] = {};
    alignas(16) uint32_t atlas[16] = {};

    void Wire(uint32_t atlasA, uint32_t atlasB, int32_t frameCount, bool renderFn, int32_t cmdLists,
              int32_t totalVtx) {
        // Every offset below is TRANSCRIBED rather than taken from ui_facts.h. A fixture that
        // laid itself out with the producer's own constants would move whenever the producer
        // moved and could not fail on a wrong offset -- it would be the producer's copy of the
        // answer wearing a test's clothes.
        atlas[8] = atlasA;   // facts-ok: GUI.ATLAS_CONST_A stored at io.Fonts+0x20 by sub_140F922D0
        atlas[9] = atlasB;   // facts-ok: GUI.ATLAS_CONST_B stored at io.Fonts+0x24 by sub_140F922D0
        unsigned char* const io = block + 0x8;   // facts-ok: GUI.IMGUI_IO_OFF transcribed, so the fixture is not the producer's own copy
        *reinterpret_cast<void**>(io + 0x88) = &atlas[0];   // facts-ok: GUI.IMGUI_FONTS_IO_OFF transcribed, io-relative
        *reinterpret_cast<int32_t*>(block + 0x1734) = frameCount;   // facts-ok: GUI.IMGUI_FRAME_COUNT_OFF transcribed
        // Any non-null value: the field is a callback POINTER and the probe asks only whether
        // one is installed. A non-null callback is necessary and not sufficient for pixels.
        *reinterpret_cast<void**>(block + 0xC0) = renderFn ? static_cast<void*>(&atlas[0]) : nullptr;   // facts-ok: GUI.IMGUI_RENDER_DRAW_LISTS_FN_OFF transcribed
        // ImDrawData. `valid` is what EndFrame sets and NewFrame clears; the index count is six
        // per quad against four vertices, which is what makes TotalIdxCount >= TotalVtxCount a
        // usable structural term.
        WireDrawData(1, cmdLists, totalVtx, (totalVtx * 3) / 2, &atlas[0]);
    }

    // The four ImDrawData fields the reader takes, each stored at the CONTEXT-relative offset its
    // own registry row names -- not at a transcribed struct layout. W2-F-C3 transcribed the 1.86+
    // ImGui layout here and the fixture then agreed with a reader that was reading the wrong four
    // dwords; a fixture that places its fields where the registry says the engine's stores land
    // cannot make that mistake, and a wrong row moves the fixture and the reader apart rather than
    // together, because only the reader subtracts GUI.IMDRAWDATA_OFF from them.
    void WireDrawData(uint8_t valid, int32_t cmdListsCount, int32_t totalVtx, int32_t totalIdx,
                      const void* cmdLists) {
        block[facts::GUI_IMDRAWDATA_OFF] = valid;
        *reinterpret_cast<const void**>(block + facts::GUI_IMDRAWDATA_CMDLISTS_PTR_OFF) = cmdLists;
        *reinterpret_cast<int32_t*>(block + facts::GUI_IMGUI_CMDLISTS_COUNT_OFF) = cmdListsCount;
        *reinterpret_cast<int32_t*>(block + facts::GUI_IMGUI_TOTAL_VTX_OFF) = totalVtx;
        *reinterpret_cast<int32_t*>(block + facts::GUI_IMDRAWDATA_TOTAL_IDX_OFF) = totalIdx;
    }
    const void* Base() const { return block; }
};

// ** ARCH-20 S6.3(a) REBUILT. The old reader took the two counts at the right offsets but at the
// wrong MOMENT -- between NewFrame and EndFrame, when the struct is cleared -- and published the
// cleared struct as a confident 0; the panel in L5-RUN-7-run2.png is a photograph of that 0 being
// wrong. (W2-F-C3 additionally re-aimed the reads at ImDrawData+0x04/+0x0C on a 1.86+ layout this
// engine does not have; W2-F-C3B put them back on the ratified rows. See ui_facts.h.) These three
// cases are the controls.
//
// THE FLOOR, STATED: the panel's browser view draws a window background and a title bar, two
// buttons (Rescan, Close), a separator and three text lines. Four filled quads is already 16
// vertices and the shortest string on it ("Rescan", six glyphs) is another 24, so any frame in
// which the panel is on the screen carries AT LEAST 40 vertices -- which is the >= 40 ARCH-20
// s6.3(a) asks of deltaVtx, and it is a floor rather than a measurement.
constexpr int32_t kPanelMinimumVertices = 40;

TEST_CASE("a cleared ImDrawData is could-not-ask, never a zero-vertex frame") {
    GateFixture f;
    replay::ui::inframe::SetGateScopeForTest(0);
    FakeImguiContext ctx;
    ctx.Wire(0x00001000u, 0x00000800u, 41, true, 3, 4242);
    ctx.WireDrawData(0, 3, 4242, (4242 * 3) / 2, &ctx.atlas[0]);   // Valid == 0: NewFrame has cleared it, EndFrame has not run
    const void* slot = ctx.Base();
    uint32_t constA = 0x00001000u;
    uint32_t constB = 0x00000800u;
    int32_t viewports = 1;
    int32_t balance = 7;
    replay::ui::inframe::SetGuiProbeForTest(&slot, &constA, &constB, &viewports, &balance);

    f.Draw();

    const replay::ui::inframe::Counters c = f.C();
    CHECK(c.guiImguiBaseValid);                 // the base is fine; it is the moment that is wrong
    CHECK(c.guiRenderedTotalVtxCount == replay::ui::inframe::kGuiCountUnasked);
    CHECK(c.guiRenderedCmdListsCount == replay::ui::inframe::kGuiCountUnasked);
    CHECK(c.guiDrawDataNotReady > 0);
    CHECK(c.guiDrawDataLatches == 0);
}

TEST_CASE("an ImDrawData that disagrees with itself is refused, not published") {
    GateFixture f;
    replay::ui::inframe::SetGateScopeForTest(0);
    FakeImguiContext ctx;
    ctx.Wire(0x00001000u, 0x00000800u, 41, true, 3, 4242);
    // What a wrong base looks like from the struct's side: three command lists and no CmdLists
    // array to hold them. This is the term unrelated memory has to satisfy by luck.
    ctx.WireDrawData(1, 3, 4242, (4242 * 3) / 2, nullptr);
    const void* slot = ctx.Base();
    uint32_t constA = 0x00001000u;
    uint32_t constB = 0x00000800u;
    int32_t viewports = 1;
    int32_t balance = 7;
    replay::ui::inframe::SetGuiProbeForTest(&slot, &constA, &constB, &viewports, &balance);

    f.Draw();

    const replay::ui::inframe::Counters c = f.C();
    CHECK(c.guiRenderedTotalVtxCount == replay::ui::inframe::kGuiCountUnasked);
    CHECK(c.guiDrawDataRefused > 0);
    CHECK(c.guiDrawDataLatches == 0);
}

TEST_CASE("a filled ImDrawData publishes at least the panel's own geometry") {
    GateFixture f;
    replay::ui::inframe::SetGateScopeForTest(1);
    f.suppress = 1;
    FakeImguiContext ctx;
    ctx.Wire(0x00001000u, 0x00000800u, 41, true, 2, kPanelMinimumVertices);
    const void* slot = ctx.Base();
    uint32_t constA = 0x00001000u;
    uint32_t constB = 0x00000800u;
    int32_t viewports = 1;
    int32_t balance = 7;
    replay::ui::inframe::SetGuiProbeForTest(&slot, &constA, &constB, &viewports, &balance);

    f.Draw();

    const replay::ui::inframe::Counters c = f.C();
    CHECK(c.guiDrawDataLatches == 1);
    CHECK(c.guiRenderedCmdListsCount == 2);
    CHECK(c.guiRenderedTotalVtxCount >= kPanelMinimumVertices);
    CHECK(c.guiDrawDataRefused == 0);
}

// Stands in for the engine's `inc cs:dword_1420DB9B4` at the first instruction past the gate, so
// the published delta spans the draw instead of being a constant zero either way.
int32_t g_s5Balance = 0;
replay::ui::inframe::DrawOutcome S5BalanceDrawFn() {
    g_s5Balance += 1;
    return replay::ui::inframe::DrawOutcome::Drawn;
}

TEST_CASE("the S5 read side publishes every field behind a base that proves all three terms") {
    GateFixture f;
    replay::ui::inframe::SetGateScopeForTest(0);          // the read side runs on BOTH sides

    FakeImguiContext ctx;
    ctx.Wire(0x00001000u, 0x00000800u, 41, true, 3, 4242);
    const void* slot = ctx.Base();
    uint32_t constA = 0x00001000u;
    uint32_t constB = 0x00000800u;
    int32_t viewports = 1;
    int32_t balance = 7;
    replay::ui::inframe::SetGuiProbeForTest(&slot, &constA, &constB, &viewports, &balance);

    // The draw increments the engine's BEGIN_BALANCE the way the gate's first instruction past the
    // gate does, so the delta is a measurement rather than a constant zero.
    f.Draw();
    balance = 8;
    ctx.Wire(0x00001000u, 0x00000800u, 43, true, 3, 4242);
    f.Draw();

    const replay::ui::inframe::Counters c = f.C();
    CHECK(c.guiGateSampled);
    CHECK(c.guiGateAddrResolved);
    CHECK(c.guiGateUnresolved[0] == '\0');
    CHECK(c.guiGateSamples == 2);
    CHECK(c.guiSuppressed == 1u);
    CHECK(c.guiHotkeyEnable == 0u);
    CHECK(c.guiViewportsEnabled == 1);
    CHECK(c.guiImguiBaseValid);
    CHECK(c.guiImguiBaseReason[0] == '\0');
    // Observed and expected are two READS, not one value written twice: the row re-runs term 3.
    CHECK(c.guiAtlasDwordA == 0x00001000u);
    CHECK(c.guiAtlasDwordB == 0x00000800u);
    CHECK(c.guiAtlasExpectA == 0x00001000u);
    CHECK(c.guiAtlasExpectB == 0x00000800u);
    CHECK(c.guiImguiFrameCountDelta == 2);          // 43 - 41, and the first entry had no previous
    CHECK(c.guiRenderDrawListsFnPresent == 1u);
    CHECK(c.guiRenderedCmdListsCount == 3);
    CHECK(c.guiRenderedTotalVtxCount == 4242);      // S6.3(a)'s number, off the second run
    // No scope was entered and NEITHER ENGINE SETTER RAN. 'C' is ClearSuppressed and 'S' is
    // SetSuppressed; the two 'D's are the draws themselves. The read side is a read.
    CHECK(uifake::GateEvents() == "DD");
    CHECK(c.suppressScopeEntries == 0);
}

TEST_CASE("the S5 read side publishes a BEGIN_BALANCE delta across the draw, not a constant") {
    GateFixture f;
    replay::ui::inframe::SetGateScopeForTest(0);

    FakeImguiContext ctx;
    ctx.Wire(0x00001000u, 0x00000800u, 1, true, 0, 0);
    const void* slot = ctx.Base();
    uint32_t constA = 0x00001000u;
    uint32_t constB = 0x00000800u;
    int32_t viewports = 0;
    g_s5Balance = 100;
    // The engine increments the counter on the first instruction past the gate. The fake draw does
    // the same, so the published delta is +1 and a producer that published `after` alone reds.
    replay::ui::inframe::SetGuiProbeForTest(&slot, &constA, &constB, &viewports, &g_s5Balance);
    replay::ui::inframe::SetDrawFn(&S5BalanceDrawFn);

    f.Draw();

    CHECK(f.C().guiBeginBalanceDelta == 1);
    CHECK(f.C().guiGateSamples == 1);
}

TEST_CASE("the B1 base -- a live global that is not the context -- publishes imguiBaseValid=0") {
    GateFixture f;
    replay::ui::inframe::SetGateScopeForTest(0);

    // Reconstructed: an unrelated live engine object read as the context.
    // Terms 1 and 2 BOTH pass on it -- there is a non-null pointer at +0x90 and dwords behind it --
    // and only term 3 can tell it apart. A producer that dropped term 3 reds here.
    FakeImguiContext wrong;
    wrong.Wire(0xDEADBEEFu, 0x0BADF00Du, 999, true, 77, 9999);
    const void* slot = wrong.Base();
    uint32_t constA = 0x00001000u;
    uint32_t constB = 0x00000800u;
    int32_t viewports = 0;
    int32_t balance = 0;
    replay::ui::inframe::SetGuiProbeForTest(&slot, &constA, &constB, &viewports, &balance);

    f.Draw();

    const replay::ui::inframe::Counters c = f.C();
    CHECK(c.guiGateSamples == 1);
    CHECK_FALSE(c.guiImguiBaseValid);
    CHECK(strstr(c.guiImguiBaseReason, "term 3") != nullptr);
    // The observed pair is still published -- that is what lets the row re-run the comparison.
    CHECK(c.guiAtlasDwordA == 0xDEADBEEFu);
    CHECK(c.guiAtlasDwordB == 0x0BADF00Du);
    CHECK(c.guiAtlasExpectA == 0x00001000u);
    CHECK(c.guiAtlasExpectB == 0x00000800u);
    // ** AND NOT ONE OF THE FOUR CONTEXT FIELDS CARRIES A NUMBER. 9999 vertices sat behind that
    // pointer and none of it reached the record: the row prints could-not-ask, never 0-the-answer.
    CHECK(c.guiImguiFrameCountDelta == 0);
    CHECK(c.guiRenderDrawListsFnPresent == 0u);
    CHECK(c.guiRenderedCmdListsCount == replay::ui::inframe::kGuiCountUnasked);
    CHECK(c.guiRenderedTotalVtxCount == replay::ui::inframe::kGuiCountUnasked);
}

TEST_CASE("a null context pointer fails term 1 by name, and reads nothing behind it") {
    GateFixture f;
    replay::ui::inframe::SetGateScopeForTest(0);

    const void* slot = nullptr;
    uint32_t constA = 0x00001000u;
    uint32_t constB = 0x00000800u;
    int32_t viewports = 0;
    int32_t balance = 0;
    replay::ui::inframe::SetGuiProbeForTest(&slot, &constA, &constB, &viewports, &balance);

    f.Draw();

    const replay::ui::inframe::Counters c = f.C();
    CHECK(c.guiGateAddrResolved);          // the ADDRESS resolved; the POINTER IN IT is null
    CHECK_FALSE(c.guiImguiBaseValid);
    CHECK(strstr(c.guiImguiBaseReason, "term 1") != nullptr);
    CHECK(c.guiAtlasDwordA == 0u);
    CHECK(c.guiAtlasExpectA == 0x00001000u);   // read from .rdata whatever the base did
}

TEST_CASE("an S5 address that does not resolve is NAMED, and no field under it is a number") {
    GateFixture f;
    replay::ui::inframe::SetGateScopeForTest(0);

    FakeImguiContext ctx;
    ctx.Wire(0x00001000u, 0x00000800u, 5, true, 1, 55);
    const void* slot = ctx.Base();
    uint32_t constA = 0x00001000u;
    int32_t viewports = 0;
    int32_t balance = 0;
    // GUI.ATLAS_CONST_B missing. Every other address is there, which is the point: the row must
    // not score six good fields beside a seventh that was invented.
    replay::ui::inframe::SetGuiProbeForTest(&slot, &constA, nullptr, &viewports, &balance);

    f.Draw();

    const replay::ui::inframe::Counters c = f.C();
    CHECK_FALSE(c.guiGateAddrResolved);
    CHECK(strcmp(c.guiGateUnresolved, "GUI.ATLAS_CONST_B") == 0);
    CHECK(c.guiGateSamples == 0);          // no pair was completed, so the denominator does not move
    CHECK(c.guiRenderedTotalVtxCount == replay::ui::inframe::kGuiCountUnasked);
}

TEST_CASE("the S5 read side runs with the gate scope ON as well, around the scope") {
    GateFixture f;
    replay::ui::inframe::SetGateScopeForTest(1);

    FakeImguiContext ctx;
    ctx.Wire(0x00001000u, 0x00000800u, 9, true, 2, 128);
    const void* slot = ctx.Base();
    uint32_t constA = 0x00001000u;
    uint32_t constB = 0x00000800u;
    int32_t viewports = 0;
    int32_t balance = 0;
    replay::ui::inframe::SetGuiProbeForTest(&slot, &constA, &constB, &viewports, &balance);
    f.suppress = 1;

    f.Draw();

    const replay::ui::inframe::Counters c = f.C();
    CHECK(c.guiGateSamples == 1);
    CHECK(c.guiRenderedTotalVtxCount == 128);
    // S6.3(a) compares this number across the flip, so the scope run must publish it too.
    CHECK(uifake::GateEvents() == "CDS");
    CHECK(c.suppressScopeEntries == 1);
}

// =================================================================================================
// H -- the distribution behind drawNanosMax (W2-F DRAW4).
// =================================================================================================

namespace {
int g_slowDraws = 0;

replay::ui::inframe::DrawOutcome SleepyDraw() {
    if (g_slowDraws > 0) {
        --g_slowDraws;
        ::Sleep(3);
    }
    return replay::ui::inframe::DrawOutcome::Drawn;
}
}  // namespace

// The budget row scores drawNanosMax and goes on scoring it: one frame over the ceiling is a
// stutter the owner sees. What a maximum cannot say is whether it is one frame or every frame,
// and that is the difference between this host's scheduler and a defect. The histogram says it at
// O(1) per frame -- and the case that proves it is the one where a single outlier moves p999 and
// leaves p99 alone, which a constant, a maximum or a mean could not do.
TEST_CASE("the draw histogram separates one outlier from the shape of every frame") {
    Fixture f;
    ui::inframe::SetDrawFn(&SleepyDraw);
    f.globals.OpenFrame();

    // 101 draws: the first is excluded from every maximum here, then 99 fast ones and one that
    // takes 3 ms -- an order of magnitude past the top of the histogram, so this is also the case
    // where a frame lands off the end of the buckets.
    constexpr int kDraws = 101;
    for (int i = 0; i < kDraws; ++i) {
        g_slowDraws = i == kDraws - 1 ? 1 : 0;
        CHECK(ui::inframe::AttemptFallbackDraw());
    }

    const ui::inframe::Counters& c = ui::inframe::Read();
    CHECK(c.drawNanosSamples == kDraws - 1);
    // The outlier is past the top edge and SAYS SO, rather than saturating silently.
    CHECK(c.drawNanosOverHistogram == 1);
    CHECK(c.drawNanosMax > ui::inframe::kDrawNanosHistogramTopNs);
    // On 100 samples p999 asks for the 100th, which IS the outlier; p99 asks for the 99th, which
    // is not. A percentile that could not tell those apart would be a constant.
    CHECK(c.drawNanosP999 == ui::inframe::kDrawNanosHistogramTopNs);
    CHECK(c.drawNanosP99 < ui::inframe::kDrawNanosHistogramTopNs);
    // The upper edge never understates, and never overstates by more than one bucket.
    CHECK(c.drawNanosP99 >= ui::inframe::kDrawNanosBucketNs);
    CHECK(c.drawNanosP99 <= c.drawNanosMax);
}
