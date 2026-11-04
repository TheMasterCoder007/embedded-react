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
 * Issue #143: damage for a node inside a transformed ancestor was measured in unmapped layout space.
 *
 * Such a node never paints to the screen itself. render_tree captures the transformed ancestor's whole
 * subtree into the transform scratch in SOURCE space and inverse-maps it out at the ancestor's
 * transformed AABB — so the inner node's pixels reach the panel only through that blit, at the
 * ancestor-transformed position. Every measurement the damage pre-pass made of the inner node (its
 * screen rect, its last_paint_rect, the bounds a removal or a hide vacated) was plain
 * layout-minus-scroll and ignored the ancestor's matrix entirely, so a change to the inner node ALONE
 * scissored the re-capture and the blit to a region the changed pixels never land in. The change was
 * simply dropped — and since source_dirty clears post-commit, it stayed dropped until unrelated
 * damage happened to cover the region.
 *
 * Two observables, asserted for every mutation kind that can reach an inner node:
 *
 *   - the damage-clipped commit is byte-identical to a forced full repaint of the same scene state,
 *   - er_get_dirty_rects() covers every pixel that commit actually changed, so a host that transfers
 *     only the reported region does not leave the panel stale.
 *
 * Requires ERUI_TRANSFORMS=FULL — under TRANSLATE_ONLY there is no capture and no source space, so
 * nothing here can diverge. Note #137's check_nested_transform_no_damage covers only the idle
 * direction, where the two wrong-but-equal rects cancel out.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h" /* er_force_full_repaint — the reference frame */
#include "transform.h"         /* er_transform_source_fits — asserts the outer really captures */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN 200
#define FRAMEBUFFER_PIXELS (SCREEN * SCREEN)
#define OPACITY_OPAQUE 255U

/*----------------------------------------------------------------------------------------------------------------------
 - Backend: source-over framebuffer
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_framebuffer[FRAMEBUFFER_PIXELS];

/** @brief Rounds a 0-65025 product back to 0-255 the way the engine's blenders do. */
static uint32_t div255(uint32_t value)
{
    return (value + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX;
}

/** @brief Backend fill_rect: source-over composites a straight-alpha colour. */
static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const uint32_t first_frame = (argb >> 24) & 0xFFU;
    if (first_frame == 0U || width <= 0 || height <= 0)
        return;
    const uint32_t inverse_alpha = ER_CHANNEL_MAX - first_frame;
    const uint32_t source_red = (argb >> 16) & 0xFFU, source_green = (argb >> 8) & 0xFFU, source_blue = argb & 0xFFU;
    for (int row = y; row < y + height; row++)
    {
        if (row < 0 || row >= SCREEN)
            continue;
        for (int column = x; column < x + width; column++)
        {
            if (column < 0 || column >= SCREEN)
                continue;
            uint32_t* destination = &s_framebuffer[row * SCREEN + column];
            if (first_frame == 0xFFU)
            {
                *destination = 0xFF000000U | (argb & 0x00FFFFFFU);
                continue;
            }
            const uint32_t destination_red = (*destination >> 16) & 0xFFU,
                           destination_green = (*destination >> 8) & 0xFFU, destination_blue = *destination & 0xFFU;
            *destination = 0xFF000000U
                           | ((div255(source_red * first_frame) + div255(destination_red * inverse_alpha)) << 16)
                           | ((div255(source_green * first_frame) + div255(destination_green * inverse_alpha)) << 8)
                           | (div255(source_blue * first_frame) + div255(destination_blue * inverse_alpha));
        }
    }
}

/** @brief Backend copy_rect: source-over composites premultiplied scratch — this is how the blit lands. */
static void copy_cb(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + (size_t)row * (size_t)stride);
        for (int column = 0; column < width; column++)
        {
            const int destination_x = x + column, destination_y = y + row;
            if (destination_x < 0 || destination_x >= SCREEN || destination_y < 0 || destination_y >= SCREEN)
                continue;
            const uint32_t source_pixel = source_row[column];
            const uint32_t source_alpha = (source_pixel >> 24) & 0xFFU;
            if (source_alpha == 0U)
                continue; /* fully transparent: leave the destination alone */
            uint32_t* destination = &s_framebuffer[destination_y * SCREEN + destination_x];
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

/** @brief Backend blend_rect: source-over composites premultiplied pixels scaled by a global alpha. */
static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + (size_t)row * (size_t)stride);
        for (int column = 0; column < width; column++)
        {
            const int destination_x = x + column, destination_y = y + row;
            if (destination_x < 0 || destination_x >= SCREEN || destination_y < 0 || destination_y >= SCREEN)
                continue;
            const uint32_t source_pixel = source_row[column];
            const uint32_t source_alpha = div255(((source_pixel >> 24) & 0xFFU) * (uint32_t)alpha);
            if (source_alpha == 0U)
                continue;
            const uint32_t inverse_alpha = ER_CHANNEL_MAX - source_alpha;
            const uint32_t source_red = div255(((source_pixel >> 16) & 0xFFU) * (uint32_t)alpha);
            const uint32_t source_green = div255(((source_pixel >> 8) & 0xFFU) * (uint32_t)alpha);
            const uint32_t source_blue = div255((source_pixel & 0xFFU) * (uint32_t)alpha);
            uint32_t* destination = &s_framebuffer[destination_y * SCREEN + destination_x];
            const uint32_t destination_red = (*destination >> 16) & 0xFFU,
                           destination_green = (*destination >> 8) & 0xFFU, destination_blue = *destination & 0xFFU;
            *destination = 0xFF000000U | ((source_red + div255(destination_red * inverse_alpha)) << 16)
                           | ((source_green + div255(destination_green * inverse_alpha)) << 8)
                           | (source_blue + div255(destination_blue * inverse_alpha));
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static ERProps props_default(void)
{
    ERProps place_index = {0};
    place_index.left = place_index.top = place_index.right = place_index.bottom = ER_LAYOUT_AUTO;
    place_index.width = place_index.height = ER_LAYOUT_AUTO;
    place_index.min_width = place_index.max_width = ER_LAYOUT_AUTO;
    place_index.min_height = place_index.max_height = ER_LAYOUT_AUTO;
    place_index.padding = place_index.padding_left = place_index.padding_top = ER_LAYOUT_AUTO;
    place_index.padding_right = place_index.padding_bottom = ER_LAYOUT_AUTO;
    place_index.margin = place_index.margin_left = place_index.margin_top = ER_LAYOUT_AUTO;
    place_index.margin_right = place_index.margin_bottom = ER_LAYOUT_AUTO;
    place_index.gap = place_index.row_gap = place_index.column_gap = ER_LAYOUT_AUTO;
    place_index.flex_basis = ER_LAYOUT_AUTO;
    place_index.opacity = OPACITY_OPAQUE;
    return place_index;
}

static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return EXIT_FAILURE;
}

/**
 * @brief Reports how two frames differ, and where.
 *
 * @param[in]  first_frame   First frame.
 * @param[in]  second_frame  Second frame.
 * @param[out] bbox          Receives the bounding box of the differing pixels (untouched when identical).
 *
 * @return Number of differing pixels.
 */
static int diff_frames(const uint32_t* first_frame, const uint32_t* second_frame, ERRect* bbox)
{
    int count = 0, min_x = SCREEN, min_y = SCREEN, max_x = -1, max_y = -1;
    for (int y = 0; y < SCREEN; y++)
    {
        for (int x = 0; x < SCREEN; x++)
        {
            if (first_frame[y * SCREEN + x] == second_frame[y * SCREEN + x])
                continue;
            count++;
            if (x < min_x)
                min_x = x;
            if (y < min_y)
                min_y = y;
            if (x > max_x)
                max_x = x;
            if (y > max_y)
                max_y = y;
        }
    }
    if (count > 0 && bbox)
    {
        bbox->x = min_x;
        bbox->y = min_y;
        bbox->width = max_x - min_x + 1;
        bbox->height = max_y - min_y + 1;
    }
    return count;
}

/**
 * @brief Counts changed pixels that no reported dirty rect covers.
 *
 * @param[in] before  Frame before the commit.
 * @param[in] after   Frame after it.
 * @param[in] rects   Rects er_get_dirty_rects() reported for that commit.
 * @param[in] count   Number of rects.
 *
 * @return Number of changed pixels outside every reported rect.
 */
static int unreported_pixels(const uint32_t* before, const uint32_t* after, const ERRect* rects, int count)
{
    int missed = 0;
    for (int y = 0; y < SCREEN; y++)
    {
        for (int x = 0; x < SCREEN; x++)
        {
            if (before[y * SCREEN + x] == after[y * SCREEN + x])
                continue;
            bool covered = false;
            for (int rect_index = 0; rect_index < count && !covered; rect_index++)
                covered = (x >= rects[rect_index].x && x < rects[rect_index].x + rects[rect_index].width
                           && y >= rects[rect_index].y && y < rects[rect_index].y + rects[rect_index].height);
            if (!covered)
                missed++;
        }
    }
    return missed;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Test
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief What is done to the inner node — every way a change can reach one inside a capture. */
typedef enum
{
    MUTATION_RECOLOUR, /**< Pure repaint: source_dirty, same geometry — the issue's own reproduction. */
    MUTATION_RESIZE,   /**< source_dirty AND moved: its own footprint changes inside the source. */
    MUTATION_HIDE,     /**< display:none — vacated pixels, registered from the last painted rect. */
    MUTATION_REMOVE,   /**< Unmounted — the same vacated-pixel channel, reached before detach. */
    MUTATION_SIBLING   /**< A SIBLING grows and reflows it: moved, never source_dirty itself. */
} Mutation;

/**
 * @brief Mutates only the inside of a rotated ancestor and demands a full-repaint match.
 *
 * Both children are laid out in the ancestor's flex column so MUTATION_SIBLING can push the second one
 * down; the other mutations touch that second child alone and the first is inert scenery.
 *
 * @param[in] outer_x   Outer node left position.
 * @param[in] outer_y   Outer node top position.
 * @param[in] rotation  Outer node rotation in degrees.
 * @param[in] mutation  Which mutation to apply.
 * @param[in] label     Printed name for the case.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
static int check_inner_mutation(int outer_x, int outer_y, float rotation, Mutation mutation, const char* label)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = SCREEN;
    root_props.height = SCREEN;
    root_props.background_color = 0xFFFFFFFFU; /* opaque: a full repaint really covers the screen */
    er_node_set_props(root, &root_props);

    /* Outer: comfortably inside the transform scratch, so its capture succeeds and its whole subtree
     * renders in SOURCE space behind it. */
    ERNode* outer = er_node_create(ER_NODE_VIEW);
    ERProps outer_props = props_default();
    outer_props.position = ER_POSITION_ABSOLUTE;
    outer_props.left = (int16_t)outer_x;
    outer_props.top = (int16_t)outer_y;
    outer_props.width = 100;
    outer_props.height = 100;
    outer_props.background_color = 0xFF2244AAU;
    outer_props.transform_rotate_z = rotation;
    er_node_set_props(outer, &outer_props);

    /* Spacer: only MUTATION_SIBLING touches it; it is what reflows the inner node without dirtying it. */
    ERNode* spacer = er_node_create(ER_NODE_VIEW);
    ERProps spacer_props = props_default();
    spacer_props.width = 100;
    spacer_props.height = 20;
    spacer_props.background_color = 0xFF884400U;
    er_node_set_props(spacer, &spacer_props);

    /* Inner: no transform of its own — the whole point is that it is measured in the ancestor's
     * source space while its pixels land at the ancestor-transformed position. */
    ERNode* inner = er_node_create(ER_NODE_VIEW);
    ERProps inner_props = props_default();
    inner_props.width = 60;
    inner_props.height = 50;
    inner_props.margin_left = 20;
    inner_props.background_color = 0xFF33FF33U;
    er_node_set_props(inner, &inner_props);

    er_tree_append_child(outer, spacer);
    er_tree_append_child(outer, inner);
    er_tree_append_child(root, outer);
    er_tree_set_root(root);

    er_force_full_repaint();
    er_commit();
    er_commit(); /* settle: every node has a last-painted footprint */

    static uint32_t before[FRAMEBUFFER_PIXELS];
    memcpy(before, s_framebuffer, sizeof(before));

    switch (mutation)
    {
        case MUTATION_RECOLOUR:
            inner_props.background_color = 0xFFFF2222U;
            er_node_set_props(inner, &inner_props);
            break;
        case MUTATION_RESIZE:
            inner_props.width = 30;
            inner_props.height = 25;
            er_node_set_props(inner, &inner_props);
            break;
        case MUTATION_HIDE:
            inner_props.display = ER_DISPLAY_NONE;
            er_node_set_props(inner, &inner_props);
            break;
        case MUTATION_REMOVE:
            er_tree_remove_child(outer, inner);
            break;
        case MUTATION_SIBLING:
            spacer_props.height = 45; /* pushes the inner node down inside the capture; it stays clean */
            er_node_set_props(spacer, &spacer_props);
            break;
    }

    er_commit(); /* the damage-clipped commit under test */

    ERRect rects[ER_DAMAGE_RECTS_MAX];
    const int rect_count = er_get_dirty_rects(rects, ER_DAMAGE_RECTS_MAX);

    static uint32_t incremental[FRAMEBUFFER_PIXELS];
    memcpy(incremental, s_framebuffer, sizeof(incremental));

    er_force_full_repaint();
    er_commit(); /* reference: the same scene state, painted in full */

    ERRect bbox = {0, 0, 0, 0};
    const int differ = diff_frames(incremental, s_framebuffer, &bbox);
    const int missed = unreported_pixels(before, incremental, rects, rect_count);

    /* An idle commit must not be what heals it — nor damage anything of its own. */
    memcpy(s_framebuffer, incremental, sizeof(incremental));
    er_commit();
    const int idle_differ = diff_frames(s_framebuffer, incremental, NULL);

    printf("%s: %d px differ from a full repaint%s; %d rect(s) reported, %d changed px unreported; "
           "idle commit moved %d px\n",
           label,
           differ,
           differ ? "" : " (identical)",
           rect_count,
           missed,
           idle_differ);
    if (differ)
        printf("  divergence bbox %d,%d %dx%d\n", bbox.x, bbox.y, bbox.width, bbox.height);

    if (mutation != MUTATION_REMOVE)
        er_node_destroy(root); /* tears down the subtree */
    else
    {
        er_node_destroy(inner); /* already detached */
        er_node_destroy(root);
    }

    if (differ != 0)
        return fail("an inner-only change under a transformed ancestor diverged from a full repaint");
    if (missed != 0)
        return fail("pixels changed outside every reported dirty rect — a partial-update host would "
                    "leave the panel stale");
    if (idle_differ != 0)
        return fail("an idle commit repainted differently — the transformed subtree is churning");
    return EXIT_SUCCESS;
}

/**
 * @brief The capture passes DOWN past a transformed ancestor whose matrix cannot be inverted.
 *
 * render_tree admits a capture on two things: the node fits the transform scratch AND its matrix
 * inverts (the inverse-map blit is what puts the scratch back on screen). A transform that collapses
 * — a scale animating toward zero is the everyday way to get one — fails the second test however
 * comfortably it passes the first, so it paints untransformed at its layout box and the capture
 * passes down to the next transform inside it.
 *
 * A damage escalation that picked the capturing ancestor on SIZE alone would name the outer node here
 * and damage its raw box, while the leaf's pixels come out of the inner node's blit somewhere else
 * entirely — the same missed update this file is about, one level down. Flagged in review on #143.
 *
 * @param[in] screen  Root size.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
static int check_noninvertible_outer(int screen)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = (int16_t)screen;
    root_props.height = (int16_t)screen;
    root_props.background_color = 0xFFFFFFFFU;
    er_node_set_props(root, &root_props);

    /* Outer: fits the scratch with room to spare, but its 2x2 determinant (scale_x * scale_y) lands
     * far below the threshold er_transform_invert() rejects at, so no capture starts here. */
    ERNode* outer = er_node_create(ER_NODE_VIEW);
    ERProps outer_props = props_default();
    outer_props.position = ER_POSITION_ABSOLUTE;
    outer_props.left = 10;
    outer_props.top = 10;
    outer_props.width = 100;
    outer_props.height = 100;
    outer_props.background_color = 0xFF2244AAU;
    outer_props.transform_rotate_z = 20.0f;
    outer_props.transform_scale_x = 5.0e-4f;
    outer_props.transform_scale_y = 5.0e-4f;
    er_node_set_props(outer, &outer_props);

    /* Inner: ordinary rotation, fits — and with the outer refused, this is what captures. Parked at
     * the outer's bottom-right corner (overflow is visible, so a child may paint past its parent) and
     * rotated 45 degrees, which throws its AABB well clear of the outer's raw box. That separation is
     * the whole point: naming the outer as the capturing ancestor damages a region that does NOT
     * contain what the inner's blit writes, instead of one that happens to. */
    ERNode* inner = er_node_create(ER_NODE_VIEW);
    ERProps inner_props = props_default();
    inner_props.position = ER_POSITION_ABSOLUTE;
    inner_props.left = 70;
    inner_props.top = 70;
    inner_props.width = 60;
    inner_props.height = 60;
    inner_props.background_color = 0xFF117733U;
    inner_props.transform_rotate_z = 45.0f;
    er_node_set_props(inner, &inner_props);

    /* Leaf: the thing that changes. Tucked into the far corner of the inner node's source, so its
     * pixels come out of the blit past the outer's box entirely. */
    ERNode* leaf = er_node_create(ER_NODE_VIEW);
    ERProps leaf_props = props_default();
    leaf_props.position = ER_POSITION_ABSOLUTE;
    leaf_props.left = 35;
    leaf_props.top = 35;
    leaf_props.width = 25;
    leaf_props.height = 25;
    leaf_props.background_color = 0xFF33FF33U;
    er_node_set_props(leaf, &leaf_props);

    er_tree_append_child(inner, leaf);
    er_tree_append_child(outer, inner);
    er_tree_append_child(root, outer);
    er_tree_set_root(root);

    er_force_full_repaint();
    er_commit();
    er_commit();

    static uint32_t before[FRAMEBUFFER_PIXELS];
    memcpy(before, s_framebuffer, sizeof(before));

    leaf_props.background_color = 0xFFFF2222U;
    er_node_set_props(leaf, &leaf_props);
    er_commit();

    ERRect rects[ER_DAMAGE_RECTS_MAX];
    const int rect_count = er_get_dirty_rects(rects, ER_DAMAGE_RECTS_MAX);
    static uint32_t incremental[FRAMEBUFFER_PIXELS];
    memcpy(incremental, s_framebuffer, sizeof(incremental));

    er_force_full_repaint();
    er_commit();

    ERRect bbox = {0, 0, 0, 0};
    const int differ = diff_frames(incremental, s_framebuffer, &bbox);
    const int missed = unreported_pixels(before, incremental, rects, rect_count);
    /* The case is only meaningful if the leaf change really moved pixels through the inner blit. */
    const int changed = diff_frames(before, incremental, NULL);

    printf("non-invertible outer, capturing inner: %d px differ from a full repaint, %d changed px "
           "unreported (%d changed in all)\n",
           differ,
           missed,
           changed);
    if (differ)
        printf("  divergence bbox %d,%d %dx%d\n", bbox.x, bbox.y, bbox.width, bbox.height);

    er_node_destroy(root);

    /* Asked first: when the escalation names the outer node, the damage misses the inner blit
     * completely and the commit changes nothing at all — so `changed` reads 0 for the very reason the
     * test exists, and reporting it as "proves nothing" would bury the real failure. */
    if (differ != 0)
        return fail("damage escalated to the wrong ancestor — the outer transform never captured, so "
                    "the capture (and the pixels) belong to the inner one");
    if (missed != 0)
        return fail("pixels changed outside every reported dirty rect");
    if (changed == 0)
        return fail("the leaf recolour changed nothing on screen — the case proves nothing");
    return EXIT_SUCCESS;
}

int main(void)
{
    EmbeddedRenderBackend backend = {0};
    backend.fill_rect = fill_cb;
    backend.copy_rect = copy_cb;
    backend.blend_rect = blend_cb;
    embedded_renderer_set_backend(&backend);

#if ERUI_TRANSFORMS_FULL
    /* Vacuous unless the outer node really takes the capture path. */
    if (!er_transform_source_fits(100, 100))
    {
        printf("SKIP: a 100x100 node does not fit this build's transform scratch\n");
        return EXIT_SUCCESS;
    }

    static const struct
    {
        Mutation mutation;
        const char* name;
    } k_mutations[] = {
        {MUTATION_RECOLOUR, "recolour"},
        {MUTATION_RESIZE, "resize"},
        {MUTATION_HIDE, "hide"},
        {MUTATION_REMOVE, "remove"},
        {MUTATION_SIBLING, "sibling reflow"},
    };

    /* Two placements: the second is far enough from the layout box that a rect measured in source
     * space and one measured on screen barely overlap. */
    static const struct
    {
        int x, y;
        float rotation;
    } k_places[] = {{10, 10, 20.0f}, {80, 80, 45.0f}};

    for (size_t place_index = 0; place_index < sizeof(k_places) / sizeof(k_places[0]); place_index++)
    {
        for (size_t mutation_index = 0; mutation_index < sizeof(k_mutations) / sizeof(k_mutations[0]); mutation_index++)
        {
            char label[96];
            snprintf(label,
                     sizeof(label),
                     "outer @(%d,%d) rot %g / %s",
                     k_places[place_index].x,
                     k_places[place_index].y,
                     (double)k_places[place_index].rotation,
                     k_mutations[mutation_index].name);
            const int result = check_inner_mutation(k_places[place_index].x,
                                                    k_places[place_index].y,
                                                    k_places[place_index].rotation,
                                                    k_mutations[mutation_index].mutation,
                                                    label);
            if (result != EXIT_SUCCESS)
                return result;
        }
    }

    const int noninvertible_result = check_noninvertible_outer(200);
    if (noninvertible_result != EXIT_SUCCESS)
        return noninvertible_result;

    printf("PASS: inner-only mutations under a transformed ancestor match a full repaint\n");
#else
    printf("SKIP: ERUI_TRANSFORMS=FULL required (no transform capture, no source space)\n");
#endif
    return EXIT_SUCCESS;
}
