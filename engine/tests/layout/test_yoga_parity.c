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
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)argb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/** @brief No-op copy. @param s src. @param st stride. @param x x. @param y y. @param w w. @param h h. @param c ctx. */
static void copy_cb(const void* s, int st, int x, int y, int w, int h, void* c)
{
    (void)s;
    (void)st;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)c;
}

/** @brief No-op blend. @param s src. @param st stride. @param a alpha. @param x x. @param y y. @param w w. @param h h.
 * @param c ctx. */
static void blend_cb(const void* s, int st, uint8_t a, int x, int y, int w, int h, void* c)
{
    (void)s;
    (void)st;
    (void)a;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)c;
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
    ERProps p = {0};
    p.left = p.top = p.right = p.bottom = ER_LAYOUT_AUTO;
    p.width = p.height = ER_LAYOUT_AUTO;
    p.min_width = p.max_width = ER_LAYOUT_AUTO;
    p.min_height = p.max_height = ER_LAYOUT_AUTO;
    p.padding = p.padding_left = p.padding_top = ER_LAYOUT_AUTO;
    p.padding_right = p.padding_bottom = ER_LAYOUT_AUTO;
    p.padding_horizontal = p.padding_vertical = ER_LAYOUT_AUTO;
    p.margin = p.margin_left = p.margin_top = ER_LAYOUT_AUTO;
    p.margin_right = p.margin_bottom = ER_LAYOUT_AUTO;
    p.margin_horizontal = p.margin_vertical = ER_LAYOUT_AUTO;
    p.gap = p.row_gap = p.column_gap = ER_LAYOUT_AUTO;
    p.flex_basis = ER_LAYOUT_AUTO;
    p.opacity = 255U;
    return p;
}

/**
 * @brief Creates a View with the given props, optionally capturing its computed rect.
 *
 * @param[in]  p    Props to apply.
 * @param[out] cap  ERRect to receive the node's computed rect via ER_EVENT_LAYOUT, or NULL.
 *
 * @return The created node.
 */
static ERNode* mk(ERProps p, ERRect* cap)
{
    ERNode* n = er_node_create(ER_NODE_VIEW);
    er_node_set_props(n, &p);
    if (cap)
    {
        *cap = (ERRect){-1, -1, -1, -1};
        er_event_set(n, ER_EVENT_LAYOUT, on_rect, cap);
    }
    return n;
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
 * @param[in] fx   Fixture name (for output).
 * @param[in] lbl  Node label (for output).
 * @param[in] st   EXPECT or XFAIL.
 * @param[in] got  The engine's computed rect.
 * @param[in] x    Expected (Yoga) x.
 * @param[in] y    Expected (Yoga) y.
 * @param[in] w    Expected (Yoga) width.
 * @param[in] h    Expected (Yoga) height.
 */
static void pcheck(const char* fx, const char* lbl, ParityStatus st, ERRect got, int x, int y, int w, int h)
{
    const bool match = (got.x == x && got.y == y && got.w == w && got.h == h);
    if (st == EXPECT)
    {
        if (match)
        {
            g_pass++;
            printf("  [PASS ] %-20s %-8s (%d,%d,%d,%d)\n", fx, lbl, got.x, got.y, got.w, got.h);
        }
        else
        {
            g_regress++;
            printf("  [FAIL ] %-20s %-8s got (%d,%d,%d,%d) want (%d,%d,%d,%d)\n",
                   fx,
                   lbl,
                   got.x,
                   got.y,
                   got.w,
                   got.h,
                   x,
                   y,
                   w,
                   h);
        }
    }
    else
    {
        if (!match)
        {
            g_xfail++;
            printf("  [xfail] %-20s %-8s engine (%d,%d,%d,%d) != yoga (%d,%d,%d,%d)  [known gap]\n",
                   fx,
                   lbl,
                   got.x,
                   got.y,
                   got.w,
                   got.h,
                   x,
                   y,
                   w,
                   h);
        }
        else
        {
            g_promote++;
            printf("  [PROMO] %-20s %-8s now matches yoga (%d,%d,%d,%d) -- change XFAIL to EXPECT\n",
                   fx,
                   lbl,
                   got.x,
                   got.y,
                   got.w,
                   got.h);
        }
    }
}

/** @brief Detaches a child from its parent and destroys it. @param parent p. @param child c. */
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
    ERProps rp = props_default();
    rp.width = 100;
    rp.height = 100;
    rp.flex_direction = ER_FLEX_COL;
    rp.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(rp, NULL);

    ERRect r0, r1;
    ERProps p = props_default();
    p.width = 40;
    p.height = 20;
    ERNode* c0 = mk(p, &r0);
    p.height = 30;
    ERNode* c1 = mk(p, &r1);
    er_tree_append_child(root, c0);
    er_tree_append_child(root, c1);

    er_tree_set_root(root);
    er_commit();

    pcheck("column-stack", "c0", EXPECT, r0, 0, 0, 40, 20);
    pcheck("column-stack", "c1", EXPECT, r1, 0, 20, 40, 30);

    kill_child(root, c0);
    kill_child(root, c1);
    er_node_destroy(root);
}

/** @brief Row with two flex:1 children splits the main axis evenly and stretches on cross. */
static void fixture_row_flex_even(void)
{
    ERProps rp = props_default();
    rp.width = 100;
    rp.height = 40;
    rp.flex_direction = ER_FLEX_ROW;
    ERNode* root = mk(rp, NULL);

    ERRect r0, r1;
    ERProps p = props_default();
    p.flex_grow = 1;
    p.flex_shrink = 1;
    p.flex_basis = 0;
    ERNode* c0 = mk(p, &r0);
    ERNode* c1 = mk(p, &r1);
    er_tree_append_child(root, c0);
    er_tree_append_child(root, c1);

    er_tree_set_root(root);
    er_commit();

    pcheck("row-flex-even", "c0", EXPECT, r0, 0, 0, 50, 40);
    pcheck("row-flex-even", "c1", EXPECT, r1, 50, 0, 50, 40);

    kill_child(root, c0);
    kill_child(root, c1);
    er_node_destroy(root);
}

/** @brief justifyContent: space-between pushes children to the edges. */
static void fixture_justify_between(void)
{
    ERProps rp = props_default();
    rp.width = 100;
    rp.height = 40;
    rp.flex_direction = ER_FLEX_ROW;
    rp.justify_content = ER_JUSTIFY_SPACE_BETWEEN;
    rp.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(rp, NULL);

    ERRect r0, r1;
    ERProps p = props_default();
    p.width = 20;
    p.height = 20;
    ERNode* c0 = mk(p, &r0);
    ERNode* c1 = mk(p, &r1);
    er_tree_append_child(root, c0);
    er_tree_append_child(root, c1);

    er_tree_set_root(root);
    er_commit();

    pcheck("justify-between", "c0", EXPECT, r0, 0, 0, 20, 20);
    pcheck("justify-between", "c1", EXPECT, r1, 80, 0, 20, 20);

    kill_child(root, c0);
    kill_child(root, c1);
    er_node_destroy(root);
}

/** @brief alignItems: center centers a child on the cross axis. */
static void fixture_align_center(void)
{
    ERProps rp = props_default();
    rp.width = 100;
    rp.height = 40;
    rp.flex_direction = ER_FLEX_ROW;
    rp.align_items = ER_ALIGN_CENTER;
    ERNode* root = mk(rp, NULL);

    ERRect r0;
    ERProps p = props_default();
    p.width = 20;
    p.height = 20;
    ERNode* c0 = mk(p, &r0);
    er_tree_append_child(root, c0);

    er_tree_set_root(root);
    er_commit();

    pcheck("align-center", "c0", EXPECT, r0, 0, 10, 20, 20);

    kill_child(root, c0);
    er_node_destroy(root);
}

/** @brief Padding insets the content box; a stretch child fills it. */
static void fixture_padding_stretch(void)
{
    ERProps rp = props_default();
    rp.width = 100;
    rp.height = 100;
    rp.flex_direction = ER_FLEX_COL;
    rp.padding = 10;
    ERNode* root = mk(rp, NULL);

    ERRect r0;
    ERProps p = props_default();
    p.height = 20;
    ERNode* c0 = mk(p, &r0);
    er_tree_append_child(root, c0);

    er_tree_set_root(root);
    er_commit();

    pcheck("padding-stretch", "c0", EXPECT, r0, 10, 10, 80, 20);

    kill_child(root, c0);
    er_node_destroy(root);
}

/** @brief gap inserts space between row children. */
static void fixture_gap_row(void)
{
    ERProps rp = props_default();
    rp.width = 100;
    rp.height = 40;
    rp.flex_direction = ER_FLEX_ROW;
    rp.gap = 10;
    rp.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(rp, NULL);

    ERRect r0, r1;
    ERProps p = props_default();
    p.width = 20;
    p.height = 20;
    ERNode* c0 = mk(p, &r0);
    ERNode* c1 = mk(p, &r1);
    er_tree_append_child(root, c0);
    er_tree_append_child(root, c1);

    er_tree_set_root(root);
    er_commit();

    pcheck("gap-row", "c0", EXPECT, r0, 0, 0, 20, 20);
    pcheck("gap-row", "c1", EXPECT, r1, 30, 0, 20, 20);

    kill_child(root, c0);
    kill_child(root, c1);
    er_node_destroy(root);
}

/** @brief A row with no explicit height grows to its children's height (auto cross-size). */
static void fixture_auto_cross_row(void)
{
    ERProps rp = props_default();
    rp.width = 100;
    rp.height = 100;
    rp.flex_direction = ER_FLEX_COL;
    ERNode* root = mk(rp, NULL);

    ERRect rowr, r0, r1;
    ERProps rowp = props_default();
    rowp.flex_direction = ER_FLEX_ROW; /* no height — derived from children */
    ERNode* row = mk(rowp, &rowr);
    er_tree_append_child(root, row);

    ERProps p = props_default();
    p.flex_grow = 1;
    p.flex_shrink = 1;
    p.flex_basis = 0;
    p.height = 30;
    ERNode* c0 = mk(p, &r0);
    ERNode* c1 = mk(p, &r1);
    er_tree_append_child(row, c0);
    er_tree_append_child(row, c1);

    er_tree_set_root(root);
    er_commit();

    pcheck("auto-cross-row", "row", EXPECT, rowr, 0, 0, 100, 30);
    pcheck("auto-cross-row", "c0", EXPECT, r0, 0, 0, 50, 30);
    pcheck("auto-cross-row", "c1", EXPECT, r1, 50, 0, 50, 30);

    kill_child(row, c0);
    kill_child(row, c1);
    kill_child(root, row);
    er_node_destroy(root);
}

/** @brief A column with no explicit height grows to the sum of its children's heights. */
static void fixture_auto_main_column(void)
{
    ERProps rp = props_default();
    rp.width = 100;
    rp.height = 100;
    rp.flex_direction = ER_FLEX_COL;
    rp.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(rp, NULL);

    ERRect innerr, r0, r1;
    ERProps innerp = props_default();
    innerp.flex_direction = ER_FLEX_COL; /* no width/height — derived from children */
    ERNode* inner = mk(innerp, &innerr);
    er_tree_append_child(root, inner);

    ERProps p = props_default();
    p.width = 30;
    p.height = 20;
    ERNode* c0 = mk(p, &r0);
    ERNode* c1 = mk(p, &r1);
    er_tree_append_child(inner, c0);
    er_tree_append_child(inner, c1);

    er_tree_set_root(root);
    er_commit();

    pcheck("auto-main-col", "inner", EXPECT, innerr, 0, 0, 30, 40);
    pcheck("auto-main-col", "c0", EXPECT, r0, 0, 0, 30, 20);
    pcheck("auto-main-col", "c1", EXPECT, r1, 0, 20, 30, 20);

    kill_child(inner, c0);
    kill_child(inner, c1);
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
    ERProps rp = props_default();
    rp.width = 100;
    rp.height = 40;
    rp.flex_direction = ER_FLEX_ROW;
    rp.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(rp, NULL);

    ERRect r0, r1;
    ERProps p = props_default();
    p.flex_grow = 1;
    p.flex_shrink = 1;
    p.flex_basis = 0;
    p.height = 20;
    p.max_width = 20;
    ERNode* c0 = mk(p, &r0);

    p = props_default();
    p.flex_grow = 1;
    p.flex_shrink = 1;
    p.flex_basis = 0;
    p.height = 20;
    ERNode* c1 = mk(p, &r1);
    er_tree_append_child(root, c0);
    er_tree_append_child(root, c1);

    er_tree_set_root(root);
    er_commit();

    /* c0 is clamped to 20; the freed 30px is redistributed to c1 (→ 80), matching Yoga. */
    pcheck("flex-max-redist", "c0", EXPECT, r0, 0, 0, 20, 20);
    pcheck("flex-max-redist", "c1", EXPECT, r1, 20, 0, 80, 20);

    kill_child(root, c0);
    kill_child(root, c1);
    er_node_destroy(root);
}

/**
 * @brief Builds a wrapping row of three 50×20 items (one per line) under the given alignContent.
 *
 * Container is 50 wide so each item wraps onto its own line; `height` controls the leftover cross
 * space. Captures the three item rects for the caller to assert.
 */
static void
build_wrap3(uint8_t align_content, int16_t height, int16_t item_h, ERNode** root_out, ERNode** kids_out, ERRect* rects)
{
    ERProps rp = props_default();
    rp.width = 50;
    rp.height = height;
    rp.flex_direction = ER_FLEX_ROW;
    rp.flex_wrap = ER_WRAP_WRAP;
    rp.align_content = align_content;
    ERNode* root = mk(rp, NULL);

    for (int i = 0; i < 3; i++)
    {
        ERProps p = props_default();
        p.width = 50;
        if (item_h != ER_LAYOUT_AUTO)
        {
            p.height = item_h;
        }
        kids_out[i] = mk(p, &rects[i]);
        er_tree_append_child(root, kids_out[i]);
    }
    er_tree_set_root(root);
    *root_out = root;
}

/** @brief Tears down a wrap3 fixture. */
static void teardown_wrap3(ERNode* root, ERNode** kids)
{
    for (int i = 0; i < 3; i++)
    {
        kill_child(root, kids[i]);
    }
    er_node_destroy(root);
}

/** @brief alignContent: center — leftover cross space splits evenly above and below the lines. */
static void fixture_align_content_center(void)
{
    ERNode* root;
    ERNode* kids[3];
    ERRect r[3];
    /* 3 lines × 20 = 60 used of 100 → 40 free; center → 20px leading offset. */
    build_wrap3(ER_ALIGN_CONTENT_CENTER, 100, 20, &root, kids, r);
    er_commit();
    pcheck("aligncontent-center", "l0", EXPECT, r[0], 0, 20, 50, 20);
    pcheck("aligncontent-center", "l1", EXPECT, r[1], 0, 40, 50, 20);
    pcheck("aligncontent-center", "l2", EXPECT, r[2], 0, 60, 50, 20);
    teardown_wrap3(root, kids);
}

/** @brief alignContent: space-between — leftover cross space goes between lines, none at edges. */
static void fixture_align_content_between(void)
{
    ERNode* root;
    ERNode* kids[3];
    ERRect r[3];
    /* 40 free / (3-1) = 20px between lines. */
    build_wrap3(ER_ALIGN_CONTENT_SPACE_BETWEEN, 100, 20, &root, kids, r);
    er_commit();
    pcheck("aligncontent-between", "l0", EXPECT, r[0], 0, 0, 50, 20);
    pcheck("aligncontent-between", "l1", EXPECT, r[1], 0, 40, 50, 20);
    pcheck("aligncontent-between", "l2", EXPECT, r[2], 0, 80, 50, 20);
    teardown_wrap3(root, kids);
}

/** @brief alignContent: stretch — each line grows equally; auto-height items fill their line. */
static void fixture_align_content_stretch(void)
{
    ERNode* root;
    ERNode* kids[3];
    ERRect r[3];
    /* Auto-height items: base line cross 0, 120 free / 3 lines = 40px tall lines; items stretch. */
    build_wrap3(ER_ALIGN_CONTENT_STRETCH, 120, ER_LAYOUT_AUTO, &root, kids, r);
    er_commit();
    pcheck("aligncontent-stretch", "l0", EXPECT, r[0], 0, 0, 50, 40);
    pcheck("aligncontent-stretch", "l1", EXPECT, r[1], 0, 40, 50, 40);
    pcheck("aligncontent-stretch", "l2", EXPECT, r[2], 0, 80, 50, 40);
    teardown_wrap3(root, kids);
}

/** @brief Percentage width on the main axis: 50% of a 200px row → 100px. */
static void fixture_pct_width_main(void)
{
    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 80;
    rp.flex_direction = ER_FLEX_ROW;
    rp.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(rp, NULL);

    ERRect r0;
    ERProps p = props_default();
    p.width_pct = 50.0f;
    p.height = 40;
    ERNode* c0 = mk(p, &r0);
    er_tree_append_child(root, c0);

    er_tree_set_root(root);
    er_commit();
    pcheck("pct-width-main", "c0", EXPECT, r0, 0, 0, 100, 40);

    kill_child(root, c0);
    er_node_destroy(root);
}

/** @brief Percentage height on the main axis: 25% of a 200px column → 50px. */
static void fixture_pct_height_main(void)
{
    ERProps rp = props_default();
    rp.width = 100;
    rp.height = 200;
    rp.flex_direction = ER_FLEX_COL;
    rp.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(rp, NULL);

    ERRect r0;
    ERProps p = props_default();
    p.width = 30;
    p.height_pct = 25.0f;
    ERNode* c0 = mk(p, &r0);
    er_tree_append_child(root, c0);

    er_tree_set_root(root);
    er_commit();
    pcheck("pct-height-main", "c0", EXPECT, r0, 0, 0, 30, 50);

    kill_child(root, c0);
    er_node_destroy(root);
}

/** @brief Percentage width on the cross axis: 50% of a 200px-wide column → 100px. */
static void fixture_pct_width_cross(void)
{
    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 100;
    rp.flex_direction = ER_FLEX_COL;
    rp.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(rp, NULL);

    ERRect r0;
    ERProps p = props_default();
    p.width_pct = 50.0f;
    p.height = 20;
    ERNode* c0 = mk(p, &r0);
    er_tree_append_child(root, c0);

    er_tree_set_root(root);
    er_commit();
    pcheck("pct-width-cross", "c0", EXPECT, r0, 0, 0, 100, 20);

    kill_child(root, c0);
    er_node_destroy(root);
}

/** @brief Percentages resolve against the parent's content box: 50% of (200 − 2×20 padding) = 80. */
static void fixture_pct_content_box(void)
{
    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 80;
    rp.flex_direction = ER_FLEX_ROW;
    rp.align_items = ER_ALIGN_FLEX_START;
    rp.padding = 20;
    ERNode* root = mk(rp, NULL);

    ERRect r0;
    ERProps p = props_default();
    p.width_pct = 50.0f;
    p.height = 40;
    ERNode* c0 = mk(p, &r0);
    er_tree_append_child(root, c0);

    er_tree_set_root(root);
    er_commit();
    pcheck("pct-content-box", "c0", EXPECT, r0, 20, 20, 80, 40);

    kill_child(root, c0);
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
    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 200;
    rp.flex_direction = ER_FLEX_COL;
    rp.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(rp, NULL);

    ERRect ra, rk;
    ERProps ap = props_default();
    ap.position = ER_POS_ABSOLUTE;
    ap.left = 20;
    ap.top = 30;
    ap.width = 100;
    ERNode* abs = mk(ap, &ra);

    ERProps kp = props_default();
    kp.width = 80;
    kp.height = 40;
    ERNode* kid = mk(kp, &rk);

    er_tree_append_child(abs, kid);
    er_tree_append_child(root, abs);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-auto-height", "abs", EXPECT, ra, 20, 30, 100, 40);
    pcheck("abs-auto-height", "kid", EXPECT, rk, 20, 30, 80, 40);

    kill_child(abs, kid);
    kill_child(root, abs);
    er_node_destroy(root);
}

/** @brief Absolute with neither width nor height sizes both axes to content (70 wide, 20+30 tall). */
static void fixture_abs_auto_both(void)
{
    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 200;
    ERNode* root = mk(rp, NULL);

    ERRect ra;
    ERProps ap = props_default();
    ap.position = ER_POS_ABSOLUTE;
    ap.left = 10;
    ap.top = 10;
    ap.flex_direction = ER_FLEX_COL;
    ERNode* abs = mk(ap, &ra);

    ERProps kp = props_default();
    kp.width = 60;
    kp.height = 20;
    ERNode* k0 = mk(kp, NULL);
    kp.width = 70;
    kp.height = 30;
    ERNode* k1 = mk(kp, NULL);

    er_tree_append_child(abs, k0);
    er_tree_append_child(abs, k1);
    er_tree_append_child(root, abs);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-auto-both", "abs", EXPECT, ra, 10, 10, 70, 50);

    kill_child(abs, k0);
    kill_child(abs, k1);
    kill_child(root, abs);
    er_node_destroy(root);
}

/** @brief Percentage width/height on an absolute resolve against the containing block: 50%/25% of 200x100. */
static void fixture_abs_pct(void)
{
    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 100;
    ERNode* root = mk(rp, NULL);

    ERRect ra;
    ERProps ap = props_default();
    ap.position = ER_POS_ABSOLUTE;
    ap.left = 10;
    ap.top = 5;
    ap.width_pct = 50.0f;
    ap.height_pct = 25.0f;
    ERNode* abs = mk(ap, &ra);

    er_tree_append_child(root, abs);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-pct", "abs", EXPECT, ra, 10, 5, 100, 25);

    kill_child(root, abs);
    er_node_destroy(root);
}

/** @brief aspectRatio fills an absolute's auto axis from the resolved one: width 100, ratio 2 → height 50. */
static void fixture_abs_aspect_ratio(void)
{
    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 200;
    ERNode* root = mk(rp, NULL);

    ERRect ra;
    ERProps ap = props_default();
    ap.position = ER_POS_ABSOLUTE;
    ap.left = 0;
    ap.top = 0;
    ap.width = 100;
    ap.aspect_ratio = 2.0f;
    ERNode* abs = mk(ap, &ra);

    er_tree_append_child(root, abs);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-aspect-ratio", "abs", EXPECT, ra, 0, 0, 100, 50);

    kill_child(root, abs);
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
    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 200;
    ERNode* root = mk(rp, NULL);

    ERRect ra;
    ERProps ap = props_default();
    ap.position = ER_POS_ABSOLUTE;
    ap.left = 0;
    ap.top = 0;
    ap.aspect_ratio = 2.0f;
    ERNode* abs = mk(ap, &ra);

    ERProps kp = props_default();
    kp.width = 60;
    kp.height = 25;
    ERNode* kid = mk(kp, NULL);

    er_tree_append_child(abs, kid);
    er_tree_append_child(root, abs);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-aspect-both-auto", "abs", EXPECT, ra, 0, 0, 60, 25);

    kill_child(abs, kid);
    kill_child(root, abs);
    er_node_destroy(root);
}

/** @brief Content sizing is max-content, not "at most the parent": a 40x120 child in a 100x50 root overflows. */
static void fixture_abs_content_overflows(void)
{
    ERProps rp = props_default();
    rp.width = 100;
    rp.height = 50;
    ERNode* root = mk(rp, NULL);

    ERRect ra;
    ERProps ap = props_default();
    ap.position = ER_POS_ABSOLUTE;
    ap.left = 0;
    ap.top = 0;
    ERNode* abs = mk(ap, &ra);

    ERProps kp = props_default();
    kp.width = 40;
    kp.height = 120;
    ERNode* kid = mk(kp, NULL);

    er_tree_append_child(abs, kid);
    er_tree_append_child(root, abs);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-overflow", "abs", EXPECT, ra, 0, 0, 40, 120);

    kill_child(abs, kid);
    kill_child(root, abs);
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
    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 200;
    rp.padding = 20;
    ERNode* root = mk(rp, NULL);

    ERRect ra;
    ERProps ap = props_default();
    ap.position = ER_POS_ABSOLUTE;
    ap.left = 0;
    ap.top = 0;
    ap.width_pct = 50.0f;
    ERNode* abs = mk(ap, &ra);

    ERProps kp = props_default();
    kp.width = 30;
    kp.height = 25;
    ERNode* kid = mk(kp, NULL);

    er_tree_append_child(abs, kid);
    er_tree_append_child(root, abs);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-containing-block", "abs", EXPECT, ra, 0, 0, 100, 25);

    kill_child(abs, kid);
    kill_child(root, abs);
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
    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 200;
    rp.padding_left = 10;
    rp.padding_top = 5;
    rp.padding_right = 30;
    rp.padding_bottom = 15;
    ERNode* root = mk(rp, NULL);

    ERRect ra, rb, rc;
    ERProps ap = props_default();
    ap.position = ER_POS_ABSOLUTE;
    ap.left = 0;
    ap.top = 0;
    ap.width = 20;
    ap.height = 20;
    ERNode* a = mk(ap, &ra);

    ERProps bp = props_default();
    bp.position = ER_POS_ABSOLUTE;
    bp.right = 0;
    bp.bottom = 0;
    bp.width = 20;
    bp.height = 20;
    ERNode* b = mk(bp, &rb);

    ERProps cp = props_default();
    cp.position = ER_POS_ABSOLUTE;
    cp.width_pct = 50.0f;
    cp.height_pct = 50.0f;
    ERNode* c = mk(cp, &rc);

    er_tree_append_child(root, a);
    er_tree_append_child(root, b);
    er_tree_append_child(root, c);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-padding-edges", "left/top", EXPECT, ra, 0, 0, 20, 20);
    pcheck("abs-padding-edges", "rt/bot", EXPECT, rb, 180, 180, 20, 20);
    pcheck("abs-padding-edges", "pct", EXPECT, rc, 10, 5, 100, 100);

    kill_child(root, a);
    kill_child(root, b);
    kill_child(root, c);
    er_node_destroy(root);
}

/** @brief Opposing insets size against the padding box too: 200 - 10 - 30 = 160 wide, x = 10. */
static void fixture_abs_inset_pair_padding(void)
{
    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 200;
    rp.padding = 20;
    ERNode* root = mk(rp, NULL);

    ERRect ra;
    ERProps ap = props_default();
    ap.position = ER_POS_ABSOLUTE;
    ap.left = 10;
    ap.right = 30;
    ap.top = 0;
    ap.bottom = 0;
    ERNode* abs = mk(ap, &ra);

    er_tree_append_child(root, abs);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-inset-pair-pad", "abs", EXPECT, ra, 10, 0, 160, 200);

    kill_child(root, abs);
    er_node_destroy(root);
}

/** @brief Margin still shifts an inset-positioned absolute, measured from the padding edge: 0 + 10 + 5. */
static void fixture_abs_margin_inset_padding(void)
{
    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 200;
    rp.padding = 20;
    ERNode* root = mk(rp, NULL);

    ERRect ra;
    ERProps ap = props_default();
    ap.position = ER_POS_ABSOLUTE;
    ap.left = 10;
    ap.top = 10;
    ap.width = 50;
    ap.height = 40;
    ap.margin = 5;
    ERNode* abs = mk(ap, &ra);

    er_tree_append_child(root, abs);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-margin-inset-pad", "abs", EXPECT, ra, 15, 15, 50, 40);

    kill_child(root, abs);
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
    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 200;
    rp.padding = 20;
    ERNode* root = mk(rp, NULL);

    ERRect ra;
    ERProps ap = props_default();
    ap.position = ER_POS_ABSOLUTE;
    ap.width = 50;
    ap.height = 40;
    ERNode* abs = mk(ap, &ra);

    er_tree_append_child(root, abs);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-static-pos-pad", "abs", EXPECT, ra, 20, 20, 50, 40);

    kill_child(root, abs);
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
    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 200;
    rp.padding = 20;
    rp.flex_direction = ER_FLEX_COL;
    rp.justify_content = ER_JUSTIFY_CENTER;
    rp.align_items = ER_ALIGN_CENTER;
    ERNode* root = mk(rp, NULL);

    ERRect ra;
    ERProps ap = props_default();
    ap.position = ER_POS_ABSOLUTE;
    ap.width = 50;
    ap.height = 40;
    ERNode* abs = mk(ap, &ra);

    er_tree_append_child(root, abs);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-static-pos-align", "abs", EXPECT, ra, 75, 80, 50, 40);

    kill_child(root, abs);
    er_node_destroy(root);
}

/**
 * @brief Builds a padded parent holding one uninset absolute and checks where it lands.
 *
 * The static-position fixtures below differ only in a couple of style fields, so they share this
 * builder rather than repeating the same twelve lines. Parent is always 200x200 with padding 20, so
 * the content box is (20,20,160,160); the child is 50x40 unless auto_size asks for a measured one.
 *
 * @param[in] lbl        Assertion label.
 * @param[in] st         EXPECT or XFAIL.
 * @param[in] rp         Parent props (already carrying the direction / justify / align under test).
 * @param[in] ap         Absolute child props (position and size are filled in here).
 * @param[in] auto_size  true to leave width/height auto and give the child a 24x18 child to measure.
 * @param[in] x,y,w,h    Expected (Yoga) rect.
 */
static void
static_pos_case(const char* lbl, ParityStatus st, ERProps rp, ERProps ap, bool auto_size, int x, int y, int w, int h)
{
    rp.width = 200;
    rp.height = 200;
    rp.padding = 20;
    ERNode* root = mk(rp, NULL);

    ERRect ra;
    ap.position = ER_POS_ABSOLUTE;
    if (!auto_size)
    {
        ap.width = 50;
        ap.height = 40;
    }
    ERNode* abs = mk(ap, &ra);

    ERNode* kid = NULL;
    if (auto_size)
    {
        ERProps kp = props_default();
        kp.width = 24;
        kp.height = 18;
        kid = mk(kp, NULL);
        er_tree_append_child(abs, kid);
    }

    er_tree_append_child(root, abs);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-static", lbl, st, ra, x, y, w, h);

    if (kid)
        kill_child(abs, kid);
    kill_child(root, abs);
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
    ERProps rp = props_default();
    rp.flex_direction = ER_FLEX_COL;
    ERProps ap = props_default();

    rp.justify_content = ER_JUSTIFY_FLEX_START;
    static_pos_case("just-start", EXPECT, rp, ap, false, 20, 20, 50, 40);
    rp.justify_content = ER_JUSTIFY_CENTER;
    static_pos_case("just-center", EXPECT, rp, ap, false, 20, 80, 50, 40);
    rp.justify_content = ER_JUSTIFY_FLEX_END;
    static_pos_case("just-end", EXPECT, rp, ap, false, 20, 140, 50, 40);
    rp.justify_content = ER_JUSTIFY_SPACE_BETWEEN;
    static_pos_case("just-between", EXPECT, rp, ap, false, 20, 20, 50, 40);
    rp.justify_content = ER_JUSTIFY_SPACE_AROUND;
    static_pos_case("just-around", EXPECT, rp, ap, false, 20, 80, 50, 40);
    rp.justify_content = ER_JUSTIFY_SPACE_EVENLY;
    static_pos_case("just-evenly", EXPECT, rp, ap, false, 20, 80, 50, 40);

    /* Row swaps the axes: the same modes now move x instead of y. */
    rp.flex_direction = ER_FLEX_ROW;
    rp.justify_content = ER_JUSTIFY_CENTER;
    static_pos_case("just-center-row", EXPECT, rp, ap, false, 75, 20, 50, 40);
    rp.justify_content = ER_JUSTIFY_FLEX_END;
    static_pos_case("just-end-row", EXPECT, rp, ap, false, 130, 20, 50, 40);
}

/** @brief alignItems places the uninset absolute on the cross axis; alignSelf overrides it. */
static void fixture_abs_static_align(void)
{
    ERProps rp = props_default();
    rp.flex_direction = ER_FLEX_COL;
    ERProps ap = props_default();

    rp.align_items = ER_ALIGN_FLEX_START;
    static_pos_case("align-start", EXPECT, rp, ap, false, 20, 20, 50, 40);
    rp.align_items = ER_ALIGN_CENTER;
    static_pos_case("align-center", EXPECT, rp, ap, false, 75, 20, 50, 40);
    rp.align_items = ER_ALIGN_FLEX_END;
    static_pos_case("align-end", EXPECT, rp, ap, false, 130, 20, 50, 40);

    /* Row swaps the axes: alignItems now moves y. */
    rp.flex_direction = ER_FLEX_ROW;
    rp.align_items = ER_ALIGN_CENTER;
    static_pos_case("align-center-row", EXPECT, rp, ap, false, 20, 80, 50, 40);

    /* alignSelf wins over the parent's alignItems, exactly as it does for a flow child. */
    rp.flex_direction = ER_FLEX_COL;
    rp.align_items = ER_ALIGN_CENTER;
    ap.align_self = ER_ALIGN_FLEX_END;
    static_pos_case("align-self-end", EXPECT, rp, ap, false, 130, 20, 50, 40);
    ap.align_self = ER_ALIGN_FLEX_START;
    static_pos_case("align-self-start", EXPECT, rp, ap, false, 20, 20, 50, 40);
}

/**
 * @brief alignItems 'stretch' does NOT stretch an absolute — it lands at the cross start, content-sized.
 *
 * Stretch is the DEFAULT alignItems, so this is the case that matters most: an auto-sized absolute in
 * an ordinary parent keeps its measured 24x18 rather than being grown to the 160px content width.
 */
static void fixture_abs_static_stretch(void)
{
    ERProps rp = props_default();
    rp.flex_direction = ER_FLEX_COL;
    rp.align_items = ER_ALIGN_STRETCH;
    ERProps ap = props_default();
    static_pos_case("stretch-auto", EXPECT, rp, ap, true, 20, 20, 24, 18);

    /* Same for an explicitly sized child: stretch places it like flex-start. */
    static_pos_case("stretch-sized", EXPECT, rp, ap, false, 20, 20, 50, 40);
}

/**
 * @brief A reversed axis measures the static position from the far edge, using the TRAILING margin.
 *
 * column-reverse turns flex-start into "against the bottom", and the margin that holds the child off
 * that edge is marginBottom, not marginTop. wrap-reverse does the same to the cross axis.
 */
static void fixture_abs_static_reverse(void)
{
    ERProps rp = props_default();
    ERProps ap = props_default();

    rp.flex_direction = ER_FLEX_COL_REVERSE;
    static_pos_case("rev-col-start", EXPECT, rp, ap, false, 20, 140, 50, 40);
    rp.justify_content = ER_JUSTIFY_FLEX_END;
    static_pos_case("rev-col-end", EXPECT, rp, ap, false, 20, 20, 50, 40);

    rp = props_default();
    rp.flex_direction = ER_FLEX_ROW_REVERSE;
    static_pos_case("rev-row-start", EXPECT, rp, ap, false, 130, 20, 50, 40);
    rp.align_items = ER_ALIGN_FLEX_END;
    static_pos_case("rev-row-align-end", EXPECT, rp, ap, false, 130, 140, 50, 40);

    /* Reversed main axis + margins: marginBottom is the one that leads. */
    rp = props_default();
    rp.flex_direction = ER_FLEX_COL_REVERSE;
    ap.margin_top = 7;
    ap.margin_bottom = 3;
    static_pos_case("rev-col-margin", EXPECT, rp, ap, false, 20, 137, 50, 40);
    rp.justify_content = ER_JUSTIFY_CENTER;
    static_pos_case("rev-col-margin-ctr", EXPECT, rp, ap, false, 20, 82, 50, 40);

    /* wrap-reverse mirrors the CROSS axis, against the full content extent. */
    rp = props_default();
    rp.flex_direction = ER_FLEX_COL;
    rp.flex_wrap = ER_WRAP_WRAP_REVERSE;
    rp.align_items = ER_ALIGN_FLEX_START;
    ap = props_default();
    static_pos_case("wrap-rev-start", EXPECT, rp, ap, false, 130, 20, 50, 40);
    rp.align_items = ER_ALIGN_FLEX_END;
    static_pos_case("wrap-rev-end", EXPECT, rp, ap, false, 20, 20, 50, 40);
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
    ERProps rp = props_default();
    rp.flex_direction = ER_FLEX_COL;
    rp.justify_content = ER_JUSTIFY_CENTER;
    rp.align_items = ER_ALIGN_CENTER;

    ERProps ap = props_default();
    ap.margin_left = 5;
    ap.margin_right = 25;
    static_pos_case("margin-ctr-cross", EXPECT, rp, ap, false, 65, 80, 50, 40);

    ap = props_default();
    ap.margin_top = 5;
    ap.margin_bottom = 25;
    static_pos_case("margin-ctr-main", EXPECT, rp, ap, false, 75, 70, 50, 40);

    rp.justify_content = ER_JUSTIFY_FLEX_START;
    rp.align_items = ER_ALIGN_FLEX_START;
    ap = props_default();
    ap.margin_left = 5;
    ap.margin_top = 9;
    static_pos_case("margin-start", EXPECT, rp, ap, false, 25, 29, 50, 40);

    /* flex-start ignores the trailing margins entirely. */
    ap = props_default();
    ap.margin_right = 99;
    ap.margin_bottom = 99;
    static_pos_case("margin-start-unused", EXPECT, rp, ap, false, 20, 20, 50, 40);

    rp.justify_content = ER_JUSTIFY_FLEX_END;
    rp.align_items = ER_ALIGN_FLEX_END;
    ap = props_default();
    ap.margin_right = 5;
    ap.margin_bottom = 9;
    static_pos_case("margin-end", EXPECT, rp, ap, false, 125, 131, 50, 40);
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
    ERProps rp = props_default();
    rp.flex_direction = ER_FLEX_COL;
    rp.justify_content = ER_JUSTIFY_CENTER;
    rp.align_items = ER_ALIGN_CENTER;

    ERProps ap = props_default();
    ap.width = 200;
    ap.height = 200;

    rp.width = 200;
    rp.height = 200;
    rp.padding = 20;
    ERNode* root = mk(rp, NULL);

    ERRect ra;
    ap.position = ER_POS_ABSOLUTE;
    ERNode* abs = mk(ap, &ra);

    er_tree_append_child(root, abs);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-static", "overflow-ctr", EXPECT, ra, 0, 0, 200, 200);

    kill_child(root, abs);
    er_node_destroy(root);
}

/**
 * @brief Insets and static positions resolve PER AXIS: `left` with no `top` pins x and aligns y.
 */
static void fixture_abs_static_per_axis(void)
{
    ERProps rp = props_default();
    rp.flex_direction = ER_FLEX_COL;
    rp.justify_content = ER_JUSTIFY_CENTER;
    rp.align_items = ER_ALIGN_CENTER;

    ERProps ap = props_default();
    ap.left = 7;
    static_pos_case("axis-left-only", EXPECT, rp, ap, false, 7, 80, 50, 40);

    ap = props_default();
    ap.top = 7;
    static_pos_case("axis-top-only", EXPECT, rp, ap, false, 75, 7, 50, 40);
}

/**
 * @brief Lays out two asymmetric-margin children on a reversed main axis and checks both rects.
 *
 * The container is the issue's shape: 200x200 with padding 20, so the content box is
 * (20, 20, 160, 160). Child A is 40 long with main-axis margins 7 (start) / 3 (end); child B is 30
 * long with 1 / 5. The margins are deliberately unequal — equal ones cancel out under the mirror and
 * hide the bug entirely. Both children are 50 on the cross axis so alignItems never enters into it.
 *
 * @param[in] name     Fixture name for the output.
 * @param[in] dir      ER_FLEX_COL_REVERSE or ER_FLEX_ROW_REVERSE.
 * @param[in] justify  justifyContent mode.
 * @param[in] gap      Main-axis gap, or 0 for none.
 * @param[in] ax       Expected x of child A.
 * @param[in] ay       Expected y of child A.
 * @param[in] bx       Expected x of child B.
 * @param[in] by       Expected y of child B.
 */
static void rev_pair_case(const char* name, uint8_t dir, uint8_t justify, int16_t gap, int ax, int ay, int bx, int by)
{
    const bool is_row = (dir == ER_FLEX_ROW_REVERSE);

    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 200;
    rp.padding = 20;
    rp.flex_direction = dir;
    rp.align_items = ER_ALIGN_FLEX_START;
    rp.justify_content = justify;
    if (gap)
        rp.gap = gap;
    ERNode* root = mk(rp, NULL);

    ERRect ra, rb;
    ERProps ap = props_default();
    ap.width = is_row ? 40 : 50;
    ap.height = is_row ? 50 : 40;
    ap.margin_left = is_row ? 7 : ER_LAYOUT_AUTO;
    ap.margin_right = is_row ? 3 : ER_LAYOUT_AUTO;
    ap.margin_top = is_row ? ER_LAYOUT_AUTO : 7;
    ap.margin_bottom = is_row ? ER_LAYOUT_AUTO : 3;
    ERNode* a = mk(ap, &ra);

    ERProps bp = props_default();
    bp.width = is_row ? 30 : 50;
    bp.height = is_row ? 50 : 30;
    bp.margin_left = is_row ? 1 : ER_LAYOUT_AUTO;
    bp.margin_right = is_row ? 5 : ER_LAYOUT_AUTO;
    bp.margin_top = is_row ? ER_LAYOUT_AUTO : 1;
    bp.margin_bottom = is_row ? ER_LAYOUT_AUTO : 5;
    ERNode* b = mk(bp, &rb);

    er_tree_append_child(root, a);
    er_tree_append_child(root, b);
    er_tree_set_root(root);
    er_commit();

    pcheck(name, "a", EXPECT, ra, ax, ay, ap.width, ap.height);
    pcheck(name, "b", EXPECT, rb, bx, by, bp.width, bp.height);

    kill_child(root, a);
    kill_child(root, b);
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
    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 200;
    rp.padding = 20;
    rp.flex_direction = ER_FLEX_COL_REVERSE;
    rp.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(rp, NULL);

    ERRect rc;
    ERProps cp = props_default();
    cp.width = 50;
    cp.height = 40;
    cp.margin_top = 7;
    cp.margin_bottom = 3;
    ERNode* c = mk(cp, &rc);

    er_tree_append_child(root, c);
    er_tree_set_root(root);
    er_commit();
    pcheck("flow-reverse-margin", "c", EXPECT, rc, 20, 137, 50, 40);

    kill_child(root, c);
    er_node_destroy(root);

    /* Overflow: a child taller than the content box hangs off the reversed start edge, keeping its
     * 3px marginBottom against the bottom. 20 + 160 - 3 - 200 = -23. */
    rp = props_default();
    rp.width = 200;
    rp.height = 200;
    rp.padding = 20;
    rp.flex_direction = ER_FLEX_COL_REVERSE;
    rp.align_items = ER_ALIGN_FLEX_START;
    root = mk(rp, NULL);

    cp.height = 200;
    c = mk(cp, &rc);
    er_tree_append_child(root, c);
    er_tree_set_root(root);
    er_commit();
    pcheck("flow-reverse-overflow", "c", EXPECT, rc, 20, -23, 50, 200);

    kill_child(root, c);
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
static void build_wrap_rev(int n,
                           uint8_t align_content,
                           int16_t column_gap,
                           int16_t item_h,
                           ERNode** root_out,
                           ERNode** kids_out,
                           ERRect* rects)
{
    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 200;
    rp.padding = 20;
    rp.flex_direction = ER_FLEX_COL;
    rp.flex_wrap = ER_WRAP_WRAP_REVERSE;
    rp.align_items = ER_ALIGN_FLEX_START;
    rp.align_content = align_content;
    rp.column_gap = column_gap;
    ERNode* root = mk(rp, NULL);

    for (int i = 0; i < n; i++)
    {
        ERProps cp = props_default();
        cp.width = 50;
        cp.height = item_h;
        kids_out[i] = mk(cp, &rects[i]);
        er_tree_append_child(root, kids_out[i]);
    }
    er_tree_set_root(root);
    *root_out = root;
}

/** @brief Tears down a wrap_rev fixture. */
static void teardown_wrap_rev(int n, ERNode* root, ERNode** kids)
{
    for (int i = 0; i < n; i++)
    {
        kill_child(root, kids[i]);
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
    ERRect r[5];

    /* One line: 160 content - 50 line = 110 of leftover cross space to travel. */
    build_wrap_rev(1, ER_ALIGN_CONTENT_FLEX_START, 0, 40, &root, kids, r);
    er_commit();
    pcheck("wrap-rev-1line", "c", EXPECT, r[0], 130, 20, 50, 40);
    teardown_wrap_rev(1, root, kids);

    /* Two lines: first line at the far edge, the second one line-width in from it. */
    build_wrap_rev(5, ER_ALIGN_CONTENT_FLEX_START, 0, 40, &root, kids, r);
    er_commit();
    pcheck("wrap-rev-2line", "l0i0", EXPECT, r[0], 130, 20, 50, 40);
    pcheck("wrap-rev-2line", "l0i3", EXPECT, r[3], 130, 140, 50, 40);
    pcheck("wrap-rev-2line", "l1i0", EXPECT, r[4], 80, 20, 50, 40);
    teardown_wrap_rev(5, root, kids);

    /* A cross-axis gap widens the step between the mirrored lines, but not the anchor. */
    build_wrap_rev(5, ER_ALIGN_CONTENT_FLEX_START, 10, 40, &root, kids, r);
    er_commit();
    pcheck("wrap-rev-gap", "l0i0", EXPECT, r[0], 130, 20, 50, 40);
    pcheck("wrap-rev-gap", "l1i0", EXPECT, r[4], 70, 20, 50, 40);
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
    ERRect r[5];
    /* Two lines of 50 in a 160 content box -> 60px of free cross space to distribute. */
    static const struct
    {
        const char* name;
        uint8_t mode;
        int16_t x0; /**< First line (4 items). */
        int16_t x1; /**< Second line (1 item). */
    } k_cases[] = {
        {"ac-flex-end", ER_ALIGN_CONTENT_FLEX_END, 70, 20},
        {"ac-center", ER_ALIGN_CONTENT_CENTER, 100, 50},
        {"ac-between", ER_ALIGN_CONTENT_SPACE_BETWEEN, 130, 20},
        {"ac-around", ER_ALIGN_CONTENT_SPACE_AROUND, 115, 35},
        {"ac-stretch", ER_ALIGN_CONTENT_STRETCH, 130, 50},
    };

    for (size_t c = 0; c < sizeof(k_cases) / sizeof(k_cases[0]); c++)
    {
        build_wrap_rev(5, k_cases[c].mode, 0, 40, &root, kids, r);
        er_commit();
        pcheck(k_cases[c].name, "l0", EXPECT, r[0], k_cases[c].x0, 20, 50, 40);
        pcheck(k_cases[c].name, "l1", EXPECT, r[4], k_cases[c].x1, 20, 50, 40);
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
    ERRect r[8];

    build_wrap_rev(8, ER_ALIGN_CONTENT_FLEX_START, 0, 80, &root, kids, r);
    er_commit();
    pcheck("wrap-rev-overflow", "l0", EXPECT, r[0], 130, 20, 50, 80);
    pcheck("wrap-rev-overflow", "l1", EXPECT, r[2], 80, 20, 50, 80);
    pcheck("wrap-rev-overflow", "l2", EXPECT, r[4], 30, 20, 50, 80);
    pcheck("wrap-rev-overflow", "l3", EXPECT, r[6], -20, 20, 50, 80);
    teardown_wrap_rev(8, root, kids);
}

/** @brief In-flow siblings never move the static position — it is the SOLE-item spot, not a slot. */
static void fixture_abs_static_ignores_siblings(void)
{
    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 200;
    rp.padding = 20;
    rp.flex_direction = ER_FLEX_COL;
    rp.justify_content = ER_JUSTIFY_CENTER;
    rp.align_items = ER_ALIGN_CENTER;
    ERNode* root = mk(rp, NULL);

    ERProps sp = props_default();
    sp.width = 30;
    sp.height = 30;
    ERNode* s0 = mk(sp, NULL);
    ERNode* s1 = mk(sp, NULL);
    ERNode* s2 = mk(sp, NULL);

    ERRect ra;
    ERProps ap = props_default();
    ap.position = ER_POS_ABSOLUTE;
    ap.width = 50;
    ap.height = 40;
    ERNode* abs = mk(ap, &ra);

    er_tree_append_child(root, s0);
    er_tree_append_child(root, s1);
    er_tree_append_child(root, s2);
    er_tree_append_child(root, abs);
    er_tree_set_root(root);
    er_commit();
    pcheck("abs-static", "siblings", EXPECT, ra, 75, 80, 50, 40);

    kill_child(root, s0);
    kill_child(root, s1);
    kill_child(root, s2);
    kill_child(root, abs);
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
    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 200;
    rp.padding = 20;
    rp.justify_content = ER_JUSTIFY_CENTER;
    rp.align_items = ER_ALIGN_CENTER;

    ERProps cp = props_default();
    cp.width = 51;
    cp.height = 41;

    static const struct
    {
        const char* name;
        uint8_t dir;
        int16_t w, h; /**< Child size. */
        int16_t x, y; /**< Expected origin. */
    } k_cases[] = {
        /* The issue's exact repro, then the same thing with the axes swapped. */
        {"half-center-col", ER_FLEX_COL, 51, 41, 75, 80},
        {"half-center-row", ER_FLEX_ROW, 51, 41, 75, 80},
        /* A reversed axis measures from the far edge but must still round to the same pixel. */
        {"half-center-colrev", ER_FLEX_COL_REVERSE, 51, 41, 75, 80},
        {"half-center-rowrev", ER_FLEX_ROW_REVERSE, 51, 41, 75, 80},
    };

    for (size_t c = 0; c < sizeof(k_cases) / sizeof(k_cases[0]); c++)
    {
        rp.flex_direction = k_cases[c].dir;
        cp.width = k_cases[c].w;
        cp.height = k_cases[c].h;

        ERNode* root = mk(rp, NULL);
        ERRect rc;
        ERNode* c0 = mk(cp, &rc);
        er_tree_append_child(root, c0);
        er_tree_set_root(root);
        er_commit();
        pcheck(k_cases[c].name, "c", EXPECT, rc, k_cases[c].x, k_cases[c].y, k_cases[c].w, k_cases[c].h);
        kill_child(root, c0);
        er_node_destroy(root);

        /* An uninset ABSOLUTE child takes the same static position, so it must round the same way. */
        rp.flex_direction = k_cases[c].dir;
        ERProps abp = cp;
        abp.position = ER_POS_ABSOLUTE;
        root = mk(rp, NULL);
        ERNode* a0 = mk(abp, &rc);
        er_tree_append_child(root, a0);
        er_tree_set_root(root);
        er_commit();
        pcheck(k_cases[c].name, "abs", EXPECT, rc, k_cases[c].x, k_cases[c].y, k_cases[c].w, k_cases[c].h);
        kill_child(root, a0);
        er_node_destroy(root);
    }

    /* A 201x171 child centred in a 160x160 content box overhangs both edges evenly: -20.5 rounds up
     * to -20, same rule on both axes. Flow and absolute have to agree — the same child placed either
     * way lands in the same place. */
    rp.flex_direction = ER_FLEX_COL;
    cp.width = 201;
    cp.height = 171;

    ERProps ovp = cp;
    ovp.position = ER_POS_ABSOLUTE;
    ERNode* root = mk(rp, NULL);
    ERRect ro;
    ERNode* ov = mk(ovp, &ro);
    er_tree_append_child(root, ov);
    er_tree_set_root(root);
    er_commit();
    pcheck("half-overhang", "abs", EXPECT, ro, 0, 15, 201, 171);
    kill_child(root, ov);
    er_node_destroy(root);

    root = mk(rp, NULL);
    ERNode* fl = mk(cp, &ro);
    er_tree_append_child(root, fl);
    er_tree_set_root(root);
    er_commit();
    pcheck("half-overhang", "flow", EXPECT, ro, 0, 15, 201, 171);
    kill_child(root, fl);
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
 * @param[in] name     Assertion label.
 * @param[in] dir      Any ERFlexDirection.
 * @param[in] justify  justifyContent under test.
 * @param[in] gap      Main-axis gap, or 0 for none.
 * @param[in] n        Child count (<= 8).
 * @param[in] item     Child main-axis size, per child.
 * @param[in] want     Expected main-axis position per child, in tree order.
 */
static void stack_sizes_case(
    const char* name, uint8_t dir, uint8_t justify, int16_t gap, int n, const int16_t* item, const int16_t* want)
{
    const bool is_row = (dir == ER_FLEX_ROW || dir == ER_FLEX_ROW_REVERSE);

    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 200;
    rp.padding = 20;
    rp.flex_direction = dir;
    rp.justify_content = justify;
    rp.align_items = ER_ALIGN_FLEX_START;
    if (gap)
        rp.gap = gap;
    ERNode* root = mk(rp, NULL);

    ERNode* kids[8];
    ERRect r[8];
    for (int i = 0; i < n; i++)
    {
        ERProps cp = props_default();
        cp.width = is_row ? item[i] : 50;
        cp.height = is_row ? 50 : item[i];
        kids[i] = mk(cp, &r[i]);
        er_tree_append_child(root, kids[i]);
    }
    er_tree_set_root(root);
    er_commit();

    for (int i = 0; i < n; i++)
    {
        char lbl[8];
        snprintf(lbl, sizeof lbl, "i%d", i);
        pcheck(name,
               lbl,
               EXPECT,
               r[i],
               is_row ? want[i] : 20,
               is_row ? 20 : want[i],
               is_row ? item[i] : 50,
               is_row ? 50 : item[i]);
    }
    for (int i = 0; i < n; i++)
        kill_child(root, kids[i]);
    er_node_destroy(root);
}

/**
 * @brief stack_sizes_case() with `n` identically sized children and no gap.
 *
 * @param[in] name     Assertion label.
 * @param[in] dir      ER_FLEX_COL or ER_FLEX_COL_REVERSE.
 * @param[in] justify  justifyContent under test.
 * @param[in] n        Child count (<= 8).
 * @param[in] item     Child main-axis size.
 * @param[in] want_y   Expected y per child, in tree order.
 */
static void stack_case(const char* name, uint8_t dir, uint8_t justify, int n, int16_t item, const int16_t* want_y)
{
    int16_t sizes[8];
    for (int i = 0; i < n; i++)
        sizes[i] = item;
    stack_sizes_case(name, dir, justify, 0, n, sizes, want_y);
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
    static const int16_t k_h[3] = {100, 90, 171};

    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 200;
    rp.padding = 20;
    rp.flex_direction = ER_FLEX_COL;
    rp.flex_wrap = ER_WRAP_WRAP;
    rp.align_content = ER_ALIGN_CONTENT_FLEX_START;
    rp.align_items = ER_ALIGN_FLEX_START;
    rp.justify_content = justify;
    ERNode* root = mk(rp, NULL);

    ERNode* kids[3];
    ERRect r[3];
    for (int i = 0; i < 3; i++)
    {
        ERProps cp = props_default();
        cp.width = 40;
        cp.height = k_h[i];
        kids[i] = mk(cp, &r[i]);
        er_tree_append_child(root, kids[i]);
    }
    er_tree_set_root(root);
    er_commit();

    for (int i = 0; i < 3; i++)
    {
        char lbl[8];
        snprintf(lbl, sizeof lbl, "l%d", i);
        pcheck(name, lbl, EXPECT, r[i], (int16_t)(20 + 40 * i), want_y[i], 40, k_h[i]);
    }
    for (int i = 0; i < 3; i++)
        kill_child(root, kids[i]);
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
 * @param[in] item_w         Child cross-axis size.
 * @param[in] want_x         Expected x for lines 0, 1, 2.
 */
static void wrap_line_case(
    const char* name, uint8_t wrap, uint8_t align_content, uint8_t align_items, int16_t item_w, const int16_t* want_x)
{
    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 200;
    rp.padding = 20;
    rp.flex_direction = ER_FLEX_COL;
    rp.flex_wrap = wrap;
    rp.align_content = align_content;
    rp.align_items = align_items;
    ERNode* root = mk(rp, NULL);

    ERNode* kids[9];
    ERRect r[9];
    for (int i = 0; i < 9; i++)
    {
        ERProps cp = props_default();
        cp.width = item_w;
        cp.height = 40;
        kids[i] = mk(cp, &r[i]);
        er_tree_append_child(root, kids[i]);
    }
    er_tree_set_root(root);
    er_commit();

    pcheck(name, "l0", EXPECT, r[0], want_x[0], 20, item_w, 40);
    pcheck(name, "l1", EXPECT, r[4], want_x[1], 20, item_w, 40);
    pcheck(name, "l2", EXPECT, r[8], want_x[2], 20, item_w, 40);

    for (int i = 0; i < 9; i++)
        kill_child(root, kids[i]);
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
    static const struct
    {
        const char* name;
        uint8_t wrap;
        uint8_t align_content;
        uint8_t align_items;
        int16_t item_w;
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

    for (size_t c = 0; c < sizeof(k_cases) / sizeof(k_cases[0]); c++)
        wrap_line_case(k_cases[c].name,
                       k_cases[c].wrap,
                       k_cases[c].align_content,
                       k_cases[c].align_items,
                       k_cases[c].item_w,
                       k_cases[c].want_x);

    /* A single line with no wrap fills the cross axis, so alignItems centre halves 160 - 21 = 139 and
     * has to round 69.5 up. The plainest form of the whole fixture. */
    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 200;
    rp.padding = 20;
    rp.flex_direction = ER_FLEX_COL;
    rp.align_items = ER_ALIGN_CENTER;
    ERNode* root = mk(rp, NULL);
    ERRect rc;
    ERProps cp = props_default();
    cp.width = 21;
    cp.height = 40;
    ERNode* c0 = mk(cp, &rc);
    er_tree_append_child(root, c0);
    er_tree_set_root(root);
    er_commit();
    pcheck("half-ai-nowrap", "c", EXPECT, rc, 90, 20, 21, 40);
    kill_child(root, c0);
    er_node_destroy(root);
}

/** @brief One child's main-axis flex inputs for flex_case(). A 0 min/max means "no bound". */
typedef struct
{
    int16_t basis;  /**< flex_basis: the base main size before grow/shrink. */
    int16_t grow;   /**< flexGrow. */
    int16_t shrink; /**< flexShrink. */
    int16_t bmin;   /**< Min main size, 0 for none. */
    int16_t bmax;   /**< Max main size, 0 for none. */
} FlexKid;

/**
 * @brief A single flex line, checking every child's main-axis position AND size.
 *
 * The cross axis is a fixed 40px the children stretch into, so the only interesting numbers are on
 * the main axis. Sizes come off the same pixel grid as positions -- a child spans from one rounded
 * edge to the next -- so a share that does not divide evenly lands on the child straddling the half
 * pixel rather than being dropped at the end of the line.
 *
 * @param[in] name       Assertion label.
 * @param[in] dir        flexDirection under test.
 * @param[in] box        Container size on the main axis, padding included.
 * @param[in] pad        Padding on all four edges.
 * @param[in] gap        Gap between children.
 * @param[in] n          Child count (<= 7).
 * @param[in] kid        Per-child flex inputs, in tree order.
 * @param[in] want_pos   Expected main-axis position per child.
 * @param[in] want_size  Expected main-axis size per child.
 */
static void flex_case(const char* name,
                      uint8_t dir,
                      int16_t box,
                      int16_t pad,
                      int16_t gap,
                      int n,
                      const FlexKid* kid,
                      const int16_t* want_pos,
                      const int16_t* want_size)
{
    const bool is_row = (dir == ER_FLEX_ROW || dir == ER_FLEX_ROW_REVERSE);

    ERProps rp = props_default();
    rp.width = is_row ? box : 40;
    rp.height = is_row ? 40 : box;
    rp.padding = pad;
    rp.gap = gap;
    rp.flex_direction = dir;
    ERNode* root = mk(rp, NULL);

    ERNode* kids[7];
    ERRect r[7];
    for (int i = 0; i < n; i++)
    {
        ERProps cp = props_default();
        cp.flex_basis = kid[i].basis;
        cp.flex_grow = kid[i].grow;
        cp.flex_shrink = kid[i].shrink;
        if (kid[i].bmin)
        {
            if (is_row)
                cp.min_width = kid[i].bmin;
            else
                cp.min_height = kid[i].bmin;
        }
        if (kid[i].bmax)
        {
            if (is_row)
                cp.max_width = kid[i].bmax;
            else
                cp.max_height = kid[i].bmax;
        }
        kids[i] = mk(cp, &r[i]);
        er_tree_append_child(root, kids[i]);
    }
    er_tree_set_root(root);
    er_commit();

    /* The cross axis is whatever padding leaves of the 40. */
    const int16_t cp_pos = pad;
    const int16_t cp_size = (int16_t)(40 - 2 * pad);
    for (int i = 0; i < n; i++)
    {
        char lbl[8];
        snprintf(lbl, sizeof(lbl), "c%d", i);
        pcheck(name,
               lbl,
               EXPECT,
               r[i],
               is_row ? want_pos[i] : cp_pos,
               is_row ? cp_pos : want_pos[i],
               is_row ? want_size[i] : cp_size,
               is_row ? cp_size : want_size[i]);
    }

    for (int i = 0; i < n; i++)
        kill_child(root, kids[i]);
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
    static const FlexKid k_g1[] = {{0, 1, 0, 0, 0},
                                   {0, 1, 0, 0, 0},
                                   {0, 1, 0, 0, 0},
                                   {0, 1, 0, 0, 0},
                                   {0, 1, 0, 0, 0},
                                   {0, 1, 0, 0, 0},
                                   {0, 1, 0, 0, 0}};

    /* 100 over three children: 33.33 each, so the middle one straddles the half pixel. */
    static const int16_t k_g3_pos[] = {0, 33, 67};
    static const int16_t k_g3_size[] = {33, 34, 33};
    flex_case("flexsz-grow3", ER_FLEX_ROW, 100, 0, 0, 3, k_g1, k_g3_pos, k_g3_size);
    flex_case("flexsz-col3", ER_FLEX_COL, 100, 0, 0, 3, k_g1, k_g3_pos, k_g3_size);

    /* Padding shifts every edge by a whole pixel, so it cannot change which child takes the leftover;
     * 80 over three is 26.67, and this time it is the OUTER two that round up. */
    static const int16_t k_pad_pos[] = {10, 37, 63};
    static const int16_t k_pad_size[] = {27, 26, 27};
    flex_case("flexsz-pad", ER_FLEX_ROW, 100, 10, 0, 3, k_g1, k_pad_pos, k_pad_size);

    /* A gap is a whole pixel too: 92 over three is 30.67. */
    static const int16_t k_gap_pos[] = {0, 35, 69};
    static const int16_t k_gap_size[] = {31, 30, 31};
    flex_case("flexsz-gap4", ER_FLEX_ROW, 100, 0, 4, 3, k_g1, k_gap_pos, k_gap_size);

    /* 100 over seven: 14.29 each, so the leftover accumulates and two children take a pixel. */
    static const int16_t k_g7_pos[] = {0, 14, 29, 43, 57, 71, 86};
    static const int16_t k_g7_size[] = {14, 15, 14, 14, 14, 15, 14};
    flex_case("flexsz-grow7", ER_FLEX_ROW, 100, 0, 0, 7, k_g1, k_g7_pos, k_g7_size);

    /* Unequal factors, and a non-zero basis the growth is added to. */
    static const FlexKid k_g12[] = {{0, 1, 0, 0, 0}, {0, 2, 0, 0, 0}};
    static const int16_t k_g12_pos[] = {0, 33};
    static const int16_t k_g12_size[] = {33, 67};
    flex_case("flexsz-grow1-2", ER_FLEX_ROW, 100, 0, 0, 2, k_g12, k_g12_pos, k_g12_size);

    static const FlexKid k_basis[] = {{10, 1, 0, 0, 0}, {10, 1, 0, 0, 0}, {10, 1, 0, 0, 0}};
    flex_case("flexsz-basis10", ER_FLEX_ROW, 100, 0, 0, 3, k_basis, k_g3_pos, k_g3_size);

    /* Bounds. A child frozen at its bound leaves a new remainder for the rest to share, and the
     * bound has to be tested against the exact size: 200/3 is 66.67, which a max of 66 clamps even
     * though the truncated 66 would have fitted. */
    static const FlexKid k_max66a[] = {{0, 1, 0, 0, 66}, {0, 1, 0, 0, 0}, {0, 1, 0, 0, 0}};
    static const int16_t k_max66a_pos[] = {0, 66, 133};
    static const int16_t k_max66a_size[] = {66, 67, 67};
    flex_case("flexsz-max66a", ER_FLEX_ROW, 200, 0, 0, 3, k_max66a, k_max66a_pos, k_max66a_size);

    static const FlexKid k_max66b[] = {{0, 1, 0, 0, 0}, {0, 1, 0, 0, 66}, {0, 1, 0, 0, 0}};
    static const int16_t k_max66b_pos[] = {0, 67, 133};
    static const int16_t k_max66b_size[] = {67, 66, 67};
    flex_case("flexsz-max66b", ER_FLEX_ROW, 200, 0, 0, 3, k_max66b, k_max66b_pos, k_max66b_size);

    /* 100 with the first child pinned at 25: the other two share 75, i.e. 37.5 each. */
    static const FlexKid k_max25[] = {{0, 1, 0, 0, 25}, {0, 1, 0, 0, 0}, {0, 1, 0, 0, 0}};
    static const int16_t k_max25_pos[] = {0, 25, 63};
    static const int16_t k_max25_size[] = {25, 38, 37};
    flex_case("flexsz-max25", ER_FLEX_ROW, 100, 0, 0, 3, k_max25, k_max25_pos, k_max25_size);

    /* A min bounds the BASIS before the free space is measured, so 60 (not 100) is shared. */
    static const FlexKid k_min40[] = {{0, 1, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 1, 0, 40, 0}};
    static const int16_t k_min40_pos[] = {0, 20, 40};
    static const int16_t k_min40_size[] = {20, 20, 60};
    flex_case("flexsz-min40", ER_FLEX_ROW, 100, 0, 0, 3, k_min40, k_min40_pos, k_min40_size);

    /* Shrink is the same rule with the free space negative: 120 into 100 is 33.33 each. */
    static const FlexKid k_s40[] = {{40, 0, 1, 0, 0}, {40, 0, 1, 0, 0}, {40, 0, 1, 0, 0}};
    flex_case("flexsz-shrink3", ER_FLEX_ROW, 100, 0, 0, 3, k_s40, k_g3_pos, k_g3_size);

    /* Shrink scales by the base size, so 80 into 60 takes 12.5 off the 50 and 7.5 off the 30. */
    static const FlexKid k_s2[] = {{50, 0, 1, 0, 0}, {30, 0, 1, 0, 0}};
    static const int16_t k_s2_pos[] = {0, 38};
    static const int16_t k_s2_size[] = {38, 22};
    flex_case("flexsz-shrink2", ER_FLEX_ROW, 60, 0, 0, 2, k_s2, k_s2_pos, k_s2_size);

    static const FlexKid k_sun[] = {{50, 0, 1, 0, 0}, {30, 0, 1, 0, 0}, {40, 0, 2, 0, 0}};
    static const int16_t k_sun_pos[] = {0, 44, 70};
    static const int16_t k_sun_size[] = {44, 26, 30};
    flex_case("flexsz-shrink-un", ER_FLEX_ROW, 100, 0, 0, 3, k_sun, k_sun_pos, k_sun_size);

    /* Reversed: the same sizes read from the other end, since neither edge lands on a half pixel. */
    static const int16_t k_rev3_pos[] = {67, 33, 0};
    flex_case("flexsz-rgrow3", ER_FLEX_ROW_REVERSE, 100, 0, 0, 3, k_g1, k_rev3_pos, k_g3_size);
    flex_case("flexsz-rshrink3", ER_FLEX_ROW_REVERSE, 100, 0, 0, 3, k_s40, k_rev3_pos, k_g3_size);

    /* Two children in 5px sit on exactly 2.5, the one case where the direction of the axis decides
     * which of them gets the pixel. */
    static const int16_t k_half_pos[] = {0, 3};
    static const int16_t k_half_size[] = {3, 2};
    static const int16_t k_rhalf_pos[] = {3, 0};
    static const int16_t k_rhalf_size[] = {2, 3};
    flex_case("flexsz-half", ER_FLEX_ROW, 5, 0, 0, 2, k_g1, k_half_pos, k_half_size);
    flex_case("flexsz-rhalf", ER_FLEX_ROW_REVERSE, 5, 0, 0, 2, k_g1, k_rhalf_pos, k_rhalf_size);
}

/**
 * @brief Wrapped 40px-tall children with no width, each stretched to fill its line.
 *
 * With no width of their own the children take their line's cross extent, so their rects report how
 * alignContent: 'stretch' shared the leftover cross space between the lines.
 *
 * @param[in] name    Assertion label.
 * @param[in] w       Container width (the cross axis).
 * @param[in] h       Container height (the main axis).
 * @param[in] pad     Padding on all four edges.
 * @param[in] wrap    ER_WRAP_WRAP or ER_WRAP_WRAP_REVERSE.
 * @param[in] n_kids  Child count (<= 9); must divide evenly into full lines.
 * @param[in] want_x  Expected x per line.
 * @param[in] want_w  Expected width per line.
 */
static void stretch_line_case(const char* name,
                              int16_t w,
                              int16_t h,
                              int16_t pad,
                              uint8_t wrap,
                              int n_kids,
                              const int16_t* want_x,
                              const int16_t* want_w)
{
    ERProps rp = props_default();
    rp.width = w;
    rp.height = h;
    rp.padding = pad;
    rp.flex_direction = ER_FLEX_COL;
    rp.flex_wrap = wrap;
    rp.align_content = ER_ALIGN_CONTENT_STRETCH;
    rp.align_items = ER_ALIGN_STRETCH;
    ERNode* root = mk(rp, NULL);

    ERNode* kids[9];
    ERRect r[9];
    for (int i = 0; i < n_kids; i++)
    {
        ERProps cp = props_default();
        cp.height = 40;
        kids[i] = mk(cp, &r[i]);
        er_tree_append_child(root, kids[i]);
    }
    er_tree_set_root(root);
    er_commit();

    const int per_line = (h - 2 * pad) / 40;
    for (int ln = 0; ln * per_line < n_kids; ln++)
    {
        char lbl[8];
        snprintf(lbl, sizeof(lbl), "l%d", ln);
        pcheck(name, lbl, EXPECT, r[ln * per_line], want_x[ln], pad, want_w[ln], 40);
    }

    for (int i = 0; i < n_kids; i++)
        kill_child(root, kids[i]);
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
    static const int16_t k_x[] = {20, 73, 127};
    static const int16_t k_w[] = {53, 54, 53};
    static const int16_t k_rx[] = {127, 73, 20};
    stretch_line_case("acs-3lines", 200, 200, 20, ER_WRAP_WRAP, 9, k_x, k_w);
    stretch_line_case("acs-r3lines", 200, 200, 20, ER_WRAP_WRAP_REVERSE, 9, k_rx, k_w);

    /* Two lines in 5px sit on exactly 2.5, so the mirror decides which line takes the pixel. */
    static const int16_t k_hx[] = {0, 3};
    static const int16_t k_hw[] = {3, 2};
    static const int16_t k_rhx[] = {3, 0};
    static const int16_t k_rhw[] = {2, 3};
    stretch_line_case("acs-half", 5, 100, 0, ER_WRAP_WRAP, 4, k_hx, k_hw);
    stretch_line_case("acs-rhalf", 5, 100, 0, ER_WRAP_WRAP_REVERSE, 4, k_rhx, k_rhw);
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
    fixture_abs_content_overflows();
    fixture_abs_containing_block();
    fixture_abs_padding_box_edges();
    fixture_abs_inset_pair_padding();
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
 *   - width-aware text wrapping / auto height: Text uses single-line measurement unless
 *     number_of_lines is set, so an auto-height container under-sizes wrapped text. (Needs a
 *     width-aware measure pass; the expected height is font-dependent, so a tolerance-based
 *     assertion would be required rather than the exact-rect compare used here.)
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
