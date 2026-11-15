# echoreplay_reader.dll: the .echodemo decoders behind a plain C API (erv_api.h), for EchoReplayViewer.
add_library(echoreplay_reader SHARED erv_api.cpp)
target_link_libraries(echoreplay_reader PRIVATE replay_flags replay_demofile replay_capture replay_playback)
target_include_directories(echoreplay_reader PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_executable(erv_dump erv_dump.cpp)
target_link_libraries(erv_dump PRIVATE echoreplay_reader)
