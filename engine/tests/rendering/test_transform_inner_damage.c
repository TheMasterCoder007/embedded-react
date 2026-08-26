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
#define FB_PIXELS (SCREEN * SCREEN)

/*----------------------------------------------------------------------------------------------------------------------
 - Backend: source-over framebuffer
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_fb[FB_PIXELS];

/** @brief Rounds a 0-65025 product back to 0-255 the way the engine's blenders do. */
static uint32_t div255(uint32_t v)
{
    return (v + 127U) / 255U;
}

/** @brief Backend fill_rect: source-over composites a straight-alpha colour. */
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const uint32_t a = (argb >> 24) & 0xFFU;
    if (a == 0U || w <= 0 || h <= 0)
        return;
    const uint32_t inv = 255U - a;
    const uint32_t sr = (argb >> 16) & 0xFFU, sg = (argb >> 8) & 0xFFU, sb = argb & 0xFFU;
    for (int row = y; row < y + h; row++)
    {
        if (row < 0 || row >= SCREEN)
            continue;
        for (int col = x; col < x + w; col++)
        {
            if (col < 0 || col >= SCREEN)
                continue;
            uint32_t* d = &s_fb[row * SCREEN + col];
            if (a == 0xFFU)
            {
                *d = 0xFF000000U | (argb & 0x00FFFFFFU);
                continue;
            }
            const uint32_t dr = (*d >> 16) & 0xFFU, dg = (*d >> 8) & 0xFFU, db = *d & 0xFFU;
            *d = 0xFF000000U | ((div255(sr * a) + div255(dr * inv)) << 16) | ((div255(sg * a) + div255(dg * inv)) << 8)
                 | (div255(sb * a) + div255(db * inv));
        }
    }
}

/** @brief Backend copy_rect: source-over composites premultiplied scratch — this is how the blit lands. */
static void copy_cb(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    for (int row = 0; row < h; row++)
    {
        const uint32_t* s = (const uint32_t*)((const uint8_t*)src + (size_t)row * (size_t)stride);
        for (int col = 0; col < w; col++)
        {
            const int dx = x + col, dy = y + row;
            if (dx < 0 || dx >= SCREEN || dy < 0 || dy >= SCREEN)
                continue;
            const uint32_t sp = s[col];
            const uint32_t sa = (sp >> 24) & 0xFFU;
            if (sa == 0U)
                continue; /* fully transparent: leave the destination alone */
            uint32_t* d = &s_fb[dy * SCREEN + dx];
            if (sa == 0xFFU)
            {
                *d = sp;
                continue;
            }
            const uint32_t inv = 255U - sa;
            const uint32_t dr = (*d >> 16) & 0xFFU, dg = (*d >> 8) & 0xFFU, db = *d & 0xFFU;
            *d = 0xFF000000U | ((((sp >> 16) & 0xFFU) + div255(dr * inv)) << 16)
                 | ((((sp >> 8) & 0xFFU) + div255(dg * inv)) << 8) | ((sp & 0xFFU) + div255(db * inv));
        }
    }
}

/** @brief Backend blend_rect: source-over composites premultiplied pixels scaled by a global alpha. */
static void blend_cb(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    for (int row = 0; row < h; row++)
    {
        const uint32_t* s = (const uint32_t*)((const uint8_t*)src + (size_t)row * (size_t)stride);
        for (int col = 0; col < w; col++)
        {
            const int dx = x + col, dy = y + row;
            if (dx < 0 || dx >= SCREEN || dy < 0 || dy >= SCREEN)
                continue;
            const uint32_t sp = s[col];
            const uint32_t sa = div255(((sp >> 24) & 0xFFU) * (uint32_t)alpha);
            if (sa == 0U)
                continue;
            const uint32_t inv = 255U - sa;
            const uint32_t sr = div255(((sp >> 16) & 0xFFU) * (uint32_t)alpha);
            const uint32_t sg = div255(((sp >> 8) & 0xFFU) * (uint32_t)alpha);
            const uint32_t sb = div255((sp & 0xFFU) * (uint32_t)alpha);
            uint32_t* d = &s_fb[dy * SCREEN + dx];
            const uint32_t dr = (*d >> 16) & 0xFFU, dg = (*d >> 8) & 0xFFU, db = *d & 0xFFU;
            *d = 0xFF000000U | ((sr + div255(dr * inv)) << 16) | ((sg + div255(dg * inv)) << 8)
                 | (sb + div255(db * inv));
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

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

static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return EXIT_FAILURE;
}

/**
 * @brief Reports how two frames differ, and where.
 *
 * @param[in]  a       First frame.
 * @param[in]  b       Second frame.
 * @param[out] bbox    Receives the bounding box of the differing pixels (untouched when identical).
 *
 * @return Number of differing pixels.
 */
static int diff_frames(const uint32_t* a, const uint32_t* b, ERRect* bbox)
{
    int n = 0, x0 = SCREEN, y0 = SCREEN, x1 = -1, y1 = -1;
    for (int y = 0; y < SCREEN; y++)
    {
        for (int x = 0; x < SCREEN; x++)
        {
            if (a[y * SCREEN + x] == b[y * SCREEN + x])
                continue;
            n++;
            if (x < x0)
                x0 = x;
            if (y < y0)
                y0 = y;
            if (x > x1)
                x1 = x;
            if (y > y1)
                y1 = y;
        }
    }
    if (n > 0 && bbox)
    {
        bbox->x = x0;
        bbox->y = y0;
        bbox->w = x1 - x0 + 1;
        bbox->h = y1 - y0 + 1;
    }
    return n;
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
            for (int i = 0; i < count && !covered; i++)
                covered =
                    (x >= rects[i].x && x < rects[i].x + rects[i].w && y >= rects[i].y && y < rects[i].y + rects[i].h);
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
    MUT_RECOLOUR, /**< Pure repaint: source_dirty, same geometry — the issue's own reproduction. */
    MUT_RESIZE,   /**< source_dirty AND moved: its own footprint changes inside the source. */
    MUT_HIDE,     /**< display:none — vacated pixels, registered from the last painted rect. */
    MUT_REMOVE,   /**< Unmounted — the same vacated-pixel channel, reached before detach. */
    MUT_SIBLING   /**< A SIBLING grows and reflows it: moved, never source_dirty itself. */
} Mutation;

/**
 * @brief Mutates only the inside of a rotated ancestor and demands a full-repaint match.
 *
 * Both children are laid out in the ancestor's flex column so MUT_SIBLING can push the second one
 * down; the other mutations touch that second child alone and the first is inert scenery.
 *
 * @param[in] ox,oy  Outer node position.
 * @param[in] rot    Outer node rotation in degrees.
 * @param[in] mut    Which mutation to apply.
 * @param[in] label  Printed name for the case.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
static int check_inner_mutation(int ox, int oy, float rot, Mutation mut, const char* label)
{
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = SCREEN;
    rp.height = SCREEN;
    rp.background_color = 0xFFFFFFFFU; /* opaque: a full repaint really covers the screen */
    er_node_set_props(root, &rp);

    /* Outer: comfortably inside the transform scratch, so its capture succeeds and its whole subtree
     * renders in SOURCE space behind it. */
    ERNode* outer = er_node_create(ER_NODE_VIEW);
    ERProps op = props_default();
    op.position = ER_POS_ABSOLUTE;
    op.left = (int16_t)ox;
    op.top = (int16_t)oy;
    op.width = 100;
    op.height = 100;
    op.background_color = 0xFF2244AAU;
    op.transform_rotate_z = rot;
    er_node_set_props(outer, &op);

    /* Spacer: only MUT_SIBLING touches it; it is what reflows the inner node without dirtying it. */
    ERNode* spacer = er_node_create(ER_NODE_VIEW);
    ERProps sp = props_default();
    sp.width = 100;
    sp.height = 20;
    sp.background_color = 0xFF884400U;
    er_node_set_props(spacer, &sp);

    /* Inner: no transform of its own — the whole point is that it is measured in the ancestor's
     * source space while its pixels land at the ancestor-transformed position. */
    ERNode* inner = er_node_create(ER_NODE_VIEW);
    ERProps ip = props_default();
    ip.width = 60;
    ip.height = 50;
    ip.margin_left = 20;
    ip.background_color = 0xFF33FF33U;
    er_node_set_props(inner, &ip);

    er_tree_append_child(outer, spacer);
    er_tree_append_child(outer, inner);
    er_tree_append_child(root, outer);
    er_tree_set_root(root);

    er_force_full_repaint();
    er_commit();
    er_commit(); /* settle: every node has a last-painted footprint */

    static uint32_t before[FB_PIXELS];
    memcpy(before, s_fb, sizeof(before));

    switch (mut)
    {
        case MUT_RECOLOUR:
            ip.background_color = 0xFFFF2222U;
            er_node_set_props(inner, &ip);
            break;
        case MUT_RESIZE:
            ip.width = 30;
            ip.height = 25;
            er_node_set_props(inner, &ip);
            break;
        case MUT_HIDE:
            ip.display = ER_DISPLAY_NONE;
            er_node_set_props(inner, &ip);
            break;
        case MUT_REMOVE:
            er_tree_remove_child(outer, inner);
            break;
        case MUT_SIBLING:
            sp.height = 45; /* pushes the inner node down inside the capture; it stays clean */
            er_node_set_props(spacer, &sp);
            break;
    }

    er_commit(); /* the damage-clipped commit under test */

    ERRect rects[ER_DAMAGE_RECTS_MAX];
    const int rect_count = er_get_dirty_rects(rects, ER_DAMAGE_RECTS_MAX);

    static uint32_t inc[FB_PIXELS];
    memcpy(inc, s_fb, sizeof(inc));

    er_force_full_repaint();
    er_commit(); /* reference: the same scene state, painted in full */

    ERRect bbox = {0, 0, 0, 0};
    const int differ = diff_frames(inc, s_fb, &bbox);
    const int missed = unreported_pixels(before, inc, rects, rect_count);

    /* An idle commit must not be what heals it — nor damage anything of its own. */
    memcpy(s_fb, inc, sizeof(inc));
    er_commit();
    const int idle_differ = diff_frames(s_fb, inc, NULL);

    printf("%s: %d px differ from a full repaint%s; %d rect(s) reported, %d changed px unreported; "
           "idle commit moved %d px\n",
           label,
           differ,
           differ ? "" : " (identical)",
           rect_count,
           missed,
           idle_differ);
    if (differ)
        printf("  divergence bbox %d,%d %dx%d\n", bbox.x, bbox.y, bbox.w, bbox.h);

    if (mut != MUT_REMOVE)
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

int main(void)
{
    EmbeddedRenderBackend be = {0};
    be.fill_rect = fill_cb;
    be.copy_rect = copy_cb;
    be.blend_rect = blend_cb;
    embedded_renderer_set_backend(&be);

#if ERUI_TRANSFORMS_FULL
    /* Vacuous unless the outer node really takes the capture path. */
    if (!er_transform_source_fits(100, 100))
    {
        printf("SKIP: a 100x100 node does not fit this build's transform scratch\n");
        return EXIT_SUCCESS;
    }

    static const struct
    {
        Mutation mut;
        const char* name;
    } k_muts[] = {
        {MUT_RECOLOUR, "recolour"},
        {MUT_RESIZE, "resize"},
        {MUT_HIDE, "hide"},
        {MUT_REMOVE, "remove"},
        {MUT_SIBLING, "sibling reflow"},
    };

    /* Two placements: the second is far enough from the layout box that a rect measured in source
     * space and one measured on screen barely overlap. */
    static const struct
    {
        int x, y;
        float rot;
    } k_places[] = {{10, 10, 20.0f}, {80, 80, 45.0f}};

    for (size_t p = 0; p < sizeof(k_places) / sizeof(k_places[0]); p++)
    {
        for (size_t m = 0; m < sizeof(k_muts) / sizeof(k_muts[0]); m++)
        {
            char label[96];
            snprintf(label,
                     sizeof(label),
                     "outer @(%d,%d) rot %g / %s",
                     k_places[p].x,
                     k_places[p].y,
                     (double)k_places[p].rot,
                     k_muts[m].name);
            const int rc = check_inner_mutation(k_places[p].x, k_places[p].y, k_places[p].rot, k_muts[m].mut, label);
            if (rc != EXIT_SUCCESS)
                return rc;
        }
    }

    printf("PASS: inner-only mutations under a transformed ancestor match a full repaint\n");
#else
    printf("SKIP: ERUI_TRANSFORMS=FULL required (no transform capture, no source space)\n");
#endif
    return EXIT_SUCCESS;
}
