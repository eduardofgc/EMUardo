#pragma once

#include <array>
#include <string>
#include <vector>

#include "core/types.h"

namespace gba {

// The Bus is the single point every memory access goes through - CPU
// fetches/loads/stores, and later the PPU/DMA controller reading and
// writing VRAM/OAM/palette RAM. Keeping this centralized (rather than
// letting CPU poke at PPU internals directly) is what makes it possible
// to add DMA and open-bus behavior later without a rewrite.
class Bus {
public:
    Bus();

    // Loads a .gba ROM file into cartridge memory. Returns false on
    // failure (bad path, exceeds max cartridge size, etc).
    bool LoadRom(const std::string& path);

    // Width-specific accessors. The GBA CPU (ARM7TDMI) supports byte,
    // halfword, and word accesses, each with distinct alignment and
    // rotation-on-misalignment rules we'll implement per GBATEK.
    u8  Read8(u32 address) const;
    u16 Read16(u32 address) const;
    u32 Read32(u32 address) const;

    void Write8(u32 address, u8 value);
    void Write16(u32 address, u16 value);
    void Write32(u32 address, u32 value);

private:
    static constexpr std::size_t kEwramSize   = 256 * 1024;
    static constexpr std::size_t kIwramSize   = 32 * 1024;
    static constexpr std::size_t kPaletteSize = 1 * 1024;
    static constexpr std::size_t kVramSize    = 96 * 1024;
    static constexpr std::size_t kOamSize     = 1 * 1024;
    static constexpr std::size_t kMaxRomSize  = 32 * 1024 * 1024;

    std::array<u8, kEwramSize>   ewram_{};
    std::array<u8, kIwramSize>   iwram_{};
    std::array<u8, kPaletteSize> palette_{};
    std::array<u8, kVramSize>    vram_{};
    std::array<u8, kOamSize>     oam_{};
    std::vector<u8>              rom_;

    // TODO: BIOS ROM, I/O register file, DMA/timer/interrupt state.
};

} // namespace gba
