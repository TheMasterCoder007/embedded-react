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
 *   - percentage width/height/padding/margin: only flex_basis has a percent field.
 *   - width-aware text wrapping / auto height: Text uses single-line measurement unless
 *     number_of_lines is set, so an auto-height container under-sizes wrapped text. (Needs a
 *     width-aware measure pass; the expected height is font-dependent, so a tolerance-based
 *     assertion would be required rather than the exact-rect compare used here.)
 *   - alignItems: baseline: no baseline alignment.
 *--------------------------------------------------------------------------------------------------------------------*/
