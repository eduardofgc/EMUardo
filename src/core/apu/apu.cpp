#include "core/apu/apu.h"

#include <algorithm>
#include <cmath>

namespace gba {

Apu::Apu(Bus& bus) : bus_(bus) {}

void Apu::SaveState(StateWriter& w) const {
    w.Write(fifoA_);
    w.Write(fifoALen_);
    w.Write(currentA_);
    w.Write(fifoB_);
    w.Write(fifoBLen_);
    w.Write(currentB_);
}

void Apu::LoadState(StateReader& r) {
    r.Read(fifoA_);
    r.Read(fifoALen_);
    r.Read(currentA_);
    r.Read(fifoB_);
    r.Read(fifoBLen_);
    r.Read(currentB_);
}

void Apu::PushFifo(std::array<s8, 32>& fifo, int& len, u32 value) {
    // "Data 0-3, Data 0 being located in least significant byte which is
    // replayed first" - GBATEK "Channel A and B - DMA Sound".
    for (int i = 0; i < 4 && len < static_cast<int>(fifo.size()); ++i) {
        fifo[static_cast<std::size_t>(len)] = static_cast<s8>((value >> (i * 8)) & 0xFFu);
        ++len;
    }
}

void Apu::PopFifo(std::array<s8, 32>& fifo, int& len, s8& current, u32 fifoAddress) {
    if (len == 0) {
        return; // nothing queued - hold the last DAC value, matching real hardware
    }
    current = fifo[0];
    --len;
    for (int i = 0; i < len; ++i) {
        fifo[static_cast<std::size_t>(i)] = fifo[static_cast<std::size_t>(i + 1)];
    }

    // GBATEK: "If FIFO contains only 4 x 32bit (16 bytes) then Request
    // more data per DMA" - i.e. a refill request fires once the FIFO
    // drops to half capacity.
    if (len <= 16 && dmaRefillCallback_) {
        dmaRefillCallback_(fifoAddress);
    }
}

void Apu::OnTimerOverflow(int timerIndex) {
    const u16 soundCntH = bus_.Read16(mem::kIoBase + io::kSoundCntH);
    const int timerA = (soundCntH & (1u << 10)) ? 1 : 0;
    const int timerB = (soundCntH & (1u << 14)) ? 1 : 0;

    if (timerIndex == timerA) {
        PopFifo(fifoA_, fifoALen_, currentA_, mem::kIoBase + io::kFifoA);
    }
    if (timerIndex == timerB) {
        PopFifo(fifoB_, fifoBLen_, currentB_, mem::kIoBase + io::kFifoB);
    }
}

void Apu::GenerateSample() {
    const u16 soundCntH = bus_.Read16(mem::kIoBase + io::kSoundCntH);

    // The reset-FIFO bits are one-shot write strobes on real hardware -
    // games commonly set them once as part of an initial "configure and
    // reset" write and never touch this register again, so they can't be
    // treated as a level to re-check every sample (that would silently
    // wipe both FIFOs empty on every single tick forever). Clear the bit
    // back out immediately after acting on it so it only fires once, the
    // same way real hardware's strobe behaves.
    u16 updatedSoundCntH = soundCntH;
    if (soundCntH & (1u << 11)) {
        fifoALen_ = 0;
        updatedSoundCntH &= static_cast<u16>(~(1u << 11));
    }
    if (soundCntH & (1u << 15)) {
        fifoBLen_ = 0;
        updatedSoundCntH &= static_cast<u16>(~(1u << 15));
    }
    if (updatedSoundCntH != soundCntH) {
        bus_.Write16(mem::kIoBase + io::kSoundCntH, updatedSoundCntH);
    }

    const u16 soundCntX = bus_.Read16(mem::kIoBase + io::kSoundCntX);
    const bool masterEnable = (soundCntX & (1u << 7)) != 0;

    s16 left = 0;
    s16 right = 0;
    if (masterEnable) {
        const double volA = (soundCntH & (1u << 2)) ? 1.0 : 0.5;
        const double volB = (soundCntH & (1u << 3)) ? 1.0 : 0.5;
        const bool aRight = (soundCntH & (1u << 8)) != 0;
        const bool aLeft  = (soundCntH & (1u << 9)) != 0;
        const bool bRight = (soundCntH & (1u << 12)) != 0;
        const bool bLeft  = (soundCntH & (1u << 13)) != 0;

        const double sampleA = (static_cast<double>(currentA_) / 128.0) * volA;
        const double sampleB = (static_cast<double>(currentB_) / 128.0) * volB;

        double mixLeft = 0.0;
        double mixRight = 0.0;
        if (aLeft) mixLeft += sampleA;
        if (aRight) mixRight += sampleA;
        if (bLeft) mixLeft += sampleB;
        if (bRight) mixRight += sampleB;

        left = static_cast<s16>(std::lround(std::clamp(mixLeft, -1.0, 1.0) * 32767.0));
        right = static_cast<s16>(std::lround(std::clamp(mixRight, -1.0, 1.0) * 32767.0));
    }

    outputBuffer_.push_back(left);
    outputBuffer_.push_back(right);
}

std::vector<s16> Apu::DrainSamples() {
    std::vector<s16> drained;
    drained.swap(outputBuffer_);
    return drained;
}

} // namespace gba
