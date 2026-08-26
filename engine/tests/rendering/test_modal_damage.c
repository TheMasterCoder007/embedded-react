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
 * Modal + scrim damage: a dialog over a static page must repaint only what changed.
 *
 * This matters well beyond frame rate on a scanned-out panel. A full-screen ARGB repaint competes
 * with the display controller's scan-out for memory bandwidth, and a repaint that lands mid-scan
 * tears. So the properties below are asserted as ratios of the screen, not as absolute pixel counts:
 *
 *   - an idle frame under a visible modal paints NOTHING,
 *   - changing a control inside the dialog stays local to that control,
 *   - changing a page node BEHIND the scrim stays local too,
 *   - the translucent scrim does not darken cumulatively as those repaints land on it (the stack
 *     under it has to be rebuilt each time, so a missing layer shows up as drift),
 *   - and, deliberately pinned: dirtying the modal NODE ITSELF is full-screen, because an idiomatic
 *     modal overlay covers the screen. That is the one shape to avoid per frame, so it is recorded
 *     here rather than left for someone to rediscover.
 *
 * The framebuffer is a half-scale stand-in for an 800x1280 panel; every assertion is proportional.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "transform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SW 400
#define SH 640
#define SCREEN_PX ((long)SW * (long)SH)

/*----------------------------------------------------------------------------------------------------------------------
 - Backend: composites for real, and records which pixels were written
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_fb[SW * SH];
static uint8_t s_touched[SW * SH];
static long s_painted; /* every write, so overdraw inside the damage region is visible too */

static uint32_t div255(uint32_t v)
{
    return (v + 127u) / 255u;
}

static void put(int x, int y, uint32_t sa, uint32_t sr, uint32_t sg, uint32_t sb)
{
    if (x < 0 || x >= SW || y < 0 || y >= SH)
        return;
    uint32_t* d = &s_fb[y * SW + x];
    const uint32_t inv = 255u - sa;
    const uint32_t dr = (*d >> 16) & 0xFFu, dg = (*d >> 8) & 0xFFu, db = *d & 0xFFu;
    *d = 0xFF000000u | ((sr + div255(dr * inv)) << 16) | ((sg + div255(dg * inv)) << 8) | (sb + div255(db * inv));
    s_touched[y * SW + x] = 1u;
    s_painted++;
}

static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const uint32_t a = (argb >> 24) & 0xFFu;
    if (!a)
        return;
    const uint32_t sr = div255(((argb >> 16) & 0xFFu) * a);
    const uint32_t sg = div255(((argb >> 8) & 0xFFu) * a);
    const uint32_t sb = div255((argb & 0xFFu) * a);
    for (int r = y; r < y + h; r++)
        for (int q = x; q < x + w; q++)
            put(q, r, a, sr, sg, sb);
}

static void copy_cb(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const uint8_t* p = src;
    for (int r = 0; r < h; r++, p += stride)
    {
        const uint32_t* row = (const uint32_t*)p;
        for (int q = 0; q < w; q++)
            put(x + q, y + r, 255u, (row[q] >> 16) & 0xFFu, (row[q] >> 8) & 0xFFu, row[q] & 0xFFu);
    }
}

static void blend_cb(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const uint8_t* p = src;
    for (int r = 0; r < h; r++, p += stride)
    {
        const uint32_t* row = (const uint32_t*)p;
        for (int q = 0; q < w; q++)
        {
            const uint32_t v = row[q];
            uint32_t sa = (v >> 24) & 0xFFu, sr = (v >> 16) & 0xFFu, sg = (v >> 8) & 0xFFu, sb = v & 0xFFu;
            if (alpha < 255)
            {
                sa = div255(sa * alpha);
                sr = div255(sr * alpha);
                sg = div255(sg * alpha);
                sb = div255(sb * alpha);
            }
            put(x + q, y + r, sa, sr, sg, sb);
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static ERProps props_default(void)
{
    ERProps p;
    er_props_default(&p);
    return p;
}

static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return EXIT_FAILURE;
}

/** @brief Commits one frame and returns the number of DISTINCT pixels written. */
static long frame_distinct(void)
{
    s_painted = 0;
    memset(s_touched, 0, sizeof(s_touched));
    er_commit();
    er_display_present();
    long n = 0;
    for (long i = 0; i < SCREEN_PX; i++)
        if (s_touched[i])
            n++;
    return n;
}

static uint32_t at(int x, int y)
{
    return s_fb[y * SW + x] & 0xFFFFFFu;
}

/**
 * @brief Asserts er_get_dirty_rect() spans the whole root.
 *
 * A scrim covers the screen, so the commits that raise and drop one have to REPORT the screen, not
 * just the modal's box. The framebuffer being right is not enough: a partial-update host flushes only
 * the reported rect, so anything outside it stays on the panel — scrim pixels that never wash off.
 * This is the single-buffer path; the multi-buffer case rewrites the rect wholesale after render.
 *
 * @param[in] when  Label for the failure message.
 *
 * @return 0 when the rect spans the root, 1 otherwise.
 */
static int dirty_rect_spans_root(const char* when)
{
    ERRect d;
    if (!er_get_dirty_rect(&d))
    {
        fprintf(stderr, "  %s: er_get_dirty_rect() reported nothing\n", when);
        return 1;
    }
    if (d.x > 0 || d.y > 0 || d.x + d.w < SW || d.y + d.h < SH)
    {
        fprintf(
            stderr, "  %s: dirty rect (%d,%d %dx%d) does not span the %dx%d root\n", when, d.x, d.y, d.w, d.h, SW, SH);
        return 1;
    }
    return 0;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Scene
 ---------------------------------------------------------------------------------------------------------------------*/

/* Geometry, in the same proportions an 800x1280 dialog would use. */
#define SHEET_X 50
#define SHEET_Y 240
#define SHEET_W 300
#define SHEET_H 160
#define CTRL_W 100
#define CTRL_H 30
#define PAGE_NODE_W 90
#define PAGE_NODE_H 25

static ERNode* s_root;
static ERNode* s_modal;
static ERNode* s_ctrl;      /* inside the dialog */
static ERNode* s_page_node; /* behind the scrim */
static ERProps s_modal_p, s_ctrl_p, s_page_p;

static void build_scene(void)
{
    s_root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = SW;
    rp.height = SH;
    rp.background_color = 0xFF203040U;
    er_node_set_props(s_root, &rp);
    er_tree_set_root(s_root);

    for (int i = 0; i < 4; i++)
    {
        ERNode* card = er_node_create(ER_NODE_VIEW);
        ERProps cp = props_default();
        cp.position = ER_POS_ABSOLUTE;
        cp.left = 20;
        cp.top = (int16_t)(20 + i * 150);
        cp.width = 360;
        cp.height = 120;
        cp.border_radius = 12;
        cp.background_color = 0xFF3A4A5AU;
        er_node_set_props(card, &cp);
        er_tree_append_child(s_root, card);
    }

    s_page_node = er_node_create(ER_NODE_VIEW);
    s_page_p = props_default();
    s_page_p.position = ER_POS_ABSOLUTE;
    s_page_p.left = 30;
    s_page_p.top = 30;
    s_page_p.width = PAGE_NODE_W;
    s_page_p.height = PAGE_NODE_H;
    s_page_p.background_color = 0xFF88CCFFU;
    er_node_set_props(s_page_node, &s_page_p);
    er_tree_append_child(s_root, s_page_node);

    /* The idiomatic overlay: the Modal node covers the screen, the dialog is a child of it. The
     * scrim is painted over the root rect, so a modal that does NOT cover the screen would have its
     * scrim scissored to its own box — hence the full-screen style every demo gives it. */
    s_modal = er_node_create(ER_NODE_MODAL);
    s_modal_p = props_default();
    s_modal_p.position = ER_POS_ABSOLUTE;
    s_modal_p.left = 0;
    s_modal_p.top = 0;
    s_modal_p.width = SW;
    s_modal_p.height = SH;
    s_modal_p.modal_visible = 1;
    s_modal_p.backdrop_color = 0x99000000U;
    er_node_set_props(s_modal, &s_modal_p);
    er_tree_append_child(s_root, s_modal);

    ERNode* sheet = er_node_create(ER_NODE_VIEW);
    ERProps sp = props_default();
    sp.position = ER_POS_ABSOLUTE;
    sp.left = SHEET_X;
    sp.top = SHEET_Y;
    sp.width = SHEET_W;
    sp.height = SHEET_H;
    sp.background_color = 0xFF101820U;
    sp.border_radius = 16;
    er_node_set_props(sheet, &sp);
    er_tree_append_child(s_modal, sheet);

    s_ctrl = er_node_create(ER_NODE_VIEW);
    s_ctrl_p = props_default();
    s_ctrl_p.position = ER_POS_ABSOLUTE;
    s_ctrl_p.left = 20;
    s_ctrl_p.top = 20;
    s_ctrl_p.width = CTRL_W;
    s_ctrl_p.height = CTRL_H;
    s_ctrl_p.background_color = 0xFF66AAFFU;
    er_node_set_props(s_ctrl, &s_ctrl_p);
    er_tree_append_child(sheet, s_ctrl);
}

/**
 * @brief Shows then hides the shared s_modal, checking the root-wide scrim contract at (px_x, px_y).
 *
 * Common to every modal shape this file exercises against a probe point outside the modal's own box:
 * raising the modal scrims that point and reports a full-root dirty rect; an optional callback runs
 * once while it's shown (e.g. to repaint something else under the scrim); hiding it reports a
 * full-root dirty rect too, restores the probe point to `expect_after_hide`, leaves no residue in the
 * reference strip, and costs nothing once it settles idle again.
 *
 * @param[in] desc              Label for failure messages ("non-full-screen", "transformed").
 * @param[in] px_x,px_y         Probe point outside the modal's own box.
 * @param[in] expect_after_hide Pixel value (px_x, px_y) must settle back to once hidden.
 * @param[in] while_shown       Optional: runs once after show, before hide.
 *
 * @return 0 on success, or a fail()-produced EXIT_FAILURE.
 */
static int
check_modal_scrim_roundtrip(const char* desc, int px_x, int px_y, uint32_t expect_after_hide, void (*while_shown)(void))
{
    const uint32_t before_show = at(px_x, px_y);

    s_modal_p.modal_visible = 1;
    er_node_set_props(s_modal, &s_modal_p);
    frame_distinct();
    if (at(px_x, px_y) == before_show)
    {
        fprintf(stderr, "  (%d,%d) still %06X with the %s modal shown\n", px_x, px_y, before_show, desc);
        return fail("a modal did not scrim the page outside its own box");
    }
    char label[64];
    snprintf(label, sizeof(label), "%s show", desc);
    if (dirty_rect_spans_root(label))
        return fail("raising a scrim did not report a full-root dirty rect");

    if (while_shown)
        while_shown();

    s_modal_p.modal_visible = 0;
    er_node_set_props(s_modal, &s_modal_p);
    frame_distinct();
    snprintf(label, sizeof(label), "%s hide", desc);
    if (dirty_rect_spans_root(label))
        return fail("dropping a scrim did not report a full-root dirty rect");
    frame_distinct();

    if (at(px_x, px_y) != expect_after_hide)
    {
        fprintf(stderr, "  (%d,%d) is %06X, expected %06X\n", px_x, px_y, at(px_x, px_y), expect_after_hide);
        return fail("hiding a modal left scrim on the page");
    }
    long stale = 0;
    for (int y = 0; y < 20; y++)
        for (int x = 200; x < 260; x++) /* page background, well clear of every card and the dialog */
            if (at(x, y) != 0x203040u)
                stale++;
    if (stale != 0)
    {
        fprintf(stderr, "  %ld px still scrimmed\n", stale);
        return fail("hiding a modal left scrim residue on the page");
    }

    /* An on-screen modal must still cost nothing while idle. */
    s_modal_p.modal_visible = 1;
    er_node_set_props(s_modal, &s_modal_p);
    frame_distinct();
    frame_distinct();
    for (int i = 0; i < 3; i++)
        if (frame_distinct() != 0)
            return fail("a modal repainted on an idle frame");
    return 0;
}

/** @brief check_modal_scrim_roundtrip() while_shown callback: repaints s_page_node under the modal. */
static void repaint_page_node_under_modal(void)
{
    s_page_p.background_color = 0xFF88CCF0U;
    er_node_set_props(s_page_node, &s_page_p);
    frame_distinct();
}

/*----------------------------------------------------------------------------------------------------------------------
 - Test
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
    static EmbeddedRenderBackend be;
    be.fill_rect = fill_cb;
    be.copy_rect = copy_cb;
    be.blend_rect = blend_cb;
    embedded_renderer_set_backend(&be);

    build_scene();

    /* Mount, then settle. The first frame legitimately paints everything. */
    const long mount = frame_distinct();
    if (mount < SCREEN_PX / 2)
        return fail("mount frame did not paint the screen");
    frame_distinct();

    /* --- an idle frame under a visible modal paints NOTHING --- */
    for (int i = 0; i < 5; i++)
    {
        const long idle = frame_distinct();
        if (idle != 0)
        {
            fprintf(stderr, "  idle frame painted %ld px\n", idle);
            return fail("a modal + scrim over a static page repainted on an idle frame");
        }
    }

    /* --- a control inside the dialog stays local ---
     * The stack under it is rebuilt (page background, card, scrim, sheet, control), so allow overdraw
     * but require the AREA to stay near the control's own size. */
    const long ctrl_px = (long)CTRL_W * (long)CTRL_H;
    const int scrim_x = SW / 2, scrim_y = 100; /* over a card, under the scrim, clear of the dialog */
    uint32_t scrim = at(scrim_x, scrim_y);
    for (int i = 0; i < 6; i++)
    {
        s_ctrl_p.background_color = 0xFF66AAFFU + (uint32_t)(i + 1) * 0x00010203U;
        er_node_set_props(s_ctrl, &s_ctrl_p);
        const long d = frame_distinct();
        if (d > ctrl_px * 2)
        {
            fprintf(stderr,
                    "  changed a %ldpx control, repainted %ld px (%.1f%% of screen)\n",
                    ctrl_px,
                    d,
                    100.0 * (double)d / (double)SCREEN_PX);
            return fail("a control inside the dialog repainted far more than itself");
        }
        /* The scrim well away from the dialog must be untouched AND unchanged: a translucent fill
         * re-blended without rebuilding what is under it would darken a little every frame. */
        const uint32_t now = at(scrim_x, scrim_y);
        if (now != scrim)
        {
            fprintf(stderr, "  scrim drifted %06X -> %06X\n", scrim, now);
            return fail("the scrim darkened cumulatively while the dialog updated");
        }
        scrim = now;
    }

    /* --- a page node BEHIND the scrim stays local, and re-renders stably --- */
    const long page_px = (long)PAGE_NODE_W * (long)PAGE_NODE_H;
    for (int i = 0; i < 4; i++)
    {
        s_page_p.background_color = 0xFF88CCFFU + (uint32_t)(i + 1) * 0x00020103U;
        er_node_set_props(s_page_node, &s_page_p);
        const long d = frame_distinct();
        if (d > page_px * 2)
        {
            fprintf(stderr, "  changed a %ldpx page node, repainted %ld px\n", page_px, d);
            return fail("a page node behind the scrim repainted far more than itself");
        }
    }
    /* Settling on one colour twice must land on the same pixel both times — if the scrim were being
     * applied over an un-rebuilt stack, the second pass would come out darker. */
    s_page_p.background_color = 0xFF88CCFFU;
    er_node_set_props(s_page_node, &s_page_p);
    frame_distinct();
    const uint32_t once = at(40, 40);
    s_page_p.background_color = 0xFF88CCFFU + 0x00010101U;
    er_node_set_props(s_page_node, &s_page_p);
    frame_distinct();
    s_page_p.background_color = 0xFF88CCFFU;
    er_node_set_props(s_page_node, &s_page_p);
    frame_distinct();
    if (at(40, 40) != once)
    {
        fprintf(stderr, "  %06X then %06X\n", once, at(40, 40));
        return fail("a node behind the scrim does not settle to a stable colour");
    }

    /* --- pinned on purpose: the modal NODE ITSELF is a full-screen repaint ---
     * An overlay covers the screen, so any prop change on it damages the screen. That is correct,
     * and it is the one thing not to do per frame on a scanned-out panel. Asserting it keeps the
     * cost model honest: if this ever stops being full-screen the scrim has stopped covering. */
    s_modal_p.backdrop_color = 0x88000000U;
    er_node_set_props(s_modal, &s_modal_p);
    const long modal_dirty = frame_distinct();
    if (modal_dirty < SCREEN_PX / 2)
    {
        fprintf(stderr, "  dirtying the modal repainted only %ld px of %ld\n", modal_dirty, SCREEN_PX);
        return fail("dirtying a full-screen modal did not repaint the screen (scrim coverage lost?)");
    }

    /* --- closing the modal takes the scrim off the whole page --- */
    s_modal_p.backdrop_color = 0x99000000U;
    er_node_set_props(s_modal, &s_modal_p);
    frame_distinct();
    s_modal_p.modal_visible = 0;
    er_node_set_props(s_modal, &s_modal_p);
    frame_distinct();
    frame_distinct();
    long stale = 0;
    for (int y = 0; y < 20; y++)
        for (int x = 200; x < 260; x++) /* page background, well clear of every card and the dialog */
            if (at(x, y) != 0x203040u)
                stale++;
    if (stale != 0)
    {
        fprintf(stderr, "  %ld px still scrimmed after close\n", stale);
        return fail("closing the modal left the scrim on the page");
    }

    /* --- a modal that does NOT cover the screen still scrims the whole page ---
     *
     * The backdrop is painted over the root while the node measures its own box, so damage tracking
     * has to be told the modal paints further than it measures. Get that wrong and the scrim is
     * scissored to the box: the page outside it stays unscrimmed on show, and stays scrimmed after
     * hide. Every demo happens to give its Modal a full-screen style, which hid this. */
    er_reset();
    build_scene();
    s_modal_p.left = 60; /* deliberately NOT full-screen */
    s_modal_p.top = 200;
    s_modal_p.width = 200;
    s_modal_p.height = 150;
    s_modal_p.modal_visible = 0;
    er_node_set_props(s_modal, &s_modal_p);
    frame_distinct();
    frame_distinct();

    const int px_x = 40, px_y = 40; /* over the page node, far outside the small modal box */
    {
        const int rc = check_modal_scrim_roundtrip("non-full-screen", px_x, px_y, at(px_x, px_y), NULL);
        if (rc)
            return rc;
    }

#if ERUI_TRANSFORMS_FULL && (ERUI_XFORM_W + 1 < SW) && (ERUI_XFORM_H + 1 < SH)
    /* --- a TRANSFORMED modal owes the same root-wide scrim damage (issue #142) ---
     *
     * The pre-pass measures a node with a scale/rotate transform down a different branch, and that
     * branch used to `continue` before the Modal case ever came up: the modal was damaged by its own
     * footprint, so the scrim was scissored to the modal's box on show and only that box was erased on
     * hide — leaving stale scrim wherever anything else had repainted under it, and latching
     * modal_scrim_shown for the node's lifetime. How the rect was measured says nothing about how far
     * the backdrop reaches, so both branches route a changed modal to the same handling now.
     *
     * The transform is scale 1.0 on purpose. That is where a zoom entrance SETTLES, and
     * er_transform_is_translate_only() reads only 0.0 as "unset" — so the modal sits on the transformed
     * branch permanently, with no animation in flight. The size is one pixel over the transform source
     * so the scratch capture fails and the backdrop is filled straight to the framebuffer, scissored to
     * whatever damage this pre-pass produced: the configuration where damage alone decides the scrim. */
    er_reset();
    build_scene();
    s_modal_p.left = 60;
    s_modal_p.top = 200;
    s_modal_p.width = (int16_t)(ERUI_XFORM_W + 1);
    s_modal_p.height = (int16_t)(ERUI_XFORM_H + 1);
    s_modal_p.transform_scale_x = 1.0f;
    s_modal_p.transform_scale_y = 1.0f;
    s_modal_p.modal_visible = 0;
    er_node_set_props(s_modal, &s_modal_p);
    frame_distinct();
    frame_distinct();

    if (er_transform_source_fits(s_modal_p.width, s_modal_p.height))
        return fail("the transformed modal fits the transform source — it is not on the intended path");

    /* The while-shown callback repaints an unrelated page node while the modal is up. With the scrim
     * damaged to the modal's box only, the backdrop fill lands on THIS damage too — a stray dark patch
     * out on the page that the hide would then have no reason to erase — so the expected post-hide
     * pixel is the page's NEW color, not its pre-show one. */
    {
        const int rc = check_modal_scrim_roundtrip("transformed", px_x, px_y, 0x88CCF0u, repaint_page_node_under_modal);
        if (rc)
            return rc;
    }
#endif /* ERUI_TRANSFORMS_FULL && the modal fits on screen without covering it */

    printf(
        "PASS: modal + scrim — idle frames paint nothing; a %ldpx control repaints <= %ldpx;\n", ctrl_px, ctrl_px * 2);
    printf("      the scrim does not drift; a non-full-screen modal scrims and un-scrims the whole\n");
    printf("      page; the modal node itself is full-screen by design\n");
#if ERUI_TRANSFORMS_FULL && (ERUI_XFORM_W + 1 < SW) && (ERUI_XFORM_H + 1 < SH)
    printf("      a %dx%d transformed modal (past the %dx%d transform source) does the same\n",
           ERUI_XFORM_W + 1,
           ERUI_XFORM_H + 1,
           ERUI_XFORM_W,
           ERUI_XFORM_H);
#endif
    return EXIT_SUCCESS;
}
