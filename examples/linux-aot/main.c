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
 */

#define SDL_MAIN_HANDLED

#include "er_scene.h"
#include "erb_runtime.h"
#include "native_renderer.h"
#include "sdl_backend.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Window size, unless ER_AOT_SCREEN_W/H say otherwise. The window's size picks the package's layout. */
#define SCREEN_W_DEFAULT 800
#define SCREEN_H_DEFAULT 480

/** @brief The firmware's arena. */
static uint32_t s_arena[(64u * 1024u) / sizeof(uint32_t)];

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
    uint32_t* words = length > 0 ? malloc(((size_t)length + 3u) & ~(size_t)3u) : NULL;
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
    const char* v = SDL_getenv(name);
    if (!v || !v[0])
    {
        return fallback;
    }
    const int n = SDL_atoi(v);
    return n > 0 ? n : fallback;
}

/** @brief Reads a boolean env var: true when set to anything but empty or "0". */
static bool env_flag(const char* name)
{
    const char* v = SDL_getenv(name);
    return v && v[0] && SDL_strcmp(v, "0") != 0;
}

/** @brief Scales a logical SDL coordinate to physical framebuffer pixels. */
static int to_px(int logical, float scale)
{
    return (int)((float)logical * scale + 0.5f);
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

    /* Framebuffer size. The parity harness sets it through ER_AOT_SCREEN_W/H, the same vars that name the
       panel a package keeps its layout for in the AOT compiler. */
    const int SCREEN_W = env_int("ER_AOT_SCREEN_W", SCREEN_W_DEFAULT);
    const int SCREEN_H = env_int("ER_AOT_SCREEN_H", SCREEN_H_DEFAULT);

    Uint32 window_flags = SDL_WINDOW_SHOWN;
    if (env_flag("ER_HIDPI") && !env_flag("ER_NO_HIDPI"))
    {
        window_flags |= SDL_WINDOW_ALLOW_HIGHDPI;
    }

    SDL_Window* window = SDL_CreateWindow("embedded-react — desktop (Flow B / AOT)",
                                          SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED,
                                          SCREEN_W,
                                          SCREEN_H,
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

    int phys_w = SCREEN_W;
    int phys_h = SCREEN_H;
    SDL_GetRendererOutputSize(renderer, &phys_w, &phys_h);
    const float scale = (float)phys_w / (float)SCREEN_W;

    if (!er_sdl_backend_init(renderer, phys_w, phys_h))
    {
        SDL_Log("er_sdl_backend_init failed");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* Anchor Date.now() to the real time before the load, so a mount effect already reads it. A board with
       no RTC would call this once it learns the time (SNTP, GPS). */
    erb_set_wall_clock((int64_t)time(NULL) * 1000);

    /* Verify the package and build its scene graph at this window's size. One that fails shows the error
       screen, as a board does, and a headless run fails. */
    const ErbConfig config = {
        .arena = s_arena,
        .arena_bytes = sizeof(s_arena),
        .screen_w = (uint16_t)phys_w,
        .screen_h = (uint16_t)phys_h,
        .log = log_line,
    };
    ErbError load_error;
    const bool loaded = erb_load(package, package_bytes, &config, &load_error);
    if (loaded)
    {
        SDL_Log("package %s running at %dx%d (no QuickJS)", package_path, phys_w, phys_h);
    }
    else
    {
        char message[256];
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
    char shot[512] = {0};
    {
        const char* s = SDL_getenv("ER_AOT_SHOT");
        if (s)
        {
            SDL_strlcpy(shot, s, sizeof(shot));
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
        int px = phys_w / 2;
        int py = (int)(phys_h * 0.57f);
        {
            const char* s = SDL_getenv("ER_AOT_CLICKS");
            if (s)
            {
                clicks = SDL_atoi(s);
            }
        }
        {
            const char* s = SDL_getenv("ER_AOT_CLICK_X");
            if (s)
            {
                px = SDL_atoi(s);
            }
        }
        {
            const char* s = SDL_getenv("ER_AOT_CLICK_Y");
            if (s)
            {
                py = SDL_atoi(s);
            }
        }
        er_commit(); /* compute layout + hit areas before injecting taps so the first one lands */
        for (int i = 0; i < clicks; i++)
        {
            embedded_renderer_touch(0, ER_TOUCH_DOWN, px, py);
            er_commit();
            embedded_renderer_tick(16);
            embedded_renderer_touch(0, ER_TOUCH_UP, px, py);
            er_commit();
            embedded_renderer_tick(16);
        }
        if (clicks > 0)
        {
            SDL_Log("injected %d tap(s) at %d,%d", clicks, px, py);
            /* Let any tap-triggered animation settle (e.g. a Switch thumb's 200 ms slide) before the shot. */
            for (int elapsed = 0; elapsed < 300; elapsed += 16)
            {
                er_commit();
                embedded_renderer_tick(16);
                erb_tick(16);
            }
        }

        /* ER_AOT_TAPS="x,y x,y …": tap each point in order (down+up), committing + settling between — for
           multi-target flows like focusing a TextInput then tapping on-screen keyboard keys. */
        {
            char taps[512] = {0};
            const char* s = SDL_getenv("ER_AOT_TAPS");
            if (s)
            {
                SDL_strlcpy(taps, s, sizeof(taps));
            }
            if (taps[0])
            {
                er_commit(); /* layout + hit areas before the first tap */
                char* p = taps;
                while (*p)
                {
                    while (*p == ' ')
                    {
                        p++;
                    }
                    if (!*p)
                    {
                        break;
                    }
                    const int tx = SDL_atoi(p);
                    char* comma = SDL_strchr(p, ',');
                    const int ty = comma ? SDL_atoi(comma + 1) : 0;
                    embedded_renderer_touch(0, ER_TOUCH_DOWN, tx, ty);
                    er_commit();
                    embedded_renderer_tick(16);
                    embedded_renderer_touch(0, ER_TOUCH_UP, tx, ty);
                    er_commit();
                    embedded_renderer_tick(16);
                    while (*p && *p != ' ')
                    {
                        p++;
                    }
                }
                SDL_Log("tapped sequence \"%s\"", taps);
            }
        }

        /* ER_AOT_TYPE=<text>: type each character into the focused TextInput (the click above focuses it),
           committing between keys so onChangeText → state → update runs. ASCII only (one byte/char). */
        {
            const char* type_str = SDL_getenv("ER_AOT_TYPE");
            if (type_str && type_str[0])
            {
                for (const char* tp = type_str; *tp; tp++)
                {
                    char ch[2] = {*tp, '\0'};
                    embedded_renderer_key(0, ch);
                    er_commit();
                    embedded_renderer_tick(16);
                    erb_tick(16);
                }
                SDL_Log("typed \"%s\"", type_str);
            }
        }

        /* ER_AOT_HOLD=<ms>: press at the click point and HOLD it, ticking the engine so a press-driven
           animation can settle, then screenshot while still held (no release). Lets a headless run
           capture an in-progress animation — e.g. a button scaled down by an onPressIn spring. */
        int hold_ms = 0;
        {
            const char* s = SDL_getenv("ER_AOT_HOLD");
            if (s)
            {
                hold_ms = SDL_atoi(s);
            }
        }
        if (hold_ms > 0)
        {
            embedded_renderer_touch(0, ER_TOUCH_DOWN, px, py);
            for (int elapsed = 0; elapsed < hold_ms; elapsed += 16)
            {
                er_commit();
                embedded_renderer_tick(16);
                erb_tick(16); /* let app timers (setInterval/setTimeout) fire during the hold */
            }
            SDL_Log("held press at %d,%d for %d ms", px, py, hold_ms);
        }

        er_commit();
        er_sdl_present();
        SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, phys_w, phys_h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (surf && SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch) == 0)
        {
            SDL_SaveBMP(surf, shot);
            SDL_Log("wrote screenshot %s", shot);
        }
        else
        {
            SDL_Log("screenshot failed: %s", SDL_GetError());
        }
        if (surf)
        {
            SDL_FreeSurface(surf);
        }
        er_sdl_backend_destroy();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }

    bool running = true;
    uint32_t prev = SDL_GetTicks();
    while (running)
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE))
            {
                running = false;
            }
            else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT)
            {
                embedded_renderer_touch(0, ER_TOUCH_DOWN, to_px(ev.button.x, scale), to_px(ev.button.y, scale));
            }
            else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT)
            {
                embedded_renderer_touch(0, ER_TOUCH_UP, to_px(ev.button.x, scale), to_px(ev.button.y, scale));
            }
            else if (ev.type == SDL_MOUSEMOTION && (ev.motion.state & SDL_BUTTON_LMASK))
            {
                embedded_renderer_touch(0, ER_TOUCH_MOVE, to_px(ev.motion.x, scale), to_px(ev.motion.y, scale));
            }
        }

        er_commit();
        er_sdl_present();

        const uint32_t now = SDL_GetTicks();
        const uint32_t dt = now - prev;
        embedded_renderer_tick(dt);
        erb_tick(dt); /* advance app timers (setInterval/setTimeout); no-op if the app has none */
        prev = now;
    }

    er_sdl_backend_destroy();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
