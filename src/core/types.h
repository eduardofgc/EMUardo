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

} // namespace gba
