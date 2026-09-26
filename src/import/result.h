#pragma once

// Outcome and progress types shared by every stage of the legacy-format converter
// (DESIGN-FORMATS §7.1, §10). Frozen by P0; append codes, never renumber.

#include <atomic>
#include <ratio>
#include <stdint.h>

namespace replay::import {

enum class ResultCode : uint16_t {
    Ok = 0,
    Container = 1,       // not zip/zstd/text, or a truncated container; message names the byte offset
    Corrupt = 2,         // more than 1% of source lines unparseable
    TapeVersion = 3,     // tape format_version other than 2; message names the number
    UnsupportedMap = 4,  // anything but mpl_arena_a; message names the map
    TooShort = 5,        // under 2 s of frames after trimming
    Basis = 6,           // mirror guard tripped on more than 0.1% of samples; names part and first frame
    Roster = 7,          // more distinct players than the capture roster can hold
    Io = 8,              // open/read/write/disk-full
    Writer = 9,          // ChapterBuilder/DemoWriter refused; its text is carried verbatim
    Cancelled = 10,      // Progress::cancel was observed
    BadArguments = 11,   // CLI usage or an option out of range
    NotImplemented = 12, // a P0 stub was reached; never valid in a shipped build
    TooLarge = 13,       // the source outgrew ConvertOptions::maxModelBytes; message names the limit
};

// Every refusal carries one human sentence: the CLI prints it, the panel shows it.
struct Result {
    bool ok;
    ResultCode code;
    char message[256];
};

Result Success();
Result Refuse(ResultCode code, const char* format, ...);
const char* ResultCodeName(ResultCode code);

// Polled, never called back: the viewer's game thread reads permille, its Cancel button sets cancel,
// and the worker checks cancel between units of work.
struct Progress {
    std::atomic<uint32_t> permille{0};
    std::atomic<bool> cancel{false};
    // Convert gives each stage a share of the bar; a stage called alone owns all of it.
    uint32_t stageBase = 0, stageSpan = std::milli::den;
    void Report(uint64_t done, uint64_t total);
};

}  // namespace replay::import
