#include <cstdio>

#include "core/emulator.h"

// Replace this with a real test runner once CPU instruction decoding
// exists. The plan: load a known-good ARM/Thumb instruction test ROM,
// run it, and assert on register/flag state at known checkpoints.
int main() {
    gba::Emulator emulator;
    std::printf("Placeholder test: Emulator constructs without crashing.\n");
    return 0;
}
