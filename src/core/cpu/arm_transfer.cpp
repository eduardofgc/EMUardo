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

u32 Cpu::ReadRotatedWord(u32 address) const {
    const u32 value = bus_.Read32(address);
    const u32 rotateBits = (address & 0x3u) * 8u;
    return rotateBits == 0 ? value : ((value >> rotateBits) | (value << (32u - rotateBits)));
}

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
        const u32 temp = ReadRotatedWord(address);
        bus_.Write32(address, GetRegister(static_cast<int>(rm)));
        SetRegister(static_cast<int>(rd), temp);
    }
}

// LDRH/STRH/LDRSB/LDRSH - distinguished from ARM's other addressing modes
// by using bit22 (not bit25) to select immediate vs register offset.
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
        // Only STRH (sh == 01) is a valid store form here.
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
    // NOTE: opposite convention from the halfword-transfer family above -
    // here bit25 selects the offset kind: 0 = immediate, 1 = shifted register.
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
        // Shift amount is always an immediate here (bit4 is guaranteed 0 -
        // register-specified shift amounts aren't a valid encoding for
        // this instruction class). The shifter's carry output is discarded:
        // LDR/STR never affects condition flags.
        const u32 rm = instruction & 0xFu;
        const auto shiftType = static_cast<ShiftType>((instruction >> 5) & 0x3u);
        const u32 shiftAmount = (instruction >> 7) & 0x1Fu;
        bool discardedCarry = false;
        offset = Shift(shiftType, GetRegister(static_cast<int>(rm)), shiftAmount,
                        discardedCarry, /*isImmediateShift=*/true);
    }

    // PC-as-base quirk (see cpu.h): +4 more on top of the stored +4 gives PC+8.
    const u32 base = (rn == 15) ? registers_[15] + 4 : GetRegister(static_cast<int>(rn));
    const u32 address = preIndex ? (up ? base + offset : base - offset) : base;

    if (load) {
        const u32 value = byteTransfer ? bus_.Read8(address) : ReadRotatedWord(address);
        SetRegister(static_cast<int>(rd), value);
        if (rd == 15) {
            registers_[15] &= ~0x3u; // keep PC word-aligned after a load into it
        }
    } else {
        u32 value = GetRegister(static_cast<int>(rd));
        if (rd == 15) {
            value = registers_[15] + 4; // storing PC also reads the PC+8 value
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

// LDM/STM. Registers are always transferred in ascending register-number
// order to ascending addresses; the P/U bits only choose where that
// ascending range sits relative to the base register (GBATEK's
// IA/IB/DA/DB addressing modes).
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

    // GBATEK "Block Data Transfer (LDM,STM)": when the S bit is set and
    // this isn't "LDM with r15 in the list" (that case restores CPSR from
    // SPSR instead, handled below), the transferred registers are read
    // from - or written to - the User-mode bank regardless of the CPU's
    // actual current mode. Real OSes use this to save/restore user-mode
    // context from a privileged handler; the GBA has no OS, but some
    // games' own interrupt/context-switch code still does it. The base
    // register itself is exempt from this redirection - `base` above was
    // already read from the current mode's Rn, and the writeback below
    // (after switching back) writes it there too, never to User's copy.
    const bool forceUserBank = loadPsrOrForceUser && !restoreCpsrFromSpsr;
    const CpuMode savedMode = GetMode();
    if (forceUserBank) {
        SwitchMode(CpuMode::User);
    }

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

    if (forceUserBank) {
        SwitchMode(savedMode);
    }

    if (restoreCpsrFromSpsr) {
        SetCpsr(Spsr());
    }

    // Per the ARM spec, writeback with the base register in the list
    // during an LDM is left to the loaded value rather than the computed
    // writeback address (the loaded value "wins" since it's written last
    // in real hardware's internal ordering too).
    const bool baseWasLoaded = load && ((regList >> rn) & 1u) != 0;
    if (writeback && !baseWasLoaded) {
        SetRegister(static_cast<int>(rn), writebackAddress);
    }
}

} // namespace gba
