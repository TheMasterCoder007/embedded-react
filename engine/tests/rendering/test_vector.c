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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FB_W 128
#define FB_H 128

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Pixel framebuffer captured during a render pass.
 */
typedef struct
{
    uint32_t* fb; /**< Flat ARGB8888 framebuffer; rows are fb_w pixels wide. */
    int fb_w;     /**< Framebuffer width in pixels. */
    int fb_h;     /**< Framebuffer height in pixels. */
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
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    TestCtx* t = ctx;
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            if (row >= 0 && col >= 0 && row < t->fb_h && col < t->fb_w)
                t->fb[row * t->fb_w + col] = argb;
}

/** @brief Backend copy_rect callback — no-op stub. */
static void copy_cb(const void* src, int s, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)s;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/**
 * @brief Backend blend_rect callback — captures gradient rows into the framebuffer (premultiplied).
 *
 * The gradient fill path flushes premultiplied ARGB rows via er_blit_blend; the real backend composites,
 * but for the test we write them straight in. Fully-covered pixels with opaque stops therefore land as the
 * exact gradient color, so the per-position color assertions hold.
 */
static void blend_cb(const void* src, int s, uint8_t a, int x, int y, int w, int h, void* ctx)
{
    (void)a;
    TestCtx* t = ctx;
    const uint32_t* px_src = src;
    const int pitch = (s > 0) ? s / (int)sizeof(uint32_t) : w;
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
        {
            const int fy = y + row, fx = x + col;
            if (fy >= 0 && fx >= 0 && fy < t->fb_h && fx < t->fb_w)
                t->fb[fy * t->fb_w + fx] = px_src[row * pitch + col];
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
 * @brief Reads a single ARGB8888 pixel from the test framebuffer.
 *
 * @param[in] t  TestCtx owning the framebuffer.
 * @param[in] x  X coordinate.
 * @param[in] y  Y coordinate.
 *
 * @return ARGB8888 pixel value at (x, y).
 */
static uint32_t px(const TestCtx* t, int x, int y)
{
    return t->fb[y * t->fb_w + x];
}

/** @brief Zeros the framebuffer. */
static void reset(TestCtx* t)
{
    memset(t->fb, 0, (size_t)t->fb_w * (size_t)t->fb_h * sizeof(uint32_t));
}

/** @brief True if the pixel is fully opaque (alpha == 0xFF) with the given RGB. */
static int is_solid(uint32_t pix, uint32_t rgb)
{
    return ((pix >> 24) == 0xFFu) && ((pix & 0x00FFFFFFu) == (rgb & 0x00FFFFFFu));
}

/** @brief Channel extractors for gradient color assertions. */
static int redOf(uint32_t p)
{
    return (int)((p >> 16) & 0xFFu);
}
static int grnOf(uint32_t p)
{
    return (int)((p >> 8) & 0xFFu);
}
static int bluOf(uint32_t p)
{
    return (int)(p & 0xFFu);
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
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    static uint32_t fb[FB_W * FB_H];
    TestCtx tc = {fb, FB_W, FB_H};
    EmbeddedRenderBackend be = {fill_cb, copy_cb, blend_cb, NULL, NULL, &tc};
    embedded_renderer_set_backend(&be);

    /* --- filled rectangle (nonzero): interior filled, outside empty --- */
    {
        reset(&tc);
        const float ops[] = {ER_VOP_SHAPE,
                             0,
                             ER_VOP_MOVE,
                             10,
                             10,
                             ER_VOP_LINE,
                             30,
                             10,
                             ER_VOP_LINE,
                             30,
                             30,
                             ER_VOP_LINE,
                             10,
                             30,
                             ER_VOP_CLOSE};
        const ERVectorPaint p = {0xFF112233u, 0, 0.0f, 0.0f, 0, 0, ER_VFILL_NONZERO};
        er_vector_render(ops, (int)(sizeof(ops) / sizeof(ops[0])), &p, 1, NULL, 0, 0, 0, 0, 0, FB_W, FB_H);
        if (!is_solid(px(&tc, 20, 20), 0x112233u))
            return fail("filled rect: interior pixel not filled");
        if (px(&tc, 5, 5) != 0)
            return fail("filled rect: pixel outside the rect was filled");
    }

    /* --- filled circle via a full VOP_ARC (nonzero) --- */
    {
        reset(&tc);
        const float ops[] = {
            ER_VOP_SHAPE, 0, ER_VOP_MOVE, 94, 64, ER_VOP_ARC, 64, 64, 30, 0.0f, 6.2831853f, 0, ER_VOP_CLOSE};
        const ERVectorPaint p = {0xFF00FF00u, 0, 0.0f, 0.0f, 0, 0, ER_VFILL_NONZERO};
        er_vector_render(ops, (int)(sizeof(ops) / sizeof(ops[0])), &p, 1, NULL, 0, 0, 0, 0, 0, FB_W, FB_H);
        if (!is_solid(px(&tc, 64, 64), 0x00FF00u))
            return fail("filled circle: center pixel not filled");
        if (px(&tc, 110, 64) != 0) /* 46px from center, well outside r=30 */
            return fail("filled circle: pixel beyond the radius was filled");
    }

    /* --- even-odd hole: outer loop + inner loop wound the same way leaves the inner region empty --- */
    {
        reset(&tc);
        const float ops[] = {ER_VOP_SHAPE, 0,  ER_VOP_MOVE, 10,          10, ER_VOP_LINE, 40,           10,
                             ER_VOP_LINE,  40, 40,          ER_VOP_LINE, 10, 40,          ER_VOP_CLOSE, ER_VOP_MOVE,
                             20,           20, ER_VOP_LINE, 30,          20, ER_VOP_LINE, 30,           30,
                             ER_VOP_LINE,  20, 30,          ER_VOP_CLOSE};
        const ERVectorPaint p = {0xFF445566u, 0, 0.0f, 0.0f, 0, 0, ER_VFILL_EVENODD};
        er_vector_render(ops, (int)(sizeof(ops) / sizeof(ops[0])), &p, 1, NULL, 0, 0, 0, 0, 0, FB_W, FB_H);
        if (!is_solid(px(&tc, 15, 25), 0x445566u))
            return fail("even-odd: ring (between outer and inner) not filled");
        if (px(&tc, 25, 25) != 0)
            return fail("even-odd: inner region should be a hole");
    }

    /* --- stroked line with round caps: the regression guard for the cap winding bug --- */
    /* Horizontal line (40,30)->(80,30), 10px wide => band y=25..35. Round caps (r=5) past each end.   */
    {
        reset(&tc);
        const float ops[] = {ER_VOP_SHAPE, 0, ER_VOP_MOVE, 40, 30, ER_VOP_LINE, 80, 30};
        const ERVectorPaint p = {0, 0xFFF4A261u, 10.0f, 4.0f, ER_VCAP_ROUND, ER_VJOIN_ROUND, ER_VFILL_NONZERO};
        er_vector_render(ops, (int)(sizeof(ops) / sizeof(ops[0])), &p, 1, NULL, 0, 0, 0, 0, 0, FB_W, FB_H);
        if (!is_solid(px(&tc, 60, 30), 0xF4A261u))
            return fail("stroke: band center not filled");
        /* (77,30): inside the band AND inside the right cap disc — if the cap winds opposite to the band
         * the two cancel to a hole here (the bug). Must be solid. */
        if (!is_solid(px(&tc, 77, 30), 0xF4A261u))
            return fail("stroke round cap: band+cap overlap is a hole (cap winding regression)");
        if (px(&tc, 92, 30) != 0) /* 12px past the end, beyond the r=5 cap */
            return fail("stroke: pixel beyond the round cap was filled");
    }

    /* --- clip box bounds the rasterize: geometry left of clipx0 emits nothing --- */
    {
        reset(&tc);
        const float ops[] = {ER_VOP_SHAPE,
                             0,
                             ER_VOP_MOVE,
                             10,
                             10,
                             ER_VOP_LINE,
                             30,
                             10,
                             ER_VOP_LINE,
                             30,
                             30,
                             ER_VOP_LINE,
                             10,
                             30,
                             ER_VOP_CLOSE};
        const ERVectorPaint p = {0xFF112233u, 0, 0.0f, 0.0f, 0, 0, ER_VFILL_NONZERO};
        er_vector_render(ops, (int)(sizeof(ops) / sizeof(ops[0])), &p, 1, NULL, 0, 0, 0, 25, 0, FB_W, FB_H);
        if (px(&tc, 20, 20) != 0)
            return fail("clip: pixel left of clipx0 was rasterized");
        if (!is_solid(px(&tc, 28, 28), 0x112233u))
            return fail("clip: pixel inside the clip box not filled");
    }

#if ERUI_GRADIENT
    /* --- linear gradient fill: red at the left edge -> blue at the right, blended across a rect --- */
    {
        reset(&tc);
        const float ops[] = {ER_VOP_SHAPE,
                             0,
                             ER_VOP_MOVE,
                             10,
                             10,
                             ER_VOP_LINE,
                             50,
                             10,
                             ER_VOP_LINE,
                             50,
                             50,
                             ER_VOP_LINE,
                             10,
                             50,
                             ER_VOP_CLOSE};
        const ERVectorPaint p = {0, 0, 0.0f, 0.0f, 0, 0, ER_VFILL_NONZERO, 1 /* fill_grad = grads[0] */};
        ERVectorGradient g;
        memset(&g, 0, sizeof(g));
        g.type = ER_GRADIENT_LINEAR;
        g.stop_count = 2;
        g.stops[0].color = 0xFFFF0000u;
        g.stops[0].position = 0.0f;
        g.stops[1].color = 0xFF0000FFu;
        g.stops[1].position = 1.0f;
        g.ax = 10.0f;
        g.ay = 10.0f;
        g.bx = 50.0f;
        g.by = 10.0f; /* horizontal axis */
        er_vector_render(ops, (int)(sizeof(ops) / sizeof(ops[0])), &p, 1, &g, 1, 0, 0, 0, 0, FB_W, FB_H);
        const uint32_t L = px(&tc, 12, 30), R = px(&tc, 48, 30), M = px(&tc, 30, 30);
        if (((L >> 24) & 0xFFu) != 0xFFu)
            return fail("linear gradient: interior pixel not opaque");
        if (!(redOf(L) > 200 && bluOf(L) < 60))
            return fail("linear gradient: left edge should be ~red");
        if (!(bluOf(R) > 200 && redOf(R) < 60))
            return fail("linear gradient: right edge should be ~blue");
        if (!(redOf(M) > 90 && redOf(M) < 170 && bluOf(M) > 90 && bluOf(M) < 170))
            return fail("linear gradient: midpoint should blend red+blue");
        if (px(&tc, 5, 5) != 0)
            return fail("linear gradient: pixel outside the rect was filled");
    }

    /* --- linear gradient STROKE: a thick horizontal line whose stroke ramps red -> blue along its length --- */
    {
        reset(&tc);
        const float ops[] = {ER_VOP_SHAPE, 0, ER_VOP_MOVE, 40, 30, ER_VOP_LINE, 80, 30};
        /* fill 0, stroke 0 (no solid colour), width 10, round caps, fill_grad 0, stroke_grad 1. */
        const ERVectorPaint p = {0, 0, 10.0f, 4.0f, ER_VCAP_ROUND, ER_VJOIN_ROUND, ER_VFILL_NONZERO, 0, 1};
        ERVectorGradient g;
        memset(&g, 0, sizeof(g));
        g.type = ER_GRADIENT_LINEAR;
        g.stop_count = 2;
        g.stops[0].color = 0xFFFF0000u;
        g.stops[0].position = 0.0f;
        g.stops[1].color = 0xFF0000FFu;
        g.stops[1].position = 1.0f;
        g.ax = 40.0f;
        g.ay = 30.0f;
        g.bx = 80.0f;
        g.by = 30.0f; /* horizontal axis along the line */
        er_vector_render(ops, (int)(sizeof(ops) / sizeof(ops[0])), &p, 1, &g, 1, 0, 0, 0, 0, FB_W, FB_H);
        const uint32_t L = px(&tc, 45, 30), R = px(&tc, 75, 30);
        if (((L >> 24) & 0xFFu) != 0xFFu)
            return fail("gradient stroke: band pixel not opaque");
        if (!(redOf(L) > 180 && bluOf(L) < 80))
            return fail("gradient stroke: left of the band should be ~red");
        if (!(bluOf(R) > 180 && redOf(R) < 80))
            return fail("gradient stroke: right of the band should be ~blue");
    }
#endif

#if ERUI_GRADIENT_RADIAL
    /* --- radial gradient fill: bright centre fading to a dark edge --- */
    {
        reset(&tc);
        const float ops[] = {ER_VOP_SHAPE,
                             0,
                             ER_VOP_MOVE,
                             10,
                             10,
                             ER_VOP_LINE,
                             50,
                             10,
                             ER_VOP_LINE,
                             50,
                             50,
                             ER_VOP_LINE,
                             10,
                             50,
                             ER_VOP_CLOSE};
        const ERVectorPaint p = {0, 0, 0.0f, 0.0f, 0, 0, ER_VFILL_NONZERO, 1};
        ERVectorGradient g;
        memset(&g, 0, sizeof(g));
        g.type = ER_GRADIENT_RADIAL;
        g.stop_count = 2;
        g.stops[0].color = 0xFFFFFFFFu;
        g.stops[0].position = 0.0f;
        g.stops[1].color = 0xFF000000u;
        g.stops[1].position = 1.0f;
        g.ax = 30.0f;
        g.ay = 30.0f;
        g.r = 20.0f;
        er_vector_render(ops, (int)(sizeof(ops) / sizeof(ops[0])), &p, 1, &g, 1, 0, 0, 0, 0, FB_W, FB_H);
        const uint32_t C = px(&tc, 30, 30), E = px(&tc, 30, 46); /* centre; 16px out (t=0.8) */
        if (!(redOf(C) > 200 && grnOf(C) > 200 && bluOf(C) > 200))
            return fail("radial gradient: centre should be ~white");
        if (!(redOf(E) < 90 && grnOf(E) < 90 && bluOf(E) < 90))
            return fail("radial gradient: outer pixel should be ~dark");
    }
#endif

    return EXIT_SUCCESS;
}
