#pragma once

#include <SDL2/SDL.h>

#include <string>
#include <vector>

#include "core/emulator.h"
#include "frontend/rom_browser.h"

namespace gba::frontend {

// Owns the whole front-end: SDL window/renderer/audio, the Emulator
// instance, and a small state machine (splash -> menu -> playing, with a
// pause overlay reachable from playing) that main.cpp just constructs and
// runs. Keeping this out of main.cpp/core is what lets core stay SDL-free
// (see gba_core's CMakeLists comment) while main.cpp itself stays a
// two-line entry point.
class App {
public:
    // Returns false (and logs why) if SDL/window/renderer/audio setup
    // fails - the caller should treat that as a fatal startup error.
    bool Init();
    void Shutdown();

    // If a ROM path is given on the command line, skip the splash/menu
    // entirely and start playing it directly - preserves the old
    // "./gba_emulator rom.gba" workflow for quick testing.
    void LoadRomDirectly(const std::string& path);

    // Runs the full event/update/render loop until the window is closed.
    // Blocks until then.
    void Run();

private:
    enum class State { kSplash, kMenu, kPlaying, kPaused };

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* gameTexture_ = nullptr;
    SDL_Texture* splashTexture_ = nullptr;
    SDL_AudioDeviceID audioDevice_ = 0;

    Emulator emulator_;
    State state_ = State::kSplash;

    // --- Splash ---
    Uint64 splashStartTicks_ = 0;

    // --- Menu ---
    std::vector<RomEntry> roms_;
    int menuSelection_ = 0;
    void RefreshRomList();

    // --- Playing / pause overlay ---
    std::string currentRomPath_;
    int pauseSelection_ = 0;
    std::string toastMessage_;
    Uint64 toastExpiryTicks_ = 0;

    // Edge-triggered key state - SDL_GetKeyboardState() gives the raw
    // held/not-held snapshot every frame, but menu navigation needs
    // "just pressed this frame", not "held", or a single tap would
    // register as dozens of repeats.
    Uint8 prevKeys_[SDL_NUM_SCANCODES]{};
    bool KeyPressed(const Uint8* keys, SDL_Scancode code) const {
        return keys[code] && !prevKeys_[code];
    }

    void StartRom(const std::string& path);
    void ReturnToMenu();
    void ShowToast(const std::string& message);

    std::string StatePathFor(const std::string& romPath) const;
    void QuickSaveState();
    void QuickLoadState();

    void UpdateSplash(const Uint8* keys);
    void UpdateMenu(const Uint8* keys);
    void UpdatePlaying(const Uint8* keys);
    void UpdatePaused(const Uint8* keys);

    void RenderSplash();
    void RenderMenu();
    void RenderPlaying();
    void RenderPauseOverlay();
};

} // namespace gba::frontend
