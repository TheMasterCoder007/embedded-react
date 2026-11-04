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
 * Clip pushes past a full clip stack stay balanced with their pops (issue #371).
 *
 * The clip stack holds a fixed number of entries. A push at a full stack used to be dropped while
 * its matching pop still popped, so that pop removed the clip of an ancestor still being painted,
 * and everything that ancestor painted afterwards escaped its clip.
 *
 * Two checks:
 *
 *   - the blit-layer API directly: push well past the stack's capacity, pop back down, and the
 *     outer clips must come back exactly as they were pushed,
 *   - the scene the issue reports: a clipping view holding a chain of nested clipping views, then a
 *     sibling that overflows it. At every chain depth the sibling must be cut off at the outer box.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h" /* er_push_clip_rect / er_pop_clip_rect / er_get_clip_rect */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_WIDTH 200
#define SCREEN_HEIGHT 200

/*----------------------------------------------------------------------------------------------------------------------
 - Backend: composites for real
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_framebuffer[SCREEN_WIDTH * SCREEN_HEIGHT];

static uint32_t div255(uint32_t value)
{
    return (value + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX;
}

static void put(int x, int y, uint32_t source_alpha, uint32_t source_red, uint32_t source_green, uint32_t source_blue)
{
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT)
        return;
    uint32_t* destination = &s_framebuffer[y * SCREEN_WIDTH + x];
    const uint32_t inverse_alpha = ER_CHANNEL_MAX - source_alpha;
    const uint32_t destination_red = (*destination >> 16) & 0xFFu, destination_green = (*destination >> 8) & 0xFFu,
                   destination_blue = *destination & 0xFFu;
    *destination = 0xFF000000u | ((source_red + div255(destination_red * inverse_alpha)) << 16)
                   | ((source_green + div255(destination_green * inverse_alpha)) << 8)
                   | (source_blue + div255(destination_blue * inverse_alpha));
}

static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const uint32_t alpha = (argb >> 24) & 0xFFu;
    if (!alpha)
        return;
    const uint32_t source_red = div255(((argb >> 16) & 0xFFu) * alpha);
    const uint32_t source_green = div255(((argb >> 8) & 0xFFu) * alpha);
    const uint32_t source_blue = div255((argb & 0xFFu) * alpha);
    for (int pixel_y = y; pixel_y < y + height; pixel_y++)
        for (int pixel_x = x; pixel_x < x + width; pixel_x++)
            put(pixel_x, pixel_y, alpha, source_red, source_green, source_blue);
}

static void copy_cb(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const uint8_t* source_bytes = source;
    for (int row_index = 0; row_index < height; row_index++, source_bytes += stride)
    {
        const uint32_t* row = (const uint32_t*)source_bytes;
        for (int column = 0; column < width; column++)
        {
            const uint32_t value = row[column];
            const uint32_t source_alpha = (value >> 24) & 0xFFu;
            if (source_alpha == 0u)
                continue;
            put(x + column, y + row_index, source_alpha, (value >> 16) & 0xFFu, (value >> 8) & 0xFFu, value & 0xFFu);
        }
    }
}

static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const uint8_t* source_bytes = source;
    for (int row_index = 0; row_index < height; row_index++, source_bytes += stride)
    {
        const uint32_t* row = (const uint32_t*)source_bytes;
        for (int column = 0; column < width; column++)
        {
            const uint32_t source_pixel = row[column];
            uint32_t source_alpha = (source_pixel >> 24) & 0xFFu, source_red = (source_pixel >> 16) & 0xFFu,
                     source_green = (source_pixel >> 8) & 0xFFu, source_blue = source_pixel & 0xFFu;
            if (alpha < ER_ALPHA_OPAQUE)
            {
                source_alpha = div255(source_alpha * alpha);
                source_red = div255(source_red * alpha);
                source_green = div255(source_green * alpha);
                source_blue = div255(source_blue * alpha);
            }
            if (source_alpha == 0u)
                continue;
            put(x + column, y + row_index, source_alpha, source_red, source_green, source_blue);
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/* Far more pushes than the stack holds, so the overflow path is exercised many times over. */
#define DEEP_PUSHES 20

/* The outer clipping view's box, and a sibling inside it that hangs past its bottom-right corner. */
#define OUTER_WIDTH 100
#define OUTER_HEIGHT 100
#define SIBLING_X 50
#define SIBLING_Y 50
#define SIBLING_WIDTH 100
#define SIBLING_HEIGHT 100

/* A point on the sibling's overflow: outside the outer box, inside the sibling. */
#define OVERFLOW_X 120
#define OVERFLOW_Y 120
/* And one on the part of the sibling inside the outer box. */
#define INSIDE_X 70
#define INSIDE_Y 70

#define BACKGROUND_COLOR 0xFF202020u
#define SIBLING_COLOR 0xFF4DA3FFu

static ERProps props_default(void)
{
    ERProps props;
    er_props_default(&props);
    return props;
}

static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return EXIT_FAILURE;
}

static uint32_t at(int x, int y)
{
    return s_framebuffer[y * SCREEN_WIDTH + x] & 0xFFFFFFu;
}

/** @brief True when the top clip is exactly (x, y, w, h). */
static int clip_is(int x, int y, int width, int height)
{
    int clip_x, clip_y, clip_width, clip_height;
    return er_get_clip_rect(&clip_x, &clip_y, &clip_width, &clip_height) && clip_x == x && clip_y == y
           && clip_width == width && clip_height == height;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Direct API
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Pushes far past the stack's capacity and checks every pop restores the clip pushed before it.
 *
 * Push i is the rect (i, i, 200 - 2i, 200 - 2i), each strictly inside the last. Pushes past the
 * capacity are not applied (the top stays the deepest entry that fit), so the test does not assume
 * what the top is after each push — it records it, and on the way down every pop must bring back
 * exactly the clip that was on top before the matching push.
 */
static int check_api(void)
{
    if (er_get_clip_rect(NULL, NULL, NULL, NULL))
        return fail("api: the clip stack is not empty between frames");

    int top[DEEP_PUSHES][4];
    for (int push_index = 0; push_index < DEEP_PUSHES; push_index++)
    {
        er_push_clip_rect(push_index, push_index, SCREEN_WIDTH - 2 * push_index, SCREEN_HEIGHT - 2 * push_index);
        if (!er_get_clip_rect(&top[push_index][0], &top[push_index][1], &top[push_index][2], &top[push_index][3]))
            return fail("api: no clip is active after a push");
    }

    /* The deepest pushes cannot all have been applied, or the stack is not overflowing at all. */
    if (top[DEEP_PUSHES - 1][0] == DEEP_PUSHES - 1)
        return fail("api: every push was applied (the stack never filled; test is not testing anything)");

    /* A reset pushed onto the full stack must be balanced the same way. */
    er_push_clip_reset();
    er_pop_clip_rect();

    for (int push_index = DEEP_PUSHES - 1; push_index >= 0; push_index--)
    {
        if (!clip_is(top[push_index][0], top[push_index][1], top[push_index][2], top[push_index][3]))
        {
            int clip_x = 0, clip_y = 0, clip_width = 0, clip_height = 0;
            const bool any = er_get_clip_rect(&clip_x, &clip_y, &clip_width, &clip_height);
            fprintf(stderr,
                    "  api: back at depth %d the top clip is %s(%d,%d %dx%d), was (%d,%d %dx%d)\n",
                    push_index + 1,
                    any ? "" : "none ",
                    clip_x,
                    clip_y,
                    clip_width,
                    clip_height,
                    top[push_index][0],
                    top[push_index][1],
                    top[push_index][2],
                    top[push_index][3]);
            return fail("api: a pop removed a clip that belongs to a push still in force");
        }
        er_pop_clip_rect();
    }

    if (er_get_clip_rect(NULL, NULL, NULL, NULL))
        return fail("api: balanced pushes and pops did not leave the stack empty");

    /* The pops that fit must still restore exactly: push 3, pop 1, the second push is on top. */
    er_push_clip_rect(0, 0, 100, 100);
    er_push_clip_rect(10, 10, 50, 50);
    er_push_clip_rect(20, 20, 10, 10);
    er_pop_clip_rect();
    if (!clip_is(10, 10, 50, 50))
        return fail("api: a pop within capacity did not restore the previous clip");
    er_pop_clip_rect();
    er_pop_clip_rect();

    /* An unmatched pop on an empty stack stays harmless, and leaves no debt for the next push. */
    er_pop_clip_rect();
    er_push_clip_rect(5, 6, 7, 8);
    if (!clip_is(5, 6, 7, 8))
        return fail("api: a push after an unmatched pop was not applied");
    er_pop_clip_rect();
    if (er_get_clip_rect(NULL, NULL, NULL, NULL))
        return fail("api: the stack is not empty after the last pop");

    return 0;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Scene
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Builds the issue's scene: an outer clipping view holding @p depth nested clipping views of
 *        the same size, then a sibling that overflows the outer view.
 */
static void build_scene(int depth)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = SCREEN_WIDTH;
    root_props.height = SCREEN_HEIGHT;
    root_props.background_color = BACKGROUND_COLOR;
    er_node_set_props(root, &root_props);
    er_tree_set_root(root);

    ERProps outer_props = props_default();
    outer_props.position = ER_POSITION_ABSOLUTE;
    outer_props.left = 0;
    outer_props.top = 0;
    outer_props.width = OUTER_WIDTH;
    outer_props.height = OUTER_HEIGHT;
    outer_props.overflow = ER_OVERFLOW_HIDDEN;

    ERNode* outer = er_node_create(ER_NODE_VIEW);
    er_node_set_props(outer, &outer_props);
    er_tree_append_child(root, outer);

    ERNode* parent = outer;
    for (int depth_index = 0; depth_index < depth; depth_index++)
    {
        ERNode* node = er_node_create(ER_NODE_VIEW);
        er_node_set_props(node, &outer_props);
        er_tree_append_child(parent, node);
        parent = node;
    }

    ERNode* sibling = er_node_create(ER_NODE_VIEW);
    ERProps sibling_props = props_default();
    sibling_props.position = ER_POSITION_ABSOLUTE;
    sibling_props.left = SIBLING_X;
    sibling_props.top = SIBLING_Y;
    sibling_props.width = SIBLING_WIDTH;
    sibling_props.height = SIBLING_HEIGHT;
    sibling_props.background_color = SIBLING_COLOR;
    er_node_set_props(sibling, &sibling_props);
    er_tree_append_child(outer, sibling);
}

static int check_scene(void)
{
    for (int depth = 0; depth <= DEEP_PUSHES; depth++)
    {
        build_scene(depth);
        memset(s_framebuffer, 0, sizeof(s_framebuffer));
        er_force_full_repaint();
        er_commit();
        er_display_present();

        if (at(INSIDE_X, INSIDE_Y) != (SIBLING_COLOR & 0xFFFFFFu))
        {
            fprintf(stderr, "  scene: nested chain depth %d\n", depth);
            return fail("scene: the sibling was not drawn inside the outer view");
        }
        if (at(OVERFLOW_X, OVERFLOW_Y) != (BACKGROUND_COLOR & 0xFFFFFFu))
        {
            fprintf(stderr, "  scene: nested chain depth %d, pixel=%06x\n", depth, at(OVERFLOW_X, OVERFLOW_Y));
            return fail("scene: the sibling was drawn past the outer view's clip");
        }
        if (er_get_clip_rect(NULL, NULL, NULL, NULL))
        {
            fprintf(stderr, "  scene: nested chain depth %d\n", depth);
            return fail("scene: the commit left the clip stack unbalanced");
        }
    }
    return 0;
}

int main(void)
{
    static EmbeddedRenderBackend backend;
    backend.fill_rect = fill_cb;
    backend.copy_rect = copy_cb;
    backend.blend_rect = blend_cb;
    embedded_renderer_set_backend(&backend);

    if (check_api() != 0)
        return EXIT_FAILURE;
    if (check_scene() != 0)
        return EXIT_FAILURE;

    printf("PASS: clip stack overflow (balanced pushes and pops past capacity, outer clips kept)\n");
    return EXIT_SUCCESS;
}
