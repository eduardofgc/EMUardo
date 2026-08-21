#include "frontend/app_paths.h"

#include <SDL2/SDL.h>

#include <filesystem>
#include <vector>

namespace gba::frontend {

std::string ResolveAppPath(const std::string& relativeName) {
    std::vector<std::string> candidates = {relativeName};

    if (char* base = SDL_GetBasePath()) {
        const std::filesystem::path exeDir(base);
        SDL_free(base);
        candidates.push_back((exeDir / relativeName).string());
        candidates.push_back((exeDir / ".." / ".." / relativeName).lexically_normal().string());
    }

    std::error_code ec;
    for (const std::string& candidate : candidates) {
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return candidate;
        }
    }
    return candidates.size() > 2 ? candidates[2] : candidates[0];
}

} // namespace gba::frontend
