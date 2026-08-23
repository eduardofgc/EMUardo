#include "core/cpu/cpu.h"

#include <cmath>

namespace gba {

namespace {
constexpr double kPi = 3.14159265358979323846;
} // namespace

bool Cpu::TryHleSwi(u32 number) {
    switch (number) {
        case 0x01: HleRegisterRamReset(); return true;
        case 0x02: HleHalt();             return true;
        case 0x04: HleIntrWait();         return true; // IntrWait
        case 0x05: HleVBlankIntrWait();   return true; // VBlankIntrWait
        case 0x06: HleDiv();              return true;
        case 0x07: HleDivArm();           return true;
        case 0x08: HleSqrt();             return true;
        case 0x09: HleArcTan();           return true;
        case 0x0A: HleArcTan2();          return true;
        case 0x0B: HleCpuSet();           return true;
        case 0x0C: HleCpuFastSet();       return true;
        case 0x0E: HleBgAffineSet();       return true; // BgAffineSet
        case 0x0F: HleObjAffineSet();      return true; // ObjAffineSet
        case 0x11: HleLz77UnComp();       return true; // LZ77UnCompWRAM
        case 0x12: HleLz77UnComp();       return true; // LZ77UnCompVRAM - see HleLz77UnComp's comment
        case 0x13: HleHuffUnComp();       return true; // HuffUnComp
        case 0x14: HleRlUnComp();         return true; // RLUnCompWRAM
        case 0x15: HleRlUnComp();         return true; // RLUnCompVRAM - same non-reason as LZ77's WRAM/VRAM split
        case 0x16: HleDiff8bitUnFilter(); return true; // Diff8bitUnFilterWRAM
        case 0x17: HleDiff8bitUnFilter(); return true; // Diff8bitUnFilterVRAM
        case 0x18: HleDiff16bitUnFilter(); return true; // Diff16bitUnFilter
        case 0x19: HleSoundBias();        return true; // SoundBias
        default:
            // Not HLE'd - the rest of the "Sound Driver" family
            // (SoundDriverInit/Mode/Main/VSync and friends, SWI 0x1A-0x25)
            // are BIOS-resident entry points into Nintendo's undocumented
            // "Sappy"/MP2k software synth (multi-channel PCM mixing,
            // envelopes, reverb, an internal work-area struct GBATEK
            // itself only partially reverse-engineers) - reimplementing
            // those from memory with no test ROM that actually calls them
            // would be pure guesswork with no way to verify correctness,
            // unlike SoundBias's single well-specified register ramp
            // above. Left unimplemented until a real sample surfaces.
            // Falls back to Arm/ThumbSoftwareInterrupt's real
            // EnterException(Supervisor, 0x08) path, which currently has
            // no real BIOS code to run there either - the call will
            // effectively do nothing useful.
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

void Cpu::HleSqrt() {
    // Sqrt (SWI 0x08) - r0 = floor(sqrt(r0)), unsigned 32-bit input,
    // result fits in 16 bits. Uses the standard bit-by-bit integer square
    // root algorithm rather than casting a floating-point std::sqrt()
    // result - the latter can land one below the correct answer for some
    // perfect squares due to rounding, which an exact integer method
    // never does.
    const u32 value = GetRegister(0);
    u32 result = 0;
    u32 bit = 1u << 30; // highest relevant power-of-4 for a 32-bit input
    while (bit > value) {
        bit >>= 2;
    }
    u32 remaining = value;
    while (bit != 0) {
        if (remaining >= result + bit) {
            remaining -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    SetRegister(0, result);
}

void Cpu::HleArcTan() {
    // ArcTan (SWI 0x09) - r0 = tan as a signed 1.14 fixed-point value;
    // result is an angle in the GBA's standard format where 0x10000 = one
    // full turn (so the documented -0x4000..0x3FFF result range is
    // exactly -90..+90 degrees). GBATEK notes the real BIOS uses its own
    // fixed polynomial approximation with a small well-known error; this
    // uses std::atan() instead, which is more accurate but not a bit-
    // exact clone of the BIOS's specific quirks - fine for anything using
    // this to steer a sprite/camera angle, since the difference is far
    // below what's visually distinguishable at GBA's angle resolution.
    const s32 raw = static_cast<s32>(static_cast<s16>(GetRegister(0) & 0xFFFFu));
    const double tan = static_cast<double>(raw) / 16384.0; // 1.14 fixed -> real
    const double angle = std::atan(tan); // radians, in (-pi/2, pi/2)
    const auto result = static_cast<s32>(std::lround(angle / (2.0 * kPi) * 65536.0));
    SetRegister(0, static_cast<u32>(result) & 0xFFFFu);
}

void Cpu::HleArcTan2() {
    // ArcTan2 (SWI 0x0A) - r0=x, r1=y as plain signed 16-bit integers;
    // result is the angle from the origin to (x,y) in the same
    // 0x0000-0xFFFF = one full turn format as ArcTan. Same "std::atan2
    // instead of the BIOS's own approximation" tradeoff as ArcTan above.
    const s32 x = static_cast<s32>(static_cast<s16>(GetRegister(0) & 0xFFFFu));
    const s32 y = static_cast<s32>(static_cast<s16>(GetRegister(1) & 0xFFFFu));
    double angle = std::atan2(static_cast<double>(y), static_cast<double>(x)); // (-pi, pi]
    if (angle < 0.0) {
        angle += 2.0 * kPi; // normalize to [0, 2pi)
    }
    const auto result = static_cast<u32>(std::lround(angle / (2.0 * kPi) * 65536.0));
    SetRegister(0, result & 0xFFFFu);
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

void Cpu::HleRlUnComp() {
    // RLUnCompWram/VRAM (SWI 0x14/0x15) - GBATEK "BIOS Decompression
    // Functions". Header (4 bytes at R0): bits8-31 = decompressed size in
    // bytes, bits4-7 = compression type (3 = run-length, not checked).
    // Then a stream of blocks, each starting with a flag byte: bit7 clear
    // means the next (flag&0x7F)+1 bytes that follow are copied verbatim;
    // bit7 set means ONE byte follows and gets repeated (flag&0x7F)+3
    // times.
    const u32 srcAddr = GetRegister(0);
    const u32 dstAddr = GetRegister(1);

    const u32 header = bus_.Read32(srcAddr);
    const u32 decompressedSize = header >> 8;

    u32 src = srcAddr + 4;
    u32 dst = dstAddr;
    u32 written = 0;

    while (written < decompressedSize) {
        const u8 flag = bus_.Read8(src++);
        if (flag & 0x80u) {
            const u8 value = bus_.Read8(src++);
            const u32 length = (flag & 0x7Fu) + 3u;
            for (u32 i = 0; i < length && written < decompressedSize; ++i) {
                bus_.Write8(dst++, value);
                ++written;
            }
        } else {
            const u32 length = (flag & 0x7Fu) + 1u;
            for (u32 i = 0; i < length && written < decompressedSize; ++i) {
                bus_.Write8(dst++, bus_.Read8(src++));
                ++written;
            }
        }
    }
}

void Cpu::HleDiff8bitUnFilter() {
    // Diff8bitUnFilterWram/VRAM (SWI 0x16/0x17) - GBATEK "BIOS
    // Decompression Functions". Not actually compression - a delta
    // filter. Header (4 bytes at R0): bits8-31 = decompressed size in
    // bytes. Each output byte is the running (8-bit wraparound) sum of
    // every input byte up to and including it - the inverse of an
    // encoder that stored each byte as the difference from the one
    // before it, which tends to compress well afterward with a generic
    // compressor since smoothly-varying data (gradients, audio-like
    // curves) turns into mostly-small values.
    const u32 srcAddr = GetRegister(0);
    const u32 dstAddr = GetRegister(1);

    const u32 header = bus_.Read32(srcAddr);
    const u32 decompressedSize = header >> 8;

    u32 src = srcAddr + 4;
    u32 dst = dstAddr;
    u8 running = 0;
    for (u32 i = 0; i < decompressedSize; ++i) {
        running = static_cast<u8>(running + bus_.Read8(src++));
        bus_.Write8(dst++, running);
    }
}

void Cpu::HleDiff16bitUnFilter() {
    // Diff16bitUnFilter (SWI 0x18) - same idea as Diff8bitUnFilter, just
    // accumulating 16-bit units instead of bytes (the header's size field
    // is still a byte count).
    const u32 srcAddr = GetRegister(0);
    const u32 dstAddr = GetRegister(1);

    const u32 header = bus_.Read32(srcAddr);
    const u32 decompressedSize = header >> 8;

    u32 src = srcAddr + 4;
    u32 dst = dstAddr;
    u16 running = 0;
    for (u32 i = 0; i < decompressedSize; i += 2) {
        running = static_cast<u16>(running + bus_.Read16(src));
        src += 2;
        bus_.Write16(dst, running);
        dst += 2;
    }
}

void Cpu::HleHuffUnComp() {
    // HuffUnComp (SWI 0x13) - GBATEK "BIOS Decompression Functions".
    // Header (4 bytes at R0): bits0-3 = data unit size in bits (4 or 8,
    // not checked - well-formed ROM data is assumed like the other
    // decompressors here), bits4-7 = compression type (2 = Huffman),
    // bits8-31 = decompressed size in bytes.
    //
    // Immediately after the header sits a binary tree: one size byte
    // (tree table byte count, including the size byte itself, is
    // (sizeByte+1)*2) followed by the node bytes, then a bitstream of
    // 32-bit words with bit31 read first.
    //
    // Each non-leaf node byte holds bits0-5 = an offset used to locate its
    // two children, bit6 = "child reached via a 1 bit is a data byte, not
    // another node", bit7 = same for the 0-bit child. Per GBATEK, from a
    // node at absolute address `nodeAddr`, its child for bit value `b` is
    // at `(nodeAddr & ~1) + offset*2 + 2 + b` - the AND-NOT-1 re-aligns to
    // the node's sibling pair regardless of whether this node itself was
    // the even or odd half of its own parent's pair.
    const u32 srcAddr = GetRegister(0);
    const u32 dstAddr = GetRegister(1);

    const u32 header = bus_.Read32(srcAddr);
    const u32 dataBits = header & 0xFu;
    const u32 decompressedSize = header >> 8;

    const u32 treeTableAddr = srcAddr + 4;
    const u32 treeTableBytes = (static_cast<u32>(bus_.Read8(treeTableAddr)) + 1u) * 2u;
    u32 bitstreamPos = treeTableAddr + treeTableBytes;

    u32 dst = dstAddr;
    u32 written = 0;
    u32 bitBuffer = 0;
    int bitsLeft = 0;
    u32 packedByte = 0;
    int packedBits = 0;

    while (written < decompressedSize) {
        u32 nodeAddr = treeTableAddr + 1; // root node, right after the size byte
        for (;;) {
            if (bitsLeft == 0) {
                bitBuffer = bus_.Read32(bitstreamPos);
                bitstreamPos += 4;
                bitsLeft = 32;
            }
            const u32 bit = bitBuffer >> 31;
            bitBuffer <<= 1;
            --bitsLeft;

            const u8 node = bus_.Read8(nodeAddr);
            const u32 offset = node & 0x3Fu;
            const bool isData = bit ? ((node & 0x40u) != 0) : ((node & 0x80u) != 0);
            const u32 childAddr = (nodeAddr & ~1u) + offset * 2u + 2u + bit;

            if (!isData) {
                nodeAddr = childAddr;
                continue;
            }

            const u32 symbol = bus_.Read8(childAddr);
            if (dataBits == 8) {
                bus_.Write8(dst++, static_cast<u8>(symbol));
                ++written;
            } else {
                packedByte |= (symbol & 0xFu) << packedBits;
                packedBits += 4;
                if (packedBits == 8) {
                    bus_.Write8(dst++, static_cast<u8>(packedByte));
                    ++written;
                    packedByte = 0;
                    packedBits = 0;
                }
            }
            break;
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

void Cpu::HleObjAffineSet() {
    // ObjAffineSet(src, dst, count, diff) - GBATEK "SWI 0Fh". Builds
    // rotation/scaling matrices from a scale+angle description, e.g. for
    // the rotating logo in Pokemon Emerald's intro.
    //
    // Source entries are 8 bytes each (2 bytes padding after the
    // meaningful 6): s16 sx, s16 sy (8.8 fixed point), u16 theta (only
    // the upper 8 bits matter - 256 steps over a full turn, GBATEK: "the
    // GBA BIOS recurses only the upper 8bit").
    //
    // Destination is PA,PB,PC,PD (s16 each) per entry, but not
    // necessarily packed together - `diff` is the byte stride between
    // each of the four fields (2 = tightly packed, 8 = OAM layout, where
    // PA/PB/PC/PD live in the affine-parameter attr of four consecutive
    // 8-byte OAM entries).
    const u32 src = GetRegister(0);
    const u32 dst = GetRegister(1);
    const u32 count = GetRegister(2);
    const u32 diff = GetRegister(3);

    for (u32 i = 0; i < count; ++i) {
        const u32 entrySrc = src + i * 8u;
        const auto sx = static_cast<s32>(static_cast<s16>(bus_.Read16(entrySrc + 0)));
        const auto sy = static_cast<s32>(static_cast<s16>(bus_.Read16(entrySrc + 2)));
        const u32 theta = bus_.Read16(entrySrc + 4) >> 8; // upper 8 bits = angle step, 256/turn

        const double angle = (static_cast<double>(theta) / 256.0) * 2.0 * kPi;
        const double cosA = std::cos(angle);
        const double sinA = std::sin(angle);

        // sx/sy are 8.8 fixed point; keep the result in that same format.
        const auto pa = static_cast<s16>(std::lround(sx * cosA));
        const auto pb = static_cast<s16>(std::lround(-sx * sinA));
        const auto pc = static_cast<s16>(std::lround(sy * sinA));
        const auto pd = static_cast<s16>(std::lround(sy * cosA));

        const u32 entryDst = dst + i * diff * 4u;
        bus_.Write16(entryDst + 0u * diff, static_cast<u16>(pa));
        bus_.Write16(entryDst + 1u * diff, static_cast<u16>(pb));
        bus_.Write16(entryDst + 2u * diff, static_cast<u16>(pc));
        bus_.Write16(entryDst + 3u * diff, static_cast<u16>(pd));
    }
}

void Cpu::HleBgAffineSet() {
    // BgAffineSet (SWI 0x0E) - GBATEK "SWI 0Eh". Same rotation/scale
    // matrix math as ObjAffineSet, but also folds in a pivot (an origin
    // center in BG/texture space plus a display center in screen space)
    // so the destination directly yields the BG's X/Y reference point
    // alongside PA/PB/PC/PD - e.g. for Pokemon Emerald's rotating
    // "Pokemon" title-screen logo, which rotates/scales around a fixed
    // point rather than around the screen's top-left corner.
    //
    // Source entries are 20 bytes: s32 origin center X, s32 origin center
    // Y (both BG-space, 8.8 fixed point), s16 display center X, s16
    // display center Y (screen-space, plain integer), s16 scale X, s16
    // scale Y (8.8 fixed point), u16 angle (only the upper 8 bits matter,
    // same 256-steps-per-turn convention as ObjAffineSet), then 2 bytes
    // of trailing padding.
    //
    // Destination entries are 16 bytes: s16 PA, PB, PC, PD, then s32
    // Start X, s32 Start Y - exactly the register layout of a BG's
    // PA/PB/PC/PD/X/Y block, so this is normally written straight onto
    // BG2's (or BG3's) affine registers. Unlike ObjAffineSet there's no
    // `diff` parameter - both source and destination are always tightly
    // packed at their fixed struct sizes.
    const u32 src = GetRegister(0);
    const u32 dst = GetRegister(1);
    const u32 count = GetRegister(2);

    for (u32 i = 0; i < count; ++i) {
        const u32 entrySrc = src + i * 20u;
        const auto originX = static_cast<s32>(bus_.Read32(entrySrc + 0));
        const auto originY = static_cast<s32>(bus_.Read32(entrySrc + 4));
        const auto displayX = static_cast<s32>(static_cast<s16>(bus_.Read16(entrySrc + 8)));
        const auto displayY = static_cast<s32>(static_cast<s16>(bus_.Read16(entrySrc + 10)));
        const auto sx = static_cast<s32>(static_cast<s16>(bus_.Read16(entrySrc + 12)));
        const auto sy = static_cast<s32>(static_cast<s16>(bus_.Read16(entrySrc + 14)));
        const u32 theta = bus_.Read16(entrySrc + 16) >> 8;

        const double angle = (static_cast<double>(theta) / 256.0) * 2.0 * kPi;
        const double cosA = std::cos(angle);
        const double sinA = std::sin(angle);

        const auto pa = static_cast<s16>(std::lround(sx * cosA));
        const auto pb = static_cast<s16>(std::lround(-sx * sinA));
        const auto pc = static_cast<s16>(std::lround(sy * sinA));
        const auto pd = static_cast<s16>(std::lround(sy * cosA));

        // Pivot correction: the reference point hardware starts sampling
        // from at screen pixel (0,0) has to be shifted back by the
        // rotated/scaled offset from the display center to the screen
        // origin, so the transform ends up centered on (displayX,
        // displayY) instead of the screen's top-left corner.
        const s32 startX = originX - (static_cast<s32>(pa) * displayX + static_cast<s32>(pb) * displayY);
        const s32 startY = originY - (static_cast<s32>(pc) * displayX + static_cast<s32>(pd) * displayY);

        const u32 entryDst = dst + i * 16u;
        bus_.Write16(entryDst + 0u, static_cast<u16>(pa));
        bus_.Write16(entryDst + 2u, static_cast<u16>(pb));
        bus_.Write16(entryDst + 4u, static_cast<u16>(pc));
        bus_.Write16(entryDst + 6u, static_cast<u16>(pd));
        bus_.Write32(entryDst + 8u, static_cast<u32>(startX));
        bus_.Write32(entryDst + 12u, static_cast<u32>(startY));
    }
}

void Cpu::HleSoundBias() {
    // SoundBias (SWI 0x19) - GBATEK "SWI 19h". On real hardware this
    // gradually steps SOUNDBIAS (0x04000088, bits 1-9) up or down toward
    // its target, one small increment at a time with a delay between
    // steps, purely to avoid an audible pop in the analog output when
    // sound is powered on/off. Our digital mixer never reads SOUNDBIAS
    // for anything - there's no DAC to pop - so jumping straight to the
    // final value produces the same end state (and the same value any
    // caller would read back afterward) without needing to model the
    // ramp's timing.
    const u32 biasLevel = GetRegister(0);
    bus_.Write16(mem::kIoBase + io::kSoundBias, biasLevel != 0 ? 0x0200u : 0x0000u);
}

} // namespace gba
