#pragma once

#include <array>

#include "core/memory/bus.h"
#include "core/types.h"

namespace gba {

// CPSR condition/control flags (GBATEK "CPSR and SPSR").
enum class Flag : u32 {
    N = 1u << 31, // Negative
    Z = 1u << 30, // Zero
    C = 1u << 29, // Carry
    V = 1u << 28, // Overflow
    I = 1u << 7,  // IRQ disable
    F = 1u << 6,  // FIQ disable
    T = 1u << 5,  // Thumb state
};

enum class CpuMode : u32 {
    User       = 0b10000,
    FIQ        = 0b10001,
    IRQ        = 0b10010,
    Supervisor = 0b10011,
    Abort      = 0b10111,
    Undefined  = 0b11011,
    System     = 0b11111,
};

enum class ShiftType : u32 {
    LSL = 0,
    LSR = 1,
    ASR = 2,
    ROR = 3,
};

// ARM7TDMI interpreter core. Handles both the 32-bit ARM instruction set
// and the 16-bit Thumb instruction set - Step() branches on the T flag to
// pick which decoder runs.
//
// A note on the PC/pipeline simplification used throughout this file: real
// hardware pipelines fetch/decode/execute, so reading r15 mid-instruction
// gives "address of this instruction + 8" in ARM state (+4 in Thumb state).
// We don't model the pipeline - Step() just does fetch-then-immediately-
// increment, so by the time an instruction executes, registers_[15]
// already holds "address of this instruction + 4" (ARM) or "+ 2" (Thumb).
// Anywhere GBATEK says an instruction reads r15 with the pipeline offset
// (PC+8/PC+12 in ARM, PC+4 in Thumb), we add the remaining difference
// explicitly at the point of use - look for comments referencing this
// paragraph.
class Cpu {
public:
    explicit Cpu(Bus& bus);

    void Reset();

    // Executes a single instruction and returns the number of cycles it
    // took. Cycle counts are currently placeholders (always 1) - see
    // GBATEK's instruction timing tables for the real values, that's a
    // later accuracy pass once something is actually running.
    int Step();

    u32 GetRegister(int index) const;
    void SetRegister(int index, u32 value);

    u32 Cpsr() const { return cpsr_; }
    void SetCpsr(u32 value);
    bool GetFlag(Flag flag) const;
    void SetFlag(Flag flag, bool set);

    CpuMode GetMode() const { return static_cast<CpuMode>(cpsr_ & 0x1Fu); }

private:
    Bus& bus_;

    std::array<u32, 16> registers_{}; // active/visible register file
    u32 cpsr_ = 0;

    // --- Banked register storage -------------------------------------
    // Only FIQ banks r8-r12; every other mode shares one set for those.
    std::array<u32, 5> r8_12_fiq_{};
    std::array<u32, 5> r8_12_other_{};
    // r13/r14 are banked per mode: index 0=User/System, 1=FIQ, 2=IRQ,
    // 3=Supervisor, 4=Abort, 5=Undefined.
    std::array<u32, 6> r13_banked_{};
    std::array<u32, 6> r14_banked_{};
    // SPSR exists for every mode except User/System.
    // Index: 0=FIQ, 1=IRQ, 2=Supervisor, 3=Abort, 4=Undefined.
    std::array<u32, 5> spsr_banked_{};

    static int R13R14BankIndex(CpuMode mode);
    static int SpsrBankIndex(CpuMode mode); // returns -1 for User/System

    void SwitchMode(CpuMode newMode);
    u32 Spsr() const;
    void SetSpsr(u32 value);

    bool CheckCondition(u32 instruction) const;

    u32 FetchArm();
    u16 FetchThumb();

    void ExecuteArm(u32 instruction);
    void ExecuteThumb(u16 instruction);

    // --- Barrel shifter -------------------------------------------------
    // General shift engine shared by data-processing operand2 and the
    // register-offset form of single data transfer. `isImmediateShift`
    // selects between the two different "amount == 0" rules ARM defines
    // (see GBATEK "Shifter Operand" / ARM ARM section on the barrel
    // shifter) - immediate shifts of 0 have per-type special meanings
    // (e.g. ROR#0 means RRX), register-specified shifts of 0 mean "no
    // shift, flags unchanged" unconditionally.
    u32 Shift(ShiftType type, u32 value, u32 amount, bool& carryInOut,
              bool isImmediateShift) const;

    u32 GetImmediateOperand2(u32 instruction, bool& carryOut) const;
    u32 GetRegisterOperand2(u32 instruction, bool& carryOut) const;

    // --- Instruction class handlers --------------------------------------
    void ArmDataProcessing(u32 instruction);
    void ArmMultiply(u32 instruction);
    void ArmMultiplyLong(u32 instruction);
    void ArmSingleDataSwap(u32 instruction);
    void ArmBranchExchange(u32 instruction);
    void ArmHalfwordTransfer(u32 instruction);
    void ArmSingleDataTransfer(u32 instruction);
    void ArmBlockDataTransfer(u32 instruction);
    void ArmBranch(u32 instruction);
    void ArmSoftwareInterrupt(u32 instruction);
    void ArmUndefined(u32 instruction);
    void ArmMrs(u32 instruction);
    void ArmMsrRegister(u32 instruction);
    void ArmMsrImmediate(u32 instruction);

    u32 ApplyPsrFieldMask(u32 current, u32 value, u32 fieldMask) const;

    // Generic exception entry: banks LR/SPSR, switches mode, masks
    // interrupts, forces ARM state, and jumps to the vector address.
    void EnterException(CpuMode mode, u32 vectorAddress);

    // --- Thumb instruction set --------------------------------------------
    // Named after the 19 instruction "formats" in the ARM7TDMI Technical
    // Reference Manual's Thumb chapter - each format is a distinct 16-bit
    // encoding shape, not a single instruction. Several formats cover
    // multiple related mnemonics (e.g. Format 4 covers AND/EOR/LSL/.../MVN).
    void ThumbMoveShiftedRegister(u16 instruction);   // Format 1: LSL/LSR/ASR Rd, Rs, #imm5
    void ThumbAddSubtract(u16 instruction);           // Format 2: ADD/SUB Rd, Rs, Rn/#imm3
    void ThumbImmediateOp(u16 instruction);           // Format 3: MOV/CMP/ADD/SUB Rd, #imm8
    void ThumbAluOp(u16 instruction);                 // Format 4: two-register ALU ops
    void ThumbHiRegOpsBranchExchange(u16 instruction); // Format 5: ADD/CMP/MOV on r8-r15, BX
    void ThumbPcRelativeLoad(u16 instruction);        // Format 6: LDR Rd, [PC, #imm8*4]
    void ThumbLoadStoreRegOffset(u16 instruction);    // Format 7: LDR/STR{B} Rd, [Rb, Ro]
    void ThumbLoadStoreSignExtended(u16 instruction); // Format 8: LDRH/STRH/LDSB/LDSH Rd, [Rb, Ro]
    void ThumbLoadStoreImmOffset(u16 instruction);    // Format 9: LDR/STR{B} Rd, [Rb, #imm5]
    void ThumbLoadStoreHalfword(u16 instruction);     // Format 10: LDRH/STRH Rd, [Rb, #imm5*2]
    void ThumbSpRelativeLoadStore(u16 instruction);   // Format 11: LDR/STR Rd, [SP, #imm8*4]
    void ThumbLoadAddress(u16 instruction);           // Format 12: ADD Rd, PC/SP, #imm8*4
    void ThumbAddOffsetToSp(u16 instruction);         // Format 13: ADD SP, #imm7*4 (signed)
    void ThumbPushPopRegisters(u16 instruction);      // Format 14: PUSH/POP {rlist}{LR/PC}
    void ThumbMultipleLoadStore(u16 instruction);     // Format 15: STMIA/LDMIA Rb!, {rlist}
    void ThumbConditionalBranch(u16 instruction);     // Format 16: Bcc label
    void ThumbSoftwareInterrupt(u16 instruction);     // Format 17: SWI #imm8
    void ThumbUnconditionalBranch(u16 instruction);   // Format 18: B label
    void ThumbLongBranchLink(u16 instruction);        // Format 19: BL label (two-instruction pair)
};

} // namespace gba
