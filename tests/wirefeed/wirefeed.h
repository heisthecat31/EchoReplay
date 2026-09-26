#pragma once

// R4's captured Echo Arena session, as a replayable message stream.
//
// `arena.bin` is the raw datagram tee -- [dir u8][ts_ns u64][len u32][payload] per record --
// and is the authority for timing. `arena_decoded.txt` is R4's full decode of the same
// datagrams, one indented line per message carrying the CMatSym symbol, the body length and
// the body bytes. This joins them on the datagram ordinal, so every message carries the wire
// timestamp of the datagram it arrived in.
//
// Nothing here needs a socket or a server. The same bytes arrive in the same order at the same
// intervals on every run, which is the property a live match cannot offer.

#include <cstdint>
#include <string>
#include <vector>

namespace w1l {

struct FeedMessage {
    uint64_t offsetNs;  // from the first datagram in the capture
    uint64_t symbol;
    uint32_t bodyAt;  // into Blob()
    uint32_t bodyBytes;
    uint32_t datagram;  // 1-based, as arena_decoded.txt numbers them
    bool     fromServer;
};

class WireFeed {
public:
    // `dir` holds arena.bin and arena_decoded.txt. False with `error` set if either is missing
    // or does not parse; a feed that could not be read must not read as an empty one.
    bool Load(const std::string& dir, std::string& error);

    const std::vector<FeedMessage>& Messages() const { return messages_; }
    const uint8_t* Body(const FeedMessage& m) const { return blob_.data() + m.bodyAt; }

    uint64_t Datagrams() const { return datagrams_; }
    uint64_t SpanNs() const { return spanNs_; }
    uint64_t CaptureBytes() const { return captureBytes_; }

private:
    std::vector<FeedMessage> messages_;
    std::vector<uint8_t>     blob_;
    uint64_t                 datagrams_ = 0;
    uint64_t                 spanNs_ = 0;
    uint64_t                 captureBytes_ = 0;
};

}  // namespace w1l
