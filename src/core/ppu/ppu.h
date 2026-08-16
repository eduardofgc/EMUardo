#pragma once

#include <array>
#include <cstdint>
#include "core/types.h"

namespace gba {

class Bus; // Forward declaration

class Ppu {
public:
    Ppu();
    ~Ppu() = default;

    // Set the bus for memory access (used for VRAM, palette, etc.)
    void SetBus(Bus* bus) { bus_ = bus; }

    // Called by Emulator to advance the PPU by one GBA frame.
    // This will update the internal framebuffer based on the current display mode.
    void Update();

    // Access the current framebuffer (for rendering).
    const std::array<u32, kScreenWidth * kScreenHeight>& Framebuffer() const {
        return framebuffer_;
    }

private:
    // The framebuffer: one u32 per pixel (ABGR8888).
    std::array<u32, kScreenWidth * kScreenHeight> framebuffer_;

    // Pointer to the bus for reading VRAM, palette, and I/O registers.
    Bus* bus_ = nullptr;

    // Generate a test pattern (fallback when no valid mode is set).
    void GenerateTestPattern();
};

} // namespace gba