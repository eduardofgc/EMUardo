#include "core/cpu/cpu.h"

#include <cstdint>
#include <iostream>

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
    
    // Clear banked registers
    banked_registers_.fill(0);
}

int Cpu::Step() {
    // Check the T flag to determine if we are in ARM or Thumb state.
    if (GetFlag(Flag::T)) {
        // Thumb state
        u16 opcode = FetchThumb();
        return ExecuteThumb(opcode);
    } else {
        // ARM state
        u32 opcode = FetchArm();
        return ExecuteArm(opcode);
    }
}

u32 Cpu::FetchArm() {
    // Fetch instruction at current PC (PC is already the address of the instruction)
    return bus_.Read32(registers_[15]);
}

u16 Cpu::FetchThumb() {
    return bus_.Read16(registers_[15]);
}

u32 Cpu::GetRegister(int index) const {
    // Handle banked registers based on current mode
    if (index >= 8 && index <= 12) {
        // FIQ banks r8-r12
        if (GetMode() == CpuMode::FIQ) {
            return banked_registers_[index - 8]; // FIQ r8-r12 at indices 0-4
        }
    } else if (index == 13 || index == 14) {
        // SP (r13) and LR (r14) are banked in all modes except User/System
        CpuMode mode = GetMode();
        if (mode != CpuMode::User && mode != CpuMode::System) {
            switch (mode) {
                case CpuMode::FIQ: return banked_registers_[5 + (index - 13)]; // FIQ r13/r14 at 5,6
                case CpuMode::IRQ: return banked_registers_[7 + (index - 13)]; // IRQ r13/r14 at 7,8
                case CpuMode::Supervisor: return banked_registers_[9 + (index - 13)]; // SVC r13/r14 at 9,10
                case CpuMode::Abort: return banked_registers_[11 + (index - 13)]; // ABT r13/r14 at 11,12
                case CpuMode::Undefined: return banked_registers_[13 + (index - 13)]; // UND r13/r14 at 13,14
                default: break;
            }
        }
    }
    return registers_[static_cast<std::size_t>(index)];
}

void Cpu::SetRegister(int index, u32 value) {
    // Handle banked registers based on current mode
    if (index >= 8 && index <= 12) {
        // FIQ banks r8-r12
        if (GetMode() == CpuMode::FIQ) {
            banked_registers_[index - 8] = value; // FIQ r8-r12 at indices 0-4
            return;
        }
    } else if (index == 13 || index == 14) {
        // SP (r13) and LR (r14) are banked in all modes except User/System
        CpuMode mode = GetMode();
        if (mode != CpuMode::User && mode != CpuMode::System) {
            switch (mode) {
                case CpuMode::FIQ: banked_registers_[5 + (index - 13)] = value; break; // FIQ r13/r14
                case CpuMode::IRQ: banked_registers_[7 + (index - 13)] = value; break; // IRQ r13/r14
                case CpuMode::Supervisor: banked_registers_[9 + (index - 13)] = value; break; // SVC r13/r14
                case CpuMode::Abort: banked_registers_[11 + (index - 13)] = value; break; // ABT r13/r14
                case CpuMode::Undefined: banked_registers_[13 + (index - 13)] = value; break; // UND r13/r14
                default: break;
            }
            return;
        }
    }
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

CpuMode Cpu::GetMode() const {
    return static_cast<CpuMode>(cpsr_ & 0x1F);
}

void Cpu::SetMode(CpuMode new_mode) {
    // Save current mode's banked registers
    CpuMode old_mode = GetMode();
    if (old_mode != CpuMode::User && old_mode != CpuMode::System) {
        // Save SP and LR
        if (old_mode == CpuMode::FIQ) {
            banked_registers_[5] = GetRegister(13); // FIQ r13
            banked_registers_[6] = GetRegister(14); // FIQ r14
        } else if (old_mode == CpuMode::IRQ) {
            banked_registers_[7] = GetRegister(13); // IRQ r13
            banked_registers_[8] = GetRegister(14); // IRQ r14
        } else if (old_mode == CpuMode::Supervisor) {
            banked_registers_[9] = GetRegister(13); // SVC r13
            banked_registers_[10] = GetRegister(14); // SVC r14
        } else if (old_mode == CpuMode::Abort) {
            banked_registers_[11] = GetRegister(13); // ABT r13
            banked_registers_[12] = GetRegister(14); // ABT r14
        } else if (old_mode == CpuMode::Undefined) {
            banked_registers_[13] = GetRegister(13); // UND r13
            banked_registers_[14] = GetRegister(14); // UND r14
        }
    }
    
    // Set new mode
    cpsr_ = (cpsr_ & ~0x1Fu) | static_cast<u32>(new_mode);
    
    // Load new mode's banked registers
    if (new_mode != CpuMode::User && new_mode != CpuMode::System) {
        if (new_mode == CpuMode::FIQ) {
            SetRegister(13, banked_registers_[5]); // FIQ r13
            SetRegister(14, banked_registers_[6]); // FIQ r14
            // Load FIQ r8-r12
            for (int i = 8; i <= 12; ++i) {
                SetRegister(i, banked_registers_[i - 8]);
            }
        } else if (new_mode == CpuMode::IRQ) {
            SetRegister(13, banked_registers_[7]); // IRQ r13
            SetRegister(14, banked_registers_[8]); // IRQ r14
        } else if (new_mode == CpuMode::Supervisor) {
            SetRegister(13, banked_registers_[9]); // SVC r13
            SetRegister(14, banked_registers_[10]); // SVC r14
        } else if (new_mode == CpuMode::Abort) {
            SetRegister(13, banked_registers_[11]); // ABT r13
            SetRegister(14, banked_registers_[12]); // ABT r14
        } else if (new_mode == CpuMode::Undefined) {
            SetRegister(13, banked_registers_[13]); // UND r13
            SetRegister(14, banked_registers_[14]); // UND r14
        }
    }
}

// Condition code evaluation
bool Cpu::CheckCondition(Cond cond) const {
    bool z = GetFlag(Flag::Z);
    bool n = GetFlag(Flag::N);
    bool c = GetFlag(Flag::C);
    bool v = GetFlag(Flag::V);

    switch (cond) {
        case Cond::EQ: return z;
        case Cond::NE: return !z;
        case Cond::CS: return c;
        case Cond::CC: return !c;
        case Cond::MI: return n;
        case Cond::PL: return !n;
        case Cond::VS: return v;
        case Cond::VC: return !v;
        case Cond::HI: return c && !z;
        case Cond::LS: return !c || z;
        case Cond::GE: return n == v;
        case Cond::LT: return n != v;
        case Cond::GT: return !z && (n == v);
        case Cond::LE: return z || (n != v);
        case Cond::AL: return true;
        case Cond::NV: return false;
        default: return false;
    }
}

// Apply shift operation
u32 Cpu::ApplyShift(u32 value, u32 shift_type, u32 shift_amount, bool& carry_out) {
    carry_out = GetFlag(Flag::C); // Default carry out is current carry flag
    
    if (shift_amount == 0) {
        return value;
    }
    
    switch (shift_type) {
        case 0b00: // LSL logical left shift
            if (shift_amount >= 32) {
                carry_out = (value != 0) && ((shift_amount == 32) ? true : false);
                return 0;
            }
            carry_out = (value >> (32 - shift_amount)) & 1;
            return value << shift_amount;
        case 0b01: // LSR logical right shift
            if (shift_amount >= 32) {
                carry_out = (value != 0) && ((shift_amount == 32) ? (value & 1) : false);
                return 0;
            }
            carry_out = (value >> (shift_amount - 1)) & 1;
            return value >> shift_amount;
        case 0b10: // ASR arithmetic right shift
            {
                bool sign = (value >> 31) & 1;
                if (shift_amount >= 32) {
                    carry_out = sign ? 1 : 0;
                    return sign ? 0xFFFFFFFF : 0;
                }
                carry_out = (value >> (shift_amount - 1)) & 1;
                u32 result = value >> shift_amount;
                // Sign extend
                if (sign && (shift_amount < 32)) {
                    result |= (~0u << (32 - shift_amount));
                }
                return result;
            }
        case 0b11: // ROR rotate right
            {
                shift_amount &= 31; // wrap around
                if (shift_amount == 0) {
                    // RRX (rotate right extended) - bit 0 <- carry, bits 31:1 <- value[30:0]
                    carry_out = value & 1;
                    return (GetFlag(Flag::C) ? 0x80000000 : 0) | (value >> 1);
                }
                carry_out = (value >> (shift_amount - 1)) & 1;
                return (value >> shift_amount) | (value << (32 - shift_amount));
            }
        default:
            return value;
    }
}

// Set flags for logical operations (AND, EOR, ORR, MOV, etc.)
void Cpu::SetFlagsLogic(u32 result, bool carry) {
    SetFlag(Flag::N, (result & 0x80000000) != 0);
    SetFlag(Flag::Z, result == 0);
    SetFlag(Flag::C, carry);
    // V flag unchanged for logical operations
}

// Set flags for arithmetic operations (ADD, SUB, etc.)
void Cpu::SetFlagsArith(u32 result, u32 op1, u32 op2, bool is_sub, bool /*carry_in*/) {
    SetFlag(Flag::N, (result & 0x80000000) != 0);
    SetFlag(Flag::Z, result == 0);
    
    // Carry flag
    if (is_sub) {
        // For subtraction: carry = NOT borrow
        SetFlag(Flag::C, (op1 >= op2));
    } else {
        // For addition: carry = overflow beyond 32 bits
        SetFlag(Flag::C, (op1 > (0xFFFFFFFF - op2)));
    }
    
    // Overflow flag
    bool op1_sign = (op1 >> 31) & 1;
    bool op2_sign = (op2 >> 31) & 1;
    bool result_sign = (result >> 31) & 1;
    
    if (is_sub) {
        // Overflow if: op1_sign != op2_sign and result_sign != op1_sign
        SetFlag(Flag::V, (op1_sign != op2_sign) && (result_sign != op1_sign));
    } else {
        // Overflow if: op1_sign == op2_sign and result_sign != op1_sign
        SetFlag(Flag::V, (op1_sign == op2_sign) && (result_sign != op1_sign));
    }
}

// Memory access helpers
u32 Cpu::Read32Aligned(u32 address) {
    // Ensure word alignment
    address &= ~0x3u;
    return bus_.Read32(address);
}

void Cpu::Write32Aligned(u32 address, u32 value) {
    // Ensure word alignment
    address &= ~0x3u;
    bus_.Write32(address, value);
}

// Exception handling
void Cpu::ExceptionEnter(CpuMode new_mode, u32 return_address, u32 cpsr_value) {
    // Save old CPSR to SPSR of new mode
    spsr_ = cpsr_value;
    
    // Switch to new mode
    SetMode(new_mode);
    
    // Set return address in LR of new mode
    SetRegister(14, return_address);
    
    // Set CPSR to new mode with interrupts disabled as appropriate
    cpsr_ = (cpsr_ & ~0x1Fu) | static_cast<u32>(new_mode);
    // Note: Interrupt handling would be added here based on exception type
}

void Cpu::ExceptionReturn() {
    // Restore CPSR from SPSR
    cpsr_ = spsr_;
    
    // Return from exception by restoring PC from LR
    // Mode switching back will happen in SetMode when we restore user mode
    // For simplicity, we'll just set PC and let SetMode handle register restoration
    u32 lr = GetRegister(14);
    SetRegister(15, lr + 4); // Adjust for pipeline
}

// Main ARM instruction execution
int Cpu::ExecuteArm(u32 opcode) {
    // Extract condition
    Cond cond = static_cast<Cond>((opcode >> 28) & 0xF);
    if (!CheckCondition(cond)) {
        // Condition failed, instruction does not execute but still takes 1S cycle
        // Advance PC normally
        SetRegister(15, GetRegister(15) + 4);
        return 1;
    }

    // Extract opcode bits (bits 24-27)
    u32 opcode_4bit = (opcode >> 24) & 0xF;
    // Extract S bit (bit 20)
    bool set_flags = (opcode & (1u << 20)) != 0;
    // Extract Rn, Rd, Rs, Rm (for data processing with register operand)
    u32 rn = (opcode >> 16) & 0xF;
    u32 rd = (opcode >> 12) & 0xF;
    u32 rs = (opcode >> 8) & 0xF;  // unused for shift amount in this implementation
    u32 rm = opcode & 0xF;

    // Decide instruction type based on opcode_4bit and other bits
    // Data processing: opcode_4bit = 0x0-0x6 (with I=0) or 0xE (with I=1 for MVN/MOV etc)
    // Actually, let's check bit 25 and bit 4-7 to distinguish instruction types
    
    // Check if this is a data processing instruction (bits 27-23 = 000x0 or 000x1 where x is opcode_4bit)
    if (((opcode >> 23) & 0xF) == 0b0000 || ((opcode >> 23) & 0xF) == 0b0001) {
        // Data processing instruction
        bool is_immediate = (opcode & (1u << 25)) != 0; // I bit
        u32 operand2;
        bool carry_out = false;
        
        if (is_immediate) {
            // Immediate value: rotate right by 2 * imm8
            u32 imm8 = opcode & 0xFF;
            u32 rot_imm = (opcode >> 8) & 0xF;
            u32 rotate_amount = rot_imm * 2;
            operand2 = (imm8 >> rotate_amount) | (imm8 << (32 - rotate_amount));
            // Carry out is bit 31 of the immediate if rotate_amount > 0
            carry_out = (rotate_amount > 0) ? ((imm8 >> (32 - rotate_amount)) & 1) : GetFlag(Flag::C);
        } else {
            // Register operand with optional shift
            u32 rm_val = GetRegister(rm);
            u32 shift_amount = 0;
            bool reg_shift = false;
            
            // Check for shift amount in rs (bits 8-11)
            if ((opcode & (0xFu << 8)) != 0) {
                // Register shift
                reg_shift = true;
                shift_amount = GetRegister((opcode >> 8) & 0xF) & 0xFF; // only low byte used
            } else {
                // Immediate shift
                shift_amount = (opcode >> 7) & 0x1F;
            }
            
            u32 shift_type = (opcode >> 5) & 0x3;
            operand2 = ApplyShift(rm_val, shift_type, shift_amount, carry_out);
        }
        
        u32 operand1 = GetRegister(rn);
        u32 result = 0;
        bool write_result = true; // by default we write the result to Rd, except for TST, TEQ, CMP, CMN
        
        switch (opcode_4bit) {
            case 0b0000: // AND
                result = operand1 & operand2;
                break;
            case 0b0001: // EOR
                result = operand1 ^ operand2;
                break;
            case 0b0010: // SUB
                result = operand1 - operand2;
                break;
            case 0b0011: // RSB
                result = operand2 - operand1;
                break;
            case 0b0100: // ADD
                result = operand1 + operand2;
                break;
            case 0b0101: // ADC
                result = operand1 + operand2 + (GetFlag(Flag::C) ? 1 : 0);
                break;
            case 0b0110: // SBC
                result = operand1 - operand2 - (GetFlag(Flag::C) ? 0 : 1);
                break;
            case 0b0111: // RSC
                result = operand2 - operand1 - (GetFlag(Flag::C) ? 0 : 1);
                break;
            case 0b1000: // TST
                result = operand1 & operand2;
                write_result = false;
                break;
            case 0b1001: // TEQ
                result = operand1 ^ operand2;
                write_result = false;
                break;
            case 0b1010: // CMP
                result = operand1 - operand2;
                write_result = false;
                break;
            case 0b1011: // CMN
                result = operand1 + operand2;
                write_result = false;
                break;
            case 0b1100: // ORR
                result = operand1 | operand2;
                break;
            case 0b1101: // MOV
                result = operand2;
                break;
            case 0b1110: // BIC
                result = operand1 & ~operand2;
                break;
            case 0b1111: // MVN
                result = ~operand2;
                break;
            default:
                // Unknown opcode, treat as NOP
                write_result = false;
                break;
        }

        if (write_result && rd != 15) { // Do not write to PC (R15) for now
            SetRegister(rd, result);
        }

        // Update condition codes if S bit is set
        if (set_flags) {
            if (opcode_4bit >= 0b0000 && opcode_4bit <= 0b0001) { // AND/EOR - logical
                SetFlagsLogic(result, carry_out);
            } else if (opcode_4bit >= 0b0010 && opcode_4bit <= 0b0111) { // SUB/RSB/ADD/ADC/SBC/RSC - arithmetic
                bool is_sub = (opcode_4bit == 0b0010 || opcode_4bit == 0b0011 || 
                            opcode_4bit == 0b0110 || opcode_4bit == 0b0111);
                SetFlagsArith(result, operand1, operand2, is_sub, false);
            } else if (opcode_4bit == 0b1000 || opcode_4bit == 0b1001) { // TST/TEQ - logical
                SetFlagsLogic(result, carry_out);
            } else if (opcode_4bit == 0b1010 || opcode_4bit == 0b1011) { // CMP/CMN - arithmetic
                bool is_sub = (opcode_4bit == 0b1010);
                SetFlagsArith(result, operand1, operand2, is_sub, false);
            } else if (opcode_4bit == 0b1100) { // ORR - logical
                SetFlagsLogic(result, carry_out);
            } else if (opcode_4bit == 0b1101) { // MOV - logical
                SetFlagsLogic(result, carry_out);
            } else if (opcode_4bit == 0b1110) { // BIC - logical
                SetFlagsLogic(result, carry_out);
            } else if (opcode_4bit == 0b1111) { // MVN - logical
                SetFlagsLogic(result, carry_out);
            }
        }

        // Handle PC write (for branches)
        if (rd == 15 && write_result) {
            // If writing to PC, we need to pipeline flush
            SetRegister(15, result);
        } else {
            // Normal instruction: advance PC
            SetRegister(15, GetRegister(15) + 4);
        }
        
        return 1;
    }
    // Branch instructions (opcode_4bit = 0b101xx)
    else if ((opcode_4bit & 0xE) == 0b010) { // B, BL
        bool link = (opcode & (1u << 24)) != 0;
        // Sign-extend 24-bit offset
        int32_t offset = static_cast<int32_t>(opcode & 0xFFFFFF);
        if (offset & 0x800000) {
            offset |= 0xFF000000;
        }
        offset <<= 2; // Shift left 2 (word aligned)
        
        if (link) {
            SetRegister(14, GetRegister(15) + 4); // Store return address in LR
        }
        
        // Branch to target
        SetRegister(15, GetRegister(15) + offset + 4); // +4 for pipeline
        return 1;
    }
    // Load/Store instructions (opcode_4bit = 0b01xxxx)
    else if ((opcode_4bit & 0xC) == 0x4) { // LDR/STR
        bool is_word = !(opcode & (1u << 22)); // B byte vs W word
        bool is_load = !(opcode & (1u << 20)); // L load vs S store
        bool is_pre_index = (opcode & (1u << 24)) != 0; // P pre-index
        bool is_up = (opcode & (1u << 23)) != 0; // U up (add) vs down (sub)
        bool is_offset = (opcode & (1u << 21)) != 0; // O offset
        
        u32 base = GetRegister(rn);
        u32 offset = 0;
        bool offset_is_reg = false;
        
        if (is_offset) {
            // Offset is register
            offset_is_reg = true;
            offset = GetRegister(opcode & 0xF);
        } else {
            // Offset is immediate 12-bit
            offset = opcode & 0xFFF;
        }
        
        // Apply offset
        u32 address;
        if (is_up) {
            address = base + offset;
        } else {
            address = base - offset;
        }
        
        // Determine actual address to use
        u32 mem_addr = is_pre_index ? address : base;
        
        if (is_load) {
            // Load operation
            u32 value = 0;
            if (is_word) {
                value = bus_.Read32(mem_addr);
            } else {
                // Byte load - need to check if signed (LDRSB) but we'll treat as unsigned for now
                value = bus_.Read8(mem_addr);
            }
            bool write_result = true; // Initialize write_result for load operations
            if (write_result && rd != 15) {
                SetRegister(rd, value);
            }
        } else {
            // Store operation
            u32 value = GetRegister(rd);
            if (is_word) {
                bus_.Write32(mem_addr, value);
            } else {
                bus_.Write8(mem_addr, static_cast<u8>(value & 0xFF));
            }
        }
        
        // Write back offset if needed
        if (!is_pre_index && is_offset) {
            if (is_up) {
                SetRegister(rn, base + offset);
            } else {
                SetRegister(rn, base - offset);
            }
        }
        
        // Advance PC
        SetRegister(15, GetRegister(15) + 4);
        return 1;
    }
    // SWI (Software Interrupt)
    else if (opcode_4bit == 0b1111 && (opcode & 0x0FFFFFF0) == 0x00000000) {
        // Software interrupt
        ExceptionEnter(CpuMode::Supervisor, GetRegister(15) + 4, cpsr_);
        // SWI handler address is at 0x08
        SetRegister(15, 0x08);
        return 1;
    }
    
    // Default: treat as NOP and advance PC
    SetRegister(15, GetRegister(15) + 4);
    return 1;
}

// Thumb instruction execution (simplified)
int Cpu::ExecuteThumb(u16 opcode) {
    // For now, we'll implement a few basic Thumb instructions
    // and just advance PC for others
    
    // Check format: bits 15-13
    u32 opcode_top = (opcode >> 13) & 0x7;
    
    if (opcode_top == 0b000) {
        // Format 1: Move shifted register (LSL, LSR, ASR) or ADD/SUB with immediate
        u32 opcode_5bit = (opcode >> 11) & 0x1F;
        if ((opcode_5bit & 0x18) == 0) { // 0xx00 - Move shifted register
            // For simplicity, just advance PC
            SetRegister(15, GetRegister(15) + 2);
            return 1;
        }
        // Fall through for now
    } else if (opcode_top == 0b001) {
        // Format 2: ADD/SUB, MOV, CMP with immediate
        SetRegister(15, GetRegister(15) + 2);
        return 1;
    } else if (opcode_top == 0b010) {
        // Format 3: ALU operations
        SetRegister(15, GetRegister(15) + 2);
        return 1;
    } else if (opcode_top == 0b011) {
        // Format 4: Hi register operations/Branch exchange
        SetRegister(15, GetRegister(15) + 2);
        return 1;
    } else if (opcode_top == 0b100) {
        // Format 5: PC-relative load
        SetRegister(15, GetRegister(15) + 2);
        return 1;
    } else if (opcode_top == 0b101) {
        // Format 6: Load/store with register offset
        SetRegister(15, GetRegister(15) + 2);
        return 1;
    } else if (opcode_top == 0b110) {
        // Format 7: Load/store with immediate offset
        SetRegister(15, GetRegister(15) + 2);
        return 1;
    } else if (opcode_top == 0b111) {
        // Format 8: SP-relative load/store, Load address, Add offset to SP
        SetRegister(15, GetRegister(15) + 2);
        return 1;
    }
    
    // Default: advance PC
    SetRegister(15, GetRegister(15) + 2);
    return 1;
}

} // namespace gba