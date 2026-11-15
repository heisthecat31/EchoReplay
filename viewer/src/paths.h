// Where the viewer finds the files it reads at run time.
//
// A release puts everything beside EchoReplayViewer.exe: echodemo.exe (converts .echoreplay/.tape), and
// EchoConstruct's registry\level_roster.tsv and recon\name_recovery\ (map names). A source build has none
// of that beside the exe, so it falls back to the EchoReplay and EchoConstruct folders it was built from.
#pragma once

#include <windows.h>

#include <filesystem>

namespace viewer_paths {

inline std::filesystem::path ExeFolder() {
    wchar_t b[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH);
    return n == 0 || n >= MAX_PATH ? std::filesystem::path{} : std::filesystem::path(b).parent_path();
}

// The folder holding registry\ and recon\: beside the exe in a release, else the EchoConstruct checkout.
inline std::filesystem::path EchoConstructData() {
    std::error_code ec;
    const std::filesystem::path here = ExeFolder();
    if (std::filesystem::exists(here / "registry" / "level_roster.tsv", ec)) return here;
    return std::filesystem::path(ERV_ECHOCONSTRUCT_DIR);
}

// echodemo.exe: beside the exe in a release, else EchoReplay's build\bin.
inline std::filesystem::path EchodemoExe() {
    std::error_code ec;
    const std::filesystem::path here = ExeFolder() / "echodemo.exe";
    if (std::filesystem::exists(here, ec)) return here;
    return std::filesystem::path(ERV_ECHOREPLAY_BIN) / "echodemo.exe";
}

}  // namespace viewer_paths
