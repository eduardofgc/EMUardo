// Covers cartridge save-chip emulation: SRAM, both Flash sizes (64K/128K,
// including the JEDEC-style command protocol and 128K's bank switching),
// and EEPROM (both real address widths - 6-bit/512B and 14-bit/8KB -
// self-detected purely from how many bits a command burst contains, since
// that's never announced anywhere on real hardware either). Also covers
// Bus-level ROM ID-string detection, address routing, and save-file
// persistence end to end.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

#include "core/memory/bus.h"
#include "core/memory/save.h"
#include "core/types.h"

namespace {

int failures = 0;

void Check(bool condition, const char* label) {
    if (!condition) {
        std::printf("FAIL: %s\n", label);
        ++failures;
    }
}

// Builds a minimal fake ROM containing the given save-type ID string
// somewhere past the start (real linkers place it after the code, but
// SaveMemory's scan doesn't care where).
std::vector<gba::u8> FakeRom(const char* idString) {
    std::vector<gba::u8> rom(256, 0);
    const std::size_t len = std::char_traits<char>::length(idString);
    for (std::size_t i = 0; i < len; ++i) {
        rom[64 + i] = static_cast<gba::u8>(idString[i]);
    }
    return rom;
}

void PushBits(gba::SaveMemory& save, const std::vector<int>& bits) {
    for (int b : bits) {
        save.WriteEeprom(static_cast<gba::u16>(b));
    }
}

std::vector<int> BitsOf(gba::u64 value, int count) {
    std::vector<int> bits(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        bits[static_cast<std::size_t>(i)] = static_cast<int>((value >> (count - 1 - i)) & 1u);
    }
    return bits;
}

// Runs a full EEPROM write command (address `addr` at `addressBits` bits
// wide, 64-bit `data`), then polls once for "not busy" like a real game
// would.
void EepromWriteRecord(gba::SaveMemory& save, gba::u64 addr, int addressBits, gba::u64 data) {
    std::vector<int> bits = {1, 0};
    const std::vector<int> addrBits = BitsOf(addr, addressBits);
    bits.insert(bits.end(), addrBits.begin(), addrBits.end());
    const std::vector<int> dataBits = BitsOf(data, 64);
    bits.insert(bits.end(), dataBits.begin(), dataBits.end());
    bits.push_back(0); // stop bit
    PushBits(save, bits);
    save.ReadEeprom(); // busy-poll - completes the write
}

// Runs a full EEPROM read command and reconstructs the 64-bit record.
gba::u64 EepromReadRecord(gba::SaveMemory& save, gba::u64 addr, int addressBits) {
    std::vector<int> bits = {1, 1};
    const std::vector<int> addrBits = BitsOf(addr, addressBits);
    bits.insert(bits.end(), addrBits.begin(), addrBits.end());
    bits.push_back(0); // stop bit
    PushBits(save, bits);

    gba::u64 result = 0;
    for (int i = 0; i < 68; ++i) {
        const gba::u16 bit = save.ReadEeprom();
        if (i >= 4) { // skip the 4 dummy bits
            result = (result << 1) | (bit & 0x1u);
        }
    }
    return result;
}

} // namespace

int main() {
    // ---------- SRAM ----------
    {
        gba::SaveMemory save;
        const auto rom = FakeRom("SRAM_V110");
        save.DetectFromRom(rom);
        Check(save.type() == gba::SaveMemory::Type::kSram, "SRAM: ID string detected");

        save.WriteSram(0x100u, 0x42u);
        Check(save.ReadSram(0x100u) == 0x42u, "SRAM: byte round-trips");
        Check(save.ReadSram(0x100u + 0x8000u) == 0x42u, "SRAM: mirrors every 32KB");
        Check(save.ReadSram(0x200u) == 0xFFu, "SRAM: untouched byte reads as erased (0xFF)");
    }

    // ---------- Flash 64K ----------
    {
        gba::SaveMemory save;
        const auto rom = FakeRom("FLASH_V");
        save.DetectFromRom(rom);
        Check(save.type() == gba::SaveMemory::Type::kFlash64K, "Flash64K: ID string detected");

        // Chip ID mode: AA@5555, 55@2AAA, 90@5555 -> reads at 0/1 return the ID.
        save.WriteSram(0x5555u, 0xAAu);
        save.WriteSram(0x2AAAu, 0x55u);
        save.WriteSram(0x5555u, 0x90u);
        Check(save.ReadSram(0u) == 0x32u && save.ReadSram(1u) == 0x1Bu,
              "Flash64K: chip ID mode returns Panasonic ID");
        save.WriteSram(0x5555u, 0xAAu);
        save.WriteSram(0x2AAAu, 0x55u);
        save.WriteSram(0x5555u, 0xF0u); // exit ID mode

        // Byte program: AA@5555, 55@2AAA, A0@5555, then value@target.
        save.WriteSram(0x5555u, 0xAAu);
        save.WriteSram(0x2AAAu, 0x55u);
        save.WriteSram(0x5555u, 0xA0u);
        save.WriteSram(0x1234u, 0x0Fu);
        Check(save.ReadSram(0x1234u) == 0x0Fu, "Flash64K: byte programs correctly");

        // Programming can only clear bits, not set them.
        save.WriteSram(0x5555u, 0xAAu);
        save.WriteSram(0x2AAAu, 0x55u);
        save.WriteSram(0x5555u, 0xA0u);
        save.WriteSram(0x1234u, 0xF0u); // 0x0F & 0xF0 = 0x00
        Check(save.ReadSram(0x1234u) == 0x00u, "Flash64K: programming ANDs with existing value");

        // Sector erase: AA@5555, 55@2AAA, 80@5555, AA@5555, 55@2AAA, 30@sector.
        save.WriteSram(0x5555u, 0xAAu);
        save.WriteSram(0x2AAAu, 0x55u);
        save.WriteSram(0x5555u, 0x80u);
        save.WriteSram(0x5555u, 0xAAu);
        save.WriteSram(0x2AAAu, 0x55u);
        save.WriteSram(0x1000u, 0x30u); // erase the sector containing 0x1234
        Check(save.ReadSram(0x1234u) == 0xFFu, "Flash64K: sector erase resets to 0xFF");
    }

    // ---------- Flash 128K (bank switching) ----------
    {
        gba::SaveMemory save;
        const auto rom = FakeRom("FLASH1M_V102");
        save.DetectFromRom(rom);
        Check(save.type() == gba::SaveMemory::Type::kFlash128K, "Flash128K: ID string detected");

        // Program a byte at offset 0x10 in bank 0.
        save.WriteSram(0x5555u, 0xAAu);
        save.WriteSram(0x2AAAu, 0x55u);
        save.WriteSram(0x5555u, 0xA0u);
        save.WriteSram(0x10u, 0x11u);

        // Switch to bank 1: AA@5555, 55@2AAA, B0@5555, then bank number@0.
        save.WriteSram(0x5555u, 0xAAu);
        save.WriteSram(0x2AAAu, 0x55u);
        save.WriteSram(0x5555u, 0xB0u);
        save.WriteSram(0x0u, 0x1u);

        // Same local offset 0x10, but now in bank 1 - should be untouched.
        Check(save.ReadSram(0x10u) == 0xFFu, "Flash128K: bank 1 is independent of bank 0");
        save.WriteSram(0x5555u, 0xAAu);
        save.WriteSram(0x2AAAu, 0x55u);
        save.WriteSram(0x5555u, 0xA0u);
        save.WriteSram(0x10u, 0x22u);
        Check(save.ReadSram(0x10u) == 0x22u, "Flash128K: bank 1 byte programs independently");

        // Switch back to bank 0 and confirm its data survived.
        save.WriteSram(0x5555u, 0xAAu);
        save.WriteSram(0x2AAAu, 0x55u);
        save.WriteSram(0x5555u, 0xB0u);
        save.WriteSram(0x0u, 0x0u);
        Check(save.ReadSram(0x10u) == 0x11u, "Flash128K: switching back to bank 0 restores its data");
    }

    // ---------- EEPROM (both real address widths, self-detected) ----------
    {
        gba::SaveMemory save;
        const auto rom = FakeRom("EEPROM_V120");
        save.DetectFromRom(rom);
        Check(save.type() == gba::SaveMemory::Type::kEeprom, "EEPROM: ID string detected");

        // 6-bit address (matches the 512-byte part's protocol).
        EepromWriteRecord(save, 5, 6, 0x1122334455667788ULL);
        Check(EepromReadRecord(save, 5, 6) == 0x1122334455667788ULL,
              "EEPROM: 6-bit-address record round-trips");

        // 14-bit address (matches the 8KB part's protocol) - only the low
        // 10 bits are actually significant on real hardware.
        EepromWriteRecord(save, 0x123, 14, 0xAABBCCDDEEFF0011ULL);
        Check(EepromReadRecord(save, 0x123, 14) == 0xAABBCCDDEEFF0011ULL,
              "EEPROM: 14-bit-address record round-trips");
        Check(EepromReadRecord(save, 0x2123, 14) == 0xAABBCCDDEEFF0011ULL,
              "EEPROM: upper 4 address bits of a 14-bit address are padding, ignored on read");

        // The two address spaces used different command lengths but share
        // the same backing store by record index - confirm they didn't
        // clobber each other.
        Check(EepromReadRecord(save, 5, 6) == 0x1122334455667788ULL,
              "EEPROM: earlier 6-bit-address record is undisturbed by the 14-bit-address one");
    }

    // ---------- SaveMemory file persistence ----------
    {
        gba::SaveMemory save;
        const auto rom = FakeRom("SRAM_V110");
        save.DetectFromRom(rom);
        save.WriteSram(0u, 0xABu);
        save.WriteSram(100u, 0xCDu);
        Check(save.IsDirty(), "SaveMemory: dirty flag set after a write");

        const std::string path = "/tmp/gba_emulator_test_save.sav";
        Check(save.SaveToFile(path), "SaveMemory: SaveToFile succeeds");
        Check(!save.IsDirty(), "SaveMemory: dirty flag cleared after a successful save");

        gba::SaveMemory reloaded;
        reloaded.DetectFromRom(rom);
        Check(reloaded.LoadFromFile(path), "SaveMemory: LoadFromFile succeeds");
        Check(reloaded.ReadSram(0u) == 0xABu && reloaded.ReadSram(100u) == 0xCDu,
              "SaveMemory: reloaded data matches what was saved");
    }

    // ---------- Bus-level integration: ROM detection, routing, persistence ----------
    {
        const std::string romPath = "/tmp/gba_emulator_test_save_bus.gba";
        const std::string savPath = "/tmp/gba_emulator_test_save_bus.sav";
        std::remove(savPath.c_str());
        {
            const auto rom = FakeRom("SRAM_V110");
            std::ofstream out(romPath, std::ios::binary);
            out.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
        }

        gba::Bus bus;
        Check(bus.LoadRom(romPath), "Bus: LoadRom succeeds");
        bus.Write8(gba::mem::kSramBase + 0x50u, 0x99u);
        Check(bus.Read8(gba::mem::kSramBase + 0x50u) == 0x99u,
              "Bus: SRAM region write/read round-trips through the save chip");
        Check(bus.FlushSave(), "Bus: FlushSave writes the .sav file next to the ROM");

        gba::Bus bus2;
        Check(bus2.LoadRom(romPath), "Bus: second LoadRom of the same ROM succeeds");
        Check(bus2.Read8(gba::mem::kSramBase + 0x50u) == 0x99u,
              "Bus: a fresh Bus loading the same ROM picks up the persisted .sav file");
    }

    if (failures == 0) {
        std::printf("PASS: SRAM, Flash 64K/128K, EEPROM 512B/8K, and Bus-level save persistence\n");
    }
    return failures;
}
