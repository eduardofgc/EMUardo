#include "core/cpu/cpu.h"

namespace gba {

// Format 1: LSL/LSR/ASR Rd, Rs, #Offset5
void Cpu::ThumbMoveShiftedRegister(u16 instruction) {
    const u32 op = (instruction >> 11) & 0x3u; // 0=LSL,1=LSR,2=ASR (3 belongs to Format 2)
    const u32 offset5 = (instruction >> 6) & 0x1Fu;
    const u32 rs = (instruction >> 3) & 0x7u;
    const u32 rd = instruction & 0x7u;

    bool carry = GetFlag(Flag::C);
    const u32 result = Shift(static_cast<ShiftType>(op), GetRegister(static_cast<int>(rs)),
                              offset5, carry, /*isImmediateShift=*/true);
    SetRegister(static_cast<int>(rd), result);

    SetFlag(Flag::Z, result == 0);
    SetFlag(Flag::N, (result & 0x8000'0000u) != 0);
    SetFlag(Flag::C, carry);
}

// Format 2: ADD/SUB Rd, Rs, Rn  (or with a 3-bit immediate instead of Rn)
void Cpu::ThumbAddSubtract(u16 instruction) {
    const bool immediate = ((instruction >> 10) & 1u) != 0;
    const bool subtract = ((instruction >> 9) & 1u) != 0;
    const u32 rnOrImm = (instruction >> 6) & 0x7u;
    const u32 rs = (instruction >> 3) & 0x7u;
    const u32 rd = instruction & 0x7u;

    const u32 operand1 = GetRegister(static_cast<int>(rs));
    const u32 operand2 = immediate ? rnOrImm : GetRegister(static_cast<int>(rnOrImm));

    const u64 wide = subtract
        ? static_cast<u64>(operand1) + static_cast<u64>(~operand2) + 1u
        : static_cast<u64>(operand1) + static_cast<u64>(operand2);
    const u32 result = static_cast<u32>(wide);

    SetRegister(static_cast<int>(rd), result);

    SetFlag(Flag::Z, result == 0);
    SetFlag(Flag::N, (result & 0x8000'0000u) != 0);
    SetFlag(Flag::C, subtract ? (operand1 >= operand2) : (wide > 0xFFFF'FFFFull));
    const bool overflow = subtract
        ? (((operand1 ^ operand2) & (operand1 ^ result)) & 0x8000'0000u) != 0
        : ((~(operand1 ^ operand2) & (operand1 ^ result)) & 0x8000'0000u) != 0;
    SetFlag(Flag::V, overflow);
}

// Format 3: MOV/CMP/ADD/SUB Rd, #Offset8. Note MOV only touches N/Z here -
// unlike Format 4's MOV-equivalent-free-lunch, this one deliberately
// leaves C/V alone per the ARM7TDMI TRM.
void Cpu::ThumbImmediateOp(u16 instruction) {
    const u32 op = (instruction >> 11) & 0x3u;
    const u32 rd = (instruction >> 8) & 0x7u;
    const u32 imm = instruction & 0xFFu;

    const u32 current = GetRegister(static_cast<int>(rd));
    u32 result = 0;
    bool writesResult = true;
    bool carry = GetFlag(Flag::C);
    bool overflow = GetFlag(Flag::V);

    switch (op) {
        case 0b00: // MOV
            result = imm;
            break;
        case 0b01: { // CMP
            const u64 diff = static_cast<u64>(current) + static_cast<u64>(~imm) + 1u;
            result = static_cast<u32>(diff);
            carry = current >= imm;
            overflow = (((current ^ imm) & (current ^ result)) & 0x8000'0000u) != 0;
            writesResult = false;
            break;
        }
        case 0b10: { // ADD
            const u64 sum = static_cast<u64>(current) + static_cast<u64>(imm);
            result = static_cast<u32>(sum);
            carry = sum > 0xFFFF'FFFFull;
            overflow = ((~(current ^ imm) & (current ^ result)) & 0x8000'0000u) != 0;
            break;
        }
        case 0b11: { // SUB
            const u64 diff = static_cast<u64>(current) + static_cast<u64>(~imm) + 1u;
            result = static_cast<u32>(diff);
            carry = current >= imm;
            overflow = (((current ^ imm) & (current ^ result)) & 0x8000'0000u) != 0;
            break;
        }
    }

    if (writesResult) {
        SetRegister(static_cast<int>(rd), result);
    }

    SetFlag(Flag::Z, result == 0);
    SetFlag(Flag::N, (result & 0x8000'0000u) != 0);
    if (op != 0b00) {
        SetFlag(Flag::C, carry);
        SetFlag(Flag::V, overflow);
    }
}

// Format 4: two-register ALU operations (AND/EOR/LSL/LSR/ASR/ADC/SBC/ROR/
// TST/NEG/CMP/CMN/ORR/MUL/BIC/MVN). Logical ops leave C/V untouched;
// register-specified shifts and the arithmetic ops update them normally.
void Cpu::ThumbAluOp(u16 instruction) {
    const u32 op = (instruction >> 6) & 0xFu;
    const u32 rs = (instruction >> 3) & 0x7u;
    const u32 rd = instruction & 0x7u;

    const u32 operand1 = GetRegister(static_cast<int>(rd));
    const u32 operand2 = GetRegister(static_cast<int>(rs));

    u32 result = 0;
    bool writesResult = true;
    bool carry = GetFlag(Flag::C);
    bool overflow = GetFlag(Flag::V);
    bool skipCV = false;

    switch (op) {
        case 0x0: result = operand1 & operand2; skipCV = true; break; // AND
        case 0x1: result = operand1 ^ operand2; skipCV = true; break; // EOR
        case 0x2: result = Shift(ShiftType::LSL, operand1, operand2 & 0xFFu, carry, false); break; // LSL
        case 0x3: result = Shift(ShiftType::LSR, operand1, operand2 & 0xFFu, carry, false); break; // LSR
        case 0x4: result = Shift(ShiftType::ASR, operand1, operand2 & 0xFFu, carry, false); break; // ASR
        case 0x5: { // ADC
            const u64 sum = static_cast<u64>(operand1) + operand2 + (GetFlag(Flag::C) ? 1u : 0u);
            result = static_cast<u32>(sum);
            carry = sum > 0xFFFF'FFFFull;
            overflow = ((~(operand1 ^ operand2) & (operand1 ^ result)) & 0x8000'0000u) != 0;
            break;
        }
        case 0x6: { // SBC: operand1 - operand2 - !C
            const u32 carryIn = GetFlag(Flag::C) ? 1u : 0u;
            const u64 diff = static_cast<u64>(operand1) + static_cast<u64>(~operand2) + carryIn;
            result = static_cast<u32>(diff);
            carry = diff > 0xFFFF'FFFFull;
            overflow = (((operand1 ^ operand2) & (operand1 ^ result)) & 0x8000'0000u) != 0;
            break;
        }
        case 0x7: result = Shift(ShiftType::ROR, operand1, operand2 & 0xFFu, carry, false); break; // ROR
        case 0x8: result = operand1 & operand2; skipCV = true; writesResult = false; break; // TST
        case 0x9: { // NEG: Rd = 0 - Rs
            result = static_cast<u32>(0u - operand2);
            carry = operand2 == 0u; // no borrow only when negating zero
            overflow = (operand2 & result & 0x8000'0000u) != 0; // overflows only negating INT_MIN
            break;
        }
        case 0xA: { // CMP
            const u64 diff = static_cast<u64>(operand1) + static_cast<u64>(~operand2) + 1u;
            result = static_cast<u32>(diff);
            carry = operand1 >= operand2;
            overflow = (((operand1 ^ operand2) & (operand1 ^ result)) & 0x8000'0000u) != 0;
            writesResult = false;
            break;
        }
        case 0xB: { // CMN
            const u64 sum = static_cast<u64>(operand1) + operand2;
            result = static_cast<u32>(sum);
            carry = sum > 0xFFFF'FFFFull;
            overflow = ((~(operand1 ^ operand2) & (operand1 ^ result)) & 0x8000'0000u) != 0;
            writesResult = false;
            break;
        }
        case 0xC: result = operand1 | operand2; skipCV = true; break; // ORR
        case 0xD: result = operand1 * operand2; skipCV = true; break; // MUL (C meaningless per TRM)
        case 0xE: result = operand1 & ~operand2; skipCV = true; break; // BIC
        case 0xF: result = ~operand2; skipCV = true; break; // MVN
    }

    if (writesResult) {
        SetRegister(static_cast<int>(rd), result);
    }

    SetFlag(Flag::Z, result == 0);
    SetFlag(Flag::N, (result & 0x8000'0000u) != 0);
    if (!skipCV) {
        SetFlag(Flag::C, carry);
        SetFlag(Flag::V, overflow);
    }
}

// Format 5: ADD/CMP/MOV using any register (including r8-r15 via the H1/H2
// bits), plus BX. This is how Thumb code reaches the high registers and
// switches back to ARM state.
void Cpu::ThumbHiRegOpsBranchExchange(u16 instruction) {
    const u32 op = (instruction >> 8) & 0x3u;
    const bool h1 = ((instruction >> 7) & 1u) != 0;
    const bool h2 = ((instruction >> 6) & 1u) != 0;
    const u32 rsField = (instruction >> 3) & 0x7u;
    const u32 rdField = instruction & 0x7u;

    const int rs = static_cast<int>(rsField) + (h2 ? 8 : 0);
    const int rd = static_cast<int>(rdField) + (h1 ? 8 : 0);

    // r15-as-operand quirk (Thumb version of the note in cpu.h): registers_[15]
    // already holds this instruction's address + 2 post-increment; +2 more
    // gives the PC+4 value Thumb-state operand reads use.
    const u32 operand2 = (rs == 15) ? (registers_[15] + 2u) : GetRegister(rs);

    switch (op) {
        case 0b00: { // ADD - flags NOT affected (TRM Format 5)
            const u32 operand1 = (rd == 15) ? (registers_[15] + 2u) : GetRegister(rd);
            SetRegister(rd, operand1 + operand2);
            if (rd == 15) registers_[15] &= ~1u;
            break;
        }
        case 0b01: { // CMP
            const u32 operand1 = (rd == 15) ? (registers_[15] + 2u) : GetRegister(rd);
            const u64 diff = static_cast<u64>(operand1) + static_cast<u64>(~operand2) + 1u;
            const u32 result = static_cast<u32>(diff);
            SetFlag(Flag::Z, result == 0);
            SetFlag(Flag::N, (result & 0x8000'0000u) != 0);
            SetFlag(Flag::C, operand1 >= operand2);
            SetFlag(Flag::V, (((operand1 ^ operand2) & (operand1 ^ result)) & 0x8000'0000u) != 0);
            break;
        }
        case 0b10: // MOV - flags NOT affected
            SetRegister(rd, operand2);
            if (rd == 15) registers_[15] &= ~1u;
            break;
        case 0b11: // BX (BLX doesn't exist on ARMv4T)
            SetFlag(Flag::T, (operand2 & 1u) != 0);
            registers_[15] = operand2 & ~1u;
            break;
    }
}

} // namespace gba
