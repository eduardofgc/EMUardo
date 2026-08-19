#pragma once

#include <cstdint>

namespace gba {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using s8  = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// GBA memory map base addresses (GBATEK section "Memory Map").
// Filled in incrementally as the Bus implementation grows.
namespace mem {
    constexpr u32 kBiosBase    = 0x0000'0000; // 16 KB BIOS ROM
    constexpr u32 kEwramBase   = 0x0200'0000; // 256 KB external work RAM
    constexpr u32 kIwramBase   = 0x0300'0000; // 32 KB internal work RAM
    constexpr u32 kIoBase      = 0x0400'0000; // I/O registers
    constexpr u32 kPaletteBase = 0x0500'0000; // 1 KB palette RAM
    constexpr u32 kVramBase    = 0x0600'0000; // 96 KB video RAM
    constexpr u32 kOamBase     = 0x0700'0000; // 1 KB object attribute memory
    constexpr u32 kRomBase     = 0x0800'0000; // cartridge ROM, up to 32 MB
}

// I/O register byte offsets within the I/O region (GBATEK "GBA I/O Map").
// Add to this as more registers come online (keypad, serial, sound, ...).
namespace io {
    constexpr u32 kDispcnt  = 0x000; // LCD Control - display mode, layer enables
    constexpr u32 kDispstat = 0x004; // General LCD Status - VBlank/HBlank/VCount flags+IRQ enables
    constexpr u32 kVcount   = 0x006; // Current scanline being drawn (read-only on real hardware)

    constexpr u32 kBg0Cnt = 0x008; constexpr u32 kBg1Cnt = 0x00A;
    constexpr u32 kBg2Cnt = 0x00C; constexpr u32 kBg3Cnt = 0x00E;
    constexpr u32 kBg0HOfs = 0x010; constexpr u32 kBg0VOfs = 0x012;
    constexpr u32 kBg1HOfs = 0x014; constexpr u32 kBg1VOfs = 0x016;
    constexpr u32 kBg2HOfs = 0x018; constexpr u32 kBg2VOfs = 0x01A;
    constexpr u32 kBg3HOfs = 0x01C; constexpr u32 kBg3VOfs = 0x01E;

    // Rotation/scaling parameters for the affine backgrounds (BG2 in modes
    // 1/2/5, BG3 in mode 2 only). PA-PD are signed 8.8 fixed-point; X/Y are
    // the 32-bit (28-bit signed, sign-extend from bit27) reference point,
    // each split into an _L/_H halfword pair 4 bytes apart. GBATEK
    // "BG Rotation/Scaling Parameters".
    constexpr u32 kBg2Pa = 0x020; constexpr u32 kBg2Pb = 0x022;
    constexpr u32 kBg2Pc = 0x024; constexpr u32 kBg2Pd = 0x026;
    constexpr u32 kBg2X  = 0x028; constexpr u32 kBg2Y  = 0x02C;
    constexpr u32 kBg3Pa = 0x030; constexpr u32 kBg3Pb = 0x032;
    constexpr u32 kBg3Pc = 0x034; constexpr u32 kBg3Pd = 0x036;
    constexpr u32 kBg3X  = 0x038; constexpr u32 kBg3Y  = 0x03C;

    // Windowing, mosaic and color special effects (GBATEK "Windows" /
    // "Mosaic Function" / "Color Special Effects"). WIN0H/WIN1H pack
    // (X1<<8)|X2 - X1 left edge inclusive, X2 right edge exclusive; WIN0V/
    // WIN1V pack (Y1<<8)|Y2 the same way. WININ bits0-5/8-13 are the
    // BG0-3/OBJ/Effect enable flags for inside Win0/Win1 respectively;
    // WINOUT bits0-5/8-13 are the same for outside all windows / inside
    // the OBJ window. MOSAIC packs BG H/V size in bits0-3/4-7 and OBJ H/V
    // size in bits8-11/12-15 (register value + 1 = size in pixels).
    // BLDCNT bits0-5/8-13 select which layers count as the 1st/2nd target
    // pixel (BG0-3, OBJ, Backdrop, in that bit order), bits6-7 select the
    // effect (0=None,1=Alpha blend,2=Brightness increase,3=decrease).
    // BLDALPHA holds the EVA/EVB blend coefficients (bits0-4/8-12), BLDY
    // the EVY brightness coefficient (bits0-4).
    constexpr u32 kWin0H = 0x040; constexpr u32 kWin1H = 0x042;
    constexpr u32 kWin0V = 0x044; constexpr u32 kWin1V = 0x046;
    constexpr u32 kWinIn = 0x048; constexpr u32 kWinOut = 0x04A;
    constexpr u32 kMosaic = 0x04C;
    constexpr u32 kBldCnt = 0x050; constexpr u32 kBldAlpha = 0x052; constexpr u32 kBldY = 0x054;

    constexpr u32 kKeyInput = 0x130; // Key status - active LOW (0=pressed, 1=released)
    constexpr u32 kKeyCnt   = 0x132; // Key interrupt control

    constexpr u32 kTm0CntL = 0x100; constexpr u32 kTm0CntH = 0x102;
    constexpr u32 kTm1CntL = 0x104; constexpr u32 kTm1CntH = 0x106;
    constexpr u32 kTm2CntL = 0x108; constexpr u32 kTm2CntH = 0x10A;
    constexpr u32 kTm3CntL = 0x10C; constexpr u32 kTm3CntH = 0x10E;

    constexpr u32 kDma0Sad = 0x0B0; constexpr u32 kDma0Dad = 0x0B4;
    constexpr u32 kDma0CntL = 0x0B8; constexpr u32 kDma0CntH = 0x0BA;
    constexpr u32 kDma1Sad = 0x0BC; constexpr u32 kDma1Dad = 0x0C0;
    constexpr u32 kDma1CntL = 0x0C4; constexpr u32 kDma1CntH = 0x0C6;
    constexpr u32 kDma2Sad = 0x0C8; constexpr u32 kDma2Dad = 0x0CC;
    constexpr u32 kDma2CntL = 0x0D0; constexpr u32 kDma2CntH = 0x0D2;
    constexpr u32 kDma3Sad = 0x0D4; constexpr u32 kDma3Dad = 0x0D8;
    constexpr u32 kDma3CntL = 0x0DC; constexpr u32 kDma3CntH = 0x0DE;

    constexpr u32 kIe  = 0x200; // Interrupt Enable
    constexpr u32 kIf  = 0x202; // Interrupt Request/Acknowledge - write 1 to a bit to clear it
    constexpr u32 kIme = 0x208; // Interrupt Master Enable
}

// Interrupt Enable/Request bit positions within IE/IF (GBATEK "Interrupt
// Sources"). Bus::RequestInterrupt() takes one of these.
namespace irq {
    constexpr u16 kVBlank  = 1u << 0;
    constexpr u16 kHBlank  = 1u << 1;
    constexpr u16 kVCount  = 1u << 2;
    constexpr u16 kTimer0  = 1u << 3;
    constexpr u16 kTimer1  = 1u << 4;
    constexpr u16 kTimer2  = 1u << 5;
    constexpr u16 kTimer3  = 1u << 6;
    constexpr u16 kSerial  = 1u << 7;
    constexpr u16 kDma0    = 1u << 8;
    constexpr u16 kDma1    = 1u << 9;
    constexpr u16 kDma2    = 1u << 10;
    constexpr u16 kDma3    = 1u << 11;
    constexpr u16 kKeypad  = 1u << 12;
    constexpr u16 kGamePak = 1u << 13;
}

// KEYINPUT/KEYCNT bit layout (GBATEK "Keypad Input"). KEYINPUT itself is
// active LOW - a set bit here means "pressed", but writing to the actual
// register requires inverting these before storing (see Bus::SetKeyState).
namespace key {
    constexpr u16 kA      = 1u << 0;
    constexpr u16 kB      = 1u << 1;
    constexpr u16 kSelect = 1u << 2;
    constexpr u16 kStart  = 1u << 3;
    constexpr u16 kRight  = 1u << 4;
    constexpr u16 kLeft   = 1u << 5;
    constexpr u16 kUp     = 1u << 6;
    constexpr u16 kDown   = 1u << 7;
    constexpr u16 kL      = 1u << 8;
    constexpr u16 kR      = 1u << 9;
    constexpr u16 kAll    = 0x3FFu;
}

} // namespace gba
