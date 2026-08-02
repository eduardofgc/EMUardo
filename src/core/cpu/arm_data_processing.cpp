#include "core/cpu/cpu.h"

namespace gba {

namespace {

// Every ARM add/subtract instruction (ADD/ADC/SUB/SBC/RSB/RSC/CMP/CMN) can
// be expressed as a 3-input add: a + b + carryIn. Subtraction is done by
// inverting the second operand and setting carryIn appropriately - this
// mirrors what the ARM7TDMI's internal adder actually does, and it means
// carry/overflow only need to be computed correctly in one place.
struct AddResult {
    u32 value;
    bool carry;
    bool overflow;
};

AddResult AddWithCarry(u32 a, u32 b, u32 carryIn) {
    const u64 sum = static_cast<u64>(a) + static_cast<u64>(b) + static_cast<u64>(carryIn);
    const u32 result = static_cast<u32>(sum);
    const bool carry = sum > 0xFFFF'FFFFull;
    const bool overflow = ((~(a ^ b)) & (a ^ result) & 0x8000'0000u) != 0;
    return {result, carry, overflow};
}

} // namespace

void Cpu::ArmDataProcessing(u32 instruction) {
    const u32 opcode = (instruction >> 21) & 0xFu;
    const bool immediate = ((instruction >> 25) & 1u) != 0;
    const bool setFlags = ((instruction >> 20) & 1u) != 0;
    const u32 rn = (instruction >> 16) & 0xFu;
    const u32 rd = (instruction >> 12) & 0xFu;

    bool shifterCarry;
    const u32 operand2 = immediate
        ? GetImmediateOperand2(instruction, shifterCarry)
        : GetRegisterOperand2(instruction, shifterCarry);

    // PC-as-operand quirk (see cpu.h): registers_[15] holds current+4
    // already, so +4 more gives the PC+8 value GBATEK specifies.
    const u32 operand1 = (rn == 15) ? registers_[15] + 4 : GetRegister(static_cast<int>(rn));

    u32 result = 0;
    bool carryOut = shifterCarry;
    bool overflowOut = GetFlag(Flag::V); // arithmetic ops overwrite this; logical ops leave V untouched
    bool writesResult = true;

    switch (opcode) {
        case 0x0: // AND
            result = operand1 & operand2;
            break;
        case 0x1: // EOR
            result = operand1 ^ operand2;
            break;
        case 0x2: { // SUB
            const auto r = AddWithCarry(operand1, ~operand2, 1);
            result = r.value; carryOut = r.carry; overflowOut = r.overflow;
            break;
        }
        case 0x3: { // RSB
            const auto r = AddWithCarry(operand2, ~operand1, 1);
            result = r.value; carryOut = r.carry; overflowOut = r.overflow;
            break;
        }
        case 0x4: { // ADD
            const auto r = AddWithCarry(operand1, operand2, 0);
            result = r.value; carryOut = r.carry; overflowOut = r.overflow;
            break;
        }
        case 0x5: { // ADC
            const auto r = AddWithCarry(operand1, operand2, GetFlag(Flag::C) ? 1u : 0u);
            result = r.value; carryOut = r.carry; overflowOut = r.overflow;
            break;
        }
        case 0x6: { // SBC
            const auto r = AddWithCarry(operand1, ~operand2, GetFlag(Flag::C) ? 1u : 0u);
            result = r.value; carryOut = r.carry; overflowOut = r.overflow;
            break;
        }
        case 0x7: { // RSC
            const auto r = AddWithCarry(operand2, ~operand1, GetFlag(Flag::C) ? 1u : 0u);
            result = r.value; carryOut = r.carry; overflowOut = r.overflow;
            break;
        }
        case 0x8: // TST
            result = operand1 & operand2;
            writesResult = false;
            break;
        case 0x9: // TEQ
            result = operand1 ^ operand2;
            writesResult = false;
            break;
        case 0xA: { // CMP
            const auto r = AddWithCarry(operand1, ~operand2, 1);
            result = r.value; carryOut = r.carry; overflowOut = r.overflow;
            writesResult = false;
            break;
        }
        case 0xB: { // CMN
            const auto r = AddWithCarry(operand1, operand2, 0);
            result = r.value; carryOut = r.carry; overflowOut = r.overflow;
            writesResult = false;
            break;
        }
        case 0xC: // ORR
            result = operand1 | operand2;
            break;
        case 0xD: // MOV
            result = operand2;
            break;
        case 0xE: // BIC
            result = operand1 & ~operand2;
            break;
        case 0xF: // MVN
            result = ~operand2;
            break;
    }

    if (writesResult) {
        SetRegister(static_cast<int>(rd), result);
    }

    if (setFlags) {
        if (rd == 15) {
            // Writing CPSR from SPSR is how privileged code returns from
            // an exception via e.g. "MOVS PC, LR" - restores mode, flags,
            // and IRQ/FIQ masks all at once.
            SetCpsr(Spsr());
        } else {
            SetFlag(Flag::Z, result == 0);
            SetFlag(Flag::N, (result & 0x8000'0000u) != 0);
            SetFlag(Flag::C, carryOut);
            SetFlag(Flag::V, overflowOut);
        }
    }
}

} // namespace gba
