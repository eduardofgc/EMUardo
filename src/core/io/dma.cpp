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
            continue;
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

    bus_.Write32(DadAddress(channel), dst);
    // TODO: destControl==3 ("increment during transfer, reload to the
    // original address at the start of each repeat") isn't fully modeled -
    // we advance normally but never reload, since repeat-triggered re-runs
    // (HBlank/audio-FIFO-style DMA) aren't implemented yet either. Worth
    // revisiting together once HBlank timing lands.

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
