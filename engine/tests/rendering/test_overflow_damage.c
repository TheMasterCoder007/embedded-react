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

#define SCREEN_WIDTH 200
#define SCREEN_HEIGHT 200
#define SCREEN_PIXELS ((long)SCREEN_WIDTH * (long)SCREEN_HEIGHT)

/*----------------------------------------------------------------------------------------------------------------------
 - Backend: composites for real, and records which pixels were written
 ---------------------------------------------------------------------------------------------------------------------*/

/* Two rotating framebuffers, so the page-flip section can model a real double-buffered panel: the
 * engine paints s_buffers[s_draw], and present() advances both it and the engine in step. The
 * single-buffer sections never flip, so they simply live in s_buffers[0] throughout. */
static uint32_t s_buffers[2][SCREEN_WIDTH * SCREEN_HEIGHT];
static int s_draw;
static uint8_t s_touched[SCREEN_WIDTH * SCREEN_HEIGHT];

#define FRAMEBUFFER_BYTES ((size_t)SCREEN_PIXELS * sizeof(uint32_t))

/** @brief The rotating buffer the engine is painting into right now. */
static uint32_t* fb(void)
{
    return s_buffers[s_draw];
}

static uint32_t div255(uint32_t value)
{
    return (value + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX;
}

static void put(int x, int y, uint32_t source_alpha, uint32_t source_red, uint32_t source_green, uint32_t source_blue)
{
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT)
        return;
    uint32_t* destination = &fb()[y * SCREEN_WIDTH + x];
    const uint32_t inverse_alpha = ER_CHANNEL_MAX - source_alpha;
    const uint32_t destination_red = (*destination >> 16) & 0xFFu, destination_green = (*destination >> 8) & 0xFFu,
                   destination_blue = *destination & 0xFFu;
    *destination = 0xFF000000u | ((source_red + div255(destination_red * inverse_alpha)) << 16)
                   | ((source_green + div255(destination_green * inverse_alpha)) << 8)
                   | (source_blue + div255(destination_blue * inverse_alpha));
    s_touched[y * SCREEN_WIDTH + x] = 1u;
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

/*
 * copy_rect hands over a PREMULTIPLIED ARGB8888 buffer, not an opaque one — the engine has a separate
 * copy_rect_format for the known-opaque case. So the per-pixel alpha is honoured here, and a fully
 * transparent pixel is skipped outright rather than "blended" at alpha 0: put() would leave the colour
 * alone but still count the pixel as touched, and a touched-pixel count is exactly what this test
 * measures. Today's scene is solid opaque Views, so neither this nor blend_cb is reached at all — the
 * handling is here so that stays true if anyone gives the scene an image, text or an opacity group.
 */
static void copy_cb(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const uint8_t* source_bytes = source;
    for (int row_index = 0; row_index < height; row_index++, source_bytes += stride)
    {
        const uint32_t* row = (const uint32_t*)source_bytes;
        for (int column = 0; column < width; column++)
        {
            const uint32_t source_pixel = row[column];
            const uint32_t source_alpha = (source_pixel >> 24) & 0xFFu;
            if (source_alpha == 0u)
                continue;
            put(x + column,
                y + row_index,
                source_alpha,
                (source_pixel >> 16) & 0xFFu,
                (source_pixel >> 8) & 0xFFu,
                source_pixel & 0xFFu);
        }
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
            if (alpha < ER_ALPHA_OPAQUE)
            {
                source_alpha = div255(source_alpha * alpha);
                source_red = div255(source_red * alpha);
                source_green = div255(source_green * alpha);
                source_blue = div255(source_blue * alpha);
            }
            if (source_alpha == 0u)
                continue; /* scaled away to nothing: not a write, so not a touched pixel either */
            put(x + column, y + row_index, source_alpha, source_red, source_green, source_blue);
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
#define CLIP_WIDTH 80
#define CLIP_HEIGHT 80
#define CHILD_X 10 /* relative to the clipper */
#define CHILD_Y 10
#define CHILD_WIDTH 120
#define CHILD_HEIGHT 120

#define BACKGROUND_COLOR 0xFF202020u
#define CLIP_COLOR 0xFF224466u
#define CHILD_COLOR 0xFFDD7722u

/* A point on the overflowing part of the child: past the clipper's right edge, inside the child. */
#define OVERFLOW_X (CLIP_X + CLIP_WIDTH + 10)
#define OVERFLOW_Y (CLIP_Y + 20)
/* And one on the part of the child that is inside the clipper either way. */
#define INSIDE_X (CLIP_X + CHILD_X + 4)
#define INSIDE_Y (CLIP_Y + CHILD_Y + 4)

/* A sibling in the far corner, untouched by any toggle: proves the damage stayed local. */
#define SIBLING_X 175
#define SIBLING_Y 175
#define SIBLING_WIDTH 20
#define SIBLING_HEIGHT 20
#define SIBLING_COLOR 0xFFFF8800u

/* Locality bound: the clipper's box unioned with the child's, plus slack for the pre-pass's
 * anti-aliasing margin. A fix that falls back to a full-screen repaint blows straight past it. */
#define TOGGLE_PIXELS_MAX ((long)(CHILD_X + CHILD_WIDTH + 8) * (long)(CHILD_Y + CHILD_HEIGHT + 8))

static ERNode* s_clipper;
static ERProps s_clip_props;

static ERProps props_default(void)
{
    ERProps props;
    er_props_default(&props);
    return props;
}

static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return EXIT_FAILURE;
}

/** @brief Reads a pixel out of rotating buffer @p buffer_index. */
static uint32_t at_in(int buffer_index, int x, int y)
{
    return s_buffers[buffer_index][y * SCREEN_WIDTH + x] & 0xFFFFFFu;
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
    long distinct_count = 0;
    for (long pixel_index = 0; pixel_index < SCREEN_PIXELS; pixel_index++)
        if (s_touched[pixel_index])
            distinct_count++;
    return distinct_count;
}

/**
 * @brief Re-renders the identical scene from scratch and checks the incremental frame matched it.
 *
 * Destroys nothing: the scene state is untouched, only the framebuffer is rebuilt, so the caller's
 * pixel assertions still hold afterwards (both frames agree, or this fails).
 */
static int matches_full_repaint(const char* label)
{
    static uint32_t incremental[SCREEN_WIDTH * SCREEN_HEIGHT];
    memcpy(incremental, fb(), FRAMEBUFFER_BYTES);

    memset(fb(), 0, FRAMEBUFFER_BYTES);
    er_force_full_repaint();
    er_commit();

    for (long pixel_index = 0; pixel_index < SCREEN_PIXELS; pixel_index++)
    {
        if (incremental[pixel_index] != fb()[pixel_index])
        {
            fprintf(stderr,
                    "  %s: first mismatch at (%ld,%ld) incremental=%08x full=%08x\n",
                    label,
                    pixel_index % SCREEN_WIDTH,
                    pixel_index / SCREEN_WIDTH,
                    incremental[pixel_index],
                    fb()[pixel_index]);
            return 0;
        }
    }
    return 1;
}

/** @brief True when the reported damage covers @p (x,y) — what a partial-update host would flush. */
static int reported_covers(int x, int y)
{
    ERRect rects[ER_DAMAGE_RECTS_MAX];
    const int rect_count = er_get_dirty_rects(rects, ER_DAMAGE_RECTS_MAX);
    for (int rect_index = 0; rect_index < rect_count; rect_index++)
        if (x >= rects[rect_index].x && y >= rects[rect_index].y && x < rects[rect_index].x + rects[rect_index].width
            && y < rects[rect_index].y + rects[rect_index].height)
            return 1;
    return 0;
}

static void build_scene(void)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = SCREEN_WIDTH;
    root_props.height = SCREEN_HEIGHT;
    root_props.background_color = BACKGROUND_COLOR;
    er_node_set_props(root, &root_props);
    er_tree_set_root(root);

    s_clipper = er_node_create(ER_NODE_VIEW);
    s_clip_props = props_default();
    s_clip_props.position = ER_POSITION_ABSOLUTE;
    s_clip_props.left = CLIP_X;
    s_clip_props.top = CLIP_Y;
    s_clip_props.width = CLIP_WIDTH;
    s_clip_props.height = CLIP_HEIGHT;
    s_clip_props.background_color = CLIP_COLOR;
    er_node_set_props(s_clipper, &s_clip_props);
    er_tree_append_child(root, s_clipper);

    ERNode* child = er_node_create(ER_NODE_VIEW);
    ERProps child_props = props_default();
    child_props.position = ER_POSITION_ABSOLUTE;
    child_props.left = CHILD_X;
    child_props.top = CHILD_Y;
    child_props.width = CHILD_WIDTH;
    child_props.height = CHILD_HEIGHT;
    child_props.background_color = CHILD_COLOR;
    er_node_set_props(child, &child_props);
    er_tree_append_child(s_clipper, child);

    ERNode* sibling = er_node_create(ER_NODE_VIEW);
    ERProps sibling_props = props_default();
    sibling_props.position = ER_POSITION_ABSOLUTE;
    sibling_props.left = SIBLING_X;
    sibling_props.top = SIBLING_Y;
    sibling_props.width = SIBLING_WIDTH;
    sibling_props.height = SIBLING_HEIGHT;
    sibling_props.background_color = SIBLING_COLOR;
    er_node_set_props(sibling, &sibling_props);
    er_tree_append_child(root, sibling);
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
    static uint32_t snapshot[2][SCREEN_WIDTH * SCREEN_HEIGHT];
    memcpy(snapshot, s_buffers, sizeof(snapshot));

    /* Forced ONCE, not per iteration: a full-repaint commit marks every buffer's debt full and then
     * discharges only the one it renders, so a second force would re-indebt the buffer just filled and
     * leave the scene owing a repaint the caller's idle checks would trip over. */
    memset(s_buffers, 0, sizeof(s_buffers));
    er_force_full_repaint();
    for (int flip_index = 0; flip_index < 2; flip_index++)
    {
        er_commit();
        present(); /* two flips of a two-buffer rotation land back on the buffer we started from */
    }

    for (int buffer_index = 0; buffer_index < 2; buffer_index++)
    {
        for (long pixel_index = 0; pixel_index < SCREEN_PIXELS; pixel_index++)
        {
            if (snapshot[buffer_index][pixel_index] != s_buffers[buffer_index][pixel_index])
            {
                fprintf(stderr,
                        "  %s: buffer %d first mismatch at (%ld,%ld) incremental=%08x full=%08x\n",
                        label,
                        buffer_index,
                        pixel_index % SCREEN_WIDTH,
                        pixel_index / SCREEN_WIDTH,
                        snapshot[buffer_index][pixel_index],
                        s_buffers[buffer_index][pixel_index]);
                return 0;
            }
        }
    }
    return 1;
}

/** @brief True when BOTH rotating buffers show @p want at the overflow point. */
static int both_buffers_show(uint32_t want)
{
    for (int buffer_index = 0; buffer_index < 2; buffer_index++)
    {
        if (at_in(buffer_index, OVERFLOW_X, OVERFLOW_Y) != (want & 0xFFFFFFu))
        {
            fprintf(stderr,
                    "  buffer %d has %06x at the overflow point, expected %06x\n",
                    buffer_index,
                    at_in(buffer_index, OVERFLOW_X, OVERFLOW_Y),
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
    for (int frame_index = 0; frame_index < 4; frame_index++)
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
    const long replay_painted = frame_distinct();
    present();

    if (replay_painted == 0)
        return fail("clipping a node was not replayed into the second rotating buffer");
    if (!both_buffers_show(BACKGROUND_COLOR))
        return fail("clipping a node left the overflow in a rotating buffer");
    if (!matches_full_repaint_both("page-flipped visible -> hidden"))
        return fail("a page-flipped clip did not land on the same frames as a full repaint");

    /* The replay window is exhausted: both buffers hold the change, so the panel goes idle again. */
    for (int frame_index = 0; frame_index < 4; frame_index++)
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

    for (int frame_index = 0; frame_index < 4; frame_index++)
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
    static EmbeddedRenderBackend backend;
    backend.fill_rect = fill_cb;
    backend.copy_rect = copy_cb;
    backend.blend_rect = blend_cb;
    embedded_renderer_set_backend(&backend);

    build_scene();

    /* Mount and settle. The first frame legitimately paints everything. */
    if (frame_distinct() < SCREEN_PIXELS / 2)
        return fail("mount frame did not paint the screen");
    frame_distinct();

    if (at(OVERFLOW_X, OVERFLOW_Y) != (CHILD_COLOR & 0xFFFFFFu))
        return fail("the child does not overflow the clipper's box (scene is not testing anything)");

    /* --- visible -> hidden: the overflowing part must be erased ------------------------------- */
    set_overflow(ER_OVERFLOW_HIDDEN);
    const long hide_painted = frame_distinct();
    if (at(OVERFLOW_X, OVERFLOW_Y) != (BACKGROUND_COLOR & 0xFFFFFFu))
        return fail("clipping a node left its child's overflow on screen");
    if (at(INSIDE_X, INSIDE_Y) != (CHILD_COLOR & 0xFFFFFFu))
        return fail("clipping a node erased the part of the child that is inside it");
    if (!reported_covers(OVERFLOW_X, OVERFLOW_Y))
        return fail("clipping a node erased the overflow without reporting it as damaged");
    if (at(SIBLING_X + 1, SIBLING_Y + 1) != (SIBLING_COLOR & 0xFFFFFFu))
        return fail("clipping a node disturbed an unrelated sibling");
    if (hide_painted > TOGGLE_PIXELS_MAX)
    {
        fprintf(stderr, "  clip toggle painted %ld px (bound %ld)\n", hide_painted, (long)TOGGLE_PIXELS_MAX);
        return fail("clipping a node repainted far more than the subtree it affects");
    }
    if (!matches_full_repaint("visible -> hidden"))
        return fail("clipping a node did not land on the same frame as a full repaint");

    /* One-shot: the toggle must not keep re-damaging the subtree on every later commit. */
    for (int frame_index = 0; frame_index < 4; frame_index++)
        if (frame_distinct() != 0)
            return fail("a clipped node kept repainting on idle frames");

    /* --- hidden -> visible: the overflowing part must come back ------------------------------- */
    set_overflow(ER_OVERFLOW_VISIBLE);
    const long show_painted = frame_distinct();
    if (at(OVERFLOW_X, OVERFLOW_Y) != (CHILD_COLOR & 0xFFFFFFu))
        return fail("unclipping a node did not paint the child's overflow");
    if (!reported_covers(OVERFLOW_X, OVERFLOW_Y))
        return fail("unclipping a node painted the overflow without reporting it as damaged");
    if (at(SIBLING_X + 1, SIBLING_Y + 1) != (SIBLING_COLOR & 0xFFFFFFu))
        return fail("unclipping a node disturbed an unrelated sibling");
    if (show_painted > TOGGLE_PIXELS_MAX)
    {
        fprintf(stderr, "  unclip toggle painted %ld px (bound %ld)\n", show_painted, (long)TOGGLE_PIXELS_MAX);
        return fail("unclipping a node repainted far more than the subtree it affects");
    }
    if (!matches_full_repaint("hidden -> visible"))
        return fail("unclipping a node did not land on the same frame as a full repaint");

    for (int frame_index = 0; frame_index < 4; frame_index++)
        if (frame_distinct() != 0)
            return fail("an unclipped node kept repainting on idle frames");

    /* --- overflow:scroll clips too, and a move between the two clipping values changes nothing -- */
    set_overflow(ER_OVERFLOW_SCROLL);
    frame_distinct();
    if (at(OVERFLOW_X, OVERFLOW_Y) != (BACKGROUND_COLOR & 0xFFFFFFu))
        return fail("overflow:scroll did not clip the child's overflow");
    if (!matches_full_repaint("visible -> scroll"))
        return fail("overflow:scroll did not land on the same frame as a full repaint");
    frame_distinct();

    /* scroll -> hidden: the clip state is the same on both sides, so the subtree bounds cannot have
     * moved and the extra damage must not be claimed. (The node's own box still repaints, as it does
     * for any visual prop set — that is not what this is measuring.) */
    set_overflow(ER_OVERFLOW_HIDDEN);
    const long same_painted = frame_distinct();
    if (same_painted > (long)(CLIP_WIDTH + 8) * (long)(CLIP_HEIGHT + 8))
    {
        fprintf(stderr, "  scroll -> hidden painted %ld px\n", same_painted);
        return fail("switching between two clipping overflow values damaged the subtree bounds");
    }
    if (reported_covers(OVERFLOW_X, OVERFLOW_Y))
        return fail("switching between two clipping overflow values reported the overflow as damaged");

    /* --- repeat the cycle: no drift, no damage that accumulates ------------------------------- */
    for (int frame_index = 0; frame_index < 3; frame_index++)
    {
        set_overflow(ER_OVERFLOW_VISIBLE);
        frame_distinct();
        if (at(OVERFLOW_X, OVERFLOW_Y) != (CHILD_COLOR & 0xFFFFFFu))
            return fail("a repeated unclip did not restore the overflow");
        if (!matches_full_repaint("repeated unclip"))
            return fail("a repeated unclip drifted from a full repaint");
        frame_distinct();

        set_overflow(ER_OVERFLOW_HIDDEN);
        frame_distinct();
        if (at(OVERFLOW_X, OVERFLOW_Y) != (BACKGROUND_COLOR & 0xFFFFFFu))
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
