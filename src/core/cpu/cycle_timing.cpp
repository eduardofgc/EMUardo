#include "core/cpu/cpu.h"

namespace gba {

namespace {
int PopCount16(u16 value) {
    int count = 0;
    for (int i = 0; i < 16; ++i) {
        if ((value >> i) & 1u) ++count;
    }
    return count;
}

// GBATEK "ARM Multiply Instruction Cycle Times" - the extra internal (I)
// cycles a multiply takes depend on how many of Rs's high bytes are
// "boring": all zero, OR (for the signed-style test MUL/MLA/SMULL/SMLAL
// use) all one, since the underlying hardware multiplier can skip early
// once the remaining partial products would all be zero.
int MultiplyCyclesSigned(u32 rs) {
    if ((rs & 0xFFFF'FF00u) == 0u || (rs & 0xFFFF'FF00u) == 0xFFFF'FF00u) return 1;
    if ((rs & 0xFFFF'0000u) == 0u || (rs & 0xFFFF'0000u) == 0xFFFF'0000u) return 2;
    if ((rs & 0xFF00'0000u) == 0u || (rs & 0xFF00'0000u) == 0xFF00'0000u) return 3;
    return 4;
}

// UMULL/UMLAL use the plain unsigned-magnitude version of the same test
// (only "all zero" counts, not "all one" - there's no sign to exploit).
int MultiplyCyclesUnsigned(u32 rs) {
    if ((rs & 0xFFFF'FF00u) == 0u) return 1;
    if ((rs & 0xFFFF'0000u) == 0u) return 2;
    if ((rs & 0xFF00'0000u) == 0u) return 3;
    return 4;
}
} // namespace

int Cpu::ComputeArmCycles(u32 instruction) const {
    // Mirrors ExecuteArm's dispatch order/bit patterns exactly (see its
    // comments for what each pattern decodes) - this just classifies,
    // rather than executes, so it can run before ExecuteArm without
    // duplicating any actual instruction semantics.

    // Branch and Exchange (BX): pipeline flush, 2S+1N.
    if ((instruction & 0x0FFF'FFF0u) == 0x012F'FF10u) {
        return 3;
    }

    // MRS/MSR (PSR transfer): 1S.
    if ((instruction & 0x0FBF'0FFFu) == 0x010F'0000u ||
        (instruction & 0x0FB0'FFF0u) == 0x0120'F000u ||
        (instruction & 0x0FB0'F000u) == 0x0320'F000u) {
        return 1;
    }

    // Multiply (MUL/MLA): 1S + mI, +1I more if accumulating (MLA).
    if ((instruction & 0x0FC0'00F0u) == 0x0000'0090u) {
        const bool accumulate = ((instruction >> 21) & 1u) != 0;
        const u32 rs = (instruction >> 8) & 0xFu;
        return 1 + MultiplyCyclesSigned(GetRegister(static_cast<int>(rs))) + (accumulate ? 1 : 0);
    }

    // Multiply Long (UMULL/UMLAL/SMULL/SMLAL): 1S + (m+1)I, +1I more if
    // accumulating.
    if ((instruction & 0x0F80'00F0u) == 0x0080'0090u) {
        const bool isSigned = ((instruction >> 22) & 1u) != 0;
        const bool accumulate = ((instruction >> 21) & 1u) != 0;
        const u32 rs = (instruction >> 8) & 0xFu;
        const u32 rsValue = GetRegister(static_cast<int>(rs));
        const int m = isSigned ? MultiplyCyclesSigned(rsValue) : MultiplyCyclesUnsigned(rsValue);
        return 1 + (m + 1) + (accumulate ? 1 : 0);
    }

    // Single Data Swap (SWP/SWPB): 1S+2N+1I.
    if ((instruction & 0x0FB0'0FF0u) == 0x0100'0090u) {
        return 4;
    }

    // Halfword/Signed Data Transfer family (LDRH/STRH/LDRSB/LDRSH).
    if ((instruction & 0x0E00'0090u) == 0x0000'0090u) {
        const bool load = ((instruction >> 20) & 1u) != 0;
        return load ? 3 : 2; // LDR-family: 1S+1N+1I; STRH: 2N
    }

    const u32 bits27_26 = (instruction >> 26) & 0x3u;

    switch (bits27_26) {
        case 0b00: {
            // Data Processing. +1I if operand2's shift amount comes from a
            // register rather than an immediate; +1S+1N more if this
            // instruction actually writes r15 (TST/TEQ/CMP/CMN never
            // write their Rd field, even though the field is still
            // present in the encoding - by this point in the dispatch,
            // those four opcodes with S=0 have already been peeled off
            // above as PSR transfers, so any of them reaching here has
            // S=1 and genuinely doesn't write Rd).
            const bool immediateOperand = ((instruction >> 25) & 1u) != 0;
            const bool registerShift = !immediateOperand && ((instruction >> 4) & 1u) != 0;
            const u32 opcode = (instruction >> 21) & 0xFu;
            const bool isTestOp = (opcode == 0x8u || opcode == 0x9u || opcode == 0xAu || opcode == 0xBu);
            const u32 rd = (instruction >> 12) & 0xFu;
            const bool writesPc = !isTestOp && rd == 15u;
            return 1 + (registerShift ? 1 : 0) + (writesPc ? 2 : 0);
        }

        case 0b01: {
            // Single Data Transfer (LDR/STR), or the reserved
            // immediate-operand+bit4 encoding (ArmUndefined - pipeline
            // flush into the Undefined exception, same 2S+1N as any
            // other exception entry).
            const bool immediateOffset = ((instruction >> 25) & 1u) == 0;
            const bool bit4 = ((instruction >> 4) & 1u) != 0;
            if (!immediateOffset && bit4) {
                return 3; // ArmUndefined
            }
            const bool load = ((instruction >> 20) & 1u) != 0;
            const u32 rd = (instruction >> 12) & 0xFu;
            if (!load) {
                return 2; // STR: 2N
            }
            return (rd == 15u) ? 5 : 3; // LDR: 1S+1N+1I, +1S+1N more if loaded into PC
        }

        case 0b10: {
            const bool branchBit = ((instruction >> 25) & 1u) != 0;
            if (branchBit) {
                return 3; // B/BL: pipeline flush, 2S+1N
            }
            // Block Data Transfer (LDM/STM).
            const bool load = ((instruction >> 20) & 1u) != 0;
            const u16 regList = static_cast<u16>(instruction & 0xFFFFu);
            const int n = PopCount16(regList);
            if (!load) {
                return (n - 1) + 2; // STM: (n-1)S+2N
            }
            const bool loadsPc = (regList & 0x8000u) != 0;
            return n + 2 + (loadsPc ? 2 : 0); // LDM: nS+1N+1I, +1S+1N more if r15 is loaded
        }

        case 0b11: {
            // SWI, or Coprocessor (unimplemented - ArmUndefined). Both
            // are a pipeline flush into an exception vector: 2S+1N.
            return 3;
        }
    }
    return 1; // unreachable
}

int Cpu::ComputeThumbCycles(u16 instruction) const {
    // Mirrors ExecuteThumb's dispatch order/bit patterns exactly.
    const u16 bits15_13 = static_cast<u16>((instruction >> 13) & 0x7u);
    const u16 bits15_12 = static_cast<u16>((instruction >> 12) & 0xFu);
    const u16 bits15_11 = static_cast<u16>((instruction >> 11) & 0x1Fu);
    const u16 bits15_10 = static_cast<u16>((instruction >> 10) & 0x3Fu);
    const u16 bits15_8  = static_cast<u16>((instruction >> 8) & 0xFFu);

    if (bits15_11 == 0b00011u) return 1;          // Format 2: Add/Subtract
    if (bits15_13 == 0b000u) return 1;            // Format 1: Move Shifted Register (immediate shift only)
    if (bits15_13 == 0b001u) return 1;            // Format 3: MOV/CMP/ADD/SUB Rd,#imm8

    if (bits15_10 == 0b010000u) {
        // Format 4: two-register ALU ops. All 1S except MUL (opcode
        // 1101), which is 1S+mI like ARM's plain MUL.
        const u16 opcode = (instruction >> 6) & 0xFu;
        if (opcode == 0b1101u) {
            const u32 rs = (instruction >> 3) & 0x7u;
            return 1 + MultiplyCyclesSigned(GetRegister(static_cast<int>(rs)));
        }
        return 1;
    }

    if (bits15_10 == 0b010001u) {
        // Format 5: Hi register ops / BX. Op field (bits9-8): 0=ADD,
        // 1=CMP, 2=MOV, 3=BX. BX is always a pipeline flush; ADD/MOV only
        // cost extra if they write r15 (H1 set and the 3-bit Rd field is
        // 111, i.e. full destination register 15); CMP never writes Rd.
        const u16 op = (instruction >> 8) & 0x3u;
        if (op == 0b11u) {
            return 3; // BX
        }
        if (op == 0b01u) {
            return 1; // CMP
        }
        const bool h1 = ((instruction >> 7) & 1u) != 0;
        const u16 rdLow = instruction & 0x7u;
        const bool writesPc = h1 && rdLow == 0x7u;
        return writesPc ? 3 : 1;
    }

    if (bits15_11 == 0b01001u) return 3;          // Format 6: PC-relative load (LDR: 1S+1N+1I)

    if (bits15_12 == 0b0101u) {
        // Format 7/8: register-offset load/store, plain or sign-extended.
        // Bit11 is the Load bit in both formats' encodings.
        const bool load = ((instruction >> 11) & 1u) != 0;
        return load ? 3 : 2;
    }

    if (bits15_13 == 0b011u) {
        // Format 9: LDR/STR{B} Rd,[Rb,#imm5].
        const bool load = ((instruction >> 11) & 1u) != 0;
        return load ? 3 : 2;
    }

    if (bits15_12 == 0b1000u) {
        // Format 10: LDRH/STRH Rd,[Rb,#imm5*2].
        const bool load = ((instruction >> 11) & 1u) != 0;
        return load ? 3 : 2;
    }

    if (bits15_12 == 0b1001u) {
        // Format 11: LDR/STR Rd,[SP,#imm8*4].
        const bool load = ((instruction >> 11) & 1u) != 0;
        return load ? 3 : 2;
    }

    if (bits15_12 == 0b1010u) return 1;           // Format 12: ADD Rd,PC/SP,#imm - pure arithmetic
    if (bits15_8 == 0b10110000u) return 1;        // Format 13: ADD SP,#imm

    if (bits15_12 == 0b1011u && ((instruction >> 9) & 0x3u) == 0b10u) {
        // Format 14: PUSH/POP. PUSH is STM-shaped, POP is LDM-shaped.
        const bool load = ((instruction >> 11) & 1u) != 0;
        const bool includeExtra = ((instruction >> 8) & 1u) != 0;
        const u8 regList = static_cast<u8>(instruction & 0xFFu);
        int n = PopCount16(regList);
        if (includeExtra) ++n;
        if (!load) {
            return (n - 1) + 2; // PUSH: (n-1)S+2N
        }
        // POP{PC} loads r15 - same +1S+1N pipeline-flush bonus as ARM LDM.
        return n + 2 + (includeExtra ? 2 : 0);
    }

    if (bits15_12 == 0b1100u) {
        // Format 15: STMIA/LDMIA Rb!,{rlist} - r0-r7 only, r15 never in
        // the list, so no pipeline-flush bonus is possible here.
        const bool load = ((instruction >> 11) & 1u) != 0;
        const u8 regList = static_cast<u8>(instruction & 0xFFu);
        const int n = PopCount16(regList);
        return load ? (n + 2) : ((n - 1) + 2);
    }

    if (bits15_8 == 0b11011111u) return 3;        // Format 17: SWI - pipeline flush

    if (bits15_12 == 0b1101u) {
        // Format 16: Bcc - taken costs 2S+1N, not-taken is just 1S (the
        // CPU discards it without ever reaching the branch target).
        // Thumb has no top-level condition gate like ARM's Step() does -
        // ThumbConditionalBranch checks its own condition internally -
        // so this reuses CheckCondition() by shifting the condition field
        // (bits11-8 here) up to where it expects it (bits31-28).
        return CheckCondition(static_cast<u32>(instruction) << 20) ? 3 : 1;
    }

    if (bits15_11 == 0b11100u) return 3;          // Format 18: unconditional B - always taken

    if (bits15_12 == 0b1111u) {
        // Format 19: BL, two-instruction pair. First half (sets LR) is
        // 1S; second half (writes PC) is the pipeline-flush 2S+1N.
        const bool isSecondHalf = (instruction & (1u << 11)) != 0;
        return isSecondHalf ? 3 : 1;
    }

    return 1; // unreachable
}

} // namespace gba
