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

#include "web_backend.h"

#include "er_assets.h"
#include "er_runtime.h"
#include "er_scene.h"
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

/** @brief Bytes per pixel in the RGBA present buffer. */
#define RGBA_BYTES_PER_PIXEL 4U

/** @brief An alpha of this value is fully opaque. */
#define ALPHA_OPAQUE 0xFFU

/** @brief Opaque black, the color a cleared framebuffer holds. */
#define OPAQUE_BLACK 0xFF000000U

/** @brief The touch phases er_web_touch() takes from the page. */
#define WEB_TOUCH_DOWN 0
#define WEB_TOUCH_MOVE 1
#define WEB_TOUCH_UP 2

/** @brief Colors of the placeholder scene shown before an app is loaded. */
#define DEMO_BACKGROUND_COLOR 0xFF101418U
#define DEMO_CARD_COLOR 0xFF1E2A44U
#define DEMO_CARD_BORDER_COLOR 0xFF4F86F7U
#define DEMO_TITLE_COLOR 0xFFFFFFFFU
#define DEMO_SUBTITLE_COLOR 0xFF9FB4D6U

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static uint8_t* s_rgba;     /**< Canvas-facing RGBA buffer (R,G,B,A), screen_width * screen_height * 4 bytes. */
static int s_width;         /**< Framebuffer width in pixels. */
static int s_height;        /**< Framebuffer height in pixels. */
static char* s_source;      /**< Private copy of the loaded app bundle (for hot reload + resize); NUL-terminated. */
static int s_source_length; /**< Byte length of s_source. */
static bool s_app_loaded;   /**< An app bundle has been evaluated at least once. */
static uint8_t* s_pack;     /**< Private copy of the latest asset pack (engine references its pixels by pointer). */

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Builds the er_runtime configuration for a given board size.
 *
 * @param[in] width  Screen width exposed to JS as screen.width.
 * @param[in] height  Screen height exposed to JS as screen.height.
 *
 * @return A populated ErRuntimeConfig (by value).
 */
static ErRuntimeConfig runtime_cfg(int width, int height)
{
    ErRuntimeConfig config;
    memset(&config, 0, sizeof config);
    config.screen_width = width;
    config.screen_height = height;
    config.screen_scale = 1.0f;
    config.install_persist = true; /* usePersistentState survives a hot reload (dev loop); plain useState on a device */
    config.log = NULL;             /* console.* -> stdout, which Emscripten routes to the browser console */
    config.malloc_functions = NULL;
    config.max_stack_size = 0; /* QuickJS default guard; the wasm stack (-sSTACK_SIZE) is sized well above it */
    return config;
}

/** @brief Runs the stored app bundle (redbox on JS error), or the static fallback scene if none is loaded. */
static void run_app_or_demo(void)
{
    if (s_source)
    {
        if (!er_runtime_load_source(s_source, (size_t)s_source_length, "<app>"))
            er_runtime_show_error();
        s_app_loaded = true;
    }
    else
    {
        er_web_demo_scene();
    }
}

/**
 * @brief Converts the software ARGB8888 framebuffer into the RGBA present buffer.
 *
 * Canvas ImageData expects bytes in R,G,B,A order; the engine framebuffer is 0xAARRGGBB words. The frame is
 * opaque, so alpha is forced to 0xFF.
 */
static void present(void)
{
    const uint32_t* framebuffer = er_software_framebuffer();
    if (!framebuffer || !s_rgba)
        return;

    const size_t pixel_count = (size_t)s_width * (size_t)s_height;
    for (size_t index = 0; index < pixel_count; index++)
    {
        const uint32_t pixel = framebuffer[index];
        uint8_t* output = s_rgba + index * RGBA_BYTES_PER_PIXEL;
        output[0] = (uint8_t)(pixel >> 16); /* R */
        output[1] = (uint8_t)(pixel >> 8);  /* G */
        output[2] = (uint8_t)(pixel);       /* B */
        output[3] = ALPHA_OPAQUE;           /* A (opaque frame) */
    }
}

/**
 * @brief Sets every int16_t layout field of @p p to ER_LAYOUT_AUTO and the rest to defaults.
 *
 * Thin wrapper over the engine's er_props_default() so the demo-scene helpers read cleanly.
 *
 * @param[out] props  Props bag to initialise.
 */
static void props_init(ERProps* props)
{
    er_props_default(props);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public (exported ABI)
 ---------------------------------------------------------------------------------------------------------------------*/

EMSCRIPTEN_KEEPALIVE
int er_web_init(int screen_width, int screen_height)
{
    if (screen_width <= 0 || screen_height <= 0)
        return 0;
    if (!er_software_backend_init(screen_width, screen_height))
        return 0;

    s_rgba = malloc((size_t)screen_width * (size_t)screen_height * RGBA_BYTES_PER_PIXEL);
    if (!s_rgba)
    {
        er_software_backend_destroy();
        return 0;
    }
    s_width = screen_width;
    s_height = screen_height;

    /* Boot the portable QuickJS host core (bridge + console + screen + persist). The render backend must
       already be set, which it is. */
    ErRuntimeConfig config = runtime_cfg(screen_width, screen_height);
    if (!er_runtime_init(&config))
    {
        free(s_rgba);
        s_rgba = NULL;
        er_software_backend_destroy();
        return 0;
    }
    return 1;
}

EMSCRIPTEN_KEEPALIVE
void er_web_load_source(const char* source, int length)
{
    if (!source || length <= 0)
        return;

    /* Keep a private copy so a hot reload (er_web_reset) or a resize can re-run the same app. */
    char* copy = malloc((size_t)length + 1U);
    if (!copy)
        return;
    memcpy(copy, source, (size_t)length);
    copy[length] = '\0';
    free(s_source);
    s_source = copy;
    s_source_length = length;

    /* Replacing a running app: reset to a fresh context first (full remount). */
    if (s_app_loaded)
        er_runtime_reset();
    if (!er_runtime_load_source(s_source, (size_t)length, "<app>"))
        er_runtime_show_error();
    s_app_loaded = true;
}

EMSCRIPTEN_KEEPALIVE
const uint8_t* er_web_compile_bytecode(const char* source, int length, int* out_length, int strip)
{
    /* Build-time `qjsc` for the consumer CLI: compile a JS bundle to QuickJS bytecode using the QuickJS
       already inside this module — no native toolchain. The host reads *out_length bytes from the returned
       pointer and frees it with Module._free. Independent of any running app (throwaway runtime).
       strip != 0 drops the embedded source text + debug tables (release/device: ~8x smaller blob);
       0 keeps them so dev stack traces have line numbers. */
    size_t bytecode_length = 0;
    uint8_t* bytecode = (source && length > 0)
                            ? er_runtime_compile_bytecode_ex(source, (size_t)length, &bytecode_length, strip != 0)
                            : NULL;
    if (out_length)
        *out_length = (int)bytecode_length;
    return bytecode;
}

EMSCRIPTEN_KEEPALIVE
int er_web_load_pack(const uint8_t* buffer, int length)
{
    if (!buffer || length <= 0)
        return 0;

    /* The engine references the pack's pixels + glyph bitmaps by pointer, so keep a private copy alive. */
    uint8_t* copy = malloc((size_t)length);
    if (!copy)
        return 0;
    memcpy(copy, buffer, (size_t)length);
    if (!er_assets_load_pack(copy, (size_t)length))
    {
        free(copy);
        return 0;
    }
    /* The new pack re-registers asset names; after the next reset/reload the old pixels are unreferenced. */
    free(s_pack);
    s_pack = copy;
    return 1;
}

/** @brief Clamps @p v to [lo, hi]. */
static int clampi(int value, int low, int high)
{
    return value < low ? low : (value > high ? high : value);
}

EMSCRIPTEN_KEEPALIVE
void er_web_demo_scene(void)
{
    ERProps props;

    /* Card + type sized from the board so the placeholder looks right at any preset (800x480 down to a
       240x320 phone-ish panel) and during a runtime resize. Font scales with the content width so even the
       longest label fits without clipping. */
    const int padding = clampi(s_width / 30, 8, 20);
    const int card_width = clampi(s_width - 60, 160, 460);
    const int card_height = clampi(s_height - 80, 110, 240);
    const int content_width = card_width - 2 * padding;
    const int title_size = clampi(content_width / 16, 11, 26);
    const int subtitle_size = clampi(title_size * 3 / 4, 9, 20);

    /* Root: full-screen dark background, centering its single child. */
    ERNode* root = er_node_create(ER_NODE_VIEW);
    props_init(&props);
    props.width = (int16_t)s_width;
    props.height = (int16_t)s_height;
    props.background_color = DEMO_BACKGROUND_COLOR;
    props.justify_content = ER_JUSTIFY_CENTER;
    props.align_items = ER_ALIGN_CENTER;
    er_node_set_props(root, &props);
    er_tree_set_root(root);

    /* Card: rounded, bordered panel that stacks its labels centered. */
    ERNode* card = er_node_create(ER_NODE_VIEW);
    props_init(&props);
    props.width = (int16_t)card_width;
    props.height = (int16_t)card_height;
    props.padding = (int16_t)padding;
    props.background_color = DEMO_CARD_COLOR;
    props.border_color = DEMO_CARD_BORDER_COLOR;
    props.border_width = 2;
    props.border_radius = (int16_t)clampi(card_width / 14, 10, 24);
    props.align_items = ER_ALIGN_CENTER;
    props.justify_content = ER_JUSTIFY_CENTER;
    er_node_set_props(card, &props);
    er_tree_append_child(root, card);

    /* Title. */
    ERNode* title = er_node_create(ER_NODE_TEXT);
    props_init(&props);
    snprintf(props.text, sizeof props.text, "%s", "embedded-react");
    props.color = DEMO_TITLE_COLOR;
    props.font_size = (uint8_t)title_size;
    props.font_weight = 0; /* normal: the built-in font measures bold with normal metrics, clipping the last glyph */
    props.text_align = ER_TEXT_ALIGN_CENTER;
    er_node_set_props(title, &props);
    er_tree_append_child(card, title);

    /* Subtitle. */
    ERNode* subtitle = er_node_create(ER_NODE_TEXT);
    props_init(&props);
    snprintf(props.text,
             sizeof props.text,
             "%dx%d  -  WASM simulator",
             s_width,
             s_height); /* ASCII only: built-in font is Latin */
    props.color = DEMO_SUBTITLE_COLOR;
    props.font_size = (uint8_t)subtitle_size;
    props.margin_top = (int16_t)clampi(title_size / 2, 6, 14);
    props.text_align = ER_TEXT_ALIGN_CENTER;
    er_node_set_props(subtitle, &props);
    er_tree_append_child(card, subtitle);
}

EMSCRIPTEN_KEEPALIVE
int er_web_resize(int screen_width, int screen_height)
{
    if (screen_width <= 0 || screen_height <= 0)
        return 0;
    if (!er_software_backend_resize(screen_width, screen_height))
        return 0;

    uint8_t* resized = realloc(s_rgba, (size_t)screen_width * (size_t)screen_height * RGBA_BYTES_PER_PIXEL);
    if (!resized)
        return 0; /* old buffer kept; software framebuffer already resized but stays consistent on next resize */
    s_rgba = resized;
    s_width = screen_width;
    s_height = screen_height;

    /* Re-init the runtime at the new screen size so the app re-runs responsively (new screen.width/height) —
       the on-device responsive-layout path. The engine pools are reset so nodes don't leak across resizes. */
    er_runtime_shutdown();
    er_reset();
    ErRuntimeConfig config = runtime_cfg(screen_width, screen_height);
    er_runtime_init(&config);
    run_app_or_demo();
    return 1;
}

EMSCRIPTEN_KEEPALIVE
void er_web_pump(int delta_ms)
{
    er_runtime_pump(); /* drain Promises/microtasks + fire due timers before painting */
    er_commit();
    embedded_renderer_tick(delta_ms > 0 ? (uint32_t)delta_ms : 0U);
    present();
}

EMSCRIPTEN_KEEPALIVE
void er_web_touch(int phase, int x, int y)
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
void er_web_clear_persist(void)
{
    er_runtime_clear_persist();
}

EMSCRIPTEN_KEEPALIVE
void er_web_reset(void)
{
    /* Hot reload: drop the JS context + reset the engine, then re-run the stored bundle (full remount). */
    if (s_app_loaded)
    {
        er_runtime_reset();
        if (s_source && !er_runtime_load_source(s_source, (size_t)s_source_length, "<app>"))
            er_runtime_show_error();
    }
    else
    {
        er_software_clear(OPAQUE_BLACK);
        present();
    }
}

EMSCRIPTEN_KEEPALIVE
const uint8_t* er_web_framebuffer(void)
{
    return s_rgba;
}

EMSCRIPTEN_KEEPALIVE
int er_web_framebuffer_width(void)
{
    return s_width;
}

EMSCRIPTEN_KEEPALIVE
int er_web_framebuffer_height(void)
{
    return s_height;
}
