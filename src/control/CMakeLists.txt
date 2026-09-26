# W2-G. The control surfaces: the console window, the ECHOREPLAY_OPEN carrier, the HTTP routes and the
# notes sidecar.
#
# The split is the same one W2-F used for the timeline, and for the same reason: what is in
# replay_control_core runs on a machine with no game, so tests/control can sweep it.
#
# ★ WHAT THAT DOES NOT MEAN, because the word this list used to use was "pure" and it was read as
# "nothing to test at runtime". These files touch NO ENGINE MEMORY, no facade, no socket and no
# mapped image -- and that list said nothing about the FILESYSTEM or the CONSOLE, which is how
# notes.cpp (_wfopen_s, fwrite, MoveFileExW, DeleteFileW) and consoleguard.cpp
# (SetConsoleCtrlHandler, GetSystemMenu) came to be classified as needing no test of their own
# while a whole gate ran green around them. The side effects each file DOES have are named here:
#
#   command.cpp     nothing at all -- a pure function of the line it is handed
#   cmdqueue.cpp    nothing but its own storage
#   launchargs.cpp  the Win32 environment and file APIs only -- no engine, no game, no UI
#   urlquery.cpp    nothing
#   notes.cpp       THE FILESYSTEM: creates, replaces and deletes the sidecar beside a capture
#
# The one with side effects is tested against a real temporary directory; a green gate that never
# called Save() is what let a sharing violation overwrite the owner's bookmarks. The three files
# that need the game process (AllocConsole, the HTTP route registration, the module's tick) go
# straight onto the shipped DLL.
add_library(replay_control_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/command.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/cmdqueue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/launchargs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/notes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/urlquery.cpp
    # notes.cpp identifies a pre-F7 capture by the md5 of its first 64 KiB, and this is THE md5 of
    # this build -- the one checked against RFC 1321's own vectors in tests/bootstrap. A second copy
    # in this directory would be a hash nobody validated producing a plausible wrong digest. The
    # object is compiled here rather than reached through replay_bootstrap because that target
    # carries ws2_32, wininet and minhook, none of which belong in a library tests/control links.
    ${CMAKE_SOURCE_DIR}/src/bootstrap/md5.cpp)
target_link_libraries(replay_control_core PRIVATE replay_flags)
target_include_directories(replay_control_core PUBLIC ${CMAKE_SOURCE_DIR}/src)

# The in-process half lands file-by-file. A name here that has no file on disk fails CONFIGURE, not
# link, which blocks every packet's build and not just this one -- so a source is added to this list
# in the same commit that adds the file.
target_sources(echoreplay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/console.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dispatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/http.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/control_module.cpp)
target_link_libraries(echoreplay PRIVATE replay_control_core)
