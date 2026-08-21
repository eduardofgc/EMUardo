#include "core/emulator.h"

namespace gba {

namespace {
constexpr int kScanlinesPerFrame = 228; // 160 visible + 68 VBlank
constexpr int kVisibleScanlines  = 160;
// 1232 cycles/line = 308 dots/line * 4 cycles/dot (GBATEK "General LCD
// Status"), split into the 240-dot draw portion and 68-dot HBlank portion.
constexpr int kDrawCycles        = 960;
constexpr int kHBlankCycles      = 272;
constexpr int kCyclesPerScanline = kDrawCycles + kHBlankCycles;
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
    // Genuinely scanline-driven: each of the 228 lines runs its 960-cycle
    // draw portion, gets rendered (visible lines only) and flagged into
    // HBlank, then runs its 68-dot/272-cycle HBlank portion - rather than
    // rendering the whole frame in one shot at VBlank. This is still not
    // truly dot-accurate (a line's rendering uses whatever register state
    // is current right as its draw portion ends, rather than tracking
    // per-dot), but it's enough for mid-frame register writes - scroll,
    // palette, window/blend, and the BG2X/Y "raster split" trick - to take
    // effect on the correct line instead of only showing up next frame.
    for (int line = 0; line < kScanlinesPerFrame; ++line) {
        bus_.Write16(mem::kIoBase + io::kVcount, static_cast<u16>(line));

        u16 dispstat = bus_.Read16(mem::kIoBase + io::kDispstat);
        dispstat = static_cast<u16>(dispstat & ~0x2u); // clear HBlank flag (bit1) - set again below once this line's draw portion ends

        const bool enteringVBlank = (line == kVisibleScanlines);
        if (enteringVBlank) {
            dispstat = static_cast<u16>(dispstat | 0x1u); // VBlank flag (bit0)
        } else if (line == 0) {
            dispstat = static_cast<u16>(dispstat & ~0x1u); // clear VBlank flag for the new frame
        }

        // VCount match (GBATEK "General LCD Status": bits8-15 = the game's
        // configured LYC value, bit2 = match flag, set/cleared fresh every
        // line rather than latched).
        const u8 lyc = static_cast<u8>(dispstat >> 8);
        const bool vcountMatch = (static_cast<u8>(line) == lyc);
        dispstat = vcountMatch ? static_cast<u16>(dispstat | 0x4u) : static_cast<u16>(dispstat & ~0x4u);
        bus_.Write16(mem::kIoBase + io::kDispstat, dispstat);

        if (enteringVBlank && (dispstat & (1u << 3))) { // VBlank IRQ enable
            bus_.RequestInterrupt(irq::kVBlank);
        }
        if (vcountMatch && (dispstat & (1u << 5))) { // VCount IRQ enable
            bus_.RequestInterrupt(irq::kVCount);
        }
        if (enteringVBlank) {
            dma_.OnVBlank();
            bus_.FlushSave();
        }

        auto runCycles = [&](int budget) {
            int cyclesRun = 0;
            while (cyclesRun < budget) {
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
        };

        runCycles(kDrawCycles);

        if (line < kVisibleScanlines) {
            ppu_.RenderScanline(line);
        }

        u16 hblankStatus = bus_.Read16(mem::kIoBase + io::kDispstat);
        hblankStatus = static_cast<u16>(hblankStatus | 0x2u); // HBlank flag (bit1)
        bus_.Write16(mem::kIoBase + io::kDispstat, hblankStatus);
        if (hblankStatus & (1u << 4)) { // HBlank IRQ enable
            bus_.RequestInterrupt(irq::kHBlank);
        }
        // HBlank fires - and HBlank-timed DMA can trigger - for all 228
        // lines, not just the 160 visible ones; the signal itself doesn't
        // distinguish visible from VBlank scanlines on real hardware.
        dma_.OnHBlank();

        runCycles(kHBlankCycles);
    }
}

} // namespace gba
