#include "core/ppu/ppu.h"

#include <algorithm>
#include <cstdio>

namespace gba {

namespace {
// GBA colors are 15-bit BGR: bit15 unused, bits14-10 = Blue, bits9-5 =
// Green, bits4-0 = Red (GBATEK "GBA Color Component Layout"). Expanding a
// 5-bit channel to 8 bits by (c<<3)|(c>>2) spreads the top 3 bits into the
// low end too, so 0x1F maps to 0xFF instead of 0xF8 - a flat left-shift
// alone would never reach full brightness.
u32 Bgr555ToRgba8888(u16 color) {
    const u32 r5 = color & 0x1Fu;
    const u32 g5 = (color >> 5) & 0x1Fu;
    const u32 b5 = (color >> 10) & 0x1Fu;

    const u32 r8 = (r5 << 3) | (r5 >> 2);
    const u32 g8 = (g5 << 3) | (g5 >> 2);
    const u32 b8 = (b5 << 3) | (b5 >> 2);

    // Packed to match SDL_PIXELFORMAT_ABGR8888 as used in main.cpp: on a
    // little-endian machine that format's in-memory byte order is
    // R,G,B,A, which is exactly what (A<<24)|(B<<16)|(G<<8)|R produces as
    // a native u32 value.
    return 0xFF00'0000u | (b8 << 16) | (g8 << 8) | r8;
}

// BGxX/BGxY reference-point registers are 32-bit but only bits 0-27 are
// meaningful (20.8 fixed point); bits 28-31 must be sign-extended from
// bit 27 rather than read as part of the magnitude. GBATEK "BG
// Rotation/Scaling Parameters".
s32 SignExtend28(u32 value) {
    value &= 0x0FFF'FFFFu;
    if (value & 0x0800'0000u) {
        value |= 0xF000'0000u;
    }
    return static_cast<s32>(value);
}

// Wraparound for affine backgrounds with the overflow bit set - C++'s %
// can return a negative result for a negative dividend, which isn't a
// valid array index.
s32 FloorMod(s32 value, s32 modulus) {
    const s32 result = value % modulus;
    return (result < 0) ? result + modulus : result;
}

// Win0H/Win1H/Win0V/Win1V each pack (start<<8)|endExclusive. A raw end
// past the screen edge clamps to it; start>end is not an error but wraps
// the window around the screen edge instead, per GBATEK "Windows".
bool InWindowRange(int coord, u8 start, u8 endExclusiveRaw, int screenSize) {
    const int endExclusive = (endExclusiveRaw > screenSize) ? screenSize : endExclusiveRaw;
    if (start <= endExclusive) {
        return coord >= start && coord < endExclusive;
    }
    return coord >= start || coord < endExclusive;
}

// Blends two already-expanded RGBA8888 colors channel-by-channel, per
// GBATEK "Color Special Effects": I = MIN(31, (A*EVA + B*EVB) / 16) in
// 5-bit hardware terms - equivalent in 8-bit terms since EVA/EVB are
// already clamped to [0,16] by the caller.
u32 AlphaBlend(u32 top, u32 bottom, s32 eva, s32 evb) {
    auto blendChannel = [&](u32 shift) -> u32 {
        const s32 t = static_cast<s32>((top >> shift) & 0xFFu);
        const s32 b = static_cast<s32>((bottom >> shift) & 0xFFu);
        s32 result = (t * eva + b * evb) / 16;
        if (result > 255) result = 255;
        return static_cast<u32>(result) << shift;
    };
    return 0xFF00'0000u | blendChannel(16) | blendChannel(8) | blendChannel(0);
}

// Fades a color toward white (BLDCNT effect 2) or black (effect 3) by
// EVY/16.
u32 BrightnessIncrease(u32 color, s32 evy) {
    auto blendChannel = [&](u32 shift) -> u32 {
        const s32 c = static_cast<s32>((color >> shift) & 0xFFu);
        s32 result = c + ((255 - c) * evy) / 16;
        if (result > 255) result = 255;
        return static_cast<u32>(result) << shift;
    };
    return 0xFF00'0000u | blendChannel(16) | blendChannel(8) | blendChannel(0);
}

u32 BrightnessDecrease(u32 color, s32 evy) {
    auto blendChannel = [&](u32 shift) -> u32 {
        const s32 c = static_cast<s32>((color >> shift) & 0xFFu);
        s32 result = c - (c * evy) / 16;
        if (result < 0) result = 0;
        return static_cast<u32>(result) << shift;
    };
    return 0xFF00'0000u | blendChannel(16) | blendChannel(8) | blendChannel(0);
}
} // namespace

Ppu::Ppu(Bus& bus) : bus_(bus) {
    // Placeholder color so it's obvious in the SDL window that the
    // pipeline is wired up correctly, before RenderScanline() has run.
    framebuffer_.fill(0xFF30'3030);
}

void Ppu::SaveState(StateWriter& w) const {
    w.Write(framebuffer_);
    w.Write(bg2RefX_);
    w.Write(bg2RefY_);
    w.Write(bg2LastRawX_);
    w.Write(bg2LastRawY_);
    w.Write(bg3RefX_);
    w.Write(bg3RefY_);
    w.Write(bg3LastRawX_);
    w.Write(bg3LastRawY_);
}

void Ppu::LoadState(StateReader& r) {
    r.Read(framebuffer_);
    r.Read(bg2RefX_);
    r.Read(bg2RefY_);
    r.Read(bg2LastRawX_);
    r.Read(bg2LastRawY_);
    r.Read(bg3RefX_);
    r.Read(bg3RefY_);
    r.Read(bg3LastRawX_);
    r.Read(bg3LastRawY_);
}

void Ppu::RenderScanline(int line) {
    const u16 dispcnt = bus_.Read16(mem::kIoBase + io::kDispcnt);
    const u32 mode = dispcnt & 0x7u;

    switch (mode) {
        case 0:
            RenderMode0(line);
            break;
        case 1:
            RenderMode1(line);
            break;
        case 2:
            RenderMode2(line);
            break;
        case 3:
            RenderMode3(line);
            break;
        case 4:
            RenderMode4(line);
            break;
        case 5:
            RenderMode5(line);
            break;
        default: {
            // Mode 6/7 don't exist on real hardware (DISPCNT mode is only
            // 3 bits, but the two unused encodings are still reachable by
            // a game writing garbage) - fill with the same placeholder
            // color used before any rendering existed, so "invalid mode"
            // and "nothing rendered yet" look identically obvious rather
            // than showing stale data from a previous frame.
            static bool warned = false;
            if (!warned) {
                std::fprintf(stderr, "Ppu::RenderScanline: mode %u not implemented yet\n", mode);
                warned = true;
            }
            const std::size_t rowBase = static_cast<std::size_t>(line) * kScreenWidth;
            for (int x = 0; x < kScreenWidth; ++x) {
                framebuffer_[rowBase + static_cast<std::size_t>(x)] = 0xFF30'3030;
            }
            break;
        }
    }
}

void Ppu::RenderMode3(int line) {
    // Mode 3: BG2 only, one 16-bit BGR555 pixel per screen pixel, laid out
    // left-to-right/top-to-bottom starting at the base of VRAM - no tiles,
    // no palette, no scrolling registers. GBATEK "BG Mode 3 - 16bit
    // Bitmap". Like Modes 0-2, sprites can be drawn on top of the bitmap,
    // so this fills bgLayer_[2] (always fully opaque - a bitmap has no
    // transparent color index) and goes through the same
    // RenderSprites()/CompositeLayers() path rather than writing
    // framebuffer_ directly.
    const u16 dispcnt = bus_.Read16(mem::kIoBase + io::kDispcnt);
    const bool bg2Enabled = (dispcnt & (1u << 10)) != 0;
    bool bgEnabled[4] = {false, false, bg2Enabled, false};
    u8 bgPriority[4] = {0, 0, 0, 0};

    if (bg2Enabled) {
        const u16 control = bus_.Read16(mem::kIoBase + io::kBg2Cnt);
        bgPriority[2] = static_cast<u8>(control & 0x3u);

        auto& layer = bgLayer_[2];
        for (int x = 0; x < kScreenWidth; ++x) {
            const std::size_t pixelIndex = static_cast<std::size_t>(line * kScreenWidth + x);
            const u32 address = mem::kVramBase + static_cast<u32>(pixelIndex) * 2u;
            layer[static_cast<std::size_t>(x)].opaque = true;
            layer[static_cast<std::size_t>(x)].color = Bgr555ToRgba8888(bus_.Read16(address));
        }
    }

    const bool objEnabled = (dispcnt & (1u << 12)) != 0;
    if (objEnabled) {
        RenderSprites(line);
    }
    CompositeLayers(line, bgEnabled, bgPriority, objEnabled);
}

void Ppu::RenderMode4(int line) {
    // Mode 4: BG2 is a 240x160 8bpp paletted bitmap - one byte per pixel,
    // indexing the BG palette (same palette bank Mode 0's 8bpp
    // backgrounds use). Double-buffered: DISPCNT bit4 selects which of
    // the two VRAM frames is currently visible. Composited the same way
    // as Mode 3 - see its comment.
    const u16 dispcnt = bus_.Read16(mem::kIoBase + io::kDispcnt);
    const bool bg2Enabled = (dispcnt & (1u << 10)) != 0;
    bool bgEnabled[4] = {false, false, bg2Enabled, false};
    u8 bgPriority[4] = {0, 0, 0, 0};

    if (bg2Enabled) {
        const bool secondFrame = (dispcnt & (1u << 4)) != 0;
        const u32 frameBase = mem::kVramBase + (secondFrame ? 0xA000u : 0u);
        const u16 control = bus_.Read16(mem::kIoBase + io::kBg2Cnt);
        bgPriority[2] = static_cast<u8>(control & 0x3u);

        auto& layer = bgLayer_[2];
        for (int x = 0; x < kScreenWidth; ++x) {
            const std::size_t pixelIndex = static_cast<std::size_t>(line * kScreenWidth + x);
            const u8 colorIndex = bus_.Read8(frameBase + static_cast<u32>(pixelIndex));
            const u32 paletteAddr = mem::kPaletteBase + static_cast<u32>(colorIndex) * 2u;
            layer[static_cast<std::size_t>(x)].opaque = true;
            layer[static_cast<std::size_t>(x)].color = Bgr555ToRgba8888(bus_.Read16(paletteAddr));
        }
    }

    const bool objEnabled = (dispcnt & (1u << 12)) != 0;
    if (objEnabled) {
        RenderSprites(line);
    }
    CompositeLayers(line, bgEnabled, bgPriority, objEnabled);
}

void Ppu::RenderMode5(int line) {
    // Mode 5: BG2 only, 16bpp direct color like Mode 3, double-buffered
    // like Mode 4, but the bitmap is 160x128 - smaller than the 240x160
    // screen - and goes through the same affine (rotate/scale) transform
    // as Modes 1/2 rather than a straight 1:1 blit. GBATEK "BG Mode 5".
    // Composited the same way as Mode 3/4 - see Mode 3's comment. Pixels
    // outside the 160x128 bitmap (when the overflow/wrap bit is clear)
    // are left non-opaque so the compositor's own backdrop fallback
    // handles them, rather than writing the backdrop color here directly.
    static constexpr int kBitmapWidth = 160;
    static constexpr int kBitmapHeight = 128;

    const u16 dispcnt = bus_.Read16(mem::kIoBase + io::kDispcnt);
    const bool bg2Enabled = (dispcnt & (1u << 10)) != 0;
    bool bgEnabled[4] = {false, false, bg2Enabled, false};
    u8 bgPriority[4] = {0, 0, 0, 0};

    if (bg2Enabled) {
        const bool secondFrame = (dispcnt & (1u << 4)) != 0;
        const u32 frameBase = mem::kVramBase + (secondFrame ? 0xA000u : 0u);

        const u16 control = bus_.Read16(mem::kIoBase + io::kBg2Cnt);
        bgPriority[2] = static_cast<u8>(control & 0x3u);
        const bool wrap = (control & (1u << 13)) != 0;

        const s16 pa = static_cast<s16>(bus_.Read16(mem::kIoBase + io::kBg2Pa));
        const s16 pb = static_cast<s16>(bus_.Read16(mem::kIoBase + io::kBg2Pb));
        const s16 pc = static_cast<s16>(bus_.Read16(mem::kIoBase + io::kBg2Pc));
        const s16 pd = static_cast<s16>(bus_.Read16(mem::kIoBase + io::kBg2Pd));

        // See the reference-point accumulators' declaration in ppu.h: BG2X/
        // BG2Y only reload the internal accumulator at frame start or on a
        // fresh write, not every line.
        const u32 rawX = bus_.Read32(mem::kIoBase + io::kBg2X);
        const u32 rawY = bus_.Read32(mem::kIoBase + io::kBg2Y);
        if (line == 0 || rawX != bg2LastRawX_) {
            bg2RefX_ = SignExtend28(rawX);
            bg2LastRawX_ = rawX;
        }
        if (line == 0 || rawY != bg2LastRawY_) {
            bg2RefY_ = SignExtend28(rawY);
            bg2LastRawY_ = rawY;
        }
        const s32 refX = bg2RefX_;
        const s32 refY = bg2RefY_;
        bg2RefX_ += pb;
        bg2RefY_ += pd;

        auto& layer = bgLayer_[2];
        for (int x = 0; x < kScreenWidth; ++x) {
            s32 srcX = (refX + x * pa) >> 8;
            s32 srcY = (refY + x * pc) >> 8;

            if (wrap) {
                srcX = FloorMod(srcX, kBitmapWidth);
                srcY = FloorMod(srcY, kBitmapHeight);
            } else if (srcX < 0 || srcX >= kBitmapWidth || srcY < 0 || srcY >= kBitmapHeight) {
                layer[static_cast<std::size_t>(x)].opaque = false;
                continue;
            }

            const u32 pixelAddr = frameBase + static_cast<u32>(srcY * kBitmapWidth + srcX) * 2u;
            layer[static_cast<std::size_t>(x)].opaque = true;
            layer[static_cast<std::size_t>(x)].color = Bgr555ToRgba8888(bus_.Read16(pixelAddr));
        }
    }

    const bool objEnabled = (dispcnt & (1u << 12)) != 0;
    if (objEnabled) {
        RenderSprites(line);
    }
    CompositeLayers(line, bgEnabled, bgPriority, objEnabled);
}

void Ppu::RenderMode0(int line) {
    const u16 dispcnt = bus_.Read16(mem::kIoBase + io::kDispcnt);

    bool bgEnabled[4];
    u8 bgPriority[4];
    static constexpr u32 kBgCntOffset[4] = {io::kBg0Cnt, io::kBg1Cnt, io::kBg2Cnt, io::kBg3Cnt};

    for (int i = 0; i < 4; ++i) {
        bgEnabled[i] = (dispcnt & (1u << (8 + i))) != 0;
        if (bgEnabled[i]) {
            const u16 control = bus_.Read16(mem::kIoBase + kBgCntOffset[static_cast<std::size_t>(i)]);
            bgPriority[i] = static_cast<u8>(control & 0x3u);
            RenderTextBackground(i, line);
        } else {
            bgPriority[i] = 0;
        }
    }

    const bool objEnabled = (dispcnt & (1u << 12)) != 0;
    if (objEnabled) {
        RenderSprites(line);
    }

    CompositeLayers(line, bgEnabled, bgPriority, objEnabled);
}

void Ppu::RenderMode1(int line) {
    // BG0/BG1 regular, BG2 affine, BG3 doesn't exist in this mode - forced
    // off regardless of its DISPCNT enable bit so a game leaving stale
    // garbage there can't make BG3 "work" by accident.
    const u16 dispcnt = bus_.Read16(mem::kIoBase + io::kDispcnt);

    bool bgEnabled[4] = {false, false, false, false};
    u8 bgPriority[4] = {0, 0, 0, 0};
    static constexpr u32 kBgCntOffset[4] = {io::kBg0Cnt, io::kBg1Cnt, io::kBg2Cnt, io::kBg3Cnt};

    for (int i = 0; i < 2; ++i) {
        bgEnabled[i] = (dispcnt & (1u << (8 + i))) != 0;
        if (bgEnabled[i]) {
            const u16 control = bus_.Read16(mem::kIoBase + kBgCntOffset[static_cast<std::size_t>(i)]);
            bgPriority[i] = static_cast<u8>(control & 0x3u);
            RenderTextBackground(i, line);
        }
    }

    bgEnabled[2] = (dispcnt & (1u << 10)) != 0;
    if (bgEnabled[2]) {
        const u16 control = bus_.Read16(mem::kIoBase + io::kBg2Cnt);
        bgPriority[2] = static_cast<u8>(control & 0x3u);
        RenderAffineBackground(2, line);
    }

    const bool objEnabled = (dispcnt & (1u << 12)) != 0;
    if (objEnabled) {
        RenderSprites(line);
    }

    CompositeLayers(line, bgEnabled, bgPriority, objEnabled);
}

void Ppu::RenderMode2(int line) {
    // BG2 and BG3 both affine, BG0/BG1 don't exist in this mode.
    const u16 dispcnt = bus_.Read16(mem::kIoBase + io::kDispcnt);

    bool bgEnabled[4] = {false, false, false, false};
    u8 bgPriority[4] = {0, 0, 0, 0};
    static constexpr u32 kBgCntOffset[2] = {io::kBg2Cnt, io::kBg3Cnt};

    for (int i = 0; i < 2; ++i) {
        const int bgIndex = 2 + i;
        bgEnabled[bgIndex] = (dispcnt & (1u << (8 + bgIndex))) != 0;
        if (bgEnabled[bgIndex]) {
            const u16 control = bus_.Read16(mem::kIoBase + kBgCntOffset[static_cast<std::size_t>(i)]);
            bgPriority[bgIndex] = static_cast<u8>(control & 0x3u);
            RenderAffineBackground(bgIndex, line);
        }
    }

    const bool objEnabled = (dispcnt & (1u << 12)) != 0;
    if (objEnabled) {
        RenderSprites(line);
    }

    CompositeLayers(line, bgEnabled, bgPriority, objEnabled);
}

void Ppu::RenderTextBackground(int bgIndex, int line) {
    static constexpr u32 kCntOffset[4]  = {io::kBg0Cnt, io::kBg1Cnt, io::kBg2Cnt, io::kBg3Cnt};
    static constexpr u32 kHOfsOffset[4] = {io::kBg0HOfs, io::kBg1HOfs, io::kBg2HOfs, io::kBg3HOfs};
    static constexpr u32 kVOfsOffset[4] = {io::kBg0VOfs, io::kBg1VOfs, io::kBg2VOfs, io::kBg3VOfs};

    const u16 control = bus_.Read16(mem::kIoBase + kCntOffset[static_cast<std::size_t>(bgIndex)]);
    const u16 hOfs = bus_.Read16(mem::kIoBase + kHOfsOffset[static_cast<std::size_t>(bgIndex)]) & 0x1FFu;
    const u16 vOfs = bus_.Read16(mem::kIoBase + kVOfsOffset[static_cast<std::size_t>(bgIndex)]) & 0x1FFu;

    const u32 charBase   = ((control >> 2) & 0x3u) * 0x4000u;
    const u32 screenBase = ((control >> 8) & 0x1Fu) * 0x800u;
    const bool colorMode8bpp = (control & (1u << 7)) != 0;
    const u32 screenSize = (control >> 14) & 0x3u;

    // Text-mode background dimensions, in tiles, per GBATEK "BG Screen
    // Size (text mode)". Sizes 1-3 are built from multiple 32x32-tile
    // "screen blocks" laid out side by side and/or stacked.
    const u32 widthTiles  = (screenSize == 1 || screenSize == 3) ? 64u : 32u;
    const u32 heightTiles = (screenSize == 2 || screenSize == 3) ? 64u : 32u;
    const u32 widthPixels  = widthTiles * 8u;
    const u32 heightPixels = heightTiles * 8u;

    // Mosaic (GBATEK "Mosaic Function"): when enabled, screen coordinates
    // are snapped down to the nearest multiple of the configured block
    // size *before* scrolling is applied, so a whole block of screen
    // pixels ends up sampling the same background texel.
    const bool mosaicEnabled = (control & (1u << 6)) != 0;
    u32 bgMosaicH = 1u;
    u32 bgMosaicV = 1u;
    if (mosaicEnabled) {
        const u16 mosaic = bus_.Read16(mem::kIoBase + io::kMosaic);
        bgMosaicH = (mosaic & 0xFu) + 1u;
        bgMosaicV = ((mosaic >> 4) & 0xFu) + 1u;
    }

    auto& layer = bgLayer_[bgIndex];

    const int effY = mosaicEnabled ? (line - (line % static_cast<int>(bgMosaicV))) : line;
    const u32 bgY = (static_cast<u32>(effY) + vOfs) % heightPixels;
    const u32 tileRow = bgY / 8u;
    const u32 pixelRowInTile = bgY % 8u;

    for (int x = 0; x < kScreenWidth; ++x) {
        const int effX = mosaicEnabled ? (x - (x % static_cast<int>(bgMosaicH))) : x;
        const u32 bgX = (static_cast<u32>(effX) + hOfs) % widthPixels;
        const u32 tileCol = bgX / 8u;
        const u32 pixelColInTile = bgX % 8u;

        // Which 32x32 screen block this tile falls in, and the tile's
        // local coordinates within that block.
        const u32 blockCol = tileCol / 32u;
        const u32 blockRow = tileRow / 32u;
        u32 blockIndex = 0;
        if (screenSize == 1) {
            blockIndex = blockCol;
        } else if (screenSize == 2) {
            blockIndex = blockRow;
        } else if (screenSize == 3) {
            blockIndex = blockRow * 2u + blockCol;
        }
        const u32 localTileCol = tileCol % 32u;
        const u32 localTileRow = tileRow % 32u;

        const u32 tilemapAddr = mem::kVramBase + screenBase + blockIndex * 0x800u +
                                 (localTileRow * 32u + localTileCol) * 2u;
        const u16 entry = bus_.Read16(tilemapAddr);
        const u32 tileNumber = entry & 0x3FFu;
        const bool hFlip = (entry & (1u << 10)) != 0;
        const bool vFlip = (entry & (1u << 11)) != 0;
        const u32 paletteNum = (entry >> 12) & 0xFu;

        const u32 px = hFlip ? (7u - pixelColInTile) : pixelColInTile;
        const u32 py = vFlip ? (7u - pixelRowInTile) : pixelRowInTile;

        u32 colorIndex;
        u32 paletteAddr;
        if (colorMode8bpp) {
            const u32 tileDataAddr = mem::kVramBase + charBase + tileNumber * 64u;
            colorIndex = bus_.Read8(tileDataAddr + py * 8u + px);
            paletteAddr = mem::kPaletteBase + colorIndex * 2u;
        } else {
            const u32 tileDataAddr = mem::kVramBase + charBase + tileNumber * 32u;
            const u32 byteOffset = (py * 8u + px) / 2u;
            const u8 byteVal = bus_.Read8(tileDataAddr + byteOffset);
            colorIndex = ((px % 2u) == 0u) ? (byteVal & 0xFu) : (byteVal >> 4);
            paletteAddr = mem::kPaletteBase + (paletteNum * 16u + colorIndex) * 2u;
        }

        if (colorIndex == 0) {
            layer[static_cast<std::size_t>(x)].opaque = false;
        } else {
            layer[static_cast<std::size_t>(x)].opaque = true;
            layer[static_cast<std::size_t>(x)].color = Bgr555ToRgba8888(bus_.Read16(paletteAddr));
        }
    }
}

void Ppu::RenderAffineBackground(int bgIndex, int line) {
    // BG2 and BG3 share this exact register layout, just at different I/O
    // offsets - GBATEK "BG Rotation/Scaling Parameters".
    const bool isBg2 = (bgIndex == 2);
    const u32 cntOffset = isBg2 ? io::kBg2Cnt : io::kBg3Cnt;
    const u32 paOffset  = isBg2 ? io::kBg2Pa  : io::kBg3Pa;
    const u32 pbOffset  = isBg2 ? io::kBg2Pb  : io::kBg3Pb;
    const u32 pcOffset  = isBg2 ? io::kBg2Pc  : io::kBg3Pc;
    const u32 pdOffset  = isBg2 ? io::kBg2Pd  : io::kBg3Pd;
    const u32 xOffset   = isBg2 ? io::kBg2X   : io::kBg3X;
    const u32 yOffset   = isBg2 ? io::kBg2Y   : io::kBg3Y;

    const u16 control = bus_.Read16(mem::kIoBase + cntOffset);
    const u32 charBase   = ((control >> 2) & 0x3u) * 0x4000u;
    const u32 screenBase = ((control >> 8) & 0x1Fu) * 0x800u;
    const bool wrap = (control & (1u << 13)) != 0;

    // Affine BG screen size (GBATEK "BG Screen Size (Rotation/Scaling)")
    // is always a single square block, unlike text mode's 32x32 sub-block
    // layout - 16, 32, 64 or 128 tiles per side.
    static constexpr u32 kSizeTiles[4] = {16, 32, 64, 128};
    const u32 sizeTiles = kSizeTiles[(control >> 14) & 0x3u];
    const s32 sizePixels = static_cast<s32>(sizeTiles) * 8;

    const s16 pa = static_cast<s16>(bus_.Read16(mem::kIoBase + paOffset));
    const s16 pb = static_cast<s16>(bus_.Read16(mem::kIoBase + pbOffset));
    const s16 pc = static_cast<s16>(bus_.Read16(mem::kIoBase + pcOffset));
    const s16 pd = static_cast<s16>(bus_.Read16(mem::kIoBase + pdOffset));

    // See the reference-point accumulators' declaration in ppu.h.
    s32& refXAccum = isBg2 ? bg2RefX_ : bg3RefX_;
    s32& refYAccum = isBg2 ? bg2RefY_ : bg3RefY_;
    u32& lastRawX = isBg2 ? bg2LastRawX_ : bg3LastRawX_;
    u32& lastRawY = isBg2 ? bg2LastRawY_ : bg3LastRawY_;

    const u32 rawX = bus_.Read32(mem::kIoBase + xOffset);
    const u32 rawY = bus_.Read32(mem::kIoBase + yOffset);
    if (line == 0 || rawX != lastRawX) {
        refXAccum = SignExtend28(rawX);
        lastRawX = rawX;
    }
    if (line == 0 || rawY != lastRawY) {
        refYAccum = SignExtend28(rawY);
        lastRawY = rawY;
    }
    // This line's true (unsnapped) reference point, before advancing the
    // accumulator for next line.
    const s32 lineRefX = refXAccum;
    const s32 lineRefY = refYAccum;
    refXAccum += pb;
    refYAccum += pd;

    // Mosaic: same screen-coordinate snapping as RenderTextBackground,
    // applied before the affine transform rather than to HOFS/VOFS. Since
    // the reference point is now an incremental accumulator rather than
    // recomputed from a frame-start value each time, snapping back to an
    // earlier line's reference point is done by walking the accumulator
    // backwards by the same PB/PD step it would have taken to get there.
    const bool mosaicEnabled = (control & (1u << 6)) != 0;
    u32 bgMosaicH = 1u;
    u32 bgMosaicV = 1u;
    if (mosaicEnabled) {
        const u16 mosaic = bus_.Read16(mem::kIoBase + io::kMosaic);
        bgMosaicH = (mosaic & 0xFu) + 1u;
        bgMosaicV = ((mosaic >> 4) & 0xFu) + 1u;
    }
    const int effLine = mosaicEnabled ? (line - (line % static_cast<int>(bgMosaicV))) : line;
    const s32 refX = lineRefX - static_cast<s32>(line - effLine) * pb;
    const s32 refY = lineRefY - static_cast<s32>(line - effLine) * pd;

    auto& layer = bgLayer_[bgIndex];

    for (int x = 0; x < kScreenWidth; ++x) {
        const int effX = mosaicEnabled ? (x - (x % static_cast<int>(bgMosaicH))) : x;
        s32 srcX = (refX + effX * pa) >> 8;
        s32 srcY = (refY + effX * pc) >> 8;

        if (wrap) {
            srcX = FloorMod(srcX, sizePixels);
            srcY = FloorMod(srcY, sizePixels);
        } else if (srcX < 0 || srcX >= sizePixels || srcY < 0 || srcY >= sizePixels) {
            layer[static_cast<std::size_t>(x)].opaque = false;
            continue;
        }

        const u32 tileCol = static_cast<u32>(srcX) / 8u;
        const u32 tileRow = static_cast<u32>(srcY) / 8u;
        const u32 pixelCol = static_cast<u32>(srcX) % 8u;
        const u32 pixelRow = static_cast<u32>(srcY) % 8u;

        // 1 byte per tilemap entry (just a tile number, 0-255) - no
        // flip or palette-select bits like text mode has.
        const u32 tilemapAddr = mem::kVramBase + screenBase + tileRow * sizeTiles + tileCol;
        const u8 tileNumber = bus_.Read8(tilemapAddr);

        // Affine backgrounds are always 8bpp (256-color, single
        // palette bank).
        const u32 tileDataAddr = mem::kVramBase + charBase + static_cast<u32>(tileNumber) * 64u;
        const u8 colorIndex = bus_.Read8(tileDataAddr + pixelRow * 8u + pixelCol);

        if (colorIndex == 0) {
            layer[static_cast<std::size_t>(x)].opaque = false;
        } else {
            layer[static_cast<std::size_t>(x)].opaque = true;
            const u32 paletteAddr = mem::kPaletteBase + static_cast<u32>(colorIndex) * 2u;
            layer[static_cast<std::size_t>(x)].color = Bgr555ToRgba8888(bus_.Read16(paletteAddr));
        }
    }
}

void Ppu::RenderSprites(int line) {
    for (auto& pixel : objLayer_) {
        pixel.opaque = false;
    }
    objWindowMask_.fill(false);
    objSemiTransparent_.fill(false);

    const u16 dispcnt = bus_.Read16(mem::kIoBase + io::kDispcnt);
    const bool mapping1D = (dispcnt & (1u << 6)) != 0;

    const u16 mosaic = bus_.Read16(mem::kIoBase + io::kMosaic);
    const u32 objMosaicH = ((mosaic >> 8) & 0xFu) + 1u;
    const u32 objMosaicV = ((mosaic >> 12) & 0xFu) + 1u;

    // In bitmap modes (3-5) the lower half of OBJ VRAM is occupied by the
    // BG2 bitmap, so the OBJ character area starts 0x4000 bytes later than
    // in tile modes (0-2) and only tile numbers 512-1023 are usable.
    // GBATEK "VRAM - Object Character Data".
    const u32 mode = dispcnt & 0x7u;
    const u32 objBase = mem::kVramBase + ((mode >= 3) ? 0x1'4000u : 0x1'0000u);

    // Shape+size (GBATEK "OBJ Size") -> pixel dimensions.
    static constexpr int kWidthTable[4][4]  = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}, {0, 0, 0, 0}};
    static constexpr int kHeightTable[4][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}, {0, 0, 0, 0}};

    // Real hardware can only spend a limited number of cycles per
    // scanline fetching sprite tile data (GBATEK "Time Available for OBJ
    // Rendering"); once that budget runs out partway through OAM, the
    // remaining OBJs simply aren't drawn on this line at all - a heavily
    // sprite-crowded scanline can visibly drop some of them while
    // adjacent, less-crowded lines render every sprite fully. The budget
    // is spent scanning OAM index 0 upward, but the compositing loop
    // below runs in reverse (127 down to 0) for its own priority
    // tie-breaking reasons (see its "already-drawn, higher-priority
    // sprite wins" comment), so eligibility has to be worked out here as
    // a separate forward pass first.
    //
    // Cost model verified against mGBA's GBAVideoRendererCleanOAM
    // (src/gba/renderers/common.c) and
    // GBAVideoSoftwareRendererPreprocessSpriteLayer (renderers/
    // video-software.c), which is more precise than GBATEK's own
    // simplified "n*1" / "10+n*2" description: every non-disabled OAM
    // slot costs 2 cycles just to be scanned - charged as 2 * (this
    // index - the last index actually charged), so disabled slots in
    // between cost nothing themselves but still widen the next real
    // slot's charge - and a slot that overlaps this scanline both
    // vertically and horizontally additionally costs (width-2) cycles
    // for a normal sprite or (8 + boundWidth*2) for an affine one,
    // reduced further when the sprite starts partially off the left
    // screen edge (full 1:1 credit per off-screen column for affine,
    // only half credit - via an arithmetic shift, not a division - for
    // normal sprites).
    bool budgetAllows[128];
    {
        const bool hblankFree = (dispcnt & (1u << 5)) != 0;
        int budget = hblankFree ? 954 : 1210;
        int lastChargedIndex = 0;
        for (int i = 0; i < 128; ++i) {
            budgetAllows[i] = false;
            const u32 addr = mem::kOamBase + static_cast<u32>(i) * 8u;
            const u16 a0 = bus_.Read16(addr);
            const u16 a1 = bus_.Read16(addr + 2u);

            const bool aff = (a0 & (1u << 8)) != 0;
            const bool doubleSz = (a0 & (1u << 9)) != 0;
            if (!aff && doubleSz) {
                continue; // disabled - never scanned, never costs anything
            }

            budget -= 2 * (i - lastChargedIndex);
            lastChargedIndex = i;
            if (budget <= 0) {
                break; // out of time for the rest of this scanline's OAM scan
            }

            const u32 shp = (a0 >> 14) & 0x3u;
            const u32 sz = (a1 >> 14) & 0x3u;
            if (shp == 3) {
                continue; // prohibited shape value
            }
            int w = kWidthTable[shp][sz];
            int h = kHeightTable[shp][sz];
            if (aff && doubleSz) {
                w *= 2;
                h *= 2;
            }

            int sY = static_cast<int>(a0 & 0xFFu);
            if (sY >= kScreenHeight) sY -= 256;
            if (line < sY || line >= sY + h) {
                continue; // doesn't reach this scanline - no width cost
            }

            int sX = static_cast<int>(a1 & 0x1FFu);
            if (sX >= kScreenWidth) sX -= 512;
            if (sX >= kScreenWidth || sX + w <= 0) {
                continue; // no visible column on any scanline
            }

            int cost;
            if (aff) {
                cost = 8 + w * 2;
                if (sX < 0) cost += sX;
            } else {
                cost = w - 2;
                if (sX < 0) cost += sX >> 1;
            }

            budget -= cost;
            budgetAllows[i] = true;
        }
    }

    for (int oamIndex = 127; oamIndex >= 0; --oamIndex) {
        if (!budgetAllows[oamIndex]) {
            continue;
        }
        const u32 entryAddr = mem::kOamBase + static_cast<u32>(oamIndex) * 8u;
        const u16 attr0 = bus_.Read16(entryAddr);
        const u16 attr1 = bus_.Read16(entryAddr + 2u);
        const u16 attr2 = bus_.Read16(entryAddr + 4u);

        const bool affine = (attr0 & (1u << 8)) != 0;
        if (!affine && (attr0 & (1u << 9)) != 0) {
            continue; // disabled (only meaningful when affine flag is clear)
        }

        // OBJ Mode (GBATEK "OBJ Attribute 0"): 0=normal, 1=semi-
        // transparent (forces alpha-blending with whatever's underneath,
        // regardless of BLDCNT's own effect selection), 2=OBJ window
        // (draws nothing itself - just marks objWindowMask_ so WINOUT's
        // "inside OBJ window" bits apply there instead of its "outside all
        // windows" bits), 3=prohibited.
        const u32 objMode = (attr0 >> 10) & 0x3u;
        if (objMode == 3) {
            continue;
        }
        const bool mosaicEnabled = (attr0 & (1u << 12)) != 0;

        const u32 shape = (attr0 >> 14) & 0x3u;
        const u32 size = (attr1 >> 14) & 0x3u;
        if (shape == 3) {
            continue; // prohibited shape value
        }
        const int width = kWidthTable[shape][size];
        const int height = kHeightTable[shape][size];

        int screenY = attr0 & 0xFFu;
        if (screenY >= 160) screenY -= 256; // 8-bit coordinate, wraps for off-top placement
        int screenX = attr1 & 0x1FFu;
        if (screenX >= 240) screenX -= 512; // 9-bit coordinate, wraps for off-left placement

        const bool colorMode8bpp = (attr0 & (1u << 13)) != 0;
        const u32 baseTileNumber = attr2 & 0x3FFu;
        const u32 priority = (attr2 >> 10) & 0x3u;
        const u32 paletteNum = (attr2 >> 12) & 0xFu;
        const u32 widthTiles = static_cast<u32>(width) / 8u;

        // Samples the sprite's own tile data at texture coordinates
        // (texX, texY) - both already guaranteed in [0,width)/[0,height) -
        // and, if opaque, composites it at screen column px on this
        // scanline with this sprite's priority. Shared by the regular and
        // affine paths below; only how (texX, texY) and px are derived
        // differs between them.
        auto plotPixel = [&](int texX, int texY, int px) {
            if (px < 0 || px >= kScreenWidth) {
                return;
            }
            const u32 tileCol = static_cast<u32>(texX) / 8u;
            const u32 tileRow = static_cast<u32>(texY) / 8u;
            const u32 pixelCol = static_cast<u32>(texX) % 8u;
            const u32 pixelRow = static_cast<u32>(texY) % 8u;

            const u32 rowStride = mapping1D ? (widthTiles * (colorMode8bpp ? 2u : 1u)) : 32u;
            const u32 tileOffset = tileRow * rowStride + tileCol * (colorMode8bpp ? 2u : 1u);
            const u32 tileDataAddr = objBase + (baseTileNumber + tileOffset) * 32u;

            u32 colorIndex;
            u32 paletteAddr;
            if (colorMode8bpp) {
                colorIndex = bus_.Read8(tileDataAddr + pixelRow * 8u + pixelCol);
                paletteAddr = mem::kPaletteBase + 0x200u + colorIndex * 2u;
            } else {
                const u32 byteOffset = (pixelRow * 8u + pixelCol) / 2u;
                const u8 byteVal = bus_.Read8(tileDataAddr + byteOffset);
                colorIndex = ((pixelCol % 2u) == 0u) ? (byteVal & 0xFu) : (byteVal >> 4);
                paletteAddr = mem::kPaletteBase + 0x200u + (paletteNum * 16u + colorIndex) * 2u;
            }

            if (colorIndex == 0) {
                return; // transparent
            }

            const std::size_t pixelIndex = static_cast<std::size_t>(px);

            if (objMode == 2) {
                // OBJ window sprite: contributes only to the window mask,
                // never to the visible OBJ layer, and never contests
                // priority with other sprites.
                objWindowMask_[pixelIndex] = true;
                return;
            }

            const u8 existingPriority = objPriority_[pixelIndex];
            if (objLayer_[pixelIndex].opaque && priority > existingPriority) {
                return; // an already-drawn, higher-priority sprite wins - see RenderSprites' header comment
            }
            objLayer_[pixelIndex].opaque = true;
            objLayer_[pixelIndex].color = Bgr555ToRgba8888(bus_.Read16(paletteAddr));
            objPriority_[pixelIndex] = static_cast<u8>(priority);
            objSemiTransparent_[pixelIndex] = (objMode == 1);
        };

        if (!affine) {
            const bool hFlip = (attr1 & (1u << 12)) != 0;
            const bool vFlip = (attr1 & (1u << 13)) != 0;

            const int localY = line - screenY;
            if (localY < 0 || localY >= height) {
                continue;
            }

            // Mosaic: snap the *screen* row to the OBJ mosaic block
            // size, then re-derive which sprite-local row that
            // corresponds to - the whole block ends up showing the
            // texel that the block's first row would have. GBATEK
            // "Mosaic Function".
            int effLocalY = localY;
            if (mosaicEnabled && objMosaicV > 1u) {
                const int snappedPy = line - (line % static_cast<int>(objMosaicV));
                effLocalY = snappedPy - screenY;
                if (effLocalY < 0) effLocalY = 0;
                if (effLocalY >= height) effLocalY = height - 1;
            }
            const int texY = vFlip ? (height - 1 - effLocalY) : effLocalY;

            for (int localX = 0; localX < width; ++localX) {
                int effLocalX = localX;
                if (mosaicEnabled && objMosaicH > 1u) {
                    const int px = screenX + localX;
                    const int snappedPx = px - (px % static_cast<int>(objMosaicH));
                    effLocalX = snappedPx - screenX;
                    if (effLocalX < 0) effLocalX = 0;
                    if (effLocalX >= width) effLocalX = width - 1;
                }
                const int texX = hFlip ? (width - 1 - effLocalX) : effLocalX;
                plotPixel(texX, texY, screenX + localX);
            }
            continue;
        }

        // Affine (rotate/scale) sprite. GBATEK "OBJ Rotation/Scaling":
        // attr1 bits9-13 select one of 32 parameter groups, each stored
        // across four consecutive OAM entries' attr3 fields (an OAM slot
        // this sprite doesn't otherwise use, since attr3 has no meaning
        // for the sprite's own attributes).
        const u32 paramGroup = (attr1 >> 9) & 0x1Fu;
        const u32 paramBase = paramGroup * 4u;
        const s16 pa = static_cast<s16>(bus_.Read16(mem::kOamBase + (paramBase + 0u) * 8u + 6u));
        const s16 pb = static_cast<s16>(bus_.Read16(mem::kOamBase + (paramBase + 1u) * 8u + 6u));
        const s16 pc = static_cast<s16>(bus_.Read16(mem::kOamBase + (paramBase + 2u) * 8u + 6u));
        const s16 pd = static_cast<s16>(bus_.Read16(mem::kOamBase + (paramBase + 3u) * 8u + 6u));

        // Double-size (attr0 bit9, only meaningful when affine): doubles
        // the on-screen bounding box the sprite is scanned over (so a
        // rotated/scaled sprite has room to grow into) without changing
        // its actual texture dimensions.
        const bool doubleSize = (attr0 & (1u << 9)) != 0;
        const int boundWidth = doubleSize ? width * 2 : width;
        const int boundHeight = doubleSize ? height * 2 : height;
        const int boundHalfW = boundWidth / 2;
        const int boundHalfH = boundHeight / 2;
        const int texHalfW = width / 2;
        const int texHalfH = height / 2;

        const int dy = line - screenY;
        if (dy < 0 || dy >= boundHeight) {
            continue;
        }

        // Mosaic snaps the screen row/column before it feeds the
        // rotation/scaling matrix, same idea as the regular-sprite
        // path above.
        int effDy = dy;
        if (mosaicEnabled && objMosaicV > 1u) {
            const int snappedPy = line - (line % static_cast<int>(objMosaicV));
            effDy = snappedPy - screenY;
            if (effDy < 0) effDy = 0;
            if (effDy >= boundHeight) effDy = boundHeight - 1;
        }
        const int ry = effDy - boundHalfH;

        for (int dx = 0; dx < boundWidth; ++dx) {
            const int px = screenX + dx;
            if (px < 0 || px >= kScreenWidth) continue;

            int effDx = dx;
            if (mosaicEnabled && objMosaicH > 1u) {
                const int snappedPx = px - (px % static_cast<int>(objMosaicH));
                effDx = snappedPx - screenX;
                if (effDx < 0) effDx = 0;
                if (effDx >= boundWidth) effDx = boundWidth - 1;
            }
            const int rx = effDx - boundHalfW;

            // Inverse-map this screen pixel back into the sprite's own
            // texture space through the rotation/scaling matrix -
            // pixels that land outside the sprite's actual tile data
            // are simply not drawn (no wraparound for OBJs).
            const int texX = ((pa * rx + pb * ry) >> 8) + texHalfW;
            const int texY = ((pc * rx + pd * ry) >> 8) + texHalfH;
            if (texX < 0 || texX >= width || texY < 0 || texY >= height) {
                continue;
            }
            plotPixel(texX, texY, px);
        }
    }
}

void Ppu::CompositeLayers(int line, const bool bgEnabled[4], const u8 bgPriority[4], bool objEnabled) {
    const u32 backdrop = Bgr555ToRgba8888(bus_.Read16(mem::kPaletteBase));

    const u16 dispcnt = bus_.Read16(mem::kIoBase + io::kDispcnt);
    const bool win0Enable = (dispcnt & (1u << 13)) != 0;
    const bool win1Enable = (dispcnt & (1u << 14)) != 0;
    const bool objWinEnable = (dispcnt & (1u << 15)) != 0;
    const bool windowsActive = win0Enable || win1Enable || objWinEnable;

    u8 win0X1 = 0, win0X2 = 0, win0Y1 = 0, win0Y2 = 0;
    u8 win1X1 = 0, win1X2 = 0, win1Y1 = 0, win1Y2 = 0;
    u16 winIn = 0, winOut = 0;
    if (windowsActive) {
        const u16 win0h = bus_.Read16(mem::kIoBase + io::kWin0H);
        const u16 win0v = bus_.Read16(mem::kIoBase + io::kWin0V);
        const u16 win1h = bus_.Read16(mem::kIoBase + io::kWin1H);
        const u16 win1v = bus_.Read16(mem::kIoBase + io::kWin1V);
        win0X1 = static_cast<u8>(win0h >> 8); win0X2 = static_cast<u8>(win0h);
        win0Y1 = static_cast<u8>(win0v >> 8); win0Y2 = static_cast<u8>(win0v);
        win1X1 = static_cast<u8>(win1h >> 8); win1X2 = static_cast<u8>(win1h);
        win1Y1 = static_cast<u8>(win1v >> 8); win1Y2 = static_cast<u8>(win1v);
        winIn = bus_.Read16(mem::kIoBase + io::kWinIn);
        winOut = bus_.Read16(mem::kIoBase + io::kWinOut);
    }

    // BLDCNT layer numbering (0-3=BG0-3, 4=OBJ, 5=Backdrop) matches its
    // own bit positions directly: bit N = "is layer N the 1st target",
    // bit (8+N) = "is layer N the 2nd target".
    const u16 bldcnt = bus_.Read16(mem::kIoBase + io::kBldCnt);
    const u32 effectMode = (bldcnt >> 6) & 0x3u;
    const u16 bldalpha = bus_.Read16(mem::kIoBase + io::kBldAlpha);
    const u16 bldy = bus_.Read16(mem::kIoBase + io::kBldY);
    const s32 eva = std::min<s32>(16, bldalpha & 0x1Fu);
    const s32 evb = std::min<s32>(16, (bldalpha >> 8) & 0x1Fu);
    const s32 evy = std::min<s32>(16, bldy & 0x1Fu);

    const std::size_t rowBase = static_cast<std::size_t>(line) * kScreenWidth;

    for (int x = 0; x < kScreenWidth; ++x) {
        const std::size_t pixelIndex = static_cast<std::size_t>(x);

        // Win0 > Win1 > OBJ window > outside, per GBATEK "Windows" -
        // the first (highest-priority) window a pixel falls inside
        // supplies its BG0-3/OBJ/Effect enable bits.
        bool layerWinEnable[6] = {true, true, true, true, true, true};
        if (windowsActive) {
            u16 bits;
            if (win0Enable && InWindowRange(x, win0X1, win0X2, kScreenWidth) &&
                InWindowRange(line, win0Y1, win0Y2, kScreenHeight)) {
                bits = winIn & 0x3Fu;
            } else if (win1Enable && InWindowRange(x, win1X1, win1X2, kScreenWidth) &&
                       InWindowRange(line, win1Y1, win1Y2, kScreenHeight)) {
                bits = (winIn >> 8) & 0x3Fu;
            } else if (objWinEnable && objWindowMask_[pixelIndex]) {
                bits = (winOut >> 8) & 0x3Fu;
            } else {
                bits = winOut & 0x3Fu;
            }
            for (int i = 0; i < 6; ++i) {
                layerWinEnable[i] = (bits & (1u << i)) != 0;
            }
        }

        // Gather every opaque, enabled layer at this pixel in
        // BG3,BG2,BG1,BG0,OBJ order and track the top-most and
        // second-most visible one - the compositor needs both for
        // blending (1st/2nd target pixel), not just the winner.
        struct Candidate { int kind; u8 priority; u32 color; };
        Candidate candidates[5];
        int candidateCount = 0;
        for (int bg = 3; bg >= 0; --bg) {
            if (!bgEnabled[bg] || !layerWinEnable[bg]) continue;
            const LayerPixel& p = bgLayer_[static_cast<std::size_t>(bg)][pixelIndex];
            if (p.opaque) {
                candidates[candidateCount++] = {bg, bgPriority[bg], p.color};
            }
        }
        if (objEnabled && layerWinEnable[4]) {
            const LayerPixel& p = objLayer_[pixelIndex];
            if (p.opaque) {
                candidates[candidateCount++] = {4, objPriority_[pixelIndex], p.color};
            }
        }

        int bestIdx = -1;
        int secondIdx = -1;
        for (int i = 0; i < candidateCount; ++i) {
            if (bestIdx == -1 || candidates[i].priority <= candidates[bestIdx].priority) {
                secondIdx = bestIdx;
                bestIdx = i;
            } else if (secondIdx == -1 || candidates[i].priority <= candidates[secondIdx].priority) {
                secondIdx = i;
            }
        }

        const int topKind = (bestIdx >= 0) ? candidates[bestIdx].kind : 5;
        const u32 topColor = (bestIdx >= 0) ? candidates[bestIdx].color : backdrop;
        const int bottomKind = (secondIdx >= 0) ? candidates[secondIdx].kind : 5;
        const u32 bottomColor = (secondIdx >= 0) ? candidates[secondIdx].color : backdrop;

        const bool effectWindowOk = !windowsActive || layerWinEnable[5];
        const bool target2Set = (bldcnt & (1u << (8 + bottomKind))) != 0;
        const bool topIsSemiTransObj = (topKind == 4) && objSemiTransparent_[pixelIndex];

        u32 finalColor = topColor;
        if (topIsSemiTransObj && target2Set && effectWindowOk) {
            // Semi-transparent OBJ (attr0 OBJ Mode 1): forces alpha
            // blending with whatever's beneath it, independent of
            // BLDCNT's own effect-mode selection or 1st-target bits -
            // GBATEK "OBJ semi-transparent pixels ... always be shown
            // with alpha blending".
            finalColor = AlphaBlend(topColor, bottomColor, eva, evb);
        } else if (effectMode != 0 && effectWindowOk && (bldcnt & (1u << topKind)) != 0) {
            if (effectMode == 1) {
                if (target2Set) {
                    finalColor = AlphaBlend(topColor, bottomColor, eva, evb);
                }
            } else if (effectMode == 2) {
                finalColor = BrightnessIncrease(topColor, evy);
            } else {
                finalColor = BrightnessDecrease(topColor, evy);
            }
        }

        framebuffer_[rowBase + pixelIndex] = finalColor;
    }
}

} // namespace gba
