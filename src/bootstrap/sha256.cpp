#include "bootstrap/sha256.h"

#include <cstring>

namespace replay {
namespace {

const uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

uint32_t Ror(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }

uint32_t Load(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

}  // namespace

Sha256::Sha256() : bits_(0), pending_(0) {
    state_[0] = 0x6a09e667u;
    state_[1] = 0xbb67ae85u;
    state_[2] = 0x3c6ef372u;
    state_[3] = 0xa54ff53au;
    state_[4] = 0x510e527fu;
    state_[5] = 0x9b05688cu;
    state_[6] = 0x1f83d9abu;
    state_[7] = 0x5be0cd19u;
    std::memset(buffer_, 0, sizeof buffer_);
}

void Sha256::Block(const uint8_t* p) {
    uint32_t w[64];
    for (size_t i = 0; i < 16; ++i) {
        w[i] = Load(p + i * 4);
    }
    for (size_t i = 16; i < 64; ++i) {
        const uint32_t s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (size_t i = 0; i < 64; ++i) {
        const uint32_t s1 = Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + ch + kK[i] + w[i];
        const uint32_t s0 = Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha256::Update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bits_ += static_cast<uint64_t>(len) * 8u;
    if (pending_ != 0) {
        const size_t want = kBlockBytes - pending_;
        const size_t take = len < want ? len : want;
        std::memcpy(buffer_ + pending_, p, take);
        pending_ += take;
        p += take;
        len -= take;
        if (pending_ < kBlockBytes) {
            return;
        }
        Block(buffer_);
        pending_ = 0;
    }
    while (len >= kBlockBytes) {
        Block(p);
        p += kBlockBytes;
        len -= kBlockBytes;
    }
    if (len != 0) {
        std::memcpy(buffer_, p, len);
        pending_ = len;
    }
}

void Sha256::Final(uint8_t digest[kDigestBytes]) {
    const uint64_t bits = bits_;
    const uint8_t pad = 0x80;
    Update(&pad, 1);
    const uint8_t zero = 0;
    while (pending_ != kBlockBytes - sizeof(uint64_t)) {
        Update(&zero, 1);
    }
    uint8_t tail[sizeof(uint64_t)];
    for (size_t i = 0; i < sizeof tail; ++i) {
        tail[i] = static_cast<uint8_t>(bits >> (56u - 8u * i));
    }
    // Straight into the block: Update would add these eight bytes to the length it just encoded.
    std::memcpy(buffer_ + pending_, tail, sizeof tail);
    Block(buffer_);
    pending_ = 0;
    for (size_t i = 0; i < 8; ++i) {
        digest[i * 4 + 0] = static_cast<uint8_t>(state_[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
    }
}

void Sha256::ToHex(const uint8_t digest[kDigestBytes], char* out) {
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < kDigestBytes; ++i) {
        out[i * 2] = kHex[digest[i] >> 4];
        out[i * 2 + 1] = kHex[digest[i] & 0x0fu];
    }
    out[kDigestBytes * 2] = '\0';
}

}  // namespace replay
