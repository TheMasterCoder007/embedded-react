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

#define FRAMEBUFFER_WIDTH 128
#define FRAMEBUFFER_HEIGHT 128

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Pixel framebuffer and call statistics gathered during a render pass.
 */
typedef struct
{
    uint32_t* framebuffer;  /**< Flat ARGB8888 framebuffer; rows are framebuffer_width pixels wide. */
    int framebuffer_width;  /**< Framebuffer width in pixels. */
    int framebuffer_height; /**< Framebuffer height in pixels. */
    int fills;              /**< Total fill_rect calls received. */
    int out_of_bounds;      /**< fill_rect calls that fell outside the framebuffer. */
} TestCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Backend fill_rect callback that writes pixels into a TestCtx framebuffer.
 *
 * @param[in] argb    ARGB8888 fill color.
 * @param[in] x       Left edge of the fill rectangle.
 * @param[in] y       Top edge of the fill rectangle.
 * @param[in] width   Width of the fill rectangle in pixels.
 * @param[in] height  Height of the fill rectangle in pixels.
 * @param[in] ctx     Pointer to the TestCtx to update.
 */
static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    TestCtx* test_context = ctx;
    test_context->fills++;
    if (x < 0 || y < 0 || x + width > test_context->framebuffer_width || y + height > test_context->framebuffer_height)
    {
        test_context->out_of_bounds++;
        return;
    }
    for (int row = y; row < y + height; row++)
        for (int column = x; column < x + width; column++)
            test_context->framebuffer[row * test_context->framebuffer_width + column] = argb;
}

/**
 * @brief Backend copy_rect callback — no-op stub.
 *
 * @param[in] source  Source pixel buffer (unused).
 * @param[in] stride  Source stride in bytes (unused).
 * @param[in] x       Destination X coordinate (unused).
 * @param[in] y       Destination Y coordinate (unused).
 * @param[in] width   Width in pixels (unused).
 * @param[in] height  Height in pixels (unused).
 * @param[in] ctx     Opaque context (unused).
 */
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
 * @brief Backend blend_rect callback — no-op stub.
 *
 * @param[in] source  Source pixel buffer (unused).
 * @param[in] stride  Source stride in bytes (unused).
 * @param[in] alpha   Global alpha (unused).
 * @param[in] x       Destination X coordinate (unused).
 * @param[in] y       Destination Y coordinate (unused).
 * @param[in] width   Width in pixels (unused).
 * @param[in] height  Height in pixels (unused).
 * @param[in] ctx     Opaque context (unused).
 */
static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)stride;
    (void)alpha;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
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

/**
 * @brief Zeros the framebuffer and resets call statistics.
 *
 * @param[in,out] test_context  TestCtx to reset.
 */
static void reset(TestCtx* test_context)
{
    memset(test_context->framebuffer,
           0,
           (size_t)test_context->framebuffer_width * (size_t)test_context->framebuffer_height * sizeof(uint32_t));
    test_context->fills = 0;
    test_context->out_of_bounds = 0;
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
    static uint32_t framebuffer[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];
    TestCtx test_context = {framebuffer, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT, 0, 0};
    EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, &test_context};
    embedded_renderer_set_backend(&backend);

    /* --- er_rrect_clamp_radii is total: no input produces a negative radius ---
     *
     * Three call sites now share this helper to agree on one silhouette, so its contract matters more
     * than the arithmetic. The mixed path multiplies by the radius while scaling an overlapping pair
     * down, which would carry a negative straight through — and a stray negative would also flip the
     * uniform/mixed choice itself. An app can reach that: borderTopLeftRadius 10 alongside
     * borderRadius -5 resolves to (10, -5, -5, -5). */
    {
        struct
        {
            int width, height, top_left, top_right, bottom_right, bottom_left;
        } cases[] = {
            {40, 40, -5, -5, -5, -5}, /* uniform negative */
            {40, 40, 10, -5, -5, -5}, /* the reachable mixed-with-negative case */
            {40, 40, -8, 30, 30, -8}, /* negatives paired with an overlapping run */
            {40, 40, 60, 60, 60, 60}, /* uniform, far past the box */
            {40, 40, 60, 10, 60, 10}, /* mixed, far past the box */
            {0, 40, 10, 10, 10, 10},  /* degenerate width */
            {40, 0, 10, 20, 30, 40},  /* degenerate height */
            {-4, -4, 10, 20, 30, 40}, /* negative box */
            {1, 1, 10, 10, 10, 10},   /* one-pixel box */
            {40, 40, 0, 0, 0, 0},     /* already square */
        };
        for (unsigned case_index = 0; case_index < sizeof(cases) / sizeof(cases[0]); case_index++)
        {
            int top_left = cases[case_index].top_left, top_right = cases[case_index].top_right,
                bottom_right = cases[case_index].bottom_right, bottom_left = cases[case_index].bottom_left;
            er_rrect_clamp_radii(
                cases[case_index].width, cases[case_index].height, &top_left, &top_right, &bottom_right, &bottom_left);
            if (top_left < 0 || top_right < 0 || bottom_right < 0 || bottom_left < 0)
            {
                printf("  w=%d h=%d in(%d,%d,%d,%d) -> out(%d,%d,%d,%d)\n",
                       cases[case_index].width,
                       cases[case_index].height,
                       cases[case_index].top_left,
                       cases[case_index].top_right,
                       cases[case_index].bottom_right,
                       cases[case_index].bottom_left,
                       top_left,
                       top_right,
                       bottom_right,
                       bottom_left);
                return fail("clamp_radii: returned a negative radius");
            }
            /* A radius may never exceed the box either, or a row's span inverts. */
            const int longest_side = (cases[case_index].width > cases[case_index].height) ? cases[case_index].width
                                                                                          : cases[case_index].height;
            if (cases[case_index].width > 0 && cases[case_index].height > 0
                && (top_left > longest_side || top_right > longest_side || bottom_right > longest_side
                    || bottom_left > longest_side))
                return fail("clamp_radii: returned a radius larger than the box");
            /* Opposing pairs must fit along their shared edge, which is the whole point. */
            if (cases[case_index].width > 0 && cases[case_index].height > 0
                && (top_left + top_right > cases[case_index].width
                    || bottom_left + bottom_right > cases[case_index].width
                    || top_left + bottom_left > cases[case_index].height
                    || top_right + bottom_right > cases[case_index].height))
                return fail("clamp_radii: an opposing pair still overlaps");
            if (cases[case_index].width <= 0 || cases[case_index].height <= 0)
            {
                if (top_left != 0 || top_right != 0 || bottom_right != 0 || bottom_left != 0)
                    return fail("clamp_radii: a box with no area kept its corner radii");
            }
        }
    }

    /* --- transparent color: no fill_rect calls emitted --- */
    er_rrect_fill(0x00FF0000, 0, 0, 20, 20, 4);
    if (test_context.fills != 0)
        return fail("transparent fill emitted fill_rect calls");

    /* --- radius=0: fills the exact bounding box --- */
    reset(&test_context);
    er_rrect_fill(0xFF112233, 10, 10, 20, 20, 0);
    if (px(&test_context, 10, 10) != 0xFF112233)
        return fail("plain rect: top-left corner not filled");
    if (px(&test_context, 29, 29) != 0xFF112233)
        return fail("plain rect: bottom-right corner not filled");
    if (px(&test_context, 9, 10) != 0)
        return fail("plain rect: pixel left of rect incorrectly filled");
    if (px(&test_context, 10, 9) != 0)
        return fail("plain rect: pixel above rect incorrectly filled");

    /* --- rounded corners exclude bounding-box corners --- */
    /* 20x20 rect at (0,0) with r=4. Arc centres at (4,4), (15,4), (4,15), (15,15). */
    /* (0,0): distance from (4,4) = sqrt(32) > 4 → outside arc → not filled.        */
    reset(&test_context);
    er_rrect_fill(0xFF445566, 0, 0, 20, 20, 4);
    if (px(&test_context, 0, 0) != 0)
        return fail("rrect: top-left corner pixel should not be filled");
    if (px(&test_context, 19, 0) != 0)
        return fail("rrect: top-right corner pixel should not be filled");
    if (px(&test_context, 0, 19) != 0)
        return fail("rrect: bottom-left corner pixel should not be filled");
    if (px(&test_context, 19, 19) != 0)
        return fail("rrect: bottom-right corner pixel should not be filled");

    /* --- interior and span-boundary pixels are filled correctly --- */
    /* dy=4 (top row y=0): dx=0, span [4, 16). Pixel 4 filled solid.         */
    /* The middle strip starts at y=4 (r=4): x=0 filled for full-width rows.  */
    if (px(&test_context, 10, 10) != 0xFF445566)
        return fail("rrect: center pixel not filled");
    if (px(&test_context, 4, 0) != 0xFF445566)
        return fail("rrect: first pixel of top-row span not filled");
    if (px(&test_context, 15, 0) != 0xFF445566)
        return fail("rrect: last pixel of top-row span not filled");
    if (px(&test_context, 0, 4) != 0xFF445566)
        return fail("rrect: first middle-strip pixel at x=0 not filled");

    /* --- no out-of-bounds fills for a rect aligned to the FB edge --- */
    reset(&test_context);
    er_rrect_fill(0xFF778899, FRAMEBUFFER_WIDTH - 20, FRAMEBUFFER_HEIGHT - 20, 20, 20, 4);
    if (test_context.out_of_bounds != 0)
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
    reset(&test_context);
    er_rrect_fill_bordered(0xFF0000FF, 0xFFFF0000, 3, 0, 0, 30, 30, 6);
    if (px(&test_context, 0, 6) != 0xFFFF0000)
        return fail("border ring: left edge pixel should be border color");
    if (px(&test_context, 29, 6) != 0xFFFF0000)
        return fail("border ring: right edge pixel should be border color");
    if (px(&test_context, 15, 15) != 0xFF0000FF)
        return fail("border ring: center pixel should be background color");
    if (px(&test_context, 3, 6) != 0xFF0000FF)
        return fail("border ring: first inner pixel should be background color");
    if (px(&test_context, 15, 0) != 0xFFFF0000)
        return fail("border ring: top-row center pixel should be border color");
    if (px(&test_context, 2, 0) != 0)
        return fail("border ring: pixel outside outer arc should be empty");

    /* The assertions above cover the OPAQUE background path, which still fills the whole shape in
     * the border colour and paints the background back over the inset — the cheap route, and the
     * one this file has always exercised. Everything below covers the ring the non-opaque cases
     * take instead. */

    /* --- transparent background: a border must leave a RING, not a solid blob ---
     *
     * The regression: er_rrect_fill_bordered used to fill the whole shape in the border colour and
     * rely on the background fill to carve the interior back out — but er_rrect_fill returns early
     * on a zero-alpha colour, so a View with a border and no backgroundColor (React Native's
     * default) rendered as a solid block of border colour. fill_cb overwrites rather than
     * composites, so an untouched interior pixel reads as exactly 0 here. */
    reset(&test_context);
    er_rrect_fill_bordered(0x00000000, 0xFFFF0000, 4, 0, 0, 40, 40, 10);
    if (px(&test_context, 20, 20) != 0)
        return fail("transparent bg + border: the interior was painted (solid blob, not a ring)");
    if (px(&test_context, 0, 20) != 0xFFFF0000)
        return fail("transparent bg + border: the left band is not border colour");
    if (px(&test_context, 39, 20) != 0xFFFF0000)
        return fail("transparent bg + border: the right band is not border colour");
    if (px(&test_context, 20, 0) != 0xFFFF0000)
        return fail("transparent bg + border: the top band is not border colour");
    if (px(&test_context, 4, 20) != 0)
        return fail("transparent bg + border: the band is thicker than borderWidth");
    if (px(&test_context, 0, 0) != 0)
        return fail("transparent bg + border: the rounded corner was filled");
    /* The whole interior, inset clear of the band's anti-aliased inner edge, must be untouched. */
    for (int pixel_y = 7; pixel_y < 33; pixel_y++)
        for (int pixel_x = 7; pixel_x < 33; pixel_x++)
            if (px(&test_context, pixel_x, pixel_y) != 0)
                return fail("transparent bg + border: interior pixels were painted");

    /* --- the ring never paints outside the silhouette a solid fill would cover ---
     *
     * Same box, once filled solid and once stroked: every pixel the fill leaves empty must stay
     * empty in the ring too, or the border overhangs its own rounded corner. */
    {
        static uint8_t solid[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];
        reset(&test_context);
        er_rrect_fill(0xFF000000, 8, 8, 44, 36, 12);
        for (int pixel_index = 0; pixel_index < FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT; pixel_index++)
            solid[pixel_index] = (uint8_t)(test_context.framebuffer[pixel_index] != 0);

        reset(&test_context);
        er_rrect_fill_ring(0xFF000000, 8, 8, 44, 36, 12, 12, 12, 12, 5);
        if (test_context.out_of_bounds != 0)
            return fail("ring: produced out-of-bounds fills");
        for (int pixel_y = 0; pixel_y < FRAMEBUFFER_HEIGHT; pixel_y++)
            for (int pixel_x = 0; pixel_x < FRAMEBUFFER_WIDTH; pixel_x++)
                if (test_context.framebuffer[pixel_y * FRAMEBUFFER_WIDTH + pixel_x] != 0
                    && !solid[pixel_y * FRAMEBUFFER_WIDTH + pixel_x])
                    return fail("ring: painted a pixel outside the solid fill's silhouette");
        /* ...and it really is hollow. */
        if (px(&test_context, 30, 26) != 0)
            return fail("ring: the interior was painted");
    }

    /* --- per-corner radii: only the rounded corners are cut --- */
    reset(&test_context);
    er_rrect_fill_ring(0xFF00FF00, 0, 0, 40, 40, 16, 0, 16, 0, 4);
    if (px(&test_context, 0, 0) != 0)
        return fail("per-corner ring: the rounded top-left corner was filled");
    if (px(&test_context, 39, 0) != 0xFF00FF00)
        return fail("per-corner ring: the square top-right corner was cut");
    if (px(&test_context, 0, 39) != 0xFF00FF00)
        return fail("per-corner ring: the square bottom-left corner was cut");
    if (px(&test_context, 39, 39) != 0)
        return fail("per-corner ring: the rounded bottom-right corner was filled");
    if (px(&test_context, 20, 20) != 0)
        return fail("per-corner ring: the interior was painted");

    /* --- a band thick enough to swallow the interior fills the whole shape --- */
    reset(&test_context);
    er_rrect_fill_ring(0xFF123456, 0, 0, 20, 20, 6, 6, 6, 6, 40);
    if (px(&test_context, 10, 10) != 0xFF123456)
        return fail("over-thick ring: the shape was not filled solid");
    if (px(&test_context, 0, 0) != 0)
        return fail("over-thick ring: the rounded corner was filled");

    /* --- per-edge borders follow the corner radius ---
     *
     * The regression: per-edge widths/colours were drawn as four straight rects, so the border
     * squared off every corner no matter what borderRadius said. The band is now stroked through the
     * same rounded geometry, and must stay inside the silhouette a solid fill would cover. */
    {
        static uint8_t solid[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];
        reset(&test_context);
        er_rrect_fill(0xFF000000, 0, 0, 48, 40, 14);
        for (int pixel_index = 0; pixel_index < FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT; pixel_index++)
            solid[pixel_index] = (uint8_t)(test_context.framebuffer[pixel_index] != 0);

        reset(&test_context);
        {
            /* Thin sides, thick top and bottom, one colour. */
            const ERRRectBorder border = {2, 8, 2, 8, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
            er_rrect_fill_ring_edges(0, 0, 48, 40, 14, 14, 14, 14, &border);
        }
        if (test_context.out_of_bounds != 0)
            return fail("per-edge ring: produced out-of-bounds fills");
        for (int pixel_y = 0; pixel_y < FRAMEBUFFER_HEIGHT; pixel_y++)
            for (int pixel_x = 0; pixel_x < FRAMEBUFFER_WIDTH; pixel_x++)
                if (test_context.framebuffer[pixel_y * FRAMEBUFFER_WIDTH + pixel_x] != 0
                    && !solid[pixel_y * FRAMEBUFFER_WIDTH + pixel_x])
                    return fail("per-edge ring: the border escaped the rounded silhouette");
        /* Each edge is as thick as it was asked to be, measured at the box's mid-lines. */
        if (px(&test_context, 0, 20) == 0 || px(&test_context, 1, 20) == 0)
            return fail("per-edge ring: the 2px left edge is missing");
        if (px(&test_context, 2, 20) != 0)
            return fail("per-edge ring: the left edge is thicker than borderLeftWidth");
        if (px(&test_context, 24, 7) == 0)
            return fail("per-edge ring: the 8px top edge is missing");
        if (px(&test_context, 24, 8) != 0)
            return fail("per-edge ring: the top edge is thicker than borderTopWidth");
    }

    /* --- each edge keeps its own colour --- */
    reset(&test_context);
    {
        const ERRRectBorder border = {5, 5, 5, 5, 0xFFFFD54F, 0xFFE57373, 0xFF81C784, 0xFF64B5F6};
        er_rrect_fill_ring_edges(0, 0, 48, 48, 12, 12, 12, 12, &border);
    }
    if (px(&test_context, 24, 0) != 0xFFE57373)
        return fail("per-edge colours: the top edge is not its own colour");
    if (px(&test_context, 24, 47) != 0xFF64B5F6)
        return fail("per-edge colours: the bottom edge is not its own colour");
    if (px(&test_context, 0, 24) != 0xFFFFD54F)
        return fail("per-edge colours: the left edge is not its own colour");
    if (px(&test_context, 47, 24) != 0xFF81C784)
        return fail("per-edge colours: the right edge is not its own colour");

    /* --- adjacent edge colours meet on a corner diagonal ---
     *
     * The regression: the owning edge was picked per ROW — top and bottom claimed their full width —
     * so two colours met on a hard horizontal step instead of a mitre. The split now follows the line
     * the two band widths imply, which is 45 degrees when they are equal.
     */
    {
        const uint32_t TOP = 0xFFE53935, RIGHT = 0xFF43A047, BOTTOM = 0xFF1E88E5, LEFT = 0xFFFDD835;

        /* Square corners, equal widths: the mitre is the 45-degree diagonal. */
        reset(&test_context);
        {
            const ERRRectBorder border = {10, 10, 10, 10, LEFT, TOP, RIGHT, BOTTOM, 0};
            er_rrect_fill_ring_edges(0, 0, 60, 60, 0, 0, 0, 0, &border);
        }
        if (px(&test_context, 8, 2) != TOP)
            return fail("square mitre: above the diagonal is not the top colour");
        if (px(&test_context, 2, 8) != LEFT)
            return fail("square mitre: below the diagonal is not the left colour");
        if (px(&test_context, 51, 2) != TOP)
            return fail("square mitre: top-right corner is not the top colour above the diagonal");
        if (px(&test_context, 57, 8) != RIGHT)
            return fail("square mitre: top-right corner is not the right colour below the diagonal");
        if (px(&test_context, 2, 51) != LEFT)
            return fail("square mitre: bottom-left corner is not the left colour");
        if (px(&test_context, 8, 57) != BOTTOM)
            return fail("square mitre: bottom-left corner is not the bottom colour");

        /* Rounded corners, equal widths: the mitre becomes a radial line through the arc. */
        reset(&test_context);
        {
            const ERRRectBorder border = {6, 6, 6, 6, LEFT, TOP, RIGHT, BOTTOM, 0};
            er_rrect_fill_ring_edges(0, 0, 60, 60, 20, 20, 20, 20, &border);
        }
        if (px(&test_context, 18, 1) != TOP)
            return fail("rounded mitre: the top of the arc is not the top colour");
        if (px(&test_context, 1, 18) != LEFT)
            return fail("rounded mitre: the left of the arc is not the left colour");

        /* Unequal widths tilt the mitre toward the thinner edge, so a thick top owns more of the
         * corner. With top 16 and left 4 the dividing line runs (0,0) to (4,16): at row 6 it sits at
         * x = 1.5, so x = 3 is still the top's and x = 0 is the left's. */
        reset(&test_context);
        {
            const ERRRectBorder border = {4, 16, 4, 16, LEFT, TOP, RIGHT, BOTTOM, 0};
            er_rrect_fill_ring_edges(0, 0, 60, 60, 0, 0, 0, 0, &border);
        }
        if (px(&test_context, 3, 6) != TOP)
            return fail("tilted mitre: a thick top did not claim its side of the diagonal");
        if (px(&test_context, 0, 6) != LEFT)
            return fail("tilted mitre: a thin left lost its side of the diagonal");

#if ERUI_BORDER_AA
        /* The seam itself is anti-aliased. Every other edge here is, so a hard mitre staircases
         * badly at 45 degrees and reads as the two colours interlocking. Pure red against pure blue
         * makes the ramp checkable: the pixel the diagonal passes through is a half-and-half mix,
         * and it steps exactly one column per row. */
        reset(&test_context);
        {
            const uint32_t RED = 0xFFFF0000, BLUE = 0xFF0000FF;
            const ERRRectBorder border = {12, 12, 12, 12, BLUE, RED, RED, BLUE, 0};
            er_rrect_fill_ring_edges(0, 0, 48, 48, 0, 0, 0, 0, &border);
            for (int diagonal = 2; diagonal <= 8; diagonal++)
            {
                const uint32_t seam = px(&test_context, diagonal, diagonal);
                const uint32_t red = (seam >> 16) & 0xFFu;
                const uint32_t blue_channel = seam & 0xFFu;
                if (red < 100 || red > 155 || blue_channel < 100 || blue_channel > 155)
                    return fail("mitre AA: the seam pixel is not a blend of the two edge colours");
                if (px(&test_context, diagonal - 1, diagonal) != BLUE)
                    return fail("mitre AA: the pixel before the seam is not the left colour");
                if (px(&test_context, diagonal + 1, diagonal) != RED)
                    return fail("mitre AA: the pixel after the seam is not the top colour");
            }
        }
#endif
    }

    /* --- a bottom-only rule follows the bottom corners and leaves the rest alone --- */
    reset(&test_context);
    {
        const ERRRectBorder border = {0, 0, 0, 6, 0, 0, 0, 0xFF4DD0E1};
        er_rrect_fill_ring_edges(0, 0, 48, 40, 14, 14, 14, 14, &border);
    }
    if (px(&test_context, 24, 0) != 0 || px(&test_context, 0, 20) != 0)
        return fail("bottom-only rule: painted an edge that has no width");
    if (px(&test_context, 24, 39) != 0xFF4DD0E1)
        return fail("bottom-only rule: the bottom edge is missing");
    if (px(&test_context, 0, 39) != 0)
        return fail("bottom-only rule: the underline squared off the bottom-left corner");

    /* --- transparent edge colours draw nothing (the borderWidth-as-spacing trick) --- */
    reset(&test_context);
    {
        const ERRRectBorder border = {8, 8, 8, 8, 0, 0, 0, 0};
        er_rrect_fill_ring_edges(0, 0, 48, 48, 12, 12, 12, 12, &border);
    }
    if (test_context.fills != 0)
        return fail("transparent edges: emitted fills");

    /* --- dashed / dotted borders run around the corners ---
     *
     * The regression: the dash pattern was stepped along x or y over four straight rects, so it both
     * ignored the radius and stopped dead at each corner. It now steps by arc length around the
     * rounded perimeter. */
    {
        static uint8_t solid[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];
        const int box_x = 4, box_y = 4, box_width = 56, box_height = 48, radius = 16;

        reset(&test_context);
        er_rrect_fill(0xFF000000, box_x, box_y, box_width, box_height, radius);
        for (int pixel_index = 0; pixel_index < FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT; pixel_index++)
            solid[pixel_index] = (uint8_t)(test_context.framebuffer[pixel_index] != 0);

        for (uint8_t style = 1; style <= 2; style++)
        {
            reset(&test_context);
            {
                const ERRRectBorder border = {3, 3, 3, 3, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, style};
                er_rrect_fill_ring_edges(box_x, box_y, box_width, box_height, radius, radius, radius, radius, &border);
            }
            if (test_context.out_of_bounds != 0)
                return fail("dashed border: produced out-of-bounds fills");

            int painted = 0;
            int corner_on = 0;
            for (int pixel_y = 0; pixel_y < FRAMEBUFFER_HEIGHT; pixel_y++)
            {
                for (int pixel_x = 0; pixel_x < FRAMEBUFFER_WIDTH; pixel_x++)
                {
                    if (test_context.framebuffer[pixel_y * FRAMEBUFFER_WIDTH + pixel_x] == 0)
                        continue;
                    painted++;
                    /* THE regression check: a straight dash rect spills past the rounded corner. */
                    if (!solid[pixel_y * FRAMEBUFFER_WIDTH + pixel_x])
                        return fail("dashed border: a dash escaped the rounded silhouette");
                    if (pixel_x < box_x + radius && pixel_y < box_y + radius)
                        corner_on++;
                }
            }
            if (painted == 0)
                return fail("dashed border: nothing was painted");

            /* The solid ring of the same box, for the two ratios below. */
            reset(&test_context);
            {
                const ERRRectBorder border = {3, 3, 3, 3, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0};
                er_rrect_fill_ring_edges(box_x, box_y, box_width, box_height, radius, radius, radius, radius, &border);
            }
            int solid_ring = 0;
            int solid_corner = 0;
            for (int pixel_y = 0; pixel_y < FRAMEBUFFER_HEIGHT; pixel_y++)
                for (int pixel_x = 0; pixel_x < FRAMEBUFFER_WIDTH; pixel_x++)
                    if (test_context.framebuffer[pixel_y * FRAMEBUFFER_WIDTH + pixel_x] != 0)
                    {
                        solid_ring++;
                        if (pixel_x < box_x + radius && pixel_y < box_y + radius)
                            solid_corner++;
                    }
            if (painted >= solid_ring)
                return fail("dashed border: painted as much as a solid ring (no gaps)");
            /* The arc carries the pattern rather than being skipped or filled straight through: the
             * corner quarter must hold both painted and unpainted band pixels. */
            if (corner_on == 0)
                return fail("dashed border: the corner arc carries no dashes");
            if (corner_on >= solid_corner)
                return fail("dashed border: the corner arc was painted solid, not dashed");
        }
    }

    /* --- a dash pattern with no radius still steps along the straight edges --- */
    reset(&test_context);
    {
        const ERRRectBorder border = {2, 2, 2, 2, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 1};
        er_rrect_fill_ring_edges(0, 0, 60, 40, 0, 0, 0, 0, &border);
    }
    {
        int on_run = 0, gaps = 0;
        for (int pixel_x = 0; pixel_x < 60; pixel_x++)
        {
            if (px(&test_context, pixel_x, 0) != 0)
            {
                on_run++;
            }
            else if (on_run > 0)
            {
                gaps++;
                on_run = 0;
            }
        }
        if (gaps == 0)
            return fail("square dashed border: the top edge has no gaps");
    }

    /* --- degenerate inputs draw nothing rather than misbehaving --- */
    reset(&test_context);
    er_rrect_fill_ring(0x00FF0000, 0, 0, 30, 30, 8, 8, 8, 8, 3); /* transparent colour */
    er_rrect_fill_ring(0xFFFF0000, 0, 0, 30, 30, 8, 8, 8, 8, 0); /* zero-width band */
    er_rrect_fill_ring(0xFFFF0000, 0, 0, 0, 30, 8, 8, 8, 8, 3);  /* empty box */
    if (test_context.fills != 0)
        return fail("degenerate ring: emitted fills");

    /* --- large radius clamps to pill shape without crashing --- */
    reset(&test_context);
    er_rrect_fill(0xFFAABBCC, 0, 0, 30, 20, 999);
    if (px(&test_context, 15, 10) != 0xFFAABBCC)
        return fail("large-radius pill: center pixel not filled");
    if (test_context.out_of_bounds != 0)
        return fail("large-radius pill: produced out-of-bounds fills");

#if ERUI_BORDER_AA
    /* --- anti-aliased edge pixel carries partial alpha --- */
    /*                                                                         */
    /* 20x20 rect at (5,5) with r=4, fully opaque black (0xFF000000).        */
    /* dy=1 → row y=8. dx_f=sqrt(15)≈3.873, dx=3. AA left pixel: ax_l=5.    */
    /* SDF: cx=3.5, cy=0.5, dist=sqrt(12.5)≈3.536, cov=4.5-3.536=0.964.     */
    /* alpha = (uint8_t)(0.964*255+0.5) = 246.                                */
    /* Accept [200, 254] to allow for minor floating-point platform variation. */
    reset(&test_context);
    er_rrect_fill(0xFF000000, 5, 5, 20, 20, 4);
    {
        uint32_t antialiased_pixel = px(&test_context, 5, 8);
        uint8_t antialiased_alpha = (uint8_t)(antialiased_pixel >> 24);
        if (antialiased_alpha == 0 || antialiased_alpha == 0xFF)
            return fail("AA edge pixel should have partial alpha (not 0 or 255)");
        if (antialiased_alpha < 200 || antialiased_alpha > 254)
            return fail("AA edge pixel alpha outside expected range [200, 254]");
    }

    /* --- the fringe is a RAMP, and it ends --- */
    /*                                                                          */
    /* The check above only ever reads the first fringe pixel, so it holds even  */
    /* if the walk stops after one step. Pin the whole ramp instead.             */
    /*                                                                          */
    /* 60x60 at r=20, row y=0: dy=r, so dx=0 and the solid span is [20,40).      */
    /* The fringe runs six pixels outward on each side with strictly falling     */
    /* coverage (0.994 .. 0.239), and the seventh sample goes negative — so      */
    /* x=13 and x=46 must stay untouched.                                        */
    reset(&test_context);
    er_rrect_fill(0xFF000000, 0, 0, 60, 60, 20);
    {
        uint8_t previous_alpha = 255U;
        for (int step = 0; step < 6; step++)
        {
            const uint8_t left_alpha = (uint8_t)(px(&test_context, 19 - step, 0) >> 24);
            const uint8_t right_alpha = (uint8_t)(px(&test_context, 40 + step, 0) >> 24);
            if (left_alpha == 0U)
                return fail("AA fringe: the ramp stopped short on the left of the arc");
            if (right_alpha == 0U)
                return fail("AA fringe: the ramp stopped short on the right of the arc");
            if (left_alpha != right_alpha)
                return fail("AA fringe: the two sides of the arc are not mirrored");
            if (left_alpha >= previous_alpha)
                return fail("AA fringe: coverage did not fall as the walk stepped outward");
            previous_alpha = left_alpha;
        }
        if (px(&test_context, 13, 0) != 0U || px(&test_context, 46, 0) != 0U)
            return fail("AA fringe: the walk painted past the end of the arc");
        if (test_context.out_of_bounds != 0)
            return fail("AA fringe: produced out-of-bounds fills");
    }
#endif

    return EXIT_SUCCESS;
}
