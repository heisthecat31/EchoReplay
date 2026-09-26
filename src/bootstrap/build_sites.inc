// The startup identity set: fact ids whose 20 recorded prologue bytes must be what the loaded image
// holds before this plugin touches anything. Included by buildguard.cpp with REPLAY_BUILD_SITE defined,
// and parsed by tests/ledgerkeys, which re-reads every one of them out of the real echovr.exe on disk.
//
// The name here is a fact id, never an address: buildguard.cpp expands it to facts::<ID> and
// facts::<ID>_PROLOGUE, so the address and the bytes come from one row and cannot drift apart. That is
// the shape S4.6 rule 3 asks for and the failure S2 F1 took when an address was written down twice.
//
// These ten are the surfaces this design's own entry, tick and session strategies stand on. They are not
// hook sites - we install no hook here - they are a fingerprint of the build at the places we will read.
// Per-site guarding of an actual hook is HookManager's job and happens at install time (S4.5.3 rule 4).

REPLAY_BUILD_SITE(ENTRY_WINMAIN)            // WinMain_0: statement 2 is where dbgcore.dll loads us
REPLAY_BUILD_SITE(ENTRY_PLUGIN_LOAD)        // the call that loads dbgcore.dll
REPLAY_BUILD_SITE(GAME_INITIALIZE)          // CR15Game::Initialize - creates the CNSProvider slots
REPLAY_BUILD_SITE(GAME_SINGLETON_WRITE_VA)  // the single writer of the CR15Game singleton
REPLAY_BUILD_SITE(GAME_UPDATE)              // the per-frame update the fallback tick lives beside
REPLAY_BUILD_SITE(NSPROVIDER_UPDATE)        // CNSProvider::Update - what the slot claim makes call us
REPLAY_BUILD_SITE(TICK_RUNLOOP)             // the frame loop
REPLAY_BUILD_SITE(CLI_PARSE)                // the command-line parser the session entry needs
REPLAY_BUILD_SITE(SESSION_STATE_MACHINE)    // the 7->8->9 net-game state machine
REPLAY_BUILD_SITE(GAME_SHUTDOWN)            // where the providers are destroyed
