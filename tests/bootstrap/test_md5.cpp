// MD5 is reimplemented in this project (src/bootstrap/md5.h explains why), and a reimplemented hash that
// is subtly wrong produces a plausible, entirely fictional digest - which for a build guard means either
// refusing every correct install or accepting every wrong one. So it is checked against RFC 1321's own
// published vectors before anything is allowed to depend on it.

#include <cstring>
#include <string>
#include <vector>

#include <doctest.h>
#include <windows.h>

#include "bootstrap/md5.h"

namespace {

std::string Hash(const void* data, size_t len) {
    replay::Md5 md5;
    md5.Update(data, len);
    uint8_t digest[16];
    md5.Final(digest);
    char hex[replay::kMd5HexChars];
    replay::Md5::ToHex(digest, hex);
    return std::string(hex);
}

std::string Hash(const std::string& s) { return Hash(s.data(), s.size()); }

}  // namespace

TEST_CASE("MD5 reproduces every RFC 1321 test vector") {
    // Appendix A.5 of RFC 1321, verbatim. All seven, not a sample: a port that is right for "abc" and
    // wrong at a block boundary is the interesting failure.
    CHECK(Hash(std::string("")) == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(Hash(std::string("a")) == "0cc175b9c0f1b6a831c399e269772661");
    CHECK(Hash(std::string("abc")) == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(Hash(std::string("message digest")) == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(Hash(std::string("abcdefghijklmnopqrstuvwxyz")) == "c3fcd3d76192e4007dfb496cca67e13b");
    CHECK(Hash(std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789")) ==
          "d174ab98d277d9f5a5611c2c9f419d9f");
    CHECK(Hash(std::string("123456789012345678901234567890123456789012345678901234567890"
                           "12345678901234567890")) == "57edf4a22be3c955ac49da2e2107b67a");
}

TEST_CASE("MD5 agrees with the million-a vector, which exercises the streaming path") {
    replay::Md5 md5;
    const std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) {
        md5.Update(chunk.data(), chunk.size());
    }
    uint8_t digest[16];
    md5.Final(digest);
    char hex[replay::kMd5HexChars];
    replay::Md5::ToHex(digest, hex);
    CHECK(std::string(hex) == "7707d6ae4e027c70eea2a935c2296f21");
}

TEST_CASE("MD5 is insensitive to how the input is split across Update calls") {
    // The buffering in Update is where a streaming hash goes wrong, and it goes wrong only at particular
    // splits - so every split of a two-block input is checked rather than a convenient one.
    std::string input;
    for (int i = 0; i < 200; ++i) {
        input += static_cast<char>('A' + (i % 26));
    }
    const std::string whole = Hash(input);
    for (size_t split = 0; split <= input.size(); ++split) {
        replay::Md5 md5;
        md5.Update(input.data(), split);
        md5.Update(input.data() + split, input.size() - split);
        uint8_t digest[16];
        md5.Final(digest);
        char hex[replay::kMd5HexChars];
        replay::Md5::ToHex(digest, hex);
        REQUIRE(std::string(hex) == whole);
    }
    CHECK(whole.size() == replay::kMd5HexChars - 1);
}

TEST_CASE("MD5 of a message whose length lands exactly on the padding boundary") {
    // 55, 56 and 57 bytes straddle the point where the length field no longer fits in the final block and
    // a second block is emitted. An implementation that pads by "if buffered > 56" rather than looping
    // gets exactly these three wrong.
    for (size_t n = 53; n <= 65; ++n) {
        const std::string input(n, 'x');
        replay::Md5 a;
        a.Update(input.data(), input.size());
        uint8_t da[16];
        a.Final(da);

        replay::Md5 b;
        for (char c : input) {
            b.Update(&c, 1);
        }
        uint8_t db[16];
        b.Final(db);
        REQUIRE(std::memcmp(da, db, sizeof da) == 0);
    }
}

TEST_CASE("Md5File hashes a file byte for byte, across the read chunk boundary") {
    wchar_t tempDir[MAX_PATH];
    wchar_t tempFile[MAX_PATH];
    REQUIRE(GetTempPathW(MAX_PATH, tempDir) != 0);
    REQUIRE(GetTempFileNameW(tempDir, L"w1c", 0, tempFile) != 0);

    // Bigger than the internal read chunk, so the loop runs more than once, and not a multiple of it.
    std::vector<uint8_t> bytes(70000 + 13);
    uint32_t state = 0x1234567u;
    for (size_t i = 0; i < bytes.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        bytes[i] = static_cast<uint8_t>(state >> 24);
    }
    HANDLE file = CreateFileW(tempFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    REQUIRE(WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE);
    CloseHandle(file);
    REQUIRE(written == bytes.size());

    char hex[replay::kMd5HexChars];
    uint64_t size = 0;
    REQUIRE(replay::Md5File(tempFile, hex, &size) == true);
    CHECK(size == bytes.size());
    CHECK(std::string(hex) == Hash(bytes.data(), bytes.size()));

    DeleteFileW(tempFile);

    // An absent file is a false, not a zero-length digest that would compare equal to an empty build.
    CHECK(replay::Md5File(tempFile, hex, &size) == false);
}
