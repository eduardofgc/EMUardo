#include "core/cpu/cpu.h"

namespace gba {

u32 Cpu::ApplyPsrFieldMask(u32 current, u32 value, u32 fieldMask) const {
    u32 byteMask = 0;
    if ((fieldMask & 0x1u) != 0) byteMask |= 0x0000'00FFu;
    if ((fieldMask & 0x2u) != 0) byteMask |= 0x0000'FF00u;
    if ((fieldMask & 0x4u) != 0) byteMask |= 0x00FF'0000u;
    if ((fieldMask & 0x8u) != 0) byteMask |= 0xFF00'0000u;
    return (current & ~byteMask) | (value & byteMask);
}

void Cpu::ArmMrs(u32 instruction) {
    const bool useSpsr = ((instruction >> 22) & 1u) != 0;
    const u32 rd = (instruction >> 12) & 0xFu;
    SetRegister(static_cast<int>(rd), useSpsr ? Spsr() : Cpsr());
}

void Cpu::ArmMsrRegister(u32 instruction) {
    const bool useSpsr = ((instruction >> 22) & 1u) != 0;
    const u32 fieldMask = (instruction >> 16) & 0xFu;
    const u32 rm = instruction & 0xFu;
    const u32 value = GetRegister(static_cast<int>(rm));

    const u32 current = useSpsr ? Spsr() : Cpsr();
    const u32 result = ApplyPsrFieldMask(current, value, fieldMask);

    if (useSpsr) {
        SetSpsr(result);
    } else {
        SetCpsr(result); 
    }
}

void Cpu::ArmMsrImmediate(u32 instruction) {
    const bool useSpsr = ((instruction >> 22) & 1u) != 0;
    const u32 fieldMask = (instruction >> 16) & 0xFu;

    bool discardedCarry = false;
    const u32 value = GetImmediateOperand2(instruction, discardedCarry);

    const u32 current = useSpsr ? Spsr() : Cpsr();
    const u32 result = ApplyPsrFieldMask(current, value, fieldMask);

    if (useSpsr) {
        SetSpsr(result);
    } else {
        SetCpsr(result);
    }
}

} // namespace gba
