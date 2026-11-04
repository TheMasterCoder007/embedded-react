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

#include "native_renderer.h"
#include "vector.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FRAMEBUFFER_WIDTH 128
#define FRAMEBUFFER_HEIGHT 128
#define TWO_PI 6.2831853f

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Pixel framebuffer captured during a render pass.
 */
typedef struct
{
    uint32_t* framebuffer;  /**< Flat ARGB8888 framebuffer; rows are framebuffer_width pixels wide. */
    int framebuffer_width;  /**< Framebuffer width in pixels. */
    int framebuffer_height; /**< Framebuffer height in pixels. */
} TestCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Backend fill_rect callback that writes pixels into a TestCtx framebuffer.
 *
 * The rasterizer forwards anti-aliased spans as fill_rect calls whose color carries the coverage alpha;
 * the real backend composites, but for the test we write the color straight in. Fully-covered interior
 * pixels therefore land as the opaque paint color, edges as partial alpha, and untouched pixels stay 0.
 *
 * @param[in] argb  ARGB8888 fill color (alpha = coverage * paint alpha).
 * @param[in] x     Left edge of the fill rectangle.
 * @param[in] y     Top edge of the fill rectangle.
 * @param[in] w     Width of the fill rectangle in pixels.
 * @param[in] h     Height of the fill rectangle in pixels.
 * @param[in] ctx   Pointer to the TestCtx to update.
 */
static long g_blits;       /**< Backend calls (fill + blend) since the last reset — see the AA-row scenario. */
static long g_blit_pixels; /**< Pixels handed to the backend since the last reset (sum of w*h). */

static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    TestCtx* test_context = ctx;
    g_blits++;
    g_blit_pixels += (long)width * height;
    for (int row = y; row < y + height; row++)
        for (int column = x; column < x + width; column++)
            if (row >= 0 && column >= 0 && row < test_context->framebuffer_height
                && column < test_context->framebuffer_width)
                test_context->framebuffer[row * test_context->framebuffer_width + column] = argb;
}

/** @brief Backend copy_rect callback — no-op stub. */
static void copy_cb(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)stride;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
}

/**
 * @brief Backend blend_rect callback — captures gradient rows into the framebuffer (premultiplied).
 *
 * The gradient fill path flushes premultiplied ARGB rows via er_blit_blend; the real backend composites,
 * but for the test we write them straight in. Fully-covered pixels with opaque stops therefore land as the
 * exact gradient color, so the per-position color assertions hold.
 */
static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)alpha;
    TestCtx* test_context = ctx;
    g_blits++;
    g_blit_pixels += (long)width * height;
    const uint32_t* source_pixels = source;
    const int pitch = (stride > 0) ? stride / (int)sizeof(uint32_t) : width;
    for (int row = 0; row < height; row++)
        for (int column = 0; column < width; column++)
        {
            const int framebuffer_y = y + row, framebuffer_x = x + column;
            if (framebuffer_y >= 0 && framebuffer_x >= 0 && framebuffer_y < test_context->framebuffer_height
                && framebuffer_x < test_context->framebuffer_width)
                test_context->framebuffer[framebuffer_y * test_context->framebuffer_width + framebuffer_x] =
                    source_pixels[row * pitch + column];
        }
}

/**
 * @brief Prints a failure message to stderr and returns EXIT_FAILURE.
 *
 * @param[in] message  Human-readable description of the failed assertion.
 *
 * @return EXIT_FAILURE.
 */
static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return EXIT_FAILURE;
}

/**
 * @brief Reads a single ARGB8888 pixel from the test framebuffer.
 *
 * @param[in] test_context  TestCtx owning the framebuffer.
 * @param[in] x             X coordinate.
 * @param[in] y             Y coordinate.
 *
 * @return ARGB8888 pixel value at (x, y).
 */
static uint32_t px(const TestCtx* test_context, int x, int y)
{
    return test_context->framebuffer[y * test_context->framebuffer_width + x];
}

/** @brief Zeros the framebuffer and the backend-traffic counters. */
static void reset(TestCtx* test_context)
{
    memset(test_context->framebuffer,
           0,
           (size_t)test_context->framebuffer_width * (size_t)test_context->framebuffer_height * sizeof(uint32_t));
    g_blits = 0;
    g_blit_pixels = 0;
}

/** @brief Counts framebuffer pixels the render left non-zero (i.e. that actually took ink). */
static long inked(const TestCtx* test_context)
{
    long ink_count = 0;
    for (int pixel_index = 0; pixel_index < test_context->framebuffer_width * test_context->framebuffer_height;
         pixel_index++)
        if (test_context->framebuffer[pixel_index] != 0u)
            ink_count++;
    return ink_count;
}

/** @brief Counts framebuffer ROWS the render left any ink in — the per-row budget's denominator. */
static long inked_rows(const TestCtx* test_context)
{
    long row_count = 0;
    for (int y = 0; y < test_context->framebuffer_height; y++)
        for (int x = 0; x < test_context->framebuffer_width; x++)
            if (test_context->framebuffer[y * test_context->framebuffer_width + x] != 0u)
            {
                row_count++;
                break;
            }
    return row_count;
}

/** @brief True if the pixel is fully opaque (alpha == 0xFF) with the given RGB. */
static int is_solid(uint32_t pixel, uint32_t rgb)
{
    return ((pixel >> 24) == 0xFFu) && ((pixel & 0x00FFFFFFu) == (rgb & 0x00FFFFFFu));
}

/** @brief Channel extractors for gradient color assertions. */
static int redOf(uint32_t paint)
{
    return (int)((paint >> 16) & 0xFFu);
}
static int grnOf(uint32_t paint)
{
    return (int)((paint >> 8) & 0xFFu);
}
static int bluOf(uint32_t paint)
{
    return (int)(paint & 0xFFu);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Test entry point — exercises er_vector_render() at the pixel level.
 *
 * Verifies that:
 *   - A filled rectangle (nonzero) fills its interior and nothing outside.
 *   - A nonzero filled circle (VOP_ARC) fills its center and clears beyond its radius.
 *   - An even-odd shape with an inner loop leaves a hole (the inner region is NOT filled).
 *   - A stroked line with round caps fills the band AND the cap region that overlaps the band — the
 *     regression guard for the round-cap winding bug (opposite-wound cap punched a hole in the stroke).
 *   - The clip box bounds the rasterize: geometry outside the clip emits no pixels.
 *   - An anti-aliased row reaches the backend as a few spans covering exactly the inked pixels —
 *     not one call per equal-alpha run, and not one call spanning the shape's holes.
 *   - Exhausting the per-node storage pool raises the sticky overflow flag (and a full-but-coping
 *     pool does not) — the signal that separates "missing shapes" from "pool merely at capacity".
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    static uint32_t framebuffer[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];
    TestCtx test_context = {framebuffer, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT};
    EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, &test_context};
    embedded_renderer_set_backend(&backend);

    /* --- filled rectangle (nonzero): interior filled, outside empty --- */
    {
        reset(&test_context);
        const float tape[] = {ER_VECTOR_OP_SHAPE,
                              0,
                              ER_VECTOR_OP_MOVE,
                              10,
                              10,
                              ER_VECTOR_OP_LINE,
                              30,
                              10,
                              ER_VECTOR_OP_LINE,
                              30,
                              30,
                              ER_VECTOR_OP_LINE,
                              10,
                              30,
                              ER_VECTOR_OP_CLOSE};
        const ERVectorPaint paint = {0xFF112233u, 0, 0.0f, 0.0f, 0, 0, ER_VECTOR_FILL_NONZERO};
        er_vector_render(tape,
                         (int)(sizeof(tape) / sizeof(tape[0])),
                         &paint,
                         1,
                         NULL,
                         0,
                         0,
                         0,
                         0,
                         0,
                         FRAMEBUFFER_WIDTH,
                         FRAMEBUFFER_HEIGHT);
        if (!is_solid(px(&test_context, 20, 20), 0x112233u))
            return fail("filled rect: interior pixel not filled");
        if (px(&test_context, 5, 5) != 0)
            return fail("filled rect: pixel outside the rect was filled");
    }

    /* --- filled circle via a full VOP_ARC (nonzero) --- */
    {
        reset(&test_context);
        const float tape[] = {ER_VECTOR_OP_SHAPE,
                              0,
                              ER_VECTOR_OP_MOVE,
                              94,
                              64,
                              ER_VECTOR_OP_ARC,
                              64,
                              64,
                              30,
                              0.0f,
                              TWO_PI,
                              0,
                              ER_VECTOR_OP_CLOSE};
        const ERVectorPaint paint = {0xFF00FF00u, 0, 0.0f, 0.0f, 0, 0, ER_VECTOR_FILL_NONZERO};
        er_vector_render(tape,
                         (int)(sizeof(tape) / sizeof(tape[0])),
                         &paint,
                         1,
                         NULL,
                         0,
                         0,
                         0,
                         0,
                         0,
                         FRAMEBUFFER_WIDTH,
                         FRAMEBUFFER_HEIGHT);
        if (!is_solid(px(&test_context, 64, 64), 0x00FF00u))
            return fail("filled circle: center pixel not filled");
        if (px(&test_context, 110, 64) != 0) /* 46px from center, well outside r=30 */
            return fail("filled circle: pixel beyond the radius was filled");
    }

    /* --- even-odd hole: outer loop + inner loop wound the same way leaves the inner region empty --- */
    {
        reset(&test_context);
        const float tape[] = {ER_VECTOR_OP_SHAPE,
                              0,
                              ER_VECTOR_OP_MOVE,
                              10,
                              10,
                              ER_VECTOR_OP_LINE,
                              40,
                              10,
                              ER_VECTOR_OP_LINE,
                              40,
                              40,
                              ER_VECTOR_OP_LINE,
                              10,
                              40,
                              ER_VECTOR_OP_CLOSE,
                              ER_VECTOR_OP_MOVE,
                              20,
                              20,
                              ER_VECTOR_OP_LINE,
                              30,
                              20,
                              ER_VECTOR_OP_LINE,
                              30,
                              30,
                              ER_VECTOR_OP_LINE,
                              20,
                              30,
                              ER_VECTOR_OP_CLOSE};
        const ERVectorPaint paint = {0xFF445566u, 0, 0.0f, 0.0f, 0, 0, ER_VECTOR_FILL_EVENODD};
        er_vector_render(tape,
                         (int)(sizeof(tape) / sizeof(tape[0])),
                         &paint,
                         1,
                         NULL,
                         0,
                         0,
                         0,
                         0,
                         0,
                         FRAMEBUFFER_WIDTH,
                         FRAMEBUFFER_HEIGHT);
        if (!is_solid(px(&test_context, 15, 25), 0x445566u))
            return fail("even-odd: ring (between outer and inner) not filled");
        if (px(&test_context, 25, 25) != 0)
            return fail("even-odd: inner region should be a hole");
    }

    /* --- stroked line with round caps: the regression guard for the cap winding bug --- */
    /* Horizontal line (40,30)->(80,30), 10px wide => band y=25..35. Round caps (r=5) past each end.   */
    {
        reset(&test_context);
        const float tape[] = {ER_VECTOR_OP_SHAPE, 0, ER_VECTOR_OP_MOVE, 40, 30, ER_VECTOR_OP_LINE, 80, 30};
        const ERVectorPaint paint = {
            0, 0xFFF4A261u, 10.0f, 4.0f, ER_VECTOR_CAP_ROUND, ER_VECTOR_JOIN_ROUND, ER_VECTOR_FILL_NONZERO};
        er_vector_render(tape,
                         (int)(sizeof(tape) / sizeof(tape[0])),
                         &paint,
                         1,
                         NULL,
                         0,
                         0,
                         0,
                         0,
                         0,
                         FRAMEBUFFER_WIDTH,
                         FRAMEBUFFER_HEIGHT);
        if (!is_solid(px(&test_context, 60, 30), 0xF4A261u))
            return fail("stroke: band center not filled");
        /* (77,30): inside the band AND inside the right cap disc — if the cap winds opposite to the band
         * the two cancel to a hole here (the bug). Must be solid. */
        if (!is_solid(px(&test_context, 77, 30), 0xF4A261u))
            return fail("stroke round cap: band+cap overlap is a hole (cap winding regression)");
        if (px(&test_context, 92, 30) != 0) /* 12px past the end, beyond the r=5 cap */
            return fail("stroke: pixel beyond the round cap was filled");
    }

    /* --- clip box bounds the rasterize: geometry left of clipx0 emits nothing --- */
    {
        reset(&test_context);
        const float tape[] = {ER_VECTOR_OP_SHAPE,
                              0,
                              ER_VECTOR_OP_MOVE,
                              10,
                              10,
                              ER_VECTOR_OP_LINE,
                              30,
                              10,
                              ER_VECTOR_OP_LINE,
                              30,
                              30,
                              ER_VECTOR_OP_LINE,
                              10,
                              30,
                              ER_VECTOR_OP_CLOSE};
        const ERVectorPaint paint = {0xFF112233u, 0, 0.0f, 0.0f, 0, 0, ER_VECTOR_FILL_NONZERO};
        er_vector_render(tape,
                         (int)(sizeof(tape) / sizeof(tape[0])),
                         &paint,
                         1,
                         NULL,
                         0,
                         0,
                         0,
                         25,
                         0,
                         FRAMEBUFFER_WIDTH,
                         FRAMEBUFFER_HEIGHT);
        if (px(&test_context, 20, 20) != 0)
            return fail("clip: pixel left of clipx0 was rasterized");
        if (!is_solid(px(&test_context, 28, 28), 0x112233u))
            return fail("clip: pixel inside the clip box not filled");
    }

#if ERUI_GRADIENT
    /* --- linear gradient fill: red at the left edge -> blue at the right, blended across a rect --- */
    {
        reset(&test_context);
        const float tape[] = {ER_VECTOR_OP_SHAPE,
                              0,
                              ER_VECTOR_OP_MOVE,
                              10,
                              10,
                              ER_VECTOR_OP_LINE,
                              50,
                              10,
                              ER_VECTOR_OP_LINE,
                              50,
                              50,
                              ER_VECTOR_OP_LINE,
                              10,
                              50,
                              ER_VECTOR_OP_CLOSE};
        const ERVectorPaint paint = {0, 0, 0.0f, 0.0f, 0, 0, ER_VECTOR_FILL_NONZERO, 1 /* fill_gradient = grads[0] */};
        ERVectorGradient gradient;
        memset(&gradient, 0, sizeof(gradient));
        gradient.type = ER_GRADIENT_LINEAR;
        gradient.stop_count = 2;
        gradient.stops[0].color = 0xFFFF0000u;
        gradient.stops[0].position = 0.0f;
        gradient.stops[1].color = 0xFF0000FFu;
        gradient.stops[1].position = 1.0f;
        gradient.ax = 10.0f;
        gradient.ay = 10.0f;
        gradient.bx = 50.0f;
        gradient.by = 10.0f; /* horizontal axis */
        er_vector_render(tape,
                         (int)(sizeof(tape) / sizeof(tape[0])),
                         &paint,
                         1,
                         &gradient,
                         1,
                         0,
                         0,
                         0,
                         0,
                         FRAMEBUFFER_WIDTH,
                         FRAMEBUFFER_HEIGHT);
        const uint32_t left_pixel = px(&test_context, 12, 30), right_pixel = px(&test_context, 48, 30),
                       middle_pixel = px(&test_context, 30, 30);
        if (((left_pixel >> 24) & 0xFFu) != 0xFFu)
            return fail("linear gradient: interior pixel not opaque");
        if (!(redOf(left_pixel) > 200 && bluOf(left_pixel) < 60))
            return fail("linear gradient: left edge should be ~red");
        if (!(bluOf(right_pixel) > 200 && redOf(right_pixel) < 60))
            return fail("linear gradient: right edge should be ~blue");
        if (!(redOf(middle_pixel) > 90 && redOf(middle_pixel) < 170 && bluOf(middle_pixel) > 90
              && bluOf(middle_pixel) < 170))
            return fail("linear gradient: midpoint should blend red+blue");
        if (px(&test_context, 5, 5) != 0)
            return fail("linear gradient: pixel outside the rect was filled");
    }

    /* --- linear gradient STROKE: a thick horizontal line whose stroke ramps red -> blue along its length --- */
    {
        reset(&test_context);
        const float tape[] = {ER_VECTOR_OP_SHAPE, 0, ER_VECTOR_OP_MOVE, 40, 30, ER_VECTOR_OP_LINE, 80, 30};
        /* fill 0, stroke 0 (no solid colour), width 10, round caps, fill_gradient 0, stroke_gradient 1. */
        const ERVectorPaint paint = {
            0, 0, 10.0f, 4.0f, ER_VECTOR_CAP_ROUND, ER_VECTOR_JOIN_ROUND, ER_VECTOR_FILL_NONZERO, 0, 1};
        ERVectorGradient gradient;
        memset(&gradient, 0, sizeof(gradient));
        gradient.type = ER_GRADIENT_LINEAR;
        gradient.stop_count = 2;
        gradient.stops[0].color = 0xFFFF0000u;
        gradient.stops[0].position = 0.0f;
        gradient.stops[1].color = 0xFF0000FFu;
        gradient.stops[1].position = 1.0f;
        gradient.ax = 40.0f;
        gradient.ay = 30.0f;
        gradient.bx = 80.0f;
        gradient.by = 30.0f; /* horizontal axis along the line */
        er_vector_render(tape,
                         (int)(sizeof(tape) / sizeof(tape[0])),
                         &paint,
                         1,
                         &gradient,
                         1,
                         0,
                         0,
                         0,
                         0,
                         FRAMEBUFFER_WIDTH,
                         FRAMEBUFFER_HEIGHT);
        const uint32_t left_pixel = px(&test_context, 45, 30), right_pixel = px(&test_context, 75, 30);
        if (((left_pixel >> 24) & 0xFFu) != 0xFFu)
            return fail("gradient stroke: band pixel not opaque");
        if (!(redOf(left_pixel) > 180 && bluOf(left_pixel) < 80))
            return fail("gradient stroke: left of the band should be ~red");
        if (!(bluOf(right_pixel) > 180 && redOf(right_pixel) < 80))
            return fail("gradient stroke: right of the band should be ~blue");
    }
#endif

#if ERUI_GRADIENT_RADIAL
    /* --- radial gradient fill: bright centre fading to a dark edge --- */
    {
        reset(&test_context);
        const float tape[] = {ER_VECTOR_OP_SHAPE,
                              0,
                              ER_VECTOR_OP_MOVE,
                              10,
                              10,
                              ER_VECTOR_OP_LINE,
                              50,
                              10,
                              ER_VECTOR_OP_LINE,
                              50,
                              50,
                              ER_VECTOR_OP_LINE,
                              10,
                              50,
                              ER_VECTOR_OP_CLOSE};
        const ERVectorPaint paint = {0, 0, 0.0f, 0.0f, 0, 0, ER_VECTOR_FILL_NONZERO, 1};
        ERVectorGradient gradient;
        memset(&gradient, 0, sizeof(gradient));
        gradient.type = ER_GRADIENT_RADIAL;
        gradient.stop_count = 2;
        gradient.stops[0].color = 0xFFFFFFFFu;
        gradient.stops[0].position = 0.0f;
        gradient.stops[1].color = 0xFF000000u;
        gradient.stops[1].position = 1.0f;
        gradient.ax = 30.0f;
        gradient.ay = 30.0f;
        gradient.r = 20.0f;
        er_vector_render(tape,
                         (int)(sizeof(tape) / sizeof(tape[0])),
                         &paint,
                         1,
                         &gradient,
                         1,
                         0,
                         0,
                         0,
                         0,
                         FRAMEBUFFER_WIDTH,
                         FRAMEBUFFER_HEIGHT);
        const uint32_t center_pixel = px(&test_context, 30, 30),
                       edge_pixel = px(&test_context, 30, 46); /* centre; 16px out (t=0.8) */
        if (!(redOf(center_pixel) > 200 && grnOf(center_pixel) > 200 && bluOf(center_pixel) > 200))
            return fail("radial gradient: centre should be ~white");
        if (!(redOf(edge_pixel) < 90 && grnOf(edge_pixel) < 90 && bluOf(edge_pixel) < 90))
            return fail("radial gradient: outer pixel should be ~dark");
    }
#endif

#if ERUI_GRADIENT_CONIC
    /* --- conic gradient fill: colour sweeps by ANGLE around the centre (red at top -> green at bottom) --- */
    {
        reset(&test_context);
        const float tape[] = {ER_VECTOR_OP_SHAPE,
                              0,
                              ER_VECTOR_OP_MOVE,
                              10,
                              10,
                              ER_VECTOR_OP_LINE,
                              50,
                              10,
                              ER_VECTOR_OP_LINE,
                              50,
                              50,
                              ER_VECTOR_OP_LINE,
                              10,
                              50,
                              ER_VECTOR_OP_CLOSE};
        const ERVectorPaint paint = {0, 0, 0.0f, 0.0f, 0, 0, ER_VECTOR_FILL_NONZERO, 1, 0};
        ERVectorGradient gradient;
        memset(&gradient, 0, sizeof(gradient));
        gradient.type = ER_GRADIENT_CONIC;
        gradient.stop_count = 3;
        gradient.stops[0].color = 0xFFFF0000u; /* t=0   (top)    red   */
        gradient.stops[0].position = 0.0f;
        gradient.stops[1].color = 0xFF00FF00u; /* t=0.5 (bottom) green */
        gradient.stops[1].position = 0.5f;
        gradient.stops[2].color = 0xFF0000FFu; /* t=1   (top)    blue  */
        gradient.stops[2].position = 1.0f;
        gradient.ax = 30.0f;
        gradient.ay = 30.0f;
        gradient.r = 0.0f; /* start angle */
        er_vector_render(tape,
                         (int)(sizeof(tape) / sizeof(tape[0])),
                         &paint,
                         1,
                         &gradient,
                         1,
                         0,
                         0,
                         0,
                         0,
                         FRAMEBUFFER_WIDTH,
                         FRAMEBUFFER_HEIGHT);
        const uint32_t top_pixel = px(&test_context, 30, 14),
                       bottom_pixel = px(&test_context, 30, 46); /* directly above / below the centre */
        if (!(redOf(top_pixel) > 180 && grnOf(top_pixel) < 90))
            return fail("conic gradient: top (t=0) should be ~red");
        if (!(grnOf(bottom_pixel) > 180 && redOf(bottom_pixel) < 90))
            return fail("conic gradient: bottom (t=0.5) should be ~green");
    }
#endif

    /* --- clip rejection keeps a shape whose STROKE reaches the clip, drops one that cannot --- */
    /* The rasterizer skips a whole shape when its bounds cannot touch the clip. The bounds have to
     * carry the paint's reach: a path outside the clip whose fat stroke crosses into it must still
     * paint, or wide strokes lose their edges at every damage-rect seam. */
    {
        reset(&test_context);
        /* Shape 0: a vertical line at x=20, 20px wide -> covers x=10..30. Clip starts at x=25.
         * Shape 1: a filled rect at x=90..110, nowhere near the clip. */
        const float tape[] = {ER_VECTOR_OP_SHAPE,
                              0,
                              ER_VECTOR_OP_MOVE,
                              20,
                              10,
                              ER_VECTOR_OP_LINE,
                              20,
                              60,
                              ER_VECTOR_OP_SHAPE,
                              1,
                              ER_VECTOR_OP_MOVE,
                              90,
                              10,
                              ER_VECTOR_OP_LINE,
                              110,
                              10,
                              ER_VECTOR_OP_LINE,
                              110,
                              60,
                              ER_VECTOR_OP_LINE,
                              90,
                              60,
                              ER_VECTOR_OP_CLOSE};
        const ERVectorPaint paint[2] = {
            {0, 0xFF3366CCu, 20.0f, 4.0f, ER_VECTOR_CAP_BUTT, ER_VECTOR_JOIN_MITER, ER_VECTOR_FILL_NONZERO},
            {0xFFCC3366u, 0, 0.0f, 0.0f, 0, 0, ER_VECTOR_FILL_NONZERO},
        };
        er_vector_render(
            tape, (int)(sizeof(tape) / sizeof(tape[0])), paint, 2, NULL, 0, 0, 0, 25, 0, 60, FRAMEBUFFER_HEIGHT);
        if (!is_solid(px(&test_context, 28, 35), 0x3366CCu))
            return fail("clip reject: a stroke reaching into the clip from outside was dropped");
        if (px(&test_context, 100, 35) != 0)
            return fail("clip reject: a shape outside the clip painted anyway");
    }

#if ERUI_GRADIENT
    /* --- clip rejection sizes its margin for a GRADIENT stroke too --- */
    /* A gradient stroke paints from the gradient table and leaves the solid stroke colour transparent.
     * Sizing the margin off that colour hands back a bare 1 px for a stroke of any width, and the shape
     * gets dropped while its ink is still well inside the clip. */
    {
        reset(&test_context);
        /* A vertical line at x=20, 20 px wide -> ink covers x=10..30. The clip starts at x=25. */
        const float tape[] = {ER_VECTOR_OP_SHAPE, 0, ER_VECTOR_OP_MOVE, 20, 10, ER_VECTOR_OP_LINE, 20, 60};
        ERVectorPaint paint;
        memset(&paint, 0, sizeof(paint));
        paint.stroke = 0x00000000u; /* transparent: only the gradient paints */
        paint.stroke_width = 20.0f;
        paint.miter = 4.0f;
        paint.cap = ER_VECTOR_CAP_BUTT;
        paint.join = ER_VECTOR_JOIN_MITER;
        paint.stroke_gradient = 1;
        ERVectorGradient gradient;
        memset(&gradient, 0, sizeof(gradient));
        gradient.type = ER_GRADIENT_LINEAR;
        gradient.stop_count = 2;
        gradient.stops[0].color = 0xFFFF0000u;
        gradient.stops[0].position = 0.0f;
        gradient.stops[1].color = 0xFF0000FFu;
        gradient.stops[1].position = 1.0f;
        gradient.ax = 10.0f;
        gradient.ay = 10.0f;
        gradient.bx = 30.0f;
        gradient.by = 10.0f;
        er_vector_render(tape,
                         (int)(sizeof(tape) / sizeof(tape[0])),
                         &paint,
                         1,
                         &gradient,
                         1,
                         0,
                         0,
                         25,
                         0,
                         FRAMEBUFFER_WIDTH,
                         FRAMEBUFFER_HEIGHT);
        const uint32_t pixel = px(&test_context, 28, 35);
        if (((pixel >> 24) & 0xFFu) == 0u)
            return fail("clip reject: a gradient stroke reaching into the clip was dropped");
        if (!(bluOf(pixel) > redOf(pixel)))
            return fail("clip reject: the gradient stroke painted the wrong colour");
    }
#endif

    /* --- a shallow polyline stroke has no notch at its corners --- */
    /* Corners are covered either by join geometry or by mitering the two segment quads together;
     * neither may leave a gap. A flattened curve is exactly this shape, so a bite here shows up as a
     * dotted seam running along every stroked arc. */
    {
        reset(&test_context);
        /* A 12px-wide stroke along a gently bending polyline (about 8 degrees per vertex). */
        float tape[2 + 3 + 3 * 12];
        int tape_count = 0;
        tape[tape_count++] = ER_VECTOR_OP_SHAPE;
        tape[tape_count++] = 0;
        tape[tape_count++] = ER_VECTOR_OP_MOVE;
        tape[tape_count++] = 10.0f;
        tape[tape_count++] = 64.0f;
        for (int vertex_index = 1; vertex_index <= 12; vertex_index++)
        {
            const float bend = (float)vertex_index * 0.14f;
            tape[tape_count++] = ER_VECTOR_OP_LINE;
            tape[tape_count++] = 10.0f + (float)vertex_index * 8.0f;
            tape[tape_count++] = 64.0f - 30.0f * (1.0f - cosf(bend));
        }
        const ERVectorPaint paint = {
            0, 0xFF22DD88u, 12.0f, 4.0f, ER_VECTOR_CAP_BUTT, ER_VECTOR_JOIN_ROUND, ER_VECTOR_FILL_NONZERO};
        er_vector_render(tape, tape_count, &paint, 1, NULL, 0, 0, 0, 0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT);
        for (int vertex_index = 1; vertex_index <= 11; vertex_index++)
        {
            const float bend = (float)vertex_index * 0.14f;
            const int vertex_x = (int)(10.0f + (float)vertex_index * 8.0f);
            const int vertex_y = (int)(64.0f - 30.0f * (1.0f - cosf(bend)));
            if (!is_solid(px(&test_context, vertex_x, vertex_y), 0x22DD88u))
                return fail("stroke corner: the band is not solid at a vertex (a join left a gap)");
        }
    }

    /* --- a closed polygon's wrap vertex is stroked like every other corner --- */
    /* The corner where a closed subpath's last segment meets its first is shared between two quads
     * that are built at opposite ends of the loop; if only one of them knows about it, the seam opens
     * at exactly one point of the ring — the point most likely to be dismissed as an artefact. */
    {
        reset(&test_context);
        float tape[2 + 3 + 3 * 24 + 1];
        int tape_count = 0;
        tape[tape_count++] = ER_VECTOR_OP_SHAPE;
        tape[tape_count++] = 0;
        for (int step_index = 0; step_index <= 24; step_index++)
        {
            const float angle = (float)step_index * (TWO_PI / 24.0f);
            tape[tape_count++] = (step_index == 0) ? ER_VECTOR_OP_MOVE : ER_VECTOR_OP_LINE;
            tape[tape_count++] = 64.0f + 45.0f * cosf(angle);
            tape[tape_count++] = 64.0f + 45.0f * sinf(angle);
        }
        tape[tape_count++] = ER_VECTOR_OP_CLOSE;
        const ERVectorPaint paint = {
            0, 0xFFEEBB33u, 6.0f, 4.0f, ER_VECTOR_CAP_BUTT, ER_VECTOR_JOIN_ROUND, ER_VECTOR_FILL_NONZERO};
        er_vector_render(tape, tape_count, &paint, 1, NULL, 0, 0, 0, 0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT);
        for (int step_index = 0; step_index < 24; step_index++)
        {
            const float angle = (float)step_index * (TWO_PI / 24.0f);
            const int vertex_x = (int)(64.0f + 45.0f * cosf(angle) + 0.5f);
            const int vertex_y = (int)(64.0f + 45.0f * sinf(angle) + 0.5f);
            if (!is_solid(px(&test_context, vertex_x, vertex_y), 0xEEBB33u))
                return fail("closed stroke: a corner of the ring is not solid (the wrap seam opened)");
        }
    }

    /* --- an anti-aliased row reaches the backend as a few spans, not one per pixel --- */
    /* A stroked ring is nothing but AA fringe, and its rows are two thin bands with a wide hole
     * between them. The rasterizer stages each covered stretch as a premultiplied row and blends it
     * in one call; emitting per equal-alpha run instead costs a backend call — a clip walk, a
     * dispatch, and on a DMA backend a transfer fence — for very nearly every pixel. Two properties
     * are asserted, because the cheap way to cut calls would be to blend the whole row: the backend
     * is handed exactly the pixels that took ink (never the hole), and the call count stays a small
     * multiple of the ring's row count rather than of its pixel count. */
    {
        reset(&test_context);
        float tape[2 + 3 + 3 * 48 + 1];
        int tape_count = 0;
        tape[tape_count++] = ER_VECTOR_OP_SHAPE;
        tape[tape_count++] = 0;
        for (int step_index = 0; step_index <= 48; step_index++)
        {
            const float angle = (float)step_index * (TWO_PI / 48.0f);
            tape[tape_count++] = (step_index == 0) ? ER_VECTOR_OP_MOVE : ER_VECTOR_OP_LINE;
            tape[tape_count++] = 64.0f + 50.0f * cosf(angle);
            tape[tape_count++] = 64.0f + 50.0f * sinf(angle);
        }
        tape[tape_count++] = ER_VECTOR_OP_CLOSE;
        const ERVectorPaint paint = {
            0, 0xFF66CCFFu, 5.0f, 4.0f, ER_VECTOR_CAP_BUTT, ER_VECTOR_JOIN_ROUND, ER_VECTOR_FILL_NONZERO};
        er_vector_render(tape, tape_count, &paint, 1, NULL, 0, 0, 0, 0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT);

        if (g_blit_pixels != inked(&test_context))
            return fail("AA row: the backend was handed pixels the shape never inked (the ring's hole?)");
        /* The ring puts two bands in every row it reaches, so two spans per row is the floor. Budget
         * four — room for a band an AA gap splits in two — against the row count taken from the render
         * itself: the ink's vertical extent follows the radius, the stroke width AND the AA fringe, so
         * a literal here would be a guess that silently loosens if the shape is ever retuned. A
         * per-run emit blows this by a wide margin (it ran to thousands of calls for these rows). */
        const long budget = 4 * inked_rows(&test_context);
        if (g_blits > budget)
            return fail("AA row: too many backend calls — AA spans are being emitted one run at a time");
    }

    /* --- a CLOSE that misses its start point still strokes the closing edge --- */
    /* A ring sector: out along the outer arc, in on a radial line, back along the inner arc, CLOSE.
     * The second radial edge exists only because of the CLOSE. The fill closes every subpath
     * implicitly, so when the stroke did not, that one side came out bare. */
    {
        reset(&test_context);
        const float center_x = 64.0f, center_y = 64.0f, outer_radius = 50.0f, inner_radius = 30.0f;
        const float start_angle = 2.4f, end_angle = start_angle + 4.5f;
        const float tape[] = {ER_VECTOR_OP_SHAPE,
                              0,
                              ER_VECTOR_OP_MOVE,
                              center_x + outer_radius * cosf(start_angle),
                              center_y + outer_radius * sinf(start_angle),
                              ER_VECTOR_OP_ARC,
                              center_x,
                              center_y,
                              outer_radius,
                              start_angle,
                              end_angle,
                              0,
                              ER_VECTOR_OP_LINE,
                              center_x + inner_radius * cosf(end_angle),
                              center_y + inner_radius * sinf(end_angle),
                              ER_VECTOR_OP_ARC,
                              center_x,
                              center_y,
                              inner_radius,
                              end_angle,
                              start_angle,
                              1,
                              ER_VECTOR_OP_CLOSE};
        const ERVectorPaint paint = {
            0, 0xFF66CCFFu, 5.0f, 4.0f, ER_VECTOR_CAP_BUTT, ER_VECTOR_JOIN_MITER, ER_VECTOR_FILL_NONZERO};
        er_vector_render(tape,
                         (int)(sizeof(tape) / sizeof(tape[0])),
                         &paint,
                         1,
                         NULL,
                         0,
                         0,
                         0,
                         0,
                         0,
                         FRAMEBUFFER_WIDTH,
                         FRAMEBUFFER_HEIGHT);
        /* Walk the closing edge from the inner arc's end back to the MOVE point, ends included. */
        for (int sample_index = 0; sample_index <= 8; sample_index++)
        {
            const float fraction = (float)sample_index / 8.0f;
            const float radius = inner_radius + (outer_radius - inner_radius) * fraction;
            const int sample_x = (int)(center_x + radius * cosf(start_angle) + 0.5f);
            const int sample_y = (int)(center_y + radius * sinf(start_angle) + 0.5f);
            if (!is_solid(px(&test_context, sample_x, sample_y), 0x66CCFFu))
                return fail("closed stroke: the closing edge of a CLOSE'd subpath was not stroked");
        }
    }

    /* --- storage pool exhaustion is flagged, not silent --- */
    {
        er_vector_reset();
        if (er_vector_slots_overflowed())
            return fail("storage pool: er_vector_reset left the overflow flag raised");

        const float tape[] = {ER_VECTOR_OP_SHAPE,
                              0,
                              ER_VECTOR_OP_MOVE,
                              0,
                              0,
                              ER_VECTOR_OP_LINE,
                              4,
                              0,
                              ER_VECTOR_OP_LINE,
                              4,
                              4,
                              ER_VECTOR_OP_CLOSE};
        const int tape_count = (int)(sizeof(tape) / sizeof(tape[0]));
        const ERVectorPaint paint = {0xFF010203u, 0, 0.0f, 0.0f, 0, 0, ER_VECTOR_FILL_NONZERO};
        const int total = er_vector_slots_total();

        for (int slot_index = 0; slot_index < total; slot_index++)
            if (er_vector_store(-1, tape, tape_count, &paint, 1, NULL, 0) < 0)
                return fail("storage pool: a slot within the pool size was refused");
        if (er_vector_slots_in_use() != total)
            return fail("storage pool: filling every slot did not show in the in-use count");
        if (er_vector_slots_overflowed())
            return fail("storage pool: a merely full pool was reported as overflowed");

        /* One node too many. It is refused (and so draws nothing) — the point of the flag is that this
         * is distinguishable from a pool that is simply at capacity and rendering fine. The first
         * refusal also prints one warning to stderr; that line below is expected output, not a fault. */
        printf("  (expect one 'ERUI_MAX_VECTOR_NODES exhausted' warning next)\n");
        if (er_vector_store(-1, tape, tape_count, &paint, 1, NULL, 0) >= 0)
            return fail("storage pool: handed out a slot past the pool size");
        if (!er_vector_slots_overflowed())
            return fail("storage pool: exhaustion was not flagged");

        /* Sticky across a later free: room reopening does not un-blank the node that was turned away. */
        er_vector_free(0);
        if (!er_vector_slots_overflowed())
            return fail("storage pool: the overflow flag was cleared by an unrelated free");

        er_vector_reset();
        if (er_vector_slots_overflowed() || er_vector_slots_in_use() != 0)
            return fail("storage pool: er_vector_reset did not clear the pool and its flag");
    }

    return EXIT_SUCCESS;
}
