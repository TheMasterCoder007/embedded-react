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
 * The Flow B simulator: the Flow B runtime in WebAssembly, loading a package and rendering through the software
 * backend into an RGBA buffer its host shows. The runtime is built with the debug hook, and erb_debug_break() calls
 * the host, which serves a debugger from there (packages/embedded-react/aot-sim).
 */

#include "aot_backend.h"

#include "erb_debug.h"
#include "erb_runtime.h"
#include "native_renderer.h"
#include "software_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

/** @brief Bytes per pixel in the RGBA buffer. */
#define RGBA_BYTES_PER_PIXEL 4U

/** @brief An alpha of this value is fully opaque. */
#define ALPHA_OPAQUE 0xFFU

/** @brief The touch phases er_web_aot_touch() takes from the host. */
#define WEB_TOUCH_DOWN 0
#define WEB_TOUCH_MOVE 1
#define WEB_TOUCH_UP 2

/** @brief The arena: more than a board reserves, so the simulator isn't what refuses an app. */
#define ARENA_BYTES (1024u * 1024u)

/** @brief Fonts a package may bring, one per family and size. */
#define FONT_COUNT 128u

/** @brief A package's alignment in memory, as a board places it. */
#define PACKAGE_ALIGNMENT 4u

/** @brief Room for an error's message. */
#define ERROR_MESSAGE_BYTES 512

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief A package kept in memory after a load that failed. */
typedef struct KeptPackage
{
    struct KeptPackage* next;
    uint32_t* package;
} KeptPackage;

static uint32_t s_arena[ARENA_BYTES / sizeof(uint32_t)];
static BitmapFont s_fonts[FONT_COUNT];
static ErAssetsStorage s_font_storage = {.fonts = s_fonts, .font_count = FONT_COUNT};

static uint8_t* s_rgba;  /**< The panel as RGBA bytes, for a canvas. */
static int s_width;      /**< The app's width in pixels. */
static int s_height;     /**< The app's height in pixels. */
static int s_dirty_top;  /**< The first row the last pump changed, or -1 for none. */
static int s_dirty_rows; /**< How many rows from there. */

static uint32_t* s_package;                 /**< The running package, which the runtime reads in place. */
static struct KeptPackage* s_kept_packages; /**< Packages the runtime may still read, after loads that failed. */
static size_t s_package_bytes;              /**< Its size. */
static char s_error[ERROR_MESSAGE_BYTES];   /**< Why the last load was refused. */

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

#ifdef __EMSCRIPTEN__
/* clang-format off */
EM_JS(void, host_debug_break, (void), {
    if (Module["onDebugBreak"]) Module["onDebugBreak"]();
});
EM_JS(void, host_log, (const char* line), {
    if (Module["onAppLog"]) Module["onAppLog"](UTF8ToString(line));
});
/* clang-format on */
#else
static void host_debug_break(void)
{
}
static void host_log(const char* line)
{
    puts(line);
}
#endif

/** @brief Where the runtime's hook stops: the host decides whether the app waits there. */
void erb_debug_host_break(void)
{
    host_debug_break();
}

static void log_line(const char* line, void* context)
{
    (void)context;
    host_log(line);
}

/** @brief Brings the panel up to date, converts it to RGBA and notes the rows that changed. */
static void present(void)
{
    er_software_present();
    const uint32_t* panel = er_software_panel();
    s_dirty_top = -1;
    s_dirty_rows = 0;
    if (!panel || !s_rgba)
    {
        return;
    }
    int last_row = -1;
    for (int row = 0; row < s_height; row++)
    {
        const uint32_t* source = panel + (size_t)row * (size_t)s_width;
        uint8_t* output = s_rgba + (size_t)row * (size_t)s_width * RGBA_BYTES_PER_PIXEL;
        bool changed = false;
        for (int column = 0; column < s_width; column++, output += RGBA_BYTES_PER_PIXEL)
        {
            const uint32_t pixel = source[column];
            const uint8_t red = (uint8_t)(pixel >> 16);
            const uint8_t green = (uint8_t)(pixel >> 8);
            const uint8_t blue = (uint8_t)pixel;
            if (output[0] != red || output[1] != green || output[2] != blue || output[3] != ALPHA_OPAQUE)
            {
                output[0] = red;
                output[1] = green;
                output[2] = blue;
                output[3] = ALPHA_OPAQUE;
                changed = true;
            }
        }
        if (changed)
        {
            if (s_dirty_top < 0)
            {
                s_dirty_top = row;
            }
            last_row = row;
        }
    }
    if (s_dirty_top >= 0)
    {
        s_dirty_rows = last_row - s_dirty_top + 1;
    }
}

/** @brief Keeps a package the runtime may still read. */
static void keep_package(uint32_t* package)
{
    KeptPackage* kept = package ? malloc(sizeof(*kept)) : NULL;
    if (kept)
    {
        kept->package = package;
        kept->next = s_kept_packages;
        s_kept_packages = kept;
    }
}

/** @brief Frees the kept packages, once the runtime reads none of them. */
static void free_kept_packages(void)
{
    while (s_kept_packages)
    {
        KeptPackage* kept = s_kept_packages;
        s_kept_packages = kept->next;
        free(kept->package);
        free(kept);
    }
}

/** @brief The config every load uses, at the simulator's size. */
static ErbConfig runtime_config(void)
{
    ErbConfig config;
    memset(&config, 0, sizeof(config));
    config.arena = s_arena;
    config.arena_bytes = sizeof(s_arena);
    config.screen_width = (uint16_t)s_width;
    config.screen_height = (uint16_t)s_height;
    config.fonts = &s_font_storage;
    config.log = log_line;
    return config;
}

/** @brief Loads a package the simulator holds. A refused one leaves its reason in s_error. */
static bool load(const uint32_t* package, size_t package_bytes)
{
    const ErbConfig config = runtime_config();
    ErbError error;
    if (erb_load(package, package_bytes, &config, &error))
    {
        s_error[0] = '\0';
        return true;
    }
    erb_error_message(&error, s_error, sizeof(s_error));
    if (!erb_running())
    {
        erb_show_error(&error);
    }
    return false;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public (exported ABI)
 ---------------------------------------------------------------------------------------------------------------------*/

EMSCRIPTEN_KEEPALIVE
int er_web_aot_init(int screen_width, int screen_height)
{
    if (screen_width <= 0 || screen_height <= 0 || !er_software_backend_init(screen_width, screen_height))
    {
        return 0;
    }
    s_rgba = calloc((size_t)screen_width * (size_t)screen_height, RGBA_BYTES_PER_PIXEL);
    if (!s_rgba)
    {
        er_software_backend_destroy();
        return 0;
    }
    s_width = screen_width;
    s_height = screen_height;
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int er_web_aot_load(const uint8_t* package, int length, double wall_clock_ms)
{
    if (!package || length <= 0)
    {
        return 0;
    }
    const size_t package_bytes = (size_t)length;
    uint32_t* copy = calloc((package_bytes + PACKAGE_ALIGNMENT) & ~(size_t)(PACKAGE_ALIGNMENT - 1u), 1);
    if (!copy)
    {
        snprintf(s_error, sizeof(s_error), "the simulator has no memory for the package");
        return 0;
    }
    memcpy(copy, package, package_bytes);
    erb_set_wall_clock((int64_t)wall_clock_ms);
    const bool loaded = load(copy, package_bytes);
    if (!loaded && erb_running())
    {
        /* The package was refused, and the app that was running still reads its own. */
        free(copy);
        return 0;
    }
    if (loaded)
    {
        /* The runtime reads the new package alone now. */
        free_kept_packages();
        free(s_package);
    }
    else
    {
        /* Nothing runs. A package that passed its checks and stopped as it started took the old app's place, and one
           that was refused didn't, so the runtime may read either: both are kept until a package loads. */
        keep_package(s_package);
    }
    s_package = copy;
    s_package_bytes = package_bytes;
    return loaded ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
const char* er_web_aot_error(void)
{
    return s_error;
}

EMSCRIPTEN_KEEPALIVE
int er_web_aot_running(void)
{
    return erb_running() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int er_web_aot_resize(int screen_width, int screen_height)
{
    if (screen_width <= 0 || screen_height <= 0)
    {
        return 0;
    }
    uint8_t* rgba = calloc((size_t)screen_width * (size_t)screen_height, RGBA_BYTES_PER_PIXEL);
    if (!rgba)
    {
        return 0;
    }
    if (!er_software_backend_resize(screen_width, screen_height))
    {
        free(rgba);
        return 0;
    }
    uint8_t* previous_rgba = s_rgba;
    const int previous_width = s_width;
    const int previous_height = s_height;
    s_rgba = rgba;
    s_width = screen_width;
    s_height = screen_height;
    /* A size that picks another layout starts the app again in it. */
    if (s_package && !erb_set_screen_size(screen_width, screen_height) && !load(s_package, s_package_bytes)
        && erb_running())
    {
        /* The package has no layout for the size, and the app runs on in the one it has: the panel stays its size. */
        if (er_software_backend_resize(previous_width, previous_height))
        {
            free(rgba);
            s_rgba = previous_rgba;
            s_width = previous_width;
            s_height = previous_height;
            return 0;
        }
    }
    free(previous_rgba);
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int er_web_aot_pump(int delta_ms)
{
    const uint32_t elapsed_ms = delta_ms > 0 ? (uint32_t)delta_ms : 0U;
    embedded_renderer_tick(elapsed_ms);
    erb_tick(elapsed_ms);
    er_commit();
    present();
    return s_dirty_rows;
}

EMSCRIPTEN_KEEPALIVE
void er_web_aot_touch(int phase, int x, int y)
{
    ERTouchPhase touch_phase;
    switch (phase)
    {
        case WEB_TOUCH_DOWN:
            touch_phase = ER_TOUCH_DOWN;
            break;
        case WEB_TOUCH_MOVE:
            touch_phase = ER_TOUCH_MOVE;
            break;
        case WEB_TOUCH_UP:
            touch_phase = ER_TOUCH_UP;
            break;
        default:
            touch_phase = ER_TOUCH_CANCEL;
            break;
    }
    embedded_renderer_touch(0, touch_phase, x, y);
}

EMSCRIPTEN_KEEPALIVE
const uint8_t* er_web_aot_framebuffer(void)
{
    return s_rgba;
}

EMSCRIPTEN_KEEPALIVE
int er_web_aot_dirty_top(void)
{
    return s_dirty_top;
}

EMSCRIPTEN_KEEPALIVE
const void* er_web_aot_debug(void)
{
    return &erb_debug;
}
