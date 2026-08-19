// Covers Direct Sound (the DMA-fed PCM channels): FIFO push/pop ordering,
// timer-driven sample popping, stereo mixing per SOUNDCNT_H, master
// enable, and the DMA refill Apu triggers once a FIFO runs half-empty.

#include <cstdio>
#include <vector>

#include "core/apu/apu.h"
#include "core/io/dma.h"
#include "core/memory/bus.h"
#include "core/types.h"

namespace {

int failures = 0;

void Check(bool condition, const char* label) {
    if (!condition) {
        std::printf("FAIL: %s\n", label);
        ++failures;
    }
}

// ---------------------------------------------------------------------
// Test 1: pushing one sample into FIFO A, popping it via a timer overflow,
// and mixing it produces the expected scaled stereo output - left only
// (right channel disabled), full volume.
// ---------------------------------------------------------------------
void TestBasicMixing() {
    gba::Bus bus;
    gba::Apu apu(bus);
    bus.SetFifoPushCallbacks(
        [&apu](gba::u32 value) { apu.PushFifoA(value); },
        [&apu](gba::u32 value) { apu.PushFifoB(value); });

    // SOUNDCNT_H: bit2=1 (channel A 100% volume), bit9=1 (A->left),
    // bit8=0 (A->right disabled), bit10=0 (A driven by timer 0).
    bus.Write16(gba::mem::kIoBase + gba::io::kSoundCntH, (1u << 2) | (1u << 9));
    // SOUNDCNT_X bit7 = master sound enable.
    bus.Write16(gba::mem::kIoBase + gba::io::kSoundCntX, 1u << 7);

    bus.Write32(gba::mem::kIoBase + gba::io::kFifoA, 0x0000'0001u); // pushes sample +1 first
    apu.OnTimerOverflow(0); // pops it - currentA_ = +1

    apu.GenerateSample();
    const std::vector<gba::s16> samples = apu.DrainSamples();
    Check(samples.size() == 2, "Basic mixing: one sample pair produced");
    if (samples.size() == 2) {
        Check(samples[0] == 256, "Basic mixing: left channel is +1/128 scaled to s16 (256)");
        Check(samples[1] == 0, "Basic mixing: right channel is silent (A->right disabled)");
    }
}

// ---------------------------------------------------------------------
// Test 2: master disable (SOUNDCNT_X bit7=0) forces silence even with a
// queued, enabled channel.
// ---------------------------------------------------------------------
void TestMasterDisable() {
    gba::Bus bus;
    gba::Apu apu(bus);
    bus.SetFifoPushCallbacks(
        [&apu](gba::u32 value) { apu.PushFifoA(value); },
        [&apu](gba::u32 value) { apu.PushFifoB(value); });

    bus.Write16(gba::mem::kIoBase + gba::io::kSoundCntH, (1u << 2) | (1u << 8) | (1u << 9));
    // SOUNDCNT_X left at 0 - master sound disabled.
    bus.Write32(gba::mem::kIoBase + gba::io::kFifoA, 0x0000'007Fu); // +127, max amplitude
    apu.OnTimerOverflow(0);

    apu.GenerateSample();
    const std::vector<gba::s16> samples = apu.DrainSamples();
    Check(samples.size() == 2 && samples[0] == 0 && samples[1] == 0,
          "Master disable: silent output despite a loud queued sample");
}

// ---------------------------------------------------------------------
// Test 3: a timer overflow for the *other* index doesn't pop anything -
// only the timer actually selected in SOUNDCNT_H should advance the FIFO.
// ---------------------------------------------------------------------
void TestOnlySelectedTimerPops() {
    gba::Bus bus;
    gba::Apu apu(bus);
    bus.SetFifoPushCallbacks(
        [&apu](gba::u32 value) { apu.PushFifoA(value); },
        [&apu](gba::u32 value) { apu.PushFifoB(value); });

    bus.Write16(gba::mem::kIoBase + gba::io::kSoundCntH, (1u << 2) | (1u << 9)); // timer 0 selected for A
    bus.Write16(gba::mem::kIoBase + gba::io::kSoundCntX, 1u << 7);
    bus.Write32(gba::mem::kIoBase + gba::io::kFifoA, 0x0000'0001u);

    apu.OnTimerOverflow(1); // wrong timer - should be ignored
    apu.GenerateSample();
    std::vector<gba::s16> samples = apu.DrainSamples();
    Check(samples.size() == 2 && samples[0] == 0,
          "Wrong timer: FIFO untouched, DAC still holds its power-on-zero value");

    apu.OnTimerOverflow(0); // right timer now
    apu.GenerateSample();
    samples = apu.DrainSamples();
    Check(samples.size() == 2 && samples[0] == 256,
          "Right timer: FIFO popped, DAC now holds the queued sample");
}

// ---------------------------------------------------------------------
// Test 4: once FIFO A drops to half (16 bytes), Apu asks Dma to refill it
// - a DMA1 channel enabled with Special timing and its destination
// pointed at FIFO_A should top it back up, and the newly-arrived samples
// should be reachable by further pops once the original 32 bytes are
// exhausted.
// ---------------------------------------------------------------------
void TestDmaRefill() {
    gba::Bus bus;
    gba::Apu apu(bus);
    gba::Dma dma(bus);
    bus.SetFifoPushCallbacks(
        [&apu](gba::u32 value) { apu.PushFifoA(value); },
        [&apu](gba::u32 value) { apu.PushFifoB(value); });
    apu.SetDmaRefillCallback([&dma](gba::u32 fifoAddress) { dma.OnFifoRequest(fifoAddress); });

    bus.Write16(gba::mem::kIoBase + gba::io::kSoundCntH, (1u << 2) | (1u << 9)); // A, full vol, left, timer 0
    bus.Write16(gba::mem::kIoBase + gba::io::kSoundCntX, 1u << 7);

    // Fill FIFO A to capacity (32 bytes) with sample value +1.
    for (int i = 0; i < 8; ++i) {
        bus.Write32(gba::mem::kIoBase + gba::io::kFifoA, 0x0101'0101u);
    }

    // DMA1: source is 4 words of sample value +2, destination fixed at
    // FIFO_A, Special timing (bits12-13=11), enabled.
    const gba::u32 refillSrc = gba::mem::kEwramBase + 0x100;
    for (int i = 0; i < 4; ++i) {
        bus.Write32(refillSrc + static_cast<gba::u32>(i) * 4, 0x0202'0202u);
    }
    bus.Write32(gba::mem::kIoBase + gba::io::kDma1Sad, refillSrc);
    bus.Write32(gba::mem::kIoBase + gba::io::kDma1Dad, gba::mem::kIoBase + gba::io::kFifoA);
    bus.Write16(gba::mem::kIoBase + gba::io::kDma1CntH, (1u << 15) | (0x3u << 12));

    // Pop all 32 original samples (crossing the half-empty threshold at
    // pop #16, which should trigger exactly one 16-byte refill).
    for (int i = 0; i < 32; ++i) {
        apu.OnTimerOverflow(0);
        apu.GenerateSample();
        const std::vector<gba::s16> samples = apu.DrainSamples();
        if (samples.size() == 2 && samples[0] != 256) {
            char label[96];
            std::snprintf(label, sizeof(label),
                          "DMA refill: pop #%d still from the original +1 batch", i + 1);
            Check(false, label);
            return;
        }
    }

    // The 33rd pop should now be reading from the refilled batch (+2).
    apu.OnTimerOverflow(0);
    apu.GenerateSample();
    const std::vector<gba::s16> samples = apu.DrainSamples();
    Check(samples.size() == 2 && samples[0] == 512,
          "DMA refill: pop #33 reads the DMA-refilled +2 sample (512 scaled)");
}

} // namespace

int main() {
    TestBasicMixing();
    TestMasterDisable();
    TestOnlySelectedTimerPops();
    TestDmaRefill();

    if (failures == 0) {
        std::printf("PASS: Direct Sound FIFO mixing, master enable, DMA refill\n");
    }
    return failures;
}
