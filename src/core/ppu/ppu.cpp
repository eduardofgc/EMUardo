#include "core/ppu/ppu.h"

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
} // namespace

Ppu::Ppu(Bus& bus) : bus_(bus) {
    // Placeholder color so it's obvious in the SDL window that the
    // pipeline is wired up correctly, before RenderFrame() has run once.
    framebuffer_.fill(0xFF30'3030);
}

void Ppu::Step() {
    // TODO: scanline-driven rendering per DISPCNT mode - see the note on
    // this function in ppu.h. RenderFrame() covers the visible behavior
    // for now.
}

void Ppu::RenderFrame() {
    const u16 dispcnt = bus_.Read16(mem::kIoBase + io::kDispcnt);
    const u32 mode = dispcnt & 0x7u;

    switch (mode) {
        case 0:
            RenderMode0();
            break;
        case 3:
            RenderMode3();
            break;
        case 4:
            RenderMode4();
            break;
        default: {
            // Modes 1-2 (affine backgrounds) and 5 (smaller bitmap mode)
            // aren't implemented yet - fill with the same placeholder
            // color used before any rendering existed, so "unimplemented
            // mode" and "nothing rendered yet" look identically obvious
            // rather than showing stale data from a previous frame.
            static bool warned = false;
            if (!warned) {
                std::fprintf(stderr, "Ppu::RenderFrame: mode %u not implemented yet\n", mode);
                warned = true;
            }
            framebuffer_.fill(0xFF30'3030);
            break;
        }
    }
}

void Ppu::RenderMode3() {
    // Mode 3: BG2 only, one 16-bit BGR555 pixel per screen pixel, laid out
    // left-to-right/top-to-bottom starting at the base of VRAM - no tiles,
    // no palette, no scrolling registers consulted here yet. GBATEK
    // "BG Mode 3 - 16bit Bitmap".
    for (int y = 0; y < kScreenHeight; ++y) {
        for (int x = 0; x < kScreenWidth; ++x) {
            const u32 pixelIndex = static_cast<u32>(y * kScreenWidth + x);
            const u32 address = mem::kVramBase + pixelIndex * 2;
            const u16 color = bus_.Read16(address);
            framebuffer_[pixelIndex] = Bgr555ToRgba8888(color);
        }
    }
}

void Ppu::RenderMode4() {
    // Mode 4: BG2 is a 240x160 8bpp paletted bitmap - one byte per pixel,
    // indexing the BG palette (same palette bank Mode 0's 8bpp
    // backgrounds use). Double-buffered: DISPCNT bit4 selects which of
    // the two VRAM frames is currently visible.
    const u16 dispcnt = bus_.Read16(mem::kIoBase + io::kDispcnt);
    const bool secondFrame = (dispcnt & (1u << 4)) != 0;
    const u32 frameBase = mem::kVramBase + (secondFrame ? 0xA000u : 0u);

    for (int y = 0; y < kScreenHeight; ++y) {
        for (int x = 0; x < kScreenWidth; ++x) {
            const u32 pixelIndex = static_cast<u32>(y * kScreenWidth + x);
            const u8 colorIndex = bus_.Read8(frameBase + pixelIndex);
            const u32 paletteAddr = mem::kPaletteBase + static_cast<u32>(colorIndex) * 2u;
            framebuffer_[pixelIndex] = Bgr555ToRgba8888(bus_.Read16(paletteAddr));
        }
    }
}

void Ppu::RenderMode0() {
    const u16 dispcnt = bus_.Read16(mem::kIoBase + io::kDispcnt);

    bool bgEnabled[4];
    u8 bgPriority[4];
    static constexpr u32 kBgCntOffset[4] = {io::kBg0Cnt, io::kBg1Cnt, io::kBg2Cnt, io::kBg3Cnt};

    for (int i = 0; i < 4; ++i) {
        bgEnabled[i] = (dispcnt & (1u << (8 + i))) != 0;
        if (bgEnabled[i]) {
            const u16 control = bus_.Read16(mem::kIoBase + kBgCntOffset[static_cast<std::size_t>(i)]);
            bgPriority[i] = static_cast<u8>(control & 0x3u);
            RenderTextBackground(i);
        } else {
            bgPriority[i] = 0;
        }
    }

    const bool objEnabled = (dispcnt & (1u << 12)) != 0;
    if (objEnabled) {
        RenderSprites();
    }

    CompositeLayers(bgEnabled, bgPriority, objEnabled);
}

void Ppu::RenderTextBackground(int bgIndex) {
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

    auto& layer = bgLayer_[bgIndex];

    for (int y = 0; y < kScreenHeight; ++y) {
        const u32 bgY = (static_cast<u32>(y) + vOfs) % heightPixels;
        const u32 tileRow = bgY / 8u;
        const u32 pixelRowInTile = bgY % 8u;

        for (int x = 0; x < kScreenWidth; ++x) {
            const u32 bgX = (static_cast<u32>(x) + hOfs) % widthPixels;
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

            const std::size_t pixelIndex = static_cast<std::size_t>(y * kScreenWidth + x);
            if (colorIndex == 0) {
                layer[pixelIndex].opaque = false;
            } else {
                layer[pixelIndex].opaque = true;
                layer[pixelIndex].color = Bgr555ToRgba8888(bus_.Read16(paletteAddr));
            }
        }
    }
}

void Ppu::RenderSprites() {
    for (auto& pixel : objLayer_) {
        pixel.opaque = false;
    }

    const u16 dispcnt = bus_.Read16(mem::kIoBase + io::kDispcnt);
    const bool mapping1D = (dispcnt & (1u << 6)) != 0;

    // Shape+size (GBATEK "OBJ Size") -> pixel dimensions.
    static constexpr int kWidthTable[4][4]  = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}, {0, 0, 0, 0}};
    static constexpr int kHeightTable[4][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}, {0, 0, 0, 0}};

    for (int oamIndex = 127; oamIndex >= 0; --oamIndex) {
        const u32 entryAddr = mem::kOamBase + static_cast<u32>(oamIndex) * 8u;
        const u16 attr0 = bus_.Read16(entryAddr);
        const u16 attr1 = bus_.Read16(entryAddr + 2u);
        const u16 attr2 = bus_.Read16(entryAddr + 4u);

        const bool affine = (attr0 & (1u << 8)) != 0;
        if (!affine && (attr0 & (1u << 9)) != 0) {
            continue; // disabled (only meaningful when affine flag is clear)
        }
        if (affine) {
            // TODO: affine (rotation/scaling) sprites aren't implemented -
            // skipping rather than drawing them in the wrong place/shape.
            continue;
        }

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

        const bool hFlip = (attr1 & (1u << 12)) != 0;
        const bool vFlip = (attr1 & (1u << 13)) != 0;
        const bool colorMode8bpp = (attr0 & (1u << 13)) != 0;
        const u32 baseTileNumber = attr2 & 0x3FFu;
        const u32 priority = (attr2 >> 10) & 0x3u;
        const u32 paletteNum = (attr2 >> 12) & 0xFu;

        const u32 widthTiles = static_cast<u32>(width) / 8u;
        const u32 objBase = mem::kVramBase + 0x1'0000u; // OBJ character area starts at 0x06010000 in modes 0-2

        for (int localY = 0; localY < height; ++localY) {
            const int py = screenY + localY;
            if (py < 0 || py >= kScreenHeight) continue;

            for (int localX = 0; localX < width; ++localX) {
                const int px = screenX + localX;
                if (px < 0 || px >= kScreenWidth) continue;

                const int srcX = hFlip ? (width - 1 - localX) : localX;
                const int srcY = vFlip ? (height - 1 - localY) : localY;
                const u32 tileCol = static_cast<u32>(srcX) / 8u;
                const u32 tileRow = static_cast<u32>(srcY) / 8u;
                const u32 pixelCol = static_cast<u32>(srcX) % 8u;
                const u32 pixelRow = static_cast<u32>(srcY) % 8u;

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
                    continue; // transparent
                }

                const std::size_t pixelIndex = static_cast<std::size_t>(py * kScreenWidth + px);
                const u8 existingPriority = objPriority_[pixelIndex];
                if (objLayer_[pixelIndex].opaque && priority > existingPriority) {
                    continue; // an already-drawn, higher-priority sprite wins - see RenderSprites' header comment
                }
                objLayer_[pixelIndex].opaque = true;
                objLayer_[pixelIndex].color = Bgr555ToRgba8888(bus_.Read16(paletteAddr));
                objPriority_[pixelIndex] = static_cast<u8>(priority);
            }
        }
    }
}

void Ppu::CompositeLayers(const bool bgEnabled[4], const u8 bgPriority[4], bool objEnabled) {
    const u32 backdrop = Bgr555ToRgba8888(bus_.Read16(mem::kPaletteBase));

    for (int y = 0; y < kScreenHeight; ++y) {
        for (int x = 0; x < kScreenWidth; ++x) {
            const std::size_t pixelIndex = static_cast<std::size_t>(y * kScreenWidth + x);

            int bestPriority = 5; // worse than any real priority (0-3), so anything opaque beats it
            u32 bestColor = backdrop;
            bool found = false;

            // BG3..BG0 so that, among equal-priority backgrounds, BG0 (the
            // last one checked here) wins ties - GBATEK: lower BG index is
            // drawn on top when priorities match.
            for (int bg = 3; bg >= 0; --bg) {
                if (!bgEnabled[bg]) continue;
                const LayerPixel& p = bgLayer_[static_cast<std::size_t>(bg)][pixelIndex];
                if (p.opaque && bgPriority[bg] <= bestPriority) {
                    bestPriority = bgPriority[bg];
                    bestColor = p.color;
                    found = true;
                }
            }

            // OBJ checked last so it wins ties against a same-priority BG,
            // matching real hardware's sprite-over-background-of-equal-
            // priority behavior.
            if (objEnabled) {
                const LayerPixel& p = objLayer_[pixelIndex];
                if (p.opaque && objPriority_[pixelIndex] <= bestPriority) {
                    bestColor = p.color;
                    found = true;
                }
            }

            framebuffer_[pixelIndex] = found ? bestColor : backdrop;
        }
    }
}

} // namespace gba
