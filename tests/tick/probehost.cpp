// Loads probe DLLs into one process, in order, the way EchoLoader does.
//
// It exists to reproduce the condition that cost sweep 4 two launches: several probe DLLs in one
// plugins folder, each embedding the same bootstrap, the first to load winning coexist's instance
// claim and every later one being refused. A probe whose verdict is written from a module Init never
// files under that condition. This host makes that reproducible offline, so the next launch is spent
// on evidence rather than on rediscovering it.
//
// argv[1] is the probe root and the working directory; argv[2..] are DLLs to load in order.

#include <windows.h>

#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: probehost <probe-root-dir> <dll> [<dll> ...]\n");
        return 2;
    }
    if (!SetCurrentDirectoryA(argv[1])) {
        printf("cannot cd to %s (%lu)\n", argv[1], GetLastError());
        return 2;
    }
    // The harness sets these on the game process; a probe reads its own id and output directory here.
    SetEnvironmentVariableA("REPLAY_PROBE_ROOT", argv[1]);
    SetEnvironmentVariableA("REPLAY_PROBE_IDS", "probe_tickattach=W1-D-attach-offline");
    SetEnvironmentVariableA("REPLAY_SWEEP_ID", "offline");

    for (int i = 2; i < argc; ++i) {
        const HMODULE loaded = LoadLibraryA(argv[i]);
        printf("load %-40s %s\n", argv[i], loaded != nullptr ? "ok" : "FAILED");
        if (loaded == nullptr) {
            printf("  GetLastError=%lu\n", GetLastError());
            return 1;
        }
    }
    // Deliberately no FreeLibrary: the point is the state after loading, and the probes pin themselves.
    return 0;
}
