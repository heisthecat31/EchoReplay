#include "import/readcommon.h"

#include <errno.h>
#include <share.h>
#include <string.h>
#include <wchar.h>

#include <ratio>

#include <miniz.h>

#include "import/reader.h"

namespace replay::import {

struct SourceStream::Zip {
    mz_zip_archive archive{};
    mz_zip_reader_extract_iter_state* entry = nullptr;
    bool entryDone = false;
};

size_t ZipReadCallback(void* opaque, uint64_t offset, void* dst, size_t n) {
    auto* stream = static_cast<SourceStream*>(opaque);
    if (_fseeki64(stream->file_, int64_t(offset), SEEK_SET) != 0) return 0;
    const size_t got = fread(dst, 1, n, stream->file_);
    if (offset + got > stream->consumed_) stream->consumed_ = offset + got;
    return got;
}

SourceStream::SourceStream() : error_(Success()) {}

SourceStream::~SourceStream() {
    if (zip_) {
        if (zip_->entry) mz_zip_reader_extract_iter_free(zip_->entry);
        mz_zip_reader_end(&zip_->archive);
    }
    if (file_) fclose(file_);
}

Result SourceStream::Open(const wchar_t* path, SourceStats& stats) {
    if (!path || !*path) return Refuse(ResultCode::BadArguments, "no source path given");
    // Shared read: Spark may still hold the recording open, and a viewer must never lock the user out.
    file_ = _wfsopen(path, L"rb", _SH_DENYNO);
    if (!file_) {
        char reason[128];
        strerror_s(reason, sizeof reason, errno);
        return Refuse(ResultCode::Io, "cannot open the source file: %s", reason);
    }
    if (_fseeki64(file_, 0, SEEK_END) != 0) return Refuse(ResultCode::Io, "cannot seek the source file");
    fileSize_ = uint64_t(_ftelli64(file_));
    rewind(file_);
    stats.sourceBytes = fileSize_;
    decodedHint_ = fileSize_;

    unsigned char magic[4] = {};
    const size_t got = fread(magic, 1, sizeof magic, file_);
    rewind(file_);
    if (got < 4 || magic[0] != 'P' || magic[1] != 'K') return Success();

    zip_ = std::make_unique<Zip>();
    mz_zip_archive& archive = zip_->archive;
    archive.m_pRead = ZipReadCallback;
    archive.m_pIO_opaque = this;
    if (!mz_zip_reader_init(&archive, fileSize_, 0)) {
        return Refuse(ResultCode::Container, "not a readable zip (%s); the file is %llu bytes, truncated or damaged",
                      mz_zip_get_error_string(mz_zip_get_last_error(&archive)),
                      static_cast<unsigned long long>(fileSize_));
    }
    stats.zipEntries = mz_zip_reader_get_num_files(&archive);
    if (stats.zipEntries == 0) return Refuse(ResultCode::Container, "the zip holds no entries");
    mz_zip_archive_file_stat entryStat;
    if (!mz_zip_reader_file_stat(&archive, 0, &entryStat) || entryStat.m_is_directory) {
        return Refuse(ResultCode::Container, "zip entry 0 is not a readable file");
    }
    decodedHint_ = entryStat.m_uncomp_size;
    zip_->entry = mz_zip_reader_extract_iter_new(&archive, 0, 0);
    if (!zip_->entry) {
        return Refuse(ResultCode::Container, "cannot open zip entry 0 (%s)",
                      mz_zip_get_error_string(mz_zip_get_last_error(&archive)));
    }
    consumed_ = 0;  // init read the central directory at the end; progress counts entry data only
    return Success();
}

size_t SourceStream::Read(void* dst, size_t capacity) {
    if (!error_.ok || capacity == 0) return 0;
    if (!zip_) {
        const size_t got = fread(dst, 1, capacity, file_);
        consumed_ += got;
        if (got == 0 && ferror(file_)) error_ = Refuse(ResultCode::Io, "read error at byte %llu", static_cast<unsigned long long>(consumed_));
        return got;
    }
    if (zip_->entryDone) return 0;
    const size_t got = mz_zip_reader_extract_iter_read(zip_->entry, dst, capacity);
    if (got > 0) return got;

    // End of entry or a failure: freeing the iterator is what verifies the size and CRC.
    const int status = zip_->entry->status;
    const bool verified = mz_zip_reader_extract_iter_free(zip_->entry) != MZ_FALSE;
    zip_->entry = nullptr;
    zip_->entryDone = true;
    if (status < 0 || !verified) {
        error_ = Refuse(ResultCode::Container, "zip entry 0 does not inflate cleanly (%s) near compressed byte %llu",
                        mz_zip_get_error_string(mz_zip_get_last_error(&zip_->archive)),
                        static_cast<unsigned long long>(consumed_));
    }
    return 0;
}

uint16_t RosterBuilder::Intern(uint64_t userId, std::string_view name, uint8_t number, uint8_t level, uint8_t team) {
    uint16_t* found = nullptr;
    if (userId != kNoUser) {
        auto it = byUser_.find(userId);
        if (it != byUser_.end()) found = &it->second;
    } else {
        auto it = byName_.find(std::string(name));
        if (it != byName_.end()) found = &it->second;
    }
    if (found) {
        SrcIdentity& known = roster_[*found];
        if (known.number == kNotInFrame) known.number = number;
        if (known.level == kNotInFrame) known.level = level;
        if (!known.name[0]) CopyText(known.name, sizeof known.name, name);
        return *found;
    }
    if (roster_.size() >= kNoSlot) return kNoSlot;

    const uint16_t index = uint16_t(roster_.size());
    SrcIdentity& id = roster_.emplace_back();
    memset(&id, 0, sizeof id);
    id.userId = userId;
    CopyText(id.name, sizeof id.name, name);
    id.number = number;
    id.level = level;
    id.team = team;
    if (userId != kNoUser) {
        byUser_.emplace(userId, index);
    } else {
        byName_.emplace(std::string(name), index);
    }
    return index;
}

void CopyText(char* dst, size_t capacity, std::string_view src) {
    if (capacity == 0) return;
    size_t n = src.size() < capacity - 1 ? src.size() : capacity - 1;
    // Never cut a UTF-8 sequence in half: back up over continuation bytes.
    if (n < src.size()) {
        while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) --n;
    }
    memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

void ParseHold(std::string_view text, Hold& hold, uint8_t& heldSlot) {
    heldSlot = kNotInFrame;
    hold = Hold::None;
    if (text == "geo") {
        hold = Hold::Geo;
    } else if (text == "disc") {
        hold = Hold::Disc;
    } else if (!text.empty() && text.size() <= 3) {
        unsigned value = 0;
        for (char c : text) {
            if (c < '0' || c > '9') return;
            value = value * 10 + unsigned(c - '0');
        }
        if (value < kNotInFrame) {
            hold = Hold::Player;
            heldSlot = uint8_t(value);
        }
    }
}

GameStatus ParseGameStatus(std::string_view text) {
    static constexpr struct {
        std::string_view name;
        GameStatus status;
    } kNames[] = {
        {"pre_match", GameStatus::PreMatch},
        {"round_start", GameStatus::RoundStart},
        {"playing", GameStatus::Playing},
        {"score", GameStatus::Score},
        {"round_over", GameStatus::RoundOver},
        {"post_match", GameStatus::PostMatch},
        {"pre_sudden_death", GameStatus::PreSuddenDeath},
        {"sudden_death", GameStatus::SuddenDeath},
        {"post_sudden_death", GameStatus::PostSuddenDeath},
    };
    for (const auto& entry : kNames) {
        if (entry.name == text) return entry.status;
    }
    return GameStatus::Empty;
}

PauseState ParsePauseState(std::string_view text) {
    if (text == "paused" || text == "autopause_replay") return PauseState::Paused;
    if (text == "unpausing") return PauseState::Unpausing;
    return PauseState::Unpaused;  // "unpaused", "none", "paused_requested" (a request, not yet a pause)
}

Result CheckModelBudget(const SourceMatch& m, uint64_t maxModelBytes) {
    if (maxModelBytes == 0) return Success();
    const uint64_t held = uint64_t(m.frames.capacity()) * sizeof(SrcFrame) +
                          uint64_t(m.players.capacity()) * sizeof(SrcPlayer) +
                          uint64_t(m.events.capacity()) * sizeof(SrcEvent);
    if (held <= maxModelBytes) return Success();
    return Refuse(ResultCode::TooLarge,
                  "the recording is too long to convert inside the game (over %llu MB of samples); "
                  "convert it with `echodemo convert` instead",
                  static_cast<unsigned long long>(maxModelBytes >> 20));
}

bool ProgressCancelled(const Progress* progress) {
    return progress && progress->cancel.load(std::memory_order_relaxed);
}

void ProgressUpdate(Progress* progress, uint64_t done, uint64_t total) {
    if (progress) progress->Report(done, total);
}

bool TooManyBadLines(uint32_t bad, uint32_t total) { return uint64_t(bad) * 100 > total; }

Result ReadSource(const wchar_t* path, SourceMatch& out, Progress* progress, uint64_t maxModelBytes) {
    const wchar_t* dot = path ? wcsrchr(path, L'.') : nullptr;
    if (dot && _wcsicmp(dot, L".echoreplay") == 0) return ReadEchoReplay(path, out, progress, maxModelBytes);
    if (dot && _wcsicmp(dot, L".tape") == 0) return ReadTape(path, out, progress, maxModelBytes);
    return Refuse(ResultCode::BadArguments, "not a .echoreplay or .tape file");
}

}  // namespace replay::import
