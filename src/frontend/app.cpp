#include "frontend/app.h"

#include <cstdio>
#include <cstring>
#include <fstream>

#include "core/ppu/ppu.h"
#include "frontend/app_paths.h"
#include "frontend/font.h"
#include "frontend/splash_image.h"

namespace gba::frontend {

namespace {
constexpr int kWindowScale = 3;
constexpr int kWindowWidth = Ppu::kScreenWidth * kWindowScale;
constexpr int kWindowHeight = Ppu::kScreenHeight * kWindowScale;

// Real GBA frame rate (see the identical constant/comment this replaces
// in the old main.cpp) - used to pace every state, not just gameplay, so
// the splash/menu screens don't busy-loop at whatever FPS the display
// happens to deliver.
constexpr double kGbaFps = 16777216.0 / 280896.0;

constexpr Uint32 kSplashMinMs = 1500;
constexpr Uint32 kToastMs = 1500;

constexpr SDL_Color kWhite{255, 255, 255, 255};
constexpr SDL_Color kDim{140, 140, 150, 255};
constexpr SDL_Color kAccent{255, 210, 90, 255};
constexpr SDL_Color kBackground{18, 18, 24, 255};
} // namespace

bool App::Init() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_AudioSpec audioSpec{};
    audioSpec.freq = 32768;
    audioSpec.format = AUDIO_S16SYS;
    audioSpec.channels = 2;
    audioSpec.samples = 2048;
    audioDevice_ = SDL_OpenAudioDevice(nullptr, 0, &audioSpec, nullptr, 0);
    if (audioDevice_ == 0) {
        std::fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        // Not fatal - the emulator still runs, just silently.
    } else {
        SDL_PauseAudioDevice(audioDevice_, 0);
    }

    window_ = SDL_CreateWindow(
        "EMUardo", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        kWindowWidth, kWindowHeight, SDL_WINDOW_SHOWN);
    if (!window_) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer_) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    gameTexture_ = SDL_CreateTexture(
        renderer_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
        Ppu::kScreenWidth, Ppu::kScreenHeight);

    SDL_Surface* splashSurface = SDL_CreateRGBSurfaceFrom(
        const_cast<unsigned char*>(kSplashImageRgba), kSplashImageWidth, kSplashImageHeight,
        32, kSplashImageWidth * 4,
        0x0000'00FFu, 0x0000'FF00u, 0x00FF'0000u, 0xFF00'0000u);
    // Sets the OS-level window icon (title bar, taskbar/Alt-Tab preview) -
    // same smiley bitmap the splash screen itself renders, reused here
    // rather than relying on Windows' convention of falling back to the
    // .exe's embedded resource icon when this isn't called, so the icon
    // is identical and explicit on every platform.
    SDL_SetWindowIcon(window_, splashSurface);
    splashTexture_ = SDL_CreateTextureFromSurface(renderer_, splashSurface);
    SDL_FreeSurface(splashSurface);

    keyBindings_ = KeyBindings::Defaults();
    keyBindings_.LoadFromFile(ResolveAppPath("keybindings.cfg")); // no saved file yet is fine - defaults stand

    RefreshRomList();
    splashStartTicks_ = SDL_GetTicks64();
    return true;
}

void App::Shutdown() {
    if (state_ == State::kPlaying || state_ == State::kPaused) {
        emulator_.FlushSave();
    }
    if (audioDevice_ != 0) {
        SDL_CloseAudioDevice(audioDevice_);
    }
    if (gameTexture_) SDL_DestroyTexture(gameTexture_);
    if (splashTexture_) SDL_DestroyTexture(splashTexture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

void App::LoadRomDirectly(const std::string& path) {
    StartRom(path);
}

void App::RefreshRomList() {
    roms_ = ScanRomsDirectory(DefaultRomsDirectory());
    if (menuSelection_ >= static_cast<int>(roms_.size())) {
        menuSelection_ = 0;
    }
}

void App::StartRom(const std::string& path) {
    if (!emulator_.LoadRom(path)) {
        ShowToast("FAILED TO LOAD ROM");
        return;
    }
    currentRomPath_ = path;
    state_ = State::kPlaying;
    if (audioDevice_ != 0) {
        SDL_ClearQueuedAudio(audioDevice_);
    }
}

void App::ReturnToMenu() {
    emulator_.FlushSave();
    if (audioDevice_ != 0) {
        SDL_ClearQueuedAudio(audioDevice_);
    }
    RefreshRomList();
    state_ = State::kMenu;
}

void App::ShowToast(const std::string& message) {
    toastMessage_ = message;
    toastExpiryTicks_ = SDL_GetTicks64() + kToastMs;
}

std::string App::StatePathFor(const std::string& romPath) const {
    // Mirrors the .sav derivation Bus already does for battery saves
    // (same directory, same base name) - see bus.cpp's DeriveSavePath.
    const std::size_t lastSlash = romPath.find_last_of("/\\");
    const std::size_t lastDot = romPath.find_last_of('.');
    if (lastDot != std::string::npos && (lastSlash == std::string::npos || lastDot > lastSlash)) {
        return romPath.substr(0, lastDot) + ".state";
    }
    return romPath + ".state";
}

void App::QuickSaveState() {
    const std::vector<u8> state = emulator_.SaveState();
    std::ofstream file(StatePathFor(currentRomPath_), std::ios::binary);
    if (!file) {
        ShowToast("SAVE STATE FAILED");
        return;
    }
    file.write(reinterpret_cast<const char*>(state.data()), static_cast<std::streamsize>(state.size()));
    ShowToast(file ? "STATE SAVED" : "SAVE STATE FAILED");
}

void App::QuickLoadState() {
    std::ifstream file(StatePathFor(currentRomPath_), std::ios::binary | std::ios::ate);
    if (!file) {
        ShowToast("NO SAVED STATE");
        return;
    }
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<u8> buffer(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size) || !emulator_.LoadState(buffer)) {
        ShowToast("LOAD STATE FAILED");
        return;
    }
    if (audioDevice_ != 0) {
        SDL_ClearQueuedAudio(audioDevice_);
    }
    ShowToast("STATE LOADED");
}

// ---------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------

void App::UpdateSplash(const Uint8* keys) {
    const bool timedOut = SDL_GetTicks64() - splashStartTicks_ >= kSplashMinMs;
    bool anyKey = false;
    for (int i = 0; i < SDL_NUM_SCANCODES && !anyKey; ++i) {
        anyKey = keys[i] && !prevKeys_[i];
    }
    if (timedOut && anyKey) {
        state_ = State::kMenu;
    }
}

void App::UpdateMenu(const Uint8* keys) {
    if (KeyPressed(keys, SDL_SCANCODE_TAB)) {
        returnStateAfterControls_ = State::kMenu;
        controlsSelection_ = 0;
        awaitingRebind_ = false;
        state_ = State::kControls;
        return;
    }
    if (roms_.empty()) {
        return;
    }
    if (KeyPressed(keys, SDL_SCANCODE_DOWN)) {
        menuSelection_ = (menuSelection_ + 1) % static_cast<int>(roms_.size());
    }
    if (KeyPressed(keys, SDL_SCANCODE_UP)) {
        menuSelection_ = (menuSelection_ - 1 + static_cast<int>(roms_.size())) % static_cast<int>(roms_.size());
    }
    if (KeyPressed(keys, SDL_SCANCODE_RETURN) || KeyPressed(keys, SDL_SCANCODE_Z)) {
        StartRom(roms_[static_cast<std::size_t>(menuSelection_)].path);
    }
}

void App::UpdatePlaying(const Uint8* keys) {
    if (KeyPressed(keys, SDL_SCANCODE_ESCAPE)) {
        pauseSelection_ = 0;
        state_ = State::kPaused;
        if (audioDevice_ != 0) {
            SDL_ClearQueuedAudio(audioDevice_);
        }
        return;
    }
    if (KeyPressed(keys, SDL_SCANCODE_F5)) {
        QuickSaveState();
    }
    if (KeyPressed(keys, SDL_SCANCODE_F9)) {
        QuickLoadState();
    }

    emulator_.SetKeyState(keyBindings_.ComputePressedMask(keys));

    emulator_.RunFrame();

    if (audioDevice_ != 0) {
        const std::vector<s16> samples = emulator_.DrainAudioSamples();
        if (!samples.empty()) {
            // Cap the queue so a host frame drop (or the emulator briefly
            // running ahead) can't build up unbounded audio latency -
            // drop the backlog and let it resync instead of playing
            // catch-up.
            constexpr Uint32 kMaxQueuedBytes = 32768 * 2 * sizeof(s16); // ~1s
            if (SDL_GetQueuedAudioSize(audioDevice_) < kMaxQueuedBytes) {
                SDL_QueueAudio(audioDevice_, samples.data(),
                                static_cast<Uint32>(samples.size() * sizeof(s16)));
            }
        }
    }
}

void App::UpdatePaused(const Uint8* keys) {
    constexpr int kOptionCount = 5; // Resume, Save State, Load State, Controls, Return to Menu
    if (KeyPressed(keys, SDL_SCANCODE_DOWN)) {
        pauseSelection_ = (pauseSelection_ + 1) % kOptionCount;
    }
    if (KeyPressed(keys, SDL_SCANCODE_UP)) {
        pauseSelection_ = (pauseSelection_ - 1 + kOptionCount) % kOptionCount;
    }
    if (KeyPressed(keys, SDL_SCANCODE_ESCAPE)) {
        state_ = State::kPlaying;
        return;
    }
    if (KeyPressed(keys, SDL_SCANCODE_RETURN) || KeyPressed(keys, SDL_SCANCODE_Z)) {
        switch (pauseSelection_) {
            case 0: state_ = State::kPlaying; break;
            case 1: QuickSaveState(); break;
            case 2: QuickLoadState(); break;
            case 3:
                returnStateAfterControls_ = State::kPaused;
                controlsSelection_ = 0;
                awaitingRebind_ = false;
                state_ = State::kControls;
                break;
            case 4: ReturnToMenu(); break;
            default: break;
        }
    }
}

void App::UpdateControls(const Uint8* keys) {
    if (awaitingRebind_) {
        if (KeyPressed(keys, SDL_SCANCODE_ESCAPE)) {
            awaitingRebind_ = false;
            return;
        }
        // Any other just-pressed key becomes the new binding - scanning
        // the full table rather than a fixed list is the whole point
        // here, so literally any key the user's keyboard has can be
        // bound.
        for (int code = 0; code < SDL_NUM_SCANCODES; ++code) {
            if (code == SDL_SCANCODE_ESCAPE) continue;
            if (keys[code] && !prevKeys_[code]) {
                keyBindings_.Set(kGbaButtons[controlsSelection_].button, static_cast<SDL_Scancode>(code));
                keyBindings_.SaveToFile(ResolveAppPath("keybindings.cfg"));
                awaitingRebind_ = false;
                break;
            }
        }
        return;
    }

    if (KeyPressed(keys, SDL_SCANCODE_DOWN)) {
        controlsSelection_ = (controlsSelection_ + 1) % kGbaButtonCount;
    }
    if (KeyPressed(keys, SDL_SCANCODE_UP)) {
        controlsSelection_ = (controlsSelection_ - 1 + kGbaButtonCount) % kGbaButtonCount;
    }
    if (KeyPressed(keys, SDL_SCANCODE_RETURN)) {
        awaitingRebind_ = true;
    }
    if (KeyPressed(keys, SDL_SCANCODE_R)) {
        keyBindings_ = KeyBindings::Defaults();
        keyBindings_.SaveToFile(ResolveAppPath("keybindings.cfg"));
        ShowToast("RESET TO DEFAULTS");
    }
    if (KeyPressed(keys, SDL_SCANCODE_ESCAPE)) {
        state_ = returnStateAfterControls_;
    }
}

// ---------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------

void App::RenderSplash() {
    SDL_SetRenderDrawColor(renderer_, kBackground.r, kBackground.g, kBackground.b, 255);
    SDL_RenderClear(renderer_);

    constexpr int kIconScale = 2;
    constexpr int kIconTopMargin = 50;
    const int iconW = kSplashImageWidth * kIconScale;
    const int iconH = kSplashImageHeight * kIconScale;
    SDL_Rect iconRect{(kWindowWidth - iconW) / 2, kIconTopMargin, iconW, iconH};
    SDL_RenderCopy(renderer_, splashTexture_, nullptr, &iconRect);

    const std::string title = "EMUARDO";
    constexpr int kTitleScale = 5;
    constexpr int kTitleGap = 30;
    const int titleWidth = MeasureText(title, kTitleScale);
    DrawText(renderer_, (kWindowWidth - titleWidth) / 2, iconRect.y + iconH + kTitleGap, title, kTitleScale, kWhite);

    // Blink the hint every half second rather than showing it statically.
    if ((SDL_GetTicks64() / 500) % 2 == 0) {
        const std::string hint = "PRESS ANY KEY";
        constexpr int kHintScale = 2;
        const int hintWidth = MeasureText(hint, kHintScale);
        DrawText(renderer_, (kWindowWidth - hintWidth) / 2, kWindowHeight - 60, hint, kHintScale, kDim);
    }

    SDL_RenderPresent(renderer_);
}

void App::RenderMenu() {
    SDL_SetRenderDrawColor(renderer_, kBackground.r, kBackground.g, kBackground.b, 255);
    SDL_RenderClear(renderer_);

    constexpr int kTitleScale = 3;
    DrawText(renderer_, 30, 30, "SELECT GAME", kTitleScale, kWhite);

    if (roms_.empty()) {
        constexpr int kScale = 2;
        DrawText(renderer_, 30, 100, "NO ROMS FOUND", kScale, kDim);
        DrawText(renderer_, 30, 130, "DROP .GBA FILES INTO:", kScale, kDim);
        DrawText(renderer_, 30, 155, DefaultRomsDirectory(), 1, kDim);
        DrawText(renderer_, 30, kWindowHeight - 24, "TAB: CONTROLS", 1, kDim);
        SDL_RenderPresent(renderer_);
        return;
    }

    constexpr int kRowScale = 2;
    constexpr int kRowHeight = 28;
    constexpr int kListTop = 90;
    // Center the selection in a scrollable window of rows, rather than
    // rendering the whole list, so a large ROM collection doesn't run off
    // the bottom of the screen.
    constexpr int kVisibleRows = 12;
    int firstVisible = menuSelection_ - kVisibleRows / 2;
    if (firstVisible < 0) firstVisible = 0;
    const int maxFirst = static_cast<int>(roms_.size()) - kVisibleRows;
    if (firstVisible > maxFirst) firstVisible = maxFirst;
    if (firstVisible < 0) firstVisible = 0;

    for (int row = 0; row < kVisibleRows; ++row) {
        const int index = firstVisible + row;
        if (index >= static_cast<int>(roms_.size())) break;
        const int y = kListTop + row * kRowHeight;
        const bool selected = (index == menuSelection_);
        const SDL_Color color = selected ? kAccent : kWhite;
        if (selected) {
            DrawText(renderer_, 24, y, ">", kRowScale, kAccent);
        }
        DrawText(renderer_, 50, y, roms_[static_cast<std::size_t>(index)].displayName, kRowScale, color);
    }

    constexpr int kHintScale = 1;
    DrawText(renderer_, 30, kWindowHeight - 24, "ARROWS: NAVIGATE   ENTER: PLAY   TAB: CONTROLS", kHintScale, kDim);

    SDL_RenderPresent(renderer_);
}

void App::RenderControls() {
    SDL_SetRenderDrawColor(renderer_, kBackground.r, kBackground.g, kBackground.b, 255);
    SDL_RenderClear(renderer_);

    constexpr int kTitleScale = 3;
    DrawText(renderer_, 30, 30, "CONTROLS", kTitleScale, kWhite);

    constexpr int kRowScale = 2;
    constexpr int kRowHeight = 30;
    constexpr int kListTop = 90;
    for (int i = 0; i < kGbaButtonCount; ++i) {
        const int y = kListTop + i * kRowHeight;
        const bool selected = (i == controlsSelection_);
        const SDL_Color color = selected ? kAccent : kWhite;
        if (selected) {
            DrawText(renderer_, 24, y, ">", kRowScale, kAccent);
        }
        DrawText(renderer_, 50, y, kGbaButtons[i].label, kRowScale, color);

        const char* keyName = SDL_GetScancodeName(keyBindings_.Get(kGbaButtons[i].button));
        const std::string keyLabel = (keyName && keyName[0]) ? keyName : "(NONE)";
        const bool showingPrompt = selected && awaitingRebind_;
        DrawText(renderer_, 260, y, showingPrompt ? "PRESS A KEY..." : keyLabel, kRowScale,
                 showingPrompt ? kAccent : kDim);
    }

    constexpr int kHintScale = 1;
    DrawText(renderer_, 30, kWindowHeight - 36, "ENTER: REBIND   R: RESET ALL TO DEFAULTS", kHintScale, kDim);
    DrawText(renderer_, 30, kWindowHeight - 20, "ESC: BACK (OR CANCEL REBIND)", kHintScale, kDim);

    if (SDL_GetTicks64() < toastExpiryTicks_) {
        constexpr int kScale = 2;
        const int width = MeasureText(toastMessage_, kScale);
        SDL_Rect backdrop{(kWindowWidth - width) / 2 - 10, kWindowHeight - 90, width + 20, 30};
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 180);
        SDL_RenderFillRect(renderer_, &backdrop);
        DrawText(renderer_, (kWindowWidth - width) / 2, kWindowHeight - 82, toastMessage_, kScale, kWhite);
    }

    SDL_RenderPresent(renderer_);
}

void App::RenderPlaying() {
    SDL_UpdateTexture(
        gameTexture_, nullptr, emulator_.ppu().Framebuffer().data(),
        Ppu::kScreenWidth * static_cast<int>(sizeof(u32)));

    SDL_RenderClear(renderer_);
    SDL_RenderCopy(renderer_, gameTexture_, nullptr, nullptr);

    if (SDL_GetTicks64() < toastExpiryTicks_) {
        constexpr int kScale = 2;
        const int width = MeasureText(toastMessage_, kScale);
        SDL_Rect backdrop{(kWindowWidth - width) / 2 - 10, kWindowHeight - 50, width + 20, 30};
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 180);
        SDL_RenderFillRect(renderer_, &backdrop);
        DrawText(renderer_, (kWindowWidth - width) / 2, kWindowHeight - 42, toastMessage_, kScale, kWhite);
    }

    SDL_RenderPresent(renderer_);
}

void App::RenderPauseOverlay() {
    // The frozen last game frame stays visible underneath a dimming
    // overlay, rather than clearing to black - it reads as "paused", not
    // "stopped".
    SDL_RenderCopy(renderer_, gameTexture_, nullptr, nullptr);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 170);
    SDL_Rect fullscreen{0, 0, kWindowWidth, kWindowHeight};
    SDL_RenderFillRect(renderer_, &fullscreen);

    static const char* kOptions[] = {"RESUME", "SAVE STATE", "LOAD STATE", "CONTROLS", "RETURN TO MENU"};
    constexpr int kOptionCount = 5;
    constexpr int kScale = 3;
    constexpr int kRowHeight = 40;
    const int boxHeight = kOptionCount * kRowHeight + 40;
    const int boxTop = (kWindowHeight - boxHeight) / 2;

    SDL_SetRenderDrawColor(renderer_, kBackground.r, kBackground.g, kBackground.b, 230);
    SDL_Rect box{kWindowWidth / 2 - 160, boxTop, 320, boxHeight};
    SDL_RenderFillRect(renderer_, &box);

    for (int i = 0; i < kOptionCount; ++i) {
        const bool selected = (i == pauseSelection_);
        const int y = boxTop + 20 + i * kRowHeight;
        if (selected) {
            DrawText(renderer_, box.x + 20, y, ">", kScale, kAccent);
        }
        DrawText(renderer_, box.x + 50, y, kOptions[i], kScale, selected ? kAccent : kWhite);
    }

    SDL_RenderPresent(renderer_);
}

// ---------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------

void App::Run() {
    bool running = true;
    SDL_Event event;

    const Uint64 perfFrequency = SDL_GetPerformanceFrequency();
    const Uint64 targetFrameTicks = static_cast<Uint64>(static_cast<double>(perfFrequency) / kGbaFps);
    Uint64 nextFrameDeadline = SDL_GetPerformanceCounter() + targetFrameTicks;

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
        }

        const Uint8* keys = SDL_GetKeyboardState(nullptr);

        switch (state_) {
            case State::kSplash:   UpdateSplash(keys);   RenderSplash();       break;
            case State::kMenu:     UpdateMenu(keys);     RenderMenu();         break;
            case State::kPlaying:  UpdatePlaying(keys);  RenderPlaying();      break;
            case State::kPaused:   UpdatePaused(keys);   RenderPauseOverlay(); break;
            case State::kControls: UpdateControls(keys); RenderControls();     break;
        }

        std::memcpy(prevKeys_, keys, sizeof(prevKeys_));

        // Same explicit-pacing approach the old main.cpp used for
        // gameplay, now applied uniformly - see kGbaFps's comment.
        const Uint64 now = SDL_GetPerformanceCounter();
        if (now < nextFrameDeadline) {
            const Uint64 remainingTicks = nextFrameDeadline - now;
            const double remainingMs =
                static_cast<double>(remainingTicks) * 1000.0 / static_cast<double>(perfFrequency);
            if (remainingMs > 1.0) {
                SDL_Delay(static_cast<Uint32>(remainingMs - 1.0));
            }
            while (SDL_GetPerformanceCounter() < nextFrameDeadline) {
                // busy-wait for the final sub-millisecond
            }
            nextFrameDeadline += targetFrameTicks;
        } else {
            nextFrameDeadline = now + targetFrameTicks;
        }
    }
}

} // namespace gba::frontend
