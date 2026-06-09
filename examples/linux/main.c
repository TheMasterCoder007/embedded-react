/*
 * embedded-react-desktop — the desktop demo.
 *
 * A board-like example (peer to examples/esp32/esp32-s3): it runs an uploaded config the same way the
 * MCU does — the firmware (this exe) ships no app and no baked assets, and at boot loads a config
 * container (app.erpkg) from the "config slot" next to the executable. No config / a corrupt one shows
 * an on-screen panel and the window stays up (no built-in fallback), just like firmware. It is a
 * *demo*, not the dev tool — hot-reload-driven development lives in the simulator (see /SIMULATOR.md).
 * All the runtime lives in the shared host core (host.c); main() just drives it.
 *
 * Build a config with `npm run pack` (in bridges/quickjs/js); the build copies it into the slot. To
 * "upload" a new one, drop a fresh app.erpkg next to the exe and restart.
 *
 * Usage:
 *   embedded-react-desktop [app.erpkg|app.js|app.qbc]
 * With no argument it loads app.erpkg from the slot. An explicit path overrides the slot (handy for
 * testing a specific config/bundle).
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

    /* No app arg → load the config slot (device parity). An explicit path overrides it. Both render an
       on-screen panel on failure (and keep the window up — firmware doesn't exit when a config is
       missing/bad), so we ignore the result and just run the loop. */
    if (argc > 1)
    {
        er_host_load_app(&host, argv[1]);
    }
    else
    {
        er_host_load_config(&host);
    }

    er_host_run(&host);
    er_host_shutdown(&host);
    return 0;
}
