#include "frontend/key_bindings.h"

#include <fstream>

namespace gba::frontend {

const GbaButtonInfo kGbaButtons[kGbaButtonCount] = {
    {GbaButton::kA, "A", key::kA},
    {GbaButton::kB, "B", key::kB},
    {GbaButton::kStart, "START", key::kStart},
    {GbaButton::kSelect, "SELECT", key::kSelect},
    {GbaButton::kUp, "UP", key::kUp},
    {GbaButton::kDown, "DOWN", key::kDown},
    {GbaButton::kLeft, "LEFT", key::kLeft},
    {GbaButton::kRight, "RIGHT", key::kRight},
    {GbaButton::kL, "L", key::kL},
    {GbaButton::kR, "R", key::kR},
};

KeyBindings KeyBindings::Defaults() {
    // Matches the mapping this project shipped with before remapping
    // existed - Z/X for A/B, Enter for Start, Right Shift for Select,
    // arrow keys for the D-pad, A/S for L/R.
    KeyBindings bindings;
    bindings.Set(GbaButton::kA, SDL_SCANCODE_Z);
    bindings.Set(GbaButton::kB, SDL_SCANCODE_X);
    bindings.Set(GbaButton::kStart, SDL_SCANCODE_RETURN);
    bindings.Set(GbaButton::kSelect, SDL_SCANCODE_RSHIFT);
    bindings.Set(GbaButton::kUp, SDL_SCANCODE_UP);
    bindings.Set(GbaButton::kDown, SDL_SCANCODE_DOWN);
    bindings.Set(GbaButton::kLeft, SDL_SCANCODE_LEFT);
    bindings.Set(GbaButton::kRight, SDL_SCANCODE_RIGHT);
    bindings.Set(GbaButton::kL, SDL_SCANCODE_A);
    bindings.Set(GbaButton::kR, SDL_SCANCODE_S);
    return bindings;
}

u16 KeyBindings::ComputePressedMask(const Uint8* keys) const {
    u16 mask = 0;
    for (const GbaButtonInfo& info : kGbaButtons) {
        if (keys[Get(info.button)]) {
            mask |= info.keyMask;
        }
    }
    return mask;
}

bool KeyBindings::LoadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string label = line.substr(0, eq);
        const std::string scancodeName = line.substr(eq + 1);

        const SDL_Scancode scancode = SDL_GetScancodeFromName(scancodeName.c_str());
        if (scancode == SDL_SCANCODE_UNKNOWN) continue;

        for (const GbaButtonInfo& info : kGbaButtons) {
            if (label == info.label) {
                Set(info.button, scancode);
                break;
            }
        }
    }
    return true;
}

bool KeyBindings::SaveToFile(const std::string& path) const {
    std::ofstream file(path);
    if (!file) {
        return false;
    }
    for (const GbaButtonInfo& info : kGbaButtons) {
        file << info.label << "=" << SDL_GetScancodeName(Get(info.button)) << "\n";
    }
    return static_cast<bool>(file);
}

} // namespace gba::frontend
