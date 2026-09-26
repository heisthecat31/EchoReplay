# W3-A. The offline half of the category-N battery: the resolve chain, the guards, the eight rows.
#
# T0 rather than T1: no file is written and no binary is read, so this belongs in the tier that gates
# every build rather than the one that needs artifacts.
replay_add_script_check(NAME residue SCRIPT check.ps1)

add_executable(residue_selftest test_residue.cpp)
target_link_libraries(residue_selftest PRIVATE replay_flags doctest_main replay_residue)
target_include_directories(residue_selftest PRIVATE ${CMAKE_SOURCE_DIR}/src)

# The exact path, written by CMake, so check.ps1 does not become a second source of truth about where
# the binary lives -- W1-A's pattern in tests/o1, and W2-B's in tests/playback.
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/residue_selftest_path.txt
     CONTENT "$<TARGET_FILE:residue_selftest>")
