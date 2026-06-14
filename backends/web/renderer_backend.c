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

#include "er_scene.h"
#include "native_renderer.h"
#include "software_backend.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static uint8_t* s_rgba; /**< Canvas-facing RGBA buffer (R,G,B,A), screen_w * screen_h * 4 bytes. */
static int s_w;         /**< Framebuffer width in pixels. */
static int s_h;         /**< Framebuffer height in pixels. */

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

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

    er_reset();          /* clear scene/anim/input pools so the rebuild does not exhaust the node pool */
    er_web_demo_scene(); /* W1: rebuild centered at the new size. W2+: re-run the loaded app at the new size. */
    return 1;
}

EMSCRIPTEN_KEEPALIVE
void er_web_pump(int dt_ms)
{
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
    /* W1: visually clear. W2 wires er_runtime_reset() + reload of the new bundle/pack. */
    er_software_clear(0xFF000000U);
    present();
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
