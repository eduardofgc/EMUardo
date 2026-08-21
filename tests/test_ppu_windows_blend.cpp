// Covers the three features layered on top of Mode 0 compositing this
// session: OBJ mosaic (pixelation), OBJ window mode (a sprite that gates
// layer visibility instead of drawing itself), semi-transparent OBJ mode
// (forced alpha blending), and Win0/WinOut layer masking combined with
// BLDCNT alpha blending and brightness effects.

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

void DisableAllObjects(gba::Bus& bus) {
    for (int i = 0; i < 128; ++i) {
        bus.Write16(gba::mem::kOamBase + static_cast<gba::u32>(i) * 8u, 1u << 9);
    }
}

} // namespace

int main() {
    // ---------- OBJ mosaic ----------
    {
        gba::Bus bus;
        gba::Ppu ppu(bus);
        DisableAllObjects(bus);

        // MOSAIC: OBJ H size = 4 (register value 3), V size = 1 (register 0).
        bus.Write16(gba::mem::kIoBase + gba::io::kMosaic, static_cast<gba::u16>(3u << 8));

        // 8x8, 8bpp sprite at (0,0), mosaic bit (attr0 bit12) set. Tile 0's
        // pixel(0,0)=color index 5, everything else 0 (transparent) - the
        // mosaic block covering columns 0-3 should still show index 5's
        // color across all four columns since it's sampled from the block's
        // first (unflipped) column.
        const gba::u16 attr0 = static_cast<gba::u16>((1u << 13) | (1u << 12));
        bus.Write16(gba::mem::kOamBase + 0u, attr0);
        bus.Write16(gba::mem::kOamBase + 2u, 0u);
        bus.Write16(gba::mem::kOamBase + 4u, 0u);

        const gba::u32 objTile0Addr = gba::mem::kVramBase + 0x1'0000u;
        bus.Write8(objTile0Addr, 5u);
        bus.Write16(gba::mem::kPaletteBase + 0x200u + 5u * 2u, 0x7C00u); // blue

        bus.Write16(gba::mem::kIoBase + gba::io::kDispcnt, static_cast<gba::u16>((1u << 12) | (1u << 6)));
        for (int l = 0; l < gba::Ppu::kScreenHeight; ++l) ppu.RenderScanline(l);

        CheckPixel(ppu, 0, 0, ExpectedRgba(0x7C00u), "OBJ mosaic: source column is blue");
        CheckPixel(ppu, 3, 0, ExpectedRgba(0x7C00u),
                   "OBJ mosaic: column 3 (same 4px block) also blue via mosaic block sampling");
        CheckPixel(ppu, 4, 0, ExpectedRgba(bus.Read16(gba::mem::kPaletteBase)),
                   "OBJ mosaic: column 4 (next block) samples its own (transparent) source column");
    }

    // ---------- OBJ window ----------
    {
        gba::Bus bus;
        gba::Ppu ppu(bus);
        DisableAllObjects(bus);

        // BG0: 4bpp text tile covering the whole screen with color index 3
        // (green) at every pixel of tile 0, used as every tilemap entry.
        const gba::u16 bg0cnt = static_cast<gba::u16>(1u << 8); // screen base block 1
        bus.Write16(gba::mem::kIoBase + gba::io::kBg0Cnt, bg0cnt);
        const gba::u32 tilemapBase = gba::mem::kVramBase + 0x800u;
        for (gba::u32 row = 0; row < 20; ++row) {
            for (gba::u32 col = 0; col < 30; ++col) {
                bus.Write16(tilemapBase + (row * 32u + col) * 2u, 0u); // tile 0, palette bank 0
            }
        }
        const gba::u32 tile0Addr = gba::mem::kVramBase;
        for (int i = 0; i < 32; ++i) {
            bus.Write8(tile0Addr + static_cast<gba::u32>(i), 0x33u); // both nibbles = color index 3
        }
        bus.Write16(gba::mem::kPaletteBase + 3u * 2u, 0x03E0u); // green

        // OBJ mode 2 (OBJ window) sprite: 8x8 at (10,10). Tile data must be
        // opaque (color index != 0) for it to mark the window mask, but its
        // own color/palette is irrelevant - it never draws.
        const gba::u16 objWinAttr0 = static_cast<gba::u16>(10u | (1u << 13) | (2u << 10));
        bus.Write16(gba::mem::kOamBase + 0u, objWinAttr0);
        bus.Write16(gba::mem::kOamBase + 2u, 10u);
        bus.Write16(gba::mem::kOamBase + 4u, 0u);
        const gba::u32 objTile0Addr = gba::mem::kVramBase + 0x1'0000u;
        bus.Write8(objTile0Addr, 9u); // any nonzero index

        // WINOUT: outside all windows -> nothing enabled (BG0 off).
        // Inside the OBJ window -> BG0 enabled (bit0).
        bus.Write16(gba::mem::kIoBase + gba::io::kWinOut, static_cast<gba::u16>(1u << 8));

        // DISPCNT: Mode 0, BG0 enabled, OBJ enabled, 1D mapping, OBJ window enabled (bit15).
        const gba::u16 dispcnt = static_cast<gba::u16>((1u << 8) | (1u << 12) | (1u << 6) | (1u << 15));
        bus.Write16(gba::mem::kIoBase + gba::io::kDispcnt, dispcnt);

        for (int l = 0; l < gba::Ppu::kScreenHeight; ++l) ppu.RenderScanline(l);

        CheckPixel(ppu, 0, 0, ExpectedRgba(bus.Read16(gba::mem::kPaletteBase)),
                   "OBJ window: outside the OBJ window, BG0 is disabled by WINOUT -> backdrop");
        CheckPixel(ppu, 10, 10, ExpectedRgba(0x03E0u),
                   "OBJ window: inside the OBJ-window sprite's shape, BG0 is enabled by WINOUT -> green");
    }

    // ---------- Semi-transparent OBJ (forced alpha blend) ----------
    {
        gba::Bus bus;
        gba::Ppu ppu(bus);
        DisableAllObjects(bus);

        // Backdrop = red, used as the 2nd target (BG absent/disabled here,
        // so the sprite blends directly against the backdrop).
        bus.Write16(gba::mem::kPaletteBase, 0x001Fu);

        // Semi-transparent (OBJ mode 1) 8x8 sprite at (0,0), color index 5 ->
        // pure blue in OBJ palette.
        const gba::u16 attr0 = static_cast<gba::u16>((1u << 13) | (1u << 10));
        bus.Write16(gba::mem::kOamBase + 0u, attr0);
        bus.Write16(gba::mem::kOamBase + 2u, 0u);
        bus.Write16(gba::mem::kOamBase + 4u, 0u);
        const gba::u32 objTile0Addr = gba::mem::kVramBase + 0x1'0000u;
        bus.Write8(objTile0Addr, 5u);
        bus.Write16(gba::mem::kPaletteBase + 0x200u + 5u * 2u, 0x7C00u); // blue

        // BLDCNT: backdrop (bit5) as 2nd target; effect mode bits (6-7)
        // deliberately left at 0 ("None") to prove the semi-transparent OBJ
        // forces blending regardless. BLDALPHA: EVA=8/16, EVB=8/16 (50/50).
        bus.Write16(gba::mem::kIoBase + gba::io::kBldCnt, static_cast<gba::u16>(1u << 13));
        bus.Write16(gba::mem::kIoBase + gba::io::kBldAlpha, static_cast<gba::u16>(8u | (8u << 8)));

        bus.Write16(gba::mem::kIoBase + gba::io::kDispcnt, static_cast<gba::u16>((1u << 12) | (1u << 6)));
        for (int l = 0; l < gba::Ppu::kScreenHeight; ++l) ppu.RenderScanline(l);

        // 50/50 blend of pure blue (0,0,255) and pure red (255,0,0) -> (127,0,127).
        const gba::u32 expected = 0xFF00'0000u | (127u << 16) | (0u << 8) | 127u;
        CheckPixel(ppu, 0, 0, expected,
                   "semi-transparent OBJ: forced 50/50 alpha blend with backdrop, even with BLDCNT effect=None");
    }

    // ---------- BLDCNT brightness increase (fade to white) ----------
    {
        gba::Bus bus;
        gba::Ppu ppu(bus);
        DisableAllObjects(bus);

        // Backdrop = pure red; nothing else drawn, so backdrop is both the
        // top and bottom candidate.
        bus.Write16(gba::mem::kPaletteBase, 0x001Fu);

        // BLDCNT: backdrop (bit5) as 1st target, effect mode 2 (brightness
        // increase). BLDY: EVY = 8/16 (halfway to white).
        bus.Write16(gba::mem::kIoBase + gba::io::kBldCnt, static_cast<gba::u16>((1u << 5) | (2u << 6)));
        bus.Write16(gba::mem::kIoBase + gba::io::kBldY, 8u);

        bus.Write16(gba::mem::kIoBase + gba::io::kDispcnt, 0u);
        for (int l = 0; l < gba::Ppu::kScreenHeight; ++l) ppu.RenderScanline(l);

        // Red (255,0,0) faded 50% toward white (255,255,255) -> (255,127,127).
        const gba::u32 expected = 0xFF00'0000u | (127u << 16) | (127u << 8) | 255u;
        CheckPixel(ppu, 0, 0, expected, "BLDCNT brightness increase: backdrop fades halfway to white");
    }

    if (failures == 0) {
        std::printf("PASS: OBJ mosaic, OBJ window, semi-transparent OBJ blending, BLDCNT brightness effects\n");
    }
    return failures;
}
