# W2-P1. One registered check named after the directory; check.ps1 drives the native selftest and
# reports the total of what it asserted.
replay_add_script_check(NAME pawn)

# The SHIPPED provisioner, not a copy of its reasoning: pawn_provision.cpp arrives inside the
# replay_identity_access link edge together with identity_access.cpp, which is where the product's
# own IIdentity lives -- the test drives both the fake and the real interface defaults.
add_executable(pawn_selftest test_pawn_provision.cpp)
target_link_libraries(pawn_selftest PRIVATE replay_flags replay_bootstrap doctest_main
                      replay_identity_access)
target_include_directories(pawn_selftest PRIVATE ${CMAKE_SOURCE_DIR}/src)
# The wiring witness reads src/plugin/dllmain.cpp to assert the product
# caller of RemoveCrashLogger exists. The source root is the build system's to know.
target_compile_definitions(pawn_selftest PRIVATE REPLAY_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
