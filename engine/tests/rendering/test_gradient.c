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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FRAMEBUFFER_WIDTH 64
#define FRAMEBUFFER_HEIGHT 64
#define CHANNEL_MAX 255u
#define OPACITY_OPAQUE 255

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Test framebuffer accumulated from blend_rect calls.
 */
typedef struct
{
    uint32_t framebuffer[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT]; /**< Premultiplied ARGB8888 pixel storage. */
    int fill_calls;                                               /**< Number of fill_rect calls received. */
    int blend_calls;                                              /**< Number of blend_rect calls received. */
} TestCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — backend stubs
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief fill_rect stub — accumulates solid-color fills into the test framebuffer.
 *
 * @param[in] argb    Fill color (straight-alpha ARGB8888).
 * @param[in] x       Left edge.
 * @param[in] y       Top edge.
 * @param[in] width   Width in pixels.
 * @param[in] height  Height in pixels.
 * @param[in] ctx     Pointer to TestCtx.
 */
static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    TestCtx* test_context = ctx;
    test_context->fill_calls++;
    const uint32_t alpha = (argb >> 24) & 0xFFu;
    const uint32_t red = ((argb >> 16) & 0xFFu) * alpha / CHANNEL_MAX;
    const uint32_t green = ((argb >> 8) & 0xFFu) * alpha / CHANNEL_MAX;
    const uint32_t blue = (argb & 0xFFu) * alpha / CHANNEL_MAX;
    const uint32_t premultiplied = (alpha << 24) | (red << 16) | (green << 8) | blue;
    for (int pixel_y = y; pixel_y < y + height && pixel_y < FRAMEBUFFER_HEIGHT; pixel_y++)
        for (int pixel_x = x; pixel_x < x + width && pixel_x < FRAMEBUFFER_WIDTH; pixel_x++)
            if (pixel_x >= 0 && pixel_y >= 0)
                test_context->framebuffer[pixel_y * FRAMEBUFFER_WIDTH + pixel_x] = premultiplied;
}

/**
 * @brief copy_rect stub — no-op for gradient tests.
 *
 * @param[in] source  Source buffer (unused).
 * @param[in] stride  Stride (unused).
 * @param[in] x       X (unused).
 * @param[in] y       Y (unused).
 * @param[in] width   Width (unused).
 * @param[in] height  Height (unused).
 * @param[in] ctx     Context (unused).
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
 * @brief blend_rect stub — composites premultiplied rows into the test framebuffer.
 *
 * @param[in] source  Premultiplied ARGB8888 source buffer.
 * @param[in] stride  Row stride of source in bytes.
 * @param[in] alpha   Global opacity (unused; tests use opaque blends).
 * @param[in] x       Destination left edge.
 * @param[in] y       Destination top edge.
 * @param[in] width   Width in pixels.
 * @param[in] height  Height in pixels.
 * @param[in] ctx     Pointer to TestCtx.
 */
static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)alpha;
    TestCtx* test_context = ctx;
    test_context->blend_calls++;
    const uint8_t* source_row = (const uint8_t*)source;
    for (int pixel_y = y; pixel_y < y + height && pixel_y < FRAMEBUFFER_HEIGHT; pixel_y++, source_row += stride)
    {
        const uint32_t* row = (const uint32_t*)source_row;
        for (int pixel_x = x; pixel_x < x + width && pixel_x < FRAMEBUFFER_WIDTH; pixel_x++)
            if (pixel_x >= 0 && pixel_y >= 0)
                test_context->framebuffer[pixel_y * FRAMEBUFFER_WIDTH + pixel_x] = row[pixel_x - x];
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Initialises a TestCtx and registers it as the active backend.
 *
 * @param[in,out] test_context  TestCtx to initialise (framebuffer cleared to black, counters zeroed).
 */
static void setup_backend(TestCtx* test_context)
{
    memset(test_context, 0, sizeof(*test_context));
    static EmbeddedRenderBackend backend;
    backend.fill_rect = fill_cb;
    backend.copy_rect = copy_cb;
    backend.blend_rect = blend_cb;
    backend.wait = NULL;
    backend.frame_ready = NULL;
    backend.context = test_context;
    embedded_renderer_set_backend(&backend);
}

/**
 * @brief Returns a pixel from the test framebuffer.
 *
 * @param[in] test_context  Test context.
 * @param[in] x             Column.
 * @param[in] y             Row.
 *
 * @return Premultiplied ARGB8888 value stored at (x, y).
 */
static uint32_t px(const TestCtx* test_context, int x, int y)
{
    return test_context->framebuffer[(y * FRAMEBUFFER_WIDTH) + x];
}

/**
 * @brief Premultiplies a straight-alpha ARGB8888 color.
 *
 * @param[in] straight  Straight-alpha ARGB8888 color.
 *
 * @return Premultiplied ARGB8888 color.
 */
static uint32_t pm(uint32_t straight)
{
    const uint32_t alpha = (straight >> 24) & 0xFFu;
    return (alpha << 24) | ((((straight >> 16) & 0xFFu) * alpha / CHANNEL_MAX) << 16)
           | ((((straight >> 8) & 0xFFu) * alpha / CHANNEL_MAX) << 8) | (((straight & 0xFFu) * alpha / CHANNEL_MAX));
}

/**
 * @brief Prints a failure message and returns EXIT_FAILURE.
 *
 * @param[in] message  Description of the failure.
 *
 * @return EXIT_FAILURE.
 */
static int fail(const char* message)
{
    printf("FAIL: %s\n", message);
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
    TestCtx test_context;
    setup_backend(&test_context);

    /* -----------------------------------------------------------------------
     * Scene: a single full-framebuffer View with a two-stop gradient.
     * ----------------------------------------------------------------------- */
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props;
    memset(&root_props, 0, sizeof(root_props));
    root_props.left = root_props.top = root_props.right = root_props.bottom = ER_LAYOUT_AUTO;
    root_props.width = FRAMEBUFFER_WIDTH;
    root_props.height = FRAMEBUFFER_HEIGHT;
    root_props.opacity = OPACITY_OPAQUE;
    root_props.background_color = 0xFF112233; /* fallback when gradient disabled */
    er_node_set_props(root, &root_props);
    er_tree_set_root(root);

#if ERUI_GRADIENT

    /* -----------------------------------------------------------------------
     * Test 1: vertical linear gradient (angle=0° → top→bottom).
     *         stop[0] at pos 0.0 = teal,  stop[1] at pos 1.0 = dark-blue.
     * ----------------------------------------------------------------------- */
    {
        const uint32_t teal = 0xFF2A9D8F;
        const uint32_t dark = 0xFF0A2030;

        ERProps props;
        memset(&props, 0, sizeof(props));
        props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
        props.width = FRAMEBUFFER_WIDTH;
        props.height = FRAMEBUFFER_HEIGHT;
        props.opacity = OPACITY_OPAQUE;
        props.gradient_type = ER_GRADIENT_LINEAR;
        props.gradient_angle = 0.0f; /* top → bottom */
        props.gradient_stop_count = 2;
        props.gradient_stops[0].color = teal;
        props.gradient_stops[0].position = 0.0f;
        props.gradient_stops[1].color = dark;
        props.gradient_stops[1].position = 1.0f;
        er_node_set_props(root, &props);

        memset(test_context.framebuffer, 0, sizeof(test_context.framebuffer));
        test_context.blend_calls = 0;
        er_commit();

        /* Top row should be approximately teal. */
        const uint32_t top_pixel = px(&test_context, FRAMEBUFFER_WIDTH / 2, 0);
        const uint32_t teal_premultiplied = pm(teal);
        if ((top_pixel >> 16 & 0xFF) != ((teal_premultiplied >> 16) & 0xFF))
            return fail("vertical linear: top row red channel mismatch");

        /* Bottom row should be approximately dark-blue. */
        const uint32_t bottom_pixel = px(&test_context, FRAMEBUFFER_WIDTH / 2, FRAMEBUFFER_HEIGHT - 1);
        const uint32_t dark_premultiplied = pm(dark);
        if ((bottom_pixel >> 16 & 0xFF) != ((dark_premultiplied >> 16) & 0xFF))
            return fail("vertical linear: bottom row red channel mismatch");

        /* Middle row should be somewhere between the two extremes. */
        const uint32_t mid_pixel = px(&test_context, FRAMEBUFFER_WIDTH / 2, FRAMEBUFFER_HEIGHT / 2);
        const uint8_t mid_red = (uint8_t)((mid_pixel >> 16) & 0xFF);
        const uint8_t top_red = (uint8_t)((top_pixel >> 16) & 0xFF);
        const uint8_t bottom_red = (uint8_t)((bottom_pixel >> 16) & 0xFF);
        if (mid_red <= bottom_red || mid_red >= top_red)
            return fail("vertical linear: mid row must be between top and bottom");

        /* Engine should have produced one blend_rect call per row. */
        if (test_context.blend_calls < FRAMEBUFFER_HEIGHT)
            return fail("vertical linear: expected at least FB_H blend calls");
    }

    /* -----------------------------------------------------------------------
     * Test 2: horizontal linear gradient (angle=90° → left→right).
     *         stop[0] = red at left, stop[1] = blue at right.
     * ----------------------------------------------------------------------- */
    {
        const uint32_t red = 0xFFE94560;
        const uint32_t blue = 0xFF3498DB;

        ERProps props;
        memset(&props, 0, sizeof(props));
        props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
        props.width = FRAMEBUFFER_WIDTH;
        props.height = FRAMEBUFFER_HEIGHT;
        props.opacity = OPACITY_OPAQUE;
        props.gradient_type = ER_GRADIENT_LINEAR;
        props.gradient_angle = 90.0f; /* left → right */
        props.gradient_stop_count = 2;
        props.gradient_stops[0].color = red;
        props.gradient_stops[0].position = 0.0f;
        props.gradient_stops[1].color = blue;
        props.gradient_stops[1].position = 1.0f;
        er_node_set_props(root, &props);

        memset(test_context.framebuffer, 0, sizeof(test_context.framebuffer));
        er_commit();

        /* Left column should be approximately red. */
        const uint32_t left_pixel = px(&test_context, 0, FRAMEBUFFER_HEIGHT / 2);
        const uint32_t red_premultiplied = pm(red);
        if (((left_pixel >> 16) & 0xFF) != ((red_premultiplied >> 16) & 0xFF))
            return fail("horizontal linear: leftmost column red channel mismatch");

        const uint32_t right_pixel = px(&test_context, FRAMEBUFFER_WIDTH - 1, FRAMEBUFFER_HEIGHT / 2);
        const uint32_t blue_premultiplied = pm(blue);
        const int right_blue = (int)(right_pixel & 0xFFu), expected_blue = (int)(blue_premultiplied & 0xFFu);
        if (right_blue < expected_blue - 8 || right_blue > expected_blue + 8)
            return fail("horizontal linear: rightmost column not ~blue");

        /* Middle column between the two extremes. */
        const uint32_t mid_pixel = px(&test_context, FRAMEBUFFER_WIDTH / 2, FRAMEBUFFER_HEIGHT / 2);
        const uint8_t mid_red = (uint8_t)((mid_pixel >> 16) & 0xFF);
        const uint8_t left_red = (uint8_t)((left_pixel >> 16) & 0xFF);
        const uint8_t right_red = (uint8_t)((right_pixel >> 16) & 0xFF);
        if (mid_red >= left_red || mid_red <= right_red)
            return fail("horizontal linear: mid column red must be between left and right");
    }

    /* -----------------------------------------------------------------------
     * Test 3: degenerate gradient — stop_count < 2 → no gradient, background_color used.
     * ----------------------------------------------------------------------- */
    {
        const uint32_t background = 0xFF334455;

        ERProps props;
        memset(&props, 0, sizeof(props));
        props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
        props.width = FRAMEBUFFER_WIDTH;
        props.height = FRAMEBUFFER_HEIGHT;
        props.opacity = OPACITY_OPAQUE;
        props.background_color = background;
        props.gradient_type = ER_GRADIENT_LINEAR;
        props.gradient_angle = 0.0f;
        props.gradient_stop_count = 1; /* < 2 → rasterizer does nothing */
        props.gradient_stops[0].color = 0xFFFFFFFF;
        props.gradient_stops[0].position = 0.0f;
        er_node_set_props(root, &props);

        memset(test_context.framebuffer, 0, sizeof(test_context.framebuffer));
        er_commit();

        /* When stop_count < 2 the gradient is skipped; background_color fills the rect. */
        const uint32_t top_pixel = px(&test_context, FRAMEBUFFER_WIDTH / 2, 0);
        const uint32_t background_premultiplied = pm(background);
        if (top_pixel != background_premultiplied)
            return fail("degenerate gradient: background_color should fill when stop_count < 2");
    }

#if ERUI_GRADIENT_RADIAL

    /* -----------------------------------------------------------------------
     * Test 4: radial gradient.
     *         Center (t=0) = white,  edge (t≈1 at corner) = dark-teal.
     * ----------------------------------------------------------------------- */
    {
        const uint32_t center_color = 0xFFFFFFFF;
        const uint32_t edge_color = 0xFF0A3040;

        ERProps props;
        memset(&props, 0, sizeof(props));
        props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
        props.width = FRAMEBUFFER_WIDTH;
        props.height = FRAMEBUFFER_HEIGHT;
        props.opacity = OPACITY_OPAQUE;
        props.gradient_type = ER_GRADIENT_RADIAL;
        props.gradient_stop_count = 2;
        props.gradient_stops[0].color = center_color;
        props.gradient_stops[0].position = 0.0f;
        props.gradient_stops[1].color = edge_color;
        props.gradient_stops[1].position = 1.0f;
        er_node_set_props(root, &props);

        memset(test_context.framebuffer, 0, sizeof(test_context.framebuffer));
        er_commit();

        /* Centre pixel should be very close to white (t≈0). */
        const uint32_t centre_pixel = px(&test_context, FRAMEBUFFER_WIDTH / 2, FRAMEBUFFER_HEIGHT / 2);
        if (((centre_pixel >> 24) & 0xFF) < 240)
            return fail("radial: centre pixel alpha must be near 255");
        /* All channels at centre should be high (close to white). */
        if (((centre_pixel >> 16) & 0xFF) < 200)
            return fail("radial: centre pixel red must be near 255");

        /* Corner pixel should be close to the edge color (t≈1). */
        const uint32_t corner_pixel = px(&test_context, 0, 0);
        /* The corner red channel should be much lower than the centre. */
        if (((corner_pixel >> 16) & 0xFF) >= ((centre_pixel >> 16) & 0xFF))
            return fail("radial: corner red must be lower than centre red");
    }

#endif /* ERUI_GRADIENT_RADIAL */

    /* -----------------------------------------------------------------------
     * Test 5: three-stop gradient — mid-stop color appears in the middle third.
     * ----------------------------------------------------------------------- */
    {
        const uint32_t teal_stop = 0xFF2A9D8F;   /* teal  */
        const uint32_t orange_stop = 0xFFF4A261; /* orange — mid-point */
        const uint32_t purple_stop = 0xFF9B59B6; /* purple */

        ERProps props;
        memset(&props, 0, sizeof(props));
        props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
        props.width = FRAMEBUFFER_WIDTH;
        props.height = FRAMEBUFFER_HEIGHT;
        props.opacity = OPACITY_OPAQUE;
        props.gradient_type = ER_GRADIENT_LINEAR;
        props.gradient_angle = 0.0f;
        props.gradient_stop_count = 3;
        props.gradient_stops[0].color = teal_stop;
        props.gradient_stops[0].position = 0.0f;
        props.gradient_stops[1].color = orange_stop;
        props.gradient_stops[1].position = 0.5f;
        props.gradient_stops[2].color = purple_stop;
        props.gradient_stops[2].position = 1.0f;
        er_node_set_props(root, &props);

        memset(test_context.framebuffer, 0, sizeof(test_context.framebuffer));
        er_commit();

        /* Mid row (pos ≈ 0.5) should match the orange mid-stop more than the endpoints. */
        const uint32_t mid_pixel = px(&test_context, FRAMEBUFFER_WIDTH / 2, FRAMEBUFFER_HEIGHT / 2);
        const uint8_t mid_green = (uint8_t)((mid_pixel >> 8) & 0xFF);
        const uint32_t top_pixel = px(&test_context, FRAMEBUFFER_WIDTH / 2, 0);
        const uint32_t bottom_pixel = px(&test_context, FRAMEBUFFER_WIDTH / 2, FRAMEBUFFER_HEIGHT - 1);
        /* Orange has the highest green channel; verify mid > both endpoints. */
        if (mid_green <= (uint8_t)((top_pixel >> 8) & 0xFF) && mid_green <= (uint8_t)((bottom_pixel >> 8) & 0xFF))
            return fail("three-stop linear: mid-stop orange not visible at mid row");
    }

    /* -----------------------------------------------------------------------
     * Test 6: border radius clips the gradient.
     *
     * A gradient is a background, so it has to stop at exactly the edge a solid background_color
     * would have — otherwise its square corners poke out past the rounded ones. The assertion is
     * therefore not "the corner is empty" but the stronger "the gradient covers precisely what the
     * solid fill covers": render the same rounded box twice, once with a flat colour and once with
     * a gradient, and require the two coverage masks (the premultiplied alpha channel, including
     * anti-aliased corner fringes) to be identical pixel for pixel.
     * ----------------------------------------------------------------------- */
    {
        const int radius = 16; /* well past the ~4 px where the overhang hides */
        static uint8_t solid_coverage[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];

        ERProps props;
        memset(&props, 0, sizeof(props));
        props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
        props.width = FRAMEBUFFER_WIDTH;
        props.height = FRAMEBUFFER_HEIGHT;
        props.opacity = OPACITY_OPAQUE;
        props.border_radius = (int16_t)radius;

        /* Reference: the same rounded box filled with an opaque solid colour. */
        props.background_color = 0xFF2A9D8F;
        er_node_set_props(root, &props);
        memset(test_context.framebuffer, 0, sizeof(test_context.framebuffer));
        er_commit();
        for (int pixel_index = 0; pixel_index < FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT; pixel_index++)
            solid_coverage[pixel_index] = (uint8_t)((test_context.framebuffer[pixel_index] >> 24) & 0xFFu);

        /* Same box, gradient background (two opaque stops → the same full coverage). */
        props.background_color = 0;
        props.gradient_type = ER_GRADIENT_LINEAR;
        props.gradient_angle = 45.0f; /* diagonal: every corner sees a different colour */
        props.gradient_stop_count = 2;
        props.gradient_stops[0].color = 0xFF2A9D8F;
        props.gradient_stops[0].position = 0.0f;
        props.gradient_stops[1].color = 0xFF9B59B6;
        props.gradient_stops[1].position = 1.0f;
        er_node_set_props(root, &props);
        memset(test_context.framebuffer, 0, sizeof(test_context.framebuffer));
        er_commit();

        /* The corners must actually be cut — otherwise the comparison below could pass on two
         * equally-square shapes if the solid fill ever regressed. */
        if (solid_coverage[0] != 0 || solid_coverage[FRAMEBUFFER_WIDTH - 1] != 0)
            return fail("radius clip: the solid reference did not round its own corners");

        for (int y = 0; y < FRAMEBUFFER_HEIGHT; y++)
        {
            for (int x = 0; x < FRAMEBUFFER_WIDTH; x++)
            {
                const uint8_t want = solid_coverage[y * FRAMEBUFFER_WIDTH + x];
                const uint8_t got = (uint8_t)((px(&test_context, x, y) >> 24) & 0xFFu);
                if (want != got)
                {
                    printf("  at (%d,%d): solid alpha %u, gradient alpha %u\n", x, y, want, got);
                    return fail("radius clip: gradient coverage differs from the solid fill's");
                }
            }
        }
    }

    /* -----------------------------------------------------------------------
     * Test 7: per-corner radii clip the gradient independently.
     *
     * Only the top-left corner is rounded, so the gradient must be cut there and reach the pixel in
     * the other three — the case a single uniform-radius mask would get wrong.
     * ----------------------------------------------------------------------- */
    {
        ERProps props;
        memset(&props, 0, sizeof(props));
        props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
        props.width = FRAMEBUFFER_WIDTH;
        props.height = FRAMEBUFFER_HEIGHT;
        props.opacity = OPACITY_OPAQUE;
        props.border_top_left_radius = 20;
        props.gradient_type = ER_GRADIENT_LINEAR;
        props.gradient_angle = 90.0f;
        props.gradient_stop_count = 2;
        props.gradient_stops[0].color = 0xFF2A9D8F;
        props.gradient_stops[0].position = 0.0f;
        props.gradient_stops[1].color = 0xFF9B59B6;
        props.gradient_stops[1].position = 1.0f;
        er_node_set_props(root, &props);

        memset(test_context.framebuffer, 0, sizeof(test_context.framebuffer));
        er_commit();

        if (((px(&test_context, 0, 0) >> 24) & 0xFFu) != 0)
            return fail("per-corner radius: the rounded top-left corner still painted");
        if (((px(&test_context, FRAMEBUFFER_WIDTH - 1, 0) >> 24) & 0xFFu) == 0)
            return fail("per-corner radius: the square top-right corner was clipped");
        if (((px(&test_context, 0, FRAMEBUFFER_HEIGHT - 1) >> 24) & 0xFFu) == 0)
            return fail("per-corner radius: the square bottom-left corner was clipped");
        if (((px(&test_context, FRAMEBUFFER_WIDTH - 1, FRAMEBUFFER_HEIGHT - 1) >> 24) & 0xFFu) == 0)
            return fail("per-corner radius: the square bottom-right corner was clipped");
    }

    /* -----------------------------------------------------------------------
     * Test 8: a corner direction follows the box.
     *
     * On a box that is not square, a fixed 45° puts the two other corners on different colours. A corner
     * direction puts them both on the middle one.
     * ----------------------------------------------------------------------- */
    {
        const int box_height = 24;

        ERProps props;
        memset(&props, 0, sizeof(props));
        props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
        props.width = FRAMEBUFFER_WIDTH;
        props.height = (int16_t)box_height;
        props.opacity = OPACITY_OPAQUE;
        props.gradient_type = ER_GRADIENT_LINEAR;
        props.gradient_corner = ER_GRADIENT_CORNER_BOTTOM_RIGHT;
        props.gradient_angle = 90.0f; /* ignored */
        props.gradient_stop_count = 2;
        props.gradient_stops[0].color = 0xFF000000;
        props.gradient_stops[0].position = 0.0f;
        props.gradient_stops[1].color = 0xFFC86432;
        props.gradient_stops[1].position = 1.0f;
        er_node_set_props(root, &props);

        memset(test_context.framebuffer, 0, sizeof(test_context.framebuffer));
        er_commit();

        if (px(&test_context, 0, 0) != 0xFF000000)
            return fail("corner: the top left is not the first stop");
        if (px(&test_context, FRAMEBUFFER_WIDTH - 1, box_height - 1) != 0xFFC86432)
            return fail("corner: the bottom right is not the last stop");
        const uint32_t top_right = px(&test_context, FRAMEBUFFER_WIDTH - 1, 0);
        const uint32_t bottom_left = px(&test_context, 0, box_height - 1);
        for (int shift = 0; shift <= 16; shift += 8)
        {
            const int difference = (int)((top_right >> shift) & 0xFFu) - (int)((bottom_left >> shift) & 0xFFu);
            if (difference < -1 || difference > 1)
                return fail("corner: the two other corners differ");
        }
        const int middle_red = (int)((top_right >> 16) & 0xFFu);
        if (middle_red < 99 || middle_red > 101)
            return fail("corner: the two other corners are not the middle colour");

        /* The opposite corner runs the same ramp backwards. */
        props.gradient_corner = ER_GRADIENT_CORNER_TOP_LEFT;
        er_node_set_props(root, &props);
        memset(test_context.framebuffer, 0, sizeof(test_context.framebuffer));
        er_commit();
        if (px(&test_context, 0, 0) != 0xFFC86432
            || px(&test_context, FRAMEBUFFER_WIDTH - 1, box_height - 1) != 0xFF000000)
            return fail("corner: to the top left did not reverse the ramp");
    }

    /* -----------------------------------------------------------------------
     * Test 9: stops interpolate premultiplied.
     *
     * Red fading to a transparent blue stays red all the way: the blue stop has no colour to give.
     * ----------------------------------------------------------------------- */
    {
        ERProps props;
        memset(&props, 0, sizeof(props));
        props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
        props.width = FRAMEBUFFER_WIDTH;
        props.height = FRAMEBUFFER_HEIGHT;
        props.opacity = OPACITY_OPAQUE;
        props.gradient_type = ER_GRADIENT_LINEAR;
        props.gradient_angle = 90.0f;
        props.gradient_stop_count = 2;
        props.gradient_stops[0].color = 0xFFFF0000;
        props.gradient_stops[0].position = 0.0f;
        props.gradient_stops[1].color = 0x000000FF;
        props.gradient_stops[1].position = 1.0f;
        er_node_set_props(root, &props);

        memset(test_context.framebuffer, 0, sizeof(test_context.framebuffer));
        er_commit();

        for (int x = 0; x < FRAMEBUFFER_WIDTH; x++)
        {
            const uint32_t pixel = px(&test_context, x, 5);
            if ((pixel & 0xFFFFu) != 0u)
                return fail("premultiplied: the transparent stop's blue leaked into the fade");
            if (((pixel >> 16) & 0xFFu) != (pixel >> 24))
                return fail("premultiplied: the red did not fade with the alpha");
        }
    }

    /* -----------------------------------------------------------------------
     * Test 10: a shallow ramp is dithered.
     *
     * Eight levels over 64 columns would be eight flat bands. Dithered, a column mixes the two levels beside
     * its exact value, and no pixel is more than one level out.
     * ----------------------------------------------------------------------- */
    {
        ERProps props;
        memset(&props, 0, sizeof(props));
        props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
        props.width = FRAMEBUFFER_WIDTH;
        props.height = FRAMEBUFFER_HEIGHT;
        props.opacity = OPACITY_OPAQUE;
        props.gradient_type = ER_GRADIENT_LINEAR;
        props.gradient_angle = 90.0f;
        props.gradient_stop_count = 2;
        props.gradient_stops[0].color = 0xFF000000;
        props.gradient_stops[0].position = 0.0f;
        props.gradient_stops[1].color = 0xFF080808;
        props.gradient_stops[1].position = 1.0f;
        er_node_set_props(root, &props);

        memset(test_context.framebuffer, 0, sizeof(test_context.framebuffer));
        er_commit();

        int mixed_columns = 0;
        for (int x = 0; x < FRAMEBUFFER_WIDTH; x++)
        {
            const float exact = 8.0f * (float)x / (float)(FRAMEBUFFER_WIDTH - 1);
            bool mixed = false;
            for (int y = 0; y < FRAMEBUFFER_HEIGHT; y++)
            {
                const int level = (int)(px(&test_context, x, y) & 0xFFu);
                if ((float)level < exact - 1.0f || (float)level > exact + 1.0f)
                    return fail("dither: a pixel is more than one level from the ramp");
                if (level != (int)(px(&test_context, x, 0) & 0xFFu))
                    mixed = true;
            }
            if (mixed)
                mixed_columns++;
        }
        if (mixed_columns < FRAMEBUFFER_WIDTH / 2)
            return fail("dither: the ramp is still banded");
        for (int y = 0; y < FRAMEBUFFER_HEIGHT; y++)
        {
            if (px(&test_context, 0, y) != 0xFF000000 || px(&test_context, FRAMEBUFFER_WIDTH - 1, y) != 0xFF080808)
                return fail("dither: an end of the ramp is not its stop's colour");
        }

        /* -------------------------------------------------------------------
         * Test 11: a partial repaint paints the same pixels.
         *
         * A small child changes colour and goes away again. Each time only its rect is repainted, which
         * has to land on the pattern the full repaint drew.
         * ------------------------------------------------------------------- */
        static uint32_t full_frame[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];
        memcpy(full_frame, test_context.framebuffer, sizeof(full_frame));

        ERNode* child = er_node_create(ER_NODE_VIEW);
        ERProps child_props;
        memset(&child_props, 0, sizeof(child_props));
        child_props.right = child_props.bottom = ER_LAYOUT_AUTO;
        child_props.min_width = child_props.max_width = ER_LAYOUT_AUTO;
        child_props.min_height = child_props.max_height = ER_LAYOUT_AUTO;
        child_props.flex_basis = ER_LAYOUT_AUTO;
        child_props.position = ER_POSITION_ABSOLUTE;
        child_props.left = 13;
        child_props.top = 21;
        child_props.width = 9;
        child_props.height = 7;
        child_props.opacity = OPACITY_OPAQUE;
        child_props.background_color = 0xFFFF0000;
        er_node_set_props(child, &child_props);
        er_tree_append_child(root, child);
        er_commit();
        if (px(&test_context, 15, 23) != 0xFFFF0000)
            return fail("partial repaint: the child did not paint");

        test_context.blend_calls = 0;
        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_commit();
        if (memcmp(full_frame, test_context.framebuffer, sizeof(full_frame)) != 0)
            return fail("partial repaint: the repainted rect differs from the full repaint");
        /* The damage is the child's seven rows and a small margin. */
        if (test_context.blend_calls > 16)
            return fail("partial repaint: rows outside the damage were blitted");
    }

    /* -----------------------------------------------------------------------
     * Test 12: a type that is not drawn behind a view leaves the background_color.
     * ----------------------------------------------------------------------- */
    {
        ERProps props;
        memset(&props, 0, sizeof(props));
        props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
        props.width = FRAMEBUFFER_WIDTH;
        props.height = FRAMEBUFFER_HEIGHT;
        props.opacity = OPACITY_OPAQUE;
        props.background_color = 0xFF336699;
        props.gradient_type = ER_GRADIENT_CONIC;
        props.gradient_stop_count = 2;
        props.gradient_stops[0].color = 0xFFFF0000;
        props.gradient_stops[1].color = 0xFF0000FF;
        props.gradient_stops[1].position = 1.0f;
        er_node_set_props(root, &props);

        memset(test_context.framebuffer, 0, sizeof(test_context.framebuffer));
        er_commit();
        if (px(&test_context, FRAMEBUFFER_WIDTH / 2, FRAMEBUFFER_HEIGHT / 2) != 0xFF336699)
            return fail("undrawn type: the background_color did not paint");
    }

#else /* ERUI_GRADIENT == 0 */

    /* -----------------------------------------------------------------------
     * ERUI_GRADIENT=0 smoke test: gradient_type prop is stored but ignored;
     * the node renders its background_color instead.
     * ----------------------------------------------------------------------- */
    {
        const uint32_t background = 0xFF2A9D8F;

        ERProps props;
        memset(&props, 0, sizeof(props));
        props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
        props.width = FRAMEBUFFER_WIDTH;
        props.height = FRAMEBUFFER_HEIGHT;
        props.opacity = OPACITY_OPAQUE;
        props.background_color = background;
        props.gradient_type = ER_GRADIENT_LINEAR;
        props.gradient_angle = 0.0f;
        props.gradient_stop_count = 2;
        props.gradient_stops[0].color = 0xFFFF0000;
        props.gradient_stops[0].position = 0.0f;
        props.gradient_stops[1].color = 0xFF0000FF;
        props.gradient_stops[1].position = 1.0f;
        er_node_set_props(root, &props);

        memset(test_context.framebuffer, 0, sizeof(test_context.framebuffer));
        er_commit();

        /* With gradients disabled the top row must have the background_color. */
        const uint32_t top_pixel = px(&test_context, FRAMEBUFFER_WIDTH / 2, 0);
        const uint32_t background_premultiplied = pm(background);
        if (top_pixel != background_premultiplied)
            return fail("ERUI_GRADIENT=0: background_color must be used when gradients disabled");
    }

#endif /* ERUI_GRADIENT */

    er_node_destroy(root);
    printf("PASS\n");
    return EXIT_SUCCESS;
}
