#include "frontend/rom_browser.h"

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
    return "roms";
}

} // namespace gba::frontend
