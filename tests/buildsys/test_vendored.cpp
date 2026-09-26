// Exercises the vendored dependencies rather than only linking them. A link-only check would pass
// against a stub; these two run the code paths the project actually depends on.

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include <MinHook.h>
#include <zstd.h>
#include <zstd_errors.h>

#include <doctest.h>

TEST_CASE("zstd round-trips a chapter-sized buffer") {
    CHECK(ZSTD_versionNumber() == 10308);

    std::vector<uint8_t> original(256 * 1024);
    uint32_t state = 0x12345678u;
    for (size_t i = 0; i < original.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        // Deliberately compressible but not trivial: a stub that returned the input would still be
        // caught by the size check below.
        original[i] = static_cast<uint8_t>((state >> 24) & (i % 17 == 0 ? 0xFF : 0x0F));
    }

    std::vector<uint8_t> packed(ZSTD_compressBound(original.size()));
    size_t packedSize = ZSTD_compress(packed.data(), packed.size(), original.data(), original.size(), 3);
    REQUIRE_FALSE(ZSTD_isError(packedSize));
    CHECK(packedSize < original.size());

    unsigned long long declared = ZSTD_getFrameContentSize(packed.data(), packedSize);
    REQUIRE(declared == original.size());

    std::vector<uint8_t> restored(original.size());
    size_t restoredSize = ZSTD_decompress(restored.data(), restored.size(), packed.data(), packedSize);
    REQUIRE_FALSE(ZSTD_isError(restoredSize));
    REQUIRE(restoredSize == original.size());
    CHECK(restored == original);
}

namespace {

int g_detourCalls = 0;
int (*g_original)(int) = nullptr;

__declspec(noinline) int HookTarget(int value) {
    return value * 3 + 1;
}

__declspec(noinline) int HookDetour(int value) {
    ++g_detourCalls;
    return g_original(value) + 1000;
}

}  // namespace

TEST_CASE("minhook installs, redirects and removes a detour") {
    REQUIRE(MH_Initialize() == MH_OK);

    // volatile so /O2 cannot fold the calls and hide whether the detour actually ran.
    volatile int input = 5;
    REQUIRE(HookTarget(input) == 16);

    void* trampoline = nullptr;
    REQUIRE(MH_CreateHook(reinterpret_cast<void*>(&HookTarget), reinterpret_cast<void*>(&HookDetour),
                          &trampoline) == MH_OK);
    g_original = reinterpret_cast<int (*)(int)>(trampoline);
    REQUIRE(MH_EnableHook(reinterpret_cast<void*>(&HookTarget)) == MH_OK);

    CHECK(HookTarget(input) == 1016);
    CHECK(g_detourCalls == 1);
    // The trampoline must still reach the unhooked body, which is what a chained hook site relies on.
    CHECK(g_original(input) == 16);

    REQUIRE(MH_DisableHook(reinterpret_cast<void*>(&HookTarget)) == MH_OK);
    CHECK(HookTarget(input) == 16);
    CHECK(g_detourCalls == 1);

    REQUIRE(MH_RemoveHook(reinterpret_cast<void*>(&HookTarget)) == MH_OK);
    REQUIRE(MH_Uninitialize() == MH_OK);
}

// Design §5.8 makes a truncated or damaged .echodemo a recoverable case and a wrong buffer size a bug, so
// the reader has to tell them apart. That needs the typed code, not ZSTD_isError()'s bool - which means
// zstd_errors.h has to be reachable through the zstd target rather than by reaching into lib/common.
TEST_CASE("zstd reports typed error codes, not just a bool") {
    std::vector<uint8_t> original(4096, 0xA5);
    std::vector<uint8_t> packed(ZSTD_compressBound(original.size()));
    size_t packedSize = ZSTD_compress(packed.data(), packed.size(), original.data(), original.size(), 3);
    REQUIRE_FALSE(ZSTD_isError(packedSize));

    SUBCASE("our bug: the destination was sized wrong") {
        std::vector<uint8_t> tiny(16);
        size_t r = ZSTD_decompress(tiny.data(), tiny.size(), packed.data(), packedSize);
        REQUIRE(ZSTD_isError(r));
        CHECK(ZSTD_getErrorCode(r) == ZSTD_error_dstSize_tooSmall);
    }

    SUBCASE("damaged file: the frame header is not a zstd frame") {
        std::vector<uint8_t> damaged(packed.begin(), packed.begin() + packedSize);
        damaged[0] ^= 0xFF;
        std::vector<uint8_t> out(original.size());
        size_t r = ZSTD_decompress(out.data(), out.size(), damaged.data(), damaged.size());
        REQUIRE(ZSTD_isError(r));
        CHECK(ZSTD_getErrorCode(r) == ZSTD_error_prefix_unknown);
    }

    SUBCASE("damaged file: the frame stops early") {
        std::vector<uint8_t> truncated(packed.begin(), packed.begin() + (packedSize / 2));
        std::vector<uint8_t> out(original.size());
        size_t r = ZSTD_decompress(out.data(), out.size(), truncated.data(), truncated.size());
        REQUIRE(ZSTD_isError(r));
        CHECK(ZSTD_getErrorCode(r) == ZSTD_error_srcSize_wrong);
    }
}
