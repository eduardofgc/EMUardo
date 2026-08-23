# Cross-compilation toolchain for producing a native Windows .exe from
# Linux, via MinGW-w64 (on Fedora: `sudo dnf install mingw64-gcc-c++
# mingw64-binutils mingw64-sdl2-compat`). Usage:
#
#   cmake -S . -B build-windows -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake -DGBA_BUILD_TESTS=OFF
#   cmake --build build-windows -j
#
# Tests are disabled for this build (GBA_BUILD_TESTS=OFF) because the
# resulting test binaries are Windows executables that CTest can't run
# directly on Linux - correctness is already covered by the native Linux
# build's test suite; this toolchain only exists to produce the
# Windows-native gba_emulator.exe.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_AR x86_64-w64-mingw32-ar)
set(CMAKE_RANLIB x86_64-w64-mingw32-ranlib)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32/sys-root/mingw)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
