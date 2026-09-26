#include "capture/duv.h"

#include <string.h>

namespace replay {
namespace capture {
namespace {

inline uint16_t Rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

inline uint32_t Rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t Rd64(const uint8_t* p) {
    return static_cast<uint64_t>(Rd32(p)) | (static_cast<uint64_t>(Rd32(p + 4)) << 32);
}

constexpr size_t kHeaderBytes = 24;
constexpr size_t kRowsetHeaderBytes = 6;
constexpr size_t kRowBytes = 16;

}  // namespace

const char* DuvStatusName(DuvStatus s) {
    switch (s) {
        case DuvStatus::Ok: return "ok";
        case DuvStatus::TooShort: return "shorter than the 24-byte header";
        case DuvStatus::RowsetTruncated: return "rowset section does not hold var_count rows";
        case DuvStatus::RowsetOversize: return "var_count exceeds the body";
    }
    return "unknown";
}

DuvMessage DuvParse(const void* bodyPtr, size_t bytes) {
    DuvMessage m{};
    m.status = DuvStatus::TooShort;
    const uint8_t* b = static_cast<const uint8_t*>(bodyPtr);
    if (!b || bytes < kHeaderBytes) {
        return m;
    }

    m.owner = Rd64(b);
    m.slot = Rd16(b + 8);
    m.gen = Rd16(b + 10);
    m.reserved12 = Rd32(b + 12);
    m.pktSeq = Rd32(b + 16);
    m.reserved20 = Rd32(b + 20);
    m.status = DuvStatus::Ok;

    if (bytes == kHeaderBytes) {
        // Header only. Seen once in the reference capture; it advances the packet number and carries no
        // rows, which is a different thing from a rowset of zero rows.
        return m;
    }
    if (bytes < kHeaderBytes + kRowsetHeaderBytes) {
        m.status = DuvStatus::RowsetTruncated;
        return m;
    }

    m.hasRowset = true;
    m.rowsetSeq = Rd32(b + 24);
    m.varCount = Rd16(b + 28);

    const size_t need = kHeaderBytes + kRowsetHeaderBytes + kRowBytes * size_t(m.varCount);
    if (need > bytes) {
        m.status = DuvStatus::RowsetOversize;
        m.varCount = 0;
        return m;
    }
    if (need < bytes) {
        // Extra trailing bytes are not a parse failure -- the engine sizes the datagram, not us -- but
        // the rows are still exactly where the count says they are.
        m.status = DuvStatus::Ok;
    }
    m.rows = b + kHeaderBytes + kRowsetHeaderBytes;
    return m;
}

DuvRow DuvRowAt(const DuvMessage& m, uint16_t i) {
    DuvRow r{};
    if (!m.rows || i >= m.varCount) {
        return r;
    }
    r.name = Rd64(m.rows + size_t(i) * kRowBytes);
    r.value = Rd64(m.rows + size_t(i) * kRowBytes + 8);
    return r;
}

SValueKind SValueKindOf(uint64_t v) {
    const uint32_t hi = static_cast<uint32_t>(v >> 32);
    if (hi == 0x00000212u) {
        return SValueKind::Scalar;
    }
    if ((hi >> 16) == 0xFFFFu) {
        const uint32_t lo = static_cast<uint32_t>(v);
        if (lo == 0xFFFF0500u) return SValueKind::FlagFalse;
        if (lo == 0xFFFF0501u) return SValueKind::FlagTrue;
        return SValueKind::Unset;
    }
    return SValueKind::Opaque;
}

const char* SValueKindName(SValueKind k) {
    switch (k) {
        case SValueKind::Scalar: return "scalar";
        case SValueKind::FlagFalse: return "flag-false";
        case SValueKind::FlagTrue: return "flag-true";
        case SValueKind::Unset: return "unset";
        case SValueKind::Opaque: return "opaque";
    }
    return "unknown";
}

bool DuvSequence::Note(const DuvMessage& m, Gap* gapOut) {
    if (gapOut) {
        gapOut->firstMissing = 0;
        gapOut->count = 0;
    }
    if (m.status == DuvStatus::TooShort) {
        return false;
    }

    if (!started_) {
        started_ = true;
        last_ = m.pktSeq;
        ++delivered_;
        ++chapterDelivered_;
        return true;
    }

    const uint32_t seq = m.pktSeq;
    const int64_t step = static_cast<int64_t>(static_cast<int32_t>(seq - last_));
    if (step < 0) {
        // A sequence that goes backwards is not a gap and must not be reported as one; it means the
        // numbering is not what this code believes. Counted, and the caller turns a non-zero count into
        // a completeness-unverified declaration rather than a silent green.
        ++regressions_;
        ++delivered_;
        ++chapterDelivered_;
        last_ = seq;
        return true;
    }
    if (step == 0) {
        // Two rowsets in one packet: the baseline pair does exactly this. Delivered, nothing missing.
        ++duplicates_;
        ++delivered_;
        ++chapterDelivered_;
        return true;
    }
    if (step > 1) {
        const uint32_t missed = static_cast<uint32_t>(step - 1);
        missing_ += missed;
        chapterMissing_ += missed;
        if (gapOut) {
            gapOut->firstMissing = last_ + 1;
            gapOut->count = missed;
        }
    }
    last_ = seq;
    ++delivered_;
    ++chapterDelivered_;
    return true;
}

void DuvSequence::BeginChapter() {
    chapterDelivered_ = 0;
    chapterMissing_ = 0;
}

}  // namespace capture
}  // namespace replay
