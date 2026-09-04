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
 * Occlusion culling: a fully opaque node that covers the region being repainted buries everything
 * painted before it, so those layers are skipped entirely instead of being drawn and immediately
 * overwritten. The backend here logs every fill it is handed, which makes "was this layer drawn?"
 * directly observable — a buried layer's colour must simply never reach the backend.
 *
 *   - culled: an opaque full-cover child skips its parent's background and its earlier siblings,
 *   - not culled when the cover cannot prove coverage: translucent, rounded corners (the corner
 *     squares show through), or smaller than the region,
 *   - partial: only the siblings BELOW the cover are skipped, the ones above still paint,
 *   - flags retire: a buried node that changed does not re-damage the same pixels forever, and a
 *     TRANSFORMED sibling is never buried at all (it has no box-shaped footprint to record),
 *   - pixel equivalence: the culled frame is byte-identical to one rendered with everything drawn.
 *
 * The assertions that a layer WAS skipped are the only ones gated on ERUI_OCCLUSION_CULLING; every
 * "must still paint" and "must look identical" check has to hold with the cull compiled out too.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h" /* er_force_full_repaint */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN 200

/* Distinct, easily-recognised layer colours (opaque unless a test overrides the alpha). */
#define C_ROOT 0xFFFFFFFFU  /* root background      */
#define C_DEEP 0xFF0000FFU  /* buried mid layer     */
#define C_INNER 0xFFFF0000U /* buried leaf inside it */
#define C_COVER 0xFF00FF00U /* the opaque occluder  */
#define C_TOP 0xFF00FFFFU   /* sibling ABOVE it     */

/*----------------------------------------------------------------------------------------------------------------------
 - Backend: source-over framebuffer + a log of every fill colour handed to it
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_fb[SCREEN * SCREEN];
static uint32_t s_fills[4096];
static int s_fill_count;
/* Every backend op, not just fills: a transformed node reaches the panel through copy/blend, so a
 * fill-only counter would score "painted nothing" for a subtree that painted plenty. */
static int s_op_count;

static void log_reset(void)
{
    s_fill_count = 0;
    s_op_count = 0;
}

/** @brief Whether any fill this frame used the given colour (alpha included). */
static bool drew(uint32_t argb)
{
    for (int i = 0; i < s_fill_count; i++)
        if (s_fills[i] == argb)
            return true;
    return false;
}

static uint32_t div255(uint32_t v)
{
    return (v + 127U) / 255U;
}

static void fb_fill(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const uint32_t a = (argb >> 24) & 0xFFU;
    if (a == 0U || w <= 0 || h <= 0)
        return;
    s_op_count++;
    if (s_fill_count < (int)(sizeof(s_fills) / sizeof(s_fills[0])))
        s_fills[s_fill_count++] = argb;
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
            }
            else
            {
                const uint32_t inv = 255U - a;
                const uint32_t dr = (*d >> 16) & 0xFFU, dg = (*d >> 8) & 0xFFU, db = *d & 0xFFU;
                const uint32_t sr = (argb >> 16) & 0xFFU, sg = (argb >> 8) & 0xFFU, sb = argb & 0xFFU;
                *d = 0xFF000000U | ((div255(sr * a) + div255(dr * inv)) << 16)
                     | ((div255(sg * a) + div255(dg * inv)) << 8) | (div255(sb * a) + div255(db * inv));
            }
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static void fb_copy(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)stride;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
    s_op_count++;
}

static void fb_blend(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)stride;
    (void)alpha;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
    s_op_count++;
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

/** @brief Absolutely positioned box, so every test scene has exact, layout-independent geometry. */
static ERProps box(int x, int y, int w, int h, uint32_t argb)
{
    ERProps p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = (int16_t)x;
    p.top = (int16_t)y;
    p.width = (int16_t)w;
    p.height = (int16_t)h;
    p.background_color = argb;
    return p;
}

static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return EXIT_FAILURE;
}

/**
 * @brief Builds root → [deep → inner, cover, top].
 *
 * `deep` and `inner` sit under `cover`; `top` is painted after it. The caller supplies cover's
 * props so each test can vary exactly one thing about the would-be occluder.
 */
static void build(const ERProps* cover_props, ERNode** out_inner, ERNode** out_cover)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = box(0, 0, SCREEN, SCREEN, C_ROOT);
    rp.position = ER_POS_RELATIVE;
    er_node_set_props(root, &rp);

    ERNode* deep = er_node_create(ER_NODE_VIEW);
    ERProps dp = box(0, 0, SCREEN, SCREEN, C_DEEP);
    er_node_set_props(deep, &dp);

    ERNode* inner = er_node_create(ER_NODE_VIEW);
    ERProps ip = box(20, 20, 100, 100, C_INNER);
    er_node_set_props(inner, &ip);
    er_tree_append_child(deep, inner);

    ERNode* cover = er_node_create(ER_NODE_VIEW);
    er_node_set_props(cover, cover_props);

    ERNode* top = er_node_create(ER_NODE_VIEW);
    ERProps tp = box(150, 150, 40, 40, C_TOP);
    er_node_set_props(top, &tp);

    er_tree_append_child(root, deep);
    er_tree_append_child(root, cover);
    er_tree_append_child(root, top);
    er_tree_set_root(root);

    if (out_inner)
        *out_inner = inner;
    if (out_cover)
        *out_cover = cover;
}

/** @brief Renders one full frame with a clean fill log. */
static void full_frame(void)
{
    er_force_full_repaint();
    log_reset();
    er_commit();
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests
 ---------------------------------------------------------------------------------------------------------------------*/

/* The headline behaviour: an opaque full-cover child buries the root's background and its earlier
 * siblings, and none of them is drawn at all. The sibling painted AFTER it still is. */
static int check_opaque_cover_culls(void)
{
    ERProps cp = box(0, 0, SCREEN, SCREEN, C_COVER);
    build(&cp, NULL, NULL);
    full_frame();

#if ERUI_OCCLUSION_CULLING
    if (drew(C_ROOT))
        return fail("root background drew under a full opaque cover");
    if (drew(C_DEEP) || drew(C_INNER))
        return fail("buried subtree drew under a full opaque cover");
#endif
    if (!drew(C_COVER))
        return fail("the occluder itself did not draw");
    if (!drew(C_TOP))
        return fail("the sibling above the occluder was wrongly culled");

    /* And the result is what it should be: green everywhere except the top sibling's corner. */
    if ((s_fb[100 * SCREEN + 100] & 0x00FFFFFFU) != (C_COVER & 0x00FFFFFFU))
        return fail("centre pixel is not the occluder's colour");
    if ((s_fb[170 * SCREEN + 170] & 0x00FFFFFFU) != (C_TOP & 0x00FFFFFFU))
        return fail("top sibling did not land on screen");
    return EXIT_SUCCESS;
}

/* A translucent cover blends with what is beneath it, so nothing below may be skipped. */
static int check_translucent_cover_does_not_cull(void)
{
    ERProps cp = box(0, 0, SCREEN, SCREEN, 0x8000FF00U);
    build(&cp, NULL, NULL);
    full_frame();

    if (!drew(C_DEEP) || !drew(C_INNER))
        return fail("buried subtree skipped under a TRANSLUCENT cover");
    return EXIT_SUCCESS;
}

/* Rounded corners leave the corner squares showing whatever is underneath. */
static int check_rounded_cover_does_not_cull(void)
{
    ERProps cp = box(0, 0, SCREEN, SCREEN, C_COVER);
    cp.border_radius = 16;
    build(&cp, NULL, NULL);
    full_frame();

    if (!drew(C_DEEP) || !drew(C_INNER))
        return fail("buried subtree skipped under a ROUNDED cover");
    return EXIT_SUCCESS;
}

/* A cover smaller than the repaint region cannot bury it. */
static int check_partial_cover_does_not_cull(void)
{
    ERProps cp = box(0, 0, SCREEN, SCREEN - 1, C_COVER);
    build(&cp, NULL, NULL);
    full_frame();

    if (!drew(C_DEEP) || !drew(C_INNER))
        return fail("buried subtree skipped under a cover one row short of the region");
    return EXIT_SUCCESS;
}

/*
 * A buried node that changes must still retire its dirty flags. If it did not, the damage pre-pass
 * would read it as changed (or moved) on every following commit and repaint the same buried pixels
 * for as long as it stayed hidden — a permanent, invisible cost.
 */
static int check_buried_change_retires(void)
{
    ERNode* inner = NULL;
    ERProps cp = box(0, 0, SCREEN, SCREEN, C_COVER);
    build(&cp, &inner, NULL);
    full_frame();

    /* Recolour + move the buried leaf: source-dirty AND moved, the two damage sources. */
    ERProps ip = box(30, 35, 90, 80, 0xFF123456U);
    er_node_set_props(inner, &ip);
    log_reset();
    er_commit();
#if ERUI_OCCLUSION_CULLING
    if (drew(0xFF123456U))
        return fail("buried leaf drew its change");
#endif

    /* Nothing changed since: the commit must be a complete no-op, not a repeat of the same damage. */
    log_reset();
    er_commit();
    if (s_op_count != 0)
        return fail("a buried change re-damaged on the following idle commit");
    return EXIT_SUCCESS;
}

#if ERUI_TRANSFORMS_FULL
/*
 * A TRANSFORMED sibling must never be buried. Burying it skips the scratch capture that gives it a
 * transformed AABB, so its last_paint_rect is recorded as the raw box; the damage pre-pass measures it
 * as an AABB, the two never agree, and it reads as "moved" on every commit from then on.
 *
 * Nothing looks wrong on screen — a move-only damage sets no dirty flag, so the region is damaged and
 * repainted by nobody. What leaks is the REPORT: er_get_dirty_rect() names that region again on every
 * idle commit, so a partial-update host re-transfers it forever. The assertion is therefore that an
 * idle commit leaves the reported rect exactly where the last real change put it.
 */
static int check_transformed_sibling_not_buried(void)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = box(0, 0, SCREEN, SCREEN, C_ROOT);
    rp.position = ER_POS_RELATIVE;
    er_node_set_props(root, &rp);

    ERNode* spun = er_node_create(ER_NODE_VIEW);
    ERProps sp = box(40, 40, 80, 80, C_DEEP);
    sp.transform_rotate_z = 30.0f; /* not translate-only: measured by its transformed AABB */
    er_node_set_props(spun, &sp);

    ERNode* cover = er_node_create(ER_NODE_VIEW);
    ERProps cp = box(0, 0, SCREEN, SCREEN, C_COVER);
    er_node_set_props(cover, &cp);

    /* A small opaque marker on top, far from the rotated node, to give the reported rect a known
     * value that the rotated node's AABB (0,40 110x110) could never be mistaken for. */
    ERNode* marker = er_node_create(ER_NODE_VIEW);
    ERProps mp = box(160, 160, 20, 20, C_TOP);
    er_node_set_props(marker, &mp);

    er_tree_append_child(root, spun);
    er_tree_append_child(root, cover); /* painted after the rotated node: would bury it */
    er_tree_append_child(root, marker);
    er_tree_set_root(root);

    full_frame();
    er_commit();
    er_commit(); /* settle */

    /* One real change, so the reported rect is the marker and nothing else. */
    ERProps mp2 = box(160, 160, 20, 20, 0xFF00AAFFU);
    er_node_set_props(marker, &mp2);
    er_commit();

    ERRect after_change;
    if (!er_get_dirty_rect(&after_change))
        return fail("recolouring the marker reported no damage");
    if (after_change.x < 100)
        return fail("the marker's own change reported a region far from the marker");

    /* Nothing has changed since. The report must not move. */
    for (int i = 0; i < 3; i++)
    {
        er_commit();
        ERRect idle;
        if (!er_get_dirty_rect(&idle))
            return fail("the dirty rect vanished across an idle commit");
        if (idle.x != after_change.x || idle.y != after_change.y || idle.w != after_change.w
            || idle.h != after_change.h)
            return fail("a buried TRANSFORMED sibling re-damages its footprint on every idle commit");
    }
    return EXIT_SUCCESS;
}
#endif

/*
 * The catch-all. Drive an incremental, damage-clipped sequence over a scene whose occluder is only
 * SOMETIMES a valid one, then compare against the same scene rendered with a forced full repaint.
 * Any cull that skipped a layer it should not have shows up as a pixel difference.
 */
static int check_pixel_equivalence(void)
{
    static uint32_t reference[SCREEN * SCREEN];
    ERNode* inner = NULL;
    ERNode* cover = NULL;

    ERProps cp = box(0, 0, SCREEN, SCREEN, C_COVER);
    build(&cp, &inner, &cover);
    full_frame();

    /* A sequence of incremental commits: shrink the cover so the buried layers reappear, recolour
     * the buried leaf while it is exposed, then make the cover opaque and full-screen again. */
    ERProps step = box(0, 0, SCREEN, 120, C_COVER);
    er_node_set_props(cover, &step);
    er_commit();

    ERProps ip = box(20, 130, 100, 60, 0xFF884400U);
    er_node_set_props(inner, &ip);
    er_commit();

    step = box(0, 0, SCREEN, 120, 0xA000FF00U); /* translucent: the layers below must show through */
    er_node_set_props(cover, &step);
    er_commit();

    memcpy(reference, s_fb, sizeof(reference));

    /* Same scene, everything drawn. */
    memset(s_fb, 0, sizeof(s_fb));
    full_frame();

    if (memcmp(reference, s_fb, sizeof(reference)) != 0)
        return fail("incremental (culled) frame differs from a full repaint");
    return EXIT_SUCCESS;
}

int main(void)
{
    static const EmbeddedRenderBackend k_backend = {.fill_rect = fb_fill, .copy_rect = fb_copy, .blend_rect = fb_blend};
    embedded_renderer_set_backend(&k_backend);

    int (*const cases[])(void) = {
        check_opaque_cover_culls,
        check_translucent_cover_does_not_cull,
        check_rounded_cover_does_not_cull,
        check_partial_cover_does_not_cull,
        check_buried_change_retires,
#if ERUI_TRANSFORMS_FULL
        check_transformed_sibling_not_buried,
#endif
        check_pixel_equivalence,
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        memset(s_fb, 0, sizeof(s_fb));
        log_reset();
        const int rc = cases[i]();
        if (rc != EXIT_SUCCESS)
            return rc;
        er_reset();
    }
    return EXIT_SUCCESS;
}
