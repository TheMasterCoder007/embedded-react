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
 * puts it at -20 relative to that box, i.e. 0 on screen. Clamping the free space at 0 (as Pass 5 does
 * for flow children) would stick it at the padding corner instead.
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
 * @brief FLOW child, reversed main axis + asymmetric margins: Pass 5 mirrors the wrong margin.
 *
 * Not about absolutes — found while giving them Yoga-correct static positions, which surfaced the
 * rule: a reversed axis leads with the margin on the edge it starts from. `column-reverse` starts at
 * the bottom, so a 40px child with marginBottom 3 sits 3px off the bottom (y = 20+160-3-40 = 137).
 * Pass 5 instead adds the LEADING margin and then mirrors the result, landing at 133.
 *
 * The absolute path (abs-static rev-col-margin) already does this correctly, so an uninset absolute
 * and an identical flow child currently disagree in a reversed parent — closing this gap makes them
 * agree again. Fixing it means moving Pass 5's reverse mirror to use margin_main_end. Issue #193.
 */
static void fixture_flow_reverse_margin(void)
{
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
    pcheck("flow-reverse-margin", "c", XFAIL, rc, 20, 137, 50, 40);

    kill_child(root, c);
    er_node_destroy(root);
}

/**
 * @brief FLOW children, wrap-reverse: the lines reverse but never travel to the far cross edge.
 *
 * Also not about absolutes — the absolute path mirrors against the full cross extent and matches Yoga
 * (abs-static wrap-rev-*), while Pass 5 mirrors about `total_cross`, the lines' OWN extent. That
 * reverses their order but leaves the block parked at the cross start, so a single 50px line in a
 * 160px content box sits at x=20 instead of x=130. The two agree only when the lines happen to fill
 * the cross axis exactly. Issue #192.
 */
static void fixture_flow_wrap_reverse_anchor(void)
{
    ERProps rp = props_default();
    rp.width = 200;
    rp.height = 200;
    rp.padding = 20;
    rp.flex_direction = ER_FLEX_COL;
    rp.flex_wrap = ER_WRAP_WRAP_REVERSE;
    rp.align_items = ER_ALIGN_FLEX_START;
    ERNode* root = mk(rp, NULL);

    ERRect rc;
    ERProps cp = props_default();
    cp.width = 50;
    cp.height = 40;
    ERNode* c = mk(cp, &rc);

    er_tree_append_child(root, c);
    er_tree_set_root(root);
    er_commit();
    pcheck("flow-wrap-reverse", "c", XFAIL, rc, 130, 20, 50, 40);

    kill_child(root, c);
    er_node_destroy(root);
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
    fixture_flow_wrap_reverse_anchor();

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
 *   - half-pixel rounding: positions are integer arithmetic and truncate, where Yoga computes in
 *     floats and rounds to the pixel grid. Centring a 51px child in a 160px content box puts it at
 *     74 here and 75 in Yoga. Systemic (it applies to flow and absolute children alike), so it is
 *     noted rather than pinned — every centring fixture would diverge if the convention changed.
 *--------------------------------------------------------------------------------------------------------------------*/
