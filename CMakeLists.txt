cmake_minimum_required(VERSION 3.20)
project(gba_emulator LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # generates compile_commands.json for clangd/LSP

if(NOT CMAKE_BUILD_TYPE)
    # An unoptimized (Debug) build of the CPU interpreter can't sustain
    # real GBA speed (~59.7fps) on typical hardware - measured ~32fps vs
    # ~131fps for the exact same code built Release, which is what the
    # "the game runs choppy/slow" reports traced back to. Release keeps
    # -DNDEBUG (this project doesn't rely on assert() for control flow,
    # so that's safe) - pass -DCMAKE_BUILD_TYPE=Debug explicitly when you
    # actually want unoptimized/steppable-in-gdb builds.
    set(CMAKE_BUILD_TYPE Release)
endif()

# Keep binaries in one place regardless of subdirectory structure
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(GBA_BUILD_TESTS "Build the CPU/PPU test-ROM harness" ON)
option(GBA_ENABLE_ASAN "Build with AddressSanitizer + UBSan (Debug only)" OFF)

# ---------------------------------------------------------------------------
# Compiler warnings - an emulator lives and dies by catching integer/overflow
# bugs early, so we keep this strict from day one.
# ---------------------------------------------------------------------------
add_library(gba_warnings INTERFACE)
target_compile_options(gba_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
)

if(GBA_ENABLE_ASAN AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_library(gba_sanitizers INTERFACE)
    target_compile_options(gba_sanitizers INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(gba_sanitizers INTERFACE -fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(SDL2 REQUIRED)

# ---------------------------------------------------------------------------
# Core emulator library (everything except main.cpp)
# This split matters: it lets the test harness link against the same CPU/PPU
# code the real emulator uses, without dragging SDL2/main() along.
# ---------------------------------------------------------------------------
add_library(gba_core
    src/core/cpu/cpu.cpp
    src/core/cpu/arm_data_processing.cpp
    src/core/cpu/arm_multiply.cpp
    src/core/cpu/arm_transfer.cpp
    src/core/cpu/arm_branch.cpp
    src/core/cpu/arm_psr.cpp
    src/core/cpu/thumb_alu.cpp
    src/core/cpu/thumb_transfer.cpp
    src/core/cpu/thumb_branch.cpp
    src/core/cpu/hle_bios.cpp
    src/core/io/timers.cpp
    src/core/io/dma.cpp
    src/core/memory/bus.cpp
    src/core/memory/save.cpp
    src/core/memory/gpio.cpp
    src/core/ppu/ppu.cpp
    src/core/emulator.cpp
)

target_include_directories(gba_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(gba_core PUBLIC gba_warnings)
if(TARGET gba_sanitizers)
    target_link_libraries(gba_core PUBLIC gba_sanitizers)
endif()

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
add_executable(gba_emulator
    src/main.cpp
)

target_link_libraries(gba_emulator PRIVATE
    gba_core
    SDL2::SDL2
    SDL2::SDL2main
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(GBA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Helpful summary printed at configure time
# ---------------------------------------------------------------------------
message(STATUS "gba_emulator configuration:")
message(STATUS "  Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Tests        : ${GBA_BUILD_TESTS}")
message(STATUS "  Sanitizers   : ${GBA_ENABLE_ASAN}")
