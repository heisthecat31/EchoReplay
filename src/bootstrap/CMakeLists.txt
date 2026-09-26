# W1-C. src/bootstrap/ is L1: dllmain's worker, the log, the build guard, the hook manager, the module
# registry, config and the coexistence rules.
#
# A static library rather than sources compiled straight into echoreplay, because tests/bootstrap and
# tests/contracts have to link it - docs/INSTALL.md's split between foundation modules (own library, both
# link it) and product modules (compiled into the DLL so static self-registration survives).
add_library(replay_bootstrap STATIC
    attachhook.cpp
    bootstrap.cpp
    buildguard.cpp
    coexist.cpp
    config.cpp
    egress.cpp
    hookmgr.cpp
    inbound.cpp
    livesafety.cpp
    log.cpp
    mainprof.cpp
    md5.cpp
    modreg.cpp
    offlinelock.cpp
    offlinepatch.cpp
    paths.cpp
    sc7.cpp
    services.cpp
    sha256.cpp
    tableguard.cpp
    tickcost.cpp)
# ws2_32 is PUBLIC because the egress blocker trampolines its exports and every consumer of
# this library links the result.
target_link_libraries(replay_bootstrap PUBLIC replay_flags ws2_32 wininet PRIVATE minhook)

# src/ only. The build guard reads the registry's addresses and prologues through
# <generated/facts_generated.h>, which W0-2 now emits into the source tree, so no second root is needed.
target_include_directories(replay_bootstrap PUBLIC ${CMAKE_SOURCE_DIR}/src)

# sc7.cpp derives the patch window it subtracts from MinHook's own JMP_REL / JMP_REL_SHORT structs rather
# than typing the sizes, so the window follows the vendored copy if it ever moves. PRIVATE: this is an
# internal header of a third-party library and nothing outside this file may depend on it.
target_include_directories(replay_bootstrap PRIVATE ${CMAKE_SOURCE_DIR}/third_party/minhook/src)

target_link_libraries(echoreplay PRIVATE replay_bootstrap)

# ★ Compiled into the DLL, not into the library above. A REPLAY_MODULE's registrar is a static object that
# nothing references by name, so an archive member holding one is dropped by the linker as unused and the
# module silently never runs. Every product module belongs here for the same reason (docs/INSTALL.md).
target_sources(echoreplay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/selfcheck_module.cpp)
