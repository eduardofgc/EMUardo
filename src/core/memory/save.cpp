#include "core/memory/save.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace gba {

namespace {
constexpr std::size_t kSramSize      = 32 * 1024;
constexpr std::size_t kFlash64KSize  = 64 * 1024;
constexpr std::size_t kFlash128KSize = 128 * 1024;
constexpr std::size_t kEepromSize    = 8 * 1024; // large enough for either real EEPROM size - see save.h

bool ContainsAscii(const std::vector<u8>& rom, const char* needle) {
    const std::size_t needleLen = std::strlen(needle);
    if (rom.size() < needleLen) {
        return false;
    }
    for (std::size_t i = 0; i + needleLen <= rom.size(); ++i) {
        if (std::memcmp(rom.data() + i, needle, needleLen) == 0) {
            return true;
        }
    }
    return false;
}

SaveMemory::Type DetectType(const std::vector<u8>& rom) {
    // Order matters only in that "FLASH1M_V" must be checked before the
    // plain "FLASH_V"/"FLASH512_V" pair would never collide with it
    // anyway (different strings), but checking the longer/more specific
    // ones first is the conventional order GBATEK lists them in.
    if (ContainsAscii(rom, "EEPROM_V")) return SaveMemory::Type::kEeprom;
    if (ContainsAscii(rom, "FLASH1M_V")) return SaveMemory::Type::kFlash128K;
    if (ContainsAscii(rom, "FLASH512_V")) return SaveMemory::Type::kFlash64K;
    if (ContainsAscii(rom, "FLASH_V")) return SaveMemory::Type::kFlash64K;
    if (ContainsAscii(rom, "SRAM_V")) return SaveMemory::Type::kSram;
    return SaveMemory::Type::kNone;
}
} // namespace

SaveMemory::SaveMemory() = default;

void SaveMemory::SaveState(StateWriter& w) const {
    w.Write(type_);
    w.WriteVector(data_);
}

void SaveMemory::LoadState(StateReader& r) {
    r.Read(type_);
    r.ReadVector(data_);
    dirty_ = true; // conservative: make sure a post-load autosave (or exit) actually flushes it once
}

void SaveMemory::DetectFromRom(const std::vector<u8>& rom) {
    type_ = DetectType(rom);

    std::size_t size = 0;
    switch (type_) {
        case Type::kSram:      size = kSramSize;      break;
        case Type::kFlash64K:  size = kFlash64KSize;  break;
        case Type::kFlash128K: size = kFlash128KSize; break;
        case Type::kEeprom:    size = kEepromSize;     break;
        case Type::kNone:      size = 0;               break;
    }
    // Real save chips read as all-1-bits (0xFF per byte) until a game
    // erases/programs them - matches Flash's actual erased state exactly,
    // and is a reasonable stand-in for SRAM/EEPROM too (both effectively
    // start "empty" from a fresh cartridge).
    data_.assign(size, 0xFFu);
    dirty_ = false;

    flashUnlockStage_ = 0;
    flashEraseUnlockStage_ = 0;
    flashErasePending_ = false;
    flashWritePending_ = false;
    flashBankSelectPending_ = false;
    flashIdMode_ = false;
    flashBank_ = 0;

    eepromBits_.clear();
    eepromResponseBits_.clear();
    eepromResponseIndex_ = 0;
    eepromResponding_ = false;
}

bool SaveMemory::LoadFromFile(const std::string& path) {
    if (type_ == Type::kNone) {
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file.read(reinterpret_cast<char*>(data_.data()), static_cast<std::streamsize>(data_.size()));
    dirty_ = false;
    return true;
}

bool SaveMemory::SaveToFile(const std::string& path) {
    if (type_ == Type::kNone) {
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(data_.data()), static_cast<std::streamsize>(data_.size()));
    dirty_ = !static_cast<bool>(file);
    return static_cast<bool>(file);
}

u8 SaveMemory::ReadSram(u32 offset) const {
    switch (type_) {
        case Type::kSram:
            return data_[offset & (kSramSize - 1)];
        case Type::kFlash64K:
        case Type::kFlash128K: {
            const u32 localOffset = offset & 0xFFFFu;
            if (flashIdMode_ && localOffset < 2u) {
                // Chip ID bytes real cartridges commonly use for each
                // size - games check these to decide whether bank
                // switching (128K parts only) applies, not much else.
                if (type_ == Type::kFlash128K) {
                    return localOffset == 0u ? 0xC2u : 0x09u; // Macronix MX29L010
                }
                return localOffset == 0u ? 0x32u : 0x1Bu; // Panasonic MN63F805MNP
            }
            const u32 realOffset = (flashBank_ << 16) | localOffset;
            return data_[realOffset % data_.size()];
        }
        default:
            return 0xFFu; // open-bus-ish default for a cart with no save chip
    }
}

void SaveMemory::WriteSram(u32 offset, u8 value) {
    switch (type_) {
        case Type::kSram:
            data_[offset & (kSramSize - 1)] = value;
            dirty_ = true;
            return;
        case Type::kFlash64K:
        case Type::kFlash128K:
            WriteFlash(offset, value);
            return;
        default:
            return;
    }
}

void SaveMemory::WriteFlash(u32 offset, u8 value) {
    const u32 localOffset = offset & 0xFFFFu;

    if (flashErasePending_) {
        if (flashEraseUnlockStage_ == 0 && localOffset == 0x5555u && value == 0xAAu) {
            flashEraseUnlockStage_ = 1;
            return;
        }
        if (flashEraseUnlockStage_ == 1 && localOffset == 0x2AAAu && value == 0x55u) {
            flashEraseUnlockStage_ = 2;
            return;
        }
        if (flashEraseUnlockStage_ == 2) {
            if (localOffset == 0x5555u && value == 0x10u) {
                std::fill(data_.begin(), data_.end(), 0xFFu); // chip erase
                dirty_ = true;
            } else if (value == 0x30u) {
                // Sector erase: the 4KB sector containing `offset`,
                // within whichever bank is currently selected.
                const u32 sectorBase = (flashBank_ << 16) | (localOffset & ~0x0FFFu);
                for (u32 i = 0; i < 0x1000u; ++i) {
                    data_[(sectorBase + i) % data_.size()] = 0xFFu;
                }
                dirty_ = true;
            }
            flashErasePending_ = false;
            flashEraseUnlockStage_ = 0;
            flashUnlockStage_ = 0;
        }
        return;
    }

    if (flashWritePending_) {
        const u32 realOffset = (flashBank_ << 16) | localOffset;
        // Flash programming can only clear bits (1->0), never set them -
        // an actual erase is required to get back to 0xFF first, which
        // `data_[...] &= value` models correctly without a separate
        // "is this byte erased" check.
        data_[realOffset % data_.size()] &= value;
        dirty_ = true;
        flashWritePending_ = false;
        return;
    }

    if (flashBankSelectPending_) {
        if (localOffset == 0u) {
            flashBank_ = value & 0x1u;
        }
        flashBankSelectPending_ = false;
        return;
    }

    if (flashUnlockStage_ == 0 && localOffset == 0x5555u && value == 0xAAu) {
        flashUnlockStage_ = 1;
        return;
    }
    if (flashUnlockStage_ == 1 && localOffset == 0x2AAAu && value == 0x55u) {
        flashUnlockStage_ = 2;
        return;
    }
    if (flashUnlockStage_ == 2 && localOffset == 0x5555u) {
        flashUnlockStage_ = 0;
        switch (value) {
            case 0x90u: flashIdMode_ = true; return;
            case 0xF0u: flashIdMode_ = false; return;
            case 0x80u: flashErasePending_ = true; return;
            case 0xA0u: flashWritePending_ = true; return;
            case 0xB0u: flashBankSelectPending_ = true; return;
            default: return; // unrecognized command byte - ignore
        }
    }

    // Not part of a recognized unlock sequence - reset progress so a
    // stray write doesn't leave the state machine stuck mid-sequence.
    flashUnlockStage_ = 0;
}

u16 SaveMemory::ReadEeprom() const {
    if (!eepromResponding_ && !eepromBits_.empty()) {
        // The write burst that built up eepromBits_ has ended - the very
        // fact that a Read16 arrived instead of another Write16 is what
        // signals that, on real hardware, the bus turned around. This
        // call itself is already the first response read (a dummy bit
        // for a read command, or the single "not busy" status bit for a
        // write command), not just a state-transition trigger.
        FinishEepromCommand();
    }

    if (eepromResponding_) {
        const u16 bit = eepromResponseBits_[eepromResponseIndex_++];
        if (eepromResponseIndex_ >= eepromResponseBits_.size()) {
            eepromResponding_ = false;
            eepromResponseBits_.clear();
            eepromResponseIndex_ = 0;
        }
        return bit;
    }

    return 1u; // idle / "not busy" - also what a write command's status poll should see
}

void SaveMemory::WriteEeprom(u16 value) {
    if (eepromResponding_) {
        // A new command started before a previous read response was
        // fully drained - abandon it and start fresh rather than mixing
        // leftover response state into the new command.
        eepromResponding_ = false;
        eepromResponseBits_.clear();
        eepromResponseIndex_ = 0;
    }
    eepromBits_.push_back(static_cast<u8>(value & 0x1u));
    // Longest real command (write, 14-bit address) is 2+14+64+1 = 81
    // bits; anything longer is a malformed stream, so cap it defensively
    // rather than growing forever.
    if (eepromBits_.size() > 128) {
        eepromBits_.erase(eepromBits_.begin());
    }
}

void SaveMemory::FinishEepromCommand() const {
    const std::size_t total = eepromBits_.size();
    if (total < 2) {
        eepromBits_.clear();
        return;
    }

    const bool isRead = (eepromBits_[0] == 1 && eepromBits_[1] == 1);
    const bool isWrite = (eepromBits_[0] == 1 && eepromBits_[1] == 0);

    // The address width (6 or 14 bits) is never announced anywhere - it's
    // implied entirely by how many bits the game's own DMA burst
    // transferred, since a read command is exactly 2+N+1 bits and a
    // write command is exactly 2+N+64+1 bits. GBATEK "GBA - EEPROM
    // Overview": only the low 10 of the 14 address bits are actually
    // significant for the 8KB part, hence masking separately below
    // rather than trusting the raw decoded value.
    std::size_t addressBitCount;
    if (isRead && total >= 3) {
        addressBitCount = total - 3;
    } else if (isWrite && total >= 67) {
        addressBitCount = total - 67;
    } else {
        eepromBits_.clear();
        return; // malformed/incomplete command - drop it
    }

    u32 address = 0;
    for (std::size_t i = 0; i < addressBitCount; ++i) {
        address = (address << 1) | eepromBits_[2 + i];
    }
    const u32 addressMask = (addressBitCount <= 6) ? 0x3Fu : 0x3FFu;
    const u32 recordOffset = (address & addressMask) * 8u;

    if (isRead) {
        eepromResponseBits_.assign(68, 0); // 4 dummy bits + 64 data bits, MSB first
        for (u32 byteIndex = 0; byteIndex < 8; ++byteIndex) {
            const u32 dataAddr = recordOffset + byteIndex;
            const u8 byte = (dataAddr < data_.size()) ? data_[dataAddr] : 0xFFu;
            for (int bitPos = 0; bitPos < 8; ++bitPos) {
                eepromResponseBits_[4 + byteIndex * 8 + static_cast<std::size_t>(bitPos)] =
                    (byte >> (7 - bitPos)) & 0x1u;
            }
        }
        eepromResponseIndex_ = 0;
        eepromResponding_ = true;
    } else if (isWrite) {
        for (u32 byteIndex = 0; byteIndex < 8; ++byteIndex) {
            u8 byte = 0;
            for (int bitPos = 0; bitPos < 8; ++bitPos) {
                const std::size_t bitIndex = 2 + addressBitCount + byteIndex * 8 + static_cast<std::size_t>(bitPos);
                byte = static_cast<u8>((byte << 1) | eepromBits_[bitIndex]);
            }
            const u32 dataAddr = recordOffset + byteIndex;
            if (dataAddr < data_.size()) {
                data_[dataAddr] = byte;
            }
        }
        dirty_ = true;
    }

    eepromBits_.clear();
}

} // namespace gba
