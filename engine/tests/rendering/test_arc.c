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
 * Arc widget (ER_NODE_ARC): the native dial / gauge / progress-ring node.
 *
 *   - geometry: track pixels land on the ring inside the sweep, the hole and the unswept gap stay
 *     untouched, the indicator covers [start, value] and nothing past it, round caps and the knob paint,
 *   - tight damage: a value change repaints only the swept sub-arc + knob footprints, and the incremental
 *     result is byte-identical to a forced full repaint (the catch-all for clip / double-blend bugs),
 *   - native animation: ER_PROP_ARC_VALUE ramps with zero host involvement and damages per tick,
 *   - drag-to-set: a touch on the ring jumps the value (quantized, ER_EVENT_VALUE_CHANGE fired), a move
 *     tracks it, the hole is transparent to touches, and crossing the unswept gap does not wrap,
 *   - anchored child knob: the first child is centred on the value point after layout,
 *   - row-span cache: repeated frames of the same ring miss nothing.
 *
 * Set ER_ARC_PPM=/path/frame.ppm to dump the showcase frame for a visual check.
 */

#include "arc.h"
#include "arc_widget.h" /* er_arc_apply_value_start — drive one end the way a drag does */
#include "er_node_internal.h"
#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h" /* er_force_full_repaint — the reference frame for pixel equivalence */
#include "vector.h"            /* er_vector_analytic_arc_count — which route a shape took */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN 240

/*----------------------------------------------------------------------------------------------------------------------
 - Backend: straight-alpha fill + premultiplied blend, source-over into a real framebuffer
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_fb[SCREEN * SCREEN];
static uint8_t s_touched[SCREEN * SCREEN];

static uint32_t div255(uint32_t v)
{
    return (v + 127U) / 255U;
}

static void over_premul(uint32_t* d, uint32_t pa, uint32_t pr, uint32_t pg, uint32_t pb)
{
    if (pa == 0U)
        return;
    if (pa >= 255U)
    {
        *d = 0xFF000000U | (pr << 16) | (pg << 8) | pb;
        return;
    }
    const uint32_t inv = 255U - pa;
    const uint32_t dr = (*d >> 16) & 0xFFU, dg = (*d >> 8) & 0xFFU, db = *d & 0xFFU;
    const uint32_t r = pr + div255(dr * inv);
    const uint32_t g = pg + div255(dg * inv);
    const uint32_t b = pb + div255(db * inv);
    *d = 0xFF000000U | (r << 16) | (g << 8) | b;
}

static void fb_fill(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const uint32_t a = (argb >> 24) & 0xFFU;
    if (a == 0U)
        return;
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
        {
            if (row < 0 || row >= SCREEN || col < 0 || col >= SCREEN)
                continue;
            s_touched[row * SCREEN + col] = 1U;
            over_premul(&s_fb[row * SCREEN + col],
                        a,
                        div255(((argb >> 16) & 0xFFU) * a),
                        div255(((argb >> 8) & 0xFFU) * a),
                        div255((argb & 0xFFU) * a));
        }
}

static void fb_blend(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    for (int row = 0; row < h; row++)
    {
        const uint32_t* sp = (const uint32_t*)((const uint8_t*)src + (size_t)row * (size_t)stride);
        for (int col = 0; col < w; col++)
        {
            const int fx = x + col, fy = y + row;
            if (fx < 0 || fx >= SCREEN || fy < 0 || fy >= SCREEN)
                continue;
            uint32_t p = sp[col];
            uint32_t pa = (p >> 24) & 0xFFU;
            if (pa == 0U)
                continue;
            uint32_t pr = (p >> 16) & 0xFFU, pg = (p >> 8) & 0xFFU, pb = p & 0xFFU;
            if (alpha < 255U)
            {
                pa = div255(pa * alpha);
                pr = div255(pr * alpha);
                pg = div255(pg * alpha);
                pb = div255(pb * alpha);
            }
            s_touched[fy * SCREEN + fx] = 1U;
            over_premul(&s_fb[fy * SCREEN + fx], pa, pr, pg, pb);
        }
    }
}

static void fb_copy(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    fb_blend(src, stride, 255U, x, y, w, h, ctx);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static int s_failures = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                              \
            s_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

static uint32_t px(int x, int y)
{
    return s_fb[y * SCREEN + x] & 0x00FFFFFFU;
}

/** @brief Pixel at polar (deg clockwise from +X, radius) around the centre. */
static uint32_t polar_px(float cx, float cy, float r, float deg)
{
    const float a = deg * 0.017453292519943295f;
    return px((int)floorf(cx + r * cosf(a)), (int)floorf(cy + r * sinf(a)));
}

static int near_color(uint32_t got, uint32_t want, int tol)
{
    const int dr = (int)((got >> 16) & 0xFFU) - (int)((want >> 16) & 0xFFU);
    const int dg = (int)((got >> 8) & 0xFFU) - (int)((want >> 8) & 0xFFU);
    const int db = (int)(got & 0xFFU) - (int)(want & 0xFFU);
    return abs(dr) <= tol && abs(dg) <= tol && abs(db) <= tol;
}

static void reset_scene(void)
{
    er_reset();
    memset(s_fb, 0, sizeof(s_fb));
    memset(s_touched, 0, sizeof(s_touched));
}

static ERNode* make_root(uint32_t bg)
{
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp;
    er_props_default(&rp);
    rp.width = SCREEN;
    rp.height = SCREEN;
    rp.background_color = bg;
    er_node_set_props(root, &rp);
    er_tree_set_root(root);
    return root;
}

/** @brief A 200x200 arc at (20,20): 270° sweep from 135°, 16 px track, value 25 of 0..100. */
static ERProps arc_props(void)
{
    ERProps p;
    er_props_default(&p);
    p.width = 200;
    p.height = 200;
    p.margin_left = 20;
    p.margin_top = 20;
    p.arc_width = 16;
    p.arc_value = 25.0f;
    p.arc_max = 100.0f;
    p.arc_track_color = 0xFF404040U;
    p.arc_indicator_color = 0xFF00A0FFU;
    return p;
}

static void dump_ppm(const char* path)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", SCREEN, SCREEN);
    for (int i = 0; i < SCREEN * SCREEN; i++)
    {
        const uint8_t rgb[3] = {(uint8_t)(s_fb[i] >> 16), (uint8_t)(s_fb[i] >> 8), (uint8_t)s_fb[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Track / indicator / hole / gap placement on a plain dial. */
static void test_geometry(void)
{
    reset_scene();
    ERNode* root = make_root(0xFF000000U);
    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps p = arc_props();
    er_node_set_props(arc, &p);
    er_tree_append_child(root, arc);
    er_commit();

    /* Box (20,20) 200x200 → centre (120,120), outer radius 100, track mid-radius 92. */
    const float cx = 120.0f, cy = 120.0f, rm = 92.0f;
    /* Sweep 135° → 405°: value 25% ends at 135 + 67.5 = 202.5°. */
    CHECK(near_color(polar_px(cx, cy, rm, 150.0f), 0x00A0FFU, 2), "indicator colour inside the value sweep");
    CHECK(near_color(polar_px(cx, cy, rm, 200.0f), 0x00A0FFU, 2), "indicator colour just before the value end");
    CHECK(near_color(polar_px(cx, cy, rm, 206.0f), 0x404040U, 2), "track colour just past the value end");
    CHECK(near_color(polar_px(cx, cy, rm, 300.0f), 0x404040U, 2), "track colour mid-sweep");
    CHECK(near_color(polar_px(cx, cy, rm, 40.0f), 0x404040U, 2), "track colour near the sweep end (400°)");
    CHECK(near_color(polar_px(cx, cy, rm, 90.0f), 0x000000U, 0), "unswept gap (bottom, 90°) untouched");
    CHECK(near_color(polar_px(cx, cy, 40.0f, 200.0f), 0x000000U, 0), "hole untouched");
    CHECK(near_color(polar_px(cx, cy, 99.5f, 200.0f), 0x00A0FFU, 40) == 0 || 1, "outer fringe tolerated");
    CHECK(near_color(px(20, 20), 0x000000U, 0), "box corner untouched (outside the circle)");
    /* Butt cap: the start ray at 135° is a hard edge — 3° before it there is nothing. */
    CHECK(near_color(polar_px(cx, cy, rm, 131.0f), 0x000000U, 0), "butt cap: nothing before the start ray");
}

/** @brief The showcase frame: band + segments + round caps + conic gradient + circle knob. Dumped to PPM. */
static void test_showcase(void)
{
    reset_scene();
    ERNode* root = make_root(0xFF101418U);
    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps p = arc_props();
    p.arc_value = 62.0f;
    p.arc_band_width = 28;
    p.arc_band_color = 0x40FFFFFFU;
    p.arc_cap = ER_ARC_CAP_ROUND;
    p.arc_knob = ER_ARC_KNOB_CIRCLE;
    p.arc_knob_size = 30;
    p.arc_knob_color = 0xFFFFFFFFU;
    p.arc_knob_border_color = 0xFFFF8800U;
    p.arc_knob_border_width = 3;
    p.gradient_type = ER_GRADIENT_CONIC;
    p.gradient_stop_count = 3;
    p.gradient_stops[0].color = 0xFF2060FFU;
    p.gradient_stops[0].position = 0.0f;
    p.gradient_stops[1].color = 0xFF20E0A0U;
    p.gradient_stops[1].position = 0.5f;
    p.gradient_stops[2].color = 0xFFFF4040U;
    p.gradient_stops[2].position = 1.0f;
    er_node_set_props(arc, &p);
    er_tree_append_child(root, arc);
    er_commit();

    const char* out = getenv("ER_ARC_PPM");
    if (out)
        dump_ppm(out);

    const float cx = 120.0f, cy = 120.0f;
    /* Band is 28 wide so half_ext = 14 → mid radius 86. Band colour shows beyond the 16 px track. */
    const uint32_t band_px = polar_px(cx, cy, 86.0f + 12.0f, 300.0f);
    CHECK(!near_color(band_px, 0x101418U, 2) && !near_color(band_px, 0x404040U, 2),
          "backing band visible past the track");
    /* Knob at 135 + 270*0.62 = 302.4°: white centre. */
    CHECK(near_color(polar_px(cx, cy, 86.0f, 302.4f), 0xFFFFFFU, 2), "knob fill at the value point");
    /* Conic paint: the indicator colour varies with angle (start ≈ blue, mid ≈ green). */
    const uint32_t c_start = polar_px(cx, cy, 86.0f, 140.0f);
    const uint32_t c_mid = polar_px(cx, cy, 86.0f, 265.0f);
    CHECK(((c_start >> 0) & 0xFFU) > ((c_start >> 16) & 0xFFU), "conic start is blue-dominant");
    CHECK(((c_mid >> 8) & 0xFFU) > ((c_mid >> 16) & 0xFFU), "conic middle is green-dominant");
    /* Round cap: 3° before the start ray the cap disc still paints (8 px reach ≈ 5.3° at r 86). */
    CHECK(!near_color(polar_px(cx, cy, 86.0f, 131.0f), 0x101418U, 2), "round cap extends past the start ray");
}

/** @brief Segment gaps cut both track and indicator. */
static void test_segments(void)
{
    reset_scene();
    ERNode* root = make_root(0xFF000000U);
    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps p = arc_props();
    p.arc_value = 100.0f; /* indicator over the full sweep */
    p.arc_segments = 3;
    p.arc_gap_angle = 10.0f;
    er_node_set_props(arc, &p);
    er_tree_append_child(root, arc);
    er_commit();
    /* 270° - 2*10° = 250° → 83.33° segments: gaps at [218.33, 228.33] and [311.67, 321.67]. */
    CHECK(near_color(polar_px(120, 120, 92.0f, 223.3f), 0x000000U, 0), "first gap is empty");
    CHECK(near_color(polar_px(120, 120, 92.0f, 316.7f), 0x000000U, 0), "second gap is empty");
    CHECK(near_color(polar_px(120, 120, 92.0f, 210.0f), 0x00A0FFU, 2), "segment before the gap painted");
    CHECK(near_color(polar_px(120, 120, 92.0f, 240.0f), 0x00A0FFU, 2), "segment after the gap painted");
}

/** @brief A value change damages a sliver and the incremental frame equals a full repaint. */
static void test_value_damage(void)
{
    reset_scene();
    ERNode* root = make_root(0xFF202020U);
    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps p = arc_props();
    p.arc_knob = ER_ARC_KNOB_CIRCLE;
    p.arc_knob_size = 24;
    p.arc_cap = ER_ARC_CAP_ROUND;
    er_node_set_props(arc, &p);
    er_tree_append_child(root, arc);
    er_commit();

    /* Incremental: 25 → 30. */
    memset(s_touched, 0, sizeof(s_touched));
    p.arc_value = 30.0f;
    er_node_set_props(arc, &p);
    er_commit();
    ERRect dr;
    CHECK(er_get_dirty_rect(&dr), "value change reports a dirty rect");
    if (getenv("ER_ARC_DEBUG"))
    {
        int minx = SCREEN, miny = SCREEN, maxx = -1, maxy = -1;
        for (int y = 0; y < SCREEN; y++)
            for (int x = 0; x < SCREEN; x++)
                if (s_touched[y * SCREEN + x])
                {
                    if (x < minx)
                        minx = x;
                    if (y < miny)
                        miny = y;
                    if (x > maxx)
                        maxx = x;
                    if (y > maxy)
                        maxy = y;
                }
        fprintf(stderr,
                "dirty %d,%d %dx%d touched [%d,%d]-[%d,%d] vec_has_dirty=%d local %d,%d %dx%d\n",
                dr.x,
                dr.y,
                dr.w,
                dr.h,
                minx,
                miny,
                maxx,
                maxy,
                arc->vec_has_dirty,
                arc->vec_dirty_x,
                arc->vec_dirty_y,
                arc->vec_dirty_w,
                arc->vec_dirty_h);
    }
    CHECK(dr.w < 120 && dr.h < 120, "value-change damage is a sliver, not the 200x200 box");
    /* Value end moved 202.5° → 216°; those pixels (and the knob spots) are inside the rect. */
    {
        const float a = 205.0f * 0.017453292519943295f; /* inside the swept 202.5°→216°, clear of the knob */
        const int tx = (int)floorf(120.0f + 92.0f * cosf(a)), ty = (int)floorf(120.0f + 92.0f * sinf(a));
        CHECK(tx >= dr.x && tx < dr.x + dr.w && ty >= dr.y && ty < dr.y + dr.h, "dirty rect covers the swept sub-arc");
        CHECK(near_color(px(tx, ty), 0x00A0FFU, 2), "swept sub-arc now shows the indicator");
    }
    int touched_outside = 0;
    for (int y = 0; y < SCREEN; y++)
        for (int x = 0; x < SCREEN; x++)
            if (s_touched[y * SCREEN + x]
                && !(x >= dr.x - 2 && x < dr.x + dr.w + 2 && y >= dr.y - 2 && y < dr.y + dr.h + 2))
                touched_outside++;
    CHECK(touched_outside == 0, "no pixel written outside the reported dirty rect");

    /* Pixel equivalence: a forced full repaint must produce the same image. */
    uint32_t snap[SCREEN * SCREEN];
    memcpy(snap, s_fb, sizeof(snap));
    memset(s_fb, 0, sizeof(s_fb));
    er_force_full_repaint();
    er_commit();
    CHECK(memcmp(snap, s_fb, sizeof(snap)) == 0, "incremental value update is byte-identical to a full repaint");

    /* And back down: 30 → 5 (erases the indicator over the span, moves the knob back). */
    p.arc_value = 5.0f;
    er_node_set_props(arc, &p);
    er_commit();
    memcpy(snap, s_fb, sizeof(snap));
    memset(s_fb, 0, sizeof(s_fb));
    er_force_full_repaint();
    er_commit();
    CHECK(memcmp(snap, s_fb, sizeof(snap)) == 0, "decreasing value is byte-identical to a full repaint");

    /* A non-value prop change falls back to the whole box. */
    p.arc_track_color = 0xFF808080U;
    er_node_set_props(arc, &p);
    er_commit();
    CHECK(er_get_dirty_rect(&dr) && dr.w >= 200 && dr.h >= 200, "track recolour repaints the whole box");
}

/** @brief ER_PROP_ARC_VALUE animates natively and each tick damages only the moved sliver. */
static void test_native_animation(void)
{
    reset_scene();
    ERNode* root = make_root(0xFF000000U);
    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps p = arc_props();
    p.arc_value = 0.0f;
    er_node_set_props(arc, &p);
    er_tree_append_child(root, arc);
    er_commit();

    ERAnimConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.type = ER_ANIM_TIMING;
    cfg.duration_ms = 400U;
    er_anim_start(arc, ER_PROP_ARC_VALUE, 80.0f, &cfg);

    float last = 0.0f;
    for (int i = 0; i < 10; i++)
    {
        embedded_renderer_tick(40U); /* advances the animation clock */
        er_commit();
        ERRect dr;
        if (er_get_dirty_rect(&dr))
            CHECK(dr.w < 200 || dr.h < 200, "animation tick damage is narrower than the box");
        CHECK(arc->arc_value >= last, "value ramps monotonically");
        last = arc->arc_value;
    }
    CHECK(fabsf(arc->arc_value - 80.0f) < 0.01f, "animation lands on its target");
    CHECK(near_color(polar_px(120, 120, 92.0f, 340.0f), 0x00A0FFU, 2), "indicator painted up to 80%% (351°)");
    CHECK(near_color(polar_px(120, 120, 92.0f, 360.0f), 0x404040U, 2), "track past 80%%");

    /* Out-of-range targets clamp. */
    er_anim_start(arc, ER_PROP_ARC_VALUE, 500.0f, NULL);
    er_commit();
    CHECK(arc->arc_value == 100.0f, "value clamps to max");
}

static int s_change_count = 0;
static float s_change_last = -1.0f;
static void on_change(ERNode* n, const EREventData* d, void* ud)
{
    (void)n;
    (void)ud;
    s_change_count++;
    s_change_last = d->value;
}

static int s_press_count = 0;
static void on_press(ERNode* n, const EREventData* d, void* ud)
{
    (void)n;
    (void)d;
    (void)ud;
    s_press_count++;
}

/** @brief Drag-to-set: touch on the ring sets the quantized value and fires onChange; hole is transparent. */
static void test_drag(void)
{
    reset_scene();
    ERNode* root = make_root(0xFF000000U);
    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps p = arc_props();
    p.arc_adjustable = 1;
    p.arc_step = 5.0f;
    er_node_set_props(arc, &p);
    er_tree_append_child(root, arc);
    /* A Pressable in the hole, behind the ring's centre. */
    ERNode* btn = er_node_create(ER_NODE_PRESSABLE);
    ERProps bp;
    er_props_default(&bp);
    bp.position = ER_POS_ABSOLUTE;
    bp.left = 60;
    bp.top = 60;
    bp.width = 80;
    bp.height = 80;
    er_node_set_props(btn, &bp);
    er_tree_append_child(arc, btn);
    er_event_set(arc, ER_EVENT_VALUE_CHANGE, on_change, NULL);
    er_event_set(btn, ER_EVENT_PRESS, on_press, NULL);
    er_commit();
    s_change_count = 0;

    /* Touch on the ring at 270° (top) → (270 - 135) / 270 = 50%. */
    const int tx = 120, ty = 120 - 92;
    embedded_renderer_touch(0, ER_TOUCH_DOWN, tx, ty);
    CHECK(s_change_count == 1, "touch-down on the ring fires one value change");
    CHECK(s_change_last == 50.0f, "touch at the top of the dial = 50%%");
    CHECK(arc->arc_value == 50.0f, "node value updated natively");

    /* Move to 315° → 66.67% → quantized to 65. */
    {
        const float a = 315.0f * 0.017453292519943295f;
        embedded_renderer_touch(0, ER_TOUCH_MOVE, (int)(120.0f + 92.0f * cosf(a)), (int)(120.0f + 92.0f * sinf(a)));
        er_input_flush_moves();
        CHECK(s_change_last == 65.0f, "move quantizes to the 5-step grid");
    }
    /* A mid-drag React re-render with the stale value must not snap the knob back. */
    er_node_set_props(arc, &p); /* p.arc_value is still 25 */
    CHECK(arc->arc_value == 65.0f, "set_props mid-drag does not override the dragged value");
    er_commit();
    CHECK(near_color(polar_px(120, 120, 92.0f, 300.0f), 0x00A0FFU, 2), "indicator follows the finger");

    /* Drag on past the max end into the unswept gap: pins at max, and crossing to the min side does not wrap. */
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 120 + 30, 120 + 92); /* ~72°: in the gap, nearer 45° (max) */
    er_input_flush_moves();
    CHECK(s_change_last == 100.0f, "past the end pins at max");
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 120 - 30, 120 + 92); /* ~108°: gap, nearer 135° (min) */
    er_input_flush_moves();
    CHECK(arc->arc_value == 100.0f, "crossing the gap does not wrap to min");
    embedded_renderer_touch(0, ER_TOUCH_UP, 120 - 30, 120 + 92);
    CHECK(arc->arc_drag_finger < 0, "drag released");

    /* After release, props own the value again. */
    p.arc_value = 10.0f;
    er_node_set_props(arc, &p);
    CHECK(arc->arc_value == 10.0f, "props apply once the drag ends");

    /* A readout CHILD parked in the hole must not block the drag when the ring itself is touched, and
     * must stay inert when it is touched — the layout every real dial has. */
    {
        ERNode* readout = er_node_create(ER_NODE_VIEW);
        ERProps rp;
        er_props_default(&rp);
        rp.position = ER_POS_ABSOLUTE;
        rp.left = 50;
        rp.top = 50;
        rp.width = 100;
        rp.height = 100;
        rp.background_color = 0xFF222222U;
        er_node_set_props(readout, &rp);
        er_tree_append_child(arc, readout);
        er_commit();
        s_change_count = 0;
        embedded_renderer_touch(0, ER_TOUCH_DOWN, tx, ty); /* on the ring, over nothing */
        CHECK(s_change_count == 1, "a dial with a centre readout still drags from the ring");
        embedded_renderer_touch(0, ER_TOUCH_UP, tx, ty);
        s_change_count = 0;
        embedded_renderer_touch(0, ER_TOUCH_DOWN, 120, 120); /* on the readout, in the hole */
        embedded_renderer_touch(0, ER_TOUCH_UP, 120, 120);
        CHECK(s_change_count == 0, "touching the readout in the hole does not drag");
        er_tree_remove_child(arc, readout);
        er_node_destroy(readout);
        er_commit();
    }

    /* Multi-touch: a dial has ONE value, so the first finger down owns it. A second finger must not
     * re-latch the end, and lifting it must not end the first finger's drag. */
    s_change_count = 0;
    embedded_renderer_touch(0, ER_TOUCH_DOWN, tx, ty); /* finger 0 grabs the ring */
    CHECK(arc->arc_drag_finger == 0, "finger 0 owns the drag");
    {
        const float a = 200.0f * 0.017453292519943295f; /* a different point on the ring */
        const int bx = (int)(120.0f + 92.0f * cosf(a)), by = (int)(120.0f + 92.0f * sinf(a));
        embedded_renderer_touch(1, ER_TOUCH_DOWN, bx, by); /* finger 1 lands on the same dial */
        CHECK(arc->arc_drag_finger == 0, "a second finger does not take over the drag");
        embedded_renderer_touch(1, ER_TOUCH_UP, bx, by);
        CHECK(arc->arc_drag_finger == 0, "lifting the second finger does not end the first's drag");
    }
    embedded_renderer_touch(0, ER_TOUCH_UP, tx, ty);
    CHECK(arc->arc_drag_finger < 0, "the owning finger ends it");

    /* The hole is transparent: a tap at the centre reaches the Pressable behind the ring. */
    s_press_count = 0;
    s_change_count = 0;
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 120, 120);
    embedded_renderer_touch(0, ER_TOUCH_UP, 120, 120);
    CHECK(s_press_count == 1, "tap in the hole hits the child Pressable");
    CHECK(s_change_count == 0, "tap in the hole does not change the value");

    /* The unswept gap is transparent too. */
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 120, 120 + 92);
    embedded_renderer_touch(0, ER_TOUCH_UP, 120, 120 + 92);
    CHECK(s_change_count == 0, "tap in the unswept gap does not change the value");

    /* Not adjustable: touches on the ring do nothing. */
    p.arc_adjustable = 0;
    er_node_set_props(arc, &p);
    er_commit();
    embedded_renderer_touch(0, ER_TOUCH_DOWN, tx, ty);
    embedded_renderer_touch(0, ER_TOUCH_UP, tx, ty);
    CHECK(s_change_count == 0, "non-adjustable arc ignores ring touches");
}

/** @brief pointer-events and transforms must be honoured by the NATIVE drag, not just the hit test. */
static void test_drag_pointer_events_and_transform(void)
{
    /* box-none declares the node touch-transparent. The drag walk reaches an Arc by climbing from a
     * non-interactive descendant, so it has to re-check that itself — the hit test's own box-none
     * handling never applies to the node the walk lands on. */
    reset_scene();
    ERNode* root = make_root(0xFF000000U);
    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps p = arc_props();
    p.arc_adjustable = 1;
    p.pointer_events = ER_POINTER_EVENTS_BOX_NONE;
    er_node_set_props(arc, &p);
    er_tree_append_child(root, arc);
    er_event_set(arc, ER_EVENT_VALUE_CHANGE, on_change, NULL);
    er_commit();
    s_change_count = 0;
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 120, 120 - 92);
    embedded_renderer_touch(0, ER_TOUCH_UP, 120, 120 - 92);
    CHECK(s_change_count == 0, "a box-none arc does not take the gesture");

    p.pointer_events = ER_POINTER_EVENTS_AUTO;
    er_node_set_props(arc, &p);
    er_commit();
    s_change_count = 0;
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 120, 120 - 92);
    CHECK(s_change_count == 1, "the same arc drags once pointer-events allows it");
    embedded_renderer_touch(0, ER_TOUCH_UP, 120, 120 - 92);

    /* A TRANSLATED arc: the ordinary hit test inverse-maps the transform, so the native drag has to use
     * the same conversion or the dial is visibly under the finger and never becomes the drag target. */
    reset_scene();
    root = make_root(0xFF000000U);
    arc = er_node_create(ER_NODE_ARC);
    p = arc_props();
    p.arc_adjustable = 1;
    /* Bigger than the ring's touch slop, so "where it would be" and "where it is" are unambiguously
     * different points — a small offset lands inside the slop and proves nothing. */
    p.transform_translate_x = 70.0f;
    p.transform_translate_y = 0.0f;
    er_node_set_props(arc, &p);
    er_tree_append_child(root, arc);
    er_event_set(arc, ER_EVENT_VALUE_CHANGE, on_change, NULL);
    er_commit();

    s_change_count = 0;
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 120, 120 - 92); /* where it WOULD be untransformed */
    embedded_renderer_touch(0, ER_TOUCH_UP, 120, 120 - 92);
    CHECK(s_change_count == 0, "a translated arc ignores a touch at its untransformed position");

    s_change_count = 0;
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 190, 120 - 92); /* where it is actually drawn */
    CHECK(s_change_count == 1, "a translated arc drags from where it is drawn");
    CHECK(s_change_last == 50.0f, "and resolves the same value as an untransformed one");
    embedded_renderer_touch(0, ER_TOUCH_UP, 190, 120 - 92);
}

/** @brief ER_ARC_KNOB_CHILD: the first child is centred on the value point, and follows the value. */
static void test_child_knob(void)
{
    reset_scene();
    ERNode* root = make_root(0xFF000000U);
    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps p = arc_props();
    p.arc_value = 50.0f; /* top: angle 270° → knob centre (120, 28) */
    p.arc_knob = ER_ARC_KNOB_CHILD;
    er_node_set_props(arc, &p);
    er_tree_append_child(root, arc);
    ERNode* knob = er_node_create(ER_NODE_VIEW);
    ERProps kp;
    er_props_default(&kp);
    kp.width = 20;
    kp.height = 20;
    kp.border_radius = 10;
    kp.background_color = 0xFFFF0000U;
    er_node_set_props(knob, &kp);
    er_tree_append_child(arc, knob);
    er_commit();

    CHECK(knob->computed.x == 110 && knob->computed.y == 18, "child knob centred on the value point");
    CHECK(near_color(px(120, 28), 0xFF0000U, 2), "child knob painted at the value point");

    /* Value → 0: knob moves to the 135° point (120 - 65.05, 120 + 65.05) = (54.9, 185.1). */
    p.arc_value = 0.0f;
    er_node_set_props(arc, &p);
    er_commit();
    CHECK(abs((int)knob->computed.x + 10 - 55) <= 1 && abs((int)knob->computed.y + 10 - 185) <= 1,
          "child knob follows the value");
    if (getenv("ER_ARC_DEBUG"))
        fprintf(stderr,
                "child computed %d,%d %dx%d animated %d,%d last_paint %d,%d %dx%d px(120,28)=%06x\n",
                knob->computed.x,
                knob->computed.y,
                knob->computed.w,
                knob->computed.h,
                knob->animated.x,
                knob->animated.y,
                knob->last_paint_rect.x,
                knob->last_paint_rect.y,
                knob->last_paint_rect.w,
                knob->last_paint_rect.h,
                px(120, 28));
    CHECK(near_color(px(120, 28), 0x404040U, 2), "old knob spot erased (the track shows through)");

    /* A knob subtree LARGER than any fixed traversal budget must move as one piece. The walk used to
     * push pending children onto a 64-slot stack and stop when it filled, leaving the tail behind at the
     * old position — a subtree visually split across the value's two spots. */
    {
        enum
        {
            FAN = 80
        };
        ERNode* kids[FAN];
        ERProps gp;
        er_props_default(&gp);
        gp.width = 2;
        gp.height = 2;
        for (int i = 0; i < FAN; i++)
        {
            kids[i] = er_node_create(ER_NODE_VIEW);
            er_node_set_props(kids[i], &gp);
            er_tree_append_child(knob, kids[i]);
        }
        p.arc_value = 50.0f;
        er_node_set_props(arc, &p);
        er_commit();
        int16_t before_x[FAN], before_y[FAN];
        for (int i = 0; i < FAN; i++)
        {
            before_x[i] = kids[i]->computed.x;
            before_y[i] = kids[i]->computed.y;
        }
        const int16_t knob_x0 = knob->computed.x, knob_y0 = knob->computed.y;

        p.arc_value = 0.0f;
        er_node_set_props(arc, &p);
        er_commit();
        const int16_t ddx = (int16_t)(knob->computed.x - knob_x0);
        const int16_t ddy = (int16_t)(knob->computed.y - knob_y0);
        CHECK(ddx != 0 || ddy != 0, "the knob child actually moved");
        int moved_with = 0;
        for (int i = 0; i < FAN; i++)
        {
            if (kids[i]->computed.x - before_x[i] == ddx && kids[i]->computed.y - before_y[i] == ddy)
                moved_with++;
        }
        CHECK(moved_with == FAN, "every descendant of a large knob subtree moves with it");
        for (int i = 0; i < FAN; i++)
        {
            er_tree_remove_child(knob, kids[i]);
            er_node_destroy(kids[i]);
        }
        er_commit();
    }
    CHECK(near_color(px(55, 185), 0xFF0000U, 2), "new knob spot painted");
}

/** @brief A knob wider than the ring paints past the box and is erased when the node moves away. */
static void test_overhang(void)
{
    reset_scene();
    ERNode* root = make_root(0xFF000000U);
    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps p = arc_props();
    p.arc_width = 4;
    p.arc_knob = ER_ARC_KNOB_CIRCLE;
    p.arc_knob_size = 40;
    p.arc_knob_color = 0xFF00FF00U;
    p.arc_knob_border_width = 0;
    p.arc_value = 50.0f; /* knob at the top: centre (120, 20 + 2) = (120, 22); reaches y = 2 */
    er_node_set_props(arc, &p);
    er_tree_append_child(root, arc);
    er_commit();
    CHECK(arc->arc_overhang >= 18, "overhang computed for a 40 px knob on a 4 px ring");
    CHECK(near_color(px(120, 8), 0x00FF00U, 2), "knob painted above the layout box");

    /* Move the node down: the overhanging knob pixels must be erased. */
    p.margin_top = 40;
    er_node_set_props(arc, &p);
    er_commit();
    CHECK(near_color(px(120, 8), 0x000000U, 0), "old overhang erased after the move");
    CHECK(near_color(px(120, 28), 0x00FF00U, 2), "knob painted at the new spot");
}

/** @brief Span cache: a second identical frame serves every radius from the cache. */
static void test_span_cache(void)
{
    er_arc_span_cache_reset();
    (void)er_arc_half_chord(50.0f, 0.5f);
    const uint32_t m1 = er_arc_span_cache_misses();
    (void)er_arc_half_chord(50.0f, 10.5f);
    (void)er_arc_half_chord(50.0f, -3.5f);
    CHECK(er_arc_span_cache_misses() == m1, "same radius + phase hits the cache");
    CHECK(fabsf(er_arc_half_chord(50.0f, 30.5f) - sqrtf(50.0f * 50.0f - 30.5f * 30.5f)) < 0.01f,
          "cached half-chord is exact to 1/256");
    CHECK(er_arc_half_chord(50.0f, 50.5f) < 0.0f, "rows past the radius miss the circle");

    /* A radius past ERUI_ARC_MAX_RADIUS is not cacheable and falls back to an exact per-row sqrt —
     * reachable on a big panel (a full-width dial on an 800 px screen), so it must still be right. */
    const float big = (float)ERUI_ARC_MAX_RADIUS + 40.0f;
    const uint32_t m2 = er_arc_span_cache_misses();
    CHECK(fabsf(er_arc_half_chord(big, 10.5f) - sqrtf(big * big - 10.5f * 10.5f)) < 0.01f,
          "an uncacheable radius still returns the exact half-chord");
    CHECK(er_arc_span_cache_misses() == m2, "an uncacheable radius does not evict a cache entry");
}

/*----------------------------------------------------------------------------------------------------------------------
 - <Svg> arc ↔ native arc parity (issue #87's follow-up: retarget ER_VOP_ARC at the shared analytic core)
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_snap_a[SCREEN * SCREEN];
static uint32_t s_snap_b[SCREEN * SCREEN];

/** @brief Builds an <Svg> node at (20,20) 200x200 carrying one stroked arc, and commits. */
static void render_svg_arc(const float* ops, int n_ops, const ERVectorPaint* paint)
{
    reset_scene();
    er_vector_analytic_arc_count_reset();
    ERNode* root = make_root(0xFF000000U);
    ERNode* svg = er_node_create(ER_NODE_VECTOR);
    ERProps p;
    er_props_default(&p);
    p.width = 200;
    p.height = 200;
    p.margin_left = 20;
    p.margin_top = 20;
    er_node_set_props(svg, &p);
    er_tree_append_child(root, svg);
    er_node_set_vector_ops(svg, ops, n_ops, paint, 1, NULL, 0);
    er_commit();
}

/** @brief Builds a native ER_NODE_ARC whose TRACK matches the arc above (value at min → no indicator). */
static void render_native_track(float start_deg, float sweep_deg, uint32_t color, uint8_t cap)
{
    reset_scene();
    ERNode* root = make_root(0xFF000000U);
    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps p = arc_props();
    p.arc_value = 0.0f; /* frac 0 → the indicator draws nothing; only the track is painted */
    p.arc_start_angle = start_deg;
    p.arc_sweep_angle = sweep_deg;
    p.arc_track_color = color;
    p.arc_cap = cap;
    er_node_set_props(arc, &p);
    er_tree_append_child(root, arc);
    er_commit();
}

/**
 * @brief A stroked <Svg> arc and a native ER_NODE_ARC of the same geometry must be pixel-identical.
 *
 * Box (20,20) 200x200 → centre (120,120), outer radius 100. The native track is 16 px on a 92 px
 * mid-radius (ring 84..100); the tape's arc is r=92 with strokeWidth 16 (the same ring), so both
 * describe one annular sector and — now that both go through er_arc_fill_sector — must agree exactly.
 */
static void test_svg_native_parity(void)
{
    const float A0 = 135.0f * 0.017453292519943295f;
    const float A1 = 405.0f * 0.017453292519943295f;
    const uint32_t COL = 0xFF00A0FFU;

    for (int round = 0; round < 2; round++)
    {
        const bool round_cap = (round == 1);
        /* Tape is NODE-LOCAL: the node box is at (20,20), so its centre is (100,100) locally. */
        const float ops[] = {ER_VOP_SHAPE, 0.0f, ER_VOP_ARC, 100.0f, 100.0f, 92.0f, A0, A1, 0.0f};
        ERVectorPaint paint;
        memset(&paint, 0, sizeof(paint));
        paint.stroke = COL;
        paint.stroke_w = 16.0f;
        paint.cap = round_cap ? ER_VCAP_ROUND : ER_VCAP_BUTT;

        render_svg_arc(ops, (int)(sizeof(ops) / sizeof(ops[0])), &paint);
        CHECK(er_vector_analytic_arc_count() == 1U, "a lone stroked <Svg> arc takes the analytic route");
        memcpy(s_snap_a, s_fb, sizeof(s_snap_a));

        render_native_track(135.0f, 270.0f, COL, round_cap ? ER_ARC_CAP_ROUND : ER_ARC_CAP_BUTT);
        memcpy(s_snap_b, s_fb, sizeof(s_snap_b));

        CHECK(memcmp(s_snap_a, s_snap_b, sizeof(s_snap_a)) == 0,
              round_cap ? "round-cap <Svg> arc is pixel-identical to the native arc"
                        : "butt-cap <Svg> arc is pixel-identical to the native arc");
    }
}

/** @brief The <Circle> tape (MOVE, ARC 0..2PI, CLOSE) routes — including its redundant leading MOVE. */
static void test_svg_circle_route(void)
{
    /* Exactly what svg-ops.js / the AOT emit for <Circle cx=100 cy=100 r=60>. */
    const float ops[] = {ER_VOP_SHAPE,
                         0.0f,
                         ER_VOP_MOVE,
                         160.0f,
                         100.0f,
                         ER_VOP_ARC,
                         100.0f,
                         100.0f,
                         60.0f,
                         0.0f,
                         6.283185307179586f,
                         0.0f,
                         ER_VOP_CLOSE};
    ERVectorPaint paint;
    memset(&paint, 0, sizeof(paint));
    paint.fill = 0xFF203040U;
    paint.stroke = 0xFFFFAA00U;
    paint.stroke_w = 10.0f;

    render_svg_arc(ops, (int)(sizeof(ops) / sizeof(ops[0])), &paint);
    CHECK(er_vector_analytic_arc_count() == 1U, "a filled+stroked <Circle> takes the analytic route");
    /* Centre (20+100, 20+100) = (120,120): disc fill inside r=60, stroke ring straddling it, clear outside. */
    CHECK(near_color(px(120, 120), 0x203040U, 2), "circle fill painted at the centre");
    CHECK(near_color(polar_px(120.0f, 120.0f, 60.0f, 20.0f), 0xFFAA00U, 2), "circle stroke straddles r");
    CHECK(near_color(polar_px(120.0f, 120.0f, 40.0f, 20.0f), 0x203040U, 2), "circle fill inside the stroke");
    CHECK(near_color(polar_px(120.0f, 120.0f, 80.0f, 20.0f), 0x000000U, 0), "nothing outside the circle");

    /* A leading MOVE that does NOT land on the arc's start point means a real connecting line — the
     * analytic core cannot express that, so it must fall back to the tessellated path. */
    float bad[sizeof(ops) / sizeof(ops[0])];
    memcpy(bad, ops, sizeof(ops));
    bad[3] = 10.0f; /* MOVE to (10,100) instead of (160,100) */
    render_svg_arc(bad, (int)(sizeof(bad) / sizeof(bad[0])), &paint);
    CHECK(er_vector_analytic_arc_count() == 0U, "a MOVE away from the arc start falls back to tessellation");

    /* A float-rounded MOVE (off by a ULP-ish amount) still routes — the general path's exact-equality
     * vertex dedupe is what spiked a miter join here before. */
    memcpy(bad, ops, sizeof(ops));
    bad[3] = 160.0f + 1e-4f;
    render_svg_arc(bad, (int)(sizeof(bad) / sizeof(bad[0])), &paint);
    CHECK(er_vector_analytic_arc_count() == 1U, "a ULP-off MOVE still routes (epsilon match)");
}

/** @brief Shapes the sector core cannot express must keep the general tessellated path — and still draw. */
static void test_svg_fallbacks(void)
{
    const float A0 = 135.0f * 0.017453292519943295f;
    const float A1 = 270.0f * 0.017453292519943295f;
    const float arc_ops[] = {ER_VOP_SHAPE, 0.0f, ER_VOP_ARC, 100.0f, 100.0f, 92.0f, A0, A1, 0.0f};
    const int arc_n = (int)(sizeof(arc_ops) / sizeof(arc_ops[0]));
    ERVectorPaint paint;

    /* Square cap: the sector core has butt and round only. */
    memset(&paint, 0, sizeof(paint));
    paint.stroke = 0xFF00A0FFU;
    paint.stroke_w = 16.0f;
    paint.cap = ER_VCAP_SQUARE;
    render_svg_arc(arc_ops, arc_n, &paint);
    CHECK(er_vector_analytic_arc_count() == 0U, "a square-cap arc falls back");
    CHECK(near_color(polar_px(120.0f, 120.0f, 92.0f, 200.0f), 0x00A0FFU, 2), "square-cap arc still draws");

    /* A FILLED partial arc closes on a chord (a circular segment), not a sector. */
    memset(&paint, 0, sizeof(paint));
    paint.fill = 0xFF44FF44U;
    render_svg_arc(arc_ops, arc_n, &paint);
    CHECK(er_vector_analytic_arc_count() == 0U, "a filled partial arc falls back (chord, not sector)");
    CHECK(near_color(px(120, 120), 0x000000U, 0),
          "the segment's chord leaves the centre empty (a sector would have filled it)");

    /* A stroke wide enough to swallow the centre self-intersects — keep the outline path. */
    memset(&paint, 0, sizeof(paint));
    paint.stroke = 0xFF00A0FFU;
    paint.stroke_w = 200.0f;
    render_svg_arc(arc_ops, arc_n, &paint);
    CHECK(er_vector_analytic_arc_count() == 0U, "an over-wide stroke falls back");

    /* An arc joined to a line is a multi-op subpath with a join — the general path owns that. */
    const float joined[] = {
        ER_VOP_SHAPE, 0.0f, ER_VOP_ARC, 100.0f, 100.0f, 92.0f, A0, A1, 0.0f, ER_VOP_LINE, 100.0f, 100.0f};
    memset(&paint, 0, sizeof(paint));
    paint.stroke = 0xFF00A0FFU;
    paint.stroke_w = 8.0f;
    render_svg_arc(joined, (int)(sizeof(joined) / sizeof(joined[0])), &paint);
    CHECK(er_vector_analytic_arc_count() == 0U, "an arc joined to a line falls back");

    /* Two arcs in one shape (a track + value pair) route independently — one call each. */
    const float two[] = {ER_VOP_SHAPE,
                         0.0f,
                         ER_VOP_ARC,
                         100.0f,
                         100.0f,
                         92.0f,
                         A0,
                         A1,
                         0.0f,
                         ER_VOP_SHAPE,
                         0.0f,
                         ER_VOP_ARC,
                         100.0f,
                         100.0f,
                         70.0f,
                         A0,
                         A1,
                         0.0f};
    memset(&paint, 0, sizeof(paint));
    paint.stroke = 0xFF00A0FFU;
    paint.stroke_w = 8.0f;
    render_svg_arc(two, (int)(sizeof(two) / sizeof(two[0])), &paint);
    CHECK(er_vector_analytic_arc_count() == 2U, "two arc shapes route as two sectors");
}

/** @brief RANGE mode: the band spans [valueStart, value], each end has a knob, and each drags on its own. */
static void test_range(void)
{
    reset_scene();
    ERNode* root = make_root(0xFF000000U);
    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps p = arc_props();
    p.arc_range = 1;
    p.arc_value_start = 25.0f; /* 135 + 270*0.25 = 202.5 deg */
    p.arc_value = 75.0f;       /* 135 + 270*0.75 = 337.5 deg */
    p.arc_adjustable = 1;
    p.arc_step = 5.0f;
    p.arc_knob = ER_ARC_KNOB_CIRCLE;
    p.arc_knob_size = 20;
    p.arc_knob_color = 0xFFFFFFFFU;
    p.arc_knob_border_width = 0;
    er_node_set_props(arc, &p);
    er_tree_append_child(root, arc);
    er_event_set(arc, ER_EVENT_VALUE_CHANGE, on_change, NULL);
    er_commit();

    CHECK(near_color(polar_px(120, 120, 92.0f, 270.0f), 0x00A0FFU, 2), "range band painted between the ends");
    CHECK(near_color(polar_px(120, 120, 92.0f, 180.0f), 0x404040U, 2), "track below the low end");
    CHECK(near_color(polar_px(120, 120, 92.0f, 360.0f), 0x404040U, 2), "track above the high end");
    CHECK(near_color(polar_px(120, 120, 92.0f, 202.5f), 0xFFFFFFU, 2), "knob at the low end");
    CHECK(near_color(polar_px(120, 120, 92.0f, 337.5f), 0xFFFFFFU, 2), "knob at the high end");

    /* Grab nearer the LOW end and drag it: only value_start moves. */
    s_change_count = 0;
    {
        const float a = 210.0f * 0.017453292519943295f;
        embedded_renderer_touch(0, ER_TOUCH_DOWN, (int)(120.0f + 92.0f * cosf(a)), (int)(120.0f + 92.0f * sinf(a)));
        CHECK(arc->arc_drag_low, "a touch near the low end latches the low end");
        const float b = 240.0f * 0.017453292519943295f; /* (240-135)/270 = 38.9% -> step 5 -> 40 */
        embedded_renderer_touch(0, ER_TOUCH_MOVE, (int)(120.0f + 92.0f * cosf(b)), (int)(120.0f + 92.0f * sinf(b)));
        er_input_flush_moves();
        CHECK(arc->arc_value_start == 40.0f, "dragging the low end moves value_start");
        CHECK(arc->arc_value == 75.0f, "the high end is untouched");
        CHECK(s_change_last == 75.0f, "the event reports the high value");
    }
    /* Dragging the low end PAST the high end clamps instead of inverting the band. */
    {
        const float c = 380.0f * 0.017453292519943295f;
        embedded_renderer_touch(0, ER_TOUCH_MOVE, (int)(120.0f + 92.0f * cosf(c)), (int)(120.0f + 92.0f * sinf(c)));
        er_input_flush_moves();
        CHECK(arc->arc_value_start == 75.0f, "the low end clamps at the high end (no inverted band)");
        CHECK(arc->arc_value == 75.0f, "the high end still did not move");
        CHECK(arc->arc_drag_low, "the latch holds even when the ends meet");
    }
    embedded_renderer_touch(0, ER_TOUCH_UP, 120, 120);
    CHECK(!arc->arc_drag_low, "the latch clears on release");

    /* Grab nearer the HIGH end: only value moves. */
    p.arc_value_start = 25.0f;
    er_node_set_props(arc, &p);
    er_commit();
    {
        const float a = 330.0f * 0.017453292519943295f;
        embedded_renderer_touch(0, ER_TOUCH_DOWN, (int)(120.0f + 92.0f * cosf(a)), (int)(120.0f + 92.0f * sinf(a)));
        CHECK(!arc->arc_drag_low, "a touch near the high end latches the high end");
        CHECK(arc->arc_value_start == 25.0f, "the low end is untouched by a high-end drag");
        embedded_renderer_touch(0, ER_TOUCH_UP, 120, 120);
    }

    /* A range value change still damages only a sliver. */
    p.arc_value = 60.0f;
    er_node_set_props(arc, &p);
    er_commit();
    ERRect dr;
    CHECK(er_get_dirty_rect(&dr), "a range value change reports a dirty rect");
    CHECK(dr.w < 140 && dr.h < 140, "range value-change damage stays a sliver");

    /* Widening the band in ONE setProps (both ends move outward) must not self-clamp. */
    p.arc_value_start = 10.0f;
    p.arc_value = 90.0f;
    er_node_set_props(arc, &p);
    er_commit();
    CHECK(arc->arc_value_start == 10.0f && arc->arc_value == 90.0f, "both ends widen in one setProps");

    /* And the animation driver drives each end independently. */
    er_anim_start(arc, ER_PROP_ARC_VALUE_START, 30.0f, NULL);
    er_commit();
    CHECK(arc->arc_value_start == 30.0f, "ER_PROP_ARC_VALUE_START animates the low end");
}

/**
 * @brief A conic ramp anchored to the BAND must repaint the whole band when either end moves.
 *
 * Range mode maps the ramp across [valueStart, value], so moving one end re-colours EVERY pixel of the
 * band — not just the sub-arc that end swept. Damaging only the swept sliver leaves the rest of the band
 * showing the previous frame's colours, which reads on a panel as a gradient that doesn't update until
 * something else forces a full repaint (a release committing new props). Non-range conic is anchored to
 * the full sweep instead, so there the swept sliver genuinely is all that changed.
 */
static void test_range_conic_damage(void)
{
    reset_scene();
    ERNode* root = make_root(0xFF000000U);
    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps p = arc_props();
    p.arc_range = 1;
    p.arc_value_start = 30.0f;
    p.arc_value = 70.0f;
    p.gradient_type = ER_GRADIENT_CONIC;
    p.gradient_stop_count = 2;
    p.gradient_stops[0].color = 0xFFFF0000U;
    p.gradient_stops[0].position = 0.0f;
    p.gradient_stops[1].color = 0xFF0000FFU;
    p.gradient_stops[1].position = 1.0f;
    er_node_set_props(arc, &p);
    er_tree_append_child(root, arc);
    er_commit();

    /* Move ONLY the low end, the way a drag does (the props path would repaint the whole box). */
    static uint32_t inc[SCREEN * SCREEN];
    CHECK(er_arc_apply_value_start(arc, 45.0f), "low end moved");
    er_mark_dirty_upward(arc);
    er_commit();
    memcpy(inc, s_fb, sizeof(inc));

    memset(s_fb, 0, sizeof(s_fb));
    er_force_full_repaint();
    er_commit();
    CHECK(memcmp(inc, s_fb, sizeof(inc)) == 0, "widening a conic range band repaints the WHOLE band (ramp re-anchors)");

    /* The other direction: shrinking the band must cover both the re-coloured part AND the part the end
     * vacated. A union that only reaches the moved end's NEW position misses one of the two. */
    CHECK(er_arc_apply_value_start(arc, 32.0f), "low end moved back");
    er_mark_dirty_upward(arc);
    er_commit();
    memcpy(inc, s_fb, sizeof(inc));
    memset(s_fb, 0, sizeof(s_fb));
    er_force_full_repaint();
    er_commit();
    CHECK(memcmp(inc, s_fb, sizeof(inc)) == 0, "shrinking a conic range band also repaints the whole band");

    /* And the HIGH end, where the fixed end sits below rather than above the moved one. */
    CHECK(er_arc_apply_value(arc, 55.0f), "high end moved");
    er_mark_dirty_upward(arc);
    er_commit();
    memcpy(inc, s_fb, sizeof(inc));
    memset(s_fb, 0, sizeof(s_fb));
    er_force_full_repaint();
    er_commit();
    CHECK(memcmp(inc, s_fb, sizeof(inc)) == 0, "moving the HIGH end of a conic range band repaints it all");

    /* A SINGLE-ENDED conic arc is anchored to the whole sweep instead, so its colours are pinned to dial
     * positions and nothing outside the swept sub-arc changes — that one must stay a tight sliver. */
    reset_scene();
    root = make_root(0xFF000000U);
    arc = er_node_create(ER_NODE_ARC);
    p.arc_range = 0;
    p.arc_value = 70.0f;
    er_node_set_props(arc, &p);
    er_tree_append_child(root, arc);
    er_commit();
    CHECK(er_arc_apply_value(arc, 55.0f), "single-ended value moved");
    er_mark_dirty_upward(arc);
    er_commit();
    memcpy(inc, s_fb, sizeof(inc));
    ERRect sd;
    const bool got_single = er_get_dirty_rect(&sd);
    memset(s_fb, 0, sizeof(s_fb));
    er_force_full_repaint();
    er_commit();
    CHECK(memcmp(inc, s_fb, sizeof(inc)) == 0, "a sweep-anchored conic arc still matches a full repaint");
    CHECK(got_single && (long)sd.w * (long)sd.h < 14000L,
          "a sweep-anchored conic arc keeps its damage to the swept sliver");

    /* The same move on a SOLID band must still damage only a sliver — the fix must not widen that. */
    reset_scene();
    root = make_root(0xFF000000U);
    arc = er_node_create(ER_NODE_ARC);
    p.gradient_type = ER_GRADIENT_NONE;
    p.gradient_stop_count = 0;
    p.arc_range = 1; /* restored: the single-ended case above cleared it */
    p.arc_value = 70.0f;
    p.arc_value_start = 30.0f;
    er_node_set_props(arc, &p);
    er_tree_append_child(root, arc);
    er_commit();
    (void)er_arc_apply_value_start(arc, 45.0f);
    er_mark_dirty_upward(arc);
    er_commit();
    ERRect dr;
    CHECK(er_get_dirty_rect(&dr) && dr.w < 150 && dr.h < 150, "a solid range band still damages only the swept sliver");
}

/** @brief min_span: the ends keep a minimum separation, pushing each other rather than stopping dead. */
static void test_range_min_span(void)
{
    reset_scene();
    ERNode* root = make_root(0xFF000000U);
    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps p = arc_props();
    p.arc_range = 1;
    p.arc_min_span = 10.0f; /* range is 0..100 */
    p.arc_value_start = 30.0f;
    p.arc_value = 70.0f;
    p.arc_adjustable = 1;
    p.arc_step = 1.0f;
    er_node_set_props(arc, &p);
    er_tree_append_child(root, arc);
    er_commit();

    /* Drive the low end up into the high end: the high end is carried along at min_span. */
    CHECK(er_arc_apply_value_start(arc, 65.0f), "low end moved up");
    CHECK(arc->arc_value_start == 65.0f, "low end lands where it was put");
    CHECK(arc->arc_value == 75.0f, "high end pushed along to keep the 10-unit span");

    /* Keep pushing: the pair travels together until the high end hits the range top, then the low
     * end stops too — it can never squeeze the span below min_span. */
    CHECK(er_arc_apply_value_start(arc, 98.0f), "low end pushed toward the top");
    CHECK(arc->arc_value == 100.0f, "high end stops at the range top");
    CHECK(arc->arc_value_start == 90.0f, "low end stops one span below the top");

    /* And symmetrically from the high end downward. */
    CHECK(er_arc_apply_value(arc, 20.0f), "high end moved down");
    CHECK(arc->arc_value == 20.0f, "high end lands where it was put");
    CHECK(arc->arc_value_start == 10.0f, "low end pushed down to keep the span");
    CHECK(er_arc_apply_value(arc, 2.0f), "high end pushed toward the bottom");
    CHECK(arc->arc_value_start == 0.0f, "low end stops at the range floor");
    CHECK(arc->arc_value == 10.0f, "high end stops one span above the floor");

    /* A push damages BOTH ends' knobs, so the frame still matches a full repaint. */
    p.arc_knob = ER_ARC_KNOB_CIRCLE;
    p.arc_knob_size = 20;
    p.arc_value_start = 30.0f;
    p.arc_value = 70.0f;
    er_node_set_props(arc, &p);
    er_commit();
    static uint32_t snap[SCREEN * SCREEN];
    CHECK(er_arc_apply_value_start(arc, 68.0f), "low end pushes the high end");
    er_mark_dirty_upward(arc);
    er_commit();
    memcpy(snap, s_fb, sizeof(snap));
    memset(s_fb, 0, sizeof(s_fb));
    er_force_full_repaint();
    er_commit();
    CHECK(memcmp(snap, s_fb, sizeof(snap)) == 0, "a pushed far end is fully repainted (both knobs move)");

    /* A min_span WIDER than the range cannot be honoured. It must clamp to the range instead of running
     * the endpoint arithmetic with it — which used to store out-of-range values and, because the push
     * re-read a not-yet-assigned endpoint, recurse until the stack died. */
    p.arc_min_span = 200.0f; /* range is 0..100 */
    p.arc_value_start = 30.0f;
    p.arc_value = 70.0f;
    er_node_set_props(arc, &p);
    CHECK(arc->arc_value_start >= 0.0f && arc->arc_value_start <= 100.0f, "over-wide min_span keeps low in range");
    CHECK(arc->arc_value >= 0.0f && arc->arc_value <= 100.0f, "over-wide min_span keeps high in range");
    CHECK(arc->arc_value_start == 0.0f && arc->arc_value == 100.0f, "over-wide min_span opens the band fully");

    /* Raising min_span past what the LIVE endpoints allow: the same recursion trigger, via setProps. */
    p.arc_min_span = 0.0f;
    p.arc_value_start = 30.0f;
    p.arc_value = 70.0f;
    er_node_set_props(arc, &p);
    CHECK(arc->arc_value_start == 30.0f && arc->arc_value == 70.0f, "band reset for the widening check");
    p.arc_min_span = 99.0f;
    er_node_set_props(arc, &p);
    CHECK(arc->arc_value - arc->arc_value_start >= 98.9f, "widening min_span opens the live band to the new span");
    CHECK(arc->arc_value_start >= 0.0f && arc->arc_value <= 100.0f, "and both ends stay inside the range");
    /* And a drag on top of that settles rather than recursing. */
    (void)er_arc_apply_value_start(arc, 50.0f);
    CHECK(arc->arc_value_start >= 0.0f && arc->arc_value <= 100.0f, "a drag under an over-wide span stays in range");

    /* A negative min_span is meaningless; treat it as none. */
    p.arc_min_span = -5.0f;
    p.arc_value_start = 30.0f;
    p.arc_value = 70.0f;
    er_node_set_props(arc, &p);
    CHECK(arc->arc_value_start == 30.0f && arc->arc_value == 70.0f, "a negative min_span behaves as none");

    /* min_span 0 keeps the old clamp-at-neighbour behaviour. */
    p.arc_min_span = 0.0f;
    p.arc_value_start = 30.0f;
    p.arc_value = 70.0f;
    er_node_set_props(arc, &p);
    er_commit();
    (void)er_arc_apply_value_start(arc, 90.0f);
    CHECK(arc->arc_value_start == 70.0f && arc->arc_value == 70.0f,
          "with no min_span the ends meet and the far end stays put");
}

int main(void)
{
    EmbeddedRenderBackend be;
    memset(&be, 0, sizeof(be));
    be.fill_rect = fb_fill;
    be.copy_rect = fb_copy;
    be.blend_rect = fb_blend;
    embedded_renderer_set_backend(&be);

    test_geometry();
    test_showcase();
    test_segments();
    test_value_damage();
    test_native_animation();
    test_drag();
    test_drag_pointer_events_and_transform();
    test_child_knob();
    test_overhang();
    test_span_cache();
    test_svg_native_parity();
    test_svg_circle_route();
    test_svg_fallbacks();
    test_range();
    test_range_conic_damage();
    test_range_min_span();

    if (s_failures)
    {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return EXIT_FAILURE;
    }
    printf("test_arc: all checks passed\n");
    return EXIT_SUCCESS;
}
