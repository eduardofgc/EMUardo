#include "core/ppu/ppu.h"

#include <algorithm>
#include <cstdint>

namespace gba {

Ppu::Ppu() {
    // Generate initial test pattern
    GenerateTestPattern(frame_seed_);
}

void Ppu::Update() {
    // Increment seed and regenerate test pattern each frame
    frame_seed_ = (frame_seed_ + 1) & 0xFF; // wrap at 255
    GenerateTestPattern(frame_seed_);
}

void Ppu::GenerateTestPattern(u8 seed) {
    // We'll create a pattern that shifts horizontally with the seed.
    // For each pixel, we compute a hue based on (x + seed) and then convert to RGB.
    // But to keep it simple, we'll do a moving vertical bar of a fixed color.

    // Let's make a simple pattern: a vertical bar that moves across the screen.
    // The bar will be white, and the background will be black.
    // The position of the bar is determined by seed.

    const int bar_width = 20;
    const int bar_pos = (seed * kScreenWidth) / 256; // scroll from left to right

    for (int y = 0; y < kScreenHeight; ++y) {
        for (int x = 0; x < kScreenWidth; ++x) {
            u32 color = 0xFF000000; // default: black, ABGR8888 (A=0xFF, B=0, G=0, R=0)

            // If x is within the moving bar, set to white.
            if (x >= bar_pos && x < bar_pos + bar_width) {
                color = 0xFFFFFFFF; // white
            }

            framebuffer_[y * kScreenWidth + x] = color;
        }
    }
}

} // namespace gba