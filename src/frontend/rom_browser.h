#pragma once

#include <string>
#include <vector>

namespace gba::frontend {

struct RomEntry {
    std::string displayName; // filename without the .gba extension
    std::string path;        // full path, ready to hand to Emulator::LoadRom
};

// Recursively scans `romsDir` for *.gba files (case-insensitive
// extension), sorted alphabetically by display name. Returns an empty
// list if the directory doesn't exist or has nothing in it - not an
// error, just an empty game-selection menu (the caller shows a "drop ROMs
// in roms/" message for that case).
std::vector<RomEntry> ScanRomsDirectory(const std::string& romsDir);

// The roms/ directory this project's .gitignore already anticipates
// (`!roms/homebrew/*.gba`), relative to the current working directory -
// matching how running the emulator itself already works (README's
// `./build/bin/gba_emulator path/to/rom.gba` is run from the repo root,
// and that ROM path is CWD-relative too), rather than resolving relative
// to wherever the executable binary happens to sit (deep in build/bin/,
// not a place a user would want to keep their ROM collection).
std::string DefaultRomsDirectory();

} // namespace gba::frontend
