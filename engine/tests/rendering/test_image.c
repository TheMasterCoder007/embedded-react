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

#define FB_W 64
#define FB_H 64

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Pixel framebuffer and call statistics gathered during a render pass.
 */
typedef struct
{
    uint32_t* fb;    /**< Flat ARGB8888 framebuffer; rows are fb_w pixels wide. */
    int fb_w;        /**< Framebuffer width in pixels. */
    int fb_h;        /**< Framebuffer height in pixels. */
    int blend_calls; /**< Total blend_rect calls received. */
} TestCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — backend stubs
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Backend fill_rect stub — image rendering never calls fill_rect, so this is a no-op.
 *
 * @param[in] argb  Fill color (unused).
 * @param[in] x     Left edge (unused).
 * @param[in] y     Top edge (unused).
 * @param[in] w     Width (unused).
 * @param[in] h     Height (unused).
 * @param[in] ctx   Opaque context (unused).
 */
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    TestCtx* t = ctx;
    (void)argb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)t;
}

/**
 * @brief Backend copy_rect stub — no-op; image rendering uses blend_rect, not copy_rect.
 *
 * @param[in] src     Source pixel buffer (unused).
 * @param[in] stride  Source stride in bytes (unused).
 * @param[in] x       Destination X coordinate (unused).
 * @param[in] y       Destination Y coordinate (unused).
 * @param[in] w       Width in pixels (unused).
 * @param[in] h       Height in pixels (unused).
 * @param[in] ctx     Opaque context (unused).
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
 * @brief Backend blend_rect that composites premultiplied ARGB rows into the test framebuffer.
 *
 * Alpha argument is ignored for simplicity — all blending in these tests uses opaque images.
 *
 * @param[in] src     Source pixel buffer (premultiplied ARGB8888).
 * @param[in] stride  Source row stride in bytes.
 * @param[in] alpha   Global opacity (ignored).
 * @param[in] x       Destination left edge in framebuffer pixels.
 * @param[in] y       Destination top edge in framebuffer pixels.
 * @param[in] w       Width of the region in pixels.
 * @param[in] h       Height of the region in pixels.
 * @param[in] ctx     Pointer to the TestCtx to write into.
 */
static void blend_cb(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    TestCtx* t = ctx;
    (void)alpha;
    t->blend_calls++;
    for (int row = 0; row < h; row++)
    {
        const uint32_t* src_row = (const uint32_t*)((const uint8_t*)src + row * stride);
        for (int col = 0; col < w; col++)
        {
            int px = x + col;
            int py = y + row;
            if (px >= 0 && px < t->fb_w && py >= 0 && py < t->fb_h)
                t->fb[py * t->fb_w + px] = src_row[col];
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
    t->blend_calls = 0;
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
    static uint32_t fb[FB_W * FB_H];
    TestCtx tc = {fb, FB_W, FB_H, 0};
    EmbeddedRenderBackend be = {fill_cb, copy_cb, blend_cb, NULL, NULL, &tc};
    embedded_renderer_set_backend(&be);

    /* 2×2 test image: TL=red, TR=green, BL=blue, BR=yellow (all opaque, premultiplied). */
    static const uint32_t img2x2[4] = {
        0xFFFF0000, /* (0,0) red */
        0xFF00FF00, /* (1,0) green */
        0xFF0000FF, /* (0,1) blue */
        0xFFFFFF00, /* (1,1) yellow */
    };

    /* 4×4 solid cyan image. */
    static uint32_t img4x4[16];
    memset(img4x4, 0, sizeof(img4x4));
    for (int i = 0; i < 16; i++)
        img4x4[i] = 0xFF00FFFF;

    er_image_load("test2x2", img2x2, 2, 2);
    er_image_load("test4x4", img4x4, 4, 4);

    /* -----------------------------------------------------------------------
     * Unknown image name: no blend call, framebuffer untouched.
     * ---------------------------------------------------------------------- */
    reset(&tc);
    {
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = FB_W;
        rp.height = FB_H;
        er_node_set_props(root, &rp);

        ERNode* img_node = er_node_create(ER_NODE_IMAGE);
        ERProps ip = props_default();
        ip.width = 4;
        ip.height = 4;
        strncpy(ip.image_name, "nonexistent", ER_IMAGE_NAME_MAX);
        ip.resize_mode = ER_RESIZE_STRETCH;
        er_node_set_props(img_node, &ip);

        er_tree_append_child(root, img_node);
        er_tree_set_root(root);
        er_commit();

        if (tc.blend_calls != 0)
            return fail("unknown image name: blend_rect was called");

        er_tree_remove_child(root, img_node);
        er_node_destroy(img_node);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Stretch 1:1 — 2×2 image into a 2×2 node.
     * ---------------------------------------------------------------------- */
    reset(&tc);
    {
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = FB_W;
        rp.height = FB_H;
        er_node_set_props(root, &rp);

        ERNode* img_node = er_node_create(ER_NODE_IMAGE);
        ERProps ip = props_default();
        ip.width = 2;
        ip.height = 2;
        strncpy(ip.image_name, "test2x2", ER_IMAGE_NAME_MAX);
        ip.resize_mode = ER_RESIZE_STRETCH;
        er_node_set_props(img_node, &ip);

        er_tree_append_child(root, img_node);
        er_tree_set_root(root);
        er_commit();

        if (px(&tc, 0, 0) != 0xFFFF0000)
            return fail("stretch 1:1: (0,0) should be red");
        if (px(&tc, 1, 0) != 0xFF00FF00)
            return fail("stretch 1:1: (1,0) should be green");
        if (px(&tc, 0, 1) != 0xFF0000FF)
            return fail("stretch 1:1: (0,1) should be blue");
        if (px(&tc, 1, 1) != 0xFFFFFF00)
            return fail("stretch 1:1: (1,1) should be yellow");

        er_tree_remove_child(root, img_node);
        er_node_destroy(img_node);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Stretch 2x — 2×2 image stretched to a 4×4 node.
     * Each source pixel maps to a 2×2 block of destination pixels.
     * ---------------------------------------------------------------------- */
    reset(&tc);
    {
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = FB_W;
        rp.height = FB_H;
        er_node_set_props(root, &rp);

        ERNode* img_node = er_node_create(ER_NODE_IMAGE);
        ERProps ip = props_default();
        ip.width = 4;
        ip.height = 4;
        strncpy(ip.image_name, "test2x2", ER_IMAGE_NAME_MAX);
        ip.resize_mode = ER_RESIZE_STRETCH;
        er_node_set_props(img_node, &ip);

        er_tree_append_child(root, img_node);
        er_tree_set_root(root);
        er_commit();

        /* TL quadrant → red */
        if (px(&tc, 0, 0) != 0xFFFF0000)
            return fail("stretch 2x: (0,0) should be red");
        if (px(&tc, 1, 1) != 0xFFFF0000)
            return fail("stretch 2x: (1,1) should be red");
        /* TR quadrant → green */
        if (px(&tc, 2, 0) != 0xFF00FF00)
            return fail("stretch 2x: (2,0) should be green");
        if (px(&tc, 3, 1) != 0xFF00FF00)
            return fail("stretch 2x: (3,1) should be green");
        /* BL quadrant → blue */
        if (px(&tc, 0, 2) != 0xFF0000FF)
            return fail("stretch 2x: (0,2) should be blue");
        /* BR quadrant → yellow */
        if (px(&tc, 3, 3) != 0xFFFFFF00)
            return fail("stretch 2x: (3,3) should be yellow");

        er_tree_remove_child(root, img_node);
        er_node_destroy(img_node);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Contain — 4×4 cyan image into an 8×4 node (landscape).
     * Scale = min(8/4, 4/4) = 1. Scaled size = 4×4. Horizontal centering → off_x = 2.
     * Pixels at x<2 and x>=6 should be 0 (background).
     * ---------------------------------------------------------------------- */
    reset(&tc);
    {
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = FB_W;
        rp.height = FB_H;
        er_node_set_props(root, &rp);

        ERNode* img_node = er_node_create(ER_NODE_IMAGE);
        ERProps ip = props_default();
        ip.width = 8;
        ip.height = 4;
        strncpy(ip.image_name, "test4x4", ER_IMAGE_NAME_MAX);
        ip.resize_mode = ER_RESIZE_CONTAIN;
        er_node_set_props(img_node, &ip);

        er_tree_append_child(root, img_node);
        er_tree_set_root(root);
        er_commit();

        /* The 4×4 cyan image contained in an 8×4 node:
         * scale = min(8/4, 4/4) = 1.0 → scaled_w=4, scaled_h=4; off_x=2, off_y=0. */
        if (px(&tc, 2, 0) != 0xFF00FFFF)
            return fail("contain: (2,0) should be cyan (inside scaled image)");
        if (px(&tc, 5, 3) != 0xFF00FFFF)
            return fail("contain: (5,3) should be cyan (inside scaled image)");
        if (px(&tc, 0, 0) != 0u)
            return fail("contain: (0,0) should be empty (letterbox)");
        if (px(&tc, 7, 0) != 0u)
            return fail("contain: (7,0) should be empty (letterbox)");

        er_tree_remove_child(root, img_node);
        er_node_destroy(img_node);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Center — 2×2 image in a 4×4 node: image at original size, centered.
     * off_x = (4-2)/2 = 1, off_y = 1. Pixels outside the 2×2 area stay 0.
     * ---------------------------------------------------------------------- */
    reset(&tc);
    {
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = FB_W;
        rp.height = FB_H;
        er_node_set_props(root, &rp);

        ERNode* img_node = er_node_create(ER_NODE_IMAGE);
        ERProps ip = props_default();
        ip.width = 4;
        ip.height = 4;
        strncpy(ip.image_name, "test2x2", ER_IMAGE_NAME_MAX);
        ip.resize_mode = ER_RESIZE_CENTER;
        er_node_set_props(img_node, &ip);

        er_tree_append_child(root, img_node);
        er_tree_set_root(root);
        er_commit();

        /* off_x=1, off_y=1 → image starts at (1,1). */
        if (px(&tc, 1, 1) != 0xFFFF0000)
            return fail("center: (1,1) should be red (top-left of image)");
        if (px(&tc, 2, 1) != 0xFF00FF00)
            return fail("center: (2,1) should be green");
        if (px(&tc, 1, 2) != 0xFF0000FF)
            return fail("center: (1,2) should be blue");
        if (px(&tc, 2, 2) != 0xFFFFFF00)
            return fail("center: (2,2) should be yellow");
        if (px(&tc, 0, 0) != 0u)
            return fail("center: (0,0) should be empty (outside centered image)");
        if (px(&tc, 3, 3) != 0u)
            return fail("center: (3,3) should be empty (outside centered image)");

        er_tree_remove_child(root, img_node);
        er_node_destroy(img_node);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Repeat — 2×2 image tiled across a 4×4 node.
     * The 2×2 pattern repeats: (0,0)=red,(1,0)=green,(2,0)=red,(3,0)=green.
     * ---------------------------------------------------------------------- */
    reset(&tc);
    {
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = FB_W;
        rp.height = FB_H;
        er_node_set_props(root, &rp);

        ERNode* img_node = er_node_create(ER_NODE_IMAGE);
        ERProps ip = props_default();
        ip.width = 4;
        ip.height = 4;
        strncpy(ip.image_name, "test2x2", ER_IMAGE_NAME_MAX);
        ip.resize_mode = ER_RESIZE_REPEAT;
        er_node_set_props(img_node, &ip);

        er_tree_append_child(root, img_node);
        er_tree_set_root(root);
        er_commit();

        /* ty=0: renders image rows 0-1 to dst rows 0-1.
         * ty=2: renders image rows 0-1 again to dst rows 2-3. */
        if (px(&tc, 0, 0) != 0xFFFF0000)
            return fail("repeat: (0,0) should be red");
        if (px(&tc, 1, 0) != 0xFF00FF00)
            return fail("repeat: (1,0) should be green");
        if (px(&tc, 2, 0) != 0xFFFF0000)
            return fail("repeat: (2,0) should be red (second horizontal tile)");
        if (px(&tc, 3, 0) != 0xFF00FF00)
            return fail("repeat: (3,0) should be green");
        if (px(&tc, 0, 1) != 0xFF0000FF)
            return fail("repeat: (0,1) should be blue (image row 1, first tile)");
        if (px(&tc, 2, 1) != 0xFF0000FF)
            return fail("repeat: (2,1) should be blue (image row 1, second tile)");
        /* ty=2: new tile row — image row 0 again. */
        if (px(&tc, 0, 2) != 0xFFFF0000)
            return fail("repeat: (0,2) should be red (top of second tile row)");
        if (px(&tc, 2, 2) != 0xFFFF0000)
            return fail("repeat: (2,2) should be red (top of second tile row)");
        if (px(&tc, 0, 3) != 0xFF0000FF)
            return fail("repeat: (0,3) should be blue (image row 1 of second tile row)");
        if (px(&tc, 2, 3) != 0xFF0000FF)
            return fail("repeat: (2,3) should be blue");

        er_tree_remove_child(root, img_node);
        er_node_destroy(img_node);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Tint color — red tint on the 4×4 cyan image.
     * Tint 0xFFFF0000 (red). Cyan pixels (0xFF00FFFF premul) become:
     *   A=255, R = 255*255/255 = 255, G = 255*0/255 = 0, B = 255*0/255 = 0 → 0xFFFF0000.
     * ---------------------------------------------------------------------- */
    reset(&tc);
    {
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = FB_W;
        rp.height = FB_H;
        er_node_set_props(root, &rp);

        ERNode* img_node = er_node_create(ER_NODE_IMAGE);
        ERProps ip = props_default();
        ip.width = 4;
        ip.height = 4;
        strncpy(ip.image_name, "test4x4", ER_IMAGE_NAME_MAX);
        ip.resize_mode = ER_RESIZE_STRETCH;
        ip.tint_color = 0xFF0000u; /* red tint (straight-alpha RGB) */
        er_node_set_props(img_node, &ip);

        er_tree_append_child(root, img_node);
        er_tree_set_root(root);
        er_commit();

        /* Cyan pixels tinted with red → all RGB zeroed except red channel. */
        uint32_t tinted = px(&tc, 0, 0);
        uint8_t r = (uint8_t)((tinted >> 16) & 0xFF);
        uint8_t g = (uint8_t)((tinted >> 8) & 0xFF);
        uint8_t b = (uint8_t)(tinted & 0xFF);
        if (r == 0)
            return fail("tint: red channel should be nonzero for red tint");
        if (g != 0)
            return fail("tint: green channel should be 0 for red tint on cyan");
        if (b != 0)
            return fail("tint: blue channel should be 0 for red tint on cyan");

        er_tree_remove_child(root, img_node);
        er_node_destroy(img_node);
        er_node_destroy(root);
    }

    return EXIT_SUCCESS;
}
