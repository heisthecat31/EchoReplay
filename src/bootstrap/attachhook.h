// Work that must happen at DllMain time, registered the way modules are.
//
// ★ WHY THIS EXISTS. Ledger row 1X.1 hooks GetCommandLineA/W so the viewer needs no special command
// line. W1-F measured the window at instruction level: in WinMain_0 the call that reaches our DllMain is
// at 0x14151D42E and the call to GetCommandLineA is at 0x14151D45C - 46 bytes of straight-line code, no
// branches. A REPLAY_MODULE cannot win that: module init runs on the worker thread, which is not started
// until the end of Attach. So the work has to be inline, and the only file that runs inline is this
// packet's.
//
// Rather than edit bootstrap.cpp for each such need - DESIGN S13.1 says dllmain.cpp is never edited again,
// and the same reasoning applies one file up - a packet registers a callback from its OWN translation
// unit and the bootstrap runs it. No file of theirs is edited by us and none of ours by them.
//
// ★ WHAT A CALLBACK MAY DO, AND IT IS VERY LITTLE. It runs under the Windows loader lock, entered from
// another DLL's DllMain. It may compute, it may touch memory the engine has not read yet, and it may
// install an inline patch. It may NOT LoadLibrary, wait on another thread, join, suspend a thread, or
// call anything that does those things for it - including MH_EnableHook, which freezes every thread in
// the process. A callback that cannot obey this belongs in a REPLAY_MODULE instead.
#pragma once

#include <stddef.h>

namespace replay {

struct AttachHook {
    const char* name;
    const char* configKey;   // its intervention-ledger key; the hook is skipped when that key is false
    bool      (*install)();
};

namespace replay_attach_detail {
struct AttachHookRegistrar {
    explicit AttachHookRegistrar(const AttachHook& hook);
};
}  // namespace replay_attach_detail

namespace attachhook {

size_t            Count();
const AttachHook* At(size_t i);

// Runs every registered hook in name order, each gated on its config key, and logs the outcome of each.
// Returns the number that installed. Called once, from bootstrap::Attach, before the worker starts.
size_t RunAll();

// True if that hook ran and returned true. For diagnostics and for the probes.
bool Installed(const char* name);

void ResetForTest();
void RegisterForTest(const AttachHook& hook);

}  // namespace attachhook
}  // namespace replay

#define REPLAY_ATTACH_PASTE_(a, b) a##b
#define REPLAY_ATTACH_NAME_(a, b) REPLAY_ATTACH_PASTE_(a, b)

// REPLAY_ATTACH_HOOK("session-cmdline", "session.cmdline_hook", replay::session::InstallCommandLineHook)
#define REPLAY_ATTACH_HOOK(NAME, CONFIG_KEY, INSTALL)                                          \
    static const ::replay::AttachHook REPLAY_ATTACH_NAME_(replay_attach_hook_, __LINE__) = {   \
        NAME, CONFIG_KEY, INSTALL};                                                            \
    static const ::replay::replay_attach_detail::AttachHookRegistrar REPLAY_ATTACH_NAME_(      \
        replay_attach_registrar_, __LINE__)(REPLAY_ATTACH_NAME_(replay_attach_hook_, __LINE__))
