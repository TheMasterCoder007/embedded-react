/*
 * Desktop host — the SDL layer over the portable QuickJS host core (er_runtime). It opens an SDL
 * window + the SDL backend, loads an app (a config container from the "config slot", or an explicit
 * bundle/bytecode/container path), and drives the frame loop (input → touch, pump, commit, present,
 * tick). Everything JS-side — context lifecycle, console/screen/persist globals, eval/reset/overlay —
 * lives in er_runtime (bridges/quickjs), shared with MCU firmware. main() is a thin driver; the
 * simulator adds watch + reload on er_host_step.
 *
 * The desktop demo mirrors the device's uploaded-config model: it ships NO app and NO baked assets,
 * and at boot loads a config container (app.erpkg) from a fixed slot next to the executable — exactly
 * as an MCU loads its config from a flash region. No config / a corrupt config → an on-screen
 * "Couldn't load config" panel (er_host_load_config), not a built-in fallback. The simulator instead
 * passes an explicit source bundle + a hot-reloadable asset pack (er_host_load_app).
 */

/* Must be defined before any SDL header to disable SDL's main() macro on Windows. */
#define SDL_MAIN_HANDLED

#include "host.h"

#include "er_runtime.h"
#include "er_scene.h"
#include "native_renderer.h"
#include "sdl_backend.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — file helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Reads an entire file into a newly allocated, null-terminated buffer.
 *
 * @param[in]  path     Filesystem path to read.
 * @param[out] out_len  Receives the byte length (excluding the appended terminator).
 *
 * @return Heap buffer the caller must free(), or NULL if the file could not be read.
 */
static char* read_file(const char* path, size_t* out_len)
{
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0)
    {
        fclose(f);
        return NULL;
    }
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    const size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    *out_len = rd;
    return buf;
}

/**
 * @brief Returns true when a path ends in ".qbc" (a compiled bytecode blob).
 *
 * @param[in] path  File path.
 *
 * @return true for a bytecode path, false for JS source.
 */
static bool is_bytecode_path(const char* path)
{
    const size_t n = strlen(path);
    return n >= 4 && strcmp(path + n - 4, ".qbc") == 0;
}

/**
 * @brief Returns true when a path ends in ".erpkg" (an ERCF config container).
 *
 * @param[in] path  File path.
 *
 * @return true for a container path.
 */
static bool is_container_path(const char* path)
{
    const size_t n = strlen(path);
    return n >= 6 && strcmp(path + n - 6, ".erpkg") == 0;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

bool er_host_start(const ErHostConfig* cfg, ErHost* host)
{
    memset(host, 0, sizeof(*host));

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    host->window = SDL_CreateWindow(cfg->title ? cfg->title : "embedded-react",
                                    SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED,
                                    cfg->width,
                                    cfg->height,
                                    SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!host->window)
    {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return false;
    }

    host->renderer = SDL_CreateRenderer(host->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!host->renderer)
    {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(host->window);
        SDL_Quit();
        return false;
    }

    host->phys_w = cfg->width;
    host->phys_h = cfg->height;
    SDL_GetRendererOutputSize(host->renderer, &host->phys_w, &host->phys_h);
    host->dpi_scale = (float)host->phys_w / (float)cfg->width;

    if (!er_sdl_backend_init(host->renderer, host->phys_w, host->phys_h))
    {
        SDL_Log("er_sdl_backend_init failed");
        SDL_DestroyRenderer(host->renderer);
        SDL_DestroyWindow(host->window);
        SDL_Quit();
        return false;
    }

    /* Boot the portable QuickJS host core: bridge + console (→ stdout) + screen [+ __erPersist]. */
    const ErRuntimeConfig rt = {
        .screen_width = host->phys_w,
        .screen_height = host->phys_h,
        .screen_scale = host->dpi_scale,
        .install_persist = cfg->persist,
        .log = NULL, /* stdout */
    };
    if (!er_runtime_init(&rt))
    {
        SDL_Log("er_runtime_init failed");
        er_sdl_backend_destroy();
        SDL_DestroyRenderer(host->renderer);
        SDL_DestroyWindow(host->window);
        SDL_Quit();
        return false;
    }

    host->running = true;
    host->prev_ticks = SDL_GetTicks();
    return true;
}

bool er_host_load_app(ErHost* host, const char* explicit_path)
{
    (void)host;
    /* Load an explicit app file, dispatched by extension: .erpkg = config container, .qbc = bytecode,
       else JS source. There is no built-in fallback and no next-to-exe discovery — the demo loads its
       config slot via er_host_load_config; the simulator passes the watched source bundle here. */
    if (!explicit_path)
    {
        return false;
    }
    size_t len = 0;
    char* loaded = read_file(explicit_path, &len);
    if (!loaded)
    {
        SDL_Log("could not read '%s'", explicit_path);
        return false;
    }

    const bool container = is_container_path(explicit_path);
    SDL_Log("running %s (%s)",
            explicit_path,
            container                         ? "container"
            : is_bytecode_path(explicit_path) ? "bytecode"
                                              : "source");

    if (container)
    {
        /* The container buffer is referenced (asset pixels/font bitmaps point into it), so it must
           outlive the app — keep `loaded` alive deliberately (one app per process; a reload re-reads
           the file). On failure the buffer is also kept: asset sections may already point into it. */
        const ErContainerStatus st = er_runtime_load_container(loaded, len);
        if (st != ER_CONTAINER_OK)
        {
            const char* reason = er_runtime_container_status_str(st);
            SDL_Log("config rejected: %s", reason);
            er_runtime_show_message("Couldn't load config", reason, "Re-pack with `npm run pack`.");
        }
        return st == ER_CONTAINER_OK;
    }

    const bool ok = is_bytecode_path(explicit_path) ? er_runtime_load_bytecode(loaded, len)
                                                    : er_runtime_load_source(loaded, len, explicit_path);
    free(loaded);
    if (!ok)
    {
        er_runtime_show_error(); /* JS error → redbox; the caller need not render one */
    }
    return ok;
}

bool er_host_load_config(ErHost* host)
{
    (void)host;
    /* Device-parity load: read the config container from the fixed "config slot" — app.erpkg next to
       the executable, the desktop analog of a flash/config region — and run it. No config, or a
       corrupt/incompatible one, paints an on-screen panel and returns false (no built-in fallback).
       The caller keeps the window up so the panel is visible (just as firmware would). */
    char slot[1024];
    char* base = SDL_GetBasePath();
    snprintf(slot, sizeof(slot), "%sapp.erpkg", base ? base : "");
    if (base)
    {
        SDL_free(base);
    }

    size_t len = 0;
    char* loaded = read_file(slot, &len); /* kept alive on success (container references it) */
    if (!loaded)
    {
        SDL_Log("no config in slot (%s)", slot);
        er_runtime_show_message("No config loaded",
                                "No config (app.erpkg) was found in the config slot next to the executable.",
                                "Build one with `npm run pack`, then place/upload it and restart.");
        return false;
    }

    const ErContainerStatus st = er_runtime_load_container(loaded, len);
    if (st != ER_CONTAINER_OK)
    {
        const char* reason = er_runtime_container_status_str(st);
        SDL_Log("config rejected: %s", reason);
        er_runtime_show_message("Couldn't load config", reason, "Re-pack with `npm run pack`, then restart.");
        return false; /* `loaded` kept: asset sections may already reference it */
    }
    SDL_Log("loaded config from %s", slot);
    return true;
}

bool er_host_reload(ErHost* host, const char* explicit_path)
{
    /* Live reload (full remount): er_runtime drops the JS context + resets the engine, then we re-run
     * the app. Component state is not preserved (use usePersistentState). Registered images/fonts
     * survive the reset, so baked assets keep resolving. */
    er_runtime_reset();
    return er_host_load_app(host, explicit_path);
}

void er_host_clear_persist(void)
{
    er_runtime_clear_persist();
}

void er_host_show_error(ErHost* host)
{
    (void)host;
    er_runtime_show_error(); /* the next er_host_step commit paints it */
}

int er_host_event_px(const ErHost* host, int logical)
{
    return (int)((float)logical * host->dpi_scale + 0.5f);
}

bool er_host_step(ErHost* host)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
    {
        if (ev.type == SDL_QUIT)
        {
            host->running = false;
        }
        else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
        {
            host->running = false;
        }
        else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_r)
        {
            host->reload_requested = true; /* the simulator acts on this; the demo ignores it */
        }
        else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT)
        {
            embedded_renderer_touch(
                0, ER_TOUCH_DOWN, er_host_event_px(host, ev.button.x), er_host_event_px(host, ev.button.y));
        }
        else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT)
        {
            embedded_renderer_touch(
                0, ER_TOUCH_UP, er_host_event_px(host, ev.button.x), er_host_event_px(host, ev.button.y));
        }
        else if (ev.type == SDL_MOUSEMOTION && (ev.motion.state & SDL_BUTTON_LMASK))
        {
            embedded_renderer_touch(
                0, ER_TOUCH_MOVE, er_host_event_px(host, ev.motion.x), er_host_event_px(host, ev.motion.y));
        }
    }

    /* Service Promises and setTimeout/setInterval before painting, so timer/async-driven state
       updates land in this frame's commit. The engine clock used by timers was advanced by last
       frame's embedded_renderer_tick. */
    er_runtime_pump();

    er_commit();
    er_sdl_present();

    const uint32_t now = SDL_GetTicks();
    embedded_renderer_tick(now - host->prev_ticks);
    host->prev_ticks = now;

    return host->running;
}

void er_host_run(ErHost* host)
{
    host->prev_ticks = SDL_GetTicks();
    while (er_host_step(host))
    {
        /* step until quit */
    }
}

void er_host_shutdown(ErHost* host)
{
    er_runtime_shutdown();
    er_sdl_backend_destroy();
    if (host->renderer)
    {
        SDL_DestroyRenderer(host->renderer);
        host->renderer = NULL;
    }
    if (host->window)
    {
        SDL_DestroyWindow(host->window);
        host->window = NULL;
    }
    SDL_Quit();
}
