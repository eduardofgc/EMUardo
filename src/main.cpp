#include "frontend/app.h"

int main(int argc, char** argv) {
    gba::frontend::App app;
    if (!app.Init()) {
        return 1;
    }

    // Preserves the old "./gba_emulator rom.gba" direct-launch workflow -
    // skips the splash/menu and starts playing immediately.
    if (argc > 1) {
        app.LoadRomDirectly(argv[1]);
    }

    app.Run();
    app.Shutdown();
    return 0;
}
