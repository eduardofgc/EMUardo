#include "core/cpu/cpu.h"

namespace gba {

void Cpu::ArmBranch(u32 instruction) {
    const bool link = ((instruction >> 24) & 1u) != 0;

    u32 offset = instruction & 0x00FF'FFFFu;
    if ((offset & 0x0080'0000u) != 0) {
        offset |= 0xFF00'0000u; 
    }
    const s32 byteOffset = static_cast<s32>(offset) << 2;

    const u32 target = static_cast<u32>(static_cast<s32>(registers_[15]) + 4 + byteOffset);

    if (link) {

        registers_[14] = registers_[15];
    }
    registers_[15] = target;
}

void Cpu::ArmBranchExchange(u32 instruction) {
    const u32 rm = instruction & 0xFu;
    const u32 target = GetRegister(static_cast<int>(rm));

    SetFlag(Flag::T, (target & 1u) != 0);
    registers_[15] = target & ~1u;
}

void Cpu::ArmSoftwareInterrupt(u32 /*instruction*/) {
    EnterException(CpuMode::Supervisor, 0x0000'0008u);
}

void Cpu::ArmUndefined(u32 /*instruction*/) {
    EnterException(CpuMode::Undefined, 0x0000'0004u);
}

} // namespace gba
