#pragma once

#include <functional>

#include "core/memory/bus.h"
#include "core/types.h"

namespace gba {

// The GBA's four hardware timers (TM0-TM3). Each is a free-running 16-bit
// counter with a selectable prescaler, an optional "cascade" mode where it
// only increments when the previous timer overflows (instead of on its own
// prescaler), and an optional IRQ on overflow. GBATEK "Timer Registers".
//
// TMxCNT_L is dual-purpose on real hardware: writing it sets the reload
// value used on the *next* overflow, but reading it returns the *live
// counter*. A flat memory array can't represent that by itself, which is
// why this class keeps its own reload_/counter_ state and only uses
// TMxCNT_L in the I/O region as a mirror for the CPU to read - see Tick()
// and TickOne().
class Timers {
public:
    explicit Timers(Bus& bus);

    // Advances all four timers by `cycles` CPU cycles. Call once per
    // Cpu::Step(), passing that call's returned cycle count.
    void Tick(int cycles);

    // Direct Sound's FIFO_A/FIFO_B channels each pop one sample whenever
    // their selected timer (0 or 1, per SOUNDCNT_H) overflows - this is
    // how Apu finds out that happened, since Apu has no other way to
    // observe a specific timer's overflow moment (only the *result*,
    // e.g. the IRQ it may also raise).
    void SetOverflowCallback(std::function<void(int)> callback) {
        overflowCallback_ = std::move(callback);
    }

private:
    Bus& bus_;

    u16 counter_[4]{};
    u16 reload_[4]{};
    bool wasRunning_[4]{}; // detects the start bit's 0->1 transition, which is when reload_ gets (re)latched
    int prescalerCounter_[4]{};
    std::function<void(int)> overflowCallback_;

    static u32 CntLAddress(int index);
    static u32 CntHAddress(int index);
    static int PrescalerCycles(u16 controlValue);

    // Increments one timer by exactly one tick. Returns true on overflow
    // (reload + IRQ already handled), so the caller can cascade into the
    // next timer if that one's in count-up mode.
    bool TickOne(int index);

    // Recursively drives cascade-mode timers: if `index`'s timer is
    // running with its count-up bit set, ticks it once (from the previous
    // timer's overflow) and, on overflow, cascades into index+1.
    void CascadeInto(int index);
};

} // namespace gba
