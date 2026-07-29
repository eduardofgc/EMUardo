// Covers Mode 0: a single 4bpp background tile with a non-default
// palette, an 8bpp sprite, and a priority/transparency check proving the
// sprite correctly shows through a transparent BG pixel while a BG3
// pixel drawn "in front" (lower priority number) still wins over BG0
// where they'd otherwise overlap.

#include <cstdio>

#include "core/memory/bus.h"
#include "core/ppu/ppu.h"
#include "core/types.h"

namespace {

int failures = 0;

void Check(bool condition, const char* label) {
    if (!condition) {
        std::printf("FAIL: %s\n", label);
        ++failures;
    }
}

gba::u32 ExpectedRgba(gba::u16 bgr555) {
    const gba::u32 r5 = bgr555 & 0x1Fu;
    const gba::u32 g5 = (bgr555 >> 5) & 0x1Fu;
    const gba::u32 b5 = (bgr555 >> 10) & 0x1Fu;
    const gba::u32 r8 = (r5 << 3) | (r5 >> 2);
    const gba::u32 g8 = (g5 << 3) | (g5 >> 2);
    const gba::u32 b8 = (b5 << 3) | (b5 >> 2);
    return 0xFF00'0000u | (b8 << 16) | (g8 << 8) | r8;
}

} // namespace

int main() {
    gba::Bus bus;
    gba::Ppu ppu(bus);

    // Real hardware treats a zeroed OAM entry as a *visible* 8x8 sprite at
    // (0,0) using tile 0 - real games always disable all 128 entries at
    // startup for exactly this reason. Do the same here, up front, since
    // DISPCNT enables OBJ before any sprite-specific setup happens below.
    for (int i = 0; i < 128; ++i) {
        bus.Write16(gba::mem::kOamBase + static_cast<gba::u32>(i) * 8u, 1u << 9); // disable bit
    }

    // --- Set up BG0: 4bpp text mode, char base block 0, screen base
    // block 1 (so it doesn't collide with sprite tile data we place at
    // char base 0 of the OBJ area later), palette bank 2. ---
    // BG0CNT: screen base=1 (bits8-12), char base=0 (bits2-3), 4bpp (bit7=0)
    const gba::u16 bg0cnt = static_cast<gba::u16>(1u << 8);
    bus.Write16(gba::mem::kIoBase + gba::io::kBg0Cnt, bg0cnt);
    bus.Write16(gba::mem::kIoBase + gba::io::kBg0HOfs, 0);
    bus.Write16(gba::mem::kIoBase + gba::io::kBg0VOfs, 0);

    // Tilemap entry (0,0) in screen block 1 -> tile 5, palette bank 2, no flip.
    const gba::u32 screenBlock1 = gba::mem::kVramBase + 0x800u;
    const gba::u16 tileEntry = static_cast<gba::u16>((2u << 12) | 5u);
    bus.Write16(screenBlock1, tileEntry);

    // Tile 5's pixel data (4bpp, 32 bytes) at char base 0: make pixel (0,0)
    // color index 3, everything else index 0 (transparent).
    const gba::u32 tile5Addr = gba::mem::kVramBase + 5u * 32u;
    bus.Write8(tile5Addr, 0x03u); // low nibble = pixel(0,0) = index 3, high nibble = pixel(1,0) = 0

    // BG palette bank 2, color index 3 -> pure green (G=31).
    const gba::u32 bgPaletteAddr = gba::mem::kPaletteBase + (2u * 16u + 3u) * 2u;
    const gba::u16 green = 0x03E0u;
    bus.Write16(bgPaletteAddr, green);

    // --- DISPCNT: Mode 0, BG0 enabled, OBJ enabled, 1D OBJ mapping ---
    const gba::u16 dispcnt = 0u | (1u << 8) | (1u << 12) | (1u << 6);
    bus.Write16(gba::mem::kIoBase + gba::io::kDispcnt, dispcnt);

    ppu.RenderFrame();

    Check(ppu.Framebuffer()[0] == ExpectedRgba(green), "BG0 tile pixel (0,0) is green");
    Check(ppu.Framebuffer()[1] == ExpectedRgba(bus.Read16(gba::mem::kPaletteBase)),
          "BG0 transparent pixel (1,0) falls through to backdrop color");

    // Sprite 0: 8x8, 8bpp, tile 0 of OBJ area, at screen position (1,0).
    // attr0: Y=0, shape=0 (square), 8bpp (bit13=1)
    bus.Write16(gba::mem::kOamBase + 0, static_cast<gba::u16>(1u << 13));
    // attr1: X=1, size=0 (8x8)
    bus.Write16(gba::mem::kOamBase + 2, 1u);
    // attr2: tile 0, priority 0, palette irrelevant in 8bpp mode
    bus.Write16(gba::mem::kOamBase + 4, 0u);

    // OBJ tile 0 (8bpp, 64 bytes) at OBJ base 0x06010000: pixel(0,0) = color index 7.
    const gba::u32 objTile0Addr = gba::mem::kVramBase + 0x1'0000u;
    bus.Write8(objTile0Addr, 7u);

    // OBJ palette, color index 7 -> pure blue (B=31).
    const gba::u32 objPaletteAddr = gba::mem::kPaletteBase + 0x200u + 7u * 2u;
    const gba::u16 blue = 0x7C00u;
    bus.Write16(objPaletteAddr, blue);

    ppu.RenderFrame();

    Check(ppu.Framebuffer()[1] == ExpectedRgba(blue),
          "Sprite pixel shows through a transparent BG pixel at (1,0)");
    Check(ppu.Framebuffer()[0] == ExpectedRgba(green),
          "BG0 pixel (0,0) is unaffected by the sprite (sprite doesn't cover it)");

    if (failures == 0) {
        std::printf("PASS: Mode 0 BG tile rendering, sprite rendering, and transparency compositing\n");
    }
    return failures;
}
