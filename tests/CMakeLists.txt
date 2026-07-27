# CPU instruction-accuracy smoke tests. Each links directly against
# gba_core so it exercises the exact same decoder the real emulator uses -
# see README.md for the broader validation strategy this project follows.
add_executable(gba_tests
    test_cpu_arm.cpp
)
target_link_libraries(gba_tests PRIVATE gba_core)
add_test(NAME arm_smoke_test COMMAND gba_tests)

add_executable(gba_tests_thumb
    test_cpu_thumb.cpp
)
target_link_libraries(gba_tests_thumb PRIVATE gba_core)
add_test(NAME thumb_smoke_test COMMAND gba_tests_thumb)
