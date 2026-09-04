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
 * Toggling overflow between visible and hidden (issue #132).
 *
 * The pixels the toggle changes are not the node's own — they are the ones its DESCENDANTS paint
 * outside its box, which is exactly what the damage pre-pass (measuring every node by its own rect,
 * and seeing no node move) cannot see. Both directions were wrong:
 *
 *   - visible -> hidden left the overflowing part of the child on screen, un-erased,
 *   - hidden -> visible never painted it, because the child did not move and was not dirty.
 *
 * The oracle throughout is a forced full repaint of the identical scene: an incremental,
 * damage-clipped commit has to land pixel-for-pixel on the same framebuffer. Correctness alone is
 * cheap to fake with a full-screen repaint, so each toggle is also held to a locality bound and a
 * settle-to-idle check — a fix that damages the subtree's paint bounds passes, one that gives up and
 * repaints everything does not.
 *
 * The last section runs the whole thing again page-flipped (er_set_display_buffer_count(2)), because
 * a single persistent framebuffer cannot catch the failure that matters on a real panel: the toggle
 * is a ONE-SHOT event, spent after a single commit, while the buffer the engine paints into was last
 * shown a present ago. Reaching the second buffer is the multi-buffer damage debt's job, and if the
 * toggle's damage never entered the debt set, every check above would still pass while the panel
 * ghosted the overflow on every other flip.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h" /* er_force_full_repaint — the reference frame */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SW 200
#define SH 200
#define SCREEN_PX ((long)SW * (long)SH)

/*----------------------------------------------------------------------------------------------------------------------
 - Backend: composites for real, and records which pixels were written
 ---------------------------------------------------------------------------------------------------------------------*/

/* Two rotating framebuffers, so the page-flip section can model a real double-buffered panel: the
 * engine paints s_bufs[s_draw], and present() advances both it and the engine in step. The
 * single-buffer sections never flip, so they simply live in s_bufs[0] throughout. */
static uint32_t s_bufs[2][SW * SH];
static int s_draw;
static uint8_t s_touched[SW * SH];

#define FB_BYTES ((size_t)SCREEN_PX * sizeof(uint32_t))

/** @brief The rotating buffer the engine is painting into right now. */
static uint32_t* fb(void)
{
    return s_bufs[s_draw];
}

static uint32_t div255(uint32_t v)
{
    return (v + 127u) / 255u;
}

static void put(int x, int y, uint32_t sa, uint32_t sr, uint32_t sg, uint32_t sb)
{
    if (x < 0 || x >= SW || y < 0 || y >= SH)
        return;
    uint32_t* d = &fb()[y * SW + x];
    const uint32_t inv = 255u - sa;
    const uint32_t dr = (*d >> 16) & 0xFFu, dg = (*d >> 8) & 0xFFu, db = *d & 0xFFu;
    *d = 0xFF000000u | ((sr + div255(dr * inv)) << 16) | ((sg + div255(dg * inv)) << 8) | (sb + div255(db * inv));
    s_touched[y * SW + x] = 1u;
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

/*
 * copy_rect hands over a PREMULTIPLIED ARGB8888 buffer, not an opaque one — the engine has a separate
 * copy_rect_fmt for the known-opaque case. So the per-pixel alpha is honoured here, and a fully
 * transparent pixel is skipped outright rather than "blended" at alpha 0: put() would leave the colour
 * alone but still count the pixel as touched, and a touched-pixel count is exactly what this test
 * measures. Today's scene is solid opaque Views, so neither this nor blend_cb is reached at all — the
 * handling is here so that stays true if anyone gives the scene an image, text or an opacity group.
 */
static void copy_cb(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const uint8_t* p = src;
    for (int r = 0; r < h; r++, p += stride)
    {
        const uint32_t* row = (const uint32_t*)p;
        for (int q = 0; q < w; q++)
        {
            const uint32_t v = row[q];
            const uint32_t sa = (v >> 24) & 0xFFu;
            if (sa == 0u)
                continue;
            put(x + q, y + r, sa, (v >> 16) & 0xFFu, (v >> 8) & 0xFFu, v & 0xFFu);
        }
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
            if (sa == 0u)
                continue; /* scaled away to nothing: not a write, so not a touched pixel either */
            put(x + q, y + r, sa, sr, sg, sb);
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Scene
 ---------------------------------------------------------------------------------------------------------------------*/

/* The clipper's box, and a child laid out to hang well past its right and bottom edges — the shape
 * the toggle is about. Everything outside CLIP but inside CHILD is the region in dispute. */
#define CLIP_X 30
#define CLIP_Y 30
#define CLIP_W 80
#define CLIP_H 80
#define CHILD_X 10 /* relative to the clipper */
#define CHILD_Y 10
#define CHILD_W 120
#define CHILD_H 120

#define BG_COLOR 0xFF202020u
#define CLIP_COLOR 0xFF224466u
#define CHILD_COLOR 0xFFDD7722u

/* A point on the overflowing part of the child: past the clipper's right edge, inside the child. */
#define OVER_X (CLIP_X + CLIP_W + 10)
#define OVER_Y (CLIP_Y + 20)
/* And one on the part of the child that is inside the clipper either way. */
#define INSIDE_X (CLIP_X + CHILD_X + 4)
#define INSIDE_Y (CLIP_Y + CHILD_Y + 4)

/* A sibling in the far corner, untouched by any toggle: proves the damage stayed local. */
#define SIB_X 175
#define SIB_Y 175
#define SIB_W 20
#define SIB_H 20
#define SIB_COLOR 0xFFFF8800u

/* Locality bound: the clipper's box unioned with the child's, plus slack for the pre-pass's
 * anti-aliasing margin. A fix that falls back to a full-screen repaint blows straight past it. */
#define TOGGLE_PX_MAX ((long)(CHILD_X + CHILD_W + 8) * (long)(CHILD_Y + CHILD_H + 8))

static ERNode* s_clipper;
static ERProps s_clip_props;

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

/** @brief Reads a pixel out of rotating buffer @p b. */
static uint32_t at_in(int b, int x, int y)
{
    return s_bufs[b][y * SW + x] & 0xFFFFFFu;
}

/** @brief Reads a pixel out of the buffer the engine is currently painting. */
static uint32_t at(int x, int y)
{
    return at_in(s_draw, x, y);
}

/**
 * @brief Ends one displayed frame: page-flip the panel, and follow the engine to the next buffer.
 *
 * A no-op for a single buffer, which is why the sections above never call it.
 */
static void present(void)
{
    er_display_present();
    if (er_get_display_buffer_count() > 1)
        s_draw ^= 1;
}

/** @brief Commits one frame and returns the number of DISTINCT pixels written. */
static long frame_distinct(void)
{
    memset(s_touched, 0, sizeof(s_touched));
    er_commit();
    long n = 0;
    for (long i = 0; i < SCREEN_PX; i++)
        if (s_touched[i])
            n++;
    return n;
}

/**
 * @brief Re-renders the identical scene from scratch and checks the incremental frame matched it.
 *
 * Destroys nothing: the scene state is untouched, only the framebuffer is rebuilt, so the caller's
 * pixel assertions still hold afterwards (both frames agree, or this fails).
 */
static int matches_full_repaint(const char* label)
{
    static uint32_t incremental[SW * SH];
    memcpy(incremental, fb(), FB_BYTES);

    memset(fb(), 0, FB_BYTES);
    er_force_full_repaint();
    er_commit();

    for (long i = 0; i < SCREEN_PX; i++)
    {
        if (incremental[i] != fb()[i])
        {
            fprintf(stderr,
                    "  %s: first mismatch at (%ld,%ld) incremental=%08x full=%08x\n",
                    label,
                    i % SW,
                    i / SW,
                    incremental[i],
                    fb()[i]);
            return 0;
        }
    }
    return 1;
}

/** @brief True when the reported damage covers @p (x,y) — what a partial-update host would flush. */
static int reported_covers(int x, int y)
{
    ERRect r[ER_DAMAGE_RECTS_MAX];
    const int n = er_get_dirty_rects(r, ER_DAMAGE_RECTS_MAX);
    for (int i = 0; i < n; i++)
        if (x >= r[i].x && y >= r[i].y && x < r[i].x + r[i].w && y < r[i].y + r[i].h)
            return 1;
    return 0;
}

static void build_scene(void)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = SW;
    rp.height = SH;
    rp.background_color = BG_COLOR;
    er_node_set_props(root, &rp);
    er_tree_set_root(root);

    s_clipper = er_node_create(ER_NODE_VIEW);
    s_clip_props = props_default();
    s_clip_props.position = ER_POS_ABSOLUTE;
    s_clip_props.left = CLIP_X;
    s_clip_props.top = CLIP_Y;
    s_clip_props.width = CLIP_W;
    s_clip_props.height = CLIP_H;
    s_clip_props.background_color = CLIP_COLOR;
    er_node_set_props(s_clipper, &s_clip_props);
    er_tree_append_child(root, s_clipper);

    ERNode* child = er_node_create(ER_NODE_VIEW);
    ERProps cp = props_default();
    cp.position = ER_POS_ABSOLUTE;
    cp.left = CHILD_X;
    cp.top = CHILD_Y;
    cp.width = CHILD_W;
    cp.height = CHILD_H;
    cp.background_color = CHILD_COLOR;
    er_node_set_props(child, &cp);
    er_tree_append_child(s_clipper, child);

    ERNode* sib = er_node_create(ER_NODE_VIEW);
    ERProps sp = props_default();
    sp.position = ER_POS_ABSOLUTE;
    sp.left = SIB_X;
    sp.top = SIB_Y;
    sp.width = SIB_W;
    sp.height = SIB_H;
    sp.background_color = SIB_COLOR;
    er_node_set_props(sib, &sp);
    er_tree_append_child(root, sib);
}

/** @brief Applies an overflow value to the clipper. */
static void set_overflow(uint8_t overflow)
{
    s_clip_props.overflow = overflow;
    er_node_set_props(s_clipper, &s_clip_props);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Page-flip (multi-buffer) coverage
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Rebuilds BOTH rotating buffers from scratch and checks the incremental frames matched.
 *
 * The multi-buffer counterpart of matches_full_repaint(): one forced full repaint, then a commit per
 * buffer, since a commit only ever paints the one it is aimed at.
 */
static int matches_full_repaint_both(const char* label)
{
    static uint32_t snap[2][SW * SH];
    memcpy(snap, s_bufs, sizeof(snap));

    /* Forced ONCE, not per iteration: a full-repaint commit marks every buffer's debt full and then
     * discharges only the one it renders, so a second force would re-indebt the buffer just filled and
     * leave the scene owing a repaint the caller's idle checks would trip over. */
    memset(s_bufs, 0, sizeof(s_bufs));
    er_force_full_repaint();
    for (int i = 0; i < 2; i++)
    {
        er_commit();
        present(); /* two flips of a two-buffer rotation land back on the buffer we started from */
    }

    for (int b = 0; b < 2; b++)
    {
        for (long i = 0; i < SCREEN_PX; i++)
        {
            if (snap[b][i] != s_bufs[b][i])
            {
                fprintf(stderr,
                        "  %s: buffer %d first mismatch at (%ld,%ld) incremental=%08x full=%08x\n",
                        label,
                        b,
                        i % SW,
                        i / SW,
                        snap[b][i],
                        s_bufs[b][i]);
                return 0;
            }
        }
    }
    return 1;
}

/** @brief True when BOTH rotating buffers show @p want at the overflow point. */
static int both_buffers_show(uint32_t want)
{
    for (int b = 0; b < 2; b++)
    {
        if (at_in(b, OVER_X, OVER_Y) != (want & 0xFFFFFFu))
        {
            fprintf(stderr,
                    "  buffer %d has %06x at the overflow point, expected %06x\n",
                    b,
                    at_in(b, OVER_X, OVER_Y),
                    want & 0xFFFFFFu);
            return 0;
        }
    }
    return 1;
}

/**
 * @brief The same toggle, page-flipped across two rotating buffers.
 *
 * The toggle's own flag is spent by the commit that consumes it, so only the buffer in front of the
 * engine that frame can be fixed by it directly; the other one is a present behind and has to be
 * brought current by the damage debt replaying the previous frame. That is the whole point of this
 * section — the pixel checks below are on BOTH buffers, and the replay frame has to do real work.
 */
static int check_page_flip(void)
{
    er_set_display_buffer_count(2);
    if (er_get_display_buffer_count() != 2)
        return fail("er_get_display_buffer_count() did not report the value just set");

    /* Every buffer starts owing a full repaint; flip until the scene has settled into both. */
    set_overflow(ER_OVERFLOW_VISIBLE);
    for (int i = 0; i < 4; i++)
    {
        frame_distinct();
        present();
    }
    if (!both_buffers_show(CHILD_COLOR))
        return fail("the overflow did not reach both rotating buffers before the toggle");

    /* --- clip, then flip once more so the (n-1) = 1 frame replay window closes ---------------- */
    set_overflow(ER_OVERFLOW_HIDDEN);
    frame_distinct();
    present();
    const long replay_px = frame_distinct();
    present();

    if (replay_px == 0)
        return fail("clipping a node was not replayed into the second rotating buffer");
    if (!both_buffers_show(BG_COLOR))
        return fail("clipping a node left the overflow in a rotating buffer");
    if (!matches_full_repaint_both("page-flipped visible -> hidden"))
        return fail("a page-flipped clip did not land on the same frames as a full repaint");

    /* The replay window is exhausted: both buffers hold the change, so the panel goes idle again. */
    for (int i = 0; i < 4; i++)
    {
        if (frame_distinct() != 0)
            return fail("a page-flipped clip kept repainting after both buffers were current");
        present();
    }

    /* --- and back, which is the direction that has to PAINT into both buffers ----------------- */
    set_overflow(ER_OVERFLOW_VISIBLE);
    frame_distinct();
    present();
    if (frame_distinct() == 0)
        return fail("unclipping a node was not replayed into the second rotating buffer");
    present();

    if (!both_buffers_show(CHILD_COLOR))
        return fail("unclipping a node left a rotating buffer without the overflow");
    if (!matches_full_repaint_both("page-flipped hidden -> visible"))
        return fail("a page-flipped unclip did not land on the same frames as a full repaint");

    for (int i = 0; i < 4; i++)
    {
        if (frame_distinct() != 0)
            return fail("a page-flipped unclip kept repainting after both buffers were current");
        present();
    }

    er_set_display_buffer_count(1);
    return 0;
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

    /* Mount and settle. The first frame legitimately paints everything. */
    if (frame_distinct() < SCREEN_PX / 2)
        return fail("mount frame did not paint the screen");
    frame_distinct();

    if (at(OVER_X, OVER_Y) != (CHILD_COLOR & 0xFFFFFFu))
        return fail("the child does not overflow the clipper's box (scene is not testing anything)");

    /* --- visible -> hidden: the overflowing part must be erased ------------------------------- */
    set_overflow(ER_OVERFLOW_HIDDEN);
    const long hide_px = frame_distinct();
    if (at(OVER_X, OVER_Y) != (BG_COLOR & 0xFFFFFFu))
        return fail("clipping a node left its child's overflow on screen");
    if (at(INSIDE_X, INSIDE_Y) != (CHILD_COLOR & 0xFFFFFFu))
        return fail("clipping a node erased the part of the child that is inside it");
    if (!reported_covers(OVER_X, OVER_Y))
        return fail("clipping a node erased the overflow without reporting it as damaged");
    if (at(SIB_X + 1, SIB_Y + 1) != (SIB_COLOR & 0xFFFFFFu))
        return fail("clipping a node disturbed an unrelated sibling");
    if (hide_px > TOGGLE_PX_MAX)
    {
        fprintf(stderr, "  clip toggle painted %ld px (bound %ld)\n", hide_px, (long)TOGGLE_PX_MAX);
        return fail("clipping a node repainted far more than the subtree it affects");
    }
    if (!matches_full_repaint("visible -> hidden"))
        return fail("clipping a node did not land on the same frame as a full repaint");

    /* One-shot: the toggle must not keep re-damaging the subtree on every later commit. */
    for (int i = 0; i < 4; i++)
        if (frame_distinct() != 0)
            return fail("a clipped node kept repainting on idle frames");

    /* --- hidden -> visible: the overflowing part must come back ------------------------------- */
    set_overflow(ER_OVERFLOW_VISIBLE);
    const long show_px = frame_distinct();
    if (at(OVER_X, OVER_Y) != (CHILD_COLOR & 0xFFFFFFu))
        return fail("unclipping a node did not paint the child's overflow");
    if (!reported_covers(OVER_X, OVER_Y))
        return fail("unclipping a node painted the overflow without reporting it as damaged");
    if (at(SIB_X + 1, SIB_Y + 1) != (SIB_COLOR & 0xFFFFFFu))
        return fail("unclipping a node disturbed an unrelated sibling");
    if (show_px > TOGGLE_PX_MAX)
    {
        fprintf(stderr, "  unclip toggle painted %ld px (bound %ld)\n", show_px, (long)TOGGLE_PX_MAX);
        return fail("unclipping a node repainted far more than the subtree it affects");
    }
    if (!matches_full_repaint("hidden -> visible"))
        return fail("unclipping a node did not land on the same frame as a full repaint");

    for (int i = 0; i < 4; i++)
        if (frame_distinct() != 0)
            return fail("an unclipped node kept repainting on idle frames");

    /* --- overflow:scroll clips too, and a move between the two clipping values changes nothing -- */
    set_overflow(ER_OVERFLOW_SCROLL);
    frame_distinct();
    if (at(OVER_X, OVER_Y) != (BG_COLOR & 0xFFFFFFu))
        return fail("overflow:scroll did not clip the child's overflow");
    if (!matches_full_repaint("visible -> scroll"))
        return fail("overflow:scroll did not land on the same frame as a full repaint");
    frame_distinct();

    /* scroll -> hidden: the clip state is the same on both sides, so the subtree bounds cannot have
     * moved and the extra damage must not be claimed. (The node's own box still repaints, as it does
     * for any visual prop set — that is not what this is measuring.) */
    set_overflow(ER_OVERFLOW_HIDDEN);
    const long same_px = frame_distinct();
    if (same_px > (long)(CLIP_W + 8) * (long)(CLIP_H + 8))
    {
        fprintf(stderr, "  scroll -> hidden painted %ld px\n", same_px);
        return fail("switching between two clipping overflow values damaged the subtree bounds");
    }
    if (reported_covers(OVER_X, OVER_Y))
        return fail("switching between two clipping overflow values reported the overflow as damaged");

    /* --- repeat the cycle: no drift, no damage that accumulates ------------------------------- */
    for (int i = 0; i < 3; i++)
    {
        set_overflow(ER_OVERFLOW_VISIBLE);
        frame_distinct();
        if (at(OVER_X, OVER_Y) != (CHILD_COLOR & 0xFFFFFFu))
            return fail("a repeated unclip did not restore the overflow");
        if (!matches_full_repaint("repeated unclip"))
            return fail("a repeated unclip drifted from a full repaint");
        frame_distinct();

        set_overflow(ER_OVERFLOW_HIDDEN);
        frame_distinct();
        if (at(OVER_X, OVER_Y) != (BG_COLOR & 0xFFFFFFu))
            return fail("a repeated clip did not erase the overflow");
        if (!matches_full_repaint("repeated clip"))
            return fail("a repeated clip drifted from a full repaint");
        frame_distinct();
    }

    if (check_page_flip() != 0)
        return EXIT_FAILURE;

    printf("PASS: overflow toggle damage (both directions, locality, idle cost, page-flip replay)\n");
    return EXIT_SUCCESS;
}
