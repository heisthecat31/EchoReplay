add_library(replay_camera_writepoint OBJECT
    ${CMAKE_SOURCE_DIR}/src/engine/camera_writepoint.cpp)
target_link_libraries(replay_camera_writepoint PUBLIC replay_flags replay_bootstrap)
target_include_directories(replay_camera_writepoint PUBLIC ${CMAKE_SOURCE_DIR}/src)
