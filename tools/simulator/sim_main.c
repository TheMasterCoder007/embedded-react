/*
 * embedded-react simulator — the dev tool (see /SIMULATOR.md).
 *
 * Runs a JSX bundle on the desktop SDL host (the shared host core, examples/linux/host.c) and
 * LIVE-RELOADS it whenever the watched bundle file changes on disk. This is the file-watch transport
 * + full-remount reload of the MVP: `npm run sim` runs esbuild in watch mode to rewrite the bundle on
 * save, and this process notices the change and re-runs the app — the React Native inner loop on the
 * desktop. Unlike the demo, a JS error doesn't exit: the window stays up and reloads once you fix it.
 *
 * Usage: embedded-react-simulator <path-to-app.bundle.js>
 */

#define SDL_MAIN_HANDLED

#include "asset_pack.h"
#include "host.h"

#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

/** @brief How often (ms) the watched bundle is polled for changes. */
#define SIM_WATCH_INTERVAL_MS 200

/**
 * @brief Computes a change signature (modification time + size) for a file.
 *
 * @param[in] path  File to stat.
 *
 * @return A non-zero signature that changes when the file is rewritten, or 0 if it can't be stat'd
 *         (e.g. mid-write or missing).
 */
static uint64_t file_signature(const char* path)
{
    struct stat st;
    if (!path || stat(path, &st) != 0)
    {
        return 0;
    }
    const uint64_t sig = (uint64_t)st.st_mtime * 1000003u ^ (uint64_t)st.st_size;
    return sig ? sig : 1u; /* never return 0 for a valid file */
}

/**
 * @brief Simulator entry point: start the host, load the watched bundle, then reload it on change.
 *
 * @param[in] argc  Argument count.
 * @param[in] argv  argv[1] = path to the watched app bundle (e.g. dist/app.bundle.js).
 *
 * @return 0 on clean exit; 1 on host setup failure; 2 on bad usage.
 */
int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: embedded-react-simulator <app.bundle.js> [assets.pack]\n");
        return 2;
    }
    const char* app_path = argv[1];
    const char* pack_path = argc > 2 ? argv[2] : NULL;

    const ErHostConfig cfg = {"embedded-react — simulator", 800, 600, /*persist=*/true};
    ErHost host;
    if (!er_host_start(&cfg, &host))
    {
        return 1;
    }

    /* Load baked assets from the pack BEFORE the app evaluates, so <Image>/<Text> resolve. The sim
     * loads assets at runtime (unlike the demo/device, which compile them in) so they hot-reload. */
    if (pack_path && er_asset_pack_load(pack_path))
    {
        SDL_Log("loaded assets from %s", pack_path);
    }

    /* Initial load. Don't bail on a JS error: show it on screen and stay up so the next save reloads. */
    if (!er_host_load_app(&host, app_path))
    {
        er_host_show_error(&host);
    }
    SDL_Log("simulator watching %s — edit & save to reload, or press R", app_path);

    uint64_t last_bundle_sig = file_signature(app_path);
    uint64_t last_pack_sig = pack_path ? file_signature(pack_path) : 0;
    uint32_t last_check = SDL_GetTicks();

    while (er_host_step(&host))
    {
        bool do_reload = false;

        /* Manual reload (R key): a "hard" reload that also drops persisted state, so the app remounts
         * fresh. (A save-triggered reload below keeps usePersistentState across the edit.) */
        if (host.reload_requested)
        {
            host.reload_requested = false;
            er_host_clear_persist();
            do_reload = true;
            SDL_Log("manual reload (state reset)");
        }

        /* File-watch: poll the bundle and the asset pack periodically. */
        const uint32_t now = SDL_GetTicks();
        if (now - last_check >= SIM_WATCH_INTERVAL_MS)
        {
            last_check = now;
            const uint64_t bsig = file_signature(app_path);
            if (bsig != 0 && bsig != last_bundle_sig)
            {
                last_bundle_sig = bsig;
                do_reload = true;
                SDL_Log("change detected — reloading %s", app_path);
            }
            if (pack_path)
            {
                const uint64_t psig = file_signature(pack_path);
                if (psig != 0 && psig != last_pack_sig)
                {
                    last_pack_sig = psig;
                    if (er_asset_pack_load(pack_path))
                    {
                        do_reload = true; /* re-render with the new assets */
                        SDL_Log("assets changed — reloaded %s", pack_path);
                    }
                }
            }
        }

        if (do_reload)
        {
            if (!er_host_reload(&host, app_path))
            {
                er_host_show_error(&host); /* on-screen error; stays up for the next save */
            }
            last_check = SDL_GetTicks();
        }
    }

    er_host_shutdown(&host);
    return 0;
}
