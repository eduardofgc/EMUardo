#include "core/ppu/ppu.h"

namespace gba {

Ppu::Ppu() {
    // Fill with a visible placeholder pattern (not black) so it's obvious
    // in the SDL window that the pipeline is wired up correctly, before
    // any real rendering exists. Delete once Mode 3 bitmap rendering lands.
    framebuffer_.fill(0xFF303030);
}

void Ppu::Step() {
    // TODO: scanline-driven rendering per DISPCNT mode.
}

} // namespace gba
