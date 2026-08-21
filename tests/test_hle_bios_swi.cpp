// Tests the HLE SWI calls directly (see Cpu::TryHleSwi): Halt actually
// stops the CPU from executing until an interrupt arrives, Div computes
// correct signed quotient/remainder/abs-quotient, and CpuSet performs a
// real memory copy through the Bus.

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

// ---------------------------------------------------------------------
// Test 1: SWI 0x02 (Halt) stops the CPU from making further progress
// until an interrupt is requested, then it resumes normally.
// ---------------------------------------------------------------------
void TestHalt() {
    const char* path = "/tmp/gba_hle_halt_test_rom.bin";
    std::vector<std::uint8_t> program;
    PushWord(program, 0xEF02'0000u); // SWI 0x02 (Halt)
    PushWord(program, 0xE3A0'0063u); // MOV R0, #99 - should only run after waking
    if (!WriteTestRom(path, program)) {
        std::printf("FAIL: could not write Halt test ROM\n");
        ++failures;
        return;
    }

    gba::Bus bus;
    bus.LoadRom(path);
    gba::Cpu cpu(bus);

    cpu.Step(); // executes SWI 0x02 -> halts
    Check(cpu.GetRegister(0) != 99, "Halt test: R0 unchanged immediately after Halt");

    // Step several more times with no interrupt pending - should make no
    // progress at all.
    for (int i = 0; i < 5; ++i) {
        cpu.Step();
    }
    Check(cpu.GetRegister(0) != 99, "Halt test: still halted after several Steps with nothing pending");

    // Now request an (enabled) interrupt - Halt should release even
    // though IME is still 0, per GBATEK's Halt-exit rule.
    bus.Write16(gba::mem::kIoBase + gba::io::kIe, gba::irq::kVBlank);
    bus.RequestInterrupt(gba::irq::kVBlank);

    cpu.Step(); // should wake and execute MOV R0,#99
    Check(cpu.GetRegister(0) == 99, "Halt test: resumed and executed the next instruction after an interrupt arrived");
}

// ---------------------------------------------------------------------
// Test 2: SWI 0x06 (Div) - signed division semantics.
// ---------------------------------------------------------------------
void TestDiv() {
    gba::Bus bus;
    gba::Cpu cpu(bus);
    // Div doesn't need a real ROM - call the HLE path directly by driving
    // register state and a synthetic SWI. Simplest: build a tiny ROM with
    // just the SWI instruction and preload R0/R1 via SetRegister isn't
    // exposed publicly, so instead load values with MOV/MVN before the
    // SWI, matching how a real caller would set up arguments.
    const char* path = "/tmp/gba_hle_div_test_rom.bin";
    std::vector<std::uint8_t> program;
    // R0 = -7 via MVN R0,#6 (bitwise-not 6 = -7 two's complement)
    PushWord(program, 0xE3E0'0006u); // MVN R0, #6
    PushWord(program, 0xE3A0'1002u); // MOV R1, #2
    PushWord(program, 0xEF06'0000u); // SWI 0x06 (Div): R0=-7/2, R1=-7%2
    if (!WriteTestRom(path, program)) {
        std::printf("FAIL: could not write Div test ROM\n");
        ++failures;
        return;
    }
    bus.LoadRom(path);

    cpu.Step(); // MVN R0,#6 -> R0 = -7
    Check(static_cast<gba::s32>(cpu.GetRegister(0)) == -7, "Div test: R0 set up as -7");
    cpu.Step(); // MOV R1,#2
    cpu.Step(); // SWI Div

    Check(static_cast<gba::s32>(cpu.GetRegister(0)) == -3, "Div test: -7/2 truncates toward zero to -3");
    Check(static_cast<gba::s32>(cpu.GetRegister(1)) == -1, "Div test: -7%2 == -1 (remainder takes dividend's sign)");
    Check(cpu.GetRegister(3) == 3, "Div test: R3 holds abs(quotient) == 3");
}

// ---------------------------------------------------------------------
// Test 3: SWI 0x0B (CpuSet) - word copy through the Bus.
// ---------------------------------------------------------------------
void TestCpuSet() {
    gba::Bus bus;

    const gba::u32 src = gba::mem::kEwramBase + 0x100u;
    const gba::u32 dst = gba::mem::kEwramBase + 0x200u;
    for (gba::u32 i = 0; i < 4; ++i) {
        bus.Write32(src + i * 4, 0xBEEF'0000u + i);
    }

    const char* path = "/tmp/gba_hle_cpuset_test_rom.bin";
    std::vector<std::uint8_t> program;
    // R0=src, R1=dst, R2=control (count=4, 32-bit transfer, not fixed-source).
    // Loading arbitrary 32-bit addresses needs LDR-from-literal-pool since
    // MOV immediate can't encode them; the control word 0x04000004 also
    // can't be a single MOV immediate (its two set bits, 26 and 2, are 9
    // bit-positions apart cyclically - too wide for ARM's 8-bit-rotated
    // immediate encoding), so it's built via MOV + ORR instead.
    PushWord(program, 0xE59F'000Cu); // LDR R0, [PC, #0x0C]  -> src literal at 0x14
    PushWord(program, 0xE59F'100Cu); // LDR R1, [PC, #0x0C]  -> dst literal at 0x18
    PushWord(program, 0xE3A0'2004u); // MOV R2, #4                 (count = 4)
    PushWord(program, 0xE382'2301u); // ORR R2, R2, #0x04000000    (word-transfer bit)
    PushWord(program, 0xEF0B'0000u); // SWI 0x0B (CpuSet)
    PushWord(program, src);
    PushWord(program, dst);
    if (!WriteTestRom(path, program)) {
        std::printf("FAIL: could not write CpuSet test ROM\n");
        ++failures;
        return;
    }
    bus.LoadRom(path);
    gba::Cpu cpu(bus);

    cpu.Step(); // LDR R0, src
    cpu.Step(); // LDR R1, dst
    cpu.Step(); // MOV R2, #4
    cpu.Step(); // ORR R2, R2, #0x04000000
    Check(cpu.GetRegister(0) == src, "CpuSet test: R0 loaded correctly");
    Check(cpu.GetRegister(1) == dst, "CpuSet test: R1 loaded correctly");
    Check(cpu.GetRegister(2) == 0x0400'0004u, "CpuSet test: R2 control word is word-transfer, count=4");
    cpu.Step(); // SWI CpuSet

    for (gba::u32 i = 0; i < 4; ++i) {
        char label[64];
        std::snprintf(label, sizeof(label), "CpuSet test: word %u copied correctly", i);
        Check(bus.Read32(dst + i * 4) == 0xBEEF'0000u + i, label);
    }
}

// ---------------------------------------------------------------------
// Test 4: SWI 0x12 (LZ77UnCompVRAM) - literal bytes plus an overlapping
// back-reference (the classic LZ77 self-referential-copy case, where the
// copy source runs into bytes the same copy already wrote).
// ---------------------------------------------------------------------
void TestLz77UnComp() {
    gba::Bus bus;

    const char* path = "/tmp/gba_hle_lz77_test_rom.bin";
    std::vector<std::uint8_t> program;
    const gba::u32 dst = gba::mem::kEwramBase + 0x300u;

    PushWord(program, 0xE59F'0004u); // LDR R0, [PC, #4]  -> src literal at offset 12
    PushWord(program, 0xE59F'1004u); // LDR R1, [PC, #4]  -> dst literal at offset 16
    PushWord(program, 0xEF12'0000u); // SWI 0x12 (LZ77UnCompVRAM)
    PushWord(program, gba::mem::kRomBase + 20u); // src literal: compressed data starts right after this literal pool
    PushWord(program, dst);          // dst literal

    // Decompresses to 8 bytes: 4 literals (0x10,0x20,0x30,0x40) followed
    // by a length-4/disp-2 back-reference, which - copied one byte at a
    // time - produces 0x30,0x40,0x30,0x40 by reading bytes the same copy
    // just wrote. See hle_bios.cpp's HleLz77UnComp comment for the format.
    PushWord(program, (8u << 8) | 0x10u); // header: size=8, type=1 (LZ77)
    program.push_back(0x08u); // flags: bits7-3 = 0,0,0,0,1 (4 literals then 1 compressed unit)
    program.push_back(0x10u); // literal
    program.push_back(0x20u); // literal
    program.push_back(0x30u); // literal
    program.push_back(0x40u); // literal
    program.push_back(0x10u); // compressed byte0: length nibble=1 (length=4), disp hi=0
    program.push_back(0x01u); // compressed byte1: disp lo=1 (disp=2)

    if (!WriteTestRom(path, program)) {
        std::printf("FAIL: could not write LZ77 test ROM\n");
        ++failures;
        return;
    }
    bus.LoadRom(path);
    gba::Cpu cpu(bus);

    cpu.Step(); // LDR R0, src
    cpu.Step(); // LDR R1, dst
    Check(cpu.GetRegister(0) == gba::mem::kRomBase + 20u, "LZ77 test: R0 (src) loaded correctly");
    Check(cpu.GetRegister(1) == dst, "LZ77 test: R1 (dst) loaded correctly");
    cpu.Step(); // SWI LZ77UnCompVRAM

    static constexpr gba::u8 expected[8] = {0x10, 0x20, 0x30, 0x40, 0x30, 0x40, 0x30, 0x40};
    for (gba::u32 i = 0; i < 8; ++i) {
        char label[80];
        std::snprintf(label, sizeof(label), "LZ77 test: decompressed byte %u correct", i);
        Check(bus.Read8(dst + i) == expected[i], label);
    }
}

// ---------------------------------------------------------------------
// Test 5: SWI 0x15 (RLUnCompVRAM) - a verbatim-copy block followed by a
// run-length block, covering both flag-byte forms.
// ---------------------------------------------------------------------
void TestRlUnComp() {
    gba::Bus bus;

    const char* path = "/tmp/gba_hle_rl_test_rom.bin";
    std::vector<std::uint8_t> program;
    const gba::u32 dst = gba::mem::kEwramBase + 0x400u;

    PushWord(program, 0xE59F'0004u); // LDR R0, [PC, #4]  -> src literal at offset 12
    PushWord(program, 0xE59F'1004u); // LDR R1, [PC, #4]  -> dst literal at offset 16
    PushWord(program, 0xEF15'0000u); // SWI 0x15 (RLUnCompVRAM)
    PushWord(program, gba::mem::kRomBase + 20u); // src: compressed data starts right after this literal pool
    PushWord(program, dst);

    // Decompresses to 3+5=8 bytes: a verbatim block of 3 bytes, then a
    // run-length block repeating one byte 5 times.
    PushWord(program, (8u << 8) | 0x30u); // header: size=8, type=3 (RL)
    program.push_back(0x02u); // flag: bit7=0 (verbatim), length = 2+1 = 3
    program.push_back(0xAAu);
    program.push_back(0xBBu);
    program.push_back(0xCCu);
    program.push_back(0x82u); // flag: bit7=1 (run), length = (2&0x7F)+3 = 5
    program.push_back(0xEEu); // repeated value

    if (!WriteTestRom(path, program)) {
        std::printf("FAIL: could not write RLUnComp test ROM\n");
        ++failures;
        return;
    }
    bus.LoadRom(path);
    gba::Cpu cpu(bus);

    cpu.Step(); // LDR R0, src
    cpu.Step(); // LDR R1, dst
    cpu.Step(); // SWI RLUnCompVRAM

    static constexpr gba::u8 expected[8] = {0xAA, 0xBB, 0xCC, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
    for (gba::u32 i = 0; i < 8; ++i) {
        char label[80];
        std::snprintf(label, sizeof(label), "RLUnComp test: decompressed byte %u correct", i);
        Check(bus.Read8(dst + i) == expected[i], label);
    }
}

// ---------------------------------------------------------------------
// Test 6: SWI 0x16 (Diff8bitUnFilterWRAM) - cumulative running sum with
// 8-bit wraparound.
// ---------------------------------------------------------------------
void TestDiff8bitUnFilter() {
    gba::Bus bus;

    const char* path = "/tmp/gba_hle_diff8_test_rom.bin";
    std::vector<std::uint8_t> program;
    const gba::u32 dst = gba::mem::kEwramBase + 0x500u;

    PushWord(program, 0xE59F'0004u);
    PushWord(program, 0xE59F'1004u);
    PushWord(program, 0xEF16'0000u); // SWI 0x16 (Diff8bitUnFilterWRAM)
    PushWord(program, gba::mem::kRomBase + 20u);
    PushWord(program, dst);

    PushWord(program, (4u << 8) | 0x10u); // header: size=4 bytes, type=1
    program.push_back(0x05u); // running = 0x05
    program.push_back(0x03u); // running = 0x08
    program.push_back(0xFEu); // running = 0x08 + 0xFE = 0x06 (wraps mod 256)
    program.push_back(0x01u); // running = 0x07

    if (!WriteTestRom(path, program)) {
        std::printf("FAIL: could not write Diff8bitUnFilter test ROM\n");
        ++failures;
        return;
    }
    bus.LoadRom(path);
    gba::Cpu cpu(bus);

    cpu.Step();
    cpu.Step();
    cpu.Step(); // SWI Diff8bitUnFilterWRAM

    static constexpr gba::u8 expected[4] = {0x05, 0x08, 0x06, 0x07};
    for (gba::u32 i = 0; i < 4; ++i) {
        char label[80];
        std::snprintf(label, sizeof(label), "Diff8bitUnFilter test: byte %u correct", i);
        Check(bus.Read8(dst + i) == expected[i], label);
    }
}

// ---------------------------------------------------------------------
// Test 7: SWI 0x18 (Diff16bitUnFilter) - same idea, 16-bit units.
// ---------------------------------------------------------------------
void TestDiff16bitUnFilter() {
    gba::Bus bus;

    const char* path = "/tmp/gba_hle_diff16_test_rom.bin";
    std::vector<std::uint8_t> program;
    const gba::u32 dst = gba::mem::kEwramBase + 0x600u;

    PushWord(program, 0xE59F'0004u);
    PushWord(program, 0xE59F'1004u);
    PushWord(program, 0xEF18'0000u); // SWI 0x18 (Diff16bitUnFilter)
    PushWord(program, gba::mem::kRomBase + 20u);
    PushWord(program, dst);

    PushWord(program, (4u << 8) | 0x20u); // header: size=4 bytes (2 halfwords), type=2
    program.push_back(0x00u); program.push_back(0x01u); // 0x0100 -> running = 0x0100
    program.push_back(0x10u); program.push_back(0xFFu); // 0xFF10 -> running = 0x0100+0xFF10 = 0x10 (wraps mod 65536)

    if (!WriteTestRom(path, program)) {
        std::printf("FAIL: could not write Diff16bitUnFilter test ROM\n");
        ++failures;
        return;
    }
    bus.LoadRom(path);
    gba::Cpu cpu(bus);

    cpu.Step();
    cpu.Step();
    cpu.Step(); // SWI Diff16bitUnFilter

    Check(bus.Read16(dst + 0) == 0x0100u, "Diff16bitUnFilter test: halfword 0 correct");
    Check(bus.Read16(dst + 2) == 0x0010u, "Diff16bitUnFilter test: halfword 1 wraps correctly");
}

} // namespace

int main() {
    TestHalt();
    TestDiv();
    TestCpuSet();
    TestLz77UnComp();
    TestRlUnComp();
    TestDiff8bitUnFilter();
    TestDiff16bitUnFilter();

    if (failures == 0) {
        std::printf("PASS: HLE Halt, Div, CpuSet, LZ77UnComp, RLUnComp, and Diff8/16bitUnFilter\n");
    }
    return failures;
}
