# W1-B. The .echodemo container: layout, chapter framing, the derived index and the recovery scan.
add_library(replay_demofile STATIC
    format.cpp
    manifest_io.cpp
    writer.cpp
    reader.cpp
    strip.cpp)
target_link_libraries(replay_demofile PRIVATE replay_flags PUBLIC zstd)
target_include_directories(replay_demofile PUBLIC ${CMAKE_SOURCE_DIR}/src)

# tools/ is outside the root CMakeLists.txt globs (src/ and tests/ only), so the CLI is added from the
# module it belongs to. Both files are W1-B's, so this creates no shared edit.
add_subdirectory(${CMAKE_SOURCE_DIR}/tools/echodemo ${CMAKE_BINARY_DIR}/tools/echodemo)
