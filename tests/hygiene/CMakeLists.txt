# The owner's Documents\EchoReplay is not scratch space: a test that writes there is a
# defect whatever else it proves. owner_folder_guard runs a suite between two listings of that
# folder and reds on anything new, so the next suite that acquires the habit is caught by a check
# rather than by the owner reading his own capture list.
add_executable(owner_folder_guard owner_folder_guard.cpp)
target_link_libraries(owner_folder_guard PRIVATE replay_flags replay_bootstrap minhook)
target_include_directories(owner_folder_guard PRIVATE ${CMAKE_SOURCE_DIR}/src)

file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/owner_folder_guard_path.txt
     CONTENT "$<TARGET_FILE:owner_folder_guard>")

# T0: it reads nothing out of echovr.exe and needs no game.
replay_add_script_check(NAME hygiene SCRIPT check.ps1)
