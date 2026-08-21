#include "frontend/rom_browser.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace gba::frontend {

namespace {
bool HasGbaExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".gba";
}
} // namespace

std::vector<RomEntry> ScanRomsDirectory(const std::string& romsDir) {
    std::vector<RomEntry> entries;

    std::error_code ec;
    if (!std::filesystem::exists(romsDir, ec) || ec) {
        return entries;
    }

    for (const auto& dirEntry : std::filesystem::recursive_directory_iterator(
             romsDir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (!dirEntry.is_regular_file(ec) || ec) continue;
        if (!HasGbaExtension(dirEntry.path())) continue;

        entries.push_back(RomEntry{
            dirEntry.path().stem().string(),
            dirEntry.path().string(),
        });
    }

    std::sort(entries.begin(), entries.end(), [](const RomEntry& a, const RomEntry& b) {
        return a.displayName < b.displayName;
    });
    return entries;
}

std::string DefaultRomsDirectory() {
    // "roms" alone (relative to whatever directory the command happened
    // to be run from) only finds the repo's roms/ folder if you `cd`
    // there first - launching via a full/relative path from elsewhere
    // (e.g. `~/gba-emulator/build/bin/gba_emulator` from $HOME) resolves
    // it against $HOME instead and silently finds nothing. Try, in
    // order: cwd-relative "roms" (works when run from the repo root, as
    // the README documents), then paths relative to the executable
    // itself - build/bin/gba_emulator's own directory, and two levels up
    // from it (the repo root, matching CMAKE_RUNTIME_OUTPUT_DIRECTORY) -
    // so it also works launched from anywhere, or if the binary is ever
    // copied out of the build tree and shipped alongside a roms/ folder.
    std::vector<std::string> candidates = {"roms"};

    if (char* base = SDL_GetBasePath()) {
        const std::filesystem::path exeDir(base);
        SDL_free(base);
        candidates.push_back((exeDir / "roms").string());
        candidates.push_back((exeDir / ".." / ".." / "roms").lexically_normal().string());
    }

    std::error_code ec;
    for (const std::string& candidate : candidates) {
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return candidate;
        }
    }
    // Nothing found - report the most likely intended location (next to
    // the repo, not wherever the shell's cwd happened to be) so the
    // "no ROMs found" message points somewhere actionable.
    return candidates.size() > 2 ? candidates[2] : candidates[0];
}

} // namespace gba::frontend
