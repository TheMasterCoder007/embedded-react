/*
 * embedded-react-desktop — the desktop demo.
 *
 * A board-like example (peer to examples/esp32/esp32-s3): it runs the active JSX bundle on the
 * desktop via SDL2, with the same QuickJS bridge + engine the MCU uses and only the backend swapped.
 * It is a *demo*, not the dev tool — hot-reload-driven development lives in the simulator (see
 * /SIMULATOR.md). All the runtime lives in the shared host core (host.c); main() just drives it.
 *
 * Usage:
 *   embedded-react-desktop [app.js|app.qbc]
 * With no argument it loads the bundle next to the executable (app.bundle.qbc / .js), falling back
 * to a small built-in app.
 */

#define SDL_MAIN_HANDLED

#include "host.h"

#include <SDL2/SDL.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define SCREEN_W 800
#define SCREEN_H 600

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Entry point: start the host, load the app, run the frame loop, shut down.
 *
 * @param[in] argc  Argument count.
 * @param[in] argv  Optional argv[1] = path to a JS/bytecode app (else the bundle / built-in app).
 *
 * @return 0 on clean exit; non-zero on setup or JS evaluation failure.
 */
int main(int argc, char** argv)
{
    const ErHostConfig cfg = {"embedded-react — desktop demo", SCREEN_W, SCREEN_H};
    ErHost host;
    if (!er_host_start(&cfg, &host))
    {
        return 1;
    }

    if (!er_host_load_app(&host, argc > 1 ? argv[1] : NULL))
    {
        er_host_shutdown(&host);
        return 1;
    }

    er_host_run(&host);
    er_host_shutdown(&host);
    return 0;
}
