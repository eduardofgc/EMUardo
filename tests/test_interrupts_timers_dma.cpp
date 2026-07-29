// Covers the three pieces added this milestone: the CPU's IRQ entry
// (specifically the LR_irq return-address convention real handlers rely
// on), timer overflow generating an interrupt, and DMA immediate-mode
// transfers actually moving memory.

#include <cstdio>
#include <cstdint>
#include <fstream>
#include <vector>

#include "core/cpu/cpu.h"
#include "core/io/dma.h"
#include "core/io/timers.h"
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

bool WriteTestRom(const char* path, const std::vector<uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return true;
}

void PushWord(std::vector<uint8_t>& bytes, uint32_t word) {
    bytes.push_back(static_cast<uint8_t>(word & 0xFF));
    bytes.push_back(static_cast<uint8_t>((word >> 8) & 0xFF));
    bytes.push_back(static_cast<uint8_t>((word >> 16) & 0xFF));
    bytes.push_back(static_cast<uint8_t>((word >> 24) & 0xFF));
}

// ---------------------------------------------------------------------
// Test 1: IRQ entry sets LR_irq to (interrupted instruction's address) + 4,
// matching the "SUBS PC,LR,#4" convention every real GBA IRQ handler uses.
// Program: MSR CPSR_c,#0x1F (unmask IRQs) at 0x08000000, then nothing else
// executes - the interrupt fires on the *next* Step() before any fetch
// happens at 0x08000004.
// ---------------------------------------------------------------------
void TestIrqReturnAddress() {
    const char* path = "/tmp/gba_irq_test_rom.bin";
    std::vector<uint8_t> program;
    PushWord(program, 0xE321F01Fu); // MSR CPSR_c, #0x1F  (mode=System, I=0, F=0)
    if (!WriteTestRom(path, program)) {
        std::printf("FAIL: could not write IRQ test ROM\n");
        ++failures;
        return;
    }

    gba::Bus bus;
    if (!bus.LoadRom(path)) {
        std::printf("FAIL: Bus::LoadRom rejected the IRQ test ROM\n");
        ++failures;
        return;
    }
    gba::Cpu cpu(bus);

    cpu.Step(); // executes MSR, unmasking IRQs; PC becomes 0x08000004

    const gba::u32 pcBeforeInterrupt = cpu.GetRegister(15);
    Check(pcBeforeInterrupt == 0x0800'0004u, "IRQ test: PC is 0x08000004 before the interrupt fires");

    // Arm a pending, enabled VBlank interrupt. Note: IF can't be *set* via
    // a plain Write16 (see Bus::Write8's write-1-to-clear special case for
    // IF) - only Bus::RequestInterrupt() can set a bit, mirroring how only
    // hardware (not software) can raise a real GBA interrupt flag.
    bus.Write16(gba::mem::kIoBase + gba::io::kIe, gba::irq::kVBlank);
    bus.RequestInterrupt(gba::irq::kVBlank);
    bus.Write16(gba::mem::kIoBase + gba::io::kIme, 1);

    cpu.Step(); // should take the IRQ instead of fetching at 0x08000004

    Check(cpu.GetMode() == gba::CpuMode::IRQ, "IRQ test: CPU entered IRQ mode");
    Check(!cpu.GetFlag(gba::Flag::T), "IRQ test: CPU is in ARM state after exception entry");
    Check(cpu.GetFlag(gba::Flag::I), "IRQ test: IRQs are disabled inside the handler");
    Check(cpu.GetRegister(15) == 0x0000'0018u, "IRQ test: PC is at the IRQ vector (0x18)");
    Check(cpu.GetRegister(14) == pcBeforeInterrupt + 4,
          "IRQ test: LR_irq is (interrupted PC) + 4, so SUBS PC,LR,#4 returns correctly");
}

// ---------------------------------------------------------------------
// Test 2: Timer 0 configured to overflow after 2 ticks fires irq::kTimer0
// into IF, and reloads from the value it was armed with.
// ---------------------------------------------------------------------
void TestTimerOverflow() {
    gba::Bus bus;
    gba::Timers timers(bus);

    const gba::u16 reload = 0xFFFEu; // two ticks from wrapping past 0xFFFF
    bus.Write16(gba::mem::kIoBase + gba::io::kTm0CntL, reload);
    // bit7 = start, bit6 = IRQ enable, bits1-0 = prescaler select (00 = /1)
    bus.Write16(gba::mem::kIoBase + gba::io::kTm0CntH, (1u << 7) | (1u << 6));

    timers.Tick(1); // 0xFFFE -> 0xFFFF, no overflow yet
    Check((bus.Read16(gba::mem::kIoBase + gba::io::kIf) & gba::irq::kTimer0) == 0,
          "Timer test: no IRQ requested before overflow");

    timers.Tick(1); // 0xFFFF -> wraps to 0x0000: overflow, reload, IRQ
    Check((bus.Read16(gba::mem::kIoBase + gba::io::kIf) & gba::irq::kTimer0) != 0,
          "Timer test: Timer0 overflow set the IF bit");
    Check(bus.Read16(gba::mem::kIoBase + gba::io::kTm0CntL) == reload,
          "Timer test: counter reloaded to the armed value after overflow");
}

// ---------------------------------------------------------------------
// Test 3: DMA0 configured for an immediate word-transfer copies 4 words
// from one EWRAM location to another, then clears its own enable bit
// (non-repeat mode).
// ---------------------------------------------------------------------
void TestDmaImmediateTransfer() {
    gba::Bus bus;
    gba::Dma dma(bus);

    const gba::u32 src = gba::mem::kEwramBase + 0x100;
    const gba::u32 dst = gba::mem::kEwramBase + 0x200;
    for (gba::u32 i = 0; i < 4; ++i) {
        bus.Write32(src + i * 4, 0xCAFE'0000u + i);
    }

    bus.Write32(gba::mem::kIoBase + gba::io::kDma0Sad, src);
    bus.Write32(gba::mem::kIoBase + gba::io::kDma0Dad, dst);
    bus.Write16(gba::mem::kIoBase + gba::io::kDma0CntL, 4);
    // bit15=enable, bit10=32-bit transfer, bits13-12=00(immediate),
    // bits8-7=00(source increment), bits6-5=00(dest increment)
    bus.Write16(gba::mem::kIoBase + gba::io::kDma0CntH, (1u << 15) | (1u << 10));

    dma.CheckImmediate();

    for (gba::u32 i = 0; i < 4; ++i) {
        char label[64];
        std::snprintf(label, sizeof(label), "DMA test: word %u copied correctly", i);
        Check(bus.Read32(dst + i * 4) == 0xCAFE'0000u + i, label);
    }

    const gba::u16 controlAfter = bus.Read16(gba::mem::kIoBase + gba::io::kDma0CntH);
    Check((controlAfter & (1u << 15)) == 0,
          "DMA test: non-repeat transfer cleared its own enable bit");
}

} // namespace

int main() {
    TestIrqReturnAddress();
    TestTimerOverflow();
    TestDmaImmediateTransfer();

    if (failures == 0) {
        std::printf("PASS: IRQ return address, timer overflow, DMA immediate transfer\n");
    }
    return failures;
}
