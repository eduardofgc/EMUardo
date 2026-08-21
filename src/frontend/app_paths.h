#pragma once

#include <string>

namespace gba::frontend {

// Resolves `relativeName` (e.g. "roms" or "keybindings.cfg") against, in
// order: the current working directory, the executable's own directory,
// and two levels up from the executable (the repo root, matching
// CMAKE_RUNTIME_OUTPUT_DIRECTORY's build/bin/ layout) - returning the
// first one that actually exists. If none exist, returns the two-levels-
// up candidate (the most likely intended location next to the repo, not
// wherever the shell's cwd happened to be) so a "not found" message
// points somewhere actionable, and so a first-time write (e.g. saving
// key bindings) lands in a predictable, reusable place rather than
// wherever the emulator happened to be launched from.
std::string ResolveAppPath(const std::string& relativeName);

} // namespace gba::frontend
