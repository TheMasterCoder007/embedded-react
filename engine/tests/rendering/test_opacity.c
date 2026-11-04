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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FRAMEBUFFER_WIDTH 32
#define FRAMEBUFFER_HEIGHT 32
#define ALPHA_OPAQUE 255U
#define CHANNEL_MAX 255U
#define OPACITY_OPAQUE 255U

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Pixel framebuffer and call statistics gathered during a render pass.
 */
typedef struct
{
    uint32_t* framebuffer;  /**< Flat premultiplied ARGB8888 framebuffer; rows are framebuffer_width pixels wide. */
    int framebuffer_width;  /**< Framebuffer width in pixels. */
    int framebuffer_height; /**< Framebuffer height in pixels. */
    int fill_calls;         /**< Number of fill_rect calls received. */
    int blend_calls;        /**< Number of blend_rect calls received. */
} TestCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — backend stubs
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Backend fill_rect: premultiplies the color and writes it into the framebuffer.
 *
 * @param[in] argb  Straight-alpha ARGB8888 fill color.
 * @param[in] x     Left edge.
 * @param[in] y     Top edge.
 * @param[in] w     Width in pixels.
 * @param[in] h     Height in pixels.
 * @param[in] ctx   Pointer to TestCtx.
 */
static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    TestCtx* test_context = ctx;
    test_context->fill_calls++;

    const uint8_t alpha = (uint8_t)((argb >> 24) & 0xFFU);
    if (alpha == 0U)
        return;
    const uint8_t red = (uint8_t)((uint32_t)((argb >> 16) & 0xFFU) * alpha / CHANNEL_MAX);
    const uint8_t green = (uint8_t)((uint32_t)((argb >> 8) & 0xFFU) * alpha / CHANNEL_MAX);
    const uint8_t blue = (uint8_t)((uint32_t)(argb & 0xFFU) * alpha / CHANNEL_MAX);
    const uint32_t premultiplied = ((uint32_t)alpha << 24) | ((uint32_t)red << 16) | ((uint32_t)green << 8) | blue;

    for (int row = y; row < y + height; row++)
    {
        for (int column = x; column < x + width; column++)
        {
            if (column >= 0 && column < test_context->framebuffer_width && row >= 0
                && row < test_context->framebuffer_height)
                test_context->framebuffer[row * test_context->framebuffer_width + column] = premultiplied;
        }
    }
}

/**
 * @brief Backend copy_rect: copies premultiplied pixels directly into the framebuffer.
 *
 * @param[in] src     Source pixel buffer (premultiplied ARGB8888).
 * @param[in] stride  Source row stride in bytes.
 * @param[in] x       Destination left edge.
 * @param[in] y       Destination top edge.
 * @param[in] w       Width in pixels.
 * @param[in] h       Height in pixels.
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
 * @param[in] src     Source pixel buffer (premultiplied ARGB8888).
 * @param[in] stride  Source row stride in bytes.
 * @param[in] alpha   Global alpha scale 0–255.
 * @param[in] x       Destination left edge.
 * @param[in] y       Destination top edge.
 * @param[in] w       Width in pixels.
 * @param[in] h       Height in pixels.
 * @param[in] ctx     Pointer to TestCtx.
 */
static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    TestCtx* test_context = ctx;
    test_context->blend_calls++;

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

            /* Scale premultiplied source by global alpha. */
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
 * @param[in] msg  Human-readable description of the failed assertion.
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
 * @param[in] t  TestCtx owning the framebuffer.
 * @param[in] x  X coordinate.
 * @param[in] y  Y coordinate.
 *
 * @return Premultiplied ARGB8888 pixel value at (x, y).
 */
static uint32_t px(const TestCtx* test_context, int x, int y)
{
    return test_context->framebuffer[y * test_context->framebuffer_width + x];
}

/**
 * @brief Returns an ERProps with all int16_t layout fields set to ER_LAYOUT_AUTO.
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

/**
 * @brief Zeros the framebuffer and resets call statistics.
 *
 * @param[in,out] t  TestCtx to reset.
 */
static void reset(TestCtx* test_context)
{
    memset(test_context->framebuffer,
           0,
           (size_t)test_context->framebuffer_width * (size_t)test_context->framebuffer_height * sizeof(uint32_t));
    test_context->fill_calls = 0;
    test_context->blend_calls = 0;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Tests opacity compositing via the scene graph API.
 *
 * Verifies:
 *  - opacity=255 on a View does not use scratch (direct fill_rect).
 *  - opacity=0 on a View renders nothing into the framebuffer.
 *  - opacity=128 on a View composites correctly: red over white → expected pink pixel.
 *  - Nested opacity: inner opacity=128 red inside outer opacity=128 container over white.
 *  - opacity on a leaf View with no children composites the background fill.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    static uint32_t framebuffer[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];
    TestCtx test_context = {framebuffer, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT, 0, 0};
    EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, &test_context};
    embedded_renderer_set_backend(&backend);

    /* -----------------------------------------------------------------------
     * opacity=255: no scratch path, background fills directly via fill_rect.
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        root_props.background_color = 0xFF0000FFU; /* opaque blue */
        er_node_set_props(root, &root_props);
        er_tree_set_root(root);
        er_commit();

        /* The opaque fill must reach the backend directly — no blend call. */
        if (test_context.blend_calls != 0)
            return fail("opacity=255: blend_rect should not be called");
        /* Pixel should be premultiplied blue: A=255 R=0 G=0 B=255 → 0xFF0000FF. */
        if (px(&test_context, 0, 0) != 0xFF0000FFU)
            return fail("opacity=255: pixel should be blue");

        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * opacity=0: View renders nothing (scratch allocated but alpha=0 blend is a no-op).
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        root_props.background_color = 0x00000000U; /* transparent root */
        er_node_set_props(root, &root_props);

        ERNode* child = er_node_create(ER_NODE_VIEW);
        ERProps child_props = props_default();
        child_props.width = 8;
        child_props.height = 8;
        child_props.background_color = 0xFFFF0000U; /* red */
        child_props.opacity = 0U;
        er_node_set_props(child, &child_props);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* alpha=0 blend produces no visible change; framebuffer stays at zero. */
        if (px(&test_context, 0, 0) != 0x00000000U)
            return fail("opacity=0: pixel should remain transparent");

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * opacity=128: red (0xFFFF0000) View over white root.
     *
     * Expected compositing math:
     *   - Root fill: 0xFFFFFFFF (white, premultiplied) written via fill_rect.
     *   - Scratch render of child: scratch ← 0xFFFF0000 (premultiplied red, A=255).
     *   - er_scratch_pop_blend at alpha=128:
     *       backend blend_rect receives src=0xFFFF0000 premul, global_alpha=128.
     *       Backend scales: A = 255*128/255 = 128, R = 255*128/255 = 128, G=0, B=0
     *       → scaled premul = 0x80800000.
     *       Source-over onto dst=0xFFFFFFFF:
     *         inverse_alpha = 255 - 128 = 127
     *         out.A = 128 + 255*127/255 = 255
     *         out.R = 128 + 255*127/255 = 255
     *         out.G =   0 + 255*127/255 = 127
     *         out.B =   0 + 255*127/255 = 127
     *       → 0xFFFF7F7F.
     * ---------------------------------------------------------------------- */
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
        child_props.opacity = 128U;
        er_node_set_props(child, &child_props);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* Inside the child region: pink (red composited over white at 50%). */
        const uint32_t inside = px(&test_context, 0, 0);
        const uint8_t alpha = (uint8_t)((inside >> 24) & 0xFFU);
        const uint8_t red = (uint8_t)((inside >> 16) & 0xFFU);
        const uint8_t green = (uint8_t)((inside >> 8) & 0xFFU);
        const uint8_t blue = (uint8_t)(inside & 0xFFU);
        if (alpha != ALPHA_OPAQUE)
            return fail("opacity=128: alpha channel should be 255");
        if (red != 255U)
            return fail("opacity=128: red channel should be 255");
        if (green != 127U)
            return fail("opacity=128: green channel should be 127");
        if (blue != 127U)
            return fail("opacity=128: blue channel should be 127");

        /* Outside the child region: still white. */
        if (px(&test_context, 8, 8) != 0xFFFFFFFFU)
            return fail("opacity=128: pixel outside child should remain white");

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Leaf node opacity: a View with no children and opacity=128.
     * Same expected result as above: red at 128 over white → 0xFFFF7F7F.
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        root_props.background_color = 0xFFFFFFFFU; /* white */
        er_node_set_props(root, &root_props);

        ERNode* leaf = er_node_create(ER_NODE_VIEW);
        ERProps leaf_props = props_default();
        leaf_props.width = 4;
        leaf_props.height = 4;
        leaf_props.background_color = 0xFFFF0000U; /* red */
        leaf_props.opacity = 128U;
        er_node_set_props(leaf, &leaf_props);

        er_tree_append_child(root, leaf);
        er_tree_set_root(root);
        er_commit();

        const uint32_t pixel = px(&test_context, 0, 0);
        if ((uint8_t)((pixel >> 16) & 0xFFU) != 255U)
            return fail("leaf opacity=128: red channel should be 255");
        if ((uint8_t)((pixel >> 8) & 0xFFU) != 127U)
            return fail("leaf opacity=128: green channel should be 127");
        if ((uint8_t)(pixel & 0xFFU) != 127U)
            return fail("leaf opacity=128: blue channel should be 127");

        er_tree_remove_child(root, leaf);
        er_node_destroy(leaf);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Nested opacity: outer=128, inner=255 red, over white.
     *
     * Inner node renders fully opaque into the outer scratch slot.
     * Outer scratch blends at 128 → same result as the single-node case: 0xFFFF7F7F.
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        root_props.background_color = 0xFFFFFFFFU; /* white */
        er_node_set_props(root, &root_props);

        ERNode* outer = er_node_create(ER_NODE_VIEW);
        ERProps outer_props = props_default();
        outer_props.width = 8;
        outer_props.height = 8;
        outer_props.background_color = 0x00000000U; /* transparent container */
        outer_props.opacity = 128U;
        er_node_set_props(outer, &outer_props);

        ERNode* inner = er_node_create(ER_NODE_VIEW);
        ERProps inner_props = props_default();
        inner_props.width = 8;
        inner_props.height = 8;
        inner_props.background_color = 0xFFFF0000U; /* red, fully opaque */
        inner_props.opacity = OPACITY_OPAQUE;
        er_node_set_props(inner, &inner_props);

        er_tree_append_child(outer, inner);
        er_tree_append_child(root, outer);
        er_tree_set_root(root);
        er_commit();

        /* Outer container is transparent; inner fills it red at full opacity.
         * The combined scratch (pure red) is blended at 128 over white → 0xFFFF7F7F. */
        const uint32_t pixel = px(&test_context, 0, 0);
        if ((uint8_t)((pixel >> 16) & 0xFFU) != 255U)
            return fail("nested opacity: red channel should be 255");
        if ((uint8_t)((pixel >> 8) & 0xFFU) != 127U)
            return fail("nested opacity: green channel should be 127");
        if ((uint8_t)(pixel & 0xFFU) != 127U)
            return fail("nested opacity: blue channel should be 127");

        er_tree_remove_child(outer, inner);
        er_tree_remove_child(root, outer);
        er_node_destroy(inner);
        er_node_destroy(outer);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Banded compositing across strip seams: a tall translucent group with two
     * fully-overlapping opaque children.
     *
     * The 16×24 group is taller than a small ERUI_SCRATCH_BAND_HEIGHT strip, so it is
     * composited in multiple band passes. Group semantics require the composite
     * (blue — the topmost opaque child) to be blended ONCE at alpha 128 over
     * white: 0xFF7F7FFF on every row. Per-primitive alpha (red@128 then blue@128)
     * or a double blend at a band seam would produce different values, and any
     * seam artefact would break row-to-row equality.
     *
     * With the default full-height strip this degenerates to the single-pass
     * path and must produce the identical result.
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        root_props.background_color = 0xFFFFFFFFU; /* white */
        er_node_set_props(root, &root_props);

        ERNode* group = er_node_create(ER_NODE_VIEW);
        ERProps group_props = props_default();
        group_props.width = 16;
        group_props.height = 24;
        group_props.background_color = 0x00000000U; /* transparent container */
        group_props.opacity = 128U;
        er_node_set_props(group, &group_props);

        ERNode* red = er_node_create(ER_NODE_VIEW);
        ERProps red_props = props_default();
        red_props.position = 1; /* ER_POSITION_ABSOLUTE */
        red_props.left = 0;
        red_props.top = 0;
        red_props.width = 16;
        red_props.height = 24;
        red_props.background_color = 0xFFFF0000U; /* red, fully opaque */
        er_node_set_props(red, &red_props);

        ERNode* blue = er_node_create(ER_NODE_VIEW);
        ERProps blue_props = props_default();
        blue_props.position = 1; /* ER_POSITION_ABSOLUTE */
        blue_props.left = 0;
        blue_props.top = 0;
        blue_props.width = 16;
        blue_props.height = 24;
        blue_props.background_color = 0xFF0000FFU; /* blue, fully opaque, on top of red */
        er_node_set_props(blue, &blue_props);

        er_tree_append_child(group, red);
        er_tree_append_child(group, blue);
        er_tree_append_child(root, group);
        er_tree_set_root(root);
        er_commit();

        /* Expected: blue premul at alpha 128 (0x80000080) over white →
         * r = 0 + 255*127/255 = 127, g = 127, b = 128 + 255*127/255 = 255. */
        const uint32_t top_pixel = px(&test_context, 2, 2);
        if ((uint8_t)((top_pixel >> 16) & 0xFFU) != 127U)
            return fail("banded overlap: red channel should be 127 (group blend, not per-child)");
        if ((uint8_t)((top_pixel >> 8) & 0xFFU) != 127U)
            return fail("banded overlap: green channel should be 127");
        if ((uint8_t)(top_pixel & 0xFFU) != 255U)
            return fail("banded overlap: blue channel should be 255");

        /* Every row of the group must be identical — any band-seam double blend,
         * gap, or missed row shows up as a differing pixel. */
        for (int row = 0; row < 24; row++)
        {
            if (px(&test_context, 2, row) != top_pixel)
                return fail("banded overlap: rows must be identical across band seams");
        }
        /* Just outside the group: untouched white. */
        if (px(&test_context, 2, 24) != 0xFFFFFFFFU)
            return fail("banded overlap: pixel below the group should stay white");
        if (px(&test_context, 16, 2) != 0xFFFFFFFFU)
            return fail("banded overlap: pixel right of the group should stay white");

        er_tree_remove_child(group, red);
        er_tree_remove_child(group, blue);
        er_tree_remove_child(root, group);
        er_node_destroy(red);
        er_node_destroy(blue);
        er_node_destroy(group);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Scratch-pool exhaustion falls back to multiplied alpha.
     *
     * Five nested translucent containers (opacity 128 each) exceed
     * ERUI_MAX_OPACITY_DEPTH=4: the innermost group cannot get a scratch slot
     * and must fall back to multiplying its opacity into the child's draw.
     * The red leaf then carries alpha 128·(128/255)^4 = 8 when it reaches the
     * framebuffer → over white: r=255, g=b=247.
     *
     * The old fallback rendered the leaf at FULL opacity inside level 4's
     * composite → effective alpha 16 → g=b=239, so this check also guards
     * against regressing to opacity-dropping.
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        root_props.background_color = 0xFFFFFFFFU; /* white */
        er_node_set_props(root, &root_props);

        ERNode* level[5];
        for (int level_index = 0; level_index < 5; level_index++)
        {
            level[level_index] = er_node_create(ER_NODE_VIEW);
            ERProps level_props = props_default();
            level_props.width = 8;
            level_props.height = 8;
            level_props.background_color = 0x00000000U; /* transparent container */
            level_props.opacity = 128U;
            er_node_set_props(level[level_index], &level_props);
            if (level_index > 0)
                er_tree_append_child(level[level_index - 1], level[level_index]);
        }

        ERNode* leaf = er_node_create(ER_NODE_VIEW);
        ERProps leaf_props = props_default();
        leaf_props.width = 8;
        leaf_props.height = 8;
        leaf_props.background_color = 0xFFFF0000U; /* red, fully opaque */
        er_node_set_props(leaf, &leaf_props);
        er_tree_append_child(level[4], leaf);

        er_tree_append_child(root, level[0]);
        er_tree_set_root(root);
        er_commit();

        const uint32_t pixel = px(&test_context, 2, 2);
        const int green = (int)((pixel >> 8) & 0xFFU);
        if ((uint8_t)((pixel >> 16) & 0xFFU) != 255U)
            return fail("depth fallback: red channel should be 255");
        if (green < 245 || green > 249)
            return fail("depth fallback: 5th nesting level must still dim (multiplied-alpha fallback)");

        er_tree_remove_child(level[4], leaf);
        er_node_destroy(leaf);
        for (int level_index = 4; level_index > 0; level_index--)
        {
            er_tree_remove_child(level[level_index - 1], level[level_index]);
            er_node_destroy(level[level_index]);
        }
        er_tree_remove_child(root, level[0]);
        er_node_destroy(level[0]);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Fade cache: repeated alpha-only commits and content invalidation.
     *
     * A translucent group re-rendered because its bound opacity VALUE changed
     * (a visual-only mark) may be served from the fade cache in builds with
     * ERUI_FADE_CACHE_WIDTH and ERUI_FADE_CACHE_HEIGHT set; a content change (child recolored via props)
     * must invalidate it. The pixel expectations are identical with the cache
     * compiled out, so this test runs in every configuration — with the cache
     * on, it verifies the cached blend is bit-exact and invalidation works.
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        root_props.background_color = 0xFFFFFFFFU; /* white */
        er_node_set_props(root, &root_props);

        ERNode* group = er_node_create(ER_NODE_VIEW);
        ERProps group_props = props_default();
        group_props.width = 16;
        group_props.height = 16;
        group_props.background_color = 0x00000000U; /* transparent container */
        group_props.opacity = 128U;
        er_node_set_props(group, &group_props);

        ERNode* child = er_node_create(ER_NODE_VIEW);
        ERProps child_props = props_default();
        child_props.width = 16;
        child_props.height = 16;
        child_props.background_color = 0xFFFF0000U; /* red, fully opaque */
        er_node_set_props(child, &child_props);

        er_tree_append_child(group, child);
        er_tree_append_child(root, group);
        er_tree_set_root(root);
        er_commit(); /* first composite (capture in cache-enabled builds) */

        if (px(&test_context, 2, 2) != 0xFFFF7F7FU)
            return fail("fade cache: initial composite should be red at alpha 128");

        /* Drive opacity through an animated value: er_animation_value_set marks the group
         * visual-only, so cache-enabled builds serve the next commits from the cache. */
        ERAnimationValueHandle value = er_animation_value_create(0.25f);
        er_animation_value_bind(value, group, ER_PROP_OPACITY); /* bind-time apply → opacity 64 */
        er_commit();
        if (px(&test_context, 2, 2) != 0xFFFFBFBFU)
            return fail("fade cache: red at alpha 64 after value bind");

        er_animation_value_set(value, 0.5f); /* alpha-only change → cache hit when enabled */
        er_commit();
        if (px(&test_context, 2, 2) != 0xFFFF7F7FU)
            return fail("fade cache: red at alpha 128 after alpha-only value change");

        /* Content change mid-fade: recolor the child. Must invalidate the cache. */
        child_props.background_color = 0xFF0000FFU; /* blue */
        er_node_set_props(child, &child_props);
        er_commit();
        if (px(&test_context, 2, 2) != 0xFF7F7FFFU)
            return fail("fade cache: child recolor mid-fade must show blue (stale cache?)");

        er_animation_value_destroy(value);
        er_tree_remove_child(group, child);
        er_tree_remove_child(root, group);
        er_node_destroy(child);
        er_node_destroy(group);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Fade cache: a child's animated opacity inside a cached group. The child's change is
     * visual-only to itself but new content to the group's cache, which must not be reused.
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        root_props.background_color = 0xFFFFFFFFU;
        er_node_set_props(root, &root_props);

        ERNode* group = er_node_create(ER_NODE_VIEW);
        ERProps group_props = props_default();
        group_props.width = 16;
        group_props.height = 16;
        group_props.opacity = 128U;
        er_node_set_props(group, &group_props);

        ERNode* child = er_node_create(ER_NODE_VIEW);
        ERProps child_props = props_default();
        child_props.width = 16;
        child_props.height = 16;
        child_props.background_color = 0xFFFF0000U;
        er_node_set_props(child, &child_props);

        er_tree_append_child(group, child);
        er_tree_append_child(root, group);
        er_tree_set_root(root);

        ERAnimationValueHandle value = er_animation_value_create(1.0f);
        er_animation_value_bind(value, child, ER_PROP_OPACITY);
        er_commit();
        er_commit(); /* capture, in cache-enabled builds */
        if (px(&test_context, 2, 2) != 0xFFFF7F7FU)
            return fail("nested fade: red at the group alpha");

        er_animation_value_set(value, 0.0f);
        er_commit();
        if (px(&test_context, 2, 2) != 0xFFFFFFFFU)
            return fail("nested fade: a child faded out inside a cached group must disappear");

        er_animation_value_destroy(value);
        er_tree_remove_child(group, child);
        er_tree_remove_child(root, group);
        er_node_destroy(child);
        er_node_destroy(group);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Releasing every binding of a value repaints its nodes at their static props, even when the
     * setProps that restores them sees nothing new.
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        root_props.background_color = 0xFFFFFFFFU;
        er_node_set_props(root, &root_props);

        ERNode* box = er_node_create(ER_NODE_VIEW);
        ERProps box_props = props_default();
        box_props.width = 8;
        box_props.height = 8;
        box_props.background_color = 0xFFFF0000U;
        er_node_set_props(box, &box_props);
        er_tree_append_child(root, box);
        er_tree_set_root(root);

        ERAnimationValueHandle value = er_animation_value_create(1.0f);
        const ERColorInterpolation color_interpolation = {
            {0.0f, 1.0f}, {0xFF000000U, 0xFF0000FFU}, 2, ER_EXTRAPOLATE_CLAMP, ER_EXTRAPOLATE_CLAMP};
        er_animation_value_bind_color(value, box, ER_PROP_BACKGROUND_COLOR, &color_interpolation);
        er_commit();
        if (px(&test_context, 2, 2) != 0xFF0000FFU)
            return fail("unbind all: the bound color should paint");

        er_animation_value_unbind_all(value);
        er_node_set_props(box, &box_props);
        er_commit();
        if (px(&test_context, 2, 2) != 0xFFFF0000U)
            return fail("unbind all: the static color should repaint");

        er_animation_value_destroy(value);
        er_tree_remove_child(root, box);
        er_node_destroy(box);
        er_node_destroy(root);
    }

#if ERUI_TRANSFORMS_FULL
    /* -----------------------------------------------------------------------
     * Transform inside a banded opacity group.
     *
     * A rotated child inside a tall translucent group is the subtle banded case:
     * its transform-source capture must stay COMPLETE during every band pass
     * (band tiles are not clip rects), while the transformed output lands only
     * in the strip being composited. A 90° rotation of an 8×8 red square about
     * its centre reproduces the same square, so the result must match an
     * unrotated red child blended at the group alpha: ~0xFFFF7F7F on every row,
     * with no gaps at band seams.
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        root_props.background_color = 0xFFFFFFFFU; /* white */
        er_node_set_props(root, &root_props);

        ERNode* group = er_node_create(ER_NODE_VIEW);
        ERProps group_props = props_default();
        group_props.width = 16;
        group_props.height = 24;
        group_props.background_color = 0x00000000U; /* transparent container */
        group_props.opacity = 128U;
        er_node_set_props(group, &group_props);

        ERNode* card = er_node_create(ER_NODE_VIEW);
        ERProps card_props = props_default();
        card_props.position = 1; /* ER_POSITION_ABSOLUTE */
        card_props.left = 4;
        card_props.top = 8; /* spans band seams for any small ERUI_SCRATCH_BAND_HEIGHT */
        card_props.width = 8;
        card_props.height = 8;
        card_props.background_color = 0xFFFF0000U; /* red, fully opaque */
        card_props.transform_rotate_z = 90.0f;
        card_props.transform_origin_x = -1.0f; /* centre pivot: square maps onto itself */
        card_props.transform_origin_y = -1.0f;
        er_node_set_props(card, &card_props);

        er_tree_append_child(group, card);
        er_tree_append_child(root, group);
        er_tree_set_root(root);
        er_commit();

        /* Interior of the rotated square, straddling seam rows: red at group alpha
         * over white ≈ (255,127,127). Allow ±2 for bilinear float jitter at 90°. */
        const int probe_x = 8;
        const int probe_rows[3] = {10, 12, 13};
        for (int probe_index = 0; probe_index < 3; probe_index++)
        {
            const uint32_t pixel = px(&test_context, probe_x, probe_rows[probe_index]);
            const int red = (int)((pixel >> 16) & 0xFFU);
            const int green = (int)((pixel >> 8) & 0xFFU);
            const int blue = (int)(pixel & 0xFFU);
            if (red < 253 || green < 125 || green > 129 || blue < 125 || blue > 129)
                return fail("banded transform: interior of rotated child should be red at group alpha on all rows");
        }
        /* Outside the group: untouched white. */
        if (px(&test_context, 2, 25) != 0xFFFFFFFFU)
            return fail("banded transform: pixel below the group should stay white");

        er_tree_remove_child(group, card);
        er_tree_remove_child(root, group);
        er_node_destroy(card);
        er_node_destroy(group);
        er_node_destroy(root);
    }
#endif /* ERUI_TRANSFORMS_FULL */

    /* -----------------------------------------------------------------------
     * Image and Text opacity: each is one leaf draw, faded at a draw alpha. A red image at 128 over
     * white lands on the same pink as a View at 128; a Text at 0 paints nothing.
     * ---------------------------------------------------------------------- */
    reset(&test_context);
    {
        er_reset();
        static const uint32_t red_4x4[16] = {
            0xFFFF0000U,
            0xFFFF0000U,
            0xFFFF0000U,
            0xFFFF0000U,
            0xFFFF0000U,
            0xFFFF0000U,
            0xFFFF0000U,
            0xFFFF0000U,
            0xFFFF0000U,
            0xFFFF0000U,
            0xFFFF0000U,
            0xFFFF0000U,
            0xFFFF0000U,
            0xFFFF0000U,
            0xFFFF0000U,
            0xFFFF0000U,
        };
        er_image_load("red4x4", red_4x4, 4, 4);

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        root_props.background_color = 0xFFFFFFFFU;
        er_node_set_props(root, &root_props);

        ERNode* image = er_node_create(ER_NODE_IMAGE);
        ERProps image_props = props_default();
        image_props.width = 4;
        image_props.height = 4;
        strncpy(image_props.image_name, "red4x4", ER_IMAGE_NAME_MAX);
        image_props.resize_mode = ER_RESIZE_STRETCH;
        image_props.opacity = 128U;
        er_node_set_props(image, &image_props);

        ERNode* text_node = er_node_create(ER_NODE_TEXT);
        ERProps text_props = props_default();
        text_props.position = ER_POSITION_ABSOLUTE;
        text_props.left = 0;
        text_props.top = 8;
        text_props.width = FRAMEBUFFER_WIDTH;
        text_props.height = 20;
        strncpy(text_props.text, "MMMM", ER_TEXT_MAX);
        text_props.color = 0xFF000000U;
        text_props.font_size = 16;
        text_props.opacity = 0U;
        er_node_set_props(text_node, &text_props);

        er_tree_append_child(root, image);
        er_tree_append_child(root, text_node);
        er_tree_set_root(root);
        er_commit();

        const uint32_t pixel = px(&test_context, 1, 1);
        if (((pixel >> 16) & 0xFFU) != 0xFFU || ((pixel >> 8) & 0xFFU) != 0x7FU || (pixel & 0xFFU) != 0x7FU)
            return fail("image opacity=128: red over white should be pink");
        for (int y = 8; y < 28; y++)
            for (int x = 0; x < FRAMEBUFFER_WIDTH; x++)
                if (px(&test_context, x, y) != 0xFFFFFFFFU)
                    return fail("text opacity=0: the text should paint nothing");

        /* The same text, opaque, does paint: the check above is not passing on an empty draw. */
        text_props.opacity = OPACITY_OPAQUE;
        er_node_set_props(text_node, &text_props);
        er_commit();
        bool inked = false;
        for (int y = 8; y < 28 && !inked; y++)
            for (int x = 0; x < FRAMEBUFFER_WIDTH && !inked; x++)
                inked = px(&test_context, x, y) != 0xFFFFFFFFU;
        if (!inked)
            return fail("text opacity=255: the text should paint");

        /* An animated color that reaches transparent black draws nothing, not the unset-color white. A
         * blue backdrop tells the two apart. */
        root_props.background_color = 0xFF0000FFU;
        er_node_set_props(root, &root_props);
        ERAnimationValueHandle color_value = er_animation_value_create(0.0f);
        const ERColorInterpolation color_interpolation = {
            {0.0f, 1.0f}, {0x00000000U, 0xFF000000U}, 2, ER_EXTRAPOLATE_CLAMP, ER_EXTRAPOLATE_CLAMP};
        er_animation_value_bind_color(color_value, text_node, ER_PROP_COLOR, &color_interpolation);
        er_commit();
        for (int y = 8; y < 28; y++)
            for (int x = 0; x < FRAMEBUFFER_WIDTH; x++)
                if (px(&test_context, x, y) != 0xFF0000FFU)
                    return fail("text color: an animated transparent color should paint nothing");
        er_animation_value_destroy(color_value);

        er_tree_remove_child(root, text_node);
        er_tree_remove_child(root, image);
        er_node_destroy(text_node);
        er_node_destroy(image);
        er_node_destroy(root);
    }

    return EXIT_SUCCESS;
}
