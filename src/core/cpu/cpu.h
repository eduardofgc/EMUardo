#pragma once

#include <array>
#include <cstdint>
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

// ARM condition codes
enum class Cond : u32 {
    EQ = 0b0000, // Equal (Z=1)
    NE = 0b0001, // Not equal (Z=0)
    CS = 0b0010, // Carry set (C=1)
    CC = 0b0011, // Carry clear (C=0)
    MI = 0b0100, // Minus/negative (N=1)
    PL = 0b0101, // Plus/positive (N=0)
    VS = 0b0110, // Overflow set (V=1)
    VC = 0b0111, // Overflow clear (V=0)
    HI = 0b1000, // Unsigned higher (C=1 and Z=0)
    LS = 0b1001, // Unsigned lower/same (C=0 or Z=1)
    GE = 0b1010, // Signed >= (N=V)
    LT = 0b1011, // Signed < (N!=V)
    GT = 0b1100, // Signed > (Z=0 and N=V)
    LE = 0b1101, // Signed <= (Z=1 or N!=V)
    AL = 0b1110, // Always
    NV = 0b1111, // Never (deprecated, treat as AL)
};

class Cpu {
public:
    explicit Cpu(Bus& bus);

    void Reset();

    // Executes a single instruction and returns the number of cycles it took.
    int Step();

    u32 GetRegister(int index) const;
    void SetRegister(int index, u32 value);

    u32 Cpsr() const { return cpsr_; }
    bool GetFlag(Flag flag) const;
    void SetFlag(Flag flag, bool set);

    // For debugging: get banked register for a specific mode
    u32 GetBankedRegister(CpuMode mode, int reg) const;
    void SetBankedRegister(CpuMode mode, int reg, u32 value);

private:
    Bus& bus_;

    // r0-r15: r13 = SP, r14 = LR, r15 = PC by convention (unbanked view).
    std::array<u32, 16> registers_{};
    u32 cpsr_ = 0;
    u32 spsr_ = 0;

    // Banked registers for each mode (only r13/r14 banked, plus FIQ r8-r12)
    // We'll store them in a flat array: [FIQ_r8..r12, FIQ_r13, FIQ_r14, IRQ_r13, IRQ_r14, SVC_r13, SVC_r14, ABT_r13, ABT_r14, UND_r13, UND_r14]
    std::array<u32, 5 + 2*5> banked_registers_{}; // 5 FIQ + 2*5 for other modes

    // ARM instruction handlers
    int ExecuteArm(u32 opcode);
    int ExecuteThumb(u16 opcode);

    // Condition evaluation
    bool CheckCondition(Cond cond) const;

    // Data processing helpers
    u32 ApplyShift(u32 value, u32 shift_type, u32 shift_amount, bool& carry_out);
    void SetFlagsLogic(u32 result, bool carry);
    void SetFlagsArith(u32 result, u32 op1, u32 op2, bool is_sub, bool carry_in);

    // Memory access with proper alignment handling
    u32 Read32Aligned(u32 address);
    void Write32Aligned(u32 address, u32 value);

    // Fetch
    u32 FetchArm();
    u16 FetchThumb();

    // Exception handling
    void ExceptionEnter(CpuMode new_mode, u32 return_address, u32 cpsr);
    void ExceptionReturn();

    // Switch to a new CPU mode, banking registers
    void SetMode(CpuMode new_mode);
    CpuMode GetMode() const;
};

} // namespace gba