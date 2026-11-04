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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FRAMEBUFFER_WIDTH 64
#define FRAMEBUFFER_HEIGHT 64
#define OPACITY_OPAQUE 255U

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
    int blend_calls;        /**< Total blend_rect calls received. */
    int copy_calls;         /**< Total copy_rect calls received. */
    int format_calls;       /**< Total copy_rect_fmt calls received. */
    int last_format;        /**< Format of the most recent copy_rect_fmt call (-1 = none). */
} TestCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — backend stubs
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Backend fill_rect stub — image rendering never calls fill_rect, so this is a no-op.
 *
 * @param[in] argb    Fill color (unused).
 * @param[in] x       Left edge (unused).
 * @param[in] y       Top edge (unused).
 * @param[in] width   Width (unused).
 * @param[in] height  Height (unused).
 * @param[in] ctx     Opaque context (unused).
 */
static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    TestCtx* test_context = ctx;
    (void)argb;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)test_context;
}

/**
 * @brief Backend copy_rect that writes premultiplied ARGB rows into the test framebuffer.
 *
 * Fully opaque images render through this path (the engine's opaque fast path); the test
 * writes the pixels verbatim, matching a real backend's replace semantics for opaque sources.
 *
 * @param[in] source  Source pixel buffer (premultiplied ARGB8888).
 * @param[in] stride  Source row stride in bytes.
 * @param[in] x       Destination left edge in framebuffer pixels.
 * @param[in] y       Destination top edge in framebuffer pixels.
 * @param[in] width   Width of the region in pixels.
 * @param[in] height  Height of the region in pixels.
 * @param[in] ctx     Pointer to the TestCtx to write into.
 */
static void copy_cb(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    TestCtx* test_context = ctx;
    test_context->copy_calls++;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + row * stride);
        for (int column = 0; column < width; column++)
        {
            int pixel_x = x + column;
            int pixel_y = y + row;
            if (pixel_x >= 0 && pixel_x < test_context->framebuffer_width && pixel_y >= 0
                && pixel_y < test_context->framebuffer_height)
                test_context->framebuffer[pixel_y * test_context->framebuffer_width + pixel_x] = source_row[column];
        }
    }
}

/**
 * @brief Backend blend_rect that composites premultiplied ARGB rows into the test framebuffer.
 *
 * Alpha argument is ignored for simplicity — all blending in these tests uses opaque images.
 *
 * @param[in] source  Source pixel buffer (premultiplied ARGB8888).
 * @param[in] stride  Source row stride in bytes.
 * @param[in] alpha   Global opacity (ignored).
 * @param[in] x       Destination left edge in framebuffer pixels.
 * @param[in] y       Destination top edge in framebuffer pixels.
 * @param[in] width   Width of the region in pixels.
 * @param[in] height  Height of the region in pixels.
 * @param[in] ctx     Pointer to the TestCtx to write into.
 */
static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    TestCtx* test_context = ctx;
    (void)alpha;
    test_context->blend_calls++;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + row * stride);
        for (int column = 0; column < width; column++)
        {
            int pixel_x = x + column;
            int pixel_y = y + row;
            if (pixel_x >= 0 && pixel_x < test_context->framebuffer_width && pixel_y >= 0
                && pixel_y < test_context->framebuffer_height)
                test_context->framebuffer[pixel_y * test_context->framebuffer_width + pixel_x] = source_row[column];
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
    test_context->blend_calls = 0;
    test_context->copy_calls = 0;
    test_context->format_calls = 0;
    test_context->last_format = -1;
}

/** @brief Expands an RGB565 pixel to opaque ARGB8888 by bit replication (mirrors the engine). */
static uint32_t expand565(uint16_t pixel)
{
    const uint32_t red_bits = (pixel >> 11) & 0x1Fu;
    const uint32_t green_bits = (pixel >> 5) & 0x3Fu;
    const uint32_t blue_bits = pixel & 0x1Fu;
    return 0xFF000000u | (((red_bits << 3) | (red_bits >> 2)) << 16) | (((green_bits << 2) | (green_bits >> 4)) << 8)
           | ((blue_bits << 3) | (blue_bits >> 2));
}

/**
 * @brief Backend copy_rect_fmt that consumes the source in its native format.
 *
 * Emulates a format-converting blitter (DMA2D M2M_PFC): RGB565 sources are expanded per pixel on
 * write, ARGB sources are written verbatim — no alpha inspection, per the opaque-source contract.
 */
static void
copy_fmt_cb(const void* source, int stride, ERImageFormat format, int x, int y, int width, int height, void* ctx)
{
    TestCtx* test_context = ctx;
    test_context->format_calls++;
    test_context->last_format = (int)format;
    for (int row = 0; row < height; row++)
    {
        const uint8_t* source_row = (const uint8_t*)source + (size_t)row * (size_t)stride;
        for (int column = 0; column < width; column++)
        {
            const int pixel_x = x + column;
            const int pixel_y = y + row;
            if (pixel_x < 0 || pixel_x >= test_context->framebuffer_width || pixel_y < 0
                || pixel_y >= test_context->framebuffer_height)
                continue;
            const uint32_t pixel = (format == ER_IMG_RGB565) ? expand565(((const uint16_t*)source_row)[column])
                                                             : ((const uint32_t*)source_row)[column];
            test_context->framebuffer[pixel_y * test_context->framebuffer_width + pixel_x] = pixel;
        }
    }
}

/**
 * @brief Returns an ERProps with all int16_t layout fields set to ER_LAYOUT_AUTO.
 *
 * Using memset(0) on ERProps would set flex_basis and max_width/max_height to 0
 * rather than ER_LAYOUT_AUTO (INT16_MIN), causing the layout engine to treat them
 * as explicit zero-pixel constraints. This helper provides the correct default state.
 *
 * @return ERProps with all layout sentinels initialised and opacity set to 255.
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

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Tests image rendering via the scene graph API.
 *
 * Verifies:
 *  - er_image_load stores images; unknown names render nothing.
 *  - stretch at 1:1 copies pixels exactly.
 *  - stretch with upscaling (2x) tiles source pixels across the destination.
 *  - contain: scaled image is centered with correct pixels.
 *  - cover: fills the rect using the center of the source.
 *  - center: image displayed at original size, centered in node.
 *  - repeat: image tiles across the node.
 *  - tint_color: replaces pixel RGB while preserving alpha.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    static uint32_t framebuffer[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];
    TestCtx test_context = {framebuffer, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT, 0, 0, 0, -1};
    EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, &test_context};
    embedded_renderer_set_backend(&backend);

    /* 2×2 test image: TL=red, TR=green, BL=blue, BR=yellow (all opaque, premultiplied). */
    static const uint32_t image_2x2[4] = {
        0xFFFF0000, /* (0,0) red */
        0xFF00FF00, /* (1,0) green */
        0xFF0000FF, /* (0,1) blue */
        0xFFFFFF00, /* (1,1) yellow */
    };

    /* 4×4 solid cyan image. */
    static uint32_t image_4x4[16];
    memset(image_4x4, 0, sizeof(image_4x4));
    for (int pixel_index = 0; pixel_index < 16; pixel_index++)
        image_4x4[pixel_index] = 0xFF00FFFF;

    er_image_load("test2x2", image_2x2, 2, 2);
    er_image_load("test4x4", image_4x4, 4, 4);

    /* -----------------------------------------------------------------------
     * Unknown image name: no blend call, framebuffer untouched.
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        er_node_set_props(root, &root_props);

        ERNode* image_node = er_node_create(ER_NODE_IMAGE);
        ERProps image_props = props_default();
        image_props.width = 4;
        image_props.height = 4;
        strncpy(image_props.image_name, "nonexistent", ER_IMAGE_NAME_MAX);
        image_props.resize_mode = ER_RESIZE_STRETCH;
        er_node_set_props(image_node, &image_props);

        er_tree_append_child(root, image_node);
        er_tree_set_root(root);
        er_commit();

        if (test_context.blend_calls != 0 || test_context.copy_calls != 0)
            return fail("unknown image name: a blit was emitted");

        er_tree_remove_child(root, image_node);
        er_node_destroy(image_node);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Stretch 1:1 — 2×2 image into a 2×2 node.
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        er_node_set_props(root, &root_props);

        ERNode* image_node = er_node_create(ER_NODE_IMAGE);
        ERProps image_props = props_default();
        image_props.width = 2;
        image_props.height = 2;
        strncpy(image_props.image_name, "test2x2", ER_IMAGE_NAME_MAX);
        image_props.resize_mode = ER_RESIZE_STRETCH;
        er_node_set_props(image_node, &image_props);

        er_tree_append_child(root, image_node);
        er_tree_set_root(root);
        er_commit();

        if (px(&test_context, 0, 0) != 0xFFFF0000)
            return fail("stretch 1:1: (0,0) should be red");
        if (px(&test_context, 1, 0) != 0xFF00FF00)
            return fail("stretch 1:1: (1,0) should be green");
        if (px(&test_context, 0, 1) != 0xFF0000FF)
            return fail("stretch 1:1: (0,1) should be blue");
        if (px(&test_context, 1, 1) != 0xFFFFFF00)
            return fail("stretch 1:1: (1,1) should be yellow");

        er_tree_remove_child(root, image_node);
        er_node_destroy(image_node);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Stretch 2x — 2×2 image stretched to a 4×4 node.
     * Each source pixel maps to a 2×2 block of destination pixels.
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        er_node_set_props(root, &root_props);

        ERNode* image_node = er_node_create(ER_NODE_IMAGE);
        ERProps image_props = props_default();
        image_props.width = 4;
        image_props.height = 4;
        strncpy(image_props.image_name, "test2x2", ER_IMAGE_NAME_MAX);
        image_props.resize_mode = ER_RESIZE_STRETCH;
        er_node_set_props(image_node, &image_props);

        er_tree_append_child(root, image_node);
        er_tree_set_root(root);
        er_commit();

        /* TL quadrant → red */
        if (px(&test_context, 0, 0) != 0xFFFF0000)
            return fail("stretch 2x: (0,0) should be red");
        if (px(&test_context, 1, 1) != 0xFFFF0000)
            return fail("stretch 2x: (1,1) should be red");
        /* TR quadrant → green */
        if (px(&test_context, 2, 0) != 0xFF00FF00)
            return fail("stretch 2x: (2,0) should be green");
        if (px(&test_context, 3, 1) != 0xFF00FF00)
            return fail("stretch 2x: (3,1) should be green");
        /* BL quadrant → blue */
        if (px(&test_context, 0, 2) != 0xFF0000FF)
            return fail("stretch 2x: (0,2) should be blue");
        /* BR quadrant → yellow */
        if (px(&test_context, 3, 3) != 0xFFFFFF00)
            return fail("stretch 2x: (3,3) should be yellow");

        er_tree_remove_child(root, image_node);
        er_node_destroy(image_node);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Contain — 4×4 cyan image into an 8×4 node (landscape).
     * Scale = min(8/4, 4/4) = 1. Scaled size = 4×4. Horizontal centering → off_x = 2.
     * Pixels at x<2 and x>=6 should be 0 (background).
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        er_node_set_props(root, &root_props);

        ERNode* image_node = er_node_create(ER_NODE_IMAGE);
        ERProps image_props = props_default();
        image_props.width = 8;
        image_props.height = 4;
        strncpy(image_props.image_name, "test4x4", ER_IMAGE_NAME_MAX);
        image_props.resize_mode = ER_RESIZE_CONTAIN;
        er_node_set_props(image_node, &image_props);

        er_tree_append_child(root, image_node);
        er_tree_set_root(root);
        er_commit();

        /* The 4×4 cyan image contained in an 8×4 node:
         * scale = min(8/4, 4/4) = 1.0 → scaled_w=4, scaled_h=4; off_x=2, off_y=0. */
        if (px(&test_context, 2, 0) != 0xFF00FFFF)
            return fail("contain: (2,0) should be cyan (inside scaled image)");
        if (px(&test_context, 5, 3) != 0xFF00FFFF)
            return fail("contain: (5,3) should be cyan (inside scaled image)");
        if (px(&test_context, 0, 0) != 0u)
            return fail("contain: (0,0) should be empty (letterbox)");
        if (px(&test_context, 7, 0) != 0u)
            return fail("contain: (7,0) should be empty (letterbox)");

        er_tree_remove_child(root, image_node);
        er_node_destroy(image_node);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Center — 2×2 image in a 4×4 node: image at original size, centered.
     * off_x = (4-2)/2 = 1, off_y = 1. Pixels outside the 2×2 area stay 0.
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        er_node_set_props(root, &root_props);

        ERNode* image_node = er_node_create(ER_NODE_IMAGE);
        ERProps image_props = props_default();
        image_props.width = 4;
        image_props.height = 4;
        strncpy(image_props.image_name, "test2x2", ER_IMAGE_NAME_MAX);
        image_props.resize_mode = ER_RESIZE_CENTER;
        er_node_set_props(image_node, &image_props);

        er_tree_append_child(root, image_node);
        er_tree_set_root(root);
        er_commit();

        /* off_x=1, off_y=1 → image starts at (1,1). */
        if (px(&test_context, 1, 1) != 0xFFFF0000)
            return fail("center: (1,1) should be red (top-left of image)");
        if (px(&test_context, 2, 1) != 0xFF00FF00)
            return fail("center: (2,1) should be green");
        if (px(&test_context, 1, 2) != 0xFF0000FF)
            return fail("center: (1,2) should be blue");
        if (px(&test_context, 2, 2) != 0xFFFFFF00)
            return fail("center: (2,2) should be yellow");
        if (px(&test_context, 0, 0) != 0u)
            return fail("center: (0,0) should be empty (outside centered image)");
        if (px(&test_context, 3, 3) != 0u)
            return fail("center: (3,3) should be empty (outside centered image)");

        er_tree_remove_child(root, image_node);
        er_node_destroy(image_node);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Repeat — 2×2 image tiled across a 4×4 node.
     * The 2×2 pattern repeats: (0,0)=red,(1,0)=green,(2,0)=red,(3,0)=green.
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        er_node_set_props(root, &root_props);

        ERNode* image_node = er_node_create(ER_NODE_IMAGE);
        ERProps image_props = props_default();
        image_props.width = 4;
        image_props.height = 4;
        strncpy(image_props.image_name, "test2x2", ER_IMAGE_NAME_MAX);
        image_props.resize_mode = ER_RESIZE_REPEAT;
        er_node_set_props(image_node, &image_props);

        er_tree_append_child(root, image_node);
        er_tree_set_root(root);
        er_commit();

        /* ty=0: renders image rows 0-1 to dst rows 0-1.
         * ty=2: renders image rows 0-1 again to dst rows 2-3. */
        if (px(&test_context, 0, 0) != 0xFFFF0000)
            return fail("repeat: (0,0) should be red");
        if (px(&test_context, 1, 0) != 0xFF00FF00)
            return fail("repeat: (1,0) should be green");
        if (px(&test_context, 2, 0) != 0xFFFF0000)
            return fail("repeat: (2,0) should be red (second horizontal tile)");
        if (px(&test_context, 3, 0) != 0xFF00FF00)
            return fail("repeat: (3,0) should be green");
        if (px(&test_context, 0, 1) != 0xFF0000FF)
            return fail("repeat: (0,1) should be blue (image row 1, first tile)");
        if (px(&test_context, 2, 1) != 0xFF0000FF)
            return fail("repeat: (2,1) should be blue (image row 1, second tile)");
        /* ty=2: new tile row — image row 0 again. */
        if (px(&test_context, 0, 2) != 0xFFFF0000)
            return fail("repeat: (0,2) should be red (top of second tile row)");
        if (px(&test_context, 2, 2) != 0xFFFF0000)
            return fail("repeat: (2,2) should be red (top of second tile row)");
        if (px(&test_context, 0, 3) != 0xFF0000FF)
            return fail("repeat: (0,3) should be blue (image row 1 of second tile row)");
        if (px(&test_context, 2, 3) != 0xFF0000FF)
            return fail("repeat: (2,3) should be blue");

        er_tree_remove_child(root, image_node);
        er_node_destroy(image_node);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Tint color — red tint on the 4×4 cyan image.
     * Tint 0xFFFF0000 (red). Cyan pixels (0xFF00FFFF premul) become:
     *   A=255, R = 255*255/255 = 255, G = 255*0/255 = 0, B = 255*0/255 = 0 → 0xFFFF0000.
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        er_node_set_props(root, &root_props);

        ERNode* image_node = er_node_create(ER_NODE_IMAGE);
        ERProps image_props = props_default();
        image_props.width = 4;
        image_props.height = 4;
        strncpy(image_props.image_name, "test4x4", ER_IMAGE_NAME_MAX);
        image_props.resize_mode = ER_RESIZE_STRETCH;
        image_props.tint_color = 0xFF0000u; /* red tint (straight-alpha RGB) */
        er_node_set_props(image_node, &image_props);

        er_tree_append_child(root, image_node);
        er_tree_set_root(root);
        er_commit();

        /* Cyan pixels tinted with red → all RGB zeroed except red channel. */
        uint32_t tinted = px(&test_context, 0, 0);
        uint8_t red = (uint8_t)((tinted >> 16) & 0xFF);
        uint8_t green = (uint8_t)((tinted >> 8) & 0xFF);
        uint8_t blue = (uint8_t)(tinted & 0xFF);
        if (red == 0)
            return fail("tint: red channel should be nonzero for red tint");
        if (green != 0)
            return fail("tint: green channel should be 0 for red tint on cyan");
        if (blue != 0)
            return fail("tint: blue channel should be 0 for red tint on cyan");

        er_tree_remove_child(root, image_node);
        er_node_destroy(image_node);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Opaque fast path — a fully opaque image must arrive via copy_rect
     * (replace), never blend_rect, both at 1:1 and when scaled.
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        er_node_set_props(root, &root_props);

        ERNode* image_node = er_node_create(ER_NODE_IMAGE);
        ERProps image_props = props_default();
        image_props.width = 2; /* 1:1 with the 2×2 opaque image */
        image_props.height = 2;
        strncpy(image_props.image_name, "test2x2", ER_IMAGE_NAME_MAX);
        image_props.resize_mode = ER_RESIZE_STRETCH;
        er_node_set_props(image_node, &image_props);

        er_tree_append_child(root, image_node);
        er_tree_set_root(root);
        er_commit();

        if (test_context.copy_calls == 0)
            return fail("opaque 1:1: expected copy_rect to be used");
        if (test_context.blend_calls != 0)
            return fail("opaque 1:1: blend_rect should not be called for an opaque image");

        /* Scale it up 2x: the row-scratch path must also stay on copy_rect. */
        reset(&test_context);
        image_props.width = 4;
        image_props.height = 4;
        er_node_set_props(image_node, &image_props);
        er_commit();

        if (test_context.copy_calls == 0)
            return fail("opaque scaled: expected copy_rect to be used");
        if (test_context.blend_calls != 0)
            return fail("opaque scaled: blend_rect should not be called for an opaque image");
        if (px(&test_context, 0, 0) != 0xFFFF0000)
            return fail("opaque scaled: (0,0) should be red");

        er_tree_remove_child(root, image_node);
        er_node_destroy(image_node);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Translucent image — one pixel with alpha < 255 must keep the blend path.
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        static const uint32_t translucent_image[4] = {
            0xFFFF0000, /* opaque red */
            0x8000FF00, /* HALF-TRANSPARENT green (premultiplied would be 0x80008000; value irrelevant) */
            0xFF0000FF,
            0xFFFFFF00,
        };
        er_image_load("translucent2x2", translucent_image, 2, 2);

        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        er_node_set_props(root, &root_props);

        ERNode* image_node = er_node_create(ER_NODE_IMAGE);
        ERProps image_props = props_default();
        image_props.width = 2;
        image_props.height = 2;
        strncpy(image_props.image_name, "translucent2x2", ER_IMAGE_NAME_MAX);
        image_props.resize_mode = ER_RESIZE_STRETCH;
        er_node_set_props(image_node, &image_props);

        er_tree_append_child(root, image_node);
        er_tree_set_root(root);
        er_commit();

        if (test_context.blend_calls == 0)
            return fail("translucent: expected blend_rect to be used");
        if (test_context.copy_calls != 0)
            return fail("translucent: copy_rect must not be used for a non-opaque image");

        er_tree_remove_child(root, image_node);
        er_node_destroy(image_node);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * RGB565 image — registered via er_image_load_rgb565: expands to exact
     * ARGB (bit replication), renders via copy_rect, and scales correctly.
     *   0xF800 → red, 0x07E0 → green, 0x001F → blue, 0xFFFF → white.
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        static const uint16_t image_565[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
        er_image_load_rgb565("test565", image_565, 2, 2);

        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        er_node_set_props(root, &root_props);

        ERNode* image_node = er_node_create(ER_NODE_IMAGE);
        ERProps image_props = props_default();
        image_props.width = 2; /* 1:1 */
        image_props.height = 2;
        strncpy(image_props.image_name, "test565", ER_IMAGE_NAME_MAX);
        image_props.resize_mode = ER_RESIZE_STRETCH;
        er_node_set_props(image_node, &image_props);

        er_tree_append_child(root, image_node);
        er_tree_set_root(root);
        er_commit();

        if (test_context.copy_calls == 0)
            return fail("rgb565 1:1: expected copy_rect to be used");
        if (test_context.blend_calls != 0)
            return fail("rgb565 1:1: blend_rect should not be called");
        if (px(&test_context, 0, 0) != 0xFFFF0000)
            return fail("rgb565 1:1: (0,0) should expand to opaque red");
        if (px(&test_context, 1, 0) != 0xFF00FF00)
            return fail("rgb565 1:1: (1,0) should expand to opaque green");
        if (px(&test_context, 0, 1) != 0xFF0000FF)
            return fail("rgb565 1:1: (0,1) should expand to opaque blue");
        if (px(&test_context, 1, 1) != 0xFFFFFFFF)
            return fail("rgb565 1:1: (1,1) should expand to opaque white");

        /* Stretch 2x through the scaling path. */
        reset(&test_context);
        image_props.width = 4;
        image_props.height = 4;
        er_node_set_props(image_node, &image_props);
        er_commit();

        if (test_context.copy_calls == 0)
            return fail("rgb565 scaled: expected copy_rect to be used");
        if (test_context.blend_calls != 0)
            return fail("rgb565 scaled: blend_rect should not be called");
        if (px(&test_context, 0, 0) != 0xFFFF0000)
            return fail("rgb565 scaled: (0,0) should be red");
        if (px(&test_context, 3, 3) != 0xFFFFFFFF)
            return fail("rgb565 scaled: (3,3) should be white");

        /* Tint: alpha is preserved (opaque), RGB replaced by the tint color. */
        reset(&test_context);
        image_props.width = 2;
        image_props.height = 2;
        image_props.tint_color = 0xFF00FFu; /* magenta tint */
        er_node_set_props(image_node, &image_props);
        er_commit();

        if (px(&test_context, 0, 0) != 0xFFFF00FF)
            return fail("rgb565 tint: (0,0) should be opaque magenta");

        er_tree_remove_child(root, image_node);
        er_node_destroy(image_node);
        er_node_destroy(root);
    }

    /* =======================================================================
     * copy_rect_fmt backend: opaque sources must reach the backend in their
     * native format as ONE whole-rect call (the DMA2D M2M_PFC path) — never
     * row-expanded on the CPU, never per-pixel scanned via copy_rect.
     * ======================================================================= */
    EmbeddedRenderBackend format_backend = backend;
    format_backend.copy_rect_fmt = copy_fmt_cb;
    embedded_renderer_set_backend(&format_backend); /* resets the image registry: re-register below */

    static const uint16_t image_565b[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
    static const uint32_t translucent_format_image[4] = {0xFFFF0000, 0x8000FF00, 0xFF0000FF, 0xFFFFFF00};
    er_image_load("test2x2", image_2x2, 2, 2);
    er_image_load("translucent2x2", translucent_format_image, 2, 2);
    er_image_load_rgb565("test565", image_565b, 2, 2);

    /* -----------------------------------------------------------------------
     * RGB565 1:1 — one copy_rect_fmt call carrying raw 565 rows; no CPU
     * expansion (copy_rect stays untouched), no blending.
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        er_node_set_props(root, &root_props);

        ERNode* image_node = er_node_create(ER_NODE_IMAGE);
        ERProps image_props = props_default();
        image_props.width = 2;
        image_props.height = 2;
        strncpy(image_props.image_name, "test565", ER_IMAGE_NAME_MAX);
        image_props.resize_mode = ER_RESIZE_STRETCH;
        er_node_set_props(image_node, &image_props);

        er_tree_append_child(root, image_node);
        er_tree_set_root(root);
        er_commit();

        if (test_context.format_calls != 1)
            return fail("fmt backend rgb565 1:1: expected exactly one copy_rect_fmt call");
        if (test_context.last_format != (int)ER_IMG_RGB565)
            return fail("fmt backend rgb565 1:1: source should arrive as RGB565");
        if (test_context.copy_calls != 0)
            return fail("fmt backend rgb565 1:1: copy_rect must not be used (no CPU row expansion)");
        if (test_context.blend_calls != 0)
            return fail("fmt backend rgb565 1:1: blend_rect must not be used");
        if (px(&test_context, 0, 0) != 0xFFFF0000 || px(&test_context, 1, 0) != 0xFF00FF00
            || px(&test_context, 0, 1) != 0xFF0000FF || px(&test_context, 1, 1) != 0xFFFFFFFF)
            return fail("fmt backend rgb565 1:1: expanded pixels wrong");

        /* Scaled draw: sampling is inherently per-pixel, so the CPU path (copy_rect rows)
         * is expected — the hook is only for the 1:1 pass-through. */
        reset(&test_context);
        image_props.width = 4;
        image_props.height = 4;
        er_node_set_props(image_node, &image_props);
        er_commit();

        if (test_context.format_calls != 0)
            return fail("fmt backend rgb565 scaled: copy_rect_fmt must not be used for scaling");
        if (test_context.copy_calls == 0)
            return fail("fmt backend rgb565 scaled: expected the CPU copy path");
        if (px(&test_context, 0, 0) != 0xFFFF0000 || px(&test_context, 3, 3) != 0xFFFFFFFF)
            return fail("fmt backend rgb565 scaled: pixels wrong");

        er_tree_remove_child(root, image_node);
        er_node_destroy(image_node);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Opaque ARGB 1:1 — also routed through copy_rect_fmt (the opacity
     * guarantee lets the backend skip its per-pixel alpha scan).
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        er_node_set_props(root, &root_props);

        ERNode* image_node = er_node_create(ER_NODE_IMAGE);
        ERProps image_props = props_default();
        image_props.width = 2;
        image_props.height = 2;
        strncpy(image_props.image_name, "test2x2", ER_IMAGE_NAME_MAX);
        image_props.resize_mode = ER_RESIZE_STRETCH;
        er_node_set_props(image_node, &image_props);

        er_tree_append_child(root, image_node);
        er_tree_set_root(root);
        er_commit();

        if (test_context.format_calls != 1)
            return fail("fmt backend argb 1:1: expected exactly one copy_rect_fmt call");
        if (test_context.last_format != (int)ER_IMG_ARGB8888)
            return fail("fmt backend argb 1:1: source should arrive as ARGB8888");
        if (test_context.copy_calls != 0)
            return fail("fmt backend argb 1:1: copy_rect must not be used");
        if (px(&test_context, 0, 0) != 0xFFFF0000 || px(&test_context, 1, 1) != 0xFFFFFF00)
            return fail("fmt backend argb 1:1: pixels wrong");

        er_tree_remove_child(root, image_node);
        er_node_destroy(image_node);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Translucent image — must NOT take the opaque hook; blend as before.
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        er_node_set_props(root, &root_props);

        ERNode* image_node = er_node_create(ER_NODE_IMAGE);
        ERProps image_props = props_default();
        image_props.width = 2;
        image_props.height = 2;
        strncpy(image_props.image_name, "translucent2x2", ER_IMAGE_NAME_MAX);
        image_props.resize_mode = ER_RESIZE_STRETCH;
        er_node_set_props(image_node, &image_props);

        er_tree_append_child(root, image_node);
        er_tree_set_root(root);
        er_commit();

        if (test_context.format_calls != 0)
            return fail("fmt backend translucent: copy_rect_fmt must not receive a non-opaque source");
        if (test_context.blend_calls == 0)
            return fail("fmt backend translucent: expected blend_rect");

        er_tree_remove_child(root, image_node);
        er_node_destroy(image_node);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * A damage-clipped repaint over an image, which produces only the part
     * inside the damage, matches the full repaint there and writes nothing
     * outside it: an upscaled ARGB image, an unscaled RGB565 image on the CPU
     * expansion path, and an unscaled tinted one on the general path, each
     * larger than the repainted rect.
     * ---------------------------------------------------------------------- */
    {
        /* No copy_rect_fmt, so RGB565 1:1 expands on the CPU. Setting the backend resets the image
         * registry, so the images load after it. */
        embedded_renderer_set_backend(&backend);
        er_image_load("test2x2", image_2x2, 2, 2);
        static uint16_t image_565[32 * 32];
        for (int pixel_index = 0; pixel_index < 32 * 32; pixel_index++)
            image_565[pixel_index] = (uint16_t)(pixel_index * 2654435761u >> 16);
        er_image_load_rgb565("clip565", image_565, 32, 32);

        static const struct
        {
            const char* image;
            uint32_t tint;
            const char* what;
        } k_cases[] = {
            {"test2x2", 0u, "scaled"},
            {"clip565", 0u, "unscaled rgb565"},
            {"clip565", 0x80FF40u, "unscaled tinted"},
        };
        for (size_t case_index = 0; case_index < sizeof(k_cases) / sizeof(k_cases[0]); case_index++)
        {
            reset(&test_context);
            er_reset();

            ERNode* root = er_node_create(ER_NODE_VIEW);
            ERProps root_props = props_default();
            root_props.width = FRAMEBUFFER_WIDTH;
            root_props.height = FRAMEBUFFER_HEIGHT;
            er_node_set_props(root, &root_props);

            ERNode* image_node = er_node_create(ER_NODE_IMAGE);
            ERProps image_props = props_default();
            image_props.width = 32;
            image_props.height = 32;
            image_props.margin_left = 4;
            image_props.margin_top = 4;
            strncpy(image_props.image_name, k_cases[case_index].image, ER_IMAGE_NAME_MAX);
            image_props.resize_mode = ER_RESIZE_STRETCH;
            image_props.tint_color = k_cases[case_index].tint;
            er_node_set_props(image_node, &image_props);

            ERNode* overlay = er_node_create(ER_NODE_VIEW);
            ERProps overlay_props = props_default();
            overlay_props.position = ER_POS_ABSOLUTE;
            overlay_props.left = 13;
            overlay_props.top = 17;
            overlay_props.width = 6;
            overlay_props.height = 5;
            overlay_props.background_color = 0x01000000u;
            er_node_set_props(overlay, &overlay_props);

            er_tree_append_child(root, image_node);
            er_tree_append_child(root, overlay);
            er_tree_set_root(root);
            er_commit();

            static uint32_t full[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];
            memcpy(full, framebuffer, sizeof(full));
            /* The stub fill_rect draws nothing, so the repaint shows only what the image emits. */
            for (int y = 17; y < 22; y++)
                for (int x = 13; x < 19; x++)
                    framebuffer[y * FRAMEBUFFER_WIDTH + x] = 0xDEADBEEFu;
            framebuffer[0] = 0xDEADBEEFu; /* outside the damage: must survive */

            overlay_props.background_color = 0x02000000u;
            er_node_set_props(overlay, &overlay_props);
            er_commit();

            if (framebuffer[0] != 0xDEADBEEFu)
            {
                fprintf(stderr,
                        "FAIL: clipped %s image repaint: wrote outside the damaged rect\n",
                        k_cases[case_index].what);
                return EXIT_FAILURE;
            }
            framebuffer[0] = full[0];
            if (memcmp(framebuffer, full, sizeof(full)) != 0)
            {
                fprintf(stderr,
                        "FAIL: clipped %s image repaint: pixels differ from the full repaint\n",
                        k_cases[case_index].what);
                return EXIT_FAILURE;
            }

            er_tree_remove_child(root, overlay);
            er_tree_remove_child(root, image_node);
            er_node_destroy(overlay);
            er_node_destroy(image_node);
            er_node_destroy(root);
        }
    }

    return EXIT_SUCCESS;
}
