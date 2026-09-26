add_library(replay_wiretap OBJECT
    ${CMAKE_SOURCE_DIR}/src/engine/wiretap.cpp)
target_link_libraries(replay_wiretap PUBLIC replay_flags replay_bootstrap)
target_include_directories(replay_wiretap PUBLIC ${CMAKE_SOURCE_DIR}/src)
