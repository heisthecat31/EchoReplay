#pragma once

// Protobuf wire-format reader for the tape schema (DESIGN-FORMATS §7.3). Hand-written against field
// numbers only; unknown fields are skipped by wire type. Every read is bounds-checked and fails closed:
// a malformed message ends iteration with ok() == false instead of reading past its buffer.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace replay::import::pb {

enum WireType : uint8_t { kVarint = 0, kFixed64 = 1, kLen = 2, kFixed32 = 5 };

inline bool ReadVarint(const uint8_t*& p, const uint8_t* end, uint64_t& value) {
    value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (p >= end) return false;
        const uint8_t byte = *p++;
        value |= uint64_t(byte & 0x7F) << shift;
        if (!(byte & 0x80)) return true;
    }
    return false;
}

struct Field {
    uint32_t number;
    uint8_t wire;
    uint64_t scalar;      // kVarint value, or the raw bits of kFixed32 / kFixed64
    const uint8_t* data;  // kLen payload
    size_t size;

    int32_t Int32() const { return int32_t(uint32_t(scalar)); }
    uint32_t Uint32() const { return uint32_t(scalar); }
    int64_t Int64() const { return int64_t(scalar); }
    bool Bool() const { return scalar != 0; }
    float Float() const {
        if (wire != kFixed32) return 0.0f;
        const uint32_t bits = uint32_t(scalar);
        float f;
        memcpy(&f, &bits, sizeof f);
        return f;
    }
};

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}

    // False at the end of the message or on malformed input; ok() tells the two apart.
    bool Next(Field& f) {
        if (!ok_ || p_ >= end_) return false;
        uint64_t tag;
        if (!ReadVarint(p_, end_, tag) || (tag >> 3) == 0 || (tag >> 3) > 0x1FFFFFFF) return Fail();
        f.number = uint32_t(tag >> 3);
        f.wire = uint8_t(tag & 7);
        f.data = nullptr;
        f.size = 0;
        f.scalar = 0;
        switch (f.wire) {
        case kVarint:
            if (!ReadVarint(p_, end_, f.scalar)) return Fail();
            return true;
        case kFixed64:
            if (end_ - p_ < 8) return Fail();
            memcpy(&f.scalar, p_, 8);
            p_ += 8;
            return true;
        case kFixed32: {
            if (end_ - p_ < 4) return Fail();
            uint32_t bits;
            memcpy(&bits, p_, 4);
            f.scalar = bits;
            p_ += 4;
            return true;
        }
        case kLen: {
            uint64_t len;
            if (!ReadVarint(p_, end_, len) || len > uint64_t(end_ - p_)) return Fail();
            f.data = p_;
            f.size = size_t(len);
            p_ += len;
            return true;
        }
        default:  // groups (3, 4) do not exist in proto3; anything else is not protobuf
            return Fail();
        }
    }

    bool ok() const { return ok_; }

private:
    bool Fail() {
        ok_ = false;
        return false;
    }

    const uint8_t* p_;
    const uint8_t* end_;
    bool ok_ = true;
};

}  // namespace replay::import::pb
