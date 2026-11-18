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
 * embedded-react-desktop-aot — the desktop host for Flow B (the AOT path).
 *
 * The Flow B peer of examples/linux: it renders a JSX app in an SDL2 window with NO QuickJS and NO JS
 * at runtime. The app is a Flow B package (dist/app.erpkg, from `npm run aot`), which the Flow B runtime
 * verifies and runs in a fixed arena; this host just brings up SDL + the engine's SDL backend, calls
 * erb_load() once, and runs the frame loop (input → touch, commit, present, tick). Same engine, same
 * backend as Flow A — only the front end (a package vs. interpreted React) differs.
 *
 * This is the desktop proxy for a no-PSRAM MCU: no JS heap, no interpreter, a fixed static footprint.
 *
 *   embedded-react-desktop-aot [package.erpkg]    defaults to packages/embedded-react/dist/app.erpkg
 *
 * T turns the panel a quarter turn while the app runs.
 */

#define SDL_MAIN_HANDLED

#include "desktop_natives.h"
#include "er_scene.h"
#include "erb_runtime.h"
#include "native_renderer.h"
#include "sdl_backend.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Window size, unless ER_AOT_SCREEN_W/H say otherwise. The window's size picks the package's layout. */
#define SCREEN_WIDTH_DEFAULT 800
#define SCREEN_HEIGHT_DEFAULT 480

/** @brief Bytes of the firmware's arena. */
#define ARENA_BYTES (64u * 1024u)

/** @brief Room for a package's font tables: fonts (one per family and size), their glyphs and extra glyphs. */
#define FONT_COUNT 64u
#define GLYPH_COUNT (FONT_COUNT * 128u)
#define EXTRA_GLYPH_COUNT 1024u

/** @brief A package is read into memory aligned to this many bytes. */
#define PACKAGE_ALIGNMENT 4u

#define MS_PER_SECOND 1000

/** @brief A simulated frame, for the headless runs. */
#define FRAME_MS 16

/** @brief How long a headless run lets tap-triggered animations settle before its screenshot. */
#define TAP_SETTLE_MS 300

/** @brief Where ER_AOT_CLICKS taps by default: mid-width, this far down the screen. */
#define CLICK_Y_FRACTION 0.57f

/** @brief Buffer sizes for an error message and a copied env value. */
#define MESSAGE_BYTES 256
#define PATH_BYTES 512

#define SCREENSHOT_BITS_PER_PIXEL 32

/** @brief What the T key turns the panel by, in degrees, and a full turn. */
#define QUARTER_TURN 90
#define FULL_TURN 360

/** @brief The firmware's arena. */
static uint32_t s_arena[ARENA_BYTES / sizeof(uint32_t)];

/** @brief The tables of the fonts a package brings. Their bitmaps stay in the package. */
static BitmapFont s_fonts[FONT_COUNT];
static GlyphInfo s_glyphs[GLYPH_COUNT];
static ExtraGlyph s_extra_glyphs[EXTRA_GLYPH_COUNT];
static ErAssetsStorage s_font_storage = {
    .fonts = s_fonts,
    .font_count = FONT_COUNT,
    .glyphs = s_glyphs,
    .glyph_count = GLYPH_COUNT,
    .extras = s_extra_glyphs,
    .extra_count = EXTRA_GLYPH_COUNT,
};

/** @brief Reads a file into a 4-aligned buffer, as a board places a package. NULL when it can't be read. */
static uint32_t* read_package(const char* path, size_t* bytes)
{
    FILE* file = fopen(path, "rb");
    if (!file)
    {
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint32_t* words =
        length > 0 ? malloc(((size_t)length + (PACKAGE_ALIGNMENT - 1u)) & ~(size_t)(PACKAGE_ALIGNMENT - 1u)) : NULL;
    if (words && fread(words, 1, (size_t)length, file) != (size_t)length)
    {
        free(words);
        words = NULL;
    }
    fclose(file);
    *bytes = (size_t)length;
    return words;
}

/** @brief The runtime's reports, one line each. */
static void log_line(const char* line, void* context)
{
    (void)context;
    SDL_Log("ERB: %s", line);
}

/** @brief Reads an integer env var, returning @p fallback when unset/empty/non-positive. */
static int env_int(const char* name, int fallback)
{
    const char* value = SDL_getenv(name);
    if (!value || !value[0])
    {
        return fallback;
    }
    const int number = SDL_atoi(value);
    return number > 0 ? number : fallback;
}

/** @brief Reads a boolean env var: true when set to anything but empty or "0". */
static bool env_flag(const char* name)
{
    const char* value = SDL_getenv(name);
    return value && value[0] && SDL_strcmp(value, "0") != 0;
}

/** @brief Scales a logical SDL coordinate to physical framebuffer pixels. */
static int to_px(int logical, float scale)
{
    return (int)((float)logical * scale + 0.5f);
}

/** @brief Reads ER_ROTATION (0, 90, 180 or 270); anything else is 0. */
static ERDisplayRotation env_rotation(void)
{
    const int degrees = env_int("ER_ROTATION", 0);
    return degrees == ER_ROTATION_90 || degrees == ER_ROTATION_180 || degrees == ER_ROTATION_270
               ? (ERDisplayRotation)degrees
               : ER_ROTATION_0;
}

/** @brief Forwards a mouse point to the engine as a touch, in panel coordinates. */
static void mouse_touch(ERTouchPhase phase, int window_x, int window_y, float scale)
{
    int x = to_px(window_x, scale);
    int y = to_px(window_y, scale);
    er_sdl_window_point_to_panel(&x, &y);
    embedded_renderer_touch(0, phase, x, y);
}

/**
 * @brief Turns the panel a quarter turn while the app runs, as examples/linux's er_host_set_rotation() does.
 *
 * @param[in]     window    The window, which swaps its sides.
 * @param[in,out] rotation  The display rotation.
 * @param[in,out] width     The app's width in pixels.
 * @param[in,out] height    The app's height in pixels.
 *
 * @return false when the backend can't take the turn, with everything left as it was.
 */
static bool turn_panel(SDL_Window* window, ERDisplayRotation* rotation, int* width, int* height)
{
    /* The panel as it scans, which a turn leaves alone. */
    const ERDisplayRotation turned = (ERDisplayRotation)(((int)*rotation + QUARTER_TURN) % FULL_TURN);
    const bool was_swapped = *rotation == ER_ROTATION_90 || *rotation == ER_ROTATION_270;
    const int panel_width = was_swapped ? *height : *width;
    const int panel_height = was_swapped ? *width : *height;
    if (!er_set_display_rotation(turned, panel_width, panel_height))
    {
        return false;
    }
    /* The window first: the backend's new textures are made for the window as it will be. */
    int window_width = 0;
    int window_height = 0;
    SDL_GetWindowSize(window, &window_width, &window_height);
    SDL_SetWindowSize(window, window_height, window_width);
    if (!er_sdl_backend_resize(*height, *width))
    {
        SDL_SetWindowSize(window, window_width, window_height);
        er_set_display_rotation(*rotation, panel_width, panel_height);
        er_sdl_backend_resize(*width, *height);
        return false;
    }
    const int turned_width = *height;
    *height = *width;
    *width = turned_width;
    *rotation = turned;
    return true;
}

/**
 * @brief Entry point: bring up SDL + the engine, load the package once, run the frame loop.
 *
 * @param[in] argc  The argument count.
 * @param[in] argv  The package's path, as the first argument, or ER_DEFAULT_PACKAGE without one.
 *
 * @return 0 on clean exit; non-zero on a setup failure.
 */
int main(int argc, char** argv)
{
    const char* package_path = argc > 1 ? argv[1] : ER_DEFAULT_PACKAGE;
    size_t package_bytes = 0;
    uint32_t* package = read_package(package_path, &package_bytes);
    if (!package)
    {
        fprintf(stderr,
                "Can't read the package %s. Build one first:\n"
                "    cd packages/embedded-react && npm run aot -- <demo>   (e.g. watch-face)\n",
                package_path);
        return 1;
    }

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    /* Window (app) size. The parity harness sets it through ER_AOT_SCREEN_W/H, the same vars that name the
       panel a package keeps its layout for in the AOT compiler. ER_ROTATION=90|180|270 puts a panel mounted
       that way behind the window, its size the window's turned. */
    const int screen_width = env_int("ER_AOT_SCREEN_W", SCREEN_WIDTH_DEFAULT);
    const int screen_height = env_int("ER_AOT_SCREEN_H", SCREEN_HEIGHT_DEFAULT);

    Uint32 window_flags = SDL_WINDOW_SHOWN;
    if (env_flag("ER_HIDPI") && !env_flag("ER_NO_HIDPI"))
    {
        window_flags |= SDL_WINDOW_ALLOW_HIGHDPI;
    }

    SDL_Window* window = SDL_CreateWindow("embedded-react — desktop (Flow B / AOT)",
                                          SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED,
                                          screen_width,
                                          screen_height,
                                          window_flags);
    if (!window)
    {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
    {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    int physical_width = screen_width;
    int physical_height = screen_height;
    SDL_GetRendererOutputSize(renderer, &physical_width, &physical_height);
    const float scale = (float)physical_width / (float)screen_width;

    ERDisplayRotation rotation = env_rotation();
    const bool swapped = rotation == ER_ROTATION_90 || rotation == ER_ROTATION_270;
    er_set_display_rotation(
        rotation, swapped ? physical_height : physical_width, swapped ? physical_width : physical_height);
    if (!er_sdl_backend_init(renderer, physical_width, physical_height))
    {
        SDL_Log("er_sdl_backend_init failed");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* Anchor Date.now() to the real time before the load, so a mount effect already reads it. A board with
       no RTC would call this once it learns the time (SNTP, GPS). */
    erb_set_wall_clock((int64_t)time(NULL) * MS_PER_SECOND);

    /* Verify the package and build its scene graph at the display's size, which the runtime takes from the
       engine. One that fails shows the error screen, as a board does, and a headless run fails. */
    const ErbConfig config = {
        .arena = s_arena,
        .arena_bytes = sizeof(s_arena),
        .fonts = &s_font_storage,
        .log = log_line,
        .natives = desktop_natives,
        .native_count = desktop_native_count,
    };
    ErbError load_error;
    const bool loaded = erb_load(package, package_bytes, &config, &load_error);
    if (loaded)
    {
        SDL_Log("package %s running at %dx%d (no QuickJS)", package_path, physical_width, physical_height);
    }
    else
    {
        char message[MESSAGE_BYTES];
        erb_error_message(&load_error, message, sizeof(message));
        SDL_Log("the package didn't load: %s", message);
        erb_show_error(&load_error);
    }
    /* The on-screen keyboard is compiled into this host (ERUI_ONSCREEN_KEYBOARD=ON in CMakeLists.txt) and
     * auto-shows whenever a TextInput is focused. The host stays neutral — the app drives its layout/colours
     * via setKeyboardConfig({...}) in App.jsx, else the built-in QWERTY. */

    /* Optional one-shot screenshot for verification: ER_AOT_SHOT=<path.bmp> renders one frame, saves
       it, and exits. Lets a headless run confirm the pixels without a visible window. */
    /* SDL_getenv returns a pointer into a SHARED static buffer on Windows — a later SDL_getenv clobbers
       an earlier result. So copy/convert each value immediately before the next call. */
    char shot[PATH_BYTES] = {0};
    {
        const char* value = SDL_getenv("ER_AOT_SHOT");
        if (value)
        {
            SDL_strlcpy(shot, value, sizeof(shot));
        }
    }
    if (shot[0] && !loaded)
    {
        er_sdl_backend_destroy();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    if (shot[0])
    {
        int clicks = 0;
        int click_x = physical_width / 2;
        int click_y = (int)(physical_height * CLICK_Y_FRACTION);
        {
            const char* value = SDL_getenv("ER_AOT_CLICKS");
            if (value)
            {
                clicks = SDL_atoi(value);
            }
        }
        {
            const char* value = SDL_getenv("ER_AOT_CLICK_X");
            if (value)
            {
                click_x = SDL_atoi(value);
            }
        }
        {
            const char* value = SDL_getenv("ER_AOT_CLICK_Y");
            if (value)
            {
                click_y = SDL_atoi(value);
            }
        }
        /* The click point is in the window, as the app reads; the touch goes in at the panel point under it. */
        int panel_click_x = click_x;
        int panel_click_y = click_y;
        er_sdl_window_point_to_panel(&panel_click_x, &panel_click_y);
        er_commit(); /* compute layout + hit areas before injecting taps so the first one lands */
        for (int click = 0; click < clicks; click++)
        {
            embedded_renderer_touch(0, ER_TOUCH_DOWN, panel_click_x, panel_click_y);
            er_commit();
            embedded_renderer_tick(FRAME_MS);
            embedded_renderer_touch(0, ER_TOUCH_UP, panel_click_x, panel_click_y);
            er_commit();
            embedded_renderer_tick(FRAME_MS);
        }
        if (clicks > 0)
        {
            SDL_Log("injected %d tap(s) at %d,%d", clicks, click_x, click_y);
            /* Let any tap-triggered animation settle (e.g. a Switch thumb's 200 ms slide) before the shot. */
            for (int elapsed = 0; elapsed < TAP_SETTLE_MS; elapsed += FRAME_MS)
            {
                er_commit();
                embedded_renderer_tick(FRAME_MS);
                erb_tick(FRAME_MS);
            }
        }

        /* ER_AOT_TAPS="x,y x,y …": tap each point in order (down+up), committing + settling between — for
           multi-target flows like focusing a TextInput then tapping on-screen keyboard keys. */
        {
            char taps[PATH_BYTES] = {0};
            const char* value = SDL_getenv("ER_AOT_TAPS");
            if (value)
            {
                SDL_strlcpy(taps, value, sizeof(taps));
            }
            if (taps[0])
            {
                er_commit(); /* layout + hit areas before the first tap */
                char* cursor = taps;
                while (*cursor)
                {
                    while (*cursor == ' ')
                    {
                        cursor++;
                    }
                    if (!*cursor)
                    {
                        break;
                    }
                    int tap_x = SDL_atoi(cursor);
                    char* comma = SDL_strchr(cursor, ',');
                    int tap_y = comma ? SDL_atoi(comma + 1) : 0;
                    er_sdl_window_point_to_panel(&tap_x, &tap_y);
                    embedded_renderer_touch(0, ER_TOUCH_DOWN, tap_x, tap_y);
                    er_commit();
                    embedded_renderer_tick(FRAME_MS);
                    embedded_renderer_touch(0, ER_TOUCH_UP, tap_x, tap_y);
                    er_commit();
                    embedded_renderer_tick(FRAME_MS);
                    while (*cursor && *cursor != ' ')
                    {
                        cursor++;
                    }
                }
                SDL_Log("tapped sequence \"%s\"", taps);
            }
        }

        /* ER_AOT_TYPE=<text>: type each character into the focused TextInput (the click above focuses it),
           committing between keys so onChangeText → state → update runs. ASCII only (one byte/char). */
        {
            const char* typed_text = SDL_getenv("ER_AOT_TYPE");
            if (typed_text && typed_text[0])
            {
                for (const char* character = typed_text; *character; character++)
                {
                    char key_text[2] = {*character, '\0'};
                    embedded_renderer_key(0, key_text);
                    er_commit();
                    embedded_renderer_tick(FRAME_MS);
                    erb_tick(FRAME_MS);
                }
                SDL_Log("typed \"%s\"", typed_text);
            }
        }

        /* ER_AOT_HOLD=<ms>: press at the click point and HOLD it, ticking the engine so a press-driven
           animation can settle, then screenshot while still held (no release). Lets a headless run
           capture an in-progress animation — e.g. a button scaled down by an onPressIn spring. */
        int hold_ms = 0;
        {
            const char* value = SDL_getenv("ER_AOT_HOLD");
            if (value)
            {
                hold_ms = SDL_atoi(value);
            }
        }
        if (hold_ms > 0)
        {
            embedded_renderer_touch(0, ER_TOUCH_DOWN, panel_click_x, panel_click_y);
            for (int elapsed = 0; elapsed < hold_ms; elapsed += FRAME_MS)
            {
                er_commit();
                embedded_renderer_tick(FRAME_MS);
                erb_tick(FRAME_MS); /* let app timers (setInterval/setTimeout) fire during the hold */
            }
            SDL_Log("held press at %d,%d for %d ms", click_x, click_y, hold_ms);
        }

        er_commit();
        er_sdl_present();
        SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(
            0, physical_width, physical_height, SCREENSHOT_BITS_PER_PIXEL, SDL_PIXELFORMAT_ARGB8888);
        if (surface
            && SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, surface->pixels, surface->pitch) == 0)
        {
            SDL_SaveBMP(surface, shot);
            SDL_Log("wrote screenshot %s", shot);
        }
        else
        {
            SDL_Log("screenshot failed: %s", SDL_GetError());
        }
        if (surface)
        {
            SDL_FreeSurface(surface);
        }
        er_sdl_backend_destroy();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }

    bool running = true;
    const uint32_t start_ticks = SDL_GetTicks();
    uint32_t previous_ticks = start_ticks;
    while (running)
    {
        bool turn_requested = false;
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_QUIT || (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE))
            {
                running = false;
            }
            else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_t && !event.key.repeat)
            {
                turn_requested = true;
            }
            else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT)
            {
                mouse_touch(ER_TOUCH_DOWN, event.button.x, event.button.y, scale);
            }
            else if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT)
            {
                mouse_touch(ER_TOUCH_UP, event.button.x, event.button.y, scale);
            }
            else if (event.type == SDL_MOUSEMOTION && (event.motion.state & SDL_BUTTON_LMASK))
            {
                mouse_touch(ER_TOUCH_MOVE, event.motion.x, event.motion.y, scale);
            }
        }

        /* Live rotation (T key): the running layout takes the new size with its state kept. A size that picks
           another layout, or a package built before it could resize, is loaded again. */
        if (turn_requested && turn_panel(window, &rotation, &physical_width, &physical_height))
        {
            SDL_Log("panel turned to %d", (int)rotation);
            ErbError turn_error;
            if (!erb_set_screen_size(0, 0) && !erb_load(package, package_bytes, &config, &turn_error))
            {
                char message[MESSAGE_BYTES];
                erb_error_message(&turn_error, message, sizeof(message));
                SDL_Log("the package didn't load: %s", message);
                erb_show_error(&turn_error);
            }
        }
        turn_requested = false;

        er_commit();
        er_sdl_present();

        const uint32_t now = SDL_GetTicks();
        const uint32_t delta_ms = now - previous_ticks;
        embedded_renderer_tick(delta_ms);
        erb_tick(delta_ms); /* advance app timers (setInterval/setTimeout); no-op if the app has none */
        desktop_natives_frame(now - start_ticks, erb_native_post);
        previous_ticks = now;
    }

    er_sdl_backend_destroy();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
