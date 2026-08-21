#include "core/io/dma.h"

namespace gba {

namespace {
constexpr u32 kSad[4]  = {io::kDma0Sad, io::kDma1Sad, io::kDma2Sad, io::kDma3Sad};
constexpr u32 kDad[4]  = {io::kDma0Dad, io::kDma1Dad, io::kDma2Dad, io::kDma3Dad};
constexpr u32 kCntL[4] = {io::kDma0CntL, io::kDma1CntL, io::kDma2CntL, io::kDma3CntL};
constexpr u32 kCntH[4] = {io::kDma0CntH, io::kDma1CntH, io::kDma2CntH, io::kDma3CntH};
constexpr u16 kCompleteIrq[4] = {irq::kDma0, irq::kDma1, irq::kDma2, irq::kDma3};
} // namespace

Dma::Dma(Bus& bus) : bus_(bus) {}

u32 Dma::SadAddress(int channel)  { return mem::kIoBase + kSad[channel]; }
u32 Dma::DadAddress(int channel)  { return mem::kIoBase + kDad[channel]; }
u32 Dma::CntLAddress(int channel) { return mem::kIoBase + kCntL[channel]; }
u32 Dma::CntHAddress(int channel) { return mem::kIoBase + kCntH[channel]; }

void Dma::CheckImmediate() {
    for (int ch = 0; ch < 4; ++ch) {
        const u16 control = bus_.Read16(CntHAddress(ch));
        const bool enabled = (control & (1u << 15)) != 0;
        const u32 timing = (control >> 12) & 0x3u;

        if (!enabled) {
            armed_[ch] = false;
            specialArmed_[ch] = false;
            continue;
        }

        // Special-timing (sound FIFO) source-position latch: this has to
        // happen here, polled every CPU cycle, rather than lazily inside
        // OnFifoRequest() - a disable-then-re-enable reset cycle (see
        // specialSrc_'s declaration comment) typically completes entirely
        // within one interrupt handler, faster than the next FIFO-empty
        // event that would otherwise be the only chance to notice it.
        // Missing it here meant the "disabled" state was invisible and
        // the internal read position just kept marching across a reset
        // the game intended to restart from a fresh SAD.
        if (timing == 3 && (ch == 1 || ch == 2) && !specialArmed_[ch]) {
            specialSrc_[ch] = bus_.Read32(SadAddress(ch));
            specialArmed_[ch] = true;
        }

        if (armed_[ch]) {
            continue; // already fired for this enable - VBlank/HBlank/special channels wait elsewhere
        }
        if (timing != 0) {
            continue; // not immediate timing
        }

        RunChannel(ch);
        armed_[ch] = true;
    }
}

void Dma::OnVBlank() {
    for (int ch = 0; ch < 4; ++ch) {
        const u16 control = bus_.Read16(CntHAddress(ch));
        const bool enabled = (control & (1u << 15)) != 0;
        const u32 timing = (control >> 12) & 0x3u;
        if (enabled && timing == 1) {
            RunChannel(ch);
        }
    }
}

void Dma::OnHBlank() {
    for (int ch = 0; ch < 4; ++ch) {
        const u16 control = bus_.Read16(CntHAddress(ch));
        const bool enabled = (control & (1u << 15)) != 0;
        const u32 timing = (control >> 12) & 0x3u;
        if (enabled && timing == 2) {
            RunChannel(ch);
        }
    }
}

void Dma::OnFifoRequest(u32 fifoAddress) {
    // Only DMA1 and DMA2 support Special timing for sound - GBATEK
    // "Channel A and B - DMA Sound" ("Whenever FIFO becomes half empty
    // ... a DMA Request is issued for DMA1 (Sound A) or DMA2 (Sound B)").
    // Nothing hardware-enforces channel 1 = A / 2 = B though - what
    // actually matters is which channel's destination is pointed at this
    // particular FIFO, so check both.
    //
    // See specialSrc_/specialArmed_'s declaration for why this tracks an
    // internal read position instead of just re-reading DMAxSAD fresh
    // every call: real games' sound engines rely on that register staying
    // frozen at whatever they wrote (e.g. to detect/manage buffer
    // position themselves), while the actual DMA hardware keeps streaming
    // forward through a much larger source buffer underneath.
    for (int channel = 1; channel <= 2; ++channel) {
        const u16 control = bus_.Read16(CntHAddress(channel));
        const bool enabled = (control & (1u << 15)) != 0;
        const u32 timing = (control >> 12) & 0x3u;
        if (!enabled || timing != 3) {
            specialArmed_[channel] = false;
            continue;
        }
        if (bus_.Read32(DadAddress(channel)) != fifoAddress) {
            continue; // armed, but for the other FIFO - leave its state alone
        }

        if (!specialArmed_[channel]) {
            // Normally already latched by CheckImmediate() (see its
            // comment) - this is just a safety net for the rare case
            // where a FIFO-empty event fires before CheckImmediate() gets
            // a chance to observe a same-cycle re-enable.
            specialSrc_[channel] = bus_.Read32(SadAddress(channel));
            specialArmed_[channel] = true;
        }

        u32 src = specialSrc_[channel];
        for (int i = 0; i < 4; ++i) {
            bus_.Write32(fifoAddress, bus_.Read32(src));
            src += 4u;
        }
        specialSrc_[channel] = src;
        // SAD/DAD/CNT_L are intentionally never written back - see the
        // declaration's comment.
    }
}

void Dma::RunChannel(int channel) {
    const u16 control = bus_.Read16(CntHAddress(channel));
    const bool wordTransfer = (control & (1u << 10)) != 0;
    const u32 destControl = (control >> 5) & 0x3u;
    const u32 srcControl  = (control >> 7) & 0x3u;
    const bool repeat = (control & (1u << 9)) != 0;
    const bool irqOnComplete = (control & (1u << 14)) != 0;

    u32 src = bus_.Read32(SadAddress(channel));
    u32 dst = bus_.Read32(DadAddress(channel));
    u32 count = bus_.Read16(CntLAddress(channel));
    if (count == 0) {
        // 0 means "maximum" - 0x4000 (14-bit field) for DMA0-2, 0x10000
        // (16-bit field) for DMA3. GBATEK "DMA Count Registers".
        count = (channel == 3) ? 0x1'0000u : 0x4000u;
    }

    const u32 unitSize = wordTransfer ? 4u : 2u;

    for (u32 i = 0; i < count; ++i) {
        if (wordTransfer) {
            bus_.Write32(dst, bus_.Read32(src));
        } else {
            bus_.Write16(dst, bus_.Read16(src));
        }

        switch (srcControl) {
            case 0: src += unitSize; break; // increment
            case 1: src -= unitSize; break; // decrement
            case 2: break;                  // fixed
            default: break;                 // 3 is prohibited for source; treat as fixed
        }
        switch (destControl) {
            case 0: dst += unitSize; break; // increment
            case 1: dst -= unitSize; break; // decrement
            case 2: break;                  // fixed
            default: dst += unitSize; break; // 3 = increment, reload at repeat start (see TODO below)
        }
    }

    // destControl==3 ("increment during transfer, reload to the original
    // address at the start of each repeat") is naturally handled by simply
    // *not* writing the advanced dst back to DADxxx here: the next repeat
    // trigger (HBlank/VBlank) re-reads DAD fresh at the top of this
    // function, so leaving the register untouched means it stays at
    // whatever address the game originally configured, which is exactly
    // what "reload" means. destControl 0/1/2 do need the write-back, so
    // this repeat's advanced position is what the next repeat continues
    // from.
    if (destControl != 3) {
        bus_.Write32(DadAddress(channel), dst);
    }
    // TODO: source-address persistence across repeat-triggered DMA
    // (VBlank/HBlank) isn't modeled - src is always re-read fresh from
    // SADxxx at the top of this function rather than carried forward like
    // dst is, so a repeating transfer with an incrementing source (e.g. a
    // per-scanline gradient table) restarts from the same address every
    // trigger instead of continuing through the table. This is the same
    // class of bug specialSrc_ fixes for the sound FIFO case (see dma.h) -
    // fixing it here needs the same kind of empirical verification against
    // real games rather than a blind port of that fix.

    if (!repeat) {
        // Real hardware clears the enable bit itself once a non-repeating
        // transfer finishes.
        const u16 clearedControl = control & static_cast<u16>(~(1u << 15));
        bus_.Write16(CntHAddress(channel), clearedControl);
    }

    if (irqOnComplete) {
        bus_.RequestInterrupt(kCompleteIrq[channel]);
    }
}

} // namespace gba
