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
 * Issue #139: an ANCESTOR's transform change flips a nested transformed node's footprint.
 *
 * Only the outermost transform that fits the scratch and inverts gets to capture; a transformed node
 * inside one is refused and paints its raw box in the ancestor's source space instead. So which
 * footprint a nested node paints — its transformed AABB, on screen, or its raw box, inside someone
 * else's capture — is decided ABOVE it. An ancestor gaining or losing a transform flips it without
 * ever dirtying the node or moving its layout box.
 *
 * The damage pre-pass predicted that from last_paint_untransformed, the flag the previous paint left
 * behind, and only ever consulted it inside `if (n->source_dirty || moved)`. An ancestor-side change
 * satisfies neither: the node is clean, and the (stale) prediction still agrees with last_paint_rect,
 * so it reads as idle and contributes nothing. render_tree then repaints it anyway — the whole subtree
 * under a dirty ancestor repaints — onto the OTHER footprint, scissored to whatever damage the
 * ancestor happened to contribute, and records that footprint. From then on the prediction agrees with
 * the new rect, `moved` stays false, and the half-drawn frame never heals.
 *
 * Both directions are covered, because the staleness runs both ways:
 *
 *   - the ancestor LOSES its transform: the child stops being fallback-painted and takes the capture
 *     itself, so its pixels appear at an AABB nothing damaged;
 *   - the ancestor GAINS one: the child stops capturing and paints into the ancestor's source, so the
 *     AABB it used to blit is abandoned and nothing erases the trail.
 *
 * Each case asserts the damage-clipped commit is byte-identical to a forced full repaint of the same
 * scene state, that every changed pixel was reported through er_get_dirty_rects(), and that a
 * following idle commit neither heals it nor churns.
 *
 * Requires ERUI_TRANSFORMS=FULL — without the capture path there is no source space, nothing to
 * refuse, and no flip.
 */

#include "er_node_internal.h" /* last_paint_untransformed — asserts each case really sets up the flip */
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

/* Everything from here down is the test proper: without a capture path there is no fallback to flip,
 * so main() skips outright and none of it is built. Only the backend above stays unconditional. */
#if ERUI_TRANSFORMS_FULL

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
 * @param[in]  a     First frame.
 * @param[in]  b     Second frame.
 * @param[out] bbox  Receives the bounding box of the differing pixels (untouched when identical).
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

/**
 * @brief Counts the screen pixels covered by at least one reported dirty rect.
 *
 * @param[in] rects  Rects er_get_dirty_rects() reported.
 * @param[in] count  Number of rects.
 *
 * @return Number of covered pixels.
 */
static int reported_area(const ERRect* rects, int count)
{
    int area = 0;
    for (int y = 0; y < SCREEN; y++)
    {
        for (int x = 0; x < SCREEN; x++)
        {
            for (int i = 0; i < count; i++)
            {
                if (x >= rects[i].x && x < rects[i].x + rects[i].w && y >= rects[i].y && y < rects[i].y + rects[i].h)
                {
                    area++;
                    break;
                }
            }
        }
    }
    return area;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Test
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Which way the ancestor's transform flips across the commit under test. */
typedef enum
{
    FLIP_LOSE, /**< Outer starts transformed (inner is fallback-painted) and goes to identity. */
    FLIP_GAIN  /**< Outer starts plain (inner captures) and gains a transform. */
} Flip;

/** @brief One scenario: an outer/inner geometry plus the transform the outer flips in and out of. */
typedef struct
{
    int ox, oy;           /**< Outer position (absolute, in the root). */
    int ix, iy;           /**< Inner position (absolute, inside the outer). */
    float o_rot, o_scale; /**< The outer's transform while it has one. */
    float i_rot, i_scale; /**< The inner's transform, unchanged throughout. */
    const char* name;
} Scenario;

/** @brief Applies (or clears) the outer's transform on a props block. */
static void set_outer_transform(ERProps* p, const Scenario* s, bool on)
{
    p->transform_rotate_z = on ? s->o_rot : 0.0f;
    p->transform_scale_x = on ? s->o_scale : 0.0f;
    p->transform_scale_y = on ? s->o_scale : 0.0f;
}

/**
 * @brief Flips an ancestor's transform over a nested transformed child and demands a full-repaint match.
 *
 * @param[in] s     Scenario geometry.
 * @param[in] flip  Direction of the flip.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
static int check_ancestor_flip(const Scenario* s, Flip flip)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = SCREEN;
    rp.height = SCREEN;
    rp.background_color = 0xFFFFFFFFU; /* opaque: a full repaint really covers the screen */
    er_node_set_props(root, &rp);

    /* Outer: fits the transform scratch comfortably, so whenever it has a transform it is the one
     * that captures — and the inner one below is refused. */
    ERNode* outer = er_node_create(ER_NODE_VIEW);
    ERProps op = props_default();
    op.position = ER_POS_ABSOLUTE;
    op.left = (int16_t)s->ox;
    op.top = (int16_t)s->oy;
    op.width = 100;
    op.height = 100;
    op.background_color = 0xFF2244AAU;
    set_outer_transform(&op, s, flip == FLIP_LOSE);
    er_node_set_props(outer, &op);

    /* Inner: its own transform never changes. Whether it paints its AABB on screen or its raw box
     * inside the outer's source is decided entirely by the outer. */
    ERNode* inner = er_node_create(ER_NODE_VIEW);
    ERProps ip = props_default();
    ip.position = ER_POS_ABSOLUTE;
    ip.left = (int16_t)s->ix;
    ip.top = (int16_t)s->iy;
    ip.width = 60;
    ip.height = 60;
    ip.background_color = 0xFF33FF33U;
    ip.transform_rotate_z = s->i_rot;
    ip.transform_scale_x = s->i_scale;
    ip.transform_scale_y = s->i_scale;
    er_node_set_props(inner, &ip);

    er_tree_append_child(outer, inner);
    er_tree_append_child(root, outer);
    er_tree_set_root(root);

    er_force_full_repaint();
    er_commit();
    er_commit(); /* settle: every node has a last-painted footprint and a settled fallback flag */

    /* The case only tests anything if the starting state really is the one it names. */
    const bool want_fallback = (flip == FLIP_LOSE);
    if (!inner->has_last_paint || inner->last_paint_untransformed != want_fallback)
    {
        fprintf(stderr,
                "  setup: inner has_last_paint=%d last_paint_untransformed=%d (wanted %d)\n",
                (int)inner->has_last_paint,
                (int)inner->last_paint_untransformed,
                (int)want_fallback);
        er_node_destroy(root);
        return fail("the scenario did not set up the capture state it names");
    }

    static uint32_t before[FB_PIXELS];
    memcpy(before, s_fb, sizeof(before));

    set_outer_transform(&op, s, flip == FLIP_GAIN);
    er_node_set_props(outer, &op);

    er_commit(); /* the damage-clipped commit under test */

    ERRect rects[ER_DAMAGE_RECTS_MAX];
    const int rect_count = er_get_dirty_rects(rects, ER_DAMAGE_RECTS_MAX);

    static uint32_t inc[FB_PIXELS];
    memcpy(inc, s_fb, sizeof(inc));

    /* An idle commit must not be what heals it — nor damage anything of its own. */
    er_commit();
    const int idle_differ = diff_frames(s_fb, inc, NULL);

    er_force_full_repaint();
    er_commit(); /* reference: the same scene state, painted in full */

    ERRect bbox = {0, 0, 0, 0};
    const int differ = diff_frames(inc, s_fb, &bbox);
    const int missed = unreported_pixels(before, inc, rects, rect_count);
    const int changed = diff_frames(before, s_fb, NULL); /* the flip has to move pixels at all */
    const int painted = reported_area(rects, rect_count);

    printf("%s / outer %s transform: %d px differ from a full repaint%s; %d rect(s) over %d px "
           "reported, %d changed px unreported; idle commit moved %d px (%d px changed in all)\n",
           s->name,
           flip == FLIP_LOSE ? "loses" : "gains",
           differ,
           differ ? "" : " (identical)",
           rect_count,
           painted,
           missed,
           idle_differ,
           changed);
    if (differ)
        printf("  divergence bbox %d,%d %dx%d\n", bbox.x, bbox.y, bbox.w, bbox.h);

    er_node_destroy(root);

    if (changed == 0)
        return fail("the ancestor's transform flip changed nothing on screen — the case proves nothing");
    if (differ != 0)
        return fail("an ancestor-side transform flip diverged from a full repaint — the nested node "
                    "changed footprint without contributing damage");
    if (missed != 0)
        return fail("pixels changed outside every reported dirty rect — a partial-update host would "
                    "leave the panel stale");
    if (idle_differ != 0)
        return fail("an idle commit repainted differently — the transformed subtree is churning");
    /* Asked last, and it is the reason the three above are worth asking: widening the damage to the
     * whole root satisfies every one of them and gives up exactly what the pre-pass exists for. */
    if (painted >= FB_PIXELS)
        return fail("the commit repainted the entire root — correct, but no longer damage-clipped");
    return EXIT_SUCCESS;
}

#endif /* ERUI_TRANSFORMS_FULL */

int main(void)
{
    EmbeddedRenderBackend be = {0};
    be.fill_rect = fill_cb;
    be.copy_rect = copy_cb;
    be.blend_rect = blend_cb;
    embedded_renderer_set_backend(&be);

#if ERUI_TRANSFORMS_FULL
    /* Vacuous unless the outer node really takes the capture path. */
    if (!er_transform_source_fits(100, 100) || !er_transform_source_fits(60, 60))
    {
        printf("SKIP: a 100x100 outer / 60x60 inner do not fit this build's transform scratch\n");
        return EXIT_SUCCESS;
    }

    static const Scenario k_scenarios[] = {
        /* The inner blows up to 120x120 when it captures, so its AABB reaches well past every rect
         * the outer's own damage can contribute. */
        {40, 40, 20, 20, 0.0f, 0.5f, 0.0f, 2.0f, "outer scale .5, inner scale 2"},
        /* Rotation on both: neither footprint is a scaled copy of the other, and the outer's old
         * AABB covering the inner's new one would be pure coincidence. */
        {10, 10, 20, 20, 20.0f, 0.0f, 45.0f, 1.5f, "outer rot 20, inner rot 45 scale 1.5"},
        /* The inner tucked into a corner of the source, so its AABB leaves the outer's box entirely. */
        {50, 50, 50, 50, 30.0f, 0.6f, 0.0f, 1.6f, "outer rot 30 scale .6, inner corner scale 1.6"},
    };

    for (size_t i = 0; i < sizeof(k_scenarios) / sizeof(k_scenarios[0]); i++)
    {
        const int rc_lose = check_ancestor_flip(&k_scenarios[i], FLIP_LOSE);
        if (rc_lose != EXIT_SUCCESS)
            return rc_lose;
        const int rc_gain = check_ancestor_flip(&k_scenarios[i], FLIP_GAIN);
        if (rc_gain != EXIT_SUCCESS)
            return rc_gain;
    }

    printf("PASS: an ancestor gaining or losing a transform over a nested transformed node matches a "
           "full repaint\n");
#else
    printf("SKIP: ERUI_TRANSFORMS=FULL required (no transform capture, no fallback to flip)\n");
#endif
    return EXIT_SUCCESS;
}
