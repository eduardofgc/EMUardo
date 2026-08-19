#include "core/cpu/cpu.h"

namespace gba {

bool Cpu::TryHleSwi(u32 number) {
    switch (number) {
        case 0x01: HleRegisterRamReset(); return true;
        case 0x02: HleHalt();             return true;
        case 0x04: HleIntrWait();         return true; // IntrWait
        case 0x05: HleVBlankIntrWait();   return true; // VBlankIntrWait
        case 0x06: HleDiv();              return true;
        case 0x07: HleDivArm();           return true;
        case 0x0B: HleCpuSet();           return true;
        case 0x0C: HleCpuFastSet();       return true;
        case 0x11: HleLz77UnComp();       return true; // LZ77UnCompWRAM
        case 0x12: HleLz77UnComp();       return true; // LZ77UnCompVRAM - see HleLz77UnComp's comment
        default:
            // Not HLE'd - Huffman/RL decompression, Diff8bit/16bitUnFilter,
            // Sqrt, ArcTan, BgAffineSet, ObjAffineSet, sound-related calls,
            // and others aren't implemented. Falls back to
            // Arm/ThumbSoftwareInterrupt's real EnterException(Supervisor,
            // 0x08) path, which currently has no real BIOS code to run
            // there either - the call will effectively do nothing useful.
            // See the TODO on this function's declaration in cpu.h.
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
    //
    // One side effect is NOT optional though: per GBATEK, real hardware
    // unconditionally forces DISPCNT=0x0080 (forced blank on) here,
    // regardless of R0. Games that call this as their very first
    // instruction (as most do, including Pokemon Emerald) rely on that -
    // Emerald's boot code queues its initial VBlank-IRQ-enable register
    // write rather than applying it immediately unless forced blank is
    // already active, and that queue is only ever flushed on a VBlank
    // interrupt - which can't fire without the very enable bit stuck in
    // the queue. Skipping this write turns that into a permanent
    // deadlock (a white screen, since nothing ever gets past the game's
    // own VBlank-wait loop).
    bus_.Write16(mem::kIoBase + io::kDispcnt, 0x0080u);
}

void Cpu::HleHalt() {
    halted_ = true;
}

void Cpu::HleIntrWait() {
    // Real IntrWait(waitForNew, wantedFlags) waits for one of a
    // *specific* set of interrupt flags, tracked through a separate BIOS
    // Interrupt Flags mirror at 0x03007FF8 that the real IRQ trampoline
    // maintains. We simplify: treat this identically to Halt, waking on
    // ANY enabled interrupt rather than only the wanted ones.
    //
    // This is exact when the wanted flags are the only interrupt enabled
    // at the time (overwhelmingly the common case), and an approximation
    // otherwise (a game waiting on, say, Timer0 specifically while VBlank
    // is also enabled could wake up one frame early). Worth revisiting if
    // that turns out to matter for a real game.
    halted_ = true;
}

void Cpu::HleVBlankIntrWait() {
    // VBlankIntrWait is a thin real-BIOS wrapper that first ensures
    // VBlank interrupts can actually reach IF at all - IE's VBlank bit
    // and, critically, DISPSTAT's VBlank-IRQ-enable bit (bit3), which is
    // what actually gates the PPU raising the interrupt condition in the
    // first place - before falling through to the same wait as
    // IntrWait(1, VBlank). A game that trusts the BIOS to do this (rather
    // than setting DISPSTAT itself beforehand) would otherwise wait
    // forever under our HLE, since nothing else ever sets that bit for
    // it. GBATEK "SWI 05h - VBlankIntrWait".
    u16 dispstat = bus_.Read16(mem::kIoBase + io::kDispstat);
    dispstat = static_cast<u16>(dispstat | (1u << 3));
    bus_.Write16(mem::kIoBase + io::kDispstat, dispstat);

    const u16 ie = bus_.Read16(mem::kIoBase + io::kIe);
    bus_.Write16(mem::kIoBase + io::kIe, static_cast<u16>(ie | irq::kVBlank));

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

void Cpu::HleLz77UnComp() {
    // LZ77UnCompWRAM/LZ77UnCompVRAM (SWI 0x11/0x12) - GBATEK "BIOS
    // Decompression Functions". Both do the exact same LZ77/LZSS-variant
    // decompression; the WRAM/VRAM distinction only exists on real
    // hardware because VRAM can't be written a byte at a time (it has to
    // buffer pairs and write 16-bit), which doesn't apply to our Bus -
    // Write8 to VRAM already stores a plain byte, so a single
    // implementation covers both.
    //
    // Header (4 bytes at R0): bits8-31 = decompressed size in bytes,
    // bits4-7 = compression type (1 = LZ77, not checked - well-formed ROM
    // data is assumed). Then a stream of 8-unit blocks: one flag byte
    // (MSB first) says whether each of the next 8 units is a raw byte
    // (flag bit clear) or a back-reference (flag bit set, 2 bytes:
    // length = high nibble of byte0 + 3, disp = (low nibble of byte0 << 8
    // | byte1) + 1) copying `length` bytes from `disp` bytes behind the
    // current output position, one byte at a time so overlapping copies
    // (runs shorter than the displacement) work correctly.
    const u32 srcAddr = GetRegister(0);
    const u32 dstAddr = GetRegister(1);

    const u32 header = bus_.Read32(srcAddr);
    const u32 decompressedSize = header >> 8;

    u32 src = srcAddr + 4;
    u32 dst = dstAddr;
    u32 written = 0;

    while (written < decompressedSize) {
        const u8 flags = bus_.Read8(src++);
        for (int bit = 7; bit >= 0 && written < decompressedSize; --bit) {
            if ((flags & (1u << bit)) == 0) {
                bus_.Write8(dst++, bus_.Read8(src++));
                ++written;
                continue;
            }
            const u8 byte0 = bus_.Read8(src++);
            const u8 byte1 = bus_.Read8(src++);
            const u32 length = (static_cast<u32>(byte0) >> 4) + 3u;
            const u32 disp = ((static_cast<u32>(byte0) & 0xFu) << 8 | byte1) + 1u;
            for (u32 i = 0; i < length && written < decompressedSize; ++i) {
                bus_.Write8(dst, bus_.Read8(dst - disp));
                ++dst;
                ++written;
            }
        }
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
