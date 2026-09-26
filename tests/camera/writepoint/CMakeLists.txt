# W1-H's own test path. tests/camera/ is W2-E's -- the rig's checks -- and
# this directory is the S8.1.7 pose-sink fallback's, so the two are disjoint by construction and
# have one owner each.
#
# The subject is the write point's build guard: six sites, two of them at mid-function offsets,
# every one derived from a fact recorded against a 35 MB image. It is driven over a reserved
# address range, so the short-image case is reproduced offline with no echovr.exe and no engine
# code executed. camera_writepoint.cpp needs replay_bootstrap for replay::Readable -- the probe
# every check in this plugin uses before it dereferences engine memory, imported rather than
# reimplemented here.
# test_poseowner.cpp is ruling 4c's both-bound control: at most one of the stock free camera and
# this pose sink owns the pose, enforced. It drives BOTH modules' real entry points, so
# camera_control.cpp is compiled in beside the write point -- the refusal happens before either one
# resolves an engine pointer, which is what makes the check runnable with no echovr.exe.
# test_poseowner_images.cpp is the same control across the arrangement that SHIPS: the two claimants
# in two separate DLLs (AUDIT_E E3 -- in one binary the check cannot fail on a per-image arbiter,
# because in one binary there is only one image). It loads both at run time, so the libraries are
# dependencies of the test rather than inputs to it.
# test_posetransfer.cpp is ARCH-17 S3.3, both halves. TransferPoseOwner is what gives
# IReplayCamera::SetPose a reachable path at all -- camera_control holds StockFreeCam for the whole
# session, so without a transfer the write point is linked, dead code -- and Relinquish()'s re-arm is
# what keeps the owner's stock free camera alive after a commanded pose. The gate half runs over
# fabricated view states through the same detail::WriteViewStates SetPose calls, so it needs no
# echovr.exe either.
# test_viewpose.cpp is the read RULING 4b rests on: camera::ReadViewStatePose, driven over a
# constructed CR15Game so that every link of the walk has a present, an absent and an
# uncommitted case. It is the read Observe() cannot be, because Observe() answers only while this
# module is bound and the ruling is about the frames when the stock free camera holds the pose.
# test_resolvecs.cpp is ARCH-10 S4 row P4: which of the CR15NetGame's three pointers reaches
# CS.LOOKUP_BY_SYMBOL. Both rigs are mirrors of each other across +0x8 and +0x2B08, so an accessor
# reading the wrong one returns a wrong answer in one case and a wrong refusal in the other. It also
# holds the resolve case analysis ARCH-11 rule 8 moved out of tests/camera/test_camera.cpp: the path
# was W2-E'''s, the SUBJECT is src/engine/camera_writepoint.*, and an assertion is owned by its subject.
# test_cistride.cpp is CAMERA-STRIDE-GUARD's: SPECTATOR.CI_STRIDE has no immediate in the image and
# is derived in camera_writepoint_facts.h from the lea/shl factors, so the product needs a record
# outside the derivation. The subject is that header, so the assertion lives with its subject.
replay_add_test(NAME writepoint
    SOURCES test_guardsites.cpp
            test_cistride.cpp
            test_resolvecs.cpp
            test_viewpose.cpp
            test_poseowner.cpp
            test_posetransfer.cpp
            test_poseowner_images.cpp
            ${CMAKE_SOURCE_DIR}/src/camera/camera_control.cpp
    LIBS replay_bootstrap replay_camera_writepoint)
add_dependencies(test_writepoint writepoint_guardhost writepoint_guardhost_noprobe
                 poseowner_image_wp poseowner_image_ctl poseowner_image_ctl2)

# The two claimant images. One source, two targets, one difference -- the same shape as guardhost
# below -- so the driver calls an export table that is identical by construction through both.
add_library(poseowner_image_wp SHARED poseowner_image.cpp)
target_compile_definitions(poseowner_image_wp PRIVATE POSEOWNER_IMAGE_WRITEPOINT=1)
target_link_libraries(poseowner_image_wp PRIVATE replay_camera_writepoint replay_flags replay_bootstrap)
target_include_directories(poseowner_image_wp PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(poseowner_image_wp PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

add_library(poseowner_image_ctl SHARED poseowner_image.cpp
    ${CMAKE_SOURCE_DIR}/src/camera/camera_control.cpp)
target_compile_definitions(poseowner_image_ctl PRIVATE POSEOWNER_IMAGE_CONTROL=1)
target_link_libraries(poseowner_image_ctl PRIVATE replay_flags replay_bootstrap minhook ws2_32)
target_include_directories(poseowner_image_ctl PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(poseowner_image_ctl PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

# A THIRD image, claiming the SAME owner as poseowner_image_ctl (AUDIT_I finding 1). Two images
# that agree on the owner are still two images: with the claim keyed on the owner alone the second
# one is read as a re-claim, both bind, and every counter prints clean -- which is E3's harm on the
# commit that claimed to close it. Only a same-owner PAIR can make that arm fail, and the existing
# wp/ctl pair is different-owner by construction, so a third target is what the check needs.
add_library(poseowner_image_ctl2 SHARED poseowner_image.cpp
    ${CMAKE_SOURCE_DIR}/src/camera/camera_control.cpp)
target_compile_definitions(poseowner_image_ctl2 PRIVATE POSEOWNER_IMAGE_CONTROL=1)
target_link_libraries(poseowner_image_ctl2 PRIVATE replay_flags replay_bootstrap minhook ws2_32)
target_include_directories(poseowner_image_ctl2 PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(poseowner_image_ctl2 PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

# The mutation control, in a child process because the mutant's whole point is that it faults.
# Two targets, one source, one difference.
add_executable(writepoint_guardhost guardhost.cpp)
target_link_libraries(writepoint_guardhost PRIVATE replay_camera_writepoint replay_flags replay_bootstrap)
target_include_directories(writepoint_guardhost PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ★ The ONE target in the replay_camera_writepoint sweep that keeps its own copy of
# camera_writepoint.cpp, and it must. camera_writepoint.cpp:59 reads W1H_MUTATE_NO_READABLE_PROBE
# and that define is PRIVATE to this target. Linking the object library instead would hand the
# mutant the CONTROL's object -- the write point compiled WITHOUT the define -- and a mutant that
# cannot differ from its control is a check that cannot fail. The guardhost above links the library;
# this one names the file, on purpose.
add_executable(writepoint_guardhost_noprobe guardhost.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/camera_writepoint.cpp)
target_compile_definitions(writepoint_guardhost_noprobe PRIVATE W1H_MUTATE_NO_READABLE_PROBE=1)
target_link_libraries(writepoint_guardhost_noprobe PRIVATE replay_flags replay_bootstrap)
target_include_directories(writepoint_guardhost_noprobe PRIVATE ${CMAKE_SOURCE_DIR}/src)
