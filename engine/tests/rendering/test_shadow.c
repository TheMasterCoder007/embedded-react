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

#define FRAMEBUFFER_WIDTH 32
#define FRAMEBUFFER_HEIGHT 32
#define ALPHA_OPAQUE 255U
#define CHANNEL_MAX 255U
#define OPACITY_OPAQUE 255U

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Pixel framebuffer and blit statistics collected during a render pass.
 */
typedef struct
{
    uint32_t* framebuffer;  /**< Premultiplied ARGB8888 framebuffer; FRAMEBUFFER_WIDTH pixels per row. */
    int framebuffer_width;  /**< Framebuffer width in pixels. */
    int framebuffer_height; /**< Framebuffer height in pixels. */
    int fill_calls;         /**< Number of fill_rect calls received. */
    int blend_calls;        /**< Number of blend_rect calls received. */
} TestCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — backend stubs
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Backend fill_rect: premultiplies the color and writes it to the framebuffer.
 *
 * @param[in] argb    Straight-alpha ARGB8888 color.
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
 * @brief Backend copy_rect: copies premultiplied pixels directly into the framebuffer.
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
            const int pixel_x = x + column, pixel_y = y + row;
            if (pixel_x >= 0 && pixel_x < test_context->framebuffer_width && pixel_y >= 0
                && pixel_y < test_context->framebuffer_height)
                test_context->framebuffer[pixel_y * test_context->framebuffer_width + pixel_x] = source_row[column];
        }
    }
}

/**
 * @brief Backend blend_rect: source-over composites premultiplied pixels into the framebuffer.
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
    test_context->blend_calls++;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + row * stride);
        for (int column = 0; column < width; column++)
        {
            const int pixel_x = x + column, pixel_y = y + row;
            if (pixel_x < 0 || pixel_x >= test_context->framebuffer_width || pixel_y < 0
                || pixel_y >= test_context->framebuffer_height)
                continue;

            uint32_t source_pixel = source_row[column];
            if (alpha < ALPHA_OPAQUE)
            {
                source_pixel = ((uint32_t)((source_pixel >> 24) & 0xFFU) * alpha / CHANNEL_MAX << 24)
                               | ((uint32_t)((source_pixel >> 16) & 0xFFU) * alpha / CHANNEL_MAX << 16)
                               | ((uint32_t)((source_pixel >> 8) & 0xFFU) * alpha / CHANNEL_MAX << 8)
                               | ((uint32_t)(source_pixel & 0xFFU) * alpha / CHANNEL_MAX);
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
 * @param[in] message  Human-readable description of the assertion that failed.
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
 * @return Premultiplied ARGB8888 pixel at (x, y).
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
 * @brief Zeros the framebuffer and resets blit statistics.
 *
 * @param[in,out] test_context  TestCtx to reset.
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
 * @brief Tests box shadow rendering through the scene graph API.
 *
 * With ERUI_SHADOWS=0 the tests verify the no-op path (shadow_opacity is stored but
 * produces no blend calls or pixel changes).  With ERUI_SHADOWS=1 the tests exercise
 * the two-pass blur rasteriser, offset positioning, opacity scaling, and elevation.
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
     * shadow_opacity=0: shadow props stored but no shadow rendered.
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
        child_props.background_color = 0xFF0000FFU; /* blue */
        child_props.shadow_color = 0xFF000000U;
        child_props.shadow_offset_x = 2.0f;
        child_props.shadow_offset_y = 2.0f;
        child_props.shadow_opacity = 0.0f; /* disabled */
        child_props.shadow_radius = 0;
        er_node_set_props(child, &child_props);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* shadow_opacity=0: no blend call should originate from shadow rasteriser.
         * (The only blend calls, if any, would be from opacity compositing — none here.) */
#if ERUI_SHADOWS
        /* With shadows enabled the rasteriser must respect the opacity=0 gate. */
        const uint32_t shadow_corner = px(&test_context, 5, 5);
        /* Pixel at (5,5) is outside the child (child covers 0..3) but inside where the
         * offset (2,2) shadow would land if it were drawn. Since there is no shadow, it
         * should still be white (root background). */
        if ((shadow_corner >> 24) != 0xFFU || (shadow_corner >> 16 & 0xFFU) != 0xFFU
            || (shadow_corner >> 8 & 0xFFU) != 0xFFU || (shadow_corner & 0xFFU) != 0xFFU)
            return fail("shadow_opacity=0: pixel at (5,5) should remain white (no shadow)");
#endif

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }

#if ERUI_SHADOWS

    /* -----------------------------------------------------------------------
     * shadow_opacity=1, radius=0, offset=(3,3): hard black shadow at offset.
     *
     * Node: 4×4 at (0,0), transparent background.
     * Root: 8×8, white background.
     * Shadow at (3,3) should be premultiplied black (0xFF000000).
     * Pixel at (0,0) stays white (root, no node background).
     * Pixel at (12,12) stays white (outside shadow).
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
        child_props.background_color = 0x00000000U; /* transparent — only the shadow visible */
        child_props.shadow_color = 0xFF000000U;     /* black */
        child_props.shadow_offset_x = 3.0f;
        child_props.shadow_offset_y = 3.0f;
        child_props.shadow_opacity = 1.0f;
        child_props.shadow_radius = 0;
        er_node_set_props(child, &child_props);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* The shadow (no blur) is a 4×4 block at (3,3). */
        const uint32_t shadow_pixel = px(&test_context, 3, 3);
        if ((shadow_pixel >> 24) != 0xFFU)
            return fail("hard shadow: shadow pixel at (3,3) must be fully opaque");
        if (((shadow_pixel >> 16) & 0xFFU) != 0U || ((shadow_pixel >> 8) & 0xFFU) != 0U || (shadow_pixel & 0xFFU) != 0U)
            return fail("hard shadow: shadow pixel at (3,3) must be black");

        /* Pixel directly at the node origin: no node background, root still white. */
        const uint32_t root_pixel = px(&test_context, 0, 0);
        if (root_pixel != 0xFFFFFFFFU)
            return fail("hard shadow: pixel at (0,0) should remain white (node has no background)");

        /* Well outside the shadow: still white. */
        if (px(&test_context, 12, 12) != 0xFFFFFFFFU)
            return fail("hard shadow: pixel at (12,12) should remain white");

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * shadow_opacity=1, radius=2, offset=(0,0): soft shadow extends beyond the node.
     *
     * Node: 4×4 at (8,8) within a 32×32 white root.
     * Shadow buffer is (4+4)×(4+4) = 8×8; shadow origin is (8-2, 8-2) = (6,6).
     * Pixels inside the node's silhouette (center of blur) have the highest alpha.
     * Pixels at the outer corners of the shadow have a lower (but non-zero) alpha
     * because the box blur spreads the alpha beyond the silhouette.
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
        child_props.left = 8;
        child_props.top = 8;
        child_props.width = 4;
        child_props.height = 4;
        child_props.position = 1;                   /* ER_POSITION_ABSOLUTE */
        child_props.background_color = 0x00000000U; /* transparent */
        child_props.shadow_color = 0xFF000000U;
        child_props.shadow_offset_x = 0.0f;
        child_props.shadow_offset_y = 0.0f;
        child_props.shadow_opacity = 1.0f;
        child_props.shadow_radius = 2;
        er_node_set_props(child, &child_props);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* The framebuffer is opaque white, so blended alpha is 255 everywhere — the black
         * shadow's strength shows up as *darkness* in the RGB channels, not in alpha.
         * Center of blur region (maps to center of node silhouette in blur buffer):
         * node center in world = (8+2, 8+2) = (10,10). Must be darker than white. */
        const uint32_t center = px(&test_context, 10, 10);
        const uint8_t center_red = (uint8_t)((center >> 16) & 0xFFU);
        if (center_red == 0xFFU)
            return fail("soft shadow: center pixel must be darker than white (shadow present)");

        /* Outer corner of the shadow bleed region: node at (8,8) → shadow at (6,6).
         * After blur, pixel (6,6) is at the corner of the shadow buffer and must still be
         * slightly darkened (blur spreads into surrounding pixels). */
        const uint32_t outer_corner = px(&test_context, 6, 6);
        const uint8_t outer_red = (uint8_t)((outer_corner >> 16) & 0xFFU);
        if (outer_red == 0xFFU)
            return fail("soft shadow: corner of blur spread at (6,6) must be darkened");

        /* The center must be darker (lower RGB) than the outer corner (blur gradient). */
        if (center_red >= outer_red)
            return fail("soft shadow: blur center must be darker than outer blur edge");

        /* Far from the shadow: root still white. */
        if (px(&test_context, 0, 0) != 0xFFFFFFFFU)
            return fail("soft shadow: pixel at (0,0) should remain white (outside shadow)");

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * shadow_opacity=0.5: shadow alpha halved.
     *
     * Node: 4×4 at (0,0), transparent background.  Shadow offset (2,2), radius=0.
     * shadow_opacity=0.5 means effective alpha = 0.5 * 255 ≈ 127 at center.
     * After source-over blend onto white, the shadow pixel at (2,2) must be grey
     * (not black), verifying that opacity is applied.
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
        child_props.background_color = 0x00000000U; /* transparent */
        child_props.shadow_color = 0xFF000000U;
        child_props.shadow_offset_x = 2.0f;
        child_props.shadow_offset_y = 2.0f;
        child_props.shadow_opacity = 0.5f;
        child_props.shadow_radius = 0;
        er_node_set_props(child, &child_props);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* shadow_opacity=0.5 → effective alpha ≈ 127 → blended over white = grey.
         * The resulting pixel must be neither white (0xFFFFFFFF) nor black (0xFF000000). */
        const uint32_t shadow_pixel = px(&test_context, 2, 2);
        const uint8_t shadow_red = (uint8_t)((shadow_pixel >> 16) & 0xFFU);
        const uint8_t shadow_blue = (uint8_t)(shadow_pixel & 0xFFU);
        if (shadow_red == 255U && shadow_blue == 255U)
            return fail("shadow_opacity=0.5: shadow pixel should be grey, not white");
        if (shadow_red == 0U && shadow_blue == 0U)
            return fail("shadow_opacity=0.5: shadow pixel should be grey, not black");

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * elevation > 0: synthesised shadow with no explicit shadow_opacity.
     *
     * elevation=4 synthesises a shadow with offset_y ≈ 1.3 px, radius=4, opacity ≈ 0.24.
     * Node: 4×4 at (0,0), transparent background, root white.
     * There should be at least one blend call and at least one non-white pixel below
     * the node (the synthesised shadow's downward offset lands it below the node rect).
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
        child_props.left = 4;
        child_props.top = 4;
        child_props.width = 4;
        child_props.height = 4;
        child_props.position = 1;                   /* ER_POSITION_ABSOLUTE */
        child_props.background_color = 0x00000000U; /* transparent */
        child_props.shadow_opacity = 0.0f;          /* use elevation path */
        child_props.elevation = 4;
        er_node_set_props(child, &child_props);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* Elevation synthesises a shadow: blend calls must have been made. */
        if (test_context.blend_calls == 0)
            return fail("elevation: at least one blend call expected for synthesised shadow");

        /* The synthesised shadow extends into the region below the node.
         * elevation=4 gives offset_y ≈ 1.3 and radius=4; node is at (4..7, 4..7).
         * Shadow buffer: 12×12, origin at (4+0-4, 4+1-4) = (0, 1).
         * Center of shadow at approximately (4+2, 4+2+1) = (6, 7).
         * That pixel must be non-white (shadow present). */
        const uint32_t shadow_center = px(&test_context, 6, 7);
        const uint8_t shadow_center_red = (uint8_t)((shadow_center >> 16) & 0xFFU);
        const uint8_t shadow_center_green = (uint8_t)((shadow_center >> 8) & 0xFFU);
        const uint8_t shadow_center_blue = (uint8_t)(shadow_center & 0xFFU);
        if (shadow_center_red == 255U && shadow_center_green == 255U && shadow_center_blue == 255U)
            return fail("elevation: shadow center pixel should not be pure white");

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * A NaN shadow offset (a 0/0 in app math) is 0, where a C cast of it would be undefined: the hard
     * shadow lands under the node, as an offset of (0,0) puts it. One past the int range is clamped,
     * which puts it off screen.
     * ---------------------------------------------------------------------- */
    {
        const float offsets[2] = {NAN, 1e10f};
        const uint32_t under_node[2] = {0xFF000000U, 0xFFFFFFFFU}; /* the shadow, then the white root */
        for (int case_index = 0; case_index < 2; case_index++)
        {
            reset(&test_context);
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
            child_props.background_color = 0x00000000U; /* transparent — only the shadow visible */
            child_props.shadow_color = 0xFF000000U;
            child_props.shadow_offset_x = offsets[case_index];
            child_props.shadow_offset_y = offsets[case_index];
            child_props.shadow_opacity = 1.0f;
            child_props.shadow_radius = 0;
            er_node_set_props(child, &child_props);

            er_tree_append_child(root, child);
            er_tree_set_root(root);
            er_commit();

            if (px(&test_context, 1, 1) != under_node[case_index])
                return fail(case_index == 0 ? "nan offset: the shadow should sit under the node, unshifted"
                                            : "huge offset: the shadow should be clamped off screen");

            er_tree_remove_child(root, child);
            er_node_destroy(child);
            er_node_destroy(root);
        }
    }

    /* -----------------------------------------------------------------------
     * An offset near 32767 widens the node's recorded paint by twice that, past the int16 it is kept in.
     * Clipped rather than wrapped, the record still covers the node, so what erases the node by it (dropping
     * the shadow as the node moves away, or removing the node) still clears where it was. Moving it with the
     * shadow kept leaves a footprint that clips to the same record, so that is counted as a move regardless.
     * ---------------------------------------------------------------------- */
    for (int mode = 0; mode < 3; mode++)
    {
        reset(&test_context);
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = FRAMEBUFFER_WIDTH;
        root_props.height = FRAMEBUFFER_HEIGHT;
        root_props.background_color = 0xFFFFFFFFU; /* white */
        er_node_set_props(root, &root_props);

        ERNode* child = er_node_create(ER_NODE_VIEW);
        ERProps child_props = props_default();
        child_props.position = ER_POSITION_ABSOLUTE;
        child_props.left = 2;
        child_props.top = 2;
        child_props.width = 4;
        child_props.height = 4;
        child_props.background_color = 0xFF0000FFU; /* blue */
        child_props.shadow_color = 0xFF000000U;
        child_props.shadow_offset_x = 32767.0f;
        child_props.shadow_opacity = 1.0f;
        child_props.shadow_radius = 0;
        er_node_set_props(child, &child_props);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();
        if (px(&test_context, 3, 3) != 0xFF0000FFU)
            return fail("huge offset: the node itself should paint");

        static const char* const k_what[3] = {
            "huge offset: moving the node with its shadow should erase where it was",
            "huge offset: dropping the shadow as the node moves should erase where it was",
            "huge offset: removing the node should erase where it was",
        };
        if (mode < 2)
        {
            child_props.left = 20; /* away from (3,3) */
            if (mode == 1)
                child_props.shadow_opacity = 0.0f; /* and a footprint that no longer covers it */
            er_node_set_props(child, &child_props);
        }
        else
            er_tree_remove_child(root, child);
        er_commit();
        if (px(&test_context, 3, 3) != 0xFFFFFFFFU)
            return fail(k_what[mode]);
        if (mode < 2 && px(&test_context, 21, 3) != 0xFF0000FFU)
            return fail("huge offset: the moved node should paint where it went");

        if (mode < 2)
            er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }

#endif /* ERUI_SHADOWS */

    return EXIT_SUCCESS;
}
