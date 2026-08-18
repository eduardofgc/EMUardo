#include "core/memory/bus.h"

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace gba {

Bus::Bus() {
    // Initialize I/O registers: set VBlank flag in REG_DISPSTAT (assume at 0x04000002) to 1
    io_reg_[2] = 0x01; // VBlank bit set
    vram_write_count_ = 0;
}

bool Bus::LoadRom(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::fprintf(stderr, "Bus::LoadRom: failed to open '%s'\\n", path.c_str());
        return false;
    }

    const std::streamsize size = file.tellg();
    if (size <= 0 || static_cast<std::size_t>(size) > kMaxRomSize) {
        std::fprintf(stderr, "Bus::LoadRom: '%s' has invalid size %lld\\n",
                     path.c_str(), static_cast<long long>(size));
        return false;
    }

    rom_.resize(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(rom_.data()), size)) {
        std::fprintf(stderr, "Bus::LoadRom: read failed for '%s'\\n", path.c_str());
        rom_.clear();
        return false;
    }

    // Print debug info about the ROM header
    if (rom_.size() >= 0xC0) {
        std::fprintf(stderr, "Bus::LoadRom: ROM loaded, size=%zu bytes\\n", rom_.size());
        std::fprintf(stderr, "  Entry point: 0x%08X\\n",
                     *reinterpret_cast<u32*>(&rom_[0x1C]));
        std::fprintf(stderr, "  First 4 instructions (at 0x08000000):\\n");
        for (int i = 0; i < 4; ++i) {
            u32 instr = *reinterpret_cast<u32*>(&rom_[i*4]);
            std::fprintf(stderr, "    0x%08X: 0x%08X\\n", 0x08000000 + i*4, instr);
        }
    }

    return true;
}

u8 Bus::Read8(u32 address) const {
    switch (address & 0x0F00'0000) {
        case mem::kEwramBase:
            return ewram_[address & (kEwramSize - 1)];
        case mem::kIwramBase:
            return iwram_[address & (kIwramSize - 1)];
        case mem::kRomBase:
        case mem::kRomBase + 0x0100'0000:
        case mem::kRomBase + 0x0200'0000: {
            const std::size_t offset = address & (kMaxRomSize - 1);
            return offset < rom_.size() ? rom_[offset] : 0;
        }
        case 0x0400'0000: // I/O registers (mirrored every 0x1000, but we only have the first 0x400 bytes)
            return io_reg_[address & 0x3FF];
        case 0x0500'0000: // Palette RAM
            return palette_[address & (kPaletteSize - 1)];
        case 0x0600'0000: // Video RAM
            return vram_[address & (kVramSize - 1)];
        case 0x0700'0000: // Object Attribute Memory
            return oam_[address & (kOamSize - 1)];
        default:
            // BIOS (0x0000'0000-0x01FF'ffff) and other unimplemented areas -> open bus (return 0 for now)
            return 0;
    }
}

u16 Bus::Read16(u32 address) const {
    address &= ~0x1u; // halfword-aligned
    return static_cast<u16>(Read8(address) | (Read8(address + 1) << 8));
}

u32 Bus::Read32(u32 address) const {
    address &= ~0x3u; // word-aligned
    return static_cast<u32>(Read16(address)) |
           (static_cast<u32>(Read16(address + 2)) << 16);
}

void Bus::Write8(u32 address, u8 value) {
    // I/O range: 0x04000000-0x04003FF
    if (address >= 0x04000000 && address < 0x0400400) {
        std::fprintf(stderr, "Bus: Write8 to 0x%08X = 0x%02X\\n", address, value);
    }
    // VRAM range: 0x0600'0000-0x0601'FFFF
    if (address >= 0x06000000 && address < 0x06020000) {
        if (vram_write_count_ < 5) {
            std::fprintf(stderr, "Bus: Write8 to VRAM 0x%08X = 0x%02X\\n", address, value);
        }
        vram_write_count_++;
    }
    switch (address & 0x0F00'0000) {
        case mem::kEwramBase:
            ewram_[address & (kEwramSize - 1)] = value;
            return;
        case mem::kIwramBase:
            iwram_[address & (kIwramSize - 1)] = value;
            return;
        case 0x0400'0000: // I/O registers
            io_reg_[address & 0x3FF] = value;
            return;
        case 0x0500'0000: // Palette RAM
            palette_[address & (kPaletteSize - 1)] = value;
            return;
        case 0x0600'0000: // Video RAM
            vram_[address & (kVramSize - 1)] = value;
            return;
        case 0x0700'0000: // Object Attribute Memory
            oam_[address & (kOamSize - 1)] = value;
            return;
        default:
            // ROM is read-only, BIOS and other areas are ignored for writes
            return;
    }
}

void Bus::Write16(u32 address, u16 value) {
    address &= ~0x1u;
    // I/O range: 0x04000000-0x04003FF
    if (address >= 0x04000000 && address < 0x0400400) {
        std::fprintf(stderr, "Bus: Write16 to 0x%08X = 0x%04X\\n", address, value);
    }
    // VRAM range
    if (address >= 0x06000000 && address < 0x06020000) {
        if (vram_write_count_ < 5) {
            std::fprintf(stderr, "Bus: Write16 to VRAM 0x%08X = 0x%04X\\n", address, value);
        }
        vram_write_count_++;
    }
    Write8(address, static_cast<u8>(value & 0xFF));
    Write8(address + 1, static_cast<u8>((value >> 8) & 0xFF));
}

void Bus::Write32(u32 address, u32 value) {
    address &= ~0x3u;
    // I/O range: 0x04000000-0x04003FF
    if (address >= 0x04000000 && address < 0x0400400) {
        std::fprintf(stderr, "Bus: Write32 to 0x%08X = 0x%08X\\n", address, value);
    }
    // VRAM range
    if (address >= 0x06000000 && address < 0x06020000) {
        if (vram_write_count_ < 5) {
            std::fprintf(stderr, "Bus: Write32 to VRAM 0x%08X = 0x%08X\\n", address, value);
        }
        vram_write_count_++;
    }
    Write16(address, static_cast<u16>(value & 0xFFFF));
    Write16(address + 2, static_cast<u16>((value >> 16) & 0xFFFF));
}

} // namespace gba