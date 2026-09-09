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
 * A node moved by a native-driver transform animation must keep painting, frame by frame, on a
 * damage-clipped host.
 *
 * The bug this guards: the compositor exempts transformed subtrees from its prune-by-cached-bounds,
 * off a flag the layout pass caches — and an animation-only frame runs no layout pass. A transform
 * that appeared after the last layout left the node prunable, so the damage clip chasing it across
 * the screen pruned it away and the node went blank.
 *
 * Every frame is compared against a full repaint of the same scene state — the same invariant
 * test_opacity_equiv.c asserts for static prop mutations, here for an animated transform.
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

#define FB_W 320
#define FB_H 120

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
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
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

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — the reproduction
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Slides a bar across the screen on the native driver, checking every frame against a full repaint.
 *
 * The bar starts at translateX 0, so the last layout sees no transform on it, and animates 240 px
 * right — well clear of its 64 px-wide layout box.
 *
 * @param[in,out] t          TestCtx owning the framebuffer.
 * @param[in]     with_fade  Also fade a stacked sibling on the native driver, the shape this was
 *                           first reported as.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first diverging frame.
 */
static int run_slide(TestCtx* t, bool with_fade)
{
    static uint32_t incremental[FB_W * FB_H];

    er_reset();
    memset(t->fb, 0, sizeof(uint32_t) * FB_W * FB_H);

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = FB_W;
    rp.height = FB_H;
    rp.background_color = 0xFF101010U;
    er_node_set_props(root, &rp);

    ERNode* bar = er_node_create(ER_NODE_VIEW);
    ERProps bp = props_default();
    bp.position = ER_POS_ABSOLUTE;
    bp.left = 0;
    bp.top = 0;
    bp.width = 64;
    bp.height = 20;
    bp.background_color = 0xFF4DA3FFU;
    er_node_set_props(bar, &bp);

    ERNode* fader = er_node_create(ER_NODE_VIEW);
    ERProps fp = props_default();
    fp.position = ER_POS_ABSOLUTE;
    fp.left = 0;
    fp.top = 34;
    fp.width = 300;
    fp.height = 20;
    fp.background_color = 0xFF3DDC84U;
    er_node_set_props(fader, &fp);

    er_tree_append_child(root, bar);
    er_tree_append_child(root, fader);
    er_tree_set_root(root);
    er_force_full_repaint();
    er_commit();

    /* Bind AFTER the layout above, exactly as a press handler starting an animation does. */
    ERAnimConfig cfg = {0};
    cfg.type = ER_ANIM_TIMING;
    cfg.duration_ms = ANIM_MS;
    cfg.easing = ER_EASE_LINEAR;

    ERAnimValueHandle slide = er_anim_value_create(0.0f);
    er_anim_value_bind(slide, bar, ER_PROP_TRANSLATE_X);
    er_anim_value_animate(slide, 240.0f, &cfg);

    ERAnimValueHandle fade = er_anim_value_create(0.0f);
    if (with_fade)
    {
        er_anim_value_bind(fade, fader, ER_PROP_OPACITY);
        er_anim_value_animate(fade, 1.0f, &cfg);
    }

    for (int f = 0; f < FRAMES; f++)
    {
        embedded_renderer_tick(FRAME_MS);
        er_commit();
        memcpy(incremental, t->fb, sizeof(incremental));

        memset(t->fb, 0, sizeof(uint32_t) * FB_W * FB_H);
        er_force_full_repaint();
        er_commit();

        if (memcmp(incremental, t->fb, sizeof(incremental)) != 0)
        {
            char msg[160];
            snprintf(msg,
                     sizeof(msg),
                     "%s: frame %d of the slide differs from a full repaint of the same state",
                     with_fade ? "translate + sibling fade" : "translate alone",
                     f);
            return fail(msg);
        }
        /* Continue the incremental history from the incremental frame, not the reference one. */
        memcpy(t->fb, incremental, sizeof(incremental));
    }
    return EXIT_SUCCESS;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Tests that a native-driver transform animation stays visible on a damage-clipped host.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    static uint32_t fb[FB_W * FB_H];
    TestCtx tc = {fb, FB_W, FB_H};
    EmbeddedRenderBackend be = {fill_cb, copy_cb, blend_cb, NULL, NULL, &tc};
    embedded_renderer_set_backend(&be);

    if (run_slide(&tc, false) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (run_slide(&tc, true) != EXIT_SUCCESS)
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}
