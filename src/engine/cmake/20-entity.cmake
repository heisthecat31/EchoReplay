target_sources(echoreplay_core PRIVATE ${CMAKE_SOURCE_DIR}/src/engine/entity_module.cpp)

target_sources(echoreplay_core PRIVATE ${CMAKE_SOURCE_DIR}/src/engine/entity_spanproof.cpp)

target_sources(echoreplay_core PRIVATE ${CMAKE_SOURCE_DIR}/src/engine/entity_emote.cpp)

add_library(replay_entity_module OBJECT
    ${CMAKE_SOURCE_DIR}/src/engine/entity_module.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/entity_spanproof.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/entity.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/asseen.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/entity_subject.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/entity_emote.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/identity_access.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/pawn_provision.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/entity_json.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/disccensus.cpp
    ${CMAKE_SOURCE_DIR}/src/camera/camera_control.cpp)
target_link_libraries(replay_entity_module PUBLIC replay_flags replay_bootstrap minhook ws2_32)
target_include_directories(replay_entity_module PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_sources(echoreplay_core PRIVATE ${CMAKE_SOURCE_DIR}/src/engine/entity_json.cpp)

add_library(replay_identity_access OBJECT
    ${CMAKE_SOURCE_DIR}/src/engine/identity_access.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/pawn_provision.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/entity_spanproof.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/entity_json.cpp)
target_link_libraries(replay_identity_access PUBLIC replay_flags replay_bootstrap minhook ws2_32)
target_include_directories(replay_identity_access PUBLIC ${CMAKE_SOURCE_DIR}/src)
