#include "core/memory/bus.h"

#include <cstdio>
#include <fstream>

namespace gba {

namespace {
// "foo/bar.gba" -> "foo/bar.sav" - same directory, same base name, so the
// save file is easy to find next to the ROM (matching every other GBA
// emulator's convention). A ROM with no extension just gets ".sav"
// appended.
std::string DeriveSavePath(const std::string& romPath) {
    const std::size_t lastSlash = romPath.find_last_of("/\\");
    const std::size_t lastDot = romPath.find_last_of('.');
    if (lastDot != std::string::npos && (lastSlash == std::string::npos || lastDot > lastSlash)) {
        return romPath.substr(0, lastDot) + ".sav";
    }
    return romPath + ".sav";
}
} // namespace

Bus::Bus() {
    InstallHleBios();
    // KEYINPUT is active-LOW; io_ defaults to all zero bits, which would
    // read as "every button held" until the first real input update -
    // set it to "nothing pressed" up front instead.
    io_[io::kKeyInput] = static_cast<u8>(key::kAll & 0xFFu);
    io_[io::kKeyInput + 1] = static_cast<u8>((key::kAll >> 8) & 0xFFu);
}

void Bus::InstallHleBios() {
    // Writes 32-bit little-endian words directly into bios_, bypassing
    // Write8/Write32 entirely - this is boot-time setup of the BIOS image
    // itself, not a memory-mapped write a CPU instruction would make.
    auto writeWord = [this](u32 offset, u32 word) {
        bios_[offset + 0] = static_cast<u8>(word & 0xFFu);
        bios_[offset + 1] = static_cast<u8>((word >> 8) & 0xFFu);
        bios_[offset + 2] = static_cast<u8>((word >> 16) & 0xFFu);
        bios_[offset + 3] = static_cast<u8>((word >> 24) & 0xFFu);
    };

    // IRQ vector (0x18) trampoline. See the class comment in bus.h for
    // what this does and why it exists instead of a real BIOS dump.
    //   0x18: STMFD SP!, {R0-R3,R12,LR}
    //   0x1C: LDR  R0, [PC, #0x10]     ; R0 = the literal at 0x34 below
    //   0x20: LDR  R0, [R0]            ; R0 = *0x03007FFC (user handler ptr)
    //   0x24: MOV  LR, PC              ; LR = 0x2C (return address after BX)
    //   0x28: BX   R0                  ; call the user handler
    //   0x2C: LDMFD SP!, {R0-R3,R12,LR}
    //   0x30: SUBS PC, LR, #4          ; return from IRQ (also restores CPSR from SPSR)
    //   0x34: .word 0x03007FFC         ; literal pool
    writeWord(0x18, 0xE92D'500Fu);
    writeWord(0x1C, 0xE59F'0010u);
    writeWord(0x20, 0xE590'0000u);
    writeWord(0x24, 0xE1A0'E00Fu);
    writeWord(0x28, 0xE12F'FF10u);
    writeWord(0x2C, 0xE8BD'500Fu);
    writeWord(0x30, 0xE25E'F004u);
    writeWord(0x34, 0x0300'7FFCu);
}

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

    save_.DetectFromRom(rom_);
    savePath_ = DeriveSavePath(path);
    save_.LoadFromFile(savePath_); // no existing .sav file is fine - fresh/erased save data is already in place

    return true;
}

bool Bus::FlushSave() {
    if (!save_.IsDirty()) {
        return true;
    }
    return save_.SaveToFile(savePath_);
}

// NOTE on the current state of this function: open-bus behavior for
// genuinely unmapped regions is still TODO. Everything the CPU and PPU
// actually need so far - the HLE BIOS trampoline, EWRAM/IWRAM/ROM, I/O
// registers, palette/VRAM/OAM - is wired up.
u8 Bus::Read8(u32 address) const {
    switch (address & 0x0F00'0000) {
        case mem::kBiosBase:
            return bios_[address & (kBiosSize - 1)];
        case mem::kEwramBase:
            return ewram_[address & (kEwramSize - 1)];
        case mem::kIwramBase:
            return iwram_[address & (kIwramSize - 1)];
        case mem::kIoBase:
            return io_[address & (kIoSize - 1)];
        case mem::kPaletteBase:
            return palette_[address & (kPaletteSize - 1)];
        case mem::kVramBase: {
            // VRAM mirrors within a 128 KB window, but only 96 KB of it is
            // real - the last 32 KB of that window mirrors the 32 KB
            // immediately before it, rather than repeating the whole thing.
            // See GBATEK "VRAM Mirroring".
            std::size_t offset = address & 0x1'FFFF;
            if (offset >= kVramSize) {
                offset -= 0x8000;
            }
            return vram_[offset];
        }
        case mem::kOamBase:
            return oam_[address & (kOamSize - 1)];
        case mem::kRomBase:
        case mem::kRomBase + 0x0100'0000:
        case mem::kRomBase + 0x0200'0000: {
            const std::size_t offset = address & (kMaxRomSize - 1);
            return offset < rom_.size() ? rom_[offset] : 0;
        }
        case mem::kSramBase:
            return save_.ReadSram(address);
        default:
            // TODO: BIOS reads, open bus for genuinely unmapped regions.
            return 0;
    }
}

u16 Bus::Read16(u32 address) const {
    address &= ~0x1u; // halfword-aligned
    if (save_.type() == SaveMemory::Type::kEeprom && (address & 0x0F00'0000) == mem::kEepromBase) {
        // EEPROM is a 1-bit-per-halfword serial protocol, not a normal
        // byte-addressable region - see SaveMemory::ReadEeprom(). Handled
        // here rather than in Read8 because composing it from two byte
        // reads would double-count protocol bits.
        return save_.ReadEeprom();
    }
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
        case mem::kIoBase: {
            const std::size_t offset = address & (kIoSize - 1);
            if (offset == io::kIf || offset == io::kIf + 1) {
                // IF: writing a 1 to a bit clears it (acknowledges that
                // interrupt); writing 0 leaves the bit alone. GBATEK
                // "IF - Interrupt Request Flags". This is why IF can't
                // just be a plain byte in the flat array like most of the
                // I/O region - a normal write would set bits, not clear
                // them.
                io_[offset] &= static_cast<u8>(~value);
                return;
            }
            if (offset == io::kKeyInput || offset == io::kKeyInput + 1) {
                // KEYINPUT is hardware-controlled (only Bus::SetKeyState
                // updates it) - real hardware ignores CPU writes here too.
                return;
            }
            io_[offset] = value;
            return;
        }
        case mem::kPaletteBase:
            palette_[address & (kPaletteSize - 1)] = value;
            return;
        case mem::kVramBase: {
            std::size_t offset = address & 0x1'FFFF;
            if (offset >= kVramSize) {
                offset -= 0x8000;
            }
            vram_[offset] = value;
            return;
        }
        case mem::kOamBase:
            oam_[address & (kOamSize - 1)] = value;
            return;
        case mem::kSramBase:
            save_.WriteSram(address, value);
            return;
        default:
            // ROM writes are ignored (or used for GPIO/RTC on some carts -
            // not a concern until much later).
            return;
    }
}

void Bus::Write16(u32 address, u16 value) {
    address &= ~0x1u;
    if (save_.type() == SaveMemory::Type::kEeprom && (address & 0x0F00'0000) == mem::kEepromBase) {
        save_.WriteEeprom(value);
        return;
    }
    Write8(address, static_cast<u8>(value & 0xFF));
    Write8(address + 1, static_cast<u8>((value >> 8) & 0xFF));
}

void Bus::Write32(u32 address, u32 value) {
    address &= ~0x3u;
    Write16(address, static_cast<u16>(value & 0xFFFF));
    Write16(address + 2, static_cast<u16>((value >> 16) & 0xFFFF));
}

void Bus::RequestInterrupt(u16 flagBit) {
    const u32 address = mem::kIoBase + io::kIf;
    const std::size_t offset = address & (kIoSize - 1);
    // Direct array OR, not the public Write16 path - see Write8's IF
    // special case for why a normal write wouldn't do the right thing
    // here (it would try to clear bits rather than set them).
    const u16 current = static_cast<u16>(io_[offset] | (io_[offset + 1] << 8));
    const u16 updated = current | flagBit;
    io_[offset] = static_cast<u8>(updated & 0xFF);
    io_[offset + 1] = static_cast<u8>((updated >> 8) & 0xFF);
}

void Bus::SetKeyState(u16 pressedMask) {
    const u16 keyInputValue = static_cast<u16>(~pressedMask & key::kAll);
    const std::size_t offset = io::kKeyInput;
    io_[offset] = static_cast<u8>(keyInputValue & 0xFFu);
    io_[offset + 1] = static_cast<u8>((keyInputValue >> 8) & 0xFFu);

    // KEYCNT: bits0-9 select which keys, bit14 = IRQ enable, bit15 =
    // condition (0=OR, any selected key; 1=AND, all selected keys).
    const u16 keycnt = static_cast<u16>(io_[io::kKeyCnt] | (io_[io::kKeyCnt + 1] << 8));
    const bool irqEnable = (keycnt & (1u << 14)) != 0;
    if (!irqEnable) {
        return;
    }
    const u16 selected = keycnt & key::kAll;
    const bool useAnd = (keycnt & (1u << 15)) != 0;
    const bool condition = useAnd ? ((pressedMask & selected) == selected)
                                   : ((pressedMask & selected) != 0);
    if (condition) {
        RequestInterrupt(irq::kKeypad);
    }
}

} // namespace gba
