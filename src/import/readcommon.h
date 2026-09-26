#pragma once

// Shared plumbing for the two source readers (packet P2): the byte stream under a file (plain or zip
// entry 0), the roster interner, and the API vocabulary both formats spell the same way.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "import/result.h"
#include "import/sourcemodel.h"

namespace replay::import {

// Decoded bytes of a source file: the file itself, or entry 0 when it is a zip. The container is sniffed
// from the first bytes, never from the extension.
class SourceStream {
public:
    SourceStream();
    ~SourceStream();
    SourceStream(const SourceStream&) = delete;
    SourceStream& operator=(const SourceStream&) = delete;

    Result Open(const wchar_t* path, SourceStats& stats);

    // Fills up to `capacity` bytes. Returns 0 at the end of the data or on failure; failed() tells which.
    size_t Read(void* dst, size_t capacity);
    bool failed() const { return !error_.ok; }
    const Result& error() const { return error_; }

    // Progress denominators: compressed bytes consumed so far, and the file size.
    uint64_t consumed() const { return consumed_; }
    uint64_t fileSize() const { return fileSize_; }
    // Inflated size of the zip entry, or the file size when it is not a zip.
    uint64_t decodedSizeHint() const { return decodedHint_; }
    bool isZip() const { return zip_ != nullptr; }

private:
    struct Zip;
    FILE* file_ = nullptr;
    std::unique_ptr<Zip> zip_;
    uint64_t fileSize_ = 0;
    uint64_t consumed_ = 0;
    uint64_t decodedHint_ = 0;
    Result error_;

    friend size_t ZipReadCallback(void*, uint64_t, void*, size_t);
};

// Roster in first-appearance order, keyed by userId (D4). Players the source gives no userId are keyed
// by name so two anonymous players stay two identities.
class RosterBuilder {
public:
    explicit RosterBuilder(std::vector<SrcIdentity>& roster) : roster_(roster) {}

    // kNoSlot when the roster is full (more identities than a uint16 index can name).
    uint16_t Intern(uint64_t userId, std::string_view name, uint8_t number, uint8_t level, uint8_t team);

private:
    std::vector<SrcIdentity>& roster_;
    std::unordered_map<uint64_t, uint16_t> byUser_;
    std::unordered_map<std::string, uint16_t> byName_;
};

void CopyText(char* dst, size_t capacity, std::string_view src);

// "none" | "geo" | "disc" | "<playerid>"; anything else reads as None.
void ParseHold(std::string_view text, Hold& hold, uint8_t& heldSlot);

// Unknown strings read as Empty / Unpaused: the API's own "no information" values.
GameStatus ParseGameStatus(std::string_view text);
PauseState ParsePauseState(std::string_view text);

// Checked where the readers poll progress, so a file can overshoot by one poll's worth of lines.
Result CheckModelBudget(const SourceMatch& m, uint64_t maxModelBytes);

bool ProgressCancelled(const Progress* progress);
void ProgressUpdate(Progress* progress, uint64_t done, uint64_t total);

// The design's corruption rule (§10): more than 1% of lines unparseable refuses the file.
bool TooManyBadLines(uint32_t bad, uint32_t total);

}  // namespace replay::import
