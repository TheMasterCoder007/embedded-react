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

#define SCREEN_WIDTH 400
#define SCREEN_HEIGHT 640
#define SCREEN_PIXELS ((long)SCREEN_WIDTH * (long)SCREEN_HEIGHT)
#define ALPHA_OPAQUE 255
#define CHANNEL_MAX 255u
#define CHANNEL_ROUND 127u

/*----------------------------------------------------------------------------------------------------------------------
 - Backend: composites for real, and records which pixels were written
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_framebuffer[SCREEN_WIDTH * SCREEN_HEIGHT];
static uint8_t s_touched[SCREEN_WIDTH * SCREEN_HEIGHT];
static long s_painted; /* every write, so overdraw inside the damage region is visible too */

static uint32_t div255(uint32_t value)
{
    return (value + CHANNEL_ROUND) / CHANNEL_MAX;
}

static void put(int x, int y, uint32_t source_alpha, uint32_t source_red, uint32_t source_green, uint32_t source_blue)
{
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT)
        return;
    uint32_t* destination = &s_framebuffer[y * SCREEN_WIDTH + x];
    const uint32_t inverse_alpha = CHANNEL_MAX - source_alpha;
    const uint32_t destination_red = (*destination >> 16) & 0xFFu, destination_green = (*destination >> 8) & 0xFFu,
                   destination_blue = *destination & 0xFFu;
    *destination = 0xFF000000u | ((source_red + div255(destination_red * inverse_alpha)) << 16)
                   | ((source_green + div255(destination_green * inverse_alpha)) << 8)
                   | (source_blue + div255(destination_blue * inverse_alpha));
    s_touched[y * SCREEN_WIDTH + x] = 1u;
    s_painted++;
}

static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const uint32_t alpha = (argb >> 24) & 0xFFu;
    if (!alpha)
        return;
    const uint32_t source_red = div255(((argb >> 16) & 0xFFu) * alpha);
    const uint32_t source_green = div255(((argb >> 8) & 0xFFu) * alpha);
    const uint32_t source_blue = div255((argb & 0xFFu) * alpha);
    for (int pixel_y = y; pixel_y < y + height; pixel_y++)
        for (int pixel_x = x; pixel_x < x + width; pixel_x++)
            put(pixel_x, pixel_y, alpha, source_red, source_green, source_blue);
}

static void copy_cb(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const uint8_t* source_bytes = source;
    for (int row_index = 0; row_index < height; row_index++, source_bytes += stride)
    {
        const uint32_t* row = (const uint32_t*)source_bytes;
        for (int column = 0; column < width; column++)
            put(x + column,
                y + row_index,
                ALPHA_OPAQUE,
                (row[column] >> 16) & 0xFFu,
                (row[column] >> 8) & 0xFFu,
                row[column] & 0xFFu);
    }
}

static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const uint8_t* source_bytes = source;
    for (int row_index = 0; row_index < height; row_index++, source_bytes += stride)
    {
        const uint32_t* row = (const uint32_t*)source_bytes;
        for (int column = 0; column < width; column++)
        {
            const uint32_t source_pixel = row[column];
            uint32_t source_alpha = (source_pixel >> 24) & 0xFFu, source_red = (source_pixel >> 16) & 0xFFu,
                     source_green = (source_pixel >> 8) & 0xFFu, source_blue = source_pixel & 0xFFu;
            if (alpha < ALPHA_OPAQUE)
            {
                source_alpha = div255(source_alpha * alpha);
                source_red = div255(source_red * alpha);
                source_green = div255(source_green * alpha);
                source_blue = div255(source_blue * alpha);
            }
            put(x + column, y + row_index, source_alpha, source_red, source_green, source_blue);
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static ERProps props_default(void)
{
    ERProps source_bytes;
    er_props_default(&source_bytes);
    return source_bytes;
}

static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return EXIT_FAILURE;
}

/** @brief Commits one frame and returns the number of DISTINCT pixels written. */
static long frame_distinct(void)
{
    s_painted = 0;
    memset(s_touched, 0, sizeof(s_touched));
    er_commit();
    er_display_present();
    long distinct_count = 0;
    for (long frame_index = 0; frame_index < SCREEN_PIXELS; frame_index++)
        if (s_touched[frame_index])
            distinct_count++;
    return distinct_count;
}

static uint32_t at(int x, int y)
{
    return s_framebuffer[y * SCREEN_WIDTH + x] & 0xFFFFFFu;
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
    ERRect dirty;
    if (!er_get_dirty_rect(&dirty))
    {
        fprintf(stderr, "  %s: er_get_dirty_rect() reported nothing\n", when);
        return 1;
    }
    if (dirty.x > 0 || dirty.y > 0 || dirty.x + dirty.w < SCREEN_WIDTH || dirty.y + dirty.h < SCREEN_HEIGHT)
    {
        fprintf(stderr,
                "  %s: dirty rect (%d,%d %dx%d) does not span the %dx%d root\n",
                when,
                dirty.x,
                dirty.y,
                dirty.w,
                dirty.h,
                SCREEN_WIDTH,
                SCREEN_HEIGHT);
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
#define SHEET_WIDTH 300
#define SHEET_HEIGHT 160
#define CONTROL_WIDTH 100
#define CONTROL_HEIGHT 30
#define PAGE_NODE_WIDTH 90
#define PAGE_NODE_HEIGHT 25

static ERNode* s_root;
static ERNode* s_modal;
static ERNode* s_control;   /* inside the dialog */
static ERNode* s_page_node; /* behind the scrim */
static ERProps s_modal_props, s_control_props, s_page_props;

static void build_scene(void)
{
    s_root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = SCREEN_WIDTH;
    root_props.height = SCREEN_HEIGHT;
    root_props.background_color = 0xFF203040U;
    er_node_set_props(s_root, &root_props);
    er_tree_set_root(s_root);

    for (int frame_index = 0; frame_index < 4; frame_index++)
    {
        ERNode* card = er_node_create(ER_NODE_VIEW);
        ERProps card_props = props_default();
        card_props.position = ER_POS_ABSOLUTE;
        card_props.left = 20;
        card_props.top = (int16_t)(20 + frame_index * 150);
        card_props.width = 360;
        card_props.height = 120;
        card_props.border_radius = 12;
        card_props.background_color = 0xFF3A4A5AU;
        er_node_set_props(card, &card_props);
        er_tree_append_child(s_root, card);
    }

    s_page_node = er_node_create(ER_NODE_VIEW);
    s_page_props = props_default();
    s_page_props.position = ER_POS_ABSOLUTE;
    s_page_props.left = 30;
    s_page_props.top = 30;
    s_page_props.width = PAGE_NODE_WIDTH;
    s_page_props.height = PAGE_NODE_HEIGHT;
    s_page_props.background_color = 0xFF88CCFFU;
    er_node_set_props(s_page_node, &s_page_props);
    er_tree_append_child(s_root, s_page_node);

    /* The idiomatic overlay: the Modal node covers the screen, the dialog is a child of it. The
     * scrim is painted over the root rect, so a modal that does NOT cover the screen would have its
     * scrim scissored to its own box — hence the full-screen style every demo gives it. */
    s_modal = er_node_create(ER_NODE_MODAL);
    s_modal_props = props_default();
    s_modal_props.position = ER_POS_ABSOLUTE;
    s_modal_props.left = 0;
    s_modal_props.top = 0;
    s_modal_props.width = SCREEN_WIDTH;
    s_modal_props.height = SCREEN_HEIGHT;
    s_modal_props.modal_visible = 1;
    s_modal_props.backdrop_color = 0x99000000U;
    er_node_set_props(s_modal, &s_modal_props);
    er_tree_append_child(s_root, s_modal);

    ERNode* sheet = er_node_create(ER_NODE_VIEW);
    ERProps sheet_props = props_default();
    sheet_props.position = ER_POS_ABSOLUTE;
    sheet_props.left = SHEET_X;
    sheet_props.top = SHEET_Y;
    sheet_props.width = SHEET_WIDTH;
    sheet_props.height = SHEET_HEIGHT;
    sheet_props.background_color = 0xFF101820U;
    sheet_props.border_radius = 16;
    er_node_set_props(sheet, &sheet_props);
    er_tree_append_child(s_modal, sheet);

    s_control = er_node_create(ER_NODE_VIEW);
    s_control_props = props_default();
    s_control_props.position = ER_POS_ABSOLUTE;
    s_control_props.left = 20;
    s_control_props.top = 20;
    s_control_props.width = CONTROL_WIDTH;
    s_control_props.height = CONTROL_HEIGHT;
    s_control_props.background_color = 0xFF66AAFFU;
    er_node_set_props(s_control, &s_control_props);
    er_tree_append_child(sheet, s_control);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Test
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
    static EmbeddedRenderBackend backend;
    backend.fill_rect = fill_cb;
    backend.copy_rect = copy_cb;
    backend.blend_rect = blend_cb;
    embedded_renderer_set_backend(&backend);

    build_scene();

    /* Mount, then settle. The first frame legitimately paints everything. */
    const long mount = frame_distinct();
    if (mount < SCREEN_PIXELS / 2)
        return fail("mount frame did not paint the screen");
    frame_distinct();

    /* --- an idle frame under a visible modal paints NOTHING --- */
    for (int frame_index = 0; frame_index < 5; frame_index++)
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
    const long control_pixels = (long)CONTROL_WIDTH * (long)CONTROL_HEIGHT;
    const int scrim_x = SCREEN_WIDTH / 2, scrim_y = 100; /* over a card, under the scrim, clear of the dialog */
    uint32_t scrim = at(scrim_x, scrim_y);
    for (int frame_index = 0; frame_index < 6; frame_index++)
    {
        s_control_props.background_color = 0xFF66AAFFU + (uint32_t)(frame_index + 1) * 0x00010203U;
        er_node_set_props(s_control, &s_control_props);
        const long distinct = frame_distinct();
        if (distinct > control_pixels * 2)
        {
            fprintf(stderr,
                    "  changed a %ldpx control, repainted %ld px (%.1f%% of screen)\n",
                    control_pixels,
                    distinct,
                    100.0 * (double)distinct / (double)SCREEN_PIXELS);
            return fail("a control inside the dialog repainted far more than itself");
        }
        /* The scrim well away from the dialog must be untouched AND unchanged: a translucent fill
         * re-blended without rebuilding what is under it would darken a little every frame. */
        const uint32_t current = at(scrim_x, scrim_y);
        if (current != scrim)
        {
            fprintf(stderr, "  scrim drifted %06X -> %06X\n", scrim, current);
            return fail("the scrim darkened cumulatively while the dialog updated");
        }
        scrim = current;
    }

    /* --- a page node BEHIND the scrim stays local, and re-renders stably --- */
    const long page_pixels = (long)PAGE_NODE_WIDTH * (long)PAGE_NODE_HEIGHT;
    for (int frame_index = 0; frame_index < 4; frame_index++)
    {
        s_page_props.background_color = 0xFF88CCFFU + (uint32_t)(frame_index + 1) * 0x00020103U;
        er_node_set_props(s_page_node, &s_page_props);
        const long distinct = frame_distinct();
        if (distinct > page_pixels * 2)
        {
            fprintf(stderr, "  changed a %ldpx page node, repainted %ld px\n", page_pixels, distinct);
            return fail("a page node behind the scrim repainted far more than itself");
        }
    }
    /* Settling on one colour twice must land on the same pixel both times — if the scrim were being
     * applied over an un-rebuilt stack, the second pass would come out darker. */
    s_page_props.background_color = 0xFF88CCFFU;
    er_node_set_props(s_page_node, &s_page_props);
    frame_distinct();
    const uint32_t once = at(40, 40);
    s_page_props.background_color = 0xFF88CCFFU + 0x00010101U;
    er_node_set_props(s_page_node, &s_page_props);
    frame_distinct();
    s_page_props.background_color = 0xFF88CCFFU;
    er_node_set_props(s_page_node, &s_page_props);
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
    s_modal_props.backdrop_color = 0x88000000U;
    er_node_set_props(s_modal, &s_modal_props);
    const long modal_dirty = frame_distinct();
    if (modal_dirty < SCREEN_PIXELS / 2)
    {
        fprintf(stderr, "  dirtying the modal repainted only %ld px of %ld\n", modal_dirty, SCREEN_PIXELS);
        return fail("dirtying a full-screen modal did not repaint the screen (scrim coverage lost?)");
    }

    /* --- closing the modal takes the scrim off the whole page --- */
    s_modal_props.backdrop_color = 0x99000000U;
    er_node_set_props(s_modal, &s_modal_props);
    frame_distinct();
    s_modal_props.modal_visible = 0;
    er_node_set_props(s_modal, &s_modal_props);
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
    s_modal_props.left = 60; /* deliberately NOT full-screen */
    s_modal_props.top = 200;
    s_modal_props.width = 200;
    s_modal_props.height = 150;
    s_modal_props.modal_visible = 0;
    er_node_set_props(s_modal, &s_modal_props);
    frame_distinct();
    frame_distinct();

    const int probe_x = 40, probe_y = 40; /* over the page node, far outside the small modal box */
    const uint32_t unscrimmed = at(probe_x, probe_y);

    s_modal_props.modal_visible = 1;
    er_node_set_props(s_modal, &s_modal_props);
    frame_distinct();
    if (at(probe_x, probe_y) == unscrimmed)
    {
        fprintf(stderr, "  (%d,%d) still %06X with the modal shown\n", probe_x, probe_y, unscrimmed);
        return fail("a non-full-screen modal did not scrim the page outside its own box");
    }
    if (dirty_rect_spans_root("show"))
        return fail("raising a scrim did not report a full-root dirty rect");

    s_modal_props.modal_visible = 0;
    er_node_set_props(s_modal, &s_modal_props);
    frame_distinct();
    if (dirty_rect_spans_root("hide"))
        return fail("dropping a scrim did not report a full-root dirty rect");
    frame_distinct();
    if (at(probe_x, probe_y) != unscrimmed)
    {
        fprintf(stderr, "  (%d,%d) is %06X, expected %06X\n", probe_x, probe_y, at(probe_x, probe_y), unscrimmed);
        return fail("hiding a non-full-screen modal left its scrim on the page");
    }
    long stale_small = 0;
    for (int y = 0; y < 20; y++)
        for (int x = 200; x < 260; x++)
            if (at(x, y) != 0x203040u)
                stale_small++;
    if (stale_small != 0)
    {
        fprintf(stderr, "  %ld px still scrimmed\n", stale_small);
        return fail("hiding a non-full-screen modal left scrim residue on the page");
    }

    /* An on-screen non-full-screen modal must still cost nothing while idle. */
    s_modal_props.modal_visible = 1;
    er_node_set_props(s_modal, &s_modal_props);
    frame_distinct();
    frame_distinct();
    for (int frame_index = 0; frame_index < 3; frame_index++)
        if (frame_distinct() != 0)
            return fail("a non-full-screen modal repainted on an idle frame");

#if ERUI_TRANSFORMS_FULL && (ERUI_XFORM_W + 1 < SCREEN_WIDTH) && (ERUI_XFORM_H + 1 < SCREEN_HEIGHT)
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
    s_modal_props.left = 60;
    s_modal_props.top = 200;
    s_modal_props.width = (int16_t)(ERUI_XFORM_W + 1);
    s_modal_props.height = (int16_t)(ERUI_XFORM_H + 1);
    s_modal_props.transform_scale_x = 1.0f;
    s_modal_props.transform_scale_y = 1.0f;
    s_modal_props.modal_visible = 0;
    er_node_set_props(s_modal, &s_modal_props);
    frame_distinct();
    frame_distinct();

    if (er_transform_source_fits(s_modal_props.width, s_modal_props.height))
        return fail("the transformed modal fits the transform source — it is not on the intended path");

    const uint32_t transformed_clean = at(probe_x, probe_y); /* over the page node, far outside the modal's box */

    s_modal_props.modal_visible = 1;
    er_node_set_props(s_modal, &s_modal_props);
    frame_distinct();
    if (at(probe_x, probe_y) == transformed_clean)
    {
        fprintf(stderr, "  (%d,%d) still %06X with the transformed modal shown\n", probe_x, probe_y, transformed_clean);
        return fail("a transformed modal did not scrim the page outside its own box");
    }
    if (dirty_rect_spans_root("transformed show"))
        return fail("raising a transformed modal's scrim did not report a full-root dirty rect");

    /* Repaint an unrelated page node while the modal is up. With the scrim damaged to the modal's box
     * only, the backdrop fill lands on THIS damage too — a stray dark patch out on the page that the
     * hide below then has no reason to erase. */
    s_page_props.background_color = 0xFF88CCF0U;
    er_node_set_props(s_page_node, &s_page_props);
    frame_distinct();

    s_modal_props.modal_visible = 0;
    er_node_set_props(s_modal, &s_modal_props);
    frame_distinct();
    if (dirty_rect_spans_root("transformed hide"))
        return fail("dropping a transformed modal's scrim did not report a full-root dirty rect");
    frame_distinct();

    if (at(probe_x, probe_y) != 0x88CCF0u)
    {
        fprintf(stderr, "  (%d,%d) is %06X, expected %06X\n", probe_x, probe_y, at(probe_x, probe_y), 0x88CCF0u);
        return fail("hiding a transformed modal left scrim on a node that repainted under it");
    }
    long transformed_stale = 0;
    for (int y = 0; y < 20; y++)
        for (int x = 200; x < 260; x++)
            if (at(x, y) != 0x203040u)
                transformed_stale++;
    if (transformed_stale != 0)
    {
        fprintf(stderr, "  %ld px still scrimmed\n", transformed_stale);
        return fail("hiding a transformed modal left scrim residue on the page");
    }

    /* And it must still cost nothing while it sits there — the shared handling is reached only for a
     * modal that is source-dirty or moved, exactly like the untransformed one. */
    s_modal_props.modal_visible = 1;
    er_node_set_props(s_modal, &s_modal_props);
    frame_distinct();
    frame_distinct();
    for (int frame_index = 0; frame_index < 3; frame_index++)
        if (frame_distinct() != 0)
            return fail("a transformed modal repainted on an idle frame");
#endif /* ERUI_TRANSFORMS_FULL && the modal fits on screen without covering it */

    printf("PASS: modal + scrim — idle frames paint nothing; a %ldpx control repaints <= %ldpx;\n",
           control_pixels,
           control_pixels * 2);
    printf("      the scrim does not drift; a non-full-screen modal scrims and un-scrims the whole\n");
    printf("      page; the modal node itself is full-screen by design\n");
#if ERUI_TRANSFORMS_FULL && (ERUI_XFORM_W + 1 < SCREEN_WIDTH) && (ERUI_XFORM_H + 1 < SCREEN_HEIGHT)
    printf("      a %dx%d transformed modal (past the %dx%d transform source) does the same\n",
           ERUI_XFORM_W + 1,
           ERUI_XFORM_H + 1,
           ERUI_XFORM_W,
           ERUI_XFORM_H);
#endif
    return EXIT_SUCCESS;
}
