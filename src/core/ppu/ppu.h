#pragma once

#include <array>

#include "core/memory/bus.h"
#include "core/state.h"
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

    // Renders exactly one visible scanline (0-159) directly into
    // framebuffer_, using whatever register state is current right now.
    // Emulator::RunFrame() calls this once per line, right as that line's
    // draw portion ends and HBlank begins - so mid-frame writes (scroll,
    // palette, window/blend registers, and the BG2X/Y "raster split"
    // trick some games use) take effect starting on the correct line
    // instead of only being visible next frame. Line 0 also reloads the
    // affine reference-point accumulators for a fresh frame - see their
    // declaration below.
    void RenderScanline(int line);

    // RGBA8888 framebuffer, ready to hand to SDL as a texture.
    const std::array<u32, kScreenWidth * kScreenHeight>& Framebuffer() const {
        return framebuffer_;
    }

    // Save-state support: the visible framebuffer (so a reloaded state
    // shows the right image immediately, rather than one stale/blank
    // frame until the next RunFrame()) plus the affine reference-point
    // accumulators - everything else (bgLayer_/objLayer_/etc.) is
    // per-scanline scratch space RenderScanline() fully overwrites every
    // call, not persistent state.
    void SaveState(StateWriter& w) const;
    void LoadState(StateReader& r);

private:
    Bus& bus_;
    std::array<u32, kScreenWidth * kScreenHeight> framebuffer_{};

    // Per-pixel scratch buffers used while compositing one scanline - only
    // one line wide/deep now that rendering happens line by line rather
    // than a whole frame at once.
    struct LayerPixel {
        u32 color = 0;
        bool opaque = false;
    };
    std::array<LayerPixel, kScreenWidth> bgLayer_[4]{};
    std::array<LayerPixel, kScreenWidth> objLayer_{};
    std::array<u8, kScreenWidth> objPriority_{};

    // Set wherever an OBJ-window-mode sprite (attr0 OBJ Mode == 2) is
    // opaque on this scanline - such sprites draw nothing themselves, they
    // only carve out a region where WINOUT's "inside OBJ window" enable
    // bits apply instead of its "outside all windows" bits. Set wherever
    // the topmost OBJ pixel came from a semi-transparent sprite (OBJ Mode
    // == 1), which forces alpha-blending with whatever is beneath it
    // regardless of BLDCNT's own effect-mode selection. Both rebuilt from
    // scratch every RenderSprites() call.
    std::array<bool, kScreenWidth> objWindowMask_{};
    std::array<bool, kScreenWidth> objSemiTransparent_{};

    // Internal affine reference-point accumulators for BG2/BG3 (GBATEK "BG
    // Rotation/Scaling Parameters" describes these as separate "internal"
    // registers, distinct from the BGxX/Y I/O registers a game writes).
    // Real hardware reloads them from BGxX/Y at the start of every frame,
    // AND immediately on any CPU write to BGxX/Y mid-frame - the latter is
    // how games implement "raster split" effects (a different
    // rotation/scale per scanline range). Bus doesn't notify Ppu of
    // writes, so a write is detected indirectly: if BGxX/Y no longer reads
    // back what we last saw, the game must have written it since we last
    // checked, so we reload from it. Between reloads, the accumulator just
    // advances by PB/PD once per scanline. Mode 5 reuses BG2's pair (its
    // rotation/scale hardware is the same BG2 affine unit, just sampling a
    // bitmap instead of tiles).
    s32 bg2RefX_ = 0, bg2RefY_ = 0;
    u32 bg2LastRawX_ = 0, bg2LastRawY_ = 0;
    s32 bg3RefX_ = 0, bg3RefY_ = 0;
    u32 bg3LastRawX_ = 0, bg3LastRawY_ = 0;

    // Mode 3: BG2 is a single 240x160 16-bit-color bitmap, one pixel per
    // VRAM halfword, no palette indirection. GBATEK "BG Mode 3 - 16bit
    // Bitmap". Like Modes 0-2, OBJ sprites still render on top of it (and
    // through windowing/blending) - the bitmap is fed into bgLayer_[2] as
    // an always-opaque "background" and composited via the same
    // RenderSprites()/CompositeLayers() calls the tiled modes use.
    void RenderMode3(int line);

    // Mode 4: BG2 is a 240x160 8-bit-per-pixel paletted bitmap (indices
    // into the BG palette), double-buffered - DISPCNT bit4 selects which
    // of the two VRAM frames (0x06000000 or 0x0600A000) is currently
    // visible, letting a game draw into the hidden one and flip instantly
    // instead of racing the beam. GBATEK "BG Mode 4 - 256 color Bitmap".
    // Composited the same way as Mode 3 - see its comment.
    void RenderMode4(int line);

    // Mode 0: up to four regular ("text mode") tiled backgrounds plus OBJ
    // sprites, composited by priority. This is the mode the large majority
    // of commercial GBA games actually use. GBATEK "Text BG" / "OBJs".
    void RenderMode0(int line);

    // Mode 1: BG0/BG1 regular (text) as in Mode 0, BG2 affine (rotate/
    // scale), BG3 doesn't exist in this mode. GBATEK "BG Modes".
    void RenderMode1(int line);

    // Mode 2: BG0/BG1 don't exist in this mode, BG2 and BG3 are both
    // affine. Used by games that need two independently-rotating tiled
    // layers (e.g. a rotating floor plus a separate rotating backdrop).
    void RenderMode2(int line);

    // Mode 5: like Mode 4 (paletted... actually 16bpp here, double-
    // buffered via DISPCNT bit4), but the bitmap is only 160x128 - smaller
    // than the screen - and, being BG2, goes through the same affine
    // transform as Modes 1/2 rather than being blitted 1:1 like Mode 3/4.
    // GBATEK "BG Mode 5 - Rot/Scale Bitmap". Composited the same way as
    // Mode 3/4 - see Mode 3's comment.
    void RenderMode5(int line);

    // Fills bgLayer_[bgIndex] with one background's pixels for this
    // scanline, honoring its control register (tile/screen base, size,
    // color depth) and HOFS/VOFS scroll registers (read fresh each line -
    // unlike BGxX/Y, GBATEK doesn't describe an internal latch for these).
    // Transparent pixels (color index 0) are left with opaque=false so the
    // compositor can see through them.
    void RenderTextBackground(int bgIndex, int line);

    // Fills bgLayer_[bgIndex] for an affine (rotate/scale) background -
    // BG2 in modes 1/2/5, BG3 in mode 2. Unlike text-mode backgrounds,
    // affine backgrounds are always 8bpp, use a single square screen
    // (16x16 to 128x128 tiles, no sub-block layout), 1-byte-per-entry
    // tilemaps (tile number only - no flip/palette bits), and are sampled
    // through the BGxPA-PD/BGxX/BGxY rotation/scaling registers rather
    // than HOFS/VOFS - see the reference-point accumulators' declaration
    // above for how the per-line reference point is tracked. GBATEK "BG
    // Rotation/Scaling".
    void RenderAffineBackground(int bgIndex, int line);

    // Fills objLayer_/objPriority_/objWindowMask_/objSemiTransparent_ from
    // OAM for this one scanline - all 128 sprites are checked, but only
    // those whose bounding box actually covers this line contribute any
    // pixels. Processes OAM back-to-front (index 127 down to 0) so that,
    // per GBATEK, lower OAM index wins ties at equal priority. Honors each
    // sprite's OBJ Mode (normal / semi-transparent / OBJ-window) and
    // mosaic flag.
    //
    // TODO: real hardware only draws up to 128 sprite dots per scanline
    // (fewer if any are affine) and drops the rest - not modeled, so an
    // unusually sprite-heavy line renders fully here instead of the
    // glitches/missing sprites real hardware would show.
    void RenderSprites(int line);

    // Combines up to 4 BG layers (using each BG's configured priority,
    // lower BG index wins ties) and the OBJ layer (which wins ties against
    // a same-priority BG) into this scanline of framebuffer_. Pixels with
    // nothing opaque fall back to the backdrop color (palette entry 0).
    // Also applies, when enabled: Win0/Win1/OBJ-window per-pixel layer
    // visibility, and BLDCNT/BLDALPHA/BLDY color special effects (alpha
    // blend, brightness increase/decrease, and forced blending under
    // semi-transparent OBJs).
    void CompositeLayers(int line, const bool bgEnabled[4], const u8 bgPriority[4], bool objEnabled);
};

} // namespace gba
