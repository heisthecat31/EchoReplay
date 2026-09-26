# W1-D. One registered check named after the directory; check.ps1 drives the native selftest and
# tablecheck.py and reports the total of what they asserted.
replay_add_script_check(NAME gamestate)

# The SHIPPED sampler, not a copy of its reasoning. gamestate.cpp is deliberately free of engine,
# Windows and MinHook dependencies so it can be linked here and driven against a fake store; the
# live half (gamestate_live.cpp) is the only part that needs a process to be inside, and it is not
# in this link.
add_executable(gamestate_selftest test_gamestate.cpp ${CMAKE_SOURCE_DIR}/src/engine/gamestate.cpp)
target_link_libraries(gamestate_selftest PRIVATE replay_flags doctest_main)
target_include_directories(gamestate_selftest PRIVATE ${CMAKE_SOURCE_DIR}/src)
