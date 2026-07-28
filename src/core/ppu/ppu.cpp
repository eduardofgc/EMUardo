#include "core/ppu/ppu.h"

#include <cstdio>

namespace gba {

namespace {
// GBA colors are 15-bit BGR: bit15 unused, bits14-10 = Blue, bits9-5 =
// Green, bits4-0 = Red (GBATEK "GBA Color Component Layout"). Expanding a
// 5-bit channel to 8 bits by (c<<3)|(c>>2) spreads the top 3 bits into the
// low end too, so 0x1F maps to 0xFF instead of 0xF8 - a flat left-shift
// alone would never reach full brightness.
u32 Bgr555ToRgba8888(u16 color) {
    const u32 r5 = color & 0x1Fu;
    const u32 g5 = (color >> 5) & 0x1Fu;
    const u32 b5 = (color >> 10) & 0x1Fu;

    const u32 r8 = (r5 << 3) | (r5 >> 2);
    const u32 g8 = (g5 << 3) | (g5 >> 2);
    const u32 b8 = (b5 << 3) | (b5 >> 2);

    // Packed to match SDL_PIXELFORMAT_ABGR8888 as used in main.cpp: on a
    // little-endian machine that format's in-memory byte order is
    // R,G,B,A, which is exactly what (A<<24)|(B<<16)|(G<<8)|R produces as
    // a native u32 value.
    return 0xFF00'0000u | (b8 << 16) | (g8 << 8) | r8;
}
} // namespace

Ppu::Ppu(Bus& bus) : bus_(bus) {
    // Placeholder color so it's obvious in the SDL window that the
    // pipeline is wired up correctly, before RenderFrame() has run once.
    framebuffer_.fill(0xFF30'3030);
}

void Ppu::Step() {
    // TODO: scanline-driven rendering per DISPCNT mode - see the note on
    // this function in ppu.h. RenderFrame() covers the visible behavior
    // for now.
}

void Ppu::RenderFrame() {
    const u16 dispcnt = bus_.Read16(mem::kIoBase + io::kDispcnt);
    const u32 mode = dispcnt & 0x7u;

    switch (mode) {
        case 3:
            RenderMode3();
            break;
        default: {
            // Modes 0-2 (tiled) and 4-5 (other bitmap modes) aren't
            // implemented yet - fill with the same placeholder color used
            // before any rendering existed, so "unimplemented mode" and
            // "nothing rendered yet" look identically obvious rather than
            // showing stale data from a previous frame.
            static bool warned = false;
            if (!warned) {
                std::fprintf(stderr, "Ppu::RenderFrame: mode %u not implemented yet\n", mode);
                warned = true;
            }
            framebuffer_.fill(0xFF30'3030);
            break;
        }
    }
}

void Ppu::RenderMode3() {
    // Mode 3: BG2 only, one 16-bit BGR555 pixel per screen pixel, laid out
    // left-to-right/top-to-bottom starting at the base of VRAM - no tiles,
    // no palette, no scrolling registers consulted here yet. GBATEK
    // "BG Mode 3 - 16bit Bitmap".
    for (int y = 0; y < kScreenHeight; ++y) {
        for (int x = 0; x < kScreenWidth; ++x) {
            const u32 pixelIndex = static_cast<u32>(y * kScreenWidth + x);
            const u32 address = mem::kVramBase + pixelIndex * 2;
            const u16 color = bus_.Read16(address);
            framebuffer_[pixelIndex] = Bgr555ToRgba8888(color);
        }
    }
}

} // namespace gba
