#include "core/cpu/cpu.h"

namespace gba {

// ---------------------------------------------------------------------
// Format 16: Bcc label
// 1101 cond[11:8] offset8[7:0]  ->  PC = PC+4 + SignExtend8(offset) * 2
//
// cond==1111 (SWI) is intercepted by the dispatcher before this is ever
// called. cond==1110 is architecturally UNDEFINED for this format on real
// hardware (that encoding is reserved) - we simplify by treating it as
// always-taken (AL) via CheckCondition's existing default-case behavior
// for unrecognized upper nibbles, since real GBA code never intentionally
// emits it. Worth revisiting if a test ROM ever hits it.
// ---------------------------------------------------------------------
void Cpu::ThumbConditionalBranch(u16 instruction) {
    const u32 armStyleCond = (static_cast<u32>(instruction) & 0x0F00u) << 20;
    if (!CheckCondition(armStyleCond)) {
        return;
    }

    const s32 offset8 = static_cast<s32>(static_cast<s8>(instruction & 0xFFu));
    // r15-as-operand quirk (see cpu.h): registers_[15] already holds this
    // instruction's address + 2 post-increment; +2 more gives PC+4.
    const u32 pc = registers_[15] + 2u;
    registers_[15] = static_cast<u32>(static_cast<s32>(pc) + offset8 * 2);
}

// ---------------------------------------------------------------------
// Format 17: SWI #imm8
// 1101 1111 imm8[7:0]  ->  software interrupt exception entry
// The imm8 "comment field" is only meaningful to whatever software
// convention reads it back out of the faulting instruction; hardware
// itself ignores it, so it's unused here.
// ---------------------------------------------------------------------
void Cpu::ThumbSoftwareInterrupt(u16 /*instruction*/) {
    EnterException(CpuMode::Supervisor, 0x0000'0008u);
}

// ---------------------------------------------------------------------
// Format 18: B label
// 11100 offset11[10:0]  ->  PC = PC+4 + SignExtend11(offset) * 2
// ---------------------------------------------------------------------
void Cpu::ThumbUnconditionalBranch(u16 instruction) {
    const u32 raw = static_cast<u32>(instruction & 0x07FFu);
    const s32 offset = static_cast<s32>(raw << 21) >> 20; // sign-extend 11 bits, then *2
    const u32 pc = registers_[15] + 2u; // PC+4
    registers_[15] = static_cast<u32>(static_cast<s32>(pc) + offset);
}

// ---------------------------------------------------------------------
// Format 19: BL label
// Encoded as two consecutive 16-bit instructions - there's no single BL
// opcode in Thumb, since a full-range branch-with-link target needs more
// bits than one halfword can hold:
//   First half  (H=0, bit11=0): 11110 offsetHigh11[10:0]
//       LR = PC+4 + (SignExtend11(offsetHigh) << 12)
//   Second half (H=1, bit11=1): 11111 offsetLow11[10:0]
//       PC = LR + (offsetLow << 1)
//       LR = (address of the instruction after this one) | 1
// The "| 1" on the return address is the same Thumb-return marker BX
// checks - it's not meaningful as an address bit, just a state flag
// riding along in bit0.
// ---------------------------------------------------------------------
void Cpu::ThumbLongBranchLink(u16 instruction) {
    const bool isSecondHalf = (instruction & (1u << 11)) != 0;
    const u32 offset11 = static_cast<u32>(instruction & 0x07FFu);

    if (!isSecondHalf) {
        const s32 signExtended = static_cast<s32>(offset11 << 21) >> 21; // sign-extend 11 bits
        const u32 pc = registers_[15] + 2u; // PC+4
        SetRegister(14, static_cast<u32>(static_cast<s32>(pc) + (signExtended << 12)));
        return;
    }

    // registers_[15] already holds (this instruction's address + 2), i.e.
    // exactly the address of the instruction after this one - no extra
    // adjustment needed here, unlike the PC-as-operand cases above.
    const u32 returnAddress = registers_[15];
    const u32 target = GetRegister(14) + (offset11 << 1);
    SetRegister(14, returnAddress | 1u);
    registers_[15] = target;
}

} // namespace gba
