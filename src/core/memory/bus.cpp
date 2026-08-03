#include "core/memory/bus.h"

#include <cstdio>
#include <fstream>

namespace gba {

Bus::Bus() = default;

bool Bus::LoadRom(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::fprintf(stderr, "Bus::LoadRom: failed to open '%s'\n", path.c_str());
        return false;
    }

    const std::streamsize size = file.tellg();
    if (size <= 0 || static_cast<std::size_t>(size) > kMaxRomSize) {
        std::fprintf(stderr, "Bus::LoadRom: '%s' has invalid size %lld\n",
                     path.c_str(), static_cast<long long>(size));
        return false;
    }

    rom_.resize(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(rom_.data()), size)) {
        std::fprintf(stderr, "Bus::LoadRom: read failed for '%s'\n", path.c_str());
        rom_.clear();
        return false;
    }

    return true;
}

// NOTE on the current state of this function: only EWRAM/IWRAM/ROM are wired
// up so far, each masked to its region size to emulate mirroring. BIOS, I/O
// registers, palette/VRAM/OAM reads, and open-bus behavior for unmapped
// regions are TODOs for the memory-map milestone - ppu.cpp will need
// palette/VRAM/OAM plumbed in once that's underway.
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
        default:
            // TODO: BIOS, I/O, palette, VRAM, OAM, open bus.
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
    switch (address & 0x0F00'0000) {
        case mem::kEwramBase:
            ewram_[address & (kEwramSize - 1)] = value;
            return;
        case mem::kIwramBase:
            iwram_[address & (kIwramSize - 1)] = value;
            return;
        default:
            // TODO: I/O, palette, VRAM, OAM. ROM writes are ignored (or used
            // for GPIO/RTC on some carts - not a concern until much later).
            return;
    }
}

void Bus::Write16(u32 address, u16 value) {
    address &= ~0x1u;
    Write8(address, static_cast<u8>(value & 0xFF));
    Write8(address + 1, static_cast<u8>((value >> 8) & 0xFF));
}

void Bus::Write32(u32 address, u32 value) {
    address &= ~0x3u;
    Write16(address, static_cast<u16>(value & 0xFFFF));
    Write16(address + 2, static_cast<u16>((value >> 16) & 0xFFFF));
}

} // namespace gba
