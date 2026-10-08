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
 * A border sweep draws a light around the rounded border, brightest at its phase and fading behind it,
 * and an animated phase repaints only the bands along the edges, every frame matching a full repaint.
 */
#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FB_W 200
#define FB_H 160

#define ANIM_MS 480 /* animation duration */
#define FRAME_MS 16 /* per-tick advance */
#define FRAMES 40   /* ticks driven (runs past the end so the settled frames are checked too) */

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Framebuffer the backend stubs rasterise into.
 */
typedef struct
{
    uint32_t* fb; /**< Flat ARGB8888 framebuffer. */
    int fb_w;     /**< Width in pixels. */
    int fb_h;     /**< Height in pixels. */
} TestCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — backend stubs
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Rounds a 0–65025 product back to 0–255 the way the engine's blenders do. */
static uint32_t div255(uint32_t v)
{
    return (v + 127U) / 255U;
}

/**
 * @brief Backend fill_rect: source-over composites a straight-alpha colour into the framebuffer.
 *
 * @param[in] argb  Straight-alpha ARGB8888 fill colour.
 * @param[in] x     Left edge.
 * @param[in] y     Top edge.
 * @param[in] w     Width in pixels.
 * @param[in] h     Height in pixels.
 * @param[in] ctx   Pointer to TestCtx.
 */
static long s_area;

static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    s_area += (long)(w > 0 ? w : 0) * (h > 0 ? h : 0);
    TestCtx* t = ctx;
    const uint32_t a = (argb >> 24) & 0xFFU;
    if (a == 0U || w <= 0 || h <= 0)
        return;
    const uint32_t inv = 255U - a;
    const uint32_t sr = (argb >> 16) & 0xFFU, sg = (argb >> 8) & 0xFFU, sb = argb & 0xFFU;

    for (int row = y; row < y + h; row++)
    {
        if (row < 0 || row >= t->fb_h)
            continue;
        for (int col = x; col < x + w; col++)
        {
            if (col < 0 || col >= t->fb_w)
                continue;
            uint32_t* d = &t->fb[row * t->fb_w + col];
            const uint32_t dr = (*d >> 16) & 0xFFU, dg = (*d >> 8) & 0xFFU, db = *d & 0xFFU;
            *d = 0xFF000000U | ((div255(sr * a) + div255(dr * inv)) << 16) | ((div255(sg * a) + div255(dg * inv)) << 8)
                 | (div255(sb * a) + div255(db * inv));
        }
    }
}

/**
 * @brief Backend copy_rect: source-over composites a premultiplied buffer into the framebuffer.
 *
 * Fully transparent source pixels are skipped and the rest blended, matching the engine's own
 * scratch copy — an unconditional overwrite would erase the background under antialiased edges and
 * make the comparison depend on repaint history rather than on scene state.
 *
 * @param[in] src     Source pixel buffer (premultiplied ARGB8888).
 * @param[in] stride  Source row stride in bytes.
 * @param[in] x       Destination left edge.
 * @param[in] y       Destination top edge.
 * @param[in] w       Width in pixels.
 * @param[in] h       Height in pixels.
 * @param[in] ctx     Pointer to TestCtx.
 */
static void copy_cb(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    s_area += (long)(w > 0 ? w : 0) * (h > 0 ? h : 0);
    TestCtx* t = ctx;
    for (int row = 0; row < h; row++)
    {
        const uint32_t* s = (const uint32_t*)((const uint8_t*)src + (size_t)row * (size_t)stride);
        for (int col = 0; col < w; col++)
        {
            const int dx = x + col, dy = y + row;
            if (dx < 0 || dx >= t->fb_w || dy < 0 || dy >= t->fb_h)
                continue;
            const uint32_t sp = s[col];
            const uint32_t sa = (sp >> 24) & 0xFFU;
            if (sa == 0U)
                continue;
            uint32_t* d = &t->fb[dy * t->fb_w + dx];
            if (sa == 0xFFU)
            {
                *d = sp;
                continue;
            }
            const uint32_t inv = 255U - sa;
            const uint32_t dr = (*d >> 16) & 0xFFU, dg = (*d >> 8) & 0xFFU, db = *d & 0xFFU;
            *d = 0xFF000000U | ((((sp >> 16) & 0xFFU) + div255(dr * inv)) << 16)
                 | ((((sp >> 8) & 0xFFU) + div255(dg * inv)) << 8) | ((sp & 0xFFU) + div255(db * inv));
        }
    }
}

/**
 * @brief Backend blend_rect: source-over composites premultiplied pixels scaled by a global alpha.
 *
 * @param[in] src     Source pixel buffer (premultiplied ARGB8888).
 * @param[in] stride  Source row stride in bytes.
 * @param[in] alpha   Global alpha scale 0–255.
 * @param[in] x       Destination left edge.
 * @param[in] y       Destination top edge.
 * @param[in] w       Width in pixels.
 * @param[in] h       Height in pixels.
 * @param[in] ctx     Pointer to TestCtx.
 */
static void blend_cb(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    s_area += (long)(w > 0 ? w : 0) * (h > 0 ? h : 0);
    TestCtx* t = ctx;
    for (int row = 0; row < h; row++)
    {
        const uint32_t* s = (const uint32_t*)((const uint8_t*)src + (size_t)row * (size_t)stride);
        for (int col = 0; col < w; col++)
        {
            const int dx = x + col, dy = y + row;
            if (dx < 0 || dx >= t->fb_w || dy < 0 || dy >= t->fb_h)
                continue;
            const uint32_t sp = s[col];
            const uint32_t sa = div255(((sp >> 24) & 0xFFU) * (uint32_t)alpha);
            if (sa == 0U)
                continue;
            const uint32_t inv = 255U - sa;
            /* Source is premultiplied: scale its colour by the same global alpha, not by sa. */
            const uint32_t sr = div255(((sp >> 16) & 0xFFU) * (uint32_t)alpha);
            const uint32_t sg = div255(((sp >> 8) & 0xFFU) * (uint32_t)alpha);
            const uint32_t sb = div255((sp & 0xFFU) * (uint32_t)alpha);
            uint32_t* d = &t->fb[dy * t->fb_w + dx];
            const uint32_t dr = (*d >> 16) & 0xFFU, dg = (*d >> 8) & 0xFFU, db = *d & 0xFFU;
            *d = 0xFF000000U | ((sr + div255(dr * inv)) << 16) | ((sg + div255(dg * inv)) << 8)
                 | (sb + div255(db * inv));
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Prints a failure message.
 *
 * @param[in] msg  Message describing the failed assertion.
 *
 * @return EXIT_FAILURE, so callers can `return fail(...)`.
 */
static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return EXIT_FAILURE;
}

/**
 * @brief Returns an ERProps with every int16_t layout field set to ER_LAYOUT_AUTO.
 *
 * @return Initialised ERProps with opacity 255.
 */
static ERProps props_default(void)
{
    ERProps p = {0};
    p.left = p.top = p.right = p.bottom = ER_LAYOUT_AUTO;
    p.width = p.height = ER_LAYOUT_AUTO;
    p.min_width = p.max_width = ER_LAYOUT_AUTO;
    p.min_height = p.max_height = ER_LAYOUT_AUTO;
    p.padding = p.padding_left = p.padding_top = ER_LAYOUT_AUTO;
    p.padding_right = p.padding_bottom = ER_LAYOUT_AUTO;
    p.margin = p.margin_left = p.margin_top = ER_LAYOUT_AUTO;
    p.margin_right = p.margin_bottom = ER_LAYOUT_AUTO;
    p.gap = p.row_gap = p.column_gap = ER_LAYOUT_AUTO;
    p.flex_basis = ER_LAYOUT_AUTO;
    p.opacity = 255U;
    return p;
}

static uint32_t px_at(const TestCtx* t, int x, int y)
{
    return t->fb[y * FB_W + x];
}

static unsigned brightness(uint32_t p)
{
    return ((p >> 16) & 0xFFU) + ((p >> 8) & 0xFFU) + (p & 0xFFU);
}

int main(void)
{
    static uint32_t fb[FB_W * FB_H];
    static uint32_t incremental[FB_W * FB_H];
    TestCtx t = {fb, FB_W, FB_H};
    EmbeddedRenderBackend be = {fill_cb, copy_cb, blend_cb, NULL, NULL, &t};
    embedded_renderer_set_backend(&be);

    er_reset();
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = FB_W;
    rp.height = FB_H;
    rp.background_color = 0xFF101010U;
    er_node_set_props(root, &rp);

    /* A 160x120 card at (20, 20) with a 12 px radius and a 3 px white sweep, head at the top centre. */
    ERNode* card = er_node_create(ER_NODE_VIEW);
    ERProps cp = props_default();
    cp.position = ER_POS_ABSOLUTE;
    cp.left = 20;
    cp.top = 20;
    cp.width = 160;
    cp.height = 120;
    cp.border_radius = 12;
    cp.background_color = 0xFF203040U;
    cp.border_sweep_color = 0xFFFFFFFFU;
    cp.border_sweep_width = 3;
    cp.border_sweep_length = 0.3f;
    er_node_set_props(card, &cp);
    er_tree_append_child(root, card);
    er_tree_set_root(root);

    ERAnimConfig cfg = {0};
    cfg.type = ER_ANIM_TIMING;
    cfg.duration_ms = 1000;
    cfg.easing = ER_EASE_LINEAR;
    ERAnimValueHandle phase = er_anim_value_create(0.1f);
    er_anim_value_bind(phase, card, ER_PROP_BORDER_SWEEP_PHASE);
    er_force_full_repaint();
    er_commit();

    /* The straight top edge spans perimeter 0..~0.24: the head (0.1) is on it, a third along. */
    const unsigned head = brightness(px_at(&t, 20 + 12 + 37, 21));
    const unsigned tail = brightness(px_at(&t, 20 + 12 + 5, 21));
    const unsigned dark = brightness(px_at(&t, 100, 20 + 119 - 1));
    const unsigned card_bg = brightness(0xFF203040U);
    if (head < 600)
        return fail("the sweep's head is not bright");
    if (!(tail > card_bg && tail < head))
        return fail("the tail behind the head does not fade");
    if (dark > card_bg + 10)
        return fail("the far side of the border is lit");
    if (brightness(px_at(&t, 100, 80)) != card_bg)
        return fail("the sweep drew inside the card");

    /* What repainting the whole card costs, to compare a phase step against. */
    cp.background_color = 0xFF203041U;
    er_node_set_props(card, &cp);
    s_area = 0;
    er_commit();
    const long whole_card = s_area;

    er_anim_value_animate(phase, 1.1f, &cfg);
    for (int f = 0; f < 20; f++)
    {
        embedded_renderer_tick(16);
        s_area = 0;
        er_commit();
        if (s_area * 2 >= whole_card)
            return fail("a phase step repainted more than half of what the whole card costs");
        memcpy(incremental, fb, sizeof(incremental));
        memset(fb, 0, sizeof(fb));
        er_force_full_repaint();
        er_commit();
        if (memcmp(incremental, fb, sizeof(incremental)) != 0)
        {
            char msg[96];
            snprintf(msg, sizeof(msg), "frame %d of the sweep differs from a full repaint", f);
            return fail(msg);
        }
        memcpy(fb, incremental, sizeof(incremental));
    }
    return EXIT_SUCCESS;
}
