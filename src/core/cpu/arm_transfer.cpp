#include "core/cpu/cpu.h"

namespace gba {

namespace {
u32 SignExtend8(u8 value) { return static_cast<u32>(static_cast<s32>(static_cast<s8>(value))); }
u32 SignExtend16(u16 value) { return static_cast<u32>(static_cast<s32>(static_cast<s16>(value))); }
int PopCount16(u16 value) {
    int count = 0;
    for (int i = 0; i < 16; ++i) {
        if ((value >> i) & 1u) ++count;
    }
    return count;
}
} // namespace

void Cpu::ArmSingleDataSwap(u32 instruction) {
    const bool byteSwap = ((instruction >> 22) & 1u) != 0;
    const u32 rn = (instruction >> 16) & 0xFu;
    const u32 rd = (instruction >> 12) & 0xFu;
    const u32 rm = instruction & 0xFu;

    const u32 address = GetRegister(static_cast<int>(rn));
    if (byteSwap) {
        const u8 temp = bus_.Read8(address);
        bus_.Write8(address, static_cast<u8>(GetRegister(static_cast<int>(rm))));
        SetRegister(static_cast<int>(rd), temp);
    } else {
        const u32 temp = bus_.Read32(address);
        bus_.Write32(address, GetRegister(static_cast<int>(rm)));
        SetRegister(static_cast<int>(rd), temp);
    }
}

void Cpu::ArmHalfwordTransfer(u32 instruction) {
    const bool preIndex = ((instruction >> 24) & 1u) != 0;
    const bool up = ((instruction >> 23) & 1u) != 0;
    const bool immediateOffset = ((instruction >> 22) & 1u) != 0;
    const bool writeback = ((instruction >> 21) & 1u) != 0;
    const bool load = ((instruction >> 20) & 1u) != 0;
    const u32 rn = (instruction >> 16) & 0xFu;
    const u32 rd = (instruction >> 12) & 0xFu;
    const u32 sh = (instruction >> 5) & 0x3u;

    const u32 offset = immediateOffset
        ? (((instruction >> 4) & 0xF0u) | (instruction & 0xFu))
        : GetRegister(static_cast<int>(instruction & 0xFu));

    const u32 base = GetRegister(static_cast<int>(rn));
    u32 address = preIndex ? (up ? base + offset : base - offset) : base;

    if (load) {
        u32 value = 0;
        switch (sh) {
            case 0b01: value = bus_.Read16(address); break;              // LDRH
            case 0b10: value = SignExtend8(bus_.Read8(address)); break;  // LDRSB
            case 0b11: value = SignExtend16(bus_.Read16(address)); break; // LDRSH
            default: break; // 00 is the SWP encoding, never reaches here
        }
        SetRegister(static_cast<int>(rd), value);
    } else {

        bus_.Write16(address, static_cast<u16>(GetRegister(static_cast<int>(rd))));
    }

    if (!preIndex) {
        address = up ? base + offset : base - offset;
    }
    if (writeback || !preIndex) {
        SetRegister(static_cast<int>(rn), address);
    }
}

void Cpu::ArmSingleDataTransfer(u32 instruction) {

    const bool immediateOffset = ((instruction >> 25) & 1u) == 0;
    const bool preIndex = ((instruction >> 24) & 1u) != 0;
    const bool up = ((instruction >> 23) & 1u) != 0;
    const bool byteTransfer = ((instruction >> 22) & 1u) != 0;
    const bool writeback = ((instruction >> 21) & 1u) != 0;
    const bool load = ((instruction >> 20) & 1u) != 0;
    const u32 rn = (instruction >> 16) & 0xFu;
    const u32 rd = (instruction >> 12) & 0xFu;

    u32 offset;
    if (immediateOffset) {
        offset = instruction & 0xFFFu;
    } else {

        const u32 rm = instruction & 0xFu;
        const auto shiftType = static_cast<ShiftType>((instruction >> 5) & 0x3u);
        const u32 shiftAmount = (instruction >> 7) & 0x1Fu;
        bool discardedCarry = false;
        offset = Shift(shiftType, GetRegister(static_cast<int>(rm)), shiftAmount,
                        discardedCarry, /*isImmediateShift=*/true);
    }

    const u32 base = (rn == 15) ? registers_[15] + 4 : GetRegister(static_cast<int>(rn));
    const u32 address = preIndex ? (up ? base + offset : base - offset) : base;

    if (load) {
        const u32 value = byteTransfer ? bus_.Read8(address) : bus_.Read32(address);
        SetRegister(static_cast<int>(rd), value);
        if (rd == 15) {
            registers_[15] &= ~0x3u;
        }
    } else {
        u32 value = GetRegister(static_cast<int>(rd));
        if (rd == 15) {
            value = registers_[15] + 4; 
        }
        if (byteTransfer) {
            bus_.Write8(address, static_cast<u8>(value));
        } else {
            bus_.Write32(address, value);
        }
    }

    if (!preIndex) {
        SetRegister(static_cast<int>(rn), up ? base + offset : base - offset);
    } else if (writeback) {
        SetRegister(static_cast<int>(rn), address);
    }
}


void Cpu::ArmBlockDataTransfer(u32 instruction) {
    const bool preIndex = ((instruction >> 24) & 1u) != 0;
    const bool up = ((instruction >> 23) & 1u) != 0;
    const bool loadPsrOrForceUser = ((instruction >> 22) & 1u) != 0;
    const bool writeback = ((instruction >> 21) & 1u) != 0;
    const bool load = ((instruction >> 20) & 1u) != 0;
    const u32 rn = (instruction >> 16) & 0xFu;
    const u16 regList = static_cast<u16>(instruction & 0xFFFFu);

    const int numRegs = PopCount16(regList);
    const u32 base = GetRegister(static_cast<int>(rn));

    u32 startAddress;
    u32 writebackAddress;
    if (up) {
        startAddress = preIndex ? base + 4 : base;
        writebackAddress = base + static_cast<u32>(numRegs) * 4;
    } else {
        startAddress = preIndex ? base - static_cast<u32>(numRegs) * 4
                                 : base - static_cast<u32>(numRegs) * 4 + 4;
        writebackAddress = base - static_cast<u32>(numRegs) * 4;
    }


    const bool restoreCpsrFromSpsr = loadPsrOrForceUser && load && ((regList & 0x8000u) != 0);

    u32 address = startAddress;
    for (int i = 0; i < 16; ++i) {
        if (((regList >> i) & 1u) == 0) {
            continue;
        }
        if (load) {
            SetRegister(i, bus_.Read32(address));
        } else {
            bus_.Write32(address, GetRegister(i));
        }
        address += 4;
    }

    if (restoreCpsrFromSpsr) {
        SetCpsr(Spsr());
    }


    const bool baseWasLoaded = load && ((regList >> rn) & 1u) != 0;
    if (writeback && !baseWasLoaded) {
        SetRegister(static_cast<int>(rn), writebackAddress);
    }
}

} // namespace gba
