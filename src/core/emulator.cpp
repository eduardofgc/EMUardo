#include "core/emulator.h"

namespace gba {

namespace {
constexpr int kScanlinesPerFrame = 228; // 160 visible + 68 VBlank
constexpr int kVisibleScanlines  = 160;
constexpr int kCyclesPerScanline = 1232;
constexpr int kCyclesPerAudioSample = 512; // 16.78MHz / 512 = 32768Hz
} // namespace

Emulator::Emulator() : cpu_(bus_), ppu_(bus_), timers_(bus_), dma_(bus_), apu_(bus_) {
    // Cross-links between peripherals that each only hold a Bus& - see
    // the comments on Timers::SetOverflowCallback, Apu::SetDmaRefillCallback
    // and Bus::SetFifoPushCallbacks for why these go through callbacks
    // instead of direct references.
    timers_.SetOverflowCallback([this](int timerIndex) { apu_.OnTimerOverflow(timerIndex); });
    apu_.SetDmaRefillCallback([this](u32 fifoAddress) { dma_.OnFifoRequest(fifoAddress); });
    bus_.SetFifoPushCallbacks(
        [this](u32 value) { apu_.PushFifoA(value); },
        [this](u32 value) { apu_.PushFifoB(value); });
}

bool Emulator::LoadRom(const std::string& path) {
    return bus_.LoadRom(path);
}

void Emulator::RunFrame() {
    // Placeholder scanline timing: a fixed cycle budget per line rather
    // than genuinely tracking PPU dot position. Once timers/DMA/PPU all
    // need tighter synchronization than "once per scanline", this needs
    // to become real cycle-accurate scheduling.
    for (int line = 0; line < kScanlinesPerFrame; ++line) {
        bus_.Write16(mem::kIoBase + io::kVcount, static_cast<u16>(line));

        const bool enteringVBlank = (line == kVisibleScanlines);
        if (enteringVBlank) {
            u16 dispstat = bus_.Read16(mem::kIoBase + io::kDispstat);
            dispstat = static_cast<u16>(dispstat | 0x1u); // VBlank flag (bit0)
            bus_.Write16(mem::kIoBase + io::kDispstat, dispstat);

            if (dispstat & (1u << 3)) { // VBlank IRQ enable
                bus_.RequestInterrupt(irq::kVBlank);
            }

            // The PPU isn't scanline-driven yet (see ppu.h's TODO), so
            // "the frame is ready" and "we just entered VBlank" are the
            // same moment for now - rendering here also means a VBlank
            // interrupt handler that reads VRAM/OAM sees this frame's
            // finished image, matching what real game code expects.
            ppu_.RenderFrame();
            dma_.OnVBlank();
            bus_.FlushSave();
        } else if (line == 0) {
            u16 dispstat = bus_.Read16(mem::kIoBase + io::kDispstat);
            dispstat = static_cast<u16>(dispstat & ~0x1u); // clear VBlank flag for the new frame
            bus_.Write16(mem::kIoBase + io::kDispstat, dispstat);
        }

        int cyclesRun = 0;
        while (cyclesRun < kCyclesPerScanline) {
            const int cycles = cpu_.Step();
            cyclesRun += cycles;
            timers_.Tick(cycles);
            dma_.CheckImmediate();

            cyclesSinceLastSample_ += cycles;
            while (cyclesSinceLastSample_ >= kCyclesPerAudioSample) {
                apu_.GenerateSample();
                cyclesSinceLastSample_ -= kCyclesPerAudioSample;
            }
        }
        ppu_.Step();
    }
}

} // namespace gba
