#include <SDL2/SDL.h>

#include <cstdio>

#include "core/emulator.h"
#include "core/ppu/ppu.h"

namespace {
constexpr int kWindowScale = 3;
}

int main(int argc, char** argv) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
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

        SDL_UpdateTexture(
            texture, nullptr, emulator.ppu().Framebuffer().data(),
            gba::Ppu::kScreenWidth * static_cast<int>(sizeof(gba::u32)));

        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
    }

    emulator.FlushSave();

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
