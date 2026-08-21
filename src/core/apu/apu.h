#pragma once

#include <array>
#include <functional>
#include <vector>

#include "core/memory/bus.h"
#include "core/state.h"
#include "core/types.h"

namespace gba {

// Direct Sound: the GBA's two DMA-fed 8-bit PCM channels (FIFO_A/FIFO_B).
// Most commercial games' music/SFX engines (including everything tested
// against this emulator so far) drive their audio through these rather
// than the four legacy Game-Boy-style tone/noise channels - those aren't
// implemented yet, so SOUNDCNT_L (their mix register) isn't read here.
//
// GBATEK "Channel A and B - DMA Sound": each FIFO holds up to 32 bytes of
// signed 8-bit samples, pushed 4 at a time by a 32-bit write to
// FIFO_A/FIFO_B; a selected Timer's overflow pops exactly one byte per
// channel for playback; once a FIFO drops to 16 bytes (half), a DMA
// request goes out (see Dma::OnFifoRequest) asking for another 16-byte
// refill. This class owns the FIFOs and the "last popped sample" DAC
// state per channel, and mixes that into stereo output on demand.
class Apu {
public:
    explicit Apu(Bus& bus);

    // Wired to Dma::OnFifoRequest by Emulator - called with the FIFO's
    // I/O address (mem::kIoBase + io::kFifoA/kFifoB) once a FIFO drops to
    // half capacity, so Dma knows which destination to look for among
    // DMA1/DMA2's Special-timing channels.
    void SetDmaRefillCallback(std::function<void(u32)> callback) {
        dmaRefillCallback_ = std::move(callback);
    }

    // Called by Bus when the CPU writes FIFO_A/FIFO_B (a 32-bit write
    // pushes up to 4 new samples - see Bus::Write32's special case).
    void PushFifoA(u32 value) { PushFifo(fifoA_, fifoALen_, value); }
    void PushFifoB(u32 value) { PushFifo(fifoB_, fifoBLen_, value); }

    // Wired to Timers::SetOverflowCallback by Emulator - pops one sample
    // from whichever FIFO(s) currently have this timer selected in
    // SOUNDCNT_H as their playback clock.
    void OnTimerOverflow(int timerIndex);

    // Mixes the two channels' current DAC values into one stereo sample
    // pair and appends it to the output buffer. Call every 512 CPU
    // cycles (16.78MHz / 512 = 32768Hz, the GBA's standard Direct Sound
    // output rate and SOUNDBIAS's power-on default sampling cycle).
    void GenerateSample();

    // Hands over every sample accumulated since the last call (as
    // interleaved L,R signed 16-bit pairs) and clears the internal
    // buffer - main.cpp drains this once per host frame to feed SDL's
    // audio queue.
    std::vector<s16> DrainSamples();

    // Save-state support. outputBuffer_ (samples generated but not yet
    // drained by main.cpp) deliberately isn't included - it's at most one
    // frame's worth (~1/60s), and dropping it across a save/load is
    // inaudible, not worth the bookkeeping. dmaRefillCallback_ isn't
    // serializable either - rewired by Emulator's constructor.
    void SaveState(StateWriter& w) const;
    void LoadState(StateReader& r);

private:
    Bus& bus_;

    // Both FIFOs are modeled as plain byte queues (max 32 bytes, so a
    // shifting pop is cheap) rather than a ring buffer - simplicity over
    // micro-optimization for a buffer this small.
    std::array<s8, 32> fifoA_{};
    int fifoALen_ = 0;
    s8 currentA_ = 0;

    std::array<s8, 32> fifoB_{};
    int fifoBLen_ = 0;
    s8 currentB_ = 0;

    std::vector<s16> outputBuffer_;
    std::function<void(u32)> dmaRefillCallback_;

    static void PushFifo(std::array<s8, 32>& fifo, int& len, u32 value);
    void PopFifo(std::array<s8, 32>& fifo, int& len, s8& current, u32 fifoAddress);
};

} // namespace gba
