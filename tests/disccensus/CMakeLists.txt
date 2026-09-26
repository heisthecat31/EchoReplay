# The live disc census: the walk, driven against a game space the test plants,
# and the live-safety claim, asserted over the source of the file that makes the claim.
#
# T0 and offline: it never launches the game and never reads the shipping exe -- the whole point of
# disccensus.cpp is that its inputs are plain memory, so a fixture can BE that memory.
replay_add_test(NAME disccensus
    SOURCES test_disccensus.cpp ${CMAKE_SOURCE_DIR}/src/engine/disccensus.cpp
    LIBS replay_bootstrap)
# The source-scan case reads src/engine/disccensus.cpp out of the CHECKOUT: the claim is about the
# file that ships, not about a copy in the build tree.
target_compile_definitions(test_disccensus PRIVATE REPLAY_REPO_ROOT="${CMAKE_SOURCE_DIR}")
