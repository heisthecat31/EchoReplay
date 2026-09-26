#pragma once

#include <stdint.h>

// `echodemo rows`: decoded records as CSV, read through the playback decoders (DESIGN-FORMATS 7.9).
namespace replay::rows {

enum class What : uint8_t { Player, Body, Event, Netvar, GameState, Roster };

// Times are on the capture axis, in microseconds; [fromUs, toUs] filters by that time.
int CmdRows(const wchar_t* path, What what, uint64_t fromUs, uint64_t toUs);

// argv after the verb and the file: --what W [--from s] [--to s]. Returns -1 on a usage error.
int RunRows(const wchar_t* path, int argc, wchar_t** argv);

}  // namespace replay::rows
