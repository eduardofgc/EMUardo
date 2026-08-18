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

add_executable(gba_tests_ppu_mode3
    test_ppu_mode3.cpp
)
target_link_libraries(gba_tests_ppu_mode3 PRIVATE gba_core)
add_test(NAME ppu_mode3_test COMMAND gba_tests_ppu_mode3)

add_executable(gba_tests_irq_timer_dma
    test_interrupts_timers_dma.cpp
)
target_link_libraries(gba_tests_irq_timer_dma PRIVATE gba_core)
add_test(NAME irq_timer_dma_test COMMAND gba_tests_irq_timer_dma)

add_executable(gba_tests_ppu_mode0
    test_ppu_mode0.cpp
)
target_link_libraries(gba_tests_ppu_mode0 PRIVATE gba_core)
add_test(NAME ppu_mode0_test COMMAND gba_tests_ppu_mode0)

add_executable(gba_tests_hle_irq
    test_hle_bios_irq_trampoline.cpp
)
target_link_libraries(gba_tests_hle_irq PRIVATE gba_core)
add_test(NAME hle_irq_trampoline_test COMMAND gba_tests_hle_irq)

add_executable(gba_tests_hle_swi
    test_hle_bios_swi.cpp
)
target_link_libraries(gba_tests_hle_swi PRIVATE gba_core)
add_test(NAME hle_swi_test COMMAND gba_tests_hle_swi)

add_executable(gba_tests_keypad
    test_keypad.cpp
)
target_link_libraries(gba_tests_keypad PRIVATE gba_core)
add_test(NAME keypad_test COMMAND gba_tests_keypad)

add_executable(gba_tests_ppu_mode4
    test_ppu_mode4.cpp
)
target_link_libraries(gba_tests_ppu_mode4 PRIVATE gba_core)
add_test(NAME ppu_mode4_test COMMAND gba_tests_ppu_mode4)

add_executable(gba_tests_ppu_affine
    test_ppu_affine.cpp
)
target_link_libraries(gba_tests_ppu_affine PRIVATE gba_core)
add_test(NAME ppu_affine_test COMMAND gba_tests_ppu_affine)
