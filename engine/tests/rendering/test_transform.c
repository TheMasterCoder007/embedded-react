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

#define FRAMEBUFFER_WIDTH 64
#define FRAMEBUFFER_HEIGHT 64
#define ALPHA_OPAQUE 255U
#define CHANNEL_MAX 255U
#define OPACITY_OPAQUE 255U

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Pixel framebuffer and dimensions used by the test backend stubs.
 */
typedef struct
{
    uint32_t* framebuffer;  /**< Flat premultiplied ARGB8888 framebuffer; rows are framebuffer_width pixels wide. */
    int framebuffer_width;  /**< Framebuffer width in pixels. */
    int framebuffer_height; /**< Framebuffer height in pixels. */
} TestCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — backend stubs
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Backend fill_rect: premultiplies the color and writes it into the test framebuffer.
 *
 * @param[in] argb    Straight-alpha ARGB8888 fill color.
 * @param[in] x       Left edge of the rectangle.
 * @param[in] y       Top edge of the rectangle.
 * @param[in] width   Width in pixels.
 * @param[in] height  Height in pixels.
 * @param[in] ctx     Pointer to TestCtx.
 */
static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    TestCtx* test_context = ctx;
    const uint8_t alpha = (uint8_t)((argb >> 24) & 0xFFU);
    if (alpha == 0U)
        return;
    const uint8_t red = (uint8_t)((uint32_t)((argb >> 16) & 0xFFU) * alpha / CHANNEL_MAX);
    const uint8_t green = (uint8_t)((uint32_t)((argb >> 8) & 0xFFU) * alpha / CHANNEL_MAX);
    const uint8_t blue = (uint8_t)((uint32_t)(argb & 0xFFU) * alpha / CHANNEL_MAX);
    const uint32_t premultiplied = ((uint32_t)alpha << 24) | ((uint32_t)red << 16) | ((uint32_t)green << 8) | blue;
    for (int row = y; row < y + height; row++)
        for (int column = x; column < x + width; column++)
            if (column >= 0 && column < test_context->framebuffer_width && row >= 0
                && row < test_context->framebuffer_height)
                test_context->framebuffer[row * test_context->framebuffer_width + column] = premultiplied;
}

/**
 * @brief Backend copy_rect: copies premultiplied ARGB8888 pixels directly into the framebuffer.
 *
 * @param[in] source  Source pixel buffer (premultiplied ARGB8888).
 * @param[in] stride  Source row stride in bytes.
 * @param[in] x       Destination left edge.
 * @param[in] y       Destination top edge.
 * @param[in] width   Width in pixels.
 * @param[in] height  Height in pixels.
 * @param[in] ctx     Pointer to TestCtx.
 */
static void copy_cb(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    TestCtx* test_context = ctx;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + row * stride);
        for (int column = 0; column < width; column++)
        {
            int pixel_x = x + column, pixel_y = y + row;
            if (pixel_x >= 0 && pixel_x < test_context->framebuffer_width && pixel_y >= 0
                && pixel_y < test_context->framebuffer_height)
                test_context->framebuffer[pixel_y * test_context->framebuffer_width + pixel_x] = source_row[column];
        }
    }
}

/**
 * @brief Backend blend_rect: source-over composites premultiplied pixels into the framebuffer.
 *
 * Scales the source by the global alpha before compositing, matching the behavior the
 * embedded_react scratch system expects from the platform backend.
 *
 * @param[in] source  Source pixel buffer (premultiplied ARGB8888).
 * @param[in] stride  Source row stride in bytes.
 * @param[in] alpha   Global alpha scale 0–255.
 * @param[in] x       Destination left edge.
 * @param[in] y       Destination top edge.
 * @param[in] width   Width in pixels.
 * @param[in] height  Height in pixels.
 * @param[in] ctx     Pointer to TestCtx.
 */
static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    TestCtx* test_context = ctx;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + row * stride);
        for (int column = 0; column < width; column++)
        {
            int pixel_x = x + column, pixel_y = y + row;
            if (pixel_x < 0 || pixel_x >= test_context->framebuffer_width || pixel_y < 0
                || pixel_y >= test_context->framebuffer_height)
                continue;
            uint32_t source_pixel = source_row[column];
            if (alpha < ALPHA_OPAQUE)
            {
                source_pixel = ((uint32_t)((source_pixel >> 24) & 0xFFU) * alpha / CHANNEL_MAX << 24)
                               | ((uint32_t)((source_pixel >> 16) & 0xFFU) * alpha / CHANNEL_MAX << 16)
                               | ((uint32_t)((source_pixel >> 8) & 0xFFU) * alpha / CHANNEL_MAX << 8)
                               | (uint32_t)(source_pixel & 0xFFU) * alpha / CHANNEL_MAX;
            }
            const uint8_t source_alpha = (uint8_t)((source_pixel >> 24) & 0xFFU);
            if (source_alpha == 0U)
                continue;
            if (source_alpha == ALPHA_OPAQUE)
            {
                test_context->framebuffer[pixel_y * test_context->framebuffer_width + pixel_x] = source_pixel;
            }
            else
            {
                const uint32_t destination_pixel =
                    test_context->framebuffer[pixel_y * test_context->framebuffer_width + pixel_x];
                const uint8_t inverse_alpha = (uint8_t)(CHANNEL_MAX - source_alpha);
                const uint8_t out_alpha =
                    (uint8_t)(source_alpha
                              + (uint32_t)((destination_pixel >> 24) & 0xFFU) * inverse_alpha / CHANNEL_MAX);
                const uint8_t out_red =
                    (uint8_t)(((source_pixel >> 16) & 0xFFU)
                              + (uint32_t)((destination_pixel >> 16) & 0xFFU) * inverse_alpha / CHANNEL_MAX);
                const uint8_t out_green =
                    (uint8_t)(((source_pixel >> 8) & 0xFFU)
                              + (uint32_t)((destination_pixel >> 8) & 0xFFU) * inverse_alpha / CHANNEL_MAX);
                const uint8_t out_blue =
                    (uint8_t)((source_pixel & 0xFFU)
                              + (uint32_t)(destination_pixel & 0xFFU) * inverse_alpha / CHANNEL_MAX);
                test_context->framebuffer[pixel_y * test_context->framebuffer_width + pixel_x] =
                    ((uint32_t)out_alpha << 24) | ((uint32_t)out_red << 16) | ((uint32_t)out_green << 8) | out_blue;
            }
        }
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
 * @brief Reads a single premultiplied ARGB8888 pixel from the test framebuffer.
 *
 * @param[in] test_context  TestCtx owning the framebuffer.
 * @param[in] x             X coordinate.
 * @param[in] y             Y coordinate.
 *
 * @return Premultiplied ARGB8888 pixel value at (x, y).
 */
static uint32_t px(const TestCtx* test_context, int x, int y)
{
    return test_context->framebuffer[y * test_context->framebuffer_width + x];
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
    ERProps props = {0};
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    props.width = props.height = ER_LAYOUT_AUTO;
    props.min_width = props.max_width = ER_LAYOUT_AUTO;
    props.min_height = props.max_height = ER_LAYOUT_AUTO;
    props.padding = props.padding_left = props.padding_top = ER_LAYOUT_AUTO;
    props.padding_right = props.padding_bottom = ER_LAYOUT_AUTO;
    props.margin = props.margin_left = props.margin_top = ER_LAYOUT_AUTO;
    props.margin_right = props.margin_bottom = ER_LAYOUT_AUTO;
    props.gap = props.row_gap = props.column_gap = ER_LAYOUT_AUTO;
    props.flex_basis = ER_LAYOUT_AUTO;
    props.opacity = OPACITY_OPAQUE;
    return props;
}

/**
 * @brief Zeros the test framebuffer, discarding any pixels written by a previous test case.
 *
 * @param[in,out] test_context  TestCtx whose framebuffer should be cleared.
 */
static void reset(TestCtx* test_context)
{
    memset(test_context->framebuffer,
           0,
           (size_t)test_context->framebuffer_width * (size_t)test_context->framebuffer_height * sizeof(uint32_t));
}

/* Press-hit counters and their callbacks for the hit-test cases in main(). These live at file scope
 * because C99 (the engine's standard, with compiler extensions off) has no nested functions; defining
 * them inside main() compiled only under GCC's extension and broke the build under Clang. */
static int s_hit_count;
#if ERUI_3D_TRANSFORMS && ERUI_TRANSFORMS_FULL
static int s_3d_hits;
#endif
#if ERUI_TRANSFORMS_FULL
static int s_scale_hits;
#endif

static void press_cb(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    (void)data;
    (void)user_data;
    s_hit_count++;
}

#if ERUI_3D_TRANSFORMS && ERUI_TRANSFORMS_FULL
static void press_3d(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    (void)data;
    (void)user_data;
    s_3d_hits++;
}
#endif

#if ERUI_TRANSFORMS_FULL
static void scale_press_cb(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    (void)data;
    (void)user_data;
    s_scale_hits++;
}
#endif

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
    static uint32_t framebuffer[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];
    TestCtx test_context = {framebuffer, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT};
    EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, &test_context};
    embedded_renderer_set_backend(&backend);

    /* -------------------------------------------------------------------
     * Translate-only: red 8×8 child with translateX=10 at origin.
     * Without transform it would paint at (0,0)–(7,7).
     * With translateX=10 it should paint at (10,0)–(17,7).
     * ------------------------------------------------------------------ */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        root_props.background_color = 0xFFFFFFFFU; /* white */
        er_node_set_props(root, &root_props);

        ERNode* child = er_node_create(ER_NODE_VIEW);
        ERProps child_props = props_default();
        child_props.width = 8;
        child_props.height = 8;
        child_props.background_color = 0xFFFF0000U; /* red */
        child_props.transform_translate_x = 10.0f;
        er_node_set_props(child, &child_props);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* Pixel at (0,0) should remain white (no red paint without transform) */
        if (px(&test_context, 0, 0) != 0xFFFFFFFFU)
            return fail("translate: pixel at origin should be white (no untranslated red)");
        /* Pixel at (10,0) should be red (premultiplied: 0xFF FF 00 00) */
        if (px(&test_context, 10, 0) != 0xFFFF0000U)
            return fail("translate: pixel at translated position should be red");
        /* Pixel at (17,0) should be red (last column of 8-wide child) */
        if (px(&test_context, 17, 0) != 0xFFFF0000U)
            return fail("translate: right edge of translated child should be red");
        /* Pixel at (18,0) outside child — white */
        if (px(&test_context, 18, 0) != 0xFFFFFFFFU)
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
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        er_node_set_props(root, &root_props);

        ERNode* child = er_node_create(ER_NODE_VIEW);
        ERProps child_props = props_default();
        child_props.width = 8;
        child_props.height = 8;
        child_props.background_color = 0xFFFF0000U;
        child_props.transform_translate_x = 10.0f;
        er_node_set_props(child, &child_props);

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
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        root_props.background_color = 0xFF000000U; /* black background */
        er_node_set_props(root, &root_props);

        /* Place a 16×16 red square in the middle of the framebuffer. */
        const int center_x = FRAMEBUFFER_WIDTH / 2;
        const int center_y = FRAMEBUFFER_HEIGHT / 2;

        ERNode* child = er_node_create(ER_NODE_VIEW);
        ERProps child_props = props_default();
        child_props.position = ER_POS_ABSOLUTE;
        child_props.left = (int16_t)(center_x - 8);
        child_props.top = (int16_t)(center_y - 8);
        child_props.width = 16;
        child_props.height = 16;
        child_props.background_color = 0xFFFF0000U; /* red */
        /* Rotate 45° around Y with perspective 200; origin defaults to center. */
        child_props.transform_rotate_y = 45.0f;
        child_props.transform_perspective = 200.0f;
        child_props.transform_origin_x = -1.0f; /* center (negative = 0.5) */
        child_props.transform_origin_y = -1.0f;
        er_node_set_props(child, &child_props);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* Centre pixel must be red (the projected face covers the pivot). */
        if (px(&test_context, center_x, center_y) != 0xFFFF0000U)
            return fail("3D rotateY: centre pixel should be red");

        /* The untransformed right edge of the source is at center_x+8 from the centre.
         * With a 45° Y-rotation and perspective=200, the foreshortened projected width is
         * ≈ 11 px, so the pixel at center_x+8 should remain the black background (no red bleed). */
        if ((px(&test_context, center_x + 8, center_y) & 0x00FF0000U) != 0u)
            return fail("3D rotateY: red channel at cx+8 should be zero — projected width is foreshortened");

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }

    /* -------------------------------------------------------------------
     * Hit-test under 3D rotateY: same layout.
     * Touch at the visible projected centre should hit the child.
     * ------------------------------------------------------------------ */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        er_node_set_props(root, &root_props);

        const int center_x = FRAMEBUFFER_WIDTH / 2;
        const int center_y = FRAMEBUFFER_HEIGHT / 2;

        ERNode* child = er_node_create(ER_NODE_VIEW);
        ERProps child_props = props_default();
        child_props.position = ER_POS_ABSOLUTE;
        child_props.left = (int16_t)(center_x - 8);
        child_props.top = (int16_t)(center_y - 8);
        child_props.width = 16;
        child_props.height = 16;
        child_props.background_color = 0xFFFF0000U;
        child_props.transform_rotate_y = 45.0f;
        child_props.transform_perspective = 200.0f;
        child_props.transform_origin_x = -1.0f;
        child_props.transform_origin_y = -1.0f;
        er_node_set_props(child, &child_props);

        s_3d_hits = 0;
        er_event_set(child, ER_EVENT_PRESS, press_3d, NULL);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* Touch at exact centre — must hit the child. */
        embedded_renderer_touch(0, ER_TOUCH_DOWN, center_x, center_y);
        embedded_renderer_touch(0, ER_TOUCH_UP, center_x, center_y);
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
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        root_props.background_color = 0xFFFFFFFFU; /* white */
        er_node_set_props(root, &root_props);

        ERNode* child = er_node_create(ER_NODE_VIEW);
        ERProps child_props = props_default();
        child_props.width = 4;
        child_props.height = 4;
        child_props.background_color = 0xFFFF0000U; /* red */
        child_props.transform_scale_x = 2.0f;
        child_props.transform_scale_y = 2.0f;
        er_node_set_props(child, &child_props);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* transform_origin defaults to (0,0) so the 4×4 square scales around its
         * top-left corner, expanding to (0,0)-(7,7) in screen space.
         * Pixel (4,4) should be red (inside the 8×8 scaled quad). */
        if (px(&test_context, 4, 4) != 0xFFFF0000U)
            return fail("scale 2x: pixel at (4,4) should be red (inside scaled quad)");

        /* Pixel (6,6) maps to source (3.0,3.0) — last texel centre — bilinear lands
         * entirely on the red source pixel so the result is full red. */
        if (px(&test_context, 6, 6) != 0xFFFF0000U)
            return fail("scale 2x: pixel at (6,6) should be red (last source texel centre)");

        /* Pixel (7,7) maps to source (3.5,3.5): bilinear blends the last red texel with
         * the transparent border; composited on the white background this is a reddish
         * blend where red channel > green channel. */
        {
            const uint32_t edge_pixel = px(&test_context, 7, 7);
            if (((edge_pixel >> 16) & 0xFFu) <= ((edge_pixel >> 8) & 0xFFu))
                return fail("scale 2x: pixel at (7,7) should be reddish (bilinear edge blend)");
        }

        /* Pixel (9,9) is outside the 8×8 scaled quad — should be white. */
        if (px(&test_context, 9, 9) != 0xFFFFFFFFU)
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
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        er_node_set_props(root, &root_props);

        ERNode* child = er_node_create(ER_NODE_VIEW);
        ERProps child_props = props_default();
        child_props.width = 4;
        child_props.height = 4;
        child_props.background_color = 0xFFFF0000U;
        child_props.transform_scale_x = 2.0f;
        child_props.transform_scale_y = 2.0f;
        er_node_set_props(child, &child_props);

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

    /* -------------------------------------------------------------------
     * Oversized destination AABB — scale (fast path).
     * A 40×40 red square scaled 8× has a 320×320 destination box, larger than
     * the 240×240 scratch. The source (40×40) fits, so the transform must still
     * render (streamed out per row segment); only the on-screen part is visible.
     * Regression: the old full-dst buffer bailed out when dst exceeded the
     * scratch, making the node vanish entirely.
     * ------------------------------------------------------------------ */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        root_props.background_color = 0xFFFFFFFFU; /* white */
        er_node_set_props(root, &root_props);

        ERNode* child = er_node_create(ER_NODE_VIEW);
        ERProps child_props = props_default();
        child_props.width = 40;
        child_props.height = 40;
        child_props.background_color = 0xFFFF0000U; /* red */
        child_props.transform_scale_x = 8.0f;
        child_props.transform_scale_y = 8.0f;
        child_props.transform_origin_x = 0.0f; /* scale around the top-left corner */
        child_props.transform_origin_y = 0.0f;
        er_node_set_props(child, &child_props);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* The scaled quad covers 0..319 in both axes; the whole framebuffer lies inside it. */
        if (px(&test_context, FRAMEBUFFER_WIDTH / 2, FRAMEBUFFER_HEIGHT / 2) != 0xFFFF0000U)
            return fail("oversized-dst scale: centre pixel should be red (node must not vanish)");
        if (px(&test_context, 2, 2) != 0xFFFF0000U)
            return fail("oversized-dst scale: near-origin pixel should be red");

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }

    /* -------------------------------------------------------------------
     * Oversized destination AABB — rotation (general path).
     * A 200×200 red square centred on the framebuffer, rotated 45° about its
     * centre. The source (200×200) fits the 240×240 scratch but the rotated
     * AABB is ~285×285 — larger than the scratch. The centre must stay red.
     * Regression: previously the node vanished (dst-size early return).
     * ------------------------------------------------------------------ */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        root_props.background_color = 0xFFFFFFFFU; /* white */
        er_node_set_props(root, &root_props);

        ERNode* child = er_node_create(ER_NODE_VIEW);
        ERProps child_props = props_default();
        child_props.width = 200;
        child_props.height = 200;
        child_props.position = ER_POS_ABSOLUTE;
        child_props.left = (int16_t)(FRAMEBUFFER_WIDTH / 2 - 100); /* centre the 200×200 node on the framebuffer */
        child_props.top = (int16_t)(FRAMEBUFFER_HEIGHT / 2 - 100);
        child_props.background_color = 0xFFFF0000U; /* red */
        child_props.transform_rotate_z = 45.0f;
        child_props.transform_origin_x = -1.0f; /* centre pivot — node stays centred while rotated */
        child_props.transform_origin_y = -1.0f;
        er_node_set_props(child, &child_props);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* The rotated square still covers the framebuffer centre. */
        if (px(&test_context, FRAMEBUFFER_WIDTH / 2, FRAMEBUFFER_HEIGHT / 2) != 0xFFFF0000U)
            return fail("oversized-dst rotate: centre pixel should be red (node must not vanish)");

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }
#endif /* ERUI_TRANSFORMS_FULL */

    return EXIT_SUCCESS;
}
