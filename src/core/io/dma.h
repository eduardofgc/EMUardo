#pragma once

#include "core/memory/bus.h"
#include "core/types.h"

namespace gba {

// The GBA's four DMA channels (DMA0-3), each able to copy a block of
// memory without CPU involvement. GBATEK "DMA Transfer Channels".
//
// Real hardware transfers happen mid-instruction and briefly halt the CPU;
// we don't model that timing - instead, CheckImmediate()/OnVBlank() run a
// channel's *entire* transfer in one shot as soon as its trigger condition
// is met. That's a real simplification (a long DMA should take visible
// time and the CPU shouldn't run during it), but it gets the actual data
// movement correct, which is what most games' logic depends on.
class Dma {
public:
    explicit Dma(Bus& bus);

    // Checks all four channels for one that's enabled with Immediate start
    // timing and hasn't run yet, and executes it. Call every CPU cycle (or
    // at least often) so immediate DMA looks instantaneous to game code.
    void CheckImmediate();

    // Runs any channel enabled with VBlank start timing. Call once, right
    // as the PPU enters the VBlank period.
    //
    // TODO: HBlank isn't implemented yet - it needs the PPU to actually be
    // scanline-driven first (see ppu.h's TODO on Step()), since right now
    // a whole frame renders in one shot at VBlank. Special timing's other
    // use (DMA3 video capture) isn't implemented either - only the sound
    // FIFO case (below) is.
    void OnVBlank();

    // The one Special-timing case that's implemented: Direct Sound's
    // FIFO_A/FIFO_B ask for a refill once they run low. Apu calls this
    // (via a callback wired up in Emulator) with the FIFO's address -
    // whichever of DMA1/DMA2 is enabled with Special timing and has its
    // destination set to that address runs a fixed 4-word (16-byte)
    // transfer, per GBATEK "Channel A and B - DMA Sound". Unlike
    // RunChannel(), this doesn't touch the count register or clear the
    // enable bit - the transfer repeats every time the FIFO empties out,
    // for as long as the game leaves the channel configured this way.
    void OnFifoRequest(u32 fifoAddress);

private:
    Bus& bus_;

    // Tracks which channels have already fired for their current enable
    // (so CheckImmediate() triggers once per enable-bit transition, not
    // every single cycle it stays set).
    bool armed_[4]{};

    static u32 SadAddress(int channel);
    static u32 DadAddress(int channel);
    static u32 CntLAddress(int channel);
    static u32 CntHAddress(int channel);

    void RunChannel(int channel);
};

} // namespace gba
