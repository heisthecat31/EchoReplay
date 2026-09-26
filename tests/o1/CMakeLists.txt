# W1-A. One check named o1, registered once: gate.ps1 keys its manifest by name, so a second
# registration under the same name would silently replace the first.
#
# The harness is not a doctest binary. It reports its own machine-readable coverage counters -- swept
# index totals, quaternion code classes, mutation divergences -- which is what check.ps1 turns into
# GATE-RESULT, and none of that fits doctest's assertion model.
add_executable(o1_harness o1.cpp)
target_link_libraries(o1_harness PRIVATE replay_flags bitio)
target_include_directories(o1_harness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The exact path, written by CMake. check.ps1 searching for the binary would be a second source of truth
# about where it lives, and the two would drift.
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/o1_harness_path.txt CONTENT "$<TARGET_FILE:o1_harness>")

replay_add_script_check(NAME o1 SCRIPT check.ps1)
