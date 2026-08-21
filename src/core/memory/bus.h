#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "core/memory/gpio.h"
#include "core/memory/save.h"
#include "core/state.h"
#include "core/types.h"

namespace gba {

// The Bus is the single point every memory access goes through - CPU
// fetches/loads/stores, and later the PPU/DMA controller reading and
// writing VRAM/OAM/palette RAM. Keeping this centralized (rather than
// letting CPU poke at PPU internals directly) is what makes it possible
// to add DMA and open-bus behavior later without a rewrite.
class Bus {
public:
    Bus();

    // Loads a .gba ROM file into cartridge memory. Returns false on
    // failure (bad path, exceeds max cartridge size, etc).
    bool LoadRom(const std::string& path);

    // Width-specific accessors. The GBA CPU (ARM7TDMI) supports byte,
    // halfword, and word accesses, each with distinct alignment and
    // rotation-on-misalignment rules we'll implement per GBATEK.
    u8  Read8(u32 address) const;
    u16 Read16(u32 address) const;
    u32 Read32(u32 address) const;

    void Write8(u32 address, u8 value);
    void Write16(u32 address, u16 value);
    void Write32(u32 address, u32 value);

    // Sets a bit in the IF register directly, bypassing the write-1-to-
    // clear semantics that apply to CPU writes at that address (see
    // Write8's special case) - hardware raising a flag is a plain OR.
    // Takes one of the gba::irq:: bit constants.
    void RequestInterrupt(u16 flagBit);

    // Updates KEYINPUT to reflect which buttons are currently held, and
    // fires the keypad interrupt if KEYCNT's condition is satisfied. Takes
    // a mask built from gba::key:: bits (1 = pressed) - the active-LOW
    // inversion for the actual register happens inside.
    void SetKeyState(u16 pressedMask);

    // Writes the cartridge save data to disk if it's changed since the
    // last flush (cheap to call every frame - it's a no-op otherwise). No
    // save chip was detected (or no ROM is loaded) is not an error.
    bool FlushSave();

    // Routes FIFO_A/FIFO_B writes (0x040000A0/0x040000A4) to Apu instead
    // of storing them as plain memory - Bus can't hold an Apu directly
    // (Apu holds a Bus&, so that header dependency would be circular), so
    // Emulator wires this up with plain callbacks once both exist, the
    // same pattern used for Timers' and Dma's cross-links to Apu.
    void SetFifoPushCallbacks(std::function<void(u32)> fifoA, std::function<void(u32)> fifoB) {
        fifoAPush_ = std::move(fifoA);
        fifoBPush_ = std::move(fifoB);
    }

    // Save-state support. Deliberately NOT included: bios_ (fixed at
    // construction by InstallHleBios(), never modified afterward - a
    // fresh Bus already has the right content), rom_ (the multi-MB
    // cartridge image - LoadRom() re-reads it from the same file path
    // before LoadState() runs), savePath_ (recomputed by LoadRom() too),
    // and fifoAPush_/fifoBPush_ (rewired by Emulator's constructor).
    void SaveState(StateWriter& w) const;
    void LoadState(StateReader& r);

private:
    static constexpr std::size_t kBiosSize    = 16 * 1024;
    static constexpr std::size_t kEwramSize   = 256 * 1024;
    static constexpr std::size_t kIwramSize   = 32 * 1024;
    static constexpr std::size_t kIoSize      = 1 * 1024; // more than the real ~0x400 I/O area needs, rounds up cleanly
    static constexpr std::size_t kPaletteSize = 1 * 1024;
    static constexpr std::size_t kVramSize    = 96 * 1024;
    static constexpr std::size_t kOamSize     = 1 * 1024;
    static constexpr std::size_t kMaxRomSize  = 32 * 1024 * 1024;

    std::array<u8, kBiosSize>    bios_{};
    std::array<u8, kEwramSize>   ewram_{};
    std::array<u8, kIwramSize>   iwram_{};
    std::array<u8, kIoSize>      io_{};
    std::array<u8, kPaletteSize> palette_{};
    std::array<u8, kVramSize>    vram_{};
    std::array<u8, kOamSize>     oam_{};
    std::vector<u8>              rom_;

    SaveMemory save_;
    std::string savePath_; // derived from the loaded ROM's path (same name, .sav extension)
    Gpio gpio_;
    std::function<void(u32)> fifoAPush_;
    std::function<void(u32)> fifoBPush_;

    // We don't have (and can't ship) a real GBA BIOS ROM dump - it's
    // Nintendo's copyrighted firmware. Instead, this writes a small,
    // hand-assembled IRQ dispatch trampoline into the BIOS region at boot,
    // mimicking just enough of real BIOS behavior for interrupts to
    // actually reach a game's registered handler: saves r0-r3/r12/lr,
    // calls the handler address the game stored at 0x03007FFC (the
    // standard devkitARM/libgba convention), and returns via
    // "SUBS PC,LR,#4". Most SWI (BIOS call) numbers are instead HLE'd
    // directly in Cpu - see Cpu::TryHleSwi() - since those are easier to
    // implement natively in C++ than as hand-assembled ARM.
    void InstallHleBios();

    // TODO: io_ is currently a flat byte array with no read-side-effect
    // handling (e.g. reading KEYINPUT won't reflect real input yet) -
    // that's the keypad-input milestone.
};

} // namespace gba
