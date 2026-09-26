// W2-F. The UI module: binds the facade, registers the band-3 draw callback, and publishes the
// browser's index. One translation unit, as B.10 requires; src/plugin/dllmain.cpp is not edited.
//
// The split between the two callbacks is deliberate and is the whole concurrency story:
//   Sample() runs on OUR stage-0 tick and is the only place engine or transport state is read.
//   Draw()   renders what Sample left, and NOTHING else.
// The band-3 callback never calls Draw: it is dispatched on a CTaskKernel worker (L5-RUN-3 F6) and
// ImGui is not thread-safe, so it records the invocation into csregister's lock-free mailbox and
// csregister::OnTick -- this tick, this thread -- drains it and runs Draw. Both functions therefore
// run on this one thread, so neither needs a lock and neither can see a half-updated playhead.

#include <windows.h>

#include "bootstrap/modreg.h"
#include "bootstrap/services.h"
#include "engine/tick.h"
#include "ui/bar.h"
#include "ui/csregister.h"
#include "ui/demoindex.h"
#include "ui/gui.h"
#include "ui/inframe.h"
#include "ui/panel.h"
#include "ui/recorder.h"
#include "ui/uiapi.h"

namespace replay {
namespace {

const char* const kReplayModuleName = "ui";

ReplayServices* g_services = nullptr;
// Static storage duration: the engine holds a pointer to our draw callback for the life of the game
// space and registration cannot be undone, so nothing the callback can reach may be destroyed.
ui::DemoIndex g_index;
bool g_publishedIndex = false;

void UiTick(const TickInfo& info) {
    ui::panel::Sample(info.frameDeltaUs);
    ui::bar::Sample(info.frameDeltaUs);
    // Before the draw the mailbox drains into: the REC indicator and the recorder row are drawn
    // from what this reads, and a press recorded by that draw is issued by the next tick's read.
    ui::recorder::Sample();
    ui::csregister::OnTick(info.netGame, ::GetCurrentThreadId());
}

// ★ ONE DRAW SITE, TWO WINDOWS, AND THE ANSWER IS THE BETTER OF THE TWO. csregister scores this
// return value: Drawn, NotShowing and FacadeRefused are three different facts about the frame, and
// a bar that drew while the demoted panel was switched off is a DRAWN frame, not a hidden one. A
// refusal only survives when nothing drew, which is exactly when it is the whole story.
ui::inframe::DrawOutcome DrawUi() {
    const ui::inframe::DrawOutcome panel = ui::panel::Draw();
    const ui::inframe::DrawOutcome bar = ui::bar::Draw();
    if (panel == ui::inframe::DrawOutcome::Drawn || bar == ui::inframe::DrawOutcome::Drawn) {
        return ui::inframe::DrawOutcome::Drawn;
    }
    if (panel == ui::inframe::DrawOutcome::FacadeRefused ||
        bar == ui::inframe::DrawOutcome::FacadeRefused) {
        return ui::inframe::DrawOutcome::FacadeRefused;
    }
    return ui::inframe::DrawOutcome::NotShowing;
}

bool UiInit(ReplayServices* services) {
    REPLAY_REQUIRE(services, log);
    REPLAY_REQUIRE(services, config);
    g_services = services;

    // A failed bind is not a failed init. The panel reports which entry points it lost and what
    // that costs; refusing to load would take the browser down with the scrub bar and leave the
    // operator with nothing to read.
    if (!ui::gui::Bind()) {
        services->log->Write(LogLevel::Warn, kReplayModuleName,
                             "the ImGui facade did not bind: %s", ui::gui::Diagnostic());
    }

    // B.0 names W1-B as IDemoIndex's owner and nothing implements it. We publish ours ONLY into a
    // null slot, so a later W1-B implementation takes precedence without either of us having to
    // know about the other.
    if (services->demoIndex == nullptr) {
        services->demoIndex = &g_index;
        g_publishedIndex = true;
        services->log->Write(LogLevel::Info, kReplayModuleName,
                             "published a manifest-only IDemoIndex into the null services slot");
    }

    ui::panel::Init(services, &g_index);
    ui::bar::Init(services);
    ui::recorder::Init(services);
    modreg::RegisterPreExit(&ui::panel::StopWorkerForExit);
    ui::csregister::SetDrawFn(&DrawUi);

    // ★ THE DRAW SITE (DESIGN S4.4 ledger row 2.5, ARCH-15). Without this detour there is no site
    // at which a draw is accepted: at our stage-0 tick the engine's GUI frame is shut and every
    // widget call is refused at NRadGui::Begin. A refusal here is logged by name and the owner-tick
    // fallback takes over -- it draws nothing visible, but it counts its refusals rather than
    // reporting them as draws, so the launch record says so.
    if (services->config->GetBool("bootstrap.minimal", false)) {
        services->log->Write(LogLevel::Warn, kReplayModuleName,
                             "bootstrap.minimal: the in-frame draw site is not installed and the "
                             "panel will not be visible. Diagnostic only");
    } else if (!ui::inframe::Install()) {
        services->log->Write(LogLevel::Warn, kReplayModuleName,
                             "in-frame draw site not installed (%s): the panel will not be visible "
                             "and every draw will be counted as refused",
                             HookStatusName(ui::inframe::InstallStatus()));
    }
    // ★ The panel is open because THIS module opened it (AUDIT_F3I F3I-4). It is deliberately not
    // waiting on the band-3 registration: that hook may never land, and the owner's product is the
    // viewer. Registration, when it lands, buys the compositing slot and the telemetry.
    ui::csregister::Enable();

    services->log->Write(LogLevel::Info, kReplayModuleName,
                         "ui ready: facade %u/%u entry points, replay folder %ls",
                         ui::gui::BoundCount(), static_cast<uint32_t>(ui::gui::Entry::Count),
                         ui::ReplayDirectory());
    return true;
}

void UiShutdown() {
    // ★ Disable rather than unregister, because there is no unregister. The engine keeps calling
    // our callback until the game space dies; Disable makes it return immediately, and every piece
    // of state it touches has static storage duration and is still alive when it does.
    ui::csregister::Disable();
    // Before the panel, because a drag still in flight holds the audio mute and its EndScrub is the
    // only thing that gives it back.
    ui::bar::Shutdown();
    ui::recorder::Shutdown();
    ui::panel::Shutdown();
    if (g_publishedIndex && g_services != nullptr && g_services->demoIndex == &g_index) {
        g_services->demoIndex = nullptr;
    }
    g_index.Close();
    g_publishedIndex = false;
    g_services = nullptr;
}

REPLAY_MODULE("ui", 70, UiInit, UiTick, UiShutdown);

}  // namespace
}  // namespace replay
