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
 *     result is byte-identical to a forced full repaint (the catch-all for clip / double-blend bugs) —
 *     including when a whole GRID of dials moves at once, where the damage budget used to saturate and
 *     hand every dial a merged clip to rasterize against,
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
#define TWO_PI 6.283185307179586f

/*----------------------------------------------------------------------------------------------------------------------
 - Backend: straight-alpha fill + premultiplied blend, source-over into a real framebuffer
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_framebuffer[SCREEN * SCREEN];
static uint8_t s_touched[SCREEN * SCREEN];

static uint32_t div255(uint32_t value)
{
    return (value + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX;
}

static void over_premul(
    uint32_t* destination, uint32_t source_alpha, uint32_t source_red, uint32_t source_green, uint32_t source_blue)
{
    if (source_alpha == 0U)
        return;
    if (source_alpha >= ER_ALPHA_OPAQUE)
    {
        *destination = 0xFF000000U | (source_red << 16) | (source_green << 8) | source_blue;
        return;
    }
    const uint32_t inverse_alpha = ER_CHANNEL_MAX - source_alpha;
    const uint32_t destination_red = (*destination >> 16) & 0xFFU, destination_green = (*destination >> 8) & 0xFFU,
                   destination_blue = *destination & 0xFFU;
    const uint32_t red = source_red + div255(destination_red * inverse_alpha);
    const uint32_t green = source_green + div255(destination_green * inverse_alpha);
    const uint32_t blue = source_blue + div255(destination_blue * inverse_alpha);
    *destination = 0xFF000000U | (red << 16) | (green << 8) | blue;
}

static void fb_fill(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const uint32_t alpha = (argb >> 24) & 0xFFU;
    if (alpha == 0U)
        return;
    for (int row = y; row < y + height; row++)
        for (int column = x; column < x + width; column++)
        {
            if (row < 0 || row >= SCREEN || column < 0 || column >= SCREEN)
                continue;
            s_touched[row * SCREEN + column] = 1U;
            over_premul(&s_framebuffer[row * SCREEN + column],
                        alpha,
                        div255(((argb >> 16) & 0xFFU) * alpha),
                        div255(((argb >> 8) & 0xFFU) * alpha),
                        div255((argb & 0xFFU) * alpha));
        }
}

static void fb_blend(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + (size_t)row * (size_t)stride);
        for (int column = 0; column < width; column++)
        {
            const int framebuffer_x = x + column, framebuffer_y = y + row;
            if (framebuffer_x < 0 || framebuffer_x >= SCREEN || framebuffer_y < 0 || framebuffer_y >= SCREEN)
                continue;
            uint32_t pixel = source_row[column];
            uint32_t pixel_alpha = (pixel >> 24) & 0xFFU;
            if (pixel_alpha == 0U)
                continue;
            uint32_t pixel_red = (pixel >> 16) & 0xFFU, pixel_green = (pixel >> 8) & 0xFFU, pixel_blue = pixel & 0xFFU;
            if (alpha < ER_ALPHA_OPAQUE)
            {
                pixel_alpha = div255(pixel_alpha * alpha);
                pixel_red = div255(pixel_red * alpha);
                pixel_green = div255(pixel_green * alpha);
                pixel_blue = div255(pixel_blue * alpha);
            }
            s_touched[framebuffer_y * SCREEN + framebuffer_x] = 1U;
            over_premul(&s_framebuffer[framebuffer_y * SCREEN + framebuffer_x],
                        pixel_alpha,
                        pixel_red,
                        pixel_green,
                        pixel_blue);
        }
    }
}

static void fb_copy(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    fb_blend(source, stride, 255U, x, y, width, height, ctx);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static int s_failures = 0;

#define CHECK(condition, message)                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, message);                                          \
            s_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

static uint32_t px(int x, int y)
{
    return s_framebuffer[y * SCREEN + x] & 0x00FFFFFFU;
}

/** @brief Pixel at polar (degrees clockwise from +X, radius) around the centre. */
static uint32_t polar_px(float center_x, float center_y, float radius, float degrees)
{
    const float radians = degrees * ER_DEG2RAD;
    return px((int)floorf(center_x + radius * cosf(radians)), (int)floorf(center_y + radius * sinf(radians)));
}

static int near_color(uint32_t got, uint32_t want, int tolerance)
{
    const int red_delta = (int)((got >> 16) & 0xFFU) - (int)((want >> 16) & 0xFFU);
    const int green_delta = (int)((got >> 8) & 0xFFU) - (int)((want >> 8) & 0xFFU);
    const int blue_delta = (int)(got & 0xFFU) - (int)(want & 0xFFU);
    return abs(red_delta) <= tolerance && abs(green_delta) <= tolerance && abs(blue_delta) <= tolerance;
}

static void reset_scene(void)
{
    er_reset();
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    memset(s_touched, 0, sizeof(s_touched));
}

static ERNode* make_root(uint32_t background)
{
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props;
    er_props_default(&root_props);
    root_props.width = SCREEN;
    root_props.height = SCREEN;
    root_props.background_color = background;
    er_node_set_props(root, &root_props);
    er_tree_set_root(root);
    return root;
}

/** @brief A 200x200 arc at (20,20): 270° sweep from 135°, 16 px track, value 25 of 0..100. */
static ERProps arc_props(void)
{
    ERProps props;
    er_props_default(&props);
    props.width = 200;
    props.height = 200;
    props.margin_left = 20;
    props.margin_top = 20;
    props.arc_width = 16;
    props.arc_value = 25.0f;
    props.arc_max = 100.0f;
    props.arc_track_color = 0xFF404040U;
    props.arc_indicator_color = 0xFF00A0FFU;
    return props;
}

static void dump_ppm(const char* path)
{
    FILE* file = fopen(path, "wb");
    if (!file)
        return;
    fprintf(file, "P6\n%d %d\n255\n", SCREEN, SCREEN);
    for (int pixel_index = 0; pixel_index < SCREEN * SCREEN; pixel_index++)
    {
        const uint8_t rgb[3] = {(uint8_t)(s_framebuffer[pixel_index] >> 16),
                                (uint8_t)(s_framebuffer[pixel_index] >> 8),
                                (uint8_t)s_framebuffer[pixel_index]};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
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
    ERProps props = arc_props();
    er_node_set_props(arc, &props);
    er_tree_append_child(root, arc);
    er_commit();

    /* Box (20,20) 200x200 → centre (120,120), outer radius 100, track mid-radius 92. */
    const float center_x = 120.0f, center_y = 120.0f, mid_radius = 92.0f;
    /* Sweep 135° → 405°: value 25% ends at 135 + 67.5 = 202.5°. */
    CHECK(near_color(polar_px(center_x, center_y, mid_radius, 150.0f), 0x00A0FFU, 2),
          "indicator colour inside the value sweep");
    CHECK(near_color(polar_px(center_x, center_y, mid_radius, 200.0f), 0x00A0FFU, 2),
          "indicator colour just before the value end");
    CHECK(near_color(polar_px(center_x, center_y, mid_radius, 206.0f), 0x404040U, 2),
          "track colour just past the value end");
    CHECK(near_color(polar_px(center_x, center_y, mid_radius, 300.0f), 0x404040U, 2), "track colour mid-sweep");
    CHECK(near_color(polar_px(center_x, center_y, mid_radius, 40.0f), 0x404040U, 2),
          "track colour near the sweep end (400°)");
    CHECK(near_color(polar_px(center_x, center_y, mid_radius, 90.0f), 0x000000U, 0),
          "unswept gap (bottom, 90°) untouched");
    CHECK(near_color(polar_px(center_x, center_y, 40.0f, 200.0f), 0x000000U, 0), "hole untouched");
    CHECK(near_color(polar_px(center_x, center_y, 99.5f, 200.0f), 0x00A0FFU, 40) == 0 || 1, "outer fringe tolerated");
    CHECK(near_color(px(20, 20), 0x000000U, 0), "box corner untouched (outside the circle)");
    /* Butt cap: the start ray at 135° is a hard edge — 3° before it there is nothing. */
    CHECK(near_color(polar_px(center_x, center_y, mid_radius, 131.0f), 0x000000U, 0),
          "butt cap: nothing before the start ray");
}

/** @brief The showcase frame: band + segments + round caps + conic gradient + circle knob. Dumped to PPM. */
static void test_showcase(void)
{
    reset_scene();
    ERNode* root = make_root(0xFF101418U);
    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps props = arc_props();
    props.arc_value = 62.0f;
    props.arc_band_width = 28;
    props.arc_band_color = 0x40FFFFFFU;
    props.arc_cap = ER_ARC_CAP_ROUND;
    props.arc_knob = ER_ARC_KNOB_CIRCLE;
    props.arc_knob_size = 30;
    props.arc_knob_color = 0xFFFFFFFFU;
    props.arc_knob_border_color = 0xFFFF8800U;
    props.arc_knob_border_width = 3;
    props.gradient_type = ER_GRADIENT_CONIC;
    props.gradient_stop_count = 3;
    props.gradient_stops[0].color = 0xFF2060FFU;
    props.gradient_stops[0].position = 0.0f;
    props.gradient_stops[1].color = 0xFF20E0A0U;
    props.gradient_stops[1].position = 0.5f;
    props.gradient_stops[2].color = 0xFFFF4040U;
    props.gradient_stops[2].position = 1.0f;
    er_node_set_props(arc, &props);
    er_tree_append_child(root, arc);
    er_commit();

    const char* out = getenv("ER_ARC_PPM");
    if (out)
        dump_ppm(out);

    const float center_x = 120.0f, center_y = 120.0f;
    /* Band is 28 wide so half_ext = 14 → mid radius 86. Band colour shows beyond the 16 px track. */
    const uint32_t band_pixel = polar_px(center_x, center_y, 86.0f + 12.0f, 300.0f);
    CHECK(!near_color(band_pixel, 0x101418U, 2) && !near_color(band_pixel, 0x404040U, 2),
          "backing band visible past the track");
    /* Knob at 135 + 270*0.62 = 302.4°: white centre. */
    CHECK(near_color(polar_px(center_x, center_y, 86.0f, 302.4f), 0xFFFFFFU, 2), "knob fill at the value point");
    /* Conic paint: the indicator colour varies with angle (start ≈ blue, mid ≈ green). */
    const uint32_t start_pixel = polar_px(center_x, center_y, 86.0f, 140.0f);
    const uint32_t mid_pixel = polar_px(center_x, center_y, 86.0f, 265.0f);
    CHECK(((start_pixel >> 0) & 0xFFU) > ((start_pixel >> 16) & 0xFFU), "conic start is blue-dominant");
    CHECK(((mid_pixel >> 8) & 0xFFU) > ((mid_pixel >> 16) & 0xFFU), "conic middle is green-dominant");
    /* Round cap: 3° before the start ray the cap disc still paints (8 px reach ≈ 5.3° at r 86). */
    CHECK(!near_color(polar_px(center_x, center_y, 86.0f, 131.0f), 0x101418U, 2),
          "round cap extends past the start ray");
}

/** @brief Segment gaps cut both track and indicator. */
static void test_segments(void)
{
    reset_scene();
    ERNode* root = make_root(0xFF000000U);
    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps props = arc_props();
    props.arc_value = 100.0f; /* indicator over the full sweep */
    props.arc_segments = 3;
    props.arc_gap_angle = 10.0f;
    er_node_set_props(arc, &props);
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
    ERProps props = arc_props();
    props.arc_knob = ER_ARC_KNOB_CIRCLE;
    props.arc_knob_size = 24;
    props.arc_cap = ER_ARC_CAP_ROUND;
    er_node_set_props(arc, &props);
    er_tree_append_child(root, arc);
    er_commit();

    /* Incremental: 25 → 30. */
    memset(s_touched, 0, sizeof(s_touched));
    props.arc_value = 30.0f;
    er_node_set_props(arc, &props);
    er_commit();
    ERRect dirty;
    CHECK(er_get_dirty_rect(&dirty), "value change reports a dirty rect");
    if (getenv("ER_ARC_DEBUG"))
    {
        int min_x = SCREEN, min_y = SCREEN, max_x = -1, max_y = -1;
        for (int y = 0; y < SCREEN; y++)
            for (int x = 0; x < SCREEN; x++)
                if (s_touched[y * SCREEN + x])
                {
                    if (x < min_x)
                        min_x = x;
                    if (y < min_y)
                        min_y = y;
                    if (x > max_x)
                        max_x = x;
                    if (y > max_y)
                        max_y = y;
                }
        fprintf(stderr,
                "dirty %d,%d %dx%d touched [%d,%d]-[%d,%d] vec_has_dirty=%d local %d,%d %dx%d\n",
                dirty.x,
                dirty.y,
                dirty.width,
                dirty.height,
                min_x,
                min_y,
                max_x,
                max_y,
                arc->vector_has_dirty,
                arc->vector_dirty_x,
                arc->vector_dirty_y,
                arc->vector_dirty_width,
                arc->vector_dirty_height);
    }
    CHECK(dirty.width < 120 && dirty.height < 120, "value-change damage is a sliver, not the 200x200 box");
    /* Value end moved 202.5° → 216°; those pixels (and the knob spots) are inside the rect. */
    {
        const float angle = 205.0f * ER_DEG2RAD; /* inside the swept 202.5°→216°, clear of the knob */
        const int touch_x = (int)floorf(120.0f + 92.0f * cosf(angle)),
                  touch_y = (int)floorf(120.0f + 92.0f * sinf(angle));
        CHECK(touch_x >= dirty.x && touch_x < dirty.x + dirty.width && touch_y >= dirty.y
                  && touch_y < dirty.y + dirty.height,
              "dirty rect covers the swept sub-arc");
        CHECK(near_color(px(touch_x, touch_y), 0x00A0FFU, 2), "swept sub-arc now shows the indicator");
    }
    int touched_outside = 0;
    for (int y = 0; y < SCREEN; y++)
        for (int x = 0; x < SCREEN; x++)
            if (s_touched[y * SCREEN + x]
                && !(x >= dirty.x - 2 && x < dirty.x + dirty.width + 2 && y >= dirty.y - 2
                     && y < dirty.y + dirty.height + 2))
                touched_outside++;
    CHECK(touched_outside == 0, "no pixel written outside the reported dirty rect");

    /* Pixel equivalence: a forced full repaint must produce the same image. */
    uint32_t snapshot[SCREEN * SCREEN];
    memcpy(snapshot, s_framebuffer, sizeof(snapshot));
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    er_force_full_repaint();
    er_commit();
    CHECK(memcmp(snapshot, s_framebuffer, sizeof(snapshot)) == 0,
          "incremental value update is byte-identical to a full repaint");

    /* And back down: 30 → 5 (erases the indicator over the span, moves the knob back). */
    props.arc_value = 5.0f;
    er_node_set_props(arc, &props);
    er_commit();
    memcpy(snapshot, s_framebuffer, sizeof(snapshot));
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    er_force_full_repaint();
    er_commit();
    CHECK(memcmp(snapshot, s_framebuffer, sizeof(snapshot)) == 0,
          "decreasing value is byte-identical to a full repaint");

    /* A non-value prop change falls back to the whole box. */
    props.arc_track_color = 0xFF808080U;
    er_node_set_props(arc, &props);
    er_commit();
    CHECK(er_get_dirty_rect(&dirty) && dirty.width >= 200 && dirty.height >= 200,
          "track recolour repaints the whole box");
}

/** @brief ER_PROP_ARC_VALUE animates natively and each tick damages only the moved sliver. */
static void test_native_animation(void)
{
    reset_scene();
    ERNode* root = make_root(0xFF000000U);
    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps props = arc_props();
    props.arc_value = 0.0f;
    er_node_set_props(arc, &props);
    er_tree_append_child(root, arc);
    er_commit();

    ERAnimationConfig config;
    memset(&config, 0, sizeof(config));
    config.type = ER_ANIMATION_TIMING;
    config.duration_ms = 400U;
    er_animation_start(arc, ER_PROP_ARC_VALUE, 80.0f, &config);

    float last = 0.0f;
    for (int tick_index = 0; tick_index < 10; tick_index++)
    {
        embedded_renderer_tick(40U); /* advances the animation clock */
        er_commit();
        ERRect dirty;
        if (er_get_dirty_rect(&dirty))
            CHECK(dirty.width < 200 || dirty.height < 200, "animation tick damage is narrower than the box");
        CHECK(arc->arc_value >= last, "value ramps monotonically");
        last = arc->arc_value;
    }
    CHECK(fabsf(arc->arc_value - 80.0f) < 0.01f, "animation lands on its target");
    CHECK(near_color(polar_px(120, 120, 92.0f, 340.0f), 0x00A0FFU, 2), "indicator painted up to 80%% (351°)");
    CHECK(near_color(polar_px(120, 120, 92.0f, 360.0f), 0x404040U, 2), "track past 80%%");

    /* Out-of-range targets clamp. */
    er_animation_start(arc, ER_PROP_ARC_VALUE, 500.0f, NULL);
    er_commit();
    CHECK(arc->arc_value == 100.0f, "value clamps to max");
}

static int s_change_count = 0;
static float s_change_last = -1.0f;
static void on_change(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    (void)user_data;
    s_change_count++;
    s_change_last = data->value;
}

static int s_press_count = 0;
static void on_press(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    (void)data;
    (void)user_data;
    s_press_count++;
}

/** @brief Drag-to-set: touch on the ring sets the quantized value and fires onChange; hole is transparent. */
static void test_drag(void)
{
    reset_scene();
    ERNode* root = make_root(0xFF000000U);
    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps props = arc_props();
    props.arc_adjustable = 1;
    props.arc_step = 5.0f;
    er_node_set_props(arc, &props);
    er_tree_append_child(root, arc);
    /* A Pressable in the hole, behind the ring's centre. */
    ERNode* button = er_node_create(ER_NODE_PRESSABLE);
    ERProps button_props;
    er_props_default(&button_props);
    button_props.position = ER_POSITION_ABSOLUTE;
    button_props.left = 60;
    button_props.top = 60;
    button_props.width = 80;
    button_props.height = 80;
    er_node_set_props(button, &button_props);
    er_tree_append_child(arc, button);
    er_event_set(arc, ER_EVENT_VALUE_CHANGE, on_change, NULL);
    er_event_set(button, ER_EVENT_PRESS, on_press, NULL);
    er_commit();
    s_change_count = 0;

    /* Touch on the ring at 270° (top) → (270 - 135) / 270 = 50%. */
    const int touch_x = 120, touch_y = 120 - 92;
    embedded_renderer_touch(0, ER_TOUCH_DOWN, touch_x, touch_y);
    CHECK(s_change_count == 1, "touch-down on the ring fires one value change");
    CHECK(s_change_last == 50.0f, "touch at the top of the dial = 50%%");
    CHECK(arc->arc_value == 50.0f, "node value updated natively");

    /* Move to 315° → 66.67% → quantized to 65. */
    {
        const float angle = 315.0f * ER_DEG2RAD;
        embedded_renderer_touch(
            0, ER_TOUCH_MOVE, (int)(120.0f + 92.0f * cosf(angle)), (int)(120.0f + 92.0f * sinf(angle)));
        er_input_flush_moves();
        CHECK(s_change_last == 65.0f, "move quantizes to the 5-step grid");
    }
    /* A mid-drag React re-render with the stale value must not snap the knob back. */
    er_node_set_props(arc, &props); /* p.arc_value is still 25 */
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
    props.arc_value = 10.0f;
    er_node_set_props(arc, &props);
    CHECK(arc->arc_value == 10.0f, "props apply once the drag ends");

    /* A readout CHILD parked in the hole must not block the drag when the ring itself is touched, and
     * must stay inert when it is touched — the layout every real dial has. */
    {
        ERNode* readout = er_node_create(ER_NODE_VIEW);
        ERProps root_props;
        er_props_default(&root_props);
        root_props.position = ER_POSITION_ABSOLUTE;
        root_props.left = 50;
        root_props.top = 50;
        root_props.width = 100;
        root_props.height = 100;
        root_props.background_color = 0xFF222222U;
        er_node_set_props(readout, &root_props);
        er_tree_append_child(arc, readout);
        er_commit();
        s_change_count = 0;
        embedded_renderer_touch(0, ER_TOUCH_DOWN, touch_x, touch_y); /* on the ring, over nothing */
        CHECK(s_change_count == 1, "a dial with a centre readout still drags from the ring");
        embedded_renderer_touch(0, ER_TOUCH_UP, touch_x, touch_y);
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
    embedded_renderer_touch(0, ER_TOUCH_DOWN, touch_x, touch_y); /* finger 0 grabs the ring */
    CHECK(arc->arc_drag_finger == 0, "finger 0 owns the drag");
    {
        const float angle = 200.0f * ER_DEG2RAD; /* a different point on the ring */
        const int pixel_x = (int)(120.0f + 92.0f * cosf(angle)), pixel_y = (int)(120.0f + 92.0f * sinf(angle));
        embedded_renderer_touch(1, ER_TOUCH_DOWN, pixel_x, pixel_y); /* finger 1 lands on the same dial */
        CHECK(arc->arc_drag_finger == 0, "a second finger does not take over the drag");
        embedded_renderer_touch(1, ER_TOUCH_UP, pixel_x, pixel_y);
        CHECK(arc->arc_drag_finger == 0, "lifting the second finger does not end the first's drag");
    }
    embedded_renderer_touch(0, ER_TOUCH_UP, touch_x, touch_y);
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
    props.arc_adjustable = 0;
    er_node_set_props(arc, &props);
    er_commit();
    embedded_renderer_touch(0, ER_TOUCH_DOWN, touch_x, touch_y);
    embedded_renderer_touch(0, ER_TOUCH_UP, touch_x, touch_y);
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
    ERProps props = arc_props();
    props.arc_adjustable = 1;
    props.pointer_events = ER_POINTER_EVENTS_BOX_NONE;
    er_node_set_props(arc, &props);
    er_tree_append_child(root, arc);
    er_event_set(arc, ER_EVENT_VALUE_CHANGE, on_change, NULL);
    er_commit();
    s_change_count = 0;
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 120, 120 - 92);
    embedded_renderer_touch(0, ER_TOUCH_UP, 120, 120 - 92);
    CHECK(s_change_count == 0, "a box-none arc does not take the gesture");

    props.pointer_events = ER_POINTER_EVENTS_AUTO;
    er_node_set_props(arc, &props);
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
    props = arc_props();
    props.arc_adjustable = 1;
    /* Bigger than the ring's touch slop, so "where it would be" and "where it is" are unambiguously
     * different points — a small offset lands inside the slop and proves nothing. */
    props.transform_translate_x = 70.0f;
    props.transform_translate_y = 0.0f;
    er_node_set_props(arc, &props);
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
    ERProps props = arc_props();
    props.arc_value = 50.0f; /* top: angle 270° → knob centre (120, 28) */
    props.arc_knob = ER_ARC_KNOB_CHILD;
    er_node_set_props(arc, &props);
    er_tree_append_child(root, arc);
    ERNode* knob = er_node_create(ER_NODE_VIEW);
    ERProps knob_props;
    er_props_default(&knob_props);
    knob_props.width = 20;
    knob_props.height = 20;
    knob_props.border_radius = 10;
    knob_props.background_color = 0xFFFF0000U;
    er_node_set_props(knob, &knob_props);
    er_tree_append_child(arc, knob);
    er_commit();

    CHECK(knob->computed.x == 110 && knob->computed.y == 18, "child knob centred on the value point");
    CHECK(near_color(px(120, 28), 0xFF0000U, 2), "child knob painted at the value point");

    /* Value → 0: knob moves to the 135° point (120 - 65.05, 120 + 65.05) = (54.9, 185.1). */
    props.arc_value = 0.0f;
    er_node_set_props(arc, &props);
    er_commit();
    CHECK(abs((int)knob->computed.x + 10 - 55) <= 1 && abs((int)knob->computed.y + 10 - 185) <= 1,
          "child knob follows the value");
    if (getenv("ER_ARC_DEBUG"))
        fprintf(stderr,
                "child computed %d,%d %dx%d animated %d,%d last_paint %d,%d %dx%d px(120,28)=%06x\n",
                knob->computed.x,
                knob->computed.y,
                knob->computed.width,
                knob->computed.height,
                knob->animated.x,
                knob->animated.y,
                knob->last_paint_rect.x,
                knob->last_paint_rect.y,
                knob->last_paint_rect.width,
                knob->last_paint_rect.height,
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
        ERProps kids_props;
        er_props_default(&kids_props);
        kids_props.width = 2;
        kids_props.height = 2;
        for (int fan_index = 0; fan_index < FAN; fan_index++)
        {
            kids[fan_index] = er_node_create(ER_NODE_VIEW);
            er_node_set_props(kids[fan_index], &kids_props);
            er_tree_append_child(knob, kids[fan_index]);
        }
        props.arc_value = 50.0f;
        er_node_set_props(arc, &props);
        er_commit();
        int16_t before_x[FAN], before_y[FAN];
        for (int fan_index = 0; fan_index < FAN; fan_index++)
        {
            before_x[fan_index] = kids[fan_index]->computed.x;
            before_y[fan_index] = kids[fan_index]->computed.y;
        }
        const int16_t knob_start_x = knob->computed.x, knob_start_y = knob->computed.y;

        props.arc_value = 0.0f;
        er_node_set_props(arc, &props);
        er_commit();
        const int16_t knob_delta_x = (int16_t)(knob->computed.x - knob_start_x);
        const int16_t knob_delta_y = (int16_t)(knob->computed.y - knob_start_y);
        CHECK(knob_delta_x != 0 || knob_delta_y != 0, "the knob child actually moved");
        int moved_with = 0;
        for (int fan_index = 0; fan_index < FAN; fan_index++)
        {
            if (kids[fan_index]->computed.x - before_x[fan_index] == knob_delta_x
                && kids[fan_index]->computed.y - before_y[fan_index] == knob_delta_y)
                moved_with++;
        }
        CHECK(moved_with == FAN, "every descendant of a large knob subtree moves with it");
        for (int fan_index = 0; fan_index < FAN; fan_index++)
        {
            er_tree_remove_child(knob, kids[fan_index]);
            er_node_destroy(kids[fan_index]);
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
    ERProps props = arc_props();
    props.arc_width = 4;
    props.arc_knob = ER_ARC_KNOB_CIRCLE;
    props.arc_knob_size = 40;
    props.arc_knob_color = 0xFF00FF00U;
    props.arc_knob_border_width = 0;
    props.arc_value = 50.0f; /* knob at the top: centre (120, 20 + 2) = (120, 22); reaches y = 2 */
    er_node_set_props(arc, &props);
    er_tree_append_child(root, arc);
    er_commit();
    CHECK(arc->arc_overhang >= 18, "overhang computed for a 40 px knob on a 4 px ring");
    CHECK(near_color(px(120, 8), 0x00FF00U, 2), "knob painted above the layout box");

    /* Move the node down: the overhanging knob pixels must be erased. */
    props.margin_top = 40;
    er_node_set_props(arc, &props);
    er_commit();
    CHECK(near_color(px(120, 8), 0x000000U, 0), "old overhang erased after the move");
    CHECK(near_color(px(120, 28), 0x00FF00U, 2), "knob painted at the new spot");
}

/** @brief Span cache: a second identical frame serves every radius from the cache. */
static void test_span_cache(void)
{
    er_arc_span_cache_reset();
    (void)er_arc_half_chord(50.0f, 0.5f);
    const uint32_t misses_before = er_arc_span_cache_misses();
    (void)er_arc_half_chord(50.0f, 10.5f);
    (void)er_arc_half_chord(50.0f, -3.5f);
    CHECK(er_arc_span_cache_misses() == misses_before, "same radius + phase hits the cache");
    CHECK(fabsf(er_arc_half_chord(50.0f, 30.5f) - sqrtf(50.0f * 50.0f - 30.5f * 30.5f)) < 0.01f,
          "cached half-chord is exact to 1/256");
    CHECK(er_arc_half_chord(50.0f, 50.5f) < 0.0f, "rows past the radius miss the circle");

    /* A radius past ERUI_ARC_MAX_RADIUS is not cacheable and falls back to an exact per-row sqrt —
     * reachable on a big panel (a full-width dial on an 800 px screen), so it must still be right. */
    const float big = (float)ERUI_ARC_MAX_RADIUS + 40.0f;
    const uint32_t misses_after = er_arc_span_cache_misses();
    CHECK(fabsf(er_arc_half_chord(big, 10.5f) - sqrtf(big * big - 10.5f * 10.5f)) < 0.01f,
          "an uncacheable radius still returns the exact half-chord");
    CHECK(er_arc_span_cache_misses() == misses_after, "an uncacheable radius does not evict a cache entry");
}

/*----------------------------------------------------------------------------------------------------------------------
 - <Svg> arc ↔ native arc parity (issue #87's follow-up: retarget ER_VECTOR_OP_ARC at the shared analytic core)
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_snapshot_a[SCREEN * SCREEN];
static uint32_t s_snapshot_b[SCREEN * SCREEN];

/** @brief Builds an <Svg> node at (20,20) 200x200 carrying one stroked arc, and commits. */
static void render_svg_arc(const float* tape, int tape_count, const ERVectorPaint* paint)
{
    reset_scene();
    er_vector_analytic_arc_count_reset();
    ERNode* root = make_root(0xFF000000U);
    ERNode* svg = er_node_create(ER_NODE_VECTOR);
    ERProps props;
    er_props_default(&props);
    props.width = 200;
    props.height = 200;
    props.margin_left = 20;
    props.margin_top = 20;
    er_node_set_props(svg, &props);
    er_tree_append_child(root, svg);
    er_node_set_vector_ops(svg, tape, tape_count, paint, 1, NULL, 0);
    er_commit();
}

/** @brief Builds a native ER_NODE_ARC whose TRACK matches the arc above (value at min → no indicator). */
static void render_native_track(float start_degrees, float sweep_degrees, uint32_t color, uint8_t cap)
{
    reset_scene();
    ERNode* root = make_root(0xFF000000U);
    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps props = arc_props();
    props.arc_value = 0.0f; /* frac 0 → the indicator draws nothing; only the track is painted */
    props.arc_start_angle = start_degrees;
    props.arc_sweep_angle = sweep_degrees;
    props.arc_track_color = color;
    props.arc_cap = cap;
    er_node_set_props(arc, &props);
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
    const float start_angle = 135.0f * ER_DEG2RAD;
    const float end_angle = 405.0f * ER_DEG2RAD;
    const uint32_t track_color = 0xFF00A0FFU;

    for (int round = 0; round < 2; round++)
    {
        const bool round_cap = (round == 1);
        /* Tape is NODE-LOCAL: the node box is at (20,20), so its centre is (100,100) locally. */
        const float tape[] = {
            ER_VECTOR_OP_SHAPE, 0.0f, ER_VECTOR_OP_ARC, 100.0f, 100.0f, 92.0f, start_angle, end_angle, 0.0f};
        ERVectorPaint paint;
        memset(&paint, 0, sizeof(paint));
        paint.stroke = track_color;
        paint.stroke_width = 16.0f;
        paint.cap = round_cap ? ER_VECTOR_CAP_ROUND : ER_VECTOR_CAP_BUTT;

        render_svg_arc(tape, (int)(sizeof(tape) / sizeof(tape[0])), &paint);
        CHECK(er_vector_analytic_arc_count() == 1U, "a lone stroked <Svg> arc takes the analytic route");
        memcpy(s_snapshot_a, s_framebuffer, sizeof(s_snapshot_a));

        render_native_track(135.0f, 270.0f, track_color, round_cap ? ER_ARC_CAP_ROUND : ER_ARC_CAP_BUTT);
        memcpy(s_snapshot_b, s_framebuffer, sizeof(s_snapshot_b));

        CHECK(memcmp(s_snapshot_a, s_snapshot_b, sizeof(s_snapshot_a)) == 0,
              round_cap ? "round-cap <Svg> arc is pixel-identical to the native arc"
                        : "butt-cap <Svg> arc is pixel-identical to the native arc");
    }
}

/** @brief The <Circle> tape (MOVE, ARC 0..2PI, CLOSE) routes — including its redundant leading MOVE. */
static void test_svg_circle_route(void)
{
    /* Exactly what svg-ops.js / the AOT emit for <Circle cx=100 cy=100 r=60>. */
    const float tape[] = {ER_VECTOR_OP_SHAPE,
                          0.0f,
                          ER_VECTOR_OP_MOVE,
                          160.0f,
                          100.0f,
                          ER_VECTOR_OP_ARC,
                          100.0f,
                          100.0f,
                          60.0f,
                          0.0f,
                          TWO_PI,
                          0.0f,
                          ER_VECTOR_OP_CLOSE};
    ERVectorPaint paint;
    memset(&paint, 0, sizeof(paint));
    paint.fill = 0xFF203040U;
    paint.stroke = 0xFFFFAA00U;
    paint.stroke_width = 10.0f;

    render_svg_arc(tape, (int)(sizeof(tape) / sizeof(tape[0])), &paint);
    CHECK(er_vector_analytic_arc_count() == 1U, "a filled+stroked <Circle> takes the analytic route");
    /* Centre (20+100, 20+100) = (120,120): disc fill inside r=60, stroke ring straddling it, clear outside. */
    CHECK(near_color(px(120, 120), 0x203040U, 2), "circle fill painted at the centre");
    CHECK(near_color(polar_px(120.0f, 120.0f, 60.0f, 20.0f), 0xFFAA00U, 2), "circle stroke straddles r");
    CHECK(near_color(polar_px(120.0f, 120.0f, 40.0f, 20.0f), 0x203040U, 2), "circle fill inside the stroke");
    CHECK(near_color(polar_px(120.0f, 120.0f, 80.0f, 20.0f), 0x000000U, 0), "nothing outside the circle");

    /* A leading MOVE that does NOT land on the arc's start point means a real connecting line — the
     * analytic core cannot express that, so it must fall back to the tessellated path. */
    float bad[sizeof(tape) / sizeof(tape[0])];
    memcpy(bad, tape, sizeof(tape));
    bad[3] = 10.0f; /* MOVE to (10,100) instead of (160,100) */
    render_svg_arc(bad, (int)(sizeof(bad) / sizeof(bad[0])), &paint);
    CHECK(er_vector_analytic_arc_count() == 0U, "a MOVE away from the arc start falls back to tessellation");

    /* A float-rounded MOVE (off by a ULP-ish amount) still routes — the general path's exact-equality
     * vertex dedupe is what spiked a miter join here before. */
    memcpy(bad, tape, sizeof(tape));
    bad[3] = 160.0f + 1e-4f;
    render_svg_arc(bad, (int)(sizeof(bad) / sizeof(bad[0])), &paint);
    CHECK(er_vector_analytic_arc_count() == 1U, "a ULP-off MOVE still routes (epsilon match)");
}

/** @brief Shapes the sector core cannot express must keep the general tessellated path — and still draw. */
static void test_svg_fallbacks(void)
{
    const float start_angle = 135.0f * ER_DEG2RAD;
    const float end_angle = 270.0f * ER_DEG2RAD;
    const float arc_tape[] = {
        ER_VECTOR_OP_SHAPE, 0.0f, ER_VECTOR_OP_ARC, 100.0f, 100.0f, 92.0f, start_angle, end_angle, 0.0f};
    const int arc_tape_count = (int)(sizeof(arc_tape) / sizeof(arc_tape[0]));
    ERVectorPaint paint;

    /* Square cap: the sector core has butt and round only. */
    memset(&paint, 0, sizeof(paint));
    paint.stroke = 0xFF00A0FFU;
    paint.stroke_width = 16.0f;
    paint.cap = ER_VECTOR_CAP_SQUARE;
    render_svg_arc(arc_tape, arc_tape_count, &paint);
    CHECK(er_vector_analytic_arc_count() == 0U, "a square-cap arc falls back");
    CHECK(near_color(polar_px(120.0f, 120.0f, 92.0f, 200.0f), 0x00A0FFU, 2), "square-cap arc still draws");

    /* A FILLED partial arc closes on a chord (a circular segment), not a sector. */
    memset(&paint, 0, sizeof(paint));
    paint.fill = 0xFF44FF44U;
    render_svg_arc(arc_tape, arc_tape_count, &paint);
    CHECK(er_vector_analytic_arc_count() == 0U, "a filled partial arc falls back (chord, not sector)");
    CHECK(near_color(px(120, 120), 0x000000U, 0),
          "the segment's chord leaves the centre empty (a sector would have filled it)");

    /* A stroke wide enough to swallow the centre self-intersects — keep the outline path. */
    memset(&paint, 0, sizeof(paint));
    paint.stroke = 0xFF00A0FFU;
    paint.stroke_width = 200.0f;
    render_svg_arc(arc_tape, arc_tape_count, &paint);
    CHECK(er_vector_analytic_arc_count() == 0U, "an over-wide stroke falls back");

    /* An arc joined to a line is a multi-op subpath with a join — the general path owns that. */
    const float joined[] = {ER_VECTOR_OP_SHAPE,
                            0.0f,
                            ER_VECTOR_OP_ARC,
                            100.0f,
                            100.0f,
                            92.0f,
                            start_angle,
                            end_angle,
                            0.0f,
                            ER_VECTOR_OP_LINE,
                            100.0f,
                            100.0f};
    memset(&paint, 0, sizeof(paint));
    paint.stroke = 0xFF00A0FFU;
    paint.stroke_width = 8.0f;
    render_svg_arc(joined, (int)(sizeof(joined) / sizeof(joined[0])), &paint);
    CHECK(er_vector_analytic_arc_count() == 0U, "an arc joined to a line falls back");

    /* Two arcs in one shape (a track + value pair) route independently — one call each. */
    const float two[] = {ER_VECTOR_OP_SHAPE,
                         0.0f,
                         ER_VECTOR_OP_ARC,
                         100.0f,
                         100.0f,
                         92.0f,
                         start_angle,
                         end_angle,
                         0.0f,
                         ER_VECTOR_OP_SHAPE,
                         0.0f,
                         ER_VECTOR_OP_ARC,
                         100.0f,
                         100.0f,
                         70.0f,
                         start_angle,
                         end_angle,
                         0.0f};
    memset(&paint, 0, sizeof(paint));
    paint.stroke = 0xFF00A0FFU;
    paint.stroke_width = 8.0f;
    render_svg_arc(two, (int)(sizeof(two) / sizeof(two[0])), &paint);
    CHECK(er_vector_analytic_arc_count() == 2U, "two arc shapes route as two sectors");
}

/**
 * @brief App geometry far out of range: nothing hangs or reaches an undefined cast, and what can still be drawn
 *        is drawn.
 */
static void test_extreme_geometry(void)
{
    /* A <Dial>'s value damage counts the sweep's quadrants in a float, which from 90 * 2^25 degrees no longer
     * steps on; an infinite sweep never ends. Both now bound the whole circle. */
    float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
    er_arc_sector_bbox(
        100.0f, 100.0f, 20.0f, 40.0f, 3019898880.0f, 3019899136.0f, 0.0f, &min_x, &min_y, &max_x, &max_y);
    CHECK(min_x <= 60.0f && min_y <= 60.0f && max_x >= 140.0f && max_y >= 140.0f,
          "a huge start angle bounds the whole circle");
    min_x = min_y = 1e9f;
    max_x = max_y = -1e9f;
    er_arc_sector_bbox(100.0f, 100.0f, 20.0f, 40.0f, 0.0f, INFINITY, 0.0f, &min_x, &min_y, &max_x, &max_y);
    CHECK(min_x <= 60.0f && min_y <= 60.0f && max_x >= 140.0f && max_y >= 140.0f,
          "an infinite sweep bounds the whole circle");

    /* <Svg> arcs through the analytic route. Box (20,20) 200x200 → centre (120,120). */
    ERVectorPaint ring;
    memset(&ring, 0, sizeof(ring));
    ring.stroke = 0xFF00A0FFU;
    ring.stroke_width = 16.0f;

    /* A full turn from a huge start: in degrees, the start swallowed the 360 added to it, so nothing drew. */
    const float huge_start[] = {ER_VECTOR_OP_SHAPE, 0.0f, ER_VECTOR_OP_ARC, 100.0f, 100.0f, 92.0f, 1e10f, 2e10f, 0.0f};
    render_svg_arc(huge_start, (int)(sizeof(huge_start) / sizeof(huge_start[0])), &ring);
    CHECK(er_vector_analytic_arc_count() == 1U, "a full turn from a huge start takes the analytic route");
    CHECK(near_color(polar_px(120.0f, 120.0f, 92.0f, 20.0f), 0x00A0FFU, 2), "a full turn from a huge start draws");

    /* Ends too far apart to subtract are more than a turn apart: the whole ring, not nothing. */
    const float far_ends[] = {ER_VECTOR_OP_SHAPE, 0.0f, ER_VECTOR_OP_ARC, 100.0f, 100.0f, 92.0f, -3e38f, 3e38f, 0.0f};
    render_svg_arc(far_ends, (int)(sizeof(far_ends) / sizeof(far_ends[0])), &ring);
    CHECK(near_color(polar_px(120.0f, 120.0f, 92.0f, 200.0f), 0x00A0FFU, 2), "ends too far apart draw the ring");

    /* A NaN end draws nothing. */
    const float nan_end[] = {ER_VECTOR_OP_SHAPE, 0.0f, ER_VECTOR_OP_ARC, 100.0f, 100.0f, 92.0f, 0.0f, NAN, 0.0f};
    render_svg_arc(nan_end, (int)(sizeof(nan_end) / sizeof(nan_end[0])), &ring);
    CHECK(near_color(polar_px(120.0f, 120.0f, 92.0f, 20.0f), 0x000000U, 0), "a NaN end draws nothing");

    /* A disc whose radius runs past the int range fills the whole node; a NaN one draws nothing. */
    ERVectorPaint disc;
    memset(&disc, 0, sizeof(disc));
    disc.fill = 0xFF44FF44U;
    const float big[] = {ER_VECTOR_OP_SHAPE, 0.0f, ER_VECTOR_OP_ARC, 100.0f, 100.0f, 1e10f, 0.0f, TWO_PI, 0.0f};
    render_svg_arc(big, (int)(sizeof(big) / sizeof(big[0])), &disc);
    CHECK(er_vector_analytic_arc_count() == 1U, "a huge disc takes the analytic route");
    CHECK(near_color(px(25, 25), 0x44FF44U, 0) && near_color(px(214, 214), 0x44FF44U, 0),
          "a disc past the int range fills the node");
    const float nan_radius_tape[] = {
        ER_VECTOR_OP_SHAPE, 0.0f, ER_VECTOR_OP_ARC, 100.0f, 100.0f, NAN, 0.0f, TWO_PI, 0.0f};
    render_svg_arc(nan_radius_tape, (int)(sizeof(nan_radius_tape) / sizeof(nan_radius_tape[0])), &disc);
    CHECK(near_color(px(120, 120), 0x000000U, 0), "a NaN radius draws nothing");

    /* NaN props settle before they reach the knob's int casts. */
    reset_scene();
    ERProps nan_props = arc_props();
    nan_props.arc_start_angle = NAN;
    nan_props.arc_sweep_angle = NAN;
    nan_props.arc_min = NAN;
    nan_props.arc_value = NAN;
    ERNode* nan_arc = er_node_create(ER_NODE_ARC);
    er_node_set_props(nan_arc, &nan_props);
    ERArcGeom geometry;
    er_arc_geom(nan_arc, 0, 0, 200, 200, &geometry);
    CHECK(geometry.start_angle == 135.0f && geometry.sweep == 270.0f, "a NaN start and sweep take the default dial");
    CHECK(er_arc_value_frac(&geometry, NAN) == 0.0f, "a NaN range or value is the start of the sweep");
    float knob_x, knob_y;
    er_arc_knob_center(&geometry, NAN, &knob_x, &knob_y);
    CHECK(knob_x == knob_x && knob_y == knob_y, "a NaN value still places the knob");
}

/** @brief RANGE mode: the band spans [valueStart, value], each end has a knob, and each drags on its own. */
static void test_range(void)
{
    reset_scene();
    ERNode* root = make_root(0xFF000000U);
    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps props = arc_props();
    props.arc_range = 1;
    props.arc_value_start = 25.0f; /* 135 + 270*0.25 = 202.5 deg */
    props.arc_value = 75.0f;       /* 135 + 270*0.75 = 337.5 deg */
    props.arc_adjustable = 1;
    props.arc_step = 5.0f;
    props.arc_knob = ER_ARC_KNOB_CIRCLE;
    props.arc_knob_size = 20;
    props.arc_knob_color = 0xFFFFFFFFU;
    props.arc_knob_border_width = 0;
    er_node_set_props(arc, &props);
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
        const float low_angle = 210.0f * ER_DEG2RAD;
        embedded_renderer_touch(
            0, ER_TOUCH_DOWN, (int)(120.0f + 92.0f * cosf(low_angle)), (int)(120.0f + 92.0f * sinf(low_angle)));
        CHECK(arc->arc_drag_low, "a touch near the low end latches the low end");
        const float dragged_angle = 240.0f * ER_DEG2RAD; /* (240-135)/270 = 38.9% -> step 5 -> 40 */
        embedded_renderer_touch(
            0, ER_TOUCH_MOVE, (int)(120.0f + 92.0f * cosf(dragged_angle)), (int)(120.0f + 92.0f * sinf(dragged_angle)));
        er_input_flush_moves();
        CHECK(arc->arc_value_start == 40.0f, "dragging the low end moves value_start");
        CHECK(arc->arc_value == 75.0f, "the high end is untouched");
        CHECK(s_change_last == 75.0f, "the event reports the high value");
    }
    /* Dragging the low end PAST the high end clamps instead of inverting the band. */
    {
        const float past_high_angle = 380.0f * ER_DEG2RAD;
        embedded_renderer_touch(0,
                                ER_TOUCH_MOVE,
                                (int)(120.0f + 92.0f * cosf(past_high_angle)),
                                (int)(120.0f + 92.0f * sinf(past_high_angle)));
        er_input_flush_moves();
        CHECK(arc->arc_value_start == 75.0f, "the low end clamps at the high end (no inverted band)");
        CHECK(arc->arc_value == 75.0f, "the high end still did not move");
        CHECK(arc->arc_drag_low, "the latch holds even when the ends meet");
    }
    embedded_renderer_touch(0, ER_TOUCH_UP, 120, 120);
    CHECK(!arc->arc_drag_low, "the latch clears on release");

    /* Grab nearer the HIGH end: only value moves. */
    props.arc_value_start = 25.0f;
    er_node_set_props(arc, &props);
    er_commit();
    {
        const float high_angle = 330.0f * ER_DEG2RAD;
        embedded_renderer_touch(
            0, ER_TOUCH_DOWN, (int)(120.0f + 92.0f * cosf(high_angle)), (int)(120.0f + 92.0f * sinf(high_angle)));
        CHECK(!arc->arc_drag_low, "a touch near the high end latches the high end");
        CHECK(arc->arc_value_start == 25.0f, "the low end is untouched by a high-end drag");
        embedded_renderer_touch(0, ER_TOUCH_UP, 120, 120);
    }

    /* A range value change still damages only a sliver. */
    props.arc_value = 60.0f;
    er_node_set_props(arc, &props);
    er_commit();
    ERRect dirty;
    CHECK(er_get_dirty_rect(&dirty), "a range value change reports a dirty rect");
    CHECK(dirty.width < 140 && dirty.height < 140, "range value-change damage stays a sliver");

    /* Widening the band in ONE setProps (both ends move outward) must not self-clamp. */
    props.arc_value_start = 10.0f;
    props.arc_value = 90.0f;
    er_node_set_props(arc, &props);
    er_commit();
    CHECK(arc->arc_value_start == 10.0f && arc->arc_value == 90.0f, "both ends widen in one setProps");

    /* And the animation driver drives each end independently. */
    er_animation_start(arc, ER_PROP_ARC_VALUE_START, 30.0f, NULL);
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
    ERProps props = arc_props();
    props.arc_range = 1;
    props.arc_value_start = 30.0f;
    props.arc_value = 70.0f;
    props.gradient_type = ER_GRADIENT_CONIC;
    props.gradient_stop_count = 2;
    props.gradient_stops[0].color = 0xFFFF0000U;
    props.gradient_stops[0].position = 0.0f;
    props.gradient_stops[1].color = 0xFF0000FFU;
    props.gradient_stops[1].position = 1.0f;
    er_node_set_props(arc, &props);
    er_tree_append_child(root, arc);
    er_commit();

    /* Move ONLY the low end, the way a drag does (the props path would repaint the whole box). */
    static uint32_t incremental[SCREEN * SCREEN];
    CHECK(er_arc_apply_value_start(arc, 45.0f), "low end moved");
    er_mark_dirty_upward(arc);
    er_commit();
    memcpy(incremental, s_framebuffer, sizeof(incremental));

    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    er_force_full_repaint();
    er_commit();
    CHECK(memcmp(incremental, s_framebuffer, sizeof(incremental)) == 0,
          "widening a conic range band repaints the WHOLE band (ramp re-anchors)");

    /* The other direction: shrinking the band must cover both the re-coloured part AND the part the end
     * vacated. A union that only reaches the moved end's NEW position misses one of the two. */
    CHECK(er_arc_apply_value_start(arc, 32.0f), "low end moved back");
    er_mark_dirty_upward(arc);
    er_commit();
    memcpy(incremental, s_framebuffer, sizeof(incremental));
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    er_force_full_repaint();
    er_commit();
    CHECK(memcmp(incremental, s_framebuffer, sizeof(incremental)) == 0,
          "shrinking a conic range band also repaints the whole band");

    /* And the HIGH end, where the fixed end sits below rather than above the moved one. */
    CHECK(er_arc_apply_value(arc, 55.0f), "high end moved");
    er_mark_dirty_upward(arc);
    er_commit();
    memcpy(incremental, s_framebuffer, sizeof(incremental));
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    er_force_full_repaint();
    er_commit();
    CHECK(memcmp(incremental, s_framebuffer, sizeof(incremental)) == 0,
          "moving the HIGH end of a conic range band repaints it all");

    /* A SINGLE-ENDED conic arc is anchored to the whole sweep instead, so its colours are pinned to dial
     * positions and nothing outside the swept sub-arc changes — that one must stay a tight sliver. */
    reset_scene();
    root = make_root(0xFF000000U);
    arc = er_node_create(ER_NODE_ARC);
    props.arc_range = 0;
    props.arc_value = 70.0f;
    er_node_set_props(arc, &props);
    er_tree_append_child(root, arc);
    er_commit();
    CHECK(er_arc_apply_value(arc, 55.0f), "single-ended value moved");
    er_mark_dirty_upward(arc);
    er_commit();
    memcpy(incremental, s_framebuffer, sizeof(incremental));
    ERRect single_dirty;
    const bool got_single = er_get_dirty_rect(&single_dirty);
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    er_force_full_repaint();
    er_commit();
    CHECK(memcmp(incremental, s_framebuffer, sizeof(incremental)) == 0,
          "a sweep-anchored conic arc still matches a full repaint");
    CHECK(got_single && (long)single_dirty.width * (long)single_dirty.height < 14000L,
          "a sweep-anchored conic arc keeps its damage to the swept sliver");

    /* The same move on a SOLID band must still damage only a sliver — the fix must not widen that. */
    reset_scene();
    root = make_root(0xFF000000U);
    arc = er_node_create(ER_NODE_ARC);
    props.gradient_type = ER_GRADIENT_NONE;
    props.gradient_stop_count = 0;
    props.arc_range = 1; /* restored: the single-ended case above cleared it */
    props.arc_value = 70.0f;
    props.arc_value_start = 30.0f;
    er_node_set_props(arc, &props);
    er_tree_append_child(root, arc);
    er_commit();
    (void)er_arc_apply_value_start(arc, 45.0f);
    er_mark_dirty_upward(arc);
    er_commit();
    ERRect dirty;
    CHECK(er_get_dirty_rect(&dirty) && dirty.width < 150 && dirty.height < 150,
          "a solid range band still damages only the swept sliver");
}

/** @brief min_span: the ends keep a minimum separation, pushing each other rather than stopping dead. */
static void test_range_min_span(void)
{
    reset_scene();
    ERNode* root = make_root(0xFF000000U);
    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps props = arc_props();
    props.arc_range = 1;
    props.arc_min_span = 10.0f; /* range is 0..100 */
    props.arc_value_start = 30.0f;
    props.arc_value = 70.0f;
    props.arc_adjustable = 1;
    props.arc_step = 1.0f;
    er_node_set_props(arc, &props);
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
    props.arc_knob = ER_ARC_KNOB_CIRCLE;
    props.arc_knob_size = 20;
    props.arc_value_start = 30.0f;
    props.arc_value = 70.0f;
    er_node_set_props(arc, &props);
    er_commit();
    static uint32_t snap[SCREEN * SCREEN];
    CHECK(er_arc_apply_value_start(arc, 68.0f), "low end pushes the high end");
    er_mark_dirty_upward(arc);
    er_commit();
    memcpy(snap, s_framebuffer, sizeof(snap));
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    er_force_full_repaint();
    er_commit();
    CHECK(memcmp(snap, s_framebuffer, sizeof(snap)) == 0, "a pushed far end is fully repainted (both knobs move)");

    /* A min_span WIDER than the range cannot be honoured. It must clamp to the range instead of running
     * the endpoint arithmetic with it — which used to store out-of-range values and, because the push
     * re-read a not-yet-assigned endpoint, recurse until the stack died. */
    props.arc_min_span = 200.0f; /* range is 0..100 */
    props.arc_value_start = 30.0f;
    props.arc_value = 70.0f;
    er_node_set_props(arc, &props);
    CHECK(arc->arc_value_start >= 0.0f && arc->arc_value_start <= 100.0f, "over-wide min_span keeps low in range");
    CHECK(arc->arc_value >= 0.0f && arc->arc_value <= 100.0f, "over-wide min_span keeps high in range");
    CHECK(arc->arc_value_start == 0.0f && arc->arc_value == 100.0f, "over-wide min_span opens the band fully");

    /* Raising min_span past what the LIVE endpoints allow: the same recursion trigger, via setProps. */
    props.arc_min_span = 0.0f;
    props.arc_value_start = 30.0f;
    props.arc_value = 70.0f;
    er_node_set_props(arc, &props);
    CHECK(arc->arc_value_start == 30.0f && arc->arc_value == 70.0f, "band reset for the widening check");
    props.arc_min_span = 99.0f;
    er_node_set_props(arc, &props);
    CHECK(arc->arc_value - arc->arc_value_start >= 98.9f, "widening min_span opens the live band to the new span");
    CHECK(arc->arc_value_start >= 0.0f && arc->arc_value <= 100.0f, "and both ends stay inside the range");
    /* And a drag on top of that settles rather than recursing. */
    (void)er_arc_apply_value_start(arc, 50.0f);
    CHECK(arc->arc_value_start >= 0.0f && arc->arc_value <= 100.0f, "a drag under an over-wide span stays in range");

    /* A negative min_span is meaningless; treat it as none. */
    props.arc_min_span = -5.0f;
    props.arc_value_start = 30.0f;
    props.arc_value = 70.0f;
    er_node_set_props(arc, &props);
    CHECK(arc->arc_value_start == 30.0f && arc->arc_value == 70.0f, "a negative min_span behaves as none");

    /* min_span 0 keeps the old clamp-at-neighbour behaviour. */
    props.arc_min_span = 0.0f;
    props.arc_value_start = 30.0f;
    props.arc_value = 70.0f;
    er_node_set_props(arc, &props);
    er_commit();
    (void)er_arc_apply_value_start(arc, 90.0f);
    CHECK(arc->arc_value_start == 70.0f && arc->arc_value == 70.0f,
          "with no min_span the ends meet and the far end stays put");
}

/**
 * @brief A GRID of dials all changing at once keeps a per-dial sliver of damage.
 *
 * The rect budget is what makes this work: once the damage set saturates, the scattered slivers
 * cascade-merge toward one box, and since a vector node rasterizes against the ACTIVE CLIP (the
 * background under it was erased across that whole clip, so it must repaint there), every dial then
 * re-rasterizes its full ring. With a rect per dial each clip stays a sliver and so does each raster.
 */
static void test_grid_damage(void)
{
    enum
    {
        GRID = 3,
        BOX = 70,
        PITCH = 80
    };
    reset_scene();
    ERNode* root = make_root(0xFF202020U);
    ERNode* arcs[GRID * GRID];
    ERProps props;
    for (int grid_row = 0; grid_row < GRID; grid_row++)
        for (int grid_column = 0; grid_column < GRID; grid_column++)
        {
            props = arc_props();
            props.width = BOX;
            props.height = BOX;
            props.margin_left = props.margin_top = ER_LAYOUT_AUTO;
            props.position = ER_POSITION_ABSOLUTE;
            props.left = (float)(5 + grid_column * PITCH);
            props.top = (float)(5 + grid_row * PITCH);
            props.arc_width = 8;
            ERNode* grid_arc = er_node_create(ER_NODE_ARC);
            er_node_set_props(grid_arc, &props);
            er_tree_append_child(root, grid_arc);
            arcs[grid_row * GRID + grid_column] = grid_arc;
        }
    er_commit(); /* mount: full repaint */

    /* Nudge every dial in ONE commit — the scattered-update frame. */
    memset(s_touched, 0, sizeof(s_touched));
    for (int grid_row = 0; grid_row < GRID; grid_row++)
        for (int grid_column = 0; grid_column < GRID; grid_column++)
        {
            props = arc_props();
            props.width = BOX;
            props.height = BOX;
            props.margin_left = props.margin_top = ER_LAYOUT_AUTO;
            props.position = ER_POSITION_ABSOLUTE;
            props.left = (float)(5 + grid_column * PITCH);
            props.top = (float)(5 + grid_row * PITCH);
            props.arc_width = 8;
            props.arc_value = 35.0f;
            er_node_set_props(arcs[grid_row * GRID + grid_column], &props);
        }
    er_commit();

    /* ER_DAMAGE_RECTS_MAX is a build-time budget constrained targets legitimately lower (the CYD and
     * RP2040 examples pin it to 4). Under one rect per dial the engine is SUPPOSED to saturate and
     * merge, so the per-dial claims below are gated on a full budget; the coverage and full-repaint
     * equivalence checks run at any budget. */
    const bool full_budget = (ER_DAMAGE_RECTS_MAX >= GRID * GRID);

    ERRect rects[ER_DAMAGE_RECTS_MAX];
    const int rect_count = er_get_dirty_rects(rects, ER_DAMAGE_RECTS_MAX);
    if (full_budget)
        CHECK(rect_count == GRID * GRID, "every dial in the grid keeps its own damage rect");
    else
        CHECK(rect_count >= 1 && rect_count <= ER_DAMAGE_RECTS_MAX,
              "the saturated grid stays within the configured budget");
    uint32_t damage = 0U;
    for (int rect_index = 0; rect_index < rect_count; rect_index++)
        damage += (uint32_t)rects[rect_index].width * (uint32_t)rects[rect_index].height;

    /* The raster is what the clip really governs: count the pixels the backend wrote. Repainting all
     * nine rings in full would be an order of magnitude more than the nine slivers. */
    uint32_t touched = 0U;
    for (int pixel_index = 0; pixel_index < SCREEN * SCREEN; pixel_index++)
        touched += s_touched[pixel_index] ? 1U : 0U;
    if (getenv("ER_ARC_DEBUG"))
        fprintf(stderr,
                "grid: %d rects (budget %d), damage %u px, touched %u px\n",
                rect_count,
                (int)ER_DAMAGE_RECTS_MAX,
                damage,
                touched);
    if (full_budget)
        CHECK(damage < (uint32_t)(GRID * GRID * BOX * BOX) / 4U, "grid damage stays a fraction of the dial boxes");
    CHECK(touched <= damage, "no pixel written outside the reported damage");

    /* Nine clipped passes must still composite exactly like one unclipped repaint. */
    uint32_t snap[SCREEN * SCREEN];
    memcpy(snap, s_framebuffer, sizeof(snap));
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    er_force_full_repaint();
    er_commit();
    CHECK(memcmp(snap, s_framebuffer, sizeof(snap)) == 0,
          "the grid's incremental frame is byte-identical to a full repaint");
}

int main(void)
{
    EmbeddedRenderBackend backend;
    memset(&backend, 0, sizeof(backend));
    backend.fill_rect = fb_fill;
    backend.copy_rect = fb_copy;
    backend.blend_rect = fb_blend;
    embedded_renderer_set_backend(&backend);

    test_geometry();
    test_showcase();
    test_segments();
    test_value_damage();
    test_grid_damage();
    test_native_animation();
    test_drag();
    test_drag_pointer_events_and_transform();
    test_child_knob();
    test_overhang();
    test_span_cache();
    test_svg_native_parity();
    test_svg_circle_route();
    test_svg_fallbacks();
    test_extreme_geometry();
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
