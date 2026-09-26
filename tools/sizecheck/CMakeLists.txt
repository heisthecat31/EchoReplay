# W2-A. SC3's instrument: bytes per second PER STREAM, computed from a real capture.
#
# Added from src/capture/CMakeLists.txt because tools/ is outside the root globs. Nothing here is a
# hardcoded denominator: every figure comes from the file it is given (S5.10), including the duration
# and the talker-seconds the VOIP allowance is measured against.
add_executable(sizecheck main.cpp)
target_link_libraries(sizecheck PRIVATE replay_flags replay_capture)
target_include_directories(sizecheck PRIVATE ${CMAKE_SOURCE_DIR}/src)

file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/sizecheck_path.txt CONTENT "$<TARGET_FILE:sizecheck>")
