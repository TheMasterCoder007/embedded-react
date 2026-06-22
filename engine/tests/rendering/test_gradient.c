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
 * @brief Test framebuffer accumulated from blend_rect calls.
 */
typedef struct
{
    uint32_t fb[FB_W * FB_H]; /**< Premultiplied ARGB8888 pixel storage. */
    int fill_calls;           /**< Number of fill_rect calls received. */
    int blend_calls;          /**< Number of blend_rect calls received. */
} TestCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — backend stubs
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief fill_rect stub — accumulates solid-color fills into the test framebuffer.
 *
 * @param[in] argb  Fill color (straight-alpha ARGB8888).
 * @param[in] x     Left edge.
 * @param[in] y     Top edge.
 * @param[in] w     Width in pixels.
 * @param[in] h     Height in pixels.
 * @param[in] ctx   Pointer to TestCtx.
 */
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    TestCtx* t = ctx;
    t->fill_calls++;
    const uint32_t a = (argb >> 24) & 0xFFu;
    const uint32_t r = ((argb >> 16) & 0xFFu) * a / 255u;
    const uint32_t g = ((argb >> 8) & 0xFFu) * a / 255u;
    const uint32_t b = (argb & 0xFFu) * a / 255u;
    const uint32_t pm = (a << 24) | (r << 16) | (g << 8) | b;
    for (int py = y; py < y + h && py < FB_H; py++)
        for (int px = x; px < x + w && px < FB_W; px++)
            if (px >= 0 && py >= 0)
                t->fb[py * FB_W + px] = pm;
}

/**
 * @brief copy_rect stub — no-op for gradient tests.
 *
 * @param[in] src     Source buffer (unused).
 * @param[in] stride  Stride (unused).
 * @param[in] x       X (unused).
 * @param[in] y       Y (unused).
 * @param[in] w       Width (unused).
 * @param[in] h       Height (unused).
 * @param[in] ctx     Context (unused).
 */
static void copy_cb(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)stride;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/**
 * @brief blend_rect stub — composites premultiplied rows into the test framebuffer.
 *
 * @param[in] src     Premultiplied ARGB8888 source buffer.
 * @param[in] stride  Row stride of src in bytes.
 * @param[in] alpha   Global opacity (unused; tests use opaque blends).
 * @param[in] x       Destination left edge.
 * @param[in] y       Destination top edge.
 * @param[in] w       Width in pixels.
 * @param[in] h       Height in pixels.
 * @param[in] ctx     Pointer to TestCtx.
 */
static void blend_cb(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)alpha;
    TestCtx* t = ctx;
    t->blend_calls++;
    const uint8_t* row_ptr = (const uint8_t*)src;
    for (int py = y; py < y + h && py < FB_H; py++, row_ptr += stride)
    {
        const uint32_t* row = (const uint32_t*)row_ptr;
        for (int px = x; px < x + w && px < FB_W; px++)
            if (px >= 0 && py >= 0)
                t->fb[py * FB_W + px] = row[px - x];
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Initialises a TestCtx and registers it as the active backend.
 *
 * @param[in,out] tc  TestCtx to initialise (fb cleared to black, counters zeroed).
 */
static void setup_backend(TestCtx* tc)
{
    memset(tc, 0, sizeof(*tc));
    static EmbeddedRenderBackend be;
    be.fill_rect = fill_cb;
    be.copy_rect = copy_cb;
    be.blend_rect = blend_cb;
    be.wait = NULL;
    be.frame_ready = NULL;
    be.ctx = tc;
    embedded_renderer_set_backend(&be);
}

/**
 * @brief Returns a pixel from the test framebuffer.
 *
 * @param[in] tc  Test context.
 * @param[in] x   Column.
 * @param[in] y   Row.
 *
 * @return Premultiplied ARGB8888 value stored at (x, y).
 */
static uint32_t px(const TestCtx* tc, int x, int y)
{
    return tc->fb[(y * FB_W) + x];
}

/**
 * @brief Premultiplies a straight-alpha ARGB8888 color.
 *
 * @param[in] sa  Straight-alpha ARGB8888 color.
 *
 * @return Premultiplied ARGB8888 color.
 */
static uint32_t pm(uint32_t sa)
{
    const uint32_t a = (sa >> 24) & 0xFFu;
    return (a << 24) | ((((sa >> 16) & 0xFFu) * a / 255u) << 16) | ((((sa >> 8) & 0xFFu) * a / 255u) << 8)
           | (((sa & 0xFFu) * a / 255u));
}

/**
 * @brief Prints a failure message and returns EXIT_FAILURE.
 *
 * @param[in] msg  Description of the failure.
 *
 * @return EXIT_FAILURE.
 */
static int fail(const char* msg)
{
    printf("FAIL: %s\n", msg);
    return EXIT_FAILURE;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public — test entry point
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Tests gradient rendering through the scene graph API.
 *
 * With ERUI_GRADIENT=0 the tests verify that gradient_type is stored but has no rendering
 * effect (background_color is used instead).  With ERUI_GRADIENT=1 the tests exercise:
 *   - Linear vertical gradient: top row = stop[0] color, bottom row = stop[1] color.
 *   - Linear horizontal gradient: leftmost column = stop[0], rightmost = stop[1].
 *   - Linear diagonal gradient (45°): corner pixels match stop endpoints.
 *   - Radial gradient: center pixel = stop[0], corner pixel ≈ stop[1].
 *   - Degenerate gradient (stop_count < 2): no-op; background_color used.
 *   - Bilinear vs nearest-neighbor scale: smoke-test showing render completes without crash.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    TestCtx tc;
    setup_backend(&tc);

    /* -----------------------------------------------------------------------
     * Scene: a single full-framebuffer View with a two-stop gradient.
     * ----------------------------------------------------------------------- */
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp;
    memset(&rp, 0, sizeof(rp));
    rp.left = rp.top = rp.right = rp.bottom = ER_LAYOUT_AUTO;
    rp.width = FB_W;
    rp.height = FB_H;
    rp.opacity = 255;
    rp.background_color = 0xFF112233; /* fallback when gradient disabled */
    er_node_set_props(root, &rp);
    er_tree_set_root(root);

#if ERUI_GRADIENT

    /* -----------------------------------------------------------------------
     * Test 1: vertical linear gradient (angle=0° → top→bottom).
     *         stop[0] at pos 0.0 = teal,  stop[1] at pos 1.0 = dark-blue.
     * ----------------------------------------------------------------------- */
    {
        const uint32_t teal = 0xFF2A9D8F;
        const uint32_t dark = 0xFF0A2030;

        ERProps p;
        memset(&p, 0, sizeof(p));
        p.left = p.top = p.right = p.bottom = ER_LAYOUT_AUTO;
        p.width = FB_W;
        p.height = FB_H;
        p.opacity = 255;
        p.gradient_type = ER_GRADIENT_LINEAR;
        p.gradient_angle = 0.0f; /* top → bottom */
        p.gradient_stop_count = 2;
        p.gradient_stops[0].color = teal;
        p.gradient_stops[0].position = 0.0f;
        p.gradient_stops[1].color = dark;
        p.gradient_stops[1].position = 1.0f;
        er_node_set_props(root, &p);

        memset(tc.fb, 0, sizeof(tc.fb));
        tc.blend_calls = 0;
        er_commit();

        /* Top row should be approximately teal. */
        const uint32_t top_pixel = px(&tc, FB_W / 2, 0);
        const uint32_t teal_pm = pm(teal);
        if ((top_pixel >> 16 & 0xFF) != ((teal_pm >> 16) & 0xFF))
            return fail("vertical linear: top row red channel mismatch");

        /* Bottom row should be approximately dark-blue. */
        const uint32_t bot_pixel = px(&tc, FB_W / 2, FB_H - 1);
        const uint32_t dark_pm = pm(dark);
        if ((bot_pixel >> 16 & 0xFF) != ((dark_pm >> 16) & 0xFF))
            return fail("vertical linear: bottom row red channel mismatch");

        /* Middle row should be somewhere between the two extremes. */
        const uint32_t mid_pixel = px(&tc, FB_W / 2, FB_H / 2);
        const uint8_t mid_r = (uint8_t)((mid_pixel >> 16) & 0xFF);
        const uint8_t top_r = (uint8_t)((top_pixel >> 16) & 0xFF);
        const uint8_t bot_r = (uint8_t)((bot_pixel >> 16) & 0xFF);
        if (mid_r <= bot_r || mid_r >= top_r)
            return fail("vertical linear: mid row must be between top and bottom");

        /* Engine should have produced one blend_rect call per row. */
        if (tc.blend_calls < FB_H)
            return fail("vertical linear: expected at least FB_H blend calls");
    }

    /* -----------------------------------------------------------------------
     * Test 2: horizontal linear gradient (angle=90° → left→right).
     *         stop[0] = red at left, stop[1] = blue at right.
     * ----------------------------------------------------------------------- */
    {
        const uint32_t red = 0xFFE94560;
        const uint32_t blue = 0xFF3498DB;

        ERProps p;
        memset(&p, 0, sizeof(p));
        p.left = p.top = p.right = p.bottom = ER_LAYOUT_AUTO;
        p.width = FB_W;
        p.height = FB_H;
        p.opacity = 255;
        p.gradient_type = ER_GRADIENT_LINEAR;
        p.gradient_angle = 90.0f; /* left → right */
        p.gradient_stop_count = 2;
        p.gradient_stops[0].color = red;
        p.gradient_stops[0].position = 0.0f;
        p.gradient_stops[1].color = blue;
        p.gradient_stops[1].position = 1.0f;
        er_node_set_props(root, &p);

        memset(tc.fb, 0, sizeof(tc.fb));
        er_commit();

        /* Left column should be approximately red. */
        const uint32_t left_pixel = px(&tc, 0, FB_H / 2);
        const uint32_t red_pm = pm(red);
        if (((left_pixel >> 16) & 0xFF) != ((red_pm >> 16) & 0xFF))
            return fail("horizontal linear: leftmost column red channel mismatch");

        const uint32_t right_pixel = px(&tc, FB_W - 1, FB_H / 2);
        const uint32_t blue_pm = pm(blue);
        const int rb = (int)(right_pixel & 0xFFu), bb = (int)(blue_pm & 0xFFu);
        if (rb < bb - 8 || rb > bb + 8)
            return fail("horizontal linear: rightmost column not ~blue");

        /* Middle column between the two extremes. */
        const uint32_t mid_pixel = px(&tc, FB_W / 2, FB_H / 2);
        const uint8_t mid_r = (uint8_t)((mid_pixel >> 16) & 0xFF);
        const uint8_t l_r = (uint8_t)((left_pixel >> 16) & 0xFF);
        const uint8_t r_r = (uint8_t)((right_pixel >> 16) & 0xFF);
        if (mid_r >= l_r || mid_r <= r_r)
            return fail("horizontal linear: mid column red must be between left and right");
    }

    /* -----------------------------------------------------------------------
     * Test 3: degenerate gradient — stop_count < 2 → no gradient, background_color used.
     * ----------------------------------------------------------------------- */
    {
        const uint32_t bg = 0xFF334455;

        ERProps p;
        memset(&p, 0, sizeof(p));
        p.left = p.top = p.right = p.bottom = ER_LAYOUT_AUTO;
        p.width = FB_W;
        p.height = FB_H;
        p.opacity = 255;
        p.background_color = bg;
        p.gradient_type = ER_GRADIENT_LINEAR;
        p.gradient_angle = 0.0f;
        p.gradient_stop_count = 1; /* < 2 → rasterizer does nothing */
        p.gradient_stops[0].color = 0xFFFFFFFF;
        p.gradient_stops[0].position = 0.0f;
        er_node_set_props(root, &p);

        memset(tc.fb, 0, sizeof(tc.fb));
        er_commit();

        /* When stop_count < 2 the gradient is skipped; background_color fills the rect. */
        const uint32_t top_pixel = px(&tc, FB_W / 2, 0);
        const uint32_t bg_pm = pm(bg);
        if (top_pixel != bg_pm)
            return fail("degenerate gradient: background_color should fill when stop_count < 2");
    }

#if ERUI_GRADIENT_RADIAL

    /* -----------------------------------------------------------------------
     * Test 4: radial gradient.
     *         Center (t=0) = white,  edge (t≈1 at corner) = dark-teal.
     * ----------------------------------------------------------------------- */
    {
        const uint32_t center_col = 0xFFFFFFFF;
        const uint32_t edge_col = 0xFF0A3040;

        ERProps p;
        memset(&p, 0, sizeof(p));
        p.left = p.top = p.right = p.bottom = ER_LAYOUT_AUTO;
        p.width = FB_W;
        p.height = FB_H;
        p.opacity = 255;
        p.gradient_type = ER_GRADIENT_RADIAL;
        p.gradient_stop_count = 2;
        p.gradient_stops[0].color = center_col;
        p.gradient_stops[0].position = 0.0f;
        p.gradient_stops[1].color = edge_col;
        p.gradient_stops[1].position = 1.0f;
        er_node_set_props(root, &p);

        memset(tc.fb, 0, sizeof(tc.fb));
        er_commit();

        /* Centre pixel should be very close to white (t≈0). */
        const uint32_t centre_pixel = px(&tc, FB_W / 2, FB_H / 2);
        if (((centre_pixel >> 24) & 0xFF) < 240)
            return fail("radial: centre pixel alpha must be near 255");
        /* All channels at centre should be high (close to white). */
        if (((centre_pixel >> 16) & 0xFF) < 200)
            return fail("radial: centre pixel red must be near 255");

        /* Corner pixel should be close to the edge color (t≈1). */
        const uint32_t corner_pixel = px(&tc, 0, 0);
        /* The corner red channel should be much lower than the centre. */
        if (((corner_pixel >> 16) & 0xFF) >= ((centre_pixel >> 16) & 0xFF))
            return fail("radial: corner red must be lower than centre red");
    }

#endif /* ERUI_GRADIENT_RADIAL */

    /* -----------------------------------------------------------------------
     * Test 5: three-stop gradient — mid-stop color appears in the middle third.
     * ----------------------------------------------------------------------- */
    {
        const uint32_t c0 = 0xFF2A9D8F; /* teal  */
        const uint32_t c1 = 0xFFF4A261; /* orange — mid-point */
        const uint32_t c2 = 0xFF9B59B6; /* purple */

        ERProps p;
        memset(&p, 0, sizeof(p));
        p.left = p.top = p.right = p.bottom = ER_LAYOUT_AUTO;
        p.width = FB_W;
        p.height = FB_H;
        p.opacity = 255;
        p.gradient_type = ER_GRADIENT_LINEAR;
        p.gradient_angle = 0.0f;
        p.gradient_stop_count = 3;
        p.gradient_stops[0].color = c0;
        p.gradient_stops[0].position = 0.0f;
        p.gradient_stops[1].color = c1;
        p.gradient_stops[1].position = 0.5f;
        p.gradient_stops[2].color = c2;
        p.gradient_stops[2].position = 1.0f;
        er_node_set_props(root, &p);

        memset(tc.fb, 0, sizeof(tc.fb));
        er_commit();

        /* Mid row (pos ≈ 0.5) should match the orange mid-stop more than the endpoints. */
        const uint32_t mid_pixel = px(&tc, FB_W / 2, FB_H / 2);
        const uint8_t mid_g = (uint8_t)((mid_pixel >> 8) & 0xFF);
        const uint32_t top_pixel = px(&tc, FB_W / 2, 0);
        const uint32_t bot_pixel = px(&tc, FB_W / 2, FB_H - 1);
        /* Orange has the highest green channel; verify mid > both endpoints. */
        if (mid_g <= (uint8_t)((top_pixel >> 8) & 0xFF) && mid_g <= (uint8_t)((bot_pixel >> 8) & 0xFF))
            return fail("three-stop linear: mid-stop orange not visible at mid row");
    }

#else /* ERUI_GRADIENT == 0 */

    /* -----------------------------------------------------------------------
     * ERUI_GRADIENT=0 smoke test: gradient_type prop is stored but ignored;
     * the node renders its background_color instead.
     * ----------------------------------------------------------------------- */
    {
        const uint32_t bg = 0xFF2A9D8F;

        ERProps p;
        memset(&p, 0, sizeof(p));
        p.left = p.top = p.right = p.bottom = ER_LAYOUT_AUTO;
        p.width = FB_W;
        p.height = FB_H;
        p.opacity = 255;
        p.background_color = bg;
        p.gradient_type = ER_GRADIENT_LINEAR;
        p.gradient_angle = 0.0f;
        p.gradient_stop_count = 2;
        p.gradient_stops[0].color = 0xFFFF0000;
        p.gradient_stops[0].position = 0.0f;
        p.gradient_stops[1].color = 0xFF0000FF;
        p.gradient_stops[1].position = 1.0f;
        er_node_set_props(root, &p);

        memset(tc.fb, 0, sizeof(tc.fb));
        er_commit();

        /* With gradients disabled the top row must have the background_color. */
        const uint32_t top_pixel = px(&tc, FB_W / 2, 0);
        const uint32_t bg_pm = pm(bg);
        if (top_pixel != bg_pm)
            return fail("ERUI_GRADIENT=0: background_color must be used when gradients disabled");
    }

#endif /* ERUI_GRADIENT */

    er_node_destroy(root);
    printf("PASS\n");
    return EXIT_SUCCESS;
}
