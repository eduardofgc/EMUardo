#include <SDL2/SDL.h>

#include <cstdio>
#include <vector>

#include "core/emulator.h"
#include "core/ppu/ppu.h"

namespace {
constexpr int kWindowScale = 3;

// The real GBA runs at 16777216Hz / 280896 cycles-per-frame = ~59.7275fps,
// not 60. SDL_RENDERER_PRESENTVSYNC alone paces RunFrame()'s cadence to
// whatever the display/driver actually delivers (a flat 60Hz monitor is
// already a 0.4% speed/pitch error; an uncapped or adaptive-sync display -
// this exact build measured ~131fps uncapped earlier - makes it much worse).
// Since Apu::GenerateSample() produces a fixed amount of audio per
// RunFrame() call, that mismatch directly speeds up and chops the audio
// output. Pace explicitly to the true GBA frame rate here instead of
// trusting vsync to get it right.
constexpr double kGbaFps = 16777216.0 / 280896.0;
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

    // No SDL_RENDERER_PRESENTVSYNC here - frame pacing is handled explicitly
    // below against the real GBA frame rate instead, so playback speed
    // (and therefore audio pitch) doesn't depend on the display's refresh
    // rate or on vsync actually being honored.
    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED);

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

    const Uint64 perfFrequency = SDL_GetPerformanceFrequency();
    const Uint64 targetFrameTicks =
        static_cast<Uint64>(static_cast<double>(perfFrequency) / kGbaFps);
    Uint64 nextFrameDeadline = SDL_GetPerformanceCounter() + targetFrameTicks;

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

        // Sleep off whatever's left of this frame's budget. SDL_Delay is
        // only accurate to ~1ms, so leave a small margin and busy-wait the
        // remainder for precision - audio glitches are far more sensitive
        // to timing slop than a spun CPU core for <1ms is costly.
        Uint64 now = SDL_GetPerformanceCounter();
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
            // Running behind (host hiccup, slow frame, etc.) - resync to
            // "now" instead of trying to burn through a backlog of frames
            // back-to-back, which would just fast-forward audio/video.
            nextFrameDeadline = now + targetFrameTicks;
        }
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
