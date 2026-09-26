# W2-F. The replay UI: the panel, the timeline, the transport controls and the browser.
#
# timeline.cpp is split out into a static library because it is pure arithmetic with no facade call
# and no engine read in it, so tests/ui links and sweeps it on a machine with no game. Everything
# else -- the facade binding, the band-3 registration, the index and the drawing -- needs Windows
# and the mapped image, and goes straight onto the shipped DLL.
add_library(replay_ui_timeline STATIC ${CMAKE_CURRENT_SOURCE_DIR}/timeline.cpp)
target_link_libraries(replay_ui_timeline PRIVATE replay_flags)
target_include_directories(replay_ui_timeline PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_sources(echoreplay_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/bar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/gui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inframe.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/csregister.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/demoindex.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/legacyimport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/panel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/recorder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ui_module.cpp)
target_link_libraries(echoreplay_core PRIVATE replay_ui_timeline)

# FORMATS-P6/P5. Opening a .echoreplay/.tape converts it through import::Convert on a worker thread.
target_sources(echoreplay_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/importbind_linked.cpp)
target_link_libraries(echoreplay_core PRIVATE replay_import)
