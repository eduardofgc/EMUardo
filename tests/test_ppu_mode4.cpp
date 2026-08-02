#include <cstdio>

#include "core/memory/bus.h"
#include "core/ppu/ppu.h"
#include "core/types.h"

namespace {
int failures = 0;
void Check(bool c, const char* label) {
    if (!c) { std::printf("FAIL: %s\n", label); ++failures; }
}
gba::u32 ExpectedRgba(gba::u16 bgr555) {
    const gba::u32 r5 = bgr555 & 0x1Fu, g5 = (bgr555 >> 5) & 0x1Fu, b5 = (bgr555 >> 10) & 0x1Fu;
    const gba::u32 r8 = (r5 << 3) | (r5 >> 2), g8 = (g5 << 3) | (g5 >> 2), b8 = (b5 << 3) | (b5 >> 2);
    return 0xFF00'0000u | (b8 << 16) | (g8 << 8) | r8;
}
}

int main() {
    gba::Bus bus;
    gba::Ppu ppu(bus);

    // Palette index 5 -> yellow-ish (R=31,G=31,B=0)
    const gba::u16 color = 0x03FFu;
    bus.Write16(gba::mem::kPaletteBase + 5u * 2u, color);

    // Frame 0: pixel (0,0) = palette index 5
    bus.Write8(gba::mem::kVramBase, 5u);
    bus.Write16(gba::mem::kIoBase + gba::io::kDispcnt, 4u); // mode 4, frame 0
    ppu.RenderFrame();
    Check(ppu.Framebuffer()[0] == ExpectedRgba(color), "Mode 4 frame 0 pixel reads palette index correctly");

    // Frame 1 (VRAM+0xA000): different pixel value, and DISPCNT bit4 flips to it
    bus.Write16(gba::mem::kPaletteBase + 9u * 2u, 0x7C00u); // index 9 -> pure blue
    bus.Write8(gba::mem::kVramBase + 0xA000u, 9u);
    bus.Write16(gba::mem::kIoBase + gba::io::kDispcnt, 4u | (1u << 4)); // mode 4, frame 1
    ppu.RenderFrame();
    Check(ppu.Framebuffer()[0] == ExpectedRgba(0x7C00u), "Mode 4 frame select (bit4) switches to the second VRAM buffer");

    if (failures == 0) {
        std::printf("PASS: Mode 4 paletted bitmap rendering and frame flip\n");
    }
    return failures;
}
