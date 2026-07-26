# Placeholder test harness. Once the CPU decode tables exist, this is where
# a known-good ARM/Thumb instruction test ROM gets loaded and its results
# diffed against expected register/flag state - see README.md for the
# validation strategy this project is following.
add_executable(gba_tests
    test_cpu_arm.cpp
)

target_link_libraries(gba_tests PRIVATE gba_core)

add_test(NAME placeholder COMMAND gba_tests)
