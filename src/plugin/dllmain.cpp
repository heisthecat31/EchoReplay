// The entry point. Owned by W1-C and, per the design, never edited again: a product module adds
// itself with REPLAY_MODULE in its own translation unit, so twelve Wave-2 packets do not serialise here.
//
// ★ THE THREE THINGS THIS FILE EXISTS TO GET RIGHT (R11 S0.2, all verified):
//
//   1. We are loaded by dbgcore.dll - the EchoLoader hijack - from inside ITS DllMain, at WinMain
//      statement 2. The Windows loader lock is held. Install hooks, start a thread, return. Never wait,
//      never join, never LoadLibrary anything here.
//   2. Load failure is SILENT. EchoLoader discards LoadLibraryA's return value and logs nothing at all,
//      and its source is not on this machine, so no manifest and no error path can be added to it. Our
//      own attach line is the entire diagnosis, which is why it is the first statement.
//   3. There is no named export in this contract. EchoLoader does not import GetProcAddress. This DLL
//      exports nothing, deliberately (S2 F8b); W1-D adds exactly Update and RadPluginShutdown for the
//      CNSProvider slot and tests/exports pins the surface either way.
//
// Everything past "start a thread" is in src/bootstrap/.

#include <windows.h>

#include "bootstrap/bootstrap.h"
#include "engine/pawn_provision.h"

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            // Returning FALSE here would make the loader unmap us, which is the correct outcome when we
            // have refused to run - and the only outcome EchoLoader would not notice either way.
            return replay::bootstrap::Attach(self) ? TRUE : FALSE;

        case DLL_PROCESS_DETACH:
            // lpReserved is non-null when the process is terminating, and null for a FreeLibrary. The two
            // need opposite behaviour, and conflating them is how a shutdown path deadlocks.
            // A FreeLibrary unmaps our pages while the process keeps running, so the
            // vectored crash handler has to come out of the chain FIRST -- one left behind
            // dispatches the next exception of any kind into freed memory. On a process
            // exit there is nothing left to protect and nothing that may still run.
            if (reserved == nullptr) replay::PawnProvision::RemoveCrashLogger();
            replay::bootstrap::Detach(reserved != nullptr);
            return TRUE;

        default:
            return TRUE;
    }
}
