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

#include "er_scene.h"
#include "native_renderer.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FB_W 64
#define FB_H 64

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Pixel framebuffer and dimensions used by the test backend stubs.
 */
typedef struct
{
    uint32_t* fb; /**< Flat premultiplied ARGB8888 framebuffer; rows are fb_w pixels wide. */
    int fb_w;     /**< Framebuffer width in pixels. */
    int fb_h;     /**< Framebuffer height in pixels. */
} TestCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — backend stubs
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Backend fill_rect: premultiplies the color and writes it into the test framebuffer.
 *
 * @param[in] argb  Straight-alpha ARGB8888 fill color.
 * @param[in] x     Left edge of the rectangle.
 * @param[in] y     Top edge of the rectangle.
 * @param[in] w     Width in pixels.
 * @param[in] h     Height in pixels.
 * @param[in] ctx   Pointer to TestCtx.
 */
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    TestCtx* t = ctx;
    const uint8_t a = (uint8_t)((argb >> 24) & 0xFFU);
    if (a == 0U)
        return;
    const uint8_t r = (uint8_t)((uint32_t)((argb >> 16) & 0xFFU) * a / 255U);
    const uint8_t g = (uint8_t)((uint32_t)((argb >> 8) & 0xFFU) * a / 255U);
    const uint8_t b = (uint8_t)((uint32_t)(argb & 0xFFU) * a / 255U);
    const uint32_t premul = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            if (col >= 0 && col < t->fb_w && row >= 0 && row < t->fb_h)
                t->fb[row * t->fb_w + col] = premul;
}

/**
 * @brief Backend copy_rect: copies premultiplied ARGB8888 pixels directly into the framebuffer.
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
        const uint32_t* sr = (const uint32_t*)((const uint8_t*)src + row * stride);
        for (int col = 0; col < w; col++)
        {
            int bx = x + col, by = y + row;
            if (bx >= 0 && bx < t->fb_w && by >= 0 && by < t->fb_h)
                t->fb[by * t->fb_w + bx] = sr[col];
        }
    }
}

/**
 * @brief Backend blend_rect: source-over composites premultiplied pixels into the framebuffer.
 *
 * Scales the source by the global alpha before compositing, matching the behavior the
 * embedded_react scratch system expects from the platform backend.
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
        const uint32_t* sr = (const uint32_t*)((const uint8_t*)src + row * stride);
        for (int col = 0; col < w; col++)
        {
            int bx = x + col, by = y + row;
            if (bx < 0 || bx >= t->fb_w || by < 0 || by >= t->fb_h)
                continue;
            uint32_t sp = sr[col];
            if (alpha < 255U)
            {
                sp = ((uint32_t)((sp >> 24) & 0xFFU) * alpha / 255U << 24)
                     | ((uint32_t)((sp >> 16) & 0xFFU) * alpha / 255U << 16)
                     | ((uint32_t)((sp >> 8) & 0xFFU) * alpha / 255U << 8) | (uint32_t)(sp & 0xFFU) * alpha / 255U;
            }
            const uint8_t sa = (uint8_t)((sp >> 24) & 0xFFU);
            if (sa == 0U)
                continue;
            if (sa == 255U)
            {
                t->fb[by * t->fb_w + bx] = sp;
            }
            else
            {
                const uint32_t d = t->fb[by * t->fb_w + bx];
                const uint8_t inv = (uint8_t)(255U - sa);
                const uint8_t oa = (uint8_t)(sa + (uint32_t)((d >> 24) & 0xFFU) * inv / 255U);
                const uint8_t or_ = (uint8_t)(((sp >> 16) & 0xFFU) + (uint32_t)((d >> 16) & 0xFFU) * inv / 255U);
                const uint8_t og = (uint8_t)(((sp >> 8) & 0xFFU) + (uint32_t)((d >> 8) & 0xFFU) * inv / 255U);
                const uint8_t ob = (uint8_t)((sp & 0xFFU) + (uint32_t)(d & 0xFFU) * inv / 255U);
                t->fb[by * t->fb_w + bx] = ((uint32_t)oa << 24) | ((uint32_t)or_ << 16) | ((uint32_t)og << 8) | ob;
            }
        }
    }
}

/**
 * @brief Prints a failure message to stderr and returns EXIT_FAILURE.
 *
 * @param[in] msg  Human-readable description of the failed assertion.
 *
 * @return EXIT_FAILURE.
 */
static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return EXIT_FAILURE;
}

/**
 * @brief Reads a single premultiplied ARGB8888 pixel from the test framebuffer.
 *
 * @param[in] t  TestCtx owning the framebuffer.
 * @param[in] x  X coordinate.
 * @param[in] y  Y coordinate.
 *
 * @return Premultiplied ARGB8888 pixel value at (x, y).
 */
static uint32_t px(const TestCtx* t, int x, int y)
{
    return t->fb[y * t->fb_w + x];
}

/**
 * @brief Returns an ERProps struct with all layout fields set to ER_LAYOUT_AUTO.
 *
 * Initializes every dimensional field to ER_LAYOUT_AUTO so that unset values are
 * treated as "let the engine decide" rather than 0. View-type opacity is set to 255
 * (fully opaque) to match React Native's default.
 *
 * @return Zero-initialized ERProps with AUTO layout sentinels and opacity 255.
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
 * @brief Zeros the test framebuffer, discarding any pixels written by a previous test case.
 *
 * @param[in,out] t  TestCtx whose framebuffer should be cleared.
 */
static void reset(TestCtx* t)
{
    memset(t->fb, 0, (size_t)t->fb_w * (size_t)t->fb_h * sizeof(uint32_t));
}

/* Press-hit counters and their callbacks for the hit-test cases in main(). These live at file scope
 * because C99 (the engine's standard, with compiler extensions off) has no nested functions; defining
 * them inside main() compiled only under GCC's extension and broke the build under Clang. */
static int s_hit_count;
static int s_3d_hits;
static int s_scale_hits;

static void press_cb(ERNode* n, const EREventData* d, void* u)
{
    (void)n;
    (void)d;
    (void)u;
    s_hit_count++;
}

static void press_3d(ERNode* n, const EREventData* d, void* u)
{
    (void)n;
    (void)d;
    (void)u;
    s_3d_hits++;
}

static void scale_press_cb(ERNode* n, const EREventData* d, void* u)
{
    (void)n;
    (void)d;
    (void)u;
    s_scale_hits++;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Tests 2D transform support: translate, hit-test under translate, and (if built with
 *        ERUI_TRANSFORMS_FULL) scale and hit-test under affine transform.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    static uint32_t fb[FB_W * FB_H];
    TestCtx tc = {fb, FB_W, FB_H};
    EmbeddedRenderBackend be = {fill_cb, copy_cb, blend_cb, NULL, NULL, &tc};
    embedded_renderer_set_backend(&be);

    /* -------------------------------------------------------------------
     * Translate-only: red 8×8 child with translateX=10 at origin.
     * Without transform it would paint at (0,0)–(7,7).
     * With translateX=10 it should paint at (10,0)–(17,7).
     * ------------------------------------------------------------------ */
    reset(&tc);
    {
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = FB_W;
        rp.height = FB_H;
        rp.background_color = 0xFFFFFFFFU; /* white */
        er_node_set_props(root, &rp);

        ERNode* child = er_node_create(ER_NODE_VIEW);
        ERProps cp = props_default();
        cp.width = 8;
        cp.height = 8;
        cp.background_color = 0xFFFF0000U; /* red */
        cp.transform_translate_x = 10.0f;
        er_node_set_props(child, &cp);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* Pixel at (0,0) should remain white (no red paint without transform) */
        if (px(&tc, 0, 0) != 0xFFFFFFFFU)
            return fail("translate: pixel at origin should be white (no untranslated red)");
        /* Pixel at (10,0) should be red (premultiplied: 0xFF FF 00 00) */
        if (px(&tc, 10, 0) != 0xFFFF0000U)
            return fail("translate: pixel at translated position should be red");
        /* Pixel at (17,0) should be red (last column of 8-wide child) */
        if (px(&tc, 17, 0) != 0xFFFF0000U)
            return fail("translate: right edge of translated child should be red");
        /* Pixel at (18,0) outside child — white */
        if (px(&tc, 18, 0) != 0xFFFFFFFFU)
            return fail("translate: pixel just outside translated child should be white");

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }

    /* -------------------------------------------------------------------
     * Hit-test under translate: same layout as above.
     * Touch at (14, 3) should hit the child (which renders at 10–17 x 0–7).
     * Touch at (3, 3) should miss (original position, no longer hittable).
     * ------------------------------------------------------------------ */
    reset(&tc);
    {
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = FB_W;
        rp.height = FB_H;
        er_node_set_props(root, &rp);

        ERNode* child = er_node_create(ER_NODE_VIEW);
        ERProps cp = props_default();
        cp.width = 8;
        cp.height = 8;
        cp.background_color = 0xFFFF0000U;
        cp.transform_translate_x = 10.0f;
        er_node_set_props(child, &cp);

        bool hit_inside = false;
        bool hit_outside = false;

        er_event_set(child, ER_EVENT_PRESS, NULL, NULL); /* make it a press target */

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* Register a simple hit counter (press_cb is at file scope) */
        s_hit_count = 0;
        er_event_set(child, ER_EVENT_PRESS, press_cb, NULL);

        /* Touch inside transformed position */
        embedded_renderer_touch(0, ER_TOUCH_DOWN, 14, 3);
        embedded_renderer_touch(0, ER_TOUCH_UP, 14, 3);
        hit_inside = (s_hit_count > 0);

        s_hit_count = 0;

        /* Touch at the original (untransformed) position — should miss */
        embedded_renderer_touch(0, ER_TOUCH_DOWN, 3, 3);
        embedded_renderer_touch(0, ER_TOUCH_UP, 3, 3);
        hit_outside = (s_hit_count > 0);

        if (!hit_inside)
            return fail("translate hit-test: touch at translated position should hit the child");
        if (hit_outside)
            return fail("translate hit-test: touch at original position should miss after translate");

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }

#if ERUI_3D_TRANSFORMS && ERUI_TRANSFORMS_FULL
    /* -------------------------------------------------------------------
     * rotateY(45°) with perspective(200): a 16×16 red square centred in
     * the framebuffer, rotated 45° around Y with a perspective distance of
     * 200 px.  The left and right edges of the source square project closer
     * together, so the rendered width in screen space is narrower than 16px.
     * We verify:
     *   1. The centre pixel of the node is red (the front face is visible).
     *   2. The untransformed right edge pixel (centre_x+8, centre_y) is
     *      transparent — the foreshortened image does not reach that far.
     *   3. Hit-test at the visible centre hits the node; hit-test at the
     *      original right-edge position misses (or hits the root, not child).
     * ------------------------------------------------------------------ */
    reset(&tc);
    {
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = FB_W;
        rp.height = FB_H;
        rp.background_color = 0xFF000000U; /* black background */
        er_node_set_props(root, &rp);

        /* Place a 16×16 red square in the middle of the framebuffer. */
        const int cx = FB_W / 2;
        const int cy = FB_H / 2;

        ERNode* child = er_node_create(ER_NODE_VIEW);
        ERProps cp = props_default();
        cp.position = ER_POS_ABSOLUTE;
        cp.left = (int16_t)(cx - 8);
        cp.top = (int16_t)(cy - 8);
        cp.width = 16;
        cp.height = 16;
        cp.background_color = 0xFFFF0000U; /* red */
        /* Rotate 45° around Y with perspective 200; origin defaults to center. */
        cp.transform_rotate_y = 45.0f;
        cp.transform_perspective = 200.0f;
        cp.transform_origin_x = -1.0f; /* center (negative = 0.5) */
        cp.transform_origin_y = -1.0f;
        er_node_set_props(child, &cp);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* Centre pixel must be red (the projected face covers the pivot). */
        if (px(&tc, cx, cy) != 0xFFFF0000U)
            return fail("3D rotateY: centre pixel should be red");

        /* The untransformed right edge of the source is at cx+8 from the centre.
         * With a 45° Y-rotation and perspective=200, the foreshortened projected width is
         * ≈ 11 px, so the pixel at cx+8 should remain the black background (no red bleed). */
        if ((px(&tc, cx + 8, cy) & 0x00FF0000U) != 0u)
            return fail("3D rotateY: red channel at cx+8 should be zero — projected width is foreshortened");

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }

    /* -------------------------------------------------------------------
     * Hit-test under 3D rotateY: same layout.
     * Touch at the visible projected centre should hit the child.
     * ------------------------------------------------------------------ */
    reset(&tc);
    {
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = FB_W;
        rp.height = FB_H;
        er_node_set_props(root, &rp);

        const int cx = FB_W / 2;
        const int cy = FB_H / 2;

        ERNode* child = er_node_create(ER_NODE_VIEW);
        ERProps cp = props_default();
        cp.position = ER_POS_ABSOLUTE;
        cp.left = (int16_t)(cx - 8);
        cp.top = (int16_t)(cy - 8);
        cp.width = 16;
        cp.height = 16;
        cp.background_color = 0xFFFF0000U;
        cp.transform_rotate_y = 45.0f;
        cp.transform_perspective = 200.0f;
        cp.transform_origin_x = -1.0f;
        cp.transform_origin_y = -1.0f;
        er_node_set_props(child, &cp);

        s_3d_hits = 0;
        er_event_set(child, ER_EVENT_PRESS, press_3d, NULL);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* Touch at exact centre — must hit the child. */
        embedded_renderer_touch(0, ER_TOUCH_DOWN, cx, cy);
        embedded_renderer_touch(0, ER_TOUCH_UP, cx, cy);
        if (s_3d_hits == 0)
            return fail("3D hit-test: touch at projected centre should hit the child");

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }
#endif /* ERUI_3D_TRANSFORMS && ERUI_TRANSFORMS_FULL */

#if ERUI_TRANSFORMS_FULL
    /* -------------------------------------------------------------------
     * Scale 2×: a 4×4 red square with scale_x=2, scale_y=2.
     * transform_origin defaults to (0,0) (top-left) so the pivot is the
     * top-left corner and the square expands to (0,0)–(7,7) in screen space.
     * ------------------------------------------------------------------ */
    reset(&tc);
    {
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = FB_W;
        rp.height = FB_H;
        rp.background_color = 0xFFFFFFFFU; /* white */
        er_node_set_props(root, &rp);

        ERNode* child = er_node_create(ER_NODE_VIEW);
        ERProps cp = props_default();
        cp.width = 4;
        cp.height = 4;
        cp.background_color = 0xFFFF0000U; /* red */
        cp.transform_scale_x = 2.0f;
        cp.transform_scale_y = 2.0f;
        er_node_set_props(child, &cp);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* transform_origin defaults to (0,0) so the 4×4 square scales around its
         * top-left corner, expanding to (0,0)-(7,7) in screen space.
         * Pixel (4,4) should be red (inside the 8×8 scaled quad). */
        if (px(&tc, 4, 4) != 0xFFFF0000U)
            return fail("scale 2x: pixel at (4,4) should be red (inside scaled quad)");

        /* Pixel (6,6) maps to source (3.0,3.0) — last texel centre — bilinear lands
         * entirely on the red source pixel so the result is full red. */
        if (px(&tc, 6, 6) != 0xFFFF0000U)
            return fail("scale 2x: pixel at (6,6) should be red (last source texel centre)");

        /* Pixel (7,7) maps to source (3.5,3.5): bilinear blends the last red texel with
         * the transparent border; composited on the white background this is a reddish
         * blend where red channel > green channel. */
        {
            const uint32_t p77 = px(&tc, 7, 7);
            if (((p77 >> 16) & 0xFFu) <= ((p77 >> 8) & 0xFFu))
                return fail("scale 2x: pixel at (7,7) should be reddish (bilinear edge blend)");
        }

        /* Pixel (9,9) is outside the 8×8 scaled quad — should be white. */
        if (px(&tc, 9, 9) != 0xFFFFFFFFU)
            return fail("scale 2x: pixel at (9,9) should be white (outside scaled quad)");

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }

    /* -------------------------------------------------------------------
     * Hit-test under scale: same 2× scaled 4×4 child.
     * Touch at (6,6) maps to layout (3,3) — inside — should hit.
     * Touch at (9,9) maps to layout (4.5,4.5) — outside — should miss.
     * ------------------------------------------------------------------ */
    reset(&tc);
    {
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = FB_W;
        rp.height = FB_H;
        er_node_set_props(root, &rp);

        ERNode* child = er_node_create(ER_NODE_VIEW);
        ERProps cp = props_default();
        cp.width = 4;
        cp.height = 4;
        cp.background_color = 0xFFFF0000U;
        cp.transform_scale_x = 2.0f;
        cp.transform_scale_y = 2.0f;
        er_node_set_props(child, &cp);

        s_scale_hits = 0;
        er_event_set(child, ER_EVENT_PRESS, scale_press_cb, NULL);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* Touch inside scaled area at (6,6): layout = (3,3) inside the 4×4 source. */
        embedded_renderer_touch(0, ER_TOUCH_DOWN, 6, 6);
        embedded_renderer_touch(0, ER_TOUCH_UP, 6, 6);
        if (s_scale_hits == 0)
            return fail("scale hit-test: touch at (6,6) should hit the scaled child");

        s_scale_hits = 0;

        /* Touch outside scaled area at (9,9): maps to layout (4.5,4.5) outside 4×4 source. */
        embedded_renderer_touch(0, ER_TOUCH_DOWN, 9, 9);
        embedded_renderer_touch(0, ER_TOUCH_UP, 9, 9);
        if (s_scale_hits != 0)
            return fail("scale hit-test: touch at (9,9) should miss the scaled child");

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }
#endif /* ERUI_TRANSFORMS_FULL */

    return EXIT_SUCCESS;
}
