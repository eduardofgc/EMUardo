// Covers Cpu::Step()'s returned cycle count (see cycle_timing.cpp) across
// the main ARM and Thumb instruction categories: plain data-processing/ALU
// ops (1S), register-shifted data processing (+1I), memory loads/stores
// (LDR/STR-shaped costs), instructions that write PC (extra pipeline-flush
// cost), block transfers (cost scales with register count), branches/BX/
// SWI (flat pipeline-flush cost), a conditional Thumb branch both taken and
// not taken, and multiply (cost depends on the multiplier's magnitude).

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

#include "core/cpu/cpu.h"
#include "core/memory/bus.h"

namespace {

int failures = 0;

void Check(bool condition, const char* label) {
    if (!condition) {
        std::printf("FAIL: %s\n", label);
        ++failures;
    }
}

bool WriteTestRom(const char* path, const std::vector<std::uint32_t>& words) {
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    for (std::uint32_t word : words) {
        file.write(reinterpret_cast<const char*>(&word), sizeof(word));
    }
    return true;
}

// Runs a fresh ARM program (one word per instruction) and returns the
// cycle cost of each Step() call, in order.
std::vector<int> RunArm(const char* path, const std::vector<std::uint32_t>& words) {
    WriteTestRom(path, words);
    gba::Bus bus;
    bus.LoadRom(path);
    gba::Cpu cpu(bus);
    std::vector<int> costs;
    costs.reserve(words.size());
    for (std::size_t i = 0; i < words.size(); ++i) {
        costs.push_back(cpu.Step());
    }
    return costs;
}

} // namespace

int main() {
    // MOV R0,#5 (immediate operand2, no shift, Rd != PC): 1S = 1.
    {
        const auto costs = RunArm("/tmp/gba_cycle_arm_mov.bin", {0xE3A0'0005u});
        Check(costs[0] == 1, "ARM MOV Rd,#imm: cost 1");
    }

    // ADD R0,R1,R2,LSL R3 (register-specified shift amount): +1I = 2.
    // Encoding: cond=1110 00 0 0100 0 R1 R0 R3 0 00 1 R2
    {
        const auto costs = RunArm("/tmp/gba_cycle_arm_regshift.bin", {0xE081'0312u});
        Check(costs[0] == 2, "ARM ADD with register-specified shift: cost 2");
    }

    // MOV R15,R0 (data processing writing PC): 1 + 0 + 2 = 3.
    {
        const auto costs = RunArm("/tmp/gba_cycle_arm_movpc.bin", {0xE1A0'F000u});
        Check(costs[0] == 3, "ARM MOV Rd=PC: cost 3");
    }

    // LDR R0,[R1] (word load, not into PC): 1S+1N+1I = 3.
    {
        const auto costs = RunArm("/tmp/gba_cycle_arm_ldr.bin", {0xE591'0000u});
        Check(costs[0] == 3, "ARM LDR (not PC): cost 3");
    }

    // LDR R15,[R1] (word load into PC): 3 + 2 = 5.
    {
        const auto costs = RunArm("/tmp/gba_cycle_arm_ldrpc.bin", {0xE591'F000u});
        Check(costs[0] == 5, "ARM LDR into PC: cost 5");
    }

    // STR R0,[R1]: 2N = 2.
    {
        const auto costs = RunArm("/tmp/gba_cycle_arm_str.bin", {0xE581'0000u});
        Check(costs[0] == 2, "ARM STR: cost 2");
    }

    // STMIA R0!,{R1,R2,R3} (3 registers, no PC): (n-1)S+2N = 2+2 = 4.
    {
        const auto costs = RunArm("/tmp/gba_cycle_arm_stm.bin", {0xE8A0'000Eu});
        Check(costs[0] == 4, "ARM STM of 3 registers: cost 4");
    }

    // LDMIA R0!,{R1,R2,R3} (3 registers, no PC): nS+1N+1I = 3+2 = 5.
    {
        const auto costs = RunArm("/tmp/gba_cycle_arm_ldm.bin", {0xE8B0'000Eu});
        Check(costs[0] == 5, "ARM LDM of 3 registers: cost 5");
    }

    // B <somewhere> (always taken - ARM has no separate untaken-branch
    // path, a failed condition just returns 1 before Execute runs at all):
    // 2S+1N = 3.
    {
        const auto costs = RunArm("/tmp/gba_cycle_arm_b.bin", {0xEA00'0000u});
        Check(costs[0] == 3, "ARM B: cost 3");
    }

    // BX R0: 2S+1N = 3.
    {
        const auto costs = RunArm("/tmp/gba_cycle_arm_bx.bin", {0xE12F'FF10u});
        Check(costs[0] == 3, "ARM BX: cost 3");
    }

    // SWI: 2S+1N = 3, regardless of whether it's HLE'd.
    {
        const auto costs = RunArm("/tmp/gba_cycle_arm_swi.bin", {0xEF06'0000u}); // SWI 0x06 (Div), HLE'd
        Check(costs[0] == 3, "ARM SWI: cost 3");
    }

    // MUL R0,R1,R2 (Rd=R0,Rm=R1,Rs=R2): the cost depends on R2's (Rs's)
    // magnitude. R2 defaults to 0 at reset - top 24 bits all zero, m=1:
    // 1S + 1I = 2.
    {
        const auto costs = RunArm("/tmp/gba_cycle_arm_mul.bin", {0xE000'0291u}); // MUL R0,R1,R2
        Check(costs[0] == 2, "ARM MUL with small (zero) multiplier: cost 2");
    }

    // Same MUL, but R2 = 0x12000000 first (top byte 0x12 - not all-zero
    // or all-one at any of the three tested boundaries, so m=4):
    // 1S + 4I = 5.
    {
        const std::vector<std::uint32_t> words = {
            0xE3A0'2412u, // MOV R2, #0x12000000 (imm8=0x12, rotate=4 -> ROR 8)
            0xE000'0291u, // MUL R0,R1,R2
        };
        const auto costs = RunArm("/tmp/gba_cycle_arm_mul_big.bin", words);
        Check(costs[0] == 1, "ARM MOV R2,#imm: cost 1");
        Check(costs[1] == 5, "ARM MUL with large-magnitude multiplier: cost 5");
    }

    // --- Thumb ---

    // Every Thumb case below starts with "LDR R0,[PC,#0]; BX R0" to switch
    // CPU state (the CPU always resets into ARM state, so there's no way
    // to start a program directly in Thumb), then the Thumb instruction(s)
    // under test. R0,[PC,#0] with the LDR itself at offset 0 reads from
    // offset 0+8=8 (PC-as-base reads as instr_addr+8, offset 0 added) -
    // so the literal has to sit at offset 8, meaning BX must come right
    // after the LDR at offset 4, not after the literal.
    auto runThumb = [](const char* path, std::vector<std::uint16_t> thumbWords) -> std::vector<int> {
        std::vector<std::uint32_t> program;
        program.push_back(0xE59F'0000u); // LDR R0,[PC,#0] -> literal at offset 8
        program.push_back(0xE12F'FF10u); // BX R0
        const gba::u32 thumbEntry = gba::mem::kRomBase + 12u; // right after the 3-word ARM header
        program.push_back(thumbEntry | 1u); // literal: Thumb-state target (bit0 set)

        std::vector<std::uint8_t> bytes(program.size() * 4);
        for (std::size_t i = 0; i < program.size(); ++i) {
            bytes[i * 4 + 0] = static_cast<std::uint8_t>(program[i] & 0xFFu);
            bytes[i * 4 + 1] = static_cast<std::uint8_t>((program[i] >> 8) & 0xFFu);
            bytes[i * 4 + 2] = static_cast<std::uint8_t>((program[i] >> 16) & 0xFFu);
            bytes[i * 4 + 3] = static_cast<std::uint8_t>((program[i] >> 24) & 0xFFu);
        }
        for (std::uint16_t hw : thumbWords) {
            bytes.push_back(static_cast<std::uint8_t>(hw & 0xFFu));
            bytes.push_back(static_cast<std::uint8_t>((hw >> 8) & 0xFFu));
        }

        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        file.close();

        gba::Bus bus;
        bus.LoadRom(path);
        gba::Cpu cpu(bus);
        cpu.Step(); // LDR R0, literal
        cpu.Step(); // BX R0 (state switch - costs 3, not under test here)

        std::vector<int> costs;
        for (std::size_t i = 0; i < thumbWords.size(); ++i) {
            costs.push_back(cpu.Step());
        }
        return costs;
    };

    // MOV R0,#5 (Format 3): 1S = 1.
    {
        const auto costs = runThumb("/tmp/gba_cycle_thumb_mov2.bin", {0x2005u});
        Check(costs[0] == 1, "Thumb MOV Rd,#imm: cost 1");
    }

    // LDR R0,[R1,R2] (Format 7, load): 1S+1N+1I = 3.
    {
        const auto costs = runThumb("/tmp/gba_cycle_thumb_ldr.bin", {0x5888u});
        Check(costs[0] == 3, "Thumb LDR reg-offset: cost 3");
    }

    // STR R0,[R1,R2] (Format 7, store): 2N = 2.
    {
        const auto costs = runThumb("/tmp/gba_cycle_thumb_str.bin", {0x5088u});
        Check(costs[0] == 2, "Thumb STR reg-offset: cost 2");
    }

    // BX R0 (Format 5): 2S+1N = 3.
    {
        const auto costs = runThumb("/tmp/gba_cycle_thumb_bx.bin", {0x4700u});
        Check(costs[0] == 3, "Thumb BX: cost 3");
    }

    // SWI (Format 17): 2S+1N = 3.
    {
        const auto costs = runThumb("/tmp/gba_cycle_thumb_swi.bin", {0xDF06u}); // SWI 0x06 (Div)
        Check(costs[0] == 3, "Thumb SWI: cost 3");
    }

    // B (Format 18, unconditional, always taken): 2S+1N = 3.
    {
        const auto costs = runThumb("/tmp/gba_cycle_thumb_b.bin", {0xE000u});
        Check(costs[0] == 3, "Thumb unconditional B: cost 3");
    }

    // BEQ (Format 16): Z flag clear after the state-switch BX (R0 held a
    // nonzero address), so the branch is NOT taken here: 1S = 1.
    {
        const auto costs = runThumb("/tmp/gba_cycle_thumb_beq_nottaken.bin", {0xD000u});
        Check(costs[0] == 1, "Thumb Bcc not taken: cost 1");
    }

    // BAL (condition AL, always true regardless of flags): taken, 2S+1N = 3.
    {
        const auto costs = runThumb("/tmp/gba_cycle_thumb_beq_taken.bin", {0xDE00u});
        Check(costs[0] == 3, "Thumb Bcc (AL) taken: cost 3");
    }

    // PUSH {R0,R1,R2} (Format 14, store, 3 registers, no LR): (n-1)S+2N
    // = 2+2 = 4.
    {
        const auto costs = runThumb("/tmp/gba_cycle_thumb_push.bin", {0xB407u});
        Check(costs[0] == 4, "Thumb PUSH of 3 registers: cost 4");
    }

    // POP {R0,R1,R2} (Format 14, load, 3 registers, no PC): nS+1N+1I
    // = 3+2 = 5.
    {
        const auto costs = runThumb("/tmp/gba_cycle_thumb_pop.bin", {0xBC07u});
        Check(costs[0] == 5, "Thumb POP of 3 registers: cost 5");
    }

    // BL pair (Format 19): first half sets LR (1S), second half writes PC
    // (2S+1N = 3).
    {
        const auto costs = runThumb("/tmp/gba_cycle_thumb_bl.bin", {0xF000u, 0xF800u});
        Check(costs[0] == 1, "Thumb BL first half: cost 1");
        Check(costs[1] == 3, "Thumb BL second half: cost 3");
    }

    if (failures == 0) {
        std::printf("PASS: ARM/Thumb per-instruction cycle costs across all major categories\n");
    }
    return failures;
}
