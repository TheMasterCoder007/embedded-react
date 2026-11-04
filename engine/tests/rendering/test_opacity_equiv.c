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
 * Damage-clipped frames must be byte-identical to a full repaint of the same scene state.
 *
 * The bug this guards (issue #130): a translucent group composited its subtree through a scratch
 * region bounded by the group's OWN box, so a child laid out past that box was clipped out of
 * existence. Because the walk never reached such a child, it never had painted_seq stamped, and
 * er_commit's post-pass — which clears flags only for nodes that painted — left it dirty forever.
 * A later commit in which the group itself was clean then found an orphaned dirty node with no
 * dirty ancestor to composite it, and painted it straight into the framebuffer at FULL alpha.
 * The incremental frame and a full repaint of the same state disagreed from then on.
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

#define FRAMEBUFFER_WIDTH 32
#define FRAMEBUFFER_HEIGHT 32
#define OPACITY_OPAQUE 255U

/* Fuzz scene: a 160x160 screen of absolutely-positioned boxes, ~1 in 6 carrying a node opacity. */
#define FUZZ_SCREEN 160
#define FUZZ_NODES 24
#define FUZZ_STEPS 8

/* Seeds that reproduced the orphaned-dirty divergence before the fix (found by an 8000-seed soak;
 * kept explicitly so the regression stays pinned even if the soak below is trimmed). */
static const int k_known_bad_seeds[] = {1337, 2448, 2685, 5259, 5318, 6647};

/* Extra seeds swept on top of the known-bad ones. Cheap enough for CI, wide enough to catch a
 * different scene shape regressing the same invariant. */
#define FUZZ_SWEEP_SEEDS 400

/* Linear-congruential generator constants, and the spread that turns a seed into its first state. */
#define LCG_MULTIPLIER 1664525U
#define LCG_INCREMENT 1013904223U
#define SEED_SPREAD 2654435761U
#define SEED_OFFSET 12345U

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Framebuffer the backend stubs rasterise into.
 */
typedef struct
{
    uint32_t* framebuffer;  /**< Flat premultiplied ARGB8888 framebuffer. */
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
 * Blending (rather than overwriting) is what makes a double-composited translucent layer show up as
 * a pixel difference instead of being silently absorbed.
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
 * copy_rect is NOT a replacing blit: only the separate copy_opaque callback carries the
 * every-pixel-is-opaque guarantee. The engine's own scratch_do_copy skips fully transparent source
 * pixels and source-overs the rest, and the reference software backend does the same — so a test
 * backend that overwrote unconditionally would erase the painted background under antialiased glyph
 * and rounded-corner edges, and make the incremental-vs-full comparison depend on repaint history
 * rather than on scene state.
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
                continue; /* fully transparent: leave the destination alone */
            uint32_t* destination =
                &test_context->framebuffer[destination_y * test_context->framebuffer_width + destination_x];
            if (source_alpha == 0xFFU)
            {
                *destination = source_pixel; /* opaque source replaces the destination */
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
 - Functions: Private — case 1, the minimal reproduction
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Checks that a child laid out past its translucent group's box composites WITH the group.
 *
 * Then drives the exact sequence that used to diverge: a commit in which the group is clean but the
 * child still carries a dirty flag from a commit that never painted it.
 *
 * @param[in,out] test_context  TestCtx owning the framebuffer.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
static int test_overflowing_child(TestCtx* test_context)
{
    static uint32_t incremental[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];

    er_reset();
    memset(test_context->framebuffer, 0, sizeof(uint32_t) * FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = FRAMEBUFFER_WIDTH;
    root_props.height = FRAMEBUFFER_HEIGHT;
    root_props.background_color = 0xFFFFFFFFU; /* opaque white */
    er_node_set_props(root, &root_props);

    /* Translucent group with a small box... */
    ERNode* group = er_node_create(ER_NODE_VIEW);
    ERProps group_props = props_default();
    group_props.position = ER_POS_ABSOLUTE;
    group_props.left = 0;
    group_props.top = 0;
    group_props.width = 8;
    group_props.height = 8;
    group_props.background_color = 0x00000000U; /* transparent container */
    group_props.opacity = 128U;
    er_node_set_props(group, &group_props);

    /* ...and a child that lays out entirely OUTSIDE it. */
    ERNode* child = er_node_create(ER_NODE_VIEW);
    ERProps child_props = props_default();
    child_props.position = ER_POS_ABSOLUTE;
    child_props.left = 12;
    child_props.top = 12;
    child_props.width = 12;
    child_props.height = 12;
    child_props.background_color = 0xFFFF0000U; /* opaque red */
    er_node_set_props(child, &child_props);

    er_tree_append_child(group, child);
    er_tree_append_child(root, group);
    er_tree_set_root(root);
    er_commit();

    /* The group does not clip: the child paints, faded to the group alpha (red over white). */
    const uint32_t inside = test_context->framebuffer[16 * FRAMEBUFFER_WIDTH + 16];
    if (inside != 0xFFFF7F7FU) /* opaque red at alpha 128 over white */
        return fail("overflowing child: should composite at the group alpha, not be clipped away");

    /* Two full repaints: the second one is the state the divergence used to appear from — every
     * flag the walk could clear has been cleared, so anything still dirty is an orphan. */
    er_force_full_repaint();
    er_commit();
    er_force_full_repaint();
    er_commit();

    /* A commit with no mutation at all. Before the fix the child was still dirty here (it had never
     * been painted, so its flags never cleared) while the group was clean — so it painted alone,
     * at full alpha, outside the group. */
    er_commit();
    memcpy(incremental, test_context->framebuffer, sizeof(incremental));

    memset(test_context->framebuffer, 0, sizeof(uint32_t) * FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    er_force_full_repaint();
    er_commit();

    if (memcmp(incremental, test_context->framebuffer, sizeof(incremental)) != 0)
        return fail("orphaned dirty child: incremental frame differs from a full repaint");

    return EXIT_SUCCESS;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — case 2, the seeded soak
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_random_state;

/**
 * @brief Linear-congruential random in [0, n).
 *
 * @param[in] bound  Exclusive upper bound (must be non-zero).
 *
 * @return Pseudo-random value below @p bound.
 */
static uint32_t rnd(uint32_t bound)
{
    s_random_state = s_random_state * LCG_MULTIPLIER + LCG_INCREMENT;
    return (s_random_state >> 16) % bound;
}

/**
 * @brief Random absolutely-positioned box props.
 *
 * ~40% are full-screen layers (the shape that makes the occlusion cull fire), backgrounds are
 * randomly opaque or translucent, and ~1 in 6 carries a node opacity.
 *
 * @return Randomised ERProps.
 */
static ERProps rand_box(void)
{
    ERProps props = props_default();
    props.position = ER_POS_ABSOLUTE;
    if (rnd(5) < 2)
    {
        props.left = 0;
        props.top = 0;
        props.width = FUZZ_SCREEN;
        props.height = FUZZ_SCREEN;
    }
    else
    {
        props.left = (int16_t)rnd(FUZZ_SCREEN);
        props.top = (int16_t)rnd(FUZZ_SCREEN);
        props.width = (int16_t)(8 + rnd(FUZZ_SCREEN));
        props.height = (int16_t)(8 + rnd(FUZZ_SCREEN));
    }
    const uint32_t alpha = rnd(2) ? 0xFFU : (uint32_t)(40 + rnd(200));
    props.background_color = (alpha << 24) | rnd(0x1000000);
    if (rnd(4) == 0)
        props.border_radius = (int16_t)rnd(12);
    if (rnd(6) == 0)
        props.opacity = (uint8_t)(60 + rnd(190));
    if (rnd(5) == 0)
    {
        props.border_width = (int16_t)(1 + rnd(4));
        props.border_color = 0xFF000000U | rnd(0x1000000);
    }
    if (rnd(6) == 0)
        props.z_index = (int16_t)rnd(4);
    if (rnd(8) == 0)
        props.overflow = ER_OVERFLOW_HIDDEN;
    return props;
}

/**
 * @brief Runs one seed: build a random scene, then mutate/commit/compare FUZZ_STEPS times.
 *
 * @param[in]     seed          Scene seed.
 * @param[in,out] test_context  TestCtx owning the framebuffer.
 * @param[out]    step          Step index of the first mismatch (untouched when the seed passes).
 *
 * @return true when every incremental frame matched a full repaint.
 */
static bool run_seed(int seed, TestCtx* test_context, int* step)
{
    static uint32_t incremental[FUZZ_SCREEN * FUZZ_SCREEN];
    ERNode* nodes[FUZZ_NODES];
    ERProps props[FUZZ_NODES];
    ERNode* parents[FUZZ_NODES + 1];

    s_random_state = (uint32_t)seed * SEED_SPREAD + SEED_OFFSET;
    er_reset();
    memset(test_context->framebuffer, 0, sizeof(uint32_t) * FUZZ_SCREEN * FUZZ_SCREEN);

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = FUZZ_SCREEN;
    root_props.height = FUZZ_SCREEN;
    root_props.background_color = 0xFF202020U;
    er_node_set_props(root, &root_props);

    int parent_count = 0;
    parents[parent_count++] = root;
    for (int node_index = 0; node_index < FUZZ_NODES; node_index++)
    {
        ERNode* node = er_node_create(rnd(6) == 0 ? ER_NODE_TEXT : ER_NODE_VIEW);
        ERProps box_props = rand_box();
        if (er_node_get_type(node) == ER_NODE_TEXT)
        {
            snprintf(box_props.text, sizeof(box_props.text), "n%d", node_index);
            box_props.font_size = 12;
            box_props.color = 0xFFFFFFFFU;
        }
        er_node_set_props(node, &box_props);
        er_tree_append_child(parents[rnd((uint32_t)parent_count)], node);
        parents[parent_count++] = node;
        nodes[node_index] = node;
        props[node_index] = box_props;
    }
    er_tree_set_root(root);
    er_force_full_repaint();
    er_commit();

    for (int step_index = 0; step_index < FUZZ_STEPS; step_index++)
    {
        const int picked = (int)rnd((uint32_t)FUZZ_NODES);
        ERProps box_props = props[picked];
        switch (rnd(4))
        {
            case 0: /* visual */
                box_props.background_color = (box_props.background_color & 0xFF000000U) | rnd(0x1000000);
                break;
            case 1: /* move */
                box_props.left = (int16_t)rnd(FUZZ_SCREEN);
                box_props.top = (int16_t)rnd(FUZZ_SCREEN);
                break;
            case 2: /* resize */
                box_props.width = (int16_t)(8 + rnd(FUZZ_SCREEN));
                box_props.height = (int16_t)(8 + rnd(FUZZ_SCREEN));
                break;
            default: /* no-op layout */
                box_props.max_width = (int16_t)(FUZZ_SCREEN + 50);
                box_props.min_height = 1;
                break;
        }
        if (rnd(6) == 0) /* flip a layer between opaque and translucent: occlusion cull on/off */
        {
            const uint32_t alpha = ((box_props.background_color >> 24) == 0xFFU) ? 0x90U : 0xFFU;
            box_props.background_color = (alpha << 24) | (box_props.background_color & 0x00FFFFFFU);
        }
        if (rnd(8) == 0)
            box_props.border_radius = (int16_t)(box_props.border_radius ? 0 : 10);

        props[picked] = box_props;
        er_node_set_props(nodes[picked], &box_props);
        er_commit();
        memcpy(incremental, test_context->framebuffer, sizeof(incremental));

        memset(test_context->framebuffer, 0, sizeof(uint32_t) * FUZZ_SCREEN * FUZZ_SCREEN);
        er_force_full_repaint();
        er_commit();
        if (memcmp(incremental, test_context->framebuffer, sizeof(incremental)) != 0)
        {
            *step = step_index;
            return false;
        }
        /* Continue the incremental history from the incremental frame, not the reference one. */
        memcpy(test_context->framebuffer, incremental, sizeof(incremental));
    }
    return true;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Tests that damage-clipped frames match a full repaint when opacity groups are involved.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    static uint32_t framebuffer[FUZZ_SCREEN * FUZZ_SCREEN];
    TestCtx test_context = {framebuffer, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT};
    EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, &test_context};
    embedded_renderer_set_backend(&backend);

    if (test_overflowing_child(&test_context) != EXIT_SUCCESS)
        return EXIT_FAILURE;

    test_context.framebuffer_width = FUZZ_SCREEN;
    test_context.framebuffer_height = FUZZ_SCREEN;
    for (size_t seed_index = 0; seed_index < sizeof(k_known_bad_seeds) / sizeof(k_known_bad_seeds[0]); seed_index++)
    {
        int step = -1;
        if (!run_seed(k_known_bad_seeds[seed_index], &test_context, &step))
        {
            char message[128];
            snprintf(message,
                     sizeof(message),
                     "known-bad seed %d step %d: incremental frame differs from a full repaint",
                     k_known_bad_seeds[seed_index],
                     step);
            return fail(message);
        }
    }
    for (int seed = 1; seed <= FUZZ_SWEEP_SEEDS; seed++)
    {
        int step = -1;
        if (!run_seed(seed, &test_context, &step))
        {
            char message[128];
            snprintf(
                message, sizeof(message), "seed %d step %d: incremental frame differs from a full repaint", seed, step);
            return fail(message);
        }
    }

    return EXIT_SUCCESS;
}
