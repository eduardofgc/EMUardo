#include "core/cpu/cpu.h"

namespace gba {

Cpu::Cpu(Bus& bus) : bus_(bus) {
    Reset();
}

void Cpu::Reset() {
    registers_.fill(0);
    r8_12_fiq_.fill(0);
    r8_12_other_.fill(0);
    r13_banked_.fill(0);
    r14_banked_.fill(0);
    spsr_banked_.fill(0);

    // Real hardware's BIOS sets up a stack pointer for each mode before
    // handing control to the game (GBATEK "BIOS RAM Usage") - since we
    // skip running real BIOS boot code, we have to set these up ourselves
    // or any code that pushes to the stack (including our own HLE IRQ
    // trampoline's STMFD/LDMFD) writes to whatever SP happens to default
    // to (zero), silently corrupting memory near address 0.
    // Index into r13_banked_: 0=User/System, 1=FIQ, 2=IRQ, 3=Supervisor,
    // 4=Abort, 5=Undefined - see R13R14BankIndex().
    r13_banked_[0] = 0x0300'7F00u; // User/System
    r13_banked_[2] = 0x0300'7FA0u; // IRQ
    r13_banked_[3] = 0x0300'7FE0u; // Supervisor
    registers_[13] = r13_banked_[0]; // active register file starts in System mode

    // Real hardware runs a bit of BIOS code that ends up jumping to
    // 0x0800'0000 in ARM state, System mode, IRQ/FIQ disabled. We
    // fast-forward directly here - a "skip BIOS" boot path is standard
    // even in mature emulators, and lets us test without a BIOS dump.
    registers_[15] = mem::kRomBase;
    cpsr_ = static_cast<u32>(CpuMode::System) |
            static_cast<u32>(Flag::I) |
            static_cast<u32>(Flag::F);
}

void Cpu::SaveState(StateWriter& w) const {
    w.Write(registers_);
    w.Write(cpsr_);
    w.Write(r8_12_fiq_);
    w.Write(r8_12_other_);
    w.Write(r13_banked_);
    w.Write(r14_banked_);
    w.Write(spsr_banked_);
    w.Write(halted_);
}

void Cpu::LoadState(StateReader& r) {
    r.Read(registers_);
    r.Read(cpsr_);
    r.Read(r8_12_fiq_);
    r.Read(r8_12_other_);
    r.Read(r13_banked_);
    r.Read(r14_banked_);
    r.Read(spsr_banked_);
    r.Read(halted_);
}

// ---------------------------------------------------------------------
// Register access / mode switching
// ---------------------------------------------------------------------

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

void Cpu::SetCpsr(u32 value) {
    const auto newMode = static_cast<CpuMode>(value & 0x1Fu);
    if (newMode != GetMode()) {
        SwitchMode(newMode);
    }
    cpsr_ = value;
}

int Cpu::R13R14BankIndex(CpuMode mode) {
    switch (mode) {
        case CpuMode::FIQ:        return 1;
        case CpuMode::IRQ:        return 2;
        case CpuMode::Supervisor: return 3;
        case CpuMode::Abort:      return 4;
        case CpuMode::Undefined:  return 5;
        default:                  return 0; // User / System share a bank
    }
}

int Cpu::SpsrBankIndex(CpuMode mode) {
    switch (mode) {
        case CpuMode::FIQ:        return 0;
        case CpuMode::IRQ:        return 1;
        case CpuMode::Supervisor: return 2;
        case CpuMode::Abort:      return 3;
        case CpuMode::Undefined:  return 4;
        default:                  return -1; // User / System have no SPSR
    }
}

void Cpu::SwitchMode(CpuMode newMode) {
    const CpuMode oldMode = GetMode();
    if (oldMode == newMode) {
        return;
    }

    // Save r8-r12 to whichever bank the outgoing mode uses.
    auto& oldR8_12 = (oldMode == CpuMode::FIQ) ? r8_12_fiq_ : r8_12_other_;
    for (int i = 0; i < 5; ++i) {
        oldR8_12[static_cast<std::size_t>(i)] = registers_[static_cast<std::size_t>(8 + i)];
    }

    // Save r13/r14 to the outgoing mode's bank.
    const int oldBank = R13R14BankIndex(oldMode);
    r13_banked_[static_cast<std::size_t>(oldBank)] = registers_[13];
    r14_banked_[static_cast<std::size_t>(oldBank)] = registers_[14];

    // Load r8-r12 for the incoming mode.
    auto& newR8_12 = (newMode == CpuMode::FIQ) ? r8_12_fiq_ : r8_12_other_;
    for (int i = 0; i < 5; ++i) {
        registers_[static_cast<std::size_t>(8 + i)] = newR8_12[static_cast<std::size_t>(i)];
    }

    // Load r13/r14 for the incoming mode.
    const int newBank = R13R14BankIndex(newMode);
    registers_[13] = r13_banked_[static_cast<std::size_t>(newBank)];
    registers_[14] = r14_banked_[static_cast<std::size_t>(newBank)];

    cpsr_ = (cpsr_ & ~0x1Fu) | static_cast<u32>(newMode);
}

u32 Cpu::Spsr() const {
    const int idx = SpsrBankIndex(GetMode());
    // Reading SPSR in User/System mode is architecturally undefined (there
    // isn't one) - returning CPSR is a harmless fallback rather than
    // reading garbage.
    return idx >= 0 ? spsr_banked_[static_cast<std::size_t>(idx)] : cpsr_;
}

void Cpu::SetSpsr(u32 value) {
    const int idx = SpsrBankIndex(GetMode());
    if (idx >= 0) {
        spsr_banked_[static_cast<std::size_t>(idx)] = value;
    }
}

void Cpu::EnterException(CpuMode mode, u32 vectorAddress) {
    // registers_[15] already holds "address of the instruction after this
    // one" per the fetch-then-increment convention described in cpu.h, so
    // it's exactly the return address exceptions need in LR.
    const u32 returnPc = registers_[15];
    const u32 oldCpsr = cpsr_;

    SwitchMode(mode);
    registers_[14] = returnPc;
    SetSpsr(oldCpsr);
    SetFlag(Flag::I, true);
    if (mode == CpuMode::FIQ) {
        SetFlag(Flag::F, true);
    }
    SetFlag(Flag::T, false); // exceptions always enter ARM state
    registers_[15] = vectorAddress;
}

bool Cpu::CheckInterrupts() {
    if (GetFlag(Flag::I)) {
        return false; // CPU-side IRQ disable (CPSR I bit)
    }

    const u16 ime = bus_.Read16(mem::kIoBase + io::kIme);
    if ((ime & 0x1u) == 0) {
        return false; // master enable off
    }

    const u16 enabled = bus_.Read16(mem::kIoBase + io::kIe);
    const u16 requested = bus_.Read16(mem::kIoBase + io::kIf);
    if ((enabled & requested) == 0) {
        return false; // nothing both enabled and pending
    }

    // NOTE: we don't clear the matching IF bit(s) here - real hardware
    // doesn't either. Acknowledging an interrupt is software's job (the
    // handler writes 1 back to IF, see Bus::Write8's special case), which
    // is also what lets several sources share one IF bit correctly.

    // Real IRQ handlers universally return via "SUBS PC,LR,#4" - a
    // convention that compensates for the 3-stage pipeline's PC-ahead
    // offset on real hardware. We don't model that pipeline (registers_[15]
    // here already holds the address of the next not-yet-fetched
    // instruction, with no offset baked in), so to make that same "-4"
    // land on the correct address, LR_irq needs the +4 added explicitly
    // here - EnterException() just copies whatever's in registers_[15]
    // into LR verbatim.
    registers_[15] += 4;
    EnterException(CpuMode::IRQ, 0x0000'0018u);
    return true;
}

// ---------------------------------------------------------------------
// Fetch / condition checking
// ---------------------------------------------------------------------

u32 Cpu::FetchArm() {
    return bus_.Read32(registers_[15]);
}

u16 Cpu::FetchThumb() {
    return bus_.Read16(registers_[15]);
}

bool Cpu::CheckCondition(u32 instruction) const {
    switch (instruction >> 28) {
        case 0x0: return GetFlag(Flag::Z);                                       // EQ
        case 0x1: return !GetFlag(Flag::Z);                                      // NE
        case 0x2: return GetFlag(Flag::C);                                       // CS/HS
        case 0x3: return !GetFlag(Flag::C);                                      // CC/LO
        case 0x4: return GetFlag(Flag::N);                                       // MI
        case 0x5: return !GetFlag(Flag::N);                                      // PL
        case 0x6: return GetFlag(Flag::V);                                       // VS
        case 0x7: return !GetFlag(Flag::V);                                      // VC
        case 0x8: return GetFlag(Flag::C) && !GetFlag(Flag::Z);                  // HI
        case 0x9: return !GetFlag(Flag::C) || GetFlag(Flag::Z);                  // LS
        case 0xA: return GetFlag(Flag::N) == GetFlag(Flag::V);                   // GE
        case 0xB: return GetFlag(Flag::N) != GetFlag(Flag::V);                   // LT
        case 0xC: return !GetFlag(Flag::Z) && (GetFlag(Flag::N) == GetFlag(Flag::V)); // GT
        case 0xD: return GetFlag(Flag::Z) || (GetFlag(Flag::N) != GetFlag(Flag::V));  // LE
        case 0xE: return true;                                                   // AL
        default:  return false;                                                  // 0xF: reserved/never on ARMv4T
    }
}

bool Cpu::CheckHaltWakeup() const {
    // Halt exit only requires (IE & IF) != 0, unlike normal interrupt
    // dispatch - it wakes even with IME=0 (the CPU just resumes normal
    // execution in that case, without actually servicing the interrupt).
    // GBATEK "SWI 02h/03h - Halt/Stop".
    const u16 enabled = bus_.Read16(mem::kIoBase + io::kIe);
    const u16 requested = bus_.Read16(mem::kIoBase + io::kIf);
    return (enabled & requested) != 0;
}

int Cpu::Step() {
    if (halted_) {
        if (CheckHaltWakeup()) {
            halted_ = false;
            // Fall through - if IME/IE also permit it, the interrupt that
            // just woke us should be dispatched immediately this same
            // Step(), matching real hardware.
        } else {
            return 1; // still halted; burn a cycle doing nothing
        }
    }

    if (CheckInterrupts()) {
        return 3; // exception entry is a pipeline flush - 2S+1N, same cost as a taken branch
    }

    if (GetFlag(Flag::T)) {
        const u16 instruction = FetchThumb();
        registers_[15] += 2; // advance PC before execute - mirrors the ARM path below
        const int cycles = ComputeThumbCycles(instruction);
        ExecuteThumb(instruction);
        return cycles;
    }

    const u32 instruction = FetchArm();
    registers_[15] += 4; // advance PC before execute - see cpu.h pipeline note

    if (CheckCondition(instruction)) {
        const int cycles = ComputeArmCycles(instruction);
        ExecuteArm(instruction);
        return cycles;
    }
    // Condition failed: real hardware just burns 1S regardless of what
    // the instruction would otherwise have cost - it never reaches
    // execution, so none of its own memory/pipeline costs apply.
    return 1;
}

// ---------------------------------------------------------------------
// Barrel shifter
// ---------------------------------------------------------------------

u32 Cpu::Shift(ShiftType type, u32 value, u32 amount, bool& carryInOut,
               bool isImmediateShift) const {
    if (isImmediateShift && amount == 0) {
        switch (type) {
            case ShiftType::LSL:
                return value; // carry unchanged
            case ShiftType::LSR:
                amount = 32; // LSR#0 encodes LSR#32
                break;
            case ShiftType::ASR:
                amount = 32; // ASR#0 encodes ASR#32
                break;
            case ShiftType::ROR: {
                // ROR#0 encodes RRX: rotate right by 1 through the carry flag.
                const bool oldCarry = carryInOut;
                const u32 result = (value >> 1) | (oldCarry ? 0x8000'0000u : 0u);
                carryInOut = (value & 1u) != 0;
                return result;
            }
        }
    } else if (!isImmediateShift && amount == 0) {
        return value; // register-specified shift of 0: no-op, carry unchanged
    }

    switch (type) {
        case ShiftType::LSL:
            if (amount >= 32) {
                carryInOut = (amount == 32) && ((value & 1u) != 0);
                return 0;
            }
            carryInOut = ((value >> (32 - amount)) & 1u) != 0;
            return value << amount;

        case ShiftType::LSR:
            if (amount >= 32) {
                carryInOut = (amount == 32) && (((value >> 31) & 1u) != 0);
                return 0;
            }
            carryInOut = ((value >> (amount - 1)) & 1u) != 0;
            return value >> amount;

        case ShiftType::ASR: {
            const bool signBit = (value & 0x8000'0000u) != 0;
            if (amount >= 32) {
                carryInOut = signBit;
                return signBit ? 0xFFFF'FFFFu : 0u;
            }
            carryInOut = ((value >> (amount - 1)) & 1u) != 0;
            return static_cast<u32>(static_cast<s32>(value) >> amount);
        }

        case ShiftType::ROR: {
            const u32 rotated = amount & 31u;
            if (rotated == 0) {
                // Original amount was a nonzero multiple of 32.
                carryInOut = ((value >> 31) & 1u) != 0;
                return value;
            }
            carryInOut = ((value >> (rotated - 1)) & 1u) != 0;
            return (value >> rotated) | (value << (32 - rotated));
        }
    }
    return value; // unreachable
}

u32 Cpu::GetImmediateOperand2(u32 instruction, bool& carryOut) const {
    const u32 imm = instruction & 0xFFu;
    const u32 rotate = ((instruction >> 8) & 0xFu) * 2;
    if (rotate == 0) {
        carryOut = GetFlag(Flag::C);
        return imm;
    }
    const u32 result = (imm >> rotate) | (imm << (32 - rotate));
    carryOut = ((result >> 31) & 1u) != 0;
    return result;
}

u32 Cpu::GetRegisterOperand2(u32 instruction, bool& carryOut) const {
    const u32 rm = instruction & 0xFu;
    const bool shiftFromRegister = ((instruction >> 4) & 1u) != 0;

    // PC-as-operand quirk (see cpu.h header comment): registers_[15]
    // already holds current+4, so +4 more gives PC+8, +8 more gives PC+12.
    u32 value = (rm == 15)
        ? registers_[15] + (shiftFromRegister ? 8u : 4u)
        : GetRegister(static_cast<int>(rm));

    const auto shiftType = static_cast<ShiftType>((instruction >> 5) & 0x3u);
    bool carry = GetFlag(Flag::C);

    if (shiftFromRegister) {
        const u32 rs = (instruction >> 8) & 0xFu;
        const u32 amount = GetRegister(static_cast<int>(rs)) & 0xFFu;
        value = Shift(shiftType, value, amount, carry, /*isImmediateShift=*/false);
    } else {
        const u32 amount = (instruction >> 7) & 0x1Fu;
        value = Shift(shiftType, value, amount, carry, /*isImmediateShift=*/true);
    }

    carryOut = carry;
    return value;
}

// ---------------------------------------------------------------------
// Top-level ARM decode. Order matters here: several instruction classes
// share overlapping bit patterns, so more specific patterns are checked
// before falling through to the more general ones. See GBATEK's
// "ARM Binary Opcode Format" for the reference bit layouts this mirrors.
// ---------------------------------------------------------------------

void Cpu::ExecuteArm(u32 instruction) {
    // Branch and Exchange: cond 0001 0010 1111 1111 1111 0001 Rn
    if ((instruction & 0x0FFF'FFF0u) == 0x012F'FF10u) {
        ArmBranchExchange(instruction);
        return;
    }

    // PSR Transfer - MRS (read): cond 00010 R 001111 Rd 000000000000
    if ((instruction & 0x0FBF'0FFFu) == 0x010F'0000u) {
        ArmMrs(instruction);
        return;
    }

    // PSR Transfer - MSR, register operand: cond 00010 R 10 1001 1111 00000000 Rm
    if ((instruction & 0x0FB0'FFF0u) == 0x0120'F000u) {
        ArmMsrRegister(instruction);
        return;
    }

    // PSR Transfer - MSR, immediate operand: cond 00 1 10 R 10 1111 rotate imm8
    if ((instruction & 0x0FB0'F000u) == 0x0320'F000u) {
        ArmMsrImmediate(instruction);
        return;
    }

    // Multiply (MUL/MLA): bits27-22=000000, bits7-4=1001
    if ((instruction & 0x0FC0'00F0u) == 0x0000'0090u) {
        ArmMultiply(instruction);
        return;
    }

    // Multiply Long (UMULL/UMLAL/SMULL/SMLAL): bits27-23=00001, bits7-4=1001
    if ((instruction & 0x0F80'00F0u) == 0x0080'0090u) {
        ArmMultiplyLong(instruction);
        return;
    }

    // Single Data Swap (SWP/SWPB): bits27-23=00010, bits11-4=00001001
    if ((instruction & 0x0FB0'0FF0u) == 0x0100'0090u) {
        ArmSingleDataSwap(instruction);
        return;
    }

    // Halfword/Signed Data Transfer family: bits27-25=000, bit7=1, bit4=1,
    // with bits6-5 != 00 (that case is one of the three patterns above).
    if ((instruction & 0x0E00'0090u) == 0x0000'0090u) {
        ArmHalfwordTransfer(instruction);
        return;
    }

    const u32 bits27_26 = (instruction >> 26) & 0x3u;

    switch (bits27_26) {
        case 0b00:
            // Everything more specific has already been peeled off above;
            // what's left is plain Data Processing (immediate or register
            // operand2, selected internally via bit25).
            ArmDataProcessing(instruction);
            return;

        case 0b01: {
            const bool immediateBit = ((instruction >> 25) & 1u) != 0;
            const bool bit4 = ((instruction >> 4) & 1u) != 0;
            if (immediateBit && bit4) {
                ArmUndefined(instruction); // reserved encoding
            } else {
                ArmSingleDataTransfer(instruction);
            }
            return;
        }

        case 0b10: {
            const bool branchBit = ((instruction >> 25) & 1u) != 0;
            if (branchBit) {
                ArmBranch(instruction);
            } else {
                ArmBlockDataTransfer(instruction);
            }
            return;
        }

        case 0b11: {
            const bool isSwi = ((instruction >> 24) & 0xFu) == 0xFu;
            if (isSwi) {
                ArmSoftwareInterrupt(instruction);
            } else {
                // Coprocessor instructions (CDP/LDC/STC/MRC/MCR) - the GBA
                // has no coprocessor, so real games never emit these.
                ArmUndefined(instruction);
            }
            return;
        }
    }
}

// ---------------------------------------------------------------------
// Top-level Thumb decode. Thumb has 19 fixed-shape instruction "formats"
// (ARM7TDMI TRM's naming, not opcodes) rather than ARM's more orthogonal
// bit-field layout, so this dispatches mostly on the top few bits, with a
// couple of formats sharing a prefix and needing one more bit to
// disambiguate (noted inline). See GBATEK "THUMB Instruction Summary".
// ---------------------------------------------------------------------

void Cpu::ExecuteThumb(u16 instruction) {
    const u16 bits15_13 = static_cast<u16>((instruction >> 13) & 0x7u);
    const u16 bits15_12 = static_cast<u16>((instruction >> 12) & 0xFu);
    const u16 bits15_11 = static_cast<u16>((instruction >> 11) & 0x1Fu);
    const u16 bits15_10 = static_cast<u16>((instruction >> 10) & 0x3Fu);
    const u16 bits15_8  = static_cast<u16>((instruction >> 8) & 0xFFu);

    // Format 2 (Add/Subtract) shares Format 1's 000 prefix; check it first.
    if (bits15_11 == 0b00011u) {
        ThumbAddSubtract(instruction);
        return;
    }
    if (bits15_13 == 0b000u) {
        ThumbMoveShiftedRegister(instruction);
        return;
    }
    if (bits15_13 == 0b001u) {
        ThumbImmediateOp(instruction);
        return;
    }
    if (bits15_10 == 0b010000u) {
        ThumbAluOp(instruction);
        return;
    }
    if (bits15_10 == 0b010001u) {
        ThumbHiRegOpsBranchExchange(instruction);
        return;
    }
    if (bits15_11 == 0b01001u) {
        ThumbPcRelativeLoad(instruction);
        return;
    }
    if (bits15_12 == 0b0101u) {
        // bit9 splits this nibble into reg-offset load/store (0) vs
        // sign-extended byte/halfword load/store (1) - GBATEK Format 7/8.
        if (((instruction >> 9) & 1u) == 0) {
            ThumbLoadStoreRegOffset(instruction);
        } else {
            ThumbLoadStoreSignExtended(instruction);
        }
        return;
    }
    if (bits15_13 == 0b011u) {
        ThumbLoadStoreImmOffset(instruction);
        return;
    }
    if (bits15_12 == 0b1000u) {
        ThumbLoadStoreHalfword(instruction);
        return;
    }
    if (bits15_12 == 0b1001u) {
        ThumbSpRelativeLoadStore(instruction);
        return;
    }
    if (bits15_12 == 0b1010u) {
        ThumbLoadAddress(instruction);
        return;
    }
    if (bits15_8 == 0b10110000u) {
        ThumbAddOffsetToSp(instruction);
        return;
    }
    if (bits15_12 == 0b1011u && ((instruction >> 9) & 0x3u) == 0b10u) {
        ThumbPushPopRegisters(instruction);
        return;
    }
    if (bits15_12 == 0b1100u) {
        ThumbMultipleLoadStore(instruction);
        return;
    }
    if (bits15_8 == 0b11011111u) {
        ThumbSoftwareInterrupt(instruction);
        return;
    }
    if (bits15_12 == 0b1101u) {
        ThumbConditionalBranch(instruction);
        return;
    }
    if (bits15_11 == 0b11100u) {
        ThumbUnconditionalBranch(instruction);
        return;
    }
    if (bits15_12 == 0b1111u) {
        ThumbLongBranchLink(instruction);
        return;
    }

    // Unreachable if the format checks above are exhaustive - every 16-bit
    // pattern belongs to exactly one of Thumb formats 1-19.
}

} // namespace gba
