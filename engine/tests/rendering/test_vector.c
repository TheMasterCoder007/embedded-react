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

/** @brief Backend blend_rect callback — no-op stub. */
static void blend_cb(const void* src, int s, uint8_t a, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)s;
    (void)a;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
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
        er_vector_render(ops, (int)(sizeof(ops) / sizeof(ops[0])), &p, 1, 0, 0, 0, 0, FB_W, FB_H);
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
        er_vector_render(ops, (int)(sizeof(ops) / sizeof(ops[0])), &p, 1, 0, 0, 0, 0, FB_W, FB_H);
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
        er_vector_render(ops, (int)(sizeof(ops) / sizeof(ops[0])), &p, 1, 0, 0, 0, 0, FB_W, FB_H);
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
        er_vector_render(ops, (int)(sizeof(ops) / sizeof(ops[0])), &p, 1, 0, 0, 0, 0, FB_W, FB_H);
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
        er_vector_render(ops, (int)(sizeof(ops) / sizeof(ops[0])), &p, 1, 0, 0, 25, 0, FB_W, FB_H);
        if (px(&tc, 20, 20) != 0)
            return fail("clip: pixel left of clipx0 was rasterized");
        if (!is_solid(px(&tc, 28, 28), 0x112233u))
            return fail("clip: pixel inside the clip box not filled");
    }

    return EXIT_SUCCESS;
}
