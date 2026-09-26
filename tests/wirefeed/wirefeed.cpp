#include "wirefeed.h"

#include <cstdio>
#include <cstring>

namespace w1l {
namespace {

constexpr size_t kRecordHeader = 13;  // dir u8, ts_ns u64, len u32

bool ReadWhole(const std::string& path, std::vector<uint8_t>& out) {
    std::FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr) {
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long long size = _ftelli64(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(size));
    const size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

int HexNibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Reads the field named by `key` out of one decoded line. Returns the cursor just past the
// value, or nullptr if the field is not where the format says it is -- which is a parse
// failure and not a message to skip, because a decode line that has drifted would otherwise
// silently shorten the feed.
const char* Field(const char* at, const char* end, const char* key, uint64_t& value, int base) {
    const size_t keyLen = std::strlen(key);
    if (static_cast<size_t>(end - at) < keyLen || std::memcmp(at, key, keyLen) != 0) {
        return nullptr;
    }
    at += keyLen;
    value = 0;
    const char* start = at;
    while (at < end) {
        const int digit = HexNibble(*at);
        if (digit < 0 || digit >= base) {
            break;
        }
        value = value * static_cast<uint64_t>(base) + static_cast<uint64_t>(digit);
        ++at;
    }
    return at == start ? nullptr : at;
}

}  // namespace

bool WireFeed::Load(const std::string& dir, std::string& error) {
    std::vector<uint8_t> capture;
    if (!ReadWhole(dir + "\\arena.bin", capture)) {
        error = "cannot read " + dir + "\\arena.bin";
        return false;
    }
    captureBytes_ = capture.size();

    std::vector<uint64_t> stamps;
    stamps.reserve(40000);
    for (size_t at = 0; at + kRecordHeader <= capture.size();) {
        uint64_t ts = 0;
        uint32_t len = 0;
        std::memcpy(&ts, capture.data() + at + 1, sizeof ts);
        std::memcpy(&len, capture.data() + at + 9, sizeof len);
        at += kRecordHeader;
        if (at + len > capture.size()) {
            error = "arena.bin is truncated inside a datagram payload";
            return false;
        }
        at += len;
        stamps.push_back(ts);
    }
    if (stamps.size() < 2) {
        error = "arena.bin holds no datagrams";
        return false;
    }
    datagrams_ = stamps.size();
    spanNs_ = stamps.back() - stamps.front();
    const uint64_t base = stamps.front();

    std::vector<uint8_t> decoded;
    if (!ReadWhole(dir + "\\arena_decoded.txt", decoded)) {
        error = "cannot read " + dir + "\\arena_decoded.txt";
        return false;
    }

    blob_.reserve(decoded.size() / 4);
    messages_.reserve(120000);
    uint32_t datagram = 0;
    bool fromServer = false;
    const char* at = reinterpret_cast<const char*>(decoded.data());
    const char* fileEnd = at + decoded.size();
    while (at < fileEnd) {
        const char* newline = static_cast<const char*>(std::memchr(at, '\n', fileEnd - at));
        const char* lineEnd = newline != nullptr ? newline : fileEnd;
        if (lineEnd > at && lineEnd[-1] == '\r') {
            --lineEnd;
        }
        if (*at == '#') {
            uint64_t n = 0;
            const char* cursor = Field(at, lineEnd, "#", n, 10);
            if (cursor == nullptr || n == 0 || n > stamps.size()) {
                error = "arena_decoded.txt names a datagram arena.bin does not have";
                return false;
            }
            datagram = static_cast<uint32_t>(n);
            fromServer = (lineEnd - cursor) > 4 && std::memcmp(cursor + 1, "S2C", 3) == 0;
        } else if (*at == ' ' && datagram != 0) {
            const char* cursor = at;
            while (cursor < lineEnd && *cursor == ' ') {
                ++cursor;
            }
            uint64_t symbol = 0;
            cursor = Field(cursor, lineEnd, "sym=0x", symbol, 16);
            if (cursor != nullptr) {
                uint64_t scratch = 0;
                cursor = Field(cursor + 1, lineEnd, "id=", scratch, 10);
                cursor = cursor != nullptr ? Field(cursor + 1, lineEnd, "dst=0x", scratch, 16)
                                           : nullptr;
                cursor = cursor != nullptr ? Field(cursor + 1, lineEnd, "frame=", scratch, 10)
                                           : nullptr;
                uint64_t size = 0;
                cursor = cursor != nullptr ? Field(cursor + 1, lineEnd, "size=", size, 10)
                                           : nullptr;
                if (cursor == nullptr || static_cast<size_t>(lineEnd - cursor) < 6 ||
                    std::memcmp(cursor + 1, "body=", 5) != 0) {
                    error = "arena_decoded.txt has a message line W1-L cannot parse";
                    return false;
                }
                const char* hex = cursor + 6;
                const size_t nibbles = static_cast<size_t>(lineEnd - hex);
                if (nibbles / 2 != size) {
                    error = "arena_decoded.txt has a body whose hex length is not its size";
                    return false;
                }
                FeedMessage m{};
                m.offsetNs = stamps[datagram - 1] - base;
                m.symbol = symbol;
                m.bodyAt = static_cast<uint32_t>(blob_.size());
                m.bodyBytes = static_cast<uint32_t>(size);
                m.datagram = datagram;
                m.fromServer = fromServer;
                for (size_t i = 0; i + 1 < nibbles; i += 2) {
                    const int hi = HexNibble(hex[i]);
                    const int lo = HexNibble(hex[i + 1]);
                    if (hi < 0 || lo < 0) {
                        error = "arena_decoded.txt has a body that is not hex";
                        return false;
                    }
                    blob_.push_back(static_cast<uint8_t>(hi * 16 + lo));
                }
                messages_.push_back(m);
            }
        }
        at = newline != nullptr ? newline + 1 : fileEnd;
    }

    if (messages_.empty()) {
        error = "arena_decoded.txt yielded no messages";
        return false;
    }
    return true;
}

}  // namespace w1l
