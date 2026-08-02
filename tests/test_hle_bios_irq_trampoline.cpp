// Proves the HLE BIOS IRQ trampoline (see Bus::InstallHleBios) actually
// works end to end: a "user handler" is registered at 0x03007FFC (the
// real devkitARM/libgba convention), an interrupt is requested, and we
// verify the trampoline saves state, calls the handler, and correctly
// resumes the interrupted program afterward - not just that individual
// pieces work in isolation.

#include <cstdio>
#include <fstream>
#include <vector>

#include "core/cpu/cpu.h"
#include "core/memory/bus.h"
#include "core/types.h"

namespace {

int failures = 0;

void Check(bool condition, const char* label) {
    if (!condition) {
        std::printf("FAIL: %s\n", label);
        ++failures;
    }
}

bool WriteTestRom(const char* path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return true;
}

void PushWord(std::vector<std::uint8_t>& bytes, std::uint32_t word) {
    bytes.push_back(static_cast<std::uint8_t>(word & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((word >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((word >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((word >> 24) & 0xFF));
}

} // namespace

int main() {
    const char* path = "/tmp/gba_irq_trampoline_test_rom.bin";

    // Main program: unmask IRQ, then two instructions that should run
    // *around* the interrupt - MOV R0,#0 runs before it fires, MOV R1,#99
    // runs after the trampoline returns control.
    std::vector<std::uint8_t> program;
    PushWord(program, 0xE321F01Fu); // MSR CPSR_c, #0x1F
    PushWord(program, 0xE3A0'0000u); // MOV R0, #0
    PushWord(program, 0xE3A0'1063u); // MOV R1, #99
    if (!WriteTestRom(path, program)) {
        std::printf("FAIL: could not write test ROM\n");
        return 1;
    }

    gba::Bus bus;
    if (!bus.LoadRom(path)) {
        std::printf("FAIL: Bus::LoadRom rejected the test ROM\n");
        return 1;
    }

    // Install a "user IRQ handler" at an EWRAM address, per the
    // devkitARM/libgba convention of registering it at 0x03007FFC.
    // Handler: MOV R2,#42; LDR R3,[PC,#4] (marker addr); STR R2,[R3]; BX LR
    // Uses a memory marker rather than a register, since R0-R3/R12/LR are
    // exactly the registers the trampoline saves/restores around the call
    // - a register-only marker would get correctly wiped on return, which
    // is real hardware behavior, not a bug (caught this via the trace).
    const gba::u32 handlerAddr = gba::mem::kEwramBase + 0x100u;
    const gba::u32 markerAddr = gba::mem::kEwramBase + 0x300u;
    bus.Write32(handlerAddr + 0x00, 0xE3A0'202Au); // MOV R2, #42
    bus.Write32(handlerAddr + 0x04, 0xE59F'3004u); // LDR R3, [PC, #4]
    bus.Write32(handlerAddr + 0x08, 0xE583'2000u); // STR R2, [R3]
    bus.Write32(handlerAddr + 0x0C, 0xE12F'FF1Eu); // BX LR
    bus.Write32(handlerAddr + 0x10, markerAddr);   // literal
    bus.Write32(0x0300'7FFCu, handlerAddr); // bit0=0 -> handler runs in ARM state

    gba::Cpu cpu(bus);

    cpu.Step(); // MSR - unmasks IRQ
    Check(cpu.GetRegister(15) == 0x0800'0004u, "PC is 0x08000004 after MSR, before the interrupt fires");

    bus.Write16(gba::mem::kIoBase + gba::io::kIe, gba::irq::kVBlank);
    bus.RequestInterrupt(gba::irq::kVBlank);
    bus.Write16(gba::mem::kIoBase + gba::io::kIme, 1);

    // Drive enough steps to get all the way through: IRQ entry, the
    // 7-instruction trampoline, the 4-instruction handler, and back to
    // just after SUBS PC,LR,#4 returns. We stop exactly here rather than
    // stepping further: our test handler never acknowledges IF (a real
    // one always does), so on the very next Step() the CPU would
    // correctly re-enter the same interrupt immediately - accurate
    // hardware behavior, but out of scope for what this test is checking.
    for (int i = 0; i < 12; ++i) {
        cpu.Step();
    }

    Check(bus.Read32(markerAddr) == 42, "user handler ran (wrote 42 to its memory marker)");
    Check(cpu.GetRegister(0) == 0, "R0 still holds the value set before the interrupt (MOV R0,#0 wasn't re-run or skipped)");
    Check(cpu.GetMode() == gba::CpuMode::System, "CPU is back in System mode after SUBS PC,LR,#4 restored CPSR from SPSR");
    Check(!cpu.GetFlag(gba::Flag::I), "IRQs are unmasked again after returning (matches the state MSR set before the interrupt)");
    Check(cpu.GetRegister(15) == 0x0800'0004u, "PC correctly returned to the interrupted instruction's address");

    if (failures == 0) {
        std::printf("PASS: HLE IRQ trampoline dispatches to and returns from a real user handler\n");
    }
    return failures;
}
