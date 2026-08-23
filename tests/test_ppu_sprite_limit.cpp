// Covers the per-scanline OBJ rendering cycle budget (GBATEK "Time
// Available for OBJ Rendering") added this session - real hardware can
// only spend a limited number of cycles per scanline fetching sprite
// tile data, so once a scanline's OAM has enough sprites queued up, the
// ones later in OAM (higher index) simply don't get drawn on that line
// at all, even though they're otherwise fully configured and visible.
// The cost model here was verified against mGBA's
// GBAVideoRendererCleanOAM/PreprocessSpriteLayer, not just GBATEK's own
// simplified description - see RenderSprites' comment in ppu.cpp.

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

// Sets up a normal (non-affine), 64x64, 4bpp sprite at OAM index `index`,
// screen position (x, y=0), filled with palette index 1 of palette bank
// `paletteBank` (so its rendered color is whatever's written there).
void SetupWideSprite(gba::Bus& bus, int index, int x, int paletteBank) {
    const gba::u32 entry = gba::mem::kOamBase + static_cast<gba::u32>(index) * 8u;
    bus.Write16(entry + 0u, 0u); // y=0, normal, 4bpp, shape=0 (square)
    bus.Write16(entry + 2u, static_cast<gba::u16>((3u << 14) | (static_cast<gba::u32>(x) & 0x1FFu))); // size=3 -> 64x64
    bus.Write16(entry + 4u, static_cast<gba::u16>(static_cast<gba::u32>(paletteBank) << 12)); // tile 0, priority 0

    // Tile 0 (1D-mapped, 4bpp): every pixel = palette index 1, so the
    // whole 64x64 sprite (64 tiles) renders as a solid color once the
    // matching palette bank entry is set.
    const gba::u32 tileBase = gba::mem::kVramBase + 0x1'0000u;
    for (gba::u32 t = 0; t < 64u; ++t) {
        for (gba::u32 b = 0; b < 32u; ++b) {
            bus.Write8(tileBase + t * 32u + b, 0x11u); // both nibbles = color index 1
        }
    }
}

} // namespace

int main() {
    // ---------- Sanity check: a handful of sprites, nowhere near the
    // budget, all render normally (the new logic doesn't regress the
    // common case). ----------
    {
        gba::Bus bus;
        gba::Ppu ppu(bus);
        DisableAllObjects(bus);

        SetupWideSprite(bus, 0, 0, 1);
        bus.Write16(gba::mem::kPaletteBase + 0x200u + 1u * 16u * 2u + 1u * 2u, 0x001Fu); // red
        SetupWideSprite(bus, 1, 70, 2);
        bus.Write16(gba::mem::kPaletteBase + 0x200u + 2u * 16u * 2u + 1u * 2u, 0x03E0u); // green
        SetupWideSprite(bus, 2, 140, 3);
        bus.Write16(gba::mem::kPaletteBase + 0x200u + 3u * 16u * 2u + 1u * 2u, 0x7C00u); // blue

        // OBJ enabled, 1D mapping, mode 0.
        bus.Write16(gba::mem::kIoBase + gba::io::kDispcnt, static_cast<gba::u16>((1u << 12) | (1u << 6)));

        for (int l = 0; l < gba::Ppu::kScreenHeight; ++l) ppu.RenderScanline(l);

        CheckPixel(ppu, 10, 0, ExpectedRgba(0x001Fu), "sprite 0 renders (well under budget)");
        CheckPixel(ppu, 80, 0, ExpectedRgba(0x03E0u), "sprite 1 renders (well under budget)");
        CheckPixel(ppu, 150, 0, ExpectedRgba(0x7C00u), "sprite 2 renders (well under budget)");
    }

    // ---------- Cutoff: enough 64x64 sprites on one scanline to exceed
    // the 1210-cycle budget (HBlank Interval Free = 0). Per the cost
    // model, a 64-wide normal sprite costs (64-2)=62 cycles plus 2 for
    // being scanned, so roughly 19 of them (0..18) fit in 1210 - a low
    // index like 2 should still render, a high index like 50 is nowhere
    // close and should be dropped entirely, giving comfortable margin
    // against off-by-one uncertainty in that estimate. ----------
    {
        gba::Bus bus;
        gba::Ppu ppu(bus);
        DisableAllObjects(bus);

        // Filler sprites (indices 0..59, all overlapping scanline 0)
        // purely to burn through the cycle budget - placed fully
        // on-screen (so each one costs its full, unclipped width - a
        // sprite clipped by the left screen edge costs less, which
        // would throw off the budget arithmetic below) at x=80,
        // spanning [80,144), well clear of the two indices actually
        // checked below ([10,74) and [150,214)) so they never visibly
        // overlap them.
        for (int i = 0; i <= 59; ++i) {
            SetupWideSprite(bus, i, 80, 4);
        }
        bus.Write16(gba::mem::kPaletteBase + 0x200u + 4u * 16u * 2u + 1u * 2u, 0x001Fu); // filler color (unused)

        // Index 2: comfortably within budget - must render.
        SetupWideSprite(bus, 2, 10, 5);
        bus.Write16(gba::mem::kPaletteBase + 0x200u + 5u * 16u * 2u + 1u * 2u, 0x03E0u); // green

        // Index 50: comfortably beyond budget - must NOT render.
        SetupWideSprite(bus, 50, 150, 6);
        bus.Write16(gba::mem::kPaletteBase + 0x200u + 6u * 16u * 2u + 1u * 2u, 0x7C00u); // blue

        // Backdrop color, so the "not rendered" check has an unambiguous
        // expected value distinct from both sprite colors.
        bus.Write16(gba::mem::kPaletteBase, 0x0000u); // black

        // OBJ enabled, 1D mapping, mode 0, HBlank Interval Free = 0
        // (bit5 clear -> the larger 1210-cycle budget).
        bus.Write16(gba::mem::kIoBase + gba::io::kDispcnt, static_cast<gba::u16>((1u << 12) | (1u << 6)));

        for (int l = 0; l < gba::Ppu::kScreenHeight; ++l) ppu.RenderScanline(l);

        CheckPixel(ppu, 20, 0, ExpectedRgba(0x03E0u), "low OAM index (2) still renders once budget is nearly full of fillers");
        CheckPixel(ppu, 160, 0, ExpectedRgba(0x0000u), "high OAM index (50) is dropped once the scanline's cycle budget runs out");
    }

    if (failures == 0) {
        std::printf("PASS: per-scanline OBJ rendering cycle budget drops late-OAM sprites once exhausted\n");
    }
    return failures;
}
