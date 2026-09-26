# Vendored dependencies. Copied, never fetched: the tree must configure and build with no network.
# See PROVENANCE.md for source paths, versions and per-file hashes.

add_library(minhook STATIC
    minhook/src/buffer.c
    minhook/src/hook.c
    minhook/src/trampoline.c
    minhook/src/hde/hde32.c
    minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/minhook/include)
target_link_libraries(minhook PRIVATE replay_flags_vendor)

# Minimal libzstd: no dictBuilder, no legacy decoders, no deprecated zbuff, single-threaded.
# zstdmt_compress.c is left out rather than compiled dead: it carries no ZSTD_MULTITHREAD guard of its own,
# so it would emit 23 ZSTDMT_* symbols that every call site in zstd_compress.c is #ifdef'd away from.
add_library(zstd STATIC
    zstd/lib/common/debug.c
    zstd/lib/common/entropy_common.c
    zstd/lib/common/error_private.c
    zstd/lib/common/fse_decompress.c
    zstd/lib/common/pool.c
    zstd/lib/common/threading.c
    zstd/lib/common/xxhash.c
    zstd/lib/common/zstd_common.c
    zstd/lib/compress/fse_compress.c
    zstd/lib/compress/hist.c
    zstd/lib/compress/huf_compress.c
    zstd/lib/compress/zstd_compress.c
    zstd/lib/compress/zstd_double_fast.c
    zstd/lib/compress/zstd_fast.c
    zstd/lib/compress/zstd_lazy.c
    zstd/lib/compress/zstd_ldm.c
    zstd/lib/compress/zstd_opt.c
    zstd/lib/decompress/huf_decompress.c
    zstd/lib/decompress/zstd_ddict.c
    zstd/lib/decompress/zstd_decompress.c
    zstd/lib/decompress/zstd_decompress_block.c)
# zstd_errors.h is moved up beside zstd.h, which is where upstream's own `make install` puts it. A consumer
# needs ZSTD_getErrorCode() to tell "this replay file is damaged" (checksum_wrong) from "we sized a buffer
# wrong" (dstSize_tooSmall) - design §5.8's recovery path turns on exactly that. lib/common stays PRIVATE so
# the internal headers do not become part of anyone's contract.
target_include_directories(zstd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/zstd/lib)
target_include_directories(zstd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/zstd/lib/common)
target_compile_definitions(zstd PRIVATE ZSTD_LEGACY_SUPPORT=0 XXH_NAMESPACE=ZSTD_)
target_link_libraries(zstd PRIVATE replay_flags_vendor)

add_library(doctest_main STATIC doctest/doctest_main.cpp)
target_include_directories(doctest_main PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/doctest)
target_link_libraries(doctest_main PRIVATE replay_flags)

# miniz: zip central directory + streaming inflate for .echoreplay (DESIGN-FORMATS §7.2). Read-only subset;
# MINIZ_NO_TIME changes mz_zip_archive_file_stat's layout, so the definitions are PUBLIC.
add_library(miniz STATIC miniz/miniz.c)
target_include_directories(miniz PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/miniz)
target_compile_definitions(miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ARCHIVE_WRITING_APIS
    MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
target_link_libraries(miniz PRIVATE replay_flags_vendor)

# yyjson: the /session JSON reader (DESIGN-FORMATS §7.2). Reader only.
add_library(yyjson STATIC yyjson/yyjson.c)
target_include_directories(yyjson PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/yyjson)
target_compile_definitions(yyjson PUBLIC YYJSON_DISABLE_WRITER=1 YYJSON_DISABLE_UTILS=1)
target_link_libraries(yyjson PRIVATE replay_flags_vendor)
