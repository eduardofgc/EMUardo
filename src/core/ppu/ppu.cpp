#include "core/ppu/ppu.h"

#include <cstdint>

namespace gba {

Ppu::Ppu() {
    // Draw a simple test pattern: vertical color bars + horizontal gradient
    // This makes it visually obvious the framebuffer is being updated and presented.
    for (int y = 0; y < kScreenHeight; ++y) {
        for (int x = 0; x < kScreenWidth; ++x) {
            // Vertical bars: cycle through R, G, B, white, black
            int bar = x / (kScreenWidth / 5);
            u8 r = 0, g = 0, b = 0;

            switch (bar) {
                case 0: r = 0xFF; break;           // Red
                case 1: g = 0xFF; break;           // Green
                case 2: b = 0xFF; break;           // Blue
                case 3: r = g = b = 0xFF; break;   // White
                case 4: r = g = b = 0x00; break;   // Black
            }

            // Add a horizontal gradient fade (top to bottom)
            u8 fade = static_cast<u8>(0xFF * y / kScreenHeight);
            r = (r * fade) / 0xFF;
            g = (g * fade) / 0xFF;
            b = (b * fade) / 0xFF;

            // ABGR8888 format for SDL_PIXELFORMAT_ABGR8888
            framebuffer_[y * kScreenWidth + x] = (0xFFu << 24) | (b << 16) | (g << 8) | r;
        }
    }
}

void Ppu::Step() {
    // Still empty - test pattern is static for now.
    // Once CPU executes real code, we'll render based on DISPCNT/VRAM here.
}

} // namespace gba