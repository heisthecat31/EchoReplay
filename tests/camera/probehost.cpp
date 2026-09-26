// Loads probe DLLs into one process, in order, the way EchoLoader does.
//
// It exists to reproduce, offline, the condition that cost sweep 4 three launches: several probe
// DLLs in one plugins folder, each embedding the same bootstrap, the FIRST to load winning
// coexist::ClaimInstance and every later one being refused. Under that refusal bootstrap::Attach
// returns before modreg::InitAll, so a verdict written from a module init is never written.
//
// Loading two copies of the SAME dll under different names reproduces it exactly, because the claim
// is keyed on the resolved log path plus the pid - not on the file name - so the second copy is
// refused for precisely the reason a different probe would be.
//
// argv[1] is the probe root and working directory; argv[2..] are DLLs to load in order.

#include <windows.h>

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: camera_probehost <probe-root-dir> <dll> [<dll> ...]\n");
        return 2;
    }
    if (!SetCurrentDirectoryA(argv[1])) {
        printf("cannot cd to %s (%lu)\n", argv[1], GetLastError());
        return 2;
    }

    // What the harness sets on the game process. The ids map each DLL stem to its probe id, so each
    // copy files under its own name and the test can tell them apart.
    SetEnvironmentVariableA("REPLAY_PROBE_ROOT", argv[1]);
    SetEnvironmentVariableA("REPLAY_SWEEP_ID", "offline");
    char ids[1024] = {0};
    for (int i = 2; i < argc; ++i) {
        const char* leaf = strrchr(argv[i], 0x5C);   // backslash
        const char* alt = strrchr(argv[i], '/');
        if (alt != nullptr && (leaf == nullptr || alt > leaf)) {
            leaf = alt;
        }
        leaf = leaf != nullptr ? leaf + 1 : argv[i];
        char stem[MAX_PATH];
        strncpy_s(stem, sizeof stem, leaf, _TRUNCATE);
        if (char* dot = strrchr(stem, '.')) {
            *dot = '\0';
        }
        char one[300];
        _snprintf_s(one, sizeof one, _TRUNCATE, "%s%s=W1-H-%s", ids[0] != '\0' ? ";" : "", stem,
                    stem);
        strncat_s(ids, sizeof ids, one, _TRUNCATE);
    }
    SetEnvironmentVariableA("REPLAY_PROBE_IDS", ids);
    printf("REPLAY_PROBE_IDS=%s\n", ids);

    int failures = 0;
    for (int i = 2; i < argc; ++i) {
        const HMODULE loaded = LoadLibraryA(argv[i]);
        const DWORD err = loaded != nullptr ? 0 : GetLastError();
        printf("load %-44s %s err=%lu\n", argv[i], loaded != nullptr ? "ok" : "FAILED", err);
        if (loaded == nullptr) {
            ++failures;
        }
    }

    // The probes measure on their own threads; give them room to file the real verdict over the
    // provisional one. There is no game here, so each one reaches its own bounded end: no netGame
    // ever appears, so no engine write is attempted and the worker files what it has.
    Sleep(4000);
    // Deliberately no FreeLibrary: the point is the state after loading, and the probes pin
    // themselves anyway.
    return failures == 0 ? 0 : 1;
}
