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

/*
 * A transform's screen coordinates come from products of the app's scale, rotation and perspective, and each
 * ends in an int cast somewhere: the node's damage box, the inverse-map sampler, the hit test. A huge,
 * lopsided or infinite scale or rotation, or a 3D corner near the camera plane, puts that value past the int
 * range or makes it NaN, where C leaves the cast undefined. This pins what happens instead: coordinates clamp
 * to ±32767, a NaN one is dropped, and a matrix whose determinant is NaN or overflowed counts as singular, so
 * the node paints untransformed at its layout box, as one scaled to 0 does. Under
 * -fsanitize=float-cast-overflow, a cast that still sees such a value fails the run.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "transform.h" /* the matrix helpers, exercised directly */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FB_W 64
#define FB_H 64
#define RED 0xFFFF0000U
#define WHITE 0xFFFFFFFFU

static uint32_t s_fb[FB_W * FB_H];

/**
 * @brief Backend fill_rect: premultiplies the color and writes it, clipped to the framebuffer.
 */
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const uint32_t a = (argb >> 24) & 0xFFU;
    if (a == 0U)
        return;
    const uint32_t premul = (a << 24) | ((((argb >> 16) & 0xFFU) * a / 255U) << 16)
                            | ((((argb >> 8) & 0xFFU) * a / 255U) << 8) | ((argb & 0xFFU) * a / 255U);
    for (int row = y < 0 ? 0 : y; row < y + h && row < FB_H; row++)
        for (int col = x < 0 ? 0 : x; col < x + w && col < FB_W; col++)
            s_fb[row * FB_W + col] = premul;
}

/**
 * @brief Backend copy_rect: copies premultiplied pixels, clipped to the framebuffer.
 */
static void copy_cb(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    for (int row = 0; row < h; row++)
    {
        const uint32_t* src_row = (const uint32_t*)((const uint8_t*)src + row * stride);
        for (int col = 0; col < w; col++)
        {
            const int px = x + col, py = y + row;
            if (px >= 0 && px < FB_W && py >= 0 && py < FB_H)
                s_fb[py * FB_W + px] = src_row[col];
        }
    }
}

/**
 * @brief Backend blend_rect: source-over composites premultiplied pixels scaled by a global alpha.
 */
static void blend_cb(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    for (int row = 0; row < h; row++)
    {
        const uint32_t* src_row = (const uint32_t*)((const uint8_t*)src + row * stride);
        for (int col = 0; col < w; col++)
        {
            const int bx = x + col, by = y + row;
            if (bx < 0 || bx >= FB_W || by < 0 || by >= FB_H)
                continue;
            uint32_t sp = src_row[col];
            if (alpha < 255U)
                sp = ((((sp >> 24) & 0xFFU) * alpha / 255U) << 24) | ((((sp >> 16) & 0xFFU) * alpha / 255U) << 16)
                     | ((((sp >> 8) & 0xFFU) * alpha / 255U) << 8) | ((sp & 0xFFU) * alpha / 255U);
            const uint32_t sa = (sp >> 24) & 0xFFU;
            if (sa == 0U)
                continue;
            const uint32_t d = s_fb[by * FB_W + bx];
            const uint32_t inv = 255U - sa;
            uint32_t out = 0U;
            for (int shift = 0; shift < 32; shift += 8)
                out |= ((((sp >> shift) & 0xFFU) + ((d >> shift) & 0xFFU) * inv / 255U) & 0xFFU) << shift;
            s_fb[by * FB_W + bx] = out;
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

#if ERUI_TRANSFORMS_FULL
/**
 * @brief Paints a red 20×21 node at (20,20) on a white root with the given transform props.
 *
 * The height is odd so the default centre pivot sits on a half pixel: a node squashed to under a pixel still
 * straddles a row, so its one-row box reaches the sampler.
 *
 * @param[in] xf  Props carrying the transform (position, size and color are overwritten).
 *
 * @return The pixel at (25,25), inside the node's layout box.
 */
static uint32_t paint(const ERProps* xf)
{
    memset(s_fb, 0, sizeof s_fb);
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp;
    er_props_default(&rp);
    rp.width = FB_W;
    rp.height = FB_H;
    rp.background_color = WHITE;
    er_node_set_props(root, &rp);

    ERNode* node = er_node_create(ER_NODE_VIEW);
    ERProps np = *xf;
    np.position = ER_POS_ABSOLUTE;
    np.left = 20;
    np.top = 20;
    np.width = 20;
    np.height = 21;
    np.background_color = RED;
    er_node_set_props(node, &np);

    er_tree_append_child(root, node);
    er_tree_set_root(root);
    er_commit();
    const uint32_t p = s_fb[25 * FB_W + 25];

    er_tree_remove_child(root, node);
    er_node_destroy(node);
    er_node_destroy(root);
    return p;
}
#endif /* ERUI_TRANSFORMS_FULL */

int main(void)
{
    EmbeddedRenderBackend be = {fill_cb, copy_cb, blend_cb, NULL, NULL, NULL};
    embedded_renderer_set_backend(&be);

    int x, y, w, h;

    /* A box past the int range clamps to ±32767 instead of an undefined cast. */
    er_transform_aabb(0, 0, 10, 10, 1e30f, 0.0f, 0.0f, 1e30f, 0.0f, 0.0f, &x, &y, &w, &h);
    if (x != 0 || y != 0 || w != 32767 || h != 32767)
        return fail("a box past the int range should clamp to 32767");
    er_transform_aabb(0, 0, 10, 10, -1e30f, 0.0f, 0.0f, -1e30f, 0.0f, 0.0f, &x, &y, &w, &h);
    if (x != -32767 || y != -32767 || w != 32767 || h != 32767)
        return fail("a box below the int range should clamp to -32767");
    /* A NaN matrix leaves no corner to bound. */
    er_transform_aabb(0, 0, 10, 10, NAN, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, &x, &y, &w, &h);
    if (x != 0 || y != 0 || w != 0 || h != 0)
        return fail("a NaN matrix should give an empty box");

    /* A NaN or overflowed determinant has no usable inverse: singular, like a scale of 0. */
    float ia, ib, ic, id, itx, ity;
    if (er_transform_invert(NAN, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, &ia, &ib, &ic, &id, &itx, &ity))
        return fail("a NaN matrix should not invert");
    if (er_transform_invert(1e30f, 0.0f, 0.0f, 1e30f, 0.0f, 0.0f, &ia, &ib, &ic, &id, &itx, &ity))
        return fail("a matrix whose determinant overflows should not invert");
    if (!er_transform_invert(2.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, &ia, &ib, &ic, &id, &itx, &ity))
        return fail("a plain 2x scale should still invert");

    /* A touch mapped past the int range clamps; one mapped to NaN has no position. */
    int lx = 0, ly = 0;
    if (!er_transform_map_point(1e10f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 100, 5, &lx, &ly) || lx != 32767 || ly != 5)
        return fail("a point mapped past the int range should clamp to 32767");
    if (er_transform_map_point(1.0f, 0.0f, 0.0f, 1.0f, NAN, 0.0f, 100, 5, &lx, &ly))
        return fail("a point mapped to NaN should report no position");

#if ERUI_3D_TRANSFORMS
    /* W = 1e-30 at every corner: a projection that runs out to 1e31, clamped. */
    const float near_plane[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1e-30f};
    er_transform_aabb_3d(0, 0, 10, 10, near_plane, &x, &y, &w, &h);
    if (x != -1 || y != -1 || w != 32769 || h != 32769)
        return fail("a corner near the camera plane should clamp its box to 32767");
    const float nan_h[9] = {NAN, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    er_transform_aabb_3d(0, 0, 10, 10, nan_h, &x, &y, &w, &h);
    if (x != 0 || y != 0 || w != 0 || h != 0)
        return fail("a NaN homography should give an empty box");

    float inv[9];
    if (er_transform_homography_invert(nan_h, inv))
        return fail("a NaN homography should not invert");
    const float huge_h[9] = {1e30f, 0.0f, 0.0f, 0.0f, 1e30f, 0.0f, 0.0f, 0.0f, 1.0f};
    if (er_transform_homography_invert(huge_h, inv))
        return fail("a homography whose determinant overflows should not invert");
    const float identity[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    if (!er_transform_homography_invert(identity, inv))
        return fail("the identity should still invert");

    if (!er_transform_map_point_3d(near_plane, 10, 0, &lx, &ly) || lx != 32767 || ly != 0)
        return fail("a point back-projected past the int range should clamp to 32767");
    if (er_transform_map_point_3d(nan_h, 10, 0, &lx, &ly))
        return fail("a point back-projected to NaN should report no position");
#endif /* ERUI_3D_TRANSFORMS */

#if ERUI_TRANSFORMS_FULL
    ERProps xf;

    /* The determinant overflows (1e60): singular, so the node paints untransformed at its layout box. */
    er_props_default(&xf);
    xf.transform_scale_x = xf.transform_scale_y = 1e30f;
    if (paint(&xf) != RED)
        return fail("a scale of 1e30 should paint the untransformed box");

    /* Infinite scale or rotation: an infinite entry times a zero one makes the determinant NaN. */
    er_props_default(&xf);
    xf.transform_scale_x = xf.transform_scale_y = INFINITY;
    if (paint(&xf) != RED)
        return fail("an infinite scale should paint the untransformed box");
    er_props_default(&xf);
    xf.transform_rotate_z = INFINITY;
    if (paint(&xf) != RED)
        return fail("an infinite rotation should paint the untransformed box");

    /* Lopsided: the determinant is 1e9 * 1e-8 = 10, which inverts, but the box is 2e10 wide and the sampler
     * maps screen rows 1e8 source rows apart. The node is under a pixel tall, so none of its box is painted. */
    er_props_default(&xf);
    xf.transform_scale_x = 1e9f;
    xf.transform_scale_y = 1e-8f;
    if (paint(&xf) != WHITE)
        return fail("a node scaled under a pixel tall should not paint its box");

#if ERUI_3D_TRANSFORMS
    /* 3D with an overflowing scale, and with an infinite tilt: both singular, both untransformed. */
    er_props_default(&xf);
    xf.transform_rotate_x = 1.0f;
    xf.transform_scale_x = xf.transform_scale_y = 1e20f;
    if (paint(&xf) != RED)
        return fail("a 3D scale of 1e20 should paint the untransformed box");
    er_props_default(&xf);
    xf.transform_rotate_x = INFINITY;
    if (paint(&xf) != RED)
        return fail("an infinite 3D tilt should paint the untransformed box");
#endif /* ERUI_3D_TRANSFORMS */
#endif /* ERUI_TRANSFORMS_FULL */

    return EXIT_SUCCESS;
}
