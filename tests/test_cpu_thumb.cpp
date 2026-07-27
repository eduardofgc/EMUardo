// Smoke test for the Thumb decoder. The program starts in ARM state (as
// Cpu::Reset() always does), executes a small ARM preamble that switches
// into Thumb state via BX, then runs a short Thumb program covering
// Format 3 (immediate MOV/CMP), Format 2 (immediate ADD), and Format 16
// (Bcc) - specifically checking that a taken branch correctly skips the
// instruction in between.
//
// Program under test (byte offsets from ROM start):
//   ARM preamble (mode switch):
//     0x00  0xE3A00001   MOV  R0, #1
//     0x04  0xE08F0000   ADD  R0, PC, R0     ; R0 = (addr of Thumb code) | 1
//     0x08  0xE12FFF10   BX   R0             ; enter Thumb state
//   Thumb program (starts at offset 0x0C):
//     0x0C  0x2005       MOV  R0, #5
//     0x0E  0x1CC1       ADD  R1, R0, #3
//     0x10  0x2805       CMP  R0, #5          ; sets Z since R0 == 5
//     0x12  0xD000       BEQ  +0              ; taken -> skips the next instruction
//     0x14  0x2263       MOV  R2, #99         ; must NOT execute
//     0x16  0x2201       MOV  R2, #1          ; must execute

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

#include "core/cpu/cpu.h"
#include "core/memory/bus.h"

namespace {

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

void PushHalf(std::vector<uint8_t>& bytes, uint16_t half) {
    bytes.push_back(static_cast<uint8_t>(half & 0xFF));
    bytes.push_back(static_cast<uint8_t>((half >> 8) & 0xFF));
}

} // namespace

int main() {
    const char* path = "/tmp/gba_cpu_thumb_test_rom.bin";

    std::vector<uint8_t> program;
    PushWord(program, 0xE3A00001u); // MOV  R0, #1
    PushWord(program, 0xE08F0000u); // ADD  R0, PC, R0
    PushWord(program, 0xE12FFF10u); // BX   R0
    PushHalf(program, 0x2005u);     // MOV  R0, #5
    PushHalf(program, 0x1CC1u);     // ADD  R1, R0, #3
    PushHalf(program, 0x2805u);     // CMP  R0, #5
    PushHalf(program, 0xD000u);     // BEQ  +0
    PushHalf(program, 0x2263u);     // MOV  R2, #99  (must be skipped)
    PushHalf(program, 0x2201u);     // MOV  R2, #1   (must execute)

    if (!WriteTestRom(path, program)) {
        std::printf("FAIL: could not write test ROM to %s\n", path);
        return 1;
    }

    gba::Bus bus;
    if (!bus.LoadRom(path)) {
        std::printf("FAIL: Bus::LoadRom rejected the test ROM\n");
        return 1;
    }
    gba::Cpu cpu(bus);

    // 3 ARM steps (mode switch) + MOV/ADD/CMP (3 Thumb steps) = 6 steps to
    // land right after CMP, where we check Z before anything else can
    // clobber it - the later MOV R2,#1 also updates Z/N (Format 3 MOV
    // always does), so checking flags only at the very end would give a
    // false read of the wrong instruction's result.
    for (int i = 0; i < 6; ++i) {
        cpu.Step();
    }

    int failures = 0;

    if (!cpu.GetFlag(gba::Flag::T)) {
        std::printf("FAIL: expected T flag set (Thumb state) after BX\n");
        ++failures;
    }
    if (cpu.GetRegister(0) != 5) {
        std::printf("FAIL: R0 expected 5, got %u\n", cpu.GetRegister(0));
        ++failures;
    }
    if (cpu.GetRegister(1) != 8) {
        std::printf("FAIL: R1 expected 8, got %u\n", cpu.GetRegister(1));
        ++failures;
    }
    if (!cpu.GetFlag(gba::Flag::Z)) {
        std::printf("FAIL: Z flag expected set after CMP R0,#5\n");
        ++failures;
    }

    // Now run the BEQ and the two instructions after it, to confirm the
    // branch actually skipped MOV R2,#99 rather than falling through to it.
    for (int i = 0; i < 2; ++i) {
        cpu.Step();
    }

    if (cpu.GetRegister(2) != 1) {
        std::printf("FAIL: R2 expected 1 (BEQ should have skipped MOV R2,#99), got %u\n",
                     cpu.GetRegister(2));
        ++failures;
    }

    if (failures == 0) {
        std::printf("PASS: ARM->Thumb switch, MOV/ADD/CMP/BEQ Thumb smoke test\n");
    }
    return failures;
}
