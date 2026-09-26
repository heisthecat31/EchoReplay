// W3-A. Two jobs, both about things nothing else in the tree would notice.
//
// ---------------------------------------------------------------------------------------------
// ★★★ WHY A MODULE THAT DOES ALMOST NOTHING IS THE MOST IMPORTANT FILE IN THIS PACKET.
//
// The residue rows are registered through ArmPlaybackResidue(battery, log), and the battery is a
// member of an anonymous-namespace Global inside W2-B's playback_module.cpp. Nothing outside that
// file can name it. So this packet's primitives run if and only if W2-B calls the seam -- and until
// it does, every one of them is code that compiles, links, tests green and never executes.
//
// That is not hypothetical. audio::ArmAudioResidue(ResidueBattery&) has existed since W2-H landed
// and HAS NO CALLERS anywhere in src/ or tests/, so N-16, N-17a, N-17b and N-5a are built, verified
// and dead in the shipping DLL right now -- while W2-B's placeholder rows go on reporting those same
// ids as unmapped to the owner. The same trap was waiting for this packet, and the difference
// between finding it and shipping into it is one log line at startup.
//
// "Verified and wired are different properties" is W2-B's own sentence, written after the transport
// linked as a library the DLL never pulled in. This is that sentence made into a check.
//
// ---------------------------------------------------------------------------------------------
// ★★ ORDER 290, WHICH IS ONE BEFORE PLAYBACK'S 300, AND THAT IS THE WHOLE TRICK.
//
// Init runs in ascending module order, so this file's Init lands the ReplayServices pointer BEFORE
// W2-B's Init calls ArmPlaybackResidue -- which is how the arm gets an IEntity without widening a
// signature W2-B is editing right now.
//
// The "did anybody arm it" check cannot run in Init, because at 290 the answer is always no. It runs
// on the FIRST TICK instead: Dispatch does not start until every module has initialised, so the
// first tick is the earliest moment at which "nobody armed it" is a fact rather than a race.
// ---------------------------------------------------------------------------------------------

#include "bootstrap/module_order.h"
#include "bootstrap/services.h"
#include "engine/tick.h"
#include "playback/residue/residue.h"

static const char* const kReplayModuleName = "residue-check";

namespace replay {
namespace playback {
namespace residue {
namespace {

ILog* g_log = nullptr;
bool g_reported = false;

void FirstTick(const TickInfo&, void*) {
    if (g_reported) {
        return;
    }
    g_reported = true;
    if (!g_log) {
        return;
    }

    const RosterSummary sum = Summary();

    if (!Stats().armed) {
        g_log->Write(LogLevel::Error, kReplayModuleName,
                     "the category-N residue battery was never armed: nobody called "
                     "residue::ArmPlaybackResidue(battery, log). N-2, N-3a/b, N-4, N-6a/b, N-7a/b, "
                     "N-8a/b and N-18 are linked into this DLL and will not run. "
                     "audio::ArmAudioResidue is in the same state and has been since W2-H landed.");
        return;
    }

    // ★★★ THE DENOMINATOR, SAID OUT LOUD ONCE PER SESSION. The battery's own UnavailableCount() is
    // taken over what REGISTERED, so a residue nobody built cannot appear as missing. This line
    // counts against the design's nineteen rows instead, which is the only denominator that
    // can report a gap it was not told about.
    g_log->Write(sum.unregistered > 0 ? LogLevel::Warn : LogLevel::Info, kReplayModuleName,
                 "category-N coverage against the design: %u known, %u ready, %u partial, "
                 "%u unavailable, %u NEVER REGISTERED",
                 (unsigned)sum.known, (unsigned)sum.ready, (unsigned)sum.partial,
                 (unsigned)sum.unavailable, (unsigned)sum.unregistered);

    // And which ones, with the reason. A count tells the owner something is missing; only the list
    // tells him what his replay will not restore.
    RosterRow rows[kRosterRows];
    Roster(rows, kRosterRows);
    for (size_t i = 0; i < kRosterRows; ++i) {
        if (rows[i].coverage == Coverage::Ready) {
            continue;
        }
        g_log->Write(LogLevel::Info, kReplayModuleName, "  %s [%s] %s%s%s", rows[i].id,
                     CoverageText(rows[i].coverage), rows[i].what,
                     rows[i].note[0] ? " -- " : "", rows[i].note);
    }
}

bool Init(ReplayServices* s) {
    REPLAY_REQUIRE(s, log);
    REPLAY_REQUIRE(s, tick);
    g_log = s->log;

    // Before W2-B's Init at 300, which is the point of order 290.
    SetArmServices(s);

    if (!s->tick->RegisterEarly(&FirstTick, nullptr)) {
        // Not fatal: the rows still work. But the report that says whether they were ever armed is
        // gone, so say which of the two this build is rather than letting silence mean either.
        s->log->Write(LogLevel::Warn, kReplayModuleName,
                      "the tick table refused this module's callback, so the once-per-session "
                      "coverage report will not run; the residue rows themselves are unaffected");
    }
    return true;
}

}  // namespace
}  // namespace residue
}  // namespace playback
}  // namespace replay

REPLAY_MODULE("residue-check", ::replay::kModuleOrderResidueCheck,
              ::replay::playback::residue::Init, nullptr, nullptr);
