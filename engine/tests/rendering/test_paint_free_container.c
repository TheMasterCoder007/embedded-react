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
 * A container that paints nothing and only grows (a child appended past the screen's bottom edge)
 * repaints nothing on screen: its children report their own changes. Damaging its whole box made a
 * list growing at its unseen end repaint the viewport on every frame of a scroll.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FB_W 200
#define FB_H 200

static long s_painted;

static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)argb, (void)x, (void)y, (void)ctx;
    if (w > 0 && h > 0)
        s_painted += (long)w * h;
}

static void copy_cb(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    (void)src, (void)stride, (void)x, (void)y, (void)ctx;
    if (w > 0 && h > 0)
        s_painted += (long)w * h;
}

static void blend_cb(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)alpha;
    copy_cb(src, stride, x, y, w, h, ctx);
}

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

static ERNode* box(int height, uint32_t color)
{
    ERNode* n = er_node_create(ER_NODE_VIEW);
    ERProps p = props_default();
    p.width = FB_W;
    p.height = (int16_t)height;
    p.background_color = color;
    er_node_set_props(n, &p);
    return n;
}

int main(void)
{
    EmbeddedRenderBackend be = {fill_cb, copy_cb, blend_cb, NULL, NULL, NULL};
    embedded_renderer_set_backend(&be);

    ERNode* root = box(FB_H, 0xFF101010U);
    ERNode* list = er_node_create(ER_NODE_VIEW);
    ERProps lp = props_default();
    lp.width = FB_W;
    er_node_set_props(list, &lp);
    er_tree_append_child(list, box(250, 0xFF2255AAU));
    er_tree_append_child(root, list);
    er_tree_set_root(root);
    er_commit();

    /* The list grows below the screen: nothing on screen changed. */
    s_painted = 0;
    er_tree_append_child(list, box(150, 0xFF22AA55U));
    er_commit();
    if (s_painted != 0)
    {
        fprintf(stderr, "FAIL: a paint-free container growing off screen repainted %ld px\n", s_painted);
        return EXIT_FAILURE;
    }

    /* The same container with a background still repaints the part of it that shows. */
    lp.background_color = 0xFF303030U;
    er_node_set_props(list, &lp);
    er_commit();
    s_painted = 0;
    er_tree_append_child(list, box(150, 0xFFAA5522U));
    er_commit();
    if (s_painted == 0)
    {
        fprintf(stderr, "FAIL: a painted container growing did not repaint\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
