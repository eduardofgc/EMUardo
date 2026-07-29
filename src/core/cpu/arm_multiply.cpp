#include "core/cpu/cpu.h"

namespace gba {


void Cpu::ArmMultiply(u32 instruction) {
    const bool accumulate = ((instruction >> 21) & 1u) != 0;
    const bool setFlags = ((instruction >> 20) & 1u) != 0;
    const u32 rd = (instruction >> 16) & 0xFu;
    const u32 rn = (instruction >> 12) & 0xFu;
    const u32 rs = (instruction >> 8) & 0xFu;
    const u32 rm = instruction & 0xFu;

    u32 result = GetRegister(static_cast<int>(rm)) * GetRegister(static_cast<int>(rs));
    if (accumulate) {
        result += GetRegister(static_cast<int>(rn));
    }
    SetRegister(static_cast<int>(rd), result);

    if (setFlags) {
        SetFlag(Flag::Z, result == 0);
        SetFlag(Flag::N, (result & 0x8000'0000u) != 0);
    }
}

void Cpu::ArmMultiplyLong(u32 instruction) {
    const bool isSigned = ((instruction >> 22) & 1u) != 0;
    const bool accumulate = ((instruction >> 21) & 1u) != 0;
    const bool setFlags = ((instruction >> 20) & 1u) != 0;
    const u32 rdHi = (instruction >> 16) & 0xFu;
    const u32 rdLo = (instruction >> 12) & 0xFu;
    const u32 rs = (instruction >> 8) & 0xFu;
    const u32 rm = instruction & 0xFu;

    u64 result;
    if (isSigned) {
        const s64 product = static_cast<s64>(static_cast<s32>(GetRegister(static_cast<int>(rm)))) *
                             static_cast<s64>(static_cast<s32>(GetRegister(static_cast<int>(rs))));
        result = static_cast<u64>(product);
    } else {
        result = static_cast<u64>(GetRegister(static_cast<int>(rm))) *
                 static_cast<u64>(GetRegister(static_cast<int>(rs)));
    }

    if (accumulate) {
        const u64 acc = (static_cast<u64>(GetRegister(static_cast<int>(rdHi))) << 32) |
                         static_cast<u64>(GetRegister(static_cast<int>(rdLo)));
        result += acc;
    }

    SetRegister(static_cast<int>(rdLo), static_cast<u32>(result & 0xFFFF'FFFFull));
    SetRegister(static_cast<int>(rdHi), static_cast<u32>(result >> 32));

    if (setFlags) {
        SetFlag(Flag::Z, result == 0);
        SetFlag(Flag::N, ((result >> 63) & 1u) != 0);
    }
}

} // namespace gba
