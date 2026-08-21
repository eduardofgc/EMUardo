#include "core/memory/gpio.h"

#include <ctime>

namespace gba {

namespace {
u8 ToBcd(int value) {
    return static_cast<u8>((((value / 10) % 10) << 4) | (value % 10));
}

// RTC register indices (bits4-6 of the command byte) and how many
// parameter bytes each transfers - cross-checked against mgba's
// RTC_BYTES table (src/gba/hardware.c). Indices 1, 5, 7 don't exist.
constexpr u32 kRtcReset = 0;
constexpr u32 kRtcDateTime = 2;
constexpr u32 kRtcForceIrq = 3;
constexpr u32 kRtcControl = 4;
constexpr u32 kRtcTime = 6;
constexpr u32 kRtcParamBytes[8] = {0, 0, 7, 0, 1, 0, 3, 0};

constexpr u8 kRtcHour24Bit = 1u << 6;
} // namespace

void Gpio::SaveState(StateWriter& w) const {
    w.Write(data_);
    w.Write(direction_);
    w.Write(control_);
    w.Write(chipSioOut_);
    w.Write(rtcControl_);
}

void Gpio::LoadState(StateReader& r) {
    r.Read(data_);
    r.Read(direction_);
    r.Read(control_);
    r.Read(chipSioOut_);
    r.Read(rtcControl_);
}

bool Gpio::TryRead8(u32 romOffset, u8& value) const {
    if (romOffset == kControlOffset) {
        value = static_cast<u8>(control_ & 0xFFu);
        return true;
    }
    if (romOffset == kControlOffset + 1) {
        value = static_cast<u8>(control_ >> 8);
        return true;
    }

    if ((control_ & 0x1u) == 0) {
        return false; // port disabled for reads - Data/Direction pass through to plain ROM data
    }
    if (romOffset == kDataOffset) {
        value = static_cast<u8>(ComputeReadbackData() & 0xFFu);
        return true;
    }
    if (romOffset == kDataOffset + 1) {
        value = static_cast<u8>(ComputeReadbackData() >> 8);
        return true;
    }
    if (romOffset == kDirectionOffset) {
        value = static_cast<u8>(direction_ & 0xFFu);
        return true;
    }
    if (romOffset == kDirectionOffset + 1) {
        value = static_cast<u8>(direction_ >> 8);
        return true;
    }
    return false;
}

u16 Gpio::ComputeReadbackData() const {
    u16 result = data_;
    if ((direction_ & kSioBit) == 0) {
        result = static_cast<u16>((result & static_cast<u16>(~kSioBit)) | (chipSioOut_ ? kSioBit : 0u));
    }
    return result;
}

bool Gpio::TryWrite16(u32 romOffset, u16 value) {
    if (romOffset == kControlOffset) {
        control_ = value;
        return true;
    }
    if (romOffset == kDirectionOffset) {
        direction_ = value;
        return true;
    }
    if (romOffset != kDataOffset) {
        return false;
    }

    const bool oldCs = (data_ & kCsBit) != 0;
    const bool newCs = (value & kCsBit) != 0;
    if (newCs && !oldCs) {
        // Chip select just asserted - start a fresh transaction.
        rtcPhase_ = RtcPhase::kCommand;
        rtcBitIndex_ = 0;
        rtcShiftByte_ = 0;
    } else if (!newCs && oldCs) {
        rtcPhase_ = RtcPhase::kIdle;
    }

    const bool oldSck = (data_ & kSckBit) != 0;
    const bool newSck = (value & kSckBit) != 0;
    if (newCs && newSck && !oldSck) {
        OnSckRisingEdge((value & kSioBit) != 0);
    }

    data_ = value;
    return true;
}

void Gpio::OnSckRisingEdge(bool sioIn) {
    switch (rtcPhase_) {
        case RtcPhase::kCommand:
            rtcShiftByte_ = static_cast<u8>(rtcShiftByte_ | ((sioIn ? 1u : 0u) << rtcBitIndex_));
            if (++rtcBitIndex_ == 8) {
                OnCommandByteComplete();
            }
            break;

        case RtcPhase::kParamsIn:
            rtcParamBuffer_[rtcParamByteIndex_] = static_cast<u8>(
                rtcParamBuffer_[rtcParamByteIndex_] | ((sioIn ? 1u : 0u) << rtcBitIndex_));
            if (++rtcBitIndex_ == 8) {
                rtcBitIndex_ = 0;
                if (++rtcParamByteIndex_ >= rtcParamTotalBytes_) {
                    if (rtcCommandReg_ == kRtcControl) {
                        rtcControl_ = rtcParamBuffer_[0];
                    }
                    // DateTime/Time writes are accepted but not stored -
                    // reads always report the host's current time (see
                    // RefreshTime), which is what games actually care
                    // about (querying "now", not persisting a set time
                    // across boots).
                    rtcPhase_ = RtcPhase::kIdle;
                }
            }
            break;

        case RtcPhase::kParamsOut:
            // Present the bit at the *current* index first (this edge is
            // what the game's very next Data-register read will sample),
            // then advance for the following edge.
            chipSioOut_ = ((rtcParamBuffer_[rtcParamByteIndex_] >> rtcBitIndex_) & 1u) != 0;
            if (++rtcBitIndex_ == 8) {
                rtcBitIndex_ = 0;
                if (++rtcParamByteIndex_ >= rtcParamTotalBytes_) {
                    rtcPhase_ = RtcPhase::kIdle;
                }
            }
            break;

        case RtcPhase::kIdle:
        default:
            break;
    }
}

void Gpio::OnCommandByteComplete() {
    // Field layout confirmed against mgba's RTCCommandData bitfield
    // (Magic=bits0-3, Command=bits4-6, Reading=bit7) - a command whose
    // low nibble isn't the fixed 0x6 "Magic" value is garbage (or this
    // class's bit-sampling lost sync somehow) and is dropped rather than
    // acted on, same as real hardware/mgba both do.
    const u32 magic = rtcShiftByte_ & 0xFu;
    rtcCommandReg_ = (rtcShiftByte_ >> 4) & 0x7u;
    rtcCommandIsRead_ = (rtcShiftByte_ & 0x80u) != 0;
    rtcBitIndex_ = 0;
    rtcParamByteIndex_ = 0;

    if (magic != 0x6u) {
        rtcPhase_ = RtcPhase::kIdle;
        return;
    }

    rtcParamTotalBytes_ = kRtcParamBytes[rtcCommandReg_];

    if (rtcCommandReg_ == kRtcReset) {
        rtcControl_ = 0; // Reset acts immediately, no parameter phase.
        rtcPhase_ = RtcPhase::kIdle;
        return;
    }
    if (rtcParamTotalBytes_ == 0) {
        rtcPhase_ = RtcPhase::kIdle; // ForceIRQ or an unused register index - no-op strobe
        return;
    }

    if (rtcCommandIsRead_) {
        if (rtcCommandReg_ == kRtcControl) {
            rtcParamBuffer_[0] = rtcControl_;
        } else {
            // DateTime and Time share the same 7-byte
            // Year/Month/Day/DayOfWeek/Hour/Minute/Second layout - Time
            // just starts 4 bytes in (Hour) and sends the last 3.
            RefreshTime();
            const u32 startByte = (rtcCommandReg_ == kRtcTime) ? 4u : 0u;
            for (u32 i = 0; i < rtcParamTotalBytes_; ++i) {
                rtcParamBuffer_[i] = rtcTime_[startByte + i];
            }
        }
        rtcPhase_ = RtcPhase::kParamsOut;
    } else {
        for (u32 i = 0; i < rtcParamTotalBytes_; ++i) {
            rtcParamBuffer_[i] = 0;
        }
        rtcPhase_ = RtcPhase::kParamsIn;
    }
}

void Gpio::RefreshTime() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif

    rtcTime_[0] = ToBcd((local.tm_year + 1900) % 100);
    rtcTime_[1] = ToBcd(local.tm_mon + 1);
    rtcTime_[2] = ToBcd(local.tm_mday);
    rtcTime_[3] = ToBcd(local.tm_wday); // 0 = Sunday, matching struct tm
    const bool hour24 = (rtcControl_ & kRtcHour24Bit) != 0;
    rtcTime_[4] = ToBcd(hour24 ? local.tm_hour : (local.tm_hour % 12));
    rtcTime_[5] = ToBcd(local.tm_min);
    rtcTime_[6] = ToBcd(local.tm_sec);
}

} // namespace gba
