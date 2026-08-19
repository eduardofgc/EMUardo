#include "core/cpu/cpu.h"

namespace gba {

void Cpu::ArmBranch(u32 instruction) {
    const bool link = ((instruction >> 24) & 1u) != 0;

    // 24-bit signed word offset -> sign-extend to 32 bits, then convert
    // from word units to byte units (<< 2).
    u32 offset = instruction & 0x00FF'FFFFu;
    if ((offset & 0x0080'0000u) != 0) {
        offset |= 0xFF00'0000u; // sign-extend
    }
    const s32 byteOffset = static_cast<s32>(offset) << 2;

    // registers_[15] currently holds this instruction's address + 4 (see
    // cpu.h pipeline note); branches are PC-relative to PC+8, so add one
    // more word to reach the real base before applying the offset.
    const u32 target = static_cast<u32>(static_cast<s32>(registers_[15]) + 4 + byteOffset);

    if (link) {
        // LR = address of the instruction after this branch, which is
        // exactly what registers_[15] already holds.
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

void Cpu::ArmSoftwareInterrupt(u32 instruction) {
    // The BIOS call number is the top byte of the 24-bit "comment" field
    // (bits[23:16]) - the rest of that field is only meaningful to a
    // disassembler, hardware ignores it. GBATEK "SWI Instruction".
    const u32 number = (instruction >> 16) & 0xFFu;
    if (TryHleSwi(number)) {
        return;
    }
    // Not HLE'd, and we have no real BIOS code sitting at the SWI vector
    // (0x08) to jump to - unlike the IRQ vector (0x18), which does hold a
    // working trampoline. Entering the exception here would switch to
    // Supervisor mode and start executing whatever zero bytes happen to
    // be there, which (confirmed by tracing a real hang) eventually
    // marches forward far enough to hit the IRQ trampoline by sheer
    // address coincidence and calls the game's real interrupt handler
    // completely out of context - corrupting CPU state far worse than
    // just not implementing the call. Treating it as a no-op is closer
    // to real hardware's own behavior for the (documented as harmless)
    // case of calling an unassigned SWI number anyway.
}

void Cpu::ArmUndefined(u32 /*instruction*/) {
    EnterException(CpuMode::Undefined, 0x0000'0004u);
}

} // namespace gba
