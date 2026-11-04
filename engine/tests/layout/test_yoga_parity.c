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
 * Yoga parity tests.
 *
 * Each fixture builds a small tree and compares the engine's computed rectangles against the
 * values a real React Native / Yoga (Chrome flexbox) layout produces for the same input. The
 * goal is to make flexbox divergences from React **visible and deterministic** instead of being
 * spotted by eye in a demo.
 *
 * Status per assertion:
 *   EXPECT — the engine should already match Yoga. A mismatch is a regression and fails the suite.
 *   XFAIL  — a known divergence (the engine does not yet match Yoga here). It does NOT fail the
 *            suite while it still diverges, so the suite stays green; but if the engine ever starts
 *            matching, the assertion becomes a PROMOTE and FAILS the suite — a reminder to flip the
 *            tag to EXPECT once the gap is closed.
 *
 * Adding a fixture: write a `fixture_*()` that builds nodes (via mk()), captures rects with an
 * ER_EVENT_LAYOUT handler, commits, calls pcheck() per node, and tears the tree down. Then call it
 * from main(). Keep expected values authoritative (hand-verifiable or copied from Chrome/Yoga).
 *
 * Positions and sizes both land on Yoga's pixel grid: a coordinate on a half pixel goes up, and a
 * size is the difference of its two rounded edges. Expected values can be taken from `yoga-layout`
 * verbatim.
 *
 * Known divergences not yet expressible through ERProps (so not testable here yet) are listed at
 * the bottom of this file.
 */

#include "er_scene.h"
#include "native_renderer.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define OPACITY_OPAQUE 255U

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static int g_pass = 0;    /**< EXPECT assertions that matched. */
static int g_xfail = 0;   /**< XFAIL assertions still diverging (expected). */
static int g_promote = 0; /**< XFAIL assertions that now match — flip to EXPECT. */
static int g_regress = 0; /**< EXPECT assertions that mismatched — real regression. */

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — backend + helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief No-op fill. @param argb c. @param x x. @param y y. @param w w. @param h h. @param ctx c. */
static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)argb;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
}

/** @brief No-op copy. */
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

/** @brief No-op blend. */
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

/**
 * @brief ER_EVENT_LAYOUT handler that records a node's computed rect into an ERRect.
 *
 * @param[in] node       Node that fired (unused).
 * @param[in] data       Event payload carrying data->layout_rect.
 * @param[in] user_data  Pointer to an ERRect to fill.
 */
static void on_rect(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    *(ERRect*)user_data = data->layout_rect;
}

/**
 * @brief Builds an ERProps with all layout fields set to ER_LAYOUT_AUTO (RN-like defaults).
 *
 * @return Zero-initialised ERProps with AUTO layout sentinels and opacity 255.
 */
static ERProps props_default(void)
{
    ERProps props = {0};
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    props.width = props.height = ER_LAYOUT_AUTO;
    props.min_width = props.max_width = ER_LAYOUT_AUTO;
    props.min_height = props.max_height = ER_LAYOUT_AUTO;
    props.padding = props.padding_left = props.padding_top = ER_LAYOUT_AUTO;
    props.padding_right = props.padding_bottom = ER_LAYOUT_AUTO;
    props.padding_horizontal = props.padding_vertical = ER_LAYOUT_AUTO;
    props.margin = props.margin_left = props.margin_top = ER_LAYOUT_AUTO;
    props.margin_right = props.margin_bottom = ER_LAYOUT_AUTO;
    props.margin_horizontal = props.margin_vertical = ER_LAYOUT_AUTO;
    props.gap = props.row_gap = props.column_gap = ER_LAYOUT_AUTO;
    props.flex_basis = ER_LAYOUT_AUTO;
    props.opacity = OPACITY_OPAQUE;
    return props;
}

/**
 * @brief Creates a View with the given props, optionally capturing its computed rect.
 *
 * @param[in]  props     Props to apply.
 * @param[out] out_rect  ERRect to receive the node's computed rect via ER_EVENT_LAYOUT, or NULL.
 *
 * @return The created node.
 */
static ERNode* mk(ERProps props, ERRect* out_rect)
{
    ERNode* node = er_node_create(ER_NODE_VIEW);
    er_node_set_props(node, &props);
    if (out_rect)
    {
        *out_rect = (ERRect){-1, -1, -1, -1};
        er_event_set(node, ER_EVENT_LAYOUT, on_rect, out_rect);
    }
    return node;
}

/** @brief Status of a parity assertion. */
typedef enum
{
    EXPECT, /**< Engine should match Yoga now. */
    XFAIL,  /**< Known divergence; engine does not yet match Yoga. */
} ParityStatus;

/**
 * @brief Compares a captured rect against the Yoga-correct value and records the outcome.
 *
 * @param[in] fixture  Fixture name (for output).
 * @param[in] label    Node label (for output).
 * @param[in] status   EXPECT or XFAIL.
 * @param[in] got      The engine's computed rect.
 * @param[in] x        Expected (Yoga) x.
 * @param[in] y        Expected (Yoga) y.
 * @param[in] width    Expected (Yoga) width.
 * @param[in] height   Expected (Yoga) height.
 */
static void
pcheck(const char* fixture, const char* label, ParityStatus status, ERRect got, int x, int y, int width, int height)
{
    const bool match = (got.x == x && got.y == y && got.w == width && got.h == height);
    if (status == EXPECT)
    {
        if (match)
        {
            g_pass++;
            printf("  [PASS ] %-20s %-8s (%d,%d,%d,%d)\n", fixture, label, got.x, got.y, got.w, got.h);
        }
        else
        {
            g_regress++;
            printf("  [FAIL ] %-20s %-8s got (%d,%d,%d,%d) want (%d,%d,%d,%d)\n",
                   fixture,
                   label,
                   got.x,
                   got.y,
                   got.w,
                   got.h,
                   x,
                   y,
                   width,
                   height);
        }
    }
    else
    {
        if (!match)
        {
            g_xfail++;
            printf("  [xfail] %-20s %-8s engine (%d,%d,%d,%d) != yoga (%d,%d,%d,%d)  [known gap]\n",
                   fixture,
                   label,
                   got.x,
                   got.y,
                   got.w,
                   got.h,
                   x,
                   y,
                   width,
                   height);
        }
        else
        {
            g_promote++;
            printf("  [PROMO] %-20s %-8s now matches yoga (%d,%d,%d,%d) -- change XFAIL to EXPECT\n",
                   fixture,
                   label,
                   got.x,
                   got.y,
                   got.w,
                   got.h);
        }
    }
}

/** @brief Detaches a child from its parent and destroys it. */
static void kill_child(ERNode* parent, ERNode* child)
{
    er_tree_remove_child(parent, child);
    er_node_destroy(child);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — fixtures
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Column stacks two fixed-size children top to bottom. */
static void fixture_column_stack(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 100;
    root_props.height = 100;
    root_props.flex_direction = ER_FLEX_COL;
    root_props.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(root_props, NULL);

    ERRect first_rect, second_rect;
    ERProps props = props_default();
    props.width = 40;
    props.height = 20;
    ERNode* first_child = mk(props, &first_rect);
    props.height = 30;
    ERNode* second_child = mk(props, &second_rect);
    er_tree_append_child(root, first_child);
    er_tree_append_child(root, second_child);

    er_tree_set_root(root);
    er_commit();

    pcheck("column-stack", "c0", EXPECT, first_rect, 0, 0, 40, 20);
    pcheck("column-stack", "c1", EXPECT, second_rect, 0, 20, 40, 30);

    kill_child(root, first_child);
    kill_child(root, second_child);
    er_node_destroy(root);
}

/** @brief Row with two flex:1 children splits the main axis evenly and stretches on cross. */
static void fixture_row_flex_even(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 100;
    root_props.height = 40;
    root_props.flex_direction = ER_FLEX_ROW;
    ERNode* root = mk(root_props, NULL);

    ERRect first_rect, second_rect;
    ERProps props = props_default();
    props.flex_grow = 1;
    props.flex_shrink = 1;
    props.flex_basis = 0;
    ERNode* first_child = mk(props, &first_rect);
    ERNode* second_child = mk(props, &second_rect);
    er_tree_append_child(root, first_child);
    er_tree_append_child(root, second_child);

    er_tree_set_root(root);
    er_commit();

    pcheck("row-flex-even", "c0", EXPECT, first_rect, 0, 0, 50, 40);
    pcheck("row-flex-even", "c1", EXPECT, second_rect, 50, 0, 50, 40);

    kill_child(root, first_child);
    kill_child(root, second_child);
    er_node_destroy(root);
}

/** @brief justifyContent: space-between pushes children to the edges. */
static void fixture_justify_between(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 100;
    root_props.height = 40;
    root_props.flex_direction = ER_FLEX_ROW;
    root_props.justify_content = ER_JUSTIFY_SPACE_BETWEEN;
    root_props.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(root_props, NULL);

    ERRect first_rect, second_rect;
    ERProps props = props_default();
    props.width = 20;
    props.height = 20;
    ERNode* first_child = mk(props, &first_rect);
    ERNode* second_child = mk(props, &second_rect);
    er_tree_append_child(root, first_child);
    er_tree_append_child(root, second_child);

    er_tree_set_root(root);
    er_commit();

    pcheck("justify-between", "c0", EXPECT, first_rect, 0, 0, 20, 20);
    pcheck("justify-between", "c1", EXPECT, second_rect, 80, 0, 20, 20);

    kill_child(root, first_child);
    kill_child(root, second_child);
    er_node_destroy(root);
}

/** @brief alignItems: center centers a child on the cross axis. */
static void fixture_align_center(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 100;
    root_props.height = 40;
    root_props.flex_direction = ER_FLEX_ROW;
    root_props.align_items = ER_ALIGN_CENTER;
    ERNode* root = mk(root_props, NULL);

    ERRect first_rect;
    ERProps props = props_default();
    props.width = 20;
    props.height = 20;
    ERNode* first_child = mk(props, &first_rect);
    er_tree_append_child(root, first_child);

    er_tree_set_root(root);
    er_commit();

    pcheck("align-center", "c0", EXPECT, first_rect, 0, 10, 20, 20);

    kill_child(root, first_child);
    er_node_destroy(root);
}

/** @brief Padding insets the content box; a stretch child fills it. */
static void fixture_padding_stretch(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 100;
    root_props.height = 100;
    root_props.flex_direction = ER_FLEX_COL;
    root_props.padding = 10;
    ERNode* root = mk(root_props, NULL);

    ERRect first_rect;
    ERProps props = props_default();
    props.height = 20;
    ERNode* first_child = mk(props, &first_rect);
    er_tree_append_child(root, first_child);

    er_tree_set_root(root);
    er_commit();

    pcheck("padding-stretch", "c0", EXPECT, first_rect, 10, 10, 80, 20);

    kill_child(root, first_child);
    er_node_destroy(root);
}

/** @brief gap inserts space between row children. */
static void fixture_gap_row(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 100;
    root_props.height = 40;
    root_props.flex_direction = ER_FLEX_ROW;
    root_props.gap = 10;
    root_props.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(root_props, NULL);

    ERRect first_rect, second_rect;
    ERProps props = props_default();
    props.width = 20;
    props.height = 20;
    ERNode* first_child = mk(props, &first_rect);
    ERNode* second_child = mk(props, &second_rect);
    er_tree_append_child(root, first_child);
    er_tree_append_child(root, second_child);

    er_tree_set_root(root);
    er_commit();

    pcheck("gap-row", "c0", EXPECT, first_rect, 0, 0, 20, 20);
    pcheck("gap-row", "c1", EXPECT, second_rect, 30, 0, 20, 20);

    kill_child(root, first_child);
    kill_child(root, second_child);
    er_node_destroy(root);
}

/** @brief A row with no explicit height grows to its children's height (auto cross-size). */
static void fixture_auto_cross_row(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 100;
    root_props.height = 100;
    root_props.flex_direction = ER_FLEX_COL;
    ERNode* root = mk(root_props, NULL);

    ERRect row_rect, first_rect, second_rect;
    ERProps row_props = props_default();
    row_props.flex_direction = ER_FLEX_ROW; /* no height — derived from children */
    ERNode* row = mk(row_props, &row_rect);
    er_tree_append_child(root, row);

    ERProps props = props_default();
    props.flex_grow = 1;
    props.flex_shrink = 1;
    props.flex_basis = 0;
    props.height = 30;
    ERNode* first_child = mk(props, &first_rect);
    ERNode* second_child = mk(props, &second_rect);
    er_tree_append_child(row, first_child);
    er_tree_append_child(row, second_child);

    er_tree_set_root(root);
    er_commit();

    pcheck("auto-cross-row", "row", EXPECT, row_rect, 0, 0, 100, 30);
    pcheck("auto-cross-row", "c0", EXPECT, first_rect, 0, 0, 50, 30);
    pcheck("auto-cross-row", "c1", EXPECT, second_rect, 50, 0, 50, 30);

    kill_child(row, first_child);
    kill_child(row, second_child);
    kill_child(root, row);
    er_node_destroy(root);
}

/** @brief A column with no explicit height grows to the sum of its children's heights. */
static void fixture_auto_main_column(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 100;
    root_props.height = 100;
    root_props.flex_direction = ER_FLEX_COL;
    root_props.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(root_props, NULL);

    ERRect inner_rect, first_rect, second_rect;
    ERProps inner_props = props_default();
    inner_props.flex_direction = ER_FLEX_COL; /* no width/height — derived from children */
    ERNode* inner = mk(inner_props, &inner_rect);
    er_tree_append_child(root, inner);

    ERProps props = props_default();
    props.width = 30;
    props.height = 20;
    ERNode* first_child = mk(props, &first_rect);
    ERNode* second_child = mk(props, &second_rect);
    er_tree_append_child(inner, first_child);
    er_tree_append_child(inner, second_child);

    er_tree_set_root(root);
    er_commit();

    pcheck("auto-main-col", "inner", EXPECT, inner_rect, 0, 0, 30, 40);
    pcheck("auto-main-col", "c0", EXPECT, first_rect, 0, 0, 30, 20);
    pcheck("auto-main-col", "c1", EXPECT, second_rect, 0, 20, 30, 20);

    kill_child(inner, first_child);
    kill_child(inner, second_child);
    kill_child(root, inner);
    er_node_destroy(root);
}

/**
 * @brief Iterative flex resolution: a flex child that hits maxWidth frees space for its siblings.
 *
 * Row 100 wide, two flex:1 children. c0 has maxWidth 20. Yoga (and now the engine) freezes c0 at
 * 20 and redistributes the freed 30 px to c1 (→ 80) via the iterative resolve-flexible-lengths
 * loop in Pass 3.
 */
static void fixture_flex_max_redistribute(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 100;
    root_props.height = 40;
    root_props.flex_direction = ER_FLEX_ROW;
    root_props.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(root_props, NULL);

    ERRect first_rect, second_rect;
    ERProps props = props_default();
    props.flex_grow = 1;
    props.flex_shrink = 1;
    props.flex_basis = 0;
    props.height = 20;
    props.max_width = 20;
    ERNode* first_child = mk(props, &first_rect);

    props = props_default();
    props.flex_grow = 1;
    props.flex_shrink = 1;
    props.flex_basis = 0;
    props.height = 20;
    ERNode* second_child = mk(props, &second_rect);
    er_tree_append_child(root, first_child);
    er_tree_append_child(root, second_child);

    er_tree_set_root(root);
    er_commit();

    /* c0 is clamped to 20; the freed 30px is redistributed to c1 (→ 80), matching Yoga. */
    pcheck("flex-max-redist", "c0", EXPECT, first_rect, 0, 0, 20, 20);
    pcheck("flex-max-redist", "c1", EXPECT, second_rect, 20, 0, 80, 20);

    kill_child(root, first_child);
    kill_child(root, second_child);
    er_node_destroy(root);
}

/**
 * @brief Builds a wrapping row of three 50×20 items (one per line) under the given alignContent.
 *
 * Container is 50 wide so each item wraps onto its own line; `height` controls the leftover cross
 * space. Captures the three item rects for the caller to assert.
 */
static void build_wrap3(
    uint8_t align_content, int16_t height, int16_t item_height, ERNode** root_out, ERNode** kids_out, ERRect* rects)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 50;
    root_props.height = height;
    root_props.flex_direction = ER_FLEX_ROW;
    root_props.flex_wrap = ER_WRAP_WRAP;
    root_props.align_content = align_content;
    ERNode* root = mk(root_props, NULL);

    for (int kid_index = 0; kid_index < 3; kid_index++)
    {
        ERProps props = props_default();
        props.width = 50;
        if (item_height != ER_LAYOUT_AUTO)
        {
            props.height = item_height;
        }
        kids_out[kid_index] = mk(props, &rects[kid_index]);
        er_tree_append_child(root, kids_out[kid_index]);
    }
    er_tree_set_root(root);
    *root_out = root;
}

/** @brief Tears down a wrap3 fixture. */
static void teardown_wrap3(ERNode* root, ERNode** kids)
{
    for (int kid_index = 0; kid_index < 3; kid_index++)
    {
        kill_child(root, kids[kid_index]);
    }
    er_node_destroy(root);
}

/** @brief alignContent: center — leftover cross space splits evenly above and below the lines. */
static void fixture_align_content_center(void)
{
    ERNode* root;
    ERNode* kids[3];
    ERRect rects[3];
    /* 3 lines × 20 = 60 used of 100 → 40 free; center → 20px leading offset. */
    build_wrap3(ER_ALIGN_CONTENT_CENTER, 100, 20, &root, kids, rects);
    er_commit();
    pcheck("aligncontent-center", "l0", EXPECT, rects[0], 0, 20, 50, 20);
    pcheck("aligncontent-center", "l1", EXPECT, rects[1], 0, 40, 50, 20);
    pcheck("aligncontent-center", "l2", EXPECT, rects[2], 0, 60, 50, 20);
    teardown_wrap3(root, kids);
}

/** @brief alignContent: space-between — leftover cross space goes between lines, none at edges. */
static void fixture_align_content_between(void)
{
    ERNode* root;
    ERNode* kids[3];
    ERRect rects[3];
    /* 40 free / (3-1) = 20px between lines. */
    build_wrap3(ER_ALIGN_CONTENT_SPACE_BETWEEN, 100, 20, &root, kids, rects);
    er_commit();
    pcheck("aligncontent-between", "l0", EXPECT, rects[0], 0, 0, 50, 20);
    pcheck("aligncontent-between", "l1", EXPECT, rects[1], 0, 40, 50, 20);
    pcheck("aligncontent-between", "l2", EXPECT, rects[2], 0, 80, 50, 20);
    teardown_wrap3(root, kids);
}

/** @brief alignContent: stretch — each line grows equally; auto-height items fill their line. */
static void fixture_align_content_stretch(void)
{
    ERNode* root;
    ERNode* kids[3];
    ERRect rects[3];
    /* Auto-height items: base line cross 0, 120 free / 3 lines = 40px tall lines; items stretch. */
    build_wrap3(ER_ALIGN_CONTENT_STRETCH, 120, ER_LAYOUT_AUTO, &root, kids, rects);
    er_commit();
    pcheck("aligncontent-stretch", "l0", EXPECT, rects[0], 0, 0, 50, 40);
    pcheck("aligncontent-stretch", "l1", EXPECT, rects[1], 0, 40, 50, 40);
    pcheck("aligncontent-stretch", "l2", EXPECT, rects[2], 0, 80, 50, 40);
    teardown_wrap3(root, kids);
}

/** @brief Percentage width on the main axis: 50% of a 200px row → 100px. */
static void fixture_pct_width_main(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 80;
    root_props.flex_direction = ER_FLEX_ROW;
    root_props.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(root_props, NULL);

    ERRect first_rect;
    ERProps props = props_default();
    props.width_pct = 50.0f;
    props.height = 40;
    ERNode* first_child = mk(props, &first_rect);
    er_tree_append_child(root, first_child);

    er_tree_set_root(root);
    er_commit();
    pcheck("pct-width-main", "c0", EXPECT, first_rect, 0, 0, 100, 40);

    kill_child(root, first_child);
    er_node_destroy(root);
}

/** @brief Percentage height on the main axis: 25% of a 200px column → 50px. */
static void fixture_pct_height_main(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 100;
    root_props.height = 200;
    root_props.flex_direction = ER_FLEX_COL;
    root_props.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(root_props, NULL);

    ERRect first_rect;
    ERProps props = props_default();
    props.width = 30;
    props.height_pct = 25.0f;
    ERNode* first_child = mk(props, &first_rect);
    er_tree_append_child(root, first_child);

    er_tree_set_root(root);
    er_commit();
    pcheck("pct-height-main", "c0", EXPECT, first_rect, 0, 0, 30, 50);

    kill_child(root, first_child);
    er_node_destroy(root);
}

/** @brief Percentage width on the cross axis: 50% of a 200px-wide column → 100px. */
static void fixture_pct_width_cross(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 100;
    root_props.flex_direction = ER_FLEX_COL;
    root_props.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(root_props, NULL);

    ERRect first_rect;
    ERProps props = props_default();
    props.width_pct = 50.0f;
    props.height = 20;
    ERNode* first_child = mk(props, &first_rect);
    er_tree_append_child(root, first_child);

    er_tree_set_root(root);
    er_commit();
    pcheck("pct-width-cross", "c0", EXPECT, first_rect, 0, 0, 100, 20);

    kill_child(root, first_child);
    er_node_destroy(root);
}

/** @brief Percentages resolve against the parent's content box: 50% of (200 − 2×20 padding) = 80. */
static void fixture_pct_content_box(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 80;
    root_props.flex_direction = ER_FLEX_ROW;
    root_props.align_items = ER_ALIGN_FLEX_START;
    root_props.padding = 20;
    ERNode* root = mk(root_props, NULL);

    ERRect first_rect;
    ERProps props = props_default();
    props.width_pct = 50.0f;
    props.height = 40;
    ERNode* first_child = mk(props, &first_rect);
    er_tree_append_child(root, first_child);

    er_tree_set_root(root);
    er_commit();
    pcheck("pct-content-box", "c0", EXPECT, first_rect, 20, 20, 80, 40);

    kill_child(root, first_child);
    er_node_destroy(root);
}

/**
 * @brief Absolute with left/top/width and no height sizes to its content (issue #94).
 *
 * Yoga lays an auto axis of an absolute child out at its max-content size, exactly as it does for a
 * flow child; the engine used to collapse it to 0, which painted correctly but made the whole subtree
 * untouchable.
 */
static void fixture_abs_auto_height(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    root_props.flex_direction = ER_FLEX_COL;
    root_props.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect, kid_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.left = 20;
    absolute_props.top = 30;
    absolute_props.width = 100;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    ERProps kid_props = props_default();
    kid_props.width = 80;
    kid_props.height = 40;
    ERNode* kid = mk(kid_props, &kid_rect);

    er_tree_append_child(absolute, kid);
    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-auto-height", "abs", EXPECT, absolute_rect, 20, 30, 100, 40);
    pcheck("abs-auto-height", "kid", EXPECT, kid_rect, 0, 0, 80, 40);

    kill_child(absolute, kid);
    kill_child(root, absolute);
    er_node_destroy(root);
}

/** @brief Absolute with neither width nor height sizes both axes to content (70 wide, 20+30 tall). */
static void fixture_abs_auto_both(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.left = 10;
    absolute_props.top = 10;
    absolute_props.flex_direction = ER_FLEX_COL;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    ERProps kid_props = props_default();
    kid_props.width = 60;
    kid_props.height = 20;
    ERNode* first_kid = mk(kid_props, NULL);
    kid_props.width = 70;
    kid_props.height = 30;
    ERNode* second_kid = mk(kid_props, NULL);

    er_tree_append_child(absolute, first_kid);
    er_tree_append_child(absolute, second_kid);
    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-auto-both", "abs", EXPECT, absolute_rect, 10, 10, 70, 50);

    kill_child(absolute, first_kid);
    kill_child(absolute, second_kid);
    kill_child(root, absolute);
    er_node_destroy(root);
}

/** @brief Percentage width/height on an absolute resolve against the containing block: 50%/25% of 200x100. */
static void fixture_abs_pct(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 100;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.left = 10;
    absolute_props.top = 5;
    absolute_props.width_pct = 50.0f;
    absolute_props.height_pct = 25.0f;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-pct", "abs", EXPECT, absolute_rect, 10, 5, 100, 25);

    kill_child(root, absolute);
    er_node_destroy(root);
}

/** @brief aspectRatio fills an absolute's auto axis from the resolved one: width 100, ratio 2 → height 50. */
static void fixture_abs_aspect_ratio(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.left = 0;
    absolute_props.top = 0;
    absolute_props.width = 100;
    absolute_props.aspect_ratio = 2.0f;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-aspect-ratio", "abs", EXPECT, absolute_rect, 0, 0, 100, 50);

    kill_child(root, absolute);
    er_node_destroy(root);
}

/**
 * @brief aspectRatio needs one axis already pinned; with both auto it is ignored and content wins.
 *
 * Yoga derives the missing axis from aspectRatio only when exactly one of width/height is resolved by
 * an explicit length, a percentage, or opposing insets. With both auto there is nothing to derive
 * from, so the node takes its max-content size on both axes and the ratio never applies.
 */
static void fixture_abs_aspect_ratio_both_auto(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.left = 0;
    absolute_props.top = 0;
    absolute_props.aspect_ratio = 2.0f;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    ERProps kid_props = props_default();
    kid_props.width = 60;
    kid_props.height = 25;
    ERNode* kid = mk(kid_props, NULL);

    er_tree_append_child(absolute, kid);
    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-aspect-both-auto", "abs", EXPECT, absolute_rect, 0, 0, 60, 25);

    kill_child(absolute, kid);
    kill_child(root, absolute);
    er_node_destroy(root);
}

/** @brief In a row, aspectRatio gives a flow child's width from its height, explicit or a percentage. */
static void fixture_flow_aspect_ratio_row(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 300;
    root_props.height = 120;
    root_props.flex_direction = ER_FLEX_ROW;
    root_props.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(root_props, NULL);

    ERRect first_rect, second_rect, third_rect;
    ERProps props = props_default();
    props.height = 80;
    props.aspect_ratio = 0.75f;
    er_tree_append_child(root, mk(props, &first_rect));

    props = props_default();
    props.height_pct = 50.0f;
    props.aspect_ratio = 2.0f;
    er_tree_append_child(root, mk(props, &second_rect));

    props = props_default();
    props.width = 20;
    props.height = 20;
    er_tree_append_child(root, mk(props, &third_rect));

    er_tree_set_root(root);
    er_commit();
    pcheck("flow-aspect-row", "a", EXPECT, first_rect, 0, 0, 60, 80);
    pcheck("flow-aspect-row", "b", EXPECT, second_rect, 60, 0, 120, 60);
    pcheck("flow-aspect-row", "c", EXPECT, third_rect, 180, 0, 20, 20);
    er_reset();
}

/** @brief In a column, aspectRatio gives a flow child's height from its width: width 100, ratio 2 → 50. */
static void fixture_flow_aspect_ratio_column(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    root_props.flex_direction = ER_FLEX_COL;
    root_props.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(root_props, NULL);

    ERRect first_rect, marker_rect;
    ERProps props = props_default();
    props.width = 100;
    props.aspect_ratio = 2.0f;
    er_tree_append_child(root, mk(props, &first_rect));

    props = props_default();
    props.width = 10;
    props.height = 10;
    er_tree_append_child(root, mk(props, &marker_rect));

    er_tree_set_root(root);
    er_commit();
    pcheck("flow-aspect-col", "a", EXPECT, first_rect, 0, 0, 100, 50);
    pcheck("flow-aspect-col", "marker", EXPECT, marker_rect, 0, 50, 10, 10);
    er_reset();
}

/** @brief The cross size is clamped before aspectRatio turns it into a main size, as in CSS: 40 high → 80 wide. */
static void fixture_flow_aspect_ratio_clamped(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 300;
    root_props.height = 120;
    root_props.flex_direction = ER_FLEX_ROW;
    root_props.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(root_props, NULL);

    ERRect first_rect;
    ERProps props = props_default();
    props.height = 80;
    props.max_height = 40;
    props.aspect_ratio = 2.0f;
    er_tree_append_child(root, mk(props, &first_rect));

    er_tree_set_root(root);
    er_commit();
    pcheck("flow-aspect-clamped", "a", EXPECT, first_rect, 0, 0, 80, 40);
    er_reset();
}

/** @brief An auto-sized container measures an aspectRatio child at the size the ratio gives it. */
static void fixture_flow_aspect_ratio_auto_parent(void)
{
    er_reset();

    /* An auto-width row in a column is measured with its children flexed. */
    ERProps root_props = props_default();
    root_props.width = 300;
    root_props.height = 200;
    root_props.flex_direction = ER_FLEX_COL;
    root_props.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(root_props, NULL);

    ERRect row, first_rect;
    ERProps props = props_default();
    props.flex_direction = ER_FLEX_ROW;
    props.align_items = ER_ALIGN_FLEX_START;
    ERNode* row_node = mk(props, &row);
    er_tree_append_child(root, row_node);

    props = props_default();
    props.height = 80;
    props.aspect_ratio = 0.75f;
    er_tree_append_child(row_node, mk(props, &first_rect));

    props = props_default();
    props.width = 20;
    props.height = 20;
    er_tree_append_child(row_node, mk(props, NULL));

    er_tree_set_root(root);
    er_commit();
    pcheck("flow-aspect-auto", "row", EXPECT, row, 0, 0, 80, 80);
    pcheck("flow-aspect-auto", "a", EXPECT, first_rect, 0, 0, 60, 80);
    er_reset();

    /* An auto-width column in a row measures its children one by one. */
    root_props.flex_direction = ER_FLEX_ROW;
    root = mk(root_props, NULL);

    ERRect column_rect;
    props = props_default();
    props.align_items = ER_ALIGN_FLEX_START;
    ERNode* column = mk(props, &column_rect);
    er_tree_append_child(root, column);

    props = props_default();
    props.height = 80;
    props.aspect_ratio = 0.75f;
    er_tree_append_child(column, mk(props, &first_rect));

    props = props_default();
    props.width = 20;
    props.height = 20;
    er_tree_append_child(column, mk(props, NULL));

    er_tree_set_root(root);
    er_commit();
    pcheck("flow-aspect-auto", "col", EXPECT, column_rect, 0, 0, 60, 100);
    pcheck("flow-aspect-auto", "a", EXPECT, first_rect, 0, 0, 60, 80);
    er_reset();
}

/** @brief An auto-height parent measures a child with a percentage width and aspectRatio at its ratio height. */
static void fixture_flow_aspect_ratio_pct_measured(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 300;
    root_props.height = 300;
    root_props.flex_direction = ER_FLEX_ROW;
    root_props.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(root_props, NULL);

    ERRect column_rect, first_rect;
    ERProps props = props_default();
    props.width = 200;
    props.align_items = ER_ALIGN_FLEX_START;
    ERNode* column = mk(props, &column_rect);
    er_tree_append_child(root, column);

    props = props_default();
    props.width_pct = 50.0f;
    props.aspect_ratio = 2.0f;
    er_tree_append_child(column, mk(props, &first_rect));

    er_tree_set_root(root);
    er_commit();
    pcheck("flow-aspect-pct-meas", "col", EXPECT, column_rect, 0, 0, 200, 50);
    pcheck("flow-aspect-pct-meas", "a", EXPECT, first_rect, 0, 0, 100, 50);
    er_reset();
}

/**
 * @brief Builds a container holding one aspectRatio child that flexes, and captures the child's rect.
 *
 * @param[in]  row     Whether the container is a row.
 * @param[in]  width   Container width.
 * @param[in]  height  Container height.
 * @param[in]  size    The child's main size.
 * @param[in]  grow    The child's flex_grow.
 * @param[in]  shrink  The child's flex_shrink.
 * @param[out] out     Receives the child's rect.
 */
static void build_flexed_aspect(
    const bool row, int16_t width, int16_t height, int16_t size, int16_t grow, int16_t shrink, ERRect* out)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = width;
    root_props.height = height;
    root_props.flex_direction = row ? ER_FLEX_ROW : ER_FLEX_COL;
    root_props.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(root_props, NULL);

    ERProps props = props_default();
    if (row)
        props.width = size;
    else
        props.height = size;
    props.aspect_ratio = 2.0f;
    props.flex_grow = grow;
    props.flex_shrink = shrink;
    er_tree_append_child(root, mk(props, out));

    er_tree_set_root(root);
    er_commit();
}

/** @brief aspectRatio gives the cross size from the main size an item flexes to, not its basis. */
static void fixture_flow_aspect_ratio_flexed(void)
{
    ERRect first_rect;
    build_flexed_aspect(true, 80, 200, 100, 0, 1, &first_rect);
    pcheck("flow-aspect-flexed", "shrink", EXPECT, first_rect, 0, 0, 80, 40);
    build_flexed_aspect(true, 200, 200, 50, 1, 0, &first_rect);
    pcheck("flow-aspect-flexed", "grow", EXPECT, first_rect, 0, 0, 200, 100);
    build_flexed_aspect(false, 300, 80, 100, 0, 1, &first_rect);
    pcheck("flow-aspect-flexed", "col", EXPECT, first_rect, 0, 0, 160, 80);
    er_reset();

    /* An auto-height row is measured at the height its shrunk child ends up with. */
    ERProps root_props = props_default();
    root_props.width = 300;
    root_props.height = 200;
    root_props.flex_direction = ER_FLEX_COL;
    root_props.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(root_props, NULL);

    ERRect row;
    ERProps props = props_default();
    props.width = 80;
    props.flex_direction = ER_FLEX_ROW;
    props.align_items = ER_ALIGN_FLEX_START;
    ERNode* row_node = mk(props, &row);
    er_tree_append_child(root, row_node);

    props = props_default();
    props.width = 100;
    props.aspect_ratio = 2.0f;
    props.flex_shrink = 1;
    er_tree_append_child(row_node, mk(props, &first_rect));

    er_tree_set_root(root);
    er_commit();
    pcheck("flow-aspect-flexed", "row", EXPECT, row, 0, 0, 80, 40);
    pcheck("flow-aspect-flexed", "a", EXPECT, first_rect, 0, 0, 80, 40);
    er_reset();
}

/** @brief Stretch does not override a cross size that aspectRatio gives: width 100, ratio 2 stays 50 high. */
static void fixture_flow_aspect_ratio_stretch(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 300;
    root_props.height = 120;
    root_props.flex_direction = ER_FLEX_ROW;
    ERNode* root = mk(root_props, NULL);

    ERRect first_rect;
    ERProps props = props_default();
    props.width = 100;
    props.aspect_ratio = 2.0f;
    er_tree_append_child(root, mk(props, &first_rect));

    er_tree_set_root(root);
    er_commit();
    pcheck("flow-aspect-stretch", "a", EXPECT, first_rect, 0, 0, 100, 50);
    er_reset();
}

/** @brief Content sizing is max-content, not "at most the parent": a 40x120 child in a 100x50 root overflows. */
static void fixture_abs_content_overflows(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 100;
    root_props.height = 50;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.left = 0;
    absolute_props.top = 0;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    ERProps kid_props = props_default();
    kid_props.width = 40;
    kid_props.height = 120;
    ERNode* kid = mk(kid_props, NULL);

    er_tree_append_child(absolute, kid);
    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-overflow", "abs", EXPECT, absolute_rect, 0, 0, 40, 120);

    kill_child(absolute, kid);
    kill_child(root, absolute);
    er_node_destroy(root);
}

/**
 * @brief An absolute's containing block is the parent's PADDING box, not its content box.
 *
 * With padding 20 on a 200x200 parent, `left:0` sits at the padding edge (0,0) -- the parent's padding
 * does not inset it -- and `width:50%` is half the full 200, not half the 160px content width.
 */
static void fixture_abs_containing_block(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    root_props.padding = 20;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.left = 0;
    absolute_props.top = 0;
    absolute_props.width_pct = 50.0f;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    ERProps kid_props = props_default();
    kid_props.width = 30;
    kid_props.height = 25;
    ERNode* kid = mk(kid_props, NULL);

    er_tree_append_child(absolute, kid);
    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-containing-block", "abs", EXPECT, absolute_rect, 0, 0, 100, 25);

    kill_child(absolute, kid);
    kill_child(root, absolute);
    er_node_destroy(root);
}

/**
 * @brief Every edge of the containing block is a padding edge -- checked against asymmetric padding.
 *
 * Padding 10/5/30/15 (l/t/r/b) on a 200x200 parent: `left:0/top:0` lands at (0,0); `right:0/bottom:0`
 * measures from the far padding edge at 200, so a 20px box lands at 180; and `50%` is 100 on both axes.
 * The percentage node pins no inset, so its ORIGIN falls back to the static position -- the flow
 * position it would have had, which is inside the content box at (10,5).
 */
static void fixture_abs_padding_box_edges(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    root_props.padding_left = 10;
    root_props.padding_top = 5;
    root_props.padding_right = 30;
    root_props.padding_bottom = 15;
    ERNode* root = mk(root_props, NULL);

    ERRect first_rect, second_rect, third_rect;
    ERProps first_props = props_default();
    first_props.position = ER_POS_ABSOLUTE;
    first_props.left = 0;
    first_props.top = 0;
    first_props.width = 20;
    first_props.height = 20;
    ERNode* first = mk(first_props, &first_rect);

    ERProps second_props = props_default();
    second_props.position = ER_POS_ABSOLUTE;
    second_props.right = 0;
    second_props.bottom = 0;
    second_props.width = 20;
    second_props.height = 20;
    ERNode* second = mk(second_props, &second_rect);

    ERProps third_props = props_default();
    third_props.position = ER_POS_ABSOLUTE;
    third_props.width_pct = 50.0f;
    third_props.height_pct = 50.0f;
    ERNode* third = mk(third_props, &third_rect);

    er_tree_append_child(root, first);
    er_tree_append_child(root, second);
    er_tree_append_child(root, third);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-padding-edges", "left/top", EXPECT, first_rect, 0, 0, 20, 20);
    pcheck("abs-padding-edges", "rt/bot", EXPECT, second_rect, 180, 180, 20, 20);
    pcheck("abs-padding-edges", "pct", EXPECT, third_rect, 10, 5, 100, 100);

    kill_child(root, first);
    kill_child(root, second);
    kill_child(root, third);
    er_node_destroy(root);
}

/** @brief Opposing insets size against the padding box too: 200 - 10 - 30 = 160 wide, x = 10. */
static void fixture_abs_inset_pair_padding(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    root_props.padding = 20;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.left = 10;
    absolute_props.right = 30;
    absolute_props.top = 0;
    absolute_props.bottom = 0;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-inset-pair-pad", "abs", EXPECT, absolute_rect, 10, 0, 160, 200);

    kill_child(root, absolute);
    er_node_destroy(root);
}

/** @brief Percentage insets on an absolute measure from the padding box: left 10% and top 25% of 200. */
static void fixture_abs_pct_inset(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    root_props.padding_left = 10;
    root_props.padding_top = 5;
    root_props.padding_right = 30;
    root_props.padding_bottom = 15;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.left_pct = 10.0f;
    absolute_props.top_pct = 25.0f;
    absolute_props.width = 20;
    absolute_props.height = 20;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-pct-inset", "abs", EXPECT, absolute_rect, 20, 50, 20, 20);

    kill_child(root, absolute);
    er_node_destroy(root);
}

/** @brief Each percentage inset takes its OWN axis: left 50% of 200, top 50% of 100. */
static void fixture_abs_pct_inset_axis(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 100;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.left_pct = 50.0f;
    absolute_props.top_pct = 50.0f;
    absolute_props.width = 10;
    absolute_props.height = 10;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-pct-axis", "abs", EXPECT, absolute_rect, 100, 50, 10, 10);

    kill_child(root, absolute);
    er_node_destroy(root);
}

/** @brief Percentage right/bottom anchor the far edges: 200 - 20 - 20 and 100 - 20 - 20. */
static void fixture_abs_pct_inset_far(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 100;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.right_pct = 10.0f;
    absolute_props.bottom_pct = 20.0f;
    absolute_props.width = 20;
    absolute_props.height = 20;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-pct-far", "abs", EXPECT, absolute_rect, 160, 60, 20, 20);

    kill_child(root, absolute);
    er_node_destroy(root);
}

/** @brief A pair of percentage insets sizes the axis: 200 - 20 - 50 wide, 100 - 10 - 30 tall. */
static void fixture_abs_pct_inset_pair(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 100;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.left_pct = 10.0f;
    absolute_props.right_pct = 25.0f;
    absolute_props.top_pct = 10.0f;
    absolute_props.bottom_pct = 30.0f;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-pct-pair", "abs", EXPECT, absolute_rect, 20, 10, 130, 60);

    kill_child(root, absolute);
    er_node_destroy(root);
}

/** @brief A negative percentage inset pushes the node outside its containing block: -10% of 200. */
static void fixture_abs_pct_inset_negative(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 100;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.left_pct = -10.0f;
    absolute_props.top_pct = -20.0f;
    absolute_props.width = 20;
    absolute_props.height = 20;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-pct-negative", "abs", EXPECT, absolute_rect, -20, -20, 20, 20);

    kill_child(root, absolute);
    er_node_destroy(root);
}

/** @brief Margin still shifts a percentage-inset absolute: 10% of 200 + marginLeft 5. */
static void fixture_abs_pct_inset_margin(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.left_pct = 10.0f;
    absolute_props.top_pct = 10.0f;
    absolute_props.margin_left = 5;
    absolute_props.margin_top = 7;
    absolute_props.width = 20;
    absolute_props.height = 20;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-pct-margin", "abs", EXPECT, absolute_rect, 25, 27, 20, 20);

    kill_child(root, absolute);
    er_node_destroy(root);
}

/**
 * @brief A percentage inset on a half pixel rounds once, at the edge it lands on.
 *
 * right is 50% of 201 = 100.5, so the left edge is 201 - 100.5 - 20 = 80.5 and rounds UP to 81.
 * Rounding the inset first (to 101) would put the node a pixel early.
 */
static void fixture_abs_pct_inset_half_pixel(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 201;
    root_props.height = 100;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.right_pct = 50.0f;
    absolute_props.width = 20;
    absolute_props.height = 20;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-pct-half-px", "abs", EXPECT, absolute_rect, 81, 0, 20, 20);

    kill_child(root, absolute);
    er_node_destroy(root);
}

/**
 * @brief A half-pixel percentage PAIR spans one rounded edge to the next.
 *
 * x runs 100.5 -> 150.75 and y 50.5 -> 75.75, so the rounded edges are 101/151 and 51/76: 50x25.
 */
static void fixture_abs_pct_pair_half_pixel(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 201;
    root_props.height = 101;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.left_pct = 50.0f;
    absolute_props.right_pct = 25.0f;
    absolute_props.top_pct = 50.0f;
    absolute_props.bottom_pct = 25.0f;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-pct-pair-half", "abs", EXPECT, absolute_rect, 101, 51, 50, 25);

    kill_child(root, absolute);
    er_node_destroy(root);
}

/** @brief right/bottom alone still size the node from its content: 60x40 pinned 30/10 off the far edges. */
static void fixture_abs_far_inset_auto_size(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.right = 30;
    absolute_props.bottom = 10;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    ERProps kid_props = props_default();
    kid_props.width = 60;
    kid_props.height = 40;
    ERNode* kid = mk(kid_props, NULL);

    er_tree_append_child(absolute, kid);
    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-far-auto", "abs", EXPECT, absolute_rect, 110, 150, 60, 40);

    kill_child(absolute, kid);
    kill_child(root, absolute);
    er_node_destroy(root);
}

/**
 * @brief A percentage wild enough to saturate the pixel grid still yields a sane box.
 *
 * Each edge clamps into int16 on its own, so a left edge pinned far off one end and a width running far
 * off the other leave a difference twice as wide as an int16 — which used to wrap the subtraction into a
 * NEGATIVE width. Yoga has no int16 to overflow, so there is no reference rect here: the assertion is
 * that the box stays a box.
 */
static void fixture_abs_pct_saturates(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 480;
    root_props.height = 320;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.left_pct = -10000.0f;  /* -48000 px, clamped to the int16 floor. */
    absolute_props.width_pct = 20000.0f;  /* 96000 px, so the far edge clamps to the ceiling. */
    absolute_props.top_pct = 0.0f / 0.0f; /* A style may spell a percentage "NaN%". */
    absolute_props.height = 20;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-pct-saturate", "abs", EXPECT, absolute_rect, -32768, 0, 32767, 20);

    kill_child(root, absolute);
    er_node_destroy(root);
}

/**
 * @brief A RELATIVE percentage offset measures against the parent's CONTENT box, not its padding box.
 *
 * Content is 160x80, so left 10% = 16 and top 25% = 20 shift the flow position (10, 5) to (26, 25).
 */
static void fixture_rel_pct_offset(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 100;
    root_props.padding_left = 10;
    root_props.padding_top = 5;
    root_props.padding_right = 30;
    root_props.padding_bottom = 15;
    ERNode* root = mk(root_props, NULL);

    ERRect first_rect;
    ERProps first_props = props_default();
    first_props.left_pct = 10.0f;
    first_props.top_pct = 25.0f;
    first_props.width = 20;
    first_props.height = 20;
    ERNode* first = mk(first_props, &first_rect);

    er_tree_append_child(root, first);
    er_tree_set_root(root);
    er_commit();
    pcheck("rel-pct-offset", "a", EXPECT, first_rect, 26, 25, 20, 20);

    kill_child(root, first);
    er_node_destroy(root);
}

/** @brief A relative right/bottom percentage shifts the flow position back: (10, 5) - (16, 20). */
static void fixture_rel_pct_offset_far(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 100;
    root_props.padding_left = 10;
    root_props.padding_top = 5;
    root_props.padding_right = 30;
    root_props.padding_bottom = 15;
    ERNode* root = mk(root_props, NULL);

    ERRect first_rect;
    ERProps first_props = props_default();
    first_props.right_pct = 10.0f;
    first_props.bottom_pct = 25.0f;
    first_props.width = 20;
    first_props.height = 20;
    ERNode* first = mk(first_props, &first_rect);

    er_tree_append_child(root, first);
    er_tree_set_root(root);
    er_commit();
    pcheck("rel-pct-far", "a", EXPECT, first_rect, -6, -15, 20, 20);

    kill_child(root, first);
    er_node_destroy(root);
}

/** @brief Margin still shifts an inset-positioned absolute, measured from the padding edge: 0 + 10 + 5. */
static void fixture_abs_margin_inset_padding(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    root_props.padding = 20;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.left = 10;
    absolute_props.top = 10;
    absolute_props.width = 50;
    absolute_props.height = 40;
    absolute_props.margin = 5;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-margin-inset-pad", "abs", EXPECT, absolute_rect, 15, 15, 50, 40);

    kill_child(root, absolute);
    er_node_destroy(root);
}

/**
 * @brief An absolute with NO insets keeps the parent's padding -- its static position is the flow one.
 *
 * The counterpart to abs-containing-block: padding is irrelevant to an inset, but it still applies when
 * there is no inset to measure from, so an unpinned absolute starts where a flow child would.
 */
static void fixture_abs_static_position_padding(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    root_props.padding = 20;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.width = 50;
    absolute_props.height = 40;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-static-pos-pad", "abs", EXPECT, absolute_rect, 20, 20, 50, 40);

    kill_child(root, absolute);
    er_node_destroy(root);
}

/**
 * @brief Static position honours justifyContent / alignItems: a 50x40 box centres at (75,80).
 *
 * The counterpart to abs-static-pos-pad. With no inset on either axis the child takes the spot it
 * would have had in flow, so a centring parent centres it in its 160x160 content box.
 */
static void fixture_abs_static_position_align(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    root_props.padding = 20;
    root_props.flex_direction = ER_FLEX_COL;
    root_props.justify_content = ER_JUSTIFY_CENTER;
    root_props.align_items = ER_ALIGN_CENTER;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.width = 50;
    absolute_props.height = 40;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-static-pos-align", "abs", EXPECT, absolute_rect, 75, 80, 50, 40);

    kill_child(root, absolute);
    er_node_destroy(root);
}

/**
 * @brief Builds a padded parent holding one uninset absolute and checks where it lands.
 *
 * The static-position fixtures below differ only in a couple of style fields, so they share this
 * builder rather than repeating the same twelve lines. Parent is always 200x200 with padding 20, so
 * the content box is (20,20,160,160); the child is 50x40 unless auto_size asks for a measured one.
 *
 * @param[in] label           Assertion label.
 * @param[in] status          EXPECT or XFAIL.
 * @param[in] root_props      Parent props (already carrying the direction / justify / align under test).
 * @param[in] absolute_props  Absolute child props (position and size are filled in here).
 * @param[in] auto_size       true to leave width/height auto and give the child a 24x18 child to measure.
 * @param[in] x,y,width,height  Expected (Yoga) rect.
 */
static void static_pos_case(const char* label,
                            ParityStatus status,
                            ERProps root_props,
                            ERProps absolute_props,
                            bool auto_size,
                            int x,
                            int y,
                            int width,
                            int height)
{
    er_reset();

    root_props.width = 200;
    root_props.height = 200;
    root_props.padding = 20;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    absolute_props.position = ER_POS_ABSOLUTE;
    if (!auto_size)
    {
        absolute_props.width = 50;
        absolute_props.height = 40;
    }
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    ERNode* kid = NULL;
    if (auto_size)
    {
        ERProps kid_props = props_default();
        kid_props.width = 24;
        kid_props.height = 18;
        kid = mk(kid_props, NULL);
        er_tree_append_child(absolute, kid);
    }

    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-static", label, status, absolute_rect, x, y, width, height);

    if (kid)
        kill_child(absolute, kid);
    kill_child(root, absolute);
    er_node_destroy(root);
}

/**
 * @brief Every justifyContent mode places the uninset absolute as the SOLE flex item.
 *
 * With nothing to distribute against, space-between collapses to flex-start and space-around /
 * space-evenly both centre — which is what Yoga produces, and what makes the static position
 * independent of however many in-flow siblings the parent happens to have.
 */
static void fixture_abs_static_justify(void)
{
    ERProps root_props = props_default();
    root_props.flex_direction = ER_FLEX_COL;
    ERProps absolute_props = props_default();

    root_props.justify_content = ER_JUSTIFY_FLEX_START;
    static_pos_case("just-start", EXPECT, root_props, absolute_props, false, 20, 20, 50, 40);
    root_props.justify_content = ER_JUSTIFY_CENTER;
    static_pos_case("just-center", EXPECT, root_props, absolute_props, false, 20, 80, 50, 40);
    root_props.justify_content = ER_JUSTIFY_FLEX_END;
    static_pos_case("just-end", EXPECT, root_props, absolute_props, false, 20, 140, 50, 40);
    root_props.justify_content = ER_JUSTIFY_SPACE_BETWEEN;
    static_pos_case("just-between", EXPECT, root_props, absolute_props, false, 20, 20, 50, 40);
    root_props.justify_content = ER_JUSTIFY_SPACE_AROUND;
    static_pos_case("just-around", EXPECT, root_props, absolute_props, false, 20, 80, 50, 40);
    root_props.justify_content = ER_JUSTIFY_SPACE_EVENLY;
    static_pos_case("just-evenly", EXPECT, root_props, absolute_props, false, 20, 80, 50, 40);

    /* Row swaps the axes: the same modes now move x instead of y. */
    root_props.flex_direction = ER_FLEX_ROW;
    root_props.justify_content = ER_JUSTIFY_CENTER;
    static_pos_case("just-center-row", EXPECT, root_props, absolute_props, false, 75, 20, 50, 40);
    root_props.justify_content = ER_JUSTIFY_FLEX_END;
    static_pos_case("just-end-row", EXPECT, root_props, absolute_props, false, 130, 20, 50, 40);
}

/** @brief alignItems places the uninset absolute on the cross axis; alignSelf overrides it. */
static void fixture_abs_static_align(void)
{
    ERProps root_props = props_default();
    root_props.flex_direction = ER_FLEX_COL;
    ERProps absolute_props = props_default();

    root_props.align_items = ER_ALIGN_FLEX_START;
    static_pos_case("align-start", EXPECT, root_props, absolute_props, false, 20, 20, 50, 40);
    root_props.align_items = ER_ALIGN_CENTER;
    static_pos_case("align-center", EXPECT, root_props, absolute_props, false, 75, 20, 50, 40);
    root_props.align_items = ER_ALIGN_FLEX_END;
    static_pos_case("align-end", EXPECT, root_props, absolute_props, false, 130, 20, 50, 40);

    /* Row swaps the axes: alignItems now moves y. */
    root_props.flex_direction = ER_FLEX_ROW;
    root_props.align_items = ER_ALIGN_CENTER;
    static_pos_case("align-center-row", EXPECT, root_props, absolute_props, false, 20, 80, 50, 40);

    /* alignSelf wins over the parent's alignItems, exactly as it does for a flow child. */
    root_props.flex_direction = ER_FLEX_COL;
    root_props.align_items = ER_ALIGN_CENTER;
    absolute_props.align_self = ER_ALIGN_FLEX_END;
    static_pos_case("align-self-end", EXPECT, root_props, absolute_props, false, 130, 20, 50, 40);
    absolute_props.align_self = ER_ALIGN_FLEX_START;
    static_pos_case("align-self-start", EXPECT, root_props, absolute_props, false, 20, 20, 50, 40);
}

/**
 * @brief alignItems 'stretch' does NOT stretch an absolute — it lands at the cross start, content-sized.
 *
 * Stretch is the DEFAULT alignItems, so this is the case that matters most: an auto-sized absolute in
 * an ordinary parent keeps its measured 24x18 rather than being grown to the 160px content width.
 */
static void fixture_abs_static_stretch(void)
{
    ERProps root_props = props_default();
    root_props.flex_direction = ER_FLEX_COL;
    root_props.align_items = ER_ALIGN_STRETCH;
    ERProps absolute_props = props_default();
    static_pos_case("stretch-auto", EXPECT, root_props, absolute_props, true, 20, 20, 24, 18);

    /* Same for an explicitly sized child: stretch places it like flex-start. */
    static_pos_case("stretch-sized", EXPECT, root_props, absolute_props, false, 20, 20, 50, 40);
}

/**
 * @brief A reversed axis measures the static position from the far edge, using the TRAILING margin.
 *
 * column-reverse turns flex-start into "against the bottom", and the margin that holds the child off
 * that edge is marginBottom, not marginTop. wrap-reverse does the same to the cross axis.
 */
static void fixture_abs_static_reverse(void)
{
    ERProps root_props = props_default();
    ERProps absolute_props = props_default();

    root_props.flex_direction = ER_FLEX_COL_REVERSE;
    static_pos_case("rev-col-start", EXPECT, root_props, absolute_props, false, 20, 140, 50, 40);
    root_props.justify_content = ER_JUSTIFY_FLEX_END;
    static_pos_case("rev-col-end", EXPECT, root_props, absolute_props, false, 20, 20, 50, 40);

    root_props = props_default();
    root_props.flex_direction = ER_FLEX_ROW_REVERSE;
    static_pos_case("rev-row-start", EXPECT, root_props, absolute_props, false, 130, 20, 50, 40);
    root_props.align_items = ER_ALIGN_FLEX_END;
    static_pos_case("rev-row-align-end", EXPECT, root_props, absolute_props, false, 130, 140, 50, 40);

    /* Reversed main axis + margins: marginBottom is the one that leads. */
    root_props = props_default();
    root_props.flex_direction = ER_FLEX_COL_REVERSE;
    absolute_props.margin_top = 7;
    absolute_props.margin_bottom = 3;
    static_pos_case("rev-col-margin", EXPECT, root_props, absolute_props, false, 20, 137, 50, 40);
    root_props.justify_content = ER_JUSTIFY_CENTER;
    static_pos_case("rev-col-margin-ctr", EXPECT, root_props, absolute_props, false, 20, 82, 50, 40);

    /* wrap-reverse mirrors the CROSS axis, against the full content extent. */
    root_props = props_default();
    root_props.flex_direction = ER_FLEX_COL;
    root_props.flex_wrap = ER_WRAP_WRAP_REVERSE;
    root_props.align_items = ER_ALIGN_FLEX_START;
    absolute_props = props_default();
    static_pos_case("wrap-rev-start", EXPECT, root_props, absolute_props, false, 130, 20, 50, 40);
    root_props.align_items = ER_ALIGN_FLEX_END;
    static_pos_case("wrap-rev-end", EXPECT, root_props, absolute_props, false, 20, 20, 50, 40);
}

/**
 * @brief Margins take part in the static position, on both axes and in every mode.
 *
 * Symmetric margins hide the rule (they cancel out when centring), so these are asymmetric: centring
 * splits what is left AFTER both margins, while flex-start/flex-end use only the margin on their own
 * edge and ignore the other.
 */
static void fixture_abs_static_margins(void)
{
    ERProps root_props = props_default();
    root_props.flex_direction = ER_FLEX_COL;
    root_props.justify_content = ER_JUSTIFY_CENTER;
    root_props.align_items = ER_ALIGN_CENTER;

    ERProps absolute_props = props_default();
    absolute_props.margin_left = 5;
    absolute_props.margin_right = 25;
    static_pos_case("margin-ctr-cross", EXPECT, root_props, absolute_props, false, 65, 80, 50, 40);

    absolute_props = props_default();
    absolute_props.margin_top = 5;
    absolute_props.margin_bottom = 25;
    static_pos_case("margin-ctr-main", EXPECT, root_props, absolute_props, false, 75, 70, 50, 40);

    root_props.justify_content = ER_JUSTIFY_FLEX_START;
    root_props.align_items = ER_ALIGN_FLEX_START;
    absolute_props = props_default();
    absolute_props.margin_left = 5;
    absolute_props.margin_top = 9;
    static_pos_case("margin-start", EXPECT, root_props, absolute_props, false, 25, 29, 50, 40);

    /* flex-start ignores the trailing margins entirely. */
    absolute_props = props_default();
    absolute_props.margin_right = 99;
    absolute_props.margin_bottom = 99;
    static_pos_case("margin-start-unused", EXPECT, root_props, absolute_props, false, 20, 20, 50, 40);

    root_props.justify_content = ER_JUSTIFY_FLEX_END;
    root_props.align_items = ER_ALIGN_FLEX_END;
    absolute_props = props_default();
    absolute_props.margin_right = 5;
    absolute_props.margin_bottom = 9;
    static_pos_case("margin-end", EXPECT, root_props, absolute_props, false, 125, 131, 50, 40);
}

/**
 * @brief A child too big for the content box overhangs both edges instead of being pinned at 0.
 *
 * Free space goes negative and stays negative — centring a 200x200 child in a 160x160 content box
 * puts it at -20 relative to that box, i.e. 0 on screen. Clamping the free space at 0 would stick it
 * at the padding corner instead. fixture_flow_justify_overflow() is the same rule for a flow child.
 */
static void fixture_abs_static_overflow(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.flex_direction = ER_FLEX_COL;
    root_props.justify_content = ER_JUSTIFY_CENTER;
    root_props.align_items = ER_ALIGN_CENTER;

    ERProps absolute_props = props_default();
    absolute_props.width = 200;
    absolute_props.height = 200;

    root_props.width = 200;
    root_props.height = 200;
    root_props.padding = 20;
    ERNode* root = mk(root_props, NULL);

    ERRect absolute_rect;
    absolute_props.position = ER_POS_ABSOLUTE;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-static", "overflow-ctr", EXPECT, absolute_rect, 0, 0, 200, 200);

    kill_child(root, absolute);
    er_node_destroy(root);
}

/**
 * @brief Insets and static positions resolve PER AXIS: `left` with no `top` pins x and aligns y.
 */
static void fixture_abs_static_per_axis(void)
{
    ERProps root_props = props_default();
    root_props.flex_direction = ER_FLEX_COL;
    root_props.justify_content = ER_JUSTIFY_CENTER;
    root_props.align_items = ER_ALIGN_CENTER;

    ERProps absolute_props = props_default();
    absolute_props.left = 7;
    static_pos_case("axis-left-only", EXPECT, root_props, absolute_props, false, 7, 80, 50, 40);

    absolute_props = props_default();
    absolute_props.top = 7;
    static_pos_case("axis-top-only", EXPECT, root_props, absolute_props, false, 75, 7, 50, 40);
}

/**
 * @brief Lays out two asymmetric-margin children on a reversed main axis and checks both rects.
 *
 * The container is the issue's shape: 200x200 with padding 20, so the content box is
 * (20, 20, 160, 160). Child A is 40 long with main-axis margins 7 (start) / 3 (end); child B is 30
 * long with 1 / 5. The margins are deliberately unequal — equal ones cancel out under the mirror and
 * hide the bug entirely. Both children are 50 on the cross axis so alignItems never enters into it.
 *
 * @param[in] name       Fixture name for the output.
 * @param[in] direction  ER_FLEX_COL_REVERSE or ER_FLEX_ROW_REVERSE.
 * @param[in] justify    justifyContent mode.
 * @param[in] gap        Main-axis gap, or 0 for none.
 * @param[in] first_x    Expected x of child A.
 * @param[in] first_y    Expected y of child A.
 * @param[in] second_x   Expected x of child B.
 * @param[in] second_y   Expected y of child B.
 */
static void rev_pair_case(const char* name,
                          uint8_t direction,
                          uint8_t justify,
                          int16_t gap,
                          int first_x,
                          int first_y,
                          int second_x,
                          int second_y)
{
    er_reset();

    const bool is_row = (direction == ER_FLEX_ROW_REVERSE);

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    root_props.padding = 20;
    root_props.flex_direction = direction;
    root_props.align_items = ER_ALIGN_FLEX_START;
    root_props.justify_content = justify;
    if (gap)
        root_props.gap = gap;
    ERNode* root = mk(root_props, NULL);

    ERRect first_rect, second_rect;
    ERProps first_props = props_default();
    first_props.width = is_row ? 40 : 50;
    first_props.height = is_row ? 50 : 40;
    first_props.margin_left = is_row ? 7 : ER_LAYOUT_AUTO;
    first_props.margin_right = is_row ? 3 : ER_LAYOUT_AUTO;
    first_props.margin_top = is_row ? ER_LAYOUT_AUTO : 7;
    first_props.margin_bottom = is_row ? ER_LAYOUT_AUTO : 3;
    ERNode* first = mk(first_props, &first_rect);

    ERProps second_props = props_default();
    second_props.width = is_row ? 30 : 50;
    second_props.height = is_row ? 50 : 30;
    second_props.margin_left = is_row ? 1 : ER_LAYOUT_AUTO;
    second_props.margin_right = is_row ? 5 : ER_LAYOUT_AUTO;
    second_props.margin_top = is_row ? ER_LAYOUT_AUTO : 1;
    second_props.margin_bottom = is_row ? ER_LAYOUT_AUTO : 5;
    ERNode* second = mk(second_props, &second_rect);

    er_tree_append_child(root, first);
    er_tree_append_child(root, second);
    er_tree_set_root(root);
    er_commit();

    pcheck(name, "a", EXPECT, first_rect, first_x, first_y, first_props.width, first_props.height);
    pcheck(name, "b", EXPECT, second_rect, second_x, second_y, second_props.width, second_props.height);

    kill_child(root, first);
    kill_child(root, second);
    er_node_destroy(root);
}

/**
 * @brief FLOW child, reversed main axis + asymmetric margins: the reversed axis picks the margin.
 *
 * A reversed axis leads with the margin on the edge it starts from. `column-reverse` starts at the
 * bottom, so a 40px child with marginBottom 3 sits 3px off the bottom of a (20, 20, 160, 160) content
 * box: y = 20 + 160 - 3 - 40 = 137. Pass 5 used to add the LEADING margin and then mirror the item
 * alone, landing at 133 — off by exactly marginTop - marginBottom, so symmetric margins hid it.
 *
 * Fixed in #193 by mirroring the item's outer (margin) box and re-seating the item inside it, which
 * is the rule the absolute path's static position already followed (abs-static rev-col-margin). An
 * uninset absolute child and an identical flow child now agree inside a reversed parent again.
 *
 * Multi-child cases matter because the mirror has to keep the packing order and the between-item
 * gaps right as well: adjacent children are separated by the sum of their facing margins, and the
 * run as a whole still honours justifyContent. All values from the real `yoga-layout` package.
 */
static void fixture_flow_reverse_margin(void)
{
    /* The issue's exact repro: one child, column-reverse, flex-start. */
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    root_props.padding = 20;
    root_props.flex_direction = ER_FLEX_COL_REVERSE;
    root_props.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(root_props, NULL);

    ERRect third_rect;
    ERProps kid_props = props_default();
    kid_props.width = 50;
    kid_props.height = 40;
    kid_props.margin_top = 7;
    kid_props.margin_bottom = 3;
    ERNode* third = mk(kid_props, &third_rect);

    er_tree_append_child(root, third);
    er_tree_set_root(root);
    er_commit();
    pcheck("flow-reverse-margin", "c", EXPECT, third_rect, 20, 137, 50, 40);

    kill_child(root, third);
    er_node_destroy(root);

    /* Overflow: a child taller than the content box hangs off the reversed start edge, keeping its
     * 3px marginBottom against the bottom. 20 + 160 - 3 - 200 = -23. */
    root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    root_props.padding = 20;
    root_props.flex_direction = ER_FLEX_COL_REVERSE;
    root_props.align_items = ER_ALIGN_FLEX_START;
    root = mk(root_props, NULL);

    kid_props.height = 200;
    third = mk(kid_props, &third_rect);
    er_tree_append_child(root, third);
    er_tree_set_root(root);
    er_commit();
    pcheck("flow-reverse-overflow", "c", EXPECT, third_rect, 20, -23, 50, 200);

    kill_child(root, third);
    er_node_destroy(root);
}

/**
 * @brief Reversed main axis + asymmetric margins across every justifyContent mode and both axes.
 *
 * space-evenly and centre divide the 74px of free space by 3 and by 2, so both land on a fraction
 * that the reversed axis then has to mirror.
 */
static void fixture_flow_reverse_margin_justify(void)
{
    rev_pair_case("rev-pair-start", ER_FLEX_COL_REVERSE, ER_JUSTIFY_FLEX_START, 0, 20, 137, 20, 95);
    rev_pair_case("rev-pair-end", ER_FLEX_COL_REVERSE, ER_JUSTIFY_FLEX_END, 0, 20, 63, 20, 21);
    rev_pair_case("rev-pair-between", ER_FLEX_COL_REVERSE, ER_JUSTIFY_SPACE_BETWEEN, 0, 20, 137, 20, 21);
    rev_pair_case("rev-pair-around", ER_FLEX_COL_REVERSE, ER_JUSTIFY_SPACE_AROUND, 0, 20, 119, 20, 40);
    rev_pair_case("rev-pair-evenly", ER_FLEX_COL_REVERSE, ER_JUSTIFY_SPACE_EVENLY, 0, 20, 112, 20, 46);
    rev_pair_case("rev-pair-center", ER_FLEX_COL_REVERSE, ER_JUSTIFY_CENTER, 0, 20, 100, 20, 58);
    rev_pair_case("rev-pair-gap", ER_FLEX_COL_REVERSE, ER_JUSTIFY_FLEX_START, 10, 20, 137, 20, 85);

    rev_pair_case("rev-row-pair-start", ER_FLEX_ROW_REVERSE, ER_JUSTIFY_FLEX_START, 0, 137, 20, 95, 20);
    rev_pair_case("rev-row-pair-between", ER_FLEX_ROW_REVERSE, ER_JUSTIFY_SPACE_BETWEEN, 0, 137, 20, 21, 20);
}

/**
 * @brief Builds the issue shape: a 200x200 column with padding 20, wrapping in reverse.
 *
 * Content box is (20, 20, 160, 160), so 40px-tall items give four per line and every extra item
 * starts a second line. `n` items, an alignContent mode and an optional cross-axis (column) gap are
 * all the fixtures below vary. Captures each item's rect for the caller to assert.
 */
static void build_wrap_rev(int count,
                           uint8_t align_content,
                           int16_t column_gap,
                           int16_t item_height,
                           ERNode** root_out,
                           ERNode** kids_out,
                           ERRect* rects)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    root_props.padding = 20;
    root_props.flex_direction = ER_FLEX_COL;
    root_props.flex_wrap = ER_WRAP_WRAP_REVERSE;
    root_props.align_items = ER_ALIGN_FLEX_START;
    root_props.align_content = align_content;
    root_props.column_gap = column_gap;
    ERNode* root = mk(root_props, NULL);

    for (int kid_index = 0; kid_index < count; kid_index++)
    {
        ERProps kid_props = props_default();
        kid_props.width = 50;
        kid_props.height = item_height;
        kids_out[kid_index] = mk(kid_props, &rects[kid_index]);
        er_tree_append_child(root, kids_out[kid_index]);
    }
    er_tree_set_root(root);
    *root_out = root;
}

/** @brief Tears down a wrap_rev fixture. */
static void teardown_wrap_rev(int count, ERNode* root, ERNode** kids)
{
    for (int kid_index = 0; kid_index < count; kid_index++)
    {
        kill_child(root, kids[kid_index]);
    }
    er_node_destroy(root);
}

/**
 * @brief FLOW children, wrap-reverse: the lines reverse AND travel to the far cross edge.
 *
 * The mirror is taken about the content box, not about the lines' own extent -- the latter reverses
 * their order but leaves the block parked at the cross-start, which is what issue #192 reported. A
 * single 50px line in a 160px content box therefore sits at x=130, not x=20, and the second line of
 * a two-line layout lands at x=80. The two only agree when the lines fill the cross axis exactly.
 */
static void fixture_flow_wrap_reverse_anchor(void)
{
    ERNode* root;
    ERNode* kids[5];
    ERRect rects[5];

    /* One line: 160 content - 50 line = 110 of leftover cross space to travel. */
    build_wrap_rev(1, ER_ALIGN_CONTENT_FLEX_START, 0, 40, &root, kids, rects);
    er_commit();
    pcheck("wrap-rev-1line", "c", EXPECT, rects[0], 130, 20, 50, 40);
    teardown_wrap_rev(1, root, kids);

    /* Two lines: first line at the far edge, the second one line-width in from it. */
    build_wrap_rev(5, ER_ALIGN_CONTENT_FLEX_START, 0, 40, &root, kids, rects);
    er_commit();
    pcheck("wrap-rev-2line", "l0i0", EXPECT, rects[0], 130, 20, 50, 40);
    pcheck("wrap-rev-2line", "l0i3", EXPECT, rects[3], 130, 140, 50, 40);
    pcheck("wrap-rev-2line", "l1i0", EXPECT, rects[4], 80, 20, 50, 40);
    teardown_wrap_rev(5, root, kids);

    /* A cross-axis gap widens the step between the mirrored lines, but not the anchor. */
    build_wrap_rev(5, ER_ALIGN_CONTENT_FLEX_START, 10, 40, &root, kids, rects);
    er_commit();
    pcheck("wrap-rev-gap", "l0i0", EXPECT, rects[0], 130, 20, 50, 40);
    pcheck("wrap-rev-gap", "l1i0", EXPECT, rects[4], 70, 20, 50, 40);
    teardown_wrap_rev(5, root, kids);
}

/**
 * @brief wrap-reverse + alignContent: the mirror carries whatever alignContent already placed.
 *
 * alignContent distributes the leftover cross space first, then the whole block is mirrored, so the
 * two must not double-count: flex-end lands the block against the NEAR edge (x=70/20) precisely
 * because flex-start already put it against the far one. Expected rects are from `yoga-layout`.
 */
static void fixture_flow_wrap_reverse_align_content(void)
{
    ERNode* root;
    ERNode* kids[5];
    ERRect rects[5];
    /* Two lines of 50 in a 160 content box -> 60px of free cross space to distribute. */
    static const struct
    {
        const char* name;
        uint8_t mode;
        int16_t first_line_x;  /**< First line (4 items). */
        int16_t second_line_x; /**< Second line (1 item). */
    } k_cases[] = {
        {"ac-flex-end", ER_ALIGN_CONTENT_FLEX_END, 70, 20},
        {"ac-center", ER_ALIGN_CONTENT_CENTER, 100, 50},
        {"ac-between", ER_ALIGN_CONTENT_SPACE_BETWEEN, 130, 20},
        {"ac-around", ER_ALIGN_CONTENT_SPACE_AROUND, 115, 35},
        {"ac-stretch", ER_ALIGN_CONTENT_STRETCH, 130, 50},
    };

    for (size_t case_index = 0; case_index < sizeof(k_cases) / sizeof(k_cases[0]); case_index++)
    {
        build_wrap_rev(5, k_cases[case_index].mode, 0, 40, &root, kids, rects);
        er_commit();
        pcheck(k_cases[case_index].name, "l0", EXPECT, rects[0], k_cases[case_index].first_line_x, 20, 50, 40);
        pcheck(k_cases[case_index].name, "l1", EXPECT, rects[4], k_cases[case_index].second_line_x, 20, 50, 40);
        teardown_wrap_rev(5, root, kids);
    }
}

/**
 * @brief wrap-reverse whose lines overflow the cross axis mirrors into negative positions.
 *
 * Eight 80px-tall items give two per line and four lines of 50 = 200 in a 160 content box. Yoga does
 * not clamp: the mirror keeps the FIRST line at the far edge and lets the last one hang off the
 * cross-start at x=-20. Clamping the overflow here would silently drop a line off the other side.
 */
static void fixture_flow_wrap_reverse_overflow(void)
{
    ERNode* root;
    ERNode* kids[8];
    ERRect rects[8];

    build_wrap_rev(8, ER_ALIGN_CONTENT_FLEX_START, 0, 80, &root, kids, rects);
    er_commit();
    pcheck("wrap-rev-overflow", "l0", EXPECT, rects[0], 130, 20, 50, 80);
    pcheck("wrap-rev-overflow", "l1", EXPECT, rects[2], 80, 20, 50, 80);
    pcheck("wrap-rev-overflow", "l2", EXPECT, rects[4], 30, 20, 50, 80);
    pcheck("wrap-rev-overflow", "l3", EXPECT, rects[6], -20, 20, 50, 80);
    teardown_wrap_rev(8, root, kids);
}

/** @brief In-flow siblings never move the static position — it is the SOLE-item spot, not a slot. */
static void fixture_abs_static_ignores_siblings(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    root_props.padding = 20;
    root_props.flex_direction = ER_FLEX_COL;
    root_props.justify_content = ER_JUSTIFY_CENTER;
    root_props.align_items = ER_ALIGN_CENTER;
    ERNode* root = mk(root_props, NULL);

    ERProps spacer_props = props_default();
    spacer_props.width = 30;
    spacer_props.height = 30;
    ERNode* first_spacer = mk(spacer_props, NULL);
    ERNode* second_spacer = mk(spacer_props, NULL);
    ERNode* third_spacer = mk(spacer_props, NULL);

    ERRect absolute_rect;
    ERProps absolute_props = props_default();
    absolute_props.position = ER_POS_ABSOLUTE;
    absolute_props.width = 50;
    absolute_props.height = 40;
    ERNode* absolute = mk(absolute_props, &absolute_rect);

    er_tree_append_child(root, first_spacer);
    er_tree_append_child(root, second_spacer);
    er_tree_append_child(root, third_spacer);
    er_tree_append_child(root, absolute);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-static", "siblings", EXPECT, absolute_rect, 75, 80, 50, 40);

    kill_child(root, first_spacer);
    kill_child(root, second_spacer);
    kill_child(root, third_spacer);
    kill_child(root, absolute);
    er_node_destroy(root);
}

/**
 * @brief Centring an ODD amount of free space: the half pixel rounds up, it does not truncate.
 *
 * Free space of 109 halves to 54.5, which Yoga rounds to 55. Every placement route has to agree —
 * flow, absolute static position, and a reversed axis. The overhang rows pin the negative side.
 */
static void fixture_half_pixel_center(void)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    root_props.padding = 20;
    root_props.justify_content = ER_JUSTIFY_CENTER;
    root_props.align_items = ER_ALIGN_CENTER;

    ERProps child_props = props_default();
    child_props.width = 51;
    child_props.height = 41;

    static const struct
    {
        const char* name;
        uint8_t direction;
        int16_t width, height; /**< Child size. */
        int16_t x, y;          /**< Expected origin. */
    } k_cases[] = {
        /* The issue's exact repro, then the same thing with the axes swapped. */
        {"half-center-col", ER_FLEX_COL, 51, 41, 75, 80},
        {"half-center-row", ER_FLEX_ROW, 51, 41, 75, 80},
        /* A reversed axis measures from the far edge but must still round to the same pixel. */
        {"half-center-colrev", ER_FLEX_COL_REVERSE, 51, 41, 75, 80},
        {"half-center-rowrev", ER_FLEX_ROW_REVERSE, 51, 41, 75, 80},
    };

    for (size_t case_index = 0; case_index < sizeof(k_cases) / sizeof(k_cases[0]); case_index++)
    {
        root_props.flex_direction = k_cases[case_index].direction;
        child_props.width = k_cases[case_index].width;
        child_props.height = k_cases[case_index].height;

        ERNode* root = mk(root_props, NULL);
        ERRect child_rect;
        ERNode* first_child = mk(child_props, &child_rect);
        er_tree_append_child(root, first_child);
        er_tree_set_root(root);
        er_commit();
        pcheck(k_cases[case_index].name,
               "c",
               EXPECT,
               child_rect,
               k_cases[case_index].x,
               k_cases[case_index].y,
               k_cases[case_index].width,
               k_cases[case_index].height);
        kill_child(root, first_child);
        er_node_destroy(root);

        /* An uninset ABSOLUTE child takes the same static position, so it must round the same way. */
        root_props.flex_direction = k_cases[case_index].direction;
        ERProps absolute_child_props = child_props;
        absolute_child_props.position = ER_POS_ABSOLUTE;
        root = mk(root_props, NULL);
        ERNode* absolute_child = mk(absolute_child_props, &child_rect);
        er_tree_append_child(root, absolute_child);
        er_tree_set_root(root);
        er_commit();
        pcheck(k_cases[case_index].name,
               "abs",
               EXPECT,
               child_rect,
               k_cases[case_index].x,
               k_cases[case_index].y,
               k_cases[case_index].width,
               k_cases[case_index].height);
        kill_child(root, absolute_child);
        er_node_destroy(root);
    }

    /* A 201x171 child centred in a 160x160 content box overhangs both edges evenly: -20.5 rounds up
     * to -20, same rule on both axes. Flow and absolute have to agree — the same child placed either
     * way lands in the same place. */
    root_props.flex_direction = ER_FLEX_COL;
    child_props.width = 201;
    child_props.height = 171;

    ERProps overflow_props = child_props;
    overflow_props.position = ER_POS_ABSOLUTE;
    ERNode* root = mk(root_props, NULL);
    ERRect overflow_rect;
    ERNode* overflow = mk(overflow_props, &overflow_rect);
    er_tree_append_child(root, overflow);
    er_tree_set_root(root);
    er_commit();
    pcheck("half-overhang", "abs", EXPECT, overflow_rect, 0, 15, 201, 171);
    kill_child(root, overflow);
    er_node_destroy(root);

    root = mk(root_props, NULL);
    ERNode* flow = mk(child_props, &overflow_rect);
    er_tree_append_child(root, flow);
    er_tree_set_root(root);
    er_commit();
    pcheck("half-overhang", "flow", EXPECT, overflow_rect, 0, 15, 201, 171);
    kill_child(root, flow);
    er_node_destroy(root);
}

/**
 * @brief Builds `n` children of the given main-axis sizes in a padded 200x200 box, checking each one.
 *
 * The container is the usual (20, 20, 160, 160) content box, and every child is 50 on the cross axis
 * under alignItems: flex-start, so only the main axis is under test. Sizes are per child, in tree
 * order: children that differ are what make an odd amount of free space, and what tell a mode that
 * distributes between items apart from one that only offsets the block.
 *
 * @param[in] name       Assertion label.
 * @param[in] direction  Any ERFlexDirection.
 * @param[in] justify    justifyContent under test.
 * @param[in] gap        Main-axis gap, or 0 for none.
 * @param[in] count      Child count (<= 8).
 * @param[in] item       Child main-axis size, per child.
 * @param[in] want       Expected main-axis position per child, in tree order.
 */
static void stack_sizes_case(const char* name,
                             uint8_t direction,
                             uint8_t justify,
                             int16_t gap,
                             int count,
                             const int16_t* item,
                             const int16_t* want)
{
    er_reset();

    const bool is_row = (direction == ER_FLEX_ROW || direction == ER_FLEX_ROW_REVERSE);

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    root_props.padding = 20;
    root_props.flex_direction = direction;
    root_props.justify_content = justify;
    root_props.align_items = ER_ALIGN_FLEX_START;
    if (gap)
        root_props.gap = gap;
    ERNode* root = mk(root_props, NULL);

    ERNode* kids[8];
    ERRect rects[8];
    for (int kid_index = 0; kid_index < count; kid_index++)
    {
        ERProps kid_props = props_default();
        kid_props.width = is_row ? item[kid_index] : 50;
        kid_props.height = is_row ? 50 : item[kid_index];
        kids[kid_index] = mk(kid_props, &rects[kid_index]);
        er_tree_append_child(root, kids[kid_index]);
    }
    er_tree_set_root(root);
    er_commit();

    for (int kid_index = 0; kid_index < count; kid_index++)
    {
        char label[8];
        snprintf(label, sizeof label, "i%d", kid_index);
        pcheck(name,
               label,
               EXPECT,
               rects[kid_index],
               is_row ? want[kid_index] : 20,
               is_row ? 20 : want[kid_index],
               is_row ? item[kid_index] : 50,
               is_row ? 50 : item[kid_index]);
    }
    for (int kid_index = 0; kid_index < count; kid_index++)
        kill_child(root, kids[kid_index]);
    er_node_destroy(root);
}

/**
 * @brief stack_sizes_case() with `n` identically sized children and no gap.
 *
 * @param[in] name       Assertion label.
 * @param[in] direction  ER_FLEX_COL or ER_FLEX_COL_REVERSE.
 * @param[in] justify    justifyContent under test.
 * @param[in] count      Child count (<= 8).
 * @param[in] item       Child main-axis size.
 * @param[in] want_y     Expected y per child, in tree order.
 */
static void
stack_case(const char* name, uint8_t direction, uint8_t justify, int count, int16_t item, const int16_t* want_y)
{
    int16_t sizes[8];
    for (int kid_index = 0; kid_index < count; kid_index++)
        sizes[kid_index] = item;
    stack_sizes_case(name, direction, justify, 0, count, sizes, want_y);
}

/**
 * @brief Three wrapping children, one line of which overflows on its own, checking each y.
 *
 * 100, 90 and 171 tall in a 160px content box: each starts a new line, and only the last line is
 * over. justifyContent runs per line, so the two that fit must keep placing normally while the third
 * hangs off the end.
 *
 * @param[in] name     Assertion label.
 * @param[in] justify  justifyContent under test.
 * @param[in] want_y   Expected y for children 0, 1 and 2.
 */
static void wrap_overflow_case(const char* name, uint8_t justify, const int16_t* want_y)
{
    er_reset();

    static const int16_t k_heights[3] = {100, 90, 171};

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    root_props.padding = 20;
    root_props.flex_direction = ER_FLEX_COL;
    root_props.flex_wrap = ER_WRAP_WRAP;
    root_props.align_content = ER_ALIGN_CONTENT_FLEX_START;
    root_props.align_items = ER_ALIGN_FLEX_START;
    root_props.justify_content = justify;
    ERNode* root = mk(root_props, NULL);

    ERNode* kids[3];
    ERRect rects[3];
    for (int kid_index = 0; kid_index < 3; kid_index++)
    {
        ERProps kid_props = props_default();
        kid_props.width = 40;
        kid_props.height = k_heights[kid_index];
        kids[kid_index] = mk(kid_props, &rects[kid_index]);
        er_tree_append_child(root, kids[kid_index]);
    }
    er_tree_set_root(root);
    er_commit();

    for (int kid_index = 0; kid_index < 3; kid_index++)
    {
        char label[8];
        snprintf(label, sizeof label, "l%d", kid_index);
        pcheck(name,
               label,
               EXPECT,
               rects[kid_index],
               (int16_t)(20 + 40 * kid_index),
               want_y[kid_index],
               40,
               k_heights[kid_index]);
    }
    for (int kid_index = 0; kid_index < 3; kid_index++)
        kill_child(root, kids[kid_index]);
    er_node_destroy(root);
}

/**
 * @brief An overflowing line: centre and flex-end keep the negative free space, the rest do not.
 *
 * Yoga only offsets the block by free space it can still spread. `center` and `flex-end` move by
 * whatever is left even when that is negative, so a child bigger than the box hangs off the far edge
 * (and, centred, off both). `space-between` / `space-around` / `space-evenly` have nothing to put
 * between items once the line is full and collapse to flex-start.
 *
 * A reversed axis measures the same line from the far edge, so it is flex-start that overhangs there
 * while centre lands in the same place either way. The gap row is there because the gap is part of
 * what fills the line: three 50px children plus two 8px gaps overflow a box the children alone fit.
 */
static void fixture_flow_justify_overflow(void)
{
    /* One 171px child in a 160px content box: 11 over, so centring lands on a half pixel. */
    static const int16_t k_solo[] = {171};
    static const int16_t k_solo_start[] = {20};
    static const int16_t k_solo_end[] = {9};
    static const int16_t k_solo_center[] = {15};
    static const int16_t k_rsolo_start[] = {9};
    static const int16_t k_rsolo_center[] = {15};
    static const int16_t k_rsolo_end[] = {20};

    stack_sizes_case("ovf-start", ER_FLEX_COL, ER_JUSTIFY_FLEX_START, 0, 1, k_solo, k_solo_start);
    stack_sizes_case("ovf-center", ER_FLEX_COL, ER_JUSTIFY_CENTER, 0, 1, k_solo, k_solo_center);
    stack_sizes_case("ovf-end", ER_FLEX_COL, ER_JUSTIFY_FLEX_END, 0, 1, k_solo, k_solo_end);
    stack_sizes_case("ovf-between", ER_FLEX_COL, ER_JUSTIFY_SPACE_BETWEEN, 0, 1, k_solo, k_solo_start);
    stack_sizes_case("ovf-around", ER_FLEX_COL, ER_JUSTIFY_SPACE_AROUND, 0, 1, k_solo, k_solo_start);
    stack_sizes_case("ovf-evenly", ER_FLEX_COL, ER_JUSTIFY_SPACE_EVENLY, 0, 1, k_solo, k_solo_start);

    stack_sizes_case("ovf-rstart", ER_FLEX_COL_REVERSE, ER_JUSTIFY_FLEX_START, 0, 1, k_solo, k_rsolo_start);
    stack_sizes_case("ovf-rcenter", ER_FLEX_COL_REVERSE, ER_JUSTIFY_CENTER, 0, 1, k_solo, k_rsolo_center);
    stack_sizes_case("ovf-rend", ER_FLEX_COL_REVERSE, ER_JUSTIFY_FLEX_END, 0, 1, k_solo, k_rsolo_end);
    stack_sizes_case("ovf-rbetween", ER_FLEX_COL_REVERSE, ER_JUSTIFY_SPACE_BETWEEN, 0, 1, k_solo, k_rsolo_start);
    stack_sizes_case("ovf-raround", ER_FLEX_COL_REVERSE, ER_JUSTIFY_SPACE_AROUND, 0, 1, k_solo, k_rsolo_start);
    stack_sizes_case("ovf-revenly", ER_FLEX_COL_REVERSE, ER_JUSTIFY_SPACE_EVENLY, 0, 1, k_solo, k_rsolo_start);

    /* The same overflow on a row, to pin that this is the main axis and not the y one. */
    stack_sizes_case("ovf-row-center", ER_FLEX_ROW, ER_JUSTIFY_CENTER, 0, 1, k_solo, k_solo_center);
    stack_sizes_case("ovf-row-end", ER_FLEX_ROW, ER_JUSTIFY_FLEX_END, 0, 1, k_solo, k_solo_end);
    stack_sizes_case("ovf-rowrev-center", ER_FLEX_ROW_REVERSE, ER_JUSTIFY_CENTER, 0, 1, k_solo, k_rsolo_center);
    stack_sizes_case("ovf-rowrev-end", ER_FLEX_ROW_REVERSE, ER_JUSTIFY_FLEX_END, 0, 1, k_solo, k_rsolo_end);

    /* Two unequal children, 90 + 91 in 160: 21 over. Both shift by the same amount and stay touching,
     * so the block is offset as a whole rather than the items being re-spaced. */
    static const int16_t k_pair[] = {90, 91};
    static const int16_t k_pair_start[] = {20, 110};
    static const int16_t k_pair_center[] = {10, 100};
    static const int16_t k_pair_end[] = {-1, 89};
    static const int16_t k_rpair_start[] = {90, -1};
    static const int16_t k_rpair_center[] = {101, 10};
    static const int16_t k_rpair_end[] = {111, 20};

    stack_sizes_case("ovf2-start", ER_FLEX_COL, ER_JUSTIFY_FLEX_START, 0, 2, k_pair, k_pair_start);
    stack_sizes_case("ovf2-center", ER_FLEX_COL, ER_JUSTIFY_CENTER, 0, 2, k_pair, k_pair_center);
    stack_sizes_case("ovf2-end", ER_FLEX_COL, ER_JUSTIFY_FLEX_END, 0, 2, k_pair, k_pair_end);
    stack_sizes_case("ovf2-between", ER_FLEX_COL, ER_JUSTIFY_SPACE_BETWEEN, 0, 2, k_pair, k_pair_start);
    stack_sizes_case("ovf2-around", ER_FLEX_COL, ER_JUSTIFY_SPACE_AROUND, 0, 2, k_pair, k_pair_start);
    stack_sizes_case("ovf2-evenly", ER_FLEX_COL, ER_JUSTIFY_SPACE_EVENLY, 0, 2, k_pair, k_pair_start);

    stack_sizes_case("ovf2-rstart", ER_FLEX_COL_REVERSE, ER_JUSTIFY_FLEX_START, 0, 2, k_pair, k_rpair_start);
    stack_sizes_case("ovf2-rcenter", ER_FLEX_COL_REVERSE, ER_JUSTIFY_CENTER, 0, 2, k_pair, k_rpair_center);
    stack_sizes_case("ovf2-rend", ER_FLEX_COL_REVERSE, ER_JUSTIFY_FLEX_END, 0, 2, k_pair, k_rpair_end);
    stack_sizes_case("ovf2-rbetween", ER_FLEX_COL_REVERSE, ER_JUSTIFY_SPACE_BETWEEN, 0, 2, k_pair, k_rpair_start);
    stack_sizes_case("ovf2-raround", ER_FLEX_COL_REVERSE, ER_JUSTIFY_SPACE_AROUND, 0, 2, k_pair, k_rpair_start);
    stack_sizes_case("ovf2-revenly", ER_FLEX_COL_REVERSE, ER_JUSTIFY_SPACE_EVENLY, 0, 2, k_pair, k_rpair_start);

    /* 3 x 50 plus two 8px gaps = 166 in 160: the gaps alone put the line 6 over. */
    static const int16_t k_gap3[] = {50, 50, 50};
    static const int16_t k_gap3_start[] = {20, 78, 136};
    static const int16_t k_gap3_center[] = {17, 75, 133};
    static const int16_t k_gap3_end[] = {14, 72, 130};

    stack_sizes_case("ovf-gap-start", ER_FLEX_COL, ER_JUSTIFY_FLEX_START, 8, 3, k_gap3, k_gap3_start);
    stack_sizes_case("ovf-gap-center", ER_FLEX_COL, ER_JUSTIFY_CENTER, 8, 3, k_gap3, k_gap3_center);
    stack_sizes_case("ovf-gap-end", ER_FLEX_COL, ER_JUSTIFY_FLEX_END, 8, 3, k_gap3, k_gap3_end);
    stack_sizes_case("ovf-gap-between", ER_FLEX_COL, ER_JUSTIFY_SPACE_BETWEEN, 8, 3, k_gap3, k_gap3_start);
    stack_sizes_case("ovf-gap-around", ER_FLEX_COL, ER_JUSTIFY_SPACE_AROUND, 8, 3, k_gap3, k_gap3_start);
    stack_sizes_case("ovf-gap-evenly", ER_FLEX_COL, ER_JUSTIFY_SPACE_EVENLY, 8, 3, k_gap3, k_gap3_start);

    /* Wrapped: the sign is per line, so lines that fit are unaffected by the one that does not. */
    static const int16_t k_wrap_start[] = {20, 20, 20};
    static const int16_t k_wrap_center[] = {50, 55, 15};
    static const int16_t k_wrap_end[] = {80, 90, 9};

    wrap_overflow_case("ovf-wrap-center", ER_JUSTIFY_CENTER, k_wrap_center);
    wrap_overflow_case("ovf-wrap-end", ER_JUSTIFY_FLEX_END, k_wrap_end);
    wrap_overflow_case("ovf-wrap-between", ER_JUSTIFY_SPACE_BETWEEN, k_wrap_start);
}

/**
 * @brief Distributing free space that does not divide evenly: no truncation, and no drift.
 *
 * Three 17px children in a 160px content box leave 109 to share — odd, and awkward to divide by 2, 3,
 * 4 and 6 — so every mode lands on a fraction. Each item is placed from one exact fraction of the
 * free space rather than from a rounded step added up along the line, which would drift.
 *
 * The reversed rows are the same layout measured from the far edge. Values from `yoga-layout`.
 */
static void fixture_half_pixel_justify(void)
{
    /* 3 x 17 = 51 used, 109 free. */
    static const int16_t k_between3[] = {20, 92, 163};
    static const int16_t k_around3[] = {38, 92, 145};
    static const int16_t k_evenly3[] = {47, 92, 136};
    static const int16_t k_center3[] = {75, 92, 109};
    /* Reversed: the same three positions, read from the other end. */
    static const int16_t k_rbetween3[] = {163, 92, 20};
    static const int16_t k_raround3[] = {145, 92, 38};
    static const int16_t k_revenly3[] = {136, 92, 47};
    static const int16_t k_rcenter3[] = {109, 92, 75};
    /* 4 x 17 = 68 used, 92 free -- 92/3 and 92/8 both land off the grid. */
    static const int16_t k_between4[] = {20, 68, 115, 163};
    static const int16_t k_around4[] = {32, 72, 112, 152};
    static const int16_t k_evenly4[] = {38, 74, 109, 145};
    static const int16_t k_center4[] = {66, 83, 100, 117};

    stack_case("half-between3", ER_FLEX_COL, ER_JUSTIFY_SPACE_BETWEEN, 3, 17, k_between3);
    stack_case("half-around3", ER_FLEX_COL, ER_JUSTIFY_SPACE_AROUND, 3, 17, k_around3);
    stack_case("half-evenly3", ER_FLEX_COL, ER_JUSTIFY_SPACE_EVENLY, 3, 17, k_evenly3);
    stack_case("half-center3", ER_FLEX_COL, ER_JUSTIFY_CENTER, 3, 17, k_center3);

    stack_case("half-rbetween3", ER_FLEX_COL_REVERSE, ER_JUSTIFY_SPACE_BETWEEN, 3, 17, k_rbetween3);
    stack_case("half-raround3", ER_FLEX_COL_REVERSE, ER_JUSTIFY_SPACE_AROUND, 3, 17, k_raround3);
    stack_case("half-revenly3", ER_FLEX_COL_REVERSE, ER_JUSTIFY_SPACE_EVENLY, 3, 17, k_revenly3);
    stack_case("half-rcenter3", ER_FLEX_COL_REVERSE, ER_JUSTIFY_CENTER, 3, 17, k_rcenter3);

    stack_case("half-between4", ER_FLEX_COL, ER_JUSTIFY_SPACE_BETWEEN, 4, 17, k_between4);
    stack_case("half-around4", ER_FLEX_COL, ER_JUSTIFY_SPACE_AROUND, 4, 17, k_around4);
    stack_case("half-evenly4", ER_FLEX_COL, ER_JUSTIFY_SPACE_EVENLY, 4, 17, k_evenly4);
    stack_case("half-center4", ER_FLEX_COL, ER_JUSTIFY_CENTER, 4, 17, k_center4);
}

/**
 * @brief Nine 40px children in a padded 200x200 column, wrapped, checking one child per line.
 *
 * Four fit per line on the main axis (4 x 40 = 160), giving lines of 4 / 4 / 1, so children 0, 4 and
 * 8 report where their line landed on the cross axis.
 *
 * @param[in] name           Assertion label.
 * @param[in] wrap           ER_WRAP_WRAP or ER_WRAP_WRAP_REVERSE.
 * @param[in] align_content  alignContent under test.
 * @param[in] align_items    alignItems under test.
 * @param[in] item_width     Child cross-axis size.
 * @param[in] want_x         Expected x for lines 0, 1, 2.
 */
static void wrap_line_case(const char* name,
                           uint8_t wrap,
                           uint8_t align_content,
                           uint8_t align_items,
                           int16_t item_width,
                           const int16_t* want_x)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    root_props.padding = 20;
    root_props.flex_direction = ER_FLEX_COL;
    root_props.flex_wrap = wrap;
    root_props.align_content = align_content;
    root_props.align_items = align_items;
    ERNode* root = mk(root_props, NULL);

    ERNode* kids[9];
    ERRect rects[9];
    for (int kid_index = 0; kid_index < 9; kid_index++)
    {
        ERProps kid_props = props_default();
        kid_props.width = item_width;
        kid_props.height = 40;
        kids[kid_index] = mk(kid_props, &rects[kid_index]);
        er_tree_append_child(root, kids[kid_index]);
    }
    er_tree_set_root(root);
    er_commit();

    pcheck(name, "l0", EXPECT, rects[0], want_x[0], 20, item_width, 40);
    pcheck(name, "l1", EXPECT, rects[4], want_x[1], 20, item_width, 40);
    pcheck(name, "l2", EXPECT, rects[8], want_x[2], 20, item_width, 40);

    for (int kid_index = 0; kid_index < 9; kid_index++)
        kill_child(root, kids[kid_index]);
    er_node_destroy(root);
}

/**
 * @brief alignContent over three lines whose leftover cross space does not divide by three.
 *
 * Three lines of 50 in a 160 content box leave 10 to share, so space-around and stretch both work in
 * thirds. The stretch rows matter most, since stretch is the default alignContent.
 *
 * The `ai-*` rows compound two fractions: a 21px child centred in a 53.33-wide line lands on exactly
 * 89.5, which only rounds right if the line offset and the centring half are summed first. The
 * wrap-reverse rows check the same numbers through the mirror. Values from `yoga-layout`.
 */
static void fixture_half_pixel_align_content(void)
{
    er_reset();

    static const struct
    {
        const char* name;
        uint8_t wrap;
        uint8_t align_content;
        uint8_t align_items;
        int16_t item_width;
        int16_t want_x[3];
    } k_cases[] = {
        /* 3 lines of 50 in 160 -> 10 free. */
        {"half-ac-center", ER_WRAP_WRAP, ER_ALIGN_CONTENT_CENTER, ER_ALIGN_FLEX_START, 50, {25, 75, 125}},
        {"half-ac-between", ER_WRAP_WRAP, ER_ALIGN_CONTENT_SPACE_BETWEEN, ER_ALIGN_FLEX_START, 50, {20, 75, 130}},
        {"half-ac-around", ER_WRAP_WRAP, ER_ALIGN_CONTENT_SPACE_AROUND, ER_ALIGN_FLEX_START, 50, {22, 75, 128}},
        {"half-ac-stretch", ER_WRAP_WRAP, ER_ALIGN_CONTENT_STRETCH, ER_ALIGN_FLEX_START, 50, {20, 73, 127}},
        {"half-ac-end", ER_WRAP_WRAP, ER_ALIGN_CONTENT_FLEX_END, ER_ALIGN_FLEX_START, 50, {30, 80, 130}},

        /* Same through the wrap-reverse mirror, which also has to run before the rounding. */
        {"half-acrev-center", ER_WRAP_WRAP_REVERSE, ER_ALIGN_CONTENT_CENTER, ER_ALIGN_FLEX_START, 50, {125, 75, 25}},
        {"half-acrev-around",
         ER_WRAP_WRAP_REVERSE,
         ER_ALIGN_CONTENT_SPACE_AROUND,
         ER_ALIGN_FLEX_START,
         50,
         {128, 75, 22}},
        {"half-acrev-stretch", ER_WRAP_WRAP_REVERSE, ER_ALIGN_CONTENT_STRETCH, ER_ALIGN_FLEX_START, 50, {130, 77, 23}},
        {"half-acrev-end", ER_WRAP_WRAP_REVERSE, ER_ALIGN_CONTENT_FLEX_END, ER_ALIGN_FLEX_START, 50, {120, 70, 20}},

        /* 3 lines of 21 in 160 -> 97 free, so a stretched line is 53.33 and a centred 21px child
         * inside line 1 sits on exactly 89.5. */
        {"half-ai-stretch", ER_WRAP_WRAP, ER_ALIGN_CONTENT_STRETCH, ER_ALIGN_CENTER, 21, {36, 90, 143}},
        {"half-ai-around", ER_WRAP_WRAP, ER_ALIGN_CONTENT_SPACE_AROUND, ER_ALIGN_CENTER, 21, {36, 90, 143}},
        {"half-ai-end", ER_WRAP_WRAP, ER_ALIGN_CONTENT_STRETCH, ER_ALIGN_FLEX_END, 21, {52, 106, 159}},
    };

    for (size_t case_index = 0; case_index < sizeof(k_cases) / sizeof(k_cases[0]); case_index++)
        wrap_line_case(k_cases[case_index].name,
                       k_cases[case_index].wrap,
                       k_cases[case_index].align_content,
                       k_cases[case_index].align_items,
                       k_cases[case_index].item_width,
                       k_cases[case_index].want_x);

    /* A single line with no wrap fills the cross axis, so alignItems centre halves 160 - 21 = 139 and
     * has to round 69.5 up. The plainest form of the whole fixture. */
    ERProps root_props = props_default();
    root_props.width = 200;
    root_props.height = 200;
    root_props.padding = 20;
    root_props.flex_direction = ER_FLEX_COL;
    root_props.align_items = ER_ALIGN_CENTER;
    ERNode* root = mk(root_props, NULL);
    ERRect child_rect;
    ERProps child_props = props_default();
    child_props.width = 21;
    child_props.height = 40;
    ERNode* first_child = mk(child_props, &child_rect);
    er_tree_append_child(root, first_child);
    er_tree_set_root(root);
    er_commit();
    pcheck("half-ai-nowrap", "c", EXPECT, child_rect, 90, 20, 21, 40);
    kill_child(root, first_child);
    er_node_destroy(root);
}

/** @brief One child's main-axis flex inputs for flex_case(). A 0 min/max means "no bound". */
typedef struct
{
    int16_t basis;    /**< flex_basis: the base main size before grow/shrink. */
    int16_t grow;     /**< flexGrow. */
    int16_t shrink;   /**< flexShrink. */
    int16_t min_size; /**< Min main size, 0 for none. */
    int16_t max_size; /**< Max main size, 0 for none. */
} FlexKid;

/**
 * @brief A single flex line, checking every child's main-axis position AND size.
 *
 * The cross axis is a fixed 40px the children stretch into, so the only interesting numbers are on
 * the main axis. Sizes come off the same pixel grid as positions -- a child spans from one rounded
 * edge to the next -- so a share that does not divide evenly lands on the child straddling the half
 * pixel rather than being dropped at the end of the line.
 *
 * @param[in] name           Assertion label.
 * @param[in] direction      flexDirection under test.
 * @param[in] box            Container size on the main axis, padding included.
 * @param[in] pad            Padding on all four edges.
 * @param[in] gap            Gap between children.
 * @param[in] count          Child count (<= 7).
 * @param[in] kid            Per-child flex inputs, in tree order.
 * @param[in] want_position  Expected main-axis position per child.
 * @param[in] want_size      Expected main-axis size per child.
 */
static void flex_case(const char* name,
                      uint8_t direction,
                      int16_t box,
                      int16_t pad,
                      int16_t gap,
                      int count,
                      const FlexKid* kid,
                      const int16_t* want_position,
                      const int16_t* want_size)
{
    const bool is_row = (direction == ER_FLEX_ROW || direction == ER_FLEX_ROW_REVERSE);

    ERProps root_props = props_default();
    root_props.width = is_row ? box : 40;
    root_props.height = is_row ? 40 : box;
    root_props.padding = pad;
    root_props.gap = gap;
    root_props.flex_direction = direction;
    ERNode* root = mk(root_props, NULL);

    ERNode* kids[7];
    ERRect rects[7];
    for (int kid_index = 0; kid_index < count; kid_index++)
    {
        ERProps kid_props = props_default();
        kid_props.flex_basis = kid[kid_index].basis;
        kid_props.flex_grow = kid[kid_index].grow;
        kid_props.flex_shrink = kid[kid_index].shrink;
        if (kid[kid_index].min_size)
        {
            if (is_row)
                kid_props.min_width = kid[kid_index].min_size;
            else
                kid_props.min_height = kid[kid_index].min_size;
        }
        if (kid[kid_index].max_size)
        {
            if (is_row)
                kid_props.max_width = kid[kid_index].max_size;
            else
                kid_props.max_height = kid[kid_index].max_size;
        }
        kids[kid_index] = mk(kid_props, &rects[kid_index]);
        er_tree_append_child(root, kids[kid_index]);
    }
    er_tree_set_root(root);
    er_commit();

    /* The cross axis is whatever padding leaves of the 40. */
    const int16_t cross_position = pad;
    const int16_t cross_size = (int16_t)(40 - 2 * pad);
    for (int kid_index = 0; kid_index < count; kid_index++)
    {
        char label[8];
        snprintf(label, sizeof(label), "c%d", kid_index);
        pcheck(name,
               label,
               EXPECT,
               rects[kid_index],
               is_row ? want_position[kid_index] : cross_position,
               is_row ? cross_position : want_position[kid_index],
               is_row ? want_size[kid_index] : cross_size,
               is_row ? cross_size : want_size[kid_index]);
    }

    for (int kid_index = 0; kid_index < count; kid_index++)
        kill_child(root, kids[kid_index]);
    er_node_destroy(root);
}

/**
 * @brief Free space that does not divide evenly: the leftover is spread, not dropped off the end.
 *
 * Yoga never distributes a size directly -- it carries floats through layout and then derives each
 * size from two rounded edges, so three `flex: 1` children in 100px are 33/34/33 rather than 33/33/33
 * with the row ending a pixel short. Both flexGrow and flexShrink land on the same rule, and so does
 * the min/max clamping that runs between the two: a bound has to be tested against the exact size,
 * since a truncated one sits inside a max the real one already exceeds.
 *
 * The `*-half` rows put an edge exactly on a half pixel, which is the only place the direction of the
 * axis changes the answer -- a reversed row measures the edges from the far end, so the mirror has to
 * run before the rounding. Values from `yoga-layout`.
 */
static void fixture_half_pixel_flex_sizes(void)
{
    static const FlexKid k_grow1[] = {{0, 1, 0, 0, 0},
                                      {0, 1, 0, 0, 0},
                                      {0, 1, 0, 0, 0},
                                      {0, 1, 0, 0, 0},
                                      {0, 1, 0, 0, 0},
                                      {0, 1, 0, 0, 0},
                                      {0, 1, 0, 0, 0}};

    /* 100 over three children: 33.33 each, so the middle one straddles the half pixel. */
    static const int16_t k_grow3_position[] = {0, 33, 67};
    static const int16_t k_grow3_size[] = {33, 34, 33};
    flex_case("flexsz-grow3", ER_FLEX_ROW, 100, 0, 0, 3, k_grow1, k_grow3_position, k_grow3_size);
    flex_case("flexsz-col3", ER_FLEX_COL, 100, 0, 0, 3, k_grow1, k_grow3_position, k_grow3_size);

    /* Padding shifts every edge by a whole pixel, so it cannot change which child takes the leftover;
     * 80 over three is 26.67, and this time it is the OUTER two that round up. */
    static const int16_t k_pad_position[] = {10, 37, 63};
    static const int16_t k_pad_size[] = {27, 26, 27};
    flex_case("flexsz-pad", ER_FLEX_ROW, 100, 10, 0, 3, k_grow1, k_pad_position, k_pad_size);

    /* A gap is a whole pixel too: 92 over three is 30.67. */
    static const int16_t k_gap_position[] = {0, 35, 69};
    static const int16_t k_gap_size[] = {31, 30, 31};
    flex_case("flexsz-gap4", ER_FLEX_ROW, 100, 0, 4, 3, k_grow1, k_gap_position, k_gap_size);

    /* 100 over seven: 14.29 each, so the leftover accumulates and two children take a pixel. */
    static const int16_t k_grow7_position[] = {0, 14, 29, 43, 57, 71, 86};
    static const int16_t k_grow7_size[] = {14, 15, 14, 14, 14, 15, 14};
    flex_case("flexsz-grow7", ER_FLEX_ROW, 100, 0, 0, 7, k_grow1, k_grow7_position, k_grow7_size);

    /* Unequal factors, and a non-zero basis the growth is added to. */
    static const FlexKid k_grow12[] = {{0, 1, 0, 0, 0}, {0, 2, 0, 0, 0}};
    static const int16_t k_grow12_position[] = {0, 33};
    static const int16_t k_grow12_size[] = {33, 67};
    flex_case("flexsz-grow1-2", ER_FLEX_ROW, 100, 0, 0, 2, k_grow12, k_grow12_position, k_grow12_size);

    static const FlexKid k_basis[] = {{10, 1, 0, 0, 0}, {10, 1, 0, 0, 0}, {10, 1, 0, 0, 0}};
    flex_case("flexsz-basis10", ER_FLEX_ROW, 100, 0, 0, 3, k_basis, k_grow3_position, k_grow3_size);

    /* Bounds. A child frozen at its bound leaves a new remainder for the rest to share, and the
     * bound has to be tested against the exact size: 200/3 is 66.67, which a max of 66 clamps even
     * though the truncated 66 would have fitted. */
    static const FlexKid k_max66a[] = {{0, 1, 0, 0, 66}, {0, 1, 0, 0, 0}, {0, 1, 0, 0, 0}};
    static const int16_t k_max66a_position[] = {0, 66, 133};
    static const int16_t k_max66a_size[] = {66, 67, 67};
    flex_case("flexsz-max66a", ER_FLEX_ROW, 200, 0, 0, 3, k_max66a, k_max66a_position, k_max66a_size);

    static const FlexKid k_max66b[] = {{0, 1, 0, 0, 0}, {0, 1, 0, 0, 66}, {0, 1, 0, 0, 0}};
    static const int16_t k_max66b_position[] = {0, 67, 133};
    static const int16_t k_max66b_size[] = {67, 66, 67};
    flex_case("flexsz-max66b", ER_FLEX_ROW, 200, 0, 0, 3, k_max66b, k_max66b_position, k_max66b_size);

    /* 100 with the first child pinned at 25: the other two share 75, i.e. 37.5 each. */
    static const FlexKid k_max25[] = {{0, 1, 0, 0, 25}, {0, 1, 0, 0, 0}, {0, 1, 0, 0, 0}};
    static const int16_t k_max25_position[] = {0, 25, 63};
    static const int16_t k_max25_size[] = {25, 38, 37};
    flex_case("flexsz-max25", ER_FLEX_ROW, 100, 0, 0, 3, k_max25, k_max25_position, k_max25_size);

    /* A min bounds the BASIS before the free space is measured, so 60 (not 100) is shared. */
    static const FlexKid k_min40[] = {{0, 1, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 1, 0, 40, 0}};
    static const int16_t k_min40_position[] = {0, 20, 40};
    static const int16_t k_min40_size[] = {20, 20, 60};
    flex_case("flexsz-min40", ER_FLEX_ROW, 100, 0, 0, 3, k_min40, k_min40_position, k_min40_size);

    /* Shrink is the same rule with the free space negative: 120 into 100 is 33.33 each. */
    static const FlexKid k_shrink40[] = {{40, 0, 1, 0, 0}, {40, 0, 1, 0, 0}, {40, 0, 1, 0, 0}};
    flex_case("flexsz-shrink3", ER_FLEX_ROW, 100, 0, 0, 3, k_shrink40, k_grow3_position, k_grow3_size);

    /* Shrink scales by the base size, so 80 into 60 takes 12.5 off the 50 and 7.5 off the 30. */
    static const FlexKid k_shrink2[] = {{50, 0, 1, 0, 0}, {30, 0, 1, 0, 0}};
    static const int16_t k_shrink2_position[] = {0, 38};
    static const int16_t k_shrink2_size[] = {38, 22};
    flex_case("flexsz-shrink2", ER_FLEX_ROW, 60, 0, 0, 2, k_shrink2, k_shrink2_position, k_shrink2_size);

    static const FlexKid k_shrink_uneven[] = {{50, 0, 1, 0, 0}, {30, 0, 1, 0, 0}, {40, 0, 2, 0, 0}};
    static const int16_t k_shrink_uneven_position[] = {0, 44, 70};
    static const int16_t k_shrink_uneven_size[] = {44, 26, 30};
    flex_case(
        "flexsz-shrink-un", ER_FLEX_ROW, 100, 0, 0, 3, k_shrink_uneven, k_shrink_uneven_position, k_shrink_uneven_size);

    /* Reversed: the same sizes read from the other end, since neither edge lands on a half pixel. */
    static const int16_t k_reverse3_position[] = {67, 33, 0};
    flex_case("flexsz-rgrow3", ER_FLEX_ROW_REVERSE, 100, 0, 0, 3, k_grow1, k_reverse3_position, k_grow3_size);
    flex_case("flexsz-rshrink3", ER_FLEX_ROW_REVERSE, 100, 0, 0, 3, k_shrink40, k_reverse3_position, k_grow3_size);

    /* Two children in 5px sit on exactly 2.5, the one case where the direction of the axis decides
     * which of them gets the pixel. */
    static const int16_t k_half_position[] = {0, 3};
    static const int16_t k_half_size[] = {3, 2};
    static const int16_t k_reverse_half_position[] = {3, 0};
    static const int16_t k_reverse_half_size[] = {2, 3};
    flex_case("flexsz-half", ER_FLEX_ROW, 5, 0, 0, 2, k_grow1, k_half_position, k_half_size);
    flex_case("flexsz-rhalf", ER_FLEX_ROW_REVERSE, 5, 0, 0, 2, k_grow1, k_reverse_half_position, k_reverse_half_size);
}

/**
 * @brief Wrapped 40px-tall children with no width, each stretched to fill its line.
 *
 * With no width of their own the children take their line's cross extent, so their rects report how
 * alignContent: 'stretch' shared the leftover cross space between the lines.
 *
 * @param[in] name        Assertion label.
 * @param[in] width       Container width (the cross axis).
 * @param[in] height      Container height (the main axis).
 * @param[in] pad         Padding on all four edges.
 * @param[in] wrap        ER_WRAP_WRAP or ER_WRAP_WRAP_REVERSE.
 * @param[in] kid_count   Child count (<= 9).
 * @param[in] want_x      Expected x per line.
 * @param[in] want_width  Expected width per line.
 */
static void stretch_line_case(const char* name,
                              int16_t width,
                              int16_t height,
                              int16_t pad,
                              uint8_t wrap,
                              int kid_count,
                              const int16_t* want_x,
                              const int16_t* want_width)
{
    er_reset();

    ERProps root_props = props_default();
    root_props.width = width;
    root_props.height = height;
    root_props.padding = pad;
    root_props.flex_direction = ER_FLEX_COL;
    root_props.flex_wrap = wrap;
    root_props.align_content = ER_ALIGN_CONTENT_STRETCH;
    root_props.align_items = ER_ALIGN_STRETCH;
    ERNode* root = mk(root_props, NULL);

    ERNode* kids[9];
    ERRect rects[9];
    for (int kid_index = 0; kid_index < kid_count; kid_index++)
    {
        ERProps kid_props = props_default();
        kid_props.height = 40;
        kids[kid_index] = mk(kid_props, &rects[kid_index]);
        er_tree_append_child(root, kids[kid_index]);
    }
    er_tree_set_root(root);
    er_commit();

    const int per_line = (height - 2 * pad) / 40;
    for (int line_index = 0; line_index * per_line < kid_count; line_index++)
    {
        char label[8];
        snprintf(label, sizeof(label), "l%d", line_index);
        pcheck(name, label, EXPECT, rects[line_index * per_line], want_x[line_index], pad, want_width[line_index], 40);
    }

    for (int kid_index = 0; kid_index < kid_count; kid_index++)
        kill_child(root, kids[kid_index]);
    er_node_destroy(root);
}

/**
 * @brief alignContent: 'stretch' growth that does not divide evenly by the line count.
 *
 * A line's extent is the difference of its two rounded cross edges, exactly like a flex child's size,
 * so the leftover goes to the line straddling the half pixel instead of being dropped at the far
 * edge. wrap-reverse measures those edges from the other end. Values from `yoga-layout`.
 */
static void fixture_half_pixel_stretch_lines(void)
{
    /* Three lines with nothing of their own in a 160 content box: 53.33 each. */
    static const int16_t k_line_x[] = {20, 73, 127};
    static const int16_t k_line_width[] = {53, 54, 53};
    static const int16_t k_reverse_line_x[] = {127, 73, 20};
    stretch_line_case("acs-3lines", 200, 200, 20, ER_WRAP_WRAP, 9, k_line_x, k_line_width);
    stretch_line_case("acs-r3lines", 200, 200, 20, ER_WRAP_WRAP_REVERSE, 9, k_reverse_line_x, k_line_width);

    /* Two lines in 5px sit on exactly 2.5, so the mirror decides which line takes the pixel. */
    static const int16_t k_half_line_x[] = {0, 3};
    static const int16_t k_half_line_width[] = {3, 2};
    static const int16_t k_reverse_half_line_x[] = {3, 0};
    static const int16_t k_reverse_half_line_width[] = {2, 3};
    stretch_line_case("acs-half", 5, 100, 0, ER_WRAP_WRAP, 4, k_half_line_x, k_half_line_width);
    stretch_line_case(
        "acs-rhalf", 5, 100, 0, ER_WRAP_WRAP_REVERSE, 4, k_reverse_half_line_x, k_reverse_half_line_width);
}

/**
 * @brief Builds a stretched column holding a wrapping row of five 40×20 items, then a 10px marker.
 *
 * The row has no height: its auto height must come from its wrapped lines. Column is `column_width`
 * wide; the row carries `row_gap`/`column_gap` and 5px padding, so its inner width is column_width - 10.
 * When `nested` is set, the row sits inside an auto-sized column (alignItems flex-start), so the
 * width constraint has to travel through a measured ancestor.
 */
static void build_wrap_auto(int16_t column_width, int16_t gap, bool nested, ERRect* row, ERRect* marker, ERRect* outer)
{
    er_reset();

    ERProps column_props = props_default();
    column_props.width = column_width;
    column_props.height = 200;
    column_props.flex_direction = ER_FLEX_COL;
    column_props.align_items = nested ? ER_ALIGN_FLEX_START : ER_ALIGN_STRETCH;
    ERNode* column = mk(column_props, NULL);

    ERNode* parent = column;
    if (nested)
    {
        ERProps outer_props = props_default();
        outer_props.flex_direction = ER_FLEX_COL;
        parent = mk(outer_props, outer);
        er_tree_append_child(column, parent);
    }

    ERProps root_props = props_default();
    root_props.flex_direction = ER_FLEX_ROW;
    root_props.flex_wrap = ER_WRAP_WRAP;
    root_props.row_gap = gap;
    root_props.column_gap = gap;
    root_props.padding = 5;
    ERNode* wrap = mk(root_props, row);
    er_tree_append_child(parent, wrap);
    for (int item_index = 0; item_index < 5; item_index++)
    {
        ERProps props = props_default();
        props.width = 40;
        props.height = 20;
        er_tree_append_child(wrap, mk(props, NULL));
    }

    ERProps marker_props = props_default();
    marker_props.height = 10;
    er_tree_append_child(column, mk(marker_props, marker));

    er_tree_set_root(column);
    er_commit();
}

/** @brief An auto-height wrapping row in a column grows to hold all of its lines. */
static void fixture_wrap_auto_height(void)
{
    ERRect row, marker;
    /* Inner 90: two 40s per line (80), so five items take three lines: 3*20 + 10 padding. */
    build_wrap_auto(100, 0, false, &row, &marker, NULL);
    pcheck("wrap-auto-height", "row", EXPECT, row, 0, 0, 100, 70);
    pcheck("wrap-auto-height", "marker", EXPECT, marker, 0, 70, 100, 10);
    er_reset();
}

/** @brief Gaps count toward line breaking (main) and between lines (cross). */
static void fixture_wrap_auto_height_gap(void)
{
    ERRect row, marker;
    /* Inner 120: 40+10+40+10+40 = 140 > 120, so two per line → three lines: 3*20 + 2*10 + 10. */
    build_wrap_auto(130, 10, false, &row, &marker, NULL);
    pcheck("wrap-auto-gap", "row", EXPECT, row, 0, 0, 130, 90);
    pcheck("wrap-auto-gap", "marker", EXPECT, marker, 0, 90, 130, 10);
    er_reset();
}

/** @brief The width constraint reaches a wrapping row through an auto-sized ancestor. */
static void fixture_wrap_auto_height_nested(void)
{
    ERRect row, marker, outer;
    /* The ancestor shrinks to the widest line (40+40 + 10 padding = 90), as Yoga's at-most mode does. */
    build_wrap_auto(100, 0, true, &row, &marker, &outer);
    pcheck("wrap-auto-nested", "outer", EXPECT, outer, 0, 0, 90, 70);
    pcheck("wrap-auto-nested", "row", EXPECT, row, 0, 0, 90, 70);
    pcheck("wrap-auto-nested", "marker", EXPECT, marker, 0, 70, 0, 10);
    er_reset();
}

/** @brief A wrapping row sized by flex inside a row grows to hold the lines it wraps into at its flexed width. */
static void fixture_wrap_auto_height_flexed(void)
{
    er_reset();

    ERProps column_props = props_default();
    column_props.width = 200;
    column_props.height = 200;
    column_props.flex_direction = ER_FLEX_COL;
    ERNode* column = mk(column_props, NULL);

    ERRect outer, wrap_rect, marker;
    ERProps outer_props = props_default();
    outer_props.flex_direction = ER_FLEX_ROW;
    outer_props.align_items = ER_ALIGN_FLEX_START;
    ERNode* row = mk(outer_props, &outer);
    er_tree_append_child(column, row);

    ERProps leaf_props = props_default();
    leaf_props.width = 40;
    leaf_props.height = 20;
    er_tree_append_child(row, mk(leaf_props, NULL));

    ERProps root_props = props_default();
    root_props.flex_direction = ER_FLEX_ROW;
    root_props.flex_wrap = ER_WRAP_WRAP;
    root_props.flex_grow = 1;
    root_props.flex_shrink = 1;
    root_props.flex_basis = 0;
    ERNode* wrap = mk(root_props, &wrap_rect);
    er_tree_append_child(row, wrap);
    for (int item_index = 0; item_index < 5; item_index++)
        er_tree_append_child(wrap, mk(leaf_props, NULL));

    ERProps marker_props = props_default();
    marker_props.height = 10;
    er_tree_append_child(column, mk(marker_props, &marker));

    er_tree_set_root(column);
    er_commit();

    /* Flexed to 160, the wrapping row holds four 40s on its first line and one on its second. */
    pcheck("wrap-auto-flexed", "outer", EXPECT, outer, 0, 0, 200, 40);
    pcheck("wrap-auto-flexed", "wrap", EXPECT, wrap_rect, 40, 0, 160, 40);
    pcheck("wrap-auto-flexed", "marker", EXPECT, marker, 0, 40, 200, 10);
    er_reset();
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Runs every parity fixture and reports the aggregate result.
 *
 * @return EXIT_SUCCESS when all EXPECT assertions match and every XFAIL still diverges;
 *         EXIT_FAILURE on a regression (EXPECT mismatch) or a promotable XFAIL (now matches Yoga).
 */
int main(void)
{
    static const EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, NULL};
    embedded_renderer_set_backend(&backend);

    fixture_column_stack();
    fixture_row_flex_even();
    fixture_justify_between();
    fixture_align_center();
    fixture_padding_stretch();
    fixture_gap_row();
    fixture_auto_cross_row();
    fixture_auto_main_column();
    fixture_flex_max_redistribute();
    fixture_align_content_center();
    fixture_align_content_between();
    fixture_align_content_stretch();
    fixture_pct_width_main();
    fixture_pct_height_main();
    fixture_pct_width_cross();
    fixture_pct_content_box();
    fixture_abs_auto_height();
    fixture_abs_auto_both();
    fixture_abs_pct();
    fixture_abs_aspect_ratio();
    fixture_abs_aspect_ratio_both_auto();
    fixture_flow_aspect_ratio_row();
    fixture_flow_aspect_ratio_column();
    fixture_flow_aspect_ratio_clamped();
    fixture_flow_aspect_ratio_auto_parent();
    fixture_flow_aspect_ratio_pct_measured();
    fixture_flow_aspect_ratio_flexed();
    fixture_flow_aspect_ratio_stretch();
    fixture_abs_content_overflows();
    fixture_abs_containing_block();
    fixture_abs_padding_box_edges();
    fixture_abs_inset_pair_padding();
    fixture_abs_pct_inset();
    fixture_abs_pct_inset_axis();
    fixture_abs_pct_inset_far();
    fixture_abs_pct_inset_pair();
    fixture_abs_pct_inset_negative();
    fixture_abs_pct_inset_margin();
    fixture_abs_pct_inset_half_pixel();
    fixture_abs_pct_pair_half_pixel();
    fixture_abs_pct_saturates();
    fixture_abs_far_inset_auto_size();
    fixture_rel_pct_offset();
    fixture_rel_pct_offset_far();
    fixture_abs_margin_inset_padding();
    fixture_abs_static_position_padding();
    fixture_abs_static_position_align();
    fixture_abs_static_justify();
    fixture_abs_static_align();
    fixture_abs_static_stretch();
    fixture_abs_static_reverse();
    fixture_abs_static_margins();
    fixture_abs_static_overflow();
    fixture_abs_static_per_axis();
    fixture_abs_static_ignores_siblings();
    fixture_flow_reverse_margin();
    fixture_flow_reverse_margin_justify();
    fixture_flow_wrap_reverse_anchor();
    fixture_flow_wrap_reverse_align_content();
    fixture_flow_wrap_reverse_overflow();
    fixture_half_pixel_center();
    fixture_half_pixel_justify();
    fixture_flow_justify_overflow();
    fixture_half_pixel_align_content();
    fixture_half_pixel_flex_sizes();
    fixture_half_pixel_stretch_lines();
    fixture_wrap_auto_height();
    fixture_wrap_auto_height_gap();
    fixture_wrap_auto_height_nested();
    fixture_wrap_auto_height_flexed();

    printf("\nYoga parity: %d passed, %d known-divergence (xfail), %d regressions, %d to promote\n",
           g_pass,
           g_xfail,
           g_regress,
           g_promote);

    if (g_regress > 0)
    {
        fprintf(stderr, "FAIL: %d EXPECT assertion(s) regressed against Yoga parity\n", g_regress);
        return EXIT_FAILURE;
    }
    if (g_promote > 0)
    {
        fprintf(
            stderr, "FAIL: %d XFAIL assertion(s) now match Yoga -- promote them to EXPECT (good news!)\n", g_promote);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

/*----------------------------------------------------------------------------------------------------------------------
 * Known divergences not yet expressible through ERProps (add fixtures when the props/fields land):
 *
 *   - margin: auto centering: margins are fixed pixels; ER_LAYOUT_AUTO margin is treated as 0.
 *   - percentage padding/margin/min/max/position: width%, height% and flex_basis% have fields
 *     (width%/height% covered by the pct-* fixtures above); the rest do not yet.
 *   - alignItems: baseline: no baseline alignment.
 *   - cross-axis margins under wrap-reverse: Pass 5 places a child with its LEADING cross margin and
 *     then mirrors, so the mirrored child is held off the far edge by the wrong margin. This is the
 *     main-axis bug of #193 one axis over, but it is deliberately NOT fixed alongside it, because the
 *     two references disagree on the answer: for a 50px child with marginLeft 7 / marginRight 3 in
 *     the fixture_flow_wrap_reverse_anchor container, Chrome says x=127 (the axis flips wholesale, so
 *     marginRight leads) while Yoga says x=130 (its wrap path drops the leading cross margin
 *     altogether — plain `wrap` + marginLeft 7 is x=20 there and x=27 in Chrome). The engine says
 *     123. Pinning it means first choosing which reference wins.
 *--------------------------------------------------------------------------------------------------------------------*/
