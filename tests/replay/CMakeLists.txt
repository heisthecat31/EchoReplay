# P3's gate check. The probe_replay TARGET is defined in the top-level CMakeLists.txt, not here: it is
# derived from echoreplay's SOURCES property, and that property is only complete after every product
# subdirectory has appended to it. This directory holds the observer translation unit the target names,
# the launch descriptor, and the check that asserts the derivation still holds.
#
# OWNER W2-R -- the design grants tests/replay/ to W2-R, and this marker states the same
# id where tools/packet_owners.ps1 also looks for it. The top-level CMakeLists.txt block that defines
# the probe_replay target belongs to W0-1 and lands in the commit immediately after this one.
replay_add_script_check(NAME replay)

# The arms, driven offline. Every input these rows read only exists inside a running game, so the
# battery is proved against hand-written producer lines and hand-filled status structs before a
# launch is spent on it. observer_rows.h is header-only and pulls in no product code, so this test
# links nothing: a driver that needed the engine could not run in T0.
replay_add_test(NAME replay
    SOURCES test_observer.cpp)

# The ARCH-12 4.3 golden lives in the SOURCE tree: it is W1-E's committed artifact and this
# suite reads it READ-ONLY, so the case that refuses its absence has to name the checkout it
# belongs to rather than the build directory (tests/entity/CMakeLists.txt defines the same
# name for the writing side).
target_compile_definitions(test_replay PRIVATE REPLAY_REPO_ROOT="${CMAKE_SOURCE_DIR}")
