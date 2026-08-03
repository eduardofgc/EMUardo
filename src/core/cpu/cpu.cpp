#include "core/cpu/cpu.h"

namespace gba {

Cpu::Cpu(Bus& bus) : bus_(bus) {
    Reset();
}

void Cpu::Reset() {
    registers_.fill(0);

    // On real hardware the boot process runs a bit of BIOS code that ends
    // up jumping to 0x0800'0000 (start of cartridge ROM) in ARM state,
    // System mode, with IRQ/FIQ disabled. We fast-forward directly here for
    // now; a "skip BIOS" boot path is standard even in mature emulators.
    registers_[15] = mem::kRomBase;
    cpsr_ = static_cast<u32>(CpuMode::System) |
            static_cast<u32>(Flag::I) |
            static_cast<u32>(Flag::F);
    spsr_ = 0;
}

int Cpu::Step() {
    // TODO: check T flag to select ARM vs Thumb fetch/decode/execute.
    // Placeholder: fetch and discard, advance PC by one ARM instruction
    // width so the loop is at least well-defined before decode exists.
    FetchArm();
    registers_[15] += 4;
    return 1;
}

u32 Cpu::FetchArm() {
    return bus_.Read32(registers_[15]);
}

u16 Cpu::FetchThumb() {
    return bus_.Read16(registers_[15]);
}

u32 Cpu::GetRegister(int index) const {
    return registers_[static_cast<std::size_t>(index)];
}

void Cpu::SetRegister(int index, u32 value) {
    registers_[static_cast<std::size_t>(index)] = value;
}

bool Cpu::GetFlag(Flag flag) const {
    return (cpsr_ & static_cast<u32>(flag)) != 0;
}

void Cpu::SetFlag(Flag flag, bool set) {
    if (set) {
        cpsr_ |= static_cast<u32>(flag);
    } else {
        cpsr_ &= ~static_cast<u32>(flag);
    }
}

} // namespace gba
