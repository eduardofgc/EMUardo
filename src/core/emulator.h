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

    // Forwards to Bus::SetKeyState - call once per host frame with the
    // current button state before RunFrame().
    void SetKeyState(u16 pressedMask) { bus_.SetKeyState(pressedMask); }

    // Forwards to Bus::FlushSave - RunFrame() already calls this once per
    // VBlank, so games that autosave mid-play are covered automatically;
    // call this explicitly on shutdown too, so a game that only writes
    // its save right before quitting isn't lost.
    bool FlushSave() { return bus_.FlushSave(); }

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
