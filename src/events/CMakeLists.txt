# W3-E. Lobby and event decode: the readers for the message bodies W2-A already stores verbatim, and
# S5.7's JOIN / LEAVE / TEAM_CHANGE markers derived from them.
#
# A static library with no engine dependency at all. Every entry point is a pure function over a
# (pointer, length) the caller owns, which is what lets the same code run inside the capture worker and
# inside tests/events against bytes lifted from a capture taken months ago.
#
# It does NOT link replay_capture. It includes capture/markers.h for the S_MARK numbering -- one
# definition of the kinds rather than a second copy that can drift -- and that is a header dependency,
# not a link one.
add_library(replay_events STATIC
    lobbymsg.cpp
    lobbytrack.cpp)
target_link_libraries(replay_events PRIVATE replay_flags)
target_include_directories(replay_events PUBLIC ${CMAKE_SOURCE_DIR}/src)
