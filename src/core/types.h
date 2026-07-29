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

} // namespace gba
