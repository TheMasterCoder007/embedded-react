/*
 * embedded-react-desktop-js — Flow A desktop host.
 *
 * Boots a QuickJS runtime, installs the NativeUI bridge, and runs a JS program that
 * builds the UI by calling into the engine's er_scene.h API. The SDL2 backend paints
 * the result to a window. This is the §0 "JS renders on screen" milestone from BRIDGE.md:
 * the same NativeUI surface the React reconciler will eventually drive, exercised here by
 * a hand-written JS app instead of bundled JSX.
 *
 * Usage:
 *   embedded-react-desktop-js [app.js]
 * With no argument it runs a built-in demo app; pass a path to iterate on UI without
 * recompiling.
 */

/* Must be defined before any SDL header to disable SDL's main() macro on Windows. */
#define SDL_MAIN_HANDLED

#include "er_scene.h"
#include "native_renderer.h"
#include "native_ui_bridge.h"
#include "quickjs.h"
#include "sdl_backend.h"

#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define SCREEN_W 480
#define SCREEN_H 320

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Physical-pixel / logical-pixel ratio, cached for HiDPI input scaling. */
static float s_dpi_scale = 1.0f;

/**
 * @brief Built-in demo app, used when no script path is given on the command line.
 *
 * Exercises the marshalling layer (flex layout, colors, border radius, text) entirely
 * from JS through the NativeUI bridge. `screen` is injected by the host before eval.
 */
static const char* k_default_app =
    "const W = screen.width, H = screen.height;\n"
    "const root = NativeUI.createNode('View');\n"
    "NativeUI.setProps(root, { width: W, height: H, backgroundColor: '#111927',\n"
    "                          flexDirection: 'column', padding: 16, gap: 12 });\n"
    "const title = NativeUI.createNode('Text');\n"
    "NativeUI.setProps(title, { text: 'Hello from QuickJS', color: 'white', fontSize: 24 });\n"
    "NativeUI.appendChild(root, title);\n"
    "const subtitle = NativeUI.createNode('Text');\n"
    "NativeUI.setProps(subtitle, { text: 'Tap a card', color: '#9aa5b1', fontSize: 16 });\n"
    "NativeUI.appendChild(root, subtitle);\n"
    "const row = NativeUI.createNode('View');\n"
    "NativeUI.setProps(row, { flexDirection: 'row', gap: 12, marginTop: 8 });\n"
    "NativeUI.appendChild(root, row);\n"
    "const cards = [ { label: 'CPU', color: '#2a9d8f' },\n"
    "                { label: 'MEM', color: '#e94560' },\n"
    "                { label: 'NET', color: '#f4a261' } ];\n"
    "for (let i = 0; i < cards.length; i++) {\n"
    "    const card = NativeUI.createNode('Pressable');\n"
    "    NativeUI.setProps(card, { flex: 1, height: 120, backgroundColor: cards[i].color,\n"
    "                              borderRadius: 10, padding: 12, justifyContent: 'flex-end' });\n"
    "    NativeUI.setEvent(card, 'onPress', () => {\n"
    "        NativeUI.setProps(subtitle, { text: 'Pressed ' + cards[i].label,\n"
    "                                      color: '#ffd166', fontSize: 16 });\n"
    "        console.log('pressed', cards[i].label);\n"
    "    });\n"
    "    const lbl = NativeUI.createNode('Text');\n"
    "    NativeUI.setProps(lbl, { text: cards[i].label, color: 'white', fontSize: 20, fontWeight: 'bold' });\n"
    "    NativeUI.appendChild(card, lbl);\n"
    "    NativeUI.appendChild(row, card);\n"
    "}\n"
    "NativeUI.setRoot(root);\n"
    "NativeUI.commit();\n"
    "console.log('App built UI at', W + 'x' + H, '- tap a card');\n";

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — host helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Scales a logical input coordinate to physical framebuffer pixels.
 *
 * @param[in] logical  Logical-pixel coordinate from an SDL mouse event.
 *
 * @return Physical-pixel coordinate.
 */
static int event_px(int logical)
{
    return (int)((float)logical * s_dpi_scale + 0.5f);
}

/**
 * @brief Reads an entire file into a newly allocated, null-terminated buffer.
 *
 * @param[in] path  Filesystem path to read.
 *
 * @return Heap buffer the caller must free(), or NULL if the file could not be read.
 */
static char* read_file(const char* path)
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
    return buf;
}

/**
 * @brief Minimal console.log implementation printing space-separated arguments to stdout.
 *
 * @param[in] ctx       QuickJS context.
 * @param[in] this_val  JS `this` (unused).
 * @param[in] argc      Argument count.
 * @param[in] argv      Argument values.
 *
 * @return JS_UNDEFINED, or JS_EXCEPTION if an argument cannot be stringified.
 */
static JSValue host_console_log(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    for (int i = 0; i < argc; i++)
    {
        const char* str = JS_ToCString(ctx, argv[i]);
        if (!str)
        {
            return JS_EXCEPTION;
        }
        printf("%s%s", i ? " " : "", str);
        JS_FreeCString(ctx, str);
    }
    printf("\n");
    return JS_UNDEFINED;
}

/**
 * @brief Installs a minimal `console` (log/warn/error all map to stdout) onto the global scope.
 *
 * @param[in] ctx  QuickJS context.
 */
static void host_install_console(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    JSValue log = JS_NewCFunction(ctx, host_console_log, "log", 1);

    JS_SetPropertyStr(ctx, console, "log", JS_DupValue(ctx, log));
    JS_SetPropertyStr(ctx, console, "warn", JS_DupValue(ctx, log));
    JS_SetPropertyStr(ctx, console, "error", log);
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
}

/**
 * @brief Injects a `screen` global ({ width, height, scale }) so the app can size its root.
 *
 * @param[in] ctx     QuickJS context.
 * @param[in] width   Framebuffer width in physical pixels.
 * @param[in] height  Framebuffer height in physical pixels.
 * @param[in] scale   DPI scale factor.
 */
static void host_install_screen(JSContext* ctx, int width, int height, float scale)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue screen = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, screen, "width", JS_NewInt32(ctx, width));
    JS_SetPropertyStr(ctx, screen, "height", JS_NewInt32(ctx, height));
    JS_SetPropertyStr(ctx, screen, "scale", JS_NewFloat64(ctx, (double)scale));
    JS_SetPropertyStr(ctx, global, "screen", screen);
    JS_FreeValue(ctx, global);
}

/**
 * @brief Evaluates a JS program, reporting any uncaught exception to stderr.
 *
 * @param[in] ctx   QuickJS context.
 * @param[in] src   Null-terminated JS source.
 * @param[in] name  Display name used in stack traces.
 *
 * @return true on clean evaluation; false if an exception propagated.
 */
static bool host_eval(JSContext* ctx, const char* src, const char* name)
{
    JSValue result = JS_Eval(ctx, src, strlen(src), name, JS_EVAL_TYPE_GLOBAL);
    bool ok = true;
    if (JS_IsException(result))
    {
        JSValue exc = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, exc);
        fprintf(stderr, "JS exception: %s\n", msg ? msg : "(unknown)");
        if (msg)
        {
            JS_FreeCString(ctx, msg);
        }
        JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
        if (!JS_IsUndefined(stack))
        {
            const char* st = JS_ToCString(ctx, stack);
            if (st)
            {
                fprintf(stderr, "%s\n", st);
                JS_FreeCString(ctx, st);
            }
        }
        JS_FreeValue(ctx, stack);
        JS_FreeValue(ctx, exc);
        ok = false;
    }
    JS_FreeValue(ctx, result);
    return ok;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Entry point: sets up SDL + the engine, runs the JS app, then drives the frame loop.
 *
 * @param[in] argc  Argument count.
 * @param[in] argv  Optional argv[1] = path to a JS app file (falls back to the built-in demo).
 *
 * @return 0 on clean exit; non-zero on setup or JS evaluation failure.
 */
int main(int argc, char** argv)
{
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("embedded-react  Flow A (QuickJS)",
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
    s_dpi_scale = (float)phys_w / (float)SCREEN_W;

    if (!er_sdl_backend_init(renderer, phys_w, phys_h))
    {
        SDL_Log("er_sdl_backend_init failed");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* Boot QuickJS and publish the bridge + host globals. */
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);
    host_install_console(ctx);
    host_install_screen(ctx, phys_w, phys_h, s_dpi_scale);
    er_bridge_install(ctx);

    /* Load the app: a file path if given, otherwise the built-in demo. */
    char* loaded = NULL;
    const char* app_src = k_default_app;
    const char* app_name = "<builtin-app>";
    if (argc > 1)
    {
        loaded = read_file(argv[1]);
        if (loaded)
        {
            app_src = loaded;
            app_name = argv[1];
        }
        else
        {
            SDL_Log("could not read '%s'; falling back to the built-in app", argv[1]);
        }
    }

    const bool app_ok = host_eval(ctx, app_src, app_name);
    free(loaded);

    if (!app_ok)
    {
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        er_sdl_backend_destroy();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* Frame loop. The app already built + committed its tree; the loop keeps painting and
       advancing animations and forwards touch input to the engine. */
    bool running = true;
    uint32_t prev = SDL_GetTicks();
    while (running)
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_QUIT)
            {
                running = false;
            }
            else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
            {
                running = false;
            }
            else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT)
            {
                embedded_renderer_touch(0, ER_TOUCH_DOWN, event_px(ev.button.x), event_px(ev.button.y));
            }
            else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT)
            {
                embedded_renderer_touch(0, ER_TOUCH_UP, event_px(ev.button.x), event_px(ev.button.y));
            }
            else if (ev.type == SDL_MOUSEMOTION && (ev.motion.state & SDL_BUTTON_LMASK))
            {
                embedded_renderer_touch(0, ER_TOUCH_MOVE, event_px(ev.motion.x), event_px(ev.motion.y));
            }
        }

        er_commit();
        er_sdl_present();

        const uint32_t now = SDL_GetTicks();
        embedded_renderer_tick(now - prev);
        prev = now;
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    er_sdl_backend_destroy();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
