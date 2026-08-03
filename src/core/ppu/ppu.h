#pragma once

#include <array>

#include "core/types.h"

namespace gba {

// The GBA screen is a fixed 240x160 pixels, always. Background mode
// (0-5) is selected via I/O register DISPCNT and changes how the four
// background layers are interpreted (tiled vs bitmap, palette depth, etc).
class Ppu {
public:
    static constexpr int kScreenWidth  = 240;
    static constexpr int kScreenHeight = 160;

    Ppu();

    // Advances the PPU by one scanline's worth of dots. The GBA runs at
    // ~59.7 Hz with 228 scanlines per frame (160 visible + 68 VBlank),
    // 1232 dots per scanline - the real implementation will be driven by
    // cycle counts from the main loop rather than being called per-line
    // directly, but this is the right seam for now.
    void Step();

    // RGBA8888 framebuffer, ready to hand to SDL as a texture.
    const std::array<u32, kScreenWidth * kScreenHeight>& Framebuffer() const {
        return framebuffer_;
    }

private:
    std::array<u32, kScreenWidth * kScreenHeight> framebuffer_{};

    // TODO: DISPCNT/DISPSTAT/VCOUNT registers, per-mode renderers
    // (Mode 3/4 bitmap first, then Mode 0/1/2 tiled+sprites), access to
    // the Bus for VRAM/OAM/palette reads.
};

} // namespace gba
