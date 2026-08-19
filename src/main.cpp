#include <SDL2/SDL.h>

#include <cstdio>
#include <vector>

#include "core/emulator.h"
#include "core/ppu/ppu.h"

namespace {
constexpr int kWindowScale = 3;
}

int main(int argc, char** argv) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // 32768Hz matches Apu::GenerateSample()'s fixed tick rate (see
    // emulator.cpp) - no resampling needed between the two. Queued rather
    // than callback-driven, since we already produce a whole frame's
    // worth of samples at once right after RunFrame().
    SDL_AudioSpec audioSpec{};
    audioSpec.freq = 32768;
    audioSpec.format = AUDIO_S16SYS;
    audioSpec.channels = 2;
    audioSpec.samples = 2048;
    const SDL_AudioDeviceID audioDevice = SDL_OpenAudioDevice(nullptr, 0, &audioSpec, nullptr, 0);
    if (audioDevice == 0) {
        std::fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        // Not fatal - the emulator still runs, just silently.
    } else {
        SDL_PauseAudioDevice(audioDevice, 0);
    }

    SDL_Window* window = SDL_CreateWindow(
        "GBA Emulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        gba::Ppu::kScreenWidth * kWindowScale,
        gba::Ppu::kScreenHeight * kWindowScale,
        SDL_WINDOW_SHOWN);

    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    SDL_Texture* texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
        gba::Ppu::kScreenWidth, gba::Ppu::kScreenHeight);

    gba::Emulator emulator;

    if (argc > 1) {
        if (!emulator.LoadRom(argv[1])) {
            std::fprintf(stderr, "Failed to load ROM: %s\n", argv[1]);
            // Not fatal - keep the window open so the render pipeline is
            // still visible/testable without a ROM on hand.
        }
    } else {
        std::fprintf(stderr, "Usage: %s <rom.gba>\n", argv[0]);
    }

    bool running = true;
    SDL_Event event;

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
        }

        // Polled per-frame keyboard state rather than tracking individual
        // KEYDOWN/KEYUP events - simpler, and correct for a system with no
        // sub-frame input timing to begin with. Standard-ish mapping:
        // Z/X for A/B, Enter for Start, Right Shift for Select, arrow keys
        // for the D-pad, A/S for L/R shoulder buttons.
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        gba::u16 pressedMask = 0;
        if (keys[SDL_SCANCODE_Z])      pressedMask |= gba::key::kA;
        if (keys[SDL_SCANCODE_X])      pressedMask |= gba::key::kB;
        if (keys[SDL_SCANCODE_RETURN]) pressedMask |= gba::key::kStart;
        if (keys[SDL_SCANCODE_RSHIFT]) pressedMask |= gba::key::kSelect;
        if (keys[SDL_SCANCODE_RIGHT])  pressedMask |= gba::key::kRight;
        if (keys[SDL_SCANCODE_LEFT])   pressedMask |= gba::key::kLeft;
        if (keys[SDL_SCANCODE_UP])     pressedMask |= gba::key::kUp;
        if (keys[SDL_SCANCODE_DOWN])   pressedMask |= gba::key::kDown;
        if (keys[SDL_SCANCODE_A])      pressedMask |= gba::key::kL;
        if (keys[SDL_SCANCODE_S])      pressedMask |= gba::key::kR;
        emulator.SetKeyState(pressedMask);

        emulator.RunFrame();

        if (audioDevice != 0) {
            const std::vector<gba::s16> samples = emulator.DrainAudioSamples();
            if (!samples.empty()) {
                // Cap the queue so a host frame drop (or the emulator
                // briefly running ahead) can't build up unbounded audio
                // latency - drop the backlog and let it resync instead of
                // playing catch-up.
                constexpr Uint32 kMaxQueuedBytes = 32768 * 2 * sizeof(gba::s16); // ~1s
                if (SDL_GetQueuedAudioSize(audioDevice) < kMaxQueuedBytes) {
                    SDL_QueueAudio(audioDevice, samples.data(),
                                    static_cast<Uint32>(samples.size() * sizeof(gba::s16)));
                }
            }
        }

        SDL_UpdateTexture(
            texture, nullptr, emulator.ppu().Framebuffer().data(),
            gba::Ppu::kScreenWidth * static_cast<int>(sizeof(gba::u32)));

        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
    }

    emulator.FlushSave();

    if (audioDevice != 0) {
        SDL_CloseAudioDevice(audioDevice);
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
