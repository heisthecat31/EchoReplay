# W4-B / FORMATS-P0. The legacy-format converter (.echoreplay, .tape -> .echodemo), DESIGN-FORMATS §7.
#
# Pure file-to-file: no engine or game-process dependency. echodemo (`convert`) and the plugin's
# convert-on-open both link it.
add_library(replay_import STATIC
    basis.cpp
    convert.cpp
    hypotheses.cpp
    infer.cpp
    normalise.cpp
    read_echoreplay.cpp
    read_tape.cpp
    readcommon.cpp
    result.cpp
    resample.cpp
    emit.cpp
    report.cpp)
target_link_libraries(replay_import PRIVATE replay_flags miniz yyjson zstd PUBLIC replay_capture)
target_include_directories(replay_import PUBLIC ${CMAKE_SOURCE_DIR}/src)
