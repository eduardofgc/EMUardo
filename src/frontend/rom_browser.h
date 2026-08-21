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
// (`!roms/homebrew/*.gba`) - see ResolveAppPath (app_paths.h) for how
// it's located regardless of the working directory the emulator is
// launched from.
std::string DefaultRomsDirectory();

} // namespace gba::frontend
