// Recomputes W1-L's CMatSym constants out of the real echovr.exe and checks them against
// the values wiretap_facts.h ships.
//
// This is the offline half of the tap's known-answer control.  The DLL calls the engine's
// own CMatSym::Hash at runtime and compares the answer to these constants, so the constants
// have to be right on their own terms -- and they are computed here by a second, deliberately
// independent implementation reading the binary's tables directly.  Two derivations, one
// comparison; the design.
//
// The whole check runs offline.  It reads echovr.exe as a file and never launches it.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <doctest.h>

#include "engine/wiretap_facts.h"
#include "../local_inputs.h"

namespace {

using namespace replay::wiretap_facts;


struct Section {
    uint32_t rva, virtualSize, rawPtr, rawSize;
};

class Image {
public:
    bool Load() {
        path_ = replay_test::FindEchoVrExeA();
        if (path_.empty()) {
            return false;
        }

        std::FILE* f = nullptr;
        if (fopen_s(&f, path_.c_str(), "rb") != 0 || f == nullptr) {
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        bytes_.resize(static_cast<size_t>(size));
        const size_t got = std::fread(bytes_.data(), 1, bytes_.size(), f);
        std::fclose(f);
        if (got != bytes_.size() || bytes_.size() < 0x400) {
            return false;
        }

        const uint32_t peOff = Read<uint32_t>(0x3C);
        if (std::memcmp(&bytes_[peOff], "PE\0\0", 4) != 0) {
            return false;
        }
        const uint16_t sectionCount = Read<uint16_t>(peOff + 6);
        const uint16_t optSize      = Read<uint16_t>(peOff + 20);
        imageBase_ = Read<uint64_t>(peOff + 24 + 24);
        size_t at  = peOff + 24 + optSize;
        for (uint16_t i = 0; i < sectionCount; ++i, at += 40) {
            sections_.push_back({Read<uint32_t>(at + 12), Read<uint32_t>(at + 8),
                                 Read<uint32_t>(at + 20), Read<uint32_t>(at + 16)});
        }
        return !sections_.empty();
    }

    const std::string& Path() const { return path_; }
    uint64_t ImageBase() const { return imageBase_; }

    // Null if the VA is unmapped or lands past the section's raw data.
    const uint8_t* At(uint64_t va, size_t need) const {
        if (va < imageBase_) {
            return nullptr;
        }
        const uint64_t rva = va - imageBase_;
        for (const Section& s : sections_) {
            const uint32_t span = s.virtualSize > s.rawSize ? s.virtualSize : s.rawSize;
            if (rva < s.rva || rva >= s.rva + span) {
                continue;
            }
            const uint64_t delta = rva - s.rva;
            if (delta + need > s.rawSize || s.rawPtr + delta + need > bytes_.size()) {
                return nullptr;
            }
            return bytes_.data() + s.rawPtr + delta;
        }
        return nullptr;
    }

private:
    template <typename T>
    T Read(size_t off) const {
        T v{};
        std::memcpy(&v, bytes_.data() + off, sizeof(T));
        return v;
    }

    std::string          path_;
    std::vector<uint8_t> bytes_;
    std::vector<Section> sections_;
    uint64_t             imageBase_ = 0;
};

// A CMatSym is an 8x8 GF(2) bit matrix stored as eight row bytes, and the engine's combine
// step is the matrix product.  The giveaway is CMatSym::Hash("") -- it returns the identity
// matrix, 0x8040201008040201.
uint64_t MatMul(uint64_t a, uint64_t b) {
    uint64_t out = 0;
    for (int row = 0; row < 8; ++row) {
        const uint8_t selector = static_cast<uint8_t>(a >> (8 * row));
        uint8_t       acc      = 0;
        for (int bit = 0; bit < 8; ++bit) {
            if ((selector >> bit) & 1) {
                acc ^= static_cast<uint8_t>(b >> (8 * bit));
            }
        }
        out |= static_cast<uint64_t>(acc) << (8 * row);
    }
    return out;
}

constexpr uint64_t kIdentity = 0x8040201008040201ull;

class MatSym {
public:
    bool Load(const Image& img) {
        const uint8_t* chars = img.At(kMatSymCharTable, 128 * sizeof(uint64_t));
        if (chars == nullptr) {
            return false;
        }
        std::memcpy(charTable_, chars, sizeof(charTable_));
        return true;
    }

    uint64_t Hash(const char* s) const {
        uint64_t h = kIdentity;
        const size_t n = std::strlen(s);
        for (size_t i = n; i > 0; --i) {
            h = MatMul(charTable_[static_cast<unsigned char>(s[i - 1])], h);
        }
        return h;
    }

    // TypeId(SFoo): hash the name WITHOUT its leading 'S', then fold into the namespace.
    uint64_t TypeId(const char* structName) const {
        return MatMul(kMatSymNamespace, Hash(structName + 1));
    }

private:
    uint64_t charTable_[128]{};
};

Image& TheImage() {
    static Image img;
    static bool  loaded = img.Load();
    static bool  warned = false;
    if (!loaded && !warned) {
        warned = true;
        std::printf("test_wiretap: cannot read echovr.exe (%s) -- set ECHOVR_DIR or REPLAY_ECHOVR_EXE\n",
                    img.Path().c_str());
    }
    return img;
}

}  // namespace

TEST_CASE("the GF(2) matrix model is the right model") {
    // These hold for a matrix product and fail for almost any other 64-bit mixing function,
    // so they are what makes the constant checks below evidence rather than coincidence.
    const uint64_t sample = 0x0123456789ABCDEFull;
    CHECK(MatMul(kIdentity, sample) == sample);
    CHECK(MatMul(sample, kIdentity) == sample);

    const uint64_t a = 0xC81A5609BE7D6F06ull, b = 0x2016340AD3FA438Bull, c = 0x0F1E2D3C4B5A6978ull;
    CHECK(MatMul(MatMul(a, b), c) == MatMul(a, MatMul(b, c)));
}

TEST_CASE("echovr.exe is readable and is the build W1-L derived against") {
    const Image& img = TheImage();
    if (img.Path().empty()) {
        MESSAGE("SKIPPED: echovr.exe not found (set ECHOVR_DIR or REPLAY_ECHOVR_EXE)");
        return;
    }
    REQUIRE(img.ImageBase() == kImageBase);

    // The spread table is self-identifying: entry i has 0xFF in every byte lane whose bit is
    // set in i.  Finding that shape at this VA is what pins EVENT.MATSYM_TABLE.
    const uint8_t* spread = img.At(kMatSymTable, 256 * sizeof(uint64_t));
    REQUIRE(spread != nullptr);
    for (int i = 0; i < 256; ++i) {
        uint64_t entry = 0;
        std::memcpy(&entry, spread + i * sizeof(uint64_t), sizeof(uint64_t));
        uint64_t want = 0;
        for (int bit = 0; bit < 8; ++bit) {
            if ((i >> bit) & 1) {
                want |= 0xFFull << (8 * bit);
            }
        }
        REQUIRE(entry == want);
    }
    CHECK(kMatSymCharTable == kMatSymTable + 256 * sizeof(uint64_t));
}

TEST_CASE("the five entry-point prologues are present in the file") {
    const Image& img = TheImage();
    if (img.Path().empty()) {
        MESSAGE("SKIPPED: echovr.exe not found (set ECHOVR_DIR or REPLAY_ECHOVR_EXE)");
        return;
    }
    REQUIRE(img.ImageBase() == kImageBase);

    struct Entry {
        const char*    name;
        uint64_t       va;
        const uint8_t* want;
    };
    const Entry entries[] = {
        {"CBroadcaster::Listen", kBroadcasterListen, kListenPrologue},
        {"CBroadcaster::Unlisten", kBroadcasterUnlisten, kUnlistenPrologue},
        {"CBroadcaster::UnlistenAll", kBroadcasterUnlistenAll, kUnlistenAllPrologue},
        {"CMatSym::Hash", kMatSymHash, kMatSymHashPrologue},
        {"CMatSym combine", kMatSymCombine, kMatSymCombinePrologue},
    };
    for (const Entry& e : entries) {
        CAPTURE(e.name);
        const uint8_t* got = img.At(e.va, 20);
        REQUIRE(got != nullptr);
        CHECK(std::memcmp(got, e.want, 20) == 0);
    }

    // The namespace matrix is the imm64 of a `mov rcx, imm64` in SR15NetReplicatedDelta-
    // UnreliableVars::TypeId.  Checking the two opcode bytes as well stops a coincidental
    // 8-byte match somewhere that is not an immediate at all.
    const uint8_t* imm = img.At(0x1400A4AB5ull, 10);
    REQUIRE(imm != nullptr);
    CHECK(imm[0] == 0x48);
    CHECK(imm[1] == 0xB9);
    uint64_t ns = 0;
    std::memcpy(&ns, imm + 2, sizeof(ns));
    CHECK(ns == kMatSymNamespace);
}

TEST_CASE("the shipped message symbols recompute from the binary's own tables") {
    const Image& img = TheImage();
    if (img.Path().empty()) {
        MESSAGE("SKIPPED: echovr.exe not found (set ECHOVR_DIR or REPLAY_ECHOVR_EXE)");
        return;
    }
    REQUIRE(img.ImageBase() == kImageBase);
    MatSym matsym;
    REQUIRE(matsym.Load(img));

    CHECK(matsym.Hash("") == kIdentity);

    CHECK(matsym.TypeId(kNameDeltaUnreliableVars) == kSymDeltaUnreliableVars);
    CHECK(matsym.TypeId(kNamePlayerStateMsg) == kSymPlayerStateMsg);
    CHECK(matsym.TypeId(kNameComponentEvent) == kSymComponentEvent);

    // Two ids catalogued by a lane that never saw this derivation (R5 S4.1).  They are the
    // control that stops the three above from being self-consistent and jointly wrong.
    CHECK(matsym.TypeId("SR15NetItemEquippedNotification") == 0xED0FBA849BADC7BFull);
    CHECK(matsym.TypeId("SNSRefreshProfileFromServer") == 0x06877B856E8D785Full);
}

TEST_CASE("the symbol derivation is discriminating") {
    const Image& img = TheImage();
    if (img.Path().empty()) {
        MESSAGE("SKIPPED: echovr.exe not found (set ECHOVR_DIR or REPLAY_ECHOVR_EXE)");
        return;
    }
    REQUIRE(img.ImageBase() == kImageBase);
    MatSym matsym;
    REQUIRE(matsym.Load(img));

    // Keeping the leading 'S', and multiplying the other way round, are the two plausible
    // misreadings of the TypeId thunk.  If either also produced the right answer, matching
    // it would prove nothing about how the engine actually computes the symbol.
    CHECK(MatMul(kMatSymNamespace, matsym.Hash(kNameDeltaUnreliableVars)) != kSymDeltaUnreliableVars);
    CHECK(MatMul(matsym.Hash(kNameDeltaUnreliableVars + 1), kMatSymNamespace) != kSymDeltaUnreliableVars);
    CHECK(matsym.TypeId(kNameDeltaUnreliableVars) != matsym.TypeId(kNamePlayerStateMsg));
}

TEST_CASE("the listener node layout the tap depends on is internally consistent") {
    CHECK(kNodeSkipOff < kNodeDelegateOff);
    CHECK(kNodeDelegateOff + kDelegateSize == kNodeStride);
    CHECK(kDelegateProxyOff + sizeof(void*) == kDelegateSize);
    // The dispatcher calls through node+40 and passes node+24 as the proxy's second
    // argument; both must fall out of the offsets above rather than being written twice.
    CHECK(kNodeDelegateOff + kDelegateProxyOff == 40u);
    CHECK(kNodeDelegateOff + sizeof(void*) == 24u);
}
