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
 * er_set_screen_bounds: the root is clamped to the bounds, and a full repaint draws nothing past them, however
 * large a node says it is. An arc is the case that matters: it rasterizes per pixel up to its own box, so one
 * tens of thousands of pixels across would stall a frame painting off screen.
 */

#include "er_node_internal.h"
#include "er_scene.h"
#include "native_renderer.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BOUNDS_W 100
#define BOUNDS_H 80
#define HUGE_SIDE 30000
#define FAR_BOUND 40000 /* a panel size a uint16_t can hold and an int16_t can't */

static int s_failures = 0;
static int s_right = 0;  /**< The furthest right anything painted. */
static int s_bottom = 0; /**< The furthest down anything painted. */

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            printf("FAIL: %s\n", msg);                                                                                 \
            s_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

static void note(int x, int y, int w, int h)
{
    if (x + w > s_right)
        s_right = x + w;
    if (y + h > s_bottom)
        s_bottom = y + h;
}

static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)argb;
    (void)ctx;
    note(x, y, w, h);
}

static void copy_cb(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)stride;
    (void)ctx;
    note(x, y, w, h);
}

static void blend_cb(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)stride;
    (void)alpha;
    (void)ctx;
    note(x, y, w, h);
}

/** @brief A root of the given size holding an arc of the same size, committed once. */
static ERNode* commit_scene(int side)
{
    er_reset();
    s_right = 0;
    s_bottom = 0;
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props;
    er_props_default(&root_props);
    root_props.width = (int16_t)side;
    root_props.height = (int16_t)side;
    root_props.background_color = 0xFF000000U;
    er_node_set_props(root, &root_props);

    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps arc_props;
    er_props_default(&arc_props);
    arc_props.position = ER_POS_ABSOLUTE;
    arc_props.left = 0;
    arc_props.top = 0;
    arc_props.width = HUGE_SIDE;
    arc_props.height = HUGE_SIDE;
    arc_props.arc_width = HUGE_SIDE / 2;
    arc_props.arc_value = 50.0f;
    arc_props.arc_max = 100.0f;
    arc_props.arc_track_color = 0xFF404040U;
    arc_props.arc_indicator_color = 0xFF00A0FFU;
    er_node_set_props(arc, &arc_props);
    er_tree_append_child(root, arc);

    er_tree_set_root(root);
    er_commit();
    return root;
}

int main(void)
{
    EmbeddedRenderBackend backend;
    memset(&backend, 0, sizeof(backend));
    backend.fill_rect = fill_cb;
    backend.copy_rect = copy_cb;
    backend.blend_rect = blend_cb;
    embedded_renderer_set_backend(&backend);

    er_set_screen_bounds(BOUNDS_W, BOUNDS_H);
    ERNode* root = commit_scene(HUGE_SIDE);
    CHECK(root->computed.w == BOUNDS_W && root->computed.h == BOUNDS_H, "the root is clamped to the bounds");
    CHECK(s_right > 0 && s_right <= BOUNDS_W, "nothing paints past the right bound");
    CHECK(s_bottom > 0 && s_bottom <= BOUNDS_H, "nothing paints past the bottom bound");

    root = commit_scene(40);
    CHECK(root->computed.w == 40 && root->computed.h == 40, "a root inside the bounds keeps its size");
    CHECK(s_right <= BOUNDS_W && s_bottom <= BOUNDS_H, "a node past a small root stops at the bounds");

    /* One limit: that axis stops at it, and the other keeps the root's size. */
    er_set_screen_bounds(BOUNDS_W, 0);
    root = commit_scene(40);
    CHECK(s_right > 0 && s_right <= BOUNDS_W, "a width alone still stops the right edge");
    CHECK(s_bottom > BOUNDS_H, "a width alone leaves the height free");

    er_set_screen_bounds(0, BOUNDS_H);
    root = commit_scene(40);
    CHECK(s_bottom > 0 && s_bottom <= BOUNDS_H, "a height alone still stops the bottom edge");
    CHECK(s_right > BOUNDS_W, "a height alone leaves the width free");

    /* Past int16 is still a limit, the largest there is, and not "no limit". */
    er_set_screen_bounds(FAR_BOUND, FAR_BOUND);
    er_reset();
    s_right = 0;
    s_bottom = 0;
    ERNode* far_root = er_node_create(ER_NODE_VIEW);
    ERProps far_root_props;
    er_props_default(&far_root_props);
    far_root_props.width = BOUNDS_W;
    far_root_props.height = BOUNDS_H;
    er_node_set_props(far_root, &far_root_props);
    ERNode* far_child = er_node_create(ER_NODE_VIEW);
    ERProps far_child_props;
    er_props_default(&far_child_props);
    far_child_props.position = ER_POS_ABSOLUTE;
    far_child_props.left = HUGE_SIDE;
    far_child_props.top = 0;
    far_child_props.width = HUGE_SIDE;
    far_child_props.height = BOUNDS_H;
    far_child_props.background_color = 0xFF404040U;
    far_child_props.transform_translate_x =
        (float)HUGE_SIDE; /* layout stops at int16, but a transform can paint past it */
    er_node_set_props(far_child, &far_child_props);
    er_tree_append_child(far_root, far_child);
    er_tree_set_root(far_root);
    er_commit();
    CHECK(s_right <= INT16_MAX, "bounds past int16 still clip");

    er_set_screen_bounds(0, 0);
    er_reset();
    ERNode* free_root = er_node_create(ER_NODE_VIEW);
    ERProps free_props;
    er_props_default(&free_props);
    free_props.width = BOUNDS_W * 2;
    free_props.height = BOUNDS_H * 2;
    free_props.background_color = 0xFF000000U;
    er_node_set_props(free_root, &free_props);
    er_tree_set_root(free_root);
    er_commit();
    CHECK(free_root->computed.w == BOUNDS_W * 2 && free_root->computed.h == BOUNDS_H * 2, "0 removes the limit");

    if (s_failures)
    {
        printf("%d failure(s)\n", s_failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
