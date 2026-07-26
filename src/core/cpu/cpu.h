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

// Skeleton for the ARM7TDMI interpreter core. The real work here is:
//   1. Fetch/decode/execute for the 32-bit ARM instruction set
//   2. Fetch/decode/execute for the 16-bit Thumb instruction set
//   3. Banked registers per CPU mode (FIQ/IRQ/SVC/ABT/UND each bank r13/r14,
//      FIQ additionally banks r8-r12)
//   4. Pipeline timing (fetch/decode/execute overlap affects PC-relative
///     addressing - PC reads as current instruction + 8 in ARM state)
class Cpu {
public:
    explicit Cpu(Bus& bus);

    void Reset();

    // Executes a single instruction and returns the number of cycles it
    // took. The main loop drives the whole system off of cycle counts so
    // CPU/PPU/APU/timers can be kept in lockstep.
    int Step();

    u32 GetRegister(int index) const;
    void SetRegister(int index, u32 value);

    u32 Cpsr() const { return cpsr_; }
    bool GetFlag(Flag flag) const;
    void SetFlag(Flag flag, bool set);

private:
    Bus& bus_;

    // r0-r15: r13 = SP, r14 = LR, r15 = PC by convention (unbanked view).
    std::array<u32, 16> registers_{};
    u32 cpsr_ = 0;
    u32 spsr_ = 0;

    // TODO: banked register sets per mode, ARM decode table, Thumb decode
    // table, pipeline state (fetched/decoded instruction latches).

    u32 FetchArm();
    u16 FetchThumb();
};

} // namespace gba
