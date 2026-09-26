# W2-H. One registered check named after the directory; check.ps1 drives the arms and reports what
# each actually asserted.
#
# T0 rather than T1: every arm here runs with no game, no sound card and no launch, so it belongs in
# the tier that gates a commit rather than the one that gates a sweep.
replay_add_script_check(NAME audio)

# The offline half: the rate ladder, the A/V bias, the O4 verdict, the pin codec, the S_VOIP record,
# the memo walk against a synthetic CSoundPlayer, and the residue battery.
add_executable(audio_selftest test_audio.cpp)
target_link_libraries(audio_selftest PRIVATE replay_flags doctest_main replay_audio)
target_include_directories(audio_selftest PRIVATE ${CMAKE_SOURCE_DIR}/src)

# The export arm. It parses echovr.exe's own export directory and fails on any decorated name
# akbind.cpp asks for and cannot find -- which is what makes the design's "offset-free" claim a
# check rather than a slogan. It takes the binary's path as an argument and reports NOT RUN naming
# the path when it is absent, because a missing binary is a gate result and not a pass.
# The wiring arm: the shipping module TU through modreg::InitAll / TickAll, the way the DLL runs
# it, against a fake transport and a counting IAudioRate. audio_selftest proves the driver's
# policy; this proves the tick actually calls it, which no unit test of the driver can see.
add_executable(audio_wiretest test_audio_wire.cpp ${CMAKE_SOURCE_DIR}/src/audio/audio_module.cpp)
target_link_libraries(audio_wiretest PRIVATE replay_flags doctest_main replay_audio replay_bootstrap)
target_include_directories(audio_wiretest PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_executable(audio_exportcheck exportcheck.cpp)
target_link_libraries(audio_exportcheck PRIVATE replay_flags replay_audio)
target_include_directories(audio_exportcheck PRIVATE ${CMAKE_SOURCE_DIR}/src)

# The ledger's table and record shape, printed by the code that owns them. check.ps1 asks this
# binary for the scored-check ceiling, the names and the reserved words instead of parsing
# checkledger.h with a regex, and holds artifacts against records this binary writes through the
# probe's own FormatRecord. It compiles ledgerdump.cpp and checkledger.h and nothing else, which is
# what its freshness guard in check.ps1 compares against.
add_executable(audio_ledgerdump ledgerdump.cpp)
target_link_libraries(audio_ledgerdump PRIVATE replay_flags)

# The in-vivo arm. It is a probe DLL, not a check: the test harness installs it for one launch and
# reads the verdict it writes. It answers the two S10.4.2 proofs against real memory, the Tier-A rate
# response in both clock domains, the device's actual buffering, and the state census the pin block is
# designed around -- none of which any offline check can reach.
#
# ★ replay_bootstrap IS NAMED HERE even though replay_audio already links it PUBLIC, because this
# target now CALLS it: probe_audio.cpp's DllMain runs bootstrap::Attach, which is what arms the
# offline-bit lock the offline-local profile is refused without. A dependency a file uses directly is
# declared directly -- an inherited one can be dropped by its owner without anything here noticing.
# check.ps1 arm 5 holds this and runs\queue\W2-H.probe.json's guard declaration to each other.
add_library(probe_audio SHARED probe_audio.cpp)
target_link_libraries(probe_audio PRIVATE replay_flags replay_audio replay_bootstrap minhook ws2_32)
target_include_directories(probe_audio PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(probe_audio PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
