#pragma once

#include <SDL2/SDL.h>

#include <string>

#include "core/types.h"

namespace gba::frontend {

// One entry per remappable GBA button, in the order the controls screen
// lists and navigates them - deliberately not the same order as the
// gba::key:: bit positions (that order groups by register layout, not by
// what reads naturally in a menu).
enum class GbaButton { kA, kB, kStart, kSelect, kUp, kDown, kLeft, kRight, kL, kR };
constexpr int kGbaButtonCount = 10;

struct GbaButtonInfo {
    GbaButton button;
    const char* label; // for the controls screen and the saved config file
    u16 keyMask;        // the matching gba::key:: bit
};
extern const GbaButtonInfo kGbaButtons[kGbaButtonCount];

// A user's keyboard-to-GBA-button mapping. Every button always has some
// binding (SDL_SCANCODE_UNKNOWN included, for "did the user finish
// clearing this without picking a replacement" - never actually stored,
// see KeyBindings::Set) - there's no "unbound" state to special-case
// elsewhere.
class KeyBindings {
public:
    static KeyBindings Defaults();

    SDL_Scancode Get(GbaButton button) const { return scancodes_[static_cast<std::size_t>(button)]; }
    void Set(GbaButton button, SDL_Scancode scancode) { scancodes_[static_cast<std::size_t>(button)] = scancode; }

    // Builds the gba::key:: pressed-mask Emulator::SetKeyState() wants,
    // straight from SDL_GetKeyboardState()'s per-frame snapshot.
    u16 ComputePressedMask(const Uint8* keys) const;

    // Simple human-readable "LABEL=SCANCODE_NAME" text format (one per
    // line, via SDL_GetScancodeName/SDL_GetScancodeFromName) - not a
    // format needing forward/backward compatibility handling, just
    // something a user could also hand-edit if they wanted to.
    bool LoadFromFile(const std::string& path);
    bool SaveToFile(const std::string& path) const;

private:
    SDL_Scancode scancodes_[kGbaButtonCount] = {}; // zero-init = SDL_SCANCODE_UNKNOWN, a safe "never triggers" default
};

} // namespace gba::frontend
