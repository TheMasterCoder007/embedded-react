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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define PASS "\033[32mPASS\033[0m"
#define FAIL "\033[31mFAIL\033[0m"

#define ASSERT(cond)                                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            fprintf(stderr, "  " FAIL " %s:%d  %s\n", __FILE__, __LINE__, #cond);                                      \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

#define ASSERT_EQ(a, b)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((a) != (b))                                                                                                \
        {                                                                                                              \
            fprintf(stderr, "  " FAIL " %s:%d  expected %d got %d\n", __FILE__, __LINE__, (int)(b), (int)(a));         \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

#define ASSERT_FLOAT_NEAR(a, b, eps)                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        float _a = (float)(a), _b = (float)(b), _e = (float)(eps);                                                     \
        float _diff = _a - _b;                                                                                         \
        if (_diff < 0.0f)                                                                                              \
            _diff = -_diff;                                                                                            \
        if (_diff > _e)                                                                                                \
        {                                                                                                              \
            fprintf(stderr,                                                                                            \
                    "  " FAIL " %s:%d  expected %.4f near %.4f (eps %.4f)\n",                                          \
                    __FILE__,                                                                                          \
                    __LINE__,                                                                                          \
                    (double)_b,                                                                                        \
                    (double)_a,                                                                                        \
                    (double)_e);                                                                                       \
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
    int w;
    int h;
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
 * @param[in] argb  Fill color.
 * @param[in] x     Left edge.
 * @param[in] y     Top edge.
 * @param[in] w     Width.
 * @param[in] h     Height.
 * @param[in] ctx   Pointer to TestCtx.
 */
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    TestCtx* t = (TestCtx*)ctx;
    if (t->fill_count < MAX_FILL_RECORDS)
    {
        FillRecord* r = &t->fills[t->fill_count++];
        r->x = x;
        r->y = y;
        r->w = w;
        r->h = h;
        r->color = argb;
    }
}

/**
 * @brief copy_rect backend stub — no-op.
 *
 * @param[in] src     Unused.
 * @param[in] stride  Unused.
 * @param[in] x       Unused.
 * @param[in] y       Unused.
 * @param[in] w       Unused.
 * @param[in] h       Unused.
 * @param[in] ctx     Unused.
 */
static void copy_cb(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)stride;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/**
 * @brief blend_rect backend stub — no-op.
 *
 * @param[in] src     Unused.
 * @param[in] stride  Unused.
 * @param[in] alpha   Unused.
 * @param[in] x       Unused.
 * @param[in] y       Unused.
 * @param[in] w       Unused.
 * @param[in] h       Unused.
 * @param[in] ctx     Unused.
 */
static void blend_cb(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)stride;
    (void)alpha;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Initialises a test backend and resets the TestCtx.
 *
 * @param[in,out] tctx  Test context to zero.
 * @param[out]    be    Backend struct to fill.
 */
static void setup_backend(TestCtx* tctx, EmbeddedRenderBackend* be)
{
    memset(tctx, 0, sizeof(*tctx));
    memset(be, 0, sizeof(*be));
    be->fill_rect = fill_cb;
    be->copy_rect = copy_cb;
    be->blend_rect = blend_cb;
    be->ctx = tctx;
    embedded_renderer_set_backend(be);
}

/**
 * @brief Returns a zero-initialised ERProps with all layout fields set to ER_LAYOUT_AUTO.
 *
 * @return Fully-auto ERProps ready to have specific fields overwritten.
 */
static ERProps make_props(void)
{
    ERProps p;
    memset(&p, 0, sizeof(p));
    p.left = p.top = p.right = p.bottom = ER_LAYOUT_AUTO;
    p.width = p.height = ER_LAYOUT_AUTO;
    p.min_width = p.max_width = ER_LAYOUT_AUTO;
    p.min_height = p.max_height = ER_LAYOUT_AUTO;
    p.padding = ER_LAYOUT_AUTO;
    p.padding_left = p.padding_top = p.padding_right = p.padding_bottom = ER_LAYOUT_AUTO;
    p.margin = ER_LAYOUT_AUTO;
    p.margin_left = p.margin_top = p.margin_right = p.margin_bottom = ER_LAYOUT_AUTO;
    p.gap = p.row_gap = p.column_gap = ER_LAYOUT_AUTO;
    p.flex_basis = ER_LAYOUT_AUTO;
    p.flex_direction = ER_FLEX_COL;
    p.align_items = ER_ALIGN_STRETCH;
    p.align_self = ER_ALIGN_AUTO;
    p.justify_content = ER_JUSTIFY_FLEX_START;
    p.opacity = 255U; /* fully opaque so nodes are visible to hit-testing */
    return p;
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
    ScrollRecord* r = (ScrollRecord*)user;
    r->count++;
    r->last_x = data->scroll_x;
    r->last_y = data->scroll_y;
}

/**
 * @brief Returns 1 when any recorded fill intersects (px, py) with the given color.
 *
 * @param[in] tctx   Test context holding fill records.
 * @param[in] px     X coordinate to test.
 * @param[in] py     Y coordinate to test.
 * @param[in] color  ARGB8888 color to match (0 = any).
 *
 * @return 1 if found, 0 otherwise.
 */
static int fill_covers(const TestCtx* tctx, int px, int py, uint32_t color)
{
    for (int i = 0; i < tctx->fill_count; i++)
    {
        const FillRecord* f = &tctx->fills[i];
        if (color != 0U && f->color != color)
            continue;
        if (px >= f->x && px < f->x + f->w && py >= f->y && py < f->y + f->h)
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
    ERProps p;

    ERNode* root = er_node_create(ER_NODE_VIEW);
    p = make_props();
    p.width = 200;
    p.height = 400;
    p.background_color = 0xFF101010U;
    er_node_set_props(root, &p);

    *sv = er_node_create(ER_NODE_SCROLL_VIEW);
    p = make_props();
    p.width = 200;
    p.height = 200;
    p.overflow = ER_OVERFLOW_SCROLL;
    p.background_color = 0xFF202020U;
    er_node_set_props(*sv, &p);

    *child0 = er_node_create(ER_NODE_VIEW);
    p = make_props();
    p.width = 200;
    p.height = 100;
    p.background_color = 0xFFFF0000U;
    er_node_set_props(*child0, &p);

    *child1 = er_node_create(ER_NODE_VIEW);
    p = make_props();
    p.width = 200;
    p.height = 100;
    p.background_color = 0xFF00FF00U;
    er_node_set_props(*child1, &p);

    *child2 = er_node_create(ER_NODE_VIEW);
    p = make_props();
    p.width = 200;
    p.height = 100;
    p.background_color = 0xFF0000FFU;
    er_node_set_props(*child2, &p);

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
 * @brief Negative offsets are clamped to zero and do not fire ER_EVENT_SCROLL.
 */
static void test_offset_clamp_negative(void)
{
    printf("test_offset_clamp_negative\n");
    TestCtx tctx;
    EmbeddedRenderBackend be;
    setup_backend(&tctx, &be);

    ERNode *sv, *c0, *c1, *c2;
    build_scene(&sv, &c0, &c1, &c2);

    /* Offset starts at 0. Setting it to negative should leave it at 0. */
    ScrollRecord rec = {0};
    er_event_set(sv, ER_EVENT_SCROLL, on_scroll, &rec);
    er_scroll_view_set_offset(sv, -10.0f, -10.0f);
    ASSERT_EQ(rec.count, 0); /* no change → no event */
}

/**
 * @brief Offsets beyond the scrollable range are clamped to content_size - viewport_size.
 */
static void test_offset_clamp_max(void)
{
    printf("test_offset_clamp_max\n");
    TestCtx tctx;
    EmbeddedRenderBackend be;
    setup_backend(&tctx, &be);

    ERNode *sv, *c0, *c1, *c2;
    build_scene(&sv, &c0, &c1, &c2);
    /* content_h = 300, viewport_h = 200  →  max_y = 100 */

    ScrollRecord rec = {0};
    er_event_set(sv, ER_EVENT_SCROLL, on_scroll, &rec);
    er_scroll_view_set_offset(sv, 0.0f, 999.0f);
    ASSERT_EQ(rec.count, 1);
    ASSERT_FLOAT_NEAR(rec.last_y, 100.0f, 0.01f);
}

/**
 * @brief ER_EVENT_SCROLL fires with the correct values and deduplicated (no double-fire).
 */
static void test_scroll_event_dedup(void)
{
    printf("test_scroll_event_dedup\n");
    TestCtx tctx;
    EmbeddedRenderBackend be;
    setup_backend(&tctx, &be);

    ERNode *sv, *c0, *c1, *c2;
    build_scene(&sv, &c0, &c1, &c2);

    ScrollRecord rec = {0};
    er_event_set(sv, ER_EVENT_SCROLL, on_scroll, &rec);

    er_scroll_view_set_offset(sv, 0.0f, 50.0f);
    ASSERT_EQ(rec.count, 1);
    ASSERT_FLOAT_NEAR(rec.last_y, 50.0f, 0.001f);

    er_scroll_view_set_offset(sv, 0.0f, 50.0f); /* same value */
    ASSERT_EQ(rec.count, 1);                    /* must not fire again */
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
    TestCtx tctx;
    EmbeddedRenderBackend be;
    setup_backend(&tctx, &be);

    ERNode *sv, *c0, *c1, *c2;
    build_scene(&sv, &c0, &c1, &c2);

    er_scroll_view_set_offset(sv, 0.0f, 100.0f);
    tctx.fill_count = 0;
    er_commit();

    /* No red fill should appear anywhere within the viewport (y < 200). */
    for (int py = 0; py < 200; py++)
    {
        for (int px = 0; px < 200; px++)
        {
            ASSERT(!fill_covers(&tctx, px, py, 0xFFFF0000U));
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
    TestCtx tctx;
    EmbeddedRenderBackend be;
    setup_backend(&tctx, &be);

    ERNode *sv, *c0, *c1, *c2;
    build_scene(&sv, &c0, &c1, &c2);

    er_scroll_view_set_offset(sv, 0.0f, 100.0f);
    tctx.fill_count = 0;
    er_commit();

    int found = 0;
    for (int py = 100; py < 200 && !found; py++)
        for (int px = 0; px < 200 && !found; px++)
            found = fill_covers(&tctx, px, py, 0xFF0000FFU);
    ASSERT(found);
}

/**
 * @brief A vertical pan gesture that exceeds the slop threshold claims the ScrollView
 *        and advances the scroll offset.
 */
static void test_gesture_claims_scroll_view(void)
{
    printf("test_gesture_claims_scroll_view\n");
    TestCtx tctx;
    EmbeddedRenderBackend be;
    setup_backend(&tctx, &be);

    ERNode *sv, *c0, *c1, *c2;
    build_scene(&sv, &c0, &c1, &c2);

    ScrollRecord rec = {0};
    er_event_set(sv, ER_EVENT_SCROLL, on_scroll, &rec);

    /* Touch child0 area and drag upward 10 px (> 5 px slop). */
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 100, 50);
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 100, 44); /* 6 px above slop */
    embedded_renderer_touch(0, ER_TOUCH_UP, 100, 44);

    /* Offset must have increased (upward swipe → scroll down). */
    ASSERT(rec.count > 0);
    ASSERT(rec.last_y > 0.0f);
}

/**
 * @brief After touch-up the scroll offset continues advancing due to momentum.
 */
static void test_momentum_advances_offset(void)
{
    printf("test_momentum_advances_offset\n");
    TestCtx tctx;
    EmbeddedRenderBackend be;
    setup_backend(&tctx, &be);

    ERNode *sv, *c0, *c1, *c2;
    build_scene(&sv, &c0, &c1, &c2);

    ScrollRecord rec = {0};
    er_event_set(sv, ER_EVENT_SCROLL, on_scroll, &rec);

    /* Fast upward drag to build velocity. */
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 100, 100);
    embedded_renderer_tick(16U);
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 100, 80); /* -20 px in 16 ms */
    embedded_renderer_touch(0, ER_TOUCH_UP, 100, 80);

    const float offset_at_release = rec.last_y;

    /* One more tick should push offset further via momentum. */
    embedded_renderer_tick(16U);

    ASSERT(rec.last_y > offset_at_release);
}

/**
 * @brief Momentum velocity decays to zero and the scroll offset stabilises.
 */
static void test_momentum_decays(void)
{
    printf("test_momentum_decays\n");
    TestCtx tctx;
    EmbeddedRenderBackend be;
    setup_backend(&tctx, &be);

    ERNode *sv, *c0, *c1, *c2;
    build_scene(&sv, &c0, &c1, &c2);

    ScrollRecord rec = {0};
    er_event_set(sv, ER_EVENT_SCROLL, on_scroll, &rec);

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 100, 100);
    embedded_renderer_tick(16U);
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 100, 70); /* 30 px drag */
    embedded_renderer_touch(0, ER_TOUCH_UP, 100, 70);

    /* Tick many frames; offset must eventually stop changing. */
    float prev_y = rec.last_y;
    int settled = 0;
    for (int i = 0; i < 600 && !settled; i++)
    {
        embedded_renderer_tick(16U);
        if (rec.last_y == prev_y)
            settled = 1;
        prev_y = rec.last_y;
    }
    ASSERT(settled);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Test entry point.
 *
 * @return 0 on success, non-zero when any assertion failed.
 */
int main(void)
{
    test_offset_clamp_negative();
    test_offset_clamp_max();
    test_scroll_event_dedup();
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
