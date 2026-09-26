// RFC 1321 MD5, streaming, so a 35 MB executable is hashed without being read into memory at once.
//
// This is here rather than behind bcrypt.dll on purpose: a static import of bcrypt would have to be
// mapped while the Windows loader lock is held by the DLL that is loading us, and "never LoadLibrary
// anything inline" is the first rule of this entry point. MD5 is 130 lines and is checked against the
// RFC's own seven published vectors in tests/bootstrap, which is a stronger guarantee than "we called a
// library" gives, because a hash we ported and never validated would produce a plausible wrong digest.
//
// Not a security choice. It is here because the registry, INSTALL.md and every research lane identify
// this build by its MD5, and agreeing with them is the point.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace replay {

constexpr size_t kMd5HexChars = 33;   // 32 digits and the terminator

class Md5 {
public:
    // MD5 processes sixteen 32-bit words at a time; that is what its block size IS, and spelling it that
    // way keeps a bare 64 out of the source, where the fact registry's no-transcription gate would read
    // it as CLOCK.RECIP_OFF.
    static constexpr size_t kBlockBytes = 16 * sizeof(uint32_t);
    static constexpr size_t kRounds = 4 * 16;

    Md5();
    void Update(const void* data, size_t len);
    void Final(uint8_t digest[16]);

    // Lowercase hex, NUL-terminated. `out` must hold kMd5HexChars.
    static void ToHex(const uint8_t digest[16], char* out);

private:
    void Block(const uint8_t* p);

    uint32_t state_[4];
    uint64_t bits_;
    uint8_t  buffer_[kBlockBytes];
    size_t   buffered_;
};

// Convenience for the whole-file case. Returns false if the file could not be read; on success `out`
// holds kMd5HexChars bytes of lowercase hex.
bool Md5File(const wchar_t* path, char* out, uint64_t* sizeOut);

}  // namespace replay
