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
 * A translate moves a node's whole subtree. A header slid up past the screen's top edge by a
 * native-driver translate must take its children with it, and every frame of the slide, the settled
 * ones included, must match a full repaint.
 *
 * The bug this guards: the translate-only fast path offset the node's own paint but not its
 * descendants', which kept painting at their untranslated boxes, so the header's items stayed on
 * screen after it left.
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

#define FB_W 2560
#define FB_H 400

#define ANIM_MS 480 /* animation duration */
#define FRAME_MS 16 /* per-tick advance */
#define FRAMES 40   /* ticks driven (runs past the end so the settled frames are checked too) */
#define ITEM_COLOR 0xFF3DDC84U

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

/**
 * @brief Lifts a header holding one item to translateY `to`, checking every frame and where the item ends.
 *
 * @param[in,out] t            TestCtx owning the framebuffer.
 * @param[in]     to           Final translateY, at least 64 px up.
 * @param[in]     translucent  Paint the header 90% black instead of opaque, like an overlay bar.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed check.
 */
static int run_lift(TestCtx* t, float to, bool translucent)
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

    ERNode* header = er_node_create(ER_NODE_VIEW);
    ERProps hp = props_default();
    hp.position = ER_POS_ABSOLUTE;
    hp.left = 0;
    hp.top = 0;
    hp.width = FB_W;
    hp.height = 128;
    hp.background_color = translucent ? 0xE6000000U : 0xFF4DA3FFU;
    er_node_set_props(header, &hp);

    ERNode* item = er_node_create(ER_NODE_VIEW);
    ERProps ip = props_default();
    ip.width = 200;
    ip.height = 80;
    ip.background_color = ITEM_COLOR;
    er_node_set_props(item, &ip);

    er_tree_append_child(header, item);
    er_tree_append_child(root, header);
    er_tree_set_root(root);
    er_force_full_repaint();
    er_commit();

    ERAnimConfig cfg = {0};
    cfg.type = ER_ANIM_TIMING;
    cfg.duration_ms = ANIM_MS;
    cfg.easing = ER_EASE_LINEAR;
    ERAnimValueHandle lift = er_anim_value_create(0.0f);
    er_anim_value_bind(lift, header, ER_PROP_TRANSLATE_Y);
    er_anim_value_animate(lift, to, &cfg);

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
                     "lift to %.0f (%s): frame %d differs from a full repaint",
                     to,
                     translucent ? "translucent" : "opaque",
                     f);
            return fail(msg);
        }
        memcpy(t->fb, incremental, sizeof(incremental));
    }
    /* Settled: the item moved up with its header, by at least 64 px, so 20 px down it is gone. Comparing
     * against a full repaint alone cannot see this: both drew the item at its untranslated box. */
    if (t->fb[20 * FB_W + 10] == ITEM_COLOR)
    {
        char msg[160];
        snprintf(msg,
                 sizeof(msg),
                 "lift to %.0f (%s): the item stayed where its header left it",
                 to,
                 translucent ? "translucent" : "opaque");
        return fail(msg);
    }
    return EXIT_SUCCESS;
}

int main(void)
{
    static uint32_t fb[FB_W * FB_H];
    TestCtx tc = {fb, FB_W, FB_H};
    EmbeddedRenderBackend be = {fill_cb, copy_cb, blend_cb, NULL, NULL, &tc};
    embedded_renderer_set_backend(&be);

    int result = EXIT_SUCCESS;
    const float targets[] = {-64.0f, -128.0f, -400.0f};
    for (int i = 0; i < 3; i++)
        for (int translucent = 0; translucent < 2; translucent++)
            if (run_lift(&tc, targets[i], translucent) != EXIT_SUCCESS)
                result = EXIT_FAILURE;
    return result;
}
