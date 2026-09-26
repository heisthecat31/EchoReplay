target_sources(echoreplay PRIVATE ${CMAKE_SOURCE_DIR}/src/engine/gamestate.cpp)
target_sources(echoreplay PRIVATE ${CMAKE_SOURCE_DIR}/src/engine/gamestate_live.cpp)

add_library(replay_gamestate OBJECT
    ${CMAKE_SOURCE_DIR}/src/engine/gamestate.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/gamestate_live.cpp)
target_link_libraries(replay_gamestate PUBLIC replay_flags replay_bootstrap minhook ws2_32)
target_include_directories(replay_gamestate PUBLIC ${CMAKE_SOURCE_DIR}/src)

add_library(replay_tick_policy OBJECT ${CMAKE_SOURCE_DIR}/src/engine/tick_policy.cpp)
target_link_libraries(replay_tick_policy PUBLIC replay_flags replay_bootstrap)
target_include_directories(replay_tick_policy PUBLIC ${CMAKE_SOURCE_DIR}/src)

add_library(replay_tick OBJECT
    ${CMAKE_SOURCE_DIR}/src/engine/tick.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/tick_policy.cpp)
target_link_libraries(replay_tick PUBLIC replay_flags replay_bootstrap minhook)
target_include_directories(replay_tick PUBLIC ${CMAKE_SOURCE_DIR}/src)
