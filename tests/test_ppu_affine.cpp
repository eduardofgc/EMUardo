// Covers affine (rotation/scaling) rendering: an affine BG2 in Mode 1
// (identity transform, 2x scale, and reference-point wraparound on/off),
// plus an affine OBJ (identity transform and the double-size bounding-box
// centering GBATEK describes for "OBJ Rotation/Scaling").

#include <cstdio>

#include "core/memory/bus.h"
#include "core/ppu/ppu.h"
#include "core/types.h"

namespace {

int failures = 0;

void CheckPixel(const gba::Ppu& ppu, int x, int y, gba::u32 expected, const char* label) {
    const gba::u32 actual = ppu.Framebuffer()[static_cast<std::size_t>(y * gba::Ppu::kScreenWidth + x)];
    if (actual != expected) {
        std::printf("FAIL: %s at (%d,%d) expected 0x%08X, got 0x%08X\n",
                     label, x, y, expected, actual);
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

    // Real hardware treats a zeroed OAM entry as a *visible* 8x8 sprite -
    // disable all 128 up front, same as the Mode 0 test.
    for (int i = 0; i < 128; ++i) {
        bus.Write16(gba::mem::kOamBase + static_cast<gba::u32>(i) * 8u, 1u << 9);
    }

    // ---------- Affine background (Mode 1, BG2) ----------
    // BG2CNT: char base block 0, screen base block 1, size 0 (16x16 tiles
    // = 128x128px), overflow/wrap bit initially clear.
    const gba::u16 bg2cnt = static_cast<gba::u16>(1u << 8);
    bus.Write16(gba::mem::kIoBase + gba::io::kBg2Cnt, bg2cnt);

    // Identity transform, no scroll.
    bus.Write16(gba::mem::kIoBase + gba::io::kBg2Pa, 0x0100u);
    bus.Write16(gba::mem::kIoBase + gba::io::kBg2Pb, 0u);
    bus.Write16(gba::mem::kIoBase + gba::io::kBg2Pc, 0u);
    bus.Write16(gba::mem::kIoBase + gba::io::kBg2Pd, 0x0100u);
    bus.Write32(gba::mem::kIoBase + gba::io::kBg2X, 0u);
    bus.Write32(gba::mem::kIoBase + gba::io::kBg2Y, 0u);

    // Screen block 1, tilemap entry (0,0) -> tile 3. Affine tilemaps are
    // 1 byte per entry (tile number only - no flip/palette bits).
    const gba::u32 screenBlock1 = gba::mem::kVramBase + 0x800u;
    bus.Write8(screenBlock1, 3u);

    // Tile 3's pixel data (affine BGs are always 8bpp, 64 bytes/tile) at
    // char base 0: pixel(0,0) = color index 9, pixel(2,0) = color index 10.
    const gba::u32 tile3Addr = gba::mem::kVramBase + 3u * 64u;
    bus.Write8(tile3Addr + 0, 9u);
    bus.Write8(tile3Addr + 2, 10u);

    bus.Write16(gba::mem::kPaletteBase + 9u * 2u, 0x001Fu);  // red
    bus.Write16(gba::mem::kPaletteBase + 10u * 2u, 0x03E0u); // green

    // Mode 1, BG2 enabled.
    bus.Write16(gba::mem::kIoBase + gba::io::kDispcnt, static_cast<gba::u16>(1u | (1u << 10)));

    ppu.RenderFrame();

    CheckPixel(ppu, 0, 0, ExpectedRgba(0x001Fu), "affine BG identity transform: texel(0,0)");
    CheckPixel(ppu, 2, 0, ExpectedRgba(0x03E0u), "affine BG identity transform: texel(2,0)");
    CheckPixel(ppu, 1, 0, ExpectedRgba(bus.Read16(gba::mem::kPaletteBase)),
               "affine BG transparent texel(1,0) falls through to backdrop");

    // Scale: PA=0x200 (2.0) advances the source 2 pixels per screen pixel,
    // so screen pixel (1,0) should now sample source pixel (2,0).
    bus.Write16(gba::mem::kIoBase + gba::io::kBg2Pa, 0x0200u);
    ppu.RenderFrame();
    CheckPixel(ppu, 1, 0, ExpectedRgba(0x03E0u),
               "affine BG 2x scale: screen pixel(1,0) samples source pixel(2,0)");

    // Wraparound: scroll the reference point exactly one 128px map width
    // to the left. Without the overflow bit, off-map reads are
    // transparent; with it set, they wrap back onto the same tile.
    bus.Write16(gba::mem::kIoBase + gba::io::kBg2Pa, 0x0100u); // back to identity
    const gba::s32 oneMapWidthLeft = -(128 << 8);
    bus.Write32(gba::mem::kIoBase + gba::io::kBg2X, static_cast<gba::u32>(oneMapWidthLeft));

    ppu.RenderFrame();
    CheckPixel(ppu, 0, 0, ExpectedRgba(bus.Read16(gba::mem::kPaletteBase)),
               "affine BG without overflow bit: off-map reference point is transparent");

    bus.Write16(gba::mem::kIoBase + gba::io::kBg2Cnt, static_cast<gba::u16>(bg2cnt | (1u << 13)));
    ppu.RenderFrame();
    CheckPixel(ppu, 0, 0, ExpectedRgba(0x001Fu),
               "affine BG with overflow bit: reference point wraps back onto the same tile");

    // ---------- Affine sprite ----------
    // Parameter group 0's PA/PB/PC/PD live in OAM entries 0-3's attr3
    // field (byte offset 6 within each 8-byte entry) - unrelated to those
    // entries' own (disabled) attr0-2, which is why the parameter
    // registers can share OAM space with sprite attributes at all.
    bus.Write16(gba::mem::kOamBase + 0u * 8u + 6u, 0x0100u); // PA = 1.0
    bus.Write16(gba::mem::kOamBase + 1u * 8u + 6u, 0u);      // PB = 0
    bus.Write16(gba::mem::kOamBase + 2u * 8u + 6u, 0u);      // PC = 0
    bus.Write16(gba::mem::kOamBase + 3u * 8u + 6u, 0x0100u); // PD = 1.0

    // OAM entry 4: affine (bit8), not double-size, 8bpp, shape/size 0
    // (8x8), at screen (10,20), using parameter group 0.
    const gba::u16 spriteAttr0 = static_cast<gba::u16>((1u << 8) | (1u << 13) | 20u);
    bus.Write16(gba::mem::kOamBase + 4u * 8u + 0u, spriteAttr0);
    const gba::u16 spriteAttr1 = static_cast<gba::u16>(10u); // X=10, param group 0, size 0
    bus.Write16(gba::mem::kOamBase + 4u * 8u + 2u, spriteAttr1);
    bus.Write16(gba::mem::kOamBase + 4u * 8u + 4u, 0u); // tile 0, priority 0

    // OBJ tile 0 (8bpp) at OBJ base 0x06010000 (Mode 0's tile-mode base):
    // pixel(0,0) = color index 5.
    const gba::u32 objTile0Addr = gba::mem::kVramBase + 0x1'0000u;
    bus.Write8(objTile0Addr, 5u);
    bus.Write16(gba::mem::kPaletteBase + 0x200u + 5u * 2u, 0x7C00u); // blue

    // Mode 0, OBJ enabled, 1D mapping.
    bus.Write16(gba::mem::kIoBase + gba::io::kDispcnt, static_cast<gba::u16>((1u << 12) | (1u << 6)));

    ppu.RenderFrame();

    CheckPixel(ppu, 10, 20, ExpectedRgba(0x7C00u),
               "affine sprite identity transform: texel(0,0) at its screen position");
    CheckPixel(ppu, 11, 21, ExpectedRgba(bus.Read16(gba::mem::kPaletteBase)),
               "affine sprite: untouched texel is transparent (backdrop)");

    // Double-size: same identity param group, but attr0 bit9 set. The
    // bounding box doubles to 16x16 while the texture stays 8x8, so
    // texture pixel (0,0) now lands at bounding-box offset (4,4) instead
    // of (0,0) - GBATEK "OBJ Rotation/Scaling".
    const gba::u16 spriteAttr0DoubleSize = static_cast<gba::u16>(spriteAttr0 | (1u << 9));
    bus.Write16(gba::mem::kOamBase + 4u * 8u + 0u, spriteAttr0DoubleSize);
    ppu.RenderFrame();

    CheckPixel(ppu, 10, 20, ExpectedRgba(bus.Read16(gba::mem::kPaletteBase)),
               "affine sprite double-size: bounding-box corner is transparent (outside texture)");
    CheckPixel(ppu, 14, 24, ExpectedRgba(0x7C00u),
               "affine sprite double-size: texel(0,0) re-centered at bounding-box offset (4,4)");

    if (failures == 0) {
        std::printf("PASS: affine BG (identity/scale/wraparound) and affine OBJ (identity/double-size)\n");
    }
    return failures;
}
