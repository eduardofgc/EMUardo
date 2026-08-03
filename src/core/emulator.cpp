#include "core/emulator.h"

namespace gba {

namespace {
constexpr int kScanlinesPerFrame = 228; // 160 visible + 68 VBlank
}

Emulator::Emulator() : cpu_(bus_) {}

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
        ppu_.Step();
    }
}

} // namespace gba
