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

    // Per-pixel scratch buffers used while compositing a Mode 0 frame -
    // members rather than locals so a single frame's render doesn't need
    // four 240x160-element arrays living on the stack at once.
    struct LayerPixel {
        u32 color = 0;
        bool opaque = false;
    };
    std::array<LayerPixel, kScreenWidth * kScreenHeight> bgLayer_[4]{};
    std::array<LayerPixel, kScreenWidth * kScreenHeight> objLayer_{};
    std::array<u8, kScreenWidth * kScreenHeight> objPriority_{};

    // Mode 3: BG2 is a single 240x160 16-bit-color bitmap, one pixel per
    // VRAM halfword, no palette indirection. GBATEK "BG Mode 3 - 16bit
    // Bitmap". This is the simplest of the six modes and the natural
    // first one to implement.
    void RenderMode3();

    // Mode 4: BG2 is a 240x160 8-bit-per-pixel paletted bitmap (indices
    // into the BG palette), double-buffered - DISPCNT bit4 selects which
    // of the two VRAM frames (0x06000000 or 0x0600A000) is currently
    // visible, letting a game draw into the hidden one and flip instantly
    // instead of racing the beam. GBATEK "BG Mode 4 - 256 color Bitmap".
    void RenderMode4();

    // Mode 0: up to four regular ("text mode") tiled backgrounds plus OBJ
    // sprites, composited by priority. This is the mode the large majority
    // of commercial GBA games actually use. GBATEK "Text BG" / "OBJs".
    void RenderMode0();

    // Mode 1: BG0/BG1 regular (text) as in Mode 0, BG2 affine (rotate/
    // scale), BG3 doesn't exist in this mode. GBATEK "BG Modes".
    void RenderMode1();

    // Mode 2: BG0/BG1 don't exist in this mode, BG2 and BG3 are both
    // affine. Used by games that need two independently-rotating tiled
    // layers (e.g. a rotating floor plus a separate rotating backdrop).
    void RenderMode2();

    // Mode 5: like Mode 4 (paletted... actually 16bpp here, double-
    // buffered via DISPCNT bit4), but the bitmap is only 160x128 - smaller
    // than the screen - and, being BG2, goes through the same affine
    // transform as Modes 1/2 rather than being blitted 1:1 like Mode 3/4.
    // GBATEK "BG Mode 5 - Rot/Scale Bitmap".
    void RenderMode5();

    // Fills bgLayer_[bgIndex] with one background's pixels for this frame,
    // honoring its control register (tile/screen base, size, color depth)
    // and HOFS/VOFS scroll registers. Transparent pixels (color index 0)
    // are left with opaque=false so the compositor can see through them.
    void RenderTextBackground(int bgIndex);

    // Fills bgLayer_[bgIndex] for an affine (rotate/scale) background -
    // BG2 in modes 1/2/5, BG3 in mode 2. Unlike text-mode backgrounds,
    // affine backgrounds are always 8bpp, use a single square screen
    // (16x16 to 128x128 tiles, no sub-block layout), 1-byte-per-entry
    // tilemaps (tile number only - no flip/palette bits), and are sampled
    // through the BGxPA-PD/BGxX/BGxY rotation/scaling registers rather
    // than HOFS/VOFS. GBATEK "BG Rotation/Scaling".
    void RenderAffineBackground(int bgIndex);

    // Fills objLayer_/objPriority_ from OAM - all 128 sprites, both
    // regular and affine (rotate/scale). Processes OAM back-to-front
    // (index 127 down to 0) so that, per GBATEK, lower OAM index wins ties
    // at equal priority.
    //
    // TODO: OBJ window mode, semi-transparent OBJ mode, mosaic - still
    // ignored.
    void RenderSprites();

    // Combines up to 4 BG layers (using each BG's configured priority,
    // lower BG index wins ties) and the OBJ layer (which wins ties against
    // a same-priority BG) into framebuffer_. Pixels with nothing opaque
    // fall back to the backdrop color (palette entry 0).
    void CompositeLayers(const bool bgEnabled[4], const u8 bgPriority[4], bool objEnabled);

    // TODO: per-scanline timing (see Step()'s TODO) - affine reference
    // points are currently re-read once per frame and extrapolated by
    // line number, so mid-frame writes to BGxX/Y (a common raster-effect
    // trick) won't take effect until the next frame.
};

} // namespace gba
