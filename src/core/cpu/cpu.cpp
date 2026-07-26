#include "core/cpu/cpu.h"

namespace gba {

Cpu::Cpu(Bus& bus) : bus_(bus) {
    Reset();
}

void Cpu::Reset() {
    registers_.fill(0);
    r8_12_fiq_.fill(0);
    r8_12_other_.fill(0);
    r13_banked_.fill(0);
    r14_banked_.fill(0);
    spsr_banked_.fill(0);

    // Real hardware runs a bit of BIOS code that ends up jumping to
    // 0x0800'0000 in ARM state, System mode, IRQ/FIQ disabled. We
    // fast-forward directly here - a "skip BIOS" boot path is standard
    // even in mature emulators, and lets us test without a BIOS dump.
    registers_[15] = mem::kRomBase;
    cpsr_ = static_cast<u32>(CpuMode::System) |
            static_cast<u32>(Flag::I) |
            static_cast<u32>(Flag::F);
}

// ---------------------------------------------------------------------
// Register access / mode switching
// ---------------------------------------------------------------------

u32 Cpu::GetRegister(int index) const {
    return registers_[static_cast<std::size_t>(index)];
}

void Cpu::SetRegister(int index, u32 value) {
    registers_[static_cast<std::size_t>(index)] = value;
}

bool Cpu::GetFlag(Flag flag) const {
    return (cpsr_ & static_cast<u32>(flag)) != 0;
}

void Cpu::SetFlag(Flag flag, bool set) {
    if (set) {
        cpsr_ |= static_cast<u32>(flag);
    } else {
        cpsr_ &= ~static_cast<u32>(flag);
    }
}

void Cpu::SetCpsr(u32 value) {
    const auto newMode = static_cast<CpuMode>(value & 0x1Fu);
    if (newMode != GetMode()) {
        SwitchMode(newMode);
    }
    cpsr_ = value;
}

int Cpu::R13R14BankIndex(CpuMode mode) {
    switch (mode) {
        case CpuMode::FIQ:        return 1;
        case CpuMode::IRQ:        return 2;
        case CpuMode::Supervisor: return 3;
        case CpuMode::Abort:      return 4;
        case CpuMode::Undefined:  return 5;
        default:                  return 0; // User / System share a bank
    }
}

int Cpu::SpsrBankIndex(CpuMode mode) {
    switch (mode) {
        case CpuMode::FIQ:        return 0;
        case CpuMode::IRQ:        return 1;
        case CpuMode::Supervisor: return 2;
        case CpuMode::Abort:      return 3;
        case CpuMode::Undefined:  return 4;
        default:                  return -1; // User / System have no SPSR
    }
}

void Cpu::SwitchMode(CpuMode newMode) {
    const CpuMode oldMode = GetMode();
    if (oldMode == newMode) {
        return;
    }

    // Save r8-r12 to whichever bank the outgoing mode uses.
    auto& oldR8_12 = (oldMode == CpuMode::FIQ) ? r8_12_fiq_ : r8_12_other_;
    for (int i = 0; i < 5; ++i) {
        oldR8_12[static_cast<std::size_t>(i)] = registers_[static_cast<std::size_t>(8 + i)];
    }

    // Save r13/r14 to the outgoing mode's bank.
    const int oldBank = R13R14BankIndex(oldMode);
    r13_banked_[static_cast<std::size_t>(oldBank)] = registers_[13];
    r14_banked_[static_cast<std::size_t>(oldBank)] = registers_[14];

    // Load r8-r12 for the incoming mode.
    auto& newR8_12 = (newMode == CpuMode::FIQ) ? r8_12_fiq_ : r8_12_other_;
    for (int i = 0; i < 5; ++i) {
        registers_[static_cast<std::size_t>(8 + i)] = newR8_12[static_cast<std::size_t>(i)];
    }

    // Load r13/r14 for the incoming mode.
    const int newBank = R13R14BankIndex(newMode);
    registers_[13] = r13_banked_[static_cast<std::size_t>(newBank)];
    registers_[14] = r14_banked_[static_cast<std::size_t>(newBank)];

    cpsr_ = (cpsr_ & ~0x1Fu) | static_cast<u32>(newMode);
}

u32 Cpu::Spsr() const {
    const int idx = SpsrBankIndex(GetMode());
    // Reading SPSR in User/System mode is architecturally undefined (there
    // isn't one) - returning CPSR is a harmless fallback rather than
    // reading garbage.
    return idx >= 0 ? spsr_banked_[static_cast<std::size_t>(idx)] : cpsr_;
}

void Cpu::SetSpsr(u32 value) {
    const int idx = SpsrBankIndex(GetMode());
    if (idx >= 0) {
        spsr_banked_[static_cast<std::size_t>(idx)] = value;
    }
}

void Cpu::EnterException(CpuMode mode, u32 vectorAddress) {
    // registers_[15] already holds "address of the instruction after this
    // one" per the fetch-then-increment convention described in cpu.h, so
    // it's exactly the return address exceptions need in LR.
    const u32 returnPc = registers_[15];
    const u32 oldCpsr = cpsr_;

    SwitchMode(mode);
    registers_[14] = returnPc;
    SetSpsr(oldCpsr);
    SetFlag(Flag::I, true);
    if (mode == CpuMode::FIQ) {
        SetFlag(Flag::F, true);
    }
    SetFlag(Flag::T, false); // exceptions always enter ARM state
    registers_[15] = vectorAddress;
}

// ---------------------------------------------------------------------
// Fetch / condition checking
// ---------------------------------------------------------------------

u32 Cpu::FetchArm() {
    return bus_.Read32(registers_[15]);
}

u16 Cpu::FetchThumb() {
    return bus_.Read16(registers_[15]);
}

bool Cpu::CheckCondition(u32 instruction) const {
    switch (instruction >> 28) {
        case 0x0: return GetFlag(Flag::Z);                                       // EQ
        case 0x1: return !GetFlag(Flag::Z);                                      // NE
        case 0x2: return GetFlag(Flag::C);                                       // CS/HS
        case 0x3: return !GetFlag(Flag::C);                                      // CC/LO
        case 0x4: return GetFlag(Flag::N);                                       // MI
        case 0x5: return !GetFlag(Flag::N);                                      // PL
        case 0x6: return GetFlag(Flag::V);                                       // VS
        case 0x7: return !GetFlag(Flag::V);                                      // VC
        case 0x8: return GetFlag(Flag::C) && !GetFlag(Flag::Z);                  // HI
        case 0x9: return !GetFlag(Flag::C) || GetFlag(Flag::Z);                  // LS
        case 0xA: return GetFlag(Flag::N) == GetFlag(Flag::V);                   // GE
        case 0xB: return GetFlag(Flag::N) != GetFlag(Flag::V);                   // LT
        case 0xC: return !GetFlag(Flag::Z) && (GetFlag(Flag::N) == GetFlag(Flag::V)); // GT
        case 0xD: return GetFlag(Flag::Z) || (GetFlag(Flag::N) != GetFlag(Flag::V));  // LE
        case 0xE: return true;                                                   // AL
        default:  return false;                                                  // 0xF: reserved/never on ARMv4T
    }
}

int Cpu::Step() {
    if (GetFlag(Flag::T)) {
        // Thumb decode isn't implemented yet - that's the next milestone
        // after this one. Fetch-and-skip keeps the loop well-defined
        // rather than silently reading garbage forever.
        FetchThumb();
        registers_[15] += 2;
        return 1;
    }

    const u32 instruction = FetchArm();
    registers_[15] += 4; // advance PC before execute - see cpu.h pipeline note

    if (CheckCondition(instruction)) {
        ExecuteArm(instruction);
    }
    return 1; // TODO: real per-instruction cycle costs (GBATEK timing tables)
}

// ---------------------------------------------------------------------
// Barrel shifter
// ---------------------------------------------------------------------

u32 Cpu::Shift(ShiftType type, u32 value, u32 amount, bool& carryInOut,
               bool isImmediateShift) const {
    if (isImmediateShift && amount == 0) {
        switch (type) {
            case ShiftType::LSL:
                return value; // carry unchanged
            case ShiftType::LSR:
                amount = 32; // LSR#0 encodes LSR#32
                break;
            case ShiftType::ASR:
                amount = 32; // ASR#0 encodes ASR#32
                break;
            case ShiftType::ROR: {
                // ROR#0 encodes RRX: rotate right by 1 through the carry flag.
                const bool oldCarry = carryInOut;
                const u32 result = (value >> 1) | (oldCarry ? 0x8000'0000u : 0u);
                carryInOut = (value & 1u) != 0;
                return result;
            }
        }
    } else if (!isImmediateShift && amount == 0) {
        return value; // register-specified shift of 0: no-op, carry unchanged
    }

    switch (type) {
        case ShiftType::LSL:
            if (amount >= 32) {
                carryInOut = (amount == 32) && ((value & 1u) != 0);
                return 0;
            }
            carryInOut = ((value >> (32 - amount)) & 1u) != 0;
            return value << amount;

        case ShiftType::LSR:
            if (amount >= 32) {
                carryInOut = (amount == 32) && (((value >> 31) & 1u) != 0);
                return 0;
            }
            carryInOut = ((value >> (amount - 1)) & 1u) != 0;
            return value >> amount;

        case ShiftType::ASR: {
            const bool signBit = (value & 0x8000'0000u) != 0;
            if (amount >= 32) {
                carryInOut = signBit;
                return signBit ? 0xFFFF'FFFFu : 0u;
            }
            carryInOut = ((value >> (amount - 1)) & 1u) != 0;
            return static_cast<u32>(static_cast<s32>(value) >> amount);
        }

        case ShiftType::ROR: {
            const u32 rotated = amount & 31u;
            if (rotated == 0) {
                // Original amount was a nonzero multiple of 32.
                carryInOut = ((value >> 31) & 1u) != 0;
                return value;
            }
            carryInOut = ((value >> (rotated - 1)) & 1u) != 0;
            return (value >> rotated) | (value << (32 - rotated));
        }
    }
    return value; // unreachable
}

u32 Cpu::GetImmediateOperand2(u32 instruction, bool& carryOut) const {
    const u32 imm = instruction & 0xFFu;
    const u32 rotate = ((instruction >> 8) & 0xFu) * 2;
    if (rotate == 0) {
        carryOut = GetFlag(Flag::C);
        return imm;
    }
    const u32 result = (imm >> rotate) | (imm << (32 - rotate));
    carryOut = ((result >> 31) & 1u) != 0;
    return result;
}

u32 Cpu::GetRegisterOperand2(u32 instruction, bool& carryOut) const {
    const u32 rm = instruction & 0xFu;
    const bool shiftFromRegister = ((instruction >> 4) & 1u) != 0;

    u32 value = (rm == 15)
        ? registers_[15] + (shiftFromRegister ? 8u : 4u)
        : GetRegister(static_cast<int>(rm));

    const auto shiftType = static_cast<ShiftType>((instruction >> 5) & 0x3u);
    bool carry = GetFlag(Flag::C);

    if (shiftFromRegister) {
        const u32 rs = (instruction >> 8) & 0xFu;
        const u32 amount = GetRegister(static_cast<int>(rs)) & 0xFFu;
        value = Shift(shiftType, value, amount, carry, /*isImmediateShift=*/false);
    } else {
        const u32 amount = (instruction >> 7) & 0x1Fu;
        value = Shift(shiftType, value, amount, carry, /*isImmediateShift=*/true);
    }

    carryOut = carry;
    return value;
}


void Cpu::ExecuteArm(u32 instruction) {
    if ((instruction & 0x0FFF'FFF0u) == 0x012F'FF10u) {
        ArmBranchExchange(instruction);
        return;
    }

    // PSR Transfer - MRS (read): cond 00010 R 001111 Rd 000000000000
    if ((instruction & 0x0FBF'0FFFu) == 0x010F'0000u) {
        ArmMrs(instruction);
        return;
    }

    // PSR Transfer - MSR, register operand: cond 00010 R 10 1001 1111 00000000 Rm
    if ((instruction & 0x0FB0'FFF0u) == 0x0120'F000u) {
        ArmMsrRegister(instruction);
        return;
    }

    // PSR Transfer - MSR, immediate operand: cond 00 1 10 R 10 1111 rotate imm8
    if ((instruction & 0x0FB0'F000u) == 0x0320'F000u) {
        ArmMsrImmediate(instruction);
        return;
    }

    // Multiply (MUL/MLA): bits27-22=000000, bits7-4=1001
    if ((instruction & 0x0FC0'00F0u) == 0x0000'0090u) {
        ArmMultiply(instruction);
        return;
    }

    // Multiply Long (UMULL/UMLAL/SMULL/SMLAL): bits27-23=00001, bits7-4=1001
    if ((instruction & 0x0F80'00F0u) == 0x0080'0090u) {
        ArmMultiplyLong(instruction);
        return;
    }

    // Single Data Swap (SWP/SWPB): bits27-23=00010, bits11-4=00001001
    if ((instruction & 0x0FB0'0FF0u) == 0x0100'0090u) {
        ArmSingleDataSwap(instruction);
        return;
    }

    // Halfword/Signed Data Transfer family: bits27-25=000, bit7=1, bit4=1,
    // with bits6-5 != 00 (that case is one of the three patterns above).
    if ((instruction & 0x0E00'0090u) == 0x0000'0090u) {
        ArmHalfwordTransfer(instruction);
        return;
    }

    const u32 bits27_26 = (instruction >> 26) & 0x3u;

    switch (bits27_26) {
        case 0b00:

            ArmDataProcessing(instruction);
            return;

        case 0b01: {
            const bool immediateBit = ((instruction >> 25) & 1u) != 0;
            const bool bit4 = ((instruction >> 4) & 1u) != 0;
            if (immediateBit && bit4) {
                ArmUndefined(instruction);
            } else {
                ArmSingleDataTransfer(instruction);
            }
            return;
        }

        case 0b10: {
            const bool branchBit = ((instruction >> 25) & 1u) != 0;
            if (branchBit) {
                ArmBranch(instruction);
            } else {
                ArmBlockDataTransfer(instruction);
            }
            return;
        }

        case 0b11: {
            const bool isSwi = ((instruction >> 24) & 0xFu) == 0xFu;
            if (isSwi) {
                ArmSoftwareInterrupt(instruction);
            } else {
                ArmUndefined(instruction);
            }
            return;
        }
    }
}

} // namespace gba
