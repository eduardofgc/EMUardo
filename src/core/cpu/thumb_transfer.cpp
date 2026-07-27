#include "core/cpu/cpu.h"

namespace gba {

// Format 6: LDR Rd, [PC, #Word8*4]
void Cpu::ThumbPcRelativeLoad(u16 instruction) {
    const u32 rd = (instruction >> 8) & 0x7u;
    const u32 word8 = instruction & 0xFFu;

    // PC read as (this instruction's address + 4), with bit1 forced to 0
    // per GBATEK ("as PC bit 1 is always read as 0" for this format).
    const u32 base = (registers_[15] + 2u) & ~0x3u;
    SetRegister(static_cast<int>(rd), bus_.Read32(base + word8 * 4u));
}

// Format 7: LDR/STR{B} Rd, [Rb, Ro]
void Cpu::ThumbLoadStoreRegOffset(u16 instruction) {
    const bool load = ((instruction >> 11) & 1u) != 0;
    const bool byteTransfer = ((instruction >> 10) & 1u) != 0;
    const u32 ro = (instruction >> 6) & 0x7u;
    const u32 rb = (instruction >> 3) & 0x7u;
    const u32 rd = instruction & 0x7u;

    const u32 address = GetRegister(static_cast<int>(rb)) + GetRegister(static_cast<int>(ro));

    if (load) {
        SetRegister(static_cast<int>(rd), byteTransfer ? bus_.Read8(address) : bus_.Read32(address));
    } else {
        const u32 value = GetRegister(static_cast<int>(rd));
        if (byteTransfer) {
            bus_.Write8(address, static_cast<u8>(value));
        } else {
            bus_.Write32(address, value);
        }
    }
}

// Format 8: LDRH/STRH/LDSB/LDSH Rd, [Rb, Ro] - the H/S bits select which of
// the four; unlike Format 7 there's no byte-vs-word STR here (STRH is the
// only store form, per GBATEK).
void Cpu::ThumbLoadStoreSignExtended(u16 instruction) {
    const bool hFlag = ((instruction >> 11) & 1u) != 0;
    const bool sFlag = ((instruction >> 10) & 1u) != 0;
    const u32 ro = (instruction >> 6) & 0x7u;
    const u32 rb = (instruction >> 3) & 0x7u;
    const u32 rd = instruction & 0x7u;

    const u32 address = GetRegister(static_cast<int>(rb)) + GetRegister(static_cast<int>(ro));

    if (!sFlag && !hFlag) { // STRH
        bus_.Write16(address, static_cast<u16>(GetRegister(static_cast<int>(rd))));
        return;
    }

    u32 value;
    if (!sFlag) { // LDRH
        value = bus_.Read16(address);
    } else if (!hFlag) { // LDSB
        value = static_cast<u32>(static_cast<s32>(static_cast<s8>(bus_.Read8(address))));
    } else { // LDSH
        value = static_cast<u32>(static_cast<s32>(static_cast<s16>(bus_.Read16(address))));
    }
    SetRegister(static_cast<int>(rd), value);
}

// Format 9: LDR/STR{B} Rd, [Rb, #Offset5] - offset is in words unless the
// byte-transfer bit is set, in which case it's a raw byte count.
void Cpu::ThumbLoadStoreImmOffset(u16 instruction) {
    const bool byteTransfer = ((instruction >> 12) & 1u) != 0;
    const bool load = ((instruction >> 11) & 1u) != 0;
    const u32 offset5 = (instruction >> 6) & 0x1Fu;
    const u32 rb = (instruction >> 3) & 0x7u;
    const u32 rd = instruction & 0x7u;

    const u32 offset = byteTransfer ? offset5 : (offset5 * 4u);
    const u32 address = GetRegister(static_cast<int>(rb)) + offset;

    if (load) {
        SetRegister(static_cast<int>(rd), byteTransfer ? bus_.Read8(address) : bus_.Read32(address));
    } else {
        const u32 value = GetRegister(static_cast<int>(rd));
        if (byteTransfer) {
            bus_.Write8(address, static_cast<u8>(value));
        } else {
            bus_.Write32(address, value);
        }
    }
}

// Format 10: LDRH/STRH Rd, [Rb, #Offset5*2]
void Cpu::ThumbLoadStoreHalfword(u16 instruction) {
    const bool load = ((instruction >> 11) & 1u) != 0;
    const u32 offset5 = (instruction >> 6) & 0x1Fu;
    const u32 rb = (instruction >> 3) & 0x7u;
    const u32 rd = instruction & 0x7u;

    const u32 address = GetRegister(static_cast<int>(rb)) + offset5 * 2u;

    if (load) {
        SetRegister(static_cast<int>(rd), bus_.Read16(address));
    } else {
        bus_.Write16(address, static_cast<u16>(GetRegister(static_cast<int>(rd))));
    }
}

// Format 11: LDR/STR Rd, [SP, #Word8*4]
void Cpu::ThumbSpRelativeLoadStore(u16 instruction) {
    const bool load = ((instruction >> 11) & 1u) != 0;
    const u32 rd = (instruction >> 8) & 0x7u;
    const u32 word8 = instruction & 0xFFu;

    const u32 address = GetRegister(13) + word8 * 4u;

    if (load) {
        SetRegister(static_cast<int>(rd), bus_.Read32(address));
    } else {
        bus_.Write32(address, GetRegister(static_cast<int>(rd)));
    }
}

// Format 12: ADD Rd, PC/SP, #Word8*4 - computes an address, doesn't load.
void Cpu::ThumbLoadAddress(u16 instruction) {
    const bool useSp = ((instruction >> 11) & 1u) != 0;
    const u32 rd = (instruction >> 8) & 0x7u;
    const u32 word8 = instruction & 0xFFu;

    const u32 base = useSp ? GetRegister(13) : ((registers_[15] + 2u) & ~0x3u);
    SetRegister(static_cast<int>(rd), base + word8 * 4u);
}

// Format 13: ADD SP, #SOffset7*4 (signed via a dedicated bit, not two's
// complement - GBATEK "ADD SP,#nn").
void Cpu::ThumbAddOffsetToSp(u16 instruction) {
    const bool negative = ((instruction >> 7) & 1u) != 0;
    const u32 offset = (instruction & 0x7Fu) * 4u;

    const u32 sp = GetRegister(13);
    SetRegister(13, negative ? sp - offset : sp + offset);
}

// Format 14: PUSH/POP {rlist}{LR/PC}. Registers in the 8-bit list are
// always r0-r7, transferred in ascending order; the extra bit adds LR (on
// push) or PC (on pop) at the far end of the range.
void Cpu::ThumbPushPopRegisters(u16 instruction) {
    const bool load = ((instruction >> 11) & 1u) != 0;
    const bool includeExtra = ((instruction >> 8) & 1u) != 0;
    const u8 regList = static_cast<u8>(instruction & 0xFFu);

    int count = 0;
    for (int i = 0; i < 8; ++i) {
        if (((regList >> i) & 1u) != 0) ++count;
    }
    if (includeExtra) ++count;

    if (load) { // POP
        u32 address = GetRegister(13);
        for (int i = 0; i < 8; ++i) {
            if (((regList >> i) & 1u) == 0) continue;
            SetRegister(i, bus_.Read32(address));
            address += 4u;
        }
        if (includeExtra) {
            // POP {PC} loads a return address; it doesn't itself act like
            // BX (no implicit Thumb/ARM state switch here per GBATEK), so
            // we just force halfword alignment and keep executing Thumb.
            registers_[15] = bus_.Read32(address) & ~1u;
            address += 4u;
        }
        SetRegister(13, address);
    } else { // PUSH
        const u32 writebackSp = GetRegister(13) - static_cast<u32>(count) * 4u;
        u32 address = writebackSp;
        for (int i = 0; i < 8; ++i) {
            if (((regList >> i) & 1u) == 0) continue;
            bus_.Write32(address, GetRegister(i));
            address += 4u;
        }
        if (includeExtra) {
            bus_.Write32(address, GetRegister(14));
        }
        SetRegister(13, writebackSp);
    }
}

// Format 15: STMIA/LDMIA Rb!, {rlist} - always writes back, always
// ascending addresses, r0-r7 only.
void Cpu::ThumbMultipleLoadStore(u16 instruction) {
    const bool load = ((instruction >> 11) & 1u) != 0;
    const u32 rb = (instruction >> 8) & 0x7u;
    const u8 regList = static_cast<u8>(instruction & 0xFFu);

    u32 address = GetRegister(static_cast<int>(rb));
    bool baseWasLoaded = false;

    for (int i = 0; i < 8; ++i) {
        if (((regList >> i) & 1u) == 0) continue;
        if (load) {
            SetRegister(i, bus_.Read32(address));
            if (i == static_cast<int>(rb)) baseWasLoaded = true;
        } else {
            bus_.Write32(address, GetRegister(i));
        }
        address += 4u;
    }

    // Skip writeback only if Rb was itself loaded - same rule as ARM LDM
    // in arm_transfer.cpp, and for the same reason (the loaded value wins).
    if (!(load && baseWasLoaded)) {
        SetRegister(static_cast<int>(rb), address);
    }
}

} // namespace gba
