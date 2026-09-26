# W1-B. Added by src/foundation/demofile/CMakeLists.txt; the root globs cover src/ and tests/ only.
add_executable(echodemo main.cpp rows.cpp)
target_link_libraries(echodemo PRIVATE replay_flags replay_demofile replay_capture replay_playback replay_import)
target_include_directories(echodemo PRIVATE ${CMAKE_SOURCE_DIR}/src)
