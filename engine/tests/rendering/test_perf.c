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
 * engine time the right thing?" into an exact-equality assertion wherever the phase contains no
 * further instrumented reads (JS, layout, present). The raster phase DOES contain further reads —
 * its own sub-split plus one pair per backend blit — so raster assertions are entered-or-not
 * (0 vs > 0) and structural (the sub-steps sum to at most the phase):
 *
 *   - a frame that ran layout reports layout_us == step; a static frame reports exactly 0 (the phase
 *     was never entered — the fast path really was taken),
 *   - the four phases plus other_us always reconstruct frame_us,
 *   - the raster split attributes the phase to pre-pass / composite / blit / sweep, and blit_px
 *     counts exactly the pixels handed to the backend,
 *   - the JS split attributes the phase to dispatch / reconcile / marshalling, with the commit the
 *     pump drove taken back OUT of the JS phase so it is not counted there and in layout+raster too,
 *   - the worst frame is retained with its whole split, so a spike minutes ago is still attributable,
 *   - the dirty-rect / vector-slot / image-slot counters track the scene.
 */

#include "er_perf.h"
#include "er_scene.h"
#include "native_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPACITY_OPAQUE 255U

/*----------------------------------------------------------------------------------------------------------------------
 - Fake clock: every read advances by g_step, so one begin/end pair == exactly one step
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t g_now_us = 1000000U; /* start well off zero so a bug can't pass by reading 0 */
static uint32_t g_step = 1000U;

static uint32_t fake_now_us(void)
{
    const uint32_t now_us = g_now_us;
    g_now_us += g_step;
    return now_us;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static void noop_fill(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)argb;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
}

static ERProps props_default(void)
{
    ERProps props = {0};
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    props.width = props.height = ER_LAYOUT_AUTO;
    props.min_width = props.max_width = ER_LAYOUT_AUTO;
    props.min_height = props.max_height = ER_LAYOUT_AUTO;
    props.padding = props.padding_left = props.padding_top = ER_LAYOUT_AUTO;
    props.padding_right = props.padding_bottom = ER_LAYOUT_AUTO;
    props.margin = props.margin_left = props.margin_top = ER_LAYOUT_AUTO;
    props.margin_right = props.margin_bottom = ER_LAYOUT_AUTO;
    props.gap = props.row_gap = props.column_gap = ER_LAYOUT_AUTO;
    props.flex_basis = ER_LAYOUT_AUTO;
    props.opacity = OPACITY_OPAQUE;
    return props;
}

static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
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
    ERProps root_props = props_default();
    root_props.width = screen;
    root_props.height = screen;
    root_props.background_color = 0xFFFFFFFFU;
    er_node_set_props(root, &root_props);

    ERNode* box = er_node_create(ER_NODE_VIEW);
    ERProps box_props = props_default();
    box_props.width = 30;
    box_props.height = 30;
    box_props.margin_left = 20;
    box_props.margin_top = 20;
    box_props.background_color = 0xFF3366FFU;
    er_node_set_props(box, &box_props);

    er_tree_append_child(root, box);
    er_tree_set_root(root);
    *out_root = root;
    return box;
}

/* Recolours the box, which dirties it without moving it (damage == the box rect, no relayout). */
static void recolour(ERNode* box, uint32_t argb)
{
    ERProps box_props = props_default();
    box_props.width = 30;
    box_props.height = 30;
    box_props.margin_left = 20;
    box_props.margin_top = 20;
    box_props.background_color = argb;
    er_node_set_props(box, &box_props);
}

/* The bounds-only refresh path: a transform appearing mid-animation runs no solver, but it does walk
 * every node, so it has to be attributed — and it has to happen ONCE, not on every animated frame. */
static int check_bounds_refresh_is_timed(int screen)
{
    ERNode* root;
    ERNode* box = build_scene(screen, &root);

    frame(); /* mount frame: layout runs */

    ERAnimValueHandle value = er_anim_value_create(0.0f);
    er_anim_value_bind(value, box, ER_PROP_TRANSLATE_X);
    ERAnimConfig config;
    memset(&config, 0, sizeof(config));
    config.type = ER_ANIM_TIMING;
    config.duration_ms = 320U;
    config.easing = ER_EASE_LINEAR;
    er_anim_value_animate(value, 60.0f, &config);

    const uint32_t passes = er_layout_pass_count();

    /* First animated frame: the node gains a transform, so the cached prune bounds are refreshed. */
    embedded_renderer_tick(16U);
    frame();

    ERPerfFrame perf_frame;
    if (!er_perf_get_last(&perf_frame))
        return fail("no frame recorded for the first animated frame");
    if (perf_frame.phase_us[ER_PERF_PHASE_LAYOUT] != g_step)
        return fail("the subtree-bounds refresh was not attributed to the layout phase");
    if (er_layout_pass_count() != passes)
        return fail("the subtree-bounds refresh ran the layout solver");

    uint32_t sum = perf_frame.other_us;
    for (int phase_index = 0; phase_index < (int)ER_PERF_PHASE_COUNT; phase_index++)
        sum += perf_frame.phase_us[phase_index];
    if (sum != perf_frame.frame_us)
        return fail("phases + other_us do not add up on a bounds-refresh frame");

    /* Every later frame of the same animation: the transform is already there, nothing to refresh.
     * Exactly 0 is the assertion worth making — the refresh is per toggle, not per frame. */
    embedded_renderer_tick(16U);
    frame();
    if (!er_perf_get_last(&perf_frame))
        return fail("no frame recorded for the second animated frame");
    if (perf_frame.phase_us[ER_PERF_PHASE_LAYOUT] != 0U)
        return fail("a mid-animation frame refreshed the subtree bounds again");
    if (er_layout_pass_count() != passes)
        return fail("a mid-animation frame ran the layout solver");

    /* Left for er_reset() to tear down: the box is still animating, and destroying the root alone
     * would orphan it mid-animation. */
    (void)root;
    return EXIT_SUCCESS;
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

    ERPerfFrame perf_frame;
    if (!er_perf_get_last(&perf_frame))
        return fail("no frame recorded after the first commit");
    if (perf_frame.phase_us[ER_PERF_PHASE_LAYOUT] != g_step)
        return fail("mount frame did not time the layout pass");
    if (perf_frame.phase_us[ER_PERF_PHASE_RASTER] == 0U)
        return fail("mount frame did not time the raster pass");
    if (perf_frame.phase_us[ER_PERF_PHASE_JS] != g_step || perf_frame.phase_us[ER_PERF_PHASE_PRESENT] != g_step)
        return fail("host-marked JS/present phases were not timed");

    uint32_t sum = perf_frame.other_us;
    for (int phase_index = 0; phase_index < (int)ER_PERF_PHASE_COUNT; phase_index++)
        sum += perf_frame.phase_us[phase_index];
    if (sum != perf_frame.frame_us)
        return fail("phases + other_us do not add up to frame_us");

    /* Idle frame: nothing changed, so the layout fast path skips the solver entirely and the phase is
     * never entered. Exactly 0 — not "small" — is the assertion worth making. */
    frame();
    if (!er_perf_get_last(&perf_frame))
        return fail("no frame recorded for the idle frame");
    if (perf_frame.phase_us[ER_PERF_PHASE_LAYOUT] != 0U)
        return fail("idle frame reported layout time although the solver never ran");
    if (perf_frame.phase_us[ER_PERF_PHASE_RASTER] == 0U)
        return fail("idle frame did not time the raster phase (the damage pre-pass still runs)");

    /* A recolour changes no layout input, yet er_node_set_props conservatively marks layout dirty, so
     * the solver DOES run again. Worth pinning: this is precisely the kind of cost the split is here
     * to make visible — the frame is not "just a repaint" and the numbers must not pretend it is. */
    recolour(box, 0xFF22CC88U);
    frame();
    if (!er_perf_get_last(&perf_frame))
        return fail("no frame recorded for the recolour frame");
    if (perf_frame.phase_us[ER_PERF_PHASE_LAYOUT] != g_step)
        return fail("a setProps-driven relayout was not attributed to the layout phase");

    er_node_destroy(root);
    printf("PASS: phase split — layout timed only when it runs, phases account for the frame\n");
    return EXIT_SUCCESS;
}

/* The raster phase's own split: pre-pass / composite / blit / sweep, plus the backend pixel count.
 * This is the instrumentation that turns "raster is 31 ms" into a named culprit — the node-pool
 * walk, the compositing, or the framebuffer writes — so what it must get right is attribution:
 * every sub-step that ran shows up, none of them over-claims the phase, and blit_px counts exactly
 * the pixels the backend was handed (not the damage, not the walk). */
static int check_raster_split(int screen)
{
    ERNode* root;
    ERNode* box = build_scene(screen, &root);

    frame(); /* mount: full-screen repaint through the backend */
    ERPerfFrame perf_frame;
    if (!er_perf_get_last(&perf_frame))
        return fail("no frame recorded after the mount commit");
    if (perf_frame.raster_us[ER_PERF_RASTER_PREPASS] == 0U)
        return fail("mount frame did not time the damage pre-pass");
    if (perf_frame.raster_us[ER_PERF_RASTER_RENDER] == 0U)
        return fail("mount frame did not time the composite passes");
    if (perf_frame.raster_us[ER_PERF_RASTER_BLIT] == 0U)
        return fail("mount frame did not time the backend blits");
    if (perf_frame.raster_us[ER_PERF_RASTER_SWEEP] == 0U)
        return fail("mount frame did not time the dirty-flag sweep");
    /* Disjointness: the buckets must never claim more than the phase they subdivide (RENDER is
     * reported net of BLIT precisely so this holds). */
    uint32_t sub_sum = 0U;
    for (int raster_index = 0; raster_index < (int)ER_PERF_RASTER_COUNT; raster_index++)
        sub_sum += perf_frame.raster_us[raster_index];
    if (sub_sum > perf_frame.phase_us[ER_PERF_PHASE_RASTER])
        return fail("the raster sub-steps sum to more than the raster phase");
    /* The mount paints the root AND the box over it, so the backend saw at least every screen pixel
     * once — blit_px is write traffic, not coverage, and overlap is exactly what it must not hide. */
    if (perf_frame.blit_px < (uint32_t)(screen * screen))
        return fail("the mount frame's blit_px is less than the screen it painted");

    /* Idle: the pre-pass and the sweep still walk the pool (their cost is the per-commit floor this
     * split exists to expose), but not one pixel may reach the backend. */
    frame();
    er_perf_get_last(&perf_frame);
    if (perf_frame.raster_us[ER_PERF_RASTER_PREPASS] == 0U)
        return fail("idle frame did not time the damage pre-pass (the pool walk still runs)");
    if (perf_frame.raster_us[ER_PERF_RASTER_SWEEP] == 0U)
        return fail("idle frame did not time the dirty-flag sweep");
    if (perf_frame.raster_us[ER_PERF_RASTER_BLIT] != 0U || perf_frame.blit_px != 0U)
        return fail("an idle frame reported backend blit work");

    /* One recoloured 30x30 box. The composite restores the parent's background over the padded
     * damage rect and then paints the box over it, and the backend sees BOTH writes: blit_px is
     * deliberately the write-traffic number, not the coverage number, so here it EXCEEDS dirty_px —
     * the write amplification the counter exists to expose (with the fake clock, blit time counts
     * the backend calls exactly: one step each). */
    recolour(box, 0xFF22CC88U);
    frame();
    er_perf_get_last(&perf_frame);
    if (perf_frame.raster_us[ER_PERF_RASTER_BLIT] != 2U * g_step)
        return fail("the recolour frame's blit time is not exactly two backend calls (bg + box)");
    if (perf_frame.blit_px < 900U)
        return fail("the recolour frame's blit_px does not cover the recoloured box");
    if (perf_frame.blit_px <= perf_frame.dirty_px)
        return fail("blit_px hides the bg-restore write under the box (no amplification reported)");
    if (perf_frame.blit_px >= (uint32_t)(screen * screen))
        return fail("a single recoloured box pushed a whole screen of pixels");

    er_node_destroy(root);
    printf("PASS: raster split — pre-pass/composite/blit/sweep attributed, blit_px counts write traffic\n");
    return EXIT_SUCCESS;
}

/* The JS phase's own split, as the QuickJS bridge marks it: a batched render (RECONCILE) with the
 * frame's callbacks (DISPATCH) and the reconciler's prop writes (MARSHAL) nested inside it, then the
 * commit the reconciler asks for on the way out (COMMIT).
 *
 * Two things have to hold. The buckets are EXCLUSIVE — a nested mark stops its parent's clock rather
 * than running two at once — so the split can be read as "where the JS went" instead of a set of
 * overlapping spans. And the nested commit is removed from the JS phase, which is what stops it
 * being charged to JS and to layout+raster at the same time and pushing the phase total past the
 * frame. */
static int check_js_split(int screen)
{
    ERNode* root;
    ERNode* box = build_scene(screen, &root);
    frame(); /* mount, so the frame under test is an ordinary repaint */

    /* One bridge-shaped frame. Timed with the fake clock, the exact step counts are predictable:
     * DISPATCH and MARSHAL are one begin/end pair each; RECONCILE keeps the three stretches of the
     * batched call that its children did not claim. */
    recolour(box, 0xFF4488CCU);
    er_perf_frame_begin();
    er_perf_phase_begin(ER_PERF_PHASE_JS);
    er_perf_js_begin(ER_PERF_JS_RECONCILE); /* batchedUpdates(...) */
    er_perf_js_begin(ER_PERF_JS_DISPATCH);  /* the pump body: touch flush, timers, microtasks */
    er_perf_js_end(ER_PERF_JS_DISPATCH);
    er_perf_js_begin(ER_PERF_JS_MARSHAL); /* the mutation pass: setProps & friends */
    er_perf_js_end(ER_PERF_JS_MARSHAL);
    er_perf_js_end(ER_PERF_JS_RECONCILE);
    er_perf_js_begin(ER_PERF_JS_COMMIT); /* resetAfterCommit -> NativeUI.commit(), as the batch closes */
    er_commit();
    er_perf_js_end(ER_PERF_JS_COMMIT);
    er_perf_phase_end(ER_PERF_PHASE_JS);
    er_perf_frame_end();

    ERPerfFrame perf_frame;
    if (!er_perf_get_last(&perf_frame))
        return fail("no frame recorded for the bridge-shaped frame");
    if (perf_frame.js_us[ER_PERF_JS_DISPATCH] != g_step)
        return fail("the dispatch span was not timed as exactly one step");
    if (perf_frame.js_us[ER_PERF_JS_MARSHAL] != g_step)
        return fail("the marshalling span was not timed as exactly one step");
    if (perf_frame.js_us[ER_PERF_JS_RECONCILE] != 3U * g_step)
        return fail("reconcile did not keep exactly the time its nested spans left it");

    /* The commit ran inside the pump, so it is layout + raster (plus er_commit's own bookkeeping). */
    if (perf_frame.js_us[ER_PERF_JS_COMMIT]
        < perf_frame.phase_us[ER_PERF_PHASE_LAYOUT] + perf_frame.phase_us[ER_PERF_PHASE_RASTER])
        return fail("the pump-driven commit measured less than the layout+raster it contains");

    /* The three in-JS buckets fit inside the phase; the frame still adds up. */
    if (perf_frame.js_us[ER_PERF_JS_DISPATCH] + perf_frame.js_us[ER_PERF_JS_RECONCILE]
            + perf_frame.js_us[ER_PERF_JS_MARSHAL]
        > perf_frame.phase_us[ER_PERF_PHASE_JS])
        return fail("the JS sub-steps sum to more than the JS phase");
    uint32_t sum = perf_frame.other_us;
    for (int phase_index = 0; phase_index < (int)ER_PERF_PHASE_COUNT; phase_index++)
        sum += perf_frame.phase_us[phase_index];
    if (sum != perf_frame.frame_us)
        return fail("phases + other_us do not add up on a frame that committed from inside the pump");
    if (perf_frame.other_us == 0U)
        return fail("the phases still claim the whole frame (the nested commit was not taken out of JS)");

    /* And the bug this exists to close: leave the commit in the JS phase and the four phases claim
     * more time than the frame lasted, which is what used to pin other_us at 0. */
    if (perf_frame.phase_us[ER_PERF_PHASE_JS] + perf_frame.js_us[ER_PERF_JS_COMMIT]
            + perf_frame.phase_us[ER_PERF_PHASE_LAYOUT] + perf_frame.phase_us[ER_PERF_PHASE_RASTER]
        <= perf_frame.frame_us)
        return fail("setup: this frame does not reproduce the double count, so the fix is untested");

    /* A commit the HOST runs, outside the pump, is nobody's JS: the bucket stays empty and the JS
     * phase keeps every microsecond the host measured. */
    recolour(box, 0xFF991144U);
    frame();
    er_perf_get_last(&perf_frame);
    if (perf_frame.js_us[ER_PERF_JS_COMMIT] != 0U)
        return fail("a host-driven commit was attributed to the JS phase");
    if (perf_frame.phase_us[ER_PERF_PHASE_JS] != g_step)
        return fail("a host-driven commit was subtracted from the JS phase");

    er_node_destroy(root);
    printf("PASS: JS split — dispatch/reconcile/marshal exclusive, pump-driven commit not double-counted\n");
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
    for (int frame_index = 0; frame_index < 20; frame_index++)
    {
        recolour(box, (uint32_t)(0xFF000000U | (uint32_t)(frame_index * 0x010203)));
        frame();
    }

    ERPerfFrame still;
    if (!er_perf_get_worst(&still))
        return fail("worst frame lost after subsequent frames");
    if (still.index != spike_index)
        return fail("a later, faster frame displaced the retained peak");
    if (still.phase_us[ER_PERF_PHASE_JS] != spike.phase_us[ER_PERF_PHASE_JS])
        return fail("the retained peak's split changed");
    if (still.dirty_px != spike.dirty_px || still.dirty_w != spike.dirty_w || still.dirty_h != spike.dirty_h)
        return fail("the retained peak's repaint region changed (PKDRT would show the wrong frame)");

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
    ERPerfFrame perf_frame;
    er_perf_get_last(&perf_frame);
    if (perf_frame.dirty_px != (uint32_t)(screen * screen))
        return fail("the mount frame's dirty area is not the whole screen");

    frame(); /* idle: nothing repainted */
    er_perf_get_last(&perf_frame);
    if (perf_frame.dirty_w != 0 || perf_frame.dirty_h != 0 || perf_frame.dirty_px != 0U)
        return fail("an idle frame reported a non-empty dirty rect");

    /* A recolour repaints the box and nothing else, so the area collapses by orders of magnitude —
     * exactly the signal that separates "one small node changed" from "the whole screen redrew". */
    recolour(box, 0xFF9900FFU);
    frame();
    er_perf_get_last(&perf_frame);
    if (perf_frame.dirty_px == 0U)
        return fail("the recolour frame reported no repaint");
    if (perf_frame.dirty_px >= (uint32_t)(screen * screen))
        return fail("a single recoloured node repainted the whole screen");
    if (perf_frame.dirty_w < 30 || perf_frame.dirty_h < 30)
        return fail("the dirty rect does not cover the changed node");

    /* Disjoint damage: recolour the box AND a second box in the far corner in one frame. dirty_w/h
     * grow to the union bbox (spanning both corners) while dirty_px stays the SUM of the two small
     * rects — strictly less than the bbox area. This is what lets the overlay show "two small
     * changes" instead of "most of the screen". */
    ERNode* far_box = er_node_create(ER_NODE_VIEW);
    ERProps far_box_props = props_default();
    far_box_props.width = 30;
    far_box_props.height = 30;
    far_box_props.margin_left = (int16_t)(screen - 40);
    far_box_props.margin_top = (int16_t)(screen - 90); /* below the first box (bottom edge y=50): lands at screen-40 */
    far_box_props.background_color = 0xFF884422U;
    er_node_set_props(far_box, &far_box_props);
    er_tree_append_child(root, far_box);
    frame(); /* paint it */
    frame(); /* settle */
    recolour(box, 0xFF66AA22U);
    far_box_props.background_color = 0xFF2266AAU;
    er_node_set_props(far_box, &far_box_props);
    frame();
    er_perf_get_last(&perf_frame);
    if (perf_frame.dirty_px == 0U)
        return fail("the two-corner frame reported no repaint");
    if (perf_frame.dirty_w < 100 || perf_frame.dirty_h < 100)
        return fail("the two-corner bbox does not span both corners");
    if (perf_frame.dirty_px >= (uint32_t)(perf_frame.dirty_w * perf_frame.dirty_h))
        return fail("dirty_px is not the sum of disjoint rects (equals the bbox area)");
    er_node_destroy(far_box);
    frame(); /* consume the removal damage so later assertions see a settled scene */
    frame();

    /* Resource pools: start empty, then fill as the scene registers assets. */
    if (perf_frame.vector_slots_used != 0U)
        return fail("vector slots reported in use before any vector node exists");
    if (perf_frame.vector_slots_total == 0U || perf_frame.image_slots_total == 0U)
        return fail("pool sizes reported as zero");

    er_image_load("perf-test-a", pixels, 2, 2);
    er_image_load("perf-test-b", pixels, 2, 2);

    ERNode* vector = er_node_create(ER_NODE_VECTOR);
    ERProps vector_props = props_default();
    vector_props.width = 40;
    vector_props.height = 40;
    er_node_set_props(vector, &vector_props);
    /* One filled rectangle: MOVE, LINE x3, CLOSE — enough to claim a storage slot. */
    const float tape[] = {(float)ER_VOP_MOVE,
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
    er_node_set_vector_ops(vector, tape, (int)(sizeof(tape) / sizeof(tape[0])), &paint, 1, NULL, 0);
    er_tree_append_child(root, vector);

    frame();
    er_perf_get_last(&perf_frame);
    if (perf_frame.vector_slots_used != 1U)
        return fail("the vector node did not show up in the vector-slot counter");
    if (perf_frame.image_slots_used != 2U)
        return fail("registered images did not show up in the image-slot counter");
    if (perf_frame.vector_slots_used > perf_frame.vector_slots_total
        || perf_frame.image_slots_used > perf_frame.image_slots_total)
        return fail("a slot counter exceeded its pool size");
    if (perf_frame.vector_slots_overflow)
        return fail("a scene well inside the vector pool reported an overflow");

    printf("PASS: counters — dirty %dx%d (%u px), VEC %u/%u, IMG %u/%u\n",
           (int)perf_frame.dirty_w,
           (int)perf_frame.dirty_h,
           (unsigned)perf_frame.dirty_px,
           (unsigned)perf_frame.vector_slots_used,
           (unsigned)perf_frame.vector_slots_total,
           (unsigned)perf_frame.image_slots_used,
           (unsigned)perf_frame.image_slots_total);

    er_node_destroy(root);
    return EXIT_SUCCESS;
}

/* The overlay lines: non-empty, null-terminated, short enough for a 240 px panel — and the region
 * line reports the PEAK frame, not the (nearly always empty) last frame. */
static int check_overlay_lines(void)
{
    const char* lines[ER_PERF_OVERLAY_LINES];
    memset(lines, 0, sizeof(lines));

    /* An idle frame first: the LAST frame now repainted nothing, while the retained peak (the mount
     * frame of the preceding test) covered the whole screen. The region line must show the peak. */
    frame();
    ERPerfFrame last, worst;
    er_perf_get_last(&last);
    er_perf_get_worst(&worst);
    if (last.dirty_px != 0U || worst.dirty_px == 0U)
        return fail("setup: expected an empty last frame under a non-empty retained peak");

    const int line_count = er_perf_overlay_lines(lines, ER_PERF_OVERLAY_LINES);
    if (line_count != ER_PERF_OVERLAY_LINES)
        return fail("er_perf_overlay_lines did not fill the requested line count");
    for (int line_index = 0; line_index < line_count; line_index++)
    {
        if (!lines[line_index] || lines[line_index][0] == '\0')
            return fail("er_perf_overlay_lines produced an empty line");
        if (strlen(lines[line_index]) >= ER_PERF_LINE_MAX)
            return fail("an overlay line overran ER_PERF_LINE_MAX");
        printf("  overlay: %s\n", lines[line_index]);
    }

    /* The region line: labelled as the peak's, showing the peak's geometry — not the last frame's
     * zeros, which would make the line read 0 on almost every refresh. */
    char want[ER_PERF_LINE_MAX];
    snprintf(want, sizeof(want), "PKDRT %dx%d", (int)worst.dirty_w, (int)worst.dirty_h);
    if (strncmp(lines[3], want, strlen(want)) != 0)
        return fail("the region line does not show the PEAK frame's repaint region");

    /* The slot line carries the pool-exhaustion marker only when it happened — a false "!FULL" would
     * send someone raising a macro that was never the problem. */
    if (!last.vector_slots_overflow && strstr(lines[4], "!FULL") != NULL)
        return fail("the slot line shows !FULL without a vector-pool overflow");

    /* The raster-split lines: the last frame's (live during a steady drag) and the peak's. */
    if (strncmp(lines[5], "RST P", 5) != 0)
        return fail("the last-frame raster-split line is missing");
    if (strncmp(lines[6], "PKR P", 5) != 0)
        return fail("the peak raster-split line is missing");
    if (strchr(lines[5], 'W') == NULL || strchr(lines[6], 'W') == NULL)
        return fail("a raster-split line dropped the blit-pixel field");

    /* The JS-split lines: same pairing, last frame then peak. */
    if (strncmp(lines[7], "JSS D", 5) != 0)
        return fail("the last-frame JS-split line is missing");
    if (strncmp(lines[8], "PKJ D", 5) != 0)
        return fail("the peak JS-split line is missing");

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
    ERNode* box = build_scene(screen, &root);

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
    ERPerfFrame perf_frame;
    er_perf_get_last(&perf_frame);
    if (perf_frame.phase_us[ER_PERF_PHASE_JS] == 0U)
        return fail("a phase left open at frame end was dropped instead of closed");

    frame();
    er_perf_get_last(&perf_frame);
    if (perf_frame.phase_us[ER_PERF_PHASE_JS] != g_step)
        return fail("an unbalanced phase leaked into the following frame");

    /* The JS stack: a stray end, a crossed end, and a span left open at frame end. None of them may
     * move time into a bucket that did not run — the split is read as an attribution, so a wrong
     * number here is worse than a missing one. */
    er_perf_js_end(ER_PERF_JS_MARSHAL); /* outside a frame */
    er_perf_frame_begin();
    er_perf_phase_begin(ER_PERF_PHASE_JS);
    er_perf_js_end(ER_PERF_JS_DISPATCH);    /* never begun */
    er_perf_js_begin(ER_PERF_JS_RECONCILE); /* left open: frame_end must close it */
    er_perf_js_end(ER_PERF_JS_MARSHAL);     /* crossed with the open reconcile: ignored */
    er_perf_phase_end(ER_PERF_PHASE_JS);
    er_perf_frame_end();
    er_perf_get_last(&perf_frame);
    if (perf_frame.js_us[ER_PERF_JS_DISPATCH] != 0U || perf_frame.js_us[ER_PERF_JS_MARSHAL] != 0U)
        return fail("an unbalanced JS mark charged time to a bucket that never ran");
    if (perf_frame.js_us[ER_PERF_JS_RECONCILE] == 0U)
        return fail("a JS span left open at frame end was dropped instead of closed");

    frame();
    er_perf_get_last(&perf_frame);
    if (perf_frame.js_us[ER_PERF_JS_RECONCILE] != 0U)
        return fail("an unbalanced JS span leaked into the following frame");

    /* No clock: timings go to zero, counters keep working — including blit_px, which must keep
     * counting pixels while the blit TIME correctly collapses to zero. */
    er_perf_set_clock(NULL);
    recolour(box, 0xFF112233U); /* make the clockless frame actually paint something */
    frame();
    er_perf_get_last(&perf_frame);
    if (perf_frame.frame_us != 0U || perf_frame.phase_us[ER_PERF_PHASE_RASTER] != 0U)
        return fail("timings are non-zero without a clock installed");
    if (perf_frame.raster_us[ER_PERF_RASTER_BLIT] != 0U)
        return fail("blit time is non-zero without a clock installed");
    if (perf_frame.blit_px == 0U)
        return fail("blit_px stopped counting without a clock");
    if (perf_frame.vector_slots_total == 0U)
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
    er_perf_raster_begin(ER_PERF_RASTER_BLIT);
    er_perf_raster_end(ER_PERF_RASTER_BLIT);
    er_perf_js_begin(ER_PERF_JS_MARSHAL);
    er_perf_js_end(ER_PERF_JS_MARSHAL);
    er_perf_note_blit(123U, 456U);
    er_perf_frame_end();
    if (er_perf_now_us() != 0U)
        return fail("ER_PERF_STATS=0 sampled the clock");
    ERPerfFrame perf_frame;
    if (er_perf_get_last(&perf_frame) || er_perf_get_worst(&perf_frame))
        return fail("ER_PERF_STATS=0 reported a frame");
    if (perf_frame.frame_us != 0U || perf_frame.raster_us[ER_PERF_RASTER_BLIT] != 0U || perf_frame.blit_px != 0U)
        return fail("ER_PERF_STATS=0 did not zero the out struct");
    if (perf_frame.js_us[ER_PERF_JS_MARSHAL] != 0U)
        return fail("ER_PERF_STATS=0 recorded a JS sub-step");
    const char* lines[ER_PERF_OVERLAY_LINES];
    if (er_perf_overlay_lines(lines, ER_PERF_OVERLAY_LINES) != 0)
        return fail("ER_PERF_STATS=0 produced overlay lines");
    printf("PASS: ER_PERF_STATS=0 — instrumentation compiled out\n");
    return EXIT_SUCCESS;
#else
    EmbeddedRenderBackend backend = {0};
    backend.fill_rect = noop_fill;
    embedded_renderer_set_backend(&backend);
    er_perf_set_clock(fake_now_us);

    const int screen = 200;

    int result = check_phase_split(screen);
    if (result != EXIT_SUCCESS)
        return result;
    er_reset();
    er_perf_reset();

    result = check_bounds_refresh_is_timed(screen);
    if (result != EXIT_SUCCESS)
        return result;
    er_reset();
    er_perf_reset();

    result = check_raster_split(screen);
    if (result != EXIT_SUCCESS)
        return result;
    er_reset();
    er_perf_reset();

    result = check_js_split(screen);
    if (result != EXIT_SUCCESS)
        return result;
    er_reset();
    er_perf_reset();

    result = check_worst_frame_is_retained(screen);
    if (result != EXIT_SUCCESS)
        return result;
    er_reset();
    er_perf_reset();

    result = check_counters(screen);
    if (result != EXIT_SUCCESS)
        return result;

    result = check_overlay_lines();
    if (result != EXIT_SUCCESS)
        return result;
    er_reset();
    er_perf_reset();

    result = check_degrades_safely(screen);
    if (result != EXIT_SUCCESS)
        return result;

    return EXIT_SUCCESS;
#endif
}
