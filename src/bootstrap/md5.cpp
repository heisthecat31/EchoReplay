#include "bootstrap/md5.h"

#include <windows.h>

#include <cstring>

namespace replay {
namespace {

constexpr size_t kBlock = Md5::kBlockBytes;
constexpr size_t kRounds = Md5::kRounds;

const uint32_t kT[kRounds] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u,
    0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u,
    0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du,
    0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
    0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
    0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u,
    0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu,
    0xeb86d391u};

const uint32_t kShift[kRounds] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

uint32_t Rotl(uint32_t v, uint32_t n) {
    return (v << n) | (v >> (32u - n));
}

uint32_t Load32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void Store32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

}  // namespace

Md5::Md5() : bits_(0), buffered_(0) {
    state_[0] = 0x67452301u;
    state_[1] = 0xefcdab89u;
    state_[2] = 0x98badcfeu;
    state_[3] = 0x10325476u;
    std::memset(buffer_, 0, sizeof buffer_);
}

void Md5::Block(const uint8_t* p) {
    uint32_t m[16];
    for (uint32_t i = 0; i < 16u; ++i) {
        m[i] = Load32(p + i * 4u);
    }
    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];

    for (uint32_t i = 0; i < kRounds; ++i) {
        uint32_t f;
        uint32_t g;
        if (i < 16u) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32u) {
            f = (d & b) | (~d & c);
            g = (5u * i + 1u) % 16u;
        } else if (i < 48u) {
            f = b ^ c ^ d;
            g = (3u * i + 5u) % 16u;
        } else {
            f = c ^ (b | ~d);
            g = (7u * i) % 16u;
        }
        const uint32_t tmp = d;
        d = c;
        c = b;
        b = b + Rotl(a + f + kT[i] + m[g], kShift[i]);
        a = tmp;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
}

void Md5::Update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bits_ += static_cast<uint64_t>(len) * 8ull;

    if (buffered_ != 0) {
        const size_t want = kBlock - buffered_;
        const size_t take = len < want ? len : want;
        std::memcpy(buffer_ + buffered_, p, take);
        buffered_ += take;
        p += take;
        len -= take;
        if (buffered_ < kBlock) {
            return;
        }
        Block(buffer_);
        buffered_ = 0;
    }
    while (len >= kBlock) {
        Block(p);
        p += kBlock;
        len -= kBlock;
    }
    if (len != 0) {
        std::memcpy(buffer_, p, len);
        buffered_ = len;
    }
}

void Md5::Final(uint8_t digest[16]) {
    const uint64_t bits = bits_;
    uint8_t pad = 0x80u;
    Update(&pad, 1);
    pad = 0;
    while (buffered_ != 56u) {
        Update(&pad, 1);
    }
    uint8_t tail[sizeof(uint64_t)];
    Store32(tail, static_cast<uint32_t>(bits));
    Store32(tail + 4, static_cast<uint32_t>(bits >> 32));
    Update(tail, sizeof tail);

    for (uint32_t i = 0; i < 4u; ++i) {
        Store32(digest + i * 4u, state_[i]);
    }
}

void Md5::ToHex(const uint8_t digest[16], char* out) {
    static const char kHex[] = "0123456789abcdef";
    for (uint32_t i = 0; i < 16u; ++i) {
        out[i * 2u] = kHex[digest[i] >> 4];
        out[i * 2u + 1u] = kHex[digest[i] & 0x0Fu];
    }
    out[kMd5HexChars - 1] = '\0';
}

bool Md5File(const wchar_t* path, char* out, uint64_t* sizeOut) {
    // FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, because the file we hash is normally the
    // running executable and the loader holds it open.
    HANDLE file = CreateFileW(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    Md5 md5;
    uint64_t total = 0;
    // On the worker thread's stack: hashing the executable must not allocate, and a static buffer would
    // make this function unusable from two threads at once.
    uint8_t chunk[1024 * kBlock];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, chunk, static_cast<DWORD>(sizeof chunk), &read, nullptr)) {
            CloseHandle(file);
            return false;
        }
        if (read == 0) {
            break;
        }
        md5.Update(chunk, read);
        total += read;
    }
    CloseHandle(file);

    uint8_t digest[16];
    md5.Final(digest);
    Md5::ToHex(digest, out);
    if (sizeOut != nullptr) {
        *sizeOut = total;
    }
    return true;
}

}  // namespace replay
