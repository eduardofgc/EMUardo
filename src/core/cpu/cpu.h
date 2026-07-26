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

// ARM7TDMI interpreter core. Handles the 32-bit ARM instruction set;
// Thumb (16-bit) decode is a separate, later milestone - Step() already
// branches on the T flag so that work slots in without touching this file.
//
// A note on the PC/pipeline simplification used throughout this file: real
// hardware pipelines fetch/decode/execute, so reading r15 mid-instruction
// gives "address of this instruction + 8" in ARM state. We don't model the
// pipeline - Step() just does fetch-then-immediately-increment, so by the
// time an instruction executes, registers_[15] already holds
// "address of this instruction + 4". Anywhere GBATEK says an instruction
// reads r15 as PC+8 (or +12 for register-specified shift amounts), we add
// the extra +4 (or +8) explicitly at the point of use - look for comments
// referencing this paragraph.
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
};

} // namespace gba
