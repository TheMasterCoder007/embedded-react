/*
 * Copyright 2026 Cory Lamming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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

#include "er_perf.h"
#include "er_runtime.h"
#include "er_scene.h"
#include "native_renderer.h"
#include "sdl_backend.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief App file suffixes, and their lengths without the terminator. */
#define BYTECODE_SUFFIX ".qbc"
#define BYTECODE_SUFFIX_LENGTH (sizeof(BYTECODE_SUFFIX) - 1)
#define CONTAINER_SUFFIX ".erpkg"
#define CONTAINER_SUFFIX_LENGTH (sizeof(CONTAINER_SUFFIX) - 1)

#define US_PER_SECOND 1000000ULL

/** @brief Bytes for the config slot's path. */
#define SLOT_PATH_BYTES 1024

/** @brief A simulated frame, and how many a screenshot settles for: at start, and after each tap. */
#define FRAME_MS 16
#define SCREENSHOT_SETTLE_FRAMES 16
#define TAP_SETTLE_FRAMES 8

#define SCREENSHOT_BITS_PER_PIXEL 32

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — file helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Reads an entire file into a newly allocated, null-terminated buffer.
 *
 * @param[in]  path     Filesystem path to read.
 * @param[out] out_length  Receives the byte length (excluding the appended terminator).
 *
 * @return Heap buffer the caller must free(), or NULL if the file could not be read.
 */
static char* read_file(const char* path, size_t* out_length)
{
    FILE* file = fopen(path, "rb");
    if (!file)
    {
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0)
    {
        fclose(file);
        return NULL;
    }
    char* buffer = (char*)malloc((size_t)size + 1);
    if (!buffer)
    {
        fclose(file);
        return NULL;
    }
    const size_t bytes_read = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    buffer[bytes_read] = '\0';
    *out_length = bytes_read;
    return buffer;
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
    const size_t length = strlen(path);
    return length >= BYTECODE_SUFFIX_LENGTH && strcmp(path + length - BYTECODE_SUFFIX_LENGTH, BYTECODE_SUFFIX) == 0;
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
    const size_t length = strlen(path);
    return length >= CONTAINER_SUFFIX_LENGTH && strcmp(path + length - CONTAINER_SUFFIX_LENGTH, CONTAINER_SUFFIX) == 0;
}

/**
 * @brief Reads a boolean env var: true when set to anything but empty or "0".
 *
 * @param[in] name  Environment variable name.
 *
 * @return true when the flag is on.
 */
static bool env_flag(const char* name)
{
    const char* value = SDL_getenv(name);
    return value && value[0] && strcmp(value, "0") != 0;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Microsecond clock for the frame instrumentation (er_perf.h).
 *
 * Whole seconds and the sub-second remainder are scaled separately: the counter is nanoseconds on
 * macOS, so the naive counter * 1000000 would overflow 64 bits within hours of uptime. The truncation
 * to 32 bits wraps every ~71 minutes, which er_perf's unsigned deltas handle.
 */
static uint32_t host_now_us(void)
{
    static Uint64 frequency = 0;
    if (!frequency)
        frequency = SDL_GetPerformanceFrequency();
    const Uint64 now = SDL_GetPerformanceCounter();
    return (uint32_t)((now / frequency) * US_PER_SECOND + (now % frequency) * US_PER_SECOND / frequency);
}

bool er_host_start(const ErHostConfig* config, ErHost* host)
{
    memset(host, 0, sizeof(*host));

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    er_perf_set_clock(host_now_us); /* frame timing split; a no-op when ER_PERF_STATS is off */

    Uint32 window_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI;
    if (env_flag("ER_NO_HIDPI"))
    {
        window_flags &= ~(Uint32)SDL_WINDOW_ALLOW_HIGHDPI;
    }

    host->window = SDL_CreateWindow(config->title ? config->title : "embedded-react",
                                    SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED,
                                    config->width,
                                    config->height,
                                    window_flags);
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

    host->physical_width = config->width;
    host->physical_height = config->height;
    SDL_GetRendererOutputSize(host->renderer, &host->physical_width, &host->physical_height);
    host->dpi_scale = (float)host->physical_width / (float)config->width;

    if (!er_sdl_backend_init(host->renderer, host->physical_width, host->physical_height))
    {
        SDL_Log("er_sdl_backend_init failed");
        SDL_DestroyRenderer(host->renderer);
        SDL_DestroyWindow(host->window);
        SDL_Quit();
        return false;
    }

    /* Boot the portable QuickJS host core: bridge + console (→ stdout) + screen [+ __erPersist]. */
    const ErRuntimeConfig runtime_config = {
        .screen_width = host->physical_width,
        .screen_height = host->physical_height,
        .screen_scale = host->dpi_scale,
        .install_persist = config->persist,
        .log = NULL, /* stdout */
        .natives = config->natives,
        .native_count = config->native_count,
    };
    if (!er_runtime_init(&runtime_config))
    {
        SDL_Log("er_runtime_init failed");
        er_sdl_backend_destroy();
        SDL_DestroyRenderer(host->renderer);
        SDL_DestroyWindow(host->window);
        SDL_Quit();
        return false;
    }

    host->running = true;
    host->previous_ticks = SDL_GetTicks();
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
    size_t length = 0;
    char* loaded = read_file(explicit_path, &length);
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
        const ErContainerStatus status = er_runtime_load_container(loaded, length);
        if (status != ER_CONTAINER_OK)
        {
            const char* reason = er_runtime_container_status_name(status);
            SDL_Log("config rejected: %s", reason);
            er_runtime_show_message("Couldn't load config", reason, "Re-pack with `npm run pack`.");
        }
        return status == ER_CONTAINER_OK;
    }

    const bool ok = is_bytecode_path(explicit_path) ? er_runtime_load_bytecode(loaded, length)
                                                    : er_runtime_load_source(loaded, length, explicit_path);
    free(loaded);
    if (!ok)
    {
        er_runtime_show_error(); /* JS error → error screen; the caller need not show one */
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
    char slot[SLOT_PATH_BYTES];
    char* base = SDL_GetBasePath();
    snprintf(slot, sizeof(slot), "%sapp.erpkg", base ? base : "");
    if (base)
    {
        SDL_free(base);
    }

    size_t length = 0;
    char* loaded = read_file(slot, &length); /* kept alive on success (container references it) */
    if (!loaded)
    {
        SDL_Log("no config in slot (%s)", slot);
        er_runtime_show_message("No config loaded",
                                "No config (app.erpkg) was found in the config slot next to the executable.",
                                "Build one with `npm run pack`, then place/upload it and restart.");
        return false;
    }

    const ErContainerStatus status = er_runtime_load_container(loaded, length);
    if (status != ER_CONTAINER_OK)
    {
        const char* reason = er_runtime_container_status_name(status);
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
    er_perf_frame_begin(); /* frame instrumentation: JS/layout/raster/present split (er_perf.h) */

    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_QUIT)
        {
            host->running = false;
        }
        else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
        {
            host->running = false;
        }
        else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_r)
        {
            host->reload_requested = true; /* the simulator acts on this; the demo ignores it */
        }
        else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT)
        {
            embedded_renderer_touch(
                0, ER_TOUCH_DOWN, er_host_event_px(host, event.button.x), er_host_event_px(host, event.button.y));
        }
        else if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT)
        {
            embedded_renderer_touch(
                0, ER_TOUCH_UP, er_host_event_px(host, event.button.x), er_host_event_px(host, event.button.y));
        }
        else if (event.type == SDL_MOUSEMOTION && (event.motion.state & SDL_BUTTON_LMASK))
        {
            embedded_renderer_touch(
                0, ER_TOUCH_MOVE, er_host_event_px(host, event.motion.x), er_host_event_px(host, event.motion.y));
        }
    }

    /* Service Promises and setTimeout/setInterval before painting, so timer/async-driven state
       updates land in this frame's commit. The engine clock used by timers was advanced by last
       frame's embedded_renderer_tick. */
    er_perf_phase_begin(ER_PERF_PHASE_JS);
    er_runtime_pump();
    er_perf_phase_end(ER_PERF_PHASE_JS);

    er_commit(); /* times its own LAYOUT + RASTER phases */

    er_perf_phase_begin(ER_PERF_PHASE_PRESENT);
    er_sdl_present();
    er_perf_phase_end(ER_PERF_PHASE_PRESENT);

    const uint32_t now = SDL_GetTicks();
    embedded_renderer_tick(now - host->previous_ticks);
    host->previous_ticks = now;

    er_perf_frame_end();

    return host->running;
}

void er_host_run(ErHost* host)
{
    host->previous_ticks = SDL_GetTicks();
    while (er_host_step(host))
    {
        /* step until quit */
    }
}
/** Renders a fixed number of frames (pump → commit → present → tick), the headless settle used below. */
static void er_host_settle(int frames)
{
    for (int frame = 0; frame < frames; frame++)
    {
        er_runtime_pump();
        er_commit();
        er_sdl_present();
        embedded_renderer_tick(FRAME_MS);
    }
}

void er_host_screenshot(ErHost* host, const char* path)
{
    er_host_settle(SCREENSHOT_SETTLE_FRAMES); /* let React mount + the scene render + any intro animation settle */

    /* Optional taps (physical pixels, parity with linux-aot's ER_AOT_TAPS): tap each + re-settle. */
    const char* cursor = SDL_getenv("ER_TAPS");
    while (cursor && *cursor)
    {
        while (*cursor == ' ')
            cursor++;
        if (!*cursor)
            break;
        const int tap_x = SDL_atoi(cursor);
        const char* comma = SDL_strchr(cursor, ',');
        const int tap_y = comma ? SDL_atoi(comma + 1) : 0;
        embedded_renderer_touch(0, ER_TOUCH_DOWN, tap_x, tap_y);
        embedded_renderer_touch(0, ER_TOUCH_UP, tap_x, tap_y);
        er_host_settle(TAP_SETTLE_FRAMES);
        while (*cursor && *cursor != ' ')
            cursor++;
    }

    er_commit();
    er_sdl_present();
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(
        0, host->physical_width, host->physical_height, SCREENSHOT_BITS_PER_PIXEL, SDL_PIXELFORMAT_ARGB8888);
    if (surface
        && SDL_RenderReadPixels(host->renderer, NULL, SDL_PIXELFORMAT_ARGB8888, surface->pixels, surface->pitch) == 0)
    {
        SDL_SaveBMP(surface, path);
        SDL_Log("wrote screenshot %s", path);
    }
    else
    {
        SDL_Log("screenshot failed: %s", SDL_GetError());
    }
    if (surface)
    {
        SDL_FreeSurface(surface);
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
