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

#include "er_scene.h"
#include "native_renderer.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define PASS "\033[32mPASS\033[0m"
#define FAIL "\033[31mFAIL\033[0m"
#define OPACITY_OPAQUE 255U

#define ASSERT(condition)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "  " FAIL " %s:%d  %s\n", __FILE__, __LINE__, #condition);                                 \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

#define ASSERT_EQ(actual, expected)                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((actual) != (expected))                                                                                    \
        {                                                                                                              \
            fprintf(                                                                                                   \
                stderr, "  " FAIL " %s:%d  expected %d got %d\n", __FILE__, __LINE__, (int)(expected), (int)(actual)); \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

#define ASSERT_FLOAT_NEAR(actual, expected, epsilon)                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        float _actual = (float)(actual), _expected = (float)(expected), _epsilon = (float)(epsilon);                   \
        float _diff = _actual - _expected;                                                                             \
        if (_diff < 0.0f)                                                                                              \
            _diff = -_diff;                                                                                            \
        if (_diff > _epsilon)                                                                                          \
        {                                                                                                              \
            fprintf(stderr,                                                                                            \
                    "  " FAIL " %s:%d  expected %.4f near %.4f (eps %.4f)\n",                                          \
                    __FILE__,                                                                                          \
                    __LINE__,                                                                                          \
                    (double)_expected,                                                                                 \
                    (double)_actual,                                                                                   \
                    (double)_epsilon);                                                                                 \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

/** @brief Maximum fill_rect calls captured in one test. */
#define MAX_FILL_RECORDS 512

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Records one fill_rect call received by the test backend.
 */
typedef struct
{
    int x;
    int y;
    int width;
    int height;
    uint32_t color;
} FillRecord;

/**
 * @brief Test backend context — captures fill_rect calls.
 */
typedef struct
{
    FillRecord fills[MAX_FILL_RECORDS];
    int fill_count;
} TestCtx;

/**
 * @brief Accumulates ER_EVENT_SCROLL callbacks.
 */
typedef struct
{
    int count;
    float last_x;
    float last_y;
} ScrollRecord;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static int g_failures = 0;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — backend stubs
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief fill_rect backend that records each call.
 *
 * @param[in] argb    Fill color.
 * @param[in] x       Left edge.
 * @param[in] y       Top edge.
 * @param[in] width   Width.
 * @param[in] height  Height.
 * @param[in] ctx     Pointer to TestCtx.
 */
static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    TestCtx* test_context = (TestCtx*)ctx;
    if (test_context->fill_count < MAX_FILL_RECORDS)
    {
        FillRecord* fill = &test_context->fills[test_context->fill_count++];
        fill->x = x;
        fill->y = y;
        fill->width = width;
        fill->height = height;
        fill->color = argb;
    }
}

/**
 * @brief copy_rect backend stub — no-op.
 *
 * @param[in] source  Unused.
 * @param[in] stride  Unused.
 * @param[in] x       Unused.
 * @param[in] y       Unused.
 * @param[in] width   Unused.
 * @param[in] height  Unused.
 * @param[in] ctx     Unused.
 */
static void copy_cb(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)stride;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
}

/**
 * @brief blend_rect backend stub — no-op.
 *
 * @param[in] source  Unused.
 * @param[in] stride  Unused.
 * @param[in] alpha   Unused.
 * @param[in] x       Unused.
 * @param[in] y       Unused.
 * @param[in] width   Unused.
 * @param[in] height  Unused.
 * @param[in] ctx     Unused.
 */
static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)stride;
    (void)alpha;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Initialises a test backend and resets the TestCtx.
 *
 * @param[in,out] test_context  Test context to zero.
 * @param[out]    backend       Backend struct to fill.
 */
static void setup_backend(TestCtx* test_context, EmbeddedRenderBackend* backend)
{
    memset(test_context, 0, sizeof(*test_context));
    memset(backend, 0, sizeof(*backend));
    backend->fill_rect = fill_cb;
    backend->copy_rect = copy_cb;
    backend->blend_rect = blend_cb;
    backend->ctx = test_context;
    embedded_renderer_set_backend(backend);
}

/**
 * @brief Returns a zero-initialised ERProps with all layout fields set to ER_LAYOUT_AUTO.
 *
 * @return Fully-auto ERProps ready to have specific fields overwritten.
 */
static ERProps make_props(void)
{
    ERProps props;
    memset(&props, 0, sizeof(props));
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    props.width = props.height = ER_LAYOUT_AUTO;
    props.min_width = props.max_width = ER_LAYOUT_AUTO;
    props.min_height = props.max_height = ER_LAYOUT_AUTO;
    props.padding = ER_LAYOUT_AUTO;
    props.padding_left = props.padding_top = props.padding_right = props.padding_bottom = ER_LAYOUT_AUTO;
    props.margin = ER_LAYOUT_AUTO;
    props.margin_left = props.margin_top = props.margin_right = props.margin_bottom = ER_LAYOUT_AUTO;
    props.gap = props.row_gap = props.column_gap = ER_LAYOUT_AUTO;
    props.flex_basis = ER_LAYOUT_AUTO;
    props.flex_direction = ER_FLEX_COL;
    props.align_items = ER_ALIGN_STRETCH;
    props.align_self = ER_ALIGN_AUTO;
    props.justify_content = ER_JUSTIFY_FLEX_START;
    props.opacity = OPACITY_OPAQUE; /* fully opaque so nodes are visible to hit-testing */
    return props;
}

/**
 * @brief ER_EVENT_SCROLL callback that stores the offset in a ScrollRecord.
 *
 * @param[in] node  Scrolled node (unused).
 * @param[in] data  Event payload.
 * @param[in] user  Pointer to ScrollRecord.
 */
static void on_scroll(ERNode* node, const EREventData* data, void* user)
{
    (void)node;
    ScrollRecord* record = (ScrollRecord*)user;
    record->count++;
    record->last_x = data->scroll_x;
    record->last_y = data->scroll_y;
}

/**
 * @brief Returns 1 when any recorded fill intersects (pixel_x, pixel_y) with the given color.
 *
 * @param[in] test_context  Test context holding fill records.
 * @param[in] pixel_x       X coordinate to test.
 * @param[in] pixel_y       Y coordinate to test.
 * @param[in] color         ARGB8888 color to match (0 = any).
 *
 * @return 1 if found, 0 otherwise.
 */
static int fill_covers(const TestCtx* test_context, int pixel_x, int pixel_y, uint32_t color)
{
    for (int fill_index = 0; fill_index < test_context->fill_count; fill_index++)
    {
        const FillRecord* fill = &test_context->fills[fill_index];
        if (color != 0U && fill->color != color)
            continue;
        if (pixel_x >= fill->x && pixel_x < fill->x + fill->width && pixel_y >= fill->y
            && pixel_y < fill->y + fill->height)
            return 1;
    }
    return 0;
}

/**
 * @brief Builds a vertical-scroll scene with a 200×200 viewport and 300 px of content.
 *
 *  Root (200 × 400, flex-col)
 *    ScrollView (200 × 200, overflow:scroll)
 *      child0 (200 × 100, red)
 *      child1 (200 × 100, green)
 *      child2 (200 × 100, blue)   ← entirely below the initial viewport
 *
 * @param[out] sv      The created ScrollView node.
 * @param[out] child0  Red child (layout y = 0..100).
 * @param[out] child1  Green child (layout y = 100..200).
 * @param[out] child2  Blue child (layout y = 200..300).
 */
static void build_scene(ERNode** sv, ERNode** child0, ERNode** child1, ERNode** child2)
{
    ERProps props;

    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    props = make_props();
    props.width = 200;
    props.height = 400;
    props.background_color = 0xFF101010U;
    er_node_set_props(root, &props);

    *sv = er_node_create(ER_NODE_SCROLL_VIEW);
    props = make_props();
    props.width = 200;
    props.height = 200;
    props.overflow = ER_OVERFLOW_SCROLL;
    props.background_color = 0xFF202020U;
    er_node_set_props(*sv, &props);

    *child0 = er_node_create(ER_NODE_VIEW);
    props = make_props();
    props.width = 200;
    props.height = 100;
    props.background_color = 0xFFFF0000U;
    er_node_set_props(*child0, &props);

    *child1 = er_node_create(ER_NODE_VIEW);
    props = make_props();
    props.width = 200;
    props.height = 100;
    props.background_color = 0xFF00FF00U;
    er_node_set_props(*child1, &props);

    *child2 = er_node_create(ER_NODE_VIEW);
    props = make_props();
    props.width = 200;
    props.height = 100;
    props.background_color = 0xFF0000FFU;
    er_node_set_props(*child2, &props);

    er_tree_append_child(root, *sv);
    er_tree_append_child(*sv, *child0);
    er_tree_append_child(*sv, *child1);
    er_tree_append_child(*sv, *child2);

    er_tree_set_root(root);
    er_commit();
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — test cases
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Sends one touch-move and dispatches it, standing in for a host frame.
 *
 * embedded_renderer_touch() coalesces moves to the newest one per finger and dispatches that one at
 * the frame boundary, so a test that wants each move observed has to mark the boundary the way a host
 * loop does — er_commit() and the JS pump both flush, and embedded_renderer_flush_touch() is that same
 * flush on its own.
 *
 * @param[in] x  X coordinate of the move.
 * @param[in] y  Y coordinate of the move.
 */
static void touch_move(int x, int y)
{
    embedded_renderer_touch(0, ER_TOUCH_MOVE, x, y);
    embedded_renderer_flush_touch();
}

/**
 * @brief Negative offsets are clamped to zero and do not fire ER_EVENT_SCROLL.
 */
static void test_offset_clamp_negative(void)
{
    printf("test_offset_clamp_negative\n");
    TestCtx test_context;
    EmbeddedRenderBackend backend;
    setup_backend(&test_context, &backend);

    ERNode *scroll_view, *first_child, *second_child, *third_child;
    build_scene(&scroll_view, &first_child, &second_child, &third_child);

    /* Offset starts at 0. Setting it to negative should leave it at 0. */
    ScrollRecord record = {0};
    er_event_set(scroll_view, ER_EVENT_SCROLL, on_scroll, &record);
    er_scroll_view_set_offset(scroll_view, -10.0f, -10.0f);
    ASSERT_EQ(record.count, 0); /* no change → no event */
}

/**
 * @brief Offsets beyond the scrollable range are clamped to content_size - viewport_size.
 */
static void test_offset_clamp_max(void)
{
    printf("test_offset_clamp_max\n");
    TestCtx test_context;
    EmbeddedRenderBackend backend;
    setup_backend(&test_context, &backend);

    ERNode *scroll_view, *first_child, *second_child, *third_child;
    build_scene(&scroll_view, &first_child, &second_child, &third_child);
    /* content_h = 300, viewport_h = 200  →  max_y = 100 */

    ScrollRecord record = {0};
    er_event_set(scroll_view, ER_EVENT_SCROLL, on_scroll, &record);
    er_scroll_view_set_offset(scroll_view, 0.0f, 999.0f);
    ASSERT_EQ(record.count, 1);
    ASSERT_FLOAT_NEAR(record.last_y, 100.0f, 0.01f);
}

/**
 * @brief ER_EVENT_SCROLL fires with the correct values and deduplicated (no double-fire).
 */
static void test_scroll_event_dedup(void)
{
    printf("test_scroll_event_dedup\n");
    TestCtx test_context;
    EmbeddedRenderBackend backend;
    setup_backend(&test_context, &backend);

    ERNode *scroll_view, *first_child, *second_child, *third_child;
    build_scene(&scroll_view, &first_child, &second_child, &third_child);

    ScrollRecord record = {0};
    er_event_set(scroll_view, ER_EVENT_SCROLL, on_scroll, &record);

    er_scroll_view_set_offset(scroll_view, 0.0f, 50.0f);
    ASSERT_EQ(record.count, 1);
    ASSERT_FLOAT_NEAR(record.last_y, 50.0f, 0.001f);

    er_scroll_view_set_offset(scroll_view, 0.0f, 50.0f); /* same value */
    ASSERT_EQ(record.count, 1);                          /* must not fire again */
}

/**
 * @brief A step that stays within the same whole pixel repaints nothing, but still reports the scroll.
 *
 * Content is placed at the whole-pixel offset, so 10.0 -> 10.4 -> 10.9 draws the same frame three times;
 * crossing into pixel 11 must repaint again.
 */
static void test_subpixel_step_repaints_nothing(void)
{
    printf("test_subpixel_step_repaints_nothing\n");
    TestCtx test_context;
    EmbeddedRenderBackend backend;
    setup_backend(&test_context, &backend);

    ERNode *scroll_view, *first_child, *second_child, *third_child;
    build_scene(&scroll_view, &first_child, &second_child, &third_child);
    er_commit();

    ScrollRecord record = {0};
    er_event_set(scroll_view, ER_EVENT_SCROLL, on_scroll, &record);

    er_scroll_view_set_offset(scroll_view, 0.0f, 10.0f);
    er_commit();

    er_scroll_view_set_offset(scroll_view, 0.0f, 10.4f);
    test_context.fill_count = 0;
    er_commit();
    ASSERT_EQ(test_context.fill_count, 0);
    ASSERT_EQ(record.count, 2);
    ASSERT_FLOAT_NEAR(record.last_y, 10.4f, 0.001f);

    er_scroll_view_set_offset(scroll_view, 0.0f, 10.9f);
    test_context.fill_count = 0;
    er_commit();
    ASSERT_EQ(test_context.fill_count, 0);

    er_scroll_view_set_offset(scroll_view, 0.0f, 11.2f);
    test_context.fill_count = 0;
    er_commit();
    ASSERT(test_context.fill_count > 0);
}

/**
 * @brief A NaN offset leaves that axis where it is; the other axis still scrolls.
 */
static void test_nan_offset_keeps_axis(void)
{
    printf("test_nan_offset_keeps_axis\n");
    TestCtx test_context;
    EmbeddedRenderBackend backend;
    setup_backend(&test_context, &backend);

    ERNode *scroll_view, *first_child, *second_child, *third_child;
    build_scene(&scroll_view, &first_child, &second_child, &third_child);

    ScrollRecord record = {0};
    er_event_set(scroll_view, ER_EVENT_SCROLL, on_scroll, &record);

    er_scroll_view_set_offset(scroll_view, 0.0f, 40.0f);
    er_scroll_view_set_offset(scroll_view, 0.0f, NAN);
    ASSERT_EQ(record.count, 1); /* nothing changed: no second event */

    er_scroll_view_set_offset(scroll_view, NAN, 60.0f);
    ASSERT_EQ(record.count, 2);
    ASSERT_FLOAT_NEAR(record.last_x, 0.0f, 0.001f);
    ASSERT_FLOAT_NEAR(record.last_y, 60.0f, 0.001f);
    er_commit();
}

/**
 * @brief Scrolled-out children are clipped and produce no fill inside the viewport.
 *
 * After scrolling down 100 px child0 (red, layout y=0..100) maps to screen y=-100..0.
 * The active clip rect is [0,0,200,200] so no red pixels land in the viewport area.
 */
static void test_clip_hides_scrolled_out(void)
{
    printf("test_clip_hides_scrolled_out\n");
    TestCtx test_context;
    EmbeddedRenderBackend backend;
    setup_backend(&test_context, &backend);

    ERNode *scroll_view, *first_child, *second_child, *third_child;
    build_scene(&scroll_view, &first_child, &second_child, &third_child);

    er_scroll_view_set_offset(scroll_view, 0.0f, 100.0f);
    test_context.fill_count = 0;
    er_commit();

    /* No red fill should appear anywhere within the viewport (y < 200). */
    for (int pixel_y = 0; pixel_y < 200; pixel_y++)
    {
        for (int pixel_x = 0; pixel_x < 200; pixel_x++)
        {
            ASSERT(!fill_covers(&test_context, pixel_x, pixel_y, 0xFFFF0000U));
        }
    }
}

/**
 * @brief Scrolled-in children produce fill calls within the viewport after scrolling.
 *
 * After scrolling 100 px child2 (blue, layout y=200..300) maps to screen y=100..200
 * and must produce at least one fill inside [0,100,200,200].
 */
static void test_clip_shows_scrolled_in(void)
{
    printf("test_clip_shows_scrolled_in\n");
    TestCtx test_context;
    EmbeddedRenderBackend backend;
    setup_backend(&test_context, &backend);

    ERNode *scroll_view, *first_child, *second_child, *third_child;
    build_scene(&scroll_view, &first_child, &second_child, &third_child);

    er_scroll_view_set_offset(scroll_view, 0.0f, 100.0f);
    test_context.fill_count = 0;
    er_commit();

    int found = 0;
    for (int pixel_y = 100; pixel_y < 200 && !found; pixel_y++)
        for (int pixel_x = 0; pixel_x < 200 && !found; pixel_x++)
            found = fill_covers(&test_context, pixel_x, pixel_y, 0xFF0000FFU);
    ASSERT(found);
}

/**
 * @brief A vertical pan gesture that exceeds the slop threshold claims the ScrollView
 *        and advances the scroll offset.
 */
static void test_gesture_claims_scroll_view(void)
{
    printf("test_gesture_claims_scroll_view\n");
    TestCtx test_context;
    EmbeddedRenderBackend backend;
    setup_backend(&test_context, &backend);

    ERNode *scroll_view, *first_child, *second_child, *third_child;
    build_scene(&scroll_view, &first_child, &second_child, &third_child);

    ScrollRecord record = {0};
    er_event_set(scroll_view, ER_EVENT_SCROLL, on_scroll, &record);

    /* Touch child0 area and drag upward 10 px (> 5 px slop). */
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 100, 50);
    touch_move(100, 44); /* 6 px above slop */
    embedded_renderer_touch(0, ER_TOUCH_UP, 100, 44);

    /* Offset must have increased (upward swipe → scroll down). */
    ASSERT(record.count > 0);
    ASSERT(record.last_y > 0.0f);
}

/**
 * @brief After touch-up the scroll offset continues advancing due to momentum.
 */
static void test_momentum_advances_offset(void)
{
    printf("test_momentum_advances_offset\n");
    TestCtx test_context;
    EmbeddedRenderBackend backend;
    setup_backend(&test_context, &backend);

    ERNode *scroll_view, *first_child, *second_child, *third_child;
    build_scene(&scroll_view, &first_child, &second_child, &third_child);

    ScrollRecord record = {0};
    er_event_set(scroll_view, ER_EVENT_SCROLL, on_scroll, &record);

    /* Fast upward drag to build velocity. */
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 100, 100);
    embedded_renderer_tick(16U);
    touch_move(100, 80); /* -20 px in 16 ms */
    embedded_renderer_touch(0, ER_TOUCH_UP, 100, 80);

    const float offset_at_release = record.last_y;

    /* One more tick should push offset further via momentum. */
    embedded_renderer_tick(16U);

    ASSERT(record.last_y > offset_at_release);
}

/**
 * @brief Momentum velocity decays to zero and the scroll offset stabilises.
 */
static void test_momentum_decays(void)
{
    printf("test_momentum_decays\n");
    TestCtx test_context;
    EmbeddedRenderBackend backend;
    setup_backend(&test_context, &backend);

    ERNode *scroll_view, *first_child, *second_child, *third_child;
    build_scene(&scroll_view, &first_child, &second_child, &third_child);

    ScrollRecord record = {0};
    er_event_set(scroll_view, ER_EVENT_SCROLL, on_scroll, &record);

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 100, 100);
    embedded_renderer_tick(16U);
    touch_move(100, 70); /* 30 px drag */
    embedded_renderer_touch(0, ER_TOUCH_UP, 100, 70);

    /* Tick many frames; offset must eventually stop changing. */
    float previous_y = record.last_y;
    int settled = 0;
    for (int frame_index = 0; frame_index < 600 && !settled; frame_index++)
    {
        embedded_renderer_tick(16U);
        if (record.last_y == previous_y)
            settled = 1;
        previous_y = record.last_y;
    }
    ASSERT(settled);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief An unchanged scene settles to zero painting after a scroll, and a row scrolled back into view
 *        is still repainted.
 *
 * HONEST SCOPE: this does NOT reproduce the scrolled-out-footprint leak fixed in compositor.c's damage
 * pre-pass (a moved-but-invisible node re-damaging its old footprint on every commit forever). It was
 * written to guard that fix and does not: it passes with the fix reverted, because this scene is small
 * enough that the scroll commit still reaches first_child and refreshes its last_paint_rect, which clears `moved`
 * before the loop can start.
 *
 * What this DOES guard is worth keeping: an idle scene must not paint, and retiring a footprint must not
 * cost a row its next paint when it scrolls back.
 */
static void test_idle_scene_settles_after_scroll(void)
{
    printf("test_idle_scene_settles_after_scroll\n");
    TestCtx test_context;
    EmbeddedRenderBackend backend;
    setup_backend(&test_context, &backend);

    ERNode *scroll_view, *first_child, *second_child, *third_child;
    build_scene(&scroll_view,
                &first_child,
                &second_child,
                &third_child); /* commits once: first_child/second_child painted inside the viewport */

    /* Scroll a full row down: first_child (was y 0..100) moves to y -100..0 — entirely outside the clipper. */
    er_scroll_view_set_offset(scroll_view, 0.0f, 100.0f);
    test_context.fill_count = 0;
    er_commit(); /* first_child is `moved`; its old footprint is unioned once, to erase the trail */

    /* Nothing changes now, so the scene must settle to zero painting. Assert on the BACKEND's fill ops,
     * not on er_get_dirty_rect(): that rect only unions source_dirty nodes the walk actually reached, so
     * a moved-only, pruned node is invisible to it — which is exactly what made this bug so hard to see.
     * An earlier version of this test asserted the rect and passed even with the bug present. */
    test_context.fill_count = 0;
    er_commit();
    ASSERT_EQ(test_context.fill_count, 0);

    /* ...and again, in case the footprint is merely alternating rather than retired. */
    test_context.fill_count = 0;
    er_commit();
    ASSERT_EQ(test_context.fill_count, 0);

    /* Scrolling it back into view must still repaint it: er_mark_dirty_upward marks the SCROLLER
     * source_dirty, so its viewport enters the damage and the walk reaches the child again. Retiring the
     * footprint must not cost the row its next paint. */
    er_scroll_view_set_offset(scroll_view, 0.0f, 0.0f);
    test_context.fill_count = 0;
    er_commit();
    if (test_context.fill_count == 0)
    {
        fprintf(stderr, "  " FAIL " %s:%d  row scrolled back into view was never repainted\n", __FILE__, __LINE__);
        g_failures++;
    }
}

/**
 * @brief Test entry point.
 *
 * @return 0 on success, non-zero when any assertion failed.
 */
int main(void)
{
    test_idle_scene_settles_after_scroll();
    test_offset_clamp_negative();
    test_offset_clamp_max();
    test_scroll_event_dedup();
    test_subpixel_step_repaints_nothing();
    test_nan_offset_keeps_axis();
    test_clip_hides_scrolled_out();
    test_clip_shows_scrolled_in();
    test_gesture_claims_scroll_view();
    test_momentum_advances_offset();
    test_momentum_decays();

    if (g_failures == 0)
        printf(PASS " all scroll tests passed\n");
    else
        fprintf(stderr, FAIL " %d scroll test(s) failed\n", g_failures);

    return g_failures > 0 ? 1 : 0;
}
