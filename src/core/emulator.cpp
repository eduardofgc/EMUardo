#include "core/emulator.h"

namespace gba {

namespace {
constexpr int kScanlinesPerFrame = 228; // 160 visible + 68 VBlank
}

Emulator::Emulator() : cpu_(bus_) {
    // Set the bus for the PPU so it can access memory for test patterns (if needed)
    ppu_.SetBus(&bus_);
}

bool Emulator::LoadRom(const std::string& path) {
    return bus_.LoadRom(path);
}

void Emulator::RunFrame() {
    // Placeholder timing: step the CPU some fixed number of times per
    // scanline. Once timers/DMA/interrupts exist this needs to be replaced
    // with proper cycle-accurate stepping shared between CPU and PPU.
    constexpr int kCyclesPerScanline = 1232;

    for (int line = 0; line < kScanlinesPerFrame; ++line) {
        int cyclesRun = 0;
        while (cyclesRun < kCyclesPerScanline) {
            cyclesRun += cpu_.Step();
        }
        // PPU stepping is handled per frame, not per scanline, in this simple version.
        // We'll update the PPU once per frame after all scanlines.
    }
    // Update the PPU once per frame (for our test pattern)
    ppu_.Update();
}

} // namespace gba