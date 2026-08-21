#pragma once

#include <string>
#include <vector>

#include "core/apu/apu.h"
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

    // Interleaved L,R signed-16-bit samples generated since the last
    // call, at a fixed 32768Hz - main.cpp drains this once per host
    // frame and queues it to SDL's audio device.
    std::vector<s16> DrainAudioSamples() { return apu_.DrainSamples(); }

    // Forwards to Bus::SetKeyState - call once per host frame with the
    // current button state before RunFrame().
    void SetKeyState(u16 pressedMask) { bus_.SetKeyState(pressedMask); }

    // Forwards to Bus::FlushSave - RunFrame() already calls this once per
    // VBlank, so games that autosave mid-play are covered automatically;
    // call this explicitly on shutdown too, so a game that only writes
    // its save right before quitting isn't lost.
    bool FlushSave() { return bus_.FlushSave(); }

    // Full save-state support: everything needed to resume exactly where
    // play stopped (CPU/PPU/Timers/Dma/Apu/Bus state), serialized into one
    // flat byte buffer and back. Deliberately NOT part of it: the ROM
    // itself (the caller must LoadRom() the matching file first - a save
    // state isn't a portable snapshot, just a resume point for whichever
    // ROM produced it) and cpu_/ppu_/etc.'s cross-wired callbacks (this
    // constructor already rewires those on every Emulator, load or not).
    std::vector<u8> SaveState() const;
    bool LoadState(const std::vector<u8>& data);

private:
    // Declaration order matters here: members construct in this order
    // regardless of the constructor's initializer-list order, and cpu_/
    // ppu_/timers_/dma_/apu_ all hold a Bus& that must already be alive.
    Bus bus_;
    Cpu cpu_;
    Ppu ppu_;
    Timers timers_;
    Dma dma_;
    Apu apu_;

    // Runs one Direct Sound sample tick (see Apu::GenerateSample) every
    // 512 CPU cycles - the exact divisor of the 16.78MHz CPU clock that
    // yields 32768Hz, so this just tracks cycles owed since the last tick.
    int cyclesSinceLastSample_ = 0;
};

} // namespace gba
