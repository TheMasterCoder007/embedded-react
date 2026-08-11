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
 * Frame instrumentation (er_perf.h): the per-frame timing split and the resource counters.
 *
 * Real durations are not reproducible, so these tests install a FAKE clock that advances by a fixed
 * step on every read. Each begin/end pair therefore measures exactly one step, which turns "did the
 * engine time the right thing?" into an exact-equality assertion:
 *
 *   - a frame that ran layout reports layout_us == step; a static frame reports exactly 0 (the phase
 *     was never entered — the fast path really was taken),
 *   - the four phases plus other_us always reconstruct frame_us,
 *   - the worst frame is retained with its whole split, so a spike minutes ago is still attributable,
 *   - the dirty-rect / vector-slot / image-slot counters track the scene.
 */

#include "er_perf.h"
#include "er_scene.h"
#include "native_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Fake clock: every read advances by g_step, so one begin/end pair == exactly one step
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t g_now_us = 1000000U; /* start well off zero so a bug can't pass by reading 0 */
static uint32_t g_step = 1000U;

static uint32_t fake_now_us(void)
{
    const uint32_t t = g_now_us;
    g_now_us += g_step;
    return t;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static void noop_fill(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)argb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
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

static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return EXIT_FAILURE;
}

/* One host frame: exactly what examples/linux/host.c does around the engine. */
static void frame(void)
{
    er_perf_frame_begin();
    er_perf_phase_begin(ER_PERF_PHASE_JS);
    er_perf_phase_end(ER_PERF_PHASE_JS);
    er_commit();
    er_perf_phase_begin(ER_PERF_PHASE_PRESENT);
    er_perf_phase_end(ER_PERF_PHASE_PRESENT);
    er_perf_frame_end();
}

/* Build a scene: opaque root + a small recolourable box. Returns the box node. */
static ERNode* build_scene(int screen, ERNode** out_root)
{
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = screen;
    rp.height = screen;
    rp.background_color = 0xFFFFFFFFU;
    er_node_set_props(root, &rp);

    ERNode* box = er_node_create(ER_NODE_VIEW);
    ERProps bp = props_default();
    bp.width = 30;
    bp.height = 30;
    bp.margin_left = 20;
    bp.margin_top = 20;
    bp.background_color = 0xFF3366FFU;
    er_node_set_props(box, &bp);

    er_tree_append_child(root, box);
    er_tree_set_root(root);
    *out_root = root;
    return box;
}

/* Recolours the box, which dirties it without moving it (damage == the box rect, no relayout). */
static void recolour(ERNode* box, uint32_t argb)
{
    ERProps bp = props_default();
    bp.width = 30;
    bp.height = 30;
    bp.margin_left = 20;
    bp.margin_top = 20;
    bp.background_color = argb;
    er_node_set_props(box, &bp);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests
 ---------------------------------------------------------------------------------------------------------------------*/

/* The split: layout is timed only on frames that actually laid out, and the phases account for the
 * whole frame. This is the property that makes a spike attributable at all. */
static int check_phase_split(int screen)
{
    ERNode* root;
    ERNode* box = build_scene(screen, &root);

    frame(); /* mount frame: layout runs */

    ERPerfFrame f;
    if (!er_perf_get_last(&f))
        return fail("no frame recorded after the first commit");
    if (f.phase_us[ER_PERF_PHASE_LAYOUT] != g_step)
        return fail("mount frame did not time the layout pass");
    if (f.phase_us[ER_PERF_PHASE_RASTER] != g_step)
        return fail("mount frame did not time the raster pass");
    if (f.phase_us[ER_PERF_PHASE_JS] != g_step || f.phase_us[ER_PERF_PHASE_PRESENT] != g_step)
        return fail("host-marked JS/present phases were not timed");

    uint32_t sum = f.other_us;
    for (int i = 0; i < (int)ER_PERF_PHASE_COUNT; i++)
        sum += f.phase_us[i];
    if (sum != f.frame_us)
        return fail("phases + other_us do not add up to frame_us");

    /* Idle frame: nothing changed, so the layout fast path skips the solver entirely and the phase is
     * never entered. Exactly 0 — not "small" — is the assertion worth making. */
    frame();
    if (!er_perf_get_last(&f))
        return fail("no frame recorded for the idle frame");
    if (f.phase_us[ER_PERF_PHASE_LAYOUT] != 0U)
        return fail("idle frame reported layout time although the solver never ran");
    if (f.phase_us[ER_PERF_PHASE_RASTER] != g_step)
        return fail("idle frame did not time the raster phase (the damage pre-pass still runs)");

    /* A recolour changes no layout input, yet er_node_set_props conservatively marks layout dirty, so
     * the solver DOES run again. Worth pinning: this is precisely the kind of cost the split is here
     * to make visible — the frame is not "just a repaint" and the numbers must not pretend it is. */
    recolour(box, 0xFF22CC88U);
    frame();
    if (!er_perf_get_last(&f))
        return fail("no frame recorded for the recolour frame");
    if (f.phase_us[ER_PERF_PHASE_LAYOUT] != g_step)
        return fail("a setProps-driven relayout was not attributed to the layout phase");

    er_node_destroy(root);
    printf("PASS: phase split — layout timed only when it runs, phases account for the frame\n");
    return EXIT_SUCCESS;
}

/* The point of the module: a single 2-second-style spike is retained with its full split long after
 * the frames that followed it were fast. */
static int check_worst_frame_is_retained(int screen)
{
    ERNode* root;
    ERNode* box = build_scene(screen, &root);
    frame(); /* mount */

    /* The spike: one frame whose JS phase alone costs 2 seconds. Only the JS begin/end pair straddles
     * the jump, so the spike lands in exactly one phase — as it would in production. */
    recolour(box, 0xFFDD8800U);
    er_perf_frame_begin();
    er_perf_phase_begin(ER_PERF_PHASE_JS);
    g_now_us += 2000000U; /* 2 s of JS work */
    er_perf_phase_end(ER_PERF_PHASE_JS);
    er_commit();
    er_perf_frame_end();

    ERPerfFrame spike;
    if (!er_perf_get_worst(&spike))
        return fail("no worst frame after the spike");
    const uint32_t spike_index = spike.index;
    if (spike.phase_us[ER_PERF_PHASE_JS] < 2000000U)
        return fail("the spike was not attributed to the JS phase");
    if (spike.phase_us[ER_PERF_PHASE_JS] <= spike.phase_us[ER_PERF_PHASE_RASTER])
        return fail("the spike's largest phase is not the one that was slow");

    /* Twenty ordinary frames later the peak must still be the spike, with its split intact. */
    for (int i = 0; i < 20; i++)
    {
        recolour(box, (uint32_t)(0xFF000000U | (uint32_t)(i * 0x010203)));
        frame();
    }

    ERPerfFrame still;
    if (!er_perf_get_worst(&still))
        return fail("worst frame lost after subsequent frames");
    if (still.index != spike_index)
        return fail("a later, faster frame displaced the retained peak");
    if (still.phase_us[ER_PERF_PHASE_JS] != spike.phase_us[ER_PERF_PHASE_JS])
        return fail("the retained peak's split changed");

    ERPerfFrame last;
    er_perf_get_last(&last);
    if (last.frame_us >= still.frame_us)
        return fail("the ordinary frames were not cheaper than the retained peak");

    /* Reset clears the peak so the next screen's spike is not hidden behind this one. */
    er_perf_reset();
    if (er_perf_get_worst(&still))
        return fail("er_perf_reset did not clear the retained peak");

    er_node_destroy(root);
    printf("PASS: worst frame retained with its split (%u us in JS) across 20 later frames\n",
           (unsigned)spike.phase_us[ER_PERF_PHASE_JS]);
    return EXIT_SUCCESS;
}

/* The counters: dirty-rect area tracks what was actually repainted, and the pool gauges track the
 * scene's vector / image slot usage. */
static int check_counters(int screen)
{
    static const uint32_t pixels[4] = {0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU};

    ERNode* root;
    ERNode* box = build_scene(screen, &root);

    frame(); /* mount: full-screen repaint */
    ERPerfFrame f;
    er_perf_get_last(&f);
    if (f.dirty_px != (uint32_t)(screen * screen))
        return fail("the mount frame's dirty area is not the whole screen");

    frame(); /* idle: nothing repainted */
    er_perf_get_last(&f);
    if (f.dirty_w != 0 || f.dirty_h != 0 || f.dirty_px != 0U)
        return fail("an idle frame reported a non-empty dirty rect");

    /* A recolour repaints the box and nothing else, so the area collapses by orders of magnitude —
     * exactly the signal that separates "one small node changed" from "the whole screen redrew". */
    recolour(box, 0xFF9900FFU);
    frame();
    er_perf_get_last(&f);
    if (f.dirty_px == 0U)
        return fail("the recolour frame reported no repaint");
    if (f.dirty_px >= (uint32_t)(screen * screen))
        return fail("a single recoloured node repainted the whole screen");
    if (f.dirty_w < 30 || f.dirty_h < 30)
        return fail("the dirty rect does not cover the changed node");

    /* Resource pools: start empty, then fill as the scene registers assets. */
    if (f.vector_slots_used != 0U)
        return fail("vector slots reported in use before any vector node exists");
    if (f.vector_slots_total == 0U || f.image_slots_total == 0U)
        return fail("pool sizes reported as zero");

    er_image_load("perf-test-a", pixels, 2, 2);
    er_image_load("perf-test-b", pixels, 2, 2);

    ERNode* vec = er_node_create(ER_NODE_VECTOR);
    ERProps vp = props_default();
    vp.width = 40;
    vp.height = 40;
    er_node_set_props(vec, &vp);
    /* One filled rectangle: MOVE, LINE x3, CLOSE — enough to claim a storage slot. */
    const float ops[] = {(float)ER_VOP_MOVE,
                         0.0f,
                         0.0f,
                         (float)ER_VOP_LINE,
                         40.0f,
                         0.0f,
                         (float)ER_VOP_LINE,
                         40.0f,
                         40.0f,
                         (float)ER_VOP_LINE,
                         0.0f,
                         40.0f};
    ERVectorPaint paint = {0};
    paint.fill = 0xFF00FF00U;
    paint.fill_rule = ER_VFILL_NONZERO;
    er_node_set_vector_ops(vec, ops, (int)(sizeof(ops) / sizeof(ops[0])), &paint, 1, NULL, 0);
    er_tree_append_child(root, vec);

    frame();
    er_perf_get_last(&f);
    if (f.vector_slots_used != 1U)
        return fail("the vector node did not show up in the vector-slot counter");
    if (f.image_slots_used != 2U)
        return fail("registered images did not show up in the image-slot counter");
    if (f.vector_slots_used > f.vector_slots_total || f.image_slots_used > f.image_slots_total)
        return fail("a slot counter exceeded its pool size");

    printf("PASS: counters — dirty %dx%d (%u px), VEC %u/%u, IMG %u/%u\n",
           (int)f.dirty_w,
           (int)f.dirty_h,
           (unsigned)f.dirty_px,
           (unsigned)f.vector_slots_used,
           (unsigned)f.vector_slots_total,
           (unsigned)f.image_slots_used,
           (unsigned)f.image_slots_total);

    er_node_destroy(root);
    return EXIT_SUCCESS;
}

/* The overlay lines: non-empty, null-terminated, and short enough for a 240 px panel. */
static int check_overlay_lines(void)
{
    const char* lines[ER_PERF_OVERLAY_LINES];
    memset(lines, 0, sizeof(lines));

    const int n = er_perf_overlay_lines(lines, ER_PERF_OVERLAY_LINES);
    if (n != ER_PERF_OVERLAY_LINES)
        return fail("er_perf_overlay_lines did not fill the requested line count");
    for (int i = 0; i < n; i++)
    {
        if (!lines[i] || lines[i][0] == '\0')
            return fail("er_perf_overlay_lines produced an empty line");
        if (strlen(lines[i]) >= ER_PERF_LINE_MAX)
            return fail("an overlay line overran ER_PERF_LINE_MAX");
        printf("  overlay: %s\n", lines[i]);
    }

    /* A smaller request must be honoured (a host merging its own metrics has limited room). */
    const char* two[2] = {NULL, NULL};
    if (er_perf_overlay_lines(two, 2) != 2 || !two[0] || !two[1])
        return fail("er_perf_overlay_lines ignored max_lines");

    printf("PASS: overlay lines\n");
    return EXIT_SUCCESS;
}

/* Robustness: unbalanced marks and a missing clock must degrade, never corrupt. */
static int check_degrades_safely(int screen)
{
    ERNode* root;
    (void)build_scene(screen, &root);

    /* Marks outside a frame are ignored rather than accumulated into the next one. */
    er_perf_phase_begin(ER_PERF_PHASE_JS);
    er_perf_phase_end(ER_PERF_PHASE_JS);
    er_perf_phase_end(ER_PERF_PHASE_RASTER);
    er_perf_frame_end(); /* no frame open */

    /* A phase left open at frame end is closed there, not leaked into the next frame. */
    er_perf_frame_begin();
    er_perf_phase_begin(ER_PERF_PHASE_JS);
    er_commit();
    er_perf_frame_end();
    ERPerfFrame f;
    er_perf_get_last(&f);
    if (f.phase_us[ER_PERF_PHASE_JS] == 0U)
        return fail("a phase left open at frame end was dropped instead of closed");

    frame();
    er_perf_get_last(&f);
    if (f.phase_us[ER_PERF_PHASE_JS] != g_step)
        return fail("an unbalanced phase leaked into the following frame");

    /* No clock: timings go to zero, counters keep working. */
    er_perf_set_clock(NULL);
    frame();
    er_perf_get_last(&f);
    if (f.frame_us != 0U || f.phase_us[ER_PERF_PHASE_RASTER] != 0U)
        return fail("timings are non-zero without a clock installed");
    if (f.vector_slots_total == 0U)
        return fail("counters stopped working without a clock");
    er_perf_set_clock(fake_now_us);

    er_node_destroy(root);
    printf("PASS: degrades safely (unbalanced marks, no clock)\n");
    return EXIT_SUCCESS;
}

int main(void)
{
#if !ER_PERF_STATS
    /* Compiled out: the contract is that every entry point is callable and reports "nothing". */
    er_perf_set_clock(fake_now_us);
    er_perf_frame_begin();
    er_perf_phase_begin(ER_PERF_PHASE_JS);
    er_perf_phase_end(ER_PERF_PHASE_JS);
    er_perf_frame_end();
    ERPerfFrame f;
    if (er_perf_get_last(&f) || er_perf_get_worst(&f))
        return fail("ER_PERF_STATS=0 reported a frame");
    if (f.frame_us != 0U)
        return fail("ER_PERF_STATS=0 did not zero the out struct");
    const char* lines[ER_PERF_OVERLAY_LINES];
    if (er_perf_overlay_lines(lines, ER_PERF_OVERLAY_LINES) != 0)
        return fail("ER_PERF_STATS=0 produced overlay lines");
    printf("PASS: ER_PERF_STATS=0 — instrumentation compiled out\n");
    return EXIT_SUCCESS;
#else
    EmbeddedRenderBackend be = {0};
    be.fill_rect = noop_fill;
    embedded_renderer_set_backend(&be);
    er_perf_set_clock(fake_now_us);

    const int screen = 200;

    int rc = check_phase_split(screen);
    if (rc != EXIT_SUCCESS)
        return rc;
    er_reset();
    er_perf_reset();

    rc = check_worst_frame_is_retained(screen);
    if (rc != EXIT_SUCCESS)
        return rc;
    er_reset();
    er_perf_reset();

    rc = check_counters(screen);
    if (rc != EXIT_SUCCESS)
        return rc;

    rc = check_overlay_lines();
    if (rc != EXIT_SUCCESS)
        return rc;
    er_reset();
    er_perf_reset();

    rc = check_degrades_safely(screen);
    if (rc != EXIT_SUCCESS)
        return rc;

    return EXIT_SUCCESS;
#endif
}
