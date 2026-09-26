# Appendix B.13's freeze gate: "A contract is frozen when it compiles." T0, so an undefined type in a
# frozen signature is a build failure on the next build rather than a Wave-2 discovery.
replay_add_test(NAME contracts
    SOURCES contracts_compile.cpp contracts_use.cpp
    LIBS replay_bootstrap)
target_include_directories(test_contracts PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
