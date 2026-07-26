# GBA Emulator

A Game Boy Advance emulator written in C++20, built as a from-scratch
exploration of the ARM7TDMI CPU, GBA memory map, and PPU rendering pipeline.

## Status

Early skeleton. Currently:
- [x] Project scaffold (CMake, SDL2 window/render loop)
- [x] Memory bus with EWRAM/IWRAM/ROM mapping and mirroring
- [ ] ARM7TDMI instruction decode/execute (ARM state)
- [ ] ARM7TDMI instruction decode/execute (Thumb state)
- [ ] Interrupts, timers, DMA
- [ ] PPU Mode 3/4 (bitmap)
- [ ] PPU Mode 0/1/2 (tiled + sprites)
- [ ] Input
- [ ] Save states
- [ ] APU

## Building

### Dependencies
- CMake >= 3.20
- A C++20 compiler (GCC 11+ / Clang 14+)
- SDL2 development headers

On Fedora:
```sh
sudo dnf install cmake gcc-c++ SDL2-devel
```

### Build
```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

### Run
```sh
./build/bin/gba_emulator path/to/rom.gba
```

Running with no ROM argument still opens the window (useful for testing the
render pipeline in isolation).

### Tests
```sh
cmake --build build -j
ctest --test-dir build
```

## Project layout

```
src/
  main.cpp              # SDL2 window/render loop, input mapping
  core/
    types.h             # integer aliases, memory map constants
    emulator.{h,cpp}     # owns Bus/Cpu/Ppu, drives frame timing
    cpu/                # ARM7TDMI interpreter
    memory/             # Bus: address decoding, region dispatch
    ppu/                # Picture Processing Unit / rendering
    apu/                # (future) audio
    io/                 # (future) I/O registers, DMA, timers, interrupts
tests/                  # instruction-accuracy test harness
```

`gba_core` is built as a separate library from `main.cpp` specifically so
the test harness can link against the exact same CPU/PPU code without
pulling in SDL2 - keep this separation as the project grows.

## Validation strategy

Rather than validating against "does Pokémon boot," the plan is to run
known-good ARM/Thumb CPU instruction test ROMs and PPU test ROMs from the
GBA homebrew/emulation-dev community, and assert against expected
register/flag/framebuffer state. This is what will go in `tests/`.

## References

- GBATEK - the primary hardware reference for the GBA (CPU, memory map, PPU,
  APU, I/O registers).
- ARM7TDMI Technical Reference Manual - instruction set details.
