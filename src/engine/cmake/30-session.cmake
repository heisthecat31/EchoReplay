add_library(replay_session OBJECT
    ${CMAKE_SOURCE_DIR}/src/engine/session.cpp)
target_link_libraries(replay_session PUBLIC replay_flags replay_bootstrap)
target_include_directories(replay_session PUBLIC ${CMAKE_SOURCE_DIR}/src)
