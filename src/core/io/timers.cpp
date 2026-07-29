#include "core/io/timers.h"

namespace gba {

namespace {
constexpr u32 kCntL[4] = {io::kTm0CntL, io::kTm1CntL, io::kTm2CntL, io::kTm3CntL};
constexpr u32 kCntH[4] = {io::kTm0CntH, io::kTm1CntH, io::kTm2CntH, io::kTm3CntH};
constexpr u16 kOverflowIrq[4] = {irq::kTimer0, irq::kTimer1, irq::kTimer2, irq::kTimer3};
} // namespace

Timers::Timers(Bus& bus) : bus_(bus) {}

u32 Timers::CntLAddress(int index) { return mem::kIoBase + kCntL[index]; }
u32 Timers::CntHAddress(int index) { return mem::kIoBase + kCntH[index]; }

int Timers::PrescalerCycles(u16 controlValue) {
    switch (controlValue & 0x3u) {
        case 0: return 1;
        case 1: return 64;
        case 2: return 256;
        default: return 1024;
    }
}

void Timers::Tick(int cycles) {
    for (int i = 0; i < 4; ++i) {
        const u16 control = bus_.Read16(CntHAddress(i));
        const bool running = (control & (1u << 7)) != 0;

        if (!running) {
            wasRunning_[i] = false;
            continue;
        }
        if (!wasRunning_[i]) {
            reload_[i] = bus_.Read16(CntLAddress(i));
            counter_[i] = reload_[i];
            prescalerCounter_[i] = 0;
            wasRunning_[i] = true;
        }

        // Cascade-mode timers (bit2, only meaningful for TM1-3) don't run
        // off their own prescaler at all - they're driven entirely by
        // CascadeInto() when the previous timer overflows.
        const bool cascade = (i != 0) && ((control & (1u << 2)) != 0);
        if (cascade) {
            continue;
        }

        prescalerCounter_[i] += cycles;
        const int step = PrescalerCycles(control);
        bool changed = false;
        while (prescalerCounter_[i] >= step) {
            prescalerCounter_[i] -= step;
            changed = true;
            if (TickOne(i)) {
                CascadeInto(i + 1);
            }
        }
        if (changed) {
            bus_.Write16(CntLAddress(i), counter_[i]);
        }
    }
}

void Timers::CascadeInto(int index) {
    if (index >= 4) {
        return;
    }
    const u16 control = bus_.Read16(CntHAddress(index));
    const bool running = (control & (1u << 7)) != 0;
    const bool cascade = (control & (1u << 2)) != 0;
    if (!running || !cascade) {
        return;
    }
    if (!wasRunning_[index]) {
        reload_[index] = bus_.Read16(CntLAddress(index));
        counter_[index] = reload_[index];
        wasRunning_[index] = true;
    }

    const bool overflowed = TickOne(index);
    bus_.Write16(CntLAddress(index), counter_[index]);
    if (overflowed) {
        CascadeInto(index + 1);
    }
}

bool Timers::TickOne(int index) {
    counter_[index]++;
    if (counter_[index] != 0) {
        return false;
    }

    // Overflow: reload from the value latched at start (NOT from CNT_L
    // directly - by now CNT_L just mirrors the live counter for CPU
    // reads, see the class comment in timers.h) and fire the IRQ if
    // TMxCNT_H's IRQ-enable bit (bit6) is set.
    counter_[index] = reload_[index];
    const u16 control = bus_.Read16(CntHAddress(index));
    if (control & (1u << 6)) {
        bus_.RequestInterrupt(kOverflowIrq[index]);
    }
    return true;
}

} // namespace gba
