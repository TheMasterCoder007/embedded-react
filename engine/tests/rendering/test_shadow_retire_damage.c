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
 * Regression (issue #140): a shadow must not outlive the paint that put it there.
 *
 * last_paint_rect used to record a node's BARE layout box, so every reader that needed the node's real
 * on-screen footprint reconstructed the bleed by calling expand_for_shadow() on the node's CURRENT
 * props. Two ways for that to be wrong, one root cause:
 *
 *   A. Clearing the shadow. expand_for_shadow() answers zero once shadow_opacity is 0, so the very
 *      commit that removes a shadow reconstructed a bleed of nothing and never damaged the ring that
 *      was actually on the panel. The worst case needed no move at all.
 *
 *   B. Retiring the node. note_removed_subtree(), propagate_hidden() and er_node_destroy() vacate
 *      last_paint_rect with no expansion whatever, so unmounting or hiding a shadowed node erased its
 *      bare box (plus add_damage's 2 px margin) and left the whole ring behind.
 *
 * The fix records what was PAINTED: render_tree inflates last_paint_rect by the bleed as it writes it,
 * gated on !doing_affine because a captured transform casts no shadow. A recorded footprint cannot
 * disagree with the paint that produced it, so all three retire paths and the shadow-removal commit
 * erase exactly what was drawn — and the pre-pass `moved` test then measures the same inflated
 * footprint on both sides.
 *
 * Asserted the way that cannot be argued with: the damage-clipped frame must be byte-identical to a
 * forced full repaint of the same scene state.
 *
 * CONFIGURATION. The plain scenarios need only ERUI_SHADOWS. The transform-fallback ones additionally
 * need a node that fails its scratch capture while still casting a shadow, which exists only when
 * ERUI_XFORM_W/H is decoupled below ERUI_SCRATCH_W/H — the same gap test_transform_shadow_damage.c
 * needs, and the same CI pass ("shadows, decoupled transform scratch") that provides it.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FB_W 400
#define FB_H 400

/* Everything below is dead without the shadow rasteriser — main() falls through to a SKIP. */
#if ERUI_SHADOWS

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
 * This is the callback the shadow itself arrives through, so its fidelity is what makes a stale shadow
 * ring show up as a pixel difference.
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

/* Wide enough that a stale ring is thousands of pixels, not a rounding artefact. */
#define SHADOW_RADIUS 10
#define SHADOW_OFFSET 6

/* Plain (untransformed) card: comfortably inside ERUI_SCRATCH_W/H, so it really does cast a shadow. */
#define PLAIN_SIDE 200
#define CARD_LEFT 60
#define CARD_TOP 60
/* Far enough that the moved-and-unshadowed cases separate the two footprints completely. */
#define MOVE_DY 40

/* The transform-fallback band exists only when the transform source is smaller than the shadow's: a
 * node above ERUI_XFORM_W/H refuses the capture and paints its raw box, shadow and all. With the
 * default (equal) limits such a node is over the shadow limit too and casts nothing. */
#if ERUI_TRANSFORMS_FULL && (ERUI_XFORM_W < ERUI_SCRATCH_W) && (ERUI_XFORM_H < ERUI_SCRATCH_H)
#define XFORM_SHADOW_GAP 1
#define FALLBACK_SIDE (ERUI_XFORM_W + 1)
#define CAPTURED_SIDE (ERUI_XFORM_W / 2)
#endif

/** @brief How the card under test reaches the screen — which damage path measures it. */
typedef enum
{
    CARD_PLAIN,    /**< No transform: the node_screen_rect() branch of the pre-pass. */
    CARD_FALLBACK, /**< Scaled but too big to capture: painted untransformed, shadow and all. */
    CARD_CAPTURED, /**< Scaled and small enough to capture: casts no shadow (control). */
} CardKind;

/** @brief What the commit under test does to the card. */
typedef enum
{
    ACT_DROP_SHADOW,       /**< shadow_opacity -> 0 and nothing else: symptom A's worst case. */
    ACT_DROP_SHADOW_MOVED, /**< ...and move in the same commit, so neither footprint covers the other. */
    ACT_MOVE,              /**< Move with the shadow kept — the case #137 already fixed (control). */
    ACT_REMOVE,            /**< Unmount: er_tree_remove_child() then er_node_destroy(). */
    ACT_HIDE,              /**< display:none — the propagate_hidden() retire path. */
} Act;

static uint32_t g_fb[FB_H * FB_W];
static uint32_t g_snap[FB_H * FB_W];
static uint32_t g_before[FB_H * FB_W];

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
        printf("  %-46s %d px stale, %d,%d..%d,%d\n", label, bad, x0, y0, x1, y1);
    else
        printf("  %-46s identical\n", label);
    return bad;
}

/**
 * @brief Checks the REPORTED damage covers every pixel the commit under test actually changed.
 *
 * The framebuffer comparison below grades what the engine painted; this grades what it told the host
 * it painted. They are separate channels, and a partial-update display driver — an SPI panel that
 * issues one transfer window per rect from er_get_dirty_rects() — only ever sees this one. A fix that
 * repaints the shadow ring correctly but forgets to report it would leave the ring on a real panel
 * while every framebuffer test still passed, so the documented guarantee ("together cover every pixel
 * modified by that commit") is asserted here rather than left to hardware.
 *
 * @param[in] label  Scenario name, printed on failure.
 *
 * @return Number of changed pixels lying outside every reported rect (0 = the report was honest).
 */
static int check_reported_damage(const char* label)
{
    ERRect rects[ER_DAMAGE_RECTS_MAX];
    const int n = er_get_dirty_rects(rects, (int)(sizeof rects / sizeof rects[0]));

    int uncovered = 0, x0 = FB_W, y0 = FB_H, x1 = -1, y1 = -1;
    for (int r = 0; r < FB_H; r++)
    {
        for (int c = 0; c < FB_W; c++)
        {
            if (g_before[r * FB_W + c] == g_snap[r * FB_W + c])
                continue; /* unchanged: the host owes it nothing */
            bool covered = false;
            for (int i = 0; i < n && !covered; i++)
                covered =
                    (c >= rects[i].x && c < rects[i].x + rects[i].w && r >= rects[i].y && r < rects[i].y + rects[i].h);
            if (covered)
                continue;
            uncovered++;
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
    if (uncovered)
        printf("  %-46s %d px changed but UNREPORTED, %d,%d..%d,%d (%d rects)\n", label, uncovered, x0, y0, x1, y1, n);
    return uncovered;
}

/**
 * @brief Builds a one-card scene, applies @p act to the card, and grades the damage-clipped frame.
 *
 * The card is absolutely positioned so a move is a pure position change (no reflow of anything else),
 * and the root is opaque white so any pixel the incremental frame fails to erase is a difference the
 * comparison sees.
 *
 * @param[in] kind      Which paint path the card takes.
 * @param[in] shadowed  Give the card a shadow (false = the no-shadow control).
 * @param[in] act       The change the commit under test makes.
 * @param[in] label     Scenario name for the report.
 *
 * @return Number of differing pixels.
 */
static int run_case(CardKind kind, bool shadowed, Act act, const char* label)
{
    er_reset();
    memset(g_fb, 0, sizeof g_fb);

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.position = ER_POS_RELATIVE;
    rp.width = FB_W;
    rp.height = FB_H;
    rp.background_color = 0xFFFFFFFFU; /* opaque, so a missed erase really shows */
    er_node_set_props(root, &rp);

    int side = PLAIN_SIDE;
#ifdef XFORM_SHADOW_GAP
    if (kind == CARD_FALLBACK)
        side = FALLBACK_SIDE;
    else if (kind == CARD_CAPTURED)
        side = CAPTURED_SIDE;
#endif

    ERNode* card = er_node_create(ER_NODE_VIEW);
    ERProps cp = props_default();
    cp.position = ER_POS_ABSOLUTE;
    cp.left = CARD_LEFT;
    cp.top = CARD_TOP;
    cp.width = (int16_t)side;
    cp.height = (int16_t)side;
    cp.background_color = 0xFF3366FFU;
    if (kind != CARD_PLAIN)
    {
        cp.transform_scale_x = 0.9f; /* non-translate → the transformed damage branch */
        cp.transform_scale_y = 0.9f;
    }
    if (shadowed)
    {
        cp.shadow_opacity = 0.8f;
        cp.shadow_radius = SHADOW_RADIUS;
        cp.shadow_offset_x = SHADOW_OFFSET;
        cp.shadow_offset_y = SHADOW_OFFSET;
    }
    er_node_set_props(card, &cp);

    er_tree_append_child(root, card);
    er_tree_set_root(root);

    er_force_full_repaint();
    er_commit();
    er_commit();                         /* settle: the card has a last-painted footprint now */
    memcpy(g_before, g_fb, sizeof g_fb); /* what is on the panel going into the commit under test */

    switch (act)
    {
        case ACT_DROP_SHADOW:
            cp.shadow_opacity = 0.0f;
            er_node_set_props(card, &cp);
            break;
        case ACT_DROP_SHADOW_MOVED:
            cp.shadow_opacity = 0.0f;
            cp.top = (int16_t)(CARD_TOP - MOVE_DY);
            er_node_set_props(card, &cp);
            break;
        case ACT_MOVE:
            cp.top = (int16_t)(CARD_TOP - MOVE_DY);
            er_node_set_props(card, &cp);
            break;
        case ACT_REMOVE:
            er_tree_remove_child(root, card);
            er_node_destroy(card);
            card = NULL;
            break;
        case ACT_HIDE:
            cp.display = ER_DISPLAY_NONE;
            er_node_set_props(card, &cp);
            break;
    }

    er_commit(); /* the frame under test: damage-clipped */
    memcpy(g_snap, g_fb, sizeof g_fb);
    /* Read the report before the reference repaint below overwrites it. */
    const int unreported = check_reported_damage(label);

    er_force_full_repaint();
    er_commit(); /* the reference */

    const int bad = compare_to_full_repaint(label);
    er_node_destroy(root);
    return bad + unreported;
}
#endif /* ERUI_SHADOWS */

/*----------------------------------------------------------------------------------------------------------------------
 - Test
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
#if ERUI_SHADOWS
    TestCtx ctx = {g_fb, FB_W, FB_H};
    EmbeddedRenderBackend be = {0};
    be.fill_rect = fill_cb;
    be.copy_rect = copy_cb;
    be.blend_rect = blend_cb;
    be.ctx = &ctx;
    embedded_renderer_set_backend(&be);

    int bad = 0;

    printf("plain card %dx%d at %d,%d, shadow radius %d offset %d (bleed %d px)\n",
           PLAIN_SIDE,
           PLAIN_SIDE,
           CARD_LEFT,
           CARD_TOP,
           SHADOW_RADIUS,
           SHADOW_OFFSET,
           SHADOW_RADIUS + SHADOW_OFFSET);

    /* --- A. removing the shadow must erase the ring that is on the panel --- */
    bad += run_case(CARD_PLAIN, true, ACT_DROP_SHADOW, "plain: shadow dropped, no move");
    bad += run_case(CARD_PLAIN, true, ACT_DROP_SHADOW_MOVED, "plain: shadow dropped + moved");
    bad += run_case(CARD_PLAIN, true, ACT_MOVE, "plain: moved, shadow kept (control)");

    /* --- B. unmounting or hiding a shadowed node must erase the ring too --- */
    bad += run_case(CARD_PLAIN, true, ACT_REMOVE, "plain: removed + destroyed");
    bad += run_case(CARD_PLAIN, true, ACT_HIDE, "plain: display:none");
    bad += run_case(CARD_PLAIN, false, ACT_REMOVE, "plain: removed, no shadow (control)");
    bad += run_case(CARD_PLAIN, false, ACT_HIDE, "plain: display:none, no shadow (control)");

#ifdef XFORM_SHADOW_GAP
    printf("transform limit %dx%d, shadow limit %dx%d → fallback side %d, captured side %d\n",
           ERUI_XFORM_W,
           ERUI_XFORM_H,
           ERUI_SCRATCH_W,
           ERUI_SCRATCH_H,
           FALLBACK_SIDE,
           CAPTURED_SIDE);

    /* The same two symptoms on the transform-fallback path, whose reach #137 extended. */
    bad += run_case(CARD_FALLBACK, true, ACT_DROP_SHADOW, "fallback: shadow dropped, no move");
    bad += run_case(CARD_FALLBACK, true, ACT_DROP_SHADOW_MOVED, "fallback: shadow dropped + moved");
    bad += run_case(CARD_FALLBACK, true, ACT_MOVE, "fallback: moved, shadow kept (control)");
    bad += run_case(CARD_FALLBACK, true, ACT_REMOVE, "fallback: removed + destroyed");
    bad += run_case(CARD_FALLBACK, true, ACT_HIDE, "fallback: display:none");

    /* A captured node paints no shadow at all, so it must be exact with the props set either way. */
    bad += run_case(CARD_CAPTURED, true, ACT_REMOVE, "captured: removed (casts no shadow, control)");
    bad += run_case(CARD_CAPTURED, true, ACT_HIDE, "captured: display:none (control)");
#else
    printf("(transform-fallback scenarios need ERUI_XFORM_W/H < ERUI_SCRATCH_W/H — skipped)\n");
#endif

    if (bad)
        return fail("a damage-clipped frame disagreed with a full repaint — a shadow outlived its node");

    printf("PASS: a shadow is erased with the paint that drew it\n");
#else
    printf("SKIP: needs ERUI_SHADOWS\n");
#endif
    return EXIT_SUCCESS;
}
