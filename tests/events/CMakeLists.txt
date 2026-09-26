# W3-E. One registered check named after the directory.
#
# T0 rather than T1: every input is a byte array committed beside the test, so the whole check runs on a
# machine with no game, no capture and no corpus on disk. That is the property S13.6.1 is arguing for
# when it says a decoder can be written later against files that already exist -- and it is only true if
# the decoder's checks do not need the live thing either.
replay_add_script_check(NAME events)

add_executable(events_selftest test_events.cpp)
# replay_capture is linked for ONE case: the seam this packet hands to W2-A, checked against the real
# tap table rather than a copy of it. src/events itself does not link it.
target_link_libraries(events_selftest PRIVATE replay_flags doctest_main replay_events replay_capture)
# The fixture header sits beside the test rather than in src/: it is evidence, not product.
target_include_directories(events_selftest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})

# The real-corpus arm. Its input is the 16 MB decode of arena.bin, which lives outside the repository,
# so absence is NOT RUN naming the path -- never a pass. It exists because the committed fixtures are a
# SAMPLE of that corpus and a sample cannot show that a framing holds for every instance: this arm runs
# the same decoders over all 97,779 messages and reports what refused.
add_executable(events_corpus corpus.cpp)
target_link_libraries(events_corpus PRIVATE replay_flags replay_events)
target_include_directories(events_corpus PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
