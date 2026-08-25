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

#define FB_W 32
#define FB_H 32

/* Fuzz scene: a 160x160 screen of absolutely-positioned boxes, ~1 in 6 carrying a node opacity. */
#define FZ_S 160
#define FZ_NODES 24
#define FZ_STEPS 8

/* Seeds that reproduced the orphaned-dirty divergence before the fix (found by an 8000-seed soak;
 * kept explicitly so the regression stays pinned even if the soak below is trimmed). */
static const int k_known_bad_seeds[] = {1337, 2448, 2685, 5259, 5318, 6647};

/* Extra seeds swept on top of the known-bad ones. Cheap enough for CI, wide enough to catch a
 * different scene shape regressing the same invariant. */
#define FZ_SWEEP_SEEDS 400

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Framebuffer the backend stubs rasterise into.
 */
typedef struct
{
    uint32_t* fb; /**< Flat premultiplied ARGB8888 framebuffer. */
    int fb_w;     /**< Width in pixels. */
    int fb_h;     /**< Height in pixels. */
} TestCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — backend stubs
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Rounds a 0–65025 product back to 0–255 the way the engine's blenders do. */
static uint32_t div255(uint32_t v)
{
    return (v + 127U) / 255U;
}

/**
 * @brief Backend fill_rect: source-over composites a straight-alpha colour into the framebuffer.
 *
 * Blending (rather than overwriting) is what makes a double-composited translucent layer show up as
 * a pixel difference instead of being silently absorbed.
 *
 * @param[in] argb  Straight-alpha ARGB8888 fill colour.
 * @param[in] x     Left edge.
 * @param[in] y     Top edge.
 * @param[in] w     Width in pixels.
 * @param[in] h     Height in pixels.
 * @param[in] ctx   Pointer to TestCtx.
 */
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    TestCtx* t = ctx;
    const uint32_t a = (argb >> 24) & 0xFFU;
    if (a == 0U || w <= 0 || h <= 0)
        return;
    const uint32_t inv = 255U - a;
    const uint32_t sr = (argb >> 16) & 0xFFU, sg = (argb >> 8) & 0xFFU, sb = argb & 0xFFU;

    for (int row = y; row < y + h; row++)
    {
        if (row < 0 || row >= t->fb_h)
            continue;
        for (int col = x; col < x + w; col++)
        {
            if (col < 0 || col >= t->fb_w)
                continue;
            uint32_t* d = &t->fb[row * t->fb_w + col];
            const uint32_t dr = (*d >> 16) & 0xFFU, dg = (*d >> 8) & 0xFFU, db = *d & 0xFFU;
            *d = 0xFF000000U | ((div255(sr * a) + div255(dr * inv)) << 16) | ((div255(sg * a) + div255(dg * inv)) << 8)
                 | (div255(sb * a) + div255(db * inv));
        }
    }
}

/**
 * @brief Backend copy_rect: copies premultiplied pixels into the framebuffer.
 *
 * @param[in] src     Source pixel buffer (premultiplied ARGB8888).
 * @param[in] stride  Source row stride in bytes.
 * @param[in] x       Destination left edge.
 * @param[in] y       Destination top edge.
 * @param[in] w       Width in pixels.
 * @param[in] h       Height in pixels.
 * @param[in] ctx     Pointer to TestCtx.
 */
static void copy_cb(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    TestCtx* t = ctx;
    for (int row = 0; row < h; row++)
    {
        const uint32_t* s = (const uint32_t*)((const uint8_t*)src + (size_t)row * (size_t)stride);
        for (int col = 0; col < w; col++)
        {
            const int dx = x + col, dy = y + row;
            if (dx >= 0 && dx < t->fb_w && dy >= 0 && dy < t->fb_h)
                t->fb[dy * t->fb_w + dx] = s[col];
        }
    }
}

/**
 * @brief Backend blend_rect: source-over composites premultiplied pixels scaled by a global alpha.
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
static void blend_cb(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    TestCtx* t = ctx;
    for (int row = 0; row < h; row++)
    {
        const uint32_t* s = (const uint32_t*)((const uint8_t*)src + (size_t)row * (size_t)stride);
        for (int col = 0; col < w; col++)
        {
            const int dx = x + col, dy = y + row;
            if (dx < 0 || dx >= t->fb_w || dy < 0 || dy >= t->fb_h)
                continue;
            const uint32_t sp = s[col];
            const uint32_t sa = div255(((sp >> 24) & 0xFFU) * (uint32_t)alpha);
            if (sa == 0U)
                continue;
            const uint32_t inv = 255U - sa;
            /* Source is premultiplied: scale its colour by the same global alpha, not by sa. */
            const uint32_t sr = div255(((sp >> 16) & 0xFFU) * (uint32_t)alpha);
            const uint32_t sg = div255(((sp >> 8) & 0xFFU) * (uint32_t)alpha);
            const uint32_t sb = div255((sp & 0xFFU) * (uint32_t)alpha);
            uint32_t* d = &t->fb[dy * t->fb_w + dx];
            const uint32_t dr = (*d >> 16) & 0xFFU, dg = (*d >> 8) & 0xFFU, db = *d & 0xFFU;
            *d = 0xFF000000U | ((sr + div255(dr * inv)) << 16) | ((sg + div255(dg * inv)) << 8)
                 | (sb + div255(db * inv));
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Prints a failure message.
 *
 * @param[in] msg  Message describing the failed assertion.
 *
 * @return EXIT_FAILURE, so callers can `return fail(...)`.
 */
static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return EXIT_FAILURE;
}

/**
 * @brief Returns an ERProps with every int16_t layout field set to ER_LAYOUT_AUTO.
 *
 * @return Initialised ERProps with opacity 255.
 */
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

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — case 1, the minimal reproduction
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Checks that a child laid out past its translucent group's box composites WITH the group.
 *
 * Then drives the exact sequence that used to diverge: a commit in which the group is clean but the
 * child still carries a dirty flag from a commit that never painted it.
 *
 * @param[in,out] t  TestCtx owning the framebuffer.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
static int test_overflowing_child(TestCtx* t)
{
    static uint32_t incremental[FB_W * FB_H];

    er_reset();
    memset(t->fb, 0, sizeof(uint32_t) * FB_W * FB_H);

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = FB_W;
    rp.height = FB_H;
    rp.background_color = 0xFFFFFFFFU; /* opaque white */
    er_node_set_props(root, &rp);

    /* Translucent group with a small box... */
    ERNode* group = er_node_create(ER_NODE_VIEW);
    ERProps gp = props_default();
    gp.position = ER_POS_ABSOLUTE;
    gp.left = 0;
    gp.top = 0;
    gp.width = 8;
    gp.height = 8;
    gp.background_color = 0x00000000U; /* transparent container */
    gp.opacity = 128U;
    er_node_set_props(group, &gp);

    /* ...and a child that lays out entirely OUTSIDE it. */
    ERNode* child = er_node_create(ER_NODE_VIEW);
    ERProps cp = props_default();
    cp.position = ER_POS_ABSOLUTE;
    cp.left = 12;
    cp.top = 12;
    cp.width = 12;
    cp.height = 12;
    cp.background_color = 0xFFFF0000U; /* opaque red */
    er_node_set_props(child, &cp);

    er_tree_append_child(group, child);
    er_tree_append_child(root, group);
    er_tree_set_root(root);
    er_commit();

    /* The group does not clip: the child paints, faded to the group alpha (red over white). */
    const uint32_t inside = t->fb[16 * FB_W + 16];
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
    memcpy(incremental, t->fb, sizeof(incremental));

    memset(t->fb, 0, sizeof(uint32_t) * FB_W * FB_H);
    er_force_full_repaint();
    er_commit();

    if (memcmp(incremental, t->fb, sizeof(incremental)) != 0)
        return fail("orphaned dirty child: incremental frame differs from a full repaint");

    return EXIT_SUCCESS;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — case 2, the seeded soak
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_rnd;

/**
 * @brief Linear-congruential random in [0, n).
 *
 * @param[in] n  Exclusive upper bound (must be non-zero).
 *
 * @return Pseudo-random value below @p n.
 */
static uint32_t rnd(uint32_t n)
{
    s_rnd = s_rnd * 1664525U + 1013904223U;
    return (s_rnd >> 16) % n;
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
    ERProps p = props_default();
    p.position = ER_POS_ABSOLUTE;
    if (rnd(5) < 2)
    {
        p.left = 0;
        p.top = 0;
        p.width = FZ_S;
        p.height = FZ_S;
    }
    else
    {
        p.left = (int16_t)rnd(FZ_S);
        p.top = (int16_t)rnd(FZ_S);
        p.width = (int16_t)(8 + rnd(FZ_S));
        p.height = (int16_t)(8 + rnd(FZ_S));
    }
    const uint32_t alpha = rnd(2) ? 0xFFU : (uint32_t)(40 + rnd(200));
    p.background_color = (alpha << 24) | rnd(0x1000000);
    if (rnd(4) == 0)
        p.border_radius = (int16_t)rnd(12);
    if (rnd(6) == 0)
        p.opacity = (uint8_t)(60 + rnd(190));
    if (rnd(5) == 0)
    {
        p.border_width = (int16_t)(1 + rnd(4));
        p.border_color = 0xFF000000U | rnd(0x1000000);
    }
    if (rnd(6) == 0)
        p.z_index = (int16_t)rnd(4);
    if (rnd(8) == 0)
        p.overflow = ER_OVERFLOW_HIDDEN;
    return p;
}

/**
 * @brief Runs one seed: build a random scene, then mutate/commit/compare FZ_STEPS times.
 *
 * @param[in]     seed   Scene seed.
 * @param[in,out] t      TestCtx owning the framebuffer.
 * @param[out]    step   Step index of the first mismatch (untouched when the seed passes).
 *
 * @return true when every incremental frame matched a full repaint.
 */
static bool run_seed(int seed, TestCtx* t, int* step)
{
    static uint32_t incremental[FZ_S * FZ_S];
    ERNode* nodes[FZ_NODES];
    ERProps props[FZ_NODES];
    ERNode* parents[FZ_NODES + 1];

    s_rnd = (uint32_t)seed * 2654435761U + 12345U;
    er_reset();
    memset(t->fb, 0, sizeof(uint32_t) * FZ_S * FZ_S);

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = FZ_S;
    rp.height = FZ_S;
    rp.background_color = 0xFF202020U;
    er_node_set_props(root, &rp);

    int np = 0;
    parents[np++] = root;
    for (int i = 0; i < FZ_NODES; i++)
    {
        ERNode* n = er_node_create(rnd(6) == 0 ? ER_NODE_TEXT : ER_NODE_VIEW);
        ERProps p = rand_box();
        if (er_node_get_type(n) == ER_NODE_TEXT)
        {
            snprintf(p.text, sizeof(p.text), "n%d", i);
            p.font_size = 12;
            p.color = 0xFFFFFFFFU;
        }
        er_node_set_props(n, &p);
        er_tree_append_child(parents[rnd((uint32_t)np)], n);
        parents[np++] = n;
        nodes[i] = n;
        props[i] = p;
    }
    er_tree_set_root(root);
    er_force_full_repaint();
    er_commit();

    for (int s = 0; s < FZ_STEPS; s++)
    {
        const int k = (int)rnd((uint32_t)FZ_NODES);
        ERProps p = props[k];
        switch (rnd(4))
        {
            case 0: /* visual */
                p.background_color = (p.background_color & 0xFF000000U) | rnd(0x1000000);
                break;
            case 1: /* move */
                p.left = (int16_t)rnd(FZ_S);
                p.top = (int16_t)rnd(FZ_S);
                break;
            case 2: /* resize */
                p.width = (int16_t)(8 + rnd(FZ_S));
                p.height = (int16_t)(8 + rnd(FZ_S));
                break;
            default: /* no-op layout */
                p.max_width = (int16_t)(FZ_S + 50);
                p.min_height = 1;
                break;
        }
        if (rnd(6) == 0) /* flip a layer between opaque and translucent: occlusion cull on/off */
        {
            const uint32_t a = ((p.background_color >> 24) == 0xFFU) ? 0x90U : 0xFFU;
            p.background_color = (a << 24) | (p.background_color & 0x00FFFFFFU);
        }
        if (rnd(8) == 0)
            p.border_radius = (int16_t)(p.border_radius ? 0 : 10);

        props[k] = p;
        er_node_set_props(nodes[k], &p);
        er_commit();
        memcpy(incremental, t->fb, sizeof(incremental));

        memset(t->fb, 0, sizeof(uint32_t) * FZ_S * FZ_S);
        er_force_full_repaint();
        er_commit();
        if (memcmp(incremental, t->fb, sizeof(incremental)) != 0)
        {
            *step = s;
            return false;
        }
        /* Continue the incremental history from the incremental frame, not the reference one. */
        memcpy(t->fb, incremental, sizeof(incremental));
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
    static uint32_t fb[FZ_S * FZ_S];
    TestCtx tc = {fb, FB_W, FB_H};
    EmbeddedRenderBackend be = {fill_cb, copy_cb, blend_cb, NULL, NULL, &tc};
    embedded_renderer_set_backend(&be);

    if (test_overflowing_child(&tc) != EXIT_SUCCESS)
        return EXIT_FAILURE;

    tc.fb_w = FZ_S;
    tc.fb_h = FZ_S;
    for (size_t i = 0; i < sizeof(k_known_bad_seeds) / sizeof(k_known_bad_seeds[0]); i++)
    {
        int step = -1;
        if (!run_seed(k_known_bad_seeds[i], &tc, &step))
        {
            char msg[128];
            snprintf(msg,
                     sizeof(msg),
                     "known-bad seed %d step %d: incremental frame differs from a full repaint",
                     k_known_bad_seeds[i],
                     step);
            return fail(msg);
        }
    }
    for (int seed = 1; seed <= FZ_SWEEP_SEEDS; seed++)
    {
        int step = -1;
        if (!run_seed(seed, &tc, &step))
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "seed %d step %d: incremental frame differs from a full repaint", seed, step);
            return fail(msg);
        }
    }

    return EXIT_SUCCESS;
}
