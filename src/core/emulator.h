#pragma once

#include <string>

#include "core/cpu/cpu.h"
#include "core/io/dma.h"
#include "core/io/timers.h"
#include "core/memory/bus.h"
#include "core/ppu/ppu.h"

namespace gba {

// Owns the whole machine and drives it forward in time. main.cpp should
// know nothing about CPU/Bus/PPU internals - it just loads a ROM and calls
// RunFrame() once per host frame, then reads the framebuffer to present it.
class Emulator {
public:
    Emulator();

    bool LoadRom(const std::string& path);

    // Runs enough CPU/PPU steps to produce one full GBA frame
    // (160 visible scanlines + 68 VBlank scanlines).
    void RunFrame();

    const Ppu& ppu() const { return ppu_; }

private:
    // Declaration order matters here: members construct in this order
    // regardless of the constructor's initializer-list order, and cpu_/
    // ppu_/timers_/dma_ all hold a Bus& that must already be alive.
    Bus bus_;
    Cpu cpu_;
    Ppu ppu_;
    Timers timers_;
    Dma dma_;
};

} // namespace gba
