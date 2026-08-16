#include "core/ppu/ppu.h"
#include "core/memory/bus.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace gba {

Ppu::Ppu() {
    // Generate initial test pattern (fallback)
    GenerateTestPattern();
}

void Ppu::Update() {
    static int frame_counter = 0;
    frame_counter++;

    if (!bus_) {
        // No bus set, fall back to test pattern
        if (frame_counter == 1) {
            std::fprintf(stderr, "PPU: bus_ is null, using test pattern\n");
        }
        GenerateTestPattern();
        return;
    }

    // Read DISPCNT (at 0x04000000) to get the display mode
    u16 dispcnt = bus_->Read16(0x04000000);
    u32 mode = dispcnt & 0x7; // bits 0-2

    // Debug: print dispcnt and mode every 60 frames
    if (frame_counter % 60 == 0) {
        std::fprintf(stderr, "PPU: dispcnt=0x%04x, mode=%u\n", dispcnt, mode);
    }

    if (mode == 0) {
        // Mode 0: Render VRAM as 4bpp grayscale (each byte = two 4-bit pixels)
        // We'll treat the first (240*160/2) = 19200 bytes of VRAM as the pixel data.
        const int bytes_per_row = kScreenWidth / 2; // 240/2 = 120
        for (int y = 0; y < kScreenHeight; ++y) {
            for (int bx = 0; bx < bytes_per_row; ++bx) {
                u8 byte = bus_->Read8(0x06000000 + y * bytes_per_row + bx);
                u8 pixel1 = byte & 0x0F;
                u8 pixel2 = (byte >> 4) & 0x0F;
                // Convert 4-bit value (0-15) to 8-bit grayscale (0-255): multiply by 17
                u8 gray1 = pixel1 * 17;
                u8 gray2 = pixel2 * 17;
                u32 color1 = (0xFFu << 24) | (gray1 << 16) | (gray1 << 8) | gray1;
                u32 color2 = (0xFFu << 24) | (gray2 << 16) | (gray2 << 8) | gray2;
                int x0 = bx * 2;
                int x1 = x0 + 1;
                framebuffer_[y * kScreenWidth + x0] = color1;
                framebuffer_[y * kScreenWidth + x1] = color2;
            }
        }
        // Debug: print first 5 pixels of first row every 60 frames
        if (frame_counter % 60 == 0) {
            std::fprintf(stderr, "PPU mode0: first 5 pixels of row 0: ");
            for (int x = 0; x < 5; ++x) {
                u32 color = framebuffer_[0 * kScreenWidth + x];
                u8 r = (color >> 0) & 0xFF;
                u8 g = (color >> 8) & 0xFF;
                u8 b = (color >> 16) & 0xFF;
                std::fprintf(stderr, "(%u,%u,%u) ", r, g, b);
            }
            std::fprintf(stderr, "\n");
        }
        return;
    }

    if (mode == 3) {
        // Mode 3: 240x160, 8-bit color (bitmap mode)
        for (int y = 0; y < kScreenHeight; ++y) {
            for (int x = 0; x < kScreenWidth; ++x) {
                u32 vram_offset = y * kScreenWidth + x;
                u8 color_index = bus_->Read8(0x06000000 + vram_offset);
                u16 palette_entry = bus_->Read16(0x05000000 + color_index * 2);

                // Convert palette_entry (which is BGR: 0bbbbbgg gggrrrrr) to ABGR8888
                u8 b = (palette_entry >> 0) & 0x1F;
                u8 g = (palette_entry >> 5) & 0x1F;
                u8 r = (palette_entry >> 10) & 0x1F;
                // Expand 5-bit to 8-bit: (x << 3) | (x >> 2)
                b = (b << 3) | (b >> 2);
                g = (g << 3) | (g >> 2);
                r = (r << 3) | (r >> 2);

                u32 color = (0xFFu << 24) | (b << 16) | (g << 8) | r;
                framebuffer_[y * kScreenWidth + x] = color;
            }
        }
        return;
    }

    if (mode == 4) {
        // Mode 4: 240x160, 8-bit color (bitmap mode 2)
        for (int y = 0; y < kScreenHeight; ++y) {
            for (int x = 0; x < kScreenWidth; ++x) {
                u32 vram_offset = y * kScreenWidth + x;
                u8 color_index = bus_->Read8(0x06000000 + vram_offset);
                u16 palette_entry = bus_->Read16(0x05000000 + color_index * 2);

                // Convert palette_entry (which is BGR: 0bbbbbgg gggrrrrr) to ABGR8888
                u8 b = (palette_entry >> 0) & 0x1F;
                u8 g = (palette_entry >> 5) & 0x1F;
                u8 r = (palette_entry >> 10) & 0x1F;
                // Expand 5-bit to 8-bit: (x << 3) | (x >> 2)
                b = (b << 3) | (b >> 2);
                g = (g << 3) | (g >> 2);
                r = (r << 3) | (r >> 2);

                u32 color = (0xFFu << 24) | (b << 16) | (g << 8) | r;
                framebuffer_[y * kScreenWidth + x] = color;
            }
        }
        return;
    }

    // If we get here, either mode not 0, 3, or 4 or we don't handle it -> fall back to test pattern
    GenerateTestPattern();
}

void Ppu::GenerateTestPattern() {
    // We'll create a pattern that shifts horizontally with a seed.
    // For each pixel, we compute a hue based on (x + seed) and then convert to RGB.
    // But to keep it simple, we'll do a moving vertical bar of a fixed color.

    // Let's make a simple pattern: a vertical bar that moves across the screen.
    // The bar will be white, and the background will be black.
    // The position of the bar is determined by a seed that we increment each frame.

    static u8 frame_seed = 0;
    frame_seed = (frame_seed + 1) & 0xFF; // wrap at 255

    const int bar_width = 20;
    const int bar_pos = (frame_seed * kScreenWidth) / 256; // scroll from left to right

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