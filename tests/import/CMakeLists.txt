# W4-B / FORMATS-P0. The converter's contract check; later packets add their suites to this target.
replay_add_test(NAME import
    SOURCES test_contract.cpp test_facts.cpp test_rows.cpp test_emit.cpp test_basis.cpp test_normalise.cpp
            test_infer.cpp test_readers.cpp test_golden.cpp
    LIBS replay_import replay_playback replay_capture replay_demofile miniz zstd)
# The IMPORTED guard (DESIGN-FORMATS §7.7) greps the tree it was built from.
target_compile_definitions(test_import PRIVATE REPLAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_import PRIVATE psapi bcrypt)

# The facts guard reads the published measurement file; the rows case drives both tools as children.
target_compile_definitions(test_import PRIVATE
    REPLAY_NATIVE_FACTS_JSON="${CMAKE_CURRENT_SOURCE_DIR}/native_facts.json")
add_dependencies(test_import echodemo capture_mkdemo)
target_compile_definitions(test_import PRIVATE IMPORT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

# P2: the owner-file and perf probe (not a test: the owner's recordings are never in the tree).
add_executable(import_readprobe readprobe.cpp)
target_link_libraries(import_readprobe PRIVATE replay_flags replay_import psapi)

# P7: Infer on one legacy file with every rule on and thresholds overridable (tools/import/validate_infer.py).
add_executable(import_inferprobe inferprobe.cpp)
target_link_libraries(import_inferprobe PRIVATE replay_flags replay_import)
