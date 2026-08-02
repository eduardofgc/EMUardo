#include "core/cpu/cpu.h"

namespace gba {

// The 4-bit field mask (bits19-16) selects which 8-bit bytes of the PSR
// get overwritten: bit0=control(bits7:0, includes mode - dangerous to
// write casually), bit1=extension(bits15:8, unused on ARMv4T), bit2=status
// (bits23:16, unused on ARMv4T), bit3=flags(bits31:24, the common case:
// N/Z/C/V plus, on ARMv5+, Q - not present here).
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
        SetCpsr(result); // handles a mode-field change via SwitchMode internally
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
