/*
 * embedded-react-desktop-aot — the desktop host for Flow B (the AOT path).
 *
 * The Flow B peer of examples/linux: it renders a JSX app in an SDL2 window with NO QuickJS and NO JS
 * at runtime. The app's scene graph is compiled straight to C (dist/app.gen.c, from `npm run aot`),
 * which calls er_scene.h directly; this host just brings up SDL + the engine's SDL backend, calls
 * er_app_build() once, and runs the frame loop (input → touch, commit, present, tick). Same engine,
 * same backend as Flow A — only the front end (compiled C vs. interpreted React) differs.
 *
 * This is the desktop proxy for a no-PSRAM MCU: no JS heap, no interpreter, a fixed static footprint.
 */

#define SDL_MAIN_HANDLED

#include "app.gen.h"
#include "er_scene.h"
#include "native_renderer.h"
#include "sdl_backend.h"

#include <SDL2/SDL.h>

#define SCREEN_W 800
#define SCREEN_H 600

/** @brief Scales a logical SDL coordinate to physical framebuffer pixels. */
static int to_px(int logical, float scale)
{
    return (int)((float)logical * scale + 0.5f);
}

/**
 * @brief Entry point: bring up SDL + the engine, build the AOT app once, run the frame loop.
 *
 * @return 0 on clean exit; non-zero on a setup failure.
 */
int main(void)
{
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("embedded-react — desktop (Flow B / AOT)",
                                          SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED,
                                          SCREEN_W,
                                          SCREEN_H,
                                          SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
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

    /* Build the compiled app's scene graph (no JS — just er_scene.h calls baked at build time). */
    er_app_build(phys_w, phys_h);
    SDL_Log("AOT app built at %dx%d (no QuickJS)", phys_w, phys_h);

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
        embedded_renderer_tick(now - prev);
        prev = now;
    }

    er_sdl_backend_destroy();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
