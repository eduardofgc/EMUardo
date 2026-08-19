#pragma once

#include "core/types.h"

namespace gba {

// Emulates the GBA cartridge's General Purpose I/O port (GBATEK "GBA
// Cart I/O Port (GPIO)") together with the Seiko S-3511 real-time clock
// chip wired to three of its four pins (SCK/SIO/CS) in carts that have
// one - most famously the Pokemon Ruby/Sapphire/Emerald family, whose
// boot sequence probes it very early and won't proceed until it gets a
// sensible response. Lives in the ROM address space at
// 0x080000C4-0x080000C9 (mirrored at the usual ROM wait-state offsets,
// same as the rest of ROM) - Bus routes reads/writes there through this
// class instead of treating them as plain ROM data.
//
// Carts without this hardware are unaffected: real ROM images are
// zero-filled at this address, the Control register starts at 0 (read-
// disabled), and while it stays that way Data/Direction reads
// transparently fall through to that zero-filled ROM data instead of
// this class's internal state - see TryRead8.
class Gpio {
public:
    // Returns true (and fills `value`) if `romOffset` (address masked to
    // the ROM's low bits, same convention Bus::Read8's ROM case already
    // uses) is inside the GPIO port window and the Control register
    // currently allows reads to reflect live state; false means the
    // caller should read plain ROM data instead.
    bool TryRead8(u32 romOffset, u8& value) const;

    // Returns true (and applies the write) if `romOffset` is inside the
    // GPIO port window; false means the caller should fall through to
    // its normal (ignored) ROM-write handling. Real hardware only
    // accepts 16/32-bit writes to this port - Bus only calls this from
    // Write16 (Write32 decomposes into two Write16 calls already), never
    // Write8, which is what makes byte writes here correctly no-ops
    // without this class needing to know about it.
    bool TryWrite16(u32 romOffset, u16 value);

private:
    static constexpr u32 kDataOffset = 0xC4;
    static constexpr u32 kDirectionOffset = 0xC6;
    static constexpr u32 kControlOffset = 0xC8;

    static constexpr u16 kSckBit = 1u << 0;
    static constexpr u16 kSioBit = 1u << 1;
    static constexpr u16 kCsBit  = 1u << 2;

    u16 data_ = 0;
    u16 direction_ = 0;
    u16 control_ = 0;

    // What Read should report for the SIO pin while it's configured as
    // an input (i.e. the RTC chip, not the GBA, is driving it) - see
    // ComputeReadbackData().
    bool chipSioOut_ = false;

    u16 ComputeReadbackData() const;

    // --- RTC (Seiko S-3511) 3-wire serial protocol state machine ---
    // Command and parameter bytes are clocked LSB-first, one bit per SCK
    // rising edge while CS is asserted. A CS 0->1 transition starts a
    // transaction (command byte first); CS 1->0 ends it. The command
    // byte's field layout and register set below are cross-checked
    // against mgba's GBA hardware emulation (src/gba/hardware.h/.c,
    // MPL-2.0) rather than guessed from GBATEK's fuller-but-not-quite-
    // matching NDS RTC table: bits0-3 are a fixed "Magic" nibble (0x6)
    // that must be present for the command to be valid at all, bits4-6
    // select the register, bit7 is the read/write flag.
    enum class RtcPhase { kIdle, kCommand, kParamsIn, kParamsOut };
    RtcPhase rtcPhase_ = RtcPhase::kIdle;
    u32 rtcBitIndex_ = 0;
    u8 rtcShiftByte_ = 0;
    u32 rtcCommandReg_ = 0;
    bool rtcCommandIsRead_ = false;
    u8 rtcParamBuffer_[7] = {};
    u32 rtcParamTotalBytes_ = 0;
    u32 rtcParamByteIndex_ = 0;
    u8 rtcControl_ = 0;       // MinIRQ=bit3, Hour24=bit6, Poweroff=bit7; 0 (12h) after Reset
    u8 rtcTime_[7] = {};      // Year,Month,Day,DayOfWeek,Hour,Minute,Second (BCD) - refreshed on each DateTime/Time read

    void OnSckRisingEdge(bool sioIn);
    void OnCommandByteComplete();
    // Refreshes rtcTime_ with the host's current wall-clock date/time in
    // BCD, per GBATEK's Date&Time byte layout - games query this to show
    // "now", not to persist a time they set themselves.
    void RefreshTime();
};

} // namespace gba
