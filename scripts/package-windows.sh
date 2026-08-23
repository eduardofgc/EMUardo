#!/usr/bin/env bash
# Cross-compiles gba_emulator.exe via MinGW-w64 and stages a distributable
# Windows folder (dist-windows/): the exe, every DLL it actually needs at
# runtime, and an empty roms/ folder ready for the user to drop .gba files
# into. Zip dist-windows/ and that's the whole distributable.
#
# Requires (Fedora): sudo dnf install mingw64-gcc-c++ mingw64-binutils mingw64-sdl2-compat
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

mingw_bin="/usr/x86_64-w64-mingw32/sys-root/mingw/bin"
if [[ ! -d "$mingw_bin" ]]; then
    echo "error: MinGW-w64 sysroot not found at $mingw_bin - install mingw64-gcc-c++ mingw64-binutils mingw64-sdl2-compat first" >&2
    exit 1
fi

cmake -S . -B build-windows -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake -DGBA_BUILD_TESTS=OFF
cmake --build build-windows -j

rm -rf dist-windows
mkdir -p dist-windows/roms
cp build-windows/bin/gba_emulator.exe dist-windows/

# SDL2.dll here is sdl2-compat's shim, which loads SDL3.dll at runtime
# (a dynamic LoadLibrary call, not a link-time PE import - it won't show
# up if you objdump -p the exe looking for what to bundle). libwinpthread
# comes from our own C++ threading use, not from SDL.
for dll in SDL2.dll SDL3.dll libwinpthread-1.dll; do
    cp "$mingw_bin/$dll" dist-windows/
done

echo "Packaged: dist-windows/ ($(du -sh dist-windows | cut -f1))"
