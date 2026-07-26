// Smoke test for the ARM decoder: hand-assembled instruction words (values
// you can verify against any ARM disassembler) loaded directly into a Bus,
// then checked against expected register/flag state after each Step().
//
// Program under test:
//   0xE3A00005   MOV   R0, #5
//   0xE2801003   ADD   R1, R0, #3
//   0xE3500005   CMP   R0, #5        ; sets Z since R0 == 5
//   0x03A02001   MOVEQ R2, #1        ; executes because Z is set

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

#include "core/cpu/cpu.h"
#include "core/memory/bus.h"

namespace {

bool WriteTestRom(const char* path, const std::vector<uint32_t>& words) {
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    for (uint32_t word : words) {
        file.write(reinterpret_cast<const char*>(&word), sizeof(word));
    }
    return true;
}

} // namespace

int main() {
    const char* path = "/tmp/gba_cpu_test_rom.bin";
    const std::vector<uint32_t> program = {
        0xE3A00005u, // MOV   R0, #5
        0xE2801003u, // ADD   R1, R0, #3
        0xE3500005u, // CMP   R0, #5
        0x03A02001u, // MOVEQ R2, #1
    };

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

    for (int i = 0; i < 4; ++i) {
        cpu.Step();
    }

    int failures = 0;

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
    if (cpu.GetRegister(2) != 1) {
        std::printf("FAIL: R2 expected 1 (MOVEQ should have executed), got %u\n",
                     cpu.GetRegister(2));
        ++failures;
    }

    if (failures == 0) {
        std::printf("PASS: MOV/ADD/CMP/MOVEQ immediate data-processing smoke test\n");
    }
    return failures;
}
