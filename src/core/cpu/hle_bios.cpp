#include "core/cpu/cpu.h"

namespace gba {

bool Cpu::TryHleSwi(u32 number) {
    switch (number) {
        case 0x01: HleRegisterRamReset(); return true;
        case 0x02: HleHalt();             return true;
        case 0x04: HleIntrWait();         return true; // IntrWait
        case 0x05: HleIntrWait();         return true; // VBlankIntrWait - see HleIntrWait's comment
        case 0x06: HleDiv();              return true;
        case 0x07: HleDivArm();           return true;
        case 0x0B: HleCpuSet();           return true;
        case 0x0C: HleCpuFastSet();       return true;
        default:
            // Not HLE'd - LZ77/Huffman/RL decompression, Sqrt, ArcTan,
            // BgAffineSet, ObjAffineSet, sound-related calls, and others
            // aren't implemented. Falls back to Arm/ThumbSoftwareInterrupt's
            // real EnterException(Supervisor, 0x08) path, which currently
            // has no real BIOS code to run there either - the call will
            // effectively do nothing useful. See the TODO on this
            // function's declaration in cpu.h.
            return false;
    }
}

void Cpu::HleRegisterRamReset() {
    // Real RegisterRamReset (R0 = bitmask of which memory regions to
    // clear: EWRAM/IWRAM/palette/VRAM/OAM/serial/sound/I-O) zeroes out
    // the requested regions. Our Bus already zero-initializes everything
    // at construction, so for a game calling this once at boot - by far
    // the common case - a no-op reaches the same end state. A game that
    // calls this mid-run expecting a real clear would see stale data;
    // that's a TODO if it turns out to matter in practice.
}

void Cpu::HleHalt() {
    halted_ = true;
}

void Cpu::HleIntrWait() {
    // Real IntrWait(waitForNew, wantedFlags) and VBlankIntrWait (a thin
    // wrapper that calls IntrWait(1, VBlank)) both wait for one of a
    // *specific* set of interrupt flags, tracked through a separate BIOS
    // Interrupt Flags mirror at 0x03007FF8 that the real IRQ trampoline
    // maintains. We simplify: treat this identically to Halt, waking on
    // ANY enabled interrupt rather than only the wanted ones.
    //
    // This is exact for VBlankIntrWait when VBlank is the only enabled
    // interrupt at the time (overwhelmingly the common case - most games'
    // main loop is "VBlankIntrWait(); do per-frame work; repeat"), and an
    // approximation otherwise (a game waiting on, say, Timer0 specifically
    // while VBlank is also enabled could wake up one frame early). Worth
    // revisiting if that turns out to matter for a real game.
    halted_ = true;
}

void Cpu::HleDiv() {
    // Div(number, denom) -> R0=number/denom, R1=number%denom, R3=|R0|
    const s32 number = static_cast<s32>(GetRegister(0));
    const s32 denom = static_cast<s32>(GetRegister(1));
    if (denom == 0) {
        // Real hardware hangs (or returns garbage depending on BIOS
        // version) on division by zero - well-behaved games never do
        // this. We just avoid the UB/crash and return zero rather than
        // replicate a hang.
        SetRegister(0, 0);
        SetRegister(1, 0);
        SetRegister(3, 0);
        return;
    }
    const s32 quotient = number / denom;   // C++ truncates toward zero, matching BIOS Div
    const s32 remainder = number % denom;  // same sign as the dividend, also matching
    SetRegister(0, static_cast<u32>(quotient));
    SetRegister(1, static_cast<u32>(remainder));
    SetRegister(3, static_cast<u32>(quotient < 0 ? -quotient : quotient));
}

void Cpu::HleDivArm() {
    // DivArm(denom, number) - same operation as Div, just with the two
    // arguments swapped in the calling convention. GBATEK "SWI 07h".
    const s32 denom = static_cast<s32>(GetRegister(0));
    const s32 number = static_cast<s32>(GetRegister(1));
    if (denom == 0) {
        SetRegister(0, 0);
        SetRegister(1, 0);
        SetRegister(3, 0);
        return;
    }
    const s32 quotient = number / denom;
    const s32 remainder = number % denom;
    SetRegister(0, static_cast<u32>(quotient));
    SetRegister(1, static_cast<u32>(remainder));
    SetRegister(3, static_cast<u32>(quotient < 0 ? -quotient : quotient));
}

void Cpu::HleCpuSet() {
    // CpuSet(src, dst, control): control bits0-20=word count, bit24=fixed
    // source (fill mode - re-reads the same source word/halfword every
    // iteration instead of advancing), bit26=transfer size (0=16-bit,
    // 1=32-bit). GBATEK "SWI 0Bh".
    const u32 src = GetRegister(0);
    const u32 dst = GetRegister(1);
    const u32 control = GetRegister(2);
    const u32 count = control & 0x1F'FFFFu;
    const bool fixedSource = (control & (1u << 24)) != 0;
    const bool wordTransfer = (control & (1u << 26)) != 0;
    const u32 unitSize = wordTransfer ? 4u : 2u;

    u32 s = src;
    u32 d = dst;
    for (u32 i = 0; i < count; ++i) {
        // We don't have direct Bus access here by design (Cpu only reads/
        // writes memory through the same Read/Write helpers instructions
        // use), so route through those rather than reaching into Bus.
        if (wordTransfer) {
            bus_.Write32(d, bus_.Read32(s));
        } else {
            bus_.Write16(d, bus_.Read16(s));
        }
        if (!fixedSource) {
            s += unitSize;
        }
        d += unitSize;
    }
}

void Cpu::HleCpuFastSet() {
    // CpuFastSet: same idea as CpuSet but always 32-bit, and real hardware
    // requires the count be a multiple of 8 words (it copies in 8-word
    // blocks). We don't need to replicate that hardware quirk for
    // correctness - copying the exact requested count produces the same
    // final memory contents for well-formed calls (which always pass a
    // multiple of 8 anyway, since that's the documented requirement).
    const u32 src = GetRegister(0);
    const u32 dst = GetRegister(1);
    const u32 control = GetRegister(2);
    const u32 count = control & 0x1F'FFFFu;
    const bool fixedSource = (control & (1u << 24)) != 0;

    u32 s = src;
    u32 d = dst;
    for (u32 i = 0; i < count; ++i) {
        bus_.Write32(d, bus_.Read32(s));
        if (!fixedSource) {
            s += 4u;
        }
        d += 4u;
    }
}

} // namespace gba
