# W1-C's offline unit checks: the md5 the build guard identifies a build with, the path policy that makes
# uninstall one file, the log ring's loss accounting, the config parser and its key registry, the build
# guard's decision table, and the module registry's ordering.
replay_add_test(NAME bootstrap
    SOURCES test_md5.cpp test_paths_log.cpp test_config.cpp test_guard_modreg.cpp test_hookmgr.cpp test_offlinelock.cpp
            test_tableguard.cpp test_session_mode.cpp test_egress_redact.cpp test_inbound.cpp
            test_module_order.cpp test_detach.cpp test_livesafety.cpp test_pageprobe.cpp
            test_offlinepatch.cpp test_coexist.cpp
    LIBS replay_bootstrap)

# The offline-local probe's report builder is a header in tests/loaderprobe: the document a launch
# depends on is built by a pure function so this suite can parse it, which is what BLOCK-1 lacked.
target_include_directories(test_bootstrap PRIVATE ${CMAKE_SOURCE_DIR}/tests/loaderprobe)

# test_config.cpp's stream cross-check reads src/capture/capture.cpp and the design as text:
# the whole point is that neither table can be reached from this suite's own translation unit.
target_compile_definitions(test_bootstrap PRIVATE REPLAY_REPO_ROOT="${CMAKE_SOURCE_DIR}")
