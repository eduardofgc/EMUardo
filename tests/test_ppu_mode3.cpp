// Verifies Ppu::RenderFrame() for Mode 3: writes known BGR555 colors
// directly into VRAM (as if a game had just drawn them), sets DISPCNT to
// select Mode 3, and checks the resulting framebuffer holds the correctly
// converted RGBA8888 pixels at the right offsets.

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

} // namespace

int main() {
    gba::Bus bus;
    gba::Ppu ppu(bus);

    // Select Mode 3, enable BG2 (bit 10) - DISPCNT bit layout per GBATEK
    // "LCD I/O Registers". BG2 enable isn't actually consulted by
    // RenderMode3() yet (see its TODO), but setting it anyway keeps this
    // test honest about what a real game would do.
    const gba::u16 dispcntModeThreeBg2 = 0x0003u | (1u << 10);
    bus.Write16(gba::mem::kIoBase + gba::io::kDispcnt, dispcntModeThreeBg2);

    // Pure red (BGR555: R=31,G=0,B=0 -> 0x001F), top-left pixel.
    bus.Write16(gba::mem::kVramBase + 0, 0x001Fu);
    // Pure green (G=31 at bits 9-5 -> 0x03E0), pixel (1,0).
    bus.Write16(gba::mem::kVramBase + 2, 0x03E0u);
    // Pure blue (B=31 at bits 14-10 -> 0x7C00), pixel (2,0).
    bus.Write16(gba::mem::kVramBase + 4, 0x7C00u);
    // White (all channels max -> 0x7FFF), one full row down: pixel (0,1).
    const gba::u32 rowOneOffset = static_cast<gba::u32>(gba::Ppu::kScreenWidth) * 2u;
    bus.Write16(gba::mem::kVramBase + rowOneOffset, 0x7FFFu);

    ppu.RenderFrame();

    CheckPixel(ppu, 0, 0, 0xFF0000FFu, "red pixel");
    CheckPixel(ppu, 1, 0, 0xFF00FF00u, "green pixel");
    CheckPixel(ppu, 2, 0, 0xFFFF0000u, "blue pixel");
    CheckPixel(ppu, 0, 1, 0xFFFFFFFFu, "white pixel one row down");

    // A pixel we never wrote should read as VRAM's zero-initialized state,
    // i.e. BGR555 0x0000 -> opaque black - confirms we're not leaking
    // stale placeholder-gray data into real rendered frames.
    CheckPixel(ppu, 239, 159, 0xFF000000u, "untouched pixel (bottom-right corner)");

    if (failures == 0) {
        std::printf("PASS: Mode 3 bitmap rendering (BGR555 -> RGBA8888, correct VRAM offsets)\n");
    }
    return failures;
}
