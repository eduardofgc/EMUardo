// Covers the gap fixed this session: OBJ sprites weren't composited on
// top of Mode 3/4/5 bitmap backgrounds at all (RenderMode3/4/5 wrote
// straight to the framebuffer and never called RenderSprites()). This
// checks a sprite drawn over a Mode 3 bitmap, a sprite hidden behind a
// higher-priority Mode 4 bitmap pixel (priority ordering still honored),
// and that Mode 3/4's bitmap only appears when DISPCNT's BG2 enable bit is
// actually set (previously ignored).

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
    // ---------- Sprite drawn over a Mode 3 bitmap ----------
    {
        gba::Bus bus;
        gba::Ppu ppu(bus);
        DisableAllObjects(bus);

        // Mode 3 bitmap: whole screen red.
        for (int y = 0; y < gba::Ppu::kScreenHeight; ++y) {
            for (int x = 0; x < gba::Ppu::kScreenWidth; ++x) {
                const gba::u32 addr = gba::mem::kVramBase +
                    static_cast<gba::u32>(y * gba::Ppu::kScreenWidth + x) * 2u;
                bus.Write16(addr, 0x001Fu); // red
            }
        }

        // 8x8, 8bpp sprite at (0,0), default priority 0 - beats BG2's
        // default priority 0 by winning ties (OBJ wins ties vs BG).
        bus.Write16(gba::mem::kOamBase + 0u, static_cast<gba::u16>(1u << 13));
        bus.Write16(gba::mem::kOamBase + 2u, 0u);
        bus.Write16(gba::mem::kOamBase + 4u, 0u);
        const gba::u32 objTile0Addr = gba::mem::kVramBase + 0x1'4000u; // bitmap-mode OBJ base
        bus.Write8(objTile0Addr, 7u);
        bus.Write16(gba::mem::kPaletteBase + 0x200u + 7u * 2u, 0x7C00u); // blue

        // Mode 3, BG2 enabled, OBJ enabled, 1D mapping.
        const gba::u16 dispcnt = 3u | (1u << 10) | (1u << 12) | (1u << 6);
        bus.Write16(gba::mem::kIoBase + gba::io::kDispcnt, dispcnt);

        ppu.RenderFrame();

        CheckPixel(ppu, 0, 0, ExpectedRgba(0x7C00u), "sprite pixel wins over Mode 3 bitmap at (0,0)");
        CheckPixel(ppu, 20, 20, ExpectedRgba(0x001Fu), "Mode 3 bitmap shows through where the sprite doesn't cover it");
    }

    // ---------- Sprite hidden behind a higher-priority Mode 4 bitmap ----------
    {
        gba::Bus bus;
        gba::Ppu ppu(bus);
        DisableAllObjects(bus);

        bus.Write16(gba::mem::kPaletteBase + 5u * 2u, 0x03E0u); // index 5 -> green
        bus.Write8(gba::mem::kVramBase, 5u); // pixel (0,0) = green

        // Sprite at (0,0), explicit priority 3 (worst) - BG2CNT priority
        // left at its default 0 (best), so the bitmap should win here even
        // though OBJ normally wins ties, because priority 0 beats 3.
        const gba::u16 spritePriority = 3u << 10;
        bus.Write16(gba::mem::kOamBase + 0u, static_cast<gba::u16>(1u << 13));
        bus.Write16(gba::mem::kOamBase + 2u, 0u);
        bus.Write16(gba::mem::kOamBase + 4u, spritePriority);
        const gba::u32 objTile0Addr = gba::mem::kVramBase + 0x1'4000u;
        bus.Write8(objTile0Addr, 7u);
        bus.Write16(gba::mem::kPaletteBase + 0x200u + 7u * 2u, 0x7C00u); // blue

        const gba::u16 dispcnt = 4u | (1u << 10) | (1u << 12) | (1u << 6);
        bus.Write16(gba::mem::kIoBase + gba::io::kDispcnt, dispcnt);

        ppu.RenderFrame();

        CheckPixel(ppu, 0, 0, ExpectedRgba(0x03E0u),
                   "higher-priority (lower number) Mode 4 bitmap wins over a lower-priority sprite");
    }

    // ---------- Mode 3 bitmap doesn't display unless BG2 is enabled ----------
    {
        gba::Bus bus;
        gba::Ppu ppu(bus);
        DisableAllObjects(bus);

        bus.Write16(gba::mem::kVramBase, 0x001Fu); // red at (0,0)

        // Mode 3 but BG2 (bit10) left clear.
        bus.Write16(gba::mem::kIoBase + gba::io::kDispcnt, 3u);
        ppu.RenderFrame();

        CheckPixel(ppu, 0, 0, ExpectedRgba(bus.Read16(gba::mem::kPaletteBase)),
                   "Mode 3 bitmap stays hidden when DISPCNT's BG2 enable bit is clear");
    }

    if (failures == 0) {
        std::printf("PASS: OBJ sprites composite over Mode 3/4 bitmaps, respecting priority and BG2 enable\n");
    }
    return failures;
}
