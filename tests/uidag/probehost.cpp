// Loads probe DLLs into one process, in order, the way EchoLoader does, and REPORTS what happened
// rather than judging it. check.ps1 does the judging.
//
// W1-M's copy rather than W1-D's, because the environment a probe reads is per-packet and one host
// cannot set two packets' REPLAY_PROBE_IDS. The purpose is the same and is worth restating: several
// probe DLLs share one plugins folder, every one that embeds the bootstrap tries to claim the
// instance, and only the first succeeds. A probe that files its verdict from a module Init is silently
// inert for every launch after that. This makes the condition reproducible offline so the next launch
// is spent on evidence.
//
// ★ THREE OUTCOMES, NOT TWO, and the difference is the whole point of the host:
//     ok                  the DLL loaded.
//     FAILED err=1114     ERROR_DLL_INIT_FAILED. The DLL was found and mapped, its DllMain RAN, and it
//                         returned FALSE - which for the shipped entry point means the bootstrap
//                         refused to run. That is the defect being reproduced, not an accident.
//     FAILED err=126/127  the DLL was not there at all. "It filed nothing" would then be evidence
//                         about the build, not about the probe.
// The host exits non-zero only for the third kind, so a check cannot mistake one for another.
//
// ★ It also SNAPSHOTS <root>\<stem>\done.json the instant LoadLibraryA returns. A static initialiser
// runs during DLL_PROCESS_ATTACH, so anything it wrote is on disk by then, while a sampler thread
// started in DllMain is still blocked on its own DLL_THREAD_ATTACH until the loader lock drops. That
// makes "the provisional record was filed before any measurement" observable instead of asserted.
//
// argv[1] is the probe root and the working directory; argv[2..] are DLLs to load in order.

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace {

void StemOf(const char* path, char* out, size_t bytes) {
    const char* leaf = strrchr(path, '\\');
    leaf = leaf != nullptr ? leaf + 1 : path;
    strncpy_s(out, bytes, leaf, _TRUNCATE);
    if (char* dot = strrchr(out, '.')) {
        *dot = '\0';
    }
}

// Reports `snapshot <stem> <absent|provisional|final>` for whatever is on disk right now.
void Snapshot(const char* root, const char* stem) {
    char path[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\done.json", root, stem);
    FILE* fh = nullptr;
    if (fopen_s(&fh, path, "rb") != 0 || fh == nullptr) {
        printf("snapshot %-24s absent\n", stem);
        return;
    }
    char body[8192] = {0};
    const size_t got = fread(body, 1, sizeof body - 1, fh);
    fclose(fh);
    body[got] = '\0';
    const bool provisional = strstr(body, "\"provisional\": true") != nullptr;
    const bool early = strstr(body, "static initialiser") != nullptr;
    printf("snapshot %-24s %s%s\n", stem, provisional ? "provisional" : "final",
           early ? " says-static-initialiser" : "");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: uidag_probehost <probe-root-dir> <dll> [<dll> ...]\n");
        return 2;
    }
    if (!SetCurrentDirectoryA(argv[1])) {
        printf("cannot cd to %s (%lu)\n", argv[1], GetLastError());
        return 2;
    }
    SetEnvironmentVariableA("REPLAY_PROBE_ROOT", argv[1]);
    SetEnvironmentVariableA("REPLAY_PROBE_IDS",
                            "probe_uidag=W1-M-uidag-offline;probe_uidag_mutant=W1-M-uidag-mutant");
    SetEnvironmentVariableA("REPLAY_SWEEP_ID", "offline");

    int missing = 0;
    for (int i = 2; i < argc; ++i) {
        char stem[128];
        StemOf(argv[i], stem, sizeof stem);
        SetLastError(0);
        const HMODULE loaded = LoadLibraryA(argv[i]);
        const DWORD err = loaded != nullptr ? 0 : GetLastError();
        if (loaded != nullptr) {
            printf("load %-24s ok\n", stem);
        } else {
            printf("load %-24s FAILED err=%lu\n", stem, err);
            if (err == ERROR_MOD_NOT_FOUND || err == ERROR_FILE_NOT_FOUND ||
                err == ERROR_PATH_NOT_FOUND) {
                ++missing;
            }
        }
        Snapshot(argv[1], stem);
    }
    // Let a sampler that did start reach its first read. Nothing here waits on a handle: the point is
    // the state after loading, and the probes pin themselves.
    Sleep(400);
    for (int i = 2; i < argc; ++i) {
        char stem[128];
        StemOf(argv[i], stem, sizeof stem);
        printf("after-settle ");
        Snapshot(argv[1], stem);
    }
    return missing == 0 ? 0 : 1;
}
