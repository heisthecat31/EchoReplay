// B-5 / B.12: the static module registry and its ordered dispatch.
//
// S4.1: "each product module ships one translation unit that declares itself and is linked in.
// dllmain.cpp is owned by W1-C and is never edited again. This is the mechanism that keeps Wave-2
// packets from serialising on one file."
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "bootstrap/services.h"

namespace replay {
namespace modreg {

size_t              Count();
const ReplayModule* At(size_t i);

// Init in ascending `order`, ties broken by name so the sequence does not depend on link order. A module
// that returns false is logged, is not ticked, and does not stop the others: one broken subsystem must
// not take the plugin down with it.
size_t InitAll(ReplayServices* services);
size_t InitFailures();

// The tick path. Allocation-free, lock-free, and it walks only the modules that initialised and declared
// a tick function.
void TickAll(const TickInfo& info);

// Reverse order, and only for modules that initialised. Never from DLL_PROCESS_DETACH: module
// shutdowns join threads and call the engine through pointers cached while it was alive.
void ShutdownAll();

// The engine's own shutdown call (RadPluginShutdown): the game thread, outside the loader lock, before
// the process exits. A hook finishes work on the module's OWN state -- closing a file -- inside
// `budgetMs`, and calls nothing in the engine, whose objects may already be gone.
using PreExitFn = void (*)(uint32_t budgetMs);

// Registering the same hook twice keeps one. False when the table is full.
bool RegisterPreExit(PreExitFn fn);

// Registration order; each hook is handed what is left of the one budget.
void RunPreExit(uint32_t budgetMs);

void ResetForTest();
// Appends a module the way REPLAY_MODULE does, so the ordering and failure paths can be driven from a
// test without a translation unit per case. ResetForTest clears everything a static registrar added.
void RegisterForTest(const ReplayModule& mod);

}  // namespace modreg
}  // namespace replay
