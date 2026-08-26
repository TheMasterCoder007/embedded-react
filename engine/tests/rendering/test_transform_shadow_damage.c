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
 * Regression: a transformed node that FALLS BACK to an untransformed paint must damage its shadow too.
 *
 * render_tree renders a scale/rotate node by capturing its subtree into the transform scratch and
 * inverse-mapping it out, and it deliberately skips the shadow on that path — the shadow would be
 * rasterised into the source and distorted by the blit. When the capture cannot be started, though, it
 * degrades to painting the node untransformed at its raw box, and on THAT path the shadow is rendered
 * like any other node's.
 *
 * The damage pre-pass has two branches. The plain node_screen_rect() one grows both the new footprint
 * and the vacated one by the shadow bleed (see expand_for_shadow, added for the move-leaves-its-shadow
 * fix in 97d536a). The transformed branch never did — correctly for a captured node, which casts no
 * shadow, but wrongly for a fallback one. A move therefore left the old shadow on screen and clipped
 * the new one at the damage edge.
 *
 * Asserted the way that cannot be argued with: the damage-clipped frame must be byte-identical to a
 * forced full repaint of the same scene state.
 *
 * CONFIGURATION. Both halves have to be reachable at once — the capture must fail while the shadow
 * still renders — and the two have separate size limits: er_transform_source_begin() refuses a node
 * larger than ERUI_XFORM_W/H, er_shadow_render() bails on one larger than ERUI_SCRATCH_W/H. With the
 * default ERUI_XFORM_W = ERUI_SCRATCH_W any node over the transform limit is over the shadow limit too
 * and simply casts no shadow, so the defect needs the decoupled configuration these knobs exist for
 * ("decouple from ERUI_SCRATCH_W when strips are screen-wide"). The scenarios below compile out unless
 * the build actually provides that gap; .github/workflows/ci.yml has a pass that does.
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

#define FB_W 400
#define FB_H 400

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Framebuffer the backend stubs rasterise into. */
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
 * @brief Backend copy_rect: source-over composites a premultiplied buffer into the framebuffer.
 *
 * Not a replacing blit — only the separate copy_opaque callback carries that guarantee. Overwriting
 * unconditionally would erase the background under the shadow's own antialiased edge and make the
 * comparison depend on repaint history rather than scene state, which is the whole thing under test.
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
            if (dx < 0 || dx >= t->fb_w || dy < 0 || dy >= t->fb_h)
                continue;
            const uint32_t sp = s[col];
            const uint32_t sa = (sp >> 24) & 0xFFU;
            if (sa == 0U)
                continue;
            uint32_t* d = &t->fb[dy * t->fb_w + dx];
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

/**
 * @brief Backend blend_rect: source-over composites premultiplied pixels scaled by a global alpha.
 *
 * This is the callback the shadow itself arrives through, so its fidelity is what makes a missing
 * shadow band show up as a pixel difference.
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

/** @brief ERProps with every layout field at AUTO — the documented starting point. */
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
    p.shadow_color = 0xFF000000U;
    return p;
}

#if ERUI_SHADOWS && ERUI_TRANSFORMS_FULL && (ERUI_XFORM_W < ERUI_SCRATCH_W) && (ERUI_XFORM_H < ERUI_SCRATCH_H)
#define XFORM_SHADOW_GAP 1

/* A node in this range fails the transform capture but still gets a shadow — the only size band where
 * the two halves of the defect coexist. */
#define FALLBACK_SIDE (ERUI_XFORM_W + 1)
#define CAPTURED_SIDE (ERUI_XFORM_W / 2)

/** @brief Gives a node a shadow big enough that a clipped band is unmistakable. */
static void set_shadow(ERProps* p)
{
    p->shadow_opacity = 0.8f;
    p->shadow_radius = 10;
    p->shadow_offset_x = 6;
    p->shadow_offset_y = 6;
}

static uint32_t g_fb[FB_H * FB_W];
static uint32_t g_snap[FB_H * FB_W];

/**
 * @brief Compares the saved damage-clipped frame against the framebuffer's current (full-repaint) state.
 *
 * @param[in] label  Scenario name, printed either way.
 *
 * @return Number of differing pixels (0 = the incremental frame was correct).
 */
static int compare_to_full_repaint(const char* label)
{
    int bad = 0, x0 = FB_W, y0 = FB_H, x1 = -1, y1 = -1;
    for (int r = 0; r < FB_H; r++)
    {
        for (int c = 0; c < FB_W; c++)
        {
            if (g_snap[r * FB_W + c] == g_fb[r * FB_W + c])
                continue;
            bad++;
            if (c < x0)
                x0 = c;
            if (r < y0)
                y0 = r;
            if (c > x1)
                x1 = c;
            if (r > y1)
                y1 = r;
        }
    }
    if (bad)
        printf("  %-44s %d px differ, region %d,%d..%d,%d\n", label, bad, x0, y0, x1, y1);
    else
        printf("  %-44s identical\n", label);
    return bad;
}

/**
 * @brief A sibling reflow shifts a shadow-casting transformed node; the incremental frame must be exact.
 *
 * The card is scaled (so the pre-pass takes the transformed branch) and carries a shadow. Growing or
 * shrinking the bar above it moves the card WITHOUT making it source_dirty, which is precisely the case
 * that has to reason about the vacated footprint — the old shadow — on its own.
 *
 * @param[in] side    Card size; > ERUI_XFORM_W makes its scratch capture fail.
 * @param[in] bar_h0  Bar height before the reflow.
 * @param[in] bar_h1  Bar height after it.
 * @param[in] label   Scenario name for the report.
 *
 * @return Number of differing pixels.
 */
static int reflow_case(int side, int bar_h0, int bar_h1, const char* label)
{
    er_reset();
    memset(g_fb, 0, sizeof g_fb);

    ERNode* root = er_node_create(ER_NODE_VIEW); /* default flex column */
    ERProps rp = props_default();
    rp.width = FB_W;
    rp.height = FB_H;
    rp.background_color = 0xFFFFFFFFU; /* opaque, so a missed erase really shows */
    er_node_set_props(root, &rp);

    ERNode* bar = er_node_create(ER_NODE_VIEW);
    ERProps ap = props_default();
    ap.width = FB_W;
    ap.height = (int16_t)bar_h0;
    ap.background_color = 0xFF333333U;
    er_node_set_props(bar, &ap);

    ERNode* card = er_node_create(ER_NODE_VIEW);
    ERProps cp = props_default();
    cp.width = (int16_t)side;
    cp.height = (int16_t)side;
    cp.margin_left = 60;
    cp.background_color = 0xFF3366FFU;
    cp.transform_scale_x = 0.9f; /* non-translate → the transformed damage branch */
    cp.transform_scale_y = 0.9f;
    set_shadow(&cp);
    er_node_set_props(card, &cp);

    er_tree_append_child(root, bar);
    er_tree_append_child(root, card);
    er_tree_set_root(root);

    er_force_full_repaint();
    er_commit();
    er_commit(); /* settle: the card has a last-painted footprint now */

    ap.height = (int16_t)bar_h1;
    er_node_set_props(bar, &ap);
    er_commit(); /* the frame under test: damage-clipped */
    memcpy(g_snap, g_fb, sizeof g_fb);

    er_force_full_repaint();
    er_commit(); /* the reference */

    const int bad = compare_to_full_repaint(label);
    er_node_destroy(root);
    return bad;
}

/**
 * @brief The card resizes itself — source_dirty AND moved edges, both footprints shadowed.
 *
 * @param[in] from   Card size before.
 * @param[in] to     Card size after.
 * @param[in] label  Scenario name for the report.
 *
 * @return Number of differing pixels.
 */
static int resize_case(int from, int to, const char* label)
{
    er_reset();
    memset(g_fb, 0, sizeof g_fb);

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.position = ER_POS_RELATIVE;
    rp.width = FB_W;
    rp.height = FB_H;
    rp.background_color = 0xFFFFFFFFU;
    er_node_set_props(root, &rp);

    ERNode* card = er_node_create(ER_NODE_VIEW);
    ERProps cp = props_default();
    cp.position = ER_POS_ABSOLUTE;
    cp.left = 60;
    cp.top = 60;
    cp.width = (int16_t)from;
    cp.height = (int16_t)from;
    cp.background_color = 0xFF3366FFU;
    cp.transform_scale_x = 0.9f;
    cp.transform_scale_y = 0.9f;
    set_shadow(&cp);
    er_node_set_props(card, &cp);

    er_tree_append_child(root, card);
    er_tree_set_root(root);

    er_force_full_repaint();
    er_commit();
    er_commit();

    cp.width = (int16_t)to;
    cp.height = (int16_t)to;
    er_node_set_props(card, &cp);
    er_commit();
    memcpy(g_snap, g_fb, sizeof g_fb);

    er_force_full_repaint();
    er_commit();

    const int bad = compare_to_full_repaint(label);
    er_node_destroy(root);
    return bad;
}
#endif /* the transform/shadow size gap */

/*----------------------------------------------------------------------------------------------------------------------
 - Test
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
#ifdef XFORM_SHADOW_GAP
    TestCtx ctx = {g_fb, FB_W, FB_H};
    EmbeddedRenderBackend be = {0};
    be.fill_rect = fill_cb;
    be.copy_rect = copy_cb;
    be.blend_rect = blend_cb;
    be.ctx = &ctx;
    embedded_renderer_set_backend(&be);

    printf("transform limit %dx%d, shadow limit %dx%d → fallback side %d, captured side %d\n",
           ERUI_XFORM_W,
           ERUI_XFORM_H,
           ERUI_SCRATCH_W,
           ERUI_SCRATCH_H,
           FALLBACK_SIDE,
           CAPTURED_SIDE);

    int bad = 0;
    /* Control: small enough to capture, so it casts no shadow at all and must stay exact either way. */
    bad += reflow_case(CAPTURED_SIDE, 20, 40, "captured node, moved down (control)");
    /* The defect, in each direction the two footprints can be wrong. */
    bad += reflow_case(FALLBACK_SIDE, 20, 40, "fallback node, moved down");
    bad += reflow_case(FALLBACK_SIDE, 60, 20, "fallback node, moved up");
    bad += reflow_case(FALLBACK_SIDE, 20, 21, "fallback node, moved 1px (trail overlaps)");
    bad += resize_case(FALLBACK_SIDE, FALLBACK_SIDE - 20, "fallback node, shrank");
    bad += resize_case(FALLBACK_SIDE - 20, FALLBACK_SIDE, "fallback node, grew");

    if (bad)
        return fail("a damage-clipped frame disagreed with a full repaint — shadow bleed not damaged");

    printf("PASS: a fallback-painted transformed node damages its shadow\n");
#else
    printf("SKIP: needs ERUI_SHADOWS, ERUI_TRANSFORMS=FULL, and ERUI_XFORM_W/H < ERUI_SCRATCH_W/H\n");
#endif
    return EXIT_SUCCESS;
}
