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

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static uint8_t* s_rgba;   /**< Canvas-facing RGBA buffer (R,G,B,A), screen_w * screen_h * 4 bytes. */
static int s_w;           /**< Framebuffer width in pixels. */
static int s_h;           /**< Framebuffer height in pixels. */
static char* s_src;       /**< Private copy of the loaded app bundle (for hot reload + resize); NUL-terminated. */
static int s_src_len;     /**< Byte length of s_src. */
static bool s_app_loaded; /**< An app bundle has been evaluated at least once. */
static uint8_t* s_pack;   /**< Private copy of the latest asset pack (engine references its pixels by pointer). */

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Builds the er_runtime configuration for a given board size.
 *
 * @param[in] w  Screen width exposed to JS as screen.width.
 * @param[in] h  Screen height exposed to JS as screen.height.
 *
 * @return A populated ErRuntimeConfig (by value).
 */
static ErRuntimeConfig runtime_cfg(int w, int h)
{
    ErRuntimeConfig rt;
    memset(&rt, 0, sizeof rt);
    rt.screen_width = w;
    rt.screen_height = h;
    rt.screen_scale = 1.0f;
    rt.install_persist = true; /* usePersistentState survives a hot reload (dev loop); plain useState on a device */
    rt.log = NULL;             /* console.* -> stdout, which Emscripten routes to the browser console */
    rt.malloc_functions = NULL;
    rt.max_stack_size = 0; /* QuickJS default guard; the wasm stack (-sSTACK_SIZE) is sized well above it */
    return rt;
}

/** @brief Runs the stored app bundle (redbox on JS error), or the static fallback scene if none is loaded. */
static void run_app_or_demo(void)
{
    if (s_src)
    {
        if (!er_runtime_load_source(s_src, (size_t)s_src_len, "<app>"))
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
    const uint32_t* fb = er_software_framebuffer();
    if (!fb || !s_rgba)
        return;

    const size_t n = (size_t)s_w * (size_t)s_h;
    for (size_t i = 0; i < n; i++)
    {
        const uint32_t px = fb[i];
        uint8_t* o = s_rgba + i * 4U;
        o[0] = (uint8_t)(px >> 16); /* R */
        o[1] = (uint8_t)(px >> 8);  /* G */
        o[2] = (uint8_t)(px);       /* B */
        o[3] = 0xFFU;               /* A (opaque frame) */
    }
}

/**
 * @brief Sets every int16_t layout field of @p p to ER_LAYOUT_AUTO and the rest to defaults.
 *
 * Thin wrapper over the engine's er_props_default() so the demo-scene helpers read cleanly.
 *
 * @param[out] p  Props bag to initialise.
 */
static void props_init(ERProps* p)
{
    er_props_default(p);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public (exported ABI)
 ---------------------------------------------------------------------------------------------------------------------*/

EMSCRIPTEN_KEEPALIVE
int er_web_init(int screen_w, int screen_h)
{
    if (screen_w <= 0 || screen_h <= 0)
        return 0;
    if (!er_software_backend_init(screen_w, screen_h))
        return 0;

    s_rgba = malloc((size_t)screen_w * (size_t)screen_h * 4U);
    if (!s_rgba)
    {
        er_software_backend_destroy();
        return 0;
    }
    s_w = screen_w;
    s_h = screen_h;

    /* Boot the portable QuickJS host core (bridge + console + screen + persist). The render backend must
       already be set, which it is. */
    ErRuntimeConfig rt = runtime_cfg(screen_w, screen_h);
    if (!er_runtime_init(&rt))
    {
        free(s_rgba);
        s_rgba = NULL;
        er_software_backend_destroy();
        return 0;
    }
    return 1;
}

EMSCRIPTEN_KEEPALIVE
void er_web_load_source(const char* js, int len)
{
    if (!js || len <= 0)
        return;

    /* Keep a private copy so a hot reload (er_web_reset) or a resize can re-run the same app. */
    char* copy = malloc((size_t)len + 1U);
    if (!copy)
        return;
    memcpy(copy, js, (size_t)len);
    copy[len] = '\0';
    free(s_src);
    s_src = copy;
    s_src_len = len;

    /* Replacing a running app: reset to a fresh context first (full remount). */
    if (s_app_loaded)
        er_runtime_reset();
    if (!er_runtime_load_source(s_src, (size_t)len, "<app>"))
        er_runtime_show_error();
    s_app_loaded = true;
}

EMSCRIPTEN_KEEPALIVE
int er_web_load_pack(const uint8_t* buf, int len)
{
    if (!buf || len <= 0)
        return 0;

    /* The engine references the pack's pixels + glyph bitmaps by pointer, so keep a private copy alive. */
    uint8_t* copy = malloc((size_t)len);
    if (!copy)
        return 0;
    memcpy(copy, buf, (size_t)len);
    if (!er_assets_load_pack(copy, (size_t)len))
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
static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

EMSCRIPTEN_KEEPALIVE
void er_web_demo_scene(void)
{
    ERProps p;

    /* Card + type sized from the board so the placeholder looks right at any preset (800x480 down to a
       240x320 phone-ish panel) and during a runtime resize. Font scales with the content width so even the
       longest label fits without clipping. */
    const int pad = clampi(s_w / 30, 8, 20);
    const int cw = clampi(s_w - 60, 160, 460);
    const int ch = clampi(s_h - 80, 110, 240);
    const int content = cw - 2 * pad;
    const int title_size = clampi(content / 16, 11, 26);
    const int sub_size = clampi(title_size * 3 / 4, 9, 20);

    /* Root: full-screen dark background, centering its single child. */
    ERNode* root = er_node_create(ER_NODE_VIEW);
    props_init(&p);
    p.width = (int16_t)s_w;
    p.height = (int16_t)s_h;
    p.background_color = 0xFF101418U;
    p.justify_content = ER_JUSTIFY_CENTER;
    p.align_items = ER_ALIGN_CENTER;
    er_node_set_props(root, &p);
    er_tree_set_root(root);

    /* Card: rounded, bordered panel that stacks its labels centered. */
    ERNode* card = er_node_create(ER_NODE_VIEW);
    props_init(&p);
    p.width = (int16_t)cw;
    p.height = (int16_t)ch;
    p.padding = (int16_t)pad;
    p.background_color = 0xFF1E2A44U;
    p.border_color = 0xFF4F86F7U;
    p.border_width = 2;
    p.border_radius = (int16_t)clampi(cw / 14, 10, 24);
    p.align_items = ER_ALIGN_CENTER;
    p.justify_content = ER_JUSTIFY_CENTER;
    er_node_set_props(card, &p);
    er_tree_append_child(root, card);

    /* Title. */
    ERNode* title = er_node_create(ER_NODE_TEXT);
    props_init(&p);
    snprintf(p.text, sizeof p.text, "%s", "embedded-react");
    p.color = 0xFFFFFFFFU;
    p.font_size = (uint8_t)title_size;
    p.font_weight = 0; /* normal: the built-in font measures bold with normal metrics, clipping the last glyph */
    p.text_align = ER_TEXT_ALIGN_CENTER;
    er_node_set_props(title, &p);
    er_tree_append_child(card, title);

    /* Subtitle. */
    ERNode* sub = er_node_create(ER_NODE_TEXT);
    props_init(&p);
    snprintf(p.text, sizeof p.text, "%dx%d  -  WASM simulator", s_w, s_h); /* ASCII only: built-in font is Latin */
    p.color = 0xFF9FB4D6U;
    p.font_size = (uint8_t)sub_size;
    p.margin_top = (int16_t)clampi(title_size / 2, 6, 14);
    p.text_align = ER_TEXT_ALIGN_CENTER;
    er_node_set_props(sub, &p);
    er_tree_append_child(card, sub);
}

EMSCRIPTEN_KEEPALIVE
int er_web_resize(int screen_w, int screen_h)
{
    if (screen_w <= 0 || screen_h <= 0)
        return 0;
    if (!er_software_backend_resize(screen_w, screen_h))
        return 0;

    uint8_t* nr = realloc(s_rgba, (size_t)screen_w * (size_t)screen_h * 4U);
    if (!nr)
        return 0; /* old buffer kept; software fb already resized but stays consistent on next resize */
    s_rgba = nr;
    s_w = screen_w;
    s_h = screen_h;

    /* Re-init the runtime at the new screen size so the app re-runs responsively (new screen.width/height) —
       the on-device responsive-layout path. The engine pools are reset so nodes don't leak across resizes. */
    er_runtime_shutdown();
    er_reset();
    ErRuntimeConfig rt = runtime_cfg(screen_w, screen_h);
    er_runtime_init(&rt);
    run_app_or_demo();
    return 1;
}

EMSCRIPTEN_KEEPALIVE
void er_web_pump(int dt_ms)
{
    er_runtime_pump(); /* drain Promises/microtasks + fire due timers before painting */
    er_commit();
    embedded_renderer_tick(dt_ms > 0 ? (uint32_t)dt_ms : 0U);
    present();
}

EMSCRIPTEN_KEEPALIVE
void er_web_touch(int phase, int x, int y)
{
    ERTouchPhase ph;
    switch (phase)
    {
        case 0:
            ph = ER_TOUCH_DOWN;
            break;
        case 1:
            ph = ER_TOUCH_MOVE;
            break;
        case 2:
            ph = ER_TOUCH_UP;
            break;
        default:
            ph = ER_TOUCH_CANCEL;
            break;
    }
    embedded_renderer_touch(0, ph, x, y);
}

EMSCRIPTEN_KEEPALIVE
void er_web_reset(void)
{
    /* Hot reload: drop the JS context + reset the engine, then re-run the stored bundle (full remount). */
    if (s_app_loaded)
    {
        er_runtime_reset();
        if (s_src && !er_runtime_load_source(s_src, (size_t)s_src_len, "<app>"))
            er_runtime_show_error();
    }
    else
    {
        er_software_clear(0xFF000000U);
        present();
    }
}

EMSCRIPTEN_KEEPALIVE
const uint8_t* er_web_framebuffer(void)
{
    return s_rgba;
}

EMSCRIPTEN_KEEPALIVE
int er_web_fb_width(void)
{
    return s_w;
}

EMSCRIPTEN_KEEPALIVE
int er_web_fb_height(void)
{
    return s_h;
}
