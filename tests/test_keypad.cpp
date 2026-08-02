#include <cstdio>

#include "core/memory/bus.h"
#include "core/types.h"

namespace {
int failures = 0;
void Check(bool c, const char* label) {
    if (!c) { std::printf("FAIL: %s\n", label); ++failures; }
}
}

int main() {
    gba::Bus bus;

    Check(bus.Read16(gba::mem::kIoBase + gba::io::kKeyInput) == gba::key::kAll,
          "KEYINPUT defaults to all-released (0x3FF) at boot");

    bus.SetKeyState(gba::key::kA | gba::key::kUp);
    const gba::u16 expected = static_cast<gba::u16>(~(gba::key::kA | gba::key::kUp) & gba::key::kAll);
    Check(bus.Read16(gba::mem::kIoBase + gba::io::kKeyInput) == expected,
          "KEYINPUT correctly inverts pressed bits (active-low)");

    // CPU/game writes to KEYINPUT should be ignored - it's hardware-driven.
    bus.Write16(gba::mem::kIoBase + gba::io::kKeyInput, 0);
    Check(bus.Read16(gba::mem::kIoBase + gba::io::kKeyInput) == expected,
          "writes to KEYINPUT are ignored (hardware-controlled register)");

    // KEYCNT: IRQ enable (bit14) + OR condition (bit15=0) on A+B.
    const gba::u16 keycntOr = static_cast<gba::u16>((gba::key::kA | gba::key::kB) | (1u << 14));
    bus.Write16(gba::mem::kIoBase + gba::io::kKeyCnt, keycntOr);
    bus.SetKeyState(gba::key::kA); // only A held - satisfies OR condition
    Check((bus.Read16(gba::mem::kIoBase + gba::io::kIf) & gba::irq::kKeypad) != 0,
          "KEYCNT OR condition fires when at least one selected key is pressed");

    // Fresh bus for a clean IF state, testing the AND condition this time.
    gba::Bus bus2;
    const gba::u16 keycntAnd = static_cast<gba::u16>((gba::key::kA | gba::key::kB) | (1u << 14) | (1u << 15));
    bus2.Write16(gba::mem::kIoBase + gba::io::kKeyCnt, keycntAnd);
    bus2.SetKeyState(gba::key::kA); // only A - should NOT satisfy AND condition
    Check((bus2.Read16(gba::mem::kIoBase + gba::io::kIf) & gba::irq::kKeypad) == 0,
          "KEYCNT AND condition doesn't fire with only one of two required keys");
    bus2.SetKeyState(gba::key::kA | gba::key::kB); // both - should satisfy it
    Check((bus2.Read16(gba::mem::kIoBase + gba::io::kIf) & gba::irq::kKeypad) != 0,
          "KEYCNT AND condition fires once all required keys are pressed");

    if (failures == 0) {
        std::printf("PASS: keypad input (KEYINPUT active-low conversion, write-protection, KEYCNT IRQ)\n");
    }
    return failures;
}
