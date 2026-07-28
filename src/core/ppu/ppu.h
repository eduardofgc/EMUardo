#pragma once

#include <array>

#include "core/memory/bus.h"
#include "core/types.h"

namespace gba {

// The GBA screen is a fixed 240x160 pixels, always. Background mode
// (0-5) is selected via I/O register DISPCNT and changes how the four
// background layers are interpreted (tiled vs bitmap, palette depth, etc).
class Ppu {
public:
    static constexpr int kScreenWidth  = 240;
    static constexpr int kScreenHeight = 160;

    explicit Ppu(Bus& bus);

    // Advances the PPU by one scanline's worth of dots. The GBA runs at
    // ~59.7 Hz with 228 scanlines per frame (160 visible + 68 VBlank),
    // 1232 dots per scanline - the real implementation will be driven by
    // cycle counts from the main loop rather than being called per-line
    // directly, but this is the right seam for now.
    //
    // TODO: currently a no-op. Once this is genuinely scanline-driven,
    // RenderFrame()'s per-mode renderers should move here, line by line,
    // instead of running once at the end of a frame.
    void Step();

    // Reads DISPCNT and redraws the whole framebuffer for the current
    // mode. Called once per frame by Emulator::RunFrame() - not yet
    // scanline-accurate (see the TODO on Step()), but correct in the
    // "final image looks right" sense for a static frame, which is what
    // Mode 3/4 bitmap modes mostly need.
    void RenderFrame();

    // RGBA8888 framebuffer, ready to hand to SDL as a texture.
    const std::array<u32, kScreenWidth * kScreenHeight>& Framebuffer() const {
        return framebuffer_;
    }

private:
    Bus& bus_;
    std::array<u32, kScreenWidth * kScreenHeight> framebuffer_{};

    // Mode 3: BG2 is a single 240x160 16-bit-color bitmap, one pixel per
    // VRAM halfword, no palette indirection. GBATEK "BG Mode 3 - 16bit
    // Bitmap". This is the simplest of the six modes and the natural
    // first one to implement.
    void RenderMode3();

    // TODO: Mode 4 (paletted bitmap, double-buffered), Mode 5 (smaller
    // 16-bit bitmap, double-buffered), Modes 0-2 (tiled backgrounds +
    // sprites - the ones real commercial games mostly use), DISPSTAT/
    // VCOUNT register updates, per-scanline timing.
};

} // namespace gba
