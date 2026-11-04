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
 * A node moved by a native-driver transform animation must keep painting, frame by frame, on a
 * damage-clipped host.
 *
 * The bug this guards: the compositor exempts transformed subtrees from its prune-by-cached-bounds,
 * off a flag the layout pass caches — and an animation-only frame runs no layout pass. A transform
 * that appeared after the last layout left the node prunable, so the damage clip chasing it across
 * the screen pruned it away and the node went blank.
 *
 * Every frame is compared against a full repaint of the same scene state — the same invariant
 * test_opacity_equiv.c asserts for static prop mutations, here for an animated transform.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FRAMEBUFFER_WIDTH 320
#define FRAMEBUFFER_HEIGHT 120
#define OPACITY_OPAQUE 255U

#define ANIMATION_MS 480 /* animation duration */
#define FRAME_MS 16      /* per-tick advance */
#define FRAMES 40        /* ticks driven (runs past the end so the settled frames are checked too) */

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Framebuffer the backend stubs rasterise into.
 */
typedef struct
{
    uint32_t* framebuffer;  /**< Flat ARGB8888 framebuffer. */
    int framebuffer_width;  /**< Width in pixels. */
    int framebuffer_height; /**< Height in pixels. */
} TestCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — backend stubs
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Rounds a 0–65025 product back to 0–255 the way the engine's blenders do. */
static uint32_t div255(uint32_t value)
{
    return (value + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX;
}

/**
 * @brief Backend fill_rect: source-over composites a straight-alpha colour into the framebuffer.
 *
 * @param[in] argb    Straight-alpha ARGB8888 fill colour.
 * @param[in] x       Left edge.
 * @param[in] y       Top edge.
 * @param[in] width   Width in pixels.
 * @param[in] height  Height in pixels.
 * @param[in] ctx     Pointer to TestCtx.
 */
static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    TestCtx* test_context = ctx;
    const uint32_t alpha = (argb >> 24) & 0xFFU;
    if (alpha == 0U || width <= 0 || height <= 0)
        return;
    const uint32_t inverse_alpha = ER_CHANNEL_MAX - alpha;
    const uint32_t source_red = (argb >> 16) & 0xFFU, source_green = (argb >> 8) & 0xFFU, source_blue = argb & 0xFFU;

    for (int row = y; row < y + height; row++)
    {
        if (row < 0 || row >= test_context->framebuffer_height)
            continue;
        for (int column = x; column < x + width; column++)
        {
            if (column < 0 || column >= test_context->framebuffer_width)
                continue;
            uint32_t* destination = &test_context->framebuffer[row * test_context->framebuffer_width + column];
            const uint32_t destination_red = (*destination >> 16) & 0xFFU,
                           destination_green = (*destination >> 8) & 0xFFU, destination_blue = *destination & 0xFFU;
            *destination = 0xFF000000U | ((div255(source_red * alpha) + div255(destination_red * inverse_alpha)) << 16)
                           | ((div255(source_green * alpha) + div255(destination_green * inverse_alpha)) << 8)
                           | (div255(source_blue * alpha) + div255(destination_blue * inverse_alpha));
        }
    }
}

/**
 * @brief Backend copy_rect: source-over composites a premultiplied buffer into the framebuffer.
 *
 * Fully transparent source pixels are skipped and the rest blended, matching the engine's own
 * scratch copy — an unconditional overwrite would erase the background under antialiased edges and
 * make the comparison depend on repaint history rather than on scene state.
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
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + (size_t)row * (size_t)stride);
        for (int column = 0; column < width; column++)
        {
            const int destination_x = x + column, destination_y = y + row;
            if (destination_x < 0 || destination_x >= test_context->framebuffer_width || destination_y < 0
                || destination_y >= test_context->framebuffer_height)
                continue;
            const uint32_t source_pixel = source_row[column];
            const uint32_t source_alpha = (source_pixel >> 24) & 0xFFU;
            if (source_alpha == 0U)
                continue;
            uint32_t* destination =
                &test_context->framebuffer[destination_y * test_context->framebuffer_width + destination_x];
            if (source_alpha == 0xFFU)
            {
                *destination = source_pixel;
                continue;
            }
            const uint32_t inverse_alpha = ER_CHANNEL_MAX - source_alpha;
            const uint32_t destination_red = (*destination >> 16) & 0xFFU,
                           destination_green = (*destination >> 8) & 0xFFU, destination_blue = *destination & 0xFFU;
            *destination = 0xFF000000U
                           | ((((source_pixel >> 16) & 0xFFU) + div255(destination_red * inverse_alpha)) << 16)
                           | ((((source_pixel >> 8) & 0xFFU) + div255(destination_green * inverse_alpha)) << 8)
                           | ((source_pixel & 0xFFU) + div255(destination_blue * inverse_alpha));
        }
    }
}

/**
 * @brief Backend blend_rect: source-over composites premultiplied pixels scaled by a global alpha.
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
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + (size_t)row * (size_t)stride);
        for (int column = 0; column < width; column++)
        {
            const int destination_x = x + column, destination_y = y + row;
            if (destination_x < 0 || destination_x >= test_context->framebuffer_width || destination_y < 0
                || destination_y >= test_context->framebuffer_height)
                continue;
            const uint32_t source_pixel = source_row[column];
            const uint32_t source_alpha = div255(((source_pixel >> 24) & 0xFFU) * (uint32_t)alpha);
            if (source_alpha == 0U)
                continue;
            const uint32_t inverse_alpha = ER_CHANNEL_MAX - source_alpha;
            /* Source is premultiplied: scale its colour by the same global alpha, not by source_alpha. */
            const uint32_t source_red = div255(((source_pixel >> 16) & 0xFFU) * (uint32_t)alpha);
            const uint32_t source_green = div255(((source_pixel >> 8) & 0xFFU) * (uint32_t)alpha);
            const uint32_t source_blue = div255((source_pixel & 0xFFU) * (uint32_t)alpha);
            uint32_t* destination =
                &test_context->framebuffer[destination_y * test_context->framebuffer_width + destination_x];
            const uint32_t destination_red = (*destination >> 16) & 0xFFU,
                           destination_green = (*destination >> 8) & 0xFFU, destination_blue = *destination & 0xFFU;
            *destination = 0xFF000000U | ((source_red + div255(destination_red * inverse_alpha)) << 16)
                           | ((source_green + div255(destination_green * inverse_alpha)) << 8)
                           | (source_blue + div255(destination_blue * inverse_alpha));
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Prints a failure message.
 *
 * @param[in] message  Message describing the failed assertion.
 *
 * @return EXIT_FAILURE, so callers can `return fail(...)`.
 */
static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return EXIT_FAILURE;
}

/**
 * @brief Returns an ERProps with every int16_t layout field set to ER_LAYOUT_AUTO.
 *
 * @return Initialised ERProps with opacity 255.
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
 - Functions: Private — the reproduction
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Slides a bar across the screen on the native driver, checking every frame against a full repaint.
 *
 * The bar starts at translateX 0, so the last layout sees no transform on it, and animates 240 px
 * right — well clear of its 64 px-wide layout box.
 *
 * @param[in,out] test_context  TestCtx owning the framebuffer.
 * @param[in]     with_fade     Also fade a stacked sibling on the native driver, the shape this was
 *                           first reported as.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first diverging frame.
 */
static int run_slide(TestCtx* test_context, bool with_fade)
{
    static uint32_t incremental[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];

    er_reset();
    memset(test_context->framebuffer, 0, sizeof(uint32_t) * FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = FRAMEBUFFER_WIDTH;
    root_props.height = FRAMEBUFFER_HEIGHT;
    root_props.background_color = 0xFF101010U;
    er_node_set_props(root, &root_props);

    ERNode* bar = er_node_create(ER_NODE_VIEW);
    ERProps bar_props = props_default();
    bar_props.position = ER_POS_ABSOLUTE;
    bar_props.left = 0;
    bar_props.top = 0;
    bar_props.width = 64;
    bar_props.height = 20;
    bar_props.background_color = 0xFF4DA3FFU;
    er_node_set_props(bar, &bar_props);

    ERNode* fader = er_node_create(ER_NODE_VIEW);
    ERProps fader_props = props_default();
    fader_props.position = ER_POS_ABSOLUTE;
    fader_props.left = 0;
    fader_props.top = 34;
    fader_props.width = 300;
    fader_props.height = 20;
    fader_props.background_color = 0xFF3DDC84U;
    er_node_set_props(fader, &fader_props);

    er_tree_append_child(root, bar);
    er_tree_append_child(root, fader);
    er_tree_set_root(root);
    er_force_full_repaint();
    er_commit();

    /* Bind AFTER the layout above, exactly as a press handler starting an animation does. */
    ERAnimConfig config = {0};
    config.type = ER_ANIM_TIMING;
    config.duration_ms = ANIMATION_MS;
    config.easing = ER_EASE_LINEAR;

    ERAnimValueHandle slide = er_anim_value_create(0.0f);
    er_anim_value_bind(slide, bar, ER_PROP_TRANSLATE_X);
    er_anim_value_animate(slide, 240.0f, &config);

    ERAnimValueHandle fade = er_anim_value_create(0.0f);
    if (with_fade)
    {
        er_anim_value_bind(fade, fader, ER_PROP_OPACITY);
        er_anim_value_animate(fade, 1.0f, &config);
    }

    for (int frame_index = 0; frame_index < FRAMES; frame_index++)
    {
        embedded_renderer_tick(FRAME_MS);
        er_commit();
        memcpy(incremental, test_context->framebuffer, sizeof(incremental));

        memset(test_context->framebuffer, 0, sizeof(uint32_t) * FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
        er_force_full_repaint();
        er_commit();

        if (memcmp(incremental, test_context->framebuffer, sizeof(incremental)) != 0)
        {
            char message[160];
            snprintf(message,
                     sizeof(message),
                     "%s: frame %d of the slide differs from a full repaint of the same state",
                     with_fade ? "translate + sibling fade" : "translate alone",
                     frame_index);
            return fail(message);
        }
        /* Continue the incremental history from the incremental frame, not the reference one. */
        memcpy(test_context->framebuffer, incremental, sizeof(incremental));
    }
    return EXIT_SUCCESS;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Tests that a native-driver transform animation stays visible on a damage-clipped host.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    static uint32_t framebuffer[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];
    TestCtx test_context = {framebuffer, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT};
    EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, &test_context};
    embedded_renderer_set_backend(&backend);

    if (run_slide(&test_context, false) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (run_slide(&test_context, true) != EXIT_SUCCESS)
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}
