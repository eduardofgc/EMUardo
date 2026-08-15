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

    // Set the bus for memory access (used for test pattern updates)
    void SetBus(Bus* bus) { bus_ = bus; }

    // Called by Emulator to advance the PPU by one GBA frame.
    // This will update the internal framebuffer if needed.
    void Update();

    // Access the current framebuffer (for rendering).
    const std::array<u32, kScreenWidth * kScreenHeight>& Framebuffer() const {
        return framebuffer_;
    }

private:
    // The framebuffer: one u32 per pixel (ABGR8888).
    std::array<u32, kScreenWidth * kScreenHeight> framebuffer_;

    // Pointer to the bus for reading test pattern seed (not used yet, but kept for future).
    Bus* bus_ = nullptr;

    // Seed for the test pattern, incremented each frame.
    u8 frame_seed_ = 0;

    // Generate the test pattern based on a seed value (0-255).
    void GenerateTestPattern(u8 seed);
};

} // namespace gba