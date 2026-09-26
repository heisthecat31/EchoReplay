// FIPS 180-4 SHA-256, streaming, no dependencies.
//
// Here rather than behind bcrypt.dll for the reason src/bootstrap/md5.h gives: a static import of
// bcrypt would be mapped while the Windows loader lock is held by the DLL loading us, and this entry
// point never LoadLibrary's anything inline.
//
// It is NOT a second copy of md5.h. MD5 identifies a build and agrees with the registry, INSTALL.md
// and the research lanes; this one exists for the egress record's destination tokens, where the token
// must not be invertible by anyone who can read the record. A reimplemented hash that is subtly wrong
// produces a plausible fictional digest, so tests/bootstrap checks it against FIPS 180-4's own
// published vectors before the redactor is allowed to depend on it.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace replay {

constexpr size_t kSha256HexChars = 65;   // 64 digits and the terminator

class Sha256 {
public:
    static constexpr size_t kBlockBytes = 16 * sizeof(uint32_t);
    static constexpr size_t kDigestBytes = 8 * sizeof(uint32_t);

    Sha256();
    void Update(const void* data, size_t len);
    void Final(uint8_t digest[kDigestBytes]);

    // Lowercase hex, NUL-terminated. `out` must hold kSha256HexChars.
    static void ToHex(const uint8_t digest[kDigestBytes], char* out);

private:
    void Block(const uint8_t* p);

    uint32_t state_[8];
    uint64_t bits_;
    uint8_t  buffer_[kBlockBytes];
    size_t   pending_;
};

}  // namespace replay
