// Covers Emulator::SaveState()/LoadState(): running a deterministic
// program from a saved snapshot must reproduce bit-identical results to
// running the same stretch uninterrupted, and a truncated/corrupt buffer
// must be rejected rather than silently loading garbage.
//
// Test program (5 ARM words, infinite loop):
//   LDR R1,[PC,#8]   ; R1 = palette base (0x05000000)
//   ADD R0,R0,#1     ; loop:
//   STR R0,[R1]      ;   backdrop color = low 16 bits of R0
//   B loop
//   .word 0x05000000
//
// DISPCNT stays 0 (all BG/OBJ disabled), so every visible pixel falls
// back to the backdrop color (palette index 0) - this ties the CPU's R0
// register, through a memory write, all the way to an observable pixel
// in Ppu::Framebuffer(), so a save/load bug anywhere in that chain (CPU
// registers, Bus memory, or the PPU's own state) shows up as a framebuffer
// mismatch.

#include <cstdio>
#include <fstream>
#include <vector>

#include "core/emulator.h"
#include "core/ppu/ppu.h"
#include "core/types.h"

namespace {

int failures = 0;

void Check(bool condition, const char* label) {
    if (!condition) {
        std::printf("FAIL: %s\n", label);
        ++failures;
    }
}

bool WriteTestRom(const char* path) {
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    const std::uint32_t words[] = {
        0xE59F'1008u, // LDR R1,[PC,#8]
        0xE280'0001u, // loop: ADD R0,R0,#1
        0xE581'0000u, //       STR R0,[R1]
        0xEAFF'FFFCu, //       B loop
        0x0500'0000u, // .word mem::kPaletteBase
    };
    for (std::uint32_t w : words) {
        file.write(reinterpret_cast<const char*>(&w), sizeof(w));
    }
    return true;
}

} // namespace

int main() {
    const char* path = "/tmp/gba_savestate_test_rom.bin";
    if (!WriteTestRom(path)) {
        std::printf("FAIL: could not write test ROM\n");
        return 1;
    }

    gba::Emulator emuA;
    if (!emuA.LoadRom(path)) {
        std::printf("FAIL: emuA.LoadRom rejected the test ROM\n");
        return 1;
    }

    // Run a bit first so the saved state isn't just the power-on default.
    for (int i = 0; i < 2; ++i) emuA.RunFrame();

    const std::vector<gba::u8> saved = emuA.SaveState();
    Check(!saved.empty(), "SaveState: produced a non-empty buffer");

    // First checkpoint: a handful of frames past the save point.
    for (int i = 0; i < 5; ++i) emuA.RunFrame();
    const gba::u32 expectedColor1 = emuA.ppu().Framebuffer()[0];

    // Second checkpoint further out, to make sure divergence (if any)
    // keeps tracking correctly rather than just matching by coincidence
    // once.
    for (int i = 0; i < 7; ++i) emuA.RunFrame();
    const gba::u32 expectedColor2 = emuA.ppu().Framebuffer()[0];

    // Fresh emulator: load the same ROM, then restore the saved state
    // instead of running from power-on.
    gba::Emulator emuB;
    Check(emuB.LoadRom(path), "emuB.LoadRom succeeded");
    Check(emuB.LoadState(saved), "LoadState: accepted a buffer it just produced");

    for (int i = 0; i < 5; ++i) emuB.RunFrame();
    Check(emuB.ppu().Framebuffer()[0] == expectedColor1,
          "LoadState: framebuffer matches at the first checkpoint");

    for (int i = 0; i < 7; ++i) emuB.RunFrame();
    Check(emuB.ppu().Framebuffer()[0] == expectedColor2,
          "LoadState: framebuffer matches at the second checkpoint");

    // Robustness: a truncated buffer must fail cleanly, not crash or
    // silently accept partial garbage.
    std::vector<gba::u8> truncated(saved.begin(), saved.begin() + static_cast<long>(saved.size() / 2));
    gba::Emulator emuC;
    emuC.LoadRom(path);
    Check(!emuC.LoadState(truncated), "LoadState: rejects a truncated buffer");

    // Robustness: a buffer with an unrecognized format version must also
    // be rejected outright, before touching any real state.
    std::vector<gba::u8> badVersion = saved;
    badVersion[0] = 0xFF; // corrupt the leading format-version word
    gba::Emulator emuD;
    emuD.LoadRom(path);
    Check(!emuD.LoadState(badVersion), "LoadState: rejects an unrecognized format version");

    if (failures == 0) {
        std::printf("PASS: Emulator::SaveState()/LoadState() round-trip and error handling\n");
    }
    return failures;
}
