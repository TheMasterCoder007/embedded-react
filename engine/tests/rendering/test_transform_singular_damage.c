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
 * Issue #138: a transform too SINGULAR to invert paints untransformed, but damage measured its AABB.
 *
 * render_tree paints a scale/rotate/3D node by capturing its subtree into the transform scratch and
 * inverse-mapping it back out. Three things stop that capture: the node is too large for the source,
 * an ancestor already holds it, or — this one — the matrix cannot be inverted, so there is no inverse
 * map to blit with (|det| < 1e-6 affine, < 1e-7 for the 3D homography). On any of them it degrades to
 * painting the node untransformed at its raw layout box.
 *
 * The damage pre-pass predicted the first two and was blind to the third, so it damaged the
 * (sub-pixel) AABB while the paint laid down the full raw box, scissored to that damage — and the tear
 * never healed: the fallback paint recorded the raw box with last_paint_untransformed set, from which
 * the next commit's prediction agreed with last_paint_rect, `moved` stayed false, and the node was
 * never source-dirty again. Zero damage, forever.
 *
 * Each case crosses the invert threshold in one commit and asserts the damage-clipped frame is
 * byte-identical to a forced full repaint of the same scene state, that every changed pixel was
 * reported through er_get_dirty_rects(), and that idle commits neither heal it nor churn. Both
 * directions are covered — into the singular band and back out of it — and each singular case is
 * paired with a control just the other side of the epsilon, where the invert succeeds and the AABB
 * really is the footprint.
 *
 * er_transform_is_invertible() is asserted against every case up front, so a scenario that stopped
 * straddling the threshold (a changed epsilon, a changed matrix) fails as a broken setup rather than
 * quietly passing on both sides of a test that no longer tests anything.
 *
 * Requires ERUI_TRANSFORMS=FULL — without the capture path there is no inverse map to fail.
 */

#include "er_node_internal.h" /* last_paint_untransformed — asserts the fallback paint really happened */
#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h" /* er_force_full_repaint — the reference frame */
#include "transform.h"         /* er_transform_is_invertible / _source_fits — asserts the path taken */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN 200
#define FB_PIXELS (SCREEN * SCREEN)

/* Node geometry. The width is ODD so the default centre pivot lands on a half pixel: a matrix that
 * collapses the box to a line then still rounds out to a 1px-wide AABB instead of a zero-area one,
 * which would take the pre-pass's full-repaint fallback and heal the bug it is meant to expose. */
#define NODE_X 60
#define NODE_Y 60
#define NODE_W 101
#define NODE_H 100

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

/* Everything from here down is the test proper: without a capture path there is no inverse map to
 * refuse, so main() skips outright and none of it is built. Only the backend above stays unconditional. */
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

/** @brief One transform state: what a scenario settles at, and what it commits to. */
typedef struct
{
    float scale;     /**< 0 = unset (identity scale). */
    float rot_z;     /**< Degrees. */
    float rot_y;     /**< Degrees (3D; ignored unless ERUI_3D_TRANSFORMS). */
    bool invertible; /**< What er_transform_is_invertible() must answer for it. */
} Xform;

/** @brief A settled state, the state one commit moves it to, and what the pair is called. */
typedef struct
{
    Xform from, to;
    const char* name;
} Scenario;

/** @brief Applies a transform state to a props block. */
static void set_transform(ERProps* p, const Xform* x)
{
    p->transform_scale_x = x->scale;
    p->transform_scale_y = x->scale;
    p->transform_rotate_z = x->rot_z;
#if ERUI_3D_TRANSFORMS
    p->transform_rotate_y = x->rot_y;
#else
    (void)x->rot_y;
#endif
}

/**
 * @brief Drives one node across the invert threshold and demands a full-repaint match.
 *
 * @param[in] s  Scenario to run.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
static int check_singular_crossing(const Scenario* s)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = SCREEN;
    rp.height = SCREEN;
    rp.background_color = 0xFFFFFFFFU; /* opaque: a full repaint really covers the screen */
    er_node_set_props(root, &rp);

    ERNode* node = er_node_create(ER_NODE_VIEW);
    ERProps np = props_default();
    np.position = ER_POS_ABSOLUTE;
    np.left = NODE_X;
    np.top = NODE_Y;
    np.width = NODE_W;
    np.height = NODE_H;
    np.background_color = 0xFF2244AAU;
    set_transform(&np, &s->from);
    er_node_set_props(node, &np);

    er_tree_append_child(root, node);
    er_tree_set_root(root);

    er_force_full_repaint();
    er_commit();
    er_commit(); /* settle: a last-painted footprint and a settled fallback flag */

    /* The scenario only tests anything if both states really sit where it says they do. The node has
     * no scroll or keyboard shift above it, so its layout position IS the origin render_tree hands the
     * matrix. */
    const bool from_ok = er_transform_is_invertible(node, NODE_X, NODE_Y, NODE_W, NODE_H) == s->from.invertible;
    if (!from_ok || node->last_paint_untransformed == s->from.invertible)
    {
        fprintf(stderr,
                "  setup: settled state invertible=%d (wanted %d), last_paint_untransformed=%d\n",
                (int)er_transform_is_invertible(node, NODE_X, NODE_Y, NODE_W, NODE_H),
                (int)s->from.invertible,
                (int)node->last_paint_untransformed);
        er_node_destroy(root);
        return fail("the settled state did not take the capture path the scenario names");
    }

    static uint32_t before[FB_PIXELS];
    memcpy(before, s_fb, sizeof(before));

    set_transform(&np, &s->to);
    er_node_set_props(node, &np);

    if (er_transform_is_invertible(node, NODE_X, NODE_Y, NODE_W, NODE_H) != s->to.invertible)
    {
        er_node_destroy(root);
        return fail("the committed state did not land on the side of the epsilon the scenario names");
    }

    er_commit(); /* the damage-clipped commit under test */

    ERRect rects[ER_DAMAGE_RECTS_MAX];
    const int rect_count = er_get_dirty_rects(rects, ER_DAMAGE_RECTS_MAX);
    const bool painted_raw = node->last_paint_untransformed;

    static uint32_t inc[FB_PIXELS];
    memcpy(inc, s_fb, sizeof(inc));

    /* Idle commits must not be what heals it — nor damage anything of their own. The bug survived
     * every one of them, so more than a single tick is worth asking for. */
    er_commit();
    er_commit();
    er_commit();
    const int idle_differ = diff_frames(s_fb, inc, NULL);

    er_force_full_repaint();
    er_commit(); /* reference: the same scene state, painted in full */

    ERRect bbox = {0, 0, 0, 0};
    const int differ = diff_frames(inc, s_fb, &bbox);
    const int missed = unreported_pixels(before, inc, rects, rect_count);
    const int changed = diff_frames(before, s_fb, NULL); /* the crossing has to move pixels at all */
    const int painted = reported_area(rects, rect_count);

    printf("%s: %d px differ from a full repaint%s; %d rect(s) over %d px reported, %d changed px "
           "unreported; 3 idle commits moved %d px (%d px changed in all; paint used the %s box)\n",
           s->name,
           differ,
           differ ? "" : " (identical)",
           rect_count,
           painted,
           missed,
           idle_differ,
           changed,
           painted_raw ? "raw" : "transformed");
    if (differ)
        printf("  divergence bbox %d,%d %dx%d\n", bbox.x, bbox.y, bbox.w, bbox.h);

    er_node_destroy(root);

    if (changed == 0)
        return fail("crossing the invert threshold changed nothing on screen — the case proves nothing");
    /* The paint has to have done what the pre-pass was asked to predict, or a passing frame says
     * nothing about the prediction. */
    if (painted_raw == s->to.invertible)
        return fail("the commit under test did not take the paint path its matrix implies");
    if (differ != 0)
        return fail("a transform that crossed the invert threshold diverged from a full repaint — "
                    "damage measured a footprint the paint never used");
    if (missed != 0)
        return fail("pixels changed outside every reported dirty rect — a partial-update host would "
                    "leave the panel stale");
    if (idle_differ != 0)
        return fail("an idle commit repainted differently — the transformed node is churning");
    /* Asked last, and it is the reason the ones above are worth asking: widening the damage to the
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
    /* Vacuous unless the node really takes the capture path when its matrix does invert. */
    if (!er_transform_source_fits(NODE_W, NODE_H))
    {
        printf("SKIP: a %dx%d node does not fit this build's transform scratch\n", NODE_W, NODE_H);
        return EXIT_SUCCESS;
    }

    static const Scenario k_scenarios[] = {
        /* det = 2.5e-7, an order of magnitude under the 1e-6 affine epsilon: the capture starts (the
         * node fits) and the inverse-map blit is what refuses, which is the hole. */
        {{0.5f, 0.0f, 0.0f, true}, {0.0005f, 0.0f, 0.0f, false}, "affine scale .5 -> .0005 (singular)"},
        /* The control, just the other side: det = 4e-6, the invert succeeds and the AABB really is
         * the footprint. Passes with or without the fix — it is here to prove the pair straddles the
         * epsilon rather than both landing on the broken side of it. */
        {{0.5f, 0.0f, 0.0f, true}, {0.002f, 0.0f, 0.0f, true}, "affine scale .5 -> .002 (invertible)"},
        /* Back out of the singular band: the node has been fallback-painting its raw box and now
         * blows up to an AABB well outside it, so the abandoned box has to be erased too. */
        {{0.0005f, 0.0f, 0.0f, false}, {1.6f, 0.0f, 0.0f, true}, "affine scale .0005 -> 1.6 (heals)"},
        /* Rotation in the mix, so the singular AABB is not simply a shrunken copy of the raw box. */
        {{1.2f, 30.0f, 0.0f, true}, {0.0008f, 30.0f, 0.0f, false}, "affine rot 30, scale 1.2 -> .0008"},
#if ERUI_3D_TRANSFORMS
        /* The same hole on the 3D path: at rotate_y 90 the plane is edge-on, the homography's det
         * (~4.4e-8 orthographic) falls under the 1e-7 3D epsilon, and er_transform_homography_invert
         * refuses. The projected AABB is a full-height 1px slice — non-degenerate, so the pre-pass
         * bounds it happily and scissors the raw-box paint down to a sliver. */
        {{0.0f, 0.0f, 45.0f, true}, {0.0f, 0.0f, 90.0f, false}, "3D rotateY 45 -> 90 (singular)"},
        /* The 3D control, 10 degrees off edge-on: det ~0.17, comfortably invertible. */
        {{0.0f, 0.0f, 45.0f, true}, {0.0f, 0.0f, 80.0f, true}, "3D rotateY 45 -> 80 (invertible)"},
        /* And back out of it. */
        {{0.0f, 0.0f, 90.0f, false}, {0.0f, 0.0f, 30.0f, true}, "3D rotateY 90 -> 30 (heals)"},
#endif
    };

    for (size_t i = 0; i < sizeof(k_scenarios) / sizeof(k_scenarios[0]); i++)
    {
        const int rc = check_singular_crossing(&k_scenarios[i]);
        if (rc != EXIT_SUCCESS)
            return rc;
    }

    printf("PASS: a transform crossing the invert threshold matches a full repaint\n");
#else
    printf("SKIP: ERUI_TRANSFORMS=FULL required (no capture path, no inverse map to refuse)\n");
#endif
    return EXIT_SUCCESS;
}
