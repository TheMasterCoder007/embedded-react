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
#include "rrect.h"
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
 * @brief Pixel framebuffer and call statistics gathered during a render pass.
 */
typedef struct
{
    uint32_t* fb;      /**< Flat ARGB8888 framebuffer; rows are fb_w pixels wide. */
    int fb_w;          /**< Framebuffer width in pixels. */
    int fb_h;          /**< Framebuffer height in pixels. */
    int fills;         /**< Total fill_rect calls received. */
    int out_of_bounds; /**< fill_rect calls that fell outside the framebuffer. */
} TestCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Backend fill_rect callback that writes pixels into a TestCtx framebuffer.
 *
 * @param[in] argb  ARGB8888 fill color.
 * @param[in] x     Left edge of the fill rectangle.
 * @param[in] y     Top edge of the fill rectangle.
 * @param[in] w     Width of the fill rectangle in pixels.
 * @param[in] h     Height of the fill rectangle in pixels.
 * @param[in] ctx   Pointer to the TestCtx to update.
 */
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    TestCtx* t = ctx;
    t->fills++;
    if (x < 0 || y < 0 || x + w > t->fb_w || y + h > t->fb_h)
    {
        t->out_of_bounds++;
        return;
    }
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            t->fb[row * t->fb_w + col] = argb;
}

/**
 * @brief Backend copy_rect callback — no-op stub.
 *
 * @param[in] src  Source pixel buffer (unused).
 * @param[in] s    Source stride in bytes (unused).
 * @param[in] x    Destination X coordinate (unused).
 * @param[in] y    Destination Y coordinate (unused).
 * @param[in] w    Width in pixels (unused).
 * @param[in] h    Height in pixels (unused).
 * @param[in] ctx  Opaque context (unused).
 */
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
 * @brief Backend blend_rect callback — no-op stub.
 *
 * @param[in] src  Source pixel buffer (unused).
 * @param[in] s    Source stride in bytes (unused).
 * @param[in] a    Global alpha (unused).
 * @param[in] x    Destination X coordinate (unused).
 * @param[in] y    Destination Y coordinate (unused).
 * @param[in] w    Width in pixels (unused).
 * @param[in] h    Height in pixels (unused).
 * @param[in] ctx  Opaque context (unused).
 */
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

/**
 * @brief Zeros the framebuffer and resets call statistics.
 *
 * @param[in,out] t  TestCtx to reset.
 */
static void reset(TestCtx* t)
{
    memset(t->fb, 0, (size_t)t->fb_w * (size_t)t->fb_h * sizeof(uint32_t));
    t->fills = 0;
    t->out_of_bounds = 0;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Test entry point — exercises er_rrect_fill() and er_rrect_fill_bordered().
 *
 * Verifies that:
 *   - A fully transparent color emits no fill_rect calls.
 *   - radius=0 fills the exact bounding box without overshooting.
 *   - Corner pixels are excluded from the filled area when radius > 0.
 *   - The first and last pixels of the top-row arc span are filled correctly.
 *   - No fill_rect call escapes the framebuffer bounds for a rect near the edge.
 *   - er_rrect_fill_bordered paints border and background pixels in the correct zones.
 *   - A very large radius clamps to a pill shape without crashing.
 *   - Anti-aliased edge pixels carry partial alpha within the expected range (ERUI_BORDER_AA).
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    static uint32_t fb[FB_W * FB_H];
    TestCtx tc = {fb, FB_W, FB_H, 0, 0};
    EmbeddedRenderBackend be = {fill_cb, copy_cb, blend_cb, NULL, NULL, &tc};
    embedded_renderer_set_backend(&be);

    /* --- transparent color: no fill_rect calls emitted --- */
    er_rrect_fill(0x00FF0000, 0, 0, 20, 20, 4);
    if (tc.fills != 0)
        return fail("transparent fill emitted fill_rect calls");

    /* --- radius=0: fills the exact bounding box --- */
    reset(&tc);
    er_rrect_fill(0xFF112233, 10, 10, 20, 20, 0);
    if (px(&tc, 10, 10) != 0xFF112233)
        return fail("plain rect: top-left corner not filled");
    if (px(&tc, 29, 29) != 0xFF112233)
        return fail("plain rect: bottom-right corner not filled");
    if (px(&tc, 9, 10) != 0)
        return fail("plain rect: pixel left of rect incorrectly filled");
    if (px(&tc, 10, 9) != 0)
        return fail("plain rect: pixel above rect incorrectly filled");

    /* --- rounded corners exclude bounding-box corners --- */
    /* 20x20 rect at (0,0) with r=4. Arc centres at (4,4), (15,4), (4,15), (15,15). */
    /* (0,0): distance from (4,4) = sqrt(32) > 4 → outside arc → not filled.        */
    reset(&tc);
    er_rrect_fill(0xFF445566, 0, 0, 20, 20, 4);
    if (px(&tc, 0, 0) != 0)
        return fail("rrect: top-left corner pixel should not be filled");
    if (px(&tc, 19, 0) != 0)
        return fail("rrect: top-right corner pixel should not be filled");
    if (px(&tc, 0, 19) != 0)
        return fail("rrect: bottom-left corner pixel should not be filled");
    if (px(&tc, 19, 19) != 0)
        return fail("rrect: bottom-right corner pixel should not be filled");

    /* --- interior and span-boundary pixels are filled correctly --- */
    /* dy=4 (top row y=0): dx=0, span [4, 16). Pixel 4 filled solid.         */
    /* The middle strip starts at y=4 (r=4): x=0 filled for full-width rows.  */
    if (px(&tc, 10, 10) != 0xFF445566)
        return fail("rrect: center pixel not filled");
    if (px(&tc, 4, 0) != 0xFF445566)
        return fail("rrect: first pixel of top-row span not filled");
    if (px(&tc, 15, 0) != 0xFF445566)
        return fail("rrect: last pixel of top-row span not filled");
    if (px(&tc, 0, 4) != 0xFF445566)
        return fail("rrect: first middle-strip pixel at x=0 not filled");

    /* --- no out-of-bounds fills for a rect aligned to the FB edge --- */
    reset(&tc);
    er_rrect_fill(0xFF778899, FB_W - 20, FB_H - 20, 20, 20, 4);
    if (tc.out_of_bounds != 0)
        return fail("rrect near FB edge produced out-of-bounds fills");

    /* --- border ring: correct colors in outer ring and inner background --- */
    /*                                                                         */
    /* er_rrect_fill_bordered(bg=blue, border=red, bw=3, 0,0, 30,30, r=6).   */
    /* Outer (red) first; inner rect at (3,3,24,24,ir=3) overpainted blue.   */
    /*                                                                         */
    /* The middle strip starts at y=6 for both outer (r=6) and inner (r=3+y=3).  */
    /* At y=6: outer fills x=0..29 red; inner fills x=3..26 blue.            */
    /* px(0,6) and px(29,6): not in inner rect → remain red (border).        */
    /* px(3,6) and px(15,15): inside inner rect → blue (background).         */
    /*                                                                         */
    /* Top row y=0: outer dy=6, dx=0, span [6,24). Inner doesn't reach y=0.  */
    /* px(15,0): in outer span → red. px(2,0): outside AA band → empty.       */
    reset(&tc);
    er_rrect_fill_bordered(0xFF0000FF, 0xFFFF0000, 3, 0, 0, 30, 30, 6);
    if (px(&tc, 0, 6) != 0xFFFF0000)
        return fail("border ring: left edge pixel should be border color");
    if (px(&tc, 29, 6) != 0xFFFF0000)
        return fail("border ring: right edge pixel should be border color");
    if (px(&tc, 15, 15) != 0xFF0000FF)
        return fail("border ring: center pixel should be background color");
    if (px(&tc, 3, 6) != 0xFF0000FF)
        return fail("border ring: first inner pixel should be background color");
    if (px(&tc, 15, 0) != 0xFFFF0000)
        return fail("border ring: top-row center pixel should be border color");
    if (px(&tc, 2, 0) != 0)
        return fail("border ring: pixel outside outer arc should be empty");

    /* --- large radius clamps to pill shape without crashing --- */
    reset(&tc);
    er_rrect_fill(0xFFAABBCC, 0, 0, 30, 20, 999);
    if (px(&tc, 15, 10) != 0xFFAABBCC)
        return fail("large-radius pill: center pixel not filled");
    if (tc.out_of_bounds != 0)
        return fail("large-radius pill: produced out-of-bounds fills");

#if ERUI_BORDER_AA
    /* --- anti-aliased edge pixel carries partial alpha --- */
    /*                                                                         */
    /* 20x20 rect at (5,5) with r=4, fully opaque black (0xFF000000).        */
    /* dy=1 → row y=8. dx_f=sqrt(15)≈3.873, dx=3. AA left pixel: ax_l=5.    */
    /* SDF: cx=3.5, cy=0.5, dist=sqrt(12.5)≈3.536, cov=4.5-3.536=0.964.     */
    /* alpha = (uint8_t)(0.964*255+0.5) = 246.                                */
    /* Accept [200, 254] to allow for minor floating-point platform variation. */
    reset(&tc);
    er_rrect_fill(0xFF000000, 5, 5, 20, 20, 4);
    {
        uint32_t aa_pixel = px(&tc, 5, 8);
        uint8_t aa_alpha = (uint8_t)(aa_pixel >> 24);
        if (aa_alpha == 0 || aa_alpha == 0xFF)
            return fail("AA edge pixel should have partial alpha (not 0 or 255)");
        if (aa_alpha < 200 || aa_alpha > 254)
            return fail("AA edge pixel alpha outside expected range [200, 254]");
    }
#endif

    return EXIT_SUCCESS;
}
