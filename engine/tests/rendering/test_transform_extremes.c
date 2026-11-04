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
 * to ±32767 (a NaN one is dropped) and a box to ±16383, so its width fits the int16 its paint is recorded in;
 * a matrix whose determinant or inverse is NaN or overflowed counts as singular, so the node paints
 * untransformed at its layout box, as one scaled to 0 does; and a spinner's infinite angle is 0. Under
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

#define FRAMEBUFFER_WIDTH 64
#define FRAMEBUFFER_HEIGHT 64
#define RED 0xFFFF0000U
#define WHITE 0xFFFFFFFFU
#define ALPHA_OPAQUE 255U
#define CHANNEL_MAX 255U

static uint32_t s_framebuffer[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];
/* Backend draws since it was last zeroed: whether a commit repainted anything at all. */
static int s_draws;

/**
 * @brief Backend fill_rect: premultiplies the color and writes it, clipped to the framebuffer.
 */
static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    s_draws++;
    const uint32_t alpha = (argb >> 24) & 0xFFU;
    if (alpha == 0U)
        return;
    const uint32_t premultiplied = (alpha << 24) | ((((argb >> 16) & 0xFFU) * alpha / CHANNEL_MAX) << 16)
                                   | ((((argb >> 8) & 0xFFU) * alpha / CHANNEL_MAX) << 8)
                                   | ((argb & 0xFFU) * alpha / CHANNEL_MAX);
    for (int row = y < 0 ? 0 : y; row < y + height && row < FRAMEBUFFER_HEIGHT; row++)
        for (int column = x < 0 ? 0 : x; column < x + width && column < FRAMEBUFFER_WIDTH; column++)
            s_framebuffer[row * FRAMEBUFFER_WIDTH + column] = premultiplied;
}

/**
 * @brief Backend copy_rect: copies premultiplied pixels, clipped to the framebuffer.
 */
static void copy_cb(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    s_draws++;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + row * stride);
        for (int column = 0; column < width; column++)
        {
            const int pixel_x = x + column, pixel_y = y + row;
            if (pixel_x >= 0 && pixel_x < FRAMEBUFFER_WIDTH && pixel_y >= 0 && pixel_y < FRAMEBUFFER_HEIGHT)
                s_framebuffer[pixel_y * FRAMEBUFFER_WIDTH + pixel_x] = source_row[column];
        }
    }
}

/**
 * @brief Backend blend_rect: source-over composites premultiplied pixels scaled by a global alpha.
 */
static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    s_draws++;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + row * stride);
        for (int column = 0; column < width; column++)
        {
            const int pixel_x = x + column, pixel_y = y + row;
            if (pixel_x < 0 || pixel_x >= FRAMEBUFFER_WIDTH || pixel_y < 0 || pixel_y >= FRAMEBUFFER_HEIGHT)
                continue;
            uint32_t source_pixel = source_row[column];
            if (alpha < ALPHA_OPAQUE)
                source_pixel = ((((source_pixel >> 24) & 0xFFU) * alpha / CHANNEL_MAX) << 24)
                               | ((((source_pixel >> 16) & 0xFFU) * alpha / CHANNEL_MAX) << 16)
                               | ((((source_pixel >> 8) & 0xFFU) * alpha / CHANNEL_MAX) << 8)
                               | ((source_pixel & 0xFFU) * alpha / CHANNEL_MAX);
            const uint32_t source_alpha = (source_pixel >> 24) & 0xFFU;
            if (source_alpha == 0U)
                continue;
            const uint32_t destination_pixel = s_framebuffer[pixel_y * FRAMEBUFFER_WIDTH + pixel_x];
            const uint32_t inverse = CHANNEL_MAX - source_alpha;
            uint32_t out = 0U;
            for (int shift = 0; shift < 32; shift += 8)
                out |= ((((source_pixel >> shift) & 0xFFU)
                         + ((destination_pixel >> shift) & 0xFFU) * inverse / CHANNEL_MAX)
                        & 0xFFU)
                       << shift;
            s_framebuffer[pixel_y * FRAMEBUFFER_WIDTH + pixel_x] = out;
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
 * @brief Clears the framebuffer and the engine, and creates a white root the size of the framebuffer.
 *
 * @return The root, to append children to and mount.
 */
static ERNode* white_root(void)
{
    memset(s_framebuffer, 0, sizeof s_framebuffer);
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props;
    er_props_default(&root_props);
    root_props.width = FRAMEBUFFER_WIDTH;
    root_props.height = FRAMEBUFFER_HEIGHT;
    root_props.background_color = WHITE;
    er_node_set_props(root, &root_props);
    return root;
}

/**
 * @brief Paints a red 40×40 ActivityIndicator at (0,0) spun to `angle` degrees.
 *
 * @param[in] angle  Spin angle, which the indicator reads from its rotation (its own spin animation drives it).
 *
 * @return The pixel at (34,20), the centre of the leading dot at angle 0.
 */
static uint32_t spinner(float angle)
{
    ERNode* root = white_root();
    ERNode* spin = er_node_create(ER_NODE_ACTIVITY_INDICATOR);
    ERProps spin_props;
    er_props_default(&spin_props);
    spin_props.position = ER_POS_ABSOLUTE;
    spin_props.left = 0;
    spin_props.top = 0;
    spin_props.width = 40;
    spin_props.height = 40;
    spin_props.indicator_color = RED;
    spin_props.transform_rotate_z = angle;
    er_node_set_props(spin, &spin_props);

    er_tree_append_child(root, spin);
    er_tree_set_root(root);
    er_commit();
    const uint32_t pixel = s_framebuffer[20 * FRAMEBUFFER_WIDTH + 34];

    er_tree_remove_child(root, spin);
    er_node_destroy(spin);
    er_node_destroy(root);
    return pixel;
}

#if ERUI_TRANSFORMS_FULL
/**
 * @brief The props of a red 20×21 node at (20,20): `transform_props`'s transform, with position, size and color set.
 *
 * The height is odd so the default centre pivot sits on a half pixel: a node squashed to under a pixel still
 * straddles a row, so its one-row box reaches the sampler.
 *
 * @param[in] transform_props  Props carrying the transform.
 *
 * @return The node's props.
 */
static ERProps node_props(const ERProps* transform_props)
{
    ERProps test_node_props = *transform_props;
    test_node_props.position = ER_POS_ABSOLUTE;
    test_node_props.left = 20;
    test_node_props.top = 20;
    test_node_props.width = 20;
    test_node_props.height = 21;
    test_node_props.background_color = RED;
    return test_node_props;
}

/**
 * @brief Paints the node_props() node on a white root with the given transform props.
 *
 * @param[in] transform_props  Props carrying the transform.
 *
 * @return The pixel at (25,25), inside the node's layout box.
 */
static uint32_t paint(const ERProps* transform_props)
{
    ERNode* root = white_root();
    ERNode* node = er_node_create(ER_NODE_VIEW);
    const ERProps test_node_props = node_props(transform_props);
    er_node_set_props(node, &test_node_props);

    er_tree_append_child(root, node);
    er_tree_set_root(root);
    er_commit();
    const uint32_t pixel = s_framebuffer[25 * FRAMEBUFFER_WIDTH + 25];

    er_tree_remove_child(root, node);
    er_node_destroy(node);
    er_node_destroy(root);
    return pixel;
}

/**
 * @brief Paints the node scaled past every edge of the framebuffer, then drops the scale and commits again.
 *
 * The scaled box is wider than the int16 a node's last paint is recorded in, and the second commit erases the
 * node's trail from that record, so everything around the layout box has to come back white.
 *
 * @param[out] big  The pixel at (5,5) after the first commit, which the scaled node covers.
 *
 * @return The pixel at (5,5) after the second commit, outside the node's layout box.
 */
static uint32_t shrink_back(uint32_t* big)
{
    ERProps transform_props;
    er_props_default(&transform_props);
    transform_props.transform_scale_x = transform_props.transform_scale_y = 1e4f;
    ERNode* root = white_root();
    ERNode* node = er_node_create(ER_NODE_VIEW);
    ERProps test_node_props = node_props(&transform_props);
    er_node_set_props(node, &test_node_props);

    er_tree_append_child(root, node);
    er_tree_set_root(root);
    er_commit();
    *big = s_framebuffer[5 * FRAMEBUFFER_WIDTH + 5];

    er_props_default(&transform_props);
    test_node_props = node_props(&transform_props);
    er_node_set_props(node, &test_node_props);
    er_commit();
    const uint32_t pixel = s_framebuffer[5 * FRAMEBUFFER_WIDTH + 5];

    er_tree_remove_child(root, node);
    er_node_destroy(node);
    er_node_destroy(root);
    return pixel;
}

/**
 * @brief Paints the node scaled past every edge of the framebuffer, then moves it and commits again.
 *
 * Its box clips to the whole ±ER_PAINT_RECT_MAX window before and after, so the recorded paint alone cannot
 * show the move.
 *
 * @return How many backend draws the second commit made.
 */
static int move_zoomed(void)
{
    ERProps transform_props;
    er_props_default(&transform_props);
    transform_props.transform_scale_x = transform_props.transform_scale_y = 1e4f;
    ERNode* root = white_root();
    ERNode* node = er_node_create(ER_NODE_VIEW);
    ERProps test_node_props = node_props(&transform_props);
    er_node_set_props(node, &test_node_props);

    er_tree_append_child(root, node);
    er_tree_set_root(root);
    er_commit();

    test_node_props.left = 10;
    er_node_set_props(node, &test_node_props);
    s_draws = 0;
    er_commit();
    const int draws = s_draws;

    er_tree_remove_child(root, node);
    er_node_destroy(node);
    er_node_destroy(root);
    return draws;
}

/**
 * @brief Paints a rotated node too wide for the transform source beside a small one, then recolors the small
 *        one and reads back what that commit repainted.
 *
 * The wide node falls back to its raw box, which runs past ±ER_PAINT_RECT_MAX. Unless the pre-pass clips that
 * box as the recorded paint is clipped, the two never agree, so every commit that runs the pre-pass damages the
 * wide node again, whatever changed.
 *
 * @return The top edge of the second commit's dirty rect; the wide node spans rows 20 to 40.
 */
static int oversized_dirty_top(void)
{
    ERProps transform_props;
    er_props_default(&transform_props);
    transform_props.transform_rotate_z = 10.0f;
    ERNode* root = white_root();
    ERNode* wide = er_node_create(ER_NODE_VIEW);
    ERProps wide_props = node_props(&transform_props);
    wide_props.width = 20000;
    er_node_set_props(wide, &wide_props);

    ERNode* dot = er_node_create(ER_NODE_VIEW);
    ERProps dot_props;
    er_props_default(&dot_props);
    dot_props.position = ER_POS_ABSOLUTE;
    dot_props.left = 2;
    dot_props.top = 52;
    dot_props.width = 4;
    dot_props.height = 4;
    dot_props.background_color = RED;
    er_node_set_props(dot, &dot_props);

    er_tree_append_child(root, wide);
    er_tree_append_child(root, dot);
    er_tree_set_root(root);
    er_commit();

    dot_props.background_color = 0xFF0000FFU; /* blue */
    er_node_set_props(dot, &dot_props);
    er_commit();
    ERRect dirty = {0, 0, 0, 0};
    er_get_dirty_rect(&dirty);

    er_tree_remove_child(root, dot);
    er_tree_remove_child(root, wide);
    er_node_destroy(dot);
    er_node_destroy(wide);
    er_node_destroy(root);
    return dirty.y;
}
#endif /* ERUI_TRANSFORMS_FULL */

int main(void)
{
    EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, NULL};
    embedded_renderer_set_backend(&backend);

    int x, y, width, height;

    /* A box past the int range is clipped to ±16383 instead of an undefined cast: every screen fits inside it,
     * and so does its width in the int16 a node's last paint is recorded in. */
    er_transform_aabb(0, 0, 10, 10, 1e30f, 0.0f, 0.0f, 1e30f, 0.0f, 0.0f, &x, &y, &width, &height);
    if (x != 0 || y != 0 || width != 16383 || height != 16383)
        return fail("a box past the int range should clip to 16383");
    er_transform_aabb(0, 0, 10, 10, -1e30f, 0.0f, 0.0f, -1e30f, 0.0f, 0.0f, &x, &y, &width, &height);
    if (x != -16383 || y != -16383 || width != 16383 || height != 16383)
        return fail("a box below the int range should clip to -16383");
    er_transform_aabb(0, 0, 10, 10, 1e30f, 0.0f, 0.0f, 1e30f, -5e30f, -5e30f, &x, &y, &width, &height);
    if (x != -16383 || y != -16383 || width != 32766 || height != 32766)
        return fail("a box past both ends should still fit an int16");
    /* A NaN matrix leaves no corner to bound. */
    er_transform_aabb(0, 0, 10, 10, NAN, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, &x, &y, &width, &height);
    if (x != 0 || y != 0 || width != 0 || height != 0)
        return fail("a NaN matrix should give an empty box");

    /* A NaN or overflowed determinant has no usable inverse: singular, like a scale of 0. */
    float inverse_a, inverse_b, inverse_c, inverse_d, inverse_translate_x, inverse_translate_y;
    if (er_transform_invert(NAN,
                            0.0f,
                            0.0f,
                            1.0f,
                            0.0f,
                            0.0f,
                            &inverse_a,
                            &inverse_b,
                            &inverse_c,
                            &inverse_d,
                            &inverse_translate_x,
                            &inverse_translate_y))
        return fail("a NaN matrix should not invert");
    if (er_transform_invert(1e30f,
                            0.0f,
                            0.0f,
                            1e30f,
                            0.0f,
                            0.0f,
                            &inverse_a,
                            &inverse_b,
                            &inverse_c,
                            &inverse_d,
                            &inverse_translate_x,
                            &inverse_translate_y))
        return fail("a matrix whose determinant overflows should not invert");
    if (!er_transform_invert(2.0f,
                             0.0f,
                             0.0f,
                             2.0f,
                             0.0f,
                             0.0f,
                             &inverse_a,
                             &inverse_b,
                             &inverse_c,
                             &inverse_d,
                             &inverse_translate_x,
                             &inverse_translate_y))
        return fail("a plain 2x scale should still invert");
    /* A usable determinant says nothing of the translation, which a huge scale about the pivot overflows. */
    if (er_transform_invert(1.0f,
                            0.0f,
                            0.0f,
                            1.0f,
                            INFINITY,
                            0.0f,
                            &inverse_a,
                            &inverse_b,
                            &inverse_c,
                            &inverse_d,
                            &inverse_translate_x,
                            &inverse_translate_y))
        return fail("a matrix whose inverse translation overflows should not invert");

    /* A touch mapped past the int range clamps; one mapped to NaN has no position. */
    int local_x = 0, local_y = 0;
    if (!er_transform_map_point(1e10f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 100, 5, &local_x, &local_y) || local_x != 32767
        || local_y != 5)
        return fail("a point mapped past the int range should clamp to 32767");
    if (er_transform_map_point(1.0f, 0.0f, 0.0f, 1.0f, NAN, 0.0f, 100, 5, &local_x, &local_y))
        return fail("a point mapped to NaN should report no position");

#if ERUI_3D_TRANSFORMS
    /* W = 1e-30 at every corner: a projection that runs out to 1e31, clamped and clipped. */
    const float near_plane[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1e-30f};
    er_transform_aabb_3d(0, 0, 10, 10, near_plane, &x, &y, &width, &height);
    if (x != -1 || y != -1 || width != 16384 || height != 16384)
        return fail("a corner near the camera plane should clip its box to 16383");
    const float nan_homography[9] = {NAN, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    er_transform_aabb_3d(0, 0, 10, 10, nan_homography, &x, &y, &width, &height);
    if (x != 0 || y != 0 || width != 0 || height != 0)
        return fail("a NaN homography should give an empty box");

    float inverse[9];
    if (er_transform_homography_invert(nan_homography, inverse))
        return fail("a NaN homography should not invert");
    const float huge_homography[9] = {1e30f, 0.0f, 0.0f, 0.0f, 1e30f, 0.0f, 0.0f, 0.0f, 1.0f};
    if (er_transform_homography_invert(huge_homography, inverse))
        return fail("a homography whose determinant overflows should not invert");
    const float identity[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    if (!er_transform_homography_invert(identity, inverse))
        return fail("the identity should still invert");
    /* The determinant is 1, but the H[1] * H[5] cofactor, 1e40, overflows the inverse. */
    const float overflow_homography[9] = {1.0f, 1e20f, 0.0f, 0.0f, 1.0f, 1e20f, 0.0f, 0.0f, 1.0f};
    if (er_transform_homography_invert(overflow_homography, inverse))
        return fail("a homography whose inverse overflows should not invert");

    if (!er_transform_map_point_3d(near_plane, 10, 0, &local_x, &local_y) || local_x != 32767 || local_y != 0)
        return fail("a point back-projected past the int range should clamp to 32767");
    if (er_transform_map_point_3d(nan_homography, 10, 0, &local_x, &local_y))
        return fail("a point back-projected to NaN should report no position");
#endif /* ERUI_3D_TRANSFORMS */

    /* An ActivityIndicator places its dots straight from its spin angle, where an infinite one would make each
     * position NaN before its int cast. It is drawn as at 0 instead. */
    if (spinner(0.0f) != RED)
        return fail("the spinner's leading dot should sit at (34,20)");
    if (spinner(INFINITY) != RED || spinner(-INFINITY) != RED)
        return fail("an infinite spin angle should draw the spinner as at 0");

#if ERUI_TRANSFORMS_FULL
    ERProps transform_props;

    /* The determinant overflows (1e60): singular, so the node paints untransformed at its layout box. */
    er_props_default(&transform_props);
    transform_props.transform_scale_x = transform_props.transform_scale_y = 1e30f;
    if (paint(&transform_props) != RED)
        return fail("a scale of 1e30 should paint the untransformed box");

    /* Infinite scale or rotation: an infinite entry times a zero one makes the determinant NaN. */
    er_props_default(&transform_props);
    transform_props.transform_scale_x = transform_props.transform_scale_y = INFINITY;
    if (paint(&transform_props) != RED)
        return fail("an infinite scale should paint the untransformed box");
    er_props_default(&transform_props);
    transform_props.transform_rotate_z = INFINITY;
    if (paint(&transform_props) != RED)
        return fail("an infinite rotation should paint the untransformed box");

    /* Lopsided: the determinant is 1e9 * 1e-8 = 10, which inverts, but the box is 2e10 wide and the sampler
     * maps screen rows 1e8 source rows apart. The node is under a pixel tall, so none of its box is painted. */
    er_props_default(&transform_props);
    transform_props.transform_scale_x = 1e9f;
    transform_props.transform_scale_y = 1e-8f;
    if (paint(&transform_props) != WHITE)
        return fail("a node scaled under a pixel tall should not paint its box");

    /* Lopsided past the float range: the determinant is 1e38 * 1e-37 = 10, but the pivot's translation
     * overflows, and so does the inverse's. Singular, so the node paints untransformed. */
    er_props_default(&transform_props);
    transform_props.transform_scale_x = 1e38f;
    transform_props.transform_scale_y = 1e-37f;
    if (paint(&transform_props) != RED)
        return fail("a scale whose inverse overflows should paint the untransformed box");

    /* A box wider than an int16, then no transform: the second commit erases the whole scaled trail. */
    uint32_t big = 0U;
    const uint32_t after = shrink_back(&big);
    if (big != RED)
        return fail("a node scaled past every edge should cover the framebuffer");
    if (after != WHITE)
        return fail("a node shrinking back from past every edge should erase its whole trail");

    /* Scaled past every edge before and after a move, its box clips to the same record either way, so the
     * move counts regardless: the second commit repaints. */
    if (move_zoomed() == 0)
        return fail("a node scaled past every edge should repaint when it moves");

    /* Too wide for the transform source, a rotated node paints its raw box, which runs past the clip. A commit
     * that changes only another node leaves it alone. */
    if (oversized_dirty_top() <= 40)
        return fail("recoloring another node should not repaint a node whose raw box runs past the clip");

#if ERUI_3D_TRANSFORMS
    /* 3D with an overflowing scale, and with an infinite tilt: both singular, both untransformed. */
    er_props_default(&transform_props);
    transform_props.transform_rotate_x = 1.0f;
    transform_props.transform_scale_x = transform_props.transform_scale_y = 1e20f;
    if (paint(&transform_props) != RED)
        return fail("a 3D scale of 1e20 should paint the untransformed box");
    er_props_default(&transform_props);
    transform_props.transform_rotate_x = INFINITY;
    if (paint(&transform_props) != RED)
        return fail("an infinite 3D tilt should paint the untransformed box");
#endif /* ERUI_3D_TRANSFORMS */
#endif /* ERUI_TRANSFORMS_FULL */

    return EXIT_SUCCESS;
}
