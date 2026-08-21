#pragma once

#include <string>
#include <vector>

#include "core/state.h"
#include "core/types.h"

namespace gba {

// Emulates whichever cartridge save chip (if any) a ROM was linked
// against - GBA carts have no separate "save slot" concept, the save
// medium is a real chip on the cartridge board, one of four kinds, and a
// game only ever talks to the one it was actually built with. GBATEK
// "Save Media Types".
class SaveMemory {
public:
    enum class Type { kNone, kSram, kFlash64K, kFlash128K, kEeprom };

    SaveMemory();

    // Scans the ROM image for the linker-inserted ID string
    // ("SRAM_V", "FLASH_V"/"FLASH512_V", "FLASH1M_V", "EEPROM_V") that
    // devkitPro's save-access libraries embed so real hardware/emulators
    // can identify the cart's save chip, then (re)allocates backing
    // storage for it. Safe to call again on a fresh ROM load.
    void DetectFromRom(const std::vector<u8>& rom);

    Type type() const { return type_; }

    // SRAM (0x0E000000) region access - also where Flash's command
    // register window lives, since both chips share the same cartridge
    // address range and only one is ever present per cart. `offset` is
    // the raw low bits of the address (masking to the chip's actual size
    // happens internally).
    u8 ReadSram(u32 offset) const;
    void WriteSram(u32 offset, u8 value);

    // EEPROM is accessed as a 1-bit-per-halfword serial stream rather
    // than being byte-addressable, so these operate on whole DMA-driven
    // Read16/Write16 calls instead - see save.cpp for the bit-serial
    // protocol this implements. Logically these advance an internal
    // cursor/state machine even though ReadEeprom() takes no explicit
    // input, hence `mutable` state rather than a non-const method (Bus'
    // Read16 is const, matching every other read path).
    u16 ReadEeprom() const;
    void WriteEeprom(u16 value);

    // True whenever the backing store has changed since the last
    // successful SaveToFile() - lets the caller avoid needless disk I/O
    // every frame.
    bool IsDirty() const { return dirty_; }

    // Loads/saves the whole backing store as a flat binary file. Missing
    // file on load is not an error (a cart with no save yet) - returns
    // false but leaves the freshly-allocated (all-0xFF, "erased") backing
    // store as-is.
    bool LoadFromFile(const std::string& path);
    bool SaveToFile(const std::string& path);

    // Save-state support: chip type and the backing store itself - the
    // actual game-progress data. Deliberately NOT included: the Flash
    // command-sequence and EEPROM bit-serial protocol state machines
    // below - both complete within a handful of CPU cycles, so the odds
    // of a save-state landing mid-sequence are vanishingly small, and if
    // it ever did, resetting to idle on load just means the in-flight
    // command needs to be retried (the same thing that'd happen after a
    // real hardware reset mid-command) rather than replaying a protocol
    // negotiation exactly - a fine trade for not serializing five
    // stopgap fields.
    void SaveState(StateWriter& w) const;
    void LoadState(StateReader& r);

private:
    Type type_ = Type::kNone;
    // mutable: EEPROM write commands are only detected/finalized inside
    // ReadEeprom() (see FinishEepromCommand's comment), which is const to
    // match Bus::Read16's constness - so this needs to be writable from
    // there even though every other access path is non-const.
    mutable std::vector<u8> data_;
    mutable bool dirty_ = false;

    // --- Flash chip command state (GBATEK "GBA Cartridge Flash ROM /
    // RAM") - Macronix/SST/Panasonic-style parts all speak the same
    // JEDEC-ish two-byte unlock sequence (0xAA@0x5555, 0x55@0x2AAA)
    // followed by a command byte. ---
    int flashUnlockStage_ = 0;         // 0=idle, 1=saw first unlock byte, 2=saw second (next write is the command)
    int flashEraseUnlockStage_ = 0;    // erase needs its own second unlock sequence after the 0x80 command
    bool flashErasePending_ = false;
    bool flashWritePending_ = false;   // 0xA0 seen - the next byte write anywhere programs that address
    bool flashBankSelectPending_ = false; // 0xB0 seen (128K parts only) - next write to offset 0 selects the bank
    bool flashIdMode_ = false;         // 0x90 seen - reads return manufacturer/device ID instead of data
    u32 flashBank_ = 0;                // 0 or 1, selects which 64KB half of a 128K part is currently windowed in

    void WriteFlash(u32 offset, u8 value);

    // --- EEPROM bit-serial protocol state (GBATEK "GBA - EEPROM
    // Overview"). The address width (6 bits for the 512-byte part, 14
    // bits - only the low 10 of which are actually significant - for the
    // 8KB part) isn't stored on the cartridge or discoverable up front;
    // real hardware/emulators infer it from how many bits a command
    // burst actually contains, which is exactly what FinishEepromCommand
    // does. ---
    mutable std::vector<u8> eepromBits_;         // bits collected so far in the current incoming command
    mutable std::vector<u8> eepromResponseBits_; // pending response bits for an in-progress read command
    mutable std::size_t eepromResponseIndex_ = 0;
    mutable bool eepromResponding_ = false;

    void FinishEepromCommand() const;
};

} // namespace gba
